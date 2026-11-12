#include "map/core/gui_available_map_provider_directory.h"
#include "map/core/map_model.h"

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
#include <QUrl>

#include <cstdio>
#include <functional>

namespace
{
const QString InheritedId = QStringLiteral("1f645e7a-3e78-4b21-b0e1-f99d319da55a");

bool require(bool condition, const char *reason)
{
    if (!condition) std::fprintf(stderr, "FAILED: %s\n", reason);
    return condition;
}

bool until(const std::function<bool()> &condition)
{
    QElapsedTimer clock;
    clock.start();
    while (!condition() && clock.elapsed() < 3000)
    {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 30);
        QThread::msleep(1);
    }
    return condition();
}

class AvailableServer final : public QObject
{
public:
    AvailableServer()
    {
        QObject::connect(&this->listener, &QTcpServer::newConnection, this, [this]()
        {
            while (this->listener.hasPendingConnections())
            {
                QTcpSocket *socket = this->listener.nextPendingConnection();
                QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket]()
                {
                    const QByteArray request = socket->readAll();
                    if (!request.contains("GET /map/v1/providers/available")) return;
                    QJsonArray providers;
                    const QString identifier = this->duplicate_local
                        ? MapProviderCatalog::builtinId(2) : InheritedId;
                    providers.append(QJsonObject{
                        {QStringLiteral("id"), identifier},
                        {QStringLiteral("name"), QStringLiteral("Inherited map")},
                        {QStringLiteral("url"), QStringLiteral("https://tiles.example.org/{z}/{x}/{y}.png")},
                        {QStringLiteral("origin"), QStringLiteral("inherited")},
                        {QStringLiteral("enabled"), this->enabled},
                        {QStringLiteral("minimum_zoom"), 8},
                        {QStringLiteral("maximum_zoom"), 14},
                        {QStringLiteral("upstreams"), QJsonArray{QStringLiteral("regional")}}
                    });
                    const QByteArray body = QJsonDocument(QJsonObject{
                        {QStringLiteral("providers"), providers}
                    }).toJson(QJsonDocument::Compact);
                    socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                                  "Connection: close\r\nContent-Length: " +
                                  QByteArray::number(body.size()) + "\r\n\r\n" + body);
                    socket->disconnectFromHost();
                });
                QObject::connect(socket, &QTcpSocket::disconnected,
                                 socket, &QObject::deleteLater);
            }
        });
    }

    QTcpServer listener;
    bool enabled = true;
    bool duplicate_local = false;
};
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    AvailableServer server;
    if (!require(server.listener.listen(QHostAddress::LocalHost, 0), "listen")) return 1;

    MapProviderCatalog local;
    MapInheritedProviderClient inherited;
    GuiAvailableMapProviderDirectory directory(local, inherited);
    const int local_count = local.providers().size();
    if (!require(local_count > 0 && directory.providers().size() == local_count,
                 "initial local providers remain visible")) return 1;

    MapServerRestConfiguration config;
    config.base_url = QStringLiteral("http://127.0.0.1:%1").arg(server.listener.serverPort());
    inherited.setServer(config);
    inherited.refresh();
    if (!require(until([&directory, local_count]() {
        return directory.providers().size() == local_count + 1;
    }), "inherited provider appears in renderer catalog")) return 1;
    if (!require(directory.isInherited(InheritedId) &&
                 directory.upstreams(InheritedId) == QStringList{QStringLiteral("regional")} &&
                 directory.isProviderEnabled(InheritedId), "inherited origin and upstream metadata")) return 1;
    if (!require(directory.sourceForZoom(InheritedId, 12) == InheritedId &&
                 directory.sourceForZoom(InheritedId, 15) == InheritedId &&
                 directory.sourceForZoom(InheritedId, 7).isEmpty() &&
                 directory.maximumSourceZoom(InheritedId) == 14,
                 "inherited imagery source addresses and zoom range")) return 1;
    if (!require(directory.firstEnabledProviderId() == local.firstEnabledProviderId(),
                 "local provider stays preferred")) return 1;

    MapModel model(&directory);
    QObject::connect(&directory, &GuiAvailableMapProviderDirectory::catalogChanged,
                     &model, &MapModel::providerCatalogChanged);
    model.setProviderId(InheritedId);
    if (!require(model.providerId() == InheritedId,
                 "map model accepts inherited raster layer")) return 1;

    server.enabled = false;
    inherited.refresh();
    if (!require(until([&directory]() {
        return !directory.isProviderEnabled(InheritedId);
    }), "server disable propagates")) return 1;
    if (!require(directory.sourceForZoom(InheritedId, 12).isEmpty() &&
                 directory.provider(InheritedId) != nullptr,
                 "disabled inherited provider remains listed but cannot render")) return 1;
    if (!require(model.providerId() != InheritedId &&
                 directory.isProviderEnabled(model.providerId()),
                 "model leaves a disabled inherited provider")) return 1;

    server.duplicate_local = true;
    inherited.refresh();
    if (!require(until([&directory, local_count]() {
        return directory.providers().size() == local_count;
    }), "local provider takes precedence on duplicate id")) return 1;
    if (!require(!directory.isInherited(MapProviderCatalog::builtinId(2)),
                 "duplicate cannot replace local ownership")) return 1;

    server.duplicate_local = false;
    server.enabled = true;
    inherited.refresh();
    if (!require(until([&directory]() {
        return directory.isProviderEnabled(InheritedId);
    }), "re-enable inherited provider")) return 1;
    server.listener.close();
    inherited.refresh();
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < 100)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    if (!require(directory.isProviderEnabled(InheritedId),
                 "last validated catalog remains usable without upstream")) return 1;
    return 0;
}
