#ifndef MAP_CANVAS_PLACEMENT_H
#define MAP_CANVAS_PLACEMENT_H

#include <QObject>
#include <QPoint>
#include <QPointF>
#include <QPointer>
#include <QString>
#include <QUuid>

#include "map/core/map_models.h"

class MapCanvasWidget;

class MapCanvasPlacement : public QObject
{
    Q_OBJECT

public:
    explicit MapCanvasPlacement(MapCanvasWidget *map_canvas, QObject *parent = nullptr);

    void startCreate(InfrastructureEntity entity, const QString &pixmap_path, int width);
    bool rearmCreate(const QString &pixmap_path, int width);
    bool startMove(InfrastructureEntity entity, const QUuid &uuid,
                   const QString &pixmap_path, int width,
                   const QPointF &mouse_position, const CoordinateWGS84 &mouse_coordinate);
    void startVirtualMove(InfrastructureEntity entity, const QPointF &mouse_position,
                          const CoordinateWGS84 &mouse_coordinate);
    void stop();

    MapEntityPlacementMode mode() const;
    InfrastructureEntity entity() const;
    bool isIdle() const;
    bool isCreating() const;
    bool isMoving() const;

    QUuid floatingUuid() const;
    QString floatingPixmapPath() const;
    int floatingWidth() const;
    bool hasFloatingMarker() const;
    bool floatingMarkerVisible() const;
    void consumeCreatedMarker();
    void completeMove();

    void updateMousePosition(const QPointF &position, const CoordinateWGS84 &coordinate);
    // A globe ray can miss the map. Hide the pointer preview instead of
    // reusing the last valid ground coordinate or snapping to an old node.
    void invalidateMousePosition(const QPointF &position);
    QPointF mousePosition() const;
    CoordinateWGS84 mouseCoordinate() const;
    CoordinateWGS84 previousMouseCoordinate() const;
    bool mouseCoordinateValid() const;
    bool revealFloatingMarkerIfReady();
    void setFloatingHiddenUntil(const QPoint &position);
    void scaleFloatingMarker(const QString &pixmap_path, int width);

    void setConnectionTarget(const QUuid &uuid);
    QUuid connectionTargetUuid() const;
    void clearConnectionTarget();

    void setMovingSelected(bool moving_selected);
    bool movingSelected() const;
    void setMoveCursor(bool enabled);

private:
    void prepareFloatingMarker();
    void focusCanvas();

    QPointer<MapCanvasWidget> map_canvas;
    QUuid floating_uuid;
    QUuid connection_target_uuid;
    QString floating_pixmap_path;
    int floating_width = 0;
    MapEntityPlacementMode placement_mode = MapEntityPlacementMode::None;
    InfrastructureEntity current_entity = InfrastructureEntity::Unknown;
    QPoint floating_hide_until;
    QPointF mouse_position;
    CoordinateWGS84 mouse_coordinate;
    CoordinateWGS84 previous_mouse_coordinate;
    bool mouse_coordinate_valid = false;
    bool draw_immediately = true;
    bool floating_visible = false;
    bool move_cursor_active = false;
    bool moving_selected = false;
};

#endif // MAP_CANVAS_PLACEMENT_H
