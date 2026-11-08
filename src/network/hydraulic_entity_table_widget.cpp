#include "config/live_ui_text.h"
#include <aowis/model/units/quantity.h>
#include "network/hydraulic_entity_table_widget.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <optional>
#include <utility>

#include <QAbstractItemView>
#include <QAbstractSpinBox>
#include <QAbstractTableModel>
#include <QComboBox>
#include <QBrush>
#include <QColor>
#include <QDate>
#include <QDoubleSpinBox>
#include <QFont>
#include <QFrame>
#include <QFontMetrics>
#include <QHash>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPalette>
#include <QList>
#include <QHBoxLayout>
#include <QPushButton>
#include <QStyle>
#include <QToolButton>
#include <QTimer>
#include <QModelIndex>
#include <QSortFilterProxyModel>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QShowEvent>
#include <QSizePolicy>
#include <QSet>
#include <QStringList>
#include <QTableView>
#include <QVariant>
#include <QVBoxLayout>
#include <QResizeEvent>

#include "network/hydraulic_data.h"
#include "network/network_symbology_values.h"
#include "network/valve_setting_sort_policy.h"
#include "config/unit_table_presentation.h"
#include "config/unit_profile_manager.h"
#include "config/unit_aware_view.h"

namespace
{
constexpr int SortRole = Qt::UserRole;
constexpr int EntityUuidRole = Qt::UserRole + 1;
constexpr int SimulationStatusRole = Qt::UserRole + 2;
constexpr int ReactionOrderRole = Qt::UserRole + 3;
constexpr int ValveSettingGroupRole = Qt::UserRole + 4;

enum class SimulationRowStatus
{
    None = 0,
    Warning = 1,
    Error = 2
};

enum class SimulationStatusFilter
{
    All = 0,
    WarningsAndErrors = 1,
    WarningsOnly = 2,
    ErrorsOnly = 3
};

enum class ColumnFilterKind
{
    None,
    Text,
    Choice,
    Number,
    Integer
};

struct TableColumn
{
    QString title;
    bool simulation_result = false;
    UnitTablePresentation::ColumnUnits units;
    ColumnFilterKind filter_kind = ColumnFilterKind::None;
};

struct DemandDetail
{
    QString prefix;
    double canonical_rate = 0.0;
    QString suffix;
};

struct TableCell
{
    QString display;
    QVariant sort_value;
    Qt::Alignment alignment = Qt::AlignLeft | Qt::AlignVCenter;
    QString tooltip;
    int decimals = 6;
    QString canonical_suffix; // for raw numeric cells without registered quantity
    std::string_view quantity; // used for heterogeneous valve-setting columns
    std::optional<UnitTablePresentation::ReactionKind> reaction_kind;
    double reaction_order = 1.0;
};

struct TableRow
{
    QUuid uuid;
    std::optional<HydraulicLinkValveType> valve_type;
    QList<TableCell> cells;
    QList<DemandDetail> demand_details; // only populated for junction/demand-point rows
    bool simulation_warning = false;
    bool simulation_error = false;
    QString simulation_warning_tooltip;
    QString simulation_error_tooltip;
};

QString formatNumber(double value, int decimals, const QString &unit = QString())
{
    return UnitTablePresentation::formatNumber(value, decimals, unit);
}

TableCell emptyCell()
{
    TableCell cell;
    cell.display = QStringLiteral("—");
    return cell;
}

TableCell textCell(const QString &value)
{
    TableCell cell;
    cell.display = value.isEmpty() ? QStringLiteral("—") : value;
    cell.sort_value = value;
    return cell;
}

TableCell numberCell(double value, int decimals, const QString &unit = QString())
{
    TableCell cell;
    if (std::isfinite(value))
    {
        cell.display = formatNumber(value, decimals, unit);
        cell.sort_value = value;
        cell.decimals = decimals;
        cell.canonical_suffix = unit;
    }
    else
    {
        cell.display = QStringLiteral("—");
    }
    cell.alignment = Qt::AlignRight | Qt::AlignVCenter;
    return cell;
}

TableCell quantityCell(double value, int decimals, std::string_view quantity, const QString &canonical_suffix)
{
    TableCell cell = numberCell(value, decimals, canonical_suffix);
    cell.quantity = quantity;
    return cell;
}

TableCell optionalNumberCell(const std::optional<double> &value, int decimals,
                             const QString &unit = QString())
{
    if (!value.has_value())
        return emptyCell();

    return numberCell(value.value(), decimals, unit);
}

TableCell reactionCell(double coefficient, double order,
                       UnitTablePresentation::ReactionKind kind)
{
    TableCell cell = numberCell(coefficient, 8);
    cell.reaction_kind = kind;
    cell.reaction_order = order;
    return cell;
}

TableCell boolCell(bool value, const QString &true_text = QStringLiteral("Yes"),
                   const QString &false_text = QStringLiteral("No"))
{
    TableCell cell;
    cell.display = value ? true_text : false_text;
    cell.sort_value = value;
    cell.alignment = Qt::AlignCenter;
    return cell;
}

TableCell integerCell(int value)
{
    TableCell cell;
    cell.display = QString::number(value);
    cell.sort_value = value;
    cell.alignment = Qt::AlignRight | Qt::AlignVCenter;
    return cell;
}

TableCell dateCell(const std::optional<QDate> &date)
{
    if (!date.has_value())
        return emptyCell();

    TableCell cell;
    cell.display = date->toString(QStringLiteral("yyyy-MM-dd"));
    cell.sort_value = date.value();
    return cell;
}

QString entityModelRoleText(EntityModelRole role)
{
    switch (role)
    {
    case EntityModelRole::Unspecified:
        return QStringLiteral("Unspecified");
    case EntityModelRole::ExistingAsset:
        return QStringLiteral("Existing Asset");
    case EntityModelRole::PlannedAsset:
        return QStringLiteral("Planned Asset");
    case EntityModelRole::VirtualModelElement:
        return QStringLiteral("Virtual / Model-Only");
    case EntityModelRole::BoundaryCondition:
        return QStringLiteral("Boundary Condition");
    case EntityModelRole::TemporaryTesting:
        return QStringLiteral("Temporary / Testing");
    case EntityModelRole::RetiredAsset:
        return QStringLiteral("Retired Asset");
    }

    return QStringLiteral("Unknown");
}

QString elevationInputText(HydraulicNodeElevationInputType input_type)
{
    if (input_type == HydraulicNodeElevationInputType::TerrainElevationAndOffset)
        return QStringLiteral("Terrain Elevation + Offset");

    return QStringLiteral("Direct Value");
}

QString tankGeometryInputText(HydraulicNodeTankGeometryInputType input_type)
{
    switch (input_type)
    {
    case HydraulicNodeTankGeometryInputType::Cylindrical:
        return QStringLiteral("Cylindrical");
    case HydraulicNodeTankGeometryInputType::UniformArea:
        return QStringLiteral("Uniform Area");
    case HydraulicNodeTankGeometryInputType::VolumeAtMaximumLevel:
        return QStringLiteral("Maximum Volume");
    case HydraulicNodeTankGeometryInputType::VolumeCurve:
        return QStringLiteral("Volume Curve");
    }

    return QStringLiteral("Unknown");
}

QString qualitySourceTypeText(HydraulicNodeQualitySourceType source_type)
{
    switch (source_type)
    {
    case HydraulicNodeQualitySourceType::None:
        return QStringLiteral("None");
    case HydraulicNodeQualitySourceType::Concentration:
        return QStringLiteral("Concentration");
    case HydraulicNodeQualitySourceType::MassBooster:
        return QStringLiteral("Mass Booster");
    case HydraulicNodeQualitySourceType::FlowPacedBooster:
        return QStringLiteral("Flow-Paced Booster");
    case HydraulicNodeQualitySourceType::SetpointBooster:
        return QStringLiteral("Setpoint Booster");
    }
    return QStringLiteral("Unknown");
}

QString tankMixingModelText(HydraulicNodeTankMixingModel mixing_model)
{
    switch (mixing_model)
    {
    case HydraulicNodeTankMixingModel::CompleteMix:
        return QStringLiteral("Complete Mix");
    case HydraulicNodeTankMixingModel::TwoCompartment:
        return QStringLiteral("Two-Compartment");
    case HydraulicNodeTankMixingModel::FirstInFirstOut:
        return QStringLiteral("First In, First Out");
    case HydraulicNodeTankMixingModel::LastInFirstOut:
        return QStringLiteral("Last In, First Out");
    }
    return QStringLiteral("Unknown");
}

TableColumn qualityResultColumn(WaterQualityAnalysisType analysis)
{
    switch (analysis)
    {
    case WaterQualityAnalysisType::Chemical:
        return {QStringLiteral("Chemical Concentration [mg/L]"), true,
                {AOWIS_QUANTITY("chemical_mass_concentration")}, ColumnFilterKind::Number};
    case WaterQualityAnalysisType::WaterAge:
        return {QStringLiteral("Water Age [h]"), true,
                {AOWIS_QUANTITY("water_age")}, ColumnFilterKind::Number};
    case WaterQualityAnalysisType::SourceTrace:
        return {QStringLiteral("Source Trace [%]"), true,
                {AOWIS_QUANTITY("source_trace_percentage")}, ColumnFilterKind::Number};
    case WaterQualityAnalysisType::None:
        return {QStringLiteral("Water Quality Result"), true, {}, ColumnFilterKind::None};
    }
    return {QStringLiteral("Water Quality Result"), true, {}, ColumnFilterKind::None};
}

template<typename ResultType>
TableCell qualityResultCell(const ResultType *result, WaterQualityAnalysisType analysis)
{
    if (result == nullptr)
        return emptyCell();

    switch (analysis)
    {
    case WaterQualityAnalysisType::Chemical:
        return numberCell(result->chemical_concentration_mg_per_l, 6, QStringLiteral(" mg/L"));
    case WaterQualityAnalysisType::WaterAge:
        return numberCell(result->water_age_h, 6, QStringLiteral(" h"));
    case WaterQualityAnalysisType::SourceTrace:
        return numberCell(result->source_trace_percent, 6, QStringLiteral(" %"));
    case WaterQualityAnalysisType::None:
        return emptyCell();
    }
    return emptyCell();
}

void appendNodeQualityInputColumns(QList<TableColumn> &columns)
{
    columns.append({QStringLiteral("Initial Chemical [mg/L]"), false,
        {AOWIS_QUANTITY("chemical_mass_concentration")}, ColumnFilterKind::Number});
    columns.append({QStringLiteral("Initial Water Age [h]"), false, {AOWIS_QUANTITY("water_age")}, ColumnFilterKind::Number});
    columns.append({QStringLiteral("Quality Source Type"), false, {}, ColumnFilterKind::Choice});
    columns.append({QStringLiteral("Quality Source Concentration [mg/L]"), false,
        {AOWIS_QUANTITY("chemical_mass_concentration")}, ColumnFilterKind::Number});
    columns.append({QStringLiteral("Quality Source Mass Flow [mg/min]"), false,
        {AOWIS_QUANTITY("chemical_mass_flow_rate")}, ColumnFilterKind::Number});
    columns.append({QStringLiteral("Quality Source Pattern"), false, {}, ColumnFilterKind::Text});
}

QString pipeMaterialId(const NetworkHydraulic &network, const QUuid &uuid)
{
    if (uuid.isNull())
        return QString();

    for (const HydraulicPipeMaterial &material : network.pipe_materials)
    {
        if (material.uuid == uuid)
            return material.id;
    }
    return QString();
}

QString patternId(const NetworkHydraulic &network, const QUuid &uuid);

template<typename NodeType>
void appendNodeQualityInputCells(QList<TableCell> &cells, const NetworkHydraulic &network,
                                 const NodeType &node)
{
    cells.append(numberCell(node.initial_chemical_concentration_mg_per_l, 6,
                            QStringLiteral(" mg/L")));
    cells.append(numberCell(node.initial_water_age_h, 6, QStringLiteral(" h")));
    cells.append(textCell(qualitySourceTypeText(node.quality_source.type)));
    cells.append(numberCell(node.quality_source.chemical_concentration_mg_per_l, 6,
                            QStringLiteral(" mg/L")));
    cells.append(numberCell(node.quality_source.chemical_mass_flow_mg_per_min, 6,
                            QStringLiteral(" mg/min")));
    cells.append(textCell(patternId(network, node.quality_source.pattern_uuid)));
}

QString patternModeText(HydraulicTimePatternMode pattern_mode)
{
    return pattern_mode == HydraulicTimePatternMode::TimePattern
        ? QStringLiteral("Time Pattern") : QStringLiteral("Constant");
}

QString demandSourceMethodText(HydraulicDemandSourceMethod source_method)
{
    switch (source_method)
    {
    case HydraulicDemandSourceMethod::ManualEstimation:
        return QStringLiteral("Manual Estimation");
    case HydraulicDemandSourceMethod::MeterData:
        return QStringLiteral("Meter Data");
    case HydraulicDemandSourceMethod::Scenario:
        return QStringLiteral("Scenario");
    }

    return QStringLiteral("Unknown");
}

QString pipeInitialStatusText(HydraulicLinkPipeInitialStatus status)
{
    switch (status)
    {
    case HydraulicLinkPipeInitialStatus::Open:
        return QStringLiteral("Open");
    case HydraulicLinkPipeInitialStatus::Closed:
        return QStringLiteral("Closed");
    case HydraulicLinkPipeInitialStatus::CheckValve:
        return QStringLiteral("Check Valve");
    }

    return QStringLiteral("Unknown");
}

QString pumpDefinitionText(HydraulicLinkPumpDefinitionType definition_type)
{
    switch (definition_type)
    {
    case HydraulicLinkPumpDefinitionType::ConstantPower:
        return QStringLiteral("Constant Power");
    case HydraulicLinkPumpDefinitionType::OnePointCurve:
        return QStringLiteral("1-Point Curve");
    case HydraulicLinkPumpDefinitionType::ThreePointCurve:
        return QStringLiteral("3-Point Curve");
    case HydraulicLinkPumpDefinitionType::Library:
        return QStringLiteral("Library Curve");
    case HydraulicLinkPumpDefinitionType::MultiPointCurve:
        return QStringLiteral("Multi-Point Curve");
    }

    return QStringLiteral("Unknown");
}

QString pumpInitialStatusText(HydraulicLinkPumpInitialStatus status)
{
    return status == HydraulicLinkPumpInitialStatus::On
        ? QStringLiteral("On") : QStringLiteral("Off");
}

QString pumpControlSummary(const NetworkHydraulic &network, const QUuid &pump_uuid)
{
    int level_count = 0;
    int timer_count = 0;
    int time_of_day_count = 0;
    for (const HydraulicControlSimple &control : network.controls_simple)
    {
        if (control.link_uuid != pump_uuid)
            continue;

        switch (control.type)
        {
        case HydraulicControlSimpleType::LowLevel:
        case HydraulicControlSimpleType::HighLevel:
            level_count++;
            break;
        case HydraulicControlSimpleType::Timer:
            timer_count++;
            break;
        case HydraulicControlSimpleType::TimeOfDay:
            time_of_day_count++;
            break;
        }
    }

    int rule_count = 0;
    for (const HydraulicControlRule &rule : network.controls_rules)
    {
        bool targets_pump = false;
        for (const HydraulicControlRuleAction &action : rule.actions_then)
            targets_pump = targets_pump || action.link_uuid == pump_uuid;
        for (const HydraulicControlRuleAction &action : rule.actions_else)
            targets_pump = targets_pump || action.link_uuid == pump_uuid;
        if (targets_pump)
            rule_count++;
    }

    QStringList parts;
    if (level_count > 0)
        parts.append(QStringLiteral("Level (%1)").arg(level_count));
    if (timer_count > 0)
        parts.append(QStringLiteral("Elapsed Time (%1)").arg(timer_count));
    if (time_of_day_count > 0)
        parts.append(QStringLiteral("Time of Day (%1)").arg(time_of_day_count));
    if (rule_count > 0)
        parts.append(QStringLiteral("Rule (%1)").arg(rule_count));

    return parts.isEmpty() ? QStringLiteral("None") : parts.join(QStringLiteral(", "));
}

QString pumpEfficiencyInputText(HydraulicLinkPumpEfficiencyInputType input_type)
{
    switch (input_type)
    {
    case HydraulicLinkPumpEfficiencyInputType::Global:
        return QStringLiteral("Global");
    case HydraulicLinkPumpEfficiencyInputType::Constant:
        return QStringLiteral("Constant");
    case HydraulicLinkPumpEfficiencyInputType::Curve:
        return QStringLiteral("Curve");
    }

    return QStringLiteral("Unknown");
}

QString pumpEnergyPriceInputText(HydraulicLinkPumpEnergyPriceInputType input_type)
{
    switch (input_type)
    {
    case HydraulicLinkPumpEnergyPriceInputType::Global:
        return QStringLiteral("Global");
    case HydraulicLinkPumpEnergyPriceInputType::Constant:
        return QStringLiteral("Constant");
    case HydraulicLinkPumpEnergyPriceInputType::Pattern:
        return QStringLiteral("Pattern");
    }

    return QStringLiteral("Unknown");
}

QString valveTypeText(HydraulicLinkValveType type)
{
    switch (type)
    {
    case HydraulicLinkValveType::PRV:
        return QStringLiteral("PRV");
    case HydraulicLinkValveType::PSV:
        return QStringLiteral("PSV");
    case HydraulicLinkValveType::FCV:
        return QStringLiteral("FCV");
    case HydraulicLinkValveType::PBV:
        return QStringLiteral("PBV");
    case HydraulicLinkValveType::TCV:
        return QStringLiteral("TCV");
    case HydraulicLinkValveType::GPV:
        return QStringLiteral("GPV");
    case HydraulicLinkValveType::PCV:
        return QStringLiteral("PCV");
    }

    return QStringLiteral("Unknown");
}

QString valveInitialStatusText(HydraulicLinkValveInitialStatus status)
{
    switch (status)
    {
    case HydraulicLinkValveInitialStatus::Active:
        return QStringLiteral("Active");
    case HydraulicLinkValveInitialStatus::Open:
        return QStringLiteral("Open");
    case HydraulicLinkValveInitialStatus::Closed:
        return QStringLiteral("Closed");
    }

    return QStringLiteral("Unknown");
}

QString pumpStateText(HydraulicSimulationPumpState state)
{
    switch (state)
    {
    case HydraulicSimulationPumpState::CannotSupplyHead:
        return QStringLiteral("Cannot Supply Head");
    case HydraulicSimulationPumpState::Closed:
        return QStringLiteral("Closed");
    case HydraulicSimulationPumpState::Open:
        return QStringLiteral("Open");
    case HydraulicSimulationPumpState::CannotSupplyFlow:
        return QStringLiteral("Cannot Supply Flow");
    }

    return QStringLiteral("Unknown");
}

template<typename EntityType>
QString referencedEntityId(const QList<EntityType> &entities, const QUuid &uuid)
{
    if (uuid.isNull())
        return QString();

    for (const EntityType &entity : entities)
    {
        if (entity.uuid == uuid)
            return entity.id.isEmpty() ? uuid.toString(QUuid::WithoutBraces) : entity.id;
    }

    return uuid.toString(QUuid::WithoutBraces);
}

template<typename ResultType>
QHash<QUuid, const ResultType *> simulationResultLookup(const QList<ResultType> &results)
{
    QHash<QUuid, const ResultType *> lookup;
    lookup.reserve(results.size());
    for (const ResultType &result : results)
        lookup.insert(result.uuid, &result);
    return lookup;
}

QHash<QUuid, const HydraulicSimulationResultLinkPumpEnergyUsage *> pumpEnergyUsageLookup(
    const QList<HydraulicSimulationResultLinkPumpEnergyUsage> &results)
{
    QHash<QUuid, const HydraulicSimulationResultLinkPumpEnergyUsage *> lookup;
    lookup.reserve(results.size());
    for (const HydraulicSimulationResultLinkPumpEnergyUsage &result : results)
        lookup.insert(result.pump_uuid, &result);
    return lookup;
}

template<typename EntityType>
void appendEntityIds(QHash<QUuid, QString> &lookup, const QList<EntityType> &entities)
{
    for (const EntityType &entity : entities)
    {
        lookup.insert(entity.uuid, entity.id.isEmpty()
            ? entity.uuid.toString(QUuid::WithoutBraces) : entity.id);
    }
}

QHash<QUuid, QString> nodeIdLookup(const NetworkHydraulic &network)
{
    QHash<QUuid, QString> lookup;
    lookup.reserve(network.nodes_junctions.size() + network.nodes_reservoirs.size()
                   + network.nodes_tanks.size());
    appendEntityIds(lookup, network.nodes_junctions);
    appendEntityIds(lookup, network.nodes_reservoirs);
    appendEntityIds(lookup, network.nodes_tanks);
    return lookup;
}

QString nodeId(const QHash<QUuid, QString> &lookup, const QUuid &uuid)
{
    if (uuid.isNull())
        return QString();

    const QHash<QUuid, QString>::const_iterator iterator = lookup.constFind(uuid);
    if (iterator != lookup.constEnd())
        return iterator.value();
    return uuid.toString(QUuid::WithoutBraces);
}

QString patternId(const NetworkHydraulic &network, const QUuid &uuid)
{
    return referencedEntityId(network.patterns_time, uuid);
}

QString tankVolumeCurveId(const NetworkHydraulic &network, const QUuid &uuid)
{
    return referencedEntityId(network.curves_tank_volume, uuid);
}

QString pumpHeadCurveId(const NetworkHydraulic &network, const QUuid &uuid)
{
    return referencedEntityId(network.curves_pump_head, uuid);
}

QString pumpEfficiencyCurveId(const NetworkHydraulic &network, const QUuid &uuid)
{
    return referencedEntityId(network.curves_pump_efficiency, uuid);
}

TableCell valveSettingCell(const HydraulicLinkValve &valve)
{
    switch (valve.type)
    {
    case HydraulicLinkValveType::PRV:
    case HydraulicLinkValveType::PSV:
    case HydraulicLinkValveType::PBV:
        return quantityCell(valve.setting_pressure_head_m, 3, AOWIS_QUANTITY("pressure_head"), QStringLiteral(" m"));
    case HydraulicLinkValveType::FCV:
        return quantityCell(valve.setting_flow_m3_per_h, 3, AOWIS_QUANTITY("volumetric_flow_rate"), QStringLiteral(" m³/h"));
    case HydraulicLinkValveType::TCV:
        return numberCell(valve.setting_loss_coefficient, 6);
    case HydraulicLinkValveType::PCV:
        return quantityCell(valve.setting_position_percent, 2, AOWIS_QUANTITY("valve_position"), QStringLiteral(" %"));
    case HydraulicLinkValveType::GPV:
        return emptyCell();
    }

    return emptyCell();
}

TableCell valveResultSettingCell(const HydraulicSimulationResultLinkValve &valve)
{
    switch (valve.type)
    {
    case HydraulicLinkValveType::PRV:
    case HydraulicLinkValveType::PSV:
    case HydraulicLinkValveType::PBV:
        return quantityCell(valve.setting_pressure_head_m, 3, AOWIS_QUANTITY("pressure_head"), QStringLiteral(" m"));
    case HydraulicLinkValveType::FCV:
        return quantityCell(valve.setting_flow_m3_per_h, 3, AOWIS_QUANTITY("volumetric_flow_rate"), QStringLiteral(" m³/h"));
    case HydraulicLinkValveType::TCV:
        return numberCell(valve.setting_loss_coefficient, 6);
    case HydraulicLinkValveType::PCV:
        return quantityCell(valve.setting_position_percent, 2, AOWIS_QUANTITY("valve_position"), QStringLiteral(" %"));
    case HydraulicLinkValveType::GPV:
        return emptyCell();
    }

    return emptyCell();
}

QString valveSettingCurveId(const NetworkHydraulic &network, const HydraulicLinkValve &valve)
{
    if (valve.type == HydraulicLinkValveType::GPV)
        return referencedEntityId(network.curves_valve_headloss, valve.head_loss_curve_uuid);
    if (valve.type == HydraulicLinkValveType::PCV)
        return referencedEntityId(network.curves_valve_characteristic, valve.characteristic_curve_uuid);

    return QString();
}

QList<DemandDetail> junctionDemandDetails(const NetworkHydraulic &network,
                                         const QList<HydraulicDemand> &demands)
{
    QList<DemandDetail> details;
    details.reserve(demands.size());
    for (const HydraulicDemand &demand : demands)
    {
        DemandDetail detail;
        if (!demand.category_name.isEmpty())
            detail.prefix = demand.category_name + QStringLiteral(": ");
        detail.canonical_rate = demand.base_demand_m3_per_h;
        detail.suffix = QStringLiteral(", ") + patternModeText(demand.pattern_mode);
        if (demand.pattern_mode == HydraulicTimePatternMode::TimePattern)
        {
            const QString id = patternId(network, demand.pattern_uuid);
            if (!id.isEmpty())
                detail.suffix += QStringLiteral(" ") + id;
        }
        detail.suffix += QStringLiteral(", ") + demandSourceMethodText(demand.source_method);
        if (!demand.note.isEmpty())
            detail.suffix += QStringLiteral(", ") + demand.note;
        details.append(std::move(detail));
    }
    return details;
}

QString demandDetailsText(const QList<DemandDetail> &details,
                          const UnitPresentationContext *context = nullptr,
                          std::string_view quantity = AOWIS_QUANTITY("base_demand_flow_rate"))
{
    QStringList parts;
    for (const DemandDetail &detail : details)
    {
        const QString rate = context == nullptr
            ? formatNumber(detail.canonical_rate, 3, QStringLiteral(" m³/h"))
            : UnitTablePresentation::formatDemandRate(*context, detail.canonical_rate, 3, quantity);
        parts.append(detail.prefix + rate + detail.suffix);
    }
    return parts.join(QStringLiteral(" | "));
}

double junctionBaseDemand(const QList<HydraulicDemand> &demands)
{
    double total = 0.0;
    for (const HydraulicDemand &demand : demands)
        total += demand.base_demand_m3_per_h;
    return total;
}

QString balancedHeaderWords(const QStringList &words, int line_count)
{
    if (words.isEmpty())
        return QString();
    if (line_count <= 1 || words.size() == 1)
        return words.join(QLatin1Char(' '));

    if (line_count == 2)
    {
        qsizetype best_split = 1;
        qsizetype best_maximum_length = words.join(QLatin1Char(' ')).size();
        for (qsizetype split = 1; split < words.size(); ++split)
        {
            const qsizetype first_length = words.mid(0, split).join(QLatin1Char(' ')).size();
            const qsizetype second_length = words.mid(split).join(QLatin1Char(' ')).size();
            const qsizetype maximum_length = qMax(first_length, second_length);
            if (maximum_length < best_maximum_length)
            {
                best_maximum_length = maximum_length;
                best_split = split;
            }
        }

        return words.mid(0, best_split).join(QLatin1Char(' ')) + QLatin1Char('\n')
            + words.mid(best_split).join(QLatin1Char(' '));
    }

    qsizetype best_first_split = 1;
    qsizetype best_second_split = 2;
    qsizetype best_maximum_length = words.join(QLatin1Char(' ')).size();
    for (qsizetype first_split = 1; first_split < words.size() - 1; ++first_split)
    {
        for (qsizetype second_split = first_split + 1; second_split < words.size();
             ++second_split)
        {
            const qsizetype first_length = words.mid(0, first_split)
                                               .join(QLatin1Char(' ')).size();
            const qsizetype second_length = words.mid(first_split,
                                                       second_split - first_split)
                                                .join(QLatin1Char(' ')).size();
            const qsizetype third_length = words.mid(second_split)
                                               .join(QLatin1Char(' ')).size();
            const qsizetype maximum_length = qMax(first_length,
                                                   qMax(second_length, third_length));
            if (maximum_length < best_maximum_length)
            {
                best_maximum_length = maximum_length;
                best_first_split = first_split;
                best_second_split = second_split;
            }
        }
    }

    return words.mid(0, best_first_split).join(QLatin1Char(' ')) + QLatin1Char('\n')
        + words.mid(best_first_split, best_second_split - best_first_split)
              .join(QLatin1Char(' '))
        + QLatin1Char('\n') + words.mid(best_second_split).join(QLatin1Char(' '));
}

QString compactHeaderText(const QString &title)
{
    QString base = title;
    QString unit;
    const qsizetype unit_separator = title.lastIndexOf(QStringLiteral(" ["));
    if (unit_separator > 0)
    {
        base = title.left(unit_separator);
        unit = title.mid(unit_separator + 1);
    }

    const QStringList words = base.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    const int available_base_lines = unit.isEmpty() ? 3 : 2;
    const int base_line_count = qMin(available_base_lines, words.size());
    QString text = balancedHeaderWords(words, base_line_count);
    if (!unit.isEmpty())
        text += QLatin1Char('\n') + unit;
    return text;
}

void appendCommonColumns(QList<TableColumn> &columns)
{
    columns.append({QStringLiteral("ID"), false, {}, ColumnFilterKind::Text});
    columns.append({QStringLiteral("Enabled"), false, {}, ColumnFilterKind::Choice});
    columns.append({QStringLiteral("Model Role"), false, {}, ColumnFilterKind::Choice});
    columns.append({QStringLiteral("Date Added"), false, {}, ColumnFilterKind::None});
    columns.append({QStringLiteral("Date Installed"), false, {}, ColumnFilterKind::None});
    columns.append({QStringLiteral("Description"), false, {}, ColumnFilterKind::Text});
    columns.append({QStringLiteral("Tags"), false, {}, ColumnFilterKind::Text});
    columns.append({QStringLiteral("Comment"), false, {}, ColumnFilterKind::Text});
}

void appendCommonCells(QList<TableCell> &cells, const QString &id,
                       const HydraulicEntityMetadata &metadata)
{
    cells.append(textCell(id));
    cells.append(boolCell(metadata.enabled));
    cells.append(textCell(entityModelRoleText(metadata.model_role)));
    cells.append(dateCell(metadata.date_added));
    cells.append(dateCell(metadata.date_installed));
    cells.append(textCell(metadata.description));
    cells.append(textCell(metadata.tags.join(QStringLiteral(", "))));
    cells.append(textCell(metadata.comment));
}

void appendNodePositionColumns(QList<TableColumn> &columns)
{
    columns.append({QStringLiteral("Latitude [°]"), false, {}, ColumnFilterKind::Number});
    columns.append({QStringLiteral("Longitude [°]"), false, {}, ColumnFilterKind::Number});
}

void appendNodePositionCells(QList<TableCell> &cells, const CoordinateWGS84 &coordinate)
{
    cells.append(numberCell(coordinate.latitude_deg, 6));
    cells.append(numberCell(coordinate.longitude_deg, 6));
}

bool isErrorSeverity(HydraulicSimulationDiagnosticSeverity severity)
{
    return severity == HydraulicSimulationDiagnosticSeverity::Error
        || severity == HydraulicSimulationDiagnosticSeverity::Fatal;
}

InfrastructureEntity diagnosticEntityType(const HydraulicData *hydraulic_data,
                                          HydraulicSimulationStatusEntityType type,
                                          const QUuid &uuid)
{
    switch (type)
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
    case HydraulicSimulationStatusEntityType::DemandPoint:
        return InfrastructureEntity::DemandPoint;
    case HydraulicSimulationStatusEntityType::Node:
        if (hydraulic_data->junction(uuid).has_value())
            return InfrastructureEntity::Junction;
        if (hydraulic_data->reservoir(uuid).has_value())
            return InfrastructureEntity::Reservoir;
        if (hydraulic_data->tank(uuid).has_value())
            return InfrastructureEntity::Tank;
        break;
    case HydraulicSimulationStatusEntityType::Link:
        if (hydraulic_data->pipe(uuid).has_value())
            return InfrastructureEntity::Pipe;
        if (hydraulic_data->pump(uuid).has_value())
            return InfrastructureEntity::Pump;
        if (hydraulic_data->valve(uuid).has_value())
            return InfrastructureEntity::Valve;
        break;
    default:
        break;
    }

