#pragma once

// Shared embedded WireGuard tunnels of the GUI, one per stored profile, for
// every service that reaches its server through WireGuard (DVMDB connections,
// the map server). Builds with the embedded engine only
// (AOWIS_GUI_WIREGUARD_PROBE); elsewhere this header declares nothing.
#ifdef AOWIS_GUI_WIREGUARD_PROBE

#include "network/wireguard_runtime_status.h"

#include <QNetworkProxy>
#include <QString>

#include <functional>
#include <memory>

namespace aowis::network { class WireGuardClient; }

struct WireGuardTunnelLease
{
    std::shared_ptr<aowis::network::WireGuardClient> tunnel; // null: failed
    QString error;
};

// One tunnel per profile, shared by every consumer using it. Required for
// correctness, not just economy: two tunnels with the same private key make
// the peer roam its endpoint to whichever handshook last, so replies land in
// the other tunnel's netstack. Each consumer keeps its own HTTP client over
// the shared proxy and holds the lease's shared_ptr; the last release stops
// the tunnel. Profile edits take effect when the tunnel is next started.
// done() is always called later from the event loop (never re-entrantly).
// A failed start never falls back to direct HTTP. GUI thread only.
void acquireWireGuardTunnel(const QString &profile_id, std::function<void(const WireGuardTunnelLease &)> done);

// Telemetry sampler for WireGuardRuntimeStatus::running(). Holds the tunnel
// weakly: the monitor never extends a tunnel's lifetime.
WireGuardRuntimeStatus::TelemetrySampler wireGuardTelemetrySampler(
    const std::shared_ptr<aowis::network::WireGuardClient> &tunnel);

// The tunnel's authenticated local SOCKS5 proxy for one QNetworkAccessManager
// (never the application proxy). Host names are resolved inside the tunnel.
QNetworkProxy wireGuardQtProxy(const aowis::network::WireGuardClient &tunnel);

#endif // AOWIS_GUI_WIREGUARD_PROBE
