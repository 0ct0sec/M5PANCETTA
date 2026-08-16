#pragma once

#include <stdint.h>

namespace PancettaCat {

// Durable memories are emitted only when the corresponding behavior actually
// begins on the update path. Their bit positions are an NVS contract: append
// new values before COUNT and never reorder existing entries.
enum class Memory : uint8_t {
    FACE_BUMP = 0,
    HEAD_NAP = 1,
    KNEAD = 2,
    SLOW_BLINK = 3,
    BINARY_MEOW = 4,
    NIGHT_ZOOMIES = 5,
    HAIRBALL = 6,
    MEOW = 7,
    COUNT = 8,
};
static_assert((uint8_t)Memory::COUNT < 32u,
              "cat memories must fit the persisted uint32_t mask");

static constexpr uint32_t memoryBit(Memory memory) {
    return (uint8_t)memory < (uint8_t)Memory::COUNT
        ? (1u << (uint8_t)memory)
        : 0u;
}

static constexpr uint32_t kAllMemoryBits =
    (1u << (uint8_t)Memory::COUNT) - 1u;

static inline uint8_t memoryCount(uint32_t mask) {
    mask &= kAllMemoryBits;
    uint8_t count = 0;
    while (mask) {
        count += (uint8_t)(mask & 1u);
        mask >>= 1;
    }
    return count;
}

}  // namespace PancettaCat
