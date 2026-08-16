/**
 * AmbientLED - M5GO Bottom2 SK6812 ambient glow
 *
 * ==[ SIDE GLOW ]== 10 LEDs, 5 per side, GPIO25.
 * Three color modes:
 *   AUTO  — samples left/right screen edges (adaptive)
 *   THEME — solid color from current theme hue
 *   FIXED — solid color from preset hue (R3D, GR33N, CY4N, etc.)
 * Brightness independent of screen (1-10 scale).
 * Rate-limited to ~15fps.
 *
 * Buffer stores display-order (big-endian) bytes in LovyanGFX sprites.
 * Must byte-swap for standard RGB565 extraction (same as fastReadPx).
 */

#include "ambient_led.h"
#include "../core/config.h"
#include "../ui/display.h"
#include "../hamlet.h"
#include <FastLED.h>
#include "../hal/platform.h"

// Pin comes from platform.h. Undefined on CoreS3 SE — see init().
#ifdef HAMLET_LED_PIN
#define LED_PIN         HAMLET_LED_PIN
#endif
#define NUM_LEDS        10
#define UPDATE_MS       66   // ~15fps LED refresh
#define EDGE_DEPTH      24   // pixels to sample from each edge
#define SAMPLE_STEP_X   4    // horizontal sample stride
#define SAMPLE_STEP_Y   8    // vertical sample stride
#define EMA_ALPHA       64   // 64/256 = 25% new, 75% old (smooth transitions)

