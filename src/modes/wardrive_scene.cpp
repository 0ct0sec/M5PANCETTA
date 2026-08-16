/**
 * wardrive_scene.cpp — orchestrator for WARTHOG cockpit POV
 *
 * Thin coordinator: owns state variables, init/reset/drawScene/shutdown,
 * and the per-frame layer dispatch. All rendering extracted to:
 *   wardrive_city_backdrop.cpp  — city parallax, tower grid, backdrop cache
 *   wardrive_cockpit_shell.cpp  — shell, dash, cables, monitors, yoke, chair, CRT
 *   wardrive_glass.cpp          — canopy geometry, bounds, pane queries
 *   wardrive_hud.cpp            — telemetry, attitude indicator, tower comms
 *   wardrive_lighting.cpp       — cockpit lighting, curvature, streaks
 *   wardrive_pig_render.cpp     — pig body, shading, comms bubble
 *   wardrive_weather.cpp        — canopy rain motion
 *   wardrive_shared.h           — constants, types, inline helpers, extern state
 */

#include "wardrive_scene.h"
#include "wardrive_shared.h"
#include "wardrive.h"
#include "wardrive_glass.h"
#include "wardrive_hud.h"
#include "wardrive_pig_render.h"
#include "wardrive_cockpit_shell.h"
#include "wardrive_lighting.h"
#include "wardrive_weather.h"
#include "wardrive_city_backdrop.h"
#include "../ui/display.h"
#include "../ui/menu_pig_render.h"
#include "../piglet/pig_face_timer.h"
#include "../piglet/mood.h"
#include "../activity/pedometer.h"
#include "../audio/sfx.h"
#include "../haptic/haptic.h"
#include "../ui/teleport.h"
#include "../defense/recon.h"
#include "../util/debug_log.h"
#include <math.h>
#include <string.h>

static constexpr float PI_F = 3.14159265f;

using namespace MenuPigRender;

