#ifndef MAP_CORE_MAP_ANNOTATION_DISTRIBUTION_TARGETS_H
#define MAP_CORE_MAP_ANNOTATION_DISTRIBUTION_TARGETS_H

#include "map/core/map_annotation_position.h"
#include <aowis/model/hydraulic/network_hydraulic_distribution.h>

// Distribution-only adapter. Other engineering domains provide their own
// typed target lists to the shared position engine, never hydraulic entities.
namespace MapAnnotationPosition
{
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
    QSet<QUuid> only;
    only.insert(anchor.entity_uuid);
    return targetPosition(targets(network, only), anchor);
}

inline std::optional<CoordinateWGS84> resolve(
    const NetworkHydraulicDistribution &network, const MapAnnotation &annotation)
{
    if (!annotation.anchor)
        return annotation.position_wgs84;
    return resolve(targets(network, QSet<QUuid>{annotation.anchor->entity_uuid}),
                   annotation);
}

inline bool place(const NetworkHydraulicDistribution &network, MapAnnotation &annotation,
                  const CoordinateWGS84 &position)
{
    if (!annotation.anchor)
        return place(QList<Target>{}, annotation, position);
    return place(targets(network, QSet<QUuid>{annotation.anchor->entity_uuid}),
                 annotation, position);
}

inline QList<MapAnnotation> drawable(const NetworkHydraulicDistribution &network,
                                      const QList<MapAnnotation> &annotations)
{
    // Preserve the O(number of anchors + relevant points) Distribution path;
    // only the generic overload performs the actual resolution.
    QSet<QUuid> needed;
    for (const MapAnnotation &annotation : annotations)
        if (annotation.anchor)
            needed.insert(annotation.anchor->entity_uuid);
    if (needed.isEmpty())
        return annotations;
    return drawable(targets(network, needed), annotations);
}

} // namespace MapAnnotationPosition

#endif // MAP_CORE_MAP_ANNOTATION_DISTRIBUTION_TARGETS_H
