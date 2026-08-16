/** room3_rooftop.cpp — Surveillance Nest (antenna array, satellite dish, rooftop shack, ledge, flying car cinematic) */

#include "../menu_pig_internal.h"
#include "../pixel_materials.h"
#include "../pixel_primitives.h"
#include "../pixel_furniture.h"
#include "../pixel_weather.h"
#include "../sky_volume.h"
#include "../smoke_volume.h"
#include "exterior_sprites.h"

namespace MenuPig {

using namespace PixelMat;

// The shack wall scratches one notch per witnessed minute. The
// fifth notch stops blinking only after ST4K30UT is actually in the ledger.
static constexpr int kStakeoutNotchStep = kRoomPX * 2;
static constexpr int kStakeoutInsetX = kRoomPX * 2;
static constexpr uint32_t kRooftopCondensationSalt = 0xB301u;
static constexpr int kRooftopCondensationCount = 2;
static constexpr int kRooftopCenterPoleOffsetX = 3 * kRoomPX;
static constexpr int kRooftopCenterPoleH = 156;
static constexpr int kRooftopSurfaceY = kFloorY & ~(kRoomPX - 1);
static constexpr int kRooftopCenterPoleTopY =
    kRooftopSurfaceY - kRooftopCenterPoleH;
static constexpr int kRooftopPerchPoleOffsetX = 6 * kRoomPX;
static constexpr int kRooftopPerchPoleTopY = kRooftopSurfaceY - 128;
static constexpr int kRooftopDishAzimuthMax = 2 * kRoomPX;
static constexpr int kRooftopGuyWireAttachY =
    kRooftopCenterPoleTopY + 7 * kRoomPX;
static_assert(kStakeoutInsetX + 4 * kStakeoutNotchStep + kRoomPX <= kR4_ShackW,
              "stakeout evidence must stay inside the shack");
static_assert(kRooftopCenterPoleTopY >= kR4_AntennaY &&
              kRooftopGuyWireAttachY < kRooftopSurfaceY &&
              kRooftopCenterPoleTopY % kRoomPX == 0,
              "rooftop attachments must land on the center antenna mast");
static_assert(kRooftopPerchPoleTopY % kRoomPX == 0 &&
              kR4_DishX + kParallaxMidMin - kRooftopDishAzimuthMax >= 0 &&
              kR4_DishX + kParallaxMidMax + kRooftopDishAzimuthMax +
                  kR4_DishW <= SCREEN_WIDTH,
              "rooftop moving silhouettes must stay inside the scene");

static int rooftopAntennaPoseX() {
    // The geometry contract proves this hero silhouette against the mid-depth
    // envelope. Using the wider near offset clipped its left mast off-screen.
    return (kR4_AntennaX + parallaxMid) & ~(kRoomPX - 1);
}

static int rooftopDishBaseX() {
    return (kR4_DishX + parallaxMid) & ~(kRoomPX - 1);
}

static int rooftopDishAzimuthOffset(uint32_t now) {
    uint32_t rotStepMs = 8000u;
    if (roomMood.spamActive) rotStepMs = 1200u;
    else if (roomMood.trackerPresent) rotStepMs = 2000u;
    else if (roomMood.alertLevel >= 3 || roomMood.rfActivity >= 176u)
        rotStepMs = 2600u;
    else if (roomMood.alertLevel >= 1 || roomMood.rfActivity >= 80u)
        rotStepMs = 4000u;

    int sweepStep = (int)((now / rotStepMs) % 8u);
    if (sweepStep > 4) sweepStep = 8 - sweepStep;
    return (sweepStep - 2) * kRoomPX;
}

static void drawRooftopStakeoutEvidence(M5Canvas& canvas, uint32_t now) {
    int x = kR4_ShackX + parallaxFar + kStakeoutInsetX;
    int y = kR4_ShackY + 16;
    bool room3Station = currentStation == Station::AT_ANTENNA ||
                        currentStation == Station::ON_LEDGE;
    uint8_t done = roomProgress.stakeout ? 5 :
        (room3Station ? min((uint8_t)5, roomProgress.stakeoutPips) : 0);
    for (uint8_t i = 0; i < 5; ++i) {
        uint16_t color = i < done ? RP::GREEN_DK : RP::D_WALL_NEAR;
        if (!roomProgress.stakeout && room3Station && roamState == RoamState::IDLE && i == done &&
            ((now / 650u) & 1u)) {
            color = RP::SPARK;
        }
        canvas.fillRect(x + i * kStakeoutNotchStep, y, kRoomPX, kRoomPX, color);
    }
}

void drawRoom3CinematicCar(M5Canvas& canvas, uint32_t now) {
    Room3CarEventState& s = carState;

    if (!s.initialized) {
        s.initialized = true;
        s.nextTriggerMs = now;
    }

    // ==[ WD FORCE START ]== startWardriveEntry sets this flag
    if (wdCarForceStart) {
        wdCarForceStart = false;
        s.active = true;
        s.startMs = now;
        s.wdMode = true;
        s.wdPigMounted = false;
        s.wdCarDropY = 0.0f;
        wdMountPhase = WDMountPhase::IDLE;
        wdMountStart = 0;
        wdImpactStart = 0;
    }

    // track re-entry for lastDrawMs (car cinematic starts only via explicit wardrive flow)
    s.lastDrawMs = now;
    if (!s.active) {
        room3CinematicCarRunning = false;
        return;
    }
    room3CinematicCarRunning = true;

    // ==[ TIMING ]== dramatic slow entrance through rain wall
    static constexpr uint32_t DISTANT_MS  = ROOM3_CAR_DISTANT_DUR_MS;
    static constexpr uint32_t APPROACH_END = ROOM3_CAR_APPROACH_END_MS;
    static constexpr uint32_t EMERGE_END  = ROOM3_CAR_EMERGE_END_MS;
    static constexpr uint32_t HOVER_END   = ROOM3_CAR_HOVER_END_MS;
    static constexpr uint32_t TURN_END    = ROOM3_CAR_TURN_END_MS;
    static constexpr uint32_t LINGER_END  = ROOM3_CAR_LINGER_END_MS;
    static constexpr uint32_t ACCEL_END   = ROOM3_CAR_ACCEL_END_MS;
    static constexpr uint32_t COOL_END    = ROOM3_CAR_COOL_END_MS;
    const uint32_t rawElapsed = now - s.startMs;
    const uint32_t elapsed = (s.wdMode && isWDMenuCineFast())
        ? wdMenuCineElapsed(rawElapsed) : rawElapsed;

    // ==[ WD MODE: kill car after accel — teleport handles the mode switch ]==
    if (s.wdMode && elapsed >= ROOM3_CAR_ACCEL_END_MS) {
        s.active = false;
        s.wdMode = false;
        room3CinematicCarRunning = false;
        s.nextTriggerMs = 0xFFFFFFFFu;
        return;
    }

    if (!s.wdReturnMode && elapsed >= ROOM3_CAR_COOL_END_MS) {
        s.active = false;
        room3CinematicCarRunning = false;
        s.nextTriggerMs = 0xFFFFFFFFu;
        return;
    }

    // ==[ DOWNWASH ]== the arrival and the departure, felt by every volume in
    // the air. s.startMs is a real event timestamp, so unlike the bath splash
    // and the thunder flash this needs no rising-edge latch — the cinematic
    // already publishes the token gust() wants.
    //
    // The two beats MUST be mutually exclusive. Calling both unconditionally
    // once elapsed passes LINGER_END makes them alternate forever: each gust
    // overwrites the other's dedup token, so neither is ever recognised as
    // already spent and the deck steam gets shredded every single frame.
    if (elapsed >= LINGER_END) {
        // Thrusters lighting up to leave — harder than the descent.
        SmokeFx::gust(s.startMs + 1u, 230u);
    } else if (elapsed >= EMERGE_END) {
        // Settling onto the roof out of the rain wall.
        SmokeFx::gust(s.startMs ? s.startMs : 1u, 196u);
    }

    // ==[ THEME-DERIVED CAR PALETTE ]== adapts to any 2-color theme via RP
    const uint16_t SPORT_BODY = RP::NEON;
    const uint16_t SPORT_LIT  = screenBlend565f(RP::NEON, RP::FLUOR, 0.35f);
    const uint16_t SPORT_DARK = Display::lerpColor565(RP::DEEP, RP::NEON, 0.30f);
    const uint16_t SPORT_SIDE = Display::lerpColor565(RP::FILL, RP::NEON, 0.50f);
    const uint16_t SPORT_ROOF = Display::lerpColor565(RP::SHADOW_C, RP::NEON, 0.30f);
    const uint16_t SPORT_GLOW = RP::PUDDLE;
    // ==[ EMISSIVE LIGHTS ]== bright sources, hue-mapped through the room palette
    const uint16_t FLARE_COL = Display::lerpColor565(RP::WARM, RP::FLUOR, 0.65f);
    const uint16_t TAIL_COL  = Display::lerpColor565(RP::SPARK, RP::WARM, 0.25f);

    // ==[ ROOM LIGHT FOR BUMP SHADING ]==
    PigLight carLight = selectRoom3PigKeyLight(0, 0, now);

    static constexpr int ROWS = 9;
    static constexpr int frontHW[ROWS] = {4, 7, 10, 13, 14, 14, 14, 13, 12};
    static constexpr int rearHW[ROWS]  = {6, 8, 11, 13, 14, 14, 14, 13, 12};
    static constexpr uint8_t faceZone[ROWS] = {0, 1, 1, 2, 2, 2, 3, 4, 4};
    static constexpr int SIDE_LEN = 41;
    static constexpr int sideTrimL[ROWS] = {13, 10, 6, 2, 0, 0, 0, 2, 4};
    static constexpr int sideTrimR[ROWS] = {13, 10, 6, 2, 0, 0, 0, 2, 4};
    static constexpr uint8_t sideZn[ROWS] = {0, 1, 1, 2, 2, 2, 2, 4, 4};
    static constexpr int ARCH_FL = 6, ARCH_FR = 11, ARCH_RL = 29, ARCH_RR = 34;

    auto q = [](int v) -> int { return v & ~3; };
    auto plot = [&](int x, int y, uint16_t c) {
        int sx = q(x), sy = q(y);
        if (sx < 0 || sx >= SCREEN_WIDTH || sy < kRoomY || sy > kFloorY + 4) return;
        canvas.fillRect(sx, sy, kRoomPX, kRoomPX, c);
    };
    auto lerp565 = [](uint16_t a, uint16_t b, float t) -> uint16_t {
        return Display::lerpColor565(a, b, t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t));
    };

    const int carCenterX = (int)WD_CAR_CENTER_X;
    const int carBaseY = (int)WD_CAR_BASE_Y;
    const int distantStartY = kRoomY + 64;  // way up in the sky, behind rain

    struct CarYawVisibility {
        float cosA;
        float sinRaw;
        float sinA;
        float faceScale;
        float faceLightAlpha;
        bool rear;
        bool sideRight;
        bool sideBand;
    };
    auto sampleCarYaw = [&](float yaw) -> CarYawVisibility {
        CarYawVisibility vis = {};
        vis.cosA = cosf(yaw);
        vis.sinRaw = sinf(yaw);
        vis.sinA = fabsf(vis.sinRaw);
        vis.faceScale = fabsf(vis.cosA);
        vis.rear = (vis.cosA < 0.0f);
        vis.sideRight = (vis.sinRaw >= 0.0f);
        vis.faceLightAlpha = Gfx::smoothstep01((vis.faceScale - 0.18f) / 0.14f);
        vis.sideBand = vis.faceLightAlpha < 0.05f;
        return vis;
    };

    // ==[ PHASE STATE COMPUTATION ]==
    // phase: 0=distant, 1=approach, 2=emerge, 3=hover, 4=turn, 5=linger, 6=accel, 7=cool
    int phase;
    float phaseT;       // 0..1 within current phase
    float phaseST;      // smoothstepped phaseT
    float scale = 1.0f;
    float bodyOpacity = 1.0f;
    float departOpacity = 1.0f;
    float lightIntensity = 1.0f;
    float angle = 0.0f;
    float bankV = 0.0f;
    int cx = carCenterX;
    int cy = carBaseY;

