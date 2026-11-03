#ifndef MAP_BGFX_EDITOR_OVERLAY_H
#define MAP_BGFX_EDITOR_OVERLAY_H

#include "map/bgfx/map_bgfx_2d_projection.h"
#include "map/bgfx/map_bgfx_hud_canvas.h"
#include "map/bgfx/map_bgfx_globe_icon_index.h"
#include "map/bgfx/map_bgfx_hud_text_cache.h"
#include "map/editor/map_editor_visual_state.h"
#include "network/network_render_snapshot.h"

#include <QHash>
#include <QPointF>
#include <QRectF>
#include <QSet>
#include <QSizeF>
#include <QString>
#include <QUuid>
#include <QVector>

#include <functional>
#include <optional>

class MapCanvasProjection;

// Map editor on bgfx (roadmap Phase 2): everything the former QPainter editor
// (MapEditorRenderer, removed) drew on top of the GPU network in its
// render-surface mode, rebuilt per frame into a
// MapBgfxHudCanvas from the editor's visual state and drawn with the HUD
// program between the network and the HUD:
//
// - tile-cache selection (map tiles: green tiles with glow and grid; terrain
//   tiles: the same in brown) and the rectangle selection (steel frame with
//   blue glow and dashed inner frame);
// - the full-colour entity icons (nodes bottom-anchored, device links
//   centred) with the selected / error / stale glows, pipe vertex dots and
//   demand-point attachment lines with their handles;
// - selection (blue pipes, device-link lines, glowing icons), the connection
//   target, simulation errors (red / grey, blue outline when selected);
// - placement feedback: pipe and device-link rubber bands, the floating
//   marker; the move preview of dragged entities (hidden in the GPU network
//   meanwhile) and the full-network move (the GPU network is shifted by the
//   camera, the icons here by the same translation).
//
// The network lines and node dots themselves stay in the retained GPU scene
// (editor symbology: black pipes, brown device links, black dots).
// Positions use the continuous camera, exactly like the GPU network, so the
// overlay never drifts against it while zooming.
//
// Pure Qt Gui + math (no bgfx, no widgets); icons come from an
// IconProvider (map_bgfx_editor_icons.h composites the glows like
// MapEntityPixmapRenderer), so the builder is unit-tested without a GPU.
class MapBgfxEditorOverlay
{
public:
    // MapEntityPixmapRenderer::Highlight.
    enum class Highlight
    {
        None,
        Selected,
        Error,
        SelectedError,
        Stale,
        SelectedStale
    };

    // An icon in the overlay atlas. width/height: the pixmap's logical size
    // (MapEntityPixmapRenderer::pixmap(path, width)); the sprite carries
    // `padding` logical pixels of glow room on every side.
    struct Icon
    {
        MapBgfxHudSprite sprite;
        double width = 0.0;
        double height = 0.0;
        double padding = 0.0;
    };
    // pixmap_path empty: the entity type's icon.
    using IconProvider = std::function<Icon(MapBgfxHudTextCache *cache,
                                            InfrastructureEntity entity,
                                            const QString &pixmap_path, int width,
                                            Highlight highlight)>;

    struct Inputs
    {
        QSizeF viewport;
        // Continuous 2D camera of the frame (the GPU network's camera
        // without the full-move translation).
        MapBgfx2d::Camera camera;
        const NetworkRenderSnapshot *snapshot = nullptr;
        const MapEditorVisualState *visual_state = nullptr;
        const MapEditorViewportRenderState *viewport_state = nullptr;
        // The whole network is being moved: the GPU network is drawn
        // shifted by this many logical pixels, and so are the icons here.
        // Map Management keeps the cache selection overlays without network
        // entities or editing previews, including on a persistent GPU surface.
        bool tile_only = false;
        bool full_network_move = false;
        QPointF full_network_move_translation;
    };

    void setIconProvider(IconProvider provider) { this->icon_provider = std::move(provider); }

    // Builds the overlay into canvas (cleared first); rebuilds once when the
    // atlas had to be cleared during the build.
    void build(MapBgfxHudCanvas *canvas, MapBgfxHudTextCache *cache, const Inputs &inputs);

