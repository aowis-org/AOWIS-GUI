#include "network/hydraulic_network_editor.h"
#include "network/hydraulic_rule_dependencies.h"
#include "geo/geo_metric_projection.h"
#include <aowis/model/uuid.h>

#include <cmath>

namespace
{
template<typename Entity>
Entity *entityByUuid(QList<Entity> &entities, const QUuid &uuid)
{
    for (Entity &entity : entities)
    {
        if (entity.uuid == uuid)
            return &entity;
    }

    return nullptr;
}

template<typename Entity>
const Entity *entityByUuid(const QList<Entity> &entities, const QUuid &uuid)
{
    for (const Entity &entity : entities)
    {
        if (entity.uuid == uuid)
            return &entity;
    }

    return nullptr;
}

template<typename Entity>
bool removeEntityByUuid(QList<Entity> &entities, const QUuid &uuid)
{
    for (int i = 0; i < entities.size(); i++)
    {
        if (entities[i].uuid != uuid)
            continue;

        entities.removeAt(i);
        return true;
    }

    return false;
}

// Removing a referenced node or link must not leave orphaned simple controls.
// The corresponding GUI mutation watches these dependent control UUIDs before
// invoking the editor, so all deletions belong to the same MODEL transaction.
void removeSimpleControlsReferencing(NetworkHydraulic &network, const QUuid &entity_uuid)
{
    for (int index = network.controls_simple.size() - 1; index >= 0; --index)
    {
        const HydraulicControlSimple &control = network.controls_simple.at(index);
        if (control.link_uuid == entity_uuid || control.trigger_node_uuid == entity_uuid)
            network.controls_simple.removeAt(index);
    }
}

// A rule may reference a deleted node/link through its premises or actions.
// Removing the rule atomically avoids dangling hydraulic dependencies.
void removeRuleControlsReferencing(NetworkHydraulic &network, const QUuid &entity_uuid)
{
    for (int index = network.controls_rules.size() - 1; index >= 0; --index)
    {
        if (hydraulic_rule_dependencies::referencesEntity(
                network.controls_rules.at(index), entity_uuid))
            network.controls_rules.removeAt(index);
    }
}

template<typename Mutation>
bool mutateNode(NetworkHydraulic &network, const QUuid &uuid, Mutation mutation)
{
    for (HydraulicNodeJunction &junction : network.nodes_junctions)
    {
        if (junction.uuid != uuid)
            continue;

        mutation(junction);
        return true;
    }

    for (HydraulicNodeReservoir &reservoir : network.nodes_reservoirs)
    {
        if (reservoir.uuid != uuid)
            continue;

        mutation(reservoir);
        return true;
    }

    for (HydraulicNodeTank &tank : network.nodes_tanks)
    {
        if (tank.uuid != uuid)
            continue;

        mutation(tank);
        return true;
    }

    return false;
}

template<typename Mutation>
bool mutateLink(NetworkHydraulic &network, const QUuid &uuid, Mutation mutation)
{
    for (HydraulicLinkPipe &pipe : network.links_pipes)
    {
        if (pipe.uuid != uuid)
            continue;

        mutation(pipe);
        return true;
    }

    for (HydraulicLinkPump &pump : network.links_pumps)
    {
        if (pump.uuid != uuid)
            continue;

        mutation(pump);
        return true;
    }

    for (HydraulicLinkValve &valve : network.links_valves)
    {
        if (valve.uuid != uuid)
            continue;

        mutation(valve);
        return true;
    }

    return false;
}
}

HydraulicNetworkEditor::HydraulicNetworkEditor(NetworkHydraulic &network)
    : network(network)
{}

bool HydraulicNetworkEditor::hasNode(const QUuid &uuid) const
{
    return nodeCoordinate(uuid).has_value();
}

std::optional<CoordinateWGS84> HydraulicNetworkEditor::nodeCoordinate(const QUuid &uuid) const
{
    if (uuid.isNull())
        return std::nullopt;

    for (const HydraulicNodeJunction &junction : this->network.nodes_junctions)
    {
        if (junction.uuid == uuid)
            return junction.coordinate_wgs84;
    }

    for (const HydraulicNodeReservoir &reservoir : this->network.nodes_reservoirs)
    {
        if (reservoir.uuid == uuid)
            return reservoir.coordinate_wgs84;
    }

    for (const HydraulicNodeTank &tank : this->network.nodes_tanks)
    {
        if (tank.uuid == uuid)
            return tank.coordinate_wgs84;
    }

    return std::nullopt;
}

std::optional<HydraulicNodeJunction> HydraulicNetworkEditor::junction(const QUuid &uuid) const
{
    if (uuid.isNull())
        return std::nullopt;

    const HydraulicNodeJunction *junction = entityByUuid(this->network.nodes_junctions, uuid);
    if (junction == nullptr)
        return std::nullopt;

    return *junction;
}

std::optional<HydraulicNodeReservoir> HydraulicNetworkEditor::reservoir(const QUuid &uuid) const
{
    if (uuid.isNull())
        return std::nullopt;

    const HydraulicNodeReservoir *reservoir = entityByUuid(this->network.nodes_reservoirs, uuid);
    if (reservoir == nullptr)
        return std::nullopt;

    return *reservoir;
}

std::optional<HydraulicNodeTank> HydraulicNetworkEditor::tank(const QUuid &uuid) const
{
    if (uuid.isNull())
        return std::nullopt;

    const HydraulicNodeTank *tank = entityByUuid(this->network.nodes_tanks, uuid);
    if (tank == nullptr)
        return std::nullopt;

    return *tank;
}

std::optional<HydraulicDemandPoint> HydraulicNetworkEditor::demandPoint(const QUuid &uuid) const
{
    if (uuid.isNull())
        return std::nullopt;

    const HydraulicDemandPoint *demand_point = entityByUuid(this->network.demand_points, uuid);
    if (demand_point == nullptr)
        return std::nullopt;

    return *demand_point;
}

std::optional<HydraulicLinkPipe> HydraulicNetworkEditor::pipe(const QUuid &uuid) const
{
    if (uuid.isNull())
        return std::nullopt;

    const HydraulicLinkPipe *pipe = entityByUuid(this->network.links_pipes, uuid);
    if (pipe == nullptr)
        return std::nullopt;

    return *pipe;
}

std::optional<HydraulicLinkPump> HydraulicNetworkEditor::pump(const QUuid &uuid) const
{
    if (uuid.isNull())
        return std::nullopt;

    const HydraulicLinkPump *pump = entityByUuid(this->network.links_pumps, uuid);
    if (pump == nullptr)
        return std::nullopt;

    return *pump;
}

std::optional<HydraulicLinkValve> HydraulicNetworkEditor::valve(const QUuid &uuid) const
{
    if (uuid.isNull())
        return std::nullopt;

    const HydraulicLinkValve *valve = entityByUuid(this->network.links_valves, uuid);
    if (valve == nullptr)
        return std::nullopt;

    return *valve;
}

QUuid HydraulicNetworkEditor::addJunction(const CoordinateWGS84 &coordinate)
{
    HydraulicNodeJunction junction;
    junction.uuid = createUuidV7();
    junction.id = nextNodeId(QStringLiteral("J"));
    junction.coordinate_wgs84 = coordinate;
    junction.metadata.date_added = QDate::currentDate();
    this->network.nodes_junctions.append(junction);
    return junction.uuid;
}

QUuid HydraulicNetworkEditor::addReservoir(const CoordinateWGS84 &coordinate)
{
    HydraulicNodeReservoir reservoir;
    reservoir.uuid = createUuidV7();
    reservoir.id = nextNodeId(QStringLiteral("R"));
    reservoir.coordinate_wgs84 = coordinate;
    reservoir.metadata.date_added = QDate::currentDate();
    this->network.nodes_reservoirs.append(reservoir);
    return reservoir.uuid;
}

QUuid HydraulicNetworkEditor::addTank(const CoordinateWGS84 &coordinate)
{
    HydraulicNodeTank tank;
    tank.uuid = createUuidV7();
    tank.id = nextNodeId(QStringLiteral("T"));
    tank.coordinate_wgs84 = coordinate;
    tank.metadata.date_added = QDate::currentDate();
    this->network.nodes_tanks.append(tank);
    return tank.uuid;
}

QUuid HydraulicNetworkEditor::addDemandPoint(const CoordinateWGS84 &coordinate)
{
    HydraulicDemandPoint demand_point;
    demand_point.uuid = createUuidV7();
    demand_point.id = nextDemandPointId();
    demand_point.coordinate_wgs84 = coordinate;
    demand_point.metadata.date_added = QDate::currentDate();
    this->network.demand_points.append(demand_point);
    return demand_point.uuid;
}

QUuid HydraulicNetworkEditor::addPipe(const QUuid &node_uuid_from, const QUuid &node_uuid_to,
                                      const QList<CoordinateWGS84> &intermediate_vertices)
{
    if (!hasNode(node_uuid_from) || !hasNode(node_uuid_to) || node_uuid_from == node_uuid_to)
        return QUuid();

    HydraulicLinkPipe pipe;
    pipe.uuid = createUuidV7();
    pipe.id = nextLinkId(QStringLiteral("P"));
    pipe.node_uuid_from = node_uuid_from;
    pipe.node_uuid_to = node_uuid_to;
    pipe.metadata.date_added = QDate::currentDate();

    for (const CoordinateWGS84 &coordinate : intermediate_vertices)
    {
        HydraulicLinkVertex vertex;
        vertex.coordinate_wgs84 = coordinate;
        pipe.vertices.append(vertex);
    }

    pipe.length_calculated_m = pipeLengthMeters(pipe.node_uuid_from, pipe.node_uuid_to, pipe.vertices);
    this->network.links_pipes.append(pipe);
    return pipe.uuid;
}

QUuid HydraulicNetworkEditor::addPump(const QUuid &node_uuid_from, const QUuid &node_uuid_to,
                                      const CoordinateWGS84 &center_coordinate)
{
    if (!hasNode(node_uuid_from) || !hasNode(node_uuid_to) || node_uuid_from == node_uuid_to)
        return QUuid();

    HydraulicLinkPump pump;
    pump.uuid = createUuidV7();
    pump.id = nextLinkId(QStringLiteral("PU"));
    pump.node_uuid_from = node_uuid_from;
    pump.node_uuid_to = node_uuid_to;
    pump.metadata.date_added = QDate::currentDate();

    HydraulicLinkVertex vertex;
    vertex.coordinate_wgs84 = center_coordinate;
    pump.vertices.append(vertex);

    this->network.links_pumps.append(pump);
    return pump.uuid;
}

QUuid HydraulicNetworkEditor::addValve(const QUuid &node_uuid_from, const QUuid &node_uuid_to,
                                       const CoordinateWGS84 &center_coordinate)
{
    if (!hasNode(node_uuid_from) || !hasNode(node_uuid_to) || node_uuid_from == node_uuid_to)
        return QUuid();

    HydraulicLinkValve valve;
    valve.uuid = createUuidV7();
    valve.id = nextLinkId(QStringLiteral("V"));
    valve.node_uuid_from = node_uuid_from;
    valve.node_uuid_to = node_uuid_to;
    valve.metadata.date_added = QDate::currentDate();

    HydraulicLinkVertex vertex;
    vertex.coordinate_wgs84 = center_coordinate;
    valve.vertices.append(vertex);

    this->network.links_valves.append(valve);
    return valve.uuid;
}

