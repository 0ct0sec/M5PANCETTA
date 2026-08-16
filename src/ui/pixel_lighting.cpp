/**
 * pixel_lighting.cpp — lighting primitives implementation
 */
#include "pixel_lighting.h"
#include "pixel_primitives.h"
#include "../gfx/gfx.h"
#include "menu_pig_internal.h"
#include <math.h>

namespace PixelLight {

using namespace PixelPrim;

using namespace MenuPigRender;
using namespace UIMeasurements::MenuPigLayout;
using MenuPig::PigPose;

// ==[ ORDERED ROOM COVERAGE ]== Now in PixelPrim (pixel_primitives.h)

// ==[ CLIP RECT TO ROOM GRID ]==
static bool clipRoomGridRect(M5Canvas& c, int x, int y, int w, int h,
                             int& x0, int& y0, int& x1, int& y1) {
    if (w <= 0 || h <= 0) return false;
    x0 = q4(x); y0 = q4(y);
    x1 = x0 + ((w + 3) & ~3);
    y1 = y0 + ((h + 3) & ~3);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    int cw = c.width() & ~3;
    int ch = c.height() & ~3;
    if (x1 > cw) x1 = cw;
    if (y1 > ch) y1 = ch;
    return x0 < x1 && y0 < y1;
}

// ==[ LIGHT POOL ]==
void drawLightPool4(M5Canvas& c, uint16_t tint, int x, int y, int w, int h,
                    uint8_t density, uint32_t seed) {
    int x0, y0, x1, y1;
    if (!clipRoomGridRect(c, x, y, w, h, x0, y0, x1, y1)) return;
    uint8_t s8 = (uint8_t)((0.30f + 0.35f * Gfx::clamp01((float)density / 80.0f)) * 255.0f);
    for (int py = y0; py < y1; py += kRoomPX) {
        for (int px = x0; px < x1; px += kRoomPX) {
            if (orderedRoomCoverage(px, py, density, seed)) {
                uint16_t base = Gfx::fastReadPx(c, px, py);
                Gfx::fastFillBlock4(c, px, py, Gfx::screenBlend565(base, tint, s8));
            }
        }
    }
}

void drawLightPoolGradient4(M5Canvas& c, uint16_t tint, int x, int y, int w, int h,
                            uint8_t centerDensity, uint32_t seed) {
    x = q4(x); y = q4(y);
    w = (w + 3) & ~3; h = (h + 3) & ~3;
    if (w <= 0 || h <= 0) return;
    float cx = (float)x + (float)w * 0.5f;
    float halfW = (float)w * 0.5f;
    float baseStr = 0.30f + 0.35f * Gfx::clamp01((float)centerDensity / 80.0f);
    int x0, y0, x1, y1;
    if (!clipRoomGridRect(c, x, y, w, h, x0, y0, x1, y1)) return;
    for (int py = y0; py < y1; py += kRoomPX) {
        for (int px = x0; px < x1; px += kRoomPX) {
            float dist = fabsf((float)px + 2.0f - cx) / halfW;
            float falloff = 1.0f - dist * dist;
            uint8_t d = (uint8_t)((float)centerDensity * falloff);
            if (orderedRoomCoverage(px, py, d, seed)) {
                uint16_t base = Gfx::fastReadPx(c, px, py);
                Gfx::fastFillBlock4(c, px, py,
                    Gfx::screenBlend565(base, tint, (uint8_t)(baseStr * falloff * 255.0f)));
            }
        }
    }
}

// ==[ NEON WASH ]==
void drawNeonWash4(M5Canvas& c, const PigLightEval& light, float maxRadius,
                   float strength, uint32_t seed,
                   int clipX, int clipY, int clipW, int clipH) {
    if (!light.active) return;
    clipX = q4(clipX); clipY = q4(clipY);
    clipW = (clipW + 3) & ~3; clipH = (clipH + 3) & ~3;
    int rx0, ry0, rx1, ry1;
    if (!clipRoomGridRect(c, clipX, clipY, clipW, clipH, rx0, ry0, rx1, ry1)) return;

    float maxR2 = maxRadius * maxRadius;
    float invMaxR2 = 1.0f / maxR2;
    float str255 = strength * 255.0f;
    int iMaxR = (int)maxRadius + kRoomPX;
    int py0 = q4((light.y - iMaxR < ry0) ? ry0 : (int)light.y - iMaxR);
    int py1 = (int)light.y + iMaxR; if (py1 > ry1) py1 = ry1;
    int px0 = q4((light.x - iMaxR < rx0) ? rx0 : (int)light.x - iMaxR);
    int px1 = (int)light.x + iMaxR; if (px1 > rx1) px1 = rx1;

    for (int py = py0; py < py1; py += kRoomPX) {
        for (int px = px0; px < px1; px += kRoomPX) {
            float dx = (float)(px + 2 - light.x);
            float dy = (float)(py + 2 - light.y);
            float dist2 = dx * dx + dy * dy;
            if (dist2 >= maxR2) continue;
            float intensity = 1.0f - dist2 * invMaxR2;
            intensity *= intensity;
            uint8_t thresh = (uint8_t)(intensity * 80.0f);
            if (!orderedRoomCoverage(px, py, thresh, seed)) continue;
            uint16_t base = Gfx::fastReadPx(c, px, py);
            Gfx::fastFillBlock4(c, px, py,
                Gfx::screenBlend565(base, light.tint, (uint8_t)(str255 * intensity)));
        }
    }
}

// ==[ FURNITURE WASH ]==
void drawFurnitureWash4(M5Canvas& c, int x, int y, int w, int h,
                        const PigLightEval& light, float maxRadius, float strength) {
    if (!light.active) return;
    int x0, y0, x1, y1;
    if (!clipRoomGridRect(c, x, y, w, h, x0, y0, x1, y1)) return;
    float maxR2 = maxRadius * maxRadius;
    float invMaxR2 = 1.0f / maxR2;
    float str255 = strength * 255.0f;
    for (int py = y0; py < y1; py += kRoomPX) {
        for (int px = x0; px < x1; px += kRoomPX) {
            float dx = (float)(px + 2 - light.x);
            float dy = (float)(py + 2 - light.y);
            float dist2 = dx * dx + dy * dy;
            if (dist2 >= maxR2) continue;
            float intensity = 1.0f - dist2 * invMaxR2;
            intensity *= intensity;
            if (intensity < 0.02f) continue;
            uint16_t base = Gfx::fastReadPx(c, px, py);
            if (MenuPigRender::isNearBG(base)) continue;
            Gfx::fastFillBlock4(c, px, py,
                Gfx::screenBlend565(base, light.tint, (uint8_t)(str255 * intensity)));
        }
    }
}

// ==[ VOLUMETRIC DUST BEAM ]==
void drawDustBeam4(M5Canvas& c, uint32_t now,
                   int topCx, int topY, int topW,
                   int botCx, int botY, int botW,
                   uint16_t shaftCol, uint16_t dustCol, uint32_t seedBase) {
    if (botY <= topY) return;
    if (topW < 8) topW = 8;
    if (botW < 8) botW = 8;

    int y0 = q4(topY);
    int y1 = q4(botY);
    static constexpr uint8_t kLoopEnergy[4] = {76, 100, 116, 92};
    static constexpr int8_t kLoopDrift[4] = {-4, 0, 4, 0};
    uint8_t loopFrame = (uint8_t)((now / 420u) & 3u);
    uint8_t loopEnergy = kLoopEnergy[loopFrame];
    int loopDrift = kLoopDrift[loopFrame];
    uint32_t seed = seedBase;

    for (int py = y0; py <= y1; py += kRoomPX) {
        float t = (float)(py - y0) / (float)(y1 - y0 + 1);
        float cx = (float)topCx + (float)(botCx - topCx) * t;
        cx += (float)loopDrift * (0.25f + t * 0.75f);
        float hw = ((float)topW + (float)(botW - topW) * t) * 0.5f;
        int x0 = q4((int)floorf(cx - hw));
        int x1 = q4((int)ceilf(cx + hw));

        for (int px = x0; px <= x1; px += kRoomPX) {
            if (px < 4 || px > SCREEN_WIDTH - 4) continue;
            float edge = fabsf(((float)px + 2.0f - cx) / (hw + 0.001f));
            if (edge > 1.0f) continue;
            float core = (1.0f - edge * edge) * (0.95f - t * 0.58f);
            if (core <= 0.02f) continue;

            uint8_t d = (uint8_t)((6.0f + core * 54.0f) * (float)loopEnergy / 100.0f);
            if ((wallHash(px, py, seed + (uint32_t)(py * 3)) & 0xFFu) < d) {
                uint16_t base = Gfx::fastReadPx(c, px, py);
                Gfx::fastFillBlock4(c, px, py,
                    Gfx::screenBlend565(base, shaftCol, (uint8_t)((0.30f + core * 0.30f) * 255.0f)));
            }
        }
    }

    // Drifting motes inside beam
    for (int i = 0; i < 7; i++) {
        uint32_t cycle = 3400u + (uint32_t)i * 470u;
        float phase = (float)((now + (uint32_t)i * 997u) % cycle) / (float)cycle;
        float vt = Gfx::fastSinf(phase * 6.28318f) * 0.5f + 0.5f;
        int my = q4(y0 + (int)(vt * (float)(y1 - y0)));
        float t = (float)(my - y0) / (float)(y1 - y0 + 1);
        float cx = (float)topCx + (float)(botCx - topCx) * t;
        float hw = ((float)topW + (float)(botW - topW) * t) * 0.5f;
        float sway = Gfx::fastSinf(phase * 12.56636f + (float)i * 1.9f) * (hw * 0.62f);
        int mx = q4((int)(cx + sway));
        if (mx < 4 || mx > SCREEN_WIDTH - 4 || my <= kRoomY + 4 || my >= kFloorY - 4) continue;

        uint8_t keep = (uint8_t)(102.0f + (1.0f - t) * 88.0f);
        if ((wallHash(mx, my, seed ^ (uint32_t)(i * 71)) & 0xFFu) < keep) {
            Gfx::fastFillBlock4(c, mx, my, dustCol);
            if (((i ^ (int)(now / 120u)) & 1) == 0 && my > kRoomY + 8)
                Gfx::fastFillBlock4(c, mx, my - kRoomPX, dustCol);
        }
    }
}

// ==[ WINDOW SHAFT ]== Yellow-green cone from blinds to floor
void drawWindowShaft4(M5Canvas& c, int wx, int wy, int ww, int wh, uint32_t now) {
    int shaftTopX = wx + 10;
    int shaftTopY = wy + wh + 4;
    int shaftHeight = kFloorY - 2 - shaftTopY;
    for (int dy = 0; dy < shaftHeight; dy += kRoomPX) {
        float t = (float)dy / (float)shaftHeight;
        int width = 20 + (int)(t * 20.0f);
        int xStart = shaftTopX - (int)(t * 15.0f);
        for (int dx = 0; dx < width; dx += kRoomPX) {
            int x = xStart + dx, y = shaftTopY + dy;
            if (x < 4 || x >= SCREEN_WIDTH - 4 || y < kRoomY || y >= kFloorY) continue;
            float edgeDist = fabsf((float)dx / (float)width - 0.5f) * 2.0f;
            uint8_t thresh = (uint8_t)(35.0f * (1.0f - edgeDist));
            if (Gfx::bayer4[(y/kRoomPX) & 3][(x/kRoomPX) & 3] < thresh * 4) {
                uint16_t base = Gfx::fastReadPx(c, x, y);
                Gfx::fastFillBlock4(c, x, y, Gfx::screenBlend565f(base, RP::SHAFT, 0.45f));
            }
        }
    }
    // Volumetric dust beam
    drawDustBeam4(c, now,
                  wx + ww / 2, shaftTopY + 2, 30,
                  wx + ww / 2 + 4, kFloorY - 2, 78,
                  RP::SHAFT, RP::DUST, 0xA111u);
}

// ==[ BLIND SHADOWS ]== Noir bars sweeping across room
void drawBlindShadows4(M5Canvas& c, int wx, int wy, int ww, int wh, uint32_t now) {
    int barW = 24;
    for (int i = 0; i < 2; i++) {
        int barCenter = (int)((now / 80) % SCREEN_WIDTH) - SCREEN_WIDTH / 2 + i * (SCREEN_WIDTH / 2);
        barCenter = q4(barCenter);
        for (int y = kRoomY + 8; y < kFloorY - 4; y += kRoomPX) {
            for (int dx = -barW / 2; dx < barW / 2; dx += kRoomPX) {
                int x = barCenter + dx;
                if (x < 4 || x >= SCREEN_WIDTH - 4) continue;
                float edgeDist = fabsf((float)dx / (float)(barW / 2));
                uint8_t density = (uint8_t)(65.0f * (1.0f - edgeDist * edgeDist));
                if ((wallHash(dx, y, 0xFA11u + (uint32_t)i * 7331u) & 0xFF) < density) {
                    uint16_t base = Gfx::fastReadPx(c, x, y);
                    Gfx::fastFillBlock4(c, x, y, Gfx::lerpColor565_8(base, RP::BG, (uint8_t)(0.45f * 256)));
                }
            }
        }
    }
}

// ==[ WINDOW GOD RAY ]==
// Divergence per px of depth. This is one number doing two jobs: it widens the
// shaft as it falls AND rakes it sideways, because both come from the same
// projection of the opening away from the source. Raising it past ~0.010 walks
// the beam off a 320px screen at the extremes of a pass.
static constexpr float kGodRaySpread = 0.0072f;
static constexpr float kGodRayHalfW = 46.0f;    // half-width at the window plane
// Slat period at the sill and its growth with depth: the bands must spread
// apart as they fall or the blinds read as wallpaper instead of perspective.
static constexpr float kGodRayBandP0 = 13.0f;
static constexpr float kGodRayBandGrow = 0.016f;
static constexpr float kGodRayBandDuty = 0.34f;   // share of a period that is slat
static constexpr float kGodRayBandFeather = 0.10f; // soft band edge, in periods
static constexpr float kGodRayDepth = 0.44f;      // max darkening outside the shaft
// The rooms are authored near-black, so darkening has little headroom on the
// walls — measured against a capture, whole rows of concrete were already at the
// background plate and could not drop at all. The darkening still does real work
// on everything that ISN'T background (pig, sofa, props), but the shaft itself
// has to be carried by the light, which is why this is more than half the depth.
static constexpr float kGodRayGlow = 0.55f;       // max lift inside it

void drawWindowGodRay4(M5Canvas& c, const GodRayCast& cast,
                       int wx, int wy, int ww, int wh) {
    if (cast.intensity <= 0.02f) return;

    const int apX0 = wx + kRoomPX;              // blind aperture = inner opening
    const int apX1 = wx + ww - kRoomPX;
    const int apY = wy + wh;                    // shaft enters the room at the sill
    const float apCx = (float)(apX0 + apX1) * 0.5f;
    // Bands slide as the headlight rises or falls behind the glass, so a car on
    // a climbing lane walks the stripes down the wall instead of holding still.
    const float phase = ((float)cast.srcY - (float)(wy + wh / 2)) * 0.012f;
    const float depth = kGodRayDepth * cast.intensity;
    const float glow = kGodRayGlow * cast.intensity;

    for (int y = kRoomY; y < kFloorY; y += kRoomPX) {
        // The opening projected from the headlight. The shaft throws AWAY from
        // its source, so a car crossing to the left sweeps the beam right.
        float k = (float)(y - apY) * kGodRaySpread;
        if (k < 0.0f) k = 0.0f;
        const float cx = apCx + (apCx - (float)cast.srcX) * k;
        const float halfW = kGodRayHalfW * (1.0f + k);

        float stripe = 0.0f;
        if (y >= apY) {
            // Integral of 1/period over depth, so a period that grows linearly
            // gives bands whose spacing grows with it.
            float u = logf(1.0f + kGodRayBandGrow * (float)(y - apY)) /
                      (kGodRayBandP0 * kGodRayBandGrow) + phase;
            u -= floorf(u);
            if (u >= kGodRayBandDuty) {
                // Feather both edges. A binary stripe would only trade one hard
                // rectangle edge for two, which is what the eye actually catches.
                float rise = (u - kGodRayBandDuty) / kGodRayBandFeather;
                float fall = (1.0f - u) / kGodRayBandFeather;
                stripe = (rise < fall) ? rise : fall;
                if (stripe > 1.0f) stripe = 1.0f;
                if (stripe < 0.0f) stripe = 0.0f;
            }
        }

        for (int x = 0; x < SCREEN_WIDTH; x += kRoomPX) {
            // Never grade the view out of the window — it is the source, and
            // dimming it would darken the very car that is casting the light.
            if (x >= wx && x < wx + ww && y >= wy && y < wy + wh) continue;

            float lit = 0.0f;
            if (stripe > 0.0f) {
                float e = fabsf(((float)x + 2.0f - cx) / halfW);
                if (e < 1.0f) lit = (1.0f - e * e * e) * stripe;
            }

            // The whole effect is this line: fully lit cells get shade 0 and are
            // left exactly as they were.
            float shade = depth * (1.0f - lit);
            if (shade > 0.02f) {
                uint16_t base = Gfx::fastReadPx(c, x, y);
                // A cell already at the background plate has nothing left to
                // give up, and this pass covers the whole playfield — skipping
                // those before the blend is most of what keeps it affordable in
                // a room that is mostly dark concrete to begin with.
                if (!isNearBG(base)) {
                    uint16_t shaded = darken565(base, shade);
                    // Same brightness floor shadeRoomShadowPx enforces: never
                    // crush a lit surface down into the background.
                    if (!isNearBG(shaded)) Gfx::fastFillBlock4(c, x, y, shaded);
                }
            }
            // The lift is NOT skipped on dark cells — the beam catching an
            // unlit floor is exactly where a god ray earns its name.
            float lift = glow * lit;
            if (lift > 0.02f) MenuPig::lightRoomBeamPx(c, x, y, cast.tint, lift);
        }
    }
}

// ==[ DIRECTIONAL HALF-LAMBERT PIG NOIR ]==
void applyPigNoirHalfLambert(M5Canvas& c, const PigPose& pose,
                             const PigLightEval& keyLight,
                             const PigNoirProfile& profile) {
    if (!keyLight.active) return;

    float kNoirDepth = profile.noirDepth / 255.0f;
    float kLightPen = profile.lightPen / 255.0f;
    float kAmbient = profile.ambient / 255.0f;
    float kTintThresh = profile.tintThresh / 255.0f;
    float kTintMul = profile.tintMul / 255.0f;

    // Light direction from pig center to key light
    float pigCX = pose.drawX + kPigW * 0.5f;
    float pigCY = pose.drawY + kPigH * 0.5f;
    float ldx = keyLight.x - pigCX;
    float ldy = keyLight.y - pigCY;
    float len = sqrtf(ldx * ldx + ldy * ldy);
    if (len < 0.001f) return;
    ldx /= len; ldy /= len;

    // Pig bounds on 2px grid
    int bodyLeft = q2(pose.drawX);
    int bodyTop = q2(pose.drawY);
    int bodyRight = bodyLeft + kPigW;
    int bodyBottom = bodyTop + kPigH;

    float invHW = 1.0f / (kPigW * 0.5f);
    float invHH = 1.0f / (kPigH * 0.5f);

    for (int py = bodyTop; py < bodyBottom; py += kPigPX) {
        for (int px = bodyLeft; px < bodyRight; px += kPigPX) {
            uint16_t base = Gfx::fastReadPx(c, px, py);
            if (MenuPigRender::isNearBG(base)) continue;

            // Surface normal proxy (sphere approximation)
            float nx = ((float)px + 1.0f - pigCX) * invHW;
            float ny = ((float)py + 1.0f - pigCY) * invHH;
            float facing = nx * ldx + ny * ldy;
            float illum = Gfx::clamp01(facing * 0.5f + 0.5f); // half-Lambert
            illum = kAmbient + illum * (1.0f - kAmbient);

            float darkenAmt = kNoirDepth * (1.0f - illum * kLightPen);
            uint16_t darkened = Gfx::lerpColor565_8(base, RP::BG, (uint8_t)(darkenAmt * 255.0f));

            // Tint light-facing side with key light color
            if (illum > kTintThresh) {
                uint8_t tintAmt = (uint8_t)((illum - kTintThresh) * kTintMul);
                darkened = Gfx::screenBlend565(darkened, keyLight.tint, tintAmt);
            }
            Gfx::fastFillBlock2(c, px, py, darkened);
        }
    }
}

} // namespace PixelLight
