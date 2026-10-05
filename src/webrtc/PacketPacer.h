#pragma once

#include "session/StreamSettings.h"

#include <rtc/rtc.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace gateway {

struct PacingStats {
    std::uint64_t totalPacketsPaced = 0;
    std::uint64_t totalPacketsImmediate = 0;
    std::uint64_t totalBursts = 0;
    std::uint64_t maxBurstPackets = 0;
    std::uint64_t catchUpEvents = 0;
    std::uint64_t maxQueueDepth = 0;
};

class PacingMediaHandler : public rtc::MediaHandler {
public:
    explicit PacingMediaHandler(PacketPacing pacing,
                                int fps = 60,
                                int bitrateKbps = 12000,
                                std::function<void(const std::string&)> logger = nullptr);
    ~PacingMediaHandler() override;

    void outgoing(rtc::message_vector& messages, const rtc::message_callback& send) override;
    void incoming(rtc::message_vector& messages, const rtc::message_callback& send) override;

    void stop();

    [[nodiscard]] PacketPacing pacingMode() const noexcept { return pacing_; }
    [[nodiscard]] PacingStats stats() const;

private:
    void pacingLoop();
    void log(const std::string& message) const;

    PacketPacing pacing_;
    int fps_;
    int bitrateKbps_;
    std::function<void(const std::string&)> logger_;

    std::uint64_t bytesPerSec_ = 0;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<rtc::message_ptr> queue_;
    rtc::message_callback sendCallback_;
    std::atomic<bool> running_{true};
    std::thread worker_;

    mutable std::mutex statsMutex_;
    PacingStats stats_;
};

} // namespace gateway
