#include "telemetry/TelemetryLogger.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace gateway::telemetry {
namespace {

std::string formatIsoTimestamp(std::chrono::system_clock::time_point tp)
{
    const auto time = std::chrono::system_clock::to_time_t(tp);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        tp.time_since_epoch())
                        % 1000;
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &time);
#else
    gmtime_r(&time, &tm);
#endif
    std::ostringstream ss;
    ss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << '.'
       << std::setfill('0') << std::setw(3) << ms.count() << " UTC";
    return ss.str();
}

std::string formatDuration(std::chrono::milliseconds ms)
{
    const auto totalSecs = ms.count() / 1000;
    const auto hours = totalSecs / 3600;
    const auto mins = (totalSecs % 3600) / 60;
    const auto secs = totalSecs % 60;

    std::ostringstream ss;
    if (hours > 0) {
        ss << hours << "h " << mins << "m " << secs << "s";
    } else if (mins > 0) {
        ss << mins << "m " << secs << "s";
    } else {
        ss << secs << "s (" << ms.count() << " ms)";
    }
    return ss.str();
}

MetricDistribution computeDistribution(std::vector<double>& samples)
{
    if (samples.empty()) {
        return {};
    }
    std::sort(samples.begin(), samples.end());
    MetricDistribution dist;
    dist.count = samples.size();
    dist.min = samples.front();
    dist.max = samples.back();

    double sum = 0.0;
    for (const double val : samples) {
        sum += val;
    }
    dist.avg = sum / static_cast<double>(samples.size());

    auto getPercentile = [&samples](double pct) -> double {
        if (samples.empty()) {
            return 0.0;
        }
        auto idx = static_cast<std::size_t>(std::ceil(pct * samples.size())) - 1;
        if (idx >= samples.size()) {
            idx = samples.size() - 1;
        }
        return samples[idx];
    };

    dist.p50 = getPercentile(0.50);
    dist.p95 = getPercentile(0.95);
    dist.p99 = getPercentile(0.99);
    return dist;
}

std::string formatDistributionLine(std::string_view name,
                                   const MetricDistribution& dist,
                                   std::string_view unit = "ms")
{
    if (dist.count == 0) {
        std::ostringstream ss;
        ss << "  " << std::left << std::setw(24) << name << ": [No data]\n";
        return ss.str();
    }
    std::ostringstream ss;
    ss << "  " << std::left << std::setw(24) << name << ": "
       << "avg: " << std::fixed << std::setprecision(1) << dist.avg << unit
       << " | min: " << dist.min << unit
       << " | p50: " << dist.p50 << unit
       << " | p95: " << dist.p95 << unit
       << " | p99: " << dist.p99 << unit
       << " | max: " << dist.max << unit
       << " (samples: " << dist.count << ")\n";
    return ss.str();
}

} // namespace

TelemetryLogger::TelemetryLogger(std::filesystem::path logDirectory,
                                 std::string logFilename,
                                 std::size_t maxFileSizeBytes)
    : logDirectory_(std::move(logDirectory))
    , logFilename_(std::move(logFilename))
    , maxFileSizeBytes_(maxFileSizeBytes)
{
}

TelemetryLogger::~TelemetryLogger()
{
    const std::lock_guard lock(mutex_);
    if (logFile_.is_open()) {
        logFile_.flush();
        logFile_.close();
    }
}

void TelemetryLogger::ensureFileOpenLocked()
{
    if (logFile_.is_open()) {
        return;
    }

    try {
        std::filesystem::create_directories(logDirectory_);
    } catch (...) {
    }

    const auto filePath = logDirectory_ / logFilename_;
    if (std::filesystem::exists(filePath)) {
        try {
            currentFileSize_ = std::filesystem::file_size(filePath);
        } catch (...) {
            currentFileSize_ = 0;
        }
    } else {
        currentFileSize_ = 0;
    }

    logFile_.open(filePath, std::ios::out | std::ios::app);
}