bool HydraulicNetworkEditor::setNodeId(const QUuid &uuid, const QString &id)
{
    return mutateNode(this->network, uuid, [&id](auto &node)
    {
        node.id = id;
    });
}

bool HydraulicNetworkEditor::setNodeModelRole(const QUuid &uuid, EntityModelRole model_role)
{
    return mutateNode(this->network, uuid, [model_role](auto &node)
    {
        node.metadata.model_role = model_role;
    });
}

bool HydraulicNetworkEditor::setNodeDateAdded(const QUuid &uuid, const std::optional<QDate> &date_added)
{
    return mutateNode(this->network, uuid, [&date_added](auto &node)
    {
        node.metadata.date_added = date_added;
    });
}

bool HydraulicNetworkEditor::setNodeDateInstalled(const QUuid &uuid, const std::optional<QDate> &date_installed)
{
    return mutateNode(this->network, uuid, [&date_installed](auto &node)
    {
        node.metadata.date_installed = date_installed;
    });
}

bool HydraulicNetworkEditor::setNodeEnabled(const QUuid &uuid, bool enabled)
{
    return mutateNode(this->network, uuid, [enabled](auto &node)
    {
        node.metadata.enabled = enabled;
    });
}

bool HydraulicNetworkEditor::setLinkId(const QUuid &uuid, const QString &id)
{
    return mutateLink(this->network, uuid, [&id](auto &link)
    {
        link.id = id;
    });
}

bool HydraulicNetworkEditor::setLinkModelRole(const QUuid &uuid, EntityModelRole model_role)
{
    return mutateLink(this->network, uuid, [model_role](auto &link)
    {
        link.metadata.model_role = model_role;
    });
}

bool HydraulicNetworkEditor::setLinkDateAdded(const QUuid &uuid,
                                               const std::optional<QDate> &date_added)
{
    return mutateLink(this->network, uuid, [&date_added](auto &link)
    {
        link.metadata.date_added = date_added;
    });
}

bool HydraulicNetworkEditor::setLinkDateInstalled(
    const QUuid &uuid, const std::optional<QDate> &date_installed)
{
    return mutateLink(this->network, uuid, [&date_installed](auto &link)
    {
        link.metadata.date_installed = date_installed;
    });
}

bool HydraulicNetworkEditor::setLinkEnabled(const QUuid &uuid, bool enabled)
{
    return mutateLink(this->network, uuid, [enabled](auto &link)
    {
        link.metadata.enabled = enabled;
    });
}

bool HydraulicNetworkEditor::setNodeCoordinate(const QUuid &uuid, const CoordinateWGS84 &coordinate)
{
    for (HydraulicNodeJunction &junction : this->network.nodes_junctions)
    {
        if (junction.uuid != uuid)
            continue;

        junction.coordinate_wgs84 = coordinate;
        recalculateConnectedPipeLengths(uuid);
        return true;
    }

    for (HydraulicNodeReservoir &reservoir : this->network.nodes_reservoirs)
    {
        if (reservoir.uuid != uuid)
            continue;

        reservoir.coordinate_wgs84 = coordinate;
        recalculateConnectedPipeLengths(uuid);
        return true;
    }

    for (HydraulicNodeTank &tank : this->network.nodes_tanks)
    {
        if (tank.uuid != uuid)
            continue;

        tank.coordinate_wgs84 = coordinate;
        recalculateConnectedPipeLengths(uuid);
        return true;
    }

    return false;
}

bool HydraulicNetworkEditor::setDemandPointId(const QUuid &uuid, const QString &id)
{
    HydraulicDemandPoint *demand_point = entityByUuid(this->network.demand_points, uuid);
    if (demand_point == nullptr)
        return false;

    demand_point->id = id;
    return true;
}

bool HydraulicNetworkEditor::setDemandPointModelRole(
    const QUuid &uuid, EntityModelRole model_role)
{
    HydraulicDemandPoint *demand_point = entityByUuid(this->network.demand_points, uuid);
    if (demand_point == nullptr)
        return false;

    demand_point->metadata.model_role = model_role;
    return true;
}

bool HydraulicNetworkEditor::setDemandPointDateAdded(
    const QUuid &uuid, const std::optional<QDate> &date_added)
{
    HydraulicDemandPoint *demand_point = entityByUuid(this->network.demand_points, uuid);
    if (demand_point == nullptr)
        return false;

    demand_point->metadata.date_added = date_added;
    return true;
}

bool HydraulicNetworkEditor::setDemandPointDateInstalled(
    const QUuid &uuid, const std::optional<QDate> &date_installed)
{
    HydraulicDemandPoint *demand_point = entityByUuid(this->network.demand_points, uuid);
    if (demand_point == nullptr)
        return false;

    demand_point->metadata.date_installed = date_installed;
    return true;
}

bool HydraulicNetworkEditor::setDemandPointEnabled(const QUuid &uuid, bool enabled)
{
    HydraulicDemandPoint *demand_point = entityByUuid(this->network.demand_points, uuid);
    if (demand_point == nullptr)
        return false;

    demand_point->metadata.enabled = enabled;
    return true;
}

bool HydraulicNetworkEditor::setDemandPointDescription(
    const QUuid &uuid, const QString &description)
{
    HydraulicDemandPoint *demand_point = entityByUuid(this->network.demand_points, uuid);
    if (demand_point == nullptr)
        return false;

    demand_point->metadata.description = description;
    return true;
}

bool HydraulicNetworkEditor::setDemandPointComment(
    const QUuid &uuid, const QString &comment)
{
    HydraulicDemandPoint *demand_point = entityByUuid(this->network.demand_points, uuid);
    if (demand_point == nullptr)
        return false;

    demand_point->metadata.comment = comment;
    return true;
}

bool HydraulicNetworkEditor::setDemandPointTags(
    const QUuid &uuid, const QStringList &tags)
{
    HydraulicDemandPoint *demand_point = entityByUuid(this->network.demand_points, uuid);
    if (demand_point == nullptr)
        return false;

    demand_point->metadata.tags = tags;
    return true;
}

bool HydraulicNetworkEditor::setDemandPointCoordinate(
    const QUuid &uuid, const CoordinateWGS84 &coordinate)
{
    HydraulicDemandPoint *demand_point = entityByUuid(this->network.demand_points, uuid);
    if (demand_point == nullptr)
        return false;

    demand_point->coordinate_wgs84 = coordinate;
    return true;
}

bool HydraulicNetworkEditor::addDemandPointDemand(
    const QUuid &uuid, const HydraulicDemand &demand)
{
    HydraulicDemandPoint *demand_point = entityByUuid(this->network.demand_points, uuid);
    if (demand_point == nullptr)
        return false;

    demand_point->demands.append(demand);
    return true;
}

bool HydraulicNetworkEditor::removeDemandPointDemand(const QUuid &uuid, int demand_index)
{
    HydraulicDemandPoint *demand_point = entityByUuid(this->network.demand_points, uuid);
    if (demand_point == nullptr || demand_index < 0 || demand_index >= demand_point->demands.size())
        return false;

    demand_point->demands.removeAt(demand_index);
    return true;
}

bool HydraulicNetworkEditor::setDemandPointDemandCategoryName(
    const QUuid &uuid, int demand_index, const QString &category_name)
{
    HydraulicDemandPoint *demand_point = entityByUuid(this->network.demand_points, uuid);
    if (demand_point == nullptr || demand_index < 0 || demand_index >= demand_point->demands.size())
        return false;

    demand_point->demands[demand_index].category_name = category_name;
    return true;
}

bool HydraulicNetworkEditor::setDemandPointDemandBaseDemandM3PerH(
    const QUuid &uuid, int demand_index, double base_demand_m3_per_h)
{
    HydraulicDemandPoint *demand_point = entityByUuid(this->network.demand_points, uuid);
    if (demand_point == nullptr || demand_index < 0 || demand_index >= demand_point->demands.size())
        return false;

    demand_point->demands[demand_index].base_demand_m3_per_h = base_demand_m3_per_h;
    return true;
}

bool HydraulicNetworkEditor::setDemandPointDemandPatternMode(
    const QUuid &uuid, int demand_index, HydraulicTimePatternMode pattern_mode)
{
    HydraulicDemandPoint *demand_point = entityByUuid(this->network.demand_points, uuid);
    if (demand_point == nullptr || demand_index < 0 || demand_index >= demand_point->demands.size())
        return false;

    demand_point->demands[demand_index].pattern_mode = pattern_mode;
    return true;
}

bool HydraulicNetworkEditor::setDemandPointDemandPatternUuid(
    const QUuid &uuid, int demand_index, const QUuid &pattern_uuid)
{
    HydraulicDemandPoint *demand_point = entityByUuid(this->network.demand_points, uuid);
    if (demand_point == nullptr || demand_index < 0 || demand_index >= demand_point->demands.size())
        return false;

    demand_point->demands[demand_index].pattern_uuid = pattern_uuid;
    return true;
}

bool HydraulicNetworkEditor::setDemandPointDemandSourceMethod(
    const QUuid &uuid, int demand_index, HydraulicDemandSourceMethod source_method)
{
    HydraulicDemandPoint *demand_point = entityByUuid(this->network.demand_points, uuid);
    if (demand_point == nullptr || demand_index < 0 || demand_index >= demand_point->demands.size())
        return false;

    demand_point->demands[demand_index].source_method = source_method;
    return true;
}

bool HydraulicNetworkEditor::setDemandPointDemandNote(
    const QUuid &uuid, int demand_index, const QString &note)
{
    HydraulicDemandPoint *demand_point = entityByUuid(this->network.demand_points, uuid);
    if (demand_point == nullptr || demand_index < 0 || demand_index >= demand_point->demands.size())
        return false;

    demand_point->demands[demand_index].note = note;
    return true;
}

bool HydraulicNetworkEditor::addDemandPointMeter(const QUuid &uuid)
{
    HydraulicDemandPoint *demand_point = entityByUuid(this->network.demand_points, uuid);
    if (demand_point == nullptr || demand_point->meter.has_value())
        return false;

    WaterMeter meter;
    meter.uuid = createUuidV7();
    meter.id = nextWaterMeterId();
    meter.metadata.date_added = QDate::currentDate();
    demand_point->meter = meter;
    return true;
}

bool HydraulicNetworkEditor::setDemandPointMeter(
    const QUuid &uuid, const WaterMeter &meter)
{
    HydraulicDemandPoint *demand_point = entityByUuid(this->network.demand_points, uuid);
    if (demand_point == nullptr || !demand_point->meter.has_value())
        return false;
    if (meter.uuid.isNull() || meter.uuid != demand_point->meter->uuid)
        return false;

    demand_point->meter = meter;
    return true;
}

bool HydraulicNetworkEditor::removeDemandPointMeter(const QUuid &uuid)
{
    HydraulicDemandPoint *demand_point = entityByUuid(this->network.demand_points, uuid);
    if (demand_point == nullptr || !demand_point->meter.has_value())
        return false;

    demand_point->meter.reset();
    return true;
}

