// Weather effects module - clouds, rain, thunder, wind
// Mood-tied weather system ported from Porkchop

#include "weather.h"
#include "avatar.h"
#include "mood.h"
#include "../ui/display.h"
#include "../audio/sfx.h"
#include "../haptic/haptic.h"
#include "../hamlet.h"
#include "../util/debug_log.h"
#include "../util/time_math.h"
#include <esp_random.h>
#include <esp_heap_caps.h>

namespace Weather {

// Forward declarations (internal-only, removed from header)
void setRaining(bool active);
bool getShuttleRect(int16_t& x, int16_t& y, int16_t& w, int16_t& h);

// ==[ TUNING CONSTANTS ]==

// --- Thunder timing (mood-based intervals) ---
static constexpr uint32_t THUNDER_MIN_INTERVAL_VERYSAD  = 30000;   // ms — tier 2 (very sad)
static constexpr uint32_t THUNDER_MAX_INTERVAL_VERYSAD  = 60000;
static constexpr uint32_t THUNDER_MIN_INTERVAL_SAD      = 60000;   // ms — tier 1 (sad)
static constexpr uint32_t THUNDER_MAX_INTERVAL_SAD      = 120000;
static constexpr uint32_t THUNDER_INTERVAL_DISABLED      = 999999;  // effectively never
static constexpr uint32_t THUNDER_INIT_MIN_INTERVAL      = 50000;   // default boot values
static constexpr uint32_t THUNDER_INIT_MAX_INTERVAL      = 90000;
static constexpr uint32_t THUNDER_INIT_NEXT_INTERVAL     = 70000;
static constexpr uint8_t  THUNDER_FLASH_MIN_COUNT        = 2;       // flashes per burst
static constexpr uint8_t  THUNDER_FLASH_MAX_COUNT        = 5;       // exclusive upper bound
static constexpr uint32_t THUNDER_BRIGHT_MIN_MS          = 30;      // bright flash duration
static constexpr uint32_t THUNDER_BRIGHT_MAX_MS          = 60;
static constexpr uint32_t THUNDER_DARK_MIN_MS            = 20;      // dark gap duration
static constexpr uint32_t THUNDER_DARK_MAX_MS            = 40;

// --- Wind parameters ---
static constexpr float    WIND_BASE_SPEED                = 2.0f;    // base particle speed
static constexpr int      WIND_SPEED_VARIANCE            = 30;      // /10.0 added to base
static constexpr int      WIND_TRAVEL_MIN                = 180;     // min max-travel distance
static constexpr int      WIND_TRAVEL_MAX                = 281;     // exclusive upper bound
static constexpr uint32_t WIND_GUST_MIN_MS               = 2000;    // gust duration range
static constexpr uint32_t WIND_GUST_MAX_MS               = 4000;
static constexpr uint32_t WIND_SPAWN_MIN_GRASS_ON        = 3000;    // interval when walking
static constexpr uint32_t WIND_SPAWN_MAX_GRASS_ON        = 8000;
static constexpr uint32_t WIND_SPAWN_MIN_GRASS_OFF       = 15000;   // interval when still
static constexpr uint32_t WIND_SPAWN_MAX_GRASS_OFF       = 30000;
static constexpr int      WIND_CHANCE_GRASS_ON           = 70;      // % spawn chance
static constexpr int      WIND_CHANCE_GRASS_OFF          = 20;

// --- Bird parameters ---
static constexpr int8_t   BIRD_SPAWN_Y_MIN_OFFSET        = 4;      // added to SKY_TOP_Y
static constexpr int8_t   BIRD_SPAWN_Y_MAX_OFFSET        = 24;     // exclusive
static constexpr int8_t   BIRD_VX_RIGHT_MIN              = 1;
static constexpr int8_t   BIRD_VX_RIGHT_MAX              = 3;      // exclusive
static constexpr int8_t   BIRD_VX_LEFT_MIN               = -2;
static constexpr int8_t   BIRD_VX_LEFT_MAX               = 0;      // exclusive
static constexpr uint32_t BIRD_SPAWN_MIN_MS              = 15000;
static constexpr uint32_t BIRD_SPAWN_MAX_MS              = 30001;   // exclusive
static constexpr float    BIRD_FALL_INITIAL_VY           = -1.5f;
static constexpr float    BIRD_FALL_GRAVITY              = 0.4f;

// --- Rain parameters ---
static constexpr uint8_t  RAIN_SPEED_MIN                 = 5;       // drop speed range
static constexpr uint8_t  RAIN_SPEED_MAX                 = 9;       // exclusive
static constexpr int      RAIN_CHANCE_VERYSAD            = 70;      // % when tier 2
static constexpr int      RAIN_CHANCE_SAD                = 35;      // % when tier 1

// --- Shockwave parameters ---
static constexpr uint32_t SHOCKWAVE_MUSHROOM_TIMEOUT_MS  = 30000;   // safety net
static constexpr uint16_t SHOCKWAVE_EXPANSION_TICKS      = 80;      // ~4s dome growth
static constexpr uint32_t SHOCKWAVE_DOME_HOLD_MS         = 550;     // filled hold
static constexpr float    SAND_EROSION_SPAN              = 6.0f;    // rows behind front
static constexpr uint32_t TREE_SUPPRESS_COOLDOWN_MS      = 60000;   // post-nuke cooldown

// --- Shuttle spawn/respawn timing ---
static constexpr uint32_t SHUTTLE_SPAWN_MIN_MS           = 45000;
static constexpr uint32_t SHUTTLE_SPAWN_MAX_MS           = 120001;  // exclusive
static constexpr uint32_t SHUTTLE_RESPAWN_MIN_MS         = 600000;  // 10 min cooldown
static constexpr uint32_t SHUTTLE_RESPAWN_MAX_MS         = 1200001; // 20 min (exclusive)

// === CLOUD SHAPE SYSTEM ===
struct CloudPuff {
    int8_t dx, dy;      // offset from cloud center
    uint8_t radius;     // circle radius (2-6px)
};

struct CloudShape {
    float x;            // current X position (float for smooth drift)
    int8_t y;           // Y center (19-26, below TOP_BAR_H=14)
    uint8_t puffCount;  // 3-5 overlapping circles per cloud
    CloudPuff puffs[5];
    uint8_t scale;      // 0-255 growth animation (controls drawn radius)
    bool active;
    bool growing;       // scaling up toward 255
    bool shrinking;     // scaling down toward 0, deactivates at 0
};

static const uint8_t MAX_CLOUDS = 8;
static uint32_t lastCloudUpdate = 0;
static const uint16_t cloudSpeed = 14400;  // Ultra slow atmospheric drift
static uint32_t lastCloudParallax = 0;
static const uint8_t CLOUD_PARALLAX_GRASS_SHIFTS = 6;  // Shift clouds every N grass shifts
static uint32_t lastDensityCheck = 0;
static uint32_t lastScaleUpdate = 0;

// === RAIN STATE ===
struct RainDrop {
    float x;
    float y;
    uint8_t speed; // pixels per update for visible rain
};
static const int RAIN_DROP_COUNT = 25;
static bool rainActive = false;
static bool rainDecided = false;
static int lastMoodTier = -1;
static uint32_t lastRainUpdate = 0;
static const uint16_t RAIN_REFERENCE_MS = 30;

// === THUNDER STATE ===
static bool thunderFlashing = false;
static uint32_t lastThunderStorm = 0;
static uint32_t thunderFlashStart = 0;
static uint8_t thunderFlashesRemaining = 0;
static uint8_t thunderFlashState = 0;
static uint32_t thunderFlashDuration = 0;  // stored threshold per state (not rerolled per frame)
static bool thunderBurstFiring = false;    // SFX/haptic gate: true while burst in progress
static uint32_t thunderMinInterval = THUNDER_INIT_MIN_INTERVAL;
static uint32_t thunderMaxInterval = THUNDER_INIT_MAX_INTERVAL;
static uint32_t nextThunderInterval = THUNDER_INIT_NEXT_INTERVAL;

// === WIND STATE ===
struct WindParticle {
    float x;
    float y;
    float speed;
    float spawnX;
    float maxTravel;
    uint8_t baseSize;
    bool active;
    bool dirRight;
};
static const int WIND_COUNT = 6;
static bool windActive = false;
static uint32_t lastWindGust = 0;
static uint32_t windGustDuration = 0;
static uint32_t windGustInterval = 15000;
static uint32_t lastWindUpdate = 0;

// === MOOD-BASED WEATHER CONTROL ===
static int currentMood = 50;

// === BIRD SYSTEM ===
struct SkyBird {
    float x;
    int8_t y;
    int8_t vx;
    uint8_t sinePhase;
    bool active;
    bool falling;
    float fallVy, fallX, fallY;
    float fallStartY;
    uint32_t lastWaveCheck;  // debounce wave collision (500ms cooldown)
};

struct BirdSpark {
    float x, y, vx, vy;
    uint8_t life;
};

struct BirdExplosion {
    float x, y;
    uint8_t radius;
    uint8_t maxRadius;
    uint8_t life;
    bool active;
};

struct ImpactSplash {
    float x, y, vx, vy;
    uint8_t life;
    bool active;
};

static const int BIRD_COUNT = 2;
static const int SPARK_COUNT = 6;
static const int EXPLOSION_COUNT = 2;
static const int SPLASH_COUNT = 6;

static int8_t whistlingBird = -1;
static uint32_t lastBirdUpdate = 0;
static uint32_t nextBirdSpawn = 0;

// ==[ AUTO-FIRE ]== pig shoots waves at visible targets during hunt
static uint32_t autoFireNextShot = 0;   // millis() when next shot allowed
static uint8_t  autoFireRemaining = 0;  // attempts left for current target sighting
static uint32_t autoFireCooldown = 0;   // ms between shots in a burst

// 4px grid — ship scene + rain stay fixed on this
static constexpr int16_t BIRD_PX = 4;
static inline int16_t birdSnap(int16_t v) {
    return (v >= 0) ? (v / BIRD_PX) * BIRD_PX : ((v - (BIRD_PX - 1)) / BIRD_PX) * BIRD_PX;
}

// Runtime-tunable grid for ordinary birds and wind. Sun, rain, clouds, ship,
// and impact geometry retain the authored 4px BIRD_PX contract.
static int16_t SKY_PX = 4;
static inline int16_t skySnap(int16_t v) {
    return (v >= 0) ? (v / SKY_PX) * SKY_PX : ((v - (SKY_PX - 1)) / SKY_PX) * SKY_PX;
}

// Coordinate envelopes (320x240 panel, top HUD = 20px, sky draw band = 20..216):
// sun: x 20..300, y 28..115
// birds: x -30..350, y 24..44 (+1 bob)
// shuttle (materialized): x -50..370, y 22..160
// mushroom anchor: x 24..296, ground y ~=192
// shockwave origin: y=190, radius 0..400
// Main action-safe area keeps cinematic objects readable alongside pig + UI.
static constexpr int16_t SKY_TOP_Y = TOP_BAR_H;                             // 14
static constexpr int16_t SKY_BOTTOM_Y = SCREEN_HEIGHT - BOTTOM_BAR_H - 4;   // 222
static constexpr int16_t FOV_SAFE_LEFT = 24;
static constexpr int16_t FOV_SAFE_RIGHT = 296;
static constexpr int16_t IMPACT_LEFT_X = 80;
static constexpr int16_t IMPACT_CENTER_X = 160;
static constexpr int16_t IMPACT_RIGHT_X = 240;
// ground baseline: grass surface for impacts, mushroom root, bird splats
static constexpr int16_t GROUND_Y = SCREEN_HEIGHT - BOTTOM_BAR_H - 28;  // 192
// shockwave origin: just above ground
static constexpr float SHOCKWAVE_ORIGIN_Y = (float)(GROUND_Y - 2);      // 190
// max dome radius: covers entire screen diagonally
static constexpr float DOME_MAX_RADIUS = 400.0f;

static inline bool rectIntersectsFov(int16_t x, int16_t y, int16_t w, int16_t h) {
    if (w <= 0 || h <= 0) return false;
    if (x + w <= FOV_SAFE_LEFT || x >= FOV_SAFE_RIGHT) return false;
    if (y + h <= SKY_TOP_Y || y >= SKY_BOTTOM_Y) return false;
    return true;
}


static int16_t choosePigDodgeTarget(float impactX, int16_t pigX) {
    float clampedImpact = Gfx::clampf(impactX, 0.0f, (float)(SCREEN_WIDTH - 1));
    float desired = (clampedImpact > (float)(SCREEN_WIDTH / 2)) ? 2.0f : 208.0f;
    if (clampedImpact > 127.0f && clampedImpact < 193.0f) {
        desired = (pigX < 106) ? 208.0f : 2.0f;
    }
    if (desired > (float)pigX - 16.0f && desired < (float)pigX + 16.0f) {
        desired = (desired < 106.0f) ? 2.0f : 208.0f;
    }
    return (int16_t)desired;
}

// ==[ NARCISSUS SHUTTLE SYSTEM ]==
// 100% cinematic atmospheric re-entry: 5-minute sequence from faint dot to full materialization.
// Hittable by waves → debris fragments → mushroom cloud on impact.

// --- NARCISSUS SHUTTLE SPRITES (fat-pixel grid, MSB = leftmost column) ---
// Side view: 8 cols x 4 rows (24x12 real px). asymmetric: engine at back, nose at front.
static const uint8_t NARCISSUS_W = 8;
static const uint8_t NARCISSUS_H = 4;
static const uint8_t NARCISSUS_R[4] = {
    0b10111100,  // engine + canopy
    0b11111111,  // full hull
    0b11111110,  // lower hull
    0b00011000   // landing skids
};
static const uint8_t NARCISSUS_L[4] = {
    0b00111101,  // canopy + engine (mirrored)
    0b11111111,  // full hull
    0b01111111,  // lower hull
    0b00011000   // landing skids
};
// Turn frame A: 3/4 view (6x4) — foreshortened, canopy compressing
static const uint8_t NARC_TURN_A_W = 6;
static const uint8_t NARC_TURN_A[4] = {
    0b011110,  // compressed canopy
    0b111111,  // full hull foreshortened
    0b111110,  // lower hull
    0b001100   // skids
};
// Turn frame F: head-on view (4x4) — narrowest, symmetrical cross-section
static const uint8_t NARC_TURN_F_W = 4;
static const uint8_t NARC_TURN_F[4] = {
    0b0110,    // top profile
    0b1111,    // fuselage cross-section
    0b1111,    // lower fuselage
    0b0110     // skids
};
// Turn frame B: 3/4 opposite (6x4) — mirror of A, completing rotation
static const uint8_t NARC_TURN_B_W = 6;
static const uint8_t NARC_TURN_B[4] = {
    0b011110,  // mirrored canopy
    0b111111,
    0b011111,  // lower hull bias flipped
    0b001100
};

// --- PHASE ENUMS ---
enum ShuttlePhase : uint8_t {
    SPHASE_DOCKED,           // invisible (unused — all spawns are cinematic)
    // ==[ ATMOSPHERIC REENTRY ]== 6 phases, ~5 minutes total
    SPHASE_FIRST_LIGHT,      // faint blinking PIG_PX dot, barely moving
    SPHASE_WAKE_FORMING,     // trail starts, dot solidifies
    SPHASE_HEATING,          // plasma grows, bow shock forms
    SPHASE_FIREBALL,         // peak brightness, sparks, max trail, screen shake
    SPHASE_DECELERATION,     // glow fades, object grows, hull hints
    SPHASE_COOLING,          // shape resolves via pixel reveal, thrusters ignite
    SPHASE_MATERIALIZED,     // normal flight
    SPHASE_FALLING           // hit by wave — broken, falling with hype trails
};

// --- REENTRY TIMING (50ms ticks, total 6000 ticks = 300s = 5 min) ---
static constexpr uint16_t SH_FIRST_LIGHT_TICKS = 1500;  // 75s
static constexpr uint16_t SH_WAKE_TICKS        = 1100;  // 55s
static constexpr uint16_t SH_HEATING_TICKS     = 1000;  // 50s
static constexpr uint16_t SH_FIREBALL_TICKS    = 800;   // 40s
static constexpr uint16_t SH_DECEL_TICKS       = 900;   // 45s
static constexpr uint16_t SH_COOLING_TICKS     = 700;   // 35s
// Reentry dot size — fixed 2px, smaller than BIRD_PX
static constexpr int16_t REENTRY_PX = 2;

// --- STRUCTS ---

struct ShuttleTrail {
    int16_t x, y;
    uint8_t age;  // 0 = fresh, increases each update
};
static constexpr uint8_t TRAIL_LEN = 12;

// Extended wake trail for cinematic reentry (comet tail)
static constexpr uint8_t WAKE_LEN = 24;
struct WakeParticle {
    int16_t x, y;
    uint8_t age;     // 0=fresh, 255=dead
    int8_t spread;   // perpendicular offset for comet tail width
};

// Bow shock crescent (forms ahead of object during heating/fireball)
struct BowShock {
    uint8_t innerR;   // inner crescent radius in BIRD_PX (0-3)
    uint8_t outerR;   // outer crescent radius (0-5)
    uint8_t flicker;  // dropout % (0-50)
};

// PIG_PX-sized thruster at shuttle corners
static constexpr uint8_t THRUSTER_COUNT = 4;
struct Thruster {
    uint8_t on;         // 0=off, 1=on
    uint8_t flameTicks; // remaining burn ticks
};

enum ShuttleBehavior : uint8_t { SH_CRUISE, SH_DRIFT, SH_LANDING };

struct Shuttle {
    float x;
    float yFloat;        // smooth Y
    int8_t vx;           // direction sign * base speed
    float yVel;          // vertical velocity for maneuvers
    bool active;
    ShuttleBehavior behavior;
    uint16_t driftPhase;  // sine phase for DRIFT
    float baseY;         // original altitude
    bool ascending;      // LANDING: climbing back up
    bool fromNostromo;      // spawned from mothership (always true now)
    uint8_t enginePhase;    // trail animation counter
    uint32_t lastWaveCheck; // debounce wave collision (500ms cooldown)
    ShuttleTrail trail[TRAIL_LEN];
    uint8_t trailIdx;
    // maneuvering (random direction flips during normal flight)
    uint32_t maneuverTimer;     // millis() for next maneuver check
    bool maneuverPending;       // direction flip queued
    uint8_t bankPhase;          // banking animation tick (0 = not banking, 1-20 = turning)
    // cinematic entry
    ShuttlePhase matPhase;
    uint16_t matTick;        // tick counter within current phase
    uint8_t revealMask[4];   // pixel reveal (AND with NARCISSUS sprite)
    uint8_t revealedCount;
    float reentryVy;         // downward velocity during descent
    bool thrusterBlink;      // thruster row flicker toggle
    float phaseStartY;       // Y at start of current cinematic phase
    float cinematicTargetY;  // target cruising altitude
    // compound Bezier trajectory (2 segments for 5-min arc)
    float bezA_P0x, bezA_P0y;   // segment A: launch point
    float bezA_P1x, bezA_P1y;   // segment A: control (gentle drift)
    float bezA_P2x, bezA_P2y;   // segment A: handoff midpoint (= segment B start)
    float bezB_P1x, bezB_P1y;   // segment B: control (gravity sag)
    float bezB_P2x, bezB_P2y;   // segment B: cruise endpoint
    float bezT;                  // parametric progress [0, 0.88]
    // post-entry slowdown
    uint16_t postEntryTick;  // ticks since materialization (0 = just materialized)
    // falling state (hit by wave)
    float fallVy;            // vertical velocity during fall
    uint16_t fallTick;       // ticks since entering SPHASE_FALLING
    uint8_t fallDitherSeed;  // randomize breakup pattern
    // ==[ REENTRY ADDITIONS ]==
    // wake trail (cinematic comet tail)
    WakeParticle wake[WAKE_LEN];
    uint8_t wakeIdx;
    // bow shock
    BowShock bowShock;
    // thrusters (PIG_PX-sized, visible in COOLING + MATERIALIZED)
    Thruster thrusters[THRUSTER_COUNT];
    int8_t thrustTilt;       // current tilt from thrusters (-2 to +2)
    // size tracking (grows PIG_PX→BIRD_PX during reentry)
    uint8_t pixelGrid;       // 2=REENTRY_PX or 3=BIRD_PX
    uint8_t drawW, drawH;   // logical size in grid units
    // ablation sparks (fireball)
    uint16_t lastAblationTick;
    // phrase tracking (don't repeat)
    bool phraseSent;
};
static uint32_t lastShuttleUpdate = 0;
static uint32_t lastMushroomUpdate = 0;
static uint32_t lastDebrisUpdate = 0;
static uint32_t nextShuttleSpawn = 0;
static bool shuttleSpawnTimerSet = false;
static uint32_t treeSuppressUntil = 0;  // millis() when post-nuke tree cooldown ends

// Debris fragments (shuttle destruction)
struct ShuttleDebris {
    float x, y, vx, vy;
    uint8_t size;        // 1 or 2 fat-pixel blocks
    uint8_t life;        // ticks remaining
    int16_t prevX, prevY;  // previous snap position for trail
    bool active;
};
static constexpr uint8_t MAX_DEBRIS = 5;

// Mushroom cloud (ground impact)
struct MushroomCloud {
    float x;
    float yOffset;       // sinking offset during fade (phase 3)
    uint8_t phase;       // 0=fireball, 1=stem, 2=cap, 3=flash fade, 4=linger dissipate
    uint8_t tick;        // ticks in current phase
    uint8_t stemH;       // stem height in PX units
    uint8_t capW;        // cap half-width in PX units
    uint8_t fireR;       // fireball radius in PX units
    int8_t lean;         // wind-shear lean (-2..2 blocks)
    uint8_t profileSeed; // shape jitter seed
    uint16_t lingerTick;     // long-form dissipation progress
    uint16_t lingerDuration; // how long the residue persists
    bool domeTriggered;      // ensure shockwave starts once
    bool active;
};

// ==[ SHOCKWAVE ]== expanding hype-filled circle from shuttle destruction
enum ShockwavePhase : uint8_t {
    SW_INACTIVE,
    SW_WAITING_MUSHROOM, // debris falling + mushroom growing before dome
    SW_EXPANDING,    // dome growing from ground impact point
    SW_FILLED,       // full screen covered, shake + hype transition
    SW_SAND_DISSIPATE, // top-down fat-pixel sand reveal
    SW_DEBRIS_RAIN   // post-shockwave debris rain from sky
};

struct Shockwave {
    float cx, cy;            // dome center (screen coords)
    float radius;            // current radius in pixels (0 → DOME_MAX_RADIUS)
    ShockwavePhase phase;
    uint32_t phaseStart;     // millis() when current phase began
    bool pigHypeDelayed;     // pig stays non-hype during expansion
    float mushroomX;         // X of ground mushroom for dome origin
    uint8_t ditherSeed;      // interior pulse seed
};

// ==[ DEBRIS RAIN ]== heavy fallout after nuclear event
// 3x3 fat-pixel bitmasks for debris shapes (MSB = left col)
// Each uint8_t = one row, 3 bits used (bits 2,1,0)
static const uint8_t DEBRIS_SHAPES[][3] = {
    { 0b110, 0b011, 0b000 },  // S-piece fragment
    { 0b010, 0b111, 0b000 },  // T-piece fragment
    { 0b110, 0b110, 0b000 },  // square chunk
    { 0b100, 0b110, 0b010 },  // Z-piece fragment
    { 0b111, 0b001, 0b000 },  // L-piece fragment
    { 0b010, 0b010, 0b010 },  // bar fragment
};
static const uint8_t DEBRIS_SHAPE_COUNT = 6;

struct DebrisRainPiece {
    float x, y, vx, vy;
    uint8_t shapeIdx;        // index into DEBRIS_SHAPES[]
    uint8_t rotation;        // 0-3 (quarter turns)
    uint8_t rotSpeed;        // ticks per rotation step
    uint8_t tick;            // frame counter for rotation
    int16_t prevX, prevY;    // previous snap position (trail)
    bool active;
};
static constexpr uint8_t MAX_RAIN_DEBRIS = 12;
static uint32_t debrisRainStart = 0;
static uint32_t debrisRainDuration = 25000;
static uint32_t lastDebrisRainSpawn = 0;
static uint32_t debrisSpawnInterval = 500;
static uint32_t lastShockwaveUpdate = 0;

// ==[ SAND DISSIPATION ]== top-down pixel dissolve revealing pig
static constexpr uint8_t SAND_ROWS = (SCREEN_HEIGHT + BIRD_PX - 1) / BIRD_PX;  // 60 rows (240/4)
static constexpr uint8_t SAND_COLS = (SCREEN_WIDTH + BIRD_PX - 1) / BIRD_PX;  // 80 cols (320/4)
// ==[ PSRAM WEATHER POOL ]== single alloc, ~2.6KB, avoids DRAM static pressure
struct WeatherPool {
    CloudShape     clouds[8];          // MAX_CLOUDS
    RainDrop       rainDrops[25];      // RAIN_DROP_COUNT
    WindParticle   windParticles[6];   // WIND_COUNT
    SkyBird        birds[2];           // BIRD_COUNT
    BirdSpark      sparks[6];          // SPARK_COUNT
    BirdExplosion  explosions[2];      // EXPLOSION_COUNT
    ImpactSplash   impactSplashes[6];  // SPLASH_COUNT
    Shuttle        shuttle;
    ShuttleDebris  shuttleDebris[5];   // MAX_DEBRIS
    MushroomCloud  mushroom;
    Shockwave      shockwave;
    DebrisRainPiece debrisRain[12];    // MAX_RAIN_DEBRIS
    uint8_t        sandErodedCount[60];// SAND_ROWS (240/4=60)
    uint8_t        sunMask[968];       // sun silhouette bitmask
};
static WeatherPool* wpool = nullptr;

// convenience aliases — keep all existing code readable
#define clouds          (wpool->clouds)
#define rainDrops       (wpool->rainDrops)
#define windParticles   (wpool->windParticles)
#define birds           (wpool->birds)
#define sparks          (wpool->sparks)
#define explosions      (wpool->explosions)
#define impactSplashes  (wpool->impactSplashes)
#define shuttle         (wpool->shuttle)
#define shuttleDebris   (wpool->shuttleDebris)
#define mushroom        (wpool->mushroom)
#define shockwave       (wpool->shockwave)
#define debrisRain      (wpool->debrisRain)
#define sandErodedCount (wpool->sandErodedCount)
#define sunMask         (wpool->sunMask)

static uint32_t sandPhaseStart = 0;
static constexpr uint16_t SAND_DURATION_MS = 4000;
static float sandFrontY = 0.0f;            // erosion front row (float)

// mushroom runs at full 50ms cadence (no decimator — smoother animation)

// ==[ SUN ]== pixelated disc, parabolic arc synced to time-of-day
static int16_t sunX = SCREEN_WIDTH / 2, sunY = SKY_TOP_Y + 6;
static constexpr int16_t SUN_RADIUS = 20;
static bool sunActive = false;
static uint32_t lastSunUpdate = 0;

// Sun edge noise: simple sin-hash noise for radial contour
static float sunEdgeNoise(float angle, float t) {
    float n1 = sinf(angle * 3.7f + t * 1.1f) * 0.5f + 0.5f;
    float n2 = sinf(angle * 7.3f - t * 0.8f + 2.1f) * 0.3f + 0.5f;
    return n1 * 0.7f + n2 * 0.3f;
}

// ==[ SOLAR PROMINENCES ]== parametric arcs from sun limb
struct Prominence {
    float angle;        // anchor position on sun limb (radians)
    float height;       // max extension (4-8 fat pixels beyond radius)
    float width;        // angular width (0.3-0.6 rad)
    float phase;        // lifecycle: 0=growing, 0.5=peak, 1.0=fading
    float speed;        // phase increment per tick
    bool active;
};
static constexpr uint8_t MAX_PROMINENCES = 3;
static Prominence prominences[MAX_PROMINENCES] = {{0}};
static uint32_t lastProminenceUpdate = 0;

static void updateSunPosition() {
    uint32_t now = millis();
    if (now - lastSunUpdate < 60000 && lastSunUpdate != 0) return;
    lastSunUpdate = now;

    int hour = -1, minute = 0;

    // system time (NTP-synced)
    time_t sysTime = time(nullptr);
    if (sysTime > 1704067200) {
        struct tm ti;
        localtime_r(&sysTime, &ti);
        hour = ti.tm_hour;
        minute = ti.tm_min;
    }

    // RTC fallback
    if (hour < 0 && M5.Rtc.isEnabled()) {
        auto dt = M5.Rtc.getDateTime();
        if (dt.date.year >= 2024 && dt.time.hours >= 0) {
            hour = dt.time.hours;
            minute = dt.time.minutes;
        }
    }

    // no time source → noon position
    if (hour < 0) {
        sunActive = true;
        sunX = birdSnap(SCREEN_WIDTH / 2);
        sunY = birdSnap(SKY_TOP_Y + 8);
        return;
    }

    // night (20:00-06:00)
    if (hour >= 20 || hour < 6) {
        sunActive = false;
        return;
    }

    sunActive = true;
    // mood gate: the sun doesn't come out for a sad pig. During SAD/DEPRESSED/
    // NEGLECTED the sky is overcast; the rain system already fires at those
    // tiers, but without suppressing the sun the scene read "rainy + sunny"
    // which broke the mood language.
    MoodTier mt = Mood::getMoodTier();
    if (mt == MoodTier::SAD || mt == MoodTier::DEPRESSED || mt == MoodTier::NEGLECTED) {
        sunActive = false;
        return;
    }
    // minutes since 06:00
    int totalMin = (hour - 6) * 60 + minute;
    float t = (float)totalMin / 840.0f;  // 0.0 at 06:00, 1.0 at 20:00
    sunX = birdSnap((int16_t)(20.0f + t * 280.0f));
    float arc = 1.0f - (2.0f * t - 1.0f) * (2.0f * t - 1.0f);
    sunY = birdSnap((int16_t)(113.0f - 87.0f * arc));
}

// prominence update — spawn/decay lifecycle
static void updateProminences(uint32_t now) {
    if (!sunActive) return;
    if (now - lastProminenceUpdate < 50) return;
    lastProminenceUpdate = now;

    for (uint8_t i = 0; i < MAX_PROMINENCES; i++) {
        Prominence& p = prominences[i];
        if (!p.active) {
            // ~1% spawn chance per tick, max concurrent controlled by loop
            if (random(0, 100) == 0) {
                p.angle = (float)random(0, 628) / 100.0f;  // 0-2PI
                p.height = (float)random(4, 9);
                p.width = 0.075f + (float)random(0, 8) / 100.0f;  // 0.075-0.15 (4x slimmer)
                p.phase = 0.0f;
                // speed: 0.75-2s lifecycle (4x shorter) → ~0.025-0.067 per 50ms tick
                p.speed = 1.0f / (float)random(15, 41);
                p.active = true;
            }
            continue;
        }
        p.phase += p.speed;
        if (p.phase >= 1.0f) p.active = false;
    }
}

// Sun mask: static DRAM at 4px grid
static constexpr uint8_t SUN_GRID_W = SCREEN_WIDTH / BIRD_PX;                // 80
static constexpr uint8_t SUN_GRID_H = (SKY_BOTTOM_Y + BIRD_PX) / BIRD_PX;   // 56
// sunMask in WeatherPool (~560 bytes, PSRAM)
static constexpr uint16_t SUN_MASK_BYTES = (SUN_GRID_W * SUN_GRID_H + 7) / 8;
static bool sunMaskPrimed = false;

static inline void sunMaskSet(uint8_t gx, uint8_t gy) {
    uint16_t idx = (uint16_t)gy * SUN_GRID_W + gx;
    sunMask[idx >> 3] |= (1u << (idx & 7));
}
static inline bool sunMaskGet(uint8_t gx, uint8_t gy) {
    uint16_t idx = (uint16_t)gy * SUN_GRID_W + gx;
    return sunMask[idx >> 3] & (1u << (idx & 7));
}

// Pass 1: compute sun mask BEFORE other sky objects (no drawing)
static void drawSunDiscPass1(M5Canvas& /*canvas*/, uint16_t /*unused*/) {
    sunMaskPrimed = false;
    if (!sunActive) return;
    float t = (float)millis() / 200.0f;
    int16_t margin = SUN_RADIUS + 12;
    memset(sunMask, 0, SUN_MASK_BYTES);
    sunMaskPrimed = true;

    for (int16_t y = sunY - margin; y <= sunY + margin; y += BIRD_PX) {
        for (int16_t x = sunX - margin; x <= sunX + margin; x += BIRD_PX) {
            int16_t px = birdSnap(x);
            int16_t py = birdSnap(y);
            if (px < 0 || px >= SCREEN_WIDTH || py < TOP_BAR_H || py >= SKY_BOTTOM_Y) continue;

            float dx = (float)(px + BIRD_PX / 2 - sunX);
            float dy = (float)(py + BIRD_PX / 2 - sunY);
            float dist = sqrtf(dx * dx + dy * dy);
            float angle = atan2f(dy, dx);

            float noiseR = (float)SUN_RADIUS + 3.0f * (sunEdgeNoise(angle * 2.0f, t) - 0.5f);
            bool inDisc = (dist <= noiseR);

            if (!inDisc) {
                for (uint8_t i = 0; i < MAX_PROMINENCES; i++) {
                    const Prominence& p = prominences[i];
                    if (!p.active) continue;
                    float adiff = angle - p.angle;
                    while (adiff > 3.14159f) adiff -= 6.28318f;
                    while (adiff < -3.14159f) adiff += 6.28318f;
                    if (adiff < 0) adiff = -adiff;
                    if (adiff > p.width) continue;
                    float localT = adiff / p.width;
                    float arch = sinf(3.14159f * (1.0f - localT));
                    float envelope = sinf(3.14159f * p.phase);
                    float prominenceR = noiseR + p.height * (float)BIRD_PX * arch * envelope;
                    if (dist <= prominenceR) { inDisc = true; break; }
                }
            }

            if (!inDisc) continue;

            uint8_t gx = (uint8_t)(px / BIRD_PX);
            uint8_t gy = (uint8_t)(py / BIRD_PX);
            sunMaskSet(gx, gy);
            // DON'T draw here — pass2 handles all sun rendering after objects
        }
    }
}

// Pass 2: strict per-pixel compositing inside sun mask (call AFTER all objects drawn).
// For every masked pixel:
//   - current == BG  -> draw FG (sun fill)
//   - current != BG  -> draw BG (object silhouette / inversion)
// This keeps non-intersecting object pixels unchanged and only inverts true intersections.
static void drawSunDiscPass2(M5Canvas& canvas, uint16_t drawFG, uint16_t drawBG) {
    if (!sunActive || !sunMaskPrimed) return;
    sunMaskPrimed = false;  // consume mask so non-mood paths cannot reuse stale sun data
    int16_t margin = SUN_RADIUS + 12;

    int16_t yMin = birdSnap(sunY - margin);
    int16_t yMax = birdSnap(sunY + margin);
    int16_t xMin = birdSnap(sunX - margin);
    int16_t xMax = birdSnap(sunX + margin);
    if (yMin < TOP_BAR_H) yMin = birdSnap(TOP_BAR_H + BIRD_PX - 1);
    if (xMin < 0) xMin = 0;
    if (yMax >= SKY_BOTTOM_Y) yMax = SKY_BOTTOM_Y - BIRD_PX + 2;
    if (xMax >= SCREEN_WIDTH) xMax = SCREEN_WIDTH - BIRD_PX + 1;

    for (int16_t py = yMin; py <= yMax; py += BIRD_PX) {
        uint8_t gy = (uint8_t)(py / BIRD_PX);
        for (int16_t px = xMin; px <= xMax; px += BIRD_PX) {
            uint8_t gx = (uint8_t)(px / BIRD_PX);
            if (!sunMaskGet(gx, gy)) continue;

            // Per-pixel compositing within each grid cell
            for (int16_t sy = 0; sy < BIRD_PX; sy++) {
                for (int16_t sx = 0; sx < BIRD_PX; sx++) {
                    uint16_t current = canvas.readPixel(px + sx, py + sy);
                    uint16_t out = (current != drawBG) ? drawBG : drawFG;
                    canvas.drawPixel(px + sx, py + sy, out);
                }
            }
        }
    }
}

static void spawnBird() {
    int slot = -1;
    for (int i = 0; i < BIRD_COUNT; i++) {
        if (!birds[i].active) { slot = i; break; }
    }
    if (slot < 0) return;

    SkyBird& b = birds[slot];
    b.y = (int8_t)random(SKY_TOP_Y + BIRD_SPAWN_Y_MIN_OFFSET, SKY_TOP_Y + BIRD_SPAWN_Y_MAX_OFFSET);
    b.sinePhase = 0;
    b.active = true;
    b.falling = false;
    b.lastWaveCheck = 0;

    bool goRight = random(0, 2) == 0;
    b.vx = goRight ? (int8_t)random(BIRD_VX_RIGHT_MIN, BIRD_VX_RIGHT_MAX) : (int8_t)random(BIRD_VX_LEFT_MIN, BIRD_VX_LEFT_MAX);
    if (b.vx == 0) b.vx = 1;
    b.x = goRight ? -30.0f : (float)(SCREEN_WIDTH + 30);
}

static void updateBirds(uint32_t now) {
    if (rainActive) {
        for (int i = 0; i < BIRD_COUNT; i++) {
            if (!birds[i].falling) birds[i].active = false;
        }
        for (int i = 0; i < SPARK_COUNT; i++) sparks[i].life = 0;
        for (int i = 0; i < EXPLOSION_COUNT; i++) explosions[i].active = false;
        for (int i = 0; i < SPLASH_COUNT; i++) impactSplashes[i].active = false;
        whistlingBird = -1;
        nextBirdSpawn = now + random(BIRD_SPAWN_MIN_MS, BIRD_SPAWN_MAX_MS);
        return;
    }

    if (now - lastBirdUpdate < 50) return;
    lastBirdUpdate = now;

    if (TimeMath::reachedOrUnset(now, nextBirdSpawn)) {
        spawnBird();
        nextBirdSpawn = now + random(BIRD_SPAWN_MIN_MS, BIRD_SPAWN_MAX_MS);
    }

    // Hunt mode policy: Hamlet does not attack birds.
    const bool birdWaveDamageEnabled = (Hamlet::getMode() != HamletMode::HUNT);

    for (int i = 0; i < BIRD_COUNT; i++) {
        if (!birds[i].active) continue;
        SkyBird& b = birds[i];

        if (!b.falling) {
            b.x += (float)b.vx;
            b.sinePhase++;

            if (b.x < -35.0f || b.x > (float)(SCREEN_WIDTH + 35)) {
                b.active = false;
                continue;
            }

            int16_t drawY = b.y + ((b.sinePhase & 0x08) ? 1 : 0);
            if (birdWaveDamageEnabled && Avatar::checkBirdWaveCollision((int16_t)b.x, drawY)) {
                // debounce: only roll once per 500ms per bird
                if (now - b.lastWaveCheck >= 500) {
                    b.lastWaveCheck = now;
                    if (random(0, 4) == 0) {
                        // 25% chance: hit
                        b.falling = true;
                        b.fallVy = BIRD_FALL_INITIAL_VY;
                        b.fallX = b.x;
                        b.fallY = (float)drawY;
                        b.fallStartY = (float)drawY;

                        SFX::play(SFX::BIRD_HIT);

                        if (whistlingBird < 0) whistlingBird = (int8_t)i;

                        int spawned = 0;
                        for (int s = 0; s < SPARK_COUNT && spawned < 3; s++) {
                            if (sparks[s].life == 0) {
                                sparks[s].x = b.fallX;
                                sparks[s].y = b.fallY;
                                sparks[s].vx = (float)random(-20, 21) / 10.0f;
                                sparks[s].vy = -1.0f - (float)random(0, 15) / 10.0f;
                                sparks[s].life = random(10, 18);
                                spawned++;
                            }
                        }

                        // no XP system in hamlet - mood reward only
                        Mood::onBirdKill();
                    }
                    // 50% chance: miss (wave passes through)
                }
            }
        } else {
            b.fallVy += BIRD_FALL_GRAVITY;
            b.fallY += b.fallVy;
            b.fallX += (float)b.vx * 0.5f;

            if (whistlingBird == i && b.fallY < (float)GROUND_Y) {
                float range = (float)GROUND_Y - b.fallStartY;
                float progress = (range > 0.0f) ? (b.fallY - b.fallStartY) / range : 1.0f;
                if (progress < 0.0f) progress = 0.0f;
                if (progress > 1.0f) progress = 1.0f;
                uint16_t freq = (uint16_t)(1200.0f - progress * 1000.0f);
                SFX::tone(freq, 60);
            }

            if (b.fallY > (float)GROUND_Y) {
                SFX::play(SFX::BIRD_IMPACT);

                if (whistlingBird == i) whistlingBird = -1;

                for (int e = 0; e < EXPLOSION_COUNT; e++) {
                    if (!explosions[e].active) {
                        explosions[e].x = b.fallX;
                        explosions[e].y = (float)GROUND_Y;
                        explosions[e].radius = 0;
                        explosions[e].maxRadius = (uint8_t)random(9, 13);
                        explosions[e].life = 12;
                        explosions[e].active = true;
                        break;
                    }
                }

                int splashed = 0;
                for (int s = 0; s < SPLASH_COUNT && splashed < 4; s++) {
                    if (!impactSplashes[s].active) {
                        impactSplashes[s].x = b.fallX + (float)random(-6, 7);
                        impactSplashes[s].y = (float)GROUND_Y;
                        impactSplashes[s].vx = (float)random(-30, 31) / 10.0f;
                        impactSplashes[s].vy = -1.0f - (float)random(0, 16) / 10.0f;
                        impactSplashes[s].life = (uint8_t)random(12, 19);
                        impactSplashes[s].active = true;
                        splashed++;
                    }
                }

                b.active = false;
            }
        }
    }

    for (int s = 0; s < SPARK_COUNT; s++) {
        if (sparks[s].life == 0) continue;
        sparks[s].x += sparks[s].vx;
        sparks[s].y += sparks[s].vy;
        sparks[s].vy += 0.25f;
        sparks[s].life--;
    }

    for (int e = 0; e < EXPLOSION_COUNT; e++) {
        if (!explosions[e].active) continue;
        if (explosions[e].radius < explosions[e].maxRadius) {
            explosions[e].radius++;
        } else {
            explosions[e].life--;
            if (explosions[e].life == 0) explosions[e].active = false;
        }
    }

    for (int s = 0; s < SPLASH_COUNT; s++) {
        if (!impactSplashes[s].active) continue;
        impactSplashes[s].x += impactSplashes[s].vx;
        impactSplashes[s].y += impactSplashes[s].vy;
        impactSplashes[s].vy += 0.3f;
        impactSplashes[s].life--;
        if (impactSplashes[s].life == 0) impactSplashes[s].active = false;
    }
}

// --- helper: spawn shuttle with given params ---
static void spawnShuttle(float startX, float startY, int8_t vx, bool /*fromMothership*/) {
    if (shuttle.active) return;  // no dual ships
    shuttle.x = startX;
    shuttle.yFloat = startY;
    shuttle.baseY = startY;
    shuttle.vx = vx;
    shuttle.yVel = 0.0f;
    shuttle.active = true;
    shuttle.fromNostromo = true;  // all spawns are cinematic now
    shuttle.enginePhase = 0;
    shuttle.lastWaveCheck = 0;
    shuttle.trailIdx = 0;
    shuttle.driftPhase = 0;
    shuttle.ascending = false;
    shuttle.maneuverTimer = millis() + random(3000, 8001);
    shuttle.maneuverPending = false;
    shuttle.bankPhase = 0;
    for (uint8_t t = 0; t < TRAIL_LEN; t++) {
        shuttle.trail[t].x = 0;
        shuttle.trail[t].y = 0;
        shuttle.trail[t].age = 255;
    }
    shuttle.postEntryTick = 0;

    // ==[ ATMOSPHERIC REENTRY SETUP ]== 100% cinematic, 5-minute sequence
    shuttle.matPhase = SPHASE_FIRST_LIGHT;
    shuttle.matTick = 0;
    memset(shuttle.revealMask, 0, sizeof(shuttle.revealMask));
    shuttle.revealedCount = 0;
    shuttle.reentryVy = 0.0f;
    shuttle.thrusterBlink = false;
    shuttle.phaseStartY = startY;
    shuttle.cinematicTargetY = (float)random(39, 70);
    shuttle.phraseSent = false;

    // init wake trail
    memset(shuttle.wake, 0, sizeof(shuttle.wake));
    for (uint8_t w = 0; w < WAKE_LEN; w++) shuttle.wake[w].age = 255;
    shuttle.wakeIdx = 0;

    // init bow shock
    shuttle.bowShock = {0, 0, 0};

    // init thrusters
    memset(shuttle.thrusters, 0, sizeof(shuttle.thrusters));
    shuttle.thrustTilt = 0;

    // init size (starts as PIG_PX dot)
    shuttle.pixelGrid = REENTRY_PX;
    shuttle.drawW = 1;
    shuttle.drawH = 1;
    shuttle.lastAblationTick = 0;

    // ==[ COMPOUND BEZIER TRAJECTORY ]== 2 segments for the long arc
    bool goRight = (vx > 0);
    // Segment A: upper sky entry → mid-screen handoff
    shuttle.bezA_P0x = startX;
    shuttle.bezA_P0y = startY;
    float handoffX = goRight ? (float)random(133, 200) : (float)random(120, 187);
    float handoffY = (float)random(59, 97);
    shuttle.bezA_P1x = (startX + handoffX) * 0.5f;
    shuttle.bezA_P1y = startY + (handoffY - startY) * 0.3f;  // gentle early sag
    shuttle.bezA_P2x = handoffX;
    shuttle.bezA_P2y = handoffY;
    // Segment B: mid-screen → cruise altitude endpoint
    shuttle.bezB_P1x = (handoffX + (goRight ? 267.0f : 40.0f)) * 0.5f;
    shuttle.bezB_P1y = handoffY + (float)random(16, 36);  // gravity sag
    shuttle.bezB_P2x = goRight ? 267.0f : 40.0f;
    shuttle.bezB_P2y = shuttle.cinematicTargetY;
    shuttle.bezT = 0.0f;

    // pick post-materialization flight behavior
    int roll = random(0, 100);
    if (roll < 60)      shuttle.behavior = SH_CRUISE;
    else if (roll < 85)  shuttle.behavior = SH_DRIFT;
    else                  shuttle.behavior = SH_LANDING;
}

// --- spawn debris from shuttle position ---
static void spawnDebris(float cx, float cy, int8_t shipVx) {
    int spawned = 0;
    float outward = (cx < (float)(SCREEN_WIDTH / 2)) ? 1.0f : -1.0f;
    for (uint8_t i = 0; i < MAX_DEBRIS && spawned < 4; i++) {
        if (shuttleDebris[i].active) continue;
        ShuttleDebris& d = shuttleDebris[i];
        d.x = cx + (float)random(-9, 10);
        d.y = cy + (float)random(-3, 4);
        float arcDir = (spawned & 1) ? outward : -outward * 0.4f;
        d.vx = (float)shipVx * 0.25f + arcDir * (0.7f + (float)random(0, 12) / 10.0f)
             + (float)random(-8, 9) / 10.0f;
        d.vy = -1.0f - (float)random(0, 15) / 10.0f;
        d.size = (uint8_t)random(1, 3);
        d.life = (uint8_t)random(30, 50);
        d.prevX = birdSnap((int16_t)d.x);
        d.prevY = birdSnap((int16_t)d.y);
        d.active = true;
        spawned++;
    }
}

// --- trigger mushroom cloud at ground level ---
static void triggerMushroom(float x) {
    // Only one mushroom per explosion event — once active, block all subsequent debris
    if (mushroom.active) return;
    // raw impact X — no lane snapping, mushroom+dome grow where ship actually fell
    float composedX = Gfx::clampf((float)birdSnap((int16_t)x), (float)FOV_SAFE_LEFT, (float)FOV_SAFE_RIGHT);
    mushroom.x = composedX;
    mushroom.yOffset = 0.0f;
    mushroom.phase = 0;
    mushroom.tick = 0;
    mushroom.stemH = 0;
    mushroom.capW = 0;
    mushroom.fireR = 1;
    mushroom.lean = (int8_t)random(-2, 3);
    mushroom.profileSeed = (uint8_t)random(0, 255);
    mushroom.lingerTick = 0;
    mushroom.lingerDuration = (uint16_t)random(220, 361);  // 11-18s at 50ms cadence
    mushroom.domeTriggered = false;
    mushroom.active = true;

    // capture ground X for dome origin
    if (shockwave.phase == SW_WAITING_MUSHROOM) {
        shockwave.mushroomX = composedX;
        shockwave.ditherSeed = (uint8_t)random(0, 255);
    }

    Avatar::triggerSparkles(8);
    Avatar::triggerTailWiggle();
    Avatar::cuteJump();
}

static void armShockwaveFromImpact(float impactX, uint32_t now) {
    const float rootX = Gfx::clampf((float)birdSnap((int16_t)impactX), (float)FOV_SAFE_LEFT, (float)FOV_SAFE_RIGHT);
    shockwave.cx = rootX;
    shockwave.cy = SHOCKWAVE_ORIGIN_Y;
    shockwave.radius = 0.0f;
    shockwave.phase = SW_WAITING_MUSHROOM;
    shockwave.phaseStart = now;
    shockwave.pigHypeDelayed = true;
    shockwave.mushroomX = rootX;
    shockwave.ditherSeed = (uint8_t)random(0, 255);
}

// --- pixel reveal helpers ---

// reveal N pixels with directional bias (nose first), keeps silhouette readable
static uint8_t revealRandomPixels8(const uint8_t* sprite, uint8_t* mask,
                                    uint8_t rows, uint8_t w, uint8_t count,
                                    bool goRight, uint8_t frontBiasPct) {
    uint8_t revealed = 0;
    for (uint8_t i = 0; i < count; i++) {
        bool found = false;
        for (uint8_t attempt = 0; attempt < 20; attempt++) {
            uint8_t row = (uint8_t)random(0, rows);
            uint8_t col = 0;
            if ((uint8_t)random(0, 100) < frontBiasPct) {
                uint8_t frontSpan = (w < 3) ? 1 : 3;
                if (goRight) {
                    col = (uint8_t)random(w - frontSpan, w);
                } else {
                    col = (uint8_t)random(0, frontSpan);
                }
            } else {
                col = (uint8_t)random(0, w);
            }
            uint8_t bit = 1u << (w - 1 - col);
            if ((sprite[row] & bit) && !(mask[row] & bit)) {
                mask[row] |= bit;
                revealed++;
                found = true;
                break;
            }
        }
        if (!found) {
            for (uint8_t r = 0; r < rows && !found; r++) {
                uint8_t unrevealed = sprite[r] & ~mask[r];
                if (!unrevealed) continue;
                for (uint8_t scan = 0; scan < w; scan++) {
                    uint8_t c = goRight ? (uint8_t)(w - 1 - scan) : scan;
                    uint8_t bit = 1u << (w - 1 - c);
                    if (unrevealed & bit) {
                        mask[r] |= bit;
                        revealed++;
                        found = true;
                        break;
                    }
                }
            }
        }
    }
    return revealed;
}

// --- Quadratic Bezier helper ---
static inline float bezier2(float p0, float p1, float p2, float t) {
    float u = 1.0f - t;
    return u * u * p0 + 2.0f * u * t * p1 + t * t * p2;
}

// --- update Nostromo station ---
// --- update shuttle flight + collision ---
static void updateShuttle(uint32_t now) {
    if (!shuttleSpawnTimerSet) {
        nextShuttleSpawn = now + random(SHUTTLE_SPAWN_MIN_MS, SHUTTLE_SPAWN_MAX_MS);
        shuttleSpawnTimerSet = true;
    }

    if (now - lastShuttleUpdate < 50) return;
    lastShuttleUpdate = now;

    // shuttle spawn: 100% cinematic atmospheric re-entry
    if (!shuttle.active && TimeMath::reached(now, nextShuttleSpawn)) {
        bool birdFalling = false;
        for (int i = 0; i < BIRD_COUNT; i++) {
            if (birds[i].active && birds[i].falling) { birdFalling = true; break; }
        }
        if (!rainActive && !birdFalling) {
            bool goRight = random(0, 2) == 0;
            int8_t svx = goRight ? 1 : -1;
            // spawn in upper sky for atmospheric entry
            float sx = goRight ? (float)random(53, 107) : (float)random(213, 267);
            float sy = (float)random(SKY_TOP_Y - 2, SKY_TOP_Y + 4);
            spawnShuttle(sx, sy, svx, true);
        }
        nextShuttleSpawn = now + random(SHUTTLE_SPAWN_MIN_MS, SHUTTLE_SPAWN_MAX_MS);
    }

    if (!shuttle.active) return;

    // ==[ CINEMATIC ENTRY SEQUENCE ]==
    // ==[ ATMOSPHERIC REENTRY ]== 6-phase cinematic, ~5 minutes from first dot to materialization
    if (shuttle.fromNostromo && shuttle.matPhase != SPHASE_MATERIALIZED && shuttle.matPhase != SPHASE_FALLING) {
        shuttle.matTick++;

        // --- compound bezier position evaluation ---
        auto evalCompoundBez = [&]() {
            if (shuttle.bezT <= 0.52f) {
                float segT = shuttle.bezT / 0.52f;
                shuttle.x = bezier2(shuttle.bezA_P0x, shuttle.bezA_P1x, shuttle.bezA_P2x, segT);
                shuttle.yFloat = bezier2(shuttle.bezA_P0y, shuttle.bezA_P1y, shuttle.bezA_P2y, segT);
            } else {
                float segT = (shuttle.bezT - 0.52f) / 0.36f;
                if (segT > 1.0f) segT = 1.0f;
                shuttle.x = bezier2(shuttle.bezA_P2x, shuttle.bezB_P1x, shuttle.bezB_P2x, segT);
                shuttle.yFloat = bezier2(shuttle.bezA_P2y, shuttle.bezB_P1y, shuttle.bezB_P2y, segT);
            }
        };

        // --- wake deposit helper ---
        auto depositWake = [&](int8_t spread) {
            WakeParticle& wp = shuttle.wake[shuttle.wakeIdx];
            int16_t snapFn = (shuttle.pixelGrid == REENTRY_PX) ? REENTRY_PX : BIRD_PX;
            wp.x = (int16_t)((int16_t)shuttle.x / snapFn) * snapFn;
            wp.y = (int16_t)((int16_t)shuttle.yFloat / snapFn) * snapFn;
            wp.age = 0;
            wp.spread = spread;
            shuttle.wakeIdx = (shuttle.wakeIdx + 1) % WAKE_LEN;
        };

        switch (shuttle.matPhase) {

            // ==[ FIRST_LIGHT ]== 75s. Faint blinking PIG_PX dot. Barely moves.
            case SPHASE_FIRST_LIGHT: {
                float phaseT = (float)shuttle.matTick / (float)SH_FIRST_LIGHT_TICKS;
                float st = phaseT * phaseT * (3.0f - 2.0f * phaseT);
                shuttle.bezT = st * 0.04f;
                evalCompoundBez();
                // size: 1x1 PIG_PX
                shuttle.pixelGrid = REENTRY_PX;
                shuttle.drawW = 1; shuttle.drawH = 1;
                // SFX: faint whisper near end
                if (shuttle.matTick == 1300) SFX::play(SFX::REENTRY_WHISPER);
                // transition
                if (shuttle.matTick >= SH_FIRST_LIGHT_TICKS) {
                    shuttle.matPhase = SPHASE_WAKE_FORMING;
                    shuttle.matTick = 0;
                    shuttle.phraseSent = false;
                }
                break;
            }

            // ==[ WAKE_FORMING ]== 55s. Trail starts, dot solidifies.
            case SPHASE_WAKE_FORMING: {
                float phaseT = (float)shuttle.matTick / (float)SH_WAKE_TICKS;
                float st = phaseT * phaseT * (3.0f - 2.0f * phaseT);
                shuttle.bezT = 0.04f + st * 0.08f;
                evalCompoundBez();
                // size: grows to 2x1 at tick 600
                shuttle.pixelGrid = REENTRY_PX;
                shuttle.drawW = (shuttle.matTick < 600) ? 1 : 2;
                shuttle.drawH = 1;
                // wake: every 8 ticks initially, every 4 later
                uint16_t wakeRate = (shuttle.matTick < 550) ? 8 : 4;
                if ((shuttle.matTick % wakeRate) == 0) depositWake(0);
                // transition
                if (shuttle.matTick >= SH_WAKE_TICKS) {
                    shuttle.matPhase = SPHASE_HEATING;
                    shuttle.matTick = 0;
                    shuttle.phraseSent = false;
                }
                break;
            }

            // ==[ HEATING ]== 50s. Plasma grows, bow shock forms. PIG_PX → BIRD_PX transition.
            case SPHASE_HEATING: {
                float phaseT = (float)shuttle.matTick / (float)SH_HEATING_TICKS;
                float st = phaseT * phaseT * (3.0f - 2.0f * phaseT);
                shuttle.bezT = 0.12f + st * 0.18f;
                evalCompoundBez();
                // size: PIG_PX 2x2 → BIRD_PX 1x1 at tick 300 → BIRD_PX 2x1
                if (shuttle.matTick < 300) {
                    shuttle.pixelGrid = REENTRY_PX;
                    shuttle.drawW = 2; shuttle.drawH = 2;
                } else {
                    shuttle.pixelGrid = BIRD_PX;
                    shuttle.drawW = (shuttle.matTick < 700) ? 1 : 2;
                    shuttle.drawH = 1;
                }
                // bow shock: starts at tick 200
                if (shuttle.matTick >= 200) {
                    float shockT = (float)(shuttle.matTick - 200) / 800.0f;
                    if (shockT > 1.0f) shockT = 1.0f;
                    shuttle.bowShock.innerR = 1 + (uint8_t)(shockT * 1.0f);
                    shuttle.bowShock.outerR = 2 + (uint8_t)(shockT * 1.0f);
                    shuttle.bowShock.flicker = 40 - (uint8_t)(shockT * 15.0f);
                }
                // heat shimmer after tick 400
                if (shuttle.matTick > 400) {
                    shuttle.yFloat += ((float)(esp_random() % 6) - 3.0f) / 10.0f;
                }
                // wake: every 3 ticks
                if ((shuttle.matTick % 3) == 0) depositWake((int8_t)(shuttle.pixelGrid));
                // SFX + mood
                if (shuttle.matTick == 300) SFX::play(SFX::REENTRY_RUMBLE);
                if (shuttle.matTick == 500 && !shuttle.phraseSent) {
                    static const char* const HEAT_PHRASES[] = {
                        "SKY JUST HIT CRITICAL",
                        "THERMAL CONTACT. NO ALIBI",
                        "BRIGHT OBJECT. CASE OPEN"
                    };
                    Mood::setPhrase(HEAT_PHRASES[random(0, 3)], AvatarState::NEUTRAL);
                    shuttle.phraseSent = true;
                }
                if (shuttle.matTick == 800) Avatar::setAttackShake(true, false);
                // transition
                if (shuttle.matTick >= SH_HEATING_TICKS) {
                    shuttle.matPhase = SPHASE_FIREBALL;
                    shuttle.matTick = 0;
                    shuttle.phraseSent = false;
                }
                break;
            }

            // ==[ FIREBALL ]== 40s. Peak brightness. Max trail, sparks, screen shake.
            case SPHASE_FIREBALL: {
                float phaseT = (float)shuttle.matTick / (float)SH_FIREBALL_TICKS;
                float st = phaseT * phaseT * (3.0f - 2.0f * phaseT);
                shuttle.bezT = 0.30f + st * 0.22f;
                evalCompoundBez();
                // size: 2x2 BIRD_PX core
                shuttle.pixelGrid = BIRD_PX;
                shuttle.drawW = 2; shuttle.drawH = 2;
                // bow shock: max
                {
                    float shockFade = (phaseT < 0.7f) ? 1.0f : (1.0f - (phaseT - 0.7f) / 0.3f);
                    shuttle.bowShock.innerR = 2 + (uint8_t)(shockFade * 1.0f);
                    shuttle.bowShock.outerR = 4 + (uint8_t)(shockFade * 1.0f);
                    shuttle.bowShock.flicker = 10 + (uint8_t)((1.0f - shockFade) * 15.0f);
                }
                // heat shimmer
                shuttle.yFloat += ((float)(esp_random() % 10) - 5.0f) / 10.0f;
                // wake: every 2 ticks with spread
                if ((shuttle.matTick % 2) == 0) {
                    int8_t spread = (int8_t)(((esp_random() % 3) - 1) * BIRD_PX * 2);
                    depositWake(spread);
                }
                // ablation fragments (sparks breaking off)
                if (shuttle.matTick > 50 && shuttle.matTick - shuttle.lastAblationTick > (uint16_t)random(15, 26)) {
                    shuttle.lastAblationTick = shuttle.matTick;
                    // reuse debris system for small ablation spark
                    for (uint8_t i = 0; i < MAX_DEBRIS; i++) {
                        if (shuttleDebris[i].active) continue;
                        ShuttleDebris& d = shuttleDebris[i];
                        d.x = shuttle.x + (float)random(-3, 4);
                        d.y = shuttle.yFloat + (float)random(-3, 4);
                        d.vx = ((float)(esp_random() % 20) - 10.0f) / 10.0f;
                        d.vy = ((float)(esp_random() % 10)) / 10.0f;
                        d.size = 1;
                        d.life = random(8, 16);
                        d.prevX = birdSnap((int16_t)d.x);
                        d.prevY = birdSnap((int16_t)d.y);
                        d.active = true;
                        break;
                    }
                }
                // SFX + mood
                if (shuttle.matTick == 50) SFX::play(SFX::REENTRY_ROAR);
                if (shuttle.matTick == 100 && !shuttle.phraseSent) {
                    static const char* const FIRE_PHRASES[] = {
                        "ABLATIVE TILE TEST",
                        "HULL ON FIRE. NOTED",
                        "IMPACT FILE OPEN"
                    };
                    Mood::setPhrase(FIRE_PHRASES[random(0, 3)], AvatarState::EXCITED);
                    shuttle.phraseSent = true;
                }
                if (shuttle.matTick == 500) SFX::play(SFX::REENTRY_ROAR);
                // transition
                if (shuttle.matTick >= SH_FIREBALL_TICKS) {
                    Avatar::setAttackShake(false, false);
                    shuttle.matPhase = SPHASE_DECELERATION;
                    shuttle.matTick = 0;
                    shuttle.phraseSent = false;
                }
                break;
            }

            // ==[ DECELERATION ]== 45s. Bow shock fades, object grows, hull hints.
            case SPHASE_DECELERATION: {
                float phaseT = (float)shuttle.matTick / (float)SH_DECEL_TICKS;
                float st = phaseT * phaseT * (3.0f - 2.0f * phaseT);
                shuttle.bezT = 0.52f + st * 0.20f;
                evalCompoundBez();
                // size: grows 2x2 → 5x3 BIRD_PX
                shuttle.pixelGrid = BIRD_PX;
                shuttle.drawW = 2 + (uint8_t)(phaseT * 3.0f);  // 2→5
                if (shuttle.drawW > 5) shuttle.drawW = 5;
                shuttle.drawH = 2 + (uint8_t)(phaseT * 1.0f);  // 2→3
                if (shuttle.drawH > 3) shuttle.drawH = 3;
                // bow shock: shrinking
                {
                    float fade = 1.0f - phaseT;
                    shuttle.bowShock.innerR = (uint8_t)(fade * 2.0f);
                    shuttle.bowShock.outerR = (uint8_t)(fade * 3.0f);
                    shuttle.bowShock.flicker = 30 + (uint8_t)((1.0f - fade) * 20.0f);
                    if (phaseT > 0.8f) { shuttle.bowShock.innerR = 0; shuttle.bowShock.outerR = 0; }
                }
                // heat shimmer fading
                if (phaseT < 0.5f) {
                    shuttle.yFloat += ((float)(esp_random() % 4) - 2.0f) / 10.0f;
                }
                // wake: every 3 ticks, shrinking spread
                if ((shuttle.matTick % 3) == 0) {
                    int8_t spread = (phaseT < 0.5f) ? (int8_t)BIRD_PX : 0;
                    depositWake(spread);
                }
                // mood
                if (shuttle.matTick == 500 && !shuttle.phraseSent) {
                    static const char* const DECEL_PHRASES[] = {
                        "CONTACT HAS THRUST",
                        "METEOR FOUND BRAKES",
                        "SHIP. BAD FIRST ALIBI"
                    };
                    Mood::setPhrase(DECEL_PHRASES[random(0, 3)], AvatarState::HAPPY);
                    shuttle.phraseSent = true;
                }
                // transition
                if (shuttle.matTick >= SH_DECEL_TICKS) {
                    shuttle.matPhase = SPHASE_COOLING;
                    shuttle.matTick = 0;
                    shuttle.phraseSent = false;
                    shuttle.bowShock = {0, 0, 0};
                }
                break;
            }

            // ==[ COOLING ]== 35s. Shape resolves via pixel reveal, thrusters ignite.
            case SPHASE_COOLING: {
                float phaseT = (float)shuttle.matTick / (float)SH_COOLING_TICKS;
                float st = phaseT * phaseT * (3.0f - 2.0f * phaseT);
                shuttle.bezT = 0.72f + st * 0.16f;
                evalCompoundBez();
                // size: full sprite dimensions, using revealMask
                shuttle.pixelGrid = BIRD_PX;
                shuttle.drawW = NARCISSUS_W;
                shuttle.drawH = NARCISSUS_H;
                // pixel reveal: nose-first, every 3 ticks
                if (shuttle.revealedCount < 22 && (shuttle.matTick % 3) == 0) {
                    bool goR = (shuttle.vx > 0);
                    const uint8_t* sprite = goR ? NARCISSUS_R : NARCISSUS_L;
                    uint8_t frontBias = 70;
                    if (shuttle.matTick > (SH_COOLING_TICKS / 3)) frontBias = 52;
                    if (shuttle.matTick > (2 * SH_COOLING_TICKS / 3)) frontBias = 38;
                    uint8_t got = revealRandomPixels8(sprite, shuttle.revealMask,
                                                      NARCISSUS_H, NARCISSUS_W, 1,
                                                      (shuttle.vx > 0), frontBias);
                    shuttle.revealedCount += got;
                }
                // thruster blink
                shuttle.thrusterBlink = ((shuttle.matTick / 12) & 1) != 0;
                // ==[ THRUSTER SEQUENCE ]== PIG_PX-sized attitude control
                if (shuttle.matTick >= 200 && shuttle.matTick < 350) {
                    // braking bursts: rear thrusters alternate
                    bool burst = ((shuttle.matTick / 15) & 1) != 0;
                    shuttle.thrusters[0].on = burst ? 1 : 0;
                    shuttle.thrusters[1].on = burst ? 0 : 1;
                    shuttle.thrusters[2].on = 0;
                    shuttle.thrusters[3].on = 0;
                } else if (shuttle.matTick >= 350 && shuttle.matTick < 550) {
                    // stabilization: random pairs, 8-tick bursts
                    if ((shuttle.matTick % 20) == 0) {
                        uint8_t pick = (uint8_t)(esp_random() % 4);
                        for (uint8_t t = 0; t < THRUSTER_COUNT; t++) shuttle.thrusters[t].on = 0;
                        shuttle.thrusters[pick].on = 1;
                        shuttle.thrusters[(pick + 2) % THRUSTER_COUNT].on = 1;
                    }
                    if ((shuttle.matTick % 20) == 8) {
                        for (uint8_t t = 0; t < THRUSTER_COUNT; t++) shuttle.thrusters[t].on = 0;
                    }
                } else if (shuttle.matTick >= 550) {
                    // steady rear burn
                    shuttle.thrusters[0].on = 1;
                    shuttle.thrusters[1].on = 1;
                    shuttle.thrusters[2].on = 0;
                    shuttle.thrusters[3].on = 0;
                } else {
                    for (uint8_t t = 0; t < THRUSTER_COUNT; t++) shuttle.thrusters[t].on = 0;
                }
                // tilt from thrusters
                {
                    int8_t targetTilt = 0;
                    if (shuttle.thrusters[0].on || shuttle.thrusters[3].on) targetTilt -= 1;
                    if (shuttle.thrusters[1].on || shuttle.thrusters[2].on) targetTilt += 1;
                    if (shuttle.thrustTilt < targetTilt) shuttle.thrustTilt++;
                    if (shuttle.thrustTilt > targetTilt) shuttle.thrustTilt--;
                }
                // wake: short exhaust trail
                if ((shuttle.matTick % 4) == 0) depositWake(0);
                // SFX
                if (shuttle.matTick == 250) SFX::play(SFX::THRUSTER_POP);
                if (shuttle.matTick == 350) SFX::play(SFX::THRUSTER_POP);
                if (shuttle.matTick == 450) SFX::play(SFX::THRUSTER_POP);
                // mood
                if (shuttle.matTick == 500 && !shuttle.phraseSent) {
                    static const char* const COOL_PHRASES[] = {
                        "FINAL VECTOR LOCKED",
                        "LANDING GEAR TESTIFIES",
                        "SHUTTLE ON THE RECORD"
                    };
                    Mood::setPhrase(COOL_PHRASES[random(0, 3)], AvatarState::HAPPY);
                    shuttle.phraseSent = true;
                }
                // transition: force-reveal + materialize
                if (shuttle.matTick >= SH_COOLING_TICKS) {
                    bool goR = (shuttle.vx > 0);
                    const uint8_t* sprite = goR ? NARCISSUS_R : NARCISSUS_L;
                    for (uint8_t r = 0; r < NARCISSUS_H; r++)
                        shuttle.revealMask[r] = sprite[r];
                    shuttle.revealedCount = 22;
                    shuttle.matPhase = SPHASE_MATERIALIZED;
                    shuttle.matTick = 0;
                    shuttle.baseY = shuttle.yFloat;
                    shuttle.reentryVy = 0.0f;
                    shuttle.thrusterBlink = false;  // engine pixel OFF first frame — avoids solid-block pop
                    shuttle.enginePhase = 3;        // skip initial nose spark too
                    shuttle.thrustTilt = 0;
                    for (uint8_t t = 0; t < THRUSTER_COUNT; t++) shuttle.thrusters[t].on = 0;
                }
                break;
            }

            default:
                break;
        }

        // age wake particles
        for (uint8_t w = 0; w < WAKE_LEN; w++) {
            if (shuttle.wake[w].age < 255) shuttle.wake[w].age++;
        }
        // age trail dots
        for (uint8_t t = 0; t < TRAIL_LEN; t++) {
            if (shuttle.trail[t].age < 255) shuttle.trail[t].age++;
        }

        // global cinematic safety clamp
        if (shuttle.x < 0.0f) shuttle.x = 0.0f;
        if (shuttle.x > (float)(SCREEN_WIDTH - NARCISSUS_W * BIRD_PX)) shuttle.x = (float)(SCREEN_WIDTH - NARCISSUS_W * BIRD_PX);
        if (shuttle.yFloat < (float)SKY_TOP_Y) shuttle.yFloat = (float)SKY_TOP_Y;
        if (shuttle.yFloat > 170.0f) shuttle.yFloat = 170.0f;
        return;  // skip normal flight until materialized
    }

    // ==[ NORMAL FLIGHT (independent spawns + post-materialization) ]==
    // FALLING has its own update below — skip normal flight/maneuvers/behaviors
    if (shuttle.matPhase != SPHASE_FALLING) {

    // post-entry slowdown for mothership-launched shuttles (160 ticks = 8s)
    float speedMul = 1.0f;
    if (shuttle.fromNostromo && shuttle.postEntryTick < 160) {
        shuttle.postEntryTick++;
        if (shuttle.postEntryTick <= 120) {
            speedMul = 0.25f;  // 4x slower for ~6s
        } else {
            // ramp 0.25→1.0 over ticks 120-160
            float rampT = (float)(shuttle.postEntryTick - 120) / 40.0f;
            speedMul = 0.25f + 0.75f * rampT;
        }
    }

    // flight movement
    shuttle.x += (float)shuttle.vx * speedMul;
    shuttle.enginePhase++;

    // thruster blink toggle (200ms period)
    shuttle.thrusterBlink = ((shuttle.enginePhase / 4) & 1) != 0;

    // ==[ MANEUVERING ]== random direction flips with banking animation
    if (TimeMath::reachedOrUnset(now, shuttle.maneuverTimer)) {
        shuttle.maneuverTimer = now + random(3000, 8001);
        if (random(0, 100) < 30) {  // 30% chance to flip
            shuttle.maneuverPending = true;
            shuttle.bankPhase = 1;
        }
    }

    // slow descent after materialization — ship sinks toward grass
    shuttle.yFloat += 0.12f;  // ~2.4 px/s descent

    // banking dip: 20-tick smooth turn with 3 intermediate sprites
    if (shuttle.bankPhase > 0) {
        shuttle.bankPhase++;
        // 20 ticks total: 10 dip down, 10 recover up (0.4px/tick)
        if (shuttle.bankPhase <= 11) {
            shuttle.yFloat += 0.4f;  // dip down (ticks 2-11)
        } else if (shuttle.bankPhase <= 21) {
            shuttle.yFloat -= 0.4f;  // recover (ticks 12-21)
        }
        // flip direction at midpoint (tick 10 = head-on view)
        if (shuttle.bankPhase == 10 && shuttle.maneuverPending) {
            shuttle.vx = -shuttle.vx;
            shuttle.maneuverPending = false;
        }
        // thruster-driven turn: fire opposite side for attitude correction
        if (shuttle.bankPhase >= 2 && shuttle.bankPhase <= 18) {
            // before midpoint: maneuverPending means we're turning to -vx
            // after midpoint: vx already flipped, thrusters push into new heading
            bool noseGoesRight = (shuttle.bankPhase < 10) ? (shuttle.vx < 0) : (shuttle.vx > 0);
            if (noseGoesRight) {
                shuttle.thrusters[1].on = 1;  // rear-bottom fires → nose tilts up/right
                shuttle.thrusters[0].on = 0;
            } else {
                shuttle.thrusters[0].on = 1;  // rear-top fires → nose tilts down/left
                shuttle.thrusters[1].on = 0;
            }
            shuttle.thrusters[2].on = 0;
            shuttle.thrusters[3].on = 0;
        } else if (shuttle.bankPhase >= 19) {
            for (uint8_t t = 0; t < THRUSTER_COUNT; t++) shuttle.thrusters[t].on = 0;
        }
        if (shuttle.bankPhase >= 21) {
            shuttle.bankPhase = 0;
            for (uint8_t t = 0; t < THRUSTER_COUNT; t++) shuttle.thrusters[t].on = 0;
        }
    }

    // tilt from thrusters (MATERIALIZED — same coupling as COOLING)
    {
        int8_t targetTilt = 0;
        if (shuttle.thrusters[0].on || shuttle.thrusters[3].on) targetTilt -= 1;
        if (shuttle.thrusters[1].on || shuttle.thrusters[2].on) targetTilt += 1;
        if (shuttle.thrustTilt < targetTilt) shuttle.thrustTilt++;
        if (shuttle.thrustTilt > targetTilt) shuttle.thrustTilt--;
    }

    // top safety clamp only — ship descends past screen bottom
    if (shuttle.yFloat < 16.0f) shuttle.yFloat = 16.0f;

    // exhaust trail at engine port (col 0 for right-facing, col W-1 for left-facing)
    bool goR = (shuttle.vx > 0);
    int16_t thrusterX = goR
        ? birdSnap((int16_t)shuttle.x)                              // engine at col 0 (left edge)
        : birdSnap((int16_t)shuttle.x + (NARCISSUS_W - 1) * BIRD_PX);  // engine at col W-1 (right edge)
    int16_t thrusterY = birdSnap((int16_t)shuttle.yFloat + BIRD_PX);
    shuttle.trail[shuttle.trailIdx].x = thrusterX;
    shuttle.trail[shuttle.trailIdx].y = thrusterY;
    shuttle.trail[shuttle.trailIdx].age = 0;
    shuttle.trailIdx = (shuttle.trailIdx + 1) % TRAIL_LEN;

    for (uint8_t t = 0; t < TRAIL_LEN; t++) {
        if (shuttle.trail[t].age < 255) shuttle.trail[t].age++;
    }

    // screen edge bounce (shuttle stays visible)
    if (shuttle.x < 0.0f) {
        shuttle.x = 0.0f;
        shuttle.vx = (int8_t)(-(int8_t)shuttle.vx);  // reverse direction
    }
    float maxX = (float)(SCREEN_WIDTH - NARCISSUS_W * BIRD_PX);
    if (shuttle.x > maxX) {
        shuttle.x = maxX;
        shuttle.vx = (int8_t)(-(int8_t)shuttle.vx);
    }

    } // end SPHASE_FALLING guard

    // wave collision — only when descending and above safe zone (30px above grass)
    static constexpr float SHUTTLE_SAFE_Y = (float)(GROUND_Y - 30);  // 162
    if (shuttle.matPhase == SPHASE_MATERIALIZED && shuttle.yFloat < SHUTTLE_SAFE_Y) {
        int16_t hitX = (int16_t)shuttle.x + (NARCISSUS_W * BIRD_PX) / 2;
        int16_t hitY = (int16_t)shuttle.yFloat + (NARCISSUS_H * BIRD_PX) / 2;
        if (Avatar::checkBirdWaveCollision(hitX, hitY)) {
            if (now - shuttle.lastWaveCheck >= 500) {
                shuttle.lastWaveCheck = now;
                if (random(0, 4) == 0) {
                    SFX::play(SFX::SHIP_EXPLODE);
                    Haptic::pulse();  // ship destruction impact
                    Mood::onShipKill();

                    // ==[ ENTER FALLING PHASE ]== shuttle breaks and falls to ground
                    shuttle.matPhase = SPHASE_FALLING;
                    shuttle.fallVy = 0.2f;   // initial slow downward drift
                    shuttle.fallTick = 0;
                    shuttle.fallDitherSeed = (uint8_t)esp_random();
                    shuttle.matTick = 0;
                    // slow horizontal drift (tumble)
                    shuttle.vx = (shuttle.vx > 0) ? 1 : -1;

                    // debris fragments arc outward from hull
                    spawnDebris(shuttle.x + (float)(NARCISSUS_W * BIRD_PX) / 2.0f,
                                shuttle.yFloat, shuttle.vx);

                    // ==[ PIG DODGE ]== cinematic owns pig during explosion sequence
                    {
                        int16_t pigX = Avatar::getCurrentX();
                        float impactX = shuttle.x + (float)(NARCISSUS_W * BIRD_PX) / 2.0f;
                        int16_t targetX = choosePigDodgeTarget(impactX, pigX);
                        CinematicPose pose;
                        pose.active = true;
                        pose.forceX = targetX;
                        pose.facingDir = (targetX > pigX) ? 1 : -1;
                        Avatar::setCinematicPose(pose);
                    }

                    // cleanup any in-progress explosion chain before restart
                    if (shockwave.phase != SW_INACTIVE) {
                        Avatar::setGrassYOffset(0);
                        Avatar::setAttackShake(false, false);
                    }
                    // kill previous mushroom so triggerMushroom() won't bounce
                    mushroom.active = false;

                    // Reset shockwave chain; it gets armed only on shuttle body ground impact.
                    shockwave.phase = SW_INACTIVE;
                    shockwave.pigHypeDelayed = false;
                    shockwave.radius = 0.0f;
                    shockwave.mushroomX = 0.0f;
                }
            }
        }
    }

    // peaceful exit: ship descended below visible area
    if (shuttle.matPhase == SPHASE_MATERIALIZED && shuttle.yFloat > (float)SKY_BOTTOM_Y) {
        shuttle.active = false;
        nextShuttleSpawn = millis() + (uint32_t)random(SHUTTLE_RESPAWN_MIN_MS, SHUTTLE_RESPAWN_MAX_MS);
        return;
    }

    // ==[ FALLING PHASE UPDATE ]== gravity + trail + ground impact
    if (shuttle.matPhase == SPHASE_FALLING) {
        shuttle.fallTick++;
        // skip physics on collision frame (normal flight already moved this tick)
        if (shuttle.fallTick <= 1) return;
        // gravity: accelerate downward
        shuttle.fallVy += 0.08f;
        shuttle.yFloat += shuttle.fallVy;
        // slow horizontal tumble
        shuttle.x += (float)shuttle.vx * 0.3f;

        // deposit hype-colored trail every 2 ticks
        if ((shuttle.fallTick % 2) == 0) {
            shuttle.trail[shuttle.trailIdx].x = birdSnap((int16_t)shuttle.x + random(-3, 4));
            shuttle.trail[shuttle.trailIdx].y = birdSnap((int16_t)shuttle.yFloat + random(-3, 4));
            shuttle.trail[shuttle.trailIdx].age = 0;
            shuttle.trailIdx = (shuttle.trailIdx + 1) % TRAIL_LEN;
        }

        // age trail dots
        for (uint8_t t = 0; t < TRAIL_LEN; t++) {
            if (shuttle.trail[t].age < 255) shuttle.trail[t].age++;
        }

        // ground impact (grass baseline ~GROUND_Y)
        if (shuttle.yFloat > (float)GROUND_Y) {
            float impactX = shuttle.x + (float)(NARCISSUS_W * BIRD_PX) / 2.0f;
            armShockwaveFromImpact(impactX, now);
            triggerMushroom(impactX);
            shuttle.active = false;
            // post-nuke cooldown: 60-300s before next shuttle
            nextShuttleSpawn = millis() + (uint32_t)random(SHUTTLE_RESPAWN_MIN_MS, SHUTTLE_RESPAWN_MAX_MS);
        } else if (shuttle.x < -30.0f || shuttle.x > (float)(SCREEN_WIDTH + 30)) {
            // safety: if it drifts off screen X, still trigger mushroom at last known position
            float impactX = Gfx::clampf(shuttle.x + (float)(NARCISSUS_W * BIRD_PX) / 2.0f, 40.0f, 280.0f);
            armShockwaveFromImpact(impactX, now);
            triggerMushroom(impactX);
            shuttle.active = false;
            nextShuttleSpawn = millis() + (uint32_t)random(SHUTTLE_RESPAWN_MIN_MS, SHUTTLE_RESPAWN_MAX_MS);
        }
        return;  // skip normal flight logic when falling
    }
}

// --- update debris fragments ---
static void updateDebris(uint32_t now) {
    if (now - lastDebrisUpdate < 50) return;
    lastDebrisUpdate = now;
    for (uint8_t i = 0; i < MAX_DEBRIS; i++) {
        if (!shuttleDebris[i].active) continue;
        ShuttleDebris& d = shuttleDebris[i];

        d.prevX = birdSnap((int16_t)d.x);
        d.prevY = birdSnap((int16_t)d.y);

        d.vy += 0.35f;  // gravity
        d.x += d.vx;
        d.y += d.vy;

        // ground impact (grass baseline ~SKY_BOTTOM_Y) — debris kicks up sparks, not mushroom
        if (d.y > (float)(SKY_BOTTOM_Y - 3)) {
            Avatar::triggerSparkles(3);
            d.active = false;
            continue;
        }

        if (d.life > 0) d.life--;
        if (d.life == 0) d.active = false;
    }
}

// --- update mushroom cloud state machine ---
// Full 50ms cadence (no decimator). All tick counts 3x original for same real-time.
static void updateMushroom(uint32_t now) {
    if (!mushroom.active) return;
    if (now - lastMushroomUpdate < 50) return;
    lastMushroomUpdate = now;
    mushroom.tick++;

    switch (mushroom.phase) {
        case 0:  // fireball: expand + pulse (36 ticks = 1.8s)
            if (mushroom.tick <= 36) {
                uint8_t pulse = ((mushroom.tick / 6) & 1u) ? 1u : 0u;
                mushroom.fireR = 2 + mushroom.tick / 4 + pulse;
                if (mushroom.fireR > 11) mushroom.fireR = 11;
            } else {
                mushroom.phase = 1;
                mushroom.tick = 0;
            }
            break;
        case 1:  // stem rises (48 ticks = 2.4s)
            if (mushroom.tick <= 48) {
                mushroom.stemH = mushroom.tick * 22 / 48;
                if (mushroom.stemH > 22) mushroom.stemH = 22;
                if (mushroom.tick % 3 == 0 && mushroom.fireR > 4) mushroom.fireR--;
            } else {
                mushroom.phase = 2;
                mushroom.tick = 0;
            }
            break;
        case 2:  // cap forms (42t) + hold (43-60) + sparks before fade
            if (mushroom.tick <= 42) {
                mushroom.capW = 4 + mushroom.tick / 4 + (mushroom.profileSeed & 1u);
                if (mushroom.capW > 13) mushroom.capW = 13;
            } else if (mushroom.tick <= 68) {
                if (mushroom.tick == 50 || mushroom.tick == 60) {
                    int16_t capTopY = GROUND_Y - (int16_t)mushroom.stemH * BIRD_PX - BIRD_PX;
                    int spawned = 0;
                    for (int s = 0; s < SPARK_COUNT && spawned < 4; s++) {
                        if (sparks[s].life == 0) {
                            sparks[s].x = mushroom.x;
                            sparks[s].y = (float)capTopY;
                            sparks[s].vx = (float)random(-25, 26) / 10.0f;
                            sparks[s].vy = -2.0f - (float)random(0, 20) / 10.0f;
                            sparks[s].life = random(14, 22);
                            spawned++;
                        }
                    }
                }
            } else {
                // final burst then transition to fade
                {
                    int16_t capTopY = GROUND_Y - (int16_t)mushroom.stemH * BIRD_PX - BIRD_PX;
                    int spawned = 0;
                    for (int s = 0; s < SPARK_COUNT && spawned < 4; s++) {
                        if (sparks[s].life == 0) {
                            sparks[s].x = mushroom.x;
                            sparks[s].y = (float)capTopY;
                            sparks[s].vx = (float)random(-30, 31) / 10.0f;
                            sparks[s].vy = -2.5f - (float)random(0, 25) / 10.0f;
                            sparks[s].life = random(16, 24);
                            spawned++;
                        }
                    }
                }
                mushroom.phase = 3;
                mushroom.tick = 0;
            }
            break;
        case 3:  // flash fade (80 ticks = 4s), then keep residue on-screen
            mushroom.yOffset += 0.14f;  // faster drift to match shorter phase
            if (mushroom.tick > 80) {
                mushroom.phase = 4;
                mushroom.tick = 0;
                if (mushroom.fireR > 5) mushroom.fireR = 5;
            }
            break;
        case 4:  // lingering column + cap slowly dissipates (tree-like collapse feel)
            mushroom.lingerTick++;
            mushroom.yOffset += 0.02f;  // 0.06/3 = same total drift
            if ((mushroom.lingerTick % 10u) == 0u && mushroom.capW > 2) mushroom.capW--;
            if ((mushroom.lingerTick % 12u) == 0u && mushroom.stemH > 3) mushroom.stemH--;
            if ((mushroom.lingerTick % 16u) == 0u && mushroom.fireR > 0) mushroom.fireR--;
            if (mushroom.lingerTick >= mushroom.lingerDuration) {
                mushroom.active = false;
            }
            break;
    }
}

// --- update shockwave expansion + debris rain ---
static void updateShockwave(uint32_t now) {
    if (shockwave.phase == SW_INACTIVE) return;

    // tick-gate: 50ms cadence like other update functions
    if (now - lastShockwaveUpdate < 50) return;
    lastShockwaveUpdate = now;

    switch (shockwave.phase) {
        case SW_WAITING_MUSHROOM: {
            // Trigger Akira dome only after impact and first visible mushroom pixels.
            bool mushroomVisible = mushroom.active &&
                                   mushroom.tick > 0 &&
                                   (mushroom.fireR > 0 || mushroom.stemH > 0 || mushroom.capW > 0);
            if (mushroomVisible) {
                float rootX = (shockwave.mushroomX > 0.0f) ? shockwave.mushroomX : mushroom.x;
                rootX = Gfx::clampf(rootX, (float)FOV_SAFE_LEFT, (float)FOV_SAFE_RIGHT);
                shockwave.cx = (float)birdSnap((int16_t)rootX);
                shockwave.cy = SHOCKWAVE_ORIGIN_Y;
                shockwave.radius = 0.0f;
                shockwave.phase = SW_EXPANDING;
                shockwave.phaseStart = now;
                shockwave.ditherSeed = (uint8_t)random(0, 255);
                mushroom.domeTriggered = true;
                // cinematic lock: pig frozen during dome expansion
                { CinematicPose cp; cp.active = true; Avatar::setCinematicPose(cp); }
                SFX::play(SFX::SHOCKWAVE_BOOM);
                Haptic::pulse();  // shockwave impact
                break;
            }

            // 30s safety net — if mushroom never fires, dome from screen center
            uint32_t elapsed = now - shockwave.phaseStart;
            if (elapsed >= SHOCKWAVE_MUSHROOM_TIMEOUT_MS) {
                shockwave.cx = (float)birdSnap(SCREEN_WIDTH / 2);
                shockwave.cy = SHOCKWAVE_ORIGIN_Y;
                shockwave.radius = 0.0f;
                shockwave.phase = SW_EXPANDING;
                shockwave.phaseStart = now;
                shockwave.ditherSeed = (uint8_t)random(0, 255);
                // cinematic lock: pig frozen during dome expansion
                { CinematicPose cp; cp.active = true; Avatar::setCinematicPose(cp); }
                SFX::play(SFX::SHOCKWAVE_BOOM);
                Haptic::pulse();  // shockwave impact
            }
            break;
        }
        case SW_EXPANDING: {
            // Akira dome: 2-phase expansion. slow visible growth, then ACCELERATION.
            // Phase A (0-50%): quadratic ease — dome clearly visible growing from ground
            // Phase B (50-100%): cubic rush — dome ACCELERATES to fill screen
            // uses SHOCKWAVE_EXPANSION_TICKS from top-level constants
            uint32_t elapsed = now - shockwave.phaseStart;
            uint16_t tick = (uint16_t)(elapsed / 50);
            // tree collapse on first tick
            if (tick <= 1 && Avatar::isTreeVisible()) Avatar::hideTree();
            // grass sink: quadratic ease-in over full expansion
            {
                float gt = (float)tick / (float)SHOCKWAVE_EXPANSION_TICKS;
                if (gt > 1.0f) gt = 1.0f;
                Avatar::setGrassYOffset((int16_t)(20.0f * gt * gt));
            }
            if (tick >= SHOCKWAVE_EXPANSION_TICKS) {
                // dome covers screen
                shockwave.radius = DOME_MAX_RADIUS;
                shockwave.phase = SW_FILLED;
                shockwave.phaseStart = now;
                Avatar::setAttackShake(true, true);
            } else {
                float t = (float)tick / (float)SHOCKWAVE_EXPANSION_TICKS;
                float eased;
                if (t < 0.5f) {
                    // Phase A: quadratic growth — dome visibly swelling from ground
                    // maps 0-0.5 → 0-0.15 radius (visible 42px dome at midpoint)
                    float a = t / 0.5f;  // 0→1 over first half
                    eased = 0.15f * a * a;
                } else {
                    // Phase B: cubic rush — dome ACCELERATES outward like shockwave
                    // maps 0.5-1.0 → 0.15-1.0 radius
                    float b = (t - 0.5f) / 0.5f;  // 0→1 over second half
                    eased = 0.15f + 0.85f * b * b * b;
                }
                shockwave.radius = DOME_MAX_RADIUS * eased;
                // crescendo screen shake starting at 10% dome progress
                if (t > 0.10f) {
                    float shakeT = (t - 0.10f) / 0.90f;
                    float shakeAmp = shakeT * shakeT;  // ease-in quadratic
                    Avatar::setAttackShakeSmooth(true, shakeAmp);
                }
            }
            break;
        }
        case SW_FILLED: {
            // re-trigger strong shake during hold
            Avatar::setAttackShake(true, true);
            // brief hold then transition to sand dissipation
            if (now - shockwave.phaseStart >= SHOCKWAVE_DOME_HOLD_MS) {
                shockwave.phase = SW_SAND_DISSIPATE;
                shockwave.phaseStart = now;
                sandPhaseStart = now;
                sandFrontY = 0.0f;
                memset(sandErodedCount, 0, sizeof(sandErodedCount));
            }
            break;
        }
        case SW_SAND_DISSIPATE: {
            // top-down fat-pixel sand dissolve revealing pig beneath
            uint32_t sandElapsed = now - sandPhaseStart;
            float st = (float)sandElapsed / (float)SAND_DURATION_MS;
            if (st > 1.0f) st = 1.0f;
            // smoothstep erosion front (smooth acceleration/deceleration)
            float front = st * st * (3.0f - 2.0f * st);
            sandFrontY = front * (float)SAND_ROWS;
            // per-row erosion count driven by front position
            for (uint8_t row = 0; row < SAND_ROWS; row++) {
                float rowDist = sandFrontY - (float)row;
                if (rowDist <= 0.0f) {
                    sandErodedCount[row] = 0;
                } else {
                    float erode = rowDist / SAND_EROSION_SPAN;
                    if (erode > 1.0f) erode = 1.0f;
                    sandErodedCount[row] = (uint8_t)(erode * (float)SAND_COLS);
                }
            }
            // decaying screen shake (0.8→0 amplitude)
            float shakeDecay = 0.8f * (1.0f - st);
            if (shakeDecay > 0.05f) {
                Avatar::setAttackShakeSmooth(true, shakeDecay);
            }
            // grass recovery during sand phase (20→0)
            {
                float gt = st;
                Avatar::setGrassYOffset((int16_t)(20.0f * (1.0f - gt)));
            }
            // sand done → debris rain
            if (sandElapsed >= SAND_DURATION_MS) {
                shockwave.phase = SW_DEBRIS_RAIN;
                shockwave.phaseStart = now;
                shockwave.pigHypeDelayed = false;
                Avatar::unlockHype();
                Mood::onDebrisRain();
                SFX::play(SFX::DEBRIS_RAIN_START);
                Haptic::buzz();  // fallout rumble
                Avatar::setGrassYOffset(0);
                // start debris rain
                debrisRainStart = now;
                debrisRainDuration = (uint32_t)random(40000, 60001);
                lastDebrisRainSpawn = now;
                debrisSpawnInterval = (uint32_t)random(300, 801);
            }
            break;
        }
        case SW_DEBRIS_RAIN: {

            // spawn new debris using stored interval (roll-once pattern)
            if (now - lastDebrisRainSpawn >= debrisSpawnInterval) {
                lastDebrisRainSpawn = now;
                debrisSpawnInterval = (uint32_t)random(300, 801);  // roll next interval
                for (uint8_t i = 0; i < MAX_RAIN_DEBRIS; i++) {
                    if (!debrisRain[i].active) {
                        float laneX = shockwave.cx + (float)random(-84, 85) + (float)random(-9, 10);
                        laneX = Gfx::clampf(laneX, 0.0f, (float)(SCREEN_WIDTH - 1));
                        debrisRain[i].x = laneX;
                        debrisRain[i].y = (float)random(-24, -4);
                        debrisRain[i].vx = (float)random(-10, 11) / 10.0f;
                        debrisRain[i].vy = 0.5f + (float)random(0, 15) / 10.0f;
                        debrisRain[i].shapeIdx = (uint8_t)random(0, DEBRIS_SHAPE_COUNT);
                        debrisRain[i].rotation = (uint8_t)random(0, 4);
                        debrisRain[i].rotSpeed = (uint8_t)random(4, 9);
                        debrisRain[i].tick = 0;
                        debrisRain[i].prevX = -99;
                        debrisRain[i].prevY = -99;
                        debrisRain[i].active = true;
                        break;
                    }
                }
            }
            // update existing pieces (rotation + physics + trail)
            for (uint8_t i = 0; i < MAX_RAIN_DEBRIS; i++) {
                if (!debrisRain[i].active) continue;
                DebrisRainPiece& d = debrisRain[i];
                // rotation
                d.tick++;
                if (d.tick >= d.rotSpeed) {
                    d.tick = 0;
                    d.rotation = (d.rotation + 1) & 3;
                }
                // trail: store current snapped position before moving
                d.prevX = birdSnap((int16_t)d.x);
                d.prevY = birdSnap((int16_t)d.y);
                // physics
                d.vy += 0.25f;
                d.x += d.vx;
                d.y += d.vy;
                if (d.y > (float)GROUND_Y) {
                    // ground impact rumble — weak shake per hit
                    Avatar::setAttackShake(true, false);
                    // ground impact splash — 2-3 particles per piece
                    int spawned = 0;
                    for (int s = 0; s < SPLASH_COUNT && spawned < 3; s++) {
                        if (!impactSplashes[s].active) {
                            impactSplashes[s].x = d.x + (float)random(-4, 5);
                            impactSplashes[s].y = (float)GROUND_Y;
                            impactSplashes[s].vx = (float)random(-25, 26) / 10.0f;
                            impactSplashes[s].vy = -1.5f - (float)random(0, 20) / 10.0f;
                            impactSplashes[s].life = (uint8_t)random(10, 16);
                            impactSplashes[s].active = true;
                            spawned++;
                        }
                    }
                    d.active = false;
                }
            }
            // end after duration + all landed
            if (now - debrisRainStart >= debrisRainDuration) {
                bool anyActive = false;
                for (uint8_t i = 0; i < MAX_RAIN_DEBRIS; i++) {
                    if (debrisRain[i].active) { anyActive = true; break; }
                }
                if (!anyActive || now - debrisRainStart >= debrisRainDuration + 10000) {
                    shockwave.phase = SW_INACTIVE;
                    shockwave.pigHypeDelayed = false;  // hype unlocked after full sequence
                    Avatar::setGrassYOffset(0);  // ground restored
                    Avatar::clearCinematic();  // release pig from shockwave choreography
                    treeSuppressUntil = now + TREE_SUPPRESS_COOLDOWN_MS;
                }
            }
            break;
        }
        default:
            break;
    }
}

// Forward declarations
static void generateCloudPuffs(CloudShape& cloud);
static void activateCloud();
static void deactivateCloud();

// === INITIALIZATION ===
void init() {
    // single PSRAM alloc for all particle state (~2.6KB)
    if (!wpool) {
        wpool = (WeatherPool*)heap_caps_malloc(sizeof(WeatherPool), MALLOC_CAP_SPIRAM);
        if (!wpool) {
            wpool = (WeatherPool*)malloc(sizeof(WeatherPool));  // DRAM fallback
            HAMLET_LOGF("[WEATHER] DRAM fallback (%d bytes)\n", (int)sizeof(WeatherPool));
        } else {
            HAMLET_LOGF("[WEATHER] PSRAM pool (%d bytes)\n", (int)sizeof(WeatherPool));
        }
    }
    if (!wpool) return;  // both PSRAM and DRAM alloc failed
    memset(wpool, 0, sizeof(WeatherPool));

    whistlingBird = -1;
    lastBirdUpdate = 0;
    nextBirdSpawn = millis() + random(5000, 15001);
    lastShuttleUpdate = 0;
    lastMushroomUpdate = 0;
    lastDebrisUpdate = 0;
    shuttleSpawnTimerSet = false;
    // pool memset already zeroed all particle arrays, shuttle, mushroom, shockwave
    Avatar::setGrassYOffset(0);
    Avatar::clearCinematic();
    lastShockwaveUpdate = 0;
    debrisSpawnInterval = 500;

    lastCloudUpdate = millis();
    lastCloudParallax = lastCloudUpdate;
    lastDensityCheck = lastCloudUpdate;
    lastScaleUpdate = lastCloudUpdate;
    lastWindGust = millis();
    // full thunder state reset
    thunderFlashing = false;
    thunderFlashState = 0;
    thunderFlashesRemaining = 0;
    thunderFlashDuration = 0;
    thunderBurstFiring = false;
    rainActive = false;
    rainDecided = false;
    lastMoodTier = -1;
    lastThunderStorm = millis();
    nextThunderInterval = random(thunderMinInterval, thunderMaxInterval);
}

static void generateCloudPuffs(CloudShape& cloud) {
    cloud.puffs[0] = {0, 0, (uint8_t)random(4, 6)};
    cloud.puffs[1] = {(int8_t)random(-10, -5), (int8_t)random(1, 3), (uint8_t)random(3, 5)};
    cloud.puffs[2] = {(int8_t)random(6, 11), (int8_t)random(1, 3), (uint8_t)random(3, 5)};
    cloud.puffCount = 3;

    if (random(0, 100) < 70) {
        cloud.puffs[cloud.puffCount] = {(int8_t)random(-2, 3), (int8_t)random(-3, -1), (uint8_t)random(2, 4)};
        cloud.puffCount++;
    }
    if (cloud.puffCount < 5 && random(0, 100) < 50) {
        int8_t side = random(0, 2) ? (int8_t)12 : (int8_t)-12;
        cloud.puffs[cloud.puffCount] = {(int8_t)(side + (int8_t)random(-2, 3)), (int8_t)random(0, 3), (uint8_t)random(2, 4)};
        cloud.puffCount++;
    }
}

static int getActiveCloudCount() {
    int count = 0;
    for (int i = 0; i < MAX_CLOUDS; i++) {
        if (clouds[i].active) count++;
    }
    return count;
}

static int getTargetCloudCount() {
    if (rainActive) return MAX_CLOUDS;
    int target = (20 - currentMood) * 8 / 100;
    if (target < 0) target = 0;
    if (target > 8) target = 8;
    return target;
}

static void activateCloud() {
    int slot = -1;
    for (int i = 0; i < MAX_CLOUDS; i++) {
        if (!clouds[i].active) { slot = i; break; }
    }
    if (slot < 0) return;

    float bestX = (float)random(-20, SCREEN_WIDTH + 21);
    float bestDist = 0;
    for (int attempt = 0; attempt < 5; attempt++) {
        float tryX = (float)random(-20, SCREEN_WIDTH + 21);
        float minDist = 400.0f;
        for (int i = 0; i < MAX_CLOUDS; i++) {
            if (i == slot || !clouds[i].active) continue;
            float d = tryX - clouds[i].x;
            float dist = d < 0 ? -d : d;
            if (dist > (float)(SCREEN_WIDTH * 7 / 12)) dist = (float)(SCREEN_WIDTH * 7 / 6) - dist;
            if (dist < minDist) minDist = dist;
        }
        if (minDist > bestDist) {
            bestDist = minDist;
            bestX = tryX;
        }
    }

    CloudShape& c = clouds[slot];
    c.x = bestX;
    c.y = (int8_t)random(SKY_TOP_Y + 9, SKY_TOP_Y + 22);
    c.scale = 0;
    c.active = true;
    c.growing = true;
    c.shrinking = false;
    generateCloudPuffs(c);
}

static void deactivateCloud() {
    float maxDist = -1;
    int pick = -1;
    for (int i = 0; i < MAX_CLOUDS; i++) {
        if (!clouds[i].active || clouds[i].shrinking) continue;
        float d = clouds[i].x - (float)(SCREEN_WIDTH / 2);
        float dist = d < 0 ? -d : d;
        if (dist > maxDist) {
            maxDist = dist;
            pick = i;
        }
    }
    if (pick >= 0) {
        clouds[pick].shrinking = true;
        clouds[pick].growing = false;
    }
}

// === WEATHER STATE CONTROL ===
static int getMoodTier(int mood, int currentTier) {
    if (currentTier < 0) {
        if (mood <= -40) return 2;
        if (mood <= -20) return 1;
        return 0;
    }
    switch (currentTier) {
        case 0:
            if (mood <= -25) return (mood <= -45) ? 2 : 1;
            return 0;
        case 1:
            if (mood <= -45) return 2;
            if (mood > -15) return 0;
            return 1;
        case 2:
            if (mood > -35) return (mood > -15) ? 0 : 1;
            return 2;
    }
    return 0;
}

void setMoodLevel(int momentum) {
    currentMood = momentum;
    int newTier = getMoodTier(momentum, lastMoodTier);

    bool shouldReroll = !rainDecided || (newTier != lastMoodTier);

    if (shouldReroll) {
        lastMoodTier = newTier;
        rainDecided = true;

        bool shouldRain = false;

        if (newTier == 2) {
            shouldRain = (random(0, 100) < RAIN_CHANCE_VERYSAD);
            thunderMinInterval = THUNDER_MIN_INTERVAL_VERYSAD;
            thunderMaxInterval = THUNDER_MAX_INTERVAL_VERYSAD;
        } else if (newTier == 1) {
            shouldRain = (random(0, 100) < RAIN_CHANCE_SAD);
            thunderMinInterval = THUNDER_MIN_INTERVAL_SAD;
            thunderMaxInterval = THUNDER_MAX_INTERVAL_SAD;
        } else {
            shouldRain = false;
            thunderMinInterval = THUNDER_INTERVAL_DISABLED;
            thunderMaxInterval = THUNDER_INTERVAL_DISABLED;
        }

        setRaining(shouldRain);
    }
}

static void seedRainDrop(RainDrop& drop, int index) {
    const uint32_t seed = Gfx::wallHash(index, 0, 0xA17E51u);
    const uint32_t ySpan = (uint32_t)((GROUND_Y - 40) - (SKY_TOP_Y - 2));
    drop.x = (float)(seed % (uint32_t)SCREEN_WIDTH);
    drop.y = (float)(SKY_TOP_Y - 2 + (int)((seed >> 9) % ySpan));
    drop.speed = (uint8_t)(RAIN_SPEED_MIN +
        ((seed >> 20) % (uint32_t)(RAIN_SPEED_MAX - RAIN_SPEED_MIN)));
}

void setRaining(bool active) {
    if (active && !rainActive) {
        lastRainUpdate = millis();
        for (int i = 0; i < RAIN_DROP_COUNT; i++)
            seedRainDrop(rainDrops[i], i);
    } else if (!active && rainActive) {
        thunderFlashing = false;
        thunderFlashState = 0;
        thunderFlashesRemaining = 0;
        thunderFlashDuration = 0;
        thunderBurstFiring = false;
        lastThunderStorm = millis();
    }
    rainActive = active;
}

// Forward declarations for static update functions
static void updateClouds(uint32_t now);
static void updateRain(uint32_t now);
static void updateThunder(uint32_t now);
static void updateWind(uint32_t now);

// ==[ AUTO-FIRE UPDATE ]== pig auto-shoots at visible sky targets during hunt
static void updateAutoFire(uint32_t now) {
    if (Hamlet::getMode() != HamletMode::HUNT) {
        autoFireRemaining = 0;
        return;
    }

    // already firing a burst — wait for next shot
    if (autoFireRemaining > 0) {
        if (TimeMath::before(now, autoFireNextShot)) return;
        // fire. pig go brrr
        Avatar::waveRipple(WaveMode::OUTGOING, 2);
        autoFireRemaining--;
        autoFireNextShot = now + autoFireCooldown;
        return;
    }

    // don't interrupt an active outgoing wave
    if (Avatar::getWaveMode() == WaveMode::OUTGOING) return;

    // scan for visible targets — shuttle only (birds are ignored in hunt mode)
    bool targetVisible = false;
    int16_t sx, sy, sw, sh;
    if (shuttle.active && shuttle.matPhase == SPHASE_MATERIALIZED &&
        getShuttleRect(sx, sy, sw, sh) && rectIntersectsFov(sx, sy, sw, sh)) {
        targetVisible = true;
    }

    if (!targetVisible) return;

    // new target sighting — roll 1-2 burst attempts (rare shots)
    autoFireRemaining = (uint8_t)random(1, 3);
    autoFireCooldown = (uint32_t)random(4000, 8001);  // 4-8s between shots
    autoFireNextShot = now + (uint32_t)random(1000, 3001);  // slower reaction
}

// === ANIMATION UPDATES ===
void update() {
    if (!wpool) return;  // alloc failed — no weather without pool
    uint32_t now = millis();
    updateClouds(now);
    if (rainActive) {
        updateRain(now);
        updateThunder(now);
    }
    updateWind(now);
    updateBirds(now);
    updateShuttle(now);
    updateDebris(now);
    updateMushroom(now);
    updateShockwave(now);
    updateProminences(now);
    updateAutoFire(now);
}

static void updateClouds(uint32_t now) {
    if (now - lastCloudUpdate >= cloudSpeed) {
        lastCloudUpdate = now;
        for (int i = 0; i < MAX_CLOUDS; i++) {
            if (clouds[i].active) clouds[i].x += 0.5f;
        }
    }

    if (Avatar::isGrassMoving()) {
        uint32_t parallaxInterval = (uint32_t)Avatar::getGrassSpeed() * CLOUD_PARALLAX_GRASS_SHIFTS;
        if (parallaxInterval < 150) parallaxInterval = 150;

        if (now - lastCloudParallax >= parallaxInterval) {
            lastCloudParallax = now;
            float shift = Avatar::isGrassDirectionRight() ? 1.0f : -1.0f;
            for (int i = 0; i < MAX_CLOUDS; i++) {
                if (clouds[i].active) clouds[i].x += shift;
            }
        }
    } else {
        lastCloudParallax = now;
    }

    for (int i = 0; i < MAX_CLOUDS; i++) {
        if (!clouds[i].active) continue;
        if (clouds[i].x > (float)(SCREEN_WIDTH + 40)) clouds[i].x -= (float)(SCREEN_WIDTH + 80);
        if (clouds[i].x < -40.0f) clouds[i].x += (float)(SCREEN_WIDTH + 80);
    }

    if (now - lastDensityCheck >= 2000) {
        lastDensityCheck = now;
        int target = getTargetCloudCount();
        int active = getActiveCloudCount();
        if (active < target) {
            activateCloud();
        } else if (active > target) {
            deactivateCloud();
        }
    }

    if (now - lastScaleUpdate >= 80) {
        lastScaleUpdate = now;
        for (int i = 0; i < MAX_CLOUDS; i++) {
            if (!clouds[i].active) continue;
            if (clouds[i].growing) {
                if (clouds[i].scale <= 240) {
                    clouds[i].scale += 15;
                } else {
                    clouds[i].scale = 255;
                    clouds[i].growing = false;
                }
            } else if (clouds[i].shrinking) {
                if (clouds[i].scale >= 15) {
                    clouds[i].scale -= 15;
                } else {
                    clouds[i].scale = 0;
                    clouds[i].active = false;
                    clouds[i].shrinking = false;
                }
            }
        }
    }
}

static void updateRain(uint32_t now) {
    uint32_t elapsed = now - lastRainUpdate;
    if (elapsed == 0) return;
    // Keep motion time-based instead of jumping on a separate 30 ms clock.
    // Clamp a stalled frame so returning from a blocking task does not teleport
    // the whole rain sheet through the ground in one update.
    if (elapsed > 60u) elapsed = 60u;
    lastRainUpdate = now;
    const float step = (float)elapsed / (float)RAIN_REFERENCE_MS;

    float horizontalDrift = 0.0f;
    if (Avatar::isGrassMoving()) {
        uint16_t grassSpeedMs = Avatar::getGrassSpeed();
        if (grassSpeedMs == 0) grassSpeedMs = 1;
        const float grassShiftPixels = 8.0f;
        float grassPixelsPerMs = grassShiftPixels / (float)grassSpeedMs;
        float grassPixelsPerUpdate = grassPixelsPerMs * (float)elapsed;
        horizontalDrift = grassPixelsPerUpdate * 0.4f;
        if (Avatar::isGrassDirectionRight()) {
            horizontalDrift *= -1.0f;
        }
    }

    for (int i = 0; i < RAIN_DROP_COUNT; i++) {
        rainDrops[i].y += (float)rainDrops[i].speed * step;
        rainDrops[i].x += horizontalDrift;

        if (rainDrops[i].x < 0.0f) rainDrops[i].x += (float)SCREEN_WIDTH;
        if (rainDrops[i].x >= (float)SCREEN_WIDTH) rainDrops[i].x -= (float)SCREEN_WIDTH;

        if (rainDrops[i].y >= (float)(GROUND_Y - 3))
            rainDrops[i].y -= (float)((GROUND_Y - 3) - (SKY_TOP_Y - 2));
    }
}

static void updateThunder(uint32_t now) {
    // staleness guard: if we've been away from IDLE, don't instant-burst on return
    if (now - lastThunderStorm > thunderMaxInterval * 3) {
        lastThunderStorm = now;
        nextThunderInterval = random(thunderMinInterval, thunderMaxInterval);
    }

    // schedule new burst
    if (!thunderFlashing && thunderFlashesRemaining == 0) {
        thunderBurstFiring = false;  // burst done, reset SFX gate
        if (now - lastThunderStorm >= nextThunderInterval) {
            thunderFlashesRemaining = random(THUNDER_FLASH_MIN_COUNT, THUNDER_FLASH_MAX_COUNT);
            lastThunderStorm = now;
            nextThunderInterval = random(thunderMinInterval, thunderMaxInterval);
        }
    }

    // start next flash in burst
    if (thunderFlashesRemaining > 0 && !thunderFlashing) {
        thunderFlashing = true;
        thunderFlashStart = now;
        thunderFlashState = 1;
        thunderFlashDuration = random(THUNDER_BRIGHT_MIN_MS, THUNDER_BRIGHT_MAX_MS);
        thunderFlashesRemaining--;
        if (!thunderBurstFiring) {
            SFX::play(SFX::THUNDER_RUMBLE);  // once per burst, not per flash
            Haptic::buzz();
            thunderBurstFiring = true;
        }
    }

    // flash timing: stored thresholds, not rerolled per frame
    if (thunderFlashing) {
        uint32_t elapsed = now - thunderFlashStart;
        if (thunderFlashState == 1 && elapsed > thunderFlashDuration) {
            thunderFlashState = 0;
            thunderFlashStart = now;
            thunderFlashDuration = random(THUNDER_DARK_MIN_MS, THUNDER_DARK_MAX_MS);
        } else if (thunderFlashState == 0 && elapsed > thunderFlashDuration) {
            thunderFlashing = false;
            thunderFlashState = 0;
        }
    }
}

static void updateWind(uint32_t now) {
    if (rainActive) {
        if (windActive) {
            windActive = false;
            for (int i = 0; i < WIND_COUNT; i++) windParticles[i].active = false;
        }
        lastWindGust = now;
        return;
    }

    if (!windActive && now - lastWindGust > windGustInterval) {
        bool grassOn = Avatar::isGrassMoving();
        int spawnChance = grassOn ? WIND_CHANCE_GRASS_ON : WIND_CHANCE_GRASS_OFF;

        if ((int)random(0, 100) < spawnChance) {
            windActive = true;
            windGustDuration = random(WIND_GUST_MIN_MS, WIND_GUST_MAX_MS);
            lastWindGust = now;

            bool goRight = grassOn ? Avatar::isGrassDirectionRight() : (random(0, 2) == 0);

            for (int i = 0; i < WIND_COUNT; i++) {
                float spawnX = goRight
                    ? (-5.0f - random(0, 40))
                    : ((float)SCREEN_WIDTH + 5.0f + random(0, 40));
                windParticles[i].x = spawnX;
                windParticles[i].spawnX = spawnX;
                windParticles[i].y = (float)random(SKY_TOP_Y + 8, GROUND_Y - 34);
                windParticles[i].speed = WIND_BASE_SPEED + (float)random(0, WIND_SPEED_VARIANCE) / 10.0f;
                windParticles[i].maxTravel = (float)random(WIND_TRAVEL_MIN, WIND_TRAVEL_MAX);
                windParticles[i].baseSize = random(1, 4);
                windParticles[i].active = true;
                windParticles[i].dirRight = goRight;
            }
        } else {
            windGustInterval = grassOn ? random(WIND_SPAWN_MIN_GRASS_ON, WIND_SPAWN_MAX_GRASS_ON) : random(WIND_SPAWN_MIN_GRASS_OFF, WIND_SPAWN_MAX_GRASS_OFF);
            lastWindGust = now;
        }
    }

    if (windActive) {
        if (now - lastWindGust > windGustDuration) {
            windActive = false;
            windGustInterval = Avatar::isGrassMoving() ? random(WIND_SPAWN_MIN_GRASS_ON, WIND_SPAWN_MAX_GRASS_ON) : random(WIND_SPAWN_MIN_GRASS_OFF, WIND_SPAWN_MAX_GRASS_OFF);
            for (int i = 0; i < WIND_COUNT; i++) {
                windParticles[i].active = false;
            }
        } else {
            if (now - lastWindUpdate > 50) {
                lastWindUpdate = now;
                for (int i = 0; i < WIND_COUNT; i++) {
                    if (!windParticles[i].active) continue;
                    float dir = windParticles[i].dirRight ? 1.0f : -1.0f;
                    windParticles[i].x += windParticles[i].speed * dir;
                    windParticles[i].y += (random(0, 3) - 1) * 0.5f;

                    float dist = windParticles[i].x - windParticles[i].spawnX;
                    if (dist < 0) dist = -dist;
                    if (dist >= windParticles[i].maxTravel) {
                        windParticles[i].active = false;
                    }
                }
            }
        }
    }
}

// === THUNDER FLASH QUERY ===
bool isThunderFlashing() {
    return thunderFlashing && thunderFlashState == 1;
}

bool isRaining() {
    return rainActive;
}

// === DRAWING ===

static void beginOrdinaryWeatherClip(M5Canvas& canvas) {
    canvas.setClipRect(0, TOP_BAR_H, SCREEN_WIDTH, SCREEN_HEIGHT - TOP_BAR_H);
}

static void endOrdinaryWeatherClip(M5Canvas& canvas) {
    canvas.clearClipRect();
}

static void drawPixelPuff4(M5Canvas& canvas, int cx, int cy, int radius,
                           uint16_t color, int yOffset = 0) {
    if (radius < 1) return;

    const int centerX = birdSnap((int16_t)cx);
    const int centerY = birdSnap((int16_t)(cy + yOffset));
    const int extent = ((radius + BIRD_PX - 1) / BIRD_PX) * BIRD_PX;
    const int radius2 = radius * radius;

    for (int py = centerY - extent; py <= centerY + extent; py += BIRD_PX) {
        for (int px = centerX - extent; px <= centerX + extent; px += BIRD_PX) {
            const int dx = px - centerX;
            const int dy = py - centerY;
            if (dx * dx + dy * dy > radius2) continue;
            canvas.fillRect(px, py, BIRD_PX, BIRD_PX, color);
        }
    }
}


void drawClouds(M5Canvas& canvas, uint16_t capColor, uint16_t shadeColor) {
    beginOrdinaryWeatherClip(canvas);
    const bool flashing = isThunderFlashing();
    const uint16_t cap = flashing ? Display::getColorBG() : capColor;
    const uint16_t shade = flashing ? Display::getColorBG() : shadeColor;

    float rainBoost = rainActive ? 1.8f : 1.0f;
    for (int i = 0; i < MAX_CLOUDS; i++) {
        if (!clouds[i].active || clouds[i].scale == 0) continue;
        float scaleFactor = (float)clouds[i].scale / 255.0f;

        // The lower mass is rendered first and one scenery cell down. The cap
        // then occludes most of it, leaving a stable underside instead of a
        // collection of native-resolution circles that shimmer while drifting.
        for (int p = 0; p < clouds[i].puffCount; p++) {
            int r = (int)((float)clouds[i].puffs[p].radius * scaleFactor *
                          rainBoost + 0.5f);
            int cx = (int)(clouds[i].x + clouds[i].puffs[p].dx);
            int cy = clouds[i].y + clouds[i].puffs[p].dy;
            drawPixelPuff4(canvas, cx, cy, r, shade, BIRD_PX);
        }
        for (int p = 0; p < clouds[i].puffCount; p++) {
            int r = (int)((float)clouds[i].puffs[p].radius * scaleFactor * rainBoost + 0.5f);
            if (r < 1) continue;
            int cx = (int)(clouds[i].x + clouds[i].puffs[p].dx);
            int cy = clouds[i].y + clouds[i].puffs[p].dy;

            drawPixelPuff4(canvas, cx, cy, r, cap);
        }
    }
    endOrdinaryWeatherClip(canvas);
}

void drawSunBase(M5Canvas& canvas, uint16_t colorFG) {
    updateSunPosition();
    drawSunDiscPass1(canvas, colorFG);  // computes sun mask only, no drawing
}

void drawSun(M5Canvas& canvas, uint16_t colorFG) {
    beginOrdinaryWeatherClip(canvas);
    uint16_t drawFG = isThunderFlashing() ? Display::getColorBG() : colorFG;
    uint16_t drawBG = isThunderFlashing() ? colorFG : Display::getColorBG();
    drawSunDiscPass2(canvas, drawFG, drawBG);
    endOrdinaryWeatherClip(canvas);
}

bool getSunDisc(int16_t& x, int16_t& y, int16_t& radius) {
    if (!sunActive || !sunMaskPrimed) return false;
    x = sunX;
    y = sunY;
    radius = SUN_RADIUS;
    return true;
}

void drawBirds(M5Canvas& canvas, uint16_t colorFG) {
    beginOrdinaryWeatherClip(canvas);
    uint16_t drawColor = isThunderFlashing() ? Display::getColorBG() : colorFG;

    for (int i = 0; i < BIRD_COUNT; i++) {
        if (!birds[i].active) continue;
        const SkyBird& b = birds[i];

        if (!b.falling) {
            int16_t bx = skySnap((int16_t)b.x);
            int16_t bodyY = skySnap(b.y + ((b.sinePhase & 0x08) ? SKY_PX : 0));
            bool wingsUp = (b.sinePhase & 0x04) != 0;
            int16_t wingY = wingsUp ? (bodyY - SKY_PX) : (bodyY + SKY_PX);
            canvas.fillRect(bx, wingY, SKY_PX, SKY_PX, drawColor);
            canvas.fillRect(bx + 2 * SKY_PX, wingY, SKY_PX, SKY_PX, drawColor);
            canvas.fillRect(bx + SKY_PX, bodyY, SKY_PX, SKY_PX, drawColor);
        } else {
            int16_t fx = skySnap((int16_t)b.fallX);
            int16_t fy = skySnap((int16_t)b.fallY);
            if (fy >= 0 && fy < SKY_BOTTOM_Y) {
                canvas.fillRect(fx, fy, SKY_PX, SKY_PX, drawColor);
                canvas.fillRect(fx + SKY_PX, fy, SKY_PX, SKY_PX, drawColor);
            }
        }
    }

    for (int s = 0; s < SPARK_COUNT; s++) {
        if (sparks[s].life == 0) continue;
        if (sparks[s].life < 4 && (sparks[s].life % 2 == 0)) continue;
        int16_t sx = skySnap((int16_t)sparks[s].x);
        int16_t sy = skySnap((int16_t)sparks[s].y);
        if (sx >= 0 && sx < SCREEN_WIDTH && sy >= 0 && sy < SKY_BOTTOM_Y)
            canvas.fillRect(sx, sy, SKY_PX, SKY_PX, drawColor);
    }

    // Ordinary birds and incidental sparks respect UI chrome. Impact bursts,
    // shuttle passes, and the mushroom tableau are deliberate cinematics.
    endOrdinaryWeatherClip(canvas);

    for (int e = 0; e < EXPLOSION_COUNT; e++) {
        if (!explosions[e].active) continue;
        if (explosions[e].life < 4 && (explosions[e].life % 2 == 0)) continue;
        int16_t cx = (int16_t)explosions[e].x;
        int16_t cy = (int16_t)explosions[e].y;
        int16_t r = (int16_t)explosions[e].radius;
        const int16_t pts[][2] = {
            {0, (int16_t)(-r)}, {0, r}, {(int16_t)(-r), 0}, {r, 0},
            {(int16_t)(r * 7 / 10), (int16_t)(-r * 7 / 10)},
            {(int16_t)(-r * 7 / 10), (int16_t)(-r * 7 / 10)},
            {(int16_t)(r * 7 / 10), (int16_t)(r * 7 / 10)},
            {(int16_t)(-r * 7 / 10), (int16_t)(r * 7 / 10)}
        };
        for (int p = 0; p < 8; p++) {
            int16_t px = skySnap(cx + pts[p][0]);
            int16_t py = skySnap(cy + pts[p][1]);
            if (px >= 0 && px < SCREEN_WIDTH && py >= 0 && py < SKY_BOTTOM_Y)
                canvas.fillRect(px, py, SKY_PX, SKY_PX, drawColor);
        }
    }

    for (int s = 0; s < SPLASH_COUNT; s++) {
        if (!impactSplashes[s].active) continue;
        if (impactSplashes[s].life < 4 && (impactSplashes[s].life % 2 == 0)) continue;
        int16_t sx = skySnap((int16_t)impactSplashes[s].x);
        int16_t sy = skySnap((int16_t)impactSplashes[s].y);
        if (sx >= 0 && sx < SCREEN_WIDTH && sy >= 0 && sy < SKY_BOTTOM_Y)
            canvas.fillRect(sx, sy, SKY_PX, SKY_PX, drawColor);
    }

    // ==[ NARCISSUS SHUTTLE ]== cinematic entry + thruster blink
    if (shuttle.active) {
        int16_t sx = birdSnap((int16_t)shuttle.x);
        int16_t sy = birdSnap((int16_t)shuttle.yFloat);
        bool goR = (shuttle.vx > 0);

        // hype rainbow available? (mood > 70)
        bool useHypeColor = (Mood::getEffectiveMood() > 70);

        // ==[ FALLING SHUTTLE ]== broken sprite + fire trails
        if (shuttle.matPhase == SPHASE_FALLING) {
            // trail particles (hype-colored only when mood > 70)
            for (uint8_t t = 0; t < TRAIL_LEN; t++) {
                const ShuttleTrail& tr = shuttle.trail[t];
                if (tr.age >= 255) continue;
                int16_t tx = tr.x;
                int16_t ty = tr.y;
                if (tx < -BIRD_PX * 2 || tx > (SCREEN_WIDTH + BIRD_PX - 1) || ty < 0 || ty > SKY_BOTTOM_Y) continue;

                if (tr.age <= 2) {
                    // fresh: 2x1 fat blocks (fire trail) — whitest white
                    canvas.fillRect(tx, ty, BIRD_PX * 2, BIRD_PX, (uint16_t)0xFFFF);
                } else if (tr.age <= 5) {
                    // cooling: single block — whitest white
                    canvas.fillRect(tx, ty, BIRD_PX, BIRD_PX, (uint16_t)0xFFFF);
                } else if (tr.age <= 8) {
                    // fading: FG dithered
                    if (!(tr.age & 1))
                        canvas.fillRect(tx, ty, BIRD_PX, BIRD_PX, drawColor);
                }
            }

            // broken shuttle sprite: dithered/flickering, missing chunks
            const uint8_t* spr = goR ? NARCISSUS_R : NARCISSUS_L;
            for (uint8_t row = 0; row < NARCISSUS_H; row++) {
                uint8_t bits = spr[row];
                for (uint8_t col = 0; col < NARCISSUS_W; col++) {
                    if (!(bits & (1u << (NARCISSUS_W - 1 - col)))) continue;
                    // progressive breakup: starts intact, more pixels missing over time
                    uint8_t hash = (uint8_t)((row * 37u + col * 13u + shuttle.fallDitherSeed) & 0xFF);
                    uint8_t threshold = (shuttle.fallTick > 60) ? 180 :
                                        (shuttle.fallTick > 30) ? 120 :
                                        (shuttle.fallTick > 10) ? 60 :
                                        (shuttle.fallTick > 3)  ? 30 : 0;
                    if (hash < threshold) continue;  // pixel broken off

                    int16_t px = sx + col * BIRD_PX;
                    int16_t py = sy + row * BIRD_PX;
                    // jitter: shake each pixel randomly as ship tumbles
                    if (shuttle.fallTick > 5) {
                        px += (int16_t)(esp_random() % 3) - 1;
                        py += (int16_t)(esp_random() % 3) - 1;
                    }
                    if (px >= 0 && px < SCREEN_WIDTH && py >= 0 && py < SKY_BOTTOM_Y)
                        canvas.fillRect(px, py, BIRD_PX, BIRD_PX, drawColor);
                }
            }

        // ==[ CINEMATIC REENTRY PHASES ]== draw based on current phase
        } else if (shuttle.fromNostromo && shuttle.matPhase != SPHASE_MATERIALIZED) {

            // --- WAKE TRAIL --- shared across reentry phases
            for (uint8_t w = 0; w < WAKE_LEN; w++) {
                const WakeParticle& wp = shuttle.wake[w];
                if (wp.age >= 255) continue;
                int16_t wx = wp.x;
                int16_t wy = wp.y;
                if (wx < -6 || wx > (SCREEN_WIDTH + BIRD_PX - 1) || wy < 0 || wy > SKY_BOTTOM_Y) continue;
                int16_t grid = shuttle.pixelGrid;
                // age-based draw: young=solid, mid=50% dither, old=25%
                bool draw = true;
                if (wp.age > 24) draw = ((wp.age & 3) == 0);       // 25% sparse
                else if (wp.age > 8) draw = ((wp.age & 1) == 0);   // 50% dither
                // age 0-8: always solid
                if (!draw) continue;
                uint16_t wColor = useHypeColor ? Avatar::getHypeColor(wx, wy) : drawColor;
                canvas.fillRect(wx, wy, grid, grid, wColor);
                // perpendicular spread (comet tail width)
                if (wp.spread != 0) {
                    int16_t above = wy - abs(wp.spread);
                    int16_t below = wy + abs(wp.spread);
                    if (above >= 0 && above < SKY_BOTTOM_Y)
                        canvas.fillRect(wx, above, grid, grid, wColor);
                    if (below >= 0 && below < SKY_BOTTOM_Y)
                        canvas.fillRect(wx, below, grid, grid, wColor);
                }
            }

            // --- BOW SHOCK CRESCENT --- tapered nose ahead of object
            if (shuttle.bowShock.outerR > 0) {
                int16_t objCX = sx + (shuttle.drawW * shuttle.pixelGrid) / 2;
                int16_t objCY = sy + (shuttle.drawH * shuttle.pixelGrid) / 2;
                int16_t shockDir = goR ? 1 : -1;
                uint8_t oR = shuttle.bowShock.outerR;
                uint8_t iR = shuttle.bowShock.innerR;
                for (int8_t dy = -(int8_t)oR; dy <= (int8_t)oR; dy++) {
                    // taper: center (dy=0) extends to outerR, edges retract to innerR
                    uint8_t absY = (uint8_t)abs(dy);
                    uint8_t maxR = oR - (absY * (oR - iR)) / max((uint8_t)1, oR);
                    if (maxR < iR) continue;  // edge rows culled when crescent thin
                    for (uint8_t r = iR; r <= maxR; r++) {
                        if ((esp_random() % 100) < shuttle.bowShock.flicker) continue;
                        int16_t bpx = birdSnap(objCX + shockDir * (int16_t)r * BIRD_PX);
                        int16_t bpy = birdSnap(objCY + (int16_t)dy * BIRD_PX);
                        if (bpx >= 0 && bpx < SCREEN_WIDTH && bpy >= 0 && bpy < SKY_BOTTOM_Y) {
                            uint16_t sc = useHypeColor ? Avatar::getHypeColor(bpx, bpy) : drawColor;
                            canvas.fillRect(bpx, bpy, BIRD_PX, BIRD_PX, sc);
                        }
                    }
                }
            }

            // --- PHASE-SPECIFIC OBJECT ---
            if (shuttle.matPhase == SPHASE_FIRST_LIGHT) {
                // flickering PIG_PX dot
                bool visible = true;
                if (shuttle.matTick < 500) visible = ((esp_random() & 1) == 0);         // 50% dropout
                else if (shuttle.matTick < 1000) visible = ((esp_random() % 10) >= 3);  // 30% dropout
                else visible = ((esp_random() % 10) >= 1);                              // 10% dropout
                if (visible && sx >= 0 && sx < SCREEN_WIDTH && sy >= 0 && sy < SKY_BOTTOM_Y)
                    canvas.fillRect(sx, sy, REENTRY_PX, REENTRY_PX, drawColor);

            } else if (shuttle.matPhase == SPHASE_WAKE_FORMING) {
                // solid PIG_PX dot, grows to 2x1
                int16_t dotW = shuttle.drawW * REENTRY_PX;
                if (sx >= 0 && sx < SCREEN_WIDTH && sy >= 0 && sy < SKY_BOTTOM_Y)
                    canvas.fillRect(sx, sy, dotW, REENTRY_PX, drawColor);

            } else if (shuttle.matPhase == SPHASE_HEATING) {
                // growing object + ionization flicker
                int16_t grid = shuttle.pixelGrid;
                int16_t w = shuttle.drawW * grid;
                int16_t h = shuttle.drawH * grid;
                bool ionDrop = (shuttle.matTick > 400) && ((esp_random() % 5) == 0);
                if (!ionDrop && sx >= 0 && sx < SCREEN_WIDTH && sy >= 0 && sy < SKY_BOTTOM_Y)
                    canvas.fillRect(sx, sy, w, h, drawColor);

            } else if (shuttle.matPhase == SPHASE_FIREBALL) {
                // 2x2 BIRD_PX core with heat shield void
                uint16_t colorBGlocal = Display::getColorBG();
                int16_t coreW = shuttle.drawW * BIRD_PX;
                int16_t coreH = shuttle.drawH * BIRD_PX;
                // heat shield void behind the core
                if (sx >= 0 && sx < SCREEN_WIDTH && sy + BIRD_PX >= 0 && sy + BIRD_PX < SKY_BOTTOM_Y)
                    canvas.fillRect(sx, sy + BIRD_PX, BIRD_PX, BIRD_PX, colorBGlocal);
                // core (with 15% ionization flicker)
                bool ionDrop = ((esp_random() % 100) < 15);
                if (!ionDrop && sx >= 0 && sx < SCREEN_WIDTH && sy >= 0 && sy < SKY_BOTTOM_Y) {
                    uint16_t coreC = useHypeColor ? Avatar::getHypeColor(sx, sy) : drawColor;
                    canvas.fillRect(sx, sy, coreW, coreH, coreC);
                }

            } else if (shuttle.matPhase == SPHASE_DECELERATION) {
                // growing hull hint block
                int16_t w = shuttle.drawW * BIRD_PX;
                int16_t h = shuttle.drawH * BIRD_PX;
                // occasional flare at nose
                bool flare = (((shuttle.matTick / 8) & 3) == 0);
                if (sx >= 0 && sx < SCREEN_WIDTH && sy >= 0 && sy < SKY_BOTTOM_Y)
                    canvas.fillRect(sx, sy, w, h, drawColor);
                if (flare) {
                    int16_t noseX = goR ? (sx + w) : (sx - BIRD_PX);
                    if (noseX >= 0 && noseX < SCREEN_WIDTH && sy >= 0 && sy < SKY_BOTTOM_Y)
                        canvas.fillRect(noseX, sy + BIRD_PX, BIRD_PX, BIRD_PX, drawColor);
                }

            } else if (shuttle.matPhase == SPHASE_COOLING) {
                // materializing sprite via revealMask + thruster tilt + PIG_PX thrusters
                const uint8_t* spr = goR ? NARCISSUS_R : NARCISSUS_L;
                uint8_t engineMask = goR ? (1u << (NARCISSUS_W - 1)) : 1u;
                int8_t tiltPx = shuttle.thrustTilt * BIRD_PX;
                for (uint8_t row = 0; row < NARCISSUS_H; row++) {
                    uint8_t bits = spr[row] & shuttle.revealMask[row];
                    if (!shuttle.thrusterBlink) bits &= ~engineMask;
                    int16_t rowShift = (row <= 1) ? (goR ? tiltPx : -tiltPx) : 0;
                    for (uint8_t col = 0; col < NARCISSUS_W; col++) {
                        if (bits & (1u << (NARCISSUS_W - 1 - col))) {
                            int16_t px = sx + col * BIRD_PX + rowShift;
                            int16_t py = sy + row * BIRD_PX;
                            bool ionFlicker = (shuttle.matTick < 300) && ((esp_random() % 10) < 2);
                            if (!ionFlicker && px >= -BIRD_PX && px < (SCREEN_WIDTH + BIRD_PX) && py >= 0 && py < SKY_BOTTOM_Y)
                                canvas.fillRect(px, py, BIRD_PX, BIRD_PX, drawColor);
                        }
                    }
                }
                // PIG_PX thruster flames
                for (uint8_t ti = 0; ti < THRUSTER_COUNT; ti++) {
                    if (!shuttle.thrusters[ti].on) continue;
                    if ((shuttle.matTick % 4) == 3) continue;  // blink off 1/4 ticks
                    int16_t fx, fy;
                    int16_t sprW = NARCISSUS_W * BIRD_PX;
                    int16_t sprH = NARCISSUS_H * BIRD_PX;
                    if (goR) {
                        fx = (ti < 2) ? (sx - REENTRY_PX) : (sx + sprW);
                        fy = (ti == 0 || ti == 2) ? sy : (sy + sprH - REENTRY_PX);
                    } else {
                        fx = (ti < 2) ? (sx + sprW) : (sx - REENTRY_PX);
                        fy = (ti == 0 || ti == 2) ? sy : (sy + sprH - REENTRY_PX);
                    }
                    if (fx >= 0 && fx < SCREEN_WIDTH && fy >= 0 && fy < SKY_BOTTOM_Y) {
                        uint16_t fc = Avatar::getHypeColor(fx, fy);
                        canvas.fillRect(fx, fy, REENTRY_PX, REENTRY_PX, fc);
                    }
                }
            }

        } else {
            // ==[ MATERIALIZED FLIGHT ]== full sprite with smooth turn animation + thrusters
            // select sprite based on bankPhase (20-tick turn animation)
            const uint8_t* spr;
            uint8_t sprW;
            if (shuttle.bankPhase >= 1 && shuttle.bankPhase <= 20) {
                if (shuttle.bankPhase <= 4 || shuttle.bankPhase >= 17) {
                    spr = goR ? NARCISSUS_R : NARCISSUS_L;
                    sprW = NARCISSUS_W;
                } else if (shuttle.bankPhase <= 8) {
                    spr = NARC_TURN_A;
                    sprW = NARC_TURN_A_W;
                } else if (shuttle.bankPhase <= 12) {
                    spr = NARC_TURN_F;
                    sprW = NARC_TURN_F_W;
                } else {
                    spr = NARC_TURN_B;
                    sprW = NARC_TURN_B_W;
                }
            } else {
                spr = goR ? NARCISSUS_R : NARCISSUS_L;
                sprW = NARCISSUS_W;
            }
            uint8_t engineMask = goR ? (1u << (sprW - 1)) : 1u;
            // center narrower sprites on full-width midpoint
            int16_t centerOffset = (NARCISSUS_W - sprW) * BIRD_PX / 2;
            // tilt from thrusters or banking dip
            int8_t tiltPx = shuttle.thrustTilt * BIRD_PX;
            for (uint8_t row = 0; row < NARCISSUS_H; row++) {
                uint8_t bits = spr[row];
                if (!shuttle.thrusterBlink) bits &= ~engineMask;
                int16_t rowShift = (row <= 1) ? (goR ? tiltPx : -tiltPx) : 0;
                for (uint8_t col = 0; col < sprW; col++) {
                    if (bits & (1u << (sprW - 1 - col))) {
                        int16_t px = sx + centerOffset + col * BIRD_PX + rowShift;
                        int16_t py = sy + row * BIRD_PX;
                        if (px >= -BIRD_PX && px < (SCREEN_WIDTH + BIRD_PX) && py >= 0 && py < SKY_BOTTOM_Y)
                            canvas.fillRect(px, py, BIRD_PX, BIRD_PX, drawColor);
                    }
                }
            }
            // nose spark
            if (((shuttle.enginePhase / 6) & 1) == 0) {
                int16_t sparkX = goR ? (sx + NARCISSUS_W * BIRD_PX) : (sx - BIRD_PX);
                int16_t sparkY = sy + BIRD_PX;
                if (sparkX >= 0 && sparkX < SCREEN_WIDTH && sparkY >= 0 && sparkY < SKY_BOTTOM_Y)
                    canvas.fillRect(sparkX, sparkY, BIRD_PX, BIRD_PX, drawColor);
            }
            // PIG_PX thruster flames (materialized flight)
            for (uint8_t ti = 0; ti < THRUSTER_COUNT; ti++) {
                if (!shuttle.thrusters[ti].on) continue;
                if ((shuttle.enginePhase % 4) == 3) continue;
                int16_t fx, fy;
                int16_t sprWpx = NARCISSUS_W * BIRD_PX;
                int16_t sprHpx = NARCISSUS_H * BIRD_PX;
                if (goR) {
                    fx = (ti < 2) ? (sx - REENTRY_PX) : (sx + sprWpx);
                    fy = (ti == 0 || ti == 2) ? sy : (sy + sprHpx - REENTRY_PX);
                } else {
                    fx = (ti < 2) ? (sx + sprWpx) : (sx - REENTRY_PX);
                    fy = (ti == 0 || ti == 2) ? sy : (sy + sprHpx - REENTRY_PX);
                }
                if (fx >= 0 && fx < SCREEN_WIDTH && fy >= 0 && fy < SKY_BOTTOM_Y) {
                    uint16_t fc = Avatar::getHypeColor(fx, fy);
                    canvas.fillRect(fx, fy, REENTRY_PX, REENTRY_PX, fc);
                }
            }
            // exhaust trail
            for (uint8_t t = 0; t < TRAIL_LEN; t++) {
                const ShuttleTrail& tr = shuttle.trail[t];
                if (tr.age >= 255) continue;
                int16_t tx = tr.x;
                int16_t ty = tr.y;
                if (tx < -BIRD_PX * 2 || tx > (SCREEN_WIDTH + BIRD_PX - 1) || ty < 0 || ty > SKY_BOTTOM_Y) continue;
                if (tr.age <= 1)
                    canvas.fillRect(tx, ty, BIRD_PX * 2, BIRD_PX * 2, drawColor);
                else if (tr.age <= 3)
                    canvas.fillRect(tx, ty, BIRD_PX, BIRD_PX, drawColor);
                else if (tr.age <= 6 && !(tr.age & 1))
                    canvas.fillRect(tx, ty, BIRD_PX, BIRD_PX, drawColor);
            }
        }
    }

    // ==[ DEBRIS FRAGMENTS ]== falling ship pieces with trail
    for (uint8_t i = 0; i < MAX_DEBRIS; i++) {
        if (!shuttleDebris[i].active) continue;
        const ShuttleDebris& d = shuttleDebris[i];
        int16_t dx = birdSnap((int16_t)d.x);
        int16_t dy = birdSnap((int16_t)d.y);

        // trail: small fat-pixel block at previous position
        if (d.prevX != dx || d.prevY != dy) {
            if (d.prevX >= 0 && d.prevX < SCREEN_WIDTH && d.prevY >= 0 && d.prevY < SKY_BOTTOM_Y) {
                canvas.fillRect(d.prevX, d.prevY, 2, 2, drawColor);
            }
        }

        // debris block
        if (dx >= 0 && dx < SCREEN_WIDTH && dy >= 0 && dy < SKY_BOTTOM_Y) {
            int16_t sz = d.size * BIRD_PX;
            canvas.fillRect(dx, dy, sz, sz, drawColor);
        }
    }

    // ==[ MUSHROOM CLOUD ]== fat-pixel atomic blast
    if (mushroom.active) {
        int16_t mx = birdSnap((int16_t)mushroom.x);
        int16_t groundY = birdSnap(GROUND_Y + (int16_t)mushroom.yOffset);
        bool flashFade = (mushroom.phase == 3);
        bool linger = (mushroom.phase == 4);
        bool dither = flashFade || linger;
        uint16_t fade16 = flashFade ? (uint16_t)(mushroom.tick / 3u) : (linger ? (mushroom.lingerTick / 2u) : 0u);
        if (fade16 > 255u) fade16 = 255u;
        uint8_t fade = (uint8_t)fade16;
        uint16_t carveColor = isThunderFlashing() ? Display::getColorFG() : Display::getColorBG();

        // fireball at ground (phases 0-1 + residual glow during dissipate)
        if (mushroom.fireR > 0 && mushroom.phase <= 4) {
            int16_t fr = (int16_t)mushroom.fireR;
            for (int16_t row = -fr; row <= fr; row++) {
                for (int16_t col = -fr; col <= fr; col++) {
                    if (row * row + col * col <= fr * fr + fr) {
                        int16_t px = mx + col * BIRD_PX;
                        int16_t py = groundY + row * BIRD_PX;
                        if (linger && ((row + col + (int16_t)(mushroom.lingerTick / 3u)) & 1)) continue;
                        if (px >= 0 && px < SCREEN_WIDTH && py >= 0 && py < (SKY_BOTTOM_Y + BIRD_PX)) {
                            canvas.fillRect(px, py, BIRD_PX, BIRD_PX, drawColor);
                        }
                    }
                }
            }
        }

        // stem (phases 1-3) — dynamic width: 2 blocks base, 4 blocks at h>=14
        if (mushroom.phase >= 1 && mushroom.stemH > 0) {
            int8_t stemW = 2 + (int8_t)(mushroom.stemH / 14);  // 2 blocks until h=13, 4 at h>=14
            int8_t wStart = -(stemW / 2);
            int8_t wEnd = wStart + stemW - 1;
            for (uint8_t s = 0; s < mushroom.stemH; s++) {
                int16_t py = groundY - (s + 1) * BIRD_PX;
                if (py < 0) break;
                int16_t shear = (mushroom.stemH > 0)
                              ? (int16_t)(((int32_t)mushroom.lean * (int32_t)s) / (int32_t)mushroom.stemH)
                              : 0;
                for (int8_t w = wStart; w <= wEnd; w++) {
                    int16_t px = mx + (w + shear) * BIRD_PX;
                    if (dither && fade > 6 && ((s + w) & 1)) continue;  // fade dither
                    if (linger && fade > 24 && (((int16_t)s + w + (int16_t)(mushroom.lingerTick / 5u)) % 3 == 0)) continue;
                    if (px >= 0 && px < SCREEN_WIDTH && py >= 0 && py < (SKY_BOTTOM_Y + BIRD_PX)) {
                        canvas.fillRect(px, py, BIRD_PX, BIRD_PX, drawColor);
                    }
                }
            }
        }

        // cap (phases 2-3) — curled mushroom shape
        if (mushroom.phase >= 2 && mushroom.capW > 0) {
            int16_t capY = groundY - (int16_t)mushroom.stemH * BIRD_PX;
            int16_t cw = (int16_t)mushroom.capW;
            int16_t midShift = mushroom.lean * BIRD_PX;
            int16_t topShift = midShift * 2;

            // dome top (row -2): narrower, ~60% width
            int16_t domeW = cw * 3 / 5;
            if (domeW < 1) domeW = 1;
            for (int16_t col = -domeW; col <= domeW; col++) {
                int16_t px = mx + col * BIRD_PX + topShift;
                int16_t py = capY - 2 * BIRD_PX;
                if (dither && fade > 3 && ((col) & 1)) continue;
                if (dither && fade > 9) continue;
                if (linger && fade > 16 && (((col + (int16_t)(mushroom.lingerTick / 4u)) & 3) == 0)) continue;
                if (px >= 0 && px < SCREEN_WIDTH && py >= 0 && py < (SKY_BOTTOM_Y + BIRD_PX))
                    canvas.fillRect(px, py, BIRD_PX, BIRD_PX, drawColor);
            }

            // main cap (row -1): full width
            for (int16_t col = -cw; col <= cw; col++) {
                int16_t px = mx + col * BIRD_PX + midShift;
                int16_t py = capY - BIRD_PX;
                if (dither && fade > 3 && ((col + 1) & 1)) continue;
                if (dither && fade > 9) continue;
                if (linger && fade > 18 && (((col + (int16_t)(mushroom.lingerTick / 3u)) % 3) == 0)) continue;
                if (px >= 0 && px < SCREEN_WIDTH && py >= 0 && py < (SKY_BOTTOM_Y + BIRD_PX))
                    canvas.fillRect(px, py, BIRD_PX, BIRD_PX, drawColor);
            }

            // curling edges (row 0): 2 blocks at each side dip down
            for (int8_t side = -1; side <= 1; side += 2) {
                for (int8_t c = 0; c < 2; c++) {
                    int16_t col = side * (cw - c);
                    int16_t px = mx + col * BIRD_PX + midShift;
                    int16_t py = capY;  // same Y as stem top = curling down
                    if (dither && fade > 3 && ((col + c) & 1)) continue;
                    if (dither && fade > 9) continue;
                    if (linger && fade > 20 && (((col + (int16_t)mushroom.lingerTick) & 1) == 0)) continue;
                    if (px >= 0 && px < SCREEN_WIDTH && py >= 0 && py < (SKY_BOTTOM_Y + BIRD_PX))
                        canvas.fillRect(px, py, BIRD_PX, BIRD_PX, drawColor);
                }
            }

            // carve a neck notch for stronger mushroom silhouette.
            int16_t notchW = (cw > 9) ? (BIRD_PX * 2) : BIRD_PX;
            int16_t notchX = mx - notchW / 2 + midShift;
            if (notchX >= 0 && notchX + notchW < SCREEN_WIDTH && capY >= 0 && capY < (SKY_BOTTOM_Y + BIRD_PX)) {
                canvas.fillRect(notchX, capY, notchW, BIRD_PX, carveColor);
            }
        }
    }

}

void draw(M5Canvas& canvas, uint16_t colorFG, uint16_t colorBG) {
    if (!wpool) return;  // alloc failed — skip weather rendering
    beginOrdinaryWeatherClip(canvas);
    SKY_PX = Display::getSceneryPX();  // refresh per frame
    uint16_t drawColor = isThunderFlashing() ? colorBG : colorFG;

    if (rainActive) {
        for (int i = 0; i < RAIN_DROP_COUNT; i++) {
            // The simulation is time-continuous; raster positions remain on
            // the authored 4px scenery grid with a fixed two-cell silhouette.
            int16_t rx = birdSnap((int16_t)rainDrops[i].x);
            int16_t ry = birdSnap((int16_t)rainDrops[i].y);
            if (ry < 0) continue;
            if (ry < (GROUND_Y - 3)) {
                canvas.fillRect(rx, ry, BIRD_PX, BIRD_PX, drawColor);
            }
            if (ry + BIRD_PX < (GROUND_Y - 3)) {
                canvas.fillRect(rx, ry + BIRD_PX, BIRD_PX, BIRD_PX, drawColor);
            }
        }
    }

    if (windActive) {
        for (int i = 0; i < WIND_COUNT; i++) {
            if (!windParticles[i].active) continue;
            int16_t wx = skySnap((int16_t)windParticles[i].x);
            int16_t wy = skySnap((int16_t)windParticles[i].y);
            if (wx < -SKY_PX || wx > SCREEN_WIDTH + SKY_PX) continue;

            float dist = windParticles[i].x - windParticles[i].spawnX;
            if (dist < 0) dist = -dist;
            float progress = dist / windParticles[i].maxTravel;
            if (progress > 1.0f) progress = 1.0f;
            int blocks = (int)((float)windParticles[i].baseSize * (1.0f - progress) + 0.5f);
            if (blocks < 1) continue;

            for (int b = 0; b < blocks; b++) {
                int16_t bx = windParticles[i].dirRight ? (wx + b * SKY_PX) : (wx - b * SKY_PX);
                if (bx >= 0 && bx < SCREEN_WIDTH && wy >= 0 && wy < SKY_BOTTOM_Y) {
                    canvas.fillRect(bx, wy, SKY_PX, SKY_PX, drawColor);
                }
            }
        }
    }
    endOrdinaryWeatherClip(canvas);
}

// ==[ SHOCKWAVE DRAW ]== full-circle akira fill from impact origin.
void drawShockwave(M5Canvas& canvas) {
    if (shockwave.phase == SW_SAND_DISSIPATE) {
        // top-down sand dissolve: iterate sky band in fat-pixel grid
        uint8_t maxRow = (uint8_t)((SKY_BOTTOM_Y + 2) / BIRD_PX);  // 39 rows (y < 118, +2px)
        if (maxRow > SAND_ROWS) maxRow = SAND_ROWS;
        for (uint8_t row = 0; row < maxRow; row++) {
            uint8_t eroded = sandErodedCount[row];
            if (eroded >= SAND_COLS) continue;  // fully eroded, skip row
            int16_t py = (int16_t)row * BIRD_PX;
            for (uint8_t col = 0; col < SAND_COLS; col++) {
                // deterministic hash for scattered dissolve pattern
                uint8_t hash = (uint8_t)((col * 37u + row * 13u + shockwave.ditherSeed) % SAND_COLS);
                if (hash < eroded) continue;  // eroded block, pig shows through
                int16_t px = (int16_t)col * BIRD_PX;
                canvas.fillRect(px, py, BIRD_PX, BIRD_PX, (uint16_t)0xFFFF);
            }
        }
        return;
    }

    if (shockwave.phase != SW_EXPANDING && shockwave.phase != SW_FILLED) return;

    int16_t cx = (int16_t)shockwave.cx;
    int16_t cy = (int16_t)shockwave.cy;
    int16_t r = (int16_t)shockwave.radius;
    if (r < 1) return;

    int16_t yStart = cy - r;
    if (yStart < 0) yStart = 0;
    int16_t yEnd = cy + r;
    if (yEnd > SKY_BOTTOM_Y + 1) yEnd = SKY_BOTTOM_Y + 1;  // +2px lower bound
    yStart = (yStart / BIRD_PX) * BIRD_PX;

    for (int16_t y = yStart; y <= yEnd; y += BIRD_PX) {
        int16_t dy = y + BIRD_PX / 2 - cy;
        int32_t dx2 = (int32_t)r * r - (int32_t)dy * dy;
        if (dx2 < 0) continue;
        int16_t dx = (int16_t)sqrtf((float)dx2);

        int16_t xStart = cx - dx;
        int16_t xEnd = cx + dx;
        if (xStart < 0) xStart = 0;
        if (xEnd > SCREEN_WIDTH - 1) xEnd = SCREEN_WIDTH - 1;
        xStart = (xStart / BIRD_PX) * BIRD_PX;

        for (int16_t x = xStart; x <= xEnd; x += BIRD_PX) {
            canvas.fillRect(x, y, BIRD_PX, BIRD_PX, (uint16_t)0xFFFF);
        }
    }
}

// ==[ DEBRIS RAIN DRAW ]== fallout bitmask shapes with rotation + trail
void drawDebrisRain(M5Canvas& canvas, uint16_t colorFG) {
    if (shockwave.phase != SW_DEBRIS_RAIN) return;
    uint16_t drawColor = isThunderFlashing() ? Display::getColorBG() : colorFG;

    for (uint8_t i = 0; i < MAX_RAIN_DEBRIS; i++) {
        if (!debrisRain[i].active) continue;
        const DebrisRainPiece& d = debrisRain[i];
        int16_t dx = birdSnap((int16_t)d.x);
        int16_t dy = birdSnap((int16_t)d.y);
        if (dy < -BIRD_PX * 3 || dy > SKY_BOTTOM_Y) continue;

        // trail ghost at previous position (single fat-pixel block)
        if (d.prevX != -99 && (d.prevX != dx || d.prevY != dy)) {
            if (d.prevX >= 0 && d.prevX < SCREEN_WIDTH && d.prevY >= 0 && d.prevY < SKY_BOTTOM_Y)
                canvas.fillRect(d.prevX, d.prevY, BIRD_PX, BIRD_PX, drawColor);
        }

        // rotated 3x3 bitmask shape
        const uint8_t* shape = DEBRIS_SHAPES[d.shapeIdx];
        for (uint8_t row = 0; row < 3; row++) {
            for (uint8_t col = 0; col < 3; col++) {
                // rotate source coords
                uint8_t srcRow, srcCol;
                switch (d.rotation) {
                    case 0: srcRow = row; srcCol = col; break;
                    case 1: srcRow = 2 - col; srcCol = row; break;
                    case 2: srcRow = 2 - row; srcCol = 2 - col; break;
                    default: srcRow = col; srcCol = 2 - row; break;
                }
                if (shape[srcRow] & (1u << (2 - srcCol))) {
                    int16_t px = dx + col * BIRD_PX;
                    int16_t py = dy + row * BIRD_PX;
                    if (px >= 0 && px < SCREEN_WIDTH && py >= 0 && py < SKY_BOTTOM_Y)
                        canvas.fillRect(px, py, BIRD_PX, BIRD_PX, drawColor);
                }
            }
        }
    }
}

// ==[ SHOCKWAVE QUERY ]== pig hype delay during expansion
bool isShockwaveExpanding() {
    return shockwave.pigHypeDelayed;
}

bool isShockwaveCoveringPig() {
    return shockwave.phase == SW_FILLED || shockwave.phase == SW_SAND_DISSIPATE;
}

bool isExplosionActive() {
    return shockwave.phase != SW_INACTIVE;
}

bool isTreeSuppressed() {
    return shockwave.phase != SW_INACTIVE || TimeMath::active(millis(), treeSuppressUntil);
}

bool getShuttleRect(int16_t& x, int16_t& y, int16_t& w, int16_t& h) {
    if (!shuttle.active) return false;
    x = birdSnap((int16_t)shuttle.x);
    y = birdSnap((int16_t)shuttle.yFloat);
    // size derived from current drawW/drawH/pixelGrid (set by updateShuttle)
    switch (shuttle.matPhase) {
        case SPHASE_DOCKED:
            return false;  // invisible
        case SPHASE_FIRST_LIGHT:
        case SPHASE_WAKE_FORMING:
            // small dot — use pixelGrid for both axes
            w = shuttle.drawW * shuttle.pixelGrid;
            h = shuttle.drawH * shuttle.pixelGrid;
            break;
        case SPHASE_HEATING:
        case SPHASE_FIREBALL:
        case SPHASE_DECELERATION:
        case SPHASE_COOLING:
            // growing object — include bow shock margin during fireball
            w = shuttle.drawW * shuttle.pixelGrid;
            h = shuttle.drawH * shuttle.pixelGrid;
            if (shuttle.bowShock.outerR > 0) {
                // expand rect to cover bow shock crescent
                w += shuttle.bowShock.outerR * BIRD_PX * 2;
                h += shuttle.bowShock.outerR * BIRD_PX * 2;
                x -= shuttle.bowShock.outerR * BIRD_PX;
                y -= shuttle.bowShock.outerR * BIRD_PX;
            }
            break;
        case SPHASE_MATERIALIZED:
        case SPHASE_FALLING:
        default:
            w = NARCISSUS_W * BIRD_PX;  // 24px
            h = NARCISSUS_H * BIRD_PX;  // 12px
            break;
    }
    return rectIntersectsFov(x, y, w, h);
}

}  // namespace Weather
