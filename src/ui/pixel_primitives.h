/**
 * pixel_primitives.h — fat-pixel drawing primitives for the shared 320×240 canvas
 *
 * All coordinates snapped to kRoomPX=4 (scenery) or kPigPX=2 (pig/UI).
 * Zero heap allocation. Stateless — takes canvas + position + color.
 */
#pragma once

#include <M5Unified.h>
#include "display.h"
#include "../gfx/gfx.h"
#include "ui_measurements.h"
#include "menu_pig_render.h"

namespace PixelPrim {

using namespace UIMeasurements::MenuPigLayout;

// ==[ GRID SNAP ]==
inline int q4(int v) { return v & ~3; }  // snap to 4px
inline int q2(int v) { return v & ~1; }  // snap to 2px

// ==[ BLOCK FILLS ]== (forward to Gfx/MenuPigRender)
inline void fillBlock4(M5Canvas& c, int x, int y, uint16_t col) {
    Gfx::fastFillBlock4(c, q4(x), q4(y), col);
}
inline void fillBlock2(M5Canvas& c, int x, int y, uint16_t col) {
    Gfx::fastFillBlock2(c, q2(x), q2(y), col);
}

inline void fillSnappedRect(M5Canvas& c, int x, int y, int w, int h,
                            uint16_t col) {
    const int x0 = x < 0 ? 0 : x;
    const int y0 = y < 0 ? 0 : y;
    const int x1 = x + w > c.width() ? c.width() : x + w;
    const int y1 = y + h > c.height() ? c.height() : y + h;
    if (x0 >= x1 || y0 >= y1) return;

    const int stride = c.width();
    const uint16_t stored = Gfx::toBufferFmt(col);
    uint16_t* buffer = static_cast<uint16_t*>(c.getBuffer());
    for (int py = y0; py < y1; ++py) {
        uint16_t* dst = buffer + py * stride + x0;
        for (int px = x0; px < x1; ++px) *dst++ = stored;
    }
}

// Rect fills (grid-aligned)
inline void fillRect4(M5Canvas& c, int x, int y, int w, int h, uint16_t col) {
    if (w <= 0 || h <= 0) return;
    x = q4(x); y = q4(y); w = (w + 3) & ~3; h = (h + 3) & ~3;
    // Swap the color and establish each row once, rather than once per cell.
    // This preserves the fat-pixel envelope while cutting repeated hot-path
    // address and format work for furniture slabs, walls, and sign plates.
    fillSnappedRect(c, x, y, w, h, col);
}
inline void fillRect2(M5Canvas& c, int x, int y, int w, int h, uint16_t col) {
    if (w <= 0 || h <= 0) return;
    x = q2(x); y = q2(y); w = (w + 1) & ~1; h = (h + 1) & ~1;
    fillSnappedRect(c, x, y, w, h, col);
}

// ==[ LINES ]== Bresenham, fat-pixel (4px or 2px cells)
void fatLine4(M5Canvas& c, int x0, int y0, int x1, int y1, uint16_t col);
void fatLine2(M5Canvas& c, int x0, int y0, int x1, int y1, uint16_t col);

// ==[ CIRCLES / ELLIPSES ]== Midpoint, grid-snapped
void circle4(M5Canvas& c, int cx, int cy, int r, uint16_t col);
void ellipse4(M5Canvas& c, int cx, int cy, int rx, int ry, uint16_t col);

// ==[ ROUNDED RECT ]== 4px corner radius
void roundRect4(M5Canvas& c, int x, int y, int w, int h, uint16_t col);

// ==[ TRIANGLE ]== Flat-bottom/top, grid-snapped
void triangleFlatBottom4(M5Canvas& c, int x1, int y1, int x2, int y2, int x3, int y3, uint16_t col);
void triangleFlatTop4(M5Canvas& c, int x1, int y1, int x2, int y2, int x3, int y3, uint16_t col);

// ==[ GLYPH RENDERING ]== 5×7 block font, multi-layer (core + accents + shadow)
struct GlyphLayer {
    uint16_t color;
    int dx, dy;  // offset in cells
};
void drawBlockGlyph5x7(M5Canvas& c, int cx, int cy, char ch, int cell,
                       const GlyphLayer* layers, int nLayers);
void drawBlockText5x7(M5Canvas& c, int cx, int cy, const char* text, int cell,
                      const GlyphLayer* layers, int nLayers, int spacing = 6);

// ==[ SCREEN BLEND HELPERS ]== read-modify-write
inline uint16_t screenBlendPx(M5Canvas& c, int x, int y, uint16_t light, uint8_t s8) {
    uint16_t base = MenuPigRender::fastReadPx(c, x, y);
    return Gfx::screenBlend565(base, light, s8);
}
inline void blendBlock4(M5Canvas& c, int x, int y, uint16_t light, uint8_t s8) {
    uint16_t base = MenuPigRender::fastReadPx(c, x, y);
    Gfx::fastFillBlock4(c, x, y, Gfx::screenBlend565(base, light, s8));
}
inline void blendBlock2(M5Canvas& c, int x, int y, uint16_t light, uint8_t s8) {
    uint16_t base = MenuPigRender::fastReadPx(c, x, y);
    Gfx::fastFillBlock2(c, x, y, Gfx::screenBlend565(base, light, s8));
}

// ==[ ORDERED ROOM COVERAGE ]== Deterministic Bayer pattern
inline bool orderedRoomCoverage(int px, int py, uint8_t threshold, uint32_t seed) {
    int phaseX = (int)(seed & 3u);
    int phaseY = (int)((seed >> 2) & 3u);
    int bx = ((px / UIMeasurements::MenuPigLayout::kRoomPX) + phaseX) & 3;
    int by = ((py / UIMeasurements::MenuPigLayout::kRoomPX) + phaseY) & 3;
    return Gfx::bayer4[by][bx] < threshold;
}

} // namespace PixelPrim