    if (s.wdReturnMode) {
        // arrival sequence: car approaches from distance, headlights facing player
        if (elapsed < WD_ARRIVE_MS) {
            float t = (float)elapsed / (float)WD_ARRIVE_MS;
            float st = t * t * (3.0f - 2.0f * t);  // smoothstep
            phase = 1;  // approach
            phaseT = t;
            scale = 0.10f + st * 0.90f;
            float bOpT = (st - 0.3f) / 0.3f;
            if (bOpT < 0.0f) bOpT = 0.0f;
            if (bOpT > 1.0f) bOpT = 1.0f;
            bodyOpacity = bOpT;
            lightIntensity = 0.3f + st * 0.7f;
            float yRange = (float)(carBaseY - distantStartY);
            cx = q(carCenterX);
            cy = q(distantStartY + (int)(st * yRange));
            float bob = sinf(t * PI * 2.0f) * (3.0f * (1.0f - st));
            cy += (int)bob;
            angle = 0.0f;  // front-facing — headlights toward player
        } else {
            phase = 3;  // hover
            phaseT = 1.0f;
            angle = 0.0f;
            cx = q(carCenterX);
            cy = q(carBaseY);
        }
    } else if (elapsed < DISTANT_MS) {
        phase = 0;
        phaseT = (float)elapsed / (float)DISTANT_MS;
        phaseST = phaseT * phaseT * (3.0f - 2.0f * phaseT);
        scale = 0.08f + phaseST * 0.02f;    // 0.08 → 0.10
        bodyOpacity = 0.0f;                   // NO body — just light
        lightIntensity = phaseST * 0.6f;
        cx = q(carCenterX);
        cy = q(distantStartY + (int)(phaseST * 20.0f));
    } else if (elapsed < APPROACH_END) {
        phase = 1;
        phaseT = (float)(elapsed - DISTANT_MS) / (float)(APPROACH_END - DISTANT_MS);
        phaseST = phaseT * phaseT * (3.0f - 2.0f * phaseT);
        scale = 0.10f + phaseST * 0.60f;     // 0.10 → 0.70
        float bOpT = (phaseT - 0.4f) / 0.6f;
        if (bOpT < 0.0f) bOpT = 0.0f;
        bodyOpacity = bOpT * bOpT * (3.0f - 2.0f * bOpT);
        lightIntensity = 0.6f + phaseST * 0.4f;
        float yRange = (float)(carBaseY - distantStartY);
        cx = q(carCenterX);
        cy = q(distantStartY + (int)(phaseST * yRange));
    } else if (elapsed < EMERGE_END) {
        phase = 2;
        phaseT = (float)(elapsed - APPROACH_END) / (float)(EMERGE_END - APPROACH_END);
        phaseST = phaseT * phaseT * (3.0f - 2.0f * phaseT);
        scale = 0.70f + phaseST * 0.30f;     // 0.70 → 1.0
        bodyOpacity = 1.0f;
        lightIntensity = 1.0f;
        float bob = sinf((float)elapsed / 900.0f) * 2.0f;
        cx = q(carCenterX);
        cy = q(carBaseY + (int)bob);
    } else if (elapsed < HOVER_END) {
        phase = 3;
        phaseT = (float)(elapsed - EMERGE_END) / (float)(HOVER_END - EMERGE_END);
        float sway = 8.0f * sinf((float)elapsed / 2500.0f * PI);
        float bob = sinf((float)elapsed / 800.0f) * 3.0f;
        cx = q(carCenterX + (int)sway);
        cy = q(carBaseY + (int)bob);
    } else if (elapsed < ROOM3_CAR_TURN_END_MS) {
        phase = 4;
        phaseT = (float)(elapsed - HOVER_END) / (float)(ROOM3_CAR_TURN_END_MS - HOVER_END);
        phaseST = phaseT * phaseT * (3.0f - 2.0f * phaseT);
        angle = phaseST * PI;
        bankV = sinf(phaseT * PI) * 0.55f;
        float dipPhase = phaseT < 0.4f ? (phaseT / 0.4f) : ((1.0f - phaseT) / 0.6f);
        float turnDip = dipPhase * dipPhase * (3.0f - 2.0f * dipPhase) * 18.0f;
        float hoverEndSway = 8.0f * sinf((float)HOVER_END / 2500.0f * PI);
        float swayDecay = (1.0f - phaseT) * (1.0f - phaseT);
        float turnDrift = sinf(phaseT * PI) * 12.0f;
        cx = q(carCenterX + (int)(hoverEndSway * swayDecay) + (int)turnDrift);
        cy = q(carBaseY + (int)turnDip);
    } else if (elapsed < LINGER_END) {
        phase = 5;  // post-turn hover with weight-shift tilt
        phaseT = (float)(elapsed - ROOM3_CAR_TURN_END_MS) / (float)(LINGER_END - ROOM3_CAR_TURN_END_MS);
        angle = PI;
        // tilt sequence: lean left → swing right → level out
        if (phaseT < 0.35f) {
            float t = phaseT / 0.35f;
            bankV = sinf(t * PI * 0.5f) * 0.45f;
        } else if (phaseT < 0.65f) {
            float t = (phaseT - 0.35f) / 0.3f;
            bankV = 0.45f * cosf(t * PI);
        } else {
            float t = (phaseT - 0.65f) / 0.35f;
            float ease = t * t * (3.0f - 2.0f * t);
            bankV = -0.45f * (1.0f - ease);
        }
        // Phase-local idle motion keeps the first linger frame anchored to the turn end pose.
        uint32_t lingerElapsed = elapsed - ROOM3_CAR_TURN_END_MS;
        float bob = sinf((float)lingerElapsed / 700.0f) * 2.0f;
        cx = q(carCenterX + (int)(sinf((float)lingerElapsed / 1200.0f) * 4.0f));
        cy = q(carBaseY + (int)bob);
    } else if (elapsed < ACCEL_END) {
        phase = 6;  // climb away
        phaseT = (float)(elapsed - LINGER_END) / (float)(ACCEL_END - LINGER_END);
        float yOff = (3.0f * phaseT * phaseT - 2.0f * phaseT * phaseT * phaseT) * 280.0f;
        cy = q(carBaseY - (int)yOff);
        scale = 1.0f - phaseT * phaseT * 0.95f;
        if (scale < 0.05f) scale = 0.05f;
        lightIntensity = 1.0f - phaseT * 0.5f;
        if (lightIntensity < 0.0f) lightIntensity = 0.0f;
        if (phaseT > 0.4f) {
            departOpacity = 1.0f - (phaseT - 0.4f) / 0.5f;
            if (departOpacity < 0.0f) departOpacity = 0.0f;
        }
        float depSway = sinf(phaseT * PI * 3.0f) * (8.0f * (1.0f - phaseT));
        cx = q(carCenterX + (int)depSway);
        float yawWobble = sinf(phaseT * PI * 4.0f) * 0.08f * (1.0f - phaseT);
        angle = PI + yawWobble;
    } else {
        phase = 7;  // trail dissipation + floor fade
        phaseT = (float)(elapsed - ACCEL_END) / (float)(COOL_END - ACCEL_END);
        bodyOpacity = 0.0f;
        lightIntensity = 0.0f;
        cx = q(carCenterX);
        cy = q(carBaseY);
    }

    // ==[ WD MODE: pig walks to car, jumps onto roof, car bounces ]==
    if (s.wdMode || s.wdReturnMode) {
        // car bounce: damped spring after pig lands
        cy += (int)s.wdCarDropY;

        // trigger pig walk toward car on the turn beat
        if (s.wdMode && phase == 4 && wdMountPhase == WDMountPhase::IDLE) {
            wdMountPhase = WDMountPhase::WALKING;
            wdMountStart = now;
            wdMountFromX = pigX;
            wdMountFromY = pigY;
            faceRight = true;  // face the car
        }

        // pig landed on roof → damped bounce: drop 10px, spring back
        if (s.wdPigMounted) {
            uint32_t bounceRaw = now - wdImpactStart;
            uint32_t bounceElapsed = (s.wdMode && isWDMenuCineFast())
                ? wdMenuCineElapsed(bounceRaw) : bounceRaw;
            float bt = (float)bounceElapsed / 800.0f;  // 800ms total bounce
            if (bt >= 1.0f) {
                s.wdCarDropY = 0.0f;  // settled
            } else {
                // damped spring: A * sin(ωt) * e^(-δt)
                float bounce = 10.0f * sinf(bt * 3.14159f * 3.0f) * expf(-bt * 4.0f);
                s.wdCarDropY = bounce;
            }
        }

    }

    // ==[ DENSE RAIN CURTAIN ]== extra rain in car's region during distant/approach/emerge
    auto drawRainCurtain = [&]() {
        if (phase > 2) return;  // only distant, approach, emerge
        int density;
        float rainAlpha;
        if (phase == 0) {
            density = 30;
            rainAlpha = 1.0f;
        } else if (phase == 1) {
            density = 30 - (int)(phaseT * 22.0f);
            rainAlpha = 1.0f - phaseT * 0.6f;
        } else {
            density = 8 - (int)(phaseT * 7.0f);
            rainAlpha = 0.4f - phaseT * 0.4f;
            if (rainAlpha < 0.0f) rainAlpha = 0.0f;
        }
        int headlightY = cy + (int)(6.0f * (float)kRoomPX * scale);
        int headlightSpread = (int)(20.0f * scale) * kRoomPX;
        const uint16_t waterBody = lerp565(RP::SHAFT, RP::PUDDLE, 0.28f);
        for (int i = 0; i < density; i++) {
            uint32_t cycle = 400u + (uint32_t)i * 35u;
            float ph = (float)((now + (uint32_t)i * 97u + 5000u) % cycle) / (float)cycle;
            // rain owns fixed 4px lanes. vertical motion reads as weather;
            // horizontal re-rolls read as RF snow.
            int rainX = cx - 80 + (int)(((wallHash(i, 0, 0xDA01u) & 0xFF) / 255.0f) * 160.0f);
            rainX = q(rainX);
            int rainY = q(kRoomY + (int)(ph * (float)(kFloorY - kRoomY - 8)));
            if (rainY >= kFloorY - 4) continue;
            // Check if raindrop is in headlight cone
            int dx = abs(rainX - cx);
            int dy = rainY - headlightY;
            bool inCone = (dy > 0 && dy < 80 && dx < headlightSpread + (int)((float)dy * 0.5f));
            uint16_t dropCol;
            if (inCone && lightIntensity > 0.2f) {
                float litT = lightIntensity * (1.0f - (float)dx / (float)(headlightSpread + (int)((float)dy * 0.5f) + 1));
                if (litT < 0.0f) litT = 0.0f;
                if (litT > 1.0f) litT = 1.0f;
                litT *= 0.7f;
                dropCol = lerp565(waterBody, FLARE_COL, litT);
            } else {
                dropCol = waterBody;
            }
            if (rainAlpha < 1.0f) {
                if ((int)(wallHash(i, 0, 0xDD01u) & 0xFF) > (int)(rainAlpha * 255.0f))
                    continue;
            }
            plot(rainX, rainY, dropCol);
            if (rainY > kRoomY + kRoomPX) {
                plot(rainX, rainY - kRoomPX, lerp565(dropCol, RP::BG, 0.5f));
            }
        }
    };