    return InfrastructureEntity::Unknown;
}

QHash<QUuid, InfrastructureEntity> simulationWarningEntities(
    const HydraulicData *hydraulic_data)
{
    QHash<QUuid, InfrastructureEntity> entities;
    const std::optional<HydraulicSimulationResultTimeline> &timeline =
        hydraulic_data->simulationResultTimeline();
    if (!timeline.has_value())
        return entities;

    for (const HydraulicSimulationDiagnostic &diagnostic : timeline->diagnostics)
    {
        if (diagnostic.severity != HydraulicSimulationDiagnosticSeverity::Warning
            || diagnostic.entity.uuid.isNull())
        {
            continue;
        }

        const InfrastructureEntity entity_type = diagnosticEntityType(
            hydraulic_data, diagnostic.entity.type, diagnostic.entity.uuid);
        if (entity_type != InfrastructureEntity::Unknown)
            entities.insert(diagnostic.entity.uuid, entity_type);
    }

    return entities;
}

QHash<QUuid, QString> simulationWarningTooltips(const HydraulicData *hydraulic_data)
{
    QHash<QUuid, QStringList> messages;
    const std::optional<HydraulicSimulationResultTimeline> &timeline =
        hydraulic_data->simulationResultTimeline();
    if (!timeline.has_value())
        return QHash<QUuid, QString>();

    for (const HydraulicSimulationDiagnostic &diagnostic : timeline->diagnostics)
    {
        if (diagnostic.severity != HydraulicSimulationDiagnosticSeverity::Warning
            || diagnostic.entity.uuid.isNull())
        {
            continue;
        }

        QString message = diagnostic.message;
        if (message.isEmpty())
            message = diagnostic.message_backend;
        if (message.isEmpty())
            message = QStringLiteral("Simulation warning for this entity.");

        messages[diagnostic.entity.uuid].append(message);
    }

    QHash<QUuid, QString> tooltips;
    QHash<QUuid, QStringList>::const_iterator iterator = messages.constBegin();
    while (iterator != messages.constEnd())
    {
        tooltips.insert(iterator.key(), QStringLiteral("Simulation warning:\n")
                                      + iterator.value().join(QStringLiteral("\n")));
        ++iterator;
    }
    return tooltips;
}

