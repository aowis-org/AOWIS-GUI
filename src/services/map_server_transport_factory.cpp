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

#include <algorithm>
#endif

#include <QCoreApplication>
#include <QDebug>
#include <QList>
#include <QObject>
#include <QPair>
#include <QPointer>
#include <QTimer>

#include <memory>
#include <utility>

namespace
{
#ifdef AOWIS_GUI_WIREGUARD_PROBE
// The map server's WireGuard tunnel: a lease of the profile's shared tunnel
// (WireGuardTunnelPool), taken on first use and kept while the map server
// connection uses that profile, since tiles, terrain, the catalog and the
// health check use it continuously. A failed start is retried after 5, 10,
// 20, 40, then every 60 seconds. When the connection's profile changes (or
// it becomes direct) the lease is released at once and waiters for the old
// profile are dropped: their owners reconnect through
// onMapServerClientConfigurationChanged(). Reported to WireGuardRuntimeStatus
// like DVMDB connections, so Settings -> WireGuard VPN -> Status lists the
// map server. GUI thread only.
class MapServerTunnel final : public QObject
{
public:
    static MapServerTunnel &instance()
    {
        static MapServerTunnel *tunnel = new MapServerTunnel(QCoreApplication::instance());
        return *tunnel;
    }

    // ready(error) after the next start attempt for the current profile:
    // empty error when running (at once, from the event loop, when it
    // already runs).
    void whenReady(QObject *context, std::function<void(const QString &)> ready)
    {
        this->reconcile();
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
        onMapServerClientConfigurationChanged(this, [this]() { this->reconcile(); });
    }

    // Drops the lease, a pending retry and the waiters of a profile the map
    // server connection no longer uses. A start in flight is discarded when
    // it completes (generation).
    void reconcile()
    {
        const QString profile_id = mapServerClientConfiguration().wireguard_profile_id;
        if (profile_id == this->profile_id) return;
        ++this->generation;
        this->profile_id = profile_id;
        this->lease = WireGuardTunnelLease();
        this->starting = false;
        this->failures = 0;
        this->retry.stop();
        this->waiters.clear();
        WireGuardRuntimeStatus::instance().stopped(this);
    }

