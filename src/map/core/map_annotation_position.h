#ifndef MAP_CORE_MAP_ANNOTATION_POSITION_H
#define MAP_CORE_MAP_ANNOTATION_POSITION_H

#include <aowis/model/map/map_annotation.h>

#include <GeographicLib/Geodesic.hpp>

#include <QHash>
#include <QList>
#include <QSet>
#include <QString>
#include <QUuid>

#include <cmath>
#include <optional>
#include <utility>

// Domain-independent WGS84 annotation placement. MODEL owns canonical
// coordinates/anchors but deliberately does not implement geodesy. The
// caller supplies ONLY targets from the annotation's own domain; this core
// never needs to include a Distribution or Drainage engineering network.
namespace MapAnnotationPosition
{
struct Target
{
    QUuid uuid;
    QString type;
    QString id;
    CoordinateWGS84 coordinate;
};

// The position resolver accepts a domain-owned collection of eligible point
// targets. Distribution adapts its hydraulic nodes below; Drainage can supply
// its own targets later without including a hydraulic network in this logic.
inline std::optional<CoordinateWGS84> targetPosition(
    const QList<Target> &available_targets, const MapAnnotationAnchor &anchor)
{
    std::optional<CoordinateWGS84> position;
    int matches = 0;
    for (const Target &target : available_targets)
    {
        if (target.uuid != anchor.entity_uuid)
            continue;
        ++matches;
        if (target.type == anchor.entity_type)
            position = target.coordinate;
    }
    // An ambiguous UUID must not select an arbitrary entity, even if only
    // one of the records has the requested type.
    if (matches != 1)
        return std::nullopt;
    return position;
}

inline std::optional<CoordinateWGS84> offsetFromTarget(
    const CoordinateWGS84 &target, const MapAnnotationAnchor &anchor)
{
    const double east = anchor.offset_east_m;
    const double north = anchor.offset_north_m;
    if (!std::isfinite(east) || !std::isfinite(north))
        return std::nullopt;
    CoordinateWGS84 position = target;
    if (east != 0.0 || north != 0.0)
    {
        const double azimuth = std::atan2(east, north) * (180.0 / 3.14159265358979323846);
        GeographicLib::Geodesic::WGS84().Direct(target.latitude_deg,
            target.longitude_deg, azimuth, std::hypot(east, north),
            position.latitude_deg, position.longitude_deg);
    }
    return position;
}

inline std::optional<CoordinateWGS84> resolve(
    const QList<Target> &available_targets, const MapAnnotation &annotation)
{
    if (!annotation.anchor)
        return annotation.position_wgs84;
    const std::optional<CoordinateWGS84> target = targetPosition(
        available_targets, *annotation.anchor);
    return target ? offsetFromTarget(*target, *annotation.anchor) : std::nullopt;
}

// Compute east/north offsets geodesically when an annotation is attached or
// dragged. A free-standing annotation stores its picked coordinate directly.
inline bool place(const QList<Target> &available_targets, MapAnnotation &annotation,
                  const CoordinateWGS84 &position)
{
    if (!std::isfinite(position.latitude_deg) || !std::isfinite(position.longitude_deg) ||
        position.latitude_deg < -90.0 || position.latitude_deg > 90.0 ||
        position.longitude_deg < -180.0 || position.longitude_deg > 180.0)
        return false;
    if (annotation.anchor)
    {
        const std::optional<CoordinateWGS84> target = targetPosition(
            available_targets, *annotation.anchor);
        if (!target)
            return false;
        double distance = 0.0;
        double azimuth = 0.0;
        double return_azimuth = 0.0;
        GeographicLib::Geodesic::WGS84().Inverse(target->latitude_deg,
            target->longitude_deg, position.latitude_deg, position.longitude_deg,
            distance, azimuth, return_azimuth);
        const double radians = azimuth * (3.14159265358979323846 / 180.0);
        annotation.anchor->offset_east_m = distance * std::sin(radians);
        annotation.anchor->offset_north_m = distance * std::cos(radians);
    }
    // Last explicitly placed location is retained for a future detach.
    annotation.position_wgs84 = position;
    return true;
}

// Rendering gets a resolved copy, never a mutated canonical record. Anchors
// missing from the supplied domain's target collection are omitted fail-closed.
inline QList<MapAnnotation> drawable(const QList<Target> &available_targets,
                                      const QList<MapAnnotation> &annotations)
{
    if (annotations.isEmpty())
        return {};
    bool anchored = false;
    for (const MapAnnotation &annotation : annotations)
        if (annotation.anchor)
        {
            anchored = true;
            break;
        }
    if (!anchored)
        return annotations;

    // One index per frame rather than scanning every point for every label.
    // Duplicate UUIDs are ambiguous and cannot resolve an attachment.
    QHash<QUuid, Target> by_id;
    QSet<QUuid> ambiguous;
    for (const Target &target : available_targets)
    {
        if (ambiguous.contains(target.uuid))
            continue;
        if (by_id.contains(target.uuid))
        {
            by_id.remove(target.uuid);
            ambiguous.insert(target.uuid);
        }
        else
            by_id.insert(target.uuid, target);
    }
    QList<MapAnnotation> result;
    result.reserve(annotations.size());
    for (const MapAnnotation &annotation : annotations)
    {
        std::optional<CoordinateWGS84> position = annotation.position_wgs84;
        if (annotation.anchor)
        {
            const QHash<QUuid, Target>::const_iterator target =
                by_id.constFind(annotation.anchor->entity_uuid);
            if (target == by_id.cend() || target->type != annotation.anchor->entity_type)
                continue;
            position = offsetFromTarget(target->coordinate, *annotation.anchor);
        }
        if (!position)
            continue;
        MapAnnotation displayed = annotation;
        displayed.position_wgs84 = *position;
        displayed.anchor.reset();
        result.append(std::move(displayed));
    }
    return result;
}

inline bool references(const QList<MapAnnotation> &annotations, const QUuid &uuid)
{
    for (const MapAnnotation &annotation : annotations)
        if (annotation.anchor && annotation.anchor->entity_uuid == uuid)
            return true;
    return false;
}
} // namespace MapAnnotationPosition

#endif // MAP_CORE_MAP_ANNOTATION_POSITION_H
