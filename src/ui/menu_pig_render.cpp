/**
 * menu_pig_render.cpp — stateless rendering infrastructure for menu pig rooms
 *
 * Room palette, blend helpers, background textures, lighting, environmental
 * props. ALL functions here are stateless — they take canvas + position + colors.
 * State-dependent rendering (neon timing, room draw, pig draw) stays in menu_pig.cpp.
 *
 * Extracted from menu_pig.cpp — function bodies are exact copies.
 */

#include "menu_pig_render.h"
#include "pixel_materials.h"
#include "rooms/exterior_sprites.h"
#include <math.h>

namespace MenuPigRender {

// ==[ ROOM PALETTE ]== theme-derived 2-color + interpolation
// All shades lerp between theme bg (t=0) and fg (t=1), cached per frame.
namespace RP {
    // Cached colors — call update() once per frame before drawing
    uint16_t BG;            // t=0.00 — background
    uint16_t DEEP;          // t=0.10 — deepest shadow, pod interior
    uint16_t SHADOW_E;      // t=0.12 — shadow edge
    uint16_t SHADOW_C;      // t=0.18 — shadow core
    uint16_t WALL_FAR;      // t=0.20 — far wall grit
    uint16_t FILL;          // t=0.22 — dark furniture fill
    uint16_t FLOOR_GRIME;   // t=0.20 — floor edge dirt
    uint16_t GREEN_DK;      // t=0.30 — life-support LED
    uint16_t WALL_MID;      // t=0.40 — mid wall detail
    uint16_t FLOOR_GRID;    // t=0.40 — floor grate pattern
    uint16_t SOFT;          // t=0.40 — fabric / cushions
    uint16_t DUST;          // t=0.40 — particles
    uint16_t SHAFT;         // t=0.45 — light shaft
    uint16_t WALL_NEAR;     // t=0.55 — near wall cracks
    uint16_t CRT;           // t=0.60 — CRT phosphor
    uint16_t WARM;          // t=0.65 — warm glow / broth
    uint16_t NEON;          // t=0.70 — neon sign
    uint16_t PUDDLE;        // t=0.70 — neon reflection
    uint16_t LED;           // t=0.72 — amber indicator
    uint16_t SPARK;         // t=0.75 — wire spark
    uint16_t VEND;          // t=0.60 — vending glow
    uint16_t STRUCT;        // t=0.80 — structural outlines
    uint16_t FLUOR;         // t=0.85 — fluorescent tube
    // pre-darkened variants — noir rooms skip global darken
    uint16_t D_STRUCT, D_WALL_NEAR, D_FILL, D_DEEP, D_WARM; // 40%
    uint16_t D50_STRUCT, D50_FILL;                         // 50% (deep noir)

    // Scene-local modifiers (currently TimeOfDay) are allowed to tint RP after
    // update(), but the next frame must start from the canonical theme palette.
    // Keep the cached fast path without letting those tints compound or leak
    // into MENU/WARTHOG when the theme inputs themselves have not changed.
    static uint16_t* const kPaletteSlots[] = {
        &BG, &DEEP, &SHADOW_E, &SHADOW_C, &WALL_FAR, &FILL,
        &FLOOR_GRIME, &GREEN_DK, &WALL_MID, &FLOOR_GRID, &SOFT,
        &DUST, &SHAFT, &WALL_NEAR, &CRT, &WARM, &NEON, &PUDDLE,
        &LED, &SPARK, &VEND, &STRUCT, &FLUOR, &D_STRUCT,
        &D_WALL_NEAR, &D_FILL, &D_DEEP, &D_WARM, &D50_STRUCT,
        &D50_FILL,
    };
    static constexpr size_t kPaletteSlotCount =
        sizeof(kPaletteSlots) / sizeof(kPaletteSlots[0]);
    static uint16_t cachedBasePalette[kPaletteSlotCount] = {};
    static bool cachedBasePaletteValid = false;

    static void restoreCachedBasePalette() {
        if (!cachedBasePaletteValid) return;
        for (size_t i = 0; i < kPaletteSlotCount; ++i)
            *kPaletteSlots[i] = cachedBasePalette[i];
    }

    static void cacheBasePalette() {
        for (size_t i = 0; i < kPaletteSlotCount; ++i)
            cachedBasePalette[i] = *kPaletteSlots[i];
        cachedBasePaletteValid = true;
    }

