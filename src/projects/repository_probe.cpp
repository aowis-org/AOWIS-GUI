#include "projects/repository_probe.h"
#include "network/wireguard_runtime_status.h"

#include <QCoreApplication>
#include <QHash>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <utility>

#ifdef AOWIS_GUI_WIREGUARD_PROBE
#include "network/wireguard_tunnel_pool.h"

#include <aowis/network/qt_http_client.h>
#include <aowis/network/wireguard_client.h>
#include <QPointer>
#endif

namespace
{
RepositoryProbeResult readReply(QNetworkReply *reply)
{
    return {reply->readAll(), reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(),
            reply->errorString(), reply->error() == QNetworkReply::NoError};
}


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
    // The HTTP adapter must be gone before this consumer releases the shared
    // tunnel (stopped only when its last consumer releases it). The runtime
    // monitor observes the actual lifetime, never merely a saved reference.
    this->http.reset();
    this->tunnel.reset();
    WireGuardRuntimeStatus::instance().stopped(this);
#endif
}

void DvmdbConnection::open(std::function<void(const QString &)> finished)
{
#ifdef AOWIS_GUI_WIREGUARD_PROBE
    if (!this->restart_observed && !this->direct)
    {
        // An edited profile: drop the stopped tunnel and reopen through the
        // restarted one (never direct).
        this->restart_observed = true;
        onWireGuardTunnelRestart(this, [this](const QString &profile_id)
        {
            if (profile_id != this->profile || !this->http) return;
            this->http.reset();
            this->tunnel.reset();
            WireGuardRuntimeStatus::instance().stopped(this);
            this->open([](const QString &) {});
        });
    }
#endif
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
    const QPointer<DvmdbConnection> self(this);
    acquireWireGuardTunnel(this->profile, [self](const WireGuardTunnelLease &result) {
        if (!self) return; // Closed while the shared tunnel was starting.
        QString error = result.error;
        if (result.tunnel)
        {
            std::unique_ptr<aowis::network::QtSocks5HttpClient> http =
                std::make_unique<aowis::network::QtSocks5HttpClient>(result.tunnel->proxy(), self->endpoint);
            error = http->configurationError();
            if (!http->isConfigured() && error.isEmpty())
                error = QObject::tr("Unable to configure the WireGuard HTTP transport.");
            if (http->isConfigured())
            {
                self->tunnel = result.tunnel;
                self->http = std::move(http);
                WireGuardRuntimeStatus::instance().running(self, wireGuardTelemetrySampler(self->tunnel));
            }
        }
        if (!result.tunnel && error.isEmpty())
            error = QObject::tr("WireGuard tunnel failed to start.");
        if (!error.isEmpty()) WireGuardRuntimeStatus::instance().failed(self, error);
        QList<std::function<void(const QString &)>> callbacks;
        callbacks.swap(self->opening);
        for (const std::function<void(const QString &)> &callback : callbacks)
        {
            if (!self) break;
            callback(error);
        }
    });
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

bool DvmdbConnection::isOpen() const
{
    if (this->direct) return true;
#ifdef AOWIS_GUI_WIREGUARD_PROBE
    return this->http != nullptr;
#else
    return false;
#endif
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
