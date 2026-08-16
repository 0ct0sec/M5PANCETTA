/** room1_apartment.cpp — Noir Apartment (worn couch, venetian blind window, floor grate, wall pipes) */

#include "../menu_pig_internal.h"
#include "exterior_sprites.h"
#include "../pixel_materials.h"
#include "../pixel_furniture.h"
#include "../pixel_lighting.h"
#include "../pixel_weather.h"
#include "../../util/time_math.h"

namespace MenuPig {

using namespace PixelMat;
using namespace PixelFurn;
using namespace PixelLight;
using namespace PixelWeather;

static uint32_t tvNextFlip = 0;
static uint32_t tvFlipStart = 0;
static bool tvFlipping = false;
static bool tvShowBars = false;

static void drawWallTV(M5Canvas& canvas, uint32_t now) {
    const int tx = kR2_TvX, ty = kR2_TvY, tw = kR2_TvW, th = kR2_TvH;
    canvas.fillRect(tx, ty - 4, kRoomPX, 4, RP::D50_STRUCT);
    canvas.fillRect(tx + 4, ty - 8, kRoomPX, kRoomPX, RP::D50_STRUCT);
    canvas.fillRect(tx + tw - 4, ty - 4, kRoomPX, 4, RP::D50_STRUCT);
    canvas.fillRect(tx + tw - 4, ty - 8, kRoomPX, kRoomPX, RP::D50_STRUCT);

    canvas.fillRect(tx, ty, tw, th, RP::D50_STRUCT);
    canvas.fillRect(tx + 4, ty, tw - 8, 4, RP::SHADOW_C);
    canvas.fillRect(tx + 4, ty + 4, tw - 8, th - 8, RP::BG);

    int sx = tx + 4, sy = ty + 4, sw = tw - 8, sh = th - 8;

    if (tvNextFlip == 0) { tvNextFlip = now + 3000 + (wallHash(2, 2, now) % 5000); }
    if (TimeMath::reached(now, tvNextFlip) && !tvFlipping) {
        tvFlipping = true;
        tvFlipStart = now;
        tvShowBars = ((wallHash(0, 0, now) & 0xFF) < 38);
        tvNextFlip = now + 8000 + (wallHash(1, 1, now) % 6000);
    }
    if (tvFlipping && (now - tvFlipStart) > 200) {
        tvFlipping = false;
    }

    if (tvFlipping) {
        if (tvShowBars) {
            uint16_t barColors[] = { RP::CRT, RP::WARM, RP::NEON, RP::FLUOR };
            for (int by = sy; by < sy + sh; by += kRoomPX) {
                int b = ((by - sy) / kRoomPX) & 3;
                canvas.fillRect(sx, by, sw, kRoomPX, barColors[b]);
            }
        } else {
            canvas.fillRect(sx, sy, sw, sh, RP::FLUOR);
        }
    } else {
        const uint8_t rf = roomMood.rfActivity;
        const uint8_t snowThreshold = (uint8_t)(76u + ((uint16_t)rf * 52u) / 255u);
        const uint32_t snowStepMs = 140u - ((uint32_t)rf * 60u) / 255u;
        uint32_t snowSeed = now / snowStepMs;
        for (int py = sy; py < sy + sh; py += kRoomPX) {
            for (int px = sx; px < sx + sw; px += kRoomPX) {
                const uint32_t grainHash = wallHash(px, py, snowSeed);
                if ((grainHash & 0xFFu) >= snowThreshold) continue;

                uint16_t grain = RP::CRT;
                const uint8_t accentRoll = (uint8_t)((grainHash >> 8) & 0xFFu);
                if (roomMood.spamActive && accentRoll < 34u) grain = RP::SPARK;
                else if (roomMood.trackerPresent && accentRoll < 24u) grain = RP::NEON;
                canvas.fillRect(px, py, kRoomPX, kRoomPX, grain);
            }
        }
        int scanY = (sy + (int)((float)(now % 2400) / 2400.0f * (float)sh)) & ~3;
        if (scanY >= sy && scanY + kRoomPX <= sy + sh) {
            canvas.fillRect(sx, scanY, sw, kRoomPX, RP::CRT);
        }
        const uint32_t noiseCycle = 3900u - ((uint32_t)rf * 1800u) / 255u;
        int noiseY = (sy + (int)((float)(now % noiseCycle) / (float)noiseCycle * (float)(sh + 8)) - 4) & ~3;
        if (noiseY >= sy && noiseY + 8 <= sy + sh) {
            canvas.fillRect(sx, noiseY, sw, 8, RP::DEEP);
        }
    }
}

static void drawLampCone(M5Canvas& canvas, uint32_t now) {
    bool thundering = Weather::isThunderFlashing();
    if (thundering) return;

    int shadeY = kR2_LampY + 14;
    int floorY = kFloorY;
    int coneH = floorY - shadeY;
    uint32_t flickSeed = now / 200;

    for (int dy = 0; dy < coneH; dy += kRoomPX) {
        float t = (float)dy / (float)coneH;
        int topW = 6, botW = 20;
        int halfW = (int)(((float)topW + t * (float)(botW - topW)) / 2.0f);
        int centerX = kR2_LampX + 4;
        int y = shadeY + dy;

        for (int dx = -halfW; dx < halfW; dx += kRoomPX) {
            int x = centerX + dx;
            if (x < 4 || x >= SCREEN_WIDTH - 4) continue;
            float edgeDist = fabsf((float)dx / (float)halfW);
            uint8_t baseDensity = (uint8_t)(40.0f * (1.0f - edgeDist * edgeDist));
            int flick = (int)(wallHash(x, y, flickSeed) & 0x7) - 3;
            int d = (int)baseDensity + flick;
            if (d < 0) d = 0;
            if (d > 255) d = 255;
            if (bayer4[(y / kRoomPX) & 3][(x / kRoomPX) & 3] < (uint8_t)d * 4) {
                uint16_t base = fastReadPx(canvas, x, y);
                float blendAmt = 0.35f + 0.25f * (1.0f - edgeDist);
                canvas.fillRect(x, y, kRoomPX, kRoomPX, screenBlend565f(base, RP::WARM, blendAmt));
            }
        }
    }
    drawLightPool4(canvas, RP::WARM, 112, kFloorY - 4, 24, 4, 25, 0x7C01u);
}

static void drawSideTableAndLamp(M5Canvas& canvas, uint32_t now) {
    bool thundering = Weather::isThunderFlashing();

    canvas.fillRect(kR2_TableX, kR2_TableY, kRoomPX, kFloorY - kR2_TableY, RP::D50_STRUCT);
    canvas.fillRect(kR2_TableX + 8, kR2_TableY, kRoomPX, kFloorY - kR2_TableY, RP::D50_STRUCT);
    canvas.fillRect(kR2_TableX, kR2_TableY, 12, kRoomPX, RP::D50_FILL);
    canvas.fillRect(kR2_TableX + 4, kR2_TableY, 4, 4, RP::SHADOW_C);

    int lx = kR2_LampX;
    canvas.fillRect(lx, kR2_LampY + 12, 8, kRoomPX, RP::D50_STRUCT);
    canvas.fillRect(lx + 4, kR2_LampY + 4, kRoomPX, 8, RP::D50_STRUCT);
    canvas.fillRect(lx + 4, kR2_LampY, 4, kRoomPX, RP::WARM);
    canvas.fillRect(lx, kR2_LampY + 4, 8, kRoomPX, RP::WARM);

    if (!thundering) {
        canvas.fillRect(lx + 4, kR2_LampY + 4, kRoomPX, kRoomPX, RP::WARM);
    }

    int gx = kR2_TableX + 4, gy = kR2_TableY - 8;
    canvas.fillRect(gx, gy, 8, 4, RP::D50_STRUCT);
    canvas.fillRect(gx, gy + 4, 8, 4, RP::D50_STRUCT);
    canvas.fillRect(gx, gy, 4, 8, RP::D50_STRUCT);
    canvas.fillRect(gx + 4, gy, 4, 8, RP::D50_STRUCT);
    canvas.fillRect(gx + 4, gy + 4, 4, 4, RP::WARM);
    int iceOff = ((now / 4000) & 1) ? 0 : 4;
    canvas.fillRect(gx + iceOff, gy + 4, kRoomPX, kRoomPX, RP::SHAFT);
}

static ExteriorSprites::Emitter sampleApartmentWindowEmitter(uint32_t now) {
    ExteriorSprites::RenderOptions exteriorGrade;
    exteriorGrade.thunder = Weather::isThunderFlashing();
    exteriorGrade.tintActive = colorEvent.active;
    exteriorGrade.tintColor565 = colorEvent.color565;
    exteriorGrade.tintIntensity = colorEvent.intensity;
    return ExteriorSprites::dominantEmitter(
        ExteriorSprites::Scene::Apartment, now,
        kR2_WindowX + kRoomPX, kR2_WindowY + kRoomPX,
        kR2_WindowW - kRoomPX * 2, kR2_WindowH - kRoomPX * 2,
        (int8_t)parallaxFar, exteriorGrade);
}

// ==[ PASSING-CAR GOD RAY ]==
// Traffic behind this pane is continuous — a car is inside the opening ~92% of
// the time, two at once half of that — so casting a room-wide shaft for every
// crossing would not read as an event, it would just leave the apartment dark.
// The room therefore commits to ONE headlight, rides its entire traverse, and
// then stands down. Against the authored apartment lanes that yields a pass
// every ~20s, 4.8s long, sweeping 140 of the pane's 152px.
static constexpr uint8_t kGodRayFlareFloor = 90;    // only real headlights
static constexpr float kGodRayArmProgress = 0.28f;  // must catch it early
static constexpr uint32_t kGodRayCooldownMs = 12000;

static int8_t godRayLane = -1;
static uint32_t godRayNextArm = 0;
static uint32_t godRayLastNow = 0;
static PixelLight::GodRayCast godRayCast = {0.0f, 0, 0, 0};

// Traverse of the opening as 0..1, independent of travel direction. False once
// the headlight is outside it.
static bool godRayProgress(const ExteriorSprites::TrafficSample& s, float& out) {
    const int apX0 = kR2_WindowX + kRoomPX;
    const int apW = kR2_WindowW - kRoomPX * 2;
    float t = (float)(s.emitterX - apX0) / (float)apW;
    if (t <= 0.0f || t >= 1.0f) return false;
    out = (s.dirX > 0) ? t : (1.0f - t);
    return true;
}

static const PixelLight::GodRayCast& apartmentGodRay(uint32_t now) {
    if (now == godRayLastNow) return godRayCast;
    godRayLastNow = now;
    godRayCast.intensity = 0.0f;

    // The storm wash already owns the whole room on those frames.
    if (Weather::isThunderFlashing()) {
        godRayLane = -1;
        return godRayCast;
    }

    ExteriorSprites::TrafficSample samples[6];
    const int count = ExteriorSprites::sampleTraffic(
        ExteriorSprites::Scene::Apartment, now,
        kR2_WindowX + kRoomPX, kR2_WindowY + kRoomPX,
        kR2_WindowW - kRoomPX * 2, kR2_WindowH - kRoomPX * 2,
        samples, 6, (int8_t)parallaxFar);

    float prog = 0.0f;
    if (godRayLane >= 0 &&
        (godRayLane >= count || !samples[godRayLane].visible ||
         !godRayProgress(samples[godRayLane], prog))) {
        godRayLane = -1;
        godRayNextArm = now + kGodRayCooldownMs;
    }
    if (godRayLane < 0 && (int32_t)(now - godRayNextArm) >= 0) {
        for (int i = 0; i < count; i++) {
            // Visibility is not redundant with progress: the headlight clears
            // the pane edge a beat before the body does, and arming in that gap
            // latches a car that is not on screen yet — it dies the same frame.
            if (samples[i].visible && samples[i].flare >= kGodRayFlareFloor &&
                godRayProgress(samples[i], prog) &&
                prog < kGodRayArmProgress) {
                godRayLane = (int8_t)i;
                break;
            }
        }
    }
    if (godRayLane >= 0 && godRayLane < count) {
        const ExteriorSprites::TrafficSample& s = samples[godRayLane];
        // Bell over the crossing: swells in, peaks mid-pane, fades out. Squared
        // rather than raised to a fractional power so it costs one multiply.
        const float bell = fastSinf(prog * 3.14159265f);
        godRayCast.intensity = bell * bell;
        godRayCast.srcX = s.emitterX;
        godRayCast.srcY = s.emitterY;
        // Same light the blinds already cast, so the pass reads as this room's
        // window rather than a second, unrelated source.
        godRayCast.tint = colorEvent.active
            ? mixColor565(RP::SHAFT, colorEvent.color565,
                          (uint8_t)(colorEvent.intensity / 4u))
            : RP::SHAFT;
    }
    return godRayCast;
}

PigLight selectRoom1PigKeyLight(int pigDrawX, int pigDrawY, uint32_t now) {
    PigLight crtLight;
    crtLight.x = (int16_t)(kR2_TvX + kR2_TvW / 2);
    crtLight.y = (int16_t)(kR2_TvY + kR2_TvH / 2);
    crtLight.tint = RP::CRT;

    ExteriorSprites::Emitter windowEmitter = sampleApartmentWindowEmitter(now);
    PigLight windowLight;
    windowLight.x = windowEmitter.x;
    windowLight.y = windowEmitter.y;
    windowLight.tint = windowEmitter.active ? windowEmitter.color565 : RP::SHAFT;

    PigLight lampLight;
    lampLight.x = (int16_t)(kR2_LampX + 6);
    lampLight.y = (int16_t)(kR2_LampY + 8);
    lampLight.tint = RP::WARM;

    PigLight grateLight;
    grateLight.x = (int16_t)(kR2_GrateX + kR2_GrateW / 2);
    grateLight.y = (int16_t)kR2_GrateY;
    grateLight.tint = RP::WARM;

    float pigCX = (float)(pigDrawX + kPigW / 2);
    float pigCY = (float)(pigDrawY + kPigH / 2);
    PigLight key = crtLight;
    float keyS = 0.0f;
    const struct { PigLight l; float r, w; } emitters[] = {
        {crtLight, 120.0f, 0.8f}, {windowLight, 160.0f, 0.7f},
        {lampLight, 110.0f, 0.7f}, {grateLight, 80.0f, 0.4f}
    };
    for (const auto& e : emitters) {
        float s = room2EmitterScoreAt(e.l, pigCX, pigCY, e.r, e.w);
        if (s > keyS) { keyS = s; key = e.l; }
    }
    if (key.tint == 0) key = getRoomNeonLight(1, now);
    return key;
}

void drawRoom1(M5Canvas& canvas, uint32_t now, RoomRenderPass pass) {
    if (pass == RoomRenderPass::BASE) {
        drawConcreteWall4(canvas, 4, kRoomY + 4,
                          SCREEN_WIDTH - 8, kFloorY - kRoomY - 8, 1u);
        drawMetalFloor4(canvas, 1u);
        PigLight rl;
        rl.x = (int16_t)(kR2_WindowX + 30);
        rl.y = (int16_t)(kR2_WindowY + 30);
        rl.tint = RP::NEON;
        drawNeonWash4(canvas, PigLightEval(rl, 160.0f, 0.07f),
                      160.0f, 0.07f, 0x4C11u,
                      4, kRoomY + 4, SCREEN_WIDTH - 8,
                      kFloorY - kRoomY - 4);

        drawACUnit4(canvas, 8 + parallaxFar, 30);
        drawCeilingStain4(canvas, 96 + parallaxFar, kRoomY + 6);
        canvas.fillRect(20 + parallaxFar, kRoomY + 8, 60,
                        kRoomPX, RP::WALL_MID);
        canvas.fillRect(128 + parallaxFar, kRoomY + 8, 48,
                        kRoomPX, RP::WALL_MID);
        canvas.fillRect(48 + parallaxFar, kRoomY + 12,
                        kRoomPX, 8, RP::SHADOW_C);
        canvas.fillRect(48 + parallaxFar, kRoomY + 8,
                        kRoomPX, 12, RP::WALL_MID);
        drawSmallVent4(canvas, 56 + parallaxFar, kRoomY + 4);
        drawSprinkler4(canvas, 150 + parallaxFar, kRoomY);
        drawConduitRun4(canvas, 10 + parallaxFar, kRoomY + 60, 40);
        drawWallOutlet4(canvas, 100 + parallaxFar, 80);

        // Outlines track the fixture footprint, not the call origin. The AC
        // unit hangs its condensate drip one cell ABOVE the body, so its real
        // envelope starts at y-4 and is 16 deep; a contour cut at the body
        // origin laid its top row straight over the drip and erased it.
        drawPopOutline1px(canvas, 8 + parallaxFar, 24, 20, 16,
                          PopOutlineStyle::MIXED, 0xB001u);
        drawPopOutline1px(canvas, 96 + parallaxFar, kRoomY + 6, 14, 12,
                          PopOutlineStyle::SPARSE, 0xB011u);
        // drawSmallVent4 is a 12x12 grille. Cut for 10x6 the right column and
        // the bottom row both landed inside it, punching a BG cross through
        // the louvres.
        drawPopOutline1px(canvas, 56 + parallaxFar, kRoomY + 4, 12, 12,
                          PopOutlineStyle::SOLID, 0xB031u);
        drawPopOutline1px(canvas, 10 + parallaxFar, kRoomY + 58, 40, 8,
                          PopOutlineStyle::SPARSE, 0xB051u);
        drawPopOutline1px(canvas, 100 + parallaxFar, 80, 12, 12,
                          PopOutlineStyle::SOLID, 0xB061u);

        drawNoirWindowBase(canvas,
                           kR2_WindowX, kR2_WindowY,
                           kR2_WindowW, kR2_WindowH, pigX);
        drawFloorGrate4(canvas, kR2_GrateX, kR2_GrateY,
                        kR2_GrateW, kR2_GrateH);
        drawPipeRun4(canvas, kR2_PipeX, kR2_PipeY, 40, true, true);
        drawPipeRun4(canvas, kR2_Pipe2X, kR2_Pipe2Y, 40, true, false);
        drawCeilingStain4(canvas, 16 + parallaxFar, kRoomY + 6);
        return;
    }

    const ExteriorSprites::Emitter windowEmitter =
        sampleApartmentWindowEmitter(now);
    const uint16_t windowTint = windowEmitter.active
        ? windowEmitter.color565 : RP::SHAFT;

    // ==[ ARCHITECTURE ANSWERS THE WINDOW ]==
    // The pane is the only thing in this room whose colour actually moves —
    // it tracks whichever exterior emitter is currently passing — and the
    // concrete beside it was the last surface not to notice. The baked pass
    // can only carry a fixed ambient, so the moving half lives here, bounded
    // to the window's own reach and drawn before any furniture so it grades
    // architecture and nothing else.
    if (windowEmitter.active) {
        PigLight winKey;
        winKey.x = (int16_t)windowEmitter.x;
        winKey.y = (int16_t)windowEmitter.y;
        winKey.tint = windowTint;
        drawNeonWash4(canvas, PigLightEval(winKey, 92.0f, 0.24f),
                      92.0f, 0.24f, 0x4C12u,
                      kR2_WindowX - 48, kR2_WindowY - 24,
                      kR2_WindowW + 96, kR2_WindowH + 64);
    }

    drawDustMotes4(canvas, now);

    calcWobble(now);
    int sofaWx = (currentStation == Station::ON_SOFA) ? wobbleX : 0;
    int sofaWy = (currentStation == Station::ON_SOFA) ? wobbleY : 0;

    drawNeonArrow4(canvas, now, 92 + parallaxFar, 24);
    if (isNeonOn(now))
        drawLightPool4(canvas, RP::NEON, 90 + parallaxFar, 32, 12, 4, 20, 0xAA01u);

    uint32_t stainCycle = 6000;
    float sdPhase = (float)(now % stainCycle) / (float)stainCycle;
    int stainBotX = 100;
    int stainBotY = kRoomY + 18;
    if (sdPhase < 0.15f) {
        canvas.fillRect(stainBotX, stainBotY, kRoomPX, kRoomPX, RP::WALL_MID);
    } else if (sdPhase < 0.5f) {
        float ft = (sdPhase - 0.15f) / 0.35f;
        int dropYQ8 = stainBotY * 256 + (int)(ft * ft * 22.0f * 256.0f);
        drawSmoothWaterDrop4(canvas, stainBotX, dropYQ8, kRoomY, kFloorY, RP::SHAFT, 120);
    }

    drawWallTV(canvas, now);
    drawSideTableAndLamp(canvas, now);

    // The TV hangs off two brackets 8px above its own body, so the fixture
    // starts at kR2_TvY-8. Cut at the body origin the contour laid its top row
    // across both brackets and — via the same room-lattice rounding as the
    // table below — its bottom row 2px inside the screen bezel.
    drawPopOutline1px(canvas, kR2_TvX, kR2_TvY - 8, kR2_TvW,
                      kR2_TvH + 8 + kRoomPX,
                      PopOutlineStyle::MIXED, 0xB0A1u);
    // These two are raw fillRect props on roomY() coordinates, so they live on
    // the room lattice (y ≡ 2 mod 4) while drawRoomPopPixel snaps its contour
    // to the screen lattice. Passing the exact footprint therefore rounds the
    // right/bottom contour 2px BACK INTO the prop; both need a cell of slack.
    // Table is 12 wide (top plate + both legs), not 8 — the old 8 put a BG
    // column down the right leg. Lamp is 16 tall to the base, not 14.
    drawPopOutline1px(canvas, kR2_TableX, kR2_TableY, 12,
                      kFloorY - kR2_TableY + kRoomPX,
                      PopOutlineStyle::SPARSE, 0xB0B1u);
    drawPopOutline1px(canvas, kR2_LampX, kR2_LampY, 8, 18, PopOutlineStyle::MIXED, 0xB0C1u);

    drawWornCouch4(canvas, kR2_SofaX + sofaWx, kR2_SofaY + sofaWy, now);
    {
        int sx0 = (kR2_SofaX + sofaWx) & ~3;
        int sy0 = (kR2_SofaY + sofaWy) & ~3;
        int sx1 = sx0 + kR2_SofaW + 4;
        int sy1 = kFloorY;
        const uint8_t dt = (uint8_t)(0.50f * 256.0f);
        for (int py = sy0; py < sy1; py += kRoomPX)
            for (int px = sx0; px < sx1; px += kRoomPX)
                fastFillBlock4(canvas, px, py, lerpColor565_8(fastReadPx(canvas, px, py), RP::BG, dt));
    }

    drawAshtray4(canvas, kR2_SofaX + kR2_SofaW - 12 + sofaWx, kR2_SofaY - 4 + sofaWy);
    {
        int embX = kR2_SofaX + kR2_SofaW - 12 + sofaWx + 8;
        int embY = kR2_SofaY - 4 + sofaWy - 4;
        if ((wallHash(embX, embY, now / 300) & 0xFF) < 179) {
            canvas.fillRect(embX, embY, kRoomPX, kRoomPX, RP::WARM);
            for (int dx = -2; dx <= 2; dx += kRoomPX) {
                if ((wallHash(embX + dx, embY + 2, 33317) & 0xFF) < 50)
                    canvas.fillRect(embX + dx, embY + 2, kRoomPX, kRoomPX, RP::WARM);
            }
        }
    }

    const int winWx = 0, winWy = 0;
    // Keep the bathroom's proven exterior -> wet glass -> hardware contract.
    // Apartment blinds remain the room-specific silhouette, but the pane must
    // expose the live generated city rather than a flat BG cutout.
    drawNoirWindowMotion(canvas, now,
                         kR2_WindowX + winWx, kR2_WindowY + winWy,
                         kR2_WindowW, kR2_WindowH, pigX, windowTint);
    // That shared window compositor also owns the pane's floor bars, wet neon
    // breakup, and event-color reflection. Keep those source-linked effects in
    // one pass; the old room-local puddle and gradient doubled their brightness
    // and made the reflection look wider than the window that cast it.
    // Window-shaft motes are owned by drawDustBeam4 below; do not stack a
    // second dust topology over the same glass volume.

    {
        int ashX = kR2_SofaX + kR2_SofaW - 8 + sofaWx;
        int ashY = kR2_SofaY - 6 + sofaWy;
        for (int i = 0; i < 3; i++) {
            float phase = (float)((now + (uint32_t)i * 1100u) % 2200) / 2200.0f;
            int dy = -(int)(phase * 14.0f);
            int dx = (int)(fastSinf(phase * 6.28f + (float)i * 2.0f) * 3.0f);
            if (phase < 0.7f || (uint8_t)(255.0f * (1.0f - (phase - 0.7f) / 0.3f)) > 80) {
                int sx = (ashX + dx) & ~3;
                int sy = (ashY + dy) & ~3;
                if (sy > kRoomY + 4 && sy < kFloorY - 4)
                    canvas.fillRect(sx, sy, kRoomPX, kRoomPX, RP::DUST);
            }
        }
    }

    {
        int wix0 = (kR2_WindowX + winWx + 4) & ~3;
        int wiy0 = (kR2_WindowY + winWy + 4) & ~3;
        int wix1 = kR2_WindowX + winWx + kR2_WindowW - 4;
        int wiy1 = kR2_WindowY + winWy + kR2_WindowH - 4;
        const uint8_t cityDarkT8 = (uint8_t)(0.30f * 256.0f);
        for (int py = wiy0; py < wiy1; py += kRoomPX) {
            for (int px = wix0; px < wix1; px += kRoomPX) {
                fastFillBlock4(canvas, px, py, lerpColor565_8(fastReadPx(canvas, px, py), RP::BG, cityDarkT8));
            }
        }
    }

    {
        uint8_t glowDensity = tvFlipping ? 40 : 20;
        if (Weather::isThunderFlashing()) glowDensity = 8;
        drawLightPool4(canvas, RP::CRT, kR2_TvX - 6, kR2_TvY - 4, kR2_TvW + 12, kR2_TvH + 8, glowDensity, 0x7B01u);
        drawLightPool4(canvas, RP::CRT, kR2_TvX - 2, kR2_SofaY, kR2_TvW + 4, 8, (uint8_t)(glowDensity * 3 / 4), 0x7B02u);
    }
    if (!Weather::isThunderFlashing()) {
        int shaftTopX = kR2_WindowX + 10;
        int shaftTopY = kR2_WindowY + kR2_WindowH + 4;
        int shaftHeight = kFloorY - 2 - shaftTopY;
        for (int dy = 0; dy < shaftHeight; dy += kRoomPX) {
            float t = (float)dy / (float)shaftHeight;
            int width = 20 + (int)(t * 20.0f);
            int xStart = shaftTopX - (int)(t * 15.0f);
            for (int dx = 0; dx < width; dx += kRoomPX) {
                int x = xStart + dx, y = shaftTopY + dy;
                if (x < 4 || x >= SCREEN_WIDTH - 4 || y < kRoomY || y >= kFloorY) continue;
                float edgeDist = fabsf((float)dx / (float)width - 0.5f) * 2.0f;
                uint8_t threshold = (uint8_t)(35.0f * (1.0f - edgeDist));
                if (bayer4[(y/kRoomPX) & 3][(x/kRoomPX) & 3] < threshold * 4) {
                    uint16_t base = fastReadPx(canvas, x, y);
                    canvas.fillRect(x, y, kRoomPX, kRoomPX, screenBlend565f(base, RP::SHAFT, 0.45f));
                }
            }
        }
        drawDustBeam4(canvas, now,
                      kR2_WindowX + kR2_WindowW / 2, shaftTopY + 2, 30,
                      kR2_WindowX + kR2_WindowW / 2 + 4, kFloorY - 2, 78,
                      RP::SHAFT, RP::DUST, 0xA111u);

        int armX = kR2_SofaX + kR2_SofaW - 8 + sofaWx;
        int armY = kR2_SofaY + 8 + sofaWy;
        for (int py = 0; py < 12; py += kRoomPX) {
            for (int px = 0; px < 8; px += kRoomPX) {
                if ((wallHash(armX + px, armY + py, 88517) & 0xFF) < 30) {
                    uint16_t base = fastReadPx(canvas, armX + px, armY + py);
                    canvas.fillRect(armX + px, armY + py, kRoomPX, kRoomPX,
                                    screenBlend565f(base, RP::SHAFT, 0.40f));
                }
            }
        }
    } else {
        PigLight stormLight;
        stormLight.x = (int16_t)(kR2_WindowX + winWx + kR2_WindowW / 2);
        stormLight.y = (int16_t)(kR2_WindowY + winWy);
        stormLight.tint = RP::FLUOR;
        PigLightEval stormEval(stormLight, 280.0f, 0.22f);
        drawFurnitureWash4(canvas, 0, kRoomY + kRoomPX, SCREEN_WIDTH, kFloorY - kRoomY,
                           stormEval, 280.0f, 0.22f);
        drawLightPoolGradient4(canvas, RP::FLUOR,
                               kR2_WindowX + winWx - 16, kFloorY - 8,
                               kR2_WindowW + 32, 8, 26, 0x99317u);
    }

    drawLampCone(canvas, now);

    {
        uint8_t grateDensity = (uint8_t)(15 + (int)(fastSinf(now * 0.0008f) * 8.0f));
        drawLightPool4(canvas, RP::WARM, kR2_GrateX + 2, kR2_GrateY + 2, kR2_GrateW - 4, kR2_GrateH - 4, grateDensity, 33517u);
    }

    drawNoirBlindShadowSweep(canvas, now,
                             kR2_WindowX + winWx, kR2_WindowY + winWy,
                             kR2_WindowW, kR2_WindowH);

    {
        uint32_t dt = (now - roomDripStart) % ROOM_DRIP_CYCLE_MS;
        float phase = (float)dt / (float)ROOM_DRIP_CYCLE_MS;
        if (phase < 0.6f) {
            int pipeBotY = kR2_PipeY + 40;
            int fallH = kFloorY - pipeBotY;
            float t = phase / 0.6f;
            int dropYQ8 = pipeBotY * 256 + (int)(t * t * (float)fallH * 256.0f);
            drawSmoothWaterDrop4(canvas, kR2_PipeX, dropYQ8, pipeBotY & ~3, kFloorY, RP::SHAFT, 132);
        }
    }
    {
        uint32_t dt2 = (now - roomDripStart + 1600) % (ROOM_DRIP_CYCLE_MS * 2);
        float phase2 = (float)dt2 / (float)(ROOM_DRIP_CYCLE_MS * 2);
        if (phase2 < 0.4f) {
            int pipe2BotY = kR2_Pipe2Y + 40;
            int fallH2 = kFloorY - pipe2BotY;
            float t2 = phase2 / 0.4f;
            int dropYQ8 = pipe2BotY * 256 + (int)(t2 * t2 * (float)fallH2 * 256.0f);
            drawSmoothWaterDrop4(canvas, kR2_Pipe2X, dropYQ8, pipe2BotY & ~3, kFloorY, RP::SHAFT, 124);
        }
    }
    {
        uint32_t acCycle = 4800;
        float acPhase = (float)((now + 2400) % acCycle) / (float)acCycle;
        if (acPhase < 0.5f) {
            float t = acPhase / 0.5f;
            int dropYQ8 = 46 * 256 + (int)(t * t * (float)(kFloorY - 46) * 256.0f);
            drawSmoothWaterDrop4(canvas, 8, dropYQ8, 44, kFloorY, RP::SHAFT, 124);
        }
    }

    // drawNoirWindow owns pane rain; sill runoff lands behind the foreground
    // pipe and pig in both ordinary and teleport compositing paths.
    drawRoomRain(canvas, now,
                 kR2_WindowX + winWx, kR2_WindowY + winWy,
                 kR2_WindowW, kR2_WindowH);
    drawFogHaze4(canvas, now,
                 kR2_WindowX + 40, kR2_WindowY + 10,
                 40, kFloorY - kR2_WindowY - 10,
                 windowTint, 18);
}

void drawRoom1Foreground(M5Canvas& canvas, uint32_t now) {
    static constexpr MenuPig::PigNoirProfile kProfile = {128, 210, 38, 90, 85};
    applyDirectionalPigNoir(canvas, kProfile);

    // The pass belongs here, not in the room body: Pancetta is already
    // composited by this point, so one sweep grades him and the room together.
    // Run before the pig and he would hold full brightness while the apartment
    // dropped 44% around him — a cutout pasted over the scene. The near pipe
    // below stays in front of it, which is what a foreground occluder is for.
    drawWindowGodRay4(canvas, apartmentGodRay(now),
                      kR2_WindowX, kR2_WindowY, kR2_WindowW, kR2_WindowH);

    if (isStationVisualActive(Station::ON_SOFA)) {
        int lipX = kR2_SofaX + 8 + wobbleX;
        int lipY = kR2_SofaY + kR2_SofaH - 8 + wobbleY;
        canvas.fillRect(lipX, lipY, kR2_SofaW - 16, kRoomPX, RP::D_STRUCT);
        canvas.fillRect(lipX + 4, lipY + kRoomPX, kR2_SofaW - 24, kRoomPX, RP::SHADOW_C);
    }
    int nearDx = parallaxNear;
    int pipeY = (kFloorY - 28) & ~3;
    int pipeX = nearDx & ~3;
    int pipeW = 100;
    canvas.fillRect(pipeX, pipeY, pipeW, kRoomPX, RP::D50_STRUCT);
    canvas.fillRect(pipeX, pipeY + kRoomPX, pipeW, kRoomPX, RP::SHADOW_C);
    canvas.fillRect(pipeX + 16, pipeY - 4, kRoomPX, 4, RP::D50_STRUCT);
    canvas.fillRect(pipeX + 56, pipeY - 4, kRoomPX, 4, RP::D50_STRUCT);
    canvas.fillRect(pipeX + 80, pipeY - 4, 8, 4, RP::WALL_MID);
    canvas.fillRect(pipeX + 80, pipeY - 8, 4, 4, RP::WALL_MID);
    if ((now / 1600) % 3 == 0) {
        int beadYQ8 = (pipeY + 8) * 256 + (int)((now % 1600u) * 12u * 256u / 1600u);
        drawSmoothWaterDrop4(canvas, pipeX + 36, beadYQ8, pipeY + 8, kFloorY, RP::SHAFT, 112);
    }
}

} // namespace MenuPig
