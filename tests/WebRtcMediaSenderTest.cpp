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
            std::this_thread::sleep_for(std::chrono::milliseconds(80));
            require(sentCount.load() == 10, "Balanced pacer must dispatch all packets within frame budget");

            const auto stats = balancedPacer.stats();
            require(stats.totalBursts == 1, "PacingStats totalBursts incorrect");
            require(stats.totalPacketsPaced == 10, "PacingStats totalPacketsPaced incorrect");
            require(stats.maxBurstPackets == 10, "PacingStats maxBurstPackets incorrect");
            balancedPacer.stop();
        }

        // Test PacingMediaHandler Fast mode: 8 immediate packets, 4.0x multiplier
        {
            gateway::PacingMediaHandler fastPacer(gateway::PacketPacing::Fast, 60, 50000);
            rtc::message_vector msgs;
            for (int i = 0; i < 12; ++i) {
                msgs.push_back(rtc::make_message(200));
            }
            std::atomic<int> sentCount = 0;
            fastPacer.outgoing(msgs, [&](rtc::message_ptr) { sentCount.fetch_add(1); });
            require(msgs.empty(), "Fast pacer must consume outgoing messages vector");
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            require(sentCount.load() == 12, "Fast pacer must dispatch all packets quickly");

            const auto stats = fastPacer.stats();
            require(stats.totalBursts == 1, "Fast PacingStats totalBursts incorrect");
            require(stats.totalPacketsPaced == 12, "Fast PacingStats totalPacketsPaced incorrect");
            require(stats.maxBurstPackets == 12, "Fast PacingStats maxBurstPackets incorrect");
            fastPacer.stop();
        }

        // Test PacingMediaHandler Non-blocking Asynchronous Dispatch and Smooth Catch-up
        {
            // Create a pacer with 50 Mbps target and 60 FPS
            gateway::PacingMediaHandler pacer(gateway::PacketPacing::Balanced, 60, 50000);
            rtc::message_vector msgs;
            constexpr int LargeBurstSize = 200; // Exceeds 160 catch-up threshold
            for (int i = 0; i < LargeBurstSize; ++i) {
                msgs.push_back(rtc::make_message(1000));
            }

            std::atomic<int> immediateCallbackCalls = 0;
            std::atomic<int> totalSent = 0;
            const auto startTime = std::chrono::steady_clock::now();

            // outgoing must return immediately without blocking the caller thread
            pacer.outgoing(msgs, [&](rtc::message_ptr) {
                totalSent.fetch_add(1);
            });

            // Immediately after outgoing returns, caller must not have been synchronously blocked for all 200 packets
            require(msgs.empty(), "Outgoing messages must be consumed by pacer");
            require(totalSent.load() < LargeBurstSize, "Pacer must not synchronously flush large burst on caller thread");

            // Wait for pacer to drain the burst
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            require(totalSent.load() == LargeBurstSize, "Pacer must deliver all packets in the burst");

            const auto stats = pacer.stats();
            require(stats.totalBursts == 1, "Burst count must be 1");
            require(stats.maxBurstPackets == LargeBurstSize, "Max burst packets must record the full burst");
            require(stats.catchUpEvents >= 1, "Catch-up must be recorded when queue exceeds threshold");
            require(stats.maxQueueDepth >= LargeBurstSize - 16, "Max queue depth must record backlog");

            // Ensure catch-up still paced packets rather than dumping 200KB instantaneously in < 1ms
            require(pacer.isCatchUpPaced(), "Catch-up mode must maintain pacing delay rather than 0 delay");
            pacer.stop();
        }

        // Test PacingMediaHandler Fast Ethernet (100 Mbps) Safety Cap
        {
            // At 80 Mbps target with Fast pacing (4.0x multiplier = 320 Mbps)
            // Ethernet safety cap must clamp maximum burst rate to <= 95 Mbps
            gateway::PacingMediaHandler cappedPacer(gateway::PacketPacing::Fast, 60, 80000, nullptr, 95000);
            constexpr std::uint64_t MaxExpected95MbpsBps = (95000ULL * 1000ULL) / 8ULL;
            require(cappedPacer.bytesPerSec() <= MaxExpected95MbpsBps,
                    "Pacer bytesPerSec must be clamped to Ethernet line rate safety cap (95 Mbps)");
            require(cappedPacer.catchUpBytesPerSec() <= MaxExpected95MbpsBps,
                    "Pacer catchUpBytesPerSec must be clamped to Ethernet line rate safety cap (95 Mbps)");
            cappedPacer.stop();

            // When maxBandwidthKbps is not provided (0), pacer is unthrottled for Wi-Fi 5GHz / Gigabit
            gateway::PacingMediaHandler uncappedPacer(gateway::PacketPacing::Fast, 60, 80000, nullptr, 0);
            require(uncappedPacer.bytesPerSec() > MaxExpected95MbpsBps,
                    "Pacer must not be capped when maxBandwidthKbps is 0");
            uncappedPacer.stop();
        }

        std::cout << "WebRTC media sender tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "WebRTC media sender test failed: " << error.what() << '\n';
        return 1;
    }
}
