#ifndef MAP_SERVER_CLIENT_CONFIGURATION_H
#define MAP_SERVER_CLIENT_CONFIGURATION_H

#include <QByteArray>
#include <QString>

#include <functional>

class QObject;

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
// The current configuration (GUI thread only). It changes when
// saveMapServerClientConfiguration() stores a different one; observers
// registered with onMapServerClientConfigurationChanged() then reconnect
// without restarting AOWIS.
const MapServerClientConfiguration &mapServerClientConfiguration();
// Reads the stored configuration afresh (not the current one).
MapServerClientConfiguration readMapServerClientConfiguration();
// changed() is called after every change of the current configuration while
// context exists. GUI thread only.
void onMapServerClientConfigurationChanged(QObject *context, std::function<void()> changed);
// Persists the configuration (updates the map-server record, or creates it),
// makes the stored record current and notifies the observers when it
// differs. Returns false if the write failed or the build cannot store it
// (WebAssembly); the current configuration is then unchanged.
bool saveMapServerClientConfiguration(const MapServerClientConfiguration &configuration);

#endif // MAP_SERVER_CLIENT_CONFIGURATION_H