QHash<QUuid, QString> simulationErrorTooltips(const HydraulicData *hydraulic_data)
{
    QHash<QUuid, QStringList> messages;
    const std::optional<HydraulicSimulationResultTimeline> &timeline =
        hydraulic_data->simulationResultTimeline();
    if (!timeline.has_value())
        return QHash<QUuid, QString>();

    for (const HydraulicSimulationDiagnostic &diagnostic : timeline->diagnostics)
    {
        if (!isErrorSeverity(diagnostic.severity) || diagnostic.entity.uuid.isNull())
            continue;

        QString message = diagnostic.message;
        if (message.isEmpty())
            message = diagnostic.message_backend;
        if (message.isEmpty())
            message = QStringLiteral("Simulation failed for this entity.");

        messages[diagnostic.entity.uuid].append(message);
    }

    const HydraulicSimulationStatus *status = hydraulic_data->simulationStatus();
    if (status != nullptr && !status->success && !status->entity.uuid.isNull())
    {
        QString message = status->message;
        if (message.isEmpty())
            message = status->message_backend;
        if (message.isEmpty())
            message = QStringLiteral("Simulation failed for this entity.");

        if (!messages[status->entity.uuid].contains(message))
            messages[status->entity.uuid].append(message);
    }

    QHash<QUuid, QString> tooltips;
    QHash<QUuid, QStringList>::const_iterator iterator = messages.constBegin();
    while (iterator != messages.constEnd())
    {
        tooltips.insert(iterator.key(), QStringLiteral("Simulation error:\n")
                                      + iterator.value().join(QStringLiteral("\n")));
        ++iterator;
    }
    return tooltips;
}
}

class HydraulicEntityTableModel : public QAbstractTableModel
{
public:
    explicit HydraulicEntityTableModel(HydraulicData *hydraulic_data,
                                       InfrastructureEntity entity_type,
                                       QObject *parent = nullptr)
        : QAbstractTableModel(parent),
          hydraulic_data(hydraulic_data),
          entity_type(entity_type)
    {
        rebuild();
    }

    void refreshPresentation()
    {
        if (!this->columns.isEmpty())
            emit headerDataChanged(Qt::Horizontal, 0, this->columns.size() - 1);
        // No dataChanged() or reset: the table viewport repaints its visible cells.
        // SortRole and numeric filter values stay in canonical units.
    }

    int rowCount(const QModelIndex &parent = QModelIndex()) const override
    {
        if (parent.isValid())
            return 0;
        return this->rows.size();
    }

    int columnCount(const QModelIndex &parent = QModelIndex()) const override
    {
        if (parent.isValid())
            return 0;
        return this->columns.size();
    }

    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override
    {
        if (!index.isValid() || index.row() < 0 || index.row() >= this->rows.size()
            || index.column() < 0 || index.column() >= this->columns.size())
        {
            return QVariant();
        }

        const TableRow &row = this->rows.at(index.row());
        const TableCell &cell = row.cells.at(index.column());

        switch (role)
        {
        case Qt::DisplayRole:
        {
            const UnitPresentationContext &context = UnitProfileManager::instance().presentation();
            const UnitTablePresentation::ColumnUnits &column_units = this->columns.at(index.column()).units;
            if (column_units.kind == UnitTablePresentation::ColumnKind::DemandDetails && !row.demand_details.isEmpty())
                return demandDetailsText(
                    row.demand_details, &context,
                    this->entity_type == InfrastructureEntity::DemandPoint
                        ? AOWIS_QUANTITY("customer_demand_flow_rate") : AOWIS_QUANTITY("base_demand_flow_rate"));
            if (cell.reaction_kind.has_value() && cell.sort_value.isValid())
                return UnitTablePresentation::formatReactionCoefficient(
                    context, *cell.reaction_kind, cell.reaction_order,
                    cell.sort_value.toDouble(), cell.decimals);
            if (column_units.kind == UnitTablePresentation::ColumnKind::EnergyPrice
                && cell.sort_value.isValid())
                return UnitTablePresentation::formatEnergyPrice(context, cell.sort_value.toDouble());
            const std::string_view quantity = cell.quantity.empty()
                ? column_units.quantity
                : cell.quantity;
            if (!quantity.empty() && cell.sort_value.isValid()
                && cell.sort_value.userType() == QMetaType::Double)
            {
                const double canonical = cell.sort_value.toDouble();
                if (const std::optional<double> displayed = UnitTablePresentation::toDisplay(
                        context, quantity, canonical))
                {
                    const QString unit = UnitTablePresentation::unitSymbol(
                        UnitTablePresentation::selectedUnit(
                            context, quantity));
                    return UnitTablePresentation::formatNumber(
                        context, *displayed, cell.decimals, QStringLiteral(" ") + unit,
                        quantity);
                }
            }
            if (cell.sort_value.isValid())
            {
                if (cell.sort_value.userType() == QMetaType::Double && quantity.empty())
                    return UnitTablePresentation::formatNumber(
                        context, cell.sort_value.toDouble(), cell.decimals, cell.canonical_suffix);
                if (cell.sort_value.userType() == QMetaType::Int)
                    return UnitTablePresentation::formatCount(context, cell.sort_value.toInt());
            }
            return cell.display;
        }
        case ReactionOrderRole:
            return cell.reaction_kind.has_value() ? QVariant(cell.reaction_order) : QVariant();
        case ValveSettingGroupRole:
            if (this->columns.at(index.column()).units.kind
                    == UnitTablePresentation::ColumnKind::RowQuantity
                && row.valve_type.has_value())
            {
                return static_cast<int>(ValveSettingSortPolicy::groupForType(*row.valve_type));
            }
            return QVariant();
        case SortRole:
            return cell.sort_value;
        case EntityUuidRole:
            return row.uuid;
        case SimulationStatusRole:
        {
            int status = static_cast<int>(SimulationRowStatus::None);
            if (row.simulation_warning)
                status |= static_cast<int>(SimulationRowStatus::Warning);
            if (row.simulation_error)
                status |= static_cast<int>(SimulationRowStatus::Error);
            return status;
        }
        case Qt::TextAlignmentRole:
            return static_cast<int>(cell.alignment);
        case Qt::ToolTipRole:
            if (row.simulation_error && !row.simulation_error_tooltip.isEmpty()
                && row.simulation_warning && !row.simulation_warning_tooltip.isEmpty())
            {
                return row.simulation_error_tooltip + QStringLiteral("\n\n")
                    + row.simulation_warning_tooltip;
            }
            if (row.simulation_error && !row.simulation_error_tooltip.isEmpty())
                return row.simulation_error_tooltip;
            if (row.simulation_warning && !row.simulation_warning_tooltip.isEmpty())
                return row.simulation_warning_tooltip;
            if (cell.reaction_kind.has_value())
                return UnitTablePresentation::reactionCellTooltip(
                    UnitProfileManager::instance().presentation(),
                    *cell.reaction_kind, cell.reaction_order);
            return cell.tooltip;
        case Qt::BackgroundRole:
            if (row.simulation_error)
                return QBrush(QColor(210, 45, 45, 70));
            if (row.simulation_warning)
                return QBrush(QColor(220, 155, 40, 55));
            if (this->columns.at(index.column()).simulation_result)
                return QBrush(QColor(70, 135, 210, 25));
            return QVariant();
        case Qt::FontRole:
            if (index.column() == 0)
            {
                QFont font;
                font.setBold(true);
                return font;
            }
            return QVariant();
        default:
            return QVariant();
        }
    }

    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override
    {
        if (orientation == Qt::Vertical)
            return QVariant();
        if (section < 0 || section >= this->columns.size())
            return QVariant();

        const TableColumn &column = this->columns.at(section);
        if (role == Qt::DisplayRole)
            return compactHeaderText(UnitTablePresentation::formattedTitle(
                columnTitle(section), UnitProfileManager::instance().presentation(), column.units));
        if (role == Qt::ToolTipRole)
        {
            const QString title = UnitTablePresentation::formattedTitle(
                columnTitle(section), UnitProfileManager::instance().presentation(), column.units);
            if (column.units.kind == UnitTablePresentation::ColumnKind::BulkReaction)
                return UnitTablePresentation::reactionColumnTooltip(
                    UnitProfileManager::instance().presentation(),
                    UnitTablePresentation::ReactionKind::Bulk);
            if (column.units.kind == UnitTablePresentation::ColumnKind::WallReaction)
                return UnitTablePresentation::reactionColumnTooltip(
                    UnitProfileManager::instance().presentation(),
                    UnitTablePresentation::ReactionKind::Wall);
            if (column.units.kind == UnitTablePresentation::ColumnKind::RowQuantity)
            {
                return title + QStringLiteral(
                    "\nValve settings have different physical units. Sorting groups "
                    "compatible quantities and orders their canonical values within "
                    "each group. Use Valve Type to filter by type; a single numeric "
                    "range would mix incompatible units.")
                    + (column.simulation_result ? QStringLiteral("\nSimulation result.")
                                                : QString());
            }
            if (column.simulation_result)
                return title + QStringLiteral("\nSimulation result.");
            return title;
        }
        if (role == Qt::BackgroundRole && column.simulation_result)
            return QBrush(QColor(70, 135, 210, 55));
        if (role == Qt::FontRole && column.simulation_result)
        {
            QFont font;
            font.setBold(true);
            return font;
        }

        return QVariant();
    }

    Qt::ItemFlags flags(const QModelIndex &index) const override
    {
        if (!index.isValid())
            return Qt::NoItemFlags;
        return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    }

    ColumnFilterKind filterKind(int column) const
    {
        if (column < 0 || column >= this->columns.size())
            return ColumnFilterKind::None;
        return this->columns.at(column).filter_kind;
    }

    QString columnTitle(int column) const
    {
        if (column < 0 || column >= this->columns.size())
            return QString();
        const QString &source = this->columns.at(column).title;
        const qsizetype suffix = source.lastIndexOf(QStringLiteral(" ["));
        const bool has_unit = suffix >= 0 && source.endsWith(QLatin1Char(']'));
        const QString caption = has_unit ? source.left(suffix) : source;
        return QCoreApplication::translate("HydraulicTableColumns", caption.toUtf8().constData())
            + (has_unit ? source.mid(suffix) : QString());
    }

    UnitTablePresentation::ColumnUnits columnUnits(int column) const
    {
        if (column < 0 || column >= this->columns.size())
            return {};
        return this->columns.at(column).units;
    }

    bool columnNumericRange(int column, double &minimum, double &maximum) const
    {
        if (column < 0 || column >= this->columns.size())
            return false;

        bool found_value = false;
        for (const TableRow &row : this->rows)
        {
            if (column >= row.cells.size())
                continue;

            bool ok = false;
            const double value = row.cells.at(column).sort_value.toDouble(&ok);
            if (!ok || !std::isfinite(value))
                continue;

            if (!found_value)
            {
                minimum = value;
                maximum = value;
                found_value = true;
            }
            else
            {
                minimum = qMin(minimum, value);
                maximum = qMax(maximum, value);
            }
        }

        return found_value;
    }

    void rebuild()
    {
        beginResetModel();
        this->columns.clear();
        this->rows.clear();

        const NetworkHydraulic &network = this->hydraulic_data->networkHydraulic();
        const HydraulicSimulationResult *simulation_result =
            this->hydraulic_data->currentSimulationResult();
        QHash<QUuid, InfrastructureEntity> error_entities =
            this->hydraulic_data->simulationErrorEntities();
        const QUuid status_error_uuid = this->hydraulic_data->simulationErrorEntityUuid();
        const InfrastructureEntity status_error_type =
            this->hydraulic_data->simulationErrorEntityType();
        if (!status_error_uuid.isNull() && status_error_type != InfrastructureEntity::Unknown)
            error_entities.insert(status_error_uuid, status_error_type);

        const QHash<QUuid, QString> error_tooltips =
            simulationErrorTooltips(this->hydraulic_data);
        this->warning_entities = simulationWarningEntities(this->hydraulic_data);
        this->warning_tooltips = simulationWarningTooltips(this->hydraulic_data);

        switch (this->entity_type)
        {
        case InfrastructureEntity::DemandPoint:
            buildDemandPoints(network, error_entities, error_tooltips);
            break;
        case InfrastructureEntity::Junction:
            buildJunctions(network, simulation_result, error_entities, error_tooltips);
            break;
        case InfrastructureEntity::Reservoir:
            buildReservoirs(network, simulation_result, error_entities, error_tooltips);
            break;
        case InfrastructureEntity::Tank:
            buildTanks(network, simulation_result, error_entities, error_tooltips);
            break;
        case InfrastructureEntity::Pipe:
            buildPipes(network, simulation_result, error_entities, error_tooltips);
            break;
        case InfrastructureEntity::Pump:
            buildPumps(network, simulation_result, error_entities, error_tooltips);
            break;
        case InfrastructureEntity::Valve:
            buildValves(network, simulation_result, error_entities, error_tooltips);
            break;
        default:
            break;
        }

        endResetModel();
    }

private:
    void finishRow(TableRow &row,
                   const QHash<QUuid, InfrastructureEntity> &error_entities,
                   const QHash<QUuid, QString> &error_tooltips)
    {
        row.simulation_error = error_entities.value(row.uuid, InfrastructureEntity::Unknown)
            == this->entity_type;
        row.simulation_warning =
            this->warning_entities.value(row.uuid, InfrastructureEntity::Unknown)
                == this->entity_type;
        row.simulation_error_tooltip = error_tooltips.value(row.uuid);
        row.simulation_warning_tooltip = this->warning_tooltips.value(row.uuid);
        this->rows.append(row);
    }

    void buildDemandPoints(
        const NetworkHydraulic &network,
        const QHash<QUuid, InfrastructureEntity> &error_entities,
        const QHash<QUuid, QString> &error_tooltips)
    {
        appendCommonColumns(this->columns);
        appendNodePositionColumns(this->columns);
        this->columns.append({QStringLiteral("Demand Count"), false, {}, ColumnFilterKind::Integer});
        this->columns.append({QStringLiteral("Customer Demand Base [m³/h]"), false,
            {AOWIS_QUANTITY("customer_demand_flow_rate")}, ColumnFilterKind::Number});
        this->columns.append({QStringLiteral("Demand Details"), false,
            {{}, UnitTablePresentation::ColumnKind::DemandDetails}, ColumnFilterKind::Text});
        this->columns.append({QStringLiteral("Attachment Type"), false, {}, ColumnFilterKind::Choice});
        this->columns.append({QStringLiteral("Attachment Target"), false, {}, ColumnFilterKind::Text});
        this->columns.append({QStringLiteral("Pipe Position [%]"), false, {}, ColumnFilterKind::Number});
        this->columns.append({QStringLiteral("Metered"), false, {}, ColumnFilterKind::Choice});
        this->columns.append({QStringLiteral("Meter ID"), false, {}, ColumnFilterKind::Text});

        for (const HydraulicDemandPoint &demand_point : network.demand_points)
        {
            TableRow row;
            row.uuid = demand_point.uuid;
            appendCommonCells(row.cells, demand_point.id, demand_point.metadata);
            appendNodePositionCells(row.cells, demand_point.coordinate_wgs84);
            row.cells.append(integerCell(demand_point.demands.size()));
            row.cells.append(numberCell(junctionBaseDemand(demand_point.demands), 3,
                                        QStringLiteral(" m³/h")));
            row.demand_details = junctionDemandDetails(network, demand_point.demands);
            row.cells.append(textCell(demandDetailsText(row.demand_details)));

            QString attachment_type = QStringLiteral("Unattached");
            QString attachment_target;
            TableCell pipe_position = emptyCell();
            if (demand_point.attachment.type == HydraulicDemandPointAttachmentType::Pipe)
            {
                attachment_type = QStringLiteral("Pipe");
                attachment_target = referencedEntityId(
                    network.links_pipes, demand_point.attachment.pipe_uuid);
                pipe_position = numberCell(demand_point.attachment.pipe_position * 100.0, 2,
                                           QStringLiteral(" %"));
            }
            else if (demand_point.attachment.type ==
                     HydraulicDemandPointAttachmentType::Junction)
            {
                attachment_type = QStringLiteral("Junction");
                attachment_target = referencedEntityId(
                    network.nodes_junctions, demand_point.attachment.junction_uuid);
            }

            row.cells.append(textCell(attachment_type));
            row.cells.append(textCell(attachment_target));
            row.cells.append(pipe_position);
            row.cells.append(boolCell(demand_point.meter.has_value()));
            row.cells.append(demand_point.meter.has_value()
                                 ? textCell(demand_point.meter->id)
                                 : emptyCell());
            finishRow(row, error_entities, error_tooltips);
        }
    }