namespace WardriveScene {

// ═══════════════════════════════════════════════════════════════════════════
// STATE — owned by the orchestrator, accessed by modules via wardrive_shared.h
// ═══════════════════════════════════════════════════════════════════════════

// Cockpit accents (set per-frame in drawScene)
uint16_t WD_NEON  = 0;
uint16_t WD_AMBER = 0;
uint16_t WD_LED   = 0;
uint16_t WD_SPARK = 0;

// IMU state
float steerAngle  = 0.0f;
float imuRoll     = 0.0f;
float imuPitch    = 0.0f;
float attRollDeg  = 0.0f;
float attPitchDeg = 0.0f;
float gyroBiasEst = 0.0f;  // local to orchestrator (not extern)
bool  parallaxEnabled = true;
static uint32_t sceneLastFrameMs = 0;

// Entry transition
bool     sceneEntryTransitionActive = false;
uint32_t sceneEntryTransitionStart  = 0;

// Chair settle
bool     chairSettleActive = false;
uint32_t chairSettleStart  = 0;

// Traffic flare
float    wdTrafficFlare    = 0.0f;
uint16_t wdTrafficFlareCol = 0;
int      wdTrafficFlareCX  = 160;
int8_t   wdTrafficFlareDir = 1;

// Face timer
PigFaceTimer sceneFaceTimer;

// Thunder — wdThunderFlashing, wdThunderFlashState, wdThunderFlickerBoost,
// wdVibrateY are extern in wardrive_shared.h (read by lighting/city modules).
// The rest are orchestrator-local.
bool     wdThunderFlashing        = false;
uint8_t  wdThunderFlashState      = 0;
float    wdThunderFlickerBoost    = 0.0f;
float    wdStormIntensity         = 0.0f;
int8_t   wdVibrateY               = 0;
static uint32_t wdThunderFlashStart      = 0;
static uint32_t wdThunderFlashDuration   = 0;
static uint8_t  wdThunderFlashesRemaining= 0;
static bool     wdThunderBurstFiring     = false;
static uint32_t wdLastThunderStorm       = 0;
static uint32_t wdNextThunderInterval    = 0;

// ═══════════════════════════════════════════════════════════════════════════
// STATIC ASSERTS (geometry invariants)
// ═══════════════════════════════════════════════════════════════════════════

static_assert(sceneRectFits(SIG_X, SIG_Y, SIG_W, SIG_H, WD_GLASS_T, WD_GLASS_B) &&
              sceneRectFits(NAV_X, NAV_Y, NAV_W, NAV_H, WD_GLASS_T, WD_GLASS_B) &&
              sceneRectFits(COORD_X, COORD_Y, COORD_W, COORD_H, WD_GLASS_T, WD_GLASS_B) &&
              sceneRectFits(LOG_X, LOG_Y, LOG_W, LOG_H, WD_GLASS_T, WD_GLASS_B) &&
              sceneRectFits(ATT_X, ATT_Y, ATT_W, ATT_H, WD_GLASS_T, WD_GLASS_B),
              "Wardrive HUD panes must stay inside the windshield");
static_assert(sceneRectOnGrid(SIG_X, SIG_Y, SIG_W, SIG_H) &&
              sceneRectOnGrid(NAV_X, NAV_Y, NAV_W, NAV_H) &&
              sceneRectOnGrid(COORD_X, COORD_Y, COORD_W, COORD_H) &&
              sceneRectOnGrid(LOG_X, LOG_Y, LOG_W, LOG_H) &&
              sceneRectOnGrid(ATT_X, ATT_Y, ATT_W, ATT_H),
              "Wardrive HUD panes must stay on the cockpit grid");
static_assert((COORD_H - SCENE_TEXT_PAD_Y) / SCENE_LINE_H >= 3 &&
              (COORD_W - 12) / SCENE_CHAR_W >= 8 &&
              COORD_Y + COORD_H <= WD_DASH_T,
              "Wardrive COORD pane must fit source, latitude, and longitude rows");
static_assert(!sceneRectsOverlap(SIG_X, SIG_Y, SIG_W, SIG_H,
                                 NAV_X, NAV_Y, NAV_W, NAV_H) &&
              !sceneRectsOverlap(SIG_X, SIG_Y, SIG_W, SIG_H,
                                 COORD_X, COORD_Y, COORD_W, COORD_H) &&
              !sceneRectsOverlap(SIG_X, SIG_Y, SIG_W, SIG_H,
                                 LOG_X, LOG_Y, LOG_W, LOG_H) &&
              !sceneRectsOverlap(SIG_X, SIG_Y, SIG_W, SIG_H,
                                 ATT_X, ATT_Y, ATT_W, ATT_H) &&
              !sceneRectsOverlap(NAV_X, NAV_Y, NAV_W, NAV_H,
                                 COORD_X, COORD_Y, COORD_W, COORD_H) &&
              !sceneRectsOverlap(NAV_X, NAV_Y, NAV_W, NAV_H,
                                 LOG_X, LOG_Y, LOG_W, LOG_H) &&
              !sceneRectsOverlap(NAV_X, NAV_Y, NAV_W, NAV_H,
                                 ATT_X, ATT_Y, ATT_W, ATT_H) &&
              !sceneRectsOverlap(COORD_X, COORD_Y, COORD_W, COORD_H,
                                 LOG_X, LOG_Y, LOG_W, LOG_H) &&
              !sceneRectsOverlap(COORD_X, COORD_Y, COORD_W, COORD_H,
                                 ATT_X, ATT_Y, ATT_W, ATT_H) &&
              !sceneRectsOverlap(LOG_X, LOG_Y, LOG_W, LOG_H,
                                 ATT_X, ATT_Y, ATT_W, ATT_H),
              "Wardrive HUD panes must not overwrite each other");
static_assert(sceneRectFits(WHEEL_X, WHEEL_Y, WHEEL_W, WHEEL_H, WD_PLAY_T, WD_PLAY_B) &&
              sceneRectFits(MON_B_X, MON_B_Y, MON_B_W, MON_B_H, WD_PLAY_T, WD_PLAY_B),
              "Wardrive controls must stay inside the scene");
static_assert(sceneRectFits(CAB_A_X, CAB_A_Y, CAB_A_W, CAB_A_H, WD_PLAY_T, WD_PLAY_B) &&
              sceneRectFits(CAB_B_X, CAB_B_Y, CAB_B_W, CAB_B_H, WD_PLAY_T, WD_PLAY_B) &&
              sceneRectFits(CAB_C_X, CAB_C_Y, CAB_C_W, CAB_C_H, WD_PLAY_T, WD_PLAY_B) &&
              sceneRectFits(CAB_D_X, CAB_D_Y, CAB_D_W, CAB_D_H, WD_PLAY_T, WD_PLAY_B),
              "Wardrive cable anchors must stay inside the scene");
static_assert(sceneRectFits(PIG_CX - (PIG_GRID_W * PIG_P) / 2, PIG_TOP_Y,
                            PIG_GRID_W * PIG_P, (PIG_GRID_H + 2) * PIG_P,
                            WD_PLAY_T, WD_PLAY_B),
              "Wardrive pig envelope must stay inside the scene");
static_assert((CHAIR_CORE_W % COCKPIT_GRID) == 0 &&
              (CHAIR_HEAD_W % COCKPIT_GRID) == 0 &&
              (CHAIR_HEAD_H % COCKPIT_GRID) == 0 &&
              (CHAIR_BACK_W_TOP % COCKPIT_GRID) == 0 &&
              (CHAIR_BACK_W_MID % COCKPIT_GRID) == 0 &&
              (CHAIR_BACK_H % COCKPIT_GRID) == 0 &&
              (CHAIR_BASE_W % COCKPIT_GRID) == 0 &&
              (CHAIR_RAIL_W % COCKPIT_GRID) == 0 &&
              (CHAIR_BOLSTER_MAX_W % COCKPIT_GRID) == 0,
              "Wardrive chair parts must stay on the cockpit-local grid");
static_assert(sceneRectFits(CHAIR_ENVELOPE_L,
                             PIG_TOP_Y + CHAIR_DY - CHAIR_MOTION_PAD_Y,
                             CHAIR_ENVELOPE_W,
                             CHAIR_ENVELOPE_H + CHAIR_MOTION_PAD_Y * 2,
                             WD_PLAY_T, WD_PLAY_B),
              "Wardrive chair envelope must stay inside the scene at full motion");
static_assert((CHAIR_SHADOW_L % COCKPIT_GRID) == 0 &&
              (CHAIR_SHADOW_R % COCKPIT_GRID) == 0 &&
              CHAIR_SHADOW_L <= CHAIR_ENVELOPE_L &&
              CHAIR_SHADOW_R >= CHAIR_ENVELOPE_R,
              "Wardrive chair shadow must cover its full grid-snapped envelope");

// ═══════════════════════════════════════════════════════════════════════════
// INTERNAL HELPERS (orchestrator-only)
// ═══════════════════════════════════════════════════════════════════════════

float getSceneEntryBlackout(uint32_t now) {
    if (!sceneEntryTransitionActive) return 0.0f;
    uint32_t elapsed = now - sceneEntryTransitionStart;
    if (elapsed <= WD_ENTRY_BLACK_HOLD_MS) return 1.0f;
    if (elapsed >= WD_ENTRY_BLACK_HOLD_MS + WD_ENTRY_REVEAL_MS) {
        sceneEntryTransitionActive = false;
        return 0.0f;
    }
    float revealT = Gfx::smoothstep01((float)(elapsed - WD_ENTRY_BLACK_HOLD_MS) /
                                      (float)WD_ENTRY_REVEAL_MS);
    return 1.0f - revealT;
}

void drawSceneEntryTransition(M5Canvas& canvas, uint32_t now) {
    float blackout = getSceneEntryBlackout(now);
    if (blackout <= 0.0f) return;
    uint16_t fadeCol = Display::getColorBG();
    if (blackout >= 0.999f) {
        canvas.fillRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, fadeCol);
        return;
    }
    // bayer4 holds 0..240, so the threshold has to be on the same scale. The
    // old 1..16 range only ever matched the single zero cell, which turned the
    // 360ms dithered reveal into a hard cut over a 1/16 stipple.
    int threshold = clampi((int)ceilf(blackout * 256.0f), 1, 256);
    for (int y = 0; y < SCREEN_HEIGHT; y += PX) {
        for (int x = 0; x < SCREEN_WIDTH; x += PX) {
            if (bayer4[(y / PX) & 3][(x / PX) & 3] < threshold) {
                canvas.fillRect(x, y, PX, PX, fadeCol);
            }
        }
    }
}

