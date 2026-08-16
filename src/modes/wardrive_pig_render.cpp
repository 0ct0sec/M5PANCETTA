/**
 * wardrive_pig_render.cpp — rear pig rendering for WARTHOG cockpit
 *
 * Over-shoulder pig body with 2-sphere bump lighting, directional arms
 * to the butterfly yoke, rear hair (with hype-rainbow), parametric tail,
 * leg nubs, neon rim highlight, head-turn animation during tower comms,
 * and speech/thought bubble.
 *
 * ==[ drawPigRear ]== layer-5 entry point — called once per frame.
 * ==[ drawPigCommsBubble ]== layer-7 overlay — speech bubble near snout.
 *
 * All helpers (mask, shade cache, arm lighting, wheel grip) are
 * file-scoped static — not exposed in the header.
 */
#include "wardrive_pig_render.h"
#include "wardrive_shared.h"
#include "../piglet/avatar.h"
#include "../piglet/mood.h"

namespace WardriveScene {

static constexpr float PI_F = 3.14159265f;

// ═══════════════════════════════════════════════════════════════════════════
// PIG SILHOUETTE DATA
// ═══════════════════════════════════════════════════════════════════════════

static constexpr int8_t PIG_REAR_LEFT[PIG_GRID_H]  = {-1,  3,  4,  1,  1,  1,  2,  1,  1,  1};
static constexpr int8_t PIG_REAR_RIGHT[PIG_GRID_H] = {-1, 14, 13, 16, 16, 16, 15, 16, 16, 16};
static constexpr int8_t PIG_REAR_EARS[][2] = {
    {0,4},{0,5},{0,12},{0,13},{1,3},{1,4},{1,13},{1,14}
};
static constexpr int PIG_PROFILE_HEAD_ROW = 1;
static constexpr int PIG_PROFILE_EYE_COL = 4;
static_assert(PIG_PROFILE_EYE_COL >= PIG_REAR_LEFT[PIG_PROFILE_HEAD_ROW] &&
              PIG_PROFILE_EYE_COL <= PIG_REAR_RIGHT[PIG_PROFILE_HEAD_ROW],
              "WARTHOG profile eye must stay on the turned head mask");

// ═══════════════════════════════════════════════════════════════════════════
// PIG SHADE CACHE
// ═══════════════════════════════════════════════════════════════════════════

struct PigShadeCache {
    uint16_t body[PIG_GRID_H][PIG_GRID_W] = {};
    uint16_t tail = 0;
    uint16_t legs[2][2][2] = {};
    uint16_t dark = 0;
    uint16_t base = 0;
    uint16_t highlight = 0;
    uint16_t hot = 0;
    uint16_t shade = 0;
    int cell = 0;
    bool valid = false;
};

static PigShadeCache pigShadeCache;

// ═══════════════════════════════════════════════════════════════════════════
// FORWARD DECLARATIONS (file-scoped helpers)
// ═══════════════════════════════════════════════════════════════════════════

static bool pigMaskAt(int row, int col);
static uint16_t pigShadeAt(int row, int col, uint32_t seed,
                           uint16_t dark, uint16_t base, uint16_t highlight, uint16_t hot,
                           uint16_t shade, int cell);
static void refreshPigShadeCache(uint16_t dark, uint16_t base,
                                 uint16_t highlight, uint16_t hot,
                                 uint16_t shade, int cell);
static uint16_t armLitPx(uint16_t fg, uint16_t shade, uint16_t hi,
                         int px, int py, int sx, int sy, int ex, int ey);
static void drawPigWheelGrip(M5Canvas& canvas, int bodyLeft, int bodyY,
                             uint16_t fg, uint16_t shade, int pigP);

// ═══════════════════════════════════════════════════════════════════════════
// PIG MASK — build the rear pig silhouette
// ═══════════════════════════════════════════════════════════════════════════

static bool pigMaskAt(int row, int col) {
    if (row < 0 || row >= PIG_GRID_H || col < 0 || col >= PIG_GRID_W) return false;
    int8_t left = PIG_REAR_LEFT[row];
    if (left < 0) {
        for (int i = 0; i < 8; i++) {
            if (PIG_REAR_EARS[i][0] == row && PIG_REAR_EARS[i][1] == col) return true;
        }
        return false;
    }
    return col >= left && col <= PIG_REAR_RIGHT[row];
}

// ═══════════════════════════════════════════════════════════════════════════
// PIG SHADE — 2-sphere bump lighting
// ═══════════════════════════════════════════════════════════════════════════

static uint16_t pigShadeAt(int row, int col, uint32_t seed,
                           uint16_t dark, uint16_t base, uint16_t highlight, uint16_t hot,
                           uint16_t shade, int cell) {
    float cx, cy, rx, ry;
    if (row <= 2) { cx = 9.0f; cy = 1.5f; rx = 5.0f; ry = 2.0f; }
    else          { cx = 9.0f; cy = 6.0f; rx = 7.5f; ry = 3.5f; }

    float nx = (col + 0.5f - cx) / rx;
    float ny = (row + 0.5f - cy) / ry;
    float r2 = nx * nx + ny * ny;
    float nz = (r2 < 1.0f) ? sqrtf(1.0f - r2) : 0.05f;
    float mag = sqrtf(nx * nx + ny * ny + nz * nz);
    if (mag > 0.001f) { nx /= mag; ny /= mag; nz /= mag; }

    float diffuse = nx * LIGHT_X + ny * LIGHT_Y + nz * LIGHT_Z;
    if (diffuse < 0.0f) diffuse = 0.0f;
    float rimF = 1.0f - nz;
    if (rimF < 0.0f) rimF = 0.0f;
    rimF = rimF * rimF * sqrtf(rimF);
    float grain = ((float)(wallHash(col, row, seed) & 0xFFu) / 255.0f - 0.5f) * 0.15f;
    float occl = ((float)row / (float)max(1, PIG_GRID_H - 1)) * 0.10f;
    float vertFade = 1.0f - ((float)row / (float)max(1, PIG_GRID_H - 1)) * 0.20f;
    float brightness = (AMBIENT + diffuse * 0.42f + rimF * 0.10f + grain - occl) * vertFade;
    brightness = clamp01(brightness);

    uint16_t c;
    if (brightness < 0.25f)
        c = Display::lerpColor565(dark, base, brightness / 0.25f);
    else if (brightness < 0.55f)
        c = Display::lerpColor565(base, highlight, (brightness - 0.25f) / 0.30f);
    else
        c = Display::lerpColor565(highlight, hot, (brightness - 0.55f) / 0.45f);

    return bumpColor(c, col * cell, row * cell, seed, hot, shade, 0.10f);
}

// ═══════════════════════════════════════════════════════════════════════════
// REFRESH PIG SHADE CACHE — rebuilds the 16-entry shade LUT
// ═══════════════════════════════════════════════════════════════════════════

static void refreshPigShadeCache(uint16_t dark, uint16_t base,
                                 uint16_t highlight, uint16_t hot,
                                 uint16_t shade, int cell) {
    if (pigShadeCache.valid &&
        pigShadeCache.dark == dark && pigShadeCache.base == base &&
        pigShadeCache.highlight == highlight && pigShadeCache.hot == hot &&
        pigShadeCache.shade == shade && pigShadeCache.cell == cell) {
        return;
    }

    pigShadeCache.dark = dark;
    pigShadeCache.base = base;
    pigShadeCache.highlight = highlight;
    pigShadeCache.hot = hot;
    pigShadeCache.shade = shade;
    pigShadeCache.cell = cell;

    for (int row = 0; row < PIG_GRID_H; ++row) {
        for (int col = 0; col < PIG_GRID_W; ++col) {
            if (!pigMaskAt(row, col)) continue;
            pigShadeCache.body[row][col] = pigShadeAt(
                row, col, 0xC190u + row * 23 + col,
                dark, base, highlight, hot, shade, cell);
        }
    }

    pigShadeCache.tail = pigShadeAt(6, 7, 0xD00Du,
                                    dark, base, highlight, hot, shade, cell);
    for (int dr = 0; dr < 2; ++dr) {
        for (int dc = 0; dc < 2; ++dc) {
            int legRow = 10 + dr;
            pigShadeCache.legs[0][dr][dc] = pigShadeAt(
                legRow, 4 + dc, 0xAB10u + dr * 7 + dc,
                dark, base, highlight, hot, shade, cell);
            pigShadeCache.legs[1][dr][dc] = pigShadeAt(
                legRow, 13 + dc, 0xAB20u + dr * 7 + dc,
                dark, base, highlight, hot, shade, cell);
        }
    }
    pigShadeCache.valid = true;
}

// ═══════════════════════════════════════════════════════════════════════════
// ARM LIGHTING — directional light on trotter reaching for yoke
// ═══════════════════════════════════════════════════════════════════════════

static uint16_t armLitPx(uint16_t fg, uint16_t shade, uint16_t hi,
                         int px, int py, int sx, int sy, int ex, int ey) {
    // normal approximation: arm runs from shoulder to wrist, perpendicular = light direction
    float armDx = (float)(ex - sx);
    float armDy = (float)(ey - sy);
    float armLen = sqrtf(armDx * armDx + armDy * armDy);
    if (armLen < 1.0f) return fg;
    // t along arm
    float t = ((float)(px - sx) * armDx + (float)(py - sy) * armDy) / (armLen * armLen);
    float nx = -armDy / armLen;
    float ny =  armDx / armLen;
    float nz = 0.6f;
    float mag = sqrtf(nx * nx + ny * ny + nz * nz);
    nx /= mag; ny /= mag; nz /= mag;
    float diffuse = nx * LIGHT_X + ny * LIGHT_Y + nz * LIGHT_Z;
    if (diffuse < 0.0f) diffuse = 0.0f;
    float brightness = AMBIENT + diffuse * 0.42f;
    // darken near wrist end
    if (t > 0.7f) brightness *= 1.0f - (t - 0.7f) * 0.6f;
    brightness = clamp01(brightness);
    if (brightness < 0.40f)
        return Display::lerpColor565(shade, fg, brightness / 0.40f);
    return Display::lerpColor565(fg, hi, (brightness - 0.40f) / 0.60f * 0.35f);
}

// ═══════════════════════════════════════════════════════════════════════════
// WHEEL GRIP — draws the yoke grip with IK arm positioning
// ═══════════════════════════════════════════════════════════════════════════

static void drawPigWheelGrip(M5Canvas& canvas, int bodyLeft, int bodyY,
                             uint16_t fg, uint16_t shade, int pigP) {
    uint16_t armHi = Display::screenBlend565(fg, WD_NEON, (uint8_t)(0.30f * 255));

    auto drawArmRun = [&](int x0, int y0, int x1, int y1) {
        int dx = x1 - x0;
        int dy = y1 - y0;
        int steps = max(abs(dx), abs(dy)) / pigP;
        if (steps < 1) steps = 1;
        for (int i = 0; i <= steps; i++) {
            float t = (float)i / (float)steps;
            int px = q(x0 + (int)(dx * t));
            int py = q(y0 + (int)(dy * t));
            uint16_t c = armLitPx(fg, shade, armHi, px, py, x0, y0, x1, y1);
            canvas.fillRect(px, py, pigP, pigP, c);
        }
    };

    // ==[ LEFT ARM ]== shoulder -> left yoke horn
    int lShoulderX = bodyLeft - pigP;
    int upperY = bodyY + 3 * pigP + pigP / 2;
    int lowerY = bodyY + 4 * pigP;
    int lWristX = q(WHEEL_X + WHEEL_W - pigP * 4);
    int lWristY = q(WHEEL_Y + WHEEL_H * 3 / 4);

    // left shoulder pad
    for (int ry = upperY - pigP; ry < upperY + pigP * 2; ry += pigP) {
        for (int rx = lShoulderX + pigP; rx < lShoulderX + pigP * 3; rx += pigP) {
            canvas.fillRect(rx, ry, pigP, pigP,
                armLitPx(fg, shade, armHi, rx, ry, lShoulderX + pigP, upperY, lWristX, lWristY));
        }
    }
    drawArmRun(lShoulderX + pigP, upperY, lWristX, lWristY);
    drawArmRun(lShoulderX + pigP, lowerY, lWristX, lWristY + pigP);
    // left hand/hoof
    uint16_t lHandCol = armLitPx(fg, shade, armHi, lWristX, lWristY, lShoulderX, upperY, lWristX, lWristY);
    canvas.fillRect(lWristX - pigP, lWristY, pigP * 2, pigP, lHandCol);
    canvas.fillRect(lWristX - pigP * 2, lWristY + pigP, pigP, pigP,
        Display::lerpColor565(lHandCol, shade, 0.35f));

    // ==[ RIGHT ARM ]== shoulder -> right yoke horn
    int rShoulderX = bodyLeft + 16 * pigP + pigP;
    int rWristX = q(WHEEL_X + pigP * 3);
    int rWristY = q(WHEEL_Y + WHEEL_H * 3 / 4);

    // right shoulder pad
    for (int ry = upperY - pigP; ry < upperY + pigP * 2; ry += pigP) {
        for (int rx = rShoulderX - pigP * 2; rx < rShoulderX; rx += pigP) {
            canvas.fillRect(rx, ry, pigP, pigP,
                armLitPx(fg, shade, armHi, rx, ry, rShoulderX - pigP, upperY, rWristX, rWristY));
        }
    }
    drawArmRun(rShoulderX - pigP, upperY, rWristX, rWristY);
    drawArmRun(rShoulderX - pigP, lowerY, rWristX, rWristY + pigP);
    // right hand/hoof
    uint16_t rHandCol = armLitPx(fg, shade, armHi, rWristX, rWristY, rShoulderX, upperY, rWristX, rWristY);
    canvas.fillRect(rWristX, rWristY, pigP * 2, pigP, rHandCol);
    canvas.fillRect(rWristX + pigP * 2, rWristY + pigP, pigP, pigP,
        Display::lerpColor565(rHandCol, shade, 0.35f));
}

// ═══════════════════════════════════════════════════════════════════════════
// DRAW PIG REAR — main pig rendering function
// ═══════════════════════════════════════════════════════════════════════════

void drawPigRear(M5Canvas& canvas, uint32_t now, float motion, const PigPose& pose) {
    int pigP = pose.cell;
    float animT = (float)now * 0.001f;

    // pig body: theme-derived — parity with menu pig (makePigPalette)
    uint16_t bodyBase = Display::lerpColor565(RP::BG, Display::getColorFG(), 0.48f);
    uint16_t dark     = Display::lerpColor565(bodyBase, RP::BG, 0.52f);
    uint16_t shade    = Display::lerpColor565(bodyBase, RP::DEEP, 0.45f);
    uint16_t highlight= Display::screenBlend565(bodyBase, WD_NEON, (uint8_t)(0.30f * 255));
    uint16_t hot      = Display::screenBlend565(bodyBase, WD_NEON, (uint8_t)(0.45f * 255));
    uint16_t rim      = Display::lerpColor565(WD_NEON, cockpitGlassPeak(), 0.35f);

    int bx = pose.bx;
    int by = pose.by;
    int bodyLeft = pose.bodyLeft;
    refreshPigShadeCache(dark, bodyBase, highlight, hot, shade, pigP);

    // ==[ BODY ]== 18x10 grid with 2-sphere bump lighting at native P=4
    // ear micro-animation from sceneFaceTimer: twitch = spread outward, blink = droop
    int earTwitchOff = sceneFaceTimer.earTwitching ? (pigP / 2) : 0;
    int earDroopOff  = sceneFaceTimer.blinking ? (pigP / 2) : 0;
    for (int row = 0; row < PIG_GRID_H; row++) {
        for (int col = 0; col < PIG_GRID_W; col++) {
            if (!pigMaskAt(row, col)) continue;
            uint16_t c = pigShadeCache.body[row][col];
            int drawX = bodyLeft + col * pigP;
            int drawY = by + row * pigP;
            // ear cells (row 0) get twitch/droop offsets
            if (row == 0) {
                bool isLeftEar = (col <= 5);
                drawX += isLeftEar ? -earTwitchOff : earTwitchOff;
                drawY += earDroopOff;
            }
            canvas.fillRect(drawX, drawY, pigP, pigP, c);
        }
    }

    // ==[ REAR HAIR ]== 6 hairs with physics sway + hype rainbow — matching avatar.cpp
    if (!Avatar::usesFedora()) {
        // local rainbow — same algorithm as avatar.cpp rainbow565()
        auto wdRainbow565 = [](uint16_t phase) -> uint16_t {
            phase %= 1536u;
            uint8_t seg = (uint8_t)(phase >> 8);
            uint8_t t = (uint8_t)(phase & 0xFFu);
            uint8_t r = 0, g = 0, b = 0;
            switch (seg) {
                case 0: r = 255; g = t;   b = 0;   break;
                case 1: r = (uint8_t)(255 - t); g = 255; b = 0;   break;
                case 2: r = 0;   g = 255; b = t;   break;
                case 3: r = 0;   g = (uint8_t)(255 - t); b = 255; break;
                case 4: r = t;   g = 0;   b = 255; break;
                default: r = 255; g = 0;  b = (uint8_t)(255 - t); break;
            }
            return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
        };

        // trippy noise for rainbow morphing
        auto noiseHash = [](int ix, int iy) -> float {
            uint32_t n = (uint32_t)ix * 374761393u + (uint32_t)iy * 668265263u;
            n = (n ^ (n >> 13)) * 1274126177u;
            n = n ^ (n >> 16);
            return (float)(n & 0xFFFFu) / 65535.0f;
        };
        auto smoothNoise = [&noiseHash](float x, float y) -> float {
            int ix = (int)floorf(x), iy = (int)floorf(y);
            float fx = x - (float)ix, fy = y - (float)iy;
            float sx = fx * fx * (3.0f - 2.0f * fx);
            float sy = fy * fy * (3.0f - 2.0f * fy);
            float n00 = noiseHash(ix, iy), n10 = noiseHash(ix + 1, iy);
            float n01 = noiseHash(ix, iy + 1), n11 = noiseHash(ix + 1, iy + 1);
            return (n00 + (n10 - n00) * sx) + ((n01 + (n11 - n01) * sx) - (n00 + (n10 - n00) * sx)) * sy;
        };
        auto trippyColor = [&](int sx, int sy) -> uint16_t {
            float t2 = animT / 3.0f;
            float nx = (float)sx / 40.0f + t2 * 0.7f;
            float ny = (float)sy / 35.0f + t2 * 0.5f;
            float n = smoothNoise(nx, ny) * 0.6f
                    + smoothNoise(nx * 2.1f + 5.3f, ny * 2.1f + 8.7f) * 0.3f
                    + smoothNoise(nx * 4.3f + 13.1f, ny * 4.3f + 17.9f) * 0.1f;
            return wdRainbow565((uint16_t)(n * 1536.0f) % 1536u);
        };

        bool rainbow = Avatar::shouldRenderHypeHair();
        uint16_t hairFG = Avatar::getHairAccentColor(Display::getColorFG());
        // shift hair roots with head turn — hair attaches to rows 0-2 which shift left
        float hairTurn = 0.0f;
        if (towerPhase == TowerPhase::TURNING) {
            hairTurn = clamp01((float)(now - towerPhaseStart) / (float)TURN_MS);
        } else if (towerPhase == TowerPhase::TALKING) {
            hairTurn = 1.0f;
        } else if (towerPhase == TowerPhase::TURNING_BACK) {
            hairTurn = 1.0f - clamp01((float)(now - towerPhaseStart) / (float)TURN_MS);
        }
        int hairShift = (int)(hairTurn * 2.5f * pigP);
        int headCX = bx - hairShift;

        // rear-view curl shapes: 6 hairs, 4 control points each (dx, dy from root)
        static const int8_t REAR_CURLS[6][4][2] = {
            { {0,0}, {-4, -8},  {-6, -14}, {-3, -10} },
            { {0,0}, {-2, -9},  {-4, -16}, {-1, -12} },
            { {0,0}, {-1, -10}, {1,  -17}, {2,  -13} },
            { {0,0}, {1,  -10}, {-1, -17}, {-2, -13} },
            { {0,0}, {2,  -9},  {4,  -16}, {1,  -12} },
            { {0,0}, {4,  -8},  {6,  -14}, {3,  -10} },
        };
        static const int8_t ROOT_COL_OFF[6] = { -3, -2, -1, 1, 2, 3 };

        for (int i = 0; i < 6; i++) {
            int rootX = headCX + ROOT_COL_OFF[i] * pigP;
            int rootY = by + pigP;

            float period = 2.5f;
            float phase = fmodf(animT + (float)i * 0.55f, period);
            float wave = (phase < period * 0.5f)
                ? (phase - period * 0.25f) / (period * 0.25f)
                : ((period * 0.75f) - phase) / (period * 0.25f);
            float swayX = wave * pigP * 0.8f;

            float steerLean = -steerAngle * pigP * 1.5f;

            int px[4], py[4];
            for (int p = 0; p < 4; p++) {
                float dx = (float)REAR_CURLS[i][p][0] * 0.8f;
                float dy = (float)REAR_CURLS[i][p][1] * 0.8f;
                float lean = (float)p / 3.0f;
                dx += (swayX + steerLean) * lean;
                px[p] = q(rootX + (int)lroundf(dx));
                py[p] = q(rootY + (int)lroundf(dy));
            }

            for (int s = 0; s < 3; s++) {
                uint16_t segCol;
                if (rainbow) {
                    int mx = (px[s] + px[s + 1]) / 2;
                    int my = (py[s] + py[s + 1]) / 2;
                    segCol = trippyColor(mx, my);
                    segCol = Display::lerpColor565(RP::BG, segCol, 0.75f);
                } else {
                    segCol = hairFG;
                }

                int x0 = px[s], y0 = py[s], x1 = px[s + 1], y1 = py[s + 1];
                int ddx = x1 - x0, ddy = y1 - y0;
                int steps = max(1, max(abs(ddx), abs(ddy)) / pigP);
                for (int j = 0; j <= steps; j++) {
                    float jt = (float)j / (float)steps;
                    int hx = q(x0 + (int)(ddx * jt));
                    int hy = q(y0 + (int)(ddy * jt));
                    canvas.fillRect(hx, hy, pigP, pigP, segCol);
                }
            }
        }
    } else {
        Avatar::drawConfiguredHeadwear(canvas, bodyLeft, by - PX, false, true, bodyBase, RP::BG);
    }

    // ==[ ROUNDED CORNERS ]== restore bg behind pig at silhouette corners
    auto readBg = [&](int px, int py) -> uint16_t {
        return fastReadPx(canvas, clampi(px, 0, 319), clampi(py, 0, 239));
    };
    int cut = max(2, pigP / 2);
    for (int row = 1; row < PIG_GRID_H; row++) {
        for (int col = 0; col < PIG_GRID_W; col++) {
            if (!pigMaskAt(row, col)) continue;
            int cx2 = bodyLeft + col * pigP;
            int cy2 = by + row * pigP;
            bool hasTop = pigMaskAt(row - 1, col);
            bool hasBot = pigMaskAt(row + 1, col);
            bool hasL   = pigMaskAt(row, col - 1);
            bool hasR   = pigMaskAt(row, col + 1);
            if (!hasTop && !hasL) canvas.fillRect(cx2, cy2, cut, cut, readBg(cx2 - 1, cy2 - 1));
            if (!hasTop && !hasR) canvas.fillRect(cx2 + pigP - cut, cy2, cut, cut, readBg(cx2 + pigP, cy2 - 1));
            if (!hasBot && !hasL) canvas.fillRect(cx2, cy2 + pigP - cut, cut, cut, readBg(cx2 - 1, cy2 + pigP));
            if (!hasBot && !hasR) canvas.fillRect(cx2 + pigP - cut, cy2 + pigP - cut, cut, cut, readBg(cx2 + pigP, cy2 + pigP));
        }
    }

    // ==[ DEEP CORNER CLIPS ]== extended rounding at concave silhouette edges
    int edge = max(2, pigP / 2);
    for (int row = 0; row < PIG_GRID_H; row++) {
        int firstCol = -1, lastCol = -1;
        for (int c = 0; c < PIG_GRID_W; c++) {
            if (pigMaskAt(row, c)) { if (firstCol < 0) firstCol = c; lastCol = c; }
        }
        if (firstCol < 0 || lastCol - firstCol < 3) continue;
        int cyR = by + row * pigP;

        if (!pigMaskAt(row + 1, firstCol) && !pigMaskAt(row, firstCol - 1)) {
            if (pigMaskAt(row, firstCol + 1) && !pigMaskAt(row + 1, firstCol + 1)) {
                int sx = bodyLeft + firstCol * pigP;
                uint16_t bg = readBg(sx - 1, cyR + pigP);
                canvas.fillRect(sx, cyR + pigP - edge, pigP, edge, bg);
                canvas.fillRect(sx, cyR, edge, edge, bg);
                canvas.fillRect(bodyLeft + (firstCol + 1) * pigP, cyR + pigP - edge, edge, edge, bg);
            }
        }
        if (!pigMaskAt(row + 1, lastCol) && !pigMaskAt(row, lastCol + 1)) {
            if (pigMaskAt(row, lastCol - 1) && !pigMaskAt(row + 1, lastCol - 1)) {
                int sx = bodyLeft + lastCol * pigP;
                uint16_t bg = readBg(sx + pigP, cyR + pigP);
                canvas.fillRect(sx, cyR + pigP - edge, pigP, edge, bg);
                canvas.fillRect(sx + pigP - edge, cyR, edge, edge, bg);
                canvas.fillRect(bodyLeft + (lastCol - 1) * pigP + pigP - edge, cyR + pigP - edge, edge, edge, bg);
            }
        }
        if (!pigMaskAt(row - 1, firstCol) && !pigMaskAt(row, firstCol - 1)) {
            if (pigMaskAt(row, firstCol + 1) && !pigMaskAt(row - 1, firstCol + 1)) {
                int sx = bodyLeft + firstCol * pigP;
                uint16_t bg = readBg(sx - 1, cyR - 1);
                canvas.fillRect(sx, cyR, pigP, edge, bg);
                canvas.fillRect(sx, cyR + pigP - edge, edge, edge, bg);
                canvas.fillRect(bodyLeft + (firstCol + 1) * pigP, cyR, edge, edge, bg);
            }
        }
        if (!pigMaskAt(row - 1, lastCol) && !pigMaskAt(row, lastCol + 1)) {
            if (pigMaskAt(row, lastCol - 1) && !pigMaskAt(row - 1, lastCol - 1)) {
                int sx = bodyLeft + lastCol * pigP;
                uint16_t bg = readBg(sx + pigP, cyR - 1);
                canvas.fillRect(sx, cyR, pigP, edge, bg);
                canvas.fillRect(sx + pigP - edge, cyR + pigP - edge, edge, edge, bg);
                canvas.fillRect(bodyLeft + (lastCol - 1) * pigP + pigP - edge, cyR, edge, edge, bg);
            }
        }
    }

    // ==[ TAIL CURL ]== parametric spiral
    int tailCX = bodyLeft + (int)lroundf(7.5f * pigP);
    int tailCY = by + (int)lroundf(6.5f * pigP);
    float wiggle = fastSinf(animT * 2.5f) * 0.55f;
    uint16_t tailCol = pigShadeCache.tail;
    int prevTX = -999, prevTY = -999;
    for (int step = 0; step < 10; step++) {
        float t = (float)step / 9.0f;
        float angle = PI_F * (1.1f + 1.35f * t) + wiggle;
        float radius = (1.25f - 0.65f * t) * pigP;
        int sx = q(tailCX + (int)(radius * cosf(angle)));
        int sy = q(tailCY + (int)(radius * sinf(angle)));
        canvas.fillRect(sx, sy, pigP, pigP, tailCol);
        if (prevTX != -999 && (prevTX != sx || prevTY != sy))
            canvas.fillRect(prevTX, prevTY, pigP, pigP, tailCol);
        prevTX = sx; prevTY = sy;
    }

    // ==[ LEG NUBS ]== 2x2 cells with per-cell lighting
    int legY = by + 10 * pigP;
    int legLX = bodyLeft + (int)lroundf(3.5f * pigP);
    int legRX = bodyLeft + (int)lroundf(12.0f * pigP);
    for (int dr = 0; dr < 2; dr++) {
        for (int dc = 0; dc < 2; dc++) {
            uint16_t cL = pigShadeCache.legs[0][dr][dc];
            canvas.fillRect(legLX + dc * pigP, legY + dr * pigP, pigP, pigP, cL);
            uint16_t cR = pigShadeCache.legs[1][dr][dc];
            canvas.fillRect(legRX + dc * pigP, legY + dr * pigP, pigP, pigP, cR);
        }
    }

    // ==[ WHEEL GRIP ]== arm links from shoulder to wheel
    drawPigWheelGrip(canvas, bodyLeft, by, bodyBase, shade, pigP);

    // ==[ RIM HIGHLIGHTS ]== neon windshield edge light
    for (int i = 0; i < 5; i++) {
        glowPx(canvas, bodyLeft + (3 + i) * pigP, by + (1 + i) * pigP,
               rim, (uint8_t)(160 - i * 20));
    }

    // ==[ HALF-SIDE HEAD TURN ]== pig looks left toward viewer during comms
    float headTurn = 0.0f;
    if (towerPhase == TowerPhase::TURNING) {
        headTurn = clamp01((float)(now - towerPhaseStart) / (float)TURN_MS);
    } else if (towerPhase == TowerPhase::TALKING) {
        headTurn = 1.0f;
    } else if (towerPhase == TowerPhase::TURNING_BACK) {
        headTurn = 1.0f - clamp01((float)(now - towerPhaseStart) / (float)TURN_MS);
    }
    if (headTurn > 0.01f) {
        int headShift = q((int)(headTurn * 2.5f * pigP));
        int earShift = q((int)(clamp01(headTurn * 1.5f) * pigP * 1.2f));
        int earDroop = q((int)(headTurn * pigP * 0.6f));
        int earTuck  = q((int)(headTurn * pigP * 0.4f));

        for (int row = 0; row < 3; row++) {
            for (int col = 0; col < PIG_GRID_W; col++) {
                if (!pigMaskAt(row, col)) continue;
                int ox = bodyLeft + col * pigP;
                int oy = by + row * pigP;
                if (row == 0) {
                    bool isNearEar = (col <= 5);
                    ox += isNearEar ? -earTwitchOff : earTwitchOff;
                    oy += earDroopOff;
                }
                uint16_t bg = readBg(bodyLeft, oy);
                canvas.fillRect(ox, oy, pigP, pigP, bg);
            }
        }

        for (int row = 0; row < 3; row++) {
            int shift = (row == 0) ? earShift : headShift;
            if (shift <= 0) continue;
            for (int col = 0; col < PIG_GRID_W; col++) {
                if (!pigMaskAt(row, col)) continue;
                int srcX = bodyLeft + col * pigP;
                int srcY = by + row * pigP;
                int dstX = srcX - shift;
                int dstY = srcY;
                if (row == 0) {
                    bool isNearEar = (col <= 5);
                    dstX += isNearEar ? -earTwitchOff : earTwitchOff;
                    dstY += earDroopOff;
                    if (isNearEar) {
                        dstY += earDroop;
                    } else {
                        dstX += earTuck;
                    }
                }
                uint16_t c = pigShadeCache.body[row][col];
                canvas.fillRect(dstX, dstY, pigP, pigP, c);
            }
        }

        if (headTurn > 0.3f) {
            uint8_t edgeStr = (uint8_t)(clamp01((headTurn - 0.3f) * 2.5f) * 180.0f);
            for (int row = 1; row < 3; row++) {
                int leftCol = PIG_GRID_W;
                for (int c = 0; c < PIG_GRID_W; c++) {
                    if (pigMaskAt(row, c)) { leftCol = c; break; }
                }
                if (leftCol >= PIG_GRID_W) continue;
                int edgeX = bodyLeft + leftCol * pigP - headShift - pigP;
                int edgeY = by + row * pigP;
                glowPx(canvas, edgeX, edgeY, WD_NEON, edgeStr);
            }
        }

        if (headTurn > 0.5f) {
            float faceAlpha = (headTurn - 0.5f) * 2.0f;
            uint8_t faceStr = (uint8_t)(faceAlpha * 200.0f);
            int faceBaseY = by;
            // The head starts three cells inside the full body envelope. Face
            // features must anchor to that shifted mask edge, never bodyLeft.
            int headLeftX = bodyLeft + PIG_REAR_LEFT[PIG_PROFILE_HEAD_ROW] * pigP - headShift;

            uint16_t cheekCol = Display::screenBlend565(bodyBase, WD_NEON, (uint8_t)(0.22f * 255));
            glowPx(canvas, headLeftX + pigP, faceBaseY + 2 * pigP, cheekCol, (uint8_t)(faceStr * 0.7f));

            // One readable profile eye, fully inside row 1 of the turned head.
            int eyeX = bodyLeft + PIG_PROFILE_EYE_COL * pigP - headShift;
            int eyeY = faceBaseY + PIG_PROFILE_HEAD_ROW * pigP;
            uint16_t eyeWhite = accentWhite(bodyBase, 0.62f);
            if (sceneFaceTimer.blinking) {
                uint16_t lid = Display::lerpColor565(bodyBase, dark, faceAlpha);
                canvas.fillRect(eyeX, eyeY + pigP / 2, pigP, pigP / 2, lid);
            } else {
                glowPx(canvas, eyeX, eyeY, eyeWhite,
                       (uint8_t)min(255, (int)faceStr + 32));
                uint16_t pupil = Display::lerpColor565(eyeWhite, dark, faceAlpha);
                canvas.fillRect(eyeX, eyeY + pigP / 2, pigP / 2, pigP / 2, pupil);
            }

            int snoutX = headLeftX - pigP;
            uint16_t snoutCol = Display::lerpColor565(bodyBase, hot, 0.58f);
            uint16_t snoutTopCol = accentWhite(snoutCol, 0.12f);
            glowPx(canvas, snoutX, faceBaseY + 1 * pigP, snoutTopCol, (uint8_t)(faceStr * 0.85f));
            glowPx(canvas, snoutX, faceBaseY + 2 * pigP, snoutCol, (uint8_t)min(255, (int)faceStr + 18));

            glowPx(canvas, snoutX, faceBaseY + 2 * pigP, dark, (uint8_t)(faceStr * 0.5f));
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// DRAW PIG COMMS BUBBLE — renders speech/thought bubbles near the pig
// ═══════════════════════════════════════════════════════════════════════════

void drawPigCommsBubble(M5Canvas& canvas, uint32_t now, float motion, const PigPose& pose) {
    if (pigBubbleText[0] == '\0') return;

    bool active = (towerPhase == TowerPhase::TURNING);
    if (towerPhase == TowerPhase::TALKING && now - towerPhaseStart < 2500UL) active = true;
    if (!active) return;

    int pigP = pose.cell;
    int by = pose.by;
    int bodyLeft = pose.bodyLeft;
    int bodyRight = pose.bodyRight;

    float turnT = 0.0f;
    if (towerPhase == TowerPhase::TURNING) {
        turnT = clamp01((float)(now - towerPhaseStart) / (float)TURN_MS);
    } else if (towerPhase == TowerPhase::TALKING) {
        turnT = 1.0f;
    }
    int headShift = q((int)(turnT * 2.5f * pigP));

    int headLeftX = bodyLeft + PIG_REAR_LEFT[PIG_PROFILE_HEAD_ROW] * pigP - headShift;
    int noseX = headLeftX - pigP;
    int noseY = by + 2 * pigP;

    Mood::drawBubbleAt(canvas, pigBubbleText, bodyLeft - headShift, bodyRight, by, noseX, noseY);
}

} // namespace WardriveScene
