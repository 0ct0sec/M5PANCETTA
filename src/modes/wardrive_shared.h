/**
 * wardrive_shared.h — shared types, constants, and state for WARTHOG cockpit
 *
 * Extracted from wardrive_scene.cpp (4769 lines → 8 modules).
 * All modules include this header to share:
 *   - Scene geometry constants (glass, dash, pig, chair)
 *   - Cockpit accent colors (WD_NEON, WD_AMBER, etc.)
 *   - Shared structs (PigPose, CityTowerSlot, CityPalette, etc.)
 *   - State variables that multiple modules read/write
 *   - Helper functions (q, clampi, lerpi, accentWhite, etc.)
 *
 * This header is NOT a standalone module — it is included by all wardrive_*.cpp files.
 * wardrive_scene.cpp includes it once and defines the actual state.
 */
#pragma once

#include <M5Unified.h>
#include "../ui/display.h"
#include "../ui/menu_pig_render.h"
#include "../ui/teleport.h"
#include "../piglet/pig_face_timer.h"
#include <math.h>
#include <string.h>

namespace WardriveScene {

using namespace MenuPigRender;

// ═══════════════════════════════════════════════════════════════════════════
// GRID & SCENE GEOMETRY
// ═══════════════════════════════════════════════════════════════════════════

static constexpr int PX = 4;
static constexpr int CPX = PX * 3;  // city detail pixel
static constexpr int COCKPIT_GRID = PX;

inline int q(int v) { return v & ~(PX - 1); }

static inline int floorDivPositive(int value, int divisor) {
    int quotient = value / divisor;
    int remainder = value % divisor;
    return (remainder < 0) ? quotient - 1 : quotient;
}

static inline int q8GridCell(int valueQ8) {
    return floorDivPositive(valueQ8, PX * 256) * PX;
}

static inline int q8GridFraction255(int valueQ8, int cell) {
    return (valueQ8 - cell * 256) * 255 / (PX * 256);
}

static inline int q8RoundedGridDelta(int deltaQ8) {
    const int gridQ8 = PX * 256;
    const int halfGridQ8 = gridQ8 / 2;
    int cells = deltaQ8 >= 0
        ? (deltaQ8 + halfGridQ8) / gridQ8
        : -((-deltaQ8 + halfGridQ8) / gridQ8);
    return cells * PX;
}

// ==[ LAYOUT ]==
static constexpr int16_t WD_TOP_H     = 14;
static constexpr int16_t WD_PLAY_T    = 16;
static constexpr int16_t WD_PLAY_B    = 240;
static constexpr int16_t WD_GLASS_T   = 20;
static constexpr int16_t WD_GLASS_B   = 156;
static constexpr int16_t WD_DASH_T    = 148;
static constexpr int16_t WD_DASH_B    = 240;
static constexpr int16_t WD_CANOPY_CX = 160;

// ==[ CANOPY ]==
static constexpr float  CANOPY_CURVE        = 1.75f;
static constexpr float  CANOPY_HEIGHT_SCALE = 0.88f;
static constexpr int16_t SHELL_COVER        = 0;
static constexpr int16_t SHELL_COVER_Y      = 36;

// ==[ HUD PANES ]==
static constexpr int16_t SIG_X  = 252, SIG_Y  = 92,  SIG_W  = 56,  SIG_H  = 56;
static constexpr int16_t NAV_X  = 28,  NAV_Y  = 56,  NAV_W  = 48,  NAV_H  = 48;
// Three readable rows: source/fix state, latitude, longitude. The old 28px
// pane only admitted two wrapped rows, so the coordinates were clipped after
// the source label even when a valid Core/C5 fix existed.
static constexpr int16_t COORD_X= 20,  COORD_Y= 104, COORD_W= 64,  COORD_H= 44;
static constexpr int16_t LOG_X  = 204, LOG_Y  = 56,  LOG_W  = 84,  LOG_H  = 36;
static constexpr int16_t ATT_X  = 124, ATT_Y  = 68,  ATT_W  = 72,  ATT_H  = 48;

// ==[ WHEEL ]==
static constexpr int16_t WHEEL_X = 108, WHEEL_Y = 128, WHEEL_W = 64, WHEEL_H = 48;

// ==[ MONITORS ]==
static constexpr int16_t MON_B_X = 4,  MON_B_Y = 108, MON_B_W = 28, MON_B_H = 24;
static constexpr int8_t  MON_B_YAW = -4;

// ==[ CABLES ]==
static constexpr int16_t CAB_A_X = 56,  CAB_A_Y = 28,  CAB_A_W = 24, CAB_A_H = 92;
static constexpr int16_t CAB_B_X = 104, CAB_B_Y = 28,  CAB_B_W = 24, CAB_B_H = 76;
static constexpr int16_t CAB_C_X = 176, CAB_C_Y = 28,  CAB_C_W = 24, CAB_C_H = 100;
static constexpr int16_t CAB_D_X = 32,  CAB_D_Y = 36,  CAB_D_W = 24, CAB_D_H = 68;

// ==[ ACTORS ]==
static constexpr int16_t PIG_CX    = 148;
static constexpr int16_t PIG_TOP_Y = 128;
static constexpr int16_t CHAIR_DX  = 0;
static constexpr int16_t CHAIR_DY  = -4;
static constexpr float   PIG_SCALE = 1.00f;

// ==[ CHAIR ]==
static constexpr int16_t CHAIR_CORE_W        = 36;
static constexpr int16_t CHAIR_HEAD_W        = 28;
static constexpr int16_t CHAIR_HEAD_H        = 16;
static constexpr int16_t CHAIR_BACK_W_TOP    = CHAIR_CORE_W - COCKPIT_GRID;
static constexpr int16_t CHAIR_BACK_W_MID    = CHAIR_CORE_W;
static constexpr int16_t CHAIR_BACK_H        = 48;
static constexpr int16_t CHAIR_BASE_W        = CHAIR_CORE_W + COCKPIT_GRID * 2;
static constexpr int16_t CHAIR_RAIL_W        = CHAIR_CORE_W + COCKPIT_GRID;
static constexpr int16_t CHAIR_BOLSTER_MAX_W = 8;
static constexpr int16_t CHAIR_ENVELOPE_W    = CHAIR_BACK_W_MID + CHAIR_BOLSTER_MAX_W * 2;
static constexpr int16_t CHAIR_ENVELOPE_H    = CHAIR_HEAD_H + COCKPIT_GRID + CHAIR_BACK_H + COCKPIT_GRID * 3;
static constexpr int16_t CHAIR_MOTION_PAD_Y  = 20;
static constexpr int16_t CHAIR_ENVELOPE_L    = PIG_CX + CHAIR_DX - CHAIR_ENVELOPE_W / 2;
static constexpr int16_t CHAIR_ENVELOPE_R    = CHAIR_ENVELOPE_L + CHAIR_ENVELOPE_W;
static constexpr int16_t CHAIR_SHADOW_L      = CHAIR_ENVELOPE_L & ~(COCKPIT_GRID - 1);
static constexpr int16_t CHAIR_SHADOW_R      = (CHAIR_ENVELOPE_R + COCKPIT_GRID - 1) & ~(COCKPIT_GRID - 1);

// ==[ LIGHTING ]==
static constexpr float LIGHT_X   = -0.1695f;
static constexpr float LIGHT_Y   = -0.7911f;
static constexpr float LIGHT_Z   =  0.5877f;
static constexpr float AMBIENT   =  0.03f;

// ==[ PIG ]==
static constexpr int PIG_P       = 4;
static constexpr int PIG_GRID_W  = 18;
static constexpr int PIG_GRID_H  = 10;

// ==[ TIMING ]==
static constexpr uint32_t CHAIR_SETTLE_MS       = 800;
static constexpr uint32_t WD_ENTRY_BLACK_HOLD_MS = 90;
static constexpr uint32_t WD_ENTRY_REVEAL_MS     = 360;

// ==[ VISIBILITY ]==
static constexpr bool SHOW_CITY        = true;
static constexpr bool SHOW_RAIN        = true;
static constexpr bool SHOW_SHELL       = true;
static constexpr bool SHOW_CONSOLE     = true;
static constexpr bool SHOW_CABLES      = false;
static constexpr bool SHOW_MONITORS    = true;
static constexpr bool SHOW_WHEEL       = true;
static constexpr bool SHOW_PIG         = true;
static constexpr bool SHOW_CHAIR       = true;
static constexpr bool SHOW_REFLECTIONS = true;
static constexpr bool SHOW_CANOPY      = true;
static constexpr bool SHOW_CURVATURE   = true;
static constexpr bool SHOW_SCANLINES   = true;
static constexpr bool SHOW_TELEMETRY   = true;
static constexpr bool SHOW_ATTITUDE    = true;
static constexpr bool SHOW_COMMS       = true;
static constexpr bool SHOW_HINTS       = true;
static constexpr bool SHOW_THUNDER     = true;

// ═══════════════════════════════════════════════════════════════════════════
// SHARED TYPES
// ═══════════════════════════════════════════════════════════════════════════

struct PigPose {
    int cell;
    int bx, by;
    int bodyLeft, bodyRight;
    int bobX, bobY;
};

struct CityTowerSlot {
    float depth;
    uint32_t seed;
    int bx, by, bw, bh;
    int16_t refBx, refBy;
};

struct CityFeaturePoint {
    uint32_t seed;
    int x, y;
};

struct CityPalette {
    uint16_t cityNeon, cityAmber;
    uint16_t towerNear, towerMid, towerFar, towerHaze, towerCol;
    uint16_t fogGray, fogCool, fogWarm, haze;
    uint16_t skyZenith, skyTop, skyMid, deckHaze;
    uint16_t roadGlow, roofSilhouetteCol;
    uint16_t layerTowerCol[3];
    uint16_t slitPalette[5];
    uint16_t signColors[4];
    uint16_t headlightWarm, headlightCool;
    uint16_t hlVariety[4];
    uint16_t cacheKeyBG, cacheKeyFG, cacheKeyNEON, cacheKeyWARM;
    uint16_t cacheKeyCRT, cacheKeyVEND, cacheKeyFLUOR, cacheKeySPARK;
};

static constexpr int CITY_OVERSCAN_X    = PX * 10;
static constexpr int CITY_OVERSCAN_Y    = PX * 4;
static constexpr int CITY_TOWER_SLOT_CAP = 1536;
static constexpr uint32_t WARTHOG_BACKDROP_MS = 33;

enum class TowerPhase : uint8_t { IDLE, TURNING, TALKING, TURNING_BACK };

static constexpr uint32_t TURN_MS    = 300;
static constexpr uint32_t TALK_MS    = 4000;
static constexpr uint32_t COMMS_MIN  = 12000;
static constexpr uint32_t COMMS_MAX  = 30000;
static constexpr uint32_t WD_THUNDER_MIN = 40000;
static constexpr uint32_t WD_THUNDER_MAX = 80000;

// ═══════════════════════════════════════════════════════════════════════════
// SHARED HELPERS (inline, zero-cost)
// ═══════════════════════════════════════════════════════════════════════════

using Gfx::clamp01;
using Gfx::clampf;
using Gfx::smoothstep01;

static inline int clampi(int v, int lo, int hi) {
    return (v < lo) ? lo : ((v > hi) ? hi : v);
}

static inline int lerpi(int a, int b, float t) {
    return a + (int)lroundf((float)(b - a) * clamp01(t));
}

static inline uint16_t accentWhite(uint16_t base, float mix) {
    return Display::lerpColor565(base, RP::FLUOR, mix);
}

static inline uint16_t cockpitGlassPeak() {
    return Display::isTheOgTheme() ? RP::SHAFT : RP::FLUOR;
}

static inline uint16_t rgbTo565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((uint16_t)(r >> 3) << 11) |
                      ((uint16_t)(g >> 2) << 5) |
                      (uint16_t)(b >> 3));
}

