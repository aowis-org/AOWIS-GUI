#include "network/hydraulic_data.h"
#include "projects/hydraulic_draft_entity_registry.h"

#include <aowis/model/serialization/model_codec.h>
#include <aowis/model/edit/hydraulic_root_edit.h>
#include <aowis/model/serialization/model_validation.h>
#include <aowis/model/uuid.h>
#include "network/hydraulic_rule_dependencies.h"
#include "network/network_render_snapshot_builder.h"
#include "network/network_symbology_values.h"

#include <cmath>
#include <utility>

#include <QHash>
#include <QTimer>

namespace
{
// Reference-bearing hydraulic edits must not persist dangling UUIDs.  A null
// UUID represents an unset optional reference; a non-null UUID must identify
// exactly one record in the expected library collection.
template<typename Entity>
bool hydraulicReferenceExists(const QList<Entity> &entities, const QUuid &uuid)
{
    if (uuid.isNull())
        return true;
    int matches = 0;
    for (const Entity &entity : entities)
    {
        if (entity.uuid == uuid)
            ++matches;
    }
    return matches == 1;
}

// Refuse ambiguous UUIDs across all persisted hydraulic collections.
int hydraulicUuidOccurrences(const NetworkHydraulic &network, const QUuid &uuid)
{
    int count = 0;
#define COUNT_ENTITY_UUIDS(collection, model_type, storage_kind, ordinal) \
    for (const model_type &entity : network.collection) \
        if (entity.uuid == uuid) ++count;
    AOWIS_HYDRAULIC_DRAFT_ENTITY_TYPES(COUNT_ENTITY_UUIDS)
#undef COUNT_ENTITY_UUIDS
    return count;
}

template<typename Entity>
Entity *editableLibraryItem(QList<Entity> &entities, const QUuid &uuid)
{
    Entity *found = nullptr;
    for (Entity &entity : entities)
    {
        if (entity.uuid != uuid)
            continue;
        if (found != nullptr)
            return nullptr;
        found = &entity;
    }
    return found;
}

// Rule references must resolve to the correct kind of hydraulic entity.
// A cross-collection duplicate UUID must never be accepted as unambiguous.
bool hydraulicNodeReferenceExists(const NetworkHydraulic &network, const QUuid &uuid)
{
    if (uuid.isNull() || hydraulicUuidOccurrences(network, uuid) != 1)
        return false;
    return hydraulicReferenceExists(network.nodes_junctions, uuid) ||
           hydraulicReferenceExists(network.nodes_reservoirs, uuid) ||
           hydraulicReferenceExists(network.nodes_tanks, uuid);
}

bool hydraulicLinkReferenceExists(const NetworkHydraulic &network, const QUuid &uuid)
{
    if (uuid.isNull() || hydraulicUuidOccurrences(network, uuid) != 1)
        return false;
    return hydraulicReferenceExists(network.links_pipes, uuid) ||
           hydraulicReferenceExists(network.links_pumps, uuid) ||
           hydraulicReferenceExists(network.links_valves, uuid);
}

bool hydraulicRuleReferencesValid(const NetworkHydraulic &network,
                                  const HydraulicControlRule &rule)
{
    if (!std::isfinite(rule.priority) || rule.premises.isEmpty() || rule.actions_then.isEmpty())
        return false;
    for (const HydraulicControlRulePremise &premise : rule.premises)
    {
        if (premise.object == HydraulicControlRuleObject::System)
        {
            if (!premise.object_uuid.isNull()) return false;
        }
        else if (premise.object == HydraulicControlRuleObject::Node)
        {
            if (!hydraulicNodeReferenceExists(network, premise.object_uuid)) return false;
        }
        else if (premise.object == HydraulicControlRuleObject::Link)
        {
            if (!hydraulicLinkReferenceExists(network, premise.object_uuid)) return false;
        }
        else return false;
    }
    for (const HydraulicControlRuleAction &action : rule.actions_then)
        if (!hydraulicLinkReferenceExists(network, action.link_uuid)) return false;
    for (const HydraulicControlRuleAction &action : rule.actions_else)
        if (!hydraulicLinkReferenceExists(network, action.link_uuid)) return false;
    return true;
}

bool hydraulicLabelReferencesValid(const NetworkHydraulic &network,
                                   const HydraulicMapLabel &label)
{
    if (!label.anchor_node_uuid.isNull() &&
        !hydraulicNodeReferenceExists(network, label.anchor_node_uuid))
        return false;
    const CoordinateWGS84 &coordinate = label.coordinate_wgs84;
    return std::isfinite(coordinate.latitude_deg) && coordinate.latitude_deg >= -90.0 &&
           coordinate.latitude_deg <= 90.0 && std::isfinite(coordinate.longitude_deg) &&
           coordinate.longitude_deg >= -180.0 && coordinate.longitude_deg <= 180.0 &&
           (!coordinate.altitude_m.has_value() || std::isfinite(*coordinate.altitude_m));
}

bool pipeMaterialReferenced(const NetworkHydraulic &network, const QUuid &uuid)
{
    for (const HydraulicLinkPipe &pipe : network.links_pipes)
        if (pipe.material_uuid == uuid) return true;
    return false;
}

bool timePatternReferenced(const NetworkHydraulic &network, const QUuid &uuid)
{
    if (network.options_hydraulic.default_demand_pattern_uuid == uuid ||
        network.options_energy.global_energy_price_pattern_uuid == uuid)
        return true;
    for (const HydraulicNodeJunction &node : network.nodes_junctions)
    {
        if (node.quality_source.pattern_uuid == uuid) return true;
        for (const HydraulicDemand &demand : node.demands)
            if (demand.pattern_uuid == uuid) return true;
    }
    for (const HydraulicNodeReservoir &node : network.nodes_reservoirs)
        if (node.head_pattern_uuid == uuid || node.quality_source.pattern_uuid == uuid)
            return true;
    for (const HydraulicNodeTank &node : network.nodes_tanks)
        if (node.quality_source.pattern_uuid == uuid) return true;
    for (const HydraulicDemandPoint &point : network.demand_points)
        for (const HydraulicDemand &demand : point.demands)
            if (demand.pattern_uuid == uuid) return true;
    for (const HydraulicLinkPump &pump : network.links_pumps)
        if (pump.speed_pattern_uuid == uuid || pump.price_pattern_uuid == uuid)
            return true;
    for (const MultiSpeciesNodeSource &source : network.multi_species.sources)
        if (source.pattern_uuid == uuid) return true;
    return false;
}

bool tankVolumeCurveReferenced(const NetworkHydraulic &network, const QUuid &uuid)
{
    for (const HydraulicNodeTank &tank : network.nodes_tanks)
        if (tank.volume_curve_uuid == uuid) return true;
    return false;
}

bool pumpHeadCurveReferenced(const NetworkHydraulic &network, const QUuid &uuid)
{
    for (const HydraulicLinkPump &pump : network.links_pumps)
        if (pump.head_curve_uuid == uuid) return true;
    return false;
}

bool pumpEfficiencyCurveReferenced(const NetworkHydraulic &network, const QUuid &uuid)
{
    for (const HydraulicLinkPump &pump : network.links_pumps)
        if (pump.efficiency_curve_uuid == uuid) return true;
    return false;
}

bool valveHeadlossCurveReferenced(const NetworkHydraulic &network, const QUuid &uuid)
{
    for (const HydraulicLinkValve &valve : network.links_valves)
        if (valve.head_loss_curve_uuid == uuid) return true;
    return false;
}

bool valveCharacteristicCurveReferenced(const NetworkHydraulic &network, const QUuid &uuid)
{
    for (const HydraulicLinkValve &valve : network.links_valves)
        if (valve.characteristic_curve_uuid == uuid) return true;
    return false;
}

bool noLibraryReference(const NetworkHydraulic &, const QUuid &)
{
    return false;
}

bool coordinateWgs84Valid(const CoordinateWGS84 &coordinate)
{
    return std::isfinite(coordinate.latitude_deg) &&
           coordinate.latitude_deg >= -90.0 && coordinate.latitude_deg <= 90.0 &&
           std::isfinite(coordinate.longitude_deg) &&
           coordinate.longitude_deg >= -180.0 && coordinate.longitude_deg <= 180.0;
}

void updateMinimumMaximum(double value, double &minimum, double &maximum, bool &initialized)
{
    if (!std::isfinite(value))
        return;

    if (!initialized)
    {
        minimum = value;
        maximum = value;
        initialized = true;
        return;
    }

    if (value < minimum)
        minimum = value;
    if (value > maximum)
        maximum = value;
}

// Folds visited symbology values (network_symbology_values.h visitors) into
// minimum / maximum without collecting them: no hash table per time step.
struct SymbologyMinimumMaximum
{
    double *minimum = nullptr;
    double *maximum = nullptr;
    bool initialized = false;

    void operator()(const QUuid &, double value)
    {
        updateMinimumMaximum(value, *this->minimum, *this->maximum, this->initialized);
    }
};

void updateWaterAgeNodeMinimumMaximum(const WaterQualitySimulationResult &result,
                                      double &minimum, double &maximum, bool &initialized)
{
    for (const WaterQualitySimulationResultNodeJunction &junction : result.nodes_junctions)
        updateMinimumMaximum(junction.water_age_h, minimum, maximum, initialized);
    for (const WaterQualitySimulationResultNodeReservoir &reservoir : result.nodes_reservoirs)
        updateMinimumMaximum(reservoir.water_age_h, minimum, maximum, initialized);
    for (const WaterQualitySimulationResultNodeTank &tank : result.nodes_tanks)
        updateMinimumMaximum(tank.water_age_h, minimum, maximum, initialized);
}

void updateWaterAgeLinkMinimumMaximum(const WaterQualitySimulationResult &result,
                                      double &minimum, double &maximum, bool &initialized)
{
    for (const WaterQualitySimulationResultLinkPipe &pipe : result.links_pipes)
        updateMinimumMaximum(pipe.water_age_h, minimum, maximum, initialized);
    for (const WaterQualitySimulationResultLinkPump &pump : result.links_pumps)
        updateMinimumMaximum(pump.water_age_h, minimum, maximum, initialized);
    for (const WaterQualitySimulationResultLinkValve &valve : result.links_valves)
        updateMinimumMaximum(valve.water_age_h, minimum, maximum, initialized);
}

}

// A scoped edit groups one property command into one reversible MODEL event.
// It records only the touched UUID and compares canonical images at scope exit;
// a failed operation or no-op produces no transaction.
class HydraulicData::EditScope final
{
public:
    EditScope(HydraulicData &owner, QString operation, const QUuid &entity_uuid)
        : owner_(owner),
          capture_(std::move(operation)),
          enabled_(owner.logical_edit_capture_enabled)
    {
        if (this->enabled_ && !entity_uuid.isNull())
            this->capture_.watch(this->owner_.network_hydraulic, entity_uuid);
    }

    ~EditScope()
    {
        if (this->enabled_ && this->owner_.logical_edit_capture_enabled)
            this->owner_.publishCapturedEdit(this->capture_);
    }

    void watchNodeDeletionDependents(const QUuid &node_uuid)
    {
        if (!this->enabled_ || node_uuid.isNull())
            return;
        QSet<QUuid> deleted_links;
        QSet<QUuid> deleted_pipes;
        for (const HydraulicLinkPipe &pipe : this->owner_.network_hydraulic.links_pipes)
        {
            if (pipe.node_uuid_from != node_uuid && pipe.node_uuid_to != node_uuid)
                continue;
            deleted_pipes.insert(pipe.uuid);
            deleted_links.insert(pipe.uuid);
            this->capture_.watch(this->owner_.network_hydraulic, pipe.uuid);
        }
        for (const HydraulicLinkPump &pump : this->owner_.network_hydraulic.links_pumps)
        {
            if (pump.node_uuid_from == node_uuid || pump.node_uuid_to == node_uuid)
            {
                deleted_links.insert(pump.uuid);
                this->capture_.watch(this->owner_.network_hydraulic, pump.uuid);
            }
        }
        for (const HydraulicLinkValve &valve : this->owner_.network_hydraulic.links_valves)
        {
            if (valve.node_uuid_from == node_uuid || valve.node_uuid_to == node_uuid)
            {
                deleted_links.insert(valve.uuid);
                this->capture_.watch(this->owner_.network_hydraulic, valve.uuid);
            }
        }
        for (const HydraulicControlSimple &control : this->owner_.network_hydraulic.controls_simple)
        {
            if (control.trigger_node_uuid == node_uuid || deleted_links.contains(control.link_uuid))
                this->capture_.watch(this->owner_.network_hydraulic, control.uuid);
        }
        for (const HydraulicControlRule &rule : this->owner_.network_hydraulic.controls_rules)
        {
            bool affected = hydraulic_rule_dependencies::referencesEntity(rule, node_uuid);
            if (!affected)
            {
                for (const QUuid &link_uuid : deleted_links)
                {
                    if (hydraulic_rule_dependencies::referencesEntity(rule, link_uuid))
                    {
                        affected = true;
                        break;
                    }
                }
            }
            if (affected)
                this->capture_.watch(this->owner_.network_hydraulic, rule.uuid);
        }
        for (const HydraulicMapLabel &label : this->owner_.network_hydraulic.map_labels)
        {
            if (label.anchor_node_uuid == node_uuid)
                this->capture_.watch(this->owner_.network_hydraulic, label.uuid);
        }
        for (const HydraulicDemandPoint &point : this->owner_.network_hydraulic.demand_points)
        {
            const bool linked_to_deleted_pipe =
                point.attachment.type == HydraulicDemandPointAttachmentType::Pipe &&
                deleted_pipes.contains(point.attachment.pipe_uuid);
            const bool linked_to_deleted_junction =
                (point.attachment.type == HydraulicDemandPointAttachmentType::Junction &&
                 point.attachment.junction_uuid == node_uuid) ||
                (point.attachment.type == HydraulicDemandPointAttachmentType::Pipe &&
                 point.attachment.pipe_assigned_junction_uuid == node_uuid);
            if (linked_to_deleted_pipe || linked_to_deleted_junction)
                this->capture_.watch(this->owner_.network_hydraulic, point.uuid);
        }
    }

    void watchLinkDeletionDependents(const QUuid &link_uuid)
    {
        if (!this->enabled_ || link_uuid.isNull())
            return;
        for (const HydraulicControlSimple &control : this->owner_.network_hydraulic.controls_simple)
        {
            if (control.link_uuid == link_uuid)
                this->capture_.watch(this->owner_.network_hydraulic, control.uuid);
        }
        for (const HydraulicControlRule &rule : this->owner_.network_hydraulic.controls_rules)
        {
            if (hydraulic_rule_dependencies::referencesEntity(rule, link_uuid))
                this->capture_.watch(this->owner_.network_hydraulic, rule.uuid);
        }
    }

private:
    HydraulicData &owner_;
    HydraulicEditTransactionCapture capture_;
    bool enabled_;
};

HydraulicData::HydraulicData(QObject *parent)
    : QObject{parent},
      network_editor(this->network_hydraulic)
{
    QTimer::singleShot(0, this, [this]()
    {
        rebuildBoundingBoxWgs84();
        markNetworkChanged(NetworkChange::Geometry);
        emit signalNetworkLoaded();
    });
}

const NetworkHydraulic &HydraulicData::networkHydraulic() const
{
    return this->network_hydraulic;
}

void HydraulicData::setLogicalEditCaptureEnabled(bool enabled)
{
    this->logical_edit_capture_enabled = enabled;
}

bool HydraulicData::logicalEditCaptureEnabled() const
{
    return this->logical_edit_capture_enabled;
}

void HydraulicData::publishCapturedEdit(const HydraulicEditTransactionCapture &capture)
{
    QString error;
    const std::optional<QByteArray> transaction = capture.finish(this->network_hydraulic, &error);
    if (!error.isEmpty())
    {
        // An editor must not regard this unsaved mutation as persisted.
        this->logical_edit_capture_enabled = false;
        emit signalLogicalEditCaptureFailed(error);
    }
    else if (transaction.has_value())
    {
        emit signalLogicalEditRecorded(transaction.value());
    }
}

void HydraulicData::replaceNetworkHydraulic(
    NetworkHydraulic network,
    QList<WaterQualitySolverOptions> quality_run_options)
{
    clearSimulationResultTimeline();

    this->logical_edit_capture_enabled = false;
    this->network_hydraulic = std::move(network);
    this->simulation_quality_run_options = std::move(quality_run_options);
    this->source_trace_origin_node_uuid = QUuid();

    for (const WaterQualitySolverOptions &quality_options : this->simulation_quality_run_options)
    {
        if (quality_options.analysis == WaterQualityAnalysisType::SourceTrace)
        {
            this->source_trace_origin_node_uuid = quality_options.trace_node_uuid;
            break;
        }
    }

    rebuildBoundingBoxWgs84();
    markNetworkChanged(NetworkChange::Geometry);
    emit signalWaterQualityOptionsChanged();
    emit signalNetworkLoaded();
}

const QList<WaterQualitySolverOptions> &HydraulicData::simulationQualityRunOptions() const
{
    return this->simulation_quality_run_options;
}

bool HydraulicData::setNetworkRootMetadata(const NetworkHydraulic &metadata)
{
    const NetworkHydraulic old_metadata = aowis::model::hydraulicRootMetadata(this->network_hydraulic);
    const NetworkHydraulic new_metadata = aowis::model::hydraulicRootMetadata(metadata);
    const QByteArray before_image = aowis::model::hydraulicRootImage(old_metadata);
    const QByteArray after_image = aowis::model::hydraulicRootImage(new_metadata);
    if (before_image.isEmpty() || after_image.isEmpty())
        return false;
    if (before_image == after_image)
        return true;

    NetworkHydraulic updated = this->network_hydraulic;
    if (!aowis::model::applyHydraulicRootMetadata(updated, new_metadata))
        return false;
    const aowis::model::HydraulicDependencyValidation checked =
        aowis::model::validateHydraulicNetworkWithDependencies(updated);
    if (!checked.report.valid())
        return false;

    HydraulicEditTransactionCapture capture(QStringLiteral("hydraulic.network.root.update"));
    if (this->logical_edit_capture_enabled &&
        !capture.watch(this->network_hydraulic, this->network_hydraulic.uuid))
    {
        this->logical_edit_capture_enabled = false;
        emit signalLogicalEditCaptureFailed(QStringLiteral("Cannot capture hydraulic network metadata"));
        return false;
    }
    const bool headloss_changed =
        this->network_hydraulic.options_hydraulic.headloss_formula !=
        updated.options_hydraulic.headloss_formula;
    this->network_hydraulic = std::move(updated);
    if (this->logical_edit_capture_enabled)
        publishCapturedEdit(capture);
    markNetworkChanged(NetworkChange::Visual);
    if (headloss_changed)
        emit signalSimulationHeadlossFormulaChanged();
    return true;
}

void HydraulicData::setSimulationHeadlossFormula(HydraulicHeadlossFormula formula)
{
    if (this->network_hydraulic.options_hydraulic.headloss_formula == formula)
        return;
    NetworkHydraulic metadata = aowis::model::hydraulicRootMetadata(this->network_hydraulic);
    metadata.options_hydraulic.headloss_formula = formula;
    if (!setNetworkRootMetadata(metadata))
        emit signalLogicalEditCaptureFailed(QStringLiteral("Invalid hydraulic headloss setting"));
}

QUuid HydraulicData::sourceTraceOriginNodeUuid() const
{
    return this->source_trace_origin_node_uuid;
}

bool HydraulicData::setSourceTraceOriginNodeUuid(const QUuid &uuid)
{
    if (!uuid.isNull() && !nodeEntityType(uuid).has_value())
        return false;

    if (this->source_trace_origin_node_uuid == uuid)
        return true;

    this->source_trace_origin_node_uuid = uuid;
    for (WaterQualitySolverOptions &quality_options : this->simulation_quality_run_options)
    {
        if (quality_options.analysis == WaterQualityAnalysisType::SourceTrace)
            quality_options.trace_node_uuid = uuid;
    }
    clearWaterQualitySimulationResultTimeline();
    emit signalWaterQualityOptionsChanged();
    return true;
}

bool HydraulicData::hasSimulationResults() const
{
    return this->simulation_result_timeline.has_value()
        && !this->simulation_result_timeline_stale
        && this->simulation_result_timeline->validity == HydraulicSimulationResultValidity::Valid
        && !this->simulation_result_timeline->results.isEmpty();
}

const std::optional<HydraulicSimulationResultTimeline> &HydraulicData::simulationResultTimeline() const
{
    return this->simulation_result_timeline;
}

bool HydraulicData::hasWaterQualitySimulationResults() const
{
    if (!this->water_quality_simulation_result_timeline.has_value()
        || this->simulation_result_timeline_stale
        || this->water_quality_simulation_result_timeline->results.isEmpty())
    {
        return false;
    }

    return this->water_quality_simulation_result_timeline->validity
            == WaterQualitySimulationResultValidity::Valid
        || this->water_quality_simulation_result_timeline->validity
            == WaterQualitySimulationResultValidity::Partial;
}

const QList<WaterQualitySimulationResultTimeline> &
HydraulicData::waterQualitySimulationResultTimelines() const
{
    return this->water_quality_simulation_result_timelines;
}

const std::optional<WaterQualitySimulationResultTimeline> &
HydraulicData::waterQualitySimulationResultTimeline() const
{
    return this->water_quality_simulation_result_timeline;
}

const WaterQualitySimulationResult *HydraulicData::currentWaterQualitySimulationResult() const
{
    if (!hasWaterQualitySimulationResults())
        return nullptr;

    const HydraulicSimulationResult *hydraulic_result = currentSimulationResult();
    if (hydraulic_result == nullptr)
        return nullptr;

    const quint64 hydraulic_time_s = hydraulic_result->time_elapsed_s;
    const WaterQualitySimulationResult *matched_result = nullptr;

    for (const WaterQualitySimulationResult &quality_result
         : this->water_quality_simulation_result_timeline->results)
    {
        if (quality_result.time_elapsed_s > hydraulic_time_s)
            break;

        matched_result = &quality_result;
    }

    return matched_result;
}

