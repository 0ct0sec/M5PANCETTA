/**
 * Bearing Tracker - Gyro PDR with RSSI Gradient Lock
 *
 * ==[ DIRECTION FINDER ]== feed it RSSI, get bearing to target.
 * 
 * shared by RF Scope client tracking and PIG EARS BLE tracking.
 * Evidence limits and tuning are documented in docs/firmware/peripherals.md.
 */
#pragma once

#include <Arduino.h>

namespace Bearing {

// ==[ RF POINT CLOUD ]== passive samples tagged with pose. no malloc.
static constexpr uint8_t RF_POINT_MAX = 192;

enum RfEvidenceFlags : uint8_t {
    RF_EVIDENCE_PASSIVE = 0x01,
    RF_EVIDENCE_ACTIVE  = 0x02,  // reserved for gated lab/active probes
    RF_EVIDENCE_CSI     = 0x04,  // reserved for ESP-IDF CSI bench path
    RF_EVIDENCE_REMOTE  = 0x08   // timestamped observation from C5/JanOS
};

enum class EstimateState : uint8_t {
    SEEK = 0u,
    COARSE = 1u,
    LOCK = 2u,
    AMBIG = 3u,
};

struct RfPoint {
    uint32_t seenMs = 0;
    uint16_t headingDegX10 = 0;   // device-relative yaw sample, 0..3599
    int16_t elevDegX10 = 0;       // tilt/pull sample, approx -900..+900
    int16_t bearing = 0;          // -100..+100 screen bearing at sample time
    int8_t rssi = -127;
    uint8_t strength = 0;         // -95..-25dBm mapped to 0..100
    uint8_t confidence = 0;
    uint8_t evidenceFlags = RF_EVIDENCE_PASSIVE;
    uint8_t csiQuality = 0;       // 0 until CSI is explicitly enabled
    uint8_t poseQuality = 0;
    uint8_t poseFlags = 0;
    uint16_t poseAgeUs = 0xffffu;
    int16_t yawDriftDegX10 = 1800;
    bool behind = false;
    bool moving = false;
};

struct ObservationPose {
    bool valid = false;
    uint16_t headingDegX10 = 0;
    int16_t elevationDegX10 = 0;
    uint8_t quality = 0;
    uint8_t flags = 0;
    uint16_t interpolationAgeUs = 0xffffu;
    int16_t yawDriftDegX10 = 1800;
};

struct AuthoritativePoseSample {
    uint32_t timestampUs = 0u;
    float headingDeg = 0.0f;
    float linearAx = 0.0f;     // screen-right gravity-free accel, g
    float linearAy = 0.0f;     // screen-up gravity-free accel, g
    float velocityX = 0.0f;    // bounded origin-relative world m/s
    float velocityY = 0.0f;
    float positionX = 0.0f;    // bounded origin-relative world metres
    float positionY = 0.0f;
    bool stationary = false;
};

// ==[ TRACKER STATE ]== all the bits needed for bearing calculation
struct TrackerState {
    // Gyro integration
    float relativeHeading = 0.0f;     // cumulative rotation (0-360)
    float gyroBias = 0.0f;            // calibrated bias (deg/s)
    bool gyroCalibrated = false;
    uint32_t lastGyroUpdate = 0;
    
    // RSSI trend detection (10 samples for better gradient)
    int8_t rssiHistory[10] = {0};
    uint8_t rssiHistoryIdx = 0;
    uint8_t rssiHistoryCount = 0;
    int8_t rssiTrend = 0;             // raw trend
    int8_t rssiTrendSmooth = 0;       // EMA smoothed
    float prevFeedRssi = -128.0f;     // for EMA-delta early lock
    float deltaEma = 0.0f;            // smoothed pairwise delta
    