static inline int snapCeilToStep(float raw, int step = PX) {
    int safeStep = max(1, step);
    int units = (int)ceilf(raw / (float)safeStep);
    return max(safeStep, units * safeStep);
}

static inline int objectGrid() { return COCKPIT_GRID; }

static inline int pigCell() {
    return max(PX, snapCeilToStep((float)PX * PIG_SCALE, PX));
}

static inline int glassOpenTop() {
    return min(WD_DASH_T - PX * 6, WD_GLASS_T + (int)SHELL_COVER_Y);
}

static inline float glassProfileT(int y) {
    float rawT = clamp01((float)(clampi(y, WD_GLASS_T, WD_GLASS_B) - WD_GLASS_T)
                         / (float)max(1, WD_GLASS_B - WD_GLASS_T));
    return clamp01(rawT / fmaxf(0.01f, CANOPY_HEIGHT_SCALE));
}

static constexpr bool sceneRectFits(int x, int y, int w, int h, int top, int bottom) {
    return x >= 0 && y >= top && w > 0 && h > 0 &&
           x + w <= SCREEN_WIDTH && y + h <= bottom;
}

static constexpr bool sceneRectOnGrid(int x, int y, int w, int h) {
    return (x % PX) == 0 && (y % PX) == 0 && (w % PX) == 0 && (h % PX) == 0;
}

