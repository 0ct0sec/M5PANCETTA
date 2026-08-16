#include "frame_presenter.h"

#include "frame_diff.h"
#include <esp_heap_caps.h>
#include <string.h>

namespace FramePresenter {

namespace {

static constexpr size_t kStagePixels = FrameDiff::kWidth * FrameDiff::kTileH;
static constexpr size_t kHashBytes = FrameDiff::kTileCount * sizeof(uint64_t);

static lgfx::swap565_t* stageBuffers[2] = {nullptr, nullptr};
static uint64_t* tileHashes = nullptr;
static uint8_t nextStage = 0;
static bool initialized = false;
static bool optimized = false;
static bool hashHistoryValid = false;

// Tiles the panel holds but the hash history never saw. Set by direct-to-LCD
// overlays after they draw; consumed and cleared by the next present().
static uint64_t forcedRows[FrameDiff::kTileRows] = {};

struct PresenterStats {
    uint64_t submittedPixels = 0;
    uint64_t plannedTiles = 0;
    uint32_t frames = 0;
    uint32_t zeroChangeFrames = 0;
    uint32_t fullRefreshFrames = 0;
    uint32_t fallbackFrames = 0;
    uint32_t maxPixels = 0;
    uint32_t lastPixels = 0;
    uint32_t lastRects = 0;
    uint32_t lastPlanUs = 0;
    uint32_t lastQueueUs = 0;
};

static PresenterStats stats;

static void recordFrame(uint32_t pixels, uint32_t rects,
                        uint16_t dirtyTiles, uint32_t planUs,
                        uint32_t queueUs, bool fallback) {
    ++stats.frames;
    stats.submittedPixels += pixels;
    stats.plannedTiles += dirtyTiles;
    stats.lastPixels = pixels;
    stats.lastRects = rects;
    stats.lastPlanUs = planUs;
    stats.lastQueueUs = queueUs;
    if (pixels == 0) ++stats.zeroChangeFrames;
    if (pixels == static_cast<uint32_t>(FrameDiff::kWidth * FrameDiff::kHeight)) {
        ++stats.fullRefreshFrames;
    }
    if (fallback) ++stats.fallbackFrames;
    if (pixels > stats.maxPixels) stats.maxPixels = pixels;
}

static void fallbackPresent(M5Canvas& canvas, uint32_t planUs = 0) {
    M5.Display.waitDMA();
    const uint32_t queueStart = micros();
    canvas.pushSprite(0, 0);
    // A full push repaints every forced tile by definition.
    memset(forcedRows, 0, sizeof(forcedRows));
    recordFrame(FrameDiff::kWidth * FrameDiff::kHeight, 1,
                FrameDiff::kTileCount, planUs, micros() - queueStart, true);
}

} // namespace

bool init() {
    if (initialized) return optimized;
    initialized = true;

    stageBuffers[0] = static_cast<lgfx::swap565_t*>(heap_caps_malloc(
        kStagePixels * sizeof(lgfx::swap565_t),
        MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    stageBuffers[1] = static_cast<lgfx::swap565_t*>(heap_caps_malloc(
        kStagePixels * sizeof(lgfx::swap565_t),
        MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    tileHashes = static_cast<uint64_t*>(heap_caps_malloc(
        kHashBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));

    optimized = stageBuffers[0] && stageBuffers[1] && tileHashes;
    if (!optimized) {
        if (stageBuffers[0]) heap_caps_free(stageBuffers[0]);
        if (stageBuffers[1]) heap_caps_free(stageBuffers[1]);
        if (tileHashes) heap_caps_free(tileHashes);
        stageBuffers[0] = nullptr;
        stageBuffers[1] = nullptr;
        tileHashes = nullptr;
    }
    hashHistoryValid = false;
    return optimized;
}

void present(M5Canvas& canvas) {
    if (!initialized) init();
    if (!optimized || canvas.width() != FrameDiff::kWidth ||
        canvas.height() != FrameDiff::kHeight || !canvas.getBuffer()) {
        fallbackPresent(canvas);
        return;
    }

    const uint32_t planStart = micros();
    const uint16_t* source = static_cast<const uint16_t*>(canvas.getBuffer());
    const FrameDiff::Plan plan = FrameDiff::buildPlan(
        source, canvas.width(), tileHashes, hashHistoryValid, forcedRows);
    hashHistoryValid = true;
    memset(forcedRows, 0, sizeof(forcedRows));
    const uint32_t planUs = micros() - planStart;

    uint32_t submittedPixels = 0;
    uint32_t submittedRects = 0;
    const uint32_t queueStart = micros();

    if (plan.dirtyTiles == 0) {
        // A settled screen must not open an SPI transaction just to close it.
        recordFrame(0, 0, 0, planUs, micros() - queueStart, false);
        return;
    }

    // One transaction for the whole frame. Each pushImageDMA would otherwise
    // open and close its own, and closing one busy-waits for the transfer to
    // drain — which would serialize every strip against its own DMA and make
    // the alternating stage buffers below pointless.
    M5.Display.startWrite();

    for (int tileY = 0; tileY < FrameDiff::kTileRows; ++tileY) {
        uint64_t dirty = plan.dirtyRows[tileY];
        while (dirty) {
            const int first = __builtin_ctzll(dirty);
            int end = first + 1;
            while (end < FrameDiff::kTileCols && (dirty & (1ull << end))) {
                ++end;
            }

            const int x = first * FrameDiff::kTileW;
            const int y = tileY * FrameDiff::kTileH;
            const int width = (end - first) * FrameDiff::kTileW;
            lgfx::swap565_t* stage = stageBuffers[nextStage];

            // The buffers alternate. M5GFX waits for the preceding DMA before
            // accepting the next descriptor, so the other strip can be packed
            // while the current strip is on the wire.
            for (int row = 0; row < FrameDiff::kTileH; ++row) {
                memcpy(stage + row * width,
                       source + (y + row) * FrameDiff::kWidth + x,
                       width * sizeof(uint16_t));
            }
            M5.Display.pushImageDMA(x, y, width, FrameDiff::kTileH, stage);
            nextStage ^= 1u;

            submittedPixels += static_cast<uint32_t>(width * FrameDiff::kTileH);
            ++submittedRects;

            const uint64_t runMask = ((1ull << (end - first)) - 1ull) << first;
            dirty &= ~runMask;
        }
    }

    // Closes the transaction and waits out the final strip, so the stage
    // buffers are free again before the caller can reach the next frame.
    M5.Display.endWrite();

    recordFrame(submittedPixels, submittedRects, plan.dirtyTiles, planUs,
                micros() - queueStart, false);
}

void invalidate() {
    if (initialized && optimized) M5.Display.waitDMA();
    hashHistoryValid = false;
}

void invalidateRect(int x, int y, int w, int h) {
    FrameDiff::markRect(forcedRows, x, y, w, h);
}

void printStats() {
    if (initialized && optimized) M5.Display.waitDMA();
    const uint32_t avgPixels = stats.frames > 0
        ? static_cast<uint32_t>(stats.submittedPixels / stats.frames) : 0u;
    const uint32_t avgPermille = static_cast<uint32_t>(
        (static_cast<uint64_t>(avgPixels) * 1000u) /
        (FrameDiff::kWidth * FrameDiff::kHeight));
    Serial.printf("[PRESENT] mode=%s frames=%lu last=%lupx/%lurects "
                  "avg=%lupx(%lu.%lu%%) max=%lupx zero=%lu full=%lu "
                  "fallback=%lu plan=%luus queue=%luus\n",
                  optimized ? "tiles-dma" : "full-fallback",
                  static_cast<unsigned long>(stats.frames),
                  static_cast<unsigned long>(stats.lastPixels),
                  static_cast<unsigned long>(stats.lastRects),
                  static_cast<unsigned long>(avgPixels),
                  static_cast<unsigned long>(avgPermille / 10u),
                  static_cast<unsigned long>(avgPermille % 10u),
                  static_cast<unsigned long>(stats.maxPixels),
                  static_cast<unsigned long>(stats.zeroChangeFrames),
                  static_cast<unsigned long>(stats.fullRefreshFrames),
                  static_cast<unsigned long>(stats.fallbackFrames),
                  static_cast<unsigned long>(stats.lastPlanUs),
                  static_cast<unsigned long>(stats.lastQueueUs));
}

void resetStats() {
    stats = PresenterStats{};
}

} // namespace FramePresenter
