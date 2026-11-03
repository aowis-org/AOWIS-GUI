#include "map/editor/map_canvas_placement.h"
#include "map/editor/map_canvas_widget.h"

#include <QApplication>

MapCanvasPlacement::MapCanvasPlacement(MapCanvasWidget *map_canvas, QObject *parent)
    : QObject(parent), map_canvas(map_canvas)
{
}

void MapCanvasPlacement::startCreate(InfrastructureEntity entity,
                                     const QString &pixmap_path,
                                     int width)
{
    stop();
    this->current_entity = entity;
    this->placement_mode = MapEntityPlacementMode::CreateNew;
    this->floating_pixmap_path = pixmap_path;
    this->floating_width = width;
    this->draw_immediately = true;
    prepareFloatingMarker();
}

bool MapCanvasPlacement::rearmCreate(const QString &pixmap_path, int width)
{
    if (this->placement_mode != MapEntityPlacementMode::CreateNew || hasFloatingMarker())
        return false;

    this->floating_pixmap_path = pixmap_path;
    this->floating_width = width;
    prepareFloatingMarker();
    return true;
}

bool MapCanvasPlacement::startMove(InfrastructureEntity entity, const QUuid &uuid,
                                   const QString &pixmap_path, int width,
                                   const QPointF &mouse_position,
                                   const CoordinateWGS84 &mouse_coordinate)
{
    if (uuid.isNull())
        return false;

    stop();
    this->current_entity = entity;
    this->placement_mode = MapEntityPlacementMode::MoveExisting;
    this->floating_uuid = uuid;
    this->floating_pixmap_path = pixmap_path;
    this->floating_width = width;
    this->mouse_position = mouse_position;
    this->mouse_coordinate = mouse_coordinate;
    this->previous_mouse_coordinate = mouse_coordinate;
    this->mouse_coordinate_valid = true;
    this->draw_immediately = true;
    prepareFloatingMarker();
    return true;
}

void MapCanvasPlacement::startVirtualMove(InfrastructureEntity entity,
                                          const QPointF &mouse_position,
                                          const CoordinateWGS84 &mouse_coordinate)
{
    stop();
    this->current_entity = entity;
    this->placement_mode = MapEntityPlacementMode::MoveExisting;
    this->mouse_position = mouse_position;
    this->mouse_coordinate = mouse_coordinate;
    this->previous_mouse_coordinate = mouse_coordinate;
    this->mouse_coordinate_valid = true;
    focusCanvas();
    setMoveCursor(true);
}

void MapCanvasPlacement::stop()
{
    setMoveCursor(false);
    clearConnectionTarget();
    this->moving_selected = false;
    this->floating_uuid = QUuid();
    this->floating_pixmap_path.clear();
    this->floating_width = 0;
    this->floating_visible = false;
    this->placement_mode = MapEntityPlacementMode::None;
    this->current_entity = InfrastructureEntity::Unknown;
    this->mouse_coordinate_valid = false;
}

MapEntityPlacementMode MapCanvasPlacement::mode() const
{
    return this->placement_mode;
}

InfrastructureEntity MapCanvasPlacement::entity() const
{
    return this->current_entity;
}

bool MapCanvasPlacement::isIdle() const
{
    return this->placement_mode == MapEntityPlacementMode::None;
}

bool MapCanvasPlacement::isCreating() const
{
    return this->placement_mode == MapEntityPlacementMode::CreateNew;
}

bool MapCanvasPlacement::isMoving() const
{
    return this->placement_mode == MapEntityPlacementMode::MoveExisting;
}

QUuid MapCanvasPlacement::floatingUuid() const
{
    return this->floating_uuid;
}

QString MapCanvasPlacement::floatingPixmapPath() const
{
    return this->floating_pixmap_path;
}

int MapCanvasPlacement::floatingWidth() const
{
    return this->floating_width;
}

bool MapCanvasPlacement::hasFloatingMarker() const
{
    return !this->floating_pixmap_path.isEmpty() && this->floating_width > 0;
}

bool MapCanvasPlacement::floatingMarkerVisible() const
{
    return hasFloatingMarker() && this->floating_visible;
}

void MapCanvasPlacement::consumeCreatedMarker()
{
    if (!isCreating())
        return;

    this->floating_uuid = QUuid();
    this->floating_pixmap_path.clear();
    this->floating_width = 0;
    this->floating_visible = false;
}