static constexpr bool sceneRectsOverlap(int ax, int ay, int aw, int ah,
                                        int bx, int by, int bw, int bh) {
    return ax < bx + bw && bx < ax + aw && ay < by + bh && by < ay + ah;
}

static inline int cityScrollX(float steer, float depth) {
    return (int)lroundf(steer * (100.0f + (1.0f - depth) * 280.0f));
}

static inline int cityScrollY(float pitch, float depth) {
    return (int)lroundf(-pitch * (40.0f + (1.0f - depth) * 100.0f));
}

static inline int par(float steer, float depth) {
    return q(cityScrollX(steer, depth));
}

static inline int parY(float pitch, float depth) {
    return q(cityScrollY(pitch, depth));
}

static inline void wrappedPhase(int offsetPx, int step, int& base, int& phase) {
    int safeStep = max(1, step);
    float baseF = floorf((float)offsetPx / (float)safeStep);
    base = (int)baseF;
    phase = offsetPx - base * safeStep;
}

// ═══════════════════════════════════════════════════════════════════════════
// EXTERN STATE — defined in wardrive_scene.cpp, accessed by all modules
// ═══════════════════════════════════════════════════════════════════════════

// Cockpit accents (theme-reactive, set per-frame in drawScene)
extern uint16_t WD_NEON;
extern uint16_t WD_AMBER;
extern uint16_t WD_LED;
extern uint16_t WD_SPARK;