bool HydraulicNetworkEditor::attachDemandPointToPipe(
    const QUuid &uuid, const QUuid &pipe_uuid, double pipe_position)
{
    if (!std::isfinite(pipe_position) || pipe_position < 0.0 || pipe_position > 1.0)
        return false;

    HydraulicDemandPoint *demand_point = entityByUuid(this->network.demand_points, uuid);
    const HydraulicLinkPipe *pipe = entityByUuid(this->network.links_pipes, pipe_uuid);
    if (demand_point == nullptr || pipe == nullptr)
        return false;

    HydraulicDemandPointPipeAllocationMode allocation_mode =
        HydraulicDemandPointPipeAllocationMode::InterpolateByPosition;
    if (demand_point->attachment.type == HydraulicDemandPointAttachmentType::Pipe)
        allocation_mode = demand_point->attachment.pipe_allocation_mode;

    const QUuid previous_assigned_junction_uuid =
        demand_point->attachment.pipe_assigned_junction_uuid;

    demand_point->attachment = HydraulicDemandPointAttachment();
    demand_point->attachment.type = HydraulicDemandPointAttachmentType::Pipe;
    demand_point->attachment.pipe_uuid = pipe_uuid;
    demand_point->attachment.pipe_position = pipe_position;
    demand_point->attachment.pipe_allocation_mode = allocation_mode;

    if (allocation_mode != HydraulicDemandPointPipeAllocationMode::AssignedJunction)
        return true;

    const bool previous_assignment_is_valid =
        previous_assigned_junction_uuid == pipe->node_uuid_from
        || previous_assigned_junction_uuid == pipe->node_uuid_to;
    if (previous_assignment_is_valid
        && entityByUuid(this->network.nodes_junctions, previous_assigned_junction_uuid) != nullptr)
    {
        demand_point->attachment.pipe_assigned_junction_uuid =
            previous_assigned_junction_uuid;
        return true;
    }

    return assignDemandPointToNearestPipeJunction(*demand_point, *pipe);
}

bool HydraulicNetworkEditor::setDemandPointPipeAllocationMode(
    const QUuid &uuid, HydraulicDemandPointPipeAllocationMode allocation_mode)
{
    HydraulicDemandPoint *demand_point = entityByUuid(this->network.demand_points, uuid);
    if (demand_point == nullptr
        || demand_point->attachment.type != HydraulicDemandPointAttachmentType::Pipe)
    {
        return false;
    }

    if (allocation_mode == HydraulicDemandPointPipeAllocationMode::InterpolateByPosition)
    {
        demand_point->attachment.pipe_allocation_mode = allocation_mode;
        demand_point->attachment.pipe_assigned_junction_uuid = QUuid();
        return true;
    }

    const HydraulicLinkPipe *pipe =
        entityByUuid(this->network.links_pipes, demand_point->attachment.pipe_uuid);
    if (pipe == nullptr)
        return false;

    const QUuid assigned_junction_uuid =
        demand_point->attachment.pipe_assigned_junction_uuid;
    const bool assigned_junction_is_valid =
        (assigned_junction_uuid == pipe->node_uuid_from
         || assigned_junction_uuid == pipe->node_uuid_to)
        && entityByUuid(this->network.nodes_junctions, assigned_junction_uuid) != nullptr;

    demand_point->attachment.pipe_allocation_mode = allocation_mode;
    if (assigned_junction_is_valid)
        return true;

    return assignDemandPointToNearestPipeJunction(*demand_point, *pipe);
}

bool HydraulicNetworkEditor::setDemandPointPipeAssignedJunction(
    const QUuid &uuid, const QUuid &junction_uuid)
{
    HydraulicDemandPoint *demand_point = entityByUuid(this->network.demand_points, uuid);
    if (demand_point == nullptr
        || demand_point->attachment.type != HydraulicDemandPointAttachmentType::Pipe
        || demand_point->attachment.pipe_allocation_mode
            != HydraulicDemandPointPipeAllocationMode::AssignedJunction)
    {
        return false;
    }

    const HydraulicLinkPipe *pipe =
        entityByUuid(this->network.links_pipes, demand_point->attachment.pipe_uuid);
    const HydraulicNodeJunction *junction =
        entityByUuid(this->network.nodes_junctions, junction_uuid);
    if (pipe == nullptr || junction == nullptr)
        return false;
    if (junction_uuid != pipe->node_uuid_from && junction_uuid != pipe->node_uuid_to)
        return false;

    demand_point->attachment.pipe_assigned_junction_uuid = junction_uuid;
    return true;
}

bool HydraulicNetworkEditor::attachDemandPointToJunction(
    const QUuid &uuid, const QUuid &junction_uuid)
{
    HydraulicDemandPoint *demand_point = entityByUuid(this->network.demand_points, uuid);
    const HydraulicNodeJunction *junction = entityByUuid(this->network.nodes_junctions, junction_uuid);
    if (demand_point == nullptr || junction == nullptr)
        return false;

    demand_point->attachment = HydraulicDemandPointAttachment();
    demand_point->attachment.type = HydraulicDemandPointAttachmentType::Junction;
    demand_point->attachment.junction_uuid = junction_uuid;
    return true;
}

bool HydraulicNetworkEditor::clearDemandPointAttachment(const QUuid &uuid)
{
    HydraulicDemandPoint *demand_point = entityByUuid(this->network.demand_points, uuid);
    if (demand_point == nullptr)
        return false;

    demand_point->attachment = HydraulicDemandPointAttachment();
    return true;
}

bool HydraulicNetworkEditor::setJunctionElevationInputType(
    const QUuid &uuid, HydraulicNodeElevationInputType input_type)
{
    HydraulicNodeJunction *junction = entityByUuid(this->network.nodes_junctions, uuid);
    if (junction == nullptr)
        return false;

    junction->elevation_input_type = input_type;
    return true;
}

bool HydraulicNetworkEditor::setJunctionElevationM(const QUuid &uuid, double elevation_m)
{
    HydraulicNodeJunction *junction = entityByUuid(this->network.nodes_junctions, uuid);
    if (junction == nullptr)
        return false;

    junction->elevation_m = elevation_m;
    return true;
}

bool HydraulicNetworkEditor::setJunctionTerrainElevationM(const QUuid &uuid,
                                                          double terrain_elevation_m)
{
    HydraulicNodeJunction *junction = entityByUuid(this->network.nodes_junctions, uuid);
    if (junction == nullptr)
        return false;

    junction->terrain_elevation_m = terrain_elevation_m;
    return true;
}

bool HydraulicNetworkEditor::setJunctionElevationOffsetM(const QUuid &uuid,
                                                         double elevation_offset_m)
{
    HydraulicNodeJunction *junction = entityByUuid(this->network.nodes_junctions, uuid);
    if (junction == nullptr)
        return false;

    junction->elevation_offset_m = elevation_offset_m;
    return true;
}

bool HydraulicNetworkEditor::addJunctionDemand(const QUuid &uuid,
                                               const HydraulicDemand &demand)
{
    HydraulicNodeJunction *junction = entityByUuid(this->network.nodes_junctions, uuid);
    if (junction == nullptr)
        return false;

    junction->demands.append(demand);
    return true;
}

bool HydraulicNetworkEditor::removeJunctionDemand(const QUuid &uuid, int demand_index)
{
    HydraulicNodeJunction *junction = entityByUuid(this->network.nodes_junctions, uuid);
    if (junction == nullptr || demand_index < 0 || demand_index >= junction->demands.size())
        return false;

    junction->demands.removeAt(demand_index);
    return true;
}

bool HydraulicNetworkEditor::setJunctionDemandCategoryName(const QUuid &uuid, int demand_index,
                                                           const QString &category_name)
{
    HydraulicNodeJunction *junction = entityByUuid(this->network.nodes_junctions, uuid);
    if (junction == nullptr || demand_index < 0 || demand_index >= junction->demands.size())
        return false;

    junction->demands[demand_index].category_name = category_name;
    return true;
}

bool HydraulicNetworkEditor::setJunctionDemandBaseDemandM3PerH(
    const QUuid &uuid, int demand_index, double base_demand_m3_per_h)
{
    HydraulicNodeJunction *junction = entityByUuid(this->network.nodes_junctions, uuid);
    if (junction == nullptr || demand_index < 0 || demand_index >= junction->demands.size())
        return false;

    junction->demands[demand_index].base_demand_m3_per_h = base_demand_m3_per_h;
    return true;
}

bool HydraulicNetworkEditor::setJunctionDemandPatternMode(
    const QUuid &uuid, int demand_index, HydraulicTimePatternMode pattern_mode)
{
    HydraulicNodeJunction *junction = entityByUuid(this->network.nodes_junctions, uuid);
    if (junction == nullptr || demand_index < 0 || demand_index >= junction->demands.size())
        return false;

    junction->demands[demand_index].pattern_mode = pattern_mode;
    return true;
}

bool HydraulicNetworkEditor::setJunctionDemandPatternUuid(const QUuid &uuid, int demand_index,
                                                          const QUuid &pattern_uuid)
{
    HydraulicNodeJunction *junction = entityByUuid(this->network.nodes_junctions, uuid);
    if (junction == nullptr || demand_index < 0 || demand_index >= junction->demands.size())
        return false;

    junction->demands[demand_index].pattern_uuid = pattern_uuid;
    return true;
}

bool HydraulicNetworkEditor::setJunctionDemandSourceMethod(
    const QUuid &uuid, int demand_index, HydraulicDemandSourceMethod source_method)
{
    HydraulicNodeJunction *junction = entityByUuid(this->network.nodes_junctions, uuid);
    if (junction == nullptr || demand_index < 0 || demand_index >= junction->demands.size())
        return false;

    junction->demands[demand_index].source_method = source_method;
    return true;
}

bool HydraulicNetworkEditor::setJunctionDemandNote(const QUuid &uuid, int demand_index,
                                                   const QString &note)
{
    HydraulicNodeJunction *junction = entityByUuid(this->network.nodes_junctions, uuid);
    if (junction == nullptr || demand_index < 0 || demand_index >= junction->demands.size())
        return false;

    junction->demands[demand_index].note = note;
    return true;
}

bool HydraulicNetworkEditor::setJunctionEmitterCoefficient(const QUuid &uuid, double coefficient)
{
    HydraulicNodeJunction *junction = entityByUuid(this->network.nodes_junctions, uuid);
    if (junction == nullptr)
        return false;

    junction->emitter.coefficient = coefficient;
    return true;
}

bool HydraulicNetworkEditor::setJunctionEmitterPressureExponent(const QUuid &uuid, double pressure_exponent)
{
    HydraulicNodeJunction *junction = entityByUuid(this->network.nodes_junctions, uuid);
    if (junction == nullptr)
        return false;

    junction->emitter.pressure_exponent = pressure_exponent;
    return true;
}

