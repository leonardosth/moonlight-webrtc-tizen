#pragma once

#include "media/MediaSender.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string_view>
#include <vector>

namespace rtc {
class RtpPacketizationConfig;
class RtpPacketizer;
class Track;
}

namespace gateway {

inline constexpr std::string_view WebRtcColorSpaceExtensionUri =
    "http://www.webrtc.org/experiments/rtp-hdrext/color-space";
inline constexpr int PreferredWebRtcColorSpaceExtensionId = 9;

inline constexpr std::string_view WebRtcPlayoutDelayExtensionUri =
    "http://www.webrtc.org/experiments/rtp-hdrext/playout-delay";
inline constexpr int PreferredWebRtcPlayoutDelayExtensionId = 10;

class WebRtcMediaSender final : public MediaSender {
public:
    WebRtcMediaSender(std::shared_ptr<rtc::Track> videoTrack,
                      std::shared_ptr<rtc::Track> audioTrack,
                      VideoCodec videoCodec);

    void sendVideoAccessUnit(VideoCodec codec,
                             std::span<const std::uint8_t> accessUnit,
                             std::uint32_t rtpTimestamp) override;
    void sendOpusPacket(std::span<const std::uint8_t> packet,
                        std::uint32_t rtpTimestamp) override;

private:
    std::shared_ptr<rtc::Track> videoTrack_;
    std::shared_ptr<rtc::Track> audioTrack_;
    VideoCodec videoCodec_;
    std::vector<std::uint8_t> av1Buffer_;
    std::mutex av1Mutex_;
};

inline constexpr std::size_t DefaultVideoRtpMaxFragmentSize = 1360;

std::shared_ptr<rtc::RtpPacketizer> makeVideoRtpPacketizer(
    VideoCodec codec,
    const std::shared_ptr<rtc::RtpPacketizationConfig>& rtpConfiguration,
    std::size_t maxFragmentSize = DefaultVideoRtpMaxFragmentSize);
void configureHdrRtpColorSpace(
    const std::shared_ptr<rtc::RtpPacketizationConfig>& rtpConfiguration,
    int negotiatedExtensionId);
void disableRtpColorSpace(
    const std::shared_ptr<rtc::RtpPacketizationConfig>& rtpConfiguration);
void configurePlayoutDelayRtp(
    const std::shared_ptr<rtc::RtpPacketizationConfig>& rtpConfiguration,
    int negotiatedExtensionId,
    std::uint16_t minDelay10ms = 0,
    std::uint16_t maxDelay10ms = 0);
void disablePlayoutDelayRtp(
    const std::shared_ptr<rtc::RtpPacketizationConfig>& rtpConfiguration);

} // namespace gateway