void TelemetryLogger::rotateIfNeededLocked(std::size_t incomingBytes)
{
    if (currentFileSize_ + incomingBytes < maxFileSizeBytes_) {
        return;
    }

    if (logFile_.is_open()) {
        logFile_.flush();
        logFile_.close();
    }

    const auto currentPath = logDirectory_ / logFilename_;
    const auto oldPath = logDirectory_ / (logFilename_ + ".old");

    try {
        if (std::filesystem::exists(oldPath)) {
            std::filesystem::remove(oldPath);
        }
        if (std::filesystem::exists(currentPath)) {
            std::filesystem::rename(currentPath, oldPath);
        }
    } catch (...) {
    }

    currentFileSize_ = 0;
    logFile_.open(currentPath, std::ios::out | std::ios::trunc);
}

void TelemetryLogger::writeLocked(std::string_view text)
{
    ensureFileOpenLocked();
    rotateIfNeededLocked(text.size());

    if (logFile_.is_open()) {
        logFile_.write(text.data(), static_cast<std::streamsize>(text.size()));
        logFile_.flush();
        currentFileSize_ += text.size();
    }
}

void TelemetryLogger::logSessionStart(const SessionParameters& params)
{
    const std::lock_guard lock(mutex_);

    ActiveSessionData sessionData;
    sessionData.params = params;
    sessionData.startTime = std::chrono::system_clock::now();
    activeSession_ = std::move(sessionData);

    std::ostringstream ss;
    ss << "\n"
       << "================================================================================\n"
       << "[SESSION START] " << formatIsoTimestamp(activeSession_->startTime)
       << " | Session ID: " << params.sessionId << "\n"
       << "--------------------------------------------------------------------------------\n"
       << "Application:      " << (params.appTitle.empty() ? "Moonlight Stream" : params.appTitle) << "\n"
       << "Target Display:   " << params.width << "x" << params.height << " @ " << params.fps << " FPS\n"
       << "Video Codec:      " << params.codec << (params.hdr ? " (HDR: Enabled)" : " (SDR)") << "\n"
       << "Bitrate:          " << params.bitrateKbps << " kbps ("
       << std::fixed << std::setprecision(1) << (static_cast<double>(params.bitrateKbps) / 1000.0) << " Mbps)\n"
       << "Audio:            " << params.audioChannels << " Channels\n"
       << "Latency Mode:     " << params.latencyMode << "\n"
       << "Client Address:   " << (params.clientIp.empty() ? "Unknown" : params.clientIp) << "\n"
       << "Pipeline Flags:\n"
       << "  - Samsung Game Mode:    " << (params.gameMode ? "Enabled" : "Disabled") << "\n"
       << "  - RTP Playout Delay:    "
       << (params.rtpPlayoutDelay
               ? ("Negotiated & Active" + (params.rtpPlayoutDelayRange.empty() ? "" : (" (" + params.rtpPlayoutDelayRange + ")")))
               : "Disabled")
       << "\n"
       << "  - RVFC Frame Pacing:    " << (params.rvfc ? "Active" : "Disabled") << "\n"
       << "  - Packet Pacing:        " << (params.packetPacing.empty() ? "Disabled" : params.packetPacing) << "\n"
       << "  - Flat Buffer Pool:     " << (params.bufferPool ? "Enabled (4 slots x 4MB)" : "Disabled") << "\n"
       << "  - Frame Drop Policy:    " << (params.frameDeadlineUs > 0 ? ("Active (" + std::to_string(params.frameDeadlineUs / 1000) + "ms deadline)") : "Disabled (Smoothest)") << "\n"
       << "  - Sunshine Encryption:  " << (params.sunshineEncryption ? "Enabled (AES-128 GCM)" : "Disabled (Direct Loopback)") << "\n"
       << "  - Target Jitter Buffer: " << params.videoJitterBufferTargetMs << "ms (Video), " << params.audioJitterBufferTargetMs << "ms (Audio)\n"
       << "  - Keyframe Rate Limiter: Active (" << params.idrCooldownMs << "ms cooldown)\n"
       << "  - NACK History Buffer:  " << params.nackBufferSize << " packets\n"
       << "  - RTP Fragment Size:    " << params.rtpFragmentSize << " bytes\n"
       << "  - SDP Bandwidth Cap:    " << (params.sdpBandwidthCapKbps > 0 ? ("Enabled (b=AS:" + std::to_string(params.sdpBandwidthCapKbps) + ")") : "Disabled") << "\n"
       << "  - ULPFEC Redundancy:    " << (params.ulpfecPercentage > 0 ? (std::to_string(params.ulpfecPercentage) + "% (RFC 5109)") : "Disabled") << "\n"
       << "================================================================================\n";

    writeLocked(ss.str());
}

