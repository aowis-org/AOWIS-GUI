#pragma once

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QElapsedTimer>
#include <cstdint>
#include <memory>

namespace aowis::network { class WireGuardClient; }

// Process-local WireGuard runtime diagnostics. No secrets or configuration
// text are retained.
// All calls must occur on the GUI thread. A service owns each tunnel; the
// settings page observes snapshots and requests peer diagnostics periodically.
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
        bool has_peer_stats = false;
        std::uint64_t received_bytes = 0;
        std::uint64_t transmitted_bytes = 0;
        std::int64_t last_handshake_unix_seconds = 0; // 0: no handshake yet
        double received_bytes_per_second = 0;
        double transmitted_bytes_per_second = 0;
        QStringList peer_endpoints;
        QString peer_stats_error;
    };

    static WireGuardRuntimeStatus &instance();

    void starting(const QString &profile_id, const QObject *owner);
    struct PeerSample
    {
        std::uint64_t received_bytes = 0;
        std::uint64_t transmitted_bytes = 0;
        std::int64_t last_handshake_unix_seconds = 0;
        QString endpoint;
    };

    void running(const QObject *owner,
                 std::weak_ptr<aowis::network::WireGuardClient> tunnel = {});
    // Reads currently running engines, if present; never initiates traffic.
    // Intended for a two-second timer while Status is visible.
    void pollTelemetry(const QString &profile_id);
    // Also usable by other transport types publishing compatible samples.
    void recordPeerSample(const QObject *owner, const PeerSample &sample);
    void httpReachable(const QObject *owner);
    void failed(const QObject *owner, const QString &error);
    void stopped(const QObject *owner);
    Snapshot snapshot(const QString &profile_id) const;

signals:
    void statusChanged(const QString &profile_id);

private:
    WireGuardRuntimeStatus() = default;
    struct Transport
    {
        QString profile_id;
        State state = State::Starting;
        bool http_reachable = false;
        std::weak_ptr<aowis::network::WireGuardClient> tunnel;
        bool has_stats = false;
        PeerSample stats;
        QElapsedTimer last_sample;
        double rx_per_second = 0;
        double tx_per_second = 0;
        QString stats_error;
    };
    QHash<const QObject *, Transport> transports;
    QSet<QString> attempted;
    QHash<QString, QString> errors;
};