    // ==[ DISTANT HEADLIGHT GLOW ]== two warm points + cones through rain
    auto drawDistantHeadlights = [&]() {
        if (lightIntensity < 0.05f) return;
        int fHW = (int)(14.0f * scale);
        int lightY = cy + (int)(6.0f * (float)kRoomPX * scale);
        int lightLX = q(cx - fHW * kRoomPX);
        int lightRX = q(cx + fHW * kRoomPX);
        float pulse = 1.0f + 0.15f * sinf((float)elapsed / 700.0f);
        float eff = lightIntensity * pulse;
        if (eff > 1.0f) eff = 1.0f;

        for (int side = -1; side <= 1; side += 2) {
            int lx = (side < 0) ? lightLX : lightRX;
            // Core light point
            if (eff > 0.3f) plot(q(lx), q(lightY), FLARE_COL);
            // Radial bloom
            int bloomR = 2 + (int)(lightIntensity * 4.0f);
            for (int ri = 0; ri < 10; ri++) {
                float rA = (float)ri * 0.6283f;
                for (int rd = 1; rd <= bloomR; rd++) {
                    float rDist = (float)rd * (float)kRoomPX * (1.0f + 0.2f * sinf((float)elapsed / 500.0f));
                    int bx = lx + (int)(cosf(rA) * rDist);
                    int by = lightY + (int)(sinf(rA) * rDist * 0.6f);
                    if (by <= kRoomY || by >= kFloorY) continue;
                    int thresh = (int)(eff * (float)(220 - rd * 40));
                    if ((int)(wallHash(bx, by, elapsed / 120 + ri) & 0xFF) < thresh)
                        plot(q(bx), q(by), rd == 1 ? FLARE_COL : lerp565(RP::BG, RP::WARM, eff * 0.6f));
                }
            }
            // Cross flare
            int crossLen = 2 + (int)(lightIntensity * 5.0f);
            for (int ci = 1; ci <= crossLen; ci++) {
                float alpha = (1.0f - (float)ci / (float)(crossLen + 1)) * eff * 0.4f;
                uint16_t crossCol = lerp565(RP::BG, FLARE_COL, alpha);
                plot(q(lx + ci * kRoomPX), q(lightY), crossCol);
                plot(q(lx - ci * kRoomPX), q(lightY), crossCol);
                plot(q(lx), q(lightY + ci * kRoomPX), crossCol);
                plot(q(lx), q(lightY - ci * kRoomPX), crossCol);
            }
            // Light cone downward (volumetric through rain)
            int coneLen = 6 + (int)(lightIntensity * 10.0f);
            for (int cRow = 1; cRow < coneLen; cRow++) {
                int coneY = lightY + cRow * kRoomPX;
                if (coneY >= kFloorY) break;
                float coneT = (float)cRow / (float)coneLen;
                int spread = (int)((2.0f + coneT * 8.0f) * (scale > 0.3f ? scale : 0.3f));
                float cAlpha = (1.0f - coneT * coneT) * eff * 0.3f;
                int cThresh = (int)(cAlpha * 255.0f);
                for (int ox = -spread; ox <= spread; ox += kRoomPX) {
                    if ((int)(wallHash(lx + ox, coneY, 0xCE01u + cRow + elapsed / 200) & 0xFF) < cThresh)
                        plot(q(lx + ox), q(coneY), lerp565(RP::BG, RP::WARM, cAlpha * 0.7f));
                }
            }
        }
        // Atmospheric scatter motes
        int scatterCount = 4 + (int)(lightIntensity * 10.0f);
        int scatterW = (int)(40.0f * scale);
        for (int mi = 0; mi < scatterCount; mi++) {
            uint32_t moteCyc = 900u + (uint32_t)mi * 130u;
            float motePh = (float)((elapsed + (uint32_t)mi * 400u) % moteCyc) / (float)moteCyc;
            int mx = cx - scatterW + (int)(((wallHash(mi, elapsed / 800, 0xAE01u) & 0xFF) / 255.0f) * (float)(scatterW * 2));
            int my = cy - (int)(10.0f * scale) + (int)(motePh * (float)(ROWS * kRoomPX) * scale + 40.0f);
            if (my <= kRoomY || my >= kFloorY || motePh >= 0.8f) continue;
            float mAlpha = (1.0f - motePh / 0.8f) * eff * 0.25f;
            plot(q(mx), q(my), lerp565(RP::BG, FLARE_COL, mAlpha));
        }
        // Floor wash (faint at distance)
        int washW = (int)(60.0f * scale + 30.0f * lightIntensity);
        for (int gx = cx - washW; gx <= cx + washW; gx += kRoomPX) {
            float dist = fabsf((float)(gx - cx)) / (float)(washW > 0 ? washW : 1);
            if (dist >= 1.0f) continue;
            int thresh = (int)((1.0f - dist * dist) * eff * 50.0f);
            if ((int)(wallHash(gx, kFloorY, 0x6101u + elapsed / 300) & 0xFF) < thresh)
                plot(q(gx), kFloorY - kRoomPX, lerp565(RP::BG, SPORT_GLOW, eff * 0.3f));
        }
    };

    // ==[ HAZE OCCLUSION ]== overlay noise on car body during approach
    auto drawHazeOverlay = [&](float hazeLevel) {
        if (hazeLevel <= 0.05f) return;
        int bodyW = (int)(28.0f * scale) * kRoomPX;
        int bodyH = (int)((float)ROWS * (float)kRoomPX * scale);
        int thresh = (int)(hazeLevel * 120.0f);
        for (int hy = (cy > kRoomY ? cy : kRoomY); hy < cy + bodyH && hy < kFloorY; hy += kRoomPX) {
            for (int hx = cx - bodyW / 2; hx < cx + bodyW / 2; hx += kRoomPX) {
                if ((int)(wallHash(hx, hy, elapsed / 150 + 0xE201u) & 0xFF) < thresh)
                    plot(q(hx), q(hy), lerp565(RP::BG, RP::WALL_FAR, 0.5f));
            }
        }
    };

    auto drawFloorGlow = [&](int gxCenter, float intensity) {
        if (intensity <= 0.01f) return;
        for (int gx = gxCenter - 84; gx <= gxCenter + 84; gx += kRoomPX) {
            float d = fabsf((float)(gx - gxCenter)) / 84.0f;
            if (d >= 1.0f) continue;
            int thr = (int)((1.0f - d * d) * intensity * 110.0f);
            if ((int)(wallHash(gx, kFloorY, 0x6101u) & 0xFF) < thr)
                plot(gx, kFloorY - kRoomPX, SPORT_GLOW);
        }
    };

    auto drawParticles = [&](int bodyX, int bodyY, float intensity, bool accelPhase) {
        if (intensity > 0.4f && !accelPhase) {
            for (int i = 0; i < 5; i++) {
                uint32_t cyc = 500u + (uint32_t)i * 170u;
                float ph = (float)((elapsed + (uint32_t)i * 220u) % cyc) / (float)cyc;
                if (ph < 0.7f) {
                    int dx = bodyX - 40 + i * 20 +
                             (((int)(wallHash(i, 0, elapsed / 260) & 0x03u) - 1) * kRoomPX);
                    int dy = bodyY + 36 + 8 + (int)(ph * ph * 60.0f);
                    if (dy < kFloorY) plot(dx, dy, RP::SHAFT);
                }
            }
        }
        for (int i = 0; i < 4; i++) {
            uint32_t cyc = 700u + (uint32_t)i * 200u;
            float ph = (float)((elapsed + (uint32_t)i * 300u) % cyc) / (float)cyc;
            if (ph >= 0.7f) continue;
            int vx = bodyX - 30 + i * 20 + (int)(sinf((float)elapsed / 1200.0f + (float)i) * 6.0f);
            int vy = bodyY - 8 - (int)(ph * 40.0f);
            if (vy > kRoomY) plot(vx, vy, lerp565(RP::DUST, RP::BG, ph));
        }
    };

