/**
 * MenuPig — helper pig + 6-room roaming
 *
 * helper mode: pig sits on tech crate (right side of menu), speech bubble
 * explains currently highlighted menu item. neon sign flickers, pipe drips.
 * pig blinks, twitches ears, sniffs. full-size 72x40.
 *
 * roaming mode: 6 full-width rooms (320x212 playfield), pig walks between stations.
 * room 0: cyberdeck lab (dual monitors, server cluster, desk lamp, neon sign)
 * room 1: noir apartment (worn couch, venetian blind window, floor grate, wall pipes)
 * room 2: ramen bar (counter, noodle bowl, bar stools, coffin pod, neon sign)
 * room 3: surveillance nest (antenna array, satellite dish, rooftop shack, ledge)
 * room 4: underground bar (CRT terminal, barman, corner booth, THE PEN neon)
 * room 5: comfort balcony (steaming bath, rain glass, warm neon city ads)
 * rooms wrap circular: 0→1→2→3→4→5→0. pig walks to edge, cut to next room.
 *
 * rain-soaked industrial noir. concrete walls, metal floors, neon signs,
 * CRT glow, server LEDs, pipe drips.
 */

#include "menu_pig_internal.h"
#include "scene_cache.h"
#include "rooms/exterior_sprites.h"
#include "teleport.h"
#include "../modes/wardrive_scene.h"
#include "../piglet/weather.h"
#include "../piglet/mood.h"
#include "../defense/recon.h"
#include "../defense/xband.h"
#include "../defense/defense_pipeline.h"
#include "../defense/noir_narrator.h"
#include "../core/config.h"
#include "../core/capture.h"
#include "../core/achievements.h"
#include "../core/challenges.h"
#include "../core/frame_budget.h"
#include "../radio/c5monster_uart.h"
#include "../activity/pedometer.h"
#include "defhog_terminal.h"
#include "pancetta_cat.h"
#include "cat_behavior_director.h"
#include "smoke_volume.h"
#include "sky_volume.h"
#include "../audio/sfx.h"
#include "../audio/noir_jazz.h"
#include "../audio/bath_mic.h"
#include "npc/barman.h"
#include "progression_text.h"
#include "../util/time_math.h"
#include <esp_random.h>
#include <math.h>
#include <string.h>

namespace MenuPig {

// Enums, structs, shared constants, using-declarations — all in menu_pig_internal.h
static constexpr int NUM_ROOMS = 6;

// RoomMood definition (extern declared in menu_pig_internal.h)
RoomMood roomMood = {};
RoomProgressVisuals roomProgress = {};

// ==[ CONSTANTS ]== (shared: NEON_CYCLE_MS, BREATHE_MS, etc. in menu_pig_internal.h)
static constexpr uint32_t STATION_MIN_MS = 12000;
static constexpr uint32_t STATION_MAX_MS = 22000;
// One authored gait speed keeps short shuffles brisk and full crossings heavy.
static constexpr uint32_t WALK_MIN_MS    = 640;
static constexpr uint32_t WALK_MAX_MS    = 3600;
static constexpr float WALK_SPEED_PX_S   = 92.0f;
static constexpr uint32_t BENCH_LEAVE_MS = 1200;
static constexpr uint32_t BENCH_RETURN_MS = 1000;
static constexpr uint32_t SETTLE_MS      = 300;
static constexpr uint32_t JUMP_UP_MS     = 400;
static constexpr uint32_t JUMP_DOWN_MS   = 300;
static constexpr uint32_t BATH_JUMP_IN_MS = 680;
static constexpr uint32_t BATH_JUMP_OUT_MS = 560;
static constexpr float BATH_JUMP_HEIGHT = 28.0f;
// Each bath visit plans whole actions once on the update path.  A plan can be
// a quiet surface interval or a dive with its own cadence, depth, and recovery
// time; render passes only sample that plan and never reroll it.
static constexpr uint32_t BATH_SOAK_MIN_MS = 4800;
static constexpr uint32_t BATH_SOAK_MAX_MS = 15500;
static constexpr uint32_t BATH_PREP_MIN_MS = 520;
static constexpr uint32_t BATH_PREP_MAX_MS = 1280;
static constexpr uint32_t BATH_SINK_MIN_MS = 720;
static constexpr uint32_t BATH_SINK_MAX_MS = 1480;
static constexpr uint32_t BATH_RISE_MIN_MS = 680;
static constexpr uint32_t BATH_RISE_MAX_MS = 1520;
static constexpr uint32_t BATH_RECOVER_MIN_MS = 1800;
static constexpr uint32_t BATH_RECOVER_MAX_MS = 4600;
static constexpr uint32_t BATH_SUBMERGED_MIN_MS = 900;
static constexpr uint32_t BATH_SUBMERGED_MAX_MS = 6200;
static uint32_t bathCycleStartMs = 0;
static uint32_t bathSoakMs = 7000;
static uint32_t bathPrepMs = 800;
static uint32_t bathSinkMs = 1000;
static uint32_t bathDiveSubmergedMs = 2000;
static uint32_t bathRiseMs = 1000;
static uint32_t bathRecoverMs = 3200;
static int8_t bathDiveDepthPx = kR6_BathDiveMaxY;
static bool bathDivePlanned = true;
static bool bathCyclePlanned = false;
static constexpr uint32_t BLINK_MS       = 120;
static constexpr uint32_t SLEEP_PHRASE_MS = 4000;
static constexpr uint32_t STEAM_CYCLE_MS = 1800;
static constexpr int STEAM_MAX_RISE = 16;
static constexpr uint32_t DRIP_CYCLE_MS = 2400;
static constexpr uint32_t SPARK_MIN_MS = 4000;
static constexpr uint32_t SPARK_MAX_MS = 12000;
static constexpr uint32_t SPARK_FRAME_MS = 50;
static constexpr uint32_t LAPTOP_BLINK_MS = 1500;
static constexpr uint32_t LAPTOP_BUBBLE_MS = 5000;
static constexpr uint32_t WALL_BREAK_MIN_MS = 20000;
static constexpr uint32_t WALL_BREAK_MAX_MS = 45000;
static constexpr uint32_t WALL_BREAK_STARE_MS = 2500;
static constexpr int ROOM_CHANGE_CHANCE = 25;

// Helper copy is owned by the grouped menu catalog and passed into the
// renderer. This keeps the pig scene independent from menu row indexes.

// Laptop lines (cyberdeck flavor)
static const char* const LAPTOP_LINES[] = {
    "JACK IN >>",
    "ICE DETECTED",
    "NEURAL LINK OK",
    "DUMPING CORTEX",
    "MATRIX ACCESS",
    "FLATLINE CHECK",
    "CYBERSPACE RDY",
    "TRACE ROUTE >>"
};
static constexpr int LAPTOP_LINE_COUNT = sizeof(LAPTOP_LINES) / sizeof(LAPTOP_LINES[0]);

// ==[ STATE ]==
PigMode mode = PigMode::ON_BENCH;
float pigX, pigY;
bool faceRight = false;
static AvatarState avatarState = AvatarState::NEUTRAL;
int currentRoom = 0;    // 0=cyberdeck lab, 1=noir apartment, 2=ramen bar
Station currentStation = Station::AT_LAPTOP;
RoamState roamState = RoamState::IDLE;

// Walking
float walkFromX, walkFromY;
float walkToX, walkToY;
bool walkToFaceRight;
uint32_t walkStart;
Station walkTargetStation;
static int walkTargetRoom;
static float walkLegDist = 0.0f;  // accumulated horizontal distance for leg frame
static float walkLegBaseDist = 0.0f;
static uint32_t walkDurationMs = WALK_MAX_MS;

// A completed C5 snapshot is already produced by hamlet's shared bridge
// scheduler. Remember just its revision so Pancetta can acknowledge real RF
// evidence without starting a scan, retaining entries, or adding render work.
static uint32_t c5SensoryRevision = 0;
static bool c5SensoryRevisionKnown = false;

// Mount/dismount. Most furniture is vertical; the bath owns an X/Y rim arc.
static float jumpFromX, jumpToX;
static float jumpFromY, jumpToY;
static uint32_t jumpStart;
static float jumpSquashPx = 0.0f;  // >0 = body pushed down (squash), render-only offset

// Bench transition
static uint32_t transStart;
static float transFromX;
static bool returnToHelperAfterBath = false;

// Room transition
static int transFromRoom, transToRoom;
static uint32_t roomTransStart;
static int roomSlideDir;  // -1 = left, +1 = right

// ==[ TELEPORT PARTICLE SYSTEM ]== room transition decompose/reassemble
enum class TeleportContext : uint8_t {
    ROOM_TO_ROOM,
    HELPER_TO_ROOM
};
static constexpr uint32_t TP_DECOMPOSE_MS  = 800;
static constexpr uint32_t TP_VOID_MS       = 150;
static constexpr uint32_t TP_REASSEMBLE_MS = 600;
static constexpr uint32_t TP_SETTLE_MS     = 150;
static constexpr uint32_t TP_TOTAL_MS = TP_DECOMPOSE_MS + TP_VOID_MS +
                                        TP_REASSEMBLE_MS + TP_SETTLE_MS;  // 1700
static constexpr int PORTAL_X = 120;
static constexpr int PORTAL_Y = 32;
// Sibling portal origin — left edge, mid-height for sweeping arc to bar stool
static constexpr int SIB_PORTAL_X = 8;
static constexpr int SIB_PORTAL_Y = 100;
static constexpr int MAX_TELEPORT_PARTICLES = 64;
static constexpr int TP_PORTAL_R = 20;       // portal ring radius

static MenuPig::TeleportParticleSample teleportSourceParticles[MAX_TELEPORT_PARTICLES];
static MenuPig::TeleportParticleSample teleportDestinationParticles[MAX_TELEPORT_PARTICLES];
static int teleportSourceParticleCount = 0;
static int teleportDestinationParticleCount = 0;
static TeleportPhase teleportPhase = TeleportPhase::NONE;
static TeleportContext teleportContext = TeleportContext::ROOM_TO_ROOM;
static uint32_t teleportStart = 0;
static float teleportJumpFromX, teleportJumpFromY;
static int destPigCenterX, destPigCenterY;
static bool useTeleportTransition = false;

// ==[ COMPANION IN THE PORTAL ]== the cat rides the same jump on the same
// clock. He gets his own sampled shape and his own two endpoints, but he
// shares Pancetta's portal: both streams collapse into it and both come back
// out of it. The landing is latched at departure so the frame that hands his
// body back cannot disagree with the frame that took it.
static MenuPig::TeleportParticleSample
    catTeleportSourceParticles[Teleport::MAX_CAT_PARTICLES];
static MenuPig::TeleportParticleSample
    catTeleportDestParticles[Teleport::MAX_CAT_PARTICLES];
static uint8_t catTeleportSourceParticleCount = 0;
static uint8_t catTeleportDestParticleCount = 0;
static bool catTeleportActive = false;
static bool catTeleportLandingPending = false;
static float catTeleportFromX = 0.0f;
static float catTeleportFromY = 0.0f;
static int catTeleportDestCenterX = 0;
static int catTeleportDestCenterY = 0;
static PancettaCat::Pose catTeleportLanding = {};


// Cat progression shares the existing 32-bit persistent memory word without
// changing the NVS schema. Authored PancettaCat memories stay in the low half;
// the high half records room dossiers, behavior mastery, and one-time set
// completion bonuses.
static constexpr uint8_t CAT_DOSSIER_SHIFT = 16;
static constexpr uint32_t CAT_DOSSIER_BITS = 0x3Fu << CAT_DOSSIER_SHIFT;
static constexpr uint32_t CAT_DOSSIER_COMPLETE_BIT = 1u << 22;
static constexpr uint8_t CAT_MASTERY_SHIFT = 23;
static constexpr uint32_t CAT_MASTERY_BITS = 0x7Fu << CAT_MASTERY_SHIFT;
static constexpr uint32_t CAT_MASTERY_COMPLETE_BIT = 1u << 30;
static_assert((uint8_t)PancettaCat::Memory::COUNT <= CAT_DOSSIER_SHIFT,
              "Cat memory namespace overlaps behavior progression bits");
static CatBehavior::Decision catBehaviorDecision = {};

// Settling (turn delay at rear-view stations)
static uint32_t settleStart;

// Station timer
static uint32_t stationStart;
static uint32_t stationDuration;

// DEFHOG4 terminal delayed appearance
static uint32_t terminalDelayEnd = 0;     // when delay expires
static bool terminalDelayActive = false;   // waiting to maybe show
static bool terminalDecided = false;       // already rolled dice this station

// ==[ GAMIFICATION V3 ]== room tracking
static uint8_t roomsVisitedMask = 0;       // 6-bit bitmask (1 per room)
static bool fullCircuitClaimed = false;    // prevents re-trigger
static uint32_t stationStayStart = 0;      // current visit start (surveillance reward)
static constexpr uint8_t STATION_COUNT = 10;
static constexpr uint32_t STAKEOUT_MS = 300000;
static_assert((uint8_t)Station::IN_BATH + 1 == STATION_COUNT,
              "station dwell ledger must cover every station");
static uint32_t stationDwellMs[STATION_COUNT] = {0};
static uint32_t lastStationDwellTick = 0;
static bool stakeoutClaimed = false;
static uint32_t lastAlertDripTime = 0;    // room mood momentum drip timer
static bool trackerSurvivalAwarded = false; // one-shot per station stay
static bool eventHold = false;
static uint32_t eventHoldStarted = 0;

// Environmental evidence changes on human-scale events, not at 60 fps. Keep
// achievement reads out of the hot render loop without making a new unlock
// feel delayed.
static constexpr uint32_t ROOM_PROGRESS_REFRESH_MS = 500;
static uint32_t roomProgressLastRefresh = 0;

static void refreshRoomProgressVisuals(uint32_t now) {
    if (roomProgressLastRefresh != 0 &&
        now - roomProgressLastRefresh < ROOM_PROGRESS_REFRESH_MS) return;

    const bool persistedCircuit =
        Achievements::has(Achievement::FULL_CIRCUIT);
    // FULL_CIRCUIT existed when the house had five rooms. That persisted bit
    // proves the original circuit, but not a visit to the newer balcony. Keep
    // those five witnesses lit and let live room evidence earn bit 5.
    const uint8_t persistedRoomMask = persistedCircuit ? 0x1Fu : 0u;
    roomProgress.roomsVisitedMask = persistedRoomMask | roomsVisitedMask;
    roomProgress.stakeout = stakeoutClaimed ||
                            Achievements::has(Achievement::STAKEOUT);

    uint8_t station = (uint8_t)currentStation;
    uint32_t dwell = station < STATION_COUNT ? stationDwellMs[station] : 0;
    roomProgress.stakeoutPips = roomProgress.stakeout ? 5 :
        (uint8_t)min(5u, dwell / 60000u);
    roomProgressLastRefresh = now;
}

// Idle animations — shared timer for blink + ear twitch
PigFaceTimer faceTimer;

// Sleep speech bubble
static const char* const SLEEP_PHRASES[] = {
    "zzz...",
    "* snork *",
    "oink.. zz..",
    "5 more min...",
    "dreaming of\nhandshakes...",
    "* snore *",
};
static constexpr int SLEEP_PHRASE_COUNT = sizeof(SLEEP_PHRASES) / sizeof(SLEEP_PHRASES[0]);
static uint8_t sleepPhraseIdx = 0;
static uint32_t lastSleepPhrase = 0;

// Neon sign animation
uint32_t neonCycleStart = 0;


// Pipe drip
uint32_t dripCycleStart = 0;

// Wire spark
uint32_t nextSparkTime = 0;
uint32_t sparkStart = 0;
bool sparkActive = false;


// Coffin pod LED
uint32_t podLedStart = 0;
bool podLedOn = true;

// Laptop
bool laptopScreenOn = true;
uint32_t lastLaptopBlink = 0;
uint8_t laptopLineIdx = 0;
uint32_t lastLaptopBubble = 0;
bool laptopMomentumAwarded = false;

// Narrator bubble (IPP-aware roaming speech)
static char narratorBubbleText[96] = "";
static uint32_t lastNarratorBubble = 0;
static constexpr uint32_t NARRATOR_BUBBLE_MS = 6000;
static void refreshNarratorBubble();  // forward decl

// ==[ REAR CINEMATIC ]== 3-sec pre-teleport visual sequence
RearCinematicPhase rearCinePhase = RearCinematicPhase::INACTIVE;
static uint32_t rearCineStart = 0;
static uint32_t rearCinePhaseStart = 0;
static int16_t rearTailOffsetY = 0;     // 0 = butt level (correct), 24 = ear level (bug)
static uint8_t rearEyePhase = 0;        // 0=blank, 1=dots, 2=full eyes
bool rearCineOverrideRear = false; // force rear view during cinematic

// ==[ ROOM TRANSITION QUIPS ]== R&M portal + walk sass
static const char* const PORTAL_QUIPS[] = {
    "in and out. 20 min adventure.",
    "dont think about it.",
    "wubba lubba oink oink.",
    "infinite rooms. dimension C-137.",
    "portal gun go brrr.",
    "nobody exists on purpose.\nnobody scans on purpose.",
};
static constexpr int PORTAL_QUIP_COUNT = sizeof(PORTAL_QUIPS) / sizeof(PORTAL_QUIPS[0]);

static const char* const WALK_QUIPS[] = {
    "yeaah you could just walk too.",
    "legs. original teleportation.",
    "portals are for tryhards.",
    "old school. respect.",
};
static constexpr int WALK_QUIP_COUNT = sizeof(WALK_QUIPS) / sizeof(WALK_QUIPS[0]);

// 4th wall break
bool wallBreakActive = false;
static uint32_t wallBreakStart = 0;
static uint32_t nextWallBreak = 0;
static bool savedFaceRight = false;

// Coffee cup
bool carryingCup = false;
uint32_t cupPickupTime = 0;
static constexpr uint32_t CUP_CARRY_MS = 12000;
// Portal jump
bool portalJumpActive = false;
uint32_t portalJumpStart = 0;
static constexpr uint32_t PORTAL_JUMP_MS = 380;
static constexpr int PORTAL_JUMP_HEIGHT = 8;

NoirVolumetricPassState noirPass;
ColorEventSample colorEvent;
// Cup steam inertia (movement-reactive, dissipating wisps)
static int cupSteamLagX = 0;
static int cupSteamLagY = 0;
static int cupSteamPrevX = 0;
static int cupSteamPrevY = 0;
static bool cupSteamPrimed = false;
// Noir window cigarette state (persists until next station arrival)
bool windowCigLit = false;

struct HeldRamenSlurpRuntime {
    bool initialized = false;
    uint32_t lastNow = 0;
    uint32_t eventStart[3] = {0, 0, 0};
    uint32_t eventTTL[3] = {0, 0, 0};
    uint32_t nextEvent[3] = {0, 0, 0};
    int8_t ampPx[3] = {0, 0, 0};
};
static HeldRamenSlurpRuntime ramenSlurpRuntime;
Room2LightingFrame room2LightingRuntime;
DebugRoamingFrameOverride debugRoamingFrame;
bool room3CinematicCarRunning = false;

static constexpr int kPigEffectSnapMaxW = kPigW;
static constexpr int kPigEffectSnapMaxH = kPigH + 12;
static constexpr int kPigEffectSnapCols =
    (kPigEffectSnapMaxW + kPigPX - 1) / kPigPX;
static constexpr int kPigEffectSnapRows =
    (kPigEffectSnapMaxH + kPigPX - 1) / kPigPX;
static constexpr size_t kPigEffectSnapPixels =
    (size_t)kPigEffectSnapCols * (size_t)kPigEffectSnapRows;

struct PigEffectSnapshot {
    bool valid = false;
    int16_t x = 0;
    int16_t y = 0;
    int16_t w = 0;
    int16_t h = 0;
    int16_t gridW = 0;
    int16_t gridH = 0;
    int16_t centerX = 0;
    int16_t centerY = 0;
    PigLight light = {};
    // Fixed 2px-cell substrate cache: render paths never allocate, and this
    // remains a bounded ~2 KB BSS cost instead of fragmenting PSRAM on first use.
    uint16_t pixels[kPigEffectSnapPixels];
};
static PigEffectSnapshot pigEffectSnapshot;

// ==[ ROOM 3 CAR EVENT STATE ]== file-scope for WD mode access
Room3CarEventState carState;
bool wdCarForceStart = false;  // set by startWardriveEntry, checked by drawRoom3CinematicCar

// ==[ WD CINEMATIC STATE ]== (forward-declared here for arriveAtStation + drawRoom3CinematicCar)
WDCinePhase wdCinePhase = WDCinePhase::NONE;
uint32_t wdCineStart = 0;

// ==[ WD MOUNT SUB-STATE ]== pig walks to car then jumps onto roof
WDMountPhase wdMountPhase = WDMountPhase::IDLE;
uint32_t wdMountStart = 0;
uint32_t wdImpactStart = 0;
float wdMountFromX = 0, wdMountFromY = 0;
float wdJumpFromX = 0, wdJumpFromY = 0;

WDReturnPhase wdReturnPhase = WDReturnPhase::NONE;
uint32_t wdReturnStart = 0;
float wdReturnLandX = 0.0f;
float wdReturnLandY = 0.0f;

// ==[ TELEPORT-BASED WD EXIT ]== pre-selected destination for WARDRIVE_TO_MENU
static uint8_t pendingExitRoom = 0;
static Station pendingExitStation = Station::AT_LAPTOP;

// Furniture wobble
int wobbleX = 0, wobbleY = 0;
uint32_t wobbleStart = 0;

// Bed momentum drip (needs reset on arrival)
uint32_t lastBedMomentum = 0;
static constexpr uint32_t kChairLegBurstMs = 360;
static constexpr uint32_t kChairLegBurstMinGapMs = 7000;
static constexpr uint32_t kChairLegBurstMaxGapMs = 15000;
uint32_t nextChairLegBurstAtMs = 0;
uint32_t chairLegBurstStartMs = 0;

// ==[ ARRIVAL MICRO-ANIMATION ]== brief visual flourish when pig reaches station
uint32_t arrivalAnimStart = 0;
static constexpr uint32_t ARRIVAL_ANIM_MS = 500;  // max duration of any arrival effect

// ==[ IDLE FIDGETS ]== random micro-behaviors during station idle
static uint32_t nextFidgetTime = 0;
static uint32_t fidgetStart = 0;
static bool fidgetRunning = false;
static constexpr uint32_t FIDGET_MS = 800;  // max fidget duration

enum class CharacterFidget : uint8_t {
    NOTICE,
    SNIFF
};
static CharacterFidget characterFidget = CharacterFidget::NOTICE;

static void resetCharacterFidget(uint32_t now,
                                 uint32_t minGapMs = 4800,
                                 uint32_t maxGapMs = 8200) {
    fidgetStart = 0;
    fidgetRunning = false;
    characterFidget = CharacterFidget::NOTICE;
    nextFidgetTime = now + randomRange(minGapMs, maxGapMs);
}

// Station fidgets are character beats, not incidental redraws. Their clocks
// advance in update(), then the room and pig renderers consume one shared
// sample so a busy display path cannot make a prop move on its own.
static bool isStationFidgetActive(uint32_t now) {
    return mode == PigMode::ROAMING &&
           roamState == RoamState::IDLE &&
           fidgetRunning &&
           characterFidget == CharacterFidget::NOTICE &&
           now - fidgetStart < FIDGET_MS;
}

static bool canCharacterSniffHere();

static bool isCharacterSniffActive(uint32_t now) {
    return canCharacterSniffHere() &&
           fidgetRunning &&
           characterFidget == CharacterFidget::SNIFF &&
           now - fidgetStart < FIDGET_MS;
}

static uint8_t sampleCharacterSniffFrame(uint32_t now) {
    // Two deliberate nostril passes with neutral bookends. The complete plan
    // is selected when the fidget starts; rendering only samples its age.
    static constexpr uint8_t kFrames[8] = {0, 1, 2, 0, 1, 2, 1, 0};
    uint32_t frame = (now - fidgetStart) / (FIDGET_MS / 8u);
    if (frame >= 8u) frame = 7u;
    return kFrames[frame];
}

static int calcCharacterSniffHeadBob(uint32_t now) {
    if (!isCharacterSniffActive(now)) return 0;
    const uint32_t frame = (now - fidgetStart) / (FIDGET_MS / 8u);
    return (frame == 1u || frame == 2u || frame == 4u || frame == 5u)
        ? -kPigPX : 0;
}

static bool canCharacterSniffHere() {
    if (mode == PigMode::ON_BENCH) return true;
    if (mode != PigMode::ROAMING || roamState != RoamState::IDLE) return false;
    // These are the awake, unobstructed side poses. Ramen, bath and sleep own
    // stronger authored face/body actions and rear stations hide the snout.
    return currentStation == Station::ON_LEDGE ||
           currentStation == Station::AT_BOOTH;
}

static PigEyeLook stationFidgetLook(Station station) {
    switch (station) {
        case Station::ON_LEDGE:
            return PigEyeLook::FRONT_UP;    // scanning the open sky
        case Station::AT_BOOTH:
            return PigEyeLook::CENTER;      // acknowledge the table-side glint
        case Station::COOKING:
            return PigEyeLook::FRONT_DOWN;  // keep attention on the bowl
        default:
            return PigEyeLook::NONE;        // rear/sleep poses stay quiet
    }
}

static void updateStationFidget(uint32_t now) {
    const bool characterIdle =
        mode == PigMode::ON_BENCH ||
        (mode == PigMode::ROAMING && roamState == RoamState::IDLE);
    if (!characterIdle) {
        fidgetRunning = false;
        return;
    }

    if (fidgetRunning) {
        if (now - fidgetStart < FIDGET_MS) return;
        fidgetRunning = false;
        nextFidgetTime = now + randomRange(6500, 11000);
        return;
    }

    if ((int32_t)(now - nextFidgetTime) < 0) return;

    fidgetStart = now;
    fidgetRunning = true;
    characterFidget =
        canCharacterSniffHere() && randomRange(0, 99) < 55
            ? CharacterFidget::SNIFF
            : CharacterFidget::NOTICE;

    if (characterFidget == CharacterFidget::SNIFF) {
        faceTimer.noticeSensoryEvent(now, PigEyeLook::FRONT_DOWN, FIDGET_MS);
        return;
    }

    const PigEyeLook look = stationFidgetLook(currentStation);
    if (look != PigEyeLook::NONE)
        faceTimer.noticeSensoryEvent(now, look, 520);
    else if (currentStation == Station::AT_LAPTOP ||
             currentStation == Station::AT_WINDOW ||
             currentStation == Station::AT_ANTENNA ||
             currentStation == Station::AT_TERMINAL) {
        // A rear-facing pig cannot sell gaze, but one short ear response lets
        // the player read that Pancetta—not the scenery—noticed the cue.
        faceTimer.triggerEarTwitch(now);
    }
}

static int calcBoothFidgetHeadBob(uint32_t now) {
    if (!isStationFidgetActive(now) || currentStation != Station::AT_BOOTH)
        return 0;
    static constexpr int8_t kBob[4] = {0, -2, -2, 0};
    const uint32_t age = now - fidgetStart;
    const uint32_t frame = age / 105u;
    return frame < 4u ? kBob[frame] : 0;
}

// ==[ WALKING DUST PUFFS ]== tiny particles at pig feet during walk
struct DustPuff {
    int16_t x, y;
    uint32_t spawnTime;
    bool active;
};
static constexpr int MAX_DUST_PUFFS = 4;
static DustPuff dustPuffs[MAX_DUST_PUFFS] = {};
static int8_t lastFootLandingFrame = -1;

// ==[ BATH WET TRACKS ]== fixed deck marks; no heap, no per-frame churn
struct BathWetTrack {
    int16_t x, y;
    uint32_t spawnTime;
    bool active;
    bool pointsRight;
};
static constexpr int MAX_BATH_WET_TRACKS = 12;
static constexpr uint32_t BATH_WET_TRACK_MS = 7000;
static BathWetTrack bathWetTracks[MAX_BATH_WET_TRACKS] = {};
static uint8_t nextBathWetTrack = 0;

static void clearBathWetTracks() {
    for (int i = 0; i < MAX_BATH_WET_TRACKS; ++i) bathWetTracks[i] = {};
    nextBathWetTrack = 0;
}

static constexpr int kRoomWalkFrameCount = 8;
static constexpr float kPixelsPerLegFrame = 10.0f;  // ~10px travel per leg frame advance
static constexpr int kRoomLegW = 12;
static constexpr int kRoomLegH = 8;
// Upright walk: two short stubs and two attached counter-swinging arms.
// Walking paws are smaller than held-prop hands so the side silhouette reads
// as one bipedal body instead of four equally loud satellite shapes.
static constexpr int kRoomWalkLegW = 14;
static constexpr int kRoomWalkLegH = 4;
static constexpr int kRoomWalkArmW = 12;
static constexpr int kRoomWalkArmH = 8;
// The raised half-stride reaches drawY+30. Keep the walk paws below the
// direct face (eyes end at drawY+25) so neither mirrored gait can cover a gaze.
static constexpr int kRoomWalkArmBaseY = 32;
static const int8_t kWalkBackDx[kRoomWalkFrameCount]  = {0, -4, -4, 0, 0, 4, 4, 0};
static const int8_t kWalkFrontDx[kRoomWalkFrameCount] = {0, 4, 4, 0, 0, -4, -4, 0};
static const int8_t kWalkBackDy[kRoomWalkFrameCount]  = {0, 0, -4, -4, 0, 0, -4, -4};
static const int8_t kWalkFrontDy[kRoomWalkFrameCount] = {-4, -4, 0, 0, -4, -4, 0, 0};

// Puddle ripple (helper drip splash)
static int rippleRadius = 0;
static uint32_t rippleStart = 0;
static constexpr uint32_t RIPPLE_MS = 600;

// Room 1 wall pipe drip
uint32_t roomDripStart = 0;


// Helper mode state
static int lastHelperIdx = -1;

// ==[ HELPERS ]==

float smootherstep(float t) {
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

static const char* skipAsciiSpaces(const char* s) {
    while (s && (*s == ' ' || *s == '\t')) ++s;
    return s;
}

static bool equalsIgnoreCaseAscii(const char* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *b) {
        char ca = *a++;
        char cb = *b++;
        if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 'a' + 'A');
        if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 'a' + 'A');
        if (ca != cb) return false;
    }
    return (*a == '\0' && *b == '\0');
}

int calcBreathe(uint32_t now) {
    uint32_t bp = now % BREATHE_MS;
    int raw;
    if (bp < 1500) raw = -(int)(bp * 2 / 1500);
    else raw = -(int)((3000 - bp) * 2 / 1500);
    return raw & ~1;  // snap to 2px grid
}

int calcWalkBounce() {
    // Rise between alternating contacts; return to ground on every touchdown.
    static const int bounce[8] = {0, -2, 0, -2, 0, -2, 0, -2};
    int frame = (int)(walkLegDist / kPixelsPerLegFrame) % 8;
    return bounce[frame];
}

uint32_t randomRange(uint32_t lo, uint32_t hi) {
    return lo + (esp_random() % (hi - lo + 1));
}

static uint32_t authoredWalkDuration(float fromX, float fromY,
                                     float toX, float toY) {
    // Horizontal span owns the gait; a smaller vertical component keeps the
    // rare floor-level diagonal from looking unnaturally fast.
    float span = fabsf(toX - fromX) + fabsf(toY - fromY) * 0.35f;
    uint32_t duration = (uint32_t)lroundf(span * 1000.0f / WALK_SPEED_PX_S);
    if (duration < WALK_MIN_MS) duration = WALK_MIN_MS;
    if (duration > WALK_MAX_MS) duration = WALK_MAX_MS;
    return duration;
}

static inline void snapPortalAnchor(int16_t& x, int16_t& y) {
    if (x < 12) x = 12;
    if (x > (int16_t)(SCREEN_WIDTH - 92)) x = (int16_t)(SCREEN_WIDTH - 92);
    if (y < (int16_t)(kRoomY + 2)) y = (int16_t)(kRoomY + 2);
    if (y > (int16_t)(kFloorY - kPigH - 2)) y = (int16_t)(kFloorY - kPigH - 2);
    x = (int16_t)(x & ~1);
    y = (int16_t)(y & ~1);
}

void calcWobble(uint32_t now) {
    if (wobbleStart == 0 || now - wobbleStart >= WOBBLE_MS) {
        wobbleX = 0; wobbleY = 0; return;
    }
    float t = (float)(now - wobbleStart) / (float)WOBBLE_MS;
    float decay = 1.0f - t;
    // Keep wobble on fat-pixel grid so room props and pig stay phase-aligned.
    float rawX = fastSinf(t * 12.0f * 3.14159f) * 2.0f * decay;
    float rawY = fastSinf(t * 15.0f * 3.14159f) * 1.2f * decay;
    int qx = (int)lroundf(rawX / (float)kPigPX) * kPigPX;
    int qy = (int)lroundf(rawY / (float)kPigPX) * kPigPX;
    if (qx > kPigPX) qx = kPigPX;
    if (qx < -kPigPX) qx = -kPigPX;
    if (qy > kPigPX) qy = kPigPX;
    if (qy < -kPigPX) qy = -kPigPX;
    wobbleX = qx;
    wobbleY = qy;
}

bool isChairLegBurstActive(uint32_t now) {
    if (!(mode == PigMode::ROAMING &&
          roamState == RoamState::IDLE &&
          currentStation == Station::COOKING)) {
        nextChairLegBurstAtMs = 0;
        chairLegBurstStartMs = 0;
        return false;
    }

    if (chairLegBurstStartMs != 0) {
        if (now - chairLegBurstStartMs < kChairLegBurstMs) return true;
        chairLegBurstStartMs = 0;
        nextChairLegBurstAtMs = now + randomRange(kChairLegBurstMinGapMs, kChairLegBurstMaxGapMs);
        return false;
    }

    if (nextChairLegBurstAtMs == 0) {
        nextChairLegBurstAtMs = now + randomRange(kChairLegBurstMinGapMs, kChairLegBurstMaxGapMs);
        return false;
    }

    if (TimeMath::reached(now, nextChairLegBurstAtMs)) {
        chairLegBurstStartMs = now;
        return true;
    }
    return false;
}

int calcChairUpperBodyBob(uint32_t now) {
    static const int8_t kBobSeq[8] = {-1, -1, 0, 0, 1, 1, 0, 0};
    return kBobSeq[(now / 240) % 8];
}

void drawChairUpperBodySeam(M5Canvas& canvas, int x, int y, int upperBobY,
                                   uint16_t pigFill, uint16_t bg) {
    if (upperBobY == 0) return;

    static const int8_t openCurve[7]  = {8, 6, 4, 4, 4, 6, 8};
    static const int8_t closeCurve[7] = {2, 4, 6, 6, 6, 4, 2};
    int parenLX = x;
    int parenRX = x + 60;
    int seamY = y + 28 + ((upperBobY < 0) ? upperBobY : 0);
    int seamH = (upperBobY < 0) ? -upperBobY : upperBobY;
    int fillX = parenLX + openCurve[6];
    int fillW = (parenRX + closeCurve[6]) - fillX;
    if (fillW <= 0 || seamH <= 0) return;

    canvas.fillRect(fillX, seamY, fillW, seamH, pigFill);
    canvas.fillRect(fillX - 2, seamY, 2, seamH, bg);
    canvas.fillRect(fillX + fillW, seamY, 2, seamH, bg);
}

// ==[ POSE RESOLUTION ]== single source of truth for body/legs/noodle alignment.

// Teleport hides the destination pig until particles converge.
static bool isPortalReconstructing() {
    return Teleport::isPigHidden();
}

static void clearWDReturnState() {
    wdReturnPhase = WDReturnPhase::NONE;
    wdReturnStart = 0;
    wdReturnLandX = 0.0f;
    wdReturnLandY = 0.0f;
}

static bool isWDReturnActive() {
    return wdReturnPhase != WDReturnPhase::NONE ||
           wdCinePhase == WDCinePhase::WD_RETURN ||
           carState.wdReturnMode;
}

// WD cinematic in progress — pig is locked in scene, no relocations allowed
bool isWDCinematicActive() {
    return wdCinePhase != WDCinePhase::NONE ||
           wdMountPhase != WDMountPhase::IDLE ||
           carState.wdMode ||
           isWDReturnActive();
}

static void updateStationDwell(uint32_t now) {
    if (lastStationDwellTick == 0) {
        lastStationDwellTick = now;
        return;
    }

    uint32_t delta = now - lastStationDwellTick;
    lastStationDwellTick = now;
    if (eventHold || roamState != RoamState::IDLE || isWDCinematicActive() || stationDuration == 0) return;
    if (delta > 1000) delta = 1000;  // never count time spent outside the menu.

    uint8_t station = (uint8_t)currentStation;
    if (station >= STATION_COUNT) return;
    if (stationDwellMs[station] < STAKEOUT_MS) {
        uint32_t room = STAKEOUT_MS - stationDwellMs[station];
        stationDwellMs[station] += (delta > room) ? room : delta;
    }
    if (!stakeoutClaimed && stationDwellMs[station] >= STAKEOUT_MS) {
        stakeoutClaimed = true;
        roomProgressLastRefresh = 0;
        bool unlocked = Achievements::tryUnlock(Achievement::STAKEOUT);
        if (unlocked && DefhogTerminal::isVisible()) {
            DefhogTerminal::pushLineHype("ST4K30UT. 5M 0N TH3 C4S3.");
        }
    }
}

bool isWDMountFrontViewLocked() {
    if (isWDReturnActive()) return true;
    return wdMountPhase != WDMountPhase::IDLE &&
           wdMountPhase != WDMountPhase::INSIDE;
}

bool isWDPigInsideCar() {
    return wdMountPhase == WDMountPhase::INSIDE ||
           wdReturnPhase == WDReturnPhase::CAR_ARRIVE;
}

bool isWDPigSneakingIntoCabin() {
    return wdMountPhase == WDMountPhase::SNEAK_IN ||
           wdReturnPhase == WDReturnPhase::UNSNEAK;
}

float getWDCanopySwitchFade(uint32_t now) {
    if (!carState.active || !carState.wdMode) return 0.0f;

    uint32_t rawElapsed = now - carState.startMs;
    uint32_t elapsed = (carState.wdMode && isWDMenuCineFast())
        ? wdMenuCineElapsed(rawElapsed) : rawElapsed;
    if (elapsed <= ROOM3_CAR_LINGER_END_MS || elapsed >= ROOM3_CAR_ACCEL_END_MS) return 0.0f;

    float accelT = (float)(elapsed - ROOM3_CAR_LINGER_END_MS) / (float)ROOM3_CAR_ACCEL_DUR_MS;
    float fadeStart = (float)WD_CANOPY_SWITCH_FADE_START_PCT / 100.0f;
    float fadeEnd = (float)WD_CANOPY_SWITCH_CUT_PCT / 100.0f;
    return smootherstep((accelT - fadeStart) / (fadeEnd - fadeStart));
}

void drawWDCanopySwitchOverlay(M5Canvas& canvas, uint32_t now) {
    float fade = getWDCanopySwitchFade(now);
    if (fade <= 0.0f) return;

    uint16_t fadeCol = Display::getColorBG();
    if (fade >= 0.999f) {
        canvas.fillRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, fadeCol);
        return;
    }

    // bayer4 holds 0..240, so the threshold has to be on the same scale. The
    // old 1..16 range only ever matched the single zero cell, which pinned the
    // fade at a 1/16 stipple instead of ramping it to full coverage.
    int threshold = max(1, min(256, (int)ceilf(fade * 256.0f)));
    for (int y = 0; y < SCREEN_HEIGHT; y += kRoomPX) {
        for (int x = 0; x < SCREEN_WIDTH; x += kRoomPX) {
            if (bayer4[(y / kRoomPX) & 3][(x / kRoomPX) & 3] < threshold) {
                canvas.fillRect(x, y, kRoomPX, kRoomPX, fadeCol);
            }
        }
    }
}

static bool shouldSuppressRoamBubble() {
    return room3CinematicCarRunning || isWDCinematicActive();
}


static uint8_t catDossierMask(uint32_t persistedMask) {
    return (uint8_t)((persistedMask & CAT_DOSSIER_BITS) >>
                     CAT_DOSSIER_SHIFT);
}

static uint8_t catMasteryMask(uint32_t persistedMask) {
    return (uint8_t)((persistedMask & CAT_MASTERY_BITS) >>
                     CAT_MASTERY_SHIFT);
}

static uint8_t catLegacyMemoryCount(uint32_t persistedMask) {
    // Keep the original actor memories logically separate even though the
    // same NVS word now carries high-bit behavior progression as well.
    return PancettaCat::memoryCount(
        persistedMask & PancettaCat::kAllMemoryBits);
}

static CatBehavior::Intent catBehaviorIntent(
        PancettaCat::SceneIntent intent) {
    // Only special scene intents need explicit translation. Any base or future
    // intent falls back to ordinary follow behavior without depending on a
    // PancettaCat enum member named FOLLOW.
    if (intent == PancettaCat::SceneIntent::ROOM_INTEREST)
        return CatBehavior::Intent::ROOM_INTEREST;
    if (intent == PancettaCat::SceneIntent::COMPANY)
        return CatBehavior::Intent::COMPANY;
    if (intent == PancettaCat::SceneIntent::EXIT)
        return CatBehavior::Intent::EXIT;
    return CatBehavior::Intent::FOLLOW;
}

static CatBehavior::Action catBehaviorAction(
        PancettaCat::Activity activity) {
    switch (activity) {
        case PancettaCat::Activity::FOLLOW:
            return CatBehavior::Action::FOLLOW;
        case PancettaCat::Activity::WATCH_CABLES:
            return CatBehavior::Action::WATCH_CABLES;
        case PancettaCat::Activity::WATCH_RAIN:
            return CatBehavior::Action::WATCH_RAIN;
        case PancettaCat::Activity::SNIFF_FOOD:
            return CatBehavior::Action::SNIFF_FOOD;
        case PancettaCat::Activity::STALK_SKY:
            return CatBehavior::Action::STALK_SKY;
        case PancettaCat::Activity::PROWL_BAR:
            return CatBehavior::Action::PROWL_BAR;
        case PancettaCat::Activity::WATCH_WATER:
            return CatBehavior::Action::WATCH_WATER;
        case PancettaCat::Activity::SCRATCH:
            return CatBehavior::Action::SCRATCH;
        case PancettaCat::Activity::SLEEP:
            return CatBehavior::Action::SLEEP;
        case PancettaCat::Activity::GROOM:
            return CatBehavior::Action::GROOM;
        case PancettaCat::Activity::HAIRBALL:
            return CatBehavior::Action::HAIRBALL;
        case PancettaCat::Activity::ARCH:
            return CatBehavior::Action::ARCH;
        case PancettaCat::Activity::ZOOMIES:
            return CatBehavior::Action::ZOOMIES;
        case PancettaCat::Activity::FACE_BUMP:
            return CatBehavior::Action::FACE_BUMP;
        case PancettaCat::Activity::HEAD_NAP:
            return CatBehavior::Action::HEAD_NAP;
        case PancettaCat::Activity::KNEAD:
            return CatBehavior::Action::KNEAD;
        case PancettaCat::Activity::SLOW_BLINK:
            return CatBehavior::Action::SLOW_BLINK;
        case PancettaCat::Activity::MEOW:
            return CatBehavior::Action::MEOW;
    }
    return CatBehavior::Action::FOLLOW;
}

static PancettaCat::Activity catActivityForBehavior(
        CatBehavior::Action action) {
    switch (action) {
        case CatBehavior::Action::FOLLOW:
            return PancettaCat::Activity::FOLLOW;
        case CatBehavior::Action::WATCH_CABLES:
            return PancettaCat::Activity::WATCH_CABLES;
        case CatBehavior::Action::WATCH_RAIN:
            return PancettaCat::Activity::WATCH_RAIN;
        case CatBehavior::Action::SNIFF_FOOD:
            return PancettaCat::Activity::SNIFF_FOOD;
        case CatBehavior::Action::STALK_SKY:
            return PancettaCat::Activity::STALK_SKY;
        case CatBehavior::Action::PROWL_BAR:
            return PancettaCat::Activity::PROWL_BAR;
        case CatBehavior::Action::WATCH_WATER:
            return PancettaCat::Activity::WATCH_WATER;
        case CatBehavior::Action::SCRATCH:
            return PancettaCat::Activity::SCRATCH;
        case CatBehavior::Action::SLEEP:
            return PancettaCat::Activity::SLEEP;
        case CatBehavior::Action::GROOM:
            return PancettaCat::Activity::GROOM;
        case CatBehavior::Action::HAIRBALL:
            return PancettaCat::Activity::HAIRBALL;
        case CatBehavior::Action::ARCH:
            return PancettaCat::Activity::ARCH;
        case CatBehavior::Action::ZOOMIES:
            return PancettaCat::Activity::ZOOMIES;
        case CatBehavior::Action::FACE_BUMP:
            return PancettaCat::Activity::FACE_BUMP;
        case CatBehavior::Action::HEAD_NAP:
            return PancettaCat::Activity::HEAD_NAP;
        case CatBehavior::Action::KNEAD:
            return PancettaCat::Activity::KNEAD;
        case CatBehavior::Action::SLOW_BLINK:
            return PancettaCat::Activity::SLOW_BLINK;
        case CatBehavior::Action::MEOW:
            return PancettaCat::Activity::MEOW;
        case CatBehavior::Action::COUNT:
            break;
    }
    return PancettaCat::Activity::FOLLOW;
}

static PancettaCat::Activity companionCatRoomActivity(bool helperScene) {
    PancettaCat::Activity base = PancettaCat::Activity::FOLLOW;
    if (helperScene) return base;
    switch (currentRoom) {
        case 0: base = PancettaCat::Activity::WATCH_CABLES; break;
        case 1: base = PancettaCat::Activity::WATCH_RAIN; break;
        case 2: base = PancettaCat::Activity::SNIFF_FOOD; break;
        case 3: base = PancettaCat::Activity::STALK_SKY; break;
        case 4: base = PancettaCat::Activity::PROWL_BAR; break;
        case 5: base = PancettaCat::Activity::WATCH_WATER; break;
        default: break;
    }
    return base;
}

static PancettaCat::CompanyContext companionCompanyContext() {
    if (currentStation == Station::ON_SOFA)
        return PancettaCat::CompanyContext::RESTING;
    // The sleep pod is a closed capsule: its roof is drawn 12px above where a
    // cat sitting on Pancetta's crown would be, so a head nap in there put the
    // cat's ears and skull through solid furniture. Everything else a resting
    // cat offers still applies beside it.
    if (currentStation == Station::IN_BED)
        return PancettaCat::CompanyContext::SHELTERED;
    if (currentStation == Station::IN_BATH)
        return PancettaCat::CompanyContext::BATHING;
    return PancettaCat::CompanyContext::ORDINARY;
}

static PancettaCat::Pose resolveCompanionCatTarget(
    int pigWorldX, int pigWorldY, bool moving, bool helperScene,
    PancettaCat::Activity activity, uint32_t now) {
    PancettaCat::Pose cat;
    const PancettaCat::SceneIntent intent = PancettaCat::sceneIntent();
    const bool useRoomAnchor =
        !helperScene && intent == PancettaCat::SceneIntent::ROOM_INTEREST;
    int catX = faceRight
        ? pigWorldX - PancettaCat::kWidth - kPigPX * 2
        : pigWorldX + kPigW + kPigPX * 2;

    int catY;
    if (helperScene) {
        catY = moving
            ? PancettaCat::originYForSupport(pigWorldY + kPigH)
            : PancettaCat::originYForSupport(kBenchY);
    } else if (intent == PancettaCat::SceneIntent::EXIT) {
        catX = PancettaCat::exitsRight()
            ? SCREEN_WIDTH
            : -PancettaCat::kWidth;
        catY = PancettaCat::originYForSupport(kFloorY);
        cat.x = (int16_t)(catX & ~(kPigPX - 1));
        cat.y = (int16_t)(catY & ~(kPigPX - 1));
        cat.faceRight = PancettaCat::exitsRight();
        return cat;
    } else if (intent == PancettaCat::SceneIntent::COMPANY) {
        if (activity == PancettaCat::Activity::HEAD_NAP) {
            // Pig's crown is the support: the cat renders on the near actor
            // plane and does not cast a floor/furniture shadow from up here.
            catX = pigWorldX + (kPigW - PancettaCat::kWidth) / 2;
            catY = PancettaCat::originYForSupport(pigWorldY + kPigPX * 4);
        } else {
            const int companyGap =
                activity == PancettaCat::Activity::FACE_BUMP
                    ? -kPigPX * 2
                    : kPigPX * 2;
            const int leftX = currentRoom == 5
                ? kR6_TubX + kRoomPX
                : max(kPigPX,
                      min(SCREEN_WIDTH - kPigPX - PancettaCat::kWidth,
                          pigWorldX - PancettaCat::kWidth - companyGap));
            const int rightX = currentRoom == 5
                ? kR6_TubX + kR6_TubW - PancettaCat::kWidth +
                      kRoomPX * 2
                : max(kPigPX,
                      min(SCREEN_WIDTH - kPigPX - PancettaCat::kWidth,
                          pigWorldX + kPigW + companyGap));
            const int currentX = PancettaCat::currentPose().x;
            catX = abs(currentX - leftX) <= abs(currentX - rightX)
                ? leftX
                : rightX;
            // The floor is the only surface every station actually offers.
            // Pancetta's sprite bottom is not one: a pig at a desk, counter,
            // stool, booth, ledge or sleep pod is drawn with that edge tucked
            // into the furniture so it can hide his legs, and standing the cat
            // on it left him hovering with nothing under him at seven of the
            // ten stations - 60px of open air at the ramen counter. Room 5 is
            // the one place the room hands out a real elevated surface, and it
            // names it: the dry tub rim.
            catY = currentRoom == 5
                ? PancettaCat::originYForSupport(kR6_TubY)
                : PancettaCat::originYForSupport(kFloorY);
        }
    } else {
        catY = PancettaCat::originYForSupport(kFloorY);

        // At idle he has his own small cat business in every room instead of
        // freezing as a second mascot beside Pancetta.
        if (useRoomAnchor) {
            switch (activity) {
                case PancettaCat::Activity::WATCH_CABLES:
                    catX = kR1_SrvX - PancettaCat::kWidth - kRoomPX;
                    break;
                case PancettaCat::Activity::WATCH_RAIN:
                    catX = kR2_WindowX + kRoomPX * 4;
                    break;
                case PancettaCat::Activity::SNIFF_FOOD:
                    catX = kR3_CounterX + kR3_CounterW + kRoomPX;
                    break;
                case PancettaCat::Activity::STALK_SKY:
                    catX = kR4_SkyX + kRoomPX * 3;
                    break;
                case PancettaCat::Activity::PROWL_BAR:
                    catX = kR5_BarX - PancettaCat::kWidth +
                           kRoomPX * 2;
                    break;
                case PancettaCat::Activity::WATCH_WATER:
                    // The dry right rim is a real support above the
                    // water/privacy pass.
                    catX = kR6_TubX + kR6_TubW -
                           PancettaCat::kWidth + kRoomPX * 2;
                    catY = PancettaCat::originYForSupport(kR6_TubY);
                    break;
                case PancettaCat::Activity::SCRATCH:
                case PancettaCat::Activity::SLEEP:
                case PancettaCat::Activity::GROOM:
                case PancettaCat::Activity::HAIRBALL:
                case PancettaCat::Activity::ARCH:
                case PancettaCat::Activity::ZOOMIES:
                case PancettaCat::Activity::KNEAD:
                case PancettaCat::Activity::SLOW_BLINK:
                case PancettaCat::Activity::MEOW:
                    // An action owns a destination. Navigation reaches it in a
                    // FOLLOW clip before the tagged one-shot action begins.
                    if (activity == PancettaCat::Activity::ZOOMIES) {
                        catX = PancettaCat::currentPose().x;
                        catY = PancettaCat::originYForSupport(kFloorY);
                    } else if (currentRoom == 0) {
                        catX = kR1_SrvX - PancettaCat::kWidth - kRoomPX;
                    } else if (currentRoom == 1) {
                        if (currentStation == Station::ON_SOFA) {
                            catX = kR2_SofaX + kR2_SofaW + kRoomPX;
                        } else {
                            catX = kR2_SofaX +
                                   (kR2_SofaW - PancettaCat::kWidth) / 2;
                            catY = PancettaCat::originYForSupport(kR2_SofaY);
                        }
                    } else if (currentRoom == 2) {
                        catX = kR3_CounterX + kR3_CounterW + kRoomPX;
                    } else if (currentRoom == 3) {
                        catX = kR4_SkyX + kRoomPX * 3;
                    } else if (currentRoom == 4) {
                        catX = kR5_BarX - PancettaCat::kWidth;
                    } else if (currentRoom == 5) {
                        // Pancetta owns the tub. Maintenance actions use the
                        // dry deck instead of sharing his water silhouette.
                        catX = kPigPX;
                        catY = PancettaCat::originYForSupport(kFloorY);
                    }
                    break;
                default:
                    break;
            }
        }
    }

    if (useRoomAnchor) {
        const int floorY = PancettaCat::originYForSupport(kFloorY);
        const int gap = kPigPX * 2;
        const bool overlapsPig =
            catY == floorY && catX < pigWorldX + kPigW + gap &&
            catX + PancettaCat::kWidth + gap > pigWorldX;
        if (overlapsPig) {
            const int left = pigWorldX - PancettaCat::kWidth - gap;
            const int right = pigWorldX + kPigW + gap;
            const bool leftFits = left >= kPigPX;
            const bool rightFits =
                right + PancettaCat::kWidth <= SCREEN_WIDTH - kPigPX;
            if (leftFits && rightFits) {
                catX = abs(catX - left) <= abs(catX - right)
                    ? left
                    : right;
            } else if (leftFits) {
                catX = left;
            } else if (rightFits) {
                catX = right;
            }
        }
    }
    // The playfield clamp belongs to every spot the cat is actually meant to
    // stand in, not only to his room anchors. The helper scene never reached
    // it: Pancetta is right-aligned on his crate at x=200, so trailing him by
    // one pig width put the cat's footprint at 276..328 on a 320px screen and
    // sheared his back end and tail off against the bezel on the most-looked-at
    // screen in the build. Walking off stage is the one deliberate exception
    // and the EXIT intent has already returned above.
    catX = max(kPigPX,
               min(SCREEN_WIDTH - kPigPX - PancettaCat::kWidth, catX));
    cat.x = (int16_t)(catX & ~(kPigPX - 1));
    cat.y = (int16_t)(catY & ~(kPigPX - 1));
    if (PancettaCat::isTransmitting()) {
        // Binary meows are addressed to Pancetta, so local room curiosity
        // yields for the duration of the four-byte burst.
        cat.faceRight =
            cat.x + PancettaCat::kWidth / 2 < pigWorldX + kPigW / 2;
        return cat;
    }
    if (useRoomAnchor) {
        switch (activity) {
            case PancettaCat::Activity::WATCH_CABLES:
            case PancettaCat::Activity::WATCH_RAIN:
            case PancettaCat::Activity::STALK_SKY:
                cat.faceRight = true;
                return cat;
            case PancettaCat::Activity::SNIFF_FOOD:
            case PancettaCat::Activity::WATCH_WATER:
                cat.faceRight = false;
                return cat;
            case PancettaCat::Activity::PROWL_BAR:
                cat.faceRight = true;
                return cat;
            case PancettaCat::Activity::SCRATCH:
                cat.faceRight = true;
                return cat;
            case PancettaCat::Activity::SLEEP:
            case PancettaCat::Activity::GROOM:
            case PancettaCat::Activity::HAIRBALL:
            case PancettaCat::Activity::ARCH:
            case PancettaCat::Activity::KNEAD:
            case PancettaCat::Activity::SLOW_BLINK:
                cat.faceRight = currentRoom != 5;
                return cat;
            case PancettaCat::Activity::FACE_BUMP:
            case PancettaCat::Activity::HEAD_NAP:
                break;
            case PancettaCat::Activity::ZOOMIES:
                // Cat-owned navigation chooses the room-crossing direction.
                return cat;
            default:
                break;
        }
    }
    cat.faceRight =
        cat.x + PancettaCat::kWidth / 2 < pigWorldX + kPigW / 2;
    (void)now;
    return cat;
}

static bool isCatSignalChannelClear(uint32_t now);
static bool narratorBubbleOwnsChannel();
bool isRoamingMoveState();

// Only mass the room has already painted counts as cover: he has to come out
// from behind something the player accepts as solid. The emerge side is the
// side his room business is on, so crossing the edge is the first thing he
// does rather than something he might never get around to. Parallax layers
// move their furniture, so the spot is rebuilt every frame from where the
// object actually is this frame, not from where it was when he hid.
// Every spot here is checked by scripts/sim_cat_portal.py: wide enough to
// cover him, a reveal edge he actually crosses, and an anchor he then keeps
// walking toward instead of turning back into the furniture.
static PancettaCat::RoomStaging companionRoomStaging(bool roomScene) {
    PancettaCat::RoomStaging staging;
    if (roomScene) {
        switch (currentRoom) {
            case 0:  // under the desk, out toward the server cables
                staging.hideX = (int16_t)kR1_DeskX;
                staging.hideWidth = (int16_t)kR1_DeskW;
                staging.hideEmergeRight = true;
                staging.hideValid = true;
                break;
            case 1:  // behind the couch, out toward the rain window
                staging.hideX = (int16_t)kR2_SofaX;
                staging.hideWidth = (int16_t)kR2_SofaW;
                staging.hideEmergeRight = true;
                staging.hideValid = true;
                break;
            case 2:  // behind the sleep pod, out toward the counter
                staging.hideX = (int16_t)kR3_PodX;
                staging.hideWidth = (int16_t)kR3_PodW;
                staging.hideEmergeRight = false;
                staging.hideValid = true;
                break;
            case 3:  // behind the shack, out toward the skyline
                staging.hideX = (int16_t)(kR4_ShackX + parallaxFar);
                staging.hideWidth = (int16_t)kR4_ShackW;
                staging.hideEmergeRight = true;
                staging.hideValid = true;
                break;
            case 4:  // behind the bar counter, out toward the stools
                staging.hideX = (int16_t)(kR5_BarX + parallaxMid);
                staging.hideWidth = (int16_t)kR5_BarW;
                staging.hideEmergeRight = false;
                staging.hideValid = true;
                break;
            case 5:  // behind the tub, out toward the dry rim
                staging.hideX = (int16_t)(kR6_TubX + parallaxMid);
                staging.hideWidth = (int16_t)kR6_TubW;
                staging.hideEmergeRight = true;
                staging.hideValid = true;
                break;
            default:
                break;
        }
        // Cover narrower than the cat is not cover.
        if (staging.hideWidth < (int16_t)PancettaCat::kWidth)
            staging.hideValid = false;
    }

    // How welcome he feels, not whether he exists. Mood carries the session,
    // the daily goal and the streak carry the week, and the memories he has
    // already given Pancetta carry the whole save — so a player who has been
    // away gets a wary cat, and a regular gets one waiting in the open.
    const int mood = max(-100, min(100, Mood::getEffectiveMood()));
    const int moodPart = (mood + 100) / 2;
    const int goalPart = min(100, (int)Config::getGoalProgress());
    const int streakPart = min(7, (int)Config::getStreak()) * 100 / 7;
    const uint32_t catProgressMask = Config::getCatMemoryMask();
    const int memoryPart =
        (int)catLegacyMemoryCount(catProgressMask) * 100 /
        (int)PancettaCat::Memory::COUNT;
    const int behaviorPart =
        (int)CatBehavior::popcount8(catDossierMask(catProgressMask)) * 4 +
        (int)CatBehavior::popcount8(catMasteryMask(catProgressMask)) * 3;
    const int bondPart = min(100, memoryPart + behaviorPart);
    staging.warmth = (uint8_t)max(0, min(100,
        (moodPart * 50 + goalPart * 25 + streakPart * 15 + bondPart * 10) /
        100));
    return staging;
}

static void reconcileCatMemoryAchievements(uint32_t memoryMask) {
    if (memoryMask & PancettaCat::memoryBit(
            PancettaCat::Memory::HEAD_NAP))
        Achievements::tryUnlock(Achievement::LIVING_PILLOW);
    if ((memoryMask & PancettaCat::kAllMemoryBits) ==
        PancettaCat::kAllMemoryBits)
        Achievements::tryUnlock(Achievement::PIG_REMEMBERS);
}

static void grantCatBehaviorReward(uint8_t xp, int8_t momentum) {
    if (xp > 0)
        Config::addXP(xp, Config::RewardSource::CAT_MEMORY);
    if (momentum > 0)
        Mood::addMomentum(momentum);
}

static void logCatBehaviorReward(const char* label, uint8_t xp,
                                 bool headline) {
    if (!DefhogTerminal::isVisible() || !label || !label[0]) return;
    char gain[24];
    ProgressionText::formatGainAmount(gain, sizeof(gain), xp);
    if (headline)
        DefhogTerminal::pushLineHype("CAT: %s. %s", label, gain);
    else
        DefhogTerminal::pushLineDim("cat: %s. %s", label, gain);
}

static void applyCatBehaviorCompletion(
        const CatBehavior::Completion& completion,
        uint32_t& persistedMask) {
    switch (completion.reward) {
        case CatBehavior::Reward::ROOM_DOSSIER: {
            if (completion.room >= NUM_ROOMS) return;
            const uint32_t bit =
                1u << (CAT_DOSSIER_SHIFT + completion.room);
            if ((persistedMask & bit) != 0) return;
            persistedMask |= bit;
            bool completedSet = false;
            if ((persistedMask & CAT_DOSSIER_BITS) == CAT_DOSSIER_BITS &&
                (persistedMask & CAT_DOSSIER_COMPLETE_BIT) == 0) {
                persistedMask |= CAT_DOSSIER_COMPLETE_BIT;
                completedSet = true;
            }
            // Match the original Cat-memory transaction order: seal the
            // one-time flag before paying it, so a reset cannot replay XP.
            Config::setCatMemoryMask(persistedMask);
            grantCatBehaviorReward(completion.xp, completion.momentum);
            logCatBehaviorReward(completion.label, completion.xp, true);
            if (completedSet) {
                grantCatBehaviorReward(12, 3);
                logCatBehaviorReward("all room dossiers", 12, true);
            }
            break;
        }
        case CatBehavior::Reward::BEHAVIOR_MASTERY: {
            const uint8_t category = (uint8_t)completion.category;
            if (category >= (uint8_t)CatBehavior::Category::COUNT) return;
            const uint32_t bit = 1u << (CAT_MASTERY_SHIFT + category);
            if ((persistedMask & bit) != 0) return;
            persistedMask |= bit;
            bool completedSet = false;
            if ((persistedMask & CAT_MASTERY_BITS) == CAT_MASTERY_BITS &&
                (persistedMask & CAT_MASTERY_COMPLETE_BIT) == 0) {
                persistedMask |= CAT_MASTERY_COMPLETE_BIT;
                completedSet = true;
            }
            Config::setCatMemoryMask(persistedMask);
            grantCatBehaviorReward(completion.xp, completion.momentum);
            logCatBehaviorReward(completion.label, completion.xp, true);
            if (completedSet) {
                grantCatBehaviorReward(10, 3);
                logCatBehaviorReward("all behavior masteries", 10, true);
            }
            break;
        }
        case CatBehavior::Reward::DAILY_GOAL:
        case CatBehavior::Reward::STREAK:
        case CatBehavior::Reward::BOND:
            grantCatBehaviorReward(completion.xp, completion.momentum);
            logCatBehaviorReward(completion.label, completion.xp, false);
            break;
        case CatBehavior::Reward::NONE:
            break;
    }
}

static void updateCompanionCatState(uint32_t now) {
    const bool roomScene = mode == PigMode::ROAMING;
    const bool helperScene = !roomScene;
    const bool pigMoving = helperScene
        ? mode == PigMode::LEAVING_BENCH ||
              mode == PigMode::RETURNING_TO_BENCH
        : isRoamingMoveState();
    const bool stationary =
        roomScene && roamState == RoamState::IDLE;
    const bool channelClear = isCatSignalChannelClear(now);
    uint32_t persistedCatMask = Config::getCatMemoryMask();

    PancettaCat::setRoomStaging(companionRoomStaging(roomScene));
    PancettaCat::update(now, channelClear, roomScene,
                        (uint8_t)currentRoom, stationary,
                        companionCompanyContext());

    CatBehavior::Context behaviorContext;
    behaviorContext.nowMs = now;
    behaviorContext.entropy = now ^
        ((uint32_t)(currentRoom & 0xFF) * 0x45D9F3Bu) ^
        ((uint32_t)(uint8_t)currentStation << 16);
    behaviorContext.mood = (int16_t)max(
        -100, min(100, Mood::getEffectiveMood()));
    behaviorContext.room = (uint8_t)currentRoom;
    behaviorContext.station = (uint8_t)currentStation;
    behaviorContext.goalProgress = (uint8_t)min(
        100, (int)Config::getGoalProgress());
    behaviorContext.streak = (uint8_t)min(
        255, (int)Config::getStreak());
    behaviorContext.memories = catLegacyMemoryCount(persistedCatMask);
    behaviorContext.dossierMask = catDossierMask(persistedCatMask);
    behaviorContext.masteryMask = catMasteryMask(persistedCatMask);
    behaviorContext.intent = catBehaviorIntent(PancettaCat::sceneIntent());
    behaviorContext.helperScene = helperScene;
    behaviorContext.pigMoving = pigMoving;
    behaviorContext.stationary = stationary;
    behaviorContext.visible = PancettaCat::isVisible();
    behaviorContext.navigating = PancettaCat::isNavigating();
    behaviorContext.transmitting = PancettaCat::isTransmitting();
    behaviorContext.channelClear = channelClear;
    behaviorContext.eventHold = eventHold;
    behaviorContext.sessionActive =
        roomScene && Config::isSessionActive();
    catBehaviorDecision = CatBehavior::update(
        behaviorContext,
        catBehaviorAction(PancettaCat::animationActivity()));

    PancettaCat::Activity roomActivity =
        companionCatRoomActivity(helperScene);
    if (!helperScene && catBehaviorDecision.overrideBase)
        roomActivity = catActivityForBehavior(catBehaviorDecision.action);
    const PancettaCat::Activity desiredActivity =
        PancettaCat::desiredActivity(roomActivity, pigMoving, helperScene);
    const PancettaCat::Activity targetActivity =
        !helperScene &&
        PancettaCat::sceneIntent() ==
            PancettaCat::SceneIntent::ROOM_INTEREST &&
        desiredActivity == PancettaCat::Activity::FOLLOW
            ? roomActivity
            : desiredActivity;
    PancettaCat::Pose target =
        resolveCompanionCatTarget((int)pigX, (int)pigY, pigMoving,
                                  helperScene, targetActivity, now);
    // While he is still coming out from behind something, the room's own
    // anchor is not his destination yet — clearing the furniture is. Same seam
    // as the exit intent: the companion owns the intent, the room owns where
    // its furniture ends.
    PancettaCat::Pose emergeTarget;
    if (!helperScene && PancettaCat::concealExitPose(emergeTarget))
        target = emergeTarget;

    // A trinket is valid only at a settled room-interest station. Explicitly
    // clear it everywhere else so a prior room cannot leave a stale interactive
    // object behind during travel, teleport, or company behavior.
    const bool trinketStation =
        !helperScene && stationary && !pigMoving && PancettaCat::isVisible() &&
        PancettaCat::sceneIntent() ==
            PancettaCat::SceneIntent::ROOM_INTEREST &&
        !PancettaCat::isNavigating();
    if (trinketStation) {
        const int trinketGap = kPigPX * 3;
        static_assert(trinketGap <= PancettaCat::kTrinketReachPx,
                      "the room must place the object inside paw reach");
        const int rightX =
            target.x + PancettaCat::kWidth + trinketGap;
        const int leftX =
            target.x - trinketGap - PancettaCat::kTrinketWidth;
        const bool rightFits =
            rightX + PancettaCat::kTrinketWidth <= SCREEN_WIDTH - kPigPX;
        const bool leftFits = leftX >= kPigPX;

        // Keep the object in front of the paws. If the authored facing points
        // it through the bezel, flip to the other valid side before the anchor
        // is latched. The final clamp is a safety net for narrow edge cases.
        if (target.faceRight && !rightFits && leftFits)
            target.faceRight = false;
        else if (!target.faceRight && !leftFits && rightFits)
            target.faceRight = true;

        int trinketX = target.faceRight ? rightX : leftX;
        trinketX = max(kPigPX,
            min(SCREEN_WIDTH - kPigPX - PancettaCat::kTrinketWidth,
                trinketX));
        PancettaCat::setTrinketAnchor(
            (int16_t)trinketX,
            (int16_t)(target.y + PancettaCat::kGroundContactY), true);
    } else {
        PancettaCat::setTrinketAnchor(0, 0, false);
    }

    PancettaCat::updateNavigation(now, target, roomActivity,
                                  pigMoving, helperScene);

    // Consume every sound latch exactly once, but only emit it when its actor
    // is visibly present in a room. This prevents a stale paw/landing event from
    // sounding during concealment, portal transit, or the helper scene.
    const bool catAudible = !helperScene && PancettaCat::isVisible();
    if (PancettaCat::consumeMeowVoice() && catAudible) {
        SFX::play(SFX::CAT_MEOW);
        if (currentRoom == 4) Barman::onCatSpoke(now);
    }
    if (PancettaCat::consumeTrinketBat() && catAudible)
        SFX::play(SFX::CAT_PAW_TAP);
    if (PancettaCat::consumeTrinketKnock() && catAudible)
        SFX::play(SFX::CAT_KNOCK);
    if (PancettaCat::consumeTrinketLanded() && catAudible)
        SFX::play(SFX::CAT_CLATTER);

    // UI-state and trophy words are separate NVS writes. Reconcile every
    // update so a power loss between them cannot strand an earned trophy.
    reconcileCatMemoryAchievements(persistedCatMask);

    PancettaCat::Memory memory;
    if (PancettaCat::consumeMemory(memory)) {
        const uint32_t bit = PancettaCat::memoryBit(memory);
        if (bit != 0 && (persistedCatMask & bit) == 0) {
            persistedCatMask |= bit;
            Config::setCatMemoryMask(persistedCatMask);
            Config::addXP(8, Config::RewardSource::CAT_MEMORY);
            Mood::addMomentum(2);
            reconcileCatMemoryAchievements(persistedCatMask);
        }
    }

    // A routine earns progress only after its phases were observed in the
    // low-level animation state. Persist unique dossiers/masteries and award
    // live goal, streak, and bond reactions without rewarding boot state.
    CatBehavior::Completion completion;
    while (CatBehavior::consumeCompletion(completion))
        applyCatBehaviorCompletion(completion, persistedCatMask);

    // The beam hands the body back only now, one step after the destination
    // room has been told he is arriving in it. Landing him any earlier lets
    // that room's own visit roll decide he never showed up.
    if (catTeleportLandingPending) {
        catTeleportLandingPending = false;
        PancettaCat::endPortalTransit(now, catTeleportLanding);
    }
}

static bool isCatSignalChannelClear(uint32_t now) {
    const bool sleepStation =
        currentStation == Station::ON_SOFA ||
        currentStation == Station::IN_BED;
    return mode == PigMode::ROAMING &&
           roamState == RoamState::IDLE &&
           !eventHold &&
           !shouldSuppressRoamBubble() &&
           currentStation != Station::IN_BATH &&
           !sleepStation &&
           !Config::getIppEnabled() &&
           !DefhogTerminal::isVisible() &&
           !Barman::isSpeaking(now) &&
           !narratorBubbleOwnsChannel();
}

bool isBathJumpActive() {
    if (mode != PigMode::ROAMING) return false;
    if (roamState == RoamState::MOUNTING)
        return walkTargetStation == Station::IN_BATH;
    if (roamState == RoamState::DISMOUNTING)
        return currentStation == Station::IN_BATH;
    return false;
}

float getBathJumpProgress(uint32_t now) {
    if (!isBathJumpActive()) return -1.0f;
    uint32_t duration = roamState == RoamState::MOUNTING
        ? BATH_JUMP_IN_MS : BATH_JUMP_OUT_MS;
    uint32_t elapsed = now - jumpStart;
    if (elapsed >= duration) return 1.0f;
    return (float)elapsed / (float)duration;
}

static uint16_t bathSmoothstepQ8(uint32_t elapsed, uint32_t duration) {
    if (elapsed >= duration) return 256u;
    uint32_t t = (elapsed << 8) / duration;
    return (uint16_t)((t * t * (768u - 2u * t) + 32768u) >> 16);
}

static bool isBathIdleEligible() {
    return mode == PigMode::ROAMING &&
           roamState == RoamState::IDLE &&
           currentRoom == 5 &&
           currentStation == Station::IN_BATH;
}

static uint32_t randomBathDuration(uint32_t minMs, uint32_t maxMs) {
    return minMs + (esp_random() % (maxMs - minMs + 1u));
}

static void planBathCycle(uint32_t now) {
    // One roll chooses a visible temperament for the whole action. This is
    // intentionally sampled once, on the update path: changing choices in a
    // draw pass is visual noise, not agency.
    const uint32_t temper = esp_random();
    const uint8_t style = static_cast<uint8_t>((temper >> 8) % 3u);
    const uint8_t diveChancePct = style == 0u ? 48u :
                                  style == 1u ? 72u : 84u;

    bathSoakMs = randomBathDuration(
        style == 0u ? 8800u : BATH_SOAK_MIN_MS,
        style == 0u ? BATH_SOAK_MAX_MS : 10400u);
    bathDivePlanned = static_cast<uint8_t>(temper) <
        static_cast<uint8_t>((uint16_t)diveChancePct * 255u / 100u);
    bathPrepMs = randomBathDuration(BATH_PREP_MIN_MS, BATH_PREP_MAX_MS);
    bathSinkMs = randomBathDuration(BATH_SINK_MIN_MS, BATH_SINK_MAX_MS);
    bathDiveSubmergedMs = randomBathDuration(BATH_SUBMERGED_MIN_MS,
                                             BATH_SUBMERGED_MAX_MS);
    bathRiseMs = randomBathDuration(BATH_RISE_MIN_MS, BATH_RISE_MAX_MS);
    bathRecoverMs = randomBathDuration(BATH_RECOVER_MIN_MS, BATH_RECOVER_MAX_MS);
    bathDiveDepthPx = static_cast<int8_t>(
        kPigPX * (6 + ((temper >> 16) % 4u)));  // 12, 14, 16, or 18 px
    bathCycleStartMs = now;
    bathCyclePlanned = true;
}

static uint32_t bathCycleDurationMs() {
    if (!bathDivePlanned) return bathSoakMs;
    return bathSoakMs + bathPrepMs + bathSinkMs + bathDiveSubmergedMs +
           bathRiseMs + bathRecoverMs;
}

static void updateBathIdlePlan(uint32_t now) {
    if (!isBathIdleEligible()) {
        bathCycleStartMs = 0;
        bathCyclePlanned = false;
        return;
    }
    if (!bathCyclePlanned || (uint32_t)(now - bathCycleStartMs) >= bathCycleDurationMs())
        planBathCycle(now);
}

BathIdleFrame sampleBathIdleFrame(uint32_t now) {
    BathIdleFrame frame;
    if (!isBathIdleEligible() || !bathCyclePlanned) {
        return frame;
    }

    const uint32_t soakEnd  = bathSoakMs;
    if (!bathDivePlanned || (uint32_t)(now - bathCycleStartMs) < soakEnd) {
        frame.phase = BathIdlePhase::SOAK;
        frame.phaseElapsedMs = static_cast<uint16_t>(now - bathCycleStartMs);
        frame.jointVisible = true;
        frame.smokeVisible = true;
        return frame;
    }

    const uint32_t prepEnd  = soakEnd + bathPrepMs;
    const uint32_t sinkEnd  = prepEnd + bathSinkMs;
    const uint32_t subEnd   = sinkEnd + bathDiveSubmergedMs;
    const uint32_t riseEnd  = subEnd + bathRiseMs;

    const uint32_t local = now - bathCycleStartMs;

    if (local < prepEnd) {
        frame.phase = BathIdlePhase::PREP;
        frame.phaseElapsedMs = (uint16_t)(local - soakEnd);
        frame.jointVisible = frame.phaseElapsedMs < 420u;
        frame.smokeVisible = frame.phaseElapsedMs < 220u;
        return frame;
    }

    frame.jointVisible = false;
    if (local < sinkEnd) {
        frame.phase = BathIdlePhase::SINK;
        frame.phaseElapsedMs = (uint16_t)(local - prepEnd);
        frame.smokeVisible = false;
        uint16_t eased = bathSmoothstepQ8(
            frame.phaseElapsedMs, bathSinkMs);
        frame.submergeY = static_cast<int8_t>(
            ((uint32_t)bathDiveDepthPx * eased) >> 8);
        frame.submergeY &= ~(kPigPX - 1);
        return frame;
    }

    if (local < subEnd) {
        frame.phase = BathIdlePhase::SUBMERGED;
        frame.phaseElapsedMs = (uint16_t)(local - sinkEnd);
        frame.submergeY = bathDiveDepthPx;
        frame.smokeVisible = false;
        return frame;
    }

    if (local < riseEnd) {
        frame.phase = BathIdlePhase::RISE;
        frame.phaseElapsedMs = (uint16_t)(local - subEnd);
        frame.smokeVisible = false;
        uint16_t eased = bathSmoothstepQ8(
            frame.phaseElapsedMs, bathRiseMs);
        frame.submergeY = (int8_t)(
            bathDiveDepthPx -
            (((uint32_t)bathDiveDepthPx * eased) >> 8));
        frame.submergeY &= ~(kPigPX - 1);
        return frame;
    }

    frame.phase = BathIdlePhase::RECOVER;
    frame.phaseElapsedMs = (uint16_t)(local - riseEnd);
    if (frame.phaseElapsedMs < 720u) {
        static const int8_t kShakeX[] = {0, -2, 2, -2, 2, 0};
        frame.shakeX = kShakeX[frame.phaseElapsedMs / 120u];
    }
    frame.jointVisible = frame.phaseElapsedMs >= 1100u;
    frame.smokeVisible = frame.phaseElapsedMs >= 1500u;
    return frame;
}

static bool isBathMicDanceEligibleAt(uint32_t now) {
    (void)now;
    if (mode != PigMode::ROAMING || roamState != RoamState::IDLE ||
        currentRoom != 5 || currentStation != Station::IN_BATH) {
        return false;
    }
    // Keep sampling through the authored dive. Pancetta may vanish beneath the
    // water, but the microphone stays in the room and the next surface hop is
    // still locked to the same song.
    return true;
}

bool isBathMicDanceEligible() {
    return isBathMicDanceEligibleAt(millis());
}

static bool isBathSoundDanceActiveAt(uint32_t now) {
    return isBathMicDanceEligibleAt(now) && BathMic::isDanceActive();
}

bool isBathSoundDanceActive() {
    return isBathSoundDanceActiveAt(millis());
}

bool isBathSoundDanceSurfaceActive(uint32_t now) {
    return isBathSoundDanceActiveAt(now) &&
        sampleBathIdleFrame(now).phase == BathIdlePhase::SOAK;
}

uint8_t getBathSoundDanceBeatPhase(uint32_t now) {
    return isBathSoundDanceSurfaceActive(now)
        ? BathMic::danceBeatPhase(now) : 0u;
}

bool isRoamingMoveState() {
    if (mode == PigMode::ROAMING &&
            (roamState == RoamState::WALKING_TO ||
             roamState == RoamState::ENTERING_ROOM ||
             roamState == RoamState::DISMOUNTING ||
             roamState == RoamState::MOUNTING))
        return true;
    // WD cinematic: pig walking toward car
    if (wdMountPhase == WDMountPhase::WALKING)
        return true;
    return false;
}

static bool isPigMoving() {
    return (mode == PigMode::LEAVING_BENCH ||
            mode == PigMode::RETURNING_TO_BENCH ||
            isRoamingMoveState());
}

bool isRearIdleStation() {
    return ((currentStation == Station::AT_LAPTOP ||
             currentStation == Station::AT_WINDOW ||
             currentStation == Station::AT_ANTENNA ||
             currentStation == Station::AT_TERMINAL) &&
            roamState == RoamState::IDLE);
}

static bool isCookingLeadIn() {
    return (currentStation == Station::COOKING &&
            roamState == RoamState::SETTLING);
}

bool shouldUseRearViewInRoaming() {
    if (rearCineOverrideRear) return true;  // cinematic forces rear view
    // WD boarding beats always stay front-facing.
    if (isWDMountFrontViewLocked())
        return false;
    return (mode == PigMode::ROAMING &&
            (isRearIdleStation() || isCookingLeadIn()) &&
            !wallBreakActive);
}

bool isRamenEatingState(bool useRearView) {
    return (mode == PigMode::ROAMING &&
            roamState == RoamState::IDLE &&
            currentStation == Station::COOKING &&
            !useRearView &&
            !carryingCup &&
            !wallBreakActive);
}

bool isCupDrinkingState(bool useRearView) {
    return (carryingCup &&
            mode == PigMode::ROAMING &&
            !useRearView &&
            currentStation != Station::COOKING &&
            currentStation != Station::IN_BED &&
            currentStation != Station::IN_BATH);
}

// Noodle slurp period — shared between head dip and strand retraction.
static constexpr uint32_t kSlurpPeriodMs = 2200;

static int calcIngestHeadDip(uint32_t now, bool ramenEating, bool cupDrinking) {
    if (ramenEating) {
        float slurpPhase = (float)(now % kSlurpPeriodMs) / (float)kSlurpPeriodMs;
        if (slurpPhase > 0.55f && slurpPhase < 0.80f) return kPigPX;
    } else if (cupDrinking) {
        float sipPhase = (float)((now + 280) % 2300) / 2300.0f;
        if (sipPhase > 0.52f && sipPhase < 0.78f) return kPigPX;
    }
    return 0;
}

static bool isSeatedStation(Station station) {
    switch (station) {
        case Station::AT_LAPTOP:
        case Station::ON_SOFA:
        case Station::COOKING:
        case Station::IN_BED:
        case Station::ON_LEDGE:
        case Station::AT_TERMINAL:
        case Station::AT_BOOTH:
        case Station::IN_BATH:
            return true;
        default:
            return false;
    }
}

static bool calcPhaseLockedPortalJumpDy(int& jumpDy) {
    // Portal overlay system removed — jump driven by triggerPortalJump() now.
    (void)jumpDy;
    return false;
}

PigPose resolvePigPose(uint32_t now, bool isMoving, bool useRearView,
                              bool allowPortalJumpFinish) {
    PigPose pose = {(int)pigX, (int)pigY, false, false, 0};
    pose.ramenEating = isRamenEatingState(useRearView);
    pose.cupDrinking = isCupDrinkingState(useRearView);
    bool wdRoofBeat =
        (wdMountPhase == WDMountPhase::JUMPING ||
         wdMountPhase == WDMountPhase::IMPACT ||
         wdMountPhase == WDMountPhase::ROOF_RIDE ||
         wdMountPhase == WDMountPhase::SNEAK_IN ||
         wdReturnPhase != WDReturnPhase::NONE);
    bool isSeatedIdle =
        (mode == PigMode::ROAMING &&
         roamState == RoamState::IDLE &&
         isSeatedStation(currentStation));

    if (isMoving) {
        // Walk bounce in room roaming mode
        if (mode == PigMode::ROAMING)
            pose.drawY += calcWalkBounce();
    } else if (wdRoofBeat) {
        // Wardrive roof beats use explicit cinematic positioning; suppress idle bob.
    } else if (mode == PigMode::ROAMING && roamState == RoamState::SETTLING) {
        // Hold still during turn pause.
    } else if (isSeatedIdle) {
        // Furniture and pig share one arrival wobble. Workstations without a
        // moving support stay hard-locked instead of breathing off the seat.
        bool movingSupport =
            currentStation == Station::AT_LAPTOP ||
            currentStation == Station::ON_SOFA ||
            currentStation == Station::COOKING ||
            currentStation == Station::IN_BED;
        if (movingSupport) {
            calcWobble(now);
            pose.drawX += wobbleX;
            if (currentStation != Station::AT_LAPTOP)
                pose.drawY += wobbleY;
        }
    } else {
        pose.drawY += calcBreathe(now);
    }

    // THE SOAK owns a deterministic dip instead of furniture wobble. Both the
    // pig renderer and the foreground water query this pose, so the nostrils,
    // waterline, bubbles, and privacy layer cannot drift apart.
    BathIdleFrame bathIdle = sampleBathIdleFrame(now);
    pose.drawX += bathIdle.shakeX;
    pose.drawY += bathIdle.submergeY;

    // Full hops happen at the surface. The dive remains the authored water
    // event, with only a gentler, still beat-locked bob underneath it.
    if (isBathSoundDanceActiveAt(now)) {
        static constexpr int8_t kDanceSwayX[8] = {0, 2, 4, 2, 0, -2, -4, -2};
        static constexpr int8_t kSurfaceHopY[8] = {0, -4, -10, -14, -10, -4, 0, -2};
        static constexpr int8_t kUnderwaterBobY[8] = {0, -2, -4, -2, 0, -2, -4, -2};
        const uint8_t beat = BathMic::danceBeatPhase(now);
        const bool surfaced = bathIdle.phase == BathIdlePhase::SOAK;
        pose.drawX += kDanceSwayX[beat];
        pose.drawY += surfaced ? kSurfaceHopY[beat] : kUnderwaterBobY[beat];
    }

    pose.ingestHeadDip = calcIngestHeadDip(now, pose.ramenEating, pose.cupDrinking);
    if (!isSeatedIdle) {
        pose.drawY += pose.ingestHeadDip;
    }

    if (mode == PigMode::ROAMING && !useRearView && !wdRoofBeat) {
        pose.drawY += kFrontPigAlignY;
    }

    int phaseJumpDy = 0;
    bool phaseJumpActive = calcPhaseLockedPortalJumpDy(phaseJumpDy);
    if (phaseJumpActive) {
        pose.drawY -= phaseJumpDy;
    } else if (portalJumpActive) {
        uint32_t jumpElapsed = now - portalJumpStart;
        if (jumpElapsed >= PORTAL_JUMP_MS) {
            if (allowPortalJumpFinish) portalJumpActive = false;
        } else {
            float jt = (float)jumpElapsed / (float)PORTAL_JUMP_MS;
            int jumpDy = (int)lroundf(fastSinf(jt * PI) * (float)PORTAL_JUMP_HEIGHT);
            jumpDy &= ~1;
            pose.drawY -= jumpDy;
        }
    }

    // Mount/dismount spring squash — body offset (positive=down, negative=up)
    if (fabsf(jumpSquashPx) > 0.5f) {
        int sq = (int)lroundf(jumpSquashPx);
        sq = (sq >= 0) ? (sq & ~(kPigPX - 1)) : -((-sq) & ~(kPigPX - 1));
        pose.drawY += sq;
    }

    // Snap final position to fat-pixel grid.
    pose.drawX &= ~1;
    pose.drawY &= ~1;
    return pose;
}

static bool shouldDrawSittingLegs() {
    if (mode == PigMode::ON_BENCH) return true;
    return (mode == PigMode::ROAMING &&
            (roamState == RoamState::IDLE || roamState == RoamState::SETTLING) &&
            isSeatedStation(currentStation));
}

static bool shouldDrawWalkingLegs(bool isMoving, bool useRearView) {
    bool holdsArrivalContact =
        mode == PigMode::ROAMING && roamState == RoamState::SETTLING &&
        !isSeatedStation(currentStation);
    return ((isMoving || holdsArrivalContact) && !useRearView &&
            !isBathJumpActive());
}

static PigEyeLook resolveMenuPigEyeLook(bool useRearView, bool isMoving, Station station,
                                        bool ramenEating, bool cupDrinking, uint32_t now) {
    if (useRearView) return PigEyeLook::NONE;
    // While the body travels, Pancetta watches the next contact rather than
    // retaining an unrelated idle glance.
    if (isMoving) return PigEyeLook::FRONT_DOWN;
    if (station == Station::ON_SOFA || station == Station::IN_BED) return PigEyeLook::NONE;
    if (ramenEating || cupDrinking || station == Station::COOKING) return PigEyeLook::FRONT_DOWN;
    if (isWDMountFrontViewLocked()) return PigEyeLook::FRONT_UP;
    if (isCharacterSniffActive(now)) return PigEyeLook::FRONT_DOWN;
    return faceTimer.eyeLook;
}

static constexpr int kHeldBowlOffsetX = 27;
// Keep held ramen high enough that the contour layer reads above counter clutter.
static constexpr int kHeldBowlOffsetY = 26;

void getHeldBowlPosition(int pigDrawX, int pigDrawY, int& bowlX, int& bowlY) {
    bowlX = (pigDrawX + kHeldBowlOffsetX + kRamenDishShiftX) & ~1;
    bowlY = (pigDrawY + kHeldBowlOffsetY + kRamenDishShiftY) & ~1;
}

static void resetIdleTimers(uint32_t now) {
    faceTimer.init(now, 3000, 7000, 6000, 12000);
}

static void syncC5SensoryGaze(uint32_t now) {
    if (!Config::getC5Enabled() || !C5Monster::isConnected()) {
        c5SensoryRevision = 0;
        c5SensoryRevisionKnown = false;
        return;
    }

    const C5Monster::ScanResults& scan = C5Monster::getScanResults();
    if (scan.revision == 0 || scan.timestampMs == 0) return;
    if (c5SensoryRevisionKnown && scan.revision == c5SensoryRevision) return;

    c5SensoryRevision = scan.revision;
    c5SensoryRevisionKnown = true;
    // A new completed dual-band snapshot is a genuine sensory beat. The face
    // timer owns its bounded duration; no new C5 command is issued here.
    faceTimer.noticeSensoryEvent(now, PigEyeLook::FRONT_UP);
}

static void resetSleepBubble(uint32_t now) {
    sleepPhraseIdx = 0;
    lastSleepPhrase = now;
}

// Neon flicker pattern: ON 800→off 60→ON 200→off 40→ON 1500→OFF 300
bool isNeonOn(uint32_t now) {
    uint32_t t = (now - neonCycleStart) % NEON_CYCLE_MS;
    if (t < 800) return true;       // ON 800ms
    if (t < 860) return false;      // off 60ms
    if (t < 1060) return true;      // ON 200ms
    if (t < 1100) return false;     // off 40ms
    if (t < 2600) return true;      // ON 1500ms
    return false;                    // OFF 300ms
}


// ==[ ROOM NEON LIGHT SOURCE ]== dominant practical per room, drives pig/frame shading.
// Rooms 0 and 2 extinguish this key with their physical sign; other rooms keep
// continuous exterior/ambient volume while individual glyphs may flicker.
static ExteriorSprites::RenderOptions currentExteriorGrade() {
    ExteriorSprites::RenderOptions options;
    options.thunder = Weather::isThunderFlashing();
    options.tintActive = colorEvent.active;
    options.tintColor565 = colorEvent.color565;
    options.tintIntensity = colorEvent.intensity;
    return options;
}

PigLight getRoomNeonLight(int room, uint32_t now) {
    PigLight l;
    switch (room) {
        case 0: l.x = kR1_NeonX + kR1_NeonW / 2;  l.y = kR1_NeonY + kR1_NeonH / 2; l.tint = isRoom0SignLit(now) ? RP::NEON : (uint16_t)0; break;
        case 1: l.x = kR2_WindowX + 30;           l.y = kR2_WindowY + 30;           l.tint = RP::NEON; break;
        case 2: l.x = kR3_SignX + kR3_SignW / 2;
                l.y = kR3_SignY + kR3_SignH / 2;
                l.tint = isNeonOn(now) ? RP::NEON : (uint16_t)0;
                break;
        case 3: {
            ExteriorSprites::Emitter exterior = ExteriorSprites::dominantEmitter(
                ExteriorSprites::Scene::Rooftop, now,
                kR4_ExteriorX, kR4_ExteriorY,
                kR4_ExteriorW, kR4_ExteriorH,
                (int8_t)parallaxFar, currentExteriorGrade());
            l.x = exterior.x;
            l.y = exterior.y;
            l.tint = exterior.active ? exterior.color565 : RP::LED;
            break;
        }
        case 4: l.x = kR5_NeonX + kR5_NeonW / 2;  l.y = kR5_NeonY + kR5_NeonH / 2; l.tint = RP::NEON; break;
        case 5: {
            ExteriorSprites::Emitter exterior = ExteriorSprites::dominantEmitter(
                ExteriorSprites::Scene::Comfort, now,
                kR6_GlassX + kRoomPX, kR6_GlassY + kRoomPX,
                kR6_GlassW - kRoomPX * 2, kR6_GlassH - kRoomPX * 2,
                (int8_t)parallaxFar, currentExteriorGrade());
            l.x = exterior.x;
            l.y = exterior.y;
            l.tint = exterior.active ? exterior.color565 : RP::NEON;
            break;
        }
        default: break;
    }
    return l;
}

// ==[ HELPER PIG NEON LIGHT ]== HACK sign illuminates hub crate pig
// Same as above — light persists through flicker for continuous bump shading.
static PigLight getHelperNeonLight(uint32_t now) {
    PigLight l;
    l.x = kNeonX + kNeonW / 2;
    l.y = kNeonY + kNeonH;
    l.tint = RP::NEON;
    return l;
}

Room2LightingFrame buildRoom2Emitters(int cookWx, int cookWy, int bedWx, int bedWy,
                                             int bowlFx, int bowlFy, bool bowlHeldByPig,
                                             int signParallax, uint32_t now) {
    Room2LightingFrame lighting;
    lighting.valid = true;
    lighting.counterX = kR3_CounterX + cookWx;
    lighting.counterY = kR3_CounterY + cookWy;
    lighting.podX = kR3_PodX + bedWx;
    lighting.podY = kR3_PodY + bedWy;
    lighting.signX = kR3_SignX + signParallax;
    lighting.signY = kR3_SignY;
    lighting.bowlX = bowlFx;
    lighting.bowlY = bowlFy;
    lighting.bowlHeldByPig = bowlHeldByPig;

    lighting.sign.x = (int16_t)(lighting.signX + kR3_SignW / 2);
    lighting.sign.y = (int16_t)(lighting.signY + kR3_SignH / 2);
    lighting.sign.tint = isNeonOn(now) ? RP::NEON : (uint16_t)0;

    ExteriorSprites::Emitter windowEmitter = ExteriorSprites::dominantEmitter(
        ExteriorSprites::Scene::Ramen, now,
        kR3_WindowX + kRoomPX, kR3_WindowY + kRoomPX,
        kR3_WindowW - kRoomPX * 2, kR3_WindowH - kRoomPX * 2,
        (int8_t)signParallax, currentExteriorGrade());
    lighting.window.x = windowEmitter.x;
    lighting.window.y = windowEmitter.y;
    lighting.window.tint = windowEmitter.active ? windowEmitter.color565 : RP::WARM;

    lighting.pod.x = (int16_t)((lighting.podX + 6) & ~1);
    lighting.pod.y = (int16_t)((lighting.podY + 8) & ~1);
    lighting.pod.tint = podLedOn ? RP::LED : 0;

    int bowlCx = bowlHeldByPig ? (bowlFx + 10) : (bowlFx + kR3_BowlW / 2);
    int bowlCy = bowlHeldByPig ? (bowlFy + 10) : (bowlFy + 8);
    lighting.bowl.x = (int16_t)(bowlCx & ~1);
    lighting.bowl.y = (int16_t)(bowlCy & ~1);
    lighting.bowl.tint = RP::WARM;
    return lighting;
}

float room2EmitterScoreAt(const PigLight& light, float px, float py,
                                 float radius, float weight) {
    if (light.tint == 0 || radius <= 1.0f) return 0.0f;
    float dx = (float)light.x - px;
    float dy = (float)light.y - py;
    float dist2 = dx * dx + dy * dy;
    float maxR2 = radius * radius;
    if (dist2 >= maxR2) return 0.0f;
    float t = 1.0f - dist2 / maxR2;
    return weight * t * t;
}

PigLight selectRoom2PigKeyLight(int pigDrawX, int pigDrawY, uint32_t now) {
    if (!room2LightingRuntime.valid) return getRoomNeonLight(2, now);

    float pigCX = (float)(pigDrawX + kPigW / 2);
    float pigCY = (float)(pigDrawY + kPigH / 2);

    float signScore = room2EmitterScoreAt(room2LightingRuntime.sign, pigCX, pigCY, 156.0f, 1.10f);
    float windowScore = room2EmitterScoreAt(room2LightingRuntime.window, pigCX, pigCY, 184.0f, 0.90f);
    float podScore = room2EmitterScoreAt(room2LightingRuntime.pod, pigCX, pigCY, 108.0f, 1.00f);
    float bowlScore = room2EmitterScoreAt(room2LightingRuntime.bowl, pigCX, pigCY, 92.0f, 1.15f);

    if (currentStation == Station::COOKING) {
        if (bowlScore > signScore * 1.05f && bowlScore > 0.04f) return room2LightingRuntime.bowl;
        if (signScore > 0.02f) return room2LightingRuntime.sign;
        if (windowScore > podScore) return room2LightingRuntime.window;
        return room2LightingRuntime.pod.tint != 0 ? room2LightingRuntime.pod : room2LightingRuntime.sign;
    }

    if (currentStation == Station::IN_BED) {
        podScore *= 1.35f;
        windowScore *= 0.92f;
        signScore *= 0.55f;
        if (podScore >= windowScore && podScore >= signScore && podScore > 0.03f) {
            return room2LightingRuntime.pod;
        }
        if (windowScore >= signScore) return room2LightingRuntime.window;
        return room2LightingRuntime.sign;
    }

    PigLight best = room2LightingRuntime.sign;
    float bestScore = signScore;
    if (windowScore > bestScore) {
        bestScore = windowScore;
        best = room2LightingRuntime.window;
    }
    if (podScore > bestScore) {
        best = room2LightingRuntime.pod;
    }
    return best;
}

PigLight selectRoomPigKeyLight(int room, int pigDrawX, int pigDrawY, uint32_t now) {
    PigLight key;
    switch (room) {
        case 0: key = selectRoom0PigKeyLight(pigDrawX, pigDrawY, now); break;
        case 1: key = selectRoom1PigKeyLight(pigDrawX, pigDrawY, now); break;
        case 2: key = selectRoom2PigKeyLight(pigDrawX, pigDrawY, now); break;
        case 3: key = selectRoom3PigKeyLight(pigDrawX, pigDrawY, now); break;
        case 4: key = selectRoom4PigKeyLight(pigDrawX, pigDrawY, now); break;
        case 5: key = selectRoom5PigKeyLight(pigDrawX, pigDrawY, now); break;
        default: key = getRoomNeonLight(room, now); break;
    }

    // Storm light owns the interior window direction. Rooftop lightning is
    // room-local because its bolt has a real mapped position in room 3.
    if (Weather::isThunderFlashing() && (room == 1 || room == 2 || room == 5)) {
        key.x = (int16_t)(room == 2
            ? kR3_WindowX + kR3_WindowW / 2
            : room == 5 ? kR6_GlassX + kR6_GlassW / 2
            : kR2_WindowX + kR2_WindowW / 2);
        key.y = (int16_t)(room == 2 ? kR3_WindowY
            : room == 5 ? kR6_GlassY
            : kR2_WindowY);
        key.tint = RP::FLUOR;
    }
    return key;
}

static PigLight selectCompanionCatKeyLight(
        const PancettaCat::Pose& pose, uint32_t now, bool helperScene) {
    if (helperScene) return getHelperNeonLight(now);

    // Existing room selectors score their emitters from a body center. Map the
    // cat's measured footprint center into that seam so the same sampled CRT,
    // window, neon, searchlight, thunder, or bath practical owns both actors.
    const int virtualPigX = pose.x + PancettaCat::kWidth / 2 - kPigW / 2;
    const int virtualPigY = pose.y + PancettaCat::kHeight / 2 - kPigH / 2;
    return selectRoomPigKeyLight(currentRoom, virtualPigX, virtualPigY, now);
}

// The batted object is latched to a surface and the cat is not, so it cannot
// borrow his key light. Scored from his body it took the desk lamp clear
// across room 0 rather than the server rack it is sitting under, and every
// time he crossed the floor the winning emitter changed beneath a tin that had
// not moved - CRT, lamp, rack, neon, with the contact shadow flipping sides
// partway. It is a thing in the room and is lit from where it is.
static PigLight selectTrinketKeyLight(uint32_t now) {
    int16_t centerX = 0;
    int16_t centerY = 0;
    if (!PancettaCat::trinketCenter(centerX, centerY)) return PigLight{};
    return selectRoomPigKeyLight(currentRoom, centerX - kPigW / 2,
                                 centerY - kPigH / 2, now);
}

// One seam decides how much of the companion the room is allowed to show this
// frame. A concealed arrival is standing behind furniture the room has already
// painted, so his silhouette is clipped to the span he has cleared and nothing
// of him is drawn over the object he is behind. The clip is dropped the frame
// he steps out of it, restoring the room's ordinary order.
static void drawCompanionCat(M5Canvas& canvas, const PancettaCat::Pose& pose,
                             uint32_t now, bool helperScene) {
    if (!PancettaCat::isVisible()) return;
    int16_t clipX = 0;
    int16_t clipW = 0;
    const bool concealed = PancettaCat::concealClip(clipX, clipW);
    if (concealed) {
        if (clipW <= 0) return;
        canvas.setClipRect(clipX, kRoomY, clipW,
                           kFloorY + kRoomPX - kRoomY);
    }
    PancettaCat::draw(canvas, pose, PancettaCat::isNavigating(),
                      PancettaCat::animationActivity(), now,
                      selectCompanionCatKeyLight(pose, now, helperScene));
    if (concealed) canvas.clearClipRect();
}


// ==[ STATION POSITION LOOKUP ]==

static void getStationPos(Station st, float& x, float& y, bool& fr) {
    switch (st) {
        case Station::AT_LAPTOP:
            x = (float)kR1_PigX; y = (float)kR1_PigY; fr = false; break;
        case Station::ON_SOFA:
            x = (float)kR2_SofaPigX; y = (float)kR2_SofaPigY; fr = true; break;
        case Station::AT_WINDOW:
            x = (float)kR2_WindowPigX; y = (float)kR2_WindowPigY; fr = true; break;
        case Station::COOKING:
            x = (float)kR3_CookPigX; y = (float)kR3_CookPigY; fr = true; break;
        case Station::IN_BED:
            x = (float)kR3_BedPigX; y = (float)kR3_BedPigY; fr = false; break;
        case Station::AT_ANTENNA:
            x = (float)kR4_AntennaPigX; y = (float)kR4_AntennaPigY; fr = false; break;
        case Station::ON_LEDGE:
            x = (float)kR4_LedgePigX; y = (float)kR4_LedgePigY; fr = true; break;
        case Station::AT_TERMINAL:
            x = (float)kR5_TermPigX; y = (float)kR5_TermPigY; fr = false; break;
        case Station::AT_BOOTH:
            x = (float)kR5_BoothPigX; y = (float)kR5_BoothPigY; fr = false; break;
        case Station::IN_BATH:
            x = (float)kR6_BathPigX; y = (float)kR6_BathPigY; fr = true; break;
    }
}

static int stationRoom(Station st) {
    switch (st) {
        case Station::AT_LAPTOP: return 0;
        case Station::ON_SOFA:
        case Station::AT_WINDOW: return 1;
        case Station::COOKING:
        case Station::IN_BED: return 2;
        case Station::AT_ANTENNA:
        case Station::ON_LEDGE: return 3;
        case Station::AT_TERMINAL:
        case Station::AT_BOOTH: return 4;
        case Station::IN_BATH: return 5;
    }
    return 0;
}

static float bathApproachXFor(float fromX) {
    float bathCenterX = (float)(kR6_BathPigX + kPigW / 2);
    float pigCenterX = fromX + (float)kPigW * 0.5f;
    return pigCenterX <= bathCenterX
        ? (float)kR6_BathApproachLeftX
        : (float)kR6_BathApproachRightX;
}

static float stationApproachX(Station target, float fromX) {
    if (target == Station::IN_BATH) return bathApproachXFor(fromX);
    float x, y;
    bool faceRightAtStation;
    getStationPos(target, x, y, faceRightAtStation);
    return x;
}

static float bathExitApproachX() {
    bool exitsRight = walkTargetRoom == (currentRoom + 1) % NUM_ROOMS;
    return exitsRight ? (float)kR6_BathApproachRightX
                      : (float)kR6_BathApproachLeftX;
}

static void beginBathDismount(uint32_t now, float exitX) {
    // The authored idle dive/shake lives outside pigX/pigY. Capture that exact
    // visible pose before DISMOUNTING disables the idle sampler, otherwise an
    // exit requested mid-dive snaps Pancetta up by as much as 16px.
    BathIdleFrame bathIdle = sampleBathIdleFrame(now);
    jumpFromX = pigX + (float)bathIdle.shakeX;
    jumpFromY = pigY + (float)bathIdle.submergeY;
    jumpToX = exitX;
    jumpToY = (float)kFloorPigY;
    jumpStart = now;
    faceRight = jumpToX > jumpFromX;
    pigX = jumpFromX;
    pigY = jumpFromY;
    roamState = RoamState::DISMOUNTING;
}

static float travelYForStation(float t, float fromY, float toY, Station /*target*/) {
    // All elevated stations walk at floor level — jump handles Y change.
    // This lerp only matters for floor-to-floor transitions now.
    return fromY + (toY - fromY) * t;
}

// ==[ ARRIVAL ]==

static void arriveAtStation(uint32_t now) {
    jumpSquashPx = 0.0f;
    currentStation = walkTargetStation;
    float stX, stY;
    bool stFR;
    getStationPos(currentStation, stX, stY, stFR);
    pigX = stX; pigY = stY;
    // Rear-view/lead-in stations: pause before settling into final idle pose.
    if (currentStation == Station::AT_LAPTOP ||
        currentStation == Station::AT_WINDOW ||
        currentStation == Station::COOKING ||
        currentStation == Station::AT_ANTENNA ||
        currentStation == Station::AT_TERMINAL) {
        roamState = RoamState::SETTLING;
        settleStart = now;
        // Keep facing direction from walk (will flip in SETTLING completion)
    } else {
        // Bath entry is side-aware. Keep the direction established by the
        // mounting arc instead of mirroring a right-side approach on contact.
        if (currentStation != Station::IN_BATH) faceRight = stFR;
        roamState = RoamState::IDLE;
    }
    stationStart = now;
    stationStayStart = now;  // current-visit surveillance timer
    lastStationDwellTick = now;
    trackerSurvivalAwarded = false;
    stationDuration = randomRange(STATION_MIN_MS, STATION_MAX_MS);
    // bath is sticky — pig stays until manually cycled
    if (currentStation == Station::IN_BATH) stationDuration = UINT32_MAX;
    // reset terminal delay for new station
    terminalDelayActive = false;
    terminalDecided = false;

    // WD0: force car cinematic on arrival at ON_LEDGE
    if (wdCinePhase == WDCinePhase::WD_WALK && currentStation == Station::ON_LEDGE) {
        wdCinePhase = WDCinePhase::WD_WAIT;
        wdCarForceStart = true;
    }

    // ==[ ROOM VISIT TRACKING ]== set bitmask on arrival
    if (currentRoom < NUM_ROOMS) {
        roomsVisitedMask |= (1 << currentRoom);
        roomProgressLastRefresh = 0;
        Challenges::onRoomVisited(roomsVisitedMask);

        // FULL CIRCUIT: all six rooms visited
        if (!fullCircuitClaimed && roomsVisitedMask == 0x3F) {
            fullCircuitClaimed = true;
            Achievements::tryUnlock(Achievement::FULL_CIRCUIT);
            if (Config::isSessionActive()) {
                char gain[24];
                ProgressionText::formatGainAmount(gain, sizeof(gain), 20);
                Config::addXP(20, Config::RewardSource::ROOM_CIRCUIT);
                Mood::addMomentum(10);
                if (DefhogTerminal::isVisible()) {
                    DefhogTerminal::pushLineHype("FULL C1RCU1T! ALL R00MS %s", gain);
                }
            } else {
                if (DefhogTerminal::isVisible()) {
                    DefhogTerminal::pushLineHype("FULL C1RCU1T! ALL R00MS");
                }
            }
        }
    }
    // BLE spam → pig paces nervously, shorter station stays (not the bath)
    if (currentStation != Station::IN_BATH &&
        Config::getIppEnabled() && DefensePipeline::snapshot().getSpamCount() > 0) {
        stationDuration = randomRange(STATION_MIN_MS / 2, STATION_MAX_MS / 2);
    }
    wobbleStart = now;
    resetCharacterFidget(now);
    // Rear-view stations spend SETTLE_MS facing into the room. Start their
    // flourish only after the turn so the entire authored beat is visible.
    arrivalAnimStart = (roamState == RoamState::IDLE) ? now : 0;
    // Every normal 12-22s visit gets at least one quiet character beat.
    resetIdleTimers(now);
    // Cigarette persists while traveling and clears only on next station arrival.
    windowCigLit = (currentStation == Station::AT_WINDOW);

    // Refresh narrator bubble on station arrival
    lastNarratorBubble = now;
    refreshNarratorBubble();
    if (currentRoom == 4 && narratorBubbleText[0])
        Barman::onPancettaSpoke(now);

    switch (currentStation) {
        case Station::AT_LAPTOP:
            avatarState = AvatarState::HAPPY;
            lastLaptopBubble = now;
            laptopMomentumAwarded = false;
            laptopLineIdx = (uint8_t)(esp_random() % LAPTOP_LINE_COUNT);
            break;
        case Station::ON_SOFA:
            avatarState = AvatarState::SLEEPY;
            resetSleepBubble(now);
            break;
        case Station::AT_WINDOW:
            avatarState = AvatarState::NEUTRAL;
            break;
        case Station::COOKING:
            avatarState = AvatarState::NEUTRAL;
            break;
        case Station::IN_BED:
            avatarState = AvatarState::SLEEPY;
            resetSleepBubble(now);
            lastBedMomentum = now;
            break;
        case Station::AT_ANTENNA:
            avatarState = AvatarState::HUNTING;
            break;
        case Station::ON_LEDGE:
            avatarState = AvatarState::NEUTRAL;
            break;
        case Station::AT_TERMINAL:
            avatarState = AvatarState::HAPPY;
            break;
        case Station::AT_BOOTH:
            avatarState = AvatarState::NEUTRAL;
            break;
        case Station::IN_BATH:
            avatarState = AvatarState::HAPPY;
            break;
    }
}

// ==[ TELEPORT PARTICLE SYSTEM ]== renderers delegated to Teleport:: module

static bool stationUsesRearStableSilhouette(Station station) {
    return station == Station::AT_LAPTOP ||
           station == Station::AT_WINDOW ||
           station == Station::AT_ANTENNA ||
           station == Station::AT_TERMINAL;
}

static bool stationBeginsRearAfterRoomTeleport(Station station) {
    // Cooking owns a rear settle/lead-in, then turns side-on for the meal.
    return station == Station::COOKING ||
           stationUsesRearStableSilhouette(station);
}

static Teleport::PigSilhouette stationTeleportSilhouette(Station station,
                                                          bool facesRight) {
    if (stationBeginsRearAfterRoomTeleport(station))
        return Teleport::PigSilhouette::REAR;
    return facesRight ? Teleport::PigSilhouette::SIDE_RIGHT
                      : Teleport::PigSilhouette::SIDE_LEFT;
}

static void samplePigParticles(TeleportContext context, bool destFacesRight) {
    bool sourceRear = context != TeleportContext::HELPER_TO_ROOM &&
                      stationUsesRearStableSilhouette(currentStation);
    Teleport::PigSilhouette source = sourceRear
        ? Teleport::PigSilhouette::REAR
        : (faceRight ? Teleport::PigSilhouette::SIDE_RIGHT
                     : Teleport::PigSilhouette::SIDE_LEFT);
    if (context == TeleportContext::HELPER_TO_ROOM)
        source = Teleport::PigSilhouette::SIDE_RIGHT;
    Teleport::PigSilhouette destination =
        stationTeleportSilhouette(walkTargetStation, destFacesRight);

    uint8_t count = 0;
    Teleport::samplePigParticles(teleportSourceParticles, count,
                                 MAX_TELEPORT_PARTICLES, source);
    teleportSourceParticleCount = (int)count;
    count = 0;
    Teleport::samplePigParticles(teleportDestinationParticles, count,
                                 MAX_TELEPORT_PARTICLES, destination);
    teleportDestinationParticleCount = (int)count;
}

// The companion goes through the portal with Pancetta or he does not go at
// all: a cat who was not in the room has nothing to decompose, and the room on
// the far side rolls its own visit for him exactly as it always has.
static void beginCompanionTeleport(uint32_t now, float destX) {
    catTeleportActive = false;
    catTeleportLandingPending = false;
    if (!PancettaCat::isVisible()) return;

    const PancettaCat::Pose from = PancettaCat::currentPose();
    catTeleportFromX = (float)(from.x + PancettaCat::kWidth / 2);
    catTeleportFromY = (float)(from.y + PancettaCat::kHeight / 2);

    // He comes out beside the station Pancetta lands at, on the room floor,
    // preferring the trailing side so the two silhouettes do not overlap on
    // the frame the particles resolve. If that side is off the playfield he
    // takes the other one.
    const int gap = kPigPX * 6;
    int landX = (int)destX - PancettaCat::kWidth - gap;
    if (landX < kPigPX) landX = (int)destX + kPigW + gap;
    landX = max(kPigPX,
                min(SCREEN_WIDTH - kPigPX - PancettaCat::kWidth, landX));
    catTeleportLanding.x = (int16_t)(landX & ~(kPigPX - 1));
    catTeleportLanding.y =
        (int16_t)(PancettaCat::originYForSupport(kFloorY) & ~(kPigPX - 1));
    // He lands looking at Pancetta, not at whichever wall he left facing.
    catTeleportLanding.faceRight = landX < (int)destX;

    catTeleportDestCenterX = catTeleportLanding.x + PancettaCat::kWidth / 2;
    catTeleportDestCenterY = catTeleportLanding.y + PancettaCat::kHeight / 2;

    Teleport::sampleCatParticles(catTeleportSourceParticles,
                                 catTeleportSourceParticleCount,
                                 Teleport::MAX_CAT_PARTICLES,
                                 from.faceRight);
    Teleport::sampleCatParticles(catTeleportDestParticles,
                                 catTeleportDestParticleCount,
                                 Teleport::MAX_CAT_PARTICLES,
                                 catTeleportLanding.faceRight);
    catTeleportActive = true;
    PancettaCat::beginPortalTransit(now);
}

// Used by every path that tears a teleport down out of band. The natural
// completion does not come through here — it defers the landing by one step so
// the destination room is told he is arriving before he has arrived.
static void finishCompanionTeleport(uint32_t now) {
    catTeleportActive = false;
    catTeleportLandingPending = false;
    PancettaCat::endPortalTransit(now, catTeleportLanding);
}

static void startTeleport(uint32_t now,
                          TeleportContext context = TeleportContext::ROOM_TO_ROOM) {
    teleportContext = context;
    teleportPhase = TeleportPhase::DECOMPOSE;
    teleportStart = now;
    teleportJumpFromX = pigX + 36.0f;  // source pig center X
    teleportJumpFromY = pigY + 21.0f;  // source pig center Y

    // Compute destination pig center
    float destX, destY;
    bool destFR;
    getStationPos(walkTargetStation, destX, destY, destFR);
    samplePigParticles(context, destFR);
    destPigCenterX = (int)destX + 36;
    destPigCenterY = (int)destY + 21;
    beginCompanionTeleport(now, destX);
}

static void updateTeleport(uint32_t now) {
    uint32_t elapsed = now - teleportStart;

    if (elapsed < TP_DECOMPOSE_MS) {
        teleportPhase = TeleportPhase::DECOMPOSE;
    } else if (elapsed < TP_DECOMPOSE_MS + TP_VOID_MS) {
        if (teleportPhase != TeleportPhase::VOID) {
            teleportPhase = TeleportPhase::VOID;
            currentRoom = walkTargetRoom;
        }
    } else if (elapsed < TP_DECOMPOSE_MS + TP_VOID_MS + TP_REASSEMBLE_MS) {
        teleportPhase = TeleportPhase::REASSEMBLE;
    } else if (elapsed < TP_TOTAL_MS) {
        teleportPhase = TeleportPhase::SETTLE;
    } else {
        // Teleport complete — arrive at station directly
        teleportPhase = TeleportPhase::NONE;
        // The companion's body comes back one step later, after the companion
        // state pass has told the destination room he is arriving in it.
        // Handing it back here would let that room's own visit roll put him
        // straight into hiding on the frame he lands.
        if (catTeleportActive) {
            catTeleportActive = false;
            catTeleportLandingPending = true;
        }
        TeleportContext context = teleportContext;
        teleportContext = TeleportContext::ROOM_TO_ROOM;
        float stX, stY;
        bool stFR;
        getStationPos(walkTargetStation, stX, stY, stFR);
        pigX = stX;
        pigY = stY;
        faceRight = stFR;
        if (context == TeleportContext::HELPER_TO_ROOM) {
            mode = PigMode::ROAMING;
        }
        arriveAtStation(now);
    }
}

// drawTeleportEffect — delegates each phase to Teleport:: shared renderers
static void drawTeleportEffect(M5Canvas& canvas, uint32_t now,
                                uint16_t fg, uint16_t bg) {
    uint32_t elapsed = now - teleportStart;

    // The companion is the far actor here too: his stream is laid down first
    // so Pancetta's owns the overlap, and it never draws the ring — one portal,
    // one ring, painted by the pass that owns it.
    switch (teleportPhase) {
        case TeleportPhase::DECOMPOSE: {
            float t = (float)elapsed / (float)TP_DECOMPOSE_MS;
            if (t > 1.0f) t = 1.0f;
            if (catTeleportActive)
                Teleport::drawDecomposeParticles(canvas,
                    catTeleportFromX, catTeleportFromY,
                    (float)PORTAL_X, (float)PORTAL_Y,
                    t, catTeleportSourceParticles,
                    catTeleportSourceParticleCount, fg, bg, false);
            Teleport::drawDecomposeParticles(canvas,
                teleportJumpFromX, teleportJumpFromY,
                (float)PORTAL_X, (float)PORTAL_Y,
                t, teleportSourceParticles, (uint8_t)teleportSourceParticleCount,
                fg, bg);
            break;
        }
        case TeleportPhase::VOID: {
            float t = (float)(elapsed - TP_DECOMPOSE_MS) / (float)TP_VOID_MS;
            Teleport::drawVoidPortal(canvas, PORTAL_X, PORTAL_Y, t);
            break;
        }
        case TeleportPhase::REASSEMBLE: {
            float t = (float)(elapsed - TP_DECOMPOSE_MS - TP_VOID_MS) /
                      (float)TP_REASSEMBLE_MS;
            if (t > 1.0f) t = 1.0f;
            if (catTeleportActive)
                Teleport::drawReassembleParticles(canvas,
                    (float)PORTAL_X, (float)PORTAL_Y,
                    catTeleportDestCenterX, catTeleportDestCenterY,
                    t, catTeleportDestParticles,
                    (int)catTeleportDestParticleCount, fg, bg, false);
            Teleport::drawReassembleParticles(canvas,
                (float)PORTAL_X, (float)PORTAL_Y,
                destPigCenterX, destPigCenterY,
                t, teleportDestinationParticles,
                teleportDestinationParticleCount,
                fg, bg);
            break;
        }
        case TeleportPhase::SETTLE: {
            float t = (float)(elapsed - TP_DECOMPOSE_MS - TP_VOID_MS - TP_REASSEMBLE_MS) /
                      (float)TP_SETTLE_MS;
            if (t > 1.0f) t = 1.0f;
            if (catTeleportActive)
                Teleport::drawSettleParticles(canvas,
                    catTeleportDestCenterX, catTeleportDestCenterY,
                    t, catTeleportDestParticles,
                    (int)catTeleportDestParticleCount, fg);
            Teleport::drawSettleParticles(canvas,
                destPigCenterX, destPigCenterY,
                t, teleportDestinationParticles,
                teleportDestinationParticleCount, fg);
            break;
        }
        default:
            break;
    }
}

bool isStationVisualActive(Station station) {
    if (teleportPhase == TeleportPhase::DECOMPOSE)
        return currentStation == station;
    const bool landing = teleportPhase == TeleportPhase::REASSEMBLE ||
                         teleportPhase == TeleportPhase::SETTLE;
    if (landing) return walkTargetStation == station;
    return currentStation == station &&
           (roamState == RoamState::IDLE || roamState == RoamState::SETTLING);
}

PigPose resolveStationVisualPose(uint32_t now) {
    const bool landing = teleportPhase == TeleportPhase::REASSEMBLE ||
                         teleportPhase == TeleportPhase::SETTLE;
    if (landing) {
        return {
            destPigCenterX - kPigW / 2,
            destPigCenterY - kPigH / 2,
            false,
            false,
            0,
        };
    }
    return resolvePigPose(now, false, false, false);
}

void drawTeleportPortalRing(M5Canvas& canvas, int cx, int cy, int radius) {
    Teleport::drawPortalRing(canvas, cx, cy, radius);
}

void sampleTeleportParticles(TeleportParticleSample* outParticles,
                             uint8_t& outCount, uint8_t maxCount) {
    Teleport::samplePigParticles(outParticles, outCount, maxCount);
}

void drawTeleportCollapseFrame(M5Canvas& canvas,
                               int sourceCenterX, int sourceCenterY,
                               int portalCenterX, int portalCenterY,
                               float collapseT,
                               const TeleportParticleSample* particles,
                               uint8_t particleCount) {
    Teleport::drawDecomposeParticles(canvas,
                                     (float)sourceCenterX, (float)sourceCenterY,
                                     (float)portalCenterX, (float)portalCenterY,
                                     collapseT,
                                     particles, particleCount,
                                     Display::getColorFG(),
                                     Display::getColorBG());
}

// ==[ WALK SETUP HELPER ]== sets up walk-to-edge or walk-to-station from current pos
// Called after dismount completes or directly from pickNextStation when already at floor.
static void beginWalkToTarget(uint32_t now) {
    jumpSquashPx = 0.0f;
    walkLegDist = 0.0f;
    walkLegBaseDist = 0.0f;
    if (walkTargetRoom != currentRoom) {
        // Room change — walk to screen edge
        walkFromX = pigX;
        walkFromY = pigY;
        if (walkTargetRoom == (currentRoom + 1) % NUM_ROOMS) {
            walkToX = (float)SCREEN_WIDTH;
            roomSlideDir = 1;
        } else {
            walkToX = (float)-kPigW;
            roomSlideDir = -1;
        }
        walkToY = (float)kFloorPigY;
        walkStart = now;
        walkDurationMs = authoredWalkDuration(walkFromX, walkFromY,
                                              walkToX, walkToY);
        faceRight = (walkToX > walkFromX);
        roamState = RoamState::WALKING_TO;
    } else {
        // Same room — walk to station X
        float destX, destY;
        bool destFR;
        getStationPos(walkTargetStation, destX, destY, destFR);
        walkFromX = pigX;
        walkFromY = pigY;
        walkToX = stationApproachX(walkTargetStation, pigX);
        // Elevated targets: walk at floor level, mount phase handles Y
        if (destY < (float)kFloorPigY - 2.0f) {
            walkToY = (float)kFloorPigY;
        } else {
            walkToY = destY;
        }
        walkToFaceRight = destFR;
        walkStart = now;
        walkDurationMs = authoredWalkDuration(walkFromX, walkFromY,
                                              walkToX, walkToY);
        faceRight = (walkToX > walkFromX);
        roamState = RoamState::WALKING_TO;
    }
}

// ==[ NEXT STATION PICK ]==

static void pickNextStation(uint32_t now) {
    if (isWDCinematicActive()) return;  // pig locked in wardrive scene
    // hide DEFHOG4 terminal when leaving station
    if (DefhogTerminal::isVisible()) {
        DefhogTerminal::hide();
    }

    // cancel 4th wall break — prevent stale savedFaceRight overwrite
    if (wallBreakActive) {
        wallBreakActive = false;
        faceRight = savedFaceRight;
    }
    // Coffee cup: pick up when leaving ramen bar
    if (currentStation == Station::COOKING) {
        carryingCup = true;
        cupPickupTime = now;
        Mood::addMomentum(3);
    }

    // Decide: same room or room change?
    bool changeRoom = ((int)(esp_random() % 100) < ROOM_CHANGE_CHANCE);

    Station candidates[5];
    int n = 0;

    if (changeRoom) {
        // Pick a station elsewhere on the six-room circuit.
        int nextRoom = (currentRoom + 1 + (int)(esp_random() % (NUM_ROOMS - 1))) % NUM_ROOMS;
        switch (nextRoom) {
            case 0:
                candidates[n++] = Station::AT_LAPTOP;
                break;
            case 1:
                candidates[n++] = Station::ON_SOFA;
                candidates[n++] = Station::AT_WINDOW;
                break;
            case 2:
                candidates[n++] = Station::COOKING;
                candidates[n++] = Station::IN_BED;
                break;
            case 3:
                candidates[n++] = Station::AT_ANTENNA;
                candidates[n++] = Station::ON_LEDGE;
                break;
            case 4:
                candidates[n++] = Station::AT_TERMINAL;
                candidates[n++] = Station::AT_BOOTH;
                break;
            case 5:
                candidates[n++] = Station::IN_BATH;
                break;
        }
    } else {
        // Same room, different station
        switch (currentRoom) {
            case 0: {
                // Only one station in room 0 — pick random other room (not just room 1)
                int r0next = 1 + (int)(esp_random() % (NUM_ROOMS - 1));
                switch (r0next) {
                    case 1: candidates[n++] = Station::ON_SOFA;
                            candidates[n++] = Station::AT_WINDOW; break;
                    case 2: candidates[n++] = Station::COOKING;
                            candidates[n++] = Station::IN_BED; break;
                    case 3: candidates[n++] = Station::AT_ANTENNA;
                            candidates[n++] = Station::ON_LEDGE; break;
                    case 4: candidates[n++] = Station::AT_TERMINAL;
                            candidates[n++] = Station::AT_BOOTH; break;
                    default: candidates[n++] = Station::IN_BATH; break;
                }
                break;
            }
            case 1:
                if (currentStation != Station::ON_SOFA) candidates[n++] = Station::ON_SOFA;
                if (currentStation != Station::AT_WINDOW) candidates[n++] = Station::AT_WINDOW;
                break;
            case 2:
                if (currentStation != Station::COOKING) candidates[n++] = Station::COOKING;
                if (currentStation != Station::IN_BED) candidates[n++] = Station::IN_BED;
                break;
            case 3:
                if (currentStation != Station::AT_ANTENNA) candidates[n++] = Station::AT_ANTENNA;
                if (currentStation != Station::ON_LEDGE) candidates[n++] = Station::ON_LEDGE;
                break;
            case 4:
                if (currentStation != Station::AT_TERMINAL) candidates[n++] = Station::AT_TERMINAL;
                if (currentStation != Station::AT_BOOTH) candidates[n++] = Station::AT_BOOTH;
                break;
            case 5: {
                // The bath is the balcony's only station. Send Pancetta back
                // into the casework circuit instead of selecting himself.
                int nextRoom = (int)(esp_random() % (NUM_ROOMS - 1));
                switch (nextRoom) {
                    case 0: candidates[n++] = Station::AT_LAPTOP; break;
                    case 1: candidates[n++] = Station::ON_SOFA; break;
                    case 2: candidates[n++] = Station::COOKING; break;
                    case 3: candidates[n++] = Station::AT_ANTENNA; break;
                    default: candidates[n++] = Station::AT_BOOTH; break;
                }
                break;
            }
        }
    }

    if (n == 0) {
        candidates[n++] = Station::ON_SOFA;
    }

    // ==[ IPP STATION AFFINITY ]== bias station pick based on recon state
    if (Config::getIppEnabled() && DefensePipeline::snapshot().isActive()) {
        Station preferred = candidates[0];  // default fallback
        bool hasBias = false;

        if (DefensePipeline::snapshot().hasActiveAttacker() || DefensePipeline::snapshot().isDualBandStalkActive()) {
            // attacker identified or dual-band stalk → terminal (investigate)
            for (int i = 0; i < n; i++) {
                if (candidates[i] == Station::AT_TERMINAL) {
                    preferred = Station::AT_TERMINAL; hasBias = true; break;
                }
            }
        } else if (DefensePipeline::snapshot().getHighConfidenceCohortCount() > 0) {
            // cohort pair detected → window (surveilling)
            for (int i = 0; i < n; i++) {
                if (candidates[i] == Station::AT_WINDOW) {
                    preferred = Station::AT_WINDOW; hasBias = true; break;
                }
            }
        } else if (DefensePipeline::snapshot().getFollowingCount() > 0) {
            // tracker following → gravitate to window (keep watching)
            for (int i = 0; i < n; i++) {
                if (candidates[i] == Station::AT_WINDOW) {
                    preferred = Station::AT_WINDOW; hasBias = true; break;
                }
            }
        } else if (DefensePipeline::snapshot().getKnownAPCount() > 0) {
            // known AP found → gravitate to laptop (check the data)
            for (int i = 0; i < n; i++) {
                if (candidates[i] == Station::AT_LAPTOP) {
                    preferred = Station::AT_LAPTOP; hasBias = true; break;
                }
            }
        } else if (DefensePipeline::snapshot().isScanning()) {
            // active scan → gravitate to antenna (check signals)
            for (int i = 0; i < n; i++) {
                if (candidates[i] == Station::AT_ANTENNA) {
                    preferred = Station::AT_ANTENNA; hasBias = true; break;
                }
            }
        } else if (!DefensePipeline::snapshot().hasThreats()) {
            // quiet wire → gravitate to sofa or booth (relaxed)
            for (int i = 0; i < n; i++) {
                if (candidates[i] == Station::ON_SOFA ||
                    candidates[i] == Station::AT_BOOTH ||
                    candidates[i] == Station::IN_BATH) {
                    preferred = candidates[i]; hasBias = true; break;
                }
            }
        }

        // 60% chance to follow bias, 40% random (don't be too predictable)
        if (hasBias && (esp_random() % 100) < 60) {
            n = 1;
            candidates[0] = preferred;
        }
    }

    Station target = candidates[esp_random() % n];
    int targetRoom = stationRoom(target);

    walkTargetStation = target;
    walkTargetRoom = targetRoom;

    // Teleport path — dissolve from current pos, no dismount needed
    if (targetRoom != currentRoom &&
        currentStation != Station::IN_BATH &&
        target != Station::IN_BATH) {
        useTeleportTransition = ((esp_random() & 1) == 0);
        if (useTeleportTransition) {
            roamState = RoamState::ROOM_TRANSITION;
            transFromRoom = currentRoom;
            transToRoom = targetRoom;
            roomTransStart = now;
            startTeleport(now);
            return;
        }
    }

    // Walk path — dismount from elevated station first if needed
    bool needDismount = pigY < (float)kFloorPigY - 2.0f;
    if (needDismount) {
        jumpFromX = pigX;
        jumpToX = currentStation == Station::IN_BATH
            ? bathExitApproachX() : pigX;
        jumpFromY = pigY;
        jumpToY = (float)kFloorPigY;
        jumpStart = now;
        if (currentStation == Station::IN_BATH)
            faceRight = jumpToX > jumpFromX;
        roamState = RoamState::DISMOUNTING;
    } else {
        beginWalkToTarget(now);
    }

}

// ==[ ROOM PARALLAX ]== cached IMU attitude drives bounded far/mid/near layers.
int parallaxFar = 0;
int parallaxMid = 0;
int parallaxNear = 0;
static float parallaxAttitudeX = 0.0f;

static int stepParallaxToward(float target, int current, int minValue, int maxValue) {
    // Scenery lives on the 4px room grid. A >half-cell Schmitt band prevents
    // a target near the midpoint from ping-ponging between adjacent poses.
    if (target > (float)current + 2.25f) current += kRoomPX;
    else if (target < (float)current - 2.25f) current -= kRoomPX;
    if (current < minValue) current = minValue;
    if (current > maxValue) current = maxValue;
    return current;
}

static void calcParallax() {
    // Serial capture presets are authored evidence poses. Keep them
    // deterministic regardless of the saved operator setting or IMU presence.
    if (debugRoamingFrame.active && debugRoamingFrame.overrideParallax) {
        int requested = (debugRoamingFrame.parallaxFar / kRoomPX) * kRoomPX;
        parallaxFar = max(kParallaxFarMin, min(kParallaxFarMax, requested));
        parallaxMid = parallaxFar;
        parallaxNear = max(kParallaxNearMin,
                           min(currentRoom == 3 ? kRooftopNearMax : kParallaxNearMax,
                               parallaxFar * 2));
        return;
    }

    if (!Config::getRoomParallaxEnabled() || !Pedometer::hasIMU()) {
        // Hold every authored plane at its neutral pose when motion is disabled
        // or no physical IMU exists. A detected IMU stays available to steps,
        // gestures and RF bearing; the setting only stops cosmetic room motion
        // and its parallax-keyed base-plate rebuilds.
        parallaxFar = 0;
        parallaxMid = 0;
        parallaxNear = 0;
        parallaxAttitudeX = 0.0f;
        return;
    }

    float ax, ay, az;
    Pedometer::getCachedAccel(ax, ay, az);
    (void)ax;
    (void)az;
    float tilt = ay;
    constexpr float kDeadzone = 0.055f;
    if (fabsf(tilt) <= kDeadzone) tilt = 0.0f;
    else tilt += tilt > 0.0f ? -kDeadzone : kDeadzone;
    tilt /= 0.62f;
    if (tilt < -1.0f) tilt = -1.0f;
    if (tilt > 1.0f) tilt = 1.0f;

    // Pancetta is an actor, not a camera. Pig-relative cues belong only to
    // source-local effects; shared scenery planes follow the IMU exclusively.
    float cameraTarget = -tilt;
    if (cameraTarget < -1.0f) cameraTarget = -1.0f;
    if (cameraTarget > 1.0f) cameraTarget = 1.0f;
    parallaxAttitudeX += (cameraTarget - parallaxAttitudeX) * 0.115f;
    if (fabsf(parallaxAttitudeX) < 0.018f) parallaxAttitudeX = 0.0f;

    const int nearMax = currentRoom == 3 ? kRooftopNearMax : kParallaxNearMax;
    parallaxFar = stepParallaxToward(parallaxAttitudeX * 4.0f, parallaxFar,
                                     kParallaxFarMin, kParallaxFarMax);
    parallaxMid = stepParallaxToward(parallaxAttitudeX * 6.0f, parallaxMid,
                                     kParallaxMidMin, kParallaxMidMax);
    parallaxNear = stepParallaxToward(parallaxAttitudeX * 8.0f, parallaxNear,
                                      kParallaxNearMin, nearMax);
}

// ==[ ROOM MOOD UPDATE ]== (struct + instance declared earlier near Station enum)
static void updateRoomMood() {
    roomMood.alertLevel = 0;
    roomMood.rfActivity = 0;
    roomMood.rfFruitCount = 0;
    roomMood.trackerPresent = false;
    roomMood.spamActive = false;
    roomMood.captureCount = 0;
    roomMood.huntWasActive = false;

    // Captures are durable Hunt evidence, not IPP telemetry. Keep their room
    // marks truthful even when background defense is switched off.
    uint32_t captureTotal = Capture::getPMKIDCount() + Capture::getHandshakeCount();
    roomMood.captureCount = (uint8_t)(captureTotal > 255u ? 255u : captureTotal);
    roomMood.huntWasActive = captureTotal > 0;

    if (!Config::getIppEnabled()) return;

    if (DefensePipeline::snapshot().hasActiveAttacker() || DefensePipeline::snapshot().isDualBandStalkActive()) roomMood.alertLevel = 4;
    else if (DefensePipeline::snapshot().hasThreats()) roomMood.alertLevel = 3;
    else if (DefensePipeline::snapshot().isScanning()) roomMood.alertLevel = 1;
    else if (DefensePipeline::snapshot().getTrackerCount() > 0) roomMood.alertLevel = 2;

    roomMood.trackerPresent = (DefensePipeline::snapshot().getFollowingCount() > 0);
    roomMood.spamActive = (DefensePipeline::snapshot().getSpamCount() > 0);
    // Main-loop RF context for slow environmental reactions. This is a
    // bounded scene signal, not callback traffic and not a security metric.
    uint32_t rf = DefensePipeline::snapshot().getLastDeauthPPS();
    int apCount = DefensePipeline::snapshot().getLastWifiAPCount();
    int trackerCount = DefensePipeline::snapshot().getTrackerCount();
    if (apCount > 32) apCount = 32;
    if (trackerCount > 16) trackerCount = 16;
    rf += (uint32_t)apCount * 4u;
    rf += (uint32_t)trackerCount * 6u;
    rf += (uint32_t)roomMood.alertLevel * 24u;
    if (roomMood.spamActive) rf += 24u;
    roomMood.rfActivity = (uint8_t)(rf > 255u ? 255u : rf);

    // Hunt's tree fruits mean viable air. Rooms use the freshest scan ledger:
    // one fruit for any AP set, another per eight APs, plus active tracker evidence.
    uint8_t fruits = apCount > 0 ? (uint8_t)(1 + (apCount - 1) / 8) : 0;
    if (roomMood.trackerPresent || roomMood.spamActive) fruits++;
    roomMood.rfFruitCount = fruits > 5u ? 5u : fruits;
}

// ==[ NEON PULSE ]== breathing brightness for neon signs (theme-tinted)
uint16_t neonPulse(uint32_t now) {
    float t = 0.55f + 0.15f * fastSinf(now * 0.003f);
    return Display::lerpColor565(RP::BG, RP::NEON, t / 0.65f);  // scale to match RP::NEON's dim
}

void drawEmitterFog(M5Canvas& canvas, uint32_t now,
                    int sourceX, int sourceY, int reflectY,
                    uint16_t tint, bool sourceActive) {
    if (!sourceActive || tint == 0) return;
    for (int i = 0; i < 5; i++) {
        float phase = (float)((now + (uint32_t)i * 700) % 2400) / 2400.0f;
        int fx = sourceX + (int)(fastSinf(phase * 6.28318f + (float)i * 1.4f) * 12.0f);
        int fy = sourceY + (int)(phase * (float)(reflectY - sourceY));
        float fade = 1.0f - phase * 0.85f;
        if (fade < 0.12f) continue;
        int sx = fx & ~3, sy = fy & ~3;
        if (sx < 0 || sx >= SCREEN_WIDTH || sy < kRoomY || sy >= kFloorY + 8) continue;
        uint16_t base = fastReadPx(canvas, sx, sy);
        canvas.fillRect(sx, sy, kRoomPX, kRoomPX,
                        screenBlend565(base, tint, (uint8_t)(fade * 75.0f)));
    }
}

// ==[ BACKGROUND TEXTURES ]== (moved to menu_pig_render.cpp)


static ColorEventSample sampleColorEvent(uint32_t now) {
    ColorEventSample ev;

    if (Weather::isThunderFlashing()) {
        ev.active = true;
        ev.color565 = RP::FLUOR;
        ev.intensity = 168;
        return ev;
    }

    // Teleport particle glow reflection (replaces old portal overlay reflection)
    if (Teleport::isActive()) {
        ev.active = true;
        ev.color565 = Avatar::getHypeColor(120, 67);
        ev.intensity = 180;
        return ev;
    }

    if (Avatar::isHypeVisualActive()) {
        int16_t hx = (int16_t)(120 + (((int32_t)(now / 70) % 17) - 8) * 2);
        int16_t hy = (int16_t)(67 + (((int32_t)(now / 95) % 9) - 4) * 2);
        ev.active = true;
        ev.color565 = Avatar::getHypeColor(hx, hy);
        ev.intensity = 112;
    }

    return ev;
}

// ==[ HELPER SCENE FURNITURE ]==

// "HACK" neon sign — text rendered as fat pixel blocks (4px grid)
static void drawAlleyNeon(M5Canvas& canvas, uint16_t fg, uint16_t bg, uint32_t now) {
    if (!isNeonOn(now)) return;
    int nx = kNeonX, ny = kNeonY;
    // Sign border
    canvas.fillRect(nx, ny, kNeonW, kNeonH, fg);
    canvas.fillRect(nx + 4, ny + 2, kNeonW - 8, kNeonH - 4, bg);
    // HACK glyphs: 4 letters at 4px grid, 8px tall (2 rows), stride 12px
    int ly0 = ny + 2, ly1 = ny + 6;  // row 0 and row 1 (centered in 12px sign)
    // H: |=|  (pillars + crossbar)
    canvas.fillRect(nx + 4,  ly0, 4, 8, fg);    // left pillar
    canvas.fillRect(nx + 12, ly0, 4, 8, fg);    // right pillar
    canvas.fillRect(nx + 8,  ly0, 4, 4, fg);    // crossbar (top half)
    // A: === / | |  (top bar + legs)
    canvas.fillRect(nx + 16, ly0, 12, 4, fg);   // top bar
    canvas.fillRect(nx + 16, ly1, 4,  4, fg);   // left leg
    canvas.fillRect(nx + 24, ly1, 4,  4, fg);   // right leg
    // C: === / |    (top bar + bottom-left)
    canvas.fillRect(nx + 28, ly0, 12, 4, fg);   // top bar
    canvas.fillRect(nx + 28, ly1, 4,  4, fg);   // left wall
    canvas.fillRect(nx + 32, ly1, 8,  4, fg);   // bottom bar
    // K: || / |`  (wall + arms)
    canvas.fillRect(nx + 40, ly0, 4, 8, fg);    // left wall
    canvas.fillRect(nx + 44, ly0, 4, 4, fg);    // upper arm
    canvas.fillRect(nx + 44, ly1, 4, 4, fg);    // lower arm
}

// Pipe drip animation — drop falls with gravity, splash at floor
static void drawAlleyDrip(M5Canvas& canvas, uint16_t dropColor,
                          uint16_t splashColor, uint32_t now) {
    uint32_t t = (now - dripCycleStart) % DRIP_CYCLE_MS;
    int pipeBotY = kPipeY + 4;
    int fallH = kFloorY - pipeBotY;
    int dropX = kPipeX + 20;
    float tNorm = (float)t / (float)DRIP_CYCLE_MS;

    if (tNorm < 0.7f) {
        // Drop falling with gravity acceleration
        float ft = tNorm / 0.7f;
        int dy = (int)(ft * ft * (float)fallH);
        int dropY = pipeBotY + dy;
        if (dropY < kFloorY - 2)
            canvas.fillRect(dropX, dropY, 4, 4, dropColor);
    } else if (tNorm < 0.8f) {
        // Splash at floor — 3 blocks spread
        float splashT = (tNorm - 0.7f) / 0.1f;
        uint8_t alpha = (uint8_t)(96.0f - splashT * 88.0f);
        int splashX = dropX & ~(kRoomPX - 1);
        int splashY = (kFloorY - kRoomPX) & ~(kRoomPX - 1);
        for (int dx = -kRoomPX; dx <= kRoomPX; dx += kRoomPX) {
            uint16_t base = fastReadPx(canvas, splashX + dx, splashY);
            canvas.fillRect(splashX + dx, splashY, kRoomPX, kRoomPX,
                            screenBlend565(base, splashColor, alpha));
        }
        // Trigger puddle ripple on splash start
        if (rippleRadius == 0 && tNorm < 0.72f) {
            rippleStart = now;
            rippleRadius = 1;
        }
    }
    // else: reforming at pipe (invisible)
}

// Sparking wire — diagonal wire with occasional 2-frame spark
static void drawAlleySpark(M5Canvas& canvas, uint16_t fg, uint32_t now) {
    int wx = kSparkX, wy = kSparkY;
    // Diagonal wire
    for (int i = 0; i < 20; i += 4) {
        canvas.fillRect(wx + i / 3, wy + i, 4, 4, fg);
    }
    // Spark burst if active
    if (sparkActive) {
        uint32_t elapsed = now - sparkStart;
        int sx = wx + 4, sy = wy + 12;
        if (elapsed < SPARK_FRAME_MS) {
            // Frame 1: cross shape
            canvas.fillRect(sx, sy - 2, 4, 8, fg);
            canvas.fillRect(sx - 2, sy, 8, 4, fg);
        } else if (elapsed < SPARK_FRAME_MS * 2) {
            // Frame 2: diagonal burst
            canvas.fillRect(sx - 2, sy - 2, 4, 4, fg);
            canvas.fillRect(sx + 2, sy - 2, 4, 4, fg);
            canvas.fillRect(sx - 2, sy + 2, 4, 4, fg);
            canvas.fillRect(sx + 2, sy + 2, 4, 4, fg);
        } else {
            sparkActive = false;
        }
    }
}







// ==[ ROOM 1: NOIR APARTMENT FURNITURE ]==

void updateNoirVolumetricPass(uint32_t now) {
    float drift = pigX - noirPass.lastPigX;
    noirPass.lastPigX = pigX;

    if (noirPass.nextTriggerMs == 0) {
        noirPass.nextTriggerMs = now + randomRange(6000, 12000);
    }

    if (noirPass.active) {
        if (now - noirPass.startMs >= noirPass.durationMs) {
            noirPass.active = false;
            noirPass.nextTriggerMs = now + randomRange(6000, 12000);
        }
        return;
    }

    if ((int32_t)(now - noirPass.nextTriggerMs) < 0) return;

    noirPass.active = true;
    noirPass.startMs = now;
    noirPass.durationMs = 1100;
    if (fabsf(drift) > 0.25f) {
        noirPass.dir = (drift > 0.0f) ? -1 : 1;  // oppose pig/parallax drift
    } else {
        noirPass.dir = (esp_random() & 1u) ? 1 : -1;
    }
}

void drawNoirVolumetricPass(M5Canvas& canvas, uint32_t now, int wx, int wy, int ww, int wh) {
    if (!noirPass.active) return;

    float t = clamp01f((float)(now - noirPass.startMs) / (float)noirPass.durationMs);
    float sweep = smootherstep(t);
    float pulse = fastSinf(t * PI);
    if (pulse <= 0.0f) return;

    uint16_t passColor = RP::SHAFT;
    uint8_t tintAmt = 0;
    if (colorEvent.active) {
        tintAmt = (uint8_t)(colorEvent.intensity / 2u);
        passColor = mixColor565(RP::SHAFT, colorEvent.color565, tintAmt);
    }

    float topSweep = (noirPass.dir > 0) ? sweep : (1.0f - sweep);
    float topCenter = (float)(wx + 4) + topSweep * (float)(ww - 8);
    float bottomCenter = topCenter - (float)noirPass.dir * 22.0f;
    int topY = wy + wh - 2;
    int bottomY = kFloorY;
    if (bottomY <= topY) return;

    uint8_t baseDensity = (uint8_t)(22.0f + pulse * 44.0f + (float)tintAmt * 0.15f);
    // Event identity owns the sparse mask. Motion comes from the beam geometry
    // and pulse above; rerolling coverage each frame read as TV static.
    uint32_t seed = 0x6B17u ^ noirPass.startMs;
    for (int py = topY; py <= bottomY; py += kRoomPX) {
        float depth = (float)(py - topY) / (float)(bottomY - topY);
        float cx = topCenter + (bottomCenter - topCenter) * depth;
        float halfW = 4.0f + depth * 18.0f;
        int x0 = ((int)floorf(cx - halfW)) & ~3;
        int x1 = ((int)ceilf(cx + halfW)) & ~3;
        for (int px = x0; px <= x1; px += kRoomPX) {
            if (px < 4 || px > SCREEN_WIDTH - 4) continue;
            float edge = fabsf(((float)px + 2.0f - cx) / (halfW + 0.01f));
            if (edge > 1.0f) continue;
            float atten = (1.0f - edge) * (1.0f - depth * 0.72f);
            uint8_t d = (uint8_t)((float)baseDensity * atten);
            if ((wallHash(px, py, seed) & 0xFFu) < d) {
                uint16_t base = fastReadPx(canvas, px, py);
                canvas.fillRect(px, py, kRoomPX, kRoomPX,
                                screenBlend565f(base, passColor, 0.35f + atten * 0.25f));
            }
        }
    }
}

void shadeRoomShadowPx(M5Canvas& canvas, int x, int y, float factor,
                              uint16_t tint, uint8_t tintAmt) {
    x &= ~3;
    y &= ~3;
    if (x < 0 || x >= SCREEN_WIDTH || y < kRoomY || y >= kFloorY + 8) return;
    uint16_t base = fastReadPx(canvas, x, y);
    uint16_t shaded = darken565(base, clamp01f(factor));
    if (tintAmt > 0) shaded = mixColor565(shaded, tint, tintAmt);
    // brightness floor — never crush surfaces into near-BG range
    if (isNearBG(shaded) && !isNearBG(base)) return;
    canvas.fillRect(x, y, kRoomPX, kRoomPX, shaded);
}

void lightRoomBeamPx(M5Canvas& canvas, int x, int y, uint16_t light, float strength) {
    x &= ~3;
    y &= ~3;
    if (x < 0 || x >= SCREEN_WIDTH || y < kRoomY || y >= kFloorY + 8) return;
    uint16_t base = fastReadPx(canvas, x, y);
    canvas.fillRect(x, y, kRoomPX, kRoomPX, screenBlend565f(base, light, clamp01f(strength)));
}

void drawNoirBlindShadowSweep(M5Canvas& canvas, uint32_t now,
                                     int wx, int wy, int ww, int wh) {
    if (Weather::isThunderFlashing()) return;

    float sweepT;
    float pulse;
    int dir;
    if (noirPass.active) {
        float t = clamp01f((float)(now - noirPass.startMs) / (float)noirPass.durationMs);
        sweepT = smootherstep(t);
        pulse = max(0.0f, fastSinf(t * PI));
        dir = noirPass.dir;
    } else {
        sweepT = 0.5f + fastSinf(now * 0.00031f + 0.8f) * 0.5f;
        pulse = 0.30f + (0.5f + fastSinf(now * 0.00053f + 1.7f) * 0.5f) * 0.22f;
        dir = (fastSinf(now * 0.00019f + 0.4f) >= 0.0f) ? 1 : -1;
    }

    int topY = (wy + wh + 4) & ~3;
    int bottomY = min(kFloorY + 4, topY + 72);
    if (bottomY <= topY) return;

    float topSweep = (dir > 0) ? sweepT : (1.0f - sweepT);
    float topCenter = (float)(wx + 8) + topSweep * (float)max(8, ww - 16);
    float bottomCenter = topCenter - (float)dir * (noirPass.active ? 28.0f : 14.0f);
    int bandCount = 6;
    float baseStrength = 0.10f + pulse * (noirPass.active ? 0.20f : 0.10f);
    uint16_t lightCol = RP::SHAFT;
    if (colorEvent.active) {
        lightCol = mixColor565(RP::SHAFT, colorEvent.color565, (uint8_t)(colorEvent.intensity / 4u));
    }
    uint16_t tintCol = colorEvent.active
                           ? mixColor565(RP::SHADOW_C, colorEvent.color565, (uint8_t)(colorEvent.intensity / 7u))
                           : RP::SHADOW_C;
    uint8_t tintAmt = colorEvent.active ? (uint8_t)(colorEvent.intensity / 10u + 6u) : 0u;

    for (int py = topY; py < bottomY; py += kRoomPX) {
        float depth = clamp01f((float)(py - topY) / (float)max(1, bottomY - topY));
        float cx = topCenter + (bottomCenter - topCenter) * depth;
        float halfW = 24.0f + depth * 26.0f;
        int x0 = ((int)floorf(cx - halfW)) & ~3;
        int x1 = ((int)ceilf(cx + halfW)) & ~3;
        for (int px = x0; px <= x1; px += kRoomPX) {
            if (px < 4 || px >= SCREEN_WIDTH - 4) continue;
            float edge = fabsf(((float)px + 2.0f - cx) / (halfW + 0.01f));
            if (edge >= 1.0f) continue;
            float beam = (1.0f - edge * edge) * (1.0f - depth * 0.16f);
            uint8_t keep = (uint8_t)(80.0f + beam * 120.0f);
            if ((wallHash(px, py, 0x6C01u) & 0xFFu) >= keep) continue;
            float strength = (0.12f + pulse * (noirPass.active ? 0.25f : 0.14f)) * beam;
            if (strength > 0.015f) {
                lightRoomBeamPx(canvas, px, py, lightCol, strength);
            }
        }
    }

    for (int band = 0; band < bandCount; band++) {
        int bandTop = topY + 4 + band * 10;
        int bandBottom = min(bottomY, bandTop + 4);
        float bandBias = ((float)band - (float)(bandCount - 1) * 0.5f) * (float)dir;
        for (int py = bandTop; py < bandBottom; py += kRoomPX) {
            float depth = clamp01f((float)(py - topY) / (float)max(1, bottomY - topY));
            float lightCx = topCenter + (bottomCenter - topCenter) * depth;
            float lightHalfW = 24.0f + depth * 26.0f;
            float cx = lightCx + bandBias * (2.0f + depth * 5.0f);
            float halfW = 18.0f + depth * 22.0f;
            int x0 = ((int)floorf(cx - halfW)) & ~3;
            int x1 = ((int)ceilf(cx + halfW)) & ~3;
            for (int px = x0; px <= x1; px += kRoomPX) {
                if (px < 4 || px >= SCREEN_WIDTH - 4) continue;
                float pxCenter = (float)px + 2.0f;
                if (pxCenter < lightCx - lightHalfW || pxCenter > lightCx + lightHalfW) continue;
                float edge = fabsf((pxCenter - cx) / (halfW + 0.01f));
                if (edge >= 1.0f) continue;
                float stripe = 1.0f - edge * edge;
                uint32_t h = wallHash(px, py, 0x6D11u + (uint32_t)band * 171u);
                if ((h & 0xFFu) > (uint8_t)(180.0f + stripe * 60.0f)) continue;
                float shade = baseStrength * stripe * (1.0f - depth * 0.18f);
                if (shade > 0.02f) {
                    shadeRoomShadowPx(canvas, px, py, shade, tintCol, tintAmt);
                }
            }
        }
    }
}

static RoomWindowBackdropParams noirWindowBackdropParams() {
    RoomWindowBackdropParams backdrop = {};
    backdrop.style = RoomWindowBackdropStyle::NoirApartment;
    backdrop.thunder = Weather::isThunderFlashing();
    backdrop.tintActive = colorEvent.active;
    backdrop.tintColor565 = colorEvent.color565;
    backdrop.tintIntensity = colorEvent.intensity;
    backdrop.parallaxX = (int8_t)parallaxFar;
    return backdrop;
}

void drawNoirWindowBase(M5Canvas& canvas,
                        int wx, int wy, int ww, int wh,
                        float pigPosX) {
    (void)pigPosX;
    RoomWindowBackdropParams backdrop = noirWindowBackdropParams();
    backdrop.thunder = false;
    backdrop.tintActive = false;
    drawRoomWindowBackdropBase(canvas, wx, wy, ww, wh, backdrop);

    canvas.fillRect(wx, wy, ww, 4, RP::STRUCT);
    canvas.fillRect(wx, wy + wh - 4, ww, 4, RP::STRUCT);
    canvas.fillRect(wx, wy, 4, wh, RP::STRUCT);
    canvas.fillRect(wx + ww - 4, wy, 4, wh, RP::STRUCT);
    canvas.fillRect(wx - 4, wy + wh, ww + 8, 4, RP::STRUCT);
    canvas.fillRect(wx + 4, wy + wh, 4, 4, RP::FILL);
    canvas.fillRect(wx, wy, 4, 4, RP::FILL);

    const int ix = wx + 4;
    const int iy = wy + 4;
    const int iw = ww - 8;
    const int ih = wh - 8;
    const int slatH = 4;
    int slatGap = (ih - 6 * slatH) / 7;
    if (slatGap < 4) slatGap = 4;
    for (int s = 0; s < 6; s++) {
        int sy = iy + slatGap + s * (slatH + slatGap);
        canvas.fillRect(ix, sy, iw, slatH, RP::STRUCT);
    }
}

// Noir window — venetian blind slats, "BAR" neon outside, weather through slats, light bars
// Uses RP internally: STRUCT frame/slats, NEON pink BAR text, FLUOR rain/stars/thunder, SHAFT light bars, PUDDLE reflection
void drawNoirWindowMotion(M5Canvas& canvas, uint32_t now,
                          int wx, int wy, int ww, int wh,
                          float pigPosX, uint16_t reflectionTint) {
    (void)pigPosX;
    static constexpr uint32_t kNoirWindowCondensationSalt = 0xA201u;
    static constexpr uint8_t kNoirWindowRainIntensity = 76;

    RoomWindowBackdropParams backdrop = noirWindowBackdropParams();
    drawRoomWindowBackdropMotion(canvas, now, wx, wy, ww, wh, backdrop);

    // Window frame (4px grid)
    canvas.fillRect(wx, wy, ww, 4, RP::STRUCT);
    canvas.fillRect(wx, wy + wh - 4, ww, 4, RP::STRUCT);
    canvas.fillRect(wx, wy, 4, wh, RP::STRUCT);
    canvas.fillRect(wx + ww - 4, wy, 4, wh, RP::STRUCT);
    // Sill (slightly warped — paint chipped)
    canvas.fillRect(wx - 4, wy + wh, ww + 8, 4, RP::STRUCT);
    canvas.fillRect(wx + 4, wy + wh, 4, 4, RP::FILL);
    // Frame corner chip
    canvas.fillRect(wx, wy, 4, 4, RP::FILL);

    int ix = wx + 4, iy = wy + 4, iw = ww - 8, ih = wh - 8;

    // Venetian blind slats — 6 horizontal bars, 4px grid
    int slatH = 4;
    int slatGap = (ih - 6 * slatH) / 7;  // space between slats
    if (slatGap < 4) slatGap = 4;

    // noir world: always raining. thunder overrides. BAR neon always outside.
    if (backdrop.thunder) {
        // Thunder flash: white fill between slats
        for (int s = 0; s < 6; s++) {
            int sy = iy + slatGap + s * (slatH + slatGap);
            if (s == 0)
                canvas.fillRect(ix, iy, iw, slatGap, RP::FLUOR);
            int gapY = sy + slatH;
            int gapH = (s < 5) ? slatGap : (iy + ih - gapY);
            if (gapH > 0)
                canvas.fillRect(ix, gapY, iw, gapH, RP::FLUOR);
        }
    } else {
        // "BAR" neon sign visible outside through rain-streaked glass
        if (isNeonOn(now)) {
            // Same bounded IMU plane as the exterior plate. Pig position is
            // not a camera, and using it here let the sign outrun the glass.
            int parallaxOff = parallaxFar;
            int bx = ix + iw / 2 - 12 + parallaxOff, by = (iy + ih / 2 - 4) & ~3;
            // B (4px grid)
            canvas.fillRect(bx, by, 4, 8, RP::NEON);
            canvas.fillRect(bx + 4, by, 4, 4, RP::NEON);
            canvas.fillRect(bx + 4, by + 4, 4, 4, RP::NEON);
            // A
            canvas.fillRect(bx + 8, by, 8, 4, RP::NEON);
            canvas.fillRect(bx + 8, by + 4, 8, 4, RP::NEON);
            canvas.fillRect(bx + 8, by + 4, 4, 4, RP::NEON);
            canvas.fillRect(bx + 12, by + 4, 4, 4, RP::NEON);
            // R
            canvas.fillRect(bx + 20, by, 4, 8, RP::NEON);
            canvas.fillRect(bx + 24, by, 4, 4, RP::NEON);
            canvas.fillRect(bx + 24, by + 4, 4, 4, RP::NEON);
        }
    }

    // Fixed bead topology + time-based creep. Draw before blind hardware so
    // the slats and seals occlude water like real foreground structure.
    uint16_t glassTint = reflectionTint != 0 ? reflectionTint : RP::NEON;
    MenuPigRender::drawWindowGlassRain(canvas, now, wx, wy, ww, wh,
                                       glassTint, kNoirWindowRainIntensity);
    MenuPigRender::drawCondensation(canvas, now, ix, iy, iw, ih,
                                    3, RP::SHAFT, kNoirWindowCondensationSalt);

    // Draw venetian blind slats (always on top)
    for (int s = 0; s < 6; s++) {
        int sy = iy + slatGap + s * (slatH + slatGap);
        canvas.fillRect(ix, sy, iw, slatH, RP::STRUCT);
    }

    // Intermittent noir volumetric pass, sweeping opposite pig drift.
    drawNoirVolumetricPass(canvas, now, wx, wy, ww, wh);

    if (colorEvent.active) {
        uint16_t paneTint = mixColor565(RP::FLUOR, colorEvent.color565, (uint8_t)(colorEvent.intensity / 2u));
        uint8_t paneDensity = (uint8_t)(colorEvent.intensity / 3u + 18u);
        drawLightPool(canvas, paneTint, ix, iy, 4, ih, paneDensity, 0x7131u);
        drawLightPool(canvas, paneTint, wx + ww / 2 - 4, iy, 4, ih, (uint8_t)(paneDensity - 6u), 0x7151u);
    }

    // Light bars projected on floor below window (yellow-green diagonal stripes)
    int lightY = kFloorY - 4;
    uint16_t floorBarColor = RP::SHAFT;
    if (colorEvent.active) {
        floorBarColor = mixColor565(RP::SHAFT, colorEvent.color565, (uint8_t)(colorEvent.intensity / 3u));
    }
    for (int lx = wx - 4; lx < wx + ww + 4; lx += 8) {
        canvas.fillRect(lx, lightY, 4, 4, floorBarColor);
        if (lx + 4 < wx + ww + 4)
            canvas.fillRect(lx + 4, lightY + 4, 4, 4, floorBarColor);
    }

    // Wet breakup stays fixed while its color follows the sampled exterior.
    if (isNeonOn(now)) {
        uint16_t wetReflection = Display::lerpColor565(RP::PUDDLE,
                                                       glassTint, 0.35f);
        int puddleY = kFloorY;
        for (int px = wx - 4; px < wx + ww + 4; px += kRoomPX) {
            float dist = fabsf((float)px - (float)(wx + ww/2)) / (float)(ww/2 + 4);
            uint8_t d = (uint8_t)(45.0f * (1.0f - dist * dist));
            if ((wallHash(px, puddleY + 100, 77777) & 0xFF) < d)
                canvas.fillRect(px, puddleY, kRoomPX, kRoomPX, wetReflection);
        }
    }

    if (colorEvent.active) {
        uint16_t wetTint = mixColor565(RP::PUDDLE, colorEvent.color565, (uint8_t)(colorEvent.intensity / 2u));
        drawLightPoolGradient(canvas, wetTint, wx - 4, kFloorY, ww + 8, 4,
                              (uint8_t)(colorEvent.intensity / 3u + 18u), 0x7191u);
    }

}

void drawNoirWindow(M5Canvas& canvas, uint32_t now,
                    int wx, int wy, int ww, int wh,
                    float pigPosX, uint16_t reflectionTint) {
    drawNoirWindowBase(canvas, wx, wy, ww, wh, pigPosX);
    drawNoirWindowMotion(canvas, now, wx, wy, ww, wh,
                         pigPosX, reflectionTint);
}



// ==[ CITY WINDOW PROJECTION ]== visible traffic owns every light + shadow

// Flying car state — persistent across frames
// Traffic state comes from ExteriorSprites::sampleTraffic(). The exact samples
// are reused below by visible bodies, light volumes, and projected shadows.

// Draw car light volumes on wall + floor BEFORE furniture
void drawCarLightVolumes(M5Canvas& canvas, uint32_t now,
                                 int wx, int wy, int ww, int wh) {
    int winBottom = wy + wh;
    ExteriorSprites::TrafficSample traffic[6];
    int count = ExteriorSprites::sampleTraffic(
        ExteriorSprites::Scene::Ramen, now, wx + 4, wy + 4, ww - 8, wh - 8,
        traffic, 6, (int8_t)parallaxFar);
    for (int i = 0; i < count; ++i) {
        const ExteriorSprites::TrafficSample& car = traffic[i];
        if (!car.visible || car.centerStrength < 24u) continue;
        int carScreenX = car.emitterX;
        uint8_t density = (uint8_t)min(34, 8 + car.centerStrength / 8);
        // Warm light wash on window glass (lower portion)
        int washX = (carScreenX - 6) & ~3;
        if (washX < wx) washX = wx;
        int washY = (winBottom - 18) & ~3;
        int washW = 10;
        int washH = 14;
        uint16_t light = car.color == 0u ? RP::NEON :
                         (car.color == 1u ? RP::CRT :
                          (car.color == 2u ? RP::WARM : RP::VEND));
        drawLightPoolGradient(canvas, light, washX, washY, washW, washH,
                              density, 90090 + i * 111);
    }
}

void drawCityWindowShadowSweep(M5Canvas& canvas, uint32_t now,
                                      int wx, int wy, int ww, int wh) {
    if (Weather::isThunderFlashing()) return;

    int topY = (wy + 4) & ~3;
    int bottomY = (wy + wh - 4) & ~3;
    if (bottomY <= topY) return;

    const int barX[3] = {
        (wx + 2) & ~3,
        (wx + ww / 2) & ~3,
        (wx + ww - 6) & ~3
    };
    const float barWeight[3] = {0.55f, 1.0f, 0.55f};
    uint16_t tintCol = colorEvent.active
                           ? mixColor565(RP::SHADOW_C, colorEvent.color565, (uint8_t)(colorEvent.intensity / 8u))
                           : RP::SHADOW_C;
    uint8_t tintAmt = colorEvent.active ? (uint8_t)(colorEvent.intensity / 12u + 4u) : 0u;

    ExteriorSprites::TrafficSample traffic[6];
    int trafficCount = ExteriorSprites::sampleTraffic(
        ExteriorSprites::Scene::Ramen, now, wx + 4, wy + 4, ww - 8, wh - 8,
        traffic, 6, (int8_t)parallaxFar);
    for (int i = 0; i < trafficCount; ++i) {
        const ExteriorSprites::TrafficSample& car = traffic[i];
        if (!car.visible || car.centerStrength < 24u) continue;

        int sourceX = car.emitterX;
        float center = (float)car.centerStrength / 255.0f;
        float sourceStrength = 0.07f + center * 0.14f;
        uint16_t lightCol = car.color == 0u ? RP::NEON :
                            (car.color == 1u ? RP::CRT :
                             (car.color == 2u ? RP::WARM : RP::VEND));
        if (colorEvent.active) {
            lightCol = mixColor565(lightCol, colorEvent.color565, (uint8_t)(colorEvent.intensity / 5u));
        }
        // Light beams extend below window to floor — dramatic headlight wash
        int winSpanBeam = max(1, bottomY - topY);
        for (int py = topY; py < kFloorY; py += kRoomPX) {
            float depth = (float)(py - topY) / (float)winSpanBeam;
            float beamCx = (float)sourceX + (float)car.dirX * (2.0f + depth * 6.0f);
            float halfW = 16.0f + depth * 24.0f;
            int x0 = ((int)floorf(beamCx - halfW)) & ~3;
            int x1 = ((int)ceilf(beamCx + halfW)) & ~3;
            for (int px = x0; px <= x1; px += kRoomPX) {
                if (px < 4 || px >= SCREEN_WIDTH - 4) continue;
                float edge = fabsf(((float)px + 2.0f - beamCx) / (halfW + 0.01f));
                if (edge >= 1.0f) continue;
                float beam = (1.0f - edge * edge) * (1.0f - depth * 0.14f);
                uint8_t keep = (uint8_t)(68.0f + beam * 100.0f);
                if ((wallHash(px, py, 0x8B21u + (uint32_t)i * 211u) & 0xFFu) >= keep) continue;
                float strength = (0.10f + center * 0.30f) * beam;
                // below window: only light furniture/floor, fade with distance
                if (py >= bottomY) {
                    uint16_t base = fastReadPx(canvas, px, py);
                    if (isNearBG(base)) continue;
                    float falloff = 1.0f - (depth - 1.0f) * 0.5f;
                    if (falloff <= 0.0f) continue;
                    strength *= falloff;
                }
                if (strength > 0.014f) {
                    lightRoomBeamPx(canvas, px, py, lightCol, strength);
                }
            }
        }
        for (int bi = 0; bi < 3; bi++) {
            float spread = (float)(barX[bi] - sourceX);
            int winSpan = max(1, bottomY - topY);
            // extend shadow bars below window to floor — wraps around furniture geometry
            for (int py = topY; py < kFloorY; py += kRoomPX) {
                float depth = (float)(py - topY) / (float)winSpan;
                float proj = spread * (0.24f + depth * 0.72f);
                float shadowCx = (float)barX[bi] + proj;
                float halfW = 3.5f + depth * (6.0f + fabsf(spread) * 0.035f);
                int x0 = ((int)floorf(shadowCx - halfW)) & ~3;
                int x1 = ((int)ceilf(shadowCx + halfW)) & ~3;
                for (int px = x0; px <= x1; px += kRoomPX) {
                    if (px < 4 || px >= SCREEN_WIDTH - 4) continue;
                    float edge = fabsf(((float)px + 2.0f - shadowCx) / (halfW + 0.01f));
                    if (edge >= 1.0f) continue;
                    float stripe = 1.0f - edge * edge;
                    uint32_t h = wallHash(px, py, 0x8C31u + (uint32_t)bi * 97u + (uint32_t)i * 311u);
                    if ((h & 0xFFu) > (uint8_t)(168.0f + stripe * 70.0f)) continue;
                    float shade = sourceStrength * barWeight[bi] * stripe * (1.0f - depth * 0.12f);
                    // below window: only shade furniture/floor surfaces, fade with distance
                    if (py >= bottomY) {
                        uint16_t base = fastReadPx(canvas, px, py);
                        if (isNearBG(base)) continue;
                        float falloff = 1.0f - (depth - 1.0f) * 0.5f;
                        if (falloff <= 0.0f) continue;
                        shade *= falloff;
                    }
                    if (shade > 0.018f) {
                        shadeRoomShadowPx(canvas, px, py, shade, tintCol, tintAmt);
                    }
                }
            }
        }
    }
}

// ==[ CAR SHADOW ON RECT ]== project window shadow bars onto arbitrary rect (pig, furniture)
// Single center bar per car, no contour map, kPigPX grid.
void applyCarShadowToRect(M5Canvas& canvas, uint32_t now,
                          int wx, int wy, int ww, int wh,
                          int rx, int ry, int rw, int rh, int pixSize) {
    if (Weather::isThunderFlashing()) return;

    int topY = (wy + 4) & ~3;
    int bottomY = (wy + wh - 4) & ~3;
    int winSpan = max(1, bottomY - topY);
    if (bottomY <= topY) return;

    int winCenterX = (wx + ww / 2) & ~3;  // single center bar

    int ry0 = ry & ~(kPigPX - 1);
    int ry1 = ry + rh;
    int rx0 = rx & ~(kPigPX - 1);
    int rx1 = rx + rw;

    ExteriorSprites::TrafficSample traffic[6];
    int trafficCount = ExteriorSprites::sampleTraffic(
        ExteriorSprites::Scene::Ramen, now, wx + 4, wy + 4, ww - 8, wh - 8,
        traffic, 6, (int8_t)parallaxFar);
    for (int i = 0; i < trafficCount; ++i) {
        const ExteriorSprites::TrafficSample& car = traffic[i];
        if (!car.visible || car.centerStrength < 24u) continue;

        int sourceX = car.emitterX;
        float center = (float)car.centerStrength / 255.0f;
        float sourceStrength = 0.07f + center * 0.14f;
        float spread = (float)(winCenterX - sourceX);

        for (int py = ry0; py < ry1; py += kPigPX) {
            float depth = (float)(py - topY) / (float)winSpan;
            if (depth <= 0.0f) continue;
            float proj = spread * (0.24f + depth * 0.72f);
            float shadowCx = (float)winCenterX + proj;
            float halfW = 3.5f + depth * 6.0f;

            for (int px = rx0; px < rx1; px += kPigPX) {
                float edge = fabsf(((float)px + 1.0f - shadowCx) / (halfW + 0.01f));
                if (edge >= 1.0f) continue;
                float stripe = 1.0f - edge * edge;
                float shade = sourceStrength * stripe * (1.0f - depth * 0.12f);
                if (depth > 1.0f) {
                    shade *= 1.0f - (depth - 1.0f) * 0.5f;
                }
                if (shade < 0.02f) continue;

                uint16_t base = fastReadPx(canvas, px, py);
                if (!isPigEffectPixel(px, py, base)) continue;
                uint16_t shaded = lerpColor565_8(base, RP::SHADOW_C, (uint8_t)(clamp01f(shade) * 255.0f));
                if (isNearBG(shaded)) continue;
                fastFillBlock2(canvas, px, py, shaded);
            }
        }
    }
}

static RoomWindowBackdropParams cityWindowBackdropParams() {
    RoomWindowBackdropParams backdrop = {};
    backdrop.style = RoomWindowBackdropStyle::RamenBar;
    backdrop.thunder = Weather::isThunderFlashing();
    backdrop.tintActive = colorEvent.active;
    backdrop.tintColor565 = colorEvent.color565;
    backdrop.tintIntensity = colorEvent.intensity;
    backdrop.parallaxX = (int8_t)parallaxFar;
    return backdrop;
}

static void drawCityWindowFrame(M5Canvas& canvas,
                                int wx, int wy, int ww, int wh) {
    canvas.fillRect(wx, wy, ww, kRoomPX, RP::STRUCT);
    canvas.fillRect(wx, wy + wh - kRoomPX, ww, kRoomPX, RP::STRUCT);
    canvas.fillRect(wx, wy, kRoomPX, wh, RP::STRUCT);
    canvas.fillRect(wx + ww - kRoomPX, wy, kRoomPX, wh, RP::STRUCT);
    canvas.fillRect(wx - kRoomPX, wy + wh, ww + kRoomPX * 2,
                    kRoomPX, RP::D_STRUCT);
    canvas.fillRect((wx + ww / 2) & ~3, wy, kRoomPX, wh, RP::STRUCT);
}

void drawCityWindowBase(M5Canvas& canvas,
                        int wx, int wy, int ww, int wh, float pigPosX) {
    (void)pigPosX;
    RoomWindowBackdropParams backdrop = cityWindowBackdropParams();
    backdrop.thunder = false;
    backdrop.tintActive = false;
    drawRoomWindowBackdropBase(canvas, wx, wy, ww, wh, backdrop);
    drawCityWindowFrame(canvas, wx, wy, ww, wh);
}

void drawCityWindowMotion(M5Canvas& canvas, uint32_t now,
                          int wx, int wy, int ww, int wh, float pigPosX,
                          uint16_t glassTint) {
    (void)pigPosX;
    static constexpr uint32_t kCityWindowCondensationSalt = 0xA301u;
    static constexpr uint8_t kCityWindowRainIntensity = 84;
    static constexpr int kCityWindowCondensationCount = 2;

    RoomWindowBackdropParams backdrop = cityWindowBackdropParams();
    drawRoomWindowBackdropMotion(canvas, now, wx, wy, ww, wh, backdrop);

    // Glass lies over the city plate but behind the structural frame/mullion.
    MenuPigRender::drawWindowGlassRain(canvas, now, wx, wy, ww, wh,
                                       glassTint, kCityWindowRainIntensity);
    MenuPigRender::drawCondensation(canvas, now,
                                    wx + kRoomPX, wy + kRoomPX,
                                    ww - kRoomPX * 2, wh - kRoomPX * 2,
                                    kCityWindowCondensationCount, glassTint,
                                    kCityWindowCondensationSalt);

    // One 4px-aligned frame. Pane water above is the single glass-rain pass.
    drawCityWindowFrame(canvas, wx, wy, ww, wh);
}

// Compact city window — generated plate, shared traffic, one weather pass.
void drawCityWindow(M5Canvas& canvas, uint32_t now,
                    int wx, int wy, int ww, int wh, float pigPosX,
                    uint16_t glassTint) {
    drawCityWindowBase(canvas, wx, wy, ww, wh, pigPosX);
    drawCityWindowMotion(canvas, now, wx, wy, ww, wh,
                         pigPosX, glassTint);
}

// ==[ ENVIRONMENTAL CLUTTER ]== (stateless props moved to menu_pig_render.cpp)

// Neon arrow sign (pointing down, flickering)
void drawNeonArrow(M5Canvas& canvas, uint32_t now, int x, int y) {
    if (!isNeonOn(now)) return;
    canvas.fillRect(x + 2, y, 4, 4, RP::NEON);
    canvas.fillRect(x, y + 4, 8, 4, RP::NEON);
    canvas.fillRect(x + 2, y + 6, 4, 4, RP::NEON);
}

// ==[ LIMB BUMP COLORS ]== highlight/shadow from room light direction
LimbBump computeLimbBump(uint16_t fg, uint16_t bg, int limbCX, int limbCY,
                                 PigLight light, float vol) {
    LimbBump b;
    // Limbs occupy only a few fat pixels. Strong per-limb modelling breaks
    // those pixels into apparent extra hands/feet, so keep their contrast below
    // the body's broad bump shade and let the shared silhouette do the work.
    float shStr = 0.14f * vol; if (shStr > 0.35f) shStr = 0.35f;
    b.shadow = Display::lerpColor565(fg, bg, shStr);
    if (light.tint == 0) { b.hiL = fg; b.hiR = fg; return b; }
    float dx = (float)(light.x - limbCX);
    float hiStr = 0.12f * vol; if (hiStr > 0.30f) hiStr = 0.30f;
    uint16_t hi = Display::screenBlend565(fg, light.tint, (uint8_t)(hiStr * 255.0f));
    if (dx < -4.0f)       { b.hiL = hi; b.hiR = fg; }      // lit side highlight, far side neutral
    else if (dx > 4.0f)   { b.hiR = hi; b.hiL = fg; }
    else                   { b.hiL = fg; b.hiR = fg; }  // directly above — neutral
    return b;
}

static void drawDirectionalLimbEdges(M5Canvas& canvas, uint16_t fg,
                                     int x, int y, int w, int h,
                                     const LimbBump& bump) {
    if (h <= 0) return;
    const bool litLeft = bump.hiL != fg;
    const bool litRight = bump.hiR != fg;
    if (litLeft) {
        canvas.fillRect(x, y, kPigPX, h, bump.hiL);
        canvas.fillRect(x + w - kPigPX, y, kPigPX, h, bump.shadow);
    } else if (litRight) {
        canvas.fillRect(x, y, kPigPX, h, bump.shadow);
        canvas.fillRect(x + w - kPigPX, y, kPigPX, h, bump.hiR);
    }
}

// Paints limb masks on the pig's 2x2 fat-pixel grid.
void drawMaskRows(M5Canvas& canvas, int x, int y,
                  const char* const* rows, int rowCount, int rowWidth,
                  char token, uint16_t color, bool mirrorX) {
    for (int row = 0; row < rowCount; ++row) {
        const char* line = rows[row];
        int runStart = -1;
        for (int col = 0; col <= rowWidth; ++col) {
            char ch = (col < rowWidth) ? line[col] : '\0';
            if (ch == token) {
                if (runStart < 0) runStart = col;
                continue;
            }
            if (runStart < 0) continue;
            int runW = col - runStart;
            int drawX = mirrorX ? (x + (rowWidth - col) * kPigPX) : (x + runStart * kPigPX);
            canvas.fillRect(drawX, y + row * kPigPX, runW * kPigPX, kPigPX, color);
            runStart = -1;
        }
    }
}

// Adds a small body-side overlap so the paw grows out of the torso seam.
void drawHandBridge(M5Canvas& canvas, uint16_t fg,
                    int x, int y, bool openLeft, int handW) {
    int bridge = (handW >= 12) ? 4 : 2;
    int bridgeX = openLeft ? x : (x + handW - bridge);
    canvas.fillRect(bridgeX, y, bridge, bridge, fg);
}

static inline int snapPigGrid(int value) {
    return value & ~(kPigPX - 1);
}

static inline int constrainWalkHoofX(int bodyX, int hoofX, int hoofW) {
    const int minX = snapPigGrid(bodyX);
    const int maxX = snapPigGrid(bodyX + kPigW - hoofW);
    hoofX = snapPigGrid(hoofX);
    if (hoofX < minX) return minX;
    if (hoofX > maxX) return maxX;
    return hoofX;
}

static inline int resolveWalkHoofBaseX(int bodyX, int rightOffset,
                                       int hoofW, bool faceRight) {
    const int offset = faceRight ? rightOffset : (kPigW - rightOffset - hoofW);
    return snapPigGrid(bodyX + offset);
}

struct WalkingGaitFrame {
    int frame = 0;
    int backLegX = 0;
    int backLegY = 0;
    int frontLegX = 0;
    int frontLegY = 0;
    int farArmX = 0;
    int farArmY = 0;
    int nearArmX = 0;
    int nearArmY = 0;
};

static WalkingGaitFrame resolveWalkingGaitFrame(int drawX, int drawY, bool faceRight) {
    WalkingGaitFrame pose;
    // Distance-based: ~10px travel per frame advance
    pose.frame = (int)(walkLegDist / kPixelsPerLegFrame) % kRoomWalkFrameCount;

    int legBaseY = snapPigGrid(drawY + kPigH);
    int armBaseY = snapPigGrid(drawY + kRoomWalkArmBaseY);
    // A biped's feet belong under the body mass. The old rump/shoulder spacing
    // put each foot directly below an arm, merging them into two giant compound
    // appendages and making the horizontal pig read quadrupedal-plus-arms.
    int baseBackLegX = resolveWalkHoofBaseX(drawX, 22, kRoomWalkLegW, faceRight);
    int baseFrontLegX = resolveWalkHoofBaseX(drawX, 38, kRoomWalkLegW, faceRight);
    int baseBackArmX = resolveWalkHoofBaseX(drawX, 6, kRoomWalkArmW, faceRight);
    int baseFrontArmX = resolveWalkHoofBaseX(drawX, 54, kRoomWalkArmW, faceRight);
    int backOffsetX = faceRight ? kWalkBackDx[pose.frame] : -kWalkBackDx[pose.frame];
    int frontOffsetX = faceRight ? kWalkFrontDx[pose.frame] : -kWalkFrontDx[pose.frame];

    pose.backLegX = constrainWalkHoofX(drawX, baseBackLegX + backOffsetX,
                                       kRoomWalkLegW);
    pose.backLegY = snapPigGrid(legBaseY + kWalkBackDy[pose.frame]);
    pose.frontLegX = constrainWalkHoofX(drawX, baseFrontLegX + frontOffsetX,
                                        kRoomWalkLegW);
    pose.frontLegY = snapPigGrid(legBaseY + kWalkFrontDy[pose.frame]);

    // Arms counter-swing against their matching leg. Half-stride keeps each
    // thick paw attached to the torso while the full 2px-grid beat stays visible.
    pose.farArmX = snapPigGrid(baseBackArmX + frontOffsetX / 2);
    pose.farArmY = snapPigGrid(armBaseY + kWalkFrontDy[pose.frame] / 2);
    pose.nearArmX = snapPigGrid(baseFrontArmX + backOffsetX / 2);
    pose.nearArmY = snapPigGrid(armBaseY + kWalkBackDy[pose.frame] / 2);
    return pose;
}

static bool isBackFootLandingFrame(int frame) {
    return frame == 0 || frame == 4;
}

static bool isFrontFootLandingFrame(int frame) {
    return frame == 2 || frame == 6;
}

static void spawnDustPuffAt(int x, int y, uint32_t now) {
    for (int i = 0; i < MAX_DUST_PUFFS; ++i) {
        if (dustPuffs[i].active) continue;
        dustPuffs[i].x = (int16_t)snapPigGrid(x);
        dustPuffs[i].y = (int16_t)snapPigGrid(y);
        dustPuffs[i].spawnTime = now;
        dustPuffs[i].active = true;
        return;
    }
}

static void drawHoofLeg(M5Canvas& canvas, uint16_t fg, uint16_t bg,
                        int x, int y, int legW, int legH,
                        PigLight light, float vol = 1.0f) {
    LimbBump bump = computeLimbBump(fg, bg, x + legW / 2, y + legH / 2, light, vol);
    // Hooves are material detail, not emitters. A restrained body-to-background
    // shade survives inverted themes without turning each toe into a new limb.
    uint16_t hoof = Display::lerpColor565(fg, bg, 0.12f);
    static const char* const kLegBodyRows[] = {
        "######",
        "######",
        "######",
        "######",
        ".####.",
    };
    static const char* const kLegHoofRows[] = {
        "......",
        "......",
        "......",
        "......",
        ".HH.H.",
    };
    static const char* const kShortLegBodyRows[] = {
        "######",
        "######",
        "######",
        ".####.",
    };
    static const char* const kShortLegHoofRows[] = {
        "......",
        "......",
        "......",
        ".HH.H.",
    };
    static const char* const kStubLegBodyRows[] = {
        "#######",
        ".#####.",
    };
    static const char* const kStubLegHoofRows[] = {
        ".......",
        "...H...",
    };

    if (legW == kRoomWalkLegW && legH == kRoomWalkLegH) {
        drawMaskRows(canvas, x, y, kStubLegBodyRows, 2, 7, '#', fg);
        drawMaskRows(canvas, x, y, kStubLegHoofRows, 2, 7, 'H', hoof);
        drawDirectionalLimbEdges(canvas, fg, x, y, legW, kPigPX, bump);
        return;
    }

    if (legW == 12 && (legH == 10 || legH == 8)) {
        const bool shortLeg = (legH == 8);
        const char* const* bodyRows = shortLeg ? kShortLegBodyRows : kLegBodyRows;
        const char* const* hoofRows = shortLeg ? kShortLegHoofRows : kLegHoofRows;
        const int rowCount = shortLeg ? 4 : 5;
        drawMaskRows(canvas, x, y, bodyRows, rowCount, 6, '#', fg);
        drawMaskRows(canvas, x, y, hoofRows, rowCount, 6, 'H', hoof);
        drawDirectionalLimbEdges(canvas, fg, x, y, legW,
                                 legH - 2 * kPigPX, bump);
        // Bottom shadow
        canvas.fillRect(x + kPigPX, y + legH - 2 * kPigPX, legW - 2 * kPigPX, kPigPX, bump.shadow);
        return;
    }

    canvas.fillRect(x, y, legW, legH - kPigPX, fg);
    canvas.fillRect(x + kPigPX, y + legH - kPigPX, legW - 2 * kPigPX, kPigPX, hoof);
}

// openLeft=true → inward tip RIGHT (toward other hand), outward LEFT.
// openLeft=false → inward tip LEFT (toward other hand), outward RIGHT.

// Active room-only paw renderer: filled rounded mass, mirrored attachment, integrated hoof.
void drawFilledUHand(M5Canvas& canvas, uint16_t fg, uint16_t bg,
                     int x, int y, bool openLeft, int handW, int handH,
                     PigLight light, float vol) {
    LimbBump bump = computeLimbBump(fg, bg, x + handW / 2, y + handH / 2, light, vol);
    uint16_t hoof = Display::lerpColor565(fg, bg, 0.12f);
    static const char* const kHand16BodyRows[] = {
        "#######.",
        "########",
        "########",
        ".######.",
        "..#####.",
    };
    static const char* const kHand16HoofRows[] = {
        "........",
        "........",
        "........",
        ".....HH.",
        ".....HH.",
    };
    static const char* const kHand12BodyRows[] = {
        "#####.",
        "######",
        "######",
        ".####.",
        "..###.",
    };
    static const char* const kHand12HoofRows[] = {
        "......",
        "......",
        "......",
        "...HH.",
        "...HH.",
    };
    static const char* const kWalkHand12BodyRows[] = {
        "#####.",
        "######",
        ".#####",
        "..####",
    };
    static const char* const kWalkHand12HoofRows[] = {
        "......",
        "......",
        "......",
        "....H.",
    };
    static const char* const kHand6BodyRows[] = {
        "##.",
        "###",
        ".##",
    };
    static const char* const kHand6HoofRows[] = {
        "...",
        "...",
        "..H",
    };

    const char* const* bodyRows = nullptr;
    const char* const* hoofRows = nullptr;
    int rowCount = 0;
    int rowWidth = 0;

    if (handW == 16 && handH == 10) {
        bodyRows = kHand16BodyRows;
        hoofRows = kHand16HoofRows;
        rowCount = 5;
        rowWidth = 8;
    } else if (handW == 12 && handH == 10) {
        bodyRows = kHand12BodyRows;
        hoofRows = kHand12HoofRows;
        rowCount = 5;
        rowWidth = 6;
    } else if (handW == kRoomWalkArmW && handH == kRoomWalkArmH) {
        bodyRows = kWalkHand12BodyRows;
        hoofRows = kWalkHand12HoofRows;
        rowCount = 4;
        rowWidth = 6;
    } else if (handW == 6 && handH == 6) {
        bodyRows = kHand6BodyRows;
        hoofRows = kHand6HoofRows;
        rowCount = 3;
        rowWidth = 3;
    }

    if (bodyRows != nullptr) {
        drawMaskRows(canvas, x, y, bodyRows, rowCount, rowWidth, '#', fg, !openLeft);
        drawMaskRows(canvas, x, y, hoofRows, rowCount, rowWidth, 'H', hoof, !openLeft);

        int hiH = handH - 3 * kPigPX;
        if (hiH > 0 && bump.hiL != fg) canvas.fillRect(x, y + kPigPX, kPigPX, hiH, bump.hiL);
        if (hiH > 0 && bump.hiR != fg) canvas.fillRect(x + handW - kPigPX, y + kPigPX, kPigPX, hiH, bump.hiR);
        if (handW > 2 * kPigPX) canvas.fillRect(x + 2 * kPigPX, y + handH - 2 * kPigPX, handW - 4 * kPigPX, kPigPX, bump.shadow);
        return;
    }

    canvas.fillRect(x, y, handW, handH, fg);
    canvas.fillRect(openLeft ? (x + handW - kPigPX) : x,
                    y + handH - kPigPX, kPigPX, kPigPX, hoof);
}

// ==[ PIG REAR VIEW ]== unified renderer, no more refill hack
static void drawPigRearView(M5Canvas& canvas, int x, int y,
                            uint16_t bodyFill, uint16_t bg, uint16_t detailColor, bool blink,
                            bool earTwitch, AvatarState state,
                            bool groundedRearStation, uint32_t now,
                            PigLight light = {}) {

    // Build rear-facing pose → PigRenderer handles butt crack + centered tail
    PigRenderPose pose;
    pose.x = x;
    pose.y = y;
    pose.facing = PigFacing::REAR;
    pose.expression = PigExpression::fromState(state, blink, false, 0, earTwitch);
    pose.tailGlyph = 'z';
    pose.tailOnLeft = false;
    pose.detailColor = detailColor;
    pose.bellyBreathePx = 0;
    pose.scale = kPigPX;
    pose.light = light;
    pose.blendRounding = (mode == PigMode::ROAMING);

    // Grounded rear stations put the body envelope itself on the support line;
    // seated rear stations get their legs from drawSittingLegs(). Appending the
    // generic REAR_NUBS here makes window/antenna poses hang below the floor.
    PigRenderer::drawBody(canvas, pose, bodyFill, bg);
    if (groundedRearStation) {
        pose.limbMode = LimbMode::REAR_PLANTED;
        PigRenderer::drawLimbs(canvas, pose, bodyFill, bg, now);
    }

    // Cinematic eyes on back of head (p = body grid cell size, 4px)
    const int p = 4;
    if (rearCinePhase >= RearCinematicPhase::EYES_GROW && rearEyePhase >= 1) {
        int eyeY = y + 14 + 2;
        if (rearEyePhase == 1) {
            canvas.fillRect(x + 20, eyeY, p, p, bg);
            canvas.fillRect(x + 38, eyeY, p, p, bg);
        } else {
            canvas.fillRect(x + 18, eyeY, p * 2, p * 2, bg);
            canvas.fillRect(x + 36, eyeY, p * 2, p * 2, bg);
        }
    }
}

static void drawSittingLegs(M5Canvas& canvas, int drawX, int drawY,
                            uint16_t fg, uint16_t bg, bool fr, bool useRearView, uint32_t now,
                            bool ramenEatingActive, bool cupDrinkingActive,
                            PigLight light = {}) {
    int legBaseY = snapPigGrid(drawY + kPigH);

    // Short, wide seated legs. Keep their original centers while trimming reach.
    const int legW = kRoomLegW;
    const int legH = kRoomLegH;
    if (useRearView) {
        for (int ox : {12, 46}) {
            int lx = snapPigGrid(drawX + ox);
            drawHoofLeg(canvas, fg, bg, lx, legBaseY, legW, legH, light, 1.3f);
            // hoof — full-width cloven continuation
        }
        return;
    }

    auto drawSittingLeg = [&](int lx, int ly) {
        drawHoofLeg(canvas, fg, bg, lx, ly, legW, legH, light, 1.3f);
        // hoof — full-width cloven continuation
    };

    int backLegX  = snapPigGrid(fr ? (drawX + 12) : (drawX + 46));
    int frontLegX = snapPigGrid(fr ? (drawX + 46) : (drawX + 12));

    int backDy = 0;
    int frontDy = 0;
    if (currentStation == Station::COOKING && isChairLegBurstActive(now)) {
        int burstFrame = (int)((now / 120) % 3);
        static const int8_t kBackDy[3] = {0, -2, 0};
        static const int8_t kFrontDy[3] = {0, 0, -2};
        backDy = kBackDy[burstFrame];
        frontDy = kFrontDy[burstFrame];
    }

    drawSittingLeg(backLegX, snapPigGrid(legBaseY + backDy));
    drawSittingLeg(frontLegX, snapPigGrid(legBaseY + frontDy));

    // 2x wider hands (8→16px), 1.5x bump volume
    if (!ramenEatingActive) {
        const int pawW = 16;
        const int pawH = 10;
        int pawY = snapPigGrid(drawY + 30);
        int backPawX  = snapPigGrid(fr ? (drawX + 20) : (drawX + 34));
        int frontPawX = snapPigGrid(fr ? (drawX + 42) : (drawX + 12));
        if (cupDrinkingActive) {
            if (fr) {
                drawFilledUHand(canvas, fg, bg, backPawX, pawY, fr, pawW, pawH, light, 1.5f);
                drawHandBridge(canvas, fg, backPawX, pawY, fr, pawW);
            } else {
                drawFilledUHand(canvas, fg, bg, frontPawX, pawY, !fr, pawW, pawH, light, 1.5f);
                drawHandBridge(canvas, fg, frontPawX, pawY, !fr, pawW);
            }
        } else {
            drawFilledUHand(canvas, fg, bg, backPawX, pawY, fr, pawW, pawH, light, 1.5f);
            drawFilledUHand(canvas, fg, bg, frontPawX, pawY, !fr, pawW, pawH, light, 1.5f);
            drawHandBridge(canvas, fg, backPawX, pawY, fr, pawW);
            drawHandBridge(canvas, fg, frontPawX, pawY, !fr, pawW);
        }
    }
}

// The far arm belongs behind Pancetta's torso. Drawing it before the body lets
// the body mask establish the occlusion instead of letting a rear paw cross the
// face plane when the walk mirrors.
static void drawWalkingFarArm(M5Canvas& canvas, int drawX, int drawY,
                              uint16_t fg, uint16_t bg, bool fr,
                              PigLight light = {}) {
    WalkingGaitFrame pose = resolveWalkingGaitFrame(drawX, drawY, fr);
    drawFilledUHand(canvas, fg, bg, pose.farArmX, pose.farArmY, fr,
                    kRoomWalkArmW, kRoomWalkArmH, light, 1.5f);
    drawHandBridge(canvas, fg, pose.farArmX, pose.farArmY, fr, kRoomWalkArmW);
}

// The near arm and both low stubs stay on the foreground limb plane after the
// body has established the face and torso silhouette.
static void drawWalkingForegroundLimbs(M5Canvas& canvas, int drawX, int drawY,
                                       uint16_t fg, uint16_t bg, bool fr, uint32_t now,
                                       PigLight light = {}) {
    (void)now;
    WalkingGaitFrame pose = resolveWalkingGaitFrame(drawX, drawY, fr);

    // Two low stubs: bipedal, contained, and deliberately less leggy.
    drawHoofLeg(canvas, fg, bg, pose.backLegX, pose.backLegY,
                kRoomWalkLegW, kRoomWalkLegH, light, 1.35f);
    drawHoofLeg(canvas, fg, bg, pose.frontLegX, pose.frontLegY,
                kRoomWalkLegW, kRoomWalkLegH, light, 1.35f);

    // The near arm uses the heavy held-prop paw and counter-swings with the legs.
    drawFilledUHand(canvas, fg, bg, pose.nearArmX, pose.nearArmY, !fr,
                    kRoomWalkArmW, kRoomWalkArmH, light, 1.5f);
    drawHandBridge(canvas, fg, pose.nearArmX, pose.nearArmY, !fr, kRoomWalkArmW);
}

// Uses RP internally: SHADOW_C dark gray core/mid, SHADOW_E dark indigo edge
static void drawPigShadow(M5Canvas& canvas, int pigDrawX, int pigDrawY, uint32_t now,
                           PigLight light = {}) {
    // Breathing sync: shadow width pulses ±2px with pig bob
    int breathe = calcBreathe(now);  // -2..0 range
    int sw = 48 + breathe;           // 46-48px wide, shrinks when pig rises
    int sh = 6;

    // Light-aware offset: push shadow away from light source
    int offsetX = 0;
    if (light.tint != 0) {
        float ldx = (float)(pigDrawX + kPigW / 2 - light.x);
        // Normalize and scale — shadow goes AWAY from light
        if (ldx > 0.1f || ldx < -0.1f) {
            float sign = (ldx > 0) ? 1.0f : -1.0f;
            float mag = fabsf(ldx);
            offsetX = (int)(sign * fminf(mag * 0.12f, 16.0f));
            offsetX = (offsetX / 2) * 2;  // snap to 2px grid
        }
        // Elevation stretch: level light → wider shadow
        float elevDiff = fabsf((float)(pigDrawY + kPigH / 2 - light.y));
        float stretchFactor = 1.0f + 0.4f * fmaxf(0.0f, 1.0f - elevDiff / 120.0f);
        sw = (int)fminf((float)sw * stretchFactor, 72.0f);
    }

    int sx = pigDrawX + (kPigW - sw) / 2 + offsetX;
    int sy = pigDrawY + kPigH;
    if (sy + sh > kFloorY) sy = kFloorY - sh;

    // Elliptical shadow — smooth darken gradient, reads underlying surface.
    // Core = heavy darken, edges = subtle. No dithering zones needed.
    float cx = (float)sx + (float)sw * 0.5f;
    float cy = (float)sy + (float)sh * 0.5f;
    float rx = (float)sw * 0.5f;
    float ry = (float)sh * 0.5f;

    for (int y = sy; y < sy + sh; y += kPigPX) {
        for (int x = sx; x < sx + sw; x += kPigPX) {
            if (x < 0 || x >= SCREEN_WIDTH || y < kRoomY || y >= kFloorY) continue;
            float dx = ((float)x + 1.0f - cx) / rx;
            float dy = ((float)y + 1.0f - cy) / ry;
            float dist = dx * dx + dy * dy;
            if (dist > 1.0f) continue;

            // Smooth falloff: core 0.45 darken → edge 0
            float darkFactor = 0.45f * (1.0f - dist);
            if (darkFactor < 0.03f) continue;  // skip imperceptible
            uint16_t base = fastReadPx(canvas, x, y);
            canvas.fillRect(x, y, kPigPX, kPigPX, darken565(base, darkFactor));
        }
    }
}

// ==[ CAST SHADOW ON SURFACES ]== dithered shadow pool projected from pig body
static void drawCastShadow(M5Canvas& canvas, int pigDrawX, int pigDrawY,
                            PigLight light, uint32_t /*now*/) {
    if (light.tint == 0) return;

    float pigCX = (float)(pigDrawX + kPigW / 2);
    float pigCY = (float)(pigDrawY + kPigH / 2);
    float rawDX = pigCX - (float)light.x;
    float rawDY = pigCY - (float)light.y;
    float rawLen = sqrtf(rawDX * rawDX + rawDY * rawDY);
    if (rawLen < 1.0f) return;
    rawDX /= rawLen;
    rawDY /= rawLen;

    // Gravity bias — blend toward downward so shadow reads as floor pool
    const float kGravBias = 0.65f;
    float dirX = rawDX * (1.0f - kGravBias);
    float dirY = rawDY * (1.0f - kGravBias) + kGravBias;
    float dLen = sqrtf(dirX * dirX + dirY * dirY);
    if (dLen < 0.01f) return;
    dirX /= dLen;
    dirY /= dLen;

    float perpX = -dirY;
    float perpY = dirX;

    const int kSteps = 4;
    const float kBaseIntensity = 0.18f;
    const int kFanHalfW = 3;
    float startHalfW = (float)kPigW * 0.35f;

    float startX = pigCX + dirX * (float)(kPigW / 2);
    float startY = pigCY + dirY * (float)(kPigH / 2);
    uint32_t ditherSeed = 0xCA570001u ^
                          ((uint32_t)(uint16_t)light.x << 16) ^
                          (uint32_t)(uint16_t)light.y;

    for (int step = 0; step < kSteps; step++) {
        float t = (float)(step + 1) / (float)kSteps;
        float rayX = startX + dirX * (float)((step + 1) * kRoomPX);
        float rayY = startY + dirY * (float)((step + 1) * kRoomPX);

        float rayIntensity = kBaseIntensity * (1.0f - t) * (1.0f - t);
        if (rayIntensity < 0.02f) break;

        float halfW = startHalfW * (1.0f + t * 0.4f);
        int perpCells = (int)(halfW / (float)kRoomPX);
        if (perpCells > kFanHalfW) perpCells = kFanHalfW;

        for (int p = -perpCells; p <= perpCells; p++) {
            int px = ((int)(rayX + perpX * (float)(p * kRoomPX))) & ~3;
            int py = ((int)(rayY + perpY * (float)(p * kRoomPX))) & ~3;

            if (px < 0 || px >= SCREEN_WIDTH || py < kRoomY || py >= kFloorY) continue;
            if (px >= pigDrawX - 2 && px < pigDrawX + kPigW + 2 &&
                py >= pigDrawY - 2 && py < pigDrawY + kPigH + 2) continue;

            float perpFade = 1.0f - (float)abs(p) / (float)(perpCells + 1);
            float intensity = rayIntensity * perpFade;
            if (intensity < 0.02f) continue;

            // Dither — skip ~40% of cells for organic dissolve
            uint32_t h = wallHash(px, py, ditherSeed ^ (uint32_t)(step * 37));
            if ((h & 0xFFu) > (uint8_t)(155.0f + intensity * 200.0f)) continue;

            uint16_t base = fastReadPx(canvas, px, py);
            canvas.fillRect(px, py, kRoomPX, kRoomPX, darken565(base, intensity));
        }
    }
}


// ==[ SLEEP BUBBLE ]==
void drawSleepBubble(M5Canvas& canvas) {
    const char* text = SLEEP_PHRASES[sleepPhraseIdx];
    int pX = (int)pigX;
    int noseX = faceRight ? (pX + 57) : (pX + 15);
    int noseY = (int)pigY + 21;
    Mood::drawBubbleAt(canvas, text, pX, pX + kPigW, (int)pigY, noseX, noseY);
}


void drawWindowDust(M5Canvas& canvas, uint16_t fg, uint32_t now, int wdx, int wdy) {
    if (Weather::isThunderFlashing()) return;  // thunder flash overpowers dust
    // 8 dust motes floating through the noir shaft to keep volume visible.
    // Shaft runs from window sill down to floor.
    const int sillY = kR2_WindowY + wdy + kR2_WindowH + 4;
    const int shaftH = kFloorY - sillY - 8;
    const float winCX = (float)(kR2_WindowX + wdx + kR2_WindowW / 2);  // window center X
    static const int periods[8] = {4000, 5500, 7000, 4700, 6200, 5100, 7600, 4300};
    static const int sizes[8][2] = {{4,4}, {4,4}, {4,8}, {8,4}, {4,4}, {4,8}, {8,4}, {4,4}};
    for (int i = 0; i < 8; i++) {
        float period = (float)periods[i];
        float phase = (float)((now + (uint32_t)(i * 1731)) % (uint32_t)periods[i]) / period;
        // Vertical: cycle between window sill and floor
        float vt = fastSinf(phase * 3.14159f * 2.0f + (float)i * 0.9f) * 0.5f + 0.5f;  // 0..1
        int dustY = sillY + (int)(vt * (float)shaftH);
        // Horizontal: widen toward floor
        float spread = 0.5f + vt * 0.3f;
        float drift = fastSinf(phase * 3.14159f * 4.0f + (float)i * 2.1f) * 28.0f * spread;
        int dustX = (int)(winCX + drift);
        int dustW = sizes[i][0];
        int dustH = sizes[i][1];
        dustX = dustX & ~3;
        dustY = dustY & ~3;
        if (dustX > kR2_WindowX + wdx && dustX + dustW < kR2_WindowX + wdx + kR2_WindowW &&
            dustY > kRoomY + 4 && dustY + dustH < kFloorY - 4)
            canvas.fillRect(dustX, dustY, dustW, dustH, fg);
    }
}

void clearPigEffectSnapshot() {
    pigEffectSnapshot.valid = false;
    pigEffectSnapshot.x = 0;
    pigEffectSnapshot.y = 0;
    pigEffectSnapshot.w = 0;
    pigEffectSnapshot.h = 0;
    pigEffectSnapshot.gridW = 0;
    pigEffectSnapshot.gridH = 0;
    pigEffectSnapshot.centerX = 0;
    pigEffectSnapshot.centerY = 0;
    pigEffectSnapshot.light = {};
}

static inline uint16_t pigEffectSnapshotAt(int x, int y) {
    int lx = x - pigEffectSnapshot.x;
    int ly = y - pigEffectSnapshot.y;
    int gx = lx / kPigPX;
    int gy = ly / kPigPX;
    return pigEffectSnapshot.pixels[gy * pigEffectSnapshot.gridW + gx];
}

void capturePigEffectSnapshot(M5Canvas& canvas, const PigPose& pose,
                              PigLight light) {
    clearPigEffectSnapshot();

    int sx = pose.drawX & ~1;
    int sy = (pose.drawY - 2) & ~1;
    int ex = sx + kPigW;
    int ey = sy + kPigH + 10;

    if (sx < 0) sx = 0;
    if (sy < kRoomY) sy = kRoomY;
    if (ex > SCREEN_WIDTH) ex = SCREEN_WIDTH;
    if (ey > kFloorY + 4) ey = kFloorY + 4;

    int w = ex - sx;
    int h = ey - sy;
    if (w <= 0 || h <= 0 || w > kPigEffectSnapMaxW || h > kPigEffectSnapMaxH) return;
    int gridW = (w + kPigPX - 1) / kPigPX;
    int gridH = (h + kPigPX - 1) / kPigPX;
    if (gridW > kPigEffectSnapCols || gridH > kPigEffectSnapRows) return;

    uint16_t* buf = (uint16_t*)canvas.getBuffer();
    int stride = canvas.width();
    for (int gy = 0; gy < gridH; gy++) {
        const uint16_t* src = &buf[(sy + gy * kPigPX) * stride + sx];
        uint16_t* dst = &pigEffectSnapshot.pixels[gy * gridW];
        for (int gx = 0; gx < gridW; gx++) {
            uint16_t raw = src[gx * kPigPX];
            // The shader samples one value per 2x2 cell; copying every source
            // pixel only burned PSRAM bandwidth without changing the mask.
            // cppcheck-suppress objectIndex
            dst[gx] = (raw << 8) | (raw >> 8);
        }
    }

    pigEffectSnapshot.x = (int16_t)sx;
    pigEffectSnapshot.y = (int16_t)sy;
    pigEffectSnapshot.w = (int16_t)w;
    pigEffectSnapshot.h = (int16_t)h;
    pigEffectSnapshot.gridW = (int16_t)gridW;
    pigEffectSnapshot.gridH = (int16_t)gridH;
    pigEffectSnapshot.centerX = (int16_t)(pose.drawX + kPigW / 2);
    pigEffectSnapshot.centerY = (int16_t)(pose.drawY + kPigH / 2);
    pigEffectSnapshot.light = light;
    pigEffectSnapshot.valid = true;
}

bool isPigEffectPixel(int x, int y, uint16_t currentColor) {
    if (!pigEffectSnapshot.valid) return false;
    if (isNearBG(currentColor)) return false;
    if (x < pigEffectSnapshot.x || y < pigEffectSnapshot.y) return false;
    if (x >= pigEffectSnapshot.x + pigEffectSnapshot.w ||
        y >= pigEffectSnapshot.y + pigEffectSnapshot.h) return false;
    return currentColor != pigEffectSnapshotAt(x, y);
}

void applyDirectionalPigNoir(M5Canvas& canvas, const PigNoirProfile& profile) {
    if (mode != PigMode::ROAMING ||
        roamState == RoamState::ROOM_TRANSITION) return;
    if (!pigEffectSnapshot.valid) return;
    const PigLight light = pigEffectSnapshot.light;
    if (light.tint == 0) return;

    const int centerX = pigEffectSnapshot.centerX;
    const int centerY = pigEffectSnapshot.centerY;
    float ldx = (float)light.x - (float)centerX;
    float ldy = (float)light.y - (float)centerY;
    float len2 = ldx * ldx + ldy * ldy;
    if (len2 < 4.0f) {
        ldx = 0.0f;
        ldy = -1.0f;
    } else {
        float invLen = 1.0f / sqrtf(len2);
        ldx *= invLen;
        ldy *= invLen;
    }

    // Normalize the light once. The per-cell half-Lambert stays in Q8 so all
    // six rooms avoid floating point in their hottest inner loop.
    const int ldxQ8 = (int)lroundf(ldx * 256.0f);
    const int ldyQ8 = (int)lroundf(ldy * 256.0f);
    const int x0 = pigEffectSnapshot.x;
    const int y0 = pigEffectSnapshot.y;
    const int x1 = x0 + pigEffectSnapshot.w;
    const int y1 = y0 + pigEffectSnapshot.h;

    for (int py = y0; py < y1; py += kPigPX) {
        const int nyQ8 = ((py + 1 - centerY) * 512) / (kPigH + 10);
        for (int px = x0; px < x1; px += kPigPX) {
            uint16_t base = fastReadPx(canvas, px, py);
            if (!isPigEffectPixel(px, py, base)) continue;

            const int nxQ8 = ((px + 1 - centerX) * 512) / kPigW;
            int facingQ8 = (nxQ8 * ldxQ8 + nyQ8 * ldyQ8) >> 8;
            if (facingQ8 < -256) facingQ8 = -256;
            else if (facingQ8 > 256) facingQ8 = 256;

            const int halfLambertQ8 = (facingQ8 + 256) >> 1;
            const int illumQ8 = profile.ambient8 +
                ((halfLambertQ8 * (256 - profile.ambient8)) >> 8);
            const int penetratedQ8 =
                (illumQ8 * profile.penetration8) >> 8;
            const uint8_t darken8 = (uint8_t)(
                (profile.depth8 * (256 - penetratedQ8)) >> 8);
            uint16_t result = lerpColor565_8(base, RP::BG, darken8);

            if (illumQ8 > profile.tintThreshold8) {
                const uint8_t tint8 = (uint8_t)(
                    ((illumQ8 - profile.tintThreshold8) * profile.tintScale8) >> 8);
                result = screenBlend565(result, light.tint, tint8);
            }
            fastFillBlock2(canvas, px, py, result);
        }
    }
}


static void drawCurrentRoom(M5Canvas& canvas, uint32_t now,
                            RoomRenderPass pass) {
    if (pass == RoomRenderPass::LIVE) {
        room2LightingRuntime.valid = false;
    }
    switch (currentRoom) {
        case 0: drawRoom0(canvas, now, pass); break;
        case 1: drawRoom1(canvas, now, pass); break;
        case 2: drawRoom2(canvas, now, pass); break;
        case 3: drawRoom3(canvas, now, pass); break;
        case 4: drawRoom4(canvas, now, pass); break;
        case 5: drawRoom5(canvas, now, pass); break;
    }
}

static uint64_t mixRoomCacheKey(uint64_t hash, uint32_t value) {
    hash ^= value;
    return hash * 1099511628211ull;
}

static uint64_t currentRoomCacheKey() {
    uint64_t key = 1469598103934665603ull;
    key = mixRoomCacheKey(key, (uint32_t)currentRoom);
    key = mixRoomCacheKey(key, (uint32_t)(parallaxFar + 32));
    key = mixRoomCacheKey(key, (uint32_t)(parallaxMid + 32));
    key = mixRoomCacheKey(key, (uint32_t)(parallaxNear + 32));
    key = mixRoomCacheKey(key, RP::BG);
    key = mixRoomCacheKey(key, RP::STRUCT);
    key = mixRoomCacheKey(key, RP::NEON);
    key = mixRoomCacheKey(key, RP::WARM);
    key = mixRoomCacheKey(key, RP::CRT);
    return key;
}

static void drawCurrentRoomCached(M5Canvas& canvas, uint32_t now) {
    const uint64_t key = currentRoomCacheKey();
    // Restore the playfield only. The caller draws the status bar before us and
    // rooms start at kRoomY, so rows 0..kTopBarH-1 are never room space — a
    // full-height copy would blit the plate's dead top rows over the bar.
    // Rooms have no bottom bar, so the plate runs to the last scanline.
    constexpr int kRoomPlateH = UIMeasurements::kScreenHeight -
                                UIMeasurements::kTopBarH;
    if (SceneCache::restore(canvas, SceneCache::Owner::ROOM, key,
                            UIMeasurements::kTopBarH, kRoomPlateH)) return;

    M5Canvas* base = SceneCache::rebuildTarget();
    if (!base) {
        drawCurrentRoom(canvas, now, RoomRenderPass::BASE);
        return;
    }

    base->fillSprite(RP::BG);
    drawCurrentRoom(*base, now, RoomRenderPass::BASE);
    SceneCache::commit(SceneCache::Owner::ROOM, key);
    if (!SceneCache::restore(canvas, SceneCache::Owner::ROOM, key,
                             UIMeasurements::kTopBarH, kRoomPlateH)) {
        drawCurrentRoom(canvas, now, RoomRenderPass::BASE);
    }
}

static void spawnBathWetTrackAt(int x, int y, uint32_t now, bool pointsRight) {
    BathWetTrack& track = bathWetTracks[nextBathWetTrack];
    nextBathWetTrack = (uint8_t)((nextBathWetTrack + 1u) % MAX_BATH_WET_TRACKS);
    track.x = (int16_t)snapPigGrid(x);
    track.y = (int16_t)snapPigGrid(y);
    track.spawnTime = now;
    track.active = true;
    track.pointsRight = pointsRight;
}

static void drawBathWetTracks(M5Canvas& canvas, uint32_t now) {
    if (currentRoom != 5) return;

    for (int i = 0; i < MAX_BATH_WET_TRACKS; ++i) {
        BathWetTrack& track = bathWetTracks[i];
        if (!track.active) continue;
        uint32_t age = now - track.spawnTime;
        if (age >= BATH_WET_TRACK_MS) {
            track.active = false;
            continue;
        }

        // Wetness fades into the sampled deck instead of carrying a flat decal.
        uint8_t strength = (uint8_t)(108u - (age * 84u) / BATH_WET_TRACK_MS);
        int dir = track.pointsRight ? 1 : -1;
        auto drawWetCell = [&](int x, int y, uint8_t alpha) {
            if (x < 0 || y < kRoomY || x + kPigPX > SCREEN_WIDTH ||
                y + kPigPX > SCREEN_HEIGHT) return;
            uint16_t deck = fastReadPx(canvas, x, y);
            fastFillBlock2(canvas, x, y,
                           Display::screenBlend565(deck, RP::PUDDLE, alpha));
        };

        // Two-cell cloven print, angled in travel direction; a fresh heel bead
        // hangs around briefly, then the deck keeps only the hoof evidence.
        drawWetCell(track.x, track.y - kPigPX, strength);
        drawWetCell(track.x + dir * kPigPX, track.y - 2 * kPigPX, strength);
        if (age < 1200u)
            drawWetCell(track.x - dir * kPigPX, track.y,
                        (uint8_t)(strength / 2u));
    }
}

// ==[ FOREGROUND LAYERS ]== drawn AFTER pig for depth/volume occlusion
// Room foreground functions (drawRoom0Foreground..drawRoom5Foreground) extracted to src/ui/rooms/

static void drawCurrentRoomForeground(M5Canvas& canvas, uint32_t now) {
    switch (currentRoom) {
        case 0: drawRoom0Foreground(canvas, now); break;
        case 1: drawRoom1Foreground(canvas, now); break;
        case 2: drawRoom2Foreground(canvas, now); break;
        case 3: drawRoom3Foreground(canvas, now); break;
        case 4: drawRoom4Foreground(canvas, now); break;
        case 5: drawRoom5Foreground(canvas, now); break;
    }
    if (currentRoom == 4) restoreRoom4CategoricalSources(canvas);
}

static void drawRoomNoirPass(M5Canvas& canvas, uint32_t now) {
    PigLight frameLight = getRoomNeonLight(currentRoom, now);
    if (mode == PigMode::ROAMING) {
        int sampleX = (SCREEN_WIDTH - kPigW) / 2;
        int sampleY = kFloorY - kPigH;
        if (roamState != RoamState::ROOM_TRANSITION) {
            PigPose pose = resolvePigPose(now, isRoamingMoveState(),
                                          shouldUseRearViewInRoaming(), false);
            sampleX = pose.drawX;
            sampleY = pose.drawY;
        }
        // During a room transition there is no stable actor position to score,
        // but the physical sources are still live. A grounded centre sample
        // keeps the final reflection on the active CRT/window/lamp instead of
        // falling back to a sign that may currently be dark or off-grid.
        frameLight = selectRoomPigKeyLight(currentRoom,
                                           sampleX, sampleY, now);
    }
    drawRoomNoirFrame(canvas, (uint8_t)currentRoom, frameLight, now);
}

// Helper: foreground pipe + drip strand
static void drawHelperForeground(M5Canvas& canvas, uint32_t now) {
    // Foreground pipe crossing over pig on crate
    int pipeY = kRoomY + 80;
    int pipeX = kHelperPigX - 8;
    int pipeW = 60;
    canvas.fillRect(pipeX, pipeY, pipeW, kRoomPX, RP::STRUCT);
    canvas.fillRect(pipeX, pipeY + kRoomPX, pipeW, kRoomPX, RP::SHADOW_C);
    // Bracket
    canvas.fillRect(pipeX + 24, pipeY - 6, kRoomPX, 6, RP::STRUCT);
    canvas.fillRect(pipeX + 48, pipeY - 6, kRoomPX, 6, RP::STRUCT);
    // Drip from foreground pipe — falls in front of pig
    static constexpr uint32_t kDripCycleMs = 2400u;
    static constexpr uint32_t kDripFallMs = 1320u;
    static constexpr uint32_t kDripSplashMs = 240u;
    const uint32_t phaseMs = (now - dripCycleStart) % kDripCycleMs;
    const int dropX = (pipeX + 30) & ~3;
    const uint16_t dropCol = lerpColor565_8(RP::PUDDLE, RP::SHAFT, 104);
    const int startY = (pipeY + kRoomPX * 2 + kRoomPX - 1) &
                       ~(kRoomPX - 1);
    const int lastCellY = (kFloorY - kRoomPX) & ~(kRoomPX - 1);

    if (phaseMs < kDripFallMs) {
        // Quadratic fall in Q8; crossfade only the moving head between cells.
        const uint16_t phaseQ8 = (uint16_t)((phaseMs * 255u) / kDripFallMs);
        const uint16_t fallQ8 = (uint16_t)(((uint32_t)phaseQ8 * phaseQ8) >> 8);
        const int travelPx = lastCellY - startY;
        const int dropYQ8 = startY * 256 + travelPx * (int)fallQ8;
        const int cellY = (dropYQ8 >> 8) & ~(kRoomPX - 1);
        const uint8_t frac8 = (uint8_t)((dropYQ8 - cellY * 256) >> 2);

        auto drawHeadCell = [&](int y, uint8_t weight8) {
            if (weight8 < 12 || y < startY || y + kRoomPX > kFloorY) return;
            const uint8_t alpha = (uint8_t)(28u + ((uint16_t)weight8 * 92u >> 8));
            const uint16_t base = fastReadPx(canvas, dropX, y);
            canvas.fillRect(dropX, y, kRoomPX, kRoomPX,
                            screenBlend565(base, dropCol, alpha));
        };
        drawHeadCell(cellY, (uint8_t)(255u - frac8));
        drawHeadCell(cellY + kRoomPX, frac8);
    } else if (phaseMs < kDripFallMs + kDripSplashMs) {
        const uint8_t splashQ8 = (uint8_t)(((phaseMs - kDripFallMs) * 255u) /
                                           kDripSplashMs);
        const uint8_t alpha = (uint8_t)(88u - ((uint16_t)splashQ8 * 64u >> 8));
        const int splashY = lastCellY;
        for (int dx = -kRoomPX; dx <= kRoomPX; dx += kRoomPX * 2) {
            const int x = dropX + dx;
            const uint16_t base = fastReadPx(canvas, x, splashY);
            canvas.fillRect(x, splashY, kRoomPX, kRoomPX,
                            screenBlend565(base, RP::PUDDLE, alpha));
        }
    }
}

// ==[ SPEECH BUBBLE FOR HELPER ]==

static void drawHelperBubble(M5Canvas& canvas, const char* description) {
    if (!description || !description[0]) return;
    int pX = (int)pigX;
    int noseX = faceRight ? (pX + 57) : (pX + 15);
    int noseY = (int)pigY + 21;
    // force bubble RIGHT/ABOVE pig — pass pigX=0 to trick pigOnLeft check
    Mood::drawBubbleAt(canvas, description, 0, pX + kPigW, (int)pigY, noseX, noseY);
}

static void drawHelperBackdrop(M5Canvas& canvas, uint32_t now) {
    // Clear right zone (behind menu)
    canvas.fillRect(UIMeasurements::roomX(164), kRoomY,
                    SCREEN_WIDTH - UIMeasurements::roomX(164), kRoomH, RP::BG);

    // Alley wall texture (depth layers — uses RP internally)
    drawConcreteWall(canvas, UIMeasurements::roomX(164), kRoomY,
                     SCREEN_WIDTH - UIMeasurements::roomX(164), kRoomH, 5u);

    // ==[ WALL BUMPS: exposed brick patch ]== cracked plaster reveals masonry
    {
        int bpx = 220, bpy = kRoomY + 100;
        for (int by = 0; by < 16; by += 8) {
            for (int bx = 0; bx < 20; bx += 12) {
                int offset = ((by / 8) & 1) ? 4 : 0;
                canvas.fillRect((bpx + bx + offset) & ~3, bpy + by, 8, 4, RP::SHADOW_C);
            }
        }
        // Mortar lines between bricks
        canvas.fillRect(bpx, bpy + 4, 20, 4, RP::DEEP);
        // Plaster fracture edges
        canvas.fillRect(bpx - 4, bpy, 4, 4, RP::FILL);
        canvas.fillRect(bpx + 20, bpy + 16, 4, 4, RP::FILL);
    }

    // ==[ WALL BUMPS: vertical conduit ]== cable run from pipe to mid-wall
    {
        int cx = 296, ctop = kPipeY + 8, cbot = 156;
        canvas.fillRect(cx, ctop, 4, cbot - ctop, RP::WALL_MID);
        // Mounting clips
        for (int cy = ctop + 16; cy < cbot; cy += 28)
            canvas.fillRect(cx - 2, cy, 8, 4, RP::STRUCT);
        // Junction box
        int jy = (ctop + cbot) / 2;
        canvas.fillRect(cx - 4, jy, 12, 8, RP::STRUCT);
        canvas.fillRect(cx - 2, jy + 2, 8, 4, RP::FILL);
        // LED on junction box (slow blink)
        if ((now / 1200) & 1)
            canvas.fillRect(cx + 6, jy + 2, 4, 4, RP::CRT);
    }

    // ==[ WALL BUMPS: crack line ]== zigzag fracture mid-right
    {
        int cx = 284, cy = kRoomY + 120;
        canvas.fillRect(cx, cy, 4, 8, RP::SHADOW_C);
        canvas.fillRect(cx + 4, cy + 8, 4, 4, RP::SHADOW_C);
        canvas.fillRect(cx + 4, cy + 12, 4, 8, RP::SHADOW_C);
        canvas.fillRect(cx, cy + 20, 4, 8, RP::SHADOW_C);
    }

    // ==[ WALL BUMPS: exhaust vent ]== upper right
    {
        int vx = 304, vy = kRoomY + 56;
        canvas.fillRect(vx, vy, 12, 8, RP::STRUCT);
        for (int s = 0; s < 3; s++)
            canvas.fillRect(vx + 2 + s * 4, vy + 2, 4, 4, RP::DEEP);
    }

    // ==[ WALL BUMPS: security camera ]== upper left corner, blinking LED
    {
        int camX = 220, camY = kRoomY + 12;
        canvas.fillRect(camX, camY, 4, 4, RP::STRUCT);         // wall bracket
        canvas.fillRect(camX, camY + 4, 8, 4, RP::WALL_MID);  // body
        canvas.fillRect(camX + 8, camY + 4, 4, 4, RP::STRUCT); // lens housing
        // Red LED: 1s on, 3s off
        if ((now / 1000) % 4 == 0)
            canvas.fillRect(camX + 8, camY + 4, 4, 4, RP::SPARK);
    }

    // ==[ WALL BUMPS: rain streaks ]== noir world, wall always wet
    for (int i = 0; i < 2; i++) {
        int rx = 228 + i * 22;
        int ry = kRoomY + 10 + (int)(wallHash(rx, 0, 88117 + i) & 0x1F);
        int streakH = 16 + (int)(wallHash(rx, 1, 88117 + i) & 0x0F);
        for (int dy = 0; dy < streakH; dy += kRoomPX) {
            if ((wallHash(rx, ry + dy, 99117 + i) & 0xFF) < 50)
                canvas.fillRect(rx & ~3, (ry + dy) & ~3, kRoomPX, kRoomPX, RP::DUST);
        }
    }

    // ==[ DUST MOTES ]== floating particles (rooms all have these — helper zone only)
    {
        int helperLeft = UIMeasurements::roomX(164);
        static const int dustPeriods[2] = {3600, 5200};
        for (int i = 0; i < 2; i++) {
            float period = (float)dustPeriods[i];
            float phase = (float)((now / 40 + (uint32_t)(i * 1571)) % (uint32_t)dustPeriods[i]) / period;
            int baseX = helperLeft + 12 + (int)((wallHash(i + 100, 0, 7713) >> 4) % 80u);
            int baseY = kRoomY + 16 + (int)((wallHash(i + 100, 1, 7713) >> 8) % 50);
            int dx = (int)(fastSinf(phase * 3.14159f * 2.0f + (float)i * 1.7f) * 6.0f);
            int dy = (int)(fastSinf(phase * 3.14159f * 4.0f + (float)i * 2.3f) * 3.0f);
            int mx = (baseX + dx) & ~3;
            int my = (baseY + dy) & ~3;
            if (mx > helperLeft + 4 && mx < SCREEN_WIDTH - 4 && my > kRoomY + 4 && my < kFloorY - 4)
                canvas.fillRect(mx, my, kRoomPX, kRoomPX, RP::DUST);
        }
    }

    // ==[ NEON WALL WASH ]== HACK sign illuminates surrounding wall
    PigLight hl = {};
    if (isNeonOn(now)) {
        hl.x = kNeonX + kNeonW / 2;
        hl.y = kNeonY + kNeonH;
        hl.tint = RP::NEON;
        drawNeonWash(canvas, 216, kRoomY + 4, SCREEN_WIDTH - 216, kFloorY - kRoomY,
                     hl, RP::WALL_FAR, 140.0f, 0.30f, 0x7A11);
    }

    // ==[ CEILING PIPE RUN ]== horizontal infrastructure (rooms all have these)
    canvas.fillRect(220, kRoomY + 8, 60, kRoomPX, RP::WALL_MID);
    canvas.fillRect(290, kRoomY + 10, 26, kRoomPX, RP::WALL_MID);
    // Vertical drop from ceiling pipe
    canvas.fillRect(252, kRoomY + 8, kRoomPX, 10, RP::WALL_MID);
    // Rust at joint
    canvas.fillRect(254, kRoomY + 16, kRoomPX, 6, RP::SHADOW_C);

    // Alley scene: neon sign, pipe, sparking wire (furniture — pre-darken)
    drawAlleyNeon(canvas, RP::NEON, RP::BG, now);
    drawAlleyPipe(canvas, RP::STRUCT);
    drawAlleySpark(canvas, RP::SPARK, now);

    // Pipe drip puddle stain
    canvas.fillRect(kPipeX + 18, kFloorY - 2, 4, 4, RP::FLOOR_GRIME);
    canvas.fillRect(kPipeX + 20, kFloorY - 4, 4, 4, RP::FLOOR_GRIME);
    canvas.fillRect(kPipeX + 22, kFloorY - 2, 4, 4, RP::FLOOR_GRIME);
    canvas.fillRect(kPipeX + 16, kFloorY - 2, 4, 4, RP::FLOOR_GRIME);

    if (rippleRadius > 0) {
        float rt = (float)(now - rippleStart) / (float)RIPPLE_MS;
        if (rt >= 1.0f) {
            rippleRadius = 0;
        } else {
            int r = 2 + (int)(rt * 8.0f);
            uint8_t density = (uint8_t)(60.0f * (1.0f - rt));
            int cx = kPipeX + 20, cy = kFloorY;
            for (int a = 0; a < 8; a++) {
                float angle = (float)a * 0.785f;
                int rx = (cx + (int)(cosf(angle) * (float)r)) & ~3;
                int ry = (cy + (int)(fastSinf(angle) * (float)r * 0.4f)) & ~3;
                if (ry >= kFloorY - 2 && ry <= kFloorY + 4 &&
                    rx >= UIMeasurements::roomX(164) && rx < SCREEN_WIDTH - 2 &&
                    (wallHash(rx, ry, 22117 + a) & 0xFF) < density) {
                    canvas.fillRect(rx, ry, kRoomPX, kRoomPX, RP::FLOOR_GRIME);
                }
            }
        }
    }

    // Quiet neon clinic glyph: pig ears + a repair cross. No copy competing
    // with the menu rows; just a small promise that the alley can fix lonely.
    if (isNeonOn(now)) {
        int gx = 276, gy = 44;
        canvas.fillRect(gx + 4, gy, 4, 4, RP::NEON);
        canvas.fillRect(gx + 16, gy, 4, 4, RP::NEON);
        canvas.fillRect(gx, gy + 4, 24, 16, RP::NEON);
        canvas.fillRect(gx + 4, gy + 8, 16, 8, RP::BG);
        canvas.fillRect(gx + 8, gy + 8, 4, 4, RP::CRT);
        canvas.fillRect(gx + 16, gy + 8, 4, 4, RP::CRT);
        canvas.fillRect(gx + 8, gy + 16, 8, 4, RP::NEON);
        canvas.fillRect(gx + 12, gy + 12, 4, 12, RP::FLUOR);
        canvas.fillRect(gx + 8, gy + 16, 12, 4, RP::FLUOR);
    }

    drawAlleyCrate(canvas, RP::STRUCT, RP::BG);

    drawFloorDrain(canvas, RP::STRUCT, RP::BG, 272, kFloorY - 4);

    // ==[ POP OUTLINES ]== BG contours for readability (rooms have 10-15 per room)
    drawPopOutline1px(canvas, kBenchX, kBenchY, kBenchW, kBenchH, PopOutlineStyle::MIXED, 0x7B01u);    // crate
    drawPopOutline1px(canvas, kNeonX, kNeonY, kNeonW, kNeonH, PopOutlineStyle::SOLID, 0x7B11u);        // HACK neon
    drawPopOutline1px(canvas, kPipeX, kPipeY - 4, kPipeW, 12, PopOutlineStyle::SPARSE, 0x7B21u);       // pipe
    drawPopOutline1px(canvas, kSparkX, kSparkY, 8, 20, PopOutlineStyle::SPARSE, 0x7B31u);               // spark wire
    drawPopOutline1px(canvas, 296, kPipeY + 8, 8, 130, PopOutlineStyle::SPARSE, 0x7B41u);               // conduit
    drawPopOutline1px(canvas, 304, kRoomY + 56, 12, 8, PopOutlineStyle::SOLID, 0x7B51u);                // exhaust vent
    drawPopOutline1px(canvas, 220, kRoomY + 12, 12, 8, PopOutlineStyle::SOLID, 0x7B61u);                // security cam
    drawPopOutline1px(canvas, 220, kRoomY + 100, 20, 16, PopOutlineStyle::SPARSE, 0x7B71u);             // brick patch
    drawPopOutline1px(canvas, 272, kFloorY - 4, 12, 8, PopOutlineStyle::SPARSE, 0x7B81u);               // floor drain

    // ==[ METAL FLOOR ]== proper floor treatment with depth gradient (manual — scoped to helper zone)
    {
        int helperLeft = UIMeasurements::roomX(164);
        // Floor grid dashes
        for (int gx = helperLeft; gx < SCREEN_WIDTH; gx += 8) {
            if (gx + 4 <= SCREEN_WIDTH) {
                uint32_t dh = wallHash(gx, 777, 54323);
                if ((dh & 0xFFF) >= 100)  // ~2.5% missing = damage
                    canvas.fillRect(gx, kFloorY, 4, kRoomPX, RP::FLOOR_GRID);
            }
        }
        // Perpendicular tick marks
        for (int gx = helperLeft; gx < SCREEN_WIDTH; gx += 16)
            canvas.fillRect(gx + 4, kFloorY - 4, kRoomPX, 4, RP::FLOOR_GRID);
        // Floor depth gradient (Bayer dithered — rooms all have this)
        for (int gy = kFloorY - 8; gy < kFloorY + 8; gy += kRoomPX) {
            for (int gx = helperLeft; gx < SCREEN_WIDTH; gx += kRoomPX) {
                float depth = (float)(gy - kFloorY + 8) / 16.0f;
                uint8_t threshold = (uint8_t)(depth * 60.0f);
                if (bayer4[(gy/kRoomPX) & 3][(gx/kRoomPX) & 3] < threshold)
                    canvas.fillRect(gx, gy, kRoomPX, kRoomPX, RP::FLOOR_GRIME);
            }
        }
        // Gutter edge
        for (int gx = helperLeft; gx < SCREEN_WIDTH; gx += 8)
            canvas.fillRect(gx, kFloorY + 4, 4, kRoomPX, RP::FLOOR_GRIME);
    }

    // ==[ ENRICHMENT: service case on floor near crate ]==
    drawServiceCase(canvas, RP::WALL_MID, RP::BG, kBenchX - 16, kFloorY - 12);
    drawPopOutline1px(canvas, kBenchX - 16, kFloorY - 12, 12, 12, PopOutlineStyle::SPARSE, 0x7BA1u);
    // ==[ ENRICHMENT: cable coil on floor ]==
    drawCableCoil(canvas, RP::WALL_MID, kBenchX + kBenchW + 16, kFloorY - 6);

    // ==[ NOIR AMBIENT KILL: darken helper zone — cast light reveals ]==
    {
        int helperLeft = UIMeasurements::roomX(164);
        // Skip zone: HACK neon sign face (gas tube is emissive)
        int nsx0 = 0, nsx1 = 0, nsy0 = 0, nsy1 = 0;
        bool neonOn = isNeonOn(now);
        if (neonOn) {
            nsx0 = (kNeonX + 4) & ~3;
            nsy0 = (kNeonY + 2) & ~3;
            nsx1 = kNeonX + kNeonW - 4;
            nsy1 = kNeonY + kNeonH - 2;
        }
        const uint16_t darkBG = RP::BG;
        const uint8_t darkT8 = (uint8_t)(0.42f * 256.0f);
        for (int py = kRoomY + 4; py < kFloorY; py += kRoomPX) {
            for (int px = helperLeft; px < SCREEN_WIDTH - 4; px += kRoomPX) {
                // neon sign face — emissive, skip
                if (neonOn && px >= nsx0 && px < nsx1 && py >= nsy0 && py < nsy1) continue;
                uint16_t base = fastReadPx(canvas, px, py);
                fastFillBlock4(canvas, px, py, lerpColor565_8(base, darkBG, darkT8));
            }
        }
    }

    // ==[ CAST LIGHT: re-apply on darkened scene — only these reveal objects ]==
    // HACK neon sign cast
    if (isNeonOn(now)) {
        // Sign glow pool below
        drawLightPool(canvas, RP::NEON, kNeonX + 4, kNeonY + kNeonH + 2, 20, 8, 55, 55055);
        // Volumetric dust beam shaft
        drawVolumetricDustBeam(canvas, now,
                               kNeonX + kNeonW / 2, kNeonY + kNeonH + 2, kNeonW - 4,
                               kNeonX + kNeonW / 2 + 2, kFloorY - 2, kNeonW + 12,
                               RP::NEON, RP::DUST, 0x7A33u);
        // Bench neon pool
        drawLightPool(canvas, RP::NEON, kBenchX, kBenchY - 2, kBenchW, 4, 30, 66317);
        // Pipe drip puddle neon reflection
        for (int dx = 0; dx < 12; dx += kRoomPX) {
            if ((wallHash(kPipeX + 16 + dx, kFloorY + 2, 44117) & 0xFF) < 40)
                canvas.fillRect(kPipeX + 16 + dx, kFloorY + 2, kRoomPX, kRoomPX, RP::PUDDLE);
        }
    }
    // Spark cast light (when active)
    if (sparkActive)
        drawLightPool(canvas, RP::SPARK, kSparkX - 4, kSparkY + 4, 16, 12, 35, 88117);
    // Junction box LED (emissive indicator — post-darken so it pops)
    {
        int jy = (kPipeY + 8 + 156) / 2;
        if ((now / 1200) & 1)
            canvas.fillRect(302, jy + 2, 4, 4, RP::CRT);
    }
    // Security camera LED (emissive — post-darken)
    if ((now / 1000) % 4 == 0)
        canvas.fillRect(228, kRoomY + 16, 4, 4, RP::SPARK);
    // Floor neon puddle (wider reflection)
    for (int px = 216; px < SCREEN_WIDTH; px += kRoomPX) {
        uint8_t h = (uint8_t)(wallHash(px, kFloorY + 4, 55117 + now / 300) & 0xFF);
        if (h < 18)
            canvas.fillRect(px, kFloorY + 4, kRoomPX, kRoomPX,
                            isNeonOn(now) ? RP::PUDDLE : RP::DUST);
    }

    // ==[ POST-CAST EMISSIVE PARTICLES ]==
    static constexpr uint32_t kRoom1PipeCondensationSalt = 0xA401u;
    static constexpr int kRoom1PipeCondensationCount = 1;

    drawAlleyDrip(canvas, lerpColor565_8(RP::PUDDLE, RP::SHAFT, 104),
                  RP::PUDDLE, now);
    drawCondensation(canvas, now, kPipeX, kPipeY, kPipeW, 8,
                     kRoom1PipeCondensationCount, RP::SHAFT,
                     kRoom1PipeCondensationSalt);
}

// ==[ ROAMING SPEECH BUBBLE ]==

static void refreshNarratorBubble() {
    // The buffer is channel state, not a history slot. Leaving the laptop
    // must release L7 even when the next station has no narrator line.
    narratorBubbleText[0] = '\0';
    if (Config::getIppEnabled()) {
        const char* phrase = NoirNarrator::getContextPhrase((uint8_t)currentStation);
        if (phrase && phrase[0]) {
            strncpy(narratorBubbleText, phrase, sizeof(narratorBubbleText) - 1);
            narratorBubbleText[sizeof(narratorBubbleText) - 1] = '\0';
            return;
        }
    }
    // fallback: original laptop lines for laptop, sleep for sofa/bed
    if (currentStation == Station::AT_LAPTOP) {
        strncpy(narratorBubbleText, LAPTOP_LINES[laptopLineIdx % LAPTOP_LINE_COUNT],
                sizeof(narratorBubbleText) - 1);
        narratorBubbleText[sizeof(narratorBubbleText) - 1] = '\0';
    }
}

static bool narratorBubbleOwnsChannel() {
    if (shouldSuppressRoamBubble() || roamState != RoamState::IDLE)
        return false;
    if (!Config::getIppEnabled() && currentStation != Station::AT_LAPTOP)
        return false;
    return narratorBubbleText[0] != '\0';
}

static void drawRoamBubble(M5Canvas& canvas) {
    if (!narratorBubbleOwnsChannel()) return;

    int pX = (int)pigX;
    int noseX = faceRight ? (pX + 57) : (pX + 15);
    int noseY = (int)pigY + 21;
    Mood::drawBubbleAt(canvas, narratorBubbleText, pX, pX + kPigW,
                       (int)pigY, noseX, noseY);
}

static void drawHeldRamenRig(M5Canvas& canvas,
                             uint16_t fg, uint16_t bg,
                             int drawX, int drawY, int headDrawY,
                             uint32_t now, bool holdPose,
                             const MenuPigRenderOptions& options,
                             PigLight light = {}) {
    const int p = kPigPX;
    const uint16_t noodleColor = Display::getColorFG();

    // -4px: face leans toward bowl (matches drawDirectFace faceDx)
    int noseX = (drawX + 40) & ~1;
    int noseY = (headDrawY + 22) & ~1;

    int bx = 0, by = 0;
    getHeldBowlPosition(drawX, drawY, bx, by);
    by += 2 * p;
    int bw = 22;
    int bh = 12;
    int bowlBy = by & ~1;  // snap to pig grid

    int strandTop = (noseY + 4 * p) & ~1;
    int strandBottom = (bowlBy + 3 * p) & ~1;
    if (strandBottom <= strandTop) strandBottom = strandTop + p;
    static const int8_t mouthDx[3] = {-2, 0, 2};
    static const int8_t bowlDx[3] = {-4, 0, 4};
    const float wavePhase = (float)now * 0.012f;
    const float waveSpan = 6.2831853f;
    if (!ramenSlurpRuntime.initialized ||
        now < ramenSlurpRuntime.lastNow ||
        now - ramenSlurpRuntime.lastNow > 4000) {
        for (int i = 0; i < 3; i++) {
            ramenSlurpRuntime.eventStart[i] = now;
            ramenSlurpRuntime.eventTTL[i] = 0;
            ramenSlurpRuntime.ampPx[i] = 0;
            ramenSlurpRuntime.nextEvent[i] = now + 160 + (uint32_t)i * 120;
        }
        ramenSlurpRuntime.initialized = true;
    }
    ramenSlurpRuntime.lastNow = now;

    int maxSlurpCut = strandBottom - strandTop - p;
    if (maxSlurpCut < p) maxSlurpCut = p;
    for (int i = 0; i < 3; i++) {
        if (!holdPose && TimeMath::reached(now, ramenSlurpRuntime.nextEvent[i])) {
            uint32_t r = (uint32_t)wallHash((int)now, i * 131, (int)(now / 41U) + i * 59);
            uint32_t ttl = 220 + (r & 0x1FF);
            uint32_t gap = 320 + ((r >> 9) & 0x3FF);
            int amp = p + (int)(((r >> 19) & 0x0F) * p);
            if (amp > maxSlurpCut) amp = maxSlurpCut;
            amp &= ~1;
            if (amp < p) amp = p;
            ramenSlurpRuntime.eventStart[i] = now;
            ramenSlurpRuntime.eventTTL[i] = ttl;
            ramenSlurpRuntime.ampPx[i] = (int8_t)amp;
            ramenSlurpRuntime.nextEvent[i] = now + ttl + gap;
        }

        int cutPx = holdPose ? min(maxSlurpCut, p * 3) : 0;
        uint32_t ttl = ramenSlurpRuntime.eventTTL[i];
        uint32_t elapsed = now - ramenSlurpRuntime.eventStart[i];
        if (!holdPose && ttl > 0 && elapsed < ttl) {
            float prog = (float)elapsed / (float)ttl;
            float env = (prog < 0.35f) ? (prog / 0.35f)
                                       : (1.0f - (prog - 0.35f) / 0.65f);
            if (env < 0.0f) env = 0.0f;
            cutPx = (int)((float)ramenSlurpRuntime.ampPx[i] * env);
            cutPx &= ~1;
        }

        int liveTop = strandTop + cutPx;
        if (liveTop > strandBottom - p) liveTop = strandBottom - p;
        const float mouthX = (float)((noseX + mouthDx[i] * p) & ~1);
        const float bowlX = (float)((bx + bw / 2 + bowlDx[i]) & ~1);
        for (int y = liveTop; y < strandBottom; y += p) {
            float t = (float)(y - liveTop) / (float)((strandBottom - liveTop) + p);
            float laneX = mouthX + (bowlX - mouthX) * t;
            int waveX = holdPose ? 0 : (int)(fastSinf(wavePhase + t * waveSpan + (float)i) * (float)(p / 2));
            int sx = ((int)laneX + waveX) & ~1;
            if (sx < bx + 2 * p) sx = bx + 2 * p;
            if (sx > bx + bw - 3 * p) sx = bx + bw - 3 * p;
            canvas.fillRect(sx, y, p, p, noodleColor);
        }
    }

    // Bowl + paws drawn OVER noodles so strands disappear behind the bowl
    drawNoodleBowl(canvas, RP::STRUCT, RP::BG, bx, bowlBy, bw, bh, true);

    static const int8_t kGripBob[8] = {0, -1, -1, 0, 0, 1, 1, 0};
    int rightBobY = holdPose ? 0 : kGripBob[(now / 120) % 8];
    // 2x wider ramen grip (6→12px), 8px tall, grid-snapped
    int leftPawX = snapPigGrid(bx - 12);
    int leftPawY = snapPigGrid(bowlBy - 6);
    int rightPawX = snapPigGrid(bx + bw);
    int rightPawY = snapPigGrid(bowlBy - 5 + rightBobY);
    drawFilledUHand(canvas, fg, bg, leftPawX, leftPawY, true, 12, 10, light);
    drawFilledUHand(canvas, fg, bg, rightPawX, rightPawY, false, 12, 10, light);
    drawHandBridge(canvas, fg, leftPawX, leftPawY, true, 12);
    drawHandBridge(canvas, fg, rightPawX, rightPawY, false, 12);

    if (options.includeRamenSteam) {
        drawRamenVapor(canvas, now, bx + bw / 2, by - 2);
        drawRamenVapor(canvas, now + 620, bx + bw / 2 - 2, by - 4);
    }
}

void drawBootRamenPig(M5Canvas& canvas, int drawX, int drawY, uint32_t now, bool holdPose) {
    uint16_t fg = Display::getColorFG();
    uint16_t bg = Display::getColorBG();
    Display::PigPalette pigPalette = Display::makePigPalette(fg, bg);
    MenuPigRenderOptions options;
    int bodyY = drawY & ~1;
    bool ingestSniff = holdPose || (calcIngestHeadDip(now, true, false) > 0);
    PigEyeLook eyeLook = resolveMenuPigEyeLook(false, false, Station::COOKING, true, false, now);

    drawPigShadow(canvas, drawX & ~1, bodyY, now);
    Avatar::drawBodyOnly(canvas, drawX & ~1, bodyY, pigPalette.bodyFill, bg,
                         AvatarState::NEUTRAL, false, true,
                         ingestSniff, false, eyeLook, pigPalette.detail, 'z', {}, true);
    drawSittingLegs(canvas, drawX & ~1, bodyY, pigPalette.bodyFill, bg, true, false, now, true, false);
    drawHeldRamenRig(canvas, pigPalette.bodyFill, bg, drawX & ~1, bodyY, bodyY, now, holdPose, options);
    Avatar::drawHairsAt(canvas, (int16_t)(drawX & ~1), (int16_t)bodyY, 0,
                        true, AvatarState::NEUTRAL, false);
}

// ==[ PIG DRAW (shared between helper and roaming) ]==

static void drawBedSleepPose(M5Canvas& canvas, int x, int y,
                             const Display::PigPalette& palette,
                             PigLight light) {
    const int p = kPigPX;
    uint16_t skin = palette.bodyFill;
    uint16_t shade = Display::lerpColor565(skin, RP::DEEP, 0.24f);
    if (light.tint != 0) shade = screenBlend565f(shade, light.tint, 0.10f);

    // Side-on silhouette: head settles into the exposed pillow at the left,
    // while the shoulder runs under the foreground blanket to the right.
    canvas.fillRect(x + 8, y, 10, 4, shade);                 // folded ear
    canvas.fillRect(x + 6, y + 2, 16, 4, skin);
    canvas.fillRect(x + 2, y + 6, 28, 16, skin);
    canvas.fillRect(x + 6, y + 22, 24, 4, shade);
    canvas.fillRect(x, y + 12, 10, 10, skin);               // snout on pillow
    canvas.fillRect(x, y + 20, 8, 4, shade);
    canvas.fillRect(x + 28, y + 12, 38, 22, skin);          // shoulder under quilt
    canvas.fillRect(x + 32, y + 30, 32, 4, shade);

    // Closed eye and small profile nose keep the rotated pose unmistakable.
    canvas.fillRect(x + 10, y + 12, 8, p, palette.detail);
    canvas.fillRect(x + 2, y + 16, p, p, palette.detail);
    canvas.fillRect(x + 12, y - 2, p, 4, palette.detail);
    canvas.fillRect(x + 18, y, p, 4, palette.detail);
}

static void drawBathJumpTuck(M5Canvas& canvas, int x, int y,
                             uint16_t pigFill, uint16_t detail,
                             uint32_t now) {
    float t = getBathJumpProgress(now);
    if (t < 0.0f) return;
    int kick = (t > 0.28f && t < 0.72f) ? kPigPX : 0;
    int legY = y + 34 - kick;

    // Two compact folded legs keep the airborne silhouette cute and stop the
    // normal walk cycle from paddling through the tub wall.
    canvas.fillRect(x + 14, legY, 14, kRoomPX, pigFill);
    canvas.fillRect(x + 18, legY + kRoomPX, 10, kPigPX, detail);
    canvas.fillRect(x + 44, legY + kPigPX, 14, kRoomPX, pigFill);
    canvas.fillRect(x + 44, legY + kRoomPX + kPigPX, 10, kPigPX, detail);
}

static void drawBathJoint(M5Canvas& canvas, int drawX, int drawY,
                          uint32_t now, uint16_t pigFill, bool includeSmoke) {
    if (mode != PigMode::ROAMING || currentRoom != 5 ||
        !isStationVisualActive(Station::IN_BATH) ||
        isBathJumpActive()) {
        return;
    }

    const int p = kPigPX;
    BathIdleFrame bathIdle = sampleBathIdleFrame(now);
    // The joint never survives a dive: while Pancetta is under (or moving
    // through) the waterline no part of the cigarette may render, regardless
    // of what the per-phase visibility flags claim. What she already breathed
    // out is NOT part of the prop — it belongs to the room air and keeps
    // drifting. Water swallowing a plume scatters it; it does not delete it.
    if (bathIdle.phase == BathIdlePhase::SINK ||
        bathIdle.phase == BathIdlePhase::SUBMERGED ||
        bathIdle.phase == BathIdlePhase::RISE) {
        if (bathIdle.phase == BathIdlePhase::SINK) {
            SmokeFx::disturb(SmokeFx::Source::PigCig,
                             (now - bathIdle.phaseElapsedMs) | 1u, 190);
        }
        return;
    }
    if (!bathIdle.jointVisible) return;

    const int dir = faceRight ? 1 : -1;
    const SmokeFx::BreathFrame breath = SmokeFx::sampleBreath(now);
    const bool dancePuff = isBathSoundDanceSurfaceActive(now) &&
        BathMic::danceBeatPhase(now) <= 1u;

    // Cigarette is a fat cylinder: 2 cells (4px) thick so it reads as a stick,
    // not a thin line. Its inner (filter) end sits at the mouth centre — matching
    // drawDirectFace's mouthX = snoutX + 10 — and it juts outward past the snout,
    // so it never lies across the whole mouth.
    const int cigH    = 2 * p;                                   // 4px, twice fatter
    const int mouthX  = (drawX + (faceRight ? 48 : 20)) & ~(p - 1);
    const int mouthY  = (drawY + 26) & ~(p - 1);
    const int filterW = 2 * p, paperW = 3 * p;
    const int filterX = faceRight ? mouthX : mouthX - filterW;
    const int paperX  = faceRight ? filterX + filterW : filterX - paperW;
    const int emberX  = faceRight ? paperX + paperW : paperX - p;

    // Filter band at the lip.
    canvas.fillRect(filterX, mouthY, filterW, cigH, RP::GREEN_DK);
    // Paper body.
    canvas.fillRect(paperX, mouthY, paperW, cigH, RP::D_WARM);
    // Ember rides the breath: it goes hot through the whole pull and banks back
    // down while the lungs hold. On screen the coal is the only thing that says
    // "inhaling" — no smoke may leave the mouth until the exhale.
    const uint8_t emberHeat = dancePuff && breath.emberHeat < 224u
        ? 224u : breath.emberHeat;
    canvas.fillRect(emberX, mouthY, p, cigH,
                    lerpColor565_8(RP::WARM, RP::SPARK, emberHeat));
    if (emberHeat > 196u && ((now / 120u) & 1u) == 0u)
        canvas.fillRect(emberX, mouthY - p, p, p, RP::WARM);

    // Relaxed paw cups the filter end, just under the stick.
    const int pawX = faceRight ? (mouthX - p) : (mouthX + p);
    canvas.fillRect(pawX, mouthY + cigH, filterW, p, pigFill);
    canvas.fillRect(pawX + (faceRight ? p : 0), mouthY + cigH - p, p, p, pigFill);

    if (!includeSmoke || !bathIdle.smokeVisible) return;

    // ==[ EMBER THREAD ]== the laminar trail a lit cigarette always has.
    // Declared, never drawn directly: the module fades it and converts it into
    // drifting volumes the moment this declaration stops. The thread renders
    // at kRoomPX like the rest of the smoke: the 2px cig grid stops at the
    // prop. Emitter grid never licenses effect grid (contract 2.3).
    SmokeFx::setWisp(SmokeFx::Source::PigCig, emberX, mouthY - kRoomPX,
                     mouthY - kRoomPX - 44, dir, 0xD3A1u);
    // ==[ EXHALE ]== the whole point. Nothing during the drag or the hold, then
    // a slug of fog out of the snout that stalls, swells, and dissolves.
    SmokeFx::ExhaleParams ex;
    ex.x = drawX + (faceRight ? 62 : 6);
    ex.y = mouthY - p;
    ex.dirX = (int8_t)dir;
    ex.power = 205;
    ex.seed = 0xD3A1u;
    SmokeFx::driveExhale(SmokeFx::Source::PigCig, breath, ex);
}

// ==[ CIGARETTE VOLUMES ]== deliberately outside every prop visibility guard.
// Smoke in the air is owned by the room, not by the cigarette that made it, so
// this pass runs even while the joint itself is hidden.
static void drawPigCigVolumes(M5Canvas& canvas, uint32_t now,
                              const PigLight& roomLight, bool includeSmoke) {
    if (!includeSmoke) return;
    SmokeFx::Lighting lit;
    lit.add(roomLight, 160.0f, 130);
    uint16_t colFar = RP::SOFT;
    if (roomLight.tint != 0)
        colFar = Display::screenBlend565(RP::SOFT, roomLight.tint, 90u);
    SmokeFx::drawWisp(canvas, SmokeFx::Source::PigCig, now,
                      RP::SOFT, colFar, &lit);
    SmokeFx::draw(canvas, SmokeFx::Source::PigCig, RP::DUST, colFar, &lit);
}

static void drawBathDancePaws(M5Canvas& canvas, int drawX, int drawY,
                              uint16_t pigFill, uint16_t detail,
                              bool facingRight, uint32_t now) {
    static constexpr uint8_t kLeadLift[8] = {0, 2, 6, 4, 0, 2, 6, 4};
    const int p = kPigPX;
    const uint8_t beat = BathMic::danceBeatPhase(now);
    const int leadLift = kLeadLift[beat];
    const int followLift = kLeadLift[(beat + 4u) & 7u];
    const int leadX = drawX + (facingRight ? 54 : 14);
    const int followX = drawX + (facingRight ? 14 : 54);
    const int leadY = drawY + 28 - leadLift;
    const int followY = drawY + 28 - followLift;

    // Two raised forepaws are enough to read as a seated tub dance without
    // introducing a third motion locus around the silhouette.
    canvas.fillRect(leadX, leadY, p * 2, p * 2, pigFill);
    canvas.fillRect(followX, followY, p * 2, p * 2, pigFill);
    canvas.fillRect(leadX, leadY, p * 2, p, detail);
    canvas.fillRect(followX, followY, p * 2, p, detail);
}

static void drawPig(M5Canvas& canvas, uint16_t fg, uint16_t bg,
                    uint32_t now, bool isMoving, bool useRearView,
                    const MenuPigRenderOptions& options) {
    const int p = kPigPX;
    Display::PigPalette pigPalette = Display::makePigPalette(fg, bg);
    const uint16_t pigFill = pigPalette.bodyFill;
    PigPose pose = resolvePigPose(now, isMoving, useRearView, true);
    int drawX = pose.drawX;
    int drawY = pose.drawY;
    bool ramenEatingActive = pose.ramenEating;
    bool cupDrinkingActive = pose.cupDrinking;
    int ingestHeadDip = pose.ingestHeadDip;
    const bool characterSniff = isCharacterSniffActive(now);
    const uint8_t characterSniffFrame = characterSniff
        ? sampleCharacterSniffFrame(now) : 0xFF;
    bool bedSleepPose =
        mode == PigMode::ROAMING && currentRoom == 2 &&
        currentStation == Station::IN_BED && roamState == RoamState::IDLE &&
        !isMoving && !useRearView && !wallBreakActive;
    PigEyeLook eyeLook = resolveMenuPigEyeLook(useRearView, isMoving, currentStation,
                                               ramenEatingActive, cupDrinkingActive, now);
    // The shared room/boot gait has two arm depth planes. Resolve once here so
    // the far paw can sit behind the body while the near paw remains readable.
    const bool walkingLimbs = shouldDrawWalkingLegs(isMoving, useRearView);
    const bool bathSurfaceDance = isBathSoundDanceSurfaceActive(now);
    // Music moves the paws and body only. FaceTimer owns eyelid timing, so a
    // new beat can never cut short or restart Pancetta's blink.
    const bool renderBlink = faceTimer.blinking;
    int16_t shakeY = (int16_t)(drawY - (int)pigY);
    // The booth glint is a small social/attention beat. Let Pancetta lean into
    // it once, then settle; the same fidget clock drives the nearby prop so
    // neither element feels independently animated.
    const int boothHeadBob = calcBoothFidgetHeadBob(now);
    const int sniffHeadBob = calcCharacterSniffHeadBob(now);
    const int upperBobY = boothHeadBob + sniffHeadBob;
    bool splitChairUpperBody =
        upperBobY != 0 && !isMoving && !useRearView && !bedSleepPose &&
        !ramenEatingActive && !cupDrinkingActive;
    int headDrawY = drawY + upperBobY;

    // One mapped key drives ground shadow, cast shadow, limbs, body, and noir.
    PigLight roomLight;
    if (mode == PigMode::ROAMING)
        roomLight = selectRoomPigKeyLight(currentRoom, drawX, drawY, now);
    else
        roomLight = getHelperNeonLight(now);

    // Shadow
    bool room2CookingShadow =
        (mode == PigMode::ROAMING &&
         currentRoom == 2 &&
         currentStation == Station::COOKING &&
         room2LightingRuntime.valid);
    if (mode != PigMode::ROAMING ||
        (currentStation != Station::IN_BED &&
         roamState != RoamState::ROOM_TRANSITION)) {
        drawPigShadow(canvas, drawX, drawY, now, roomLight);
        if (room2CookingShadow)
            drawRoom2CounterTopShadow(canvas, drawX, drawY, now, roomLight);
        else
            drawCastShadow(canvas, drawX, drawY, roomLight, now);
    }
    if (mode == PigMode::ROAMING)
        capturePigEffectSnapshot(canvas, pose, roomLight);
    else clearPigEffectSnapshot();

    if (walkingLimbs)
        drawWalkingFarArm(canvas, drawX, drawY, pigFill, bg, faceRight, roomLight);

    // Body
    if (bedSleepPose) {
        drawBedSleepPose(canvas, drawX, drawY, pigPalette, roomLight);
    } else if (useRearView) {
        const bool groundedRearStation =
            mode == PigMode::ROAMING && roamState == RoamState::IDLE &&
            !isSeatedStation(currentStation);
        drawPigRearView(canvas, drawX, drawY, pigFill, bg, pigPalette.detail,
                        renderBlink, faceTimer.earTwitching, avatarState,
                        groundedRearStation, now, roomLight);
    } else {
        bool ingestSniff = (ingestHeadDip > 0) || characterSniff;
        if (splitChairUpperBody) {
            const int clipPadX = 16;
            const int clipX = drawX - clipPadX;
            const int clipW = kPigW + clipPadX * 2;

            const bool blend = (mode == PigMode::ROAMING);
            canvas.setClipRect(clipX, drawY + 28, clipW, 24);
            Avatar::drawBodyOnly(canvas, drawX, drawY, pigFill, bg,
                                 avatarState, renderBlink, faceRight,
                                 ingestSniff, faceTimer.earTwitching, eyeLook, pigPalette.detail, 'z', roomLight, ramenEatingActive, blend,
                                 characterSniffFrame);
            canvas.clearClipRect();

            canvas.setClipRect(clipX, drawY - 4, clipW, 34);
            Avatar::drawBodyOnly(canvas, drawX, headDrawY, pigFill, bg,
                                 avatarState, renderBlink, faceRight,
                                 ingestSniff, faceTimer.earTwitching, eyeLook, pigPalette.detail, 'z', roomLight, ramenEatingActive, blend,
                                 characterSniffFrame);
            canvas.clearClipRect();

            drawChairUpperBodySeam(canvas, drawX, drawY, upperBobY, pigFill, bg);
        } else {
            Avatar::drawBodyOnly(canvas, drawX, drawY, pigFill, bg,
                                 avatarState, renderBlink, faceRight,
                                 ingestSniff, faceTimer.earTwitching, eyeLook, pigPalette.detail, 'z', roomLight, ramenEatingActive,
                                 mode == PigMode::ROAMING, characterSniffFrame);
        }
    }

    // Seated and walking limbs are mutually exclusive. The boot-to-WARTHOG
    // tableau intentionally keeps ON_LEDGE as its station anchor while its
    // mount phase is WALKING; evaluating both predicates independently drew
    // the seated hands and the walking hands together.

    // Sitting legs
    if (!bedSleepPose && !walkingLimbs && shouldDrawSittingLegs()) {
        drawSittingLegs(canvas, drawX, drawY, pigFill, bg, faceRight, useRearView, now,
                        ramenEatingActive, cupDrinkingActive, roomLight);
    }

    if (bathSurfaceDance) {
        drawBathDancePaws(canvas, drawX, drawY, pigFill, pigPalette.detail,
                          faceRight, now);
    }

    if (isBathJumpActive())
        drawBathJumpTuck(canvas, drawX, drawY, pigFill, pigPalette.detail, now);

    // Walking foreground limbs (moving, front view only). The far arm is
    // already behind the body, so this pass cannot overwrite the face.
    if (walkingLimbs) {
        drawWalkingForegroundLimbs(canvas, drawX, drawY, pigFill, bg, faceRight, now,
                                   roomLight);
    }

    // Coffee cup
    if (carryingCup) {
        // Cup is a transit prop; never render while seated at ramen or in pod.
        bool badState = (mode != PigMode::ROAMING ||
                         currentStation == Station::IN_BED ||
                         currentStation == Station::COOKING ||
                         currentStation == Station::IN_BATH);
        if (now - cupPickupTime >= CUP_CARRY_MS || badState) {
            carryingCup = false;
            cupSteamPrimed = false;
        } else if (!useRearView) {
            // Sip cycle (kept close to ramen slurp cadence).
            float sipPhase = (float)((now + 280) % 2300) / 2300.0f;
            int sipDy = (sipPhase > 0.52f && sipPhase < 0.78f) ? p : 0;
            int cupW = 10, cupH = 10;
            int cupX = (drawX + 31) & ~1;  // centered between paws
            int cupY = (drawY + 24 + sipDy) & ~1;
            bool seatedCup = cupDrinkingActive && !walkingLimbs && shouldDrawSittingLegs();

            if (!seatedCup) {
                // Transit pose keeps the cup-side reach.
                int armY = cupY + 4;
                if (faceRight) {
                    int armX0 = drawX + 56;
                    int armW = cupX - armX0 + p;
                    if (armW < p) armW = p;
                    canvas.fillRect(armX0, armY, armW, p, pigFill);
                } else {
                    int armX0 = cupX + cupW;
                    int armW = drawX + 14 - armX0 + p;
                    if (armW < p) armW = p;
                    canvas.fillRect(armX0, armY, armW, p, pigFill);
                }
            }

            // Cup body with solid top outline (same visual language as bowl).
            canvas.fillRect(cupX - p, cupY - p, cupW + 2 * p, p, bg);
            canvas.fillRect(cupX - p, cupY, p, cupH, bg);
            canvas.fillRect(cupX + cupW, cupY, p, cupH, bg);
            canvas.fillRect(cupX, cupY + cupH, cupW, p, bg);
            canvas.fillRect(cupX, cupY, cupW, cupH, fg);
            canvas.fillRect(cupX + p, cupY + p, cupW - 2 * p, cupH - 3 * p, RP::FILL);
            canvas.fillRect(cupX + p, cupY + cupH - 2 * p, cupW - 2 * p, p, fg);

            // Handle
            int hx = faceRight ? (cupX + cupW) : (cupX - p);
            canvas.fillRect(hx, cupY + 2 * p, p, 3 * p, fg);
            if (faceRight) canvas.fillRect(hx + p, cupY + 3 * p, p, p, fg);
            else canvas.fillRect(hx - p, cupY + 3 * p, p, p, fg);

            if (!seatedCup) {
                // Transit pose keeps the cup-side paw wrap.
                int pawX = faceRight ? (cupX - p) : (cupX + cupW - p);
                int pawY = cupY + 3 * p;
                canvas.fillRect(pawX, pawY, p, p, pigFill);
                canvas.fillRect(pawX, pawY + p, p, p, pigFill);
                canvas.fillRect(faceRight ? (pawX - p) : (pawX + p), pawY + p, p, p, pigFill);
            }

            // Movement-reactive steam (lags with pig motion, fades as it rises).
            int cupMidX = (cupX + cupW / 2) & ~1;
            int cupTopY = cupY - p;
            if (!cupSteamPrimed) {
                cupSteamPrevX = cupMidX;
                cupSteamPrevY = cupTopY;
                cupSteamLagX = 0;
                cupSteamLagY = 0;
                cupSteamPrimed = true;
            }
            int mvx = cupMidX - cupSteamPrevX;
            int mvy = cupTopY - cupSteamPrevY;
            cupSteamPrevX = cupMidX;
            cupSteamPrevY = cupTopY;
            cupSteamLagX = (cupSteamLagX * 3 + mvx * 2) / 4;
            cupSteamLagY = (cupSteamLagY * 3 + mvy * 2) / 4;
            if (cupSteamLagX > 6) cupSteamLagX = 6;
            if (cupSteamLagX < -6) cupSteamLagX = -6;
            if (cupSteamLagY > 6) cupSteamLagY = 6;
            if (cupSteamLagY < -6) cupSteamLagY = -6;

            if (options.includeCupSteam) {
                for (int i = 0; i < 4; i++) {
                    uint32_t phase = (now + i * 420) % 1700;
                    uint32_t generation = (now + (uint32_t)i * 420u) / 1700u;
                    float t = (float)phase / 1700.0f;
                    int rise = (int)(t * 16.0f);
                    float wave = fastSinf(t * 3.14159f * 2.0f + (float)i * 1.9f) * (2.0f + (float)(i & 1));
                    int dragX = (int)((float)cupSteamLagX * (1.0f - t));
                    int dragY = (int)((float)cupSteamLagY * (1.0f - t));
                    int sx = (cupMidX + (int)wave - dragX) & ~1;
                    int sy = (cupTopY - rise + dragY) & ~1;
                    if (sy <= (int)UIMeasurements::kTopBarH + 1) continue;
                    uint8_t keep = (uint8_t)(190.0f * (1.0f - t));
                    if ((wallHash(i, (int)generation, 0xC0F5u) & 0xFF) < keep) {
                        canvas.fillRect(sx, sy, p, p, fg);
                        if (t < 0.35f) {
                            int sx2 = sx + ((i & 1) ? p : -p);
                            if ((wallHash(i, (int)generation, 0xC0F6u) & 0xFF) < (keep >> 1))
                                canvas.fillRect(sx2, sy, p, p, fg);
                        }
                    }
                }
            }
        } else {
            cupSteamPrimed = false;
        }
    } else {
        cupSteamPrimed = false;
    }

    // ==[ RAMEN EATING ]== both paws hold bowl + synced vertical noodle waves
    if (ramenEatingActive) {
        drawHeldRamenRig(canvas, pigFill, bg, drawX, drawY, headDrawY, now, false, options, roomLight);
    }

    // Bath prop remains on the pig layer. The privacy mosaic, water laps, tub
    // lip, and foreground steam still occlude it in their physical order.
    drawBathJoint(canvas, drawX, drawY, now, pigFill,
                  options.includeCigSmoke);

    // ==[ NOIR CIG ]== at avatar mouth line, smoke only when rear-view at window.
    if (windowCigLit && !ramenEatingActive && !cupDrinkingActive) {
        int cigY = (drawY + 28) & ~1;  // avatar mouth Y (PIG_DRAW_TOP_INSET + 26)
        int cigX;
        bool rearWindowSmoke =
            (useRearView && mode == PigMode::ROAMING &&
             currentStation == Station::AT_WINDOW && roamState == RoamState::IDLE);
        const SmokeFx::BreathFrame breath = SmokeFx::sampleBreath(now, 900u);
        const uint16_t ember =
            lerpColor565_8(RP::WARM, RP::SPARK, breath.emberHeat);

        if (useRearView) {
            // Rear view: cigarette protrudes from right cheek toward window.
            cigX = (drawX + 60) & ~1;
            canvas.fillRect(cigX, cigY, 2 * p, p, fg);
            canvas.fillRect(cigX + 2 * p, cigY, p, p, ember);
        } else if (faceRight) {
            cigX = (drawX + 50) & ~1;
            canvas.fillRect(cigX, cigY, 2 * p, p, fg);
            canvas.fillRect(cigX + 2 * p, cigY, p, p, ember);
        } else {
            // Face left: cig extends leftward from mouth, ember at tip.
            cigX = (drawX + 22) & ~1;
            canvas.fillRect(cigX - 2 * p, cigY, 2 * p, p, fg);
            canvas.fillRect(cigX - 3 * p, cigY, p, p, ember);  // ember tip
        }

        // Standing at the glass she smokes the same way she does in the tub:
        // the coal glows through the pull with a bare thread off the tip, then
        // the breath goes out against the pane and rolls back off it.
        if (rearWindowSmoke && options.includeCigSmoke) {
            const int emberCellX = (cigX + 2 * p) & ~1;
            const int wispBaseY = cigY - kRoomPX;
            SmokeFx::setWisp(SmokeFx::Source::PigCig, emberCellX, wispBaseY,
                             wispBaseY - 40, 1, 0xC167u);
            SmokeFx::ExhaleParams ex;
            ex.x = drawX + 66;
            ex.y = cigY - 2 * p;
            ex.dirX = 1;
            ex.power = 190;
            ex.seed = 0xC167u;
            SmokeFx::driveExhale(SmokeFx::Source::PigCig, breath, ex);
        }
    }

    // Hair
    if (!bedSleepPose)
        Avatar::drawHairsAt(canvas, (int16_t)drawX, (int16_t)headDrawY, shakeY,
                            faceRight, avatarState, useRearView);

    // After the hair: a breath that crosses the face has to pass in FRONT of
    // it, including the strands.
    drawPigCigVolumes(canvas, now, roomLight, options.includeCigSmoke);

    // ==[ WALKING DUST PUFFS ]== tiny particles at pig feet during movement
    if (options.includeDust) {
        if (isMoving && !useRearView && mode == PigMode::ROAMING &&
            !isBathJumpActive()) {
            WalkingGaitFrame dustPose = resolveWalkingGaitFrame(drawX, drawY, faceRight);
            int dustX = 0;
            int dustY = 0;
            int landingFrame = -1;

            if (isBackFootLandingFrame(dustPose.frame)) {
                landingFrame = dustPose.frame;
                dustX = snapPigGrid(dustPose.backLegX + kRoomWalkLegW / 2 - kPigPX / 2);
                dustY = snapPigGrid(dustPose.backLegY + kRoomWalkLegH - kPigPX);
            } else if (isFrontFootLandingFrame(dustPose.frame)) {
                landingFrame = dustPose.frame;
                dustX = snapPigGrid(dustPose.frontLegX + kRoomWalkLegW / 2 - kPigPX / 2);
                dustY = snapPigGrid(dustPose.frontLegY + kRoomWalkLegH - kPigPX);
            }

            if (landingFrame >= 0) {
                if (landingFrame != lastFootLandingFrame) {
                    bool leavingBathWet = currentRoom == 5 &&
                        currentStation == Station::IN_BATH &&
                        roamState == RoamState::WALKING_TO;
                    if (leavingBathWet)
                        spawnBathWetTrackAt(dustX, dustY, now, faceRight);
                    else
                        spawnDustPuffAt(dustX, dustY, now);
                }
                lastFootLandingFrame = (int8_t)landingFrame;
            } else {
                lastFootLandingFrame = -1;
            }
        } else {
            lastFootLandingFrame = -1;
        }
    }
    // Dust puffs are composited with the room environment in drawRoaming().

    // ==[ ARRIVAL MICRO-ANIMATION ]== brief visual flourish at station
    if (arrivalAnimStart > 0 && mode == PigMode::ROAMING && roamState == RoamState::IDLE) {
        uint32_t elapsed = now - arrivalAnimStart;
        if (elapsed < ARRIVAL_ANIM_MS) {
            float t = (float)elapsed / (float)ARRIVAL_ANIM_MS;
            switch (currentStation) {
                case Station::AT_LAPTOP:
                    // 3 rapid key flashes on CRT area
                    if (elapsed < 200) {
                        int flashIdx = (int)(elapsed / 66);
                        int flashX = kR1_KeybX + 4 + flashIdx * 14;
                        canvas.fillRect(flashX, kR1_KeybY + 2, 8, 2, RP::STRUCT);
                    }
                    break;
                case Station::ON_SOFA:
                    // Cushion compression travels outward from the landing.
                    if (elapsed < 420) {
                        int spread = ((int)(elapsed / 105u) * p) & ~1;
                        int seamY = kR2_SofaY + kR2_SofaH - p;
                        canvas.fillRect(drawX + 12 - spread, seamY,
                                        p * 2, p, RP::D_STRUCT);
                        canvas.fillRect(drawX + kPigW - 16 + spread, seamY,
                                        p * 2, p, RP::D_STRUCT);
                    }
                    break;
                case Station::AT_WINDOW:
                    // A small breath bloom lands on the cold pane, then clears.
                    if (elapsed < 360) {
                        int bloom = (int)(elapsed / 120u);
                        int bx = kR2_WindowX + kR2_WindowW / 2 + bloom * p;
                        int by = kR2_WindowY + kR2_WindowH - 24 - bloom * p;
                        canvas.fillRect(bx & ~1, by & ~1, p, p, RP::DUST);
                        if (bloom > 0)
                            canvas.fillRect((bx - p * 2) & ~1, (by + p) & ~1,
                                            p, p, RP::SHAFT);
                    }
                    break;
                case Station::COOKING:
                    // Bowl contact: paired rim ticks and one warm steam kick.
                    if (elapsed < 360) {
                        int beat = (int)(elapsed / 90u) & 1;
                        canvas.fillRect(kR3_BowlX - p * (beat + 1), kR3_BowlY,
                                        p, p, RP::WARM);
                        canvas.fillRect(kR3_BowlX + kR3_BowlW + p * beat,
                                        kR3_BowlY, p, p, RP::FLUOR);
                    }
                    break;
                case Station::IN_BED:
                    // The pod quilt settles in two restrained outward ripples.
                    if (elapsed < 420) {
                        int spread = ((int)(elapsed / 140u) * p * 2) & ~1;
                        int qy = kR3_PodY + kR3_PodH - 12;
                        canvas.fillRect(kR3_PodX + kR3_PodW / 2 - 8 - spread,
                                        qy, p * 2, p, RP::D_WALL_NEAR);
                        canvas.fillRect(kR3_PodX + kR3_PodW / 2 + 6 + spread,
                                        qy, p * 2, p, RP::D_WALL_NEAR);
                    }
                    break;
                case Station::AT_ANTENNA:
                    // Nearest antenna LED flash
                    if (elapsed < 300)
                        canvas.fillRect(kR4_AntennaX + 16, kFloorY - 162, kPigPX, kPigPX, RP::FLUOR);
                    break;
                case Station::ON_LEDGE:
                    // Wind gust (3 dust particles sweeping right)
                    if (elapsed < 400) {
                        for (int i = 0; i < 3; i++) {
                            int gx = (drawX + kPigW + (int)(t * 40.0f) + i * 12) & ~1;
                            int gy = (drawY + 10 + i * 8) & ~1;
                            if (gx < SCREEN_WIDTH - 4)
                                canvas.fillRect(gx, gy, p, p, RP::DUST);
                        }
                    }
                    break;
                case Station::AT_TERMINAL:
                    // Screen flash
                    if (elapsed < 100)
                        canvas.fillRect(kR5_TermX + parallaxMid + 2,
                                        kR5_TermY + 2,
                                        kR5_TermW - 4, kR5_TermH - 4, RP::FLUOR);
                    break;
                case Station::AT_BOOTH:
                    // Sign flicker on arrival
                    if (elapsed < 200)
                        drawLightPool(canvas, RP::NEON,
                                      kR5_NeonX + parallaxFar - 8,
                                      kR5_NeonY + kR5_NeonH + 2,
                                      kR5_NeonW + 16, 6, 50, 88773);
                    break;
                case Station::IN_BATH:
                    // drawBathJumpSplash() already follows the physical water
                    // contact through landing. Do not restart a second,
                    // unrelated hot/neon splash after the mount completes.
                    break;
                default:
                    break;
            }
        } else {
            arrivalAnimStart = 0;  // animation finished
        }
    }

    // ==[ IDLE FIDGETS ]== shared update-owned micro-behaviors during station idle
    if (isStationFidgetActive(now)) {
        uint32_t fidgetAge = now - fidgetStart;
        float ft = (float)fidgetAge / (float)FIDGET_MS;
        switch (currentStation) {
                case Station::AT_LAPTOP:
                    if (fidgetAge < 180 || (fidgetAge >= 320 && fidgetAge < 500)) {
                        int key = (fidgetAge < 240) ? 0 : 1;
                        canvas.fillRect(kR1_KeybX + 10 + key * 16,
                                        kR1_KeybY + p, p * 2, p, RP::FLUOR);
                    }
                    break;
                case Station::ON_SOFA:
                    if (fidgetAge < 420) {
                        int drift = ((int)(fidgetAge / 140u) & 1) * p;
                        canvas.fillRect(drawX + 14 + drift,
                                        kR2_SofaY + kR2_SofaH - p,
                                        p * 3, p, RP::D_STRUCT);
                    }
                    break;
                case Station::AT_WINDOW:
                    if (fidgetAge < 360) {
                        int tapX = kR2_WindowX + kR2_WindowW / 2;
                        int tapY = kR2_WindowY + kR2_WindowH - 30;
                        canvas.fillRect(tapX, tapY, p, p * 2, RP::FLUOR);
                        if (fidgetAge > 160)
                            canvas.fillRect(tapX + p, tapY - p, p, p, RP::DUST);
                    }
                    break;
                case Station::COOKING:
                    // Chopstick tap on bowl rim
                    if (fidgetAge < 200 || (fidgetAge > 300 && fidgetAge < 500)) {
                        int tapX = kR3_BowlX + kR3_BowlW - 2;
                        int tapY = kR3_BowlY - 2;
                        canvas.fillRect(tapX, tapY, p, p, RP::STRUCT);
                    }
                    break;
                case Station::ON_LEDGE:
                    // Leg kick (visual hint — small dot below pig oscillates)
                    {
                        int kickPhase = (int)(ft * 3.0f);
                        int hipX = drawX + kPigW / 2;
                        int kickY = drawY + kPigH - p + (kickPhase & 1) * p * 2;
                        canvas.fillRect(hipX, drawY + kPigH - p * 2,
                                        p, p * 2, fg);
                        canvas.fillRect(hipX, kickY & ~1, p * 2, p, fg);
                    }
                    break;
                case Station::IN_BED:
                    if (fidgetAge < 460) {
                        int twitch = ((int)(fidgetAge / 115u) & 1) * p;
                        canvas.fillRect(kR3_PodX + kR3_PodW - 28 + twitch,
                                        kR3_PodY + kR3_PodH - 14,
                                        p * 2, p, RP::D_WALL_NEAR);
                    }
                    break;
                case Station::AT_ANTENNA:
                    if (fidgetAge < 420) {
                        int rung = (int)(fidgetAge / 105u);
                        canvas.fillRect(kR4_AntennaX + 12,
                                        kFloorY - 154 - rung * p * 2,
                                        p, p,
                                        roomMood.alertLevel >= 3 ? RP::SPARK : RP::FLUOR);
                    }
                    break;
                case Station::AT_TERMINAL:
                    if (fidgetAge < 520) {
                        int scanY = kR5_TermY + 4 +
                            ((int)(fidgetAge / 80u) * p) % (kR5_TermH - 8);
                        canvas.fillRect(kR5_TermX + parallaxMid + 4, scanY,
                                        kR5_TermW - 8, p, RP::CRT);
                    }
                    break;
                case Station::AT_BOOTH:
                    if (fidgetAge < 420) {
                        int glintX = kR5_BoothX + 12 +
                            ((int)(fidgetAge / 105u) * p * 2);
                        canvas.fillRect(glintX, kR5_BoothY + 8,
                                        p, p, RP::WARM);
                    }
                    break;
                case Station::IN_BATH:
                    if (fidgetAge < 520) {
                        int spread = (int)(fidgetAge / 130u) * p * 2;
                        canvas.fillRect(kR6_BathPigX + 28 - spread,
                                        kR6_TubWaterY, p, p, RP::WARM);
                        canvas.fillRect(kR6_BathPigX + 42 + spread,
                                        kR6_TubWaterY, p, p, RP::NEON);
                    }
                    break;
            default:
                break;
        }
    }
}

static constexpr uint32_t BOOT_WD_ARRIVAL_SPEEDUP = 2;
static constexpr uint32_t BOOT_WD_ARRIVAL_MS =
    ROOM3_CAR_TURN_END_MS / BOOT_WD_ARRIVAL_SPEEDUP;
static constexpr uint32_t BOOT_WD_ROOF_HOLD_MS = 900;
static constexpr uint32_t BOOT_WD_BOARD_MS =
    WD_WALK_MS + WD_JUMP_MS + WD_IMPACT_MS + BOOT_WD_ROOF_HOLD_MS;
static constexpr float BOOT_WD_START_X = 72.0f;
static constexpr float BOOT_WD_START_Y = (float)kFloorPigY;
static constexpr float BOOT_WD_LAND_X = WD_CAR_ROOF_PIG_X;
static constexpr float BOOT_WD_LAND_Y = WD_CAR_ROOF_PIG_Y;
static constexpr int BOOT_WD_BAR_X = 28;
static constexpr int BOOT_WD_BAR_Y = 216;
static constexpr int BOOT_WD_BAR_W = 264;
static constexpr int BOOT_WD_BAR_H = 4;
static constexpr int16_t BOOT_WD_PORTAL_X = 108;
static constexpr int16_t BOOT_WD_PORTAL_Y = 168;

static void drawBootWardriveLoadingBar(M5Canvas& canvas, uint32_t now,
                                       float loadingT) {
    if (loadingT < 0.0f) loadingT = 0.0f;
    else if (loadingT > 1.0f) loadingT = 1.0f;
    uint16_t barTrack = Display::lerpColor565(RP::BG, RP::WALL_MID, 0.42f);
    uint16_t barFill = Display::lerpColor565(RP::FLUOR, RP::NEON, 0.35f);
    uint16_t barSpec = Display::lerpColor565(Display::getColorFG(), RP::FLUOR, 0.45f);

    int fillW = (int)lroundf((float)BOOT_WD_BAR_W * loadingT);
    fillW &= ~3;

    canvas.fillRect(BOOT_WD_BAR_X + 8, BOOT_WD_BAR_Y + BOOT_WD_BAR_H + 4,
                    BOOT_WD_BAR_W - 16, 4, Display::lerpColor565(barTrack, RP::BG, 0.35f));
    canvas.fillRect(BOOT_WD_BAR_X, BOOT_WD_BAR_Y, BOOT_WD_BAR_W, BOOT_WD_BAR_H, barTrack);
    if (fillW > 0) {
        canvas.fillRect(BOOT_WD_BAR_X, BOOT_WD_BAR_Y, fillW, BOOT_WD_BAR_H, barFill);
        int specX = BOOT_WD_BAR_X + fillW - 4;
        if (specX < BOOT_WD_BAR_X) specX = BOOT_WD_BAR_X;
        canvas.fillRect(specX, BOOT_WD_BAR_Y, 4, BOOT_WD_BAR_H, barSpec);
    }

    int pulseW = 28 + (int)lroundf(loadingT * 52.0f);
    pulseW &= ~3;
    if (pulseW > 0) {
        int pulseX = BOOT_WD_BAR_X + 8 +
            (((int)(now / 28u) % (BOOT_WD_BAR_W + 24)) - 12);
        pulseX &= ~3;
        if (pulseX < BOOT_WD_BAR_X + fillW) {
            if (pulseX < BOOT_WD_BAR_X) pulseX = BOOT_WD_BAR_X;
            int drawW = pulseW;
            if (pulseX + drawW > BOOT_WD_BAR_X + fillW) drawW = (BOOT_WD_BAR_X + fillW) - pulseX;
            drawW &= ~3;
            if (drawW > 0) {
                canvas.fillRect(pulseX, BOOT_WD_BAR_Y, drawW, BOOT_WD_BAR_H,
                                Display::screenBlend565(barFill, barSpec, 56));
            }
        }
    }
}

static void drawBootWardriveSkyTraffic(M5Canvas& canvas, uint32_t now, int horizonY,
                                       uint16_t bodyFar, uint16_t bodyNear,
                                       uint16_t headCol, uint16_t tailCol) {
    struct BootSkyTrafficLane {
        int yOff;
        int dir;
        uint32_t period;
        int bodyLen;
    };
    static constexpr BootSkyTrafficLane lanes[] = {
        { 18, +1, 7200, 4 },
        { 34, -1, 6100, 8 },
        { 52, +1, 4700, 12 },
        { 64, -1, 3900, 16 }
    };

    for (size_t i = 0; i < sizeof(lanes) / sizeof(lanes[0]); i++) {
        const auto& lane = lanes[i];
        float t = (float)((now + (uint32_t)i * 1700u) % lane.period) / (float)lane.period;
        float cx = (lane.dir > 0) ? t : (1.0f - t);
        int carX = (96 + (int)(cx * (float)(SCREEN_WIDTH - 108 - lane.bodyLen))) & ~3;
        int carY = (kRoomY + lane.yOff) & ~3;
        if (carY >= horizonY - 8) continue;
        uint16_t body = (lane.bodyLen <= 8) ? bodyFar : bodyNear;
        canvas.fillRect(carX, carY, lane.bodyLen, kRoomPX, body);
        if (lane.dir > 0) {
            canvas.fillRect(carX + lane.bodyLen, carY, kRoomPX, kRoomPX, headCol);
            canvas.fillRect(carX - kRoomPX, carY, kRoomPX, kRoomPX, tailCol);
        } else {
            canvas.fillRect(carX - kRoomPX, carY, kRoomPX, kRoomPX, headCol);
            canvas.fillRect(carX + lane.bodyLen, carY, kRoomPX, kRoomPX, tailCol);
        }
        int trailDir = -lane.dir;
        for (int ti = 1; ti <= ((lane.bodyLen >= 12) ? 2 : 1); ti++) {
            int tx = (carX + trailDir * (lane.bodyLen + ti * 4)) & ~3;
            if (tx > 0 && tx < SCREEN_WIDTH - kRoomPX) {
                uint16_t trailCol = (ti == 1)
                    ? Display::lerpColor565(body, RP::DUST, 0.35f)
                    : Display::lerpColor565(body, RP::BG, 0.55f);
                canvas.fillRect(tx, carY, kRoomPX, kRoomPX, trailCol);
            }
        }
    }
}

static void drawBootWardriveRainSweep(M5Canvas& canvas, uint32_t now, int horizonY,
                                      uint16_t streakCol, uint16_t mistCol) {
    drawAmbientRainStreaks(canvas, now, 152, horizonY + kRoomPX,
                           SCREEN_WIDTH - 152,
                           kFloorY - horizonY - kRoomPX * 3,
                           streakCol, mistCol);
}

static void drawBootWardriveBackdrop(M5Canvas& canvas, uint32_t now, float loadingT) {
    uint16_t bootSkyTop = Display::lerpColor565(RP::BG, RP::DEEP, 0.18f);
    uint16_t bootSkyMid = Display::lerpColor565(RP::BG, RP::NEON, 0.10f);
    uint16_t bootSkyBot = Display::lerpColor565(RP::DEEP, RP::WALL_FAR, 0.42f);
    uint16_t bootGridMain = Display::lerpColor565(RP::NEON, RP::FLUOR, 0.45f);
    uint16_t bootGridDim = Display::lerpColor565(RP::BG, RP::WALL_MID, 0.50f);
    uint16_t bootSunHot = Display::lerpColor565(RP::WARM, RP::NEON, 0.28f);
    uint16_t bootSunWarm = Display::lerpColor565(RP::WARM, RP::FILL, 0.35f);
    uint16_t bootFloor = RP::BG;
    uint16_t bootStrip = Display::lerpColor565(RP::STRUCT, RP::NEON, 0.22f);

    const int px = kRoomPX;
    const int horizonY = (kFloorY - 52) & ~3;
    const int vanishX = ((int)WD_CAR_CENTER_X - 18) & ~3;

    for (int y = 0; y < SCREEN_HEIGHT; y += px) {
        float t = (float)y / (float)(SCREEN_HEIGHT - px);
        uint16_t rowColor = (t < 0.55f)
            ? Display::lerpColor565(bootSkyTop, bootSkyMid, t / 0.55f)
            : Display::lerpColor565(bootSkyMid, bootSkyBot, (t - 0.55f) / 0.45f);
        canvas.fillRect(0, y, SCREEN_WIDTH, px, rowColor);
        if (y < horizonY &&
            ((((y / px) + (int)(now / 360u)) & 7) == 0)) {
            // Sparse sky-cell drift reads as atmosphere; a full-width band
            // made the entire tableau flash every 150ms.
            int phaseX = (int)((now / 180u) & 7u) * px;
            for (int x = phaseX; x < SCREEN_WIDTH; x += 32) {
                if ((wallHash(x, y, 0xB005u) & 3u) != 0u)
                    canvas.fillRect(x, y, px * 2, px,
                                    Display::lerpColor565(rowColor, bootFloor, 0.20f));
            }
        }
    }

    {
        const int sunCX = ((int)WD_CAR_CENTER_X + 6) & ~3;
        const int sunCY = (horizonY - 18) & ~3;
        const int sunR = 40;
        for (int sy = -sunR; sy <= sunR; sy += px) {
            int yy = (sunCY + sy) & ~3;
            if (yy < 8 || yy > horizonY + 8) continue;
            float ny = (float)sy / (float)sunR;
            float span = 1.0f - ny * ny;
            if (span <= 0.0f) continue;
            int halfW = ((int)(sqrtf(span) * (float)sunR)) & ~3;
            if (halfW <= 0) continue;
            if ((((sy + sunR) / px) & 1) != 0 && yy > sunCY - 8) continue;
            float blend = (float)(yy - (sunCY - sunR)) / (float)(sunR * 2);
            uint16_t band = Display::lerpColor565(bootSunWarm, bootSunHot, blend);
            canvas.fillRect((sunCX - halfW) & ~3, yy, halfW * 2, px, band);
        }
    }

    for (int i = 0; i < 5; i++) {
        int barX = (20 + i * 22 + (int)(sinf((float)now * 0.0017f + (float)i) * 3.0f)) & ~3;
        int barH = 36 + i * 14;
        int barY = (horizonY - barH - 20 + (i & 1) * 6) & ~3;
        uint16_t barColor = (i & 1) ? bootSunHot : bootStrip;
        canvas.fillRect(barX, barY, px, barH, Display::lerpColor565(bootFloor, barColor, 0.55f));
    }

    canvas.fillRect(0, horizonY, SCREEN_WIDTH, px, bootGridMain);
    canvas.fillRect(0, horizonY + px, SCREEN_WIDTH, px,
                    Display::lerpColor565(bootGridMain, bootFloor, 0.55f));

    for (int gy = horizonY + px * 2, idx = 0; gy < SCREEN_HEIGHT; gy += px * 2, idx++) {
        float depth = (float)(gy - horizonY) / (float)(SCREEN_HEIGHT - horizonY);
        uint16_t lineColor = Display::lerpColor565(bootGridMain, bootGridDim, depth * 0.8f);
        int y = gy + (int)(depth * depth * 18.0f);
        y &= ~3;
        if (y >= SCREEN_HEIGHT) break;
        int glowW = SCREEN_WIDTH - (int)(depth * 28.0f);
        int glowX = (SCREEN_WIDTH - glowW) / 2;
        if (((idx + (int)(now / 220u)) & 3) == 0) {
            canvas.fillRect(glowX, y, glowW, px, Display::lerpColor565(lineColor, bootSunHot, 0.25f));
        } else {
            canvas.fillRect(glowX, y, glowW, px, lineColor);
        }
    }

    for (int baseX = -40; baseX <= SCREEN_WIDTH + 40; baseX += 28) {
        for (int y = horizonY; y < SCREEN_HEIGHT; y += px) {
            float t = (float)(y - horizonY) / (float)(SCREEN_HEIGHT - horizonY);
            int x = vanishX + (int)((float)(baseX - vanishX) * t);
            if (x < -px || x >= SCREEN_WIDTH) continue;
            uint16_t c = Display::lerpColor565(bootGridMain, bootGridDim, t * 0.75f);
            canvas.fillRect(x & ~3, y, px, px, c);
        }
    }

    for (int i = 0; i < 8; i++) {
        bool nearMote = i >= 5;
        uint32_t speedDiv = nearMote ? 9u : 18u;
        int sx = (12 + (int)((now / speedDiv + i * 47u) %
                             (SCREEN_WIDTH + 32u)) - 16) & ~3;
        int sy = (20 + i * 15 +
                  (int)(sinf((float)now * 0.0013f + (float)i) *
                        (nearMote ? 5.0f : 3.0f))) & ~3;
        if (sx >= 0 && sx < SCREEN_WIDTH - px && sy > 8 && sy < horizonY - 12) {
            uint16_t mote = nearMote
                ? Display::lerpColor565(bootSunWarm, bootSunHot, 0.35f)
                : Display::lerpColor565(bootSkyMid, bootSunWarm, 0.28f);
            canvas.fillRect(sx, sy, nearMote ? px * 2 : px, px, mote);
        }
    }

    drawBootWardriveSkyTraffic(canvas, now, horizonY,
                               Display::lerpColor565(bootFloor, bootGridDim, 0.72f),
                               Display::lerpColor565(bootGridMain, bootSunWarm, 0.20f),
                               bootSunWarm, bootSunHot);
    drawBootWardriveRainSweep(canvas, now, horizonY,
                              Display::lerpColor565(bootGridMain, bootSunWarm, 0.22f),
                              Display::lerpColor565(bootGridDim, bootFloor, 0.18f));

    canvas.fillRect(0, kFloorY + px, SCREEN_WIDTH, SCREEN_HEIGHT - (kFloorY + px),
                    Display::lerpColor565(bootFloor, bootGridDim, 0.35f));
    drawBootWardriveLoadingBar(canvas, now, loadingT);
}

static float bootWardriveCarDropY(uint32_t elapsedSinceImpactMs) {
    float bt = (float)elapsedSinceImpactMs / 800.0f;
    if (bt >= 1.0f) return 0.0f;
    return 10.0f * sinf(bt * 3.14159f * 3.0f) * expf(-bt * 4.0f);
}

static uint32_t bootWardrivePreparedNow = UINT32_MAX;
static uint32_t bootWardrivePreparedElapsed = UINT32_MAX;

static void setupBootWardriveSceneState(uint32_t now, uint32_t introElapsedMs) {
    mode = PigMode::ROAMING;
    roamState = RoamState::IDLE;
    avatarState = AvatarState::NEUTRAL;
    currentRoom = 3;
    currentStation = Station::ON_LEDGE;
    walkTargetRoom = 3;
    walkTargetStation = Station::ON_LEDGE;
    carryingCup = false;
    windowCigLit = false;
    wallBreakActive = false;
    rearCinePhase = RearCinematicPhase::INACTIVE;
    rearCineOverrideRear = false;
    faceTimer.blinking = false;
    faceTimer.earTwitching = false;
    arrivalAnimStart = 0;
    fidgetStart = 0;
    fidgetRunning = false;
    characterFidget = CharacterFidget::NOTICE;
    nextFidgetTime = 0xFFFFFFFFu;
    room3CinematicCarRunning = false;
    pigX = BOOT_WD_START_X;
    pigY = BOOT_WD_START_Y;
    faceRight = true;

    wdCinePhase = WDCinePhase::WD_WAIT;
    wdCarForceStart = false;
    wdMountPhase = WDMountPhase::IDLE;
    wdMountStart = 0;
    wdImpactStart = 0;
    clearWDReturnState();

    carState.initialized = true;
    carState.active = true;
    carState.lastDrawMs = now;
    carState.wdMode = false;
    carState.wdReturnMode = false;
    carState.wdPigMounted = false;
    carState.wdCarDropY = 0.0f;

    if (introElapsedMs < BOOT_WD_ARRIVAL_MS) {
        uint32_t arrivalElapsed = introElapsedMs * BOOT_WD_ARRIVAL_SPEEDUP;
        if (arrivalElapsed > ROOM3_CAR_TURN_END_MS) arrivalElapsed = ROOM3_CAR_TURN_END_MS;
        carState.startMs = now - arrivalElapsed;
        return;
    }

    uint32_t boardElapsed = introElapsedMs - BOOT_WD_ARRIVAL_MS;
    if (boardElapsed > BOOT_WD_BOARD_MS) boardElapsed = BOOT_WD_BOARD_MS;

    carState.startMs = now - ROOM3_CAR_TURN_END_MS;
    carState.wdMode = true;
    faceRight = true;

    if (boardElapsed < WD_WALK_MS) {
        wdMountPhase = WDMountPhase::WALKING;
        wdMountStart = now - boardElapsed;
        float st = smootherstep((float)boardElapsed / (float)WD_WALK_MS);
        pigX = BOOT_WD_START_X + (WD_WALK_TARGET_X - BOOT_WD_START_X) * st;
        pigY = BOOT_WD_START_Y;
        walkLegDist = fabsf(pigX - BOOT_WD_START_X);
        return;
    }

    uint32_t jumpElapsed = boardElapsed - WD_WALK_MS;
    if (jumpElapsed < WD_JUMP_MS) {
        wdMountPhase = WDMountPhase::JUMPING;
        wdMountStart = now - jumpElapsed;
        float jt = (float)jumpElapsed / (float)WD_JUMP_MS;
        float finalT = (jt > 0.62f) ? smootherstep((jt - 0.62f) / 0.38f) : 0.0f;
        float jumpTargetX = WD_CAR_CENTER_X + (BOOT_WD_LAND_X - WD_CAR_CENTER_X) * finalT;
        float jumpTargetY = WD_CAR_ROOF_Y + (BOOT_WD_LAND_Y - WD_CAR_ROOF_Y) * finalT;
        pigX = WD_WALK_TARGET_X + (jumpTargetX - WD_WALK_TARGET_X) * jt;
        pigY = BOOT_WD_START_Y + (jumpTargetY - BOOT_WD_START_Y) * jt
               - WD_JUMP_HEIGHT * sinf(jt * 3.14159f);
        return;
    }

    uint32_t impactElapsed = jumpElapsed - WD_JUMP_MS;
    if (impactElapsed < WD_IMPACT_MS) {
        wdMountPhase = WDMountPhase::IMPACT;
        wdMountStart = now - impactElapsed;
        wdImpactStart = now - impactElapsed;
        carState.wdPigMounted = true;
        carState.wdCarDropY = bootWardriveCarDropY(impactElapsed);
        float roofY = BOOT_WD_LAND_Y + carState.wdCarDropY;
        float impactT = (float)impactElapsed / (float)WD_IMPACT_MS;
        pigX = BOOT_WD_LAND_X;
        pigY = roofY + 2.0f * sinf(impactT * 3.14159f);
        return;
    }

    uint32_t roofHoldElapsed = impactElapsed - WD_IMPACT_MS;
    uint32_t bounceElapsed = WD_IMPACT_MS + roofHoldElapsed;
    wdImpactStart = now - bounceElapsed;
    carState.wdPigMounted = true;
    carState.wdCarDropY = bootWardriveCarDropY(bounceElapsed);
    float roofY = BOOT_WD_LAND_Y + carState.wdCarDropY;

    wdMountPhase = WDMountPhase::ROOF_RIDE;
    wdMountStart = now - roofHoldElapsed;
    pigX = BOOT_WD_LAND_X;
    pigY = roofY;
}

static void prepareBootWardriveSceneState(uint32_t now,
                                          uint32_t introElapsedMs) {
    if (bootWardrivePreparedNow == now &&
        bootWardrivePreparedElapsed == introElapsedMs) return;
    bootWardrivePreparedNow = now;
    bootWardrivePreparedElapsed = introElapsedMs;
    RP::update();
    setupBootWardriveSceneState(now, introElapsedMs);
    calcParallax();
}

static void drawBootWardrivePigLayer(M5Canvas& canvas, uint32_t now) {
    uint16_t pigFg = Display::getColorFG();
    uint16_t pigBg = RP::BG;
    Avatar::setState(avatarState);

    if (!isWDPigInsideCar()) {
        bool isMoving = (wdMountPhase == WDMountPhase::WALKING) || isRoamingMoveState();
        bool useRearView = shouldUseRearViewInRoaming();
        MenuPigRenderOptions options;
        if (isWDPigSneakingIntoCabin()) {
            int clipBottom = ((int)(WD_CAR_CANOPY_Y + carState.wdCarDropY)) & ~1;
            int clipTop = (int)UIMeasurements::kTopBarH;
            int clipH = clipBottom - clipTop;
            if (clipH > 0) {
                canvas.setClipRect(0, clipTop, SCREEN_WIDTH, clipH);
                drawPig(canvas, pigFg, pigBg, now, isMoving, useRearView, options);
                canvas.clearClipRect();
            }
        } else {
            drawPig(canvas, pigFg, pigBg, now, isMoving, useRearView, options);
        }
    }
}

void drawBootWardriveSceneBase(M5Canvas& canvas, uint32_t now, uint32_t introElapsedMs,
                               float loadingT) {
    Avatar::setRenderTimeOverride(now);
    prepareBootWardriveSceneState(now, introElapsedMs);

    drawBootWardriveBackdrop(canvas, now, loadingT);
    drawRoom3CinematicCar(canvas, now);
    Avatar::clearRenderTimeOverride();
}

void drawBootWardrivePigOverlay(M5Canvas& canvas, uint32_t now, uint32_t introElapsedMs) {
    Avatar::setRenderTimeOverride(now);
    prepareBootWardriveSceneState(now, introElapsedMs);
    drawBootWardrivePigLayer(canvas, now);
    Avatar::clearRenderTimeOverride();
}

void getBootWardriveTeleportAnchors(int16_t& sourceX, int16_t& sourceY,
                                    int16_t& portalX, int16_t& portalY) {
    sourceX = (int16_t)(((int)(BOOT_WD_LAND_X + 36.0f)) & ~1);
    sourceY = (int16_t)(((int)(BOOT_WD_LAND_Y + 21.0f)) & ~1);
    portalX = BOOT_WD_PORTAL_X;
    portalY = BOOT_WD_PORTAL_Y;
    snapPortalAnchor(sourceX, sourceY);
    snapPortalAnchor(portalX, portalY);
}

uint32_t getBootWardriveSceneDurationMs() {
    return BOOT_WD_ARRIVAL_MS + BOOT_WD_BOARD_MS;
}

uint32_t getBootWardriveJumpStartMs() {
    return BOOT_WD_ARRIVAL_MS + WD_WALK_MS;
}

// ==[ ENTER ]==

static void resetMenuPigEnterState(uint32_t now) {
    returnToHelperAfterBath = false;
    eventHold = false;
    eventHoldStarted = 0;
    roamState = RoamState::IDLE;
    carryingCup = false;
    windowCigLit = false;
    portalJumpActive = false;
    portalJumpStart = 0;
    rearCinePhase = RearCinematicPhase::INACTIVE;
    rearCineStart = 0;
    rearCinePhaseStart = 0;
    rearTailOffsetY = 0;
    rearEyePhase = 0;
    rearCineOverrideRear = false;
    wdCinePhase = WDCinePhase::NONE;
    wdCineStart = 0;
    wdCarForceStart = false;
    wdMountPhase = WDMountPhase::IDLE;
    wdMountStart = 0;
    wdImpactStart = 0;
    clearWDReturnState();
    room3CinematicCarRunning = false;
    carState.active = false;
    carState.startMs = 0;
    carState.lastDrawMs = 0;
    carState.wdMode = false;
    carState.wdReturnMode = false;
    carState.wdPigMounted = false;
    carState.wdCarDropY = 0.0f;
    teleportPhase = TeleportPhase::NONE;
    teleportContext = TeleportContext::ROOM_TO_ROOM;
    catTeleportActive = false;
    catTeleportLandingPending = false;
    useTeleportTransition = false;
    walkFromX = walkFromY = 0.0f;
    walkToX = walkToY = 0.0f;
    jumpFromX = jumpToX = 0.0f;
    jumpFromY = jumpToY = 0.0f;
    jumpSquashPx = 0.0f;
    walkStart = 0;
    jumpStart = 0;
    transStart = 0;
    transFromX = 0.0f;
    transFromRoom = currentRoom;
    transToRoom = currentRoom;
    roomTransStart = 0;
    roomSlideDir = 0;
    stationStart = now;
    stationStayStart = now;
    lastStationDwellTick = now;
    stationDuration = 0;
    settleStart = 0;
    narratorBubbleText[0] = '\0';
    Barman::clearDialogue();
    lastNarratorBubble = now;
    noirPass.active = false;
    noirPass.startMs = 0;
    noirPass.durationMs = 1100;
    noirPass.nextTriggerMs = now + randomRange(6000, 12000);
    noirPass.dir = 1;
    noirPass.lastPigX = pigX;
    colorEvent = {};
    cupSteamPrimed = false;
    cupSteamLagX = cupSteamLagY = 0;
    lastHelperIdx = -1;
    neonCycleStart = now;
    dripCycleStart = now;
    sparkActive = false;
    nextSparkTime = now + randomRange(SPARK_MIN_MS, SPARK_MAX_MS);
    podLedStart = now;
    podLedOn = true;
    lastLaptopBlink = now;
    lastLaptopBubble = now;
    laptopLineIdx = (uint8_t)(esp_random() % LAPTOP_LINE_COUNT);
    laptopScreenOn = true;
    wallBreakActive = false;
    nextWallBreak = now + randomRange(WALL_BREAK_MIN_MS, WALL_BREAK_MAX_MS);
    rippleRadius = 0;
    roomDripStart = now;
    resetIdleTimers(now);
    resetCharacterFidget(now);
    resetSleepBubble(now);
    PancettaCat::reset(now);
    CatBehavior::reset(now, esp_random() ^ now ^ 0xC47B0A7Du);
    catBehaviorDecision = {};
    DefhogTerminal::init();
    NoirJazz::init();
}

void enter() {
    uint32_t now = millis();
    roomProgressLastRefresh = 0;
    fullCircuitClaimed = Achievements::has(Achievement::FULL_CIRCUIT);
    stakeoutClaimed = Achievements::has(Achievement::STAKEOUT);
    mode = PigMode::ON_BENCH;
    pigX = (float)kHelperPigX;
    pigY = (float)kHelperPigY;
    faceRight = false;  // face left toward menu
    avatarState = AvatarState::NEUTRAL;
    currentRoom = 0;
    currentStation = Station::AT_LAPTOP;
    walkTargetRoom = 0;
    walkTargetStation = Station::AT_LAPTOP;
    resetMenuPigEnterState(now);
    roamState = RoamState::IDLE;
    carryingCup = false;
    windowCigLit = false;
    portalJumpActive = false;
    portalJumpStart = 0;
    rearCinePhase = RearCinematicPhase::INACTIVE;
    rearCineStart = 0;
    rearCinePhaseStart = 0;
    rearTailOffsetY = 0;
    rearEyePhase = 0;
    rearCineOverrideRear = false;
    wdCinePhase = WDCinePhase::NONE;
    wdCineStart = 0;
    wdCarForceStart = false;
    wdMountPhase = WDMountPhase::IDLE;
    wdMountStart = 0;
    wdImpactStart = 0;
    room3CinematicCarRunning = false;
    carState.wdMode = false;
    carState.wdPigMounted = false;
    carState.wdCarDropY = 0.0f;
    teleportPhase = TeleportPhase::NONE;
    teleportContext = TeleportContext::ROOM_TO_ROOM;
    catTeleportActive = false;
    catTeleportLandingPending = false;
    noirPass.active = false;
    noirPass.startMs = 0;
    noirPass.durationMs = 1100;
    noirPass.nextTriggerMs = now + randomRange(6000, 12000);
    noirPass.dir = 1;
    noirPass.lastPigX = pigX;
    colorEvent = {};
    cupSteamPrimed = false;
    cupSteamLagX = cupSteamLagY = 0;
    lastHelperIdx = -1;
    // Animation state init
    neonCycleStart = now;
    dripCycleStart = now;
    sparkActive = false;
    nextSparkTime = now + randomRange(SPARK_MIN_MS, SPARK_MAX_MS);
    podLedStart = now;
    podLedOn = true;
    lastLaptopBlink = now;
    lastLaptopBubble = now;
    laptopLineIdx = (uint8_t)(esp_random() % LAPTOP_LINE_COUNT);
    laptopScreenOn = true;
    wallBreakActive = false;
    nextWallBreak = now + randomRange(WALL_BREAK_MIN_MS, WALL_BREAK_MAX_MS);
    rippleRadius = 0;
    roomDripStart = now;
    resetIdleTimers(now);
    resetCharacterFidget(now);
    resetSleepBubble(now);
    DefhogTerminal::init();

}

void resetCaptureState() {
    returnToHelperAfterBath = false;
    mode = PigMode::ON_BENCH;
    pigX = 124.0f;
    pigY = 116.0f;
    faceRight = true;
    avatarState = AvatarState::NEUTRAL;
    currentRoom = 0;
    currentStation = Station::AT_LAPTOP;
    roamState = RoamState::IDLE;
    walkFromX = walkFromY = 0.0f;
    walkToX = walkToY = 0.0f;
    walkToFaceRight = true;
    walkStart = 0;
    walkTargetStation = Station::AT_LAPTOP;
    walkTargetRoom = 0;
    jumpFromX = jumpToX = 0.0f;
    jumpFromY = jumpToY = 0.0f;
    jumpSquashPx = 0.0f;
    jumpStart = 0;
    transStart = 0;
    transFromX = 0.0f;
    transFromRoom = 0;
    transToRoom = 0;
    roomTransStart = 0;
    roomSlideDir = 0;
    stationStart = 0;
    stationDuration = 0;
    bathCycleStartMs = 0;
    bathCyclePlanned = false;
    settleStart = 0;
    faceTimer.reset();
    fidgetStart = 0;
    fidgetRunning = false;
    characterFidget = CharacterFidget::NOTICE;
    nextFidgetTime = 0xFFFFFFFFu;
    const uint32_t catResetNow = millis();
    PancettaCat::reset(catResetNow);
    CatBehavior::reset(catResetNow, 0xC47B0A7Du);
    catBehaviorDecision = {};
    sleepPhraseIdx = 0;
    lastSleepPhrase = 0;
    wallBreakActive = false;
    wallBreakStart = 0;
    nextWallBreak = 0;
    savedFaceRight = false;
    carryingCup = false;
    cupPickupTime = 0;
    cupSteamLagX = 0;
    cupSteamLagY = 0;
    cupSteamPrevX = 0;
    cupSteamPrevY = 0;
    cupSteamPrimed = false;
    windowCigLit = false;
    portalJumpActive = false;
    portalJumpStart = 0;
    rearCinePhase = RearCinematicPhase::INACTIVE;
    rearCineStart = 0;
    rearCinePhaseStart = 0;
    rearTailOffsetY = 0;
    rearEyePhase = 0;
    rearCineOverrideRear = false;
    wdCinePhase = WDCinePhase::NONE;
    wdCineStart = 0;
    wdCarForceStart = false;
    wdMountPhase = WDMountPhase::IDLE;
    wdMountStart = 0;
    wdImpactStart = 0;
    clearWDReturnState();
    room3CinematicCarRunning = false;
    carState = {};
    nextChairLegBurstAtMs = 0;
    chairLegBurstStartMs = 0;
    ramenSlurpRuntime = {};
    lastFootLandingFrame = -1;
    for (int i = 0; i < MAX_DUST_PUFFS; ++i) dustPuffs[i] = {};
    clearBathWetTracks();
}

void clearDebugRoamingFrame() {
    debugRoamingFrame = {};
}

bool isDebugRoamingFrameActive() {
    return debugRoamingFrame.active;
}

static bool isRoom2DebugPreset(const char* id, const char* bareName) {
    char prefixed[32];
    snprintf(prefixed, sizeof(prefixed), "ROOM2_%s", bareName);
    return equalsIgnoreCaseAscii(id, bareName) || equalsIgnoreCaseAscii(id, prefixed);
}

bool forceDebugRoamingFrame(const char* frameId) {
    const char* id = skipAsciiSpaces(frameId);
    if (!id || !id[0]) return false;

    DebugRoamingFrameOverride next = {};
    next.active = true;
    next.renderNow = 1200;

    Station nextStation = Station::COOKING;
    Station nextWalkTarget = Station::COOKING;
    RoamState nextRoamState = RoamState::IDLE;
    AvatarState nextAvatarState = AvatarState::NEUTRAL;
    bool nextFaceRight = true;
    float nextPigX = (float)kR3_CookPigX;
    float nextPigY = (float)kR3_CookPigY;
    int nextRoom = 2;
    // Bath idle presets rewind the soak clock so a single captured frame can
    // land inside a chosen dive phase. 0 leaves the cycle untouched.
    uint32_t nextBathCycleOffsetMs = 0;
    uint32_t nextJumpOffsetMs = 0;

    auto setStationPreset = [&](Station station, AvatarState state = AvatarState::NEUTRAL) {
        nextStation = station;
        nextWalkTarget = station;
        nextAvatarState = state;
        getStationPos(station, nextPigX, nextPigY, nextFaceRight);
        nextRoom = stationRoom(station);
    };

    // Deterministic plan, so preset offsets address the same dive phase on
    // every capture run without changing the live randomized planner.
    constexpr uint32_t kDebugSoakMs = 7000u;
    constexpr uint32_t kDebugPrepMs = 800u;
    constexpr uint32_t kDebugSinkMs = 1000u;
    constexpr uint32_t kDebugSubmergedMs = 2000u;
    constexpr uint32_t kDebugSinkEnd =
        kDebugSoakMs + kDebugPrepMs + kDebugSinkMs;
    constexpr uint32_t kDebugSubEnd = kDebugSinkEnd + kDebugSubmergedMs;
    constexpr uint32_t kDebugRiseEnd = kDebugSubEnd + 1000u;
    // Every bath preset lands on the same station at the same wall clock; only
    // which sub-animation clock gets rewound differs.
    auto setBathPreset = [&]() {
        setStationPreset(Station::IN_BATH, AvatarState::HAPPY);
        next.renderNow = 40000;
    };
    auto setBathPhasePreset = [&](uint32_t cycleOffsetMs) {
        setBathPreset();
        nextBathCycleOffsetMs = cycleOffsetMs;
    };
    auto setBathJumpPreset = [&](RoamState state, uint32_t jumpOffsetMs) {
        setBathPreset();
        nextRoamState = state;
        nextJumpOffsetMs = jumpOffsetMs;
        nextPigY = (float)(kR6_BathPigY - 8);
    };

    if (equalsIgnoreCaseAscii(id, "ROOM0_LAB_IDLE")) {
        setStationPreset(Station::AT_LAPTOP, AvatarState::HAPPY);
    } else if (equalsIgnoreCaseAscii(id, "ROOM0_LAB_WALK")) {
        setStationPreset(Station::AT_LAPTOP);
        nextRoamState = RoamState::WALKING_TO;
        nextPigX = (float)pigSnapX(92);
        nextPigY = (float)kFloorPigY;
        nextFaceRight = true;
    } else if (equalsIgnoreCaseAscii(id, "ROOM1_SOFA_IDLE")) {
        setStationPreset(Station::ON_SOFA, AvatarState::SLEEPY);
    } else if (equalsIgnoreCaseAscii(id, "ROOM1_WINDOW_IDLE")) {
        setStationPreset(Station::AT_WINDOW);
    } else if (equalsIgnoreCaseAscii(id, "ROOM3_ANTENNA_IDLE")) {
        setStationPreset(Station::AT_ANTENNA, AvatarState::HUNTING);
    } else if (equalsIgnoreCaseAscii(id, "ROOM3_LEDGE_IDLE")) {
        setStationPreset(Station::ON_LEDGE);
    } else if (equalsIgnoreCaseAscii(id, "ROOM4_TERMINAL_IDLE")) {
        setStationPreset(Station::AT_TERMINAL, AvatarState::HAPPY);
    } else if (equalsIgnoreCaseAscii(id, "ROOM4_BOOTH_IDLE")) {
        setStationPreset(Station::AT_BOOTH);
    } else if (equalsIgnoreCaseAscii(id, "ROOM4_BARMAN_EXIT")) {
        setStationPreset(Station::AT_BOOTH);
        next.renderNow = 23000;  // walking behind the counter toward its open end
    } else if (equalsIgnoreCaseAscii(id, "ROOM4_BARMAN_FLOOR")) {
        setStationPreset(Station::AT_BOOTH);
        next.renderNow = 27500;  // canonical full body on the foreground plane
    } else if (equalsIgnoreCaseAscii(id, "ROOM4_BARMAN_RETURN")) {
        setStationPreset(Station::AT_BOOTH);
        next.renderNow = 31500;  // climbing back through the depth planes
    } else if (equalsIgnoreCaseAscii(id, "ROOM5_BATH_IDLE")) {
        // The live planner owns a randomized temperament. Debug captures need
        // an explicit quiet-soak plan so their visible joint, smoke, water,
        // and possible microphone dance all begin from the same source state.
        setBathPhasePreset(1u);
    } else if (equalsIgnoreCaseAscii(id, "ROOM5_BATH_SINK")) {
        setBathPhasePreset(kDebugSinkEnd - kDebugSinkMs / 2u);
    } else if (equalsIgnoreCaseAscii(id, "ROOM5_BATH_SUBMERGED")) {
        setBathPhasePreset(kDebugSinkEnd + kDebugSubmergedMs / 2u);
    } else if (equalsIgnoreCaseAscii(id, "ROOM5_BATH_RISE")) {
        setBathPhasePreset(kDebugSubEnd + 1000u / 2u);
    } else if (equalsIgnoreCaseAscii(id, "ROOM5_BATH_SHAKE")) {
        setBathPhasePreset(kDebugRiseEnd + 240u);
    } else if (equalsIgnoreCaseAscii(id, "ROOM5_BATH_JUMP_IN")) {
        // Mid-splash: past the water-contact threshold in drawBathJumpSplash.
        setBathJumpPreset(RoamState::MOUNTING, (BATH_JUMP_IN_MS * 4u) / 5u);
    } else if (equalsIgnoreCaseAscii(id, "ROOM5_BATH_JUMP_OUT")) {
        setBathJumpPreset(RoamState::DISMOUNTING, BATH_JUMP_OUT_MS / 5u);
    } else if (isRoom2DebugPreset(id, "COOK_IDLE")) {
        // Default already matches.
    } else if (isRoom2DebugPreset(id, "COOK_WOBBLE_LEFT")) {
        next.cookWx = -kRoomPX;
    } else if (isRoom2DebugPreset(id, "COOK_WOBBLE_RIGHT")) {
        next.cookWx = kRoomPX;
    } else if (isRoom2DebugPreset(id, "BED_IDLE")) {
        nextStation = Station::IN_BED;
        nextWalkTarget = Station::IN_BED;
        nextAvatarState = AvatarState::SLEEPY;
        nextFaceRight = false;
        nextPigX = (float)kR3_BedPigX;
        nextPigY = (float)kR3_BedPigY;
    } else if (isRoom2DebugPreset(id, "BED_WOBBLE_LEFT")) {
        nextStation = Station::IN_BED;
        nextWalkTarget = Station::IN_BED;
        nextAvatarState = AvatarState::SLEEPY;
        nextFaceRight = false;
        nextPigX = (float)kR3_BedPigX;
        nextPigY = (float)kR3_BedPigY;
        next.bedWx = -kRoomPX;
    } else if (isRoom2DebugPreset(id, "BED_WOBBLE_RIGHT")) {
        nextStation = Station::IN_BED;
        nextWalkTarget = Station::IN_BED;
        nextAvatarState = AvatarState::SLEEPY;
        nextFaceRight = false;
        nextPigX = (float)kR3_BedPigX;
        nextPigY = (float)kR3_BedPigY;
        next.bedWx = kRoomPX;
    } else if (isRoom2DebugPreset(id, "PARALLAX_LEFT")) {
        nextStation = Station::AT_WINDOW;
        nextWalkTarget = Station::IN_BED;
        nextRoamState = RoamState::WALKING_TO;
        nextFaceRight = true;
        nextPigX = (float)pigSnapX(76);
        nextPigY = (float)kFloorPigY;
        next.overrideParallax = true;
        next.parallaxFar = -8;
    } else if (isRoom2DebugPreset(id, "PARALLAX_RIGHT")) {
        nextStation = Station::AT_WINDOW;
        nextWalkTarget = Station::COOKING;
        nextRoamState = RoamState::WALKING_TO;
        nextFaceRight = false;
        nextPigX = (float)pigSnapX(96);
        nextPigY = (float)kFloorPigY;
        next.overrideParallax = true;
        next.parallaxFar = 8;
    } else {
        return false;
    }

    clearDebugRoamingFrame();
    debugRoamingFrame = next;

    const uint32_t sceneNow = next.renderNow;
    mode = PigMode::ROAMING;
    currentRoom = nextRoom;
    walkTargetRoom = nextRoom;
    currentStation = nextStation;
    walkTargetStation = nextWalkTarget;
    roamState = nextRoamState;
    pigX = nextPigX;
    pigY = nextPigY;
    faceRight = nextFaceRight;
    avatarState = nextAvatarState;
    walkFromX = pigX;
    walkFromY = pigY;
    walkToX = pigX;
    walkToY = pigY;
    jumpFromX = jumpToX = pigX;
    jumpFromY = pigY;
    jumpToY = pigY;
    jumpSquashPx = 0.0f;
    walkStart = 0;
    jumpStart = 0;
    transStart = 0;
    transFromX = pigX;
    transFromRoom = nextRoom;
    transToRoom = nextRoom;
    roomTransStart = 0;
    roomSlideDir = 0;
    teleportPhase = TeleportPhase::NONE;
    teleportContext = TeleportContext::ROOM_TO_ROOM;
    teleportStart = 0;
    teleportSourceParticleCount = 0;
    teleportDestinationParticleCount = 0;
    catTeleportActive = false;
    catTeleportLandingPending = false;
    catTeleportSourceParticleCount = 0;
    catTeleportDestParticleCount = 0;
    useTeleportTransition = false;
    settleStart = 0;
    stationStart = sceneNow;
    stationStayStart = sceneNow;
    stationDuration = 86400000u;
    arrivalAnimStart = 0;
    nextFidgetTime = sceneNow + 86400000u;
    fidgetStart = 0;
    fidgetRunning = false;
    characterFidget = CharacterFidget::NOTICE;
    wobbleStart = 0;
    wobbleX = 0;
    wobbleY = 0;
    carryingCup = false;
    cupPickupTime = 0;
    cupSteamLagX = 0;
    cupSteamLagY = 0;
    cupSteamPrevX = 0;
    cupSteamPrevY = 0;
    cupSteamPrimed = false;
    wallBreakActive = false;
    nextChairLegBurstAtMs = 0;
    chairLegBurstStartMs = 0;
    lastFootLandingFrame = -1;
    for (int i = 0; i < MAX_DUST_PUFFS; ++i) dustPuffs[i] = {};
    clearBathWetTracks();
    roomDripStart = 0;
    rippleRadius = 0;
    rippleStart = 0;
    sparkActive = false;
    sparkStart = 0;
    nextSparkTime = sceneNow + 86400000u;
    room2LightingRuntime = {};
    ramenSlurpRuntime = {};
    noirPass = {};
    colorEvent = {};
    windowCigLit = false;
    portalJumpActive = false;
    portalJumpStart = 0;
    rearCinePhase = RearCinematicPhase::INACTIVE;
    rearCineStart = 0;
    rearCinePhaseStart = 0;
    rearTailOffsetY = 0;
    rearEyePhase = 0;
    rearCineOverrideRear = false;
    wdCinePhase = WDCinePhase::NONE;
    wdCineStart = 0;
    wdCarForceStart = false;
    wdMountPhase = WDMountPhase::IDLE;
    wdMountStart = 0;
    wdImpactStart = 0;
    clearWDReturnState();
    room3CinematicCarRunning = false;
    carState = {};
    narratorBubbleText[0] = '\0';
    Barman::clearDialogue();
    lastNarratorBubble = sceneNow;
    resetSleepBubble(sceneNow);
    DefhogTerminal::hideImmediate();
    faceTimer.reset();
    faceTimer.blinking = false;
    faceTimer.earTwitching = false;

    // Applied last: the reset block above owns the generic roaming state, while
    // these rewind the two clocks that select a bath sub-animation.
    if (nextBathCycleOffsetMs != 0u) {
        bathCycleStartMs = sceneNow - nextBathCycleOffsetMs;
        bathSoakMs = kDebugSoakMs;
        bathPrepMs = kDebugPrepMs;
        bathSinkMs = kDebugSinkMs;
        bathDiveSubmergedMs = kDebugSubmergedMs;
        bathRiseMs = 1000u;
        bathRecoverMs = 3200u;
        bathDiveDepthPx = kR6_BathDiveMaxY;
        bathDivePlanned = true;
        bathCyclePlanned = true;
    }
    if (nextJumpOffsetMs != 0u) {
        jumpStart = sceneNow - nextJumpOffsetMs;
    }
#ifdef HAMLET_SIM
    const bool catMoving = isRoamingMoveState();
    const PancettaCat::Activity catActivity =
        catMoving ? PancettaCat::Activity::FOLLOW
                  : companionCatRoomActivity(false);
    const PancettaCat::Pose catTarget =
        resolveCompanionCatTarget((int)pigX, (int)pigY, catMoving,
                                  false, catActivity, sceneNow);
    PancettaCat::forceStateForCapture(sceneNow, catTarget,
                                      catActivity, catMoving);
#endif
    return true;
}

static void drawCaptureMenuRamenHold(M5Canvas& canvas, uint32_t now,
                                     const MenuPigRenderOptions& options) {
    uint16_t fg = Display::getColorFG();
    uint16_t bg = Display::getColorBG();
    Display::PigPalette pigPalette = Display::makePigPalette(fg, bg);
    int drawX = ((int)pigX) & ~1;
    int drawY = ((int)pigY) & ~1;
    PigEyeLook eyeLook = resolveMenuPigEyeLook(false, false, Station::COOKING, true, false, now);

    drawPigShadow(canvas, drawX, drawY, now);
    Avatar::drawBodyOnly(canvas, drawX, drawY, pigPalette.bodyFill, bg,
                         AvatarState::NEUTRAL, false, faceRight,
                         true, false, eyeLook, pigPalette.detail, 'z', {}, true);
    drawSittingLegs(canvas, drawX, drawY, pigPalette.bodyFill, bg, faceRight, false, now, true, false);
    drawHeldRamenRig(canvas, pigPalette.bodyFill, bg, drawX, drawY, drawY, now, true, options);
    Avatar::drawHairsAt(canvas, (int16_t)drawX, (int16_t)drawY, 0,
                        faceRight, AvatarState::NEUTRAL, false);
}

bool renderCaptureClip(M5Canvas& canvas, const char* clipId, uint32_t now,
                       const MenuPigRenderOptions& options) {
    if (!clipId) return false;

    Avatar::setRenderTimeOverride(now);
    RP::update();

    pigX = 124.0f;
    pigY = 116.0f;
    mode = PigMode::ROAMING;
    roamState = RoamState::IDLE;
    faceRight = true;
    avatarState = AvatarState::NEUTRAL;
    currentStation = Station::ON_LEDGE;
    carryingCup = false;
    windowCigLit = false;
    wallBreakActive = false;
    rearCinePhase = RearCinematicPhase::INACTIVE;
    rearCineOverrideRear = false;
    rearTailOffsetY = 0;
    rearEyePhase = 0;
    portalJumpActive = false;
    portalJumpStart = 0;
    faceTimer.blinking = false;
    faceTimer.earTwitching = false;
    fidgetStart = 0;
    fidgetRunning = false;
    characterFidget = CharacterFidget::NOTICE;
    chairLegBurstStartMs = 0;
    nextChairLegBurstAtMs = 0;

#ifdef HAMLET_SIM
    if (strcmp(clipId, "menu_cat_runtime_sheet") == 0 ||
        strcmp(clipId, "menu_cat_locomotion_sheet") == 0 ||
        strcmp(clipId, "menu_cat_locomotion_sheet_a") == 0) {
        PancettaCat::drawRuntimeSheet(canvas, 0);
        Avatar::clearRenderTimeOverride();
        return true;
    }
    if (strcmp(clipId, "menu_cat_locomotion_sheet_b") == 0) {
        PancettaCat::drawRuntimeSheet(canvas, 1);
        Avatar::clearRenderTimeOverride();
        return true;
    }
    if (strcmp(clipId, "menu_cat_actions_sheet") == 0 ||
        strcmp(clipId, "menu_cat_actions_sheet_a") == 0) {
        PancettaCat::drawRuntimeSheet(canvas, 2);
        Avatar::clearRenderTimeOverride();
        return true;
    }
    if (strcmp(clipId, "menu_cat_actions_sheet_b") == 0) {
        PancettaCat::drawRuntimeSheet(canvas, 3);
        Avatar::clearRenderTimeOverride();
        return true;
    }
    if (strcmp(clipId, "menu_cat_actions_sheet_c") == 0) {
        PancettaCat::drawRuntimeSheet(canvas, 4);
        Avatar::clearRenderTimeOverride();
        return true;
    }
    if (strcmp(clipId, "menu_cat_actions_sheet_d") == 0) {
        PancettaCat::drawRuntimeSheet(canvas, 5);
        Avatar::clearRenderTimeOverride();
        return true;
    }
    if (strcmp(clipId, "menu_cat_binary_signal") == 0) {
        PancettaCat::Pose catPose = {
            148, (int16_t)PancettaCat::originYForSupport(232), true,
        };
        PancettaCat::forceSignalForCapture(now, 0);
        PigLight catLight;
        catLight.x = (int16_t)(catPose.x + PancettaCat::kWidth + 20);
        catLight.y = (int16_t)(catPose.y - 12);
        catLight.tint = RP::NEON;
        PancettaCat::draw(canvas, catPose, false,
                          PancettaCat::Activity::FOLLOW, now, catLight);
        PancettaCat::drawSignalBubble(canvas, catPose, now);
        Avatar::clearRenderTimeOverride();
        return true;
    }
#endif

    bool handled = true;
    bool isMoving = false;
    bool useRearView = false;

    if (strcmp(clipId, "boot_static_ramen_hold") == 0) {
        drawBootRamenPig(canvas, 124, 116, now, true);
    } else if (strcmp(clipId, "boot_anim_ramen_slurp") == 0) {
        drawBootRamenPig(canvas, 124, 116, now, false);
    } else if (strcmp(clipId, "menu_static_helper_bench") == 0) {
        mode = PigMode::ON_BENCH;
        faceRight = false;
    } else if (strcmp(clipId, "menu_static_laptop_rear") == 0) {
        currentStation = Station::AT_LAPTOP;
        faceRight = false;
        avatarState = AvatarState::HAPPY;
    } else if (strcmp(clipId, "menu_static_window_rear") == 0) {
        currentStation = Station::AT_WINDOW;
        faceRight = true;
        windowCigLit = true;
    } else if (strcmp(clipId, "menu_static_antenna_rear") == 0) {
        currentStation = Station::AT_ANTENNA;
        faceRight = false;
    } else if (strcmp(clipId, "menu_static_terminal_rear") == 0) {
        currentStation = Station::AT_TERMINAL;
        faceRight = false;
    } else if (strcmp(clipId, "menu_static_sofa_sleep") == 0) {
        currentStation = Station::ON_SOFA;
        faceRight = true;
        avatarState = AvatarState::SLEEPY;
    } else if (strcmp(clipId, "menu_static_bed_sleep") == 0) {
        currentStation = Station::IN_BED;
        currentRoom = 2;
        pigX = (float)kR3_BedPigX;
        pigY = (float)kR3_BedPigY;
        faceRight = false;
        avatarState = AvatarState::SLEEPY;
    } else if (strcmp(clipId, "menu_static_ledge_idle") == 0) {
        currentStation = Station::ON_LEDGE;
        faceRight = true;
    } else if (strcmp(clipId, "menu_static_booth_idle") == 0) {
        currentStation = Station::AT_BOOTH;
        faceRight = true;
    } else if (strcmp(clipId, "menu_static_cooking_settle_rear") == 0) {
        currentStation = Station::COOKING;
        roamState = RoamState::SETTLING;
        faceRight = true;
    } else if (strcmp(clipId, "menu_static_ramen_hold") == 0) {
        currentStation = Station::COOKING;
        faceRight = true;
        drawCaptureMenuRamenHold(canvas, now, options);
    } else if (strcmp(clipId, "menu_static_cup_hold_transit") == 0) {
        currentStation = Station::ON_LEDGE;
        faceRight = true;
        carryingCup = true;
        roamState = RoamState::WALKING_TO;
        isMoving = true;
    } else if (strcmp(clipId, "menu_static_wall_break") == 0) {
        currentStation = Station::ON_LEDGE;
        faceRight = true;
        wallBreakActive = true;
        avatarState = AvatarState::HUNTING;
    } else if (strcmp(clipId, "menu_anim_walk_march") == 0 ||
               strcmp(clipId, "menu_anim_walk_march_left") == 0) {
        currentStation = Station::ON_LEDGE;
        faceRight = strcmp(clipId, "menu_anim_walk_march_left") != 0;
        roamState = RoamState::WALKING_TO;
        walkLegDist = (float)((now / 120) % kRoomWalkFrameCount) * kPixelsPerLegFrame;
        isMoving = true;
    } else if (strcmp(clipId, "menu_anim_idle_sniff") == 0) {
        currentStation = Station::ON_LEDGE;
        faceRight = true;
        fidgetStart = 0;
        fidgetRunning = true;
        characterFidget = CharacterFidget::SNIFF;
    } else if (strcmp(clipId, "menu_anim_ramen_slurp") == 0) {
        currentStation = Station::COOKING;
        faceRight = true;
    } else if (strcmp(clipId, "menu_anim_cup_sip") == 0) {
        currentStation = Station::ON_SOFA;
        faceRight = true;
        avatarState = AvatarState::NEUTRAL;
        carryingCup = true;
    } else if (strcmp(clipId, "menu_anim_chair_leg_burst") == 0) {
        currentStation = Station::COOKING;
        faceRight = true;
        carryingCup = true;
        chairLegBurstStartMs = now;
    } else if (strcmp(clipId, "menu_anim_window_smoke") == 0) {
        currentStation = Station::AT_WINDOW;
        faceRight = true;
        windowCigLit = true;
    } else if (strcmp(clipId, "menu_anim_rear_cinematic") == 0) {
        currentStation = Station::AT_ANTENNA;
        faceRight = false;
        rearCineOverrideRear = true;
        if (now < 2000) {
            rearCinePhase = RearCinematicPhase::TAIL_SLIDE;
            float t = (float)now / 2000.0f;
            if (t > 1.0f) t = 1.0f;
            float s = 1.70158f;
            float t1 = t - 1.0f;
            float ease = t1 * t1 * ((s + 1.0f) * t1 + s) + 1.0f;
            rearTailOffsetY = (int16_t)(24.0f * (1.0f - ease));
            if (rearTailOffsetY < 0) rearTailOffsetY = 0;
            rearEyePhase = 0;
        } else {
            rearCinePhase = RearCinematicPhase::EYES_GROW;
            rearTailOffsetY = 0;
            uint32_t phaseElapsed = now - 2000;
            if (phaseElapsed < 300) rearEyePhase = 0;
            else if (phaseElapsed < 600) rearEyePhase = 1;
            else rearEyePhase = 2;
        }
    } else if (strcmp(clipId, "menu_anim_portal_jump") == 0) {
        currentStation = Station::ON_LEDGE;
        faceRight = true;
        portalJumpActive = (now < PORTAL_JUMP_MS);
        portalJumpStart = 0;
    } else {
        handled = false;
    }

    if (handled &&
        strcmp(clipId, "menu_static_ramen_hold") != 0 &&
        strcmp(clipId, "boot_static_ramen_hold") != 0 &&
        strcmp(clipId, "boot_anim_ramen_slurp") != 0) {
        Avatar::setState(avatarState);
        useRearView = shouldUseRearViewInRoaming();
        drawPig(canvas, Display::getColorFG(), Display::getColorBG(), now, isMoving, useRearView, options);
    }

    Avatar::clearRenderTimeOverride();
    return handled;
}

// ==[ PUBLIC API ]==

void startRoaming() {
    if (mode != PigMode::ON_BENCH) return;
    mode = PigMode::LEAVING_BENCH;
    walkLegDist = 0.0f;
    transStart = millis();
    transFromX = pigX;
    int startRoom;
    Station startStation;
    // Initial helper teleport never materializes inside the bath. Room 5 joins
    // the later roaming circuit, where its physical rim jump can always play.
    startRoom = (int)(esp_random() % (NUM_ROOMS - 1));
    switch (startRoom) {
        case 0: startStation = Station::AT_LAPTOP; break;
        case 1: startStation = (esp_random() & 1) ? Station::ON_SOFA : Station::AT_WINDOW; break;
        case 2: startStation = (esp_random() & 1) ? Station::COOKING : Station::IN_BED; break;
        case 3: startStation = (esp_random() & 1) ? Station::AT_ANTENNA : Station::ON_LEDGE; break;
        case 4: startStation = Station::AT_BOOTH; break;
        default: startStation = Station::IN_BATH; break;
    }
    walkTargetRoom = startRoom;
    walkTargetStation = startStation;
    startTeleport(transStart, TeleportContext::HELPER_TO_ROOM);
}

void returnToHelper() {
    if (mode == PigMode::ON_BENCH) return;
    DefhogTerminal::hideImmediate();
    if (mode == PigMode::ROAMING &&
        currentStation == Station::IN_BATH &&
        roamState == RoamState::IDLE) {
        returnToHelperAfterBath = true;
        beginBathDismount(millis(), (float)kR6_BathApproachRightX);
        return;
    }
    teleportPhase = TeleportPhase::NONE;  // cancel any in-progress teleport
    // A cancelled jump still owes the companion his body back, at the landing
    // the jump had already committed to.
    finishCompanionTeleport(millis());
    mode = PigMode::RETURNING_TO_BENCH;
    walkLegDist = 0.0f;
    transStart = millis();
    transFromX = pigX;
    walkFromY = pigY;
}

bool isMenuTransitionLocked() {
    return rearCinePhase != RearCinematicPhase::INACTIVE || isWDCinematicActive();
}

bool isRoaming() {
    return mode == PigMode::ROAMING || mode == PigMode::LEAVING_BENCH;
}


bool isPigOnBench() {
    return mode == PigMode::ON_BENCH;
}

void setEventHold(bool held) {
    if (held == eventHold) return;
    uint32_t now = millis();
    if (held) {
        eventHold = true;
        eventHoldStarted = now;
        return;
    }

    uint32_t heldMs = now - eventHoldStarted;
    stationStart += heldMs;
    stationStayStart += heldMs;
    lastStationDwellTick = now;
    eventHold = false;
    eventHoldStarted = 0;
}

bool isHousePortalReady() {
    return mode == PigMode::ROAMING &&
           roamState == RoamState::IDLE &&
           teleportPhase == TeleportPhase::NONE &&
           !isMenuTransitionLocked();
}

uint8_t getCurrentStation() {
    return (uint8_t)currentStation;
}

uint8_t getCurrentRoom() {
    return (uint8_t)stationRoom(currentStation);
}

void triggerPortalJump() {
    portalJumpActive = true;
    portalJumpStart = millis();
}

// ==[ REAR CINEMATIC API ]==

bool isRearCinematicActive() {
    return rearCinePhase != RearCinematicPhase::INACTIVE &&
           rearCinePhase != RearCinematicPhase::DONE;
}

bool isRearCinematicDone() {
    return rearCinePhase == RearCinematicPhase::DONE;
}

void startRearCinematic() {
    if (rearCinePhase != RearCinematicPhase::INACTIVE) return;
    uint32_t now = millis();
    rearCinePhase = RearCinematicPhase::TAIL_SLIDE;
    rearCineStart = now;
    rearCinePhaseStart = now;
    rearTailOffsetY = 24;       // start at ear level (the bug)
    rearEyePhase = 0;
    rearCineOverrideRear = true; // force rear view
}

void clearRearCinematic() {
    rearCinePhase = RearCinematicPhase::INACTIVE;
    rearCineStart = 0;
    rearCinePhaseStart = 0;
    rearCineOverrideRear = false;
    rearTailOffsetY = 0;
    rearEyePhase = 0;
}

static void updateRearCinematic(uint32_t now) {
    if (rearCinePhase == RearCinematicPhase::INACTIVE ||
        rearCinePhase == RearCinematicPhase::DONE) return;

    uint32_t totalElapsed = now - rearCineStart;
    uint32_t phaseElapsed = now - rearCinePhaseStart;

    switch (rearCinePhase) {
        case RearCinematicPhase::TAIL_SLIDE: {
            // 0–2000ms: tail slides from ear-level (24) to butt (0)
            float t = (float)phaseElapsed / 2000.0f;
            if (t > 1.0f) t = 1.0f;
            // easeOutBack: overshoots slightly then settles
            float s = 1.70158f;
            float t1 = t - 1.0f;
            float ease = t1 * t1 * ((s + 1.0f) * t1 + s) + 1.0f;
            rearTailOffsetY = (int16_t)(24.0f * (1.0f - ease));
            if (rearTailOffsetY < 0) rearTailOffsetY = 0;

            if (phaseElapsed >= 2000) {
                rearTailOffsetY = 0;
                rearCinePhase = RearCinematicPhase::EYES_GROW;
                rearCinePhaseStart = now;
            }
            break;
        }
        case RearCinematicPhase::EYES_GROW: {
            // 2000–3000ms: eyes appear on back of head
            if (phaseElapsed < 300) {
                rearEyePhase = 0;
            } else if (phaseElapsed < 600) {
                rearEyePhase = 1;  // faint dots
            } else {
                rearEyePhase = 2;  // full eyes
            }
            if (phaseElapsed >= 1000) {
                rearCinePhase = RearCinematicPhase::DONE;
            }
            break;
        }
        default: break;
    }
}


bool getPortalAnchor(int16_t& x, int16_t& y) {
    uint32_t now = millis();
    bool useRearView = shouldUseRearViewInRoaming();
    PigPose pose = resolvePigPose(now, isPigMoving(), useRearView, false);

    // Body center — reconstruct mask is pig-shaped around this point.
    x = (int16_t)(pose.drawX + 36);
    y = (int16_t)(pose.drawY + 21);

    snapPortalAnchor(x, y);
    return true;
}

void cycleRoom() {
    if (mode != PigMode::ROAMING || roamState != RoamState::IDLE) return;
    if (isWDCinematicActive()) return;    // locked during wardrive cinematic

    // dissolve terminal when leaving station
    if (DefhogTerminal::isVisible()) {
        DefhogTerminal::hide();
    }
    int targetRoom = (currentRoom + 1) % NUM_ROOMS;
    Station target;
    switch (targetRoom) {
        case 0: target = Station::AT_LAPTOP;  break;
        case 1: target = Station::ON_SOFA;    break;
        case 2: target = Station::COOKING;    break;
        case 3: target = Station::AT_ANTENNA; break;
        case 4: target = Station::AT_BOOTH; break;
        default: target = Station::IN_BATH; break;
    }
    walkTargetStation = target;
    walkTargetRoom = targetRoom;
    useTeleportTransition = currentStation != Station::IN_BATH &&
                            target != Station::IN_BATH &&
                            ((esp_random() & 1) == 0);
    uint32_t now = millis();

    if (useTeleportTransition) {
        // R&M portal quip
        strncpy(narratorBubbleText,
                PORTAL_QUIPS[esp_random() % PORTAL_QUIP_COUNT],
                sizeof(narratorBubbleText) - 1);
        narratorBubbleText[sizeof(narratorBubbleText) - 1] = '\0';
        lastNarratorBubble = now;
        // Teleport from current position
        roamState = RoamState::ROOM_TRANSITION;
        transFromRoom = currentRoom;
        transToRoom = targetRoom;
        roomTransStart = now;
        startTeleport(now);
    } else {
        // Walk quip
        strncpy(narratorBubbleText,
                WALK_QUIPS[esp_random() % WALK_QUIP_COUNT],
                sizeof(narratorBubbleText) - 1);
        narratorBubbleText[sizeof(narratorBubbleText) - 1] = '\0';
        lastNarratorBubble = now;
        // Walk path — dismount from elevated station first if needed
        bool needDismount = pigY < (float)kFloorPigY - 2.0f;
        if (needDismount) {
            if (currentStation == Station::IN_BATH) {
                beginBathDismount(now, bathExitApproachX());
            } else {
                jumpFromX = pigX;
                jumpToX = pigX;
                jumpFromY = pigY;
                jumpToY = (float)kFloorPigY;
                jumpStart = now;
                roamState = RoamState::DISMOUNTING;
            }
        } else {
            walkFromX = pigX;
            walkFromY = pigY;
            walkToX = (float)SCREEN_WIDTH;
            roomSlideDir = 1;
            walkToY = (float)kFloorPigY;
            walkStart = now;
            walkLegDist = 0.0f;
            walkLegBaseDist = 0.0f;
            walkDurationMs = authoredWalkDuration(walkFromX, walkFromY,
                                                  walkToX, walkToY);
            faceRight = true;
            roamState = RoamState::WALKING_TO;
        }
    }
}

static void finishWardriveReturn(uint32_t now) {
    clearWDReturnState();
    wdCinePhase = WDCinePhase::NONE;
    wdCineStart = 0;
    wdMountPhase = WDMountPhase::IDLE;
    wdMountStart = 0;
    wdImpactStart = 0;
    room3CinematicCarRunning = false;
    carState.active = false;
    carState.startMs = 0;
    carState.wdMode = false;
    carState.wdReturnMode = false;
    carState.wdPigMounted = false;
    carState.wdCarDropY = 0.0f;
    currentRoom = 3;
    walkTargetRoom = 3;
    walkTargetStation = Station::ON_LEDGE;
    avatarState = AvatarState::NEUTRAL;
    arriveAtStation(now);
}

static void updateWardriveReturn(uint32_t now) {
    faceRight = true;
    float roofY = WD_CAR_ROOF_Y + carState.wdCarDropY;
    uint32_t phaseElapsed = now - wdReturnStart;

    switch (wdReturnPhase) {
        case WDReturnPhase::CAR_ARRIVE:
            pigX = WD_CAR_CENTER_X + WD_SNEAK_SHIFT_X;
            pigY = roofY + WD_SNEAK_DROP_Y;
            if (phaseElapsed >= WD_ARRIVE_MS) {
                wdReturnPhase = WDReturnPhase::UNSNEAK;
                wdReturnStart = now;
            }
            break;

        case WDReturnPhase::UNSNEAK: {
            if (phaseElapsed >= WD_SNEAK_MS) {
                wdReturnPhase = WDReturnPhase::ROOF_BEAT;
                wdReturnStart = now;
                wdImpactStart = now;
                carState.wdPigMounted = true;
                carState.wdCarDropY = 0.0f;
                pigX = WD_CAR_CENTER_X;
                pigY = roofY;
            } else {
                float st = smootherstep((float)phaseElapsed / (float)WD_SNEAK_MS);
                float revT = 1.0f - st;
                pigX = WD_CAR_CENTER_X + WD_SNEAK_SHIFT_X * revT;
                pigY = roofY + WD_SNEAK_DROP_Y * revT;
            }
            break;
        }

        case WDReturnPhase::ROOF_BEAT: {
            float impactT = (phaseElapsed >= WD_IMPACT_MS)
                ? 1.0f : ((float)phaseElapsed / (float)WD_IMPACT_MS);
            pigX = WD_CAR_CENTER_X;
            pigY = roofY + 2.0f * sinf(impactT * 3.14159f);
            if (phaseElapsed >= WD_IMPACT_MS) {
                wdReturnPhase = WDReturnPhase::JUMP_OUT;
                wdReturnStart = now;
                wdJumpFromX = WD_CAR_CENTER_X;
                wdJumpFromY = roofY;
                carState.wdPigMounted = false;
                carState.wdCarDropY = 0.0f;
            }
            break;
        }

        case WDReturnPhase::JUMP_OUT:
            if (phaseElapsed >= WD_JUMP_MS) {
                pigX = wdReturnLandX;
                pigY = wdReturnLandY;
                finishWardriveReturn(now);
            } else {
                float jt = (float)phaseElapsed / (float)WD_JUMP_MS;
                pigX = wdJumpFromX + (wdReturnLandX - wdJumpFromX) * jt;
                pigY = wdJumpFromY + (wdReturnLandY - wdJumpFromY) * jt
                       - WD_JUMP_HEIGHT * sinf(jt * 3.14159f);
            }
            break;

        default:
            break;
    }
}

// ==[ NOIR JAZZ ROOM OVERRIDES ]==
// Pump + start moved to hamlet.cpp main loop (plays in ALL modes).
// Here we only set room-specific tension/sax overrides when in MENU mode.
static void updateNoirJazzRoomState() {
    if (NoirJazz::isPlaying()) {
        // Room mood alert overrides mode-level tension with finer grain
        float t = (float)roomMood.alertLevel / 4.0f;
        NoirJazz::setTension(t);

        // Sax enters on rooftop (room 3) or DEFHOG4 terminal overlay
        bool saxOn = DefhogTerminal::isVisible() || (currentRoom == 3);
        NoirJazz::setSaxActive(saxOn);
    }
}

// ==[ UPDATE ]==

void update(uint32_t now) {
    updateRearCinematic(now);
    updateNoirVolumetricPass(now);
    updateNoirJazzRoomState();
    updateBathIdlePlan(now);

    // Wire spark trigger
    if (!sparkActive && TimeMath::reachedOrUnset(now, nextSparkTime)) {
        sparkActive = true;
        sparkStart = now;
        nextSparkTime = now + randomRange(SPARK_MIN_MS, SPARK_MAX_MS);
    }
    if (sparkActive && now - sparkStart >= SPARK_FRAME_MS * 2) {
        sparkActive = false;
    }

    // Coffin pod LED toggle
    if (now - podLedStart >= POD_LED_MS) {
        podLedStart = now;
        podLedOn = !podLedOn;
    }

    // Laptop screen blink
    if (now - lastLaptopBlink >= LAPTOP_BLINK_MS) {
        lastLaptopBlink = now;
        laptopScreenOn = !laptopScreenOn;
    }

    bool suppressRoamBubble = eventHold || shouldSuppressRoamBubble() ||
                              currentStation == Station::IN_BATH;
    if (suppressRoamBubble) {
        narratorBubbleText[0] = '\0';
        Barman::clearDialogue();
    }

    // Roaming speech bubble rotation (all stations when IPP active, laptop only otherwise)
    if (!suppressRoamBubble &&
        mode == PigMode::ROAMING && roamState == RoamState::IDLE &&
        now - lastNarratorBubble >= NARRATOR_BUBBLE_MS) {
        lastNarratorBubble = now;
        laptopLineIdx = (laptopLineIdx + 1) % LAPTOP_LINE_COUNT;
        refreshNarratorBubble();
        if (currentRoom == 4 && narratorBubbleText[0])
            Barman::onPancettaSpoke(now);
    }
    // ==[ BENCH TRANSITIONS ]==
    if (mode == PigMode::LEAVING_BENCH) {
        if (teleportPhase != TeleportPhase::NONE) {
            updateTeleport(now);
            updateCompanionCatState(now);
            return;
        }
        uint32_t elapsed = now - transStart;
        if (elapsed >= BENCH_LEAVE_MS) {
            mode = PigMode::ROAMING;
            arriveAtStation(now);
        } else {
            float targetX, targetY;
            bool targetFR;
            getStationPos(walkTargetStation, targetX, targetY, targetFR);
            float t = smootherstep((float)elapsed / (float)BENCH_LEAVE_MS);
            pigX = transFromX + (targetX - transFromX) * t;
            pigY = (float)kHelperPigY + (targetY - (float)kHelperPigY) * t;
            walkLegDist = fabsf(pigX - transFromX);
            faceRight = false;
        }
        updateCompanionCatState(now);
        return;
    }

    if (mode == PigMode::RETURNING_TO_BENCH) {
        uint32_t elapsed = now - transStart;
        if (elapsed >= BENCH_RETURN_MS) {
            mode = PigMode::ON_BENCH;
            pigX = (float)kHelperPigX;
            pigY = (float)kHelperPigY;
            faceRight = false;
            avatarState = AvatarState::NEUTRAL;
        } else {
            float t = smootherstep((float)elapsed / (float)BENCH_RETURN_MS);
            pigX = transFromX + ((float)kHelperPigX - transFromX) * t;
            pigY = walkFromY + ((float)kHelperPigY - walkFromY) * t;
            walkLegDist = fabsf(pigX - transFromX);
            faceRight = false;
        }
        updateCompanionCatState(now);
        return;
    }

    // ==[ ON_BENCH — idle animations ]==
    if (mode == PigMode::ON_BENCH) {
        syncC5SensoryGaze(now);
        faceTimer.update(now);
        updateStationFidget(now);
        updateCompanionCatState(now);
        return;
    }

    // ==[ ROAMING ]==
    if (mode != PigMode::ROAMING) {
        updateCompanionCatState(now);
        return;
    }

    // Roaming state machine
    switch (roamState) {
        case RoamState::WALKING_TO: {
            uint32_t elapsed = now - walkStart;
            if (elapsed >= walkDurationMs) {
                pigX = walkToX;
                pigY = walkToY;
                walkLegDist = walkLegBaseDist + fabsf(walkToX - walkFromX);
                // Check if this was an exit-room walk
                if (walkTargetRoom != currentRoom) {
                    // Preserve contact phase across the doorway. The target
                    // room reveal owns the visual cut while Pancetta stays offstage.
                    walkLegBaseDist = walkLegDist;
                    roamState = RoamState::ROOM_TRANSITION;
                    transFromRoom = currentRoom;
                    transToRoom = walkTargetRoom;
                    roomTransStart = now;
                    currentRoom = walkTargetRoom;
                } else {
                    // Same room arrival — mount onto elevated station if needed
                    float stX, stY;
                    bool stFR;
                    getStationPos(walkTargetStation, stX, stY, stFR);
                    if (stY < (float)kFloorPigY - 2.0f) {
                        jumpFromX = pigX;
                        jumpToX = stX;
                        pigY = (float)kFloorPigY;
                        jumpFromY = (float)kFloorPigY;
                        jumpToY = stY;
                        jumpStart = now;
                        if (walkTargetStation == Station::IN_BATH)
                            faceRight = jumpToX > jumpFromX;
                        roamState = RoamState::MOUNTING;
                    } else {
                        arriveAtStation(now);
                    }
                }
            } else {
                float t = smootherstep((float)elapsed / (float)walkDurationMs);
                pigX = walkFromX + (walkToX - walkFromX) * t;
                pigY = travelYForStation(t, walkFromY, walkToY, walkTargetStation);
                walkLegDist = walkLegBaseDist + fabsf(pigX - walkFromX);
            }
            break;
        }

        case RoamState::ROOM_TRANSITION: {
            if (teleportPhase != TeleportPhase::NONE) {
                // Teleport path: particle decompose/reassemble
                updateTeleport(now);
            } else {
                // Walk path: a short 4px-grid room reveal, then enter from the
                // opposite edge on the same gait phase.
                uint32_t elapsed = now - roomTransStart;
                if (elapsed >= kRoomTransMs) {
                    float entryX = (roomSlideDir > 0) ? (float)-kPigW : (float)SCREEN_WIDTH;
                    float targetX, targetY;
                    bool targetFR;
                    getStationPos(walkTargetStation, targetX, targetY, targetFR);
                    // Always enter at floor level — mount phase handles elevation
                    float entryY = (targetY < (float)kFloorPigY - 2.0f)
                                   ? (float)kFloorPigY : targetY;
                    pigX = entryX;
                    pigY = entryY;
                    walkFromX = entryX;
                    walkFromY = entryY;
                    walkToX = stationApproachX(walkTargetStation, entryX);
                    // Elevated: walk at floor level, mount after
                    walkToY = (targetY < (float)kFloorPigY - 2.0f)
                              ? (float)kFloorPigY : targetY;
                    walkStart = now;
                    walkDurationMs = authoredWalkDuration(walkFromX, walkFromY,
                                                          walkToX, walkToY);
                    walkLegDist = walkLegBaseDist;
                    faceRight = (walkToX > walkFromX);
                    roamState = RoamState::ENTERING_ROOM;
                }
            }
            break;
        }

        case RoamState::ENTERING_ROOM: {
            uint32_t elapsed = now - walkStart;
            if (elapsed >= walkDurationMs) {
                pigX = walkToX;
                pigY = walkToY;
                walkLegDist = walkLegBaseDist + fabsf(walkToX - walkFromX);
                // Mount onto elevated station if needed
                float stX, stY;
                bool stFR;
                getStationPos(walkTargetStation, stX, stY, stFR);
                if (stY < (float)kFloorPigY - 2.0f) {
                    jumpFromX = pigX;
                    jumpToX = stX;
                    pigY = (float)kFloorPigY;
                    jumpFromY = (float)kFloorPigY;
                    jumpToY = stY;
                    jumpStart = now;
                    if (walkTargetStation == Station::IN_BATH)
                        faceRight = jumpToX > jumpFromX;
                    roamState = RoamState::MOUNTING;
                } else {
                    arriveAtStation(now);
                }
            } else {
                float t = smootherstep((float)elapsed / (float)walkDurationMs);
                pigX = walkFromX + (walkToX - walkFromX) * t;
                pigY = travelYForStation(t, walkFromY, walkToY, walkTargetStation);
                walkLegDist = walkLegBaseDist + fabsf(pigX - walkFromX);
            }
            break;
        }

        case RoamState::SETTLING: {
            // Brief pause at station before turning to rear view
            uint32_t settleElapsed = now - settleStart;
            if (settleElapsed >= SETTLE_MS) {
                bool stFR;
                float dummyX, dummyY;
                getStationPos(currentStation, dummyX, dummyY, stFR);
                faceRight = stFR;
                jumpSquashPx = 0.0f;
                roamState = RoamState::IDLE;
                wobbleStart = now;
                arrivalAnimStart = now;
                resetCharacterFidget(now);
                resetIdleTimers(now);
            } else {
                // Damped settle bounce: 2px amplitude, ~2 oscillations
                float bt = (float)settleElapsed / (float)SETTLE_MS;
                jumpSquashPx = 2.0f * fastSinf(bt * 3.14159f * 2.0f) * (1.0f - bt);
            }
            break;
        }

        case RoamState::DISMOUNTING: {
            // Jump down from elevated station to floor before walking
            uint32_t elapsed = now - jumpStart;
            bool bathJump = currentStation == Station::IN_BATH;
            uint32_t jumpDuration = bathJump ? BATH_JUMP_OUT_MS : JUMP_DOWN_MS;
            if (elapsed >= jumpDuration) {
                pigX = jumpToX;
                pigY = (float)kFloorPigY;
                jumpSquashPx = 0.0f;
                // Bath landings leave two fixed wet hoof marks; dry furniture
                // keeps the existing dust beat.
                if (bathJump) {
                    int contactY = snapPigGrid(kFloorY);
                    spawnBathWetTrackAt(snapPigGrid((int)pigX + 20), contactY,
                                        now, faceRight);
                    spawnBathWetTrackAt(snapPigGrid((int)pigX + 50), contactY,
                                        now, faceRight);
                    lastFootLandingFrame = 0;  // landing pair already owns gait frame zero
                } else {
                    int footY = snapPigGrid((int)pigY + kPigH + kRoomLegH - kPigPX);
                    spawnDustPuffAt(snapPigGrid((int)pigX + 18), footY, now);
                    spawnDustPuffAt(snapPigGrid((int)pigX + 52), footY, now);
                }
                if (returnToHelperAfterBath) {
                    returnToHelperAfterBath = false;
                    mode = PigMode::RETURNING_TO_BENCH;
                    walkLegDist = 0.0f;
                    transStart = now;
                    transFromX = pigX;
                    walkFromY = pigY;
                } else {
                    beginWalkToTarget(now);
                }
            } else {
                float t = (float)elapsed / (float)jumpDuration;
                float moveT = smootherstep(t);
                float arc = fastSinf(t * 3.14159f) *
                    (bathJump ? BATH_JUMP_HEIGHT : 6.0f);
                pigX = jumpFromX + (jumpToX - jumpFromX) * moveT;
                pigY = jumpFromY + (jumpToY - jumpFromY) * t - arc;
                // Squash-stretch: crouch before launch, compress on landing
                if (t < 0.15f) {
                    jumpSquashPx = 2.0f * fastSinf(t / 0.15f * 3.14159f * 0.5f);
                } else if (t > 0.85f) {
                    jumpSquashPx = 2.0f * (1.0f - (t - 0.85f) / 0.15f);
                } else {
                    jumpSquashPx = 0.0f;
                }
            }
            break;
        }

        case RoamState::MOUNTING: {
            // Jump up from floor onto elevated station
            uint32_t elapsed = now - jumpStart;
            bool bathJump = walkTargetStation == Station::IN_BATH;
            uint32_t jumpDuration = bathJump ? BATH_JUMP_IN_MS : JUMP_UP_MS;
            if (elapsed >= jumpDuration) {
                pigX = jumpToX;
                pigY = jumpToY;
                jumpSquashPx = 0.0f;
                // Landing dust at foot positions
                if (!bathJump) {
                    int footY = snapPigGrid((int)pigY + kPigH + kRoomLegH - kPigPX);
                    spawnDustPuffAt(snapPigGrid((int)pigX + 18), footY, now);
                    spawnDustPuffAt(snapPigGrid((int)pigX + 52), footY, now);
                }
                arriveAtStation(now);
            } else {
                float t = (float)elapsed / (float)jumpDuration;
                float moveT = smootherstep(t);
                float arc = fastSinf(t * 3.14159f) *
                    (bathJump ? BATH_JUMP_HEIGHT : 10.0f);
                pigX = jumpFromX + (jumpToX - jumpFromX) * moveT;
                pigY = jumpFromY + (jumpToY - jumpFromY) * t - arc;
                // Squash-stretch: deep crouch before big jump, compress on landing
                if (t < 0.12f) {
                    jumpSquashPx = 4.0f * fastSinf(t / 0.12f * 3.14159f * 0.5f);
                } else if (t > 0.88f) {
                    jumpSquashPx = 2.0f * (1.0f - (t - 0.88f) / 0.12f);
                } else {
                    jumpSquashPx = 0.0f;
                }
            }
            break;
        }

        case RoamState::IDLE: {
            syncC5SensoryGaze(now);
            faceTimer.update(now);
            updateStationDwell(now);

            if (wdCinePhase == WDCinePhase::WD_RETURN) {
                updateWardriveReturn(now);
                break;
            }

            updateStationFidget(now);

            // ==[ WD0: pig mount ]== walk to car then jump onto roof
            if (wdCinePhase == WDCinePhase::WD_WAIT && carState.active &&
                wdMountPhase != WDMountPhase::INSIDE) {
                uint32_t rawMountElapsed =
                    (wdMountPhase != WDMountPhase::IDLE) ? (now - wdMountStart) : 0;
                uint32_t mountElapsed = isWDMenuCineFast()
                    ? wdMenuCineElapsed(rawMountElapsed) : rawMountElapsed;

                if (wdMountPhase == WDMountPhase::WALKING) {
                    // walk rightward toward car at floor level
                    if (mountElapsed >= WD_WALK_MS) {
                        // walk done — start jump
                        wdMountPhase = WDMountPhase::JUMPING;
                        wdMountStart = now;
                        wdJumpFromX = pigX;
                        wdJumpFromY = pigY;
                    } else {
                        float wt = (float)mountElapsed / (float)WD_WALK_MS;
                        float st = wt * wt * wt * (wt * (wt * 6.0f - 15.0f) + 10.0f); // smootherstep
                        pigX = wdMountFromX + (WD_WALK_TARGET_X - wdMountFromX) * st;
                        pigY = wdMountFromY;  // stay at floor level
                        walkLegDist = fabsf(pigX - wdMountFromX);
                        faceRight = true;
                    }
                } else if (wdMountPhase == WDMountPhase::JUMPING) {
                    // arc jump from walk end up onto car roof
                    if (mountElapsed >= WD_JUMP_MS) {
                        // landed on roof — hold for impact before ducking inside
                        wdMountPhase = WDMountPhase::IMPACT;
                        wdMountStart = now;
                        wdImpactStart = now;
                        carState.wdPigMounted = true;
                        carState.wdCarDropY = 0.0f;
                        pigX = WD_CAR_ROOF_PIG_X;
                        pigY = WD_CAR_ROOF_PIG_Y;
                    } else {
                        float jt = (float)mountElapsed / (float)WD_JUMP_MS;
                        float targetX = WD_CAR_ROOF_PIG_X;
                        pigX = wdJumpFromX + (targetX - wdJumpFromX) * jt;
                        pigY = wdJumpFromY + (WD_CAR_ROOF_PIG_Y - wdJumpFromY) * jt
                               - WD_JUMP_HEIGHT * sinf(jt * 3.14159f);
                        faceRight = true;
                    }
                } else if (wdMountPhase == WDMountPhase::IMPACT) {
                    float roofY = WD_CAR_ROOF_PIG_Y + carState.wdCarDropY;
                    float impactT = (mountElapsed >= WD_IMPACT_MS)
                        ? 1.0f : ((float)mountElapsed / (float)WD_IMPACT_MS);
                    pigX = WD_CAR_ROOF_PIG_X;
                    pigY = roofY + 2.0f * sinf(impactT * 3.14159f);
                    faceRight = true;
                    if (mountElapsed >= WD_IMPACT_MS) {
                        // pig on roof — teleport into cockpit instead of sneaking in
                        wdMountPhase = WDMountPhase::ROOF_RIDE;
                        wdMountStart = now;
                        carState.wdPigMounted = true;
                        float srcCX = pigX + 36.0f;
                        float srcCY = pigY + 21.0f;
                        int16_t dstCXi, dstCYi;
                        WardriveScene::getCockpitPigCenter(dstCXi, dstCYi);
                        Teleport::startCrossMode(Teleport::Context::MENU_TO_WARDRIVE,
                            srcCX, srcCY,
                            (float)WD_CAR_CENTER_X, WD_CAR_ROOF_Y - 30.0f,
                            (float)dstCXi, (float)dstCYi, now,
                            faceRight ? Teleport::PigSilhouette::SIDE_RIGHT
                                      : Teleport::PigSilhouette::SIDE_LEFT,
                            Teleport::PigSilhouette::REAR);
                    }
                }
            }

            // 4th wall break
            if (!wallBreakActive && nextWallBreak > 0 && TimeMath::reached(now, nextWallBreak) &&
                currentStation != Station::COOKING &&
                currentStation != Station::IN_BATH) {
                wallBreakActive = true;
                wallBreakStart = now;
                savedFaceRight = faceRight;
                faceRight = true;
                avatarState = AvatarState::HUNTING;
            }
            if (wallBreakActive) {
                if (now - wallBreakStart >= WALL_BREAK_STARE_MS) {
                    wallBreakActive = false;
                    faceRight = savedFaceRight;
                    switch (currentStation) {
                        case Station::AT_LAPTOP:   avatarState = AvatarState::HAPPY; break;
                        case Station::ON_SOFA:     avatarState = AvatarState::SLEEPY; break;
                        case Station::IN_BED:      avatarState = AvatarState::SLEEPY; break;
                        case Station::AT_ANTENNA:  avatarState = AvatarState::HUNTING; break;
                        case Station::AT_TERMINAL: avatarState = AvatarState::HAPPY; break;
                        case Station::IN_BATH:     avatarState = AvatarState::HAPPY; break;
                        default:                   avatarState = AvatarState::NEUTRAL; break;
                    }
                    nextWallBreak = now + randomRange(WALL_BREAK_MIN_MS, WALL_BREAK_MAX_MS);
                }
            }

            // Sleep phrase cycling (sofa/bed)
            if (currentStation == Station::ON_SOFA || currentStation == Station::IN_BED) {
                if (now - lastSleepPhrase >= SLEEP_PHRASE_MS) {
                    lastSleepPhrase = now;
                    sleepPhraseIdx = (sleepPhraseIdx + 1) % SLEEP_PHRASE_COUNT;
                }
            }

            // ==[ DEFHOG4 DEFENSE ]== terminal overlay at left-side work stations
            // delayed appearance with probability — not every visit triggers
            {
                bool plStation = (currentStation == Station::AT_LAPTOP ||
                                  currentStation == Station::ON_SOFA ||
                                  currentStation == Station::AT_ANTENNA ||
                                  currentStation == Station::AT_TERMINAL);

                if (!eventHold && plStation && !DefhogTerminal::isVisible() && !terminalDecided) {
                    // start delay timer on first frame at PL station
                    if (!terminalDelayActive) {
                        terminalDelayEnd = now + randomRange(2000, 5000);
                        terminalDelayActive = true;
                    }
                    // check if delay expired
                    if (terminalDelayActive && TimeMath::reached(now, terminalDelayEnd)) {
                        terminalDelayActive = false;
                        terminalDecided = true;
                        if ((esp_random() % 100) < 70) {  // 70% chance
                            DefhogTerminal::show((uint8_t)currentStation);
                            // extend station duration for terminal viewing
                            stationDuration = randomRange(35000, 55000);
                        }
                    }
                } else if (!plStation && DefhogTerminal::isVisible()) {
                    DefhogTerminal::hide();
                }
                if (!eventHold && DefhogTerminal::isVisible()) {
                    DefhogTerminal::update(now, (uint8_t)currentStation,
                                            (uint8_t)stationRoom(currentStation));
                }
            }

            // Station timer — frozen during WD cinematic
            bool sessionActive = Config::isSessionActive();

            if (!eventHold && now - stationStart >= stationDuration && !isWDCinematicActive()) {
                // ==[ STATION COMPLETION REWARDS ]== +1 momentum, +2 XP base
                uint32_t stationXP = 2;
                uint32_t stationGainXP = 1;

                // DEFHOG4-extended stations get +5 XP instead of +2
                if (DefhogTerminal::isVisible()) stationXP = 5;

                if (sessionActive) {
                    Mood::addMomentum(stationGainXP);
                    // mood boost per station type
                    switch (currentStation) {
                        case Station::ON_SOFA:   Mood::addMomentum(2); break;
                        case Station::AT_LAPTOP: Mood::addMomentum(1); break;
                        case Station::COOKING:   Mood::addMomentum(1); break;
                        case Station::IN_BATH:   Mood::addMomentum(2); break;
                        default: break;
                    }
                    Config::addXP(stationXP, Config::RewardSource::STATION);
                }

                pickNextStation(now);
            }

            // ==[ ROOM MOOD ESCALATION ]== alert level drives momentum drip (every 10s)
            if (now - lastAlertDripTime >= 10000) {
                lastAlertDripTime = now;
                if (roomMood.alertLevel >= 3) Mood::addMomentum(3);
                else if (roomMood.alertLevel >= 2) Mood::addMomentum(1);

                // tracker present during full station stay: +5 XP (once per stay)
                if (!trackerSurvivalAwarded && roomMood.trackerPresent &&
                    Config::isSessionActive() &&
                    now - stationStayStart >= stationDuration) {
                    trackerSurvivalAwarded = true;
                    Config::addXP(5, Config::RewardSource::TRACKER_SURVIVAL);
                    if (sessionActive && DefhogTerminal::isVisible()) {
                        char gain[24];
                        ProgressionText::formatGainAmount(gain, sizeof(gain), 5);
                        DefhogTerminal::pushLineDim("survived surveillance. %s", gain);
                    }
                }
            }

            // Pod sleep momentum drip
            if (currentStation == Station::IN_BED) {
                if (now - lastBedMomentum >= 10000) {
                    Mood::addMomentum(1);
                    lastBedMomentum = now;
                }
            }

            // Laptop bubble momentum (one-shot per arrival)
            if (currentStation == Station::AT_LAPTOP && !laptopMomentumAwarded && now - lastLaptopBubble < 100) {
                Mood::addMomentum(1);
                laptopMomentumAwarded = true;
            }

            break;
        }
    }
    updateCompanionCatState(now);
}

// ==[ DRAW HELPER ]==

void drawHelper(M5Canvas& canvas, const char* description) {
    RP::update();
    uint16_t pigFg = Display::getColorFG();
    uint16_t pigBg = RP::BG;
    uint32_t now = millis();
    Barman::setRoomVisible(false, now);
    // The helper scene has no smoker. Anything still airborne from the rooms
    // would otherwise reappear mid-drift the next time roaming resumes.
    SmokeFx::clear();
    SkyFx::clear();
    drawHelperBackdrop(canvas, now);

    // Pig on crate — suppressed while particles own the silhouette. The cat
    // shares L5 but stays on its far sub-plane so Pancetta and held props keep
    // the primary silhouette when the two actors meet.
    const bool pigHidden = isPortalReconstructing();
    const PancettaCat::Pose catPose = PancettaCat::currentPose();
    // Same seam as the room pass: the near plane is only offered while
    // Pancetta is actually drawn, so the companion is never asked to stand in
    // front of a silhouette that is not there and then dropped.
    const bool catNear = !pigHidden && PancettaCat::drawsAbovePig();
    if (!catNear)
        drawCompanionCat(canvas, catPose, now, true);
    if (!pigHidden) {
        bool isMoving = (mode == PigMode::LEAVING_BENCH || mode == PigMode::RETURNING_TO_BENCH);
        MenuPigRenderOptions options;
        drawPig(canvas, pigFg, pigBg, now, isMoving, false, options);
    }
    if (catNear)
        drawCompanionCat(canvas, catPose, now, true);

    // Cross-mode particles are a pig-plane effect. The helper pipe remains L6
    // above both the ordinary pig and its reconstruction particles.
    if (Teleport::isActive())
        Teleport::draw(canvas, pigFg, pigBg, now);
    drawHelperForeground(canvas, now);

    // Speech bubble for highlighted menu item
    if (!pigHidden && mode == PigMode::ON_BENCH)
        drawHelperBubble(canvas, description);
}

// ==[ ROOM DOORWAY REVEAL ]==

static void drawRoomTransitionReveal(M5Canvas& canvas, uint32_t now) {
    if (roamState != RoamState::ROOM_TRANSITION ||
        teleportPhase != TeleportPhase::NONE) return;

    uint32_t elapsed = now - roomTransStart;
    float t = Gfx::clamp01((float)elapsed / (float)kRoomTransMs);
    int reveal = ((int)lroundf(smootherstep(t) * (float)SCREEN_WIDTH)) & ~3;
    uint32_t seed = 0xD00Fu ^ (uint32_t)(transFromRoom + 1) * 131u ^
                    (uint32_t)(transToRoom + 1) * 977u;

    // Eight-pixel scan bands make the new room read as a physical doorway,
    // not a held blank frame. Stagger collapses to zero at both endpoints.
    const int roomBottom = kFloorY + kRoomPX * 2;
    for (int y = kRoomY; y < roomBottom; y += kRoomPX * 2) {
        int bandH = min(kRoomPX * 2, roomBottom - y);
        int stagger = (int)(wallHash(y, transFromRoom, seed) & 3u) * kRoomPX;
        stagger = ((int)lroundf((float)stagger * (1.0f - t))) & ~3;
        int rowReveal = max(0, min(SCREEN_WIDTH, reveal - stagger));
        if (roomSlideDir > 0) {
            if (rowReveal < SCREEN_WIDTH)
                canvas.fillRect(rowReveal, y, SCREEN_WIDTH - rowReveal,
                                bandH, RP::BG);
            if (rowReveal > 0 && rowReveal < SCREEN_WIDTH)
                canvas.fillRect(rowReveal - kRoomPX, y, kRoomPX,
                                bandH, RP::D_DEEP);
        } else {
            int coverW = SCREEN_WIDTH - rowReveal;
            if (coverW > 0)
                canvas.fillRect(0, y, coverW, bandH, RP::BG);
            if (coverW > 0 && coverW < SCREEN_WIDTH)
                canvas.fillRect(coverW, y, kRoomPX,
                                bandH, RP::D_DEEP);
        }
    }
}

// ==[ DRAW ROAMING ]==

void drawRoaming(M5Canvas& canvas) {
    RP::update();
    calcParallax();
    uint32_t now = debugRoamingFrame.active ? debugRoamingFrame.renderNow : millis();
#ifdef HAMLET_FRAME_PROFILE
    FrameBudget::setProfileRoom((uint8_t)currentRoom);
#endif
    Barman::setRoomVisible(currentRoom == 4, now);
    // Airborne smoke is the one effect allowed to outlive its emitter, so it
    // gets integrated once per rendered frame ahead of every draw. A room cut
    // is the only hard reset — smoke may not ride a teleport into the next set.
    SmokeFx::setScene(((uint32_t)currentRoom << 8) | (uint32_t)mode);
    SmokeFx::update(now);
    // One storm, felt the same way everywhere. Thunder is the only signal in
    // this build that every set already shares, so it is the cheapest way to
    // make the rooms read as rooms in one building: the bar's cigarettes, the
    // ramen steam and the bath columns all break on the same beat.
    // Deliberately weaker than the bath cannonball — a pressure wave through
    // the glass, not something in the room. Latched on the rising edge because
    // Weather publishes a flag, not a flash timestamp.
    static SmokeFx::EdgeToken thunderEdge;
    if (const uint32_t tk =
            thunderEdge.sample(Weather::isThunderFlashing(), now)) {
        SmokeFx::gust(tk, 104u);
    }
    // Same contract for the rooftop sky: an airship crossing is an event with
    // a start and an end, so it needs a clock of its own and a hard cut on a
    // room change. Cheap to tick from any room — it only schedules.
    SkyFx::setScene(((uint32_t)currentRoom << 8) | (uint32_t)mode);
    SkyFx::update(now);
    if (debugRoamingFrame.active) {
        roomMood = {};
        roomProgress = {};
        roomProgressLastRefresh = 0;
    } else {
        updateRoomMood();
        refreshRoomProgressVisuals(now);
    }
    uint16_t pigFg = Display::getColorFG();
    uint16_t pigBg = RP::BG;
    colorEvent = debugRoamingFrame.active ? ColorEventSample{} : sampleColorEvent(now);
    if (currentRoom != 3) room3CinematicCarRunning = false;
    clearPigEffectSnapshot();

    // Fill full area below top bar with room black (no gaps)
    canvas.fillRect(0, UIMeasurements::kTopBarH, SCREEN_WIDTH,
                    UIMeasurements::kScreenHeight - UIMeasurements::kTopBarH, RP::BG);

    // During teleport: draw appropriate room + teleport effect instead of pig
    if (teleportPhase != TeleportPhase::NONE) {
        if (teleportContext == TeleportContext::HELPER_TO_ROOM &&
            teleportPhase == TeleportPhase::DECOMPOSE) {
            drawHelperBackdrop(canvas, now);
            // Nothing to draw once he is in the beam — his particles are part
            // of the teleport effect from here to the far room.
            const PancettaCat::Pose catPose = PancettaCat::currentPose();
            if (!PancettaCat::drawsAbovePig())
                drawCompanionCat(canvas, catPose, now, true);
            drawTeleportEffect(canvas, now, pigFg, pigBg);
            if (PancettaCat::drawsAbovePig())
                drawCompanionCat(canvas, catPose, now, true);
            drawHelperForeground(canvas, now);
        } else {
            drawCurrentRoomCached(canvas, now);
            drawCurrentRoom(canvas, now, RoomRenderPass::LIVE);
            drawRoomNoirPass(canvas, now);
            // A companion left behind by the jump — one who was not in the
            // room to begin with — still keeps his own plane around the beam.
            const PancettaCat::Pose catPose = PancettaCat::currentPose();
            if (!PancettaCat::drawsAbovePig())
                drawCompanionCat(canvas, catPose, now, false);
            drawTeleportEffect(canvas, now, pigFg, pigBg);
            if (PancettaCat::drawsAbovePig())
                drawCompanionCat(canvas, catPose, now, false);
            // L6 stays above reconstruction particles, with destination-owned
            // lips/privacy selected by isStationVisualActive().
            drawCurrentRoomForeground(canvas, now);
        }
        return;
    }

    // L0-L3: room background (far plate, structural, env FX)
    drawCurrentRoomCached(canvas, now);
    drawCurrentRoom(canvas, now, RoomRenderPass::LIVE);
    drawBathWetTracks(canvas, now);

    // L2: footfall dust belongs in scene space behind the global grade,
    // Pancetta, and all near occluders. Expiry is bounded to four puffs.
    {
        constexpr int p = kPigPX;
        for (int i = 0; i < MAX_DUST_PUFFS; i++) {
            DustPuff& dp = dustPuffs[i];
            if (!dp.active) continue;
            uint32_t age = now - dp.spawnTime;
            if (age > 300) { dp.active = false; continue; }
            float t = (float)age / 300.0f;
            int dy = -(int)(t * 4.0f);
            int dx = (int)(fastSinf(t * 3.14f + (float)i) * 2.0f);
            if (t < 0.7f)
                canvas.fillRect(snapPigGrid(dp.x + dx),
                                snapPigGrid(dp.y + dy), p, p, RP::DUST);
        }
    }

    // L4: global room grade belongs below the pig. Pig-specific noir lighting
    // is applied from the captured substrate in each room foreground pass.
    drawRoomNoirPass(canvas, now);

    // ==[ CROSS-MODE TELEPORT ]== decompose from room or reassemble into room
    bool crossModeTP = Teleport::isActive() &&
        (Teleport::getContext() == Teleport::Context::MENU_TO_WARDRIVE ||
         Teleport::getContext() == Teleport::Context::WARDRIVE_TO_MENU);
    bool crossModeHidePig = crossModeTP && Teleport::isPigHidden();
    // L5: the companion is the far actor; Pancetta and held props own the
    // near actor plane. Pig-only teleport and vehicle states never erase an
    // independent cat from the room. Foregrounds still occlude both at L6.
    // The near plane exists only while Pancetta is on screen: with no
    // silhouette to stand in front of, a companion who was on it belongs back
    // on his own. Asking for a plane that is not being drawn is how a cat in
    // company blinked out for the whole WD mount and every cross-mode beam.
    const PancettaCat::Pose catPose = PancettaCat::currentPose();
    const bool pigDrawn = !crossModeHidePig && !isWDPigInsideCar();
    const bool catNear = pigDrawn && PancettaCat::drawsAbovePig();
    // The trinket shares the companion's plane but sits behind him, so a paw
    // beat reads as reaching over the object rather than through it. Its key
    // light is its own: it stays where it was put and he does not.
    PancettaCat::drawTrinket(canvas, selectTrinketKeyLight(now));
    if (!catNear)
        drawCompanionCat(canvas, catPose, now, false);
    const bool catVisible = PancettaCat::isVisible();

    if (pigDrawn) {
        bool isMoving = isRoamingMoveState();
        bool useRearView = shouldUseRearViewInRoaming();
        MenuPigRenderOptions options;
        drawPig(canvas, pigFg, pigBg, now, isMoving, useRearView, options);
    }
    if (catNear)
        drawCompanionCat(canvas, catPose, now, false);

    // Cross-mode teleport replaces the L5 pig silhouette and must remain below
    // room foreground/UI. Menu::draw no longer composites a duplicate pass.
    if (crossModeTP)
        Teleport::draw(canvas, pigFg, RP::BG, now);

    // L6: foreground occluders (near glass, rails, overhangs)
    drawCurrentRoomForeground(canvas, now);

    // The doorway is the last scene-space composite. It masks old-room dust
    // and clamps exactly to the authored room, never into bottom chrome.
    drawRoomTransitionReveal(canvas, now);

    // Barman response bubble (room 4 only)
    // Drawn before DEFHOG4 so terminal renders on top
    bool barmanBubbleDrawn = false;
    if (currentRoom == 4 && !debugRoamingFrame.active &&
        roamState == RoamState::IDLE)
        barmanBubbleDrawn = Barman::drawBubble(canvas, now);

    // ==[ DEFHOG4 DEFENSE OVERLAY ]== terminal over room scene
    if (DefhogTerminal::isVisible() && !(currentRoom == 3 && room3CinematicCarRunning)) {
        DefhogTerminal::draw(canvas, pigFg, RP::BG);
    }

    // Sleep bubble (sofa/bed idle) — skip when IPP narrates or DEFHOG4 active
    bool ippNarrating = Config::getIppEnabled();
    bool sleepBubbleDrawn =
        !debugRoamingFrame.active &&
        !ippNarrating && !DefhogTerminal::isVisible() &&
        roamState == RoamState::IDLE &&
        (currentStation == Station::ON_SOFA || currentStation == Station::IN_BED);
    if (sleepBubbleDrawn)
        drawSleepBubble(canvas);

    // Roaming bubble (all stations when IPP active, else laptop only)
    if (!debugRoamingFrame.active && !DefhogTerminal::isVisible())
        drawRoamBubble(canvas);

    // Cat transmissions are L7 dialogue. Existing terminal, narrator, sleep,
    // and barman speech always owns the channel first.
    if (catVisible && !debugRoamingFrame.active &&
        !DefhogTerminal::isVisible() &&
        !barmanBubbleDrawn && !sleepBubbleDrawn &&
        !narratorBubbleOwnsChannel())
        PancettaCat::drawSignalBubble(canvas, catPose, now);

}


uint8_t getCatDossierCount() {
    return CatBehavior::popcount8(
        catDossierMask(Config::getCatMemoryMask()));
}

uint8_t getCatMasteryCount() {
    return CatBehavior::popcount8(
        catMasteryMask(Config::getCatMemoryMask()));
}

uint8_t getCatBondTier() {
    const uint32_t mask = Config::getCatMemoryMask();
    return CatBehavior::bondTierFor(
        catLegacyMemoryCount(mask), catDossierMask(mask),
        catMasteryMask(mask),
        (uint8_t)min(255, (int)Config::getStreak()),
        (uint8_t)min(100, (int)Config::getGoalProgress()));
}

const char* getCatCurrentRoutineName() {
    return CatBehavior::routineName(catBehaviorDecision.routine);
}

void cleanupForModeExit() {
    DefhogTerminal::hideImmediate();
    teleportPhase = TeleportPhase::NONE;
    teleportContext = TeleportContext::ROOM_TO_ROOM;
    finishCompanionTeleport(millis());
    PancettaCat::setTrinketAnchor(0, 0, false);
    useTeleportTransition = false;
    narratorBubbleText[0] = '\0';
    SmokeFx::clear();
    SkyFx::clear();
    Barman::setRoomVisible(false, millis());
    Barman::clearDialogue();
    eventHold = false;
    eventHoldStarted = 0;

    clearRearCinematic();
    clearWDCinematic();
}

// ==[ WD PUBLIC API ]==

void startWardriveEntry() {
    if (wdCinePhase != WDCinePhase::NONE) return;
    uint32_t now = millis();

    wdCinePhase = WDCinePhase::WD_WALK;
    wdCineStart = now;

    // Helper bench -> Room 3 ON_LEDGE through a single helper-to-room teleport.
    if (mode == PigMode::ON_BENCH) {
        DefhogTerminal::hideImmediate();
        walkTargetStation = Station::ON_LEDGE;
        walkTargetRoom = 3;
        transStart = now;
        transFromX = pigX;
        mode = PigMode::LEAVING_BENCH;
        startTeleport(now, TeleportContext::HELPER_TO_ROOM);
        return;
    }

    // Already at ON_LEDGE in Room 3? Skip teleport, force car.
    if (currentRoom == 3 && currentStation == Station::ON_LEDGE &&
        roamState == RoamState::IDLE) {
        wdCinePhase = WDCinePhase::WD_WAIT;
        wdCarForceStart = true;
        return;
    }

    // Hide any existing terminal
    if (DefhogTerminal::isVisible()) DefhogTerminal::hideImmediate();

    // Teleport to Room 3 ON_LEDGE using the standard room-transition flow
    walkTargetStation = Station::ON_LEDGE;
    walkTargetRoom = 3;
    useTeleportTransition = true;
    roamState = RoamState::ROOM_TRANSITION;
    transFromRoom = currentRoom;
    transToRoom = 3;
    roomTransStart = now;
    startTeleport(now, TeleportContext::ROOM_TO_ROOM);
}

bool isWDCinematicRunning() {
    return wdCinePhase != WDCinePhase::NONE;
}

void clearWDCinematic() {
    bool restoreStablePose = (wdCinePhase != WDCinePhase::NONE) ||
                             wdCarForceStart ||
                             carState.wdMode ||
                             carState.wdReturnMode ||
                             carState.wdPigMounted ||
                             wdMountPhase != WDMountPhase::IDLE ||
                             wdReturnPhase != WDReturnPhase::NONE;

    wdCinePhase = WDCinePhase::NONE;
    wdCineStart = 0;
    wdCarForceStart = false;
    wdMountPhase = WDMountPhase::IDLE;
    wdMountStart = 0;
    wdImpactStart = 0;
    clearWDReturnState();
    room3CinematicCarRunning = false;
    if (carState.wdMode || carState.wdReturnMode) {
        carState.active = false;
        carState.startMs = 0;
    }
    carState.wdMode = false;
    carState.wdReturnMode = false;
    carState.wdPigMounted = false;
    carState.wdCarDropY = 0.0f;

    if (restoreStablePose && mode == PigMode::ROAMING) {
        currentRoom = stationRoom(currentStation);
        float stX, stY;
        bool stFR;
        getStationPos(currentStation, stX, stY, stFR);
        pigX = stX;
        pigY = stY;
        faceRight = stFR;
        roamState = RoamState::IDLE;
        walkTargetRoom = currentRoom;
        walkTargetStation = currentStation;
        transFromRoom = currentRoom;
        transToRoom = currentRoom;
        roomTransStart = 0;
        roomSlideDir = 0;
        teleportPhase = TeleportPhase::NONE;
        teleportContext = TeleportContext::ROOM_TO_ROOM;
        finishCompanionTeleport(millis());
        useTeleportTransition = false;
    }
}

void returnFromWardrive() {
    // restore pig at Room 3 AT_ANTENNA after WARDRIVE exit
    clearWDReturnState();
    wdCinePhase = WDCinePhase::NONE;
    wdCineStart = 0;
    wdMountPhase = WDMountPhase::IDLE;
    wdMountStart = 0;
    wdImpactStart = 0;
    room3CinematicCarRunning = false;
    // clear car WD mode flags
    carState.active = false;
    carState.startMs = 0;
    carState.wdMode = false;
    carState.wdReturnMode = false;
    carState.wdPigMounted = false;
    carState.wdCarDropY = 0.0f;
    wdCarForceStart = false;
    // land pig at antenna — arriveAtStation inits timers + roamState
    currentRoom = 3;
    walkTargetRoom = 3;
    walkTargetStation = Station::AT_ANTENNA;
    uint32_t now = millis();
    arriveAtStation(now);
}

void prepareWardriveExitStation(float& dstCX, float& dstCY,
                                 bool& dstRear, bool& dstFaceRight) {
    // pick random room + station for teleport landing
    pendingExitRoom = esp_random() % NUM_ROOMS;
    Station candidates[STATION_COUNT] = {};
    int count = 0;
    for (int i = 0; i < STATION_COUNT; ++i) {
        Station s = (Station)i;
        if (stationRoom(s) == (int)pendingExitRoom) {
            candidates[count++] = s;
        }
    }
    pendingExitStation = (count > 0)
        ? candidates[esp_random() % count]
        : Station::AT_LAPTOP;
    float stX, stY;
    bool stFR;
    getStationPos(pendingExitStation, stX, stY, stFR);
    dstCX = stX + 36.0f;
    dstCY = stY + 21.0f;
    // Cross-mode body ownership starts after the 300ms cooking lead-in, so
    // cooking exits settle side-on while persistent rear stations stay rear.
    dstRear = stationUsesRearStableSilhouette(pendingExitStation);
    dstFaceRight = stFR;
}

void returnFromWardriveViaTeleport() {
    // set up pig at pre-selected station — teleport reassemble handles the visual
    clearWDReturnState();
    wdCinePhase = WDCinePhase::NONE;
    wdCineStart = 0;
    wdMountPhase = WDMountPhase::IDLE;
    wdMountStart = 0;
    wdImpactStart = 0;
    room3CinematicCarRunning = false;
    carState.active = false;
    carState.startMs = 0;
    carState.wdMode = false;
    carState.wdReturnMode = false;
    carState.wdPigMounted = false;
    carState.wdCarDropY = 0.0f;
    wdCarForceStart = false;
    currentRoom = pendingExitRoom;
    walkTargetRoom = pendingExitRoom;
    walkTargetStation = pendingExitStation;
    mode = PigMode::ROAMING;
    roamState = RoamState::IDLE;
    float stX, stY;
    bool stFR;
    getStationPos(pendingExitStation, stX, stY, stFR);
    pigX = stX;
    pigY = stY;
    faceRight = stFR;
    uint32_t now = millis();
    arriveAtStation(now);
}

} // namespace MenuPig
