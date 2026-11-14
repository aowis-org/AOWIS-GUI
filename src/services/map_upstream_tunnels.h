#ifndef MAP_UPSTREAM_TUNNELS_H
#define MAP_UPSTREAM_TUNNELS_H

#include <aowis/map/map_cascade_config.h>

#include <QHash>
#include <QObject>
#include <QStringList>

// Standalone: the WireGuard transports of the embedded MAP engine's
// cascading upstreams. Each referenced central profile gets one lease of its
// shared tunnel (WireGuardTunnelPool), so upstreams, the GUI's MAP server
// connection and DVMDB connections using the same profile share one tunnel.
// A failed start is retried after 5, 10, 20, 40, then every 60 seconds; a
// profile no longer referenced is released at once. Leases are reported to
// WireGuardRuntimeStatus (Settings -> WireGuard VPN -> Status, Used by now).
// Builds without the embedded engine never set a proxy, so MAP never
// contacts such upstreams (no direct fallback).
// A started tunnel is not proof that its peer answers: telemetry is sampled
// every 5 s (WireGuardPeerLiveness), and while the peer does not answer the
// proxy is withheld, so MAP skips these upstreams at once instead of letting
// every tile wait for a timeout. Every 15 s a TCP connection to one of the
// profile's upstreams is attempted through the tunnel, which makes WireGuard
// retry its handshake; the first answer restores the proxy. GUI thread only.
class MapUpstreamTunnels final : public QObject
{
    Q_OBJECT
public:
    static MapUpstreamTunnels &instance();

    // The cascade as configured; starts and releases tunnels to match it.
    void setCascade(const Aowis::Map::MapCascadeConfig &cascade);
    // That cascade with proxy set for every WireGuard upstream whose tunnel
    // runs; the others have none and are skipped by MAP.
    Aowis::Map::MapCascadeConfig resolved() const;

signals:
    // A tunnel started (resolved() changed). Not emitted by setCascade().
    void resolvedChanged();

private:
    explicit MapUpstreamTunnels(QObject *parent);
    class ProfileTunnel;
    void start(ProfileTunnel *tunnel);
    void sampleLiveness(ProfileTunnel *tunnel);
    void probe(ProfileTunnel *tunnel);
    void setReachable(ProfileTunnel *tunnel, bool reachable);

    Aowis::Map::MapCascadeConfig cascade;
    QHash<QString, ProfileTunnel *> tunnels; // By profile ID.
};

#endif // MAP_UPSTREAM_TUNNELS_H
