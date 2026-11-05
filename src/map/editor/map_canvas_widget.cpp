#include "map/editor/map_canvas_widget.h"
#include "map/core/map_input_focus_policy.h"

#include "map/editor/map_editor_controller.h"
#include "map/bgfx/map_bgfx_2d_imagery_layout.h"

#include <QFocusEvent>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QShowEvent>
#include <QWheelEvent>

MapCanvasWidget::MapCanvasWidget(MapModel *map_model, MapWidget *map,
                                 HydraulicData *hydraulic_data, QWidget *parent)
    : QWidget(parent), map_model(map_model), map(map), hydraulic_data(hydraulic_data),
      map_canvas_entities(new MapCanvasEntities(map_model, hydraulic_data, this))
{
    // Input only: no background, no painting (paintEvent() is QWidget's).
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_NoSystemBackground);
    setMouseTracking(true);
    connect(qApp, &QGuiApplication::applicationStateChanged, this,
            [this](Qt::ApplicationState state)
    {
        if (mapApplicationInputFocusLost(state))
            cancelActiveInput();
    });

    // Camera motion (every frame while zooming or panning): the pointer now
    // lies over another map position, so the editor re-evaluates it - only
    // while the editor is shown (synced map movement moves this camera with
    // the monitor's, and hover hit testing there is wasted work). The visual
    // state itself is camera independent except for the pointer's map
    // coordinate: signalCameraChanged(), not a full render update.
    connect(this->map_model, &MapModel::view2dContinuousZoomChanged, this, [this](double)
    {
        if ((isVisible() || this->shared_editor_active) && this->editor_controller
            && this->last_pointer_position_valid)
            this->editor_controller->mouseMove(this->last_pointer_position, size(), false);
        emit signalCameraChanged();
    });
    connect(this->map_model, &MapModel::centerChangedWGS84, this, [this]
    {
        if ((isVisible() || this->shared_editor_active) && this->editor_controller
            && this->last_pointer_position_valid)
            this->editor_controller->mouseMove(this->last_pointer_position, size(), false);
        emit signalCameraChanged();
    });
    // Globe (3D) camera: a tile-selection drag follows the ground now under
    // the pointer while the view moves. The globe overlay is built every
    // globe frame, so no render update is needed beyond that.
    connect(this->map_model, &MapModel::viewGlobeCameraChanged, this, [this]
    {
        if (this->map_model->viewMode() != MapViewMode::Globe)
            return;
        if ((isVisible() || this->shared_editor_active) && this->editor_controller
            && this->last_pointer_position_valid)
            this->editor_controller->mouseMove(this->last_pointer_position, size(), false);
        emit signalCameraChanged();
    });
    if (this->hydraulic_data)
    {
        connect(this->hydraulic_data, &HydraulicData::signalNetworkLoaded, this, [this]
        {
            requestRenderUpdate();
        });
        connect(this->hydraulic_data, &HydraulicData::signalNetworkGeometryChanged,
                this, [this](quint64)
        {
            requestRenderUpdate();
        });
    }
}

void MapCanvasWidget::setSharedEditorActive(bool active)
{
    this->shared_editor_active = active;
    requestRenderUpdate();
}

void MapCanvasWidget::setEditorController(MapEditorController *editor_controller)
{
    if (this->editor_controller == editor_controller)
        return;

    if (this->editor_controller)
        disconnect(this->editor_controller, nullptr, this, nullptr);

    this->editor_controller = editor_controller;
    if (this->editor_controller)
    {
        connect(this->editor_controller, &MapEditorController::signalStateChanged,
                this, &MapCanvasWidget::applyControllerState);
        connect(this->editor_controller, &MapEditorController::signalFocusRequested,
                this, [this]
        {
            setFocus(Qt::OtherFocusReason);
        });
    }

    applyControllerState();
}

MapCanvasEntities *MapCanvasWidget::mapCanvasEntities() const
{
    return this->map_canvas_entities;
}

int MapCanvasWidget::backgroundOpacity() const
{
    return this->map_background_opacity;
}

MapEditorVisualState MapCanvasWidget::visualState() const
{
    return this->map_canvas_entities->visualState();
}

void MapCanvasWidget::setBackgroundOpacity(int opacity)
{
    opacity = qBound(0, opacity, 100);
    if (this->map_background_opacity == opacity)
        return;

    this->map_background_opacity = opacity;
    requestRenderUpdate();
}

void MapCanvasWidget::setIconSizePercent(int size_percent)
{
    this->map_canvas_entities->setIconSizePercent(size_percent);
}

void MapCanvasWidget::applyControllerState()
{
    if (!this->editor_controller)
    {
        unsetCursor();
        requestRenderUpdate();
        return;
    }

    const std::optional<Qt::CursorShape> cursor_shape = this->editor_controller->cursorShape();
    if (cursor_shape.has_value())
        setCursor(cursor_shape.value());
    else
        unsetCursor();

    requestRenderUpdate();
}

void MapCanvasWidget::requestRenderUpdate()
{
    emit signalRenderUpdateRequested();
}

