#include "map/bgfx/map_bgfx_2d_snapshot_adapter.h"
#include "map/bgfx/map_bgfx_2d_scene.h"
#include "map/bgfx/map_bgfx_2d_scene_variants.h"
#include "map/bgfx/map_bgfx_2d_mesh.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace
{
CoordinateWGS84 coordinate(double longitude, double latitude)
{
    CoordinateWGS84 result;
    result.longitude_deg = longitude;
    result.latitude_deg = latitude;
    return result;
}

bool check(bool condition, const char *description)
{
    if (!condition)
        std::fprintf(stderr, "bgfx_2d_scene: %s\n", description);
    return condition;
}
}

int main()
{
    NetworkRenderSnapshot snapshot;
    snapshot.geometry_revision = 1;
    NetworkRenderNode junction;
    junction.render_id = 11;
    junction.coordinate_wgs84 = coordinate(18.1, 11.9);
    snapshot.nodes.append(junction);

    NetworkRenderLink pipe;
    pipe.render_id = 12;
    pipe.vertices_wgs84.append(coordinate(18.1, 11.9));
    pipe.vertices_wgs84.append(coordinate(18.2, 11.9));
    pipe.vertices_wgs84.append(coordinate(18.3, 12.0));
    snapshot.links.append(pipe);

    for (int index = 0; index < 10000; ++index)
    {
        NetworkRenderDemandPointAttachment attachment;
        attachment.demand_point_render_id = quint32(index + 100);
        attachment.demand_point_coordinate_wgs84 = coordinate(
            18.1 + double(index % 100) * 0.001,
            11.9 + double(index / 100) * 0.001);
        attachment.attachment_coordinate_wgs84 = coordinate(
            18.1 + double(index % 100) * 0.001, 11.9);
        snapshot.demand_point_attachments.append(attachment);
    }

    MapRetainedNetworkScene retained;
    MapBgfx2dScene prepared;
    if (!check(retained.setSnapshot(snapshot).geometry_changed, "initial snapshot")
        || !check(prepared.prepare(mapBgfx2dNetworkInput(retained)), "first geometry preparation")
        || !check(prepared.linkSegmentCount() == 2, "all pipe segments retained")
        || !check(prepared.linkPolylines().size() == 1, "pipe kept as one polyline")
        || !check(prepared.networkNodes().size() == 1, "junction retained")
        || !check(prepared.attachments().size() == 10000, "attachment lines retained"))
        return 1;

    // No icon atlas in this test: an empty lookup stages no icons.
    const MapBgfx2dMesh::IconLookup no_icons;
    MapBgfx2dMesh mesh;
    if (!check(mesh.prepare(prepared, no_icons), "first GPU mesh staging")
        || !check(mesh.linkVertexCount() == 8, "line quad vertex count (2 segments)")
        || !check(mesh.linkIndexCount() == 12, "line quad index count (2 segments)")
        || !check(mesh.attachmentVertexCount() == 40000, "attachment quad vertex count")
        || !check(mesh.nodeVertexCount() == 6, "node quad count")
        || !check(mesh.iconVertexCount() == 0, "no icons without an atlas lookup"))
        return 1;

    // Every chunk-local vertex reconstructs its world position exactly
    // (float error far below a pixel at zoom 19), and local values are small.
    {
        const MapBgfx2d::WorldPoint junction_world = prepared.networkNodes().first().position;
        bool found_junction = false;
        double maximum_local = 0.0;
        for (const MapBgfx2dMeshChunk &chunk : mesh.chunks())
        {
            for (const MapBgfx2dGpuVertex &vertex : chunk.node_vertices)
            {
                const double world_x = chunk.origin.x + double(vertex.x);
                const double world_y = chunk.origin.y + double(vertex.y);
                found_junction = found_junction
                    || (std::fabs(world_x - junction_world.x) < 1e-10
                        && std::fabs(world_y - junction_world.y) < 1e-10);
            }
            for (const MapBgfx2dLineVertex &vertex : chunk.attachment_vertices)
                maximum_local = std::max({maximum_local, std::fabs(double(vertex.x)),
                                          std::fabs(double(vertex.y))});
        }
        if (!check(found_junction, "chunk-local node does not reconstruct its world position")
            || !check(maximum_local < 2.0 * MapBgfx2dChunking::CellSize,
                      "chunk-local coordinates are not small"))
            return 1;
    }

    // Chunk transform: the junction under a centred camera maps to clip 0,0.
    {
        const MapBgfx2d::Camera centered = MapBgfx2d::cameraFor(18.1, 11.9, 15.2, 1200, 800);
        bool centred_ok = false;
        for (const MapBgfx2dMeshChunk &chunk : mesh.chunks())
        {
            if (chunk.node_vertices.isEmpty())
                continue;
            const MapBgfx2dClipCamera uniforms = MapBgfx2dChunking::cameraUniform(chunk.origin, centered);
            const MapBgfx2dGpuVertex vertex = chunk.node_vertices.first();
            const double clip_x = double(uniforms.origin_x) + double(vertex.x) * double(uniforms.scale_x);
            const double clip_y = double(uniforms.origin_y) + double(vertex.y) * double(uniforms.scale_y);
            centred_ok = std::fabs(clip_x) < 1e-4 && std::fabs(clip_y) < 1e-4;
        }
        if (!check(centred_ok, "centred junction is not at clip 0,0"))
            return 1;
    }

    for (int iteration = 0; iteration < 300; ++iteration)
    {
        const double zoom = 13.0 + double(iteration) * 0.03;
        const MapBgfx2d::Camera view = MapBgfx2d::cameraFor(
            18.19, 11.98, zoom, 1200, 800);
        const MapBgfx2d::ScreenPoint point = MapBgfx2d::screenFromWorld(
            view, prepared.attachments().first().start);
        if (!check(std::isfinite(point.x) && std::isfinite(point.y), "finite camera transform")
            || !check(!prepared.prepare(mapBgfx2dNetworkInput(retained)), "camera movement regenerated geometry")
            || !check(!mesh.prepare(prepared, no_icons), "camera movement regenerated GPU mesh"))
            return 1;
    }
    if (!check(prepared.preparationCount() == 1, "zoom triggered geometry preparation")
        || !check(mesh.preparationCount() == 1, "zoom triggered GPU mesh preparation"))
        return 1;

    snapshot.visual_revision = 1;
    if (!check(retained.setSnapshot(snapshot).visual_changed, "visual revision observed")
        || !check(!prepared.prepare(mapBgfx2dNetworkInput(retained)), "visual update regenerated geometry")
        || !check(!mesh.prepare(prepared, no_icons), "visual update regenerated GPU mesh"))
        return 1;

    snapshot.geometry_revision = 2;
    snapshot.nodes[0].coordinate_wgs84 = coordinate(18.4, 12.0);
    if (!check(retained.setSnapshot(snapshot).geometry_changed, "geometry revision observed")
        || !check(prepared.prepare(mapBgfx2dNetworkInput(retained)), "geometry revision did not prepare")
        || !check(prepared.preparationCount() == 2, "unexpected geometry preparation count")
        || !check(mesh.prepare(prepared, no_icons), "geometry revision did not rebuild GPU mesh")
        || !check(mesh.preparationCount() == 2, "GPU mesh revision count"))
        return 1;

    // Workspace monitor/editor modes use decluttered / exact node positions.
    // Both derived presentations must be prepared at most once per geometry
    // revision, and returning to a mode must preserve its preparation id.
    MapBgfx2dSceneVariants variants;
    NetworkRenderNode coincident;
    coincident.render_id = 13;
    coincident.coordinate_wgs84 = snapshot.nodes.first().coordinate_wgs84;
    snapshot.nodes.append(coincident);
    snapshot.geometry_revision = 3;
    if (!check(retained.setSnapshot(snapshot).geometry_changed, "new variant geometry revision")
        || !check(variants.prepare(mapBgfx2dNetworkInput(retained)), "monitor variant was not prepared")
        || !check(variants.active().declutteringEnabled(), "monitor decluttering disabled"))
        return 1;
    const quint64 monitor_preparation_id = variants.active().preparationId();
    if (!check(variants.active().networkNodes().size() == 2, "monitor node count")
        || !check(variants.active().networkNodes()[0].position.x
                      != variants.active().networkNodes()[1].position.x
                  || variants.active().networkNodes()[0].position.y
                      != variants.active().networkNodes()[1].position.y,
                  "monitor failed to separate coincident nodes")
        || !check(variants.setDeclutteringEnabled(false, mapBgfx2dNetworkInput(retained)), "editor variant not prepared")
        || !check(!variants.active().declutteringEnabled(), "editor decluttering still enabled"))
        return 1;
    const quint64 editor_preparation_id = variants.active().preparationId();
    if (!check(variants.active().networkNodes()[0].position.x
                   == variants.active().networkNodes()[1].position.x
               && variants.active().networkNodes()[0].position.y
                   == variants.active().networkNodes()[1].position.y,
               "editor changed exact coincident coordinates"))
        return 1;
    for (int iteration = 0; iteration < 100; ++iteration)
    {
        const bool monitor_mode = iteration % 2 == 0;
        if (!check(!variants.setDeclutteringEnabled(monitor_mode, mapBgfx2dNetworkInput(retained)),
                   "mode switch recalculated cached geometry")
            || !check(variants.active().preparationId()
                          == (monitor_mode ? monitor_preparation_id : editor_preparation_id),
                       "mode switch lost cached variant identity")
            || !check(!variants.prepare(mapBgfx2dNetworkInput(retained)), "same revision regenerated geometry"))
            return 1;
    }
    // New geometry discards both old presentations. Only the active one is
    // prepared eagerly; the other follows lazily on its next activation.
    snapshot.geometry_revision = 4;
    snapshot.nodes[0].coordinate_wgs84 = coordinate(19.0, 13.0);
    if (!check(retained.setSnapshot(snapshot).geometry_changed, "variant geometry update")
        || !check(variants.prepare(mapBgfx2dNetworkInput(retained)), "active variant did not refresh")
        || !check(variants.active().preparationId() != editor_preparation_id,
                  "active variant retained obsolete geometry")
        || !check(variants.setDeclutteringEnabled(true, mapBgfx2dNetworkInput(retained)),
                  "stale monitor variant was not refreshed")
        || !check(variants.active().preparationId() != monitor_preparation_id,
                  "stale monitor variant identity was reused"))
        return 1;

    std::puts("Retained bgfx 2D geometry staging and zoom invariants passed");
    return 0;
}
