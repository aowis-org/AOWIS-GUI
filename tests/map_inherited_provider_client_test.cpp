#include "services/map_inherited_provider_client.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QUuid>

#include <cstdio>
#include <functional>

namespace
{
const QString ProviderId = QStringLiteral("1f645e7a-3e78-4b21-b0e1-f99d319da55a");

bool expect(bool condition, const char *message)
{
    if (!condition) std::fprintf(stderr, "FAILED: %s\n", message);
    return condition;
}

bool pumpUntil(const std::function<bool()> &done)
{
    QElapsedTimer clock;
    clock.start();
    while (!done() && clock.elapsed() < 3000)
    {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 30);
        QThread::msleep(1);
    }
    return done();
}

class CatalogServer final : public QObject
{
public:
    CatalogServer()
    {
        QObject::connect(&this->tcp, &QTcpServer::newConnection, this, [this]()
        {
            while (this->tcp.hasPendingConnections())
            {
                QTcpSocket *socket = this->tcp.nextPendingConnection();
                QObject::connect(socket, &QTcpSocket::readyRead, socket,
                    [this, socket, data = QByteArray(), handled = false]() mutable
                {
                    if (handled) return;
                    data += socket->readAll();
                    const int header_end = data.indexOf("\r\n\r\n");
                    if (header_end < 0) return;
                    const QByteArray header = data.left(header_end);
                    int body_length = 0;
                    for (const QByteArray &line : header.split('\n'))
                    {
                        if (line.trimmed().toLower().startsWith("content-length:"))
                            body_length = line.trimmed().mid(15).trimmed().toInt();
                    }
                    if (data.size() < header_end + 4 + body_length) return;
                    handled = true;
                    const QByteArray lower_header = header.toLower();
                    const bool read_key = lower_header.contains("x-api-key: read-key");
                    const bool admin_key = lower_header.contains("x-api-key: admin-key");
                    QByteArray body;
                    int code = 403;
                    if (header.startsWith("GET /map/v1/providers/available") && read_key)
                    {
                        QJsonObject provider{{QStringLiteral("id"), ProviderId},
                            {QStringLiteral("name"), QStringLiteral("Inherited example")},
                            {QStringLiteral("url"), QStringLiteral("https://example.com/{z}/{x}/{y}.png")},
                            {QStringLiteral("origin"), QStringLiteral("inherited")},
                            {QStringLiteral("enabled"), this->enabled},
                            {QStringLiteral("minimum_zoom"), 3},
                            {QStringLiteral("maximum_zoom"), 14},
                            {QStringLiteral("upstreams"), QJsonArray{QStringLiteral("central")}}};
                        body = QJsonDocument(QJsonObject{{QStringLiteral("providers"),
                            QJsonArray{provider}}}).toJson(QJsonDocument::Compact);
                        code = 200;
                    }
                    else if (header.startsWith("PUT /map/v1/providers/inherited") && admin_key)
                    {
                        const QJsonObject update = QJsonDocument::fromJson(
                            data.mid(header_end + 4, body_length)).object();
                        if (update.value(QStringLiteral("id")).toString() == ProviderId &&
                            update.value(QStringLiteral("enabled")).isBool())
                        {
                            this->enabled = update.value(QStringLiteral("enabled")).toBool();
                            ++this->updates;
                            code = 200;
                        }
                    }
                    const QByteArray response = QByteArray("HTTP/1.1 ") +
                        QByteArray::number(code) + (code == 200 ? " OK\r\n" : " Forbidden\r\n") +
                        "Content-Type: application/json\r\nConnection: close\r\nContent-Length: " +
                        QByteArray::number(body.size()) + "\r\n\r\n" + body;
                    socket->write(response);
                    socket->disconnectFromHost();
                });
                QObject::connect(socket, &QTcpSocket::disconnected,
                                 socket, &QObject::deleteLater);
            }
        });
    }

    QTcpServer tcp;
    bool enabled = true;
    int updates = 0;
};
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    CatalogServer server;
    if (!expect(server.tcp.listen(QHostAddress::LocalHost, 0), "mock server listening")) return 1;

    MapServerRestConfiguration config;
    config.base_url = QStringLiteral("http://127.0.0.1:%1").arg(server.tcp.serverPort());
    config.api_key = "read-key";
    config.delete_api_key = "admin-key";
    MapInheritedProviderClient client;
    client.setServer(config);
    client.refresh();
    if (!expect(pumpUntil([&client]() { return client.loaded(); }), "read merged catalog")) return 1;
    if (!expect(client.providers().size() == 1 && client.providers().at(0).id == ProviderId &&
        client.providers().at(0).upstreams == QStringList{QStringLiteral("central")},
        "inherited metadata")) return 1;
    client.setEnabled(ProviderId, false);
    if (!expect(pumpUntil([&client, &server]() {
        return server.updates == 1 && !client.updating() &&
            client.providers().size() == 1 && !client.providers().at(0).enabled;
    }), "admin-only local disable")) return 1;

    config.delete_api_key.clear();
    client.setServer(config);
    client.refresh();
    if (!expect(pumpUntil([&client]() { return client.loaded(); }), "read-only catalog")) return 1;
    client.setEnabled(ProviderId, true);
    QCoreApplication::processEvents();
    if (!expect(!client.canEdit() && server.updates == 1 && !client.providers().at(0).enabled,
                "read-only connection cannot edit")) return 1;
    return 0;
}
