#ifndef MAP_CANVAS_MARKERS_H
#define MAP_CANVAS_MARKERS_H

#include <optional>
#include <QObject>
#include <QHash>
#include <QList>
#include <QPointer>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QUuid>

#include "map/render/map_entity_pixmap_renderer.h"
#include "map/core/map_model.h"
#include "map/core/map_models.h"

class MapCanvasWidget;
class MapCanvasProjection;

class MapCanvasMarkers : public QObject
{
    Q_OBJECT

public:
    explicit MapCanvasMarkers(MapModel *map_model, MapCanvasWidget *map_canvas,
                              MapEntityPixmapRenderer *pixmap_renderer,
                              QObject *parent = nullptr);

    void setProjection(MapCanvasProjection *projection);
    void setWrapReferenceLongitude(double longitude);
    void setIconSizePercent(int size_percent);

    const QList<MapEntityMarker> &markers() const;
    void clear();
    int entityWidth() const;
    int iconSizePercent() const;
    QString pixmapPathForEntity(InfrastructureEntity entity) const;
    std::optional<InfrastructureEntityReference> nearestConnectionTarget(
        const QPointF &mouse_position, const QUuid &excluded_uuid = QUuid(),
        double max_distance = 18.0) const;
    std::optional<MapEntityMarker> markerByUuid(const QUuid &uuid) const;
    // The marker of uuid in place (uuid index, no copy), nullptr if none.
    // Valid until the next marker list change.
    const MapEntityMarker *findMarker(const QUuid &uuid) const;
    MapEntityMarker addMarker(const InfrastructureEntityReference &entity,
                              const CoordinateWGS84 &coordinate,
                              const QString &pixmap_path,
                              int width);
    bool removeMarker(const QUuid &uuid);
    bool setCoordinate(const QUuid &uuid, const CoordinateWGS84 &coordinate);
    bool moveByDelta(const QUuid &uuid, double longitude_delta, double latitude_delta);

    void scaleMarkers(int width);
    std::optional<InfrastructureEntityReference> markerAt(const QPointF &position) const;
    bool isMarkerAt(const QPointF &position) const;
    QPointF markerAnchorPosition(const MapEntityMarker &marker) const;
    QRectF markerRect(const MapEntityMarker &marker) const;

private:
    QPointF screenFromWgs84(const CoordinateWGS84 &coordinate) const;
    bool dotHit(const QPointF &position, const QPointF &dot_center) const;
    void rebuildUuidIndex();

    MapModel *map_model = nullptr;
    MapCanvasProjection *projection = nullptr;
    QPointer<MapCanvasWidget> map_canvas;
    MapEntityPixmapRenderer *pixmap_renderer = nullptr;
    double wrap_reference_lon = 0.0;
    int icon_size_percent = 100;
    int marker_width = 10;
    QList<MapEntityMarker> list_markers;
    QHash<QUuid, int> marker_indices_by_uuid;
};

#endif // MAP_CANVAS_MARKERS_H
