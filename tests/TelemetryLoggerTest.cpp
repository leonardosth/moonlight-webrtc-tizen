#include "telemetry/TelemetryLogger.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::string readFile(const std::filesystem::path& path)
{
    std::ifstream file(path);
    if (!file.is_open()) {
        return "";
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

} // namespace

int main()
{
    const std::filesystem::path testDir = "test_telemetry_output";

    try {
        if (std::filesystem::exists(testDir)) {
            std::filesystem::remove_all(testDir);
        }

        // Test 1: Session start, periodic telemetry, and session end summary
        {
            gateway::telemetry::TelemetryLogger logger(testDir, "test_session.log", 1024 * 1024);

            gateway::telemetry::SessionParameters params;
            params.sessionId = 42;
            params.appTitle = "Test Game 4K";
            params.width = 3840;
            params.height = 2160;
            params.fps = 60;
            params.codec = "HEVC";
            params.bitrateKbps = 50000;
            params.hdr = true;
            params.audioChannels = 2;
            params.latencyMode = "Lowest Latency";
            params.frameDeadlineUs = 25000;
            params.idrCooldownMs = 350;
            params.sdpBandwidthCapKbps = 50000;
            params.ulpfecPercentage = 10;
            params.clientIp = "192.168.1.100:50000";

            logger.logSessionStart(params);

            // Record periodic telemetry
            for (int i = 0; i < 5; ++i) {
                gateway::telemetry::ClientTelemetrySample client;
                client.decodedFps = 60.0;
                client.decodeTimeMs = 3.0 + i * 0.5; // 3.0, 3.5, 4.0, 4.5, 5.0
                client.networkRttMs = 2.0;
                client.videoJitterMs = 0.5;
                client.framesReceived = 100 * (i + 1);
                client.framesDecoded = 100 * (i + 1) - 1;
                client.framesDropped = 1;
                client.droppedPerSec = 0.0;
                client.packetsReceived = 1000 * (i + 1);
                client.packetsLost = (i == 0) ? 0 : (i * 5); // 0, 5, 10, 15, 20
                client.currentMbps = 49.5;
                client.rvfcProcessingMs = 1.0 + i * 0.1;
                client.rvfcExpectedDelayMs = 5.0 + i * 0.2;
                client.rvfcPresentedFrames = 100 * (i + 1);

                gateway::telemetry::HostTelemetrySample host;
                host.hostFps = 60.0f;
                host.hostFrameIntervalMs = 16.6f;
                host.hostFrameJitterMs = 0.2f;
                host.hostProcessingLatencyMs = 2.0f;
                host.gatewayLatencyMs = 0.3f;
                host.totalHostLatencyMs = 2.3f;
                host.totalFrames = 100 * (i + 1);
                host.gatewayLateFrameDrops = 2;
                host.keyframeRequestsReceived = 5;
                host.keyframeRequestsForwarded = 2;
                host.keyframeRequestsThrottled = 3;
                host.rfiRequestsSent = 4;
                host.pacerCatchUpEvents = 1;
                host.pacerQueueDepth = 4;
                host.nackRetransmittedPackets = 12;
                host.nackSuppressedDuplicates = 3;
                host.nackRateLimited = 1;
                host.fecPacketsGenerated = 8;

                logger.recordTelemetry(42, client, host);
            }

            logger.logSessionEnd(42);
        }

        const auto logContent = readFile(testDir / "test_session.log");
        require(!logContent.empty(), "Log file was not created or is empty");
        require(logContent.find("[SESSION START]") != std::string::npos, "Log must contain [SESSION START]");
        require(logContent.find("Session ID: 42") != std::string::npos, "Log must contain Session ID");
        require(logContent.find("Test Game 4K") != std::string::npos, "Log must contain application title");
        require(logContent.find("3840x2160 @ 60 FPS") != std::string::npos, "Log must contain resolution and FPS");
        require(logContent.find("HEVC (HDR: Enabled)") != std::string::npos, "Log must contain HEVC with HDR");
        require(logContent.find("50000 kbps (50.0 Mbps)") != std::string::npos, "Log must contain bitrate");
        require(logContent.find("Frame Drop Policy:    Active (25ms deadline)") != std::string::npos,
                "Log must contain Frame Drop Policy");
        require(logContent.find("Sunshine Encryption:  Enabled (AES-128 GCM)") != std::string::npos,
                "Log must contain Sunshine Encryption");
        require(logContent.find("Target Jitter Buffer: 15ms (Video), 40ms (Audio)") != std::string::npos,
                "Log must contain Target Jitter Buffer");
        require(logContent.find("Keyframe Rate Limiter: Active (350ms cooldown)") != std::string::npos,
                "Log must contain Keyframe Rate Limiter");
        require(logContent.find("NACK History Buffer:  8192 packets") != std::string::npos,
                "Log must contain NACK History Buffer");
        require(logContent.find("RTP Fragment Size:    1360 bytes") != std::string::npos,
                "Log must contain RTP Fragment Size");
        require(logContent.find("SDP Bandwidth Cap:    Enabled (b=AS:50000)") != std::string::npos,
                "Log must contain SDP Bandwidth Cap");
        require(logContent.find("ULPFEC Redundancy:    10% (RFC 5109)") != std::string::npos,
                "Log must contain ULPFEC Redundancy in start");
        require(logContent.find("[SESSION END]") != std::string::npos, "Log must contain [SESSION END]");
        require(logContent.find("TV Decode Time") != std::string::npos, "Log must contain TV Decode Time distribution");
        require(logContent.find("TV RVFC Processing") != std::string::npos, "Log must contain TV RVFC Processing distribution");
        require(logContent.find("TV RVFC Present Delay") != std::string::npos, "Log must contain TV RVFC Present Delay distribution");
        require(logContent.find("Gateway Late Drops:     2") != std::string::npos, "Log must contain Gateway Late Drops");
        require(logContent.find("Keyframe Requests (TV): 5") != std::string::npos, "Log must contain Keyframe Requests (TV)");
        require(logContent.find("Keyframes Forwarded:    2") != std::string::npos, "Log must contain Keyframes Forwarded");
        require(logContent.find("Keyframes Throttled:    3") != std::string::npos, "Log must contain Keyframes Throttled");
        require(logContent.find("RFI Requests (Loss):    4") != std::string::npos, "Log must contain RFI Requests in session end");
        require(logContent.find("ULPFEC Packets Gen:     8") != std::string::npos, "Log must contain ULPFEC Packets Gen in session end");
        require(logContent.find("dropGw: 2") != std::string::npos, "Log must contain dropGw in periodic line");
        require(logContent.find("idr: 5recv/2fwd/3throt/4rfi") != std::string::npos, "Log must contain idr and rfi in periodic line");
        require(logContent.find("fec: 8") != std::string::npos, "Log must contain fec in periodic line");
        require(logContent.find("lostPkts: 5 [+5]") != std::string::npos, "Log must contain packet loss delta in periodic line");
        require(logContent.find("pacer: q:4/catchUp:1") != std::string::npos, "Log must contain pacer stats in periodic line");
        require(logContent.find("nack: 12tx/3suppr/1lim") != std::string::npos, "Log must contain NACK limiter stats in periodic line");
        require(logContent.find("Pacer Catch-Up Events:  1") != std::string::npos, "Log must contain Pacer Catch-Up Events in session end");
        require(logContent.find("NACK Retransmissions:   12 (3 suppressed, 1 rate-limited)") != std::string::npos,
                "Log must contain NACK retransmission totals in session end");
        require(logContent.find("p50:") != std::string::npos, "Log must contain p50 metric");
        require(logContent.find("p95:") != std::string::npos, "Log must contain p95 metric");
        require(logContent.find("p99:") != std::string::npos, "Log must contain p99 metric");

        // Test 2: File Rotation on reaching size limit
        {
            const std::filesystem::path rotDir = "test_telemetry_rotation";
            if (std::filesystem::exists(rotDir)) {
                std::filesystem::remove_all(rotDir);
            }

            {
                // Max size set to small buffer (e.g. 1 KB) to trigger rotation quickly
                gateway::telemetry::TelemetryLogger rotLogger(rotDir, "rot_session.log", 1024);

                gateway::telemetry::SessionParameters params;
                params.sessionId = 1;
                rotLogger.logSessionStart(params);

                // Write enough entries to breach 1KB and trigger rotation
                for (int i = 0; i < 30; ++i) {
                    gateway::telemetry::ClientTelemetrySample client;
                    client.decodedFps = 60.0;
                    client.decodeTimeMs = 4.0;
                    rotLogger.recordTelemetry(1, client, std::nullopt);
                }

                require(std::filesystem::exists(rotDir / "rot_session.log.old"),
                        "Old log file (.log.old) must exist after rotation limit reached");
                require(std::filesystem::exists(rotDir / "rot_session.log"),
                        "Current log file must continue to exist after rotation");
            }

            std::filesystem::remove_all(rotDir);
        }

        std::filesystem::remove_all(testDir);
        std::cout << "Telemetry logger tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        if (std::filesystem::exists(testDir)) {
            std::filesystem::remove_all(testDir);
        }
        std::cerr << "Telemetry logger test failed: " << error.what() << '\n';
        return 1;
    }
}