    void update() {
        uint16_t bg = Display::getColorBG();
        uint16_t fg = Display::getColorFG();
        uint8_t accent = Display::getAccentMode();
        uint8_t lightInt = Display::getLightIntensity();
        uint8_t style = Display::getCurrentStyle();
        // skip recompute if nothing changed since last call
        static uint16_t cachedBG = 0xDEAD, cachedFG = 0xDEAD;
        static uint8_t cachedAccent = 0xFF, cachedLightInt = 0xFF, cachedStyle = 0xFF;
        if (bg == cachedBG && fg == cachedFG && accent == cachedAccent &&
            lightInt == cachedLightInt && style == cachedStyle) {
            restoreCachedBasePalette();
            return;
        }
        cachedBG = bg; cachedFG = fg; cachedAccent = accent;
        cachedLightInt = lightInt; cachedStyle = style;

        auto L = [bg, fg](float t) { return Display::lerpColor565(bg, fg, t); };
        uint16_t hue = Display::getAccentBaseHue();
        auto H = [](uint16_t h, uint8_t s, uint8_t v) {
            return Display::hsvToRgb565(h % 360, s, v);
        };
        auto N = [bg, fg](uint16_t accentCol, float dim) {
            uint16_t themed = Display::lerpColor565(accentCol, fg, 0.50f);
            return Display::lerpColor565(bg, themed, dim);
        };

        // accent hue offsets — skip for MONO (style 3, no hue)
        const auto& offs = Display::getAccentOffsets();
        int16_t nOff = (style == 3) ? 0   : offs.neon;
        int16_t wOff = (style == 3) ? 40  : offs.warm;
        int16_t cOff = (style == 3) ? 150 : offs.crt;
        int16_t vOff = (style == 3) ? 180 : offs.vend;

        const bool isTheOg = Display::isTheOgTheme();
        const uint16_t crtSource = H(hue + cOff, 200, 200);
        const uint16_t warmSource = H(hue + wOff, 220, 230);
        const uint16_t neonSource = H(hue + nOff, 240, 240);
        const uint16_t sparkSource = H(hue + nOff + 20, 120, 250);
        const uint16_t vendSource = H(hue + vOff, 200, 210);
        const uint16_t shaftSource = H(195, 100, 195);  // cold rain / city air
        const uint16_t fluorSource = H(105, 55, 245);   // Alien sick-bay white

        // ==[ NON-EMISSIVE ]== surfaces, walls, floor
        BG         = bg;
        DEEP       = L(0.10f);
        SHADOW_E   = L(0.12f);
        SHADOW_C   = L(0.18f);
        WALL_FAR   = L(0.20f);
        FILL       = L(0.22f);
        FLOOR_GRIME= L(0.20f);
        SOFT       = L(0.35f);
        WALL_MID   = L(0.38f);
        FLOOR_GRID = L(0.42f);
        DUST       = L(0.44f);
        SHAFT      = isTheOg ? Display::lerpColor565(bg, shaftSource, 0.45f) : L(0.45f);
        WALL_NEAR  = L(0.55f);
        STRUCT     = L(0.80f);

        // ==[ EMISSIVE ]== light sources, accent-offset driven
        GREEN_DK   = N(crtSource, 0.30f);
        CRT        = N(crtSource, 0.55f);
        WARM       = N(warmSource, 0.55f);
        NEON       = N(neonSource, 0.65f);
        // Wet material is cold glass carrying a restrained neon reflection,
        // not a second neon tube. Keep legacy styles stable; THE OG gets the
        // source-separated wet base its mixed practical palette needs.
        uint16_t wetSource = isTheOg
            ? Display::lerpColor565(shaftSource, neonSource, 0.30f)
            : neonSource;
        PUDDLE     = N(wetSource, isTheOg ? 0.42f : 0.45f);
        LED        = N(warmSource, 0.60f);
        SPARK      = N(sparkSource, 0.70f);
        VEND       = N(vendSource, 0.45f);
        FLUOR      = isTheOg ? Display::lerpColor565(bg, fluorSource, 0.85f) : L(0.85f);

        // ==[ BRIGHTNESS HIERARCHY ]== boost emissive, suppress non-emissive
        if (lightInt > 0) {
            // reference brightness: max of fg/bg (handles INVERTED where bg > fg)
            uint8_t fgBr = Display::brightness565(fg);
            uint8_t bgBr = Display::brightness565(bg);
            uint8_t refBr = (fgBr > bgBr) ? fgBr : bgBr;

            uint8_t boost = lightInt * 32;  // 32 / 64 / 96
            uint8_t ceiling = (refBr > lightInt * 10) ? refBr - lightInt * 10 : 0;

            // THE OG keeps neon/sodium/CRT/vend split under boost. other styles
            // still converge on theme FG as before.
            const bool ownHueBoost = isTheOg;
            const uint16_t neonTarget = ownHueBoost ? neonSource : fg;
            const uint16_t warmTarget = ownHueBoost ? warmSource : fg;
            const uint16_t crtTarget = ownHueBoost ? crtSource : fg;
            const uint16_t sparkTarget = ownHueBoost ? sparkSource : fg;
            const uint16_t vendTarget = ownHueBoost ? vendSource : fg;
            NEON   = screenBlend565(NEON,   neonTarget, boost);
            WARM   = screenBlend565(WARM,   warmTarget, boost);
            CRT    = screenBlend565(CRT,    crtTarget, boost);
            LED    = screenBlend565(LED,    warmTarget, boost);
            SPARK  = screenBlend565(SPARK,  sparkTarget, boost);
            VEND   = screenBlend565(VEND,   vendTarget, boost);
            FLUOR  = screenBlend565(FLUOR,  ownHueBoost ? fluorSource : fg, boost);
            PUDDLE = screenBlend565(PUDDLE, neonTarget, boost / 2);
            GREEN_DK = screenBlend565(GREEN_DK, crtTarget, boost / 2);

            // clamp non-emissive (SHAFT, DUST left unclamped — volumetric)
            DEEP       = clampBrightness565(DEEP,       ceiling);
            SHADOW_E   = clampBrightness565(SHADOW_E,   ceiling);
            SHADOW_C   = clampBrightness565(SHADOW_C,   ceiling);
            WALL_FAR   = clampBrightness565(WALL_FAR,   ceiling);
            FILL       = clampBrightness565(FILL,        ceiling);
            FLOOR_GRIME= clampBrightness565(FLOOR_GRIME,ceiling);
            SOFT       = clampBrightness565(SOFT,        ceiling);
            WALL_MID   = clampBrightness565(WALL_MID,    ceiling);
            FLOOR_GRID = clampBrightness565(FLOOR_GRID,  ceiling);
            WALL_NEAR  = clampBrightness565(WALL_NEAR,   ceiling);
            STRUCT     = clampBrightness565(STRUCT,       ceiling);
        }

        // pre-darkened variants — skip global darken, draw furniture dark directly
        static constexpr uint8_t kNoirT8    = 102; // 0.40 * 256
        static constexpr uint8_t kNoirT8_50 = 128; // 0.50 * 256
        D_STRUCT    = lerpColor565_8(STRUCT,    BG, kNoirT8);
        D_WALL_NEAR = lerpColor565_8(WALL_NEAR, BG, kNoirT8);
        D_FILL      = lerpColor565_8(FILL,      BG, kNoirT8);
        D_DEEP      = lerpColor565_8(DEEP,      BG, kNoirT8);
        D_WARM      = lerpColor565_8(WARM,      BG, kNoirT8);
        D50_STRUCT  = lerpColor565_8(STRUCT,    BG, kNoirT8_50);
        D50_FILL    = lerpColor565_8(FILL,      BG, kNoirT8_50);
        cacheBasePalette();
    }  // update()
}  // namespace RP

// bayer4 + SIN_LUT_Q15 moved to gfx/gfx.cpp

static bool orderedRoomCoverage(int px, int py, uint8_t threshold,
                                uint32_t seed) {
    int phaseX = (int)(seed & 3u);
    int phaseY = (int)((seed >> 2) & 3u);
    int bx = ((px / kRoomPX) + phaseX) & 3;
    int by = ((py / kRoomPX) + phaseY) & 3;
    return bayer4[by][bx] < threshold;
}

// ==[ BLEND HELPERS ]== read-modify-write pixel blending for light/shadow

// Darken: multiply pixel toward black. factor 0=no change, 1=black.
uint16_t darken565(uint16_t base, float factor) {
    // lerp toward theme bg — never drops outside the palette
    return Display::lerpColor565(base, Display::getColorBG(), factor);
}

uint16_t mixColor565(uint16_t base, uint16_t tint, uint8_t amount) {
    return lerpColor565_8(base, tint, amount);
}

// ==[ NOIR PROSCENIUM ]== chunky edge falloff pulls the eye into the room.
// One tight perimeter pass; no full-screen post-process tax.
struct RoomFrameGrade {
    uint8_t sideBase, sideStep;
    uint8_t topBase, topStep;
    uint8_t bottomBase, bottomStep;
    uint8_t corner;
    uint8_t tintMix, shadowTint;
    uint8_t reflectionGain;
    uint8_t halfSpan, sweep, echo;
};

static constexpr RoomFrameGrade kRoomFrameGrades[6] = {
    {24, 16, 30, 18, 18, 12, 18, 52, 28,  96, 44, 42, 18}, // lab: cold CRT
    {26, 17, 30, 20, 20, 12, 18, 56, 32,  88, 48, 38, 16}, // apartment: neon rain
    {20, 14, 24, 16, 16, 10, 14, 72, 36, 108, 56, 46, 20}, // ramen: warm counter
    {30, 18, 36, 22, 24, 14, 22, 44, 24, 116, 64, 52, 22}, // rooftop: storm air
    {28, 17, 30, 18, 22, 13, 20, 68, 40, 104, 52, 46, 20}, // bar: deep neon
    {18, 13, 22, 15, 16,  9, 14, 76, 32, 112, 56, 48, 22}, // comfort: warm/cold
};

void drawRoomNoirFrame(M5Canvas& canvas, uint8_t room, PigLight keyLight,
                       uint32_t now) {
    static_assert(kRoomLightLoopCellPx == kRoomPX,
                  "room light drift must stay on the authored room grid");
    if (room >= 6) room = 0;
    const RoomFrameGrade& grade = kRoomFrameGrades[room];
    uint16_t roomTint = RP::CRT;
    switch (room) {
        case 1: roomTint = RP::SHAFT; break;
        case 2: roomTint = RP::WARM; break;
        case 3: roomTint = RP::SHAFT; break;
        case 4: roomTint = RP::NEON; break;
        case 5: roomTint = RP::SHAFT; break;
        default: break;
    }
    uint16_t shadowTarget = mixColor565(RP::BG, roomTint, grade.shadowTint);
    const int p = kRoomPX;
    const int y0 = kRoomY;
    const int y1 = kRoomY + kRoomH;
    static_assert(kRoomY + kRoomH == kFloorY + kRoomPX * 2,
                  "room frame must close on the floor gutter, above UI chrome");

    auto shadeCell = [&](int x, int y) {
        int topCell = (y - y0) / p;
        int bottomCell = (y1 - p - y) / p;
        int leftCell = x / p;
        int rightCell = (SCREEN_WIDTH - p - x) / p;
        int sideCell = leftCell < rightCell ? leftCell : rightCell;
        int shade = 0;

        if (sideCell < 4) shade = grade.sideBase + (3 - sideCell) * grade.sideStep;
        if (topCell < 2) {
            int topShade = grade.topBase + (1 - topCell) * grade.topStep;
            if (topShade > shade) shade = topShade;
        }
        if (bottomCell < 3) {
            int bottomShade = grade.bottomBase + (2 - bottomCell) * grade.bottomStep;
            if (bottomShade > shade) shade = bottomShade;
        }
        if (shade == 0) return;

        // corners eat more light; hash grain breaks the perfect rectangle.
        if (sideCell < 3 && (topCell < 3 || bottomCell < 4)) shade += grade.corner;
        uint32_t grain = wallHash(x, y, 0xA701u + (uint32_t)room * 0x131u);
        shade += ((int)(grain & 0x7u) - 3) * 3;
        if (shade < 8) shade = 8;
        if (shade > 104) shade = 104;

        uint16_t base = fastReadPx(canvas, x, y);
        canvas.fillRect(x, y, p, p,
                        lerpColor565_8(base, shadowTarget, (uint8_t)shade));
    };

    // side curtains use the room-relative grid; top/bottom lips close the seam.
    for (int y = y0; y <= y1 - p; y += p) {
        for (int cell = 0; cell < 4; cell++) {
            shadeCell(cell * p, y);
            shadeCell(SCREEN_WIDTH - p * (cell + 1), y);
        }
    }
    for (int x = p * 4; x < SCREEN_WIDTH - p * 4; x += p) {
        shadeCell(x, y0);
        shadeCell(x, y0 + p);
        shadeCell(x, y1 - p);
        shadeCell(x, y1 - p * 2);
        shadeCell(x, y1 - p * 3);
    }

    if (keyLight.tint == 0) return;

    RoomLightLoopFrame lightLoop = sampleRoomLightLoop(room, now);
    uint16_t reflectedTint = mixColor565(keyLight.tint, roomTint, grade.tintMix);

    // wet floor lip catches the room's dominant practical. sparse, not disco.
    int lightX = (keyLight.x + lightLoop.driftX) & ~(p - 1);
    for (int row = 0; row < 2; row++) {
        int y = y1 - p * (row + 1);
        for (int dx = -(int)grade.halfSpan; dx <= (int)grade.halfSpan; dx += p) {
            int x = lightX + dx;
            if (x < 0 || x >= SCREEN_WIDTH) continue;

            int falloff = (int)grade.halfSpan - abs(dx);
            if (falloff <= 0) continue;
            uint32_t glint = wallHash(x, y, 0xF109u + (uint32_t)room * 0x211u);
            int keepScaled = (72 + falloff * 3 - row * 20) *
                             (int)lightLoop.reflection * grade.reflectionGain /
                             (72 * 100);
            uint8_t keep = (uint8_t)min(255, keepScaled);
            if ((glint & 0xFFu) > keep) continue;

            int strengthI = (8 + falloff / 3 - row * 2) *
                            (int)lightLoop.reflection * grade.reflectionGain /
                            (72 * 100);
            uint8_t strength = (uint8_t)max(0, min(255, strengthI));
            uint16_t base = fastReadPx(canvas, x, y);
            canvas.fillRect(x, y, p, p,
                            screenBlend565(base, reflectedTint, strength));
        }
    }

    // One practical-light skate keeps the wet lip alive without turning the
    // whole floor into a shader. The normalized path runs out and back, so the
    // highlight closes on its starting edge instead of teleporting across the
    // floor at loop wrap. Grid lock prevents sub-pixel shimmer.
    const int sweepSpan = (int)grade.halfSpan * 2;
    const int sweep = (int)(((uint32_t)lightLoop.sweepQ8 *
                             (uint32_t)sweepSpan) >> 8);
    const int sweepX = (lightX - sweepSpan / 2 + sweep) & ~(p - 1);
    const int sweepY = y1 - p;
    const uint8_t sweepStrength = (uint8_t)(
        ((uint16_t)grade.sweep * lightLoop.energy) / 100u);
    const uint8_t echoStrength = (uint8_t)(
        ((uint16_t)grade.echo * lightLoop.energy) / 100u);
    if (sweepX >= 0 && sweepX < SCREEN_WIDTH) {
        uint16_t base = fastReadPx(canvas, sweepX, sweepY);
        fastFillBlock4(canvas, sweepX, sweepY,
                       screenBlend565(base, reflectedTint, sweepStrength));
        int echoX = sweepX - p;
        if (echoX >= 0) {
            base = fastReadPx(canvas, echoX, sweepY);
            fastFillBlock4(canvas, echoX, sweepY,
                            screenBlend565(base, reflectedTint, echoStrength));
        }
    }
}

// ==[ POP OUTLINE ]==

void drawRoomPopPixel(M5Canvas& canvas, int x, int y, uint16_t color) {
    int sx = x & ~3, sy = y & ~3;
    if (sx < 0 || sx >= SCREEN_WIDTH) return;
    if (sy < kRoomY || sy >= (kFloorY + 8)) return;
    canvas.fillRect(sx, sy, kRoomPX, kRoomPX, color);
}

// 4px BG contour to make small wall/floor props read against busy noise layers.
void drawPopOutline1px(M5Canvas& canvas, int x, int y, int w, int h,
                       PopOutlineStyle style, uint32_t seed) {
    if (w <= 0 || h <= 0) return;
    const int p = kRoomPX;
    const int l = x - p;
    const int t = y - p;
    const int r = x + w;
    const int b = y + h;

    auto keepPx = [&](int px, int py, bool primaryEdge, uint32_t salt) {
        switch (style) {
            case PopOutlineStyle::SOLID:
                return true;
            case PopOutlineStyle::SPARSE:
                return ((wallHash(px, py, seed ^ salt) & 0xFFu) < 144u);
            case PopOutlineStyle::MIXED:
            default:
                if (primaryEdge) return true;
                return ((wallHash(px, py, seed ^ salt) & 0xFFu) < 156u);
        }
    };

    for (int px = l; px <= r; px += p) {
        if (keepPx(px, t, true, 0x11u)) drawRoomPopPixel(canvas, px, t, RP::BG);    // top: solid anchor in MIXED
        if (keepPx(px, b, false, 0x2Bu)) drawRoomPopPixel(canvas, px, b, RP::BG);   // bottom: sparse in MIXED
    }
    for (int py = t; py <= b; py += p) {
        if (keepPx(l, py, true, 0x45u)) drawRoomPopPixel(canvas, l, py, RP::BG);    // left: solid anchor in MIXED
        if (keepPx(r, py, false, 0x63u)) drawRoomPopPixel(canvas, r, py, RP::BG);   // right: sparse in MIXED
    }
}

// ==[ BACKGROUND TEXTURES ]==

// The concrete wall and the metal floor used to exist twice: once here and
// once in PixelMat, as byte-identical copies. Every improvement to one of them
// silently skipped whichever rooms called the other, and the two copies are
// why the invisible-grit and inverted-crack bugs survived a full material
// rewrite. There is now one implementation; these remain as the legacy entry
// points their callers already use.
void drawConcreteWall(M5Canvas& canvas, int sx, int sy, int w, int h,
                      uint32_t variant) {
    PixelMat::drawConcreteWall4(canvas, sx, sy, w, h, variant);
}

void drawMetalFloor(M5Canvas& canvas, uint32_t variant) {
    PixelMat::drawMetalFloor4(canvas, variant);
}

// Dust motes: 5 particles, varied sizes and speeds
void drawDustMotes(M5Canvas& canvas, uint16_t fg, uint32_t now) {
    // Size table: {w, h} in pixels — all 4px grid aligned
    static const int sizes[5][2] = {{4,4}, {4,4}, {4,8}, {4,8}, {8,4}};
    // Cycle period: particles 2,3 float slower (5s) for depth illusion
    static const int cycles[5] = {3000, 3000, 5000, 5000, 3000};
    for (int i = 0; i < 5; i++) {
        uint32_t seed = (uint32_t)(i * 8291 + 5347);
        float period = (float)cycles[i];
        float phase = (float)((now + (uint32_t)i * 1200u) % (uint32_t)cycles[i]) / period;
        int baseX = 20 + (int)(seed % 200u);
        int baseY = kRoomY + 12 + (int)((seed >> 8) % 60);
        int dx = (int)(fastSinf(phase * 3.14159f * 2.0f + (float)i * 1.7f) * 8.0f);
        int dy = (int)(fastSinf(phase * 3.14159f * 4.0f + (float)i * 2.3f) * 4.0f);
        int mx = (baseX + dx) & ~3;
        int my = (baseY + dy) & ~3;
        int mw = sizes[i][0], mh = sizes[i][1];
        if (mx > 4 && mx + mw < SCREEN_WIDTH - 4 && my > kRoomY + 4 && my + mh < kFloorY - 4)
            canvas.fillRect(mx, my, mw, mh, fg);
    }
}

// ==[ LIGHT POOLS ]== surface-blended glow near light sources
// reads underlying pixel, screen-blends with light color.
// density controls ordered pixel coverage AND blend intensity.

static bool clipRoomGridRect(M5Canvas& canvas, int x, int y, int w, int h,
                             int& x0, int& y0, int& x1, int& y1) {
    if (w <= 0 || h <= 0) return false;
    x0 = x & ~3;
    y0 = y & ~3;
    x1 = x0 + ((w + 3) & ~3);
    y1 = y0 + ((h + 3) & ~3);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    int canvasW = canvas.width() & ~3;
    int canvasH = canvas.height() & ~3;
    if (x1 > canvasW) x1 = canvasW;
    if (y1 > canvasH) y1 = canvasH;
    return x0 < x1 && y0 < y1;
}

void drawLightPool(M5Canvas& canvas, uint16_t fg, int x, int y, int w, int h,
                   uint8_t density, uint32_t seed) {
    int x0, y0, x1, y1;
    if (!clipRoomGridRect(canvas, x, y, w, h, x0, y0, x1, y1)) return;
    uint8_t s8 = (uint8_t)((0.30f + 0.35f * clamp01f((float)density / 80.0f)) * 255.0f);
    for (int py = y0; py < y1; py += kRoomPX) {
        for (int px = x0; px < x1; px += kRoomPX) {
            if (orderedRoomCoverage(px, py, density, seed)) {
                uint16_t base = fastReadPx(canvas, px, py);
                canvas.fillRect(px, py, kRoomPX, kRoomPX,
                                screenBlend565(base, fg, s8));
            }
        }
    }
}

// Gradient light pool: dense center, sparse edges (horizontal gradient)
// Screen-blends with underlying surface — intensity follows falloff curve.
void drawLightPoolGradient(M5Canvas& canvas, uint16_t fg, int x, int y, int w, int h,
                           uint8_t centerDensity, uint32_t seed) {
    x &= ~3; y &= ~3;
    w = (w + 3) & ~3; h = (h + 3) & ~3;
    if (w <= 0 || h <= 0) return;
    float cx = (float)x + (float)w * 0.5f;
    float halfW = (float)w * 0.5f;
    float baseStrength = 0.30f + 0.35f * clamp01f((float)centerDensity / 80.0f);
    int x0, y0, x1, y1;
    if (!clipRoomGridRect(canvas, x, y, w, h, x0, y0, x1, y1)) return;
    for (int py = y0; py < y1; py += kRoomPX) {
        for (int px = x0; px < x1; px += kRoomPX) {
            float dist = fabsf((float)px + 2.0f - cx) / halfW;  // 0=center, 1=edge
            float falloff = 1.0f - dist * dist;
            uint8_t d = (uint8_t)((float)centerDensity * falloff);
            if (orderedRoomCoverage(px, py, d, seed)) {
                uint16_t base = fastReadPx(canvas, px, py);
                canvas.fillRect(px, py, kRoomPX, kRoomPX,
                                screenBlend565(base, fg, (uint8_t)(baseStrength * falloff * 255.0f)));
            }
        }
    }
}

// ==[ NEON WASH ]== distance-based directed neon tint on surfaces
// Reads actual underlying pixel and screen-blends with light tint.
// Grid-aligned 4px blocks with quadratic falloff + sparse Bayer skip.
void drawNeonWash(M5Canvas& canvas, int x, int y, int w, int h,
                  PigLight light, uint16_t /* surfaceColor (unused — reads canvas) */,
                  float maxRadius, float strength,
                  uint32_t seed) {
    if (light.tint == 0) return;
    x &= ~3; y &= ~3;
    w = (w + 3) & ~3; h = (h + 3) & ~3;
    int rectX0, rectY0, rectX1, rectY1;
    if (!clipRoomGridRect(canvas, x, y, w, h, rectX0, rectY0, rectX1, rectY1)) return;
    float maxR2 = maxRadius * maxRadius;
    float invMaxR2 = 1.0f / maxR2;
    float str255 = strength * 255.0f;
    // tight bounds — clip iteration to light radius intersection
    int iMaxR = (int)maxRadius + kRoomPX;
    int py0 = ((light.y - iMaxR < rectY0 ? rectY0 : light.y - iMaxR) & ~3);
    int py1 = light.y + iMaxR; if (py1 > rectY1) py1 = rectY1;
    int px0 = ((light.x - iMaxR < rectX0 ? rectX0 : light.x - iMaxR) & ~3);
    int px1 = light.x + iMaxR; if (px1 > rectX1) px1 = rectX1;
    for (int py = py0; py < py1; py += kRoomPX) {
        for (int px = px0; px < px1; px += kRoomPX) {
            float dx = (float)(px + 2 - light.x);
            float dy = (float)(py + 2 - light.y);
            float dist2 = dx * dx + dy * dy;
            if (dist2 >= maxR2) continue;
            float intensity = 1.0f - dist2 * invMaxR2;
            intensity *= intensity;  // quadratic falloff
            uint8_t threshold = (uint8_t)(intensity * 80.0f);
            if (!orderedRoomCoverage(px, py, threshold, seed)) continue;
            uint16_t base = fastReadPx(canvas, px, py);
            fastFillBlock4(canvas, px, py,
                           screenBlend565(base, light.tint, (uint8_t)(str255 * intensity)));
        }
    }
}

// ==[ FURNITURE WASH ]== smooth surface lighting for furniture pieces
// Every cell gets blended — no Bayer skip. Matches pig bump-shade coverage
// so furniture sits in the same lit scene. Quadratic falloff keeps soft edges.
// Use on furniture AFTER drawing. Call for EVERY furniture piece in each room.
void drawFurnitureWash(M5Canvas& canvas, int x, int y, int w, int h,
                       PigLight light, float maxRadius,
                       float strength) {
    if (light.tint == 0) return;
    int x0, y0, x1, y1;
    if (!clipRoomGridRect(canvas, x, y, w, h, x0, y0, x1, y1)) return;
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
            intensity *= intensity;  // quadratic falloff
            if (intensity < 0.02f) continue;  // skip negligible
            uint16_t base = fastReadPx(canvas, px, py);
            if (isNearBG(base)) continue;
            canvas.fillRect(px, py, kRoomPX, kRoomPX,
                            screenBlend565(base, light.tint, (uint8_t)(str255 * intensity)));
        }
    }
}

