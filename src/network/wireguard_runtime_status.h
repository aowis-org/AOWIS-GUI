#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>

#include <functional>

// Process-local diagnostics for embedded WireGuard transports. Lifecycle is
// reported per connection by DvmdbConnection; all connections of a profile
// share one tunnel, whose health (handshake, traffic, relays) is sampled on
// demand through the sampler each connection registers when it runs. This class has no AOWIS-NETWORK dependency,
// so it builds identically without the embedded engine. No secrets or
// configuration text are retained. All calls must occur on the GUI thread.
class WireGuardRuntimeStatus final : public QObject
{
    Q_OBJECT

public:
    enum class State { NotStarted, Stopped, Starting, Running, Error };
    struct Snapshot
    {
        State state = State::NotStarted;
        int active_connections = 0;   // Connections currently using the profile's tunnel
        int starting_connections = 0;
        int http_reachable_connections = 0;
        QString last_error;
    };

    // Redacted sample of the profile's tunnel. Timestamps: Unix epoch ms.
    struct TunnelTelemetry
    {
        qint64 started_unix_ms = 0;        // Also identifies the tunnel instance
        qint64 last_handshake_unix_ms = 0; // 0: no handshake completed yet
        quint64 received_bytes = 0;
        quint64 sent_bytes = 0;
        quint64 persistent_keepalive_seconds = 0;
        quint64 open_relays = 0;           // SOCKS5 relays currently open
        quint64 accepted_relays = 0;       // Authenticated CONNECT requests
        quint64 failed_dials = 0;
        QString peer_endpoint;
    };
    // Fills the sample and returns true, or false when it cannot be sampled.
    using TelemetrySampler = std::function<bool(TunnelTelemetry &)>;

    static WireGuardRuntimeStatus &instance();

    void starting(const QString &profile_id, const QObject *owner, const QString &service = QString());
    void running(const QObject *owner, TelemetrySampler sampler = TelemetrySampler());
    void httpReachable(const QObject *owner);
    void failed(const QObject *owner, const QString &error);
    void stopped(const QObject *owner);
    // Cheap: lifecycle state only, never touches the engine.
    Snapshot snapshot(const QString &profile_id) const;
    // Samples the profile's tunnel (one engine call). Connections of one
    // profile share a single tunnel, so any running connection's sampler
    // describes it. Returns false when none is running or sampling failed.
    bool telemetry(const QString &profile_id, TunnelTelemetry &out) const;
    // Service labels of the running connections, in the order they started.
    QStringList connections(const QString &profile_id) const;

signals:
    void statusChanged(const QString &profile_id);

private:
    WireGuardRuntimeStatus() = default;
    struct Transport
    {
        QString profile_id;
        State state = State::Starting;
        bool http_reachable = false;
        quint64 sequence = 0; // Start order, for stable presentation
        QString service;
        TelemetrySampler sampler;
    };
    QHash<const QObject *, Transport> transports;
    QSet<QString> attempted;
    QHash<QString, QString> errors;
    quint64 next_sequence = 0;
};
