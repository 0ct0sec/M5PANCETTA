/**
 * CSI Tracker - lightweight WiFi channel state intelligence
 *
 * ==[ STABILITY ]== Keep callback work to a memcpy and drain everything on task loop.
 */

#ifndef CSI_TRACKER_H
#define CSI_TRACKER_H

#include <stdint.h>

namespace CsiTracker {

enum TargetKind : uint8_t {
    TARGET_NONE = 0,
    TARGET_WIFI_CLIENT = 1,
    TARGET_WIFI_AP = 2,
    TARGET_DEAUTH_SOURCE = 3
};

struct Snapshot {
    bool valid;
    uint8_t kind;
    uint8_t mac[6];
    uint8_t channel;

    int8_t rssi;
    int8_t noiseFloor;
    uint8_t signalMode;
    uint8_t channelBandwidth;  // 0=20 MHz, 1=40 MHz
    uint8_t secondaryChannel;  // IDF: 0=none, 1=above, 2=below
    uint8_t stbc;
    uint8_t ltfMask;
    uint16_t originalLength;
    uint16_t retainedLength;
    uint16_t usablePairs;
    uint16_t lltfPairs;
    uint16_t htltfPairs;
    uint16_t stbcHtlftPairs;
    uint32_t lastSeenMs;
    uint32_t rxTimestampUs;  // hardware packet identity shared with promisc RX
    uint32_t ageMs;
    uint16_t sampleCount;

    uint8_t quality;          // capture maturity + usable subcarrier coverage
    uint8_t channelChange;    // temporal normalized-amplitude change, not motion/AoA
    uint8_t frequencySpread;  // within-frame amplitude selectivity, not multipath proof
    uint8_t stability;        // inverse temporal channel change
    uint8_t fade;             // short-term RSSI deviation from the target EMA
    uint8_t fadeShape;        // deep-fade subcarrier fraction, not distance
    uint8_t temporalCorrelation; // per-subcarrier power-shape similarity
    uint8_t confidence;
    uint16_t incompatibleLayouts;
    uint16_t droppedSamples;

    uint16_t proximity;  // 0..1000
};

bool begin();
void end();

void setTarget(TargetKind kind, const uint8_t mac[6], uint8_t channel);
void clearTarget();

void update();
bool getSnapshot(Snapshot& out);

bool isEnabled();
bool isTargetActive();

}  // namespace CsiTracker

#endif  // CSI_TRACKER_H
