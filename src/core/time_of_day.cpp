/**
 * time_of_day.cpp — 4-phase day/night cycle implementation
 *
 * All palette shifts are additive modifiers applied AFTER RP::update().
 * No direct color writes — everything goes through Gfx:: helpers.
 */
#include "time_of_day.h"
#ifndef NATIVE_TEST
#include "../ui/menu_pig_render.h"
#endif
#include <math.h>

namespace TimeOfDay {

#ifndef NATIVE_TEST
namespace RP = MenuPigRender::RP;
#endif

// ==[ PHASE BOUNDARIES ]== (hours in 24h, floating)
static constexpr float kDawnStart  = 5.0f;
static constexpr float kDawnEnd    = 7.0f;
static constexpr float kDayStart   = 7.0f;
static constexpr float kDayEnd     = 19.0f;
static constexpr float kDuskStart  = 19.0f;
static constexpr float kDuskEnd    = 21.0f;
static constexpr float kNightStart = 21.0f;
static constexpr float kNightEnd   = 29.0f;  // 5:00 next day (24+5)

// ==[ PHASE FROM HOUR ]==
static Phase phaseFromHour(float h) {
    if (h >= kDawnStart && h < kDawnEnd)   return DAWN;
    if (h >= kDayStart  && h < kDayEnd)    return DAY;
    if (h >= kDuskStart && h < kDuskEnd)   return DUSK;
    return NIGHT;
}

// ==[ PHASE PROGRESS ]==
// 0.0 at phase start, 1.0 at phase end
static float phaseProgress(float hour, Phase phase) {
    switch (phase) {
        case DAWN:  return (hour - kDawnStart)  / (kDawnEnd  - kDawnStart);
        case DAY:   return (hour - kDayStart)   / (kDayEnd   - kDayStart);
        case DUSK:  return (hour - kDuskStart)  / (kDuskEnd  - kDuskStart);
        case NIGHT: {
            float nh = (hour >= kNightStart) ? hour : hour + 24.0f;
            return (nh - kNightStart) / (kNightEnd - kNightStart);
        }
    }
    return 0.0f;
}

// ==[ SUN ANGLE ]==
// -PI/2 at dawn, 0 at noon, +PI/2 at dusk, stays below horizon at night
static float sunAngleFromHour(float hour) {
    float t;
    if (hour >= kDawnStart && hour <= kDuskEnd) {
        float mid = (kDawnStart + kDuskEnd) * 0.5f;  // noon = 13.0
        t = (hour - mid) / (mid - kDawnStart);  // -1 at dawn, +1 at dusk
    } else {
        t = 2.0f;  // well below horizon
    }
    return t * 1.5708f;  // PI/2 range
}

// ==[ MOON PHASE ]==
// 29.5-day synodic cycle, deterministic from epoch
static float moonPhaseFromMs(uint64_t epochMs) {
    static constexpr uint64_t kMoonCycleMs =
        (29ull * 24ull * 3600ull + 12ull * 3600ull) * 1000ull;
    return (float)(epochMs % kMoonCycleMs) / (float)kMoonCycleMs;
}

// ==[ LERP HELPERS ]==
static uint16_t lerpRGB565(uint16_t a, uint16_t b, float t) {
#ifdef NATIVE_TEST
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    const int ar = (a >> 11) & 0x1F;
    const int ag = (a >> 5) & 0x3F;
    const int ab = a & 0x1F;
    const int br = (b >> 11) & 0x1F;
    const int bg = (b >> 5) & 0x3F;
    const int bb = b & 0x1F;
    const int rr = (int)lroundf((float)ar + (float)(br - ar) * t);
    const int rg = (int)lroundf((float)ag + (float)(bg - ag) * t);
    const int rb = (int)lroundf((float)ab + (float)(bb - ab) * t);
    return (uint16_t)((rr << 11) | (rg << 5) | rb);
#else
    return ::Display::lerpColor565(a, b, t);
#endif
}

static float smoothstep(float t) {
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    return t * t * (3.0f - 2.0f * t);
}

// ==[ COMPUTE ]==
State compute(uint64_t epochMs) {
    State s{};

    // Hours since midnight (0.0 - 24.0)
    uint32_t secOfDay = (uint32_t)((epochMs / 1000ull) % (24ull * 3600ull));
    float hour = (float)secOfDay / 3600.0f;

    s.dayProgress = hour / 24.0f;
    s.phase = phaseFromHour(hour);
    s.phaseProgress = phaseProgress(hour, s.phase);
    s.sunAngle = sunAngleFromHour(hour);
    s.moonPhase = moonPhaseFromMs(epochMs);

    // ==[ SKY COLORS ]==
    // DAWN: deep indigo -> pale blue, warm pink/orange horizon
    // DAY: cyan zenith, pale cyan horizon
    // DUSK: pale blue -> indigo, orange/purple horizon
    // NIGHT: near-black zenith, deep indigo horizon
    static constexpr uint16_t nightZenith   = 0x0001;   // near-black
    static constexpr uint16_t nightHorizon  = 0x0812;   // deep indigo
    static constexpr uint16_t dawnZenithA   = 0x0812;   // deep indigo
    static constexpr uint16_t dawnZenithB   = 0x428C;   // pale blue
    static constexpr uint16_t dawnHorizonA  = 0x4008;   // deep indigo-ish
    static constexpr uint16_t dawnHorizonB  = 0xFCA8;   // warm pink/orange
    static constexpr uint16_t dayZenith     = 0x56BC;   // cyan-blue
    static constexpr uint16_t dayHorizon    = 0xBDD7;   // pale cyan
    static constexpr uint16_t duskZenithA   = 0x56BC;   // pale blue
    static constexpr uint16_t duskZenithB   = 0x0812;   // indigo
    static constexpr uint16_t duskHorizonA  = 0xFCA0;   // orange
    static constexpr uint16_t duskHorizonB  = 0x4010;   // purple-ish

    switch (s.phase) {
        case DAWN: {
            float t = smoothstep(s.phaseProgress);
            s.skyZenith  = lerpRGB565(dawnZenithA, dawnZenithB, t);
            s.skyHorizon = lerpRGB565(dawnHorizonA, dawnHorizonB, t);
            break;
        }
        case DAY: {
            s.skyZenith  = dayZenith;
            s.skyHorizon = dayHorizon;
            break;
        }
        case DUSK: {
            float t = smoothstep(s.phaseProgress);
            s.skyZenith  = lerpRGB565(duskZenithA, duskZenithB, t);
            s.skyHorizon = lerpRGB565(duskHorizonA, duskHorizonB, t);
            break;
        }
        case NIGHT: {
            s.skyZenith  = nightZenith;
            s.skyHorizon = nightHorizon;
            break;
        }
    }

    // ==[ SUN / MOON COLORS ]==
    float daylightProgress = (hour - kDawnStart) / (kDuskEnd - kDawnStart);
    daylightProgress = daylightProgress < 0.0f ? 0.0f :
                       (daylightProgress > 1.0f ? 1.0f : daylightProgress);
    float sunHeat = 1.0f - fabsf(daylightProgress * 2.0f - 1.0f);
    s.sunColor  = lerpRGB565(0xFCA0, 0xFFE0, smoothstep(sunHeat));
    s.moonColor = lerpRGB565(0xA514, 0xE73C, s.moonPhase < 0.5f ? s.moonPhase * 2.0f : (1.0f - s.moonPhase) * 2.0f);

    // ==[ STARS ]==
    s.starsVisible = (s.phase == NIGHT) || (s.phase == DAWN && s.phaseProgress < 0.3f)
                  || (s.phase == DUSK && s.phaseProgress > 0.7f);
    if (s.phase == NIGHT) {
        s.starDensity = (s.phaseProgress < 0.1f || s.phaseProgress > 0.9f) ? 200 : 128;
    } else if (s.starsVisible) {
        s.starDensity = 60;
    } else {
        s.starDensity = 0;
    }

    // ==[ SHADOW LENGTH ]==
    // Short at noon (1.0), long at dawn/dusk (3.0), infinite at night
    float sunElev = cosf(s.sunAngle);  // 1 at noon, 0 at horizon, negative below
    if (sunElev <= 0.0f) {
        s.shadowLength = 3.0f;
    } else {
        s.shadowLength = 1.0f + 2.0f * (1.0f - sunElev);
    }

    // ==[ NEON BOOST ]==
    // 0.0 during day, ramps to 1.0 at night — used for emissive intensity
    switch (s.phase) {
        case DAY:   s.neonBoost = 0.0f; break;
        case DAWN:  s.neonBoost = 1.0f - smoothstep(s.phaseProgress); break;
        case DUSK:  s.neonBoost = smoothstep(s.phaseProgress); break;
        case NIGHT: s.neonBoost = 1.0f; break;
    }

    return s;
}

State computeSessionRelative(uint32_t sessionMs, uint8_t sessionStartHour) {
    uint64_t fakeEpoch = (uint64_t)(sessionStartHour % 24u) * 3600ull * 1000ull +
                         (uint64_t)sessionMs;
    return compute(fakeEpoch);
}

// ==[ APPLY TO RP ]==
// Modifies emissive palette entries based on time-of-day state.
// Called AFTER RP::update() — frame-local nighttime contrast on emissives.
void applyToRP(const State& s) {
#ifdef NATIVE_TEST
    (void)s;
#else
    if (s.neonBoost <= 0.01f) return;  // daytime — no modification

    // Raise contrast toward the active theme foreground, not hardcoded white.
    // lerp (rather than screen blend) also preserves inverted-theme semantics.
    uint8_t boost8 = (uint8_t)(s.neonBoost * 48.0f);  // max 48/255 blend
    const uint16_t contrastTarget = Display::getColorFG();

    RP::NEON     = MenuPigRender::lerpColor565_8(RP::NEON, contrastTarget, boost8);
    RP::CRT      = MenuPigRender::lerpColor565_8(RP::CRT, contrastTarget, boost8 / 2);
    RP::WARM     = MenuPigRender::lerpColor565_8(RP::WARM, contrastTarget, boost8 / 2);
    RP::PUDDLE   = MenuPigRender::lerpColor565_8(RP::PUDDLE, contrastTarget, boost8 / 3);
    RP::LED      = MenuPigRender::lerpColor565_8(RP::LED, contrastTarget, boost8 / 3);
    RP::SPARK    = MenuPigRender::lerpColor565_8(RP::SPARK, contrastTarget, boost8 / 4);
    RP::GREEN_DK = MenuPigRender::lerpColor565_8(RP::GREEN_DK, contrastTarget, boost8 / 4);

    // Recompute pre-darkened variants with boosted base
    static constexpr uint8_t kNoirT8    = 102;
    static constexpr uint8_t kNoirT8_50 = 128;
    RP::D_STRUCT    = MenuPigRender::lerpColor565_8(RP::STRUCT,    RP::BG, kNoirT8);
    RP::D_WALL_NEAR = MenuPigRender::lerpColor565_8(RP::WALL_NEAR, RP::BG, kNoirT8);
    RP::D_FILL      = MenuPigRender::lerpColor565_8(RP::FILL,      RP::BG, kNoirT8);
    RP::D_DEEP      = MenuPigRender::lerpColor565_8(RP::DEEP,      RP::BG, kNoirT8);
    RP::D_WARM      = MenuPigRender::lerpColor565_8(RP::WARM,      RP::BG, kNoirT8);
    RP::D50_STRUCT  = MenuPigRender::lerpColor565_8(RP::STRUCT,    RP::BG, kNoirT8_50);
    RP::D50_FILL    = MenuPigRender::lerpColor565_8(RP::FILL,      RP::BG, kNoirT8_50);
#endif
}

} // namespace TimeOfDay