float getChairSettleOffset(uint32_t now) {
    if (!chairSettleActive) return 0.0f;
    uint32_t e = now - chairSettleStart;
    if (e >= CHAIR_SETTLE_MS) { chairSettleActive = false; return 0.0f; }
    float t = (float)e / (float)CHAIR_SETTLE_MS;
    return 10.0f * sinf(t * PI_F * 3.0f) * expf(-t * 4.0f);
}

static inline PigPose computePigPose(uint32_t now, float motion) {
    float animT = (float)now * 0.001f;
    int cell = pigCell();
    int settleY = q((int)getChairSettleOffset(now));
    int vibeY = q(wdVibrateY);
    wdVibrateY = (int8_t)(-wdVibrateY / 2);
    // Road rumble rides the same ±PX channel as the thunder jolt, but only in
    // the top quarter of the speed band. The hash advances on a 70ms cell so a
    // hit persists ~4 frames at 60Hz — irregular chatter, not a sine wobble.
    // Anything below PX is invisible: q() floors sub-cell offsets to zero.
    float rumble = Gfx::clamp01((motion - 0.72f) * 3.6f);
    if (rumble > 0.0f) {
        uint32_t rh = wallHash((int)(now / 70u), 0x3B, 0x2E17u);
        if ((rh & 0xFFu) < (uint32_t)(rumble * 150.0f)) {
            vibeY += (rh & 0x100u) ? PX : -PX;
        }
    }
    int bobY = q((int)(fastSinf(animT * 1.8f) * (cell * 0.7f))) + settleY + vibeY;
    int bobX = q((int)(steerAngle * cell * 2.5f));
    int bx = q(PIG_CX) + bobX;
    int by = q(PIG_TOP_Y) + bobY;
    return {cell, bx, by, bx - 9 * cell, bx + 9 * cell, bobX, bobY};
}

