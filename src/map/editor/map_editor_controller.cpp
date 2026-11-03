#include "map/editor/map_editor_controller.h"

#include "map/editor/map_canvas_entities.h"
#include "map/core/map_model.h"

#include "geo/geo_web_mercator.h"
#include "map/bgfx/map_bgfx_2d_imagery_layout.h"

#include <QLineF>
#ifdef Q_OS_WASM
#include <QTimer>
#endif

#include <algorithm>
#include <cmath>

MapEditorController::MapEditorController(MapModel *map_model, MapCanvasEntities *map_canvas_entities,
                                         QObject *parent)
    : QObject(parent), map_model(map_model), map_canvas_entities(map_canvas_entities)
{
    if (this->map_canvas_entities)
    {
        connect(this->map_canvas_entities, &MapCanvasEntities::signalEntityMarkerSelected,
                this, &MapEditorController::signalEntitySelectionChanged);
    }

    // An interaction never spans a view switch: the old camera's projection
    // cannot safely drive the new one. 3D may preview a tool, but must not
    // commit network changes until terrain-safe editing is implemented.
    if (this->map_model)
    {
        connect(this->map_model, &MapModel::viewModeChanged, this, [this](MapViewMode view_mode)
        {
            if (view_mode == MapViewMode::Globe)
                stopEntityPositioning();
            if (this->rectangle_dragging)
            {
                this->rectangle_dragging = false;
#ifdef Q_OS_WASM
                this->rectangle_selection_timer->stop();
                this->rectangle_selection_update_pending = false;
#endif
            }
            this->cursor_shape.reset();
            emit signalStateChanged();
        });
    }

#ifdef Q_OS_WASM
    this->rectangle_selection_timer = new QTimer(this);
    this->rectangle_selection_timer->setInterval(16);
    this->rectangle_selection_timer->setSingleShot(true);
    connect(this->rectangle_selection_timer, &QTimer::timeout, this, [this]
    {
        if (!this->rectangle_selection_update_pending)
            return;

        applyPendingRectangleSelection();
        this->rectangle_selection_timer->start();
    });
#endif
}

void MapEditorController::setGlobeCoordinateResolver(ScreenCoordinateResolver resolver)
{
    this->globe_coordinate_resolver = std::move(resolver);
}

void MapEditorController::setTileSelectionKind(MapEditorTileSelectionKind kind)
{
    if (this->tile_selection_kind == kind)
        return;
    this->tile_selection_kind = kind;
    ++this->tile_selection_revision;
    emit signalStateChanged();
}

MapEditorTileSelectionKind MapEditorController::tileSelectionKind() const
{
    return this->tile_selection_kind;
}

void MapEditorController::setTerrainZoomRange(int minimum_zoom, int maximum_zoom)
{
    if (this->terrain_minimum_zoom == minimum_zoom && this->terrain_maximum_zoom == maximum_zoom)
        return;
    this->terrain_minimum_zoom = minimum_zoom;
    this->terrain_maximum_zoom = maximum_zoom;
    ++this->tile_selection_revision;
    emit signalStateChanged();
}

int MapEditorController::imageryZoom(const QSize &viewport_size) const
{
    if (!this->map_model)
        return MapModel::MinZoom;
    const double zoom = globeMode()
        ? this->map_model->viewGlobeZoomLevel(viewport_size)
        : this->map_model->view2dContinuousZoom();
    return MapBgfx2dImagery::imageryZoomFor(zoom, MapModel::MinZoom, MapModel::MaxZoom);
}

int MapEditorController::tileSelectionDisplayZoom(const QSize &viewport_size) const
{
    const int zoom = imageryZoom(viewport_size);
    if (this->tile_selection_kind != MapEditorTileSelectionKind::Terrain
        || this->terrain_minimum_zoom > this->terrain_maximum_zoom)
    {
        return zoom;
    }
    // The terrain tiles drawn around this view come from this range: finer
    // imagery subdivides the dataset's finest tile, coarser has none.
    return std::clamp(zoom, this->terrain_minimum_zoom, this->terrain_maximum_zoom);
}

void MapEditorController::startEntityPositioning(InfrastructureEntity tool)
{
    if (this->map_canvas_entities)
        this->map_canvas_entities->startEntityPositioning(tool);
}

void MapEditorController::stopEntityPositioning()
{
    if (this->map_canvas_entities)
        this->map_canvas_entities->stopEntityPositioning();
}

