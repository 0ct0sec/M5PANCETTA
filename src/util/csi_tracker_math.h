#pragma once

#include <stdint.h>

namespace CsiTrackerMath {

static constexpr uint32_t kTemporalFullConfidenceMs = 2500u;
static constexpr uint32_t kTemporalComparableMaxMs = 16000u;
static constexpr uint32_t kStaleDecayStartMs = 5000u;
static constexpr uint32_t kStaleDecayStepMs = 1000u;

static inline uint32_t elapsedMs(uint32_t nowMs, uint32_t thenMs) {
    return nowMs - thenMs;
}

static inline uint8_t temporalWeight(bool sameShape, uint32_t gapMs) {
    if (!sameShape || gapMs > kTemporalComparableMaxMs) return 0u;
    if (gapMs <= kTemporalFullConfidenceMs) return 100u;

    const uint32_t span = kTemporalComparableMaxMs -
                          kTemporalFullConfidenceMs;
    const uint32_t excess = gapMs - kTemporalFullConfidenceMs;
    const int weight = 100 - (int)(excess * 75u / span);
    return (uint8_t)weight;
}

static inline uint8_t staleDecaySteps(uint32_t ageMs) {
    if (ageMs <= kStaleDecayStartMs) return 0u;
    const uint32_t steps =
        ((ageMs - kStaleDecayStartMs) / kStaleDecayStepMs) + 1u;
    return (uint8_t)(steps > 20u ? 20u : steps);
}

}  // namespace CsiTrackerMath
