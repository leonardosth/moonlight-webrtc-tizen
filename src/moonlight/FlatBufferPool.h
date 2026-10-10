#pragma once

#include <Limelight.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

namespace gateway {

class FlatBufferPool;

class FlatBufferLease {
public:
    FlatBufferLease() = default;
    FlatBufferLease(FlatBufferPool* pool,
                    std::size_t slotIndex,
                    std::span<const std::uint8_t> data,
                    std::chrono::nanoseconds duration) noexcept;

    ~FlatBufferLease();

    FlatBufferLease(const FlatBufferLease&) = delete;
    FlatBufferLease& operator=(const FlatBufferLease&) = delete;

    FlatBufferLease(FlatBufferLease&& other) noexcept;
    FlatBufferLease& operator=(FlatBufferLease&& other) noexcept;

    [[nodiscard]] std::span<const std::uint8_t> data() const noexcept { return data_; }
    [[nodiscard]] std::size_t size() const noexcept { return data_.size(); }
    [[nodiscard]] bool empty() const noexcept { return data_.empty(); }
    [[nodiscard]] std::chrono::nanoseconds duration() const noexcept { return duration_; }
    [[nodiscard]] std::size_t slotIndex() const noexcept { return slotIndex_; }

private:
    FlatBufferPool* pool_ = nullptr;
    std::size_t slotIndex_ = 0;
    std::span<const std::uint8_t> data_{};
    std::chrono::nanoseconds duration_{0};
};

class FlatBufferPool {
public:
    struct Slot {
        std::vector<std::uint8_t> buffer;
        bool inUse = false;
    };

    explicit FlatBufferPool(std::size_t slotCount = 4,
                            std::size_t initialCapacity = 2 * 1024 * 1024);

    ~FlatBufferPool() = default;

    FlatBufferPool(const FlatBufferPool&) = delete;
    FlatBufferPool& operator=(const FlatBufferPool&) = delete;

    // Flattens a Moonlight decode unit into a pre-allocated slot.
    // If prefix is provided, it is prepended to the output buffer in a single pass.
    [[nodiscard]] std::optional<FlatBufferLease> flatten(
        PDECODE_UNIT decodeUnit,
        std::span<const std::uint8_t> prefix = {});

    // Releases a slot back to the pool.
    void release(std::size_t slotIndex) noexcept;

    [[nodiscard]] std::size_t slotCount() const noexcept;
    [[nodiscard]] std::size_t slotCapacity(std::size_t index) const noexcept;
    [[nodiscard]] std::size_t activeLeases() const noexcept;

private:
    mutable std::mutex mutex_;
    std::vector<Slot> slots_;
    std::size_t nextSlot_ = 0;
};

} // namespace gateway