void MapEditorController::deleteSelectedEntities()
{
    // Preview-only globe interaction must never mutate the hydraulic model.
    if (!globeMode() && this->map_canvas_entities)
        this->map_canvas_entities->onMarkerSelectedDeleteRequested();
}

void MapEditorController::startRectangleSelection(bool oneshot, bool interact_with_entities)
{
    this->is_rectangle_selection_oneshot = oneshot;
    this->rectangle_selection_interacts_with_entities = interact_with_entities;
    this->rectangle_selection_active = true;
    this->rectangle_dragging = false;
#ifdef Q_OS_WASM
    this->rectangle_selection_timer->stop();
    this->rectangle_selection_update_pending = false;
#endif

    if (this->rectangle_selection_interacts_with_entities)
        clearTileSelectionOverlayState();

    this->rectangle_start_wgs84 = CoordinateWGS84();
    this->rectangle_current_wgs84 = CoordinateWGS84();
    this->cursor_shape.reset();

    emit signalStateChanged();
    emit signalFocusRequested();
}

void MapEditorController::cancelRectangleSelection()
{
    if (!this->rectangle_selection_active)
        return;

    if (this->is_rectangle_selection_oneshot)
        this->rectangle_selection_active = false;

    this->rectangle_dragging = false;
#ifdef Q_OS_WASM
    this->rectangle_selection_timer->stop();
    this->rectangle_selection_update_pending = false;
#endif
    if (!this->rectangle_selection_interacts_with_entities)
        clearTileSelectionOverlayState();

    this->cursor_shape.reset();
    emit signalStateChanged();
    emit signalRectangleSelectionCanceled();
}

void MapEditorController::clearTileSelectionOverlay()
{
    if (!clearTileSelectionOverlayState())
        return;

    emit signalStateChanged();
}

bool MapEditorController::rectangleSelectionActive() const
{
    return this->rectangle_selection_active;
}

bool MapEditorController::rectangleDragging() const
{
    return this->rectangle_dragging;
}

QRect MapEditorController::currentSelectionRect(const QSize &viewport_size) const
{
    // Globe drags have no screen rectangle (the live tile selection is the
    // feedback).
    if (!this->map_model || viewport_size.isEmpty() || globeMode())
        return QRect();

    const QPoint rectangle_start_pos = this->map_model->screenFromWgs84(
        this->rectangle_start_wgs84, viewport_size).toPoint();
    const QPoint rectangle_current_pos = this->map_model->screenFromWgs84(
        this->rectangle_current_wgs84, viewport_size).toPoint();
    return QRect(rectangle_start_pos, rectangle_current_pos).normalized();
}

quint64 MapEditorController::tileSelectionRevision() const
{
    return this->tile_selection_revision;
}

const MapEditorController::TileSelectionOverlay &MapEditorController::tileSelectionOverlay() const
{
    return this->tile_selection_overlay;
}

MapEditorController::TileSelectionRange MapEditorController::tileSelectionRange(int zoom) const
{
    TileSelectionRange range;
    range.zoom = zoom;

    if (!this->tile_selection_overlay.visible || zoom < MapModel::MinZoom || zoom > MapModel::MaxZoom)
        return range;

    const double zoom_scale = GeoWebMercator::zoomScale(
        zoom, this->tile_selection_overlay.zoom);
    const double selected_west_tile = this->tile_selection_overlay.tile_x_min * zoom_scale;
    const double selected_east_tile = (this->tile_selection_overlay.tile_x_max + 1.0) * zoom_scale;
    const double selected_north_tile = this->tile_selection_overlay.tile_y_min * zoom_scale;
    const double selected_south_tile = (this->tile_selection_overlay.tile_y_max + 1.0) * zoom_scale;
    const int tile_count = 1 << zoom;

    range.tile_x_min = int(std::floor(selected_west_tile));
    range.tile_x_max = int(std::ceil(selected_east_tile)) - 1;
    range.tile_y_min = qBound(0, int(std::floor(selected_north_tile)), tile_count - 1);
    range.tile_y_max = qBound(0, int(std::ceil(selected_south_tile)) - 1, tile_count - 1);
    range.valid = range.tile_x_min <= range.tile_x_max && range.tile_y_min <= range.tile_y_max;
    return range;
}

std::optional<Qt::CursorShape> MapEditorController::cursorShape() const
{
    return this->cursor_shape;
}

bool MapEditorController::keyPress(Qt::Key key)
{
    if (key != Qt::Key_Escape)
        return false;

    if (this->map_canvas_entities && this->map_canvas_entities->cancelActiveMove())
        return true;

    if (!this->rectangle_selection_active)
        return false;

    cancelRectangleSelection();
    return true;
}