// ==[ MULTI-EMITTER FURNITURE WASH ]== single pass, N lights on same rect
void drawFurnitureWashMulti(M5Canvas& canvas, int x, int y, int w, int h,
                            const FurnitureWashLight* lights, int count) {
    if (count <= 0) return;
    int x0, y0, x1, y1;
    if (!clipRoomGridRect(canvas, x, y, w, h, x0, y0, x1, y1)) return;
    // pre-compute per-light
    float maxR2[4], invMaxR2[4], str255[4];
    if (count > 4) count = 4;
    for (int i = 0; i < count; i++) {
        maxR2[i] = lights[i].maxRadius * lights[i].maxRadius;
        invMaxR2[i] = 1.0f / maxR2[i];
        str255[i] = lights[i].strength * 255.0f;
    }
    for (int py = y0; py < y1; py += kRoomPX) {
        for (int px = x0; px < x1; px += kRoomPX) {
            uint16_t base = fastReadPx(canvas, px, py);
            if (isNearBG(base)) continue;
            uint16_t result = base;
            for (int i = 0; i < count; i++) {
                if (lights[i].light.tint == 0) continue;
                float dx = (float)(px + 2 - lights[i].light.x);
                float dy = (float)(py + 2 - lights[i].light.y);
                float dist2 = dx * dx + dy * dy;
                if (dist2 >= maxR2[i]) continue;
                float intensity = 1.0f - dist2 * invMaxR2[i];
                intensity *= intensity;
                if (intensity < 0.02f) continue;
                result = screenBlend565(result, lights[i].light.tint,
                                        (uint8_t)(str255[i] * intensity));
            }
            if (result != base)
                canvas.fillRect(px, py, kRoomPX, kRoomPX, result);
        }
    }
}

