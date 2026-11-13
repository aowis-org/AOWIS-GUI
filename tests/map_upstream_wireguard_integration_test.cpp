// Standalone integration test over a real userspace WireGuard peer
// (tests/wireguard_catalogue_peer.go): an embedded MAP cascading upstream is
// reached only through its central profile's shared tunnel, for provider
// discovery and raster tiles; a DVMDB connection with the same profile
// shares that one tunnel; saving an edited profile restarts it once and
// every consumer reconnects; a stopped peer or missing profile never causes
// a direct request; cached tiles stay available offline.
//
// The peer forwards its in-tunnel HTTP port 10.173.91.1:18080 to the loopback
// fake MAP upstream below and adds X-Forwarded-For with the tunnel client's
// address (10.173.91.2), which proves the request crossed the tunnel.
#include "config/wireguard_profile_storage.h"
#include "network/wireguard_runtime_status.h"
#include "network/wireguard_tunnel_pool.h"
#include "projects/repository_probe.h"
#include "services/map_upstream_tunnels.h"

#include <aowis/map/map_provider.h>
#include <aowis/map/maptiles.h>
#include <aowis/map/provider_catalog.h>
#include <aowis/map/settings_repository.h>
#include <aowis/network/wireguard_client.h>

#include <QBuffer>
#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>

#include <cstdio>
#include <functional>
#include <memory>

namespace
{
const QString TunnelUpstream = QStringLiteral("http://10.173.91.1:18080");

bool require(bool condition, const char *message)
{
    if (!condition) std::fprintf(stderr, "FAILED: %s\n", message);
    return condition;
}

bool waitUntil(const std::function<bool()> &ready, int timeout_ms = 15000)
{
    QElapsedTimer timer;
    timer.start();
    while (!ready() && timer.elapsed() < timeout_ms)
    {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(2);
    }
    return ready();
}

QByteArray pngTile()
{
    QImage image(256, 256, QImage::Format_RGB32);
    image.fill(QColor(30, 120, 60));
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return bytes;
}

// Fake MAP upstream (and DVMDB status endpoint) on loopback.
class MapUpstreamStub final : public QTcpServer
{
public:
    QByteArray catalog;
    QByteArray tile = pngTile();
    QList<QByteArray> requests; // Request heads, lower case.

    bool start() { return listen(QHostAddress::LocalHost, 0); }