bool MapEditorController::mousePress(const QPointF &position, const QPoint &global_position,
                                     Qt::MouseButton button, const QSize &viewport_size)
{
    if (!this->map_model || !this->map_canvas_entities || viewport_size.isEmpty())
        return false;
    const bool entity_interaction_enabled = !this->rectangle_selection_active ||
                                            this->rectangle_selection_interacts_with_entities;

    if (entity_interaction_enabled && button == Qt::LeftButton && !this->rectangle_dragging &&
        this->map_canvas_entities->selectEntityAt(position))
    {
        emit signalStateChanged();
        return true;
    }

    if (globeMode())
        return globeMousePress(position, button, viewport_size);

    if (button != Qt::RightButton)
        return false;

    if (this->rectangle_selection_active && !this->rectangle_selection_interacts_with_entities)
    {
        clearTileSelectionOverlayState();
        beginRectangleDrag(position, viewport_size);
        return true;
    }

    if (this->map_canvas_entities->handleDemandPointAttachmentRightClick(
            position, global_position))
    {
        emit signalStateChanged();
        return true;
    }

    if (this->map_canvas_entities->anchorMarker(position))
    {
        emit signalStateChanged();
        return true;
    }

    if (this->map_canvas_entities->showMarkerContextMenuAt(position, global_position) ||
        this->map_canvas_entities->showPipeContextMenuAt(position, global_position))
    {
        emit signalStateChanged();
        return true;
    }

    if (!this->rectangle_selection_active)
        return false;

    beginRectangleDrag(position, viewport_size);
    return true;
}

bool MapEditorController::mouseMove(const QPointF &position, const QSize &viewport_size,
                                    bool allow_entity_interaction)
{
    if (!this->map_model || !this->map_canvas_entities || viewport_size.isEmpty())
        return false;
    if (globeMode())
    {
        if (this->rectangle_dragging)
            return globeMouseMove(position, viewport_size);
        // Use the EXACT 2D placement state machine for a live globe preview.
        // Creating only: floatEntity() in move mode changes model geometry.
        // Link-start/bend choices remain previews; terrain-aware nodes are
        // committed by the shared right-click creation handler.
        if (this->map_canvas_entities->creatingPreviewActive()
            && (!this->rectangle_selection_active
                || this->rectangle_selection_interacts_with_entities))
        {
            setCursorShape(Qt::CrossCursor);
            return this->map_canvas_entities->floatEntity(position);
        }
        if (allow_entity_interaction &&
            (!this->rectangle_selection_active || this->rectangle_selection_interacts_with_entities)
            && this->map_canvas_entities->isEntityAt(position))
            setCursorShape(Qt::PointingHandCursor);
        else
            setCursorShape(std::nullopt);
        return false;
    }

    if (this->rectangle_selection_active && this->rectangle_dragging)
    {
        this->rectangle_current_wgs84 = this->map_model->wgs84FromScreen(
            position.toPoint(), viewport_size);

        const QRect selected_rect = currentSelectionRect(viewport_size);
        if (this->rectangle_selection_interacts_with_entities)
        {
#ifdef Q_OS_WASM
            scheduleRectangleSelectionUpdate(selected_rect);
#else
            this->map_canvas_entities->onRectangleSelect(selected_rect, RectangleSelectMode::Replace);
#endif
        }
        else
        {
            updateTileSelectionOverlay(selected_rect, viewport_size);
        }

        this->cursor_shape = Qt::CrossCursor;
        emit signalStateChanged();
        return true;
    }

    if (!allow_entity_interaction)
    {
        if (this->map_canvas_entities->positioningActive())
            return this->map_canvas_entities->floatEntity(position);
        return false;
    }

    const bool entity_interaction_enabled = !this->rectangle_selection_active ||
                                            this->rectangle_selection_interacts_with_entities;
    if (!entity_interaction_enabled)
    {
        setCursorShape(std::nullopt);
        return false;
    }

    const bool handled = this->map_canvas_entities->floatEntity(position);
    if (this->map_canvas_entities->isEntityAt(position))
    {
        setCursorShape(Qt::PointingHandCursor);
    }
    else
    {
        setCursorShape(std::nullopt);
    }

    return handled;
}

