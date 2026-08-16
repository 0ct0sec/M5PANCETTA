#ifndef FRAME_DIFF_H
#define FRAME_DIFF_H

#include <stddef.h>
#include <stdint.h>

// Pure, host-testable change detector for the 320x240 RGB565 presentation
// surface. Rendering still composes a complete frame; only LCD traffic is
// sparse. Eight-pixel tiles preserve the room/pig pixel grids while keeping
// rain, hair, and HUD changes local.
namespace FrameDiff {

static constexpr int kWidth = 320;
static constexpr int kHeight = 240;
static constexpr int kTileW = 8;
static constexpr int kTileH = 8;
static constexpr int kTileCols = kWidth / kTileW;
static constexpr int kTileRows = kHeight / kTileH;
static constexpr int kTileCount = kTileCols * kTileRows;
static constexpr uint64_t kAllTileBits = (1ull << kTileCols) - 1ull;

struct Plan {
    uint64_t dirtyRows[kTileRows]{};
    uint16_t dirtyTiles = 0;
};

// Two independent 32-bit mixes make accidental unchanged classifications
// vanishingly unlikely without making the ESP32 pay for 64-bit multiplies.
inline uint64_t hashTile8(const uint16_t* pixels, int stride,
                          int tileX, int tileY) {
    uint32_t hashA = 2166136261u;
    uint32_t hashB = 0x9E3779B9u;
    const int x = tileX * kTileW;
    const int y = tileY * kTileH;

    for (int row = 0; row < kTileH; ++row) {
        const uint16_t* words = pixels + (y + row) * stride + x;
        for (int word = 0; word < kTileW / 2; ++word) {
            const uint32_t value =
                static_cast<uint32_t>(words[word * 2]) |
                (static_cast<uint32_t>(words[word * 2 + 1]) << 16);
            hashA = (hashA ^ value) * 16777619u;
            hashB ^= value + 0x9E3779B9u + (hashB << 6) + (hashB >> 2);
        }
    }
    return (static_cast<uint64_t>(hashA) << 32) | hashB;
}

// forcedRows marks tiles whose panel content is known to differ from the hash
// history even though the canvas bytes never moved. Overlays painted straight
// onto the LCD are invisible to the history, so the frame that stops drawing
// them has nothing to compare against; without the force bit those tiles are
// classified unchanged and the overlay stays burned into the panel.
inline Plan buildPlan(const uint16_t* pixels, int stride,
                      uint64_t* previousHashes, bool previousValid,
                      const uint64_t* forcedRows = nullptr) {
    Plan plan{};
    if (!pixels || !previousHashes || stride < kWidth) return plan;

    for (int tileY = 0; tileY < kTileRows; ++tileY) {
        const uint64_t forced = forcedRows ? forcedRows[tileY] : 0ull;
        uint64_t rowMask = 0;
        for (int tileX = 0; tileX < kTileCols; ++tileX) {
            const int index = tileY * kTileCols + tileX;
            const uint64_t hash = hashTile8(pixels, stride, tileX, tileY);
            if (!previousValid || (forced & (1ull << tileX)) ||
                previousHashes[index] != hash) {
                rowMask |= 1ull << tileX;
                ++plan.dirtyTiles;
            }
            previousHashes[index] = hash;
        }
        plan.dirtyRows[tileY] = rowMask;
    }
    return plan;
}

// Tile-aligned expansion of a pixel rectangle. Out-of-range rectangles clip
// rather than wrap; a caller that mis-measures an overlay loses repair area,
// never memory outside the mask.
inline void markRect(uint64_t* rows, int x, int y, int w, int h) {
    if (!rows || w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x >= kWidth || y >= kHeight || w <= 0 || h <= 0) return;
    if (x + w > kWidth) w = kWidth - x;
    if (y + h > kHeight) h = kHeight - y;

    const int firstCol = x / kTileW;
    const int lastCol = (x + w - 1) / kTileW;
    const int firstRow = y / kTileH;
    const int lastRow = (y + h - 1) / kTileH;
    // kTileCols is 40, so the span never reaches the 64-bit shift edge.
    const uint64_t colMask =
        ((1ull << (lastCol - firstCol + 1)) - 1ull) << firstCol;

    for (int tileY = firstRow; tileY <= lastRow; ++tileY) {
        rows[tileY] |= colMask;
    }
}

} // namespace FrameDiff

#endif // FRAME_DIFF_H
