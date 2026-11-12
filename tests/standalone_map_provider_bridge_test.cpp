#include "services/standalone_map_provider_catalog_bridge.h"
#include <aowis/map/settings_repository.h>
#include <aowis/map/provider_catalog.h>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QTemporaryDir>
#include <QTimer>
#include <QUuid>

#include <cstdio>
#include <functional>
#include <memory>

namespace
{
bool expect(bool condition, const char *message)
{
    if (!condition) std::fprintf(stderr, "FAILED: %s\n", message);
    return condition;
}

constexpr const char *InheritedProviderId = "15c2d930-1e4a-4b76-9675-31ea802989fa";

bool waitUntil(const std::function<bool()> &ready)
{
    QElapsedTimer timer;
    timer.start();
    while (!ready() && timer.elapsed() < 4000)
    {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 40);
        QThread::msleep(1);
    }
    return ready();
}

// Real MAP's ProviderCatalog performs authenticated HTTP discovery. Exercise
// that pipeline rather than populating the Standalone bridge with fake rows.
class TestUpstream final : public QObject
{
public:
    TestUpstream(const QByteArray &key, const QByteArray &catalog, QObject *parent = nullptr)
        : QObject(parent), expected_key(key), catalog_payload(catalog), listener(this)
    {
        QObject::connect(&this->listener, &QTcpServer::newConnection, this, [this]()
        {
            while (this->listener.hasPendingConnections())
            {
                QTcpSocket *socket = this->listener.nextPendingConnection();
                QObject::connect(socket, &QTcpSocket::readyRead, socket,
                    [this, socket, buffered = QByteArray()]() mutable
                {
                    buffered += socket->readAll();
                    const int end = buffered.indexOf("\r\n\r\n");
                    if (end < 0) return;
                    const QByteArray headers = buffered.left(end);
                    ++this->requests;
                    const bool authorized = headers.toLower().contains(
                        QByteArray("x-api-key: ") + this->expected_key.toLower());
                    const bool requested_catalog = headers.startsWith(
                        "GET /map/v1/providers/available ");
                    if (authorized && requested_catalog) ++this->authorized_requests;
                    const QByteArray payload = authorized && requested_catalog ?
                        this->catalog_payload : QByteArray("{}");
                    const QByteArray status = authorized && requested_catalog ?
                        QByteArray("200 OK") : QByteArray("403 Forbidden");
                    socket->write(QByteArray("HTTP/1.1 ") + status +
                        "\r\nContent-Type: application/json\r\nConnection: close\r\n"
                        "Content-Length: " + QByteArray::number(payload.size()) +
                        "\r\n\r\n" + payload);
                    socket->disconnectFromHost();
                });
                QObject::connect(socket, &QTcpSocket::disconnected,
                                 socket, &QObject::deleteLater);
            }
        });
    }

    bool start() { return this->listener.listen(QHostAddress::LocalHost, 0); }
    QUrl url() const
    {
        return QUrl(QStringLiteral("http://127.0.0.1:%1").arg(this->listener.serverPort()));
    }
    void stop() { this->listener.close(); }

    int requests = 0;
    int authorized_requests = 0;

private:
    QByteArray expected_key;
    QByteArray catalog_payload;
    QTcpServer listener;
};

QJsonObject providerById(const QByteArray &payload, const QString &id)
{
    const QJsonArray providers = QJsonDocument::fromJson(payload).object()
        .value(QStringLiteral("providers")).toArray();
    for (const QJsonValue &value : providers)
        if (value.toObject().value(QStringLiteral("id")).toString() == id)
            return value.toObject();
    return {};
}

struct Result
{
    int code = 0;
    QByteArray body;
    bool finished = false;
};

Result request(QNetworkAccessManager *manager, const QUrl &url,
               const QByteArray &key, const QByteArray &method, const QByteArray &body = {})
{
    QNetworkRequest query(url);
    query.setTransferTimeout(3000);
    if (!key.isEmpty()) query.setRawHeader("X-API-Key", key);
    query.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    QNetworkReply *reply = method == "GET" ? manager->get(query) : manager->put(query, body);
    QEventLoop loop;
    QTimer watchdog;
    watchdog.setSingleShot(true);
    watchdog.start(4000);
    QObject::connect(&watchdog, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    if (!reply->isFinished()) loop.exec();
    Result result;
    result.finished = reply->isFinished();
    result.code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    result.body = reply->readAll();
    reply->deleteLater();
    return result;
}
}

