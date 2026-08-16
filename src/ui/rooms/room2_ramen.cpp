/** room2_ramen.cpp — Ramen Bar (counter, noodle bowl, coffin pod, neon sign) */

#include "../menu_pig_internal.h"
#include "../pixel_materials.h"
#include "../pixel_furniture.h"
#include "../pixel_lighting.h"
#include "../pixel_weather.h"
#include "../smoke_volume.h"

namespace MenuPig {

using namespace PixelMat;
using namespace PixelFurn;
using namespace PixelLight;
using namespace PixelWeather;

static constexpr int kRamenLanternX = 84;
static constexpr int kRamenLanternY = 76;
static constexpr int kRamenRunnerX = 80;
static constexpr int kRamenRunnerY = (kFloorY - 2 * kRoomPX) & ~3;
static constexpr int kRamenRunnerW = 72;
static constexpr int kRamenTipJarX = 144;
static constexpr int kRamenTipJarY = 108;
static constexpr int kRamenCounterGlowLift = 3 * kRoomPX;
static constexpr int kRamenCounterGlowH = 4 * kRoomPX;

static_assert(kRamenLanternX % kRoomPX == 0 &&
              kRamenLanternY % kRoomPX == 0 &&
              kRamenRunnerX % kRoomPX == 0 &&
              kRamenRunnerY % kRoomPX == 0 &&
              kRamenRunnerW % kRoomPX == 0 &&
              kRamenTipJarX % kRoomPX == 0 &&
              kRamenTipJarY % kRoomPX == 0,
              "ramen detail anchors must stay on the room grid");
static_assert(roomObjectFits(kRamenLanternX, 16, 24, 96),
              "ramen lantern must stay inside the playfield");
static_assert(roomObjectFits(kRamenRunnerX, kRamenRunnerY,
                             kRamenRunnerW, 2 * kRoomPX),
              "ramen runner must stay inside the playfield");
static_assert(roomObjectFits(kRamenTipJarX - 8, kRamenTipJarY,
                             36, 5 * kRoomPX),
              "ramen tip shelf must stay inside the playfield");
static_assert(kRamenCounterGlowH > kRamenCounterGlowLift,
              "ramen warm island must reach the counter top");

static void drawRamenCozyDetails(M5Canvas& canvas, uint32_t now) {
    drawPaperLantern4(canvas, kRamenLanternX, kRamenLanternY, now);
    canvas.fillRect(kRamenRunnerX, kRamenRunnerY,
                    kRamenRunnerW, 2 * kRoomPX, RP::D_WARM);
    for (int x = kRamenRunnerX + kRoomPX;
         x < kRamenRunnerX + kRamenRunnerW; x += 3 * kRoomPX) {
        canvas.fillRect(x, kRamenRunnerY, kRoomPX,
                        2 * kRoomPX, RP::D_FILL);
    }
}

static void drawRamenTipJar(M5Canvas& canvas) {
    // A small wall shelf keeps the jar clear of the bowl/sake/kettle cluster.
    canvas.fillRect(kRamenTipJarX - 8, kRamenTipJarY + 16,
                    36, kRoomPX, RP::D_STRUCT);
    canvas.fillRect(kRamenTipJarX + 4, kRamenTipJarY,
                    12, kRoomPX, RP::D_STRUCT);
    canvas.fillRect(kRamenTipJarX, kRamenTipJarY + kRoomPX,
                    20, 12, RP::D_WALL_NEAR);
    canvas.fillRect(kRamenTipJarX + 4, kRamenTipJarY + 8,
                    12, kRoomPX, RP::WARM);
    canvas.fillRect(kRamenTipJarX + 8, kRamenTipJarY + 12,
                    kRoomPX, kRoomPX, RP::SHAFT);
}

void drawRamenVapor(M5Canvas& canvas, uint32_t now, int baseX, int baseY) {
    static constexpr uint32_t VAPOR_CYCLE = 3200;
    static constexpr int VAPOR_RISE = 32;
    for (int i = 0; i < 5; i++) {
        uint32_t phase = (now + (uint32_t)i * (VAPOR_CYCLE / 5)) % VAPOR_CYCLE;
        float t = (float)phase / (float)VAPOR_CYCLE;
        int rise = (int)(t * (float)VAPOR_RISE);
        float wave = fastSinf(t * 6.28318f + (float)i * 1.5f) * (3.0f + (float)(i & 1) * 2.0f);
        int sx = (baseX + (int)wave + (i - 2) * 4) & ~3;
        int sy = (baseY - rise) & ~3;
        if (sy <= (int)UIMeasurements::kTopBarH + 2 || sy >= kFloorY) continue;
        float fade = (t < 0.6f) ? 1.0f : (1.0f - (t - 0.6f) / 0.4f);
        uint8_t keep = (uint8_t)(200.0f * fade);
        if ((wallHash(sx, sy, 0xBE11 + i * 53) & 0xFF) < keep) {
            uint16_t col = (t < 0.4f) ? RP::SOFT : RP::DUST;
            canvas.fillRect(sx, sy, kRoomPX, kRoomPX, col);
            if (t < 0.3f && ((i + (int)(now / 200)) & 1) == 0) {
                int sx2 = (sx + ((i & 1) ? kRoomPX : -kRoomPX)) & ~3;
                if (sx2 > 4 && sx2 < SCREEN_WIDTH - 4)
                    canvas.fillRect(sx2, sy, kRoomPX, kRoomPX, RP::SOFT);
            }
        }
    }
}

static void drawRoom2FurnitureLighting(M5Canvas& canvas, const Room2LightingFrame& lighting,
                                       uint32_t now) {
    if (!lighting.valid) return;
    (void)now;
    // The pod LED is the only secondary furniture wash. The sign, window,
    // lantern, and bowl keep their own bounded pools instead of tinting the
    // same counter/stool/pod cells from four directions.
    if (lighting.pod.tint != 0) {
        drawFurnitureWash4(canvas, lighting.podX, lighting.podY, 36, 20,
                           PigLightEval(lighting.pod, 68.0f, 0.18f), 68.0f, 0.18f);
    }
}

void drawRoom2(M5Canvas& canvas, uint32_t now, RoomRenderPass pass) {
    if (pass == RoomRenderPass::BASE) {
        drawConcreteWall4(canvas, 4, kRoomY + 4,
                          SCREEN_WIDTH - 8, kFloorY - kRoomY - 8, 2u);
        drawMetalFloor4(canvas, 2u);
        // Rooms 0, 1 and 2 are the three concrete-and-metal builds, and the
        // distinct wall/floor variants above are only half of what stops them
        // reading as one set redressed — the other half is that each room's
        // architecture carries its own colour bounce. Rooms 0 and 1 both bake
        // one; this room was the last flat grey box in the set, despite owning
        // the largest neon plate of the three. Weak and room-wide: the sign's
        // real, cycling contribution is live and bounded, below.
        PigLight rl;
        rl.x = (int16_t)(kR3_SignX + parallaxFar + kR3_SignW / 2);
        rl.y = (int16_t)(kR3_SignY + kR3_SignH / 2);
        rl.tint = RP::NEON;
        drawNeonWash4(canvas, PigLightEval(rl, 160.0f, 0.08f),
                      160.0f, 0.08f, 0x4C21u,
                      4, kRoomY + 4, SCREEN_WIDTH - 8,
                      kFloorY - kRoomY - 4);
        canvas.fillRect(8 + parallaxFar, kRoomY + 4,
                        100, kRoomPX, RP::WALL_MID);
        canvas.fillRect(40 + parallaxFar, kRoomY + 4,
                        kRoomPX, 8, RP::WALL_MID);
        drawSmallVent4(canvas, 200 + parallaxFar, kRoomY + 4);
        drawConduitRun4(canvas, 240 + parallaxFar, kRoomY + 10, 40);

        drawCityWindowBase(canvas,
                           kR3_WindowX, kR3_WindowY,
                           kR3_WindowW, kR3_WindowH, pigX);
        // One cold service run is enough wall grammar. Leave the counter,
        // lantern, sign, and city pane separated by real negative space.
        drawConduitRun4(canvas, 20 + parallaxFar, kRoomY + 28, 60);
        drawSprinkler4(canvas, 110 + parallaxFar, kRoomY);
        drawCeilingStain4(canvas, 50 + parallaxFar, kRoomY + 8);

        // Same service fittings as rooms 0 and 1, and until now the only ones
        // in the set without a contour — on variant-2 concrete they sat as
        // decals printed on the wall rather than hardware standing off it.
        // Sizes are the primitives' true footprints: 12x12 grille, 12x8 stain
        // (a cell of slack below), and w x 4 conduit (likewise).
        drawPopOutline1px(canvas, 200 + parallaxFar, kRoomY + 4, 12, 12,
                          PopOutlineStyle::SOLID, 0xC001u);
        drawPopOutline1px(canvas, 240 + parallaxFar, kRoomY + 10, 40, 8,
                          PopOutlineStyle::SPARSE, 0xC011u);
        drawPopOutline1px(canvas, 20 + parallaxFar, kRoomY + 28, 60, 8,
                          PopOutlineStyle::SPARSE, 0xC021u);
        drawPopOutline1px(canvas, 50 + parallaxFar, kRoomY + 8, 12, 12,
                          PopOutlineStyle::MIXED, 0xC031u);
        return;
    }

    calcWobble(now);
    int cookWx = (currentStation == Station::COOKING) ? wobbleX : 0;
    int cookWy = (currentStation == Station::COOKING) ? wobbleY : 0;
    int bedWx  = (currentStation == Station::IN_BED) ? wobbleX : 0;
    int bedWy  = (currentStation == Station::IN_BED) ? wobbleY : 0;
    if (debugRoamingFrame.active) {
        cookWx = debugRoamingFrame.cookWx;
        cookWy = debugRoamingFrame.cookWy;
        bedWx = debugRoamingFrame.bedWx;
        bedWy = debugRoamingFrame.bedWy;
    }
    bool useRearView = shouldUseRearViewInRoaming();
    bool bowlHeldByPig = isRamenEatingState(useRearView);
    int bowlFx = kR3_BowlX + cookWx + kRamenDishShiftX;
    int bowlFy = kR3_BowlY + cookWy - kRoomPX + kRamenDishShiftY;
    if (bowlHeldByPig) {
        PigPose pose = resolvePigPose(now, false, useRearView, false);
        getHeldBowlPosition(pose.drawX, pose.drawY, bowlFx, bowlFy);
    }
    Room2LightingFrame lighting = buildRoom2Emitters(cookWx, cookWy, bedWx, bedWy,
                                                     bowlFx, bowlFy, bowlHeldByPig,
                                                     parallaxFar, now);
    room2LightingRuntime = lighting;
    const bool ramenNeonOn = isNeonOn(now);

    // ==[ ARCHITECTURE ANSWERS THE SIGN ]==
    // The retained pass can only carry a fixed ambient, and RAMEN spends a
    // real part of its cycle dark — baking its full contribution would leave
    // the concrete permanently glowing from an unlit tube. So the moving half
    // lives here, bounded to the plate's actual reach and drawn before any
    // furniture so it grades architecture and nothing else. Same contract as
    // room 0's SYS sign; this room simply never had it.
    if (ramenNeonOn) {
        PigLight signKey;
        signKey.x = (int16_t)(lighting.signX + kR3_SignW / 2);
        signKey.y = (int16_t)(lighting.signY + kR3_SignH / 2);
        signKey.tint = RP::NEON;
        drawNeonWash4(canvas, PigLightEval(signKey, 88.0f, 0.26f),
                      88.0f, 0.26f, 0x4C22u,
                      lighting.signX - 56, kRoomY + 4,
                      kR3_SignW + 112, 96);
    }

    // Match the bathroom's source-truth glass stack: generated exterior first,
    // then clipped wet glass and condensation, then the structural frame. The
    // ramen center mullion keeps this a compact service-window silhouette.
    drawCityWindowMotion(canvas, now,
                         kR3_WindowX, kR3_WindowY,
                         kR3_WindowW, kR3_WindowH,
                         pigX, lighting.window.tint);
    // Runoff is behind the counter/pod so those physical objects occlude it.
    drawRoomRain(canvas, now,
                 kR3_WindowX, kR3_WindowY,
                 kR3_WindowW, kR3_WindowH);

    drawRamenCozyDetails(canvas, now);
    drawRamenTipJar(canvas);
    drawRFBonsai4(canvas, kR3_RfRootX, kR3_RfRootY, kR3_RfTreeH, 0xBEEF, now, false);

    drawRamenCounter4(canvas, kR3_CounterX + cookWx, kR3_CounterY + cookWy, kR3_CounterW, kR3_CounterH);
    if (!bowlHeldByPig) {
        drawNoodleBowl4(canvas, bowlFx, bowlFy, kR3_BowlW, kR3_BowlH, false, now);
    }
    const int sakeX = (kR3_CounterX + kRoomPX + cookWx) & ~3;
    const int sakeY = (kR3_CounterY - 3 * kRoomPX + cookWy) & ~3;
    const int kettleX = (kR3_CounterX + kR3_CounterW - 4 * kRoomPX + cookWx) & ~3;
    const int kettleY = sakeY;
    drawSakeBottle4(canvas, sakeX, sakeY, now);
    drawKettle4(canvas, kettleX, kettleY, now, false);

    // Two-part plume, same contract as the cigarette: drawRamenVapor is the thin
    // near-field stem off the broth, and the shared volume system owns
    // everything above it. The column is sized (rise 10 / 1300ms ≈ 57px) to top
    // out at the bottom edge of the RAMEN sign, so the steam crosses the neon
    // and picks up its tint instead of dying in dead air.
    //
    // Emission stops when the pig lifts the bowl; the draw does not. What is
    // already airborne has to be allowed to dissipate, exactly like a cigarette
    // surviving the smoker lowering their hand.
    if (!bowlHeldByPig) {
        drawRamenVapor(canvas, now, bowlFx + kR3_BowlW / 2, bowlFy - 2);

        SmokeFx::VentParams vent;
        vent.x = (bowlFx + kR3_BowlW / 2) & ~3;
        vent.y = (bowlFy - kRoomPX) & ~3;
        vent.intervalMs = 420;
        vent.rise = 10;
        vent.lifeMs = 1300;
        vent.spreadPx = (uint8_t)(kR3_BowlW / 4);
        vent.scale = 62;        // broth steam is thinner than a lungful
        vent.opacity = 118;
        vent.seed = 0xB0F1u;
        SmokeFx::driveVent(SmokeFx::Source::Vent0, now, vent);
    }

    SmokeFx::Lighting lit;
    if (lighting.valid) {
        lit.add(lighting.sign, 132.0f, 140);
        lit.add(lighting.bowl, 46.0f, 120);
    }
    SmokeFx::draw(canvas, SmokeFx::Source::Vent0, RP::SOFT, RP::DUST, &lit);

    drawBarStool4(canvas, kR3_Stool1X, kR3_StoolY);
    drawBarStool4(canvas, kR3_Stool2X, kR3_StoolY);

    {
        const int drainX = (kR3_PodX + kR3_PodW + kRoomPX) & ~3;
        const int drainY = (kFloorY - kRoomPX) & ~3;
        drawFloorDrain4(canvas, drainX, drainY);
    }

    bool podOccupied = isStationVisualActive(Station::IN_BED);
    drawCoffinPod4(canvas, lighting.podX, lighting.podY,
                   kR3_PodW, kR3_PodH, podOccupied, now);

    if (ramenNeonOn) {
        drawLightPool4(canvas, RP::NEON,
                       lighting.signX, lighting.signY - kRoomPX,
                       kR3_SignW, kR3_SignH + 2 * kRoomPX,
                       18, 33033u);
    }

    if (podLedOn) {
        drawLightPool4(canvas, RP::LED,
                       lighting.podX + 8, lighting.podY + 4,
                       24, 2 * kRoomPX, 22, 44044u);
    }

    // One warm island binds the lantern, counter, and bowl. Steam remains the
    // only continuous interior motion.
    drawLightPoolGradient4(canvas, RP::WARM,
                           lighting.counterX,
                           lighting.counterY - kRamenCounterGlowLift,
                           kR3_CounterW, kRamenCounterGlowH, 34, 77411u);
    drawLightPoolGradient4(canvas, RP::WARM,
                           kRamenLanternX - 8, kRamenLanternY + 12,
                           40, 20, 28, 0xC02Eu);
    if (!bowlHeldByPig)
        drawLightPool4(canvas, RP::WARM, bowlFx - 4, bowlFy - 4,
                       kR3_BowlW + 8, kRoomPX, 30, 77317u);

    drawRoom2FurnitureLighting(canvas, lighting, now);

    drawNeonSign_RAMEN4(canvas, lighting.signX, lighting.signY, now);
    if (podLedOn) {
        canvas.fillRect(lighting.podX + kR3_PodW - 8, lighting.podY + 8, kRoomPX, kRoomPX, RP::LED);
        if (fastSinf(now * 0.0015f) > -0.2f)
            canvas.fillRect(lighting.podX + 4, lighting.podY + 4, kRoomPX, kRoomPX, RP::GREEN_DK);
    }
}

void drawRoom2CounterTopShadow(M5Canvas& canvas, int pigDrawX, int pigDrawY,
                                uint32_t now, PigLight light) {
    if (!room2LightingRuntime.valid) return;

    int topX = room2LightingRuntime.counterX - 4;
    int topY = room2LightingRuntime.counterY;
    int topW = kR3_CounterW + 8;
    int pigCX = pigDrawX + kPigW / 2;
    int breathe = calcBreathe(now);
    float halfW = 16.0f + (float)breathe * 0.5f;

    for (int px = topX; px < topX + topW; px += kRoomPX) {
        if (px < 0 || px >= SCREEN_WIDTH) continue;
        float dx = fabsf(((float)px + 2.0f - (float)pigCX) / (halfW + 0.01f));
        if (dx >= 1.0f) continue;
        float darkFactor = 0.26f * (1.0f - dx * dx);
        if (darkFactor < 0.03f) continue;
        uint16_t base = fastReadPx(canvas, px, topY);
        canvas.fillRect(px, topY, kRoomPX, kRoomPX, darken565(base, darkFactor));
    }

    if (light.tint == 0) return;

    float dir = ((float)pigCX - (float)light.x) >= 0.0f ? 1.0f : -1.0f;
    float tailLen = 24.0f + fminf(24.0f, fabsf((float)light.y - (float)(pigDrawY + 18)) * 0.18f);
    for (int px = topX; px < topX + topW; px += kRoomPX) {
        if (px < 0 || px >= SCREEN_WIDTH) continue;
        float proj = (((float)px + 2.0f) - (float)pigCX) * dir;
        if (proj <= 0.0f || proj >= tailLen) continue;
        float t = proj / tailLen;
        float falloff = (1.0f - t);
        falloff *= falloff;
        float lateral = fabsf(((float)px + 2.0f) - (float)pigCX) / (halfW * 2.0f + 0.01f);
        if (lateral >= 1.0f) continue;
        float darkFactor = 0.16f * falloff * (1.0f - lateral * lateral);
        if (darkFactor < 0.02f) continue;
        uint16_t base = fastReadPx(canvas, px, topY);
        canvas.fillRect(px, topY, kRoomPX, kRoomPX, darken565(base, darkFactor));
    }
}

static void applyRoom2NoirToPig(M5Canvas& canvas) {
    if (!room2LightingRuntime.valid) return;
    static constexpr MenuPig::PigNoirProfile kProfile = {102, 210, 38, 90, 85};
    applyDirectionalPigNoir(canvas, kProfile);
}

void drawRoom2Foreground(M5Canvas& canvas, uint32_t now) {
    if (roamState != RoamState::ROOM_TRANSITION) {
        applyRoom2NoirToPig(canvas);
    }
    bool pigBehindCounter = isStationVisualActive(Station::COOKING) ||
        (roamState == RoamState::WALKING_TO && walkTargetStation == Station::COOKING &&
         pigX < (float)(kR3_CounterX + kR3_CounterW));
    if (pigBehindCounter) {
        int cx = kR3_CounterX;
        int cy = kR3_CounterY;
        if (isStationVisualActive(Station::COOKING) && room2LightingRuntime.valid) {
            cx = room2LightingRuntime.counterX;
            cy = room2LightingRuntime.counterY;
        }
        canvas.fillRect(cx, cy, kR3_CounterW, kRoomPX, RP::D_STRUCT);
        canvas.fillRect(cx, cy + kRoomPX, kR3_CounterW, kRoomPX, RP::DEEP);
    }
    if (isStationVisualActive(Station::IN_BED)) {
        int px = room2LightingRuntime.valid ? room2LightingRuntime.podX : kR3_PodX;
        int py = room2LightingRuntime.valid ? room2LightingRuntime.podY : kR3_PodY;
        int sillY = py + kR3_PodH - 8;
        float breath = fastSinf((float)now * 0.0021f);
        int lift = breath > 0.35f ? -kRoomPX : (breath > -0.35f ? -kPigPX : 0);
        int blanketX = px + 40;
        int blanketY = py + 20 + lift;
        int blanketR = px + kR3_PodW - 8;

        canvas.fillRect(blanketX + 12, blanketY, blanketR - blanketX - 12,
                        kRoomPX, RP::D_FILL);
        canvas.fillRect(blanketX + 4, blanketY + kRoomPX,
                        blanketR - blanketX - 4, kRoomPX, RP::D_FILL);
        canvas.fillRect(blanketX, blanketY + 2 * kRoomPX,
                        blanketR - blanketX, sillY - (blanketY + 2 * kRoomPX),
                        RP::D_WALL_NEAR);
        canvas.fillRect(blanketX + 8, blanketY + 3 * kRoomPX,
                        blanketR - blanketX - 12, kRoomPX, RP::D_STRUCT);
        canvas.fillRect(blanketX + 24, blanketY + kRoomPX,
                        kRoomPX, kRoomPX, RP::D_DEEP);
        canvas.fillRect(blanketX + 52, blanketY + 3 * kRoomPX,
                        kRoomPX, kRoomPX, RP::D_DEEP);

        canvas.fillRect(px + 4, sillY, kR3_PodW - 8, kRoomPX, RP::D_STRUCT);
        canvas.fillRect(px + 8, sillY + kRoomPX,
                        kR3_PodW - 16, kRoomPX, RP::D_DEEP);
    }
}

} // namespace MenuPig
