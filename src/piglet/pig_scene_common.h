// pig_scene_common.h — shared state + helpers for avatar subsystem files
// INTERNAL HEADER — include from avatar.cpp, pig_tree.cpp, pig_waves.cpp, pig_grass.cpp
#pragma once

#include "avatar.h"
#include "../ui/display.h"  // includes gfx.h
#include <M5Unified.h>
#include <stdint.h>
#include <cmath>

// ==[ SHARED FILE-SCOPE STATE ]==
// Defined in avatar.cpp, accessed by subsystem .cpp files via extern.

// Scenery fat-pixel grid (cached per frame from Display::getSceneryPX())
extern int16_t PX;

// Thunder flash inversion state
extern bool thunderFlashActive;

// ==[ SHARED STRUCTS ]== used across subsystem boundaries

struct WaveRing { int16_t cx, cy; };

struct WaveState {
    uint8_t intensity = 3;
    static constexpr uint8_t MAX_RINGS = 5;
    WaveRing rings[5] = {};
    uint32_t ringLastCycle[5] = {};
    bool ringsInitialized = false;
    bool treeShaking = false;
    uint32_t treeShakeStart = 0;
};
extern WaveState wave;

struct CollisionState {
    bool treeColliding = false;
    bool wasTreeColliding = false;
    int8_t treeCollisionShake = 0;
    uint32_t lastTreeShakeSparkle = 0;
};
extern CollisionState collision;

struct DroppingFruit {
    int16_t x, y;
    uint8_t radius;
    uint32_t dropStart;
    bool active;
};
static constexpr uint8_t MAX_DROPPING = 4;
extern DroppingFruit droppingFruits[MAX_DROPPING];

struct FruitSplash {
    float x, y;
    float vx, vy;
    uint8_t size;
    uint32_t spawnTime;
    bool active;
};
static constexpr uint8_t FRUIT_SPLASH_COUNT = 8;
extern FruitSplash fruitSplashes[FRUIT_SPLASH_COUNT];
extern uint8_t fruitSplashIdx;

struct TrailParticle {
    float x, y;
    float vx, vy;
    float startX;
    float maxDist;
    uint8_t baseSize;
    bool active;
};
static constexpr int TRAIL_COUNT = 10;
extern TrailParticle trailParticles[TRAIL_COUNT];
extern uint32_t lastTrailSpawn;
extern uint32_t lastTrailUpdate;
extern int trailSpawnIdx;

struct HypeState {
    bool unlocked = false;
    bool fillActive = false;
    uint32_t fillStart = 0;
    uint8_t fillSeed = 0;
    static constexpr uint16_t FILL_MS = 2500;
    static constexpr uint8_t FILL_ROWS = 21;
    static constexpr uint8_t FILL_COLS = 6;
    uint8_t fillCount[21];
};
extern HypeState hype;

// ==[ GEOMETRY CONSTANTS ]== all derived from PIG_PX
static constexpr int16_t PIG_PX_CONST = 2;
static constexpr int16_t PIG_BODY_W_CONST = 6 * PIG_PX_CONST * 6;                         // 72px
static constexpr int16_t PIG_BODY_H_CONST = 42;
static constexpr int16_t PIG_MIN_X_CONST = 2;
static constexpr int16_t PIG_MAX_X_CONST = SCREEN_WIDTH - PIG_BODY_W_CONST - 4;           // 244
static constexpr int16_t PIG_Y_CONST = SCREEN_HEIGHT - BOTTOM_BAR_H - PIG_BODY_H_CONST - 5;  // 179
static constexpr int16_t PIG_CENTER_X_CONST = SCREEN_WIDTH / 2 - PIG_BODY_W_CONST / 2;    // 124
static constexpr int16_t GRASS_BASE_Y = PIG_Y_CONST + PIG_BODY_H_CONST + 2;         // 217

// Nose anchor positions (matching ASCII pig: snout front tip)
// Snout drawn at snoutX+12 center, spans snoutX+4 to snoutX+20 (16px wide)
// snoutX = 38 (right) or 10 (left), so tip = 58 (right) or 14 (left)
static constexpr int16_t NOSE_RIGHT_X = 58;  // right snout tip (38 + 20)
static constexpr int16_t NOSE_LEFT_X  = 14;  // left snout tip (10 + 4)
static constexpr int16_t NOSE_AWAY_X  = 36;  // body center rear view
static constexpr int16_t NOSE_Y       = 21;  // face line vertical center (14 + 7)

// ==[ SHARED HELPERS ]== canonical impls in gfx/gfx.h
static inline uint8_t hash8(uint16_t v) { return Gfx::hash8(v); }
static inline int16_t snapToPx(int16_t v, int16_t px) { return Gfx::snapToPx(v, px); }

static inline int16_t snapPx(int16_t v) {
    return snapToPx(v, PX);
}

static inline int16_t reflectAxis(int16_t v, int16_t hi, uint8_t& bounces) {
    for (uint8_t i = 0; i < 4; i++) {
        if (v >= 0 && v <= hi) return v;
        if (v < 0) { v = -v;           bounces++; }
        else       { v = hi + hi - v;   bounces++; }
    }
    return (v < 0) ? 0 : (v > hi) ? hi : v;
}

// Color helpers (thunder flash inversion)
static inline uint16_t getDrawColor() {
    if (thunderFlashActive) return Display::getColorBG();
    return Display::getColorFG();
}

static inline uint16_t getBGColor() {
    if (thunderFlashActive) return Display::getColorFG();
    return Display::getColorBG();
}

static inline void fatLinePx(M5Canvas& canvas, int16_t x1, int16_t y1,
                              int16_t x2, int16_t y2, uint16_t color, int16_t px) {
    Gfx::fatLinePx(canvas, x1, y1, x2, y2, color, px);
}

static inline void fatLine(M5Canvas& canvas, int16_t x1, int16_t y1,
                            int16_t x2, int16_t y2, uint16_t color) {
    fatLinePx(canvas, x1, y1, x2, y2, color, PX);
}

// ==[ WALK LOOK STATE ]== used by grass movement control
struct WalkLookState {
    bool facingRight = true;
    uint32_t lastFlipTime = 0;
    uint32_t flipInterval = 5000;
    uint32_t lastLookTime = 0;
    uint32_t lookInterval = 2000;
    uint32_t grassWanderTimer = 0;
    uint32_t grassWanderInterval = 4000;
};
extern WalkLookState walkLook;

// ==[ POSITION OWNERSHIP ]== priority-based position control
enum class PosOwner : uint8_t {
    IDLE = 0,
    GRASS_WALK,
    WALK_TRANS,
    ATTACK_HOP,
    PORTAL_PULL,
    CINEMATIC
};

struct PositionControl {
    PosOwner owner = PosOwner::IDLE;
    bool canClaim(PosOwner p) const { return (uint8_t)p >= (uint8_t)owner; }
    void claim(PosOwner p) { if (canClaim(p)) owner = p; }
    void release(PosOwner p) { if (owner == p) owner = PosOwner::IDLE; }
};
extern PositionControl posControl;

// Rainbow color helpers (shared for hype rainbow)
extern bool shouldUseHypeRainbow();
extern uint16_t trippyRainbow(int16_t screenX, int16_t screenY);
