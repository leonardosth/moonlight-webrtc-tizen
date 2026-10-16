#pragma once

#include "session/StreamSettings.h"
#include "moonlight/MoonlightMediaBridge.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace gateway::telemetry {

struct SessionParameters {
    std::uint64_t sessionId = 0;
    std::string appTitle;
    int width = 0;
    int height = 0;
    int fps = 0;
    std::string codec;
    int bitrateKbps = 0;
    bool hdr = false;
    int audioChannels = 2;
    std::string latencyMode = "Balanced";
    std::string packetPacing = "Balanced (1.50x smooth)";
    bool gameMode = true;
    bool rtpPlayoutDelay = false;
    std::string rtpPlayoutDelayRange;
    bool rvfc = true;
    bool bufferPool = true;
    bool frameDropPolicy = true;
    std::uint64_t frameDeadlineUs = 0;
    std::uint32_t idrCooldownMs = 500;
    int sdpBandwidthCapKbps = 0;
    std::size_t nackBufferSize = 8192;
    std::size_t rtpFragmentSize = 1360;
    bool sunshineEncryption = true;
    int videoJitterBufferTargetMs = 15;
    int audioJitterBufferTargetMs = 40;
    std::string frameDropPolicyName = "auto";
    std::string clientIp;
};

struct ClientTelemetrySample {
    std::chrono::system_clock::time_point timestamp = std::chrono::system_clock::now();
    double decodedFps = 0.0;
    double decodeTimeMs = 0.0;
    double networkRttMs = 0.0;
    double videoJitterMs = 0.0;
    std::uint64_t framesReceived = 0;
    std::uint64_t framesDecoded = 0;
    std::uint64_t framesDropped = 0;
    double droppedPerSec = 0.0;
    std::uint64_t packetsReceived = 0;
    std::uint64_t packetsLost = 0;
    double currentMbps = 0.0;
    std::uint64_t audioPacketsReceived = 0;
    std::uint64_t audioPacketsLost = 0;
    double audioJitterMs = 0.0;
    double clientFrameIntervalMs = 0.0;
    double clientFrameJitterMs = 0.0;
    double rvfcProcessingMs = 0.0;
    double rvfcExpectedDelayMs = 0.0;
    std::uint64_t rvfcPresentedFrames = 0;
};

struct HostTelemetrySample {
    std::chrono::system_clock::time_point timestamp = std::chrono::system_clock::now();
    float hostFps = 0.0f;
    float hostFrameIntervalMs = 0.0f;
    float hostFrameJitterMs = 0.0f;
    float hostProcessingLatencyMs = 0.0f;
    float gatewayLatencyMs = 0.0f;
    float totalHostLatencyMs = 0.0f;
    float flattenTimeMs = 0.0f;
    std::uint64_t totalFrames = 0;
    std::uint64_t gatewayLateFrameDrops = 0;
    std::uint64_t keyframeRequestsReceived = 0;
    std::uint64_t keyframeRequestsForwarded = 0;
    std::uint64_t keyframeRequestsThrottled = 0;
    std::uint64_t rfiRequestsSent = 0;
    std::uint64_t pacerQueueDepth = 0;
    std::uint64_t pacerCatchUpEvents = 0;
    std::uint64_t nackRetransmittedPackets = 0;
    std::uint64_t nackSuppressedDuplicates = 0;
    std::uint64_t nackRateLimited = 0;
};

struct MetricDistribution {
    double min = 0.0;
    double max = 0.0;
    double avg = 0.0;
    double p50 = 0.0;
    double p95 = 0.0;
    double p99 = 0.0;
    std::size_t count = 0;
};

class TelemetryLogger {
public:
    explicit TelemetryLogger(std::filesystem::path logDirectory = "logs",
                            std::string logFilename = "session_telemetry.log",
                            std::size_t maxFileSizeBytes = 10 * 1024 * 1024);

    ~TelemetryLogger();

    void logSessionStart(const SessionParameters& params);
    void recordTelemetry(std::uint64_t sessionId,
                         const ClientTelemetrySample& clientSample,
                         const std::optional<HostTelemetrySample>& hostSample);
    void logSessionEnd(std::uint64_t sessionId);

private:
    void ensureFileOpenLocked();
    void rotateIfNeededLocked(std::size_t incomingBytes);
    void writeLocked(std::string_view text);

    std::filesystem::path logDirectory_;
    std::string logFilename_;
    std::size_t maxFileSizeBytes_;
    std::ofstream logFile_;
    std::size_t currentFileSize_ = 0;
    std::mutex mutex_;

    // Active session state and sample accumulation for final summary
    struct ActiveSessionData {
        SessionParameters params;
        std::chrono::system_clock::time_point startTime;
        std::vector<double> decodeTimeSamples;
        std::vector<double> hostLatencySamples;
        std::vector<double> gatewayLatencySamples;
        std::vector<double> flattenTimeSamples;
        std::vector<double> hostProcessingSamples;
        std::vector<double> rttSamples;
        std::vector<double> clientFpsSamples;
        std::vector<double> hostFpsSamples;
        std::vector<double> videoJitterSamples;
        std::vector<double> rvfcProcessingSamples;
        std::vector<double> rvfcDelaySamples;
        ClientTelemetrySample lastClientSample;
        std::optional<HostTelemetrySample> lastHostSample;
        std::uint64_t lastPacketsLost = 0;
        bool hasPreviousLossSample = false;
    };

    std::optional<ActiveSessionData> activeSession_;
};

} // namespace gateway::telemetry