    auto drawCarBody = [&](int centerX, int baseY, float yaw, float bank, float scl, float opacity) {
        CarYawVisibility yawVis = sampleCarYaw(yaw);
        float cosA = yawVis.cosA;
        float sinRaw = yawVis.sinRaw;
        float sinA = yawVis.sinA;
        float fScale = yawVis.faceScale;
        bool isRear = yawVis.rear;
        bool sideRight = yawVis.sideRight;
        float maxBank = 12.0f * bank * scl;
        int maxVisW = 0, roofTW = 0;
        const int* fHWs = isRear ? rearHW : frontHW;

        // ==[ BUMP LIGHT DIRECTION ]== room light → car center
        float faceDot = 0.0f, sideDotR = 0.0f;
        if (carLight.tint != 0) {
            float ldx = (float)(carLight.x - centerX);
            float ldy = (float)(carLight.y - (baseY + ROWS * kRoomPX / 2));
            float ldz = 80.0f;
            float lmag = sqrtf(ldx * ldx + ldy * ldy + ldz * ldz);
            if (lmag < 1.0f) lmag = 1.0f;
            ldx /= lmag; ldy /= lmag; ldz /= lmag;
            faceDot = sinRaw * ldx + cosA * ldz;     // face normal · light dir
            sideDotR = cosA * ldx - sinRaw * ldz;    // right-side normal · light dir
        }

        for (int row = 0; row < ROWS; row++) {
            int y = baseY + (int)((float)row * (float)kRoomPX * scl);
            float fHWf = fHWs[row] * fScale * scl;
            int fHW = (int)lroundf(fHWf);
            // Keep a minimum face only when we're outside the clean side band.
            if (!yawVis.sideBand && fScale > 0.03f && fHW == 0 && fHWs[row] > 0) fHW = 1;
            // Edge darken: only when we rounded UP (included a partial column)
            float fEdgeDarken = ((float)fHW > fHWf) ? ((float)fHW - fHWf) * 0.8f : 0.0f;
            int fW = fHW > 0 ? (fHW * 2 + 1) : 0;
            int tL = sideTrimL[row], tR = sideTrimR[row];
            int visSide = SIDE_LEN - tL - tR;
            float sWf = (float)visSide * sinA * scl;
            int sW = (int)lroundf(sWf);
            if (sW < 0) sW = 0;
            float sEdgeDarken = ((float)sW > sWf) ? ((float)sW - sWf) * 0.8f : 0.0f;
            int totalW = fW + sW;
            if (totalW <= 0) continue;
            if (totalW > maxVisW) maxVisW = totalW;
            if (row == 0) roofTW = totalW;

            float rowHW = (float)totalW * 0.5f;
            if (rowHW < 0.5f) rowHW = 0.5f;
            int rowStart = -(totalW / 2);
            int faceStart, sideStart;
            if (sideRight) { faceStart = rowStart; sideStart = rowStart + fW; }
            else           { sideStart = rowStart; faceStart = rowStart + sW; }

            auto emitPx = [&](int screenCol, uint16_t color) {
                int px = q(centerX + screenCol * kRoomPX);
                float colFrac = (float)screenCol / rowHW;
                int py = q(y + (int)(colFrac * maxBank));
                if (opacity < 1.0f) {
                    float reveal = (float)(ROWS - 1 - row) / (float)ROWS;
                    if (reveal > opacity * 1.2f) return;
                    if (reveal > opacity * 0.9f &&
                        (int)(wallHash(px, py, elapsed / 80 + 0xCA01u) & 0xFF) > 140) return;
                }
                plot(px, py, color);
            };

            if (sW > 0) {
                for (int si = 0; si < sW; si++) {
                    int mapIdx = sideRight ? si : (sW - 1 - si);
                    int fullCol = tL + (sW > 1 ? (mapIdx * (visSide - 1) / (sW - 1)) : 0);
                    if (fullCol >= SIDE_LEN - tR) continue;
                    uint8_t zone = sideZn[row];
                    uint16_t c;
                    if (zone == 0)      c = SPORT_DARK;
                    else if (zone == 1) c = lerp565(RP::STRUCT, RP::DEEP, 0.5f);
                    else                c = SPORT_SIDE;
                    float faceDist = sideRight
                        ? ((float)si / (float)(sW > 1 ? sW - 1 : 1))
                        : (1.0f - (float)si / (float)(sW > 1 ? sW - 1 : 1));
                    c = lerp565(c, SPORT_DARK, faceDist * 0.35f);
                    // Side specular highlight band
                    float sideT = (float)mapIdx / (float)(visSide > 1 ? visSide - 1 : 1);
                    if (sideT > 0.3f && sideT < 0.6f && row >= 3 && row <= 6) {
                        float specBand = 1.0f - fabsf(sideT - 0.45f) / 0.15f;
                        if (specBand > 0.0f) c = lerp565(c, SPORT_LIT, specBand * 0.3f);
                    }
                    if (zone >= 2) {
                        c = lerp565(c, SPORT_GLOW, 0.12f + faceDist * 0.08f);
                        int bv = (int)(wallHash(fullCol * 7 + row * 13, elapsed / 2000, 0xBE02u) & 0xFF);
                        if (bv > 180) c = lerp565(c, SPORT_LIT, (float)(bv - 180) / 280.0f);
                    }
                    if (row >= 7 && ((fullCol >= ARCH_FL && fullCol <= ARCH_FR) ||
                                    (fullCol >= ARCH_RL && fullCol <= ARCH_RR)))
                        c = SPORT_DARK;
                    if (zone == 1) {
                        float dt = (float)(fullCol - tL) / (float)(visSide > 1 ? visSide - 1 : 1);
                        if (dt > 0.3f && dt < 0.7f) c = lerp565(RP::DEEP, RP::STRUCT, 0.3f);
                    }
                    // ==[ SIDE BUMP ]== room light interaction
                    if (carLight.tint != 0) {
                        float sDot = sideRight ? sideDotR : -sideDotR;
                        float sCurve = 1.0f - (faceDist - 0.5f) * (faceDist - 0.5f) * 2.0f;
                        float bDot = sDot * sCurve;
                        if (bDot > 0.08f) {
                            float bt = (bDot - 0.08f) * 1.4f;
                            if (bt > 1.0f) bt = 1.0f;
                            c = screenBlend565(c, carLight.tint, (uint8_t)(bt * 0.35f * 255.0f));
                        } else if (bDot < -0.05f) {
                            float bt = (-bDot - 0.05f) * 2.0f;
                            if (bt > 1.0f) bt = 1.0f;
                            c = lerp565(c, RP::DEEP, bt * 0.25f);
                        }
                    }
                    // Anti-alias outermost side column when rounded up
                    if (si == sW - 1 && sEdgeDarken > 0.01f)
                        c = lerp565(c, SPORT_DARK, sEdgeDarken);
                    emitPx(sideStart + si, c);
                }
            }

            if (fW > 0) {
                for (int fi = 0; fi < fW; fi++) {
                    int faceCol = fi - fHW;
                    uint8_t zone = faceZone[row];
                    uint16_t c;
                    if (zone == 0)      c = SPORT_DARK;
                    else if (zone == 1) c = RP::DEEP;
                    else if (zone == 2) c = SPORT_BODY;
                    else if (zone == 3) c = RP::DEEP;
                    else                c = SPORT_BODY;
                    if ((zone == 2 || zone == 4) && fHW > 3) {
                        int specPos = (int)(sinRaw * (float)fHW * 0.4f);
                        int specDist = abs(faceCol - specPos);
                        if (specDist <= 2)
                            c = lerp565(c, SPORT_LIT, (1.0f - (float)specDist * 0.5f) * 0.5f);
                    }
                    if (zone == 2 || zone == 4) {
                        int bv = (int)(wallHash(faceCol * 7 + row * 13, elapsed / 2000, 0xBE01u) & 0xFF);
                        if (bv > 180) c = lerp565(c, SPORT_LIT, (float)(bv - 180) / 210.0f);
                        else if (bv < 75) c = lerp565(c, SPORT_DARK, (float)(75 - bv) / 250.0f);
                    }
                    if ((zone == 2 || zone == 4) && fHW > 2) {
                        float edgeT = fabsf((float)faceCol) / (float)fHW;
                        if (edgeT > 0.85f) c = lerp565(c, RP::DEEP, 0.5f);
                        else if (edgeT > 0.7f) c = lerp565(c, SPORT_DARK, 0.3f);
                    }
                    if (zone == 3 && fHW > 3) {
                        bool edgeBand = abs(faceCol) >= fHW - 3;
                        uint16_t grillBase = (abs(faceCol) % 3 == 0) ? RP::STRUCT : RP::DEEP;
                        if (edgeBand) {
                            bool seamR = sideRight && (sW > 0);
                            bool seamL = !sideRight && (sW > 0);
                            bool seamEdge = (seamR && faceCol > 0) || (seamL && faceCol < 0);
                            if (seamEdge || yawVis.sideBand)
                                c = grillBase;
                            else
                                c = lerp565(grillBase, isRear ? TAIL_COL : FLARE_COL,
                                            yawVis.faceLightAlpha);
                        } else {
                            c = grillBase;
                        }
                    }
                    // ==[ FACE BUMP ]== room light interaction (skip grill zone — it's self-lit)
                    if (carLight.tint != 0 && zone != 3) {
                        float colT = (float)faceCol / (float)(fHW > 0 ? fHW : 1);
                        float bDot = faceDot * (1.0f - colT * colT * 0.4f);
                        if (bDot > 0.08f) {
                            float bt = (bDot - 0.08f) * 1.4f;
                            if (bt > 1.0f) bt = 1.0f;
                            c = screenBlend565(c, carLight.tint, (uint8_t)(bt * 0.35f * 255.0f));
                        } else if (bDot < -0.05f) {
                            float bt = (-bDot - 0.05f) * 2.0f;
                            if (bt > 1.0f) bt = 1.0f;
                            c = lerp565(c, RP::DEEP, bt * 0.25f);
                        }
                    }
                    // Anti-alias outermost face columns when rounded up
                    if ((fi == 0 || fi == fW - 1) && fEdgeDarken > 0.01f)
                        c = lerp565(c, SPORT_DARK, fEdgeDarken);
                    emitPx(faceStart + fi, c);
                }
            }
        }

        if (opacity > 0.4f && maxVisW > 0) {
            float half = (float)maxVisW * 0.5f;
            int glowY = baseY + (int)((float)ROWS * (float)kRoomPX * scl);
            for (int i = 0; i < maxVisW; i++) {
                int gx = q(centerX + (i - (int)half) * kRoomPX);
                float frac = ((float)i - half) / (half > 0.5f ? half : 1.0f);
                int gy = q(glowY + (int)(frac * maxBank));
                if ((wallHash(gx, gy, 0xB0D1u) & 0xFF) < 140) plot(gx, gy, SPORT_GLOW);
            }
        }

        if (sinA > 0.15f && opacity > 0.6f && roofTW > 0) {
            float half = (float)roofTW * 0.5f;
            int ry = baseY - (int)((float)kRoomPX * scl);
            for (int i = 0; i < roofTW; i++) {
                float frac = ((float)i - half) / (half > 0.5f ? half : 1.0f);
                plot(q(centerX + (i - (int)half) * kRoomPX), q(ry + (int)(frac * maxBank)), SPORT_ROOF);
            }
        }

        if (opacity > 0.5f && scl > 0.5f && fScale > 0.5f && maxVisW > 0) {
            int mirY = baseY + (int)(3.0f * (float)kRoomPX * scl);
            int mirOff = (int)(((float)maxVisW * 0.5f + 1.0f) * (float)kRoomPX);
            if (mirOff >= 8) {
                plot(q(centerX - mirOff), q(mirY), RP::STRUCT);
                plot(q(centerX + mirOff), q(mirY), RP::STRUCT);
            }
        }
    };

    // ==[ FULL VOLUMETRIC LIGHTING ]== (used emerge/hover/turn/accel)
    auto drawCarLighting = [&](int centerX, int centerY, float yaw, float scl, float opacity) {
        if (opacity < 0.3f) return;
        CarYawVisibility yawVis = sampleCarYaw(yaw);
        float sinA = yawVis.sinA;
        float fScl = yawVis.faceScale;
        bool rear = yawVis.rear;
        int cH = (int)((float)ROWS * (float)kRoomPX * scl);

        if (yawVis.faceLightAlpha > 0.05f) {
            int grillY = centerY + (int)(6.0f * (float)kRoomPX * scl);
            int fHW = (int)(14.0f * fScl * scl);
            uint16_t lightCol = rear ? TAIL_COL : FLARE_COL;
            uint16_t dimCol = rear ? lerp565(TAIL_COL, RP::BG, 0.5f) : lerp565(FLARE_COL, RP::BG, 0.4f);
            for (int side = -1; side <= 1; side += 2) {
                int lxOff = (fHW > 2) ? (fHW - 2) : 0;
                int lx = centerX + side * lxOff * kRoomPX;
                for (int ri = 0; ri < 10; ri++) {
                    float rA = (float)ri * 0.6283f;
                    for (int rd = 1; rd <= 3; rd++) {
                        float rDist = (float)rd * (float)kRoomPX * (1.0f + 0.2f * sinf((float)elapsed / 500.0f));
                        int rx = lx + (int)(cosf(rA) * rDist);
                        int ry = grillY + (int)(sinf(rA) * rDist * 0.6f);
                        if (ry <= kRoomY || ry >= kFloorY) continue;
                        int thresh = (int)(fScl * opacity * yawVis.faceLightAlpha *
                                           (float)(200 - rd * 50));
                        if ((int)(wallHash(rx, ry, elapsed / 120 + ri) & 0xFF) < thresh)
                            plot(q(rx), q(ry), rd == 1 ? lightCol : dimCol);
                    }
                }
                int crossLen = (int)(3.0f * fScl * scl);
                for (int ci = 1; ci <= crossLen; ci++) {
                    if ((wallHash(lx + ci * kRoomPX, grillY, 0xCB01u) & 0xFF) <
                        (int)(fScl * yawVis.faceLightAlpha * 200.0f)) {
                        plot(q(lx + ci * kRoomPX), q(grillY), dimCol);
                        plot(q(lx - ci * kRoomPX), q(grillY), dimCol);
                    }
                    if ((wallHash(lx, grillY + ci * kRoomPX, 0xCB02u) & 0xFF) <
                        (int)(fScl * yawVis.faceLightAlpha * 160.0f)) {
                        plot(q(lx), q(grillY + ci * kRoomPX), dimCol);
                        plot(q(lx), q(grillY - ci * kRoomPX), dimCol);
                    }
                }
            }
        }

        // Side ambient glow — bridges the 90° lighting gap
        if (sinA > 0.3f && opacity > 0.3f) {
            float sideGlow = sinA * opacity * 0.4f;
            int bodyMidY = centerY + cH / 2;
            int sideW = (int)(14.0f * sinA * scl) * kRoomPX;
            float sinRaw_ = sinf(yaw);
            int sideDir = (sinRaw_ >= 0.0f) ? 1 : -1;
            int sideEdge = centerX + sideDir * sideW;
            for (int gi = 0; gi < 6; gi++) {
                int gx = sideEdge + sideDir * gi * kRoomPX;
                for (int gy = centerY; gy < centerY + cH; gy += kRoomPX) {
                    if (gy <= kRoomY || gy >= kFloorY) continue;
                    float falloff = 1.0f - (float)gi / 6.0f;
                    int sThresh = (int)(sideGlow * falloff * 120.0f);
                    if ((int)(wallHash(gx, gy, elapsed / 200 + 0xAD01u) & 0xFF) < sThresh)
                        plot(q(gx), q(gy), lerp565(RP::BG, SPORT_GLOW, sideGlow * falloff * 0.6f));
                }
            }
        }

        if (yawVis.faceLightAlpha > 0.05f) {
            int coneTop = centerY + cH;
            int coneLen = (int)(44.0f * scl * opacity);
            uint16_t coneBase = rear ? TAIL_COL : FLARE_COL;
            for (int ci = kRoomPX; ci < coneLen; ci += kRoomPX) {
                if (coneTop + ci >= kFloorY) break;
                float t = (float)ci / (float)coneLen;
                int spread = (int)((3.0f + t * 12.0f) * scl * fScl);
                int intensity = (int)((1.0f - t * t) * opacity * yawVis.faceLightAlpha *
                                      fScl * (rear ? 55.0f : 85.0f));
                for (int ox = -spread; ox <= spread; ox += kRoomPX) {
                    if ((wallHash(centerX + ox, coneTop + ci, elapsed / 260 + 0xAC01u) & 0xFF) < intensity)
                        plot(q(centerX + ox), q(coneTop + ci), lerp565(coneBase, RP::DUST, t * t));
                }
            }
        }

        float envelopeScl = fScl > sinA ? fScl : sinA;
        int scatterR = (int)(52.0f * scl * envelopeScl);
        for (int si = 0; si < 14; si++) {
            float sPhase = (float)elapsed / 1400.0f + (float)si * 0.5f;
            int sx = centerX + (int)(cosf(sPhase) * (float)scatterR * (0.3f + 0.7f * sinf((float)si * 1.3f)));
            int sy = centerY + cH / 2 + (int)(sinf(sPhase * 0.7f) * (float)scatterR * 0.3f);
            if (sy <= kRoomY || sy >= kFloorY) continue;
            int dx_ = sx - centerX, dy_ = sy - centerY - cH / 2;
            float sDist = (float)(dx_ * dx_ + dy_ * dy_) / (float)(scatterR * scatterR + 1);
            int sInt = (int)((1.0f - sDist) * opacity * envelopeScl * 45.0f);
            if (sInt > 0 && (wallHash(sx, sy, elapsed / 180 + si * 7) & 0xFF) < sInt)
                plot(q(sx), q(sy), lerp565(SPORT_GLOW, RP::DUST, (float)(si & 3) * 0.25f));
        }

        if (!yawVis.sideBand) {
            float washEnv = fScl > sinA ? fScl : sinA;
            if (washEnv < 0.3f) washEnv = 0.3f;
            int washW = (int)(92.0f * scl * washEnv);
            for (int gx = centerX - washW; gx <= centerX + washW; gx += kRoomPX) {
                float dist = fabsf((float)(gx - centerX)) / (float)washW;
                if (dist >= 1.0f) continue;
                float base = (1.0f - dist * dist) * opacity * washEnv;
                for (int rowOff = 0; rowOff < 28; rowOff += kRoomPX) {
                    int gy = kFloorY - rowOff;
                    if (gy <= kRoomY) continue;
                    float ri = base * (1.0f - (float)rowOff / 28.0f);
                    if ((wallHash(gx, gy, elapsed / 320 + 0x6105u + rowOff) & 0xFF) < (int)(ri * 120.0f))
                        plot(q(gx), q(gy), lerp565(SPORT_GLOW, RP::DUST, (float)rowOff / 16.0f + dist * 0.3f));
                }
            }
        }
    };

