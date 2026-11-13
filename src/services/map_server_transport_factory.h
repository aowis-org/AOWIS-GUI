#ifndef MAP_SERVER_TRANSPORT_FACTORY_H
#define MAP_SERVER_TRANSPORT_FACTORY_H

#include "map/data/map_server_transport.h"
#include "services/map_server_rest_configuration.h"

#include <functional>

class QObject;

// The GUI's map server transport for the map data repositories (Render):
// the in-process map server in the Standalone build, REST over HTTP
// otherwise -- directly, or through the configured WireGuard profile's shared
// tunnel (requests wait until it runs; never a direct fallback). The REST
// transport follows changes of the map server connection
// (saveMapServerClientConfiguration) without a restart. On Windows
// Standalone the tile requests in flight follow Qt's HTTP/1 per-host
// concurrency (six), see the definition.
MapServerTransport guiMapServerTransport();
// The map server of the current GUI configuration (mapServerClientConfiguration()),
// without a proxy: for direct connections only. Consumers that create their
// own network managers use whenGuiMapServerReachable() instead.
MapServerRestConfiguration guiMapServerRestConfiguration();
// Calls reachable() with the REST configuration to use: at once (from the
// event loop) for a direct connection, after the map server's WireGuard
// tunnel runs otherwise, with the tunnel's proxy set -- and again after every
// change of the map server connection, so consumers reconnect live (they
// must replace what an earlier call set up). Not called while the tunnel
// cannot start (retried with backoff), nor once context is destroyed.
void whenGuiMapServerReachable(QObject *context,
                               std::function<void(const MapServerRestConfiguration &)> reachable);

#endif // MAP_SERVER_TRANSPORT_FACTORY_H
