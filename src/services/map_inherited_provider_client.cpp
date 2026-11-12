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