    void buildJunctions(const NetworkHydraulic &network,
                        const HydraulicSimulationResult *simulation_result,
                        const QHash<QUuid, InfrastructureEntity> &error_entities,
                        const QHash<QUuid, QString> &error_tooltips)
    {
        const QHash<QUuid, const HydraulicSimulationResultNodeJunction *> result_lookup =
            simulation_result == nullptr
                ? QHash<QUuid, const HydraulicSimulationResultNodeJunction *>()
                : simulationResultLookup(simulation_result->nodes_junctions);

        const std::optional<WaterQualitySimulationResultTimeline> &quality_timeline =
            this->hydraulic_data->waterQualitySimulationResultTimeline();
        const WaterQualityAnalysisType quality_analysis = quality_timeline.has_value()
            ? quality_timeline->analysis : WaterQualityAnalysisType::None;
        const WaterQualitySimulationResult *quality_result =
            this->hydraulic_data->currentWaterQualitySimulationResult();
        const QHash<QUuid, const WaterQualitySimulationResultNodeJunction *> quality_lookup =
            quality_result == nullptr
                ? QHash<QUuid, const WaterQualitySimulationResultNodeJunction *>()
                : simulationResultLookup(quality_result->nodes_junctions);

        appendCommonColumns(this->columns);
        appendNodePositionColumns(this->columns);
        this->columns.append({QStringLiteral("Elevation Input"), false, {}, ColumnFilterKind::Choice});
        this->columns.append({QStringLiteral("Elevation Direct [m]"), false, {AOWIS_QUANTITY("elevation")}, ColumnFilterKind::Number});
        this->columns.append({QStringLiteral("Elevation Terrain [m]"), false, {AOWIS_QUANTITY("elevation")}, ColumnFilterKind::Number});
        this->columns.append({QStringLiteral("Elevation Offset [m]"), false,
            {AOWIS_QUANTITY("vertical_offset")}, ColumnFilterKind::Number});
        this->columns.append({QStringLiteral("Elevation Resolved [m]"), false,
            {AOWIS_QUANTITY("elevation")}, ColumnFilterKind::Number});
        this->columns.append({QStringLiteral("Demand Count"), false, {}, ColumnFilterKind::Integer});
        this->columns.append({QStringLiteral("Demand Base [m³/h]"), false,
            {AOWIS_QUANTITY("base_demand_flow_rate")}, ColumnFilterKind::Number});
        this->columns.append({QStringLiteral("Demand Details"), false,
            {{}, UnitTablePresentation::ColumnKind::DemandDetails}, ColumnFilterKind::Text});
        this->columns.append({QStringLiteral("Emitter Coefficient"), false, {}, ColumnFilterKind::Number});
        this->columns.append({QStringLiteral("Emitter Exponent"), false, {}, ColumnFilterKind::Number});
        this->columns.append({QStringLiteral("Demand Requested [m³/h]"), true,
            {AOWIS_QUANTITY("volumetric_flow_rate")}, ColumnFilterKind::Number});
        this->columns.append({QStringLiteral("Demand Delivered [m³/h]"), true,
            {AOWIS_QUANTITY("volumetric_flow_rate")}, ColumnFilterKind::Number});
        this->columns.append({QStringLiteral("Demand Deficit [m³/h]"), true,
            {AOWIS_QUANTITY("volumetric_flow_rate")}, ColumnFilterKind::Number});
        this->columns.append({QStringLiteral("Demand Total [m³/h]"), true,
            {AOWIS_QUANTITY("volumetric_flow_rate")}, ColumnFilterKind::Number});
        this->columns.append({QStringLiteral("Flow Emitter [m³/h]"), true,
            {AOWIS_QUANTITY("volumetric_flow_rate")}, ColumnFilterKind::Number});
        this->columns.append({QStringLiteral("Flow Leakage [m³/h]"), true,
            {AOWIS_QUANTITY("volumetric_flow_rate")}, ColumnFilterKind::Number});
        this->columns.append({QStringLiteral("Head [m]"), true, {AOWIS_QUANTITY("hydraulic_head")}, ColumnFilterKind::Number});
        this->columns.append({QStringLiteral("Pressure [m]"), true, {AOWIS_QUANTITY("pressure_head")}, ColumnFilterKind::Number});
        this->columns.append({QStringLiteral("Referenced by Control"), true, {}, ColumnFilterKind::Choice});
        appendNodeQualityInputColumns(this->columns);
        this->columns.append(qualityResultColumn(quality_analysis));
        if (quality_analysis == WaterQualityAnalysisType::Chemical)
            this->columns.append({QStringLiteral("Quality Source Mass Flow [mg/min]"), true,
                {AOWIS_QUANTITY("chemical_mass_flow_rate")}, ColumnFilterKind::Number});

        for (const HydraulicNodeJunction &junction : network.nodes_junctions)
        {
            TableRow row;
            row.uuid = junction.uuid;
            appendCommonCells(row.cells, junction.id, junction.metadata);
            appendNodePositionCells(row.cells, junction.coordinate_wgs84);
            row.cells.append(textCell(elevationInputText(junction.elevation_input_type)));
            row.cells.append(numberCell(junction.elevation_m, 3, QStringLiteral(" m")));
            row.cells.append(numberCell(junction.terrain_elevation_m, 3, QStringLiteral(" m")));
            row.cells.append(numberCell(junction.elevation_offset_m, 3, QStringLiteral(" m")));
            row.cells.append(numberCell(resolvedSymbologyElevationM(junction), 3, QStringLiteral(" m")));
            row.cells.append(integerCell(junction.demands.size()));
            row.cells.append(numberCell(junctionBaseDemand(junction.demands), 3, QStringLiteral(" m³/h")));
            row.demand_details = junctionDemandDetails(network, junction.demands);
            row.cells.append(textCell(demandDetailsText(row.demand_details)));
            row.cells.append(numberCell(junction.emitter.coefficient, 6));
            row.cells.append(numberCell(junction.emitter.pressure_exponent, 6));

            const HydraulicSimulationResultNodeJunction *result =
                result_lookup.value(junction.uuid, nullptr);
            appendJunctionSimulationCells(row.cells, result);
            appendNodeQualityInputCells(row.cells, network, junction);
            const WaterQualitySimulationResultNodeJunction *quality_entity_result =
                quality_lookup.value(junction.uuid, nullptr);
            row.cells.append(qualityResultCell(quality_entity_result, quality_analysis));
            if (quality_analysis == WaterQualityAnalysisType::Chemical)
            {
                row.cells.append(quality_entity_result == nullptr
                    ? emptyCell()
                    : numberCell(quality_entity_result->source_mass_flow_mg_per_min, 6,
                                 QStringLiteral(" mg/min")));
            }
            finishRow(row, error_entities, error_tooltips);
        }
    }

    void appendJunctionSimulationCells(QList<TableCell> &cells,
                                       const HydraulicSimulationResultNodeJunction *result) const
    {
        if (result == nullptr)
        {
            for (int index = 0; index < 9; ++index)
                cells.append(emptyCell());
            return;
        }

        cells.append(numberCell(result->demand_requested_m3_per_h, 3, QStringLiteral(" m³/h")));
        cells.append(numberCell(result->demand_delivered_m3_per_h, 3, QStringLiteral(" m³/h")));
        cells.append(numberCell(result->demand_deficit_m3_per_h, 3, QStringLiteral(" m³/h")));
        cells.append(numberCell(result->total_demand_m3_per_h, 3, QStringLiteral(" m³/h")));
        cells.append(numberCell(result->emitter_flow_m3_per_h, 3, QStringLiteral(" m³/h")));
        cells.append(numberCell(result->leakage_flow_m3_per_h, 3, QStringLiteral(" m³/h")));
        cells.append(numberCell(result->hydraulic_head_m, 3, QStringLiteral(" m")));
        cells.append(numberCell(result->pressure_head_m, 3, QStringLiteral(" m")));
        cells.append(boolCell(result->appears_in_control));
    }

    void buildReservoirs(const NetworkHydraulic &network,
                         const HydraulicSimulationResult *simulation_result,
                         const QHash<QUuid, InfrastructureEntity> &error_entities,
                         const QHash<QUuid, QString> &error_tooltips)
    {
        const QHash<QUuid, const HydraulicSimulationResultNodeReservoir *> result_lookup =
            simulation_result == nullptr
                ? QHash<QUuid, const HydraulicSimulationResultNodeReservoir *>()
                : simulationResultLookup(simulation_result->nodes_reservoirs);

        const std::optional<WaterQualitySimulationResultTimeline> &quality_timeline =
            this->hydraulic_data->waterQualitySimulationResultTimeline();
        const WaterQualityAnalysisType quality_analysis = quality_timeline.has_value()
            ? quality_timeline->analysis : WaterQualityAnalysisType::None;
        const WaterQualitySimulationResult *quality_result =
            this->hydraulic_data->currentWaterQualitySimulationResult();
        const QHash<QUuid, const WaterQualitySimulationResultNodeReservoir *> quality_lookup =
            quality_result == nullptr
                ? QHash<QUuid, const WaterQualitySimulationResultNodeReservoir *>()
                : simulationResultLookup(quality_result->nodes_reservoirs);

        appendCommonColumns(this->columns);
        appendNodePositionColumns(this->columns);
        this->columns.append({QStringLiteral("Head Input"), false, {}, ColumnFilterKind::Choice});
        this->columns.append({QStringLiteral("Head Direct [m]"), false, {AOWIS_QUANTITY("hydraulic_head")}, ColumnFilterKind::Number});
        this->columns.append({QStringLiteral("Elevation Terrain [m]"), false, {AOWIS_QUANTITY("elevation")}, ColumnFilterKind::Number});
        this->columns.append({QStringLiteral("Head Offset [m]"), false, {AOWIS_QUANTITY("vertical_offset")}, ColumnFilterKind::Number});
        this->columns.append({QStringLiteral("Head Resolved [m]"), false,
            {AOWIS_QUANTITY("hydraulic_head")}, ColumnFilterKind::Number});
        this->columns.append({QStringLiteral("Head Pattern Mode"), false, {}, ColumnFilterKind::Choice});
        this->columns.append({QStringLiteral("Head Pattern"), false, {}, ColumnFilterKind::Text});
        this->columns.append({QStringLiteral("Demand Net [m³/h]"), true,
            {AOWIS_QUANTITY("volumetric_flow_rate")}, ColumnFilterKind::Number});
        this->columns.append({QStringLiteral("Head [m]"), true, {AOWIS_QUANTITY("hydraulic_head")}, ColumnFilterKind::Number});
        this->columns.append({QStringLiteral("Pressure [m]"), true, {AOWIS_QUANTITY("pressure_head")}, ColumnFilterKind::Number});
        this->columns.append({QStringLiteral("Referenced by Control"), true, {}, ColumnFilterKind::Choice});
        appendNodeQualityInputColumns(this->columns);
        this->columns.append(qualityResultColumn(quality_analysis));
        if (quality_analysis == WaterQualityAnalysisType::Chemical)
            this->columns.append({QStringLiteral("Quality Source Mass Flow [mg/min]"), true,
                {AOWIS_QUANTITY("chemical_mass_flow_rate")}, ColumnFilterKind::Number});

        for (const HydraulicNodeReservoir &reservoir : network.nodes_reservoirs)
        {
            TableRow row;
            row.uuid = reservoir.uuid;
            appendCommonCells(row.cells, reservoir.id, reservoir.metadata);
            appendNodePositionCells(row.cells, reservoir.coordinate_wgs84);
            row.cells.append(textCell(elevationInputText(reservoir.head_input_type)));
            row.cells.append(numberCell(reservoir.hydraulic_head_m, 3, QStringLiteral(" m")));
            row.cells.append(numberCell(reservoir.terrain_elevation_m, 3, QStringLiteral(" m")));
            row.cells.append(numberCell(reservoir.hydraulic_head_offset_m, 3, QStringLiteral(" m")));
            row.cells.append(numberCell(resolvedSymbologyElevationM(reservoir), 3, QStringLiteral(" m")));
            row.cells.append(textCell(patternModeText(reservoir.head_pattern_mode)));
            row.cells.append(textCell(patternId(network, reservoir.head_pattern_uuid)));

            const HydraulicSimulationResultNodeReservoir *result =
                result_lookup.value(reservoir.uuid, nullptr);
            if (result == nullptr)
            {
                for (int index = 0; index < 4; ++index)
                    row.cells.append(emptyCell());
            }
            else
            {
                row.cells.append(numberCell(result->net_demand_m3_per_h, 3, QStringLiteral(" m³/h")));
                row.cells.append(numberCell(result->hydraulic_head_m, 3, QStringLiteral(" m")));
                row.cells.append(numberCell(result->pressure_head_m, 3, QStringLiteral(" m")));
                row.cells.append(boolCell(result->appears_in_control));
            }

            appendNodeQualityInputCells(row.cells, network, reservoir);
            const WaterQualitySimulationResultNodeReservoir *quality_entity_result =
                quality_lookup.value(reservoir.uuid, nullptr);
            row.cells.append(qualityResultCell(quality_entity_result, quality_analysis));
            if (quality_analysis == WaterQualityAnalysisType::Chemical)
            {
                row.cells.append(quality_entity_result == nullptr
                    ? emptyCell()
                    : numberCell(quality_entity_result->source_mass_flow_mg_per_min, 6,
                                 QStringLiteral(" mg/min")));
            }

            finishRow(row, error_entities, error_tooltips);
        }
    }

