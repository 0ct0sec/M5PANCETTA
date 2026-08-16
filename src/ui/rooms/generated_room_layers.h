/** generated_room_layers.h - deterministic room and pig runtime pixel pack. */
#pragma once

#include <M5Unified.h>
#include <stdint.h>

namespace MenuPig {
namespace RoomLayers {

enum class PigSprite : uint8_t {
    IDLE_RIGHT,
    IDLE_LEFT,
    REAR,
    SITTING,
    WALK_0,
    WALK_1,
    WALK_2,
    WALK_3,
    WALK_4,
    WALK_5,
    WALK_6,
    WALK_7,
};

void drawBack(M5Canvas& canvas, uint8_t room, uint32_t now,
              int16_t farDx, int16_t midDx);
void drawFront(M5Canvas& canvas, uint8_t room, uint32_t now, int16_t nearDx);
void drawPigSprite(M5Canvas& canvas, PigSprite sprite, int16_t x, int16_t y,
                   uint16_t fill, uint16_t detail, uint16_t paper, uint16_t light,
                   bool mirrorX = false);

}  // namespace RoomLayers
}  // namespace MenuPig
