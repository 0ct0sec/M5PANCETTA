#pragma once

#include <cstddef>
#include <cstdint>

namespace RfMeasurement {

static constexpr uint8_t kFirstChannel = 1u;
static constexpr uint8_t kLastChannel = 13u;
static constexpr uint16_t kAllChannelsMask = 0x1FFFu;
static constexpr uint8_t kRobustWindow = 17u;

enum class FrameClass : uint8_t {
    MANAGEMENT = 0u,
    CONTROL = 1u,
    DATA = 2u,
    OTHER = 3u,
};

struct PacketObservation {
    uint32_t rxTimestampUs = 0u;
    uint16_t bytes = 0u;
    int8_t rssi = -127;
    int8_t noiseFloor = -127;
    uint8_t channel = 0u;
    FrameClass frameClass = FrameClass::OTHER;
};

struct ChannelEvidence {
    uint32_t frames = 0u;
    uint32_t bytes = 0u;
    uint32_t dwellMs = 0u;
    uint32_t firstRxTimestampUs = 0u;
    uint32_t lastRxTimestampUs = 0u;
    uint16_t managementFrames = 0u;
    uint16_t controlFrames = 0u;
    uint16_t dataFrames = 0u;
    uint16_t sampleCount = 0u;
    int8_t medianRssi = -127;
    int8_t trimmedMeanRssi = -127;
    uint8_t rssiSpread = 0u;
    int8_t peakRssi = -127;
    int8_t medianNoiseFloor = -127;
    int8_t medianSnr = -127;
};

struct SweepSnapshot {
    ChannelEvidence channels[kLastChannel + 1u] = {};
    uint32_t epoch = 0u;
    uint32_t completedAtMs = 0u;
    uint32_t queueDrops = 0u;
    uint16_t coverageMask = 0u;
};

inline uint32_t framesPerSecond(const ChannelEvidence& evidence) {
    if (evidence.dwellMs == 0u) return 0u;
    const uint64_t rate =
        (static_cast<uint64_t>(evidence.frames) * 1000u) /
        evidence.dwellMs;
    return rate > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(rate);
}

inline uint32_t bytesPerSecond(const ChannelEvidence& evidence) {
    if (evidence.dwellMs == 0u) return 0u;
    const uint64_t rate =
        (static_cast<uint64_t>(evidence.bytes) * 1000u) /
        evidence.dwellMs;
    return rate > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(rate);
}

/**
 * Deterministic 87..117 ms dwell. The varying cadence avoids repeatedly
 * phase-locking a 100 ms hop to common 102.4 ms beacon intervals.
 */
inline uint16_t dwellDurationMs(uint8_t channel, uint32_t sweepEpoch) {
    uint32_t hash = 0x9E3779B9u ^ (sweepEpoch * 0x85EBCA6Bu);
    hash ^= static_cast<uint32_t>(channel) * 0xC2B2AE35u;
    hash ^= hash >> 16u;
    hash *= 0x7FEB352Du;
    hash ^= hash >> 15u;
    return static_cast<uint16_t>(87u + (hash % 31u));
}

class Tracker {
public:
    void reset(uint32_t nowMs, uint8_t initialChannel) {
        clearAccumulators();
        latest_ = {};
        activeChannel_ = validChannel(initialChannel) ? initialChannel : 0u;
        dwellStartMs_ = nowMs;
        coverageMask_ = 0u;
        epoch_ = 0u;
        revision_ = 0u;
        consumedRevision_ = 0u;
    }

    /**
     * Abandon only the sweep currently being collected. The last completed
     * snapshot and its epoch remain available for display, but traffic seen
     * during a manual channel lock cannot leak into the next hopping sweep.
     */
    void discardPartial(uint32_t nowMs, uint8_t initialChannel) {
        clearAccumulators();
        activeChannel_ = validChannel(initialChannel) ? initialChannel : 0u;
        dwellStartMs_ = nowMs;
        coverageMask_ = 0u;
    }

