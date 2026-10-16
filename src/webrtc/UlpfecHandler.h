#pragma once

#include <rtc/rtc.hpp>

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace gateway {

struct UlpfecStats {
    std::uint64_t mediaPacketsProtected = 0;
    std::uint64_t fecPacketsGenerated = 0;
    std::uint64_t fecBytesGenerated = 0;
};

class UlpfecHandler : public rtc::MediaHandler {
public:
    explicit UlpfecHandler(int fecPercentage = 0,
                           int primaryPayloadType = 96,
                           int redPayloadType = 97,
                           int ulpfecPayloadType = 98,
                           std::shared_ptr<rtc::RtpPacketizationConfig> rtpConfig = nullptr);
    ~UlpfecHandler() override = default;

    void outgoing(rtc::message_vector& messages, const rtc::message_callback& send) override;

    [[nodiscard]] int fecPercentage() const noexcept;
    [[nodiscard]] UlpfecStats stats() const;

    // Helper for testing packet reconstruction:
    // Given the FEC packet and received packets, reconstructs the missing packet with sequence number missingSeq.
    static std::optional<rtc::binary> reconstructPacket(
        const rtc::binary& fecPacket,
        const std::vector<rtc::binary>& receivedPackets,
        std::uint16_t missingSeq,
        int primaryPayloadType = 96,
        int redPayloadType = 97,
        int ulpfecPayloadType = 98);

private:
    struct ProtectedPacketInfo {
        std::uint16_t seqNumber;
        std::uint32_t timestamp;
        bool marker;
        std::uint8_t payloadType;
        std::vector<std::uint8_t> payload;
    };

    int fecPercentage_;
    int primaryPayloadType_;
    int redPayloadType_;
    int ulpfecPayloadType_;
    std::shared_ptr<rtc::RtpPacketizationConfig> rtpConfig_;

    int blockSize_;
    std::vector<ProtectedPacketInfo> currentBlock_;
    std::uint16_t fallbackSeqNumber_ = 1000;

    mutable std::mutex statsMutex_;
    UlpfecStats stats_;

    rtc::message_ptr generateFecPacket(const std::vector<ProtectedPacketInfo>& block,
                                       std::uint32_t ssrc);
};

} // namespace gateway