MapEditorViewportRenderState MapCanvasWidget::viewportRenderState() const
{
    MapEditorViewportRenderState state;
    state.background_opacity = this->map_background_opacity;

    if (!this->editor_controller)
        return state;

    const MapEditorController::TileSelectionOverlay &overlay =
        this->editor_controller->tileSelectionOverlay();
    state.tile_selection_kind = this->editor_controller->tileSelectionKind();
    if (overlay.visible)
    {
        // The selection in tiles of the level drawn now (2D or 3D; terrain:
        // within the terrain dataset's range).
        const int display_zoom = this->editor_controller->tileSelectionDisplayZoom(size());
        const MapEditorController::TileSelectionRange range =
            this->editor_controller->tileSelectionRange(display_zoom);
        if (range.valid)
        {
            state.tile_selection_visible = true;
            state.tile_selection_zoom = display_zoom;
            state.tile_x_min = range.tile_x_min;
            state.tile_x_max = range.tile_x_max;
            state.tile_y_min = range.tile_y_min;
            state.tile_y_max = range.tile_y_max;
        }
    }

    // The Select tool uses the same screen-space rubber band in 2D and 3D.
    // Cache-tile selection on the globe still uses geographic tile outlines.
    state.rectangle_selection_visible =
        this->editor_controller->rectangleSelectionActive() &&
        this->editor_controller->rectangleDragging() &&
        !this->editor_controller->currentSelectionRect(size()).isEmpty();
    if (state.rectangle_selection_visible)
    {
        state.rectangle_selection = this->editor_controller->currentSelectionRect(size());
        if (!state.rectangle_selection.isEmpty() &&
            this->map_model->viewMode() != MapViewMode::Globe)
        {
            state.rectangle_selection_north_west = this->map_model->wgs84FromScreen(
                state.rectangle_selection.topLeft(), size());
            state.rectangle_selection_south_east = this->map_model->wgs84FromScreen(
                state.rectangle_selection.bottomRight(), size());
            state.rectangle_selection_wgs84_valid = true;
        }
    }

    return state;
}

void MapCanvasWidget::keyPressEvent(QKeyEvent *event)
{
    if (this->editor_controller && this->editor_controller->keyPress(Qt::Key(event->key())))
    {
        event->accept();
        return;
    }

    if (this->map->handleKeyPressEvent(event))
        return;

    QWidget::keyPressEvent(event);
}

void MapCanvasWidget::keyReleaseEvent(QKeyEvent *event)
{
    if (this->map->handleKeyReleaseEvent(event))
        return;

    QWidget::keyReleaseEvent(event);
}

void MapCanvasWidget::cancelActiveInput()
{
    this->map->cancelNavigationInput();
    // The editor may have captured a press without receiving its release.
    // Cancel only in-progress drags; do not discard an armed placement tool
    // or a completed selection merely because the user alt-tabbed.
    if (this->map_canvas_entities != nullptr)
        this->map_canvas_entities->cancelActiveMove();
    if (this->editor_controller && this->editor_controller->rectangleDragging())
        this->editor_controller->cancelRectangleSelection();
    this->last_pointer_position_valid = false;
}

void MapCanvasWidget::focusOutEvent(QFocusEvent *event)
{
    cancelActiveInput();
    QWidget::focusOutEvent(event);
}

void MapCanvasWidget::mousePressEvent(QMouseEvent *event)
{
    this->last_pointer_position = event->position();
    this->last_pointer_position_valid = true;

    if (this->editor_controller &&
        this->editor_controller->mousePress(event->position(), event->globalPosition().toPoint(),
                                            event->button(), size()))
    {
        event->accept();
        return;
    }

    if (this->map->handleMousePressEvent(event))
        return;

    QWidget::mousePressEvent(event);
}

void MapCanvasWidget::mouseMoveEvent(QMouseEvent *event)
{
    this->last_pointer_position = event->position();
    this->last_pointer_position_valid = true;

    const bool map_handled_event = this->map->handleMouseMoveEvent(event);
    const bool editor_handled_event = this->editor_controller &&
        this->editor_controller->mouseMove(event->position(), size(), !map_handled_event);

    if (editor_handled_event)
    {
        event->accept();
        return;
    }

    if (map_handled_event)
        return;

    QWidget::mouseMoveEvent(event);
}

void MapCanvasWidget::mouseReleaseEvent(QMouseEvent *event)
{
    this->last_pointer_position = event->position();
    this->last_pointer_position_valid = true;

    if (this->editor_controller &&
        this->editor_controller->mouseRelease(event->position(), event->button(), size()))
    {
        event->accept();
        return;
    }

    if (this->map->handleMouseReleaseEvent(event))
    {
        if (this->editor_controller)
            this->editor_controller->mouseMove(event->position(), size(), false);
        return;
    }

    QWidget::mouseReleaseEvent(event);
}

void MapCanvasWidget::wheelEvent(QWheelEvent *event)
{
    this->map->handleWheelEvent(event);
}

void MapCanvasWidget::resizeEvent(QResizeEvent *event)
{
    this->map_canvas_entities->positionMarkers();
    QWidget::resizeEvent(event);
}

void MapCanvasWidget::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    requestRenderUpdate();
}
