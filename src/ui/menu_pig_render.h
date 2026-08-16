/**
 * menu_pig_render.h — shared stateless rendering infrastructure
 *
 * Room palette, blend helpers, background textures, lighting, environmental
 * props. ALL functions here are stateless — they take canvas + position + colors.
 * State-dependent rendering (neon timing, room draw, pig draw) stays in menu_pig.cpp.
 */
#pragma once

#include <M5Unified.h>
#include "display.h"  // includes gfx.h
#include "room_light_loop.h"
#include "ui_measurements.h"
#include "../piglet/avatar.h"

namespace MenuPigRender {

using namespace UIMeasurements::MenuPigLayout;

// ==[ ROOM PALETTE ]== theme-derived 2-color + interpolation
// All shades lerp between theme bg (t=0) and fg (t=1). Call update() once per frame.
namespace RP {
    extern uint16_t BG, DEEP, SHADOW_E, SHADOW_C;
    extern uint16_t WALL_FAR, FILL, FLOOR_GRIME, GREEN_DK;
    extern uint16_t WALL_MID, FLOOR_GRID, SOFT, DUST, SHAFT;
    extern uint16_t WALL_NEAR, CRT, WARM, NEON, PUDDLE;
    extern uint16_t LED, SPARK, VEND, STRUCT, FLUOR;
    // pre-darkened variants — noir rooms draw furniture dark, skip global darken
    extern uint16_t D_STRUCT, D_WALL_NEAR, D_FILL, D_DEEP, D_WARM; // 40% (Room 2, ~Room 0/4)
    extern uint16_t D50_STRUCT, D50_FILL;                         // 50% (Room 1 deep noir)
    void update();
}

// ==[ GFX FORWARDERS ]== canonical impls in gfx/gfx.h
inline uint16_t fastReadPx(M5Canvas& c, int x, int y) { return Gfx::fastReadPx(c, x, y); }
inline float clamp01f(float v) { return Gfx::clamp01(v); }
inline uint16_t screenBlend565(uint16_t base, uint16_t light, uint8_t s8) { return Gfx::screenBlend565(base, light, s8); }
inline uint16_t screenBlend565f(uint16_t base, uint16_t light, float s) { return Gfx::screenBlend565f(base, light, s); }
inline uint16_t lerpColor565_8(uint16_t c1, uint16_t c2, uint8_t t8) { return Gfx::lerpColor565_8(c1, c2, t8); }
inline uint16_t clampBrightness565(uint16_t c, uint8_t maxBright) { return Gfx::clampBrightness565(c, maxBright); }
uint16_t darken565(uint16_t base, float factor);
uint16_t mixColor565(uint16_t base, uint16_t tint, uint8_t amount);
inline void fastFillBlock4(M5Canvas& c, int x, int y, uint16_t color) { Gfx::fastFillBlock4(c, x, y, color); }
inline void fastFillBlock2(M5Canvas& c, int x, int y, uint16_t color) { Gfx::fastFillBlock2(c, x, y, color); }
// Skip pixels near room background — prevents transparent-square artifacts
// from rectangular bounding box operations (furniture wash, pig noir pass)
inline bool isNearBG(uint16_t color, int threshold = 18) {
    int diff = (int)Gfx::brightness565(color) - (int)Gfx::brightness565(RP::BG);
    if (diff < 0) diff = -diff;
    return diff < threshold;
}

inline uint32_t wallHash(int x, int y, uint32_t seed) { return Gfx::wallHash(x, y, seed); }
inline float fastSinf(float rad) { return Gfx::fastSinf(rad); }
using Gfx::SIN_LUT_Q15;

// ==[ POP OUTLINE ]==
enum class PopOutlineStyle : uint8_t { SOLID, SPARSE, MIXED };
void drawRoomPopPixel(M5Canvas& canvas, int x, int y, uint16_t color);
void drawPopOutline1px(M5Canvas& canvas, int x, int y, int w, int h,
                       PopOutlineStyle style, uint32_t seed);

// ==[ DITHERING ]==
using Gfx::bayer4;

// ==[ BACKGROUND TEXTURES ]== legacy entry points; both forward to the single
// PixelMat implementation. `variant` reseeds the wear so two callers sharing a
// rect do not render the same wall twice.
void drawConcreteWall(M5Canvas& canvas, int sx, int sy, int w, int h,
                      uint32_t variant = 0);
void drawMetalFloor(M5Canvas& canvas, uint32_t variant = 0);
void drawDustMotes(M5Canvas& canvas, uint16_t fg, uint32_t now);

// ==[ LIGHTING ]==
void drawLightPool(M5Canvas& canvas, uint16_t fg, int x, int y, int w, int h,
                   uint8_t density, uint32_t seed = 77077);
void drawLightPoolGradient(M5Canvas& canvas, uint16_t fg, int x, int y, int w, int h,
                           uint8_t centerDensity, uint32_t seed = 88088);
void drawNeonWash(M5Canvas& canvas, int x, int y, int w, int h,
                  PigLight light, uint16_t surfaceColor,
                  float maxRadius = 160.0f, float strength = 0.35f,
                  uint32_t seed = 0x4C01);
void drawFurnitureWash(M5Canvas& canvas, int x, int y, int w, int h,
                       PigLight light, float maxRadius = 140.0f,
                       float strength = 0.55f);

// ==[ MULTI-EMITTER FURNITURE WASH ]== single-pass over same rect with N lights
struct FurnitureWashLight {
    PigLight light;
    float maxRadius;
    float strength;
};
void drawFurnitureWashMulti(M5Canvas& canvas, int x, int y, int w, int h,
                            const FurnitureWashLight* lights, int count);
// Shared final-pass composition: fat-pixel edge falloff + floor-edge halation.
void drawRoomNoirFrame(M5Canvas& canvas, uint8_t room, PigLight keyLight,
                       uint32_t now);

// ==[ PIG NOIR EMITTER EVAL ]== pre-computed radius reciprocals for inner loops
struct PigLightEval {
    float x, y, maxR2, invMaxR2, weight;
    uint16_t tint;
    bool active;
    PigLightEval() : x(0), y(0), maxR2(0), invMaxR2(0), weight(0), tint(0), active(false) {}
    PigLightEval(const PigLight& l, float radius, float w)
        : x(l.x), y(l.y), maxR2(radius * radius), invMaxR2(0),
          weight(w), tint(l.tint), active(l.tint != 0 && radius > 1.0f) {
        invMaxR2 = active ? 1.0f / maxR2 : 0.0f;
    }
};

inline float emitterScore(const PigLightEval& e, float px, float py) {
    if (!e.active) return 0.0f;
    float dx = e.x - px, dy = e.y - py;
    float dist2 = dx * dx + dy * dy;
    if (dist2 >= e.maxR2) return 0.0f;
    float t = 1.0f - dist2 * e.invMaxR2;
    return e.weight * t * t;
}
void drawVolumetricDustBeam(M5Canvas& canvas, uint32_t now,
                            int topCenterX, int topY, int topWidth,
                            int bottomCenterX, int bottomY, int bottomWidth,
                            uint16_t shaftColor, uint16_t dustColor,
                            uint32_t seedBase = 0x7D11u,
                            const RoomLightLoopFrame* syncedLoop = nullptr);
enum class RoomWindowBackdropStyle : uint8_t {
    NoirApartment,
    RamenBar,
    ComfortBalcony,
};
struct RoomWindowBackdropParams {
    RoomWindowBackdropStyle style = RoomWindowBackdropStyle::NoirApartment;
    bool thunder = false;
    bool tintActive = false;
    uint16_t tintColor565 = 0;
    uint8_t tintIntensity = 0;
    int8_t parallaxX = 0;
};
void drawRoomWindowBackdropBase(M5Canvas& canvas,
                                int wx, int wy, int ww, int wh,
                                const RoomWindowBackdropParams& params);
void drawRoomWindowBackdropMotion(M5Canvas& canvas, uint32_t now,
                                  int wx, int wy, int ww, int wh,
                                  const RoomWindowBackdropParams& params);
void drawRoomWindowBackdrop(M5Canvas& canvas, uint32_t now,
                            int wx, int wy, int ww, int wh, float pigPosX,
                            const RoomWindowBackdropParams& params);

// ==[ ROOM WEATHER ]==
void drawRoomRain(M5Canvas& canvas, uint32_t now,
                  int windowX, int windowY, int windowW, int windowH);
// One bounded Q8 water bead, crossfaded between adjacent 4px scenery cells.
void drawSmoothWaterDrop(M5Canvas& canvas, int x, int yQ8,
                         int minY, int maxY, uint16_t color,
                         uint8_t alpha = 128);
// Rain streaks on window glass (wardrive technique, clipped to interior pane).
void drawWindowGlassRain(M5Canvas& canvas, uint32_t now,
                         int windowX, int windowY, int windowW, int windowH,
                         uint16_t reflectionTint = 0,
                         uint8_t intensity = 96);
// Wardrive windshield streaks adapted for interior playfield (alpha-blended).
void drawAmbientRainStreaks(M5Canvas& canvas, uint32_t now,
                            int x0, int y0, int w, int h,
                            uint16_t streakCol, uint16_t sparkCol);
void drawCondensation(M5Canvas& canvas, uint32_t now,
                      int surfX, int surfY, int surfW, int surfH, int count,
                      uint16_t tint = 0, uint32_t stableSalt = 0xCDCDu);
void drawSteam(M5Canvas& canvas, uint16_t fg, uint32_t now, int baseX, int baseY);
// Cigarette smoke lives in src/ui/smoke_volume.h, not here. It is the one
// effect in the game that is deliberately stateful — a plume has to outlive the
// cigarette that made it — so it cannot be a stateless RP:: helper.

// ==[ ENVIRONMENTAL PROPS — stateless ]== all take explicit position + colors

// Furniture
void drawWornCouch(M5Canvas& canvas, int sx, int sy, int sw, int sh);
void drawFloorGrate(M5Canvas& canvas, uint16_t fg, uint16_t bg,
                    int rx, int ry, int rw, int rh);
void drawWallPipes(M5Canvas& canvas, uint16_t fg, int px, int py, bool withValve);
void drawRamenCounter(M5Canvas& canvas, uint16_t fg, uint16_t bg,
                      int cx, int cy, int cw, int ch);
void drawNoodleBowl(M5Canvas& canvas, uint16_t fg, uint16_t bg,
                    int bx, int by, int bw, int bh,
                    bool noHaloSolid = false, uint16_t bowlColor = 0,
                    int px = kPigPX);
void drawBarStool(M5Canvas& canvas, uint16_t fg, int sx, int sy, int sw);
void drawDeskLamp(M5Canvas& canvas, uint32_t now, int lx, int ly);
void drawWallMonitor(M5Canvas& canvas, uint32_t now, int mx, int my,
                     int mw, int mh, uint32_t seed);
void drawServerCluster(M5Canvas& canvas, uint32_t now, int sx, int sy,
                       int sw, int sh, int dx = 0);
void drawAlleyCrate(M5Canvas& canvas, uint16_t fg, uint16_t bg);
void drawAlleyPipe(M5Canvas& canvas, uint16_t fg);

// Clutter
void drawConduitRun(M5Canvas& canvas, uint16_t fg, int x, int y, int w);
void drawCableCoil(M5Canvas& canvas, uint16_t fg, int x, int y);
void drawWallOutlet(M5Canvas& canvas, uint16_t fg, uint16_t bg, int x, int y);
void drawGraffiti(M5Canvas& canvas, uint16_t fg, uint16_t bg, int x, int y);
void drawFuseBox(M5Canvas& canvas, uint16_t fg, uint16_t bg, int x, int y);
void drawWallPoster(M5Canvas& canvas, uint16_t fg, uint16_t bg, int x, int y, int w, int h);
void drawSprinkler(M5Canvas& canvas, uint16_t fg, int x, int y);
void drawWallClock(M5Canvas& canvas, uint16_t fg, uint16_t bg, int x, int y);
void drawFloorBottle(M5Canvas& canvas, uint16_t fg, int x, int y);
void drawFireExtinguisher(M5Canvas& canvas, uint16_t fg, int x, int y);
void drawSmallVent(M5Canvas& canvas, uint16_t fg, uint16_t bg, int x, int y);
void drawTrashBag(M5Canvas& canvas, uint16_t fg, int x, int y);
void drawACUnit(M5Canvas& canvas, uint16_t fg, uint16_t bg, int x, int y);
void drawAshtray(M5Canvas& canvas, uint16_t fg, uint16_t bg, int x, int y);
void drawCeilingStain(M5Canvas& canvas, uint16_t fg, int x, int y);
void drawMenuBoard(M5Canvas& canvas, uint16_t fg, uint16_t bg, int x, int y);
void drawPatchPanel(M5Canvas& canvas, uint16_t fg, uint16_t bg, int x, int y);
void drawServiceCase(M5Canvas& canvas, uint16_t fg, uint16_t bg, int x, int y);
void drawSakeBottle(M5Canvas& canvas, uint16_t fg, int x, int y);
void drawFloorDrain(M5Canvas& canvas, uint16_t fg, uint16_t bg, int x, int y);

} // namespace MenuPigRender