bool HydraulicNetworkEditor::setNodeInitialChemicalConcentrationMgPerL(const QUuid &uuid, double value_mg_per_l)
{
    HydraulicNodeJunction *junction = entityByUuid(this->network.nodes_junctions, uuid);
    if (junction != nullptr)
    {
        junction->initial_chemical_concentration_mg_per_l = value_mg_per_l;
        return true;
    }
    HydraulicNodeReservoir *reservoir = entityByUuid(this->network.nodes_reservoirs, uuid);
    if (reservoir != nullptr)
    {
        reservoir->initial_chemical_concentration_mg_per_l = value_mg_per_l;
        return true;
    }
    HydraulicNodeTank *tank = entityByUuid(this->network.nodes_tanks, uuid);
    if (tank != nullptr)
    {
        tank->initial_chemical_concentration_mg_per_l = value_mg_per_l;
        return true;
    }
    return false;
}

bool HydraulicNetworkEditor::setNodeInitialWaterAgeH(const QUuid &uuid, double value_h)
{
    HydraulicNodeJunction *junction = entityByUuid(this->network.nodes_junctions, uuid);
    if (junction != nullptr) { junction->initial_water_age_h = value_h; return true; }
    HydraulicNodeReservoir *reservoir = entityByUuid(this->network.nodes_reservoirs, uuid);
    if (reservoir != nullptr) { reservoir->initial_water_age_h = value_h; return true; }
    HydraulicNodeTank *tank = entityByUuid(this->network.nodes_tanks, uuid);
    if (tank != nullptr) { tank->initial_water_age_h = value_h; return true; }
    return false;
}

bool HydraulicNetworkEditor::setNodeQualitySourceType(const QUuid &uuid, HydraulicNodeQualitySourceType source_type)
{
    HydraulicNodeJunction *junction = entityByUuid(this->network.nodes_junctions, uuid);
    if (junction != nullptr) { junction->quality_source.type = source_type; return true; }
    HydraulicNodeReservoir *reservoir = entityByUuid(this->network.nodes_reservoirs, uuid);
    if (reservoir != nullptr) { reservoir->quality_source.type = source_type; return true; }
    HydraulicNodeTank *tank = entityByUuid(this->network.nodes_tanks, uuid);
    if (tank != nullptr) { tank->quality_source.type = source_type; return true; }
    return false;
}

bool HydraulicNetworkEditor::setNodeQualitySourceChemicalConcentrationMgPerL(const QUuid &uuid, double value_mg_per_l)
{
    HydraulicNodeJunction *junction = entityByUuid(this->network.nodes_junctions, uuid);
    if (junction != nullptr) { junction->quality_source.chemical_concentration_mg_per_l = value_mg_per_l; return true; }
    HydraulicNodeReservoir *reservoir = entityByUuid(this->network.nodes_reservoirs, uuid);
    if (reservoir != nullptr) { reservoir->quality_source.chemical_concentration_mg_per_l = value_mg_per_l; return true; }
    HydraulicNodeTank *tank = entityByUuid(this->network.nodes_tanks, uuid);
    if (tank != nullptr) { tank->quality_source.chemical_concentration_mg_per_l = value_mg_per_l; return true; }
    return false;
}

bool HydraulicNetworkEditor::setNodeQualitySourceMassFlowMgPerMin(const QUuid &uuid, double value_mg_per_min)
{
    HydraulicNodeJunction *junction = entityByUuid(this->network.nodes_junctions, uuid);
    if (junction != nullptr) { junction->quality_source.chemical_mass_flow_mg_per_min = value_mg_per_min; return true; }
    HydraulicNodeReservoir *reservoir = entityByUuid(this->network.nodes_reservoirs, uuid);
    if (reservoir != nullptr) { reservoir->quality_source.chemical_mass_flow_mg_per_min = value_mg_per_min; return true; }
    HydraulicNodeTank *tank = entityByUuid(this->network.nodes_tanks, uuid);
    if (tank != nullptr) { tank->quality_source.chemical_mass_flow_mg_per_min = value_mg_per_min; return true; }
    return false;
}

bool HydraulicNetworkEditor::setNodeQualitySourcePatternUuid(const QUuid &uuid, const QUuid &pattern_uuid)
{
    HydraulicNodeJunction *junction = entityByUuid(this->network.nodes_junctions, uuid);
    if (junction != nullptr) { junction->quality_source.pattern_uuid = pattern_uuid; return true; }
    HydraulicNodeReservoir *reservoir = entityByUuid(this->network.nodes_reservoirs, uuid);
    if (reservoir != nullptr) { reservoir->quality_source.pattern_uuid = pattern_uuid; return true; }
    HydraulicNodeTank *tank = entityByUuid(this->network.nodes_tanks, uuid);
    if (tank != nullptr) { tank->quality_source.pattern_uuid = pattern_uuid; return true; }
    return false;
}

bool HydraulicNetworkEditor::setReservoirHeadInputType(
    const QUuid &uuid, HydraulicNodeElevationInputType input_type)
{
    HydraulicNodeReservoir *reservoir = entityByUuid(this->network.nodes_reservoirs, uuid);
    if (reservoir == nullptr)
        return false;

    reservoir->head_input_type = input_type;
    return true;
}

bool HydraulicNetworkEditor::setReservoirHeadM(const QUuid &uuid, double hydraulic_head_m)
{
    HydraulicNodeReservoir *reservoir = entityByUuid(this->network.nodes_reservoirs, uuid);
    if (reservoir == nullptr)
        return false;

    reservoir->hydraulic_head_m = hydraulic_head_m;
    return true;
}

bool HydraulicNetworkEditor::setReservoirTerrainElevationM(const QUuid &uuid,
                                                           double terrain_elevation_m)
{
    HydraulicNodeReservoir *reservoir = entityByUuid(this->network.nodes_reservoirs, uuid);
    if (reservoir == nullptr)
        return false;

    reservoir->terrain_elevation_m = terrain_elevation_m;
    return true;
}

bool HydraulicNetworkEditor::setReservoirHeadOffsetM(const QUuid &uuid, double hydraulic_head_offset_m)
{
    HydraulicNodeReservoir *reservoir = entityByUuid(this->network.nodes_reservoirs, uuid);
    if (reservoir == nullptr)
        return false;

    reservoir->hydraulic_head_offset_m = hydraulic_head_offset_m;
    return true;
}

bool HydraulicNetworkEditor::setReservoirHeadPatternMode(const QUuid &uuid, HydraulicTimePatternMode pattern_mode)
{
    HydraulicNodeReservoir *reservoir = entityByUuid(this->network.nodes_reservoirs, uuid);
    if (reservoir == nullptr)
        return false;

    reservoir->head_pattern_mode = pattern_mode;
    return true;
}

bool HydraulicNetworkEditor::setReservoirHeadPatternUuid(const QUuid &uuid,
                                                         const QUuid &pattern_uuid)
{
    HydraulicNodeReservoir *reservoir = entityByUuid(this->network.nodes_reservoirs, uuid);
    if (reservoir == nullptr)
        return false;

    reservoir->head_pattern_uuid = pattern_uuid;
    return true;
}

bool HydraulicNetworkEditor::setTankElevationInputType(
    const QUuid &uuid, HydraulicNodeTankElevationInputType input_type)
{
    HydraulicNodeTank *tank = entityByUuid(this->network.nodes_tanks, uuid);
    if (tank == nullptr)
        return false;

    tank->elevation_input_type = input_type;
    return true;
}

bool HydraulicNetworkEditor::setTankBottomElevationM(const QUuid &uuid, double bottom_elevation_m)
{
    HydraulicNodeTank *tank = entityByUuid(this->network.nodes_tanks, uuid);
    if (tank == nullptr)
        return false;

    tank->bottom_elevation_m = bottom_elevation_m;
    return true;
}

bool HydraulicNetworkEditor::setTankTerrainElevationM(const QUuid &uuid,
                                                      double terrain_elevation_m)
{
    HydraulicNodeTank *tank = entityByUuid(this->network.nodes_tanks, uuid);
    if (tank == nullptr)
        return false;

    tank->terrain_elevation_m = terrain_elevation_m;
    return true;
}

bool HydraulicNetworkEditor::setTankBottomOffsetM(const QUuid &uuid, double bottom_offset_m)
{
    HydraulicNodeTank *tank = entityByUuid(this->network.nodes_tanks, uuid);
    if (tank == nullptr)
        return false;

    tank->bottom_offset_m = bottom_offset_m;
    return true;
}

bool HydraulicNetworkEditor::setTankWaterLevelInitialM(const QUuid &uuid,
                                                       double water_level_initial_m)
{
    HydraulicNodeTank *tank = entityByUuid(this->network.nodes_tanks, uuid);
    if (tank == nullptr)
        return false;

    tank->water_level_initial_m = water_level_initial_m;
    return true;
}

bool HydraulicNetworkEditor::setTankWaterLevelMinimumM(const QUuid &uuid,
                                                       double water_level_minimum_m)
{
    HydraulicNodeTank *tank = entityByUuid(this->network.nodes_tanks, uuid);
    if (tank == nullptr)
        return false;

    tank->water_level_minimum_m = water_level_minimum_m;
    return true;
}

bool HydraulicNetworkEditor::setTankWaterLevelMaximumM(const QUuid &uuid,
                                                       double water_level_maximum_m)
{
    HydraulicNodeTank *tank = entityByUuid(this->network.nodes_tanks, uuid);
    if (tank == nullptr)
        return false;

    tank->water_level_maximum_m = water_level_maximum_m;
    return true;
}

bool HydraulicNetworkEditor::setTankGeometryInputType(
    const QUuid &uuid, HydraulicNodeTankGeometryInputType input_type)
{
    HydraulicNodeTank *tank = entityByUuid(this->network.nodes_tanks, uuid);
    if (tank == nullptr)
        return false;

    tank->geometry_input_type = input_type;
    return true;
}

bool HydraulicNetworkEditor::setTankDiameterM(const QUuid &uuid, double diameter_m)
{
    HydraulicNodeTank *tank = entityByUuid(this->network.nodes_tanks, uuid);
    if (tank == nullptr)
        return false;

    tank->diameter_m = diameter_m;
    return true;
}

bool HydraulicNetworkEditor::setTankCrossSectionAreaM2(const QUuid &uuid,
                                                       double cross_section_area_m2)
{
    HydraulicNodeTank *tank = entityByUuid(this->network.nodes_tanks, uuid);
    if (tank == nullptr)
        return false;

    tank->cross_section_area_m2 = cross_section_area_m2;
    return true;
}

bool HydraulicNetworkEditor::setTankVolumeAtMaximumLevelM3(
    const QUuid &uuid, double volume_at_maximum_level_m3)
{
    HydraulicNodeTank *tank = entityByUuid(this->network.nodes_tanks, uuid);
    if (tank == nullptr)
        return false;

    tank->volume_at_maximum_level_m3 = volume_at_maximum_level_m3;
    return true;
}

bool HydraulicNetworkEditor::setTankMinimumVolumeM3(const QUuid &uuid, double minimum_volume_m3)
{
    HydraulicNodeTank *tank = entityByUuid(this->network.nodes_tanks, uuid);
    if (tank == nullptr)
        return false;

    tank->minimum_volume_m3 = minimum_volume_m3;
    return true;
}

bool HydraulicNetworkEditor::setTankVolumeCurveUuid(const QUuid &uuid,
                                                    const QUuid &volume_curve_uuid)
{
    HydraulicNodeTank *tank = entityByUuid(this->network.nodes_tanks, uuid);
    if (tank == nullptr)
        return false;

    tank->volume_curve_uuid = volume_curve_uuid;
    return true;
}