    void buildTanks(const NetworkHydraulic &network,
                    const HydraulicSimulationResult *simulation_result,
                    const QHash<QUuid, InfrastructureEntity> &error_entities,
                    const QHash<QUuid, QString> &error_tooltips)
    {
        const QHash<QUuid, const HydraulicSimulationResultNodeTank *> result_lookup =
            simulation_result == nullptr
                ? QHash<QUuid, const HydraulicSimulationResultNodeTank *>()
                : simulationResultLookup(simulation_result->nodes_tanks);

        const std::optional<WaterQualitySimulationResultTimeline> &quality_timeline =
            this->hydraulic_data->waterQualitySimulationResultTimeline();
        const WaterQualityAnalysisType quality_analysis = quality_timeline.has_value()
            ? quality_timeline->analysis : WaterQualityAnalysisType::None;
        const WaterQualitySimulationResult *quality_result =
            this->hydraulic_data->currentWaterQualitySimulationResult();
        const QHash<QUuid, const WaterQualitySimulationResultNodeTank *> quality_lookup =
            quality_result == nullptr
                ? QHash<QUuid, const WaterQualitySimulationResultNodeTank *>()
                : simulationResultLookup(quality_result->nodes_tanks);

        appendCommonColumns(this->columns);
        appendNodePositionColumns(this->columns);
        this->columns.append({QStringLiteral("Elevation Input"), false, {}, ColumnFilterKind::Choice});
        this->columns.append({QStringLiteral("Elevation Bottom [m]"), false, {AOWIS_QUANTITY("elevation")}, ColumnFilterKind::Number});
        this->columns.append({QStringLiteral("Elevation Terrain [m]"), false, {AOWIS_QUANTITY("elevation")}, ColumnFilterKind::Number});
        this->columns.append({QStringLiteral("Bottom Offset [m]"), false,
            {AOWIS_QUANTITY("vertical_offset")}, ColumnFilterKind::Number});
        this->columns.append({QStringLiteral("Elevation Bottom Resolved [m]"), false,
            {AOWIS_QUANTITY("elevation")}, ColumnFilterKind::Number});
        this->columns.append({QStringLiteral("Level Initial [m]"), false,
            {AOWIS_QUANTITY("tank_initial_level")}, ColumnFilterKind::Number});
        this->columns.append({QStringLiteral("Level Minimum [m]"), false,
            {AOWIS_QUANTITY("tank_minimum_level")}, ColumnFilterKind::Number});
        this->columns.append({QStringLiteral("Level Maximum [m]"), false,
            {AOWIS_QUANTITY("tank_maximum_level")}, ColumnFilterKind::Number});
        this->columns.append({QStringLiteral("Geometry Input"), false, {}, ColumnFilterKind::Choice});
        this->columns.append({QStringLiteral("Diameter [m]"), false, {AOWIS_QUANTITY("tank_diameter")}, ColumnFilterKind::Number});
        this->columns.append({QStringLiteral("Area Cross-Section [m²]"), false, {AOWIS_QUANTITY("area")}, ColumnFilterKind::Number});
        this->columns.append({QStringLiteral("Volume Maximum [m³]"), false, {AOWIS_QUANTITY("volume")}, ColumnFilterKind::Number});
        this->columns.append({QStringLiteral("Volume Minimum [m³]"), false,
            {AOWIS_QUANTITY("tank_minimum_volume")}, ColumnFilterKind::Number});
        this->columns.append({QStringLiteral("Volume Curve"), false, {}, ColumnFilterKind::Text});
        this->columns.append({QStringLiteral("Can Overflow"), false, {}, ColumnFilterKind::Choice});
        this->columns.append({QStringLiteral("Demand Net [m³/h]"), true,
            {AOWIS_QUANTITY("volumetric_flow_rate")}, ColumnFilterKind::Number});
        this->columns.append({QStringLiteral("Head [m]"), true, {AOWIS_QUANTITY("hydraulic_head")}, ColumnFilterKind::Number});
        this->columns.append({QStringLiteral("Pressure [m]"), true, {AOWIS_QUANTITY("pressure_head")}, ColumnFilterKind::Number});
        this->columns.append({QStringLiteral("Level Water [m]"), true, {AOWIS_QUANTITY("water_level")}, ColumnFilterKind::Number});
        this->columns.append({QStringLiteral("Volume [m³]"), true, {AOWIS_QUANTITY("volume")}, ColumnFilterKind::Number});
        this->columns.append({QStringLiteral("Volume Mixing-Zone [m³]"), true, {AOWIS_QUANTITY("volume")}, ColumnFilterKind::Number});
        this->columns.append({QStringLiteral("Referenced by Control"), true, {}, ColumnFilterKind::Choice});
        appendNodeQualityInputColumns(this->columns);
        this->columns.append({QStringLiteral("Quality Mixing Model"), false, {}, ColumnFilterKind::Choice});
        this->columns.append({QStringLiteral("Quality Mixing Fraction"), false, {}, ColumnFilterKind::Number});
        this->columns.append({QStringLiteral("Quality Override Bulk Reaction"), false, {}, ColumnFilterKind::Choice});
        this->columns.append({QStringLiteral("Quality Bulk Reaction Coefficient"), false,
            {{}, UnitTablePresentation::ColumnKind::BulkReaction}, ColumnFilterKind::None});
        this->columns.append(qualityResultColumn(quality_analysis));
        if (quality_analysis == WaterQualityAnalysisType::Chemical)
            this->columns.append({QStringLiteral("Quality Source Mass Flow [mg/min]"), true,
                {AOWIS_QUANTITY("chemical_mass_flow_rate")}, ColumnFilterKind::Number});

        for (const HydraulicNodeTank &tank : network.nodes_tanks)
        {
            TableRow row;
            row.uuid = tank.uuid;
            appendCommonCells(row.cells, tank.id, tank.metadata);
            appendNodePositionCells(row.cells, tank.coordinate_wgs84);
            row.cells.append(textCell(elevationInputText(tank.elevation_input_type)));
            row.cells.append(numberCell(tank.bottom_elevation_m, 3, QStringLiteral(" m")));
            row.cells.append(numberCell(tank.terrain_elevation_m, 3, QStringLiteral(" m")));
            row.cells.append(numberCell(tank.bottom_offset_m, 3, QStringLiteral(" m")));
            row.cells.append(numberCell(resolvedSymbologyElevationM(tank), 3, QStringLiteral(" m")));
            row.cells.append(numberCell(tank.water_level_initial_m, 3, QStringLiteral(" m")));
            row.cells.append(numberCell(tank.water_level_minimum_m, 3, QStringLiteral(" m")));
            row.cells.append(numberCell(tank.water_level_maximum_m, 3, QStringLiteral(" m")));
            row.cells.append(textCell(tankGeometryInputText(tank.geometry_input_type)));
            row.cells.append(numberCell(tank.diameter_m, 3, QStringLiteral(" m")));
            row.cells.append(numberCell(tank.cross_section_area_m2, 3, QStringLiteral(" m²")));
            row.cells.append(numberCell(tank.volume_at_maximum_level_m3, 3, QStringLiteral(" m³")));
            row.cells.append(numberCell(tank.minimum_volume_m3, 3, QStringLiteral(" m³")));
            row.cells.append(textCell(tankVolumeCurveId(network, tank.volume_curve_uuid)));
            row.cells.append(boolCell(tank.can_overflow));

            const HydraulicSimulationResultNodeTank *result =
                result_lookup.value(tank.uuid, nullptr);
            if (result == nullptr)
            {
                for (int index = 0; index < 7; ++index)
                    row.cells.append(emptyCell());
            }
            else
            {
                row.cells.append(numberCell(result->net_demand_m3_per_h, 3, QStringLiteral(" m³/h")));
                row.cells.append(numberCell(result->hydraulic_head_m, 3, QStringLiteral(" m")));
                row.cells.append(numberCell(result->pressure_head_m, 3, QStringLiteral(" m")));
                row.cells.append(numberCell(result->water_level_m, 3, QStringLiteral(" m")));
                row.cells.append(numberCell(result->volume_m3, 3, QStringLiteral(" m³")));
                row.cells.append(numberCell(result->mixing_zone_volume_m3, 3, QStringLiteral(" m³")));
                row.cells.append(boolCell(result->appears_in_control));
            }

            appendNodeQualityInputCells(row.cells, network, tank);
            row.cells.append(textCell(tankMixingModelText(tank.mixing_model)));
            row.cells.append(numberCell(tank.mixing_fraction, 4));
            row.cells.append(boolCell(tank.override_bulk_reaction));
            row.cells.append(reactionCell(tank.bulk_reaction.coefficient,
                                          tank.bulk_reaction.order,
                                          UnitTablePresentation::ReactionKind::Bulk));
            const WaterQualitySimulationResultNodeTank *quality_entity_result =
                quality_lookup.value(tank.uuid, nullptr);
            row.cells.append(qualityResultCell(quality_entity_result, quality_analysis));
            if (quality_analysis == WaterQualityAnalysisType::Chemical)
            {
                row.cells.append(quality_entity_result == nullptr
                    ? emptyCell()
                    : numberCell(quality_entity_result->source_mass_flow_mg_per_min, 6,
                                 QStringLiteral(" mg/min")));
            }

            finishRow(row, error_entities, error_tooltips);
        }
    }

    void buildPipes(const NetworkHydraulic &network,
                    const HydraulicSimulationResult *simulation_result,
                    const QHash<QUuid, InfrastructureEntity> &error_entities,
                    const QHash<QUuid, QString> &error_tooltips)
    {
        const QHash<QUuid, QString> node_ids = nodeIdLookup(network);
        const QHash<QUuid, const HydraulicSimulationResultLinkPipe *> result_lookup =
            simulation_result == nullptr
                ? QHash<QUuid, const HydraulicSimulationResultLinkPipe *>()
                : simulationResultLookup(simulation_result->links_pipes);

        const std::optional<WaterQualitySimulationResultTimeline> &quality_timeline =
            this->hydraulic_data->waterQualitySimulationResultTimeline();
        const WaterQualityAnalysisType quality_analysis = quality_timeline.has_value()
            ? quality_timeline->analysis : WaterQualityAnalysisType::None;
        const WaterQualitySimulationResult *quality_result =
            this->hydraulic_data->currentWaterQualitySimulationResult();
        const QHash<QUuid, const WaterQualitySimulationResultLinkPipe *> quality_lookup =
            quality_result == nullptr
                ? QHash<QUuid, const WaterQualitySimulationResultLinkPipe *>()
                : simulationResultLookup(quality_result->links_pipes);

        appendCommonColumns(this->columns);
        this->columns.append({QStringLiteral("Node 1"), false, {}, ColumnFilterKind::Text});
        this->columns.append({QStringLiteral("Node 2"), false, {}, ColumnFilterKind::Text});
        this->columns.append({QStringLiteral("Vertices"), false, {}, ColumnFilterKind::Integer});
        this->columns.append({QStringLiteral("Length Calculated [m]"), false, {AOWIS_QUANTITY("length")}, ColumnFilterKind::Number});
        this->columns.append({QStringLiteral("Length Measured [m]"), false, {AOWIS_QUANTITY("length")}, ColumnFilterKind::Number});
        this->columns.append({QStringLiteral("Length Effective [m]"), false, {AOWIS_QUANTITY("length")}, ColumnFilterKind::Number});
        this->columns.append({QStringLiteral("Status Initial"), false, {}, ColumnFilterKind::Choice});
        this->columns.append({QStringLiteral("Diameter [mm]"), false, {AOWIS_QUANTITY("link_diameter")}, ColumnFilterKind::Number});
        this->columns.append({QStringLiteral("Material"), false, {}, ColumnFilterKind::Text});
        this->columns.append({QStringLiteral("Roughness HW"), false, {}, ColumnFilterKind::Number});
        this->columns.append({QStringLiteral("Roughness DW [mm]"), false,
            {AOWIS_QUANTITY("darcy_weisbach_roughness_height")}, ColumnFilterKind::Number});
        this->columns.append({QStringLiteral("Roughness CM"), false, {}, ColumnFilterKind::Number});
        this->columns.append({QStringLiteral("Minor Loss"), false, {}, ColumnFilterKind::Number});
        this->columns.append({QStringLiteral("Leak Area [mm²/100m]"), false,
            {AOWIS_QUANTITY("leak_area_per_100m_pipe_length")}, ColumnFilterKind::Number});
        this->columns.append({QStringLiteral("Leak Expansion [mm²/m head]"), false,
            {AOWIS_QUANTITY("leak_area_expansion_per_pressure_head")}, ColumnFilterKind::Number});
        this->columns.append({QStringLiteral("Flow [m³/h]"), true, {AOWIS_QUANTITY("volumetric_flow_rate")}, ColumnFilterKind::Number});
        this->columns.append({QStringLiteral("Flow Leakage [m³/h]"), true,
            {AOWIS_QUANTITY("volumetric_flow_rate")}, ColumnFilterKind::Number});
        this->columns.append({QStringLiteral("Velocity [m/s]"), true, {AOWIS_QUANTITY("velocity")}, ColumnFilterKind::Number});
        this->columns.append({QStringLiteral("Head Loss [m]"), true, {AOWIS_QUANTITY("head_loss")}, ColumnFilterKind::Number});
        this->columns.append({QStringLiteral("Head Loss Unit [m/km]"), true,
            {AOWIS_QUANTITY("head_loss_gradient")}, ColumnFilterKind::Number});
        this->columns.append({QStringLiteral("Friction Factor"), true, {}, ColumnFilterKind::Number});
        this->columns.append({QStringLiteral("Status"), true, {}, ColumnFilterKind::Choice});
        this->columns.append({QStringLiteral("Roughness HW Effective"), true, {}, ColumnFilterKind::Number});
        this->columns.append({QStringLiteral("Roughness DW Effective [mm]"), true,
            {AOWIS_QUANTITY("darcy_weisbach_roughness_height")}, ColumnFilterKind::Number});
        this->columns.append({QStringLiteral("Roughness CM Effective"), true, {}, ColumnFilterKind::Number});
        this->columns.append({QStringLiteral("Referenced by Control"), true, {}, ColumnFilterKind::Choice});
        this->columns.append({QStringLiteral("Quality Override Bulk Reaction"), false, {}, ColumnFilterKind::Choice});
        this->columns.append({QStringLiteral("Quality Bulk Reaction Coefficient"), false,
            {{}, UnitTablePresentation::ColumnKind::BulkReaction}, ColumnFilterKind::None});
        this->columns.append({QStringLiteral("Quality Override Wall Reaction"), false, {}, ColumnFilterKind::Choice});
        this->columns.append({QStringLiteral("Quality Wall Reaction Coefficient"), false,
            {{}, UnitTablePresentation::ColumnKind::WallReaction}, ColumnFilterKind::None});
        this->columns.append(qualityResultColumn(quality_analysis));

        for (const HydraulicLinkPipe &pipe : network.links_pipes)
        {
            TableRow row;
            row.uuid = pipe.uuid;
            appendCommonCells(row.cells, pipe.id, pipe.metadata);
            row.cells.append(textCell(nodeId(node_ids, pipe.node_uuid_from)));
            row.cells.append(textCell(nodeId(node_ids, pipe.node_uuid_to)));
            row.cells.append(integerCell(pipe.vertices.size()));
            row.cells.append(numberCell(pipe.length_calculated_m, 3, QStringLiteral(" m")));
            row.cells.append(optionalNumberCell(pipe.length_measured_m, 3, QStringLiteral(" m")));
            row.cells.append(numberCell(pipe.length_measured_m.value_or(pipe.length_calculated_m), 3,
                                        QStringLiteral(" m")));
            row.cells.append(textCell(pipeInitialStatusText(pipe.initial_status)));
            row.cells.append(numberCell(pipe.diameter_mm, 3, QStringLiteral(" mm")));
            row.cells.append(textCell(pipeMaterialId(network, pipe.material_uuid)));
            row.cells.append(numberCell(pipe.roughness_hazen_williams, 3));
            row.cells.append(numberCell(pipe.roughness_darcy_weisbach_mm, 6, QStringLiteral(" mm")));
            row.cells.append(numberCell(pipe.roughness_chezy_manning, 6));
            row.cells.append(numberCell(pipe.minor_loss_coefficient, 6));
            row.cells.append(numberCell(pipe.leak_area_mm2_per_100m, 6, QStringLiteral(" mm²/100m")));
            row.cells.append(numberCell(pipe.leak_area_expansion_per_pressure_head_mm2_per_m, 6,
                                        QStringLiteral(" mm²/m head")));

            const HydraulicSimulationResultLinkPipe *result =
                result_lookup.value(pipe.uuid, nullptr);
            if (result == nullptr)
            {
                for (int index = 0; index < 11; ++index)
                    row.cells.append(emptyCell());
            }
            else
            {
                row.cells.append(numberCell(result->flow_m3_per_h, 3, QStringLiteral(" m³/h")));
                row.cells.append(numberCell(result->leakage_flow_m3_per_h, 3, QStringLiteral(" m³/h")));
                row.cells.append(numberCell(result->velocity_m_per_s, 3, QStringLiteral(" m/s")));
                row.cells.append(numberCell(result->head_loss_m, 3, QStringLiteral(" m")));
                row.cells.append(numberCell(result->head_loss_gradient_m_per_km, 3, QStringLiteral(" m/km")));
                row.cells.append(numberCell(result->friction_factor, 6));
                row.cells.append(boolCell(result->open, QStringLiteral("Open"), QStringLiteral("Closed")));
                row.cells.append(optionalNumberCell(result->roughness_hazen_williams, 3));
                row.cells.append(optionalNumberCell(result->roughness_darcy_weisbach_mm, 6, QStringLiteral(" mm")));
                row.cells.append(optionalNumberCell(result->roughness_chezy_manning, 6));
                row.cells.append(boolCell(result->appears_in_control));
            }

            row.cells.append(boolCell(pipe.override_bulk_reaction));
            row.cells.append(reactionCell(pipe.bulk_reaction.coefficient,
                                          pipe.bulk_reaction.order,
                                          UnitTablePresentation::ReactionKind::Bulk));
            row.cells.append(boolCell(pipe.override_wall_reaction));
            row.cells.append(reactionCell(pipe.wall_reaction.coefficient,
                                          pipe.wall_reaction.order,
                                          UnitTablePresentation::ReactionKind::Wall));
            const WaterQualitySimulationResultLinkPipe *quality_entity_result =
                quality_lookup.value(pipe.uuid, nullptr);
            row.cells.append(qualityResultCell(quality_entity_result, quality_analysis));

            finishRow(row, error_entities, error_tooltips);
        }
    }

