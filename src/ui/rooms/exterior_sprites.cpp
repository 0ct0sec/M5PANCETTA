/** exterior_sprites.cpp — generated city plates with runtime motion + light */

#include "exterior_sprites.h"
#include "exterior_sprites_data.h"
#include "../menu_pig_render.h"
#include <pgmspace.h>

namespace ExteriorSprites {

using namespace MenuPigRender;

namespace {

static constexpr int kCell = 4;
static constexpr int kMaxTraffic = 6;
using namespace UIMeasurements::MenuPigLayout;
static_assert(ExteriorSpriteData::kViewApartmentWidthCells * kCell ==
                  kR2_WindowW - kRoomPX * 2 &&
              ExteriorSpriteData::kViewApartmentHeightCells * kCell ==
                  kR2_WindowH - kRoomPX * 2,
              "apartment exterior must match the live pane");
static_assert(ExteriorSpriteData::kViewRamenWidthCells * kCell ==
                  kR3_WindowW - kRoomPX * 2 &&
              ExteriorSpriteData::kViewRamenHeightCells * kCell ==
                  kR3_WindowH - kRoomPX * 2,
              "ramen exterior must match the live pane");
static_assert(ExteriorSpriteData::kViewRooftopWidthCells * kCell ==
                  kR4_ExteriorW &&
              ExteriorSpriteData::kViewRooftopHeightCells * kCell ==
                  kR4_ExteriorH,
              "rooftop exterior must match the live horizon");
static_assert(ExteriorSpriteData::kViewComfortWidthCells * kCell ==
                  kR6_GlassW - kRoomPX * 2 &&
              ExteriorSpriteData::kViewComfortHeightCells * kCell ==
                  kR6_GlassH - kRoomPX * 2,
              "comfort exterior must match the live pane");

static constexpr int8_t snapParallaxToCell(int8_t value) {
    // Scenery normally arrives on the 4px room grid. Truncate defensively so
    // debug overrides cannot split plate, ads, traffic, and sampled emitters.
    return (int8_t)(((int)value / kCell) * kCell);
}
static_assert(snapParallaxToCell(-4) == -4 &&
              snapParallaxToCell(-2) == 0 &&
              snapParallaxToCell(2) == 0 &&
              snapParallaxToCell(4) == 4,
              "exterior parallax must quantize symmetrically");

static inline int snapToSceneGrid(int value, int origin) {
    return origin + ((value - origin) & ~(kCell - 1));
}

static constexpr bool rectIntersectsViewport(int sx, int sy, int sw, int sh,
                                             int x, int y, int w, int h) {
    return sx < x + w && sx + sw > x && sy < y + h && sy + sh > y;
}

static_assert(!rectIntersectsViewport(-8, 0, 8, 8, 0, 0, 32, 32),
              "edge-touching traffic must stay outside the viewport");
static_assert(rectIntersectsViewport(-4, 0, 8, 8, 0, 0, 32, 32),
              "partially visible traffic must intersect the viewport");

struct Lane {
    int8_t x0Pct, y0Pct, x1Pct, y1Pct;
    uint16_t periodMs;
    uint16_t phaseMs;
    uint8_t sprite;
    uint8_t color;
    uint8_t flare;
};

struct LaneSpan {
    uint8_t first;
    uint8_t count;
};

struct TrafficCache {
    uint32_t now = 0;
    int16_t x = 0, y = 0, w = 0, h = 0;
    int8_t parallaxX = 0;
    uint8_t count = 0;
    bool valid = false;
    TrafficSample samples[kMaxTraffic];
};

static TrafficCache trafficCache[4];

// Dominant room light should stay attached to one authored traffic lane until
// another source wins decisively. This is selection hysteresis only: position,
// color, and strength still come from the live source sample every frame.
struct EmitterHistory {
    uint32_t now = 0;
    int16_t x = 0, y = 0, w = 0, h = 0;
    int8_t parallaxX = 0;
    int8_t selectedLane = -1;
    bool valid = false;
};

static EmitterHistory emitterHistory[4];

// Different periods, depths, and approach vectors keep the sky alive without
// per-frame RNG. Body scale is baked into the selected generated sprite.
static constexpr Lane kLanes[] = {
    // apartment: sparse, distant, mostly lateral
    {-18, 24, 112, 30,  9300, 1300, 6, 1,  72},
    {116, 58, -18, 48,  6900, 4100, 1, 0, 108},
    // ramen: alley traffic crosses, approaches, and climbs away
    {-24, 30, 116, 36,  5800,  900, 0, 2, 118},
    {118, 63, -26, 57,  7900, 3600, 1, 0, 108},
    { 16, 94,  72, 10,  9700, 6200, 2, 3, 156},
    { 92, 12,  38, 88,  4300, 1700, 3, 1, 132},
    // rooftop: open vectors and mixed scale
    {-18, 30, 114, 38,  6500,  300, 6, 1,  64},
    {116, 44, -20, 54,  4700, 2100, 1, 0, 118},
    { 16, 86,  74, 20,  8200, 5600, 2, 2, 164},
    { 98, 26,  48, 94,  5900, 4300, 3, 3, 146},
    {-26, 78, 118, 66,  3600, 1100, 4, 1, 176},
    // comfort: denser boulevard, still no dominant billboard
    {-20, 18, 114, 24,  9800, 5800, 6, 1,  62},
    {118, 30, -18, 34,  7200, 2400, 1, 0,  94},
    {-28, 48, 116, 56,  5100,  500, 0, 2, 132},
    {118, 72, -30, 62,  8600, 3900, 4, 3, 148},
    { 14, 96,  70,  8, 11100, 7900, 2, 1, 182},
    { 98, 10,  46, 94,  6100, 1700, 5, 0, 164},
};

static constexpr LaneSpan kLaneSpans[] = {
    {0, 2}, {2, 4}, {6, 5}, {11, 6},
};

struct AdSlot {
    uint8_t xPct;
    uint8_t yPct;
    uint8_t baseIcon;
    uint8_t color;
    uint16_t periodMs;
    uint16_t phaseMs;
};

static constexpr AdSlot kApartmentAds[] = {
    {16, 18,  5, 1, 5100,  400},
    {68, 30,  3, 2, 7300, 2100},
    {46, 58, 11, 0, 8900, 4700},
};
static constexpr AdSlot kRamenAds[] = {
    { 8, 14,  0, 2, 4700,  700},
    {71, 20, 11, 0, 6100, 2800},
    {42, 54, 15, 3, 8300, 5200},
};
static constexpr AdSlot kRooftopAds[] = {
    { 5, 20,  7, 1, 5500,  300},
    {27, 40,  4, 2, 7100, 1800},
    {52, 18,  9, 0, 8900, 4100},
    {72, 46, 13, 3, 6300, 2400},
    {87, 25,  6, 1, 9700, 6600},
};
static constexpr AdSlot kComfortAds[] = {
    { 4, 16,  2, 0, 4900,  200},
    {20, 38,  8, 2, 6700, 1300},
    {39, 17, 10, 1, 8100, 3700},
    {57, 43, 12, 3, 5900, 2800},
    {74, 22, 14, 0, 9300, 6100},
    {89, 48,  1, 2, 7500, 4600},
};

static inline uint8_t packedToken(const ExteriorSpriteData::Sprite& sprite,
                                  int x, int y) {
    int index = y * (int)sprite.w + x;
    uint8_t packed = pgm_read_byte(sprite.data + index / 2);
    return (index & 1) ? (packed & 0x0Fu) : (packed >> 4);
}

static uint16_t accentColor(uint8_t accent) {
    switch (accent & 3u) {
        case 0: return RP::NEON;
        case 1: return RP::CRT;
        case 2: return RP::WARM;
        default: return RP::VEND;
    }
}

static uint16_t gradeDynamicColor(uint16_t color,
                                  const RenderOptions* options,
                                  bool emissive = true) {
    if (!options) return color;
    if (options->tintActive && options->tintIntensity > 0u) {
        uint8_t amount = emissive ? (uint8_t)(options->tintIntensity / 3u)
                                  : (uint8_t)(options->tintIntensity / 7u);
        color = mixColor565(color, options->tintColor565, amount);
    }
    if (options->thunder) {
        color = Display::lerpColor565(color, RP::FLUOR,
                                      emissive ? 0.34f : 0.22f);
    }
    return color;
}

static uint16_t tokenColor(uint8_t token, uint8_t accent,
                           const RenderOptions* options) {
    uint16_t color = RP::BG;
    switch (token) {
        case 1: color = RP::DEEP; break;
        case 2: color = RP::SHADOW_C; break;
        case 3: color = RP::WALL_FAR; break;
        case 4: color = RP::FILL; break;
        case 5: color = RP::WALL_MID; break;
        case 6: color = RP::WALL_NEAR; break;
        case 7: color = RP::STRUCT; break;
        case 8: color = RP::WARM; break;
        case 9: color = RP::NEON; break;
        case 10: color = RP::CRT; break;
        case 11: color = RP::VEND; break;
        case 12: color = RP::LED; break;
        case 13: color = RP::FLUOR; break;
        case 14: color = RP::SHAFT; break;
        case 15: color = RP::SOFT; break;
        default: color = RP::BG; break;
    }

    if (accent < 4u && token >= 8u && token <= 13u) {
        uint16_t chosen = accentColor(accent);
        color = token == 8u ? Display::lerpColor565(chosen, RP::WARM, 0.24f)
                            : Display::lerpColor565(chosen, RP::FLUOR, 0.18f);
    }
    return gradeDynamicColor(color, options, token >= 8u);
}

static void drawPackedScaled(M5Canvas& canvas,
                             const ExteriorSpriteData::Sprite& sprite,
                             int x, int y, int w, int h,
                             bool transparent, uint8_t accent,
                             const RenderOptions* options) {
    int cols = max(1, w / kCell);
    int rows = max(1, h / kCell);
    bool nativeSize = cols == (int)sprite.w && rows == (int)sprite.h;
    for (int ty = 0; ty < rows; ++ty) {
        int sy = nativeSize ? ty :
            min((int)sprite.h - 1, (ty * (int)sprite.h) / rows);
        if (options && sy < (int)options->transparentTopRows) continue;
        int runStart = 0;
        uint8_t runToken = 0xFFu;
        for (int tx = 0; tx <= cols; ++tx) {
            uint8_t token = 0xFEu;
            if (tx < cols) {
                int sampleTx = tx;
                if (options) {
                    sampleTx -= (int)options->parallaxX / kCell;
                    sampleTx = max(0, min(cols - 1, sampleTx));
                }
                int sx = nativeSize ? sampleTx :
                    min((int)sprite.w - 1, (sampleTx * (int)sprite.w) / cols);
                token = packedToken(sprite, sx, sy);
            }
            if (tx == 0) {
                runToken = token;
                continue;
            }
            if (token == runToken) continue;
            if (!(transparent && runToken == 0u)) {
                canvas.fillRect(x + runStart * kCell, y + ty * kCell,
                                (tx - runStart) * kCell, kCell,
                                tokenColor(runToken, accent, options));
            }
            runStart = tx;
            runToken = token;
        }
    }
}

static void drawPackedNative(M5Canvas& canvas,
                             const ExteriorSpriteData::Sprite& sprite,
                             int x, int y, int clipX, int clipY, int clipW, int clipH,
                             uint8_t accent, bool hologram = false,
                             bool tintBody = false,
                             const RenderOptions* options = nullptr) {
    for (int sy = 0; sy < (int)sprite.h; ++sy) {
        int py = y + sy * kCell;
        if (py < clipY || py >= clipY + clipH) continue;
        for (int sx = 0; sx < (int)sprite.w; ++sx) {
            int px = x + sx * kCell;
            if (px < clipX || px >= clipX + clipW) continue;
            uint8_t token = packedToken(sprite, sx, sy);
            if (token == 0u) continue;
            uint16_t color = tokenColor(token, accent, nullptr);
            if (hologram && token < 8u) {
                uint8_t lift = (uint8_t)(60u + token * 18u);
                color = screenBlend565(color, accentColor(accent), lift);
            } else if (tintBody && token < 8u) {
                uint8_t tint = token >= 3u ? 84u : 46u;
                color = screenBlend565(color, accentColor(accent), tint);
            }
            color = gradeDynamicColor(color, options,
                                      token >= 8u || hologram);
            canvas.fillRect(px, py, kCell, kCell, color);
        }
    }
}

template <size_t N>
static void drawAdSlots(M5Canvas& canvas, const AdSlot (&slots)[N],
                        uint32_t now, int x, int y, int w, int h,
                        const RenderOptions& options) {
    for (size_t i = 0; i < N; ++i) {
        const AdSlot& slot = slots[i];
        uint8_t icon = (uint8_t)((slot.baseIcon +
            ((now + slot.phaseMs) / slot.periodMs)) % 16u);
        const ExteriorSpriteData::Sprite& sprite = ExteriorSpriteData::kAds[icon];
        int px = snapToSceneGrid(x + ((int)slot.xPct * w) / 100 +
                                 (int)options.parallaxX, x);
        int py = snapToSceneGrid(y + ((int)slot.yPct * h) / 100, y);
        int sw = (int)sprite.w * kCell;
        int sh = (int)sprite.h * kCell;
        uint16_t glow = gradeDynamicColor(
            accentColor(slot.color + (uint8_t)(i & 1u)), &options, true);

        // Thin projector rail + scanline: hologram, not another billboard.
        int railY = min(y + h - kCell, py + sh);
        if (px >= x && px < x + w)
            canvas.fillRect(px, railY, min(sw, x + w - px), kCell,
                            gradeDynamicColor(RP::D_STRUCT, &options, false));
        int scanY = py + (int)(((now / 180u) + i * 3u) % max(1, (int)sprite.h)) * kCell;
        for (int sx = px; sx < min(px + sw, x + w); sx += kCell) {
            if (scanY >= y && scanY < y + h) {
                uint16_t base = fastReadPx(canvas, sx, scanY);
                canvas.fillRect(sx, scanY, kCell, kCell, screenBlend565(base, glow, 34));
            }
        }
        drawPackedNative(canvas, sprite, px, py, x, y, w, h,
                         (uint8_t)((slot.color + i) & 3u), true, false,
                         &options);
    }
}

static void drawAds(M5Canvas& canvas, Scene scene, uint32_t now,
                    int x, int y, int w, int h,
                    const RenderOptions& options) {
    switch (scene) {
        case Scene::Apartment: drawAdSlots(canvas, kApartmentAds, now, x, y, w, h, options); break;
        case Scene::Ramen: drawAdSlots(canvas, kRamenAds, now, x, y, w, h, options); break;
        case Scene::Rooftop: drawAdSlots(canvas, kRooftopAds, now, x, y, w, h, options); break;
        case Scene::Comfort: drawAdSlots(canvas, kComfortAds, now, x, y, w, h, options); break;
    }
}

struct FallbackAdAnchor {
    const AdSlot* slot;
    uint8_t ordinal;
};

static FallbackAdAnchor fallbackAdForScene(Scene scene) {
    // Choose an unobstructed authored slot in each view. The same slot is
    // visible, drawn, and sampled, so light never teleports to an orphan source.
    switch (scene) {
        case Scene::Apartment: return {&kApartmentAds[1], 1};
        case Scene::Ramen: return {&kRamenAds[1], 1};
        case Scene::Rooftop: return {&kRooftopAds[2], 2};
        case Scene::Comfort: return {&kComfortAds[2], 2};
    }
    return {&kApartmentAds[1], 1};
}

static void drawGlowCell(M5Canvas& canvas, int px, int py,
                         int x, int y, int w, int h,
                         uint16_t color, uint8_t strength) {
    if (px < x || px >= x + w || py < y || py >= y + h) return;
    uint16_t base = fastReadPx(canvas, px, py);
    canvas.fillRect(px, py, kCell, kCell, screenBlend565(base, color, strength));
}

static void drawTraffic(M5Canvas& canvas, Scene scene, uint32_t now,
                        int x, int y, int w, int h,
                        int8_t parallaxX,
                        const RenderOptions& options) {
    TrafficSample samples[kMaxTraffic];
    int count = sampleTraffic(scene, now, x, y, w, h,
                              samples, kMaxTraffic, parallaxX);
    for (int i = 0; i < count; ++i) {
        const TrafficSample& sample = samples[i];
        if (!sample.visible) continue;
        uint16_t glow = gradeDynamicColor(accentColor(sample.color),
                                          &options, true);
        int sx = sample.dirX > 0 ? 1 : (sample.dirX < 0 ? -1 : 0);
        int sy = sample.dirY > 0 ? 1 : (sample.dirY < 0 ? -1 : 0);
        int tailX = sample.x + (sx > 0 ? 0 : sample.w - kCell);
        int tailY = sample.y + sample.h / 2;
        for (int trail = 1; trail <= 3; ++trail) {
            drawGlowCell(canvas, snapToSceneGrid(tailX - sx * trail * kCell, x),
                         snapToSceneGrid(tailY - sy * trail * kCell, y),
                         x, y, w, h, glow, (uint8_t)(52 - trail * 10));
        }

        const ExteriorSpriteData::Sprite& sprite =
            ExteriorSpriteData::kVehicles[sample.sprite & 7u];
        drawPackedNative(canvas, sprite, sample.x, sample.y,
                         x, y, w, h, sample.color, false, true, &options);

        // The flare is attached to the car sample that also drives room light.
        int flareX = sample.emitterX;
        int flareY = sample.emitterY;
        uint8_t flare = (uint8_t)(((uint16_t)sample.flare *
                                   (uint16_t)(96u + sample.centerStrength)) / 351u);
        if (flare > 20u) {
            uint16_t flareColor = gradeDynamicColor(RP::FLUOR,
                                                     &options, true);
            drawGlowCell(canvas, snapToSceneGrid(flareX, x), snapToSceneGrid(flareY, y),
                         x, y, w, h, flareColor, flare);
            drawGlowCell(canvas, snapToSceneGrid(flareX - sx * kCell, x),
                         snapToSceneGrid(flareY, y),
                         x, y, w, h, glow, (uint8_t)(flare / 2u));
            if (sample.centerStrength > 128u) {
                drawGlowCell(canvas, snapToSceneGrid(flareX, x),
                             snapToSceneGrid(flareY - kCell, y),
                             x, y, w, h, glow, (uint8_t)(flare / 3u));
                drawGlowCell(canvas, snapToSceneGrid(flareX, x),
                             snapToSceneGrid(flareY + kCell, y),
                             x, y, w, h, glow, (uint8_t)(flare / 3u));
            }
        }
    }
}

} // namespace

int sampleTraffic(Scene scene, uint32_t now,
                  int x, int y, int w, int h,
                  TrafficSample* out, int capacity,
                  int8_t parallaxX) {
    if (!out || capacity <= 0 || w <= 0 || h <= 0) return 0;
    parallaxX = snapParallaxToCell(parallaxX);
    uint8_t sceneIndex = (uint8_t)scene;
    if (sceneIndex >= 4u) sceneIndex = 0u;
    TrafficCache& cache = trafficCache[sceneIndex];
    if (cache.valid && cache.now == now && cache.x == x && cache.y == y &&
        cache.w == w && cache.h == h && cache.parallaxX == parallaxX) {
        int cachedCount = min(capacity, (int)cache.count);
        for (int i = 0; i < cachedCount; ++i) out[i] = cache.samples[i];
        return cachedCount;
    }

    const LaneSpan& span = kLaneSpans[sceneIndex];
    int count = min(kMaxTraffic, (int)span.count);
    for (int i = 0; i < count; ++i) {
        const Lane& lane = kLanes[span.first + i];
        uint32_t phase = (now + lane.phaseMs) % lane.periodMs;
        int x0 = x + ((int)lane.x0Pct * w) / 100;
        int y0 = y + ((int)lane.y0Pct * h) / 100;
        int x1 = x + ((int)lane.x1Pct * w) / 100;
        int y1 = y + ((int)lane.y1Pct * h) / 100;
        int px = x0 + (int)(((int64_t)(x1 - x0) * phase) / lane.periodMs) +
                 (int)parallaxX;
        int py = y0 + (int)(((int64_t)(y1 - y0) * phase) / lane.periodMs);
        const ExteriorSpriteData::Sprite& sprite =
            ExteriorSpriteData::kVehicles[lane.sprite & 7u];

        TrafficSample& sample = cache.samples[i];
        sample.x = (int16_t)snapToSceneGrid(px, x);
        sample.y = (int16_t)snapToSceneGrid(py, y);
        sample.w = (int16_t)((int)sprite.w * kCell);
        sample.h = (int16_t)((int)sprite.h * kCell);
        sample.dirX = (int8_t)((x1 > x0) - (x1 < x0));
        sample.dirY = (int8_t)((y1 > y0) - (y1 < y0));
        sample.emitterX = (int16_t)snapToSceneGrid(
            sample.x + (sample.dirX >= 0 ? sample.w : -kCell), x);
        sample.emitterY = (int16_t)snapToSceneGrid(
            sample.y + sample.h / 2 + sample.dirY * (sample.h / 3), y);
        sample.sprite = lane.sprite;
        sample.color = lane.color;
        sample.flare = lane.flare;
        int cx = sample.x + sample.w / 2;
        int cy = sample.y + sample.h / 2;
        int dx = abs(cx - (x + w / 2)) * 255 / max(1, w / 2);
        int dy = abs(cy - (y + h / 2)) * 255 / max(1, h / 2);
        sample.centerStrength = (uint8_t)(255 - min(255, (dx + dy) / 2));
        sample.visible = rectIntersectsViewport(sample.x, sample.y,
                                                sample.w, sample.h,
                                                x, y, w, h);
    }
    cache.now = now;
    cache.x = (int16_t)x;
    cache.y = (int16_t)y;
    cache.w = (int16_t)w;
    cache.h = (int16_t)h;
    cache.parallaxX = parallaxX;
    cache.count = (uint8_t)count;
    cache.valid = true;

    int copyCount = min(capacity, count);
    for (int i = 0; i < copyCount; ++i) out[i] = cache.samples[i];
    return copyCount;
}

Emitter dominantEmitter(Scene scene, uint32_t now,
                        int x, int y, int w, int h,
                        int8_t parallaxX,
                        const RenderOptions& options) {
    parallaxX = snapParallaxToCell(parallaxX);
    uint8_t sceneIndex = (uint8_t)scene;
    if (sceneIndex >= 4u) sceneIndex = 0u;
    EmitterHistory& history = emitterHistory[sceneIndex];
    const bool historyMatches = history.valid &&
        history.x == x && history.y == y && history.w == w &&
        history.h == h && history.parallaxX == parallaxX &&
        (uint32_t)(now - history.now) < 1400u;
    TrafficSample samples[kMaxTraffic];
    int count = sampleTraffic(scene, now, x, y, w, h,
                              samples, kMaxTraffic, parallaxX);
    const TrafficSample* best = nullptr;
    int bestIndex = -1;
    int bestScore = -1;
    int bestRawScore = -1;
    for (int i = 0; i < count; ++i) {
        if (!samples[i].visible) continue;
        int rawScore = (int)samples[i].centerStrength +
                       min(96, (int)samples[i].w * (int)samples[i].h / 8);
        // Retain the same visible lane through near-ties. The emitted point is
        // still the exact sampled flare, never an interpolated orphan light.
        int score = rawScore +
            ((historyMatches && history.selectedLane == i) ? 64 : 0);
        if (score > bestScore) {
            bestScore = score;
            bestRawScore = rawScore;
            best = &samples[i];
            bestIndex = i;
        }
    }

    // When the authored fallback holo already owns the room, do not hand its
    // light to the first one-cell glimpse of an entering car. The car must be
    // visibly established; position still comes from that exact car sample.
    bool holdFallback = historyMatches && history.selectedLane < 0 &&
                        best && bestRawScore < 148;

    Emitter emitter;
    if (best && !holdFallback) {
        emitter.x = best->emitterX;
        emitter.y = best->emitterY;
        emitter.color565 = gradeDynamicColor(accentColor(best->color),
                                             &options, true);
        emitter.strength = (uint8_t)min(255, 72 + bestRawScore / 2);
        emitter.active = true;
        history.now = now;
        history.x = (int16_t)x;
        history.y = (int16_t)y;
        history.w = (int16_t)w;
        history.h = (int16_t)h;
        history.parallaxX = parallaxX;
        history.selectedLane = (int8_t)bestIndex;
        history.valid = true;
        return emitter;
    }

    // Between cars, sample a holo that drawAds() actually put in the view.
    FallbackAdAnchor anchor = fallbackAdForScene(scene);
    const AdSlot& slot = *anchor.slot;
    uint8_t icon = (uint8_t)((slot.baseIcon +
        ((now + slot.phaseMs) / slot.periodMs)) % 16u);
    const ExteriorSpriteData::Sprite& sprite = ExteriorSpriteData::kAds[icon];
    int adX = snapToSceneGrid(x + ((int)slot.xPct * w) / 100 +
                              (int)parallaxX, x);
    int adY = snapToSceneGrid(y + ((int)slot.yPct * h) / 100, y);
    emitter.x = (int16_t)max(x, min(x + w - kCell,
        snapToSceneGrid(adX + (int)sprite.w * kCell / 2, x)));
    emitter.y = (int16_t)max(y, min(y + h - kCell,
        snapToSceneGrid(adY + (int)sprite.h * kCell / 2, y)));
    emitter.color565 = gradeDynamicColor(
        accentColor((uint8_t)(slot.color + (anchor.ordinal & 1u))),
        &options, true);
    emitter.strength = 72;
    emitter.active = true;
    history.now = now;
    history.x = (int16_t)x;
    history.y = (int16_t)y;
    history.w = (int16_t)w;
    history.h = (int16_t)h;
    history.parallaxX = parallaxX;
    history.selectedLane = -1;
    history.valid = true;
    return emitter;
}

void drawSceneBase(M5Canvas& canvas, Scene scene,
                   int x, int y, int w, int h,
                   const RenderOptions& options) {
    if (w < kCell || h < kCell) return;
    uint8_t sceneIndex = (uint8_t)scene;
    if (sceneIndex >= 4u) sceneIndex = 0u;
    RenderOptions snappedOptions = options;
    snappedOptions.parallaxX = snapParallaxToCell(options.parallaxX);
    drawPackedScaled(canvas, ExteriorSpriteData::kViews[sceneIndex],
                     x, y, w, h, false, 0xFFu, &snappedOptions);
}

void drawSceneMotion(M5Canvas& canvas, Scene scene, uint32_t now,
                     int x, int y, int w, int h,
                     const RenderOptions& options) {
    if (w < kCell || h < kCell) return;
    RenderOptions snappedOptions = options;
    snappedOptions.parallaxX = snapParallaxToCell(options.parallaxX);
    drawAds(canvas, scene, now, x, y, w, h, snappedOptions);
    drawTraffic(canvas, scene, now, x, y, w, h,
                snappedOptions.parallaxX, snappedOptions);
}

void drawScene(M5Canvas& canvas, Scene scene, uint32_t now,
               int x, int y, int w, int h,
               const RenderOptions& options) {
    drawSceneBase(canvas, scene, x, y, w, h, options);
    drawSceneMotion(canvas, scene, now, x, y, w, h, options);
}

} // namespace ExteriorSprites
