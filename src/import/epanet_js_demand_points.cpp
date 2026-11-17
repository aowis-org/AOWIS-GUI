#include "import/epanet_js_demand_points.h"
#include "import/epanet_js_conversion_common.h"
#include "import/epanet_js_geometry.h"
#include "import/epanet_js_schema.h"
#include "import/epanet_js_units.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QHash>
#include <QSet>
#include <QVariant>

#include <algorithm>
#include <cmath>
#include <optional>

namespace EpanetJsDemandPoints
{
using namespace EpanetJsConversionCommon;
using namespace EpanetJsGeometry;
using namespace EpanetJsSchema;
using namespace EpanetJsUnits;

void importCustomerPoints(
    const EpanetJsProjectSnapshot &project,
    const QJsonObject &project_settings,
    EpanetJsProjectConversionResult &result)
{
    const EpanetJsTableSnapshot *points_table = tableByName(
        project, QStringLiteral("customer_points"));
    if (points_table == nullptr)
        return;

    // These network collections are complete at this import stage. Build
    // per-stage indexes once instead of scanning them for every customer point.
    // Store positions rather than pointers so container storage remains owned
    // by NetworkHydraulicDistribution.
    const QHash<QUuid, qsizetype> pipe_positions = uuidPositionIndex(result.network.links_pipes);

    // Preserve the original junction -> reservoir -> tank lookup precedence,
    // including first-match behavior for duplicate UUIDs.
    QHash<QUuid, CoordinateWGS84> node_coordinates;
    for (const HydraulicNodeJunction &junction : result.network.nodes_junctions)
    {
        if (!node_coordinates.contains(junction.uuid))
            node_coordinates.insert(junction.uuid, junction.coordinate_wgs84);
    }
    for (const HydraulicNodeReservoir &reservoir : result.network.nodes_reservoirs)
    {
        if (!node_coordinates.contains(reservoir.uuid))
            node_coordinates.insert(reservoir.uuid, reservoir.coordinate_wgs84);
    }
    for (const HydraulicNodeTank &tank : result.network.nodes_tanks)
    {
        if (!node_coordinates.contains(tank.uuid))
            node_coordinates.insert(tank.uuid, tank.coordinate_wgs84);
    }

    const QSet<QUuid> junction_uuids = uuidMembershipIndex(result.network.nodes_junctions);

    const QSet<QUuid> pattern_uuids = uuidMembershipIndex(result.network.patterns_time);

    const QJsonObject units = projectUnitsObject(project_settings);
    const QString demand_unit = firstUnit(units, QStringList{QStringLiteral("customerDemand")});

    struct SourceDemand
    {
        qint64 customer_point_id = 0;
        qint64 ordinal = 0;
        QVariantMap row;
    };

    QMap<qint64, QList<HydraulicDemand>> demands_by_customer_point;
    const EpanetJsTableSnapshot *demands_table = tableByName(
        project, QStringLiteral("customer_point_demands"));
    if (demands_table != nullptr)
    {
        QList<SourceDemand> source_demands;
        for (const QVariantMap &row : demands_table->rows)
        {
            const std::optional<qint64> customer_point_id = integerValue(
                row.value(QStringLiteral("customer_point_id")));
            const std::optional<qint64> ordinal = integerValue(
                row.value(QStringLiteral("ordinal")));
            if (!customer_point_id.has_value() || !ordinal.has_value())
            {
                appendDiagnostic(
                    result,
                    EpanetJsConversionDiagnosticSeverity::Error,
                    QStringLiteral("invalid-customer-demand-reference"),
                    QStringLiteral("epanet-js customer_point_demands contains an invalid customer_point_id or ordinal."),
                    QStringLiteral("customer_point_demands"));
                continue;
            }
            if (!result.id_map.contains(QStringLiteral("customer_points"), *customer_point_id))
            {
                appendDiagnostic(
                    result,
                    EpanetJsConversionDiagnosticSeverity::Error,
                    QStringLiteral("missing-customer-demand-point-reference"),
                    QStringLiteral("epanet-js customer demand references missing customer point id %1.")
                        .arg(*customer_point_id),
                    QStringLiteral("customer_point_demands"),
                    *customer_point_id);
                continue;
            }

            SourceDemand source;
            source.customer_point_id = *customer_point_id;
            source.ordinal = *ordinal;
            source.row = row;
            source_demands.append(source);
        }

        std::sort(
            source_demands.begin(), source_demands.end(),
            [](const SourceDemand &left, const SourceDemand &right)
            {
                if (left.customer_point_id != right.customer_point_id)
                    return left.customer_point_id < right.customer_point_id;
                return left.ordinal < right.ordinal;
            });

        for (const SourceDemand &source : source_demands)
        {
            const std::optional<double> source_base_demand = finiteVariantDouble(
                source.row.value(QStringLiteral("base_demand")));
            const std::optional<double> base_demand = source_base_demand.has_value()
                ? flowToM3PerH(*source_base_demand, demand_unit)
                : std::nullopt;
            if (!base_demand.has_value())
            {
                appendDiagnostic(
                    result,
                    EpanetJsConversionDiagnosticSeverity::Error,
                    QStringLiteral("unsupported-customer-demand"),
                    QStringLiteral("AOWIS cannot convert epanet-js customer point %1 demand ordinal %2 with unit '%3'.")
                        .arg(source.customer_point_id)
                        .arg(source.ordinal)
                        .arg(demand_unit),
                    QStringLiteral("customer_point_demands"),
                    source.customer_point_id);
                continue;
            }

            HydraulicDemand demand;
            demand.base_demand_m3_per_h = *base_demand;

            if (source.row.contains(QStringLiteral("pattern_id"))
                && !source.row.value(QStringLiteral("pattern_id")).isNull())
            {
                const std::optional<qint64> pattern_id = integerValue(
                    source.row.value(QStringLiteral("pattern_id")));
                const QUuid pattern_uuid = pattern_id.has_value()
                    ? result.id_map.uuidFor(QStringLiteral("patterns"), *pattern_id)
                    : QUuid();
                if (pattern_id.has_value() && *pattern_id <= 0)
                {
                    // EPANET-style zero references mean no pattern.
                }
                else if (!pattern_id.has_value() || pattern_uuid.isNull()
                    || !pattern_uuids.contains(pattern_uuid))
                {
                    appendDiagnostic(
                        result,
                        EpanetJsConversionDiagnosticSeverity::Error,
                        QStringLiteral("missing-customer-demand-pattern-reference"),
                        QStringLiteral("epanet-js customer point %1 demand ordinal %2 references a missing pattern.")
                            .arg(source.customer_point_id)
                            .arg(source.ordinal),
                        QStringLiteral("customer_point_demands"),
                        source.customer_point_id);
                }
                else
                {
                    demand.pattern_mode = HydraulicTimePatternMode::TimePattern;
                    demand.pattern_uuid = pattern_uuid;
                }
            }

            demands_by_customer_point[source.customer_point_id].append(demand);
        }
    }

    for (const QVariantMap &row : points_table->rows)
    {
        const std::optional<qint64> source_id = integerValue(row.value(QStringLiteral("id")));
        if (!source_id.has_value()
            || !result.id_map.contains(QStringLiteral("customer_points"), *source_id))
        {
            continue;
        }

        HydraulicDemandPoint demand_point;
        demand_point.id = importedEntityId(
            row, QStringLiteral("customer_points"), *source_id, result);
        demand_point.uuid = result.id_map.uuidFor(
            QStringLiteral("customer_points"), *source_id);
        importStoredWgs84Coordinate(
            row, QStringLiteral("customer_points"), *source_id,
            demand_point.coordinate_wgs84, result);
        demand_point.demands = demands_by_customer_point.value(*source_id);

        const bool has_pipe_reference = row.contains(QStringLiteral("pipe_id"))
            && !row.value(QStringLiteral("pipe_id")).isNull();
        const bool has_junction_reference = row.contains(QStringLiteral("junction_id"))
            && !row.value(QStringLiteral("junction_id")).isNull();
        const std::optional<qint64> pipe_id = has_pipe_reference
            ? integerValue(row.value(QStringLiteral("pipe_id")))
            : std::nullopt;
        const std::optional<qint64> junction_id = has_junction_reference
            ? integerValue(row.value(QStringLiteral("junction_id")))
            : std::nullopt;

        if (has_pipe_reference && !pipe_id.has_value())
        {
            appendDiagnostic(
                result,
                EpanetJsConversionDiagnosticSeverity::Error,
                QStringLiteral("invalid-customer-pipe-reference"),
                QStringLiteral("epanet-js customer point id %1 has an invalid pipe_id.")
                    .arg(*source_id),
                QStringLiteral("customer_points"),
                *source_id);
        }
        else if (has_junction_reference && !junction_id.has_value())
        {
            appendDiagnostic(
                result,
                EpanetJsConversionDiagnosticSeverity::Error,
                QStringLiteral("invalid-customer-junction-reference"),
                QStringLiteral("epanet-js customer point id %1 has an invalid junction_id.")
                    .arg(*source_id),
                QStringLiteral("customer_points"),
                *source_id);
        }
        else if (pipe_id.has_value())
        {
            const QUuid pipe_uuid = result.id_map.uuidFor(
                QStringLiteral("pipes"), *pipe_id);
            const auto pipe_position = pipe_positions.constFind(pipe_uuid);
            const HydraulicLinkPipe *pipe = pipe_position == pipe_positions.cend()
                ? nullptr : &result.network.links_pipes.at(*pipe_position);
            if (pipe == nullptr)
            {
                appendDiagnostic(
                    result,
                    EpanetJsConversionDiagnosticSeverity::Error,
                    QStringLiteral("missing-customer-pipe-reference"),
                    QStringLiteral("epanet-js customer point id %1 references missing pipe id %2.")
                        .arg(*source_id)
                        .arg(*pipe_id),
                    QStringLiteral("customer_points"),
                    *source_id);
            }
            else if (!junction_id.has_value())
            {
                appendDiagnostic(
                    result,
                    EpanetJsConversionDiagnosticSeverity::Error,
                    QStringLiteral("missing-customer-assigned-junction"),
                    QStringLiteral("epanet-js customer point id %1 is pipe-attached but has no assigned junction.")
                        .arg(*source_id),
                    QStringLiteral("customer_points"),
                    *source_id);
            }
            else
            {
                const QUuid assigned_junction_uuid = result.id_map.uuidFor(
                    QStringLiteral("junctions"), *junction_id);
                if (assigned_junction_uuid.isNull()
                    || !junction_uuids.contains(assigned_junction_uuid))
                {
                    appendDiagnostic(
                        result,
                        EpanetJsConversionDiagnosticSeverity::Error,
                        QStringLiteral("missing-customer-junction-reference"),
                        QStringLiteral("epanet-js customer point id %1 references missing junction id %2.")
                            .arg(*source_id)
                            .arg(*junction_id),
                        QStringLiteral("customer_points"),
                        *source_id);
                }
                else if (assigned_junction_uuid != pipe->node_uuid_from
                    && assigned_junction_uuid != pipe->node_uuid_to)
                {
                    appendDiagnostic(
                        result,
                        EpanetJsConversionDiagnosticSeverity::Error,
                        QStringLiteral("invalid-customer-assigned-junction"),
                        QStringLiteral("epanet-js customer point id %1 assigns demand to junction id %2, which is not an endpoint of pipe id %3.")
                            .arg(*source_id)
                            .arg(*junction_id)
                            .arg(*pipe_id),
                        QStringLiteral("customer_points"),
                        *source_id);
                }
                else
                {
                    CoordinateWGS84 snap_coordinate = demand_point.coordinate_wgs84;
                    const std::optional<double> snap_x = finiteVariantDouble(
                        row.value(QStringLiteral("snap_x")));
                    const std::optional<double> snap_y = finiteVariantDouble(
                        row.value(QStringLiteral("snap_y")));
                    if (snap_x.has_value() && snap_y.has_value()
                        && *snap_x >= -180.0 && *snap_x <= 180.0
                        && *snap_y >= -90.0 && *snap_y <= 90.0)
                    {
                        snap_coordinate.longitude_deg = *snap_x;
                        snap_coordinate.latitude_deg = *snap_y;
                    }
                    else
                    {
                        appendDiagnostic(
                            result,
                            EpanetJsConversionDiagnosticSeverity::Warning,
                            QStringLiteral("missing-customer-snap-coordinate"),
                            QStringLiteral("epanet-js customer point id %1 has no valid snap coordinate; AOWIS projected the customer point coordinate onto its pipe instead.")
                                .arg(*source_id),
                            QStringLiteral("customer_points"),
                            *source_id);
                    }

                    const std::optional<double> pipe_position = normalizedPipePosition(
                        node_coordinates, *pipe, snap_coordinate);
                    if (!pipe_position.has_value())
                    {
                        appendDiagnostic(
                            result,
                            EpanetJsConversionDiagnosticSeverity::Error,
                            QStringLiteral("invalid-customer-pipe-position"),
                            QStringLiteral("AOWIS could not resolve the attachment position of epanet-js customer point id %1 on pipe id %2.")
                                .arg(*source_id)
                                .arg(*pipe_id),
                            QStringLiteral("customer_points"),
                            *source_id);
                    }
                    else
                    {
                        demand_point.attachment.type = HydraulicDemandPointAttachmentType::Pipe;
                        demand_point.attachment.pipe_uuid = pipe_uuid;
                        demand_point.attachment.pipe_position = *pipe_position;
                        demand_point.attachment.pipe_allocation_mode =
                            HydraulicDemandPointPipeAllocationMode::AssignedJunction;
                        demand_point.attachment.pipe_assigned_junction_uuid =
                            assigned_junction_uuid;
                    }
                }
            }
        }
        else if (junction_id.has_value())
        {
            const QUuid junction_uuid = result.id_map.uuidFor(
                QStringLiteral("junctions"), *junction_id);
            if (junction_uuid.isNull()
                || !junction_uuids.contains(junction_uuid))
            {
                appendDiagnostic(
                    result,
                    EpanetJsConversionDiagnosticSeverity::Error,
                    QStringLiteral("missing-customer-junction-reference"),
                    QStringLiteral("epanet-js customer point id %1 references missing junction id %2.")
                        .arg(*source_id)
                        .arg(*junction_id),
                    QStringLiteral("customer_points"),
                    *source_id);
            }
            else
            {
                demand_point.attachment.type = HydraulicDemandPointAttachmentType::Junction;
                demand_point.attachment.junction_uuid = junction_uuid;
            }
        }
        else if (!demand_point.demands.isEmpty())
        {
            appendDiagnostic(
                result,
                EpanetJsConversionDiagnosticSeverity::Error,
                QStringLiteral("unattached-customer-demand"),
                QStringLiteral("epanet-js customer point id %1 has demand but no network attachment.")
                    .arg(*source_id),
                QStringLiteral("customer_points"),
                *source_id);
        }

        result.network.demand_points.append(demand_point);
    }
}
}
