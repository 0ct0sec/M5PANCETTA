#include "house_map.h"

#include <M5Unified.h>
#include "display.h"
#include "display_profile.h"
#include "menu_pig.h"
#include "menu_pig_render.h"
#include "ui_measurements.h"
#include "../util/psram_block.h"
#include <string.h>
#include <cstdlib>

namespace HouseMap {

namespace {

static constexpr uint8_t kCol[kRoomCount] = {0, 1, 0, 0, 1, 1};
static constexpr uint8_t kRow[kRoomCount] = {1, 1, 2, 0, 2, 0};

// left, right, up, down; -1 = closed outer wall
static constexpr int8_t kLeft[kRoomCount]  = {-1, 0, -1, -1, 2, 3};
static constexpr int8_t kRight[kRoomCount] = { 1,-1,  4,  5,-1,-1};
static constexpr int8_t kUp[kRoomCount]    = { 3, 5,  0, -1, 1,-1};
static constexpr int8_t kDown[kRoomCount]  = { 2, 4, -1,  0,-1, 1};

static M5Canvas* houseAtlas = nullptr;
static bool ready = false;
static uint16_t* baseTile[kRoomCount] = {};
static uint64_t baseTileKey[kRoomCount] = {};
static bool baseTileValid[kRoomCount] = {};

static void copyPlayfield(const uint16_t* tileBuf, uint16_t* atlasBuf,
                          uint8_t room, int tileW, int playY, int playH,
                          bool cropDoors) {
    const int dx0 = originX(room);
    const int dy0 = originY(room);
    const bool cropL = cropDoors && kLeft[room] >= 0;
    const bool cropR = cropDoors && kRight[room] >= 0;
    const int srcX = cropL ? kDoorCrop : 0;
    const int copyW = tileW - srcX - (cropR ? kDoorCrop : 0);
    if (copyW <= 0) return;
    for (int row = 0; row < playH; ++row) {
        const uint16_t* src = tileBuf + (playY + row) * tileW + srcX;
        uint16_t* dst = atlasBuf + (dy0 + playY + row) * DisplayProfile::kHouseW
                        + dx0 + srcX;
        memcpy(dst, src, (size_t)copyW * sizeof(uint16_t));
    }
}


static int8_t neighborOn(uint8_t room, Dir dir) {
    if (room >= kRoomCount) return -1;
    switch (dir) {
        case Dir::Left:  return kLeft[room];
        case Dir::Right: return kRight[room];
        case Dir::Up:    return kUp[room];
        case Dir::Down:  return kDown[room];
        default:         return -1;
    }
}

}  // namespace

void init() {
    if (ready) return;
    if (!enabled()) {
        ready = true;
        return;
    }
    houseAtlas = new M5Canvas(&M5.Display);
    if (!houseAtlas) return;
    houseAtlas->setPsram(true);
    houseAtlas->createSprite(DisplayProfile::kHouseW, DisplayProfile::kHouseH);
    if (!houseAtlas->getBuffer()) {
        delete houseAtlas;
        houseAtlas = nullptr;
        return;
    }
    ready = true;
}

uint8_t colOf(uint8_t room) {
    return (room < kRoomCount) ? kCol[room] : 0;
}

uint8_t rowOf(uint8_t room) {
    return (room < kRoomCount) ? kRow[room] : 0;
}

int originX(uint8_t room) {
    return (int)colOf(room) * DisplayProfile::kTileW;
}

int originY(uint8_t room) {
    return (int)rowOf(room) * DisplayProfile::kTileH;
}

int8_t neighbor(uint8_t room, Dir dir) {
    return neighborOn(room, dir);
}

Dir directionTo(uint8_t fromRoom, uint8_t toRoom) {
    if (fromRoom >= kRoomCount || toRoom >= kRoomCount) return Dir::None;
    if (kLeft[fromRoom] == (int8_t)toRoom) return Dir::Left;
    if (kRight[fromRoom] == (int8_t)toRoom) return Dir::Right;
    if (kUp[fromRoom] == (int8_t)toRoom) return Dir::Up;
    if (kDown[fromRoom] == (int8_t)toRoom) return Dir::Down;
    return Dir::None;
}

bool roomsAdjacent(uint8_t a, uint8_t b) {
    return directionTo(a, b) != Dir::None;
}

int worldX(uint8_t room, int localX) {
    return originX(room) + localX;
}

int worldY(uint8_t room, int localY) {
    return originY(room) + localY;
}

uint8_t roomAt(int wx, int wy) {
    if (wx < 0 || wy < 0) return 0xFF;
    const int col = wx / DisplayProfile::kTileW;
    const int row = wy / DisplayProfile::kTileH;
    if (col < 0 || col >= DisplayProfile::kHouseCols ||
        row < 0 || row >= DisplayProfile::kHouseRows) {
        return 0xFF;
    }
    for (uint8_t room = 0; room < kRoomCount; ++room) {
        if ((int)kCol[room] == col && (int)kRow[room] == row) return room;
    }
    return 0xFF;
}

uint8_t nextHop(uint8_t fromRoom, uint8_t toRoom) {
    if (fromRoom >= kRoomCount || toRoom >= kRoomCount) return 0xFF;
    if (fromRoom == toRoom) return fromRoom;
    if (roomsAdjacent(fromRoom, toRoom)) return toRoom;
    int best = -1;
    int bestDist = 99;
    const Dir dirs[4] = {Dir::Left, Dir::Right, Dir::Up, Dir::Down};
    for (Dir dir : dirs) {
        const int8_t n = neighborOn(fromRoom, dir);
        if (n < 0) continue;
        const int dist = abs((int)colOf((uint8_t)n) - (int)colOf(toRoom)) +
                         abs((int)rowOf((uint8_t)n) - (int)rowOf(toRoom));
        if (dist < bestDist) {
            bestDist = dist;
            best = n;
        }
    }
    return (best < 0) ? 0xFF : (uint8_t)best;
}

M5Canvas* atlas() {
    return houseAtlas;
}

void render(M5Canvas& dest, uint32_t now, uint8_t occupiedRoom) {
    if (!houseAtlas) init();
    if (!houseAtlas) return;

    MenuPig::syncHouseAtmosphere(now);

    const int tileW = DisplayProfile::kTileW;
    const int tileH = DisplayProfile::kTileH;
    const int playY = UIMeasurements::kTopBarH;
    const int playH = UIMeasurements::kMainAreaH;

    houseAtlas->fillSprite(MenuPigRender::RP::BG);

    M5Canvas* tile = Display::getSharedCanvas();
    // Occupied-room LIVE is drawn last into dest after tiles, via occupants.
    // Each room BASE+cheap LIVE is rendered through MenuPig into the 320 canvas
    // then copied. We snapshot/restore the 320 canvas around this so the tool
    // pane can still own it after present.
    uint16_t* tileBuf = tile ? static_cast<uint16_t*>(tile->getBuffer()) : nullptr;
    uint16_t* atlasBuf = static_cast<uint16_t*>(houseAtlas->getBuffer());
    if (!tileBuf || !atlasBuf) return;

    static uint16_t* stash = nullptr;
    if (!stash) {
        stash = static_cast<uint16_t*>(
            PsramBlock::alloc(tileW * tileH * sizeof(uint16_t)));
    }
    if (stash) memcpy(stash, tileBuf, tileW * tileH * sizeof(uint16_t));

    for (uint8_t room = 0; room < kRoomCount; ++room) {
        if (room == occupiedRoom) continue;
        const uint64_t key = MenuPig::houseTileKey(room);
        if (!baseTile[room]) {
            baseTile[room] = static_cast<uint16_t*>(
                PsramBlock::alloc((size_t)tileW * tileH * sizeof(uint16_t)));
        }
        const bool live = roomsAdjacent(occupiedRoom, room);
        if (baseTile[room] && baseTileValid[room] && baseTileKey[room] == key) {
            memcpy(tileBuf, baseTile[room],
                   (size_t)tileW * tileH * sizeof(uint16_t));
            if (live) MenuPig::drawRoomTile(*tile, room, now, true);
        } else {
            MenuPig::drawRoomTile(*tile, room, now, false);
            if (baseTile[room]) {
                memcpy(baseTile[room], tileBuf,
                       (size_t)tileW * tileH * sizeof(uint16_t));
                baseTileKey[room] = key;
                baseTileValid[room] = true;
            }
            if (live) MenuPig::drawRoomTile(*tile, room, now, true);
        }
        copyPlayfield(tileBuf, atlasBuf, room, tileW, playY, playH, true);
    }

    MenuPig::drawHouseOccupants(*houseAtlas, now);

    if (stash) memcpy(tileBuf, stash, tileW * tileH * sizeof(uint16_t));

    if (&dest != houseAtlas) {
        uint16_t* destBuf = static_cast<uint16_t*>(dest.getBuffer());
        if (destBuf && dest.width() == houseAtlas->width() &&
            dest.height() == houseAtlas->height()) {
            memcpy(destBuf, atlasBuf,
                   (size_t)DisplayProfile::kHouseW * DisplayProfile::kHouseH
                   * sizeof(uint16_t));
        }
    }
}

void blitToPanel(M5Canvas& panel) {
    if (!houseAtlas || !houseAtlas->getBuffer() || !panel.getBuffer()) return;
    const int houseW = DisplayProfile::kHouseW;
    const int houseH = DisplayProfile::kHouseH;
    const int panelW = panel.width();
    if (panelW < houseW || panel.height() < houseH) return;
    const uint16_t* src = static_cast<const uint16_t*>(houseAtlas->getBuffer());
    uint16_t* dst = static_cast<uint16_t*>(panel.getBuffer());
    for (int y = 0; y < houseH; ++y) {
        memcpy(dst + y * panelW + DisplayProfile::kHouseX,
               src + y * houseW,
               (size_t)houseW * sizeof(uint16_t));
    }
}

}  // namespace HouseMap
