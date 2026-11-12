#include "services/standalone_map_provider_catalog_bridge.h"
#include <aowis/map/settings_repository.h>

#include <QCoreApplication>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTemporaryDir>
#include <QTimer>

#include <cstdio>
#include <memory>

namespace
{
bool expect(bool condition, const char *message)
{
    if (!condition) std::fprintf(stderr, "FAILED: %s\n", message);
    return condition;
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
    Aowis::Map::MapCascadeConfig cascade;
    cascade.allow_direct_fetch = false;
    if (!expect(setup_repository->saveCascade(cascade, {}, nullptr, &setup_error),
                "persist direct-fetch prohibition")) return 1;
    StandaloneMapProviderCatalogBridge &bridge = StandaloneMapProviderCatalogBridge::instance();
    const MapServerRestConfiguration server = bridge.catalogServer();
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
    if (!expect(available_rows.size() == 4 &&
        available_rows.at(0).toObject().value(QStringLiteral("origin")).toString() ==
            QStringLiteral("local"), "bridge distinguishes local catalog")) return 1;
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
    first.insert(QStringLiteral("url"), QStringLiteral("file:///etc/passwd"));
    providers.replace(0, first);
    catalog.insert(QStringLiteral("providers"), providers);
    const Result rejected = request(&manager, url, server.api_key, "PUT",
                                    QJsonDocument(catalog).toJson(QJsonDocument::Compact));
    if (!expect(rejected.code == 400 && changed == 1, "invalid edit rejected atomically")) return 1;
    return 0;
}
