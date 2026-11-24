#include "services/map_inherited_provider_client.h"
#ifdef AOWIS_STANDALONE
#include "services/standalone_map_provider_catalog.h"
#endif

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSet>
#include <QTimer>
#include <QUuid>
#include <QUrl>

namespace
{
constexpr qsizetype MaximumCatalogBytes = 512 * 1024;

QNetworkRequest makeRequest(const MapServerRestConfiguration &server,
                            const QString &path, bool write)
{
    QString base = server.base_url.trimmed();
    while (base.endsWith(QLatin1Char('/'))) base.chop(1);
    QNetworkRequest request(QUrl(base + path));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(10000);
    request.setRawHeader("Accept", "application/json");
    const QByteArray key = write ? server.delete_api_key : server.api_key;
    if (!key.isEmpty()) request.setRawHeader("X-API-Key", key);
    if (write) request.setHeader(QNetworkRequest::ContentTypeHeader,
                                 QStringLiteral("application/json"));
    return request;
}
}

MapInheritedProviderClient::MapInheritedProviderClient(QObject *parent)
    : QObject(parent)
{
}

void MapInheritedProviderClient::setServer(const MapServerRestConfiguration &server)
{
#ifdef AOWIS_STANDALONE
    QObject::disconnect(this->embedded_connection);
    QObject::disconnect(this->embedded_local_connection);
    this->embedded = nullptr;
#endif
    ++this->request_generation;
    this->map_server = server;
    this->network.setProxy(server.proxy);
    this->has_catalog = false;
    this->entries.clear();
    this->route_entries.clear();
    this->local_provider_count = 0;
    this->local_with_upstreams = 0;
    emit catalogChanged();
}

bool MapInheritedProviderClient::canEdit() const
{
#ifdef AOWIS_STANDALONE
    if (this->embedded != nullptr) return this->embedded->isCatalogValid();
#endif
    return this->map_server.configured() && !this->map_server.delete_api_key.isEmpty();
}

#ifdef AOWIS_STANDALONE
void MapInheritedProviderClient::setEmbeddedCatalog(StandaloneMapProviderCatalog &catalog)
{
    this->setServer(MapServerRestConfiguration());
    this->embedded = &catalog;
    this->embedded_connection = connect(&catalog, &StandaloneMapProviderCatalog::providersChanged,
        this, [this]() { if (!this->change_pending) this->readEmbeddedCatalog(); },
        Qt::QueuedConnection);
    // A local change may also restore catalog validity without changing the
    // advertised provider rows.
    this->embedded_local_connection = connect(&catalog, &StandaloneMapProviderCatalog::localProvidersChanged,
            this, &MapInheritedProviderClient::readEmbeddedCatalog, Qt::QueuedConnection);
    this->readEmbeddedCatalog();
}

void MapInheritedProviderClient::readEmbeddedCatalog()
{
    if (this->embedded == nullptr) return;
    QVector<MapInheritedProviderEntry> inherited;
    QVector<MapProviderRouteEntry> routes;
    int local_count = 0;
    int local_with_upstreams = 0;
    for (const Aowis::Map::AvailableMapProvider &value : this->embedded->availableEntries())
    {
        const Aowis::Map::MapProvider &provider = value.provider;
        MapProviderRouteEntry route;
        route.id = provider.id;
        route.name = provider.name;
        route.upstreams = value.upstream_names;
        route.selected_upstreams = value.selected_upstreams;
        route.inherited = value.inherited;
        route.acquisition_mode = Aowis::Map::providerAcquisitionModeName(value.acquisition_mode);
        route.direct_fetch_allowed = value.direct_fetch_allowed && !value.inherited;
        routes.append(route);
        if (!value.inherited)
        {
            ++local_count;
            if (!route.upstreams.isEmpty()) ++local_with_upstreams;
            continue;
        }
        MapInheritedProviderEntry entry;
        entry.id = provider.id;
        entry.name = provider.name;
        entry.url = provider.url_template;
        entry.enabled = provider.enabled;
        entry.minimum_zoom = provider.minimum_zoom;
        entry.maximum_zoom = provider.maximum_zoom;
        entry.upstreams = route.upstreams;
        inherited.append(entry);
    }
    this->entries = inherited;
    this->route_entries = routes;
    this->local_provider_count = local_count;
    this->local_with_upstreams = local_with_upstreams;
    this->has_catalog = true;
    emit catalogChanged();
}