// IMU state
extern float steerAngle;
extern float imuRoll;
extern float imuPitch;
extern float attRollDeg;
extern float attPitchDeg;

// Motion
extern bool parallaxEnabled;

// Thunder
extern bool  wdThunderFlashing;
extern uint8_t wdThunderFlashState;
extern float wdThunderFlickerBoost;
extern int8_t wdVibrateY;
// Storm surge — latched to 1.0 when a burst opens, decaying over ~8s. Unlike
// wdThunderFlickerBoost (a sub-second window flicker gate) this is the slow
// "heavy weather" channel that thickens rain long after the flash is gone.
extern float wdStormIntensity;

// Traffic
extern float wdTrafficFlare;
extern uint16_t wdTrafficFlareCol;
extern int wdTrafficFlareCX;
// Travel direction of the vehicle that won the frame's flare: +1 rightward,
// -1 leftward. Lets the cockpit wash rake across instead of pulsing in place.
extern int8_t wdTrafficFlareDir;

// Chair settle
extern bool chairSettleActive;
extern uint32_t chairSettleStart;

// Tower comms
extern TowerPhase towerPhase;
extern uint32_t towerPhaseStart;
extern char towerCallsign[16];
extern char towerBubbleText[80];
extern char pigBubbleText[40];
extern uint8_t towerSpeakerIdx;
extern uint32_t nextCommsTime;
extern bool lastCommsWasRecon;
extern uint16_t savedCommsIdx;
extern bool hasCommsTower;

