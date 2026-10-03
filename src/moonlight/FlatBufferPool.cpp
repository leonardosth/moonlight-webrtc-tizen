#include "moonlight/FlatBufferPool.h"

#include <algorithm>
#include <cstring>

namespace gateway {

FlatBufferLease::FlatBufferLease(FlatBufferPool* pool,
                                 std::size_t slotIndex,
                                 std::span<const std::uint8_t> data,
                                 std::chrono::nanoseconds duration) noexcept
    : pool_(pool)
    , slotIndex_(slotIndex)
    , data_(data)
    , duration_(duration)
{
}

FlatBufferLease::~FlatBufferLease()
{
    if (pool_) {
        pool_->release(slotIndex_);
    }
}

FlatBufferLease::FlatBufferLease(FlatBufferLease&& other) noexcept
    : pool_(other.pool_)
    , slotIndex_(other.slotIndex_)
    , data_(other.data_)
    , duration_(other.duration_)
{
    other.pool_ = nullptr;
    other.data_ = {};
    other.duration_ = std::chrono::nanoseconds{0};
}

FlatBufferLease& FlatBufferLease::operator=(FlatBufferLease&& other) noexcept
{
    if (this != &other) {
        if (pool_) {
            pool_->release(slotIndex_);
        }
        pool_ = other.pool_;
        slotIndex_ = other.slotIndex_;
        data_ = other.data_;
        duration_ = other.duration_;

        other.pool_ = nullptr;
        other.data_ = {};
        other.duration_ = std::chrono::nanoseconds{0};
    }
    return *this;
}

FlatBufferPool::FlatBufferPool(std::size_t slotCount, std::size_t initialCapacity)
    : slots_(std::max<std::size_t>(1, slotCount))
{
    for (auto& slot : slots_) {
        slot.buffer.reserve(initialCapacity);
        slot.inUse = false;
    }
}

std::optional<FlatBufferLease> FlatBufferPool::flatten(
    PDECODE_UNIT decodeUnit,
    std::span<const std::uint8_t> prefix)
{
    if (!decodeUnit || !decodeUnit->bufferList || decodeUnit->fullLength <= 0) {
        return std::nullopt;
    }

    const auto start = std::chrono::steady_clock::now();
    const std::size_t totalBytes = prefix.size() + static_cast<std::size_t>(decodeUnit->fullLength);

    std::size_t acquiredIndex = 0;
    std::vector<std::uint8_t>* targetBuffer = nullptr;

    {
        const std::lock_guard lock(mutex_);
        bool found = false;
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            const std::size_t idx = (nextSlot_ + i) % slots_.size();
            if (!slots_[idx].inUse) {
                slots_[idx].inUse = true;
                acquiredIndex = idx;
                targetBuffer = &slots_[idx].buffer;
                nextSlot_ = (idx + 1) % slots_.size();
                found = true;
                break;
            }
        }
        if (!found) {
            // All configured slots are leased concurrently. Expand pool with a new slot.
            slots_.emplace_back();
            slots_.back().inUse = true;
            slots_.back().buffer.reserve(totalBytes);
            acquiredIndex = slots_.size() - 1;
            targetBuffer = &slots_.back().buffer;
            nextSlot_ = 0;
        }
    }

    if (targetBuffer->capacity() < totalBytes) {
        targetBuffer->reserve(totalBytes);
    }
    targetBuffer->resize(totalBytes);

    std::uint8_t* writePtr = targetBuffer->data();

    // Copy prefix if provided
    if (!prefix.empty()) {
        std::memcpy(writePtr, prefix.data(), prefix.size());
        writePtr += prefix.size();
    }

    // Copy Moonlight buffer chain
    std::size_t copiedPayload = 0;
    for (auto* entry = decodeUnit->bufferList; entry; entry = entry->next) {
        if (!entry->data || entry->length <= 0) {
            release(acquiredIndex);
            return std::nullopt;
        }
        std::memcpy(writePtr, entry->data, static_cast<std::size_t>(entry->length));
        writePtr += entry->length;
        copiedPayload += static_cast<std::size_t>(entry->length);
    }

    if (copiedPayload != static_cast<std::size_t>(decodeUnit->fullLength)) {
        release(acquiredIndex);
        return std::nullopt;
    }

    const auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - start);

    return FlatBufferLease(this,
                           acquiredIndex,
                           std::span<const std::uint8_t>(targetBuffer->data(), totalBytes),
                           duration);
}

void FlatBufferPool::release(std::size_t slotIndex) noexcept
{
    const std::lock_guard lock(mutex_);
    if (slotIndex < slots_.size()) {
        slots_[slotIndex].inUse = false;
    }
}

std::size_t FlatBufferPool::slotCount() const noexcept
{
    const std::lock_guard lock(mutex_);
    return slots_.size();
}

std::size_t FlatBufferPool::slotCapacity(std::size_t index) const noexcept
{
    const std::lock_guard lock(mutex_);
    if (index < slots_.size()) {
        return slots_[index].buffer.capacity();
    }
    return 0;
}

std::size_t FlatBufferPool::activeLeases() const noexcept
{
    const std::lock_guard lock(mutex_);
    std::size_t count = 0;
    for (const auto& slot : slots_) {
        if (slot.inUse) {
            ++count;
        }
    }
    return count;
}

} // namespace gateway
