#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>

#include <functional>

// Process-local diagnostics for embedded WireGuard transports. Lifecycle is
// reported by DvmdbConnection; peer health (handshake, traffic, relays) is
// sampled on demand through a per-transport sampler that DvmdbConnection
// registers when its tunnel runs. This class has no AOWIS-NETWORK dependency,
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
        int running_tunnels = 0;
        int starting_tunnels = 0;
        int http_reachable_tunnels = 0;
        QString last_error;
    };

    // Redacted per-tunnel sample. Timestamps are Unix epoch milliseconds.
    struct TunnelTelemetry
    {
        quint64 tunnel_id = 0;          // Process-unique, stable for one tunnel lifetime
        QString service;                // Presentation label of the owning connection
        bool http_reachable = false;
        bool available = false;         // false: the engine could not be sampled
        qint64 started_unix_ms = 0;
        qint64 last_handshake_unix_ms = 0; // 0: no handshake completed yet
        quint64 received_bytes = 0;
        quint64 sent_bytes = 0;
        quint64 persistent_keepalive_seconds = 0;
        quint64 active_connections = 0;
        quint64 accepted_connections = 0;
        quint64 failed_dials = 0;
        QString peer_endpoint;
    };
    // Fills the engine fields (started_unix_ms .. peer_endpoint) and returns
    // true, or returns false when the tunnel cannot be sampled.
    using TelemetrySampler = std::function<bool(TunnelTelemetry &)>;

    static WireGuardRuntimeStatus &instance();

    void starting(const QString &profile_id, const QObject *owner, const QString &service = QString());
    void running(const QObject *owner, TelemetrySampler sampler = TelemetrySampler());
    void httpReachable(const QObject *owner);
    void failed(const QObject *owner, const QString &error);
    void stopped(const QObject *owner);
    // Cheap: lifecycle state only, never touches the engine.
    Snapshot snapshot(const QString &profile_id) const;
    // Samples every running tunnel of the profile now (one engine call each).
    QList<TunnelTelemetry> telemetry(const QString &profile_id) const;

signals:
    void statusChanged(const QString &profile_id);

private:
    WireGuardRuntimeStatus() = default;
    struct Transport
    {
        QString profile_id;
        State state = State::Starting;
        bool http_reachable = false;
        quint64 tunnel_id = 0;
        QString service;
        TelemetrySampler sampler;
    };
    QHash<const QObject *, Transport> transports;
    QSet<QString> attempted;
    QHash<QString, QString> errors;
    quint64 next_tunnel_id = 0;
};