int main(int argc, char **argv)
{
    QTemporaryDir root;
    if (!expect(root.isValid(), "temporary root")) return 1;
    qputenv("XDG_CONFIG_HOME", root.path().toUtf8());
    qputenv("XDG_DATA_HOME", root.path().toUtf8());
    QCoreApplication application(argc, argv);
    // Standalone must respect its MAP-owned administrator acquisition policy.
    QString setup_error;
    std::unique_ptr<Aowis::Map::SettingsRepository> setup_repository =
        Aowis::Map::SettingsRepository::openStandaloneForCurrentUser(&setup_error);
    if (!expect(setup_repository != nullptr, "Standalone MAP settings repository")) return 1;
    // Create two actual upstream HTTP endpoints advertising an identical
    // provider that is absent from Standalone's editable local catalog.
    Aowis::Map::MapProvider inherited_provider = Aowis::Map::defaultMapProviders().first();
    const QString inherited_id = QString::fromLatin1(InheritedProviderId);
    inherited_provider.id = inherited_id;
    inherited_provider.name = QStringLiteral("Remote project imagery");
    inherited_provider.url_template = QStringLiteral(
        "https://tiles.example.org/project/{z}/{x}/{y}.png");
    const QByteArray upstream_catalog = Aowis::Map::encodeProviderCatalog(
        QVector<Aowis::Map::MapProvider>{inherited_provider});
    TestUpstream primary("primary-read", upstream_catalog);
    TestUpstream backup("backup-read", upstream_catalog);
    if (!expect(primary.start() && backup.start(), "upstream HTTP listeners")) return 1;

    Aowis::Map::MapCascadeConfig cascade;
    cascade.allow_direct_fetch = false;
    cascade.upstreams.append({QStringLiteral("primary"), primary.url(), QByteArray("primary-read")});
    cascade.upstreams.append({QStringLiteral("backup"), backup.url(), QByteArray("backup-read")});
    if (!expect(setup_repository->saveCascade(cascade, {}, nullptr, &setup_error),
                "persist ordered upstreams and direct-fetch prohibition")) return 1;
    StandaloneMapProviderCatalogBridge &bridge = StandaloneMapProviderCatalogBridge::instance();
    const MapServerRestConfiguration server = bridge.catalogServer();
    if (!expect(waitUntil([&bridge, &inherited_id]()
    {
        // Wait for both independent HTTP responses, not just the first one.
        const QSet<QString> sources = bridge.inheritedUpstreams().value(inherited_id);
        return sources.contains(QStringLiteral("primary")) &&
               sources.contains(QStringLiteral("backup"));
    }), "Standalone MAP discovers provider from both upstreams")) return 1;
    if (!expect(primary.authorized_requests > 0 && backup.authorized_requests > 0,
                "upstream credentials forwarded for both catalogs")) return 1;
    if (!expect(!server.base_url.isEmpty() && !server.api_key.isEmpty(), "localhost catalog listening")) return 1;
    QNetworkAccessManager manager;
    const QUrl url(server.base_url + QStringLiteral("/map/v1/providers"));
    const Result unauthorized = request(&manager, url, {}, "GET");
    if (!expect(unauthorized.finished && unauthorized.code == 403, "key required")) return 1;
    const Result original = request(&manager, url, server.api_key, "GET");
    if (!expect(original.finished && original.code == 200, "catalog GET")) return 1;
    const Result available = request(&manager,
        QUrl(server.base_url + QStringLiteral("/map/v1/providers/available")),
        server.api_key, "GET");
    if (!expect(available.code == 200, "combined catalog GET")) return 1;
    const QJsonArray available_rows = QJsonDocument::fromJson(available.body)
        .object().value(QStringLiteral("providers")).toArray();
    const QJsonObject inherited_available = providerById(available.body, inherited_id);
    if (!expect(available_rows.size() == 5 &&
        available_rows.at(0).toObject().value(QStringLiteral("origin")).toString() ==
            QStringLiteral("local") &&
        inherited_available.value(QStringLiteral("origin")).toString() ==
            QStringLiteral("inherited") &&
        inherited_available.value(QStringLiteral("enabled")).toBool() &&
        inherited_available.value(QStringLiteral("upstreams")).toArray() ==
            QJsonArray{QStringLiteral("primary"), QStringLiteral("backup")},
        "bridge combines local and discovered upstream providers")) return 1;
    const Result rejected_inherited = request(&manager,
        QUrl(server.base_url + QStringLiteral("/map/v1/providers/inherited")),
        server.api_key, "PUT",
        QJsonDocument(QJsonObject{{QStringLiteral("id"),
            available_rows.at(0).toObject().value(QStringLiteral("id"))},
            {QStringLiteral("enabled"), false}}).toJson(QJsonDocument::Compact));
    if (!expect(rejected_inherited.code == 404,
                "local provider cannot be changed as inherited")) return 1;
    const Result rejected_route = request(&manager,
        QUrl(server.base_url + QStringLiteral("/map/v1/providers/upstreams")),
        server.api_key, "PUT",
        QJsonDocument(QJsonObject{{QStringLiteral("id"),
            available_rows.at(0).toObject().value(QStringLiteral("id"))},
            {QStringLiteral("upstreams"), QJsonArray{QStringLiteral("nonexistent")}}})
            .toJson(QJsonDocument::Compact));
    if (!expect(rejected_route.code == 400,
                "Standalone rejects routing to a non-advertising peer")) return 1;
    QJsonObject catalog = QJsonDocument::fromJson(original.body).object();
    QJsonArray providers = catalog.value(QStringLiteral("providers")).toArray();
    if (!expect(providers.size() == 4, "default providers loaded")) return 1;
    QJsonObject first = providers.at(0).toObject();
    first.insert(QStringLiteral("name"), QStringLiteral("Edited embedded provider"));
    providers.replace(0, first);
    catalog.insert(QStringLiteral("providers"), providers);
    int changed = 0;
    QObject::connect(&bridge, &StandaloneMapProviderCatalogBridge::providersChanged,
                     &application, [&changed]() { ++changed; });
    const Result saved = request(&manager, url, server.api_key, "PUT",
                                 QJsonDocument(catalog).toJson(QJsonDocument::Compact));
    if (!expect(saved.code == 200 && changed == 1, "valid local edit saved and signalled")) return 1;
    const Result updated = request(&manager, url, server.api_key, "GET");
    if (!expect(QJsonDocument::fromJson(updated.body).object().value(QStringLiteral("providers"))
                    .toArray().at(0).toObject().value(QStringLiteral("name")).toString() ==
                    QStringLiteral("Edited embedded provider"), "updated catalog readable")) return 1;
    const QString first_id = providers.at(0).toObject().value(QStringLiteral("id")).toString();
    const Result policy = request(&manager,
        QUrl(server.base_url + QStringLiteral("/map/v1/providers/acquisition")),
        server.delete_api_key, "PUT", QJsonDocument(QJsonObject{
            {QStringLiteral("id"), first_id},
            {QStringLiteral("mode"), QStringLiteral("cascade")}
        }).toJson(QJsonDocument::Compact));
    if (!expect(policy.code == 200, "Standalone MAP policy uses library-owned repository")) return 1;
    const QJsonDocument policy_result = QJsonDocument::fromJson(policy.body);
    if (!expect(policy_result.isObject() &&
                policy_result.object().value(QStringLiteral("ok")).toBool(),
                "Standalone acquisition response is valid JSON")) return 1;
    const Result updated_available = request(&manager,
        QUrl(server.base_url + QStringLiteral("/map/v1/providers/available")),
        server.api_key, "GET");
    const QJsonArray updated_rows = QJsonDocument::fromJson(updated_available.body)
        .object().value(QStringLiteral("providers")).toArray();
    bool cascade_visible = false;
    for (const QJsonValue &value : updated_rows)
        if (value.toObject().value(QStringLiteral("id")).toString() == first_id)
            cascade_visible = value.toObject().value(
                QStringLiteral("acquisition_mode")).toString() == QStringLiteral("cascade");
    if (!expect(updated_available.code == 200 && cascade_visible,
                "Standalone read-only catalog reflects selected acquisition")) return 1;
    const Result denied_direct = request(&manager,
        QUrl(server.base_url + QStringLiteral("/map/v1/providers/acquisition")),
        server.delete_api_key, "PUT", QJsonDocument(QJsonObject{
            {QStringLiteral("id"), first_id},
            {QStringLiteral("mode"), QStringLiteral("direct")}
        }).toJson(QJsonDocument::Compact));
    const QJsonDocument denied_direct_json = QJsonDocument::fromJson(denied_direct.body);
    if (!expect(denied_direct.code == 403 && denied_direct_json.isObject() &&
                !denied_direct_json.object().value(QStringLiteral("error")).toString().isEmpty(),
                "global policy forbids Direct and returns valid JSON")) return 1;
    const Result invalid_mode = request(&manager,
        QUrl(server.base_url + QStringLiteral("/map/v1/providers/acquisition")),
        server.delete_api_key, "PUT", QJsonDocument(QJsonObject{
            {QStringLiteral("id"), first_id},
            {QStringLiteral("mode"), QStringLiteral("invalid")}
        }).toJson(QJsonDocument::Compact));
    if (!expect(invalid_mode.code == 400 &&
                QJsonDocument::fromJson(invalid_mode.body).isObject(),
                "invalid acquisition mode is rejected with valid JSON")) return 1;
    QString error;
    std::unique_ptr<Aowis::Map::SettingsRepository> repository =
        Aowis::Map::SettingsRepository::openStandaloneForCurrentUser(&error);
    QHash<QString, Aowis::Map::MapProviderAcquisitionMode> stored_modes;
    if (!expect(repository->readProviderAcquisitionModes(
                    &stored_modes, nullptr, &error) &&
                stored_modes.value(first_id) ==
                    Aowis::Map::MapProviderAcquisitionMode::Cascade,
                "acquisition persists in MAP-owned repository")) return 1;
    QVector<Aowis::Map::MapProvider> stored;
    if (!expect(repository != nullptr && repository->readProviders(&stored, nullptr, &error) &&
                stored.at(0).name == QStringLiteral("Edited embedded provider"),
                "catalog persisted exclusively by MAP")) return 1;
    // The combined catalog emits providersChanged for acquisition-mode changes,
    // too. Measure this rejection relative to the current count, not the one
    // successful provider edit earlier in the test.
    const Result before_invalid = request(&manager, url, server.api_key, "GET");
    if (!expect(before_invalid.code == 200,
                "catalog readable before invalid edit")) return 1;
    const int changes_before_invalid = changed;
    first.insert(QStringLiteral("url"), QStringLiteral("file:///etc/passwd"));
    providers.replace(0, first);
    catalog.insert(QStringLiteral("providers"), providers);
    const Result rejected = request(&manager, url, server.api_key, "PUT",
                                    QJsonDocument(catalog).toJson(QJsonDocument::Compact));
    const Result after_invalid = request(&manager, url, server.api_key, "GET");
    QVector<Aowis::Map::MapProvider> persisted_after_invalid;
    const bool persisted_unchanged = repository->readProviders(
        &persisted_after_invalid, nullptr, &error) &&
        Aowis::Map::encodeProviderCatalog(persisted_after_invalid) ==
            Aowis::Map::encodeProviderCatalog(stored);
    if (!expect(rejected.code == 400 && changed == changes_before_invalid &&
                after_invalid.code == 200 &&
                after_invalid.body == before_invalid.body && persisted_unchanged,
                "invalid edit rejected atomically"))
    {
        std::fprintf(stderr, "invalid PUT status=%d, signals=%d->%d, "
                     "GET status=%d, catalog unchanged=%d, persisted unchanged=%d, "
                     "response=%s, repository error=%s\n",
                     rejected.code, changes_before_invalid, changed,
                     after_invalid.code, after_invalid.body == before_invalid.body,
                     persisted_unchanged, rejected.body.constData(),
                     error.toUtf8().constData());
        return 1;
    }

    // Inherited definitions are read-only, but their availability and routing
    // are local MAP settings that persist independently from the upstream.
    const QUrl inherited_endpoint(server.base_url + QStringLiteral("/map/v1/providers/inherited"));
    const Result disabled = request(&manager, inherited_endpoint, server.delete_api_key,
        "PUT", QJsonDocument(QJsonObject{{QStringLiteral("id"), inherited_id},
            {QStringLiteral("enabled"), false}}).toJson(QJsonDocument::Compact));
    if (!expect(disabled.code == 200 &&
                !providerById(request(&manager, QUrl(server.base_url +
                    QStringLiteral("/map/v1/providers/available")), server.api_key,
                    "GET").body, inherited_id).value(QStringLiteral("enabled")).toBool(true),
                "local inherited disable affects available catalog")) return 1;
    QSet<QString> disabled_ids;
    if (!expect(repository->readDisabledInheritedProviders(&disabled_ids, nullptr, &error) &&
                disabled_ids.contains(inherited_id),
                "inherited availability persisted in MAP CONFIG")) return 1;
    const Result reenabled = request(&manager, inherited_endpoint, server.delete_api_key,
        "PUT", QJsonDocument(QJsonObject{{QStringLiteral("id"), inherited_id},
            {QStringLiteral("enabled"), true}}).toJson(QJsonDocument::Compact));
    if (!expect(reenabled.code == 200 &&
                bridge.inheritedUpstreams().value(inherited_id).contains(QStringLiteral("backup")),
                "inherited re-enable restores allowed MAP upstreams")) return 1;

    // Select backup only; neither local provider editing nor catalogue
    // discovery should silently reinsert the excluded primary.
    const Result selected = request(&manager,
        QUrl(server.base_url + QStringLiteral("/map/v1/providers/upstreams")),
        server.delete_api_key, "PUT", QJsonDocument(QJsonObject{
            {QStringLiteral("id"), inherited_id},
            {QStringLiteral("upstreams"), QJsonArray{QStringLiteral("backup")}}
        }).toJson(QJsonDocument::Compact));
    if (!expect(selected.code == 200, "save inherited provider backup-only route")) return 1;
    const QJsonObject routed = providerById(request(&manager, QUrl(server.base_url +
        QStringLiteral("/map/v1/providers/available")), server.api_key,
        "GET").body, inherited_id);
    if (!expect(routed.value(QStringLiteral("selected_upstreams")).toArray() ==
                QJsonArray{QStringLiteral("backup")},
                "effective route preserves the selected upstream subset")) return 1;
    QHash<QString, QStringList> restored_routes;
    if (!expect(repository->readProviderUpstreamRoutes(&restored_routes, nullptr, &error) &&
                restored_routes.value(inherited_id) == QStringList{QStringLiteral("backup")},
                "inherited route survives a repository reopen")) return 1;

    const Result inherited_direct = request(&manager,
        QUrl(server.base_url + QStringLiteral("/map/v1/providers/acquisition")),
        server.delete_api_key, "PUT", QJsonDocument(QJsonObject{
            {QStringLiteral("id"), inherited_id},
            {QStringLiteral("mode"), QStringLiteral("direct")}
        }).toJson(QJsonDocument::Compact));
    if (!expect(inherited_direct.code == 403 &&
                providerById(request(&manager, QUrl(server.base_url +
                    QStringLiteral("/map/v1/providers/available")), server.api_key,
                    "GET").body, inherited_id).value(QStringLiteral("acquisition_mode")).toString() ==
                    QStringLiteral("cascade"),
                "inherited provider can never switch to origin-direct mode")) return 1;

    // Closing both upstreams must not remove the validated on-disk catalog.
    // A new MAP ProviderCatalog instance loads that exact saved snapshot.
    primary.stop();
    backup.stop();
    Aowis::Map::ProviderCatalog *live_catalog =
        bridge.findChild<Aowis::Map::ProviderCatalog *>();
    if (!expect(live_catalog != nullptr, "Standalone owns MAP catalog")) return 1;
    live_catalog->refresh();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    if (!expect(providerById(request(&manager, QUrl(server.base_url +
                    QStringLiteral("/map/v1/providers/available")), server.api_key,
                    "GET").body, inherited_id).value(QStringLiteral("enabled")).toBool(),
                "disconnected upstream retains discovered provider")) return 1;
    Aowis::Map::ProviderCatalog restarted(
        QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
            .filePath(QStringLiteral("maptiles")));
    restarted.configure(stored, cascade);
    bool recovered = false;
    for (const Aowis::Map::AvailableMapProvider &entry : restarted.available())
        if (entry.inherited && entry.provider.id == inherited_id &&
            entry.upstream_names == QStringList({QStringLiteral("primary"),
                                                 QStringLiteral("backup")}))
            recovered = true;
    if (!expect(recovered, "new MAP catalog restores upstream snapshot offline")) return 1;
    return 0;
}
