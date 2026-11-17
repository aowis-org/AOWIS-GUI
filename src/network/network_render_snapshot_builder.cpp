#include "network/network_render_snapshot_builder.h"
#include "network/network_symbology_values.h"

#include <QHash>
#include <QVector>

#include <cmath>
#include <functional>
#include <limits>
#include <optional>

namespace
{
constexpr double Pi = 3.14159265358979323846;

double approximateCoordinateDistance(const CoordinateWGS84 &first,
                                     const CoordinateWGS84 &second)
{
    const double average_latitude_rad =
        (first.latitude_deg + second.latitude_deg) * Pi / 360.0;
    const double longitude_delta =
        (second.longitude_deg - first.longitude_deg) * std::cos(average_latitude_rad);
    const double latitude_delta = second.latitude_deg - first.latitude_deg;
    return std::hypot(longitude_delta, latitude_delta);
}

std::optional<CoordinateWGS84> coordinateAtPipePosition(
    const HydraulicLinkPipe &pipe, double position,
    const QHash<QUuid, CoordinateWGS84> &node_coordinates)
{
    if (!std::isfinite(position) || position < 0.0 || position > 1.0)
        return std::nullopt;

    const QHash<QUuid, CoordinateWGS84>::const_iterator start_iterator =
        node_coordinates.constFind(pipe.node_uuid_from);
    const QHash<QUuid, CoordinateWGS84>::const_iterator end_iterator =
        node_coordinates.constFind(pipe.node_uuid_to);
    if (start_iterator == node_coordinates.cend() || end_iterator == node_coordinates.cend())
        return std::nullopt;

    QList<CoordinateWGS84> coordinates;
    coordinates.reserve(pipe.vertices.size() + 2);
    coordinates.append(start_iterator.value());
    for (const HydraulicLinkVertex &vertex : pipe.vertices)
        coordinates.append(vertex.coordinate_wgs84);
    coordinates.append(end_iterator.value());

    QList<double> segment_lengths;
    segment_lengths.reserve(coordinates.size() - 1);
    double total_length = 0.0;
    for (qsizetype index = 1; index < coordinates.size(); ++index)
    {
        const double length = approximateCoordinateDistance(
            coordinates.at(index - 1), coordinates.at(index));
        segment_lengths.append(length);
        total_length += length;
    }
    if (!std::isfinite(total_length) || total_length <= 0.0)
        return std::nullopt;

    const double target_length = position * total_length;
    double accumulated_length = 0.0;
    for (qsizetype index = 1; index < coordinates.size(); ++index)
    {
        const double segment_length = segment_lengths.at(index - 1);
        if (target_length > accumulated_length + segment_length
            && index + 1 < coordinates.size())
        {
            accumulated_length += segment_length;
            continue;
        }

        const double fraction = segment_length > 0.0
            ? qBound(0.0, (target_length - accumulated_length) / segment_length, 1.0)
            : 0.0;
        const CoordinateWGS84 &from = coordinates.at(index - 1);
        const CoordinateWGS84 &to = coordinates.at(index);
        CoordinateWGS84 result;
        result.latitude_deg = from.latitude_deg
            + (to.latitude_deg - from.latitude_deg) * fraction;
        double longitude_delta = to.longitude_deg - from.longitude_deg;
        while (longitude_delta > 180.0)
            longitude_delta -= 360.0;
        while (longitude_delta < -180.0)
            longitude_delta += 360.0;
        result.longitude_deg = from.longitude_deg + longitude_delta * fraction;
        while (result.longitude_deg > 180.0)
            result.longitude_deg -= 360.0;
        while (result.longitude_deg < -180.0)
            result.longitude_deg += 360.0;
        return result;
    }

    return coordinates.constLast();
}
}