// ==[ THUNDER ]== burst-flash state machine
static void updateWdThunder(uint32_t now) {
    if (!SHOW_THUNDER) {
        wdThunderFlashing = false;
        wdThunderFlashState = 0;
        wdThunderFlashesRemaining = 0;
        wdThunderBurstFiring = false;
        return;
    }
    if (now - wdLastThunderStorm > WD_THUNDER_MAX * 3) {
        wdLastThunderStorm = now;
        wdNextThunderInterval = WD_THUNDER_MIN + (esp_random() % (WD_THUNDER_MAX - WD_THUNDER_MIN));
    }
    if (!wdThunderFlashing && wdThunderFlashesRemaining == 0) {
        wdThunderBurstFiring = false;
        if (now - wdLastThunderStorm >= wdNextThunderInterval) {
            wdThunderFlashesRemaining = 2 + (esp_random() % 3);
            wdLastThunderStorm = now;
            wdNextThunderInterval = WD_THUNDER_MIN + (esp_random() % (WD_THUNDER_MAX - WD_THUNDER_MIN));
        }
    }
    if (wdThunderFlashesRemaining > 0 && !wdThunderFlashing) {
        wdThunderFlashing = true;
        wdThunderFlashStart = now;
        wdThunderFlashState = 1;
        wdThunderFlashDuration = 30 + (esp_random() % 31);
        wdThunderFlashesRemaining--;
        if (!wdThunderBurstFiring) {
            SFX::play(SFX::THUNDER_RUMBLE);
            Haptic::buzz();
            wdThunderBurstFiring = true;
            wdThunderFlickerBoost = 1.0f;
            wdStormIntensity = 1.0f;
            wdVibrateY = PX;
        }
    }
    if (wdThunderFlashing) {
        uint32_t elapsed = now - wdThunderFlashStart;
        if (wdThunderFlashState == 1 && elapsed > wdThunderFlashDuration) {
            wdThunderFlashState = 0;
            wdThunderFlashStart = now;
            wdThunderFlashDuration = 20 + (esp_random() % 21);
        } else if (wdThunderFlashState == 0 && elapsed > wdThunderFlashDuration) {
            wdThunderFlashing = false;
            wdThunderFlashState = 0;
        }
    }
}

// ==[ IMU STEERING ]== yaw rate → visual steer angle
static inline float frameAlpha(float referenceAlpha, float frameScale) {
    return Gfx::clampf(referenceAlpha * frameScale, 0.0f, 1.0f);
}

static inline float frameRetention(float referenceRetention, float frameScale) {
    return Gfx::clampf(1.0f - (1.0f - referenceRetention) * frameScale, 0.0f, 1.0f);
}

static float getFrameScale60Hz(uint32_t now) {
    uint32_t elapsedMs = sceneLastFrameMs == 0 ? 17u : (uint32_t)(now - sceneLastFrameMs);
    sceneLastFrameMs = now;
    if (elapsedMs < 1u) elapsedMs = 1u;
    if (elapsedMs > 50u) elapsedMs = 50u;
    return (float)elapsedMs * (60.0f / 1000.0f);
}

static void updateSteerAngle(float frameScale) {
    if (!parallaxEnabled) return;
    float gx, gy, gz;
    Pedometer::getCachedGyro(gx, gy, gz);
    float ax, ay, az;
    Pedometer::getCachedAccel(ax, ay, az);
    float flatness = Gfx::clamp01((fabsf(az) - 0.55f) * 3.3f);
    float yawRate = gx + (gz - gx) * flatness;
    if (fabsf(yawRate) < 8.0f) {
        gyroBiasEst += (yawRate - gyroBiasEst) * frameAlpha(0.0025f, frameScale);
    }
    yawRate -= gyroBiasEst;
    if (fabsf(yawRate) < 4.0f) yawRate = 0.0f;
    float target = Gfx::clamp01(fabsf(yawRate) / 45.0f);
    if (yawRate < 0) target = -target;
    steerAngle += (target - steerAngle) * frameAlpha(0.095f, frameScale);
    if (fabsf(steerAngle) < 0.05f) steerAngle = 0.0f;

    sampleAttitudeDegrees(attRollDeg, attPitchDeg);
    float rollTarget = Gfx::clampf(attRollDeg / 35.0f, -1.0f, 1.0f);
    imuRoll += (rollTarget - imuRoll) * frameAlpha(0.095f, frameScale);
    if (fabsf(imuRoll) < 0.02f) imuRoll = 0.0f;

    float pitchTarget = Gfx::clampf(attPitchDeg / 70.0f, -1.0f, 1.0f);
    imuPitch += (pitchTarget - imuPitch) * frameAlpha(0.105f, frameScale);
    if (fabsf(imuPitch) < 0.02f) imuPitch = 0.0f;
}