    // Globe mode: use the same full-colour editor entity icons as 2D,
    // projected onto the terrain-aware globe, plus the tile-cache selection.
    // Icons are drawn far-to-near using the projector camera distance, not
    // insertion order. The optional shared editor state supplies exactly the
    // same placement/selection previews as 2D, using the globe projection;
    // hydraulic editing itself is not enabled by this rendering hook.
    using GlobeProjector = std::function<bool(double longitude_deg, double latitude_deg,
                                              QPointF *screen_position, double *distance_m)>;
    void buildGlobe(MapBgfxHudCanvas *canvas, MapBgfxHudTextCache *cache,
                    const MapEditorViewportRenderState &viewport_state,
                    const GlobeProjector &projector,
                    const NetworkRenderSnapshot *snapshot = nullptr,
                    int icon_width = 0, const QSizeF &viewport_size = QSizeF(),
                    const MapBgfxGlobeIconIndex::Frustum &frustum = {},
                    const MapEditorVisualState *editor_state = nullptr,
                    const MapCanvasProjection *editor_projection = nullptr);
    // The rectangles are recorded while painting the actual 3D Editor sprites.
    // Query them before the globe network geometry hit-test so clicking an
    // icon (including through terrain) selects the entity the user sees.
    // Returns no hit when the last globe frame contained no drawable icons.
    std::optional<InfrastructureEntityReference> hitTestGlobeIcon(
        const QPointF &position) const;
    // Globe tile selection lattice: at most this many cells per axis (each
    // lattice point is one projection, relief lookup included), at most
    // this many cells per tile (relief detail along the frame and grid).
    static constexpr int GlobeSelectionMaximumCells = 48;
    static constexpr int GlobeSelectionMaximumCellsPerTile = 16;

    // The former QPainter editor's sizes (logical px) and colours (0xAARRGGBB).
    static constexpr double MarkerDotRadiusPx = 5.0;
    static constexpr double ConnectionTargetRadiusPx = 9.0;
    static constexpr double PipeVertexRadiusPx = 4.0;
    static constexpr double LineWidthPx = 3.0;
    static constexpr double SelectedOutlineWidthPx = 7.0;
    static constexpr quint32 BlackColor = 0xff000000u;
    static constexpr quint32 SelectedColor = 0xff00beffu;
    static constexpr quint32 PreviewColor = 0xff008cffu;
    static constexpr quint32 DeviceLinkColor = 0xff8b5a2bu;
    static constexpr quint32 ErrorColor = 0xffff0000u;
    static constexpr quint32 StaleColor = 0xff808080u;
    // Demand-point attachment handles are drawn only where the attachment
    // line is at least this long on screen (or it is selected / being
    // edited): zoomed out, thousands of handles would only cover the dots.
    static constexpr double MinimumAttachmentHandleLinePx = 12.0;
    // Logical length of a dash pattern strip in the atlas (rounded down to
    // whole periods).
    static constexpr double DashStripLengthPx = 512.0;

private:
    // Snapshot geometry in normalized Web Mercator, built once per geometry
    // revision (the per-frame work is then a linear transform).
    struct Geometry
    {
        bool valid = false;
        quint64 geometry_revision = 0;
        const NetworkRenderSnapshot *snapshot = nullptr;
        QVector<MapBgfx2d::WorldPoint> node_world;
        QVector<QVector<MapBgfx2d::WorldPoint>> link_world;
        // Per link: x range continuous from its first point (unwrapped like
        // screenPolyline()), y range; culls off-screen pipes before they are
        // projected.
        struct Bounds
        {
            double min_x = 0.0;
            double max_x = 0.0;
            double min_y = 0.0;
            double max_y = 0.0;
        };
        QVector<Bounds> link_bounds;
        QVector<MapBgfx2d::WorldPoint> device_center_world;
        QHash<QUuid, int> node_index;
        QHash<QUuid, int> link_index;
    };

    void synchronizeGeometry(const NetworkRenderSnapshot &snapshot);
    void buildOnce();

    // Projection with the current translation offset.
    MapBgfxHudPoint screen(const MapBgfx2d::WorldPoint &world) const;
    MapBgfxHudPoint screen(const CoordinateWGS84 &coordinate) const;
    // A polyline unwrapped continuously, then placed at its copy nearest to
    // the camera (screenSegment() rule for whole links).
    QVector<MapBgfxHudPoint> screenPolyline(const QVector<MapBgfx2d::WorldPoint> &world) const;
    QVector<MapBgfxHudPoint> screenPolyline(const QList<CoordinateWGS84> &coordinates) const;

    // QPainter pens: round caps and joins.
    void polyline(const QVector<MapBgfxHudPoint> &points, double width, quint32 argb);
    // Qt::DashLine (4 on, 2 off) / Qt::DotLine (1 on, 2 off) in pen widths.
    // Drawn as textured quads with a dash pattern strip from the atlas (one
    // quad per DashStripLengthPx, not one per dash: thousands of attachment
    // lines per frame).
    void dashedPolyline(const QVector<MapBgfxHudPoint> &points, double width, double dash,
                        double gap, double offset, quint32 argb);
    void dashedSegment(const MapBgfxHudPoint &a, const MapBgfxHudPoint &b, double width,
                       double dash, double gap, double phase, quint32 argb);
    // A filled disc with a ring (pen centred on radius), as one sprite.
    void disc(double x, double y, double radius, double pen, quint32 fill_argb,
              quint32 ring_argb);

