/**
 * pixel_weather.h — weather/particle primitives for pixel art
 *
 * All effects stateless, grid-snapped, bounded rects.
 * Deterministic positions from hash + time (no per-frame RNG).
 */
#pragma once

#include <M5Unified.h>
#include "display.h"
#include "ui_measurements.h"
#include "menu_pig_render.h"

namespace PixelWeather {

using namespace MenuPigRender;
using namespace UIMeasurements::MenuPigLayout;

// ==[ WINDOW RAIN ]== Streaks + beads on glass pane
void drawWindowRain4(M5Canvas& c, uint32_t now,
                     int x, int y, int w, int h,
                     uint16_t reflectionTint, uint8_t intensity);

// ==[ OPEN-AIR RAIN ]== Three depth bands with stable lanes and ground impacts.
struct OpenAirRainParams {
    float motion = 0.0f;       // 0.0-1.0 speed factor
    float lateral = 0.0f;      // -1.0 to 1.0 wind bias
    float imuPitch = 0.0f;     // 0.0-1.0 forward tilt
    bool thundering = false;
};
void drawOpenAirRain4(M5Canvas& c, uint32_t now,
                      int x, int y, int w, int h,
                      const OpenAirRainParams& p);

// ==[ CONDENSATION BEADS ]== Slow capillary creep on cold surfaces
void drawCondensation4(M5Canvas& c, uint32_t now,
                       int x, int y, int w, int h, int count,
                       uint16_t tint, uint32_t seed);

// ==[ STEAM / VAPOR ]== Rising, fading wisps
void drawSteam4(M5Canvas& c, uint16_t fg, uint32_t now, int baseX, int baseY);
void drawVapor4(M5Canvas& c, uint32_t now, int x, int y,
                float rise, float wave, uint16_t color);

// ==[ FOG / HAZE ]== Sparse screen-blend overlay; density is 0..255.
void drawFogHaze4(M5Canvas& c, uint32_t now,
                  int x, int y, int w, int h,
                  uint16_t color, uint8_t density);

// ==[ THUNDER FLASH ]== Transient full-screen wash
void drawThunderFlash4(M5Canvas& c, uint32_t now,
                       uint16_t skyTint, float intensity);

// ==[ CELESTIAL BODIES ]==
void drawSun4(M5Canvas& c, int cx, int cy, int radius, uint32_t now);
void drawMoon4(M5Canvas& c, int cx, int cy, int radius, uint32_t phase, uint32_t now);
void drawStars4(M5Canvas& c, int x, int y, int w, int h, uint32_t seed);

// ==[ ROOM RAIN (INTERIOR) ]== Sill beads form, fall, splash
void drawRoomRain4(M5Canvas& c, uint32_t now,
                   int windowX, int windowY, int windowW, int windowH);

// ==[ SMOOTH WATER DROP ]== Crossfaded between adjacent 4px cells
void drawSmoothWaterDrop4(M5Canvas& c, int x, int yQ8,
                          int minY, int maxY, uint16_t color, uint8_t alpha);

// ==[ AMBIENT RAIN STREAKS ]== Wardrive windshield technique, interior variant
void drawAmbientRainStreaks4(M5Canvas& c, uint32_t now,
                             int x0, int y0, int w, int h,
                             uint16_t streakCol, uint16_t sparkCol);

// ==[ DUST MOTES ]== 5 particles, varied sizes/speeds
void drawDustMotes4(M5Canvas& c, uint32_t now);

} // namespace PixelWeather
