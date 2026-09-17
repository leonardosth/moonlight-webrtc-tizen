#pragma once

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace gateway {

enum class VideoCodec {
    H264,
    HEVC,
    AV1,
};

struct VideoMode {
    int width;
    int height;
    int fps;
    bool supportsH264;
    bool supportsHevc;
    bool supportsAv1;
    VideoCodec defaultCodec;
    int defaultBitrateKbps;
    bool experimental;
    bool supportsHdr;
};

struct StreamSettings {
    int width = 1280;
    int height = 720;
    int fps = 60;
    int bitrateKbps = 12000;
    VideoCodec codec = VideoCodec::H264;
    bool hdr = false;
    int audioChannels = 2;

    bool operator==(const StreamSettings&) const = default;
};

inline constexpr std::array SupportedVideoModes{
    // 60 fps modes
    VideoMode{1280,  720, 60, true,  true,  true,  VideoCodec::H264, 12000, false, false},
    VideoMode{1920, 1080, 60, true,  true,  true,  VideoCodec::H264, 20000, false, true},
    VideoMode{2560, 1440, 60, true,  true,  true,  VideoCodec::HEVC, 30000, true,  true},
    VideoMode{3840, 2160, 60, false, true,  true,  VideoCodec::HEVC, 50000, false, true},
    // 120 fps modes (experimental)
    VideoMode{1280,  720, 120, true,  true,  true,  VideoCodec::H264, 20000, true, false},
    VideoMode{1920, 1080, 120, true,  true,  true,  VideoCodec::HEVC, 40000, true, true},
    VideoMode{2560, 1440, 120, true,  true,  true,  VideoCodec::HEVC, 50000, true, true},
    VideoMode{3840, 2160, 120, false, true,  true,  VideoCodec::HEVC, 70000, true, true},
};

inline constexpr std::array SupportedBitratesKbps{
    10000,
    12000,
    15000,
    20000,
    25000,
    30000,
    40000,
    50000,
    60000,
    70000,
    80000,
    90000,
    100000,
};

const VideoMode* findVideoMode(int width, int height, int fps = 60);
bool videoModeSupportsCodec(const VideoMode& mode, VideoCodec codec);
bool videoModeSupportsHdr(const VideoMode& mode, VideoCodec codec);
std::span<const VideoCodec> supportedVideoCodecs();
StreamSettings defaultStreamSettings(
    int width = 1280,
    int height = 720,
    std::optional<VideoCodec> codec = std::nullopt);
std::optional<std::string> validateStreamSettings(const StreamSettings& settings);
std::string_view videoCodecName(VideoCodec codec);
std::string_view videoCodecDisplayName(VideoCodec codec);
std::optional<VideoCodec> parseVideoCodec(std::string_view name);

} // namespace gateway
