#include "map/bgfx/map_bgfx_editor_overlay.h"
#include "map/editor/map_canvas_projection.h"
#include "map/bgfx/map_bgfx_hud_canvas.h"
#include "map/bgfx/map_bgfx_hud_text_cache.h"
#include "map/bgfx/map_bgfx_pan_reuse.h"
#include "geo/geo_wgs84_ellipsoid.h"

#include <QGuiApplication>
#include <QImage>
#include <QVector>

#include <cmath>
#include <cstdio>
#include <functional>

// Map editor on bgfx (roadmap Phase 2) without a GPU: what the overlay
// builder draws for each editor state, placed with the continuous camera
// exactly where the GPU network is (icons, pipe vertices, selection,
// placement, move preview, full-network move, diagnostics, selection frames,
// culling, antimeridian).
namespace
{
int fails = 0;

void req(bool ok, const char *what)
{
    if (!ok)
    {
        std::printf("FAIL: %s\n", what);
        ++fails;
    }
}

constexpr double ViewportWidth = 800.0;
constexpr double ViewportHeight = 600.0;
constexpr int EntityWidth = 20;
constexpr double IconHeight = 24.0;

using Highlight = MapBgfxEditorOverlay::Highlight;

struct IconCall
{
    InfrastructureEntity entity = InfrastructureEntity::Unknown;
    int width = 0;
    Highlight highlight = Highlight::None;
};

// A provider that hands out one solid sprite per (entity, highlight) and
// records every request.
struct IconProbe
{
    QVector<IconCall> calls;

    MapBgfxEditorOverlay::Icon icon(MapBgfxHudTextCache *cache, InfrastructureEntity entity,
                                    int width, Highlight highlight)
    {
        calls.append(IconCall{entity, width, highlight});
        MapBgfxEditorOverlay::Icon result;
        result.width = double(width);
        result.height = IconHeight;
        result.padding = highlight == Highlight::None ? 0.0 : 9.0;
        QImage image(int(result.width + 2.0 * result.padding),
                     int(result.height + 2.0 * result.padding), QImage::Format_ARGB32);
        image.fill(qRgba(10 * int(entity), 20, 30 * int(highlight) % 256, 255));
        result.sprite = cache->image(QStringLiteral("probe-%1-%2").arg(int(entity)).arg(int(highlight)),
                                     image, QSizeF(image.width(), image.height()));
        return result;
    }
    int count(Highlight highlight) const
    {
        int result = 0;
        for (const IconCall &call : calls)
            result += call.highlight == highlight ? 1 : 0;
        return result;
    }
};

CoordinateWGS84 coordinate(double longitude, double latitude)
{
    CoordinateWGS84 result;
    result.longitude_deg = longitude;
    result.latitude_deg = latitude;
    return result;
}

struct Scene
{
    NetworkRenderSnapshot snapshot;
    MapEditorVisualState state;
    MapEditorViewportRenderState viewport;
    QUuid junction_a;
    QUuid junction_b;
    QUuid pipe;
    QUuid pump;
};

// Two junctions, a pipe between them with one intermediate vertex, a pump
// from B to a third junction, around (9.0, 47.0).
Scene scene()
{
    Scene result;
    result.snapshot.geometry_revision = 7;
    const double lons[3] = {8.9995, 9.0005, 9.0010};
    for (int index = 0; index < 3; ++index)
    {
        NetworkRenderNode node;
        node.render_id = quint32(index + 1);
        node.uuid = QUuid::createUuid();
        node.entity_type = InfrastructureEntity::Junction;
        node.coordinate_wgs84 = coordinate(lons[index], 47.0);
        result.snapshot.nodes.append(node);
    }
    result.junction_a = result.snapshot.nodes.at(0).uuid;
    result.junction_b = result.snapshot.nodes.at(1).uuid;
    NetworkRenderLink pipe;
    pipe.render_id = 1;
    pipe.uuid = QUuid::createUuid();
    pipe.entity_type = InfrastructureEntity::Pipe;
    pipe.vertices_wgs84.append(coordinate(8.9995, 47.0));
    pipe.vertices_wgs84.append(coordinate(9.0, 47.0002));
    pipe.vertices_wgs84.append(coordinate(9.0005, 47.0));
    result.snapshot.links.append(pipe);
    result.pipe = pipe.uuid;
    NetworkRenderLink pump;
    pump.render_id = 2;
    pump.uuid = QUuid::createUuid();
    pump.entity_type = InfrastructureEntity::Pump;
    pump.vertices_wgs84.append(coordinate(9.0005, 47.0));
    pump.vertices_wgs84.append(coordinate(9.0010, 47.0));
    result.snapshot.links.append(pump);
    result.pump = pump.uuid;
    result.state.entity_width = EntityWidth;
    return result;
}

MapBgfx2d::Camera camera(double longitude = 9.0, double latitude = 47.0)
{
    return MapBgfx2d::cameraFor(longitude, latitude, 18.0, ViewportWidth, ViewportHeight);
}

MapBgfxHudPoint project(const MapBgfx2d::Camera &view, double longitude, double latitude)
{
    const MapBgfx2d::ScreenPoint point =
        MapBgfx2d::screenFromWorld(view, MapBgfx2d::worldFromWgs84(longitude, latitude));
    return MapBgfxHudPoint{point.x, point.y};
}

bool hasColorNear(const MapBgfxHudCanvas &canvas, quint32 argb, double x, double y, double radius)
{
    const quint32 abgr = MapBgfxHudCanvas::premultipliedAbgr(argb);
    for (const MapBgfxHudVertex &vertex : canvas.vertices())
    {
        if (vertex.abgr == abgr && std::fabs(vertex.x - x) <= radius
            && std::fabs(vertex.y - y) <= radius)
            return true;
    }
    return false;
}

// Check one frame-specific colour without assuming the whole editor overlay
// is empty: network icons and pipe vertices must remain after selection ends.
bool hasColor(const MapBgfxHudCanvas &canvas, quint32 argb)
{
    const quint32 abgr = MapBgfxHudCanvas::premultipliedAbgr(argb);
    for (const MapBgfxHudVertex &vertex : canvas.vertices())
    {
        if (vertex.abgr == abgr)
            return true;
    }
    return false;
}

// A sprite vertex (white tint, not the white texel) at (x, y).
bool hasSpriteCorner(const MapBgfxHudCanvas &canvas, const MapBgfxHudTextCache &cache,
                     double x, double y)
{
    for (const MapBgfxHudVertex &vertex : canvas.vertices())
    {
        const bool white_texel = vertex.u == cache.whiteU() && vertex.v == cache.whiteV();
        if (!white_texel && vertex.abgr == 0xffffffffu && std::fabs(vertex.x - x) < 0.01
            && std::fabs(vertex.y - y) < 0.01)
            return true;
    }
    return false;
}

// Textured, white-tinted vertices (sprites, not solid shapes) near (x, y).
int spriteVerticesNear(const MapBgfxHudCanvas &canvas, const MapBgfxHudTextCache &cache,
                       double x, double y, double radius)
{
    int result = 0;
    for (const MapBgfxHudVertex &vertex : canvas.vertices())
    {
        const bool white_texel = vertex.u == cache.whiteU() && vertex.v == cache.whiteV();
        if (!white_texel && vertex.abgr == 0xffffffffu && std::fabs(vertex.x - x) <= radius
            && std::fabs(vertex.y - y) <= radius)
            ++result;
    }
    return result;
}

// Dashes drawn with colour abgr: the textured quads' pattern, read back from
// the atlas along each quad's u range (on -> off transitions = dashes).
int texturedDashes(const MapBgfxHudCanvas &canvas, const MapBgfxHudTextCache &cache,
                   quint32 abgr)
{
    const QVector<MapBgfxHudVertex> &vertices = canvas.vertices();
    const QImage &atlas = cache.atlas();
    int dashes = 0;
    for (qsizetype index = 0; index + 3 < vertices.size(); )
    {
        const MapBgfxHudVertex &first = vertices.at(index);
        const bool textured = !(first.u == cache.whiteU() && first.v == cache.whiteV());
        if (first.abgr != abgr || !textured)
        {
            ++index;
            continue;
        }
        float u_min = first.u;
        float u_max = first.u;
        float v_sum = 0.0f;
        for (qsizetype corner = 0; corner < 4; ++corner)
        {
            u_min = std::min(u_min, vertices.at(index + corner).u);
            u_max = std::max(u_max, vertices.at(index + corner).u);
            v_sum += vertices.at(index + corner).v;
        }
        const int row = int(v_sum / 4.0f * float(atlas.height()));
        bool previous_on = false;
        for (int column = int(u_min * float(atlas.width())); column < int(u_max * float(atlas.width())); ++column)
        {
            const bool on = qAlpha(atlas.pixel(column, row)) > 0;
            if (on && !previous_on)
                ++dashes;
            previous_on = on;
        }
        index += 4;
    }
    return dashes;
}

struct Harness
{
    MapBgfxEditorOverlay overlay;
    IconProbe probe;
    MapBgfxHudTextCache cache{1024};
    MapBgfxHudCanvas canvas;