const WaterQualitySimulationResult *HydraulicData::currentWaterQualitySimulationResult(
    WaterQualityAnalysisType analysis) const
{
    if (this->simulation_result_timeline_stale)
        return nullptr;

    const HydraulicSimulationResult *hydraulic_result = currentSimulationResult();
    if (hydraulic_result == nullptr)
        return nullptr;

    const quint64 hydraulic_time_s = hydraulic_result->time_elapsed_s;
    for (const WaterQualitySimulationResultTimeline &timeline
         : this->water_quality_simulation_result_timelines)
    {
        if (timeline.analysis != analysis || timeline.results.isEmpty())
            continue;
        if (timeline.validity != WaterQualitySimulationResultValidity::Valid
            && timeline.validity != WaterQualitySimulationResultValidity::Partial)
        {
            continue;
        }

        const WaterQualitySimulationResult *matched_result = nullptr;
        for (const WaterQualitySimulationResult &quality_result : timeline.results)
        {
            if (quality_result.time_elapsed_s > hydraulic_time_s)
                break;
            matched_result = &quality_result;
        }
        return matched_result;
    }

    return nullptr;
}

const HydraulicSimulationStatus *HydraulicData::simulationStatus() const
{
    if (!this->simulation_result_timeline.has_value())
        return nullptr;

    return &this->simulation_result_timeline->status;
}

const HydraulicSimulationStatus *HydraulicData::waterQualitySimulationStatus() const
{
    if (!this->water_quality_simulation_result_timeline.has_value())
        return nullptr;

    return &this->water_quality_simulation_result_timeline->status;
}

QUuid HydraulicData::simulationErrorEntityUuid() const
{
    const HydraulicSimulationStatus *status = simulationStatus();
    if (status == nullptr || status->success)
        return QUuid();

    return status->entity.uuid;
}

InfrastructureEntity HydraulicData::simulationErrorEntityType() const
{
    const HydraulicSimulationStatus *status = simulationStatus();
    if (status == nullptr || status->success || status->entity.uuid.isNull())
        return InfrastructureEntity::Unknown;

    switch (status->entity.type)
    {
    case HydraulicSimulationStatusEntityType::Junction:
        return InfrastructureEntity::Junction;
    case HydraulicSimulationStatusEntityType::Reservoir:
        return InfrastructureEntity::Reservoir;
    case HydraulicSimulationStatusEntityType::Tank:
        return InfrastructureEntity::Tank;
    case HydraulicSimulationStatusEntityType::Pipe:
        return InfrastructureEntity::Pipe;
    case HydraulicSimulationStatusEntityType::Pump:
        return InfrastructureEntity::Pump;
    case HydraulicSimulationStatusEntityType::Valve:
        return InfrastructureEntity::Valve;
    case HydraulicSimulationStatusEntityType::Node:
        if (junction(status->entity.uuid).has_value())
            return InfrastructureEntity::Junction;
        if (reservoir(status->entity.uuid).has_value())
            return InfrastructureEntity::Reservoir;
        if (tank(status->entity.uuid).has_value())
            return InfrastructureEntity::Tank;
        break;
    case HydraulicSimulationStatusEntityType::Link:
        if (pipe(status->entity.uuid).has_value())
            return InfrastructureEntity::Pipe;
        if (pump(status->entity.uuid).has_value())
            return InfrastructureEntity::Pump;
        if (valve(status->entity.uuid).has_value())
            return InfrastructureEntity::Valve;
        break;
    default:
        break;
    }

    return InfrastructureEntity::Unknown;
}

QHash<QUuid, InfrastructureEntity> HydraulicData::simulationErrorEntities() const
{
    QHash<QUuid, InfrastructureEntity> entities;
    if (!this->simulation_result_timeline.has_value())
        return entities;

    for (const HydraulicSimulationDiagnostic &diagnostic : this->simulation_result_timeline->diagnostics)
    {
        if (diagnostic.severity != HydraulicSimulationDiagnosticSeverity::Error
            && diagnostic.severity != HydraulicSimulationDiagnosticSeverity::Fatal)
        {
            continue;
        }

        const QUuid uuid = diagnostic.entity.uuid;
        if (uuid.isNull())
            continue;

        InfrastructureEntity entity_type = InfrastructureEntity::Unknown;
        switch (diagnostic.entity.type)
        {
        case HydraulicSimulationStatusEntityType::Junction:
            entity_type = InfrastructureEntity::Junction;
            break;
        case HydraulicSimulationStatusEntityType::Reservoir:
            entity_type = InfrastructureEntity::Reservoir;
            break;
        case HydraulicSimulationStatusEntityType::Tank:
            entity_type = InfrastructureEntity::Tank;
            break;
        case HydraulicSimulationStatusEntityType::Pipe:
            entity_type = InfrastructureEntity::Pipe;
            break;
        case HydraulicSimulationStatusEntityType::Pump:
            entity_type = InfrastructureEntity::Pump;
            break;
        case HydraulicSimulationStatusEntityType::Valve:
            entity_type = InfrastructureEntity::Valve;
            break;
        case HydraulicSimulationStatusEntityType::Node:
            if (junction(uuid).has_value())
                entity_type = InfrastructureEntity::Junction;
            else if (reservoir(uuid).has_value())
                entity_type = InfrastructureEntity::Reservoir;
            else if (tank(uuid).has_value())
                entity_type = InfrastructureEntity::Tank;
            break;
        case HydraulicSimulationStatusEntityType::Link:
            if (pipe(uuid).has_value())
                entity_type = InfrastructureEntity::Pipe;
            else if (pump(uuid).has_value())
                entity_type = InfrastructureEntity::Pump;
            else if (valve(uuid).has_value())
                entity_type = InfrastructureEntity::Valve;
            break;
        default:
            break;
        }

        if (entity_type != InfrastructureEntity::Unknown)
            entities.insert(uuid, entity_type);
    }

    return entities;
}

bool HydraulicData::simulationDiagnosticsStale() const
{
    return this->simulation_result_timeline.has_value() && this->simulation_result_timeline_stale;
}

bool HydraulicData::simulationDiagnosticEntityStale(const QUuid &uuid) const
{
    return !uuid.isNull() && this->simulation_stale_diagnostic_entity_uuids.contains(uuid);
}

const QSet<QUuid> &HydraulicData::simulationStaleDiagnosticEntityUuids() const
{
    return this->simulation_stale_diagnostic_entity_uuids;
}

const HydraulicSimulationResult *HydraulicData::currentSimulationResult() const
{
    if (!hasSimulationResults())
        return nullptr;

    if (this->current_simulation_result_index < 0
        || this->current_simulation_result_index >= this->simulation_result_timeline->results.size())
    {
        return nullptr;
    }

    return &this->simulation_result_timeline->results.at(this->current_simulation_result_index);
}

int HydraulicData::currentSimulationResultIndex() const
{
    return this->current_simulation_result_index;
}

void HydraulicData::setSimulationResultTimeline(const HydraulicSimulationResultTimeline &result_timeline)
{
    this->simulation_result_timeline = result_timeline;
    this->simulation_result_timeline_stale = false;
    this->simulation_stale_diagnostic_entity_uuids.clear();
    this->current_simulation_result_index = hasSimulationResults() ? 0 : -1;

    emit signalSimulationResultTimelineChanged(hasSimulationResults());
    emit signalCurrentSimulationResultChanged(this->current_simulation_result_index);
}

void HydraulicData::setWaterQualitySimulationResultTimelines(
    const QList<WaterQualitySimulationResultTimeline> &result_timelines)
{
    this->water_quality_simulation_result_timelines = result_timelines;

    if (this->water_quality_simulation_result_timelines.isEmpty())
        this->water_quality_simulation_result_timeline.reset();
    else
        this->water_quality_simulation_result_timeline = this->water_quality_simulation_result_timelines.constFirst();

    emit signalWaterQualitySimulationResultTimelineChanged(hasWaterQualitySimulationResults());
}

void HydraulicData::setWaterQualitySimulationResultTimeline(
    const WaterQualitySimulationResultTimeline &result_timeline)
{
    QList<WaterQualitySimulationResultTimeline> result_timelines;
    result_timelines.append(result_timeline);
    setWaterQualitySimulationResultTimelines(result_timelines);
}

void HydraulicData::clearSimulationResultTimeline()
{
    const bool had_timeline = this->simulation_result_timeline.has_value();
    const bool had_quality_timeline = this->water_quality_simulation_result_timeline.has_value();
    const int previous_index = this->current_simulation_result_index;

    this->simulation_result_timeline.reset();
    this->water_quality_simulation_result_timelines.clear();
    this->water_quality_simulation_result_timeline.reset();
    this->current_simulation_result_index = -1;
    this->simulation_result_timeline_stale = false;
    this->simulation_stale_diagnostic_entity_uuids.clear();

    if (had_timeline)
        emit signalSimulationResultTimelineChanged(false);
    if (had_quality_timeline)
        emit signalWaterQualitySimulationResultTimelineChanged(false);

    if (previous_index != -1)
        emit signalCurrentSimulationResultChanged(-1);
}

void HydraulicData::clearWaterQualitySimulationResultTimeline()
{
    const bool had_timeline = this->water_quality_simulation_result_timeline.has_value()
        || !this->water_quality_simulation_result_timelines.isEmpty();
    this->water_quality_simulation_result_timelines.clear();
    this->water_quality_simulation_result_timeline.reset();

    if (had_timeline)
        emit signalWaterQualitySimulationResultTimelineChanged(false);
}

bool HydraulicData::setCurrentSimulationResultIndex(int result_index)
{
    if (!hasSimulationResults())
        return false;

    if (result_index < 0 || result_index >= this->simulation_result_timeline->results.size())
        return false;

    if (this->current_simulation_result_index == result_index)
        return true;

    this->current_simulation_result_index = result_index;
    emit signalCurrentSimulationResultChanged(this->current_simulation_result_index);
    return true;
}

const NetworkRenderSnapshot &HydraulicData::networkRenderSnapshot() const
{
    if (this->network_render_snapshot.geometry_revision != this->geometry_revision)
        rebuildNetworkRenderSnapshot();

    this->network_render_snapshot.visual_revision = this->visual_revision;
    return this->network_render_snapshot;
}

quint64 HydraulicData::geometryRevision() const
{
    return this->geometry_revision;
}

quint64 HydraulicData::visualRevision() const
{
    return this->visual_revision;
}

QDateTime HydraulicData::timeChangedLast() const
{
    return this->time_changed_last;
}

bool HydraulicData::boundingBoxWgs84Valid() const
{
    return this->bounding_box_wgs84_valid;
}

const CoordinateWGS84 &HydraulicData::boundingBoxWgs84Minimum() const
{
    return this->bounding_box_wgs84_minimum;
}

const CoordinateWGS84 &HydraulicData::boundingBoxWgs84Maximum() const
{
    return this->bounding_box_wgs84_maximum;
}

void HydraulicData::rebuildSymbologyMinMaxValues()
{
    this->node_elevation_m_minimum = 0.0;
    this->node_elevation_m_maximum = 0.0;
    this->node_base_demand_m3_per_h_minimum = 0.0;
    this->node_base_demand_m3_per_h_maximum = 0.0;
    this->node_total_demand_m3_per_h_minimum = 0.0;
    this->node_total_demand_m3_per_h_maximum = 0.0;
    this->node_demand_deficit_m3_per_h_minimum = 0.0;
    this->node_demand_deficit_m3_per_h_maximum = 0.0;
    this->node_emitter_flow_m3_per_h_minimum = 0.0;
    this->node_emitter_flow_m3_per_h_maximum = 0.0;
    this->node_leakage_m3_per_h_minimum = 0.0;
    this->node_leakage_m3_per_h_maximum = 0.0;
    this->node_head_m_minimum = 0.0;
    this->node_head_m_maximum = 0.0;
    this->node_pressure_m_minimum = 0.0;
    this->node_pressure_m_maximum = 0.0;
    this->node_chlorine_mg_per_l_minimum = 0.0;
    this->node_chlorine_mg_per_l_maximum = 0.0;
    this->node_river_water_percent_minimum = 0.0;
    this->node_river_water_percent_maximum = 0.0;
    this->node_lake_water_percent_minimum = 0.0;
    this->node_lake_water_percent_maximum = 0.0;
    this->link_diameter_mm_minimum = 0.0;
    this->link_diameter_mm_maximum = 0.0;
    this->link_length_m_minimum = 0.0;
    this->link_length_m_maximum = 0.0;
    this->link_roughness_hw_minimum = 0.0;
    this->link_roughness_hw_maximum = 0.0;
    this->link_roughness_dw_mm_minimum = 0.0;
    this->link_roughness_dw_mm_maximum = 0.0;
    this->link_roughness_cm_minimum = 0.0;
    this->link_roughness_cm_maximum = 0.0;
    this->link_flow_rate_m3_per_h_minimum = 0.0;
    this->link_flow_rate_m3_per_h_maximum = 0.0;
    this->link_velocity_m_per_s_minimum = 0.0;
    this->link_velocity_m_per_s_maximum = 0.0;
    this->link_head_loss_m_minimum = 0.0;
    this->link_head_loss_m_maximum = 0.0;
    this->link_leakage_m3_per_h_minimum = 0.0;
    this->link_leakage_m3_per_h_maximum = 0.0;
    this->link_chlorine_mg_per_l_minimum = 0.0;
    this->link_chlorine_mg_per_l_maximum = 0.0;
    this->link_river_water_percent_minimum = 0.0;
    this->link_river_water_percent_maximum = 0.0;
    this->link_lake_water_percent_minimum = 0.0;
    this->link_lake_water_percent_maximum = 0.0;
    this->heatmap_elevation_m_minimum = 0.0;
    this->heatmap_elevation_m_maximum = 0.0;
    this->heatmap_total_demand_m3_per_h_minimum = 0.0;
    this->heatmap_total_demand_m3_per_h_maximum = 0.0;
    this->heatmap_demand_deficit_m3_per_h_minimum = 0.0;
    this->heatmap_demand_deficit_m3_per_h_maximum = 0.0;
    this->heatmap_leakage_m3_per_h_minimum = 0.0;
    this->heatmap_leakage_m3_per_h_maximum = 0.0;
    this->heatmap_head_m_minimum = 0.0;
    this->heatmap_head_m_maximum = 0.0;
    this->heatmap_pressure_m_minimum = 0.0;
    this->heatmap_pressure_m_maximum = 0.0;
    this->heatmap_chlorine_mg_per_l_minimum = 0.0;
    this->heatmap_chlorine_mg_per_l_maximum = 0.0;
    this->heatmap_river_water_percent_minimum = 0.0;
    this->heatmap_river_water_percent_maximum = 0.0;
    this->heatmap_lake_water_percent_minimum = 0.0;
    this->heatmap_lake_water_percent_maximum = 0.0;

    bool node_elevation_m_initialized = false;
    bool node_base_demand_m3_per_h_initialized = false;
    bool link_diameter_mm_initialized = false;
    bool link_length_m_initialized = false;
    bool link_roughness_hw_initialized = false;
    bool link_roughness_dw_mm_initialized = false;
    bool link_roughness_cm_initialized = false;

    for (const HydraulicNodeJunction &junction : this->network_hydraulic.nodes_junctions)
    {
        updateMinimumMaximum(resolvedSymbologyElevationM(junction),
                             this->node_elevation_m_minimum,
                             this->node_elevation_m_maximum,
                             node_elevation_m_initialized);

        double base_demand_m3_per_h = 0.0;
        for (const HydraulicDemand &demand : junction.demands)
            base_demand_m3_per_h += demand.base_demand_m3_per_h;

        updateMinimumMaximum(base_demand_m3_per_h,
                             this->node_base_demand_m3_per_h_minimum,
                             this->node_base_demand_m3_per_h_maximum,
                             node_base_demand_m3_per_h_initialized);
    }

    for (const HydraulicDemandPoint &demand_point : this->network_hydraulic.demand_points)
    {
        double base_demand_m3_per_h = 0.0;
        for (const HydraulicDemand &demand : demand_point.demands)
            base_demand_m3_per_h += demand.base_demand_m3_per_h;

        updateMinimumMaximum(base_demand_m3_per_h,
                             this->node_base_demand_m3_per_h_minimum,
                             this->node_base_demand_m3_per_h_maximum,
                             node_base_demand_m3_per_h_initialized);
    }

    for (const HydraulicNodeReservoir &reservoir : this->network_hydraulic.nodes_reservoirs)
    {
        updateMinimumMaximum(resolvedSymbologyElevationM(reservoir),
                             this->node_elevation_m_minimum,
                             this->node_elevation_m_maximum,
                             node_elevation_m_initialized);
    }

    for (const HydraulicNodeTank &tank : this->network_hydraulic.nodes_tanks)
    {
        updateMinimumMaximum(resolvedSymbologyElevationM(tank),
                             this->node_elevation_m_minimum,
                             this->node_elevation_m_maximum,
                             node_elevation_m_initialized);
    }

    for (const HydraulicLinkPipe &pipe : this->network_hydraulic.links_pipes)
    {
        updateMinimumMaximum(pipe.diameter_mm,
                             this->link_diameter_mm_minimum,
                             this->link_diameter_mm_maximum,
                             link_diameter_mm_initialized);
        updateMinimumMaximum(pipe.length_measured_m.value_or(pipe.length_calculated_m),
                             this->link_length_m_minimum,
                             this->link_length_m_maximum,
                             link_length_m_initialized);
        updateMinimumMaximum(pipe.roughness_hazen_williams,
                             this->link_roughness_hw_minimum,
                             this->link_roughness_hw_maximum,
                             link_roughness_hw_initialized);
        updateMinimumMaximum(pipe.roughness_darcy_weisbach_mm,
                             this->link_roughness_dw_mm_minimum,
                             this->link_roughness_dw_mm_maximum,
                             link_roughness_dw_mm_initialized);
        updateMinimumMaximum(pipe.roughness_chezy_manning,
                             this->link_roughness_cm_minimum,
                             this->link_roughness_cm_maximum,
                             link_roughness_cm_initialized);
    }

    for (const HydraulicLinkValve &valve : this->network_hydraulic.links_valves)
    {
        updateMinimumMaximum(valve.diameter_mm,
                             this->link_diameter_mm_minimum,
                             this->link_diameter_mm_maximum,
                             link_diameter_mm_initialized);
    }

    this->heatmap_elevation_m_minimum = this->node_elevation_m_minimum;
    this->heatmap_elevation_m_maximum = this->node_elevation_m_maximum;
    this->heatmap_total_demand_m3_per_h_minimum = this->node_total_demand_m3_per_h_minimum;
    this->heatmap_total_demand_m3_per_h_maximum = this->node_total_demand_m3_per_h_maximum;
    this->heatmap_demand_deficit_m3_per_h_minimum = this->node_demand_deficit_m3_per_h_minimum;
    this->heatmap_demand_deficit_m3_per_h_maximum = this->node_demand_deficit_m3_per_h_maximum;
    this->heatmap_leakage_m3_per_h_minimum = this->node_leakage_m3_per_h_minimum;
    this->heatmap_leakage_m3_per_h_maximum = this->node_leakage_m3_per_h_maximum;
    this->heatmap_head_m_minimum = this->node_head_m_minimum;
    this->heatmap_head_m_maximum = this->node_head_m_maximum;
    this->heatmap_pressure_m_minimum = this->node_pressure_m_minimum;
    this->heatmap_pressure_m_maximum = this->node_pressure_m_maximum;
    this->heatmap_chlorine_mg_per_l_minimum = this->node_chlorine_mg_per_l_minimum;
    this->heatmap_chlorine_mg_per_l_maximum = this->node_chlorine_mg_per_l_maximum;
    this->heatmap_river_water_percent_minimum = this->node_river_water_percent_minimum;
    this->heatmap_river_water_percent_maximum = this->node_river_water_percent_maximum;
    this->heatmap_lake_water_percent_minimum = this->node_lake_water_percent_minimum;
    this->heatmap_lake_water_percent_maximum = this->node_lake_water_percent_maximum;
}

