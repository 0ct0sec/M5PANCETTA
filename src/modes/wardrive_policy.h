#pragma once

#include <stdint.h>

namespace WardrivePolicy {

// JanOS dual-band scans take about 12-13 seconds on the bench C5. Hold the
// checksum-valid RMC fix long enough for that bounded scan to finish, while
// still rejecting coordinates from an earlier scan cycle.
static constexpr uint32_t C5_COORD_TTL_MS = 18000;

// The telemetry tape is deliberately slower than the cockpit. Ten updates per
// second keep the fastest supported GPS UART serviced without paying for sixty
// full cockpit composites that the operator switched away from.
static constexpr uint8_t TELEMETRY_TARGET_FPS = 10;
static constexpr uint32_t TELEMETRY_REDRAW_MS = 500;
static constexpr uint32_t FIX_WARNING_REPEAT_MS = 6000;
static constexpr uint32_t C5_SCAN_TELEMETRY_TTL_MS = 30000;

enum class CoordinateSource : uint8_t {
    NONE = 0,
    CORE_GPS,
    C5_UART,
};

enum class TelemetryFixState : uint8_t {
    GPS_OFF = 0,
    NO_UART_DATA,
    INVALID_NMEA,
    ACQUIRING,
    LOCKED,
};

struct CoordinateChoice {
    CoordinateSource source;
    double latitude;
    double longitude;
    float altitudeMeters;
    float accuracyMeters;
};

inline bool isFreshC5Coordinate(bool hasCoordinate,
                                uint32_t coordinateMs,
                                uint32_t nowMs) {
    return hasCoordinate && coordinateMs != 0 &&
           (nowMs - coordinateMs) <= C5_COORD_TTL_MS;
}

// A coordinate parsed from the C5 is only usable while its transport is
// still live.  Keeping a recently received value after a disconnect makes
// the HUD and WiGLE rows look current even though the C5 can no longer vouch
// for it; callers must fall back to the Core GPS in that state.
inline bool isUsableC5Coordinate(bool c5Connected,
                                 bool hasCoordinate,
                                 uint32_t coordinateMs,
                                 uint32_t nowMs) {
    return c5Connected &&
           isFreshC5Coordinate(hasCoordinate, coordinateMs, nowMs);
}

inline bool isFreshC5ScanTelemetry(bool c5Connected,
                                   uint32_t completedMs,
                                   uint32_t nowMs) {
    return c5Connected && completedMs != 0 &&
           (nowMs - completedMs) <= C5_SCAN_TELEMETRY_TTL_MS;
}

inline CoordinateChoice chooseCoordinates(
    uint32_t nowMs,
    bool c5Connected,
    bool c5HasCoordinate,
    uint32_t c5CoordinateMs,
    double c5Latitude,
    double c5Longitude,
    bool coreHasFix,
    double coreLatitude,
    double coreLongitude,
    float coreAltitudeMeters,
    float coreAccuracyMeters) {
    if (isUsableC5Coordinate(c5Connected, c5HasCoordinate,
                             c5CoordinateMs, nowMs)) {
        return {
            CoordinateSource::C5_UART,
            c5Latitude,
            c5Longitude,
            0.0f,
            0.0f,
        };
    }
    if (coreHasFix) {
        return {
            CoordinateSource::CORE_GPS,
            coreLatitude,
            coreLongitude,
            coreAltitudeMeters,
            coreAccuracyMeters,
        };
    }
    return {
        CoordinateSource::NONE,
        0.0,
        0.0,
        0.0f,
        0.0f,
    };
}

inline bool isC5WardriveChannel(bool is5GHz, uint8_t channel) {
    return is5GHz && channel >= 36 && channel <= 165;
}

inline TelemetryFixState classifyTelemetryFixState(bool positionFix,
                                                   bool gpsRunning,
                                                   bool hasUartData,
                                                   bool hasNmea) {
    // A fresh C5 position is a valid Wardrive navigation fix even when the Core
    // GPS UART is intentionally off or owned by another peripheral.
    if (positionFix) return TelemetryFixState::LOCKED;
    if (!gpsRunning) return TelemetryFixState::GPS_OFF;
    if (!hasUartData) return TelemetryFixState::NO_UART_DATA;
    if (!hasNmea) return TelemetryFixState::INVALID_NMEA;
    return TelemetryFixState::ACQUIRING;
}

inline bool warningDeadlineReached(uint32_t nowMs, uint32_t warningAtMs) {
    return warningAtMs == 0 ||
           static_cast<int32_t>(nowMs - warningAtMs) >= 0;
}

} // namespace WardrivePolicy