// ==[ AUTO-PILOT ]== smooth random flight when screen is locked
static float apTargetSteer  = 0.0f;
static float apTargetPitch  = 0.0f;
static float apTargetRoll   = 0.0f;
static uint32_t apSequenceSeed = 0;
static uint32_t apWaypointIndex = 0;
static uint32_t apWaypointStartMs = 0;
static uint32_t apWaypointIntervalMs = 0;
// Set when the scan engine reports a fresh network; the next waypoint becomes
// a hard bank so the idle scene reacts to what the radio is actually doing.
static bool     apBankPending = false;
static uint32_t apLastNewNets = UINT32_MAX;   // UINT32_MAX = not yet sampled

static float autoPilotSignedUnit(uint32_t hash) {
    return (float)(hash & 0xFFFFu) * (2.0f / 65535.0f) - 1.0f;
}

static void pickAutoPilotWaypoint(uint32_t now) {
    int sequenceCell = (int)(apWaypointIndex++ & 0xFFFFu);
    uint32_t steerHash = wallHash(sequenceCell, 0x51, apSequenceSeed ^ 0xA17Cu);
    uint32_t pitchHash = wallHash(sequenceCell, 0x93, apSequenceSeed ^ 0xC42Bu);
    uint32_t rollHash  = wallHash(sequenceCell, 0xD7, apSequenceSeed ^ 0x71E5u);
    uint32_t timeHash  = wallHash(sequenceCell, 0x29, apSequenceSeed ^ 0x5B09u);
    // Roughly one waypoint in four is a bank: a wider envelope held for a
    // shorter beat. The tame envelope stays the baseline so the scene never
    // reads as drunk, but the idle loop stops feeling like one repeated arc.
    bool bank = apBankPending || ((timeHash >> 19) & 3u) == 0u;
    apBankPending = false;
    apTargetSteer = autoPilotSignedUnit(steerHash) * (bank ? 0.85f : 0.50f);
    apTargetPitch = autoPilotSignedUnit(pitchHash) * (bank ? 0.28f : 0.20f);
    apTargetRoll  = autoPilotSignedUnit(rollHash)  * (bank ? 0.38f : 0.15f);
    apWaypointStartMs = now;
    apWaypointIntervalMs = bank ? (1800u + timeHash % 2201u)
                                : (3000u + timeHash % 5001u);
}

// A new network on the air pulls the auto-pilot into its next bank early, so
// the horizon swings when the scan actually finds something. Ignored while the
// IMU owns the camera — real motion must never be overridden.
static void requestAutoPilotBank(uint32_t now) {
    apBankPending = true;
    if (parallaxEnabled) return;
    // Only cut a waypoint short once it has been held long enough to have
    // visibly arrived; otherwise a burst of discoveries produces a stutter.
    if (apWaypointIntervalMs != 0 &&
        (uint32_t)(now - apWaypointStartMs) >= 900u) {
        pickAutoPilotWaypoint(now);
    }
}

static void updateAutoPilot(float frameScale, uint32_t now) {
    if (apWaypointIntervalMs == 0 ||
        (uint32_t)(now - apWaypointStartMs) >= apWaypointIntervalMs) {
        pickAutoPilotWaypoint(now);
    }

    // Banks approach faster so the wider envelope is actually reached inside
    // the shorter hold; the tame envelope keeps its original settling rates.
    bool banking = apWaypointIntervalMs < 3000u;
    float steerAlpha = frameAlpha(banking ? 0.060f : 0.035f, frameScale);
    float pitchAlpha = frameAlpha(banking ? 0.060f : 0.040f, frameScale);
    float rollAlpha  = frameAlpha(banking ? 0.075f : 0.050f, frameScale);

    steerAngle += (apTargetSteer - steerAngle) * steerAlpha;
    imuPitch   += (apTargetPitch - imuPitch)   * pitchAlpha;
    imuRoll    += (apTargetRoll  - imuRoll)     * rollAlpha;

    attRollDeg  = imuRoll  * 35.0f;
    attPitchDeg = imuPitch * 70.0f;
}

