/**
 * wardrive_weather.cpp — canopy rain/weather rendering for WARTHOG cockpit
 *
 * Exterior falling rain is drawn behind cockpit structure and occupants.
 * Foreground glass water is limited to seal/interior beads, watercut
 * rivulets, and frame-contour runoff drawn before curvature/HUD.
 *
 * Dependencies (via wardrive_shared.h):
 *   - Glass geometry: WD_GLASS_T/B, glassBounds(), insideGlass(), glassOpenTop()
 *   - IMU state: imuPitch, imuRoll, steerAngle
 *   - Thunder: thunderFlashActive()
 *   - Colors: RP::PUDDLE, RP::SHAFT, WD_NEON
 *   - Utilities: wallHash(), glowPx(), accentWhite(), floorDivPositive(), q()
 */

#include "wardrive_weather.h"
#include "wardrive_glass.h"

namespace WardriveScene {

static constexpr int WATER_RIVULET_SEGMENTS = 8;
static constexpr int FRAME_RIVULET_SEGMENTS = 6;

// Hard ceilings for the intensity-scaled populations. Both passes are pure
// loops over seeded elements, so the only cost of a higher ceiling is draw
// calls — but the glass still has to read as rain, not static.
static constexpr int EXTERIOR_STREAK_MAX = 20;
static constexpr int BEAD_BASE  = 16;
static constexpr int BEAD_MAX   = 24;
static constexpr int RIVULET_MAX = 8;

struct CanopyFlow {
    float motionBoost;
    float lateralFlow;
    int lateralShift;
    float storm;        // slow post-thunder downpour surge, 0..1
    bool flash;
};

// Fractional element counts fade their marginal element in rather than popping
// it into existence when the threshold is crossed. Returns the loop bound and
// writes the alpha scale that the last index should use.
static int fadedCount(int base, float extra, int cap, float& lastScale) {
    if (extra <= 0.0f) { lastScale = 1.0f; return base; }
    int whole = (int)extra;
    float frac = extra - (float)whole;
    int count = base + whole + (frac > 0.0f ? 1 : 0);
    // Only a count PAST the cap loses its fade — clamping at exactly the cap
    // must keep the fraction, or the last element pops in at full alpha.
    if (count > cap) { lastScale = 1.0f; return cap; }
    lastScale = (frac > 0.0f) ? frac : 1.0f;
    return count;
}

static uint32_t motionCycleMs(uint32_t slowMs, uint32_t fastMs,
                              float motion) {
    motion = Gfx::clampf(motion, 0.0f, 1.0f);
    return slowMs - (uint32_t)((float)(slowMs - fastMs) * motion);
}

static int signedFlowOffset(float flow) {
    // Round symmetrically. floorDivPositive() made small positive turns vanish
    // while equally small negative turns jumped a whole cockpit cell.
    return clampi((int)lroundf(flow * 1.6f), -2, 2) * PX;
}

static CanopyFlow sampleCanopyFlow(float motion) {
    float motionBoost = motion + imuPitch * 0.35f;
    if (motionBoost < 0.0f) motionBoost = 0.0f;
    if (motionBoost > 1.0f) motionBoost = 1.0f;
    float lateralFlow = imuRoll * 1.2f + steerAngle * 0.3f;
    return {motionBoost, lateralFlow, signedFlowOffset(lateralFlow),
            Gfx::clamp01(wdStormIntensity), thunderFlashActive()};
}

// L2 exterior precipitation. The canopy polygon clips the outside world; the
// later cockpit passes provide the opaque interior occlusion mask.
void drawExteriorRainMotion(M5Canvas& canvas, uint32_t now, float motion) {
    const CanopyFlow flow = sampleCanopyFlow(motion);
    const float motionBoost = flow.motionBoost;
    const float lateralFlow = flow.lateralFlow;

    uint16_t streakBody = Display::lerpColor565(RP::SHAFT, WD_NEON, 0.14f);
    uint16_t streakHead = accentWhite(streakBody, flow.flash ? 0.48f : 0.22f);

    auto drawRainCell = [&](int x, int y, uint16_t color, uint8_t alpha) {
        if (alpha < 8 || y < WD_GLASS_T || y >= WD_GLASS_B) return;
        int sx = snapRainXToPane(x, y);
        if (insideGlass(sx, y)) glowPx(canvas, sx, y, color, alpha);
    };

    const int travelH = WD_GLASS_B - WD_GLASS_T + PX * 10;
    // Speed and storm both thicken the field. The marginal streak fades in so
    // crossing a threshold never pops a full-brightness streak onto the glass.
    float streakLastScale = 1.0f;
    int streakCount = fadedCount(6, motionBoost * 8.0f + flow.storm * 6.0f,
                                 EXTERIOR_STREAK_MAX, streakLastScale);
    // Graded lean: the tail slope tracks lateral flow continuously instead of
    // snapping to ±PX by its sign, so a hard roll actually reads as a hard
    // slant. Q8-accumulated across segments to survive the 4px grid.
    float leanUnit = Gfx::clampf(lateralFlow, -1.0f, 1.0f);
    const int leanQ8 = (int)lroundf(leanUnit * 0.85f * (float)(PX * 256));
    for (int i = 0; i < streakCount; ++i) {
        uint32_t seed = wallHash(i, 0x5D, 0x1C91u);
        uint32_t cycle = (uint32_t)(1850.0f - motionBoost * 620.0f) +
                         ((seed >> 8) % 540u);
        uint32_t local = (now + (seed & 0x0FFFu)) % cycle;
        int yQ8 = (WD_GLASS_T - PX * 4) * 256 +
                  (int)(local * (uint32_t)travelH * 256u / cycle);
        int cellY = q8GridCell(yQ8);
        int frac = q8GridFraction255(yQ8, cellY);
        int laneX = floorDivPositive((int)((seed >> 13) % 344u) - 12 +
                                     flow.lateralShift, PX) * PX;
        // Below the flow dead zone the drop keeps a seeded ±1/8-cell wander so
        // a dead-level canopy still shows individual streak character.
        int streakLeanQ8 = (fabsf(leanUnit) > 0.15f)
            ? leanQ8
            : ((seed & 1u) ? (PX * 256) / 8 : -(PX * 256) / 8);
        int lifeScale = (i == streakCount - 1)
            ? clampi((int)(streakLastScale * 255.0f), 0, 255) : 255;

        uint8_t headAlpha = (uint8_t)min(
            238, 140 + (int)(motionBoost * 72.0f) +
                 (int)(flow.storm * 30.0f) +
                 (flow.flash ? 42 : 0));
        int headLife = (int)headAlpha * lifeScale / 255;
        drawRainCell(laneX, cellY, streakHead,
                     (uint8_t)(headLife * (255 - frac) / 255));
        drawRainCell(laneX, cellY + PX, streakHead,
                     (uint8_t)(headLife * frac / 255));

        int tailLen = 3 + (int)(motionBoost * 3.0f + flow.storm * 2.0f);
        for (int s = 1; s <= tailLen; ++s) {
            int sy = cellY - s * PX;
            int sx = laneX - q8RoundedGridDelta(s * streakLeanQ8);
            int alpha = 92 + (int)(motionBoost * 28.0f) - s * 16;
            alpha = alpha * lifeScale / 255;
            if (alpha > 12) drawRainCell(sx, sy, streakBody, (uint8_t)alpha);
        }
    }
}

// L6 adhered canopy water — drawn after occupants/teleport, before
// curvature/HUD. Falling streaks are deliberately excluded from this pass.
void drawCanopySurfaceWater(M5Canvas& canvas, uint32_t now, float motion) {
    const CanopyFlow flow = sampleCanopyFlow(motion);
    const float motionBoost = flow.motionBoost;
    const float lateralFlow = flow.lateralFlow;
    const int lateralShift = flow.lateralShift;
    const bool flash = flow.flash;

    uint16_t beadBody = Display::lerpColor565(RP::PUDDLE, RP::SHAFT, 0.46f);
    uint16_t beadHead = accentWhite(beadBody, flash ? 0.42f : 0.18f);
    // Rivulet body: neon smear on wet glass (Cloudpunk technique)
    uint16_t rivBody = Display::lerpColor565(RP::PUDDLE, WD_NEON, 0.22f);
    uint16_t rivHead = accentWhite(rivBody, flash ? 0.44f : 0.16f);

    auto drawDropCell = [&](int x, int y, uint16_t color, uint8_t alpha) {
        if (alpha < 8 || y < WD_GLASS_T || y >= WD_GLASS_B) return;
        int sx = snapRainXToPane(x, y);
        if (insideGlass(sx, y)) glowPx(canvas, sx, y, color, alpha);
    };

    auto beadXAtRow = [&](uint32_t seed, int index, int y) {
        int left, right;
        glassBounds(clampi(y, WD_GLASS_T, WD_GLASS_B - PX), left, right);
        if (index < 6) {
            int sealInset = PX + (int)((seed >> 6) & 1u) * PX;
            return (index & 1) ? right - PX - sealInset : left + sealInset;
        }
        int span = max(PX, right - left - PX * 3);
        int lane255 = (int)((seed >> 5) & 0xFFu);
        return left + PX + span * lane255 / 255;
    };

    // Six seal beads plus ten interior beads. Their masks never reroll; each
    // bead holds under surface tension, then eases through a short gravity
    // creep. Adjacent-cell alpha keeps the creep smooth on the 4px grid.
    // Motion speeds the hold→creep transition so beads release faster at speed.
    // Six seal beads and ten interior beads are the still-weather baseline;
    // speed and storm add up to eight more. Indices are stable, so extra beads
    // only ever append — every existing bead keeps its mask and its lane.
    float beadLastScale = 1.0f;
    const int beadCount = fadedCount(
        BEAD_BASE, motionBoost * 4.0f + flow.storm * 4.0f, BEAD_MAX,
        beadLastScale);
    const int beadTop = glassOpenTop();
    const int beadBaseSpan = WD_GLASS_B - beadTop - PX * 6;
    for (int i = 0; i < beadCount; ++i) {
        uint32_t seed = wallHash(i, 0x71, 0x7D31u);
        uint32_t cycle = 5200u + ((seed >> 8) & 0x0FFFu);
        uint32_t local = (now + (seed & 0x1FFFu)) % cycle;
        uint32_t hold = cycle * (68u + ((seed >> 4) & 0x0Fu)) / 100u;
        // Motion accelerates the hold→creep threshold
        hold = (uint32_t)((float)hold * (1.0f - motionBoost * 0.30f));
        int beadYQ8 = (beadTop + PX +
                       (int)((seed >> 12) % (uint32_t)max(PX, beadBaseSpan))) * 256;
        if (local > hold) {
            uint32_t tQ8 = (local - hold) * 255u / max(1u, cycle - hold);
            int creepSpan = PX + (int)((seed >> 20) % (uint32_t)(PX * 4));
            beadYQ8 += (int)((uint32_t)creepSpan * 256u * tQ8 * tQ8 /
                             (255u * 255u));
        }

        int cellY = q8GridCell(beadYQ8);
        int frac = q8GridFraction255(beadYQ8, cellY);
        int baseAlpha = (i < 6 ? 72 : 44) + (flash ? 54 : 0);
        // Fade through the cycle seam so a released bead can reform at its
        // stable anchor without teleporting upward while still visible.
        uint32_t fadeInEnd = cycle / 10u;
        uint32_t fadeOutStart = cycle * 88u / 100u;
        uint8_t lifeAlpha = 255;
        if (local < fadeInEnd) {
            lifeAlpha = (uint8_t)(local * 255u / max(1u, fadeInEnd));
        } else if (local > fadeOutStart) {
            lifeAlpha = (uint8_t)((cycle - local) * 255u /
                                  max(1u, cycle - fadeOutStart));
        }
        baseAlpha = baseAlpha * lifeAlpha / 255;
        if (i == beadCount - 1) {
            baseAlpha = baseAlpha * clampi((int)(beadLastScale * 255.0f), 0, 255) / 255;
        }
        uint16_t color = ((seed >> 3) & 0x07u) == 0u ? beadHead : beadBody;
        drawDropCell(beadXAtRow(seed, i, cellY) + lateralShift, cellY, color,
                     (uint8_t)(baseAlpha * (255 - frac) / 255));
        drawDropCell(beadXAtRow(seed, i, cellY + PX) + lateralShift,
                     cellY + PX, color,
                     (uint8_t)(baseAlpha * frac / 255));
    }

    // Rivulet traversal keeps the former full-pane overscan so the complete
    // tail leaves the glass before reforming at its stable source.
    const int travelH = WD_GLASS_B - WD_GLASS_T + PX * 10;
    // Watercut rivulets — gravity-dominant chains around vertical with gentle
    // steer nudge. The bright head leads a dimmer tail up the pane. Count is
    // hard-bounded; pitch already contributes through motionBoost.
    float rivLastScale = 1.0f;
    int rivCount = fadedCount(3, motionBoost * 3.0f + flow.storm * 3.0f,
                              RIVULET_MAX, rivLastScale);
    for (int i = 0; i < rivCount; ++i) {
        int rivLife = (i == rivCount - 1)
            ? clampi((int)(rivLastScale * 255.0f), 0, 255) : 255;
        uint32_t seed = wallHash(i, 0xA3, 0x4C2Eu);
        int rx = PX * 2 + (int)((seed & 0xFFu) % 280u);
        rx = floorDivPositive(rx + lateralShift, PX) * PX;
        float seededDrift =
            (((seed >> 8) & 0xFFu) / 255.0f - 0.5f) * 0.16f;
        float flowDrift = Gfx::clampf(lateralFlow, -1.0f, 1.0f) * 0.55f;
        int driftQ8 = (int)lroundf((seededDrift + flowDrift) *
                                    (float)(PX * 256));
        const int rivTravel = travelH + PX * 4;
        // Preserve the pre-smoothing traversal envelope. The former
        // per-cell duration stretched this to 1.7-3.8 s and hid navigation
        // response; Q8 head interpolation still provides the smoothness.
        uint32_t cycle = motionCycleMs(800u, 350u, motionBoost);
        uint32_t local = (now + (seed >> 16 & 0xFFFu)) % cycle;
        int headYQ8 = (WD_GLASS_T - PX * 2) * 256 +
                      (int)((uint64_t)rivTravel * local * 256u / cycle);
        int headCellY = q8GridCell(headYQ8);
        int headFrac = q8GridFraction255(headYQ8, headCellY);
        const int headAlpha = 88 + (int)(motionBoost * 48.0f) +
                              (flash ? 30 : 0);
        auto drawRivCell = [&](int px, int py, uint16_t col, int alpha) {
            alpha = alpha * rivLife / 255;
            if (alpha < 10 || py < WD_GLASS_T || py >= WD_GLASS_B) return;
            int sx = snapRainXToPane(px, py);
            if (insideGlass(sx, py)) glowPx(canvas, sx, py, col, (uint8_t)alpha);
        };
        drawRivCell(rx, headCellY, rivHead,
                    headAlpha * (255 - headFrac) / 255);
        drawRivCell(rx, headCellY + PX, rivHead,
                    headAlpha * headFrac / 255);

        // Only the head crossfades. The tail rounds as one stable cell chain,
        // avoiding a doubled translucent ribbon at 60 fps.
        int roundedHeadY = headCellY + (headFrac >= 128 ? PX : 0);
        for (int s = 1; s < WATER_RIVULET_SEGMENTS; ++s) {
            int px = rx + q8RoundedGridDelta(driftQ8 * s);
            int py = roundedHeadY - s * PX;
            if (py >= WD_GLASS_B) continue;
            if (py < WD_GLASS_T) break;
            int alpha = 108 - s * 11 + (flash ? 30 : 0);
            drawRivCell(px, py, rivBody, alpha);
        }
    }

    // Frame-contour rivulets — water runoff tracing the canopy frame edges.
    // Two per side, with a leading head and an edge-hugging tail. The travel
    // includes the full tail so the chain exits before it reforms at the seal.
    for (int side = 0; side < 2; ++side) {
        for (int r = 0; r < 2; ++r) {
            uint32_t seed = wallHash(side * 2 + r, 0xF7, 0x9B1Au);
            int startRow = (int)((seed >> 8) & 0x1Fu) % 6;
            int startYSnap = glassOpenTop() + startRow * PX;
            int travel = WD_GLASS_B - startYSnap + FRAME_RIVULET_SEGMENTS * PX;
            uint32_t cycle = motionCycleMs(720u, 320u, motionBoost);
            uint32_t local = (now + (seed & 0x0FFFu)) % cycle;
            int headYQ8 = (startYSnap - PX) * 256 +
                          (int)((uint64_t)travel * local * 256u / cycle);
            int headCellY = q8GridCell(headYQ8);
            int headFrac = q8GridFraction255(headYQ8, headCellY);
            int inward = (side == 0) ? PX : -PX;

            auto drawFrameCell = [&](int segment, int py, uint16_t col, int alpha) {
                if (alpha < 10 || py < glassOpenTop() || py >= WD_GLASS_B) return;
                int left, right;
                glassBounds(py, left, right);
                int edgeX = (side == 0) ? left + PX : right - PX * 2;
                // The leading head has crept furthest inward; the older tail
                // remains attached closer to the canopy seal.
                int inwardSteps = (FRAME_RIVULET_SEGMENTS - 1 - segment) / 2;
                int px = edgeX + inwardSteps * inward;
                px = floorDivPositive(px, PX) * PX;
                if (insideGlass(px, py)) glowPx(canvas, px, py, col, (uint8_t)alpha);
            };

            const int headAlpha = 96 + (flash ? 24 : 0);
            drawFrameCell(0, headCellY, rivHead,
                          headAlpha * (255 - headFrac) / 255);
            drawFrameCell(0, headCellY + PX, rivHead,
                          headAlpha * headFrac / 255);

            int roundedHeadY = headCellY + (headFrac >= 128 ? PX : 0);
            for (int s = 1; s < FRAME_RIVULET_SEGMENTS; ++s) {
                int py = roundedHeadY - s * PX;
                if (py >= WD_GLASS_B) continue;
                if (py < glassOpenTop()) break;
                drawFrameCell(s, py, rivBody,
                              88 - s * 10 + (flash ? 24 : 0));
            }
        }
    }
}

} // namespace WardriveScene
