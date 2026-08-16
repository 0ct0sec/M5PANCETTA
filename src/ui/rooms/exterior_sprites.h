/**
 * exterior_sprites.h — compact city plates, holo ads, and shared traffic truth
 *
 * Generated art stays dumb. Runtime owns motion, weather, and light
 * evidence. Everything is fixed-size and allocation-free.
 */
#pragma once

#include <M5Unified.h>

namespace ExteriorSprites {

enum class Scene : uint8_t {
    Apartment = 0,
    Ramen = 1,
    Rooftop = 2,
    Comfort = 3,
};

struct RenderOptions {
    bool thunder = false;
    bool tintActive = false;
    uint16_t tintColor565 = 0;
    uint8_t tintIntensity = 0;
    int8_t parallaxX = 0;  // internal plate shift; viewport frame never moves
    uint8_t transparentTopRows = 0;  // reveal a live sky behind skyline rows
};

struct TrafficSample {
    int16_t x = 0;
    int16_t y = 0;
    int16_t w = 0;
    int16_t h = 0;
    int16_t emitterX = 0;
    int16_t emitterY = 0;
    int8_t dirX = 0;
    int8_t dirY = 0;
    uint8_t sprite = 0;
    uint8_t color = 0;
    uint8_t flare = 0;
    uint8_t centerStrength = 0;
    bool visible = false;
};

struct Emitter {
    int16_t x = 0;
    int16_t y = 0;
    uint16_t color565 = 0;
    uint8_t strength = 0;
    bool active = false;
};

// Split retained scenery from live motion so room caches never freeze traffic.
void drawSceneBase(M5Canvas& canvas, Scene scene,
                   int x, int y, int w, int h,
                   const RenderOptions& options = {});
void drawSceneMotion(M5Canvas& canvas, Scene scene, uint32_t now,
                     int x, int y, int w, int h,
                     const RenderOptions& options = {});

// Convenience composition for uncached callers.
void drawScene(M5Canvas& canvas, Scene scene, uint32_t now,
               int x, int y, int w, int h,
               const RenderOptions& options = {});

// One sampler is consumed by visible cars, room light volumes, and shadows.
// Samples are screen-space and clipped against the supplied viewport.
int sampleTraffic(Scene scene, uint32_t now,
                  int x, int y, int w, int h,
                  TrafficSample* out, int capacity,
                  int8_t parallaxX = 0);

// Strongest currently visible traffic emitter; falls back to a small holo ad.
Emitter dominantEmitter(Scene scene, uint32_t now,
                        int x, int y, int w, int h,
                        int8_t parallaxX = 0,
                        const RenderOptions& options = {});

} // namespace ExteriorSprites
