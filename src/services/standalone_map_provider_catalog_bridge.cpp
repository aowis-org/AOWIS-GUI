#include "services/standalone_map_provider_catalog_bridge.h"

#include "config/gui_preferences.h"

#include <QCoreApplication>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QTimer>
#include <QDebug>
#include <QSet>
#include <QTcpSocket>
#include <QUrl>
#include <QUuid>

namespace
{
const QString ProviderStore = QStringLiteral("embedded_map_providers");
constexpr int ProviderVersion = 1;
constexpr int MaximumRequestBytes = 256 * 1024;
constexpr int MaximumProviders = 64;

QJsonObject providerJson(const Aowis::Map::MapProvider &provider)
{
    return {
        {QStringLiteral("id"), provider.id},
        {QStringLiteral("name"), provider.name},
        {QStringLiteral("url"), provider.url_template},
        {QStringLiteral("coverage"), provider.coverage},
        {QStringLiteral("minimum_zoom"), provider.minimum_zoom},
        {QStringLiteral("maximum_zoom"), provider.maximum_zoom},
        {QStringLiteral("enabled"), provider.enabled},
        {QStringLiteral("remote_fetch_enabled"), provider.remote_fetch_enabled},
        {QStringLiteral("allow_lossy_transfer"), provider.allow_lossy_transfer}
    };
}

bool parseProviders(const QJsonArray &array, const QVector<Aowis::Map::MapProvider> &existing,
                    QVector<Aowis::Map::MapProvider> *parsed, QString *error)
{
    if (array.size() > MaximumProviders)
    {
        *error = QStringLiteral("Too many map providers");
        return false;
    }
    QVector<Aowis::Map::MapProvider> result;
    QSet<QString> ids;
    for (const QJsonValue &value : array)
    {
        if (!value.isObject())
        {
            *error = QStringLiteral("Invalid provider entry");
            return false;
        }
        const QJsonObject object = value.toObject();
        Aowis::Map::MapProvider provider;
        provider.id = object.value(QStringLiteral("id")).toString().trimmed().toLower();
        provider.name = object.value(QStringLiteral("name")).toString().trimmed();
        provider.url_template = object.value(QStringLiteral("url")).toString().trimmed();
        provider.coverage = object.value(QStringLiteral("coverage")).toString().trimmed();
        provider.minimum_zoom = object.value(QStringLiteral("minimum_zoom")).toInt(-1);
        provider.maximum_zoom = object.value(QStringLiteral("maximum_zoom")).toInt(-1);
        provider.enabled = object.value(QStringLiteral("enabled")).toBool(true);
        provider.remote_fetch_enabled = object.value(QStringLiteral("remote_fetch_enabled")).toBool(true);
        provider.allow_lossy_transfer = object.value(QStringLiteral("allow_lossy_transfer")).toBool(true);
        // The renderer's current PUT omits allow_lossy_transfer. Preserve it.
        if (!object.contains(QStringLiteral("allow_lossy_transfer")))
            for (const Aowis::Map::MapProvider &old : existing)
                if (old.id == provider.id) provider.allow_lossy_transfer = old.allow_lossy_transfer;
        const QUrl url(provider.url_template);
        if (QUuid(provider.id).isNull() || ids.contains(provider.id) ||
            provider.name.isEmpty() || provider.name.contains(QLatin1Char('\n')) ||
            provider.name.contains(QLatin1Char('\r')) ||
            provider.url_template.contains(QLatin1Char('\n')) ||
            provider.url_template.contains(QLatin1Char('\r')) ||
            (!provider.coverage.isEmpty()) ||
            (url.scheme() != QStringLiteral("http") && url.scheme() != QStringLiteral("https")) ||
            url.host().isEmpty() || !url.userInfo().isEmpty() ||
            !provider.url_template.contains(QStringLiteral("{z}")) ||
            !provider.url_template.contains(QStringLiteral("{x}")) ||
            !provider.url_template.contains(QStringLiteral("{y}")) ||
            provider.minimum_zoom < 0 || provider.maximum_zoom > 30 ||
            provider.minimum_zoom > provider.maximum_zoom)
        {
            *error = QStringLiteral("Invalid provider ID, URL, coverage or zoom range: %1").arg(provider.name);
            return false;
        }
        ids.insert(provider.id);
        result.append(provider);
    }
    *parsed = result;
    return true;
}

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
    this->map_providers = Aowis::Map::defaultMapProviders();
    const QJsonObject stored = GuiPreferences::group(ProviderStore);
    if (!stored.isEmpty())
    {
        const QJsonArray array = stored.value(QStringLiteral("providers")).toArray();
        QVector<Aowis::Map::MapProvider> parsed;
        QString error;
        if (stored.value(QStringLiteral("version")).toInt(-1) == ProviderVersion &&
            stored.value(QStringLiteral("providers")).isArray() &&
            parseProviders(array, this->map_providers, &parsed, &error))
            this->map_providers = parsed;
        else
            qWarning().noquote() << "Invalid embedded MAP provider catalog; using defaults:" << error;
    }
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
    }
    return config;
}

QByteArray StandaloneMapProviderCatalogBridge::catalogJson() const
{
    QJsonArray array;
    for (const Aowis::Map::MapProvider &provider : this->map_providers)
        array.append(providerJson(provider));
    return QJsonDocument(QJsonObject{{QStringLiteral("providers"), array}})
        .toJson(QJsonDocument::Compact);
}

bool StandaloneMapProviderCatalogBridge::storeCatalog(const QByteArray &body, QString *error)
{
    QJsonParseError parse_error;
    const QJsonDocument document = QJsonDocument::fromJson(body, &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject() ||
        !document.object().value(QStringLiteral("providers")).isArray())
    {
        *error = QStringLiteral("Expected a providers array");
        return false;
    }
    QVector<Aowis::Map::MapProvider> parsed;
    if (!parseProviders(document.object().value(QStringLiteral("providers")).toArray(),
                        this->map_providers, &parsed, error))
        return false;
    QJsonArray array;
    for (const Aowis::Map::MapProvider &provider : parsed) array.append(providerJson(provider));
    if (!GuiPreferences::update(ProviderStore, QJsonObject{
            {QStringLiteral("version"), ProviderVersion},
            {QStringLiteral("providers"), array}}))
    {
        *error = QStringLiteral("Could not persist MAP providers");
        return false;
    }
    this->map_providers = parsed;
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
    if (route.size() < 2 || route.at(1) != "/map/v1/providers")
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
    if (route.first() == "GET")
    {
        sendResponse(socket, 200, catalogJson());
        return;
    }
    if (route.first() == "PUT")
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
