#include "services/standalone_map_provider_catalog_bridge.h"


#include <QCoreApplication>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <QDebug>
#include <QStandardPaths>
#include <QDir>
#include <QJsonParseError>
#include <QTcpSocket>
#include <QUuid>

namespace
{
constexpr int MaximumRequestBytes = 256 * 1024;

void sendResponse(QTcpSocket *socket, int status, const QByteArray &body)
{
    const QByteArray reason = status == 200 ? "OK" : status == 400 ? "Bad Request" :
                              status == 403 ? "Forbidden" : status == 405 ? "Method Not Allowed" :
                              status == 413 ? "Payload Too Large" : "Not Found";
    QByteArray response = QByteArray("HTTP/1.1 ") + QByteArray::number(status) + " " + reason + "\r\n";
    response += "Content-Type: application/json\r\nCache-Control: no-store\r\n";
    response += "Connection: close\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n\r\n";
    response += body;
    socket->write(response);
    socket->disconnectFromHost();
}
}

StandaloneMapProviderCatalogBridge &StandaloneMapProviderCatalogBridge::instance()
{
    static StandaloneMapProviderCatalogBridge *bridge =
        new StandaloneMapProviderCatalogBridge(QCoreApplication::instance());
    return *bridge;
}

StandaloneMapProviderCatalogBridge::StandaloneMapProviderCatalogBridge(QObject *parent)
    : QObject(parent), listener(this)
{
    this->repository = Aowis::Map::SettingsRepository::openStandaloneForCurrentUser(
        &this->catalog_error);
    this->catalog_valid = this->repository != nullptr &&
        this->repository->readProviders(&this->map_providers,
                                        &this->provider_revision, &this->catalog_error);
    if (!this->catalog_valid)
    {
        // Do not silently restore fetching defaults on an unreadable catalog.
        qCritical().noquote() << "Invalid embedded MAP provider catalog:" << this->catalog_error;
        this->map_providers = Aowis::Map::defaultMapProviders();
        for (Aowis::Map::MapProvider &provider : this->map_providers)
            provider.remote_fetch_enabled = false;
    }
    Aowis::Map::MapCascadeConfig cascade;
    cascade.allow_direct_fetch = false;
    if (this->repository != nullptr)
    {
        QString error;
        if (!this->repository->readCascade(&cascade, nullptr, &error))
        {
            qCritical().noquote() << "Invalid MAP cascade settings:" << error;
            this->catalog_valid = false;
            this->catalog_error = error;
            cascade = Aowis::Map::MapCascadeConfig();
            cascade.allow_direct_fetch = false;
        }
        if (!this->repository->readDisabledInheritedProviders(
                &this->disabled_inherited, &this->inherited_revision, &error))
        {
            qCritical().noquote() << "Invalid inherited MAP overrides:" << error;
            this->catalog_valid = false;
            this->catalog_error = error;
            // Do not accidentally re-enable inherited sources when the local
            // persisted deny-list is unreadable. Keep local providers usable.
            cascade.upstreams.clear();
            this->disabled_inherited.clear();
        }
        if (!this->repository->readProviderAcquisitionModes(
                &this->acquisition_modes, &this->acquisition_modes_revision, &error))
        {
            qCritical().noquote() << "Invalid MAP provider acquisition modes:" << error;
            this->catalog_valid = false;
            this->catalog_error = error;
            cascade.upstreams.clear(); // Fail closed on corrupt mode policy.
            cascade.allow_direct_fetch = false;
            this->acquisition_modes.clear();
        }
        if (!this->repository->readProviderUpstreamRoutes(
                &this->upstream_routes, &this->upstream_routes_revision, &error))
        {
            qCritical().noquote() << "Invalid MAP provider routes:" << error;
            this->catalog_valid = false;
            this->catalog_error = error;
            cascade.upstreams.clear(); // Cannot safely apply corrupt routing policy.
            this->upstream_routes.clear();
        }
    }
    this->available_catalog.reset(new Aowis::Map::ProviderCatalog(
        QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
            .filePath(QStringLiteral("maptiles")), this));
    this->available_catalog->configure(this->map_providers, cascade);
    this->available_catalog->setDisabledInheritedProviders(this->disabled_inherited);
    this->available_catalog->setUpstreamRoutes(this->upstream_routes);
    this->available_catalog->setAcquisitionModes(this->acquisition_modes);
    connect(this->available_catalog.get(), &Aowis::Map::ProviderCatalog::catalogChanged,
            this, &StandaloneMapProviderCatalogBridge::providersChanged);
    this->available_catalog->refresh();
    this->access_token = QUuid::createUuid().toString(QUuid::WithoutBraces).toUtf8();
    connect(&this->listener, &QTcpServer::newConnection,
            this, &StandaloneMapProviderCatalogBridge::acceptConnections);
    if (!this->listener.listen(QHostAddress::LocalHost, 0))
        qCritical() << "Cannot start embedded MAP provider editor:" << this->listener.errorString();
}