bool MapEditorController::mouseRelease(const QPointF &position, Qt::MouseButton button,
                                       const QSize &viewport_size)
{
    if (this->map_model && this->map_canvas_entities && !viewport_size.isEmpty() && globeMode())
        return globeMouseRelease(position, button, viewport_size);
    if (!this->map_model || !this->map_canvas_entities || viewport_size.isEmpty() ||
        button != Qt::RightButton || !this->rectangle_selection_active ||
        !this->rectangle_dragging)
    {
        return false;
    }

    this->rectangle_current_wgs84 = this->map_model->wgs84FromScreen(
        position.toPoint(), viewport_size);
    const QRect selected_rect = currentSelectionRect(viewport_size);

    if (this->is_rectangle_selection_oneshot)
        this->rectangle_selection_active = false;

    this->rectangle_dragging = false;
    this->cursor_shape.reset();
#ifdef Q_OS_WASM
    this->rectangle_selection_timer->stop();
    this->rectangle_selection_update_pending = false;
#endif

    if (selected_rect.width() > 0 && selected_rect.height() > 0)
    {
        if (this->rectangle_selection_interacts_with_entities)
        {
            this->map_canvas_entities->onRectangleSelect(selected_rect, RectangleSelectMode::Replace);
            emit signalRectangleSelected(selectionRectWgs84(selected_rect, viewport_size));
        }
        else
        {
            updateTileSelectionOverlay(selected_rect, viewport_size);
            emit signalRectangleSelected(tileSelectionRectWgs84());
        }
    }
    else
    {
        emit signalRectangleSelectionCanceled();
    }

    emit signalStateChanged();
    return true;
}

void MapEditorController::beginRectangleDrag(const QPointF &position, const QSize &viewport_size)
{
    this->rectangle_start_wgs84 = this->map_model->wgs84FromScreen(
        position.toPoint(), viewport_size);
    this->rectangle_current_wgs84 = this->rectangle_start_wgs84;
    this->rectangle_dragging = true;
    this->cursor_shape = Qt::CrossCursor;
    emit signalStateChanged();
}

#ifdef Q_OS_WASM
void MapEditorController::scheduleRectangleSelectionUpdate(const QRect &selected_rect)
{
    this->pending_rectangle_selection_rect = selected_rect;
    this->rectangle_selection_update_pending = true;
    if (this->rectangle_selection_timer->isActive())
        return;

    applyPendingRectangleSelection();
    this->rectangle_selection_timer->start();
}

void MapEditorController::applyPendingRectangleSelection()
{
    this->rectangle_selection_update_pending = false;
    if (!this->rectangle_selection_active || !this->rectangle_dragging ||
        !this->rectangle_selection_interacts_with_entities)
    {
        return;
    }

    this->map_canvas_entities->onRectangleSelect(
        this->pending_rectangle_selection_rect, RectangleSelectMode::Replace);
}
#endif

void MapEditorController::setCursorShape(std::optional<Qt::CursorShape> cursor_shape)
{
    if (this->cursor_shape == cursor_shape)
        return;

    this->cursor_shape = cursor_shape;
    emit signalStateChanged();
}

bool MapEditorController::globeMode() const
{
    return this->map_model && this->map_model->viewMode() == MapViewMode::Globe;
}

bool MapEditorController::globeCoordinateAt(const QPointF &position, const QSize &viewport_size,
                                            CoordinateWGS84 *coordinate) const
{
    if (this->globe_coordinate_resolver)
        return this->globe_coordinate_resolver(position, coordinate);
    return this->map_model->globeCoordinateAtScreen(position.toPoint(), viewport_size, coordinate);
}

bool MapEditorController::globeMousePress(const QPointF &position, Qt::MouseButton button,
                                          const QSize &viewport_size)
{
    if (button == Qt::RightButton && this->map_canvas_entities
        && this->map_canvas_entities->creatingPreviewActive()
        && (!this->rectangle_selection_active || this->rectangle_selection_interacts_with_entities))
    {
        // Node placement now commits through the SAME shared editor path as
        // 2D, but only if the globe DEM resolver supplies a valid elevation.
        // Pipe/device previews are still read-only in this increment.
        this->map_canvas_entities->floatEntity(position);
        return this->map_canvas_entities->anchorMarker(
            position, this->map_canvas_entities->creatingGlobeNode());
    }
    // Only the cache sections' tile selection; everything else (left drag
    // pans, middle / Ctrl orbits) belongs to the globe navigation.
    if (button != Qt::RightButton || !this->rectangle_selection_active
        || this->rectangle_selection_interacts_with_entities)
    {
        return false;
    }

    CoordinateWGS84 coordinate;
    if (!globeCoordinateAt(position, viewport_size, &coordinate))
        return false;

    clearTileSelectionOverlayState();
    this->rectangle_start_wgs84 = coordinate;
    this->rectangle_current_wgs84 = coordinate;
    this->rectangle_dragging = true;
    this->globe_drag_press_position = position;
    this->globe_drag_moved = false;
    this->cursor_shape = Qt::CrossCursor;
    emit signalStateChanged();
    return true;
}

