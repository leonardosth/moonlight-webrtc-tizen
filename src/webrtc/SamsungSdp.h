#pragma once

#include "session/StreamSettings.h"

#include <string>
#include <string_view>
#include <optional>

namespace gateway {

std::string samsungGameModeImageAttribute(const StreamSettings& settings,
                                          int payloadType = 96);
std::string samsungGameModeSdpLine(const StreamSettings& settings,
                                   int payloadType = 96);
bool hasValidSamsungGameModeImageAttribute(std::string_view sdp,
                                           const StreamSettings& settings,
                                           int payloadType = 96);
bool hasExpectedVideoCodec(std::string_view sdp,
                           VideoCodec codec,
                           int payloadType = 96);
std::optional<std::string> hevcFormatParameters(const StreamSettings& settings);
bool hasExpectedHevcFormatParameters(std::string_view sdp,
                                     const StreamSettings& settings,
                                     int payloadType = 96);
std::optional<std::string> av1FormatParameters(const StreamSettings& settings);
bool hasExpectedAv1FormatParameters(std::string_view sdp,
                                    const StreamSettings& settings,
                                    int payloadType = 96);
std::optional<int> hevcLevelId(std::string_view sdp, int payloadType = 96);
std::optional<int> videoExtensionId(std::string_view sdp, std::string_view uri);
std::optional<int> videoBandwidthKbps(std::string_view sdp);
bool hasExpectedVideoBandwidth(std::string_view sdp, int expectedBitrateKbps);
bool hasRedAndUlpfecCodecs(std::string_view sdp, int redPt = 97, int ulpfecPt = 98);

} // namespace gateway
