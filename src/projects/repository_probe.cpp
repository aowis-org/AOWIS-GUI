#include "projects/repository_probe.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <utility>

#ifdef AOWIS_GUI_WIREGUARD_PROBE
#include <aowis/network/qt_http_client.h>
#include <QPointer>
#include <QFile>
#include <QFutureWatcher>
#include <QSet>

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

struct TunnelStart
{
    std::shared_ptr<aowis::network::WireGuardClient> tunnel;
    QString error;
};

TunnelStart startTunnel(const QString &profile)
{
    QFile file(profile);
    if (!file.open(QIODevice::ReadOnly))
        return {{}, QObject::tr("Could not open the WireGuard profile.")};
    aowis::network::WireGuardConfig config;
    if (!readProfile(file, config))
        return {{}, QObject::tr("Expected a single-interface, single-peer WireGuard profile.")};
    std::shared_ptr<aowis::network::WireGuardClient> tunnel = std::make_shared<aowis::network::WireGuardClient>();
    std::string error;
    if (!tunnel->start(config, &error))
        return {{}, QString::fromStdString(error)};
    return {tunnel, {}};
}
#endif
}

DvmdbConnection::DvmdbConnection(const QUrl &endpoint, const QString &profile, QObject *parent)
    : QObject(parent), endpoint(endpoint), profile(profile)
{
    if (profile.isEmpty()) this->direct = std::make_unique<QNetworkAccessManager>();
}

DvmdbConnection::~DvmdbConnection() = default;

void DvmdbConnection::open(std::function<void(const QString &)> finished)
{
    if (this->direct)
    {
        finished({});
        return;
    }
#ifdef AOWIS_GUI_WIREGUARD_PROBE
    if (this->http)
    {
        finished({});
        return;
    }
    this->opening.append(std::move(finished));
    if (this->opening.size() > 1) return;
    QFutureWatcher<TunnelStart> *watcher = new QFutureWatcher<TunnelStart>(this);
    connect(watcher, &QFutureWatcher<TunnelStart>::finished, this, [this, watcher]() {
        const TunnelStart result = watcher->result();
        watcher->deleteLater();
        QString error = result.error;
        if (result.tunnel)
        {
            std::unique_ptr<aowis::network::QtSocks5HttpClient> http =
                std::make_unique<aowis::network::QtSocks5HttpClient>(result.tunnel->proxy(), this->endpoint);
            error = http->configurationError();
            if (http->isConfigured())
            {
                this->tunnel = result.tunnel;
                this->http = std::move(http);
            }
        }
        QList<std::function<void(const QString &)>> callbacks;
        callbacks.swap(this->opening);
        const QPointer<DvmdbConnection> alive(this);
        for (const std::function<void(const QString &)> &callback : callbacks)
        {
            if (!alive) break;
            callback(error);
        }
    });
    watcher->setFuture(QtConcurrent::run(startTunnel, this->profile));
#else
    finished(QObject::tr("WireGuard is unavailable in this build."));
#endif
}

QNetworkReply *DvmdbConnection::get(const QNetworkRequest &request)
{
    if (this->direct) return this->direct->get(request);
#ifdef AOWIS_GUI_WIREGUARD_PROBE
    if (this->http) return this->http->get(request);
#endif
    return nullptr;
}

bool DvmdbConnection::matches(const QUrl &url, const QString &path) const
{
    return this->endpoint == url && this->profile == path;
}

QNetworkReply *DvmdbConnection::write(const QNetworkRequest &request, const QByteArray &method,
                                      const QByteArray &body)
{
    if (this->direct)
        return method == "POST" ? this->direct->post(request, body)
                                : this->direct->sendCustomRequest(request, method, body);
#ifdef AOWIS_GUI_WIREGUARD_PROBE
    if (this->http)
        return method == "POST" ? this->http->post(request, body)
                                : this->http->sendCustomRequest(request, method, body);
#endif
    return nullptr;
}

void DvmdbConnection::verify(const QNetworkRequest &request, std::function<void(RepositoryProbeResult)> finished)
{
    this->open([this, request, finished](const QString &error) {
        if (!error.isEmpty())
        {
            finished({{}, 0, error, false});
            return;
        }
        QNetworkReply *reply = this->get(request);
        if (reply == nullptr)
        {
            finished({{}, 0, QObject::tr("WireGuard is unavailable in this build."), false});
            return;
        }
        QTimer::singleShot(request.transferTimeout(), reply, [reply]() {
            if (!reply->isFinished()) reply->abort();
        });
        connect(reply, &QNetworkReply::finished, this, [reply, finished]() {
            const RepositoryProbeResult result = readReply(reply);
            reply->deleteLater();
            finished(result);
        });
    });
}

void probeRepository(const QNetworkRequest &request, const QString &profile, QObject *context,
                     std::function<void(RepositoryProbeResult)> finished)
{
    DvmdbConnection *connection = new DvmdbConnection(request.url(), profile, context);
    connection->verify(request, [connection, finished](RepositoryProbeResult result) {
        connection->deleteLater();
        finished(result);
    });
}
