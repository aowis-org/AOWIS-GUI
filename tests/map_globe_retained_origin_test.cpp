#include "map/render/map_globe_network_scene.h"
#include "map/render/map_render_diagnostics.h"
#include "geo/geo_wgs84_ellipsoid.h"

#include <QGuiApplication>
#include <QMatrix4x4>
#include <QVector4D>
#include <QVector3D>
#include <QString>

#include <cmath>
#include <cstdio>

namespace
{
constexpr int DemandCount = 10000;

bool require(bool condition, const char *message)
{
    if (!condition)
        std::fprintf(stderr, "map_globe_retained_origin: %s\n", message);
    return condition;
}

CoordinateWGS84 coordinate(double longitude, double latitude)
{
    CoordinateWGS84 result;
    result.longitude_deg = longitude;
    result.latitude_deg = latitude;
    return result;
}

QUuid stableUuid(int value)
{
    return QUuid(QStringLiteral("{00000000-0000-0000-0000-%1}")
        .arg(value, 12, 16, QLatin1Char('0')));
}

bool sameOrigin(const GeoWgs84Ellipsoid::EcefPositionD &a,
                const GeoWgs84Ellipsoid::EcefPositionD &b)
{
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

// Camera/terrain and network may use different float32 coordinate frames.
// The same physical point must nevertheless project to the same pixel.
bool testIndependentProjectionFrames()
{
    const GeoWgs84Ellipsoid::EcefPositionD camera_origin =
        GeoWgs84Ellipsoid::geodeticToEcefD(18.187, 11.944, 0.0);
    const GeoWgs84Ellipsoid::EcefPositionD network_origin =
        GeoWgs84Ellipsoid::geodeticToEcefD(18.18, 11.94, 0.0);
    const GeoWgs84Ellipsoid::EcefPositionD world_point =
        GeoWgs84Ellipsoid::geodeticToEcefD(18.1872, 11.9443, 440.0);
    const GeoWgs84Ellipsoid::OrbitCameraBasisRelative camera_basis =
        GeoWgs84Ellipsoid::orbitCameraBasisRelativeToOrigin(
            18.187, 11.944, 17.0, 55.0, 3500.0, 440.0,
            camera_origin, 0.0);
    const GeoWgs84Ellipsoid::OrbitCameraBasisRelative network_basis =
        GeoWgs84Ellipsoid::orbitCameraBasisRelativeToOrigin(
            18.187, 11.944, 17.0, 55.0, 3500.0, 440.0,
            network_origin, 0.0);

    QMatrix4x4 projection;
    projection.perspective(45.0f, 1.5f, 3.5f, 100000.0f);
    QMatrix4x4 camera_view;
    camera_view.lookAt(camera_basis.eye, camera_basis.target, camera_basis.up);
    QMatrix4x4 network_view;
    network_view.lookAt(network_basis.eye, network_basis.target, network_basis.up);
    const QVector4D camera_local(
        float(world_point.x - camera_origin.x),
        float(world_point.y - camera_origin.y),
        float(world_point.z - camera_origin.z), 1.0f);
    const QVector4D network_local(
        float(world_point.x - network_origin.x),
        float(world_point.y - network_origin.y),
        float(world_point.z - network_origin.z), 1.0f);
    const QVector4D clip_camera = projection * camera_view * camera_local;
    const QVector4D clip_network = projection * network_view * network_local;
    if (clip_camera.w() <= 0.0f || clip_network.w() <= 0.0f)
        return false;
    const double delta_x_pixels = std::abs(
        double(clip_camera.x() / clip_camera.w()
            - clip_network.x() / clip_network.w())) * 675.0;
    const double delta_y_pixels = std::abs(
        double(clip_camera.y() / clip_camera.w()
            - clip_network.y() / clip_network.w())) * 450.0;
    return delta_x_pixels < 0.1 && delta_y_pixels < 0.1;
}

bool testLocalPrecision(double longitude, double latitude)
{
    const GeoWgs84Ellipsoid::EcefPositionD anchor =
        GeoWgs84Ellipsoid::geodeticToEcefD(longitude, latitude, 0.0);
    // A displacement of a few km is characteristic of one network chunk.
    // Float narrowing happens only AFTER double ECEF subtraction.
    double moved_lon = longitude;
    double moved_lat = latitude;
    if (!GeoWgs84Ellipsoid::offsetGeodetic(
            longitude, latitude, 5000.0, 3000.0, &moved_lon, &moved_lat))
    {
        return false;
    }
    const GeoWgs84Ellipsoid::EcefPositionD absolute =
        GeoWgs84Ellipsoid::geodeticToEcefD(moved_lon, moved_lat, 43.7);
    const QVector3D local(
        float(absolute.x - anchor.x), float(absolute.y - anchor.y),
        float(absolute.z - anchor.z));
    const double error_m = std::hypot(
        std::hypot(double(local.x()) - (absolute.x - anchor.x),
                   double(local.y()) - (absolute.y - anchor.y)),
        double(local.z()) - (absolute.z - anchor.z));
    return error_m < 0.002;
}
}

int main(int argc, char **argv)
{
    qputenv("AOWIS_MAP_RENDER_DIAGNOSTICS", "1");
    QGuiApplication application(argc, argv);

    if (!require(testIndependentProjectionFrames(),
                 "camera and network ECEF frames disagree on screen projection")
        || !require(testLocalPrecision(18.18, 11.94), "Bitkine-local ECEF precision")
        || !require(testLocalPrecision(179.99, -12.0), "antimeridian ECEF precision")
        || !require(testLocalPrecision(-41.0, 89.8), "polar ECEF precision"))
    {
        return 1;
    }

    NetworkRenderSnapshot original;
    original.geometry_revision = 1;
    original.visual_revision = 1;

    NetworkRenderNode junction;
    junction.uuid = stableUuid(20001);
    junction.entity_type = InfrastructureEntity::Junction;
    junction.render_id = 20001;
    junction.coordinate_wgs84 = coordinate(18.18, 11.94);
    junction.elevation_m = 460.0;
    original.nodes.append(junction);

    original.nodes.reserve(DemandCount + 1);
    original.demand_point_attachments.reserve(DemandCount);
    for (int index = 0; index < DemandCount; ++index)
    {
        const double longitude = 18.1801 + double(index % 100) * 0.00019;
        const double latitude = 11.9401 + double(index / 100) * 0.00010;
        NetworkRenderNode demand;
        demand.uuid = stableUuid(index + 1);
        demand.render_id = quint32(index + 1);
        demand.entity_type = InfrastructureEntity::DemandPoint;
        demand.coordinate_wgs84 = coordinate(longitude, latitude);
        demand.elevation_m = 460.0;
        original.nodes.append(demand);

        NetworkRenderDemandPointAttachment attachment;
        attachment.demand_point_uuid = demand.uuid;
        attachment.demand_point_render_id = demand.render_id;
        attachment.demand_point_coordinate_wgs84 = demand.coordinate_wgs84;
        attachment.attachment_coordinate_wgs84 = coordinate(longitude, latitude - 0.00005);
        original.demand_point_attachments.append(attachment);
    }

    MapGlobeNetworkScene scene;
    scene.setNetworkSnapshot(original);
    const GeoWgs84Ellipsoid::EcefPositionD anchor = scene.renderOriginEcef();
    const MapNetworkLinkVertex *const original_links = scene.linkVertices().constData();
    const MapNetworkNodeVertex *const original_nodes = scene.nodeVertices().constData();
    const qsizetype link_count = scene.linkVertices().size();
    const qsizetype node_count = scene.nodeVertices().size();

    if (!require(link_count >= 6 * DemandCount, "attachment vertices missing")
        || !require(node_count >= 6 * DemandCount, "demand vertices missing")
        || !require(MapRenderDiagnostics::value(
                        MapRenderDiagnostics::Counter::GlobeGeometryRebuilt) == 1,
                    "unexpected initial geometry rebuild count")
        || !require(MapRenderDiagnostics::value(
                        MapRenderDiagnostics::Counter::DemandAttachmentsPrepared)
                        == DemandCount,
                    "initial demand attachment count incorrect"))
    {
        return 1;
    }

    // Exercises both nearby and planetary camera-origin changes without any
    // geometry copy, change to vertex storage, or demand-point preparation.
    for (int index = 0; index < 80; ++index)
    {
        const GeoWgs84Ellipsoid::EcefPositionD camera_origin =
            GeoWgs84Ellipsoid::geodeticToEcefD(
                -175.0 + double(index) * 4.0,
                -75.0 + double(index % 50) * 3.0, 0.0);
        if (!require(!scene.setRenderOriginEcef(camera_origin),
                     "camera rebase changed retained geometry origin")
            || !require(sameOrigin(scene.renderOriginEcef(), anchor),
                        "camera rebase replaced network-local anchor")
            || !require(scene.linkVertices().constData() == original_links,
                        "camera rebase reallocated attachment vertices")
            || !require(scene.nodeVertices().constData() == original_nodes,
                        "camera rebase reallocated demand vertices")
            || !require(scene.linkVertices().size() == link_count
                        && scene.nodeVertices().size() == node_count,
                        "camera rebase changed retained vertex counts"))
        {
            return 1;
        }
    }

    original.visual_revision = 2;
    scene.setNetworkSnapshot(original);
    if (!require(MapRenderDiagnostics::value(
                    MapRenderDiagnostics::Counter::GlobeGeometryRebuilt) == 1,
                 "visual-only revision rebuilt ECEF geometry")
        || !require(MapRenderDiagnostics::value(
                        MapRenderDiagnostics::Counter::DemandAttachmentsPrepared)
                        == DemandCount,
                    "camera or visual update reprocessed demand attachments"))
    {
        return 1;
    }

    // Render's retained-scene invalidation plan must refresh only the
    // affected CPU streams. None of these visual/UI changes may retessellate
    // thousands of demand attachments or re-anchor the ECEF coordinate frame.
    MapNetworkRenderSymbology display;
    display.icon_size_px += 2;
    display.flow_direction_size_px += 2;
    scene.setSymbology(display);
    scene.setSelectedEntity(InfrastructureEntity::DemandPoint, original.nodes[0].uuid);
    scene.setSimulationErrorEntities({}, {});
    scene.setUse3dIconModels(true);
    scene.setFlowDirectionPixelsPerMeter(2.0);
    if (!require(MapRenderDiagnostics::value(
                    MapRenderDiagnostics::Counter::GlobeGeometryRebuilt) == 1,
                 "visual invalidations rebuilt network geometry")
        || !require(sameOrigin(scene.renderOriginEcef(), anchor),
                    "visual invalidations replaced retained ECEF anchor")
        || !require(scene.linkVertices().constData() == original_links,
                    "visual invalidations replaced retained link storage")
        || !require(scene.nodeVertices().constData() == original_nodes,
                    "visual invalidations replaced retained node storage"))
    {
        return 1;
    }

    original.geometry_revision = 2;
    original.nodes[0].coordinate_wgs84 = coordinate(18.21, 11.95);
    scene.setNetworkSnapshot(original);
    if (!require(MapRenderDiagnostics::value(
                    MapRenderDiagnostics::Counter::GlobeGeometryRebuilt) == 2,
                 "actual geometry change failed to rebuild network")
        || !require(!sameOrigin(scene.renderOriginEcef(), anchor),
                    "geometry revision failed to update network-local frame"))
    {
        return 1;
    }

    std::printf("globe retained origin: 10000 attachments, 80 camera-origin changes, "
                "zero camera-driven geometry rebuilds; precision tests passed\n");
    return 0;
}
