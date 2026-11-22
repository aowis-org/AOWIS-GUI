#pragma once

#include <aowis/model/hydraulic/network_hydraulic_distribution.h>
#include <aowis/model/map/map_annotation.h>

#include <QList>
#include <QSet>
#include <QUuid>

// The current network's map annotations, ready for EPANET INP export: an
// annotation whose anchor no longer exists (its node was deleted after the
// import) is exported free at its last stored position_wgs84. Canonical
// editing refuses to remove a referenced entity until the user detaches its
// annotation, since mixed hydraulic/annotation transactions are unsupported.
// Export still handles inconsistent external imports without mutating state.
inline QList<MapAnnotation> exportableMapAnnotations(
    const NetworkHydraulicDistribution &network, const QList<MapAnnotation> &annotations)
{
    QSet<QUuid> targets;
    for (const HydraulicNodeJunction &node : network.nodes_junctions) targets.insert(node.uuid);
    for (const HydraulicNodeReservoir &node : network.nodes_reservoirs) targets.insert(node.uuid);
    for (const HydraulicNodeTank &node : network.nodes_tanks) targets.insert(node.uuid);
    for (const HydraulicDemandPoint &point : network.demand_points) targets.insert(point.uuid);
    QList<MapAnnotation> exported = annotations;
    for (MapAnnotation &annotation : exported)
        if (annotation.anchor && !targets.contains(annotation.anchor->entity_uuid))
            annotation.anchor.reset();
    return exported;
}
