/**
 * menu_pig_internal.h — shared internal state for menu_pig room renderers
 *
 * NOT a public API. Only included by menu_pig.cpp and src/ui/rooms/*.cpp.
 * Follows the MenuPigRender::RP:: extern pattern from menu_pig_render.h.
 *
 * State defined in menu_pig.cpp, extern-declared here so room renderers
 * can read/write the same data. All runs single-threaded on main loop.
 */
#pragma once

#include <M5Unified.h>
#include "menu_pig.h"
#include "menu_pig_render.h"
#include "display.h"
#include "ui_measurements.h"
#include "../piglet/avatar.h"
#include "../piglet/pig_face_timer.h"
#include "../piglet/weather.h"

namespace MenuPig {

using namespace UIMeasurements::MenuPigLayout;
using UIMeasurements::roomX;
using UIMeasurements::roomY;
using UIMeasurements::roomW;
using UIMeasurements::roomH;
using UIMeasurements::pigSnapX;
using UIMeasurements::pigSnapY;
using namespace MenuPigRender;

// ==[ ENUMS ]== defined in menu_pig.cpp, declared here for room files

enum class PigMode : uint8_t {
    ON_BENCH,
    LEAVING_BENCH,
    ROAMING,
    RETURNING_TO_BENCH,
};

enum class Station : uint8_t {
    AT_LAPTOP,
    ON_SOFA,
    AT_WINDOW,
    COOKING,
    IN_BED,
    AT_ANTENNA,
    ON_LEDGE,
    AT_TERMINAL,
    AT_BOOTH,
    IN_BATH,
};

enum class RoamState : uint8_t {
    IDLE,
    WALKING_TO,
    ROOM_TRANSITION,
    ENTERING_ROOM,
    SETTLING,
    DISMOUNTING,
    MOUNTING,
};

enum class WDCinePhase : uint8_t {
    NONE,
    WD_WALK,
    WD_WAIT,
    WD_RETURN
};

enum class WDMountPhase : uint8_t {
    IDLE,
    WALKING,
    JUMPING,
    IMPACT,
    ROOF_RIDE,
    SNEAK_IN,
    INSIDE
};

enum class WDReturnPhase : uint8_t {
    NONE,
    CAR_ARRIVE,
    UNSNEAK,
    ROOF_BEAT,
    JUMP_OUT
};

enum class TeleportPhase : uint8_t {
    NONE, DECOMPOSE, VOID, REASSEMBLE, SETTLE
};

enum class RearCinematicPhase : uint8_t {
    INACTIVE = 0,
    TAIL_SLIDE,
    EYES_GROW,
    DONE
};

// ==[ STRUCTS ]== defined in menu_pig.cpp

struct RoomMood {
    uint8_t alertLevel;
    uint8_t rfActivity;
    uint8_t rfFruitCount;
    bool trackerPresent;
    bool spamActive;
    uint8_t captureCount;
    bool huntWasActive;
};

// Scenery reads this snapshot; progression modules stay owned by menu_pig.cpp.
// Only full-grid environmental evidence belongs in room scenes.
struct RoomProgressVisuals {
    uint8_t roomsVisitedMask;
    uint8_t stakeoutPips;
    bool stakeout;
};

struct Room2LightingFrame {
    bool valid = false;
    int counterX = 0;
    int counterY = 0;
    int podX = 0;
    int podY = 0;
    int signX = 0;
    int signY = 0;
    int bowlX = 0;
    int bowlY = 0;
    bool bowlHeldByPig = false;
    PigLight sign;
    PigLight window;
    PigLight pod;
    PigLight bowl;
};

struct PigPose {
    int drawX;
    int drawY;
    bool ramenEating;
    bool cupDrinking;
    int ingestHeadDip;
};

// Direct-face snout tips shared by attached props and room foreground effects.
// Keep these synchronized with Avatar::drawDirectFace().
static constexpr int kMenuPigNoseRightX = 58;
static constexpr int kMenuPigNoseLeftX = 14;

enum class BathIdlePhase : uint8_t {
    SOAK,
    PREP,
    SINK,
    SUBMERGED,
    RISE,
    RECOVER,
};

struct BathIdleFrame {
    BathIdlePhase phase = BathIdlePhase::SOAK;
    uint16_t phaseElapsedMs = 0;
    int8_t submergeY = 0;
    int8_t shakeX = 0;
    // Hidden by default: only the authored SOAK/late-RECOVER beats may show the
    // joint. A default-true flag is how the cig once survived the whole dive.
    bool jointVisible = false;
    bool smokeVisible = false;
};

struct PigNoirProfile {
    uint8_t depth8;
    uint8_t penetration8;
    uint8_t ambient8;
    uint8_t tintThreshold8;
    uint8_t tintScale8;
};

struct NoirVolumetricPassState {
    bool active = false;
    uint32_t startMs = 0;
    uint32_t durationMs = 1100;
    uint32_t nextTriggerMs = 0;
    int8_t dir = 1;
    float lastPigX = 120.0f;
};

struct ColorEventSample {
    bool active = false;
    uint16_t color565 = 0;
    uint8_t intensity = 0;
};

struct Room3CarEventState {
    bool initialized = false;
    bool active = false;
    uint32_t startMs = 0;
    uint32_t nextTriggerMs = 0;
    uint32_t lastDrawMs = 0;
    bool wdMode = false;
    bool wdReturnMode = false;
    bool wdPigMounted = false;
    float wdCarDropY = 0.0f;
};

struct DebugRoamingFrameOverride {
    bool active = false;
    uint32_t renderNow = 1200;
    int cookWx = 0;
    int cookWy = 0;
    int bedWx = 0;
    int bedWy = 0;
    bool overrideParallax = false;
    int parallaxFar = 0;
};

// ==[ SHARED CONSTANTS ]== used by multiple room files or shared helpers

static constexpr uint32_t NEON_CYCLE_MS = 2900;
static constexpr uint32_t WOBBLE_MS = 400;
static constexpr uint32_t BREATHE_MS = 3000;
static constexpr uint32_t SMOKE_CYCLE_MS = 2200;
static constexpr uint32_t ROOM_DRIP_CYCLE_MS = 3200;
static constexpr uint32_t POD_LED_MS = 1500;
static constexpr int kRamenDishShiftX = 8;
static constexpr int kRamenDishShiftY = 8;

// Room 3 car timing constants (shared between room3 and WD overlay in menu_pig.cpp)
static constexpr uint32_t ROOM3_CAR_DISTANT_DUR_MS = 2200;
static constexpr uint32_t ROOM3_CAR_APPROACH_DUR_MS = 3800;
static constexpr uint32_t ROOM3_CAR_EMERGE_DUR_MS = 1800;
static constexpr uint32_t ROOM3_CAR_HOVER_DUR_MS = 2700;
static constexpr uint32_t ROOM3_CAR_TURN_DUR_MS = 1000;
static constexpr uint32_t ROOM3_CAR_LINGER_DUR_MS = 2300;
static constexpr uint32_t ROOM3_CAR_ACCEL_DUR_MS = 4700;
static constexpr uint32_t ROOM3_CAR_COOL_DUR_MS = 3500;
static constexpr uint32_t ROOM3_CAR_DISTANT_END_MS = ROOM3_CAR_DISTANT_DUR_MS;
static constexpr uint32_t ROOM3_CAR_APPROACH_END_MS = ROOM3_CAR_DISTANT_END_MS + ROOM3_CAR_APPROACH_DUR_MS;
static constexpr uint32_t ROOM3_CAR_EMERGE_END_MS = ROOM3_CAR_APPROACH_END_MS + ROOM3_CAR_EMERGE_DUR_MS;
static constexpr uint32_t ROOM3_CAR_HOVER_END_MS = ROOM3_CAR_EMERGE_END_MS + ROOM3_CAR_HOVER_DUR_MS;
static constexpr uint32_t ROOM3_CAR_TURN_END_MS = ROOM3_CAR_HOVER_END_MS + ROOM3_CAR_TURN_DUR_MS;
static constexpr uint32_t ROOM3_CAR_LINGER_END_MS = ROOM3_CAR_TURN_END_MS + ROOM3_CAR_LINGER_DUR_MS;
static constexpr uint32_t ROOM3_CAR_ACCEL_END_MS = ROOM3_CAR_LINGER_END_MS + ROOM3_CAR_ACCEL_DUR_MS;
static constexpr uint32_t ROOM3_CAR_COOL_END_MS = ROOM3_CAR_ACCEL_END_MS + ROOM3_CAR_COOL_DUR_MS;

// WD mount sub-state timing
static constexpr float WD_WALK_TARGET_X = 210.0f;
static constexpr uint32_t WD_WALK_MS = 1600;
static constexpr uint32_t WD_JUMP_MS = 400;
static constexpr uint32_t WD_IMPACT_MS = 250;
static constexpr uint32_t WD_SNEAK_MS = 550;
static constexpr float WD_JUMP_HEIGHT = 20.0f;
static constexpr float WD_CAR_CENTER_X = 236.0f;
static constexpr float WD_CAR_BASE_Y = (float)(kFloorY - 80);
static constexpr float WD_CAR_ROOF_Y = WD_CAR_BASE_Y - 50.0f;
static constexpr float WD_CAR_CANOPY_Y = WD_CAR_ROOF_Y + 18.0f;
// Pig draw origin on the visible roof. The body ink is intentionally 8px
// right-heavy relative to the car center, matching the boot tableau.
static constexpr int WD_CAR_ROOF_PIG_X = (int)WD_CAR_CENTER_X - 28;
static constexpr int WD_CAR_ROOF_PIG_Y = (int)WD_CAR_ROOF_Y + 4;
static_assert(WD_CAR_ROOF_PIG_X % kPigPX == 0 &&
              WD_CAR_ROOF_PIG_Y % kPigPX == 0 &&
              WD_CAR_ROOF_PIG_X >= 0 &&
              WD_CAR_ROOF_PIG_X + kPigW <= SCREEN_WIDTH &&
              WD_CAR_ROOF_PIG_Y >= kRoomY &&
              WD_CAR_ROOF_PIG_Y + kPigH <= kFloorY,
              "wardrive roof pig must stay gridded inside the rooftop playfield");
static constexpr float WD_SNEAK_SHIFT_X = 10.0f;
static constexpr float WD_SNEAK_DROP_Y = 22.0f;
static constexpr uint32_t WD_ARRIVE_MS = 3000;
static constexpr uint32_t WD_CINE_SPEEDUP = 3;  // menu wardrive entry: rooftop car scene
static constexpr uint32_t WD_CANOPY_SWITCH_FADE_START_PCT = 24;
static constexpr uint32_t WD_CANOPY_SWITCH_CUT_PCT = 52;
static constexpr uint32_t WD_CANOPY_SWITCH_CUT_MS =
    ROOM3_CAR_LINGER_END_MS + (ROOM3_CAR_ACCEL_DUR_MS * WD_CANOPY_SWITCH_CUT_PCT) / 100u;

// ==[ SHARED STATE ]== defined in menu_pig.cpp, extern here for room files

// Core pig state
extern PigMode mode;
extern float pigX, pigY;
extern bool faceRight;
extern int currentRoom;
extern Station currentStation;
extern RoamState roamState;
extern RoomProgressVisuals roomProgress;

// Walking state
extern float walkFromX, walkFromY;
extern float walkToX, walkToY;
extern bool walkToFaceRight;
extern uint32_t walkStart;
extern Station walkTargetStation;

// Parallax
extern int parallaxFar, parallaxMid, parallaxNear;

// Furniture wobble
extern int wobbleX, wobbleY;
extern uint32_t wobbleStart;

// Neon sign timing
extern uint32_t neonCycleStart;

// Laptop (Room 0)
extern bool laptopScreenOn;
extern uint32_t lastLaptopBlink;
extern uint8_t laptopLineIdx;
extern uint32_t lastLaptopBubble;

// Room 2 lighting
extern Room2LightingFrame room2LightingRuntime;
extern bool podLedOn;
extern uint32_t podLedStart;

// Room 3 car
extern Room3CarEventState carState;
extern bool room3CinematicCarRunning;
extern bool wdCarForceStart;

// WD cinematic state
extern WDCinePhase wdCinePhase;
extern uint32_t wdCineStart;
extern WDMountPhase wdMountPhase;
extern uint32_t wdMountStart;
extern uint32_t wdImpactStart;
extern float wdMountFromX, wdMountFromY;
extern float wdJumpFromX, wdJumpFromY;
extern WDReturnPhase wdReturnPhase;
extern uint32_t wdReturnStart;
extern float wdReturnLandX, wdReturnLandY;

inline bool isWDMenuCineFast() { return wdCineStart != 0; }
inline uint32_t wdMenuCineElapsed(uint32_t rawElapsedMs) {
    return rawElapsedMs * WD_CINE_SPEEDUP;
}

// Noir volumetric pass
extern NoirVolumetricPassState noirPass;
extern ColorEventSample colorEvent;

// Room mood
extern RoomMood roomMood;

// Room 1 window state
extern bool windowCigLit;
extern uint32_t roomDripStart;

// Wire spark (helper + rooms)
extern bool sparkActive;
extern uint32_t sparkStart;
extern uint32_t nextSparkTime;

// Pipe drip (helper)
extern uint32_t dripCycleStart;

// Debug overrides
extern DebugRoamingFrameOverride debugRoamingFrame;

// Face timer
extern PigFaceTimer faceTimer;

// Rear cinematic
extern RearCinematicPhase rearCinePhase;
extern bool rearCineOverrideRear;

// 4th wall break
extern bool wallBreakActive;

// Carrying cup
extern bool carryingCup;
extern uint32_t cupPickupTime;

// Portal jump
extern bool portalJumpActive;
extern uint32_t portalJumpStart;

// Chair leg burst
extern uint32_t nextChairLegBurstAtMs;
extern uint32_t chairLegBurstStartMs;

// Arrival animation
extern uint32_t arrivalAnimStart;

// Bed momentum
extern uint32_t lastBedMomentum;

// ==[ LIMB RENDERING ]== shared by room pigs + NPC barman

struct LimbBump { uint16_t hiL, hiR, shadow; };
LimbBump computeLimbBump(uint16_t fg, uint16_t bg, int limbCX, int limbCY,
                         PigLight light, float vol = 1.0f);
void drawMaskRows(M5Canvas& canvas, int x, int y,
                  const char* const* rows, int rowCount, int rowWidth,
                  char token, uint16_t color, bool mirrorX = false);
void drawFilledUHand(M5Canvas& canvas, uint16_t fg, uint16_t bg,
                     int x, int y, bool openLeft, int handW, int handH,
                     PigLight light, float vol = 1.0f);
void drawHandBridge(M5Canvas& canvas, uint16_t fg,
                    int x, int y, bool openLeft, int handW);

// ==[ SHARED HELPER FUNCTIONS ]== defined in menu_pig.cpp

bool isNeonOn(uint32_t now);
bool isRoom0SignLit(uint32_t now);
// Station-owned occlusion stays coherent through local teleport reconstruction
// and rear-pose settling without mutating gameplay state early.
bool isStationVisualActive(Station station);
PigPose resolveStationVisualPose(uint32_t now);
void calcWobble(uint32_t now);
PigLight getRoomNeonLight(int room, uint32_t now);
uint16_t neonPulse(uint32_t now);
void drawEmitterFog(M5Canvas& canvas, uint32_t now,
                    int sourceX, int sourceY, int reflectY,
                    uint16_t tint, bool sourceActive);
void drawWindowDust(M5Canvas& canvas, uint16_t fg, uint32_t now, int wdx = 0, int wdy = 0);
void drawNeonArrow(M5Canvas& canvas, uint32_t now, int x, int y);
float smootherstep(float t);
int calcBreathe(uint32_t now);
int calcWalkBounce();
uint32_t randomRange(uint32_t lo, uint32_t hi);

// Noir rendering helpers (used by rooms 1, 3)
void updateNoirVolumetricPass(uint32_t now);
void drawNoirVolumetricPass(M5Canvas& canvas, uint32_t now, int wx, int wy, int ww, int wh);
void drawNoirBlindShadowSweep(M5Canvas& canvas, uint32_t now,
                               int wx, int wy, int ww, int wh);
void drawNoirWindow(M5Canvas& canvas, uint32_t now,
                    int wx, int wy, int ww, int wh,
                    float pigPosX = 120.0f, uint16_t reflectionTint = 0);
void drawNoirWindowBase(M5Canvas& canvas,
                        int wx, int wy, int ww, int wh,
                        float pigPosX = 120.0f);
void drawNoirWindowMotion(M5Canvas& canvas, uint32_t now,
                          int wx, int wy, int ww, int wh,
                          float pigPosX = 120.0f,
                          uint16_t reflectionTint = 0);
void shadeRoomShadowPx(M5Canvas& canvas, int x, int y, float factor,
                        uint16_t tint = 0, uint8_t tintAmt = 0);
void lightRoomBeamPx(M5Canvas& canvas, int x, int y, uint16_t light, float strength);

// City window (used by room 3)
void drawCityWindow(M5Canvas& canvas, uint32_t now,
                     int wx, int wy, int ww, int wh, float pigPosX,
                     uint16_t glassTint);
void drawCityWindowBase(M5Canvas& canvas,
                        int wx, int wy, int ww, int wh, float pigPosX);
void drawCityWindowMotion(M5Canvas& canvas, uint32_t now,
                          int wx, int wy, int ww, int wh, float pigPosX,
                          uint16_t glassTint);
void drawCityWindowShadowSweep(M5Canvas& canvas, uint32_t now,
                                int wx, int wy, int ww, int wh);
void applyCarShadowToRect(M5Canvas& canvas, uint32_t now,
                           int wx, int wy, int ww, int wh,
                           int rx, int ry, int rw, int rh, int pixSize);
void drawCarLightVolumes(M5Canvas& canvas, uint32_t now,
                          int originX, int originY, int sweepWidth, int sweepH);
void clearPigEffectSnapshot();
void capturePigEffectSnapshot(M5Canvas& canvas, const PigPose& pose,
                              PigLight light);
bool isPigEffectPixel(int x, int y, uint16_t currentColor);
void applyDirectionalPigNoir(M5Canvas& canvas, const PigNoirProfile& profile);

// Room lighting helpers. One selector owns shadow, bump, and noir direction.
float room2EmitterScoreAt(const PigLight& light, float px, float py,
                           float radius, float weight);
Room2LightingFrame buildRoom2Emitters(int cookWx, int cookWy, int bedWx, int bedWy,
                                       int bowlFx, int bowlFy, bool bowlHeldByPig,
                                       int signParallax, uint32_t now);
PigLight selectRoom0PigKeyLight(int pigDrawX, int pigDrawY, uint32_t now);
PigLight selectRoom1PigKeyLight(int pigDrawX, int pigDrawY, uint32_t now);
PigLight selectRoom2PigKeyLight(int pigDrawX, int pigDrawY, uint32_t now);
PigLight selectRoom3PigKeyLight(int pigDrawX, int pigDrawY, uint32_t now);
PigLight selectRoom4PigKeyLight(int pigDrawX, int pigDrawY, uint32_t now);
PigLight selectRoom5PigKeyLight(int pigDrawX, int pigDrawY, uint32_t now);
PigLight selectRoomPigKeyLight(int room, int pigDrawX, int pigDrawY, uint32_t now);

// Room 2 helpers (defined in rooms/room2_ramen.cpp, called from menu_pig.cpp pig body)
void drawRamenVapor(M5Canvas& canvas, uint32_t now, int baseX, int baseY);
void drawRoom2CounterTopShadow(M5Canvas& canvas, int pigDrawX, int pigDrawY,
                                uint32_t now, PigLight light);
// Shared living greenery. Each room supplies a compile-time-checked root;
// one renderer owns slow growth and RF fruit semantics.
void drawRamenRfGreenery(M5Canvas& canvas, uint32_t now);
void drawComfortRfGreenery(M5Canvas& canvas, uint32_t now);

// Pose / state query helpers
PigPose resolvePigPose(uint32_t now, bool isMoving, bool useRearView,
                        bool allowPortalJumpFinish);
bool isRoamingMoveState();
bool isBathJumpActive();
float getBathJumpProgress(uint32_t now);
BathIdleFrame sampleBathIdleFrame(uint32_t now);
bool shouldUseRearViewInRoaming();
bool isRearIdleStation();
bool isWDCinematicActive();
bool isWDMountFrontViewLocked();
bool isWDPigInsideCar();
bool isWDPigSneakingIntoCabin();
float getWDCanopySwitchFade(uint32_t now);
void drawWDCanopySwitchOverlay(M5Canvas& canvas, uint32_t now);
bool isRamenEatingState(bool useRearView);
bool isCupDrinkingState(bool useRearView);
void getHeldBowlPosition(int pigDrawX, int pigDrawY, int& bowlX, int& bowlY);
bool isChairLegBurstActive(uint32_t now);
int calcChairUpperBodyBob(uint32_t now);
void drawChairUpperBodySeam(M5Canvas& canvas, int x, int y, int upperBobY,
                             uint16_t pigFill, uint16_t bg);

// Sleep bubble
void drawSleepBubble(M5Canvas& canvas);

// ==[ ROOM DRAW FUNCTIONS ]== extracted to src/ui/rooms/*.cpp

enum class RoomRenderPass : uint8_t {
    BASE,
    LIVE,
};

void drawRoom0(M5Canvas& canvas, uint32_t now, RoomRenderPass pass);
void drawRoom0Foreground(M5Canvas& canvas, uint32_t now);

void drawRoom1(M5Canvas& canvas, uint32_t now, RoomRenderPass pass);
void drawRoom1Foreground(M5Canvas& canvas, uint32_t now);

void drawRoom2(M5Canvas& canvas, uint32_t now, RoomRenderPass pass);
void drawRoom2Foreground(M5Canvas& canvas, uint32_t now);

void drawRoom3(M5Canvas& canvas, uint32_t now, RoomRenderPass pass);
void drawRoom3CinematicCar(M5Canvas& canvas, uint32_t now);
void drawRoom3Foreground(M5Canvas& canvas, uint32_t now);

void drawRoom4(M5Canvas& canvas, uint32_t now, RoomRenderPass pass);
void drawRoom4Foreground(M5Canvas& canvas, uint32_t now);
void restoreRoom4CategoricalSources(M5Canvas& canvas);

void drawRoom5(M5Canvas& canvas, uint32_t now, RoomRenderPass pass);
void drawRoom5Foreground(M5Canvas& canvas, uint32_t now);

} // namespace MenuPig
