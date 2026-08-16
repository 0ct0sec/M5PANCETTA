/** room0_lab.cpp — Cyberdeck Lab (dual monitors, server cluster, neon sign) */

#include "../menu_pig_internal.h"
#include "../pixel_materials.h"
#include "../pixel_primitives.h"
#include "../pixel_furniture.h"
#include "../pixel_lighting.h"
#include "../pixel_weather.h"

namespace MenuPig {

using namespace PixelMat;
using namespace PixelPrim;
using namespace PixelFurn;
using namespace PixelLight;
using namespace PixelWeather;
using namespace MenuPigRender;
using namespace UIMeasurements::MenuPigLayout;

static constexpr uint32_t kLabMonitorCondensationSalt = 0xB011u;
static constexpr uint32_t kLabServerCondensationSalt = 0xB012u;
static constexpr int kLabMonitorCondensationCount = 2;
static constexpr int kLabServerCondensationCount = 2;
static constexpr int kTankX = SCREEN_WIDTH / 2 - 24;
static constexpr int kTankY = kRoomY + 20;
static constexpr int kTankW = 48;
static constexpr int kTankH = 52;

bool isRoom0SignLit(uint32_t now) {
    return isNeonOn(now);
}

// ==[ ROOM 0 MAIN DRAW ]== (external linkage — declared in menu_pig_internal.h)

void drawRoom0(M5Canvas& canvas, uint32_t now, RoomRenderPass pass) {
    int farDx = parallaxFar;
    int midDx = parallaxMid;
    const bool labSignLit = isRoom0SignLit(now);

    if (pass == RoomRenderPass::BASE) {
        // Retained L0/L1/L2: materials and architecture never depend on time.
        // Variant 0. Rooms 0, 1 and 2 pass this exact rect; without distinct
        // seeds the three of them render one identical wall and one identical
        // floor, which is most of why they read as the same set redressed.
        drawConcreteWall4(canvas, 4, kRoomY + 4,
                          SCREEN_WIDTH - 8, kFloorY - kRoomY - 8, 0u);
        drawMetalFloor4(canvas, 0u);
        // The retained pass is re-baked per parallax step (the room cache key
        // mixes all three offsets), so the ambient has to ride the sign the
        // same way every other fixture in this pass does. Without farDx the
        // bounce stays nailed to the un-parallaxed plate and slides up to 4px
        // off the tube it is supposed to come from.
        PigLight rl;
        rl.x = (int16_t)(kR1_NeonX + farDx + kR1_NeonW / 2);
        rl.y = (int16_t)(kR1_NeonY + kR1_NeonH / 2);
        rl.tint = RP::NEON;
        drawNeonWash4(canvas, PigLightEval(rl, 160.0f, 0.08f),
                      160.0f, 0.08f, 0x4C01,
                      4, kRoomY + 4, SCREEN_WIDTH - 8,
                      kFloorY - kRoomY - 4);

        drawPipeRun4(canvas, kR1_SrvX + farDx - 20, kRoomY + 8,
                     roomW(50), true, false);
        drawFuseBox4(canvas, kR1_SrvX + farDx + 4, kRoomY + 12);
        drawPatchPanel4(canvas, kR1_SrvX + farDx - 12, kRoomY + 20);
        drawWallOutlet4(canvas, kR1_SrvX + farDx + 30, kRoomY + 36);
        drawCableCoil4(canvas, kR1_SrvX + farDx + 10, kFloorY - 4);
        drawCeilingStain4(canvas, kR1_SrvX + farDx + 20, kRoomY + 6);
        drawSmallVent4(canvas, kR1_SrvX + farDx + 40, kRoomY + 4);
        drawSprinkler4(canvas, kR1_SrvX + farDx + 60, kRoomY);
        drawSprinkler4(canvas, kR1_SrvX + farDx + 120, kRoomY);
        drawConduitRun4(canvas, kR1_SrvX + farDx + 40,
                        kRoomY + 10, roomW(40));
        drawConduitRun4(canvas, kR1_SrvX + farDx - 30,
                        kRoomY + 4, roomW(30));
        drawSmallVent4(canvas, kR1_SrvX + farDx - 10, kRoomY + 4);
        drawFloorDrain4(canvas, kR1_SrvX + farDx + 20, kFloorY - 4);

        // drawPipeRun4 is not just the run: it weeps an 8px rust tail off the
        // joint at y+4, so the fixture is 12 deep, not 4. Cut for 8 the bottom
        // contour landed at y+8 — inside the tail — and punched BG through it.
        drawPopOutline1px(canvas, kR1_SrvX + farDx - 20, kRoomY + 8,
                          roomW(50), 12, PopOutlineStyle::MIXED, 0xA041u);
        // Outline tracks the fixture footprint. The fuse box grew to 16x20
        // when its 2px breakers became real cells; an outline still cut for
        // 14x16 draws its right and bottom edges INSIDE the enclosure and
        // punches BG holes through the door.
        drawPopOutline1px(canvas, kR1_SrvX + farDx + 4, kRoomY + 12,
                          16, 20, PopOutlineStyle::SOLID, 0xA051u);
        drawPopOutline1px(canvas, kR1_SrvX + farDx - 12, kRoomY + 20,
                          16, 16, PopOutlineStyle::MIXED, 0xA061u);
        // The arrow's widest row is its 12px base course, not the 8px shaft.
        // An 8x8 contour cut its right column and bottom row straight through
        // the glyph — the one fixture in this room that is pure emissive neon.
        drawPopOutline1px(canvas, kR1_SrvX + farDx + 72, kRoomY + 4,
                          12, 12, PopOutlineStyle::SOLID, 0xA071u);
        return;
    }

    // ==[ ARCHITECTURE ANSWERS THE PRACTICALS ]==
    // The wall's depth grade and its ambient bounce are baked into the
    // retained pass, which is correct for this room's continuous fixtures —
    // server, lamp, monitors all burn without stopping. The sign does not. Its
    // contribution has to be live or the concrete wears a permanent glow from
    // a tube that spends half its cycle dark. Bounded to the fixture's real
    // reach: a 160px wash over the whole wall is a screen-sized shader every
    // frame for a spill that carries a few cells past the sign.
    if (labSignLit) {
        PigLight signKey;
        signKey.x = (int16_t)(kR1_NeonX + farDx + kR1_NeonW / 2);
        signKey.y = (int16_t)(kR1_NeonY + kR1_NeonH / 2);
        signKey.tint = RP::NEON;
        drawNeonWash4(canvas, PigLightEval(signKey, 88.0f, 0.26f),
                      88.0f, 0.26f, 0x4C02u,
                      kR1_NeonX + farDx - 56, kRoomY + 4,
                      kR1_NeonW + 112, 96);
    }

    // Live L2-L4: bounded atmosphere, animated faces, and attached light.
    drawDustMotes4(canvas, now);
    drawNeonArrow4(canvas, now,
                   kR1_SrvX + farDx + 72, kRoomY + 4);

    // ---- LAYER 3: FURNITURE & STATIONS (mid parallax) ----
    // Neon sign "SYS" (far parallax, upper-right)
    drawNeonSign_SYS4(canvas, kR1_NeonX + farDx, kR1_NeonY, now);
    canvas.fillRect(kR1_NeonX + farDx + 4, kR1_NeonY - 4, kR1_NeonW - 8, kRoomPX, RP::D_STRUCT);

    // Cyberdolphin aquarium (wall cutout, far parallax)
    drawCyberdolphinAquarium4(canvas, kTankX + farDx, kTankY, kTankW, kTankH, now);

    // Server cluster (mid parallax)
    drawServerRack4(canvas, kR1_SrvX + midDx, kR1_SrvY, now);

    // Desk hardware is scenery on the 4px room grid. Pancetta's arrival
    // wobble is a 2px body animation; feeding that offset through q4() made
    // negative phases jump a full cell while positive phases stayed put.
    drawDualMonitorDesk4(canvas, kR1_DeskX, kR1_DeskY, laptopScreenOn, now);
    drawCableCoil4(canvas, kR1_DeskX + kR1_DeskW + 8, kFloorY - 6);

    // ---- LAYER 4: CAST LIGHT (reveals objects on darkened scene) ----
    // Monitor CRT glow
    if (laptopScreenOn) {
        drawLightPool4(canvas, RP::CRT, kR1_MonAX - 2, kR1_MonAY - 2, kR1_MonAW + 4, 4, 36, 70070);
        drawLightPool4(canvas, RP::CRT, kR1_MonBX - 2, kR1_MonBY - 2, kR1_MonBW + 4, 4, 36, 70071);
        drawLightPool4(canvas, RP::CRT, kR1_DeskX + 4, kR1_DeskY - 4, kR1_DeskW - 8, 6, 28, 80080);
        drawLightPool4(canvas, RP::CRT, kR1_KeybX, kR1_KeybY, kR1_KeybW, kR1_KeybH, 18, 0x6C01);
    }

    // Neon sign cast pool + volumetric dust beam
    if (labSignLit) {
        drawLightPool4(canvas, RP::NEON, kR1_NeonX + farDx + 2, kR1_NeonY + kR1_NeonH + 2, kR1_NeonW - 4, 4, 40, 99077);
        drawDustBeam4(canvas, now,
                      kR1_NeonX + farDx + kR1_NeonW / 2, kR1_NeonY + kR1_NeonH + 2, kR1_NeonW - 4,
                      kR1_NeonX + farDx + kR1_NeonW / 2 + 4, kFloorY - 2, kR1_NeonW + 16,
                      RP::NEON, RP::DUST, 0xA233u);
        drawLightPool4(canvas, RP::NEON, roomX(106) + farDx, roomY(30), 12, 4, 20, 0xAA04);
    }

    // Server green floor glow
    drawLightPool4(canvas, RP::GREEN_DK, kR1_SrvX + midDx + 4, kFloorY - 6, kR1_SrvW - 8, 6, 22, 77119);
    // Aquarium glow
    drawLightPool4(canvas, RP::CRT, kTankX + farDx + 8, kTankY + kTankH, kTankW - 16, 4, 18, 0xD01Fu);
    // Desk lamp warm pool
    drawLightPool4(canvas, RP::WARM, kR1_LampX - 4, kR1_LampY + 8, 24, 24, 30, 0x1A77);

    // ---- LAYER 5: POST-CAST EMISSIVE EFFECTS ----
    drawCondensation4(canvas, now, kR1_MonBX, kR1_MonBY, kR1_MonBW, kR1_MonBH,
                      kLabMonitorCondensationCount, RP::CRT, kLabMonitorCondensationSalt);
    drawCondensation4(canvas, now + 800, kR1_SrvX + midDx + 4, kR1_SrvY + 8,
                      kR1_SrvW - 8, kR1_SrvH - 16,
                      kLabServerCondensationCount, RP::CRT, kLabServerCondensationSalt);
}

// ==[ ROOM 0 KEY LIGHT ]== nearest practical owns every pig light layer
PigLight selectRoom0PigKeyLight(int pigDrawX, int pigDrawY, uint32_t now) {
    PigLight crtLight;
    crtLight.x = (int16_t)(kR1_MonAX + (kR1_MonBX + kR1_MonBW - kR1_MonAX) / 2);
    crtLight.y = (int16_t)(kR1_MonAY + kR1_MonAH / 2);
    crtLight.tint = laptopScreenOn ? RP::CRT : (uint16_t)0;

    PigLight neonLight;
    neonLight.x = (int16_t)(kR1_NeonX + parallaxFar + kR1_NeonW / 2);
    neonLight.y = (int16_t)(kR1_NeonY + kR1_NeonH / 2);
    neonLight.tint = isRoom0SignLit(now) ? RP::NEON : (uint16_t)0;

    PigLight serverLight;
    serverLight.x = (int16_t)(kR1_SrvX + parallaxMid + kR1_SrvW / 2);
    serverLight.y = (int16_t)(kR1_SrvY + kR1_SrvH / 2);
    serverLight.tint = RP::GREEN_DK;

    PigLight lampLight;
    lampLight.x = (int16_t)(kR1_LampX + 6);
    lampLight.y = (int16_t)(kR1_LampY + 8);
    lampLight.tint = RP::WARM;

    float pigCX = (float)(pigDrawX + kPigW / 2);
    float pigCY = (float)(pigDrawY + kPigH / 2);
    PigLight key = neonLight;
    float keyS = 0.0f;
    const struct { PigLight l; float r, w; } emitters[] = {
        {crtLight, 130.0f, 0.8f}, {neonLight, 160.0f, 1.0f},
        {serverLight, 100.0f, 0.5f}, {lampLight, 110.0f, 0.7f}
    };
    for (const auto& e : emitters) {
        float s = room2EmitterScoreAt(e.l, pigCX, pigCY, e.r, e.w);
        if (s > keyS) { keyS = s; key = e.l; }
    }
    if (key.tint == 0) key = getRoomNeonLight(0, now);
    return key;
}

// ==[ NOIR ON PIG: Room 0 Lab ]== directional half-Lambert volume shading
static void applyRoom0NoirToPig(M5Canvas& canvas, uint32_t) {
    static constexpr MenuPig::PigNoirProfile kProfile = {115, 210, 38, 90, 85};
    applyDirectionalPigNoir(canvas, kProfile);
}

static void drawRoom0ForegroundDeck(M5Canvas& canvas, uint32_t now) {
    // Foreground deck (near parallax)
    int nearDx = parallaxNear;
    int fx = q4(208 + nearDx);
    if (fx < 0) fx = 0;
    if (fx > SCREEN_WIDTH - 12) return;

    int fy = kFloorY - 8;
    int fw = SCREEN_WIDTH - fx;
    canvas.fillRect(fx, fy, fw, 12, RP::DEEP);
    canvas.fillRect(fx, fy, fw, kRoomPX, RP::D_STRUCT);

    // The grating is the only thing on this deck that was cut half a cell off
    // the lattice: fy+2 put an 8px slat at 212..219 on a deck that runs
    // 210..221, so every slat carried a 2px sliver above it and left the deck's
    // bottom two rows bare. fy+4 seats it one whole cell down and lands it
    // flush on the deck's bottom edge, which is also where the wear cell
    // already sat.
    for (int gx = fx + 4; gx < SCREEN_WIDTH; gx += 16) {
        canvas.fillRect(gx, fy + 4, kRoomPX, 8, RP::D_STRUCT);
        if ((wallHash(gx, fy, 0x7161u) & 0xFFu) < 120u)
            canvas.fillRect(gx, fy + 4, kRoomPX, kRoomPX, RP::FILL);
    }

    int boxX = q4(fx + fw - 28);
    canvas.fillRect(boxX, fy - 4, 24, 8, RP::D_STRUCT);
    canvas.fillRect(boxX + 4, fy - 4, 20, 4, RP::FILL);
    if (((now / 420u) & 1u) != 0)
        canvas.fillRect(boxX + 12, fy, kRoomPX, kRoomPX, RP::LED);

    // kFloorY + 8 is the room's exclusive bottom — drawMetalFloor4's depth band
    // stops there and drawRoomPopPixel rejects anything at or past it. The
    // inclusive bound ran one cell further and hung a deck-wide SHADOW_C bar at
    // 226..229, below the last floor row, in the black gutter under the room.
    for (int gy = fy + 4; gy < kFloorY + 8; gy += kRoomPX) {
        if ((wallHash(fx, gy, 0x7171u) & 0xFFu) < 168u)
            canvas.fillRect(fx, gy, fw, kRoomPX, RP::SHADOW_C);
    }
}

void drawRoom0Foreground(M5Canvas& canvas, uint32_t now) {
    // ==[ NOIR ON PIG ]== must come before foreground furniture
    applyRoom0NoirToPig(canvas, now);

    if (isStationVisualActive(Station::AT_LAPTOP)) {
        canvas.fillRect(kR1_DeskX, kR1_DeskY, kR1_DeskW, kRoomPX, RP::D_STRUCT);
        canvas.fillRect(kR1_DeskX, kR1_DeskY + kRoomPX, kR1_DeskW, kRoomPX, RP::FILL);
    }

    drawRoom0ForegroundDeck(canvas, now);

    // Hanging cable loop from ceiling pipe — crosses pig walk path
    int nearDx = parallaxNear;
    int cableX = q4(80 + nearDx);
    // kRoomY + 10 is 2px off the room lattice (kRoomY ≡ 2 mod 4), and it started
    // the run 10px below the ceiling with nothing overhead — the strand read as
    // beginning in mid-air rather than dropping out of the dark. kRoomY + 4 is
    // the wall's own top course, so it enters from the ceiling and the drop is a
    // whole number of cells.
    int cableTop = kRoomY + 4;
    int cableMid = kRoomY + 80;
    canvas.fillRect(cableX, cableTop, kRoomPX, cableMid - cableTop, RP::WALL_MID);
    canvas.fillRect(cableX, cableMid, 8, kRoomPX, RP::WALL_MID);
    canvas.fillRect(cableX + 8, cableMid + 4, kRoomPX, 12, RP::WALL_MID);
    canvas.fillRect(cableX, cableMid + 16, 8, kRoomPX, RP::WALL_MID);
    canvas.fillRect(cableX, cableMid + 16, kRoomPX, 20, RP::WALL_MID);
    canvas.fillRect(cableX, cableMid, kRoomPX, kRoomPX, RP::D_STRUCT);
}

} // namespace MenuPig
