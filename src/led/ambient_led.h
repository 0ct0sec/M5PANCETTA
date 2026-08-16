/**
 * AmbientLED - M5GO Bottom2 SK6812 LED strip ambient lighting
 *
 * 10 RGB LEDs (5 per side) on GPIO25. Samples screen edge colors
 * from the render canvas and pushes ambient glow to the LED strip.
 * Respects dim state and battery conservation.
 */
#pragma once

#include <M5GFX.h>

namespace AmbientLED {
    void init();
    void update(M5Canvas* canvas);  // sample edges + push LEDs (rate-limited)
    void off();                      // kill all LEDs immediately
}
