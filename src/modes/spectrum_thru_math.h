#pragma once

#include <stddef.h>
#include <stdint.h>

namespace SpectrumThruMath {

static constexpr size_t kMacLength = 6u;

struct DataFrameRoute {
    uint8_t bssidOffset;
    uint8_t clientOffset;
    bool clientTransmitted;
    bool valid;
};

static inline DataFrameRoute routeDataFrame(uint8_t frameControlFlags) {
    switch (frameControlFlags & 0x03u) {
        case 0x01u:  // ToDS: addr1 AP/BSSID, addr2 client transmitter
            return {4u, 10u, true, true};
        case 0x02u:  // FromDS: addr2 AP/BSSID, addr1 client receiver
            return {10u, 4u, false, true};
        default:
            return {0u, 0u, false, false};
    }
}

// ESP32 rx_ctrl.timestamp is a shared microsecond packet identity across the
// promiscuous and CSI callbacks. Equality is proof; time proximity is not.
static inline bool samePacket(uint32_t firstRxUs, uint32_t secondRxUs) {
    return firstRxUs == secondRxUs;
}

static inline bool packetIsNew(bool havePacket, uint32_t rxTimestampUs,
                               bool consumed, uint32_t lastConsumedRxUs) {
    return havePacket &&
           (!consumed || !samePacket(rxTimestampUs, lastConsumedRxUs));
}

static inline bool alreadyConsumedByOther(bool packetIsNewForSource,
                                          uint32_t rxTimestampUs,
                                          bool otherSourceConsumed,
                                          uint32_t otherConsumedRxUs) {
    return packetIsNewForSource && otherSourceConsumed &&
           samePacket(rxTimestampUs, otherConsumedRxUs);
}

// Modular ordering is valid while observations are less than half a uint32
// timestamp period apart (about 35 minutes for the microsecond RX clock).
static inline bool firstBeforeSecond(uint32_t firstRxUs,
                                     uint32_t secondRxUs) {
    return static_cast<int32_t>(firstRxUs - secondRxUs) < 0;
}

static inline uint32_t elapsedMs(uint32_t nowMs, uint32_t thenMs) {
    return nowMs - thenMs;
}

// Packet draining can publish an observation a few milliseconds newer than a
// frame snapshot captured before the drain. Clamp that small future delta to
// zero while retaining normal uint32 rollover behavior.
static inline uint32_t observationAgeMs(uint32_t nowMs, uint32_t seenMs) {
    const int32_t signedAge = static_cast<int32_t>(nowMs - seenMs);
    return signedAge < 0 ? 0u : static_cast<uint32_t>(signedAge);
}

// Rebind a selected MAC after a compacting memmove. `records` points at the
// first MAC field and `stride` advances between containing records.
static inline int16_t findMacIndex(const uint8_t* records, uint16_t count,
                                   size_t stride,
                                   const uint8_t target[kMacLength]) {
    if (!records || !target || stride < kMacLength) return -1;

    for (uint16_t i = 0u; i < count; ++i) {
        const uint8_t* mac = records + static_cast<size_t>(i) * stride;
        bool same = true;
        for (size_t b = 0u; b < kMacLength; ++b) {
            if (mac[b] != target[b]) {
                same = false;
                break;
            }
        }
        if (same) return static_cast<int16_t>(i);
    }
    return -1;
}

// A capacity replacement must not overwrite a user-owned catalog row. The
// main Spectrum pane owns the selected AP; THRU owns its monitored AP.
static inline bool retainCatalogSlot(bool selected, bool monitored) {
    return selected || monitored;
}

// Frame-rate-independent integer response used by the analyzer display.
// elapsedMs >= responseMs lands exactly on target; shorter frames advance a
// proportional minimum-one-unit step without overshoot.
static inline int16_t approachTrace(int16_t current, int16_t target,
                                    uint32_t elapsedMs,
                                    uint16_t responseMs = 180u) {
    if (current == target || elapsedMs == 0u) return current;
    if (responseMs == 0u || elapsedMs >= responseMs) return target;

    const int32_t delta = static_cast<int32_t>(target) - current;
    int32_t step =
        delta * static_cast<int32_t>(elapsedMs) / responseMs;
    if (step == 0) step = delta > 0 ? 1 : -1;

    const int32_t next = static_cast<int32_t>(current) + step;
    if ((delta > 0 && next > target) || (delta < 0 && next < target)) {
        return target;
    }
    return static_cast<int16_t>(next);
}

// Display-only analyzer sweep. The modulo keeps the head moving through a
// delayed measurement update without manufacturing another RF observation.
static inline uint16_t analyzerSweepPhase(uint32_t nowMs,
                                          uint32_t startedMs,
                                          uint16_t periodMs) {
    if (periodMs <= 1u) return 0u;
    const uint32_t withinPeriod = (nowMs - startedMs) % periodMs;
    const uint32_t span = static_cast<uint32_t>(periodMs - 1u);
    return static_cast<uint16_t>(
        (withinPeriod * 65535u + span / 2u) / span);
}

static inline uint16_t analyzerSweepColumn(uint16_t phase,
                                           uint16_t width) {
    if (width <= 1u) return 0u;
    const uint32_t lastColumn = static_cast<uint32_t>(width - 1u);
    return static_cast<uint16_t>(
        (static_cast<uint32_t>(phase) * lastColumn + 32767u) / 65535u);
}

static inline uint16_t smoothSweepPeriod(uint16_t currentPeriodMs,
                                         uint32_t previousCompletedMs,
                                         uint32_t completedMs,
                                         uint16_t minimumMs,
                                         uint16_t maximumMs) {
    if (minimumMs > maximumMs) {
        const uint16_t swap = minimumMs;
        minimumMs = maximumMs;
        maximumMs = swap;
    }
    if (previousCompletedMs == 0u || completedMs == previousCompletedMs) {
        return currentPeriodMs;
    }

    uint32_t observedMs = completedMs - previousCompletedMs;
    if (observedMs < minimumMs) observedMs = minimumMs;
    if (observedMs > maximumMs) observedMs = maximumMs;
    if (currentPeriodMs == 0u) {
        return static_cast<uint16_t>(observedMs);
    }

    // 3:1 IIR: stable enough to read, responsive enough to follow dwell changes.
    return static_cast<uint16_t>(
        (static_cast<uint32_t>(currentPeriodMs) * 3u + observedMs) / 4u);
}

// One ordered density grammar for the live carrier field, waterfall history,
// quiet coverage floor, and C5 snapshot strip. Each tier is a nested subset of
// the next tier in a 4x4 Bayer lattice, so rising activity adds ink without
// scrambling already-visible dots. Thresholds are inclusive: the named value
// is the first intensity that belongs to that tier.
static constexpr uint8_t kDensityThresholdDots = 20u;
static constexpr uint8_t kDensityThresholdSparse3 = 50u;
static constexpr uint8_t kDensityThresholdSparse2 = 100u;
static constexpr uint8_t kDensityThresholdChecker = 150u;
static constexpr uint8_t kDensityThresholdFull = 200u;

static inline uint16_t orderedDensityMask(uint8_t intensity) {
    if (intensity >= kDensityThresholdFull) return 0xFFFFu;     // 16/16
    if (intensity >= kDensityThresholdChecker) return 0xA5A5u; // 8/16
    if (intensity >= kDensityThresholdSparse2) return 0x0505u; // 4/16
    if (intensity >= kDensityThresholdSparse3) return 0x0405u; // 3/16
    if (intensity >= kDensityThresholdDots) return 0x0001u;    // 1/16
    return 0u;
}

static inline bool orderedDensityCellVisible(uint8_t intensity,
                                              uint16_t x,
                                              uint16_t y) {
    const uint8_t cell = static_cast<uint8_t>(
        ((y & 3u) << 2u) | (x & 3u));
    return (orderedDensityMask(intensity) &
            static_cast<uint16_t>(1u << cell)) != 0u;
}

static inline uint8_t newestWaterfallRow(uint8_t writeRow,
                                         uint8_t screenRow,
                                         uint8_t rowCount) {
    if (rowCount == 0u) return 0u;
    return static_cast<uint8_t>(
        (writeRow + rowCount - 1u - (screenRow % rowCount)) % rowCount);
}

// A completed remote scan is one observation even when several retained rows
// are refreshed from it. Revision zero is the "no completed scan" sentinel.
static inline bool scanObservationIsNew(uint32_t revision,
                                        uint32_t lastConsumedRevision) {
    return revision != 0u && revision != lastConsumedRevision;
}

// Scan RSSI tagged to device pose supports only a coarse bearing. It cannot
// justify packet-cadence or phase-level certainty.
static inline uint8_t capScanBearingConfidence(uint8_t confidence,
                                               uint8_t confidenceCap) {
    return confidence < confidenceCap ? confidence : confidenceCap;
}

}  // namespace SpectrumThruMath