NetworkSymbologyRanges HydraulicData::symbologyRanges(
    const NetworkSymbologySettings &settings) const
{
    NetworkSymbologyRanges ranges;

    double water_age_node_minimum = 0.0;
    double water_age_node_maximum = 0.0;
    double water_age_link_minimum = 0.0;
    double water_age_link_maximum = 0.0;
    bool water_age_node_initialized = false;
    bool water_age_link_initialized = false;

    const WaterQualitySimulationResult *quality_result =
        this->currentWaterQualitySimulationResult(WaterQualityAnalysisType::WaterAge);
    if (quality_result != nullptr)
    {
        if (settings.visual_node == VisualNode::WaterAge
            || settings.visual_heatmap == VisualHeatmap::WaterAge)
        {
            updateWaterAgeNodeMinimumMaximum(
                *quality_result, water_age_node_minimum, water_age_node_maximum,
                water_age_node_initialized);
        }
        if (settings.visual_link == VisualLink::WaterAge)
        {
            updateWaterAgeLinkMinimumMaximum(
                *quality_result, water_age_link_minimum, water_age_link_maximum,
                water_age_link_initialized);
        }
    }

    const HydraulicSimulationResult *hydraulic_result = this->currentSimulationResult();

    switch (settings.visual_node)
    {
    case VisualNode::Elevation:
        ranges.node_minimum = this->node_elevation_m_minimum;
        ranges.node_maximum = this->node_elevation_m_maximum;
        break;
    case VisualNode::BaseDemand:
        ranges.node_minimum = this->node_base_demand_m3_per_h_minimum;
        ranges.node_maximum = this->node_base_demand_m3_per_h_maximum;
        break;
    case VisualNode::TotalDemand:
    case VisualNode::DemandDeficit:
    case VisualNode::EmitterFlow:
    case VisualNode::Leakage:
    case VisualNode::Head:
    case VisualNode::Pressure:
        if (hydraulic_result != nullptr)
        {
            visitHydraulicNodeSymbologyValues(*hydraulic_result, settings.visual_node,
                SymbologyMinimumMaximum{&ranges.node_minimum, &ranges.node_maximum});
        }
        break;
    case VisualNode::Chlorine:
        ranges.node_minimum = this->node_chlorine_mg_per_l_minimum;
        ranges.node_maximum = this->node_chlorine_mg_per_l_maximum;
        break;
    case VisualNode::RiverWater:
        ranges.node_minimum = this->node_river_water_percent_minimum;
        ranges.node_maximum = this->node_river_water_percent_maximum;
        break;
    case VisualNode::LakeWater:
        ranges.node_minimum = this->node_lake_water_percent_minimum;
        ranges.node_maximum = this->node_lake_water_percent_maximum;
        break;
    case VisualNode::WaterAge:
        if (water_age_node_initialized)
        {
            ranges.node_minimum = water_age_node_minimum;
            ranges.node_maximum = water_age_node_maximum;
        }
        break;
    case VisualNode::None:
        break;
    }

    switch (settings.visual_link)
    {
    case VisualLink::Diameter:
        ranges.link_minimum = this->link_diameter_mm_minimum;
        ranges.link_maximum = this->link_diameter_mm_maximum;
        break;
    case VisualLink::Length:
        ranges.link_minimum = this->link_length_m_minimum;
        ranges.link_maximum = this->link_length_m_maximum;
        break;
    case VisualLink::Roughness:
        switch (this->network_hydraulic.options_hydraulic.headloss_formula)
        {
        case HydraulicHeadlossFormula::HazenWilliams:
            ranges.link_minimum = this->link_roughness_hw_minimum;
            ranges.link_maximum = this->link_roughness_hw_maximum;
            break;
        case HydraulicHeadlossFormula::DarcyWeisbach:
            ranges.link_minimum = this->link_roughness_dw_mm_minimum;
            ranges.link_maximum = this->link_roughness_dw_mm_maximum;
            break;
        case HydraulicHeadlossFormula::ChezyManning:
            ranges.link_minimum = this->link_roughness_cm_minimum;
            ranges.link_maximum = this->link_roughness_cm_maximum;
            break;
        }
        break;
    case VisualLink::FlowRate:
    case VisualLink::Velocity:
    case VisualLink::HeadLoss:
    case VisualLink::Leakage:
        if (hydraulic_result != nullptr)
        {
            visitHydraulicLinkSymbologyValues(*hydraulic_result, settings.visual_link,
                SymbologyMinimumMaximum{&ranges.link_minimum, &ranges.link_maximum});
        }
        break;
    case VisualLink::Chlorine:
        ranges.link_minimum = this->link_chlorine_mg_per_l_minimum;
        ranges.link_maximum = this->link_chlorine_mg_per_l_maximum;
        break;
    case VisualLink::RiverWater:
        ranges.link_minimum = this->link_river_water_percent_minimum;
        ranges.link_maximum = this->link_river_water_percent_maximum;
        break;
    case VisualLink::LakeWater:
        ranges.link_minimum = this->link_lake_water_percent_minimum;
        ranges.link_maximum = this->link_lake_water_percent_maximum;
        break;
    case VisualLink::WaterAge:
        if (water_age_link_initialized)
        {
            ranges.link_minimum = water_age_link_minimum;
            ranges.link_maximum = water_age_link_maximum;
        }
        break;
    case VisualLink::None:
        break;
    }

    switch (settings.visual_heatmap)
    {
    case VisualHeatmap::Elevation:
        ranges.heatmap_minimum = this->heatmap_elevation_m_minimum;
        ranges.heatmap_maximum = this->heatmap_elevation_m_maximum;
        break;
    case VisualHeatmap::BaseDemand:
        ranges.heatmap_minimum = this->node_base_demand_m3_per_h_minimum;
        ranges.heatmap_maximum = this->node_base_demand_m3_per_h_maximum;
        break;
    case VisualHeatmap::TotalDemand:
    case VisualHeatmap::DemandDeficit:
    case VisualHeatmap::EmitterFlow:
    case VisualHeatmap::Leakage:
    case VisualHeatmap::Head:
    case VisualHeatmap::Pressure:
        if (hydraulic_result != nullptr)
        {
            visitHydraulicNodeSymbologyValues(*hydraulic_result,
                networkHeatmapNodeVisual(settings.visual_heatmap),
                SymbologyMinimumMaximum{&ranges.heatmap_minimum, &ranges.heatmap_maximum});
        }
        break;
    case VisualHeatmap::Chlorine:
        ranges.heatmap_minimum = this->heatmap_chlorine_mg_per_l_minimum;
        ranges.heatmap_maximum = this->heatmap_chlorine_mg_per_l_maximum;
        break;
    case VisualHeatmap::RiverWater:
        ranges.heatmap_minimum = this->heatmap_river_water_percent_minimum;
        ranges.heatmap_maximum = this->heatmap_river_water_percent_maximum;
        break;
    case VisualHeatmap::LakeWater:
        ranges.heatmap_minimum = this->heatmap_lake_water_percent_minimum;
        ranges.heatmap_maximum = this->heatmap_lake_water_percent_maximum;
        break;
    case VisualHeatmap::WaterAge:
        if (water_age_node_initialized)
        {
            ranges.heatmap_minimum = water_age_node_minimum;
            ranges.heatmap_maximum = water_age_node_maximum;
        }
        break;
    case VisualHeatmap::None:
        break;
    }

    return ranges;
}

double HydraulicData::nodeElevationMMinimum() const
{
    return this->node_elevation_m_minimum;
}

void HydraulicData::setNodeElevationMMinimum(double node_elevation_m_minimum)
{
    this->node_elevation_m_minimum = node_elevation_m_minimum;
}

double HydraulicData::nodeElevationMMaximum() const
{
    return this->node_elevation_m_maximum;
}

void HydraulicData::setNodeElevationMMaximum(double node_elevation_m_maximum)
{
    this->node_elevation_m_maximum = node_elevation_m_maximum;
}

double HydraulicData::nodeBaseDemandM3PerHMinimum() const
{
    return this->node_base_demand_m3_per_h_minimum;
}

void HydraulicData::setNodeBaseDemandM3PerHMinimum(double node_base_demand_m3_per_h_minimum)
{
    this->node_base_demand_m3_per_h_minimum = node_base_demand_m3_per_h_minimum;
}

double HydraulicData::nodeBaseDemandM3PerHMaximum() const
{
    return this->node_base_demand_m3_per_h_maximum;
}

void HydraulicData::setNodeBaseDemandM3PerHMaximum(double node_base_demand_m3_per_h_maximum)
{
    this->node_base_demand_m3_per_h_maximum = node_base_demand_m3_per_h_maximum;
}

double HydraulicData::nodeTotalDemandM3PerHMinimum() const
{
    return this->node_total_demand_m3_per_h_minimum;
}

void HydraulicData::setNodeTotalDemandM3PerHMinimum(double node_total_demand_m3_per_h_minimum)
{
    this->node_total_demand_m3_per_h_minimum = node_total_demand_m3_per_h_minimum;
}

double HydraulicData::nodeTotalDemandM3PerHMaximum() const
{
    return this->node_total_demand_m3_per_h_maximum;
}

void HydraulicData::setNodeTotalDemandM3PerHMaximum(double node_total_demand_m3_per_h_maximum)
{
    this->node_total_demand_m3_per_h_maximum = node_total_demand_m3_per_h_maximum;
}

double HydraulicData::nodeDemandDeficitM3PerHMinimum() const
{
    return this->node_demand_deficit_m3_per_h_minimum;
}

void HydraulicData::setNodeDemandDeficitM3PerHMinimum(double node_demand_deficit_m3_per_h_minimum)
{
    this->node_demand_deficit_m3_per_h_minimum = node_demand_deficit_m3_per_h_minimum;
}

double HydraulicData::nodeDemandDeficitM3PerHMaximum() const
{
    return this->node_demand_deficit_m3_per_h_maximum;
}

void HydraulicData::setNodeDemandDeficitM3PerHMaximum(double node_demand_deficit_m3_per_h_maximum)
{
    this->node_demand_deficit_m3_per_h_maximum = node_demand_deficit_m3_per_h_maximum;
}

double HydraulicData::nodeEmitterFlowM3PerHMinimum() const
{
    return this->node_emitter_flow_m3_per_h_minimum;
}

void HydraulicData::setNodeEmitterFlowM3PerHMinimum(double node_emitter_flow_m3_per_h_minimum)
{
    this->node_emitter_flow_m3_per_h_minimum = node_emitter_flow_m3_per_h_minimum;
}

double HydraulicData::nodeEmitterFlowM3PerHMaximum() const
{
    return this->node_emitter_flow_m3_per_h_maximum;
}

void HydraulicData::setNodeEmitterFlowM3PerHMaximum(double node_emitter_flow_m3_per_h_maximum)
{
    this->node_emitter_flow_m3_per_h_maximum = node_emitter_flow_m3_per_h_maximum;
}

double HydraulicData::nodeLeakageM3PerHMinimum() const
{
    return this->node_leakage_m3_per_h_minimum;
}

void HydraulicData::setNodeLeakageM3PerHMinimum(double node_leakage_m3_per_h_minimum)
{
    this->node_leakage_m3_per_h_minimum = node_leakage_m3_per_h_minimum;
}

double HydraulicData::nodeLeakageM3PerHMaximum() const
{
    return this->node_leakage_m3_per_h_maximum;
}

void HydraulicData::setNodeLeakageM3PerHMaximum(double node_leakage_m3_per_h_maximum)
{
    this->node_leakage_m3_per_h_maximum = node_leakage_m3_per_h_maximum;
}

double HydraulicData::nodeHeadMMinimum() const
{
    return this->node_head_m_minimum;
}

void HydraulicData::setNodeHeadMMinimum(double node_head_m_minimum)
{
    this->node_head_m_minimum = node_head_m_minimum;
}

double HydraulicData::nodeHeadMMaximum() const
{
    return this->node_head_m_maximum;
}

void HydraulicData::setNodeHeadMMaximum(double node_head_m_maximum)
{
    this->node_head_m_maximum = node_head_m_maximum;
}

double HydraulicData::nodePressureMMinimum() const
{
    return this->node_pressure_m_minimum;
}

void HydraulicData::setNodePressureMMinimum(double node_pressure_m_minimum)
{
    this->node_pressure_m_minimum = node_pressure_m_minimum;
}

double HydraulicData::nodePressureMMaximum() const
{
    return this->node_pressure_m_maximum;
}

void HydraulicData::setNodePressureMMaximum(double node_pressure_m_maximum)
{
    this->node_pressure_m_maximum = node_pressure_m_maximum;
}

double HydraulicData::nodeChlorineMgPerLMinimum() const
{
    return this->node_chlorine_mg_per_l_minimum;
}

void HydraulicData::setNodeChlorineMgPerLMinimum(double node_chlorine_mg_per_l_minimum)
{
    this->node_chlorine_mg_per_l_minimum = node_chlorine_mg_per_l_minimum;
}

double HydraulicData::nodeChlorineMgPerLMaximum() const
{
    return this->node_chlorine_mg_per_l_maximum;
}

void HydraulicData::setNodeChlorineMgPerLMaximum(double node_chlorine_mg_per_l_maximum)
{
    this->node_chlorine_mg_per_l_maximum = node_chlorine_mg_per_l_maximum;
}

double HydraulicData::nodeRiverWaterPercentMinimum() const
{
    return this->node_river_water_percent_minimum;
}

void HydraulicData::setNodeRiverWaterPercentMinimum(double node_river_water_percent_minimum)
{
    this->node_river_water_percent_minimum = node_river_water_percent_minimum;
}

double HydraulicData::nodeRiverWaterPercentMaximum() const
{
    return this->node_river_water_percent_maximum;
}

void HydraulicData::setNodeRiverWaterPercentMaximum(double node_river_water_percent_maximum)
{
    this->node_river_water_percent_maximum = node_river_water_percent_maximum;
}

double HydraulicData::nodeLakeWaterPercentMinimum() const
{
    return this->node_lake_water_percent_minimum;
}

void HydraulicData::setNodeLakeWaterPercentMinimum(double node_lake_water_percent_minimum)
{
    this->node_lake_water_percent_minimum = node_lake_water_percent_minimum;
}

double HydraulicData::nodeLakeWaterPercentMaximum() const
{
    return this->node_lake_water_percent_maximum;
}

void HydraulicData::setNodeLakeWaterPercentMaximum(double node_lake_water_percent_maximum)
{
    this->node_lake_water_percent_maximum = node_lake_water_percent_maximum;
}

double HydraulicData::linkDiameterMmMinimum() const
{
    return this->link_diameter_mm_minimum;
}

void HydraulicData::setLinkDiameterMmMinimum(double link_diameter_mm_minimum)
{
    this->link_diameter_mm_minimum = link_diameter_mm_minimum;
}

double HydraulicData::linkDiameterMmMaximum() const
{
    return this->link_diameter_mm_maximum;
}

void HydraulicData::setLinkDiameterMmMaximum(double link_diameter_mm_maximum)
{
    this->link_diameter_mm_maximum = link_diameter_mm_maximum;
}

double HydraulicData::linkLengthMMinimum() const
{
    return this->link_length_m_minimum;
}

void HydraulicData::setLinkLengthMMinimum(double link_length_m_minimum)
{
    this->link_length_m_minimum = link_length_m_minimum;
}

double HydraulicData::linkLengthMMaximum() const
{
    return this->link_length_m_maximum;
}

void HydraulicData::setLinkLengthMMaximum(double link_length_m_maximum)
{
    this->link_length_m_maximum = link_length_m_maximum;
}

double HydraulicData::linkRoughnessHwMinimum() const
{
    return this->link_roughness_hw_minimum;
}

void HydraulicData::setLinkRoughnessHwMinimum(double link_roughness_hw_minimum)
{
    this->link_roughness_hw_minimum = link_roughness_hw_minimum;
}

double HydraulicData::linkRoughnessHwMaximum() const
{
    return this->link_roughness_hw_maximum;
}

void HydraulicData::setLinkRoughnessHwMaximum(double link_roughness_hw_maximum)
{
    this->link_roughness_hw_maximum = link_roughness_hw_maximum;
}

double HydraulicData::linkRoughnessDwMmMinimum() const
{
    return this->link_roughness_dw_mm_minimum;
}

void HydraulicData::setLinkRoughnessDwMmMinimum(double link_roughness_dw_mm_minimum)
{
    this->link_roughness_dw_mm_minimum = link_roughness_dw_mm_minimum;
}

double HydraulicData::linkRoughnessDwMmMaximum() const
{
    return this->link_roughness_dw_mm_maximum;
}

void HydraulicData::setLinkRoughnessDwMmMaximum(double link_roughness_dw_mm_maximum)
{
    this->link_roughness_dw_mm_maximum = link_roughness_dw_mm_maximum;
}

double HydraulicData::linkRoughnessCmMinimum() const
{
    return this->link_roughness_cm_minimum;
}

void HydraulicData::setLinkRoughnessCmMinimum(double link_roughness_cm_minimum)
{
    this->link_roughness_cm_minimum = link_roughness_cm_minimum;
}

double HydraulicData::linkRoughnessCmMaximum() const
{
    return this->link_roughness_cm_maximum;
}

void HydraulicData::setLinkRoughnessCmMaximum(double link_roughness_cm_maximum)
{
    this->link_roughness_cm_maximum = link_roughness_cm_maximum;
}

double HydraulicData::linkFlowRateM3PerHMinimum() const
{
    return this->link_flow_rate_m3_per_h_minimum;
}

void HydraulicData::setLinkFlowRateM3PerHMinimum(double link_flow_rate_m3_per_h_minimum)
{
    this->link_flow_rate_m3_per_h_minimum = link_flow_rate_m3_per_h_minimum;
}

double HydraulicData::linkFlowRateM3PerHMaximum() const
{
    return this->link_flow_rate_m3_per_h_maximum;
}

void HydraulicData::setLinkFlowRateM3PerHMaximum(double link_flow_rate_m3_per_h_maximum)
{
    this->link_flow_rate_m3_per_h_maximum = link_flow_rate_m3_per_h_maximum;
}

double HydraulicData::linkVelocityMPerSMinimum() const
{
    return this->link_velocity_m_per_s_minimum;
}

void HydraulicData::setLinkVelocityMPerSMinimum(double link_velocity_m_per_s_minimum)
{
    this->link_velocity_m_per_s_minimum = link_velocity_m_per_s_minimum;
}

double HydraulicData::linkVelocityMPerSMaximum() const
{
    return this->link_velocity_m_per_s_maximum;
}

void HydraulicData::setLinkVelocityMPerSMaximum(double link_velocity_m_per_s_maximum)
{
    this->link_velocity_m_per_s_maximum = link_velocity_m_per_s_maximum;
}

double HydraulicData::linkHeadLossMMinimum() const
{
    return this->link_head_loss_m_minimum;
}

void HydraulicData::setLinkHeadLossMMinimum(double link_head_loss_m_minimum)
{
    this->link_head_loss_m_minimum = link_head_loss_m_minimum;
}

double HydraulicData::linkHeadLossMMaximum() const
{
    return this->link_head_loss_m_maximum;
}

void HydraulicData::setLinkHeadLossMMaximum(double link_head_loss_m_maximum)
{
    this->link_head_loss_m_maximum = link_head_loss_m_maximum;
}

double HydraulicData::linkLeakageM3PerHMinimum() const
{
    return this->link_leakage_m3_per_h_minimum;
}

void HydraulicData::setLinkLeakageM3PerHMinimum(double link_leakage_m3_per_h_minimum)
{
    this->link_leakage_m3_per_h_minimum = link_leakage_m3_per_h_minimum;
}

double HydraulicData::linkLeakageM3PerHMaximum() const
{
    return this->link_leakage_m3_per_h_maximum;
}

void HydraulicData::setLinkLeakageM3PerHMaximum(double link_leakage_m3_per_h_maximum)
{
    this->link_leakage_m3_per_h_maximum = link_leakage_m3_per_h_maximum;
}

double HydraulicData::linkChlorineMgPerLMinimum() const
{
    return this->link_chlorine_mg_per_l_minimum;
}

void HydraulicData::setLinkChlorineMgPerLMinimum(double link_chlorine_mg_per_l_minimum)
{
    this->link_chlorine_mg_per_l_minimum = link_chlorine_mg_per_l_minimum;
}

double HydraulicData::linkChlorineMgPerLMaximum() const
{
    return this->link_chlorine_mg_per_l_maximum;
}

void HydraulicData::setLinkChlorineMgPerLMaximum(double link_chlorine_mg_per_l_maximum)
{
    this->link_chlorine_mg_per_l_maximum = link_chlorine_mg_per_l_maximum;
}

double HydraulicData::linkRiverWaterPercentMinimum() const
{
    return this->link_river_water_percent_minimum;
}

void HydraulicData::setLinkRiverWaterPercentMinimum(double link_river_water_percent_minimum)
{
    this->link_river_water_percent_minimum = link_river_water_percent_minimum;
}

double HydraulicData::linkRiverWaterPercentMaximum() const
{
    return this->link_river_water_percent_maximum;
}

void HydraulicData::setLinkRiverWaterPercentMaximum(double link_river_water_percent_maximum)
{
    this->link_river_water_percent_maximum = link_river_water_percent_maximum;
}

double HydraulicData::linkLakeWaterPercentMinimum() const
{
    return this->link_lake_water_percent_minimum;
}

void HydraulicData::setLinkLakeWaterPercentMinimum(double link_lake_water_percent_minimum)
{
    this->link_lake_water_percent_minimum = link_lake_water_percent_minimum;
}

double HydraulicData::linkLakeWaterPercentMaximum() const
{
    return this->link_lake_water_percent_maximum;
}

void HydraulicData::setLinkLakeWaterPercentMaximum(double link_lake_water_percent_maximum)
{
    this->link_lake_water_percent_maximum = link_lake_water_percent_maximum;
}

double HydraulicData::heatmapElevationMMinimum() const
{
    return this->heatmap_elevation_m_minimum;
}

void HydraulicData::setHeatmapElevationMMinimum(double heatmap_elevation_m_minimum)
{
    this->heatmap_elevation_m_minimum = heatmap_elevation_m_minimum;
}

double HydraulicData::heatmapElevationMMaximum() const
{
    return this->heatmap_elevation_m_maximum;
}

void HydraulicData::setHeatmapElevationMMaximum(double heatmap_elevation_m_maximum)
{
    this->heatmap_elevation_m_maximum = heatmap_elevation_m_maximum;
}

double HydraulicData::heatmapTotalDemandM3PerHMinimum() const
{
    return this->heatmap_total_demand_m3_per_h_minimum;
}

void HydraulicData::setHeatmapTotalDemandM3PerHMinimum(double heatmap_total_demand_m3_per_h_minimum)
{
    this->heatmap_total_demand_m3_per_h_minimum = heatmap_total_demand_m3_per_h_minimum;
}

double HydraulicData::heatmapTotalDemandM3PerHMaximum() const
{
    return this->heatmap_total_demand_m3_per_h_maximum;
}

void HydraulicData::setHeatmapTotalDemandM3PerHMaximum(double heatmap_total_demand_m3_per_h_maximum)
{
    this->heatmap_total_demand_m3_per_h_maximum = heatmap_total_demand_m3_per_h_maximum;
}

double HydraulicData::heatmapDemandDeficitM3PerHMinimum() const
{
    return this->heatmap_demand_deficit_m3_per_h_minimum;
}

void HydraulicData::setHeatmapDemandDeficitM3PerHMinimum(double heatmap_demand_deficit_m3_per_h_minimum)
{
    this->heatmap_demand_deficit_m3_per_h_minimum = heatmap_demand_deficit_m3_per_h_minimum;
}

double HydraulicData::heatmapDemandDeficitM3PerHMaximum() const
{
    return this->heatmap_demand_deficit_m3_per_h_maximum;
}

void HydraulicData::setHeatmapDemandDeficitM3PerHMaximum(double heatmap_demand_deficit_m3_per_h_maximum)
{
    this->heatmap_demand_deficit_m3_per_h_maximum = heatmap_demand_deficit_m3_per_h_maximum;
}

double HydraulicData::heatmapLeakageM3PerHMinimum() const
{
    return this->heatmap_leakage_m3_per_h_minimum;
}

void HydraulicData::setHeatmapLeakageM3PerHMinimum(double heatmap_leakage_m3_per_h_minimum)
{
    this->heatmap_leakage_m3_per_h_minimum = heatmap_leakage_m3_per_h_minimum;
}

double HydraulicData::heatmapLeakageM3PerHMaximum() const
{
    return this->heatmap_leakage_m3_per_h_maximum;
}

void HydraulicData::setHeatmapLeakageM3PerHMaximum(double heatmap_leakage_m3_per_h_maximum)
{
    this->heatmap_leakage_m3_per_h_maximum = heatmap_leakage_m3_per_h_maximum;
}

double HydraulicData::heatmapHeadMMinimum() const
{
    return this->heatmap_head_m_minimum;
}

void HydraulicData::setHeatmapHeadMMinimum(double heatmap_head_m_minimum)
{
    this->heatmap_head_m_minimum = heatmap_head_m_minimum;
}

double HydraulicData::heatmapHeadMMaximum() const
{
    return this->heatmap_head_m_maximum;
}

void HydraulicData::setHeatmapHeadMMaximum(double heatmap_head_m_maximum)
{
    this->heatmap_head_m_maximum = heatmap_head_m_maximum;
}

double HydraulicData::heatmapPressureMMinimum() const
{
    return this->heatmap_pressure_m_minimum;
}

