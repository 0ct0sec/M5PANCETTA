/**
 * Stable, bounded contracts shared by defense acquisition, fusion and sinks.
 *
 * This header intentionally has no Arduino/ESP32 dependencies so the pipeline
 * boundary can be characterized by native tests. Producers fill observation
 * batches; fusion emits DefenseEventData; delivery and persistence are separate
 * concerns owned by DefensePipeline.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace Defense {

enum class ThreatType : uint8_t {
    UNKNOWN = 0,
    AIRTAG,
    SMARTTAG,
    TILE,
    FAST_PAIR,
    BLE_SPAM,
    GENERIC_TRACKER,
    IBEACON,
    EDDYSTONE,
    APPLE_NEARBY,
    SUSPICIOUS_PERIPHERAL,
    FLIPPER,
    HID_DEVICE,
    SMARTTAG_UNREGISTERED,
    XIAOMI_TRACKER,
    SIDEWALK_BEACON,
    EXPOSURE_NOTIF,
    FMDN,
    _COUNT
};

enum class SpamPlatform : uint8_t { UNKNOWN = 0, IOS, WINDOWS, SAMSUNG, ANDROID };

enum class DeauthTool : uint8_t {
    UNKNOWN = 0,
    MDK3,
    AIRGEDDON,
    FLIPPER,
    BETTERCAP,
    ESP32_DEAUTHER,
    PWNAGOTCHI,
    CUSTOM,
};

enum class DeauthSourceOrigin : uint8_t {
    RECON_SNIFF = 0,
    HUNT_CALLBACK,
    SPECTRUM_CALLBACK
};

enum class CadenceTier : uint8_t { NORMAL = 0, ELEVATED, AGGRESSIVE };

enum class DefenseEvent : uint8_t {
    NONE = 0,
    TRACKER_NEW,
    TRACKER_FOLLOWING,
    BLE_SPAM,
    EVIL_TWIN,
    KARMA_HONEYPOT,
    FINGERPRINT_MISMATCH,
    SEQ_ANOMALY,
    RSSI_ANOMALY,
    KNOWN_AP,
    OPEN_AP_WARNING,
    PROBE_VULN_CLIENT,
    DEAUTH_DETECTED,
    SCAN_COMPLETE,
    COORDINATED_ATTACK,
    ATTACKER_IDENTIFIED,
    DUAL_BAND_STALK,
    FOLLOWING_NETWORK_ID,
    WATCHLIST_ENTER,
    WATCHLIST_EXIT,
    KARMA_CONFIRMED,
    CANARY_TRIPPED,
    RELAY_SUSPECT,
    HOSTILE_CLIENT,
    TOOL_IDENTIFIED,
    LOW_ENTROPY_BEACON,
};

struct DefenseEventData {
    DefenseEvent event;
    ThreatType threatType;
    int8_t rssi;
    uint8_t channel;
    uint8_t count;
    uint8_t bssid[6];
    char ssid[33];
    char detail[32];
};

// Capacities are part of the firmware behavior contract, not tuning defaults.
static constexpr size_t MAX_TRACKERS = 64;
static constexpr size_t MAX_BLE_OBSERVATIONS = 96;
static constexpr size_t MAX_WIFI_OBSERVATIONS = 64;
static constexpr size_t MAX_XBAND_OBSERVATIONS = 16;
static constexpr size_t MAX_EVENT_QUEUE = 16;
static constexpr size_t EVENT_CRITICAL_RESERVE = 4;
// A queue slot can only be replaced by a strictly higher priority (0..3).
// This bounds every successful admission, including replacements, while a
// fusion batch is staged against a copy of the published event queue.
static constexpr size_t MAX_EVENT_ADMISSIONS_PER_BATCH =
    (MAX_EVENT_QUEUE - 1) * 4;

struct BleObservation {
    uint32_t timestampMs;
    uint8_t addr[6];
    int8_t rssi;
    ThreatType type;
    uint8_t payloadHash[4];
    int8_t txPower;
    uint8_t advFlags;
    uint8_t advType;
    uint8_t addrType;
    uint8_t frameType;
    uint16_t companyId;
    uint16_t primaryService;
    uint16_t appearance;
    uint16_t major;
    uint16_t minor;
    uint8_t spamPlatform;
    uint8_t payloadLen;
    uint8_t serviceCount;
    uint8_t manufacturerCount;
    uint8_t payloadPreviewLen;
    uint32_t classOfDevice;
    uint16_t advInterval;
    uint16_t measuredAdvIntervalMs;
    uint32_t lastAdvTimestamp;
    uint16_t intervalVariance;
    uint16_t repeatCount;
    uint16_t companyId2;
    char name[16];
    uint8_t payloadPreview[16];
    uint8_t macs[4][6];
    uint8_t macCount;
    bool valid;
};

struct WifiObservation {
    uint32_t timestampMs;
    char ssid[33];
    uint8_t bssid[6];
    int8_t rssi;
    uint8_t channel;
    uint8_t authMode;
    uint8_t entropyScore;
    float lat;
    float lon;
    bool valid;
};

enum class XBandObservationKind : uint8_t {
    ATTACKER = 0,
    PERSISTENT_CLIENT,
    COHORT,
    CROWD,
    VENDOR
};

struct XBandObservation {
    uint32_t timestampMs;
    XBandObservationKind kind;
    uint32_t primaryId;
    uint32_t secondaryId;
    int8_t primaryRssi;
    int8_t secondaryRssi;
    uint8_t confidence;
    uint8_t flags;
    bool valid;
};

template <typename T, size_t Capacity>
class ObservationBatch {
public:
    static constexpr size_t capacity() { return Capacity; }

    bool push(const T& observation) {
        if (!observation.valid || count_ >= Capacity) return false;
        records_[count_++] = observation;
        return true;
    }

    void clear() { count_ = 0; }
    size_t size() const { return count_; }
    bool empty() const { return count_ == 0; }
    const T* data() const { return records_; }
    const T& operator[](size_t index) const { return records_[index]; }

private:
    T records_[Capacity] = {};
    size_t count_ = 0;
};

using BleObservationBatch = ObservationBatch<BleObservation, MAX_BLE_OBSERVATIONS>;
using WifiObservationBatch = ObservationBatch<WifiObservation, MAX_WIFI_OBSERVATIONS>;
using XBandObservationBatch = ObservationBatch<XBandObservation, MAX_XBAND_OBSERVATIONS>;

inline bool elapsedAtLeast(uint32_t now, uint32_t then, uint32_t intervalMs) {
    return static_cast<uint32_t>(now - then) >= intervalMs;
}

inline size_t temporalBucketIndex(uint32_t now, uint32_t timestamp,
                                  size_t bucketCount, uint32_t windowMs) {
    if (timestamp == 0 || bucketCount == 0) return bucketCount;
    const uint32_t age = now - timestamp;
    if (age > windowMs) return bucketCount;
    uint32_t bucketMs = windowMs / bucketCount;
    if (bucketMs == 0) bucketMs = 1;
    size_t bucket = (windowMs - age) / bucketMs;
    if (bucket >= bucketCount) bucket = bucketCount - 1;
    return bucket;
}

inline uint32_t nextGeneration(uint32_t current) {
    const uint32_t next = current + 1;
    return next == 0 ? 1 : next;
}

// Pure publication state used by the double-buffered snapshot publisher. The
// non-zero generation makes an uninitialized snapshot distinguishable from a
// published one, including across uint32_t rollover.
struct PublicationCursor {
    uint32_t generation = 0;
    uint8_t writeBuffer = 0;

    uint32_t advance() {
        generation = nextGeneration(generation);
        writeBuffer ^= 1u;
        return generation;
    }
};

inline uint8_t eventPriority(DefenseEvent event) {
    switch (event) {
        case DefenseEvent::DEAUTH_DETECTED:
        case DefenseEvent::EVIL_TWIN:
        case DefenseEvent::KARMA_HONEYPOT:
        case DefenseEvent::FINGERPRINT_MISMATCH:
        case DefenseEvent::TRACKER_FOLLOWING:
        case DefenseEvent::COORDINATED_ATTACK:
        case DefenseEvent::DUAL_BAND_STALK:
        case DefenseEvent::ATTACKER_IDENTIFIED:
        case DefenseEvent::KARMA_CONFIRMED:
        case DefenseEvent::CANARY_TRIPPED:
        case DefenseEvent::TOOL_IDENTIFIED:
            return 3;
        case DefenseEvent::BLE_SPAM:
        case DefenseEvent::OPEN_AP_WARNING:
        case DefenseEvent::KNOWN_AP:
        case DefenseEvent::PROBE_VULN_CLIENT:
        case DefenseEvent::SEQ_ANOMALY:
        case DefenseEvent::RSSI_ANOMALY:
        case DefenseEvent::FOLLOWING_NETWORK_ID:
        case DefenseEvent::RELAY_SUSPECT:
        case DefenseEvent::HOSTILE_CLIENT:
        case DefenseEvent::LOW_ENTROPY_BEACON:
            return 2;
        case DefenseEvent::TRACKER_NEW:
        case DefenseEvent::SCAN_COMPLETE:
            return 1;
        default:
            return 0;
    }
}

}  // namespace Defense
