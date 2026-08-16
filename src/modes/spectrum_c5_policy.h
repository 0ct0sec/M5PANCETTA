#pragma once

#include <stdint.h>

namespace SpectrumC5Policy {

// Include the full modeled 44 MHz lobe for the lowest and highest observer
// channels. The renderer uses channel center +/- 22 MHz.
static constexpr float BAND_START_MHZ = 5155.0f;
static constexpr float BAND_END_MHZ = 5850.0f;
static constexpr float BAND_WIDTH_MHZ = BAND_END_MHZ - BAND_START_MHZ;
static constexpr uint8_t CHANNELS_5GHZ[] = {
    36, 40, 44, 48, 52, 56, 60, 64,
    100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144,
    149, 153, 157, 161, 165
};
static constexpr uint8_t CHANNEL_5GHZ_COUNT =
    static_cast<uint8_t>(sizeof(CHANNELS_5GHZ) /
                         sizeof(CHANNELS_5GHZ[0]));

inline bool isObserverChannel(uint8_t channel) {
    for (uint8_t i = 0u; i < CHANNEL_5GHZ_COUNT; ++i) {
        if (CHANNELS_5GHZ[i] == channel) return true;
    }
    return false;
}

inline float channelCenterMHz(uint8_t channel) {
    return 5000.0f + static_cast<float>(channel) * 5.0f;
}

inline float normalizedViewWidthMHz(float viewWidthMHz) {
    if (viewWidthMHz <= 0.0f || viewWidthMHz > BAND_WIDTH_MHZ) {
        return BAND_WIDTH_MHZ;
    }
    return viewWidthMHz;
}

inline float focusedCenterMHz(uint8_t channel, float viewWidthMHz) {
    viewWidthMHz = normalizedViewWidthMHz(viewWidthMHz);
    const float halfWidth = viewWidthMHz * 0.5f;
    const float minCenter = BAND_START_MHZ + halfWidth;
    const float maxCenter = BAND_END_MHZ - halfWidth;
    const float target = channelCenterMHz(channel);
    if (target < minCenter) return minCenter;
    if (target > maxCenter) return maxCenter;
    return target;
}

inline void displayWindowMHz(float viewCenterMHz, float viewWidthMHz,
                             float& leftMHz, float& rightMHz) {
    viewWidthMHz = normalizedViewWidthMHz(viewWidthMHz);

    leftMHz = viewCenterMHz - viewWidthMHz * 0.5f;
    rightMHz = viewCenterMHz + viewWidthMHz * 0.5f;
    if (leftMHz < BAND_START_MHZ) {
        leftMHz = BAND_START_MHZ;
        rightMHz = leftMHz + viewWidthMHz;
    }
    if (rightMHz > BAND_END_MHZ) {
        rightMHz = BAND_END_MHZ;
        leftMHz = rightMHz - viewWidthMHz;
    }
}

inline int16_t selectionAfterSnapshot(int16_t previousIndex,
                                      int16_t restoredIndex,
                                      uint16_t networkCount) {
    if (networkCount == 0u) return -1;
    const int16_t lastIndex = static_cast<int16_t>(networkCount - 1u);
    if (restoredIndex >= 0 && restoredIndex <= lastIndex) {
        return restoredIndex;
    }
    if (previousIndex < 0) return 0;
    if (previousIndex > lastIndex) return lastIndex;
    return previousIndex;
}

}  // namespace SpectrumC5Policy