void HydraulicData::setHeatmapPressureMMinimum(double heatmap_pressure_m_minimum)
{
    this->heatmap_pressure_m_minimum = heatmap_pressure_m_minimum;
}

double HydraulicData::heatmapPressureMMaximum() const
{
    return this->heatmap_pressure_m_maximum;
}

void HydraulicData::setHeatmapPressureMMaximum(double heatmap_pressure_m_maximum)
{
    this->heatmap_pressure_m_maximum = heatmap_pressure_m_maximum;
}

double HydraulicData::heatmapChlorineMgPerLMinimum() const
{
    return this->heatmap_chlorine_mg_per_l_minimum;
}

void HydraulicData::setHeatmapChlorineMgPerLMinimum(double heatmap_chlorine_mg_per_l_minimum)
{
    this->heatmap_chlorine_mg_per_l_minimum = heatmap_chlorine_mg_per_l_minimum;
}

double HydraulicData::heatmapChlorineMgPerLMaximum() const
{
    return this->heatmap_chlorine_mg_per_l_maximum;
}

void HydraulicData::setHeatmapChlorineMgPerLMaximum(double heatmap_chlorine_mg_per_l_maximum)
{
    this->heatmap_chlorine_mg_per_l_maximum = heatmap_chlorine_mg_per_l_maximum;
}

double HydraulicData::heatmapRiverWaterPercentMinimum() const
{
    return this->heatmap_river_water_percent_minimum;
}

void HydraulicData::setHeatmapRiverWaterPercentMinimum(double heatmap_river_water_percent_minimum)
{
    this->heatmap_river_water_percent_minimum = heatmap_river_water_percent_minimum;
}

double HydraulicData::heatmapRiverWaterPercentMaximum() const
{
    return this->heatmap_river_water_percent_maximum;
}

void HydraulicData::setHeatmapRiverWaterPercentMaximum(double heatmap_river_water_percent_maximum)
{
    this->heatmap_river_water_percent_maximum = heatmap_river_water_percent_maximum;
}

double HydraulicData::heatmapLakeWaterPercentMinimum() const
{
    return this->heatmap_lake_water_percent_minimum;
}

void HydraulicData::setHeatmapLakeWaterPercentMinimum(double heatmap_lake_water_percent_minimum)
{
    this->heatmap_lake_water_percent_minimum = heatmap_lake_water_percent_minimum;
}

double HydraulicData::heatmapLakeWaterPercentMaximum() const
{
    return this->heatmap_lake_water_percent_maximum;
}

void HydraulicData::setHeatmapLakeWaterPercentMaximum(double heatmap_lake_water_percent_maximum)
{
    this->heatmap_lake_water_percent_maximum = heatmap_lake_water_percent_maximum;
}

std::optional<HydraulicNodeJunction> HydraulicData::junction(const QUuid &uuid) const
{
    return this->network_editor.junction(uuid);
}

std::optional<HydraulicNodeReservoir> HydraulicData::reservoir(const QUuid &uuid) const
{
    return this->network_editor.reservoir(uuid);
}

std::optional<HydraulicNodeTank> HydraulicData::tank(const QUuid &uuid) const
{
    return this->network_editor.tank(uuid);
}

std::optional<HydraulicDemandPoint> HydraulicData::demandPoint(const QUuid &uuid) const
{
    return this->network_editor.demandPoint(uuid);
}

std::optional<HydraulicLinkPipe> HydraulicData::pipe(const QUuid &uuid) const
{
    return this->network_editor.pipe(uuid);
}

std::optional<HydraulicLinkPump> HydraulicData::pump(const QUuid &uuid) const
{
    return this->network_editor.pump(uuid);
}

std::optional<HydraulicLinkValve> HydraulicData::valve(const QUuid &uuid) const
{
    return this->network_editor.valve(uuid);
}

std::optional<HydraulicNodeCommonData> HydraulicData::nodeCommonData(
    InfrastructureEntity entity_type, const QUuid &uuid) const
{
    switch (entity_type)
    {
    case InfrastructureEntity::Junction:
    {
        const std::optional<HydraulicNodeJunction> node = junction(uuid);
        if (!node.has_value())
            return std::nullopt;

        return HydraulicNodeCommonData{node->id, node->uuid, node->coordinate_wgs84, node->metadata};
    }
    case InfrastructureEntity::Reservoir:
    {
        const std::optional<HydraulicNodeReservoir> node = reservoir(uuid);
        if (!node.has_value())
            return std::nullopt;

        return HydraulicNodeCommonData{node->id, node->uuid, node->coordinate_wgs84, node->metadata};
    }
    case InfrastructureEntity::Tank:
    {
        const std::optional<HydraulicNodeTank> node = tank(uuid);
        if (!node.has_value())
            return std::nullopt;

        return HydraulicNodeCommonData{node->id, node->uuid, node->coordinate_wgs84, node->metadata};
    }
    default:
        return std::nullopt;
    }
}

std::optional<HydraulicLinkCommonData> HydraulicData::linkCommonData(
    InfrastructureEntity entity_type, const QUuid &uuid) const
{
    switch (entity_type)
    {
    case InfrastructureEntity::Pipe:
    {
        const std::optional<HydraulicLinkPipe> link = pipe(uuid);
        if (!link.has_value())
            return std::nullopt;

        return HydraulicLinkCommonData{link->id, link->uuid, link->metadata};
    }
    case InfrastructureEntity::Pump:
    {
        const std::optional<HydraulicLinkPump> link = pump(uuid);
        if (!link.has_value())
            return std::nullopt;

        return HydraulicLinkCommonData{link->id, link->uuid, link->metadata};
    }
    case InfrastructureEntity::Valve:
    {
        const std::optional<HydraulicLinkValve> link = valve(uuid);
        if (!link.has_value())
            return std::nullopt;

        return HydraulicLinkCommonData{link->id, link->uuid, link->metadata};
    }
    default:
        return std::nullopt;
    }
}

std::optional<InfrastructureEntity> HydraulicData::nodeEntityType(const QUuid &uuid) const
{
    if (uuid.isNull())
        return std::nullopt;

    for (const HydraulicNodeJunction &junction : this->network_hydraulic.nodes_junctions)
    {
        if (junction.uuid == uuid)
            return InfrastructureEntity::Junction;
    }

    for (const HydraulicNodeReservoir &reservoir : this->network_hydraulic.nodes_reservoirs)
    {
        if (reservoir.uuid == uuid)
            return InfrastructureEntity::Reservoir;
    }

    for (const HydraulicNodeTank &tank : this->network_hydraulic.nodes_tanks)
    {
        if (tank.uuid == uuid)
            return InfrastructureEntity::Tank;
    }

    return std::nullopt;
}

std::optional<InfrastructureEntity> HydraulicData::linkEntityType(const QUuid &uuid) const
{
    if (uuid.isNull())
        return std::nullopt;

    for (const HydraulicLinkPipe &pipe : this->network_hydraulic.links_pipes)
    {
        if (pipe.uuid == uuid)
            return InfrastructureEntity::Pipe;
    }

    for (const HydraulicLinkPump &pump : this->network_hydraulic.links_pumps)
    {
        if (pump.uuid == uuid)
            return InfrastructureEntity::Pump;
    }

    for (const HydraulicLinkValve &valve : this->network_hydraulic.links_valves)
    {
        if (valve.uuid == uuid)
            return InfrastructureEntity::Valve;
    }

    return std::nullopt;
}

void HydraulicData::markSimulationDiagnosticEntityStale(const QUuid &uuid)
{
    if (!this->simulation_result_timeline.has_value() || uuid.isNull()
        || this->simulation_stale_diagnostic_entity_uuids.contains(uuid))
    {
        return;
    }

    this->simulation_stale_diagnostic_entity_uuids.insert(uuid);
    emit signalSimulationResultTimelineChanged(false);
}

void HydraulicData::markSimulationResultTimelineStale(const QUuid &edited_entity_uuid)
{
    if (!this->simulation_result_timeline.has_value())
        return;

    const int previous_index = this->current_simulation_result_index;
    const bool timeline_became_stale = !this->simulation_result_timeline_stale;
    this->simulation_result_timeline_stale = true;
    this->current_simulation_result_index = -1;

    const bool entity_became_stale = !edited_entity_uuid.isNull()
        && !this->simulation_stale_diagnostic_entity_uuids.contains(edited_entity_uuid);
    if (entity_became_stale)
        this->simulation_stale_diagnostic_entity_uuids.insert(edited_entity_uuid);

    if (timeline_became_stale || entity_became_stale)
        emit signalSimulationResultTimelineChanged(false);
    if (timeline_became_stale && this->water_quality_simulation_result_timeline.has_value())
        emit signalWaterQualitySimulationResultTimelineChanged(false);
    if (previous_index != -1)
        emit signalCurrentSimulationResultChanged(-1);
}

void HydraulicData::markNetworkChanged(NetworkChange change, const QUuid &edited_entity_uuid)
{
    markSimulationResultTimelineStale(edited_entity_uuid);
    rebuildSymbologyMinMaxValues();

    if (change == NetworkChange::Geometry)
        ++this->geometry_revision;

    ++this->visual_revision;
    this->time_changed_last = QDateTime::currentDateTimeUtc();

    if (change == NetworkChange::Geometry)
        emit signalNetworkGeometryChanged(this->geometry_revision);
}

bool HydraulicData::emitNodeChangedIfSuccessful(const QUuid &uuid, bool successful, NetworkChange change)
{
    if (!successful)
        return false;

    markNetworkChanged(change, uuid);

    const std::optional<InfrastructureEntity> entity_type = nodeEntityType(uuid);
    if (!entity_type.has_value())
        return false;
    emit signalNodeChanged(entity_type.value(), uuid);
    return true;
}

bool HydraulicData::emitLinkChangedIfSuccessful(const QUuid &uuid, bool successful, NetworkChange change)
{
    if (!successful)
        return false;

    markNetworkChanged(change, uuid);

    const std::optional<InfrastructureEntity> entity_type = linkEntityType(uuid);
    if (!entity_type.has_value())
        return false;
    emit signalLinkChanged(entity_type.value(), uuid);
    return true;
}

void HydraulicData::emitConnectedPipeChanges(const QUuid &node_uuid)
{
    for (const HydraulicLinkPipe &pipe : this->network_hydraulic.links_pipes)
    {
        if (pipe.node_uuid_from == node_uuid || pipe.node_uuid_to == node_uuid)
            emit signalLinkChanged(InfrastructureEntity::Pipe, pipe.uuid);
    }
}

bool HydraulicData::extendBoundingBoxWgs84(const CoordinateWGS84 &coordinate)
{
    if (!coordinateWgs84Valid(coordinate))
        return false;

    if (!this->bounding_box_wgs84_valid)
    {
        this->bounding_box_wgs84_minimum = coordinate;
        this->bounding_box_wgs84_maximum = coordinate;
        this->bounding_box_wgs84_valid = true;
        return true;
    }

    bool changed = false;

    if (coordinate.latitude_deg < this->bounding_box_wgs84_minimum.latitude_deg)
    {
        this->bounding_box_wgs84_minimum.latitude_deg = coordinate.latitude_deg;
        changed = true;
    }
    if (coordinate.longitude_deg < this->bounding_box_wgs84_minimum.longitude_deg)
    {
        this->bounding_box_wgs84_minimum.longitude_deg = coordinate.longitude_deg;
        changed = true;
    }
    if (coordinate.latitude_deg > this->bounding_box_wgs84_maximum.latitude_deg)
    {
        this->bounding_box_wgs84_maximum.latitude_deg = coordinate.latitude_deg;
        changed = true;
    }
    if (coordinate.longitude_deg > this->bounding_box_wgs84_maximum.longitude_deg)
    {
        this->bounding_box_wgs84_maximum.longitude_deg = coordinate.longitude_deg;
        changed = true;
    }

    return changed;
}

void HydraulicData::updateBoundingBoxWgs84(const CoordinateWGS84 &coordinate_previous,
                                           const CoordinateWGS84 &coordinate)
{
    if (!this->bounding_box_wgs84_valid)
    {
        if (extendBoundingBoxWgs84(coordinate))
            emit signalBoundingBoxWgs84Changed();
        return;
    }

    const bool coordinate_previous_valid = coordinateWgs84Valid(coordinate_previous);
    const bool coordinate_valid = coordinateWgs84Valid(coordinate);
    bool rebuild_required = false;

    if (coordinate_previous_valid)
    {
        const bool latitude_is_minimum =
            coordinate_previous.latitude_deg == this->bounding_box_wgs84_minimum.latitude_deg;
        const bool longitude_is_minimum =
            coordinate_previous.longitude_deg == this->bounding_box_wgs84_minimum.longitude_deg;
        const bool latitude_is_maximum =
            coordinate_previous.latitude_deg == this->bounding_box_wgs84_maximum.latitude_deg;
        const bool longitude_is_maximum =
            coordinate_previous.longitude_deg == this->bounding_box_wgs84_maximum.longitude_deg;

        if (!coordinate_valid)
        {
            rebuild_required = latitude_is_minimum || longitude_is_minimum ||
                               latitude_is_maximum || longitude_is_maximum;
        }
        else
        {
            rebuild_required =
                (latitude_is_minimum && coordinate.latitude_deg > coordinate_previous.latitude_deg) ||
                (longitude_is_minimum && coordinate.longitude_deg > coordinate_previous.longitude_deg) ||
                (latitude_is_maximum && coordinate.latitude_deg < coordinate_previous.latitude_deg) ||
                (longitude_is_maximum && coordinate.longitude_deg < coordinate_previous.longitude_deg);
        }
    }

    if (rebuild_required)
    {
        rebuildBoundingBoxWgs84();
        return;
    }

    if (extendBoundingBoxWgs84(coordinate))
        emit signalBoundingBoxWgs84Changed();
}

void HydraulicData::rebuildBoundingBoxWgs84()
{
    this->bounding_box_wgs84_minimum = CoordinateWGS84{};
    this->bounding_box_wgs84_maximum = CoordinateWGS84{};
    this->bounding_box_wgs84_valid = false;

    for (const HydraulicNodeJunction &junction : this->network_hydraulic.nodes_junctions)
        extendBoundingBoxWgs84(junction.coordinate_wgs84);
    for (const HydraulicNodeReservoir &reservoir : this->network_hydraulic.nodes_reservoirs)
        extendBoundingBoxWgs84(reservoir.coordinate_wgs84);
    for (const HydraulicNodeTank &tank : this->network_hydraulic.nodes_tanks)
        extendBoundingBoxWgs84(tank.coordinate_wgs84);
    for (const HydraulicDemandPoint &demand_point : this->network_hydraulic.demand_points)
        extendBoundingBoxWgs84(demand_point.coordinate_wgs84);

    for (const HydraulicLinkPipe &pipe : this->network_hydraulic.links_pipes)
    {
        for (const HydraulicLinkVertex &vertex : pipe.vertices)
            extendBoundingBoxWgs84(vertex.coordinate_wgs84);
    }
    for (const HydraulicLinkPump &pump : this->network_hydraulic.links_pumps)
    {
        for (const HydraulicLinkVertex &vertex : pump.vertices)
            extendBoundingBoxWgs84(vertex.coordinate_wgs84);
    }
    for (const HydraulicLinkValve &valve : this->network_hydraulic.links_valves)
    {
        for (const HydraulicLinkVertex &vertex : valve.vertices)
            extendBoundingBoxWgs84(vertex.coordinate_wgs84);
    }

    emit signalBoundingBoxWgs84Changed();
}

void HydraulicData::rebuildNetworkRenderSnapshot() const
{
    this->network_render_snapshot = buildNetworkRenderSnapshot(
        this->network_hydraulic, this->geometry_revision, this->visual_revision);
}

void HydraulicData::refreshNetworkRenderSnapshotIds()
{
    // An ID is no geometry: the revision stays, but the render snapshot
    // carries the IDs (map labels), so a current snapshot is rebuilt now
    // (deterministic: same render ids, same geometry). A stale one is
    // rebuilt on its next access anyway.
    if (this->network_render_snapshot.geometry_revision == this->geometry_revision)
        rebuildNetworkRenderSnapshot();
}

void HydraulicData::setSelectedUuid(InfrastructureEntity entity_type, const QUuid &uuid)
{
    switch (entity_type)
    {
    case InfrastructureEntity::Tank:
        for (const HydraulicNodeTank &tank : this->network_hydraulic.nodes_tanks)
        {
            if (tank.uuid == uuid)
            {
                emit signalSelectedTank(tank);
                return;
            }
        }
        
        break;
        
    case InfrastructureEntity::Reservoir:
        for (const HydraulicNodeReservoir &reservoir : this->network_hydraulic.nodes_reservoirs)
        {
            if (reservoir.uuid == uuid)
            {
                emit signalSelectedReservoir(reservoir);
                return;
            }
        }
        
        break;
        
    case InfrastructureEntity::Junction:
        for (const HydraulicNodeJunction &junction : this->network_hydraulic.nodes_junctions)
        {
            if (junction.uuid == uuid)
            {
                emit signalSelectedJunction(junction);
                return;
            }
        }
        
        break;
        
    case InfrastructureEntity::Pipe:
        for (const HydraulicLinkPipe &pipe : this->network_hydraulic.links_pipes)
        {
            if (pipe.uuid == uuid)
            {
                emit signalSelectedPipe(pipe);
                return;
            }
        }
        
        break;
        
    case InfrastructureEntity::Pump:
        for (const HydraulicLinkPump &pump : this->network_hydraulic.links_pumps)
        {
            if (pump.uuid == uuid)
            {
                emit signalSelectedPump(pump);
                return;
            }
        }
        
        break;
    
    case InfrastructureEntity::Valve:
        for (const HydraulicLinkValve &valve : this->network_hydraulic.links_valves)
        {
            if (valve.uuid == uuid)
            {
                emit signalSelectedValve(valve);
                return;
            }
        }
        
        break;
    
    case InfrastructureEntity::DemandPoint:
        for (const HydraulicDemandPoint &demand_point : this->network_hydraulic.demand_points)
        {
            if (demand_point.uuid == uuid)
            {
                emit signalSelectedDemandPoint(demand_point);
                return;
            }
        }
        
    default:
        break;
    }
}

void HydraulicData::requestDemandPointAttachmentSelection(const QUuid &uuid)
{
    if (!demandPoint(uuid).has_value())
        return;

    emit signalDemandPointAttachmentSelectionRequested(uuid);
}

void HydraulicData::requestEntityLocate(InfrastructureEntity entity_type, const QUuid &uuid)
{
    const bool demand_point_exists =
        entity_type == InfrastructureEntity::DemandPoint && demandPoint(uuid).has_value();
    if (!demand_point_exists &&
        !nodeCommonData(entity_type, uuid).has_value() &&
        !linkCommonData(entity_type, uuid).has_value())
    {
        return;
    }

    emit signalEntityLocateRequested(entity_type, uuid);
}

// HC-02.7: Create/update/delete rule controls and map labels through the
// same captured edit and collection-membership path as hydraulic libraries.
QUuid HydraulicData::addRuleControl(HydraulicControlRule entity)
{
    if (!hydraulicRuleReferencesValid(this->network_hydraulic, entity))
        return QUuid();
    entity.uuid = createUuidV7();
    if (entity.uuid.isNull() || hydraulicUuidOccurrences(this->network_hydraulic, entity.uuid) != 0 ||
        aowis::model::ModelCodec<HydraulicControlRule>::encode(entity).isEmpty())
        return QUuid();
    const QUuid uuid = entity.uuid;
    HydraulicEditTransactionCapture capture(QStringLiteral("hydraulic.controls.rules.create"));
    if (this->logical_edit_capture_enabled && !capture.watchCreated(uuid))
        return QUuid();
    this->network_hydraulic.controls_rules.append(std::move(entity));
    markNetworkChanged(NetworkChange::Visual, uuid);
    if (this->logical_edit_capture_enabled)
        publishCapturedEdit(capture);
    return uuid;
}

bool HydraulicData::updateRuleControl(const HydraulicControlRule &entity)
{
    if (entity.uuid.isNull() || hydraulicUuidOccurrences(this->network_hydraulic, entity.uuid) != 1 ||
        !hydraulicRuleReferencesValid(this->network_hydraulic, entity) ||
        aowis::model::ModelCodec<HydraulicControlRule>::encode(entity).isEmpty())
        return false;
    HydraulicControlRule *existing = editableLibraryItem(this->network_hydraulic.controls_rules, entity.uuid);
    if (existing == nullptr)
        return false;
    if (aowis::model::ModelCodec<HydraulicControlRule>::encode(*existing) ==
        aowis::model::ModelCodec<HydraulicControlRule>::encode(entity))
        return true;
    EditScope edit_scope(*this, QStringLiteral("hydraulic.controls.rules.update"), entity.uuid);
    *existing = entity;
    markNetworkChanged(NetworkChange::Visual, entity.uuid);
    return true;
}

bool HydraulicData::removeRuleControl(const QUuid &uuid)
{
    if (uuid.isNull() || hydraulicUuidOccurrences(this->network_hydraulic, uuid) != 1 ||
        editableLibraryItem(this->network_hydraulic.controls_rules, uuid) == nullptr)
        return false;
    EditScope edit_scope(*this, QStringLiteral("hydraulic.controls.rules.delete"), uuid);
    for (int index = 0; index < this->network_hydraulic.controls_rules.size(); ++index)
    {
        if (this->network_hydraulic.controls_rules.at(index).uuid != uuid) continue;
        this->network_hydraulic.controls_rules.removeAt(index);
        markNetworkChanged(NetworkChange::Visual, uuid);
        return true;
    }
    return false;
}

QUuid HydraulicData::addMapLabel(HydraulicMapLabel entity)
{
    if (!hydraulicLabelReferencesValid(this->network_hydraulic, entity))
        return QUuid();
    entity.uuid = createUuidV7();
    if (entity.uuid.isNull() || hydraulicUuidOccurrences(this->network_hydraulic, entity.uuid) != 0 ||
        aowis::model::ModelCodec<HydraulicMapLabel>::encode(entity).isEmpty())
        return QUuid();
    const QUuid uuid = entity.uuid;
    HydraulicEditTransactionCapture capture(QStringLiteral("hydraulic.map.labels.create"));
    if (this->logical_edit_capture_enabled && !capture.watchCreated(uuid))
        return QUuid();
    this->network_hydraulic.map_labels.append(std::move(entity));
    markNetworkChanged(NetworkChange::Visual, uuid);
    if (this->logical_edit_capture_enabled)
        publishCapturedEdit(capture);
    return uuid;
}