    auto drawSideVapor = [&](float scl, float vaporIntensity) {
        if (vaporIntensity < 0.1f) return;
        int halfW = (int)(14.0f * scl) * kRoomPX;
        int bodyMidY = q(cy) + (int)(4.5f * (float)kRoomPX * scl);
        for (int vi = 0; vi < 10; vi++) {
            uint32_t vapCyc = 600u + (uint32_t)vi * 140u;
            float vapPh = (float)((elapsed + vi * 250u) % vapCyc) / (float)vapCyc;
            if (vapPh >= 0.8f) continue;
            int side = (vi & 1) ? 1 : -1;
            int vSize = 1 + (int)(wallHash(vi, elapsed / 500, 0xA501u) & 1);
            int vx = q(cx) + side * (halfW + 4 + (int)(vapPh * 28.0f * scl));
            int vy = bodyMidY + (int)(sinf(vapPh * PI) * 6.0f) - (int)(vapPh * 16.0f);
            if (vy <= kRoomY || vy >= kFloorY) continue;
            float vOp = (1.0f - vapPh / 0.8f) * vaporIntensity;
            uint16_t vCol = lerp565(RP::DUST, RP::BG, vapPh);
            for (int dx = 0; dx < vSize; dx++) {
                for (int dy = 0; dy < vSize; dy++) {
                    if ((wallHash(vx + dx * kRoomPX, vy + dy * kRoomPX, vi + 0xA502u) & 0xFF) < (int)(vOp * 200.0f))
                        plot(q(vx + dx * kRoomPX), q(vy + dy * kRoomPX), vCol);
                }
            }
        }
    };

    // ==[ HEADLIGHT SCENE CAST ]== lights illuminate surrounding surfaces via screenBlend
    auto castHeadlightWash = [&](int carCX, int carCY, float yaw_, float scl_, float intensity_) {
        if (intensity_ < 0.15f) return;
        CarYawVisibility yawVis = sampleCarYaw(yaw_);
        float fScl_ = yawVis.faceScale;
        if (yawVis.sideBand || yawVis.faceLightAlpha <= 0.05f) return;
        bool rear_ = yawVis.rear;
        int hlY = carCY + (int)(6.0f * (float)kRoomPX * scl_);
        int fHW_ = (int)(14.0f * fScl_ * scl_);
        uint16_t hlTint = rear_ ? TAIL_COL : FLARE_COL;
        float fSclClamped = fScl_ > 0.1f ? fScl_ : 0.1f;
        float hlStr = intensity_ * yawVis.faceLightAlpha *
                      (rear_ ? 0.25f : 0.45f) * fSclClamped;
        // Cast from each headlight onto floor + nearby area
        for (int side = -1; side <= 1; side += 2) {
            PigLight hl;
            int hlOff = (fHW_ > 1) ? fHW_ : 1;
            hl.x = (int16_t)(carCX + side * hlOff * kRoomPX);
            hl.y = (int16_t)hlY;
            hl.tint = hlTint;
            // Floor wash — rect extends well beyond maxRadius so falloff dithers to zero
            int washX0 = carCX - (int)(180.0f * scl_);
            int washW  = (int)(360.0f * scl_);
            drawFurnitureWash(canvas, washX0, kFloorY - 32, washW, 40, hl, 140.0f * scl_, hlStr);
            // Parapet wall wash — large rect, tight radius for natural dither fade
            drawFurnitureWash(canvas, 0, kFloorY - 52, 180, 56, hl, 120.0f * scl_, hlStr * 0.4f);
        }
    };

    // ==[ AKIRA BACKLIGHT TRAILS ]== organic curves from 2 taillights
    auto drawAkiraTrails = [&](float accelProgress, float trailAlpha) {
        if (trailAlpha < 0.02f || accelProgress < 0.02f) return;
        const int N = 24;
        for (int side = -1; side <= 1; side += 2) {
            // per-side wobble — different freqs so trails aren't parallel
            float wF1 = (side < 0) ? 5.3f : 6.7f;
            float wP1 = (side < 0) ? 0.0f : 2.1f;
            float wF2 = (side < 0) ? 2.1f : 1.7f;
            for (int seg = 1; seg < N; seg++) {
                float segFrac = (float)seg / (float)(N - 1);  // 0=car, 1=oldest
                float pastT = accelProgress * (1.0f - segFrac);
                // reconstruct car position along accel trajectory
                float pastYOff = (3.0f * pastT * pastT - 2.0f * pastT * pastT * pastT) * 280.0f;
                int segCY = carBaseY - (int)pastYOff;
                float pastScale = 1.0f - pastT * pastT * 0.95f;
                if (pastScale < 0.05f) pastScale = 0.05f;
                float pastSway = sinf(pastT * PI * 3.0f) * (8.0f * (1.0f - pastT));
                int segCX = carCenterX + (int)pastSway;
                // taillight offset from car center
                int fHW = (int)(14.0f * pastScale);
                int tlX = segCX + side * (fHW - 2) * kRoomPX;
                int tlY = segCY + (int)((float)ROWS * (float)kRoomPX * pastScale);
                // organic wobble — two overlapping sines, amplitude grows toward tail
                float wobAmp = segFrac * segFrac;
                float wobble = sinf(segFrac * wF1 + wP1 + (float)elapsed / 500.0f)
                               * (2.0f + wobAmp * 8.0f)
                             + sinf(segFrac * wF2 + (float)elapsed / 800.0f)
                               * (1.0f + wobAmp * 4.0f);
                tlX += (int)wobble;
                // trail intensity — bright near car, dim at tail
                float fadeA = (1.0f - segFrac * segFrac) * trailAlpha;
                fadeA *= (0.5f + accelProgress * 0.5f);
                if (fadeA < 0.02f) continue;
                int sy = q(tlY);
                if (sy >= kFloorY || sy <= kRoomY) continue;
                // width: 2 fat px near car, 1 at tail, dithered edges
                int spread = (segFrac < 0.5f) ? 1 : 0;
                for (int dx = -spread; dx <= spread; dx++) {
                    int sx = q(tlX + dx * kRoomPX);
                    float dFade = (spread > 0) ? (float)abs(dx) / 2.0f : 0.0f;
                    float pixA = fadeA * (1.0f - dFade * 0.5f);
                    if ((int)(wallHash(sx, sy, elapsed / 90 + seg * 3
                             + (side + 2) * 37 + 0xAE01u) & 0xFF)
                        < (int)(pixA * 210.0f))
                        plot(sx, sy, lerp565(TAIL_COL, RP::BG,
                             segFrac * 0.55f + dFade * 0.2f));
                }
            }
        }
    };

    auto drawWetFloorReflection = [&](int carCX, int carCY, float yaw_,
                                      float scl_, float intensity_) {
        if (carCY >= kFloorY - 20 || intensity_ < 0.05f) return;
        CarYawVisibility yawVis = sampleCarYaw(yaw_);
        float faceProfile = yawVis.faceScale;
        if (faceProfile < 0.25f) faceProfile = 0.25f;
        int halfW = ((int)(28.0f * scl_ * (0.35f + 0.65f * faceProfile)) + 3) & ~3;
        int reflY = (kFloorY + kRoomPX) & ~3;
        uint16_t reflTint = yawVis.rear ? TAIL_COL : SPORT_GLOW;
        for (int rx = (carCX - halfW) & ~3; rx <= carCX + halfW; rx += kRoomPX) {
            if (rx < 0 || rx + kRoomPX > canvas.width()) continue;
            if ((wallHash(rx, reflY, 0xDE01u) & 0xFFu) >= 112u) continue;
            float edge = fabsf((float)(rx - carCX)) / (float)(halfW + 1);
            float strength = intensity_ * faceProfile * (1.0f - edge * edge);
            if (strength < 0.08f) continue;
            uint16_t base = fastReadPx(canvas, rx, reflY);
            uint8_t strength8 = (uint8_t)(40.0f + strength * 70.0f);
            plot(rx, reflY, screenBlend565(base, reflTint, strength8));
        }
    };