    void observe(const PacketObservation& observation) {
        if (!validChannel(observation.channel)) return;
        Accumulator& acc = accumulators_[observation.channel];
        if (acc.frames != UINT32_MAX) ++acc.frames;
        const uint32_t room = UINT32_MAX - acc.bytes;
        acc.bytes += observation.bytes > room ? room : observation.bytes;
        saturatingIncrement(acc.classCounts[
            static_cast<uint8_t>(observation.frameClass)]);

        if (!acc.haveTimestamp) {
            acc.firstRxTimestampUs = observation.rxTimestampUs;
            acc.haveTimestamp = true;
        }
        acc.lastRxTimestampUs = observation.rxTimestampUs;

        addSample(acc.rssi, observation.rssi);
        if (observation.noiseFloor < 0) {
            addSample(acc.noise, observation.noiseFloor);
            int16_t snr = static_cast<int16_t>(observation.rssi) -
                          static_cast<int16_t>(observation.noiseFloor);
            if (snr < -127) snr = -127;
            if (snr > 127) snr = 127;
            addSample(acc.snr, static_cast<int8_t>(snr));
        }
        if (!acc.havePeak || observation.rssi > acc.peakRssi) {
            acc.peakRssi = observation.rssi;
            acc.havePeak = true;
        }
    }

    /**
     * Close the previous dwell and start the next. A snapshot becomes
     * available only after all 13 native 2.4 GHz channels were actually
     * visited, so incomplete sweeps never masquerade as full coverage.
     */
    bool onChannelChange(uint8_t previousChannel, uint8_t nextChannel,
                         uint32_t nowMs, uint32_t queueDrops) {
        if (validChannel(previousChannel) && previousChannel == activeChannel_) {
            accumulators_[previousChannel].dwellMs += nowMs - dwellStartMs_;
            coverageMask_ |=
                static_cast<uint16_t>(1u << (previousChannel - 1u));
        }
        activeChannel_ = validChannel(nextChannel) ? nextChannel : 0u;
        dwellStartMs_ = nowMs;

        if (coverageMask_ != kAllChannelsMask) return false;
        finalize(nowMs, queueDrops);
        return true;
    }

    bool consumeCompleted(SweepSnapshot& out) {
        if (revision_ == consumedRevision_) return false;
        out = latest_;
        consumedRevision_ = revision_;
        return true;
    }

    const SweepSnapshot& latest() const { return latest_; }
    uint32_t epoch() const { return epoch_; }
    uint16_t coverageMask() const { return coverageMask_; }

private:
    struct SampleWindow {
        int8_t values[kRobustWindow] = {};
        uint8_t count = 0u;
        uint8_t head = 0u;
    };

    struct Accumulator {
        uint32_t frames = 0u;
        uint32_t bytes = 0u;
        uint32_t dwellMs = 0u;
        uint32_t firstRxTimestampUs = 0u;
        uint32_t lastRxTimestampUs = 0u;
        uint16_t classCounts[4] = {};
        SampleWindow rssi{};
        SampleWindow noise{};
        SampleWindow snr{};
        int8_t peakRssi = -127;
        bool haveTimestamp = false;
        bool havePeak = false;
    };

    static bool validChannel(uint8_t channel) {
        return channel >= kFirstChannel && channel <= kLastChannel;
    }

    static void saturatingIncrement(uint16_t& value) {
        if (value != UINT16_MAX) ++value;
    }

    static void addSample(SampleWindow& window, int8_t value) {
        window.values[window.head] = value;
        window.head = static_cast<uint8_t>(
            (window.head + 1u) % kRobustWindow);
        if (window.count < kRobustWindow) ++window.count;
    }

