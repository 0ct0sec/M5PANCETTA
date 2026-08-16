// gfx.h — domain-agnostic rendering primitives
// Color math, pixel helpers, trig LUTs, blend functions.
// Canonical home — everything else forwards here.
#pragma once

#include <stdint.h>
#include <cmath>
#include <M5GFX.h>

namespace Gfx {

// ==[ MATH ]==
inline float clamp01(float v) { return (v < 0.0f) ? 0.0f : ((v > 1.0f) ? 1.0f : v); }
inline float clampf(float v, float lo, float hi) { return (v < lo) ? lo : ((v > hi) ? hi : v); }
inline float smoothstep01(float t) { t = clamp01(t); return t * t * (3.0f - 2.0f * t); }

// ==[ COLOR OPS ]==
uint16_t hsvToRgb565(uint16_t h, uint8_t s, uint8_t v);
uint16_t lerpColor565(uint16_t c1, uint16_t c2, float t);
uint8_t brightness565(uint16_t c);

// additive screen blend — always brightens base toward light color
inline uint16_t screenBlend565(uint16_t base, uint16_t light, uint8_t strength8) {
    int br = (base >> 11) & 0x1F, bg = (base >> 5) & 0x3F, bb = base & 0x1F;
    int lr = (((light >> 11) & 0x1F) * strength8) >> 8;
    int lg = (((light >> 5) & 0x3F) * strength8) >> 8;
    int lb = ((light & 0x1F) * strength8) >> 8;
    int or_ = br + lr - (br * lr + 15) / 31;
    int og  = bg + lg - (bg * lg + 31) / 63;
    int ob  = bb + lb - (bb * lb + 15) / 31;
    if (or_ > 31) or_ = 31;
    if (og > 63) og = 63;
    if (ob > 31) ob = 31;
    return (uint16_t)(or_ << 11) | (uint16_t)(og << 5) | (uint16_t)ob;
}

// float wrapper — converts once per call site
inline uint16_t screenBlend565f(uint16_t base, uint16_t light, float strength) {
    return screenBlend565(base, light, (uint8_t)(strength * 255.0f));
}

// integer-only lerp — t8: 0=c1, 255=c2
inline uint16_t lerpColor565_8(uint16_t c1, uint16_t c2, uint8_t t8) {
    if (t8 == 0) return c1;
    if (t8 == 255) return c2;
    int r1 = (c1 >> 11) & 0x1F, g1 = (c1 >> 5) & 0x3F, b1 = c1 & 0x1F;
    int r2 = (c2 >> 11) & 0x1F, g2 = (c2 >> 5) & 0x3F, b2 = c2 & 0x1F;
    uint8_t r = r1 + (((r2 - r1) * t8) >> 8);
    uint8_t g = g1 + (((g2 - g1) * t8) >> 8);
    uint8_t b = b1 + (((b2 - b1) * t8) >> 8);
    return (r << 11) | (g << 5) | b;
}

// clamp perceived brightness of RGB565 color (proportional channel scaling)
inline uint16_t clampBrightness565(uint16_t c, uint8_t maxBright) {
    int r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
    uint8_t br = (uint8_t)((r * 8 + g * 4 + b * 8) / 2);
    if (br <= maxBright || br == 0) return c;
    int scale8 = ((int)maxBright << 8) / br;
    r = (r * scale8) >> 8;
    g = (g * scale8) >> 8;
    b = (b * scale8) >> 8;
    if (r > 31) r = 31;
    if (g > 63) g = 63;
    if (b > 31) b = 31;
    return (uint16_t)(r << 11) | (uint16_t)(g << 5) | (uint16_t)b;
}

// ==[ PIXEL HELPERS ]==

// bypass readPixel virtual dispatch + bounds check
// LovyanGFX readPixel() unconditionally byte-swaps (swap565_t format).
// Buffer stores native-order; readPixel returns (data<<8)|(data>>8).
// We must match that format since all blend math expects swapped output.
inline uint16_t fastReadPx(M5Canvas& c, int x, int y) {
    uint16_t raw = ((uint16_t*)c.getBuffer())[y * c.width() + x];
    return (raw << 8) | (raw >> 8);
}

// standard 565 → buffer byte order (big-endian on ESP32 LovyanGFX)
inline uint16_t toBufferFmt(uint16_t c) { return (c << 8) | (c >> 8); }

// direct 4×4 fat pixel write — bypasses fillRect bounds/clip/rotation overhead.
// Caller must guarantee x in [0..316), y in [0..236), 4-aligned.
inline void fastFillBlock4(M5Canvas& c, int x, int y, uint16_t color) {
    uint16_t sw = toBufferFmt(color);
    uint16_t* row = &((uint16_t*)c.getBuffer())[y * c.width() + x];
    int stride = c.width();
    for (int dy = 0; dy < 4; dy++, row += stride)
        row[0] = row[1] = row[2] = row[3] = sw;
}

// direct 2×2 pig pixel write — same pattern, kPigPX=2
inline void fastFillBlock2(M5Canvas& c, int x, int y, uint16_t color) {
    uint16_t sw = toBufferFmt(color);
    uint16_t* row = &((uint16_t*)c.getBuffer())[y * c.width() + x];
    int stride = c.width();
    row[0] = row[1] = sw; row += stride;
    row[0] = row[1] = sw;
}

// deterministic noise — 3 integer ops, called hundreds of times/frame
inline uint32_t wallHash(int x, int y, uint32_t seed) {
    uint32_t h = (uint32_t)(x * 7919 + y * 6271 + seed);
    h ^= h >> 13; h *= 1274126177u; h ^= h >> 16;
    return h;
}

// 8-bit hash for small values
inline uint8_t hash8(uint16_t v) {
    v ^= (uint16_t)(v << 7);
    v ^= (uint16_t)(v >> 9);
    v *= 73u;
    return (uint8_t)v;
}

// snap to fat-pixel grid
inline int16_t snapToPx(int16_t v, int16_t px) {
    return (v >= 0) ? (v / px) * px : ((v - (px - 1)) / px) * px;
}

// Bresenham fat line on arbitrary PX grid
inline void fatLinePx(M5Canvas& canvas, int16_t x1, int16_t y1,
                      int16_t x2, int16_t y2, uint16_t color, int16_t px) {
    int gx1 = x1 / px, gy1 = y1 / px;
    int gx2 = x2 / px, gy2 = y2 / px;
    int dx = abs(gx2 - gx1), dy = abs(gy2 - gy1);
    int sx = (gx1 < gx2) ? 1 : -1, sy = (gy1 < gy2) ? 1 : -1;
    int err = dx - dy;
    while (true) {
        canvas.fillRect(gx1 * px, gy1 * px, px, px, color);
        if (gx1 == gx2 && gy1 == gy2) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; gx1 += sx; }
        if (e2 < dx)  { err += dx; gy1 += sy; }
    }
}

// ==[ TRIG ]==
extern const int16_t SIN_LUT_Q15[256];
inline float fastSinf(float rad) {
    int i = (int)(rad * (256.0f / 6.2831853f));
    return (float)SIN_LUT_Q15[i & 0xFF] * (1.0f / 32768.0f);
}

// ==[ DITHERING ]==
extern const uint8_t bayer4[4][4];

} // namespace Gfx
