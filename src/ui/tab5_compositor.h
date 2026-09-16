#pragma once
/**
 * Tab5Compositor — landscape house + chrome, PPA rotate to portrait DSI.
 *
 * Logical buffer is 1280x720. Physical panel is 720x1280. Never call
 * M5.Display.setRotation() for the blit path.
 */
#include <M5GFX.h>
#include "../hal/platform.h"
#include "../hamlet.h"

namespace Tab5Compositor {

void init();
void present(M5Canvas& toolCanvas);
void invalidate();

// Physical (rotation-0) touch -> logical landscape 1280x720.
void physicalToLogical(int16_t physX, int16_t physY,
                       int16_t& logicalX, int16_t& logicalY);

enum class TouchTarget : uint8_t {
    Tool,
    House,
    Status,
    Footer,
    Outside
};

// Map a physical touch into 320x240 UI space (tool/status/footer) or a house
// tile. houseRoom is 0xFF when the hit is not the building.
TouchTarget mapTouch(int16_t physX, int16_t physY,
                     int16_t& uiX, int16_t& uiY, uint8_t& houseRoom);

const char* modeLabel(HamletMode mode);

}  // namespace Tab5Compositor
