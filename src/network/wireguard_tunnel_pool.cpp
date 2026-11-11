#include "network/wireguard_tunnel_pool.h"

#ifdef AOWIS_GUI_WIREGUARD_PROBE
#include "config/wireguard_profile_storage.h"

#include <aowis/network/wireguard_client.h>
#include <aowis/network/wireguard_conf.h>

#include <QCoreApplication>
#include <QFutureWatcher>
#include <QHash>
#include <QList>
#include <QObject>
#include <QTimer>
#include <QtConcurrent/qtconcurrentrun.h>

#include <string>
#include <utility>

namespace
{
// Runs on a worker thread. The profile text comes from the GUI's
// AOWIS-CONFIG store and is parsed by AOWIS-NETWORK's single .conf parser,
// which accepts exactly what the embedded engine can run. Any failure keeps
// the connection closed: there is never a fallback to direct HTTP.
WireGuardTunnelLease startTunnel(const QString &profile_id)
{
    std::string text;
    QString error;
    if (!readWireGuardProfileText(profile_id, text, &error))
        return {{}, error};
    const aowis::network::WireGuardConfParse parsed = aowis::network::parseWireGuardConf(text);
    if (!parsed.ok())
    {
        // Diagnostics never contain profile text or keys.
        for (const aowis::network::WireGuardConfDiagnostic &diagnostic : parsed.diagnostics)
            if (diagnostic.severity == aowis::network::WireGuardConfSeverity::error)
                return {{}, diagnostic.line > 0
                    ? QObject::tr("The WireGuard profile cannot be used (line %1): %2")
                          .arg(static_cast<qulonglong>(diagnostic.line)).arg(QString::fromStdString(diagnostic.message))
                    : QObject::tr("The WireGuard profile cannot be used: %1")
                          .arg(QString::fromStdString(diagnostic.message))};
        return {{}, QObject::tr("The WireGuard profile cannot be used.")};
    }
    std::shared_ptr<aowis::network::WireGuardClient> tunnel = std::make_shared<aowis::network::WireGuardClient>();
    std::string start_error;
    if (!tunnel->start(parsed.config, &start_error))
        return {{}, QString::fromStdString(start_error)};
    return {tunnel, {}};
}

}

WireGuardRuntimeStatus::TelemetrySampler wireGuardTelemetrySampler(
    const std::shared_ptr<aowis::network::WireGuardClient> &tunnel)
{
    const std::weak_ptr<aowis::network::WireGuardClient> weak = tunnel;
    return [weak](WireGuardRuntimeStatus::TunnelTelemetry &out) -> bool
    {
        const std::shared_ptr<aowis::network::WireGuardClient> locked = weak.lock();
        if (!locked) return false;
        aowis::network::WireGuardTelemetry sample;
        if (!locked->telemetry(&sample)) return false;
        out.started_unix_ms = static_cast<qint64>(sample.startedUnixMs);
        out.last_handshake_unix_ms = static_cast<qint64>(sample.lastHandshakeUnixMs);
        out.received_bytes = static_cast<quint64>(sample.receivedBytes);
        out.sent_bytes = static_cast<quint64>(sample.sentBytes);
        out.persistent_keepalive_seconds = static_cast<quint64>(sample.persistentKeepaliveSeconds);
        out.open_relays = static_cast<quint64>(sample.activeProxyConnections);
        out.accepted_relays = static_cast<quint64>(sample.acceptedProxyConnections);
        out.failed_dials = static_cast<quint64>(sample.failedProxyDials);
        out.peer_endpoint = QString::fromStdString(sample.peerEndpoint);
        return true;
    };
}

namespace
{
struct SharedTunnel
{
    std::weak_ptr<aowis::network::WireGuardClient> tunnel;
    bool starting = false;
    QList<std::function<void(const WireGuardTunnelLease &)>> waiters;
};

QHash<QString, SharedTunnel> &sharedTunnels()
{
    static QHash<QString, SharedTunnel> tunnels;
    return tunnels;
}

}

void acquireWireGuardTunnel(const QString &profile_id, std::function<void(const WireGuardTunnelLease &)> done)
{
    SharedTunnel &entry = sharedTunnels()[profile_id];
    const std::shared_ptr<aowis::network::WireGuardClient> live = entry.tunnel.lock();
    if (live && live->isRunning())
    {
        // Delivered from the event loop like a fresh start, so callers are
        // never re-entered from inside open().
        QTimer::singleShot(0, QCoreApplication::instance(), [live, done]() {
            done({live, {}});
        });
        return;
    }
    entry.waiters.append(std::move(done));
    if (entry.starting) return; // Join the start already in flight.
    entry.starting = true;
    QFutureWatcher<WireGuardTunnelLease> *watcher = new QFutureWatcher<WireGuardTunnelLease>();
    QObject::connect(watcher, &QFutureWatcher<WireGuardTunnelLease>::finished, watcher, [watcher, profile_id]() {
        const WireGuardTunnelLease result = watcher->result();
        watcher->deleteLater();
        QList<std::function<void(const WireGuardTunnelLease &)>> waiters;
        QHash<QString, SharedTunnel>::iterator it = sharedTunnels().find(profile_id);
        if (it != sharedTunnels().end())
        {
            it->starting = false;
            it->tunnel = result.tunnel; // Weak: alive only while a consumer holds it.
            waiters.swap(it->waiters);
        }
        for (const std::function<void(const WireGuardTunnelLease &)> &waiter : waiters) waiter(result);
        // If no waiter kept the tunnel, `result` releases it here and it stops.
    });
    watcher->setFuture(QtConcurrent::run(startTunnel, profile_id));
}

QNetworkProxy wireGuardQtProxy(const aowis::network::WireGuardClient &tunnel)
{
    // Same settings as AOWIS-NETWORK's QtSocks5HttpClient: TCP tunnelling
    // and remote (in-tunnel) host name lookup only.
    const aowis::network::Socks5Proxy socks = tunnel.proxy();
    QNetworkProxy proxy(QNetworkProxy::Socks5Proxy, QString::fromStdString(socks.host), socks.port,
                        QString::fromStdString(socks.username), QString::fromStdString(socks.password));
    proxy.setCapabilities(QNetworkProxy::TunnelingCapability | QNetworkProxy::HostNameLookupCapability);
    return proxy;
}

#endif // AOWIS_GUI_WIREGUARD_PROBE
