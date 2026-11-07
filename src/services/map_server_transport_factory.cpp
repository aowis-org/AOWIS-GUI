#include "services/map_server_transport_factory.h"

#include "config/map_server_client_configuration.h"
#include "services/interface_server_map_rest.h"
#ifdef AOWIS_STANDALONE
#include "services/interface_server_map_standalone.h"
#endif

#include <QObject>

MapServerRestConfiguration guiMapServerRestConfiguration()
{
    const MapServerClientConfiguration &configuration = mapServerClientConfiguration();
    MapServerRestConfiguration server;
    server.base_url = configuration.base_url;
    server.api_key = configuration.api_key;
    server.delete_api_key = configuration.delete_api_key;
    return server;
}

MapServerTransport guiMapServerTransport()
{
    MapServerTransport transport;
#ifdef AOWIS_STANDALONE
    transport.create = [](QObject *parent) -> InterfaceServerMap *
    {
        return new InterfaceServerMapStandalone(parent);
    };
#ifdef Q_OS_WIN
    // The embedded map client uses one QNetworkAccessManager. Qt's desktop
    // HTTP/1 implementation executes at most six requests concurrently per
    // host/port, so keeping more than six direct Windows requests "in flight"
    // only hides them inside Qt's network queue and makes them susceptible to
    // transfer timeouts before useful work can start. Keep the excess in the
    // tile repository instead, where viewport/batch priority can still
    // reorder it.
    transport.tile_requests_in_flight = 6;
    transport.background_tile_requests_in_flight = 2;
#endif
#else
    transport.create = [](QObject *parent) -> InterfaceServerMap *
    {
        return new InterfaceServerMapREST(guiMapServerRestConfiguration(), parent);
    };
#endif
    return transport;
}
