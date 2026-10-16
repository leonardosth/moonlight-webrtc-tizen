#include "media/HevcSpsParser.h"
#include "moonlight/MoonlightMediaBridge.h"

extern "C" std::uint64_t PltGetMicroseconds(void);

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

struct SentFrame {
    std::vector<std::uint8_t> data;
    std::uint32_t rtpTimestamp;
    gateway::VideoCodec codec;
};

class RecordingMediaSender final : public gateway::MediaSender {
public:
    void sendVideoAccessUnit(gateway::VideoCodec codec,
                             std::span<const std::uint8_t> accessUnit,
                             std::uint32_t rtpTimestamp) override
    {
        video.push_back({{accessUnit.begin(), accessUnit.end()}, rtpTimestamp, codec});
    }

    void sendOpusPacket(std::span<const std::uint8_t> packet,
                        std::uint32_t rtpTimestamp) override
    {
        audio.push_back(
            {{packet.begin(), packet.end()}, rtpTimestamp, gateway::VideoCodec::H264});
    }

    std::vector<SentFrame> video;
    std::vector<SentFrame> audio;
};

void require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class BitWriter {
public:
    void writeBits(std::uint64_t value, int count)
    {
        for (int bit = count - 1; bit >= 0; --bit) {
            if (bitOffset_ == 0) {
                bytes_.push_back(0);
            }
            bytes_.back() |= static_cast<std::uint8_t>(
                ((value >> bit) & 1U) << (7 - bitOffset_));
            bitOffset_ = (bitOffset_ + 1) % 8;
        }
    }

    void writeUnsignedExpGolomb(std::uint32_t value)
    {
        const std::uint32_t code = value + 1;
        int bitCount = 0;
        for (auto current = code; current != 0; current >>= 1) {
            ++bitCount;
        }
        writeBits(0, bitCount - 1);
        writeBits(code, bitCount);
    }

    std::vector<std::uint8_t> take()
    {
        return std::move(bytes_);
    }

private:
    std::vector<std::uint8_t> bytes_;
    int bitOffset_ = 0;
};

std::vector<std::uint8_t> syntheticMain10Sps()
{
    BitWriter writer;
    writer.writeBits(0, 4);
    writer.writeBits(0, 3);
    writer.writeBits(1, 1);
    writer.writeBits(0, 2);
    writer.writeBits(0, 1);
    writer.writeBits(2, 5);
    writer.writeBits(0, 32);
    writer.writeBits(0, 48);
    writer.writeBits(120, 8);
    writer.writeUnsignedExpGolomb(0);
    writer.writeUnsignedExpGolomb(1);
    writer.writeUnsignedExpGolomb(1920);
    writer.writeUnsignedExpGolomb(1080);
    writer.writeBits(0, 1);
    writer.writeUnsignedExpGolomb(2);
    writer.writeUnsignedExpGolomb(2);

    const auto rbsp = writer.take();
    std::vector<std::uint8_t> nal{0, 0, 0, 1, 0x42, 0x01};
    int zeroCount = 0;
    for (const auto byte : rbsp) {
        if (zeroCount >= 2 && byte <= 3) {
            nal.push_back(3);
            zeroCount = 0;
        }
        nal.push_back(byte);
        zeroCount = byte == 0 ? zeroCount + 1 : 0;
    }
    return nal;
}

} // namespace

