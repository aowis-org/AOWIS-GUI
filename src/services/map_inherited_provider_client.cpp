#include "services/map_inherited_provider_client.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSet>
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

void MapInheritedProviderClient::refresh()
{
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
                for (const QJsonValue &value : chosen.toArray())
                    if (value.isString() && route.upstreams.contains(value.toString()) &&
                        !route.selected_upstreams.contains(value.toString()))
                        route.selected_upstreams.append(value.toString());
            else route.selected_upstreams = route.upstreams; // Older MAP versions.
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
    QJsonArray names;
    QSet<QString> unique;
    for (const QString &name : upstream_names)
    {
        if (!entry->upstreams.contains(name) || unique.contains(name)) return;
        unique.insert(name);
        names.append(name);
    }
    this->change_pending = true;
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
