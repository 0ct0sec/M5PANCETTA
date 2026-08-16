/**
 * time_of_day.h — 4-phase day/night cycle for HAMLET PANCETTA
 *
 * Drives global palette shifts, sky rendering, star/sun/moon visibility.
 * Epoch-based: compute() from RTC or session-relative ms, then applyToRP().
 *
 * Phases:
 *   DAWN  (05:00-07:00)  Deep indigo -> pale blue, warm horizon, rising sun
 *   DAY   (07:00-19:00)  Cyan zenith, pale horizon, hot sun, short shadows
 *   DUSK  (19:00-21:00)  Pale -> indigo, orange/purple horizon, neon igniting
 *   NIGHT (21:00-05:00)  Near-black zenith, deep indigo horizon, moon + stars
 */
#pragma once

#include <cstdint>

namespace TimeOfDay {

enum Phase : uint8_t { DAWN, DAY, DUSK, NIGHT };

struct State {
    Phase phase;
    float dayProgress;     // 0.0 (midnight) -> 1.0 (next midnight)
    float sunAngle;        // radians: -PI/2 (dawn) -> PI/2 (dusk), 0 at noon
    float moonPhase;       // 0.0 -> 1.0 (29.5-day cycle)
    float phaseProgress;   // 0.0 -> 1.0 within current phase
    uint16_t skyZenith;    // RGB565 interpolated
    uint16_t skyHorizon;   // RGB565 interpolated
    uint16_t sunColor;     // RGB565 warm -> hot -> warm
    uint16_t moonColor;    // RGB565 cool -> neutral
    bool starsVisible;
    uint8_t starDensity;   // 0 = none, 255 = full
    float shadowLength;    // 1.0 = short (noon), 3.0 = long (dawn/dusk)
    float neonBoost;       // 0.0 (day) -> 1.0 (night) — for RP emissive scaling
};

// Compute state from epoch milliseconds (RTC or session-relative). Real epoch
// milliseconds do not fit in 32 bits, so keep the public clock input wide.
State compute(uint64_t epochMs);

// Session-relative variant: milliseconds since boot plus a local start hour.
State computeSessionRelative(uint32_t sessionMs, uint8_t sessionStartHour);

// Apply time-of-day modifiers to RP:: palette globals.
// Should be called once per frame AFTER RP::update().
void applyToRP(const State& s);

// Quick query: is it nighttime? (for weather, neon, etc.)
inline bool isNight(const State& s) { return s.phase == NIGHT || s.phase == DUSK; }

} // namespace TimeOfDay