namespace AmbientLED {

static CRGB leds[NUM_LEDS];
static CRGB smooth[NUM_LEDS];  // EMA-filtered colors
static uint32_t lastUpdate = 0;
static bool initialized = false;
static bool ledsActive = false;
static bool firstFrame = true;

// ==[ PRESET HUES ]== 12 fixed colors (30° steps around HSV wheel)
// indices 2-13 in Config::getLedColor() map here (idx - 2)
static const uint16_t PRESET_HUES[] = {
    0, 30, 60, 90, 120, 150, 180, 210, 240, 270, 300, 330
};

void init() {
#ifdef LED_PIN
    FastLED.addLeds<SK6812, LED_PIN, GRB>(leds, NUM_LEDS);
    FastLED.setBrightness(0);
    FastLED.clear(true);
    memset(smooth, 0, sizeof(smooth));
    firstFrame = true;
    initialized = true;
#else
    // ==[ NO STRIP ]== CoreS3 SE: GPIO25 does not exist on the S3 and the
    // pin the Bottom2 LED line reaches over M-BUS is unverified. Leaving
    // initialized false makes update()/off() no-op on their existing
    // guards rather than driving a pin that isn't there.
    initialized = false;
#endif
}

// ==[ PIXEL READ ]== RGB565 from canvas buffer
// LovyanGFX sprite stores display-order (big-endian) bytes.
// Read as uint16_t on LE gives swapped value; swap back to standard RGB565.
static inline uint16_t readPx(M5Canvas* c, int x, int y) {
    uint16_t raw = ((uint16_t*)c->getBuffer())[y * c->width() + x];
    return (raw << 8) | (raw >> 8);
}

// ==[ ZONE SAMPLE ]== average a rectangular region → CRGB
static CRGB sampleZone(M5Canvas* c, int x0, int x1, int y0, int y1) {
    uint32_t rA = 0, gA = 0, bA = 0;
    int n = 0;

    for (int y = y0; y < y1; y += SAMPLE_STEP_Y) {
        for (int x = x0; x < x1; x += SAMPLE_STEP_X) {
            uint16_t px = readPx(c, x, y);
            rA += ((px >> 11) & 0x1F);
            gA += ((px >> 5)  & 0x3F);
            bA += (px & 0x1F);
            n++;
        }
    }

    if (n == 0) return CRGB::Black;
    // expand 5-6-5 to 8-8-8
    uint8_t r = (uint8_t)((rA / n) << 3);
    uint8_t g = (uint8_t)((gA / n) << 2);
    uint8_t b = (uint8_t)((bA / n) << 3);
    return CRGB(r, g, b);
}

// ==[ EMA BLEND ]== smooth single channel transition
static inline uint8_t emaBlend(uint8_t old_, uint8_t new_, uint8_t alpha) {
    return (uint8_t)(((uint16_t)old_ * (256 - alpha) + (uint16_t)new_ * alpha) >> 8);
}

// ==[ HSV → CRGB ]== convert hue angle to solid LED color
static CRGB hueToColor(uint16_t hue) {
    return CHSV((uint8_t)(hue * 255 / 359), 255, 255);
}

void update(M5Canvas* canvas) {
    if (!initialized || !Config::getLedAmbient()) {
        if (initialized && ledsActive) {
            FastLED.clear(true);
            ledsActive = false;
        }
        return;
    }

    // ==[ LOW-BATTERY GATE ]== 10 SK6812s at 15fps draw enough (~30-60mA)
    // to shorten runtime meaningfully on the 390mAh cell once we're already
    // critical. Below 10%, kill the strip and let the user reach a charger.
    if (Hamlet::getBatteryPercent() < 10) {
        if (ledsActive) {
            FastLED.clear(true);
            ledsActive = false;
        }
        return;
    }

    uint32_t now = millis();
    if (now - lastUpdate < UPDATE_MS) return;
    lastUpdate = now;

    uint8_t colorMode = Config::getLedColor();

    // ==[ SOLID COLOR MODES ]== theme or fixed hue
    if (colorMode > 0) {
        CRGB solid;
        if (colorMode == 1) {
            // Fixed skins follow their effect anchor, not the dormant hue knob.
            solid = hueToColor(Display::getAccentBaseHue());
        } else {
            // fixed preset (2-13 → hue index 0-11)
            uint8_t idx = colorMode - 2;
            if (idx >= 12) idx = 0;
            solid = hueToColor(PRESET_HUES[idx]);
        }

        // smooth transition to target color
        for (int i = 0; i < NUM_LEDS; i++) {
            if (firstFrame) {
                smooth[i] = solid;
            } else {
                smooth[i].r = emaBlend(smooth[i].r, solid.r, EMA_ALPHA);
                smooth[i].g = emaBlend(smooth[i].g, solid.g, EMA_ALPHA);
                smooth[i].b = emaBlend(smooth[i].b, solid.b, EMA_ALPHA);
            }
            leds[i] = smooth[i];
        }
        firstFrame = false;

        uint8_t bright = (Config::getLedBrightness() * 255 + 5) / 10;  // 1-10 → 26-255
        FastLED.setBrightness(bright);
        FastLED.show();
        ledsActive = true;
        return;
    }

    // ==[ AUTO MODE ]== sample screen edges (original behavior)
    if (!canvas || !canvas->getBuffer()) {
        if (ledsActive) { FastLED.clear(true); ledsActive = false; }
        return;
    }

    int h = canvas->height();  // 240
    int w = canvas->width();   // 320
    int zoneH = h / 5;         // 48px per LED zone

    // ==[ SAMPLE + SMOOTH ]== per-zone EMA filter
    for (int i = 0; i < 5; i++) {
        int y0 = i * zoneH;
        int y1 = y0 + zoneH;

        // left side: LEDs 0-4
        CRGB rawL = sampleZone(canvas, 0, EDGE_DEPTH, y0, y1);
        // right side: LEDs 5-9
        CRGB rawR = sampleZone(canvas, w - EDGE_DEPTH, w, y0, y1);

        if (firstFrame) {
            smooth[i] = rawL;
            smooth[5 + i] = rawR;
        } else {
            smooth[i].r = emaBlend(smooth[i].r, rawL.r, EMA_ALPHA);
            smooth[i].g = emaBlend(smooth[i].g, rawL.g, EMA_ALPHA);
            smooth[i].b = emaBlend(smooth[i].b, rawL.b, EMA_ALPHA);
            smooth[5+i].r = emaBlend(smooth[5+i].r, rawR.r, EMA_ALPHA);
            smooth[5+i].g = emaBlend(smooth[5+i].g, rawR.g, EMA_ALPHA);
            smooth[5+i].b = emaBlend(smooth[5+i].b, rawR.b, EMA_ALPHA);
        }

        leds[i] = smooth[i];
        leds[5 + i] = smooth[5 + i];
    }
    firstFrame = false;

    // ==[ BRIGHTNESS ]== use LED-specific brightness setting
    uint8_t bright = (Config::getLedBrightness() * 255 + 5) / 10;  // 1-10 → 26-255

    FastLED.setBrightness(bright);
    FastLED.show();
    ledsActive = true;
}

void off() {
    if (!initialized) return;
    FastLED.clear(true);
    ledsActive = false;
    firstFrame = true;
}

} // namespace AmbientLED