MapServerRestConfiguration StandaloneMapProviderCatalogBridge::catalogServer() const
{
    MapServerRestConfiguration config;
    if (this->listener.isListening())
    {
        config.base_url = QStringLiteral("http://127.0.0.1:%1").arg(this->listener.serverPort());
        config.api_key = this->access_token;
        config.delete_api_key = this->access_token;
    }
    return config;
}

QVector<Aowis::Map::MapProvider> StandaloneMapProviderCatalogBridge::availableProviders() const
{
    QVector<Aowis::Map::MapProvider> result;
    if (this->available_catalog == nullptr) return this->map_providers;
    for (const Aowis::Map::AvailableMapProvider &entry : this->available_catalog->available())
        result.append(entry.provider);
    return result;
}

QHash<QString, QSet<QString>> StandaloneMapProviderCatalogBridge::inheritedUpstreams() const
{
    return this->available_catalog == nullptr ? QHash<QString, QSet<QString>>() :
        this->available_catalog->inheritedUpstreams();
}

QByteArray StandaloneMapProviderCatalogBridge::catalogJson() const
{
    return Aowis::Map::encodeProviderCatalog(this->map_providers);
}

bool StandaloneMapProviderCatalogBridge::storeCatalog(const QByteArray &body, QString *error)
{
    if (!this->catalog_valid || this->repository == nullptr)
    {
        if (error != nullptr)
            *error = this->catalog_error.isEmpty()
                ? QStringLiteral("MAP provider repository unavailable") : this->catalog_error;
        return false;
    }
    QVector<Aowis::Map::MapProvider> parsed;
    if (!Aowis::Map::decodeProviderCatalog(body, this->map_providers, &parsed, error))
        return false;
    QString revision;
    if (!this->repository->saveProviders(parsed, this->provider_revision, &revision, error))
        return false;
    this->provider_revision = revision;
    this->map_providers = parsed;
    if (this->available_catalog != nullptr)
        this->available_catalog->setLocalProviders(parsed); // Emits providersChanged once.
    else
        emit providersChanged();
    return true;
}

void StandaloneMapProviderCatalogBridge::acceptConnections()
{
    while (this->listener.hasPendingConnections())
    {
        QTcpSocket *socket = this->listener.nextPendingConnection();
        socket->setParent(this);
        // Qt may deliver a request in multiple packets. Limit both header and
        // body before parsing, and close idle connections after five seconds.
        connect(socket, &QTcpSocket::readyRead, socket,
                [this, socket, buffer = QByteArray(), completed = false]() mutable
        {
            if (completed) return;
            buffer += socket->readAll();
            if (buffer.size() > MaximumRequestBytes)
            {
                completed = true;
                sendResponse(socket, 413, "{}");
                return;
            }
            const int delimiter = buffer.indexOf("\r\n\r\n");
            if (delimiter < 0) return;
            const QByteArray header = buffer.left(delimiter);
            int length = 0;
            const QList<QByteArray> lines = header.split('\n');
            for (const QByteArray &line : lines)
            {
                const QByteArray trimmed = line.trimmed();
                if (trimmed.toLower().startsWith("content-length:"))
                {
                    bool valid = false;
                    length = trimmed.mid(15).trimmed().toInt(&valid);
                    if (!valid || length < 0 || length > MaximumRequestBytes)
                    {
                        completed = true;
                        sendResponse(socket, 413, "{}");
                        return;
                    }
                }
            }
            if (buffer.size() < delimiter + 4 + length) return;
            completed = true;
            respond(socket, buffer.left(delimiter + 4 + length));
        });
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        QTimer::singleShot(5000, socket, [socket]() { if (socket->isOpen()) socket->disconnectFromHost(); });
    }
}