    // Motion detection (variance-based)
    float accelHistory[8] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    uint8_t accelIdx = 0;
    bool motionState = false;
    uint32_t lastHighVariance = 0;
    uint32_t motionStartTime = 0;
    uint32_t lastMotionTime = 0;
    float accelMotionEnergy = 0.0f;  // 0..100 filtered translation energy
    float motionLastGx = 0.0f;
    float motionLastGy = 0.0f;
    float motionLastGz = 0.0f;
    bool motionSamplePrimed = false;
    uint32_t lastMotionSampleTime = 0;
    uint8_t stationaryConfidence = 0; // 0..100, accel + gyro quiet confidence
    float observerVelocityX = 0.0f;
    float observerVelocityY = 0.0f;
    float observerPositionX = 0.0f;
    float observerPositionY = 0.0f;
    float thruLastAx = 0.0f;
    float thruLastAy = 0.0f;
    float thruLastAz = 1.0f;
    bool thruAccelPrimed = false;
    int16_t thruScanX = 0;           // -100..+100, accel-derived THRU sweep
    int16_t thruScanY = 0;           // -100..+100, accel-derived THRU sweep
    uint8_t thruMotionHeat = 0;      // 0..100, decaying observer motion energy
    uint32_t lastAuthoritativePoseTimestampUs = 0u;
    uint32_t lastAuthoritativePoseUpdateMs = 0u;
    bool authoritativePosePrimed = false;
    uint16_t lastHeadingDegX10 = 0;  // pose tag for RF point cloud
    int16_t lastElevDegX10 = 0;      // pose tag for RF point cloud
    RfPoint rfPoints[RF_POINT_MAX];  // newest at rfPointHead - 1
    uint8_t rfPointHead = 0;
    uint8_t rfPointCount = 0;
    
    // Bearing lock
    float lockedHeading = 0.0f;       // heading when locked
    bool bearingLocked = false;
    uint8_t lockConfidence = 0;       // 0-100%
    uint32_t lastReinforceTime = 0;
    uint32_t lastDecay = 0;
    uint32_t lastFeedTime = 0;        // any scalar RSSI arrival
    uint32_t lastDirectionalFeedTime = 0; // point/sweep evidence only
    uint32_t approachConfirmCount = 0;
    uint32_t lockGeneration = 0;      // accepted-sweep token; survives reset()
    
    // Rotation lock (STI mode)
    float peakRssi = -127.0f;
    float peakHeading = 0.0f;
    bool wasRotating = false;
    uint32_t rotationStartTime = 0;
    float rotationLastHeading = 0.0f;
    float rotationTravelDeg = 0.0f;
    uint8_t rotationRfSamples = 0;
    int8_t rotationBinPeak[12] = {};
    float rotationBinHeading[12] = {};
    uint8_t rotationBinSamples[12] = {};
    int16_t rotationBinMeanX8[12] = {};
    uint16_t rotationBinDeviationX8[12] = {};
    uint16_t rotationBinQualitySum[12] = {};
    float rotationFitAmplitude = 0.0f;
    float rotationFitResidual = 0.0f;
    float rotationAmbiguity = 0.0f;
    EstimateState estimateState = EstimateState::SEEK;
    
    // Orientation tracking
    uint8_t yawPose = 0;               // 0 unknown, 1 flat, 2..5 upright edge
    
    // Calibration accumulator
    float biasSum = 0.0f;
    uint16_t biasSamples = 0;
    
    // Output
    int32_t bearing_x256 = 0;         // x256 fixed-point for IIR (zero bias)
    int16_t bearing = 0;              // -100 to +100 (screen position)
    int16_t bearingRaw = 0;
    uint8_t bearingConfidence = 0;
    bool porkBehind = false;
    bool isMoving = false;
    bool isFlat = false;
};

// ==[ TRACKER CONFIG ]== tuning parameters
struct TrackerConfig {
    // Calibration
    float stillMin = 0.95f;           // accelMag lower bound
    float stillMax = 1.05f;           // accelMag upper bound
    uint16_t calSamples = 50;         // samples for calibration
    
    // Motion detection
    float varianceEnter = 0.0008f;    // enter MOT threshold
    float varianceExit = 0.0003f;     // exit to STI threshold
    uint32_t motionHoldMs = 300;      // hold time before exit
    uint32_t stationarySettleMs = 300; // min settle time before stationary confidence hits 100%
    uint8_t stationaryBoostWhileIdle = 20; // extra confidence bonus when no motion
    
    // Rotation
    float rotationThreshold = 8.0f;   // deg/s for rotation detection
    uint32_t minRotationMs = 500;     // minimum rotation for lock
    uint8_t minRotationDegrees = 60;  // minimum sampled angular travel
    uint8_t minRotationSamples = 5;   // minimum RF samples during sweep
    uint8_t minRotationBins = 4;      // minimum distinct 30-degree sectors
    uint8_t minPeakSeparationDb = 3;  // peak must beat a non-adjacent direction
    
    // Lock
    int8_t trendThreshold = 3;        // |trend| >= 3 for lock (sim: 37% FPR, 53% TPR)
    uint32_t minMotionMs = 800;       // motion duration before lock
    uint8_t initialConfidence = 70;
    uint8_t reinforceBoost = 5;
    uint8_t minConfidence = 15;       // below this, bearing not shown
    