// ═══════════════════════════════════════════════════════════════════════════
// PUBLIC API
// ═══════════════════════════════════════════════════════════════════════════

void init() {
    // no persistent scene state beyond the shared micro-animation timer.
}

void reset() {
    sceneBackdropCacheLastMs = UINT32_MAX;

    // Geometry and scene state must not depend on optional PSRAM.
    precomputeGlassBounds();

    // Reserve the essential city data before the optional full-width cache.
    // If either allocation fails the scene still has a valid cockpit/glass
    // fallback; prepareSceneBackdropCache() is likewise failure-latched.
    if (!sceneTowerSlots) {
        sceneTowerSlots = (CityTowerSlot*)heap_caps_malloc(
            sizeof(CityTowerSlot) * CITY_TOWER_SLOT_CAP, MALLOC_CAP_SPIRAM);
        if (!sceneTowerSlots) HAMLET_LOGLN("[WARDRIVE] PSRAM alloc failed: sceneTowerSlots");
    }
    if (!sceneVisibleIdx) {
        sceneVisibleIdx = (uint16_t*)heap_caps_malloc(
            sizeof(uint16_t) * CITY_TOWER_SLOT_CAP, MALLOC_CAP_SPIRAM);
        if (!sceneVisibleIdx) HAMLET_LOGLN("[WARDRIVE] PSRAM alloc failed: sceneVisibleIdx");
    }
    prepareSceneBackdropCache();

    cityPal.cacheKeyBG = ~RP::BG;
    cityGridValid = false;
    cachedTowerCount = 0;
    refHorizonY = 0;

    uint32_t resetNow = millis();
    sceneFaceTimer.init(resetNow, 4500, 9000, 6000, 12000);
    generateCallsign(false);
    towerPhase = TowerPhase::IDLE;
    towerPhaseStart = 0;
    towerBubbleText[0] = '\0';
    pigBubbleText[0] = '\0';
    towerSpeakerIdx = 0;
    nextCommsTime = 0;
    lastCommsWasRecon = false;
    hasCommsTower = false;
    savedCommsIdx = 0;

    wdThunderFlashing = false;
    wdThunderFlashState = 0;
    wdThunderFlashesRemaining = 0;
    wdThunderBurstFiring = false;
    wdThunderFlickerBoost = 0.0f;
    wdStormIntensity = 0.0f;
    wdVibrateY = 0;
    wdThunderFlashStart = 0;
    wdThunderFlashDuration = 0;
    wdLastThunderStorm = resetNow;
    wdNextThunderInterval = WD_THUNDER_MIN + (esp_random() % (WD_THUNDER_MAX - WD_THUNDER_MIN));

    steerAngle = 0.0f;
    imuRoll = 0.0f;
    imuPitch = 0.0f;
    attRollDeg = 0.0f;
    attPitchDeg = 0.0f;
    gyroBiasEst = 0.0f;
    wdTrafficFlare = 0.0f;
    wdTrafficFlareCol = 0;
    wdTrafficFlareCX = 160;
    wdTrafficFlareDir = 1;
    chairSettleActive = false;
    chairSettleStart = 0;
    sceneLastFrameMs = 0;

    apTargetSteer = 0.0f;
    apTargetPitch = 0.0f;
    apTargetRoll  = 0.0f;
    apSequenceSeed = wallHash((int)(resetNow & 0xFFFFu), 0x41,
                              resetNow ^ 0xA670u);
    apWaypointIndex = 0;
    apWaypointStartMs = 0;
    apWaypointIntervalMs = 0;
    apBankPending = false;
    apLastNewNets = UINT32_MAX;

    if (Teleport::isActive() && Teleport::getContext() == Teleport::Context::MENU_TO_WARDRIVE) {
        sceneEntryTransitionActive = false;
        sceneEntryTransitionStart = 0;
    } else {
        sceneEntryTransitionActive = true;
        sceneEntryTransitionStart = resetNow;
    }
}

void setParallaxEnabled(bool enabled) {
    parallaxEnabled = enabled;
    sceneBackdropCacheLastMs = UINT32_MAX;
    if (parallaxEnabled) {
        // IMU takes over — fade auto-pilot values to zero for smooth handoff
        apTargetSteer = 0.0f;
        apTargetPitch = 0.0f;
        apTargetRoll  = 0.0f;
        apWaypointStartMs = 0;
        apWaypointIntervalMs = 0;
    } else {
        // Auto-pilot takes over — seed from current values for smooth handoff
        apWaypointStartMs = 0;
        apWaypointIntervalMs = 0;
    }
}

