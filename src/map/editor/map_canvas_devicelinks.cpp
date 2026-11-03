#include "map/editor/map_canvas_devicelinks.h"
#include "map/editor/map_canvas_projection.h"
#include "map/editor/map_canvas_markers.h"
#include "map/editor/map_canvas_widget.h"

#include "geo/geo_web_mercator.h"
#include "network/infrastructure_entity_traits.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
constexpr double link_hit_distance = 7.0;

QPointF nearestPointOnSegment(const QPointF &point,
                              const QPointF &segment_start,
                              const QPointF &segment_end)
{
    const double segment_x = segment_end.x() - segment_start.x();
    const double segment_y = segment_end.y() - segment_start.y();
    const double segment_length_squared = segment_x * segment_x + segment_y * segment_y;
    if (segment_length_squared <= 0.0)
        return segment_start;

    const double projection = ((point.x() - segment_start.x()) * segment_x +
                               (point.y() - segment_start.y()) * segment_y) /
                              segment_length_squared;
    const double bounded_projection = qBound(0.0, projection, 1.0);
    return QPointF(segment_start.x() + bounded_projection * segment_x,
                   segment_start.y() + bounded_projection * segment_y);
}

double distanceSquaredToSegment(const QPointF &point,
                                const QPointF &segment_start,
                                const QPointF &segment_end)
{
    const QPointF nearest_point = nearestPointOnSegment(point, segment_start, segment_end);
    const double distance_x = point.x() - nearest_point.x();
    const double distance_y = point.y() - nearest_point.y();
    return distance_x * distance_x + distance_y * distance_y;
}
}

MapCanvasDeviceLinks::MapCanvasDeviceLinks(MapModel *map_model,
                                           MapCanvasWidget *map_canvas,
                                           MapEntityPixmapRenderer *pixmap_renderer,
                                           QObject *parent)
    : QObject(parent)
{
    this->map_model = map_model;
    this->map_canvas = map_canvas;
    this->pixmap_renderer = pixmap_renderer;
}

void MapCanvasDeviceLinks::setProjection(MapCanvasProjection *projection)
{
    this->projection = projection;
}

void MapCanvasDeviceLinks::setWrapReferenceLongitude(double longitude)
{
    this->wrap_reference_lon = GeoWebMercator::normalizeLongitude(longitude);
}

QPointF MapCanvasDeviceLinks::screenFromWgs84(const CoordinateWGS84 &coordinate) const
{
    const std::optional<QPointF> position = this->projection->screenFromCoordinate(
        coordinate, this->map_canvas->size(), this->wrap_reference_lon);
    return position.value_or(QPointF(std::numeric_limits<double>::quiet_NaN(),
                                    std::numeric_limits<double>::quiet_NaN()));
}

void MapCanvasDeviceLinks::clear()
{
    clearPlacement();
    this->list_device_links.clear();
    this->device_link_indices_by_uuid.clear();
    updateCanvas();
}

void MapCanvasDeviceLinks::clearPlacement()
{
    this->device_link_start_uuid = QUuid();
}

bool MapCanvasDeviceLinks::hasStartNode() const
{
    return !this->device_link_start_uuid.isNull();
}

QUuid MapCanvasDeviceLinks::startNodeUuid() const
{
    return this->device_link_start_uuid;
}

bool MapCanvasDeviceLinks::addDeviceLink(const InfrastructureEntityReference &entity,
                                         const DeviceLinkGeometry &geometry,
                                         const QString &pixmap_path,
                                         int marker_width)
{
    if (geometry.start_node.uuid.isNull() || geometry.end_node.uuid.isNull() ||
        geometry.start_node.uuid == geometry.end_node.uuid || entity.uuid.isNull() ||
        !InfrastructureEntityTraits::isHydraulicConnectionNode(geometry.start_node.type) ||
        !InfrastructureEntityTraits::isHydraulicConnectionNode(geometry.end_node.type) ||
        !InfrastructureEntityTraits::isHydraulicDeviceLink(entity.type))
    {
        return false;
    }

    this->device_marker_width = marker_width;

    DeviceLinkCanvasItem device_link;
    device_link.entity = entity;
    device_link.geometry = geometry;
    device_link.path_pixmap = pixmap_path;
    this->list_device_links.append(device_link);
    this->device_link_indices_by_uuid.insert(entity.uuid, this->list_device_links.size() - 1);
    updateCanvas();
    return true;
}

