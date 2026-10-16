#include "webrtc/UlpfecHandler.h"

#include <algorithm>
#include <cstring>

namespace gateway {

UlpfecHandler::UlpfecHandler(int fecPercentage,
                             int primaryPayloadType,
                             int redPayloadType,
                             int ulpfecPayloadType,
                             std::shared_ptr<rtc::RtpPacketizationConfig> rtpConfig)
    : fecPercentage_(std::clamp(fecPercentage, 0, 50))
    , primaryPayloadType_(primaryPayloadType)
    , redPayloadType_(redPayloadType)
    , ulpfecPayloadType_(ulpfecPayloadType)
    , rtpConfig_(std::move(rtpConfig))
{
    if (fecPercentage_ > 0) {
        // e.g. 5% -> 20 packets, 10% -> 10 packets, 20% -> 5 packets
        blockSize_ = std::clamp(100 / fecPercentage_, 2, 16);
    } else {
        blockSize_ = 0;
    }
    currentBlock_.reserve(blockSize_ > 0 ? blockSize_ : 16);
}

int UlpfecHandler::fecPercentage() const noexcept
{
    return fecPercentage_;
}

UlpfecStats UlpfecHandler::stats() const
{
    const std::lock_guard lock(statsMutex_);
    return stats_;
}

void UlpfecHandler::outgoing(rtc::message_vector& messages, const rtc::message_callback&)
{
    if (fecPercentage_ <= 0) {
        return;
    }

    rtc::message_vector outputMessages;
    outputMessages.reserve(messages.size() + 2);

    for (auto& msg : messages) {
        if (!msg || msg->size() < sizeof(rtc::RtpHeader)) {
            outputMessages.push_back(std::move(msg));
            continue;
        }

        const auto* rtp = reinterpret_cast<const rtc::RtpHeader*>(msg->data());
        const auto headerSize = rtp->getSize();
        if (msg->size() < headerSize) {
            outputMessages.push_back(std::move(msg));
            continue;
        }

        const auto payloadSize = msg->size() - headerSize;
        const auto* payloadPtr = reinterpret_cast<const std::uint8_t*>(rtp->getBody());

        ProtectedPacketInfo info{
            .seqNumber = rtp->seqNumber(),
            .timestamp = rtp->timestamp(),
            .marker = (rtp->marker() != 0),
            .payloadType = rtp->payloadType(),
            .payload = std::vector<std::uint8_t>(payloadPtr, payloadPtr + payloadSize),
        };

        const auto ssrc = rtp->ssrc();

        // 1. Wrap media packet into RED (RFC 2198):
        // Header (headerSize) + 1 byte RED header (primaryPayloadType) + payload (payloadSize)
        rtc::binary redMediaData(headerSize + 1 + payloadSize);
        std::memcpy(redMediaData.data(), msg->data(), headerSize);
        auto* redRtp = reinterpret_cast<rtc::RtpHeader*>(redMediaData.data());
        redRtp->setPayloadType(static_cast<std::uint8_t>(redPayloadType_));
        redMediaData[headerSize] = static_cast<std::byte>(primaryPayloadType_ & 0x7F);
        if (payloadSize > 0) {
            std::memcpy(redMediaData.data() + headerSize + 1, payloadPtr, payloadSize);
        }

        outputMessages.push_back(rtc::make_message(std::move(redMediaData)));

        // 2. Protect packet in current block
        currentBlock_.push_back(std::move(info));
        {
            const std::lock_guard lock(statsMutex_);
            stats_.mediaPacketsProtected++;
        }

        // 3. When block reaches blockSize_, generate FEC packet
        if (static_cast<int>(currentBlock_.size()) >= blockSize_) {
            auto fecPkt = generateFecPacket(currentBlock_, ssrc);
            if (fecPkt) {
                outputMessages.push_back(std::move(fecPkt));
            }
            currentBlock_.clear();
        }
    }

    messages = std::move(outputMessages);
}

rtc::message_ptr UlpfecHandler::generateFecPacket(
    const std::vector<ProtectedPacketInfo>& block,
    std::uint32_t ssrc)
{
    if (block.empty()) {
        return nullptr;
    }

    const auto snBase = block.front().seqNumber;
    std::uint16_t mask = 0;
    std::uint32_t tsRecovery = 0;
    std::uint16_t lengthRecovery = 0;
    std::uint8_t ptRecovery = 0;
    std::uint8_t mRecovery = 0;
    std::size_t maxPayloadLen = 0;

    for (const auto& pkt : block) {
        const int diff = static_cast<std::int16_t>(pkt.seqNumber - snBase);
        if (diff >= 0 && diff < 16) {
            mask |= static_cast<std::uint16_t>(1 << (15 - diff));
        }
        tsRecovery ^= pkt.timestamp;
        lengthRecovery ^= static_cast<std::uint16_t>(pkt.payload.size());
        ptRecovery ^= (pkt.payloadType & 0x7F);
        if (pkt.marker) {
            mRecovery ^= 1;
        }
        if (pkt.payload.size() > maxPayloadLen) {
            maxPayloadLen = pkt.payload.size();
        }
    }

    std::vector<std::uint8_t> fecPayload(maxPayloadLen, 0);
    for (const auto& pkt : block) {
        for (std::size_t i = 0; i < pkt.payload.size(); ++i) {
            fecPayload[i] ^= pkt.payload[i];
        }
    }

    // Level 0 FEC Header (10 bytes) + Level 1 Mask (2 bytes) = 12 bytes
    std::uint8_t fecHeader[12];
    fecHeader[0] = 0x00; // E=0, L=0, P=0, X=0, CC=0
    fecHeader[1] = static_cast<std::uint8_t>((mRecovery ? 0x80 : 0x00) | (ptRecovery & 0x7F));
    fecHeader[2] = static_cast<std::uint8_t>(snBase >> 8);
    fecHeader[3] = static_cast<std::uint8_t>(snBase & 0xFF);
    fecHeader[4] = static_cast<std::uint8_t>(tsRecovery >> 24);
    fecHeader[5] = static_cast<std::uint8_t>((tsRecovery >> 16) & 0xFF);
    fecHeader[6] = static_cast<std::uint8_t>((tsRecovery >> 8) & 0xFF);
    fecHeader[7] = static_cast<std::uint8_t>(tsRecovery & 0xFF);
    fecHeader[8] = static_cast<std::uint8_t>(lengthRecovery >> 8);
    fecHeader[9] = static_cast<std::uint8_t>(lengthRecovery & 0xFF);
    fecHeader[10] = static_cast<std::uint8_t>(mask >> 8);
    fecHeader[11] = static_cast<std::uint8_t>(mask & 0xFF);

    std::uint16_t fecSeq = 0;
    if (rtpConfig_) {
        fecSeq = rtpConfig_->sequenceNumber++;
    } else {
        fecSeq = fallbackSeqNumber_++;
    }

    constexpr std::size_t rtpHeaderSize = 12;
    constexpr std::size_t redHeaderSize = 1;
    constexpr std::size_t fecHeaderSize = 12;
    const std::size_t totalSize = rtpHeaderSize + redHeaderSize + fecHeaderSize + maxPayloadLen;

    rtc::binary fecData(totalSize);
    auto* fecRtp = reinterpret_cast<rtc::RtpHeader*>(fecData.data());
    fecRtp->preparePacket();
    fecRtp->setPayloadType(static_cast<std::uint8_t>(redPayloadType_));
    fecRtp->setSeqNumber(fecSeq);
    fecRtp->setTimestamp(block.back().timestamp);
    fecRtp->setSsrc(ssrc);
    fecRtp->setMarker(false);

    // RED header (1 byte)
    fecData[rtpHeaderSize] = static_cast<std::byte>(ulpfecPayloadType_ & 0x7F);

    // FEC header (12 bytes)
    std::memcpy(fecData.data() + rtpHeaderSize + redHeaderSize, fecHeader, fecHeaderSize);

    // FEC payload
    if (maxPayloadLen > 0) {
        std::memcpy(fecData.data() + rtpHeaderSize + redHeaderSize + fecHeaderSize,
                    fecPayload.data(), maxPayloadLen);
    }

    {
        const std::lock_guard lock(statsMutex_);
        stats_.fecPacketsGenerated++;
        stats_.fecBytesGenerated += totalSize;
    }

    return rtc::make_message(std::move(fecData));
}

std::optional<rtc::binary> UlpfecHandler::reconstructPacket(
    const rtc::binary& fecPacket,
    const std::vector<rtc::binary>& receivedPackets,
    std::uint16_t missingSeq,
    int primaryPayloadType,
    int redPayloadType,
    int ulpfecPayloadType)
{
    if (fecPacket.size() < 12 + 1 + 12) {
        return std::nullopt;
    }

    const auto* fecRtp = reinterpret_cast<const rtc::RtpHeader*>(fecPacket.data());
    const auto rtpHeaderSize = fecRtp->getSize();
    if (fecPacket.size() < rtpHeaderSize + 1 + 12) {
        return std::nullopt;
    }

    // Verify RED header is ulpfecPayloadType
    const auto redPt = static_cast<std::uint8_t>(fecPacket[rtpHeaderSize]) & 0x7F;
    if (redPt != (ulpfecPayloadType & 0x7F)) {
        return std::nullopt;
    }

    const auto* fecHeader = reinterpret_cast<const std::uint8_t*>(fecPacket.data() + rtpHeaderSize + 1);
    const bool mRecovery = (fecHeader[1] & 0x80) != 0;
    const std::uint8_t ptRecovery = fecHeader[1] & 0x7F;
    const std::uint16_t snBase = (static_cast<std::uint16_t>(fecHeader[2]) << 8) | fecHeader[3];
    const std::uint32_t tsRecovery = (static_cast<std::uint32_t>(fecHeader[4]) << 24)
                                   | (static_cast<std::uint32_t>(fecHeader[5]) << 16)
                                   | (static_cast<std::uint32_t>(fecHeader[6]) << 8)
                                   | static_cast<std::uint32_t>(fecHeader[7]);
    const std::uint16_t lengthRecovery = (static_cast<std::uint16_t>(fecHeader[8]) << 8) | fecHeader[9];
    const std::uint16_t mask = (static_cast<std::uint16_t>(fecHeader[10]) << 8) | fecHeader[11];

    const int missingDiff = static_cast<std::int16_t>(missingSeq - snBase);
    if (missingDiff < 0 || missingDiff >= 16) {
        return std::nullopt;
    }
    if ((mask & (1 << (15 - missingDiff))) == 0) {
        return std::nullopt;
    }

    const std::size_t fecPayloadSize = fecPacket.size() - (rtpHeaderSize + 1 + 12);
    const auto* fecPayloadPtr = reinterpret_cast<const std::uint8_t*>(fecPacket.data() + rtpHeaderSize + 1 + 12);

    std::uint32_t recTimestamp = tsRecovery;
    std::uint16_t recLength = lengthRecovery;
    std::uint8_t recPt = ptRecovery;
    bool recMarker = mRecovery;
    std::vector<std::uint8_t> recPayload(fecPayloadPtr, fecPayloadPtr + fecPayloadSize);

    // XOR out all other packets in the mask that we received
    for (const auto& pkt : receivedPackets) {
        if (pkt.size() < sizeof(rtc::RtpHeader)) continue;
        const auto* r = reinterpret_cast<const rtc::RtpHeader*>(pkt.data());
        const auto hSize = r->getSize();
        if (pkt.size() < hSize + 1) continue;

        const auto seq = r->seqNumber();
        if (seq == missingSeq) continue;

        const int diff = static_cast<std::int16_t>(seq - snBase);
        if (diff < 0 || diff >= 16) continue;
        if ((mask & (1 << (15 - diff))) == 0) continue;

        // Strip RED header (1 byte) to get media payload
        const auto pSize = pkt.size() - (hSize + 1);
        const auto* pPtr = reinterpret_cast<const std::uint8_t*>(pkt.data() + hSize + 1);

        recTimestamp ^= r->timestamp();
        recLength ^= static_cast<std::uint16_t>(pSize);
        recPt ^= (primaryPayloadType & 0x7F);
        if (r->marker() != 0) {
            recMarker = !recMarker;
        }

        for (std::size_t i = 0; i < pSize && i < recPayload.size(); ++i) {
            recPayload[i] ^= pPtr[i];
        }
    }

    if (recPayload.size() < recLength) {
        return std::nullopt;
    }
    recPayload.resize(recLength);

    // Reconstruct original media RTP packet (standard 12-byte RTP header + payload)
    rtc::binary reconstructed(12 + recLength);
    auto* recRtp = reinterpret_cast<rtc::RtpHeader*>(reconstructed.data());
    recRtp->preparePacket();
    recRtp->setSeqNumber(missingSeq);
    recRtp->setTimestamp(recTimestamp);
    recRtp->setPayloadType(recPt);
    recRtp->setMarker(recMarker);
    recRtp->setSsrc(fecRtp->ssrc());

    if (recLength > 0) {
        std::memcpy(reconstructed.data() + 12, recPayload.data(), recLength);
    }

    return reconstructed;
}

} // namespace gateway
