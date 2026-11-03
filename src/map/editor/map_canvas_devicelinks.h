#ifndef MAP_CANVAS_DEVICELINKS_H
#define MAP_CANVAS_DEVICELINKS_H

#include <optional>
#include <QObject>
#include <QHash>
#include <QSet>
#include <QList>
#include <QPointer>
#include <QRectF>
#include <QPointF>
#include <QString>
#include <QUuid>

#include "map/render/map_entity_pixmap_renderer.h"
#include "map/core/map_model.h"
#include "map/core/map_models.h"

class MapCanvasMarkers;
class MapCanvasWidget;
class MapCanvasProjection;

class MapCanvasDeviceLinks : public QObject
{
    Q_OBJECT

public:
    enum class AnchorStatus
    {
        NoChange,
        StartSet,
        Completed
    };

    struct AnchorResult
    {
        AnchorStatus status = AnchorStatus::NoChange;
    };

    explicit MapCanvasDeviceLinks(MapModel *map_model, MapCanvasWidget *map_canvas,
                                  MapEntityPixmapRenderer *pixmap_renderer,
                                  QObject *parent = nullptr);

    void setProjection(MapCanvasProjection *projection);
    void setWrapReferenceLongitude(double longitude);

    void clear();
    void clearPlacement();
    bool hasStartNode() const;
    QUuid startNodeUuid() const;
    bool addDeviceLink(const InfrastructureEntityReference &entity,
                       const DeviceLinkGeometry &geometry,
                       const QString &pixmap_path,
                       int marker_width);
    AnchorResult anchor(const InfrastructureEntityReference &entity,
                        const QUuid &connection_target_uuid,
                        const MapCanvasMarkers &markers,
                        const QString &pixmap_path,
                        int marker_width);
    std::optional<DeviceLinkGeometry> completionGeometry(
        const QUuid &connection_target_uuid,
        const MapCanvasMarkers &markers) const;

    bool updateMove(const QUuid &uuid, const QPointF &screen_position);
    bool setCenterCoordinate(const QUuid &uuid, const CoordinateWGS84 &coordinate);
    void scaleMarkers(int width);
    bool moveCenterByDelta(const QUuid &uuid,
                           double longitude_delta,
                           double latitude_delta);
    void removeConnectedToUuid(const QUuid &uuid);

    QList<MapEntityMarker> markers() const;
    std::optional<MapEntityMarker> markerByUuid(const QUuid &uuid) const;
    std::optional<DeviceLinkGeometry> geometryByUuid(const QUuid &uuid) const;
    QList<QUuid> connectedLinkUuids(const QSet<QUuid> &node_uuids) const;
    std::optional<InfrastructureEntityReference> markerAt(const QPointF &position) const;
    std::optional<InfrastructureEntityReference> linkAt(
        const QPointF &position, const MapCanvasMarkers &markers) const;
    QPointF markerCenterPosition(const MapEntityMarker &marker) const;
    QRectF markerRect(const MapEntityMarker &marker) const;

signals:
    void signalCanvasUpdateRequested();

private:
    struct DeviceLinkCanvasItem
    {
        InfrastructureEntityReference entity;
        DeviceLinkGeometry geometry;
        QString path_pixmap;
    };

    QPointF screenFromWgs84(const CoordinateWGS84 &coordinate) const;
    std::optional<MapEntityMarker> pointMarkerByUuid(
        const QUuid &uuid, const MapCanvasMarkers &markers) const;
    void rebuildUuidIndex();
    void updateCanvas();

    MapModel *map_model = nullptr;
    MapCanvasProjection *projection = nullptr;
    QPointer<MapCanvasWidget> map_canvas;
    MapEntityPixmapRenderer *pixmap_renderer = nullptr;
    double wrap_reference_lon = 0.0;
    int device_marker_width = 10;
    QList<DeviceLinkCanvasItem> list_device_links;
    QHash<QUuid, int> device_link_indices_by_uuid;
    QUuid device_link_start_uuid;
};

#endif // MAP_CANVAS_DEVICELINKS_H
