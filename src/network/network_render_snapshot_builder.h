#ifndef NETWORK_RENDER_SNAPSHOT_BUILDER_H
#define NETWORK_RENDER_SNAPSHOT_BUILDER_H

#include "network/network_render_snapshot.h"

#include <aowis/model/hydraulic/network_hydraulic_distribution.h>

NetworkRenderSnapshot buildNetworkRenderSnapshot(const NetworkHydraulicDistribution &network,
                                                 quint64 geometry_revision,
                                                 quint64 visual_revision);

#endif // NETWORK_RENDER_SNAPSHOT_BUILDER_H
