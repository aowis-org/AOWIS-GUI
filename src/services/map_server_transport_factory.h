#ifndef MAP_SERVER_TRANSPORT_FACTORY_H
#define MAP_SERVER_TRANSPORT_FACTORY_H

#include "map/data/map_server_transport.h"
#include "services/map_server_rest_configuration.h"

// The GUI's map server transport for the map data repositories (Render):
// the in-process map server in the Standalone build, REST over HTTP
// otherwise. On Windows Standalone the tile requests in flight follow Qt's
// HTTP/1 per-host concurrency (six), see the definition.
MapServerTransport guiMapServerTransport();
// The map server of the GUI configuration (mapServerClientConfiguration()).
MapServerRestConfiguration guiMapServerRestConfiguration();

#endif // MAP_SERVER_TRANSPORT_FACTORY_H
