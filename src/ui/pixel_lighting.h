/**
 * pixel_lighting.h — lighting primitives for noir pixel art
 *
 * All functions stateless: canvas + position + PigLight + params.
 * Screen-blend over existing pixels (read-modify-write).
 */
#pragma once

#include <M5Unified.h>
#include "display.h"
#include "ui_measurements.h"
#include "menu_pig_render.h"
#include "piglet/avatar.h"

namespace MenuPig { struct PigPose; }

namespace PixelLight {

using namespace MenuPigRender;
using namespace UIMeasurements::MenuPigLayout;

// ==[ LIGHT POOL ]== Screen-blend glow near sources
void drawLightPool4(M5Canvas& c, uint16_t tint, int x, int y, int w, int h,
                    uint8_t density, uint32_t seed);
void drawLightPoolGradient4(M5Canvas& c, uint16_t tint, int x, int y, int w, int h,
                            uint8_t centerDensity, uint32_t seed);

// ==[ NEON WASH ]== Distance-based directed tint on surfaces
void drawNeonWash4(M5Canvas& c, const PigLightEval& light, float maxRadius,
                   float strength, uint32_t seed,
                   int clipX, int clipY, int clipW, int clipH);

// ==[ FURNITURE WASH ]== Full coverage, quadratic falloff (no Bayer skip)
void drawFurnitureWash4(M5Canvas& c, int x, int y, int w, int h,
                        const PigLightEval& light, float maxRadius, float strength);

// ==[ VOLUMETRIC DUST BEAM ]== Trapezoid shaft + drifting motes
void drawDustBeam4(M5Canvas& c, uint32_t now,
                   int topCx, int topY, int topW,
                   int botCx, int botY, int botW,
                   uint16_t shaftCol, uint16_t dustCol, uint32_t seed);

// ==[ WINDOW CAST LIGHTS ]==
void drawWindowShaft4(M5Canvas& c, int wx, int wy, int ww, int wh, uint32_t now);
void drawBlindShadows4(M5Canvas& c, int wx, int wy, int ww, int wh, uint32_t now);

// ==[ WINDOW GOD RAY ]== One passing headlight, projected through the blinds.
// The room darkens by (1 - coverage), so cells the shaft actually reaches keep
// the value they already had. The beam reads because everything around it
// dropped, not because glow was painted on top.
//
// The caller owns which source is casting and how strong it is; this only
// projects it. intensity <= 0.02 draws nothing.
struct GodRayCast {
    float intensity;    // 0..1 swell of the pass
    int16_t srcX, srcY; // headlight position, screen space
    uint16_t tint;
};
void drawWindowGodRay4(M5Canvas& c, const GodRayCast& cast,
                       int wx, int wy, int ww, int wh);

// ==[ DIRECTIONAL HALF-LAMBERT PIG NOIR ]==
struct PigNoirProfile {
    uint8_t noirDepth;   // 0-255
    uint8_t lightPen;    // 0-255
    uint8_t ambient;     // 0-255
    uint8_t tintThresh;  // 0-255
    uint8_t tintMul;     // 0-255
};

void applyPigNoirHalfLambert(M5Canvas& c, const MenuPig::PigPose& pose,
                             const PigLightEval& keyLight,
                             const PigNoirProfile& profile);

} // namespace PixelLight
