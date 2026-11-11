#include "services/map_server_transport_factory.h"

#include "config/map_server_client_configuration.h"
#include "services/interface_server_map_rest.h"
#ifdef AOWIS_STANDALONE
#include "services/interface_server_map_standalone.h"
#endif
#ifdef AOWIS_GUI_WIREGUARD_PROBE
#include "network/wireguard_runtime_status.h"
#include "network/wireguard_tunnel_pool.h"

#include <aowis/network/wireguard_client.h>

#include <QList>
#include <QPair>
#include <QTimer>

#include <algorithm>
#include <memory>
#endif

#include <QCoreApplication>
#include <QDebug>
#include <QObject>
#include <QPointer>
#include <QTimer>

#include <utility>

namespace
{
#ifdef AOWIS_GUI_WIREGUARD_PROBE
// The map server's WireGuard tunnel: started on first use and then kept for
// the process lifetime (the map configuration is read once; a change needs a
// restart), since tiles, terrain, the catalog and the health check use it
// continuously. A failed start is retried after 5, 10, 20, 40, then every
// 60 seconds. Reported to WireGuardRuntimeStatus like DVMDB connections, so
// Settings -> WireGuard VPN -> Status lists the map server. GUI thread only.
class MapServerTunnel final : public QObject
{
public:
    static MapServerTunnel &instance()
    {
        static MapServerTunnel *tunnel = new MapServerTunnel(QCoreApplication::instance());
        return *tunnel;
    }

    // ready(error) after the next start attempt: empty error when running
    // (at once, from the event loop, when it already runs).
    void whenReady(QObject *context, std::function<void(const QString &)> ready)
    {
        if (this->lease.tunnel)
        {
            const QPointer<QObject> guard(context);
            QTimer::singleShot(0, this, [guard, ready]() { if (guard) ready(QString()); });
            return;
        }
        this->waiters.append(QPair<QPointer<QObject>, std::function<void(const QString &)>>(
            QPointer<QObject>(context), std::move(ready)));
        if (!this->starting && !this->retry.isActive()) this->attempt();
    }

    QNetworkProxy proxy() const
    {
        return this->lease.tunnel ? wireGuardQtProxy(*this->lease.tunnel) : QNetworkProxy();
    }

private:
    ~MapServerTunnel() override
    {
        WireGuardRuntimeStatus::instance().stopped(this);
    }

    explicit MapServerTunnel(QObject *parent) : QObject(parent)
    {
        this->retry.setSingleShot(true);
        connect(&this->retry, &QTimer::timeout, this, [this]() { this->attempt(); });
    }

    void attempt()
    {
        const MapServerClientConfiguration &configuration = mapServerClientConfiguration();
        this->starting = true;
        WireGuardRuntimeStatus::instance().starting(configuration.wireguard_profile_id, this,
            QObject::tr("Map server (%1)").arg(configuration.base_url));
        const QPointer<MapServerTunnel> self(this);
        acquireWireGuardTunnel(configuration.wireguard_profile_id, [self](const WireGuardTunnelLease &result)
        {
            if (!self) return;
            self->starting = false;
            QString error = result.error;
            if (result.tunnel)
            {
                self->lease = result;
                self->failures = 0;
                WireGuardRuntimeStatus::instance().running(self, wireGuardTelemetrySampler(result.tunnel));
            }
            else
            {
                if (error.isEmpty()) error = QObject::tr("WireGuard tunnel failed to start.");
                WireGuardRuntimeStatus::instance().failed(self, error);
                ++self->failures;
                self->retry.start(std::min(60000, 5000 << std::min(self->failures - 1, 4)));
                qWarning() << "Map server WireGuard tunnel failed to start; retrying:" << error;
            }
            QList<QPair<QPointer<QObject>, std::function<void(const QString &)>>> notified;
            notified.swap(self->waiters);
            for (const QPair<QPointer<QObject>, std::function<void(const QString &)>> &waiter : notified)
                if (waiter.first) waiter.second(error);
        });
    }

    WireGuardTunnelLease lease;
    bool starting = false;
    int failures = 0;
    QTimer retry;
    QList<QPair<QPointer<QObject>, std::function<void(const QString &)>>> waiters;
};

// InterfaceServerMap over the map server's WireGuard tunnel: requests made
// before the tunnel runs are queued and replayed; if a start attempt fails
// they fail with its error (the repositories retry as for an unreachable
// server). Once running, everything goes to an InterfaceServerMapREST whose
// network managers use the tunnel's SOCKS5 proxy.
class InterfaceServerMapWireGuard final : public InterfaceServerMap
{
public:
    explicit InterfaceServerMapWireGuard(QObject *parent) : InterfaceServerMap(parent) {}

