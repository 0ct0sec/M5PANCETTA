// PigStars — night sky pixel stars with hash placement, fade-in, and blink
#pragma once

#include <M5Unified.h>
#include <stdint.h>

namespace PigStars {
    void init();
    void update();
    void draw(M5Canvas& canvas, uint16_t fg);
    bool isNightTime();
}