// City PSRAM
extern CityTowerSlot* sceneTowerSlots;
extern uint16_t* sceneVisibleIdx;
extern CityPalette cityPal;
extern int refHorizonY;
extern int cachedTowerCount;
extern bool cityGridValid;
extern uint32_t sceneBackdropCacheLastMs;

// Face timer
extern PigFaceTimer sceneFaceTimer;

// Entry transition
extern bool sceneEntryTransitionActive;
extern uint32_t sceneEntryTransitionStart;

// Backdrop cache
struct BackdropCache {
    M5Canvas* canvas;
    uint32_t lastMs;
};
extern BackdropCache backdropCache;

// Thunder state struct (for functions that need multiple fields)
struct ThunderState {
    bool& flashing;
    uint32_t& flashStart;
    uint8_t& flashState;
    uint32_t& flashDuration;
    uint8_t& flashesRemaining;
    bool& burstFiring;
    uint32_t& lastStorm;
    uint32_t& nextInterval;
    float& flickerBoost;
    int8_t& vibrateY;
};

// Comms generation helpers (implemented in wardrive_hud.cpp)
void generateCallsign(bool isRecon);
bool generateReconTowerMsg(char* buf, size_t bufLen);
void generateTowerMsg(char* buf, size_t bufLen);
void generatePigMsg(char* buf, size_t bufLen);
void updateTowerComms(uint32_t now);

// Glass bounds (implemented in wardrive_glass.cpp, declared in wardrive_glass.h)
// #include "wardrive_glass.h" if you need glassBounds/insideGlass/precomputeGlassBounds

// City backdrop (implemented in wardrive_city_backdrop.cpp)
bool prepareSceneBackdropCache();
bool isSceneBackdropCacheReady();
void releaseSceneBackdropCache();
void updateCityPalette();
int collectCityTowers(float steer, float pitch);
void drawGlassBackdropBase(M5Canvas& c, uint32_t now, float motion);
void drawGlassBackdropMotion(M5Canvas& c, uint32_t now, float motion);
void drawGlassBackdrop(M5Canvas& c, uint32_t now, float motion);
void drawHorizonShadowPass(M5Canvas& c, uint32_t now, int horizonY);

// Pig rendering (implemented in wardrive_pig_render.cpp)
void drawPigRear(M5Canvas& c, uint32_t now, float motion, const PigPose& pose);
void drawPigCommsBubble(M5Canvas& c, uint32_t now, float motion, const PigPose& pose);
void getCockpitPigCenter(int16_t& cx, int16_t& cy);

// Cockpit shell (implemented in wardrive_cockpit_shell.cpp)
// litFrame declared in wardrive_cockpit_shell.h
void drawCockpitShell(M5Canvas& c, uint32_t now, float motion);
void drawConsoleLayer(M5Canvas& c, uint32_t now);
void drawDashboardCables(M5Canvas& c, uint32_t now, float motion);
void drawDashboardMonitors(M5Canvas& c, uint32_t now);
void drawSteeringWheel(M5Canvas& c, int x, int y, int w, int h);
void drawChairPass(M5Canvas& c, const PigPose& pose, bool isForeground);
void drawDashReflections(M5Canvas& c, uint32_t now, float motion);
void drawCRTScanlines(M5Canvas& c);
void drawDashHints(M5Canvas& c, uint32_t now);
void drawAttitudeIndicator(M5Canvas& c);
void drawTelemetryText(M5Canvas& c, uint32_t now);

// Lighting (implemented in wardrive_lighting.cpp)
void drawCockpitLighting(M5Canvas& c, const PigPose& pose);
void drawWindshieldCurvature(M5Canvas& c, uint32_t now, float motion);
void drawCanopyStreaks(M5Canvas& c, uint32_t now, float motion);

// Weather (implemented in wardrive_weather.cpp)
void drawExteriorRainMotion(M5Canvas& c, uint32_t now, float motion);
void drawCanopySurfaceWater(M5Canvas& c, uint32_t now, float motion);

// Glass bounds (implemented in wardrive_glass.cpp, declared in wardrive_glass.h)
void glassBounds(int y, int& left, int& right);

