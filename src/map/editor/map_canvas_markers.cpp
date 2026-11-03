#include "map/editor/map_canvas_markers.h"
#include "map/editor/map_canvas_projection.h"
#include "map/editor/map_canvas_hit_box.h"
#include "map/editor/map_canvas_widget.h"

#include "geo/geo_web_mercator.h"
#include "network/infrastructure_entity_traits.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <QtMath>

namespace
{
constexpr double marker_dot_radius = 5.0;
constexpr double marker_dot_hit_radius = marker_dot_radius * 3.0;
}

MapCanvasMarkers::MapCanvasMarkers(MapModel *map_model, MapCanvasWidget *map_canvas,
                                   MapEntityPixmapRenderer *pixmap_renderer,
                                   QObject *parent)
    : QObject(parent)
{
    this->map_model = map_model;
    this->map_canvas = map_canvas;
    this->pixmap_renderer = pixmap_renderer;
    this->marker_width = entityWidth();
}

void MapCanvasMarkers::setProjection(MapCanvasProjection *projection)
{
    this->projection = projection;
}

void MapCanvasMarkers::setWrapReferenceLongitude(double longitude)
{
    this->wrap_reference_lon = GeoWebMercator::normalizeLongitude(longitude);
}

void MapCanvasMarkers::setIconSizePercent(int size_percent)
{
    this->icon_size_percent = qBound(50, size_percent, 600);
}

QPointF MapCanvasMarkers::screenFromWgs84(const CoordinateWGS84 &coordinate) const
{
    const std::optional<QPointF> position = this->projection->screenFromCoordinate(
        coordinate, this->map_canvas->size(), this->wrap_reference_lon);
    return position.value_or(QPointF(std::numeric_limits<double>::quiet_NaN(),
                                    std::numeric_limits<double>::quiet_NaN()));
}

const QList<MapEntityMarker> &MapCanvasMarkers::markers() const
{
    return this->list_markers;
}

void MapCanvasMarkers::clear()
{
    this->list_markers.clear();
    this->marker_indices_by_uuid.clear();
}

int MapCanvasMarkers::entityWidth() const
{
    // Continuous in the camera's zoom, never the integer tile zoom: the
    // former steps (zoom <= 16: 10 px, 17: 20, 18: 30, >= 19: 40)
    // interpolated, so icons grow with the zoom motion instead of jumping
    // whenever an integer level is crossed. Whole pixels (pixmap widths).
    const double zoom = this->map_model->view2dContinuousZoom();
    const double base_width = std::clamp(10.0 + (zoom - 16.0) * 10.0, 10.0, 40.0);
    return qMax(1, qRound(base_width * this->icon_size_percent / 100.0));
}

int MapCanvasMarkers::iconSizePercent() const
{
    return this->icon_size_percent;
}

QString MapCanvasMarkers::pixmapPathForEntity(InfrastructureEntity entity) const
{
    return MapEntityPixmapRenderer::pixmapPathForEntity(entity);
}

std::optional<InfrastructureEntityReference> MapCanvasMarkers::nearestConnectionTarget(
    const QPointF &mouse_position, const QUuid &excluded_uuid, double max_distance) const
{
    std::optional<InfrastructureEntityReference> nearest_entity;
    double nearest_distance_squared = max_distance * max_distance;

    for (const MapEntityMarker &marker : this->list_markers)
    {
        if (marker.entity.uuid == excluded_uuid ||
            !InfrastructureEntityTraits::isHydraulicConnectionNode(marker.entity.type))
            continue;

        const QPointF point = markerAnchorPosition(marker);
        // The globe projector can reject nodes behind the horizon or before
        // its first frame. NaNs must never become connection candidates.
        if (!std::isfinite(point.x()) || !std::isfinite(point.y()))
            continue;
        const double distance_x = point.x() - mouse_position.x();
        const double distance_y = point.y() - mouse_position.y();
        const double distance_squared = distance_x * distance_x + distance_y * distance_y;
        if (distance_squared > nearest_distance_squared)
            continue;

        nearest_distance_squared = distance_squared;
        nearest_entity = marker.entity;
    }

    return nearest_entity;
}

std::optional<MapEntityMarker> MapCanvasMarkers::markerByUuid(const QUuid &uuid) const
{
    if (uuid.isNull())
        return std::nullopt;

    const auto iterator = this->marker_indices_by_uuid.constFind(uuid);
    if (iterator == this->marker_indices_by_uuid.cend())
        return std::nullopt;
    return this->list_markers.at(iterator.value());
}

const MapEntityMarker *MapCanvasMarkers::findMarker(const QUuid &uuid) const
{
    if (uuid.isNull())
        return nullptr;
    const QHash<QUuid, int>::const_iterator iterator = this->marker_indices_by_uuid.constFind(uuid);
    if (iterator == this->marker_indices_by_uuid.cend())
        return nullptr;
    return &this->list_markers.at(iterator.value());
}

