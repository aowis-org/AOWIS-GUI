// Intermediate pipe vertices are WGS84 positions, not hydraulic elevations.
// On the globe they sit on the drawn terrain, just like demand points.
#include "map/render/map_globe_network_scene.h"
#include "map/render/map_globe_vertical_transform.h"
#include "geo/geo_wgs84_ellipsoid.h"

#include <QGuiApplication>
#include <QString>

#include <cmath>
#include <cstdio>
#include <limits>

namespace
{
constexpr double ToleranceM = 0.03;

bool require(bool condition, const char *message)
{
    if (!condition)
        std::fprintf(stderr, "map_globe_pipe_bend_drape: %s\n", message);
    return condition;
}

CoordinateWGS84 coordinate(double longitude, double latitude)
{
    CoordinateWGS84 result;
    result.longitude_deg = longitude;
    result.latitude_deg = latitude;
    return result;
}

NetworkRenderNode makeJunction(quint32 id, const CoordinateWGS84 &position, double height)
{
    NetworkRenderNode node;
    node.render_id = id;
    node.uuid = QUuid::createUuid();
    node.id = QStringLiteral("J%1").arg(id);
    node.entity_type = InfrastructureEntity::Junction;
    node.coordinate_wgs84 = position;
    node.elevation_m = height;
    return node;
}

bool decodeHeight(const MapGlobeNetworkScene &scene, float x, float y, float z,
                  double *longitude, double *height)
{
    const GeoWgs84Ellipsoid::EcefPositionD origin = scene.renderOriginEcef();
    GeoWgs84Ellipsoid::EcefPositionD absolute;
    absolute.x = origin.x + double(x);
    absolute.y = origin.y + double(y);
    absolute.z = origin.z + double(z);
    double latitude = 0.0;
    return GeoWgs84Ellipsoid::ecefToGeodetic(absolute, longitude, &latitude, height);
}

// Find the segment ending at the intermediate bend, not at a junction.
double bendHeight(const MapGlobeNetworkScene &scene, quint32 render_id, double bend_longitude)
{
    for (const MapNetworkLinkVertex &vertex : scene.linkVertices())
    {
        if (vertex.entity_type != InfrastructureEntity::Pipe || vertex.render_id != render_id)
            continue;
        double longitude = 0.0;
        double height = 0.0;
        if (decodeHeight(scene, vertex.end_x, vertex.end_y, vertex.end_z, &longitude, &height)
            && std::abs(longitude - bend_longitude) < 1e-7)
            return height;
    }
    return std::numeric_limits<double>::quiet_NaN();
}

double endpointHeight(const MapGlobeNetworkScene &scene, quint32 render_id,
                      double endpoint_longitude)
{
    for (const MapNetworkLinkVertex &vertex : scene.linkVertices())
    {
        if (vertex.entity_type != InfrastructureEntity::Pipe || vertex.render_id != render_id)
            continue;
        double longitude = 0.0;
        double height = 0.0;
        if (decodeHeight(scene, vertex.start_x, vertex.start_y, vertex.start_z, &longitude, &height)
            && std::abs(longitude - endpoint_longitude) < 1e-7)
            return height;
        if (decodeHeight(scene, vertex.end_x, vertex.end_y, vertex.end_z, &longitude, &height)
            && std::abs(longitude - endpoint_longitude) < 1e-7)
            return height;
    }
    return std::numeric_limits<double>::quiet_NaN();
}
}

