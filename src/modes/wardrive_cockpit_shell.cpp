/**
 * wardrive_cockpit_shell.cpp — cockpit shell, dash, chair, yoke, scanlines, hints
 *
 * Extracted from wardrive_scene.cpp: structural cockpit frame (canopy glass,
 * pillars, roof, dashboard), console layer, monitors, steering wheel, chair,
 * dash reflections, CRT scanlines, and dash hint labels.
 *
 * Includes internal helpers: drawCableRun, drawAuxMonitor,
 * drawConsoleModule, drawConsoleElectronics, chairLitMaterial.
 */

#include "wardrive_cockpit_shell.h"
#include "wardrive_shared.h"
#include "wardrive_glass.h"
#include "wardrive.h"
#include <math.h>
#include <string.h>

static constexpr float PI_F = 3.14159265f;

namespace WardriveScene {

// ═══════════════════════════════════════════════════════════════════════════
// INTERNAL HELPERS (static, used by cockpit shell functions)
// ═══════════════════════════════════════════════════════════════════════════

static void drawCableRun(M5Canvas& canvas, int mountX, int mountY, int height,
                         float sway, uint16_t accent, uint32_t now, uint32_t seed) {
    uint16_t cableCol = Display::lerpColor565(RP::DEEP, RP::STRUCT, 0.35f);
    uint16_t cableHi = Display::lerpColor565(RP::STRUCT, RP::FLUOR, 0.16f);
    uint16_t plugCol = Display::lerpColor565(WD_NEON, RP::STRUCT, 0.58f);
    uint16_t mountCol = Display::lerpColor565(RP::DEEP, RP::STRUCT, 0.52f);
    int length = clampi(height / PX, 3, 24);
    int swayPx = q((int)sway);
    float sag = 3.0f + fabsf(sway) * 0.20f;

    int plateX = q(mountX - PX);
    int plateY = q(mountY - PX);
    canvas.fillRect(plateX, plateY, PX * 3, PX, mountCol);
    canvas.fillRect(plateX + PX, mountY, PX, PX, cableHi);
    if (((now / 320UL) + seed) & 1u) glowPx(canvas, plateX + PX, plateY, accent, 58);

    for (int i = 0; i < length; i++) {
        float t = (float)i / (float)max(1, length - 1);
        int cx = q(mountX + (int)(swayPx * t * t));
        int cy = q(mountY + i * PX + (int)(sag * sinf(t * PI_F)));
        if (cy < WD_PLAY_T || cy >= WD_PLAY_B) continue;
        canvas.fillRect(cx, cy, PX, PX, (i < 2) ? cableHi : cableCol);
    }

    int endX = q(mountX + swayPx);
    int endY = q(mountY + height);
    if (endY >= WD_PLAY_T && endY < WD_PLAY_B) {
        canvas.fillRect(endX, endY, PX * 2, PX, plugCol);
        if (((seed + now / 220UL) & 3u) != 0u) glowPx(canvas, endX, endY, accent, 92);
    }
}

// ==[ 3D MONITOR ]== yaw-based perspective box with tapered face
static void drawAuxMonitor(M5Canvas& canvas, int mx, int my, int mw, int mh,
                           uint16_t accent, const char* value,
                           uint32_t now, uint32_t seed, int8_t yawVal) {
    int cell = PX;
    uint16_t dimAccent = Display::lerpColor565(RP::DEEP, accent, 0.33f);  // 3× less bright
    accent = dimAccent;
    uint16_t monBg    = Display::lerpColor565(RP::BG, RP::DEEP, 0.82f);
    uint16_t monFrame = Display::lerpColor565(RP::DEEP, RP::SHADOW_C, 0.42f);
    uint16_t monSide  = Display::lerpColor565(RP::DEEP, RP::SHADOW_C, 0.85f);
    uint16_t monTop   = Display::lerpColor565(RP::DEEP, RP::SHADOW_C, 0.62f);

    if (mx < 0 || mx + mw > 320 || my < WD_PLAY_T || my + mh >= WD_PLAY_B) return;

    int yaw = (yawVal < 0) ? -1 : 1;
    int yawMag = max(1, abs(yawVal));
    int sideW = min(mw - cell * 4, cell * (yawMag + 1));
    int topH = cell;
    int taper = cell * yawMag;

    int faceLeft  = mx + ((yaw < 0) ? sideW : 0);
    int faceRight = mx + mw - ((yaw > 0) ? sideW : 0);
    int faceY0 = my + topH;
    int faceY1 = my + mh;
    int faceH = faceY1 - faceY0;
    if (faceRight - faceLeft < cell * 4 || faceH < cell * 3) return;

    // shadow under monitor
    int shadowX = mx + ((yaw > 0) ? cell : 0);
    int shadowW = max(cell * 2, faceRight - faceLeft);
    canvas.fillRect(shadowX, my + mh, shadowW, cell, Display::lerpColor565(RP::BG, RP::DEEP, 0.65f));

    int armX = q((yaw < 0) ? (mx + mw - cell * 2) : (mx + cell));
    int armY = my + mh;
    for (int py = armY; py < armY + cell * 3; py += cell) {
        canvas.fillRect(armX, py, cell, cell, monSide);
    }
    canvas.fillRect(armX - cell, armY + cell * 2, cell * 3, cell, monFrame);

    // tapered face top/bottom
    int topLeft, topRight;
    if (yaw < 0) { topLeft = faceLeft + taper; topRight = faceRight; }
    else         { topLeft = faceLeft; topRight = faceRight - taper; }

    // per-row: interpolate from tapered top to full-width bottom
    for (int py = 0; py < faceH; py += cell) {
        float t = (float)py / (float)max(cell, faceH - cell);
        int rowLeft  = q((int)(topLeft  + (faceLeft  - topLeft)  * t));
        int rowRight = q((int)(topRight + (faceRight - topRight) * t));
        int rowY = faceY0 + py;
        int rowW = rowRight - rowLeft;
        if (rowW < cell * 2) continue;

        // side panel (gap between box edge and tapered face)
        if (yaw < 0 && rowLeft > mx) {
            for (int px = mx; px < rowLeft; px += cell) {
                uint16_t sc = bumpColor(monSide, px, rowY, 0x4DC0u + mx, monFrame, RP::DEEP, 0.08f);
                canvas.fillRect(px, rowY, cell, cell, sc);
            }
        } else if (yaw > 0 && rowRight < mx + mw) {
            for (int px = rowRight; px < mx + mw; px += cell) {
                uint16_t sc = bumpColor(monSide, px, rowY, 0x4DC0u + mx, monFrame, RP::DEEP, 0.08f);
                canvas.fillRect(px, rowY, cell, cell, sc);
            }
        }

        // face fill — dark base accepts accent light via screen blend
        for (int px = rowLeft; px < rowRight; px += cell) {
            uint16_t base = bumpColor(monBg, px, rowY, 0x4D00u + mx, monTop, monSide, 0.06f);
            base = Display::screenBlend565(base, accent, (uint8_t)((1.0f - t) * 18.0f));
            canvas.fillRect(px, rowY, cell, cell, base);
        }

        // left/right frame edges
        canvas.fillRect(rowLeft, rowY, cell, cell, monFrame);
        canvas.fillRect(rowRight - cell, rowY, cell, cell, monFrame);
    }

    // top plate
    for (int px = topLeft; px < topRight; px += cell) {
        uint16_t tc = bumpColor(monTop, px, my, 0x4D80u + mx, monFrame, monSide, 0.10f);
        canvas.fillRect(px, my, cell, cell, tc);
    }
    if (yaw < 0) canvas.fillRect(faceLeft - cell, my, cell, cell, monSide);
    else         canvas.fillRect(faceRight, my, cell, cell, monSide);

    // bottom frame
    for (int px = faceLeft; px < faceRight; px += cell)
        canvas.fillRect(px, faceY1 - cell, cell, cell, monFrame);

    // text + bars (inside tapered face)
    canvas.setTextSize(1);
    canvas.setTextColor(accent);
    canvas.setCursor(q(faceLeft + PX * 2), q(faceY0 + (faceY1 - faceY0) / 3));
    canvas.print(value);

    int barBase = q(faceY1 - PX * 2);
    int bars = max(2, (faceRight - faceLeft - PX * 3) / (PX * 2));
    for (int i = 0; i < bars; i++) {
        uint32_t hsh = wallHash(mx + i * 7, my, seed + now / 220UL);
        int lvl = 1 + (int)(hsh & 0x3u);
        for (int row = 0; row < lvl; row++) {
            glowPx(canvas, q(faceLeft + PX * 2 + i * PX * 2), barBase - row * PX, accent, (uint8_t)(70 + row * 22));
        }
    }

    if (((now / 320UL) + seed) & 1u) glowPx(canvas, q(faceRight - PX * 2), q(faceY0 + PX), accent, 120);
}

static void drawConsoleModule(M5Canvas& canvas, int x, int y, int w, int h,
                              uint16_t accent, uint32_t now, uint32_t seed) {
    uint16_t frame = Display::lerpColor565(RP::STRUCT, RP::FLUOR, 0.04f);
    uint16_t shell = Display::lerpColor565(RP::BG, RP::DEEP, 0.92f);
    uint16_t plate = Display::lerpColor565(shell, accent, 0.08f);
    uint16_t slit = Display::lerpColor565(RP::DEEP, RP::STRUCT, 0.24f);

    canvas.fillRect(x, y, w, h, frame);
    canvas.fillRect(x + PX, y + PX, w - PX * 2, h - PX * 2, shell);
    canvas.fillRect(x + PX, y + PX, w - PX * 2, PX, plate);
    canvas.fillRect(x + PX * 2, y + h - PX * 2, w - PX * 4, PX,
                    Display::lerpColor565(shell, RP::SHADOW_C, 0.28f));

    for (int py = y + PX * 2; py < y + h - PX * 2; py += PX) {
        for (int px = x + PX * 2; px < x + w - PX * 2; px += PX) {
            canvas.fillRect(px, py, PX, PX,
                bumpColor(plate, px, py, seed ^ 0x5A11u, accent, RP::DEEP, 0.08f));
        }
    }

    for (int slot = x + PX * 2; slot < x + w - PX * 3; slot += PX * 2) {
        canvas.fillRect(slot, y + PX * 3, PX, PX, slit);
    }
    for (int slot = x + PX * 3; slot < x + w - PX * 4; slot += PX * 3) {
        canvas.fillRect(slot, y + h - PX * 4, PX * 2, PX, slit);
    }

    int ledCols = max(2, (w - PX * 4) / (PX * 3));
    for (int i = 0; i < ledCols; i++) {
        int lx = x + PX * 2 + i * PX * 3;
        int ly = y + h - PX * 3;
        uint16_t tint = (i & 1) ? accent : WD_AMBER;
        if (((now / (160UL + i * 60UL)) + seed + i) & 1u) {
            glowPx(canvas, lx, ly, tint, 118);
        }
    }

    canvas.fillRect(x - PX, y + PX, PX, h - PX * 2, frame);
    canvas.fillRect(x + w, y + PX * 2, PX, h - PX * 3, RP::SHADOW_C);
}

static void drawConsoleElectronics(M5Canvas& canvas, uint32_t now) {
    // ==[ MON-A ]== small status brick — left of glass border (raised for shadow)
    drawConduitRun(canvas, RP::WALL_MID, 12, 132, 32);
    drawConsoleModule(canvas, 16, 136, 24, 16, WD_NEON, now, 0x6111u);
    drawPatchPanel(canvas, RP::STRUCT, RP::BG, 44, 140);

    // ==[ MON-B ]== wide command panel — center-left, raised
    drawConduitRun(canvas, RP::WALL_MID, 64, 128, 40);
    drawConsoleModule(canvas, 68, 132, 48, 28, WD_AMBER, now, 0x6222u);
    drawServiceCase(canvas, RP::STRUCT, RP::BG, 120, 144);
    drawWallOutlet(canvas, RP::WALL_MID, RP::BG, 56, 136);

    // ==[ MON-C ]== signal rack — chair armrest right, flanking pig
    drawConduitRun(canvas, RP::WALL_MID, 168, 128, 36);
    drawConsoleModule(canvas, 172, 132, 36, 24, WD_SPARK, now, 0x6333u);
    drawCableCoil(canvas, RP::WALL_MID, 212, 148);

    for (int i = 0; i < 2; i++) {
        int x = 136 + i * 8;
        glowPx(canvas, x, 152, (i & 1) ? WD_AMBER : WD_NEON, 84);
    }
    for (int i = 0; i < 2; i++) {
        glowPx(canvas, 176 + i * 8, 156, WD_NEON, (uint8_t)(92 - i * 12));
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// COCKPIT SHELL FUNCTIONS
// ═══════════════════════════════════════════════════════════════════════════

// ==[ PANEL LIGHTING ]== directional light on frame borders
uint16_t litFrame(uint16_t frame, uint16_t glow, int px, int py, int cx, int cy, float halfW, float halfH) {
    float nx = (float)(px - cx) / fmaxf(1.0f, halfW);
    float ny = (float)(py - cy) / fmaxf(1.0f, halfH);
    float r2 = nx * nx + ny * ny;
    float nz = (r2 < 1.0f) ? sqrtf(1.0f - r2) : 0.05f;
    float diffuse = nx * LIGHT_X + ny * LIGHT_Y + nz * LIGHT_Z;
    if (diffuse < 0.0f) diffuse = 0.0f;
    float brightness = AMBIENT + diffuse * 0.97f;
    if (brightness > 1.0f) brightness = 1.0f;
    uint16_t dark = Display::lerpColor565(frame, RP::DEEP, 0.55f);
    if (brightness < 0.50f)
        return Display::lerpColor565(dark, frame, brightness / 0.50f);
    return Display::lerpColor565(frame, glow, (brightness - 0.50f) / 0.50f * 0.35f);
}

// ==[ DASHBOARD CABLES ]==
void drawDashboardCables(M5Canvas& canvas, uint32_t now, float motion) {
    float swayA = sinf((float)now * 0.0016f) * (4.0f + motion * 3.0f);
    float swayB = sinf((float)now * 0.0014f + 0.8f) * (3.0f + motion * 2.0f);
    drawCableRun(canvas, q(CAB_A_X + CAB_A_W / 2), CAB_A_Y, CAB_A_H, swayA, WD_NEON, now, 0xA1u);
    drawCableRun(canvas, q(CAB_B_X + CAB_B_W / 2), CAB_B_Y, CAB_B_H, swayB, WD_NEON, now, 0xB2u);
    drawCableRun(canvas, q(CAB_C_X + CAB_C_W / 2), CAB_C_Y, CAB_C_H, -swayA * 0.8f, WD_AMBER, now, 0xC3u);
    drawCableRun(canvas, q(CAB_D_X + CAB_D_W / 2), CAB_D_Y, CAB_D_H, -swayB * 0.7f, WD_AMBER, now, 0xD4u);
}

// ==[ DASHBOARD MONITORS ]==
void drawDashboardMonitors(M5Canvas& canvas, uint32_t now) {
    char monB[12];
    float dspd = getDisplaySpeedKmh();
    if (dspd < 0.0f) snprintf(monB, sizeof(monB), "--KM");
    else snprintf(monB, sizeof(monB), "%02uKM", (unsigned)(dspd + 0.5f));
    drawAuxMonitor(canvas, MON_B_X, MON_B_Y, MON_B_W, MON_B_H, WD_AMBER, monB, now, 0x4202u, MON_B_YAW);
}

// ==[ CONSOLE LAYER ]==
void drawConsoleLayer(M5Canvas& canvas, uint32_t now) {
    drawConsoleElectronics(canvas, now);
    // no ambient light pools — terminals visible only from their own emitted LEDs
}

// ==[ COCKPIT SHELL ]== canopy glass, pillars, roof, dash (LAYER 1)
void drawCockpitShell(M5Canvas& canvas, uint32_t now, float motion) {
    uint16_t roofCol     = Display::lerpColor565(RP::BG, RP::DEEP, 0.30f);
    uint16_t shellCol    = Display::lerpColor565(RP::BG, RP::DEEP, 0.25f);
    uint16_t shellShadow = Display::lerpColor565(RP::BG, RP::DEEP, 0.20f);
    uint16_t edgeCol     = Display::lerpColor565(RP::BG, RP::DEEP, 0.40f);
    uint16_t innerTrim   = Display::lerpColor565(RP::DEEP, RP::STRUCT, 0.03f);
    uint16_t dashCol     = Display::lerpColor565(RP::BG, RP::DEEP, 0.22f);
    uint16_t dashHot     = Display::lerpColor565(RP::DEEP, RP::STRUCT, 0.03f);
    uint16_t sillCol     = Display::lerpColor565(RP::BG, RP::DEEP, 0.28f);
    uint16_t sillHi      = Display::lerpColor565(RP::DEEP, RP::STRUCT, 0.05f);
    uint16_t slitCol     = Display::lerpColor565(RP::BG, RP::DEEP, 0.32f);
    uint16_t bridgeGlow = Display::lerpColor565(WD_AMBER, WD_NEON, 0.35f);

    // roof / canopy top — fills gap between status bar and glass
    int roofBot = min(WD_DASH_T - PX * 2, glassOpenTop());
    for (int y = WD_PLAY_T; y < roofBot; y += PX) {
        canvas.fillRect(0, y, 320, PX, roofCol);
        int phase = (int)((wallHash(0, y, 0xE709u) & 3u)) * PX;
        for (int x = phase; x < 320; x += PX * 6)
            canvas.fillRect(x, y, PX, PX,
                bumpColor(roofCol, x, y, 0xE709u, edgeCol, RP::BG, 0.08f));
    }
    canvas.fillRect(0, roofBot - PX, 320, PX, edgeCol);
    // dither below roof edge — 2 rows of ordered dissolve into glass
    for (int d = 0; d < 2; d++) {
        int dy = roofBot + d * PX;
        if (dy >= WD_GLASS_B) break;
        int left, right;
        glassBounds(dy, left, right);
        uint8_t threshold = (d == 0) ? 160 : 64;
        uint16_t ditherCol = Display::lerpColor565(shellCol, edgeCol, 0.25f - d * 0.12f);
        for (int x = left; x < right; x += PX) {
            if (bayer4[(dy / PX) & 3][(x / PX) & 3] < threshold)
                canvas.fillRect(x, dy, PX, PX, ditherCol);
        }
    }
    const int cableMounts[] = {
        q(CAB_D_X + CAB_D_W / 2), q(CAB_A_X + CAB_A_W / 2),
        q(CAB_B_X + CAB_B_W / 2), q(CAB_C_X + CAB_C_W / 2)
    };
    for (int i = 0; i < 4; i++) {
        int mx = cableMounts[i];
        canvas.fillRect(mx - PX, roofBot - PX * 2, PX * 3, PX,
                        Display::lerpColor565(roofCol, RP::STRUCT, 0.22f));
        if (((now / (220UL + i * 40UL)) + i) & 1u) glowPx(canvas, mx, roofBot - PX * 2, WD_NEON, 52);
    }

    // outside-glass shell + layered A-pillars
    for (int y = WD_GLASS_T; y < WD_GLASS_B; y += PX) {
        int left, right;
        glassBounds(y, left, right);
        float t = glassRowTAtY(y);
        if (left > 0) {
            canvas.fillRect(0, y, left, PX, shellCol);
            for (int x = (int)((wallHash(0, y, 0xE205u) & 3u)) * PX; x < left; x += PX * 5)
                canvas.fillRect(x, y, PX, PX,
                    bumpColor(shellCol, x, y, 0xE205u, edgeCol, roofCol, 0.10f));
        }
        if (right < 320) {
            canvas.fillRect(right, y, 320 - right, PX, shellCol);
            for (int x = right + (int)((wallHash(320, y, 0xE205u) & 3u)) * PX; x < 320; x += PX * 5)
                canvas.fillRect(x, y, PX, PX,
                    bumpColor(shellCol, x, y, 0xE205u, edgeCol, roofCol, 0.10f));
        }
        int leftEdge = clampi(left - PX, 0, 316);
        int rightEdge = clampi(right, 0, 316);
        canvas.fillRect(leftEdge, y, PX, PX, edgeCol);
        canvas.fillRect(rightEdge, y, PX, PX, edgeCol);
        if (leftEdge - PX >= 0) canvas.fillRect(leftEdge - PX, y, PX, PX, shellShadow);
        if (rightEdge + PX < 320) canvas.fillRect(rightEdge + PX, y, PX, PX, shellShadow);

        // dither transition — 2-cell gradient between shell and glass
        for (int d = 0; d < 2; d++) {
            uint8_t threshold = (d == 0) ? 192 : 96;  // inner = sparser, outer = denser
            int dlx = leftEdge - PX * (d + 2);
            int drx = rightEdge + PX * (d + 1);
            if (dlx >= 0) {
                uint8_t bay = bayer4[(y / PX) & 3][(dlx / PX) & 3];
                if (bay < threshold)
                    canvas.fillRect(dlx, y, PX, PX, Display::lerpColor565(shellCol, edgeCol, 0.30f - d * 0.15f));
            }
            if (drx < 316) {
                uint8_t bay = bayer4[(y / PX) & 3][(drx / PX) & 3];
                if (bay < threshold)
                    canvas.fillRect(drx, y, PX, PX, Display::lerpColor565(shellCol, edgeCol, 0.30f - d * 0.15f));
            }
        }

        if (t > 0.10f && t < 0.82f && ((y / PX) & 3) == 0) {
            glowPx(canvas, leftEdge, y, innerTrim, (uint8_t)(26 + (1.0f - fabsf(t - 0.48f) * 1.8f) * 18.0f));
        }
        if (t > 0.14f && t < 0.74f && ((y / PX) & 7) == 0) {
            glowPx(canvas, rightEdge, y, innerTrim, (uint8_t)(16 + (1.0f - fabsf(t - 0.52f) * 1.8f) * 12.0f));
        }
    }

    // windshield sill / glare shield — separate mass from the main dashboard
    for (int y = WD_DASH_T - PX * 2; y < WD_DASH_T + PX * 2; y += PX) {
        float t = clamp01((float)(y - (WD_DASH_T - PX * 2)) / (float)(PX * 4));
        int inset = q(28 + (int)(12.0f * t));
        uint16_t rowCol = Display::lerpColor565(dashCol, RP::SHADOW_C, t * 0.35f);
        rowCol = Display::lerpColor565(sillCol, rowCol, t * 0.55f);
        int rowW = 320 - inset * 2;
        canvas.fillRect(inset, y, rowW, PX, rowCol);
        canvas.fillRect(inset, y, PX, PX, sillHi);
        canvas.fillRect(320 - inset - PX, y, PX, PX, sillHi);
        int notchHalf = max(PX * 2, q(14 - (int)(4.0f * t)));
        canvas.fillRect(160 - notchHalf, y, notchHalf * 2, PX,
                        Display::lerpColor565(rowCol, RP::DEEP, 0.20f));
    }
    canvas.fillRect(36, WD_DASH_T - PX, 248, PX, edgeCol);
    // dither above sill edge — 2 rows of ordered dissolve into glass
    for (int d = 0; d < 2; d++) {
        int dy = WD_DASH_T - PX * (d + 2);
        if (dy < WD_GLASS_T) break;
        int left, right;
        glassBounds(dy, left, right);
        uint8_t threshold = (d == 0) ? 128 : 48;
        uint16_t ditherCol = Display::lerpColor565(sillCol, edgeCol, 0.20f - d * 0.10f);
        for (int x = left + PX; x < right - PX; x += PX) {
            if (bayer4[(dy / PX) & 3][(x / PX) & 3] < threshold)
                canvas.fillRect(x, dy, PX, PX, ditherCol);
        }
    }
    for (int x = 44; x < 276; x += PX * 3) {
        if (x > (PIG_CX - PX * 2) && x < (PIG_CX + PX * 10)) continue;
        canvas.fillRect(x, WD_DASH_T, PX * 2, PX, slitCol);
    }

    // dashboard body — the lower bulk that supports the instrument bridge and side consoles
    for (int y = WD_DASH_T + PX; y < WD_DASH_B; y += PX) {
        float t = (float)(y - WD_DASH_T) / (float)(WD_DASH_B - WD_DASH_T);
        int inset = q(16 + (int)(40.0f * t));
        uint16_t rowCol = Display::lerpColor565(dashCol, RP::SHADOW_C, 0.10f + t * 0.26f);
        int rowW = 320 - inset * 2;
        canvas.fillRect(inset, y, rowW, PX, rowCol);
        int phase = (int)((wallHash(0, y, 0xD104u) & 3u)) * PX;
        for (int x = inset + phase; x < inset + rowW; x += PX * 5) {
            canvas.fillRect(x, y, PX, PX,
                bumpColor(rowCol, x, y, 0xD104u, dashHot, RP::DEEP, 0.10f));
        }
        int troughHalf = q(18 + (int)((1.0f - t) * 12.0f));
        canvas.fillRect(160 - troughHalf, y, troughHalf * 2, PX,
                        Display::lerpColor565(rowCol, RP::DEEP, 0.16f + (1.0f - t) * 0.08f));
    }
    canvas.fillRect(24, WD_DASH_T + PX, 272, PX, edgeCol);

    // left console — shallow pilot-access shelf, tighter toward the footwell
    for (int y = WD_DASH_T; y < WD_DASH_B - PX * 2; y += PX) {
        float t = (float)(y - WD_DASH_T) / (float)(WD_DASH_B - WD_DASH_T);
        int x = q(8 + (int)(20.0f * t));
        int w = q(104 - (int)(28.0f * t));
        uint16_t conCol = Display::lerpColor565(dashCol, RP::SHADOW_C, 0.18f + t * 0.22f);
        canvas.fillRect(x, y, w, PX, conCol);
        int phase = (int)((wallHash(0, y, 0xD507u) & 3u)) * PX;
        for (int cx2 = x + phase; cx2 < x + w; cx2 += PX * 3) {
            canvas.fillRect(cx2, y, PX, PX,
                bumpColor(conCol, cx2, y, 0xD507u, dashHot, RP::DEEP, 0.08f));
        }
        if (y > WD_DASH_T + PX * 4 && y < WD_DASH_T + PX * 10) {
            shadePx(canvas, x + w - PX * 2, y, 0.10f);
        }
    }

    // right console / seat pocket — slightly deeper, with a stronger inner shadow
    for (int y = WD_DASH_T - PX; y < WD_DASH_B; y += PX) {
        float t = clamp01((float)(y - (WD_DASH_T - PX)) / (float)(WD_DASH_B - WD_DASH_T + PX));
        int x = q(200 + (int)(16.0f * t));
        int w = q(108 - (int)(24.0f * t));
        uint16_t conCol = Display::lerpColor565(dashCol, RP::SHADOW_C, 0.14f + t * 0.20f);
        canvas.fillRect(x, y, w, PX, conCol);
        int phase = (int)((wallHash(0, y, 0xD608u) & 3u)) * PX;
        for (int cx2 = x + phase; cx2 < x + w; cx2 += PX * 3) {
            canvas.fillRect(cx2, y, PX, PX,
                bumpColor(conCol, cx2, y, 0xD608u, dashHot, RP::DEEP, 0.08f));
        }
        if (y > WD_DASH_T + PX * 4 && y < WD_DASH_T + PX * 10) {
            shadePx(canvas, x + PX, y, 0.10f);
        }
    }

    // center instrument bridge — the one place where the eye should land first
    uint16_t cowlOuter = Display::lerpColor565(dashCol, RP::STRUCT, 0.12f);
    uint16_t cowlInner = Display::lerpColor565(cowlOuter, shellShadow, 0.10f);
    canvas.fillRect(148, 164, 64, 24, cowlOuter);
    canvas.fillRect(156, 168, 48, 20, cowlInner);
    canvas.fillRect(160, 172, 40, 12, Display::lerpColor565(RP::DEEP, WD_AMBER, 0.08f));
    canvas.fillRect(148, 188, 64, PX, edgeCol);
    // cowl accent line — shelf edge catching light
    for (int ax = 148; ax < 212; ax += PX) glowPx(canvas, ax, 164, WD_AMBER, 50);
    for (int i = 0; i < 4; i++) glowPx(canvas, 164 + i * 8, 176, (i & 1) ? WD_AMBER : WD_NEON, 130);
    for (int i = 0; i < 3; i++) {
        glowPx(canvas, 168 + i * 8, 184, bridgeGlow, (uint8_t)(70 + i * 18));
    }

    // deliberate control banks — logical placement beats random blink noise
    static const int kLeftBank[][2] = {
        { 32, 176 }, { 40, 180 }, { 48, 184 }, { 56, 188 }, { 64, 192 }
    };
    static const int kRightBank[][2] = {
        { 236, 176 }, { 244, 180 }, { 252, 184 }, { 260, 188 }, { 268, 192 }
    };
    for (int i = 0; i < 5; i++) {
        if (((now / (600UL + i * 80UL)) + i) & 1u) {
            glowPx(canvas, kLeftBank[i][0], kLeftBank[i][1],
                   (i & 1) ? WD_LED : WD_NEON, (uint8_t)(104 + i * 6));
        }
        if (((now / (640UL + i * 70UL)) + i + 1) & 1u) {
            glowPx(canvas, kRightBank[i][0], kRightBank[i][1],
                   (i & 1) ? WD_SPARK : WD_AMBER, (uint8_t)(92 + i * 8));
        }
    }
    for (int i = 0; i < 6; i++) {
        int slotX = 108 + i * 12;
        int slotY = 208 + ((i & 1) ? PX : 0);
        canvas.fillRect(slotX, slotY, PX * 2, PX, slitCol);
        if (((now / (260UL + i * 20UL)) + i) & 1u) {
            glowPx(canvas, slotX + PX, slotY, WD_AMBER, (uint8_t)(72 + i * 8));
        }
    }
    for (int i = 0; i < 3; i++) {
        int slotX = 36 + i * 12;
        canvas.fillRect(slotX, 204, PX * 2, PX, slitCol);
        if (((now / (300UL + i * 41UL)) + i + 2) & 1u) glowPx(canvas, slotX + PX, 204, WD_NEON, 70);
    }
    for (int i = 0; i < 3; i++) {
        int slotX = 236 + i * 12;
        canvas.fillRect(slotX, 204, PX * 2, PX, slitCol);
        if (((now / (280UL + i * 37UL)) + i + 3) & 1u) glowPx(canvas, slotX + PX, 204, WD_SPARK, 68);
    }

    // ==[ LOWER DASH DETAIL ]== rivet line + vent grill in the extra space
    uint16_t rivetCol = Display::lerpColor565(RP::STRUCT, RP::FLUOR, 0.06f);
    int rivetY = q(WD_DASH_B - 24);
    float rivetT = (float)(rivetY - WD_DASH_T) / (float)(WD_DASH_B - WD_DASH_T);
    int rivetInset = q(16 + (int)(34.0f * rivetT));
    for (int x = rivetInset; x < 320 - rivetInset; x += PX * 4) {
        glowPx(canvas, x, rivetY, rivetCol, 70);
    }

    canvas.fillRect(48, WD_DASH_B - 16, 224, PX * 2,
                    Display::lerpColor565(RP::DEEP, RP::SHADOW_C, 0.28f));

    // vent slits above button hint zone
    uint16_t ventCol = Display::lerpColor565(RP::DEEP, RP::SHADOW_C, 0.60f);
    int ventY = q(WD_DASH_B - 20);
    float ventT = (float)(ventY - WD_DASH_T) / (float)(WD_DASH_B - WD_DASH_T);
    int ventInset = q(20 + (int)(34.0f * ventT));
    for (int x = ventInset + PX; x < 320 - ventInset - PX; x += PX * 3) {
        canvas.fillRect(x, ventY, PX * 2, PX, ventCol);
    }
}

// ==[ YOKE ]== blade runner butterfly flight stick — U-shaped with lit grips
void drawSteeringWheel(M5Canvas& canvas, int x, int y, int w, int h) {
    uint16_t frame = RP::STRUCT;
    uint16_t glow = Display::lerpColor565(WD_AMBER, RP::FLUOR, 0.20f);
    uint16_t dark = Display::lerpColor565(RP::DEEP, RP::SHADOW_C, 0.50f);
    uint16_t accent = Display::lerpColor565(WD_NEON, RP::FLUOR, 0.30f);
    uint16_t gripHi = Display::lerpColor565(RP::STRUCT, RP::FLUOR, 0.18f);
    int cx = q(x + w / 2);
    int cy = q(y + h / 2);
    int halfW = w / 2;
    int halfH = h / 2;
    float phw = (float)halfW;
    float phh = (float)halfH;

    // IMU rotation applied to yoke angle
    float yokeAngle = steerAngle * 0.25f;  // subtle tilt

    // ==[ CROSS BAR ]== horizontal center beam with lighting
    int barY = q(cy - PX);
    int barL = q(cx - halfW + PX * 2);
    int barR = q(cx + halfW - PX * 2);
    for (int px = barL; px <= barR; px += PX) {
        uint16_t c = litFrame(frame, glow, px, barY, cx, cy, phw, phh);
        canvas.fillRect(px, barY, PX, PX, bumpColor(c, px, barY, 0xF306u, glow, dark, 0.12f));
        // thinner shadow below
        canvas.fillRect(px, barY + PX, PX, PX, Display::lerpColor565(dark, RP::BG, 0.30f));
    }

    // ==[ LEFT HORN ]== upward prong with rounded tip
    int hornH = max(PX * 3, halfH - PX);
    for (int s = 0; s < hornH; s += PX) {
        float t = (float)s / (float)max(PX, hornH - PX);
        int hx = q(barL + (int)(PX * 2.0f * t * yokeAngle));  // yoke tilt
        int hy = q(barY - s);
        if (hy < WD_PLAY_T || hy >= WD_PLAY_B) continue;
        uint16_t c = litFrame(frame, glow, hx, hy, cx, cy, phw, phh);
        canvas.fillRect(hx, hy, PX, PX, bumpColor(c, hx, hy, 0xF307u, glow, dark, 0.12f));
        // taper: narrower at top
        if (t < 0.4f) canvas.fillRect(hx + PX, hy, PX, PX,
            bumpColor(Display::lerpColor565(c, dark, 0.20f), hx + PX, hy, 0xF308u, glow, dark, 0.10f));
    }
    // left tip glow
    glowPx(canvas, q(barL + (int)(PX * 2.0f * yokeAngle)), q(barY - hornH + PX), accent, 140);

    // ==[ RIGHT HORN ]== upward prong
    for (int s = 0; s < hornH; s += PX) {
        float t = (float)s / (float)max(PX, hornH - PX);
        int hx = q(barR - (int)(PX * 2.0f * t * yokeAngle));
        int hy = q(barY - s);
        if (hy < WD_PLAY_T || hy >= WD_PLAY_B) continue;
        uint16_t c = litFrame(frame, glow, hx, hy, cx, cy, phw, phh);
        canvas.fillRect(hx, hy, PX, PX, bumpColor(c, hx, hy, 0xF309u, glow, dark, 0.12f));
        if (t < 0.4f) canvas.fillRect(hx - PX, hy, PX, PX,
            bumpColor(Display::lerpColor565(c, dark, 0.20f), hx - PX, hy, 0xF30Au, glow, dark, 0.10f));
    }
    glowPx(canvas, q(barR - (int)(PX * 2.0f * yokeAngle)), q(barY - hornH + PX), accent, 140);

    // ==[ COLUMN ]== downward center stem
    int stemH = max(PX * 2, halfH - PX);
    for (int s = PX; s < stemH; s += PX) {
        int sy = q(barY + PX * 2 + s);
        if (sy >= WD_PLAY_B) break;
        uint16_t c = litFrame(dark, glow, cx, sy, cx, cy, phw, phh);
        canvas.fillRect(cx, sy, PX, PX, bumpColor(c, cx, sy, 0xF30Bu, gripHi, dark, 0.10f));
    }

    // ==[ CENTER HUB ]== lit control pad
    uint16_t hubCol = litFrame(frame, glow, cx, barY, cx, cy, phw, phh);
    canvas.fillRect(cx - PX, barY - PX, PX * 3, PX * 3, hubCol);
    glowPx(canvas, cx, barY, accent, 160);
    // indicator dots on hub
    glowPx(canvas, cx - PX, barY - PX, WD_NEON, 80);
    glowPx(canvas, cx + PX, barY - PX, WD_AMBER, 80);

    // ==[ GRIP WRAPS ]== textured grip sections on horns
    for (int g = 0; g < 3; g++) {
        int gy = q(barY - PX * 2 - g * PX);
        if (gy < WD_PLAY_T) break;
        glowPx(canvas, barL, gy, gripHi, (uint8_t)(60 + g * 12));
        glowPx(canvas, barR, gy, gripHi, (uint8_t)(60 + g * 12));
    }
}

// ==[ CHAIR ]== dark cyberpunk racing seat — smooth shell, no harness
static uint16_t chairLitMaterial(float relX, float relY, float halfW, float halfH, uint32_t seed,
                                 uint16_t chDark, uint16_t chMid, uint16_t chLit, uint16_t chHi) {
    float nx = relX / fmaxf(1.0f, halfW);
    float ny = relY / fmaxf(1.0f, halfH);
    float r2 = nx * nx + ny * ny;
    float nz = 0.0f;

    // One smooth normal field owns the molded shell. Hash-perturbing nx/ny here
    // produced bright diagonal ridges that read as a four-point safety harness.
    // The hemisphere is unit length by construction; only skirt samples need
    // normalization, keeping this hot loop to at most one square root per cell.
    if (r2 < 1.0f) {
        nz = sqrtf(1.0f - r2);
    } else if (r2 > 1.0f) {
        float invLen = 1.0f / sqrtf(r2);
        nx *= invLen;
        ny *= invLen;
    }

    float diffuse = nx * LIGHT_X + ny * LIGHT_Y + nz * LIGHT_Z;
    if (diffuse < 0.0f) diffuse = 0.0f;

    // Pre-normalized Blinn-Phong H = normalize(L + V), V ~= (0, 0, 1).
    static constexpr float HALF_X = -0.09512f;
    static constexpr float HALF_Y = -0.44395f;
    static constexpr float HALF_Z =  0.89099f;
    float spec = nx * HALF_X + ny * HALF_Y + nz * HALF_Z;
    if (spec < 0.0f) spec = 0.0f;
    spec *= spec;
    spec *= spec;
    spec *= spec;  // pow8 — small sheen, not a strap-shaped lobe

    float rimF = 1.0f - nz;
    if (rimF < 0.0f) rimF = 0.0f;
    rimF *= rimF;

    float grain = ((float)(wallHash((int)(relX * 17.0f), (int)(relY * 17.0f), seed) & 0xFFu) / 255.0f - 0.5f) * 0.04f;
    float brightness = clamp01(AMBIENT + diffuse * 0.38f + spec * 0.04f + rimF * 0.06f + grain);

    uint16_t c;
    if (brightness < 0.32f)
        c = Display::lerpColor565(chDark, chMid, brightness / 0.32f);
    else if (brightness < 0.75f)
        c = Display::lerpColor565(chMid, chLit, (brightness - 0.32f) / 0.43f);
    else
        c = Display::lerpColor565(chLit, chHi, (brightness - 0.75f) / 0.25f);

    return c;
}

void drawChairPass(M5Canvas& canvas, const PigPose& pose, bool foreground) {
    int cell = objectGrid();
    int cx = q(PIG_CX + CHAIR_DX);                   // no steer — chair stays centered
    int cy = q(PIG_TOP_Y + CHAIR_DY) + pose.bobY;    // breathing bob only

    // chair colors: theme-derived — dark cyberpunk noir seat
    uint16_t chDark = Display::lerpColor565(RP::BG, RP::DEEP, 0.70f);
    uint16_t chMid  = Display::lerpColor565(RP::DEEP, RP::WALL_MID, 0.20f);
    uint16_t chLit  = Display::lerpColor565(RP::DEEP, RP::WALL_MID, 0.55f);
    uint16_t chHi   = Display::lerpColor565(RP::WALL_MID, RP::STRUCT, 0.12f);
    uint16_t frameDk= Display::lerpColor565(RP::BG, RP::DEEP, 0.85f);

    int headW = CHAIR_HEAD_W;
    int headH = CHAIR_HEAD_H;
    int backWTop = CHAIR_BACK_W_TOP;
    int backWMid = CHAIR_BACK_W_MID;
    int backH = CHAIR_BACK_H;
    int baseW = CHAIR_BASE_W;
    int railW = CHAIR_RAIL_W;

    int headX = cx - headW / 2;
    int neckY = cy + headH;
    int neckW = max(cell * 2, headW / 5 / cell * cell);
    int backY = neckY + cell;

    if (!foreground) {
        // ==[ HEADREST + SEAT CORE ]== behind Pancetta's silhouette
        for (int row = 0; row < headH; row += cell) {
            for (int col = 0; col < headW; col += cell) {
                float relX = (col + cell * 0.5f) - headW * 0.5f;
                float relY = (row + cell * 0.5f) - headH * 0.5f;
                uint16_t c = chairLitMaterial(relX, relY, headW * 0.55f, headH * 0.85f, 0xC701u,
                                              chDark, chMid, chLit, chHi);
                canvas.fillRect(headX + col, cy + row, cell, cell, c);
            }
        }
        canvas.fillRect(cx - neckW / 2, neckY, neckW, cell, chMid);
    }

    // ==[ SEAT BACK ]== core behind pig; paired bolsters wrap in foreground
    for (int row = 0; row < backH; row += cell) {
        float t = (float)row / (float)max(cell, backH - cell);
        int rowW = (int)ceilf((backWTop + (backWMid - backWTop) * fastSinf(t * PI_F * 0.5f)) / cell) * cell;
        int rowX = cx - rowW / 2;

        // Paired bolsters frame the flanks; the left face catches more glass light.
        if (foreground && row > cell && t < 0.85f) {
            float bT = clamp01((t - 0.05f) * 1.2f);
            int bolW = (bT < 0.55f) ? cell : CHAIR_BOLSTER_MAX_W;
            uint16_t left = chairLitMaterial(-rowW * 0.6f,
                                             (row + cell * 0.5f) - backH * 0.5f,
                                             rowW * 0.5f, backH * 0.72f,
                                             0xC780u,
                                             chDark, chMid, chLit, chHi);
            uint16_t right = chairLitMaterial(rowW * 0.6f,
                                              (row + cell * 0.5f) - backH * 0.5f,
                                              rowW * 0.5f, backH * 0.72f,
                                              0xC7C0u,
                                              chDark, chMid, chLit, chHi);
            left = Display::lerpColor565(left, chDark, 0.28f);
            right = Display::lerpColor565(right, chDark, 0.48f);
            for (int bx = 0; bx < bolW; bx += cell) {
                canvas.fillRect(rowX - bolW + bx, backY + row, cell, cell, left);
                canvas.fillRect(rowX + rowW + bx, backY + row, cell, cell, right);
            }
        }

        if (foreground) continue;

        for (int col = 0; col < rowW; col += cell) {
            float relX = (col + cell * 0.5f) - rowW * 0.5f;
            float relY = (row + cell * 0.5f) - backH * 0.5f;
            uint16_t c = chairLitMaterial(relX, relY, rowW * 0.65f, backH * 0.72f, 0xC702u,
                                          chDark, chMid, chLit, chHi);
            if (col < cell) c = Display::lerpColor565(c, chLit, 0.18f);  // left edge catches light
            if (col >= rowW - cell) c = Display::lerpColor565(c, chDark, 0.42f);
            canvas.fillRect(rowX + col, backY + row, cell, cell, c);
        }
    }

    if (!foreground) return;

    // ==[ BASE + RAIL ]== foreground frame below the pig
    int baseY = backY + backH;
    int baseX = cx - baseW / 2;
    for (int col = 0; col < baseW; col += cell) {
        float relX = (col + cell * 0.5f) - baseW * 0.5f;
        uint16_t c = chairLitMaterial(relX, 0.0f, baseW * 0.7f, cell * 1.1f, 0xC799u,
                                      chDark, chMid, chLit, chHi);
        c = Display::lerpColor565(c, chMid, 0.10f);
        canvas.fillRect(baseX + col, baseY, cell, cell, c);
    }

    // ==[ RAIL ]==
    int railX = cx - railW / 2;
    canvas.fillRect(railX, baseY + cell, railW, cell, frameDk);
    for (int col = 0; col < railW; col += cell) {
        float relX = (col + cell * 0.5f) - railW * 0.5f;
        uint16_t rc = chairLitMaterial(relX, 0.0f, railW * 0.75f, (float)cell, 0xCA55u,
                                       chDark, chMid, chLit, chHi);
        rc = Display::lerpColor565(frameDk, rc, 0.45f);
        canvas.fillRect(railX + col, baseY + cell, cell, cell, rc);
        canvas.fillRect(railX + col, baseY + cell * 2, cell, cell,
                        Display::lerpColor565(rc, RP::BG, 0.55f));
    }

}

// ==[ DASH REFLECTIONS ]== neon/amber reflection streaks on dash and glass
void drawDashReflections(M5Canvas& canvas, uint32_t now, float motion) {
    uint32_t stripeStep = 180UL;
    if (motion > 0.10f) stripeStep = 88UL;
    if (motion > 0.45f) stripeStep = 48UL;

    uint16_t glassPeak = cockpitGlassPeak();
    uint16_t leftTint = Display::lerpColor565(WD_NEON, glassPeak, 0.22f);
    uint16_t rightTint = Display::lerpColor565(WD_AMBER, glassPeak, 0.20f);
    uint16_t bridgeTint = Display::lerpColor565(WD_AMBER, WD_NEON, 0.45f);

    for (uint8_t i = 0; i < 5; i++) {
        int y = 172 + i * PX * 2;
        float dt = clamp01((float)(y - WD_DASH_T) / (float)(WD_DASH_B - WD_DASH_T));
        int inset = q(16 + (int)(40.0f * dt));
        int centerGap = q(40 - (int)(dt * 12.0f));
        int leftLimit = 160 - centerGap / 2;
        int rightStart = 160 + centerGap / 2;

        int leftSpan = max(PX * 3, leftLimit - inset);
        int rightSpan = max(PX * 3, (320 - inset) - rightStart);
        uint32_t phase = (now / (stripeStep + 16UL)) * (1u + (i & 1u)) + i * 43u;
        int xL = q(inset + (int)(phase % (uint32_t)leftSpan));
        int xR = q(rightStart + (int)(phase % (uint32_t)rightSpan));

        for (int s = 0; s < 4; s++) {
            int sx = xL + s * 8;
            if (sx >= inset && sx < leftLimit) {
                glowPx(canvas, sx, y, leftTint, (uint8_t)max(0, 60 - s * 14 + i * 4));
            }
            sx = xR + s * 8;
            if (sx >= rightStart && sx < 320 - inset) {
                glowPx(canvas, sx, y, rightTint, (uint8_t)max(0, 56 - s * 12 + i * 6));
            }
        }
        if (i < 3) {
            glowPx(canvas, 160 + (i - 1) * 8, y, bridgeTint, (uint8_t)(30 + i * 8));
        }
    }
}

// ==[ CRT SCANLINES ]== dashboard scanlines — sparse, low cost.
void drawCRTScanlines(M5Canvas& canvas) {
    for (int y = WD_DASH_T + PX * 3; y < WD_DASH_B - PX; y += PX * 4) {
        float t = (float)(y - WD_DASH_T) / (float)(WD_DASH_B - WD_DASH_T);
        int inset = q(12 + (int)(34.0f * t));
        for (int x = inset + PX; x < 320 - inset - PX; x += PX * 2) {
            shadePx(canvas, x, y, 0.04f);
        }
    }
}

// ==[ DASH HINTS ]== button labels etched into bottom dashboard as dim HUD text
void drawDashHints(M5Canvas& canvas, uint32_t now) {
    uint16_t hudDark = Display::isInvertedTheme() ? Display::getColorFG() : Display::getColorBG();
    uint16_t hintCol = Display::lerpColor565(WD_AMBER, RP::SHADOW_C, 0.55f);
    uint16_t hintShadow = Display::lerpColor565(hudDark, RP::DEEP, 0.10f);
    canvas.setTextSize(1);

    // left hint: pause/resume
    const char* leftHint = Wardrive::isPaused() ? "B:RESUME" : "B:PAUSE";
    canvas.setTextColor(hintShadow);
    canvas.setCursor(28, 232);
    canvas.print(leftHint);
    canvas.setTextColor(hintCol);
    canvas.setCursor(24, 228);
    canvas.print(leftHint);

    // right hint: exit
    canvas.setTextColor(hintShadow);
    canvas.setCursor(268, 232);
    canvas.print("C:EXIT");
    canvas.setTextColor(hintCol);
    canvas.setCursor(264, 228);
    canvas.print("C:EXIT");

    // subtle blink indicator between hints — comms activity
    if (((now / 800UL) & 1u) && !Wardrive::isPaused()) {
        glowPx(canvas, 156, 232, WD_NEON, 60);
    }
}

} // namespace WardriveScene