bool HydraulicNetworkEditor::setTankCanOverflow(const QUuid &uuid, bool can_overflow)
{
    HydraulicNodeTank *tank = entityByUuid(this->network.nodes_tanks, uuid);
    if (tank == nullptr)
        return false;

    tank->can_overflow = can_overflow;
    return true;
}

bool HydraulicNetworkEditor::setTankQualityMixingModel(const QUuid &uuid, HydraulicNodeTankMixingModel mixing_model)
{
    HydraulicNodeTank *tank = entityByUuid(this->network.nodes_tanks, uuid);
    if (tank == nullptr) return false;
    tank->mixing_model = mixing_model;
    return true;
}

bool HydraulicNetworkEditor::setTankQualityMixingFraction(const QUuid &uuid, double mixing_fraction)
{
    HydraulicNodeTank *tank = entityByUuid(this->network.nodes_tanks, uuid);
    if (tank == nullptr) return false;
    tank->mixing_fraction = mixing_fraction;
    return true;
}

bool HydraulicNetworkEditor::setTankOverrideBulkReaction(const QUuid &uuid, bool override_bulk_reaction)
{
    HydraulicNodeTank *tank = entityByUuid(this->network.nodes_tanks, uuid);
    if (tank == nullptr) return false;
    tank->override_bulk_reaction = override_bulk_reaction;
    return true;
}

bool HydraulicNetworkEditor::setTankBulkReactionCoefficient(const QUuid &uuid, double coefficient)
{
    HydraulicNodeTank *tank = entityByUuid(this->network.nodes_tanks, uuid);
    if (tank == nullptr) return false;
    tank->bulk_reaction.coefficient = coefficient;
    return true;
}

bool HydraulicNetworkEditor::setPipeInitialStatus(
    const QUuid &uuid, HydraulicLinkPipeInitialStatus initial_status)
{
    HydraulicLinkPipe *pipe = entityByUuid(this->network.links_pipes, uuid);
    if (pipe == nullptr)
        return false;

    pipe->initial_status = initial_status;
    return true;
}

bool HydraulicNetworkEditor::setPipeDiameterMm(const QUuid &uuid, double diameter_mm)
{
    HydraulicLinkPipe *pipe = entityByUuid(this->network.links_pipes, uuid);
    if (pipe == nullptr)
        return false;

    pipe->diameter_mm = diameter_mm;
    return true;
}

bool HydraulicNetworkEditor::setPipeMeasuredLengthM(
    const QUuid &uuid, const std::optional<double> &length_measured_m)
{
    HydraulicLinkPipe *pipe = entityByUuid(this->network.links_pipes, uuid);
    if (pipe == nullptr)
        return false;

    pipe->length_measured_m = length_measured_m;
    return true;
}

bool HydraulicNetworkEditor::setPipeMaterialUuid(const QUuid &uuid, const QUuid &material_uuid)
{
    HydraulicLinkPipe *pipe = entityByUuid(this->network.links_pipes, uuid);
    if (pipe == nullptr)
        return false;

    if (!material_uuid.isNull())
    {
        const HydraulicPipeMaterial *material =
            entityByUuid(this->network.pipe_materials, material_uuid);
        if (material == nullptr)
            return false;
    }

    pipe->material_uuid = material_uuid;
    if (material_uuid.isNull()
        && pipe->roughness_mode == HydraulicPipeRoughnessMode::MaterialLibrary)
    {
        pipe->roughness_mode = HydraulicPipeRoughnessMode::Explicit;
    }
    return true;
}

bool HydraulicNetworkEditor::setPipeRoughnessMode(
    const QUuid &uuid, HydraulicPipeRoughnessMode roughness_mode)
{
    HydraulicLinkPipe *pipe = entityByUuid(this->network.links_pipes, uuid);
    if (pipe == nullptr)
        return false;

    if (roughness_mode == HydraulicPipeRoughnessMode::MaterialLibrary
        && pipe->material_uuid.isNull())
    {
        return false;
    }

    pipe->roughness_mode = roughness_mode;
    return true;
}

bool HydraulicNetworkEditor::setPipeRoughnessHw(const QUuid &uuid, double roughness_hazen_williams)
{
    HydraulicLinkPipe *pipe = entityByUuid(this->network.links_pipes, uuid);
    if (pipe == nullptr)
        return false;

    pipe->roughness_hazen_williams = roughness_hazen_williams;
    return true;
}

bool HydraulicNetworkEditor::setPipeRoughnessDwMm(const QUuid &uuid, double roughness_darcy_weisbach_mm)
{
    HydraulicLinkPipe *pipe = entityByUuid(this->network.links_pipes, uuid);
    if (pipe == nullptr)
        return false;

    pipe->roughness_darcy_weisbach_mm = roughness_darcy_weisbach_mm;
    return true;
}

bool HydraulicNetworkEditor::setPipeRoughnessCm(const QUuid &uuid, double roughness_chezy_manning)
{
    HydraulicLinkPipe *pipe = entityByUuid(this->network.links_pipes, uuid);
    if (pipe == nullptr)
        return false;

    pipe->roughness_chezy_manning = roughness_chezy_manning;
    return true;
}

bool HydraulicNetworkEditor::setPipeMinorLoss(const QUuid &uuid, double minor_loss_coefficient)
{
    HydraulicLinkPipe *pipe = entityByUuid(this->network.links_pipes, uuid);
    if (pipe == nullptr)
        return false;

    pipe->minor_loss_coefficient = minor_loss_coefficient;
    return true;
}

bool HydraulicNetworkEditor::setPipeOverrideBulkReaction(
    const QUuid &uuid, bool override_bulk_reaction)
{
    HydraulicLinkPipe *pipe = entityByUuid(this->network.links_pipes, uuid);
    if (pipe == nullptr)
        return false;
    pipe->override_bulk_reaction = override_bulk_reaction;
    return true;
}

bool HydraulicNetworkEditor::setPipeOverrideWallReaction(
    const QUuid &uuid, bool override_wall_reaction)
{
    HydraulicLinkPipe *pipe = entityByUuid(this->network.links_pipes, uuid);
    if (pipe == nullptr)
        return false;
    pipe->override_wall_reaction = override_wall_reaction;
    return true;
}

bool HydraulicNetworkEditor::setPipeBulkReactionCoefficient(const QUuid &uuid, double coefficient)
{
    HydraulicLinkPipe *pipe = entityByUuid(this->network.links_pipes, uuid);
    if (pipe == nullptr) return false;
    pipe->bulk_reaction.coefficient = coefficient;
    return true;
}

bool HydraulicNetworkEditor::setPipeWallReactionCoefficient(const QUuid &uuid, double coefficient)
{
    HydraulicLinkPipe *pipe = entityByUuid(this->network.links_pipes, uuid);
    if (pipe == nullptr) return false;
    pipe->wall_reaction.coefficient = coefficient;
    return true;
}

bool HydraulicNetworkEditor::setPumpDefinitionType(
    const QUuid &uuid, HydraulicLinkPumpDefinitionType definition_type)
{
    HydraulicLinkPump *pump = entityByUuid(this->network.links_pumps, uuid);
    if (pump == nullptr)
        return false;

    pump->definition_type = definition_type;
    return true;
}

bool HydraulicNetworkEditor::setPumpConstantPowerKw(const QUuid &uuid, double constant_power_kw)
{
    HydraulicLinkPump *pump = entityByUuid(this->network.links_pumps, uuid);
    if (pump == nullptr)
        return false;

    pump->constant_power_kw = constant_power_kw;
    return true;
}

bool HydraulicNetworkEditor::setPumpInitialSpeed(const QUuid &uuid, double initial_speed_ratio)
{
    HydraulicLinkPump *pump = entityByUuid(this->network.links_pumps, uuid);
    if (pump == nullptr)
        return false;

    pump->initial_speed_ratio = initial_speed_ratio;
    return true;
}

bool HydraulicNetworkEditor::setPumpInitialStatus(
    const QUuid &uuid, HydraulicLinkPumpInitialStatus initial_status)
{
    HydraulicLinkPump *pump = entityByUuid(this->network.links_pumps, uuid);
    if (pump == nullptr)
        return false;

    pump->initial_status = initial_status;
    return true;
}

bool HydraulicNetworkEditor::setPumpSpeedPatternUuid(
    const QUuid &uuid, const QUuid &speed_pattern_uuid)
{
    HydraulicLinkPump *pump = entityByUuid(this->network.links_pumps, uuid);
    if (pump == nullptr)
        return false;

    pump->speed_pattern_uuid = speed_pattern_uuid;
    return true;
}

QUuid HydraulicNetworkEditor::addPumpSimpleControl(
    const QUuid &pump_uuid, HydraulicControlSimpleType type, const QUuid &trigger_node_uuid)
{
    if (entityByUuid(this->network.links_pumps, pump_uuid) == nullptr)
        return QUuid();

    HydraulicControlSimple control;
    control.id = nextSimpleControlId();
    control.uuid = createUuidV7();
    control.type = type;
    control.link_uuid = pump_uuid;
    control.action = type == HydraulicControlSimpleType::HighLevel
        ? HydraulicControlActionType::Close : HydraulicControlActionType::Open;
    control.trigger_node_uuid = trigger_node_uuid;
    this->network.controls_simple.append(control);
    return control.uuid;
}

bool HydraulicNetworkEditor::setPumpSimpleControl(
    const QUuid &pump_uuid, const HydraulicControlSimple &control)
{
    if (entityByUuid(this->network.links_pumps, pump_uuid) == nullptr)
        return false;
    if (control.uuid.isNull() || control.link_uuid != pump_uuid)
        return false;

    HydraulicControlSimple *stored = entityByUuid(this->network.controls_simple, control.uuid);
    if (stored == nullptr || stored->link_uuid != pump_uuid)
        return false;

    *stored = control;
    return true;
}

bool HydraulicNetworkEditor::removePumpSimpleControl(
    const QUuid &pump_uuid, const QUuid &control_uuid)
{
    if (entityByUuid(this->network.links_pumps, pump_uuid) == nullptr)
        return false;

    HydraulicControlSimple *control = entityByUuid(this->network.controls_simple, control_uuid);
    if (control == nullptr || control->link_uuid != pump_uuid)
        return false;

    return removeEntityByUuid(this->network.controls_simple, control_uuid);
}

bool HydraulicNetworkEditor::setPumpEfficiencyInput(
    const QUuid &uuid, HydraulicLinkPumpEfficiencyInputType input_type,
    const QUuid &efficiency_curve_uuid)
{
    HydraulicLinkPump *pump = entityByUuid(this->network.links_pumps, uuid);
    if (pump == nullptr)
        return false;

    pump->efficiency_input_type = input_type;
    pump->efficiency_curve_uuid = efficiency_curve_uuid;
    return true;
}

bool HydraulicNetworkEditor::setPumpEnergyPricePerKwh(
    const QUuid &uuid, double energy_price_per_kw_h)
{
    HydraulicLinkPump *pump = entityByUuid(this->network.links_pumps, uuid);
    if (pump == nullptr)
        return false;

    pump->energy_price_per_kw_h = energy_price_per_kw_h;
    return true;
}