bool HydraulicData::updateMapLabel(const HydraulicMapLabel &entity)
{
    if (entity.uuid.isNull() || hydraulicUuidOccurrences(this->network_hydraulic, entity.uuid) != 1 ||
        !hydraulicLabelReferencesValid(this->network_hydraulic, entity) ||
        aowis::model::ModelCodec<HydraulicMapLabel>::encode(entity).isEmpty())
        return false;
    HydraulicMapLabel *existing = editableLibraryItem(this->network_hydraulic.map_labels, entity.uuid);
    if (existing == nullptr)
        return false;
    if (aowis::model::ModelCodec<HydraulicMapLabel>::encode(*existing) ==
        aowis::model::ModelCodec<HydraulicMapLabel>::encode(entity))
        return true;
    EditScope edit_scope(*this, QStringLiteral("hydraulic.map.labels.update"), entity.uuid);
    *existing = entity;
    markNetworkChanged(NetworkChange::Visual, entity.uuid);
    return true;
}

bool HydraulicData::removeMapLabel(const QUuid &uuid)
{
    if (uuid.isNull() || hydraulicUuidOccurrences(this->network_hydraulic, uuid) != 1 ||
        editableLibraryItem(this->network_hydraulic.map_labels, uuid) == nullptr)
        return false;
    EditScope edit_scope(*this, QStringLiteral("hydraulic.map.labels.delete"), uuid);
    for (int index = 0; index < this->network_hydraulic.map_labels.size(); ++index)
    {
        if (this->network_hydraulic.map_labels.at(index).uuid != uuid) continue;
        this->network_hydraulic.map_labels.removeAt(index);
        markNetworkChanged(NetworkChange::Visual, uuid);
        return true;
    }
    return false;
}

// Complete HC-02 library CRUD: every operation has exactly one canonical
// entity edit, with no mutation if UUID or reference validation fails.
QUuid HydraulicData::addPipeMaterial(HydraulicPipeMaterial entity)
{
    entity.uuid = createUuidV7();
    if (entity.uuid.isNull() || hydraulicUuidOccurrences(this->network_hydraulic, entity.uuid) != 0 ||
        aowis::model::ModelCodec<HydraulicPipeMaterial>::encode(entity).isEmpty())
        return QUuid();
    const QUuid uuid = entity.uuid;
    HydraulicEditTransactionCapture capture(QStringLiteral("hydraulic.library.pipe_materials.create"));
    if (this->logical_edit_capture_enabled && !capture.watchCreated(uuid))
        return QUuid();
    this->network_hydraulic.pipe_materials.append(std::move(entity));
    markNetworkChanged(NetworkChange::Visual, uuid);
    if (this->logical_edit_capture_enabled)
        publishCapturedEdit(capture);
    return uuid;
}

bool HydraulicData::updatePipeMaterial(const HydraulicPipeMaterial &entity)
{
    if (entity.uuid.isNull() || hydraulicUuidOccurrences(this->network_hydraulic, entity.uuid) != 1 ||
        aowis::model::ModelCodec<HydraulicPipeMaterial>::encode(entity).isEmpty())
        return false;
    HydraulicPipeMaterial *existing = editableLibraryItem(this->network_hydraulic.pipe_materials, entity.uuid);
    if (existing == nullptr)
        return false;
    if (aowis::model::ModelCodec<HydraulicPipeMaterial>::encode(*existing) ==
        aowis::model::ModelCodec<HydraulicPipeMaterial>::encode(entity))
        return true;
    EditScope edit_scope(*this, QStringLiteral("hydraulic.library.pipe_materials.update"), entity.uuid);
    *existing = entity;
    markNetworkChanged(NetworkChange::Visual, entity.uuid);
    return true;
}

bool HydraulicData::removePipeMaterial(const QUuid &uuid)
{
    if (uuid.isNull() || hydraulicUuidOccurrences(this->network_hydraulic, uuid) != 1 ||
        pipeMaterialReferenced(this->network_hydraulic, uuid))
        return false;
    QList<HydraulicPipeMaterial> &entities = this->network_hydraulic.pipe_materials;
    if (editableLibraryItem(entities, uuid) == nullptr)
        return false;
    EditScope edit_scope(*this, QStringLiteral("hydraulic.library.pipe_materials.delete"), uuid);
    for (int index = 0; index < entities.size(); ++index)
    {
        if (entities.at(index).uuid != uuid)
            continue;
        entities.removeAt(index);
        markNetworkChanged(NetworkChange::Visual, uuid);
        return true;
    }
    return false;
}

QUuid HydraulicData::addPatternTime(HydraulicPatternTime entity)
{
    entity.uuid = createUuidV7();
    if (entity.uuid.isNull() || hydraulicUuidOccurrences(this->network_hydraulic, entity.uuid) != 0 ||
        aowis::model::ModelCodec<HydraulicPatternTime>::encode(entity).isEmpty())
        return QUuid();
    const QUuid uuid = entity.uuid;
    HydraulicEditTransactionCapture capture(QStringLiteral("hydraulic.library.patterns_time.create"));
    if (this->logical_edit_capture_enabled && !capture.watchCreated(uuid))
        return QUuid();
    this->network_hydraulic.patterns_time.append(std::move(entity));
    markNetworkChanged(NetworkChange::Visual, uuid);
    if (this->logical_edit_capture_enabled)
        publishCapturedEdit(capture);
    return uuid;
}

bool HydraulicData::updatePatternTime(const HydraulicPatternTime &entity)
{
    if (entity.uuid.isNull() || hydraulicUuidOccurrences(this->network_hydraulic, entity.uuid) != 1 ||
        aowis::model::ModelCodec<HydraulicPatternTime>::encode(entity).isEmpty())
        return false;
    HydraulicPatternTime *existing = editableLibraryItem(this->network_hydraulic.patterns_time, entity.uuid);
    if (existing == nullptr)
        return false;
    if (aowis::model::ModelCodec<HydraulicPatternTime>::encode(*existing) ==
        aowis::model::ModelCodec<HydraulicPatternTime>::encode(entity))
        return true;
    EditScope edit_scope(*this, QStringLiteral("hydraulic.library.patterns_time.update"), entity.uuid);
    *existing = entity;
    markNetworkChanged(NetworkChange::Visual, entity.uuid);
    return true;
}

bool HydraulicData::removePatternTime(const QUuid &uuid)
{
    if (uuid.isNull() || hydraulicUuidOccurrences(this->network_hydraulic, uuid) != 1 ||
        timePatternReferenced(this->network_hydraulic, uuid))
        return false;
    QList<HydraulicPatternTime> &entities = this->network_hydraulic.patterns_time;
    if (editableLibraryItem(entities, uuid) == nullptr)
        return false;
    EditScope edit_scope(*this, QStringLiteral("hydraulic.library.patterns_time.delete"), uuid);
    for (int index = 0; index < entities.size(); ++index)
    {
        if (entities.at(index).uuid != uuid)
            continue;
        entities.removeAt(index);
        markNetworkChanged(NetworkChange::Visual, uuid);
        return true;
    }
    return false;
}

QUuid HydraulicData::addCurveTankVolume(HydraulicCurveTankVolume entity)
{
    entity.uuid = createUuidV7();
    if (entity.uuid.isNull() || hydraulicUuidOccurrences(this->network_hydraulic, entity.uuid) != 0 ||
        aowis::model::ModelCodec<HydraulicCurveTankVolume>::encode(entity).isEmpty())
        return QUuid();
    const QUuid uuid = entity.uuid;
    HydraulicEditTransactionCapture capture(QStringLiteral("hydraulic.library.curves_tank_volume.create"));
    if (this->logical_edit_capture_enabled && !capture.watchCreated(uuid))
        return QUuid();
    this->network_hydraulic.curves_tank_volume.append(std::move(entity));
    markNetworkChanged(NetworkChange::Visual, uuid);
    if (this->logical_edit_capture_enabled)
        publishCapturedEdit(capture);
    return uuid;
}

bool HydraulicData::updateCurveTankVolume(const HydraulicCurveTankVolume &entity)
{
    if (entity.uuid.isNull() || hydraulicUuidOccurrences(this->network_hydraulic, entity.uuid) != 1 ||
        aowis::model::ModelCodec<HydraulicCurveTankVolume>::encode(entity).isEmpty())
        return false;
    HydraulicCurveTankVolume *existing = editableLibraryItem(this->network_hydraulic.curves_tank_volume, entity.uuid);
    if (existing == nullptr)
        return false;
    if (aowis::model::ModelCodec<HydraulicCurveTankVolume>::encode(*existing) ==
        aowis::model::ModelCodec<HydraulicCurveTankVolume>::encode(entity))
        return true;
    EditScope edit_scope(*this, QStringLiteral("hydraulic.library.curves_tank_volume.update"), entity.uuid);
    *existing = entity;
    markNetworkChanged(NetworkChange::Visual, entity.uuid);
    return true;
}

bool HydraulicData::removeCurveTankVolume(const QUuid &uuid)
{
    if (uuid.isNull() || hydraulicUuidOccurrences(this->network_hydraulic, uuid) != 1 ||
        tankVolumeCurveReferenced(this->network_hydraulic, uuid))
        return false;
    QList<HydraulicCurveTankVolume> &entities = this->network_hydraulic.curves_tank_volume;
    if (editableLibraryItem(entities, uuid) == nullptr)
        return false;
    EditScope edit_scope(*this, QStringLiteral("hydraulic.library.curves_tank_volume.delete"), uuid);
    for (int index = 0; index < entities.size(); ++index)
    {
        if (entities.at(index).uuid != uuid)
            continue;
        entities.removeAt(index);
        markNetworkChanged(NetworkChange::Visual, uuid);
        return true;
    }
    return false;
}

QUuid HydraulicData::addCurvePumpHead(HydraulicCurvePumpHead entity)
{
    entity.uuid = createUuidV7();
    if (entity.uuid.isNull() || hydraulicUuidOccurrences(this->network_hydraulic, entity.uuid) != 0 ||
        aowis::model::ModelCodec<HydraulicCurvePumpHead>::encode(entity).isEmpty())
        return QUuid();
    const QUuid uuid = entity.uuid;
    HydraulicEditTransactionCapture capture(QStringLiteral("hydraulic.library.curves_pump_head.create"));
    if (this->logical_edit_capture_enabled && !capture.watchCreated(uuid))
        return QUuid();
    this->network_hydraulic.curves_pump_head.append(std::move(entity));
    markNetworkChanged(NetworkChange::Visual, uuid);
    if (this->logical_edit_capture_enabled)
        publishCapturedEdit(capture);
    return uuid;
}

bool HydraulicData::updateCurvePumpHead(const HydraulicCurvePumpHead &entity)
{
    if (entity.uuid.isNull() || hydraulicUuidOccurrences(this->network_hydraulic, entity.uuid) != 1 ||
        aowis::model::ModelCodec<HydraulicCurvePumpHead>::encode(entity).isEmpty())
        return false;
    HydraulicCurvePumpHead *existing = editableLibraryItem(this->network_hydraulic.curves_pump_head, entity.uuid);
    if (existing == nullptr)
        return false;
    if (aowis::model::ModelCodec<HydraulicCurvePumpHead>::encode(*existing) ==
        aowis::model::ModelCodec<HydraulicCurvePumpHead>::encode(entity))
        return true;
    EditScope edit_scope(*this, QStringLiteral("hydraulic.library.curves_pump_head.update"), entity.uuid);
    *existing = entity;
    markNetworkChanged(NetworkChange::Visual, entity.uuid);
    return true;
}

bool HydraulicData::removeCurvePumpHead(const QUuid &uuid)
{
    if (uuid.isNull() || hydraulicUuidOccurrences(this->network_hydraulic, uuid) != 1 ||
        pumpHeadCurveReferenced(this->network_hydraulic, uuid))
        return false;
    QList<HydraulicCurvePumpHead> &entities = this->network_hydraulic.curves_pump_head;
    if (editableLibraryItem(entities, uuid) == nullptr)
        return false;
    EditScope edit_scope(*this, QStringLiteral("hydraulic.library.curves_pump_head.delete"), uuid);
    for (int index = 0; index < entities.size(); ++index)
    {
        if (entities.at(index).uuid != uuid)
            continue;
        entities.removeAt(index);
        markNetworkChanged(NetworkChange::Visual, uuid);
        return true;
    }
    return false;
}

QUuid HydraulicData::addCurvePumpEfficiency(HydraulicCurvePumpEfficiency entity)
{
    entity.uuid = createUuidV7();
    if (entity.uuid.isNull() || hydraulicUuidOccurrences(this->network_hydraulic, entity.uuid) != 0 ||
        aowis::model::ModelCodec<HydraulicCurvePumpEfficiency>::encode(entity).isEmpty())
        return QUuid();
    const QUuid uuid = entity.uuid;
    HydraulicEditTransactionCapture capture(QStringLiteral("hydraulic.library.curves_pump_efficiency.create"));
    if (this->logical_edit_capture_enabled && !capture.watchCreated(uuid))
        return QUuid();
    this->network_hydraulic.curves_pump_efficiency.append(std::move(entity));
    markNetworkChanged(NetworkChange::Visual, uuid);
    if (this->logical_edit_capture_enabled)
        publishCapturedEdit(capture);
    return uuid;
}

bool HydraulicData::updateCurvePumpEfficiency(const HydraulicCurvePumpEfficiency &entity)
{
    if (entity.uuid.isNull() || hydraulicUuidOccurrences(this->network_hydraulic, entity.uuid) != 1 ||
        aowis::model::ModelCodec<HydraulicCurvePumpEfficiency>::encode(entity).isEmpty())
        return false;
    HydraulicCurvePumpEfficiency *existing = editableLibraryItem(this->network_hydraulic.curves_pump_efficiency, entity.uuid);
    if (existing == nullptr)
        return false;
    if (aowis::model::ModelCodec<HydraulicCurvePumpEfficiency>::encode(*existing) ==
        aowis::model::ModelCodec<HydraulicCurvePumpEfficiency>::encode(entity))
        return true;
    EditScope edit_scope(*this, QStringLiteral("hydraulic.library.curves_pump_efficiency.update"), entity.uuid);
    *existing = entity;
    markNetworkChanged(NetworkChange::Visual, entity.uuid);
    return true;
}

bool HydraulicData::removeCurvePumpEfficiency(const QUuid &uuid)
{
    if (uuid.isNull() || hydraulicUuidOccurrences(this->network_hydraulic, uuid) != 1 ||
        pumpEfficiencyCurveReferenced(this->network_hydraulic, uuid))
        return false;
    QList<HydraulicCurvePumpEfficiency> &entities = this->network_hydraulic.curves_pump_efficiency;
    if (editableLibraryItem(entities, uuid) == nullptr)
        return false;
    EditScope edit_scope(*this, QStringLiteral("hydraulic.library.curves_pump_efficiency.delete"), uuid);
    for (int index = 0; index < entities.size(); ++index)
    {
        if (entities.at(index).uuid != uuid)
            continue;
        entities.removeAt(index);
        markNetworkChanged(NetworkChange::Visual, uuid);
        return true;
    }
    return false;
}

QUuid HydraulicData::addCurveValveHeadloss(HydraulicCurveValveHeadloss entity)
{
    entity.uuid = createUuidV7();
    if (entity.uuid.isNull() || hydraulicUuidOccurrences(this->network_hydraulic, entity.uuid) != 0 ||
        aowis::model::ModelCodec<HydraulicCurveValveHeadloss>::encode(entity).isEmpty())
        return QUuid();
    const QUuid uuid = entity.uuid;
    HydraulicEditTransactionCapture capture(QStringLiteral("hydraulic.library.curves_valve_headloss.create"));
    if (this->logical_edit_capture_enabled && !capture.watchCreated(uuid))
        return QUuid();
    this->network_hydraulic.curves_valve_headloss.append(std::move(entity));
    markNetworkChanged(NetworkChange::Visual, uuid);
    if (this->logical_edit_capture_enabled)
        publishCapturedEdit(capture);
    return uuid;
}

bool HydraulicData::updateCurveValveHeadloss(const HydraulicCurveValveHeadloss &entity)
{
    if (entity.uuid.isNull() || hydraulicUuidOccurrences(this->network_hydraulic, entity.uuid) != 1 ||
        aowis::model::ModelCodec<HydraulicCurveValveHeadloss>::encode(entity).isEmpty())
        return false;
    HydraulicCurveValveHeadloss *existing = editableLibraryItem(this->network_hydraulic.curves_valve_headloss, entity.uuid);
    if (existing == nullptr)
        return false;
    if (aowis::model::ModelCodec<HydraulicCurveValveHeadloss>::encode(*existing) ==
        aowis::model::ModelCodec<HydraulicCurveValveHeadloss>::encode(entity))
        return true;
    EditScope edit_scope(*this, QStringLiteral("hydraulic.library.curves_valve_headloss.update"), entity.uuid);
    *existing = entity;
    markNetworkChanged(NetworkChange::Visual, entity.uuid);
    return true;
}

bool HydraulicData::removeCurveValveHeadloss(const QUuid &uuid)
{
    if (uuid.isNull() || hydraulicUuidOccurrences(this->network_hydraulic, uuid) != 1 ||
        valveHeadlossCurveReferenced(this->network_hydraulic, uuid))
        return false;
    QList<HydraulicCurveValveHeadloss> &entities = this->network_hydraulic.curves_valve_headloss;
    if (editableLibraryItem(entities, uuid) == nullptr)
        return false;
    EditScope edit_scope(*this, QStringLiteral("hydraulic.library.curves_valve_headloss.delete"), uuid);
    for (int index = 0; index < entities.size(); ++index)
    {
        if (entities.at(index).uuid != uuid)
            continue;
        entities.removeAt(index);
        markNetworkChanged(NetworkChange::Visual, uuid);
        return true;
    }
    return false;
}

QUuid HydraulicData::addCurveValveCharacteristic(HydraulicCurveValveCharacteristic entity)
{
    entity.uuid = createUuidV7();
    if (entity.uuid.isNull() || hydraulicUuidOccurrences(this->network_hydraulic, entity.uuid) != 0 ||
        aowis::model::ModelCodec<HydraulicCurveValveCharacteristic>::encode(entity).isEmpty())
        return QUuid();
    const QUuid uuid = entity.uuid;
    HydraulicEditTransactionCapture capture(QStringLiteral("hydraulic.library.curves_valve_characteristic.create"));
    if (this->logical_edit_capture_enabled && !capture.watchCreated(uuid))
        return QUuid();
    this->network_hydraulic.curves_valve_characteristic.append(std::move(entity));
    markNetworkChanged(NetworkChange::Visual, uuid);
    if (this->logical_edit_capture_enabled)
        publishCapturedEdit(capture);
    return uuid;
}

bool HydraulicData::updateCurveValveCharacteristic(const HydraulicCurveValveCharacteristic &entity)
{
    if (entity.uuid.isNull() || hydraulicUuidOccurrences(this->network_hydraulic, entity.uuid) != 1 ||
        aowis::model::ModelCodec<HydraulicCurveValveCharacteristic>::encode(entity).isEmpty())
        return false;
    HydraulicCurveValveCharacteristic *existing = editableLibraryItem(this->network_hydraulic.curves_valve_characteristic, entity.uuid);
    if (existing == nullptr)
        return false;
    if (aowis::model::ModelCodec<HydraulicCurveValveCharacteristic>::encode(*existing) ==
        aowis::model::ModelCodec<HydraulicCurveValveCharacteristic>::encode(entity))
        return true;
    EditScope edit_scope(*this, QStringLiteral("hydraulic.library.curves_valve_characteristic.update"), entity.uuid);
    *existing = entity;
    markNetworkChanged(NetworkChange::Visual, entity.uuid);
    return true;
}

bool HydraulicData::removeCurveValveCharacteristic(const QUuid &uuid)
{
    if (uuid.isNull() || hydraulicUuidOccurrences(this->network_hydraulic, uuid) != 1 ||
        valveCharacteristicCurveReferenced(this->network_hydraulic, uuid))
        return false;
    QList<HydraulicCurveValveCharacteristic> &entities = this->network_hydraulic.curves_valve_characteristic;
    if (editableLibraryItem(entities, uuid) == nullptr)
        return false;
    EditScope edit_scope(*this, QStringLiteral("hydraulic.library.curves_valve_characteristic.delete"), uuid);
    for (int index = 0; index < entities.size(); ++index)
    {
        if (entities.at(index).uuid != uuid)
            continue;
        entities.removeAt(index);
        markNetworkChanged(NetworkChange::Visual, uuid);
        return true;
    }
    return false;
}

QUuid HydraulicData::addCurveGeneric(HydraulicCurveGeneric entity)
{
    entity.uuid = createUuidV7();
    if (entity.uuid.isNull() || hydraulicUuidOccurrences(this->network_hydraulic, entity.uuid) != 0 ||
        aowis::model::ModelCodec<HydraulicCurveGeneric>::encode(entity).isEmpty())
        return QUuid();
    const QUuid uuid = entity.uuid;
    HydraulicEditTransactionCapture capture(QStringLiteral("hydraulic.library.curves_generic.create"));
    if (this->logical_edit_capture_enabled && !capture.watchCreated(uuid))
        return QUuid();
    this->network_hydraulic.curves_generic.append(std::move(entity));
    markNetworkChanged(NetworkChange::Visual, uuid);
    if (this->logical_edit_capture_enabled)
        publishCapturedEdit(capture);
    return uuid;
}

bool HydraulicData::updateCurveGeneric(const HydraulicCurveGeneric &entity)
{
    if (entity.uuid.isNull() || hydraulicUuidOccurrences(this->network_hydraulic, entity.uuid) != 1 ||
        aowis::model::ModelCodec<HydraulicCurveGeneric>::encode(entity).isEmpty())
        return false;
    HydraulicCurveGeneric *existing = editableLibraryItem(this->network_hydraulic.curves_generic, entity.uuid);
    if (existing == nullptr)
        return false;
    if (aowis::model::ModelCodec<HydraulicCurveGeneric>::encode(*existing) ==
        aowis::model::ModelCodec<HydraulicCurveGeneric>::encode(entity))
        return true;
    EditScope edit_scope(*this, QStringLiteral("hydraulic.library.curves_generic.update"), entity.uuid);
    *existing = entity;
    markNetworkChanged(NetworkChange::Visual, entity.uuid);
    return true;
}

