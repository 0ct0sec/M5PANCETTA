/**
 * pixel_street.cpp — hunt/idle street zone implementation
 *
 * Cyberpunk street-level zone for idle/hunt mode. Day and night variants
 * driven by TimeOfDay state. All rendering on kRoomPX=4 grid.
 */
#include "pixel_street.h"
#include "pixel_primitives.h"
#include "pixel_materials.h"
#include "pixel_weather.h"
#include "pixel_lighting.h"
#include "../gfx/gfx.h"
#include "../piglet/pig_stars.h"
#include "../piglet/weather.h"
#include <math.h>

namespace PixelStreet {

using namespace PixelPrim;
using namespace PixelMat;
using namespace PixelLight;
using namespace MenuPigRender;
using namespace UIMeasurements::MenuPigLayout;

// ─────────────────────────────────────────────────────────────────────────
// STATIC HELPERS
// ─────────────────────────────────────────────────────────────────────────

// ═══════════════════════════════════════════════════════════════════════════
// SKY
// ═══════════════════════════════════════════════════════════════════════════

struct StreetSkyPalette {
    uint16_t zenith;
    uint16_t horizon;
};

static StreetSkyPalette makeStreetSkyPalette(const TimeOfDay::State& state) {
    const uint16_t nightZenith = Display::lerpColor565(RP::BG, RP::DEEP, 0.42f);
    const uint16_t nightHorizon = Display::lerpColor565(RP::DEEP, RP::SHAFT, 0.24f);
    const uint16_t dayZenith = Display::lerpColor565(RP::WALL_FAR, RP::CRT, 0.30f);
    const uint16_t dayHorizon = Display::lerpColor565(RP::WALL_MID, RP::SHAFT, 0.48f);
    const uint16_t warmHorizon = Display::lerpColor565(RP::SHAFT, RP::WARM, 0.46f);

    StreetSkyPalette sky = {nightZenith, nightHorizon};
    switch (state.phase) {
        case TimeOfDay::DAWN:
            sky.zenith = Display::lerpColor565(nightZenith, dayZenith,
                                               state.phaseProgress);
            sky.horizon = Display::lerpColor565(nightHorizon, warmHorizon,
                                                state.phaseProgress);
            break;
        case TimeOfDay::DAY:
            sky.zenith = dayZenith;
            sky.horizon = dayHorizon;
            break;
        case TimeOfDay::DUSK:
            sky.zenith = Display::lerpColor565(dayZenith, nightZenith,
                                               state.phaseProgress);
            sky.horizon = Display::lerpColor565(warmHorizon, nightHorizon,
                                                state.phaseProgress);
            break;
        case TimeOfDay::NIGHT:
            break;
    }

    // THE OG is the one intentionally polychrome style. Let the astronomical
    // model steer its air colour while the other display styles retain their
    // strict theme-derived contrast and inversion semantics.
    if (Display::isTheOgTheme()) {
        sky.zenith = Display::lerpColor565(sky.zenith, state.skyZenith, 0.24f);
        sky.horizon = Display::lerpColor565(sky.horizon, state.skyHorizon, 0.30f);
    }
    return sky;
}

void drawSkyGradient4(M5Canvas& c, uint16_t zenith, uint16_t horizon, uint32_t now) {
    (void)now;
    // kRoomY is 14, between scenery cells. Fill that two-pixel seam exactly,
    // then start fast 4px writes at y=16; fastFillBlock4 bypasses canvas clips.
    int y0 = (kSkyT + kRoomPX - 1) & ~(kRoomPX - 1);
    int y1 = q4(kSkyB);
    int span = y1 - y0;
    if (span <= 0) {
        return;
    }
    if (y0 > kSkyT)
        c.fillRect(0, kSkyT, SCREEN_WIDTH, y0 - kSkyT, zenith);
    const uint16_t upperSky = Display::lerpColor565(zenith, horizon, 0.26f);
    const uint16_t lowerSky = Display::lerpColor565(zenith, horizon, 0.68f);
    for (int py = y0; py < y1; py += kRoomPX) {
        float t = (float)(py - y0) / (float)span;
        uint16_t rowCol;
        if (t < 0.46f) {
            float local = t / 0.46f;
            local = local * local * (3.0f - 2.0f * local);
            rowCol = Display::lerpColor565(zenith, upperSky, local);
        } else if (t < 0.82f) {
            float local = (t - 0.46f) / 0.36f;
            local = local * local * (3.0f - 2.0f * local);
            rowCol = Display::lerpColor565(upperSky, lowerSky, local);
        } else {
            float local = (t - 0.82f) / 0.18f;
            local = local * local * (3.0f - 2.0f * local);
            rowCol = Display::lerpColor565(lowerSky, horizon, local);
        }
        for (int px = 0; px < SCREEN_WIDTH; px += kRoomPX)
            Gfx::fastFillBlock4(c, px, py, rowCol);
    }
}

static void drawCelestialHalo4(M5Canvas& c, int cx, int cy, int radius,
                               uint16_t tint, uint32_t now, uint32_t seed) {
    const int outer = radius + kRoomPX * 3;
    const int inner2 = radius * radius;
    const int outer2 = outer * outer;
    const uint8_t pulse = (uint8_t)(24 +
        (int)(10.0f * (0.5f + 0.5f * fastSinf((float)(now % 2400u) *
                                               (6.28318f / 2400.0f)))));
    for (int py = q4(cy - outer); py <= q4(cy + outer); py += kRoomPX) {
        for (int px = q4(cx - outer); px <= q4(cx + outer); px += kRoomPX) {
            if (px < 0 || px + kRoomPX > SCREEN_WIDTH ||
                py < kSkyT || py + kRoomPX > kSkyB) continue;
            const int dx = px + kRoomPX / 2 - cx;
            const int dy = py + kRoomPX / 2 - cy;
            const int d2 = dx * dx + dy * dy;
            if (d2 <= inner2 || d2 > outer2) continue;
            uint32_t h = wallHash(px, py, seed);
            uint8_t keep = d2 < (radius + kRoomPX) * (radius + kRoomPX)
                ? 12 : d2 < (radius + kRoomPX * 2) *
                           (radius + kRoomPX * 2) ? 7 : 3;
            if ((h & 0x0Fu) >= keep) continue;
            uint16_t base = Gfx::fastReadPx(c, px, py);
            Gfx::fastFillBlock4(c, px, py,
                                Gfx::screenBlend565(base, tint, pulse));
        }
    }
}

static void drawSunDisc4(M5Canvas& c, int cx, int cy, int radius,
                         const TimeOfDay::State& state, uint32_t now) {
    const int r = q4(radius);
    const int innerR = r - kRoomPX * 2;
    const uint16_t authored = Display::isTheOgTheme() ? state.sunColor : RP::WARM;
    const uint16_t rim = Display::lerpColor565(RP::WARM, authored, 0.42f);
    const uint16_t core = Display::lerpColor565(authored, RP::FLUOR, 0.58f);
    drawCelestialHalo4(c, cx, cy, r, rim, now, 0x50A4u);
    for (int py = q4(cy - r); py <= q4(cy + r); py += kRoomPX) {
        for (int px = q4(cx - r); px <= q4(cx + r); px += kRoomPX) {
            if (px < 0 || px + kRoomPX > SCREEN_WIDTH ||
                py < kSkyT || py + kRoomPX > kSkyB) continue;
            const int dx = px + kRoomPX / 2 - cx;
            const int dy = py + kRoomPX / 2 - cy;
            const int d2 = dx * dx + dy * dy;
            if (d2 > r * r) continue;
            Gfx::fastFillBlock4(c, px, py,
                d2 <= innerR * innerR ? core : rim);
        }
    }
}

static void drawMoonDisc4(M5Canvas& c, const TimeOfDay::State& state,
                          uint32_t now) {
    float hour = state.dayProgress * 24.0f;
    float nightT;
    if (hour >= 21.0f) nightT = (hour - 21.0f) / 8.0f;
    else if (hour < 5.0f) nightT = (hour + 3.0f) / 8.0f;
    else if (state.phase == TimeOfDay::DUSK) nightT = 0.0f;
    else nightT = 1.0f;
    if (nightT < 0.0f) nightT = 0.0f;
    if (nightT > 1.0f) nightT = 1.0f;
    const int cx = q4(280 - (int)(nightT * 240.0f));
    const float arc = 1.0f - (2.0f * nightT - 1.0f) *
                              (2.0f * nightT - 1.0f);
    const int cy = q4(52 - (int)(arc * 20.0f));
    const int radius = 12;
    const uint16_t authored = Display::isTheOgTheme()
        ? state.moonColor : RP::SHAFT;
    const uint16_t moon = Display::lerpColor565(authored, RP::FLUOR, 0.46f);
    const uint16_t crater = Display::lerpColor565(RP::SHAFT, RP::SHADOW_C, 0.55f);
    const int phase = (int)(state.moonPhase * 8.0f) & 7;
    drawCelestialHalo4(c, cx, cy, radius, authored, now, 0xA100u);

    for (int py = cy - radius; py <= cy + radius; py += kRoomPX) {
        for (int px = cx - radius; px <= cx + radius; px += kRoomPX) {
            if (px < 0 || px + kRoomPX > SCREEN_WIDTH ||
                py < kSkyT || py + kRoomPX > kSkyB) continue;
            const int dx = px + kRoomPX / 2 - cx;
            const int dy = py + kRoomPX / 2 - cy;
            if (dx * dx + dy * dy > radius * radius) continue;
            bool lit = false;
            if (phase == 4) {
                lit = true;
            } else if (phase < 4) {
                lit = dx >= (2 - phase) * 8;
            } else {
                lit = dx <= (6 - phase) * 8;
            }
            if (lit) Gfx::fastFillBlock4(c, px, py, moon);
        }
    }
    if (phase == 4)
        Gfx::fastFillBlock4(c, cx - kRoomPX, cy, crater);
}

static void drawCelestial4(M5Canvas& c, const TimeOfDay::State& state,
                           uint32_t now) {
    int16_t sunX = 0, sunY = 0, sunRadius = 0;
    if (Weather::getSunDisc(sunX, sunY, sunRadius)) {
        drawSunDisc4(c, sunX, sunY, sunRadius, state, now);
    } else if (state.phase == TimeOfDay::NIGHT ||
               (state.phase == TimeOfDay::DAWN && state.phaseProgress < 0.30f) ||
               (state.phase == TimeOfDay::DUSK && state.phaseProgress > 0.70f)) {
        drawMoonDisc4(c, state, now);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// DISTANT TOWERS — 3-layer parallax silhouette
// ═══════════════════════════════════════════════════════════════════════════

struct TowerSlot {
    int16_t x, w, h;
    uint8_t layer;    // 0=far, 1=mid, 2=near
    uint8_t variant;  // 0=flat, 1=stepped, 2=spire, 3=shouldered
    uint8_t lightSeed;
};

static constexpr TowerSlot kTowers[] = {
    {  0, 40, 36, 0, 0,  3}, { 44, 32, 52, 0, 2,  7},
    { 80, 52, 40, 0, 1, 11}, {136, 36, 60, 0, 3, 13},
    {176, 48, 44, 0, 0, 17}, {228, 36, 56, 0, 2, 19},
    {268, 52, 40, 0, 1, 23},
    {  0, 32, 52, 1, 1, 29}, { 36, 40, 36, 1, 0, 31},
    { 80, 36, 64, 1, 3, 37}, {120, 48, 44, 1, 1, 41},
    {172, 32, 72, 1, 2, 43}, {208, 56, 48, 1, 0, 47},
    {268, 28, 60, 1, 3, 53}, {300, 20, 40, 1, 0, 59},
    {  0, 28, 32, 2, 0, 61}, { 96, 40, 36, 2, 1, 67},
    {184, 32, 32, 2, 3, 71}, {284, 36, 44, 2, 0, 73},
};

static constexpr bool towerFits(const TowerSlot& s) {
    return s.x >= 0 && s.x + s.w <= SCREEN_WIDTH &&
           s.w > 0 && s.h > 0 && s.layer <= 2 &&
           (s.x % kRoomPX) == 0 && (s.w % kRoomPX) == 0 &&
           (s.h % kRoomPX) == 0 &&
           kSkyB - s.h - kRoomPX * 2 >= ((kSkyT + 3) & ~3);
}
static_assert(towerFits(kTowers[0]) && towerFits(kTowers[1]) &&
              towerFits(kTowers[2]) && towerFits(kTowers[3]) &&
              towerFits(kTowers[4]) && towerFits(kTowers[5]) &&
              towerFits(kTowers[6]) && towerFits(kTowers[7]) &&
              towerFits(kTowers[8]) && towerFits(kTowers[9]) &&
              towerFits(kTowers[10]) && towerFits(kTowers[11]) &&
              towerFits(kTowers[12]) && towerFits(kTowers[13]) &&
              towerFits(kTowers[14]) && towerFits(kTowers[15]) &&
              towerFits(kTowers[16]) && towerFits(kTowers[17]) &&
              towerFits(kTowers[18]),
              "street skyline must fit the open-air 4px geometry contract");

static void drawTowerBody4(M5Canvas& c, int tx, int ty, int tw, int th,
                           uint8_t variant, uint16_t color) {
    int topInset = (variant == 1 || variant == 3) ? kRoomPX : 0;
    if (variant == 2) {
        fillRect4(c, tx + tw / 2 - kRoomPX / 2, ty - kRoomPX * 2,
                  kRoomPX, kRoomPX * 2, color);
        topInset = kRoomPX * 2;
    }
    if (topInset > 0)
        fillRect4(c, tx + topInset, ty, tw - topInset * 2,
                  kRoomPX * 2, color);
    fillRect4(c, tx, ty + (topInset > 0 ? kRoomPX * 2 : 0),
              tw, th - (topInset > 0 ? kRoomPX * 2 : 0), color);
}

static void drawTowerDetails4(M5Canvas& c, int tx, int ty, int tw, int th,
                              uint8_t layer, uint8_t variant, uint8_t seed,
                              uint16_t body, uint16_t detail, bool isNight,
                              uint32_t now) {
    const int topInset = (variant == 1 || variant == 3) ? kRoomPX :
                         variant == 2 ? kRoomPX * 2 : 0;
    const int roofX = tx + topInset;
    const int roofW = tw - topInset * 2;

    // Roof furniture breaks the repeated box silhouette without adding a
    // second asset system. Far towers get fewer marks, so depth stays legible.
    if (variant != 2 && roofW >= kRoomPX * 3 &&
        (layer > 0 || (seed & 1u) != 0u)) {
        const int antennaX = roofX + ((seed & 2u) ? kRoomPX : roofW - kRoomPX * 2);
        fillRect4(c, antennaX, ty - kRoomPX * 2, kRoomPX, kRoomPX * 2, detail);
        if ((seed & 4u) != 0u)
            fillRect4(c, antennaX - kRoomPX, ty - kRoomPX * 2,
                      kRoomPX * 3, kRoomPX, detail);
    }
    if (variant == 2) {
        const int beaconY = ty - kRoomPX * 2;
        const bool beaconOn = isNight &&
            (((now / 900u) + (uint32_t)seed) & 1u) == 0u;
        if (beaconOn)
            fillBlock4(c, tx + tw / 2 - kRoomPX / 2, beaconY, RP::LED);
    }

    // One roof lip and one shaded service spine give each mass a material
    // direction. Keep the far plane quieter than the inhabited layers.
    if (roofW >= kRoomPX * 2)
        fillRect4(c, roofX, ty, roofW, kRoomPX, detail);
    if (layer > 0 && th >= kRoomPX * 4)
        fillRect4(c, tx + tw - kRoomPX, ty + kRoomPX * 2,
                  kRoomPX, th - kRoomPX * 2, detail);

    const uint16_t darkWindow = Display::lerpColor565(body, RP::BG, 0.34f);
    const uint16_t sources[3] = {RP::WARM, RP::CRT, RP::VEND};
    if (layer == 0) return;
    for (int wy = ty + kRoomPX * 2; wy < kSkyB - kRoomPX;
         wy += kRoomPX * 2) {
        for (int wx = tx + kRoomPX; wx < tx + tw - kRoomPX;
             wx += kRoomPX * 2) {
            uint32_t wh = wallHash(wx, wy, 0xBEEFu + seed);
            if (isNight) {
                if ((wh & 0xFFu) >= 34u) continue;
                uint16_t source = sources[(wh >> 8) % 3u];
                uint16_t wc = Display::lerpColor565(body, source, 0.34f);
                if ((wh & 0xFFu) < 5u && ((now / 2200u) & 1u)) wc = body;
                fillBlock4(c, wx, wy, wc);
            } else if ((wh & 0xFFu) < 78u) {
                fillBlock4(c, wx, wy, darkWindow);
            }
        }
    }
}

void drawDistantTowers4(M5Canvas& c, bool isNight, int parallaxX, uint32_t now) {
    uint16_t towerCols[3];
    uint16_t detailCols[3];
    if (isNight) {
        towerCols[0] = Display::lerpColor565(RP::DEEP, RP::SHAFT, 0.15f);
        towerCols[1] = Display::lerpColor565(RP::DEEP, RP::WALL_FAR, 0.58f);
        towerCols[2] = Display::lerpColor565(RP::BG, RP::DEEP, 0.72f);
        detailCols[0] = Display::lerpColor565(towerCols[0], RP::SHAFT, 0.12f);
        detailCols[1] = Display::lerpColor565(towerCols[1], RP::SHAFT, 0.18f);
        detailCols[2] = Display::lerpColor565(towerCols[2], RP::WALL_MID, 0.26f);
    } else {
        towerCols[0] = Display::lerpColor565(RP::WALL_MID, RP::SHAFT, 0.18f);
        towerCols[1] = RP::WALL_FAR;
        towerCols[2] = RP::SHADOW_C;
        detailCols[0] = Display::lerpColor565(towerCols[0], RP::WALL_NEAR, 0.18f);
        detailCols[1] = Display::lerpColor565(towerCols[1], RP::WALL_MID, 0.30f);
        detailCols[2] = Display::lerpColor565(towerCols[2], RP::WALL_NEAR, 0.32f);
    }

    for (int layer = 0; layer < 3; layer++) {
        float parallaxScale = (layer == 0) ? 0.25f : (layer == 1) ? 0.55f : 1.0f;
        int layerParallax = q4((int)((float)parallaxX * parallaxScale));

        for (const TowerSlot& s : kTowers) {
            if (s.layer != layer) continue;
            int tx = s.x + layerParallax;
            int ty = kSkyB - s.h;
            if (tx + s.w < 0 || tx > SCREEN_WIDTH) continue;

            uint16_t col = towerCols[layer];
            drawTowerBody4(c, tx, ty, s.w, s.h, s.variant, col);
            drawTowerDetails4(c, tx, ty, s.w, s.h, (uint8_t)layer,
                              s.variant, s.lightSeed, col, detailCols[layer],
                              isNight, now);
        }
    }

    // Haze band at horizon
    uint16_t hazeCol = isNight ? RP::SHAFT : RP::WALL_NEAR;
    for (int py = kSkyB - kRoomPX * 3; py < kSkyB; py += kRoomPX) {
        for (int px = 0; px < SCREEN_WIDTH; px += kRoomPX) {
            uint8_t alpha = (uint8_t)(10 + (py - (kSkyB - kRoomPX * 3)) * 2);
            if (Gfx::bayer4[(py / kRoomPX) & 3][(px / kRoomPX) & 3] < 9) {
                uint16_t base = Gfx::fastReadPx(c, px, py);
                Gfx::fastFillBlock4(c, px, py,
                                    Gfx::screenBlend565(base, hazeCol, alpha));
            }
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// FLYING TRAFFIC
// ═══════════════════════════════════════════════════════════════════════════

void drawFlyingTraffic4(M5Canvas& c, bool isNight, uint32_t now) {
    if (!isNight) return;
    for (int i = 0; i < 3; i++) {
        uint32_t seed = wallHash(i, 0, 0xF1A7);
        int baseY = kSkyT + 20 +
            (int)((seed >> 8) % (uint32_t)(kSkyB - kSkyT - 48));
        baseY = q4(baseY);
        uint32_t cycle = 6000u + (seed & 0x1FFF);
        uint32_t local = (now + (seed & 0xFFF)) % cycle;
        float t = (float)local / (float)cycle;
        const int direction = (seed & 1u) != 0u ? 1 : -1;
        int tx = direction > 0
            ? (int)(t * (SCREEN_WIDTH + 64)) - 32
            : SCREEN_WIDTH + 32 - (int)(t * (SCREEN_WIDTH + 64));
        tx = q4(tx); // tx is the lit nose, not the hull origin
        if (tx < -12 || tx > SCREEN_WIDTH + 8) continue;
        uint16_t headlightCol = (i & 1) ? RP::NEON : RP::WARM;
        const int hullX = direction > 0 ? tx - kRoomPX * 2 : tx + kRoomPX;
        if (hullX >= 0 && hullX + kRoomPX * 2 <= SCREEN_WIDTH &&
            tx >= 0 && tx < SCREEN_WIDTH) {
            uint16_t hull = Display::lerpColor565(RP::WALL_FAR,
                                                  headlightCol, 0.26f);
            fillRect4(c, hullX, baseY, kRoomPX * 2, kRoomPX, hull);
            fillBlock4(c, tx, baseY, headlightCol);
        }
        // The trail blends from the painted air beneath it, so dawn/dusk do
        // not acquire black barcode streaks.
        for (int s = 2; s < 5; s++) {
            int sx = tx - direction * s * kRoomPX;
            if (sx >= 0 && sx < SCREEN_WIDTH) {
                float trailMix = (float)(105 - s * 18) / 255.0f;
                uint16_t base = Gfx::fastReadPx(c, sx, baseY);
                Gfx::fastFillBlock4(c, sx, baseY,
                    Display::lerpColor565(base, headlightCol, trailMix));
            }
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// PAVEMENT
// ═══════════════════════════════════════════════════════════════════════════

void drawPavement4(M5Canvas& c, bool isNight, uint32_t now) {
    (void)now;
    uint16_t paveCol = isNight ? RP::DEEP : RP::WALL_MID;
    uint16_t lineCol = isNight ? RP::SHADOW_C : RP::WALL_NEAR;
    uint16_t grimeCol = isNight ? RP::BG : RP::FLOOR_GRIME;

    // Base pavement fill
    for (int py = kFgT; py < kRoamB; py += kRoomPX) {
        for (int px = 0; px < SCREEN_WIDTH; px += kRoomPX) {
            Gfx::fastFillBlock4(c, px, py, paveCol);
        }
    }

    // Grid lines (every 16px)
    for (int gy = kFgT; gy < kRoamB; gy += 16) {
        for (int px = 0; px < SCREEN_WIDTH; px += kRoomPX) {
            if ((wallHash(px, gy, 0x1111) & 0xFF) < 200)
                Gfx::fastFillBlock4(c, px, gy, lineCol);
        }
    }
    for (int gx = 0; gx < SCREEN_WIDTH; gx += 16) {
        for (int py = kFgT; py < kRoamB; py += kRoomPX) {
            if ((wallHash(gx, py, 0x2222) & 0xFF) < 200)
                Gfx::fastFillBlock4(c, gx, py, lineCol);
        }
    }

    // Two-cell curb: a lit lip over a recessed joint. This is the depth hinge
    // between the skyline and the walkable plate, not another flat horizon.
    fillRect4(c, 0, kFgT, SCREEN_WIDTH, kRoomPX,
              Display::lerpColor565(paveCol, RP::WALL_NEAR, 0.34f));
    for (int px = 0; px < SCREEN_WIDTH; px += kRoomPX) {
        if ((wallHash(px, kFgT + kRoomPX, 0xC08Bu) & 0x07u) != 0u)
            fillBlock4(c, px, kFgT + kRoomPX, grimeCol);
    }

    // Grime patches
    for (int i = 0; i < 8; i++) {
        uint32_t seed = wallHash(i, 0, 0x3333);
        int gx = (int)((seed >> 0) % (uint32_t)(SCREEN_WIDTH - 8));
        int gy = q4(kFgT + (int)((seed >> 8) % (uint32_t)(kRoamB - kFgT - 8)));
        gx = q4(gx);
        fillRect4(c, gx, gy, kRoomPX * 2, kRoomPX, grimeCol);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// GRASS VERGE
// ═══════════════════════════════════════════════════════════════════════════

void drawGrassVerge4(M5Canvas& c, bool isNight, uint32_t now) {
    (void)now;
    uint16_t grassCol = isNight
        ? Display::lerpColor565(RP::BG, RP::GREEN_DK, 0.30f)
        : Display::lerpColor565(RP::WALL_MID, RP::GREEN_DK, 0.35f);
    uint16_t bladeCol = isNight ? RP::DEEP : RP::FLOOR_GRID;

    for (int py = kRoamT; py < kRoamB; py += kRoomPX) {
        for (int px = 0; px < SCREEN_WIDTH; px += kRoomPX) {
            uint32_t seed = wallHash(px, py, 0x7777);
            Gfx::fastFillBlock4(c, px, py, grassCol);
            // Grass blades (sparse brighter cells)
            if ((seed & 0xFF) < 40) {
                Gfx::fastFillBlock4(c, px, py, bladeCol);
            }
        }
    }
    // Irregular verge edge ties the avatar-owned foreground grass to the
    // street plate without adding sub-grid blades.
    for (int px = 0; px < SCREEN_WIDTH; px += kRoomPX) {
        uint32_t seed = wallHash(px, kRoamT, 0x7E26u);
        if ((seed & 0x03u) == 0u)
            fillBlock4(c, px, kRoamT - kRoomPX, bladeCol);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// SOLAR LAMP
// ═══════════════════════════════════════════════════════════════════════════

void drawSolarLamp4(M5Canvas& c, int x, int y, bool isNight, uint32_t now) {
    x = q4(x); y = q4(y);

    // Post
    fillRect4(c, x + 4, y, kRoomPX, kFloorY - y, RP::WALL_MID);

    // Lamp head
    fillRect4(c, x, y, kRoomPX * 3, kRoomPX, RP::WALL_NEAR);

    if (isNight) {
        // LED pulse
        uint32_t pulse = 1200u;
        uint32_t phase = (now + (uint32_t)wallHash(x, y, 0x5555)) % pulse;
        float brightness = 0.6f + 0.4f * fastSinf((float)phase / (float)pulse * 6.28f);
        uint16_t glowCol = RP::LED;
        Gfx::fastFillBlock4(c, x + 4, y, glowCol);

        // Light pool on ground
        int poolW = 40;
        int poolH = kRoomPX * 2;
        int poolX = x + 4 - poolW / 2;
        int poolY = kFloorY - kRoomPX * 3;
        uint8_t density = (uint8_t)(brightness * 50.0f);
        drawLightPool4(c, glowCol, poolX, poolY, poolW, poolH, density, wallHash(x, y, 0x5556));
    } else {
        Gfx::fastFillBlock4(c, x + 4, y, RP::WALL_NEAR);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// NEON SIGN BUILDING
// ═══════════════════════════════════════════════════════════════════════════

static bool isStreetSignLit(int x, int y, bool isNight, uint32_t now) {
    const uint32_t flickerPhase =
        (now + (wallHash(x, y, 0x51A7u) % 1600u)) % 3200u;
    return isNight && !(flickerPhase >= 2940u && flickerPhase < 3020u);
}

void drawNeonSignBuilding4(M5Canvas& c, int x, int y, const char* text,
                           bool isNight, uint32_t now) {
    x = q4(x); y = q4(y);
    if (!text || !text[0]) return;

    // The facade owns the wall. The sign is a mounted source, not a second
    // rectangular building pasted over the skyline.
    int signW = kRoomPX * 10;
    int signH = kRoomPX * 3;
    bool signLit = isStreetSignLit(x, y, isNight, now);
    uint16_t frameCol = signLit ? RP::NEON : RP::D_STRUCT;
    fillRect4(c, x, y, signW, signH, frameCol);
    fillRect4(c, x + 4, y + 4, signW - 8, signH - 8, RP::D_DEEP);

    if (signLit) {
        // Five chunky signal cells carry the CYBER rhythm without stretching
        // the utility font into fake pixel art.
        int len = 0;
        while (len < 5 && text[len]) len++;
        for (int ch = 0; ch < len; ch++) {
            int cx = x + kRoomPX + ch * (kRoomPX * 2);
            if (cx + kRoomPX > x + signW - 2) break;
            fillBlock4(c, cx, y + kRoomPX, RP::NEON);
        }

        // Cast light pool on pavement below
        drawLightPool4(c, RP::NEON, x - 8, kFgT, signW + 16, 32, 35,
                       wallHash(x, y, 0xABCD));
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// PUDDLE REFLECTION
// ═══════════════════════════════════════════════════════════════════════════

void drawPuddleReflection4(M5Canvas& c, int x, int y, int w, int h,
                           bool isNight, uint32_t now,
                           uint16_t sourceTint, bool sourceLit) {
    x = q4(x); y = q4(y); w = (w + 3) & ~3; h = (h + 3) & ~3;
    if (w < kRoomPX || h < kRoomPX) return;

    uint16_t puddleCol = isNight
        ? Display::lerpColor565(RP::BG, RP::PUDDLE, 0.30f)
        : Display::lerpColor565(RP::WALL_MID, RP::SHAFT, 0.25f);

    // Stable tapered body. Only the small material grain is hashed; the pool
    // cannot disappear because both rows happened to lose the edge lottery.
    const int rows = h / kRoomPX;
    for (int py = y; py < y + h; py += kRoomPX) {
        const int row = (py - y) / kRoomPX;
        const int inset = (rows > 1 && (row == 0 || row == rows - 1))
            ? kRoomPX : 0;
        for (int px = x + inset; px < x + w - inset; px += kRoomPX) {
            uint32_t seed = wallHash(px, py, 0x9999);
            const bool core = abs(px + kRoomPX / 2 - (x + w / 2)) <= kRoomPX;
            if (core || (seed & 0xFFu) < 208u)
                Gfx::fastFillBlock4(c, px, py, puddleCol);
        }
    }

    // A puddle reflects one visible practical and stays dimmer than its source.
    if (isNight && sourceLit && sourceTint != 0) {
        uint16_t reflCol = Display::lerpColor565(puddleCol, sourceTint, 0.45f);
        const int shimmer = ((now / 360u) & 1u) ? kRoomPX : 0;
        for (int py = y; py < y + h; py += kRoomPX) {
            int rx = q4(x + w / 2 - kRoomPX + shimmer -
                         ((py - y) / kRoomPX) * kRoomPX);
            if (rx < x || rx + kRoomPX > x + w) continue;
            uint16_t base = Gfx::fastReadPx(c, rx, py);
            Gfx::fastFillBlock4(c, rx, py,
                                Gfx::screenBlend565(base, reflCol, 72));
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// STREETLIGHT POLE
// ═══════════════════════════════════════════════════════════════════════════

void drawStreetlightPole4(M5Canvas& c, int x, bool isNight, uint32_t now) {
    x = q4(x);
    int poleTop = kMidT + kRoomPX * 2;
    int poleBot = kRoamT;

    // Pole
    fillRect4(c, x, poleTop, kRoomPX, poleBot - poleTop, RP::WALL_MID);

    // Lamp fixture
    fillRect4(c, x - 4, poleTop, kRoomPX + 8, kRoomPX, RP::WALL_NEAR);

    if (isNight) {
        Gfx::fastFillBlock4(c, x, poleTop, RP::WARM);
        // Cone of light on pavement
        int coneTop = poleTop + kRoomPX;
        int coneBot = kRoamT + 16;
        int coneTopW = kRoomPX * 2;
        int coneBotW = kRoomPX * 8;
        for (int py = coneTop; py < coneBot; py += kRoomPX) {
            float t = (float)(py - coneTop) / (float)(coneBot - coneTop);
            int cw = (int)(coneTopW + (coneBotW - coneTopW) * t);
            int cx = x + kRoomPX / 2 - cw / 2;
            cx = q4(cx);
            uint8_t alpha = (uint8_t)(40.0f * (1.0f - t * 0.6f));
            for (int px = cx; px < cx + cw; px += kRoomPX) {
                if (px < 0 || px >= SCREEN_WIDTH) continue;
                uint16_t base = Gfx::fastReadPx(c, px, py);
                Gfx::fastFillBlock4(c, px, py,
                    Gfx::screenBlend565(base, RP::WARM, alpha));
            }
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// GUTTER
// ═══════════════════════════════════════════════════════════════════════════

void drawStreetGutter4(M5Canvas& c) {
    for (int py = kGutterT; py < kGutterB; py += kRoomPX) {
        for (int px = 0; px < SCREEN_WIDTH; px += kRoomPX) {
            Gfx::fastFillBlock4(c, px, py, RP::FLOOR_GRIME);
            if ((wallHash(px, py, 0x4444) & 0xFF) < 60)
                Gfx::fastFillBlock4(c, px, py, RP::DEEP);
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// FULL STREET SCENE COMPOSITION
// ═══════════════════════════════════════════════════════════════════════════

static void drawMidgroundFacade4(M5Canvas& c, bool isNight) {
    const uint16_t wall = isNight ? RP::D_DEEP : RP::WALL_FAR;
    const uint16_t seam = isNight ? RP::D_STRUCT : RP::WALL_MID;
    const uint16_t recess = isNight ? RP::BG : RP::SHADOW_C;

    // A low transit deck roots the skyline without becoming another opaque
    // horizontal band.
    fillRect4(c, 0, kMidB - kRoomPX, SCREEN_WIDTH, kRoomPX, seam);

    // Near architecture stays at the edges, framing the open city and keeping
    // the pig path / RF waves readable through the center.
    fillRect4(c, 0, 84, 56, kMidB - 84, wall);
    fillRect4(c, 8, 76, 32, 8, wall);
    fillRect4(c, 0, 84, 56, kRoomPX, seam);
    fillRect4(c, 12, 96, 28, 8, recess);
    fillRect4(c, 16, 96, 4, 8, seam);
    fillRect4(c, 32, 96, 4, 8, seam);

    fillRect4(c, 232, 68, 88, kMidB - 68, wall);
    fillRect4(c, 240, 64, 80, kRoomPX, seam);
    fillRect4(c, 232, 68, kRoomPX, kMidB - 68, seam);
    fillRect4(c, 244, 100, 64, 8, recess);
    for (int x = 248; x < 308; x += 12)
        fillRect4(c, x, 100, kRoomPX, 8, seam);
}

static void drawStreetBaseLayers(M5Canvas& c, bool isNight,
                                 uint16_t zenith, uint16_t horizon,
                                 int pigX) {
    // Pancetta is an actor, not the camera. Until one shared IMU camera offset
    // drives every depth plane, keep the skyline locked to the street.
    (void)pigX;
    // Retained L0: the expensive air plate only. Celestial glow is live so its
    // pulse and cloud occlusion never freeze on a five-minute cache hit.
    drawSkyGradient4(c, zenith, horizon, 0);

    // A stable skyline is retained for cache-only/fallback frames. The live
    // pass redraws it after sky motion so stars and clouds remain behind it.
    drawDistantTowers4(c, isNight, 0, 0);

    // Retained L1: structural depth.
    drawMidgroundFacade4(c, isNight);

    // Retained L3: ground plates and wet material bodies. Dynamic reflections
    // and light pools are added only by the live pass.
    drawPavement4(c, isNight, 0);
    drawGrassVerge4(c, isNight, 0);
    drawPuddleReflection4(c, 60, kFgT + 16, 32, 8,
                          isNight, 0, RP::WARM, false);
    drawPuddleReflection4(c, 200, kFgT + 32, 24, 8,
                          isNight, 0, RP::NEON, false);

    // Retained furniture shells. Passing day/off states deliberately excludes
    // neon, solar pulses, and their light pools from the substrate.
    drawStreetlightPole4(c, 48, false, 0);
    drawStreetlightPole4(c, 176, false, 0);
    drawNeonSignBuilding4(c, 264, 76, "CYBER", false, 0);
    drawSolarLamp4(c, 80, kRoamT + 8, false, 0);
    drawSolarLamp4(c, 240, kRoamT + 8, false, 0);
    drawStreetGutter4(c);
}

static void drawStreetMotionLayers(M5Canvas& c, bool isNight,
                                   const TimeOfDay::State* timeOfDay,
                                   uint32_t now, int pigX) {
    (void)pigX;

    // Live L0: stars/graffiti, celestial bodies, clouds, and birds stay inside
    // open air. Updating PigStars here makes both IDLE and HUNT ownership
    // advance once per rendered frame instead of only on a cache miss.
    PigStars::update();
    c.setClipRect(0, kSkyT, SCREEN_WIDTH, kSkyB - kSkyT);
    PigStars::draw(c, Display::getColorFG());
    if (timeOfDay) drawCelestial4(c, *timeOfDay, now);
    c.clearClipRect();

    uint16_t birdColor = isNight ? RP::WALL_NEAR : RP::STRUCT;
    uint16_t cloudCap = isNight
        ? Display::lerpColor565(RP::DEEP, RP::SHAFT, 0.28f)
        : Display::lerpColor565(RP::DUST, RP::SHAFT, 0.30f);
    uint16_t cloudShade = isNight
        ? Display::lerpColor565(RP::BG, RP::DEEP, 0.58f)
        : Display::lerpColor565(RP::WALL_MID, RP::SHAFT, 0.16f);
    if (timeOfDay && (timeOfDay->phase == TimeOfDay::DAWN ||
                      timeOfDay->phase == TimeOfDay::DUSK)) {
        float warmth = timeOfDay->phase == TimeOfDay::DAWN
            ? 1.0f - timeOfDay->phaseProgress : timeOfDay->phaseProgress;
        cloudCap = Display::lerpColor565(cloudCap, RP::WARM,
                                         0.16f + warmth * 0.12f);
        birdColor = Display::lerpColor565(birdColor, RP::WARM, 0.12f);
    }
    Weather::drawClouds(c, cloudCap, cloudShade);
    Weather::drawBirds(c, birdColor);

    // Repaint the skyline after sky motion. This keeps atmospheric effects
    // behind tower mass while allowing window blink and flying traffic to use
    // the real frame clock.
    drawDistantTowers4(c, isNight, 0, now);
    drawFlyingTraffic4(c, isNight, now);
    drawMidgroundFacade4(c, isNight);

    // Live L4: every emitter starts from the restored unlit furniture shell,
    // so an off beat cannot leave stale glow in the retained substrate.
    const int signX = 264;
    const int signY = 76;
    const bool signLit = isStreetSignLit(signX, signY, isNight, now);
    drawPuddleReflection4(c, 60, kFgT + 16, 32, 8,
                          isNight, now, RP::WARM, isNight);
    drawPuddleReflection4(c, 200, kFgT + 32, 24, 8,
                          isNight, now, RP::NEON, signLit);
    drawStreetlightPole4(c, 48, isNight, now);
    drawStreetlightPole4(c, 176, isNight, now);
    drawNeonSignBuilding4(c, signX, signY, "CYBER", isNight, now);
    drawSolarLamp4(c, 80, kRoamT + 8, isNight, now);
    drawSolarLamp4(c, 240, kRoamT + 8, isNight, now);
}

void drawIdleBackdropBase(M5Canvas& c,
                          const TimeOfDay::State& timeOfDay,
                          int pigX) {
    const StreetSkyPalette sky = makeStreetSkyPalette(timeOfDay);
    drawStreetBaseLayers(c, TimeOfDay::isNight(timeOfDay),
                         sky.zenith, sky.horizon, pigX);
}

void drawIdleBackdropMotion(M5Canvas& c,
                            const TimeOfDay::State& timeOfDay,
                            uint32_t now, int pigX) {
    // Idle/HUNT's shared Weather compositor owns open-air rain, so this pass
    // deliberately stops at animated scenery and furniture sources.
    drawStreetMotionLayers(c, TimeOfDay::isNight(timeOfDay), &timeOfDay,
                           now, pigX);
}

void drawIdleBackdrop(M5Canvas& c, const TimeOfDay::State& timeOfDay,
                      uint32_t now, int pigX) {
    drawIdleBackdropBase(c, timeOfDay, pigX);
    drawIdleBackdropMotion(c, timeOfDay, now, pigX);
}

void drawStreetScene(M5Canvas& c, bool isNight, uint32_t now, int pigX) {
    uint16_t zenith = isNight
        ? Display::lerpColor565(RP::BG, RP::DEEP, 0.20f)
        : Display::lerpColor565(RP::WALL_MID, RP::CRT, 0.28f);
    uint16_t horizon = isNight
        ? Display::lerpColor565(RP::BG, RP::NEON, 0.12f)
        : Display::lerpColor565(RP::WALL_NEAR, RP::SHAFT, 0.30f);
    drawStreetBaseLayers(c, isNight, zenith, horizon, pigX);
    drawStreetMotionLayers(c, isNight, nullptr, now, pigX);
    if (isNight) {
        PixelWeather::OpenAirRainParams rp;
        rp.motion = 0.3f;
        rp.lateral = 0.1f;
        rp.imuPitch = 0.0f;
        rp.thundering = false;
        PixelWeather::drawOpenAirRain4(c, now, 0, kSkyT,
                                       SCREEN_WIDTH, kRoamB - kSkyT, rp);
    }
}

} // namespace PixelStreet
