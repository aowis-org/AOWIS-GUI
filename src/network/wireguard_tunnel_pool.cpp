#include "network/wireguard_tunnel_pool.h"

#ifdef AOWIS_GUI_WIREGUARD_PROBE
#include "config/wireguard_profile_storage.h"
#include "network/wireguard_runtime_status.h"

#include <aowis/network/wireguard_client.h>
#include <aowis/network/wireguard_conf.h>

#include <QCoreApplication>
#include <QFutureWatcher>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPair>
#include <QPointer>
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

namespace
{
QString profileName(const QString &profile_id)
{
    for (const WireGuardProfileEntry &profile : listWireGuardProfiles())
        if (profile.id == profile_id) return profile.name;
    return profile_id;
}

// Fails a request for a profile the user stopped, and reports it.
WireGuardTunnelLease suspendedLease(const QString &profile_id)
{
    WireGuardRuntimeStatus::instance().reportSuspendedRequest(profile_id);
    return {nullptr, WireGuardRuntimeStatus::suspendedError(profileName(profile_id))};
}
}

void acquireWireGuardTunnel(const QString &profile_id, std::function<void(const WireGuardTunnelLease &)> done)
{
    if (WireGuardRuntimeStatus::instance().isSuspended(profile_id))
    {
        // Never started while stopped by the user; never a direct fallback.
        QTimer::singleShot(0, QCoreApplication::instance(), [profile_id, done]() {
            done(suspendedLease(profile_id));
        });
        return;
    }
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
        WireGuardTunnelLease result = watcher->result();
        watcher->deleteLater();
        if (WireGuardRuntimeStatus::instance().isSuspended(profile_id))
        {
            // Stopped while starting: do not hand the tunnel out.
            if (result.tunnel) result.tunnel->stop();
            result = suspendedLease(profile_id);
        }
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

namespace
{
QList<QPair<QPointer<QObject>, std::function<void(const QString &)>>> &restartObservers()
{
    static QList<QPair<QPointer<QObject>, std::function<void(const QString &)>>> observers;
    return observers;
}
}

void onWireGuardTunnelRestart(QObject *context, std::function<void(const QString &)> restarted)
{
    QList<QPair<QPointer<QObject>, std::function<void(const QString &)>>> &observers = restartObservers();
    for (qsizetype index = observers.size() - 1; index >= 0; --index)
        if (observers.at(index).first.isNull()) observers.removeAt(index);
    observers.append(QPair<QPointer<QObject>, std::function<void(const QString &)>>(
        QPointer<QObject>(context), std::move(restarted)));
}

namespace
{
// Stops the running tunnel (if any) before anyone acquires again: two tunnels
// with the same key must never run at once (the peer would roam).
bool stopRunningTunnel(const QString &profile_id)
{
    QHash<QString, SharedTunnel>::iterator entry = sharedTunnels().find(profile_id);
    if (entry == sharedTunnels().end()) return false;
    const std::shared_ptr<aowis::network::WireGuardClient> live = entry->tunnel.lock();
    entry->tunnel.reset();
    if (!live || !live->isRunning()) return false;
    live->stop();
    return true;
}

void notifyRestartObservers(const QString &profile_id)
{
    const QList<QPair<QPointer<QObject>, std::function<void(const QString &)>>> observers = restartObservers();
    for (const QPair<QPointer<QObject>, std::function<void(const QString &)>> &observer : observers)
        if (!observer.first.isNull()) observer.second(profile_id);
}
}

void restartWireGuardTunnel(const QString &profile_id)
{
    if (WireGuardRuntimeStatus::instance().isSuspended(profile_id)) return;
    if (stopRunningTunnel(profile_id)) notifyRestartObservers(profile_id);
}

void stopWireGuardTunnel(const QString &profile_id)
{
    if (WireGuardRuntimeStatus::instance().isSuspended(profile_id)) return;
    WireGuardRuntimeStatus::instance().setSuspended(profile_id, true);
    stopRunningTunnel(profile_id);
    // Consumers drop the stopped tunnel; their next attempts are refused
    // with the "stopped" error (never direct).
    notifyRestartObservers(profile_id);
}

void startWireGuardTunnel(const QString &profile_id)
{
    if (!WireGuardRuntimeStatus::instance().isSuspended(profile_id)) return;
    WireGuardRuntimeStatus::instance().setSuspended(profile_id, false);
    // Consumers acquire again; the first starts the one shared tunnel.
    notifyRestartObservers(profile_id);
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
