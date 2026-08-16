/**
 * wardrive_city_backdrop.cpp — city skyline, tower grid, fog, traffic, comms bubble
 *
 * Extracted from wardrive_scene.cpp (~1820 lines of city backdrop code).
 * Manages: PSRAM tower grid, 3-layer parallax city, sky gradient, fog/haze,
 * window lights, neon signs, traffic headlights, air traffic volumetric beams,
 * steam vents, searchlight, horizon shadow pass, tower comms speech bubble,
 * and backdrop cache management.
 *
 * All rendering targets the glass region (WD_GLASS_T .. WD_GLASS_B).
 * PSRAM tower slots allocated in WardriveScene::reset(), freed in shutdown().
 */

#include "wardrive_city_backdrop.h"
#include "wardrive_shared.h"
#include "../util/debug_log.h"
#include "wardrive_glass.h"
#include "../ui/display.h"
#include "../ui/menu_pig_render.h"
#include <math.h>
#include <string.h>

static constexpr float PI_F = 3.14159265f;

using namespace MenuPigRender;

namespace WardriveScene {

// ==[ PSRAM SCENE BUFFERS ]== allocated in reset(), freed in shutdown()
CityTowerSlot* sceneTowerSlots = nullptr;      // CITY_TOWER_SLOT_CAP entries
uint16_t* sceneVisibleIdx = nullptr;            // CITY_TOWER_SLOT_CAP entries

// City owns the expensive wide loops. Cache the structural PSRAM base at 30Hz;
// time-driven city motion is restored after that base at the full scene rate.
static M5Canvas* sceneBackdropCache = nullptr;
static bool sceneBackdropCacheFailureLatched = false;
uint32_t sceneBackdropCacheLastMs = UINT32_MAX;
static bool sceneBackdropGeometryValid = false;
static float sceneBackdropSteer = 0.0f;
static float sceneBackdropPitch = 0.0f;
static int sceneBackdropHorizonY = 0;
static int sceneBackdropMidLift = 0;
static int sceneBackdropSkyLift = 0;
static int sceneVisibleTowerCount = 0;

// BackdropCache exposed for orchestrator (matches shared.h extern)
BackdropCache backdropCache = {};

// ==[ CITY CACHE ]== world-space tower grid, generated once, viewport-scrolled per frame
int   refHorizonY     = 0;     // horizonY at generation time (pitch=0)
int   cachedTowerCount = 0;
bool  cityGridValid   = false;

// ==[ CACHED CITY PALETTE ]== recomputed on theme change, not every frame
CityPalette cityPal = {};

// ==[ COMMS TOWER ANCHOR ]== saved when bubble first appears — prevents teleport during steer
// (declared early — updateCityPalette() needs hasCommsTower on grid invalidation)
uint16_t savedCommsIdx = 0;   // index into sceneTowerSlots (stable across frames)
bool hasCommsTower = false;

// ═══════════════════════════════════════════════════════════════════════════
// BACKDROP CACHE MANAGEMENT
// ═══════════════════════════════════════════════════════════════════════════

bool prepareSceneBackdropCache() {
    // reset() is the only allocation point. A failed attempt stays latched for
    // the scene session so drawScene() can use the direct-render fallback
    // without retrying heap work every frame.
    sceneBackdropCacheFailureLatched = false;
    backdropCache.canvas = nullptr;
    backdropCache.lastMs = UINT32_MAX;
    sceneBackdropGeometryValid = false;
    sceneVisibleTowerCount = 0;

    if (!sceneBackdropCache) {
        sceneBackdropCache = new M5Canvas(&M5.Display);
        if (!sceneBackdropCache) {
            sceneBackdropCacheFailureLatched = true;
            HAMLET_LOGLN("[WARDRIVE] alloc failed: sceneBackdropCache canvas");
            return false;
        }
        sceneBackdropCache->setPsram(true);
        sceneBackdropCache->setColorDepth(16);
    }
    if (sceneBackdropCache->width() != SCREEN_WIDTH ||
        sceneBackdropCache->height() != WD_GLASS_B ||
        sceneBackdropCache->getBuffer() == nullptr) {
        sceneBackdropCache->deleteSprite();
        if (!sceneBackdropCache->createSprite(SCREEN_WIDTH, WD_GLASS_B)) {
            sceneBackdropCacheFailureLatched = true;
            HAMLET_LOGLN("[WARDRIVE] PSRAM alloc failed: sceneBackdropCache");
            return false;
        }
    }
    backdropCache.canvas = sceneBackdropCache;
    backdropCache.lastMs = sceneBackdropCacheLastMs;
    if (!sceneBackdropCache->getBuffer()) {
        sceneBackdropCacheFailureLatched = true;
        backdropCache.canvas = nullptr;
        HAMLET_LOGLN("[WARDRIVE] PSRAM cache has no backing buffer");
        return false;
    }
    return true;
}

bool isSceneBackdropCacheReady() {
    return !sceneBackdropCacheFailureLatched &&
           backdropCache.canvas != nullptr &&
           backdropCache.canvas->getBuffer() != nullptr;
}

void releaseSceneBackdropCache() {
    if (sceneBackdropCache) {
        sceneBackdropCache->deleteSprite();
        delete sceneBackdropCache;
        sceneBackdropCache = nullptr;
    }
    sceneBackdropCacheFailureLatched = false;
    sceneBackdropCacheLastMs = UINT32_MAX;
    sceneBackdropGeometryValid = false;
    sceneVisibleTowerCount = 0;
    backdropCache.canvas = nullptr;
    backdropCache.lastMs = UINT32_MAX;
}

// ═══════════════════════════════════════════════════════════════════════════
// CITY PALETTE
// ═══════════════════════════════════════════════════════════════════════════

void updateCityPalette() {
    uint16_t fg = Display::getColorFG();
    if (cityPal.cacheKeyBG == RP::BG && cityPal.cacheKeyFG == fg &&
        cityPal.cacheKeyNEON == RP::NEON && cityPal.cacheKeyWARM == RP::WARM &&
        cityPal.cacheKeyCRT == RP::CRT && cityPal.cacheKeyVEND == RP::VEND &&
        cityPal.cacheKeyFLUOR == RP::FLUOR && cityPal.cacheKeySPARK == RP::SPARK) return;
    cityPal.cacheKeyBG = RP::BG;
    cityPal.cacheKeyFG = fg;
    cityPal.cacheKeyNEON = RP::NEON;
    cityPal.cacheKeyWARM = RP::WARM;
    cityPal.cacheKeyCRT = RP::CRT;
    cityPal.cacheKeyVEND = RP::VEND;
    cityPal.cacheKeyFLUOR = RP::FLUOR;
    cityPal.cacheKeySPARK = RP::SPARK;
    cityGridValid = false;  // theme change → tower colors stale
    hasCommsTower = false;  // tower array will repopulate — stale savedCommsIdx invalid

    const bool isTheOg = Display::isTheOgTheme();
    auto peak = [isTheOg](uint16_t source, uint8_t amount) {
        return isTheOg ? source : Display::screenBlend565(source, RP::FLUOR, amount);
    };
    cityPal.cityNeon = peak(RP::NEON, 80);
    cityPal.cityAmber = peak(RP::WARM, 70);
    cityPal.towerNear = Display::lerpColor565(RP::BG, RP::DEEP, 0.65f);
    cityPal.towerMid  = Display::lerpColor565(RP::DEEP, RP::SHADOW_C, 0.40f);
    cityPal.towerFar  = Display::lerpColor565(RP::WALL_FAR, RP::WALL_MID, 0.35f);
    cityPal.towerHaze = Display::lerpColor565(RP::WALL_MID, RP::SOFT, 0.40f);
    cityPal.towerCol  = cityPal.towerMid;
    cityPal.fogGray  = Display::lerpColor565(RP::WALL_MID, RP::WALL_NEAR, 0.55f);
    cityPal.fogCool  = Display::lerpColor565(cityPal.fogGray, cityPal.cityNeon, 0.22f);
    cityPal.fogWarm  = Display::lerpColor565(cityPal.fogGray, cityPal.cityAmber, 0.20f);
    cityPal.haze     = Display::lerpColor565(RP::SOFT, RP::WALL_NEAR, 0.30f);
    cityPal.skyZenith = Display::lerpColor565(RP::DEEP, RP::SHADOW_C, 0.38f);          // murky, not black
    cityPal.skyTop   = Display::lerpColor565(RP::SHADOW_C, RP::WALL_FAR, 0.35f);      // slightly brighter than original
    cityPal.skyMid   = Display::lerpColor565(Display::lerpColor565(RP::SOFT, RP::WALL_NEAR, 0.30f), cityPal.fogWarm, 0.28f); // subtle warm
    cityPal.deckHaze = Display::lerpColor565(RP::SHADOW_C, RP::WALL_MID, 0.55f);
    cityPal.roadGlow = Display::lerpColor565(cityPal.cityAmber, cityPal.fogWarm, 0.20f);
    cityPal.roofSilhouetteCol = Display::lerpColor565(RP::SHADOW_C, cityPal.towerMid, 0.40f);
    cityPal.layerTowerCol[0] = Display::lerpColor565(cityPal.fogGray, cityPal.haze, 0.80f);   // washed out haze
    cityPal.layerTowerCol[1] = Display::lerpColor565(cityPal.towerFar, cityPal.fogGray, 0.40f);
    cityPal.layerTowerCol[2] = Display::lerpColor565(cityPal.towerNear, RP::DEEP, 0.20f);    // dark monoliths

    // window slit palette
    cityPal.slitPalette[0] = peak(cityPal.fogCool, 40);
    cityPal.slitPalette[1] = peak(RP::VEND, 50);
    cityPal.slitPalette[2] = peak(RP::WARM, 50);
    cityPal.slitPalette[3] = peak(RP::NEON, 60);
    cityPal.slitPalette[4] = peak(RP::CRT, 50);

    // neon sign colors
    cityPal.signColors[0] = peak(RP::NEON, 70);
    cityPal.signColors[1] = peak(RP::CRT, 60);
    cityPal.signColors[2] = peak(RP::WARM, 60);
    cityPal.signColors[3] = peak(RP::VEND, 55);

    // traffic colors
    uint16_t headlightPeak = isTheOg ? RP::SPARK : RP::FLUOR;
    cityPal.headlightWarm = Display::screenBlend565(cityPal.cityAmber, headlightPeak, 50);
    cityPal.headlightCool = Display::screenBlend565(cityPal.cityNeon, headlightPeak, 50);
    cityPal.hlVariety[0] = cityPal.headlightWarm;
    cityPal.hlVariety[1] = cityPal.headlightCool;
    cityPal.hlVariety[2] = headlightPeak;
    cityPal.hlVariety[3] = Display::screenBlend565(cityPal.cityAmber, headlightPeak, 30);
}

// ═══════════════════════════════════════════════════════════════════════════
// TOWER GRID HELPERS (internal)
// ═══════════════════════════════════════════════════════════════════════════

static inline int cityExtraCols(int slotW) {
    return max(1, CITY_OVERSCAN_X / max(PX, slotW) + 1);
}

static inline int cityExtraRows(int rowStep) {
    return max(1, CITY_OVERSCAN_Y / max(PX, rowStep) + 1);
}

static inline CityTowerSlot cityTowerSlot(int col, int row, int worldX, int worldY, float depth,
                                          uint32_t salt = 0x5A17u,
                                          int slotW = PX * 6, int rowStep = PX * 7, int baseY = 52,
                                          int minWCells = 3, int maxWCells = 6,
                                          int minHCells = 6, int maxHCells = 17) {
    int colBase, xPhase, rowBase, yPhase;
    wrappedPhase(worldX, slotW, colBase, xPhase);
    wrappedPhase(worldY, rowStep, rowBase, yPhase);
    int worldCol = colBase + col;
    int worldRow = rowBase + row;
    uint32_t seed = wallHash(worldCol * 13 + (int)(depth * 17.0f), worldRow * 17, salt);
    int bwCells = minWCells + (int)((seed >> 7) % (uint32_t)max(1, maxWCells - minWCells + 1));
    int bhCells = minHCells + (int)((seed >> 19) % (uint32_t)max(1, maxHCells - minHCells + 1));
    int bw = q(bwCells * PX);
    int bh = q(bhCells * PX);
    int xJitter = (int)(seed % (uint32_t)max(PX, slotW - bw + PX));
    int bx = q(col * slotW - xPhase + xJitter);
    int by = q(baseY + row * rowStep - yPhase + (int)((seed >> 13) & 0x3u) * PX);
    return {depth, seed, bx, by, bw, bh, (int16_t)bx, (int16_t)by};
}

static inline CityFeaturePoint cityFeatureSlot(int col, int row, int worldX, int worldY,
                                               int slotW, int rowStep, int baseY, uint32_t salt) {
    int colBase, xPhase, rowBase, yPhase;
    wrappedPhase(worldX, slotW, colBase, xPhase);
    wrappedPhase(worldY, rowStep, rowBase, yPhase);
    int worldCol = colBase + col;
    int worldRow = rowBase + row;
    uint32_t seed = wallHash(worldCol * 19, worldRow * 23, salt);
    int x = q(col * slotW - xPhase);
    int y = q(baseY + row * rowStep - yPhase);
    return {seed, x, y};
}

static inline void cityTowerRowBounds(const CityTowerSlot& slot, int y,
                                      int& rowLeft, int& rowRight, bool& roofCap) {
    rowLeft = slot.bx;
    rowRight = slot.bx + slot.bw;
    roofCap = false;

    int topY = slot.by - slot.bh;
    int rowFromTop = y - topY;
    if (rowFromTop < 0 || rowFromTop >= slot.bh) return;

    // ==[ LANDMARK SHAPES ]== wide+tall near towers get iconic BR2049 silhouettes
    if (slot.bw >= PX * 5 && slot.bh >= PX * 10 && slot.depth < 0.50f) {
        float rowT = (float)rowFromTop / (float)max(1, slot.bh);  // 0=top, 1=base
        uint8_t landmarkType = (uint8_t)((slot.seed >> 25) & 0x3u);
        if (landmarkType == 0u) {
            // TRUNCATED PYRAMID — LAPD HQ, flat top, angled sides
            float taperFrac = fminf((1.0f - rowT) * 0.75f, 0.30f);  // cap at 30% per side
            int taperEach = q((int)(taperFrac * (float)slot.bw));
            rowLeft += taperEach;
            rowRight -= taperEach;
        } else if (landmarkType == 1u) {
            // ZIGGURAT — 3-tier stepped taper
            int tier = min(2, (int)((1.0f - rowT) * 3.0f));
            int stepInset = q(tier * slot.bw / 8);
            rowLeft += stepInset;
            rowRight -= stepInset;
        } else if (landmarkType == 2u) {
            // SPIRE — narrow top 30%, wide base
            if (rowT < 0.30f) {
                int narrowInset = q((int)((0.30f - rowT) / 0.30f * (float)(slot.bw * 2 / 5)));
                rowLeft += narrowInset;
                rowRight -= narrowInset;
            }
        }
        // type 3: monolith — falls through to standard flat-top
        if (landmarkType < 3u) {
            roofCap = rowFromTop < PX * (3 + (int)((slot.seed >> 9) & 0x1u));
            rowLeft = q(max(rowLeft, slot.bx));
            rowRight = q(min(rowRight, slot.bx + slot.bw));
            if (rowRight - rowLeft < PX * 2) {
                int mid = (slot.bx + slot.bx + slot.bw) / 2;
                rowLeft = q(mid - PX);
                rowRight = q(mid + PX);
            }
            return;
        }
    }

    int topRows = max(2, min(6, slot.bh / PX / 3));
    int topBand = topRows * PX;
    int topRow = rowFromTop / PX;
    int maxInsetCells = max(1, min(3, slot.bw / PX / 3));
    int inset = q(max(0, topRows - topRow) * maxInsetCells * PX / max(1, topRows));
    int halfInset = q(inset / 2);

    switch ((slot.seed >> 28) & 0x7u) {
        case 1u:
            rowLeft += inset;
            rowRight -= inset;
            break;
        case 2u:
            rowLeft += inset;
            rowRight -= halfInset;
            break;
        case 3u:
            rowLeft += halfInset;
            rowRight -= inset;
            break;
        case 4u: {
            int stepInset = max(PX, inset - ((topRow >= topRows / 2) ? PX : 0));
            rowLeft += stepInset;
            rowRight -= stepInset;
            break;
        }
        case 5u:
            rowLeft += inset;
            break;
        case 6u:
            rowRight -= inset;
            break;
        case 7u:
            rowLeft += halfInset;
            rowRight -= max(PX, q((inset * 3) / 4));
            break;
        default:
            break;
    }

    int shoulderRows = min(2, max(1, slot.bh / PX / 5));
    if (((slot.seed >> 18) & 0x3u) == 0u &&
        rowFromTop >= topBand &&
        rowFromTop < topBand + shoulderRows * PX &&
        slot.bw >= PX * 5) {
        int stepInset = min(PX * 2, max(PX, slot.bw / 5));
        if (((slot.seed >> 17) & 0x1u) != 0u) {
            rowLeft += stepInset;
        } else {
            rowRight -= stepInset;
        }
    }

    rowLeft = q(rowLeft);
    rowRight = q(rowRight);
    // clamp to minimum 2-cell width — proportional squeeze, no full-width snap
    if (rowRight - rowLeft < PX * 2) {
        int mid = (slot.bx + slot.bx + slot.bw) / 2;
        rowLeft = q(mid - PX);
        rowRight = q(mid + PX);
    }
    roofCap = rowFromTop < PX * (2 + (int)((slot.seed >> 9) & 0x1u));
}

// ═══════════════════════════════════════════════════════════════════════════
// TOWER COLLECTION (internal — full parameter version)
// ═══════════════════════════════════════════════════════════════════════════

static int collectCityTowersInternal(CityTowerSlot* out, int maxSlots, float steer, float pitch, int horizonY) {
    struct CityLayerSpec {
        float depth;
        int slotW;
        int rowStep;
        int baseOffset;
        int rows;
        int minW;
        int maxW;
        int minH;
        int maxH;
        uint32_t mask;
    };

    // far-to-near: painter's algorithm (far rendered first, near overdraw)
    // 3 layers — BR2049 aerial: massive forms in thick smog.
    // mask: 0x1u = 50% spawn, 0x3u = 75%. lower = sparser.
    static const CityLayerSpec kLayerSpecs[] = {
        //  depth    slotW      rowStep    baseOff  rows minW maxW minH maxH  mask
        {0.88f, PX * 10, PX * 5, PX * 1,  2, 5, 10, 2,  5, 0x1u},  // far: faint haze, 50%
        {0.60f, PX * 8,  PX * 6, PX * 6,  2, 4,  8, 4, 10, 0x1u},  // mid: 50%, smaller
        {0.28f, PX * 7,  PX * 7, PX * 14, 2, 3,  7, 6, 13, 0x1u},  // near: 50%, capped height
    };

    // glass X envelope: conservative bounds (glass centered at 160, widest ~260px)
    constexpr int glassMinX = 16;
    constexpr int glassMaxX = 304;

    int count = 0;
    constexpr int kLayerCount = 3;
    for (int li = 0; li < kLayerCount; li++) {
        const CityLayerSpec& spec = kLayerSpecs[li];
        // reserve slots for nearer layers: each remaining layer gets ≥48 slots
        int layerCap = maxSlots - (kLayerCount - 1 - li) * 48;
        int baseCols = 320 / spec.slotW + 3;
        // extra cols must cover full parallax range — wider banking needs more columns
        int maxShift = abs(cityScrollX(1.0f, spec.depth));
        int extraCols = max(1, (maxShift + CITY_OVERSCAN_X) / max(PX, spec.slotW) + 1);
        int maxShiftY = abs((int)lroundf(cityScrollY(1.0f, spec.depth) * 0.35f)) + 24;
        int extraRows = max(1, (maxShiftY + CITY_OVERSCAN_Y) / max(PX, spec.rowStep) + 1);
        int worldX = cityScrollX(steer, spec.depth);
        int worldY = (int)lroundf((float)cityScrollY(pitch, spec.depth) * 0.35f);
        int baseY = q(horizonY + spec.baseOffset);
        for (int row = -1 - extraRows; row < spec.rows + 1 + extraRows; row++) {
            for (int col = -1 - extraCols; col < baseCols + extraCols; col++) {
                if (count >= layerCap) break;
                CityTowerSlot slot = cityTowerSlot(
                    col, row, worldX, worldY, spec.depth, 0x5A17u,
                    spec.slotW, spec.rowStep, baseY,
                    spec.minW, spec.maxW, spec.minH, spec.maxH
                );
                if ((slot.seed & spec.mask) == 0u) continue;
                // world-space clipping: cover full parallax range for viewport scrolling
                if (slot.bx + slot.bw < glassMinX - maxShift - CITY_OVERSCAN_X ||
                    slot.bx > glassMaxX + maxShift + CITY_OVERSCAN_X) continue;
                if (slot.by < glassOpenTop() - maxShiftY - CITY_OVERSCAN_Y ||
                    slot.by - slot.bh > WD_GLASS_B + maxShiftY + CITY_OVERSCAN_Y) continue;
                out[count++] = slot;
            }
            if (count >= layerCap) break;
        }
    }
    return count;
}

// ═══════════════════════════════════════════════════════════════════════════
// PUBLIC TOWER COLLECTION WRAPPER
// ═══════════════════════════════════════════════════════════════════════════

int collectCityTowers(float steer, float pitch) {
    int horizonY = q(glassOpenTop());
    return collectCityTowersInternal(sceneTowerSlots, CITY_TOWER_SLOT_CAP, steer, pitch, horizonY);
}

// ═══════════════════════════════════════════════════════════════════════════
// HORIZON SHADOW PASS (internal — full parameter version)
// ═══════════════════════════════════════════════════════════════════════════

static void drawHorizonShadowPassInternal(M5Canvas& canvas, const CityTowerSlot* towerSlots,
                                   const uint16_t* visIdx, int visCount,
                                   int horizonY, float steer, float pitch, bool flash) {
    if (flash || !towerSlots || !visIdx || visCount <= 0) return;

    float levelT = clamp01(1.0f - fabsf(pitch) * 0.88f);
    if (levelT <= 0.04f) return;

    int shadowSpan = q((int)(PX * (8.0f + levelT * 8.0f)));
    int shadowSkew = q((int)((0.42f + steer * 0.26f - pitch * 0.20f) * (float)shadowSpan));
    // ==[ O(n) ANCHOR SELECTION ]== single pass picks 5 best shadow anchors
    // by (depth ASC, by ASC, bw DESC), with spacing filter. no sort needed.
    CityTowerSlot anchors[5];
    int anchorCount = 0;

    auto slotBeforeAnchor = [](const CityTowerSlot& a, const CityTowerSlot& b) -> bool {
        if (a.depth < b.depth) return true;
        if (a.depth > b.depth) return false;
        if (a.by < b.by) return true;
        if (a.by > b.by) return false;
        return a.bw > b.bw;
    };

    for (int vi = 0; vi < visCount; vi++) {
        const CityTowerSlot& slot = towerSlots[visIdx[vi]];
        if (slot.by < horizonY - PX * 1 || slot.by > horizonY + PX * 12) continue;
        if (slot.bh < PX * 4) continue;
        if (slot.depth > 0.80f && (slot.seed & 0x1u)) continue;

        // spacing: reject if too close to any existing anchor
        int centerX = q(slot.bx + slot.bw / 2);
        bool tooClose = false;
        for (int j = 0; j < anchorCount; j++) {
            if (abs(centerX - q(anchors[j].bx + anchors[j].bw / 2)) < PX * 14) {
                tooClose = true;
                break;
            }
        }
        if (tooClose) continue;

        if (anchorCount < 5) {
            anchors[anchorCount++] = slot;
        } else {
            // replace worst anchor if this slot ranks better
            int worstIdx = 0;
            for (int j = 1; j < 5; j++) {
                if (slotBeforeAnchor(anchors[worstIdx], anchors[j])) worstIdx = j;
            }
            if (slotBeforeAnchor(slot, anchors[worstIdx])) {
                anchors[worstIdx] = slot;
            }
        }
    }

    for (int i = 0; i < anchorCount; i++) {
        const CityTowerSlot& slot = anchors[i];
        int sourceX = q(slot.bx + slot.bw / 2);
        int sourceY = max(horizonY - PX, min(WD_GLASS_B - PX * 6, slot.by - PX));
        int endY = min(WD_GLASS_B - PX * 2,
                       sourceY + shadowSpan + q((int)((1.0f - slot.depth) * PX * 10.0f)));
        if (endY <= sourceY + PX) continue;

        int skew = q((int)((float)shadowSkew * (0.82f + (0.66f - slot.depth) * 0.40f)));
        int startHalf = max(PX * 2, q((int)((float)slot.bw * (0.24f + (0.60f - slot.depth) * 0.14f))));
        float strength = 0.08f + levelT * 0.14f + max(0.0f, 0.68f - slot.depth) * 0.10f;

        for (int y = sourceY; y < endY; y += PX) {
            float t = (float)(y - sourceY) / (float)max(1, endY - sourceY);
            int cx = q(sourceX + (int)(skew * t));
            int halfW = max(startHalf, q((int)((float)startHalf * (0.82f + t * (1.20f + (1.0f - slot.depth) * 0.18f)))));
            float rowStrength = strength * (1.0f - t) * (1.0f - t * 0.28f);
            if (rowStrength <= 0.01f) continue;

            int left, right;
            glassBounds(y, left, right);
            int x0 = max(left, cx - halfW);
            int x1 = min(right, cx + halfW);
            if (x1 <= x0) continue;

            for (int x = x0; x < x1; x += PX) {
                float dist = (float)abs(x - cx) / (float)max(PX, halfW);
                if (dist >= 1.0f) continue;
                float edge = 1.0f - dist * dist;
                float noise = 0.84f + (float)((wallHash(x / PX, y / PX, slot.seed ^ 0x71D0u) >> 5) & 0x0Fu) / 15.0f * 0.24f;
                float shade = min(0.36f, rowStrength * edge * noise * 1.18f);
                if (shade > 0.02f) {
                    shadePx(canvas, x, y, min(0.42f, shade));
                    if (shade > 0.06f) blendPx(canvas, x, y, RP::SHADOW_C, min(0.18f, shade * 0.48f));
                }
            }
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// PUBLIC HORIZON SHADOW PASS WRAPPER
// ═══════════════════════════════════════════════════════════════════════════

void drawHorizonShadowPass(M5Canvas& c, uint32_t now, int horizonY) {
    (void)now;
    float steer = steerAngle + imuRoll * 0.50f;
    float pitch = imuPitch;
    bool flash = thunderFlashActive();
    drawHorizonShadowPassInternal(c, sceneTowerSlots, sceneVisibleIdx,
                                  cachedTowerCount, horizonY, steer, pitch, flash);
}

// ═══════════════════════════════════════════════════════════════════════════
// GLASS BACKDROP — THE BIG ONE (~900 lines)
// ═══════════════════════════════════════════════════════════════════════════

static void drawGlassBackdropPass(M5Canvas& canvas, uint32_t now,
                                  float motion, bool drawBase) {
    (void)motion;
    if (!sceneTowerSlots || !sceneVisibleIdx) return;  // PSRAM alloc failed

    float animT = (float)now * 0.001f;
    float steer;
    float pitch;
    int midLift;
    int skyLift;
    int horizonY;

    if (drawBase) {
        // Base and live overlays share this exact geometry sample until the
        // retained substrate refreshes. That prevents signs, windows, and
        // comms stems from sliding away from their tower silhouettes.
        updateCityPalette();
        sceneBackdropSteer = steerAngle + imuRoll * 0.50f;
        sceneBackdropPitch = imuPitch;
        sceneBackdropMidLift = parY(sceneBackdropPitch, 0.60f);
        sceneBackdropSkyLift = parY(sceneBackdropPitch, 0.82f);
        sceneBackdropHorizonY =
            q(glassOpenTop() - (int)(sceneBackdropPitch * 20.0f));
        sceneBackdropGeometryValid = true;
    } else if (!sceneBackdropGeometryValid) {
        return;
    }

    steer = sceneBackdropSteer;
    pitch = sceneBackdropPitch;
    midLift = sceneBackdropMidLift;
    skyLift = sceneBackdropSkyLift;
    horizonY = sceneBackdropHorizonY;

    // local aliases — modified in-place for thunder flash
    uint16_t cityNeon = cityPal.cityNeon;
    uint16_t towerCol = cityPal.towerCol;
    uint16_t fogGray = cityPal.fogGray;
    uint16_t fogCool = cityPal.fogCool;
    uint16_t fogWarm = cityPal.fogWarm;
    uint16_t haze = cityPal.haze;
    uint16_t skyZenith = cityPal.skyZenith;
    uint16_t skyTop = cityPal.skyTop;
    uint16_t skyMid = cityPal.skyMid;
    uint16_t deckHaze = cityPal.deckHaze;
    uint16_t roadGlow = cityPal.roadGlow;
    uint16_t roofSilhouetteCol = cityPal.roofSilhouetteCol;
    uint16_t layerTowerCol[3];
    memcpy(layerTowerCol, cityPal.layerTowerCol, sizeof(layerTowerCol));

    bool flash = thunderFlashActive();
    if (flash) {
        uint16_t hudPeak = Display::isInvertedTheme() ? Display::getColorBG() : Display::getColorFG();
        uint16_t flashCol = Display::lerpColor565(RP::FLUOR, hudPeak, 0.70f);
        // sky washes out toward peak brightness
        skyZenith = Display::lerpColor565(skyZenith, flashCol, 0.50f);
        skyTop = Display::lerpColor565(skyTop, flashCol, 0.65f);
        skyMid = Display::lerpColor565(skyMid, flashCol, 0.62f);
        deckHaze = Display::lerpColor565(deckHaze, flashCol, 0.50f);
        fogGray = Display::lerpColor565(fogGray, flashCol, 0.55f);
        fogCool = Display::lerpColor565(fogCool, flashCol, 0.48f);
        fogWarm = Display::lerpColor565(fogWarm, flashCol, 0.48f);
        haze = Display::lerpColor565(haze, flashCol, 0.50f);
        // towers lit from behind — all layers brighten
        towerCol = Display::lerpColor565(towerCol, flashCol, 0.35f);
        for (int li = 0; li < 3; li++)
            layerTowerCol[li] = Display::lerpColor565(layerTowerCol[li], flashCol, 0.50f + (float)li * 0.08f);
        roofSilhouetteCol = Display::lerpColor565(roofSilhouetteCol, flashCol, 0.50f);
        roadGlow = Display::lerpColor565(roadGlow, flashCol, 0.40f);
    }

    if (drawBase) {
    // precompute sky constants outside row loop
    uint16_t deckBase = Display::lerpColor565(skyMid, fogGray, 0.34f);
    uint16_t horizonPeak = Display::lerpColor565(skyMid, Display::lerpColor565(fogWarm, RP::FLUOR, 0.20f), 0.45f);
    float invSkySpan = 1.0f / (float)max(1, horizonY - WD_GLASS_T);
    float invDeckSpan = 1.0f / (float)max(1, WD_GLASS_B - horizonY);
    float invGlassH = 1.0f / (float)max(1, WD_GLASS_B - WD_GLASS_T);
    int glassOpenTopY = glassOpenTop();

    for (int y = WD_GLASS_T; y < WD_GLASS_B; y += PX) {
        int left, right;
        glassBounds(y, left, right);

        float worldT = clamp01((float)(y - skyLift - WD_GLASS_T) * invGlassH);
        float skyT = clamp01((float)(y - WD_GLASS_T) * invSkySpan);
        float deckT = clamp01((float)(y - horizonY) * invDeckSpan);

        // 3-zone sky: zenith (dark) → mid (ramp) → horizon (pollution bloom)
        uint16_t rowCol;
        if (y >= horizonY) {
            rowCol = Display::lerpColor565(deckBase, deckHaze, deckT);
        } else if (skyT < 0.25f) {
            // zenith — murky but not black
            float zenithT = skyT / 0.25f;
            rowCol = Display::lerpColor565(skyZenith, skyTop, zenithT);
        } else if (skyT < 0.55f) {
            // mid-sky — ramp into pollution
            float midT = (skyT - 0.25f) / 0.30f;
            rowCol = Display::lerpColor565(skyTop, skyMid, midT);
        } else {
            // horizon — warm pollution bloom
            float horizT = (skyT - 0.55f) / 0.45f;
            rowCol = Display::lerpColor565(skyMid, horizonPeak, horizT);
        }

        float lowBank = max(0.0f, 1.0f - fabsf(worldT - 0.66f) * 2.8f);
        float horizonBand = max(0.0f, 1.0f - (float)abs(y - horizonY) / (float)(PX * 10));  // wider glow band
        float horizonShelf = max(0.0f, 1.0f - (float)abs(y - (horizonY + PX * 4)) / (float)(PX * 9));
        float wave = 0.5f + 0.5f * fastSinf(animT * 0.18f + worldT * 11.5f + pitch * 2.4f);

        rowCol = Display::lerpColor565(rowCol, fogGray, lowBank * (0.22f + wave * 0.10f));
        rowCol = Display::lerpColor565(rowCol, RP::DEEP, deckT * 0.28f + horizonShelf * 0.10f);
        // horizon band: mix of gray + subtle warm tint
        uint16_t horizBlend = Display::lerpColor565(fogGray, fogWarm, 0.35f);
        rowCol = Display::lerpColor565(rowCol, horizBlend, horizonBand * 0.50f);

        // ==[ CRT SCANLINE ]== baked into fill — odd rows darken 7%, no read-modify-write
        if (SHOW_SCANLINES && y >= glassOpenTopY + PX * 2 && ((y / PX) & 1) != 0) {
            rowCol = Display::lerpColor565(rowCol, RP::BG, 0.07f);
        }

        canvas.fillRect(left, y, right - left, PX, rowCol);
        int phase = (int)((wallHash(0, y, 0xA7F1u) & 3u)) * PX;
        for (int x = left + phase; x < right; x += PX * 4) {
            canvas.fillRect(x, y, PX, PX, bumpColor(rowCol, x, y, 0xA7F1u, fogGray, RP::DEEP, 0.10f));
        }
    }

    // ==[ HORIZON GLOW + TRACE ]== merged single pass — one RMW per pixel
    int horizonGlowY0 = max((int)WD_GLASS_T, horizonY - PX * 2);
    int horizonGlowY1 = min((int)WD_GLASS_B, horizonY + PX * 5);
    // precompute above/below horizon color variants
    uint16_t glowBandAbove = Display::lerpColor565(fogGray, haze, 0.24f);
    uint16_t glowBandBelow = Display::lerpColor565(fogWarm, roadGlow, 0.38f);
    uint16_t traceAbove = Display::lerpColor565(fogGray, fogCool, 0.14f);
    uint16_t traceBelow = Display::lerpColor565(fogWarm, roadGlow, 0.36f);
    for (int y = horizonGlowY0; y < horizonGlowY1; y += PX) {
        int left, right;
        glassBounds(y, left, right);
        float bandT = max(0.0f, 1.0f - (float)abs(y - horizonY) / (float)(PX * 7));
        if (bandT <= 0.0f) continue;
        uint16_t bandCol = (y <= horizonY) ? glowBandAbove : glowBandBelow;
        float bandAlpha = 0.14f + bandT * ((y <= horizonY) ? 0.24f : 0.22f);

        // trace overlay — narrow band around horizon line
        bool hasTrace = (y >= horizonY - (int)PX && y < horizonY + PX * 2);
        uint16_t traceCol = 0;
        uint8_t traceAlpha8 = 0;
        if (hasTrace) {
            traceCol = (y <= horizonY) ? traceAbove : traceBelow;
            traceAlpha8 = (y == horizonY) ? 82 : 46;  // 0.32*256=82, 0.18*256=46
        }
        // pre-convert band alpha once per row
        uint8_t bandAlpha8 = (uint8_t)clampi((int)(bandAlpha * 256.0f), 0, 255);

        for (int x = left + PX; x < right - PX; x += PX) {
            int px = q(x);
            if (px < 0 || px >= 316 || y < WD_PLAY_T || y >= WD_PLAY_B) continue;
            uint16_t base = fastReadPx(canvas, px, y);
            uint16_t c = lerpColor565_8(base, bandCol, bandAlpha8);
            if (hasTrace) c = lerpColor565_8(c, traceCol, traceAlpha8);
            canvas.fillRect(px, y, PX, PX, c);
        }
    }
    }

    // ==[ PSRAM TOWER CACHE ]== world-space grid, generated once at steer=0/pitch=0
    int towerCount = cachedTowerCount;
    if (drawBase) {
        if (!cityGridValid) {
            refHorizonY = q(glassOpenTop());
            cachedTowerCount = collectCityTowersInternal(
                sceneTowerSlots, CITY_TOWER_SLOT_CAP,
                0.0f, 0.0f, refHorizonY);
            cityGridValid = true;
            hasCommsTower = false;
        }
        towerCount = cachedTowerCount;

        int horizonShift = horizonY - refHorizonY;
        for (int i = 0; i < towerCount; i++) {
            CityTowerSlot& s = sceneTowerSlots[i];
            s.bx = s.refBx - cityScrollX(steer, s.depth);
            int worldY =
                (int)lroundf((float)cityScrollY(pitch, s.depth) * 0.35f);
            s.by = s.refBy - worldY + horizonShift;
        }
        sceneVisibleTowerCount = 0;
        for (int i = 0; i < towerCount; i++) {
            const CityTowerSlot& slot = sceneTowerSlots[i];
            if (slot.bx + slot.bw <= 0 || slot.bx >= 320) continue;
            if (slot.bh < PX * 3) continue;
            if (slot.by <= WD_GLASS_T + PX * 2 ||
                slot.by - slot.bh >= WD_GLASS_B - PX * 2) continue;
            if (sceneVisibleTowerCount >= CITY_TOWER_SLOT_CAP) break;
            sceneVisibleIdx[sceneVisibleTowerCount++] = (uint16_t)i;
        }
    }
    const int visibleTowerCount = sceneVisibleTowerCount;

    if (drawBase) {
    // ==[ PASS 1: TOWER BODIES ]== per-layer atmospheric fill + sparse bump
    for (int vi = 0; vi < visibleTowerCount; vi++) {
        const CityTowerSlot& slot = sceneTowerSlots[sceneVisibleIdx[vi]];
        // depth-bucket into layer index: 0.88→0, 0.60→1, 0.28→2
        int layerIdx = (slot.depth > 0.74f) ? 0
                     : (slot.depth > 0.44f) ? 1 : 2;
        uint16_t towerFill = layerTowerCol[layerIdx];
        // near towers: higher bump contrast. far towers: softer bumps.
        float bumpScale = (layerIdx >= 2) ? 0.20f : (layerIdx >= 1) ? 0.12f : 0.06f;
        uint16_t bumpHi = Display::lerpColor565(towerFill, haze, bumpScale * 1.8f);
        uint16_t bumpLo = Display::lerpColor565(towerFill, RP::DEEP, bumpScale * 0.7f);
        uint16_t bumpHL = Display::lerpColor565(towerFill, haze, bumpScale * 0.9f);
        // precompute roof-cap variants once per tower (not per row)
        uint16_t capFill = Display::lerpColor565(towerFill, roofSilhouetteCol, 0.48f);
        uint16_t capBumpHi = Display::lerpColor565(capFill, haze, 0.16f * 1.8f);
        uint16_t capBumpLo = Display::lerpColor565(capFill, RP::DEEP, 0.16f * 0.7f);
        uint16_t capBumpHL = Display::lerpColor565(capFill, haze, 0.16f * 0.9f);
        uint32_t bumpSeed = 0xC903u ^ slot.seed;

        // clamp Y iteration to glass range — skip invisible rows entirely
        int towerYStart = max(slot.by - slot.bh, (int)WD_GLASS_T);
        // align to CPX grid
        towerYStart = towerYStart - ((towerYStart - (slot.by - slot.bh)) % CPX);
        if (towerYStart < WD_GLASS_T) towerYStart += CPX;
        int towerYEnd = min(slot.by, (int)WD_GLASS_B);
        for (int y = towerYStart; y < towerYEnd; y += CPX) {
            int left, right;
            glassBounds(y, left, right);
            int rowLeft, rowRight;
            bool roofCap = false;
            cityTowerRowBounds(slot, y, rowLeft, rowRight, roofCap);
            int x0 = max(rowLeft, left);
            int x1 = min(rowRight, right);
            if (x1 <= x0) continue;
            uint16_t rowFill = roofCap ? capFill : towerFill;

            // single wide fill for the whole row
            canvas.fillRect(x0, y, x1 - x0, CPX, rowFill);

            // sparse bump overlay — coarser city detail
            uint16_t rowBumpHi = roofCap ? capBumpHi : bumpHi;
            uint16_t rowBumpLo = roofCap ? capBumpLo : bumpLo;
            uint16_t rowBumpHL = roofCap ? capBumpHL : bumpHL;
            for (int x = x0; x < x1; x += CPX) {
                uint32_t h = wallHash(x, y, bumpSeed);
                uint8_t n = h & 0xFFu;
                if (n < 10u) canvas.fillRect(x, y, CPX, CPX, rowBumpHi);
                else if (n < 50u) canvas.fillRect(x, y, CPX, CPX, rowBumpLo);
                else if (n > 205u) canvas.fillRect(x, y, CPX, CPX, rowBumpHL);
                // 77% of pixels: no write needed, row fill already correct
            }
        }
    }

    // ==[ FOG + HAZE ]== merged single pass — one read-modify-write per pixel max
    int hazeCenter = q(horizonY + PX * 1);
    int hazeY0 = max((int)WD_GLASS_T, horizonY - PX * 3);
    int hazeY1 = min((int)WD_GLASS_B, horizonY + PX * 8);
    // precompute haze band colors — only 2 variants (above/below horizon)
    uint16_t hazeBandColAbove = Display::lerpColor565(fogGray, fogCool, 0.24f);
    uint16_t hazeBandColBelow = Display::lerpColor565(fogGray, fogCool, 0.10f);
    // ==[ FOG DRIFT ]== the stipple field is sampled through an advancing
    // column offset, so the whole bank translates instead of re-randomizing in
    // place, and a long triangular swell over 64 cells (256px, wider than the
    // pane) groups it into one soft bank rather than uniform noise. Both ride
    // the far-layer parallax so the fog tracks the world when you steer.
    // This lives in the retained base pass: the drift is 4px-quantized and
    // steps at ~30Hz, which is invisible at 2 cells/sec.
    const int fogDriftCells = (int)(animT * 2.2f) +
                              cityScrollX(steer, 0.86f) / (PX * 3);
    static constexpr int FOG_BANK_AMP = 46;   // coverage swing across a bank
    for (int y = WD_GLASS_T + PX; y < WD_GLASS_B - PX; y += PX) {
        int left, right;
        glassBounds(y, left, right);

        // aerial fog: thick near horizon (far), clear at bottom (near)
        float fogT = clamp01((float)(y - midLift - WD_GLASS_T) / (float)max(1, WD_GLASS_B - WD_GLASS_T));
        float horizonFog = max(0.0f, 1.0f - fogT * 1.25f);   // slightly further down than original 1.4
        float rowAlpha = horizonFog * 0.22f;                // 1.6x original — thicker but not opaque

        // haze contribution
        float hazeBandT = 0.0f;
        uint16_t hazeBandCol = 0;
        float hazeBandAlpha = 0.0f;
        if (y >= hazeY0 && y < hazeY1) {
            hazeBandT = max(0.0f, 1.0f - (float)abs(y - hazeCenter) / (float)(PX * 12));
            if (hazeBandT > 0.0f) {
                hazeBandCol = (y < horizonY) ? hazeBandColAbove : hazeBandColBelow;
                hazeBandAlpha = 0.06f + hazeBandT * 0.16f;
            }
        }

        bool hasFog = (rowAlpha > 0.01f);
        bool hasHaze = (hazeBandT > 0.0f);
        if (!hasFog && !hasHaze) continue;

        float fogPhase = 0.0f;
        int coverage = 0;
        // precompute fog alpha base in Q8 once per row — per-pixel hash modulates
        int fogAlphaBase8 = 0;
        int fogAlphaStep8 = 0;  // per hash unit increment
        uint16_t fogColA = fogGray;  // fog color variant A (hash bit selects)
        uint16_t fogColB = fogGray;  // fog color variant B
        if (hasFog) {
            fogPhase = 0.5f + 0.5f * fastSinf(animT * 0.12f + (float)y * 0.11f + pitch * 2.2f);
            coverage = clampi((int)(90.0f + horizonFog * 120.0f + fogPhase * 15.0f), 0, 255);
            // alpha range: rowAlpha * [0.60, 1.0]
            fogAlphaBase8 = clampi((int)(rowAlpha * 0.60f * 256.0f), 0, 100);
            fogAlphaStep8 = clampi((int)(rowAlpha * 0.38f / 15.0f * 256.0f), 0, 10);
            fogColB = (fogT < 0.45f) ? fogCool : fogWarm;
        }
        // pre-convert haze alpha to Q8 once per row
        uint8_t hazeAlpha8 = hasHaze ? (uint8_t)clampi((int)(hazeBandAlpha * 256.0f), 0, 255) : 0;

        for (int x = left + PX; x < right - PX; x += PX) {
            int px = q(x);
            if (px < 0 || px >= 316) continue;

            // determine if this pixel gets fog
            bool fogHit = false;
            uint16_t fogCol = fogGray;
            uint8_t fogAlpha8 = 0;
            if (hasFog) {
                int fogCol8 = px / PX + fogDriftCells;
                // Triangular swell, 0..31 over a 64-cell period.
                int bankPhase = fogCol8 & 63;
                int bankTri = (bankPhase < 32) ? bankPhase : 63 - bankPhase;
                int cov = coverage - FOG_BANK_AMP / 2 +
                          ((bankTri * FOG_BANK_AMP) >> 5);
                uint32_t h = wallHash(fogCol8, (y - midLift) / PX, 0xF04Du);
                if ((int)(h & 0xFFu) < cov) {
                    fogCol = ((h >> 11) & 1u) ? fogColA : fogColB;
                    // integer-only alpha: base + hash * step
                    fogAlpha8 = (uint8_t)clampi(fogAlphaBase8 + (int)((h >> 14) & 0x0Fu) * fogAlphaStep8, 0, 140);
                    fogHit = true;
                }
            }

            if (!fogHit && !hasHaze) continue;

            // single read-modify-write — integer-only blends
            uint16_t base = fastReadPx(canvas, px, y);
            uint16_t c = base;
            if (fogHit) c = lerpColor565_8(c, fogCol, fogAlpha8);
            if (hasHaze) c = lerpColor565_8(c, hazeBandCol, hazeAlpha8);
            canvas.fillRect(px, y, PX, PX, c);
        }
    }

    drawHorizonShadowPassInternal(canvas, sceneTowerSlots, sceneVisibleIdx, visibleTowerCount, horizonY, steer, pitch, flash);

    for (int vi = 0; vi < visibleTowerCount; vi++) {
        const CityTowerSlot& slot = sceneTowerSlots[sceneVisibleIdx[vi]];
        int topY = slot.by - slot.bh;
        if (topY >= WD_GLASS_B) continue;  // entirely below glass
        int capRows = 2 + (int)((slot.seed >> 9) & 0x1u);
        int capYStart = max(topY, (int)WD_GLASS_T);
        int capYEnd = min(min(slot.by, topY + capRows * CPX), (int)WD_GLASS_B);
        if (capYEnd <= capYStart) continue;
        // hoist capCol outside row loop — depends only on slot.depth
        uint16_t capCol = Display::lerpColor565(roofSilhouetteCol, fogGray, (slot.depth > 0.74f) ? 0.10f : 0.04f);
        uint32_t capBumpSeed = 0x6D17u ^ slot.seed;
        for (int y = capYStart; y < capYEnd; y += CPX) {
            int left, right;
            glassBounds(y, left, right);
            int rowLeft, rowRight;
            bool roofCap = false;
            cityTowerRowBounds(slot, y, rowLeft, rowRight, roofCap);
            if (!roofCap) break;
            int x0 = max(rowLeft, left);
            int x1 = min(rowRight, right);
            if (x1 <= x0) continue;
            for (int x = x0; x < x1; x += CPX) {
                canvas.fillRect(x, y, CPX, CPX, bumpColor(capCol, x, y, capBumpSeed, fogGray, RP::DEEP, 0.08f));
            }
        }
    }
    }

    if (!drawBase) {
    for (int vi = 0; vi < visibleTowerCount; vi++) {
        const CityTowerSlot& slot = sceneTowerSlots[sceneVisibleIdx[vi]];
        if (slot.bh <= PX * 10 || flash) continue;
        int spireX = q(slot.bx + slot.bw / 2);
        int spireTop = slot.by - slot.bh - CPX;
        for (int sy = spireTop; sy < slot.by - slot.bh; sy += CPX) {
            if (sy < WD_GLASS_T || sy >= WD_GLASS_B || !insideGlass(spireX, sy)) continue;
            glowPx(canvas, spireX, sy, towerCol, 96);
        }
        if (spireTop >= WD_GLASS_T && insideGlass(spireX, spireTop)) {
            float beaconRate = 1.4f + (float)((slot.seed >> 4) & 0x3u) * 0.6f;
            uint32_t blink = ((uint32_t)(animT * beaconRate) + (slot.seed & 0x7u)) & 3u;
            if (blink != 0u) glowPx(canvas, spireX, spireTop, cityNeon, (uint8_t)clampi((int)(150.0f + (1.0f - slot.depth) * 80.0f), 0, 255));
        }
    }

    // ==[ STEAM VENTS ]== periodic white plumes from building tops — BR2049 atmosphere
    if (!flash) {
        uint16_t steamCol = Display::lerpColor565(fogGray, RP::FLUOR, 0.15f);
        uint16_t steamDim = Display::lerpColor565(fogGray, haze, 0.30f);
        for (int vi = 0; vi < visibleTowerCount; vi++) {
            const CityTowerSlot& slot = sceneTowerSlots[sceneVisibleIdx[vi]];
            if (slot.depth > 0.70f) continue;
            if (slot.bh < PX * 8) continue;
            uint32_t vs = wallHash(slot.seed, 0, 0x57E4u);
            if ((vs & 0x7u) >= 2u) continue;  // ~25% of qualifying towers

            int ventX = q(slot.bx + (int)((vs >> 8) % max(1u, (uint32_t)(slot.bw / PX))) * PX);
            int ventBaseY = slot.by - slot.bh;

            // duty cycle: burst ~2s, off ~4s, staggered per tower
            float ventCycle = animT * 0.5f + (float)(vs & 0xFFu) * 0.04f;
            float duty = ventCycle - floorf(ventCycle);
            if (duty >= 0.35f) continue;  // 35% on, 65% off

            float burstT = duty / 0.35f;
            int plumeLen = 4 + (int)((vs >> 14) & 0x3u);
            float riseOffset = burstT * (float)(CPX * 2);
            int driftX = (int)(steer * (1.0f - slot.depth) * 8.0f);

            for (int p = 0; p < plumeLen; p++) {
                int py = q(ventBaseY - p * CPX - (int)riseOffset);
                int px = q(ventX + driftX * p / max(1, plumeLen));
                if (py < WD_GLASS_T || py >= WD_GLASS_B) continue;
                if (!insideGlass(px, py)) continue;
                float pT = (float)p / (float)max(1, plumeLen - 1);
                float fadeUp = 1.0f - pT * pT;
                float fadeTime = (burstT < 0.7f) ? 1.0f : (1.0f - (burstT - 0.7f) / 0.3f);
                uint8_t alpha = (uint8_t)clampi((int)(fadeUp * fadeTime * 120.0f), 0, 180);
                if (alpha < 15) continue;
                uint16_t col = (p < 2) ? steamCol : steamDim;
                glowPx(canvas, px, py, col, alpha);
                if (p < 2 && px + PX < 320 && insideGlass(px + PX, py))
                    glowPx(canvas, px + PX, py, col, (uint8_t)(alpha >> 1));
            }
        }
    }
    }

    if (drawBase) {
    // ==[ PASS 1b: BUILDING EDGES ]== subtle left-edge silhouette, near towers only
    for (int vi = 0; vi < visibleTowerCount; vi++) {
        const CityTowerSlot& slot = sceneTowerSlots[sceneVisibleIdx[vi]];
        if (slot.depth > 0.50f) continue;  // near layers only
        uint8_t edgeStr = (slot.depth < 0.38f) ? 40 : 25;
        int edgeYStart = max(slot.by - slot.bh, (int)WD_GLASS_T);
        int edgeYEnd = min(slot.by, (int)WD_GLASS_B);
        for (int y = edgeYStart; y < edgeYEnd; y += CPX * 2) {  // every other row
            int left, right;
            glassBounds(y, left, right);
            int rowLeft, rowRight;
            bool roofCap = false;
            cityTowerRowBounds(slot, y, rowLeft, rowRight, roofCap);
            int x0 = max(rowLeft, left);
            int x1 = min(rowRight, right);
            if (x1 - x0 < PX * 3) continue;
            blendPx8(canvas, x0, y, fogGray, edgeStr);
        }
    }
    }

    // ==[ PASS 2: WINDOW GRID ]== columnar coherence — vertical lines of light
    // Stable office bands belong to the retained base. Only the sparse
    // flickering bands return in the live pass.
    const uint16_t (&slitPalette)[5] = cityPal.slitPalette;
    if (!flash) {
        for (int vi = 0; vi < visibleTowerCount; vi++) {
            const CityTowerSlot& slot = sceneTowerSlots[sceneVisibleIdx[vi]];
            // 50% dark towers — BR2049 restraint
            if (((slot.seed >> 23) & 0x1u) == 0u) continue;

            int layerIdx = (slot.depth > 0.74f) ? 0
                         : (slot.depth > 0.44f) ? 1 : 2;

            // column count: far=0 (skip), mid=1, near=sparse
            if (layerIdx == 0) continue;  // no windows on far haze
            int maxCols = (layerIdx == 1) ? 1 : max(1, slot.bw / (PX * 4));
            int numCols = max(1, min(4, maxCols));
            // window strength: dimmed for atmosphere
            uint8_t baseStr = (layerIdx == 1) ? 70 : 120;
            // lit probability: mid=50%, near=70%
            uint8_t litThreshold = (layerIdx == 1) ? 128 : 77;

            // compute stable column X positions once per tower
            int colX[6];
            for (int c = 0; c < numCols; c++) {
                colX[c] = q(slot.bx + ((c + 1) * slot.bw) / (numCols + 1));
            }

            // divide tower into horizontal bands — each gets 1-2 colors
            // band height: 3-4 CPX rows per band
            int bandH = CPX * (3 + (int)((slot.seed >> 5) & 0x1u));

            int winYStart = max(slot.by - slot.bh, (int)WD_GLASS_T);
            int winYEnd = min(slot.by, (int)WD_GLASS_B);
            for (int y = winYStart; y < winYEnd; y += CPX) {
                int left, right;
                glassBounds(y, left, right);
                int rowLeft, rowRight;
                bool roofCap = false;
                cityTowerRowBounds(slot, y, rowLeft, rowRight, roofCap);
                if (roofCap) continue;
                int x0 = max(rowLeft, left);
                int x1 = min(rowRight, right);
                if (x1 - x0 < PX * 2) continue;

                // band index for color coherence — same color within a horizontal band
                int bandIdx = (y - (slot.by - slot.bh)) / max(1, bandH);
                uint32_t bandSeed = wallHash(bandIdx, 0, slot.seed ^ 0x7E11u);

                // dark floor check — 55% of bands are unlit
                if ((bandSeed & 0xFFu) < 140u) continue;

                // slow-flicker: 6% normal, ~25% during thunder burst decay
                uint8_t flickThresh = (wdThunderFlickerBoost > 0.1f) ? 3u : 0u;
                bool isFlickerer = ((slot.seed >> 12) & 0xFu) <= flickThresh;
                if (drawBase == isFlickerer) continue;
                if (isFlickerer) {
                    uint32_t blink = ((uint32_t)(animT * 0.4f) + (slot.seed & 0xFu) + (uint32_t)(y / PX)) & 7u;
                    if (blink == 0u) continue;
                }

                // band color: 1-2 colors per band for "different offices" look
                uint16_t bandCol = slitPalette[((bandSeed >> 8) + (slot.seed >> 18)) % 5u];

                for (int c = 0; c < numCols; c++) {
                    int wx = colX[c];
                    if (wx < x0 || wx >= x1 - PX) continue;
                    // per-column lit check (seeded, not random per frame)
                    uint32_t colSeed = wallHash(c, bandIdx, slot.seed ^ 0xA3B1u);
                    if ((uint8_t)(colSeed & 0xFFu) < litThreshold) continue;
                    uint8_t str = (uint8_t)max(0, (int)baseStr - c * 8);
                    glowPx(canvas, wx, y, bandCol, str);
                }
            }
        }
    }

    if (!drawBase) {
    // latch tower anchor on first visible frame — prevents bubble jumping during steer
    bool canShowTower = (towerPhase == TowerPhase::TALKING &&
                         now - towerPhaseStart >= 1200UL &&
                         towerBubbleText[0] != '\0');
    if (canShowTower && !hasCommsTower && visibleTowerCount > 0) {
        savedCommsIdx = sceneVisibleIdx[towerSpeakerIdx % visibleTowerCount];
        hasCommsTower = true;
    }
    if (canShowTower && hasCommsTower && savedCommsIdx < (uint16_t)towerCount) {
        // read tower's current screen position — already scrolled per-frame
        const CityTowerSlot& commsTower = sceneTowerSlots[savedCommsIdx];
        int anchorX = q(commsTower.bx + commsTower.bw / 2);
        int anchorY = q(commsTower.by - commsTower.bh + PX);

        char lines[4][SCENE_TEXT_MAX] = {};
        int numLines = wrapSceneText(towerBubbleText, lines, 4, (108 - 16) / SCENE_CHAR_W);
        int maxChars = 0;
        for (int i = 0; i < numLines; i++) maxChars = max(maxChars, (int)strlen(lines[i]));

        int bubbleW = q(clampi(maxChars * SCENE_CHAR_W + 16, 52, 108));
        int bubbleH = q(clampi(12 + numLines * SCENE_LINE_H, 28, 60));
        int bubbleY = max(WD_GLASS_T + PX, q(anchorY - bubbleH - 16));
        int left, right;
        glassBounds(clampi(bubbleY + bubbleH / 2, WD_GLASS_T, WD_GLASS_B - PX), left, right);
        int bubbleX = q(max(left + PX, min(right - bubbleW - PX, anchorX - bubbleW / 2)));

        uint16_t hp = Display::isInvertedTheme() ? Display::getColorBG() : Display::getColorFG();
        uint16_t border = Display::lerpColor565(WD_NEON, hp, 0.24f);
        uint16_t fill = Display::lerpColor565(RP::DEEP, RP::BG, 0.18f);
        uint16_t textCol = Display::lerpColor565(border, hp, 0.16f);
        uint16_t hdr = Display::lerpColor565(WD_NEON, hp, 0.22f);

        canvas.fillRect(bubbleX, bubbleY, bubbleW, bubbleH, border);
        canvas.fillRect(bubbleX + PX, bubbleY + PX, bubbleW - PX * 2, bubbleH - PX * 2, fill);
        canvas.fillRect(bubbleX + PX, bubbleY + PX, bubbleW - PX * 2, PX, hdr);

        int stemX = q(clampi(anchorX, bubbleX + PX * 3, bubbleX + bubbleW - PX * 3));
        for (int step = 0; step < 4; step++) {
            int stepX = stemX + ((anchorX >= stemX) ? step * PX : -step * PX);
            int stepY = bubbleY + bubbleH + step * PX;
            if (stepY < anchorY + PX) canvas.fillRect(stepX, stepY, PX, PX, border);
        }

        canvas.setTextSize(1);
        canvas.setTextColor(textCol);
        for (int i = 0; i < numLines; i++) {
            canvas.setCursor(q(bubbleX + 8), q(bubbleY + 8) + i * SCENE_LINE_H);
            canvas.print(lines[i]);
        }

        for (int i = 0; i < 4; i++) {
            if (((int)(animT * 5.0f) + i) & 1) glowPx(canvas, q(bubbleX + bubbleW - PX * 2 - i * PX * 2), q(bubbleY + PX), WD_NEON, 100);
        }
    }

    // ==[ NEON SIGNS ]== anchored to tower surfaces — move with buildings
    const uint16_t (&signColors)[4] = cityPal.signColors;
    for (int vi = 0; vi < visibleTowerCount; vi++) {
        const CityTowerSlot& slot = sceneTowerSlots[sceneVisibleIdx[vi]];
        if (slot.depth > 0.50f) continue;  // signs only on near towers
        if (slot.bh < PX * 10) continue;   // needs a real building face
        uint32_t ss = wallHash(slot.seed, 0, 0xFE01u);
        if ((ss & 0x7u) != 0u) continue;   // ~12% of towers get signs — rare punctuation
        int sw = q(PX * 3 + (int)((ss >> 14) % 4u) * PX);  // 12-24px wide
        int sh = q(PX * 2 + (int)((ss >> 22) & 0x1u) * PX); // 8-12px tall
        // Y: avoid roof cap and base
        int availH = slot.bh - sh - PX * 5;
        if (availH < PX) continue;
        int sy = q(slot.by - slot.bh + PX * 3 + (int)((ss >> 8) % max(1u, (uint32_t)(availH / PX))) * PX);
        if (sy < WD_GLASS_T || sy + sh >= WD_GLASS_B) continue;
        // X: within tower row bounds at sign Y
        int rowLeft, rowRight;
        bool roofCap;
        cityTowerRowBounds(slot, sy, rowLeft, rowRight, roofCap);
        if (roofCap) continue;
        int towerW = rowRight - rowLeft;
        if (sw > towerW - PX * 2) continue;
        int signRange = (towerW - sw - PX) / PX;
        if (signRange < 1) signRange = 1;
        int sx = q(rowLeft + PX + (int)((ss >> 4) % (uint32_t)signRange) * PX);
        int left, right;
        glassBounds(sy, left, right);
        sx = max(left, min(right - sw, sx));
        uint16_t sc = signColors[(ss >> 20) % 4u];
        // duty variety: solid (35%), normal (30%), dying (25%), strobe (10%)
        uint8_t signType = (uint8_t)((ss >> 24) % 20u);
        bool signVisible;
        if (signType < 7u) {
            signVisible = true;  // solid
        } else if (signType < 13u) {
            uint32_t blink = ((uint32_t)(animT * 2.5f) + (ss & 0x7u)) & 3u;
            signVisible = (blink != 0u);  // 75% on
        } else if (signType < 18u) {
            uint32_t blink = ((uint32_t)(animT * 3.0f) + (ss & 0xFu)) & 7u;
            signVisible = (blink < 2u);  // 25% on, dying
        } else {
            uint32_t strobePhase = ((uint32_t)(animT * 2.0f) + (ss & 0x7u)) & 1u;
            sc = signColors[((ss >> 20) + strobePhase) % 4u];
            signVisible = true;
        }
        if (signVisible) {
            // dark outline behind sign (framing)
            for (int oy = -PX; oy < sh + PX; oy += PX) {
                int ry = sy + oy;
                if (ry < WD_GLASS_T || ry >= WD_GLASS_B) continue;
                int gl, gr;
                glassBounds(ry, gl, gr);
                int ox0 = max(gl, sx - PX);
                int ox1 = min(gr, sx + sw + PX);
                if (ox1 > ox0) canvas.fillRect(ox0, ry, ox1 - ox0, PX, RP::DEEP);
            }
            // top row: full width, bright
            uint8_t topStr = (uint8_t)clampi(110 + (int)((ss >> 20) & 0x1Fu), 0, 255);
            for (int s = 0; s < sw; s += PX) {
                int px = sx + s;
                if (px >= left && px < right) glowPx(canvas, px, sy, sc, topStr);
            }
            // bottom rows: inset 1 PX each side, dimmer (letterbox)
            for (int dy = PX; dy < sh; dy += PX) {
                int ry = sy + dy;
                if (ry >= WD_GLASS_B) break;
                int rl, rr;
                glassBounds(ry, rl, rr);
                uint8_t rowStr = (uint8_t)max(40, (int)topStr - dy * 12);
                for (int s = PX; s < sw - PX; s += PX) {
                    int px = sx + s;
                    if (px >= rl && px < rr) glowPx(canvas, px, ry, sc, rowStr);
                }
            }
            // spill glow below sign
            int spillY = sy + sh;
            if (spillY >= WD_GLASS_T && spillY < WD_GLASS_B) {
                int sl, sr;
                glassBounds(spillY, sl, sr);
                for (int s = PX; s < sw - PX; s += PX * 2) {
                    int px = sx + s;
                    if (px >= sl && px < sr) glowPx(canvas, px, spillY, sc, 30);
                }
            }
        }
    }

    // ==[ SEARCHLIGHT ]== single distant beam sweeping through smog
    if (!flash) {
        uint16_t beamCol = Display::screenBlend565(cityPal.cityAmber, RP::FLUOR, 40);  // warm amber beam
        float beamAngle = animT * 0.15f;
        float beamCos = fastSinf(beamAngle + PI_F * 0.5f);
        float beamSin = fastSinf(beamAngle);
        int beamSrcX = q(160 + par(steer, 0.75f));
        int beamSrcY = q(horizonY + PX * 2 + parY(pitch, 0.75f));
        int beamLen = PX * 12;
        for (int step = 0; step < beamLen; step += PX) {
            float t = (float)step / (float)max(1, beamLen);
            int bx = q(beamSrcX + (int)(beamCos * (float)step));
            int by = q(beamSrcY - (int)(fabsf(beamSin) * (float)step));
            if (by < WD_GLASS_T || by >= WD_GLASS_B) continue;
            if (!insideGlass(bx, by)) continue;
            uint8_t alpha = (uint8_t)clampi((int)(55.0f * (1.0f - t * t)), 0, 80);
            if (alpha < 10) continue;
            glowPx(canvas, bx, by, beamCol, alpha);
            if (bx + PX < 320 && insideGlass(bx + PX, by))
                glowPx(canvas, bx + PX, by, beamCol, (uint8_t)(alpha >> 1));
        }
    }

    // ==[ AIR TRAFFIC VOLUMETRIC SPOTLIGHTS ]== brief bright cones from flying cars
    //   cutting through the smog/fog volume above the city — aerial headlight beams
    //   volumetric scattering in the atmospheric depth, intermittent & sweeping
    if (!flash) {
        // Flying car volumetric beams — deeper in the atmospheric column than ground traffic
        // Uses a separate air traffic layer system with conical light scattering
        struct AirTrafficSlot {
            float depth;          // 0.15-0.35 (above mid-layer towers, below canopy)
            int slotW;
            float speed;
            int rows;
            uint8_t spawnMask;
            uint8_t baseStr;
            int baseOffY;         // PX offset above horizon (negative = above horizon)
            uint32_t salt;
            uint8_t beamAngleVar; // 0=steep down, 1=shallow, 2=steep up
            uint8_t coneWidth;    // cone spread in PX at max range
            uint8_t beamLenCells; // beam length in CPX cells
        };
        static constexpr AirTrafficSlot kAirTrafficLayers[] = {
            //  depth  slotW   spd    rows mask str  offY  salt      angVar coneW len
            {0.22f, PX*16, 38.0f, 1,  2,  210, -PX*6, 0xE881u, 1, PX*3, 10}, // high flying cars
            {0.18f, PX*14, 45.0f, 1,  1,  230, -PX*10, 0xF332u, 0, PX*4, 12}, // low swooping
            {0.30f, PX*20, 28.0f, 1,  3,  180, -PX*3,  0xA119u, 2, PX*2, 8},  // distant high
        };

        // Beam colors — cooler/whiter than ground traffic, more intense
        const uint16_t airBeamCol[] = {
            Display::screenBlend565(cityPal.cityNeon, RP::FLUOR, 60),   // cool white-blue
            Display::screenBlend565(cityPal.cityAmber, RP::FLUOR, 50),  // warm white
            Display::screenBlend565(RP::SPARK, RP::FLUOR, 40),          // bright spark
        };
        static constexpr uint32_t kAirBeamColorCount =
            sizeof(airBeamCol) / sizeof(airBeamCol[0]);

        for (int li = 0; li < 3; li++) {
            const AirTrafficSlot& at = kAirTrafficLayers[li];
            int cols = 320 / at.slotW + 3;
            int extraCols = cityExtraCols(at.slotW);
            int rowStep = max(PX * 4, at.slotW / 3);
            int extraRows = cityExtraRows(rowStep);
            int worldX = cityScrollX(steer, at.depth) + (int)(animT * at.speed);
            int worldY = (int)lroundf((float)cityScrollY(pitch, at.depth) * 0.35f);
            int baseY = horizonY + at.baseOffY;

            for (int row = -extraRows; row < at.rows + extraRows; row++) {
                for (int col = -1 - extraCols; col < cols + extraCols; col++) {
                    CityFeaturePoint pt = cityFeatureSlot(col, row, worldX, worldY,
                                                          at.slotW, rowStep, baseY, at.salt);
                    if ((pt.seed & 0xFu) >= at.spawnMask) continue;

                    int sy = q(pt.y + (int)((pt.seed >> 9) % 5u) * PX);
                    if (sy < WD_GLASS_T || sy >= WD_GLASS_B) continue;

                    int left, right;
                    glassBounds(sy, left, right);
                    if (right - left <= PX * 4) continue;

                    int startX = q(pt.x + (int)(pt.seed % (uint32_t)max(PX, at.slotW - PX * 4)));
                    if (startX + PX * 3 < left || startX > right) continue;

                    // Per-vehicle beam color variation
                    uint16_t beamCol =
                        airBeamCol[(pt.seed >> 20) % kAirBeamColorCount];
                    uint8_t beamStr = at.baseStr + (uint8_t)((pt.seed >> 5) & 0x1Fu);

                    // Duty cycle: brief appearances (20% on, 80% off) — "brief bright spotlights"
                    float phase = animT * 0.8f + (float)((pt.seed >> 8) & 0xFFu) * 0.01f;
                    float duty = phase - floorf(phase);
                    if (duty >= 0.20f) continue;  // only 20% of the time

                    // Fade in/out within the active window
                    float burstT = duty / 0.20f;
                    float fadeIn = (burstT < 0.3f) ? (burstT / 0.3f) : 1.0f;
                    float fadeOut = (burstT > 0.7f) ? (1.0f - (burstT - 0.7f) / 0.3f) : 1.0f;
                    float pulseAlpha = fadeIn * fadeOut;

                    // Beam direction from angle variant
                    float beamAngle;
                    if (at.beamAngleVar == 0) {
                        beamAngle = -PI_F * 0.45f - (float)((pt.seed >> 14) & 0x3u) * 0.08f; // steep down
                    } else if (at.beamAngleVar == 1) {
                        beamAngle = -PI_F * 0.15f + (float)(((pt.seed >> 14) & 0x7u) - 3) * 0.05f; // shallow
                    } else {
                        beamAngle = -PI_F * 0.70f + (float)((pt.seed >> 14) & 0x3u) * 0.06f; // steep up (rare)
                    }
                    float beamCos = fastSinf(beamAngle + PI_F * 0.5f);
                    float beamSin = fastSinf(beamAngle);

                    // Volumetric cone: draw expanding width with distance
                    // Each step is a horizontal slice of the cone at depth step
                    int beamLen = at.beamLenCells * CPX;
                    int coneW = at.coneWidth;

                    for (int step = 0; step < beamLen; step += CPX) {
                        float t = (float)step / (float)max(1, beamLen);

                        // Cone expands with distance
                        int coneHalfW = q((int)(coneW * t * 0.5f));
                        int bx = q(startX + (int)(beamCos * (float)step));
                        int by = q(sy - (int)(fabsf(beamSin) * (float)step));

                        if (by < WD_GLASS_T || by >= WD_GLASS_B) continue;

                        // Atmospheric attenuation: stronger near source, fades with distance + depth
                        float depthAtten = 1.0f - t * 0.65f;
                        float fogAtten = 1.0f - (float)(sy - WD_GLASS_T) / (float)max(1, WD_GLASS_B - WD_GLASS_T) * 0.3f;
                        uint8_t alpha = (uint8_t)clampi((int)(beamStr * pulseAlpha * depthAtten * fogAtten), 0, 255);
                        if (alpha < 15) continue;

                        // Draw cone cross-section at this depth (horizontal line expanding)
                        for (int cx = -coneHalfW; cx <= coneHalfW; cx += PX) {
                            int px = q(bx + cx);
                            if (px < left || px >= right - PX) continue;
                            if (!insideGlass(px, by)) continue;

                            // Center brighter, edges fall off
                            float edgeFalloff = 1.0f - (float)abs(cx) / (float)max(1, coneHalfW);
                            uint8_t sliceAlpha = (uint8_t)clampi((int)(alpha * (0.4f + edgeFalloff * 0.6f)), 0, 255);
                            if (sliceAlpha < 8) continue;

                            glowPx(canvas, px, by, beamCol, sliceAlpha);
                        }

                        // Add vertical "god ray" samples for volumetric depth — sparse
                        if ((step % (CPX * 2)) == 0 && (pt.seed & 0x1u)) {
                            int vy = q(by - (int)(fabsf(beamSin) * CPX * 0.5f));
                            if (vy >= WD_GLASS_T && vy < WD_GLASS_B && insideGlass(bx, vy)) {
                                glowPx(canvas, bx, vy, beamCol, (uint8_t)(alpha * 0.6f));
                            }
                        }
                    }

                    // Bright source point at vehicle position (headlight origin)
                    if (insideGlass(startX, sy)) {
                        uint8_t srcAlpha = (uint8_t)clampi((int)(beamStr * pulseAlpha * 1.5f), 0, 255);
                        glowPx(canvas, startX, sy, beamCol, srcAlpha);
                        if (startX + PX < right && insideGlass(startX + PX, sy))
                            glowPx(canvas, startX + PX, sy, beamCol, (uint8_t)(srcAlpha * 0.7f));
                    }
                }
            }
        }
    }

// ==[ 3-LAYER TRAFFIC ]== far/mid/close headlights with depth-scaled flares
    //   far:   tiny faint dots at horizon, fast crossing, atmosphere only
    //   mid:   moderate dots, mid-brightness, weak flare
    //   close: large bright headlights, slow crossing, strong cockpit flare sweep
    {
        struct TrafficLayer {
            float depth;
            int   slotW;
            float speedL, speedR;
            int   rows;
            uint8_t spawnMask;   // (seed & 0xF) < spawnMask to exist
            uint8_t baseStr;
            uint8_t dotCells;    // headlight width in PX cells
            float   flareMul;   // 0 = no flare, >0 = cockpit wash strength
            int     baseOffY;   // PX offset below horizon
            uint32_t salt;
        };
        static constexpr TrafficLayer kTrafficLayers[] = {
            //  depth  slotW   spdL  spdR  rows mask str dots flare offY   salt
            {0.78f, PX*22, 18.0f, 14.0f, 2, 5, 110, 1, 0.00f, PX*2,  0xB441u}, // far
            {0.55f, PX*18, 30.0f, 24.0f, 2, 3, 155, 2, 0.30f, PX*4,  0xC772u}, // mid
            {0.30f, PX*14, 52.0f, 42.0f, 1, 2, 195, 3, 0.70f, PX*8,  0xD104u}, // close
        };

        const uint16_t (&hlVariety)[4] = cityPal.hlVariety;
        uint16_t hlWarm = cityPal.headlightWarm;
        uint16_t hlCool = cityPal.headlightCool;

        for (int li = 0; li < 3; li++) {
            const TrafficLayer& tl = kTrafficLayers[li];
            int cols = 320 / tl.slotW + 3;
            int extraCols = cityExtraCols(tl.slotW);
            int rowStep = max(PX * 4, tl.slotW / 3);
            int extraRows = cityExtraRows(rowStep);
            int worldXL = cityScrollX(steer, tl.depth) - (int)(animT * tl.speedL);
            int worldXR = cityScrollX(steer, tl.depth) + (int)(animT * tl.speedR);
            int worldY  = (int)lroundf((float)cityScrollY(pitch, tl.depth) * 0.35f);
            int baseY   = horizonY + tl.baseOffY;
            bool hasFlare = tl.flareMul > 0.0f;

            for (int row = -extraRows; row < tl.rows + extraRows; row++) {
                bool rightward = (abs(row) & 1) != 0;
                int worldX = rightward ? worldXR : worldXL;
                for (int col = -1 - extraCols; col < cols + extraCols; col++) {
                    CityFeaturePoint pt = cityFeatureSlot(col, row, worldX, worldY,
                                                          tl.slotW, rowStep, baseY, tl.salt);
                    if ((pt.seed & 0xFu) >= tl.spawnMask) continue;
                    int sy = q(pt.y + (int)((pt.seed >> 9) % 7u) * PX);
                    if (sy < WD_GLASS_T || sy >= WD_GLASS_B) continue;
                    int left, right;
                    glassBounds(sy, left, right);
                    int minSpan = PX * (2 + tl.dotCells);
                    if (right - left <= minSpan) continue;
                    int startX = q(pt.x + (int)(pt.seed % (uint32_t)max(PX, tl.slotW - PX * 3)));
                    if (startX + PX * 3 < left || startX > right) continue;

                    // color: far uses warm/cool by direction, mid+close use variety palette
                    uint16_t hlCol = (li == 0) ? (rightward ? hlCool : hlWarm)
                                               : hlVariety[(pt.seed >> 20) & 0x3u];
                    // per-vehicle brightness variation
                    uint8_t hlStr = tl.baseStr + (uint8_t)((pt.seed >> 5) & 0x1Fu);
                    int dir = rightward ? 1 : -1;

                    // lead headlight
                    int h1 = q(startX);
                    if (h1 >= left && h1 < right - PX) glowPx(canvas, h1, sy, hlCol, hlStr);

                    // paired headlight (offset by direction)
                    int h2 = q(startX + dir * PX * 2);
                    if (h2 >= left && h2 < right - PX) glowPx(canvas, h2, sy, hlCol, (uint8_t)max(0, (int)hlStr - 25));

                    // extra width for mid+close: wider headlight spread
                    for (int d = 1; d < tl.dotCells; d++) {
                        int hx = q(h1 + dir * d * PX);
                        if (hx >= left && hx < right - PX) {
                            uint8_t ds = (uint8_t)max(40, (int)hlStr - d * 35);
                            glowPx(canvas, hx, sy, hlCol, ds);
                        }
                    }

                    // tail glow behind lead (motion blur) for close layer
                    if (tl.dotCells >= 3) {
                        int tailX = q(h1 - dir * PX);
                        if (tailX >= left && tailX < right - PX)
                            glowPx(canvas, tailX, sy, hlCol, (uint8_t)max(30, (int)hlStr - 80));
                    }

                    // cockpit flare — mid+close only (far skips entirely)
                    if (hasFlare) {
                        float centerDist = fabsf((float)startX - 160.0f) / 120.0f;
                        float depthProx = clamp01((float)(sy - horizonY) / (float)max(1, WD_GLASS_B - horizonY));
                        float flareStr = depthProx * max(0.0f, 1.0f - centerDist * 0.5f) * tl.flareMul;
                        if (flareStr > wdTrafficFlare) {
                            wdTrafficFlare = flareStr;
                            wdTrafficFlareCol = hlCol;
                            wdTrafficFlareCX = startX;
                            // Carry the lane's travel direction so the cockpit
                            // wash rakes the way the vehicle is actually going.
                            wdTrafficFlareDir = (int8_t)dir;
                        }
                    }
                }
            }
        }
    }
    }
}

void drawGlassBackdropBase(M5Canvas& canvas, uint32_t now, float motion) {
    drawGlassBackdropPass(canvas, now, motion, true);
}

void drawGlassBackdropMotion(M5Canvas& canvas, uint32_t now, float motion) {
    drawGlassBackdropPass(canvas, now, motion, false);
}

void drawGlassBackdrop(M5Canvas& canvas, uint32_t now, float motion) {
    drawGlassBackdropBase(canvas, now, motion);
    drawGlassBackdropMotion(canvas, now, motion);
}

} // namespace WardriveScene
