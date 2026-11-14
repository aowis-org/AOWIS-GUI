#pragma once

#include <cstdint>

// Whether the peer of a running embedded WireGuard tunnel stopped answering,
// judged from its telemetry. A started tunnel only means its local proxy
// listens; consumers such as MAP upstreams would otherwise send every request
// into a tunnel whose peer is gone and wait for each one to time out.
//
// Rule: the peer is unresponsive when there has been no handshake for
// WireGuard's REJECT_AFTER_TIME (180 s: no usable session exists), or none
// at all, while packets are being sent for UnansweredLimitMs. Without
// outgoing packets nothing is judged: an idle tunnel without keepalives has
// no fresh handshake either, and is not broken. A live peer with traffic
// completes a handshake within a round trip and renews it every 120 s, so it
// never meets the rule. A peer that dies during a fresh session is detected
// once that session is 180 s old; the consumers' own timeouts cover the gap.
// Pure and Qt-free so it can be tested on its own.
struct WireGuardPeerLiveness
{
    static constexpr std::int64_t SessionLifetimeMs = 180000;
    static constexpr std::int64_t UnansweredLimitMs = 20000;

    // Feed one telemetry sample; returns true when the peer is unresponsive.
    bool update(std::int64_t now_ms, std::int64_t last_handshake_ms, std::uint64_t sent_bytes)
    {
        const bool session = last_handshake_ms > 0 && now_ms - last_handshake_ms < SessionLifetimeMs;
        const bool sending = this->sampled && sent_bytes != this->last_sent_bytes;
        this->sampled = true;
        this->last_sent_bytes = sent_bytes;
        if (session)
        {
            this->unanswered_since_ms = 0;
            return false;
        }
        if (sending && this->unanswered_since_ms == 0)
            this->unanswered_since_ms = now_ms;
        return this->unanswered_since_ms != 0 &&
               now_ms - this->unanswered_since_ms >= UnansweredLimitMs;
    }

    void reset()
    {
        this->sampled = false;
        this->last_sent_bytes = 0;
        this->unanswered_since_ms = 0;
    }

private:
    bool sampled = false;
    std::uint64_t last_sent_bytes = 0;
    std::int64_t unanswered_since_ms = 0;
};