    // ==[ PHASE RENDERING ]==
    if (phase == 0) {
        // DISTANT: just headlights piercing through rain. No body. Pure atmosphere.
        drawDistantHeadlights();
        drawRainCurtain();
    } else if (phase == 1) {
        // APPROACH: car body fading in through haze, headlights growing
        float hazeLevel = 1.0f - phaseT * 1.2f;
        if (hazeLevel < 0.0f) hazeLevel = 0.0f;
        drawFloorGlow(cx, lightIntensity * 0.5f);
        drawDistantHeadlights();
        if (bodyOpacity > 0.05f) {
            drawCarBody(q(cx), q(cy), 0.0f, 0.0f, scale, bodyOpacity);
            drawHazeOverlay(hazeLevel);
        }
        drawRainCurtain();
    } else if (phase == 2) {
        // EMERGE: final push — full body reveal, bloom peaks, rain parts
        drawFloorGlow(cx, lightIntensity);
        drawCarBody(q(cx), q(cy), 0.0f, 0.0f, scale, 1.0f);
        drawCarLighting(q(cx), q(cy), 0.0f, scale, lightIntensity);
        castHeadlightWash(q(cx), q(cy), 0.0f, scale, lightIntensity);
        drawSideVapor(scale, phaseT);
        drawParticles(q(cx), q(cy), phaseT, false);
        drawRainCurtain();
    } else if (phase == 3) {
        // HOVER: full-scale front-facing hover + sway
        drawFloorGlow(cx, 1.0f);
        drawCarBody(q(cx), q(cy), 0.0f, 0.0f, 1.0f, 1.0f);
        drawCarLighting(q(cx), q(cy), 0.0f, 1.0f, 1.0f);
        castHeadlightWash(q(cx), q(cy), 0.0f, 1.0f, 1.0f);
        drawSideVapor(1.0f, 1.0f);
        drawParticles(q(cx), q(cy), 1.0f, false);
        drawWetFloorReflection(cx, cy, 0.0f, scale, 1.0f);
        // Heat shimmer below hover car
        {
            int shimY = q(cy + ROWS * kRoomPX + 8);
            for (int i = 0; i < 6; i++) {
                int sx = cx - 12 + i * 6 + (int)(sinf((float)elapsed / 300.0f + (float)i) * 2.0f);
                if (shimY < kFloorY && shimY > kRoomY)
                    plot(q(sx), shimY, lerp565(RP::DUST, RP::BG,
                         (float)(wallHash(sx, shimY, elapsed / 100) & 0xFF) / 255.0f));
            }
        }
    } else if (phase == 4) {
        // TURN: 180° rotation
        drawFloorGlow(cx, 1.0f);
        drawCarBody(q(cx), q(cy), angle, bankV, 1.0f, 1.0f);
        drawCarLighting(q(cx), q(cy), angle, 1.0f, 1.0f);
        castHeadlightWash(q(cx), q(cy), angle, 1.0f, 1.0f);
        drawSideVapor(1.0f, 1.0f);
        drawParticles(q(cx), q(cy), 1.0f, false);
        drawWetFloorReflection(cx, cy, angle, 1.0f, 1.0f);
        // Rotation motion trail particles on receding edge
        {
            float sinRaw = sinf(angle);
            float sinAbs = fabsf(sinRaw);
            if (sinAbs > 0.2f) {
                int trailDir = (sinRaw >= 0.0f) ? -1 : 1;  // opposite to visible side
                int bodyH = ROWS * kRoomPX;
                for (int ti = 0; ti < 8; ti++) {
                    uint32_t tCyc = 300u + (uint32_t)ti * 80u;
                    float tPh = (float)((elapsed + (uint32_t)ti * 150u) % tCyc) / (float)tCyc;
                    if (tPh >= 0.7f) continue;
                    int tx = cx + trailDir * (int)(20.0f + tPh * 40.0f);
                    int ty = cy + (int)((float)ti / 8.0f * (float)bodyH);
                    if (ty <= kRoomY || ty >= kFloorY) continue;
                    float tOp = (1.0f - tPh / 0.7f) * sinAbs * 0.5f;
                    if ((int)(wallHash(tx, ty, elapsed / 100 + ti + 0xBB01u) & 0xFF) < (int)(tOp * 200.0f))
                        plot(q(tx), q(ty), lerp565(SPORT_GLOW, RP::DUST, tPh));
                }
            }
        }
    } else if (phase == 5) {
        // LINGER: rear-facing hover with weight-shift tilt, taillights active
        drawFloorGlow(cx, 1.0f);
        drawCarBody(q(cx), q(cy), angle, bankV, 1.0f, 1.0f);
        drawCarLighting(q(cx), q(cy), angle, 1.0f, 1.0f);
        castHeadlightWash(q(cx), q(cy), angle, 1.0f, 1.0f);
        drawSideVapor(1.0f, 1.0f);
        drawParticles(q(cx), q(cy), 1.0f, false);
        drawWetFloorReflection(cx, cy, angle, 1.0f, 1.0f);
    } else if (phase == 6) {
        // ACCEL: climb away, dematerialize, akira trails
        // Thruster ignition flash
        if (!s.wdReturnMode && phaseT < 0.15f) {
            float flashT = phaseT / 0.15f;
            float flashI = (1.0f - flashT) * (1.0f - flashT);
            int flashW = (int)(20.0f * (1.0f - flashT * 0.5f));
            int flashY = q(cy + (int)((float)ROWS * (float)kRoomPX * scale) + 4);
            for (int fx = cx - flashW; fx <= cx + flashW; fx += kRoomPX) {
                if (flashY < kFloorY && flashY > kRoomY) {
                    float dist = fabsf((float)(fx - cx)) / (float)(flashW > 0 ? flashW : 1);
                    if ((1.0f - dist) * flashI > 0.2f)
                        plot(q(fx), flashY, lerp565(FLARE_COL, RP::BG, dist * 0.5f));
                }
            }
        }
        if (cy > kRoomY - 20) {
            drawCarBody(q(cx), q(cy), angle, 0.0f, scale, departOpacity);
            drawCarLighting(q(cx), q(cy), angle, scale, lightIntensity);
            castHeadlightWash(q(cx), q(cy), angle, scale, lightIntensity);
            drawSideVapor(scale, 1.0f - phaseT * 0.7f);
            if (!s.wdReturnMode && departOpacity < 0.7f) {
                drawHazeOverlay((0.7f - departOpacity) / 0.7f * 0.5f);
            }
        }
        drawFloorGlow(cx, 1.0f - phaseT);
        if (!s.wdReturnMode) {
            drawAkiraTrails(phaseT, 1.0f);
        }
        // Burst particles at start of accel
        if (!s.wdReturnMode && phaseT < 0.25f) {
            float bt = phaseT / 0.25f;
            for (int i = 0; i < 12; i++) {
                float a = ((float)i / 12.0f) * PI + 0.1f;
                float d = bt * 56.0f;
                int bx = cx + (int)(cosf(a) * d * 0.7f);
                int by = cy + ROWS * kRoomPX + (int)(sinf(a) * d * 0.5f);
                if (by > kRoomY && by < kFloorY)
                    plot(bx, by, lerp565(RP::WARM, RP::DUST, bt));
            }
        }
    } else {
        // COOL: akira trails dissipate + floor glow fade
        float fade = 1.0f - phaseT;
        drawAkiraTrails(1.0f, fade * fade);
        for (int gx = carCenterX - 42; gx <= carCenterX + 42; gx += kRoomPX) {
            if ((wallHash(gx, kFloorY, 0x6103u) & 0xFF) < (int)(fade * 42.0f))
                plot(gx, kFloorY - kRoomPX, SPORT_GLOW);
        }
    }

    // car cinematic cleanup — teleport handles the mode switch directly
    if (s.wdMode && elapsed >= WD_CANOPY_SWITCH_CUT_MS) {
        s.active = false;
        s.wdMode = false;
        room3CinematicCarRunning = false;
        s.nextTriggerMs = 0xFFFFFFFFu;
    }
}

static constexpr int kRooftopBoltX = 252;

// ==[ OPEN SKY VIEWPORT ]==
// The deck lives between the top bar and the horizon haze, and stops short of
// it: PixelWeather::drawFogHaze4 owns the band from kFloorY-68 down, and a
// cloud stamped into that band fights the haze instead of sitting above it.
// Height is a whole number of cells off kRoomY so the masses share the room
// lattice with everything else on this roof.
static constexpr int kRooftopSkyY = kRoomY;
static constexpr int kRooftopSkyH = 33 * kRoomPX;      // 132 -> ends at y=146
static_assert(kRooftopSkyY + kRooftopSkyH <= ((kFloorY - 68) & ~(kRoomPX - 1)),
              "cloud deck must clear the horizon haze band");

static SkyFx::Params rooftopSkyParams() {
    SkyFx::Params sky;
    sky.x = 0;
    sky.y = kRooftopSkyY;
    sky.w = SCREEN_WIDTH;
    sky.h = kRooftopSkyH;
    sky.groundY = kRooftopSurfaceY;
    sky.parallaxX = (int8_t)parallaxFar;
    sky.thunder = Weather::isThunderFlashing();
    sky.tintActive = colorEvent.active;
    sky.tintColor565 = colorEvent.color565;
    sky.tintIntensity = colorEvent.intensity;
    return sky;
}

static ExteriorSprites::RenderOptions room3ExteriorOptions() {
    ExteriorSprites::RenderOptions options;
    options.thunder = Weather::isThunderFlashing();
    options.tintActive = colorEvent.active;
    options.tintColor565 = colorEvent.color565;
    options.tintIntensity = colorEvent.intensity;
    options.parallaxX = (int8_t)parallaxFar;
    options.transparentTopRows = kR4_ExteriorSkyRows;
    return options;
}

static ExteriorSprites::Emitter room3ExteriorEmitter(
        uint32_t now, const ExteriorSprites::RenderOptions& options) {
    return ExteriorSprites::dominantEmitter(
        ExteriorSprites::Scene::Rooftop, now,
        kR4_ExteriorX, kR4_ExteriorY,
        kR4_ExteriorW, kR4_ExteriorH,
        options.parallaxX, options);
}

PigLight selectRoom3PigKeyLight(int pigDrawX, int pigDrawY, uint32_t now) {
    (void)pigDrawY;
    ExteriorSprites::RenderOptions options = room3ExteriorOptions();
    ExteriorSprites::Emitter exterior = room3ExteriorEmitter(now, options);
    PigLight key;
    key.x = exterior.x;
    key.y = exterior.y;
    key.tint = exterior.active ? exterior.color565 : RP::LED;

    // A searchlight raking across the roof outranks a holo ad three blocks
    // away — but only while it is actually on her. Outside its footprint the
    // beam is somebody else's light and the city goes back to being the key.
    int beamX = 0, beamY = 0;
    uint8_t beamS = 0;
    if (SkyFx::sampleSearchlight(beamX, beamY, beamS) && beamS > 40u) {
        const int pigCX = pigDrawX + kPigW / 2;
        const int reach = pigCX > beamX ? pigCX - beamX : beamX - pigCX;
        if (reach < 56) {
            key.x = (int16_t)beamX;
            key.y = (int16_t)beamY;
            key.tint = RP::FLUOR;
        }
    }

    if (Weather::isThunderFlashing()) {
        key.x = (int16_t)kRooftopBoltX;
        key.y = (int16_t)kRoomY;
        key.tint = RP::FLUOR;
    }
    return key;
}

static void drawRooftopLightningBackdrop(M5Canvas& canvas, uint32_t now) {
    if (!Weather::isThunderFlashing()) return;

    PixelWeather::drawThunderFlash4(canvas, now, RP::FLUOR, 0.30f);

    static const int8_t kStepCells[] = {
        0, -1, 0, 1, 0, 0, -1, 1, 1, 0,
        -1, 0, -1, 1, 0, 1, -1, 0, 0
    };
    int boltX = kRooftopBoltX & ~3;
    int boltY = (kRoomY + kRoomPX) & ~3;
    for (size_t i = 0; i < sizeof(kStepCells); ++i) {
        boltX += (int)kStepCells[i] * kRoomPX;
        int y = boltY + (int)i * kRoomPX;
        canvas.fillRect(boltX, y, kRoomPX, kRoomPX, RP::FLUOR);
        if (i == 7 || i == 12) {
            int branchDir = (i == 7) ? -1 : 1;
            canvas.fillRect(boltX + branchDir * kRoomPX, y + kRoomPX,
                            kRoomPX, kRoomPX, RP::SHAFT);
        }
    }
}

static void drawRooftopRfField(M5Canvas& canvas, uint32_t now, int antennaX) {
    uint8_t activity = roomMood.rfActivity;
    if (roomMood.trackerPresent && activity < 132u) activity = 132u;
    if (roomMood.spamActive && activity < 196u) activity = 196u;
    if (activity < 8u) return;

    uint16_t signal = roomMood.spamActive ? RP::SPARK
        : (roomMood.trackerPresent ? RP::CRT : RP::GREEN_DK);
    uint32_t period = 980u - ((uint32_t)activity * 700u) / 255u;
    uint8_t activeRing = (uint8_t)(((now % period) * 3u) / period);
    uint8_t trails = activity >= 176u ? 2u : 1u;
    const int sourceX = antennaX + 4 * kRoomPX;
    const int sourceY = kFloorY - 108;

    for (uint8_t trail = 0; trail < trails; ++trail) {
        uint8_t ring = (uint8_t)((activeRing + 3u - trail) % 3u);
        int radius = 3 * kRoomPX + ring * 3 * kRoomPX;
        int halfH = 2 * kRoomPX + ring * kRoomPX;
        uint8_t strength = trail == 0u ? 112u : 54u;
        const int cells[][2] = {
            {sourceX + radius, sourceY},
            {sourceX + radius - kRoomPX, sourceY - halfH},
            {sourceX + radius - kRoomPX, sourceY + halfH},
            {sourceX + radius - 2 * kRoomPX, sourceY - halfH + kRoomPX},
            {sourceX + radius - 2 * kRoomPX, sourceY + halfH - kRoomPX},
        };
        for (const auto& cell : cells) {
            uint16_t base = fastReadPx(canvas, cell[0], cell[1]);
            canvas.fillRect(cell[0], cell[1], kRoomPX, kRoomPX,
                            screenBlend565(base, signal, strength));
        }
    }
}

static void drawRooftopShackBase(M5Canvas& canvas) {
    int sx = kR4_ShackX + parallaxFar;
    int sy = kR4_ShackY;
    int sw = kR4_ShackW;
    int sh = kR4_ShackH;
    canvas.fillRect(sx, sy, sw, sh, RP::WALL_FAR);
    for (int ly = sy + 2; ly < sy + sh; ly += kRoomPX)
        canvas.fillRect(sx + 2, ly, sw - 4, kRoomPX, RP::WALL_MID);
    for (int rx = sx + 12; rx < sx + sw - 8; rx += 18) {
        int rh = 8 + (int)(wallHash(rx, sy, 22331) & 0x0F);
        canvas.fillRect(rx, sy + sh - rh, kRoomPX, rh, RP::SHADOW_C);
    }

    int doorX = sx + sw / 2 - 10;
    int doorY = sy + sh - 40;
    canvas.fillRect(doorX, doorY, 20, 40, RP::DEEP);
    canvas.fillRect(doorX - 2, doorY - 2, 24, kRoomPX, RP::STRUCT);
    canvas.fillRect(doorX - 2, doorY - 2, kRoomPX, 44, RP::STRUCT);
    canvas.fillRect(doorX + 20, doorY - 2, kRoomPX, 44, RP::STRUCT);
    drawLightPool(canvas, RP::WARM, doorX + 2, doorY + 20,
                  16, 20, 40, 88219);
    drawLightPool(canvas, RP::WARM, doorX - 2, sy + sh,
                  24, 6, 30, 88220);

    int winX = sx + 8;
    int winY = sy + 10;
    int winW = 16;
    int winH = 12;
    canvas.fillRect(winX, winY, winW, winH, RP::DEEP);
    drawLightPool(canvas, RP::WARM, winX + 2, winY + 2,
                  winW - 4, winH - 4, 55, 88119);
    canvas.fillRect(winX, winY, winW, kRoomPX, RP::STRUCT);
    canvas.fillRect(winX, winY + winH, winW, kRoomPX, RP::STRUCT);
    canvas.fillRect(winX - 4, winY, kRoomPX, winH + 4, RP::STRUCT);
    canvas.fillRect(winX + winW, winY, kRoomPX, winH + 4, RP::STRUCT);
    canvas.fillRect(sx + sw - 14, sy - 12, 8, 16, RP::STRUCT);
    canvas.fillRect(sx + sw - 16, sy - 12, 12, kRoomPX, RP::STRUCT);
}

