/**
 * RF Utilities - RSSI smoothing, MAC formatting, proximity mapping
 *
 * ==[ RADIO MATH ]== shared formulas. no more copy-pasta.
 */
#pragma once

#include <Arduino.h>

namespace RFUtil {

// ==[ IIR SMOOTHING ]== exponential moving average with integer ratio
// ratio=4 means 3:1 weight (current*3 + new)/4
// ratio=16 means 15:1 weight (current*15 + new)/16
inline int8_t smoothIIR(int8_t current, int8_t newVal, uint8_t ratio) {
    if (current < -99) return newVal;  // init case
    return (int8_t)((current * (ratio - 1) + newVal) / ratio);
}

// ==[ PIECEWISE PROXIMITY ]== RSSI → proximity score (0-1000)
// close range (-50 to -15): higher resolution, maps to 500-1000
// far range (-85 to -50): compressed, maps to 0-500
inline int16_t mapProximity(int8_t rssi) {
    int8_t clamped = constrain(rssi, -85, -15);
    if (clamped >= -50) {
        return map(clamped, -50, -15, 500, 1000);
    } else {
        return map(clamped, -85, -50, 0, 500);
    }
}

// ==[ MAC FORMATTING ]== bssid to colon-separated string
// buf must be at least 18 bytes (17 chars + null)
inline void formatMAC(char* buf, const uint8_t* mac) {
    snprintf(buf, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// ==[ SIGNAL STRENGTH WEIGHTING ]== convert RSSI to weight factor (0-100)
// Stronger signals get higher weight for weighted averaging
// -30dB (very strong) → 100, -90dB (very weak) → 0
inline uint8_t rssiToWeight(int8_t rssi) {
    int8_t clamped = constrain(rssi, -90, -30);
    return (uint8_t)map(clamped, -90, -30, 0, 100);
}

}  // namespace RFUtil
