/**
 * Bearing Tracker - Gyro PDR with RSSI Gradient Lock
 *
 * ==[ DIRECTION FINDER ]== implementation
 *
 * Split into updateIMU() (every frame) and feedRSSI() (new samples only).
 * Gyro integration needs constant rate. RSSI trend needs real samples only.
 *
 * Clock injection: `now` parameter replaces direct millis() calls.
 * Pass millis() from caller (ESP32) or mock time (tests).
 * Default 0 = use millis() for backward compat on ESP32.
 */

#include "bearing.h"
#include "rf_pose_math.h"
#include "rf_util.h"
#include <math.h>
#include <cstring>

#ifndef NATIVE_TEST
#include <Arduino.h>  // millis() fallback on ESP32
#endif

namespace Bearing {

static constexpr uint32_t AUTHORITATIVE_POSE_STALE_MS = 120u;

// Resolve clock: use provided value, or fall back to millis() on ESP32
static inline uint32_t resolveClock(uint32_t now) {
#ifndef NATIVE_TEST
    return (now != 0) ? now : millis();
#else
    return now;  // In tests, caller must always provide time
#endif
}

static int16_t scanClamp(float v) {
    int iv = (int)((v >= 0.0f) ? (v + 0.5f) : (v - 0.5f));
    return (int16_t)constrain(iv, -100, 100);
}

static float wrapHeading(float heading) {
    while (heading >= 360.0f) heading -= 360.0f;
    while (heading < 0.0f) heading += 360.0f;
    return heading;
}

static uint16_t headingDegX10(float heading) {
    heading = wrapHeading(heading);
    int h = (int)(heading * 10.0f + 0.5f);
    if (h >= 3600) h -= 3600;
    return (uint16_t)h;
}

static int16_t elevationDegX10(float ax, float ay, float az) {
    const float horiz = sqrtf(ax * ax + ay * ay);
    int elev = (int)(atan2f(az, horiz) * 572.9578f);
    return (int16_t)constrain(elev, -900, 900);
}

static uint8_t strengthFromRssi(int8_t rssi) {
    return (uint8_t)constrain(map((long)rssi, -95, -25, 0, 100), 0, 100);
}

static float targetRelativeAngle(float targetHeading, float currentHeading) {
    float angle = targetHeading - currentHeading;
    while (angle > 180.0f) angle -= 360.0f;
    while (angle < -180.0f) angle += 360.0f;
    return angle;
}

static float angularTravel(float fromHeading, float toHeading) {
    return fabsf(targetRelativeAngle(toHeading, fromHeading));
}

static void clearRotationEvidence(TrackerState& state) {
    state.peakRssi = -127.0f;
    state.peakHeading = state.relativeHeading;
    state.rotationLastHeading = state.relativeHeading;
    state.rotationTravelDeg = 0.0f;
    state.rotationRfSamples = 0;
    for (uint8_t i = 0; i < 12; ++i) {
        state.rotationBinPeak[i] = -127;
        state.rotationBinHeading[i] = 0.0f;
        state.rotationBinSamples[i] = 0;
        state.rotationBinMeanX8[i] = 0;
        state.rotationBinDeviationX8[i] = 0u;
        state.rotationBinQualitySum[i] = 0u;
    }
    state.rotationFitAmplitude = 0.0f;
    state.rotationFitResidual = 0.0f;
    state.rotationAmbiguity = 0.0f;
    state.estimateState = EstimateState::SEEK;
}

static float rotationBinWeight(const TrackerState& state, int bin) {
    const uint8_t samples = state.rotationBinSamples[bin];
    if (samples == 0u) return 0.0f;

    const float deviationDb =
        (float)state.rotationBinDeviationX8[bin] / 8.0f;
    const float quality =
        state.rotationBinQualitySum[bin] > 0u
            ? (float)state.rotationBinQualitySum[bin] /
                  ((float)samples * 100.0f)
            : 0.5f;
    // Counts help, but cap their leverage so a busy sector cannot win by
    // packet volume alone. Robust within-sector deviation lowers weight.
    const float countWeight =
        1.0f + (float)min((int)samples, 4) * 0.35f;
    return countWeight * constrain(quality, 0.2f, 1.0f) /
           (1.0f + deviationDb * 0.35f);
}

static bool solve3x3(float matrix[3][4], float result[3]) {
    for (int pivot = 0; pivot < 3; ++pivot) {
        int best = pivot;
        for (int row = pivot + 1; row < 3; ++row) {
            if (fabsf(matrix[row][pivot]) >
                fabsf(matrix[best][pivot])) {
                best = row;
            }
        }
        if (fabsf(matrix[best][pivot]) < 0.0001f) return false;
        if (best != pivot) {
            for (int column = pivot; column < 4; ++column) {
                const float tmp = matrix[pivot][column];
                matrix[pivot][column] = matrix[best][column];
                matrix[best][column] = tmp;
            }
        }

        const float divisor = matrix[pivot][pivot];
        for (int column = pivot; column < 4; ++column) {
            matrix[pivot][column] /= divisor;
        }
        for (int row = 0; row < 3; ++row) {
            if (row == pivot) continue;
            const float factor = matrix[row][pivot];
            for (int column = pivot; column < 4; ++column) {
                matrix[row][column] -= factor * matrix[pivot][column];
            }
        }
    }

    result[0] = matrix[0][3];
    result[1] = matrix[1][3];
    result[2] = matrix[2][3];
    return true;
}

static bool fitRotationHarmonic(const TrackerState& state, uint8_t harmonic,
                                float& baseline, float& cosine,
                                float& sine, float& sumWeight,
                                uint8_t& occupiedBins) {
    float normal[3][4] = {};
    sumWeight = 0.0f;
    occupiedBins = 0u;

    for (int i = 0; i < 12; ++i) {
        const float weight = rotationBinWeight(state, i);
        if (weight <= 0.0f) continue;
        ++occupiedBins;

        const float theta =
            state.rotationBinHeading[i] *
            (3.1415926535f / 180.0f) * (float)harmonic;
        const float x[3] = {1.0f, cosf(theta), sinf(theta)};
        const float y = (float)state.rotationBinMeanX8[i] / 8.0f;
        sumWeight += weight;
        for (int row = 0; row < 3; ++row) {
            for (int column = 0; column < 3; ++column) {
                normal[row][column] += weight * x[row] * x[column];
            }
            normal[row][3] += weight * x[row] * y;
        }
    }

    float coefficients[3] = {};
    if (occupiedBins < 4u || sumWeight <= 0.0f ||
        !solve3x3(normal, coefficients)) {
        return false;
    }
    baseline = coefficients[0];
    cosine = coefficients[1];
    sine = coefficients[2];
    return true;
}

static bool finishRotationSweep(TrackerState& state,
                                const TrackerConfig& config,
                                uint32_t rotationDuration) {
    if (rotationDuration < config.minRotationMs ||
        state.rotationTravelDeg < (float)config.minRotationDegrees ||
        state.rotationRfSamples < config.minRotationSamples) {
        state.estimateState =
            state.rotationTravelDeg >= 30.0f &&
            state.rotationRfSamples >= 3u
                ? EstimateState::COARSE
                : EstimateState::SEEK;
        return false;
    }

    uint8_t occupiedBins = 0u;
    float sumW = 0.0f;
    float baseline = 0.0f;
    float cos1 = 0.0f;
    float sin1 = 0.0f;
    if (!fitRotationHarmonic(state, 1u, baseline, cos1, sin1,
                             sumW, occupiedBins) ||
        occupiedBins < config.minRotationBins) {
        state.estimateState = EstimateState::COARSE;
        return false;
    }

    float secondBaseline = 0.0f;
    float cos2 = 0.0f;
    float sin2 = 0.0f;
    float secondWeight = 0.0f;
    uint8_t secondBins = 0u;
    const bool secondFit =
        fitRotationHarmonic(state, 2u, secondBaseline, cos2, sin2,
                            secondWeight, secondBins);

    const float amplitude1 = sqrtf(cos1 * cos1 + sin1 * sin1);
    const float amplitude2 =
        secondFit ? sqrtf(cos2 * cos2 + sin2 * sin2) : 0.0f;
    float fitHeading = atan2f(sin1, cos1) * (180.0f / 3.1415926535f);
    while (fitHeading < 0.0f) fitHeading += 360.0f;
    while (fitHeading >= 360.0f) fitHeading -= 360.0f;

    float residualSum = 0.0f;
    for (int i = 0; i < 12; ++i) {
        const float weight = rotationBinWeight(state, i);
        if (weight <= 0.0f) continue;
        const float theta =
            state.rotationBinHeading[i] * (3.1415926535f / 180.0f);
        const float prediction =
            baseline + cos1 * cosf(theta) + sin1 * sinf(theta);
        const float error =
            (float)state.rotationBinMeanX8[i] / 8.0f - prediction;
        residualSum += weight * error * error;
    }
    const float residual = sqrtf(residualSum / sumW);
    const float ambiguity =
        amplitude2 / max(amplitude1, 0.25f);
    state.rotationFitAmplitude = amplitude1;
    state.rotationFitResidual = residual;
    state.rotationAmbiguity = ambiguity;

    const float minimumContrast =
        max(1.5f, (float)config.minPeakSeparationDb * 0.55f);
    // A second harmonic is a useful front/back ambiguity detector only after
    // most of the circle has been observed. On a partial arc its basis is
    // strongly correlated with the first harmonic even with a proper fit.
    const bool ambiguityObservable =
        occupiedBins >= 8u || state.rotationTravelDeg >= 240.0f;
    const bool ambiguous =
        amplitude1 < minimumContrast ||
        (ambiguityObservable &&
         amplitude2 > amplitude1 * 1.15f && amplitude2 >= 2.0f);
    if (ambiguous) {
        state.estimateState = EstimateState::AMBIG;
        return false;
    }

    const int coverageScore =
        constrain((int)occupiedBins * 30 / 12, 0, 30);
    const int travelScore = constrain(
        (int)(state.rotationTravelDeg * 35.0f / 180.0f), 0, 35);
    const int sampleScore = constrain(
        (int)state.rotationRfSamples * 20 / 12, 0, 20);
    const int contrastScore =
        constrain((int)(amplitude1 * 4.0f), 0, 25);
    const int residualPenalty =
        constrain((int)(residual * 2.0f), 0, 25);
    const int ambiguityPenalty =
        ambiguityObservable && ambiguity > 0.8f
            ? constrain((int)((ambiguity - 0.8f) * 20.0f), 0, 15)
            : 0;
    const int confidence =
        coverageScore + travelScore + sampleScore + contrastScore -
        residualPenalty - ambiguityPenalty;
    if (confidence < 25) {
        state.estimateState = EstimateState::COARSE;
        return false;
    }

    state.lockedHeading = fitHeading;
    state.bearingLocked = true;
    state.lockConfidence = (uint8_t)constrain(
        confidence, 25, 85);
    state.estimateState = EstimateState::LOCK;
    if (++state.lockGeneration == 0u) state.lockGeneration = 1u;
    state.lastReinforceTime = state.lastDirectionalFeedTime;
    return true;
}

static int16_t bearingFromX256(int32_t value) {
    // Round symmetrically. Plain (value + 128) / 256 pulls negatives inward.
    return (int16_t)((value >= 0 ? value + 128 : value - 128) / 256);
}

static int16_t currentBearingAtHeading(const TrackerState& state,
                                       float heading,
                                       bool& behind,
                                       uint8_t& confidence) {
    behind = state.porkBehind;
    confidence = state.bearingConfidence;

    if (state.bearingLocked && state.lockConfidence > 0) {
        const float angleToPork = targetRelativeAngle(state.lockedHeading,
                                                       heading);

        behind = (fabsf(angleToPork) > 90.0f);
        confidence = state.lockConfidence;
        return constrain((int16_t)(angleToPork * 100.0f / 90.0f),
                         -100, 100);
    }

    return state.bearing;
}

static void appendRfPoint(TrackerState& state, int8_t rssi,
                          int8_t rssiSmooth, uint32_t seenMs,
                          uint8_t evidenceFlags, uint8_t csiQuality,
                          const ObservationPose* observationPose) {
    const bool hasObservationPose =
        observationPose && observationPose->valid;
    const float pointHeading = hasObservationPose
        ? (float)observationPose->headingDegX10 / 10.0f
        : state.relativeHeading;
    bool behind = false;
    uint8_t conf = 0;
    const int16_t bearing =
        currentBearingAtHeading(state, pointHeading, behind, conf);

    RfPoint& p = state.rfPoints[state.rfPointHead];
    p.seenMs = seenMs;
    p.headingDegX10 =
        hasObservationPose
            ? observationPose->headingDegX10
            : state.lastHeadingDegX10;
    p.elevDegX10 =
        hasObservationPose
            ? observationPose->elevationDegX10
            : state.lastElevDegX10;
    p.bearing = bearing;
    p.rssi = rssi;
    p.strength = strengthFromRssi(rssiSmooth);
    p.confidence = conf;
    p.evidenceFlags = evidenceFlags | RF_EVIDENCE_PASSIVE;
    p.csiQuality = csiQuality;
    p.poseQuality =
        hasObservationPose
            ? observationPose->quality
            : state.stationaryConfidence;
    p.poseFlags =
        hasObservationPose
            ? observationPose->flags
            : 0u;
    p.poseAgeUs =
        hasObservationPose
            ? observationPose->interpolationAgeUs
            : 0xffffu;
    p.yawDriftDegX10 =
        hasObservationPose
            ? observationPose->yawDriftDegX10
            : 1800;
    p.behind = behind;
    p.moving = state.isMoving;

    state.rfPointHead = (uint8_t)((state.rfPointHead + 1) % RF_POINT_MAX);
    if (state.rfPointCount < RF_POINT_MAX) state.rfPointCount++;
}

static void updateThruScan(TrackerState& state,
                           float ax, float ay, float az,
                           float accelMag,
                           float observerMotion,
                           float dt) {
    if (!state.thruAccelPrimed) {
        state.thruLastAx = ax;
        state.thruLastAy = ay;
        state.thruLastAz = az;
        state.thruAccelPrimed = true;
        return;
    }

    float prevX, prevY, curX, curY;
    RfPoseMath::deviceToScreen(
        state.isFlat, state.yawPose,
        state.thruLastAx, state.thruLastAy, state.thruLastAz,
        prevX, prevY);
    RfPoseMath::deviceToScreen(
        state.isFlat, state.yawPose, ax, ay, az, curX, curY);

    const float dx = ax - state.thruLastAx;
    const float dy = ay - state.thruLastAy;
    const float dz = az - state.thruLastAz;
    const float screenDx = curX - prevX;
    const float screenDy = curY - prevY;
    const float deltaMag = sqrtf(dx * dx + dy * dy + dz * dz);

    if (dt > 0.0f && dt <= 0.12f) {
        const float deadZone = 0.035f;
        float linearX = fabsf(curX) > deadZone
            ? curX - copysignf(deadZone, curX) : 0.0f;
        float linearY = fabsf(curY) > deadZone
            ? curY - copysignf(deadZone, curY) : 0.0f;
        if (state.accelMotionEnergy >= 18.0f && deltaMag >= 0.012f) {
            const float heading = state.relativeHeading * 0.017453292519943295f;
            const float c = cosf(heading);
            const float s = sinf(heading);
            const float worldLinearX = c * linearX + s * linearY;
            const float worldLinearY = -s * linearX + c * linearY;
            state.observerVelocityX = constrain(
                state.observerVelocityX * 0.90f + worldLinearX * 9.80665f * dt,
                -1.2f, 1.2f);
            state.observerVelocityY = constrain(
                state.observerVelocityY * 0.90f + worldLinearY * 9.80665f * dt,
                -1.2f, 1.2f);
            state.observerPositionX = constrain(
                state.observerPositionX + state.observerVelocityX * dt,
                -3.0f, 3.0f);
            state.observerPositionY = constrain(
                state.observerPositionY + state.observerVelocityY * dt,
                -3.0f, 3.0f);
        } else {
            state.observerVelocityX *= 0.35f;
            state.observerVelocityY *= 0.35f;
            if (fabsf(state.observerVelocityX) < 0.01f) {
                state.observerVelocityX = 0.0f;
            }
            if (fabsf(state.observerVelocityY) < 0.01f) {
                state.observerVelocityY = 0.0f;
            }
        }
    }

    // Dynamic displacement only. Static gravity/tilt is pose, not motion.
    const int16_t targetX = scanClamp(screenDx * 1150.0f);
    const int16_t targetY = scanClamp(screenDy * 900.0f +
                                      (accelMag - 1.0f) * 180.0f);

    if (observerMotion >= 12.0f) {
        state.thruScanX = (int16_t)((state.thruScanX * 2 + targetX) / 3);
        state.thruScanY = (int16_t)((state.thruScanY * 2 + targetY) / 3);
    } else {
        state.thruScanX = (int16_t)((state.thruScanX * 3) / 4);
        state.thruScanY = (int16_t)((state.thruScanY * 3) / 4);
    }

    const int heatRaw = constrain((int)max(observerMotion,
                                            deltaMag * 520.0f +
                                            fabsf(accelMag - 1.0f) * 110.0f),
                                  0, 100);
    if (heatRaw > state.thruMotionHeat) {
        state.thruMotionHeat = (uint8_t)heatRaw;
    } else {
        state.thruMotionHeat = (uint8_t)((state.thruMotionHeat * 230) / 256);
    }

    state.thruLastAx = ax;
    state.thruLastAy = ay;
    state.thruLastAz = az;
}

float yawRateForPose(bool isFlat,
                     float gx, float gy, float gz,
                     float ax, float ay) {
    // M5Unified exposes the Core2 sensor frame from the screen side:
    // +X right, +Y up, +Z toward the viewer. Gyro signs follow the
    // right-hand rule, while our heading grows clockwise on screen.
    if (isFlat) return -gz;

    // Upright yaw is rotation around gravity projected into the panel. A
    // normalized dot product stays full-scale between the four hard edges.
    const float uprightG = sqrtf(ax * ax + ay * ay);
    if (uprightG < 0.001f) return 0.0f;
    return (gx * ax + gy * ay) / uprightG;
}

void reset(TrackerState& state) {
    state.relativeHeading = 0.0f;
    state.gyroBias = 0.0f;
    state.gyroCalibrated = false;
    state.lastGyroUpdate = 0;

    memset(state.rssiHistory, 0, sizeof(state.rssiHistory));
    state.rssiHistoryIdx = 0;
    state.rssiHistoryCount = 0;
    state.rssiTrend = 0;
    state.rssiTrendSmooth = 0;

    for (int i = 0; i < 8; i++) state.accelHistory[i] = 1.0f;
    state.accelIdx = 0;
    state.motionState = false;
    state.lastHighVariance = 0;
    state.motionStartTime = 0;
    state.lastMotionTime = 0;
    state.accelMotionEnergy = 0.0f;
    state.motionLastGx = 0.0f;
    state.motionLastGy = 0.0f;
    state.motionLastGz = 0.0f;
    state.motionSamplePrimed = false;
    state.lastMotionSampleTime = 0;
    state.stationaryConfidence = 0;
    state.observerVelocityX = 0.0f;
    state.observerVelocityY = 0.0f;
    state.observerPositionX = 0.0f;
    state.observerPositionY = 0.0f;
    state.thruLastAx = 0.0f;
    state.thruLastAy = 0.0f;
    state.thruLastAz = 1.0f;
    state.thruAccelPrimed = false;
    state.thruScanX = 0;
    state.thruScanY = 0;
    state.thruMotionHeat = 0;
    state.lastAuthoritativePoseTimestampUs = 0u;
    state.lastAuthoritativePoseUpdateMs = 0u;
    state.authoritativePosePrimed = false;
    state.lastHeadingDegX10 = 0;
    state.lastElevDegX10 = 0;
    for (RfPoint& point : state.rfPoints) {
        point = {};
    }
    state.rfPointHead = 0;
    state.rfPointCount = 0;

    state.lockedHeading = 0.0f;
    state.bearingLocked = false;
    state.lockConfidence = 0;
    state.lastReinforceTime = 0;
    state.lastDecay = 0;
    state.lastDirectionalFeedTime = 0;
    state.approachConfirmCount = 0;
    // Monotonic across resets: BLE can preserve an identity-level LKP while
    // clearing live evidence during an ambiguous scan.

    state.peakRssi = -127.0f;
    state.peakHeading = 0.0f;
    state.wasRotating = false;
    state.rotationStartTime = 0;
    state.rotationLastHeading = 0.0f;
    state.rotationTravelDeg = 0.0f;
    state.rotationRfSamples = 0;
    memset(state.rotationBinPeak, -127, sizeof(state.rotationBinPeak));
    memset(state.rotationBinHeading, 0, sizeof(state.rotationBinHeading));
    memset(state.rotationBinSamples, 0, sizeof(state.rotationBinSamples));
    memset(state.rotationBinMeanX8, 0, sizeof(state.rotationBinMeanX8));
    memset(state.rotationBinDeviationX8, 0,
           sizeof(state.rotationBinDeviationX8));
    memset(state.rotationBinQualitySum, 0,
           sizeof(state.rotationBinQualitySum));
    state.rotationFitAmplitude = 0.0f;
    state.rotationFitResidual = 0.0f;
    state.rotationAmbiguity = 0.0f;
    state.estimateState = EstimateState::SEEK;

    state.yawPose = 0;
    state.biasSum = 0.0f;
    state.biasSamples = 0;

    state.prevFeedRssi = -128.0f;
    state.deltaEma = 0.0f;
    state.lastFeedTime = 0;

    state.bearing_x256 = 0;
    state.bearing = 0;
    state.bearingRaw = 0;
    state.bearingConfidence = 0;
    state.porkBehind = false;
    state.isMoving = false;
    state.isFlat = false;
}

static void consumeAuthoritativePose(
    TrackerState& state, const AuthoritativePoseSample& pose) {
    uint32_t elapsedUs = 10000u;
    if (state.authoritativePosePrimed) {
        elapsedUs = pose.timestampUs -
            state.lastAuthoritativePoseTimestampUs;
        elapsedUs = min(elapsedUs, 250000u);
    }

    const float linearMagnitude =
        sqrtf(pose.linearAx * pose.linearAx +
              pose.linearAy * pose.linearAy);
    const float velocityMagnitude =
        sqrtf(pose.velocityX * pose.velocityX +
              pose.velocityY * pose.velocityY);
    float targetEnergy = constrain(
        max(linearMagnitude * 900.0f,
            velocityMagnitude * 65.0f),
        0.0f, 100.0f);
    if (pose.stationary) targetEnergy = 0.0f;
    const float energyTau =
        targetEnergy > state.accelMotionEnergy ? 0.045f : 0.18f;
    const float energyAlpha =
        RfPoseMath::timeBlendAlpha(elapsedUs, energyTau);
    state.accelMotionEnergy +=
        (targetEnergy - state.accelMotionEnergy) * energyAlpha;

    const float deadZone = 0.015f;
    const float linearX = fabsf(pose.linearAx) > deadZone
        ? pose.linearAx - copysignf(deadZone, pose.linearAx)
        : 0.0f;
    const float linearY = fabsf(pose.linearAy) > deadZone
        ? pose.linearAy - copysignf(deadZone, pose.linearAy)
        : 0.0f;
    const int16_t targetX = scanClamp(linearX * 1050.0f);
    const int16_t targetY = scanClamp(linearY * 900.0f);
    const bool scanAttack =
        abs((int)targetX) > abs((int)state.thruScanX) ||
        abs((int)targetY) > abs((int)state.thruScanY);
    const float scanAlpha = RfPoseMath::timeBlendAlpha(
        elapsedUs, scanAttack ? 0.035f : 0.12f);
    state.thruScanX = scanClamp(
        (float)state.thruScanX +
        ((float)targetX - (float)state.thruScanX) * scanAlpha);
    state.thruScanY = scanClamp(
        (float)state.thruScanY +
        ((float)targetY - (float)state.thruScanY) * scanAlpha);

    const float targetHeat = constrain(
        max(targetEnergy, linearMagnitude * 700.0f),
        0.0f, 100.0f);
    const float heatAlpha = RfPoseMath::timeBlendAlpha(
        elapsedUs,
        targetHeat > (float)state.thruMotionHeat ? 0.04f : 0.25f);
    state.thruMotionHeat = (uint8_t)constrain(
        (int)((float)state.thruMotionHeat +
              (targetHeat - (float)state.thruMotionHeat) * heatAlpha +
              0.5f),
        0, 100);

    state.observerVelocityX = pose.stationary
        ? 0.0f : constrain(pose.velocityX, -2.0f, 2.0f);
    state.observerVelocityY = pose.stationary
        ? 0.0f : constrain(pose.velocityY, -2.0f, 2.0f);
    state.observerPositionX = constrain(pose.positionX, -3.0f, 3.0f);
    state.observerPositionY = constrain(pose.positionY, -3.0f, 3.0f);
    state.lastAuthoritativePoseTimestampUs = pose.timestampUs;
    state.authoritativePosePrimed = true;
}

// ==[ updateIMU ]== shared implementation for integrated and sourced heading.
static void updateIMUImpl(TrackerState& state, const TrackerConfig& config,
                          float gx, float gy, float gz,
                          float ax, float ay, float az,
                          bool hasAuthoritativeHeading,
                          float authoritativeHeadingDeg,
                          const AuthoritativePoseSample* authoritativePose,
                          uint32_t now_param) {

    uint32_t now = resolveClock(now_param);
    const bool newAuthoritativePose =
        authoritativePose &&
        (!state.authoritativePosePrimed ||
         RfPoseMath::timestampIsNewer(
             authoritativePose->timestampUs,
             state.lastAuthoritativePoseTimestampUs));
    const bool authoritativePoseStale =
        authoritativePose && !newAuthoritativePose &&
        state.lastAuthoritativePoseUpdateMs != 0u &&
        now - state.lastAuthoritativePoseUpdateMs >=
            AUTHORITATIVE_POSE_STALE_MS;
    if (newAuthoritativePose) {
        state.relativeHeading =
            wrapHeading(authoritativePose->headingDeg);
    } else if (hasAuthoritativeHeading && !authoritativePose) {
        state.relativeHeading = wrapHeading(authoritativeHeadingDeg);
    }

    // ==[ ORIENTATION DETECTION ]== with hysteresis
    bool isFlat;
    if (state.isFlat) {
        isFlat = (fabsf(az) > 0.65f);
    } else {
        isFlat = (fabsf(az) > 0.75f);
    }
    state.isFlat = isFlat;

    float yawRate = yawRateForPose(state.isFlat, gx, gy, gz, ax, ay);
    const uint8_t yawPose = RfPoseMath::yawPoseForGravity(
        state.isFlat, ax, ay, state.yawPose);
    const float accelMag = sqrtf(ax*ax + ay*ay + az*az);
    float gyroMag = sqrtf(gx*gx + gy*gy + gz*gz);
    if (authoritativePoseStale) {
        // A stalled high-rate producer must not replay its last moving sample
        // forever. Heading remains at the last sourced pose while motion,
        // scan heat, and rotation state decay through the quiet path below.
        yawRate = 0.0f;
        gyroMag = 0.0f;
    }

    // ==[ ORIENTATION CHANGE ]== reset calibration
    // Each upright edge uses a different signed gyro axis. Never carry an
    // old axis bias into the new screen-relative yaw rate.
    const bool orientationChanged = (yawPose != state.yawPose);
    if (orientationChanged) {
        state.gyroCalibrated = false;
        state.biasSum = 0.0f;
        state.biasSamples = 0;
        state.yawPose = yawPose;
        state.thruAccelPrimed = false;
        state.thruScanX = 0;
        state.thruScanY = 0;
        state.thruMotionHeat = 0;
        state.observerVelocityX = 0.0f;
        state.observerVelocityY = 0.0f;
        state.motionSamplePrimed = false;
        state.stationaryConfidence = 0;
        state.lastHighVariance = now;
    }

    // Pedometer owns the I2C read at 20Hz while modes can render faster.
    // Ignore repeated cache values for variance/calibration; keep gyro ZOH
    // integration below running at frame cadence.
    const bool authoritativeMotion = authoritativePose != nullptr;
    bool newImuSample = authoritativeMotion
        ? newAuthoritativePose
        : (!state.motionSamplePrimed || !state.thruAccelPrimed ||
           ax != state.thruLastAx || ay != state.thruLastAy ||
           az != state.thruLastAz || gx != state.motionLastGx ||
           gy != state.motionLastGy || gz != state.motionLastGz);

    float accelDelta = 0.0f;
    if (state.thruAccelPrimed) {
        const float dx = ax - state.thruLastAx;
        const float dy = ay - state.thruLastAy;
        const float dz = az - state.thruLastAz;
        accelDelta = sqrtf(dx * dx + dy * dy + dz * dz);
    }

    float variance = 0.0f;
    if (authoritativeMotion && newAuthoritativePose) {
        consumeAuthoritativePose(state, *authoritativePose);
        state.lastAuthoritativePoseUpdateMs = now;
        state.thruLastAx = ax;
        state.thruLastAy = ay;
        state.thruLastAz = az;
        state.thruAccelPrimed = true;
        state.motionLastGx = gx;
        state.motionLastGy = gy;
        state.motionLastGz = gz;
        state.motionSamplePrimed = true;
        state.lastMotionSampleTime = now;
    } else if (!authoritativeMotion && newImuSample) {
        state.accelHistory[state.accelIdx] = accelMag;
        state.accelIdx = (state.accelIdx + 1) % 8;

        float sum = 0.0f;
        float sumSq = 0.0f;
        for (int i = 0; i < 8; i++) {
            sum += state.accelHistory[i];
            sumSq += state.accelHistory[i] * state.accelHistory[i];
        }
        const float mean = sum / 8.0f;
        variance = max(0.0f, (sumSq / 8.0f) - (mean * mean));

        const float varianceScore = (config.varianceEnter > 0.0f)
            ? constrain(variance * 55.0f / config.varianceEnter, 0.0f, 100.0f)
            : 0.0f;
        const float deltaScore = constrain(accelDelta * 850.0f, 0.0f, 100.0f);
        const float normScore = constrain(
            max(0.0f, fabsf(accelMag - 1.0f) - 0.015f) * 700.0f,
            0.0f, 100.0f);
        const float translationRaw = constrain(
            max(deltaScore, normScore) + varianceScore * 0.30f,
            0.0f, 100.0f);

        const float alpha = (translationRaw > state.accelMotionEnergy)
            ? 0.55f : 0.18f;
        state.accelMotionEnergy +=
            (translationRaw - state.accelMotionEnergy) * alpha;

        const float gyroFloor = state.gyroCalibrated ? 2.5f : 4.0f;
        const float gyroScore = constrain(
            max(0.0f, gyroMag - gyroFloor) * 5.0f, 0.0f, 100.0f);
        const float observerMotion = max(state.accelMotionEnergy, gyroScore);
        const uint32_t motionElapsed = now - state.lastMotionSampleTime;
        const float motionDt = state.lastMotionSampleTime > 0u &&
                               motionElapsed > 0u
            ? (float)motionElapsed / 1000.0f : 0.0f;
        updateThruScan(state, ax, ay, az, accelMag, observerMotion, motionDt);

        state.motionLastGx = gx;
        state.motionLastGy = gy;
        state.motionLastGz = gz;
        state.motionSamplePrimed = true;
        state.lastMotionSampleTime = now;
    } else if (!authoritativeMotion || authoritativePoseStale) {
        // Quantized MPU samples can repeat exactly at rest. Give unchanged
        // cache values a slow, time-based quiet decay instead of freezing the
        // last movement spike forever. Gyro energy remains independently live.
        if (state.lastMotionSampleTime != 0u &&
            now - state.lastMotionSampleTime >= 100u) {
            state.accelMotionEnergy *= 0.72f;
            state.observerVelocityX *= 0.35f;
            state.observerVelocityY *= 0.35f;
            if (fabsf(state.observerVelocityX) < 0.01f) {
                state.observerVelocityX = 0.0f;
            }
            if (fabsf(state.observerVelocityY) < 0.01f) {
                state.observerVelocityY = 0.0f;
            }
            state.thruScanX = (int16_t)((state.thruScanX * 3) / 4);
            state.thruScanY = (int16_t)((state.thruScanY * 3) / 4);
            state.thruMotionHeat = (uint8_t)((state.thruMotionHeat * 220u) / 256u);
            state.lastMotionSampleTime = now;
        }

        float sum = 0.0f;
        float sumSq = 0.0f;
        for (int i = 0; i < 8; i++) {
            sum += state.accelHistory[i];
            sumSq += state.accelHistory[i] * state.accelHistory[i];
        }
        const float mean = sum / 8.0f;
        variance = max(0.0f, (sumSq / 8.0f) - (mean * mean));
    }

    const float gyroFloor = state.gyroCalibrated ? 2.5f : 4.0f;
    const float gyroScore = constrain(
        max(0.0f, gyroMag - gyroFloor) * 5.0f, 0.0f, 100.0f);
    const float observerMotion = max(state.accelMotionEnergy, gyroScore);

    // Translation and rotation stay separate: walking lock consumes isMoving;
    // rotation lock consumes gyro. Stationary confidence distrusts both.
    if (state.accelMotionEnergy >= 26.0f || variance > config.varianceEnter) {
        state.motionState = true;
        if (!authoritativeMotion || newAuthoritativePose) {
            state.lastHighVariance = now;
        }
    } else if (state.accelMotionEnergy < 10.0f &&
               variance < config.varianceExit &&
               (now - state.lastHighVariance > config.motionHoldMs)) {
        state.motionState = false;
    }
    if (observerMotion >= 18.0f &&
        (!authoritativeMotion || newAuthoritativePose)) {
        state.lastHighVariance = now;
    }

    state.isMoving = state.motionState;
    const uint32_t settleMs = max(config.stationarySettleMs,
                                  config.motionHoldMs);
    const uint32_t quietMs = now - state.lastHighVariance;
    const int quietRamp = (quietMs >= settleMs)
        ? 100
        : (int)((quietMs * 100u) / settleMs);
    int stationary = constrain(100 - (int)(observerMotion * 2.0f), 0, 100);
    stationary = min(stationary, quietRamp);
    if (state.motionState) stationary = min(stationary, 12);
    state.stationaryConfidence = (uint8_t)stationary;

    const bool isStill = accelMag > config.stillMin &&
                         accelMag < config.stillMax &&
                         state.stationaryConfidence >= 70;

    // ==[ GYRO CALIBRATION ]== unique, genuinely quiet samples only.
    if (newImuSample && isStill && !state.gyroCalibrated) {
        state.biasSum += yawRate;
        state.biasSamples++;

        if (state.biasSamples >= config.calSamples) {
            state.gyroBias = state.biasSum / state.biasSamples;
            state.gyroCalibrated = true;
            state.biasSum = 0.0f;
            state.biasSamples = 0;
        }
    } else if (!isStill && observerMotion >= 24.0f &&
               !state.gyroCalibrated) {
        state.biasSum = 0.0f;
        state.biasSamples = 0;
    }

    if (state.gyroCalibrated) yawRate -= state.gyroBias;

    // ==[ GYRO INTEGRATION ]==
    float dt = 0.0f;
    if (state.lastGyroUpdate > 0) {
        dt = (now - state.lastGyroUpdate) / 1000.0f;
    }
    state.lastGyroUpdate = now;

    if (!hasAuthoritativeHeading && !authoritativeMotion &&
        dt > 0 && dt < 0.5f) {
        state.relativeHeading += yawRate * dt;
        state.relativeHeading = wrapHeading(state.relativeHeading);
    }
    state.lastHeadingDegX10 = headingDegX10(state.relativeHeading);
    state.lastElevDegX10 = elevationDegX10(ax, ay, az);

    if (state.isMoving) {
        if (state.motionStartTime == 0) state.motionStartTime = now;
        state.lastMotionTime = now;
    } else if (now - state.lastMotionTime > 500) {
        state.motionStartTime = 0;
    }

    // ==[ ROTATION DETECTION ]==
    float absYawRate = fabsf(yawRate);
    bool isRotating = (absYawRate > config.rotationThreshold);

    if (!state.isMoving && isRotating && state.gyroCalibrated) {
        if (!state.wasRotating) {
            clearRotationEvidence(state);
            state.rotationStartTime = now;
        } else {
            state.rotationTravelDeg += angularTravel(state.rotationLastHeading,
                                                      state.relativeHeading);
            state.rotationLastHeading = state.relativeHeading;
        }
        state.wasRotating = true;
    } else if (state.wasRotating && !isRotating && !state.isMoving) {
        const uint32_t rotationDuration = now - state.rotationStartTime;
        const bool sweepLocked = finishRotationSweep(state, config,
                                                       rotationDuration);
        if (!sweepLocked &&
            state.rotationTravelDeg >= (float)config.minRotationDegrees) {
            state.bearingLocked = false;
            state.lockConfidence = 0;
        }
        state.wasRotating = false;
    } else {
        if (state.wasRotating && state.isMoving) clearRotationEvidence(state);
        state.wasRotating = false;
    }

    // ==[ BEARING CALCULATION ]==
    if (state.bearingLocked && state.lockConfidence > config.minConfidence) {
        // Screen bearing is target minus handset heading. The inverse made a
        // fixed contact orbit counterclockwise in both WiFi and BLE views.
        const float angleToPork = targetRelativeAngle(state.lockedHeading,
                                                       state.relativeHeading);

        state.porkBehind = (fabsf(angleToPork) > 90.0f);

        int16_t screenBearing = constrain((int16_t)(angleToPork * 100.0f / 90.0f), -100, 100);

        state.bearingRaw = screenBearing;
        uint8_t iirRatio = state.isFlat ? config.iirRatioFlat : config.iirRatioUpright;
        state.bearing_x256 = (state.bearing_x256 * (iirRatio - 1) + (int32_t)state.bearingRaw * 256) / iirRatio;
        state.bearing = bearingFromX256(state.bearing_x256);
        state.bearingConfidence = state.lockConfidence;
    } else {
        state.bearingRaw = 0;
        state.bearing_x256 = (state.bearing_x256 * 7) / 8;
        state.bearing = bearingFromX256(state.bearing_x256);
        state.bearingConfidence = 10;
        state.porkBehind = false;
    }

    // ==[ SILENT DECAY ]==
    if (config.expectedCadenceMs > 0 && state.bearingLocked &&
        state.lastDirectionalFeedTime > 0) {
        uint32_t silenceMs = now - state.lastDirectionalFeedTime;
        uint32_t silenceThreshold = config.expectedCadenceMs * 3;
        if (silenceMs > silenceThreshold) {
            bool veryStale = (now - state.lastReinforceTime > config.staleTimeout);
            uint32_t decayRate = veryStale ? config.decayRateWrong : config.decayRateStale;
            if (now - state.lastDecay > decayRate) {
                state.lockConfidence = (state.lockConfidence > 0) ? state.lockConfidence - 1 : 0;
                state.lastDecay = now;
                if (state.lockConfidence == 0) {
                    state.bearingLocked = false;
                    state.estimateState = EstimateState::SEEK;
                }
            }
        }
    }
}

void updateIMU(TrackerState& state, const TrackerConfig& config,
               float gx, float gy, float gz,
               float ax, float ay, float az,
               uint32_t now_param) {
    updateIMUImpl(state, config, gx, gy, gz, ax, ay, az,
                  false, 0.0f, nullptr, now_param);
}

void updateIMUAuthoritativeHeading(
    TrackerState& state, const TrackerConfig& config,
    float gx, float gy, float gz,
    float ax, float ay, float az,
    float authoritativeHeadingDeg,
    uint32_t now_param) {
    updateIMUImpl(state, config, gx, gy, gz, ax, ay, az,
                  true, authoritativeHeadingDeg, nullptr, now_param);
}

void updateIMUAuthoritativePose(
    TrackerState& state, const TrackerConfig& config,
    float gx, float gy, float gz,
    float ax, float ay, float az,
    const AuthoritativePoseSample& pose,
    uint32_t now_param) {
    updateIMUImpl(state, config, gx, gy, gz, ax, ay, az,
                  true, pose.headingDeg, &pose, now_param);
}

// ==[ feedRSSI ]== call ONLY when new beacon/frame arrives
static void feedRSSIImpl(TrackerState& state, const TrackerConfig& config,
                         int8_t rssi, int8_t rssiSmooth,
                         uint32_t observedMs, uint32_t now,
                         uint8_t evidenceFlags, uint8_t csiQuality,
                         const ObservationPose* observationPose,
                         bool directionalEvidence) {
    const float observedHeading =
        observationPose && observationPose->valid
            ? (float)observationPose->headingDegX10 / 10.0f
            : state.relativeHeading;

    // ==[ EMA-DELTA EARLY LOCK ]==
    int8_t earlyTrend = 0;
    if (state.prevFeedRssi > -128.0f) {
        float delta = rssiSmooth - state.prevFeedRssi;
        state.deltaEma = config.deltaEmaAlpha * delta + (1.0f - config.deltaEmaAlpha) * state.deltaEma;
        earlyTrend = constrain((int)(state.deltaEma * config.deltaEmaScale), -127, 127);
    }
    state.prevFeedRssi = rssiSmooth;

    // ==[ RSSI TREND WINDOW ]==
    state.rssiHistory[state.rssiHistoryIdx] = rssiSmooth;
    state.rssiHistoryIdx = (state.rssiHistoryIdx + 1) % 10;
    if (state.rssiHistoryCount < 10) state.rssiHistoryCount++;

    int8_t trend = 0;
    if (state.rssiHistoryCount >= 5) {
        int32_t sumXY = 0;
        int32_t sumY = 0;
        uint8_t n = state.rssiHistoryCount;

        for (uint8_t i = 0; i < n; i++) {
            uint8_t idx = (state.rssiHistoryIdx + 10 - n + i) % 10;
            sumY += state.rssiHistory[idx];
            sumXY += i * state.rssiHistory[idx];
        }

        int32_t sumX = (n * (n - 1)) / 2;
        int32_t sumX2 = (n * (n - 1) * (2*n - 1)) / 6;
        int32_t denom = n * sumX2 - sumX * sumX;

        if (denom > 0) {
            // Use int64_t to prevent overflow with extreme RSSI values
            int64_t numer = (int64_t)n * sumXY - (int64_t)sumX * sumY;
            trend = constrain((int32_t)(numer * 10 / denom), -127, 127);
        }
        state.rssiTrendSmooth = (int8_t)(state.rssiTrendSmooth * 0.7f + trend * 0.3f);
    } else {
        trend = earlyTrend;
    }
    state.rssiTrend = trend;

    // ==[ DERIVED STATE ]==
    bool isMoving = state.isMoving;
    uint32_t motionDuration = (state.motionStartTime > 0) ? (now - state.motionStartTime) : 0;

    // ==[ BEARING LOCK LOGIC ]==

    // Walking RSSI has no directional sign without a translation vector.
    // It may reinforce an existing sweep lock only when trend and heading agree.
    if (directionalEvidence &&
        state.bearingLocked && isMoving &&
        motionDuration > config.minMotionMs &&
        abs(trend) >= config.trendThreshold) {
        const float rel = fabsf(targetRelativeAngle(state.lockedHeading,
                                                     state.relativeHeading));
        const bool agrees = (trend > 0 && rel <= 45.0f) ||
                            (trend < 0 && rel >= 135.0f);
        if (agrees) {
            state.lockConfidence = min(100, state.lockConfidence +
                                            max(1, config.reinforceBoost / 2));
            state.lastReinforceTime = now;
            if (trend > 0 && rel <= 45.0f) {
                state.approachConfirmCount++;
            }
        }
    }

    // MODE 2: Rotation lock
    if (directionalEvidence &&
        state.wasRotating && rssiSmooth > state.peakRssi) {
        state.peakRssi = rssiSmooth;
        state.peakHeading = observedHeading;
    }
    if (directionalEvidence && state.wasRotating) {
        const uint8_t bin =
            (uint8_t)((int)(observedHeading / 30.0f) % 12);
        const uint8_t previousSamples = state.rotationBinSamples[bin];
        if (state.rotationBinSamples[bin] < 255u) {
            state.rotationBinSamples[bin]++;
        }
        if (state.rotationRfSamples < 255u) state.rotationRfSamples++;
        if (state.rotationBinSamples[bin] == 1u ||
            rssiSmooth > state.rotationBinPeak[bin]) {
            state.rotationBinPeak[bin] = rssiSmooth;
        }

        if (previousSamples == 0u) {
            state.rotationBinMeanX8[bin] =
                static_cast<int16_t>(rssiSmooth) * 8;
            state.rotationBinDeviationX8[bin] = 0u;
            state.rotationBinHeading[bin] = observedHeading;
        } else {
            const int16_t sampleX8 =
                static_cast<int16_t>(rssiSmooth) * 8;
            const int16_t residualX8 =
                sampleX8 - state.rotationBinMeanX8[bin];
            const uint16_t absResidualX8 =
                static_cast<uint16_t>(abs((int)residualX8));
            const int16_t clipX8 = static_cast<int16_t>(
                max(16, (int)state.rotationBinDeviationX8[bin] * 3 + 8));
            const int16_t robustResidual =
                constrain(residualX8, -clipX8, clipX8);
            const int16_t divisor = min((int)previousSamples + 1, 4);
            state.rotationBinMeanX8[bin] += robustResidual / divisor;
            const int32_t deviationDelta =
                (int32_t)absResidualX8 -
                (int32_t)state.rotationBinDeviationX8[bin];
            state.rotationBinDeviationX8[bin] = static_cast<uint16_t>(
                max(0, (int)state.rotationBinDeviationX8[bin] +
                           (int)(deviationDelta / 4)));

            const float headingDelta = targetRelativeAngle(
                observedHeading, state.rotationBinHeading[bin]);
            state.rotationBinHeading[bin] +=
                headingDelta / (float)min((int)previousSamples + 1, 4);
            while (state.rotationBinHeading[bin] < 0.0f) {
                state.rotationBinHeading[bin] += 360.0f;
            }
            while (state.rotationBinHeading[bin] >= 360.0f) {
                state.rotationBinHeading[bin] -= 360.0f;
            }
        }

        const uint8_t poseQuality =
            observationPose && observationPose->valid
                ? observationPose->quality
                : static_cast<uint8_t>(constrain(
                      100 - (int)(state.accelMotionEnergy * 2.0f),
                      20, 100));
        const uint32_t qualityTotal =
            (uint32_t)state.rotationBinQualitySum[bin] + poseQuality;
        state.rotationBinQualitySum[bin] = static_cast<uint16_t>(
            min((uint32_t)UINT16_MAX, qualityTotal));
    }

    // ==[ CONFIDENCE DECAY ]==
    if (state.bearingLocked) {
        bool movingWrong = isMoving && motionDuration > 500 && trend <= -config.trendThreshold;
        bool movingNeutral = isMoving && motionDuration > 1000 && abs(trend) < config.trendThreshold;
        bool veryStale = (now - state.lastReinforceTime > config.staleTimeout);

        bool withinCadence = config.expectedCadenceMs > 0 &&
            state.lastDirectionalFeedTime > 0 &&
            (now - state.lastDirectionalFeedTime <
             config.expectedCadenceMs * 3 / 2);

        if (!withinCadence && (movingWrong || movingNeutral || veryStale)) {
            uint32_t decayRate = movingWrong ? config.decayRateWrong :
                                 movingNeutral ? (config.decayRateStale / 2) : config.decayRateStale;

            if (now - state.lastDecay > decayRate) {
                state.lockConfidence = (state.lockConfidence > 0) ? state.lockConfidence - 1 : 0;
                state.lastDecay = now;

                if (state.lockConfidence == 0) {
                    state.bearingLocked = false;
                    state.estimateState = EstimateState::SEEK;
                }
            }
        }
    }

    state.lastFeedTime = now;
    if (directionalEvidence) {
        state.lastDirectionalFeedTime = now;
        appendRfPoint(state, rssi, rssiSmooth, observedMs,
                      evidenceFlags, csiQuality, observationPose);
    }
}

void feedRSSI(TrackerState& state, const TrackerConfig& config,
              int8_t rssi, int8_t rssiSmooth,
              uint32_t now_param) {
    const uint32_t now = resolveClock(now_param);
    feedRSSIImpl(state, config, rssi, rssiSmooth, now, now,
                 RF_EVIDENCE_PASSIVE, 0u, nullptr, true);
}

void feedRSSIObserved(TrackerState& state, const TrackerConfig& config,
                      int8_t rssi, int8_t rssiSmooth,
                      uint32_t observedMs, uint32_t now_param,
                      uint8_t evidenceFlags, uint8_t csiQuality) {
    const uint32_t now = resolveClock(now_param);
    feedRSSIImpl(state, config, rssi, rssiSmooth, observedMs, now,
                 evidenceFlags, csiQuality, nullptr, true);
}

void feedRSSIObservedPose(
    TrackerState& state, const TrackerConfig& config,
    int8_t rssi, int8_t rssiSmooth,
    uint32_t observedMs, uint32_t now_param,
    const ObservationPose& pose,
    uint8_t evidenceFlags, uint8_t csiQuality) {
    const uint32_t now = resolveClock(now_param);
    feedRSSIImpl(state, config, rssi, rssiSmooth, observedMs, now,
                 evidenceFlags, csiQuality, &pose, true);
}

void feedRSSIScalarObserved(
    TrackerState& state, const TrackerConfig& config,
    int8_t rssi, int8_t rssiSmooth,
    uint32_t observedMs, uint32_t now_param) {
    const uint32_t now = resolveClock(now_param);
    feedRSSIImpl(state, config, rssi, rssiSmooth, observedMs, now,
                 RF_EVIDENCE_PASSIVE, 0u, nullptr, false);
}

// ==[ LEGACY WRAPPER ]==
void update(TrackerState& state, const TrackerConfig& config,
            int8_t rssi, int8_t rssiSmooth,
            float gx, float gy, float gz,
            float ax, float ay, float az,
            uint32_t now) {
    updateIMU(state, config, gx, gy, gz, ax, ay, az, now);
    feedRSSI(state, config, rssi, rssiSmooth, now);
}

uint8_t getRfPointCount(const TrackerState& state) {
    return state.rfPointCount;
}

bool getRfPointNewest(const TrackerState& state, uint8_t newestIdx,
                      RfPoint& out) {
    if (newestIdx >= state.rfPointCount) return false;
    const uint8_t offset = (uint8_t)(newestIdx + 1);
    const uint8_t idx = (uint8_t)((state.rfPointHead + RF_POINT_MAX - offset) %
                                  RF_POINT_MAX);
    out = state.rfPoints[idx];
    return true;
}

bool addNewestRfPointEvidence(TrackerState& state, uint8_t evidenceFlags,
                              uint8_t csiQuality) {
    if (state.rfPointCount == 0u) return false;
    const uint8_t idx = (uint8_t)((state.rfPointHead + RF_POINT_MAX - 1u) %
                                  RF_POINT_MAX);
    RfPoint& point = state.rfPoints[idx];
    point.evidenceFlags |= evidenceFlags;
    if (csiQuality > point.csiQuality) point.csiQuality = csiQuality;
    return true;
}

}  // namespace Bearing