    void requestTile(const QString &endpoint, const QString &key, int x, int y) override
    {
        this->dispatch([=](InterfaceServerMap *server) { server->requestTile(endpoint, key, x, y); },
                       [this, key](const QString &) { emit this->signalTileFailed(key); });
    }
    void requestTerrainTile(const QString &endpoint, const QString &key) override
    {
        this->dispatch([=](InterfaceServerMap *server) { server->requestTerrainTile(endpoint, key); },
                       [this, key](const QString &error) { emit this->signalTerrainTileFailed(key, error); });
    }
    void requestTerrainDatasets() override
    {
        this->dispatch([](InterfaceServerMap *server) { server->requestTerrainDatasets(); },
                       [this](const QString &error) { emit this->signalTerrainDatasetsFailed(error); });
    }
    void requestTerrainElevation(const QString &endpoint) override
    {
        this->dispatch([=](InterfaceServerMap *server) { server->requestTerrainElevation(endpoint); },
                       [this](const QString &error) { emit this->signalTerrainElevationFailed(error); });
    }
    void deleteTiles(quint64 request_id, const QString &provider, int zoom,
                     int tile_x_min, int tile_x_max, int tile_y_min, int tile_y_max) override
    {
        this->dispatch([=](InterfaceServerMap *server)
                       { server->deleteTiles(request_id, provider, zoom, tile_x_min, tile_x_max, tile_y_min, tile_y_max); },
                       [this, request_id](const QString &error) { emit this->signalTileDeletionFailed(request_id, error); });
    }
    void deleteTerrainCache(quint64 request_id, MapTerrainCacheDeletionScope scope,
                            const QString &dataset, int zoom,
                            int tile_x_min, int tile_x_max, int tile_y_min, int tile_y_max) override
    {
        this->dispatch([=](InterfaceServerMap *server)
                       { server->deleteTerrainCache(request_id, scope, dataset, zoom,
                                                    tile_x_min, tile_x_max, tile_y_min, tile_y_max); },
                       [this, request_id](const QString &error)
                       { emit this->signalTerrainCacheDeletionFailed(request_id, error); });
    }
    void requestMapTileUpstreamActivity() override
    {
        this->dispatch([](InterfaceServerMap *server) { server->requestMapTileUpstreamActivity(); },
                       [this](const QString &error) { emit this->signalUpstreamControlError(error); });
    }
    void requestTerrainUpstreamActivity() override
    {
        this->dispatch([](InterfaceServerMap *server) { server->requestTerrainUpstreamActivity(); },
                       [this](const QString &error) { emit this->signalUpstreamControlError(error); });
    }
    // Cancelling before the tunnel runs: nothing reached the server yet.
    void cancelMapTileUpstreamDownloads() override
    {
        if (this->server != nullptr) this->server->cancelMapTileUpstreamDownloads();
    }
    void cancelTerrainUpstreamDownloads() override
    {
        if (this->server != nullptr) this->server->cancelTerrainUpstreamDownloads();
    }

private:
    using Call = std::function<void(InterfaceServerMap *)>;
    using Fail = std::function<void(const QString &)>;

    void dispatch(Call call, Fail fail)
    {
        if (this->server != nullptr)
        {
            call(this->server);
            return;
        }
        this->queued.append(QPair<Call, Fail>(std::move(call), std::move(fail)));
        if (this->waiting) return;
        this->waiting = true;
        MapServerTunnel::instance().whenReady(this, [this](const QString &error)
        {
            this->waiting = false;
            QList<QPair<Call, Fail>> pending;
            pending.swap(this->queued);
            if (!error.isEmpty())
            {
                for (const QPair<Call, Fail> &request : pending) request.second(error);
                return;
            }
            this->connectServer();
            for (const QPair<Call, Fail> &request : pending) request.first(this->server);
        });
    }

