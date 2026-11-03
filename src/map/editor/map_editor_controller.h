#ifndef MAP_EDITOR_CONTROLLER_H
#define MAP_EDITOR_CONTROLLER_H

#include <QObject>
#include <QPointF>
#include <QRect>
#include <QSize>

#include <functional>
#include <optional>

#include "common/_enums_structs.h"
#include "map/core/map_models.h"
#include "map/editor/map_editor_visual_state.h"

class MapCanvasEntities;
class MapModel;
class QTimer;

class MapEditorController : public QObject
{
    Q_OBJECT

public:
    struct TileSelectionRange
    {
        bool valid = false;
        int zoom = 0;
        int tile_x_min = 0;
        int tile_x_max = -1;
        int tile_y_min = 0;
        int tile_y_max = -1;
    };

    struct TileSelectionOverlay
    {
        bool visible = false;
        int zoom = 0;
        int tile_x_min = 0;
        int tile_x_max = -1;
        int tile_y_min = 0;
        int tile_y_max = -1;
    };

    // The coordinate on the drawn surface under a screen position (the bgfx
    // surface's terrain-aware pick in Globe mode); false over space.
    using ScreenCoordinateResolver =
        std::function<bool(const QPointF &screen_position, CoordinateWGS84 *coordinate)>;

    explicit MapEditorController(MapModel *map_model, MapCanvasEntities *map_canvas_entities,
                                 QObject *parent = nullptr);

    // Globe mode shares MapCanvasEntities selection and hover with 2D.
    // Right-drag cache tile selection remains globe-specific; hydraulic
    // creation and movement are not enabled until elevation-safe editing.
    // Without a terrain resolver the ellipsoid is picked.
    void setGlobeCoordinateResolver(ScreenCoordinateResolver resolver);
    // What the tile selection is for (Tile Cache: map tiles, Terrain Cache:
    // terrain tiles); the overlay draws it green or brown, and terrain
    // selections are shown at a level of the terrain dataset's range.
    void setTileSelectionKind(MapEditorTileSelectionKind kind);
    MapEditorTileSelectionKind tileSelectionKind() const;
    // The terrain dataset's normalized zoom range (MapTerrainDatasetInfo);
    // invalid (minimum > maximum) while no dataset is known.
    void setTerrainZoomRange(int minimum_zoom, int maximum_zoom);
    // The imagery tile level the map draws now (2D: the continuous zoom
    // rounded; 3D: the camera distance's 2D-equivalent level), and the
    // level the selection is shown at (terrain: clamped to its range).
    int imageryZoom(const QSize &viewport_size) const;
    int tileSelectionDisplayZoom(const QSize &viewport_size) const;

    void startEntityPositioning(InfrastructureEntity tool);
    void stopEntityPositioning();
    void deleteSelectedEntities();

    void startRectangleSelection(bool oneshot, bool interact_with_entities = true);
    void cancelRectangleSelection();
    void clearTileSelectionOverlay();

    bool rectangleSelectionActive() const;
    bool rectangleDragging() const;
    QRect currentSelectionRect(const QSize &viewport_size) const;
    const TileSelectionOverlay &tileSelectionOverlay() const;
    // Changes with every change of what the tile selection shows (range,
    // kind, terrain range): renderers that keep a built overlay compare it.
    quint64 tileSelectionRevision() const;
    TileSelectionRange tileSelectionRange(int zoom) const;
    std::optional<Qt::CursorShape> cursorShape() const;

    bool keyPress(Qt::Key key);
    bool mousePress(const QPointF &position, const QPoint &global_position,
                    Qt::MouseButton button, const QSize &viewport_size);
    bool mouseMove(const QPointF &position, const QSize &viewport_size,
                   bool allow_entity_interaction);
    bool mouseRelease(const QPointF &position, Qt::MouseButton button,
                      const QSize &viewport_size);

signals:
    void signalStateChanged();
    void signalFocusRequested();
    void signalRectangleSelected(const CoordinateWGS84Rect &rect);
    void signalRectangleSelectionCanceled();
    void signalEntitySelectionChanged(bool selected);

private:
    void beginRectangleDrag(const QPointF &position, const QSize &viewport_size);
#ifdef Q_OS_WASM
    void scheduleRectangleSelectionUpdate(const QRect &selected_rect);
    void applyPendingRectangleSelection();
#endif
    void setCursorShape(std::optional<Qt::CursorShape> cursor_shape);
    bool clearTileSelectionOverlayState();
    bool globeMode() const;
    // Globe mode: the coordinate under a screen position (resolver, else the
    // ellipsoid); false over space.
    bool globeCoordinateAt(const QPointF &position, const QSize &viewport_size,
                           CoordinateWGS84 *coordinate) const;
    bool globeMousePress(const QPointF &position, Qt::MouseButton button,
                         const QSize &viewport_size);
    bool globeMouseMove(const QPointF &position, const QSize &viewport_size);
    bool globeMouseRelease(const QPointF &position, Qt::MouseButton button,
                           const QSize &viewport_size);
    // The tiles (at the imagery level drawn now) between the drag's two
    // corner coordinates, the shorter way round the antimeridian.
    void updateTileSelectionOverlayFromCorners(const QSize &viewport_size);
    CoordinateWGS84Rect selectionRectWgs84(const QRect &selected_rect,
                                           const QSize &viewport_size) const;
    CoordinateWGS84Rect tileSelectionRectWgs84() const;
    void updateTileSelectionOverlay(const QRect &selected_rect,
                                    const QSize &viewport_size);

    MapModel *map_model = nullptr;
    MapCanvasEntities *map_canvas_entities = nullptr;

    bool is_rectangle_selection_oneshot = true;
    bool rectangle_selection_active = false;
    bool rectangle_dragging = false;
    bool rectangle_selection_interacts_with_entities = true;
    CoordinateWGS84 rectangle_start_wgs84;
    CoordinateWGS84 rectangle_current_wgs84;
    TileSelectionOverlay tile_selection_overlay;
    quint64 tile_selection_revision = 0;
    MapEditorTileSelectionKind tile_selection_kind = MapEditorTileSelectionKind::MapTiles;
    int terrain_minimum_zoom = 0;
    int terrain_maximum_zoom = -1;
    ScreenCoordinateResolver globe_coordinate_resolver;
    // Globe drag: where it was pressed (a click without motion selects
    // nothing, as an empty 2D rectangle).
    QPointF globe_drag_press_position;
    bool globe_drag_moved = false;
    std::optional<Qt::CursorShape> cursor_shape;
#ifdef Q_OS_WASM
    QTimer *rectangle_selection_timer = nullptr;
    QRect pending_rectangle_selection_rect;
    bool rectangle_selection_update_pending = false;
#endif
};

#endif // MAP_EDITOR_CONTROLLER_H