void MapInheritedProviderClient::completeEmbeddedUpdate(bool success, const QString &error)
{
    // Do not rebuild settings widgets from inside their itemChanged handler.
    // The operation itself was a direct C++ call; deliver the result next turn.
    QTimer::singleShot(0, this, [this, success, error]()
    {
        this->change_pending = false;
        this->readEmbeddedCatalog();
        emit updateFinished(success, error);
    });
}
#endif

void MapInheritedProviderClient::refresh()
{
#ifdef AOWIS_STANDALONE
    if (this->embedded != nullptr)
    {
        this->readEmbeddedCatalog();
        return;
    }
#endif
    if (!this->map_server.configured()) return;
    const quint64 generation = ++this->request_generation;
    QNetworkReply *reply = this->network.get(makeRequest(this->map_server,
        QStringLiteral("/map/v1/providers/available"), false));
    connect(reply, &QNetworkReply::finished, this, [this, reply, generation]()
    {
        const QByteArray bytes = reply->readAll();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QString request_error = reply->errorString();
        const bool network_error = reply->error() != QNetworkReply::NoError;
        reply->deleteLater();
        if (generation != this->request_generation) return;
        if (network_error || status != 200 || bytes.size() > MaximumCatalogBytes)
        {
            emit errorOccurred(QStringLiteral("MAP available providers unavailable: %1 (HTTP %2)")
                .arg(request_error).arg(status));
            return; // Retain the last known good catalog while offline.
        }
        QJsonParseError parse_error;
        const QJsonDocument document = QJsonDocument::fromJson(bytes, &parse_error);
        if (parse_error.error != QJsonParseError::NoError || !document.isObject() ||
            !document.object().value(QStringLiteral("providers")).isArray())
        {
            emit errorOccurred(QStringLiteral("Invalid MAP available-provider response"));
            return;
        }
        const QJsonArray array = document.object().value(QStringLiteral("providers")).toArray();
        if (array.size() > 256)
        {
            emit errorOccurred(QStringLiteral("MAP available-provider catalog is too large"));
            return;
        }
        QVector<MapInheritedProviderEntry> parsed;
        QVector<MapProviderRouteEntry> parsed_routes;
        QSet<QString> identities;
        int local_count = 0;
        int local_upstream_count = 0;
        for (const QJsonValue &value : array)
        {
            if (!value.isObject())
            {
                emit errorOccurred(QStringLiteral("Invalid MAP provider entry"));
                return;
            }
            const QJsonObject object = value.toObject();
            const QString origin = object.value(QStringLiteral("origin")).toString();
            if (origin != QStringLiteral("local") && origin != QStringLiteral("inherited"))
                continue;
            MapProviderRouteEntry route;
            route.id = object.value(QStringLiteral("id")).toString();
            route.name = object.value(QStringLiteral("name")).toString();
            route.inherited = origin == QStringLiteral("inherited");
            route.acquisition_mode = object.value(QStringLiteral("acquisition_mode"))
                .toString(route.inherited ? QStringLiteral("cascade") :
                           QStringLiteral("automatic"));
            route.direct_fetch_allowed = !route.inherited &&
                object.value(QStringLiteral("direct_fetch_allowed")).toBool(false);
            if (route.acquisition_mode != QStringLiteral("automatic") &&
                route.acquisition_mode != QStringLiteral("cascade") &&
                route.acquisition_mode != QStringLiteral("direct"))
            {
                emit errorOccurred(QStringLiteral("Invalid MAP provider acquisition mode"));
                return;
            }
            if (!route.direct_fetch_allowed &&
                route.acquisition_mode == QStringLiteral("direct"))
            {
                emit errorOccurred(QStringLiteral("MAP advertises forbidden direct acquisition"));
                return;
            }
            for (const QJsonValue &value : object.value(QStringLiteral("upstreams")).toArray())
                if (value.isString() && !route.upstreams.contains(value.toString()))
                    route.upstreams.append(value.toString());
            const QJsonValue chosen = object.value(QStringLiteral("selected_upstreams"));
            if (chosen.isArray())
            {
                for (const QJsonValue &value : chosen.toArray())
                {
                    if (value.isString() && route.upstreams.contains(value.toString()) &&
                        !route.selected_upstreams.contains(value.toString()))
                        route.selected_upstreams.append(value.toString());
                }
            }
            else
            {
                // Older MAP versions omit selected_upstreams: use advertised order.
                route.selected_upstreams = route.upstreams;
            }
            if (QUuid(route.id).isNull() || route.name.isEmpty())
            {
                emit errorOccurred(QStringLiteral("Invalid MAP routing provider entry"));
                return;
            }
            // Local sources with no MAP upstream still need their acquisition
            // mode visible/editable (for example, explicitly cascade-only).
            parsed_routes.append(route);
            if (origin == QStringLiteral("local"))
            {
                ++local_count;
                if (!object.value(QStringLiteral("upstreams")).toArray().isEmpty())
                    ++local_upstream_count;
                continue;
            }
            if (origin != QStringLiteral("inherited")) continue;
            MapInheritedProviderEntry entry;
            entry.id = object.value(QStringLiteral("id")).toString();
            entry.name = object.value(QStringLiteral("name")).toString();
            entry.url = object.value(QStringLiteral("url")).toString();
            entry.minimum_zoom = object.value(QStringLiteral("minimum_zoom")).toInt();
            entry.maximum_zoom = object.value(QStringLiteral("maximum_zoom")).toInt(19);
            entry.enabled = object.value(QStringLiteral("enabled")).toBool(true);
            for (const QJsonValue &upstream : object.value(QStringLiteral("upstreams")).toArray())
                if (upstream.isString()) entry.upstreams.append(upstream.toString());
            if (QUuid(entry.id).isNull() || entry.name.isEmpty() || identities.contains(entry.id))
            {
                emit errorOccurred(QStringLiteral("Invalid inherited MAP provider identity"));
                return;
            }
            identities.insert(entry.id);
            parsed.append(entry);
        }
        this->entries = parsed;
        this->route_entries = parsed_routes;
        this->local_provider_count = local_count;
        this->local_with_upstreams = local_upstream_count;
        this->has_catalog = true;
        emit catalogChanged();
    });
}