void drawVolumetricDustBeam(M5Canvas& canvas, uint32_t now,
                            int topCenterX, int topY, int topWidth,
                            int bottomCenterX, int bottomY, int bottomWidth,
                            uint16_t shaftColor, uint16_t dustColor,
                            uint32_t seedBase,
                            const RoomLightLoopFrame* syncedLoop) {
    if (bottomY <= topY) return;
    if (topWidth < 8) topWidth = 8;
    if (bottomWidth < 8) bottomWidth = 8;

    int y0 = topY & ~3;
    int y1 = bottomY & ~3;
    RoomLightLoopFrame localLoop;
    if (!syncedLoop) {
        localLoop = sampleVolumetricLightLoop(now);
        syncedLoop = &localLoop;
    }
    const RoomLightLoopFrame& lightLoop = *syncedLoop;
    const uint8_t loopEnergy = lightLoop.energy;
    const int loopDrift = lightLoop.driftX;
    // Stable coverage mask; the authored loop eases its threshold while drift
    // remains on whole room cells. Re-seeding made the volume read as damage.
    uint32_t seed = seedBase;
    for (int py = y0; py <= y1; py += kRoomPX) {
        float t = (float)(py - y0) / (float)(y1 - y0 + 1);
        float cx = (float)topCenterX + (float)(bottomCenterX - topCenterX) * t;
        cx += (float)loopDrift * (0.25f + t * 0.75f);
        float hw = ((float)topWidth + (float)(bottomWidth - topWidth) * t) * 0.5f;
        int x0 = ((int)floorf(cx - hw)) & ~3;
        int x1 = ((int)ceilf(cx + hw)) & ~3;

        for (int px = x0; px <= x1; px += kRoomPX) {
            if (px < 4 || px > SCREEN_WIDTH - 4) continue;
            float edge = fabsf(((float)px + 2.0f - cx) / (hw + 0.001f));
            if (edge > 1.0f) continue;
            float core = (1.0f - edge * edge) * (0.95f - t * 0.58f);
            if (core <= 0.02f) continue;

            uint8_t d = (uint8_t)((6.0f + core * 54.0f) *
                                  (float)loopEnergy / 100.0f);
            if ((wallHash(px, py, seed + (uint32_t)(py * 3)) & 0xFFu) < d) {
                uint16_t base = fastReadPx(canvas, px, py);
                canvas.fillRect(px, py, kRoomPX, kRoomPX,
                                screenBlend565(base, shaftColor, (uint8_t)((0.30f + core * 0.30f) * 255.0f)));
            }
        }
    }

    // Drifting motes inside the beam.
    for (int i = 0; i < 7; i++) {
        uint32_t cycle = 3400u + (uint32_t)i * 470u;
        float phase = (float)((now + (uint32_t)i * 997u) % cycle) / (float)cycle;
        float vt = fastSinf(phase * 6.28318f) * 0.5f + 0.5f;
        int my = (y0 + (int)(vt * (float)(y1 - y0))) & ~3;
        float t = (float)(my - y0) / (float)(y1 - y0 + 1);
        float cx = (float)topCenterX + (float)(bottomCenterX - topCenterX) * t;
        float hw = ((float)topWidth + (float)(bottomWidth - topWidth) * t) * 0.5f;
        float sway = fastSinf(phase * 12.56636f + (float)i * 1.9f) * (hw * 0.62f);
        int mx = ((int)(cx + sway)) & ~3;
        if (mx < 4 || mx > SCREEN_WIDTH - 4 || my <= kRoomY + 4 || my >= kFloorY - 4) continue;

        uint8_t keep = (uint8_t)(102.0f + (1.0f - t) * 88.0f);
        if ((wallHash(mx, my, seed ^ (uint32_t)(i * 71)) & 0xFFu) < keep) {
            canvas.fillRect(mx, my, kRoomPX, kRoomPX, dustColor);
            if (((i ^ (int)(now / 120u)) & 1) == 0 && my > kRoomY + 8) {
                canvas.fillRect(mx, my - kRoomPX, kRoomPX, kRoomPX, dustColor);
            }
        }
    }
}

static ExteriorSprites::Scene roomWindowScene(
        const RoomWindowBackdropParams& params) {
    ExteriorSprites::Scene scene = ExteriorSprites::Scene::Apartment;
    if (params.style == RoomWindowBackdropStyle::RamenBar)
        scene = ExteriorSprites::Scene::Ramen;
    else if (params.style == RoomWindowBackdropStyle::ComfortBalcony)
        scene = ExteriorSprites::Scene::Comfort;
    return scene;
}

static ExteriorSprites::RenderOptions roomWindowOptions(
        const RoomWindowBackdropParams& params) {
    ExteriorSprites::RenderOptions options;
    options.thunder = params.thunder;
    options.tintActive = params.tintActive;
    options.tintColor565 = params.tintColor565;
    options.tintIntensity = params.tintIntensity;
    options.parallaxX = params.parallaxX;
    return options;
}

void drawRoomWindowBackdropBase(M5Canvas& canvas,
                                int wx, int wy, int ww, int wh,
                                const RoomWindowBackdropParams& params) {
    const int frameInset = kRoomPX;
    const int ix = wx + frameInset;
    const int iy = wy + frameInset;
    const int iw = ww - frameInset * 2;
    const int ih = wh - frameInset * 2;
    if (iw < kRoomPX * 4 || ih < kRoomPX * 4) return;

    ExteriorSprites::drawSceneBase(canvas, roomWindowScene(params),
                                   ix, iy, iw, ih,
                                   roomWindowOptions(params));
}

void drawRoomWindowBackdropMotion(M5Canvas& canvas, uint32_t now,
                                  int wx, int wy, int ww, int wh,
                                  const RoomWindowBackdropParams& params) {
    const int frameInset = kRoomPX;
    const int ix = wx + frameInset;
    const int iy = wy + frameInset;
    const int iw = ww - frameInset * 2;
    const int ih = wh - frameInset * 2;
    if (iw < kRoomPX * 4 || ih < kRoomPX * 4) return;

    ExteriorSprites::drawSceneMotion(canvas, roomWindowScene(params), now,
                                     ix, iy, iw, ih,
                                     roomWindowOptions(params));
}

void drawRoomWindowBackdrop(M5Canvas& canvas, uint32_t now,
                            int wx, int wy, int ww, int wh, float pigPosX,
                            const RoomWindowBackdropParams& params) {
    (void)pigPosX;
    drawRoomWindowBackdropBase(canvas, wx, wy, ww, wh, params);
    drawRoomWindowBackdropMotion(canvas, now, wx, wy, ww, wh, params);
}

// ==[ ROOM WEATHER ]== rain/condensation effects inside rooms

void drawRoomRain(M5Canvas& canvas, uint32_t now,
                  int windowX, int windowY, int windowW, int windowH) {
    // Noir world: always raining outside. Stable sill beads form, release,
    // accelerate, then leave a tiny floor splash. Fractional cell blending
    // keeps the 4px grid while removing the old hard position jumps.
    const uint16_t water = Display::lerpColor565(RP::SHAFT, RP::PUDDLE, 0.32f);
    const uint16_t glint = Display::lerpColor565(RP::SHAFT, RP::FLUOR, 0.16f);
    const int sillY = windowY + windowH;
    const int dropStartY = (sillY + kRoomPX - 1) & ~(kRoomPX - 1);
    const int lastDropY = (kFloorY - kRoomPX) & ~(kRoomPX - 1);
    const int fallH = max(0, lastDropY - dropStartY);
    if (dropStartY > lastDropY || windowW <= kRoomPX * 2) return;

    auto drawTweenedDrop = [&](int x, int yQ8, uint16_t color, uint8_t alpha) {
        const int yPx = yQ8 >> 8;
        const int cellY = yPx & ~(kRoomPX - 1);
        const int fracQ8 = ((yQ8 - cellY * 256) * 255) /
                           (kRoomPX << 8);
        auto drawCell = [&](int py, uint8_t a) {
            if (a < 8 || py < dropStartY || py + kRoomPX > kFloorY) return;
            int sx = x & ~(kRoomPX - 1);
            uint16_t base = fastReadPx(canvas, sx, py);
            fastFillBlock4(canvas, sx, py, screenBlend565(base, color, a));
        };
        drawCell(cellY, (uint8_t)((int)alpha * (255 - fracQ8) / 255));
        drawCell(cellY + kRoomPX, (uint8_t)((int)alpha * fracQ8 / 255));
    };

    for (int i = 0; i < 4; i++) {
        const uint32_t cycle = 2600u + (uint32_t)i * 430u;
        const uint32_t local = (now + (uint32_t)i * 719u) % cycle;
        const uint32_t formEnd = cycle * 28u / 100u;
        const uint32_t fallEnd = cycle * 82u / 100u;
        int spread = (windowW > 8) ? (windowW - 8) : 1;
        int sx = windowX + 4 + (int)((wallHash(i, 0, 0xD2D2) >> 4) % (uint32_t)spread);
        sx &= ~(kRoomPX - 1);
        if (local < formEnd) {
            uint8_t alpha = (uint8_t)(28u + local * 82u / max(1u, formEnd));
            uint16_t base = fastReadPx(canvas, sx, dropStartY);
            fastFillBlock4(canvas, sx, dropStartY,
                           screenBlend565(base, glint, alpha));
        } else if (local < fallEnd) {
            uint32_t tQ8 = (local - formEnd) * 255u /
                           max(1u, fallEnd - formEnd);
            uint32_t easedQ8 = tQ8 * tQ8 / 255u;
            int yQ8 = dropStartY * 256 +
                (int)((uint32_t)fallH * easedQ8 * 256u / 255u);
            drawTweenedDrop(sx, yQ8, water, 124);
            drawTweenedDrop(sx, yQ8 - kRoomPX * 256, water, 48);
        } else {
            uint32_t splashSpan = max(1u, cycle - fallEnd);
            uint8_t fade = (uint8_t)(96u -
                min(88u, (local - fallEnd) * 88u / splashSpan));
            int splashY = lastDropY;
            for (int side = -1; side <= 1; side += 2) {
                int px = sx + side * kRoomPX;
                if (px < 0 || px >= SCREEN_WIDTH) continue;
                uint16_t base = fastReadPx(canvas, px, splashY);
                fastFillBlock4(canvas, px, splashY,
                               screenBlend565(base, RP::PUDDLE, fade));
            }
        }
    }
}

void drawSmoothWaterDrop(M5Canvas& canvas, int x, int yQ8,
                         int minY, int maxY, uint16_t color, uint8_t alpha) {
    if (alpha < 8 || minY >= maxY) return;
    const int sx = x & ~(kRoomPX - 1);
    const int yPx = yQ8 >= 0 ? yQ8 / 256 : -((-yQ8 + 255) / 256);
    const int cellY = yPx & ~(kRoomPX - 1);
    const int frac = ((yQ8 - cellY * 256) * 255) / (kRoomPX << 8);
    auto drawCell = [&](int py, uint8_t weight) {
        if (weight < 8 || sx < 0 || sx + kRoomPX > canvas.width() ||
            py < minY || py + kRoomPX > maxY) return;
        uint8_t cellAlpha = (uint8_t)((int)alpha * weight / 255);
        uint16_t base = fastReadPx(canvas, sx, py);
        fastFillBlock4(canvas, sx, py,
                       screenBlend565(base, color, cellAlpha));
    };
    drawCell(cellY, (uint8_t)(255 - frac));
    drawCell(cellY + kRoomPX, (uint8_t)frac);
}

// ==[ AMBIENT RAIN STREAKS ]== wardrive windshield technique, room interior variant
static void rainStreakGlowPx(M5Canvas& canvas, int x, int y, uint16_t col, uint8_t alpha) {
    if (alpha < 8) return;
    x &= ~3;
    y &= ~3;
    if (x < 0 || x + kRoomPX > canvas.width() ||
        y < kRoomY || y + kRoomPX > canvas.height() || y >= kFloorY) return;
    uint16_t base = fastReadPx(canvas, x, y);
    canvas.fillRect(x, y, kRoomPX, kRoomPX, screenBlend565(base, col, alpha));
}

