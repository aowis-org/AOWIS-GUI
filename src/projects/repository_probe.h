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
class DvmdbConnection final : public QObject
{
public:
    DvmdbConnection(const QUrl &endpoint, const QString &profile, QObject *parent);
    ~DvmdbConnection() override;
    void open(std::function<void(const QString &)> finished);
    // Call only after open() succeeds.
    QNetworkReply *get(const QNetworkRequest &request);
    QNetworkReply *write(const QNetworkRequest &request, const QByteArray &method, const QByteArray &body);
    bool matches(const QUrl &url, const QString &path) const;
    void verify(const QNetworkRequest &request, std::function<void(RepositoryProbeResult)> finished);

private:
    QUrl endpoint;
    QString profile;
    std::unique_ptr<QNetworkAccessManager> direct;
#ifdef AOWIS_GUI_WIREGUARD_PROBE
    std::shared_ptr<aowis::network::WireGuardClient> tunnel;
    std::unique_ptr<aowis::network::QtSocks5HttpClient> http;
    QList<std::function<void(const QString &)>> opening;
#endif
};

void probeRepository(const QNetworkRequest &request, const QString &profile, QObject *context,
                     std::function<void(RepositoryProbeResult)> finished);
