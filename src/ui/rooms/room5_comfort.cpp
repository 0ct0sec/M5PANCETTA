/** room5_comfort.cpp - Comfort Balcony (hot bath, rain glass, warm neon city) */

#include "../menu_pig_internal.h"
#include "exterior_sprites.h"
#include "../pixel_furniture.h"
#include "../smoke_volume.h"
#include <stdint.h>

namespace MenuPig {

// ==[ DEPTH PLANES ]== the balcony was the only room still nailed to its
// backdrop: parallaxFar reached the city behind the glass and stopped there, so
// tilting the device slid the skyline while every object in front of it stayed
// rigid. That reads as parallax being broken here, not as a shallow room.
//
// Plane assignment now matches rooms 0-4:
//   far  — wall-mounted fixtures (the sconce) and the city behind the pane
//   mid  — the tub and everything the water touches
//   near — the balcony deck under all of it
//
// Two things deliberately stay put. The glass plate is full-bleed (x=4..316)
// and parallaxes internally through backdrop.parallaxX, so its frame, seals and
// mullions hold still — the same rule the envelope block in ui_measurements.h
// states for every window plate. And Pancetta keeps his own plane: he is an
// actor, not a camera. The bath water band is 160px against a 72px body with
// 52px of margin either side, so the tub can slide its 4px without ever
// uncovering him (verified across the full mid envelope).

static constexpr uint32_t kComfortWindowCondensationSalt = 0xB501u;
static constexpr int kComfortWindowCondensationCount = 10;
static constexpr uint8_t kComfortWindowRainIntensity = 96;
static constexpr uint8_t kComfortClockAlpha = 116;
static constexpr uint8_t kComfortBeamMinStrength = 128;
static_assert(kComfortBeamMinStrength > 72u,
              "fallback holo reflections must not open the bath volume");

// Authored 5x7 decorative digits. Utility font stretching would violate the
// room pixel contract and cannot produce the viscous per-column melt.
static constexpr uint8_t kComfortClockDigits[10][7] = {
    {0x0Eu, 0x11u, 0x13u, 0x15u, 0x19u, 0x11u, 0x0Eu}, // 0
    {0x04u, 0x0Cu, 0x04u, 0x04u, 0x04u, 0x04u, 0x0Eu}, // 1
    {0x0Eu, 0x11u, 0x01u, 0x02u, 0x04u, 0x08u, 0x1Fu}, // 2
    {0x1Eu, 0x01u, 0x01u, 0x0Eu, 0x01u, 0x01u, 0x1Eu}, // 3
    {0x02u, 0x06u, 0x0Au, 0x12u, 0x1Fu, 0x02u, 0x02u}, // 4
    {0x1Fu, 0x10u, 0x10u, 0x1Eu, 0x01u, 0x01u, 0x1Eu}, // 5
    {0x0Eu, 0x10u, 0x10u, 0x1Eu, 0x11u, 0x11u, 0x0Eu}, // 6
    {0x1Fu, 0x01u, 0x02u, 0x04u, 0x08u, 0x08u, 0x08u}, // 7
    {0x0Eu, 0x11u, 0x11u, 0x0Eu, 0x11u, 0x11u, 0x0Eu}, // 8
    {0x0Eu, 0x11u, 0x11u, 0x0Fu, 0x01u, 0x01u, 0x0Eu}, // 9
};

struct ComfortClockFrame {
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
    bool valid;
};

static ComfortClockFrame sampleComfortClock(uint32_t now) {
    static ComfortClockFrame frame = {};
    static uint32_t sampledAt = 0;
    static bool sampled = false;
    if (sampled && (uint32_t)(now - sampledAt) < 1000u) return frame;

    sampled = true;
    sampledAt = now;
    frame.valid = false;
    if (!M5.Rtc.isEnabled()) return frame;

    const auto dt = M5.Rtc.getDateTime();
    if (dt.time.hours > 23u || dt.time.minutes > 59u ||
        dt.time.seconds > 59u) {
        return frame;
    }
    frame.hour = dt.time.hours;
    frame.minute = dt.time.minutes;
    frame.second = dt.time.seconds;
    frame.valid = true;
    return frame;
}

static void drawComfortClockGlassCell(M5Canvas& canvas,
                                      int x, int y, int w, int h,
                                      uint16_t tint, uint8_t alpha) {
    // Blend every 4px substrate cell independently. Sampling one color for an
    // entire 8px digit block would flatten city detail behind the projection.
    for (int py = y; py < y + h; py += kRoomPX) {
        for (int px = x; px < x + w; px += kRoomPX) {
            const uint16_t base = fastReadPx(canvas, px, py);
            canvas.fillRect(px, py, kRoomPX, kRoomPX,
                            lerpColor565_8(base, tint, alpha));
        }
    }
}

static int comfortClockBottomRow(int digit, int col) {
    // Invalid RTC data renders a three-cell dash. Blank outer columns must not
    // grow orphan drips underneath pixels that do not exist.
    if (digit < 0 || digit > 9)
        return col >= 1 && col <= 3 ? 3 : -1;
    for (int row = 6; row >= 0; --row) {
        if ((kComfortClockDigits[digit][row] &
             (uint8_t)(1u << (4 - col))) != 0u) {
            return row;
        }
    }
    return -1;
}

static void drawComfortClockDrip(M5Canvas& canvas,
                                 int anchorX, int anchorY,
                                 uint32_t now, uint32_t seed,
                                 uint16_t tint) {
    const uint32_t period = 5200u + (seed % 2400u);
    const uint32_t local = (now + (seed >> 8)) % period;
    const uint32_t half = period / 2u;
    const uint32_t triangleQ8 =
        local <= half
            ? (local << 8) / max(1u, half)
            : ((period - local) << 8) / max(1u, period - half);
    const uint32_t maxCells = 2u + ((seed >> 4) % 5u);
    const uint32_t lengthQ8 = triangleQ8 * maxCells;
    const uint32_t fullCells = lengthQ8 >> 8;
    const uint8_t headBlend = (uint8_t)(lengthQ8 & 0xFFu);
    const int dripX = anchorX + (((seed >> 12) & 1u) ? 0 : kRoomPX);

    for (uint32_t cell = 0; cell < fullCells; ++cell) {
        const uint8_t fade = (uint8_t)max(
            36, 84 - (int)cell * 8);
        drawComfortClockGlassCell(canvas, dripX,
                                  anchorY + (int)cell * kRoomPX,
                                  kRoomPX, kRoomPX, tint, fade);
    }
    if (headBlend != 0u && fullCells < maxCells) {
        const uint8_t alpha = (uint8_t)(
            ((uint16_t)76u * headBlend + 127u) / 255u);
        drawComfortClockGlassCell(canvas, dripX,
                                  anchorY + (int)fullCells * kRoomPX,
                                  kRoomPX, kRoomPX, tint, alpha);
    }
}

static void drawComfortClockDigit(M5Canvas& canvas,
                                  int x, int digit, uint32_t now,
                                  uint32_t salt, uint16_t tint) {
    for (int row = 0; row < 7; ++row) {
        const uint8_t bits = digit >= 0 && digit <= 9
            ? kComfortClockDigits[digit][row]
            : (row == 3 ? 0x0Eu : 0u);
        for (int col = 0; col < 5; ++col) {
            if ((bits & (uint8_t)(1u << (4 - col))) == 0u) continue;
            const uint8_t alpha = (uint8_t)(
                kComfortClockAlpha - (row >= 5 ? 12u : 0u));
            drawComfortClockGlassCell(
                canvas, x + col * kR6_ClockCell,
                kR6_ClockY + row * kR6_ClockCell,
                kR6_ClockCell, kR6_ClockCell, tint, alpha);
        }
    }

    for (int col = 0; col < 5; ++col) {
        // Position owns melt timing/topology. The displayed value may change
        // its anchor row, but must not reroll every drip lane once per minute.
        const uint32_t seed = wallHash(col, (int)salt, 0xC10CDu);
        if ((seed & 3u) == 0u) continue;
        const int bottom = comfortClockBottomRow(digit, col);
        if (bottom < 0) continue;
        drawComfortClockDrip(
            canvas, x + col * kR6_ClockCell,
            kR6_ClockY + (bottom + 1) * kR6_ClockCell,
            now, seed, tint);
    }
}

static void drawComfortGlassClock(M5Canvas& canvas, uint32_t now,
                                  uint16_t exteriorTint) {
    const ComfortClockFrame clock = sampleComfortClock(now);
    const int digits[4] = {
        clock.valid ? clock.hour / 10 : -1,
        clock.valid ? clock.hour % 10 : -1,
        clock.valid ? clock.minute / 10 : -1,
        clock.valid ? clock.minute % 10 : -1,
    };
    const int digitX[4] = {
        kR6_ClockDigit0X, kR6_ClockDigit1X,
        kR6_ClockDigit2X, kR6_ClockDigit3X,
    };
    const uint16_t tint =
        lerpColor565_8(RP::CRT, exteriorTint, 52u);

    canvas.setClipRect(kR6_GlassX + kRoomPX,
                       kR6_GlassY + kRoomPX,
                       kR6_GlassW - kRoomPX * 2,
                       kR6_GlassH - kRoomPX * 2);
    for (int i = 0; i < 4; ++i) {
        drawComfortClockDigit(canvas, digitX[i], digits[i], now,
                              0xC10Cu + (uint32_t)i * 0x113u, tint);
    }

    // The real seconds value drives a restrained breathing colon. It confirms
    // the displayed HH:MM is a clock, not decorative signage.
    const uint8_t colonAlpha = clock.valid && (clock.second & 1u)
        ? 128u : 84u;
    const int colonTopY = kR6_ClockY + 2 * kR6_ClockCell;
    const int colonBottomY = kR6_ClockY + 4 * kR6_ClockCell;
    drawComfortClockGlassCell(canvas, kR6_ClockColonX, colonTopY,
                              kR6_ClockCell, kR6_ClockCell,
                              tint, colonAlpha);
    drawComfortClockGlassCell(canvas, kR6_ClockColonX, colonBottomY,
                              kR6_ClockCell, kR6_ClockCell,
                              tint, colonAlpha);
    drawComfortClockDrip(canvas, kR6_ClockColonX, colonBottomY +
                        kR6_ClockCell, now, 0xC10C24u, tint);
    canvas.clearClipRect();
}

struct ComfortMotionFrame {
    uint32_t beat;
    uint8_t blend;
};

// Bath motion shares one wrap-safe Q8 clock. Geometry remains grid-snapped,
// but outgoing and incoming cells can crossfade during every 200ms beat.
static ComfortMotionFrame comfortMotionFrame(uint32_t now) {
    static constexpr uint32_t kBeatMs = 200u;
    static constexpr uint32_t kCycleBeats = 144u;
    static constexpr uint32_t kCycleMs = kBeatMs * kCycleBeats;
    static constexpr uint32_t kOffsetMs = (5u * 317u) % kCycleMs;
    uint32_t localMs = (now % kCycleMs) + kOffsetMs;
    if (localMs >= kCycleMs) localMs -= kCycleMs;
    uint32_t phaseQ8 = (localMs << 8) / kBeatMs;
    ComfortMotionFrame frame;
    frame.beat = phaseQ8 >> 8;
    frame.blend = (uint8_t)(phaseQ8 & 0xFFu);
    return frame;
}

static uint8_t comfortWeightedAlpha(uint8_t alpha, uint8_t weight) {
    return (uint8_t)(((uint16_t)alpha * weight + 127u) / 255u);
}

static void drawComfortBlendBlock(M5Canvas& canvas, int x, int y,
                                  int w, int h, uint16_t color,
                                  uint8_t alpha, uint8_t weight) {
    uint8_t weighted = comfortWeightedAlpha(alpha, weight);
    if (weighted == 0u) return;
    uint16_t base = fastReadPx(canvas, x, y);
    canvas.fillRect(x, y, w, h,
                    Display::screenBlend565(base, color, weighted));
}

static ExteriorSprites::RenderOptions room5ExteriorOptions() {
    ExteriorSprites::RenderOptions options;
    options.thunder = Weather::isThunderFlashing();
    options.tintActive = colorEvent.active;
    options.tintColor565 = colorEvent.color565;
    options.tintIntensity = colorEvent.intensity;
    options.parallaxX = (int8_t)parallaxFar;
    return options;
}

static ExteriorSprites::Emitter room5ExteriorEmitter(uint32_t now) {
    ExteriorSprites::RenderOptions options = room5ExteriorOptions();
    return ExteriorSprites::dominantEmitter(
        ExteriorSprites::Scene::Comfort, now,
        kR6_GlassX + kRoomPX, kR6_GlassY + kRoomPX,
        kR6_GlassW - kRoomPX * 2, kR6_GlassH - kRoomPX * 2,
        options.parallaxX, options);
}

static void drawComfortDeckReflections(M5Canvas& canvas, uint32_t now) {
    const int deckY = kR6_GlassY + kR6_GlassH;
    ExteriorSprites::Emitter exterior = room5ExteriorEmitter(now);
    // The warm pool is the sconce's bounce, so it rides the sconce's plane.
    // Left on the fixed plate it slides up to 4px out from under its own source.
    drawLightPoolGradient(canvas, RP::WARM,
                          4 + parallaxFar, deckY + 4, 88, 12, 44, 0xB463u);
    if (exterior.active) {
        int reflectionX = max(4, (int)exterior.x - 32);
        int reflectionW = min(SCREEN_WIDTH - 4 - reflectionX, 64);
        drawLightPoolGradient(canvas, exterior.color565,
                              reflectionX, deckY + 4,
                              reflectionW, 16,
                              (uint8_t)min(72, 24 + exterior.strength / 4),
                              0xB467u);
    }
    if (colorEvent.active) {
        drawLightPoolGradient(canvas, colorEvent.color565,
                              4, deckY + 8, SCREEN_WIDTH - 8, 12,
                              (uint8_t)(18u + colorEvent.intensity / 5u),
                              0xB46Du);
    }
}

// ==[ WATER CONTACT ]== shared by the splash particles and the surface ring, so
// both fire off exactly the same instant of the same jump.
static constexpr float kBathEntryContact = 0.62f;
static constexpr float kBathExitRelease = 0.36f;

static bool bathJumpTouchesWater(float t, bool entering) {
    if (t < 0.0f) return false;
    return entering ? t >= kBathEntryContact : t <= kBathExitRelease;
}

// ==[ SURFACE IMPULSE ]== the waterline is a real surface, not a painted strip.
// Everything that enters or leaves it displaces it: one travelling ring, armed
// on rising edges only, that the surface renderer adds to every cell's Y before
// it draws. Splash droplets and foam still ride on top, but they are the
// evidence of the event — the deformation is the event.
// The life is the ring's *visible* life, not a round number. At 170px/s the
// front clears the far tub wall and drags its own band past it in a little
// under 600ms; every millisecond after that renders nothing while the
// rising-edge guard below still refuses to arm the next event.
static constexpr uint32_t kBathRippleLifeMs = 620u;
static constexpr int kBathRippleSpeedPxPerS = 170;
static constexpr int kBathRippleBandPx = 10;
static constexpr int kBathCraterMs = 220;
static constexpr int kBathCraterHalfW = 10;
// Two pig cells of crest, two of crater: the whole displacement envelope. Both
// ends have to stay in the tub, because the water volume below the surface is
// placed off the same number.
static constexpr int kBathRippleMaxDy = 2 * kPigPX;
static_assert(kR6_TubWaterY - kBathRippleMaxDy > kR6_TubY &&
              kR6_TubWaterY + kBathRippleMaxDy + kPigPX <= kR6_TubFrontY,
              "bath ripple must not push the waterline out of the tub");

// The lit water rect drawHotBath4 paints, shared by the surface, the volume
// below it and the impulse ring that deforms both. Authored geometry: the live
// band is these plus the mid-plane offset (see bathInnerX/bathInnerRight).
static constexpr int kBathInnerX = kR6_TubX + 12;
static constexpr int kBathInnerRight = kR6_TubX + kR6_TubW - 12;
static_assert(((kBathInnerRight - kBathInnerX) % kRoomPX) == 0,
              "bath surface must stay on the room grid");

// Every offset on this plane is a whole multiple of kRoomPX, so the water's
// cell lattice translates rigidly: cell indices — and with them the travelling
// wave phase — are identical at every pose instead of jumping a cell per step.
static int bathInnerX() { return kBathInnerX + parallaxMid; }
static int bathInnerRight() { return kBathInnerRight + parallaxMid; }

static int      bathRippleX = 0;
static uint32_t bathRippleStartMs = 0;
static uint8_t  bathRippleStrength = 0;
static uint8_t  bathRippleShakeStep = 0xFFu;
static uint8_t  bathRippleDanceStep = 0xFFu;

// Everything bathRippleDy() needs that is constant across the frame, resolved
// once by updateBathSurfaceImpulse(). Roughly eighty cells ask for a
// displacement each frame — surface, submerged volume, both laps — and without
// this every one of them re-ran the energy and front-position divides for an
// answer that cannot change until `now` does.
//
// The crater and the crest read the energy at different thresholds on purpose:
// a climb-out is strong enough to raise a one-cell crest but not to punch a
// two-cell hole, so they are resolved separately.
struct BathRippleFrame {
    int front = 0;         // how far the ring has travelled, px
    int crest = 0;         // crest depth for this energy, px (0 = no ring)
    int craterDepth = 0;   // cavity depth, px (0 = filled back in)
};
static BathRippleFrame bathRipple;

static void armBathRipple(uint32_t now, int centerX, uint8_t strength) {
    bathRippleX = centerX & ~(kRoomPX - 1);
    bathRippleStartMs = now;
    bathRippleStrength = strength;
}

// Resolves the ring for this frame and retires it once spent. This is the only
// place the ring's own state is written during a frame, so the draw helpers
// below stay pure reads of `bathRipple`.
static void sampleBathRipple(uint32_t now) {
    bathRipple = BathRippleFrame();
    if (bathRippleStrength == 0u) return;
    const uint32_t age = (uint32_t)(now - bathRippleStartMs);
    if (age >= kBathRippleLifeMs) {
        bathRippleStrength = 0u;
        return;
    }
    // Remaining energy, linearly spent over the ring's life.
    const uint8_t energy = (uint8_t)(((uint32_t)bathRippleStrength *
                                      (kBathRippleLifeMs - age)) /
                                     kBathRippleLifeMs);
    if (energy == 0u) return;
    bathRipple.front =
        (int)((age * (uint32_t)kBathRippleSpeedPxPerS) / 1000u);
    bathRipple.crest = ((energy >= 150u) ? 2 : 1) * kPigPX;
    if (age < (uint32_t)kBathCraterMs) {
        bathRipple.craterDepth = ((energy >= 160u) ? 2 : 1) * kPigPX;
    }
}

// Vertical offset for one surface cell, in whole pig cells so the deformed
// waterline stays as crisp as the flat one.
static int bathRippleDy(int cellX) {
    if (bathRipple.crest == 0) return 0;

    int d = cellX + kRoomPX / 2 - bathRippleX;
    if (d < 0) d = -d;

    // The cavity the body punched stays open while the ring pulls away from it,
    // then fills back in. Without this the entry reads as a bump appearing out
    // of flat water instead of water being shoved aside.
    if (bathRipple.craterDepth != 0 && d <= kBathCraterHalfW) {
        return bathRipple.craterDepth;
    }

    const int band = d - bathRipple.front;
    if (band > kBathRippleBandPx / 2 || band < -kBathRippleBandPx) return 0;
    // Trough just ahead of the front, crest just behind it.
    return band >= 0 ? kPigPX : -bathRipple.crest;
}

// Rising-edge arming for every event that moves the water. Runs before the
// surface is drawn so the deformation and the particles agree within a frame.
static void updateBathSurfaceImpulse(uint32_t now) {
    sampleBathRipple(now);
    const float t = getBathJumpProgress(now);
    if (t >= 0.0f) {
        const bool entering = roamState == RoamState::MOUNTING;
        if (bathJumpTouchesWater(t, entering) && bathRippleStrength == 0u) {
            // A cannonball displaces more than a climb-out.
            armBathRipple(now, (int)pigX + kPigW / 2,
                          entering ? 255u : 180u);
        }
        bathRippleShakeStep = 0xFFu;
        bathRippleDanceStep = 0xFFu;
        return;
    }

    const BathIdleFrame bathIdle = sampleBathIdleFrame(now);
    const int headCx = (int)pigX + kPigW / 2;
    if (bathIdle.phase == BathIdlePhase::RECOVER &&
        bathIdle.phaseElapsedMs < 720u) {
        // One ring per authored shake keyframe, thrown to the side the head
        // just whipped towards.
        const uint8_t step = (uint8_t)(bathIdle.phaseElapsedMs / 120u);
        if (step != bathRippleShakeStep) {
            bathRippleShakeStep = step;
            const int lean = (step & 1u) ? -14 : 14;
            armBathRipple(now, headCx + lean, 120u);
        }
        bathRippleDanceStep = 0xFFu;
        return;
    }
    bathRippleShakeStep = 0xFFu;

    // One compact ripple per microphone-locked surface beat. The underwater
    // bob belongs to Pancetta's pose; the automatic dive keeps this water pass.
    // A ring is visible for 620 ms while a valid cadence may be as fast as
    // 280 ms. Re-arm the one existing ring on a fresh downbeat instead of
    // dropping every other hit; this stays one bounded water pass.
    if (isBathSoundDanceSurfaceActive(now)) {
        const uint8_t step = getBathSoundDanceBeatPhase(now);
        if (step == 0u && bathRippleDanceStep != 0u) {
            static constexpr int8_t kDanceRippleOffset[4] = {-20, 20, -12, 12};
            const uint8_t ripple = static_cast<uint8_t>((now / 1600u) & 3u);
            armBathRipple(now, headCx + kDanceRippleOffset[ripple], 92u);
        }
        bathRippleDanceStep = step;
        return;
    }
    bathRippleDanceStep = 0xFFu;

    // Going under and breaking back out are both water events.
    if (bathIdle.phase == BathIdlePhase::SINK &&
        bathIdle.phaseElapsedMs < 120u && bathRippleStrength == 0u) {
        armBathRipple(now, headCx, 150u);
    }
}

// getBathJumpProgress() publishes a ratio, not an event timestamp, so the
// cannonball needs a latch to mint a gust token that holds for the whole splash.
static SmokeFx::EdgeToken bathSplashEdge;

static void drawBathJumpSplash(M5Canvas& canvas, uint32_t now) {
    float t = getBathJumpProgress(now);
    bool entering = roamState == RoamState::MOUNTING;
    // Also false for t < 0, so this one test covers "no jump" and "airborne".
    bool touching = bathJumpTouchesWater(t, entering);

    // 72kg of pig hitting hot water. Verified against the integrator: this
    // throws each volume ~15px sideways before drag eats it, and cuts what is
    // left of its life roughly in half — the column tears open and reforms.
    if (const uint32_t tk = bathSplashEdge.sample(touching, now)) {
        SmokeFx::gust(tk, entering ? 190u : 120u);
    }
    if (!touching) return;

    float waterT = entering
        ? (t - kBathEntryContact) / (1.0f - kBathEntryContact)
        : t / kBathExitRelease;
    if (waterT < 0.0f) waterT = 0.0f;
    if (waterT > 1.0f) waterT = 1.0f;

    // Keep the burst attached to Pancetta's actual water contact. The old
    // fixed-center entry splash fired while he was still crossing the rim.
    int cx = (((int)pigX + kPigW / 2) & ~(kPigPX - 1));
    int spread = ((int)(waterT * 24.0f)) & ~(kPigPX - 1);

    // Six cheap ballistic droplets. Their arcs share the real contact point,
    // but staggered velocity keeps the splash from reading as mirrored bars.
    // The arcs launch from Pancetta's own plane; only the rim they are culled
    // against belongs to the tub, so that is the one term carrying the offset.
    const int tubX = kR6_TubX + parallaxMid;
    int travelDir = faceRight ? 1 : -1;
    int wake = entering ? 0 : -travelDir * kRoomPX;
    for (int drop = 0; drop < 6; ++drop) {
        int side = (drop & 1) ? 1 : -1;
        float speed = 10.0f + (float)(drop / 2) * 5.0f;
        int dx = (int)(speed * waterT) * side + wake;
        if (!entering) dx += travelDir * (int)(waterT * 6.0f);
        int lift = (int)(fastSinf(waterT * 3.14159f) *
                         (9.0f + (float)(drop % 3) * 4.0f));
        int dropX = (cx + dx) & ~(kPigPX - 1);
        int dropY = (kR6_TubWaterY - lift - (drop & 2 ? kPigPX : 0)) &
                    ~(kPigPX - 1);
        if (dropX < tubX + kRoomPX ||
            dropX >= tubX + kR6_TubW - kRoomPX ||
            dropY <= kRoomY + kRoomPX) continue;
        uint16_t color = (drop % 3) == 0 ? RP::SOFT :
                         ((drop & 1) ? RP::SHAFT : RP::PUDDLE);
        canvas.fillRect(dropX, dropY,
                        (drop < 2) ? kRoomPX : kPigPX, kPigPX, color);
    }

    // Broken foam collar and two widening capillary wakes survive the rim.
    int foamCells = 2 + spread / kRoomPX;
    for (int cell = -foamCells; cell <= foamCells; ++cell) {
        if (((cell + (int)(now / 120u)) & 2) != 0) continue;
        int foamX = cx + cell * kPigPX;
        if (foamX < tubX + 12 || foamX >= tubX + kR6_TubW - 12)
            continue;
        canvas.fillRect(foamX, kR6_TubWaterY, kPigPX, kPigPX,
                        (cell & 1) ? RP::SOFT : RP::PUDDLE);
    }
    int wakeY = kR6_TubWaterY + kPigPX;
    canvas.fillRect(cx - kRoomPX - spread + wake, wakeY,
                    8, kPigPX, RP::PUDDLE);
    canvas.fillRect(cx + spread + wake, wakeY,
                    8, kPigPX, RP::SHAFT);
}

// ==[ SUBMERGED VOLUME ]== drawHotBath4 already lays eight rows of water into
// the tub, but Pancetta is painted over them, so everything below his waist
// used to sit in front of the bath instead of in it. The old privacy mosaic
// covered that with a block of flat room colours — a patch over a depth
// problem. Now that the waterline is a real deforming surface the honest fix
// is available: put the water back on top of him. Every cell between the live
// surface and the front panel is pulled toward the water palette, harder with
// depth, so the body sinks into the tub and the tub reads as deep even when
// nobody is in it.
static constexpr uint8_t kBathSinkAlpha0 = 104;
static constexpr uint8_t kBathSinkAlphaPerRow = 40;

// No clock of its own: the surface it hangs off was already resolved for this
// frame by sampleBathRipple().
static void drawBathSubmergedVolume(M5Canvas& canvas) {
    const int innerRight = bathInnerRight();
    for (int px = bathInnerX(); px < innerRight; px += kRoomPX) {
        // One column of water, hanging off wherever this column's surface
        // currently is. A crater that drops the surface onto the front panel
        // leaves no column at all, which is the point: the ring shoved the
        // water off him and the body is briefly, honestly bare.
        int top = kR6_TubWaterY + bathRippleDy(px) + kPigPX;
        int row = 0;
        for (int py = top; py < kR6_TubFrontY; py += kPigPX, ++row) {
            uint16_t alpha = (uint16_t)kBathSinkAlpha0 +
                             (uint16_t)row * (uint16_t)kBathSinkAlphaPerRow;
            if (alpha > 255u) alpha = 255u;
            // The first row under the surface still carries the lit water
            // colour; everything below it is the volume swallowing light.
            uint16_t tint = (row == 0) ? RP::PUDDLE : RP::D_DEEP;
            uint16_t base = fastReadPx(canvas, px, py);
            canvas.fillRect(px, py, kRoomPX, kPigPX,
                            lerpColor565_8(base, tint, (uint8_t)alpha));
        }
    }
}

// The tub is full whether or not anyone is in it, so the surface always runs.
// `occupied` only gates the parts that need a body: the laps that cross the
// silhouette and the soak column rising past it.
static void drawBathSurface(M5Canvas& canvas, uint32_t now,
                            const PigPose& pose, bool occupied) {
    static_assert(kR6_BathPigY < kR6_TubWaterY &&
                  kR6_BathPigY + kPigH > kR6_TubWaterY,
                  "bath pig must cross the live water surface");

    RoomLightLoopFrame bathLoop = sampleRoomLightLoop(5, now);
    ComfortMotionFrame motion = comfortMotionFrame(now);
    BathIdleFrame bathIdle = sampleBathIdleFrame(now);
    uint32_t bathBeat = motion.beat;
    int travel = (int)(bathBeat & 7u);
    int nextTravel = (travel + 1) & 7;
    const int innerX = bathInnerX();
    const int innerRight = bathInnerRight();
    for (int px = innerX; px < innerRight; px += kRoomPX) {
        int cell = (px - innerX) / kRoomPX;
        bool visibleNow = ((cell + travel) & 3) != 3;
        bool visibleNext = ((cell + nextTravel) & 3) != 3;
        // Displacement is added to both the outgoing and incoming cell so the
        // 200ms crossfade carries the ring instead of fighting it.
        int shove = bathRippleDy(px);
        int py = kR6_TubWaterY + shove +
            ((((cell + travel) & 7) == 0) ? -kPigPX : 0);
        int nextPy = kR6_TubWaterY + shove +
            ((((cell + nextTravel) & 7) == 0) ? -kPigPX : 0);
        const bool coolNow = ((cell + bathLoop.phase) & 1) != 0;
        const uint16_t tintNow = coolNow ? RP::SHAFT : RP::PUDDLE;
        const uint16_t tintNext = coolNow ? RP::PUDDLE : RP::SHAFT;
        uint16_t tint = lerpColor565_8(tintNow, tintNext,
                                       bathLoop.phaseBlend);
        uint8_t strength = (uint8_t)(42u + bathLoop.reflection / 3u);
        // A shoved cell is churned water: brighten it so the ring reads as
        // motion and not as the whole strip sliding down the tub.
        if (shove != 0) {
            tint = RP::SOFT;
            strength = (uint8_t)(strength + 40u > 255u ? 255u : strength + 40u);
        }
        if (visibleNow && visibleNext && py == nextPy) {
            drawComfortBlendBlock(canvas, px, py, kRoomPX, kPigPX,
                                  tint, strength, 255u);
        } else {
            if (visibleNow) {
                drawComfortBlendBlock(canvas, px, py, kRoomPX, kPigPX,
                                      tint, strength,
                                      (uint8_t)(255u - motion.blend));
            }
            if (visibleNext) {
                drawComfortBlendBlock(canvas, px, nextPy,
                                      kRoomPX, kPigPX, tint, strength,
                                      motion.blend);
            }
        }
    }

    // Under the surface, over Pancetta. Unconditional for the same reason the
    // surface is: the water does not drain when he steps out of frame.
    drawBathSubmergedVolume(canvas);

    if (!occupied) return;

    // Two brighter laps cross Pancetta's lower silhouette. The previous water
    // lived wholly behind him, so the stationary bath pose read as standing in
    // front of a painted tub instead of sitting in it.
    int pigCx = (pose.drawX + kPigW / 2) & ~(kPigPX - 1);
    uint32_t lapQ8 = ((bathBeat << 8) | motion.blend) / 2u;
    int lapBaseY = kR6_TubWaterY +
        ((((lapQ8 >> 8) & 1u) != 0u) ? kPigPX : 0);
    int nextLapBaseY = kR6_TubWaterY +
        (((((lapQ8 >> 8) + 1u) & 1u) != 0u) ? kPigPX : 0);
    uint8_t lapBlend = (uint8_t)(lapQ8 & 0xFFu);
    // While the dive parks the nostrils on the waterline, the pale lap bar on
    // the facing side lands exactly on the surfaced snout and reads as a
    // cigarette stuck in the nostril. Skip that side for the whole dive.
    const bool noseAtWaterline =
        bathIdle.phase == BathIdlePhase::SINK ||
        bathIdle.phase == BathIdlePhase::SUBMERGED ||
        bathIdle.phase == BathIdlePhase::RISE;
    const int noseSide = faceRight ? 1 : -1;
    for (int side = -1; side <= 1; side += 2) {
        if (noseAtWaterline && side == noseSide) continue;
        int lapX = pigCx + side * 24 - kRoomPX;
        // A lap is a highlight *on* the surface, so it travels with the ring
        // instead of hanging at the flat waterline while the water moves.
        int lapShove = bathRippleDy(lapX & ~(kRoomPX - 1));
        int lapY = lapBaseY + lapShove;
        int nextLapY = nextLapBaseY + lapShove;
        if (lapY == nextLapY) {
            drawComfortBlendBlock(canvas, lapX, lapY,
                                  kRoomPX * 2, kPigPX,
                                  RP::SOFT, 76u, 255u);
        } else {
            drawComfortBlendBlock(canvas, lapX, lapY,
                                  kRoomPX * 2, kPigPX,
                                  RP::SOFT, 76u,
                                  (uint8_t)(255u - lapBlend));
            drawComfortBlendBlock(canvas, lapX, nextLapY,
                                  kRoomPX * 2, kPigPX,
                                  RP::SOFT, 76u, lapBlend);
        }
    }

    // The soak column now runs through the dive as well. It is the only thing
    // still moving while Pancetta is under, and it sits a full pig-width out to
    // either side, so it can never be mistaken for something on his face — the
    // mistake the nostril burbles kept making. It only yields to the recovery
    // shake, whose own drips own that moment.
    const bool shaking = bathIdle.phase == BathIdlePhase::RECOVER &&
                         bathIdle.phaseElapsedMs < 720u;
    if (!shaking) {
        // The visible water column is kR6_TubWaterY..kR6_TubFrontY — the front
        // panel occludes everything below it — so a bubble rises through that
        // band and pops at the surface. The old travel ran the other way: it
        // started one cell *above* the waterline and climbed twelve more, which
        // put it six pixels clear of the tub rim, out over the deck and onto the
        // window's bottom seal.
        constexpr int kSoakBottom = kR6_TubFrontY - kRoomPX;
        constexpr int kSoakSpan = kSoakBottom - kR6_TubWaterY;
        static_assert(kSoakSpan > 0 && (kSoakSpan % kRoomPX) == 0,
                      "soak bubbles must rise inside the visible water band");
        for (int bubble = 0; bubble < 3; ++bubble) {
            uint32_t phase = (now + (uint32_t)bubble * 830u) % 2400u;
            if (phase >= 1500u) continue;
            int lift = ((int)(phase * (uint32_t)(kSoakSpan + kRoomPX) / 1500u)) &
                       ~(kRoomPX - 1);
            if (lift > kSoakSpan) lift = kSoakSpan;
            int sway = ((phase / 300u) & 1u) ? kRoomPX : 0;
            static const int kSoakOffsetX[3] = {-44, 44, -32};
            int bx = (pigCx + kSoakOffsetX[bubble] + sway) & ~(kRoomPX - 1);
            int by = kSoakBottom - lift;
            canvas.fillRect(bx, by, kRoomPX, kRoomPX,
                            (bubble & 1) ? RP::PUDDLE : RP::SOFT);
        }
    }
}

// ==[ NOTHING AT THE MUZZLE UNDERWATER ]== the nostril breath burbles are gone
// for good, and so is the bubble-ring helper that only they used. Recolouring
// them to the water palette was not enough: a stack of 2px cells stepping up
// and away from the snout chains into the waterline cells beside it, and the
// whole run reads as one thin stick poking out of the face — a cigarette, which
// is exactly what it must never look like. The snout carries exactly one prop,
// the fat joint of the surfaced smoking sequence, and it is never lit while
// Pancetta is under. The soak column in drawBathSurface keeps the dive alive
// instead, a full pig-width clear of the head.

static void drawBathRecoveryDrips(M5Canvas& canvas, uint32_t now,
                                  const PigPose& pose) {
    BathIdleFrame bathIdle = sampleBathIdleFrame(now);
    if (bathIdle.phase != BathIdlePhase::RECOVER ||
        bathIdle.phaseElapsedMs >= 720u) {
        return;
    }

    const int p = kPigPX;
    int step = (int)(bathIdle.phaseElapsedMs / 120u);
    int spread = step * p;
    int lift = (step < 3 ? step : 6 - step) * p;
    int headCx = (pose.drawX + kPigW / 2) & ~(p - 1);
    int dripY = (kR6_TubWaterY - 12 - lift) & ~(p - 1);
    canvas.fillRect(headCx - 16 - spread, dripY,
                    p, p, RP::SOFT);
    canvas.fillRect(headCx + 14 + spread, dripY + p,
                    p, p, RP::SHAFT);

    // The shake also kicks a tiny broken wake along the waterline. It is the
    // same motion event, not a separate ambient particle system.
    canvas.fillRect(headCx - 20 - spread, kR6_TubWaterY,
                    p * 2, p, RP::PUDDLE);
    canvas.fillRect(headCx + 16 + spread, kR6_TubWaterY,
                    p * 2, p, RP::SOFT);
}

static void drawTubForeground(M5Canvas& canvas, uint32_t now) {
    const int x = kR6_TubX + parallaxMid;
    const int y = kR6_TubFrontY;
    const int w = kR6_TubW;
    const int h = kR6_TubY + kR6_TubH - y;
    canvas.fillRect(x, y, w, h - 4, RP::D_WALL_NEAR);
    canvas.fillRect(x + 4, y + 4, w - 8, h - 8, RP::D_FILL);
    canvas.fillRect(x + 8, y + 12, w - 16, kRoomPX, RP::D_STRUCT);
    canvas.fillRect(x + 4, y + h - 8, w - 8, kRoomPX, RP::D_DEEP);
    canvas.fillRect(x + 8, y, w - 16, kRoomPX, RP::STRUCT);
    canvas.fillRect(x + 20, y + kRoomPX,
                    36, kRoomPX, RP::D_WARM);
    canvas.fillRect(x + w - 64, y + kRoomPX,
                    40, kRoomPX, RP::WALL_FAR);

    // Foreground wisps break the rim and sell hot water in cold rain.
    drawSteam(canvas, RP::SOFT, now + 430u, x + 28, y + 2);
    drawSteam(canvas, RP::SOFT, now + 1220u, x + w - 28, y);
}

static PigLight room5BathLight() {
    PigLight bath;
    bath.x = (int16_t)(kR6_TubX + parallaxMid + kR6_TubW / 2);
    bath.y = (int16_t)kR6_TubWaterY;
    bath.tint = RP::WARM;
    return bath;
}

static PigLight room5ExteriorLight(uint32_t now) {
    ExteriorSprites::Emitter exterior = room5ExteriorEmitter(now);
    PigLight light;
    light.x = exterior.x;
    light.y = exterior.y;
    light.tint = exterior.active ? exterior.color565 : RP::SHAFT;
    return light;
}

PigLight selectRoom5PigKeyLight(int pigDrawX, int pigDrawY, uint32_t now) {
    PigLight bath = room5BathLight();
    PigLight exterior = room5ExteriorLight(now);
    float px = (float)(pigDrawX + kPigW / 2);
    float py = (float)(pigDrawY + kPigH / 2);
    float bathScore = room2EmitterScoreAt(bath, px, py, 138.0f,
                                           currentStation == Station::IN_BATH ? 1.45f : 1.05f);
    float exteriorScore = room2EmitterScoreAt(exterior, px, py, 194.0f, 0.90f);
    return bathScore >= exteriorScore ? bath : exterior;
}

static void applyRoom5NoirToPig(M5Canvas& canvas, uint32_t) {
    static constexpr PigNoirProfile kProfile = {61, 220, 56, 82, 72};
    applyDirectionalPigNoir(canvas, kProfile);
}

void drawRoom5(M5Canvas& canvas, uint32_t now, RoomRenderPass pass) {
    RoomWindowBackdropParams backdrop = {};
    backdrop.style = RoomWindowBackdropStyle::ComfortBalcony;
    backdrop.thunder = Weather::isThunderFlashing();
    backdrop.tintActive = colorEvent.active;
    backdrop.tintColor565 = colorEvent.color565;
    backdrop.tintIntensity = colorEvent.intensity;
    backdrop.parallaxX = (int8_t)parallaxFar;
    if (pass == RoomRenderPass::BASE) {
        canvas.fillRect(0, kRoomY, SCREEN_WIDTH,
                        kFloorY - kRoomY + 8, RP::DEEP);
        RoomWindowBackdropParams retainedBackdrop = backdrop;
        retainedBackdrop.thunder = false;
        retainedBackdrop.tintActive = false;
        drawRoomWindowBackdropBase(canvas,
                                   kR6_GlassX, kR6_GlassY,
                                   kR6_GlassW, kR6_GlassH,
                                   retainedBackdrop);
        canvas.fillRect(kR6_GlassX, kR6_GlassY,
                        kR6_GlassW, kRoomPX, RP::STRUCT);
        canvas.fillRect(kR6_GlassX,
                        kR6_GlassY + kR6_GlassH - 4,
                        kR6_GlassW, kRoomPX, RP::STRUCT);
        canvas.fillRect(kR6_GlassX, kR6_GlassY,
                        kRoomPX, kR6_GlassH, RP::STRUCT);
        canvas.fillRect(kR6_GlassX + kR6_GlassW - 4,
                        kR6_GlassY, kRoomPX, kR6_GlassH, RP::STRUCT);
        for (int x = kR6_GlassX + 76;
             x < kR6_GlassX + kR6_GlassW - 20; x += 80) {
            canvas.fillRect(x, kR6_GlassY,
                            kRoomPX, kR6_GlassH, RP::D_STRUCT);
        }
        canvas.fillRect(0, kR6_GlassY + kR6_GlassH,
                        SCREEN_WIDTH,
                        kFloorY - (kR6_GlassY + kR6_GlassH),
                        RP::D_DEEP);
        // The under-deck plate is full-bleed and holds still; its banding is
        // deck detail, so it travels on the near plane. Start a whole band pair
        // off-screen either side so a shifted run still reaches both edges, and
        // alternate on the index — (x & 32) only alternates while the run starts
        // on a 32px boundary, which parallax stops being true.
        for (int band = 0; band < (SCREEN_WIDTH / 32) + 4; ++band) {
            canvas.fillRect(parallaxNear - 64 + band * 32,
                            kR6_GlassY + kR6_GlassH + 8,
                            20, kRoomPX,
                            (band & 1) ? RP::D_WARM : RP::D_WALL_NEAR);
        }
        return;
    }

    drawRoomWindowBackdropMotion(canvas, now,
                                 kR6_GlassX, kR6_GlassY,
                                 kR6_GlassW, kR6_GlassH, backdrop);

    ExteriorSprites::Emitter exterior = room5ExteriorEmitter(now);
    PigLight exteriorLight;
    exteriorLight.x = exterior.x;
    exteriorLight.y = exterior.y;
    exteriorLight.tint = exterior.active ? exterior.color565 : RP::SHAFT;

    // Diegetic pane projection: city motion is behind it; emitter fog, rain,
    // condensation, seals, and mullions all remain in front of it.
    drawComfortGlassClock(canvas, now, exteriorLight.tint);

    if (exterior.active) {
        // Exterior haze belongs to the glass depth plane, behind Pancetta and
        // the bath. Clip it inside the seals so it cannot drift over the room.
        canvas.setClipRect(kR6_GlassX + kRoomPX, kR6_GlassY + kRoomPX,
                           kR6_GlassW - 2 * kRoomPX,
                           kR6_GlassH - 2 * kRoomPX);
        drawEmitterFog(canvas, now, exterior.x, exterior.y + kRoomPX,
                       kR6_GlassY + kR6_GlassH - kRoomPX,
                       exterior.color565, exterior.active);
        canvas.clearClipRect();
    }

    // Water and reflected city light sit on the pane; the structural frame is
    // painted afterward so its seals and mullions occlude every glass effect.
    drawWindowGlassRain(canvas, now, kR6_GlassX, kR6_GlassY,
                        kR6_GlassW, kR6_GlassH, exteriorLight.tint,
                        kComfortWindowRainIntensity);
    drawCondensation(canvas, now, kR6_GlassX + 4, kR6_GlassY + 4,
                     kR6_GlassW - 8, kR6_GlassH - 8,
                     kComfortWindowCondensationCount,
                     exteriorLight.tint, kComfortWindowCondensationSalt);
    // Rain wall + balcony mullions. Warm interior light fights cold city neon.
    canvas.fillRect(kR6_GlassX, kR6_GlassY, kR6_GlassW, kRoomPX, RP::STRUCT);
    canvas.fillRect(kR6_GlassX, kR6_GlassY + kR6_GlassH - 4,
                    kR6_GlassW, kRoomPX, RP::STRUCT);
    canvas.fillRect(kR6_GlassX, kR6_GlassY, kRoomPX, kR6_GlassH, RP::STRUCT);
    canvas.fillRect(kR6_GlassX + kR6_GlassW - 4, kR6_GlassY,
                    kRoomPX, kR6_GlassH, RP::STRUCT);
    for (int x = kR6_GlassX + 76; x < kR6_GlassX + kR6_GlassW - 20; x += 80)
        canvas.fillRect(x, kR6_GlassY, kRoomPX, kR6_GlassH, RP::D_STRUCT);
    // Wall sconce: a mounted fixture on the far plane, the same class as room
    // 4's outlet and fuse box. Body, warm lens, drop and bounce move together.
    const int sconceDx = parallaxFar;
    canvas.fillRect(16 + sconceDx, kR6_GlassY + 20, 20, 12, RP::D_STRUCT);
    canvas.fillRect(20 + sconceDx, kR6_GlassY + 24, 12, 8, RP::WARM);
    canvas.fillRect(24 + sconceDx, kR6_GlassY + 32, kRoomPX, 24, RP::D_STRUCT);
    drawLightPoolGradient(canvas, RP::WARM, 8 + sconceDx, kR6_GlassY + 48,
                          64, 16, 72, 0xB451u);

    // Wet balcony deck and rail silhouettes behind the bath. The plate and its
    // rail are already in the retained pass and nothing above touches
    // y >= kR6_GlassY + kR6_GlassH: the clock, emitter fog and the frame are
    // clipped or bounded to the pane, the glass rain rejects any cell at or past
    // the pane's bottom, and the sconce sits at the top of the frame. Repainting
    // 320x68 plus ten rail bars per frame only rewrote the cache with its own
    // bytes; drawBalconyDeck4 lands on the identical substrate either way.
    // Near plane. The deck is full-bleed, so it is drawn from two slat pitches
    // off-screen on each side: at ±8 the run still reaches both edges instead
    // of opening a bare gutter, and 48 is an even number of slats, so the
    // alternation lands exactly where it does today at rest.
    PixelFurn::drawBalconyDeck4(canvas, parallaxNear - 48,
                                kR6_GlassY + kR6_GlassH, SCREEN_WIDTH + 96);
    drawComfortDeckReflections(canvas, now);
    // Runoff belongs over the finished wet deck but under greenery and tub.
    drawRoomRain(canvas, now, kR6_GlassX, kR6_GlassY,
                 kR6_GlassW, kR6_GlassH);
    drawComfortRfGreenery(canvas, now);
    const int tubX = kR6_TubX + parallaxMid;
    PixelFurn::drawHotBath4(canvas, tubX, kR6_TubY, kR6_TubW, kR6_TubH, now);

    const FurnitureWashLight tubLights[] = {
        {room5BathLight(), 148.0f, 0.34f},
        {exteriorLight, 214.0f, 0.16f},
    };
    drawFurnitureWashMulti(canvas, tubX, kR6_TubY - 4,
                           kR6_TubW, kR6_TubH + 4,
                           tubLights, 2);
    if (exterior.active && exterior.strength >= kComfortBeamMinStrength) {
        // A low-strength fallback holo remains a real pane reflection, but it
        // cannot project the same tub-wide volume as a close traffic pass.
        drawVolumetricDustBeam(canvas, now,
                               exterior.x, exterior.y + kRoomPX,
                               28, tubX + kR6_TubW / 2,
                               kFloorY, kR6_TubW,
                               exterior.color565, RP::DUST, 0xB452u);
    }
    if (Weather::isThunderFlashing()) {
        PigLight storm;
        storm.x = (int16_t)(kR6_GlassX + kR6_GlassW / 2);
        storm.y = (int16_t)kR6_GlassY;
        storm.tint = RP::FLUOR;
        drawFurnitureWash(canvas, 0, kRoomY + kRoomPX,
                          SCREEN_WIDTH, kFloorY - kRoomY,
                          storm, 320.0f, 0.18f);
        drawLightPoolGradient(canvas, RP::FLUOR,
                              4, kFloorY - 12, SCREEN_WIDTH - 8, 12,
                              34, 0xB47Du);
    }
}

// ==[ BATH STEAM ]== the tub runs hot and never showed it.
//
// Two columns, not one: the tub is 184px wide and a single plume off a bath
// that size reads as a kettle. They sit either side of the bathing pig (who
// occupies x=124..196) so the steam frames him instead of erasing his face.
//
// Sized rise=12/1500ms ≈ 77px from the waterline, so the columns top out around
// y=75 — clear of the pig's head, dissolving in open air well under the ceiling.
// The exterior traffic emitter is fed in as a key light, which is the whole
// reason to route this through the shared volume system rather than stamp
// another local wisp: when a holo pass sweeps the balcony, the steam catches it.
//
// The tub is hot whether or not anyone is in it, so this runs unconditionally.
// That is not just physics pedantry: gating it on `bathing` means there is no
// steam in the air at the instant Pancetta lands, and the cannonball gust in
// drawBathJumpSplash has nothing to tear open.
static void driveBathSteam(M5Canvas& canvas, uint32_t now) {
    SmokeFx::VentParams vent;
    vent.y = (kR6_TubWaterY - kRoomPX) & ~3;
    vent.intervalMs = 520;
    vent.rise = 12;
    vent.lifeMs = 1500;
    vent.spreadPx = 10;
    vent.scale = 66;        // wider than the ramen bowl — a 184px tub of hot water
    vent.opacity = 104;

    // The plumes rise off the water, so both vents ride the tub's plane.
    const int tubX = kR6_TubX + parallaxMid;
    vent.x = (tubX + 28) & ~3;
    vent.seed = 0xA71Bu;
    SmokeFx::driveVent(SmokeFx::Source::Vent0, now, vent);

    vent.x = (tubX + kR6_TubW - 28) & ~3;
    vent.seed = 0xA72Fu;
    SmokeFx::driveVent(SmokeFx::Source::Vent1, now, vent);

    SmokeFx::Lighting lit;
    lit.add(room5ExteriorLight(now), 150.0f, 130);
    if (Weather::isThunderFlashing()) {
        PigLight storm;
        storm.x = (int16_t)(kR6_GlassX + kR6_GlassW / 2);
        storm.y = (int16_t)kR6_GlassY;
        storm.tint = RP::FLUOR;
        lit.add(storm, 260.0f, 150);
    }
    SmokeFx::draw(canvas, SmokeFx::Source::Vent0, RP::SOFT, RP::DUST, &lit);
    SmokeFx::draw(canvas, SmokeFx::Source::Vent1, RP::SOFT, RP::DUST, &lit);
}

void drawRoom5Foreground(M5Canvas& canvas, uint32_t now) {
    applyRoom5NoirToPig(canvas, now);

    // Arm the surface ring before anything reads it, so the deformation, the
    // splash particles and the steam gust all belong to the same frame.
    updateBathSurfaceImpulse(now);

    const bool bathing = isStationVisualActive(Station::IN_BATH);
    const PigPose pose = resolveStationVisualPose(now);
    // Unconditional: a tub that loses its waterline the instant Pancetta leaves
    // the IDLE state is exactly why the entry and exit jumps used to land in
    // nothing. The water is scenery; the body only adds to it.
    drawBathSurface(canvas, now, pose, bathing);
    if (bathing) drawBathRecoveryDrips(canvas, now, pose);
    drawBathJumpSplash(canvas, now);
    drawTubForeground(canvas, now);
    driveBathSteam(canvas, now);
}

} // namespace MenuPig