int main(int argc, char **argv)
{
    QGuiApplication application(argc, argv);
    const CoordinateWGS84 start = coordinate(10.000, 50.000);
    const CoordinateWGS84 bend = coordinate(10.001, 50.000);
    const CoordinateWGS84 end = coordinate(10.002, 50.000);

    NetworkRenderSnapshot snapshot;
    snapshot.geometry_revision = 1;
    snapshot.nodes = {makeJunction(1, start, 100.0), makeJunction(2, end, 120.0)};
    NetworkRenderLink pipe;
    pipe.render_id = 7;
    pipe.uuid = QUuid::createUuid();
    pipe.entity_type = InfrastructureEntity::Pipe;
    pipe.start_node_render_id = 1;
    pipe.end_node_render_id = 2;
    pipe.vertices_wgs84 = {start, bend, end};
    // Deliberately unrelated to terrain: model elevations are not changed.
    pipe.elevations_m = {100.0, 210.0, 120.0};
    snapshot.links = {pipe};

    bool terrain_ready = false;
    double drawn_surface_height = 315.0;
    MapGlobeNetworkScene scene;
    scene.setSurfaceHeightResolver(
        [&terrain_ready, &drawn_surface_height](const CoordinateWGS84 &, double *height)
    {
        if (!terrain_ready)
            return false;
        *height = drawn_surface_height;
        return true;
    });
    scene.setNetworkSnapshot(snapshot);
    const double lift = MapGlobeVerticalTransform::MinimumNetworkAntiZFightingLiftM;
    if (!require(std::abs(bendHeight(scene, 7, bend.longitude_deg) - lift) < ToleranceM,
                 "unloaded terrain: bend not on ellipsoid")
        || !require(std::abs(endpointHeight(scene, 7, start.longitude_deg) - (100.0 + lift)) < ToleranceM,
                    "start elevation was replaced")
        || !require(std::abs(endpointHeight(scene, 7, end.longitude_deg) - (120.0 + lift)) < ToleranceM,
                    "end elevation was replaced")
        || !require(!scene.refreshDrapedPipeBendHeights(), "unchanged terrain triggered rebuild"))
        return 1;

    terrain_ready = true;
    if (!require(scene.refreshDrapedPipeBendHeights(), "new terrain did not redrape bend")
        || !require(std::abs(bendHeight(scene, 7, bend.longitude_deg) - (drawn_surface_height + lift))
                    < ToleranceM, "bend not on drawn relief")
        || !require(!scene.refreshDrapedPipeBendHeights(), "unchanged relief triggered rebuild"))
        return 1;

    drawn_surface_height += 16.0;
    if (!require(scene.refreshDrapedPipeBendHeights(), "changed LOD height did not redrape bend")
        || !require(std::abs(bendHeight(scene, 7, bend.longitude_deg) - (drawn_surface_height + lift))
                    < ToleranceM, "new LOD height not used"))
        return 1;

    scene.setGroundOffsetM(5.0);
    if (!require(std::abs(bendHeight(scene, 7, bend.longitude_deg) - (drawn_surface_height + 5.0))
                 < ToleranceM, "ground offset not used")
        || !require(std::abs(endpointHeight(scene, 7, start.longitude_deg) - 105.0) < ToleranceM,
                    "ground offset lost endpoint elevation")
        || !require(!scene.refreshDrapedPipeBendHeights(), "ground offset rebuild left stale bend cache")
        || !require(snapshot.links.first().elevations_m.at(1) == 210.0,
                    "visual terrain drape changed the hydraulic model"))
        return 1;

    // X-Ray classification uses the actual terrain LOD and must preserve
    // the normal retained link geometry while entering/leaving underground.
    double classification_terrain_m = 400.0;
    scene.setTerrainElevationResolver(
        [&classification_terrain_m](const CoordinateWGS84 &,
                                    double *terrain_m, double *cell_size_m) {
            *terrain_m = classification_terrain_m;
            *cell_size_m = 50.0;
            return true;
        });
    const qsizetype normal_link_count = scene.linkVertices().size();
    if (!require(scene.setUndergroundXRayEnabled(true), "could not enable X-Ray")
        || !require(!scene.undergroundLinkVertices().isEmpty(), "buried pipe has no X-Ray segments")
        || !require(scene.undergroundLinkVertices().size() % 6 == 0,
                    "X-Ray ribbons do not contain six vertices per segment")
        || !require(scene.linkVertices().size() == normal_link_count,
                    "X-Ray modified the normal network geometry"))
        return 1;

    classification_terrain_m = -1000.0;
    if (!require(scene.refreshUndergroundXRayGeometry(), "X-Ray did not refresh terrain")
        || !require(scene.undergroundLinkVertices().isEmpty(),
                    "visible pipe incorrectly classified as buried")
        || !require(scene.linkVertices().size() == normal_link_count,
                    "terrain-only X-Ray refresh rebuilt normal links"))
        return 1;

    classification_terrain_m = 400.0;
    if (!require(scene.refreshUndergroundXRayGeometry(), "X-Ray did not refresh buried pipe")
        || !require(!scene.undergroundLinkVertices().isEmpty(),
                    "buried pipe did not reappear in X-Ray")
        || !require(scene.setUndergroundXRayEnabled(false), "could not disable X-Ray")
        || !require(scene.undergroundLinkVertices().isEmpty(),
                    "disabled X-Ray retained underground geometry"))
        return 1;

    std::printf("globe pipe bend drape: ellipsoid, relief, LOD change, no redundant rebuild, offsets\n");
    return 0;
}