void MapInheritedProviderClient::setEnabled(const QString &id, bool enabled)
{
    if (!canEdit() || this->change_pending || QUuid(id).isNull()) return;
    bool found = false;
    for (const MapInheritedProviderEntry &entry : this->entries)
        if (entry.id == id) found = true;
    if (!found) return;
    this->change_pending = true;
#ifdef AOWIS_STANDALONE
    if (this->embedded != nullptr)
    {
        QString error;
        const bool success = this->embedded->setInheritedEnabled(id, enabled, &error);
        this->completeEmbeddedUpdate(success, error);
        return;
    }
#endif
    const QByteArray body = QJsonDocument(QJsonObject{
        {QStringLiteral("id"), id}, {QStringLiteral("enabled"), enabled}
    }).toJson(QJsonDocument::Compact);
    QNetworkReply *reply = this->network.put(makeRequest(this->map_server,
        QStringLiteral("/map/v1/providers/inherited"), true), body);
    connect(reply, &QNetworkReply::finished, this, [this, reply]()
    {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray response = reply->readAll();
        const bool success = reply->error() == QNetworkReply::NoError && status == 200;
        const QString message = success ? QString() : QStringLiteral("MAP provider update failed: HTTP %1 — %2")
            .arg(status).arg(QString::fromUtf8(response.left(256)));
        reply->deleteLater();
        this->change_pending = false;
        emit updateFinished(success, message);
        refresh(); // Server response is authoritative, never optimistic local state.
    });
}