    static void sortSamples(const SampleWindow& window, int8_t* sorted) {
        for (uint8_t i = 0u; i < window.count; ++i) {
            sorted[i] = window.values[i];
        }
        for (uint8_t i = 1u; i < window.count; ++i) {
            const int8_t key = sorted[i];
            uint8_t j = i;
            while (j > 0u && sorted[j - 1u] > key) {
                sorted[j] = sorted[j - 1u];
                --j;
            }
            sorted[j] = key;
        }
    }

    static int8_t median(const SampleWindow& window) {
        if (window.count == 0u) return -127;
        int8_t sorted[kRobustWindow];
        sortSamples(window, sorted);
        const uint8_t mid = window.count / 2u;
        if ((window.count & 1u) != 0u) return sorted[mid];
        return static_cast<int8_t>(
            (static_cast<int16_t>(sorted[mid - 1u]) + sorted[mid]) / 2);
    }

    static int8_t trimmedMean(const SampleWindow& window) {
        if (window.count == 0u) return -127;
        int8_t sorted[kRobustWindow];
        sortSamples(window, sorted);
        const uint8_t trim = window.count >= 10u ? window.count / 10u : 0u;
        int16_t total = 0;
        uint8_t used = 0u;
        for (uint8_t i = trim; i < window.count - trim; ++i) {
            total += sorted[i];
            ++used;
        }
        return used == 0u ? -127 : static_cast<int8_t>(total / used);
    }

    static uint8_t spread(const SampleWindow& window) {
        if (window.count < 2u) return 0u;
        int8_t sorted[kRobustWindow];
        sortSamples(window, sorted);
        const uint8_t low = window.count / 4u;
        const uint8_t high =
            static_cast<uint8_t>((window.count * 3u) / 4u);
        const int16_t delta =
            static_cast<int16_t>(sorted[high]) - sorted[low];
        return delta < 0 ? 0u : static_cast<uint8_t>(delta);
    }

    static ChannelEvidence summarize(const Accumulator& acc) {
        ChannelEvidence out{};
        out.frames = acc.frames;
        out.bytes = acc.bytes;
        out.dwellMs = acc.dwellMs;
        out.firstRxTimestampUs = acc.firstRxTimestampUs;
        out.lastRxTimestampUs = acc.lastRxTimestampUs;
        out.managementFrames =
            acc.classCounts[static_cast<uint8_t>(FrameClass::MANAGEMENT)];
        out.controlFrames =
            acc.classCounts[static_cast<uint8_t>(FrameClass::CONTROL)];
        out.dataFrames =
            acc.classCounts[static_cast<uint8_t>(FrameClass::DATA)];
        out.sampleCount = acc.rssi.count;
        out.medianRssi = median(acc.rssi);
        out.trimmedMeanRssi = trimmedMean(acc.rssi);
        out.rssiSpread = spread(acc.rssi);
        out.peakRssi = acc.havePeak ? acc.peakRssi : -127;
        out.medianNoiseFloor = median(acc.noise);
        out.medianSnr = median(acc.snr);
        return out;
    }

    void clearAccumulators() {
        for (Accumulator& accumulator : accumulators_) {
            accumulator = {};
        }
    }

    void finalize(uint32_t nowMs, uint32_t queueDrops) {
        SweepSnapshot next{};
        next.epoch = ++epoch_;
        next.completedAtMs = nowMs;
        next.queueDrops = queueDrops;
        next.coverageMask = coverageMask_;
        for (uint8_t channel = kFirstChannel;
             channel <= kLastChannel; ++channel) {
            next.channels[channel] = summarize(accumulators_[channel]);
        }
        latest_ = next;
        ++revision_;

        clearAccumulators();
        coverageMask_ = 0u;
    }

    Accumulator accumulators_[kLastChannel + 1u] = {};
    SweepSnapshot latest_{};
    uint8_t activeChannel_ = 0u;
    uint32_t dwellStartMs_ = 0u;
    uint16_t coverageMask_ = 0u;
    uint32_t epoch_ = 0u;
    uint32_t revision_ = 0u;
    uint32_t consumedRevision_ = 0u;
};

}  // namespace RfMeasurement
