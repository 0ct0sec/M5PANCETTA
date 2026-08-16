/**
 * ui_measurements.h — HAMLET CORE
 *
 * Centralized UI geometry and text budgets for both 320×240 targets.
 *
 * Scaling ratios:
 *   Horizontal: 320/240 = 1.333
 *   Vertical playfield: 212/107 ≈ 1.981 (unified 14px bar contract)
 *   PIG_PX: 2 (pixel perfect), BIRD_PX: 4 (unified scenery grid)
 */
#pragma once

#include "display.h"

namespace UIMeasurements {

// ==[ PANEL GEOMETRY ]== single contract with display.h (14/212/14)
static constexpr int kScreenWidth = SCREEN_WIDTH;
static constexpr int kScreenHeight = SCREEN_HEIGHT;
static constexpr int kTopBarH = TOP_BAR_H;
static constexpr int kBottomBarH = BOTTOM_BAR_H;
static constexpr int kMainAreaTop = kTopBarH;
static constexpr int kMainAreaH = MAIN_H;  // 212

// ==[ FAT-PIXEL GRIDS ]==
static constexpr int kPigPX = 2;     // pig body pixels, direct 4px body renderer keeps the legacy 72x42 footprint
static constexpr int kRoomPX = 4;    // room interior pixels (chunky furniture/walls)
static constexpr int kBirdPX = 4;    // weather/scenery object pixels

// ==[ FONT METRICS ]==
static constexpr int kCharWSize1 = 6;
static constexpr int kCharHSize1 = 8;
static constexpr int kCharWSize2 = 12;
static constexpr int kCharHSize2 = 16;

static constexpr int maxCharsForWidth(int widthPx, int charW) {
    return (widthPx > 0 && charW > 0) ? (widthPx / charW) : 0;
}

static constexpr int maxCharsSize1(int widthPx) {
    return maxCharsForWidth(widthPx, kCharWSize1);
}

static constexpr int maxCharsSize2(int widthPx) {
    return maxCharsForWidth(widthPx, kCharWSize2);
}

// Common width budgets (320px wide).
static constexpr int kBudgetFullSize1 = maxCharsSize1(kScreenWidth);  // 53
static constexpr int kBudgetFullSize2 = maxCharsSize2(kScreenWidth);  // 26
static constexpr int kBudgetRow300Size1 = maxCharsSize1(300);         // 50
static constexpr int kBudgetRow300Size2 = maxCharsSize2(300);         // 25
static constexpr int kBudgetModal280Size1 = maxCharsSize1(280);       // 46
static constexpr int kBudgetModal280Size2 = maxCharsSize2(280);       // 23

// ==[ ROOM SCALING HELPERS ]==
// Map original 240×135 coordinates to 320×240
// ALL results snapped to kRoomPX=4 grid
static constexpr int roomX(int origX) {
    return ((origX * 320) / 240) & ~3;  // snap to 4px grid
}
static constexpr int roomY(int origY) {
    // Original playfield: y=14 to y=121 (107px) → y=14..226 (212px).
    // Snap the OFFSET from kTopBarH, not the absolute sum, so results land on the
    // room lattice (kRoomY=14 / kFloorY=218, both ≡ 2 mod 4) that walls and floor
    // already use. Snapping the absolute put roomY() content on the screen lattice
    // (multiples of 4), 2px off the floor grid — the S1 seam. Uniform −2px shift.
    return kTopBarH + ((((origY - 14) * kMainAreaH) / 107) & ~3);
}
static constexpr int roomW(int origW) {
    return ((origW * 320) / 240) & ~3;  // snap to 4px grid
}
static constexpr int roomH(int origH) {
    return ((origH * kMainAreaH) / 107) & ~3;  // snap to 4px grid
}
// Pig station positions — snap to 2px (pig body stays at kPigPX=2)
static constexpr int pigSnapX(int origX) {
    return ((origX * 320) / 240) & ~1;  // snap to 2px grid
}
static constexpr int pigSnapY(int origY) {
    return (kTopBarH + ((origY - 14) * kMainAreaH) / 107) & ~1;  // snap to 2px grid
}

// ==[ SHARED MENU RHYTHM ]==
static constexpr int kMenuTitleY = kTopBarH + 2;       // 16
static constexpr int kMenuDividerY = kTopBarH + 22;    // 36
static constexpr int kMenuStartY = kTopBarH + 24;      // 38
static constexpr int kMenuRowH = 22;
static constexpr int kMenuVisibleRows = 8;

// ==[ SETTINGS MENU ]== src/ui/settings_menu.cpp
namespace SettingsLayout {
static constexpr int kRootStartY = kTopBarH + 4;
static constexpr int kRootItemH = 18;
static constexpr int kRootVisibleRows = 10;
static constexpr int kRootSelectX = 5;
static constexpr int kRootSelectW = kScreenWidth - 10;
static constexpr int kRootTextX = 10;
static constexpr int kRootValueRightX = kScreenWidth - 20;
static constexpr int kScrollX = kScreenWidth - 20;

static constexpr int kModalW = kScreenWidth - 60;       // 260px
static constexpr int kModalH = 150;                     // title(20) + 7×18 + padding
static constexpr int kModalX = (kScreenWidth - kModalW) / 2;
static constexpr int kModalY = 18;
static constexpr int kModalTitleH = 20;
static constexpr int kModalItemStartY = kModalY + 22;
static constexpr int kModalItemH = 18;
static constexpr int kModalVisibleRows = 7;
static constexpr int kModalItemPadX = 6;
static constexpr int kModalTextIndent = 10;
static constexpr int kModalValueMargin = 14;

static constexpr int kWarningW = 260;
}

// ==[ MAIN MENU ]== src/ui/menu.cpp
namespace MainMenu {
static constexpr int kSelectX = 4;
static constexpr int kSelectW = 210;    // wider menu zone, pig helper on right
static constexpr int kTextX = 8;
static constexpr int kScrollX = 216;    // scroll indicators after select zone
static constexpr int kPracticalRowCharsSize2 = maxCharsSize2(208);  // 17
}

// ==[ MENU PIG ]== src/ui/menu_pig.cpp
namespace MenuPigLayout {
// Shared geometry — 320×240, playfield y=14..226 (212px main)
static constexpr int kFloorY = kScreenHeight - kBottomBarH - 8;  // 218 — gutter before nav bar
static constexpr int kRoomY = kTopBarH; // playfield top = 14
static constexpr int kRoomH = kMainAreaH;  // 212
// Mirror the outer UIMeasurements grid constants — one source of truth so the
// "minimum pixel resolution" cannot silently diverge between the two scopes.
static constexpr int kPigPX = UIMeasurements::kPigPX;    // 2 — pig X-snap grid
static constexpr int kRoomPX = UIMeasurements::kRoomPX;  // 4 — room fat-pixel grid
static constexpr int kPigW = 6 * kPigPX * 6;   // 72px (6 chars × 12px)
static constexpr int kPigH = 42;               // legacy body budget: 40px visible fat body + 2px top pad
static constexpr int kFrontPigAlignY = -kPigPX; // front body ink starts one pig cell high
static constexpr int kFloorPigY = kFloorY - kPigH; // 176 — pig standing on floor surface
static constexpr int kRearSeatOverlap = kRoomPX;   // rump sinks one scenery cell into a seat/lip
static constexpr int kGroundedRearPigY = kFloorPigY; // rear body owns contact; no legs below
static constexpr int kShadowW = 36;     // pig shadow width
static constexpr int kShadowH = 4;      // pig shadow height

// Structural room objects live above the floor contact line. Keep the proof
// here beside the anchor source of truth so scenery edits cannot silently
// leave the viewport while station checks still pass.
static constexpr bool roomObjectFits(int x, int y, int w, int h) {
    return x >= 0 && y >= kRoomY && w > 0 && h > 0 &&
           x + w <= kScreenWidth && y + h <= kFloorY;
}

// IMU camera travel is intentionally asymmetric where foreground art nearly
// touches a screen edge. Prove both extreme poses, not only the neutral frame.
static constexpr int kParallaxFarMin = -4;
static constexpr int kParallaxFarMax = 4;
static constexpr int kParallaxMidMin = -4;
static constexpr int kParallaxMidMax = 4;
static constexpr int kParallaxNearMin = -8;
static constexpr int kParallaxNearMax = 8;
static constexpr int kRooftopNearMax = 4;
static_assert(kParallaxFarMin % kRoomPX == 0 && kParallaxFarMax % kRoomPX == 0 &&
              kParallaxMidMin % kRoomPX == 0 && kParallaxMidMax % kRoomPX == 0 &&
              kParallaxNearMin % kRoomPX == 0 && kParallaxNearMax % kRoomPX == 0 &&
              kRooftopNearMax % kRoomPX == 0,
              "every scenery parallax pose must stay on the room grid");
static constexpr bool roomObjectFitsShifted(int x, int y, int w, int h,
                                            int minDx, int maxDx) {
    return roomObjectFits(x + minDx, y, w + maxDx - minDx, h);
}

// Center the 72px body on live furniture, then keep its origin on the
// 2px pig grid. Station anchors must not inherit the old 240-wide source X.
static constexpr int pigCenteredX(int propX, int propW) {
    return (propX + (propW - kPigW) / 2) & ~1;
}

// ==[ HELPER SCENE ]== dark alley corner, right side of menu screen
static constexpr int kBenchX = 204;       // tech crate under pig
static constexpr int kBenchY = roomY(100);
static constexpr int kBenchW = 96;        // bench under 108px pig
static constexpr int kBenchH = 16;
static constexpr int kHelperPigX = 200;   // pig sits on crate, facing left (right-aligned, 108px pig)
static constexpr int kHelperPigY = (kBenchY - kPigH + 4) & ~1;  // 142, ass on crate surface
// "HACK" neon sign
static constexpr int kNeonX = 240;
static constexpr int kNeonY = roomY(22);
static constexpr int kNeonW = 56;
static constexpr int kNeonH = 12;
// Ceiling pipe + drip
static constexpr int kPipeX = 232;
static constexpr int kPipeY = kTopBarH + 4;  // snapped to 4px grid
static constexpr int kPipeW = 80;
// Sparking wire
static constexpr int kSparkX = 256;
static constexpr int kSparkY = roomY(50);

// ==[ ROAMING — 6 ROOMS ]== full 320px each, y=14-226
static constexpr int kRoomW = 320;

// Idle timeout before roaming starts
static constexpr uint32_t kIdleTimeoutMs = 10000;
static constexpr uint32_t kRoomTransMs = 400;

// Room 0: Cyberdeck Lab v2 — 3 focal points: dual monitors, server cluster, neon sign
// Wider desk with dual flat-panel monitors + lamp
static constexpr int kR1_DeskX = roomX(4);
static constexpr int kR1_DeskY = roomY(96);
static constexpr int kR1_DeskW = roomW(72);
static constexpr int kR1_DeskH = roomH(18);
// Monitor A (left flat panel)
static constexpr int kR1_MonAX = roomX(8);
static constexpr int kR1_MonAY = roomY(72);
static constexpr int kR1_MonAW = roomW(24);
static constexpr int kR1_MonAH = roomH(24);
// Monitor B (right flat panel, slightly taller)
static constexpr int kR1_MonBX = roomX(36);
static constexpr int kR1_MonBY = roomY(68);
static constexpr int kR1_MonBW = roomW(28);
static constexpr int kR1_MonBH = roomH(28);
// Keyboard slab (simplified, wider desk)
static constexpr int kR1_KeybX = roomX(10);
static constexpr int kR1_KeybY = roomY(100);
static constexpr int kR1_KeybW = roomW(52);
static constexpr int kR1_KeybH = roomH(6);
// Desk lamp (right of monitors)
static constexpr int kR1_LampX = roomX(60);
static constexpr int kR1_LampY = roomY(78);
// Server cluster (mid-right, floor-standing)
static constexpr int kR1_SrvX = roomX(130);
static constexpr int kR1_SrvY = roomY(64);
static constexpr int kR1_SrvW = roomW(36);
static constexpr int kR1_SrvH = roomH(48);
// Neon sign (upper-right, far parallax)
static constexpr int kR1_NeonX = roomX(180);
static constexpr int kR1_NeonY = roomY(26);
static constexpr int kR1_NeonW = roomW(40);
static constexpr int kR1_NeonH = roomH(12);
// Pig station: AT_LAPTOP (rear view). The lower rump overlaps the desk/chair
// support by one room cell; the seated legs continue below that support.
static constexpr int kR1_PigX = pigCenteredX(kR1_DeskX, kR1_DeskW);
static constexpr int kR1_PigY =
    (kR1_DeskY - kPigH + kRearSeatOverlap) & ~(kPigPX - 1);

// Room 1: Noir Apartment
static constexpr int kR2_SofaX = roomX(10);
static constexpr int kR2_SofaY = roomY(86);
static constexpr int kR2_SofaW = roomW(80);
static constexpr int kR2_SofaH = roomH(28);
static constexpr int kR2_WindowX = roomX(100);
static constexpr int kR2_WindowY = roomY(20);
static constexpr int kR2_WindowW = roomW(120);
static constexpr int kR2_WindowH = roomH(60);
// Floor grate
static constexpr int kR2_GrateX = roomX(14);
static constexpr int kR2_GrateY = kFloorY - roomH(6);  // floor grate sits AT floor level (was roomY(108), floated 13px)
static constexpr int kR2_GrateW = roomW(72);
static constexpr int kR2_GrateH = roomH(6);
// Wall-mounted TV
static constexpr int kR2_TvX = roomX(28);
static constexpr int kR2_TvY = roomY(36);
static constexpr int kR2_TvW = roomW(30);
static constexpr int kR2_TvH = roomH(20);
// Side table + desk lamp
static constexpr int kR2_TableX = roomX(92);
static constexpr int kR2_TableY = roomY(96);
static constexpr int kR2_LampX = roomX(92);
static constexpr int kR2_LampY = roomY(82);
// Wall pipes
static constexpr int kR2_PipeX = roomX(224);
static constexpr int kR2_PipeY = roomY(26);
static constexpr int kR2_Pipe2X = roomX(232);
static constexpr int kR2_Pipe2Y = roomY(52);
// Pig stations (pigSnapX/Y snap to PX=2 grid for pig body)
static constexpr int kR2_SofaPigX = pigCenteredX(kR2_SofaX, kR2_SofaW);
static constexpr int kR2_SofaPigY = (kR2_SofaY + kR2_SofaH - 8 - kPigH + 4) & ~1;  // 162, ass on seat cushion
static constexpr int kR2_WindowPigX = pigCenteredX(kR2_WindowX, kR2_WindowW);
static constexpr int kR2_WindowPigY = kGroundedRearPigY;  // rear body ends on apartment floor

// Room 2: Ramen Bar
static constexpr int kR3_CounterX = roomX(10);
static constexpr int kR3_CounterY = roomY(86);
static constexpr int kR3_CounterW = roomW(50);
static constexpr int kR3_CounterH = roomH(28);
static constexpr int kR3_BowlX = roomX(18);
static constexpr int kR3_BowlY = roomY(70);
static constexpr int kR3_BowlW = roomW(26);
static constexpr int kR3_BowlH = roomH(16);
static constexpr int kR3_Stool1X = roomX(22);
static constexpr int kR3_Stool2X = roomX(38);
static constexpr int kR3_StoolY = roomY(92);
static constexpr int kR3_StoolW = roomW(8);
static constexpr int kR3_WindowX = roomX(134);
static constexpr int kR3_WindowY = roomY(18);
static constexpr int kR3_WindowW = roomW(96);
static constexpr int kR3_WindowH = roomH(56);
static constexpr int kR3_PodX = roomX(140);
static constexpr int kR3_PodY = roomY(86);
static constexpr int kR3_PodW = roomW(80);
static constexpr int kR3_PodH = roomH(28);
// Neon ラーメン sign (wall-mounted, far parallax)
static constexpr int kR3_SignX = roomX(80);
static constexpr int kR3_SignY = roomY(28);
static constexpr int kR3_SignW = 68;   // 4 katakana × 3px × kRoomPX + padding
static constexpr int kR3_SignH = 24;   // 4 rows × kRoomPX + padding
// Pig stations
// Living RF bonsai. The root is the planter lip, not a free-floating sprite
// origin, so growth always stays welded to the room floor.
static constexpr int kR3_RfRootX = 164;
static constexpr int kR3_RfRootY = kFloorY - 12;
static constexpr int kR3_RfTreeH = 72;
static constexpr int kR3_RfTreeHalfW = 36;
static constexpr int kR3_CookPigX = pigCenteredX(kR3_CounterX, kR3_CounterW);
static constexpr int kR3_CookPigY = (kR3_CounterY - kPigH + 4) & ~1;  // 118, behind counter lip
static constexpr int kR3_BedPigX = pigCenteredX(kR3_PodX, kR3_PodW);
static constexpr int kR3_BedPigY = kR3_PodY + 8;                       // 164, inside pod under ceiling

// Room 3: Surveillance Nest (Rooftop)
// Antenna array (left side, 3 poles)
static constexpr int kR4_AntennaX = roomX(4);
static constexpr int kR4_AntennaY = kRoomY + 4;
static constexpr int kR4_AntennaW = roomW(36);
static constexpr int kR4_AntennaH = kFloorY - kRoomY - 8;
// Satellite dish
static constexpr int kR4_DishX = roomX(50);
static constexpr int kR4_DishY = roomY(40);
static constexpr int kR4_DishW = roomW(40);
static constexpr int kR4_DishH = roomH(30);
// Rooftop shack (narrowed to open cityscape behind pig)
static constexpr int kR4_ShackX = 100;                    // was roomX(100)=132, pushed left
static constexpr int kR4_ShackY = roomY(24);
static constexpr int kR4_ShackW = 72;                     // was roomW(80)=104, narrowed for city view
static constexpr int kR4_ShackH = kFloorY - kR4_ShackY;   // extend to floor
// Sky zone boundary (haze, flying cars, cityscape start here)
static constexpr int kR4_SkyZoneX = 100;                  // was 128, matches narrower shack
// Skyline (open air, right of shack — 148px open sky)
static constexpr int kR4_SkyX = kR4_ShackX + kR4_ShackW;
static constexpr int kR4_SkyY = kRoomY;
// Generated skyline is a horizon overlay. Live storm air owns the full room
// behind it; the plate's upper rows are deliberately transparent at runtime.
static constexpr int kR4_ExteriorX = kR4_SkyZoneX;
static constexpr int kR4_ExteriorW = kScreenWidth - kR4_ExteriorX;
static constexpr int kR4_ExteriorH = 100;
static constexpr int kR4_ExteriorY = kFloorY - kR4_ExteriorH;
static constexpr int kR4_ExteriorSkyRows = 13;
static_assert(kR4_ExteriorW == 55 * kRoomPX &&
              kR4_ExteriorH == 25 * kRoomPX,
              "rooftop plate must stay at its native 55x25 cell size");
static_assert((kR4_ExteriorY - kRoomY) % kRoomPX == 0 &&
              kR4_ExteriorY + kR4_ExteriorH == kFloorY,
              "rooftop horizon must land on the floor grid");
static_assert(kR4_ExteriorSkyRows > 0 && kR4_ExteriorSkyRows < 25,
              "rooftop plate must retain a skyline below its live sky mask");
// Pig stations
static constexpr int kR4_AntennaPigX = pigSnapX(0);
static constexpr int kR4_AntennaPigY = kGroundedRearPigY;  // rear body ends on roof surface
// The live rail's readable seat bay is x=212..284. Center the body in that bay
// so the foreground post frames the flank instead of cutting through the belly.
static constexpr int kR4_LedgeSeatX = 212;
static constexpr int kR4_LedgeSeatW = 72;
static constexpr int kR4_LedgePigX = pigCenteredX(kR4_LedgeSeatX, kR4_LedgeSeatW);
static constexpr int kR4_LedgePigY = (kFloorY - 30 - kPigH + 4) & ~1;  // 150, ass on railing

// Room 4: Underground Bar
// Wall terminal CRT
static constexpr int kR5_TermX = roomX(4);
static constexpr int kR5_TermY = roomY(28);
static constexpr int kR5_TermW = roomW(48);
static constexpr int kR5_TermH = roomH(36);
// Terminal stool (pig sits on it facing CRT)
static constexpr int kR5_StoolX = 24;       // centered under pig body (pig center ~36)
static constexpr int kR5_StoolW = 20;       // seat width
static constexpr int kR5_StoolSeatY = 160;  // stool seat top
static constexpr int kR5_StoolSeatH = 8;    // seat thickness
// Bar counter (center, replaces hologram table)
static constexpr int kR5_BarX = 84;         // counter start X
static constexpr int kR5_BarTopY = 152;     // counter surface top
static constexpr int kR5_BarW = 96;         // counter width (84..180)
static constexpr int kR5_BarSurfH = 8;      // counter top thickness
// Corner booth (lowered, extends to floor)
static constexpr int kR5_BoothX = roomX(148);                   // 196
static constexpr int kR5_BoothY = 164;                          // seat level (was roomY(76)=132)
static constexpr int kR5_BoothW = roomW(80);                    // 104
static constexpr int kR5_BoothH = kFloorY - kR5_BoothY;        // 49, extends to floor
// Pendant light
static constexpr int kR5_LightX = roomX(180);
static constexpr int kR5_LightY = roomY(24);
// THE PEN neon sign (big tilted wall sign)
static constexpr int kR5_NeonX = 112;   // sign plate left X (centered on wall)
static constexpr int kR5_NeonY = 28;    // sign plate top Y
static constexpr int kR5_NeonW = 164;   // plate width (6 glyphs + tilt + padding)
static constexpr int kR5_NeonH = 36;    // plate height (7-row glyphs + padding)
// Glass rack (ceiling-mounted, above bar counter)
static constexpr int kR5_RackX = 96;          // left-aligned with counter + offset
static constexpr int kR5_RackY = 80;          // below neon sign bottom (64) + gap
static constexpr int kR5_RackW = 56;          // rack bar width
// Barman pig NPC — same 72×42 body as Pancetta, behind bar counter
static constexpr int kR5_BarmanX = 96;        // body left X (centered on counter: 96+36 = counter center 132)
static constexpr int kR5_BarmanY = 114;       // body top Y (counter at 152 hides bottom 4px + legs)
// Pig stations
static constexpr int kR5_TermPigX = pigCenteredX(kR5_TermX, kR5_TermW);
static constexpr int kR5_TermPigY = (kR5_StoolSeatY - kPigH + 4) & ~1;  // 122, ass on stool
static constexpr int kR5_BoothPigX = pigCenteredX(kR5_BoothX, kR5_BoothW);
static constexpr int kR5_BoothPigY = (kR5_BoothY - kPigH + 4) & ~1;     // 126, ass on booth seat

// Room 5: Comfort Balcony
// Floor-to-ceiling wet glass keeps the city outside; the tub and warm practical
// own the interior. The compact city plate stays behind live glass, traffic,
// privacy, and tub layers so station seams keep one source of truth.
static constexpr int kR6_GlassX = 4;
static constexpr int kR6_GlassY = kRoomY + 4;
static constexpr int kR6_GlassW = kScreenWidth - 8;
static constexpr int kR6_GlassH = 132;
// Four-pane 24-hour glass projection. Each 5x7 digit occupies one pane so the
// mullions remain physical occluders instead of cutting through digit strokes.
static constexpr int kR6_ClockCell = kRoomPX * 2;
static constexpr int kR6_ClockDigitW = 5 * kR6_ClockCell;
static constexpr int kR6_ClockDigitH = 7 * kR6_ClockCell;
// The retained exterior plate begins at glass + one frame cell. Keep every
// projected 8px clock cell on that local 4px substrate grid.
static constexpr int kR6_ClockY = kR6_GlassY + kRoomPX + 4 * kRoomPX;
static constexpr int kR6_ClockDigit0X = 16;
static constexpr int kR6_ClockDigit1X = 96;
static constexpr int kR6_ClockColonX = 152;
static constexpr int kR6_ClockDigit2X = 184;
static constexpr int kR6_ClockDigit3X = 264;
static constexpr int kR6_ClockMeltMax = 24;
static constexpr int kR6_TubX = 68;
static constexpr int kR6_TubY = 150;
static constexpr int kR6_TubW = 184;
static constexpr int kR6_TubH = 52;
static constexpr int kR6_TubWaterY = kR6_TubY + 8;
static constexpr int kR6_TubFrontY = kR6_TubY + 16;
static constexpr int kR6_BathPigX = pigCenteredX(kR6_TubX, kR6_TubW);
static constexpr int kR6_BathPigY = (kR6_TubY - 26) & ~1;               // 124, torso above waterline
// The dive travel absorbs the seat height: the soak pose sits one pig cell
// higher than the tub rim suggests, and the dive still has to land the snapped
// nostril row on the live water surface. The tub foreground contains the whole
// body at either end of the travel.
static constexpr int kR6_BathDiveMaxY = kPigPX * 9;
static constexpr int kR6_BathDirectNostrilInsetY = 18;
// Bath entry owns real left/right deck approaches. Pancetta reaches one rim,
// clears it in an arc, then lands at the centered water anchor.
static constexpr int kR6_BathApproachLeftX = kR6_TubX - kPigW + kRoomPX;
static constexpr int kR6_BathApproachRightX = kR6_TubX + kR6_TubW - kRoomPX;
// Balcony RF tree grows from the planter lip behind the left end of the tub.
static constexpr int kR6_RfRootX = 40;
static constexpr int kR6_RfRootY = kFloorY - 12;
static constexpr int kR6_RfTreeH = 88;
static constexpr int kR6_RfTreeHalfW = 36;
// Highest branch tips and leaf cells can clear the full trunk by two cells.
static constexpr int kRfTreeCanopyPad = kRoomPX * 2;

// ==[ STRUCTURAL OBJECT ENVELOPES ]== live procedural hosts, not just imported ink
static_assert(roomObjectFits(kR1_DeskX, kR1_DeskY, kR1_DeskW, kR1_DeskH),
              "room 0 desk must stay inside the playfield");
static_assert(roomObjectFits(kR1_MonAX, kR1_MonAY, kR1_MonAW, kR1_MonAH),
              "room 0 monitor A must stay inside the playfield");
static_assert(roomObjectFits(kR1_MonBX, kR1_MonBY, kR1_MonBW, kR1_MonBH),
              "room 0 monitor B must stay inside the playfield");
static_assert(roomObjectFits(kR1_KeybX, kR1_KeybY, kR1_KeybW, kR1_KeybH),
              "room 0 keyboard must stay inside the playfield");
static_assert(roomObjectFits(kR1_SrvX, kR1_SrvY, kR1_SrvW, kR1_SrvH),
              "room 0 server must stay inside the playfield");
static_assert(roomObjectFits(kR1_NeonX, kR1_NeonY, kR1_NeonW, kR1_NeonH),
              "room 0 sign must stay inside the playfield");

static_assert(roomObjectFits(kR2_SofaX, kR2_SofaY, kR2_SofaW, kR2_SofaH),
              "room 1 sofa must stay inside the playfield");
static_assert(roomObjectFits(kR2_WindowX, kR2_WindowY, kR2_WindowW, kR2_WindowH),
              "room 1 window must stay inside the playfield");
static_assert(roomObjectFits(kR2_GrateX, kR2_GrateY, kR2_GrateW, kR2_GrateH),
              "room 1 grate must stay inside the playfield");
static_assert(roomObjectFits(kR2_TvX, kR2_TvY, kR2_TvW, kR2_TvH),
              "room 1 TV must stay inside the playfield");

static_assert(roomObjectFits(kR3_CounterX, kR3_CounterY, kR3_CounterW, kR3_CounterH),
              "room 2 counter must stay inside the playfield");
static_assert(roomObjectFits(kR3_BowlX, kR3_BowlY, kR3_BowlW, kR3_BowlH),
              "room 2 bowl must stay inside the playfield");
static_assert(roomObjectFits(kR3_WindowX, kR3_WindowY, kR3_WindowW, kR3_WindowH),
              "room 2 window must stay inside the playfield");
static_assert(roomObjectFits(kR3_PodX, kR3_PodY, kR3_PodW, kR3_PodH),
              "room 2 pod must stay inside the playfield");
static_assert(roomObjectFits(kR3_SignX, kR3_SignY, kR3_SignW, kR3_SignH),
              "room 2 sign must stay inside the playfield");

static_assert(roomObjectFits(kR4_AntennaX, kR4_AntennaY, kR4_AntennaW, kR4_AntennaH),
              "room 3 antenna must stay inside the playfield");
static_assert(roomObjectFits(kR4_DishX, kR4_DishY, kR4_DishW, kR4_DishH),
              "room 3 dish must stay inside the playfield");
static_assert(roomObjectFits(kR4_ShackX, kR4_ShackY, kR4_ShackW, kR4_ShackH),
              "room 3 shack must stay inside the playfield");
static_assert(roomObjectFits(kR4_LedgeSeatX, kFloorY - 30, kR4_LedgeSeatW, 30),
              "room 3 ledge bay must stay inside the playfield");

static_assert(roomObjectFits(kR5_TermX, kR5_TermY, kR5_TermW, kR5_TermH),
              "room 4 terminal must stay inside the playfield");
static_assert(roomObjectFits(kR5_StoolX, kR5_StoolSeatY, kR5_StoolW, kR5_StoolSeatH),
              "room 4 terminal stool must stay inside the playfield");
static_assert(roomObjectFits(kR5_BarX, kR5_BarTopY, kR5_BarW, kFloorY - kR5_BarTopY),
              "room 4 bar must stay inside the playfield");
static_assert(roomObjectFits(kR5_BoothX, kR5_BoothY, kR5_BoothW, kR5_BoothH),
              "room 4 booth must stay inside the playfield");
static_assert(roomObjectFits(kR5_NeonX, kR5_NeonY, kR5_NeonW, kR5_NeonH),
              "room 4 sign must stay inside the playfield");
static_assert(roomObjectFits(kR5_BarmanX, kR5_BarmanY, kPigW, kPigH),
              "room 4 barman must stay inside the playfield");

static_assert(roomObjectFits(kR6_GlassX, kR6_GlassY, kR6_GlassW, kR6_GlassH),
              "room 5 glass wall must stay inside the playfield");
static_assert(roomObjectFits(kR6_TubX, kR6_TubY, kR6_TubW, kR6_TubH),
              "room 5 tub must stay inside the playfield");
static_assert(((kR6_ClockY - (kR6_GlassY + kRoomPX)) % kRoomPX) == 0 &&
              ((kR6_ClockDigit0X - (kR6_GlassX + kRoomPX)) % kRoomPX) == 0 &&
              ((kR6_ClockDigit1X - (kR6_GlassX + kRoomPX)) % kRoomPX) == 0 &&
              ((kR6_ClockColonX - (kR6_GlassX + kRoomPX)) % kRoomPX) == 0 &&
              ((kR6_ClockDigit2X - (kR6_GlassX + kRoomPX)) % kRoomPX) == 0 &&
              ((kR6_ClockDigit3X - (kR6_GlassX + kRoomPX)) % kRoomPX) == 0,
              "room 5 glass clock must stay on the retained pane grid");
static_assert(kR6_ClockDigit0X >= kR6_GlassX + kRoomPX &&
              kR6_ClockDigit3X + kR6_ClockDigitW <=
                  kR6_GlassX + kR6_GlassW - kRoomPX &&
              kR6_ClockY >= kR6_GlassY + kRoomPX &&
              kR6_ClockY + kR6_ClockDigitH + kR6_ClockMeltMax <=
                  kR6_GlassY + kR6_GlassH - kRoomPX,
              "room 5 melting clock must stay inside the wet pane");

// ==[ IMU PARALLAX ENVELOPES ]== structural/station silhouettes never tear
// off-screen at either attitude extreme. Full-bleed walls and window plates
// use clipped internal sampling and therefore do not move their outer frame.
static_assert(roomObjectFitsShifted(kR1_DeskX, kR1_DeskY, kR1_DeskW, kR1_DeskH,
                                    kParallaxMidMin, kParallaxMidMax),
              "room 0 desk parallax must stay inside the playfield");
static_assert(roomObjectFitsShifted(kR1_SrvX, kR1_SrvY, kR1_SrvW, kR1_SrvH,
                                    kParallaxMidMin, kParallaxMidMax),
              "room 0 server parallax must stay inside the playfield");
static_assert(roomObjectFitsShifted(kR2_SofaX, kR2_SofaY, kR2_SofaW, kR2_SofaH,
                                    kParallaxMidMin, kParallaxMidMax),
              "room 1 sofa parallax must stay inside the playfield");
static_assert(roomObjectFitsShifted(kR2_WindowX, kR2_WindowY, kR2_WindowW, kR2_WindowH,
                                    kParallaxFarMin, kParallaxFarMax),
              "room 1 window parallax must stay inside the playfield");
static_assert(roomObjectFitsShifted(kR3_CounterX, kR3_CounterY, kR3_CounterW, kR3_CounterH,
                                    kParallaxMidMin, kParallaxMidMax),
              "room 2 counter parallax must stay inside the playfield");
static_assert(roomObjectFitsShifted(kR3_PodX, kR3_PodY, kR3_PodW, kR3_PodH,
                                    kParallaxMidMin, kParallaxMidMax),
              "room 2 pod parallax must stay inside the playfield");
static_assert(roomObjectFitsShifted(kR4_AntennaX, kR4_AntennaY, kR4_AntennaW, kR4_AntennaH,
                                    kParallaxMidMin, kParallaxMidMax),
              "room 3 antenna parallax must stay inside the playfield");
static_assert(roomObjectFitsShifted(kR4_LedgeSeatX, kFloorY - 30,
                                    kR4_LedgeSeatW, 30,
                                    kParallaxNearMin, kRooftopNearMax),
              "room 3 ledge parallax must stay inside the playfield");
static_assert(roomObjectFitsShifted(kR5_TermX, kR5_TermY, kR5_TermW, kR5_TermH,
                                    kParallaxMidMin, kParallaxMidMax),
              "room 4 terminal parallax must stay inside the playfield");
static_assert(roomObjectFitsShifted(kR5_BarX, kR5_BarTopY, kR5_BarW,
                                    kFloorY - kR5_BarTopY,
                                    kParallaxMidMin, kParallaxMidMax),
              "room 4 counter parallax must stay inside the playfield");
static_assert(roomObjectFitsShifted(kR5_NeonX, kR5_NeonY, kR5_NeonW, kR5_NeonH,
                                    kParallaxFarMin, kParallaxFarMax),
              "room 4 sign parallax must stay inside the playfield");
static_assert(roomObjectFitsShifted(kR6_TubX, kR6_TubY, kR6_TubW, kR6_TubH,
                                    kParallaxMidMin, kParallaxMidMax),
              "room 5 tub parallax must stay inside the playfield");
// The bathing pig holds his own plane while the tub travels. The water band is
// 160px against a 72px body, so prove it still swallows him at both extremes —
// otherwise a mid step strips a column off one flank and he surfaces sideways.
static_assert(kR6_TubX + 12 + kParallaxMidMin >= 0 &&
              kR6_TubX + 12 + kParallaxMidMax <= kR6_BathPigX &&
              kR6_BathPigX + kPigW <=
                  kR6_TubX + kR6_TubW - 12 + kParallaxMidMin,
              "bath water must cover the bathing pig at every mid pose");
static_assert(roomObjectFits(kR3_RfRootX - kR3_RfTreeHalfW,
                             kR3_RfRootY - kR3_RfTreeH - kRfTreeCanopyPad,
                             kR3_RfTreeHalfW * 2,
                             kR3_RfTreeH + kRfTreeCanopyPad + 12),
              "ramen RF tree envelope must stay rooted inside the playfield");
static_assert(roomObjectFits(kR6_RfRootX - kR6_RfTreeHalfW,
                             kR6_RfRootY - kR6_RfTreeH - kRfTreeCanopyPad,
                             kR6_RfTreeHalfW * 2,
                             kR6_RfTreeH + kRfTreeCanopyPad + 12),
              "bath RF tree envelope must stay rooted inside the playfield");

static_assert(kR1_PigX + kPigW / 2 == kR1_DeskX + kR1_DeskW / 2,
              "laptop pig must stay centered on desk");
static_assert(kR1_PigY + kPigH - kRearSeatOverlap == kR1_DeskY,
              "laptop rump must meet chair support without a waist cut");
static_assert(kR2_SofaPigX + kPigW / 2 == kR2_SofaX + kR2_SofaW / 2,
              "sofa pig must stay centered on cushion");
static_assert(kR2_WindowPigX + kPigW / 2 == kR2_WindowX + kR2_WindowW / 2,
              "window pig must stay centered on glass");
static_assert(kR2_WindowPigY + kPigH == kFloorY,
              "window rear silhouette must end on floor support");
static_assert(kR3_CookPigX + kPigW / 2 == kR3_CounterX + kR3_CounterW / 2,
              "cooking pig must stay centered on counter");
static_assert(kR3_BedPigX + kPigW / 2 == kR3_PodX + kR3_PodW / 2,
              "sleeping pig must stay centered in pod");
static_assert(kR4_LedgePigX + kPigW / 2 == kR4_LedgeSeatX + kR4_LedgeSeatW / 2,
              "ledge pig must stay centered in rail bay");
static_assert(kR4_AntennaPigY + kPigH == kFloorY,
              "antenna rear silhouette must end on roof support");
static_assert(kR5_TermPigX + kPigW / 2 == kR5_TermX + kR5_TermW / 2,
              "terminal pig must stay centered on CRT");
static_assert(kR5_BoothPigX + kPigW / 2 == kR5_BoothX + kR5_BoothW / 2,
              "booth pig must stay centered on seat");
static_assert(kR6_BathPigX + kPigW / 2 == kR6_TubX + kR6_TubW / 2,
              "bath pig must stay centered in tub");

// Station coordinates are FSM origins. Front poses render one 2px cell above
// that origin, so prove the visible 72x42 body instead of only the base point.
static constexpr bool stationPigFitsRoom(int x, int y, bool frontPose) {
    return (x % kPigPX) == 0 && (y % kPigPX) == 0 &&
           x >= 0 && x + kPigW <= kScreenWidth &&
           y + (frontPose ? kFrontPigAlignY : 0) >= kRoomY &&
           y + (frontPose ? kFrontPigAlignY : 0) + kPigH <= kFloorY;
}

static_assert(stationPigFitsRoom(kR1_PigX, kR1_PigY, false),
              "laptop pig must stay inside the room");
static_assert(stationPigFitsRoom(kR2_SofaPigX, kR2_SofaPigY, true),
              "sofa pig must stay inside the room");
static_assert(stationPigFitsRoom(kR2_WindowPigX, kR2_WindowPigY, false),
              "window pig must stay inside the room");
static_assert(stationPigFitsRoom(kR3_CookPigX, kR3_CookPigY, true),
              "cooking pig must stay inside the room");
static_assert(stationPigFitsRoom(kR3_BedPigX, kR3_BedPigY, true),
              "sleeping pig must stay inside the room");
static_assert(stationPigFitsRoom(kR4_AntennaPigX, kR4_AntennaPigY, false),
              "antenna pig must stay inside the room");
static_assert(stationPigFitsRoom(kR4_LedgePigX, kR4_LedgePigY, true),
              "ledge pig must stay inside the room");
static_assert(stationPigFitsRoom(kR5_TermPigX, kR5_TermPigY, false),
              "terminal pig must stay inside the room");
static_assert(stationPigFitsRoom(kR5_BoothPigX, kR5_BoothPigY, true),
              "booth pig must stay inside the room");
static_assert(stationPigFitsRoom(kR6_BathPigX, kR6_BathPigY, true),
              "bath pig must stay inside the room");
static_assert(stationPigFitsRoom(kR6_BathPigX,
                                kR6_BathPigY + kR6_BathDiveMaxY, true),
              "submerged bath pig must stay inside the room");
static_assert(kR6_BathPigY + kFrontPigAlignY + kR6_BathDiveMaxY +
                  kR6_BathDirectNostrilInsetY == kR6_TubWaterY,
              "bath dive must land the direct-face nostrils on the waterline");
static_assert(stationPigFitsRoom(kR6_BathApproachLeftX, kFloorPigY, true),
              "left bath jump approach must stay inside the room");
static_assert(stationPigFitsRoom(kR6_BathApproachRightX, kFloorPigY, true),
              "right bath jump approach must stay inside the room");
static_assert(kR6_BathApproachLeftX < kR6_BathPigX &&
              kR6_BathApproachRightX > kR6_BathPigX,
              "bath jump approaches must bracket the water anchor");
}

// ==[ FEEDING ]== src/ui/feeding_menu.cpp
namespace Feeding {
static constexpr int kColLabel = 12;
static constexpr int kValueRightX = 300;
static constexpr int kRowH = 22;
static constexpr int kVisibleRows = 8;
}

// ==[ DISPLAY OVERLAY ]== src/ui/display.cpp
namespace Overlay {
static constexpr int kHuntOverlaySsidMax = 16;
}

// ==[ DEFHOG4 DEFENSE TERMINAL ]== src/ui/defhog_terminal.cpp
namespace DefhogLayout {
// Floating terminal window — margins from bars and right edge
static constexpr int kTermX = 106;
static constexpr int kTermY = kTopBarH + 10;                        // 24
static constexpr int kTermW = kScreenWidth - kTermX - 6;            // 208
static constexpr int kTermH = kScreenHeight - kBottomBarH - kTermY - 12; // 190
static constexpr int kTermBorder = 1;

// Inner padding — breathing room between border and text
static constexpr int kTermPadL = 4;                                  // left
static constexpr int kTermPadR = 4;                                  // right
static constexpr int kTermPadT = 3;                                  // top
static constexpr int kTermPadB = 3;                                  // bottom
static constexpr int kTermTextX = kTermX + kTermBorder + kTermPadL;  // 111
static constexpr int kTermTextW = kTermW - 2 * kTermBorder - kTermPadL - kTermPadR; // 198

// Header: "DEFHOG4" inverted size-1 with rotating roasts, then separator
static constexpr int kHeaderH = kCharHSize1 + 3;                    // 11 (8px text + 3px gap+sep)
static constexpr int kHeaderTextY = kTermY + kTermBorder + kTermPadT; // 28
static constexpr int kSepY = kHeaderTextY + kCharHSize1 + 1;        // 37

// Text area below header
static constexpr int kTextAreaY = kSepY + 2;                        // 39
static constexpr int kTextAreaH = kTermY + kTermH - kTermBorder - kTermPadB - kTextAreaY; // 171
static constexpr int kLineH = 9;                                    // 8px text + 1px gap

// Text budgets — full width, no panel
static constexpr int kTermChars = maxCharsSize1(kTermTextW);        // 33
static constexpr int kTermVisibleLines = kTextAreaH / kLineH;       // 19

// Logo centered during boot (text size 2)
static constexpr int kLogoCenterY = kTermY + kTermH / 2 - kCharHSize2; // centered vertically

// Animation timings — particle materialize
static constexpr uint32_t kMaterializeMs = 2000;      // total materialize
static constexpr uint32_t kMatScatterEnd = 300;        // phase 1: scatter particles
static constexpr uint32_t kMatConvergeEnd = 900;       // phase 2: converge to frame
static constexpr uint32_t kMatLogoEnd = 1400;          // phase 3: logo splash
static constexpr uint32_t kMatBootEnd = 2000;          // phase 4: boot lines
static constexpr uint32_t kDissolveMs = 600;

// Line reveal timing
static constexpr uint32_t kBootLineDelayMs = 100;     // delay between boot lines

}

// ==[ TOKYO NIGHT PALETTE ]== opencode tokyonight dark, RGB565
// Decoupled from hamlet accent system — fixed palette for DEFHOG4.
namespace TokyoNight {
static constexpr uint16_t BG        = 0x10C2;  // #1A1B26 — background (dark navy)
static constexpr uint16_t BG_PANEL  = 0x1904;  // #1E2030 — panel bg (header, footer, status bar)
static constexpr uint16_t FG        = 0xAD79;  // #C8D3F5 — text (bright blue-white)
static constexpr uint16_t DIM       = 0x8472;  // #828BB8 — muted text (lavender-gray)
static constexpr uint16_t PRIMARY   = 0x835F;  // #82AAFF — primary (bright blue)
static constexpr uint16_t ACCENT    = 0xFC97;  // #FF966C — accent/warning (warm orange)
static constexpr uint16_t ERROR     = 0xFC15;  // #FF757F — error (salmon red)
static constexpr uint16_t SUCCESS   = 0xB78D;  // #C3E88D — success (green)
static constexpr uint16_t BORDER    = 0x528E;  // #545C7E — border subtle
}

// ==[ TOAST OVERLAY ]== unified Y for help/alert toasts
namespace ToastLayout {
static constexpr int kToastY = kTopBarH + 10;  // 24 — aligned with DEFHOG4 terminal top
}

// ==[ SPECTRUM ]== src/modes/spectrum.cpp
namespace Spectrum {
static constexpr int kGraphLeft = 26;
static constexpr int kGraphRight = 316;                       // exclusive
static constexpr int kGraphTop = kTopBarH;                   // 14
static constexpr int kGraphBottom = kTopBarH + 80;           // 94
static constexpr int kGraphWidth = kGraphRight - kGraphLeft; // 290
static constexpr int kGraphHeaderH = 10;
static constexpr int kGraphPlotTop = kGraphTop + kGraphHeaderH; // 24
// Vertical layout (pixel-perfect):
//   Y=14-23:  Owned graph caption lane
//   Y=24-94:  Spectrum evidence plot
//   Y=95:     Waterfall separator line
//   Y=96-117: Waterfall / C5 observer strip (22 rows)
//   Y=120-127: Channel labels
//   Y=130-204: Network list (5 rows × 15px = 75px)
//   Y=205-212: Attack/action hint
//   Y=226-239: Bottom bar
static constexpr int kChannelLabelY = kGraphBottom + 26;
static constexpr int kNetworkListY = kGraphBottom + 36;      // 130
static constexpr int kNetworkRowH = 15;
static constexpr int kNetworkVisibleRows = 5;
static_assert(kGraphPlotTop < kGraphBottom,
              "Spectrum caption lane must leave a visible evidence plot");
static_assert(kNetworkListY + kNetworkRowH * kNetworkVisibleRows + kCharHSize1
                  <= kScreenHeight - kBottomBarH,
              "Spectrum list and hint must stay above the bottom bar");

// Network row columns — intelligence layout v4 (320px, pixel-exact)
// Each column has ≥4px gap to next. Font: 6px/char at textSize 1.
//
// Element          X start  Width   X end   Gap
// ───────────────────────────────────────────────
// Readiness bar    0        3px     2       2px
// Tier icon        5        6px     10      4px
// SSID (24ch)      14       144px   157     4px
// Sparkline/5G CH  162      30px    191     4px
// Client/5G age    196      24px    219     4px
// PHY+flags        224      30px    253     4px
// RSSI dBm         right-aligned at 312 (TR_DATUM, all rows)
// Scroll arrows    314      6px     319
//
static constexpr int kReadinessBarW = 3;
static constexpr int kTierIconX = 5;
static constexpr int kSsidX = 14;
static constexpr int kSsidMax = 24;        // 24ch × 6px = 144px, ends X=157
static constexpr int kSparklineX = 162;
static constexpr int kSparklineW = 30;     // ends X=191
static constexpr int kClientCountX = 196;  // "C:NN" 4ch=24px, ends X=219
static constexpr int kInfoX = 224;         // "ac!R#" 5ch=30px, ends X=253
static constexpr int kRssiRightX = 312;    // dBm right-aligned, all rows
// Client monitor header cap
static constexpr int kClientHeaderSsidMax = 28;
}

}  // namespace UIMeasurements
