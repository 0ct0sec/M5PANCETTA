/**
 * pixel_furniture.cpp — furniture primitives for pixel art rooms
 *
 * Each function draws a complete furniture piece at grid position.
 * Stateless: canvas + position + animation time.
 * All coords snapped to kRoomPX=4.
 */
#include "pixel_furniture.h"
#include "pixel_primitives.h"
#include "pixel_materials.h"
#include "pixel_lighting.h"
#include "pixel_weather.h"
#include "menu_pig_internal.h"

namespace PixelFurn {

using namespace PixelPrim;
using namespace PixelMat;
using namespace PixelLight;
using namespace PixelWeather;
using namespace MenuPigRender;
using MenuPig::isNeonOn;
using MenuPig::isRoom0SignLit;
using MenuPig::podLedOn;
using MenuPig::roomMood;

// ─────────────────────────────────────────────────────────────────────────
// STATIC HELPERS (forward-defined before first use)
// ─────────────────────────────────────────────────────────────────────────

static void drawFlatMonitor4(M5Canvas& c, int mx, int my, int mw, int mh,
                             bool screenOn, uint32_t now) {
    fillRect4(c, mx, my, mw, mh, RP::D_STRUCT);
    int sx = mx + 4, sy = my + 4, sw = mw - 8, sh = mh - 8;
    fillRect4(c, sx, sy, sw, sh, RP::BG);
    if (screenOn) {
        uint32_t linePhase = now / 500 + (uint32_t)(mx * 71);
        for (int ly = sy + 4; ly + 4 <= sy + sh - 4; ly += 8) {
            uint32_t h = wallHash(mx, ly, linePhase);
            int lineW = 4 + (int)(h % (uint32_t)(sw - 8));
            if (lineW > sw - 4) lineW = sw - 4;
            fillRect4(c, sx + 4, ly, lineW, 4, RP::CRT);
        }
        if (((now / 700) & 1) != 0)
            fillBlock4(c, sx + sw - 8, sy + sh - 8, RP::CRT);
    }
    fillRect4(c, mx + 4, my + mh - kRoomPX, mw - 8, kRoomPX, RP::SHADOW_C);
    int standX = mx + mw / 2 - 4;
    fillRect4(c, standX, my + mh, 8, kRoomPX, RP::D_STRUCT);
    fillRect4(c, standX - 4, my + mh + kRoomPX, 16, kRoomPX, RP::D_STRUCT);
}

static void drawDeadLedModule4(M5Canvas& c, int px, int py, uint32_t salt) {
    uint16_t moduleColor = ((wallHash(px, py, salt) & 0x03u) == 0u)
                           ? RP::D_STRUCT : RP::D_FILL;
    fillRect4(c, px, py, kRoomPX, kRoomPX, moduleColor);
}

static void drawRamenVapor4(M5Canvas& c, uint32_t now, int baseX, int baseY) {
    for (int i = 0; i < 5; i++) {
        uint32_t phase = (now + (uint32_t)i * 640u) % 3200u;
        float t = (float)phase / 3200.0f;
        int rise = (int)(t * 32.0f);
        float wave = fastSinf(t * 6.28f + (float)i * 1.5f) * 3.0f;
        int sx = (baseX + (int)wave + (i - 2) * 4) & ~3;
        int sy = (baseY - rise) & ~3;
        if (sy <= (int)UIMeasurements::kTopBarH + 2 || sy >= kFloorY) continue;
        float fade = (t < 0.6f) ? 1.0f : (1.0f - (t - 0.6f) / 0.4f);
        if ((wallHash(sx, sy, 0xBE11 + i * 53) & 0xFF) < (uint8_t)(200.0f * fade))
            fillBlock4(c, sx, sy, (t < 0.4f) ? RP::SOFT : RP::DUST);
    }
}

static void drawDeadPenModule4(M5Canvas& c, int px, int py) {
    uint16_t moduleColor = ((wallHash(px, py, 0x51A7u) & 0x03u) == 0u)
                           ? RP::WALL_MID : RP::FILL;
    fillRect4(c, px, py, kRoomPX, kRoomPX, moduleColor);
}

// ─────────────────────────────────────────────────────────────────────────
// ROOM 0: CYBERDECK LAB
// ─────────────────────────────────────────────────────────────────────────

void drawDualMonitorDesk4(M5Canvas& c, int x, int y, bool screenOn, uint32_t now) {
    const int dx = x - kR1_DeskX;
    const int dy = y - kR1_DeskY;
    // Desk surface
    fillRect4(c, x, y, kR1_DeskW, kRoomPX, RP::D_STRUCT);
    fillRect4(c, x, y + kRoomPX, kR1_DeskW, kRoomPX, RP::FILL);
    // Cup ring stain
    fillBlock4(c, x + 20, y + kRoomPX, RP::SHADOW_C);
    // Desk legs
    fillRect4(c, x, y + 2 * kRoomPX, kRoomPX, kFloorY - y - 2 * kRoomPX, RP::D_STRUCT);
    fillRect4(c, x + kR1_DeskW - kRoomPX, y + 2 * kRoomPX, kRoomPX, kFloorY - y - 2 * kRoomPX, RP::D_STRUCT);
    // Monitor A
    drawFlatMonitor4(c, kR1_MonAX + dx, kR1_MonAY + dy,
                     kR1_MonAW, kR1_MonAH, screenOn, now);
    // Monitor B
    drawFlatMonitor4(c, kR1_MonBX + dx, kR1_MonBY + dy,
                     kR1_MonBW, kR1_MonBH, screenOn, now);
    // Keyboard
    int kx = kR1_KeybX + dx, ky = kR1_KeybY + dy, kw = kR1_KeybW;
    fillRect4(c, kx, ky, kw, kRoomPX, RP::D_STRUCT);
    for (int col = 0; col < 12; col++) {
        int px = kx + 4 + col * kRoomPX;
        if (px + kRoomPX <= kx + kw - 4)
            fillBlock4(c, px, ky + 4, RP::BG);
    }
    // Worn WASD keys
    fillBlock4(c, kx + 12, ky + 4, RP::FILL);
    fillBlock4(c, kx + 20, ky + 4, RP::FILL);
    fillRect4(c, kx, ky + kR1_KeybH, kw, kRoomPX, RP::SHADOW_C);
    // Desk lamp
    drawDeskLamp4(c, kR1_LampX + dx, kR1_LampY + dy, now);
}

void drawServerRack4(M5Canvas& c, int x, int y, uint32_t now) {
    const int unitH = (kR1_SrvH / 3) & ~3;
    fillRect4(c, x, y, kR1_SrvW, kR1_SrvH, RP::D_STRUCT);
    for (int unit = 0; unit < 3; ++unit) {
        const uint32_t beatMs = 400u + (uint32_t)unit * 120u;
        int unitY = (y + unit * unitH) & ~3;
        int ledX = x + kR1_SrvW - 8;
        int ledCenterY = unitY + unitH / 2;
        uint16_t ledColor = Avatar::getHypeColor((int16_t)ledX, (int16_t)ledCenterY);
        if ((((now / beatMs) + (uint32_t)unit) & 1u) != 0u)
            fillBlock4(c, ledX, ledCenterY - 2, ledColor);
    }
}

void drawDeskLamp4(M5Canvas& c, int x, int y, uint32_t now) {
    fillRect4(c, x, y + 12, 8, kRoomPX, RP::D_STRUCT);
    fillRect4(c, x + 4, y + 4, kRoomPX, 8, RP::D_STRUCT);
    fillRect4(c, x + 4, y, 4, kRoomPX, RP::WARM);
    fillRect4(c, x, y + 4, 8, kRoomPX, RP::WARM);
    fillBlock4(c, x + 4, y + 4, RP::WARM);
}

void drawNeonSign_SYS4(M5Canvas& c, int x, int y, uint32_t now) {
    static const uint8_t glyphs[3][3] = {
        {0b110, 0b111, 0b011},
        {0b101, 0b010, 0b010},
        {0b110, 0b111, 0b011},
    };
    static const int charX[3] = { 0, 16, 32 };
    bool signLit = isRoom0SignLit(now);
    uint16_t frameColor = signLit ? RP::NEON : RP::D_STRUCT;
    fillRect4(c, x, y, kR1_NeonW, kR1_NeonH, frameColor);
    fillRect4(c, x + 4, y + 4, kR1_NeonW - 8, kR1_NeonH - 8, RP::D_DEEP);
    int lx = x + 4, ly = y + 4;
    for (int ch = 0; ch < 3; ch++) {
        int charBaseX = lx + charX[ch];
        for (int row = 0; row < 3; row++) {
            uint8_t bits = glyphs[ch][row];
            for (int col = 0; col < 3; col++) {
                if (bits & (1 << (2 - col))) {
                    int px = charBaseX + col * kRoomPX;
                    int py = ly + row * kRoomPX;
                    drawDeadLedModule4(c, px, py, 0x24C7u);
                    if (signLit) fillRect4(c, px, py, kRoomPX, kRoomPX, RP::NEON);
                }
            }
        }
    }
}

void drawCyberdolphinAquarium4(M5Canvas& c, int x, int y, int w, int h,
                               uint32_t now) {
    fillRect4(c, x, y, w, h, RP::D_STRUCT);
    fillRect4(c, x + 4, y + 4, w - 8, h - 8, RP::DEEP);
    fillRect4(c, x + 4, y + 4, w - 8, kRoomPX, RP::SHAFT);
    fillRect4(c, x + 4, y + h - 8, w - 8, kRoomPX, RP::D_WALL_NEAR);
    fillRect4(c, x, y - 4, 12, kRoomPX, RP::STRUCT);
    fillRect4(c, x + w - 12, y - 4, 12, kRoomPX, RP::STRUCT);
    for (int cable = 0; cable < 3; ++cable) {
        int cx = x + 8 + cable * 8;
        fillRect4(c, cx, kRoomY + 8, kRoomPX, y - kRoomY - 8, RP::WALL_MID);
        if (cable == 1) fillRect4(c, cx, y - 8, 12, kRoomPX, RP::WALL_MID);
    }
    static constexpr uint32_t kLegMs = 7000u;
    uint32_t leg = now / kLegMs;
    float t = (float)(now % kLegMs) / (float)kLegMs;
    t = t * t * (3.0f - 2.0f * t);
    int ix = x + 4, iy = y + 4, iw = w - 8, ih = h - 8;
    int x0 = (int)(wallHash((int)leg, 0, 0xD011u) % 40u);
    int x1 = (int)(wallHash((int)(leg + 1u), 0, 0xD011u) % 40u);
    int y0 = (int)(wallHash((int)leg, 1, 0xD012u) % 28u);
    int y1 = (int)(wallHash((int)(leg + 1u), 1, 0xD012u) % 28u);
    int dX = (ix + 8 + (int)((float)x0 + ((float)x1 - (float)x0) * t)) & ~3;
    int dY = (iy + 16 + (int)((float)y0 + ((float)y1 - (float)y0) * t)) & ~3;
    dX = max(ix + 4, min(dX, ix + iw - 40));
    dY = max(iy + 4, min(dY, iy + ih - 24));
    uint16_t body = Display::lerpColor565(RP::CRT, RP::SHAFT, 0.22f);
    fillRect4(c, dX + 8, dY + 8, 24, 8, body);
    fillRect4(c, dX + 12, dY + 4, 16, 4, body);
    fillRect4(c, dX + 28, dY + 10, 8, 4, body);
    fillBlock4(c, dX + 24, dY + 8, RP::SPARK);
    uint32_t pulse = 720u;
    if (((now / pulse) & 1u) != 0u)
        fillBlock4(c, dX + 14, dY + 8, RP::LED);
    for (uint8_t i = 0; i < 3; ++i) {
        float phase = (float)((now + (uint32_t)i * 733u) % 3200u) / 3200.0f;
        int bx = ix + 8 + (int)(wallHash(i, 0, 0xBABB1Eu) % (uint32_t)(iw - 16));
        int by = iy + ih - 8 - (int)(phase * (float)(ih - 20));
        bx &= ~3; by &= ~3;
        fillBlock4(c, bx, by, RP::SHAFT);
    }
    uint32_t causticStep = now / 900u;
    int causticCell = (int)(causticStep % 7u);
    if (causticCell > 3) causticCell = 6 - causticCell;
    int causticShift = causticCell * kRoomPX;
    for (int lane = 0; lane < 5; ++lane) {
        if ((wallHash(lane, iy, 0xCA0571Cu) & 0x01u) != 0u) continue;
        int cx = ix + 8 + lane * 16 + causticShift;
        if (cx + 8 <= ix + iw - 8)
            fillRect4(c, cx, iy + ih - 12, 8, kRoomPX, RP::PUDDLE);
    }
}

// ─────────────────────────────────────────────────────────────────────────
// ROOM 1: NOIR APARTMENT
// ─────────────────────────────────────────────────────────────────────────

void drawWornCouch4(M5Canvas& c, int x, int y, uint32_t now) {
    fillRect4(c, x, y, kR2_SofaW, kR2_SofaH, RP::D_STRUCT);
    fillRect4(c, x + 8, y + 4, kR2_SofaW - 16, kR2_SofaH - 12, RP::D_FILL);
    fillRect4(c, x, y, 8, kR2_SofaH, RP::D_STRUCT);
    fillRect4(c, x + kR2_SofaW - 8, y, 8, kR2_SofaH, RP::D_STRUCT);
    fillRect4(c, x + 20, y + 8, 8, kRoomPX, RP::SHADOW_C);
    fillRect4(c, x + 40, y + 12, 12, kRoomPX, RP::SHADOW_C);
    fillRect4(c, x + kR2_SofaW - 12, y - 4, 8, 4, RP::D_STRUCT);
    fillBlock4(c, x + kR2_SofaW - 8, y - 4, RP::BG);
    int embX = x + kR2_SofaW - 4;
    int embY = y - 4;
    if ((wallHash(embX, embY, now / 300) & 0xFF) < 179) {
        fillBlock4(c, embX, embY, RP::WARM);
    }
}

void drawVenetianWindow4(M5Canvas& c, int x, int y, int w, int h) {
    fillRect4(c, x, y, w, h, RP::D_STRUCT);
    fillRect4(c, x + 4, y + 4, w - 8, h - 8, RP::BG);
    for (int slat = 0; slat < 6; slat++) {
        int slatY = y + 8 + slat * 24;
        if (slatY + kRoomPX < y + h - 4) {
            fillRect4(c, x + 4, slatY, w - 8, kRoomPX, RP::D_STRUCT);
        }
    }
}

void drawFloorGrate4(M5Canvas& c, int x, int y, int w, int h) {
    fillRect4(c, x, y, w, h, RP::D_STRUCT);
    for (int gy = y + 4; gy < y + h - 4; gy += 8) {
        for (int gx = x + 4; gx < x + w - 4; gx += 8) {
            fillRect4(c, gx, gy, 4, 4, RP::BG);
        }
    }
}

void drawWallTV4(M5Canvas& c, int x, int y, int w, int h, uint32_t now) {
    fillRect4(c, x, y, w, h, RP::D50_STRUCT);
    fillRect4(c, x + 4, y, w - 8, kRoomPX, RP::SHADOW_C);
    fillRect4(c, x + 4, y + 4, w - 8, h - 8, RP::BG);
    fillRect4(c, x, y - 4, kRoomPX, kRoomPX, RP::D50_STRUCT);
    fillRect4(c, x + 4, y - 8, kRoomPX, kRoomPX, RP::D50_STRUCT);
    fillRect4(c, x + w - 4, y - 4, kRoomPX, kRoomPX, RP::D50_STRUCT);
    fillRect4(c, x + w - 4, y - 8, kRoomPX, kRoomPX, RP::D50_STRUCT);
    int sx = x + 4, sy = y + 4, sw = w - 8, sh = h - 8;
    uint32_t snowSeed = now / 140u;
    for (int py = sy; py < sy + sh; py += kRoomPX) {
        for (int px = sx; px < sx + sw; px += kRoomPX) {
            if ((wallHash(px, py, snowSeed) & 0xFFu) >= 76u) continue;
            fillBlock4(c, px, py, RP::CRT);
        }
    }
    int scanY = (sy + (int)((float)(now % 2400) / 2400.0f * (float)sh)) & ~3;
    if (scanY >= sy && scanY + kRoomPX <= sy + sh)
        fillRect4(c, sx, scanY, sw, kRoomPX, RP::CRT);
    uint32_t noiseCycle = 3900u;
    int noiseY = (sy + (int)((float)(now % noiseCycle) / (float)noiseCycle * (float)(sh + 8)) - 4) & ~3;
    if (noiseY >= sy && noiseY + 8 <= sy + sh)
        fillRect4(c, sx, noiseY, sw, 8, RP::DEEP);
}

void drawSideTableLamp4(M5Canvas& c, int x, int y, uint32_t now) {
    fillRect4(c, x, y, kRoomPX, kFloorY - y, RP::D50_STRUCT);
    fillRect4(c, x + 8, y, kRoomPX, kFloorY - y, RP::D50_STRUCT);
    fillRect4(c, x, y, 12, kRoomPX, RP::D50_FILL);
    fillBlock4(c, x + 4, y, RP::SHADOW_C);
    fillRect4(c, x, y - 16, 8, kRoomPX, RP::D50_STRUCT);
    fillRect4(c, x + 4, y - 8, kRoomPX, 8, RP::D50_STRUCT);
    fillRect4(c, x + 4, y - 20, 4, kRoomPX, RP::WARM);
    fillRect4(c, x, y - 16, 8, kRoomPX, RP::WARM);
    fillBlock4(c, x + 4, y - 16, RP::WARM);
    int gx = x + 4, gy = y - 8;
    fillRect4(c, gx, gy, 8, 4, RP::D50_STRUCT);
    fillRect4(c, gx, gy + 4, 8, 4, RP::D50_STRUCT);
    fillRect4(c, gx, gy, 4, 8, RP::D50_STRUCT);
    fillRect4(c, gx + 4, gy, 4, 8, RP::D50_STRUCT);
    fillBlock4(c, gx + 4, gy + 4, RP::WARM);
    int iceOff = ((now / 4000) & 1) ? 0 : 4;
    fillBlock4(c, gx + iceOff, gy + 4, RP::SHAFT);
}

// ─────────────────────────────────────────────────────────────────────────
// ROOM 2: RAMEN BAR
// ─────────────────────────────────────────────────────────────────────────

void drawRamenCounter4(M5Canvas& c, int x, int y, int w, int h) {
    fillRect4(c, x, y, w, kRoomPX, RP::D_STRUCT);
    fillRect4(c, x + 4, y + kRoomPX, w - 8, h - kRoomPX, RP::FILL);
    fillRect4(c, x, y + h, w, kFloorY - y - h, RP::D_STRUCT);
    for (int gy = y + kRoomPX; gy < y + h; gy += 8) {
        for (int gx = x + 4; gx < x + w - 4; gx += 12) {
            if ((wallHash(gx, gy, 0x5555) & 0xFF) < 60)
                fillBlock4(c, gx, gy, RP::SHADOW_C);
        }
    }
}

void drawNoodleBowl4(M5Canvas& c, int x, int y, int w, int h,
                     bool held, uint32_t now) {
    const int p = kRoomPX;
    if (held) {
        fillRect4(c, x, y, w, p, RP::D_STRUCT);
        fillRect4(c, x + p, y + p, w - 2 * p, h - 2 * p, RP::D_WARM);
        fillRect4(c, x + 2 * p, y + h - p, w - 4 * p, p, RP::D_WARM);
        fillRect4(c, x + 2 * p, y + 2 * p, w - 4 * p, p, RP::D_FILL);
    } else {
        // Full stepped bowl: the migration kept only the upper half, leaving
        // the room's hero prop visibly suspended above the counter.
        fillRect4(c, x - p, y, w + 2 * p, p, RP::D_STRUCT);
        fillRect4(c, x, y + p, w, p, RP::D_WARM);
        fillRect4(c, x, y + 2 * p, w, h - 4 * p, RP::D_WARM);
        fillRect4(c, x + p, y + h - 2 * p, w - 2 * p, p, RP::D_WARM);
        fillRect4(c, x + 2 * p, y + h - p, w - 4 * p, p, RP::D_WARM);
        fillRect4(c, x + 3 * p, y + h, w - 6 * p, p, RP::D_WARM);

        const int brothY = y + 3 * p;
        const int brothH = h - 5 * p;
        if (brothH > 0)
            fillRect4(c, x + p, brothY, w - 2 * p, brothH, RP::D_FILL);
        for (int i = 0; i < 3; ++i) {
            int nx = x + 2 * p + i * 3 * p;
            if (nx + 2 * p > x + w - p) break;
            int ny = brothY + (i & 1) * p;
            fillRect4(c, nx, ny, 2 * p, p, RP::WARM);
        }
        fillBlock4(c, x + w - 3 * p, y - 2 * p, RP::D_STRUCT);
        fillBlock4(c, x + w - 2 * p, y - p, RP::D_STRUCT);
        fillBlock4(c, x + w - 4 * p, y - 2 * p, RP::D_STRUCT);
        fillBlock4(c, x + w - 3 * p, y - p, RP::D_STRUCT);
    }
    if (held) {
        drawRamenVapor4(c, now, x + w / 2, y - 2);
    }
}

void drawBarStool4(M5Canvas& c, int x, int y) {
    fillRect4(c, x, y, kR3_StoolW, kRoomPX, RP::D_WALL_NEAR);
    int postX = x + kR3_StoolW / 2 - 2;
    fillRect4(c, postX, y + kRoomPX, kRoomPX, kFloorY - y - kRoomPX, RP::WALL_MID);
    fillRect4(c, x, kFloorY - 4, kR3_StoolW, kRoomPX, RP::WALL_MID);
}

void drawCoffinPod4(M5Canvas& c, int x, int y, int w, int h,
                    bool occupied, uint32_t now) {
    fillRect4(c, x + 4, y - 4, w - 8, kRoomPX, RP::D_STRUCT);
    fillRect4(c, x + 4, y, w - 8, kRoomPX, RP::D_STRUCT);
    fillRect4(c, x, y + 4, w, h - 4, RP::D_STRUCT);
    fillRect4(c, x + 4, y + 4, w - 8, h - 12, occupied ? RP::D_DEEP : RP::D_FILL);
    if (occupied) {
        fillRect4(c, x + 8, y + 8, w - 16, h - 16, RP::D_WALL_NEAR);
        fillRect4(c, x + 8, y + 8, 36, 20, RP::D_FILL);
        fillRect4(c, x + 12, y + 12, 28, 12, RP::D_WARM);
    } else {
        for (int fy = y + 8; fy < y + h - 8; fy += 8) {
            int row = (fy - y - 8) / 8;
            int tuftX0 = x + 8 + ((row & 1) ? kRoomPX : 0);
            for (int fx = tuftX0; fx < x + w - 8; fx += 12)
                fillBlock4(c, fx, fy, RP::D_DEEP);
        }
        for (int ly = y + 20; ly < y + h - 8; ly += 16)
            fillRect4(c, x + 8, ly, w - 16, kRoomPX, RP::D_STRUCT);
    }
    fillRect4(c, x + 4, y + 4, 4, 8, RP::D_STRUCT);
    fillBlock4(c, x + 4, y + 4, RP::BG);
    fillBlock4(c, x + 4, y + 8, RP::BG);
    if (podLedOn)
        fillBlock4(c, x + w - 8, y + 8, RP::LED);
    fillRect4(c, x, y + h, w, kRoomPX, RP::D_STRUCT);
    int footH = kFloorY - y - h - kRoomPX;
    if (footH > 0) {
        fillRect4(c, x + 4, y + h + kRoomPX, 4, footH, RP::D_STRUCT);
        fillRect4(c, x + w - 8, y + h + kRoomPX, 4, footH, RP::D_STRUCT);
    }
    fillBlock4(c, x + w - 4, y + 8, RP::D_FILL);
    fillBlock4(c, x + w - 4, y + 16, RP::D_FILL);
    fillRect4(c, x + 4, kFloorY, 8, kRoomPX, RP::SHADOW_C);
    fillRect4(c, x + w - 12, kFloorY, 8, kRoomPX, RP::SHADOW_C);
}

void drawNeonSign_RAMEN4(M5Canvas& c, int x, int y, uint32_t now) {
    static const uint8_t glyphs[4][5] = {
        {0b1111, 0b0100, 0b0110, 0b0010, 0b0001},
        {0b0000, 0b0000, 0b1111, 0b0000, 0b0000},
        {0b0100, 0b1010, 0b0100, 0b1000, 0b0000},
        {0b1100, 0b0000, 0b0010, 0b0100, 0b1000},
    };
    bool neonOn = isNeonOn(now);
    fillRect4(c, x + 4, y - 4, kR3_SignW - 8, kRoomPX, RP::D_STRUCT);
    fillRect4(c, x + 12, y - 4, kRoomPX, 8, RP::D_STRUCT);
    fillRect4(c, x + kR3_SignW - 16, y - 4, kRoomPX, 8, RP::D_STRUCT);
    for (int ch = 0; ch < 4; ch++) {
        bool charDim = ((wallHash(ch, 0, now / 200) & 0xFF) < 20);
        bool charLit = neonOn && !charDim;
        int charBaseX = x + 4 + ch * 16;
        for (int row = 0; row < 5; row++) {
            uint8_t bits = glyphs[ch][row];
            int tiltDx = (row / 4) * kRoomPX;
            for (int col = 0; col < 4; col++) {
                if (bits & (1 << (3 - col))) {
                    int px = charBaseX + col * kRoomPX + tiltDx;
                    if (px > x + kR3_SignW - kRoomPX) continue;
                    int py = (y + 4 + row * kRoomPX) & ~3;
                    drawDeadLedModule4(c, px, py, 0x38B1u);
                    if (charLit) fillRect4(c, px, py, kRoomPX, kRoomPX, RP::NEON);
                }
            }
        }
    }
}

void drawPaperLantern4(M5Canvas& c, int x, int y, uint32_t now) {
    const int cordY = (kRoomY + kRoomPX - 1) & ~(kRoomPX - 1);
    fillRect4(c, x + 8, cordY, kRoomPX, y - cordY, RP::D_STRUCT);
    fillRect4(c, x + 4, y, 16, kRoomPX, RP::D_STRUCT);
    fillRect4(c, x, y + kRoomPX, 24, 24, RP::D_WARM);
    uint16_t glow = ((now / 1300u) & 1u) ? RP::WARM : RP::D_WARM;
    fillRect4(c, x + 4, y + 8, 16, 12, glow);
    fillRect4(c, x, y + 28, 24, kRoomPX, RP::D_STRUCT);
    fillRect4(c, x + 4, y + 32, 16, kRoomPX, RP::D_STRUCT);
}

// ─────────────────────────────────────────────────────────────────────────
// ROOM 3: ROOST (SURVEILLANCE NEST)
// ─────────────────────────────────────────────────────────────────────────

void drawAntennaArray4(M5Canvas& c, int x, int y, uint32_t now) {
    const int anchorX = x + kR4_AntennaW / 2;
    const int anchorY = y + kR4_AntennaH + 4;
    const int poleGap = 16;

    for (int i = 0; i < 3; i++) {
        int poleX = x + i * poleGap;
        int poleH = kR4_AntennaH - (i == 0 || i == 2 ? 2 * kRoomPX : 0);
        int poleBaseY = y + kR4_AntennaH - poleH;
        uint16_t poleCol = (i == 1) ? RP::WALL_NEAR : RP::WALL_MID;

        fillRect4(c, poleX, poleBaseY, kRoomPX, poleH, poleCol);
        fillRect4(c, poleX, poleBaseY + kRoomPX, kRoomPX, kRoomPX, RP::STRUCT);

        // Base stiffeners and cross members.
        fillRect4(c, poleX - 4, poleBaseY + (int)(poleH * 0.55f), kRoomPX, kRoomPX, RP::STRUCT);
        fatLine4(c, poleX, poleBaseY + 2 * kRoomPX, poleX + (i == 1 ? 8 : -4),
                 poleBaseY + 5 * kRoomPX, RP::WALL_MID);
        if (i != 1) {
            fillRect4(c, poleX - 4, poleBaseY + 8 * kRoomPX, 12, kRoomPX, RP::WALL_MID);
            fillRect4(c, poleX - 4, poleBaseY + 12 * kRoomPX, 12, kRoomPX, RP::SHADOW_C);
        }

        // Guy wire from each pole to midpoint anchor.
        fatLine4(c, poleX, poleBaseY + 7 * kRoomPX,
                 anchorX, anchorY, RP::WALL_MID);
        fatLine4(c, poleX + kRoomPX, poleBaseY + 6 * kRoomPX,
                 anchorX + kRoomPX, anchorY + kRoomPX, RP::SHADOW_C);

        // Blinking beacon at each mast top; center mast stays brighter.
        bool beaconOn = ((now / (i == 1 ? 140u : 220u)) & 1u) != 0u;
        uint16_t beaconCol = i == 1 ? RP::LED : RP::D_WARM;
        fillBlock4(c, poleX, poleBaseY - 2 * kRoomPX, beaconOn ? beaconCol : RP::D_DEEP);
        if (beaconOn) {
            fillBlock4(c, poleX - 4, poleBaseY - kRoomPX, RP::NEON);
            if (i == 1) {
                fillRect4(c, poleX - 4, poleBaseY - 4, 12, kRoomPX, RP::LED);
            }
        }
    }

    // Guy-wire anchor hardware.
    fillRect4(c, anchorX - 8, anchorY - 4, 12, 4, RP::WALL_MID);
    fillRect4(c, anchorX - 4, anchorY - 8, 4, 12, RP::WALL_NEAR);
    fillBlock4(c, anchorX - 4, anchorY - kRoomPX, RP::WARM);
}

void drawSatelliteDish4(M5Canvas& c, int x, int y, uint32_t now) {
    const int cx = x + (kR4_DishW / 2);
    fillRect4(c, x + 8, y + 8, kR4_DishW - 16, kR4_DishH - 20, RP::WALL_MID);
    fillRect4(c, cx - 2 * kRoomPX, y + 4, kR4_DishW - 4 * kRoomPX, 4, RP::WALL_NEAR);
    fillRect4(c, cx + 12, y + kR4_DishH - 12, kRoomPX, 4, RP::WALL_MID);
    fillRect4(c, cx - kRoomPX, y + kR4_DishH - 12, 8, kRoomPX, RP::D_STRUCT);

    // Parabolic mesh (5x5 ringed segments) for a cleaner dish silhouette.
    for (int band = 0; band < 5; ++band) {
        int hh = y + 12 + band * 6;
        int halfW = 2 * kRoomPX - band * kRoomPX / 2;
        if (halfW <= 0) break;
        int left = cx - halfW;
        int right = cx + halfW;
        fatLine4(c, left, hh, right, hh, (band % 2 == 0) ? RP::WALL_MID : RP::WALL_NEAR);
        if (band % 2 == 1 && kRoomPX * (band + 1) < kR4_DishH - 12)
            fatLine4(c, left, hh, cx + (band & 1 ? -kRoomPX : kRoomPX), hh + kRoomPX,
                     RP::D_WALL_NEAR);
    }

    // LNB mast and flexible arm.
    fillRect4(c, cx + 4, y + 4, 4, 16, RP::D_STRUCT);
    fillRect4(c, cx + 8, y + 16, kRoomPX, kRoomPX, RP::D_FILL);
    fatLine4(c, cx + kRoomPX, y + 20, cx + 16, y + 28, RP::WALL_MID);
    if (((now / 350u) & 1u) != 0u) {
        fillBlock4(c, cx + 16, y + 28, RP::LED);
        drawDeadLedModule4(c, cx - 4, y + 28, 0xD1F1u);
    }
}

void drawRooftopShack4(M5Canvas& c, int x, int y, int w, int h) {
    fillRect4(c, x, y, w, h, RP::D_STRUCT);
    fillRect4(c, x, y + h - 24, 16, 24, RP::DEEP);
    fillRect4(c, x + 4, y + h - 24, 16, kRoomPX, RP::CRT);
    fillRect4(c, x + 24, y + 8, 16, 12, RP::DEEP);
    fillRect4(c, x + 24, y + 8, 16, kRoomPX, RP::CRT);
    fillRect4(c, x + 24, y + h - 24, 16, kRoomPX, RP::D_WARM);
    fillRect4(c, x + w - 16, y, 12, 8, RP::D_STRUCT);
    fillBlock4(c, x + w - 12, y + 4, RP::GREEN_DK);

    // Doorway and emissive window.
    int doorX = x + w / 2 - 10;
    int doorY = y + 2 * kRoomPX;
    int doorW = 20;
    int doorH = 16;
    fillRect4(c, doorX, doorY, doorW, kRoomPX, RP::WALL_MID);
    fillRect4(c, doorX + 4, doorY, 12, doorH, RP::DEEP);
    fillRect4(c, doorX + 4, doorY + 8, 12, kRoomPX, RP::DEEP);
    fillBlock4(c, doorX + 12, doorY + 4, RP::GREEN_DK);
    fillRect4(c, doorX + 4, doorY + 4, 12, 4, (wallHash(doorX, doorY, 0xA111u) & 0x03u) ? RP::WARM : RP::CRT);
    fillRect4(c, doorX + 6, doorY + 9, 8, kRoomPX, RP::D_DEEP);
    fillRect4(c, x + 18, doorY + 12, kRoomPX, kRoomPX, RP::FLUOR);

    // Small window glow + antenna junction light source.
    fillRect4(c, x + 10, y + 6, 8, 8, RP::DEEP);
    fillRect4(c, x + 12, y + 8, 4, 4, RP::NEON);
    fillBlock4(c, x + 11, y + 7, RP::FLUOR);
    fillBlock4(c, x + w - 10, y + 8, RP::LED);
    fillRect4(c, x + w - 14, y + 12, 8, 4, RP::D_DEEP);
    drawCableCoil4(c, x + w - 12, y + 20);
}

void drawLedgeRailing4(M5Canvas& c, int x, int y, int w) {
    fillRect4(c, x, y, w, kRoomPX, RP::D_STRUCT);
    for (int px = x + 4; px < x + w; px += 16) {
        fillRect4(c, px, y + kRoomPX, kRoomPX, 20, RP::WALL_MID);
    }
    fillRect4(c, x, y + 12, w, kRoomPX, RP::D_STRUCT);
    for (int px = x + 8; px < x + w - 4; px += 20) {
        fillRect4(c, px, y + 4, 8, kRoomPX, RP::WALL_NEAR);
        fillRect4(c, px, y + 16, 8, kRoomPX, RP::SHADOW_C);
        if (((wallHash(px, y, 0x77u) & 0x03u) == 0u))
            fillRect4(c, px + 2, y + 12, 4, 8, RP::WARM);
    }
}

// ─────────────────────────────────────────────────────────────────────────
// ROOM 4: UNDERGROUND BAR
// ─────────────────────────────────────────────────────────────────────────

void drawCRTTerminal4(M5Canvas& c, int x, int y, int w, int h, uint32_t now) {
    fillRect4(c, x, y, w, h, RP::D_STRUCT);
    fillRect4(c, x + 4, y + 4, w - 8, h - 8, RP::DEEP);
    int screenH = h - 8;
    int maxLineW = w - 8;
    for (int ly = 0; ly < screenH; ly += 4) {
        uint32_t lh = wallHash(ly, 0, now / 300);
        if ((lh & 0xFF) < 50) continue;
        int indent = (int)(lh & 0x03) * 4;
        int lineW = (8 + (int)((lh >> 4) & 0x1F)) & ~(kRoomPX - 1);
        if (lineW > maxLineW - indent) lineW = maxLineW - indent;
        fillRect4(c, x + 4 + indent, y + 4 + ly, lineW, 4, RP::CRT);
    }
    if (roomMood.captureCount > 0) {
        int flashLine = y + 4 + (int)((now / 200) % (uint32_t)(screenH / 4)) * 4;
        fillRect4(c, x + 4, flashLine, w - 8, 4, RP::NEON);
    }
    int scanY = y + 4 + (int)((now % 2400) * (uint32_t)screenH / 2400) & ~3;
    if (scanY >= y + 4 && scanY < y + 4 + screenH - 4) {
        for (int sx = x + 4; sx < x + w - 4; sx += kRoomPX) {
            uint16_t base = fastReadPx(c, sx, scanY);
            c.fillRect(sx, scanY, kRoomPX, kRoomPX, screenBlend565(base, RP::CRT, 40));
        }
    }
    if ((now / 530) & 1) {
        int cursorY = y + screenH;
        int cursorX = x + 8 + (int)((wallHash(0, 0, now / 4000) >> 4) & 0x07) * 4;
        fillBlock4(c, cursorX, cursorY, RP::CRT);
    }
    uint8_t pips = (uint8_t)min(6u, ((uint32_t)roomMood.rfActivity + 42u) / 43u);
    for (uint8_t pip = 0; pip < 6u; ++pip)
        fillBlock4(c, x + kRoomPX + pip * kRoomPX, y + kRoomPX,
                   pip < pips ? RP::GREEN_DK : RP::D_DEEP);
    uint16_t threat = roomMood.spamActive ? RP::SPARK
        : (roomMood.trackerPresent ? RP::CRT : RP::D_DEEP);
    fillBlock4(c, x + w - 3 * kRoomPX, y + kRoomPX, threat);
    fillBlock4(c, x + w - 2 * kRoomPX, y + kRoomPX,
               roomMood.captureCount > 0 ? RP::FLUOR : RP::D_DEEP);
    fillRect4(c, x, y + h, w, kRoomPX, RP::WALL_MID);
    for (int i = 0; i < 3; i++)
        fillRect4(c, x + 4 + i * 16, y + h + kRoomPX, 8, kRoomPX, RP::FILL);
    for (int i = 0; i < 3; i++) {
        uint32_t ledCycle = 1200 + i * 400;
        if ((now % ledCycle) < 400)
            fillBlock4(c, x + 8 + i * 16, y + h + kRoomPX, RP::LED);
    }
}

void drawNeonSign_THEPEN4(M5Canvas& c, int x, int y, uint32_t now) {
    static const uint8_t glyphs[6][7] = {
        {0b11111, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100},
        {0b10001, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001},
        {0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b11111},
        {0b11110, 0b10001, 0b10001, 0b11110, 0b10000, 0b10000, 0b10000},
        {0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b11111},
        {0b10001, 0b11001, 0b10101, 0b10011, 0b10001, 0b10001, 0b10001},
    };
    static const int charX[6] = { 0, 24, 48, 76, 100, 124 };
    bool neonOn = isNeonOn(now);
    fillRect4(c, x + 8, y - 4, kR5_NeonW - 16, kRoomPX, RP::D_STRUCT);
    fillRect4(c, x + 20, y - 4, kRoomPX, 8, RP::D_STRUCT);
    fillRect4(c, x + kR5_NeonW - 24, y - 4, kRoomPX, 8, RP::D_STRUCT);
    fillRect4(c, x + kR5_NeonW / 2 - 2, y - 4, kRoomPX, 8, RP::D_STRUCT);
    fillRect4(c, x, y, kR5_NeonW, kR5_NeonH, RP::DEEP);
    for (int ch = 0; ch < 6; ch++) {
        bool charDim = ((wallHash(ch, 0, now / 200) & 0xFFu) < 20u);
        bool charLit = neonOn && !charDim;
        int charBaseX = x + 4 + charX[ch];
        for (int row = 0; row < 7; row++) {
            uint8_t bits = glyphs[ch][row];
            int tiltDx = row * 2;
            for (int col = 0; col < 5; col++) {
                if (bits & (1 << (4 - col))) {
                    int px = (charBaseX + col * kRoomPX + tiltDx) & ~3;
                    if (px > x + kR5_NeonW - kRoomPX) continue;
                    int py = (y + 4 + row * kRoomPX) & ~3;
                    drawDeadPenModule4(c, px, py);
                    if (charLit) fillRect4(c, px, py, kRoomPX, kRoomPX, RP::NEON);
                }
            }
        }
    }
}

void drawBarCounter4(M5Canvas& c, int x, int y, int w, uint32_t now) {
    fillRect4(c, x, y, w, kR5_BarSurfH, RP::D_STRUCT);
    fillRect4(c, x + 4, y + 4, w - 8, kR5_BarSurfH - 4, RP::FILL);
    fillRect4(c, x, y + kR5_BarSurfH, w, kFloorY - y - kR5_BarSurfH, RP::WALL_MID);
    fillRect4(c, x + 4, y + kR5_BarSurfH + 4, w - 8,
              kFloorY - y - kR5_BarSurfH - 8, RP::FILL);
    fillRect4(c, x + 4, kFloorY - 4, w - 8, kRoomPX, RP::DEEP);
    fillRect4(c, x, y, w, kRoomPX, RP::D_STRUCT);
    int bottleBase = y - 4;
    fillRect4(c, x + 8, bottleBase - 12, 4, 4, RP::D_WALL_NEAR);
    fillRect4(c, x + 8, bottleBase - 8, 4, 12,
              Display::lerpColor565(RP::DEEP, RP::WARM, 0.35f));
    fillRect4(c, x + 8, bottleBase - 12, 8, kRoomPX, RP::D_STRUCT);
    fillRect4(c, x + 20, bottleBase - 8, 4, 4, RP::D_WALL_NEAR);
    fillRect4(c, x + 20, bottleBase - 4, 4, 8,
              Display::lerpColor565(RP::DEEP, RP::CRT, 0.35f));
    fillRect4(c, x + 20, bottleBase - 8, 8, kRoomPX, RP::D_STRUCT);
    fillRect4(c, x + 36, bottleBase - 12, 4, 8,
              Display::lerpColor565(RP::DEEP, RP::SHAFT, 0.18f));
    fillRect4(c, x + 36, bottleBase - 4, 4, 8, RP::D_WALL_NEAR);
    fillRect4(c, x + 52, bottleBase - 8, 4, 8,
              Display::lerpColor565(RP::DEEP, RP::WARM, 0.35f));
    fillRect4(c, x + 52, bottleBase, 4, 4, RP::D_WALL_NEAR);
    fillRect4(c, x + 52, bottleBase - 8, 8, kRoomPX, RP::D_STRUCT);
    fillRect4(c, x + 68, bottleBase, 8, kRoomPX, RP::PUDDLE);
    fillRect4(c, x + 68, bottleBase - 4, 8, kRoomPX, RP::D_WALL_NEAR);
    for (int si = 0; si < 2; si++) {
        int stX = x + 16 + si * 52;
        int stSeatY = kFloorY - 24;
        fillRect4(c, stX, stSeatY, 12, kRoomPX, RP::D_STRUCT);
        fillRect4(c, stX + 4, stSeatY + kRoomPX, 4, kFloorY - stSeatY - kRoomPX, RP::WALL_MID);
        fillRect4(c, stX, kFloorY - 8, 12, kRoomPX, RP::WALL_MID);
    }
}

void drawCornerBooth4(M5Canvas& c, int x, int y, int w, int h) {
    fillRect4(c, x, y, w - 16, h, RP::SOFT);
    fillRect4(c, x + 4, y + 4, w - 24, h - 8, RP::FILL);
    fillRect4(c, x + 12, y + 4, 8, kRoomPX, RP::SHADOW_C);
    fillRect4(c, x + 32, y + 8, 8, kRoomPX, RP::SHADOW_C);
    fillRect4(c, x + 4, kFloorY - 4, w - 24, kRoomPX, RP::DEEP);
    int backX = x + w - 20;
    fillRect4(c, backX, y - 56, 16, h + 56, RP::SOFT);
    fillRect4(c, backX + 4, y - 52, 8, h + 48, RP::FILL);
    fillRect4(c, backX, y - 56, 16, kRoomPX, RP::D_STRUCT);
    fillRect4(c, x, y - 32, 8, 32, RP::SOFT);
    fillRect4(c, x + 4, y - 28, 4, 24, RP::FILL);
    fillRect4(c, x, y - 32, 8, kRoomPX, RP::D_STRUCT);
    int tableW = 72, tableH = 8;
    int tableX = x + 8, tableY = y - 16;
    fillRect4(c, tableX, tableY, tableW, tableH, RP::D_STRUCT);
    fillRect4(c, tableX + 4, tableY + 4, tableW - 8, 4, RP::FILL);
    fillRect4(c, tableX + 12, tableY + tableH, kRoomPX, kFloorY - tableY - tableH, RP::WALL_MID);
    fillRect4(c, tableX + tableW - 12, tableY + tableH, kRoomPX, kFloorY - tableY - tableH, RP::WALL_MID);
    fillRect4(c, tableX + 48, tableY - 8, 4, 8, RP::D_WALL_NEAR);
    fillRect4(c, tableX + 48, tableY - 8, 8, kRoomPX, RP::PUDDLE);
    fillRect4(c, tableX + 46, tableY - 2, 8, 2, RP::SOFT);
    fillBlock4(c, tableX + 46, tableY - 4, RP::PUDDLE);
    fillBlock4(c, tableX + 54, tableY - 4, RP::PUDDLE);
    fillRect4(c, tableX + 28, tableY - 4, 8, kRoomPX, RP::D_WALL_NEAR);
    fillBlock4(c, tableX + 24, tableY - 4, RP::PUDDLE);
}

void drawBarmanNPC4(M5Canvas& c, int x, int y, uint32_t now) {
    fillRect4(c, x + 24, y, 24, 16, RP::FILL);
    fillRect4(c, x + 28, y + 4, 16, 8, RP::D_STRUCT);
    fillBlock4(c, x + 44, y + 8, RP::DEEP);
    fillRect4(c, x + 48, y + 12, 8, 4, RP::SOFT);
    fillRect4(c, x + 40, y + 4, kPigPX, 8, RP::SHADOW_C);
    fillRect4(c, x + 20, y + 16, 32, 16, RP::FILL);
    fillRect4(c, x + 24, y + 20, 24, 8, RP::D_STRUCT);
    fillRect4(c, x + 12, y + 20, 8, 12, RP::FILL);
    fillRect4(c, x + 52, y + 20, 8, 12, RP::FILL);
    fillRect4(c, x + 24, y + 32, 8, 10, RP::FILL);
    fillRect4(c, x + 40, y + 32, 8, 10, RP::FILL);
    int emberX = x + 12;
    int emberY = y + 18;
    drawSteam4(c, RP::DUST, now, emberX, emberY);
    drawSteam4(c, RP::DUST, now + 700, emberX + 2, emberY - 2);
}

void drawGlassRack4(M5Canvas& c, int x, int y) {
    fillRect4(c, x, y, kR5_RackW, kRoomPX, RP::D_STRUCT);
    fillRect4(c, x + 4, y + kRoomPX, kR5_RackW - 8, kRoomPX, RP::WALL_MID);
    for (int i = 0; i < 4; i++) {
        int gx = x + kRoomPX + i * 3 * kRoomPX;
        int gy = y + 2 * kRoomPX;
        fillRect4(c, gx, gy, 8, kRoomPX, RP::PUDDLE);
        fillRect4(c, gx + kRoomPX, gy + kRoomPX, kRoomPX, kRoomPX, RP::D_WALL_NEAR);
        fillRect4(c, gx + kRoomPX, gy + 2 * kRoomPX, kRoomPX, kRoomPX, RP::WALL_FAR);
    }
}

void drawKaraokeStage4(M5Canvas& c, int x, int y, uint32_t now) {
    fillRect4(c, x, y + 64, 64, kRoomPX, RP::D_STRUCT);
    fillRect4(c, x + 4, y + 68, 56, kRoomPX, RP::WALL_MID);
    fillBlock4(c, x + 8, y + 64, RP::NEON);
    fillBlock4(c, x + 28, y + 64, RP::NEON);
    fillBlock4(c, x + 48, y + 64, RP::NEON);
    fillRect4(c, x + 52, y + 16, 8, 8, RP::D_STRUCT);
    fillBlock4(c, x + 52, y + 16, RP::CRT);
    fillRect4(c, x + 52, y + 24, kRoomPX, 40, RP::WALL_MID);
    fillRect4(c, x + 48, y + 60, 20, kRoomPX, RP::D_STRUCT);
    fillRect4(c, x + 12, y + 32, 32, 8, RP::FILL);
    fillRect4(c, x + 16, y + 40, 24, 12, RP::FILL);
    fillRect4(c, x + 20, y + 52, 8, 12, RP::FILL);
    fillRect4(c, x + 32, y + 52, 8, 12, RP::FILL);
    fillRect4(c, x + 24, y + 16, 24, 12, RP::FILL);
    fillBlock4(c, x + 44, y + 20, RP::DEEP);
    fillRect4(c, x + 48, y + 24, 8, 4, RP::SOFT);
}

void drawPendantLight4(M5Canvas& c, int x, int y, uint32_t now) {
    fillRect4(c, x, kRoomY, kRoomPX, y - kRoomY, RP::WALL_MID);
    fillRect4(c, x - kRoomPX, y, 12, 8, RP::D_STRUCT);
    fillBlock4(c, x, y + 4, RP::FILL);
    fillRect4(c, x, y + 8, 8, kRoomPX, RP::WALL_MID);
}

// ─────────────────────────────────────────────────────────────────────────
// ROOM 5: COMFORT BALCONY
// ─────────────────────────────────────────────────────────────────────────

void drawHotBath4(M5Canvas& c, int x, int y, int w, int h, uint32_t now) {
    fillRect4(c, x + 8, y - 4, w - 16, 8, RP::D_STRUCT);
    fillRect4(c, x, y + 4, w, 16, RP::D_STRUCT);
    fillRect4(c, x + 8, y + 4, w - 16, 16, RP::D_DEEP);
    fillRect4(c, x + 12, kR6_TubWaterY, w - 24, 8, RP::PUDDLE);
    drawSteam4(c, RP::DUST, now + 160u, x + 44, y + 2);
    drawSteam4(c, RP::DUST, now + 920u, x + w / 2, y - 2);
    drawSteam4(c, RP::DUST, now + 1510u, x + w - 40, y + 4);
    int beat = (int)((now + 5u * 317u) / 200u);
    int leftLift = ((beat / 4u) & 1u) ? 0 : kPigPX;
    int rightLift = leftLift == 0 ? kPigPX : 0;
    fillRect4(c, x + 20, y - 8, kRoomPX, 8, RP::D_WARM);
    fillBlock4(c, x + 20, y - 12 - leftLift, RP::WARM);
    fillRect4(c, x + w - 24, y - 8, kRoomPX, 8, RP::D_WARM);
    fillBlock4(c, x + w - 24, y - 12 - rightLift, RP::WARM);
}

void drawRainGlassWall4(M5Canvas& c, int x, int y, int w, int h,
                        uint32_t now, int pigX) {
    fillRect4(c, x, y, w, kRoomPX, RP::STRUCT);
    fillRect4(c, x, y + h - 4, w, kRoomPX, RP::STRUCT);
    fillRect4(c, x, y, kRoomPX, h, RP::STRUCT);
    fillRect4(c, x + w - 4, y, kRoomPX, h, RP::STRUCT);
    for (int mx = x + 76; mx < x + w - 20; mx += 80)
        fillRect4(c, mx, y, kRoomPX, h, RP::D_STRUCT);
}

void drawBalconyDeck4(M5Canvas& c, int x, int y, int w) {
    // Phase off the slat index, not the absolute X. The deck rides the near
    // parallax plane, so room 5 draws it from a negative origin to over-cover
    // both screen edges — and C++ integer division truncates toward zero, which
    // makes (sx / 24) repeat across x = 0 and butt two identical slats
    // together. The index is what this alternation always meant.
    int slat = 0;
    for (int sx = x; sx < x + w; sx += 24, ++slat) {
        uint16_t tone = (slat & 1) ? RP::D_WARM : RP::D_WALL_NEAR;
        fillRect4(c, sx, y, 20, 8, tone);
        fillRect4(c, sx + 4, y + 4, 12, kPigPX, RP::D_DEEP);
    }
}

void drawCandles4(M5Canvas& c, int x, int y, uint32_t now) {
    for (int i = 0; i < 2; i++) {
        int cx = x + i * 24;
        fillRect4(c, cx, y, kRoomPX, 8, RP::D_WARM);
        int flameLift = (((now / 800u) + i) & 1u) ? 0 : kPigPX;
        fillBlock4(c, cx, y - 4 - flameLift, RP::WARM);
    }
}

// ─────────────────────────────────────────────────────────────────────────
// SHARED PROPS
// ─────────────────────────────────────────────────────────────────────────

void drawSakeBottle4(M5Canvas& c, int x, int y, uint32_t now) {
    fillRect4(c, x, y, 4, 12, RP::SHAFT);
    fillRect4(c, x, y, 8, 4, RP::D_STRUCT);
}

void drawKettleSteam4(M5Canvas& c, int x, int y, uint32_t now) {
    drawSteam4(c, RP::DUST, now, x, y);
    drawSteam4(c, RP::DUST, now + 700, x + 2, y - 2);
}

void drawKettle4(M5Canvas& c, int x, int y, uint32_t now, bool steaming) {
    fillRect4(c, x + 4, y, 8, kRoomPX, RP::D_STRUCT);
    fillRect4(c, x, y + 4, 12, 8, RP::D_STRUCT);
    fillRect4(c, x + 4, y + 4, 8, 8, RP::D_WARM);
    fillBlock4(c, x + 12, y + 4, RP::D_STRUCT);
    fillBlock4(c, x - 4, y + 4, RP::D_STRUCT);
    if (steaming)
        drawKettleSteam4(c, x + 10, y - 4, now);
}

void drawRFBonsai4(M5Canvas& c, int rootX, int rootY, int maxH,
                   uint32_t seed, uint32_t now, bool bathStyle) {
    fillRect4(c, rootX - 8, rootY, 16, 12, RP::D_STRUCT);
    fillRect4(c, rootX - 4, rootY + 4, 8, 8, RP::D_DEEP);
    int trunkH = maxH / 3;
    fillRect4(c, rootX - 2, rootY - trunkH, kRoomPX, trunkH, RP::D_WARM);
    for (int i = 0; i < 6; i++) {
        uint32_t h = wallHash(i, 0, seed);
        int bx = rootX + (int)((h % 40u) - 20u);
        int by = rootY - trunkH + (int)((h >> 4) % (uint32_t)(trunkH / 2));
        int bw = 4 + (int)(h % 12u);
        uint16_t leaf = bathStyle ? RP::D_WARM : RP::GREEN_DK;
        fillRect4(c, bx, by, bw, kRoomPX, leaf);
    }
    uint8_t fruitCount = min((uint8_t)3, roomMood.captureCount);
    for (uint8_t f = 0; f < fruitCount; f++) {
        uint32_t fh = wallHash(f, 1, seed);
        int fx = rootX + (int)((fh % 24u) - 12u);
        int fy = rootY - trunkH - 4 + (int)((fh >> 4) % 12u);
        fillBlock4(c, fx, fy, RP::FLUOR);
    }
}

} // namespace PixelFurn
