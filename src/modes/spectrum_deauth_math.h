#pragma once

#include <stdint.h>

namespace SpectrumDeauthMath {

struct Batch {
    uint16_t count = 0u;
    int8_t peakRssi = -127;
    uint8_t peakChannel = 0u;
};

static inline uint32_t pack(const Batch& batch) {
    return ((uint32_t)batch.count << 16u) |
           ((uint32_t)(uint8_t)batch.peakRssi << 8u) |
           (uint32_t)batch.peakChannel;
}

static inline Batch unpack(uint32_t packed) {
    Batch batch{};
    batch.count = (uint16_t)(packed >> 16u);
    batch.peakRssi = (int8_t)((packed >> 8u) & 0xFFu);
    batch.peakChannel = (uint8_t)(packed & 0xFFu);
    return batch;
}

// Fold one callback observation into a single atomic word. The main loop gets
// the real frame count plus the strongest sample/channel without a torn
// multi-field payload.
static inline uint32_t accumulate(uint32_t packed, int8_t rssi,
                                  uint8_t channel) {
    Batch batch = unpack(packed);
    if (batch.count == 0u || rssi > batch.peakRssi) {
        batch.peakRssi = rssi;
        batch.peakChannel = channel;
    }
    if (batch.count < UINT16_MAX) ++batch.count;
    return pack(batch);
}

static inline uint16_t saturatingAdd(uint16_t value, uint16_t add) {
    const uint32_t sum = (uint32_t)value + (uint32_t)add;
    return sum > UINT16_MAX ? UINT16_MAX : (uint16_t)sum;
}

}  // namespace SpectrumDeauthMath
