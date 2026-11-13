#pragma once

#include <QByteArray>
#include <QString>
#include <QList>
#include <QObject>
#include <QUrl>
#include <memory>
#include <functional>

class QNetworkAccessManager;
class QNetworkReply;
namespace aowis::network { class WireGuardClient; class QtSocks5HttpClient; }
class QNetworkRequest;

struct RepositoryProbeResult
{
    QByteArray body;
    int status = 0;
    QString error;
    bool transport_ok = false;
};

// One transport per remote database. All Qt HTTP work uses the owning thread.
// wireguard_profile_id empty: direct HTTP. Otherwise the ID of a stored
// WireGuard profile (see config/wireguard_profile_storage.h); the tunnel
// fails closed if that profile is missing or unusable. Connections using the
// same profile share one tunnel (one WireGuard peer session per key).
class DvmdbConnection final : public QObject
{
public:
    DvmdbConnection(const QUrl &endpoint, const QString &wireguard_profile_id, QObject *parent);
    ~DvmdbConnection() override;
    void open(std::function<void(const QString &)> finished);
    // Call only after open() succeeds.
    QNetworkReply *get(const QNetworkRequest &request);
    QNetworkReply *write(const QNetworkRequest &request, const QByteArray &method, const QByteArray &body);
    bool matches(const QUrl &url, const QString &wireguard_profile_id) const;
    // get()/write() can be used: direct, or the WireGuard transport is up.
    bool isOpen() const;
    void verify(const QNetworkRequest &request, std::function<void(RepositoryProbeResult)> finished);

private:
    QUrl endpoint;
    QString profile;
    std::unique_ptr<QNetworkAccessManager> direct;
#ifdef AOWIS_GUI_WIREGUARD_PROBE
    std::shared_ptr<aowis::network::WireGuardClient> tunnel;
    std::unique_ptr<aowis::network::QtSocks5HttpClient> http;
    QList<std::function<void(const QString &)>> opening;
    bool restart_observed = false;
#endif
};

void probeRepository(const QNetworkRequest &request, const QString &wireguard_profile_id, QObject *context,
                     std::function<void(RepositoryProbeResult)> finished);