MapCanvasDeviceLinks::AnchorResult MapCanvasDeviceLinks::anchor(
    const InfrastructureEntityReference &entity,
    const QUuid &connection_target_uuid,
    const MapCanvasMarkers &markers,
    const QString &pixmap_path,
    int marker_width)
{
    AnchorResult result;
    const std::optional<MapEntityMarker> target_marker = pointMarkerByUuid(
        connection_target_uuid, markers);
    if (!target_marker.has_value() ||
        !InfrastructureEntityTraits::isHydraulicConnectionNode(target_marker->entity.type))
        return result;

    if (this->device_link_start_uuid.isNull())
    {
        this->device_link_start_uuid = connection_target_uuid;
        result.status = AnchorStatus::StartSet;
        updateCanvas();
        return result;
    }

    const std::optional<DeviceLinkGeometry> geometry = completionGeometry(
        connection_target_uuid, markers);
    if (!geometry.has_value())
        return result;

    if (!addDeviceLink(entity, geometry.value(), pixmap_path, marker_width))
        return result;

    result.status = AnchorStatus::Completed;
    clearPlacement();
    return result;
}

std::optional<DeviceLinkGeometry> MapCanvasDeviceLinks::completionGeometry(
    const QUuid &connection_target_uuid,
    const MapCanvasMarkers &markers) const
{
    if (this->device_link_start_uuid.isNull() || connection_target_uuid.isNull() ||
        connection_target_uuid == this->device_link_start_uuid)
    {
        return std::nullopt;
    }

    const std::optional<MapEntityMarker> start_marker = pointMarkerByUuid(
        this->device_link_start_uuid, markers);
    const std::optional<MapEntityMarker> end_marker = pointMarkerByUuid(
        connection_target_uuid, markers);
    if (!start_marker.has_value() || !end_marker.has_value() ||
        !InfrastructureEntityTraits::isHydraulicConnectionNode(start_marker->entity.type) ||
        !InfrastructureEntityTraits::isHydraulicConnectionNode(end_marker->entity.type))
    {
        return std::nullopt;
    }

    DeviceLinkGeometry geometry;
    geometry.start_node = start_marker->entity;
    geometry.end_node = end_marker->entity;
    if (!this->projection->supportsMercatorHitBox())
    {
        // A globe screen-space midpoint is not guaranteed to hit terrain:
        // the endpoints may be elevated, underground, or across a valley.
        // Store the geographic center of the actual hydraulic endpoints.
        const CoordinateWGS84 &start = start_marker->coord_wgs84;
        const CoordinateWGS84 &end = end_marker->coord_wgs84;
        geometry.center_coordinate.latitude_deg =
            (start.latitude_deg + end.latitude_deg) / 2.0;
        const double longitude_delta = GeoWebMercator::normalizeLongitude(
            end.longitude_deg - start.longitude_deg);
        geometry.center_coordinate.longitude_deg = GeoWebMercator::normalizeLongitude(
            start.longitude_deg + longitude_delta / 2.0);
        return geometry;
    }

    // Keep the original 2D behavior, where the marker is centered in the
    // projected map between the two visible endpoints.
    const QPointF start_point = screenFromWgs84(start_marker->coord_wgs84);
    const QPointF end_point = screenFromWgs84(end_marker->coord_wgs84);
    if (!std::isfinite(start_point.x()) || !std::isfinite(start_point.y()) ||
        !std::isfinite(end_point.x()) || !std::isfinite(end_point.y()))
        return std::nullopt;
    const std::optional<CoordinateWGS84> center = this->projection->coordinateFromScreen(
        (start_point + end_point) / 2.0, this->map_canvas->size());
    if (!center.has_value())
        return std::nullopt;
    geometry.center_coordinate = center.value();
    return geometry;
}

