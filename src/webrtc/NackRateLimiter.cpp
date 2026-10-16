#include "webrtc/NackRateLimiter.h"

namespace gateway {

NackRateLimitingHandler::NackRateLimitingHandler(std::size_t maxStorageSize,
                                                 int cooldownMs,
                                                 int maxBurstRetransmits)
    : responder_(std::make_shared<rtc::RtcpNackResponder>(maxStorageSize))
    , cooldownMs_(cooldownMs > 0 ? cooldownMs : 25)
    , maxBurstRetransmits_(maxBurstRetransmits > 0 ? maxBurstRetransmits : 40)
    , burstWindowStart_(std::chrono::steady_clock::now())
{
}

void NackRateLimitingHandler::outgoing(rtc::message_vector& messages,
                                       const rtc::message_callback& send)
{
    if (responder_) {
        responder_->outgoing(messages, send);
    }
    rtc::MediaHandler::outgoing(messages, send);
}

void NackRateLimitingHandler::incoming(rtc::message_vector& messages,
                                       const rtc::message_callback& send)
{
    if (!responder_) {
        rtc::MediaHandler::incoming(messages, send);
        return;
    }

    auto wrappedSend = [this, send](rtc::message_ptr pkt) {
        if (!pkt) {
            return;
        }

        // Parse RTP sequence number (Bytes 2-3 in standard RTP header)
        std::uint16_t seq = 0;
        if (pkt->size() >= 4) {
            const auto* data = reinterpret_cast<const std::uint8_t*>(pkt->data());
            seq = (static_cast<std::uint16_t>(data[2]) << 8) | data[3];
        }

        const auto now = std::chrono::steady_clock::now();

        {
            const std::lock_guard lock(mutex_);

            // 1. Deduplication: Suppress duplicate retransmissions within RTT cooldown window
            const auto it = lastRetransmitTime_.find(seq);
            if (it != lastRetransmitTime_.end()) {
                const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second).count();
                if (elapsedMs < cooldownMs_) {
                    const std::lock_guard statsLock(statsMutex_);
                    stats_.duplicateNacksSuppressed++;
                    return;
                }
            }

            // 2. Rate limiting: Capped burst window (100ms interval) to protect 100M Ethernet and switch buffers
            const auto windowElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - burstWindowStart_).count();
            if (windowElapsed >= 100) {
                burstWindowStart_ = now;
                burstCount_ = 0;
            }

            if (burstCount_ >= maxBurstRetransmits_) {
                const std::lock_guard statsLock(statsMutex_);
                stats_.rateLimitedNacks++;
                return;
            }

            burstCount_++;
            lastRetransmitTime_[seq] = now;

            // Prune deduplication cache if it exceeds 2048 entries
            if (lastRetransmitTime_.size() > 2048) {
                for (auto pruneIt = lastRetransmitTime_.begin(); pruneIt != lastRetransmitTime_.end();) {
                    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - pruneIt->second).count() > 500) {
                        pruneIt = lastRetransmitTime_.erase(pruneIt);
                    } else {
                        ++pruneIt;
                    }
                }
            }
        }

        {
            const std::lock_guard statsLock(statsMutex_);
            stats_.retransmittedPackets++;
        }

        if (send) {
            try {
                send(std::move(pkt));
            } catch (...) {}
        }
    };

    responder_->incoming(messages, wrappedSend);
    rtc::MediaHandler::incoming(messages, send);
}

NackLimiterStats NackRateLimitingHandler::stats() const
{
    const std::lock_guard lock(statsMutex_);
    return stats_;
}

} // namespace gateway