    // Decay
    uint32_t decayRateWrong = 100;    // ms per point when moving wrong
    uint32_t decayRateStale = 500;    // ms per point when stale
    uint32_t staleTimeout = 60000;    // ms before considered stale
    
    // Smoothing
    uint8_t iirRatioFlat = 4;
    uint8_t iirRatioUpright = 3;

    // Cadence-aware (0 = disabled, WiFi default)
    uint32_t expectedCadenceMs = 0;   // expected sample interval for decay freeze
    float deltaEmaAlpha = 0.3f;       // EMA alpha for pairwise delta trend
    int8_t deltaEmaScale = 3;         // scale factor for delta trend output
};

// ==[ POSE YAW ]== clockwise-positive screen heading for the current grip.
float yawRateForPose(bool isFlat,
                     float gx, float gy, float gz,
                     float ax, float ay);

/**
 * Update IMU state — call every frame (~40Hz)
 * Orientation, gyro cal, heading integration, motion/rotation detection,
 * bearing output (dot position updates smoothly when turning)
 *
 * @param now  current time in milliseconds (pass millis() or mock value)
 */
void updateIMU(TrackerState& state, const TrackerConfig& config,
               float gx, float gy, float gz,
               float ax, float ay, float az,
               uint32_t now = 0);

/**
 * Update IMU-derived motion and rotation state while accepting an
 * authoritative screen-relative heading. The supplied heading replaces local
 * gyro integration, so callers must not pre-integrate it into relativeHeading.
 */
void updateIMUAuthoritativeHeading(
    TrackerState& state, const TrackerConfig& config,
    float gx, float gy, float gz,
    float ax, float ay, float az,
    float authoritativeHeadingDeg,
    uint32_t now = 0);

/**
 * Consume one timestamped high-rate pose sample. Repeated or out-of-order
 * timestamps still refresh bearing/decay output, but cannot integrate motion
 * twice. Raw render-cadence accel translation is bypassed.
 */
void updateIMUAuthoritativePose(
    TrackerState& state, const TrackerConfig& config,
    float gx, float gy, float gz,
    float ax, float ay, float az,
    const AuthoritativePoseSample& pose,
    uint32_t now = 0);

/**
 * Feed new RSSI sample — call ONLY when a new beacon/frame arrives
 * Trend window, lock logic (walking + rotation), confidence decay
 *
 * @param now  current time in milliseconds (pass millis() or mock value)
 */
void feedRSSI(TrackerState& state, const TrackerConfig& config,
              int8_t rssi, int8_t rssiSmooth,
              uint32_t now = 0);

/**
 * Feed a real RF arrival while preserving its observation timestamp.
 * Processing/lock timers use now; the retained cloud point uses observedMs.
 */
void feedRSSIObserved(TrackerState& state, const TrackerConfig& config,
                      int8_t rssi, int8_t rssiSmooth,
                      uint32_t observedMs, uint32_t now,
                       uint8_t evidenceFlags = RF_EVIDENCE_PASSIVE,
                       uint8_t csiQuality = 0);

void feedRSSIObservedPose(
    TrackerState& state, const TrackerConfig& config,
    int8_t rssi, int8_t rssiSmooth,
    uint32_t observedMs, uint32_t now,
    const ObservationPose& pose,
    uint8_t evidenceFlags = RF_EVIDENCE_PASSIVE,
    uint8_t csiQuality = 0);

/**
 * Feed a real scalar RSSI arrival without treating it as directional
 * evidence. Updates cadence/trend state but adds no point-cloud or rotation
 * sample.
 */
void feedRSSIScalarObserved(
    TrackerState& state, const TrackerConfig& config,
    int8_t rssi, int8_t rssiSmooth,
    uint32_t observedMs, uint32_t now);

/**
 * Legacy wrapper — calls updateIMU() then feedRSSI()
 * Fine for callers with 1:1 IMU-to-RSSI ratio
 *
 * @param now  current time in milliseconds (pass millis() or mock value)
 */
void update(TrackerState& state, const TrackerConfig& config,
            int8_t rssi, int8_t rssiSmooth,
            float gx, float gy, float gz,
            float ax, float ay, float az,
            uint32_t now = 0);

/**
 * Reset tracker state (call when switching targets or modes)
 */
void reset(TrackerState& state);

uint8_t getRfPointCount(const TrackerState& state);
bool getRfPointNewest(const TrackerState& state, uint8_t newestIdx,
                      RfPoint& out);
bool addNewestRfPointEvidence(TrackerState& state, uint8_t evidenceFlags,
                              uint8_t csiQuality = 0);

}  // namespace Bearing
