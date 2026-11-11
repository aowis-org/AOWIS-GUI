#include "projects/repository_probe.h"
#include "network/wireguard_runtime_status.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <utility>

#ifdef AOWIS_GUI_WIREGUARD_PROBE
#include "config/wireguard_profile_storage.h"

#include <aowis/network/qt_http_client.h>
#include <aowis/network/wireguard_client.h>
#include <aowis/network/wireguard_conf.h>
#include <QPointer>
#include <QFutureWatcher>
#include <string>

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
struct TunnelStart
{
    std::shared_ptr<aowis::network::WireGuardClient> tunnel;
    QString error;
};

// Runs on a worker thread. The profile text comes from the GUI's
// AOWIS-CONFIG store and is parsed by AOWIS-NETWORK's single .conf parser,
// which accepts exactly what the embedded engine can run. Any failure keeps
// the connection closed: there is never a fallback to direct HTTP.
TunnelStart startTunnel(const QString &profile_id)
{
    std::string text;
    QString error;
    if (!readWireGuardProfileText(profile_id, text, &error))
        return {{}, error};
    const aowis::network::WireGuardConfParse parsed = aowis::network::parseWireGuardConf(text);
    if (!parsed.ok())
    {
        // Diagnostics never contain profile text or keys.
        for (const aowis::network::WireGuardConfDiagnostic &diagnostic : parsed.diagnostics)
            if (diagnostic.severity == aowis::network::WireGuardConfSeverity::error)
                return {{}, diagnostic.line > 0
                    ? QObject::tr("The WireGuard profile cannot be used (line %1): %2")
                          .arg(static_cast<qulonglong>(diagnostic.line)).arg(QString::fromStdString(diagnostic.message))
                    : QObject::tr("The WireGuard profile cannot be used: %1")
                          .arg(QString::fromStdString(diagnostic.message))};
        return {{}, QObject::tr("The WireGuard profile cannot be used.")};
    }
    std::shared_ptr<aowis::network::WireGuardClient> tunnel = std::make_shared<aowis::network::WireGuardClient>();
    std::string start_error;
    if (!tunnel->start(parsed.config, &start_error))
        return {{}, QString::fromStdString(start_error)};
    return {tunnel, {}};
}

// Holds the tunnel weakly: the monitor must never extend a tunnel's lifetime
// beyond its DvmdbConnection, which stops it on destruction.
WireGuardRuntimeStatus::TelemetrySampler tunnelSampler(const std::shared_ptr<aowis::network::WireGuardClient> &tunnel)
{
    const std::weak_ptr<aowis::network::WireGuardClient> weak = tunnel;
    return [weak](WireGuardRuntimeStatus::TunnelTelemetry &out) -> bool
    {
        const std::shared_ptr<aowis::network::WireGuardClient> locked = weak.lock();
        if (!locked) return false;
        aowis::network::WireGuardTelemetry sample;
        if (!locked->telemetry(&sample)) return false;
        out.started_unix_ms = static_cast<qint64>(sample.startedUnixMs);
        out.last_handshake_unix_ms = static_cast<qint64>(sample.lastHandshakeUnixMs);
        out.received_bytes = static_cast<quint64>(sample.receivedBytes);
        out.sent_bytes = static_cast<quint64>(sample.sentBytes);
        out.persistent_keepalive_seconds = static_cast<quint64>(sample.persistentKeepaliveSeconds);
        out.active_connections = static_cast<quint64>(sample.activeProxyConnections);
        out.accepted_connections = static_cast<quint64>(sample.acceptedProxyConnections);
        out.failed_dials = static_cast<quint64>(sample.failedProxyDials);
        out.peer_endpoint = QString::fromStdString(sample.peerEndpoint);
        return true;
    };
}
#endif

