#pragma once

// Shared embedded WireGuard tunnels of the GUI, one per stored profile, for
// every service that reaches its server through WireGuard (DVMDB connections,
// the map server). Builds with the embedded engine only
// (AOWIS_GUI_WIREGUARD_PROBE); elsewhere this header declares nothing.
#ifdef AOWIS_GUI_WIREGUARD_PROBE

#include "network/wireguard_runtime_status.h"

#include <QNetworkProxy>
#include <QObject>
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
// the tunnel. Saving an edited profile restarts its running tunnel once
// (restartWireGuardTunnel below).
// done() is always called later from the event loop (never re-entrantly).
// A failed start never falls back to direct HTTP. GUI thread only.
void acquireWireGuardTunnel(const QString &profile_id, std::function<void(const WireGuardTunnelLease &)> done);

// Telemetry sampler for WireGuardRuntimeStatus::running(). Holds the tunnel
// weakly: the monitor never extends a tunnel's lifetime.
WireGuardRuntimeStatus::TelemetrySampler wireGuardTelemetrySampler(
    const std::shared_ptr<aowis::network::WireGuardClient> &tunnel);

// Applies an edited profile to its running tunnel without duplicating it:
// the running tunnel is stopped first (its consumers' requests fail), then
// every observer registered with onWireGuardTunnelRestart() is told to drop
// its lease and acquire again, which starts one tunnel from the stored
// profile. Nothing happens while the profile has no running tunnel (the next
// start reads the stored profile anyway). GUI thread only.
void restartWireGuardTunnel(const QString &profile_id);
// restarted(profile_id) after restartWireGuardTunnel() while context exists.
void onWireGuardTunnelRestart(QObject *context, std::function<void(const QString &)> restarted);

// The tunnel's authenticated local SOCKS5 proxy for one QNetworkAccessManager
// (never the application proxy). Host names are resolved inside the tunnel.
QNetworkProxy wireGuardQtProxy(const aowis::network::WireGuardClient &tunnel);

#endif // AOWIS_GUI_WIREGUARD_PROBE