bool HydraulicData::removeCurveGeneric(const QUuid &uuid)
{
    if (uuid.isNull() || hydraulicUuidOccurrences(this->network_hydraulic, uuid) != 1 ||
        noLibraryReference(this->network_hydraulic, uuid))
        return false;
    QList<HydraulicCurveGeneric> &entities = this->network_hydraulic.curves_generic;
    if (editableLibraryItem(entities, uuid) == nullptr)
        return false;
    EditScope edit_scope(*this, QStringLiteral("hydraulic.library.curves_generic.delete"), uuid);
    for (int index = 0; index < entities.size(); ++index)
    {
        if (entities.at(index).uuid != uuid)
            continue;
        entities.removeAt(index);
        markNetworkChanged(NetworkChange::Visual, uuid);
        return true;
    }
    return false;
}

QUuid HydraulicData::addJunction(const CoordinateWGS84 &coordinate)
{
    HydraulicEditTransactionCapture capture(QStringLiteral("hydraulic.junction.create"));
    const QUuid uuid = this->network_editor.addJunction(coordinate);
    if (!uuid.isNull())
    {
        if (extendBoundingBoxWgs84(coordinate))
            emit signalBoundingBoxWgs84Changed();
        markNetworkChanged(NetworkChange::Geometry, uuid);
    }
    if (this->logical_edit_capture_enabled && !uuid.isNull())
    {
        capture.watchCreated(uuid);
        publishCapturedEdit(capture);
    }
    return uuid;
}

QUuid HydraulicData::addReservoir(const CoordinateWGS84 &coordinate)
{
    HydraulicEditTransactionCapture capture(QStringLiteral("hydraulic.reservoir.create"));
    const QUuid uuid = this->network_editor.addReservoir(coordinate);
    if (!uuid.isNull())
    {
        if (extendBoundingBoxWgs84(coordinate))
            emit signalBoundingBoxWgs84Changed();
        markNetworkChanged(NetworkChange::Geometry, uuid);
    }
    if (this->logical_edit_capture_enabled && !uuid.isNull())
    {
        capture.watchCreated(uuid);
        publishCapturedEdit(capture);
    }
    return uuid;
}

QUuid HydraulicData::addTank(const CoordinateWGS84 &coordinate)
{
    HydraulicEditTransactionCapture capture(QStringLiteral("hydraulic.tank.create"));
    const QUuid uuid = this->network_editor.addTank(coordinate);
    if (!uuid.isNull())
    {
        if (extendBoundingBoxWgs84(coordinate))
            emit signalBoundingBoxWgs84Changed();
        markNetworkChanged(NetworkChange::Geometry, uuid);
    }
    if (this->logical_edit_capture_enabled && !uuid.isNull())
    {
        capture.watchCreated(uuid);
        publishCapturedEdit(capture);
    }
    return uuid;
}

QUuid HydraulicData::addDemandPoint(const CoordinateWGS84 &coordinate)
{
    HydraulicEditTransactionCapture capture(QStringLiteral("hydraulic.demand-point.create"));
    const QUuid uuid = this->network_editor.addDemandPoint(coordinate);
    if (!uuid.isNull())
    {
        if (extendBoundingBoxWgs84(coordinate))
            emit signalBoundingBoxWgs84Changed();
        markNetworkChanged(NetworkChange::Geometry, uuid);
        emit signalDemandPointChanged(uuid);
    }
    if (this->logical_edit_capture_enabled && !uuid.isNull())
    {
        capture.watchCreated(uuid);
        publishCapturedEdit(capture);
    }
    return uuid;
}

QUuid HydraulicData::addPipe(const QUuid &node_uuid_from, const QUuid &node_uuid_to,
                             const QList<CoordinateWGS84> &intermediate_vertices)
{
    HydraulicEditTransactionCapture capture(QStringLiteral("hydraulic.pipe.create"));
    const QUuid uuid =
        this->network_editor.addPipe(node_uuid_from, node_uuid_to, intermediate_vertices);
    if (!uuid.isNull())
    {
        bool bounding_box_changed = false;
        for (const CoordinateWGS84 &coordinate : intermediate_vertices)
            bounding_box_changed = extendBoundingBoxWgs84(coordinate) || bounding_box_changed;
        if (bounding_box_changed)
            emit signalBoundingBoxWgs84Changed();
        markNetworkChanged(NetworkChange::Geometry, uuid);
    }
    if (this->logical_edit_capture_enabled && !uuid.isNull())
    {
        capture.watchCreated(uuid);
        publishCapturedEdit(capture);
    }
    return uuid;
}

QUuid HydraulicData::addPump(const QUuid &node_uuid_from, const QUuid &node_uuid_to,
                             const CoordinateWGS84 &center_coordinate)
{
    HydraulicEditTransactionCapture capture(QStringLiteral("hydraulic.pump.create"));
    const QUuid uuid =
        this->network_editor.addPump(node_uuid_from, node_uuid_to, center_coordinate);
    if (!uuid.isNull())
    {
        if (extendBoundingBoxWgs84(center_coordinate))
            emit signalBoundingBoxWgs84Changed();
        markNetworkChanged(NetworkChange::Geometry, uuid);
    }
    if (this->logical_edit_capture_enabled && !uuid.isNull())
    {
        capture.watchCreated(uuid);
        publishCapturedEdit(capture);
    }
    return uuid;
}

QUuid HydraulicData::addValve(const QUuid &node_uuid_from, const QUuid &node_uuid_to,
                              const CoordinateWGS84 &center_coordinate)
{
    HydraulicEditTransactionCapture capture(QStringLiteral("hydraulic.valve.create"));
    const QUuid uuid =
        this->network_editor.addValve(node_uuid_from, node_uuid_to, center_coordinate);
    if (!uuid.isNull())
    {
        if (extendBoundingBoxWgs84(center_coordinate))
            emit signalBoundingBoxWgs84Changed();
        markNetworkChanged(NetworkChange::Geometry, uuid);
    }
    if (this->logical_edit_capture_enabled && !uuid.isNull())
    {
        capture.watchCreated(uuid);
        publishCapturedEdit(capture);
    }
    return uuid;
}

bool HydraulicData::setNodeId(const QUuid &uuid, const QString &id)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setNodeId"), uuid);
    if (!this->network_editor.setNodeId(uuid, id))
        return false;
    refreshNetworkRenderSnapshotIds();
    return emitNodeChangedIfSuccessful(uuid, true);
}

bool HydraulicData::setNodeModelRole(const QUuid &uuid, EntityModelRole model_role)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setNodeModelRole"), uuid);
    return emitNodeChangedIfSuccessful(
        uuid, this->network_editor.setNodeModelRole(uuid, model_role));
}

bool HydraulicData::setNodeDateAdded(const QUuid &uuid, const std::optional<QDate> &date_added)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setNodeDateAdded"), uuid);
    return emitNodeChangedIfSuccessful(
        uuid, this->network_editor.setNodeDateAdded(uuid, date_added));
}

bool HydraulicData::setNodeDateInstalled(const QUuid &uuid,
                                           const std::optional<QDate> &date_installed)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setNodeDateInstalled"), uuid);
    return emitNodeChangedIfSuccessful(
        uuid, this->network_editor.setNodeDateInstalled(uuid, date_installed));
}

bool HydraulicData::setNodeEnabled(const QUuid &uuid, bool enabled)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setNodeEnabled"), uuid);
    return emitNodeChangedIfSuccessful(uuid, this->network_editor.setNodeEnabled(uuid, enabled));
}

bool HydraulicData::setNodeCoordinate(const QUuid &uuid, const CoordinateWGS84 &coordinate)
{
    HydraulicEditTransactionCapture capture(QStringLiteral("hydraulic.node.move"));
    if (this->logical_edit_capture_enabled)
    {
        capture.watch(this->network_hydraulic, uuid);
        for (const HydraulicLinkPipe &pipe : this->network_hydraulic.links_pipes)
        {
            if (pipe.node_uuid_from == uuid || pipe.node_uuid_to == uuid)
                capture.watch(this->network_hydraulic, pipe.uuid);
        }
    }

    const std::optional<InfrastructureEntity> entity_type = nodeEntityType(uuid);
    if (!entity_type.has_value())
        return false;
    const std::optional<CoordinateWGS84> coordinate_previous =
        this->network_editor.nodeCoordinate(uuid);
    if (!coordinate_previous.has_value())
        return false;
    if (!this->network_editor.setNodeCoordinate(uuid, coordinate))
        return false;

    updateBoundingBoxWgs84(coordinate_previous.value(), coordinate);
    markNetworkChanged(NetworkChange::Geometry, uuid);
    emit signalNodeChanged(entity_type.value(), uuid);
    emitConnectedPipeChanges(uuid);
    if (this->logical_edit_capture_enabled)
        publishCapturedEdit(capture);
    return true;
}

bool HydraulicData::setLinkId(const QUuid &uuid, const QString &id)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setLinkId"), uuid);
    if (!this->network_editor.setLinkId(uuid, id))
        return false;
    refreshNetworkRenderSnapshotIds();
    return emitLinkChangedIfSuccessful(uuid, true);
}

bool HydraulicData::setLinkModelRole(const QUuid &uuid, EntityModelRole model_role)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setLinkModelRole"), uuid);
    return emitLinkChangedIfSuccessful(
        uuid, this->network_editor.setLinkModelRole(uuid, model_role));
}

bool HydraulicData::setLinkDateAdded(const QUuid &uuid,
                                     const std::optional<QDate> &date_added)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setLinkDateAdded"), uuid);
    return emitLinkChangedIfSuccessful(
        uuid, this->network_editor.setLinkDateAdded(uuid, date_added));
}

bool HydraulicData::setLinkDateInstalled(const QUuid &uuid,
                                         const std::optional<QDate> &date_installed)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setLinkDateInstalled"), uuid);
    return emitLinkChangedIfSuccessful(
        uuid, this->network_editor.setLinkDateInstalled(uuid, date_installed));
}

bool HydraulicData::setLinkEnabled(const QUuid &uuid, bool enabled)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setLinkEnabled"), uuid);
    return emitLinkChangedIfSuccessful(uuid, this->network_editor.setLinkEnabled(uuid, enabled));
}

bool HydraulicData::setDemandPointId(const QUuid &uuid, const QString &id)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setDemandPointId"), uuid);
    if (!this->network_editor.setDemandPointId(uuid, id))
        return false;

    refreshNetworkRenderSnapshotIds();
    markNetworkChanged(NetworkChange::Visual, uuid);
    emit signalDemandPointChanged(uuid);
    return true;
}

bool HydraulicData::setDemandPointModelRole(const QUuid &uuid, EntityModelRole model_role)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setDemandPointModelRole"), uuid);
    if (!this->network_editor.setDemandPointModelRole(uuid, model_role))
        return false;

    markNetworkChanged(NetworkChange::Visual, uuid);
    emit signalDemandPointChanged(uuid);
    return true;
}

bool HydraulicData::setDemandPointDateAdded(
    const QUuid &uuid, const std::optional<QDate> &date_added)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setDemandPointDateAdded"), uuid);
    if (!this->network_editor.setDemandPointDateAdded(uuid, date_added))
        return false;

    markNetworkChanged(NetworkChange::Visual, uuid);
    emit signalDemandPointChanged(uuid);
    return true;
}

bool HydraulicData::setDemandPointDateInstalled(
    const QUuid &uuid, const std::optional<QDate> &date_installed)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setDemandPointDateInstalled"), uuid);
    if (!this->network_editor.setDemandPointDateInstalled(uuid, date_installed))
        return false;

    markNetworkChanged(NetworkChange::Visual, uuid);
    emit signalDemandPointChanged(uuid);
    return true;
}

bool HydraulicData::setDemandPointEnabled(const QUuid &uuid, bool enabled)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setDemandPointEnabled"), uuid);
    if (!this->network_editor.setDemandPointEnabled(uuid, enabled))
        return false;

    markNetworkChanged(NetworkChange::Visual, uuid);
    emit signalDemandPointChanged(uuid);
    return true;
}

bool HydraulicData::setDemandPointDescription(const QUuid &uuid, const QString &description)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setDemandPointDescription"), uuid);
    if (!this->network_editor.setDemandPointDescription(uuid, description))
        return false;

    markNetworkChanged(NetworkChange::Visual, uuid);
    emit signalDemandPointChanged(uuid);
    return true;
}

bool HydraulicData::setDemandPointComment(const QUuid &uuid, const QString &comment)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setDemandPointComment"), uuid);
    if (!this->network_editor.setDemandPointComment(uuid, comment))
        return false;

    markNetworkChanged(NetworkChange::Visual, uuid);
    emit signalDemandPointChanged(uuid);
    return true;
}

bool HydraulicData::setDemandPointTags(const QUuid &uuid, const QStringList &tags)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setDemandPointTags"), uuid);
    if (!this->network_editor.setDemandPointTags(uuid, tags))
        return false;

    markNetworkChanged(NetworkChange::Visual, uuid);
    emit signalDemandPointChanged(uuid);
    return true;
}

bool HydraulicData::setDemandPointCoordinate(
    const QUuid &uuid, const CoordinateWGS84 &coordinate)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setDemandPointCoordinate"), uuid);
    const std::optional<HydraulicDemandPoint> demand_point = this->network_editor.demandPoint(uuid);
    if (!demand_point.has_value())
        return false;

    if (!this->network_editor.setDemandPointCoordinate(uuid, coordinate))
        return false;

    updateBoundingBoxWgs84(demand_point->coordinate_wgs84, coordinate);
    markNetworkChanged(NetworkChange::Geometry, uuid);
    emit signalDemandPointChanged(uuid);
    return true;
}

bool HydraulicData::addDemandPointDemand(const QUuid &uuid, const HydraulicDemand &demand)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.addDemandPointDemand"), uuid);
    if (!this->network_editor.addDemandPointDemand(uuid, demand))
        return false;

    markNetworkChanged(NetworkChange::Visual, uuid);
    emit signalDemandPointChanged(uuid);
    return true;
}

bool HydraulicData::removeDemandPointDemand(const QUuid &uuid, int demand_index)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.removeDemandPointDemand"), uuid);
    if (!this->network_editor.removeDemandPointDemand(uuid, demand_index))
        return false;

    markNetworkChanged(NetworkChange::Visual, uuid);
    emit signalDemandPointChanged(uuid);
    return true;
}

bool HydraulicData::setDemandPointDemandCategoryName(
    const QUuid &uuid, int demand_index, const QString &category_name)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setDemandPointDemandCategoryName"), uuid);
    if (!this->network_editor.setDemandPointDemandCategoryName(uuid, demand_index, category_name))
        return false;

    markNetworkChanged(NetworkChange::Visual, uuid);
    emit signalDemandPointChanged(uuid);
    return true;
}

bool HydraulicData::setDemandPointDemandBaseDemandM3PerH(
    const QUuid &uuid, int demand_index, double base_demand_m3_per_h)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setDemandPointDemandBaseDemandM3PerH"), uuid);
    if (!this->network_editor.setDemandPointDemandBaseDemandM3PerH(
            uuid, demand_index, base_demand_m3_per_h))
        return false;

    markNetworkChanged(NetworkChange::Visual, uuid);
    emit signalDemandPointChanged(uuid);
    return true;
}

bool HydraulicData::setDemandPointDemandPatternMode(
    const QUuid &uuid, int demand_index, HydraulicTimePatternMode pattern_mode)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setDemandPointDemandPatternMode"), uuid);
    if (!this->network_editor.setDemandPointDemandPatternMode(uuid, demand_index, pattern_mode))
        return false;

    markNetworkChanged(NetworkChange::Visual, uuid);
    emit signalDemandPointChanged(uuid);
    return true;
}

bool HydraulicData::setDemandPointDemandPatternUuid(
    const QUuid &uuid, int demand_index, const QUuid &pattern_uuid)
{
    if (!hydraulicReferenceExists(this->network_hydraulic.patterns_time, pattern_uuid))
        return false;
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setDemandPointDemandPatternUuid"), uuid);
    if (!this->network_editor.setDemandPointDemandPatternUuid(uuid, demand_index, pattern_uuid))
        return false;

    markNetworkChanged(NetworkChange::Visual, uuid);
    emit signalDemandPointChanged(uuid);
    return true;
}

bool HydraulicData::setDemandPointDemandSourceMethod(
    const QUuid &uuid, int demand_index, HydraulicDemandSourceMethod source_method)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setDemandPointDemandSourceMethod"), uuid);
    if (!this->network_editor.setDemandPointDemandSourceMethod(uuid, demand_index, source_method))
        return false;

    markNetworkChanged(NetworkChange::Visual, uuid);
    emit signalDemandPointChanged(uuid);
    return true;
}

bool HydraulicData::setDemandPointDemandNote(
    const QUuid &uuid, int demand_index, const QString &note)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setDemandPointDemandNote"), uuid);
    if (!this->network_editor.setDemandPointDemandNote(uuid, demand_index, note))
        return false;

    markNetworkChanged(NetworkChange::Visual, uuid);
    emit signalDemandPointChanged(uuid);
    return true;
}

bool HydraulicData::addDemandPointMeter(const QUuid &uuid)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.addDemandPointMeter"), uuid);
    if (!this->network_editor.addDemandPointMeter(uuid))
        return false;

    markNetworkChanged(NetworkChange::Visual, uuid);
    emit signalDemandPointChanged(uuid);
    return true;
}

bool HydraulicData::setDemandPointMeter(const QUuid &uuid, const WaterMeter &meter)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setDemandPointMeter"), uuid);
    if (!this->network_editor.setDemandPointMeter(uuid, meter))
        return false;

    markNetworkChanged(NetworkChange::Visual, uuid);
    emit signalDemandPointChanged(uuid);
    return true;
}

bool HydraulicData::removeDemandPointMeter(const QUuid &uuid)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.removeDemandPointMeter"), uuid);
    if (!this->network_editor.removeDemandPointMeter(uuid))
        return false;

    markNetworkChanged(NetworkChange::Visual, uuid);
    emit signalDemandPointChanged(uuid);
    return true;
}

bool HydraulicData::attachDemandPointToPipe(
    const QUuid &uuid, const QUuid &pipe_uuid, double pipe_position)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.attachDemandPointToPipe"), uuid);
    if (!this->network_editor.attachDemandPointToPipe(uuid, pipe_uuid, pipe_position))
        return false;

    markNetworkChanged(NetworkChange::Geometry, uuid);
    emit signalDemandPointChanged(uuid);
    return true;
}

bool HydraulicData::setDemandPointPipeAllocationMode(
    const QUuid &uuid, HydraulicDemandPointPipeAllocationMode allocation_mode)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setDemandPointPipeAllocationMode"), uuid);
    if (!this->network_editor.setDemandPointPipeAllocationMode(uuid, allocation_mode))
        return false;

    markNetworkChanged(NetworkChange::Visual, uuid);
    emit signalDemandPointChanged(uuid);
    return true;
}

bool HydraulicData::setDemandPointPipeAssignedJunction(
    const QUuid &uuid, const QUuid &junction_uuid)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setDemandPointPipeAssignedJunction"), uuid);
    if (!this->network_editor.setDemandPointPipeAssignedJunction(uuid, junction_uuid))
        return false;

    markNetworkChanged(NetworkChange::Visual, uuid);
    emit signalDemandPointChanged(uuid);
    return true;
}

bool HydraulicData::attachDemandPointToJunction(
    const QUuid &uuid, const QUuid &junction_uuid)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.attachDemandPointToJunction"), uuid);
    if (!this->network_editor.attachDemandPointToJunction(uuid, junction_uuid))
        return false;

    markNetworkChanged(NetworkChange::Geometry, uuid);
    emit signalDemandPointChanged(uuid);
    return true;
}

bool HydraulicData::clearDemandPointAttachment(const QUuid &uuid)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.clearDemandPointAttachment"), uuid);
    if (!this->network_editor.clearDemandPointAttachment(uuid))
        return false;

    markNetworkChanged(NetworkChange::Geometry, uuid);
    emit signalDemandPointChanged(uuid);
    return true;
}

bool HydraulicData::setJunctionElevationInputType(
    const QUuid &uuid, HydraulicNodeElevationInputType input_type)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setJunctionElevationInputType"), uuid);
    return emitNodeChangedIfSuccessful(
        uuid, this->network_editor.setJunctionElevationInputType(uuid, input_type),
        NetworkChange::Geometry);
}

bool HydraulicData::setJunctionElevationM(const QUuid &uuid, double elevation_m)
{
    HydraulicEditTransactionCapture capture(QStringLiteral("hydraulic.junction.elevation"));
    if (this->logical_edit_capture_enabled)
        capture.watch(this->network_hydraulic, uuid);
    const bool successful = emitNodeChangedIfSuccessful(
        uuid, this->network_editor.setJunctionElevationM(uuid, elevation_m),
        NetworkChange::Geometry);
    if (successful && this->logical_edit_capture_enabled)
        publishCapturedEdit(capture);
    return successful;
}

bool HydraulicData::setJunctionTerrainElevationM(const QUuid &uuid, double terrain_elevation_m)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setJunctionTerrainElevationM"), uuid);
    return emitNodeChangedIfSuccessful(
        uuid, this->network_editor.setJunctionTerrainElevationM(uuid, terrain_elevation_m),
        NetworkChange::Geometry);
}

bool HydraulicData::setJunctionElevationOffsetM(const QUuid &uuid, double elevation_offset_m)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setJunctionElevationOffsetM"), uuid);
    return emitNodeChangedIfSuccessful(
        uuid, this->network_editor.setJunctionElevationOffsetM(uuid, elevation_offset_m),
        NetworkChange::Geometry);
}

bool HydraulicData::addJunctionDemand(const QUuid &uuid,
                                      const HydraulicDemand &demand)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.addJunctionDemand"), uuid);
    return emitNodeChangedIfSuccessful(
        uuid, this->network_editor.addJunctionDemand(uuid, demand));
}

bool HydraulicData::removeJunctionDemand(const QUuid &uuid, int demand_index)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.removeJunctionDemand"), uuid);
    return emitNodeChangedIfSuccessful(
        uuid, this->network_editor.removeJunctionDemand(uuid, demand_index));
}

bool HydraulicData::setJunctionDemandCategoryName(const QUuid &uuid, int demand_index,
                                                  const QString &category_name)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setJunctionDemandCategoryName"), uuid);
    return emitNodeChangedIfSuccessful(
        uuid, this->network_editor.setJunctionDemandCategoryName(
                  uuid, demand_index, category_name));
}

bool HydraulicData::setJunctionDemandBaseDemandM3PerH(
    const QUuid &uuid, int demand_index, double base_demand_m3_per_h)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setJunctionDemandBaseDemandM3PerH"), uuid);
    return emitNodeChangedIfSuccessful(
        uuid, this->network_editor.setJunctionDemandBaseDemandM3PerH(
                  uuid, demand_index, base_demand_m3_per_h));
}