// Presentation label for the settings page: the DVMDB origin, no path,
// query or credentials.
QString serviceLabel(const QUrl &endpoint)
{
    return endpoint.adjusted(QUrl::RemoveUserInfo | QUrl::RemovePath | QUrl::RemoveQuery
                             | QUrl::RemoveFragment).toString();
}
}

DvmdbConnection::DvmdbConnection(const QUrl &endpoint, const QString &wireguard_profile_id, QObject *parent)
    : QObject(parent), endpoint(endpoint), profile(wireguard_profile_id)
{
    if (wireguard_profile_id.isEmpty()) this->direct = std::make_unique<QNetworkAccessManager>();
}

DvmdbConnection::~DvmdbConnection()
{
#ifdef AOWIS_GUI_WIREGUARD_PROBE
    // The HTTP adapter must be gone before the tunnel is stopped. The runtime
    // monitor observes the actual lifetime, never merely a saved reference.
    this->http.reset();
    this->tunnel.reset();
    WireGuardRuntimeStatus::instance().stopped(this);
#endif
}

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
    WireGuardRuntimeStatus::instance().starting(this->profile, this, serviceLabel(this->endpoint));
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
            if (!http->isConfigured() && error.isEmpty())
                error = QObject::tr("Unable to configure the WireGuard HTTP transport.");
            if (http->isConfigured())
            {
                this->tunnel = result.tunnel;
                this->http = std::move(http);
                WireGuardRuntimeStatus::instance().running(this, tunnelSampler(this->tunnel));
            }
        }
        if (!result.tunnel && error.isEmpty())
            error = QObject::tr("WireGuard tunnel failed to start.");
        if (!error.isEmpty()) WireGuardRuntimeStatus::instance().failed(this, error);
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
    const QString error = QObject::tr("WireGuard is unavailable in this build.");
    WireGuardRuntimeStatus::instance().starting(this->profile, this, serviceLabel(this->endpoint));
    WireGuardRuntimeStatus::instance().failed(this, error);
    finished(error);
#endif
}

QNetworkReply *DvmdbConnection::get(const QNetworkRequest &request)
{
    if (this->direct) return this->direct->get(request);
#ifdef AOWIS_GUI_WIREGUARD_PROBE
    if (this->http)
    {
        QNetworkReply *reply = this->http->get(request);
        if (reply != nullptr)
            connect(reply, &QNetworkReply::finished, this, [this, reply] {
                // A real HTTP response through the SOCKS5 transport confirms
                // peer/service reachability, not merely local proxy startup.
                if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() >= 100)
                    WireGuardRuntimeStatus::instance().httpReachable(this);
            });
        return reply;
    }
#endif
    return nullptr;
}

bool DvmdbConnection::matches(const QUrl &url, const QString &wireguard_profile_id) const
{
    return this->endpoint == url && this->profile == wireguard_profile_id;
}

QNetworkReply *DvmdbConnection::write(const QNetworkRequest &request, const QByteArray &method,
                                      const QByteArray &body)
{
    if (this->direct)
        return method == "POST" ? this->direct->post(request, body)
                                : this->direct->sendCustomRequest(request, method, body);
#ifdef AOWIS_GUI_WIREGUARD_PROBE
    if (this->http)
    {
        QNetworkReply *reply = method == "POST" ? this->http->post(request, body)
                                : this->http->sendCustomRequest(request, method, body);
        if (reply != nullptr)
            connect(reply, &QNetworkReply::finished, this, [this, reply] {
                if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() >= 100)
                    WireGuardRuntimeStatus::instance().httpReachable(this);
            });
        return reply;
    }
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

void probeRepository(const QNetworkRequest &request, const QString &wireguard_profile_id, QObject *context,
                     std::function<void(RepositoryProbeResult)> finished)
{
    DvmdbConnection *connection = new DvmdbConnection(request.url(), wireguard_profile_id, context);
    connection->verify(request, [connection, finished](RepositoryProbeResult result) {
        connection->deleteLater();
        finished(result);
    });
}
