#pragma once

#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QTimer>
#include <QUrl>

#include <functional>
#include <memory>
#include <utility>

// A bounded, non-overlapping HTTP liveness probe shared by the map service
// and remote DVMDB endpoints. HTTP 200 alone is not proof of service identity;
// each caller validates the expected response. Never follows redirects.
class HttpHealthMonitor final : public QObject
{
public:
    enum class State { Checking, Healthy, Degraded, Offline };
    using Validator = std::function<bool(const QByteArray &)>;
    using Observer = std::function<void(State, const QString &)>;

    using Get = std::function<QNetworkReply *(const QNetworkRequest &)>;
    explicit HttpHealthMonitor(QObject *parent = nullptr, Get get = {})
        : QObject(parent), get(std::move(get)), timer(this)
    {
        if (!this->get) this->manager = std::make_unique<QNetworkAccessManager>();
        this->timer.setInterval(5000);
        connect(&this->timer, &QTimer::timeout, this, [this] { poll(); });
    }

    ~HttpHealthMonitor() override { stop(); }

    void start(const QUrl &url, Validator validator, Observer observer,
               const QByteArray &api_key = {}, int interval_ms = 5000)
    {
        stop();
        this->url = url;
        this->validator = std::move(validator);
        this->observer = std::move(observer);
        this->api_key = api_key;
        this->failures = 0;
        this->state = State::Checking;
        this->running = true;
        this->timer.start(qMax(1000, interval_ms));
        if (this->observer) this->observer(this->state, QString());
        poll();
    }

    void stop()
    {
        this->running = false;
        this->timer.stop();
        if (this->reply)
        {
            QNetworkReply *old = this->reply;
            this->reply.clear();
            old->abort();
        }
        this->observer = {};
    }

    void poll()
    {
        if (!this->running || this->reply) return;
        if (!this->url.isValid() || (this->url.scheme() != QStringLiteral("http")
            && this->url.scheme() != QStringLiteral("https")) || this->url.host().isEmpty())
        {
            report(false, QStringLiteral("Invalid endpoint URL"));
            return;
        }
        QNetworkRequest request(this->url);
        request.setRawHeader("Accept", "application/json, text/plain");
        if (!this->api_key.isEmpty()) request.setRawHeader("X-API-Key", this->api_key);
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                             QNetworkRequest::ManualRedirectPolicy);
        request.setTransferTimeout(3000);
        QNetworkReply *pending = this->get ? this->get(request) : this->manager->get(request);
        pending->setReadBufferSize(256 * 1024);
        this->reply = pending;
        const QPointer<QNetworkReply> guarded(pending);
        // Transfer timeout is inactivity-based; also enforce a wall-clock deadline.
        QTimer::singleShot(4000, this, [guarded]
        {
            if (guarded && !guarded->isFinished()) guarded->abort();
        });
        connect(pending, &QNetworkReply::finished, this, [this, pending]
        {
            if (pending != this->reply)
            {
                pending->deleteLater();
                return;
            }
            this->reply.clear();
            const int status = pending->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const bool transport_ok = pending->error() == QNetworkReply::NoError && status == 200;
            // Bound memory even when an unexpected server returns an enormous body.
            const QByteArray body = transport_ok ? pending->read(256 * 1024 + 1) : QByteArray();
            const bool valid = transport_ok && body.size() <= 256 * 1024
                && this->validator && this->validator(body);
            const QString error = valid ? QString()
                : transport_ok ? QStringLiteral("Unexpected service response")
                : status != 0 ? QStringLiteral("HTTP %1: %2").arg(status).arg(pending->errorString())
                : pending->errorString();
            pending->deleteLater();
            if (this->running) report(valid, error);
        });
    }

    State currentState() const { return this->state; }

private:
    void report(bool valid, const QString &error)
    {
        this->failures = valid ? 0 : this->failures + 1;
        const State next = valid ? State::Healthy
            : this->failures >= 2 ? State::Offline : State::Degraded;
        if (next != this->state || (next != State::Healthy && error != this->last_error))
        {
            this->state = next;
            this->last_error = error;
            if (this->observer) this->observer(next, error);
        }
    }

    Get get;
    std::unique_ptr<QNetworkAccessManager> manager;
    QTimer timer;
    QPointer<QNetworkReply> reply;
    QUrl url;
    QByteArray api_key;
    Validator validator;
    Observer observer;
    State state = State::Checking;
    QString last_error;
    int failures = 0;
    bool running = false;
};
