/**
 * Geiger scan view - shared RAD/THRU UI bits.
 *
 * ==[ THRU MAP ]== Wildcard posture view for upright signal hunts.
 */
#pragma once

#include <M5Unified.h>
#include <stdint.h>

namespace Bearing {
struct TrackerState;
}

namespace GeigerScanView {

// Describes the current evidence source, not a universal accuracy rank.
// In particular, CSI can characterize stability but cannot supply left/right
// angle of arrival on the present single-antenna hardware.
enum class RfEvidenceGrade : uint8_t {
    NONE,
    SCAN_SNAPSHOT,
    PACKET_STREAM,
    CSI_STABILITY,
    FTM_RANGE,
};

struct ThroughTarget {
    const char* header;
    const char* scope;
    const char* evidenceLabel; // short source truth: RX, C5 SNAP, etc.
    RfEvidenceGrade evidenceGrade;
    int8_t rssi;
    uint16_t proximity;      // 0..1000
    int16_t bearing;         // -100..+100, screen-space
    uint8_t confidence;      // 0..100
    int8_t trend;
    uint32_t ageMs;
    uint16_t dotCount;
    uint8_t historyDensity;    // 0..100 recent RF point density (rolling window)
    uint8_t historyConsistency; // 0..100 heading continuity in recent RF samples
    uint8_t historyCadence;   // 0..100 sample cadence health in recent RF samples
    uint8_t historyConfidence; // 0..100 derived confidence from recent history
    bool locked;
    bool behind;
    bool moving;
    bool flat;
    int8_t motionScreenSign; // display rotation for motion only, never bearing
    int16_t scanX;           // -100..+100 accel-derived THRU sweep
    int16_t scanY;           // -100..+100 accel-derived THRU sweep
    uint8_t motionHeat;      // 0..100 IMU observer-motion energy
    uint8_t stationaryConfidence; // 0..100 IMU quiet confidence
    int16_t sceneScanX;      // -100..+100 RF residual shape axis
    int16_t sceneScanY;      // -100..+100 RF residual shape axis
    uint8_t sceneMotionHeat; // 0..100 motion evidence after IMU gating
    const Bearing::TrackerState* tracker;
    int16_t fallbackBearing;  // -100..+100 when gyro lock is still cold
    uint16_t seekHeadingDegX10; // scan-entry IMU reference; not RF direction
    bool seekHeadingValid;
    uint16_t lastKnownHeadingDegX10;
    int16_t lastKnownElevDegX10;
    uint16_t lastKnownProximity;
    float lastKnownObserverX;
    float lastKnownObserverY;
    uint32_t lastKnownAgeMs;
    bool lastKnownValid;
    // RAD always keeps its last qualified plot in the world frame. The
    // user-configurable ghost marker still controls THRU's stale overlay.
    bool radarAnchorValid = false;
    int8_t lastKnownRssi = -127;
    bool csiValid = false;   // CSI telemetry present for this frame
    bool csiWaiting = false; // CSI target armed but no usable sample yet
    uint32_t csiAgeMs = 0;   // age of the CSI evidence, independent of RSSI age
    uint8_t csiQuality = 0;  // 0..100 normalized CSI usability
    uint8_t csiChannelChange = 0; // 0..100 temporal channel-amplitude change
    uint8_t csiFrequencySpread = 0; // 0..100 frequency selectivity, not AoA
    uint8_t csiStability = 0;  // 0..100 temporal stability
    uint8_t csiFade = 0;       // 0..100 short-term RSSI deviation
    bool ftmResponder = false;
    bool ftmActive = false;
    bool ftmValid = false;
    uint32_t ftmDistanceCm = 0;
    uint32_t ftmVarianceCm2 = 0;
    uint16_t ftmSampleCount = 0;
    uint32_t ftmAgeMs = 0;
    bool gpsRouteValid = false;
    uint32_t gpsEvidenceAgeMs = 0;
    uint16_t gpsFixAgeMs = 0;
    uint8_t gpsHdopX10 = 0;
    uint8_t gpsRouteSamples = 0;
    uint16_t gpsStrongestRadiusM = 0;
    uint16_t band24Count = 0;  // independent observed 2.4GHz carrier census
    uint16_t band5Count = 0;   // independent observed 5GHz carrier census
    uint16_t channelCount = 0; // co-channel AP census; never packet activity
    uint8_t channel = 0;
    uint32_t channelPps = 0;   // all traffic on channel, not target traffic
    bool channelPpsValid = false;
    const uint16_t* channelPpsHistory = nullptr;
    uint8_t channelPpsHistoryCount = 0;
    bool csiUnsupported = false;
};

void drawThroughScanner(M5Canvas& canvas,
                        int boxX, int boxY, int boxW, int boxH,
                        uint16_t fg, uint16_t bg,
                        const ThroughTarget& target);

void drawRadarScanner(M5Canvas& canvas,
                      int boxX, int boxY, int boxW, int boxH,
                      uint16_t fg, uint16_t bg,
                      const ThroughTarget& target);

}  // namespace GeigerScanView
