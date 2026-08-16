/**
 * pixel_furniture.h — furniture primitives for pixel art rooms
 *
 * Each function draws a complete furniture piece at grid position.
 * Stateless: canvas + position + animation time.
 * All coords snapped to kRoomPX=4.
 */
#pragma once

#include <M5Unified.h>
#include "display.h"
#include "ui_measurements.h"
#include "menu_pig_render.h"
#include "piglet/avatar.h"

namespace PixelFurn {

using namespace MenuPigRender;
using namespace UIMeasurements::MenuPigLayout;

// ==[ ROOM 0: CYBERDECK LAB ]==
void drawDualMonitorDesk4(M5Canvas& c, int x, int y, bool screenOn, uint32_t now);
void drawServerRack4(M5Canvas& c, int x, int y, uint32_t now);
void drawDeskLamp4(M5Canvas& c, int x, int y, uint32_t now);
void drawNeonSign_SYS4(M5Canvas& c, int x, int y, uint32_t now);
void drawCyberdolphinAquarium4(M5Canvas& c, int x, int y, int w, int h, uint32_t now);

// ==[ ROOM 1: NOIR APARTMENT ]==
void drawWornCouch4(M5Canvas& c, int x, int y, uint32_t now);
void drawVenetianWindow4(M5Canvas& c, int x, int y, int w, int h);
void drawFloorGrate4(M5Canvas& c, int x, int y, int w, int h);
void drawWallTV4(M5Canvas& c, int x, int y, int w, int h, uint32_t now);
void drawSideTableLamp4(M5Canvas& c, int x, int y, uint32_t now);

// ==[ ROOM 2: RAMEN BAR ]==
void drawRamenCounter4(M5Canvas& c, int x, int y, int w, int h);
void drawNoodleBowl4(M5Canvas& c, int x, int y, int w, int h, bool held, uint32_t now);
void drawBarStool4(M5Canvas& c, int x, int y);
void drawCoffinPod4(M5Canvas& c, int x, int y, int w, int h, bool occupied, uint32_t now);
void drawNeonSign_RAMEN4(M5Canvas& c, int x, int y, uint32_t now);
void drawPaperLantern4(M5Canvas& c, int x, int y, uint32_t now);

// ==[ ROOM 3: ROOST (SURVEILLANCE NEST) ]==
void drawAntennaArray4(M5Canvas& c, int x, int y, uint32_t now);
void drawSatelliteDish4(M5Canvas& c, int x, int y, uint32_t now);
void drawRooftopShack4(M5Canvas& c, int x, int y, int w, int h);
void drawLedgeRailing4(M5Canvas& c, int x, int y, int w);

// ==[ ROOM 4: UNDERGROUND BAR ]==
void drawCRTTerminal4(M5Canvas& c, int x, int y, int w, int h, uint32_t now);
void drawNeonSign_THEPEN4(M5Canvas& c, int x, int y, uint32_t now);
void drawBarCounter4(M5Canvas& c, int x, int y, int w, uint32_t now);
void drawCornerBooth4(M5Canvas& c, int x, int y, int w, int h);
void drawBarmanNPC4(M5Canvas& c, int x, int y, uint32_t now);
void drawGlassRack4(M5Canvas& c, int x, int y);
void drawKaraokeStage4(M5Canvas& c, int x, int y, uint32_t now);
void drawPendantLight4(M5Canvas& c, int x, int y, uint32_t now);

// ==[ ROOM 5: COMFORT BALCONY ]==
void drawHotBath4(M5Canvas& c, int x, int y, int w, int h, uint32_t now);
void drawRainGlassWall4(M5Canvas& c, int x, int y, int w, int h, uint32_t now, int pigX);
void drawBalconyDeck4(M5Canvas& c, int x, int y, int w);
void drawCandles4(M5Canvas& c, int x, int y, uint32_t now);

// ==[ SHARED PROPS ]==
void drawSakeBottle4(M5Canvas& c, int x, int y, uint32_t now);
void drawKettleSteam4(M5Canvas& c, int x, int y, uint32_t now);
void drawKettle4(M5Canvas& c, int x, int y, uint32_t now, bool steaming = true);
void drawRFBonsai4(M5Canvas& c, int rootX, int rootY, int maxH, uint32_t seed, uint32_t now, bool bathStyle);

} // namespace PixelFurn