    void connectServer()
    {
        MapServerRestConfiguration configuration = guiMapServerRestConfiguration();
        configuration.proxy = MapServerTunnel::instance().proxy();
        this->server = new InterfaceServerMapREST(configuration, this);
        // Re-emit every answer as this transport's own.
        connect(this->server, &InterfaceServerMap::signalTileDataReceived, this, &InterfaceServerMap::signalTileDataReceived);
        connect(this->server, &InterfaceServerMap::signalTileFailed, this, &InterfaceServerMap::signalTileFailed);
        connect(this->server, &InterfaceServerMap::signalTerrainTileDataReceived, this, &InterfaceServerMap::signalTerrainTileDataReceived);
        connect(this->server, &InterfaceServerMap::signalTerrainTileFailed, this, &InterfaceServerMap::signalTerrainTileFailed);
        connect(this->server, &InterfaceServerMap::signalTerrainDatasetsReceived, this, &InterfaceServerMap::signalTerrainDatasetsReceived);
        connect(this->server, &InterfaceServerMap::signalTerrainDatasetsFailed, this, &InterfaceServerMap::signalTerrainDatasetsFailed);
        connect(this->server, &InterfaceServerMap::signalTerrainElevationDataReceived, this, &InterfaceServerMap::signalTerrainElevationDataReceived);
        connect(this->server, &InterfaceServerMap::signalTerrainElevationFailed, this, &InterfaceServerMap::signalTerrainElevationFailed);
        connect(this->server, &InterfaceServerMap::signalTilesDeleted, this, &InterfaceServerMap::signalTilesDeleted);
        connect(this->server, &InterfaceServerMap::signalTileDeletionFailed, this, &InterfaceServerMap::signalTileDeletionFailed);
        connect(this->server, &InterfaceServerMap::signalTerrainCacheDeleted, this, &InterfaceServerMap::signalTerrainCacheDeleted);
        connect(this->server, &InterfaceServerMap::signalTerrainCacheDeletionFailed, this, &InterfaceServerMap::signalTerrainCacheDeletionFailed);
        connect(this->server, &InterfaceServerMap::signalMapTileUpstreamActivity, this, &InterfaceServerMap::signalMapTileUpstreamActivity);
        connect(this->server, &InterfaceServerMap::signalTerrainUpstreamActivity, this, &InterfaceServerMap::signalTerrainUpstreamActivity);
        connect(this->server, &InterfaceServerMap::signalUpstreamControlError, this, &InterfaceServerMap::signalUpstreamControlError);
    }

    InterfaceServerMap *server = nullptr;
    QList<QPair<Call, Fail>> queued;
    bool waiting = false;
};
#endif

bool usesWireGuard()
{
#ifdef AOWIS_STANDALONE
    return false; // The in-process map server needs no transport.
#else
    return !mapServerClientConfiguration().wireguard_profile_id.isEmpty();
#endif
}
}

MapServerRestConfiguration guiMapServerRestConfiguration()
{
    const MapServerClientConfiguration &configuration = mapServerClientConfiguration();
    MapServerRestConfiguration server;
    server.base_url = configuration.base_url;
    server.api_key = configuration.api_key;
    server.delete_api_key = configuration.delete_api_key;
    return server;
}

void whenGuiMapServerReachable(QObject *context,
                               std::function<void(const MapServerRestConfiguration &)> reachable)
{
    const QPointer<QObject> guard(context);
    if (!usesWireGuard())
    {
        QTimer::singleShot(0, context, [reachable]() { reachable(guiMapServerRestConfiguration()); });
        return;
    }
#ifdef AOWIS_GUI_WIREGUARD_PROBE
    // Keep waiting across failed attempts; each retry notifies again.
    std::shared_ptr<std::function<void(const QString &)>> waiter =
        std::make_shared<std::function<void(const QString &)>>();
    *waiter = [guard, reachable, waiter](const QString &error)
    {
        if (!guard)
        {
            *waiter = {}; // Break the self-reference.
            return;
        }
        if (!error.isEmpty())
        {
            MapServerTunnel::instance().whenReady(guard, *waiter);
            return;
        }
        MapServerRestConfiguration configuration = guiMapServerRestConfiguration();
        configuration.proxy = MapServerTunnel::instance().proxy();
        reachable(configuration);
        *waiter = {}; // Break the self-reference.
    };
    MapServerTunnel::instance().whenReady(context, *waiter);
#else
    Q_UNUSED(guard)
    qCritical() << "The map server is configured for WireGuard, which this build cannot run; it is disabled";
#endif
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
    if (usesWireGuard())
    {
        transport.create = [](QObject *parent) -> InterfaceServerMap *
        {
#ifdef AOWIS_GUI_WIREGUARD_PROBE
            return new InterfaceServerMapWireGuard(parent);
#else
            Q_UNUSED(parent)
            qCritical() << "The map server is configured for WireGuard, which this build cannot run; it is disabled";
            return nullptr; // No map server: never a direct fallback.
#endif
        };
        return transport;
    }
    transport.create = [](QObject *parent) -> InterfaceServerMap *
    {
        return new InterfaceServerMapREST(guiMapServerRestConfiguration(), parent);
    };
#endif
    return transport;
}
