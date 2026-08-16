#pragma once

#include <stdint.h>

namespace CsiFeatures {

enum LtfMask : uint8_t {
    LTF_NONE = 0u,
    LTF_LLTF = 1u << 0,
    LTF_HT = 1u << 1,
    LTF_STBC_HT = 1u << 2,
};

struct Metadata {
    uint8_t signalMode = 0u;
    uint8_t channelBandwidth = 0u;
    uint8_t secondaryChannel = 0u;
    uint8_t stbc = 0u;
    uint16_t originalLength = 0u;
    uint16_t retainedLength = 0u;
    bool firstWordInvalid = false;
};

struct Layout {
    bool valid = false;
    uint8_t ltfMask = LTF_NONE;
    uint16_t lltfStart = 0u;
    uint16_t lltfPairs = 0u;
    uint16_t htltfStart = 0u;
    uint16_t htltfPairs = 0u;
    uint16_t stbcStart = 0u;
    uint16_t stbcPairs = 0u;
    uint16_t retainedPairs = 0u;
    uint16_t usablePairs = 0u;
    uint32_t key = 0u;
};

struct PowerFeatures {
    uint16_t usablePairs = 0u;
    uint16_t medianPower = 0u;
    uint8_t retainedCoverage = 0u;
    uint8_t frequencySpread = 0u;
    uint8_t fadeShape = 0u;
};

static inline int absInt(int value) {
    return value < 0 ? -value : value;
}

static inline uint8_t clampU8(int value) {
    if (value < 0) return 0u;
    if (value > 100) return 100u;
    return (uint8_t)value;
}

static inline bool pilotSubcarrier(int index, bool ht40) {
    const int magnitude = absInt(index);
    if (ht40) {
        return magnitude == 11 || magnitude == 25 ||
               magnitude == 53;
    }
    return magnitude == 7 || magnitude == 21;
}

static inline int signedSubcarrier(uint16_t localPair,
                                   uint16_t segmentPairs) {
    if (segmentPairs <= 64u) {
        return localPair < 32u
            ? (int)localPair
            : (int)localPair - 64;
    }
    const uint16_t positivePairs =
        (uint16_t)((segmentPairs + 1u) / 2u);
    return localPair < positivePairs
        ? (int)localPair
        : (int)localPair - (int)segmentPairs;
}

static inline bool segmentUsable(uint16_t localPair,
                                 uint16_t segmentPairs,
                                 bool htLtf,
                                 bool ht40) {
    const int index = signedSubcarrier(localPair, segmentPairs);
    if (index == 0) return false;
    const int edge = htLtf ? (ht40 ? 57 : 28) : 26;
    if (absInt(index) > edge) return false;
    return !pilotSubcarrier(index, ht40 && htLtf);
}

static inline bool usablePair(const Layout& layout,
                              const Metadata& metadata,
                              uint16_t pair) {
    if (!layout.valid || pair >= layout.retainedPairs) return false;
    if (metadata.firstWordInvalid && pair < 2u) return false;

    if (pair >= layout.lltfStart &&
        pair < layout.lltfStart + layout.lltfPairs) {
        return segmentUsable(
            (uint16_t)(pair - layout.lltfStart),
            layout.lltfPairs, false, false);
    }
    if (pair >= layout.htltfStart &&
        pair < layout.htltfStart + layout.htltfPairs) {
        return segmentUsable(
            (uint16_t)(pair - layout.htltfStart),
            layout.htltfPairs, true,
            metadata.channelBandwidth != 0u);
    }
    if (pair >= layout.stbcStart &&
        pair < layout.stbcStart + layout.stbcPairs) {
        return segmentUsable(
            (uint16_t)(pair - layout.stbcStart),
            layout.stbcPairs, true,
            metadata.channelBandwidth != 0u);
    }
    return false;
}

static inline Layout deriveLayout(const Metadata& metadata) {
    Layout layout{};
    if (metadata.originalLength < 2u ||
        metadata.retainedLength < 2u) {
        return layout;
    }

    const uint16_t originalPairs =
        (uint16_t)(metadata.originalLength / 2u);
    layout.retainedPairs =
        (uint16_t)(metadata.retainedLength / 2u);
    if (layout.retainedPairs > originalPairs) {
        layout.retainedPairs = originalPairs;
    }

    // With LLTF, HT-LTF and STBC-HT-LTF enabled, IDF stores fields in that
    // order. LLTF is always 64 complex pairs. HT40/STBC field lengths vary
    // with secondary-channel placement, so derive their exact boundaries
    // from the reported total rather than assuming a universal 128-pair LTF.
    layout.lltfPairs = originalPairs < 64u ? originalPairs : 64u;
    if (layout.lltfPairs > 0u) layout.ltfMask |= LTF_LLTF;
    uint16_t cursor = layout.lltfPairs;

    if (metadata.signalMode != 0u && originalPairs > cursor) {
        const uint16_t remaining =
            (uint16_t)(originalPairs - cursor);
        layout.htltfStart = cursor;
        if (metadata.stbc != 0u) {
            layout.htltfPairs = (uint16_t)(remaining / 2u);
            layout.stbcStart =
                (uint16_t)(cursor + layout.htltfPairs);
            layout.stbcPairs =
                (uint16_t)(remaining - layout.htltfPairs);
        } else {
            layout.htltfPairs = remaining;
        }
        if (layout.htltfPairs > 0u) layout.ltfMask |= LTF_HT;
        if (layout.stbcPairs > 0u) layout.ltfMask |= LTF_STBC_HT;
    }

    layout.valid = layout.lltfPairs > 0u;
    for (uint16_t pair = 0u; pair < layout.retainedPairs; ++pair) {
        if (usablePair(layout, metadata, pair)) ++layout.usablePairs;
    }

    uint32_t key = 2166136261u;
    const uint32_t words[] = {
        metadata.signalMode,
        metadata.channelBandwidth,
        metadata.secondaryChannel,
        metadata.stbc,
        metadata.originalLength,
        metadata.retainedLength,
        layout.ltfMask,
        layout.lltfPairs,
        layout.htltfPairs,
        layout.stbcPairs,
        metadata.firstWordInvalid ? 1u : 0u,
    };
    for (uint8_t i = 0u;
         i < (uint8_t)(sizeof(words) / sizeof(words[0])); ++i) {
        key ^= words[i];
        key *= 16777619u;
    }
    layout.key = key;
    return layout;
}

static inline uint32_t selectMedian(uint32_t* values, uint16_t count) {
    if (count == 0u) return 0u;
    int left = 0;
    int right = (int)count - 1;
    const int target = (int)count / 2;
    while (left < right) {
        uint32_t pivot = values[(left + right) / 2];
        int i = left;
        int j = right;
        while (i <= j) {
            while (values[i] < pivot) ++i;
            while (values[j] > pivot) --j;
            if (i <= j) {
                const uint32_t temp = values[i];
                values[i] = values[j];
                values[j] = temp;
                ++i;
                --j;
            }
        }
        if (target <= j) {
            right = j;
        } else if (target >= i) {
            left = i;
        } else {
            break;
        }
    }
    return values[target];
}

static inline bool extractPower(
    const int8_t* iq, uint16_t retainedLength,
    const Metadata& metadata, const Layout& layout,
    uint32_t* powers, uint32_t* medianScratch,
    uint16_t* normalizedPower, uint16_t capacity,
    PowerFeatures& features) {
    features = {};
    if (!iq || !layout.valid || capacity == 0u) return false;

    const uint16_t availablePairs =
        (uint16_t)(retainedLength / 2u);
    uint16_t count = 0u;
    for (uint16_t pair = 0u;
         pair < availablePairs && pair < layout.retainedPairs;
         ++pair) {
        if (!usablePair(layout, metadata, pair)) continue;
        if (count >= capacity) break;
        const int imaginary = (int)iq[pair * 2u];
        const int real = (int)iq[pair * 2u + 1u];
        const uint32_t power =
            (uint32_t)(imaginary * imaginary + real * real);
        powers[count] = power;
        medianScratch[count] = power;
        ++count;
    }
    if (count == 0u) return false;

    const uint32_t median = selectMedian(medianScratch, count);
    if (median == 0u) return false;
    uint32_t spreadSum = 0u;
    uint16_t deepFadeCount = 0u;
    for (uint16_t i = 0u; i < count; ++i) {
        uint32_t normalized = powers[i] * 256u / median;
        if (normalized > 2048u) normalized = 2048u;
        normalizedPower[i] = (uint16_t)normalized;
        spreadSum += (uint32_t)absInt((int)normalized - 256);
        if (normalized < 96u) ++deepFadeCount;
    }

    features.usablePairs = count;
    features.medianPower =
        (uint16_t)(median > 65535u ? 65535u : median);
    features.retainedCoverage = clampU8(
        metadata.originalLength == 0u
            ? 0
            : (int)((uint32_t)metadata.retainedLength * 100u /
                    metadata.originalLength));
    features.frequencySpread = clampU8(
        (int)(spreadSum / count) / 2);
    features.fadeShape = clampU8(
        (int)((uint32_t)deepFadeCount * 100u / count));
    return true;
}

static inline uint8_t compareNormalized(
    const uint16_t* prior, const uint16_t* current,
    uint16_t count, uint8_t& correlation) {
    correlation = 0u;
    if (!prior || !current || count == 0u) return 0u;
    uint32_t delta = 0u;
    uint32_t intersection = 0u;
    uint32_t unionPower = 0u;
    for (uint16_t i = 0u; i < count; ++i) {
        delta += (uint32_t)absInt(
            (int)current[i] - (int)prior[i]);
        intersection +=
            prior[i] < current[i] ? prior[i] : current[i];
        unionPower +=
            prior[i] > current[i] ? prior[i] : current[i];
    }
    correlation = unionPower == 0u
        ? 0u
        : clampU8((int)(intersection * 100u / unionPower));
    return clampU8((int)(delta / count) / 2);
}

}  // namespace CsiFeatures