void drawAmbientRainStreaks(M5Canvas& canvas, uint32_t now,
                             int x0, int y0, int w, int h,
                             uint16_t streakCol, uint16_t sparkCol) {
    if (w < 12 || h < 16) return;
    int streakCount = 5 + (h / 28);
    if (streakCount > 10) streakCount = 10;
    const uint32_t fallStep = 88u;
    const int xSpan = max(kRoomPX, w);
    // Four cells enter above the pane; ten cells of total overscan carry even
    // the longest five-cell tail fully below it before the cycle wraps.
    const int travelH = h + kRoomPX * 10;

    for (int i = 0; i < streakCount; i++) {
        uint32_t seed = wallHash(i, 91, 0x2A71u);
        const uint32_t cycle = 1450u + ((seed >> 7) % 1050u);
        const uint32_t local = (now + (seed & 0x7FFu)) % cycle;
        const uint32_t phase = local / fallStep;
        const uint32_t travelQ8 = local * (uint32_t)travelH * 256u / cycle;
        const int baseYQ8 = (y0 - kRoomPX * 4) * 256 + (int)travelQ8;
        int rawX = (int)((seed >> 11) % (uint32_t)xSpan);
        int baseX = x0 + ((rawX + (int)(phase * (1 + (i & 1u)))) % (xSpan + kRoomPX * 2));
        baseX &= ~(kRoomPX - 1);
        int len = 3 + (int)((seed >> 4) & 0x03u);
        const int lean = ((seed >> 2) & 1u) ? kRoomPX : -kRoomPX;

        for (int s = 0; s < len; s++) {
            int sx = baseX - (s / 2) * lean;
            int syQ8 = baseYQ8 - s * kRoomPX * 256;
            int syPx = syQ8 >= 0 ? syQ8 / 256
                                  : -((-syQ8 + 255) / 256);
            int sy = syPx & ~(kRoomPX - 1);
            if (sx < x0 || sx >= x0 + w || sy + kRoomPX < y0 || sy >= y0 + h) continue;
            uint16_t segCol = (s == 0) ? sparkCol : streakCol;
            int rawA = 142 - s * 20 + ((s == 0) ? 18 : 0);
            if (rawA < 28) rawA = 28;
            if (rawA > 220) rawA = 220;

            // Only the leading bead crossfades between cells. Tails stay one
            // block each, keeping write count comparable to the old loop.
            if (s == 0) {
                int fracQ8 = ((syQ8 - sy * 256) * 255) / (kRoomPX << 8);
                uint8_t a0 = (uint8_t)(rawA * (255 - fracQ8) / 255);
                uint8_t a1 = (uint8_t)(rawA * fracQ8 / 255);
                if (sy >= y0) rainStreakGlowPx(canvas, sx, sy, segCol, a0);
                if (sy + kRoomPX < y0 + h)
                    rainStreakGlowPx(canvas, sx, sy + kRoomPX, segCol, a1);
            } else {
                int roundedY = sy + ((((syQ8 - sy * 256) >> 8) >= kRoomPX / 2)
                                     ? kRoomPX : 0);
                if (roundedY >= y0 && roundedY < y0 + h)
                    rainStreakGlowPx(canvas, sx, roundedY, segCol, (uint8_t)rawA);
            }
        }
    }
}

void drawWindowGlassRain(M5Canvas& canvas, uint32_t now,
                         int windowX, int windowY, int windowW, int windowH,
                         uint16_t reflectionTint, uint8_t intensity) {
    int ix = windowX + kRoomPX;
    int iy = windowY + kRoomPX;
    int iw = windowW - kRoomPX * 2;
    int ih = windowH - kRoomPX * 2;
    if (iw < 12 || ih < 12) return;
    if (reflectionTint == 0) reflectionTint = RP::PUDDLE;
    uint16_t waterBody = Display::lerpColor565(RP::SHAFT, RP::BG, 0.18f);
    uint16_t streakCol = Display::lerpColor565(waterBody, reflectionTint, 0.20f);
    uint16_t sparkCol = Display::lerpColor565(RP::SHAFT, RP::FLUOR, 0.18f);
    drawAmbientRainStreaks(canvas, now, ix, iy, iw, ih, streakCol, sparkCol);

    auto glassGlow = [&](int x, int y, uint16_t color, uint8_t alpha) {
        x &= ~(kRoomPX - 1);
        y &= ~(kRoomPX - 1);
        if (alpha < 8 || x < ix || x >= ix + iw || y < iy || y >= iy + ih) return;
        uint16_t base = fastReadPx(canvas, x, y);
        fastFillBlock4(canvas, x, y, screenBlend565(base, color, alpha));
    };

    // Stable beads: geometry stays fixed; only slow capillary creep and light
    // response animate. Edge-biased lanes make the frame seals read as wet.
    int beadCount = 4 + intensity / 32;
    if (beadCount > 8) beadCount = 8;
    const int creepMax = max(kRoomPX, min(ih / 3, kRoomPX * 5));
    const int startSpan = max(kRoomPX, ih - creepMax - kRoomPX);
    for (int i = 0; i < beadCount; ++i) {
        uint32_t seed = wallHash(i, windowX ^ windowY, 0x6A51u);
        int bx;
        if (i < 2) {
            bx = (i == 0) ? ix : ix + iw - kRoomPX;
        } else {
            bx = ix + (int)((seed >> 9) % (uint32_t)max(kRoomPX, iw - kRoomPX));
        }
        int by = iy + (int)((seed >> 17) % (uint32_t)startSpan);
        uint32_t cycle = 5200u + (seed & 0x7FFu);
        uint32_t local = (now + ((seed >> 5) & 0xFFFu)) % cycle;
        uint32_t hold = cycle * (55u + ((seed >> 3) & 0x0Fu)) / 100u;
        int creepQ8 = 0;
        if (local > hold) {
            uint32_t tQ8 = (local - hold) * 255u / max(1u, cycle - hold);
            uint32_t easedQ8 = tQ8 * tQ8 / 255u;
            int span = kRoomPX + (int)((seed >> 21) % (uint32_t)creepMax);
            creepQ8 = (int)((uint32_t)span * easedQ8 * 256u / 255u);
        }
        int yQ8 = by * 256 + creepQ8;
        int py = (yQ8 >> 8) & ~(kRoomPX - 1);
        int frac = ((yQ8 - py * 256) * 255) / (kRoomPX << 8);
        // Disappear through the wrap seam, then reform at the same anchor.
        // The topology stays fixed without a visible upward teleport.
        uint32_t fadeInEnd = cycle / 10u;
        uint32_t fadeOutStart = cycle * 88u / 100u;
        uint8_t lifeAlpha = 255;
        if (local < fadeInEnd) {
            lifeAlpha = (uint8_t)(local * 255u / max(1u, fadeInEnd));
        } else if (local > fadeOutStart) {
            lifeAlpha = (uint8_t)((cycle - local) * 255u /
                                  max(1u, cycle - fadeOutStart));
        }
        uint8_t bodyA = (uint8_t)((34u + intensity / 3u) * lifeAlpha / 255u);
        glassGlow(bx, py, waterBody,
                  (uint8_t)((int)bodyA * (255 - frac) / 255));
        glassGlow(bx, py + kRoomPX, waterBody,
                  (uint8_t)((int)bodyA * frac / 255));
        int glintX = bx + (((seed >> 2) & 1u) ? -kRoomPX : kRoomPX);
        uint8_t glintA = (uint8_t)((18u + intensity / 5u) * lifeAlpha / 255u);
        glassGlow(glintX, py, sparkCol,
                  (uint8_t)((int)glintA * (255 - frac) / 255));
        glassGlow(glintX, py + kRoomPX, sparkCol,
                  (uint8_t)((int)glintA * frac / 255));
    }

    // Two restrained source-colored reflection ribbons. Fixed breakup plus a
    // slow intensity pulse reads as reflected city light, not moving paint.
    for (int band = 0; band < 2; ++band) {
        uint32_t seed = wallHash(band, windowW, 0x6B71u);
        uint32_t phase = (now + (seed & 0x7FFu)) % (3600u + band * 900u);
        uint32_t half = (3600u + band * 900u) / 2u;
        uint8_t pulse = (uint8_t)(phase < half
            ? phase * 72u / max(1u, half)
            : (3600u + band * 900u - phase) * 72u / max(1u, half));
        int rx = ix + iw * (band == 0 ? 2 : 5) / 7;
        int ry = iy + kRoomPX * (2 + band * 3);
        for (int cell = 0; cell < 4; ++cell) {
            if ((wallHash(cell, band, seed) & 0x03u) == 0u) continue;
            glassGlow(rx + cell * kRoomPX, ry + cell * kRoomPX,
                      reflectionTint,
                      (uint8_t)((18u + pulse / 3u) * intensity / 128u));
        }
    }
}

void drawCondensation(M5Canvas& canvas, uint32_t now,
                      int surfX, int surfY, int surfW, int surfH, int count,
                      uint16_t tint, uint32_t stableSalt) {
    if (surfW < kRoomPX * 2 || surfH < kRoomPX * 2 || count <= 0) return;
    if (tint == 0) tint = RP::SHAFT;
    uint16_t body = Display::lerpColor565(RP::SHAFT, RP::BG, 0.24f);
    uint16_t rim = Display::lerpColor565(tint, RP::SHAFT, 0.48f);
    // Stable bead mask with a long hold and short creep. No topology reroll.
    for (int i = 0; i < count; i++) {
        // Position may parallax; identity must not. Local dimensions plus an
        // explicit surface salt keep the mask attached to moving geometry.
        uint32_t seed = wallHash(i, surfW ^ (surfH << 8), stableSalt);
        uint32_t cycle = 5600u + (seed & 0x7FFu);
        uint32_t local = (now + (uint32_t)i * 977u) % cycle;
        uint32_t hold = cycle * 68u / 100u;
        int spanX = max(1, surfW - kRoomPX * 2);
        int spanY = max(1, surfH - kRoomPX * 3);
        int bx = surfX + kRoomPX + (int)((seed >> 4) % (uint32_t)spanX);
        int by = surfY + kRoomPX + (int)((seed >> 15) % (uint32_t)spanY);
        int creepQ8 = 0;
        if (local > hold) {
            uint32_t tQ8 = (local - hold) * 255u / max(1u, cycle - hold);
            creepQ8 = (int)((uint32_t)(kRoomPX * 2) * 256u * tQ8 * tQ8 /
                            (255u * 255u));
        }
        int sx = bx & ~(kRoomPX - 1);
        int yQ8 = by * 256 + creepQ8;
        int sy = (yQ8 >> 8) & ~(kRoomPX - 1);
        int frac = ((yQ8 - sy * 256) * 255) / (kRoomPX << 8);
        uint32_t fadeInEnd = cycle / 10u;
        uint32_t fadeOutStart = cycle * 88u / 100u;
        uint8_t lifeAlpha = 255;
        if (local < fadeInEnd) {
            lifeAlpha = (uint8_t)(local * 255u / max(1u, fadeInEnd));
        } else if (local > fadeOutStart) {
            lifeAlpha = (uint8_t)((cycle - local) * 255u /
                                  max(1u, cycle - fadeOutStart));
        }
        int rimX = sx + (((seed >> 2) & 1u) ? kRoomPX : -kRoomPX);
        auto drawBeadCell = [&](int py, uint8_t weight) {
            if (weight < 8 || sx < surfX || sx >= surfX + surfW ||
                py < surfY || py >= surfY + surfH) return;
            uint8_t bodyAlpha = (uint8_t)(58u * lifeAlpha * weight /
                                          (255u * 255u));
            uint16_t base = fastReadPx(canvas, sx, py);
            fastFillBlock4(canvas, sx, py,
                           screenBlend565(base, body, bodyAlpha));
            if (rimX >= surfX && rimX < surfX + surfW) {
                uint8_t rimAlpha = (uint8_t)(28u * lifeAlpha * weight /
                                             (255u * 255u));
                base = fastReadPx(canvas, rimX, py);
                fastFillBlock4(canvas, rimX, py,
                               screenBlend565(base, rim, rimAlpha));
            }
        };
        drawBeadCell(sy, (uint8_t)(255 - frac));
        drawBeadCell(sy + kRoomPX, (uint8_t)frac);
    }
}

