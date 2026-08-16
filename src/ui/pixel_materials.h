/**
 * pixel_materials.h — surface materials for pixel art rooms
 *
 * Stateless draw functions. All coords snapped to kRoomPX=4.
 * Palette colors from MenuPigRender::RP::
 */
#pragma once

#include <M5Unified.h>
#include "menu_pig_render.h"
#include "ui_measurements.h"

namespace PixelMat {

using namespace UIMeasurements::MenuPigLayout;
using namespace MenuPigRender;

// ==[ CONCRETE WALL ]== Depth-graded pour: form ties, cold joint, wet drips,
// spalls with exposed aggregate/rebar, cracks with lit lips, kick plate.
// `variant` reseeds every wear layer — rooms sharing a rect must not share a
// wall. See the implementation's contrast-budget note before adding a layer.
void drawConcreteWall4(M5Canvas& c, int x, int y, int w, int h,
                       uint32_t variant = 0);

// ==[ EXPOSED BRICK ]== Staggered bricks, mortar, moisture streaks
void drawBrickWall4(M5Canvas& c, int x, int y, int w, int h, int parallaxX = 0);

// ==[ METAL FLOOR ]== Plate joints, bolts, traffic polish, rust, gutter
void drawMetalFloor4(M5Canvas& c, uint32_t variant = 0);

// ==[ WET GLASS ]== Window pane with rain streaks, condensation beads, reflection tint
void drawWetGlass4(M5Canvas& c, int x, int y, int w, int h,
                   uint16_t reflectionTint, uint8_t rainIntensity, uint32_t now);

// ==[ DEAD NEON MODULE ]== 4px cell, unlit (struct/fill) + lit overlay (neon)
void drawDeadNeonModule4(M5Canvas& c, int x, int y, uint32_t seed);

// ==[ CRT STATIC ]== Phosphor snow, scanline, channel flip
void drawCRTStatic4(M5Canvas& c, int x, int y, int w, int h, uint32_t now, uint8_t rfActivity);

// ==[ GRATE / VENT ]== Metal grille pattern
void drawGrate4(M5Canvas& c, int x, int y, int w, int h);
void drawSmallVent4(M5Canvas& c, int x, int y);

// ==[ PIPE / CONDUIT ]== Horizontal/vertical runs with joints, valves, rust
void drawPipeRun4(M5Canvas& c, int x, int y, int w, bool horizontal, bool withValve);
void drawConduitRun4(M5Canvas& c, int x, int y, int w);

// ==[ CABLE COIL ]== Floor cable bundle
void drawCableCoil4(M5Canvas& c, int x, int y);

// ==[ WALL OUTLET / FUSE BOX / SPRINKLER ]==
void drawWallOutlet4(M5Canvas& c, int x, int y);
void drawFuseBox4(M5Canvas& c, int x, int y);
void drawSprinkler4(M5Canvas& c, int x, int y);

// ==[ CEILING STAIN / DRIP ]==
void drawCeilingStain4(M5Canvas& c, int x, int y);

// ==[ WALL POSTER / CLOCK / EXTINGUISHER ]==
void drawWallPoster4(M5Canvas& c, int x, int y, int w, int h);
void drawWallClock4(M5Canvas& c, int x, int y);
void drawFireExtinguisher4(M5Canvas& c, int x, int y);

// ==[ FLOOR DRAIN / BOTTLE / ASHTRAY ]==
void drawFloorDrain4(M5Canvas& c, int x, int y);
void drawFloorBottle4(M5Canvas& c, int x, int y);
void drawAshtray4(M5Canvas& c, int x, int y);

// ==[ AC UNIT / SERVICE CASE / PATCH PANEL ]==
void drawACUnit4(M5Canvas& c, int x, int y);
void drawServiceCase4(M5Canvas& c, int x, int y);
void drawPatchPanel4(M5Canvas& c, int x, int y);

// ==[ MENU BOARD / NEON ARROW ]==
void drawMenuBoard4(M5Canvas& c, int x, int y);
void drawNeonArrow4(M5Canvas& c, uint32_t now, int x, int y);

} // namespace PixelMat