int main()
{
    try {
        RecordingMediaSender sender;
        std::vector<std::string> logs;
        gateway::MoonlightMediaBridge bridge(
            sender,
            gateway::defaultStreamSettings(),
            [&logs](const std::string& message) { logs.push_back(message); });

        auto* videoCallbacks = bridge.videoCallbacks();
        require((videoCallbacks->capabilities & CAPABILITY_REFERENCE_FRAME_INVALIDATION_AVC) != 0,
                "AVC Reference Frame Invalidation capability must be advertised");
        require((videoCallbacks->capabilities & CAPABILITY_REFERENCE_FRAME_INVALIDATION_HEVC) != 0,
                "HEVC Reference Frame Invalidation capability must be advertised");
        require((videoCallbacks->capabilities & CAPABILITY_REFERENCE_FRAME_INVALIDATION_AV1) != 0,
                "AV1 Reference Frame Invalidation capability must be advertised");
        require(videoCallbacks->setup(VIDEO_FORMAT_H265, 1280, 720, 60, &bridge, 0) != 0,
                "HEVC must be rejected");
        require(videoCallbacks->setup(VIDEO_FORMAT_H264, 1280, 720, 60, &bridge, 0) == 0,
                "H.264 setup failed");
        videoCallbacks->start();

        std::array<char, 6> sps = {0, 0, 0, 1, 0x67, 0x01};
        std::array<char, 6> pps = {0, 0, 0, 1, 0x68, 0x02};
        std::array<char, 7> idr = {0, 0, 0, 1, 0x65, 0x03, 0x04};
        LENTRY idrEntry{nullptr, idr.data(), static_cast<int>(idr.size()), BUFFER_TYPE_PICDATA};
        LENTRY ppsEntry{&idrEntry, pps.data(), static_cast<int>(pps.size()), BUFFER_TYPE_PPS};
        LENTRY spsEntry{&ppsEntry, sps.data(), static_cast<int>(sps.size()), BUFFER_TYPE_SPS};

        DECODE_UNIT idrUnit{};
        idrUnit.frameNumber = 42;
        idrUnit.frameType = FRAME_TYPE_IDR;
        idrUnit.receiveTimeUs = 1000;
        idrUnit.enqueueTimeUs = 2000;
        idrUnit.presentationTimeUs = 3000;
        idrUnit.rtpTimestamp = 123456;
        idrUnit.fullLength = static_cast<int>(sps.size() + pps.size() + idr.size());
        idrUnit.bufferList = &spsEntry;

        require(videoCallbacks->submitDecodeUnit(&idrUnit) == DR_OK,
                "IDR decode unit did not return DR_OK");
        require(sender.video.size() == 1, "IDR decode unit was not forwarded");
        require(sender.video[0].rtpTimestamp == idrUnit.rtpTimestamp,
                "Moonlight video RTP timestamp changed");
        require(sender.video[0].codec == gateway::VideoCodec::H264,
                "H.264 decode unit used the wrong sender codec");

        std::vector<std::uint8_t> expectedIdr;
        expectedIdr.insert(expectedIdr.end(), sps.begin(), sps.end());
        expectedIdr.insert(expectedIdr.end(), pps.begin(), pps.end());
        expectedIdr.insert(expectedIdr.end(), idr.begin(), idr.end());
        require(sender.video[0].data == expectedIdr,
                "DECODE_UNIT buffer chain was not flattened in order");

        const auto idrDiagnostics = bridge.lastVideoFrame();
        require(idrDiagnostics && idrDiagnostics->frameNumber == 42
                    && idrDiagnostics->frameType == FRAME_TYPE_IDR
                    && idrDiagnostics->presentationTimeUs == 3000
                    && idrDiagnostics->receiveTimeUs == 1000
                    && idrDiagnostics->enqueueTimeUs == 2000,
                "IDR diagnostics were not preserved");

        std::array<char, 7> pframe = {0, 0, 0, 1, 0x41, 0x05, 0x06};
        LENTRY pframeEntry{
            nullptr, pframe.data(), static_cast<int>(pframe.size()), BUFFER_TYPE_PICDATA};
        DECODE_UNIT pframeUnit{};
        pframeUnit.frameNumber = 43;
        pframeUnit.frameType = FRAME_TYPE_PFRAME;
        pframeUnit.rtpTimestamp = 124956;
        pframeUnit.fullLength = static_cast<int>(pframe.size());
        pframeUnit.bufferList = &pframeEntry;

        bridge.onWebRtcKeyframeRequest();
        require(bridge.hasPendingIdrRequest(), "IDR request flag was not set");
        require(videoCallbacks->submitDecodeUnit(&pframeUnit) == DR_NEED_IDR,
                "WebRTC keyframe request did not produce DR_NEED_IDR");
        require(!bridge.hasPendingIdrRequest(), "IDR request flag was not consumed");
        require(videoCallbacks->submitDecodeUnit(&pframeUnit) == DR_OK,
                "IDR request flag was consumed more than once");

        const auto pframeDiagnostics = bridge.lastVideoFrame();
        require(pframeDiagnostics && pframeDiagnostics->frameType == FRAME_TYPE_PFRAME,
                "P-frame diagnostics were not preserved");
        require(videoCallbacks->submitDecodeUnit(&idrUnit) == DR_OK,
                "Moonlight IDR after keyframe request was not accepted");

        // Immediate second keyframe request within cooldown must be throttled
        require(bridge.idrCooldownMs() == 500, "Default Balanced mode IDR cooldown should be 500ms");
        bridge.onWebRtcKeyframeRequest();
        require(bridge.hasPendingIdrRequest(), "Second IDR request flag was not set");
        require(videoCallbacks->submitDecodeUnit(&pframeUnit) == DR_OK,
                "Rapid repeated keyframe request was not throttled by rate limiter");
        require(bridge.keyframeRequestsThrottled() == 1,
                "Keyframe throttled counter was not incremented");
        require(bridge.hasPendingIdrRequest(),
                "Deferred IDR flag must stay pending while in cooldown");

        // Wait for cooldown to expire (500ms default in Balanced mode)
        std::this_thread::sleep_for(std::chrono::milliseconds(550));
        require(videoCallbacks->submitDecodeUnit(&pframeUnit) == DR_NEED_IDR,
                "Deferred keyframe request was not forwarded after cooldown expired");
        require(bridge.keyframeRequestsForwarded() == 2,
                "Keyframe forwarded counter was not incremented after cooldown");
        require(!bridge.hasPendingIdrRequest(),
                "Deferred IDR flag was not consumed after forwarding");
        require(videoCallbacks->submitDecodeUnit(&idrUnit) == DR_OK,
                "Moonlight IDR after deferred request was not accepted");

        // Test that arriving IDR clears deferred keyframe request and suppresses redundant IDR
        bridge.onWebRtcKeyframeRequest();
        require(bridge.hasPendingIdrRequest(), "Third IDR request flag was not set");
        require(videoCallbacks->submitDecodeUnit(&pframeUnit) == DR_OK,
                "Request within cooldown must be throttled");
        require(bridge.hasPendingIdrRequest(), "Deferred flag must be set during cooldown");
        require(videoCallbacks->submitDecodeUnit(&idrUnit) == DR_OK,
                "Arriving IDR must be accepted");
        require(!bridge.hasPendingIdrRequest(),
                "Arriving IDR must clear deferred IDR flag");
        std::this_thread::sleep_for(std::chrono::milliseconds(550));
        require(videoCallbacks->submitDecodeUnit(&pframeUnit) == DR_OK,
                "Redundant keyframe request must NOT be forwarded if IDR arrived during cooldown");

        // Test Reference Frame Invalidation (RFI) recovery
        {
            std::uint32_t lossStart = 0;
            std::uint32_t lossEnd = 0;
            int rfiCallCount = 0;
            bridge.setFrameLossReporter([&](std::uint32_t start, std::uint32_t end) {
                lossStart = start;
                lossEnd = end;
                rfiCallCount++;
            });

            // When RFI predicate is enabled:
            bridge.setRfiEnabledPredicate([] { return true; });

            // Ensure lastFrameNumber is known by submitting a P-frame
            pframeUnit.frameNumber = 105;
            require(videoCallbacks->submitDecodeUnit(&pframeUnit) == DR_OK,
                    "P-frame 105 submission failed");

            // WebRTC requests recovery (forceIdr = false): must trigger RFI, NOT full IDR
            bridge.onWebRtcKeyframeRequest(false);
            require(!bridge.hasPendingIdrRequest(),
                    "RFI should NOT set pending IDR flag");
            require(bridge.rfiRequestsSent() == 1,
                    "RFI request counter must be incremented");
            require(rfiCallCount == 1,
                    "Frame loss reporter must be called once");
            require(lossStart == 105 && lossEnd == 106,
                    "Frame loss reporter must receive tuple [105, 106]");

            // Subsequent P-frame submission should NOT return DR_NEED_IDR because RFI handled recovery
            pframeUnit.frameNumber = 106;
            require(videoCallbacks->submitDecodeUnit(&pframeUnit) == DR_OK,
                    "Subsequent frame after RFI should return DR_OK, not DR_NEED_IDR");

            // If WebRTC requests forced full IDR (forceIdr = true): must trigger full IDR even if RFI enabled
            std::this_thread::sleep_for(std::chrono::milliseconds(550)); // clear cooldown
            bridge.onWebRtcKeyframeRequest(true);
            require(bridge.hasPendingIdrRequest(),
                    "Forced IDR must set pending IDR flag even when RFI is enabled");
            require(videoCallbacks->submitDecodeUnit(&pframeUnit) == DR_NEED_IDR,
                    "Forced IDR must return DR_NEED_IDR");

            // If RFI is disabled by host (rfiEnabled == false): falls back to full IDR
            bridge.setRfiEnabledPredicate([] { return false; });
            require(videoCallbacks->submitDecodeUnit(&idrUnit) == DR_OK,
                    "Accept IDR to clear state");
            std::this_thread::sleep_for(std::chrono::milliseconds(550)); // clear cooldown
            bridge.onWebRtcKeyframeRequest(false);
            require(bridge.hasPendingIdrRequest(),
                    "Disabled RFI must fall back to IDR request");
            require(videoCallbacks->submitDecodeUnit(&pframeUnit) == DR_NEED_IDR,
                    "Disabled RFI must return DR_NEED_IDR on submit");

            // Reset bridge RFI handlers back to default
            bridge.setFrameLossReporter(nullptr);
            bridge.setRfiEnabledPredicate(nullptr);
            require(videoCallbacks->submitDecodeUnit(&idrUnit) == DR_OK,
                    "Accept IDR to clear state");
        }

        // Test frame deadline drop policy in LowestLatency mode
        {
            RecordingMediaSender dropSender;
            auto dropSettings = gateway::defaultStreamSettings();
            dropSettings.latencyMode = gateway::LatencyMode::LowestLatency;
            dropSettings.fps = 60;
            gateway::MoonlightMediaBridge dropBridge(
                dropSender,
                dropSettings,
                [](const std::string&) {});
            auto* dropCallbacks = dropBridge.videoCallbacks();
            require(dropCallbacks->setup(VIDEO_FORMAT_H264, 1280, 720, 60, &dropBridge, 0) == 0,
                    "H.264 setup on dropBridge failed");
            dropCallbacks->start();
            require(dropBridge.frameDeadlineUs() > 0,
                    "frameDeadlineUs must be configured in LowestLatency mode");

            // IDR must not be dropped even if late
            DECODE_UNIT lateIdr = idrUnit;
            lateIdr.receiveTimeUs = 1;
            require(dropCallbacks->submitDecodeUnit(&lateIdr) == DR_OK,
                    "IDR frame must never be dropped by deadline");
            require(dropBridge.lateFrameDrops() == 0, "IDR frame must not increment lateFrameDrops");

            // Fresh P-frame (receiveTimeUs == now): must be accepted
            const auto nowUs = PltGetMicroseconds();
            DECODE_UNIT freshPframe = pframeUnit;
            freshPframe.receiveTimeUs = nowUs;
            require(dropCallbacks->submitDecodeUnit(&freshPframe) == DR_OK,
                    "Fresh P-frame within deadline must return DR_OK");
            require(dropBridge.lateFrameDrops() == 0,
                    "Fresh P-frame must not increment lateFrameDrops");

            // Late P-frame (age > deadline: e.g. age = 30ms > 25ms): must drop and return DR_NEED_IDR
            std::this_thread::sleep_for(std::chrono::milliseconds(35));
            const auto currentPlt = PltGetMicroseconds();
            DECODE_UNIT latePframe = pframeUnit;
            latePframe.receiveTimeUs = currentPlt > 30000 ? (currentPlt - 30000) : 1;
            require(dropCallbacks->submitDecodeUnit(&latePframe) == DR_NEED_IDR,
                    "Late P-frame exceeding deadline must return DR_NEED_IDR");
            require(dropBridge.lateFrameDrops() == 1,
                    "Late frame drop must increment lateFrameDrops counter");

            // Subsequent late P-frame while waiting for IDR: returns DR_OK
            require(dropCallbacks->submitDecodeUnit(&latePframe) == DR_OK,
                    "Subsequent late P-frame while waiting for IDR must return DR_OK");
            require(dropBridge.lateFrameDrops() == 2,
                    "Second late frame drop must increment lateFrameDrops to 2");

            // Stream recovers upon fresh IDR
            lateIdr.receiveTimeUs = PltGetMicroseconds();
            require(dropCallbacks->submitDecodeUnit(&lateIdr) == DR_OK,
                    "IDR frame must be accepted and reset waitingForMoonlightIdr");
        }

        // Test Smoothest mode: frameDeadlineUs == 0 (no drops)
        {
            RecordingMediaSender smoothSender;
            auto smoothSettings = gateway::defaultStreamSettings();
            smoothSettings.latencyMode = gateway::LatencyMode::Smoothest;
            gateway::MoonlightMediaBridge smoothBridge(
                smoothSender,
                smoothSettings,
                [](const std::string&) {});
            auto* smoothCallbacks = smoothBridge.videoCallbacks();
            require(smoothCallbacks->setup(VIDEO_FORMAT_H264, 1280, 720, 60, &smoothBridge, 0) == 0,
                    "H.264 setup on smoothBridge failed");
            smoothCallbacks->start();
            require(smoothBridge.frameDeadlineUs() == 0,
                    "frameDeadlineUs must be 0 in Smoothest mode");

            DECODE_UNIT latePframe = pframeUnit;
            latePframe.receiveTimeUs = 1;
            require(smoothCallbacks->submitDecodeUnit(&latePframe) == DR_OK,
                    "Smoothest mode must never drop late frames");
            require(smoothBridge.lateFrameDrops() == 0,
                    "Smoothest mode must have 0 late frame drops");
        }

        // Test Balanced mode: relaxed deadline (~250ms), frames with 60ms jitter pass through without drops
        {
            RecordingMediaSender balancedSender;
            auto balancedSettings = gateway::defaultStreamSettings();
            balancedSettings.latencyMode = gateway::LatencyMode::Balanced;
            balancedSettings.fps = 60;
            gateway::MoonlightMediaBridge balancedBridge(
                balancedSender,
                balancedSettings,
                [](const std::string&) {});
            auto* balancedCallbacks = balancedBridge.videoCallbacks();
            require(balancedCallbacks->setup(VIDEO_FORMAT_H264, 1280, 720, 60, &balancedBridge, 0) == 0,
                    "H.264 setup on balancedBridge failed");
            balancedCallbacks->start();
            require(balancedBridge.frameDeadlineUs() >= 200000,
                    "frameDeadlineUs must be relaxed to ~250ms in Balanced mode");

            const auto currentPlt = PltGetMicroseconds();
            DECODE_UNIT jitteredPframe = pframeUnit;
            jitteredPframe.receiveTimeUs = currentPlt > 60000 ? (currentPlt - 60000) : 1;
            require(balancedCallbacks->submitDecodeUnit(&jitteredPframe) == DR_OK,
                    "Jittered P-frame within relaxed Balanced deadline must return DR_OK");
            require(balancedBridge.lateFrameDrops() == 0,
                    "Jittered P-frame in Balanced mode must not increment lateFrameDrops");
        }

        // Edge Case: Null and malformed decode units must be rejected safely without crashing
        {
            RecordingMediaSender malformedSender;
            gateway::MoonlightMediaBridge malformedBridge(
                malformedSender,
                gateway::defaultStreamSettings(),
                [](const std::string&) {});
            auto* malformedCallbacks = malformedBridge.videoCallbacks();
            require(malformedCallbacks->setup(VIDEO_FORMAT_H264, 1280, 720, 60, &malformedBridge, 0) == 0,
                    "H.264 setup on malformedBridge failed");
            malformedCallbacks->start();

            require(malformedCallbacks->submitDecodeUnit(nullptr) == DR_NEED_IDR,
                    "Null decode unit must safely return DR_NEED_IDR");

            DECODE_UNIT nullBufferUnit = pframeUnit;
            nullBufferUnit.bufferList = nullptr;
            require(malformedCallbacks->submitDecodeUnit(&nullBufferUnit) == DR_NEED_IDR,
                    "Decode unit with null bufferList must return DR_NEED_IDR");

            LENTRY emptyDesc{ nullptr, nullptr, 0, BUFFER_TYPE_PICDATA };
            DECODE_UNIT emptyBufferUnit = pframeUnit;
            emptyBufferUnit.bufferList = &emptyDesc;
            require(malformedCallbacks->submitDecodeUnit(&emptyBufferUnit) == DR_NEED_IDR,
                    "Decode unit with 0-length bufferList must return DR_NEED_IDR");
        }

        // Edge Case: FrameDropPolicy::Disabled under LowestLatency must NEVER drop late frames
        {
            RecordingMediaSender disabledDropSender;
            auto disabledSettings = gateway::defaultStreamSettings();
            disabledSettings.latencyMode = gateway::LatencyMode::LowestLatency;
            disabledSettings.frameDropPolicy = gateway::FrameDropPolicy::Disabled;
            disabledSettings.fps = 60;
            gateway::MoonlightMediaBridge disabledDropBridge(
                disabledDropSender,
                disabledSettings,
                [](const std::string&) {});
            auto* disabledCallbacks = disabledDropBridge.videoCallbacks();
            require(disabledCallbacks->setup(VIDEO_FORMAT_H264, 1280, 720, 60, &disabledDropBridge, 0) == 0,
                    "H.264 setup on disabledDropBridge failed");
            disabledCallbacks->start();
            require(disabledDropBridge.frameDeadlineUs() == 0,
                    "frameDeadlineUs must be 0 when frameDropPolicy is Disabled, even in LowestLatency");

            DECODE_UNIT ancientPframe = pframeUnit;
            ancientPframe.receiveTimeUs = 1; // ancient timestamp
            require(disabledCallbacks->submitDecodeUnit(&ancientPframe) == DR_OK,
                    "FrameDropPolicy::Disabled must never drop late frames");
            require(disabledDropBridge.lateFrameDrops() == 0,
                    "Disabled frameDropPolicy must maintain 0 lateFrameDrops");
        }

        // Edge Case: FrameDropPolicy::Strict under 60fps enforces strict 1.5x frame interval (25ms)
        {
            RecordingMediaSender strictSender;
            auto strictSettings = gateway::defaultStreamSettings();
            strictSettings.latencyMode = gateway::LatencyMode::Smoothest; // Smoothest default is 0
            strictSettings.frameDropPolicy = gateway::FrameDropPolicy::Strict; // Strict overrides it
            strictSettings.fps = 60;
            gateway::MoonlightMediaBridge strictBridge(
                strictSender,
                strictSettings,
                [](const std::string&) {});
            auto* strictCallbacks = strictBridge.videoCallbacks();
            require(strictCallbacks->setup(VIDEO_FORMAT_H264, 1280, 720, 60, &strictBridge, 0) == 0,
                    "H.264 setup on strictBridge failed");
            strictCallbacks->start();
            require(strictBridge.frameDeadlineUs() == 25000,
                    "Strict frame drop policy must enforce exactly 1.5x frame interval (25ms @ 60fps)");

            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            const auto currentPlt = PltGetMicroseconds();
            DECODE_UNIT latePframe = pframeUnit;
            latePframe.receiveTimeUs = currentPlt > 30000 ? (currentPlt - 30000) : 1;
            require(strictCallbacks->submitDecodeUnit(&latePframe) == DR_NEED_IDR,
                    "Strict policy must drop frame with age 30ms > 25ms deadline and request IDR");
            require(strictBridge.lateFrameDrops() == 1,
                    "Strict late frame drop must increment lateFrameDrops counter");
        }

        // Edge Case: Burst of rapid WebRTC keyframe requests during cooldown
        {
            RecordingMediaSender burstSender;
            gateway::MoonlightMediaBridge burstBridge(
                burstSender,
                gateway::defaultStreamSettings(),
                [](const std::string&) {});
            auto* burstCallbacks = burstBridge.videoCallbacks();
            require(burstCallbacks->setup(VIDEO_FORMAT_H264, 1280, 720, 60, &burstBridge, 0) == 0,
                    "H.264 setup on burstBridge failed");
            burstCallbacks->start();

            // 1st request of burst is forwarded
            burstBridge.onWebRtcKeyframeRequest();
            require(burstCallbacks->submitDecodeUnit(&pframeUnit) == DR_NEED_IDR,
                    "First request of burst outside cooldown must be forwarded");

            // Subsequents requests in the burst are within cooldown and must be throttled
            const auto throttledBefore = burstBridge.keyframeRequestsThrottled();
            for (int i = 0; i < 5; ++i) {
                burstBridge.onWebRtcKeyframeRequest();
            }
            require(burstBridge.hasPendingIdrRequest(),
                    "hasPendingIdrRequest must be true during cooldown burst");
            const auto submitRes = burstCallbacks->submitDecodeUnit(&pframeUnit);
            require(submitRes == DR_OK,
                    "Burst requests within cooldown must be throttled");
            require(burstBridge.keyframeRequestsThrottled() > throttledBefore,
                    "Keyframe throttle counter must increment during request burst");

            // Arriving IDR clears both in-flight and deferred requests
            require(burstCallbacks->submitDecodeUnit(&idrUnit) == DR_OK,
                    "IDR frame must be accepted after burst");
            require(!burstBridge.hasPendingIdrRequest(),
                    "Arriving IDR must clear all pending/deferred burst requests");
        }

        // Edge Case: Concurrent multi-threaded keyframe requests
        {
            RecordingMediaSender stressSender;
            gateway::MoonlightMediaBridge stressBridge(
                stressSender,
                gateway::defaultStreamSettings(),
                [](const std::string&) {});
            auto* stressCallbacks = stressBridge.videoCallbacks();
            require(stressCallbacks->setup(VIDEO_FORMAT_H264, 1280, 720, 60, &stressBridge, 0) == 0,
                    "H.264 setup on stressBridge failed");
            stressCallbacks->start();

            std::vector<std::thread> threads;
            for (int t = 0; t < 4; ++t) {
                threads.emplace_back([&stressBridge]() {
                    for (int req = 0; req < 25; ++req) {
                        stressBridge.onWebRtcKeyframeRequest();
                        std::this_thread::yield();
                    }
                });
            }

            for (int i = 0; i < 30; ++i) {
                stressCallbacks->submitDecodeUnit(&pframeUnit);
                if (i % 5 == 0) {
                    stressCallbacks->submitDecodeUnit(&idrUnit);
                }
            }

            for (auto& th : threads) {
                th.join();
            }

            require(stressBridge.keyframeRequestsReceived() >= 100,
                    "Stress tested keyframe requests must be received");
            require(stressCallbacks->submitDecodeUnit(&idrUnit) == DR_OK,
                    "Bridge must remain in healthy state after concurrent stress");
        }

        auto* audioCallbacks = bridge.audioCallbacks();
        OPUS_MULTISTREAM_CONFIGURATION unsupportedAudio{};
        unsupportedAudio.sampleRate = 48000;
        unsupportedAudio.channelCount = 6;
        unsupportedAudio.samplesPerFrame = 960;
        require(audioCallbacks->init(0, &unsupportedAudio, &bridge, 0) != 0,
                "Surround Opus must be rejected");

        OPUS_MULTISTREAM_CONFIGURATION opusConfig{};
        opusConfig.sampleRate = 48000;
        opusConfig.channelCount = 2;
        opusConfig.streams = 1;
        opusConfig.coupledStreams = 1;
        opusConfig.samplesPerFrame = 960;
        opusConfig.mapping[0] = 0;
        opusConfig.mapping[1] = 1;
        require(audioCallbacks->init(0, &opusConfig, &bridge, 0) == 0,
                "Stereo Opus setup failed");
        audioCallbacks->start();

        std::array<char, 5> opusPacket = {0x01, 0x02, 0x03, 0x04, 0x05};
        audioCallbacks->decodeAndPlaySample(opusPacket.data(),
                                            static_cast<int>(opusPacket.size()));
        audioCallbacks->decodeAndPlaySample(opusPacket.data(),
                                            static_cast<int>(opusPacket.size()));

        require(sender.audio.size() == 2, "Encoded Opus packets were not forwarded");
        require(sender.audio[0].data
                    == std::vector<std::uint8_t>(opusPacket.begin(), opusPacket.end()),
                "Encoded Opus packet was modified");
        require(sender.audio[0].rtpTimestamp == 0 && sender.audio[1].rtpTimestamp == 960,
                "samplesPerFrame did not drive the Opus RTP timeline");

        bool keyframeLogFound = false;
        bool idrObservedLogFound = false;
        for (const auto& message : logs) {
            keyframeLogFound |= message == "WebRTC keyframe request forwarded to Moonlight";
            idrObservedLogFound |=
                message == "Moonlight IDR frame observed after keyframe request";
        }
        require(keyframeLogFound, "Keyframe forwarding diagnostic was not logged");
        require(idrObservedLogFound, "Moonlight IDR observation was not logged");

        audioCallbacks->stop();
        audioCallbacks->cleanup();
        videoCallbacks->stop();
        videoCallbacks->cleanup();

        RecordingMediaSender hevcSender;
        auto hevcSettings = gateway::defaultStreamSettings(
            1920, 1080, gateway::VideoCodec::HEVC);
        gateway::MoonlightMediaBridge hevcBridge(
            hevcSender,
            hevcSettings,
            [&logs](const std::string& message) { logs.push_back(message); });
        auto* hevcVideoCallbacks = hevcBridge.videoCallbacks();
        require(hevcVideoCallbacks->setup(
                    VIDEO_FORMAT_H265_MAIN10, 1920, 1080, 60, &hevcBridge, 0)
                    != 0,
                "HEVC Main10 must be rejected for an HEVC Main session");
        require(hevcVideoCallbacks->setup(
                    VIDEO_FORMAT_H265, 1920, 1080, 60, &hevcBridge, 0)
                    == 0,
                "HEVC Main setup failed");
        hevcVideoCallbacks->start();

        std::array<char, 7> vps = {0, 0, 0, 1, 0x40, 0x01, 0x11};
        std::array<char, 7> hevcSps = {0, 0, 0, 1, 0x42, 0x01, 0x22};
        std::array<char, 7> hevcPps = {0, 0, 0, 1, 0x44, 0x01, 0x33};
        std::array<char, 8> hevcIdr = {0, 0, 0, 1, 0x26, 0x01, 0x44, 0x55};
        LENTRY hevcIdrEntry{
            nullptr,
            hevcIdr.data(),
            static_cast<int>(hevcIdr.size()),
            BUFFER_TYPE_PICDATA};
        LENTRY hevcPpsEntry{
            &hevcIdrEntry,
            hevcPps.data(),
            static_cast<int>(hevcPps.size()),
            BUFFER_TYPE_PPS};
        LENTRY hevcSpsEntry{
            &hevcPpsEntry,
            hevcSps.data(),
            static_cast<int>(hevcSps.size()),
            BUFFER_TYPE_SPS};
        LENTRY vpsEntry{
            &hevcSpsEntry,
            vps.data(),
            static_cast<int>(vps.size()),
            BUFFER_TYPE_VPS};
        DECODE_UNIT hevcIdrUnit{};
        hevcIdrUnit.frameNumber = 100;
        hevcIdrUnit.frameType = FRAME_TYPE_IDR;
        hevcIdrUnit.rtpTimestamp = 90000;
        hevcIdrUnit.fullLength = static_cast<int>(
            vps.size() + hevcSps.size() + hevcPps.size() + hevcIdr.size());
        hevcIdrUnit.bufferList = &vpsEntry;

        require(hevcVideoCallbacks->submitDecodeUnit(&hevcIdrUnit) == DR_OK,
                "HEVC access unit was rejected");
        std::vector<std::uint8_t> expectedHevc;
        expectedHevc.insert(expectedHevc.end(), vps.begin(), vps.end());
        expectedHevc.insert(expectedHevc.end(), hevcSps.begin(), hevcSps.end());
        expectedHevc.insert(expectedHevc.end(), hevcPps.begin(), hevcPps.end());
        expectedHevc.insert(expectedHevc.end(), hevcIdr.begin(), hevcIdr.end());
        require(hevcSender.video.size() == 1
                    && hevcSender.video[0].data == expectedHevc
                    && hevcSender.video[0].codec == gateway::VideoCodec::HEVC,
                "HEVC VPS/SPS/PPS/IDR access unit was modified or misclassified");
        require(hevcSender.video[0].rtpTimestamp == 90000,
                "HEVC Moonlight RTP timestamp changed");

        std::array<char, 7> hevcPframe = {0, 0, 0, 1, 0x02, 0x01, 0x66};
        LENTRY hevcPframeEntry{nullptr,
                               hevcPframe.data(),
                               static_cast<int>(hevcPframe.size()),
                               BUFFER_TYPE_PICDATA};
        DECODE_UNIT hevcPframeUnit{};
        hevcPframeUnit.frameNumber = 101;
        hevcPframeUnit.frameType = FRAME_TYPE_PFRAME;
        hevcPframeUnit.rtpTimestamp = 91500;
        hevcPframeUnit.fullLength = static_cast<int>(hevcPframe.size());
        hevcPframeUnit.bufferList = &hevcPframeEntry;
        hevcBridge.onWebRtcKeyframeRequest();
        require(hevcVideoCallbacks->submitDecodeUnit(&hevcPframeUnit) == DR_NEED_IDR,
                "HEVC PLI did not produce one DR_NEED_IDR");
        require(hevcVideoCallbacks->submitDecodeUnit(&hevcPframeUnit) == DR_OK,
                "HEVC PLI produced more than one DR_NEED_IDR");
        require(hevcVideoCallbacks->submitDecodeUnit(&hevcIdrUnit) == DR_OK,
                "HEVC keyframe after PLI was rejected");
        hevcVideoCallbacks->stop();
        hevcVideoCallbacks->cleanup();

        RecordingMediaSender hdrSender;
        auto hdrSettings = hevcSettings;
        hdrSettings.hdr = true;
        bool hdrValidationFailed = false;
        gateway::MoonlightMediaBridge hdrBridge(
            hdrSender,
            hdrSettings,
            [&logs](const std::string& message) { logs.push_back(message); },
            [&hdrValidationFailed](const std::string&) {
                hdrValidationFailed = true;
            });
        auto* hdrCallbacks = hdrBridge.videoCallbacks();
        require(hdrCallbacks->setup(
                    VIDEO_FORMAT_H265, 1920, 1080, 60, &hdrBridge, 0)
                    != 0,
                "HEVC Main must be rejected for an HDR session");
        require(hdrCallbacks->setup(
                    VIDEO_FORMAT_H265_MAIN10, 1920, 1080, 60, &hdrBridge, 0)
                    == 0,
                "HEVC Main10 HDR setup failed");
        hdrCallbacks->start();

        std::vector<std::uint8_t> hdrVps{0, 0, 0, 1, 0x40, 0x01, 0x11};
        auto hdrSps = syntheticMain10Sps();
        std::vector<std::uint8_t> hdrPps{0, 0, 0, 1, 0x44, 0x01, 0x22};
        std::vector<std::uint8_t> hdrSei{0, 0, 0, 1, 0x4e, 0x01, 0x33, 0x80};
        std::vector<std::uint8_t> hdrIdr{0, 0, 0, 1, 0x26, 0x01, 0x44, 0x55};

        const auto parsedSps = gateway::parseHevcSps(hdrSps);
        require(parsedSps && parsedSps->isMain10_420()
                    && parsedSps->profileIdc == 2
                    && parsedSps->bitDepthLuma == 10
                    && parsedSps->bitDepthChroma == 10,
                "Synthetic HEVC Main10 SPS parser test failed");

        LENTRY hdrIdrEntry{nullptr,
                           reinterpret_cast<char*>(hdrIdr.data()),
                           static_cast<int>(hdrIdr.size()),
                           BUFFER_TYPE_PICDATA};
        LENTRY hdrSeiEntry{&hdrIdrEntry,
                           reinterpret_cast<char*>(hdrSei.data()),
                           static_cast<int>(hdrSei.size()),
                           BUFFER_TYPE_PICDATA};
        LENTRY hdrPpsEntry{&hdrSeiEntry,
                           reinterpret_cast<char*>(hdrPps.data()),
                           static_cast<int>(hdrPps.size()),
                           BUFFER_TYPE_PPS};
        LENTRY hdrSpsEntry{&hdrPpsEntry,
                           reinterpret_cast<char*>(hdrSps.data()),
                           static_cast<int>(hdrSps.size()),
                           BUFFER_TYPE_SPS};
        LENTRY hdrVpsEntry{&hdrSpsEntry,
                           reinterpret_cast<char*>(hdrVps.data()),
                           static_cast<int>(hdrVps.size()),
                           BUFFER_TYPE_VPS};
        DECODE_UNIT hdrUnit{};
        hdrUnit.frameNumber = 200;
        hdrUnit.frameType = FRAME_TYPE_IDR;
        hdrUnit.rtpTimestamp = 180000;
        hdrUnit.hdrActive = true;
        hdrUnit.colorspace = COLORSPACE_REC_2020;
        hdrUnit.fullLength = static_cast<int>(hdrVps.size() + hdrSps.size()
            + hdrPps.size() + hdrSei.size() + hdrIdr.size());
        hdrUnit.bufferList = &hdrVpsEntry;

        require(hdrCallbacks->submitDecodeUnit(&hdrUnit) == DR_OK,
                "Valid HEVC Main10 HDR access unit was rejected");
        std::vector<std::uint8_t> expectedHdr;
        for (const auto* part : {&hdrVps, &hdrSps, &hdrPps, &hdrSei, &hdrIdr}) {
            expectedHdr.insert(expectedHdr.end(), part->begin(), part->end());
        }
        require(hdrSender.video.size() == 1
                    && hdrSender.video[0].data == expectedHdr
                    && hdrSender.video[0].rtpTimestamp == 180000,
                "HDR VPS/SPS/PPS/SEI/IDR or Moonlight timestamp was modified");
        const auto hdrDiagnostics = hdrBridge.lastVideoFrame();
        require(hdrDiagnostics && hdrDiagnostics->hdrActive
                    && hdrDiagnostics->colorSpace == COLORSPACE_REC_2020
                    && hdrDiagnostics->main10Verified
                    && hdrDiagnostics->bitDepthLuma == 10
                    && hdrDiagnostics->bitDepthChroma == 10
                    && hdrDiagnostics->chromaFormatIdc == 1
                    && !hdrValidationFailed,
                "HDR DECODE_UNIT diagnostics were not propagated");
        require(std::ranges::find(logs, "HEVC bit depth: 10") != logs.end(),
                "Verified HEVC bit depth diagnostic was not logged");
        hdrCallbacks->stop();
        hdrCallbacks->cleanup();

        std::cout << "Moonlight media bridge tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Moonlight media bridge test failed: " << error.what() << '\n';
        return 1;
    }
}
