#include "moonlight/FlatBufferPool.h"

#include <cassert>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

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
        std::cout << "[Test 1] FlatBufferPool initialization..." << std::endl;
        gateway::FlatBufferPool pool(4, 1024); // 4 slots of 1KB for testing
        require(pool.slotCount() == 4, "Slot count must be 4");
        require(pool.activeLeases() == 0, "Initial active leases must be 0");
        require(pool.slotCapacity(0) >= 1024, "Slot 0 initial capacity must be >= 1024");

        std::cout << "[Test 2] Null or invalid decode unit handling..." << std::endl;
        require(!pool.flatten(nullptr).has_value(), "Null decodeUnit must return nullopt");

        DECODE_UNIT invalidDu{};
        invalidDu.fullLength = 0;
        invalidDu.bufferList = nullptr;
        require(!pool.flatten(&invalidDu).has_value(), "Empty decodeUnit must return nullopt");

        std::cout << "[Test 3] Single buffer chunk flattening..." << std::endl;
        std::string chunk1 = "Hello Moonlight WebRTC!";
        LENTRY entry1{};
        entry1.data = chunk1.data();
        entry1.length = static_cast<int>(chunk1.size());
        entry1.next = nullptr;

        DECODE_UNIT du1{};
        du1.fullLength = static_cast<int>(chunk1.size());
        du1.bufferList = &entry1;

        {
            auto lease = pool.flatten(&du1);
            require(lease.has_value(), "Lease 1 must be valid");
            require(lease->size() == chunk1.size(), "Size must match chunk1");
            require(std::memcmp(lease->data().data(), chunk1.data(), chunk1.size()) == 0,
                    "Payload must match chunk1");
            require(lease->duration().count() >= 0, "Duration must be non-negative");
            require(pool.activeLeases() == 1, "Active leases must be 1 while lease is alive");
        }
        require(pool.activeLeases() == 0, "Active leases must return to 0 after lease destruction");

        std::cout << "[Test 4] Multi-chunk linked list flattening..." << std::endl;
        std::string chunkA = "Part1_";
        std::string chunkB = "Part2_";
        std::string chunkC = "Part3!";
        std::string expectedConcatenated = chunkA + chunkB + chunkC;

        LENTRY eC{nullptr, chunkC.data(), static_cast<int>(chunkC.size()), 0};
        LENTRY eB{&eC, chunkB.data(), static_cast<int>(chunkB.size()), 0};
        LENTRY eA{&eB, chunkA.data(), static_cast<int>(chunkA.size()), 0};

        DECODE_UNIT duMulti{};
        duMulti.fullLength = static_cast<int>(expectedConcatenated.size());
        duMulti.bufferList = &eA;

        {
            auto lease = pool.flatten(&duMulti);
            require(lease.has_value(), "Multi-chunk lease must be valid");
            require(lease->size() == expectedConcatenated.size(), "Multi-chunk size must match");
            require(std::memcmp(lease->data().data(), expectedConcatenated.data(), expectedConcatenated.size()) == 0,
                    "Multi-chunk payload must be correctly concatenated");
        }
        require(pool.activeLeases() == 0, "Active leases must be 0");

        std::cout << "[Test 5] Flattening with prefix (AV1 Temporal Delimiter simulation)..." << std::endl;
        const std::uint8_t prefix[] = { 0x12, 0x00 };
        {
            auto lease = pool.flatten(&duMulti, std::span<const std::uint8_t>(prefix, sizeof(prefix)));
            require(lease.has_value(), "Prefixed lease must be valid");
            require(lease->size() == sizeof(prefix) + expectedConcatenated.size(), "Size must include prefix");
            require(lease->data()[0] == 0x12 && lease->data()[1] == 0x00, "Prefix bytes must match");
            require(std::memcmp(lease->data().data() + 2, expectedConcatenated.data(), expectedConcatenated.size()) == 0,
                    "Payload after prefix must match");
        }

        std::cout << "[Test 6] Concurrent leases and pool slot recycling..." << std::endl;
        {
            auto l1 = pool.flatten(&du1);
            auto l2 = pool.flatten(&du1);
            auto l3 = pool.flatten(&du1);
            auto l4 = pool.flatten(&du1);
            require(pool.activeLeases() == 4, "All 4 configured slots should be active");

            // Exceed configured slots: pool should dynamically add a 5th slot
            auto l5 = pool.flatten(&du1);
            require(pool.activeLeases() == 5, "Pool must dynamically expand to 5 active leases");
            require(pool.slotCount() >= 5, "Slot count must be >= 5");
        }
        require(pool.activeLeases() == 0, "All leases destroyed, active leases must be 0");

        std::cout << "[Test 7] Dynamic capacity expansion for oversized frames..." << std::endl;
        std::vector<char> hugePayload(8192, 'X');
        LENTRY hugeEntry{nullptr, hugePayload.data(), static_cast<int>(hugePayload.size()), 0};
        DECODE_UNIT hugeDu{};
        hugeDu.fullLength = static_cast<int>(hugePayload.size());
        hugeDu.bufferList = &hugeEntry;

        {
            auto lease = pool.flatten(&hugeDu);
            require(lease.has_value(), "Huge frame lease must be valid");
            require(lease->size() == 8192, "Huge frame size must match 8192");
            require(pool.slotCapacity(lease->slotIndex()) >= 8192, "Slot capacity must have grown to at least 8192");
        }
        require(pool.activeLeases() == 0, "Active leases must be 0");

        std::cout << "All FlatBufferPool tests passed successfully!" << std::endl;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FlatBufferPoolTest failed: " << error.what() << std::endl;
        return 1;
    }
}
