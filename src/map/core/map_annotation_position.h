#ifndef MAP_CORE_MAP_ANNOTATION_POSITION_H
#define MAP_CORE_MAP_ANNOTATION_POSITION_H

#include <aowis/model/hydraulic/network_hydraulic_distribution.h>
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

// Presentation-only WGS84 placement. MODEL owns canonical coordinates and
// anchors, but deliberately does not implement geodesy. Both map modes and
// the Inspector use these functions; no renderer-specific state is involved.
namespace MapAnnotationPosition
{
struct Target
{
    QUuid uuid;
    QString type;
    QString id;
    CoordinateWGS84 coordinate;
};

inline QList<Target> targets(const NetworkHydraulicDistribution &network,
                             const QSet<QUuid> &only = {})
{
    QList<Target> result;
    for (const HydraulicNodeJunction &node : network.nodes_junctions)
        if (only.isEmpty() || only.contains(node.uuid))
            result.append(Target{node.uuid, QStringLiteral("HydraulicNodeJunction"), node.id,
                       node.coordinate_wgs84});
    for (const HydraulicNodeReservoir &node : network.nodes_reservoirs)
        if (only.isEmpty() || only.contains(node.uuid))
            result.append(Target{node.uuid, QStringLiteral("HydraulicNodeReservoir"), node.id,
                       node.coordinate_wgs84});
    for (const HydraulicNodeTank &node : network.nodes_tanks)
        if (only.isEmpty() || only.contains(node.uuid))
            result.append(Target{node.uuid, QStringLiteral("HydraulicNodeTank"), node.id,
                       node.coordinate_wgs84});
    for (const HydraulicDemandPoint &point : network.demand_points)
        if (only.isEmpty() || only.contains(point.uuid))
            result.append(Target{point.uuid, QStringLiteral("HydraulicDemandPoint"), point.id,
                       point.coordinate_wgs84});
    return result;
}

inline std::optional<CoordinateWGS84> targetPosition(
    const NetworkHydraulicDistribution &network, const MapAnnotationAnchor &anchor)
{
    std::optional<CoordinateWGS84> position;
    int matches = 0;
    QSet<QUuid> only;
    only.insert(anchor.entity_uuid);
    for (const Target &target : targets(network, only))
    {
        if (target.uuid != anchor.entity_uuid)
            continue;
        ++matches;
        if (target.type == anchor.entity_type)
            position = target.coordinate;
    }
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
    const NetworkHydraulicDistribution &network, const MapAnnotation &annotation)
{
    if (!annotation.anchor)
        return annotation.position_wgs84;
    const std::optional<CoordinateWGS84> target = targetPosition(network, *annotation.anchor);
    return target ? offsetFromTarget(*target, *annotation.anchor) : std::nullopt;
}

// Compute east/north offsets geodesically when a label is attached or dragged.
// A free-standing annotation stores the picked coordinate directly.
inline bool place(const NetworkHydraulicDistribution &network, MapAnnotation &annotation,
                  const CoordinateWGS84 &position)
{
    if (!std::isfinite(position.latitude_deg) || !std::isfinite(position.longitude_deg) ||
        position.latitude_deg < -90.0 || position.latitude_deg > 90.0 ||
        position.longitude_deg < -180.0 || position.longitude_deg > 180.0)
        return false;
    if (annotation.anchor)
    {
        const std::optional<CoordinateWGS84> target = targetPosition(network, *annotation.anchor);
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

// Rendering gets a resolved copy of each label; canonical anchors are never
// stripped from the stored records. Missing anchors are omitted fail-closed.
inline QList<MapAnnotation> drawable(const NetworkHydraulicDistribution &network,
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
    // One index per frame rather than scanning every node for every label.
    // A duplicate UUID is ambiguous and must never resolve an attachment.
    QSet<QUuid> needed;
    for (const MapAnnotation &annotation : annotations)
        if (annotation.anchor)
            needed.insert(annotation.anchor->entity_uuid);
    QHash<QUuid, Target> by_id;
    QSet<QUuid> ambiguous;
    for (const Target &target : targets(network, needed))
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
