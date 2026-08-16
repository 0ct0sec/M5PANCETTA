/**
 * Pedometer - Step counting via MPU6886
 *
 * ==[ STEP WATCHER ]== counts steps + motion state for adaptive hunt.
 */
#pragma once

#include <Arduino.h>

// motion state for adaptive hunt mode
enum class MotionState {
    STATIONARY,     // no movement - camp mode (aggressive capture)
    WALKING         // moving - patrol mode (balanced discovery/capture)
};

namespace Pedometer {
    enum RfPoseFlags : uint8_t {
        RF_POSE_IMU_PRESENT = 1u << 0,
        RF_POSE_CALIBRATED = 1u << 1,
        RF_POSE_STATIONARY = 1u << 2,
        RF_POSE_ROTATING = 1u << 3,
        RF_POSE_TRANSLATING = 1u << 4,
        RF_POSE_INTERPOLATED = 1u << 5,
    };

    struct RfPoseSample {
        uint32_t timestampUs = 0u;
        float ax = 0.0f;
        float ay = 1.0f;
        float az = 0.0f;
        float gx = 0.0f;
        float gy = 0.0f;
        float gz = 0.0f;
        float rollDeg = 0.0f;
        float pitchDeg = 0.0f;
        float screenYawDeg = 0.0f;
        float linearAx = 0.0f;     // screen-right gravity-free accel, g
        float linearAy = 0.0f;     // screen-up gravity-free accel, g
        float velocityX = 0.0f;    // bounded origin-relative world m/s
        float velocityY = 0.0f;
        float positionX = 0.0f;    // bounded origin-relative world metres
        float positionY = 0.0f;
        float yawDriftDeg = 180.0f;
        uint16_t interpolationAgeUs = 0xffffu;
        uint8_t quality = 0u;
        uint8_t flags = 0u;
        uint8_t yawPose = 0u;      // 1 flat, 2..5 latched upright edge
    };

    // init IMU
    void init();

    // active display tick
    void update();

    // background update (always counting)
    void updateBackground();

    // stats
    uint32_t getSteps();
    uint32_t getDistance();      // meters
    float getDistanceKm();       // kilometers
    uint32_t getCalories();

    // walking detection (for auto-hunt)
    bool isWalking();
    bool isVisuallyWalking();       // 4+ steps in 3s for animation trigger
    uint32_t getWalkingDuration();  // ms since started walking
    bool shouldAutoHunt();          // true once after 30s walking
    void resetAutoHunt();           // reset trigger flag
    // ==[ MOTION STATE ]== for adaptive hunt
    MotionState getMotionState();
    uint32_t getStationaryDuration();   // ms since became stationary
    float getStepsPerSecond();          // recent step rate (last 10s)
    // sniff trigger (for client detection)
    void requestSniff();
    bool consumeSniffRequest();

    // reset session
    void resetSession();

    // ==[ IMU CACHE ]== one read per frame, all consumers share
    void getCachedAccel(float& ax, float& ay, float& az);
    void getCachedGyro(float& gx, float& gy, float& gz);
    bool hasIMU();
    // ==[ ORIENTATION CACHE ]== unified flat/upright with hysteresis
    // Enter FLT: |az| > 0.75, Exit FLT: |az| < 0.65 (0.1g deadband)
    bool isCachedFlat();

    // ==[ RF POSE ]== CoreS3 SE uses a dedicated ~100 Hz sensor task while
    // THRU/RAD is active. Core2 and no-IMU builds retain the neutral fallback.
    void setRfPoseActive(bool active);
    void resetRfPoseOrigin();
    bool getRfPoseAt(uint32_t rfTimestampUs, RfPoseSample& out);
    bool getLatestRfPose(RfPoseSample& out);
}
