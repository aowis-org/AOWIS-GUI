#pragma once

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>

// Process-local diagnostics for embedded WireGuard transports. This is NOT a
// WireGuard handshake monitor: the current AOWIS-NETWORK client exposes only
// local engine start/stop state. No secrets or configuration text are retained.
// All calls must occur on the GUI thread. DvmdbConnection reports its own
// lifecycle; the settings page only observes snapshots.
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

    static WireGuardRuntimeStatus &instance();

    void starting(const QString &profile_id, const QObject *owner);
    void running(const QObject *owner);
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
    };
    QHash<const QObject *, Transport> transports;
    QSet<QString> attempted;
    QHash<QString, QString> errors;
};