// Steam constants (local to this file, matching menu_pig.cpp)
static constexpr uint32_t STEAM_CYCLE_MS = 1800;
static constexpr int STEAM_MAX_RISE = 16;

void drawSteam(M5Canvas& canvas, uint16_t fg, uint32_t now, int baseX, int baseY) {
    for (int i = 0; i < 3; i++) {
        uint32_t phase = (now + i * (STEAM_CYCLE_MS / 3)) % STEAM_CYCLE_MS;
        float t = (float)phase / (float)STEAM_CYCLE_MS;
        int rise = (int)(t * STEAM_MAX_RISE);
        float wave = fastSinf(t * 3.14159f * 2.0f + i * 2.0f) * 4.0f;
        int sx = baseX + (int)wave + (i - 1) * 4;
        int sy = baseY - rise;
        sx = sx & ~3;
        sy = sy & ~3;
        if (sy > (int)UIMeasurements::kTopBarH && rise < STEAM_MAX_RISE - 4)
            canvas.fillRect(sx, sy, 4, 4, fg);
    }
}

// ==[ FURNITURE ]==

// Worn couch — same sofa structure with bg "tear" holes and patch stitching
// Uses RP internally: STRUCT arms, SOFT cushion fabric, BG interior
void drawWornCouch(M5Canvas& canvas, int sx, int sy, int sw, int sh) {
    int backH = 8, armW = 8, seatH = 8;
    // Frame/structure in STRUCT, cushion in SOFT (4px grid)
    canvas.fillRect(sx, sy, sw, backH, RP::SOFT);
    canvas.fillRect(sx, sy + backH, armW, sh - backH, RP::STRUCT);
    canvas.fillRect(sx + sw - armW, sy + backH, armW, sh - backH, RP::STRUCT);
    canvas.fillRect(sx + armW, sy + sh - seatH, sw - armW * 2, seatH, RP::SOFT);
    canvas.fillRect(sx + armW, sy + backH, sw - armW * 2, sh - backH - seatH, RP::FILL);
    canvas.fillRect(sx + sw / 2, sy + backH, 4, sh - backH - seatH, RP::STRUCT);
    // Tear holes in seat cushion
    canvas.fillRect(sx + 16, sy + sh - 8, 4, 4, RP::BG);
    canvas.fillRect(sx + 36, sy + sh - 4, 8, 4, RP::BG);
    canvas.fillRect(sx + 56, sy + sh - 8, 4, 4, RP::BG);
    // Tear hole in back
    canvas.fillRect(sx + 20, sy + 4, 4, 4, RP::BG);
    canvas.fillRect(sx + 52, sy + 4, 4, 4, RP::BG);
    // Patch stitching marks
    canvas.fillRect(sx + 16, sy + sh - 8, 4, 4, RP::STRUCT);
    canvas.fillRect(sx + 20, sy + sh - 8, 4, 4, RP::STRUCT);
    canvas.fillRect(sx + 56, sy + sh - 8, 4, 4, RP::STRUCT);
    canvas.fillRect(sx + 60, sy + sh - 8, 4, 4, RP::STRUCT);
    // Worn armrest corners (faded fabric)
    canvas.fillRect(sx, sy, 4, 4, RP::FILL);
    canvas.fillRect(sx + sw - 4, sy, 4, 4, RP::FILL);
    // Spring sag
    canvas.fillRect(sx + 12, sy + sh - 4, 4, 4, RP::SHADOW_C);
    // Feet — stubby legs to floor (scaling gap fix)
    int footH = kFloorY - sy - sh;
    if (footH > 0) {
        canvas.fillRect(sx + 4, sy + sh, 4, footH, RP::STRUCT);
        canvas.fillRect(sx + sw - 8, sy + sh, 4, footH, RP::STRUCT);
        // Ground shadow under feet
        canvas.fillRect(sx, kFloorY, 8, 4, RP::SHADOW_C);
        canvas.fillRect(sx + sw - 8, kFloorY, 8, 4, RP::SHADOW_C);
    }
}

// Floor grate — metal grid with rectangular bg holes
void drawFloorGrate(M5Canvas& canvas, uint16_t fg, uint16_t bg,
                    int rx, int ry, int rw, int rh) {
    canvas.fillRect(rx, ry, rw, rh, fg);
    // Regular rectangular holes (4px grid)
    for (int x = rx + 4; x < rx + rw - 4; x += 8) {
        for (int y = ry + 4; y < ry + rh - 4; y += 8) {
            canvas.fillRect(x, y, 4, 4, bg);
        }
    }
    // Bent grate bar
    canvas.fillRect(rx + 20, ry + 4, 4, 4, RP::BG);
    // Shadow below grate
    canvas.fillRect(rx, ry + rh, rw, 4, RP::SHADOW_C);
}

// Wall pipes — vertical pipe run with elbow, mounting brackets, valve (4px grid)
void drawWallPipes(M5Canvas& canvas, uint16_t fg,
                   int px, int py, bool withValve) {
    // Vertical pipe
    canvas.fillRect(px, py, 4, 52, fg);
    // Shadow on right side
    canvas.fillRect(px + 4, py, 4, 52, RP::SHADOW_C);
    // Mounting brackets
    canvas.fillRect(px - 4, py + 8, 8, 4, fg);
    canvas.fillRect(px - 4, py + 28, 8, 4, fg);
    // Elbow to horizontal
    canvas.fillRect(px, py + 44, 4, 4, fg);
    canvas.fillRect(px + 4, py + 44, 12, 4, fg);
    // Vertical drop from horizontal end
    {
        int dropTop = py + 48;
        int dropH = 16;
        if (dropTop + dropH > kFloorY) dropH = kFloorY - dropTop;
        if (dropH > 0) canvas.fillRect(px + 12, dropTop, 4, dropH, fg);
    }
    // Corrosion at elbow joint
    canvas.fillRect(px + 4, py + 48, 4, 4, RP::FILL);

    if (withValve) {
        // Valve wheel on main vertical
        canvas.fillRect(px - 4, py + 16, 4, 4, fg);
        canvas.fillRect(px - 8, py + 16, 4, 8, fg);
    }
}

// Ramen counter — bar top with overhang, front panel with rails (4px grid)
void drawRamenCounter(M5Canvas& canvas, uint16_t fg, uint16_t bg,
                      int cx, int cy, int cw, int ch) {
    // Bar top surface with overhang
    canvas.fillRect(cx - 4, cy, cw + 8, 4, fg);
    // Front panel
    canvas.fillRect(cx, cy + 4, cw, ch - 4, fg);
    // Interior detail (dark gray fill — cabinet depth)
    canvas.fillRect(cx + 4, cy + 8, cw - 8, ch - 16, RP::D_FILL);
    // Horizontal rails on front panel
    canvas.fillRect(cx + 4, cy + 12, cw - 8, 4, fg);
    canvas.fillRect(cx + 4, cy + 20, cw - 8, 4, fg);
    // Bar top wear marks
    canvas.fillRect(cx + 8, cy, 4, 4, RP::D_FILL);
    canvas.fillRect(cx + 24, cy, 4, 4, RP::SHADOW_C);
    canvas.fillRect(cx + 36, cy, 4, 4, RP::D_FILL);
    // Base panel to floor (scaling gap fix — counter must sit on floor)
    int baseGap = kFloorY - cy - ch;
    if (baseGap > 0) {
        canvas.fillRect(cx, cy + ch, cw, baseGap, fg);
        // Kick plate recess
        canvas.fillRect(cx + 4, cy + ch + 4, cw - 8, baseGap - 4, RP::D_FILL);
    }
    // Ground shadow at base
    canvas.fillRect(cx - 4, kFloorY, cw + 8, 4, RP::SHADOW_C);
}

// Noodle bowl — WARM-tinted solid, no contour. Optional simple held mode.
void drawNoodleBowl(M5Canvas& canvas, uint16_t fg, uint16_t bg,
                    int bx, int by, int bw, int bh, bool noHaloSolid,
                    uint16_t bowlColor, int px) {
    const int p = px;
    const bool noir = (bowlColor != 0);
    const uint16_t bowl = noir ? bowlColor : RP::WARM;
    const uint16_t broth = noir ? RP::D_FILL : RP::FILL;
    if (noHaloSolid) {
        // Held bowl: compact solid form, no outline.
        canvas.fillRect(bx, by, bw, p, bowl);                               // rim
        canvas.fillRect(bx + p, by + p, bw - 2 * p, bh - 2 * p, bowl);     // body
        canvas.fillRect(bx + 2 * p, by + bh - p, bw - 4 * p, p, bowl);     // base
        canvas.fillRect(bx + 2 * p, by + 2 * p, bw - 4 * p, bh - 4 * p, broth);  // broth
        return;
    }

    // Rim and shoulder use full fat-pixel height for grid compliance.
    const int rimH = p;
    const int shoulderH = p;
    const int taperY = by + bh - 2 * p;
    const int sideTopY = by + rimH + shoulderH;
    int sideH = taperY - sideTopY;
    if (sideH < 1) sideH = 1;

    // Solid bowl with stepped curved bottom — no contour
    canvas.fillRect(bx - p, by, bw + 2 * p, rimH, bowl);            // wide rim
    canvas.fillRect(bx, by + rimH, bw, shoulderH, bowl);            // upper shoulder
    canvas.fillRect(bx, sideTopY, bw, sideH, bowl);                 // body
    canvas.fillRect(bx + p, by + bh - 2 * p, bw - 2 * p, p, bowl);  // taper row
    canvas.fillRect(bx + 2 * p, by + bh - p, bw - 4 * p, p, bowl);  // curve row 1
    canvas.fillRect(bx + 3 * p, by + bh, bw - 6 * p, p, bowl);      // curve row 2

    // Broth interior
    int brothY = by + rimH + shoulderH + p;
    int brothH = taperY - brothY;
    if (brothH > 0) {
        canvas.fillRect(bx + p, brothY, bw - 2 * p, brothH, broth);
    }
    // Noodle squiggles — clamp to bowl interior
    int noodleBaseY = brothY + p;
    for (int i = 0; i < 3; i++) {
        int nx = bx + 2 * p + i * 3 * p;
        if (nx + 2 * p > bx + bw - p) break;
        int ny = noodleBaseY + (i & 1) * p;
        canvas.fillRect(nx, ny, 2 * p, p, bowl);
        canvas.fillRect(nx + p, ny + p, 2 * p, p, bowl);
    }
    // Chopsticks top-right — structural color for contrast
    canvas.fillRect(bx + bw - 3 * p, by - 2 * p, p, p, fg);
    canvas.fillRect(bx + bw - 2 * p, by - p, p, p, fg);
    canvas.fillRect(bx + bw - 4 * p, by - 2 * p, p, p, fg);
    canvas.fillRect(bx + bw - 3 * p, by - p, p, p, fg);
    // Broth stain kept below rim so top contour stays fully solid.
    canvas.fillRect(bx + 2 * p, by + p, p, p, broth);
}

