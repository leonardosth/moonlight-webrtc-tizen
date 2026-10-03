#include "moonlight/MoonlightMediaBridge.h"

#include "media/HevcSpsParser.h"
#include "moonlight/MoonlightVideoProfile.h"

#include <cmath>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <utility>

extern "C" std::uint64_t PltGetMicroseconds(void);

namespace gateway {

std::atomic<MoonlightMediaBridge*> MoonlightMediaBridge::activeVideoBridge_ = nullptr;
std::atomic<MoonlightMediaBridge*> MoonlightMediaBridge::activeAudioBridge_ = nullptr;

MoonlightMediaBridge::MoonlightMediaBridge(MediaSender& sender,
                                           StreamSettings settings,
                                           Logger logger,
                                           ValidationFailureHandler validationFailureHandler)
    : sender_(sender)
    , settings_(settings)
    , logger_(std::move(logger))
    , validationFailureHandler_(std::move(validationFailureHandler))
{
    if (const auto error = validateStreamSettings(settings_)) {
        throw std::invalid_argument(*error);
    }
    LiInitializeVideoCallbacks(&videoCallbacks_);
    videoCallbacks_.setup = &MoonlightMediaBridge::videoSetupCallback;
    videoCallbacks_.start = &MoonlightMediaBridge::videoStartCallback;
    videoCallbacks_.stop = &MoonlightMediaBridge::videoStopCallback;
    videoCallbacks_.cleanup = &MoonlightMediaBridge::videoCleanupCallback;
    videoCallbacks_.submitDecodeUnit = &MoonlightMediaBridge::videoSubmitCallback;

    LiInitializeAudioCallbacks(&audioCallbacks_);
    audioCallbacks_.init = &MoonlightMediaBridge::audioInitCallback;
    audioCallbacks_.start = &MoonlightMediaBridge::audioStartCallback;
    audioCallbacks_.stop = &MoonlightMediaBridge::audioStopCallback;
    audioCallbacks_.cleanup = &MoonlightMediaBridge::audioCleanupCallback;
    audioCallbacks_.decodeAndPlaySample = &MoonlightMediaBridge::audioSubmitCallback;
    audioCallbacks_.capabilities = CAPABILITY_SUPPORTS_ARBITRARY_AUDIO_DURATION;
}

MoonlightMediaBridge::~MoonlightMediaBridge()
{
    auto* expected = this;
    activeVideoBridge_.compare_exchange_strong(expected, nullptr);
    expected = this;
    activeAudioBridge_.compare_exchange_strong(expected, nullptr);
}

DECODER_RENDERER_CALLBACKS* MoonlightMediaBridge::videoCallbacks()
{
    return &videoCallbacks_;
}

AUDIO_RENDERER_CALLBACKS* MoonlightMediaBridge::audioCallbacks()
{
    return &audioCallbacks_;
}

void MoonlightMediaBridge::onWebRtcKeyframeRequest()
{
    moonlightIdrRequested_.store(true, std::memory_order_release);
}

bool MoonlightMediaBridge::hasPendingIdrRequest() const
{
    return moonlightIdrRequested_.load(std::memory_order_acquire);
}

std::optional<MoonlightVideoDiagnostics> MoonlightMediaBridge::lastVideoFrame() const
{
    const std::lock_guard lock(diagnosticsMutex_);
    return lastVideoFrame_;
}

MoonlightStreamDiagnostics MoonlightMediaBridge::currentStreamDiagnostics() const
{
    const std::lock_guard lock(diagnosticsMutex_);
    return currentStreamDiagnostics_;
}

int MoonlightMediaBridge::videoSetupCallback(int videoFormat,
                                              int width,
                                              int height,
                                              int redrawRate,
                                              void* context,
                                              int drFlags)
{
    auto* bridge = static_cast<MoonlightMediaBridge*>(context);
    if (!bridge) {
        return -1;
    }

    activeVideoBridge_.store(bridge, std::memory_order_release);
    return bridge->setupVideo(videoFormat, width, height, redrawRate, drFlags);
}

void MoonlightMediaBridge::videoStartCallback()
{
    if (auto* bridge = activeVideoBridge_.load(std::memory_order_acquire)) {
        bridge->videoStarted_.store(true, std::memory_order_release);
        const std::lock_guard lock(bridge->diagnosticsMutex_);
        bridge->nextVideoLog_ = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        bridge->hdrValidationDeadline_ =
            std::chrono::steady_clock::now() + std::chrono::seconds(2);
    }
}

void MoonlightMediaBridge::videoStopCallback()
{
    if (auto* bridge = activeVideoBridge_.load(std::memory_order_acquire)) {
        bridge->videoStarted_.store(false, std::memory_order_release);
    }
}

void MoonlightMediaBridge::videoCleanupCallback()
{
    if (auto* bridge = activeVideoBridge_.exchange(nullptr, std::memory_order_acq_rel)) {
        bridge->videoStarted_.store(false, std::memory_order_release);
        bridge->videoConfigured_.store(false, std::memory_order_release);
    }
}

int MoonlightMediaBridge::videoSubmitCallback(PDECODE_UNIT decodeUnit)
{
    auto* bridge = activeVideoBridge_.load(std::memory_order_acquire);
    if (!bridge) {
        return DR_NEED_IDR;
    }

    try {
        return bridge->submitVideo(decodeUnit);
    } catch (const std::exception& error) {
        bridge->log("Moonlight video callback error: " + std::string(error.what()));
        return DR_NEED_IDR;
    } catch (...) {
        bridge->log("Moonlight video callback error: unknown exception");
        return DR_NEED_IDR;
    }
}

int MoonlightMediaBridge::audioInitCallback(
    int,
    const POPUS_MULTISTREAM_CONFIGURATION opusConfig,
    void* context,
    int arFlags)
{
    auto* bridge = static_cast<MoonlightMediaBridge*>(context);
    if (!bridge) {
        return -1;
    }

    activeAudioBridge_.store(bridge, std::memory_order_release);
    return bridge->setupAudio(opusConfig, arFlags);
}

void MoonlightMediaBridge::audioStartCallback()
{
    if (auto* bridge = activeAudioBridge_.load(std::memory_order_acquire)) {
        bridge->audioStarted_.store(true, std::memory_order_release);
        const std::lock_guard lock(bridge->diagnosticsMutex_);
        bridge->nextAudioLog_ = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    }
}

void MoonlightMediaBridge::audioStopCallback()
{
    if (auto* bridge = activeAudioBridge_.load(std::memory_order_acquire)) {
        bridge->audioStarted_.store(false, std::memory_order_release);
    }
}

void MoonlightMediaBridge::audioCleanupCallback()
{
    if (auto* bridge = activeAudioBridge_.exchange(nullptr, std::memory_order_acq_rel)) {
        bridge->audioStarted_.store(false, std::memory_order_release);
        bridge->audioConfigured_.store(false, std::memory_order_release);
    }
}

void MoonlightMediaBridge::audioSubmitCallback(char* sampleData, int sampleLength)
{
    auto* bridge = activeAudioBridge_.load(std::memory_order_acquire);
    if (!bridge) {
        return;
    }

    try {
        bridge->submitAudio(sampleData, sampleLength);
    } catch (const std::exception& error) {
        bridge->log("Moonlight audio callback error: " + std::string(error.what()));
    } catch (...) {
        bridge->log("Moonlight audio callback error: unknown exception");
    }
}

int MoonlightMediaBridge::setupVideo(int videoFormat,
                                      int width,
                                      int height,
                                      int redrawRate,
                                      int)
{
    const auto requestedProfile = moonlight::moonlightVideoProfile(settings_);
    const int expectedFormat = requestedProfile.videoFormat;
    const auto actualCodecName = videoFormat == VIDEO_FORMAT_H264
        ? "H.264 High"
        : videoFormat == VIDEO_FORMAT_H265
        ? "HEVC Main"
        : videoFormat == VIDEO_FORMAT_H265_MAIN10
        ? "HEVC Main10"
        : videoFormat == VIDEO_FORMAT_H265_REXT8_444
        ? "HEVC RExt 8-bit 4:4:4"
        : videoFormat == VIDEO_FORMAT_H265_REXT10_444
        ? "HEVC RExt 10-bit 4:4:4"
        : videoFormat == VIDEO_FORMAT_AV1_MAIN8
        ? "AV1 Main 8-bit"
        : videoFormat == VIDEO_FORMAT_AV1_MAIN10
        ? "AV1 Main 10-bit"
        : "unexpected";
    std::ostringstream message;
    message << "Moonlight video setup: " << width << 'x' << height << " @ " << redrawRate
            << ", codec=" << actualCodecName << ", format=" << videoFormat;
    log(message.str());

    if (videoFormat != expectedFormat) {
        log("Unsupported Moonlight video format: " + std::to_string(videoFormat)
            + " (requested " + std::string(requestedProfile.name) + ")");
        videoConfigured_.store(false, std::memory_order_release);
        return -1;
    }
    if (width != settings_.width || height != settings_.height
        || redrawRate != settings_.fps) {
        log("Moonlight video dimensions do not match the requested stream settings");
        videoConfigured_.store(false, std::memory_order_release);
        return -1;
    }

    if (settings_.codec == VideoCodec::AV1) {
        bitDepthLuma_.store(settings_.hdr ? 10 : 8, std::memory_order_release);
        bitDepthChroma_.store(settings_.hdr ? 10 : 8, std::memory_order_release);
        chromaFormatIdc_.store(1, std::memory_order_release);
    }

    videoConfigured_.store(true, std::memory_order_release);
    return 0;
}

int MoonlightMediaBridge::submitVideo(PDECODE_UNIT decodeUnit)
{
    if (!videoConfigured_.load(std::memory_order_acquire)
        || !videoStarted_.load(std::memory_order_acquire) || !decodeUnit) {
        return DR_NEED_IDR;
    }

    // Determine if AV1 Temporal Delimiter OBU (0x12, 0x00) should be prepended
    std::span<const std::uint8_t> prefix;
    constexpr std::uint8_t obuTemporalDelimiter[] = { 0x12, 0x00 };
    if (settings_.codec == VideoCodec::AV1 && decodeUnit->bufferList && decodeUnit->bufferList->data) {
        const auto* firstChunk = reinterpret_cast<const std::uint8_t*>(decodeUnit->bufferList->data);
        const bool hasTemporalDelimiter = decodeUnit->bufferList->length >= 2
            && firstChunk[0] == obuTemporalDelimiter[0]
            && firstChunk[1] == obuTemporalDelimiter[1];
        if (!hasTemporalDelimiter) {
            prefix = std::span<const std::uint8_t>(obuTemporalDelimiter, sizeof(obuTemporalDelimiter));
        }
    }

    auto lease = bufferPool_.flatten(decodeUnit, prefix);
    if (!lease) {
        log("Invalid Moonlight video decode unit");
        return DR_NEED_IDR;
    }

    const auto flattened = lease->data();
    const float flattenMs = static_cast<float>(lease->duration().count()) / 1'000'000.0f;

    if (!firstVideoFrameLogged_.exchange(true, std::memory_order_acq_rel)) {
        std::ostringstream message;
        message << "First Moonlight DECODE_UNIT: codec=" << videoCodecName(settings_.codec)
                << ", hdrActive=" << (decodeUnit->hdrActive ? "true" : "false")
                << ", colorSpace=" << static_cast<int>(decodeUnit->colorspace)
                << (decodeUnit->colorspace == COLORSPACE_REC_2020 ? " (Rec.2020)" : "")
                << ", frameType=" << decodeUnit->frameType
                << ", RTP=" << decodeUnit->rtpTimestamp
                << ", size=" << flattened.size()
                << ", flattenTime=" << std::fixed << std::setprecision(2) << flattenMs << "ms";
        if (!flattened.empty()) {
            message << ", first bytes=";
            const auto count = std::min<std::size_t>(flattened.size(), 12);
            for (std::size_t i = 0; i < count; ++i) {
                message << std::hex << std::setw(2) << std::setfill('0')
                        << static_cast<int>(flattened[i]) << ' ';
            }
        }
        log(message.str());
    }

    if (settings_.hdr && !hdrValidationFailed_.load(std::memory_order_acquire)) {
        if (decodeUnit->hdrActive) {
            observedHdrActive_.store(true, std::memory_order_release);
        }
        if (decodeUnit->colorspace == COLORSPACE_REC_2020) {
            observedRec2020_.store(true, std::memory_order_release);
        }

        if (!main10Verified_.load(std::memory_order_acquire)) {
            if (settings_.codec == VideoCodec::AV1) {
                if (observedHdrActive_.load(std::memory_order_acquire)
                    && observedRec2020_.load(std::memory_order_acquire)) {
                    bitDepthLuma_.store(10, std::memory_order_release);
                    bitDepthChroma_.store(10, std::memory_order_release);
                    chromaFormatIdc_.store(1, std::memory_order_release);
                    main10Verified_.store(true, std::memory_order_release);
                    log("AV1 HDR: Profile 0 Main 10-bit 4:2:0 Rec.2020 verified");
                }
            } else if (settings_.codec == VideoCodec::HEVC) {
                if (const auto sps = parseHevcSps(flattened)) {
                    bitDepthLuma_.store(sps->bitDepthLuma, std::memory_order_release);
                    bitDepthChroma_.store(sps->bitDepthChroma, std::memory_order_release);
                    chromaFormatIdc_.store(sps->chromaFormatIdc, std::memory_order_release);
                    std::ostringstream message;
                    message << "HEVC SPS: profile_idc=" << sps->profileIdc
                            << ", Main10-compatible="
                            << (sps->main10CompatibleProfile ? "yes" : "no")
                            << ", chroma_format_idc=" << sps->chromaFormatIdc
                            << ", bit_depth_luma=" << sps->bitDepthLuma
                            << ", bit_depth_chroma=" << sps->bitDepthChroma;
                    log(message.str());
                    if (!sps->isMain10_420()) {
                        failHdrValidation(
                            "HDR HEVC SPS is not Main10 10-bit 4:2:0 compatible");
                    } else {
                        main10Verified_.store(true, std::memory_order_release);
                        log("HEVC bit depth: 10");
                    }
                }
            }
        }

        if (observedHdrActive_.load(std::memory_order_acquire)
            && observedRec2020_.load(std::memory_order_acquire)
            && main10Verified_.load(std::memory_order_acquire)
            && !hdrValidationComplete_.exchange(true, std::memory_order_acq_rel)) {
            log(std::string("Moonlight HDR validation passed: ")
                + (settings_.codec == VideoCodec::AV1 ? "AV1 Main 10-bit" : "HEVC Main10")
                + " 4:2:0, hdrActive=true, Rec.2020");
        }

        std::chrono::steady_clock::time_point deadline;
        {
            const std::lock_guard lock(diagnosticsMutex_);
            deadline = hdrValidationDeadline_;
        }
        if (!hdrValidationComplete_.load(std::memory_order_acquire)
            && deadline != std::chrono::steady_clock::time_point{}
            && std::chrono::steady_clock::now() >= deadline) {
            std::ostringstream reason;
            reason << "HDR validation timed out (hdrActive="
                   << (observedHdrActive_.load() ? "true" : "false")
                   << ", Rec.2020=" << (observedRec2020_.load() ? "true" : "false")
                   << ", 10-bit=" << (main10Verified_.load() ? "true" : "false")
                   << ')';
            failHdrValidation(reason.str());
        }
    }

    sender_.sendVideoAccessUnit(settings_.codec, flattened, decodeUnit->rtpTimestamp);

    if (decodeUnit->frameType == FRAME_TYPE_IDR
        && waitingForMoonlightIdr_.exchange(false, std::memory_order_acq_rel)) {
        log("Moonlight IDR frame observed after keyframe request");
    }

    const auto frames = videoFrames_.fetch_add(1, std::memory_order_relaxed) + 1;
    const auto bytes = videoBytes_.fetch_add(flattened.size(), std::memory_order_relaxed)
        + flattened.size();
    const auto now = std::chrono::steady_clock::now();
    const auto nowPltUs = PltGetMicroseconds();

    const float hostLatencyMs = decodeUnit->frameHostProcessingLatency > 0
        ? static_cast<float>(decodeUnit->frameHostProcessingLatency) * 0.1f
        : 0.0f;

    float gatewayLatencyMs = 0.0f;
    if (decodeUnit->receiveTimeUs > 0 && nowPltUs >= decodeUnit->receiveTimeUs) {
        gatewayLatencyMs = static_cast<float>(nowPltUs - decodeUnit->receiveTimeUs) / 1000.0f;
        // Gateway transit and queue delay is normally 0.5ms - 10ms. Clamp anomalies (>500ms).
        if (gatewayLatencyMs > 500.0f) {
            gatewayLatencyMs = 0.0f;
        }
    }

    bool report = false;
    {
        const std::lock_guard lock(diagnosticsMutex_);
        lastVideoFrame_ = MoonlightVideoDiagnostics{
            decodeUnit->frameNumber,
            decodeUnit->frameType,
            decodeUnit->rtpTimestamp,
            decodeUnit->presentationTimeUs,
            decodeUnit->receiveTimeUs,
            decodeUnit->enqueueTimeUs,
            flattened.size(),
            decodeUnit->hdrActive,
            static_cast<int>(decodeUnit->colorspace),
            main10Verified_.load(std::memory_order_acquire),
            bitDepthLuma_.load(std::memory_order_acquire) > 0
                ? std::optional<int>(bitDepthLuma_.load(std::memory_order_acquire))
                : std::nullopt,
            bitDepthChroma_.load(std::memory_order_acquire) > 0
                ? std::optional<int>(bitDepthChroma_.load(std::memory_order_acquire))
                : std::nullopt,
            chromaFormatIdc_.load(std::memory_order_acquire) >= 0
                ? std::optional<int>(chromaFormatIdc_.load(std::memory_order_acquire))
                : std::nullopt,
            decodeUnit->frameHostProcessingLatency,
            flattenMs,
        };

        double intervalMs = 0.0;
        if (lastSubmitTime_ != std::chrono::steady_clock::time_point{}) {
            intervalMs = std::chrono::duration<double, std::milli>(now - lastSubmitTime_).count();
        }
        lastSubmitTime_ = now;

        if (windowStartTime_ == std::chrono::steady_clock::time_point{}) {
            windowStartTime_ = now;
        }
        windowFrameCount_++;
        if (intervalMs > 0.0 && intervalMs < 1000.0) {
            windowIntervalSumMs_ += intervalMs;
            windowIntervalSqSumMs_ += intervalMs * intervalMs;
        }
        if (hostLatencyMs > 0.0f) {
            windowHostLatencySumMs_ += hostLatencyMs;
        }
        windowGatewayLatencySumMs_ += gatewayLatencyMs;
        windowFlattenTimeSumMs_ += flattenMs;

        const auto windowElapsed = std::chrono::duration<double>(now - windowStartTime_).count();
        if (windowElapsed >= 0.5) {
            const float hostFps = windowElapsed > 0.0
                ? static_cast<float>(windowFrameCount_ / windowElapsed)
                : 0.0f;
            const float avgIntervalMs = (windowFrameCount_ > 0 && windowIntervalSumMs_ > 0.0)
                ? static_cast<float>(windowIntervalSumMs_ / windowFrameCount_)
                : (settings_.fps > 0 ? 1000.0f / static_cast<float>(settings_.fps) : 16.6f);

            float jitterMs = 0.0f;
            if (windowFrameCount_ > 1) {
                const double variance = (windowIntervalSqSumMs_ / windowFrameCount_)
                    - (static_cast<double>(avgIntervalMs) * static_cast<double>(avgIntervalMs));
                if (variance > 0.0) {
                    jitterMs = static_cast<float>(std::sqrt(variance));
                }
            }

            const float avgHostLatency = (windowFrameCount_ > 0 && windowHostLatencySumMs_ > 0.0)
                ? static_cast<float>(windowHostLatencySumMs_ / windowFrameCount_)
                : hostLatencyMs;
            const float avgGatewayLatency = windowFrameCount_ > 0
                ? static_cast<float>(windowGatewayLatencySumMs_ / windowFrameCount_)
                : gatewayLatencyMs;
            const float avgFlattenTime = windowFrameCount_ > 0
                ? static_cast<float>(windowFlattenTimeSumMs_ / windowFrameCount_)
                : flattenMs;

            const std::string colorName = decodeUnit->colorspace == COLORSPACE_REC_2020
                ? "Rec.2020"
                : (decodeUnit->colorspace == COLORSPACE_REC_709 ? "Rec.709" : "Rec.601");

            const int bitDepth = bitDepthLuma_.load(std::memory_order_acquire) > 0
                ? bitDepthLuma_.load(std::memory_order_acquire)
                : (settings_.hdr ? 10 : 8);

            currentStreamDiagnostics_ = MoonlightStreamDiagnostics{
                true,
                decodeUnit->hdrActive,
                static_cast<int>(decodeUnit->colorspace),
                colorName,
                bitDepth,
                main10Verified_.load(std::memory_order_acquire),
                std::string(videoCodecName(settings_.codec)),
                settings_.width,
                settings_.height,
                settings_.fps,
                hostFps,
                avgIntervalMs,
                jitterMs,
                avgHostLatency,
                avgGatewayLatency,
                avgFlattenTime,
                avgHostLatency + avgGatewayLatency,
                frames,
                bytes
            };

            windowStartTime_ = now;
            windowFrameCount_ = 0;
            windowIntervalSumMs_ = 0.0;
            windowIntervalSqSumMs_ = 0.0;
            windowHostLatencySumMs_ = 0.0;
            windowGatewayLatencySumMs_ = 0.0;
            windowFlattenTimeSumMs_ = 0.0;
        }

        if (now >= nextVideoLog_) {
            nextVideoLog_ = now + std::chrono::seconds(1);
            report = true;
        }
    }

    if (report) {
        std::ostringstream message;
        message << "Moonlight video: " << frames << " frames, " << bytes
                << " bytes, last RTP=" << decodeUnit->rtpTimestamp;
        log(message.str());
    }

    if (moonlightIdrRequested_.exchange(false, std::memory_order_acq_rel)) {
        waitingForMoonlightIdr_.store(true, std::memory_order_release);
        log("WebRTC keyframe request forwarded to Moonlight");
        return DR_NEED_IDR;
    }

    return DR_OK;
}

int MoonlightMediaBridge::setupAudio(const POPUS_MULTISTREAM_CONFIGURATION opusConfig, int)
{
    if (!opusConfig) {
        log("Unsupported Moonlight audio configuration: missing Opus configuration");
        return -1;
    }

    std::ostringstream message;
    message << "Moonlight audio setup: " << opusConfig->sampleRate << " Hz, "
            << opusConfig->channelCount << " channels, " << opusConfig->samplesPerFrame
            << " samples/frame";
    log(message.str());

    if (opusConfig->sampleRate != 48000 || opusConfig->channelCount != 2
        || opusConfig->streams != 1 || opusConfig->coupledStreams != 1
        || opusConfig->samplesPerFrame <= 0) {
        log("Unsupported Moonlight audio configuration: expected 48000 Hz stereo Opus");
        audioConfigured_.store(false, std::memory_order_release);
        return -1;
    }

    audioSamplesPerFrame_.store(opusConfig->samplesPerFrame, std::memory_order_release);
    audioRtpTimestamp_.store(0, std::memory_order_release);
    audioConfigured_.store(true, std::memory_order_release);
    return 0;
}

void MoonlightMediaBridge::submitAudio(char* sampleData, int sampleLength)
{
    if (!audioConfigured_.load(std::memory_order_acquire)
        || !audioStarted_.load(std::memory_order_acquire) || !sampleData
        || sampleLength <= 0) {
        return;
    }

    const auto samplesPerFrame = audioSamplesPerFrame_.load(std::memory_order_acquire);
    const auto timestamp = audioRtpTimestamp_.fetch_add(
        static_cast<std::uint32_t>(samplesPerFrame), std::memory_order_acq_rel);
    sender_.sendOpusPacket(
        std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(sampleData),
                                      static_cast<std::size_t>(sampleLength)),
        timestamp);