bool MapCanvasDeviceLinks::updateMove(const QUuid &uuid, const QPointF &screen_position)
{
    if (!this->map_model || !this->map_canvas)
        return false;
    const auto iterator = this->device_link_indices_by_uuid.constFind(uuid);
    if (iterator == this->device_link_indices_by_uuid.cend())
        return false;

    const std::optional<CoordinateWGS84> coordinate = this->projection->coordinateFromScreen(
        screen_position, this->map_canvas->size());
    if (!coordinate.has_value())
        return false;
    this->list_device_links[iterator.value()].geometry.center_coordinate = coordinate.value();
    updateCanvas();
    return true;
}

bool MapCanvasDeviceLinks::setCenterCoordinate(const QUuid &uuid,
                                               const CoordinateWGS84 &coordinate)
{
    const auto iterator = this->device_link_indices_by_uuid.constFind(uuid);
    if (iterator == this->device_link_indices_by_uuid.cend())
        return false;
    this->list_device_links[iterator.value()].geometry.center_coordinate = coordinate;
    updateCanvas();
    return true;
}

void MapCanvasDeviceLinks::scaleMarkers(int width)
{
    this->device_marker_width = width;
    updateCanvas();
}

bool MapCanvasDeviceLinks::moveCenterByDelta(const QUuid &uuid,
                                             double longitude_delta,
                                             double latitude_delta)
{
    const auto iterator = this->device_link_indices_by_uuid.constFind(uuid);
    if (iterator == this->device_link_indices_by_uuid.cend())
        return false;

    DeviceLinkCanvasItem &device_link = this->list_device_links[iterator.value()];
    device_link.geometry.center_coordinate.latitude_deg = std::clamp(
        device_link.geometry.center_coordinate.latitude_deg + latitude_delta,
        -GeoWebMercator::MaximumLatitude, GeoWebMercator::MaximumLatitude);
    device_link.geometry.center_coordinate.longitude_deg = GeoWebMercator::normalizeLongitude(
        device_link.geometry.center_coordinate.longitude_deg + longitude_delta);
    return true;
}

void MapCanvasDeviceLinks::removeConnectedToUuid(const QUuid &uuid)
{
    if (uuid.isNull())
        return;

    if (this->device_link_start_uuid == uuid)
        clearPlacement();

    for (int i = this->list_device_links.size() - 1; i >= 0; i--)
    {
        const DeviceLinkCanvasItem &device_link = this->list_device_links[i];
        if (device_link.geometry.start_node.uuid != uuid &&
            device_link.geometry.end_node.uuid != uuid &&
            device_link.entity.uuid != uuid)
        {
            continue;
        }

        this->list_device_links.removeAt(i);
    }

    rebuildUuidIndex();
    updateCanvas();
}

QList<MapEntityMarker> MapCanvasDeviceLinks::markers() const
{
    QList<MapEntityMarker> result;
    result.reserve(this->list_device_links.size());

    for (const DeviceLinkCanvasItem &device_link : this->list_device_links)
    {
        MapEntityMarker marker;
        marker.entity = device_link.entity;
        marker.coord_wgs84 = device_link.geometry.center_coordinate;
        marker.path_pixmap = device_link.path_pixmap;
        result.append(marker);
    }

    return result;
}

std::optional<MapEntityMarker> MapCanvasDeviceLinks::markerByUuid(const QUuid &uuid) const
{
    const auto iterator = this->device_link_indices_by_uuid.constFind(uuid);
    if (iterator == this->device_link_indices_by_uuid.cend())
        return std::nullopt;
    const DeviceLinkCanvasItem &device_link = this->list_device_links.at(iterator.value());
    MapEntityMarker marker;
    marker.entity = device_link.entity;
    marker.coord_wgs84 = device_link.geometry.center_coordinate;
    marker.path_pixmap = device_link.path_pixmap;
    return marker;
}

std::optional<DeviceLinkGeometry> MapCanvasDeviceLinks::geometryByUuid(const QUuid &uuid) const
{
    const auto iterator = this->device_link_indices_by_uuid.constFind(uuid);
    if (iterator == this->device_link_indices_by_uuid.cend())
        return std::nullopt;
    return this->list_device_links.at(iterator.value()).geometry;
}

