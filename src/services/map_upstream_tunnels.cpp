#include "services/map_upstream_tunnels.h"

#ifdef AOWIS_GUI_WIREGUARD_PROBE
#include "network/wireguard_peer_liveness.h"
#include "network/wireguard_runtime_status.h"
#include "network/wireguard_tunnel_pool.h"

#include <aowis/network/wireguard_client.h>

#include <QDateTime>
#include <QDebug>
#include <QPointer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#endif

#include <QCoreApplication>

// One profile's lease and retry state; the owner reported to
// WireGuardRuntimeStatus.
class MapUpstreamTunnels::ProfileTunnel final : public QObject
{
public:
    ProfileTunnel(const QString &profile_id, QObject *parent)
        : QObject(parent), profile_id(profile_id)
    {
#ifdef AOWIS_GUI_WIREGUARD_PROBE
        this->retry.setSingleShot(true);
        this->liveness_timer.setInterval(5000);
        this->probe_timer.setInterval(15000);
#endif
    }
#ifdef AOWIS_GUI_WIREGUARD_PROBE
    ~ProfileTunnel() override
    {
        WireGuardRuntimeStatus::instance().stopped(this);
    }

    // Drops the lease and all liveness state (released or restarted tunnel).
    void releaseLease()
    {
        this->liveness_timer.stop();
        this->probe_timer.stop();
        if (this->probe_socket != nullptr)
        {
            this->probe_socket->abort();
            this->probe_socket->deleteLater();
            this->probe_socket = nullptr;
        }
        this->liveness.reset();
        this->reachable = true;
        this->lease = WireGuardTunnelLease();
    }

    WireGuardTunnelLease lease;
    QTimer retry;
    int failures = 0;
    // Peer liveness of the running tunnel; see MapUpstreamTunnels.
    WireGuardPeerLiveness liveness;
    bool reachable = true;
    QTimer liveness_timer;
    QTimer probe_timer;
    QPointer<QTcpSocket> probe_socket;
#endif
    const QString profile_id;
    QStringList upstream_names;
    QList<QUrl> upstream_urls; // Probe targets, in cascade order.
};

MapUpstreamTunnels &MapUpstreamTunnels::instance()
{
    static MapUpstreamTunnels *tunnels = new MapUpstreamTunnels(QCoreApplication::instance());
    return *tunnels;
}

MapUpstreamTunnels::MapUpstreamTunnels(QObject *parent)
    : QObject(parent)
{
#ifdef AOWIS_GUI_WIREGUARD_PROBE
    // An edited profile: the old tunnel is stopped; MAP skips the upstreams
    // (and cancels their downloads) until the new one runs.
    onWireGuardTunnelRestart(this, [this](const QString &profile_id)
    {
        ProfileTunnel *tunnel = this->tunnels.value(profile_id);
        if (tunnel == nullptr) return;
        tunnel->releaseLease();
        tunnel->retry.stop();
        tunnel->failures = 0;
        emit this->resolvedChanged();
        this->start(tunnel);
    });
#endif
}

void MapUpstreamTunnels::setCascade(const Aowis::Map::MapCascadeConfig &cascade)
{
    this->cascade = cascade;
    QHash<QString, QStringList> referenced;
    QHash<QString, QList<QUrl>> referenced_urls;
    for (const Aowis::Map::MapCascadeUpstream &upstream : cascade.upstreams)
        if (Aowis::Map::requiresWireGuardTransport(upstream))
        {
            referenced[upstream.wireguard_profile_id].append(upstream.name);
            referenced_urls[upstream.wireguard_profile_id].append(upstream.url);
        }
    for (QHash<QString, ProfileTunnel *>::iterator tunnel = this->tunnels.begin();
         tunnel != this->tunnels.end();)
    {
        if (referenced.contains(tunnel.key()))
        {
            tunnel.value()->upstream_names = referenced.value(tunnel.key());
            tunnel.value()->upstream_urls = referenced_urls.value(tunnel.key());
            ++tunnel;
            continue;
        }
        delete tunnel.value(); // Releases its lease.
        tunnel = this->tunnels.erase(tunnel);
    }
    for (QHash<QString, QStringList>::const_iterator profile = referenced.constBegin();
         profile != referenced.constEnd(); ++profile)
    {
        if (this->tunnels.contains(profile.key())) continue;
        ProfileTunnel *tunnel = new ProfileTunnel(profile.key(), this);
        tunnel->upstream_names = profile.value();
        tunnel->upstream_urls = referenced_urls.value(profile.key());
#ifdef AOWIS_GUI_WIREGUARD_PROBE
        connect(&tunnel->liveness_timer, &QTimer::timeout, this, [this, tunnel]()
        {
            sampleLiveness(tunnel);
        });
        connect(&tunnel->probe_timer, &QTimer::timeout, this, [this, tunnel]()
        {
            probe(tunnel);
        });
#endif
        this->tunnels.insert(profile.key(), tunnel);
        start(tunnel);
    }
}