bool HydraulicNetworkEditor::setPumpEnergyPriceInput(
    const QUuid &uuid, HydraulicLinkPumpEnergyPriceInputType input_type,
    const QUuid &price_pattern_uuid)
{
    HydraulicLinkPump *pump = entityByUuid(this->network.links_pumps, uuid);
    if (pump == nullptr)
        return false;

    pump->energy_price_input_type = input_type;
    pump->price_pattern_uuid = price_pattern_uuid;
    return true;
}

bool HydraulicNetworkEditor::setValveType(const QUuid &uuid, HydraulicLinkValveType type)
{
    HydraulicLinkValve *valve = entityByUuid(this->network.links_valves, uuid);
    if (valve == nullptr)
        return false;

    valve->type = type;
    return true;
}

bool HydraulicNetworkEditor::setValveSettingPressureHeadM(
    const QUuid &uuid, double setting_pressure_head_m)
{
    HydraulicLinkValve *valve = entityByUuid(this->network.links_valves, uuid);
    if (valve == nullptr)
        return false;

    valve->setting_pressure_head_m = setting_pressure_head_m;
    return true;
}

bool HydraulicNetworkEditor::setValveSettingFlowM3PerH(
    const QUuid &uuid, double setting_flow_m3_per_h)
{
    HydraulicLinkValve *valve = entityByUuid(this->network.links_valves, uuid);
    if (valve == nullptr)
        return false;

    valve->setting_flow_m3_per_h = setting_flow_m3_per_h;
    return true;
}

bool HydraulicNetworkEditor::setValveSettingLossCoefficient(
    const QUuid &uuid, double setting_loss_coefficient)
{
    HydraulicLinkValve *valve = entityByUuid(this->network.links_valves, uuid);
    if (valve == nullptr)
        return false;

    valve->setting_loss_coefficient = setting_loss_coefficient;
    return true;
}

bool HydraulicNetworkEditor::setValveSettingPositionPercent(
    const QUuid &uuid, double setting_position_percent)
{
    HydraulicLinkValve *valve = entityByUuid(this->network.links_valves, uuid);
    if (valve == nullptr)
        return false;

    valve->setting_position_percent = setting_position_percent;
    return true;
}

bool HydraulicNetworkEditor::setValveHeadLossCurveUuid(
    const QUuid &uuid, const QUuid &head_loss_curve_uuid)
{
    HydraulicLinkValve *valve = entityByUuid(this->network.links_valves, uuid);
    if (valve == nullptr)
        return false;

    valve->head_loss_curve_uuid = head_loss_curve_uuid;
    return true;
}

bool HydraulicNetworkEditor::setValveCharacteristicCurveUuid(
    const QUuid &uuid, const QUuid &characteristic_curve_uuid)
{
    HydraulicLinkValve *valve = entityByUuid(this->network.links_valves, uuid);
    if (valve == nullptr)
        return false;

    valve->characteristic_curve_uuid = characteristic_curve_uuid;
    return true;
}

bool HydraulicNetworkEditor::setValveInitialStatus(
    const QUuid &uuid, HydraulicLinkValveInitialStatus initial_status)
{
    HydraulicLinkValve *valve = entityByUuid(this->network.links_valves, uuid);
    if (valve == nullptr)
        return false;

    valve->initial_status = initial_status;
    return true;
}

bool HydraulicNetworkEditor::setValveDiameterMm(const QUuid &uuid, double diameter_mm)
{
    HydraulicLinkValve *valve = entityByUuid(this->network.links_valves, uuid);
    if (valve == nullptr)
        return false;

    valve->diameter_mm = diameter_mm;
    return true;
}

bool HydraulicNetworkEditor::setValveMinorLoss(const QUuid &uuid, double minor_loss_coefficient)
{
    HydraulicLinkValve *valve = entityByUuid(this->network.links_valves, uuid);
    if (valve == nullptr)
        return false;

    valve->minor_loss_coefficient = minor_loss_coefficient;
    return true;
}

bool HydraulicNetworkEditor::setPipeVertexCoordinate(const QUuid &pipe_uuid, int vertex_index,
                                                       const CoordinateWGS84 &coordinate)
{
    for (HydraulicLinkPipe &pipe : this->network.links_pipes)
    {
        if (pipe.uuid != pipe_uuid)
            continue;
        if (vertex_index < 0 || vertex_index >= pipe.vertices.size())
            return false;

        pipe.vertices[vertex_index].coordinate_wgs84 = coordinate;
        pipe.length_calculated_m = pipeLengthMeters(pipe.node_uuid_from, pipe.node_uuid_to, pipe.vertices);
        return true;
    }

    return false;
}

bool HydraulicNetworkEditor::setPipeVertices(const QUuid &pipe_uuid,
                                               const QList<CoordinateWGS84> &intermediate_vertices)
{
    for (HydraulicLinkPipe &pipe : this->network.links_pipes)
    {
        if (pipe.uuid != pipe_uuid)
            continue;

        pipe.vertices.clear();
        pipe.vertices.reserve(intermediate_vertices.size());
        for (const CoordinateWGS84 &coordinate : intermediate_vertices)
        {
            HydraulicLinkVertex vertex;
            vertex.coordinate_wgs84 = coordinate;
            pipe.vertices.append(vertex);
        }

        pipe.length_calculated_m = pipeLengthMeters(pipe.node_uuid_from, pipe.node_uuid_to, pipe.vertices);
        return true;
    }

    return false;
}

bool HydraulicNetworkEditor::setPumpCenterCoordinate(const QUuid &pump_uuid, const CoordinateWGS84 &coordinate)
{
    for (HydraulicLinkPump &pump : this->network.links_pumps)
    {
        if (pump.uuid != pump_uuid)
            continue;

        if (pump.vertices.isEmpty())
            pump.vertices.append(HydraulicLinkVertex());
        pump.vertices[0].coordinate_wgs84 = coordinate;
        return true;
    }

    return false;
}

bool HydraulicNetworkEditor::setValveCenterCoordinate(const QUuid &valve_uuid, const CoordinateWGS84 &coordinate)
{
    for (HydraulicLinkValve &valve : this->network.links_valves)
    {
        if (valve.uuid != valve_uuid)
            continue;

        if (valve.vertices.isEmpty())
            valve.vertices.append(HydraulicLinkVertex());
        valve.vertices[0].coordinate_wgs84 = coordinate;
        return true;
    }

    return false;
}

HydraulicGeometryBatchResult HydraulicNetworkEditor::applyGeometryBatch(
    const HydraulicGeometryBatch &batch)
{
    HydraulicGeometryBatchResult result;
    if (batch.isEmpty())
    {
        result.successful = true;
        return result;
    }

    QSet<QUuid> available_node_uuids;
    available_node_uuids.reserve(this->network.nodes_junctions.size() +
                                 this->network.nodes_reservoirs.size() +
                                 this->network.nodes_tanks.size());
    for (const HydraulicNodeJunction &node : this->network.nodes_junctions)
        available_node_uuids.insert(node.uuid);
    for (const HydraulicNodeReservoir &node : this->network.nodes_reservoirs)
        available_node_uuids.insert(node.uuid);
    for (const HydraulicNodeTank &node : this->network.nodes_tanks)
        available_node_uuids.insert(node.uuid);

    QSet<QUuid> available_demand_point_uuids;
    available_demand_point_uuids.reserve(this->network.demand_points.size());
    for (const HydraulicDemandPoint &demand_point : this->network.demand_points)
        available_demand_point_uuids.insert(demand_point.uuid);

    QSet<QUuid> available_pipe_uuids;
    available_pipe_uuids.reserve(this->network.links_pipes.size());
    for (const HydraulicLinkPipe &pipe : this->network.links_pipes)
        available_pipe_uuids.insert(pipe.uuid);

    QSet<QUuid> available_pump_uuids;
    available_pump_uuids.reserve(this->network.links_pumps.size());
    for (const HydraulicLinkPump &pump : this->network.links_pumps)
        available_pump_uuids.insert(pump.uuid);

    QSet<QUuid> available_valve_uuids;
    available_valve_uuids.reserve(this->network.links_valves.size());
    for (const HydraulicLinkValve &valve : this->network.links_valves)
        available_valve_uuids.insert(valve.uuid);

    for (auto iterator = batch.node_coordinates.cbegin(); iterator != batch.node_coordinates.cend(); ++iterator)
    {
        if (!available_node_uuids.contains(iterator.key()))
            return result;
    }
    for (QHash<QUuid, CoordinateWGS84>::const_iterator iterator =
             batch.demand_point_coordinates.cbegin();
         iterator != batch.demand_point_coordinates.cend(); ++iterator)
    {
        if (!available_demand_point_uuids.contains(iterator.key()))
            return result;
    }
    for (QHash<QUuid, QList<CoordinateWGS84>>::const_iterator iterator =
             batch.pipe_vertices.cbegin();
         iterator != batch.pipe_vertices.cend(); ++iterator)
    {
        if (!available_pipe_uuids.contains(iterator.key()))
            return result;
    }
    for (auto iterator = batch.pump_center_coordinates.cbegin(); iterator != batch.pump_center_coordinates.cend(); ++iterator)
    {
        if (!available_pump_uuids.contains(iterator.key()))
            return result;
    }
    for (auto iterator = batch.valve_center_coordinates.cbegin(); iterator != batch.valve_center_coordinates.cend(); ++iterator)
    {
        if (!available_valve_uuids.contains(iterator.key()))
            return result;
    }

    for (HydraulicNodeJunction &node : this->network.nodes_junctions)
    {
        const auto iterator = batch.node_coordinates.constFind(node.uuid);
        if (iterator != batch.node_coordinates.cend())
            node.coordinate_wgs84 = iterator.value();
    }
    for (HydraulicNodeReservoir &node : this->network.nodes_reservoirs)
    {
        const auto iterator = batch.node_coordinates.constFind(node.uuid);
        if (iterator != batch.node_coordinates.cend())
            node.coordinate_wgs84 = iterator.value();
    }
    for (HydraulicNodeTank &node : this->network.nodes_tanks)
    {
        const auto iterator = batch.node_coordinates.constFind(node.uuid);
        if (iterator != batch.node_coordinates.cend())
            node.coordinate_wgs84 = iterator.value();
    }

    for (HydraulicDemandPoint &demand_point : this->network.demand_points)
    {
        const QHash<QUuid, CoordinateWGS84>::const_iterator iterator =
            batch.demand_point_coordinates.constFind(demand_point.uuid);
        if (iterator != batch.demand_point_coordinates.cend())
            demand_point.coordinate_wgs84 = iterator.value();
    }

    for (HydraulicLinkPump &pump : this->network.links_pumps)
    {
        const auto iterator = batch.pump_center_coordinates.constFind(pump.uuid);
        if (iterator == batch.pump_center_coordinates.cend())
            continue;
        if (pump.vertices.isEmpty())
            pump.vertices.append(HydraulicLinkVertex());
        pump.vertices[0].coordinate_wgs84 = iterator.value();
    }
    for (HydraulicLinkValve &valve : this->network.links_valves)
    {
        const auto iterator = batch.valve_center_coordinates.constFind(valve.uuid);
        if (iterator == batch.valve_center_coordinates.cend())
            continue;
        if (valve.vertices.isEmpty())
            valve.vertices.append(HydraulicLinkVertex());
        valve.vertices[0].coordinate_wgs84 = iterator.value();
    }

    QSet<QUuid> moved_node_uuids;
    moved_node_uuids.reserve(batch.node_coordinates.size());
    for (auto iterator = batch.node_coordinates.cbegin(); iterator != batch.node_coordinates.cend(); ++iterator)
        moved_node_uuids.insert(iterator.key());
    for (HydraulicLinkPipe &pipe : this->network.links_pipes)
    {
        bool affected = moved_node_uuids.contains(pipe.node_uuid_from) ||
                        moved_node_uuids.contains(pipe.node_uuid_to);
        const auto vertices_iterator = batch.pipe_vertices.constFind(pipe.uuid);
        if (vertices_iterator != batch.pipe_vertices.cend())
        {
            pipe.vertices.clear();
            pipe.vertices.reserve(vertices_iterator.value().size());
            for (const CoordinateWGS84 &coordinate : vertices_iterator.value())
            {
                HydraulicLinkVertex vertex;
                vertex.coordinate_wgs84 = coordinate;
                pipe.vertices.append(vertex);
            }
            affected = true;
        }

        if (!affected)
            continue;

        pipe.length_calculated_m = pipeLengthMeters(
            pipe.node_uuid_from, pipe.node_uuid_to, pipe.vertices);
        result.affected_pipe_uuids.insert(pipe.uuid);
    }

    result.successful = true;
    return result;
}

