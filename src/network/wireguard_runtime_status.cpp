#include "network/wireguard_runtime_status.h"

#ifdef AOWIS_GUI_WIREGUARD_PROBE
#include <aowis/network/wireguard_client.h>
#endif

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

void WireGuardRuntimeStatus::running(const QObject *owner,
                                     std::weak_ptr<aowis::network::WireGuardClient> tunnel)
{
    QHash<const QObject *, Transport>::iterator it = this->transports.find(owner);
    if (it == this->transports.end()) return;
    it->state = State::Running;
    it->tunnel = std::move(tunnel);
    emit this->statusChanged(it->profile_id);
}

void WireGuardRuntimeStatus::recordPeerSample(const QObject *owner, const PeerSample &sample)
{
    QHash<const QObject *, Transport>::iterator it = this->transports.find(owner);
    if (it == this->transports.end() || it->state != State::Running) return;
    const bool had_stats = it->has_stats;
    const PeerSample old = it->stats;
    if (had_stats && it->last_sample.isValid())
    {
        const qint64 millis = it->last_sample.restart();
        // On counter reset/restart, do not report a negative or huge rate.
        if (millis > 0 && sample.received_bytes >= old.received_bytes
            && sample.transmitted_bytes >= old.transmitted_bytes)
        {
            it->rx_per_second = static_cast<double>(sample.received_bytes - old.received_bytes) * 1000.0 / millis;
            it->tx_per_second = static_cast<double>(sample.transmitted_bytes - old.transmitted_bytes) * 1000.0 / millis;
        }
        else
        {
            it->rx_per_second = 0;
            it->tx_per_second = 0;
        }
    }
    else
    {
        it->last_sample.start();
        it->rx_per_second = 0;
        it->tx_per_second = 0;
    }
    it->has_stats = true;
    it->stats = sample;
    it->stats_error.clear();
    emit this->statusChanged(it->profile_id);
}

void WireGuardRuntimeStatus::pollTelemetry(const QString &profile_id)
{
#ifdef AOWIS_GUI_WIREGUARD_PROBE
    // A short local wireguard-go IPC read; no packets or network probes.
    // Read only while the Status tab is visible, never from a render frame.
    for (QHash<const QObject *, Transport>::iterator it = this->transports.begin();
         it != this->transports.end(); ++it)
    {
        if (it->profile_id != profile_id || it->state != State::Running) continue;
        const std::shared_ptr<aowis::network::WireGuardClient> tunnel = it->tunnel.lock();
        if (!tunnel) continue;
        aowis::network::WireGuardPeerStats stats;
        std::string error;
        if (tunnel->peerStats(&stats, &error))
        {
            const QObject *owner = it.key();
            this->recordPeerSample(owner, {stats.receivedBytes, stats.transmittedBytes,
                                          stats.lastHandshakeUnixSeconds,
                                          QString::fromStdString(stats.endpoint)});
        }
        else
        {
            it->stats_error = QString::fromStdString(error);
            it->has_stats = false;
            it->last_sample.invalidate();
            it->rx_per_second = 0;
            it->tx_per_second = 0;
            emit this->statusChanged(profile_id);
        }
    }
#else
    Q_UNUSED(profile_id)
#endif
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
            if (it->has_stats)
            {
                result.has_peer_stats = true;
                result.received_bytes += it->stats.received_bytes;
                result.transmitted_bytes += it->stats.transmitted_bytes;
                result.received_bytes_per_second += it->rx_per_second;
                result.transmitted_bytes_per_second += it->tx_per_second;
                if (it->stats.last_handshake_unix_seconds > result.last_handshake_unix_seconds)
                    result.last_handshake_unix_seconds = it->stats.last_handshake_unix_seconds;
                if (!it->stats.endpoint.isEmpty() && !result.peer_endpoints.contains(it->stats.endpoint))
                    result.peer_endpoints.append(it->stats.endpoint);
            }
            if (!it->stats_error.isEmpty()) result.peer_stats_error = it->stats_error;
        }
        else if (it->state == State::Starting) ++result.starting_tunnels;
    }
    if (result.running_tunnels > 0) result.state = State::Running;
    else if (result.starting_tunnels > 0) result.state = State::Starting;
    else if (this->errors.contains(profile_id)) result.state = State::Error;
    result.last_error = this->errors.value(profile_id);
    return result;
}