// Entry transition (implemented in wardrive_scene.cpp orchestrator)
void drawSceneEntryTransition(M5Canvas& c, uint32_t now);
float getSceneEntryBlackout(uint32_t now);

// ==[ SCENE TEXT CONSTANTS ]==
static constexpr int SCENE_CHAR_W    = 6;
static constexpr int SCENE_LINE_H    = 10;
static constexpr int SCENE_TEXT_PAD_X = 8;
static constexpr int SCENE_TEXT_PAD_Y = 8;
static constexpr int SCENE_TEXT_MAX   = 24;

// ==[ BUMP / WEAR ]== per-pixel hash jitter for surface texture
static inline uint16_t bumpColor(uint16_t base, int x, int y, uint32_t seed,
                                  uint16_t hiCol, uint16_t loCol, float range = 0.12f) {
    uint32_t h = wallHash(x, y, seed);
    uint8_t n = h & 0xFFu;
    if (n < 10u)  return Display::lerpColor565(base, hiCol, range * 1.8f);
    if (n < 50u)  return Display::lerpColor565(base, loCol, range * 0.7f);
    if (n > 205u) return Display::lerpColor565(base, hiCol, range * 0.9f);
    return base;
}

static inline void shadePx(M5Canvas& c, int x, int y, float factor) {
    x = q(x); y = q(y);
    if (x < 0 || x + PX > c.width() || y < 0 || y + PX > c.height()) return;
    uint16_t base = Gfx::fastReadPx(c, x, y);
    Gfx::fastFillBlock4(c, x, y, MenuPigRender::darken565(base, clampf(factor, 0.0f, 0.90f)));
}

// ==[ SCENE TEXT WRAPPING & RENDERING ]==
static int wrapSceneText(const char* text, char lines[][SCENE_TEXT_MAX], int maxLines, int maxChars) {
    if (!text || !lines || maxLines <= 0 || maxChars <= 0) return 0;
    int line = 0;
    const char* p = text;
    while (*p && line < maxLines) {
        while (*p == '\r') ++p;
        if (*p == '\n') { lines[line][0] = '\0'; ++line; ++p; continue; }
        char raw[96] = {0};
        int rawLen = 0;
        while (p[rawLen] && p[rawLen] != '\r' && p[rawLen] != '\n' && rawLen < (int)sizeof(raw) - 1) {
            raw[rawLen] = p[rawLen]; ++rawLen;
        }
        p += rawLen;
        while (*p == '\r') ++p;
        if (*p == '\n') ++p;
        char* src = raw;
        while (*src == ' ') ++src;
        if (*src == '\0') { lines[line][0] = '\0'; ++line; continue; }
        while (*src && line < maxLines) {
            int len = (int)strlen(src);
            if (len <= maxChars) {
                int copyLen = min(len, SCENE_TEXT_MAX - 1);
                memcpy(lines[line], src, copyLen);
                lines[line][copyLen] = '\0'; ++line; break;
            }
            int cut = maxChars;
            while (cut > 0 && src[cut] != ' ') --cut;
            if (cut == 0) cut = maxChars;
            int copyLen = min(cut, SCENE_TEXT_MAX - 1);
            memcpy(lines[line], src, copyLen);
            lines[line][copyLen] = '\0'; ++line;
            src += cut; while (*src == ' ') ++src;
        }
    }
    if (line == 0) { lines[0][0] = '\0'; return 1; }
    return line;
}