QUuid HydraulicNetworkEditor::splitPipeAtVertex(const QUuid &pipe_uuid, int vertex_index,
                                                const QUuid &junction_uuid)
{
    if (!hasNode(junction_uuid))
        return QUuid();

    for (int i = 0; i < this->network.links_pipes.size(); i++)
    {
        const HydraulicLinkPipe original_pipe = this->network.links_pipes[i];
        if (original_pipe.uuid != pipe_uuid)
            continue;
        if (vertex_index < 0 || vertex_index >= original_pipe.vertices.size())
            return QUuid();

        const std::optional<double> split_position =
            pipeVertexPosition(original_pipe, vertex_index);
        if (!split_position.has_value())
            return QUuid();

        HydraulicLinkPipe first_pipe = original_pipe;
        first_pipe.node_uuid_to = junction_uuid;
        first_pipe.vertices = original_pipe.vertices.mid(0, vertex_index);
        first_pipe.length_calculated_m = pipeLengthMeters(first_pipe.node_uuid_from, first_pipe.node_uuid_to,
                                                          first_pipe.vertices);

        HydraulicLinkPipe second_pipe = original_pipe;
        second_pipe.uuid = createUuidV7();
        second_pipe.id = nextLinkId(QStringLiteral("P"));
        second_pipe.node_uuid_from = junction_uuid;
        second_pipe.vertices = original_pipe.vertices.mid(vertex_index + 1);
        second_pipe.length_calculated_m = pipeLengthMeters(second_pipe.node_uuid_from, second_pipe.node_uuid_to,
                                                           second_pipe.vertices);

        if (original_pipe.length_measured_m.has_value())
        {
            const double total_calculated_length_m = first_pipe.length_calculated_m + second_pipe.length_calculated_m;
            if (total_calculated_length_m > 0.0)
            {
                first_pipe.length_measured_m = original_pipe.length_measured_m.value() *
                                               first_pipe.length_calculated_m / total_calculated_length_m;
                second_pipe.length_measured_m = original_pipe.length_measured_m.value() -
                                                first_pipe.length_measured_m.value();
            }
            else
            {
                first_pipe.length_measured_m.reset();
                second_pipe.length_measured_m.reset();
            }
        }

        this->network.links_pipes[i] = first_pipe;
        this->network.links_pipes.insert(i + 1, second_pipe);

        const double split = split_position.value();
        constexpr double attachment_tolerance = 1.0e-12;
        for (HydraulicDemandPoint &demand_point : this->network.demand_points)
        {
            if (demand_point.attachment.type != HydraulicDemandPointAttachmentType::Pipe
                || demand_point.attachment.pipe_uuid != original_pipe.uuid)
            {
                continue;
            }

            const double old_position = demand_point.attachment.pipe_position;
            const bool assigned_junction_mode =
                demand_point.attachment.pipe_allocation_mode
                == HydraulicDemandPointPipeAllocationMode::AssignedJunction;

            if (std::abs(old_position - split) <= attachment_tolerance)
            {
                if (!assigned_junction_mode)
                {
                    demand_point.attachment = HydraulicDemandPointAttachment();
                    demand_point.attachment.type = HydraulicDemandPointAttachmentType::Junction;
                    demand_point.attachment.junction_uuid = junction_uuid;
                    continue;
                }

                demand_point.attachment.pipe_uuid = first_pipe.uuid;
                demand_point.attachment.pipe_position = 1.0;
                assignDemandPointToNearestPipeJunction(demand_point, first_pipe);
            }
            else if (old_position < split && split > 0.0)
            {
                demand_point.attachment.pipe_position = old_position / split;
                if (assigned_junction_mode)
                    assignDemandPointToNearestPipeJunction(demand_point, first_pipe);
            }
            else if (old_position > split && split < 1.0)
            {
                demand_point.attachment.pipe_uuid = second_pipe.uuid;
                demand_point.attachment.pipe_position =
                    (old_position - split) / (1.0 - split);
                if (assigned_junction_mode)
                    assignDemandPointToNearestPipeJunction(demand_point, second_pipe);
            }
        }

        return second_pipe.uuid;
    }

    return QUuid();
}

bool HydraulicNetworkEditor::undoPipeSplit(const QUuid &first_pipe_uuid, const QUuid &second_pipe_uuid,
                                            const QUuid &junction_uuid)
{
    int first_pipe_index = -1;
    int second_pipe_index = -1;

    for (int i = 0; i < this->network.links_pipes.size(); i++)
    {
        const QUuid uuid = this->network.links_pipes[i].uuid;
        if (uuid == first_pipe_uuid)
            first_pipe_index = i;
        else if (uuid == second_pipe_uuid)
            second_pipe_index = i;
    }

    if (first_pipe_index < 0 || second_pipe_index < 0)
        return false;

    const std::optional<CoordinateWGS84> junction_coordinate = nodeCoordinate(junction_uuid);
    if (!junction_coordinate.has_value())
        return false;

    const HydraulicLinkPipe first_pipe = this->network.links_pipes[first_pipe_index];
    const HydraulicLinkPipe second_pipe = this->network.links_pipes[second_pipe_index];
    if (first_pipe.node_uuid_to != junction_uuid || second_pipe.node_uuid_from != junction_uuid)
        return false;

    const double total_calculated_length_m =
        first_pipe.length_calculated_m + second_pipe.length_calculated_m;
    if (!std::isfinite(total_calculated_length_m) || total_calculated_length_m <= 0.0)
        return false;
    const double split_position = first_pipe.length_calculated_m / total_calculated_length_m;

    HydraulicLinkPipe restored_pipe = first_pipe;
    restored_pipe.node_uuid_to = second_pipe.node_uuid_to;

    HydraulicLinkVertex junction_vertex;
    junction_vertex.coordinate_wgs84 = junction_coordinate.value();
    restored_pipe.vertices.append(junction_vertex);
    for (const HydraulicLinkVertex &vertex : second_pipe.vertices)
        restored_pipe.vertices.append(vertex);

    if (first_pipe.length_measured_m.has_value() && second_pipe.length_measured_m.has_value())
    {
        restored_pipe.length_measured_m = first_pipe.length_measured_m.value() +
                                          second_pipe.length_measured_m.value();
    }
    else
    {
        restored_pipe.length_measured_m.reset();
    }

    restored_pipe.length_calculated_m = pipeLengthMeters(restored_pipe.node_uuid_from,
                                                          restored_pipe.node_uuid_to,
                                                          restored_pipe.vertices);
    this->network.links_pipes.removeAt(second_pipe_index);

    for (int i = 0; i < this->network.links_pipes.size(); i++)
    {
        if (this->network.links_pipes[i].uuid != first_pipe_uuid)
            continue;

        this->network.links_pipes[i] = restored_pipe;

        for (HydraulicDemandPoint &demand_point : this->network.demand_points)
        {
            if (demand_point.attachment.type == HydraulicDemandPointAttachmentType::Junction
                && demand_point.attachment.junction_uuid == junction_uuid)
            {
                demand_point.attachment = HydraulicDemandPointAttachment();
                demand_point.attachment.type = HydraulicDemandPointAttachmentType::Pipe;
                demand_point.attachment.pipe_uuid = first_pipe_uuid;
                demand_point.attachment.pipe_position = split_position;
                continue;
            }

            if (demand_point.attachment.type != HydraulicDemandPointAttachmentType::Pipe)
                continue;

            if (demand_point.attachment.pipe_uuid == first_pipe_uuid)
            {
                demand_point.attachment.pipe_position *= split_position;
                if (demand_point.attachment.pipe_allocation_mode
                    == HydraulicDemandPointPipeAllocationMode::AssignedJunction)
                {
                    assignDemandPointToNearestPipeJunction(demand_point, restored_pipe);
                }
            }
            else if (demand_point.attachment.pipe_uuid == second_pipe_uuid)
            {
                demand_point.attachment.pipe_uuid = first_pipe_uuid;
                demand_point.attachment.pipe_position =
                    split_position
                    + demand_point.attachment.pipe_position * (1.0 - split_position);
                if (demand_point.attachment.pipe_allocation_mode
                    == HydraulicDemandPointPipeAllocationMode::AssignedJunction)
                {
                    assignDemandPointToNearestPipeJunction(demand_point, restored_pipe);
                }
            }
        }

        return true;
    }

    return false;
}

bool HydraulicNetworkEditor::deleteJunction(const QUuid &uuid)
{
    if (!removeEntityByUuid(this->network.nodes_junctions, uuid))
        return false;

    removeSimpleControlsReferencing(this->network, uuid);
    removeRuleControlsReferencing(this->network, uuid);
    clearMapLabelAnchorsToNode(uuid);
    clearDemandPointAttachmentsToJunction(uuid);
    deleteConnectedLinks(uuid);
    return true;
}

bool HydraulicNetworkEditor::deleteReservoir(const QUuid &uuid)
{
    if (!removeEntityByUuid(this->network.nodes_reservoirs, uuid))
        return false;

    removeSimpleControlsReferencing(this->network, uuid);
    removeRuleControlsReferencing(this->network, uuid);
    clearMapLabelAnchorsToNode(uuid);
    deleteConnectedLinks(uuid);
    return true;
}

bool HydraulicNetworkEditor::deleteTank(const QUuid &uuid)
{
    if (!removeEntityByUuid(this->network.nodes_tanks, uuid))
        return false;

    removeSimpleControlsReferencing(this->network, uuid);
    removeRuleControlsReferencing(this->network, uuid);
    clearMapLabelAnchorsToNode(uuid);
    deleteConnectedLinks(uuid);
    return true;
}

