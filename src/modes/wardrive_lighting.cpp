/**
 * wardrive_lighting.cpp — cockpit lighting passes for WARTHOG
 *
 * Extracted from wardrive_scene.cpp. Do NOT modify logic — port is exact.
 */
#include "wardrive_lighting.h"
#include "wardrive_glass.h"

namespace WardriveScene {

static constexpr float PI_F = 3.14159265f;

struct ShadowCaster {
    int cx, top, left, right;  // center X, top Y, left/right edges
};

static constexpr int CASTER_COUNT = 5;

// Umbra is the caster silhouette itself; coverage ramps to zero across a
// penumbra band that widens with distance below the caster. shiftGain > 0
// projects the shadow away from a light at lightCX (0 = light overhead).
// The strongest caster wins — overlapping shadows do not compound.
static float shadowCoverage(const ShadowCaster casters[CASTER_COUNT],
                            int x, int y, float penumbraGain,
                            int lightCX, float shiftGain) {
    float best = 0.0f;
    for (int c = 0; c < CASTER_COUNT; c++) {
        float dist = (float)(y - casters[c].top) / 60.0f;
        if (dist < 0.0f) dist = 0.0f;
        int shift = (shiftGain > 0.0f)
            ? q((int)((float)(casters[c].cx - lightCX) * dist * shiftGain))
            : 0;
        int uL = casters[c].left + shift;
        int uR = casters[c].right + shift;
        if (x >= uL && x < uR) return 1.0f;   // solid umbra, nothing beats it
        int pen = (int)(dist * penumbraGain);
        if (pen < PX) continue;               // too close to cast a soft edge
        // Measure to the cell CENTRE, not its near edge. Measuring to the edge
        // makes the first cell outside the umbra read a full PX away, which
        // drops it to 1-PX/pen — a visible step down from the solid core on
        // narrow penumbras.
        int d = (x < uL) ? (uL - x - PX / 2) : (x - uR + PX / 2);
        if (d >= pen) continue;
        float cov = 1.0f - (float)d / (float)pen;
        if (cov > best) best = cov;
    }
    return best;
}

// Ordered dither on the coverage ramp. Without it the graded penumbra just
// trades one hard rectangle edge for two; with it the falloff breaks into a
// stipple that reads as soft at 320x240. Untouched at cov 0 and 1, so the
// umbra stays solid and the lit field stays clean — the mud lives in between,
// and the jitter is deliberately under a third of the range to keep it there.
static inline float ditherCoverage(float cov, int x, int y) {
    if (cov <= 0.0f || cov >= 1.0f) return cov;
    // bayer4 holds 0..240, not 0..15 — centre on 120 and scale to ±0.31.
    float jitter = ((float)bayer4[(y / PX) & 3][(x / PX) & 3] - 120.0f) *
                   (1.0f / 384.0f);
    return clamp01(cov + jitter);
}

void drawCockpitLighting(M5Canvas& canvas, const PigPose& pose) {
    // Moving pig, fixed-x chair/yoke, and consoles own separate silhouettes.
    const ShadowCaster casters[CASTER_COUNT] = {
        {pose.bx, pose.by, pose.bodyLeft, pose.bodyRight},
        {PIG_CX + CHAIR_DX, PIG_TOP_Y + CHAIR_DY + pose.bobY,
         CHAIR_SHADOW_L, CHAIR_SHADOW_R},
        {WHEEL_X + WHEEL_W / 2, WHEEL_Y, WHEEL_X, WHEEL_X + WHEEL_W},
        {92,  132,  64, 120},   // MON-B console (left)
        {190, 132, 168, 212},   // MON-C (right, on chair armrest)
    };
    bool hasLightning = thunderFlashActive();
    bool hasFlare = wdTrafficFlare > 0.04f;
    if (!hasLightning && !hasFlare) return;

    // Travel direction of the vehicle that won this frame's flare.
    const float flareDir = (wdTrafficFlareDir >= 0) ? 1.0f : -1.0f;

    for (int y = WD_DASH_T; y < WD_DASH_B; y += PX) {
        float yT = (float)(y - WD_DASH_T) / (float)(WD_DASH_B - WD_DASH_T);
        float yFade = 1.0f - yT;  // strongest at top (glass edge)

        // Per-row flare constants. The wash center rakes along the travel
        // direction as it descends the dash, so a passing vehicle sweeps a
        // diagonal across the cockpit instead of pulsing in place.
        const int flareCX = wdTrafficFlareCX + (int)(flareDir * yT * 48.0f);
        const float flareAmbient = wdTrafficFlare * 0.12f * (0.5f + yFade * 0.5f);

        for (int x = 0; x < 320; x += PX) {
            float shade = 0.0f;
            float light = 0.0f;
            uint16_t lightCol = RP::FLUOR;

            // ==[ LIGHTNING ]== white flash from above, static object shadows
            if (hasLightning) {
                // ambient darken
                shade += 0.20f * (0.5f + yFade * 0.5f);

                float cov = ditherCoverage(
                    shadowCoverage(casters, x, y, 10.0f, 0, 0.0f), x, y);
                shade += yFade * 0.15f * cov;
                float lit = yFade * 0.75f * (1.0f - cov);
                if (lit > light) light = lit;
            }

            // ==[ TRAFFIC FLARE ]== colored light from passing vehicle, moving shadows
            if (hasFlare) {
                shade += flareAmbient;

                float cov = ditherCoverage(
                    shadowCoverage(casters, x, y, 12.0f, flareCX, 0.4f), x, y);

                // Headlights throw further ahead of the vehicle than behind
                // it, so the wash is asymmetric about its own center.
                float dx = (float)(x - flareCX);
                float reach = (dx * flareDir > 0.0f) ? 190.0f : 105.0f;
                float flareProx = max(0.0f, 1.0f - fabsf(dx) / reach);
                flareProx *= flareProx;

                float flareLight = wdTrafficFlare * flareProx * yFade *
                                   (1.0f - cov);
                if (flareLight > 0.02f && flareLight > light) {
                    light = flareLight;
                    lightCol = wdTrafficFlareCol;
                }
                shade += wdTrafficFlare * flareProx * yFade * 0.10f * cov;
            }

            if (shade > 0.02f) shadePx(canvas, x, y, min(0.45f, shade));
            if (light > 0.02f) glowPx(canvas, x, y, lightCol, (uint8_t)clampi((int)(light * 180.0f), 0, 180));
        }
    }
}

void drawWindshieldCurvature(M5Canvas& canvas, uint32_t now, float motion) {
    uint16_t glassPeak = cockpitGlassPeak();
    uint16_t coolArc = Display::lerpColor565(WD_NEON, glassPeak, 0.24f);
    uint16_t warmArc = Display::lerpColor565(WD_AMBER, glassPeak, 0.30f);
    uint16_t crownCol = Display::lerpColor565(RP::SHAFT, glassPeak, 0.26f);
    uint16_t sillCol = Display::lerpColor565(WD_AMBER, WD_NEON, 0.40f);
    float drift = (float)now * 0.0012f;
    int openTop = glassOpenTop();

    for (int y = openTop + PX; y < WD_GLASS_B; y += PX) {
        int left, right;
        glassBounds(y, left, right);
        int span = right - left;
        if (span <= PX * 6) continue;

        float t = glassRowTAtY(y);
        int edgeBands = 2 + ((t > 0.48f) ? 1 : 0);
        for (int e = 0; e < edgeBands; e++) {
            float edgeDark = 0.06f + t * 0.03f + (float)e * 0.04f;
            shadePx(canvas, left + e * PX, y, edgeDark);
            shadePx(canvas, right - (e + 1) * PX, y, edgeDark);
        }

        if (y < openTop + PX * 4) {
            for (int x = left + PX * 3; x < right - PX * 3; x += PX * 4) {
                shadePx(canvas, x, y, 0.06f + (1.0f - t) * 0.05f);
            }
        }

        int leftArcX = q(left + lerpi(0, span, 0.18f + 0.08f * t)
                         + (int)(fastSinf(t * PI_F * 1.15f + drift) * PX * 2.0f));
        int rightArcX = q(left + lerpi(0, span, 0.79f - 0.06f * t)
                          + (int)(fastSinf(t * PI_F * 1.05f + drift + 1.3f) * PX * 2.0f));
        int crownX = q(glassCenterX(y) + (int)(steerAngle * (10.0f - t * 8.0f)));
        bool drawLeftArc = (((y / PX) + (int)(now / 180UL)) & 3) <= 1;
        bool drawRightArc = (((y / PX) + (int)(now / 220UL) + 1) & 7) <= 1;

        float leftA = clamp01(1.0f - fabsf(t - 0.42f) * 1.8f);
        float rightA = clamp01(1.0f - fabsf(t - 0.56f) * 1.8f);
        float crownA = clamp01(1.0f - t * 2.8f);
        if (((y / PX) & 1) != 0 && drawLeftArc) {
            glowPx(canvas, leftArcX, y, coolArc, (uint8_t)(18 + leftA * 46.0f));
            if (leftArcX + PX < right - PX * 4) {
                glowPx(canvas, leftArcX + PX, y, coolArc, (uint8_t)(10 + leftA * 22.0f));
            }
        }
        if (((y / PX) & 1) != 0 && drawRightArc) {
            glowPx(canvas, rightArcX, y, warmArc, (uint8_t)(14 + rightA * 34.0f));
            if (rightArcX - PX > left + PX * 4) {
                glowPx(canvas, rightArcX - PX, y, warmArc, (uint8_t)(8 + rightA * 18.0f));
            }
        }
        if (t < 0.34f && ((y / PX) & 3) != 0) {
            glowPx(canvas, crownX, y, crownCol, (uint8_t)(16 + crownA * 62.0f));
            if (motion > 0.10f) glowPx(canvas, crownX + PX, y, crownCol, (uint8_t)(12 + crownA * 28.0f));
        }
        if (y >= WD_GLASS_B - PX * 3) {
            int baseX = q(glassCenterX(y) + steerAngle * 12.0f);
            glowPx(canvas, baseX - PX, y, sillCol, 34);
            glowPx(canvas, baseX + PX, y, sillCol, 42);
        }
    }
}

// Source reflections on the canopy; steer shifts their angle across the glass.
void drawCanopyStreaks(M5Canvas& canvas, uint32_t now, float motion) {
    uint32_t stripeStep = max((uint32_t)50u, (uint32_t)(200.0f - motion * 100.0f));
    uint16_t glassPeak = cockpitGlassPeak();
    uint16_t warmWash = Display::lerpColor565(WD_AMBER, glassPeak, 0.18f);
    uint16_t coolWash = Display::lerpColor565(WD_NEON, glassPeak, 0.38f);
    // Symmetric rounding keeps the bounded auto-pilot envelope visible on the
    // 4px cockpit grid. The old q(int(...)) erased small right turns and made
    // equally small left turns jump one cell.
    int steerOff = clampi((int)lroundf(steerAngle * 2.0f), -2, 2) * PX;

    for (uint8_t i = 0; i < 4; i++) {
        int startY = q(40 + i * (PX * 2 + 6));
        int left, right;
        glassBounds(startY, left, right);
        int travel = (right - left) + PX * 18;
        uint32_t speedMul = 1u + (i & 1u);
        // Preserve the old one-pixel-per-stripeStep cadence. Q8 interpolation
        // smooths that motion; it must not multiply its speed by the 4px grid.
        uint32_t cycleMs = max(1u,
            (uint32_t)max(PX * 12, travel) * stripeStep / speedMul);
        uint32_t local = (now + (uint32_t)i * stripeStep * 53u / speedMul) % cycleMs;
        int travelQ8 = (int)((uint64_t)local *
                             (uint32_t)max(PX * 12, travel) * 256u /
                             cycleMs);
        int startXQ8 = (right + PX * 4 + steerOff) * 256 - travelQ8;
        uint16_t tint = (i & 1u) ? coolWash : warmWash;
        // Q8 per-segment offset lets modest IMU/autopilot turns accumulate
        // into one readable cell without abandoning the gravity-first streak.
        int dyPerSegQ8 = (int)lroundf(steerAngle * 0.55f *
                                      (float)(PX * 256));
        for (int s = 0; s < 6; s++) {
            int sxQ8 = startXQ8 - s * PX * 2 * 256;
            int sx = q8GridCell(sxQ8);
            int frac = q8GridFraction255(sxQ8, sx);
            int sy = startY + s * PX +
                     q8RoundedGridDelta(s * dyPerSegQ8);
            uint8_t alpha = (uint8_t)(110 - s * 18);
            if (s == 0) {
                if (insideGlass(sx, sy))
                    glowPx(canvas, sx, sy, tint,
                           (uint8_t)((int)alpha * (255 - frac) / 255));
                if (insideGlass(sx + PX, sy))
                    glowPx(canvas, sx + PX, sy, tint,
                           (uint8_t)((int)alpha * frac / 255));
            } else {
                int roundedX = sx + (frac >= 128 ? PX : 0);
                if (insideGlass(roundedX, sy))
                    glowPx(canvas, roundedX, sy, tint, alpha);
            }
        }
    }
}

} // namespace WardriveScene