    void buildPumps(const NetworkHydraulic &network,
                    const HydraulicSimulationResult *simulation_result,
                    const QHash<QUuid, InfrastructureEntity> &error_entities,
                    const QHash<QUuid, QString> &error_tooltips)
    {
        const QHash<QUuid, QString> node_ids = nodeIdLookup(network);
        const QHash<QUuid, const HydraulicSimulationResultLinkPump *> result_lookup =
            simulation_result == nullptr
                ? QHash<QUuid, const HydraulicSimulationResultLinkPump *>()
                : simulationResultLookup(simulation_result->links_pumps);

        const std::optional<WaterQualitySimulationResultTimeline> &quality_timeline =
            this->hydraulic_data->waterQualitySimulationResultTimeline();
        const WaterQualityAnalysisType quality_analysis = quality_timeline.has_value()
            ? quality_timeline->analysis : WaterQualityAnalysisType::None;
        const WaterQualitySimulationResult *quality_result =
            this->hydraulic_data->currentWaterQualitySimulationResult();
        const QHash<QUuid, const WaterQualitySimulationResultLinkPump *> quality_lookup =
            quality_result == nullptr
                ? QHash<QUuid, const WaterQualitySimulationResultLinkPump *>()
                : simulationResultLookup(quality_result->links_pumps);

        appendCommonColumns(this->columns);
        this->columns.append({QStringLiteral("Node 1"), false, {}, ColumnFilterKind::Text});
        this->columns.append({QStringLiteral("Node 2"), false, {}, ColumnFilterKind::Text});
        this->columns.append({QStringLiteral("Vertices"), false, {}, ColumnFilterKind::Integer});
        this->columns.append({QStringLiteral("Definition"), false, {}, ColumnFilterKind::Choice});
        this->columns.append({QStringLiteral("Power Constant [kW]"), false, {AOWIS_QUANTITY("power")}, ColumnFilterKind::Number});
        this->columns.append({QStringLiteral("Head Curve"), false, {}, ColumnFilterKind::Text});
        this->columns.append({QStringLiteral("Speed Initial"), false, {}, ColumnFilterKind::Number});
        this->columns.append({QStringLiteral("Status Initial"), false, {}, ColumnFilterKind::Choice});
        this->columns.append({QStringLiteral("Speed Pattern"), false, {}, ColumnFilterKind::Text});
        this->columns.append({QStringLiteral("Controls"), false, {}, ColumnFilterKind::None});
        this->columns.append({QStringLiteral("Efficiency Input"), false, {}, ColumnFilterKind::Choice});
        this->columns.append({QStringLiteral("Efficiency Constant [%]"), false,
            {AOWIS_QUANTITY("efficiency")}, ColumnFilterKind::Number});
        this->columns.append({QStringLiteral("Efficiency Curve"), false, {}, ColumnFilterKind::Text});
        this->columns.append({QStringLiteral("Energy Price Input"), false, {}, ColumnFilterKind::Choice});
        const QString energy_currency = network.options_energy.currency_iso4217;
        const QString energy_price_header = energy_currency.isEmpty()
            ? QStringLiteral("Energy Price [/kWh]")
            : QStringLiteral("Energy Price [%1/kWh]").arg(energy_currency);
        const QString energy_cost_header = energy_currency.isEmpty()
            ? QStringLiteral("Cost Average / Day")
            : QStringLiteral("Cost Average [%1/day]").arg(energy_currency);
        this->columns.append({energy_price_header, false, {{}, UnitTablePresentation::ColumnKind::EnergyPrice, energy_currency}, ColumnFilterKind::Number});
        this->columns.append({QStringLiteral("Price Pattern"), false, {}, ColumnFilterKind::Text});
        this->columns.append({QStringLiteral("Flow [m³/h]"), true, {AOWIS_QUANTITY("volumetric_flow_rate")}, ColumnFilterKind::Number});
        this->columns.append({QStringLiteral("Velocity [m/s]"), true, {AOWIS_QUANTITY("velocity")}, ColumnFilterKind::Number});
        this->columns.append({QStringLiteral("Head Gain [m]"), true, {AOWIS_QUANTITY("head_gain")}, ColumnFilterKind::Number});
        this->columns.append({QStringLiteral("Status"), true, {}, ColumnFilterKind::Choice});
        this->columns.append({QStringLiteral("State Operating"), true, {}, ColumnFilterKind::Choice});
        this->columns.append({QStringLiteral("Speed"), true, {}, ColumnFilterKind::Number});
        this->columns.append({QStringLiteral("Efficiency [%]"), true, {AOWIS_QUANTITY("efficiency")}, ColumnFilterKind::Number});
        this->columns.append({QStringLiteral("Power [kW]"), true, {AOWIS_QUANTITY("power")}, ColumnFilterKind::Number});
        this->columns.append({QStringLiteral("Referenced by Control"), true, {}, ColumnFilterKind::Choice});
        this->columns.append({QStringLiteral("Time Online [%]"), true,
            {AOWIS_QUANTITY("operating_time_percentage")}, ColumnFilterKind::Number});
        this->columns.append({QStringLiteral("Efficiency Average [%]"), true,
            {AOWIS_QUANTITY("efficiency")}, ColumnFilterKind::Number});
        this->columns.append({QStringLiteral("Energy Intensity Average [kWh/m³]"), true,
            {AOWIS_QUANTITY("energy_intensity")}, ColumnFilterKind::Number});
        this->columns.append({QStringLiteral("Power Average [kW]"), true, {AOWIS_QUANTITY("power")}, ColumnFilterKind::Number});
        this->columns.append({QStringLiteral("Power Peak [kW]"), true, {AOWIS_QUANTITY("power")}, ColumnFilterKind::Number});
        this->columns.append({energy_cost_header, true, {}, ColumnFilterKind::Number});
        this->columns.append(qualityResultColumn(quality_analysis));

        const HydraulicSimulationResult *final_result = nullptr;
        const std::optional<HydraulicSimulationResultTimeline> &timeline =
            this->hydraulic_data->simulationResultTimeline();
        if (this->hydraulic_data->hasSimulationResults() && timeline.has_value()
            && !timeline->results.isEmpty())
        {
            final_result = &timeline->results.constLast();
        }

        const QHash<QUuid, const HydraulicSimulationResultLinkPumpEnergyUsage *>
            energy_usage_lookup = final_result == nullptr
                ? QHash<QUuid, const HydraulicSimulationResultLinkPumpEnergyUsage *>()
                : pumpEnergyUsageLookup(final_result->links_pump_energy_usage);

        for (const HydraulicLinkPump &pump : network.links_pumps)
        {
            TableRow row;
            row.uuid = pump.uuid;
            appendCommonCells(row.cells, pump.id, pump.metadata);
            row.cells.append(textCell(nodeId(node_ids, pump.node_uuid_from)));
            row.cells.append(textCell(nodeId(node_ids, pump.node_uuid_to)));
            row.cells.append(integerCell(pump.vertices.size()));
            row.cells.append(textCell(pumpDefinitionText(pump.definition_type)));
            row.cells.append(numberCell(pump.constant_power_kw, 3, QStringLiteral(" kW")));
            row.cells.append(textCell(pumpHeadCurveId(network, pump.head_curve_uuid)));
            row.cells.append(numberCell(pump.initial_speed_ratio, 4));
            row.cells.append(textCell(pumpInitialStatusText(pump.initial_status)));
            row.cells.append(textCell(patternId(network, pump.speed_pattern_uuid)));
            row.cells.append(textCell(pumpControlSummary(network, pump.uuid)));
            row.cells.append(textCell(pumpEfficiencyInputText(pump.efficiency_input_type)));
            row.cells.append(numberCell(pump.constant_efficiency_percent, 3, QStringLiteral(" %")));
            row.cells.append(textCell(pumpEfficiencyCurveId(network, pump.efficiency_curve_uuid)));
            row.cells.append(textCell(pumpEnergyPriceInputText(pump.energy_price_input_type)));
            row.cells.append(numberCell(pump.energy_price_per_kw_h, 6));
            row.cells.append(textCell(patternId(network, pump.price_pattern_uuid)));

            const HydraulicSimulationResultLinkPump *result =
                result_lookup.value(pump.uuid, nullptr);
            if (result == nullptr)
            {
                for (int index = 0; index < 9; ++index)
                    row.cells.append(emptyCell());
            }
            else
            {
                row.cells.append(numberCell(result->flow_m3_per_h, 3, QStringLiteral(" m³/h")));
                row.cells.append(numberCell(result->velocity_m_per_s, 3, QStringLiteral(" m/s")));
                row.cells.append(numberCell(result->head_gain_m, 3, QStringLiteral(" m")));
                row.cells.append(boolCell(result->open, QStringLiteral("Open"), QStringLiteral("Closed")));
                row.cells.append(textCell(pumpStateText(result->state)));
                row.cells.append(numberCell(result->speed_ratio, 4));
                row.cells.append(numberCell(result->efficiency_percent, 3, QStringLiteral(" %")));
                row.cells.append(numberCell(result->power_kw, 3, QStringLiteral(" kW")));
                row.cells.append(boolCell(result->appears_in_control));
            }

            const HydraulicSimulationResultLinkPumpEnergyUsage *energy_usage =
                energy_usage_lookup.value(pump.uuid, nullptr);
            if (energy_usage == nullptr)
            {
                for (int index = 0; index < 6; ++index)
                    row.cells.append(emptyCell());
            }
            else
            {
                row.cells.append(numberCell(energy_usage->time_online_percent, 2, QStringLiteral(" %")));
                row.cells.append(numberCell(energy_usage->average_efficiency_percent, 2, QStringLiteral(" %")));
                row.cells.append(numberCell(energy_usage->average_energy_intensity_kw_h_per_m3, 6));
                row.cells.append(numberCell(energy_usage->average_power_kw, 3, QStringLiteral(" kW")));
                row.cells.append(numberCell(energy_usage->peak_power_kw, 3, QStringLiteral(" kW")));
                row.cells.append(numberCell(energy_usage->average_cost_per_day, 4));
            }

            const WaterQualitySimulationResultLinkPump *quality_entity_result =
                quality_lookup.value(pump.uuid, nullptr);
            row.cells.append(qualityResultCell(quality_entity_result, quality_analysis));

            finishRow(row, error_entities, error_tooltips);
        }
    }

    void buildValves(const NetworkHydraulic &network,
                     const HydraulicSimulationResult *simulation_result,
                     const QHash<QUuid, InfrastructureEntity> &error_entities,
                     const QHash<QUuid, QString> &error_tooltips)
    {
        const QHash<QUuid, QString> node_ids = nodeIdLookup(network);
        const QHash<QUuid, const HydraulicSimulationResultLinkValve *> result_lookup =
            simulation_result == nullptr
                ? QHash<QUuid, const HydraulicSimulationResultLinkValve *>()
                : simulationResultLookup(simulation_result->links_valves);

        const std::optional<WaterQualitySimulationResultTimeline> &quality_timeline =
            this->hydraulic_data->waterQualitySimulationResultTimeline();
        const WaterQualityAnalysisType quality_analysis = quality_timeline.has_value()
            ? quality_timeline->analysis : WaterQualityAnalysisType::None;
        const WaterQualitySimulationResult *quality_result =
            this->hydraulic_data->currentWaterQualitySimulationResult();
        const QHash<QUuid, const WaterQualitySimulationResultLinkValve *> quality_lookup =
            quality_result == nullptr
                ? QHash<QUuid, const WaterQualitySimulationResultLinkValve *>()
                : simulationResultLookup(quality_result->links_valves);

        appendCommonColumns(this->columns);
        this->columns.append({QStringLiteral("Node 1"), false, {}, ColumnFilterKind::Text});
        this->columns.append({QStringLiteral("Node 2"), false, {}, ColumnFilterKind::Text});
        this->columns.append({QStringLiteral("Vertices"), false, {}, ColumnFilterKind::Integer});
        this->columns.append({QStringLiteral("Valve Type"), false, {}, ColumnFilterKind::Choice});
        // A pressure, flow, loss coefficient and position cannot share one
        // numeric range filter. The Valve Type filter remains available.
        this->columns.append({QStringLiteral("Setting"), false,
            {{}, UnitTablePresentation::ColumnKind::RowQuantity}, ColumnFilterKind::None});
        this->columns.append({QStringLiteral("Setting Curve"), false, {}, ColumnFilterKind::Text});
        this->columns.append({QStringLiteral("Status Initial"), false, {}, ColumnFilterKind::Choice});
        this->columns.append({QStringLiteral("Diameter [mm]"), false, {AOWIS_QUANTITY("link_diameter")}, ColumnFilterKind::Number});
        this->columns.append({QStringLiteral("Minor Loss"), false, {}, ColumnFilterKind::Number});
        this->columns.append({QStringLiteral("Flow [m³/h]"), true, {AOWIS_QUANTITY("volumetric_flow_rate")}, ColumnFilterKind::Number});
        this->columns.append({QStringLiteral("Velocity [m/s]"), true, {AOWIS_QUANTITY("velocity")}, ColumnFilterKind::Number});
        this->columns.append({QStringLiteral("Head Loss [m]"), true, {AOWIS_QUANTITY("head_loss")}, ColumnFilterKind::Number});
        this->columns.append({QStringLiteral("Status"), true, {}, ColumnFilterKind::Choice});
        this->columns.append({QStringLiteral("Regulating"), true, {}, ColumnFilterKind::Choice});
        this->columns.append({QStringLiteral("Setting"), true,
            {{}, UnitTablePresentation::ColumnKind::RowQuantity}, ColumnFilterKind::None});
        this->columns.append({QStringLiteral("Referenced by Control"), true, {}, ColumnFilterKind::Choice});
        this->columns.append(qualityResultColumn(quality_analysis));

        for (const HydraulicLinkValve &valve : network.links_valves)
        {
            TableRow row;
            row.uuid = valve.uuid;
            row.valve_type = valve.type;
            appendCommonCells(row.cells, valve.id, valve.metadata);
            row.cells.append(textCell(nodeId(node_ids, valve.node_uuid_from)));
            row.cells.append(textCell(nodeId(node_ids, valve.node_uuid_to)));
            row.cells.append(integerCell(valve.vertices.size()));
            row.cells.append(textCell(valveTypeText(valve.type)));
            row.cells.append(valveSettingCell(valve));
            row.cells.append(textCell(valveSettingCurveId(network, valve)));
            row.cells.append(textCell(valveInitialStatusText(valve.initial_status)));
            row.cells.append(numberCell(valve.diameter_mm, 3, QStringLiteral(" mm")));
            row.cells.append(numberCell(valve.minor_loss_coefficient, 6));

            const HydraulicSimulationResultLinkValve *result =
                result_lookup.value(valve.uuid, nullptr);
            if (result == nullptr)
            {
                for (int index = 0; index < 7; ++index)
                    row.cells.append(emptyCell());
            }
            else
            {
                row.cells.append(numberCell(result->flow_m3_per_h, 3, QStringLiteral(" m³/h")));
                row.cells.append(numberCell(result->velocity_m_per_s, 3, QStringLiteral(" m/s")));
                row.cells.append(numberCell(result->head_loss_m, 3, QStringLiteral(" m")));
                row.cells.append(boolCell(result->open, QStringLiteral("Open"), QStringLiteral("Closed")));
                row.cells.append(boolCell(result->active));
                row.cells.append(valveResultSettingCell(*result));
                row.cells.append(boolCell(result->appears_in_control));
            }

            const WaterQualitySimulationResultLinkValve *quality_entity_result =
                quality_lookup.value(valve.uuid, nullptr);
            row.cells.append(qualityResultCell(quality_entity_result, quality_analysis));

            finishRow(row, error_entities, error_tooltips);
        }
    }

    QHash<QUuid, InfrastructureEntity> warning_entities;
    QHash<QUuid, QString> warning_tooltips;
    HydraulicData *hydraulic_data;
    InfrastructureEntity entity_type;
    QList<TableColumn> columns;
    QList<TableRow> rows;
};

class HydraulicEntityFilterProxyModel : public QSortFilterProxyModel
{
public:
    explicit HydraulicEntityFilterProxyModel(QObject *parent = nullptr)
        : QSortFilterProxyModel(parent)
    {
    }

    QString columnFilterValue(int column) const
    {
        if (!this->filters.contains(column))
            return QString();
        return this->filters.value(column).second;
    }

    bool numericColumnFilter(int column, double &minimum, double &maximum) const
    {
        if (!this->numeric_filters.contains(column))
            return false;

        minimum = this->numeric_filters.value(column).first;
        maximum = this->numeric_filters.value(column).second;
        return true;
    }

    void setColumnFilter(int column, ColumnFilterKind kind, const QString &value)
    {
        const QString trimmed_value = value.trimmed();
        if (trimmed_value.isEmpty())
        {
            if (this->filters.remove(column) == 0)
                return;
            notifyFilterChanged();
            return;
        }

        const QPair<ColumnFilterKind, QString> new_filter = qMakePair(kind, trimmed_value);
        if (this->filters.value(column) == new_filter)
            return;

        this->filters.insert(column, new_filter);
        notifyFilterChanged();
    }

    void setNumericColumnFilter(int column, double minimum, double maximum)
    {
        const QPair<double, double> new_filter = qMakePair(minimum, maximum);
        if (this->numeric_filters.contains(column)
            && this->numeric_filters.value(column) == new_filter)
        {
            return;
        }

        this->numeric_filters.insert(column, new_filter);
        notifyFilterChanged();
    }