bool HydraulicNetworkEditor::deleteDemandPoint(const QUuid &uuid)
{
    return removeEntityByUuid(this->network.demand_points, uuid);
}

bool HydraulicNetworkEditor::deletePipe(const QUuid &uuid)
{
    if (!removeEntityByUuid(this->network.links_pipes, uuid))
        return false;

    removeSimpleControlsReferencing(this->network, uuid);
    removeRuleControlsReferencing(this->network, uuid);
    clearDemandPointAttachmentsToPipe(uuid);
    return true;
}

bool HydraulicNetworkEditor::deletePump(const QUuid &uuid)
{
    if (!removeEntityByUuid(this->network.links_pumps, uuid))
        return false;
    removeSimpleControlsReferencing(this->network, uuid);
    removeRuleControlsReferencing(this->network, uuid);
    return true;
}

bool HydraulicNetworkEditor::deleteValve(const QUuid &uuid)
{
    if (!removeEntityByUuid(this->network.links_valves, uuid))
        return false;
    removeSimpleControlsReferencing(this->network, uuid);
    removeRuleControlsReferencing(this->network, uuid);
    return true;
}

bool HydraulicNetworkEditor::assignDemandPointToNearestPipeJunction(
    HydraulicDemandPoint &demand_point, const HydraulicLinkPipe &pipe)
{
    if (demand_point.attachment.type != HydraulicDemandPointAttachmentType::Pipe
        || demand_point.attachment.pipe_allocation_mode
            != HydraulicDemandPointPipeAllocationMode::AssignedJunction)
    {
        return false;
    }

    const bool from_is_junction =
        entityByUuid(this->network.nodes_junctions, pipe.node_uuid_from) != nullptr;
    const bool to_is_junction =
        entityByUuid(this->network.nodes_junctions, pipe.node_uuid_to) != nullptr;
    if (!from_is_junction && !to_is_junction)
    {
        demand_point.attachment.pipe_assigned_junction_uuid = QUuid();
        return false;
    }

    if (from_is_junction && to_is_junction)
    {
        demand_point.attachment.pipe_assigned_junction_uuid =
            demand_point.attachment.pipe_position <= 0.5
                ? pipe.node_uuid_from
                : pipe.node_uuid_to;
    }
    else if (from_is_junction)
    {
        demand_point.attachment.pipe_assigned_junction_uuid = pipe.node_uuid_from;
    }
    else
    {
        demand_point.attachment.pipe_assigned_junction_uuid = pipe.node_uuid_to;
    }

    return true;
}

void HydraulicNetworkEditor::clearDemandPointAttachmentsToPipe(const QUuid &pipe_uuid)
{
    for (HydraulicDemandPoint &demand_point : this->network.demand_points)
    {
        if (demand_point.attachment.type != HydraulicDemandPointAttachmentType::Pipe
            || demand_point.attachment.pipe_uuid != pipe_uuid)
        {
            continue;
        }

        demand_point.attachment = HydraulicDemandPointAttachment();
    }
}

void HydraulicNetworkEditor::clearDemandPointAttachmentsToJunction(const QUuid &junction_uuid)
{
    for (HydraulicDemandPoint &demand_point : this->network.demand_points)
    {
        const bool direct_junction_attachment =
            demand_point.attachment.type == HydraulicDemandPointAttachmentType::Junction
            && demand_point.attachment.junction_uuid == junction_uuid;
        const bool assigned_pipe_junction =
            demand_point.attachment.type == HydraulicDemandPointAttachmentType::Pipe
            && demand_point.attachment.pipe_allocation_mode
                == HydraulicDemandPointPipeAllocationMode::AssignedJunction
            && demand_point.attachment.pipe_assigned_junction_uuid == junction_uuid;
        if (!direct_junction_attachment && !assigned_pipe_junction)
            continue;

        demand_point.attachment = HydraulicDemandPointAttachment();
    }
}

void HydraulicNetworkEditor::clearMapLabelAnchorsToNode(const QUuid &node_uuid)
{
    // Labels retain their text and map position when their anchor is deleted.
    for (HydraulicMapLabel &label : this->network.map_labels)
    {
        if (label.anchor_node_uuid == node_uuid)
            label.anchor_node_uuid = QUuid();
    }
}

void HydraulicNetworkEditor::deleteConnectedLinks(const QUuid &node_uuid)
{
    for (int i = this->network.links_pipes.size() - 1; i >= 0; i--)
    {
        const HydraulicLinkPipe &pipe = this->network.links_pipes[i];
        if (pipe.node_uuid_from != node_uuid && pipe.node_uuid_to != node_uuid)
            continue;

        const QUuid pipe_uuid = pipe.uuid;
        this->network.links_pipes.removeAt(i);
        removeSimpleControlsReferencing(this->network, pipe_uuid);
        removeRuleControlsReferencing(this->network, pipe_uuid);
        clearDemandPointAttachmentsToPipe(pipe_uuid);
    }

    for (int i = this->network.links_pumps.size() - 1; i >= 0; i--)
    {
        const HydraulicLinkPump &pump = this->network.links_pumps[i];
        if (pump.node_uuid_from == node_uuid || pump.node_uuid_to == node_uuid)
        {
            const QUuid link_uuid = pump.uuid;
            this->network.links_pumps.removeAt(i);
            removeSimpleControlsReferencing(this->network, link_uuid);
            removeRuleControlsReferencing(this->network, link_uuid);
        }
    }

    for (int i = this->network.links_valves.size() - 1; i >= 0; i--)
    {
        const HydraulicLinkValve &valve = this->network.links_valves[i];
        if (valve.node_uuid_from == node_uuid || valve.node_uuid_to == node_uuid)
        {
            const QUuid link_uuid = valve.uuid;
            this->network.links_valves.removeAt(i);
            removeSimpleControlsReferencing(this->network, link_uuid);
            removeRuleControlsReferencing(this->network, link_uuid);
        }
    }
}

QString HydraulicNetworkEditor::nextNodeId(const QString &prefix) const
{
    for (int number = 1; ; number++)
    {
        const QString candidate = prefix + QString::number(number);
        bool used = false;

        for (const HydraulicNodeJunction &junction : this->network.nodes_junctions)
            used = used || junction.id == candidate;
        for (const HydraulicNodeReservoir &reservoir : this->network.nodes_reservoirs)
            used = used || reservoir.id == candidate;
        for (const HydraulicNodeTank &tank : this->network.nodes_tanks)
            used = used || tank.id == candidate;

        if (!used)
            return candidate;
    }
}

QString HydraulicNetworkEditor::nextLinkId(const QString &prefix) const
{
    for (int number = 1; ; number++)
    {
        const QString candidate = prefix + QString::number(number);
        bool used = false;

        for (const HydraulicLinkPipe &pipe : this->network.links_pipes)
            used = used || pipe.id == candidate;
        for (const HydraulicLinkPump &pump : this->network.links_pumps)
            used = used || pump.id == candidate;
        for (const HydraulicLinkValve &valve : this->network.links_valves)
            used = used || valve.id == candidate;

        if (!used)
            return candidate;
    }
}

QString HydraulicNetworkEditor::nextDemandPointId() const
{
    for (int number = 1; ; number++)
    {
        const QString candidate = QStringLiteral("DP") + QString::number(number);
        bool used = false;
        for (const HydraulicDemandPoint &demand_point : this->network.demand_points)
            used = used || demand_point.id == candidate;

        if (!used)
            return candidate;
    }
}

QString HydraulicNetworkEditor::nextWaterMeterId() const
{
    for (int number = 1; ; number++)
    {
        const QString candidate = QStringLiteral("M") + QString::number(number);
        bool used = false;
        for (const HydraulicDemandPoint &demand_point : this->network.demand_points)
        {
            if (demand_point.meter.has_value())
                used = used || demand_point.meter->id == candidate;
        }

        if (!used)
            return candidate;
    }
}

QString HydraulicNetworkEditor::nextSimpleControlId() const
{
    for (int number = 1; ; number++)
    {
        const QString candidate = QStringLiteral("C") + QString::number(number);
        bool used = false;
        for (const HydraulicControlSimple &control : this->network.controls_simple)
            used = used || control.id == candidate;

        if (!used)
            return candidate;
    }
}

double HydraulicNetworkEditor::pipeLengthMeters(const QUuid &node_uuid_from, const QUuid &node_uuid_to,
                                                  const QList<HydraulicLinkVertex> &vertices) const
{
    const std::optional<CoordinateWGS84> start_coordinate = nodeCoordinate(node_uuid_from);
    const std::optional<CoordinateWGS84> end_coordinate = nodeCoordinate(node_uuid_to);
    if (!start_coordinate.has_value() || !end_coordinate.has_value())
        return 0.0;

    double length_m = 0.0;
    CoordinateWGS84 previous_coordinate = start_coordinate.value();

    for (const HydraulicLinkVertex &vertex : vertices)
    {
        length_m += GeoMetricProjection::distanceMeters(previous_coordinate, vertex.coordinate_wgs84);
        previous_coordinate = vertex.coordinate_wgs84;
    }

    length_m += GeoMetricProjection::distanceMeters(previous_coordinate, end_coordinate.value());
    return length_m;
}

std::optional<double> HydraulicNetworkEditor::pipeVertexPosition(
    const HydraulicLinkPipe &pipe, int vertex_index) const
{
    if (vertex_index < 0 || vertex_index >= pipe.vertices.size())
        return std::nullopt;

    const std::optional<CoordinateWGS84> start_coordinate = nodeCoordinate(pipe.node_uuid_from);
    const std::optional<CoordinateWGS84> end_coordinate = nodeCoordinate(pipe.node_uuid_to);
    if (!start_coordinate.has_value() || !end_coordinate.has_value())
        return std::nullopt;

    double total_length_m = 0.0;
    double length_to_vertex_m = 0.0;
    CoordinateWGS84 previous_coordinate = start_coordinate.value();

    for (int index = 0; index < pipe.vertices.size(); index++)
    {
        const CoordinateWGS84 coordinate = pipe.vertices.at(index).coordinate_wgs84;
        const double segment_length_m =
            GeoMetricProjection::distanceMeters(previous_coordinate, coordinate);
        total_length_m += segment_length_m;
        if (index <= vertex_index)
            length_to_vertex_m += segment_length_m;
        previous_coordinate = coordinate;
    }

    total_length_m +=
        GeoMetricProjection::distanceMeters(previous_coordinate, end_coordinate.value());
    if (!std::isfinite(total_length_m) || total_length_m <= 0.0)
        return std::nullopt;

    const double position = length_to_vertex_m / total_length_m;
    if (!std::isfinite(position) || position < 0.0 || position > 1.0)
        return std::nullopt;

    return position;
}

void HydraulicNetworkEditor::recalculateConnectedPipeLengths(const QUuid &node_uuid)
{
    for (HydraulicLinkPipe &pipe : this->network.links_pipes)
    {
        if (pipe.node_uuid_from != node_uuid && pipe.node_uuid_to != node_uuid)
            continue;

        pipe.length_calculated_m = pipeLengthMeters(pipe.node_uuid_from, pipe.node_uuid_to, pipe.vertices);
    }
}
