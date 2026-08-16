/**
 * Wardrive Scene — cockpit POV renderer
 *
 * ==[ WARTHOG POV ]== rear-right pig head, holo panes, rain, whoosh stripes.
 * all rendering in M5Canvas. no heap. theme-reactive noir.
 */
#pragma once

#include <M5Unified.h>

namespace WardriveScene {

    // ==[ RENDER ]==
    void init();
    void drawScene(M5Canvas& canvas, uint32_t now);

    // ==[ ANIMATION STATE ]==
    void reset();         // alloc PSRAM scene buffers, clear state on mode enter
    void shutdown();      // free PSRAM scene buffers on mode exit

    // ==[ PERFORMANCE ]==
    void setParallaxEnabled(bool enabled);
    void resumeFrameClock();  // resume after the static telemetry tape

    // ==[ TELEPORT INTEGRATION ]==
    void getCockpitPigCenter(int16_t& cx, int16_t& cy);
    void triggerChairSettle(uint32_t now);
    bool isChairSettling();
}