void MapUpstreamTunnels::start(ProfileTunnel *tunnel)
{
#ifdef AOWIS_GUI_WIREGUARD_PROBE
    WireGuardRuntimeStatus::instance().starting(tunnel->profile_id, tunnel,
        tr("MAP upstream (%1)").arg(tunnel->upstream_names.join(QStringLiteral(", "))));
    const QPointer<MapUpstreamTunnels> self(this);
    const QPointer<QObject> owner(tunnel);
    acquireWireGuardTunnel(tunnel->profile_id, [self, owner, tunnel](const WireGuardTunnelLease &result)
    {
        // A released profile drops the result (and with it the lease).
        if (!self || !owner) return;
        if (result.tunnel)
        {
            tunnel->releaseLease();
            tunnel->lease = result;
            tunnel->failures = 0;
            tunnel->liveness_timer.start();
            WireGuardRuntimeStatus::instance().running(tunnel, wireGuardTelemetrySampler(result.tunnel));
            emit self->resolvedChanged();
            return;
        }
        const QString error = result.error.isEmpty()
            ? self->tr("WireGuard tunnel failed to start.") : result.error;
        WireGuardRuntimeStatus::instance().failed(tunnel, error);
        ++tunnel->failures;
        qWarning().noquote() << "MAP upstream WireGuard tunnel failed to start; retrying:" << error;
        QObject::disconnect(&tunnel->retry, nullptr, nullptr, nullptr);
        QObject::connect(&tunnel->retry, &QTimer::timeout, self, [self, tunnel]()
        {
            self->start(tunnel);
        });
        tunnel->retry.start(std::min(60000, 5000 << std::min(tunnel->failures - 1, 4)));
    });
#else
    Q_UNUSED(tunnel)
#endif
}

void MapUpstreamTunnels::sampleLiveness(ProfileTunnel *tunnel)
{
#ifdef AOWIS_GUI_WIREGUARD_PROBE
    if (!tunnel->lease.tunnel) return;
    aowis::network::WireGuardTelemetry sample;
    if (!tunnel->lease.tunnel->telemetry(&sample)) return;
    const bool unresponsive = tunnel->liveness.update(
        QDateTime::currentMSecsSinceEpoch(), sample.lastHandshakeUnixMs,
        sample.sentBytes);
    setReachable(tunnel, !unresponsive);
#else
    Q_UNUSED(tunnel)
#endif
}

void MapUpstreamTunnels::probe(ProfileTunnel *tunnel)
{
#ifdef AOWIS_GUI_WIREGUARD_PROBE
    if (!tunnel->lease.tunnel || tunnel->reachable || tunnel->upstream_urls.isEmpty() ||
        tunnel->probe_socket != nullptr)
        return;
    // Any TCP connection into the tunnel makes WireGuard initiate a
    // handshake; connecting to the upstream itself also proves it listens.
    // Never sent anywhere but through this tunnel's own proxy.
    const QUrl url = tunnel->upstream_urls.first();
    const quint16 port = quint16(url.port(url.scheme() == QStringLiteral("https") ? 443 : 80));
    QTcpSocket *socket = new QTcpSocket(tunnel);
    socket->setProxy(wireGuardQtProxy(*tunnel->lease.tunnel));
    tunnel->probe_socket = socket;
    const QPointer<MapUpstreamTunnels> self(this);
    const QPointer<ProfileTunnel> owner(tunnel);
    connect(socket, &QTcpSocket::connected, this, [self, owner, socket]()
    {
        socket->abort();
        socket->deleteLater();
        if (!self || !owner) return;
        owner->probe_socket = nullptr;
        owner->liveness.reset();
        self->setReachable(owner, true);
    });
    connect(socket, &QTcpSocket::errorOccurred, this, [owner, socket](QAbstractSocket::SocketError)
    {
        socket->deleteLater();
        if (owner && owner->probe_socket == socket) owner->probe_socket = nullptr;
    });
    // Bounded like an upstream request; the next probe starts a new attempt.
    // abort() reports no error, so the socket is released here as well.
    QTimer::singleShot(10000, socket, [socket]()
    {
        socket->abort();
        socket->deleteLater();
    });
    socket->connectToHost(url.host(), port);
#else
    Q_UNUSED(tunnel)
#endif
}

void MapUpstreamTunnels::setReachable(ProfileTunnel *tunnel, bool reachable)
{
#ifdef AOWIS_GUI_WIREGUARD_PROBE
    if (tunnel->reachable == reachable) return;
    tunnel->reachable = reachable;
    if (reachable)
    {
        tunnel->probe_timer.stop();
        qInfo().noquote() << "MAP upstream WireGuard peer answers again; using"
                          << tunnel->upstream_names.join(QStringLiteral(", "));
    }
    else
    {
        tunnel->probe_timer.start();
        qWarning().noquote() << "MAP upstream WireGuard peer does not answer; skipping"
                             << tunnel->upstream_names.join(QStringLiteral(", "))
                             << "until it does";
    }
    emit resolvedChanged();
#else
    Q_UNUSED(tunnel)
    Q_UNUSED(reachable)
#endif
}

Aowis::Map::MapCascadeConfig MapUpstreamTunnels::resolved() const
{
    Aowis::Map::MapCascadeConfig result = this->cascade;
#ifdef AOWIS_GUI_WIREGUARD_PROBE
    for (Aowis::Map::MapCascadeUpstream &upstream : result.upstreams)
    {
        if (!Aowis::Map::requiresWireGuardTransport(upstream)) continue;
        const ProfileTunnel *tunnel = this->tunnels.value(upstream.wireguard_profile_id);
        // An unresponsive peer: no proxy, so MAP skips the upstream at once.
        if (tunnel != nullptr && tunnel->lease.tunnel && tunnel->reachable)
            upstream.proxy = wireGuardQtProxy(*tunnel->lease.tunnel);
    }
#endif
    return result;
}
