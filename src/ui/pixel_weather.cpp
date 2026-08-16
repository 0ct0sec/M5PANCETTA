/**
 * pixel_weather.cpp — weather/particle implementations
 */
#include "pixel_weather.h"
#include "pixel_primitives.h"
#include "../gfx/gfx.h"
#include <math.h>

namespace PixelWeather {

using namespace PixelPrim;

using namespace MenuPigRender;
using namespace UIMeasurements::MenuPigLayout;
using UIMeasurements::kTopBarH;

// ==[ ORDERED ROOM COVERAGE ]== Now in PixelPrim (pixel_primitives.h)

// ==[ RAIN STREAK GLOW PIXEL ]==
static void rainStreakGlowPx(M5Canvas& c, int x, int y, uint16_t col, uint8_t alpha) {
    if (alpha < 8) return;
    x = q4(x); y = q4(y);
    if (x < 0 || x + kRoomPX > c.width() ||
        y < kRoomY || y + kRoomPX > c.height() || y >= kFloorY) return;
    uint16_t base = Gfx::fastReadPx(c, x, y);
    c.fillRect(x, y, kRoomPX, kRoomPX, Gfx::screenBlend565(base, col, alpha));
}

// ==[ WINDOW RAIN ]== (from menu_pig_render.cpp drawWindowGlassRain)
void drawWindowRain4(M5Canvas& c, uint32_t now,
                     int x, int y, int w, int h,
                     uint16_t reflectionTint, uint8_t intensity) {
    int ix = q4(x + kRoomPX);
    int iy = q4(y + kRoomPX);
    int iw = (w - kRoomPX * 2 + 3) & ~3;
    int ih = (h - kRoomPX * 2 + 3) & ~3;
    if (iw < 12 || ih < 12) return;
    if (reflectionTint == 0) reflectionTint = RP::PUDDLE;

    uint16_t waterBody = Display::lerpColor565(RP::SHAFT, RP::BG, 0.18f);
    uint16_t streakCol = Display::lerpColor565(waterBody, reflectionTint, 0.20f);
    uint16_t sparkCol = Display::lerpColor565(RP::SHAFT, RP::FLUOR, 0.18f);

    // Sinusoidal streaks
    int streakCount = 5 + (ih / 28);
    if (streakCount > 10) streakCount = 10;
    const uint32_t fallStep = 88u;
    int xSpan = max(kRoomPX, iw);
    int travelH = ih + kRoomPX * 10;

    for (int i = 0; i < streakCount; i++) {
        uint32_t seed = wallHash(i, 91, 0x2A71u);
        uint32_t cycle = 1450u + ((seed >> 7) % 1050u);
        uint32_t local = (now + (seed & 0x7FFu)) % cycle;
        uint32_t phase = local / fallStep;
        uint32_t travelQ8 = local * (uint32_t)travelH * 256u / cycle;
        int baseYQ8 = (iy - kRoomPX * 4) * 256 + (int)travelQ8;
        int rawX = (int)((seed >> 11) % (uint32_t)xSpan);
        int baseX = ix + ((rawX + (int)(phase * (1 + (i & 1u)))) % (xSpan + kRoomPX * 2));
        baseX = q4(baseX);
        int len = 3 + (int)((seed >> 4) & 0x03u);
        int lean = ((seed >> 2) & 1u) ? kRoomPX : -kRoomPX;

        for (int s = 0; s < len; s++) {
            int sx = baseX - (s / 2) * lean;
            int syQ8 = baseYQ8 - s * kRoomPX * 256;
            int syPx = syQ8 >= 0 ? syQ8 / 256 : -((-syQ8 + 255) / 256);
            int sy = q4(syPx);
            if (sx < ix || sx >= ix + iw || sy + kRoomPX < iy || sy >= iy + ih) continue;
            uint16_t segCol = (s == 0) ? sparkCol : streakCol;
            int rawA = 142 - s * 20 + ((s == 0) ? 18 : 0);
            rawA = max(28, min(220, rawA));
            if (s == 0) {
                int fracQ8 = ((syQ8 - sy * 256) * 255) / (kRoomPX << 8);
                uint8_t a0 = (uint8_t)(rawA * (255 - fracQ8) / 255);
                uint8_t a1 = (uint8_t)(rawA * fracQ8 / 255);
                if (sy >= iy) rainStreakGlowPx(c, sx, sy, segCol, a0);
                if (sy + kRoomPX < iy + ih) rainStreakGlowPx(c, sx, sy + kRoomPX, segCol, a1);
            } else {
                int roundedY = sy + ((((syQ8 - sy * 256) >> 8) >= kRoomPX / 2) ? kRoomPX : 0);
                if (roundedY >= iy && roundedY < iy + ih)
                    rainStreakGlowPx(c, sx, roundedY, segCol, (uint8_t)rawA);
            }
        }
    }

    // Stable beads on glass
    auto glassGlow = [&](int bx, int by, uint16_t color, uint8_t alpha) {
        bx = q4(bx); by = q4(by);
        if (alpha < 8 || bx < ix || bx >= ix + iw || by < iy || by >= iy + ih) return;
        uint16_t base = Gfx::fastReadPx(c, bx, by);
        Gfx::fastFillBlock4(c, bx, by, Gfx::screenBlend565(base, color, alpha));
    };

    int beadCount = 4 + intensity / 32;
    if (beadCount > 8) beadCount = 8;
    int creepMax = max(kRoomPX, min(ih / 3, kRoomPX * 5));
    int startSpan = max(kRoomPX, ih - creepMax - kRoomPX);

    for (int i = 0; i < beadCount; ++i) {
        uint32_t seed = wallHash(i, x ^ y, 0x6A51u);
        int bx = (i < 2) ? (i == 0 ? ix : ix + iw - kRoomPX)
                         : ix + (int)((seed >> 9) % (uint32_t)max(kRoomPX, iw - kRoomPX));
        int by = iy + (int)((seed >> 17) % (uint32_t)startSpan);
        uint32_t cycle = 5200u + (seed & 0x7FFu);
        uint32_t local = (now + ((seed >> 5) & 0xFFFu)) % cycle;
        uint32_t hold = cycle * (55u + ((seed >> 3) & 0x0Fu)) / 100u;
        int creepQ8 = 0;
        if (local > hold) {
            uint32_t tQ8 = (local - hold) * 255u / max(1u, cycle - hold);
            uint32_t easedQ8 = tQ8 * tQ8 / 255u;
            int span = kRoomPX + (int)((seed >> 21) % (uint32_t)creepMax);
            creepQ8 = (int)((uint32_t)span * easedQ8 * 256u / 255u);
        }
        int yQ8 = by * 256 + creepQ8;
        int py = q4(yQ8 >> 8);
        int frac = ((yQ8 - py * 256) * 255) / (kRoomPX << 8);

        uint32_t fadeInEnd = cycle / 10u;
        uint32_t fadeOutStart = cycle * 88u / 100u;
        uint8_t lifeAlpha = 255;
        if (local < fadeInEnd) lifeAlpha = (uint8_t)(local * 255u / max(1u, fadeInEnd));
        else if (local > fadeOutStart) lifeAlpha = (uint8_t)((cycle - local) * 255u / max(1u, cycle - fadeOutStart));

        uint8_t bodyA = (uint8_t)(((34u + intensity / 3u) * lifeAlpha) / 255u);
        glassGlow(bx, py, waterBody, (uint8_t)((int)bodyA * (255 - frac) / 255));
        glassGlow(bx, py + kRoomPX, waterBody, (uint8_t)((int)bodyA * frac / 255));

        int glintX = bx + (((seed >> 2) & 1u) ? -kRoomPX : kRoomPX);
        uint8_t glintA = (uint8_t)(((18u + intensity / 5u) * lifeAlpha) / 255u);
        glassGlow(glintX, py, sparkCol, (uint8_t)((int)glintA * (255 - frac) / 255));
        glassGlow(glintX, py + kRoomPX, sparkCol, (uint8_t)((int)glintA * frac / 255));
    }

    // Two reflection ribbons
    for (int band = 0; band < 2; ++band) {
        uint32_t seed = wallHash(band, w, 0x6B71u);
        uint32_t phase = (now + (seed & 0x7FFu)) % (3600u + band * 900u);
        uint32_t half = (3600u + band * 900u) / 2u;
        uint8_t pulse = (uint8_t)(phase < half
            ? phase * 72u / max(1u, half)
            : (3600u + band * 900u - phase) * 72u / max(1u, half));
        int rx = ix + iw * (band == 0 ? 2 : 5) / 7;
        int ry = iy + kRoomPX * (2 + band * 3);
        for (int cell = 0; cell < 4; ++cell) {
            if ((wallHash(cell, band, seed) & 0x03u) == 0u) continue;
            int px = q4(rx);
            int py = q4(ry + cell * kRoomPX);
            if (px < ix || px >= ix + iw || py < iy || py >= iy + ih) continue;
            uint16_t base = Gfx::fastReadPx(c, px, py);
            Gfx::fastFillBlock4(c, px, py, Gfx::screenBlend565(base, reflectionTint, pulse));
        }
    }
}

// ==[ OPEN-AIR RAIN ]== stable far/mid/near depth bands
void drawOpenAirRain4(M5Canvas& c, uint32_t now,
                      int x, int y, int w, int h,
                      const OpenAirRainParams& p) {
    const int clipX0 = (max(0, x) + kRoomPX - 1) & ~(kRoomPX - 1);
    const int clipY0 = (max(kRoomY, y) + kRoomPX - 1) & ~(kRoomPX - 1);
    const int clipX1 = min((int)c.width(), x + w) & ~(kRoomPX - 1);
    const int clipY1 = min((int)c.height(), y + h) & ~(kRoomPX - 1);
    const int clipW = clipX1 - clipX0;
    const int clipH = clipY1 - clipY0;
    if (clipW < kRoomPX * 4 || clipH < kRoomPX * 4) return;

    const float motion = Gfx::clamp01(p.motion + p.imuPitch * 0.4f);
    const int motionQ8 = (int)(motion * 256.0f + 0.5f);
    const int windQ8 = max(-256, min(256, (int)(p.lateral * 256.0f)));
    const int windSign = (windQ8 > 20) - (windQ8 < -20);
    const uint16_t water = Display::lerpColor565(RP::SHAFT, RP::PUDDLE, 0.30f);
    const uint16_t farWater = Display::lerpColor565(water, RP::BG, 0.38f);
    const uint16_t glint = Display::lerpColor565(RP::SHAFT, RP::FLUOR,
                                                 p.thundering ? 0.30f : 0.10f);
    const int thunderBoost = p.thundering ? 38 : 0;

    struct RainBand {
        uint8_t count;
        uint8_t motionExtra;
        uint8_t tailMin;
        uint8_t tailRange;
        uint8_t alpha;
        uint8_t windScale;
        uint16_t cycleBase;
        uint16_t cycleRange;
        uint32_t salt;
    };
    static constexpr RainBand kBands[] = {
        {9,  2, 1, 2,  54, 1, 1700, 900, 0x31A1u},
        {10, 3, 2, 3,  88, 2, 1050, 620, 0x42B1u},
        {5,  2, 4, 2, 132, 3,  720, 380, 0x53C1u},
    };

    auto blendCell = [&](int px, int py, uint16_t color, int alpha) {
        if (alpha < 8 || px < clipX0 || px + kRoomPX > clipX1 ||
            py < clipY0 || py + kRoomPX > clipY1) return;
        alpha = min(255, alpha);
        uint16_t base = Gfx::fastReadPx(c, px, py);
        Gfx::fastFillBlock4(c, px, py,
                            Gfx::screenBlend565(base, color, (uint8_t)alpha));
    };

    for (int bandIndex = 0; bandIndex < 3; ++bandIndex) {
        const RainBand& band = kBands[bandIndex];
        const int count = band.count + (motionQ8 * band.motionExtra) / 256;
        for (int i = 0; i < count; ++i) {
            const uint32_t seed = wallHash(i, bandIndex, band.salt);
            uint32_t cycle = band.cycleBase + seed % band.cycleRange;
            const uint32_t speedQ8 = 256u - (uint32_t)(motionQ8 * 88 / 256);
            cycle = max(360u, cycle * speedQ8 / 256u);
            const int tailCells = band.tailMin +
                (int)((seed >> 9) % max(1u, (uint32_t)band.tailRange));
            const int overscan = (tailCells + 2) * kRoomPX;
            const int travelPx = clipH + overscan + kRoomPX;
            const uint32_t local = (now + ((seed >> 16) & 0x0FFFu)) % cycle;
            const int travelQ8 = (int)(((uint64_t)local * (uint32_t)travelPx * 256u) /
                                       cycle);
            const int headYQ8 = (clipY0 - overscan) * 256 + travelQ8;
            const int travelledPx = travelQ8 >> 8;
            const int windPx = (windQ8 * travelledPx * band.windScale) /
                               (256 * 6);
            const int laneSpan = max(kRoomPX, clipW - kRoomPX * 2);
            const int laneX = clipX0 + kRoomPX +
                (int)((seed >> 3) % (uint32_t)laneSpan);
            const int headX = q4(laneX + windPx);
            const int headYPx = headYQ8 >> 8;
            const int headY = q4(headYPx);
            const int fracQ8 = ((headYQ8 - headY * 256) * 255) /
                               (kRoomPX << 8);

            for (int segment = 0; segment < tailCells; ++segment) {
                const int px = headX - windSign * ((segment + 1) / 2) * kRoomPX;
                const int py = headY - segment * kRoomPX;
                int alpha = band.alpha - segment * (bandIndex == 2 ? 16 : 13) +
                            thunderBoost;
                const uint16_t color = segment == 0 && bandIndex > 0
                    ? glint : (bandIndex == 0 ? farWater : water);
                if (segment == 0) {
                    blendCell(px, py, color, alpha * (255 - fracQ8) / 255);
                    blendCell(px, py + kRoomPX, color, alpha * fracQ8 / 255);
                } else {
                    const int roundedY = py + (fracQ8 >= 128 ? kRoomPX : 0);
                    blendCell(px, roundedY, color, alpha);
                }
            }

            // Mid and near drops briefly kick two cells sideways on contact.
            // This is tied to the same stable lane and cycle as the falling head.
            if (bandIndex > 0 && headYPx >= clipY1 - kRoomPX &&
                headYPx < clipY1 + kRoomPX * 2) {
                const int impactY = clipY1 - kRoomPX;
                const int impactAlpha = band.alpha / 2 + thunderBoost;
                blendCell(headX - kRoomPX, impactY, water, impactAlpha);
                blendCell(headX + kRoomPX, impactY, water, impactAlpha);
            }
        }
    }
}

// ==[ CONDENSATION BEADS ]== (from menu_pig_render.cpp)
void drawCondensation4(M5Canvas& c, uint32_t now,
                       int x, int y, int w, int h, int count,
                       uint16_t tint, uint32_t seed) {
    x = q4(x); y = q4(y); w = (w + 3) & ~3; h = (h + 3) & ~3;
    if (w < 8 || h < 8) return;
    for (int i = 0; i < count; ++i) {
        uint32_t bs = seed + i * 173u;
        int bx = x + 4 + (int)((bs & 0xFFu) % max(1, w - 8));
        int by = y + 4 + (int)(((bs >> 8) & 0xFFu) % max(1, h - 8));
        bx = q4(bx); by = q4(by);
        uint32_t cycle = 4000u + (bs & 0x7FFu);
        uint32_t local = (now + (bs >> 16)) % cycle;
        if (local > cycle * 7 / 8) continue; // mostly visible
        uint16_t col = tint ? tint : RP::PUDDLE;
        uint16_t base = Gfx::fastReadPx(c, bx, by);
        Gfx::fastFillBlock4(c, bx, by, Gfx::screenBlend565(base, col, 120));
        // Highlight
        base = Gfx::fastReadPx(c, bx + 2, by);
        Gfx::fastFillBlock2(c, bx + 2, by, Gfx::screenBlend565(base, RP::FLUOR, 80));
    }
}

// ==[ STEAM ]== Rising vapor from hot sources
void drawSteam4(M5Canvas& c, uint16_t fg, uint32_t now, int baseX, int baseY) {
    static constexpr uint32_t STEAM_CYCLE = 1800;
    static constexpr int STEAM_RISE = 16;
    for (int i = 0; i < 3; i++) {
        uint32_t phase = (now + (uint32_t)i * (STEAM_CYCLE / 3)) % STEAM_CYCLE;
        float t = (float)phase / (float)STEAM_CYCLE;
        int rise = (int)(t * STEAM_RISE);
        float wave = Gfx::fastSinf(t * 6.283f + (float)i * 1.5f) * 3.0f;
        int sx = q4(baseX + (int)wave + (i - 1) * 4);
        int sy = q4(baseY - rise);
        if (sy <= kTopBarH + 2 || sy >= kFloorY) continue;
        float fade = (t < 0.6f) ? 1.0f : (1.0f - (t - 0.6f) / 0.4f);
        uint8_t keep = (uint8_t)(200.0f * fade);
        if ((wallHash(sx, sy, 0xBE11 + i * 53) & 0xFF) < keep) {
            uint16_t col = (t < 0.4f) ? RP::SOFT : RP::DUST;
            Gfx::fastFillBlock4(c, sx, sy, col);
        }
    }
}

void drawVapor4(M5Canvas& c, uint32_t now, int x, int y,
                float rise, float wave, uint16_t color) {
    x = q4(x); y = q4(y);
    uint32_t cycle = 3200;
    for (int i = 0; i < 5; i++) {
        uint32_t phase = (now + (uint32_t)i * (cycle / 5)) % cycle;
        float t = (float)phase / (float)cycle;
        int r = (int)(t * rise);
        float wv = Gfx::fastSinf(t * 6.28318f + (float)i * 1.5f) * (3.0f + (float)(i & 1) * 2.0f);
        int sx = q4(x + (int)wv + (i - 2) * 4);
        int sy = q4(y - r);
        if (sy <= kTopBarH + 2 || sy >= kFloorY) continue;
        float fade = (t < 0.6f) ? 1.0f : (1.0f - (t - 0.6f) / 0.4f);
        uint8_t keep = (uint8_t)(200.0f * fade);
        if ((wallHash(sx, sy, 0xBE11 + i * 53) & 0xFF) < keep) {
            Gfx::fastFillBlock4(c, sx, sy, color);
        }
    }
}

// ==[ FOG / HAZE ]== Sparse volumetric
void drawFogHaze4(M5Canvas& c, uint32_t now,
                  int x, int y, int w, int h,
                  uint16_t color, uint8_t density) {
    x = q4(x); y = q4(y); w = (w + 3) & ~3; h = (h + 3) & ~3;
    if (w < 16 || h < 16 || density == 0) return;
    uint8_t coverage = (uint8_t)((density + 15u) / 16u);
    if (coverage > 16u) coverage = 16u;
    const uint8_t phase = (uint8_t)((now / 80u) & 31u);
    const uint8_t triangle = phase < 16u ? phase : (uint8_t)(31u - phase);
    const uint8_t alpha = (uint8_t)(32u + triangle / 2u);
    for (int py = y; py < y + h; py += kRoomPX * 2) {
        for (int px = x; px < x + w; px += kRoomPX * 2) {
            if (orderedRoomCoverage(px, py, coverage, 0xF061u)) {
                uint16_t base = Gfx::fastReadPx(c, px, py);
                Gfx::fastFillBlock4(c, px, py,
                                    Gfx::screenBlend565(base, color, alpha));
            }
        }
    }
}

// ==[ THUNDER FLASH ]==
void drawThunderFlash4(M5Canvas& c, uint32_t now,
                       uint16_t skyTint, float intensity) {
    // Called when thunder flashing - transient wash over entire playfield
    uint8_t alpha = (uint8_t)(intensity * 80.0f);
    if (alpha < 8) return;
    for (int py = kRoomY; py < kFloorY; py += kRoomPX) {
        for (int px = 4; px < SCREEN_WIDTH - 4; px += kRoomPX) {
            uint16_t base = Gfx::fastReadPx(c, px, py);
            Gfx::fastFillBlock4(c, px, py, Gfx::screenBlend565(base, skyTint, alpha));
        }
    }
}

// ==[ CELESTIAL BODIES ]==
void drawSun4(M5Canvas& c, int cx, int cy, int radius, uint32_t now) {
    cx = q4(cx); cy = q4(cy); radius = q4(radius);
    // Core (grid-snapped)
    PixelPrim::fillRect4(c, cx - radius, cy - radius, radius * 2, radius * 2, RP::FLUOR);
    // Corona rays (pre-computed for 8 directions, avoids cosf/sinf)
    static constexpr int kCos8[8] = {100, 71, 0, -71, -100, -71, 0, 71};
    static constexpr int kSin8[8] = {0, 71, 100, 71, 0, -71, -100, -71};
    for (int i = 0; i < 8; i++) {
        int x1 = cx + (kCos8[i] * radius) / 100;
        int y1 = cy + (kSin8[i] * radius) / 100;
        int x2 = cx + (kCos8[i] * (radius + 8)) / 100;
        int y2 = cy + (kSin8[i] * (radius + 8)) / 100;
        PixelPrim::fatLine4(c, x1, y1, x2, y2, RP::WARM);
    }
    // Shimmer
    if ((now / 200) & 1) {
        PixelPrim::fillRect4(c, cx - 4, cy - 4, 8, 8, RP::FLUOR);
    }
}

void drawMoon4(M5Canvas& c, int cx, int cy, int radius, uint32_t phase, uint32_t now) {
    cx = q4(cx); cy = q4(cy); radius = q4(radius);
    // Moon disk (phase-aware) — no background clear, sky shows through unlit portion
    uint16_t moonCol = Display::lerpColor565(RP::SHAFT, RP::FLUOR, 0.3f);
    int phase8 = phase & 7;
    for (int py = cy - radius; py < cy + radius; py += kRoomPX) {
        for (int px = cx - radius; px < cx + radius; px += kRoomPX) {
            int dx = px + 2 - cx;
            int dy = py + 2 - cy;
            if (dx*dx + dy*dy <= radius*radius) {
                // Simple phase: left half dark for waning, right for waxing
                bool lit = (phase8 < 4) ? (dx >= 0) : (dx <= 0);
                if (phase8 == 0 || phase8 == 4) lit = true; // full/new
                if (lit) {
                    c.fillRect(px, py, kRoomPX, kRoomPX, moonCol);
                }
            }
        }
    }
    // Craters
    for (int i = 0; i < 3; i++) {
        uint32_t cs = wallHash(i, cx, 0xC00Lu);
        int crx = cx + (int)((cs & 0xFF) % (radius - 4)) - (radius - 4) / 2;
        int cry = cy + (int)(((cs >> 8) & 0xFF) % (radius - 4)) - (radius - 4) / 2;
        crx = q4(crx); cry = q4(cry);
        c.fillRect(crx, cry, 4, 4, RP::SHADOW_C);
    }
}

void drawStars4(M5Canvas& c, int x, int y, int w, int h, uint32_t seed) {
    x = q4(x); y = q4(y); w = (w + 3) & ~3; h = (h + 3) & ~3;
    for (int py = y; py < y + h; py += kRoomPX * 4) {
        for (int px = x; px < x + w; px += kRoomPX * 4) {
            uint32_t hs = wallHash(px, py, seed);
            if ((hs & 0xFF) < 4) { // ~1.5%
                uint16_t col = ((hs >> 8) & 1) ? RP::FLUOR : RP::SHAFT;
                c.fillRect(px, py, kRoomPX, kRoomPX, col);
            }
        }
    }
}

// ==[ ROOM RAIN ]== (from menu_pig_render.cpp)
void drawRoomRain4(M5Canvas& c, uint32_t now,
                   int windowX, int windowY, int windowW, int windowH) {
    uint16_t water = Display::lerpColor565(RP::SHAFT, RP::PUDDLE, 0.32f);
    uint16_t glint = Display::lerpColor565(RP::SHAFT, RP::FLUOR, 0.16f);
    int sillY = windowY + windowH;
    int dropStartY = q4(sillY + kRoomPX - 1);
    int lastDropY = q4(kFloorY - kRoomPX);
    int fallH = max(0, lastDropY - dropStartY);
    if (dropStartY > lastDropY || windowW <= kRoomPX * 2) return;

    auto drawTweenedDrop = [&](int x, int yQ8, uint16_t color, uint8_t alpha) {
        int yPx = yQ8 >> 8;
        int cellY = q4(yPx);
        int fracQ8 = ((yQ8 - cellY * 256) * 255) / (kRoomPX << 8);
        auto drawCell = [&](int py, uint8_t a) {
            if (a < 8 || py < dropStartY || py + kRoomPX > kFloorY) return;
            int sx = q4(x);
            uint16_t base = Gfx::fastReadPx(c, sx, py);
            Gfx::fastFillBlock4(c, sx, py, Gfx::screenBlend565(base, color, a));
        };
        drawCell(cellY, (uint8_t)((int)alpha * (255 - fracQ8) / 255));
        drawCell(cellY + kRoomPX, (uint8_t)((int)alpha * fracQ8 / 255));
    };

    for (int i = 0; i < 4; i++) {
        uint32_t cycle = 2600u + (uint32_t)i * 430u;
        uint32_t local = (now + (uint32_t)i * 719u) % cycle;
        uint32_t formEnd = cycle * 28u / 100u;
        uint32_t fallEnd = cycle * 82u / 100u;
        int spread = max(1, windowW - 8);
        int sx = windowX + 4 + (int)((wallHash(i, 0, 0xD2D2) >> 4) % (uint32_t)spread);
        sx = q4(sx);
        if (local < formEnd) {
            uint8_t alpha = (uint8_t)(28u + local * 82u / max(1u, formEnd));
            uint16_t base = Gfx::fastReadPx(c, sx, dropStartY);
            Gfx::fastFillBlock4(c, sx, dropStartY, Gfx::screenBlend565(base, glint, alpha));
        } else if (local < fallEnd) {
            uint32_t tQ8 = (local - formEnd) * 255u / max(1u, fallEnd - formEnd);
            uint32_t easedQ8 = tQ8 * tQ8 / 255u;
            int yQ8 = dropStartY * 256 + (int)((uint32_t)fallH * easedQ8 * 256u / 255u);
            drawTweenedDrop(sx, yQ8, water, 124);
            drawTweenedDrop(sx, yQ8 - kRoomPX * 256, water, 48);
        } else {
            uint32_t splashSpan = max(1u, cycle - fallEnd);
            uint8_t fade = (uint8_t)(96u - min(88u, (local - fallEnd) * 88u / splashSpan));
            int splashY = lastDropY;
            for (int side = -1; side <= 1; side += 2) {
                int px = sx + side * kRoomPX;
                if (px < 0 || px >= SCREEN_WIDTH) continue;
                uint16_t base = Gfx::fastReadPx(c, px, splashY);
                Gfx::fastFillBlock4(c, px, splashY, Gfx::screenBlend565(base, RP::PUDDLE, fade));
            }
        }
    }
}

// ==[ SMOOTH WATER DROP ]== (from menu_pig_render.cpp)
void drawSmoothWaterDrop4(M5Canvas& c, int x, int yQ8,
                          int minY, int maxY, uint16_t color, uint8_t alpha) {
    if (alpha < 8 || minY >= maxY) return;
    int sx = q4(x);
    int yPx = yQ8 >= 0 ? yQ8 / 256 : -((-yQ8 + 255) / 256);
    int cellY = q4(yPx);
    int frac = ((yQ8 - cellY * 256) * 255) / (kRoomPX << 8);
    auto drawCell = [&](int py, uint8_t weight) {
        if (weight < 8 || sx < 0 || sx + kRoomPX > c.width() ||
            py < minY || py + kRoomPX > maxY) return;
        uint8_t cellAlpha = (uint8_t)((int)alpha * weight / 255);
        uint16_t base = Gfx::fastReadPx(c, sx, py);
        Gfx::fastFillBlock4(c, sx, py, Gfx::screenBlend565(base, color, cellAlpha));
    };
    drawCell(cellY, (uint8_t)(255 - frac));
    drawCell(cellY + kRoomPX, (uint8_t)frac);
}

// ==[ AMBIENT RAIN STREAKS ]== (from menu_pig_render.cpp)
void drawAmbientRainStreaks4(M5Canvas& c, uint32_t now,
                             int x0, int y0, int w, int h,
                             uint16_t streakCol, uint16_t sparkCol) {
    if (w < 12 || h < 16) return;
    int streakCount = 5 + (h / 28);
    if (streakCount > 10) streakCount = 10;
    const uint32_t fallStep = 88u;
    int xSpan = max(kRoomPX, w);
    int travelH = h + kRoomPX * 10;

    for (int i = 0; i < streakCount; i++) {
        uint32_t seed = wallHash(i, 91, 0x2A71u);
        uint32_t cycle = 1450u + ((seed >> 7) % 1050u);
        uint32_t local = (now + (seed & 0x7FFu)) % cycle;
        uint32_t phase = local / fallStep;
        uint32_t travelQ8 = local * (uint32_t)travelH * 256u / cycle;
        int baseYQ8 = (y0 - kRoomPX * 4) * 256 + (int)travelQ8;
        int rawX = (int)((seed >> 11) % (uint32_t)xSpan);
        int baseX = x0 + ((rawX + (int)(phase * (1 + (i & 1u)))) % (xSpan + kRoomPX * 2));
        baseX = q4(baseX);
        int len = 3 + (int)((seed >> 4) & 0x03u);
        int lean = ((seed >> 2) & 1u) ? kRoomPX : -kRoomPX;

        for (int s = 0; s < len; s++) {
            int sx = baseX - (s / 2) * lean;
            int syQ8 = baseYQ8 - s * kRoomPX * 256;
            int syPx = syQ8 >= 0 ? syQ8 / 256 : -((-syQ8 + 255) / 256);
            int sy = q4(syPx);
            if (sx < x0 || sx >= x0 + w || sy + kRoomPX < y0 || sy >= y0 + h) continue;
            uint16_t segCol = (s == 0) ? sparkCol : streakCol;
            int rawA = 142 - s * 20 + ((s == 0) ? 18 : 0);
            rawA = max(28, min(220, rawA));
            if (s == 0) {
                int fracQ8 = ((syQ8 - sy * 256) * 255) / (kRoomPX << 8);
                uint8_t a0 = (uint8_t)(rawA * (255 - fracQ8) / 255);
                uint8_t a1 = (uint8_t)(rawA * fracQ8 / 255);
                if (sy >= y0) rainStreakGlowPx(c, sx, sy, segCol, a0);
                if (sy + kRoomPX < y0 + h) rainStreakGlowPx(c, sx, sy + kRoomPX, segCol, a1);
            } else {
                int roundedY = sy + ((((syQ8 - sy * 256) >> 8) >= kRoomPX / 2) ? kRoomPX : 0);
                if (roundedY >= y0 && roundedY < y0 + h)
                    rainStreakGlowPx(c, sx, roundedY, segCol, (uint8_t)rawA);
            }
        }
    }
}

// ==[ DUST MOTES ]==
void drawDustMotes4(M5Canvas& c, uint32_t now) {
    static const int sizes[5][2] = {{4,4}, {4,4}, {4,8}, {4,8}, {8,4}};
    static const int cycles[5] = {3000, 3000, 5000, 5000, 3000};
    for (int i = 0; i < 5; i++) {
        uint32_t seed = (uint32_t)(i * 8291 + 5347);
        float period = (float)cycles[i];
        float phase = (float)((now + (uint32_t)i * 1200u) % (uint32_t)cycles[i]) / period;
        int baseX = 20 + (int)(seed % 200u);
        int baseY = kRoomY + 12 + (int)((seed >> 8) % 60);
        int dx = (int)(Gfx::fastSinf(phase * 6.28318f * 2.0f + (float)i * 1.7f) * 8.0f);
        int dy = (int)(Gfx::fastSinf(phase * 6.28318f * 4.0f + (float)i * 2.3f) * 4.0f);
        int mx = q4(baseX + dx);
        int my = q4(baseY + dy);
        int mw = sizes[i][0], mh = sizes[i][1];
        if (mx > 4 && mx + mw < SCREEN_WIDTH - 4 && my > kRoomY + 4 && my + mh < kFloorY - 4)
            c.fillRect(mx, my, mw, mh, RP::DUST);
    }
}

} // namespace PixelWeather