// ==[ ROOM 3: SURVEILLANCE NEST (ROOFTOP) ]== exposed antenna farm, panoramic skyline
void drawRoom3(M5Canvas& canvas, uint32_t now, RoomRenderPass pass) {
    const int antennaX = rooftopAntennaPoseX();
    const int dishBaseX = rooftopDishBaseX();
    const int dishAzimuth = rooftopDishAzimuthOffset(now);
    const int dishBowlX = dishBaseX + dishAzimuth;
    const uint16_t wetPlate = Display::lerpColor565(RP::PUDDLE, RP::SHAFT, 0.18f);
    ExteriorSprites::RenderOptions exteriorOptions = room3ExteriorOptions();
    if (pass == RoomRenderPass::BASE) {
        canvas.fillRect(0, kRoomY, SCREEN_WIDTH,
                        kFloorY - kRoomY, RP::BG);
        ExteriorSprites::RenderOptions retainedOptions = exteriorOptions;
        retainedOptions.thunder = false;
        retainedOptions.tintActive = false;
        ExteriorSprites::drawSceneBase(
            canvas, ExteriorSprites::Scene::Rooftop,
            kR4_ExteriorX, kR4_ExteriorY,
            kR4_ExteriorW, kR4_ExteriorH, retainedOptions);
        drawConcreteWall(canvas, 0, kFloorY - 40, 120, 40, 3u);
        drawRooftopShackBase(canvas);
        drawMetalFloor(canvas, 3u);
        return;
    }

    SkyFx::Params sky = rooftopSkyParams();
    SkyFx::drawSky(canvas, now, sky);
    ExteriorSprites::drawSceneMotion(
        canvas, ExteriorSprites::Scene::Rooftop, now,
        kR4_ExteriorX, kR4_ExteriorY,
        kR4_ExteriorW, kR4_ExteriorH, exteriorOptions);
    drawRooftopLightningBackdrop(canvas, now);

    // The retained shack is static; only its vapor remains live.
    {
        int sx = kR4_ShackX + parallaxFar;
        int sy = kR4_ShackY;
        int sw = kR4_ShackW;
        PixelFurn::drawRooftopShack4(canvas, sx, sy, sw, kR4_ShackH);
        drawSteam(canvas, RP::DUST, now, sx + sw - 12, sy - 14);
        drawSteam(canvas, RP::DUST, now + 800, sx + sw - 14, sy - 16);
    }
    // Light pools from shack (far layer)
    {
        int sx = kR4_ShackX + parallaxFar;
        int doorBaseX = sx + kR4_ShackW / 2 - 10;
        drawVolumetricDustBeam(canvas, now,
                               doorBaseX + 10, kR4_ShackY + kR4_ShackH, 20,
                               doorBaseX + 14, kFloorY - 2, 44,
                               RP::WARM, RP::DUST, 0xD011u);
    }
    // Low storm mist belongs at the horizon. Blend it over the city instead of
    // stamping opaque, drifting cloud cells through the entire open sky.
    PixelWeather::drawFogHaze4(canvas, now,
                               kR4_SkyZoneX, (kFloorY - 68) & ~3,
                               SCREEN_WIDTH - kR4_SkyZoneX, 12 * kRoomPX,
                               RP::SHAFT, 20);

    // L2 open-air rain: stable lanes, three depth bands, and cold contact
    // contact flecks. It stays behind all rooftop hardware and Pancetta.
    {
        PixelWeather::OpenAirRainParams rain;
        rain.motion = 0.62f;
        rain.lateral = -0.16f;
        rain.thundering = Weather::isThunderFlashing();
        PixelWeather::drawOpenAirRain4(canvas, now, 4, kRoomY,
                                       SCREEN_WIDTH - 8,
                                       kFloorY - kRoomY + kRoomPX, rain);
    }
    // City warmth belongs on top of the slab; drawing it earlier erased it.
    drawLightPool(canvas, RP::WARM, 140, kFloorY - 4, 140, 4, 12, 66551);
    // ==[ NEON WASH: sky/antenna LED on parapet + floor ]==
    ExteriorSprites::Emitter roomEmitter =
        room3ExteriorEmitter(now, exteriorOptions);
    PigLight rl;
    rl.x = roomEmitter.x;
    rl.y = roomEmitter.y;
    rl.tint = roomEmitter.active ? roomEmitter.color565 : RP::LED;
    drawNeonWash(canvas, 0, kFloorY - 44, SCREEN_WIDTH, 52,
                 rl, RP::WALL_FAR, 180.0f, 0.30f, 0x4C31);
    // The blimp's searchlight lands here — after the slab has its own grade,
    // before the hardware that has to occlude it. The shaft itself was drawn
    // with the sky; this is only where it touches the roof.
    SkyFx::drawGroundPool(canvas, now, sky, kRooftopSurfaceY);
    // Rare cinematic hover-car event (over floor + neon wash, behind railing)
    drawRoom3CinematicCar(canvas, now);
    // Live RF energy originates at the authored antenna and stays behind its
    // steel silhouette. Tracker/spam only changes its real categorical color.
    drawRooftopRfField(canvas, now, antennaX);
    // (shack moved to far background layer — drawn before haze)
    // Antenna array (left, near layer — pig interacts here)
    {
        int ax = antennaX;
        PixelFurn::drawAntennaArray4(canvas, ax, kRooftopSurfaceY - kR4_AntennaH, now);
        // Junction box at base
        canvas.fillRect(ax + 4, kFloorY - 12, 20, 12, RP::STRUCT);
        canvas.fillRect(ax + 4, kFloorY - 8, 16, 8, RP::FILL);
        drawLightPool(canvas, RP::GREEN_DK, ax + 8, kFloorY - 8, 12, 4, 40, 77331);
        uint8_t activityPips = (uint8_t)min(3u,
            ((uint32_t)roomMood.rfActivity + 84u) / 85u);
        for (uint8_t pip = 0; pip < 3u; ++pip) {
            canvas.fillRect(ax + 4 + pip * kRoomPX, kFloorY - 8,
                            kRoomPX, kRoomPX,
                            pip < activityPips ? RP::GREEN_DK : RP::D_DEEP);
        }
        uint16_t threatCell = roomMood.spamActive ? RP::SPARK
            : (roomMood.trackerPresent ? RP::CRT : RP::D_DEEP);
        canvas.fillRect(ax + 16, kFloorY - 8,
                        kRoomPX, kRoomPX, threatCell);
        canvas.fillRect(ax + 20, kFloorY - 8,
                        kRoomPX, kRoomPX,
                        roomMood.captureCount > 0 ? RP::FLUOR : RP::D_DEEP);
        // Cable runs from junction box to poles
        for (int i = 0; i < 3; i++) {
            int px = ax + i * 12;
            canvas.fillRect(px, kFloorY - 16, kRoomPX, 4, RP::WALL_MID);
        }
        // Beacon LED at center pole top (recon-reactive)
        uint32_t beaconCycle = 2000;
        if (roomMood.alertLevel >= 3) beaconCycle = 200;
        else if (roomMood.alertLevel >= 1) beaconCycle = 500;
        bool beaconOn = (now % beaconCycle) < 200;
        int bx = ax + kRooftopCenterPoleOffsetX;
        int by = kRooftopCenterPoleTopY;
        if (beaconOn) {
            canvas.fillRect(bx, by, kRoomPX, kRoomPX, RP::SPARK);
            // Glow halo around beacon
            drawLightPool(canvas, RP::SPARK, bx - 4, by - 4, 12, 12, 25, 99771);
            // Red tint on nearest crossbar (beacon spill on structure)
            drawLightPool(canvas, RP::SPARK, bx - 6, by + 6, 16, 4, 18, 99772);
        }
        // Furniture light on antenna structure
        drawFurnitureWash(canvas, ax, kFloorY - 160, 36, 160, rl, 100.0f, 0.20f);
    }
    // Satellite dish (mid parallax, recon-reactive rotation)
    {
        int dx = dishBaseX, bowlX = dishBowlX, dy = kR4_DishY;
        int dw = kR4_DishW, dh = kR4_DishH;
        // The reflector sweeps around a fixed motor pedestal. Previously the
        // whole assembly translated sideways, which made it look ungrounded.
        PixelFurn::drawSatelliteDish4(canvas, bowlX, dy, now);
        const int pivotX = (dx + dw / 2) & ~3;
        const int pivotY = (dy + dh / 2) & ~3;
        const int feedX = (bowlX + (dw * 3) / 4) & ~3;
        const int feedY = (dy + dh / 4) & ~3;
        PixelPrim::fatLine4(canvas, pivotX, pivotY, feedX, feedY,
                            RP::WALL_MID);
        canvas.fillRect(feedX, feedY, kRoomPX * 2, kRoomPX, RP::WALL_NEAR);
        // Fixed motor housing and roof anchor.
        canvas.fillRect(pivotX, pivotY, kRoomPX,
                        ((dh / 2 + 14) + 3) & ~3, RP::WALL_MID);
        const int motorY = (dy + dh) & ~(kRoomPX - 1);
        canvas.fillRect(pivotX - 4, motorY, 12, 8, RP::STRUCT);
        canvas.fillRect(pivotX - 8, motorY + 8, 16, kRoomPX, RP::STRUCT);
        const int stemY = motorY + 3 * kRoomPX;
        canvas.fillRect(pivotX - 4, stemY, 8,
                        kRooftopSurfaceY - stemY, RP::WALL_MID);
        const int washX = min(dx, bowlX);
        drawFurnitureWash(canvas, washX, dy, dw + abs(dishAzimuth), dh,
                          rl, 100.0f, 0.20f);
    }
    // Environmental clutter
    drawCableCoil4(canvas, 56, kFloorY - 4);
    drawWallOutlet4(canvas, 82, kFloorY - 22);  // weatherproof outlet
    drawConduitRun4(canvas, 44, kFloorY - 18, 36);      // cable tray
    // Toolbox (open lid, near antenna base — 4px grid)
    canvas.fillRect(24, kFloorY - 8, 16, 8, RP::STRUCT);
    canvas.fillRect(28, kFloorY - 8, 12, 4, RP::FILL);
    canvas.fillRect(24, kFloorY - 12, 16, 4, RP::STRUCT);  // lid
    canvas.fillRect(24, kFloorY - 12, 4, 8, RP::STRUCT);    // lid hinge
    // Scratched corner (dropped on roof)
    canvas.fillRect(40, kFloorY - 8, 4, 4, RP::SHADOW_C);
    // Roof AC unit (between parapet and railing — fills dead zone)
    drawACUnit4(canvas, 120, kFloorY - 24);
    // Drainage gutter (horizontal strip at parapet base — sells roof surface)
    for (int gx = 0; gx < 120; gx += 8)
        canvas.fillRect(gx, kFloorY - 2, 4, kRoomPX, RP::WALL_NEAR);
    // Pop outlines for readability.
    // Every prop on this deck stands on kFloorY, and kFloorY is 218 — two off
    // the screen lattice that drawRoomPopPixel snaps its contour to. q4() can
    // therefore never reach the floor line: an outline cut to a prop's exact
    // height rounds its bottom row DOWN to 216, two pixels inside the prop,
    // and draws BG through it. Floor-standing props get a cell of slack so the
    // contour lands at 220, clear of the surface.
    drawPopOutline1px(canvas, 24, kFloorY - 12, 16, 12 + kRoomPX,
                      PopOutlineStyle::SOLID, 0xD001u);
    // The AC hangs its condensate drip one cell above the body, so the roof
    // unit's envelope starts at kFloorY-30 and is 16 deep, not 20 from -24.
    drawPopOutline1px(canvas, 120, kFloorY - 28, 20, 16,
                      PopOutlineStyle::MIXED, 0xD051u);
    drawPopOutline1px(canvas, antennaX + 4, kFloorY - 12, 20, 12 + kRoomPX,
                      PopOutlineStyle::SOLID, 0xD031u);
    drawPopOutline1px(canvas, 56, kFloorY - 4, 8, 8,
                      PopOutlineStyle::SPARSE, 0xD041u);
    // Ledge railing (right side where pig sits — structural barrier)
    {
        int railX = 140, railY = kFloorY - 30;
        PixelFurn::drawLedgeRailing4(canvas, railX, railY, SCREEN_WIDTH - railX);
        // Paint peel on top rail (multiple spots — exposed to weather)
        canvas.fillRect(railX + 30, railY, 4, 4, RP::FILL);
        canvas.fillRect(railX + 60, railY, 8, 4, RP::FILL);
        canvas.fillRect(railX + 100, railY, 4, 4, RP::SHADOW_C);
        // Furniture light on railing
        drawFurnitureWash(canvas, railX, railY, SCREEN_WIDTH - railX, 32, rl, 100.0f, 0.20f);
    }
    // Furniture light on ground clutter (toolbox, sandbags, tarp, AC)
    drawFurnitureWash(canvas, 24, kFloorY - 32, 120, 32, rl, 100.0f, 0.20f);
    if (Weather::isThunderFlashing()) {
        PigLight stormLight;
        stormLight.x = (int16_t)kRooftopBoltX;
        stormLight.y = (int16_t)kRoomY;
        stormLight.tint = RP::FLUOR;
        drawFurnitureWash(canvas, 0, kRoomY + kRoomPX, SCREEN_WIDTH, kFloorY - kRoomY,
                          stormLight, 320.0f, 0.18f);
        PixelWeather::drawThunderFlash4(canvas, now, RP::FLUOR, 0.22f);
    }
    // Wire spark on antenna (intermittent arcing between crossbars)
    {
        uint32_t sparkWindow = 9000;
        uint32_t sparkPhase = now % sparkWindow;
        if (sparkPhase < 80) {
            int spkX = antennaX + 12;
            int spkY = kFloorY - 128 + 48;
            canvas.fillRect(spkX, spkY, kRoomPX, kRoomPX, RP::SPARK);
            uint32_t h = wallHash(0, 0, now / 40);
            int sdx = ((int)(h % 3) - 1) * kRoomPX;
            int sdy = ((int)((h >> 2) % 3) - 1) * kRoomPX;
            canvas.fillRect((spkX + sdx) & ~3, (spkY + sdy) & ~3, kRoomPX, kRoomPX, RP::SPARK);
            // Momentary flash pool on crossbar
            drawLightPool(canvas, RP::SPARK, spkX - 6, spkY - 4, 16, 12, 30, 99881);
        }
    }
    // ==[ WEATHER: full rain exposure ]== noir world: rooftop always wet
    {
        // Puddles own a fixed mask. Moving light travels across the water;
        // the water itself does not teleport every few frames.
        const int puddleCells = (SCREEN_WIDTH - 8) / (kRoomPX * 2);
        const int sweepCell = (int)((now / 140u) % (uint32_t)puddleCells);
        const int secondSweepCell = (sweepCell + puddleCells / 2) % puddleCells;
        const bool thunder = Weather::isThunderFlashing();
        for (int px = 4; px < SCREEN_WIDTH - 4; px += kRoomPX * 2) {
            uint8_t h = (uint8_t)(wallHash(px, kFloorY + 2, 33551u) & 0xFFu);
            bool puddle = false;
            uint16_t puddleCol = Display::lerpColor565(wetPlate, RP::BG, 0.34f);
            if (px < 80) {
                puddle = h < 35;
                puddleCol = RP::PUDDLE;
            } else {
                puddle = h < 30;
                if (h < 18) puddleCol = RP::PUDDLE;
            }
            if (!puddle) continue;
            if (thunder)
                puddleCol = screenBlend565(puddleCol, RP::FLUOR, 70);
            for (int ox = 0; ox < 8; ox += kRoomPX) {
                uint16_t wetBase = fastReadPx(canvas, px + ox, kFloorY + 2);
                canvas.fillRect(px + ox, kFloorY + 2, kRoomPX, kRoomPX,
                                screenBlend565(wetBase, puddleCol, 72));
            }

            int cell = (px - 4) / (kRoomPX * 2);
            if (cell == sweepCell || cell == secondSweepCell) {
                uint16_t glint = thunder ? RP::FLUOR : RP::WARM;
                for (int ox = 0; ox < 8; ox += kRoomPX) {
                    uint16_t base = fastReadPx(canvas, px + ox, kFloorY + 2);
                    canvas.fillRect(px + ox, kFloorY + 2, kRoomPX, kRoomPX,
                                    screenBlend565(base, glint, thunder ? 96 : 42));
                }
            }
        }
        // Antenna drip (water running down mast)
        {
            uint32_t dripCyc = 1800u;
            float dp = (float)(now % dripCyc) / (float)dripCyc;
            if (dp < 0.5f) {
                float dt = dp / 0.5f;
                int dripY = kRooftopCenterPoleTopY +
                    (int)(dt * dt *
                          (float)(kRooftopSurfaceY -
                                  kRooftopCenterPoleTopY - kRoomPX));
                canvas.fillRect(antennaX + kRooftopCenterPoleOffsetX,
                                dripY & ~3, kRoomPX, kRoomPX, wetPlate);
            } else if (dp < 0.6f) {
                uint16_t splashCol = thunder ? RP::FLUOR : RP::SHAFT;
                canvas.fillRect(antennaX + kRooftopCenterPoleOffsetX,
                                kRooftopSurfaceY, 8, kRoomPX, splashCol);
            }
        }
    }
    drawCondensation(canvas, now, dishBowlX, kR4_DishY, kR4_DishW, kR4_DishH,
                     kRooftopCondensationCount, RP::SHAFT,
                     kRooftopCondensationSalt);
    // ==[ ENRICHMENT: cable coil near antenna base ]==
    drawCableCoil4(canvas, antennaX + 24, kFloorY - 8);
    // ==[ ENRICHMENT: extra puddle patches on roof — wet everywhere ]==
    for (int i = 0; i < 4; i++) {
        int px = 140 + i * 36 + (int)((wallHash(i + 10, 0, 66331) & 0xF) - 4);
        px = px & ~3;
        if (px > 4 && px < SCREEN_WIDTH - 12) {
            for (int ox = 0; ox < 12; ox += kRoomPX) {
                uint16_t base = fastReadPx(canvas, px + ox, kFloorY + 2);
                canvas.fillRect(px + ox, kFloorY + 2, kRoomPX, kRoomPX,
                                screenBlend565(base, wetPlate, 48));
            }
        }
    }
    // ==[ ENRICHMENT: bird silhouette perched on center antenna pole ]==
    {
        int birdX = antennaX + kRooftopPerchPoleOffsetX;
        int birdY = kRooftopPerchPoleTopY - kRoomPX;
        // Wing flap: brief 200ms raise every 6-8s (irregular, asymmetric)
        uint32_t flapPhase = now % 7200;
        bool wingUp = (flapPhase < 200);
        int lwDy = wingUp ? -4 : 0;
        int rwDy = wingUp ? (flapPhase < 100 ? -4 : 0) : 0;  // right wing drops 100ms early
        canvas.fillRect(birdX - 4, birdY + lwDy, 4, 4, RP::STRUCT);  // left wing
        canvas.fillRect(birdX, birdY - 4, 4, 8, RP::STRUCT);          // body
        canvas.fillRect(birdX + 4, birdY + rwDy, 4, 4, RP::STRUCT);  // right wing
        canvas.fillRect(birdX, birdY + 4, 4, 4, RP::WALL_MID);          // tail
    }
    drawRooftopStakeoutEvidence(canvas, now);
}

