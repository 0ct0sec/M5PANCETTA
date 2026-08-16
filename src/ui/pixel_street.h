/**
 * pixel_street.h — hunt/idle street zone components for HAMLET PANCETTA
 *
 * IDLE and HUNT share this cyberpunk street-level zone. WARDRIVE owns a
 * separate cockpit/city renderer. Day/night variants come from TimeOfDay.
 *
 * Layered layout (320×212 playfield):
 *   Y=14-120:  Continuous sky behind a floor-bound skyline
 *   Y=64-120:  Edge facades, signs, streetlights, and city horizon
 *   Y=120-180: Foreground — pavement, solar lamps, puddles
 *   Y=180-218: Pig roam zone — grass texture, footprints, sparse clutter
 *   Y=218-226: Floor gutter (shared)
 *
 * All coords snapped to kRoomPX=4. Stateless draw functions.
 * Palette colors from MenuPigRender::RP::.
 */
#pragma once

#include <M5Unified.h>
#include "display.h"
#include "ui_measurements.h"
#include "menu_pig_render.h"
#include "../core/time_of_day.h"

namespace PixelStreet {

using namespace MenuPigRender;
using namespace UIMeasurements::MenuPigLayout;

// ==[ STREET ZONE LAYOUT ]==
static constexpr int kStreetPlayT   = kRoomY;        // 14
static constexpr int kStreetPlayB   = kFloorY;       // 218
static constexpr int kStreetPlayH   = kStreetPlayB - kStreetPlayT; // 204

// Vertical zones
static constexpr int kSkyT          = kStreetPlayT;  // 14
static constexpr int kSkyB          = 120;           // continuous open-air plate
static constexpr int kMidT          = 64;            // architecture overlays the sky
static constexpr int kMidB          = kSkyB;         // city meets the pavement
static constexpr int kFgT           = kSkyB;         // 120
static constexpr int kFgB           = 180;           // 180
static constexpr int kRoamT         = 180;           // 180
static constexpr int kRoamB         = kStreetPlayB;  // 218
static constexpr int kGutterT       = kStreetPlayB;  // 218
static constexpr int kGutterB       = kStreetPlayB + 8; // 226
static_assert(kSkyB == kFgT && kMidT < kMidB && kMidB == kFgT &&
              kFgB == kRoamT && kRoamB == kGutterT,
              "street depth planes must meet the ground without gaps");
static_assert((kSkyB % kRoomPX) == 0 && (kMidT % kRoomPX) == 0 &&
              (kFgT % kRoomPX) == 0,
              "street architecture must stay on the 4px scenery grid");
static_assert(kGutterB == UIMeasurements::kScreenHeight - UIMeasurements::kBottomBarH,
              "street gutter must meet the bottom bar");

// ==[ PAVEMENT ]== Wet concrete grid, grime, gutter edge
void drawPavement4(M5Canvas& c, bool isNight, uint32_t now);

// ==[ GRASS VERGE ]== Synthetic turf with solar lamps
void drawGrassVerge4(M5Canvas& c, bool isNight, uint32_t now);

// ==[ SOLAR LAMP ]== Post + LED glow (pulsing at night)
void drawSolarLamp4(M5Canvas& c, int x, int y, bool isNight, uint32_t now);

// ==[ NEON SIGN BUILDING ]== Building-mounted neon signage
void drawNeonSignBuilding4(M5Canvas& c, int x, int y, const char* text,
                           bool isNight, uint32_t now);

// ==[ PUDDLE REFLECTION ]== Wet pavement mirror (sharp day, neon bloom night)
void drawPuddleReflection4(M5Canvas& c, int x, int y, int w, int h,
                           bool isNight, uint32_t now,
                           uint16_t sourceTint, bool sourceLit);

// ==[ DISTANT TOWERS ]== 3-layer parallax city backdrop
void drawDistantTowers4(M5Canvas& c, bool isNight, int parallaxX, uint32_t now);

// ==[ FLYING TRAFFIC ]== Neon headlight streaks in sky
void drawFlyingTraffic4(M5Canvas& c, bool isNight, uint32_t now);

// ==[ IDLE BACKDROP ]== Retained structure + live environmental motion.
// BASE contains the retained five-minute air/structure/ground bucket. MOTION
// redraws every clocked or animated source after restore, then restores the
// skyline and facade occlusion that live sky effects pass behind.
void drawIdleBackdropBase(M5Canvas& c, const TimeOfDay::State& timeOfDay,
                          int pigX);
void drawIdleBackdropMotion(M5Canvas& c,
                            const TimeOfDay::State& timeOfDay,
                            uint32_t now, int pigX);
// Convenience composition for uncached callers.
void drawIdleBackdrop(M5Canvas& c, const TimeOfDay::State& timeOfDay,
                      uint32_t now, int pigX);
// ==[ FULL STREET SCENE ]== Scenery plus open-air rain for future hunt use.
void drawStreetScene(M5Canvas& c, bool isNight, uint32_t now, int pigX);

// ==[ SKY GRADIENT ]== Zenith → horizon based on TimeOfDay
void drawSkyGradient4(M5Canvas& c, uint16_t zenith, uint16_t horizon, uint32_t now);

// ==[ STREETLIGHT POLE ]== Tall lamp post with warm pool
void drawStreetlightPole4(M5Canvas& c, int x, bool isNight, uint32_t now);

// ==[ GUTTER ]== Shared floor gutter with grime
void drawStreetGutter4(M5Canvas& c);

} // namespace PixelStreet