    int count(const QByteArray &fragment) const
    {
        int result = 0;
        for (const QByteArray &request : this->requests)
            if (request.contains(fragment)) ++result;
        return result;
    }

protected:
    void incomingConnection(qintptr descriptor) override
    {
        QTcpSocket *socket = new QTcpSocket(this);
        if (!socket->setSocketDescriptor(descriptor))
        {
            socket->deleteLater();
            return;
        }
        QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        QObject::connect(socket, &QTcpSocket::readyRead, socket,
            [this, socket, received = QByteArray()]() mutable
        {
            received += socket->readAll();
            const qsizetype end = received.indexOf("\r\n\r\n");
            if (end < 0) return;
            const QByteArray head = received.left(end).toLower();
            this->requests.append(head);
            QByteArray type = QByteArrayLiteral("text/plain");
            QByteArray body = QByteArrayLiteral("AOWIS map server");
            if (head.startsWith("get /map/v1/providers/available "))
            {
                type = QByteArrayLiteral("application/json");
                body = this->catalog;
            }
            else if (head.split(' ').value(1).endsWith(".png"))
            {
                type = QByteArrayLiteral("image/png");
                body = this->tile;
            }
            socket->write("HTTP/1.1 200 OK\r\nContent-Type: " + type + "\r\nContent-Length: " +
                          QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
            socket->disconnectFromHost();
        });
    }
};

struct TileWait
{
    QByteArray data;
    bool failed = false;
};
}

int main(int argc, char **argv)
{
    if (!require(argc == 2, "usage: aowis-map-upstream-wireguard-integration-tests <peer>")) return 1;
    QTemporaryDir root;
    if (!require(root.isValid(), "temporary root")) return 1;
    qputenv("XDG_CONFIG_HOME", QDir(root.path()).filePath(QStringLiteral("config")).toUtf8());
    qputenv("XDG_DATA_HOME", QDir(root.path()).filePath(QStringLiteral("data")).toUtf8());
    qputenv("XDG_CACHE_HOME", QDir(root.path()).filePath(QStringLiteral("cache")).toUtf8());
    QCoreApplication app(argc, argv);

    Aowis::Map::MapProvider inherited = Aowis::Map::defaultMapProviders().first();
    inherited.id = QStringLiteral("2d5b8f0e-9c1a-4f7e-8b36-5a4e1c0d9f21");
    inherited.name = QStringLiteral("Regional imagery");
    inherited.url_template = QStringLiteral("https://tiles.example.invalid/regional/{z}/{x}/{y}.png");
    MapUpstreamStub stub;
    stub.catalog = Aowis::Map::encodeProviderCatalog(QVector<Aowis::Map::MapProvider>{inherited});
    if (!require(stub.start(), "fake MAP upstream did not start")) return 1;

    QProcess peer;
    peer.start(QString::fromLocal8Bit(argv[1]), {QString::number(stub.serverPort())});
    if (!require(peer.waitForStarted() && waitUntil([&peer]() { return peer.canReadLine(); }),
                 "WireGuard peer did not start")) return 1;
    const QJsonObject ready = QJsonDocument::fromJson(peer.readLine()).object();
    const QByteArray config_text = "[Interface]\nAddress=10.173.91.2/32\nPrivateKey="
        + ready.value(QStringLiteral("client_private_key")).toString().toLatin1()
        + "\n[Peer]\nPublicKey=" + ready.value(QStringLiteral("server_public_key")).toString().toLatin1()
        + "\nEndpoint=" + ready.value(QStringLiteral("endpoint")).toString().toLatin1()
        + "\nAllowedIPs=10.173.91.1/32\n";
    QString profile_id;
    QString import_error;
    if (!require(importWireGuardProfile(QStringLiteral("Regional VPN"), config_text, &profile_id, &import_error),
                 "could not store the WireGuard profile")) return 1;

    // 1. The upstream's tunnel starts; until then it has no transport.
    Aowis::Map::MapCascadeConfig cascade;
    cascade.allow_direct_fetch = false;
    Aowis::Map::MapCascadeUpstream regional;
    regional.name = QStringLiteral("regional");
    regional.url = QUrl(TunnelUpstream);
    regional.api_key = QByteArrayLiteral("regional-read");
    regional.wireguard_profile_id = profile_id;
    cascade.upstreams.append(regional);
    MapUpstreamTunnels &tunnels = MapUpstreamTunnels::instance();
    tunnels.setCascade(cascade);
    if (!require(!Aowis::Map::hasWireGuardTransport(tunnels.resolved().upstreams.first()),
                 "transport available before the tunnel runs")) return 1;
    if (!require(waitUntil([&tunnels]()
        {
            return Aowis::Map::hasWireGuardTransport(tunnels.resolved().upstreams.first());
        }), "MAP upstream tunnel did not start")) return 1;
    const quint16 first_port = tunnels.resolved().upstreams.first().proxy.port();

    // 2. Provider discovery through the tunnel, with the upstream's read key.
    Aowis::Map::ProviderCatalog catalog(QDir(root.path()).filePath(QStringLiteral("catalog")));
    catalog.configure({}, tunnels.resolved());
    catalog.refresh();
    if (!require(waitUntil([&catalog, &inherited]()
        {
            return catalog.inheritedUpstreams().value(inherited.id).contains(QStringLiteral("regional"));
        }), "provider discovery through WireGuard failed")) return 1;
    if (!require(stub.count("get /map/v1/providers/available ") == 1 &&
                 stub.requests.last().contains("x-forwarded-for: 10.173.91.2") &&
                 stub.requests.last().contains("x-api-key: regional-read"),
                 "discovery did not cross the tunnel with the read key")) return 1;

    // 3. Raster tiles through the tunnel.
    MapTiles tiles(QDir(root.path()).filePath(QStringLiteral("tiles")), 2, 8);
    tiles.configureProviders(Aowis::Map::defaultMapProviders(), {}, {});
    tiles.setCascadeConfig(tunnels.resolved());
    const QString provider = Aowis::Map::defaultMapProviders().first().id;
    QHash<QString, TileWait> waits;
    QObject::connect(&tiles, &MapTiles::tileReady, &app,
                     [&waits](const QString &key, const QByteArray &data) { waits[key].data = data; });
    QObject::connect(&tiles, &MapTiles::tileFailed, &app,
                     [&waits](const QString &key, MapTiles::TileFailureReason) { waits[key].failed = true; });
    if (!require(tiles.getTile(provider, 3, 4, 2, QStringLiteral("first")).status ==
                     MapTiles::TileRequestStatus::Pending &&
                 waitUntil([&waits]() { return !waits.value(QStringLiteral("first")).data.isEmpty(); }),
                 "raster tile through WireGuard failed")) return 1;
    if (!require(waits.value(QStringLiteral("first")).data == stub.tile &&
                 stub.count(".png ") == 1 && stub.requests.last().contains("x-forwarded-for: 10.173.91.2"),
                 "raster tile did not cross the tunnel")) return 1;

    // 4. A DVMDB connection with the same profile shares the one tunnel.
    DvmdbConnection dvmdb(QUrl(TunnelUpstream), profile_id, &app);
    QString open_error = QStringLiteral("pending");
    dvmdb.open([&open_error](const QString &error) { open_error = error; });
    if (!require(waitUntil([&open_error]() { return open_error != QStringLiteral("pending"); }) &&
                 open_error.isEmpty() && dvmdb.isOpen(), "DVMDB connection over WireGuard failed")) return 1;
    QNetworkReply *status_reply = dvmdb.get(QNetworkRequest(QUrl(TunnelUpstream + QStringLiteral("/status"))));
    if (!require(status_reply != nullptr && waitUntil([status_reply]() { return status_reply->isFinished(); }) &&
                 status_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200,
                 "DVMDB request through the shared tunnel failed")) return 1;
    status_reply->deleteLater();
    if (!require(WireGuardRuntimeStatus::instance().snapshot(profile_id).active_connections == 2,
                 "MAP upstream and DVMDB are not both reported on the profile")) return 1;
    std::shared_ptr<aowis::network::WireGuardClient> held;
    acquireWireGuardTunnel(profile_id, [&held](const WireGuardTunnelLease &lease) { held = lease.tunnel; });
    if (!require(waitUntil([&held]() { return held != nullptr; }) && held->proxy().port == first_port,
                 "a second tunnel was started for the same profile")) return 1;

    // 5. Saving an edited profile restarts the one tunnel; all reconnect.
    restartWireGuardTunnel(profile_id);
    if (!require(!held->isRunning() &&
                 !Aowis::Map::hasWireGuardTransport(tunnels.resolved().upstreams.first()),
                 "restart did not stop the old tunnel first")) return 1;
    if (!require(waitUntil([&tunnels, &dvmdb]()
        {
            return Aowis::Map::hasWireGuardTransport(tunnels.resolved().upstreams.first()) && dvmdb.isOpen();
        }), "consumers did not reconnect after the restart")) return 1;
    held.reset();
    std::shared_ptr<aowis::network::WireGuardClient> restarted;
    acquireWireGuardTunnel(profile_id, [&restarted](const WireGuardTunnelLease &lease) { restarted = lease.tunnel; });
    if (!require(waitUntil([&restarted]() { return restarted != nullptr; }) &&
                 restarted->proxy().port == tunnels.resolved().upstreams.first().proxy.port() &&
                 WireGuardRuntimeStatus::instance().snapshot(profile_id).active_connections == 2,
                 "restart did not leave exactly one shared tunnel")) return 1;
    restarted.reset();
    tiles.setCascadeConfig(tunnels.resolved());
    if (!require(tiles.getTile(provider, 3, 5, 2, QStringLiteral("after-restart")).status ==
                     MapTiles::TileRequestStatus::Pending &&
                 waitUntil([&waits]() { return !waits.value(QStringLiteral("after-restart")).data.isEmpty(); }) &&
                 stub.count(".png ") == 2,
                 "raster tile through the restarted tunnel failed")) return 1;

    // 6. A stopped peer or a missing profile never causes a direct request.
    peer.write("quit\n");
    if (!require(peer.waitForFinished(5000), "WireGuard peer did not stop")) return 1;
    const int requests_before = static_cast<int>(stub.requests.size());
    if (!require(tiles.getTile(provider, 3, 6, 2, QStringLiteral("peer-down")).status ==
                     MapTiles::TileRequestStatus::Pending &&
                 waitUntil([&waits]() { return waits.value(QStringLiteral("peer-down")).failed; }, 30000),
                 "tile with the peer down did not fail")) return 1;
    Aowis::Map::MapCascadeConfig missing = cascade;
    missing.upstreams[0].url = QUrl(QStringLiteral("http://127.0.0.1:%1").arg(stub.serverPort()));
    missing.upstreams[0].wireguard_profile_id = QStringLiteral("019a47b1-2040-7af9-8d1c-2670badb6399");
    tunnels.setCascade(missing);
    tiles.setCascadeConfig(tunnels.resolved());
    catalog.configure({}, tunnels.resolved());
    catalog.refresh();
    tiles.getTile(provider, 3, 7, 2, QStringLiteral("missing-profile"));
    waitUntil([]() { return false; }, 1500);
    if (!require(static_cast<int>(stub.requests.size()) == requests_before &&
                 !Aowis::Map::hasWireGuardTransport(tunnels.resolved().upstreams.first()),
                 "a WireGuard upstream was contacted directly")) return 1;

    // 7. Tiles fetched through the tunnel stay available offline.
    const MapTiles::TileRequestResult cached = tiles.getTile(provider, 3, 4, 2, QStringLiteral("cached"));
    if (!require((cached.status == MapTiles::TileRequestStatus::Ready && cached.data == stub.tile) ||
                 (cached.status == MapTiles::TileRequestStatus::Pending &&
                  waitUntil([&waits]() { return waits.value(QStringLiteral("cached")).data == pngTile(); })),
                 "tile fetched through WireGuard is not available offline")) return 1;

    tunnels.setCascade(Aowis::Map::MapCascadeConfig());
    std::fprintf(stdout, "MAP upstream WireGuard integration tests passed\n");
    return 0;
}