bool MapEditorController::globeMouseMove(const QPointF &position, const QSize &viewport_size)
{
    if (!this->rectangle_selection_active || !this->rectangle_dragging)
    {
        // Globe tile-selection drags are separate from editor previews.
        setCursorShape(std::nullopt);
        return false;
    }

    if (QLineF(position, this->globe_drag_press_position).length() > 3.0)
        this->globe_drag_moved = true;
    // Over space (past the globe's edge) the last corner on the globe holds.
    CoordinateWGS84 coordinate;
    if (globeCoordinateAt(position, viewport_size, &coordinate))
        this->rectangle_current_wgs84 = coordinate;
    if (this->globe_drag_moved)
        updateTileSelectionOverlayFromCorners(viewport_size);

    this->cursor_shape = Qt::CrossCursor;
    emit signalStateChanged();
    return true;
}

bool MapEditorController::globeMouseRelease(const QPointF &position, Qt::MouseButton button,
                                            const QSize &viewport_size)
{
    if (button != Qt::RightButton || !this->rectangle_selection_active || !this->rectangle_dragging)
        return false;

    CoordinateWGS84 coordinate;
    if (globeCoordinateAt(position, viewport_size, &coordinate))
        this->rectangle_current_wgs84 = coordinate;
    if (QLineF(position, this->globe_drag_press_position).length() > 3.0)
        this->globe_drag_moved = true;

    if (this->is_rectangle_selection_oneshot)
        this->rectangle_selection_active = false;
    this->rectangle_dragging = false;
    this->cursor_shape.reset();

    if (this->globe_drag_moved)
        updateTileSelectionOverlayFromCorners(viewport_size);
    if (this->globe_drag_moved && this->tile_selection_overlay.visible)
    {
        emit signalRectangleSelected(tileSelectionRectWgs84());
    }
    else
    {
        clearTileSelectionOverlayState();
        emit signalRectangleSelectionCanceled();
    }

    emit signalStateChanged();
    return true;
}

void MapEditorController::updateTileSelectionOverlayFromCorners(const QSize &viewport_size)
{
    // The tiles of the imagery level the globe draws around the view now,
    // like the 2D selection's (the stored range scales to any level,
    // tileSelectionRange()).
    const int zoom = imageryZoom(viewport_size);
    const int tile_count = 1 << zoom;
    const double first_x = GeoWebMercator::lonToTileX(
        GeoWebMercator::normalizeLongitude(this->rectangle_start_wgs84.longitude_deg), zoom);
    double second_x = GeoWebMercator::lonToTileX(
        GeoWebMercator::normalizeLongitude(this->rectangle_current_wgs84.longitude_deg), zoom);
    // The shorter way round: a drag across the antimeridian selects the
    // tiles on both sides of it, not the rest of the world (x may leave
    // 0..tile_count-1, as in 2D).
    const double half_world = double(tile_count) / 2.0;
    if (second_x - first_x > half_world)
        second_x -= double(tile_count);
    else if (first_x - second_x > half_world)
        second_x += double(tile_count);
    const double first_y = GeoWebMercator::latToTileY(this->rectangle_start_wgs84.latitude_deg, zoom);
    const double second_y = GeoWebMercator::latToTileY(this->rectangle_current_wgs84.latitude_deg, zoom);

    const int tile_y_min = int(std::floor(qMin(first_y, second_y)));
    const int tile_y_max = int(std::floor(qMax(first_y, second_y)));
    if (tile_y_max < 0 || tile_y_min >= tile_count)
    {
        clearTileSelectionOverlayState();
        return;
    }

    ++this->tile_selection_revision;
    this->tile_selection_overlay.zoom = zoom;
    this->tile_selection_overlay.tile_x_min = int(std::floor(qMin(first_x, second_x)));
    this->tile_selection_overlay.tile_x_max = int(std::floor(qMax(first_x, second_x)));
    this->tile_selection_overlay.tile_y_min = qBound(0, tile_y_min, tile_count - 1);
    this->tile_selection_overlay.tile_y_max = qBound(0, tile_y_max, tile_count - 1);
    this->tile_selection_overlay.visible =
        this->tile_selection_overlay.tile_x_min <= this->tile_selection_overlay.tile_x_max &&
        this->tile_selection_overlay.tile_y_min <= this->tile_selection_overlay.tile_y_max;
}

