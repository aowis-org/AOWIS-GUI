#ifndef MAP_EDITOR_MAP_ANNOTATION_SELECTION_H
#define MAP_EDITOR_MAP_ANNOTATION_SELECTION_H

#include "map/editor/map_annotation_hit_area.h"

#include <GeographicLib/Geodesic.hpp>

#include <QHash>
#include <QList>
#include <QPointF>
#include <QRectF>
#include <QSet>
#include <QUuid>

#include <functional>
#include <cmath>

// Annotation visibility in MODEL controls MONITOR only. The Editor must
// always render/select annotations so that hiding one cannot strand it.
namespace MapAnnotationSelection
{
inline QList<MapAnnotation> editorVisible(const QList<MapAnnotation> &annotations)
{
    QList<MapAnnotation> result = annotations;
    for (MapAnnotation &annotation : result)
        annotation.style.visible = true;
    return result;
}

// Select the same callout footprint that hit-testing and bgfx draw, not just
// the pin. The screen projector is the Editor's 2D or terrain-aware 3D one.
inline QSet<QUuid> inRectangle(
    const QList<MapAnnotation> &annotations, const QRectF &rectangle,
    int scale_percent,
    const std::function<bool(const MapAnnotation &, QPointF *)> &project)
{
    QSet<QUuid> result;
    if (scale_percent <= 0 || rectangle.isEmpty() || !project)
        return result;
    for (const MapAnnotation &annotation : annotations)
    {
        if (annotation.uuid.isNull())
            continue;
        QPointF position;
        if (!project(annotation, &position))
            continue;
        if (MapAnnotationHitArea::rect(annotation, position, scale_percent)
                .intersects(rectangle))
            result.insert(annotation.uuid);
    }
    return result;
}

// Translate selected annotations by the same WGS84 geodesic displacement as
// the dragged callout. Input positions are resolved (anchor offsets applied),
// but the caller writes changes through MapAnnotationPosition::place() so
// canonical anchor offsets remain correct.
inline QHash<QUuid, CoordinateWGS84> translatedPositions(
    const QList<MapAnnotation> &resolved, const QSet<QUuid> &selected,
    const QUuid &dragged_uuid, const CoordinateWGS84 &new_dragged_position)
{
    QHash<QUuid, CoordinateWGS84> positions;
    if (selected.isEmpty() || !selected.contains(dragged_uuid))
        return positions;
    const MapAnnotation *dragged = nullptr;
    for (const MapAnnotation &annotation : resolved)
        if (annotation.uuid == dragged_uuid)
        {
            dragged = &annotation;
            break;
        }
    if (!dragged)
        return positions;
    double distance = 0.0;
    double azimuth = 0.0;
    double reverse_azimuth = 0.0;
    GeographicLib::Geodesic::WGS84().Inverse(
        dragged->position_wgs84.latitude_deg,
        dragged->position_wgs84.longitude_deg,
        new_dragged_position.latitude_deg, new_dragged_position.longitude_deg,
        distance, azimuth, reverse_azimuth);
    if (!std::isfinite(distance) || !std::isfinite(azimuth))
        return {};
    for (const MapAnnotation &annotation : resolved)
    {
        if (!selected.contains(annotation.uuid))
            continue;
        CoordinateWGS84 position = annotation.position_wgs84;
        GeographicLib::Geodesic::WGS84().Direct(
            annotation.position_wgs84.latitude_deg,
            annotation.position_wgs84.longitude_deg, azimuth, distance,
            position.latitude_deg, position.longitude_deg);
        positions.insert(annotation.uuid, position);
    }
    return positions;
}
}

#endif // MAP_EDITOR_MAP_ANNOTATION_SELECTION_H
