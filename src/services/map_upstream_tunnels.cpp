#include "services/map_upstream_tunnels.h"

#ifdef AOWIS_GUI_WIREGUARD_PROBE
#include "network/wireguard_runtime_status.h"
#include "network/wireguard_tunnel_pool.h"

#include <aowis/network/wireguard_client.h>

#include <QDebug>
#include <QPointer>
#include <QTimer>

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
#endif
    }
#ifdef AOWIS_GUI_WIREGUARD_PROBE
    ~ProfileTunnel() override
    {
        WireGuardRuntimeStatus::instance().stopped(this);
    }

    WireGuardTunnelLease lease;
    QTimer retry;
    int failures = 0;
#endif
    const QString profile_id;
    QStringList upstream_names;
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
        tunnel->lease = WireGuardTunnelLease();
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
    for (const Aowis::Map::MapCascadeUpstream &upstream : cascade.upstreams)
        if (Aowis::Map::requiresWireGuardTransport(upstream))
            referenced[upstream.wireguard_profile_id].append(upstream.name);
    for (QHash<QString, ProfileTunnel *>::iterator tunnel = this->tunnels.begin();
         tunnel != this->tunnels.end();)
    {
        if (referenced.contains(tunnel.key()))
        {
            tunnel.value()->upstream_names = referenced.value(tunnel.key());
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
            tunnel->lease = result;
            tunnel->failures = 0;
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

Aowis::Map::MapCascadeConfig MapUpstreamTunnels::resolved() const
{
    Aowis::Map::MapCascadeConfig result = this->cascade;
#ifdef AOWIS_GUI_WIREGUARD_PROBE
    for (Aowis::Map::MapCascadeUpstream &upstream : result.upstreams)
    {
        if (!Aowis::Map::requiresWireGuardTransport(upstream)) continue;
        const ProfileTunnel *tunnel = this->tunnels.value(upstream.wireguard_profile_id);
        if (tunnel != nullptr && tunnel->lease.tunnel)
            upstream.proxy = wireGuardQtProxy(*tunnel->lease.tunnel);
    }
#endif
    return result;
}