template <typename LinkType>
static bool appendNetworkRenderLink(const LinkType &source, InfrastructureEntity entity_type,
                                    quint32 render_id,
                                    const QList<NetworkRenderNode> &nodes,
                                    const QHash<QUuid, qsizetype> &node_indices,
                                    QList<NetworkRenderLink> &links)
{
    const QHash<QUuid, qsizetype>::const_iterator start_iterator =
        node_indices.constFind(source.node_uuid_from);
    const QHash<QUuid, qsizetype>::const_iterator end_iterator =
        node_indices.constFind(source.node_uuid_to);

    if (start_iterator == node_indices.cend() || end_iterator == node_indices.cend())
        return false;

    const NetworkRenderNode &start_node = nodes.at(start_iterator.value());
    const NetworkRenderNode &end_node = nodes.at(end_iterator.value());

    NetworkRenderLink link;
    link.render_id = render_id;
    link.id = source.id;
    link.uuid = source.uuid;
    link.entity_type = entity_type;
    link.start_node_render_id = start_node.render_id;
    link.end_node_render_id = end_node.render_id;
    link.vertices_wgs84.reserve(source.vertices.size() + 2);
    link.vertices_wgs84.append(start_node.coordinate_wgs84);
    for (const HydraulicLinkVertex &vertex : source.vertices)
        link.vertices_wgs84.append(vertex.coordinate_wgs84);
    link.vertices_wgs84.append(end_node.coordinate_wgs84);

    link.elevations_m.reserve(link.vertices_wgs84.size());
    QVector<double> cumulative_distance;
    cumulative_distance.reserve(link.vertices_wgs84.size());
    cumulative_distance.append(0.0);
    double total_distance = 0.0;
    for (qsizetype index = 1; index < link.vertices_wgs84.size(); ++index)
    {
        total_distance += approximateCoordinateDistance(
            link.vertices_wgs84.at(index - 1), link.vertices_wgs84.at(index));
        cumulative_distance.append(total_distance);
    }

    for (qsizetype index = 0; index < link.vertices_wgs84.size(); ++index)
    {
        double fraction = 0.0;
        if (total_distance > 0.0)
            fraction = cumulative_distance.at(index) / total_distance;
        else if (link.vertices_wgs84.size() > 1)
            fraction = double(index) / double(link.vertices_wgs84.size() - 1);

        link.elevations_m.append(
            start_node.elevation_m
            + (end_node.elevation_m - start_node.elevation_m) * fraction);
    }

    links.append(link);
    return true;
}

