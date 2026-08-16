#ifndef FRAME_PRESENTER_H
#define FRAME_PRESENTER_H

#include <M5Unified.h>

namespace FramePresenter {

// Allocates the fixed DMA strips and tile history. Safe to call repeatedly.
// Falls back to M5Canvas::pushSprite if the boot-time allocations fail.
bool init();

// Present the completed 320x240 RGB565 canvas. No allocation occurs here.
void present(M5Canvas& canvas);

// Force the next presented canvas to refresh every tile. Use after rotation or
// any full-screen drawing path that bypasses present().
void invalidate();

// Force the next presented canvas to refresh the tiles covering this rectangle.
// Overlays painted straight onto the panel must report their footprint here or
// the diff will read those tiles as unchanged and leave the overlay behind.
void invalidateRect(int x, int y, int w, int h);

void printStats();
void resetStats();

} // namespace FramePresenter

#endif // FRAME_PRESENTER_H
