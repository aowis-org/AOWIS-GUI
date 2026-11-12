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
                            {QStringLiteral("upstreams"), QJsonArray{QStringLiteral("central"),
                                                                      QStringLiteral("backup")}},
                            {QStringLiteral("selected_upstreams"), this->selected_upstreams},
                            {QStringLiteral("acquisition_mode"), this->mode},
                            {QStringLiteral("direct_fetch_allowed"), false}};
                        QJsonArray advertised;
                        if (this->only_local)
                        {
                            advertised.append(QJsonObject{
                                {QStringLiteral("id"), QStringLiteral("0381bc44-67e9-5af9-aeb0-023ccba36658")},
                                {QStringLiteral("name"), QStringLiteral("OpenStreetMap")},
                                {QStringLiteral("origin"), QStringLiteral("local")},
                                {QStringLiteral("upstreams"), this->local_also_upstream ?
                                    QJsonArray{QStringLiteral("central")} : QJsonArray{}}});
                        }
                        else advertised.append(provider);
                        body = QJsonDocument(QJsonObject{{QStringLiteral("providers"),
                            advertised}}).toJson(QJsonDocument::Compact);
                        code = 200;
                    }
                    else if (header.startsWith("PUT /map/v1/providers/acquisition") && admin_key)
                    {
                        const QJsonObject update = QJsonDocument::fromJson(
                            data.mid(header_end + 4, body_length)).object();
                        if (update.value(QStringLiteral("id")).toString() == ProviderId &&
                            (update.value(QStringLiteral("mode")).toString() ==
                                QStringLiteral("cascade") ||
                             update.value(QStringLiteral("mode")).toString() ==
                                QStringLiteral("automatic")))
                        {
                            this->mode = update.value(QStringLiteral("mode")).toString();
                            ++this->mode_updates;
                            code = 200;
                        }
                    }
                    else if (header.startsWith("PUT /map/v1/providers/upstreams") && admin_key)
                    {
                        const QJsonObject update = QJsonDocument::fromJson(
                            data.mid(header_end + 4, body_length)).object();
                        const QJsonArray requested = update.value(QStringLiteral("upstreams")).toArray();
                        if (update.value(QStringLiteral("id")).toString() == ProviderId &&
                            update.value(QStringLiteral("upstreams")).isArray())
                        {
                            this->selected_upstreams = requested.isEmpty() ?
                                QJsonArray{QStringLiteral("central"), QStringLiteral("backup")} :
                                requested;
                            ++this->route_updates;
                            code = 200;
                        }
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
    int route_updates = 0;
    int mode_updates = 0;
    QString mode = QStringLiteral("cascade");
    QJsonArray selected_upstreams{QStringLiteral("central"), QStringLiteral("backup")};
    bool only_local = false;
    bool local_also_upstream = false;
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
        client.providers().at(0).upstreams ==
            QStringList({QStringLiteral("central"), QStringLiteral("backup")}),
        "inherited metadata")) return 1;
    if (!expect(client.routes().size() == 1 &&
                client.routes().at(0).selected_upstreams ==
                    QStringList({QStringLiteral("central"), QStringLiteral("backup")}),
                "MAP route catalog includes effective priorities")) return 1;
    if (!expect(client.routes().at(0).acquisition_mode == QStringLiteral("cascade") &&
                !client.routes().at(0).direct_fetch_allowed,
                "Read inherited provider acquisition policy")) return 1;
    client.setAcquisitionMode(ProviderId, QStringLiteral("direct"));
    if (!expect(!client.updating() && server.mode_updates == 0,
                "Client never offers forbidden inherited Direct mode")) return 1;
    client.setAcquisitionMode(ProviderId, QStringLiteral("automatic"));
    if (!expect(pumpUntil([&client, &server]() {
        return server.mode_updates == 1 && !client.updating() &&
            client.routes().size() == 1 &&
            client.routes().at(0).acquisition_mode == QStringLiteral("automatic");
    }), "Change provider acquisition policy over authenticated REST")) return 1;
    client.setUpstreamRoute(ProviderId,
        {QStringLiteral("backup"), QStringLiteral("central")});
    if (!expect(pumpUntil([&client, &server]() {
        return server.route_updates == 1 && !client.updating() &&
            client.routes().size() == 1 &&
            client.routes().at(0).selected_upstreams ==
                QStringList({QStringLiteral("backup"), QStringLiteral("central")});
    }), "Change provider-specific upstream priority through REST")) return 1;
    client.setUpstreamRoute(ProviderId, {});
    if (!expect(pumpUntil([&client, &server]() {
        return server.route_updates == 2 && !client.updating() &&
            client.routes().size() == 1 &&
            client.routes().at(0).selected_upstreams.first() == QStringLiteral("central");
    }), "Reset provider upstream order to default")) return 1;
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

    server.only_local = true;
    server.local_also_upstream = true;
    client.refresh();
    if (!expect(pumpUntil([&client]() {
        return client.providers().isEmpty() && client.localProviderCount() == 1 &&
            client.locallyDefinedWithUpstreams() == 1;
    }), "local provider with matching upstream is advertised, not inherited")) return 1;
    server.local_also_upstream = false;
    client.refresh();
    if (!expect(pumpUntil([&client]() {
        return client.localProviderCount() == 1 && client.locallyDefinedWithUpstreams() == 0;
    }), "local provider without matching upstream is still advertised")) return 1;
    return 0;
}