    void clearNumericColumnFilter(int column)
    {
        if (this->numeric_filters.remove(column) == 0)
            return;
        notifyFilterChanged();
    }

    void setSimulationStatusFilter(SimulationStatusFilter filter)
    {
        if (this->simulation_status_filter == filter)
            return;

        this->simulation_status_filter = filter;
        notifyFilterChanged();
    }

    void clearAllFilters()
    {
        const bool changed = !this->filters.isEmpty() || !this->numeric_filters.isEmpty()
            || this->simulation_status_filter != SimulationStatusFilter::All;
        if (!changed)
            return;

        this->filters.clear();
        this->numeric_filters.clear();
        this->simulation_status_filter = SimulationStatusFilter::All;
        notifyFilterChanged();
    }

    void refreshUnitDependentTextFilters()
    {
        // Normal unit changes only repaint visible cells. Refilter the full
        // source model exclusively when a text filter actually depends on a
        // unit-formatted value, such as the compound demand-category summary.
        const HydraulicEntityTableModel *entity_model =
            static_cast<const HydraulicEntityTableModel *>(sourceModel());
        if (entity_model == nullptr)
            return;
        QHash<int, QPair<ColumnFilterKind, QString>>::const_iterator iterator =
            this->filters.constBegin();
        while (iterator != this->filters.constEnd())
        {
            if (iterator.value().first == ColumnFilterKind::Text
                && UnitTablePresentation::isUnitDependentTextColumn(
                    entity_model->columnUnits(iterator.key())))
            {
                notifyFilterChanged();
                return;
            }
            ++iterator;
        }
    }

protected:
    bool lessThan(const QModelIndex &left, const QModelIndex &right) const override
    {
        const QVariant left_group = sourceModel()->data(left, ValveSettingGroupRole);
        const QVariant right_group = sourceModel()->data(right, ValveSettingGroupRole);
        if (left_group.isValid() && right_group.isValid())
        {
            const QVariant left_value = sourceModel()->data(left, SortRole);
            const QVariant right_value = sourceModel()->data(right, SortRole);
            bool left_ok = false;
            bool right_ok = false;
            const double left_number = left_value.toDouble(&left_ok);
            const double right_number = right_value.toDouble(&right_ok);
            const ValveSettingSortPolicy::Key first{
                static_cast<ValveSettingSortPolicy::Group>(left_group.toInt()),
                left_ok && std::isfinite(left_number) ? std::optional<double>(left_number)
                                                    : std::nullopt};
            const ValveSettingSortPolicy::Key second{
                static_cast<ValveSettingSortPolicy::Group>(right_group.toInt()),
                right_ok && std::isfinite(right_number) ? std::optional<double>(right_number)
                                                     : std::nullopt};
            const int order = ValveSettingSortPolicy::compare(first, second);
            if (order != 0)
                return order < 0;

            // GPVs have curve references, not numerical settings. Make
            // equal/missing settings deterministic without comparing units.
            const QModelIndex left_id = sourceModel()->index(left.row(), 0);
            const QModelIndex right_id = sourceModel()->index(right.row(), 0);
            return QString::compare(sourceModel()->data(left_id, SortRole).toString(),
                                    sourceModel()->data(right_id, SortRole).toString(),
                                    Qt::CaseInsensitive) < 0;
        }

        const QVariant left_order = sourceModel()->data(left, ReactionOrderRole);
        const QVariant right_order = sourceModel()->data(right, ReactionOrderRole);
        if (left_order.isValid() && right_order.isValid())
        {
            const double first = left_order.toDouble();
            const double second = right_order.toDouble();
            if (std::isfinite(first) && std::isfinite(second) && first != second)
                return first < second;
        }
        return QSortFilterProxyModel::lessThan(left, right);
    }

    bool filterAcceptsRow(int source_row, const QModelIndex &source_parent) const override
    {
        QHash<int, QPair<ColumnFilterKind, QString>>::const_iterator iterator =
            this->filters.constBegin();
        while (iterator != this->filters.constEnd())
        {
            const QModelIndex index = sourceModel()->index(source_row, iterator.key(),
                                                           source_parent);
            const QString display = sourceModel()->data(index, Qt::DisplayRole).toString();
            const ColumnFilterKind kind = iterator.value().first;
            const QString value = iterator.value().second;

            if (kind == ColumnFilterKind::Text)
            {
                if (!display.contains(value, Qt::CaseInsensitive))
                    return false;
            }
            else if (kind == ColumnFilterKind::Choice)
            {
                if (display.compare(value, Qt::CaseInsensitive) != 0)
                    return false;
            }

            ++iterator;
        }

        QHash<int, QPair<double, double>>::const_iterator numeric_iterator =
            this->numeric_filters.constBegin();
        while (numeric_iterator != this->numeric_filters.constEnd())
        {
            const QModelIndex index = sourceModel()->index(source_row, numeric_iterator.key(),
                                                           source_parent);
            bool ok = false;
            const double value = sourceModel()->data(index, SortRole).toDouble(&ok);
            if (!ok || !std::isfinite(value)
                || value < numeric_iterator.value().first
                || value > numeric_iterator.value().second)
            {
                return false;
            }

            ++numeric_iterator;
        }

        const QModelIndex status_index = sourceModel()->index(source_row, 0, source_parent);
        const int status = sourceModel()->data(status_index, SimulationStatusRole).toInt();
        const bool has_warning =
            (status & static_cast<int>(SimulationRowStatus::Warning)) != 0;
        const bool has_error =
            (status & static_cast<int>(SimulationRowStatus::Error)) != 0;
        switch (this->simulation_status_filter)
        {
        case SimulationStatusFilter::WarningsAndErrors:
            if (!has_warning && !has_error)
                return false;
            break;
        case SimulationStatusFilter::WarningsOnly:
            if (!has_warning)
                return false;
            break;
        case SimulationStatusFilter::ErrorsOnly:
            if (!has_error)
                return false;
            break;
        case SimulationStatusFilter::All:
            break;
        }

        return true;
    }

private:
    void notifyFilterChanged()
    {
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
        beginFilterChange();
        endFilterChange(QSortFilterProxyModel::Direction::Rows);
#else
        invalidateFilter();
#endif
    }

    QHash<int, QPair<ColumnFilterKind, QString>> filters;
    QHash<int, QPair<double, double>> numeric_filters;
    SimulationStatusFilter simulation_status_filter = SimulationStatusFilter::All;
};

struct NumericRangeControl
{
    QWidget *container = nullptr;
    QDoubleSpinBox *minimum = nullptr;
    QDoubleSpinBox *maximum = nullptr;
    double global_minimum = 0.0; // canonical
    double global_maximum = 0.0; // canonical
    double selected_minimum = 0.0; // canonical, not the rounded spin-box value
    double selected_maximum = 0.0;
};

class HydraulicEntityFilterBar : public QWidget
{
public:
    HydraulicEntityFilterBar(QTableView *table, HydraulicEntityTableModel *model,
                             HydraulicEntityFilterProxyModel *proxy_model,
                             QWidget *parent = nullptr)
        : QWidget(parent),
          table(table),
          model(model),
          proxy_model(proxy_model)
    {
        setFixedHeight(52);
        buildControls();

        QHeaderView *header = this->table->horizontalHeader();
        connect(header, &QHeaderView::sectionResized, this,
                [this](int, int, int) { updateControlGeometries(); });
        connect(header, &QHeaderView::sectionMoved, this,
                [this](int, int, int) { updateControlGeometries(); });
        connect(header, &QHeaderView::geometriesChanged, this,
                [this]() { updateControlGeometries(); });
        connect(this->table->horizontalScrollBar(), &QScrollBar::valueChanged, this,
                [this](int) { updateControlGeometries(); });
        connect(this->model, &QAbstractItemModel::modelReset, this,
                [this]()
        {
            refreshChoiceControls();
            refreshNumericControls();
            updateControlGeometries();
        });
    }

    void refreshUnitControls()
    {
        refreshNumericControls();
        for (int column : this->numeric_controls.keys())
        {
            NumericRangeControl &control = this->numeric_controls[column];
            const QString title = UnitTablePresentation::formattedTitle(
                this->model->columnTitle(column),
                UnitProfileManager::instance().presentation(), this->model->columnUnits(column));
            control.minimum->setToolTip(QStringLiteral("Minimum filter for ") + title);
            control.maximum->setToolTip(QStringLiteral("Maximum filter for ") + title);
        }
    }

    void resetFilters()
    {
        for (QWidget *control : this->controls)
        {
            if (QLineEdit *line_edit = qobject_cast<QLineEdit *>(control))
            {
                const QSignalBlocker blocker(line_edit);
                line_edit->clear();
            }
            else if (QComboBox *combo_box = qobject_cast<QComboBox *>(control))
            {
                const QSignalBlocker blocker(combo_box);
                combo_box->setCurrentIndex(0);
            }
        }

        const QList<int> numeric_columns = this->numeric_controls.keys();
        for (int column : numeric_columns)
        {
            NumericRangeControl &control = this->numeric_controls[column];
            const QSignalBlocker minimum_blocker(control.minimum);
            const QSignalBlocker maximum_blocker(control.maximum);
            const UnitTablePresentation::ColumnUnits units = this->model->columnUnits(column);
            const UnitPresentationContext &context = UnitProfileManager::instance().presentation();
            control.minimum->setValue(UnitTablePresentation::toDisplayForColumn(
                context, units, control.global_minimum).value_or(control.global_minimum));
            control.maximum->setValue(UnitTablePresentation::toDisplayForColumn(
                context, units, control.global_maximum).value_or(control.global_maximum));
            control.selected_minimum = control.global_minimum;
            control.selected_maximum = control.global_maximum;
        }

        this->proxy_model->clearAllFilters();
    }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QWidget::resizeEvent(event);
        updateControlGeometries();
    }

private:
    static bool sameNumber(double first, double second)
    {
        const double scale = qMax(1.0, qMax(std::abs(first), std::abs(second)));
        return std::abs(first - second) <= scale * 1.0e-12;
    }

    void buildControls()
    {
        const int column_count = this->model->columnCount();
        this->controls.clear();
        this->numeric_controls.clear();
        this->controls.reserve(column_count);
        for (int column = 0; column < column_count; ++column)
        {
            const ColumnFilterKind kind = this->model->filterKind(column);
            const QString active_filter = this->proxy_model->columnFilterValue(column);
            QWidget *control = nullptr;

            if (kind == ColumnFilterKind::Text)
            {
                QLineEdit *line_edit = new QLineEdit(this);
                line_edit->setPlaceholderText(QStringLiteral("Filter…"));
                line_edit->setClearButtonEnabled(true);
                line_edit->setToolTip(QStringLiteral("Filter ")
                                      + this->model->columnTitle(column));
                connect(line_edit, &QLineEdit::textChanged, this,
                        [this, column](const QString &text)
                {
                    this->proxy_model->setColumnFilter(column, ColumnFilterKind::Text, text);
                });
                line_edit->setText(active_filter);
                control = line_edit;
            }
            else if (kind == ColumnFilterKind::Choice)
            {
                QComboBox *combo_box = new QComboBox(this);
                combo_box->setToolTip(QStringLiteral("Filter ")
                                      + this->model->columnTitle(column));
                populateChoiceControl(combo_box, column, active_filter);

                connect(combo_box, &QComboBox::currentIndexChanged, this,
                        [this, combo_box, column](int)
                {
                    this->proxy_model->setColumnFilter(
                        column, ColumnFilterKind::Choice,
                        combo_box->currentData().toString());
                });
                control = combo_box;
            }
            else if (kind == ColumnFilterKind::Number
                     || kind == ColumnFilterKind::Integer)
            {
                QWidget *range_widget = new QWidget(this);
                QVBoxLayout *range_layout = new QVBoxLayout(range_widget);
                range_layout->setContentsMargins(0, 0, 0, 0);
                range_layout->setSpacing(2);

                QWidget *minimum_row = new QWidget(range_widget);
                QHBoxLayout *minimum_row_layout = new QHBoxLayout(minimum_row);
                minimum_row_layout->setContentsMargins(0, 0, 0, 0);
                minimum_row_layout->setSpacing(1);
                QDoubleSpinBox *minimum = new QDoubleSpinBox(minimum_row);
                configureNumericSpinBox(minimum, column, true, kind);
                QToolButton *minimum_reset = createNumericResetButton(minimum_row, column, true);
                minimum_row_layout->addWidget(minimum, 1);
                minimum_row_layout->addWidget(minimum_reset);

                QWidget *maximum_row = new QWidget(range_widget);
                QHBoxLayout *maximum_row_layout = new QHBoxLayout(maximum_row);
                maximum_row_layout->setContentsMargins(0, 0, 0, 0);
                maximum_row_layout->setSpacing(1);
                QDoubleSpinBox *maximum = new QDoubleSpinBox(maximum_row);
                configureNumericSpinBox(maximum, column, false, kind);
                QToolButton *maximum_reset = createNumericResetButton(maximum_row, column, false);
                maximum_row_layout->addWidget(maximum, 1);
                maximum_row_layout->addWidget(maximum_reset);

                range_layout->addWidget(minimum_row);
                range_layout->addWidget(maximum_row);

                NumericRangeControl range_control;
                range_control.container = range_widget;
                range_control.minimum = minimum;
                range_control.maximum = maximum;
                this->numeric_controls.insert(column, range_control);
                refreshNumericControl(column);

                connect(minimum, &QDoubleSpinBox::valueChanged, this,
                        [this, column](double)
                {
                    numericControlChanged(column, true);
                });
                connect(maximum, &QDoubleSpinBox::valueChanged, this,
                        [this, column](double)
                {
                    numericControlChanged(column, false);
                });
                control = range_widget;
            }

            this->controls.append(control);
        }

        updateControlGeometries();
    }

    void configureNumericSpinBox(QDoubleSpinBox *spin_box, int column, bool minimum,
                                 ColumnFilterKind kind)
    {
        spin_box->setDecimals(kind == ColumnFilterKind::Integer ? 0 : 6);
        spin_box->setKeyboardTracking(false);
        spin_box->setAccelerated(true);
        if (kind == ColumnFilterKind::Integer)
            spin_box->setSingleStep(1.0);
        else
            spin_box->setStepType(QAbstractSpinBox::AdaptiveDecimalStepType);
        spin_box->setToolTip((minimum ? QStringLiteral("Minimum filter for ")
                                      : QStringLiteral("Maximum filter for "))
                             + UnitTablePresentation::formattedTitle(
                                 this->model->columnTitle(column),
                                 UnitProfileManager::instance().presentation(), this->model->columnUnits(column)));
        spin_box->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

    QToolButton *createNumericResetButton(QWidget *parent, int column, bool minimum)
    {
        QToolButton *button = new QToolButton(parent);
        button->setAutoRaise(true);
        button->setIcon(style()->standardIcon(QStyle::SP_DialogResetButton));
        button->setIconSize(QSize(12, 12));
        button->setFixedSize(18, 18);
        button->setToolTip((minimum ? QStringLiteral("Reset minimum to global minimum for ")
                                    : QStringLiteral("Reset maximum to global maximum for "))
                           + this->model->columnTitle(column));
        connect(button, &QToolButton::clicked, this,
                [this, column, minimum]()
        {
            resetNumericBound(column, minimum);
        });
        return button;
    }

    void resetNumericBound(int column, bool minimum)
    {
        if (!this->numeric_controls.contains(column))
            return;

        NumericRangeControl &control = this->numeric_controls[column];
        if (minimum)
            control.selected_minimum = control.global_minimum;
        else
            control.selected_maximum = control.global_maximum;
        if (sameNumber(control.selected_minimum, control.global_minimum)
            && sameNumber(control.selected_maximum, control.global_maximum))
            this->proxy_model->clearNumericColumnFilter(column);
        else
            this->proxy_model->setNumericColumnFilter(column,
                control.selected_minimum, control.selected_maximum);
        refreshNumericControl(column);
    }

    void populateChoiceControl(QComboBox *combo_box, int column,
                               const QString &active_filter)
    {
        combo_box->clear();
        combo_box->addItem(QStringLiteral("All"), QString());

        QSet<QString> values;
        for (int row = 0; row < this->model->rowCount(); ++row)
        {
            const QString value = this->model->index(row, column)
                                      .data(Qt::DisplayRole).toString();
            if (!value.isEmpty() && value != QStringLiteral("—"))
                values.insert(value);
        }

        QStringList sorted_values;
        for (const QString &value : values)
            sorted_values.append(value);
        sorted_values.sort(Qt::CaseInsensitive);
        for (const QString &value : sorted_values)
            combo_box->addItem(value, value);

        const int active_index = combo_box->findData(active_filter);
        if (active_index >= 0)
            combo_box->setCurrentIndex(active_index);
    }

    void refreshChoiceControls()
    {
        const int column_count = qMin(this->controls.size(), this->model->columnCount());
        for (int column = 0; column < column_count; ++column)
        {
            QComboBox *combo_box = qobject_cast<QComboBox *>(this->controls.at(column));
            if (combo_box == nullptr)
                continue;

            const QString active_filter = this->proxy_model->columnFilterValue(column);
            {
                const QSignalBlocker blocker(combo_box);
                populateChoiceControl(combo_box, column, active_filter);
            }

            if (!active_filter.isEmpty() && combo_box->findData(active_filter) < 0)
            {
                this->proxy_model->setColumnFilter(column, ColumnFilterKind::Choice,
                                                   QString());
            }
        }
    }

    void refreshNumericControl(int column)
    {
        if (!this->numeric_controls.contains(column))
            return;

        NumericRangeControl &control = this->numeric_controls[column];
        double global_minimum = 0.0;
        double global_maximum = 0.0;
        if (!this->model->columnNumericRange(column, global_minimum, global_maximum))
        {
            const QSignalBlocker minimum_blocker(control.minimum);
            const QSignalBlocker maximum_blocker(control.maximum);
            control.global_minimum = 0.0;
            control.global_maximum = 0.0;
            control.selected_minimum = 0.0;
            control.selected_maximum = 0.0;
            control.minimum->setSpecialValueText(QStringLiteral("—"));
            control.maximum->setSpecialValueText(QStringLiteral("—"));
            control.minimum->setRange(0.0, 0.0);
            control.maximum->setRange(0.0, 0.0);
            control.minimum->setValue(0.0);
            control.maximum->setValue(0.0);
            control.container->setEnabled(false);
            this->proxy_model->clearNumericColumnFilter(column);
            return;
        }

        double selected_minimum = global_minimum;
        double selected_maximum = global_maximum;
        const bool has_active_filter = this->proxy_model->numericColumnFilter(
            column, selected_minimum, selected_maximum);
        selected_minimum = qBound(global_minimum, selected_minimum, global_maximum);
        selected_maximum = qBound(global_minimum, selected_maximum, global_maximum);
        if (selected_minimum > selected_maximum)
            selected_minimum = selected_maximum;

        {
            const UnitTablePresentation::ColumnUnits units = this->model->columnUnits(column);
            const UnitPresentationContext &context = UnitProfileManager::instance().presentation();
            const double display_minimum = UnitTablePresentation::toDisplayForColumn(
                context, units, global_minimum).value_or(global_minimum);
            const double display_maximum = UnitTablePresentation::toDisplayForColumn(
                context, units, global_maximum).value_or(global_maximum);
            const double display_selected_minimum = UnitTablePresentation::toDisplayForColumn(
                context, units, selected_minimum).value_or(selected_minimum);
            const double display_selected_maximum = UnitTablePresentation::toDisplayForColumn(
                context, units, selected_maximum).value_or(selected_maximum);
            const QSignalBlocker minimum_blocker(control.minimum);
            const QSignalBlocker maximum_blocker(control.maximum);
            if (units.kind == UnitTablePresentation::ColumnKind::EnergyPrice)
            {
                // A normal 6-decimal spin box would quantize per-joule prices
                // to zero. Increase decimals only as far as the visible range
                // requires; keep the stored filter bounds canonical.
                double smallest_positive = 0.0;
                for (double value : {display_minimum, display_maximum,
                                     display_selected_minimum, display_selected_maximum})
                {
                    if (value > 0.0 && (smallest_positive == 0.0 || value < smallest_positive))
                        smallest_positive = value;
                }
                int decimals = 6;
                if (smallest_positive > 0.0 && smallest_positive < 1.0)
                    decimals = std::clamp(
                        static_cast<int>(std::ceil(-std::log10(smallest_positive))) + 3,
                        6, 18);
                control.minimum->setDecimals(decimals);
                control.maximum->setDecimals(decimals);
            }
            control.global_minimum = global_minimum;
            control.global_maximum = global_maximum;
            control.selected_minimum = selected_minimum;
            control.selected_maximum = selected_maximum;
            control.minimum->setSpecialValueText(QString());
            control.maximum->setSpecialValueText(QString());
            control.minimum->setRange(display_minimum, display_maximum);
            control.maximum->setRange(display_minimum, display_maximum);
            control.minimum->setValue(display_selected_minimum);
            control.maximum->setValue(display_selected_maximum);
            control.container->setEnabled(true);
        }

        Q_UNUSED(has_active_filter);
    }

    void refreshNumericControls()
    {
        const QList<int> columns = this->numeric_controls.keys();
        for (int column : columns)
            refreshNumericControl(column);
    }

    void numericControlChanged(int column, bool minimum_changed)
    {
        if (!this->numeric_controls.contains(column))
            return;

        NumericRangeControl &control = this->numeric_controls[column];
        const UnitTablePresentation::ColumnUnits units = this->model->columnUnits(column);
        const UnitPresentationContext &context = UnitProfileManager::instance().presentation();
        const QDoubleSpinBox *changed = minimum_changed ? control.minimum : control.maximum;
        const std::optional<double> canonical = UnitTablePresentation::toCanonicalForColumn(
            context, units, changed->value());
        if (!canonical.has_value())
            return;
        double minimum = minimum_changed ? *canonical : control.selected_minimum;
        double maximum = minimum_changed ? control.selected_maximum : *canonical;
        if (minimum > maximum)
        {
            if (minimum_changed) maximum = minimum;
            else minimum = maximum;
        }
        control.selected_minimum = minimum;
        control.selected_maximum = maximum;

        if (sameNumber(minimum, control.global_minimum)
            && sameNumber(maximum, control.global_maximum))
        {
            this->proxy_model->clearNumericColumnFilter(column);
            refreshNumericControl(column);
            return;
        }

        this->proxy_model->setNumericColumnFilter(column, minimum, maximum);
        refreshNumericControl(column);
    }

    void updateControlGeometries()
    {
        QHeaderView *header = this->table->horizontalHeader();
        const int x_offset = header->geometry().x();
        for (int column = 0; column < this->controls.size(); ++column)
        {
            QWidget *control = this->controls.at(column);
            if (control == nullptr)
                continue;

            const int x = x_offset + header->sectionViewportPosition(column);
            const int section_width = header->sectionSize(column);
            if (this->numeric_controls.contains(column))
            {
                control->setGeometry(x + 2, 1, qMax(0, section_width - 4), height() - 2);
            }
            else
            {
                const int control_height = 28;
                control->setGeometry(x + 2, (height() - control_height) / 2,
                                     qMax(0, section_width - 4), control_height);
            }
            control->setVisible(x + section_width > 0 && x < width());
        }
    }

    QTableView *table;
    HydraulicEntityTableModel *model;
    HydraulicEntityFilterProxyModel *proxy_model;
    QList<QWidget *> controls;
    QHash<int, NumericRangeControl> numeric_controls;
};