static void applyRoom3NoirToPig(M5Canvas& canvas, uint32_t) {
    static constexpr PigNoirProfile kProfile = {102, 210, 38, 90, 85};
    applyDirectionalPigNoir(canvas, kProfile);
}

// ==[ DECK VENT ]== the thing the flying car lands on top of.
//
// Sited at x=180: clear of the shack (ends at 172) and clear of the car body
// (spans roughly 195..277), in the one strip of open deck between them. Sized
// rise=13/1700ms ≈ 94px, so from the deck plate at y=212 the column tops out
// around y=118 — twenty pixels above the car's hover altitude of y=138. The car
// descends *through* the steam, which is the only reason the arrival gust in
// drawRoom3CinematicCar has anything to throw.
//
// Drawn in the foreground pass so the column reads in front of both the car and
// Pancetta, the same way the bar hangs its smoke over everything.
static void drawRooftopDeckVent(M5Canvas& canvas, uint32_t now) {
    SmokeFx::VentParams vent;
    vent.x = 180;
    vent.y = (kFloorY - kRoomPX) & ~3;
    vent.intervalMs = 480;
    vent.rise = 13;
    vent.lifeMs = 1700;
    vent.spreadPx = 8;
    vent.scale = 70;
    vent.opacity = 96;      // thinner than interior steam — it is losing to the rain
    vent.seed = 0xD3C4u;
    SmokeFx::driveVent(SmokeFx::Source::Vent0, now, vent);

    SmokeFx::Lighting lit;
    lit.add(selectRoom3PigKeyLight(0, 0, now), 150.0f, 120);
    // The airship already publishes its beam for exactly this kind of use, so
    // the column lights up when the searchlight rakes across the roof.
    int beamX = 0, beamY = 0;
    uint8_t beamS = 0;
    if (SkyFx::sampleSearchlight(beamX, beamY, beamS) && beamS > 40u) {
        PigLight beam;
        beam.x = (int16_t)beamX;
        beam.y = (int16_t)beamY;
        beam.tint = RP::FLUOR;
        lit.add(beam, 88.0f, (uint8_t)(beamS > 180u ? 180u : beamS));
    }
    SmokeFx::draw(canvas, SmokeFx::Source::Vent0, RP::SOFT, RP::DUST, &lit);
}

void drawRoom3Foreground(M5Canvas& canvas, uint32_t now) {
    // Exterior now obeys the same directional light contract as every interior.
    applyRoom3NoirToPig(canvas, now);
    drawRooftopDeckVent(canvas, now);
    // Railing foreground post — when pig at ON_LEDGE, vertical bar in front
    if (isStationVisualActive(Station::ON_LEDGE)) {
        // Snap the near post to the rail's 20px structural rhythm and keep it
        // at Pancetta's right flank. The old +30 anchor cut through the belly.
        int postX = 140 + (((kR4_LedgePigX + kPigW - 140) / 20) * 20);
        int postTop = kFloorY - 52;
        canvas.fillRect(postX, postTop, kRoomPX, 52, RP::STRUCT);
        canvas.fillRect(postX - 4, postTop, 12, kRoomPX, RP::STRUCT);  // top rail
        canvas.fillRect(postX - 4, postTop + 24, 12, kRoomPX, RP::WALL_MID);  // mid rail
        // Rust spot
        canvas.fillRect(postX, postTop + 12, kRoomPX, kRoomPX, RP::FILL);
    }
    // Guy-wire — diagonal from the center mast to the roof anchor, crossing pig zone
    {
        int wireStartX = rooftopAntennaPoseX() + kRooftopCenterPoleOffsetX;
        int wireStartY = kRooftopGuyWireAttachY;
        int wireEndX = 200;
        int wireEndY = (kFloorY - 20) & ~(kRoomPX - 1);
        int steps = (wireEndY - wireStartY) / kRoomPX;
        for (int s = 0; s < steps; s += 2) {
            float t = (float)s / (float)steps;
            int wx = (wireStartX + (int)((float)(wireEndX - wireStartX) * t)) & ~3;
            int wy = (wireStartY + s * kRoomPX) & ~3;
            if (wx > 4 && wx < SCREEN_WIDTH - 4 && wy > kRoomY && wy < kFloorY)
                canvas.fillRect(wx, wy, kRoomPX, kRoomPX, RP::WALL_MID);
        }
    }
}

} // namespace MenuPig
