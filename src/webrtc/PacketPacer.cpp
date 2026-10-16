#include "webrtc/PacketPacer.h"

#include <algorithm>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <timeapi.h>
#include <immintrin.h>
#endif

namespace gateway {

PacingMediaHandler::PacingMediaHandler(PacketPacing pacing,
                                       int fps,
                                       int bitrateKbps,
                                       std::function<void(const std::string&)> logger,
                                       int maxBandwidthKbps)
    : pacing_(pacing)
    , fps_(fps > 0 ? fps : 60)
    , bitrateKbps_(bitrateKbps > 0 ? bitrateKbps : 12000)
    , logger_(std::move(logger))
{
    if (pacing_ != PacketPacing::Disabled) {
        // Multiplier:
        // Fast: 4.0x target bitrate (~4ms delivery window / 25% budget)
        // Balanced: 1.15x target bitrate (~14.5ms send time out of 16.6ms frame budget)
        // Aggressive: 1.05x target bitrate (~15.8ms send time out of 16.6ms frame budget, near-flat Wi-Fi rate)
        double multiplier = 1.15;
        if (pacing_ == PacketPacing::Fast) {
            multiplier = 4.0;
        } else if (pacing_ == PacketPacing::Aggressive) {
            multiplier = 1.05;
        }
        const double targetBps = static_cast<double>(bitrateKbps_) * 1000.0 * multiplier;
        bytesPerSec_ = static_cast<std::uint64_t>(targetBps / 8.0);
        if (bytesPerSec_ == 0) {
            bytesPerSec_ = 2'500'000; // 20 Mbps default fallback
        }

        // Catch-up pacing rate: accelerated (1.25x - 1.35x), but ALWAYS with pacing delay.
        // Never zero delay so network buffers don't get blown up.
        const double catchUpMultiplier = std::max(multiplier * 1.15, 1.25);
        catchUpBytesPerSec_ = static_cast<std::uint64_t>((static_cast<double>(bitrateKbps_) * 1000.0 * catchUpMultiplier) / 8.0);

        // Line-rate safety cap: If maxBandwidthKbps is explicitly provided (>0), cap rate to that.
        // No implicit cap is applied by default so Wi-Fi 5GHz and Gigabit links are unthrottled.
        const int effectiveCapKbps = (maxBandwidthKbps > 0) ? maxBandwidthKbps : 0;
        if (effectiveCapKbps > 0) {
            maxLineRateBytesPerSec_ = static_cast<std::uint64_t>((static_cast<double>(effectiveCapKbps) * 1000.0) / 8.0);
            if (bytesPerSec_ > maxLineRateBytesPerSec_) {
                bytesPerSec_ = maxLineRateBytesPerSec_;
            }
            if (catchUpBytesPerSec_ > maxLineRateBytesPerSec_) {
                catchUpBytesPerSec_ = maxLineRateBytesPerSec_;
            }
        }

        worker_ = std::thread([this] { pacingLoop(); });
    }
}

PacingMediaHandler::~PacingMediaHandler()
{
    stop();
}

void PacingMediaHandler::stop()
{
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    {
        const std::lock_guard lock(mutex_);
        if (sendCallback_) {
            while (!queue_.empty()) {
                auto pkt = std::move(queue_.front());
                queue_.pop_front();
                if (pkt) {
                    try {
                        sendCallback_(std::move(pkt));
                    } catch (...) {}
                }
            }
        } else {
            queue_.clear();
        }
    }
    cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}

void PacingMediaHandler::outgoing(rtc::message_vector& messages, const rtc::message_callback& send)
{
    if (pacing_ == PacketPacing::Disabled || messages.empty()) {
        return;
    }

    const auto count = messages.size();
    {
        const std::lock_guard statsLock(statsMutex_);
        stats_.totalBursts++;
        stats_.maxBurstPackets = std::max(stats_.maxBurstPackets, static_cast<std::uint64_t>(count));
    }

    // Fully asynchronous: enqueue all messages to the worker thread without blocking the calling thread.
    // The worker thread immediately starts draining the first batch (Head-of-Line),
    // ensuring sub-millisecond dispatch while isolating the Moonlight pipeline from network socket latency.
    {
        const std::lock_guard lock(mutex_);
        sendCallback_ = send;
        for (std::size_t i = 0; i < count; ++i) {
            if (messages[i]) {
                queue_.push_back(std::move(messages[i]));
            }
        }
    }
    cv_.notify_one();

    // Clear messages so Track::outgoing does not transmit them in a single unpaced burst
    messages.clear();
}

void PacingMediaHandler::incoming(rtc::message_vector& messages, const rtc::message_callback& send)
{
    rtc::MediaHandler::incoming(messages, send);
}

PacingStats PacingMediaHandler::stats() const
{
    const std::lock_guard lock(statsMutex_);
    return stats_;
}

void PacingMediaHandler::log(const std::string& message) const
{
    if (logger_) {
        logger_(message);
    }
}

void PacingMediaHandler::pacingLoop()
{
#if defined(_WIN32)
    timeBeginPeriod(1);
#endif

    std::unique_lock lock(mutex_);
    while (running_) {
        cv_.wait(lock, [this] { return !queue_.empty() || !running_; });
        if (!running_ && queue_.empty()) {
            break;
        }

        const auto queueDepth = queue_.size();
        {
            const std::lock_guard statsLock(statsMutex_);
            stats_.maxQueueDepth = std::max(stats_.maxQueueDepth, static_cast<std::uint64_t>(queueDepth));
        }

        // If queue backlog exceeds 1 frame worth of packets (~160 pkts),
        // enter catch-up mode: drain packets in larger batches with 0 delay
        // to prevent latency accumulation.
        constexpr std::size_t MaxBacklogPackets = 160;
        const bool catchUp = queueDepth > MaxBacklogPackets;
        if (catchUp) {
            const std::lock_guard statsLock(statsMutex_);
            stats_.catchUpEvents++;
        }

        const std::size_t batchSize = catchUp ? 16 : 8;
        std::vector<rtc::message_ptr> batch;
        batch.reserve(batchSize);
        std::size_t batchBytes = 0;

        while (!queue_.empty() && batch.size() < batchSize) {
            auto pkt = std::move(queue_.front());
            queue_.pop_front();
            if (pkt) {
                batchBytes += pkt->size();
                batch.push_back(std::move(pkt));
            }
        }

        auto send = sendCallback_;
        lock.unlock();

        for (auto& pkt : batch) {
            if (send) {
                try {
                    send(std::move(pkt));
                } catch (...) {}
            }
        }

        {
            const std::lock_guard statsLock(statsMutex_);
            stats_.totalPacketsPaced += batch.size();
        }

        // Pacing delay calculation:
        // In catch-up mode, use catchUpBytesPerSec_ (accelerated drain rate) instead of 0 delay.
        // This ensures packets are still paced smoothly and never overwhelm 100 Mbps Ethernet switches or TV buffers.
        const std::uint64_t effectiveBytesPerSec = catchUp ? catchUpBytesPerSec_ : bytesPerSec_;
        if (effectiveBytesPerSec > 0 && batchBytes > 0) {
            const auto delayUs = std::chrono::microseconds((batchBytes * 1'000'000ULL) / effectiveBytesPerSec);
            const auto targetTime = std::chrono::steady_clock::now() + delayUs;

            if (delayUs >= std::chrono::milliseconds(1)) {
                std::this_thread::sleep_for(delayUs - std::chrono::microseconds(300));
            }
            while (std::chrono::steady_clock::now() < targetTime && running_) {
#if defined(_WIN32)
                _mm_pause();
#else
                std::this_thread::yield();
#endif
            }
        }

        lock.lock();
    }

#if defined(_WIN32)
    timeEndPeriod(1);
#endif
}

} // namespace gateway
