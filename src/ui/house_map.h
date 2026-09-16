#pragma once
/**
 * HouseMap — Tab5 2x3 building of six 320x240 rooms.
 *
 * Room pixel art is unchanged. This module places tiles, crops interior
 * walls so Pancetta can walk through, and tracks world coordinates.
 */
#include <Arduino.h>
#include <M5GFX.h>
#include "../hal/platform.h"
#include "display_profile.h"

namespace HouseMap {

enum class Dir : int8_t {
    None = 0,
    Left = 1,
    Right = 2,
    Up = 3,
    Down = 4
};

static constexpr uint8_t kRoomCount = 6;
static constexpr int kDoorCrop = 4;

void init();

inline bool enabled() { return DisplayProfile::houseMapEnabled(); }

// Floor plan: col 0/1, row 0=top (floor 2).
uint8_t colOf(uint8_t room);
uint8_t rowOf(uint8_t room);
int originX(uint8_t room);
int originY(uint8_t room);
int8_t neighbor(uint8_t room, Dir dir);
Dir directionTo(uint8_t fromRoom, uint8_t toRoom);
bool roomsAdjacent(uint8_t a, uint8_t b);

// World-space helpers. Local pig coords stay 0..320 x 14..240.
int worldX(uint8_t room, int localX);
int worldY(uint8_t room, int localY);
uint8_t roomAt(int worldX, int worldY);
uint8_t nextHop(uint8_t fromRoom, uint8_t toRoom);

// Render the six-tile atlas (BASE cache + LIVE for occupied/neighbors).
void render(M5Canvas& dest, uint32_t now, uint8_t occupiedRoom);

// Copy house atlas into a logical 1280x720 panel buffer at (kHouseX,kHouseY).
void blitToPanel(M5Canvas& panel);

M5Canvas* atlas();

}  // namespace HouseMap