HydraulicEntityTableWidget::HydraulicEntityTableWidget(HydraulicData *hydraulic_data,
                                                       InfrastructureEntity entity_type,
                                                       const QString &entity_plural,
                                                       QWidget *parent)
    : QWidget(parent),
      hydraulic_data(hydraulic_data),
      entity_type(entity_type),
      label_help(new QLabel(this)),
      table(new QTableView(this)),
      model(new HydraulicEntityTableModel(hydraulic_data, entity_type, this)),
      proxy_model(new HydraulicEntityFilterProxyModel(this))
{
    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(8);

    this->proxy_model->setSourceModel(this->model);
    this->proxy_model->setSortRole(SortRole);
    this->proxy_model->setDynamicSortFilter(true);

    this->table->setModel(this->proxy_model);
    LiveUiText::bind(this->table, [this] {
        if (this->model->columnCount() > 0)
            emit this->model->headerDataChanged(Qt::Horizontal, 0, this->model->columnCount() - 1);
    });
    this->table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    this->table->setSelectionBehavior(QAbstractItemView::SelectRows);
    this->table->setSelectionMode(QAbstractItemView::SingleSelection);
    this->table->setAlternatingRowColors(true);
    this->table->setSortingEnabled(true);
    this->table->setWordWrap(false);
    this->table->setTextElideMode(Qt::ElideRight);
    this->table->verticalHeader()->setVisible(false);
    this->table->verticalHeader()->setDefaultSectionSize(28);
    this->table->horizontalHeader()->setSectionsClickable(true);
    this->table->horizontalHeader()->setSectionsMovable(true);
    this->table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    this->table->horizontalHeader()->setDefaultAlignment(Qt::AlignCenter);
    this->table->horizontalHeader()->setTextElideMode(Qt::ElideNone);
    const QFontMetrics header_font_metrics(this->table->horizontalHeader()->font());
    this->table->horizontalHeader()->setFixedHeight(header_font_metrics.lineSpacing() * 3 + 12);
    this->table->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    this->table->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    this->table->viewport()->setCursor(Qt::PointingHandCursor);

    HydraulicEntityFilterBar *filter_bar = new HydraulicEntityFilterBar(
        this->table, this->model,
        static_cast<HydraulicEntityFilterProxyModel *>(this->proxy_model), this);
    layout->addWidget(filter_bar);
    layout->addWidget(this->table, 1);

    LiveUiText::bind(this->label_help, [this, entity_plural] {
        const QString entities = QCoreApplication::translate("MainNavigationWidget", entity_plural.toUtf8().constData());
        this->label_help->setText(LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "<b>Hint:</b> Create %1 in the Map Editor.")).arg(entities));
    });
    this->label_help->setTextFormat(Qt::RichText);
    this->label_help->setWordWrap(true);
    this->label_help->setMargin(8);
    this->label_help->setFrameShape(QFrame::StyledPanel);
    this->label_help->setBackgroundRole(QPalette::AlternateBase);
    this->label_help->setAutoFillBackground(true);

    QWidget *bottom_row = new QWidget(this);
    QHBoxLayout *bottom_layout = new QHBoxLayout(bottom_row);
    bottom_layout->setContentsMargins(0, 0, 0, 0);
    bottom_layout->setSpacing(8);

    QPushButton *reset_filters_button = new QPushButton(QStringLiteral("Reset filters"), bottom_row);
    reset_filters_button->setToolTip(QStringLiteral("Reset all table filters"));
    reset_filters_button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    bottom_layout->addWidget(reset_filters_button, 0, Qt::AlignVCenter);

    QLabel *simulation_filter_label = new QLabel(QStringLiteral("Simulation:"), bottom_row);
    bottom_layout->addWidget(simulation_filter_label, 0, Qt::AlignVCenter);

    QComboBox *simulation_filter = new QComboBox(bottom_row);
    simulation_filter->addItem(QStringLiteral("All"),
                               static_cast<int>(SimulationStatusFilter::All));
    simulation_filter->addItem(QStringLiteral("Warnings & errors"),
                               static_cast<int>(SimulationStatusFilter::WarningsAndErrors));
    simulation_filter->addItem(QStringLiteral("Warnings only"),
                               static_cast<int>(SimulationStatusFilter::WarningsOnly));
    simulation_filter->addItem(QStringLiteral("Errors only"),
                               static_cast<int>(SimulationStatusFilter::ErrorsOnly));
    simulation_filter->setToolTip(QStringLiteral("Filter entities by simulation diagnostics"));
    simulation_filter->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    LiveUiText::bind(simulation_filter, [simulation_filter, simulation_filter_label, reset_filters_button] {
        const QSignalBlocker blocker(simulation_filter);
        simulation_filter_label->setText(LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "Simulation:")));
        reset_filters_button->setText(LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "Reset filters")));
        reset_filters_button->setToolTip(LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "Reset all table filters")));
        simulation_filter->setToolTip(LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "Filter entities by simulation diagnostics")));
        simulation_filter->setItemText(0, LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "All")));
        simulation_filter->setItemText(1, LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "Warnings & errors")));
        simulation_filter->setItemText(2, LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "Warnings only")));
        simulation_filter->setItemText(3, LiveUiText::translate(QT_TRANSLATE_NOOP("LiveUiText", "Errors only")));
    });
    bottom_layout->addWidget(simulation_filter, 0, Qt::AlignVCenter);

    bottom_layout->addWidget(this->label_help, 1);
    layout->addWidget(bottom_row);

    connect(simulation_filter, &QComboBox::currentIndexChanged, this,
            [this, simulation_filter](int)
    {
        const SimulationStatusFilter filter = static_cast<SimulationStatusFilter>(
            simulation_filter->currentData().toInt());
        static_cast<HydraulicEntityFilterProxyModel *>(this->proxy_model)
            ->setSimulationStatusFilter(filter);
    });

    connect(reset_filters_button, &QPushButton::clicked, filter_bar,
            [filter_bar, simulation_filter]()
    {
        {
            const QSignalBlocker blocker(simulation_filter);
            simulation_filter->setCurrentIndex(0);
        }
        filter_bar->resetFilters();
    });

    this->table->sortByColumn(0, Qt::AscendingOrder);

    connect(this->table, &QTableView::clicked, this,
            [this](const QModelIndex &index)
    {
        openEntity(index);
    });

    connect(this->hydraulic_data, &HydraulicData::signalNetworkLoaded, this,
            [this]()
    {
        this->resize_columns_after_rebuild = true;
        requestRebuild();
    });
    connect(this->hydraulic_data, &HydraulicData::signalNetworkGeometryChanged, this,
            [this](quint64)
    {
        requestRebuild();
    });
    connect(this->hydraulic_data, &HydraulicData::signalNodeChanged, this,
            [this](InfrastructureEntity changed_type, const QUuid &)
    {
        const bool table_is_node = this->entity_type == InfrastructureEntity::Junction
            || this->entity_type == InfrastructureEntity::Reservoir
            || this->entity_type == InfrastructureEntity::Tank;
        const bool table_is_link = this->entity_type == InfrastructureEntity::Pipe
            || this->entity_type == InfrastructureEntity::Pump
            || this->entity_type == InfrastructureEntity::Valve;
        const bool table_is_demand_point =
            this->entity_type == InfrastructureEntity::DemandPoint;

        if ((table_is_node && changed_type == this->entity_type) || table_is_link
            || (table_is_demand_point && changed_type == InfrastructureEntity::Junction))
        {
            requestRebuild();
        }
    });
    connect(this->hydraulic_data, &HydraulicData::signalLinkChanged, this,
            [this](InfrastructureEntity changed_type, const QUuid &)
    {
        if (changed_type == this->entity_type
            || (this->entity_type == InfrastructureEntity::DemandPoint
                && changed_type == InfrastructureEntity::Pipe))
        {
            requestRebuild();
        }
    });
    connect(this->hydraulic_data, &HydraulicData::signalDemandPointChanged, this,
            [this](const QUuid &)
    {
        if (this->entity_type == InfrastructureEntity::DemandPoint)
            requestRebuild();
    });
    connect(this->hydraulic_data, &HydraulicData::signalSimulationResultTimelineChanged, this,
            [this](bool)
    {
        requestRebuild();
    });
    connect(this->hydraulic_data, &HydraulicData::signalWaterQualitySimulationResultTimelineChanged, this,
            [this](bool)
    {
        requestRebuild();
    });
    connect(this->hydraulic_data, &HydraulicData::signalCurrentSimulationResultChanged, this,
            [this](int)
    {
        requestRebuild();
    });

    new UnitAwareView(this, UnitProfileManager::instance().presentation(),
        [this, filter_bar]()
    {
        this->model->refreshPresentation();
        filter_bar->refreshUnitControls();
        static_cast<HydraulicEntityFilterProxyModel *>(this->proxy_model)
            ->refreshUnitDependentTextFilters();
        this->table->viewport()->update();
    });
}

void HydraulicEntityTableWidget::requestRebuild()
{
    this->rebuild_pending = true;
    if (!isVisible() || this->rebuild_scheduled)
        return;

    this->rebuild_scheduled = true;
    QTimer::singleShot(0, this, [this]()
    {
        this->rebuild_scheduled = false;
        if (!isVisible() || !this->rebuild_pending)
            return;

        this->rebuild_pending = false;
        this->model->rebuild();

        if (this->resize_columns_after_rebuild)
        {
            this->resize_columns_after_rebuild = false;
            this->table->resizeColumnsToContents();
        }
    });
}

void HydraulicEntityTableWidget::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    if (this->rebuild_pending)
        requestRebuild();
}

void HydraulicEntityTableWidget::openEntity(const QModelIndex &proxy_index)
{
    if (!proxy_index.isValid())
        return;

    const QModelIndex source_index = this->proxy_model->mapToSource(proxy_index);
    const QUuid uuid = source_index.data(EntityUuidRole).toUuid();
    if (uuid.isNull())
        return;

    this->hydraulic_data->setSelectedUuid(this->entity_type, uuid);
}