bool MapEditorController::clearTileSelectionOverlayState()
{
    if (!this->tile_selection_overlay.visible)
        return false;

    this->tile_selection_overlay = TileSelectionOverlay();
    ++this->tile_selection_revision;
    return true;
}

CoordinateWGS84Rect MapEditorController::selectionRectWgs84(
    const QRect &selected_rect, const QSize &viewport_size) const
{
    CoordinateWGS84Rect rect;
    rect.north_west = this->map_model->wgs84FromScreen(selected_rect.topLeft(), viewport_size);
    rect.south_east = this->map_model->wgs84FromScreen(selected_rect.bottomRight(), viewport_size);
    return rect;
}

void MapEditorController::updateTileSelectionOverlay(const QRect &selected_rect,
                                                      const QSize &viewport_size)
{
    if (selected_rect.isEmpty())
    {
        clearTileSelectionOverlayState();
        return;
    }

    // The tiles of the imagery level the map draws now (the cache holds
    // tiles, the camera zoom is continuous).
    const double camera_zoom = this->map_model->view2dContinuousZoom();
    const int zoom = MapBgfx2dImagery::imageryZoomFor(camera_zoom, MapModel::MinZoom, MapModel::MaxZoom);
    const int tile_count = 1 << zoom;
    const QPointF center_tile(GeoWebMercator::lonToTileX(this->map_model->centerLon(), zoom),
                              GeoWebMercator::latToTileY(this->map_model->centerLat(), zoom));
    // Screen pixels per tile of that level at the continuous camera zoom.
    const double rendered_tile_size = MapModel::TileSize * std::exp2(camera_zoom - double(zoom));
    const double tile_x_first = center_tile.x() +
        (selected_rect.left() - viewport_size.width() / 2.0) / rendered_tile_size;
    const double tile_x_second = center_tile.x() +
        (selected_rect.right() - viewport_size.width() / 2.0) / rendered_tile_size;
    const double tile_y_first = center_tile.y() +
        (selected_rect.top() - viewport_size.height() / 2.0) / rendered_tile_size;
    const double tile_y_second = center_tile.y() +
        (selected_rect.bottom() - viewport_size.height() / 2.0) / rendered_tile_size;

    const int tile_y_min = int(std::floor(qMin(tile_y_first, tile_y_second)));
    const int tile_y_max = int(std::floor(qMax(tile_y_first, tile_y_second)));
    if (tile_y_max < 0 || tile_y_min >= tile_count)
    {
        clearTileSelectionOverlayState();
        return;
    }

    ++this->tile_selection_revision;
    this->tile_selection_overlay.zoom = zoom;
    this->tile_selection_overlay.tile_x_min = int(std::floor(qMin(tile_x_first, tile_x_second)));
    this->tile_selection_overlay.tile_x_max = int(std::floor(qMax(tile_x_first, tile_x_second)));
    this->tile_selection_overlay.tile_y_min = qBound(0, tile_y_min, tile_count - 1);
    this->tile_selection_overlay.tile_y_max = qBound(0, tile_y_max, tile_count - 1);
    this->tile_selection_overlay.visible =
        this->tile_selection_overlay.tile_x_min <= this->tile_selection_overlay.tile_x_max &&
        this->tile_selection_overlay.tile_y_min <= this->tile_selection_overlay.tile_y_max;
}

CoordinateWGS84Rect MapEditorController::tileSelectionRectWgs84() const
{
    CoordinateWGS84Rect rect;
    rect.north_west.latitude_deg = GeoWebMercator::tileYToLat(
        this->tile_selection_overlay.tile_y_min, this->tile_selection_overlay.zoom);
    rect.north_west.longitude_deg = GeoWebMercator::normalizeLongitude(
        GeoWebMercator::tileXToLon(
            this->tile_selection_overlay.tile_x_min, this->tile_selection_overlay.zoom));
    rect.south_east.latitude_deg = GeoWebMercator::tileYToLat(
        this->tile_selection_overlay.tile_y_max + 1.0, this->tile_selection_overlay.zoom);
    rect.south_east.longitude_deg = GeoWebMercator::normalizeLongitude(
        GeoWebMercator::tileXToLon(
            this->tile_selection_overlay.tile_x_max + 1.0, this->tile_selection_overlay.zoom));
    return rect;
}