    const auto packets = audioPackets_.fetch_add(1, std::memory_order_relaxed) + 1;
    const auto bytes = audioBytes_.fetch_add(static_cast<std::size_t>(sampleLength),
                                             std::memory_order_relaxed)
        + static_cast<std::size_t>(sampleLength);
    const auto now = std::chrono::steady_clock::now();
    bool report = false;
    {
        const std::lock_guard lock(diagnosticsMutex_);
        if (now >= nextAudioLog_) {
            nextAudioLog_ = now + std::chrono::seconds(1);
            report = true;
        }
    }

    if (report) {
        std::ostringstream message;
        message << "Moonlight audio: " << packets << " packets, " << bytes << " bytes";
        log(message.str());
    }
}

std::optional<std::vector<std::uint8_t>> MoonlightMediaBridge::flattenDecodeUnit(
    PDECODE_UNIT decodeUnit)
{
    if (auto lease = bufferPool_.flatten(decodeUnit)) {
        return std::vector<std::uint8_t>(lease->data().begin(), lease->data().end());
    }
    return std::nullopt;
}

void MoonlightMediaBridge::failHdrValidation(const std::string& message)
{
    if (hdrValidationFailed_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    log("Moonlight HDR validation failed: " + message);
    if (validationFailureHandler_) {
        validationFailureHandler_(message);
    }
}

void MoonlightMediaBridge::log(const std::string& message)
{
    const std::lock_guard lock(logMutex_);
    if (logger_) {
        logger_(message);
    }
}

} // namespace gateway