// Bar stool — seat + fat center leg + foot ring (4px grid)
void drawBarStool(M5Canvas& canvas, uint16_t fg, int sx, int sy, int sw) {
    // Seat bar (tilted — left side lower)
    canvas.fillRect(sx, sy + 4, 4, 4, fg);
    canvas.fillRect(sx + 4, sy, sw - 4, 4, fg);
    // Seat thickness
    canvas.fillRect(sx + 4, sy + 4, sw - 8, 4, RP::FILL);
    // Center leg (4px wide)
    canvas.fillRect(sx + sw / 2 - 4, sy + 4, 4, kFloorY - sy - 8, fg);
    // Foot ring
    canvas.fillRect(sx + 4, kFloorY - 4, sw - 8, 4, fg);
    // Scratch on foot ring
    canvas.fillRect(sx + 4, kFloorY - 4, 4, 4, RP::FILL);
    // Ground shadow under foot ring
    canvas.fillRect(sx + 4, kFloorY, sw - 8, 4, RP::SHADOW_C);
}

// Desk lamp — STRUCT arm + WARM amber light cone (4px grid)
void drawDeskLamp(M5Canvas& canvas, uint32_t now, int lx, int ly) {
    // Lamp base
    canvas.fillRect(lx, ly + 28, 16, 4, RP::STRUCT);
    // Arm (angled)
    canvas.fillRect(lx + 8, ly + 8, 4, 24, RP::STRUCT);
    canvas.fillRect(lx + 4, ly + 4, 4, 8, RP::STRUCT);
    // Shade
    canvas.fillRect(lx - 4, ly, 16, 4, RP::STRUCT);
    // Amber light cone (dithered trapezoid)
    for (int dy = 4; dy < 24; dy += kRoomPX) {
        int spread = dy / 2;
        int coneX = lx + 4 - spread;
        int coneW = 8 + spread * 2;
        for (int px = coneX; px < coneX + coneW; px += kRoomPX) {
            if ((wallHash(px, ly + dy, 0x1A01u) & 0xFFu) < (uint8_t)(80 - dy))
                canvas.fillRect(px & ~3, (ly + dy) & ~3, kRoomPX, kRoomPX, RP::WARM);
        }
    }
}

// Wall monitor — small wall-mounted screen with random data content (4px grid)
void drawWallMonitor(M5Canvas& canvas, uint32_t now, int mx, int my, int mw, int mh, uint32_t seed) {
    // Bezel
    canvas.fillRect(mx, my, mw, mh, RP::STRUCT);
    // Screen interior
    int sx = mx + 4, sy = my + 4, sw = mw - 8, sh = mh - 8;
    canvas.fillRect(sx, sy, sw, sh, RP::BG);
    // Random content lines (seeded per monitor, shift over time)
    for (int ly = sy, row = 0; ly + 4 <= sy + sh; ly += 4, ++row) {
        // Stagger row refreshes so the monitor scrolls instead of replacing
        // its entire contents on one frame boundary.
        uint32_t linePhase = (now + (uint32_t)row * 113u) / 600u + seed;
        uint32_t h = wallHash(mx, ly, linePhase);
        int lineW = 4 + (int)(h % (uint32_t)(sw - 4));
        lineW = (lineW + 3) & ~3;
        if (lineW > sw) lineW = sw;
        uint16_t lineColor = ((h >> 16) & 1) ? RP::CRT : RP::NEON;
        canvas.fillRect(sx, ly, lineW, 4, lineColor);
    }
    // Wall mount bracket
    canvas.fillRect(mx + mw / 2 - 4, my + mh, 4, 4, RP::STRUCT);
    // Glow on wall below
    drawLightPool(canvas, RP::CRT, mx - 4, my + mh + 4, mw + 8, 4, 20, seed + 1000);
}

// Server cluster — 3 stacked boxes with blinking LEDs
void drawServerCluster(M5Canvas& canvas, uint32_t now, int sx, int sy, int sw, int sh, int dx) {
    int fx = sx + dx;
    int unitH = (sh / 3) & ~3;  // snap to 4px grid — prevents diagonal artifacts
    for (int u = 0; u < 3; u++) {
        int uy = (sy + u * unitH) & ~3;  // snap each unit Y
        // Server box (4px grid)
        canvas.fillRect(fx, uy, sw, unitH - 4, RP::STRUCT);
        canvas.fillRect(fx + 4, uy + 4, sw - 8, unitH - 8, RP::BG);
        // Ventilation slits
        for (int vy = uy + 8; vy + 4 <= uy + unitH - 8; vy += 8) {
            canvas.fillRect(fx + 8, vy, sw - 16, 4, RP::STRUCT);
        }
        // Rack rash (scuff from sliding units)
        if (u == 1) canvas.fillRect(fx + 4, uy + unitH - 4, 4, 4, RP::FILL);
        // LED indicator (per-unit independent blink)
        bool ledOn = (((now / (400u + (uint32_t)u * 120u)) + (uint32_t)u) & 1u) == 0u;
        if (ledOn) {
            uint16_t col = Avatar::getHypeColor((int16_t)(fx + sw - 8), (int16_t)(uy + unitH / 2));
            canvas.fillRect(fx + sw - 8, uy + unitH / 2 - 2, 4, 4, col);
            drawLightPool(canvas, col, fx + sw - 10, uy + unitH / 2, 6, 4, 22, 66066u + u);
        }
    }
    // Rack legs (4px grid)
    canvas.fillRect(fx + 4, sy + sh, 4, kFloorY - sy - sh, RP::STRUCT);
    canvas.fillRect(fx + sw - 8, sy + sh, 4, kFloorY - sy - sh, RP::STRUCT);
    // Ground shadows
    canvas.fillRect(fx, kFloorY, 8, 4, RP::SHADOW_C);
    canvas.fillRect(fx + sw - 8, kFloorY, 8, 4, RP::SHADOW_C);
}

// Tech crate — circuit trace markings, corner brackets (4px grid)
void drawAlleyCrate(M5Canvas& canvas, uint16_t fg, uint16_t bg) {
    int bx = kBenchX, by = kBenchY, bw = kBenchW, bh = kBenchH;
    canvas.fillRect(bx, by, bw, bh, fg);
    canvas.fillRect(bx + 4, by + 4, bw - 8, bh - 8, bg);
    // Corner brackets
    canvas.fillRect(bx + 4, by + 4, 8, 4, fg);
    canvas.fillRect(bx + 4, by + 4, 4, 8, fg);
    canvas.fillRect(bx + bw - 8, by + 4, 8, 4, fg);
    canvas.fillRect(bx + bw - 4, by + 4, 4, 8, fg);
    canvas.fillRect(bx + 4, by + bh - 4, 8, 4, fg);
    canvas.fillRect(bx + 4, by + bh - 8, 4, 8, fg);
    canvas.fillRect(bx + bw - 8, by + bh - 4, 8, 4, fg);
    canvas.fillRect(bx + bw - 4, by + bh - 8, 4, 8, fg);
    // Circuit traces
    canvas.fillRect(bx + 12, by + 8, 16, 4, fg);
    canvas.fillRect(bx + 28, by + 4, 4, 8, fg);
    canvas.fillRect(bx + 32, by + 8, 12, 4, fg);
    // Scuff mark on crate top surface
    canvas.fillRect(bx + 20, by, 4, 4, RP::FILL);
    // Legs
    canvas.fillRect(bx + 4, by + bh, 4, kFloorY - by - bh, fg);
    canvas.fillRect(bx + bw - 8, by + bh, 4, kFloorY - by - bh, fg);
    // Ground shadows under legs
    canvas.fillRect(bx, kFloorY, 8, 4, RP::SHADOW_C);
    canvas.fillRect(bx + bw - 8, kFloorY, 8, 4, RP::SHADOW_C);
}

// Ceiling pipe with mounting brackets (4px grid)
void drawAlleyPipe(M5Canvas& canvas, uint16_t fg) {
    int px = kPipeX, py = kPipeY;
    // Horizontal pipe
    canvas.fillRect(px, py, kPipeW, 4, fg);
    // Underside shadow (pipe is round)
    canvas.fillRect(px, py + 4, kPipeW, 4, RP::SHADOW_C);
    // Top highlight (round pipe catches light)
    canvas.fillRect(px, py, kPipeW, 4, RP::FILL);
    // Mounting brackets
    canvas.fillRect(px + 8, py - 4, 4, 4, fg);
    canvas.fillRect(px + 8, py - 4, 4, 8, fg);
    canvas.fillRect(px + 36, py - 4, 4, 4, fg);
    canvas.fillRect(px + 36, py - 4, 4, 8, fg);
    canvas.fillRect(px + kPipeW - 8, py - 4, 4, 4, fg);
}

// ==[ ENVIRONMENTAL CLUTTER ]== dense, rain-soaked industrial props

// Exposed conduit run — horizontal 2px cable with junction box
void drawConduitRun(M5Canvas& canvas, uint16_t fg, int x, int y, int w) {
    // Cable (4px grid)
    canvas.fillRect(x, y, w, 4, fg);
    // Top highlight
    canvas.fillRect(x + 4, y, w - 8, 4, RP::FILL);
    // Junction box
    int bx = (x + w / 3) & ~3;
    canvas.fillRect(bx, y - 4, 8, 8, fg);
    // Mounting clips
    canvas.fillRect(x + 8, y - 4, 4, 8, fg);
    canvas.fillRect(x + w - 12, y - 4, 4, 8, fg);
    // Cable sag
    canvas.fillRect(x + w / 2, y + 4, 4, 4, fg);
    // Paint peel at junction edge
    canvas.fillRect(bx + 8, y, 4, 4, RP::SHADOW_C);
}

// Loose cable coil on floor (4px grid)
void drawCableCoil(M5Canvas& canvas, uint16_t fg, int x, int y) {
    canvas.fillRect(x, y, 4, 4, fg);
    canvas.fillRect(x + 4, y - 4, 4, 4, fg);
    canvas.fillRect(x + 4, y, 4, 4, fg);
    canvas.fillRect(x, y + 4, 4, 4, fg);
    canvas.fillRect(x + 8, y - 4, 4, 4, fg);  // tail
    // Frayed end highlight
    canvas.fillRect(x + 8, y - 4, 4, 4, RP::FILL);
}

// Wall outlet (4px grid)
void drawWallOutlet(M5Canvas& canvas, uint16_t fg, uint16_t bg, int x, int y) {
    canvas.fillRect(x, y, 8, 8, fg);
    // Top edge highlight
    canvas.fillRect(x + 4, y, 4, 4, RP::FILL);
    canvas.fillRect(x, y + 4, 4, 4, bg);
    canvas.fillRect(x + 4, y + 4, 4, 4, RP::SHADOW_C);
}

// Graffiti mark (4px grid)
void drawGraffiti(M5Canvas& canvas, uint16_t fg, uint16_t bg, int x, int y) {
    canvas.fillRect(x, y, 8, 8, fg);
    canvas.fillRect(x, y + 4, 4, 4, bg);
    canvas.fillRect(x + 4, y + 4, 4, 4, bg);
}