    // Pattern strips and discs in the overlay atlas, memoized per atlas
    // generation (no string keys per draw).
    struct DashStrip
    {
        double width = 0.0;
        double dash = 0.0;
        double gap = 0.0;
        // Logical length: a whole number of periods.
        double length = 0.0;
        MapBgfxHudSprite sprite;
    };
    struct Disc
    {
        double radius = 0.0;
        double pen = 0.0;
        quint32 fill = 0;
        quint32 ring = 0;
        // Logical edge length of the square sprite.
        double extent = 0.0;
        MapBgfxHudSprite sprite;
    };
    DashStrip dashStrip(double width, double dash, double gap);
    Disc discSprite(double radius, double pen, quint32 fill_argb, quint32 ring_argb);
    void synchronizeSpriteMemo();
    QVector<DashStrip> dash_strips;
    QVector<Disc> discs;
    quint64 sprite_memo_generation = 0;
    bool sprite_memo_valid = false;
    void dashedCircle(double x, double y, double radius, double width, double dash, double gap,
                      quint32 argb);
    // QPen stroke centred on the path of a rect (sharp corners or radius).
    void strokeRectCentered(const MapBgfxHudRect &rect, double radius, double width, quint32 argb);
    bool onScreen(double x, double y, double width, double height) const;
    // A link's world bounds intersect the viewport grown by margin_px.
    bool boundsOnScreen(const Geometry::Bounds &bounds, double margin_px) const;

    // Icon helpers: bottom-anchored at the rounded anchor (nodes) or
    // centred (device links, MapEntityPixmapRenderer rects).
    void bottomAnchoredIcon(const MapBgfxHudPoint &anchor, InfrastructureEntity entity,
                            const QString &path, int width, Highlight highlight, bool round_anchor);
    void centeredIcon(const MapBgfxHudPoint &center, InfrastructureEntity entity,
                      const QString &path, int width, Highlight highlight);
    void drawIcon(double x, double y, const Icon &icon);

    void tileSelection();
    void globeTileSelection(const MapEditorViewportRenderState &viewport_state,
                            const GlobeProjector &projector);
    struct GlobeIconAnchor
    {
        CoordinateWGS84 coordinate;
        QUuid uuid;
        InfrastructureEntity entity = InfrastructureEntity::Unknown;
        bool bottom_anchored = false;
    };
    struct GlobeIconHitArea
    {
        InfrastructureEntityReference entity;
        QRectF bounds;
    };
    // Built on a network geometry revision only; never on camera movement.
    void synchronizeGlobeIcons(const NetworkRenderSnapshot &snapshot);
    void globeEditorVisuals(const NetworkRenderSnapshot &snapshot,
                            const MapEditorVisualState &state,
                            const MapCanvasProjection &projection,
                            const QSizeF &viewport_size, int icon_width);
    void globeNetworkIcons(const NetworkRenderSnapshot &snapshot,
                           const GlobeProjector &projector, int icon_width,
                           const QSizeF &viewport_size,
                           const MapBgfxGlobeIconIndex::Frustum &frustum,
                           const MapEditorVisualState *editor_state);
    MapBgfxGlobeIconIndex globe_icon_index;
    QVector<GlobeIconAnchor> globe_icon_anchors;
    // Screen-space hit rectangles follow the actual far-to-near painter order.
    // Rebuilt on every globe frame; never cache a frame-local projector.
    QVector<GlobeIconHitArea> globe_icon_hit_areas;
    quint64 globe_icons_revision = 0;
    qsizetype globe_icons_node_count = -1;
    qsizetype globe_icons_link_count = -1;
    const NetworkRenderSnapshot *globe_icons_snapshot = nullptr;
    void rectangleSelection();
    void staticDetails(bool include_moving);
    void movingNetwork();
    void selectedPipes();
    void pipePlacement();
    void deviceLinkPlacement();
    void selectedMarkersAndDeviceLinks();
    void simulationErrors();
    void floatingPlacement();
    void demandPointAttachments();
    // Start, centre and end of a device link on screen.
    bool deviceLinkScreen(int link_index, MapBgfxHudPoint *start, MapBgfxHudPoint *center,
                          MapBgfxHudPoint *end) const;
    const NetworkRenderNode *nodeByUuid(const QUuid &uuid) const;

    IconProvider icon_provider;
    Geometry geometry;

    // Per-build state.
    MapBgfxHudCanvas *canvas = nullptr;
    MapBgfxHudTextCache *cache = nullptr;
    const Inputs *inputs = nullptr;
    double offset_x = 0.0;
    double offset_y = 0.0;
    QSet<QUuid> selected_markers;
    QSet<QUuid> selected_pipes;
    QSet<QUuid> moving_markers;
    QSet<QUuid> moving_links;
};

#endif // MAP_BGFX_EDITOR_OVERLAY_H
