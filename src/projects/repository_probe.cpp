#include "projects/repository_probe.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>

#ifdef AOWIS_GUI_WIREGUARD_PROBE
#include <aowis/network/qt_http_client.h>
#include <QEventLoop>
#include <QFile>
#include <QFutureWatcher>
#include <QSet>
#include <QTimer>
#include <QtConcurrent/qtconcurrentrun.h>
#endif

namespace
{
RepositoryProbeResult readReply(QNetworkReply *reply)
{
    return {reply->readAll(), reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(),
            reply->errorString(), reply->error() == QNetworkReply::NoError};
}

#ifdef AOWIS_GUI_WIREGUARD_PROBE
// Accept one userspace interface and one peer. Reject unsupported directives
// so a profile cannot silently acquire a different meaning inside the GUI.
bool readProfile(QFile &file, aowis::network::WireGuardConfig &config)
{
    QByteArray section;
    QSet<QByteArray> sections;
    QSet<QByteArray> fields;
    while (!file.atEnd())
    {
        QByteArray line = file.readLine().split('#').first().trimmed();
        if (line.isEmpty()) continue;
        if (line == "[Interface]" || line == "[Peer]")
        {
            if (sections.contains(line)) return false;
            sections.insert(line);
            section = line;
            continue;
        }
        const qsizetype equals = line.indexOf('=');
        if (equals <= 0) return false;
        const QByteArray field = line.left(equals).trimmed();
        const QByteArray value = line.mid(equals + 1).trimmed();
        if (value.isEmpty() || fields.contains(field)) return false;
        fields.insert(field);
        if (section == "[Interface]" && field == "Address") config.address = value.toStdString();
        else if (section == "[Interface]" && field == "PrivateKey") config.privateKey = value.toStdString();
        else if (section == "[Peer]" && field == "PublicKey") config.peerPublicKey = value.toStdString();
        else if (section == "[Peer]" && field == "PresharedKey") config.preSharedKey = value.toStdString();
        else if (section == "[Peer]" && field == "Endpoint") config.endpoint = value.toStdString();
        else if ((section == "[Interface]" && field == "DNS") || (section == "[Peer]" && field == "AllowedIPs"))
        {
            std::vector<std::string> &values = field == "DNS" ? config.dnsServers : config.allowedIPs;
            for (const QByteArray &item : value.split(',')) values.push_back(item.trimmed().toStdString());
        }
        else if ((section == "[Interface]" && field == "MTU") || (section == "[Peer]" && field == "PersistentKeepalive"))
        {
            bool valid = false;
            const int number = value.toInt(&valid);
            if (!valid) return false;
            if (field == "MTU") config.mtu = number;
            else config.persistentKeepaliveSeconds = number;
        }
        else return false;
    }
    return file.error() == QFileDevice::NoError && sections.size() == 2;
}

RepositoryProbeResult probeThroughTunnel(const QString &profile, const QNetworkRequest &request)
{
    QFile file(profile);
    if (!file.open(QIODevice::ReadOnly))
        return {{}, 0, QObject::tr("Could not open the WireGuard profile."), false};
    aowis::network::WireGuardConfig config;
    if (!readProfile(file, config))
        return {{}, 0, QObject::tr("Expected a single-interface, single-peer WireGuard profile."), false};
    aowis::network::WireGuardClient tunnel;
    std::string error;
    if (!tunnel.start(config, &error))
        return {{}, 0, QString::fromStdString(error), false};

    aowis::network::QtSocks5HttpClient http(tunnel.proxy(), request.url());
    if (!http.isConfigured()) return {{}, 0, http.configurationError(), false};
    QNetworkReply *reply = http.get(request);
    QEventLoop loop;
    QTimer deadline;
    deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, reply, &QNetworkReply::abort);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    deadline.start(request.transferTimeout());
    if (!reply->isFinished()) loop.exec();
    return readReply(reply);
}
#endif
}

void probeRepository(const QNetworkRequest &request, const QString &profile, QObject *context,
                     std::function<void(RepositoryProbeResult)> finished)
{
    if (!profile.isEmpty())
    {
#ifdef AOWIS_GUI_WIREGUARD_PROBE
        QFutureWatcher<RepositoryProbeResult> *watcher = new QFutureWatcher<RepositoryProbeResult>(context);
        QObject::connect(watcher, &QFutureWatcher<RepositoryProbeResult>::finished, context,
                         [watcher, finished]() {
            const RepositoryProbeResult result = watcher->result();
            watcher->deleteLater();
            finished(result);
        });
        watcher->setFuture(QtConcurrent::run(probeThroughTunnel, profile, request));
#else
        finished({{}, 0, QObject::tr("WireGuard is unavailable in this build."), false});
#endif
        return;
    }
    QNetworkAccessManager *manager = new QNetworkAccessManager(context);
    QNetworkReply *reply = manager->get(request);
    QObject::connect(reply, &QNetworkReply::finished, context, [manager, reply, finished]() {
        const RepositoryProbeResult result = readReply(reply);
        manager->deleteLater();
        finished(result);
    });
}