// Wall-mounted fuse box (4px grid)
void drawFuseBox(M5Canvas& canvas, uint16_t fg, uint16_t bg, int x, int y) {
    // Box body
    canvas.fillRect(x, y, 12, 16, fg);
    canvas.fillRect(x + 4, y + 4, 8, 12, bg);
    // Open door (hinge on left)
    canvas.fillRect(x - 8, y + 4, 8, 16, fg);
    canvas.fillRect(x - 4, y + 4, 4, 12, bg);
    // Hinge dots
    canvas.fillRect(x - 4, y + 4, 4, 4, fg);
    canvas.fillRect(x - 4, y + 12, 4, 4, fg);
    // Interior switches
    canvas.fillRect(x + 4, y + 4, 8, 4, fg);
    canvas.fillRect(x + 4, y + 8, 8, 4, fg);
    canvas.fillRect(x + 4, y + 12, 8, 4, fg);
    // Switch handles
    canvas.fillRect(x + 4, y + 4, 4, 4, fg);
    canvas.fillRect(x + 8, y + 8, 4, 4, fg);
    canvas.fillRect(x + 4, y + 12, 4, 4, fg);
    // Scorch mark
    canvas.fillRect(x + 4, y + 12, 4, 4, RP::SHADOW_C);
}

// Wall poster (4px grid)
void drawWallPoster(M5Canvas& canvas, uint16_t fg, uint16_t bg, int x, int y, int w, int h) {
    canvas.fillRect(x, y, w, h, fg);
    canvas.fillRect(x + 4, y + 4, w - 8, h - 8, bg);
    // Abstract content bars
    canvas.fillRect(x + 4, y + 4, w - 8, 4, fg);
    canvas.fillRect(x + 4, y + h / 2, w / 2, 4, fg);
    // Torn corner
    canvas.fillRect(x + w - 4, y, 4, 4, RP::BG);
    canvas.fillRect(x + w - 4, y + 4, 4, 4, RP::BG);
}

// Ceiling-mounted fire sprinkler (4px grid)
void drawSprinkler(M5Canvas& canvas, uint16_t fg, int x, int y) {
    canvas.fillRect(x, y, 4, 4, fg);
    canvas.fillRect(x - 4, y + 4, 8, 4, fg);
    // Paint flake on deflector
    canvas.fillRect(x - 4, y + 4, 4, 4, RP::FILL);
}

// Small wall clock (4px grid)
void drawWallClock(M5Canvas& canvas, uint16_t fg, uint16_t bg, int x, int y) {
    canvas.fillRect(x, y, 8, 8, fg);
    // Top highlight
    canvas.fillRect(x + 4, y, 4, 4, RP::FILL);
    canvas.fillRect(x, y + 4, 4, 4, bg);
    // Hands
    canvas.fillRect(x + 4, y + 4, 4, 4, fg);
    // Yellowed dial
    canvas.fillRect(x, y + 4, 4, 4, RP::SHADOW_C);
}

// Empty bottle/can on floor (4px grid)
void drawFloorBottle(M5Canvas& canvas, uint16_t fg, int x, int y) {
    canvas.fillRect(x, y, 4, 4, fg);
    canvas.fillRect(x + 4, y, 4, 4, RP::FILL);
    // Top glint
    canvas.fillRect(x + 4, y - 4, 4, 4, RP::WALL_NEAR);
}

// Wall-mounted fire extinguisher (4px grid)
void drawFireExtinguisher(M5Canvas& canvas, uint16_t fg, int x, int y) {
    canvas.fillRect(x, y, 4, 4, fg);        // handle
    canvas.fillRect(x, y + 4, 8, 12, fg);   // body
    // Top highlight
    canvas.fillRect(x, y + 4, 8, 4, RP::FILL);
    canvas.fillRect(x + 4, y + 8, 4, 4, RP::FILL); // label
    // Dent on lower body
    canvas.fillRect(x + 4, y + 12, 4, 4, RP::SHADOW_C);
}

// Vent grate on wall (4px grid)
void drawSmallVent(M5Canvas& canvas, uint16_t fg, uint16_t bg, int x, int y) {
    canvas.fillRect(x, y, 12, 8, fg);
    // Top edge highlight
    canvas.fillRect(x + 4, y, 8, 4, RP::FILL);
    canvas.fillRect(x + 4, y + 4, 4, 4, bg);
    canvas.fillRect(x + 8, y + 4, 4, 4, bg);
    // Dust clog
    canvas.fillRect(x + 8, y + 4, 4, 4, RP::SHADOW_C);
}

// Trash bag on floor (4px grid)
void drawTrashBag(M5Canvas& canvas, uint16_t fg, int x, int y) {
    canvas.fillRect(x, y, 8, 8, fg);
    canvas.fillRect(x + 4, y - 4, 4, 4, fg);  // tie
    // Top highlight (plastic sheen)
    canvas.fillRect(x + 4, y, 4, 4, RP::FILL);
    canvas.fillRect(x, y + 4, 4, 4, RP::FILL);  // wrinkle
    // Wet stain at base
    canvas.fillRect(x, y + 4, 4, 4, RP::SHADOW_C);
}

// AC unit on wall (4px grid)
void drawACUnit(M5Canvas& canvas, uint16_t fg, uint16_t bg, int x, int y) {
    // Body
    canvas.fillRect(x, y, 20, 16, fg);
    // Vent slats
    for (int s = 0; s < 3; s++) {
        canvas.fillRect(x + 4, y + 4 + s * 4, 12, 4, bg);
    }
    // Mounting brackets
    canvas.fillRect(x - 4, y, 4, 4, fg);
    canvas.fillRect(x + 20, y, 4, 4, fg);
    // Rust stain
    canvas.fillRect(x + 16, y + 4, 4, 4, RP::FILL);
    // Wall shadow below unit
    canvas.fillRect(x, y + 16, 20, 4, RP::SHADOW_C);
    // Drip mark below
    canvas.fillRect(x + 12, y + 16, 4, 4, fg);
}

// Ashtray on surface (4px grid)
void drawAshtray(M5Canvas& canvas, uint16_t fg, uint16_t bg, int x, int y) {
    canvas.fillRect(x, y, 8, 4, fg);
    canvas.fillRect(x + 4, y, 4, 4, bg);
    // Cigarette diagonal
    canvas.fillRect(x + 8, y - 4, 4, 4, fg);
    canvas.fillRect(x + 12, y - 8, 4, 4, fg);
}

// Ceiling stain — irregular dithered cluster
void drawCeilingStain(M5Canvas& canvas, uint16_t fg, int x, int y) {
    for (int py = 0; py < 12; py += kRoomPX) {
        for (int px = 0; px < 14; px += kRoomPX) {
            // Rough circle via distance check
            int dx = px - 6, dy = py - 5;
            if (dx * dx + dy * dy > 40) continue;  // outside ~radius 6
            // Dither: ~60% fill for organic look
            if ((wallHash(px, py, 67891) & 0xFF) < 153)
                canvas.fillRect(x + px, y + py, kRoomPX, kRoomPX, fg);
        }
    }
}

// Menu board — rectangle with "text lines" inside (4px grid)
void drawMenuBoard(M5Canvas& canvas, uint16_t fg, uint16_t bg, int x, int y) {
    // Cast shadow (wall-mounted, below and right)
    canvas.fillRect(x + 4, y + 16, 24, 4, RP::SHADOW_C);
    canvas.fillRect(x + 24, y + 4, 4, 16, RP::SHADOW_C);
    canvas.fillRect(x, y, 24, 16, fg);
    // Top edge highlight (frame catch)
    canvas.fillRect(x + 4, y, 16, 4, RP::FILL);
    canvas.fillRect(x + 4, y + 4, 16, 12, bg);
    // Text lines (alternating bars, slightly uneven widths)
    canvas.fillRect(x + 4, y + 4, 16, 4, fg);
    canvas.fillRect(x + 4, y + 8, 12, 4, fg);
    canvas.fillRect(x + 8, y + 12, 8, 4, fg);
    // Chalk smudge on board surface
    canvas.fillRect(x + 16, y + 4, 4, 4, RP::FILL);
}

// Patch panel — dense sockets and toggles, good filler for lab walls (4px grid)
void drawPatchPanel(M5Canvas& canvas, uint16_t fg, uint16_t bg, int x, int y) {
    canvas.fillRect(x, y, 16, 16, fg);
    // Top edge highlight (metal bezel catch)
    canvas.fillRect(x + 4, y, 8, 4, RP::FILL);
    canvas.fillRect(x + 4, y + 4, 12, 12, bg);
    for (int r = 0; r < 3; r++) {
        int ry = y + 4 + r * 4;
        canvas.fillRect(x + 4, ry, 8, 4, fg);
    }
    canvas.fillRect(x + 12, y + 4, 4, 4, fg);
    canvas.fillRect(x + 12, y + 12, 4, 4, fg);
    canvas.fillRect(x + 12, y + 4, 4, 4, RP::FILL);
    canvas.fillRect(x + 12, y + 12, 4, 4, RP::SHADOW_C);
    // Dust in bottom port row
    canvas.fillRect(x + 4, y + 12, 4, 4, RP::SHADOW_C);
}

// Service case — low floor prop for the lab side (4px grid)
void drawServiceCase(M5Canvas& canvas, uint16_t fg, uint16_t bg, int x, int y) {
    canvas.fillRect(x, y, 20, 8, fg);
    // Top highlight (case lid catch)
    canvas.fillRect(x + 4, y, 12, 4, RP::FILL);
    canvas.fillRect(x + 4, y + 4, 12, 4, bg);
    canvas.fillRect(x + 8, y - 4, 8, 4, fg);
    canvas.fillRect(x + 4, y + 4, 4, 4, fg);
    canvas.fillRect(x + 12, y + 4, 4, 4, fg);
    // Corner scuff (dropped)
    canvas.fillRect(x, y + 8, 4, 4, RP::SHADOW_C);
    canvas.fillRect(x + 4, y + 8, 4, 4, fg);
    canvas.fillRect(x + 16, y + 8, 4, 4, fg);
    canvas.fillRect(x - 4, y + 12, 28, 4, RP::SHADOW_C);
}

// Sake bottle — chunky neck + wide body (4px grid)
void drawSakeBottle(M5Canvas& canvas, uint16_t fg, int x, int y) {
    // Neck (tilted)
    canvas.fillRect(x + 4, y, 4, 4, fg);
    canvas.fillRect(x + 4, y + 4, 4, 4, fg);
    // Body
    canvas.fillRect(x, y + 4, 8, 8, fg);
    // Body fill inset
    canvas.fillRect(x + 4, y + 8, 4, 4, RP::FILL);
    // Label wear mark
    canvas.fillRect(x + 4, y + 8, 4, 4, RP::SHADOW_C);
    // Ground shadow
    canvas.fillRect(x - 4, y + 12, 16, 4, RP::SHADOW_C);
}

// Floor drain — chunky grate with center hole (4px grid)
void drawFloorDrain(M5Canvas& canvas, uint16_t fg, uint16_t bg, int x, int y) {
    canvas.fillRect(x, y, 12, 8, fg);
    // Top rim catches light; bottom row keeps two posts around the dark throat.
    canvas.fillRect(x, y, 12, 4, RP::FILL);
    canvas.fillRect(x + 4, y, 4, 4, fg);
    canvas.fillRect(x + 4, y + 4, 4, 4, bg);
    // Crud buildup at edge
    canvas.fillRect(x - 4, y + 4, 4, 4, RP::SHADOW_C);
}

} // namespace MenuPigRender