void TelemetryLogger::recordTelemetry(std::uint64_t sessionId,
                                     const ClientTelemetrySample& clientSample,
                                     const std::optional<HostTelemetrySample>& hostSample)
{
    const std::lock_guard lock(mutex_);
    std::int64_t deltaLoss = 0;

    if (activeSession_ && activeSession_->params.sessionId == sessionId) {
        // Collect metrics into vectors for percentiles
        if (clientSample.decodeTimeMs > 0.0) {
            activeSession_->decodeTimeSamples.push_back(clientSample.decodeTimeMs);
        }
        if (clientSample.networkRttMs > 0.0) {
            activeSession_->rttSamples.push_back(clientSample.networkRttMs);
        }
        if (clientSample.decodedFps > 0.0) {
            activeSession_->clientFpsSamples.push_back(clientSample.decodedFps);
        }
        if (clientSample.videoJitterMs > 0.0) {
            activeSession_->videoJitterSamples.push_back(clientSample.videoJitterMs);
        }
        if (clientSample.rvfcProcessingMs > 0.0) {
            activeSession_->rvfcProcessingSamples.push_back(clientSample.rvfcProcessingMs);
        }
        if (clientSample.rvfcExpectedDelayMs > 0.0) {
            activeSession_->rvfcDelaySamples.push_back(clientSample.rvfcExpectedDelayMs);
        }

        if (activeSession_->hasPreviousLossSample) {
            if (clientSample.packetsLost >= activeSession_->lastPacketsLost) {
                deltaLoss = static_cast<std::int64_t>(clientSample.packetsLost - activeSession_->lastPacketsLost);
            }
        }
        activeSession_->lastPacketsLost = clientSample.packetsLost;
        activeSession_->hasPreviousLossSample = true;

        if (hostSample) {
            if (hostSample->hostFps > 0.0f) {
                activeSession_->hostFpsSamples.push_back(hostSample->hostFps);
            }
            if (hostSample->hostProcessingLatencyMs > 0.0f) {
                activeSession_->hostProcessingSamples.push_back(hostSample->hostProcessingLatencyMs);
            }
            if (hostSample->gatewayLatencyMs >= 0.0f && hostSample->gatewayLatencyMs < 500.0f) {
                activeSession_->gatewayLatencySamples.push_back(hostSample->gatewayLatencyMs);
            }
            if (hostSample->flattenTimeMs > 0.0f) {
                activeSession_->flattenTimeSamples.push_back(hostSample->flattenTimeMs);
            }
            if (hostSample->totalHostLatencyMs > 0.0f) {
                activeSession_->hostLatencySamples.push_back(hostSample->totalHostLatencyMs);
            }
            activeSession_->lastHostSample = hostSample;
        }

        activeSession_->lastClientSample = clientSample;
    }

    std::ostringstream ss;
    ss << "[" << formatIsoTimestamp(clientSample.timestamp) << "] [SID:" << sessionId << "] ";

    if (hostSample) {
        ss << "Host: " << std::fixed << std::setprecision(1) << hostSample->hostFps << " FPS "
           << "(enc: " << hostSample->hostProcessingLatencyMs << "ms, gw: "
           << hostSample->gatewayLatencyMs << "ms, flat: "
           << hostSample->flattenTimeMs << "ms";
        if (hostSample->gatewayLateFrameDrops > 0) {
            ss << ", dropGw: " << hostSample->gatewayLateFrameDrops;
        }
        if (hostSample->keyframeRequestsReceived > 0 || hostSample->rfiRequestsSent > 0) {
            ss << ", idr: " << hostSample->keyframeRequestsReceived << "recv/"
               << hostSample->keyframeRequestsForwarded << "fwd/"
               << hostSample->keyframeRequestsThrottled << "throt";
            if (hostSample->rfiRequestsSent > 0) {
                ss << "/" << hostSample->rfiRequestsSent << "rfi";
            }
        }
        if (hostSample->pacerCatchUpEvents > 0 || hostSample->pacerQueueDepth > 0) {
            ss << ", pacer: q:" << hostSample->pacerQueueDepth
               << "/catchUp:" << hostSample->pacerCatchUpEvents;
        }
        if (hostSample->nackRetransmittedPackets > 0 || hostSample->nackSuppressedDuplicates > 0 || hostSample->nackRateLimited > 0) {
            ss << ", nack: " << hostSample->nackRetransmittedPackets << "tx/"
               << hostSample->nackSuppressedDuplicates << "suppr/"
               << hostSample->nackRateLimited << "lim";
        }
        if (hostSample->fecPacketsGenerated > 0) {
            ss << ", fec: " << hostSample->fecPacketsGenerated;
        }
        ss << ") | ";
    }

    ss << "TV: " << std::fixed << std::setprecision(1) << clientSample.decodedFps << " FPS "
       << "(decode: " << clientSample.decodeTimeMs << "ms, ";
    if (clientSample.rvfcProcessingMs > 0.0) {
        ss << "rvfc: " << clientSample.rvfcProcessingMs << "ms, ";
    }
    ss << "jit: " << clientSample.videoJitterMs << "ms, "
       << "rtt: " << clientSample.networkRttMs << "ms, "
       << "drop: " << clientSample.framesDropped
       << (clientSample.droppedPerSec > 0 ? " [+" + std::to_string(clientSample.droppedPerSec) + "/s]" : "");
    if (deltaLoss > 0) {
        ss << ", lostPkts: " << clientSample.packetsLost << " [+" << deltaLoss << "], ";
    } else {
        ss << ", lostPkts: " << clientSample.packetsLost << ", ";
    }
    ss << "bitr: " << clientSample.currentMbps << " Mbps)\n";

    writeLocked(ss.str());
}

