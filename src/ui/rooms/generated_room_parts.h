/** generated_room_parts.h - generated scenery on the canonical 4px room grid. */
#pragma once

#include <M5Unified.h>
#include <stdint.h>

namespace MenuPig {
namespace RoomParts {

enum class PartId : uint8_t {
    LAB_CONSOLE,
    LAB_SERVER,
    LAB_CABLES,
    APARTMENT_SOFA,
    APARTMENT_WINDOW,
    APARTMENT_LAMP,
    APARTMENT_TV_PIPES,
    RAMEN_COUNTER,
    RAMEN_POD,
    RAMEN_SIGN_VENT,
    ROOFTOP_ANTENNA,
    ROOFTOP_DISH,
    ROOFTOP_SHACK,
    ROOFTOP_RAIL,
    BAR_TERMINAL,
    BAR_COUNTER,
    BAR_BOOTH,
    BAR_SIGN,
    COUNT
};

void drawBackground(M5Canvas& canvas, uint8_t room, int16_t farDx, int16_t midDx);
void drawForeground(M5Canvas& canvas, uint8_t room, int16_t nearDx);

}  // namespace RoomParts
}  // namespace MenuPig