void MapCanvasPlacement::completeMove()
{
    setMoveCursor(false);
    clearConnectionTarget();
    this->floating_uuid = QUuid();
    this->floating_pixmap_path.clear();
    this->floating_width = 0;
    this->floating_visible = false;
    this->placement_mode = MapEntityPlacementMode::None;
    this->current_entity = InfrastructureEntity::Unknown;
    this->moving_selected = false;
    this->mouse_coordinate_valid = false;
}

void MapCanvasPlacement::updateMousePosition(const QPointF &position,
                                             const CoordinateWGS84 &coordinate)
{
    this->mouse_position = position;
    // A group move has already translated its markers to the last valid
    // coordinate. If the globe ray missed in between two valid positions,
    // resume from that last point instead of dropping the missing delta.
    if (this->mouse_coordinate_valid || (isMoving() && this->moving_selected))
        this->previous_mouse_coordinate = this->mouse_coordinate;
    else
        this->previous_mouse_coordinate = coordinate;
    this->mouse_coordinate = coordinate;
    this->mouse_coordinate_valid = true;
}

void MapCanvasPlacement::invalidateMousePosition(const QPointF &position)
{
    this->mouse_position = position;
    this->mouse_coordinate_valid = false;
    this->connection_target_uuid = QUuid();
    this->floating_visible = false;
    // This is a globe-ray miss, not the 2D post-placement suppression
    // around a just-created node. Reveal immediately on the next valid ray.
    this->draw_immediately = true;
}

QPointF MapCanvasPlacement::mousePosition() const
{
    return this->mouse_position;
}

CoordinateWGS84 MapCanvasPlacement::mouseCoordinate() const
{
    return this->mouse_coordinate;
}

CoordinateWGS84 MapCanvasPlacement::previousMouseCoordinate() const
{
    return this->previous_mouse_coordinate;
}

bool MapCanvasPlacement::mouseCoordinateValid() const
{
    return this->mouse_coordinate_valid;
}

bool MapCanvasPlacement::revealFloatingMarkerIfReady()
{
    if (!hasFloatingMarker())
        return false;
    if (this->floating_visible)
        return true;

    if (isCreating() && !this->draw_immediately &&
        (this->mouse_position.toPoint() - this->floating_hide_until).manhattanLength() <= 10)
    {
        return false;
    }

    this->floating_visible = true;
    return true;
}

void MapCanvasPlacement::setFloatingHiddenUntil(const QPoint &position)
{
    this->floating_hide_until = position;
    this->floating_visible = false;
}

void MapCanvasPlacement::scaleFloatingMarker(const QString &pixmap_path, int width)
{
    if (!hasFloatingMarker())
        return;

    this->floating_pixmap_path = pixmap_path;
    this->floating_width = width;
}

void MapCanvasPlacement::setConnectionTarget(const QUuid &uuid)
{
    this->connection_target_uuid = uuid;
}

QUuid MapCanvasPlacement::connectionTargetUuid() const
{
    return this->connection_target_uuid;
}

void MapCanvasPlacement::clearConnectionTarget()
{
    this->connection_target_uuid = QUuid();
}

void MapCanvasPlacement::setMovingSelected(bool moving_selected)
{
    this->moving_selected = moving_selected;
}

bool MapCanvasPlacement::movingSelected() const
{
    return this->moving_selected;
}

void MapCanvasPlacement::setMoveCursor(bool enabled)
{
    if (enabled)
    {
        if (this->move_cursor_active)
            QApplication::changeOverrideCursor(Qt::SizeAllCursor);
        else
        {
            QApplication::setOverrideCursor(Qt::SizeAllCursor);
            this->move_cursor_active = true;
        }
        return;
    }

    if (!this->move_cursor_active)
        return;

    QApplication::restoreOverrideCursor();
    this->move_cursor_active = false;
}

void MapCanvasPlacement::prepareFloatingMarker()
{
    this->floating_visible = this->draw_immediately;
    this->draw_immediately = false;
    focusCanvas();
    if (isMoving())
        setMoveCursor(true);
}

void MapCanvasPlacement::focusCanvas()
{
    if (!this->map_canvas)
        return;

    this->map_canvas->setFocusPolicy(Qt::StrongFocus);
    this->map_canvas->setFocus(Qt::OtherFocusReason);
}