QList<QUuid> MapCanvasDeviceLinks::connectedLinkUuids(const QSet<QUuid> &node_uuids) const
{
    QList<QUuid> result;
    for (const DeviceLinkCanvasItem &item : this->list_device_links)
    {
        if (node_uuids.contains(item.geometry.start_node.uuid) ||
            node_uuids.contains(item.geometry.end_node.uuid))
        {
            result.append(item.entity.uuid);
        }
    }
    return result;
}

std::optional<InfrastructureEntityReference> MapCanvasDeviceLinks::markerAt(
    const QPointF &position) const
{
    if (!this->pixmap_renderer)
        return std::nullopt;

    for (int i = this->list_device_links.size() - 1; i >= 0; i--)
    {
        const DeviceLinkCanvasItem &device_link = this->list_device_links[i];
        MapEntityMarker marker;
        marker.entity = device_link.entity;
        marker.coord_wgs84 = device_link.geometry.center_coordinate;
        marker.path_pixmap = device_link.path_pixmap;
        const QPointF center = markerCenterPosition(marker);
        if (!std::isfinite(center.x()) || !std::isfinite(center.y()))
            continue;
        if (this->pixmap_renderer->hitTest(marker.path_pixmap, this->device_marker_width,
                                           markerRect(marker), position))
        {
            return marker.entity;
        }
    }

    return std::nullopt;
}

std::optional<InfrastructureEntityReference> MapCanvasDeviceLinks::linkAt(
    const QPointF &position, const MapCanvasMarkers &markers) const
{
    double nearest_distance_squared = link_hit_distance * link_hit_distance;
    std::optional<InfrastructureEntityReference> nearest_entity;

    for (const DeviceLinkCanvasItem &device_link : this->list_device_links)
    {
        const std::optional<MapEntityMarker> start_marker = pointMarkerByUuid(
            device_link.geometry.start_node.uuid, markers);
        const std::optional<MapEntityMarker> end_marker = pointMarkerByUuid(
            device_link.geometry.end_node.uuid, markers);
        if (!start_marker.has_value() || !end_marker.has_value())
            continue;

        const QPointF start_point = screenFromWgs84(start_marker->coord_wgs84);
        const QPointF center_point = screenFromWgs84(device_link.geometry.center_coordinate);
        const QPointF end_point = screenFromWgs84(end_marker->coord_wgs84);
        if (!std::isfinite(start_point.x()) || !std::isfinite(start_point.y()) ||
            !std::isfinite(center_point.x()) || !std::isfinite(center_point.y()) ||
            !std::isfinite(end_point.x()) || !std::isfinite(end_point.y()))
        {
            continue;
        }
        const double distance_squared = qMin(
            distanceSquaredToSegment(position, start_point, center_point),
            distanceSquaredToSegment(position, center_point, end_point));
        if (distance_squared > nearest_distance_squared)
            continue;

        nearest_distance_squared = distance_squared;
        nearest_entity = device_link.entity;
    }

    return nearest_entity;
}

QPointF MapCanvasDeviceLinks::markerCenterPosition(const MapEntityMarker &marker) const
{
    return screenFromWgs84(marker.coord_wgs84);
}

QRectF MapCanvasDeviceLinks::markerRect(const MapEntityMarker &marker) const
{
    if (!this->pixmap_renderer)
        return QRectF();

    return this->pixmap_renderer->centeredRect(
        markerCenterPosition(marker), marker.path_pixmap, this->device_marker_width);
}

std::optional<MapEntityMarker> MapCanvasDeviceLinks::pointMarkerByUuid(
    const QUuid &uuid, const MapCanvasMarkers &markers) const
{
    // The node markers' uuid index: linkAt() asks twice per device link on
    // every hover move, a linear search over all markers (demand points
    // included) made that O(links x markers).
    const MapEntityMarker *marker = markers.findMarker(uuid);
    if (marker == nullptr)
        return std::nullopt;
    return *marker;
}

void MapCanvasDeviceLinks::rebuildUuidIndex()
{
    this->device_link_indices_by_uuid.clear();
    this->device_link_indices_by_uuid.reserve(this->list_device_links.size());
    for (int i = 0; i < this->list_device_links.size(); ++i)
        this->device_link_indices_by_uuid.insert(this->list_device_links.at(i).entity.uuid, i);
}

void MapCanvasDeviceLinks::updateCanvas()
{
    emit signalCanvasUpdateRequested();
}
