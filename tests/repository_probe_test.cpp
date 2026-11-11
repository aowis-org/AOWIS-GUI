#include "projects/repository_probe.h"

#include "projects/dvmdb_identity.h"
#include "config/wireguard_profile_storage.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QCoreApplication>
#include <QEventLoop>
#include <QHostAddress>
#include <QNetworkRequest>
#include <QTcpServer>
#include <QFile>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <cstdio>

namespace
{
RepositoryProbeResult receive(const QUrl &url, const QString &profile)
{
    QNetworkRequest request(url);
    request.setTransferTimeout(4000);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    QEventLoop loop;
    bool received = false;
    RepositoryProbeResult result;
    probeRepository(request, profile, &loop, [&](RepositoryProbeResult response) {
        result = response;
        received = true;
        loop.quit();
    });
    QTimer::singleShot(12000, &loop, &QEventLoop::quit);
    if (!received) loop.exec();
    return result;
}

QString repositoryId(const QUrl &url, const QString &profile)
{
    const RepositoryProbeResult result = receive(url, profile);
    if (!result.transport_ok || result.status != 200) return {};
    return QJsonDocument::fromJson(result.body).object().value(QStringLiteral("repositoryId")).toString();
}
}

int main(int argc, char **argv)
{
    // WireGuard profiles are read from the GUI's AOWIS-CONFIG store; keep it
    // isolated from the developer's real configuration.
    QTemporaryDir configuration;
    if (!configuration.isValid()) return 1;
    qputenv("XDG_CONFIG_HOME", configuration.path().toUtf8());
    QCoreApplication application(argc, argv);
    if (argc == 4)
    {
        // Manual check: argv[3] is a .conf file, stored first like an import.
        QFile conf(QString::fromLocal8Bit(argv[3]));
        QString profile_id;
        QString error;
        if (!conf.open(QIODevice::ReadOnly)
            || !importWireGuardProfile(QStringLiteral("Manual probe"), conf.readAll(), &profile_id, &error))
        {
            std::fprintf(stderr, "Could not store the WireGuard profile: %s\n", qPrintable(error));
            return 1;
        }
        const QString direct = repositoryId(QUrl(QString::fromLocal8Bit(argv[1])), {});
        const QString tunneled = repositoryId(QUrl(QString::fromLocal8Bit(argv[2])), profile_id);
        if (!DvmdbIdentity::isUuidV7(direct) || direct != tunneled)
        {
            std::fprintf(stderr, "Direct and tunneled probes did not return the same valid repository ID.\n");
            return 1;
        }
        std::printf("Direct and tunneled repository UUID: %s\n", direct.toLatin1().constData());
        return 0;
    }
    if (argc != 1) return 2;
    QTcpServer server;
    if (!server.listen(QHostAddress::LocalHost)) return 1;
    int connections = 0;
    const QByteArray identity = R"({"repositoryId":"019a6600-0000-7000-8000-000000000001"})";
    QObject::connect(&server, &QTcpServer::newConnection, &server, [&]() {
        ++connections;
        QTcpSocket *socket = server.nextPendingConnection();
        QObject::connect(socket, &QTcpSocket::readyRead, socket, [socket, identity]() {
            if (!socket->peek(socket->bytesAvailable()).contains("\r\n\r\n")) return;
            if (!socket->readAll().startsWith("GET /dvmdb/v1/repository HTTP/")) return;
            socket->write("HTTP/1.1 200 OK\r\nContent-Length: " + QByteArray::number(identity.size())
                          + "\r\nConnection: close\r\n\r\n" + identity);
            socket->disconnectFromHost();
        });
    });
    const QUrl url(QStringLiteral("http://127.0.0.1:%1/dvmdb/v1/repository").arg(server.serverPort()));
    // Stored but unusable (no [Interface]), and an id naming no profile: both
    // must fail before any connection is made, never falling back to direct.
    QString invalid_profile;
    QString import_error;
    if (!importWireGuardProfile(QStringLiteral("Invalid"), QByteArrayLiteral("[Peer]\n"), &invalid_profile, &import_error))
        return 1;
    const QString missing_profile = DvmdbIdentity::newUuidV7();
    for (int attempt = 0; attempt < 4; ++attempt)
    {
        const QString profile = attempt == 1 ? invalid_profile : attempt == 2 ? missing_profile : QString();
        const RepositoryProbeResult result = receive(url, profile);
        if (connections != (attempt == 3 ? 2 : 1)
            || (profile.isEmpty() && (!result.transport_ok || result.status != 200 || result.body != identity))
            || (!profile.isEmpty() && (result.transport_ok || result.status != 0 || result.error.isEmpty())))
        {
            std::fprintf(stderr, "Repository probe routing failed on attempt %d\n", attempt);
            return 1;
        }
    }
    return 0;
}