bool HydraulicData::setJunctionDemandPatternMode(
    const QUuid &uuid, int demand_index, HydraulicTimePatternMode pattern_mode)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setJunctionDemandPatternMode"), uuid);
    return emitNodeChangedIfSuccessful(
        uuid, this->network_editor.setJunctionDemandPatternMode(
                  uuid, demand_index, pattern_mode));
}

bool HydraulicData::setJunctionDemandPatternUuid(const QUuid &uuid, int demand_index,
                                                 const QUuid &pattern_uuid)
{
    if (!hydraulicReferenceExists(this->network_hydraulic.patterns_time, pattern_uuid))
        return false;
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setJunctionDemandPatternUuid"), uuid);
    return emitNodeChangedIfSuccessful(
        uuid, this->network_editor.setJunctionDemandPatternUuid(
                  uuid, demand_index, pattern_uuid));
}

bool HydraulicData::setJunctionDemandSourceMethod(
    const QUuid &uuid, int demand_index, HydraulicDemandSourceMethod source_method)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setJunctionDemandSourceMethod"), uuid);
    return emitNodeChangedIfSuccessful(
        uuid, this->network_editor.setJunctionDemandSourceMethod(
                  uuid, demand_index, source_method));
}

bool HydraulicData::setJunctionDemandNote(const QUuid &uuid, int demand_index,
                                          const QString &note)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setJunctionDemandNote"), uuid);
    return emitNodeChangedIfSuccessful(
        uuid, this->network_editor.setJunctionDemandNote(uuid, demand_index, note));
}

bool HydraulicData::setJunctionEmitterCoefficient(const QUuid &uuid, double coefficient)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setJunctionEmitterCoefficient"), uuid);
    return emitNodeChangedIfSuccessful(
        uuid, this->network_editor.setJunctionEmitterCoefficient(uuid, coefficient));
}

bool HydraulicData::setJunctionEmitterPressureExponent(const QUuid &uuid, double pressure_exponent)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setJunctionEmitterPressureExponent"), uuid);
    return emitNodeChangedIfSuccessful(
        uuid, this->network_editor.setJunctionEmitterPressureExponent(uuid, pressure_exponent));
}

bool HydraulicData::setNodeInitialChemicalConcentrationMgPerL(const QUuid &uuid, double value_mg_per_l)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setNodeInitialChemicalConcentrationMgPerL"), uuid);
    return emitNodeChangedIfSuccessful(uuid, this->network_editor.setNodeInitialChemicalConcentrationMgPerL(uuid, value_mg_per_l));
}

bool HydraulicData::setNodeInitialWaterAgeH(const QUuid &uuid, double value_h)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setNodeInitialWaterAgeH"), uuid);
    return emitNodeChangedIfSuccessful(uuid, this->network_editor.setNodeInitialWaterAgeH(uuid, value_h));
}

bool HydraulicData::setNodeQualitySourceType(const QUuid &uuid, HydraulicNodeQualitySourceType source_type)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setNodeQualitySourceType"), uuid);
    return emitNodeChangedIfSuccessful(uuid, this->network_editor.setNodeQualitySourceType(uuid, source_type));
}

bool HydraulicData::setNodeQualitySourceChemicalConcentrationMgPerL(const QUuid &uuid, double value_mg_per_l)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setNodeQualitySourceChemicalConcentrationMgPerL"), uuid);
    return emitNodeChangedIfSuccessful(uuid, this->network_editor.setNodeQualitySourceChemicalConcentrationMgPerL(uuid, value_mg_per_l));
}

bool HydraulicData::setNodeQualitySourceMassFlowMgPerMin(const QUuid &uuid, double value_mg_per_min)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setNodeQualitySourceMassFlowMgPerMin"), uuid);
    return emitNodeChangedIfSuccessful(uuid, this->network_editor.setNodeQualitySourceMassFlowMgPerMin(uuid, value_mg_per_min));
}

bool HydraulicData::setNodeQualitySourcePatternUuid(const QUuid &uuid, const QUuid &pattern_uuid)
{
    if (!hydraulicReferenceExists(this->network_hydraulic.patterns_time, pattern_uuid))
        return false;
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setNodeQualitySourcePatternUuid"), uuid);
    return emitNodeChangedIfSuccessful(uuid, this->network_editor.setNodeQualitySourcePatternUuid(uuid, pattern_uuid));
}

bool HydraulicData::setReservoirHeadInputType(
    const QUuid &uuid, HydraulicNodeElevationInputType input_type)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setReservoirHeadInputType"), uuid);
    return emitNodeChangedIfSuccessful(
        uuid, this->network_editor.setReservoirHeadInputType(uuid, input_type),
        NetworkChange::Geometry);
}

bool HydraulicData::setReservoirHeadM(const QUuid &uuid, double hydraulic_head_m)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setReservoirHeadM"), uuid);
    return emitNodeChangedIfSuccessful(
        uuid, this->network_editor.setReservoirHeadM(uuid, hydraulic_head_m),
        NetworkChange::Geometry);
}

bool HydraulicData::setReservoirTerrainElevationM(const QUuid &uuid,
                                                  double terrain_elevation_m)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setReservoirTerrainElevationM"), uuid);
    return emitNodeChangedIfSuccessful(
        uuid, this->network_editor.setReservoirTerrainElevationM(
                  uuid, terrain_elevation_m),
        NetworkChange::Geometry);
}

bool HydraulicData::setReservoirHeadOffsetM(const QUuid &uuid, double hydraulic_head_offset_m)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setReservoirHeadOffsetM"), uuid);
    return emitNodeChangedIfSuccessful(
        uuid, this->network_editor.setReservoirHeadOffsetM(uuid, hydraulic_head_offset_m),
        NetworkChange::Geometry);
}

bool HydraulicData::setReservoirHeadPatternMode(const QUuid &uuid, HydraulicTimePatternMode pattern_mode)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setReservoirHeadPatternMode"), uuid);
    return emitNodeChangedIfSuccessful(
        uuid, this->network_editor.setReservoirHeadPatternMode(uuid, pattern_mode));
}

bool HydraulicData::setReservoirHeadPatternUuid(const QUuid &uuid, const QUuid &pattern_uuid)
{
    if (!hydraulicReferenceExists(this->network_hydraulic.patterns_time, pattern_uuid))
        return false;
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setReservoirHeadPatternUuid"), uuid);
    return emitNodeChangedIfSuccessful(
        uuid, this->network_editor.setReservoirHeadPatternUuid(uuid, pattern_uuid));
}

bool HydraulicData::setTankElevationInputType(
    const QUuid &uuid, HydraulicNodeTankElevationInputType input_type)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setTankElevationInputType"), uuid);
    return emitNodeChangedIfSuccessful(
        uuid, this->network_editor.setTankElevationInputType(uuid, input_type),
        NetworkChange::Geometry);
}

bool HydraulicData::setTankBottomElevationM(const QUuid &uuid, double bottom_elevation_m)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setTankBottomElevationM"), uuid);
    return emitNodeChangedIfSuccessful(
        uuid, this->network_editor.setTankBottomElevationM(uuid, bottom_elevation_m),
        NetworkChange::Geometry);
}

bool HydraulicData::setTankTerrainElevationM(const QUuid &uuid, double terrain_elevation_m)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setTankTerrainElevationM"), uuid);
    return emitNodeChangedIfSuccessful(
        uuid, this->network_editor.setTankTerrainElevationM(uuid, terrain_elevation_m),
        NetworkChange::Geometry);
}

bool HydraulicData::setTankBottomOffsetM(const QUuid &uuid, double bottom_offset_m)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setTankBottomOffsetM"), uuid);
    return emitNodeChangedIfSuccessful(
        uuid, this->network_editor.setTankBottomOffsetM(uuid, bottom_offset_m),
        NetworkChange::Geometry);
}

bool HydraulicData::setTankWaterLevelInitialM(const QUuid &uuid, double water_level_initial_m)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setTankWaterLevelInitialM"), uuid);
    return emitNodeChangedIfSuccessful(
        uuid, this->network_editor.setTankWaterLevelInitialM(uuid, water_level_initial_m));
}

bool HydraulicData::setTankWaterLevelMinimumM(const QUuid &uuid, double water_level_minimum_m)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setTankWaterLevelMinimumM"), uuid);
    return emitNodeChangedIfSuccessful(
        uuid, this->network_editor.setTankWaterLevelMinimumM(uuid, water_level_minimum_m));
}

bool HydraulicData::setTankWaterLevelMaximumM(const QUuid &uuid, double water_level_maximum_m)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setTankWaterLevelMaximumM"), uuid);
    return emitNodeChangedIfSuccessful(
        uuid, this->network_editor.setTankWaterLevelMaximumM(uuid, water_level_maximum_m));
}

bool HydraulicData::setTankGeometryInputType(
    const QUuid &uuid, HydraulicNodeTankGeometryInputType input_type)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setTankGeometryInputType"), uuid);
    return emitNodeChangedIfSuccessful(
        uuid, this->network_editor.setTankGeometryInputType(uuid, input_type));
}

bool HydraulicData::setTankDiameterM(const QUuid &uuid, double diameter_m)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setTankDiameterM"), uuid);
    return emitNodeChangedIfSuccessful(
        uuid, this->network_editor.setTankDiameterM(uuid, diameter_m));
}

bool HydraulicData::setTankCrossSectionAreaM2(const QUuid &uuid, double cross_section_area_m2)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setTankCrossSectionAreaM2"), uuid);
    return emitNodeChangedIfSuccessful(
        uuid, this->network_editor.setTankCrossSectionAreaM2(uuid, cross_section_area_m2));
}

bool HydraulicData::setTankVolumeAtMaximumLevelM3(
    const QUuid &uuid, double volume_at_maximum_level_m3)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setTankVolumeAtMaximumLevelM3"), uuid);
    return emitNodeChangedIfSuccessful(
        uuid, this->network_editor.setTankVolumeAtMaximumLevelM3(
                  uuid, volume_at_maximum_level_m3));
}

bool HydraulicData::setTankMinimumVolumeM3(const QUuid &uuid, double minimum_volume_m3)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setTankMinimumVolumeM3"), uuid);
    return emitNodeChangedIfSuccessful(
        uuid, this->network_editor.setTankMinimumVolumeM3(uuid, minimum_volume_m3));
}

bool HydraulicData::setTankVolumeCurveUuid(const QUuid &uuid, const QUuid &volume_curve_uuid)
{
    if (!hydraulicReferenceExists(this->network_hydraulic.curves_tank_volume, volume_curve_uuid))
        return false;
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setTankVolumeCurveUuid"), uuid);
    return emitNodeChangedIfSuccessful(
        uuid, this->network_editor.setTankVolumeCurveUuid(uuid, volume_curve_uuid));
}

bool HydraulicData::setTankCanOverflow(const QUuid &uuid, bool can_overflow)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setTankCanOverflow"), uuid);
    return emitNodeChangedIfSuccessful(
        uuid, this->network_editor.setTankCanOverflow(uuid, can_overflow));
}

bool HydraulicData::setTankQualityMixingModel(const QUuid &uuid, HydraulicNodeTankMixingModel mixing_model)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setTankQualityMixingModel"), uuid);
    return emitNodeChangedIfSuccessful(uuid, this->network_editor.setTankQualityMixingModel(uuid, mixing_model));
}

bool HydraulicData::setTankQualityMixingFraction(const QUuid &uuid, double mixing_fraction)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setTankQualityMixingFraction"), uuid);
    return emitNodeChangedIfSuccessful(uuid, this->network_editor.setTankQualityMixingFraction(uuid, mixing_fraction));
}

bool HydraulicData::setTankOverrideBulkReaction(const QUuid &uuid, bool override_bulk_reaction)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setTankOverrideBulkReaction"), uuid);
    return emitNodeChangedIfSuccessful(uuid, this->network_editor.setTankOverrideBulkReaction(uuid, override_bulk_reaction));
}

bool HydraulicData::setTankBulkReactionCoefficient(const QUuid &uuid, double coefficient)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setTankBulkReactionCoefficient"), uuid);
    return emitNodeChangedIfSuccessful(uuid, this->network_editor.setTankBulkReactionCoefficient(uuid, coefficient));
}

bool HydraulicData::setPipeInitialStatus(
    const QUuid &uuid, HydraulicLinkPipeInitialStatus initial_status)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setPipeInitialStatus"), uuid);
    return emitLinkChangedIfSuccessful(
        uuid, this->network_editor.setPipeInitialStatus(uuid, initial_status));
}

bool HydraulicData::setPipeDiameterMm(const QUuid &uuid, double diameter_mm)
{
    HydraulicEditTransactionCapture capture(QStringLiteral("hydraulic.pipe.diameter"));
    if (this->logical_edit_capture_enabled)
        capture.watch(this->network_hydraulic, uuid);
    const bool successful = emitLinkChangedIfSuccessful(
        uuid, this->network_editor.setPipeDiameterMm(uuid, diameter_mm));
    if (successful && this->logical_edit_capture_enabled)
        publishCapturedEdit(capture);
    return successful;
}

bool HydraulicData::setPipeMeasuredLengthM(
    const QUuid &uuid, const std::optional<double> &length_measured_m)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setPipeMeasuredLengthM"), uuid);
    return emitLinkChangedIfSuccessful(
        uuid, this->network_editor.setPipeMeasuredLengthM(uuid, length_measured_m));
}

bool HydraulicData::setPipeMaterialUuid(const QUuid &uuid, const QUuid &material_uuid)
{
    if (!hydraulicReferenceExists(this->network_hydraulic.pipe_materials, material_uuid))
        return false;
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setPipeMaterialUuid"), uuid);
    return emitLinkChangedIfSuccessful(
        uuid, this->network_editor.setPipeMaterialUuid(uuid, material_uuid));
}

bool HydraulicData::setPipeRoughnessMode(
    const QUuid &uuid, HydraulicPipeRoughnessMode roughness_mode)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setPipeRoughnessMode"), uuid);
    return emitLinkChangedIfSuccessful(
        uuid, this->network_editor.setPipeRoughnessMode(uuid, roughness_mode));
}

bool HydraulicData::setPipeRoughnessHw(const QUuid &uuid, double roughness_hazen_williams)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setPipeRoughnessHw"), uuid);
    return emitLinkChangedIfSuccessful(
        uuid, this->network_editor.setPipeRoughnessHw(uuid, roughness_hazen_williams));
}

bool HydraulicData::setPipeRoughnessDwMm(const QUuid &uuid, double roughness_darcy_weisbach_mm)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setPipeRoughnessDwMm"), uuid);
    return emitLinkChangedIfSuccessful(
        uuid, this->network_editor.setPipeRoughnessDwMm(uuid, roughness_darcy_weisbach_mm));
}

bool HydraulicData::setPipeRoughnessCm(const QUuid &uuid, double roughness_chezy_manning)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setPipeRoughnessCm"), uuid);
    return emitLinkChangedIfSuccessful(
        uuid, this->network_editor.setPipeRoughnessCm(uuid, roughness_chezy_manning));
}

bool HydraulicData::setPipeMinorLoss(const QUuid &uuid, double minor_loss_coefficient)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setPipeMinorLoss"), uuid);
    return emitLinkChangedIfSuccessful(
        uuid, this->network_editor.setPipeMinorLoss(uuid, minor_loss_coefficient));
}

bool HydraulicData::setPipeOverrideBulkReaction(
    const QUuid &uuid, bool override_bulk_reaction)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setPipeOverrideBulkReaction"), uuid);
    return emitLinkChangedIfSuccessful(
        uuid, this->network_editor.setPipeOverrideBulkReaction(uuid, override_bulk_reaction));
}

bool HydraulicData::setPipeOverrideWallReaction(
    const QUuid &uuid, bool override_wall_reaction)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setPipeOverrideWallReaction"), uuid);
    return emitLinkChangedIfSuccessful(
        uuid, this->network_editor.setPipeOverrideWallReaction(uuid, override_wall_reaction));
}

bool HydraulicData::setPipeBulkReactionCoefficient(const QUuid &uuid, double coefficient)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setPipeBulkReactionCoefficient"), uuid);
    return emitLinkChangedIfSuccessful(uuid, this->network_editor.setPipeBulkReactionCoefficient(uuid, coefficient));
}

bool HydraulicData::setPipeWallReactionCoefficient(const QUuid &uuid, double coefficient)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setPipeWallReactionCoefficient"), uuid);
    return emitLinkChangedIfSuccessful(uuid, this->network_editor.setPipeWallReactionCoefficient(uuid, coefficient));
}

bool HydraulicData::setPumpDefinitionType(
    const QUuid &uuid, HydraulicLinkPumpDefinitionType definition_type)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setPumpDefinitionType"), uuid);
    return emitLinkChangedIfSuccessful(
        uuid, this->network_editor.setPumpDefinitionType(uuid, definition_type));
}

bool HydraulicData::setPumpConstantPowerKw(const QUuid &uuid, double constant_power_kw)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setPumpConstantPowerKw"), uuid);
    return emitLinkChangedIfSuccessful(
        uuid, this->network_editor.setPumpConstantPowerKw(uuid, constant_power_kw));
}

bool HydraulicData::setPumpInitialSpeed(const QUuid &uuid, double initial_speed_ratio)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setPumpInitialSpeed"), uuid);
    return emitLinkChangedIfSuccessful(
        uuid, this->network_editor.setPumpInitialSpeed(uuid, initial_speed_ratio));
}

bool HydraulicData::setPumpInitialStatus(
    const QUuid &uuid, HydraulicLinkPumpInitialStatus initial_status)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setPumpInitialStatus"), uuid);
    return emitLinkChangedIfSuccessful(
        uuid, this->network_editor.setPumpInitialStatus(uuid, initial_status));
}

bool HydraulicData::setPumpSpeedPatternUuid(
    const QUuid &uuid, const QUuid &speed_pattern_uuid)
{
    if (!hydraulicReferenceExists(this->network_hydraulic.patterns_time, speed_pattern_uuid))
        return false;
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setPumpSpeedPatternUuid"), uuid);
    return emitLinkChangedIfSuccessful(
        uuid, this->network_editor.setPumpSpeedPatternUuid(uuid, speed_pattern_uuid));
}

QUuid HydraulicData::addPumpSimpleControl(
    const QUuid &pump_uuid, HydraulicControlSimpleType type, const QUuid &trigger_node_uuid)
{
    HydraulicEditTransactionCapture capture(QStringLiteral("hydraulic.addPumpSimpleControl"));
    const QUuid control_uuid = this->network_editor.addPumpSimpleControl(
        pump_uuid, type, trigger_node_uuid);
    if (control_uuid.isNull())
        return QUuid();

    markNetworkChanged(NetworkChange::Visual, pump_uuid);
    emit signalLinkChanged(InfrastructureEntity::Pump, pump_uuid);
    if (this->logical_edit_capture_enabled)
    {
        capture.watchCreated(control_uuid);
        publishCapturedEdit(capture);
    }
    return control_uuid;
}

bool HydraulicData::setPumpSimpleControl(
    const QUuid &pump_uuid, const HydraulicControlSimple &control)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setPumpSimpleControl"), control.uuid);
    return emitLinkChangedIfSuccessful(
        pump_uuid, this->network_editor.setPumpSimpleControl(pump_uuid, control));
}

bool HydraulicData::removePumpSimpleControl(
    const QUuid &pump_uuid, const QUuid &control_uuid)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.removePumpSimpleControl"), control_uuid);
    return emitLinkChangedIfSuccessful(
        pump_uuid, this->network_editor.removePumpSimpleControl(pump_uuid, control_uuid));
}

bool HydraulicData::setPumpEfficiencyInput(
    const QUuid &uuid, HydraulicLinkPumpEfficiencyInputType input_type,
    const QUuid &efficiency_curve_uuid)
{
    if (!hydraulicReferenceExists(this->network_hydraulic.curves_pump_efficiency, efficiency_curve_uuid))
        return false;
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setPumpEfficiencyInput"), uuid);
    return emitLinkChangedIfSuccessful(
        uuid, this->network_editor.setPumpEfficiencyInput(
                  uuid, input_type, efficiency_curve_uuid));
}

bool HydraulicData::setPumpEnergyPricePerKwh(
    const QUuid &uuid, double energy_price_per_kw_h)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setPumpEnergyPricePerKwh"), uuid);
    return emitLinkChangedIfSuccessful(
        uuid, this->network_editor.setPumpEnergyPricePerKwh(
                  uuid, energy_price_per_kw_h));
}

bool HydraulicData::setPumpEnergyPriceInput(
    const QUuid &uuid, HydraulicLinkPumpEnergyPriceInputType input_type,
    const QUuid &price_pattern_uuid)
{
    if (!hydraulicReferenceExists(this->network_hydraulic.patterns_time, price_pattern_uuid))
        return false;
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setPumpEnergyPriceInput"), uuid);
    return emitLinkChangedIfSuccessful(
        uuid, this->network_editor.setPumpEnergyPriceInput(
                  uuid, input_type, price_pattern_uuid));
}

bool HydraulicData::setValveType(const QUuid &uuid, HydraulicLinkValveType type)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setValveType"), uuid);
    return emitLinkChangedIfSuccessful(
        uuid, this->network_editor.setValveType(uuid, type));
}

bool HydraulicData::setValveSettingPressureHeadM(
    const QUuid &uuid, double setting_pressure_head_m)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setValveSettingPressureHeadM"), uuid);
    return emitLinkChangedIfSuccessful(
        uuid, this->network_editor.setValveSettingPressureHeadM(uuid, setting_pressure_head_m));
}

bool HydraulicData::setValveSettingFlowM3PerH(
    const QUuid &uuid, double setting_flow_m3_per_h)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setValveSettingFlowM3PerH"), uuid);
    return emitLinkChangedIfSuccessful(
        uuid, this->network_editor.setValveSettingFlowM3PerH(uuid, setting_flow_m3_per_h));
}

bool HydraulicData::setValveSettingLossCoefficient(
    const QUuid &uuid, double setting_loss_coefficient)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setValveSettingLossCoefficient"), uuid);
    return emitLinkChangedIfSuccessful(
        uuid, this->network_editor.setValveSettingLossCoefficient(uuid, setting_loss_coefficient));
}

bool HydraulicData::setValveSettingPositionPercent(
    const QUuid &uuid, double setting_position_percent)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setValveSettingPositionPercent"), uuid);
    return emitLinkChangedIfSuccessful(
        uuid, this->network_editor.setValveSettingPositionPercent(uuid, setting_position_percent));
}

