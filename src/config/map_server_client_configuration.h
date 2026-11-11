#ifndef MAP_SERVER_CLIENT_CONFIGURATION_H
#define MAP_SERVER_CLIENT_CONFIGURATION_H

#include <QByteArray>
#include <QString>

struct MapServerClientConfiguration
{
    QString base_url;
    QByteArray api_key;
    QByteArray delete_api_key;
    // Stored WireGuard profile (config/wireguard_profile_storage.h) the map
    // server is reached through; empty: direct HTTP. Never a direct fallback.
    QString wireguard_profile_id;
};

// Native builds: the map-server connection record of the GUI's AOWIS-CONFIG
// "connections" collection (service_type "map-server", connection_version 1),
// private to the user like the WireGuard profiles; API keys never go to the
// GUI INI. Without a record the defaults are http://aowis-server-map.localhost:80
// and no keys. transport is {"type":"direct"} or {"type":"wireguard",
// "profile_id":...} (as DVMDB connections); any other transport, or a
// wireguard one without a profile, fails closed (empty base URL). WebAssembly: globalThis.aowisMapServerConfiguration of the
// hosting page; nothing is stored in the browser.
//
// Read once and cached for the process lifetime.
const MapServerClientConfiguration &mapServerClientConfiguration();
// Reads the stored configuration afresh (not cached).
MapServerClientConfiguration readMapServerClientConfiguration();
// Persists the configuration (updates the map-server record, or creates it),
// but does NOT update the cached configuration returned by
// mapServerClientConfiguration() -- the map server client and REST/tile
// fetch code read that once at construction, so a restart is required for
// a saved change to actually take effect. Returns false if the write
// failed or the build cannot store it (WebAssembly).
bool saveMapServerClientConfiguration(const MapServerClientConfiguration &configuration);

#endif // MAP_SERVER_CLIENT_CONFIGURATION_H