    void attempt()
    {
        const MapServerClientConfiguration &configuration = mapServerClientConfiguration();
        if (configuration.wireguard_profile_id.isEmpty()) return;
        this->profile_id = configuration.wireguard_profile_id;
        this->starting = true;
        const quint64 generation = this->generation;
        WireGuardRuntimeStatus::instance().starting(configuration.wireguard_profile_id, this,
            QObject::tr("Map server (%1)").arg(configuration.base_url));
        const QPointer<MapServerTunnel> self(this);
        acquireWireGuardTunnel(configuration.wireguard_profile_id,
            [self, generation](const WireGuardTunnelLease &result)
        {
            // Released at once (the shared_ptr) when the profile changed meanwhile.
            if (!self || generation != self->generation) return;
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

    QString profile_id;
    quint64 generation = 0;
    WireGuardTunnelLease lease;
    bool starting = false;
    int failures = 0;
    QTimer retry;
    QList<QPair<QPointer<QObject>, std::function<void(const QString &)>>> waiters;
};
#endif

#ifndef AOWIS_STANDALONE
// One attempt to make the current map server connection usable: ready(error,
// server) at once (from the event loop) for a direct connection; after the
// next tunnel start attempt for a WireGuard one, with the tunnel's proxy set,
// or with the start error (never a direct fallback). Not called once context
// is destroyed.
void whenTransportReady(QObject *context,
                        std::function<void(const QString &, const MapServerRestConfiguration &)> ready)
{
    const QPointer<QObject> guard(context);
    if (mapServerClientConfiguration().wireguard_profile_id.isEmpty())
    {
        QTimer::singleShot(0, context, [guard, ready]()
        {
            if (guard) ready(QString(), guiMapServerRestConfiguration());
        });
        return;
    }
#ifdef AOWIS_GUI_WIREGUARD_PROBE
    MapServerTunnel::instance().whenReady(context, [guard, ready](const QString &error)
    {
        if (!guard) return;
        MapServerRestConfiguration server = guiMapServerRestConfiguration();
        if (error.isEmpty()) server.proxy = MapServerTunnel::instance().proxy();
        ready(error, server);
    });
#else
    QTimer::singleShot(0, context, [guard, ready]()
    {
        if (guard)
            ready(QObject::tr("The map server is configured for WireGuard, which this build cannot run."),
                  MapServerRestConfiguration());
    });
#endif
}

// InterfaceServerMap of the GUI's map server connection, direct or over the
// map server's WireGuard tunnel, following configuration changes without a
// restart. Requests made while the connection is not usable yet are queued
// and replayed; if a tunnel start fails they fail with its error (the
// repositories retry as for an unreachable server). Once usable, everything
// goes to an InterfaceServerMapREST (with the tunnel's SOCKS5 proxy for
// WireGuard). After a configuration change new requests use a new
// InterfaceServerMapREST; the previous one stays connected for a while so
// requests in flight still deliver their answers (or failures), then it is
// deleted. Cached tiles are unaffected.
class InterfaceServerMapRemote final : public InterfaceServerMap
{
public:
    explicit InterfaceServerMapRemote(QObject *parent) : InterfaceServerMap(parent)
    {
        onMapServerClientConfigurationChanged(this, [this]() { this->reconnect(); });
    }

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
    // Cancelling before the connection is usable: nothing reached the server yet.
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

    // Longer than any transfer timeout of InterfaceServerMapREST.
    static constexpr int RetiredServerLifetimeMs = 180000;

    void reconnect()
    {
        ++this->generation;
        if (this->server != nullptr)
        {
            InterfaceServerMap *retired = this->server;
            this->server = nullptr;
            QTimer::singleShot(RetiredServerLifetimeMs, retired, &QObject::deleteLater);
        }
        // A wait for the previous connection is abandoned; queued requests
        // wait for the new one.
        this->waiting = false;
        if (!this->queued.isEmpty()) this->wait();
    }

    void dispatch(Call call, Fail fail)
    {
        if (this->server != nullptr)
        {
            call(this->server);
            return;
        }
        this->queued.append(QPair<Call, Fail>(std::move(call), std::move(fail)));
        if (!this->waiting) this->wait();
    }

    void wait()
    {
        this->waiting = true;
        const quint64 generation = this->generation;
        whenTransportReady(this, [this, generation](const QString &error,
                                                    const MapServerRestConfiguration &configuration)
        {
            if (generation != this->generation) return; // Superseded by reconnect().
            this->waiting = false;
            QList<QPair<Call, Fail>> pending;
            pending.swap(this->queued);
            if (!error.isEmpty())
            {
                for (const QPair<Call, Fail> &request : pending) request.second(error);
                return;
            }
            this->connectServer(configuration);
            for (const QPair<Call, Fail> &request : pending) request.first(this->server);
        });
    }

    void connectServer(const MapServerRestConfiguration &configuration)
    {
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
    quint64 generation = 0;
    bool waiting = false;
};
#endif
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
#ifdef AOWIS_STANDALONE
    // No map server connection: the in-process map server needs none.
    const QPointer<QObject> guard(context);
    QTimer::singleShot(0, context, [guard, reachable]()
    {
        if (guard) reachable(guiMapServerRestConfiguration());
    });
#else
    // Deliveries of an earlier configuration are dropped (generation). A
    // WireGuard connection keeps waiting across failed tunnel starts.
    const QPointer<QObject> guard(context);
    const std::shared_ptr<quint64> generation = std::make_shared<quint64>(0);
    const std::shared_ptr<std::function<void(quint64)>> attempt =
        std::make_shared<std::function<void(quint64)>>();
    const std::weak_ptr<std::function<void(quint64)>> weak_attempt = attempt;
    *attempt = [guard, reachable, generation, weak_attempt](quint64 current)
    {
        if (!guard || current != *generation) return;
        whenTransportReady(guard, [guard, reachable, generation, weak_attempt, current](
                                      const QString &error, const MapServerRestConfiguration &server)
        {
            if (!guard || current != *generation) return;
            if (error.isEmpty())
            {
                reachable(server);
                return;
            }
#ifdef AOWIS_GUI_WIREGUARD_PROBE
            // The tunnel retries with backoff; wait for its next attempt.
            const std::shared_ptr<std::function<void(quint64)>> retry = weak_attempt.lock();
            if (retry) (*retry)(current);
#else
            // Permanent in this build until the connection changes.
            Q_UNUSED(weak_attempt)
            qCritical().noquote() << "Map server unavailable:" << error;
#endif
        });
    };
    // The observer owns the attempt function for context's lifetime.
    onMapServerClientConfigurationChanged(context, [attempt, generation]()
    {
        (*attempt)(++*generation);
    });
    (*attempt)(*generation);
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
    // Direct or WireGuard, switching live on configuration changes; a
    // WireGuard connection never falls back to direct HTTP.
    transport.create = [](QObject *parent) -> InterfaceServerMap *
    {
        return new InterfaceServerMapRemote(parent);
    };
#endif
    return transport;
}