void MapInheritedProviderClient::setUpstreamRoute(const QString &id,
                                                  const QStringList &upstream_names)
{
    if (!canEdit() || this->change_pending || QUuid(id).isNull()) return;
    const MapProviderRouteEntry *entry = nullptr;
    for (const MapProviderRouteEntry &candidate : this->route_entries)
        if (candidate.id == id) { entry = &candidate; break; }
    if (entry == nullptr) return;
    QSet<QString> unique;
    for (const QString &name : upstream_names)
    {
        if (!entry->upstreams.contains(name) || unique.contains(name)) return;
        unique.insert(name);
    }
    this->change_pending = true;
#ifdef AOWIS_STANDALONE
    if (this->embedded != nullptr)
    {
        QString error;
        const bool success = this->embedded->setUpstreamRoute(id, upstream_names, &error);
        this->completeEmbeddedUpdate(success, error);
        return;
    }
#endif
    QJsonArray names;
    for (const QString &name : upstream_names) names.append(name);
    const QByteArray body = QJsonDocument(QJsonObject{
        {QStringLiteral("id"), id}, {QStringLiteral("upstreams"), names}
    }).toJson(QJsonDocument::Compact);
    QNetworkReply *reply = this->network.put(makeRequest(this->map_server,
        QStringLiteral("/map/v1/providers/upstreams"), true), body);
    connect(reply, &QNetworkReply::finished, this, [this, reply]()
    {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray response = reply->readAll();
        const bool success = reply->error() == QNetworkReply::NoError && status == 200;
        const QString message = success ? QString() :
            QStringLiteral("MAP upstream route update failed: HTTP %1 — %2")
                .arg(status).arg(QString::fromUtf8(response.left(256)));
        reply->deleteLater();
        this->change_pending = false;
        emit updateFinished(success, message);
        refresh();
    });
}

void MapInheritedProviderClient::setAcquisitionMode(const QString &id, const QString &mode)
{
    if (!canEdit() || this->change_pending || QUuid(id).isNull()) return;
    const MapProviderRouteEntry *found = nullptr;
    for (const MapProviderRouteEntry &entry : this->route_entries)
        if (entry.id == id) { found = &entry; break; }
    if (found == nullptr ||
        (mode != QStringLiteral("automatic") && mode != QStringLiteral("cascade") &&
         mode != QStringLiteral("direct")) ||
        (mode == QStringLiteral("direct") && !found->direct_fetch_allowed)) return;
    this->change_pending = true;
#ifdef AOWIS_STANDALONE
    if (this->embedded != nullptr)
    {
        QString error;
        Aowis::Map::MapProviderAcquisitionMode parsed;
        const bool success = Aowis::Map::parseProviderAcquisitionMode(mode, &parsed) &&
            this->embedded->setAcquisitionMode(id, parsed, &error);
        this->completeEmbeddedUpdate(success, error);
        return;
    }
#endif
    const QByteArray body = QJsonDocument(QJsonObject{
        {QStringLiteral("id"), id}, {QStringLiteral("mode"), mode}
    }).toJson(QJsonDocument::Compact);
    QNetworkReply *reply = this->network.put(makeRequest(this->map_server,
        QStringLiteral("/map/v1/providers/acquisition"), true), body);
    connect(reply, &QNetworkReply::finished, this, [this, reply]()
    {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray response = reply->readAll();
        const bool success = reply->error() == QNetworkReply::NoError && status == 200;
        const QString message = success ? QString() :
            QStringLiteral("MAP provider acquisition change failed: HTTP %1 — %2")
                .arg(status).arg(QString::fromUtf8(response.left(256)));
        reply->deleteLater();
        this->change_pending = false;
        emit updateFinished(success, message);
        refresh();
    });
}
