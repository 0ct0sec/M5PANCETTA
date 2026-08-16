#pragma once

#include <math.h>
#include <stdint.h>

namespace RfPoseMath {

static constexpr float FLAT_EXIT_AZ_G = 0.65f;
static constexpr float FLAT_ENTER_AZ_G = 0.75f;
static constexpr float UPRIGHT_EDGE_HYSTERESIS_G = 0.15f;
// RF bearing is an operator aid, so reject sensor creep and physically
// implausible single-axis spikes before they can move a retained contact.
// 360 dps still permits a deliberate 90-degree turn in a quarter second.
static constexpr float YAW_RATE_DEADBAND_DPS = 1.5f;
static constexpr float YAW_RATE_MAX_DPS = 360.0f;

inline bool hystereticFlat(bool wasFlat, float az) {
    const float absAz = fabsf(az);
    return wasFlat ? absAz > FLAT_EXIT_AZ_G
                   : absAz > FLAT_ENTER_AZ_G;
}

inline float boundedYawRateDps(float yawRateDps) {
    if (fabsf(yawRateDps) <= YAW_RATE_DEADBAND_DPS) return 0.0f;
    if (yawRateDps > YAW_RATE_MAX_DPS) return YAW_RATE_MAX_DPS;
    if (yawRateDps < -YAW_RATE_MAX_DPS) return -YAW_RATE_MAX_DPS;
    return yawRateDps;
}

// A flat/upright grip transition changes the selected device axis. It is not
// an operator yaw sample, so never integrate that transition into world pose.
inline float boundedYawDeltaDeg(float yawRateDps, float elapsedSeconds,
                                bool gripChanged) {
    if (gripChanged || elapsedSeconds <= 0.0f) return 0.0f;
    return boundedYawRateDps(yawRateDps) * elapsedSeconds;
}

inline bool interpolationSpanValid(uint32_t beforeUs, uint32_t afterUs,
                                   uint32_t maxSpanUs) {
    const uint32_t spanUs = afterUs - beforeUs;
    return spanUs > 0u && spanUs <= maxSpanUs;
}

inline uint8_t yawPoseForGravity(bool isFlat, float ax, float ay,
                                 uint8_t previousPose) {
    if (isFlat) return 1u;

    const float absAx = fabsf(ax);
    const float absAy = fabsf(ay);
    const uint8_t xPose = ax >= 0.0f ? 2u : 3u;
    const uint8_t yPose = ay >= 0.0f ? 4u : 5u;

    if (previousPose == 2u || previousPose == 3u) {
        if (absAy > absAx + UPRIGHT_EDGE_HYSTERESIS_G) return yPose;
        return absAx > UPRIGHT_EDGE_HYSTERESIS_G
            ? xPose : previousPose;
    }
    if (previousPose == 4u || previousPose == 5u) {
        if (absAx > absAy + UPRIGHT_EDGE_HYSTERESIS_G) return xPose;
        return absAy > UPRIGHT_EDGE_HYSTERESIS_G
            ? yPose : previousPose;
    }
    return absAx >= absAy ? xPose : yPose;
}

inline void deviceToScreen(bool isFlat, uint8_t yawPose,
                           float x, float y, float z,
                           float& screenX, float& screenY) {
    if (isFlat) {
        screenX = x;
        screenY = y;
        return;
    }

    switch (yawPose) {
        case 2u:
            screenX = y;
            break;
        case 3u:
            screenX = -y;
            break;
        case 4u:
            screenX = -x;
            break;
        case 5u:
            screenX = x;
            break;
        default:
            if (fabsf(x) >= fabsf(y)) {
                screenX = x >= 0.0f ? y : -y;
            } else {
                screenX = y >= 0.0f ? -x : x;
            }
            break;
    }
    screenY = -z;
}

inline void screenToWorld(float headingDeg,
                          float screenX, float screenY,
                          float& worldX, float& worldY) {
    const float headingRad = headingDeg * 0.017453292519943295f;
    const float c = cosf(headingRad);
    const float s = sinf(headingRad);
    worldX = c * screenX + s * screenY;
    worldY = -s * screenX + c * screenY;
}

inline bool timestampIsNewer(uint32_t candidate, uint32_t previous) {
    return static_cast<int32_t>(candidate - previous) > 0;
}

inline float timeBlendAlpha(uint32_t elapsedUs, float tauSeconds) {
    if (elapsedUs == 0u) return 0.0f;
    if (tauSeconds <= 0.0f) return 1.0f;
    const float elapsedSeconds = (float)elapsedUs / 1000000.0f;
    return 1.0f - expf(-elapsedSeconds / tauSeconds);
}

}  // namespace RfPoseMath
