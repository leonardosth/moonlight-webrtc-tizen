#include "webrtc/PacketPacer.h"
#include "webrtc/WebRtcMediaSender.h"

#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <rtc/rtc.hpp>

namespace {

void require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

} // namespace

int main()
{
    try {
        auto h264Configuration = std::make_shared<rtc::RtpPacketizationConfig>(
            42, "test", 96, 90000);
        auto h264Packetizer = gateway::makeVideoRtpPacketizer(
            gateway::VideoCodec::H264, h264Configuration);
        require(std::dynamic_pointer_cast<rtc::H264RtpPacketizer>(h264Packetizer)
                    != nullptr,
                "H.264 did not select rtc::H264RtpPacketizer");
        require(std::dynamic_pointer_cast<rtc::H265RtpPacketizer>(h264Packetizer)
                    == nullptr,
                "H.264 selected the H.265 packetizer");

        auto hevcConfiguration = std::make_shared<rtc::RtpPacketizationConfig>(
            43, "test", 96, 90000);
        auto hevcPacketizer = gateway::makeVideoRtpPacketizer(
            gateway::VideoCodec::HEVC, hevcConfiguration);
        require(std::dynamic_pointer_cast<rtc::H265RtpPacketizer>(hevcPacketizer)
                    != nullptr,
                "HEVC did not select rtc::H265RtpPacketizer");
        require(std::dynamic_pointer_cast<rtc::H264RtpPacketizer>(hevcPacketizer)
                    == nullptr,
                "HEVC selected the H.264 packetizer");

        auto av1Configuration = std::make_shared<rtc::RtpPacketizationConfig>(
            44, "test", 96, 90000);
        auto av1Packetizer = gateway::makeVideoRtpPacketizer(
            gateway::VideoCodec::AV1, av1Configuration);
        require(std::dynamic_pointer_cast<rtc::AV1RtpPacketizer>(av1Packetizer)
                    != nullptr,
                "AV1 did not select rtc::AV1RtpPacketizer");
        require(std::dynamic_pointer_cast<rtc::H264RtpPacketizer>(av1Packetizer)
                    == nullptr
                    && std::dynamic_pointer_cast<rtc::H265RtpPacketizer>(av1Packetizer)
                        == nullptr,
                "AV1 selected H.264 or HEVC packetizer");
        require(h264Configuration->clockRate == 90000
                    && hevcConfiguration->clockRate == 90000,
                "Video RTP clock is not 90000 Hz");

        auto customMtuPacketizer = gateway::makeVideoRtpPacketizer(
            gateway::VideoCodec::AV1, av1Configuration, 1360);
        require(customMtuPacketizer != nullptr,
                "Custom fragment size AV1 packetizer creation failed");
        auto defaultMtuPacketizer = gateway::makeVideoRtpPacketizer(
            gateway::VideoCodec::H264, h264Configuration);
        require(defaultMtuPacketizer != nullptr,
                "Default fragment size H.264 packetizer creation failed");
        require(hevcConfiguration->colorSpaceId == 0,
                "RTP color-space extension was enabled before negotiation");
        gateway::configureHdrRtpColorSpace(hevcConfiguration, 9);
        require(hevcConfiguration->colorSpaceId == 9
                    && hevcConfiguration->colorRange == 1
                    && hevcConfiguration->colorPrimaries == 9
                    && hevcConfiguration->colorTransfer == 16
                    && hevcConfiguration->colorMatrix == 9
                    && hevcConfiguration->colorChromaSitingHorz == 0
                    && hevcConfiguration->colorChromaSitingVert == 0,
                "Negotiated BT.2020/PQ limited-range RTP values are incorrect");
        gateway::disableRtpColorSpace(hevcConfiguration);
        require(hevcConfiguration->colorSpaceId == 0,
                "RTP color-space extension was not disabled");

        require(hevcConfiguration->playoutDelayId == 0,
                "RTP playout-delay extension was enabled before negotiation");
        gateway::configurePlayoutDelayRtp(hevcConfiguration, 10, 0, 0);
        require(hevcConfiguration->playoutDelayId == 10
                    && hevcConfiguration->playoutDelayMin == 0
                    && hevcConfiguration->playoutDelayMax == 0,
                "Negotiated RTP playout-delay values are incorrect");
        gateway::disablePlayoutDelayRtp(hevcConfiguration);
        require(hevcConfiguration->playoutDelayId == 0,
                "RTP playout-delay extension was not disabled");

        const auto lowestDelay = gateway::playoutDelayForLatencyMode(gateway::LatencyMode::LowestLatency);
        require(lowestDelay.first == 0 && lowestDelay.second == 0,
                "Lowest latency mode must set playout delay to min=0ms, max=0ms");
        const auto balancedDelay = gateway::playoutDelayForLatencyMode(gateway::LatencyMode::Balanced);
        require(balancedDelay.first == 0 && balancedDelay.second == 3,
                "Balanced latency mode must set playout delay to min=0ms, max=30ms");
        const auto smoothestDelay = gateway::playoutDelayForLatencyMode(gateway::LatencyMode::Smoothest);
        require(smoothestDelay.first == 1 && smoothestDelay.second == 5,
                "Smoothest latency mode must set playout delay to min=10ms, max=50ms");

        rtc::Description::Video h264Description(
            "video", rtc::Description::Direction::SendOnly);
        h264Description.addH264Codec(96);
        const std::string h264Sdp = h264Description.generateSdp();
        require(h264Sdp.find("a=rtpmap:96 H264/90000") != std::string::npos,
                "libdatachannel H.264 SDP description is incorrect");

        rtc::Description::Video hevcDescription(
            "video", rtc::Description::Direction::SendOnly);
        hevcDescription.addH265Codec(96);
        hevcDescription.addExtMap(rtc::Description::Entry::ExtMap(
            gateway::PreferredWebRtcColorSpaceExtensionId,
            std::string(gateway::WebRtcColorSpaceExtensionUri)));
        hevcDescription.addExtMap(rtc::Description::Entry::ExtMap(
            gateway::PreferredWebRtcPlayoutDelayExtensionId,
            std::string(gateway::WebRtcPlayoutDelayExtensionUri)));
        const std::string hevcSdp = hevcDescription.generateSdp();
        require(hevcSdp.find("a=rtpmap:96 H265/90000") != std::string::npos
                    && hevcSdp.find("a=fmtp:96") == std::string::npos
                    && hevcSdp.find("H264/90000") == std::string::npos
                    && hevcSdp.find(
                           "a=extmap:9 http://www.webrtc.org/experiments/rtp-hdrext/color-space")
                        != std::string::npos
                    && hevcSdp.find(
                           "a=extmap:10 http://www.webrtc.org/experiments/rtp-hdrext/playout-delay")
                        != std::string::npos,
                "libdatachannel HEVC SDP description is incorrect");

        rtc::Description::Video main10Description(
            "video", rtc::Description::Direction::SendOnly);
        main10Description.addH265Codec(
            96, "profile-id=2;tier-flag=0;level-id=123");
        const std::string main10Sdp = main10Description.generateSdp();
        require(main10Sdp.find(
                    "a=fmtp:96 profile-id=2;tier-flag=0;level-id=123")
                    != std::string::npos,
                "libdatachannel did not generate the HEVC Main10 fmtp line");

        // Test PacingMediaHandler Disabled mode: passes messages through
        {
            gateway::PacingMediaHandler disabledPacer(gateway::PacketPacing::Disabled);
            rtc::message_vector msgs;
            for (int i = 0; i < 8; ++i) {
                msgs.push_back(rtc::make_message(100));
            }
            int sendCount = 0;
            disabledPacer.outgoing(msgs, [&](rtc::message_ptr) { sendCount++; });
            require(msgs.size() == 8, "Disabled pacer must leave outgoing messages vector intact");
            require(sendCount == 0, "Disabled pacer must not dispatch through callback");
        }

        // Test PacingMediaHandler Balanced mode: absorbs into queue and paces
        {
            gateway::PacingMediaHandler balancedPacer(gateway::PacketPacing::Balanced, 60, 50000);
            rtc::message_vector msgs;
            for (int i = 0; i < 10; ++i) {
                msgs.push_back(rtc::make_message(200));
            }
            std::atomic<int> sentCount = 0;
            balancedPacer.outgoing(msgs, [&](rtc::message_ptr) { sentCount.fetch_add(1); });
            require(msgs.empty(), "Balanced pacer must consume outgoing messages vector");
            require(sentCount.load() >= 4, "Balanced pacer must dispatch initial burst immediately");

            std::this_thread::sleep_for(std::chrono::milliseconds(80));
            require(sentCount.load() == 10, "Balanced pacer must dispatch all packets within frame budget");

            const auto stats = balancedPacer.stats();
            require(stats.totalBursts == 1, "PacingStats totalBursts incorrect");
            require(stats.totalPacketsImmediate == 4, "PacingStats totalPacketsImmediate incorrect");
            require(stats.totalPacketsPaced == 6, "PacingStats totalPacketsPaced incorrect");
            require(stats.maxBurstPackets == 10, "PacingStats maxBurstPackets incorrect");
            balancedPacer.stop();
        }

        std::cout << "WebRTC media sender tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "WebRTC media sender test failed: " << error.what() << '\n';
        return 1;
    }
}
