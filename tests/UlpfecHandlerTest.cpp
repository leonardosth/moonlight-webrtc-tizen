#include "webrtc/UlpfecHandler.h"

#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <rtc/rtc.hpp>

namespace {

void require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

rtc::message_ptr makeRtpPacket(std::uint16_t seq,
                               std::uint32_t timestamp,
                               std::uint8_t payloadType,
                               bool marker,
                               const std::vector<std::uint8_t>& payloadData)
{
    rtc::binary data(12 + payloadData.size());
    auto* rtp = reinterpret_cast<rtc::RtpHeader*>(data.data());
    rtp->preparePacket();
    rtp->setSeqNumber(seq);
    rtp->setTimestamp(timestamp);
    rtp->setPayloadType(payloadType);
    rtp->setMarker(marker);
    rtp->setSsrc(0x12345678);

    if (!payloadData.empty()) {
        std::memcpy(data.data() + 12, payloadData.data(), payloadData.size());
    }

    return rtc::make_message(std::move(data));
}

} // namespace

int main()
{
    try {
        // Test 1: Disabled (0% FEC) - packets pass through untouched without RED wrapping
        {
            gateway::UlpfecHandler handler(0, 96, 97, 98);
            require(handler.fecPercentage() == 0, "FEC percentage must be 0");

            rtc::message_vector messages;
            std::vector<std::uint8_t> payload = {1, 2, 3, 4, 5};
            messages.push_back(makeRtpPacket(100, 1000, 96, false, payload));

            handler.outgoing(messages, [](rtc::message_ptr) {});

            require(messages.size() == 1, "Pass-through must preserve exactly 1 packet");
            const auto* rtp = reinterpret_cast<const rtc::RtpHeader*>(messages[0]->data());
            require(rtp->payloadType() == 96, "PT must remain primary 96 when FEC is disabled");
            require(messages[0]->size() == 12 + payload.size(), "Packet size must not have RED overhead");
            require(handler.stats().fecPacketsGenerated == 0, "No FEC packets generated");
        }

        // Test 2: 10% FEC (Block size = 10 packets) - generates 1 ULPFEC packet per 10 media packets
        {
            auto config = std::make_shared<rtc::RtpPacketizationConfig>(
                0x12345678, "video", 96, 90000);
            config->sequenceNumber = 500;

            gateway::UlpfecHandler handler(10, 96, 97, 98, config);
            require(handler.fecPercentage() == 10, "FEC percentage must be 10");

            rtc::message_vector accumulatedSent;

            // Feed 9 packets: all should be RED-wrapped, but no FEC packet yet
            for (std::uint16_t i = 0; i < 9; ++i) {
                rtc::message_vector batch;
                std::vector<std::uint8_t> payload = {static_cast<std::uint8_t>(i), 0xAA, 0xBB};
                batch.push_back(makeRtpPacket(100 + i, 1000 + i * 1500, 96, false, payload));
                handler.outgoing(batch, [](rtc::message_ptr) {});
                for (auto& msg : batch) accumulatedSent.push_back(std::move(msg));
            }

            require(accumulatedSent.size() == 9, "9 media packets emitted before block completion");
            require(handler.stats().fecPacketsGenerated == 0, "No FEC packet before block completes");

            // Feed 10th packet: completes the block -> must emit 10th media packet + 1 FEC packet (total 11)
            {
                rtc::message_vector batch;
                std::vector<std::uint8_t> payload = {9, 0xAA, 0xBB, 0xCC, 0xDD};
                batch.push_back(makeRtpPacket(109, 1000 + 9 * 1500, 96, true, payload));
                handler.outgoing(batch, [](rtc::message_ptr) {});
                for (auto& msg : batch) accumulatedSent.push_back(std::move(msg));
            }

            require(accumulatedSent.size() == 11, "10 media packets + 1 FEC packet must equal 11 emitted packets");
            require(handler.stats().mediaPacketsProtected == 10, "10 packets protected");
            require(handler.stats().fecPacketsGenerated == 1, "1 FEC packet generated");
            require(handler.stats().fecBytesGenerated > 0, "FEC bytes recorded");

            // Verify the 11th packet is indeed a RED-wrapped ULPFEC packet
            const auto& fecPkt = accumulatedSent.back();
            const auto* fecRtp = reinterpret_cast<const rtc::RtpHeader*>(fecPkt->data());
            require(fecRtp->payloadType() == 97, "FEC packet RTP header must use RED payload type 97");
            require(fecPkt->size() >= 12 + 1 + 12, "FEC packet must include RTP, RED, and Level 0 FEC header");

            // RED header (byte 12)
            const auto redPt = static_cast<std::uint8_t>(fecPkt->data()[12]) & 0x7F;
            require(redPt == 98, "RED header inside FEC packet must indicate ULPFEC payload type 98");
        }

        // Test 3: Mathematical Packet Reconstruction via RFC 5109 XOR
        {
            gateway::UlpfecHandler handler(20, 96, 97, 98); // 20% -> block size = 5

            std::vector<rtc::binary> originalMediaPackets;
            std::vector<rtc::binary> receivedProtectedPackets;
            rtc::binary emittedFecPacket;

            for (std::uint16_t i = 0; i < 5; ++i) {
                rtc::message_vector batch;
                std::vector<std::uint8_t> payload = {
                    static_cast<std::uint8_t>(0x10 * (i + 1)),
                    static_cast<std::uint8_t>(0x20 * (i + 1)),
                    static_cast<std::uint8_t>(0x30 * (i + 1))
                };
                if (i == 2) {
                    payload.push_back(0x99); // Variable length packet in block
                }
                auto pkt = makeRtpPacket(200 + i, 50000 + i * 3000, 96, (i == 4), payload);
                originalMediaPackets.push_back(*pkt);
                batch.push_back(pkt);
                handler.outgoing(batch, [](rtc::message_ptr) {});

                for (auto& outPkt : batch) {
                    if (receivedProtectedPackets.size() < 5) {
                        receivedProtectedPackets.push_back(*outPkt);
                    } else {
                        emittedFecPacket = *outPkt;
                    }
                }
            }

            require(receivedProtectedPackets.size() == 5, "5 media packets expected");
            require(!emittedFecPacket.empty(), "FEC packet must be captured");

            // Now simulate loss of packet with seq = 202 (i = 2, the variable-length packet)
            std::vector<rtc::binary> survivingPackets;
            for (size_t i = 0; i < receivedProtectedPackets.size(); ++i) {
                if (i != 2) {
                    survivingPackets.push_back(receivedProtectedPackets[i]);
                }
            }
            require(survivingPackets.size() == 4, "4 surviving packets");

            // Attempt reconstruction of missingSeq = 202
            auto reconstructedOpt = gateway::UlpfecHandler::reconstructPacket(
                emittedFecPacket, survivingPackets, 202, 96, 97, 98);
            require(reconstructedOpt.has_value(), "Reconstruction of missing packet 202 must succeed");

            const auto& reconstructed = *reconstructedOpt;
            const auto& expectedOriginal = originalMediaPackets[2];

            require(reconstructed.size() == expectedOriginal.size(),
                    "Reconstructed packet size must match original packet size");
            require(std::memcmp(reconstructed.data(), expectedOriginal.data(), reconstructed.size()) == 0,
                    "Reconstructed packet bytes must bit-for-bit match original dropped packet");
        }

        // Test 4: Edge Cases - Empty messages, malformed packets, and sequence wrap
        {
            gateway::UlpfecHandler handler(10, 96, 97, 98);
            rtc::message_vector empty;
            handler.outgoing(empty, [](rtc::message_ptr) {
                throw std::runtime_error("Send should not be called for empty batch");
            });
            require(empty.empty(), "Empty messages remain empty");

            // Malformed packet (smaller than 12-byte RTP header) should be passed through safely
            rtc::message_vector malformed;
            rtc::binary shortData = {std::byte{1}, std::byte{2}, std::byte{3}};
            malformed.push_back(rtc::make_message(std::move(shortData)));
            handler.outgoing(malformed, [](rtc::message_ptr) {});
            require(malformed.size() == 1 && malformed[0]->size() == 3, "Malformed packet must pass through safely");
        }

        std::cout << "All UlpfecHandler tests passed successfully!" << std::endl;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "UlpfecHandlerTest failed: " << error.what() << std::endl;
        return 1;
    }
}