    Harness()
    {
        overlay.setIconProvider([this](MapBgfxHudTextCache *atlas, InfrastructureEntity entity,
                                       const QString &, int width, Highlight highlight)
        {
            return probe.icon(atlas, entity, width, highlight);
        });
    }
    void build(const Scene &scene, const MapBgfx2d::Camera &view, bool full_move = false,
               QPointF translation = QPointF(),
               const QSizeF &viewport = QSizeF(ViewportWidth, ViewportHeight),
               bool tile_only = false)
    {
        probe.calls.clear();
        MapBgfxEditorOverlay::Inputs inputs;
        inputs.viewport = viewport;
        inputs.camera = view;
        inputs.snapshot = &scene.snapshot;
        inputs.visual_state = &scene.state;
        inputs.viewport_state = &scene.viewport;
        inputs.tile_only = tile_only;
        inputs.full_network_move = full_move;
        inputs.full_network_move_translation = translation;
        overlay.build(&canvas, &cache, inputs);
    }
};

void testMapManagementOverlay()
{
    Harness harness;
    Scene current = scene();
    const MapBgfx2d::Camera view = camera();
    // The same nonempty distribution snapshot is retained in memory; the
    // Management overlay should draw no distribution entity icons.
    harness.build(current, view, false, QPointF(),
                  QSizeF(ViewportWidth, ViewportHeight), true);
    req(harness.probe.calls.isEmpty() && harness.canvas.vertices().isEmpty(),
        "management hides network icons without changing the retained snapshot");

    current.viewport.rectangle_selection_visible = true;
    current.viewport.rectangle_selection = QRect(100, 100, 200, 120);
    harness.build(current, view, false, QPointF(),
                  QSizeF(ViewportWidth, ViewportHeight), true);
    req(hasColor(harness.canvas, 0xb456d7ffu),
        "management keeps its 2D cache-selection rectangle");
    req(harness.probe.calls.isEmpty(),
        "management selection does not re-enable distribution icons");

    current.viewport.rectangle_selection_visible = false;
    harness.build(current, view, false, QPointF(),
                  QSizeF(ViewportWidth, ViewportHeight), true);
    req(harness.canvas.vertices().isEmpty(),
        "management rectangle vanishes on release");

    // The temporary blue drag frame disappears, but the selected tiles
    // remain highlighted until deletion, cancellation, or mode change.
    const MapBgfx2d::WorldPoint center = MapBgfx2d::worldFromWgs84(9.0, 47.0);
    const double tiles_per_axis = std::exp2(18.0);
    current.viewport.tile_selection_visible = true;
    current.viewport.tile_selection_zoom = 18;
    current.viewport.tile_x_min = int(std::floor(center.x * tiles_per_axis));
    current.viewport.tile_x_max = current.viewport.tile_x_min + 1;
    current.viewport.tile_y_min = int(std::floor(center.y * tiles_per_axis));
    current.viewport.tile_y_max = current.viewport.tile_y_min;
    current.viewport.tile_selection_kind = MapEditorTileSelectionKind::MapTiles;
    harness.build(current, view, false, QPointF(), QSizeF(ViewportWidth, ViewportHeight), true);
    req(hasColor(harness.canvas, 0xe69bff91u) && harness.probe.calls.isEmpty(),
        "management keeps the green map-tile selection without network icons in 2D");

    current.viewport.tile_selection_kind = MapEditorTileSelectionKind::Terrain;
    harness.build(current, view, false, QPointF(), QSizeF(ViewportWidth, ViewportHeight), true);
    req(hasColor(harness.canvas, 0xebf0ca96u) && harness.probe.calls.isEmpty(),
        "management keeps the brown terrain selection without network icons in 2D");

    const MapBgfxEditorOverlay::GlobeProjector projector = [](
        double longitude, double latitude, QPointF *screen, double *distance_m)
    {
        *screen = QPointF(400.0 + (longitude - 9.0) * 100000.0,
                          300.0 - (latitude - 47.0) * 100000.0);
        if (distance_m != nullptr)
            *distance_m = 1000.0;
        return true;
    };
    harness.overlay.buildGlobe(&harness.canvas, &harness.cache, current.viewport, projector);
    req(hasColor(harness.canvas, 0xebf0ca96u) && harness.probe.calls.isEmpty(),
        "management keeps the brown terrain selection without network icons in 3D");

    current.viewport.tile_selection_kind = MapEditorTileSelectionKind::MapTiles;
    harness.overlay.buildGlobe(&harness.canvas, &harness.cache, current.viewport, projector);
    req(hasColor(harness.canvas, 0xe69bff91u) && harness.probe.calls.isEmpty(),
        "management keeps the green map-tile selection without network icons in 3D");

    current.viewport.tile_selection_visible = false;
    // Switching back to Editor must reuse the same network snapshot and
    // restore its icons without any geometry change.
    harness.build(current, view);
    req(harness.probe.calls.size() == 4,
        "distribution editor restores retained entity icons after management");
}

void testStaticDetails()
{
    Harness harness;
    Scene s = scene();
    const MapBgfx2d::Camera view = camera();
    harness.build(s, view);
    req(harness.probe.calls.size() == 4, "one icon per node and one for the pump");
    req(harness.probe.count(Highlight::None) == 4, "unselected icons have no glow");
    for (const IconCall &call : harness.probe.calls)
        req(call.width == EntityWidth, "icons use the marker width");

    // Bottom-anchored at the rounded node position (no padding without glow).
    const MapBgfxHudPoint a = project(view, 8.9995, 47.0);
    req(hasSpriteCorner(harness.canvas, harness.cache, double(qRound(a.x)), double(qRound(a.y))),
        "node icon's bottom-left corner sits on the rounded node position");
    req(hasSpriteCorner(harness.canvas, harness.cache, double(qRound(a.x)),
                        double(qRound(a.y)) - IconHeight),
        "node icon extends upwards by its height");

    // The pipe's intermediate vertex gets a black dot; the end nodes do not.
    const MapBgfxHudPoint vertex = project(view, 9.0, 47.0002);
    req(hasColorNear(harness.canvas, MapBgfxEditorOverlay::BlackColor, vertex.x, vertex.y,
                     MapBgfxEditorOverlay::PipeVertexRadiusPx + 0.01),
        "pipe vertex dot at the intermediate vertex");
    // The pump icon is centred at the link middle.
    const MapBgfxHudPoint pump_center = project(view, 0.5 * (9.0005 + 9.0010), 47.0);
    req(hasSpriteCorner(harness.canvas, harness.cache, pump_center.x - 0.5 * EntityWidth,
                        pump_center.y - 0.5 * IconHeight),
        "device link icon is centred on the link");

    // Culling: far away from the camera nothing is requested.
    harness.build(s, camera(20.0, 47.0));
    req(harness.probe.calls.isEmpty(), "off-screen icons are culled before the provider");
    req(!harness.canvas.overflowed(), "overlay draw list stays within budget");
}

void testSelection()
{
    Harness harness;
    Scene s = scene();
    const MapBgfx2d::Camera view = camera();
    s.state.selected_pipe_uuids.append(s.pipe);
    s.state.selected_marker_uuids.append(s.junction_b);
    s.state.selected_marker_uuids.append(s.pump);
    harness.build(s, view);
    const MapBgfxHudPoint vertex = project(view, 9.0, 47.0002);
    req(hasColorNear(harness.canvas, MapBgfxEditorOverlay::SelectedColor, vertex.x, vertex.y, 4.1),
        "selected pipe is drawn blue with its vertices");
    req(harness.probe.count(Highlight::Selected) == 2, "selected node and pump glow");
    const MapBgfxHudPoint b = project(view, 9.0005, 47.0);
    // Glowing icon: padded by 9 around the bottom-anchored rect.
    req(hasSpriteCorner(harness.canvas, harness.cache, double(qRound(b.x)) - 9.0,
                        double(qRound(b.y)) + 9.0),
        "selected icon carries its glow padding");
}

void testPlacement()
{
    Harness harness;
    Scene s = scene();
    const MapBgfx2d::Camera view = camera();
    s.state.placement.creating = true;
    s.state.placement.entity = InfrastructureEntity::Pipe;
    s.state.placement.pipe_start_node_uuid = s.junction_a;
    s.state.placement.mouse_position = QPointF(700.0, 500.0);
    s.state.placement.floating_marker_visible = false;
    harness.build(s, view);
    req(hasColorNear(harness.canvas, MapBgfxEditorOverlay::PreviewColor, 700.0, 500.0, 2.0),
        "pipe rubber band ends at the pointer");
    const MapBgfxHudPoint a = project(view, 8.9995, 47.0);
    req(hasColorNear(harness.canvas, MapBgfxEditorOverlay::PreviewColor, a.x, a.y, 2.0),
        "pipe rubber band starts at the start node");

    // Snapped to a target node: ends there, and the target is ringed.
    s.state.placement.connection_target_uuid = s.junction_b;
    harness.build(s, view);
    const MapBgfxHudPoint b = project(view, 9.0005, 47.0);
    req(hasColorNear(harness.canvas, MapBgfxEditorOverlay::PreviewColor, b.x + 9.0, b.y, 0.6),
        "connection target gets the 9 px circle");
    req(!hasColorNear(harness.canvas, MapBgfxEditorOverlay::PreviewColor, 700.0, 500.0, 2.0),
        "a snapped rubber band no longer follows the pointer");

    // Floating marker of a new junction under the pointer.
    Scene floating = scene();
    floating.state.placement.creating = true;
    floating.state.placement.entity = InfrastructureEntity::Tank;
    floating.state.placement.floating_marker_visible = true;
    floating.state.placement.floating_width = 30;
    floating.state.placement.mouse_position = QPointF(400.5, 300.5);
    harness.build(floating, view);
    bool tank = false;
    for (const IconCall &call : harness.probe.calls)
        tank = tank || (call.entity == InfrastructureEntity::Tank && call.width == 30);
    req(tank, "floating marker uses the floating width");
    req(hasSpriteCorner(harness.canvas, harness.cache, 400.5, 300.5),
        "floating marker is bottom-anchored at the unrounded pointer");
}

void testMove()
{
    Harness harness;
    Scene s = scene();
    const MapBgfx2d::Camera view = camera();
    s.state.move.active = true;
    s.state.move.session_id = 3;
    MapEditorDynamicMarkerVisualState moved;
    moved.entity = InfrastructureEntity::Junction;
    moved.uuid = s.junction_a;
    moved.coordinate_wgs84 = coordinate(8.9992, 47.0003);
    s.state.move.markers.append(moved);
    harness.build(s, view);
    const MapBgfxHudPoint old_position = project(view, 8.9995, 47.0);
    const MapBgfxHudPoint new_position = project(view, 8.9992, 47.0003);
    req(!hasSpriteCorner(harness.canvas, harness.cache, double(qRound(old_position.x)),
                         double(qRound(old_position.y))),
        "the moved node is not drawn at its old place");
    req(hasSpriteCorner(harness.canvas, harness.cache, double(qRound(new_position.x)),
                        double(qRound(new_position.y))),
        "the moved node's icon follows the drag");
    req(hasColorNear(harness.canvas, MapBgfxEditorOverlay::BlackColor, new_position.x,
                     new_position.y, MapBgfxEditorOverlay::MarkerDotRadiusPx + 0.01),
        "the moved node gets its dot (hidden in the GPU network)");

    // Full-network move: everything shifted by the translation.
    Scene all = scene();
    all.state.move.active = true;
    harness.build(all, view, true, QPointF(30.0, -10.0));
    const MapBgfxHudPoint a = project(view, 8.9995, 47.0);
    req(hasSpriteCorner(harness.canvas, harness.cache, double(qRound(a.x + 30.0)),
                        double(qRound(a.y - 10.0))),
        "full-network move shifts the icons by the network translation");
    req(!hasSpriteCorner(harness.canvas, harness.cache, double(qRound(a.x)), double(qRound(a.y))),
        "nothing remains at the unshifted position");
}

void testDiagnostics()
{
    Harness harness;
    Scene s = scene();
    const MapBgfx2d::Camera view = camera();
    s.state.simulation_error_entities.insert(s.pipe, InfrastructureEntity::Pipe);
    s.state.simulation_error_entities.insert(s.junction_b, InfrastructureEntity::Junction);
    s.state.simulation_stale_diagnostic_entity_uuids.insert(s.junction_b);
    harness.build(s, view);
    const MapBgfxHudPoint vertex = project(view, 9.0, 47.0002);
    req(hasColorNear(harness.canvas, MapBgfxEditorOverlay::ErrorColor, vertex.x, vertex.y, 2.0),
        "pipe with a simulation error is drawn red");
    req(harness.probe.count(Highlight::Stale) == 1, "stale node error glows grey");

    s.state.selected_pipe_uuids.append(s.pipe);
    harness.build(s, view);
    req(hasColorNear(harness.canvas, MapBgfxEditorOverlay::SelectedColor, vertex.x, vertex.y, 4.0),
        "a selected erroneous pipe gets the blue outline");
}

void testFramesAndAttachments()
{
    Harness harness;
    Scene s = scene();
    const MapBgfx2d::Camera view = camera();
    s.viewport.rectangle_selection_visible = true;
    s.viewport.rectangle_selection = QRect(100, 100, 200, 120);
    harness.build(s, view);
    bool inside = true;
    int frame_vertices = 0;
    for (const MapBgfxHudVertex &vertex : harness.canvas.vertices())
    {
        if (vertex.x > 60.0 && vertex.x < 340.0 && vertex.y > 60.0 && vertex.y < 260.0)
            ++frame_vertices;
        else if (vertex.x < 300.0 && vertex.y < 300.0)
            inside = false;
    }
    req(frame_vertices > 50, "rectangle selection draws its frame");
    req(inside, "rectangle selection stays around its rectangle (glow included)");
    // The blue edge is specific to the drag rectangle, unlike the persistent
    // node icons and pipe-vertex details also drawn by this overlay.
    constexpr quint32 RectangleEdgeColor = 0xb456d7ffu;
    req(hasColor(harness.canvas, RectangleEdgeColor),
        "drag rectangle includes its blue edge");

    // Release: rebuild the same overlay without the drag rectangle. Its edge
    // must disappear; the ordinary network details must remain visible.
    s.viewport.rectangle_selection_visible = false;
    harness.build(s, view);
    req(!hasColor(harness.canvas, RectangleEdgeColor),
        "released entity selection rectangle disappears from the overlay");
    req(harness.probe.calls.size() == 4 && !harness.canvas.vertices().isEmpty(),
        "network editor details remain visible after rectangle release");

    // Tile selection: the tile under the camera at zoom 18, drawn green.
    Scene tiles = scene();
    const double tiles_per_axis = std::exp2(18.0);
    const MapBgfx2d::WorldPoint center = MapBgfx2d::worldFromWgs84(9.0, 47.0);
    tiles.viewport.tile_selection_visible = true;
    tiles.viewport.tile_selection_zoom = 18;
    tiles.viewport.tile_x_min = int(std::floor(center.x * tiles_per_axis));
    tiles.viewport.tile_x_max = tiles.viewport.tile_x_min + 1;
    tiles.viewport.tile_y_min = int(std::floor(center.y * tiles_per_axis));
    tiles.viewport.tile_y_max = tiles.viewport.tile_y_min;
    harness.build(tiles, view);
    const double tile_left = ViewportWidth * 0.5
        + (double(tiles.viewport.tile_x_min) / tiles_per_axis - center.x) * view.world_pixels;
    req(hasColorNear(harness.canvas, 0xe69bff91u, tile_left, 300.0, 300.0),
        "tile selection border is drawn");
    // The inner grid line between the two tiles.
    req(hasColorNear(harness.canvas, 0x6968ff68u, tile_left + 256.0, 300.0, 300.0),
        "tile selection grid line between tiles");

    // The editor still retains the completed selection range for the
    // Delete Tiles action, but the overlay is hidden after mouse release.
    tiles.viewport.tile_selection_visible = false;
    harness.build(tiles, view);
    req(!hasColor(harness.canvas, 0xe69bff91u) &&
        !hasColor(harness.canvas, 0x6968ff68u),
        "released cache selection rectangle and grid disappear from the overlay");
    req(harness.probe.calls.size() == 4 && !harness.canvas.vertices().isEmpty(),
        "network editor details remain visible after cache selection release");
    req(tiles.viewport.tile_x_max == tiles.viewport.tile_x_min + 1,
        "released cache selection keeps its tile bounds");

    // Demand-point attachment: dashed (several separate dashes).
    Scene attachment = scene();
    MapEditorDemandPointAttachmentVisualState state;
    state.visible = true;
    state.demand_point_coordinate_wgs84 = coordinate(9.0, 46.9995);
    state.attachment_coordinate_wgs84 = coordinate(9.0, 47.0);
    state.attachment_coordinate_valid = true;
    attachment.state.demand_point_attachments.append(state);
    harness.build(attachment, view);
    const quint32 dash = MapBgfxHudCanvas::premultipliedAbgr(qRgba(70, 105, 125, 190));
    // 0.0005 deg of latitude ~ 136 px at zoom 18: about 22 dashes of 4 on / 2 off.
    const int dashes = texturedDashes(harness.canvas, harness.cache, dash);
    req(dashes >= 15 && dashes <= 30, "attachment line is dashed (pattern strip, ~22 dashes)");
}

// Terrain tile selections: the same frame in brown; in 3D the frame, fill
// and grid laid on the globe through the surface's projector.
void testTerrainAndGlobeTileSelection()
{
    Harness harness;
    const MapBgfx2d::Camera view = camera();
    const double tiles_per_axis = std::exp2(18.0);
    const MapBgfx2d::WorldPoint center = MapBgfx2d::worldFromWgs84(9.0, 47.0);
    Scene tiles = scene();
    tiles.viewport.tile_selection_visible = true;
    tiles.viewport.tile_selection_kind = MapEditorTileSelectionKind::Terrain;
    tiles.viewport.tile_selection_zoom = 18;
    tiles.viewport.tile_x_min = int(std::floor(center.x * tiles_per_axis));
    tiles.viewport.tile_x_max = tiles.viewport.tile_x_min + 1;
    tiles.viewport.tile_y_min = int(std::floor(center.y * tiles_per_axis));
    tiles.viewport.tile_y_max = tiles.viewport.tile_y_min;
    harness.build(tiles, view);
    const double tile_left = ViewportWidth * 0.5
        + (double(tiles.viewport.tile_x_min) / tiles_per_axis - center.x) * view.world_pixels;
    const quint32 terrain_line = 0xebf0ca96u;
    const quint32 terrain_grid = 0x73dea86cu;
    req(hasColorNear(harness.canvas, terrain_line, tile_left, 300.0, 300.0),
        "terrain tile selection border is drawn brown");
    req(!hasColorNear(harness.canvas, 0xe69bff91u, tile_left, 300.0, 1000.0),
        "terrain tile selection has no green border");
    req(hasColorNear(harness.canvas, terrain_grid, tile_left + 256.0, 300.0, 300.0),
        "terrain tile selection grid line between tiles");

    // Globe: a flat stand-in projection around the camera (1e5 px per
    // degree); the frame's west edge lies where the projector puts it.
    const std::function<double(double)> screen_x = [](double longitude) { return 400.0 + (longitude - 9.0) * 1.0e5; };
    const std::function<double(double)> screen_y = [](double latitude) { return 300.0 - (latitude - 47.0) * 1.0e5; };
    const MapBgfxEditorOverlay::GlobeProjector flat =
        [&screen_x, &screen_y](double longitude, double latitude, QPointF *screen, double *distance_m)
    {
        *screen = QPointF(screen_x(longitude), screen_y(latitude));
        return true;
    };
    const double west_longitude = double(tiles.viewport.tile_x_min) / tiles_per_axis * 360.0 - 180.0;
    const double middle_longitude = double(tiles.viewport.tile_x_min + 1) / tiles_per_axis * 360.0 - 180.0;
    harness.overlay.buildGlobe(&harness.canvas, &harness.cache, tiles.viewport, flat);
    req(hasColorNear(harness.canvas, terrain_line, screen_x(west_longitude), 300.0, 400.0),
        "globe terrain selection frame is drawn on the projected west edge");
    req(hasColorNear(harness.canvas, terrain_grid, screen_x(middle_longitude), 300.0, 400.0),
        "globe terrain selection grid line between tiles");
    const qsizetype full_vertices = harness.canvas.vertices().size();

    // Points beyond the horizon break the lines and drop their fill cells.
    const MapBgfxEditorOverlay::GlobeProjector half =
        [&flat, middle_longitude](double longitude, double latitude, QPointF *screen, double *distance_m)
    {
        return longitude <= middle_longitude + 1e-9 && flat(longitude, latitude, screen, distance_m);
    };
    harness.overlay.buildGlobe(&harness.canvas, &harness.cache, tiles.viewport, half);
    req(harness.canvas.vertices().size() > 0 && harness.canvas.vertices().size() < full_vertices,
        "globe selection drops what lies beyond the horizon");
    for (const MapBgfxHudVertex &vertex : harness.canvas.vertices())
    {
        if (vertex.x > screen_x(middle_longitude) + 10.0)
        {
            req(false, "nothing is drawn east of the horizon");
            break;
        }
    }

    const MapBgfxEditorOverlay::GlobeProjector hidden = [](double, double, QPointF *, double *) { return false; };
    harness.overlay.buildGlobe(&harness.canvas, &harness.cache, tiles.viewport, hidden);
    req(harness.canvas.vertices().isEmpty(), "a selection behind the globe draws nothing");

    // Map tiles stay green on the globe.
    tiles.viewport.tile_selection_kind = MapEditorTileSelectionKind::MapTiles;
    harness.overlay.buildGlobe(&harness.canvas, &harness.cache, tiles.viewport, flat);
    req(hasColorNear(harness.canvas, 0xe69bff91u, screen_x(west_longitude), 300.0, 400.0),
        "globe map tile selection frame is green");
}

// The 3D Select tool draws the exact same screen-space rectangle as 2D,
// independent of the terrain projector. Unlike cache selection, this frame
// must not require a DEM hit or even a network icon to be present.
void testGlobeSharedRectangleSelection()
{
    Harness harness;
    MapEditorViewportRenderState viewport;
    viewport.rectangle_selection_visible = true;
    viewport.rectangle_selection = QRect(120, 90, 240, 160);
    const MapBgfxEditorOverlay::GlobeProjector projector = [](
        double, double, QPointF *, double *) { return false; };
    constexpr quint32 RectangleEdgeColor = 0xb456d7ffu;
    harness.overlay.buildGlobe(&harness.canvas, &harness.cache, viewport, projector);
    req(hasColor(harness.canvas, RectangleEdgeColor),
        "globe entity rectangle uses the same blue frame as 2D even when terrain misses");
    const int frame_vertices = harness.canvas.vertices().size();
    req(frame_vertices > 50,
        "globe selection rectangle produces a visible gradient and outline without network geometry");
    for (const MapBgfxHudVertex &vertex : harness.canvas.vertices())
    {
        if (vertex.x < 50.0 || vertex.x > 430.0 || vertex.y < 20.0 || vertex.y > 320.0)
        {
            req(false, "globe rectangle frame remains within its screen-space bounds");
            break;
        }
    }

    viewport.rectangle_selection_visible = false;
    harness.overlay.buildGlobe(&harness.canvas, &harness.cache, viewport, projector);
    req(harness.canvas.vertices().isEmpty(),
        "globe rectangle is cleared when the shared drag finishes");

    // A terrain-cache selection is still governed by its independent tile
    // geometry, not by the new entity rectangle flag.
    viewport.tile_selection_visible = true;
    viewport.tile_selection_zoom = 10;
    viewport.tile_x_min = 530;
    viewport.tile_x_max = 530;
    viewport.tile_y_min = 370;
    viewport.tile_y_max = 370;
    harness.overlay.buildGlobe(&harness.canvas, &harness.cache, viewport, projector);
    req(!hasColor(harness.canvas, RectangleEdgeColor),
        "globe cache selection cannot accidentally draw the hydraulic rectangle");
}

// 3D-00B: the same editor visual state is projected over the globe. This
// tests rendering only; no separate globe placement/controller is involved.
void testGlobeSharedEditorVisuals()
{
    Harness harness;
    Scene s = scene();
    const MapBgfxEditorOverlay::GlobeProjector projector = [](
        double longitude, double latitude, QPointF *screen, double *distance)
    {
        *screen = QPointF(400.0 + (longitude - 9.0) * 1.0e5,
                          300.0 - (latitude - 47.0) * 1.0e5);
        if (distance != nullptr)
            *distance = 100.0;
        return true;
    };
    const MapCanvasGlobeProjection projection(
        [](const QPointF &screen, const QSize &) -> std::optional<CoordinateWGS84>
        {
            return coordinate(9.0 + (screen.x() - 400.0) / 1.0e5,
                              47.0 - (screen.y() - 300.0) / 1.0e5);
        },
        [&projector](const CoordinateWGS84 &world, const QSize &)
            -> std::optional<QPointF>
        {
            QPointF screen;
            if (!projector(world.longitude_deg, world.latitude_deg, &screen, nullptr))
                return std::nullopt;
            return screen;
        });
    const std::function<void()> build = [&harness, &s, &projector, &projection]()
    {
        harness.overlay.buildGlobe(&harness.canvas, &harness.cache, s.viewport,
                                   projector, &s.snapshot, EntityWidth,
                                   QSizeF(ViewportWidth, ViewportHeight), {},
                                   &s.state, &projection);
    };

    build();
    req(hasColorNear(harness.canvas, MapBgfxEditorOverlay::BlackColor, 400.0, 280.0, 4.5),
        "unselected 3D pipe bend has the same black dot as the 2D editor");

    s.state.selected_pipe_uuids.append(s.pipe);
    s.state.selected_marker_uuids.append(s.junction_a);
    build();
    req(hasColor(harness.canvas, MapBgfxEditorOverlay::SelectedColor),
        "shared selected pipe and its bend dots are drawn on the globe");
    req(harness.probe.count(Highlight::Selected) == 1,
        "3D selected junction uses the same highlighted Editor icon");

    s.state.placement.creating = true;
    s.state.placement.entity = InfrastructureEntity::Pipe;
    s.state.placement.pipe_start_node_uuid = s.junction_a;
    s.state.placement.pipe_intermediate_vertices.append(coordinate(9.0, 47.0002));
    s.state.placement.mouse_position = QPointF(10.0, 10.0); // deliberately wrong 2D pointer
    s.state.placement.mouse_coordinate_wgs84 = coordinate(9.0004, 47.0001);
    s.state.placement.mouse_coordinate_wgs84_valid = true;
    build();
    req(hasColorNear(harness.canvas, MapBgfxEditorOverlay::PreviewColor, 440.0, 290.0, 2.0),
        "3D pipe preview uses the shared geographic pointer instead of 2D pixels");
    req(hasColorNear(harness.canvas, MapBgfxEditorOverlay::PreviewColor, 400.0, 280.0, 4.5),
        "intermediate 3D pipe bend has the shared preview marker");

    s.state.placement.connection_target_uuid = s.junction_b;
    build();
    req(hasColorNear(harness.canvas, MapBgfxEditorOverlay::PreviewColor, 450.0 + 9.0, 300.0, 1.0),
        "snapped 3D target uses the same ring as 2D");
    req(!hasColorNear(harness.canvas, MapBgfxEditorOverlay::PreviewColor, 440.0, 290.0, 2.0),
        "snapped 3D pipe preview stops at target rather than the cursor");

    s.state.placement = MapEditorPlacementVisualState();
    s.state.placement.creating = true;
    s.state.placement.entity = InfrastructureEntity::Tank;
    s.state.placement.floating_marker_visible = true;
    s.state.placement.floating_width = 30;
    s.state.placement.mouse_coordinate_wgs84 = coordinate(9.0, 47.0);
    s.state.placement.mouse_coordinate_wgs84_valid = true;
    build();
    req(hasSpriteCorner(harness.canvas, harness.cache, 400.0, 300.0 - IconHeight),
        "shared floating Tank sprite is bottom-anchored on the 3D terrain");
    bool floating_tank = false;
    for (const IconCall &call : harness.probe.calls)
        floating_tank = floating_tank || (call.entity == InfrastructureEntity::Tank
                                         && call.width == 30);
    req(floating_tank, "3D floating sprite uses exactly the 2D Editor icon provider");

    s.state.placement = MapEditorPlacementVisualState();
    s.state.placement.creating = true;
    s.state.placement.entity = InfrastructureEntity::Pump;
    s.state.placement.device_link_start_node_uuid = s.junction_a;
    s.state.placement.floating_marker_visible = true;
    s.state.placement.floating_width = EntityWidth;
    s.state.placement.mouse_coordinate_wgs84 = coordinate(9.0005, 47.0);
    s.state.placement.mouse_coordinate_wgs84_valid = true;
    build();
    req(hasColor(harness.canvas, MapBgfxEditorOverlay::PreviewColor),
        "3D pump placement uses the shared link preview");
    req(hasSpriteCorner(harness.canvas, harness.cache,
                        400.0 - EntityWidth / 2.0, 300.0 - IconHeight / 2.0),
        "3D pump floating icon is centered on the preview midpoint");

    s.state.placement.mouse_coordinate_wgs84_valid = false;
    s.state.placement.connection_target_uuid = QUuid();
    build();
    req(!hasColor(harness.canvas, MapBgfxEditorOverlay::PreviewColor),
        "invalid 3D cursor position does not draw a fabricated link preview");

    // The shared demand-point attachment state must also render on the
    // globe, with the same selected colour and handle as the flat editor.
    // No separate 3D attachment UI or placement state is introduced.
    s.state.placement = MapEditorPlacementVisualState();
    s.state.selected_pipe_uuids.clear();
    s.state.selected_marker_uuids.clear();
    MapEditorDemandPointAttachmentVisualState attachment;
    attachment.visible = true;
    attachment.selected = true;
    attachment.demand_point_coordinate_wgs84 = coordinate(9.0, 46.9995);
    attachment.attachment_coordinate_wgs84 = coordinate(9.0, 47.0);
    attachment.attachment_coordinate_valid = true;
    s.state.demand_point_attachments.append(attachment);
    build();
    req(hasColor(harness.canvas, 0xff0078beu),
        "3D demand attachment uses shared selected guide and handle colour");
    req(hasColorNear(harness.canvas, 0xff0078beu, 400.0, 300.0, 12.0),
        "3D demand attachment draws a handle at the network endpoint");

    s.state.demand_point_attachments[0].selected = false;
    s.state.demand_point_attachments[0].selecting_target = true;
    s.state.demand_point_attachments[0].attachment_coordinate_valid = false;
    build();
    req(hasColorNear(harness.canvas, 0xff0078beu, 400.0, 350.0, 13.0),
        "3D demand reattachment draws the shared target-selection ring");
    req(!hasColorNear(harness.canvas, 0xff0078beu, 400.0, 300.0, 12.0),
        "invalid 3D attachment target never keeps the previous handle");

    s.state.demand_point_attachments[0].visible = false;
    build();
    req(!hasColor(harness.canvas, 0xff0078beu),
        "hidden 3D demand attachment leaves no stale guide");
    s.state.demand_point_attachments.clear();

    // The demand-attachment case cleared selection above. Restore the
    // selected pipe and node that this movement-preview case is asserting.
    s.state.selected_pipe_uuids.append(s.pipe);
    s.state.selected_marker_uuids.append(s.junction_a);

    // The 3D move consumes the same state.move data generated by the 2D
    // hydraulic editor; there is no globe-specific drag/placement state.
    s.state.placement = MapEditorPlacementVisualState();
    s.state.move.active = true;
    MapEditorDynamicMarkerVisualState moved;
    moved.uuid = s.junction_a;
    moved.entity = InfrastructureEntity::Junction;
    moved.coordinate_wgs84 = coordinate(9.0015, 47.0002);
    s.state.move.markers.append(moved);
    MapEditorDynamicLinkVisualState connected;
    connected.uuid = s.pipe;
    connected.entity = InfrastructureEntity::Pipe;
    connected.vertices_wgs84.append(moved.coordinate_wgs84);
    connected.vertices_wgs84.append(coordinate(9.0, 47.0002));
    connected.vertices_wgs84.append(coordinate(9.0005, 47.0));
    s.state.move.links.append(connected);
    build();
    // The HUD line is a quad with vertices at its endpoints, not at the
    // midpoint of the segment. Verify the selected stroke reaches the moved
    // node at (550, 280), rather than testing for a nonexistent midpoint vertex.
    req(hasColorNear(harness.canvas, MapBgfxEditorOverlay::SelectedColor,
                     550.0, 280.0, 3.0),
        "3D move uses the same selected connected-pipe stroke as 2D");
    req(hasSpriteCorner(harness.canvas, harness.cache,
                        550.0 - 9.0, 280.0 - IconHeight - 9.0),
        "3D move shows the shared relocated marker sprite");
}

// The 3D editor projects its full-colour 2D sprites onto the globe. It must
// not fall back to the monitor's generic star atlas, and off-horizon entities
// must not create HUD sprites.
void testGlobeEditorIcons()
{
    Harness harness;
    Scene s = scene();
    s.snapshot.nodes.clear();
    s.snapshot.links.clear();
    NetworkRenderNode demand;
    demand.uuid = QUuid::createUuid();
    demand.entity_type = InfrastructureEntity::DemandPoint;
    demand.coordinate_wgs84 = coordinate(9.0, 47.0);
    s.snapshot.nodes.append(demand);
    NetworkRenderNode junction;
    junction.uuid = QUuid::createUuid();
    junction.entity_type = InfrastructureEntity::Junction;
    junction.coordinate_wgs84 = coordinate(9.0001, 47.0);
    s.snapshot.nodes.append(junction);
    NetworkRenderLink pump;
    pump.uuid = QUuid::createUuid();
    pump.entity_type = InfrastructureEntity::Pump;
    pump.vertices_wgs84.append(coordinate(9.0, 47.0));
    pump.vertices_wgs84.append(coordinate(9.0002, 47.0));
    s.snapshot.links.append(pump);

    const MapBgfxEditorOverlay::GlobeProjector visible = [](
        double longitude, double latitude, QPointF *point, double *)
    {
        *point = QPointF(400.0 + (longitude - 9.0) * 1.0e5,
                         300.0 - (latitude - 47.0) * 1.0e5);
        return true;
    };
    harness.overlay.buildGlobe(&harness.canvas, &harness.cache, s.viewport,
                               visible, &s.snapshot, EntityWidth,
                               QSizeF(ViewportWidth, ViewportHeight));
    req(harness.probe.calls.size() == 2,
        "globe editor renders demand-point and pump pixmaps, not junction stars");
    req(harness.probe.calls.size() == 2
        && harness.probe.calls.at(0).entity == InfrastructureEntity::DemandPoint
        && harness.probe.calls.at(1).entity == InfrastructureEntity::Pump,
        "globe editor requests its real 2D entity icon types");
    req(hasSpriteCorner(harness.canvas, harness.cache, 400.0, 300.0 - IconHeight),
        "globe demand-point icon retains the editor's bottom anchoring");
    req(hasSpriteCorner(harness.canvas, harness.cache, 400.0 + 10.0 - EntityWidth / 2.0,
                        300.0 - IconHeight / 2.0),
        "globe device-link icon is centred like the 2D editor");

    // The hit target must match the actually drawn full-colour pixmap, not
    // the smaller 3D network point or the underlying pipe geometry.
    const std::optional<InfrastructureEntityReference> demand_hit =
        harness.overlay.hitTestGlobeIcon(QPointF(405.0, 280.0));
    req(demand_hit.has_value() && demand_hit->type == InfrastructureEntity::DemandPoint
        && demand_hit->uuid == demand.uuid,
        "globe sprite hit picks a demand point above its terrain anchor");
    const std::optional<InfrastructureEntityReference> pump_hit =
        harness.overlay.hitTestGlobeIcon(QPointF(415.0, 305.0));
    req(pump_hit.has_value() && pump_hit->type == InfrastructureEntity::Pump
        && pump_hit->uuid == pump.uuid,
        "globe sprite hit picks the centered device-link icon");
    const std::optional<InfrastructureEntityReference> overlap_hit =
        harness.overlay.hitTestGlobeIcon(QPointF(405.0, 295.0));
    req(overlap_hit.has_value() && overlap_hit->uuid == pump.uuid,
        "overlapping globe sprites pick the last painted entity");
    req(!harness.overlay.hitTestGlobeIcon(QPointF(399.0, 280.0)).has_value(),
        "globe sprite hit excludes pixels beyond the actual image rectangle");
    req(!harness.overlay.hitTestGlobeIcon(QPointF(NAN, 280.0)).has_value(),
        "globe sprite hit rejects invalid pointer positions");

    harness.overlay.buildGlobe(&harness.canvas, &harness.cache, s.viewport,
                               visible, &s.snapshot, EntityWidth * 2,
                               QSizeF(ViewportWidth, ViewportHeight));
    const std::optional<InfrastructureEntityReference> resized_hit =
        harness.overlay.hitTestGlobeIcon(QPointF(425.0, 282.0));
    req(resized_hit.has_value() && resized_hit->uuid == demand.uuid,
        "globe sprite hit rectangle follows the Editor icon-size slider");

    // Reverse the original snapshot order by supplying camera distances:
    // the pump is farther away than the demand point, hence drawn first.
    const MapBgfxEditorOverlay::GlobeProjector depth_order = [](
        double longitude, double latitude, QPointF *point, double *distance_m)
    {
        *point = QPointF(400.0 + (longitude - 9.0) * 1.0e5,
                         300.0 - (latitude - 47.0) * 1.0e5);
        if (distance_m != nullptr)
            *distance_m = longitude > 9.00005 ? 1000.0 : 100.0;
        return true;
    };
    harness.probe.calls.clear();
    harness.overlay.buildGlobe(&harness.canvas, &harness.cache, s.viewport,
                               depth_order, &s.snapshot, EntityWidth,
                               QSizeF(ViewportWidth, ViewportHeight));
    req(harness.probe.calls.size() == 2
        && harness.probe.calls.at(0).entity == InfrastructureEntity::Pump
        && harness.probe.calls.at(1).entity == InfrastructureEntity::DemandPoint,
        "3D editor icons use far-to-near painter ordering, not snapshot order");
    const std::optional<InfrastructureEntityReference> depth_hit =
        harness.overlay.hitTestGlobeIcon(QPointF(405.0, 295.0));
    req(depth_hit.has_value() && depth_hit->uuid == demand.uuid,
        "globe sprite selection follows far-to-near painter order");
    // Equal-depth sprites still respect source order after switching the
    // candidate query to kd-tree traversal order (no O(n log n) index sort).
    const MapBgfxEditorOverlay::GlobeProjector same_depth = [](
        double longitude, double latitude, QPointF *point, double *distance_m)
    {
        *point = QPointF(400.0 + (longitude - 9.0) * 1.0e5,
                         300.0 - (latitude - 47.0) * 1.0e5);
        if (distance_m != nullptr)
            *distance_m = 100.0;
        return true;
    };
    harness.probe.calls.clear();
    harness.overlay.buildGlobe(&harness.canvas, &harness.cache, s.viewport,
                               same_depth, &s.snapshot, EntityWidth,
                               QSizeF(ViewportWidth, ViewportHeight));
    req(harness.probe.calls.size() == 2
        && harness.probe.calls.at(0).entity == InfrastructureEntity::DemandPoint
        && harness.probe.calls.at(1).entity == InfrastructureEntity::Pump,
        "equal-distance globe icon painter order matches source order");

    const MapBgfxEditorOverlay::GlobeProjector hidden = [](
        double, double, QPointF *, double *) { return false; };
    harness.probe.calls.clear();
    harness.overlay.buildGlobe(&harness.canvas, &harness.cache, s.viewport,
                               hidden, &s.snapshot, EntityWidth,
                               QSizeF(ViewportWidth, ViewportHeight));
    req(harness.probe.calls.isEmpty() && harness.canvas.vertices().isEmpty(),
        "off-horizon globe entities never generate icon sprites");
    req(!harness.overlay.hitTestGlobeIcon(QPointF(405.0, 295.0)).has_value(),
        "hidden globe icons are not selectable from a previous render frame");

    const MapBgfxEditorOverlay::GlobeProjector offscreen = [](
        double, double, QPointF *point, double *)
    {
        *point = QPointF(1000000.0, 1000000.0);
        return true;
    };
    harness.probe.calls.clear();
    harness.overlay.buildGlobe(&harness.canvas, &harness.cache, s.viewport,
                               offscreen, &s.snapshot, EntityWidth,
                               QSizeF(ViewportWidth, ViewportHeight));
    req(harness.probe.calls.isEmpty() && harness.canvas.vertices().isEmpty(),
        "off-viewport globe icons are culled before atlas allocation");
    req(!harness.overlay.hitTestGlobeIcon(QPointF(405.0, 295.0)).has_value(),
        "off-viewport icons cannot leave stale picking rectangles");

    // A far-away sprite can be in the same snapshot but outside the camera
    // frustum. The spatial index must reject it *before* the costly projector.
    NetworkRenderNode far_node = demand;
    far_node.uuid = QUuid::createUuid();
    far_node.coordinate_wgs84 = coordinate(120.0, 47.0);
    s.snapshot.nodes.append(far_node);
    ++s.snapshot.geometry_revision;
    const GeoWgs84Ellipsoid::EcefPositionD near_ecef =
        GeoWgs84Ellipsoid::geodeticToEcefD(9.0, 47.0, 0.0);
    MapBgfxGlobeIconIndex::Frustum frustum;
    frustum.valid = true;
    frustum.sides = {{{1.0, 0.0, 0.0, -near_ecef.x + 5000.0},
                      {-1.0, 0.0, 0.0, near_ecef.x + 5000.0},
                      {0.0, 1.0, 0.0, 10000000.0},
                      {0.0, -1.0, 0.0, 10000000.0}}};
    int projection_count = 0;
    const MapBgfxEditorOverlay::GlobeProjector counting =
        [&projection_count](double, double, QPointF *point, double *distance_m)
    {
        ++projection_count;
        *point = QPointF(400.0, 300.0);
        if (distance_m != nullptr)
            *distance_m = 100.0;
        return true;
    };
    harness.probe.calls.clear();
    harness.overlay.buildGlobe(&harness.canvas, &harness.cache, s.viewport,
                               counting, &s.snapshot, EntityWidth,
                               QSizeF(ViewportWidth, ViewportHeight), frustum);
    req(projection_count == 2 && harness.probe.calls.size() == 2,
        "frustum rejects distant anchors before terrain-aware projection");
    projection_count = 0;
    harness.overlay.buildGlobe(&harness.canvas, &harness.cache, s.viewport,
                               counting, &s.snapshot, EntityWidth,
                               QSizeF(ViewportWidth, ViewportHeight), frustum);
    req(projection_count == 2,
        "camera-stable icon geometry reuses the retained spatial index");
}

// Attachments run per frame for every demand point: culled, clipped, and
// without handles where the line is too short to show one.
void testAttachmentCost()
{
    Harness harness;
    const MapBgfx2d::Camera view = camera();
    const quint32 dash = MapBgfxHudCanvas::premultipliedAbgr(qRgba(70, 105, 125, 190));
    const std::function<MapEditorDemandPointAttachmentVisualState(double, double, double, double)>
        attachment = [](double lon0, double lat0, double lon1, double lat1)
    {
        MapEditorDemandPointAttachmentVisualState state;
        state.visible = true;
        state.demand_point_coordinate_wgs84 = coordinate(lon0, lat0);
        state.attachment_coordinate_wgs84 = coordinate(lon1, lat1);
        state.attachment_coordinate_valid = true;
        return state;
    };

    // Entirely off screen (0.01 deg = ~1900 px away): nothing at all.
    Scene away = scene();
    away.snapshot.nodes.clear();
    away.snapshot.links.clear();
    away.state.demand_point_attachments.append(attachment(9.01, 47.0, 9.0101, 47.0));
    harness.build(away, view);
    req(harness.canvas.vertices().isEmpty(), "attachment: off-screen attachment draws nothing");

    // Crossing the screen from far away: only the visible dashes, a few
    // hundred, not the ~1000 of the whole line.
    Scene across = away;
    across.state.demand_point_attachments.clear();
    across.state.demand_point_attachments.append(attachment(8.99, 47.0, 9.01, 47.0));
    harness.build(across, view);
    const int dashes = texturedDashes(harness.canvas, harness.cache, dash);
    req(dashes > 50 && dashes < 160, "attachment: dashes clipped to the viewport");
    req(harness.canvas.vertices().size() <= 4 * 4,
        "attachment: a few pattern quads instead of one quad per dash");
    req(spriteVerticesNear(harness.canvas, harness.cache, 0.0, 0.0, 1e9) == 0,
        "attachment: no handle when it is off screen");

    // Shorter on screen than MinimumAttachmentHandleLinePx: no handle.
    Scene tiny = away;
    tiny.state.demand_point_attachments.clear();
    tiny.state.demand_point_attachments.append(attachment(9.0, 47.0, 9.00003, 47.0));
    harness.build(tiny, view);
    const MapBgfxHudPoint network = project(view, 9.00003, 47.0);
    req(spriteVerticesNear(harness.canvas, harness.cache, network.x, network.y, 12.0) == 0,
        "attachment: no handle on a short line");
    tiny.state.demand_point_attachments[0].selected = true;
    harness.build(tiny, view);
    req(spriteVerticesNear(harness.canvas, harness.cache, network.x, network.y, 12.0) == 4,
        "attachment: a selected one keeps its handle (one disc sprite)");
}

// Pure pans (tab_map_editor_container.cpp): the overlay built with the pan
// margin (MapBgfxPanReuse) and drawn with the reuse offset puts every icon
// where an exact build for the panned camera does, and icons that scroll in
// from beyond the edge are part of it already.
void testPanReuse()
{
    Harness harness;
    Scene s = scene();
    const QSizeF viewport(ViewportWidth, ViewportHeight);
    const double ratio = harness.cache.devicePixelRatio();
    MapBgfx2d::Camera panned = camera();
    // Junction C (9.0010) about 880 px from the left: right of the screen.
    const MapBgfxHudPoint c_start = project(panned, 9.0010, 47.0);
    panned.center.x -= (880.0 - c_start.x) / panned.world_pixels;
    MapBgfxPanReuse pan;
    pan.begin(camera(), viewport, ratio, true);
    const MapBgfxPanReuse::Build build = pan.begin(panned, viewport, ratio, true);
    req(build.margin > 100.0, "pan reuse: a pan frame builds with the margin");

    harness.build(s, panned);
    const int exact_icons = harness.probe.calls.size();
    harness.build(s, build.camera, false, QPointF(), build.viewport);
    req(harness.probe.calls.size() > exact_icons,
        "pan reuse: the margin build contains the icon beyond the right edge");

    MapBgfx2d::Camera later = panned;
    later.center.x += 120.0 / panned.world_pixels;
    later.center.y += 13.4 / panned.world_pixels;
    QPointF offset;
    req(pan.reuse(later, viewport, ratio, &offset), "pan reuse: reused within the margin");
    const double tolerance = 1.0 / ratio + 0.01;
    const double nodes[3] = {8.9995, 9.0005, 9.0010};
    for (const double longitude : nodes)
    {
        const MapBgfxHudPoint exact = project(later, longitude, 47.0);
        req(spriteVerticesNear(harness.canvas, harness.cache, double(qRound(exact.x)) - offset.x(),
                               double(qRound(exact.y)) - offset.y(), tolerance) > 0,
            "pan reuse: translated icon corner where an exact build puts it");
    }
    const MapBgfxHudPoint c_later = project(later, 9.0010, 47.0);
    req(c_later.x > 0.0 && c_later.x < ViewportWidth, "pan reuse: junction C scrolled in");
}

void testAntimeridian()
{
    Harness harness;
    Scene s = scene();
    s.snapshot.geometry_revision = 9;
    s.snapshot.nodes[0].coordinate_wgs84 = coordinate(-179.9999, 0.0);
    s.snapshot.links.clear();
    const MapBgfx2d::Camera view = camera(179.9999, 0.0);
    harness.build(s, view);
    const MapBgfx2d::WorldPoint node = MapBgfx2d::worldFromWgs84(-179.9999, 0.0);
    const MapBgfx2d::ScreenPoint screen = MapBgfx2d::screenFromWorld(view, node);
    req(std::fabs(screen.x - 0.5 * ViewportWidth) < 200.0, "node across the antimeridian is near the camera");
    req(hasSpriteCorner(harness.canvas, harness.cache, double(qRound(screen.x)), double(qRound(screen.y))),
        "its icon is drawn at the nearest world copy");
}
}

int main(int argc, char **argv)
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication application(argc, argv);

    testMapManagementOverlay();
    testStaticDetails();
    testSelection();
    testPlacement();
    testMove();
    testDiagnostics();
    testFramesAndAttachments();
    testTerrainAndGlobeTileSelection();
    testGlobeSharedRectangleSelection();
    testGlobeEditorIcons();
    testGlobeSharedEditorVisuals();
    testAttachmentCost();
    testPanReuse();
    testAntimeridian();

    if (fails == 0)
        std::printf("bgfx editor overlay tests passed\n");
    return fails == 0 ? 0 : 1;
}