void resumeFrameClock() {
    // The cockpit was not rendered while the telemetry tape owned the canvas.
    // Drop the stale delta without resetting any scene or session state.
    sceneLastFrameMs = 0;
    sceneBackdropCacheLastMs = UINT32_MAX;
}

void shutdown() {
    if (sceneTowerSlots) { heap_caps_free(sceneTowerSlots); sceneTowerSlots = nullptr; }
    if (sceneVisibleIdx) { heap_caps_free(sceneVisibleIdx); sceneVisibleIdx = nullptr; }
    releaseSceneBackdropCache();
    sceneEntryTransitionActive = false;
    sceneEntryTransitionStart = 0;
    chairSettleActive = false;
    chairSettleStart = 0;
    sceneLastFrameMs = 0;
    apTargetSteer = 0.0f;
    apTargetPitch = 0.0f;
    apTargetRoll  = 0.0f;
    apSequenceSeed = 0;
    apWaypointIndex = 0;
    apWaypointStartMs = 0;
    apWaypointIntervalMs = 0;
    apBankPending = false;
    apLastNewNets = UINT32_MAX;
}

void getCockpitPigCenter(int16_t& cx, int16_t& cy) {
    cx = q(PIG_CX);
    cy = q(PIG_TOP_Y) + 5 * pigCell();
}

void triggerChairSettle(uint32_t now) {
    chairSettleActive = true;
    chairSettleStart = now;
}

bool isChairSettling() {
    return chairSettleActive;
}

// ═══════════════════════════════════════════════════════════════════════════
// DRAW SCENE — per-frame orchestrator
// ═══════════════════════════════════════════════════════════════════════════

