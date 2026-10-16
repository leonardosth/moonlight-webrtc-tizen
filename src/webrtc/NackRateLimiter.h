#pragma once

#include <rtc/rtc.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace gateway {

struct NackLimiterStats {
    std::uint64_t retransmittedPackets = 0;
    std::uint64_t duplicateNacksSuppressed = 0;
    std::uint64_t rateLimitedNacks = 0;
};

class NackRateLimitingHandler : public rtc::MediaHandler {
public:
    explicit NackRateLimitingHandler(std::size_t maxStorageSize = 8192,
                                     int cooldownMs = 25,
                                     int maxBurstRetransmits = 40);
    ~NackRateLimitingHandler() override = default;

    void incoming(rtc::message_vector& messages, const rtc::message_callback& send) override;
    void outgoing(rtc::message_vector& messages, const rtc::message_callback& send) override;

    [[nodiscard]] NackLimiterStats stats() const;

private:
    std::shared_ptr<rtc::RtcpNackResponder> responder_;
    int cooldownMs_;
    int maxBurstRetransmits_;

    mutable std::mutex mutex_;
    std::unordered_map<std::uint16_t, std::chrono::steady_clock::time_point> lastRetransmitTime_;
    std::chrono::steady_clock::time_point burstWindowStart_{};
    int burstCount_ = 0;

    mutable std::mutex statsMutex_;
    NackLimiterStats stats_;
};

} // namespace gateway