static void drawSceneTextBlock(M5Canvas& canvas, int x, int y, int w, int h,
                                const char* text, uint16_t color) {
    char lines[6][SCENE_TEXT_MAX] = {};
    int maxChars = max(1, (w - 12) / SCENE_CHAR_W);
    int maxLines = clampi((h - SCENE_TEXT_PAD_Y) / SCENE_LINE_H, 1, 6);
    int numLines = wrapSceneText(text, lines, maxLines, maxChars);
    bool leftSide = (x + w / 2) < 160;
    int topLeft, topRight, bottomLeft, bottomRight;
    glassBounds(clampi(y + SCENE_TEXT_PAD_Y, WD_GLASS_T, WD_GLASS_B - PX), topLeft, topRight);
    glassBounds(clampi(y + h - SCENE_TEXT_PAD_Y, WD_GLASS_T, WD_GLASS_B - PX), bottomLeft, bottomRight);
    int edgeDrift = leftSide ? (bottomLeft - topLeft) : (bottomRight - topRight);
    float shearPerLine = (float)edgeDrift * (float)SCENE_LINE_H / (float)max(SCENE_LINE_H, h - SCENE_TEXT_PAD_Y);
    { // HUD glow halo
        int gi = PX;
        int gx0 = q(max(0, x - gi));
        int gy0 = q(max((int)WD_GLASS_T, y - gi));
        int gx1 = q(min(316, x + w + gi));
        int gy1 = q(min((int)WD_GLASS_B, y + h + gi));
        float gcx = (float)(x + w / 2), gcy = (float)(y + h / 2);
        float grx = (float)(w / 2 + gi + PX), gry = (float)(h / 2 + gi + PX);
        for (int gy = gy0; gy < gy1; gy += PX * 2) {
            for (int gx = gx0; gx < gx1; gx += PX * 2) {
                float dx = ((float)gx + 2.0f - gcx) / grx;
                float dy = ((float)gy + 2.0f - gcy) / gry;
                float dist = dx * dx + dy * dy;
                if (dist > 1.0f) continue;
                uint8_t str = (uint8_t)((1.0f - dist) * 48.0f);
                if (str > 0 && gx >= 0 && gx < 316 && gy >= WD_PLAY_T && gy < WD_PLAY_B) {
                    uint16_t base = Gfx::fastReadPx(canvas, gx, gy);
                    canvas.fillRect(gx, gy, PX, PX, Gfx::screenBlend565(base, color, str));
                }
            }
        }
    }
    uint16_t hudDark = Display::isInvertedTheme() ? Display::getColorFG() : Display::getColorBG();
    uint16_t shadow = Display::lerpColor565(hudDark, RP::DEEP, 0.10f);
    for (int i = 0; i < numLines; ++i) {
        int sx = (int)(shearPerLine * (float)i);
        int drawX = x + SCENE_TEXT_PAD_X + sx;
        int drawY = y + SCENE_TEXT_PAD_Y + i * SCENE_LINE_H;
        canvas.setTextColor(shadow);
        canvas.setCursor(q(drawX) + PX, q(drawY) + PX);
        canvas.print(lines[i]);
    }
    canvas.setTextColor(color);
    for (int i = 0; i < numLines; ++i) {
        int sx = (int)(shearPerLine * (float)i);
        int drawX = x + SCENE_TEXT_PAD_X + sx;
        int drawY = y + SCENE_TEXT_PAD_Y + i * SCENE_LINE_H;
        canvas.setCursor(q(drawX), q(drawY));
        canvas.print(lines[i]);
    }
}

// Utility
static inline bool thunderFlashActive() {
    return SHOW_THUNDER && (wdThunderFlashState == 1);
}

static void glowPx(M5Canvas& c, int x, int y, uint16_t col, uint8_t alpha) {
    if (alpha < 8) return;
    x = q(x); y = q(y);
    if (x < 0 || x + PX > c.width() || y < 0 || y + PX > c.height()) return;
    uint16_t base = Gfx::fastReadPx(c, x, y);
    Gfx::fastFillBlock4(c, x, y, Gfx::screenBlend565(base, col, alpha));
}

static void blendPx8(M5Canvas& c, int x, int y, uint16_t col, uint8_t a) {
    if (a < 8) return;
    x = q(x); y = q(y);
    if (x < 0 || x + PX > c.width() || y < 0 || y + PX > c.height()) return;
    uint16_t base = Gfx::fastReadPx(c, x, y);
    Gfx::fastFillBlock4(c, x, y, Gfx::screenBlend565(base, col, a));
}

static void blendPx(M5Canvas& c, int x, int y, uint16_t col, float f) {
    blendPx8(c, x, y, col, (uint8_t)(clamp01(f) * 255.0f));
}

// Speed helpers
float getEffectSpeedKmh();
float getDisplaySpeedKmh();
float getMotion();

} // namespace WardriveScene