void drawScene(M5Canvas& canvas, uint32_t now) {
    const float frameScale = getFrameScale60Hz(now);
    RP::update();
    uint16_t fg = Display::getColorFG();
    if (Display::isTheOgTheme()) {
        WD_NEON  = Display::lerpColor565(RP::BG, RP::NEON,  0.78f);
        WD_AMBER = Display::lerpColor565(RP::BG, RP::WARM,  0.70f);
        WD_LED   = Display::lerpColor565(RP::BG, RP::LED,   0.72f);
        WD_SPARK = Display::lerpColor565(RP::BG, RP::SPARK, 0.80f);
    } else {
        WD_NEON  = Display::lerpColor565(RP::BG, Display::lerpColor565(RP::NEON,  fg, 0.22f), 0.78f);
        WD_AMBER = Display::lerpColor565(RP::BG, Display::lerpColor565(RP::WARM,  fg, 0.18f), 0.70f);
        WD_LED   = Display::lerpColor565(RP::BG, Display::lerpColor565(RP::LED,   fg, 0.14f), 0.72f);
        WD_SPARK = Display::lerpColor565(RP::BG, Display::lerpColor565(RP::SPARK, fg, 0.16f), 0.80f);
    }
    sceneFaceTimer.update(now, 0, false);
    updateTowerComms(now);
    if (SHOW_THUNDER) {
        updateWdThunder(now);
    } else {
        wdThunderFlashing = false;
        wdThunderFlashState = 0;
        wdThunderFlashesRemaining = 0;
        wdThunderBurstFiring = false;
    }
    float motion = 0.0f;
    if (parallaxEnabled) {
        updateSteerAngle(frameScale);
        motion = getMotion();
    } else {
        updateAutoPilot(frameScale, now);
        motion = getMotion();
    }

    wdTrafficFlare *= frameRetention(0.917f, frameScale);
    wdThunderFlickerBoost *= frameRetention(0.92f, frameScale);
    if (wdThunderFlickerBoost < 0.05f) wdThunderFlickerBoost = 0.0f;
    // ~2s half-life: the downpour outlives the flash by several seconds.
    wdStormIntensity *= frameRetention(0.994f, frameScale);
    if (wdStormIntensity < 0.02f) wdStormIntensity = 0.0f;

    // ==[ SCAN EVENTS ]== a fresh network jolts the cockpit and, when the
    // auto-pilot has the camera, pulls the next banking waypoint forward.
    uint32_t newNets = Wardrive::getSessionNewNets();
    if (apLastNewNets == UINT32_MAX) {
        apLastNewNets = newNets;          // first frame: adopt, do not fire
    } else if (newNets != apLastNewNets) {
        apLastNewNets = newNets;
        if (wdVibrateY == 0) wdVibrateY = PX;   // never stomp a thunder jolt
        requestAutoPilotBank(now);
    }

    // ==[ LAYER ORDER ]== city -> exterior rain -> cockpit structure/occupants
    // -> adhered canopy water -> curvature/HUD.
    bool backdropCopied = false;
    if (isSceneBackdropCacheReady()) {
        const bool refreshBackdrop = sceneBackdropCacheLastMs == UINT32_MAX ||
            wdThunderFlashing || wdThunderBurstFiring ||
            (uint32_t)(now - sceneBackdropCacheLastMs) >= WARTHOG_BACKDROP_MS;
        if (refreshBackdrop) {
            backdropCache.canvas->fillSprite(RP::BG);
            backdropCache.canvas->fillRect(0, 0, 320, WD_TOP_H, RP::DEEP);
            if (SHOW_CITY) {
                drawGlassBackdropBase(*backdropCache.canvas, now, motion);
            }
            sceneBackdropCacheLastMs = now;
        }
        uint16_t* dst = static_cast<uint16_t*>(canvas.getBuffer());
        const uint16_t* src = static_cast<const uint16_t*>(backdropCache.canvas->getBuffer());
        if (dst && src) {
            memcpy(dst, src, SCREEN_WIDTH * WD_GLASS_B * sizeof(uint16_t));
            canvas.fillRect(0, WD_GLASS_B, SCREEN_WIDTH,
                            SCREEN_HEIGHT - WD_GLASS_B, RP::BG);
            backdropCopied = true;
        }
    }
    if (!backdropCopied) {
        canvas.fillSprite(RP::BG);
        canvas.fillRect(0, 0, 320, WD_TOP_H, RP::DEEP);
        if (SHOW_CITY) {
            drawGlassBackdropBase(canvas, now, motion);
        }
    }
    if (SHOW_CITY) {
        drawGlassBackdropMotion(canvas, now, motion);
    }
    if (SHOW_RAIN) {
        // Falling precipitation is outside the vehicle. Cockpit structure,
        // yoke, chair, and Pancetta must occlude it.
        drawExteriorRainMotion(canvas, now, motion);
    }
    if (SHOW_SHELL) {
        drawCockpitShell(canvas, now, motion);
    }
    if (SHOW_CONSOLE) {
        drawConsoleLayer(canvas, now);
    }
    if (SHOW_CABLES) {
        drawDashboardCables(canvas, now, motion);
    }
    if (SHOW_MONITORS) {
        drawDashboardMonitors(canvas, now);
    }
    PigPose scenePose = computePigPose(now, motion);
    if (SHOW_REFLECTIONS) {
        drawDashReflections(canvas, now, motion);
    }
    if (SHOW_CHAIR) {
        drawChairPass(canvas, scenePose, false);
    }
    if (SHOW_WHEEL) {
        drawSteeringWheel(canvas, WHEEL_X, WHEEL_Y, WHEEL_W, WHEEL_H);
    }
    bool tpHidePig = Teleport::isActive() && Teleport::isPigHidden() &&
        (Teleport::getContext() == Teleport::Context::MENU_TO_WARDRIVE ||
         Teleport::getContext() == Teleport::Context::WARDRIVE_TO_MENU);
    if (SHOW_PIG && !tpHidePig) {
        drawPigRear(canvas, now, motion, scenePose);
    }
    if (SHOW_CHAIR) {
        drawChairPass(canvas, scenePose, true);
    }
    if (SHOW_REFLECTIONS) {
        drawCockpitLighting(canvas, scenePose);
    }
    if (Teleport::isActive()) {
        auto ctx = Teleport::getContext();
        if (ctx == Teleport::Context::MENU_TO_WARDRIVE ||
            ctx == Teleport::Context::WARDRIVE_TO_MENU) {
            Teleport::draw(canvas, Display::getColorFG(), RP::BG, now);
        }
    }
    if (SHOW_RAIN) {
        // Only water adhered to the near canopy returns above the occupants.
        drawCanopySurfaceWater(canvas, now, motion);
    }
    if (SHOW_CANOPY) {
        drawCanopyStreaks(canvas, now, motion);
    }
    if (SHOW_CURVATURE) {
        drawWindshieldCurvature(canvas, now, motion);
    }
    if (SHOW_SCANLINES) {
        drawCRTScanlines(canvas);
    }
    if (SHOW_TELEMETRY) {
        drawTelemetryText(canvas, now);
    }
    if (SHOW_ATTITUDE) {
        drawAttitudeIndicator(canvas);
    }
    if (SHOW_COMMS) {
        drawPigCommsBubble(canvas, now, motion, scenePose);
    }
    if (SHOW_HINTS) {
        drawDashHints(canvas, now);
    }
    drawSceneEntryTransition(canvas, now);
    Display::drawStatusBarTo(&canvas, "WARTHOG");
    Display::drawHintBottomBar(&canvas);
}

} // namespace WardriveScene
