/**
 * Pedometer - Implementation using MPU6886
 *
 * ==[ STEP ENGINE ]== motion state + milestones for adaptive hunt.
 */

#include "pedometer.h"
#include <M5Unified.h>
#include <atomic>
#include "../core/config.h"
#include "../core/constants.h"
#include "../ui/display.h"
#include "../piglet/mood.h"
#include "../util/debug_log.h"
#include "../util/rf_pose_math.h"

namespace Pedometer {

// ==[ STEP DETECTION ]==
static float lastMagnitude = 0;
static float threshold = 1.6;  // 1.6g. fewer ghosts.
static bool stepPending = false;
static uint32_t lastStepTime = 0;
static const uint32_t STEP_DEBOUNCE = 250;  // 250ms. no double taps.
// ==[ SHAKE-TO-WAKE ]==
static const float SHAKE_THRESHOLD = 2.2;  // 2.2g spike wakes screen
static uint32_t lastShakeTime = 0;
static const uint32_t SHAKE_DEBOUNCE = 500;  // calm down.
// ==[ SESSION STATS ]==
static uint32_t sessionSteps = 0;
static uint32_t persistedThisSession = 0;  // ram counter. batch to nvs.
// ==[ IMU CACHE ]== one read per frame, shared by all consumers
static float cachedAx = 0, cachedAy = 0, cachedAz = 0;
static float cachedGx = 0, cachedGy = 0, cachedGz = 0;
// ==[ IMU PRESENCE ]== latched at init. Core2 has an MPU6886 onboard;
// CoreS3 SE has none and depends on a stacked base for one, so this is a
// runtime fact, not a build-time one. See docs/reference/cores3se-port.md.
static bool imuPresent = false;
// ==[ ORIENTATION CACHE ]== unified flat/upright with hysteresis
// Enter FLT: |az| > 0.75, Exit FLT: |az| < 0.65 (0.1g deadband)
static bool cachedIsFlat = false;
// ==[ WALK DETECTION ]== drives auto-hunt
static uint32_t walkingStartTime = 0;  // walk session start
static uint32_t lastStepEventTime = 0; // last step timestamp
static const uint32_t WALKING_TIMEOUT = 5000;  // 5s idle = walk ended
static const uint32_t AUTO_HUNT_THRESHOLD = 30000;  // 30s walk = auto hunt fires
static bool isCurrentlyWalking = false;
static bool autoHuntTriggered = false;  // once per session. no spam.
// ==[ VISUAL WALK ]== animation only, filters shakes
static bool visuallyWalking = false;
static const uint32_t VISUAL_WALK_MIN_STEPS = 4;    // need 4 rhythmic steps
static const uint32_t VISUAL_WALK_WINDOW_MS = 3000; // within 3 seconds
static const uint32_t VISUAL_WALK_TIMEOUT = 2500;   // 2.5s to stop animation
// ==[ MOTION STATE ]==
static MotionState currentMotionState = MotionState::STATIONARY;
static uint32_t stationaryStartTime = 0;
// stationary timeout + min steps from config
static int recentStepCount = 0;      // steps in current burst
static uint32_t burstStartTime = 0;  // burst start time
// ==[ STEP RATE WINDOW ]==
static const int STEP_WINDOW_SIZE = 10;
static uint32_t stepTimestamps[STEP_WINDOW_SIZE];
static int stepWindowIdx = 0;
static int stepWindowCount = 0;

// ==[ SNIFF TRIGGER ]== for client detection animation
static bool sniffRequested = false;

#if defined(HAMLET_CORE3SE)
static constexpr uint8_t RF_POSE_RING_SIZE = 64u;
static constexpr uint32_t RF_POSE_PERIOD_US = 10000u;
static constexpr uint32_t RF_POSE_MAX_LOOKUP_AGE_US = 120000u;
static RfPoseSample rfPoseRing[RF_POSE_RING_SIZE];
static uint8_t rfPoseHead = 0u;
static uint8_t rfPoseCount = 0u;
static std::atomic<bool> rfPoseActive{false};
static std::atomic<bool> rfPoseResetRequested{false};
static TaskHandle_t rfPoseTaskHandle = nullptr;
static portMUX_TYPE rfPoseMux = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE imuCacheMux = portMUX_INITIALIZER_UNLOCKED;

static float rfBiasGx = 0.0f;
static float rfBiasGy = 0.0f;
static float rfBiasGz = 0.0f;
static uint16_t rfBiasSamples = 0u;
static float rfGravityX = 0.0f;
static float rfGravityY = 1.0f;
static float rfGravityZ = 0.0f;
static float rfRollDeg = 0.0f;
static float rfPitchDeg = 0.0f;
static float rfScreenYawDeg = 0.0f;
static float rfVelocityX = 0.0f;
static float rfVelocityY = 0.0f;
static float rfPositionX = 0.0f;
static float rfPositionY = 0.0f;
static uint8_t rfYawPose = 0u;
static bool rfGravityPrimed = false;
static uint32_t rfLastSampleUs = 0u;

static float wrapDegrees(float value) {
    while (value >= 360.0f) value -= 360.0f;
    while (value < 0.0f) value += 360.0f;
    return value;
}

static float shortestDegrees(float from, float to) {
    float delta = to - from;
    while (delta > 180.0f) delta -= 360.0f;
    while (delta < -180.0f) delta += 360.0f;
    return delta;
}

static float screenYawRate(bool flat, float gx, float gy, float gz,
                           float ax, float ay) {
    if (flat) return -gz;
    const float uprightG = sqrtf(ax * ax + ay * ay);
    if (uprightG < 0.001f) return 0.0f;
    return (gx * ax + gy * ay) / uprightG;
}

static void clearRfPoseRingLocked() {
    rfPoseHead = 0u;
    rfPoseCount = 0u;
}

static void resetRfPoseIntegrator() {
    rfScreenYawDeg = 0.0f;
    rfVelocityX = 0.0f;
    rfVelocityY = 0.0f;
    rfPositionX = 0.0f;
    rfPositionY = 0.0f;
    rfYawPose = 0u;
    rfGravityPrimed = false;
    rfLastSampleUs = 0u;
}

static void appendRfPose(const RfPoseSample& sample) {
    portENTER_CRITICAL(&rfPoseMux);
    if (rfPoseActive.load(std::memory_order_acquire)) {
        rfPoseRing[rfPoseHead] = sample;
        rfPoseHead = (uint8_t)((rfPoseHead + 1u) % RF_POSE_RING_SIZE);
        if (rfPoseCount < RF_POSE_RING_SIZE) ++rfPoseCount;
    }
    portEXIT_CRITICAL(&rfPoseMux);
}

static void rfPoseTask(void*) {
    TickType_t wake = xTaskGetTickCount();
    for (;;) {
        if (!rfPoseActive.load(std::memory_order_acquire) || !imuPresent) {
            rfLastSampleUs = 0u;
            vTaskDelay(pdMS_TO_TICKS(20));
            wake = xTaskGetTickCount();
            continue;
        }
        if (rfPoseResetRequested.exchange(
                false, std::memory_order_acq_rel)) {
            resetRfPoseIntegrator();
        }

        float ax = 0.0f;
        float ay = 1.0f;
        float az = 0.0f;
        float gx = 0.0f;
        float gy = 0.0f;
        float gz = 0.0f;
        if (!M5.Imu.getAccel(&ax, &ay, &az) ||
            !M5.Imu.getGyro(&gx, &gy, &gz)) {
            vTaskDelayUntil(
                &wake, pdMS_TO_TICKS(RF_POSE_PERIOD_US / 1000u));
            continue;
        }

        const uint32_t timestampUs = micros();
        float dt = 0.01f;
        if (rfLastSampleUs != 0u) {
            const uint32_t elapsedUs = timestampUs - rfLastSampleUs;
            if (elapsedUs >= 4000u && elapsedUs <= 50000u) {
                dt = (float)elapsedUs / 1000000.0f;
            }
        }
        rfLastSampleUs = timestampUs;

        const float accelMagnitude =
            sqrtf(ax * ax + ay * ay + az * az);
        const float gyroMagnitude =
            sqrtf(gx * gx + gy * gy + gz * gz);
        const bool stationary =
            fabsf(accelMagnitude - 1.0f) < 0.035f &&
            gyroMagnitude < 3.5f;
        if (stationary) {
            const float alpha = rfBiasSamples < 50u ? 0.08f : 0.005f;
            rfBiasGx += (gx - rfBiasGx) * alpha;
            rfBiasGy += (gy - rfBiasGy) * alpha;
            rfBiasGz += (gz - rfBiasGz) * alpha;
            if (rfBiasSamples < 65535u) ++rfBiasSamples;
        }

        const float correctedGx = gx - rfBiasGx;
        const float correctedGy = gy - rfBiasGy;
        const float correctedGz = gz - rfBiasGz;
        portENTER_CRITICAL(&imuCacheMux);
        const bool flat =
            RfPoseMath::hystereticFlat(cachedIsFlat, az);
        cachedIsFlat = flat;
        portEXIT_CRITICAL(&imuCacheMux);
        const uint8_t yawPose = RfPoseMath::yawPoseForGravity(
            flat, ax, ay, rfYawPose);
        const bool poseChanged = yawPose != rfYawPose;
        rfYawPose = yawPose;
        const float yawRate = RfPoseMath::boundedYawRateDps(
            screenYawRate(flat, correctedGx, correctedGy, correctedGz, ax, ay));
        // Keep the world yaw continuous between RAD and THRU, but do not let
        // a grip-axis handoff or a one-sample gyro spike kick the plot past a
        // real handset turn.
        rfScreenYawDeg = wrapDegrees(
            rfScreenYawDeg + RfPoseMath::boundedYawDeltaDeg(
                                  yawRate, dt, poseChanged));

        // Six-axis complementary fusion: gyro carries short-term orientation;
        // the gravity vector corrects roll/pitch without claiming absolute yaw.
        const float accelRoll =
            atan2f(ay, az) * 57.2957795f;
        const float accelPitch =
            atan2f(-ax, sqrtf(ay * ay + az * az)) * 57.2957795f;
        rfRollDeg =
            (rfRollDeg + correctedGx * dt) * 0.98f +
            accelRoll * 0.02f;
        rfPitchDeg =
            (rfPitchDeg + correctedGy * dt) * 0.98f +
            accelPitch * 0.02f;

        if (!rfGravityPrimed || poseChanged) {
            // Grip changes rotate gravity into a different device axis. Prime
            // from the current vector so that rotation is not misreported as
            // translation.
            rfGravityX = ax;
            rfGravityY = ay;
            rfGravityZ = az;
            rfGravityPrimed = true;
        } else {
            const float gravityAlpha = stationary ? 0.08f : 0.015f;
            rfGravityX += (ax - rfGravityX) * gravityAlpha;
            rfGravityY += (ay - rfGravityY) * gravityAlpha;
            rfGravityZ += (az - rfGravityZ) * gravityAlpha;
        }
        const float deviceLinearX = ax - rfGravityX;
        const float deviceLinearY = ay - rfGravityY;
        const float deviceLinearZ = az - rfGravityZ;
        float linearAx = 0.0f;
        float linearAy = 0.0f;
        RfPoseMath::deviceToScreen(
            flat, yawPose,
            deviceLinearX, deviceLinearY, deviceLinearZ,
            linearAx, linearAy);
        const float linearMagnitude =
            sqrtf(linearAx * linearAx + linearAy * linearAy);
        const bool translating =
            !stationary && !poseChanged && linearMagnitude > 0.055f;
        float worldLinearX = 0.0f;
        float worldLinearY = 0.0f;
        RfPoseMath::screenToWorld(
            rfScreenYawDeg, linearAx, linearAy,
            worldLinearX, worldLinearY);
        if (stationary || poseChanged) {
            // Zero-velocity update bounds double-integration drift.
            rfVelocityX = 0.0f;
            rfVelocityY = 0.0f;
        } else if (translating) {
            rfVelocityX = constrain(
                rfVelocityX + worldLinearX * 9.80665f * dt,
                -2.0f, 2.0f);
            rfVelocityY = constrain(
                rfVelocityY + worldLinearY * 9.80665f * dt,
                -2.0f, 2.0f);
        } else {
            const float damping = 1.0f / (1.0f + dt * 8.0f);
            rfVelocityX *= damping;
            rfVelocityY *= damping;
        }
        rfPositionX = constrain(
            rfPositionX + rfVelocityX * dt, -3.0f, 3.0f);
        rfPositionY = constrain(
            rfPositionY + rfVelocityY * dt, -3.0f, 3.0f);

        RfPoseSample sample{};
        sample.timestampUs = timestampUs;
        sample.ax = ax;
        sample.ay = ay;
        sample.az = az;
        sample.gx = correctedGx;
        sample.gy = correctedGy;
        sample.gz = correctedGz;
        sample.rollDeg = rfRollDeg;
        sample.pitchDeg = rfPitchDeg;
        sample.screenYawDeg = rfScreenYawDeg;
        sample.linearAx = linearAx;
        sample.linearAy = linearAy;
        sample.velocityX = rfVelocityX;
        sample.velocityY = rfVelocityY;
        sample.positionX = rfPositionX;
        sample.positionY = rfPositionY;
        sample.yawPose = yawPose;
        sample.flags = RF_POSE_IMU_PRESENT;
        if (rfBiasSamples >= 50u) sample.flags |= RF_POSE_CALIBRATED;
        if (stationary) sample.flags |= RF_POSE_STATIONARY;
        if (fabsf(yawRate) > 8.0f) sample.flags |= RF_POSE_ROTATING;
        if (translating) sample.flags |= RF_POSE_TRANSLATING;
        sample.yawDriftDeg = rfBiasSamples >= 50u
            ? constrain(0.5f + fabsf(yawRate) * 0.002f, 0.5f, 12.0f)
            : 180.0f;
        int quality = 100;
        if ((sample.flags & RF_POSE_CALIBRATED) == 0u) quality -= 35;
        if (translating) quality -= 15;
        if (sample.yawDriftDeg > 5.0f) quality -= 15;
        sample.quality = (uint8_t)constrain(quality, 0, 100);

        portENTER_CRITICAL(&imuCacheMux);
        cachedAx = ax;
        cachedAy = ay;
        cachedAz = az;
        cachedGx = correctedGx;
        cachedGy = correctedGy;
        cachedGz = correctedGz;
        portEXIT_CRITICAL(&imuCacheMux);
        if (rfPoseActive.load(std::memory_order_acquire)) {
            appendRfPose(sample);
        }
        vTaskDelayUntil(
            &wake, pdMS_TO_TICKS(RF_POSE_PERIOD_US / 1000u));
    }
}
#endif

// step constants from constants.h (single source of truth)

void init() {
    // ==[ IMU PROBE ]== Pass the live internal bus explicitly. On CoreS3 SE,
    // M-Bus pins 17/18 land on In_I2C G12/G11 and reach the Bottom2 MPU6886.
    // The probe is deliberately here (after M-Bus power-up), not in M5.begin.
    // Latch the answer once — a base cannot be attached mid-run.
    const bool imuStarted = M5.Imu.begin(&M5.In_I2C, M5.getBoard());
    imuPresent = imuStarted && M5.Imu.isEnabled();
    HAMLET_LOGF("[IMU] %s (type %d)\n",
                imuPresent ? "detected" : "ABSENT - using level fallback",
                (int)M5.Imu.getType());

    // Prime the shared cache now. The boot state machine can service a serial
    // diagnostic before the first 20 Hz background poll; never expose a
    // misleading all-zero (0 g) sample during that window.
    if (!imuPresent ||
        !M5.Imu.getAccel(&cachedAx, &cachedAy, &cachedAz) ||
        !M5.Imu.getGyro(&cachedGx, &cachedGy, &cachedGz)) {
        cachedAx = 0.0f;
        cachedAy = 1.0f;
        cachedAz = 0.0f;
        cachedGx = cachedGy = cachedGz = 0.0f;
    }
    cachedIsFlat = fabsf(cachedAz) > 0.75f;

    sessionSteps = 0;
    persistedThisSession = 0;
    lastMagnitude = 1.0;  // 1g at rest. physics.
    walkingStartTime = 0;
    lastStepEventTime = 0;
    isCurrentlyWalking = false;
    autoHuntTriggered = false;
    sniffRequested = false;
    visuallyWalking = false;
    
    // Start with an empty case: motion must earn the WALKING transition.
    currentMotionState = MotionState::STATIONARY;
    stationaryStartTime = millis();
    stepWindowCount = 0;
    stepWindowIdx = 0;
    memset(stepTimestamps, 0, sizeof(stepTimestamps));

#if defined(HAMLET_CORE3SE)
    rfPoseActive.store(false, std::memory_order_release);
    rfPoseResetRequested.store(true, std::memory_order_release);
    rfBiasGx = rfBiasGy = rfBiasGz = 0.0f;
    rfBiasSamples = 0u;
    rfGravityX = cachedAx;
    rfGravityY = cachedAy;
    rfGravityZ = cachedAz;
    rfRollDeg = 0.0f;
    rfPitchDeg = 0.0f;
    portENTER_CRITICAL(&rfPoseMux);
    clearRfPoseRingLocked();
    portEXIT_CRITICAL(&rfPoseMux);
    if (imuPresent && rfPoseTaskHandle == nullptr) {
        const BaseType_t taskResult = xTaskCreatePinnedToCore(
            rfPoseTask, "rf_pose", 4096, nullptr, 1,
            &rfPoseTaskHandle, 1);
        if (taskResult != pdPASS) {
            rfPoseTaskHandle = nullptr;
            HAMLET_LOGLN("[IMU] RF pose task creation failed");
        }
    }
#endif
}

void update() {
    // Foreground callers share the same sensor path as the background tick.
    updateBackground();
}

// ==[ IMU POLL THROTTLE ]== 20Hz is plenty for 1-3Hz human cadence.
// saves ~100+ I2C reads/sec at idle. step debounce is 250ms anyway.
static uint32_t lastImuPoll = 0;
static const uint32_t IMU_POLL_INTERVAL = 50;  // 50ms = 20Hz

void updateBackground() {
    uint32_t now = millis();

    // Sensor reads are throttled; state timeouts still advance every call.
    if (now - lastImuPoll < IMU_POLL_INTERVAL) {
        // Skip the bus, not the clocks that close a walking interval.
        goto check_timeouts;
    }
    lastImuPoll = now;

    {
    float ax, ay, az;
    if (imuPresent) {
#if defined(HAMLET_CORE3SE)
        if (rfPoseActive.load(std::memory_order_acquire)) {
            portENTER_CRITICAL(&imuCacheMux);
            ax = cachedAx;
            ay = cachedAy;
            az = cachedAz;
            portEXIT_CRITICAL(&imuCacheMux);
        } else {
            M5.Imu.getAccel(&ax, &ay, &az);
            M5.Imu.getGyro(&cachedGx, &cachedGy, &cachedGz);
        }
#else
        M5.Imu.getAccel(&ax, &ay, &az);
        M5.Imu.getGyro(&cachedGx, &cachedGy, &cachedGz);
#endif
    } else {
        // ==[ NO IMU ]== synthesise level-and-upright at rest, not zeros.
        // ay is UP in the landscape axis map, so this reads as exactly 1g
        // held still: the magnitude below lands on 1.0 and never reaches
        // the 1.6g step threshold, both atan2 attitude terms resolve to
        // 0 deg so parallax sits centred, and az = 0 stays clear of the
        // az > 0.7 up-tilt gesture so it cannot self-trigger at boot.
        // All-zeros would break the 1g-at-rest invariant instead.
        ax = 0.0f; ay = 1.0f; az = 0.0f;
        cachedGx = cachedGy = cachedGz = 0.0f;
    }
#if defined(HAMLET_CORE3SE)
    if (!rfPoseActive.load(std::memory_order_acquire)) {
        portENTER_CRITICAL(&imuCacheMux);
        cachedAx = ax; cachedAy = ay; cachedAz = az;
        portEXIT_CRITICAL(&imuCacheMux);
    }
#else
    cachedAx = ax; cachedAy = ay; cachedAz = az;
#endif

    // ==[ ORIENTATION HYSTERESIS ]== 0.1g deadband prevents oscillation
#if defined(HAMLET_CORE3SE)
    portENTER_CRITICAL(&imuCacheMux);
#endif
    cachedIsFlat = RfPoseMath::hystereticFlat(cachedIsFlat, az);
#if defined(HAMLET_CORE3SE)
    portEXIT_CRITICAL(&imuCacheMux);
#endif

    // Acceleration magnitude is orientation-independent and stays near 1g at rest.
    float magnitude = sqrtf(ax*ax + ay*ay + az*az);

    // ==[ SHAKE-TO-WAKE ]== violent shake wakes screen
    if (Config::getShakeWake() && Display::isDimmed()) {
        if (magnitude > SHAKE_THRESHOLD && (now - lastShakeTime > SHAKE_DEBOUNCE)) {
            lastShakeTime = now;
            Display::resetDimTimer();
        }
    }

    // A step is one complete upward threshold crossing followed by a fall.
    if (!stepPending && magnitude > threshold && lastMagnitude <= threshold) {
        stepPending = true;
    }

    if (stepPending && magnitude < threshold) {
        // The falling edge confirms the pending peak.
        if (now - lastStepTime > STEP_DEBOUNCE) {
            // ==[ CADENCE SANITY ]== reject impossible-fast bursts. Humans top
            // out around 180 steps/min (≈333ms/step) even sprinting; when two
            // prior intervals are both sub-350ms, the signal is periodic
            // mechanical noise (car bumps, desk vibration, riding a bus), not
            // gait. Drop this step without updating lastStepTime so the
            // debounce keeps anchored to the last ACCEPTED step.
            bool roboticBurst = false;
            if (stepWindowCount >= 2) {
                uint32_t t2 = stepTimestamps[(stepWindowIdx + STEP_WINDOW_SIZE - 1) % STEP_WINDOW_SIZE];
                uint32_t t1 = stepTimestamps[(stepWindowIdx + STEP_WINDOW_SIZE - 2) % STEP_WINDOW_SIZE];
                if (t2 > t1 && (now - t2) < 350 && (t2 - t1) < 350) {
                    roboticBurst = true;
                }
            }
            if (!roboticBurst) {
            sessionSteps++;

            uint32_t prevStepTime = lastStepTime;
            lastStepTime = now;
            lastStepEventTime = now;
            
            // The bounded timestamp ring feeds cadence and visible-walk state.
            stepTimestamps[stepWindowIdx] = now;
            stepWindowIdx = (stepWindowIdx + 1) % STEP_WINDOW_SIZE;
            if (stepWindowCount < STEP_WINDOW_SIZE) stepWindowCount++;
            
            // A gap over two seconds opens a new walking burst.
            if (burstStartTime == 0 || (prevStepTime != 0 && (now - prevStepTime > 2000))) {
                // First accepted step in this burst.
                burstStartTime = now;
                recentStepCount = 1;
            } else {
                recentStepCount++;
            }

            // Visual gait begins only after enough recent accepted steps.
            int windowSteps = 0;
            for (int i = 0; i < stepWindowCount; i++) {
                if (now - stepTimestamps[i] < VISUAL_WALK_WINDOW_MS) {
                    windowSteps++;
                }
            }
            if (windowSteps >= (int)VISUAL_WALK_MIN_STEPS) {
                visuallyWalking = true;
            }
            
            // The first accepted step opens a walking interval.
            if (!isCurrentlyWalking) {
                isCurrentlyWalking = true;
                walkingStartTime = now;
                autoHuntTriggered = false;
            }
            
            // Three accepted steps promote the motion state to WALKING.
            if (currentMotionState == MotionState::STATIONARY &&
                recentStepCount >= 3) {
                currentMotionState = MotionState::WALKING;
            }
            
            // ==[ SESSION GOAL TRACKING ]==
            Config::incrementSessionSteps();  // ram update per step
            // ==[ SESSION ACTIVE ]== 50 steps = you showed up
            if (sessionSteps == 50) {
                Config::markSessionActive();
            }
            // ==[ GOAL PROGRESS TRIGGERS ]==
            uint8_t progress = Config::getGoalProgress();
            if (progress >= 100 && !Config::wasGoalCompleteTriggered()) {
                Mood::onGoalComplete();
                Config::setGoalCompleteTriggered();
            } else if (progress >= 80 && !Config::wasGoalCloseTriggered()) {
                uint32_t remaining = Config::getGoalTarget() - Config::getSessionSteps();
                Mood::onGoalClose(remaining);
                Config::setGoalCloseTriggered();
            }

            // Batch persistence every 100 steps to protect NVS endurance.
            if (sessionSteps % 100 == 0) {
                Config::addSteps(100);
                persistedThisSession += 100;  // track persisted count
                Config::persistSessionSteps();  // batch nvs save
            }
            }  // end if (!roboticBurst)
        }
        stepPending = false;
    }

    lastMagnitude = magnitude;
    } // end sensor-processing scope

check_timeouts:
    // Five seconds without a step closes the active walking interval.
    if (isCurrentlyWalking && (now - lastStepEventTime > WALKING_TIMEOUT)) {
        isCurrentlyWalking = false;
        walkingStartTime = 0;
        recentStepCount = 0;
        burstStartTime = 0;
    }

    // The shorter visual latch clears independently of session motion state.
    if (visuallyWalking && (now - lastStepEventTime > VISUAL_WALK_TIMEOUT)) {
        visuallyWalking = false;
    }

    // Ten quiet seconds demote the coarse motion classifier to STATIONARY.
    if (currentMotionState == MotionState::WALKING &&
        (now - lastStepEventTime > 10000)) {
        currentMotionState = MotionState::STATIONARY;
        stationaryStartTime = now;
    }
}

bool isWalking() {
    return isCurrentlyWalking;
}

bool isVisuallyWalking() {
    return visuallyWalking;
}

uint32_t getWalkingDuration() {
    if (!isCurrentlyWalking || walkingStartTime == 0) return 0;
    return millis() - walkingStartTime;
}

bool shouldAutoHunt() {
    // Open one auto-hunt invitation per continuous 30-second walking interval.
    if (isCurrentlyWalking && !autoHuntTriggered && getWalkingDuration() >= AUTO_HUNT_THRESHOLD) {
        autoHuntTriggered = true;
        return true;
    }
    return false;
}

void resetAutoHunt() {
    autoHuntTriggered = false;
}

// ==[ MOTION STATE API ]==

MotionState getMotionState() {
    return currentMotionState;
}

uint32_t getStationaryDuration() {
    if (currentMotionState != MotionState::STATIONARY) return 0;
    return millis() - stationaryStartTime;
}

float getStepsPerSecond() {
    if (stepWindowCount < 2) return 0.0f;
    
    uint32_t now = millis();
    
    // scan last 10s window
    uint32_t oldest = UINT32_MAX;
    uint32_t newest = 0;
    int validCount = 0;
    
    for (int i = 0; i < stepWindowCount; i++) {
        if (now - stepTimestamps[i] < 10000) {
            if (stepTimestamps[i] < oldest) oldest = stepTimestamps[i];
            if (stepTimestamps[i] > newest) newest = stepTimestamps[i];
            validCount++;
        }
    }
    
    if (validCount < 2 || newest <= oldest) return 0.0f;
    
    float duration = (newest - oldest) / 1000.0f;
    if (duration < 0.5f) return 0.0f;
    
    return (validCount - 1) / duration;
}

void requestSniff() {
    sniffRequested = true;
}

bool consumeSniffRequest() {
    if (sniffRequested) {
        sniffRequested = false;
        return true;
    }
    return false;
}

uint32_t getSteps() {
    // config has persisted count. add ram delta.
    return Config::getTotalSteps() + (sessionSteps - persistedThisSession);
}

uint32_t getDistance() {
    return (uint32_t)(getSteps() * STEP_LENGTH_M);
}

float getDistanceKm() {
    return (getSteps() * STEP_LENGTH_M) / 1000.0f;
}

uint32_t getCalories() {
    return (uint32_t)(getSteps() * CALORIES_PER_STEP);
}

void resetSession() {
    // flush unpersisted steps to nvs
    uint32_t unpersisted = sessionSteps - persistedThisSession;
    if (unpersisted > 0) {
        Config::addSteps(unpersisted);
    }
    sessionSteps = 0;
    persistedThisSession = 0;
}

void getCachedAccel(float& ax, float& ay, float& az) {
#if defined(HAMLET_CORE3SE)
    portENTER_CRITICAL(&imuCacheMux);
#endif
    ax = cachedAx; ay = cachedAy; az = cachedAz;
#if defined(HAMLET_CORE3SE)
    portEXIT_CRITICAL(&imuCacheMux);
#endif
}

void getCachedGyro(float& gx, float& gy, float& gz) {
#if defined(HAMLET_CORE3SE)
    portENTER_CRITICAL(&imuCacheMux);
#endif
    gx = cachedGx; gy = cachedGy; gz = cachedGz;
#if defined(HAMLET_CORE3SE)
    portEXIT_CRITICAL(&imuCacheMux);
#endif
}

bool hasIMU() {
    return imuPresent;
}

bool isCachedFlat() {
#if defined(HAMLET_CORE3SE)
    portENTER_CRITICAL(&imuCacheMux);
    const bool flat = cachedIsFlat;
    portEXIT_CRITICAL(&imuCacheMux);
    return flat;
#else
    return cachedIsFlat;
#endif
}

void setRfPoseActive(bool active) {
#if defined(HAMLET_CORE3SE)
    if (active && (!imuPresent || rfPoseTaskHandle == nullptr)) {
        const bool wasActive =
            rfPoseActive.exchange(false, std::memory_order_acq_rel);
        rfPoseResetRequested.store(false, std::memory_order_release);
        if (wasActive) {
            portENTER_CRITICAL(&rfPoseMux);
            clearRfPoseRingLocked();
            portEXIT_CRITICAL(&rfPoseMux);
        }
        return;
    }
    if (rfPoseActive.load(std::memory_order_acquire) == active) return;
    if (active) {
        rfPoseResetRequested.store(true, std::memory_order_release);
        rfPoseActive.store(true, std::memory_order_release);
    } else {
        rfPoseActive.store(false, std::memory_order_release);
        rfPoseResetRequested.store(false, std::memory_order_release);
    }
    portENTER_CRITICAL(&rfPoseMux);
    clearRfPoseRingLocked();
    portEXIT_CRITICAL(&rfPoseMux);
#else
    (void)active;
#endif
}

void resetRfPoseOrigin() {
#if defined(HAMLET_CORE3SE)
    const bool wasActive =
        rfPoseActive.exchange(false, std::memory_order_acq_rel);
    rfPoseResetRequested.store(true, std::memory_order_release);
    portENTER_CRITICAL(&rfPoseMux);
    clearRfPoseRingLocked();
    portEXIT_CRITICAL(&rfPoseMux);
    if (wasActive) {
        rfPoseActive.store(true, std::memory_order_release);
    }
#endif
}

bool getLatestRfPose(RfPoseSample& out) {
    out = {};
#if defined(HAMLET_CORE3SE)
    portENTER_CRITICAL(&rfPoseMux);
    if (rfPoseCount == 0u) {
        portEXIT_CRITICAL(&rfPoseMux);
        return false;
    }
    const uint8_t newest = (uint8_t)(
        (rfPoseHead + RF_POSE_RING_SIZE - 1u) % RF_POSE_RING_SIZE);
    out = rfPoseRing[newest];
    portEXIT_CRITICAL(&rfPoseMux);
    return (out.flags & RF_POSE_IMU_PRESENT) != 0u;
#else
    return false;
#endif
}

bool getRfPoseAt(uint32_t rfTimestampUs, RfPoseSample& out) {
    out = {};
#if defined(HAMLET_CORE3SE)
    if (rfTimestampUs == 0u) return false;
    RfPoseSample before{};
    RfPoseSample after{};
    bool haveBefore = false;
    bool haveAfter = false;

    portENTER_CRITICAL(&rfPoseMux);
    const uint8_t oldest = (uint8_t)(
        (rfPoseHead + RF_POSE_RING_SIZE - rfPoseCount) %
        RF_POSE_RING_SIZE);
    for (uint8_t i = 0u; i < rfPoseCount; ++i) {
        const uint8_t index =
            (uint8_t)((oldest + i) % RF_POSE_RING_SIZE);
        const RfPoseSample& sample = rfPoseRing[index];
        const int32_t relative =
            (int32_t)(sample.timestampUs - rfTimestampUs);
        if (relative <= 0) {
            before = sample;
            haveBefore = true;
        } else {
            after = sample;
            haveAfter = true;
            break;
        }
    }
    portEXIT_CRITICAL(&rfPoseMux);

    if (!haveBefore && !haveAfter) return false;
    if (haveBefore && haveAfter &&
        before.timestampUs != after.timestampUs) {
        if (!RfPoseMath::interpolationSpanValid(
                before.timestampUs, after.timestampUs,
                RF_POSE_MAX_LOOKUP_AGE_US)) {
            return false;
        }
        const uint32_t spanUs =
            after.timestampUs - before.timestampUs;
        const uint32_t intoUs =
            rfTimestampUs - before.timestampUs;
        const float t = constrain(
            (float)intoUs / (float)spanUs, 0.0f, 1.0f);
        const auto lerp = [t](float a, float b) {
            return a + (b - a) * t;
        };
        out.timestampUs = rfTimestampUs;
        out.ax = lerp(before.ax, after.ax);
        out.ay = lerp(before.ay, after.ay);
        out.az = lerp(before.az, after.az);
        out.gx = lerp(before.gx, after.gx);
        out.gy = lerp(before.gy, after.gy);
        out.gz = lerp(before.gz, after.gz);
        out.rollDeg = lerp(before.rollDeg, after.rollDeg);
        out.pitchDeg = lerp(before.pitchDeg, after.pitchDeg);
        out.screenYawDeg = wrapDegrees(
            before.screenYawDeg +
            shortestDegrees(before.screenYawDeg,
                            after.screenYawDeg) * t);
        out.linearAx = lerp(before.linearAx, after.linearAx);
        out.linearAy = lerp(before.linearAy, after.linearAy);
        out.velocityX = lerp(before.velocityX, after.velocityX);
        out.velocityY = lerp(before.velocityY, after.velocityY);
        out.positionX = lerp(before.positionX, after.positionX);
        out.positionY = lerp(before.positionY, after.positionY);
        out.yawDriftDeg =
            lerp(before.yawDriftDeg, after.yawDriftDeg);
        const uint32_t beforeAge = rfTimestampUs - before.timestampUs;
        const uint32_t afterAge = after.timestampUs - rfTimestampUs;
        const uint32_t nearestAge =
            beforeAge < afterAge ? beforeAge : afterAge;
        out.interpolationAgeUs = (uint16_t)min(
            nearestAge, (uint32_t)0xffffu);
        out.flags = (uint8_t)(
            (before.flags & after.flags) | RF_POSE_INTERPOLATED);
        out.yawPose = t < 0.5f ? before.yawPose : after.yawPose;
        const int baseQuality = min(
            (int)before.quality, (int)after.quality);
        out.quality = (uint8_t)constrain(
            baseQuality - (int)(nearestAge / 2000u), 0, 100);
        return true;
    }

    out = haveBefore ? before : after;
    const uint32_t ageUs = haveBefore
        ? rfTimestampUs - before.timestampUs
        : after.timestampUs - rfTimestampUs;
    if (ageUs > RF_POSE_MAX_LOOKUP_AGE_US) {
        out = {};
        return false;
    }
    out.interpolationAgeUs =
        (uint16_t)min(ageUs, (uint32_t)0xffffu);
    out.quality = (uint8_t)constrain(
        (int)out.quality - (int)(ageUs / 2000u), 0, 100);
    return true;
#else
    (void)rfTimestampUs;
    return false;
#endif
}

}  // namespace Pedometer