void TelemetryLogger::logSessionEnd(std::uint64_t sessionId)
{
    const std::lock_guard lock(mutex_);

    if (!activeSession_ || activeSession_->params.sessionId != sessionId) {
        std::ostringstream ss;
        ss << "\n[SESSION END] " << formatIsoTimestamp(std::chrono::system_clock::now())
           << " | Session ID: " << sessionId << " (No active telemetry session recorded)\n";
        writeLocked(ss.str());
        return;
    }

    const auto endTime = std::chrono::system_clock::now();
    const auto durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        endTime - activeSession_->startTime);

    auto decodeDist = computeDistribution(activeSession_->decodeTimeSamples);
    auto hostLatencyDist = computeDistribution(activeSession_->hostLatencySamples);
    auto gwLatencyDist = computeDistribution(activeSession_->gatewayLatencySamples);
    auto flattenDist = computeDistribution(activeSession_->flattenTimeSamples);
    auto hostProcDist = computeDistribution(activeSession_->hostProcessingSamples);
    auto rttDist = computeDistribution(activeSession_->rttSamples);
    auto clientFpsDist = computeDistribution(activeSession_->clientFpsSamples);
    auto hostFpsDist = computeDistribution(activeSession_->hostFpsSamples);
    auto videoJitterDist = computeDistribution(activeSession_->videoJitterSamples);
    auto rvfcProcDist = computeDistribution(activeSession_->rvfcProcessingSamples);
    auto rvfcDelayDist = computeDistribution(activeSession_->rvfcDelaySamples);

    const auto& lastClient = activeSession_->lastClientSample;
    const auto totalFramesSent = activeSession_->lastHostSample ? activeSession_->lastHostSample->totalFrames : 0;
    const auto totalLateDrops = activeSession_->lastHostSample ? activeSession_->lastHostSample->gatewayLateFrameDrops : 0;
    const auto totalFramesDecoded = lastClient.framesDecoded;
    const auto totalFramesDropped = lastClient.framesDropped;
    const auto totalPacketsLost = lastClient.packetsLost;
    const auto totalPacerCatchUp = activeSession_->lastHostSample ? activeSession_->lastHostSample->pacerCatchUpEvents : 0;
    const auto totalNackRetransmit = activeSession_->lastHostSample ? activeSession_->lastHostSample->nackRetransmittedPackets : 0;
    const auto totalNackSuppressed = activeSession_->lastHostSample ? activeSession_->lastHostSample->nackSuppressedDuplicates : 0;
    const auto totalNackRateLimited = activeSession_->lastHostSample ? activeSession_->lastHostSample->nackRateLimited : 0;
    const auto totalKeyframeRecv = activeSession_->lastHostSample ? activeSession_->lastHostSample->keyframeRequestsReceived : 0;
    const auto totalKeyframeFwd = activeSession_->lastHostSample ? activeSession_->lastHostSample->keyframeRequestsForwarded : 0;
    const auto totalKeyframeThrot = activeSession_->lastHostSample ? activeSession_->lastHostSample->keyframeRequestsThrottled : 0;
    const auto totalRfiSent = activeSession_->lastHostSample ? activeSession_->lastHostSample->rfiRequestsSent : 0;
    const auto totalFecGen = activeSession_->lastHostSample ? activeSession_->lastHostSample->fecPacketsGenerated : 0;

    double dropRate = 0.0;
    if (totalFramesDecoded + totalFramesDropped > 0) {
        dropRate = (static_cast<double>(totalFramesDropped) * 100.0)
                   / static_cast<double>(totalFramesDecoded + totalFramesDropped);
    }

    std::ostringstream ss;
    ss << "\n"
       << "================================================================================\n"
       << "[SESSION END] " << formatIsoTimestamp(endTime)
       << " | Session ID: " << sessionId << " | Duration: " << formatDuration(durationMs) << "\n"
       << "--------------------------------------------------------------------------------\n"
       << "Frame & Packet Totals:\n"
       << "  Host Frames Sent:       " << totalFramesSent << "\n"
       << "  TV Frames Decoded:      " << totalFramesDecoded << "\n"
       << "  TV Frames Dropped:      " << totalFramesDropped << " ("
       << std::fixed << std::setprecision(2) << dropRate << "% drop rate)\n"
       << "  Gateway Late Drops:     " << totalLateDrops << "\n"
       << "  Packets Lost (RTP):     " << totalPacketsLost << "\n"
       << "  Pacer Catch-Up Events:  " << totalPacerCatchUp << "\n"
       << "  NACK Retransmissions:   " << totalNackRetransmit << " ("
       << totalNackSuppressed << " suppressed, " << totalNackRateLimited << " rate-limited)\n"
       << "  Keyframe Requests (TV): " << totalKeyframeRecv << "\n"
       << "  Keyframes Forwarded:    " << totalKeyframeFwd << "\n"
       << "  Keyframes Throttled:    " << totalKeyframeThrot << " (Suppressed by rate limiter)\n"
       << "  RFI Requests (Loss):    " << totalRfiSent << "\n"
       << "  ULPFEC Packets Gen:     " << totalFecGen << "\n\n"
       << "Performance Metrics Distribution:\n"
       << formatDistributionLine("TV Decode Time", decodeDist, "ms");
    if (!activeSession_->rvfcProcessingSamples.empty()) {
        ss << formatDistributionLine("TV RVFC Processing", rvfcProcDist, "ms");
    }
    if (!activeSession_->rvfcDelaySamples.empty()) {
        ss << formatDistributionLine("TV RVFC Present Delay", rvfcDelayDist, "ms");
    }
    ss << formatDistributionLine("Host Latency (Total)", hostLatencyDist, "ms")
       << formatDistributionLine("Host Processing (Enc)", hostProcDist, "ms")
       << formatDistributionLine("Gateway Processing", gwLatencyDist, "ms")
       << formatDistributionLine("Gateway Flatten Time", flattenDist, "ms")
       << formatDistributionLine("Network RTT", rttDist, "ms")
       << formatDistributionLine("Video RTP Jitter", videoJitterDist, "ms")
       << formatDistributionLine("TV Decoded FPS", clientFpsDist, " fps")
       << formatDistributionLine("Host Captured FPS", hostFpsDist, " fps")
       << "================================================================================\n\n";

    writeLocked(ss.str());
    activeSession_.reset();
}

} // namespace gateway::telemetry