bool HydraulicData::setValveHeadLossCurveUuid(
    const QUuid &uuid, const QUuid &head_loss_curve_uuid)
{
    if (!hydraulicReferenceExists(this->network_hydraulic.curves_valve_headloss, head_loss_curve_uuid))
        return false;
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setValveHeadLossCurveUuid"), uuid);
    return emitLinkChangedIfSuccessful(
        uuid, this->network_editor.setValveHeadLossCurveUuid(uuid, head_loss_curve_uuid));
}

bool HydraulicData::setValveCharacteristicCurveUuid(
    const QUuid &uuid, const QUuid &characteristic_curve_uuid)
{
    if (!hydraulicReferenceExists(this->network_hydraulic.curves_valve_characteristic, characteristic_curve_uuid))
        return false;
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setValveCharacteristicCurveUuid"), uuid);
    return emitLinkChangedIfSuccessful(
        uuid, this->network_editor.setValveCharacteristicCurveUuid(uuid, characteristic_curve_uuid));
}

bool HydraulicData::setValveInitialStatus(
    const QUuid &uuid, HydraulicLinkValveInitialStatus initial_status)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setValveInitialStatus"), uuid);
    return emitLinkChangedIfSuccessful(
        uuid, this->network_editor.setValveInitialStatus(uuid, initial_status));
}

bool HydraulicData::setValveDiameterMm(const QUuid &uuid, double diameter_mm)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setValveDiameterMm"), uuid);
    return emitLinkChangedIfSuccessful(
        uuid, this->network_editor.setValveDiameterMm(uuid, diameter_mm));
}

bool HydraulicData::setValveMinorLoss(const QUuid &uuid, double minor_loss_coefficient)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setValveMinorLoss"), uuid);
    return emitLinkChangedIfSuccessful(
        uuid, this->network_editor.setValveMinorLoss(uuid, minor_loss_coefficient));
}

bool HydraulicData::setPipeVertexCoordinate(const QUuid &pipe_uuid, int vertex_index,
                                            const CoordinateWGS84 &coordinate)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setPipeVertexCoordinate"), pipe_uuid);
    const std::optional<HydraulicLinkPipe> pipe = this->network_editor.pipe(pipe_uuid);
    if (!pipe.has_value() || vertex_index < 0 || vertex_index >= pipe->vertices.size())
        return false;

    const CoordinateWGS84 coordinate_previous = pipe->vertices.at(vertex_index).coordinate_wgs84;
    if (!this->network_editor.setPipeVertexCoordinate(pipe_uuid, vertex_index, coordinate))
        return false;

    updateBoundingBoxWgs84(coordinate_previous, coordinate);
    return emitLinkChangedIfSuccessful(pipe_uuid, true, NetworkChange::Geometry);
}

bool HydraulicData::setPipeVertices(const QUuid &pipe_uuid,
                                    const QList<CoordinateWGS84> &intermediate_vertices)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setPipeVertices"), pipe_uuid);
    if (!this->network_editor.setPipeVertices(pipe_uuid, intermediate_vertices))
        return false;

    rebuildBoundingBoxWgs84();
    return emitLinkChangedIfSuccessful(pipe_uuid, true, NetworkChange::Geometry);
}

bool HydraulicData::setPumpCenterCoordinate(const QUuid &pump_uuid,
                                            const CoordinateWGS84 &coordinate)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setPumpCenterCoordinate"), pump_uuid);
    const std::optional<HydraulicLinkPump> pump = this->network_editor.pump(pump_uuid);
    if (!pump.has_value())
        return false;

    std::optional<CoordinateWGS84> coordinate_previous;
    if (!pump->vertices.isEmpty())
        coordinate_previous = pump->vertices.first().coordinate_wgs84;
    if (!this->network_editor.setPumpCenterCoordinate(pump_uuid, coordinate))
        return false;

    if (coordinate_previous.has_value())
        updateBoundingBoxWgs84(coordinate_previous.value(), coordinate);
    else if (extendBoundingBoxWgs84(coordinate))
        emit signalBoundingBoxWgs84Changed();

    return emitLinkChangedIfSuccessful(pump_uuid, true, NetworkChange::Geometry);
}

bool HydraulicData::setValveCenterCoordinate(const QUuid &valve_uuid,
                                             const CoordinateWGS84 &coordinate)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.setValveCenterCoordinate"), valve_uuid);
    const std::optional<HydraulicLinkValve> valve = this->network_editor.valve(valve_uuid);
    if (!valve.has_value())
        return false;

    std::optional<CoordinateWGS84> coordinate_previous;
    if (!valve->vertices.isEmpty())
        coordinate_previous = valve->vertices.first().coordinate_wgs84;
    if (!this->network_editor.setValveCenterCoordinate(valve_uuid, coordinate))
        return false;

    if (coordinate_previous.has_value())
        updateBoundingBoxWgs84(coordinate_previous.value(), coordinate);
    else if (extendBoundingBoxWgs84(coordinate))
        emit signalBoundingBoxWgs84Changed();

    return emitLinkChangedIfSuccessful(valve_uuid, true, NetworkChange::Geometry);
}

bool HydraulicData::applyGeometryBatch(const HydraulicGeometryBatch &batch)
{
    if (batch.isEmpty())
        return true;

    HydraulicEditTransactionCapture capture(QStringLiteral("hydraulic.geometry.batch"));
    if (this->logical_edit_capture_enabled)
    {
        for (QHash<QUuid, CoordinateWGS84>::const_iterator it = batch.node_coordinates.cbegin();
             it != batch.node_coordinates.cend(); ++it)
            capture.watch(this->network_hydraulic, it.key());
        for (QHash<QUuid, CoordinateWGS84>::const_iterator it = batch.demand_point_coordinates.cbegin();
             it != batch.demand_point_coordinates.cend(); ++it)
            capture.watch(this->network_hydraulic, it.key());
        for (QHash<QUuid, QList<CoordinateWGS84>>::const_iterator it = batch.pipe_vertices.cbegin();
             it != batch.pipe_vertices.cend(); ++it)
            capture.watch(this->network_hydraulic, it.key());
        for (QHash<QUuid, CoordinateWGS84>::const_iterator it = batch.pump_center_coordinates.cbegin();
             it != batch.pump_center_coordinates.cend(); ++it)
            capture.watch(this->network_hydraulic, it.key());
        for (QHash<QUuid, CoordinateWGS84>::const_iterator it = batch.valve_center_coordinates.cbegin();
             it != batch.valve_center_coordinates.cend(); ++it)
            capture.watch(this->network_hydraulic, it.key());
        for (const HydraulicLinkPipe &pipe : this->network_hydraulic.links_pipes)
        {
            if (batch.node_coordinates.contains(pipe.node_uuid_from) ||
                batch.node_coordinates.contains(pipe.node_uuid_to))
                capture.watch(this->network_hydraulic, pipe.uuid);
        }
    }

    const bool snapshot_was_current =
        this->network_render_snapshot.geometry_revision == this->geometry_revision;
    const HydraulicGeometryBatchResult result = this->network_editor.applyGeometryBatch(batch);
    if (!result.successful)
        return false;

    rebuildBoundingBoxWgs84();
    markNetworkChanged(NetworkChange::Geometry);
    for (QHash<QUuid, CoordinateWGS84>::const_iterator iterator = batch.node_coordinates.cbegin();
         iterator != batch.node_coordinates.cend(); ++iterator)
    {
        markSimulationDiagnosticEntityStale(iterator.key());
    }
    for (QHash<QUuid, CoordinateWGS84>::const_iterator iterator =
             batch.demand_point_coordinates.cbegin();
         iterator != batch.demand_point_coordinates.cend(); ++iterator)
    {
        markSimulationDiagnosticEntityStale(iterator.key());
    }
    for (QHash<QUuid, QList<CoordinateWGS84>>::const_iterator iterator = batch.pipe_vertices.cbegin();
         iterator != batch.pipe_vertices.cend(); ++iterator)
    {
        markSimulationDiagnosticEntityStale(iterator.key());
    }
    for (QHash<QUuid, CoordinateWGS84>::const_iterator iterator = batch.pump_center_coordinates.cbegin();
         iterator != batch.pump_center_coordinates.cend(); ++iterator)
    {
        markSimulationDiagnosticEntityStale(iterator.key());
    }
    for (QHash<QUuid, CoordinateWGS84>::const_iterator iterator = batch.valve_center_coordinates.cbegin();
         iterator != batch.valve_center_coordinates.cend(); ++iterator)
    {
        markSimulationDiagnosticEntityStale(iterator.key());
    }

    if (snapshot_was_current)
    {
        QSet<quint32> moved_node_render_ids;
        for (NetworkRenderNode &node : this->network_render_snapshot.nodes)
        {
            const QHash<QUuid, CoordinateWGS84>::const_iterator iterator =
                batch.node_coordinates.constFind(node.uuid);
            if (iterator == batch.node_coordinates.cend())
                continue;
            node.coordinate_wgs84 = iterator.value();
            moved_node_render_ids.insert(node.render_id);
        }

        QHash<QUuid, const HydraulicLinkPipe *> pipes_by_uuid;
        pipes_by_uuid.reserve(result.affected_pipe_uuids.size());
        for (const HydraulicLinkPipe &pipe : this->network_hydraulic.links_pipes)
        {
            if (result.affected_pipe_uuids.contains(pipe.uuid))
                pipes_by_uuid.insert(pipe.uuid, &pipe);
        }

        QHash<QUuid, const HydraulicLinkPump *> pumps_by_uuid;
        if (!batch.pump_center_coordinates.isEmpty() || !moved_node_render_ids.isEmpty())
        {
            pumps_by_uuid.reserve(this->network_hydraulic.links_pumps.size());
            for (const HydraulicLinkPump &pump : this->network_hydraulic.links_pumps)
                pumps_by_uuid.insert(pump.uuid, &pump);
        }

        QHash<QUuid, const HydraulicLinkValve *> valves_by_uuid;
        if (!batch.valve_center_coordinates.isEmpty() || !moved_node_render_ids.isEmpty())
        {
            valves_by_uuid.reserve(this->network_hydraulic.links_valves.size());
            for (const HydraulicLinkValve &valve : this->network_hydraulic.links_valves)
                valves_by_uuid.insert(valve.uuid, &valve);
        }

        for (NetworkRenderLink &link : this->network_render_snapshot.links)
        {
            const bool endpoint_moved =
                moved_node_render_ids.contains(link.start_node_render_id) ||
                moved_node_render_ids.contains(link.end_node_render_id);
            const qsizetype start_index = qsizetype(link.start_node_render_id) - 1;
            const qsizetype end_index = qsizetype(link.end_node_render_id) - 1;
            if (start_index < 0 || end_index < 0 ||
                start_index >= this->network_render_snapshot.nodes.size() ||
                end_index >= this->network_render_snapshot.nodes.size())
            {
                continue;
            }

            if (link.entity_type == InfrastructureEntity::Pipe)
            {
                const QHash<QUuid, const HydraulicLinkPipe *>::const_iterator source_iterator =
                    pipes_by_uuid.constFind(link.uuid);
                if (source_iterator == pipes_by_uuid.cend())
                    continue;
                const HydraulicLinkPipe *source = source_iterator.value();
                link.vertices_wgs84.clear();
                link.vertices_wgs84.reserve(source->vertices.size() + 2);
                link.vertices_wgs84.append(
                    this->network_render_snapshot.nodes.at(start_index).coordinate_wgs84);
                for (const HydraulicLinkVertex &vertex : source->vertices)
                    link.vertices_wgs84.append(vertex.coordinate_wgs84);
                link.vertices_wgs84.append(
                    this->network_render_snapshot.nodes.at(end_index).coordinate_wgs84);
                continue;
            }

            if (link.entity_type == InfrastructureEntity::Pump &&
                (endpoint_moved || batch.pump_center_coordinates.contains(link.uuid)))
            {
                const QHash<QUuid, const HydraulicLinkPump *>::const_iterator source_iterator =
                    pumps_by_uuid.constFind(link.uuid);
                if (source_iterator == pumps_by_uuid.cend())
                    continue;
                const HydraulicLinkPump *source = source_iterator.value();
                link.vertices_wgs84.clear();
                link.vertices_wgs84.reserve(source->vertices.size() + 2);
                link.vertices_wgs84.append(
                    this->network_render_snapshot.nodes.at(start_index).coordinate_wgs84);
                for (const HydraulicLinkVertex &vertex : source->vertices)
                    link.vertices_wgs84.append(vertex.coordinate_wgs84);
                link.vertices_wgs84.append(
                    this->network_render_snapshot.nodes.at(end_index).coordinate_wgs84);
            }
            else if (link.entity_type == InfrastructureEntity::Valve &&
                     (endpoint_moved || batch.valve_center_coordinates.contains(link.uuid)))
            {
                const QHash<QUuid, const HydraulicLinkValve *>::const_iterator source_iterator =
                    valves_by_uuid.constFind(link.uuid);
                if (source_iterator == valves_by_uuid.cend())
                    continue;
                const HydraulicLinkValve *source = source_iterator.value();
                link.vertices_wgs84.clear();
                link.vertices_wgs84.reserve(source->vertices.size() + 2);
                link.vertices_wgs84.append(
                    this->network_render_snapshot.nodes.at(start_index).coordinate_wgs84);
                for (const HydraulicLinkVertex &vertex : source->vertices)
                    link.vertices_wgs84.append(vertex.coordinate_wgs84);
                link.vertices_wgs84.append(
                    this->network_render_snapshot.nodes.at(end_index).coordinate_wgs84);
            }
        }

        this->network_render_snapshot.geometry_revision = this->geometry_revision;
        this->network_render_snapshot.visual_revision = this->visual_revision;
    }

    for (QHash<QUuid, CoordinateWGS84>::const_iterator iterator = batch.node_coordinates.cbegin();
         iterator != batch.node_coordinates.cend(); ++iterator)
    {
        const std::optional<InfrastructureEntity> entity_type = nodeEntityType(iterator.key());
        if (entity_type.has_value())
            emit signalNodeChanged(entity_type.value(), iterator.key());
    }
    for (QHash<QUuid, CoordinateWGS84>::const_iterator iterator =
             batch.demand_point_coordinates.cbegin();
         iterator != batch.demand_point_coordinates.cend(); ++iterator)
        emit signalDemandPointChanged(iterator.key());
    for (QHash<QUuid, CoordinateWGS84>::const_iterator iterator = batch.pump_center_coordinates.cbegin();
         iterator != batch.pump_center_coordinates.cend(); ++iterator)
        emit signalLinkChanged(InfrastructureEntity::Pump, iterator.key());
    for (QHash<QUuid, CoordinateWGS84>::const_iterator iterator = batch.valve_center_coordinates.cbegin();
         iterator != batch.valve_center_coordinates.cend(); ++iterator)
        emit signalLinkChanged(InfrastructureEntity::Valve, iterator.key());
    for (const QUuid &pipe_uuid : result.affected_pipe_uuids)
        emit signalLinkChanged(InfrastructureEntity::Pipe, pipe_uuid);

    if (this->logical_edit_capture_enabled)
        publishCapturedEdit(capture);
    return true;
}

QUuid HydraulicData::splitPipeAtVertex(const QUuid &pipe_uuid, int vertex_index,
                                       const QUuid &junction_uuid)
{
    HydraulicEditTransactionCapture capture(QStringLiteral("hydraulic.pipe.split"));
    if (this->logical_edit_capture_enabled)
    {
        capture.watch(this->network_hydraulic, pipe_uuid);
        for (const HydraulicDemandPoint &point : this->network_hydraulic.demand_points)
        {
            if (point.attachment.type == HydraulicDemandPointAttachmentType::Pipe &&
                point.attachment.pipe_uuid == pipe_uuid)
                capture.watch(this->network_hydraulic, point.uuid);
        }
    }
    const QUuid second_pipe_uuid =
        this->network_editor.splitPipeAtVertex(pipe_uuid, vertex_index, junction_uuid);
    if (!second_pipe_uuid.isNull())
        markNetworkChanged(NetworkChange::Geometry, pipe_uuid);
    if (this->logical_edit_capture_enabled && !second_pipe_uuid.isNull())
    {
        capture.watchCreated(second_pipe_uuid);
        publishCapturedEdit(capture);
    }
    return second_pipe_uuid;
}

bool HydraulicData::undoPipeSplit(const QUuid &first_pipe_uuid, const QUuid &second_pipe_uuid,
                                  const QUuid &junction_uuid)
{
    HydraulicEditTransactionCapture capture(QStringLiteral("hydraulic.pipe.rejoin"));
    if (this->logical_edit_capture_enabled)
    {
        capture.watch(this->network_hydraulic, first_pipe_uuid);
        capture.watch(this->network_hydraulic, second_pipe_uuid);
        for (const HydraulicControlSimple &control : this->network_hydraulic.controls_simple)
        {
            if (control.link_uuid == second_pipe_uuid)
                capture.watch(this->network_hydraulic, control.uuid);
        }
        for (const HydraulicControlRule &rule : this->network_hydraulic.controls_rules)
        {
            if (hydraulic_rule_dependencies::referencesEntity(rule, second_pipe_uuid))
                capture.watch(this->network_hydraulic, rule.uuid);
        }
        for (const HydraulicDemandPoint &point : this->network_hydraulic.demand_points)
        {
            if ((point.attachment.type == HydraulicDemandPointAttachmentType::Pipe &&
                 (point.attachment.pipe_uuid == first_pipe_uuid ||
                  point.attachment.pipe_uuid == second_pipe_uuid)) ||
                (point.attachment.type == HydraulicDemandPointAttachmentType::Junction &&
                 point.attachment.junction_uuid == junction_uuid))
                capture.watch(this->network_hydraulic, point.uuid);
        }
    }
    const bool successful =
        this->network_editor.undoPipeSplit(first_pipe_uuid, second_pipe_uuid, junction_uuid);
    if (successful)
    {
        markNetworkChanged(NetworkChange::Geometry, first_pipe_uuid);
        markSimulationDiagnosticEntityStale(second_pipe_uuid);
        markSimulationDiagnosticEntityStale(junction_uuid);
    }
    if (successful && this->logical_edit_capture_enabled)
        publishCapturedEdit(capture);
    return successful;
}

bool HydraulicData::deleteJunction(const QUuid &uuid)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.deleteJunction"), uuid);
    edit_scope.watchNodeDeletionDependents(uuid);
    const bool was_source_trace_origin = this->source_trace_origin_node_uuid == uuid;
    const bool successful = this->network_editor.deleteJunction(uuid);
    if (successful)
    {
        if (was_source_trace_origin)
        {
            this->source_trace_origin_node_uuid = QUuid();
            emit signalWaterQualityOptionsChanged();
        }

        rebuildBoundingBoxWgs84();
        markNetworkChanged(NetworkChange::Geometry, uuid);
    }
    return successful;
}

bool HydraulicData::deleteReservoir(const QUuid &uuid)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.deleteReservoir"), uuid);
    edit_scope.watchNodeDeletionDependents(uuid);
    const bool was_source_trace_origin = this->source_trace_origin_node_uuid == uuid;
    const bool successful = this->network_editor.deleteReservoir(uuid);
    if (successful)
    {
        if (was_source_trace_origin)
        {
            this->source_trace_origin_node_uuid = QUuid();
            emit signalWaterQualityOptionsChanged();
        }

        rebuildBoundingBoxWgs84();
        markNetworkChanged(NetworkChange::Geometry, uuid);
    }
    return successful;
}

bool HydraulicData::deleteTank(const QUuid &uuid)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.deleteTank"), uuid);
    edit_scope.watchNodeDeletionDependents(uuid);
    const bool was_source_trace_origin = this->source_trace_origin_node_uuid == uuid;
    const bool successful = this->network_editor.deleteTank(uuid);
    if (successful)
    {
        if (was_source_trace_origin)
        {
            this->source_trace_origin_node_uuid = QUuid();
            emit signalWaterQualityOptionsChanged();
        }

        rebuildBoundingBoxWgs84();
        markNetworkChanged(NetworkChange::Geometry, uuid);
    }
    return successful;
}

bool HydraulicData::deleteDemandPoint(const QUuid &uuid)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.deleteDemandPoint"), uuid);
    const bool successful = this->network_editor.deleteDemandPoint(uuid);
    if (successful)
    {
        rebuildBoundingBoxWgs84();
        markNetworkChanged(NetworkChange::Geometry, uuid);
        markSimulationDiagnosticEntityStale(uuid);
    }
    return successful;
}

bool HydraulicData::deletePipe(const QUuid &uuid)
{
    HydraulicEditTransactionCapture capture(QStringLiteral("hydraulic.pipe.delete"));
    if (this->logical_edit_capture_enabled)
    {
        capture.watch(this->network_hydraulic, uuid);
        for (const HydraulicControlSimple &control : this->network_hydraulic.controls_simple)
        {
            if (control.link_uuid == uuid)
                capture.watch(this->network_hydraulic, control.uuid);
        }
        for (const HydraulicDemandPoint &point : this->network_hydraulic.demand_points)
        {
            if (point.attachment.type == HydraulicDemandPointAttachmentType::Pipe &&
                point.attachment.pipe_uuid == uuid)
                capture.watch(this->network_hydraulic, point.uuid);
        }
        for (const HydraulicControlRule &rule : this->network_hydraulic.controls_rules)
        {
            if (hydraulic_rule_dependencies::referencesEntity(rule, uuid))
                capture.watch(this->network_hydraulic, rule.uuid);
        }
    }
    const bool successful = this->network_editor.deletePipe(uuid);
    if (successful)
    {
        rebuildBoundingBoxWgs84();
        markNetworkChanged(NetworkChange::Geometry, uuid);
    }
    if (successful && this->logical_edit_capture_enabled)
        publishCapturedEdit(capture);
    return successful;
}

bool HydraulicData::deletePump(const QUuid &uuid)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.deletePump"), uuid);
    edit_scope.watchLinkDeletionDependents(uuid);
    const bool successful = this->network_editor.deletePump(uuid);
    if (successful)
    {
        rebuildBoundingBoxWgs84();
        markNetworkChanged(NetworkChange::Geometry, uuid);
    }
    return successful;
}

bool HydraulicData::deleteValve(const QUuid &uuid)
{
    EditScope edit_scope(*this, QStringLiteral("hydraulic.deleteValve"), uuid);
    edit_scope.watchLinkDeletionDependents(uuid);
    const bool successful = this->network_editor.deleteValve(uuid);
    if (successful)
    {
        rebuildBoundingBoxWgs84();
        markNetworkChanged(NetworkChange::Geometry, uuid);
    }
    return successful;
}
