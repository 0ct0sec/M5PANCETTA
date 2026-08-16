#pragma once

#include <cstddef>
#include <cstdint>

namespace RfTrace {

static constexpr uint32_t kMagic = 0x52544632u;  // "RTF2"
static constexpr uint8_t kVersion = 2u;

enum class Board : uint8_t {
    UNKNOWN = 0u,
    CORE2 = 1u,
    CORES3SE = 2u,
};

enum PoseFlags : uint8_t {
    POSE_CALIBRATED = 1u << 0u,
    POSE_STATIONARY = 1u << 1u,
    POSE_ROTATING = 1u << 2u,
    POSE_TRANSLATING = 1u << 3u,
    POSE_INTERPOLATED = 1u << 4u,
};

/**
 * Binary RF trace record. Values are scaled integers so recording never needs
 * heap allocation or locale-sensitive text formatting in the radio path.
 * A zero GPS fix-quality marks all GPS fields as unavailable.
 */
#pragma pack(push, 1)
struct Record {
    uint32_t magic = kMagic;
    uint32_t firmwareRevision = 0u;
    uint32_t rxTimestampUs = 0u;
    uint32_t observationMs = 0u;
    uint32_t queueDrops = 0u;
    uint32_t channelDwellUs = 0u;
    int32_t latitudeE7 = 0;
    int32_t longitudeE7 = 0;
    uint16_t gpsAgeMs = UINT16_MAX;
    uint16_t hdopX100 = 0u;
    uint16_t speedCms = 0u;
    uint16_t poseAgeUs = UINT16_MAX;
    int16_t yawDegX10 = 0;
    int16_t pitchDegX10 = 0;
    int16_t rollDegX10 = 0;
    uint16_t yawDriftDegX100 = 0u;
    uint16_t csiOriginalLength = 0u;
    uint16_t csiRetainedLength = 0u;
    uint16_t frameBytes = 0u;
    uint8_t identity[6] = {};
    uint8_t peer[6] = {};
    int8_t rssi = -127;
    int8_t noiseFloor = -127;
    uint8_t board = static_cast<uint8_t>(Board::UNKNOWN);
    uint8_t version = kVersion;
    uint8_t channel = 0u;
    uint8_t frameClass = 0u;
    uint8_t phyMode = 0u;
    uint8_t channelWidth = 0u;
    uint8_t secondaryChannel = 0u;
    uint8_t csiLayout = 0u;
    uint8_t poseFlags = 0u;
    uint8_t gpsFixQuality = 0u;
    uint8_t reserved[2] = {};
};
#pragma pack(pop)

static_assert(sizeof(Record) == 80u,
              "RF trace format changed; bump the version intentionally");

template <size_t Capacity>
class Ring {
    static_assert(Capacity > 0u, "RF trace capacity must be positive");

public:
    void reset() {
        head_ = 0u;
        count_ = 0u;
        overwritten_ = 0u;
    }

    void push(const Record& record) {
        records_[head_] = record;
        head_ = (head_ + 1u) % Capacity;
        if (count_ < Capacity) {
            ++count_;
        } else if (overwritten_ != UINT32_MAX) {
            ++overwritten_;
        }
    }

    size_t count() const { return count_; }
    uint32_t overwritten() const { return overwritten_; }

    bool chronological(size_t index, Record& out) const {
        if (index >= count_) return false;
        const size_t oldest =
            (head_ + Capacity - count_) % Capacity;
        out = records_[(oldest + index) % Capacity];
        return true;
    }

private:
    Record records_[Capacity] = {};
    size_t head_ = 0u;
    size_t count_ = 0u;
    uint32_t overwritten_ = 0u;
};

}  // namespace RfTrace
