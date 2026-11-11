#include "network/wireguard_runtime_status.h"

WireGuardRuntimeStatus &WireGuardRuntimeStatus::instance()
{
    // No persistent storage, no network access, and no implicit tunnel start.
    static WireGuardRuntimeStatus monitor;
    return monitor;
}

void WireGuardRuntimeStatus::starting(const QString &profile_id, const QObject *owner)
{
    if (profile_id.isEmpty() || owner == nullptr) return;
    if (this->transports.contains(owner)) this->stopped(owner);
    this->transports.insert(owner, {profile_id, State::Starting, false});
    this->attempted.insert(profile_id);
    this->errors.remove(profile_id); // A new attempt supersedes an old error.
    emit this->statusChanged(profile_id);
}

void WireGuardRuntimeStatus::running(const QObject *owner)
{
    QHash<const QObject *, Transport>::iterator it = this->transports.find(owner);
    if (it == this->transports.end()) return;
    it->state = State::Running;
    emit this->statusChanged(it->profile_id);
}

void WireGuardRuntimeStatus::httpReachable(const QObject *owner)
{
    QHash<const QObject *, Transport>::iterator it = this->transports.find(owner);
    if (it == this->transports.end() || it->state != State::Running || it->http_reachable) return;
    it->http_reachable = true;
    emit this->statusChanged(it->profile_id);
}

void WireGuardRuntimeStatus::failed(const QObject *owner, const QString &error)
{
    QHash<const QObject *, Transport>::iterator it = this->transports.find(owner);
    if (it == this->transports.end()) return;
    const QString profile_id = it->profile_id;
    this->transports.erase(it);
    this->errors.insert(profile_id, error);
    emit this->statusChanged(profile_id);
}

void WireGuardRuntimeStatus::stopped(const QObject *owner)
{
    QHash<const QObject *, Transport>::iterator it = this->transports.find(owner);
    if (it == this->transports.end()) return;
    const QString profile_id = it->profile_id;
    this->transports.erase(it);
    emit this->statusChanged(profile_id);
}

WireGuardRuntimeStatus::Snapshot WireGuardRuntimeStatus::snapshot(const QString &profile_id) const
{
    Snapshot result;
    if (!this->attempted.contains(profile_id)) return result;
    result.state = State::Stopped;
    for (QHash<const QObject *, Transport>::const_iterator it = this->transports.cbegin();
         it != this->transports.cend(); ++it)
    {
        if (it->profile_id != profile_id) continue;
        if (it->state == State::Running)
        {
            ++result.running_tunnels;
            if (it->http_reachable) ++result.http_reachable_tunnels;
        }
        else if (it->state == State::Starting) ++result.starting_tunnels;
    }
    if (result.running_tunnels > 0) result.state = State::Running;
    else if (result.starting_tunnels > 0) result.state = State::Starting;
    else if (this->errors.contains(profile_id)) result.state = State::Error;
    result.last_error = this->errors.value(profile_id);
    return result;
}
