/**
 * Geiger scan math - host-testable screen projection helpers.
 *
 * ==[ FRAME CONTRACT ]== bearing is already operator-relative. display
 * rotation only transforms accelerometer-derived motion vectors.
 */
#pragma once

#include <math.h>
#include <stdint.h>

namespace GeigerScanMath {

static inline int clampInt(int value, int lo, int hi) {
    return value < lo ? lo : (value > hi ? hi : value);
}

static inline int divRoundSigned(int value, int divisor) {
    if (divisor == 0) return 0;
    return value >= 0 ? (value + divisor / 2) / divisor
                      : (value - divisor / 2) / divisor;
}

static inline int normX10(int value) {
    while (value >= 3600) value -= 3600;
    while (value < 0) value += 3600;
    return value;
}

static inline int circularX10Delta(int target, int current) {
    int delta = normX10(target) - normX10(current);
    while (delta > 1800) delta -= 3600;
    while (delta < -1800) delta += 3600;
    return delta;
}

static inline int bearingX10(int bearing) {
    return clampInt(bearing, -100, 100) * 9;
}

static inline int bearingDegrees(int bearing) {
    return divRoundSigned(bearingX10(bearing), 10);
}

// Cold RAD has no RF direction yet. Keep its dotted SEEK ray tied to the
// heading where the scan began so handset yaw is still visible without
// pretending RSSI supplied left/right evidence.
static inline int seekRelativeDegrees(uint16_t referenceHeadingX10,
                                      uint16_t currentHeadingX10) {
    return clampInt(divRoundSigned(
                        circularX10Delta((int)referenceHeadingX10,
                                         (int)currentHeadingX10), 10),
                    -180, 180);
}

static inline bool radarBehind(bool anchoredPosition,
                               bool staleContact,
                               bool trackerBehind,
                               bool liveBearing,
                               int relativeDegrees) {
    // SEEK yaw proves handset motion only. It cannot put an RF target behind.
    return !anchoredPosition && !staleContact &&
           (trackerBehind ||
            (liveBearing && (relativeDegrees < -95 || relativeDegrees > 95)));
}

// New sweep locks and aligned approach evidence both supersede an old LKP.
static inline bool anchorNeedsRefresh(bool anchorValid,
                                      uint32_t lockGeneration,
                                      uint32_t anchoredLockGeneration,
                                      uint32_t approachConfirmCount,
                                      uint32_t anchoredApproachConfirmCount) {
    return !anchorValid ||
           lockGeneration != anchoredLockGeneration ||
           approachConfirmCount != anchoredApproachConfirmCount;
}

static inline uint16_t anchorHeadingX10(uint16_t currentHeadingX10,
                                        int bearing) {
    return (uint16_t)normX10((int)currentHeadingX10 + bearingX10(bearing));
}

// A retained contact is stored in the world frame. Reproject it from the
// current IMU heading every frame so a RAD/THRU posture switch cannot reset
// the target to the centre of the fan.
static inline int anchoredRelativeDegrees(uint16_t anchorHeadingX10,
                                         uint16_t currentHeadingX10) {
    return clampInt(divRoundSigned(
                        circularX10Delta((int)anchorHeadingX10,
                                         (int)currentHeadingX10), 10),
                    -180, 180);
}

static inline int relativeDegrees(uint16_t sampleHeadingX10,
                                  int bearing,
                                  uint16_t currentHeadingX10) {
    const int anchor = normX10((int)sampleHeadingX10 + bearingX10(bearing));
    return clampInt(divRoundSigned(
                        circularX10Delta(anchor, (int)currentHeadingX10), 10),
                    -120, 120);
}

static inline int motionComponent(int component, int8_t motionScreenSign) {
    const int value = clampInt(component, -100, 100);
    return motionScreenSign < 0 ? -value : value;
}

static inline uint8_t bearingConfidence(uint8_t lockConfidence,
                                        uint8_t historyConfidence) {
    const int lock = clampInt((int)lockConfidence, 0, 100);
    const int history = clampInt((int)historyConfidence, 0, 100);
    return (uint8_t)((lock * 3 + history * 2) / 5);
}

static inline void retainedMotionPixels(float anchorWorldX,
                                        float anchorWorldY,
                                        float currentWorldX,
                                        float currentWorldY,
                                        uint16_t currentHeadingX10,
                                        int8_t motionScreenSign,
                                        int pixelsPerMeter,
                                        int maxPixels,
                                        int& outX,
                                        int& outY) {
    const float heading = (float)normX10((int)currentHeadingX10) *
                          0.0017453292519943296f;
    const float c = cosf(heading);
    const float s = sinf(heading);
    const float worldX = currentWorldX - anchorWorldX;
    const float worldY = currentWorldY - anchorWorldY;
    const float localX = c * worldX - s * worldY;
    const float localY = s * worldX + c * worldY;
    const float sign = motionScreenSign < 0 ? -1.0f : 1.0f;
    const float pixelX = -localX * (float)pixelsPerMeter * sign;
    const float pixelY = localY * (float)pixelsPerMeter * sign;
    const int roundedX = (int)(pixelX >= 0.0f ? pixelX + 0.5f : pixelX - 0.5f);
    const int roundedY = (int)(pixelY >= 0.0f ? pixelY + 0.5f : pixelY - 0.5f);
    outX = clampInt(roundedX, -maxPixels, maxPixels);
    outY = clampInt(roundedY, -maxPixels, maxPixels);
}

}  // namespace GeigerScanMath