NetworkRenderSnapshot buildNetworkRenderSnapshot(const NetworkHydraulicDistribution &network,
                                                 quint64 geometry_revision,
                                                 quint64 visual_revision)
{
    NetworkRenderSnapshot snapshot;
    snapshot.geometry_revision = geometry_revision;
    snapshot.visual_revision = visual_revision;

    const qsizetype node_count = network.nodes_junctions.size() +
                                 network.nodes_reservoirs.size() +
                                 network.nodes_tanks.size() +
                                 network.demand_points.size();
    const qsizetype link_count = network.links_pipes.size() +
                                 network.links_pumps.size() +
                                 network.links_valves.size();
    snapshot.nodes.reserve(node_count);
    snapshot.links.reserve(link_count);

    QHash<QUuid, qsizetype> node_indices;
    node_indices.reserve(node_count);
    QHash<QUuid, CoordinateWGS84> connection_node_coordinates;
    connection_node_coordinates.reserve(
        network.nodes_junctions.size() + network.nodes_reservoirs.size()
        + network.nodes_tanks.size());
    QHash<QUuid, quint32> demand_point_render_ids;
    demand_point_render_ids.reserve(network.demand_points.size());
    quint32 next_node_render_id = 1;

    const auto add_node = [&snapshot, &node_indices, &connection_node_coordinates,
                           &next_node_render_id](
        const auto &source, InfrastructureEntity entity_type)
    {
        NetworkRenderNode node;
        node.render_id = next_node_render_id++;
        node.id = source.id;
        node.uuid = source.uuid;
        node.entity_type = entity_type;
        node.coordinate_wgs84 = source.coordinate_wgs84;
        node.elevation_m = resolvedSymbologyElevationM(source);
        node_indices.insert(node.uuid, snapshot.nodes.size());
        connection_node_coordinates.insert(node.uuid, node.coordinate_wgs84);
        snapshot.nodes.append(node);
    };

    for (const HydraulicNodeJunction &source : network.nodes_junctions)
        add_node(source, InfrastructureEntity::Junction);
    for (const HydraulicNodeReservoir &source : network.nodes_reservoirs)
        add_node(source, InfrastructureEntity::Reservoir);
    for (const HydraulicNodeTank &source : network.nodes_tanks)
        add_node(source, InfrastructureEntity::Tank);
    for (const HydraulicDemandPoint &source : network.demand_points)
    {
        NetworkRenderNode node;
        node.render_id = next_node_render_id++;
        node.id = source.id;
        node.uuid = source.uuid;
        node.entity_type = InfrastructureEntity::DemandPoint;
        node.coordinate_wgs84 = source.coordinate_wgs84;
        node.elevation_m = resolvedSymbologyElevationM(source);
        demand_point_render_ids.insert(source.uuid, node.render_id);
        snapshot.nodes.append(node);
    }

    quint32 next_link_render_id = 1;
    for (const HydraulicLinkPipe &source : network.links_pipes)
    {
        if (appendNetworkRenderLink(source, InfrastructureEntity::Pipe, next_link_render_id,
                                    snapshot.nodes, node_indices, snapshot.links))
            ++next_link_render_id;
    }
    for (const HydraulicLinkPump &source : network.links_pumps)
    {
        if (appendNetworkRenderLink(source, InfrastructureEntity::Pump, next_link_render_id,
                                    snapshot.nodes, node_indices, snapshot.links))
            ++next_link_render_id;
    }
    for (const HydraulicLinkValve &source : network.links_valves)
    {
        if (appendNetworkRenderLink(source, InfrastructureEntity::Valve, next_link_render_id,
                                    snapshot.nodes, node_indices, snapshot.links))
            ++next_link_render_id;
    }

    QHash<QUuid, const HydraulicLinkPipe *> pipes_by_uuid;
    pipes_by_uuid.reserve(network.links_pipes.size());
    for (const HydraulicLinkPipe &pipe : network.links_pipes)
        pipes_by_uuid.insert(pipe.uuid, &pipe);

    // Elevation of a connection node (junction, reservoir, tank); NaN when
    // it is not in the snapshot.
    const std::function<double(const QUuid &)> node_elevation =
        [&snapshot, &node_indices](const QUuid &uuid)
    {
        const QHash<QUuid, qsizetype>::const_iterator iterator = node_indices.constFind(uuid);
        return iterator != node_indices.cend()
            ? snapshot.nodes.at(iterator.value()).elevation_m
            : std::numeric_limits<double>::quiet_NaN();
    };

    snapshot.demand_point_attachments.reserve(network.demand_points.size());
    for (const HydraulicDemandPoint &demand_point : network.demand_points)
    {
        CoordinateWGS84 attachment_coordinate;
        double attachment_elevation_m = std::numeric_limits<double>::quiet_NaN();
        bool attachment_valid = false;
        if (demand_point.attachment.type == HydraulicDemandPointAttachmentType::Junction)
        {
            const QHash<QUuid, CoordinateWGS84>::const_iterator iterator =
                connection_node_coordinates.constFind(demand_point.attachment.junction_uuid);
            if (iterator != connection_node_coordinates.cend())
            {
                attachment_coordinate = iterator.value();
                attachment_elevation_m = node_elevation(demand_point.attachment.junction_uuid);
                attachment_valid = true;
            }
        }
        else if (demand_point.attachment.type == HydraulicDemandPointAttachmentType::Pipe)
        {
            const QHash<QUuid, const HydraulicLinkPipe *>::const_iterator pipe_iterator =
                pipes_by_uuid.constFind(demand_point.attachment.pipe_uuid);
            if (pipe_iterator != pipes_by_uuid.cend())
            {
                const std::optional<CoordinateWGS84> coordinate = coordinateAtPipePosition(
                    *pipe_iterator.value(), demand_point.attachment.pipe_position,
                    connection_node_coordinates);
                if (coordinate.has_value())
                {
                    attachment_coordinate = coordinate.value();
                    // pipe_position is the fraction of the pipe's length,
                    // which is also how the link's elevations interpolate
                    // between its end nodes (appendNetworkRenderLink()).
                    const double elevation_from_m = node_elevation(pipe_iterator.value()->node_uuid_from);
                    const double elevation_to_m = node_elevation(pipe_iterator.value()->node_uuid_to);
                    attachment_elevation_m = elevation_from_m
                        + (elevation_to_m - elevation_from_m) * demand_point.attachment.pipe_position;
                    attachment_valid = true;
                }
            }
        }

        if (!attachment_valid)
            continue;

        NetworkRenderDemandPointAttachment attachment;
        attachment.demand_point_render_id = demand_point_render_ids.value(demand_point.uuid, 0);
        attachment.demand_point_uuid = demand_point.uuid;
        attachment.demand_point_coordinate_wgs84 = demand_point.coordinate_wgs84;
        attachment.attachment_coordinate_wgs84 = attachment_coordinate;
        attachment.attachment_elevation_m = attachment_elevation_m;
        if (attachment.demand_point_render_id != 0)
            snapshot.demand_point_attachments.append(attachment);
    }

    return snapshot;
}
