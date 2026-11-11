#include "services/standalone_map_provider_catalog_bridge.h"


#include <QCoreApplication>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <QDebug>
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