MapEntityMarker MapCanvasMarkers::addMarker(const InfrastructureEntityReference &entity,
                                             const CoordinateWGS84 &coordinate,
                                             const QString &pixmap_path,
                                             int width)
{
    this->marker_width = width;

    MapEntityMarker marker;
    marker.entity = entity;
    marker.coord_wgs84 = coordinate;
    marker.path_pixmap = pixmap_path;
    this->list_markers.append(marker);
    this->marker_indices_by_uuid.insert(marker.entity.uuid, this->list_markers.size() - 1);
    return marker;
}

bool MapCanvasMarkers::removeMarker(const QUuid &uuid)
{
    for (int i = 0; i < this->list_markers.size(); i++)
    {
        if (this->list_markers[i].entity.uuid != uuid)
            continue;

        this->list_markers.removeAt(i);
        rebuildUuidIndex();
        return true;
    }

    return false;
}

bool MapCanvasMarkers::setCoordinate(const QUuid &uuid, const CoordinateWGS84 &coordinate)
{
    const auto iterator = this->marker_indices_by_uuid.constFind(uuid);
    if (iterator == this->marker_indices_by_uuid.cend())
        return false;
    this->list_markers[iterator.value()].coord_wgs84 = coordinate;
    return true;
}

bool MapCanvasMarkers::moveByDelta(const QUuid &uuid,
                                   double longitude_delta,
                                   double latitude_delta)
{
    const auto iterator = this->marker_indices_by_uuid.constFind(uuid);
    if (iterator == this->marker_indices_by_uuid.cend())
        return false;

    MapEntityMarker &marker = this->list_markers[iterator.value()];
    marker.coord_wgs84.latitude_deg = std::clamp(
        marker.coord_wgs84.latitude_deg + latitude_delta,
        -GeoWebMercator::MaximumLatitude, GeoWebMercator::MaximumLatitude);
    marker.coord_wgs84.longitude_deg = GeoWebMercator::normalizeLongitude(
        marker.coord_wgs84.longitude_deg + longitude_delta);
    return true;
}

void MapCanvasMarkers::scaleMarkers(int width)
{
    this->marker_width = width;
}

std::optional<InfrastructureEntityReference> MapCanvasMarkers::markerAt(
    const QPointF &position) const
{
    if (!this->pixmap_renderer || !this->map_model || !this->map_canvas)
        return std::nullopt;

    // A marker can only be hit within its dot radius or its icon, which is
    // marker_width wide and (editor pixmaps) at most a few widths tall above
    // the anchor: everything else is rejected in map coordinates before any
    // projection or pixmap lookup.
    const double reach = marker_dot_hit_radius + 4.0 * double(std::max(1, this->marker_width));
    const MapCanvasHitBox box = MapCanvasHitBox::around(
        *this->projection, position, reach, this->map_canvas->size());
    for (int i = this->list_markers.size() - 1; i >= 0; i--)
    {
        const MapEntityMarker &marker = this->list_markers[i];
        if (!box.contains(marker.coord_wgs84))
            continue;
        const QPointF dot_center = markerAnchorPosition(marker);
        if (!std::isfinite(dot_center.x()) || !std::isfinite(dot_center.y()))
            continue;
        if (dotHit(position, dot_center) ||
            this->pixmap_renderer->hitTest(marker.path_pixmap, this->marker_width,
                                           markerRect(marker), position))
        {
            return marker.entity;
        }
    }

    return std::nullopt;
}

bool MapCanvasMarkers::isMarkerAt(const QPointF &position) const
{
    return markerAt(position).has_value();
}

QPointF MapCanvasMarkers::markerAnchorPosition(const MapEntityMarker &marker) const
{
    return screenFromWgs84(marker.coord_wgs84);
}

QRectF MapCanvasMarkers::markerRect(const MapEntityMarker &marker) const
{
    if (!this->pixmap_renderer)
        return QRectF();

    const QPointF screen_position = screenFromWgs84(marker.coord_wgs84);
    const QPointF rounded_anchor(qRound(screen_position.x()), qRound(screen_position.y()));
    return this->pixmap_renderer->bottomAnchoredRect(
        rounded_anchor, marker.path_pixmap, this->marker_width);
}

bool MapCanvasMarkers::dotHit(const QPointF &position, const QPointF &dot_center) const
{
    const double distance_x = position.x() - dot_center.x();
    const double distance_y = position.y() - dot_center.y();
    return distance_x * distance_x + distance_y * distance_y <=
           marker_dot_hit_radius * marker_dot_hit_radius;
}

void MapCanvasMarkers::rebuildUuidIndex()
{
    this->marker_indices_by_uuid.clear();
    this->marker_indices_by_uuid.reserve(this->list_markers.size());
    for (int i = 0; i < this->list_markers.size(); ++i)
        this->marker_indices_by_uuid.insert(this->list_markers.at(i).entity.uuid, i);
}
