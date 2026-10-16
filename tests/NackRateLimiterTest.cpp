#include "webrtc/NackRateLimiter.h"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

void require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

// Helper to construct a minimal dummy RTP packet with given sequence number
rtc::message_ptr makeRtpPacket(std::uint16_t seq, std::size_t payloadSize = 100)
{
    auto msg = rtc::make_message(12 + payloadSize);
    auto* data = reinterpret_cast<std::uint8_t*>(msg->data());
    data[0] = 0x80; // V=2
    data[1] = 96;   // PT=96
    data[2] = static_cast<std::uint8_t>(seq >> 8);
    data[3] = static_cast<std::uint8_t>(seq & 0xFF);
    return msg;
}

// Helper to construct an RTCP NACK packet (PT=205, FMT=1) requesting seq
rtc::message_ptr makeRtcpNackPacket(std::uint16_t seq, std::uint16_t blp = 0)
{
    // RTCP Header: 4 bytes (V=2, FMT=1, PT=205, Length=3 in 32-bit words - 1 = 2)
    // SSRC sender: 4 bytes
    // SSRC media: 4 bytes
    // FCI: 4 bytes (PID 2 bytes, BLP 2 bytes)
    // Total size = 16 bytes
    auto msg = rtc::make_message(16);
    msg->type = rtc::Message::Control;
    auto* data = reinterpret_cast<std::uint8_t*>(msg->data());
    data[0] = 0x81; // V=2, P=0, FMT=1
    data[1] = 205;  // PT=205 (RTPFB)
    data[2] = 0x00; // length in 32-bit words minus 1 = 3 (16 bytes)
    data[3] = 0x03;
    // SSRC sender (bytes 4-7)
    data[4] = 0; data[5] = 0; data[6] = 0; data[7] = 1;
    // SSRC media (bytes 8-11)
    data[8] = 0; data[9] = 0; data[10] = 0; data[11] = 42;
    // FCI: PID (bytes 12-13)
    data[12] = static_cast<std::uint8_t>(seq >> 8);
    data[13] = static_cast<std::uint8_t>(seq & 0xFF);
    // FCI: BLP (bytes 14-15)
    data[14] = static_cast<std::uint8_t>(blp >> 8);
    data[15] = static_cast<std::uint8_t>(blp & 0xFF);
    return msg;
}

} // namespace

int main()
{
    try {
        std::cout << "Starting NACK rate limiter tests...\n";

        // Test 1: Store packets and retransmit on valid NACK
        {
            gateway::NackRateLimitingHandler limiter(1024, 25 /* cooldownMs */, 100 /* maxRetransmitsPerSec */);

            // Send RTP packets 100, 101, 102 through outgoing
            rtc::message_vector outgoingMsgs;
            outgoingMsgs.push_back(makeRtpPacket(100));
            outgoingMsgs.push_back(makeRtpPacket(101));
            outgoingMsgs.push_back(makeRtpPacket(102));

            limiter.outgoing(outgoingMsgs, [](rtc::message_ptr) {});

            // Simulate incoming RTCP NACK for seq 101
            rtc::message_vector incomingMsgs;
            incomingMsgs.push_back(makeRtcpNackPacket(101));

            int retransmittedCount = 0;
            std::uint16_t lastRetransmittedSeq = 0;
            limiter.incoming(incomingMsgs, [&](rtc::message_ptr pkt) {
                retransmittedCount++;
                if (pkt && pkt->size() >= 4) {
                    const auto* d = reinterpret_cast<const std::uint8_t*>(pkt->data());
                    lastRetransmittedSeq = (static_cast<std::uint16_t>(d[2]) << 8) | d[3];
                }
            });

            require(retransmittedCount == 1, "Expected exactly 1 retransmission for seq 101");
            require(lastRetransmittedSeq == 101, "Retransmitted packet must match requested seq 101");

            const auto stats = limiter.stats();
            require(stats.retransmittedPackets == 1, "Stats retransmittedPackets should be 1");
            require(stats.duplicateNacksSuppressed == 0, "No duplicate NACK should be suppressed yet");
        }

        // Test 2: Suppress duplicate NACK within cooldown window
        {
            gateway::NackRateLimitingHandler limiter(1024, 50 /* cooldownMs */, 100);

            rtc::message_vector outgoingMsgs;
            outgoingMsgs.push_back(makeRtpPacket(200));
            limiter.outgoing(outgoingMsgs, [](rtc::message_ptr) {});

            // First NACK for 200
            rtc::message_vector nack1;
            nack1.push_back(makeRtcpNackPacket(200));
            int sendCount = 0;
            limiter.incoming(nack1, [&](rtc::message_ptr) { sendCount++; });
            require(sendCount == 1, "First NACK must trigger retransmission");

            // Immediate duplicate NACK for 200 within cooldown window
            rtc::message_vector nack2;
            nack2.push_back(makeRtcpNackPacket(200));
            limiter.incoming(nack2, [&](rtc::message_ptr) { sendCount++; });
            require(sendCount == 1, "Duplicate NACK within cooldown window must be suppressed!");

            const auto stats = limiter.stats();
            require(stats.duplicateNacksSuppressed == 1, "duplicateNacksSuppressed must be incremented");

            // Wait for cooldown to expire (60ms > 50ms)
            std::this_thread::sleep_for(std::chrono::milliseconds(60));

            // Third NACK for 200 after cooldown should be allowed
            rtc::message_vector nack3;
            nack3.push_back(makeRtcpNackPacket(200));
            limiter.incoming(nack3, [&](rtc::message_ptr) { sendCount++; });
            require(sendCount == 2, "NACK after cooldown must trigger fresh retransmission");
        }

        // Test 3: Rate limiting / burst capping
        {
            // Limit to at most 10 retransmissions per burst window
            gateway::NackRateLimitingHandler limiter(1024, 25, 10 /* maxBurstRetransmits */);

            rtc::message_vector outgoingMsgs;
            for (std::uint16_t s = 1000; s < 1030; ++s) {
                outgoingMsgs.push_back(makeRtpPacket(s));
            }
            limiter.outgoing(outgoingMsgs, [](rtc::message_ptr) {});

            // Request 25 packets in NACKs
            int sentRetransmits = 0;
            for (std::uint16_t s = 1000; s < 1025; ++s) {
                rtc::message_vector nack;
                nack.push_back(makeRtcpNackPacket(s));
                limiter.incoming(nack, [&](rtc::message_ptr) { sentRetransmits++; });
            }

            // Must cap retransmissions to prevent storm flooding
            require(sentRetransmits <= 10, "Retransmissions must be capped by rate limiter");
            const auto stats = limiter.stats();
            require(stats.rateLimitedNacks > 0, "rateLimitedNacks counter must record throttled requests");
        }

        std::cout << "All NACK rate limiter tests passed!\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "NACK rate limiter test failed: " << error.what() << '\n';
        return 1;
    }
}