void StandaloneMapProviderCatalogBridge::respond(QTcpSocket *socket, const QByteArray &request)
{
    const int delimiter = request.indexOf("\r\n\r\n");
    const QList<QByteArray> lines = request.left(delimiter).split('\n');
    const QByteArray first = lines.isEmpty() ? QByteArray() : lines.first().trimmed();
    const QList<QByteArray> route = first.split(' ');
    if (route.size() < 2 || (route.at(1) != "/map/v1/providers" &&
        route.at(1) != "/map/v1/providers/available" &&
        route.at(1) != "/map/v1/providers/inherited" &&
        route.at(1) != "/map/v1/providers/upstreams" &&
        route.at(1) != "/map/v1/providers/acquisition"))
    {
        sendResponse(socket, 404, "{}");
        return;
    }
    bool authorized = false;
    for (const QByteArray &line : lines)
    {
        const QByteArray trimmed = line.trimmed();
        if (trimmed.toLower().startsWith("x-api-key:") &&
            trimmed.mid(10).trimmed() == this->access_token)
            authorized = true;
    }
    if (!authorized)
    {
        sendResponse(socket, 403, "{}");
        return;
    }
    if (route.first() == "GET" && route.at(1) == "/map/v1/providers/available")
    {
        sendResponse(socket, 200, this->available_catalog == nullptr ? QByteArray("{}") :
            this->available_catalog->encodedAvailable());
        return;
    }
    if (route.first() == "PUT" && route.at(1) == "/map/v1/providers/inherited")
    {
        if (this->repository == nullptr || this->available_catalog == nullptr)
        {
            sendResponse(socket, 400, "{\"error\":\"MAP settings are unavailable\"}");
            return;
        }
        QJsonParseError parse_error;
        const QJsonDocument document = QJsonDocument::fromJson(
            request.mid(delimiter + 4), &parse_error);
        const QJsonObject object = document.object();
        const QString id = object.value(QStringLiteral("id")).toString();
        if (parse_error.error != QJsonParseError::NoError || !document.isObject() ||
            QUuid(id).isNull() || !object.value(QStringLiteral("enabled")).isBool())
        {
            sendResponse(socket, 400, "{\"error\":\"Invalid inherited provider request\"}");
            return;
        }
        bool inherited = false;
        for (const Aowis::Map::AvailableMapProvider &entry : this->available_catalog->available())
            if (entry.inherited && entry.provider.id == id) inherited = true;
        if (!inherited)
        {
            sendResponse(socket, 404, "{\"error\":\"Provider is not inherited\"}");
            return;
        }
        QSet<QString> disabled = this->disabled_inherited;
        if (object.value(QStringLiteral("enabled")).toBool()) disabled.remove(id);
        else disabled.insert(id);
        if (disabled != this->disabled_inherited)
        {
            QString error;
            QString revision;
            if (!this->repository->saveDisabledInheritedProviders(
                    disabled, this->inherited_revision, &revision, &error))
            {
                sendResponse(socket, 400,
                    QJsonDocument(QJsonObject{{QStringLiteral("error"), error}})
                        .toJson(QJsonDocument::Compact));
                return;
            }
            this->inherited_revision = revision;
            this->disabled_inherited = disabled;
            this->available_catalog->setDisabledInheritedProviders(disabled);
        }
        sendResponse(socket, 200, "{\"ok\":true}");
        return;
    }
    if (route.first() == "PUT" && route.at(1) == "/map/v1/providers/upstreams")
    {
        if (this->repository == nullptr || this->available_catalog == nullptr)
        {
            sendResponse(socket, 400, "{\"error\":\"MAP settings unavailable\"}");
            return;
        }
        QJsonParseError parse_error;
        const QJsonDocument document = QJsonDocument::fromJson(
            request.mid(delimiter + 4), &parse_error);
        const QJsonObject object = document.object();
        const QString id = object.value(QStringLiteral("id")).toString();
        const QJsonValue requested = object.value(QStringLiteral("upstreams"));
        if (parse_error.error != QJsonParseError::NoError || !document.isObject() ||
            QUuid(id).isNull() || !requested.isArray() || requested.toArray().size() > 32)
        {
            sendResponse(socket, 400, "{\"error\":\"Invalid route request\"}");
            return;
        }
        QStringList advertised;
        bool found = false;
        for (const Aowis::Map::AvailableMapProvider &entry : this->available_catalog->available())
            if (entry.provider.id == id)
            {
                advertised = entry.upstream_names;
                found = true;
                break;
            }
        if (!found)
        {
            sendResponse(socket, 404, "{\"error\":\"Unknown provider\"}");
            return;
        }
        QStringList chosen;
        for (const QJsonValue &value : requested.toArray())
        {
            if (!value.isString() || !advertised.contains(value.toString()) ||
                chosen.contains(value.toString()))
            {
                sendResponse(socket, 400, "{\"error\":\"Unknown or duplicate upstream\"}");
                return;
            }
            chosen.append(value.toString());
        }
        QHash<QString, QStringList> updated = this->upstream_routes;
        if (chosen.isEmpty()) updated.remove(id);
        else updated.insert(id, chosen);
        if (updated != this->upstream_routes)
        {
            QString error;
            QString revision;
            if (!this->repository->saveProviderUpstreamRoutes(
                    updated, this->upstream_routes_revision, &revision, &error))
            {
                sendResponse(socket, 409,
                    QJsonDocument(QJsonObject{{QStringLiteral("error"), error}})
                        .toJson(QJsonDocument::Compact));
                return;
            }
            this->upstream_routes_revision = revision;
            this->upstream_routes = updated;
            this->available_catalog->setUpstreamRoutes(updated);
        }
        sendResponse(socket, 200, "{\"ok\":true}");
        return;
    }
    if (route.first() == "PUT" && route.at(1) == "/map/v1/providers/acquisition")
    {
        if (this->repository == nullptr || this->available_catalog == nullptr)
        {
            sendResponse(socket, 503, "{"error":"MAP settings unavailable"}");
            return;
        }
        QJsonParseError parse_error;
        const QJsonDocument document = QJsonDocument::fromJson(
            request.mid(delimiter + 4), &parse_error);
        const QJsonObject body = document.object();
        const QString id = body.value(QStringLiteral("id")).toString();
        const QUuid uuid(id);
        Aowis::Map::MapProviderAcquisitionMode mode;
        if (parse_error.error != QJsonParseError::NoError || !document.isObject() ||
            uuid.isNull() || uuid.toString(QUuid::WithoutBraces) != id ||
            !Aowis::Map::parseProviderAcquisitionMode(
                body.value(QStringLiteral("mode")).toString(), &mode))
        {
            sendResponse(socket, 400, "{"error":"Invalid acquisition mode"}");
            return;
        }
        bool found = false;
        bool inherited = false;
        bool direct_allowed = false;
        for (const Aowis::Map::AvailableMapProvider &entry : this->available_catalog->available())
            if (entry.provider.id == id)
            {
                found = true;
                inherited = entry.inherited;
                direct_allowed = entry.direct_fetch_allowed;
                break;
            }
        if (!found)
        {
            sendResponse(socket, 404, "{"error":"Unknown provider"}");
            return;
        }
        if (mode == Aowis::Map::MapProviderAcquisitionMode::Direct && !direct_allowed)
        {
            sendResponse(socket, 403, "{"error":"Direct acquisition forbidden"}");
            return;
        }
        QHash<QString, Aowis::Map::MapProviderAcquisitionMode> updated = this->acquisition_modes;
        const Aowis::Map::MapProviderAcquisitionMode default_mode = inherited ?
            Aowis::Map::MapProviderAcquisitionMode::Cascade :
            Aowis::Map::MapProviderAcquisitionMode::Automatic;
        if (mode == default_mode) updated.remove(id);
        else updated.insert(id, mode);
        if (updated != this->acquisition_modes)
        {
            QString error;
            QString revision;
            if (!this->repository->saveProviderAcquisitionModes(
                    updated, this->acquisition_modes_revision, &revision, &error))
            {
                sendResponse(socket, 409,
                    QJsonDocument(QJsonObject{{QStringLiteral("error"), error}})
                        .toJson(QJsonDocument::Compact));
                return;
            }
            this->acquisition_modes_revision = revision;
            this->acquisition_modes = updated;
            this->available_catalog->setAcquisitionModes(updated);
        }
        sendResponse(socket, 200, "{"ok":true}");
        return;
    }
    if (route.first() == "GET" && route.at(1) == "/map/v1/providers")
    {
        sendResponse(socket, 200, catalogJson());
        return;
    }
    if (route.first() == "PUT" && route.at(1) == "/map/v1/providers")
    {
        QString error;
        if (storeCatalog(request.mid(delimiter + 4), &error))
            sendResponse(socket, 200, "{\"ok\":true}");
        else
            sendResponse(socket, 400,
                         QJsonDocument(QJsonObject{{QStringLiteral("error"), error}})
                             .toJson(QJsonDocument::Compact));
        return;
    }
    sendResponse(socket, 405, "{}");
}
