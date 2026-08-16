// Piglet ASCII avatar
#pragma once

#include "pig_face_timer.h"

#include <M5Unified.h>

enum class AvatarState {
    NEUTRAL,
    HAPPY,
    EXCITED,
    HUNTING,
    SLEEPY,
    SAD,
    ANGRY
};

enum class WaveMode : uint8_t {
    NONE,      // No waves (idle, cooldown, bored)
    INCOMING,  // Converging toward nose (scanning/sniffing)
    OUTGOING   // Radiating from nose (deauth/sending)
};

enum class TreePhase : uint8_t {
    HIDDEN = 0,   // Not visible
    GROWING,      // Trunk → canopy → fruits animation
    ALIVE,        // Fully grown, gentle sway + fruit bob
    COLLAPSING    // Reverse animation
};

// ==[ CINEMATIC OVERRIDE ]== supreme lock — Weather/Mood choreography owns pig
struct CinematicPose {
    bool active = false;          // cinematic owns the pig
    int16_t forceX = -1;         // -1 = don't override currentX
    int8_t  facingDir = 0;        // 0 = don't override, 1 = right, -1 = left
    int16_t shakeY = 0;          // Y offset override
    int16_t offsetX = 0;         // X jitter from position
    bool suppressBody = true;    // block tryStartBody()
    bool suppressIdle = true;    // block random walk/look behaviors
};

// ==[ BODY ANIM FSM ]== priority = enum order. higher preempts lower.
enum class BodyAnim : uint8_t {
    IDLE = 0,
    BUTT_FLEX,      // 1 — rear view twerk wiggle
    PAW_SCRATCH,    // 2 — X oscillation when bored
    PERK_UP,        // 3 — ears pop + bounce on new network
    CUTE_JUMP,      // 4 — celebration bounce
    FLINCH,         // 5 — duck + jitter on error
    SPIN,           // 6 — rapid direction flips (level-up)
    ATTACK_HOP,     // 7 — multi-hop pounce
    ATTACK_TREE     // 8 — charge tree + retreat
};

struct BodySlot {
    BodyAnim anim = BodyAnim::IDLE;
    uint32_t startTime = 0;
    uint8_t phase = 0;      // sub-phase (anim-specific meaning)

    bool active() const { return anim != BodyAnim::IDLE; }
    void clear() { anim = BodyAnim::IDLE; phase = 0; }
};

// ==[ WALK FSM ]== replaces windup booleans in hamlet.cpp
enum class WalkPhase : uint8_t {
    STOPPED = 0,
    SLIDE,          // sliding to walk position (0-450ms)
    FLIP,           // face direction change (450-500ms)
    WALKING         // grass parallax active
};

// ==[ UNIFIED PIG FACING ]== replaces facingRight bool + facingAway bool
enum class PigFacing : uint8_t { LEFT = 0, RIGHT = 1, REAR = 2 };

// ==[ FACE DETAIL SHAPES ]== expression components, no more ASCII glyph select
enum class EyeShape : uint8_t {
    OPEN_ROUND,    // 'o' — neutral
    HAPPY,         // '^' — content
    EXCITED,       // '@' — hyped
    HUNTING,       // '=' — focused
    SLEEPY,        // '-' — droopy
    SAD,           // 'T' — crying
    ANGRY,         // '#' — rage
    BLINK          // '-' — blink override
};

enum class EarShape : uint8_t {
    NEUTRAL,       // '?' — default
    HAPPY,         // '^' — perky
    EXCITED,       // '!' — alert
    HUNTING,       // '|' — pinned
    SLEEPY,        // 'v' — droopy
    SAD,           // '.' — flat
    ANGRY,         // '\/' — V-split
    TWITCH         // '^' — micro-anim override
};

enum class SnoutPhase : uint8_t {
    IDLE,          // 'oo' — default nostrils
    SNIFF_1,       // 'oO' — inhale
    SNIFF_2        // 'Oo' — exhale
};

// ==[ PIG EXPRESSION ]== composable face state, replaces per-frame ASCII switch
struct PigExpression {
    EyeShape eyes;
    EarShape ears;
    SnoutPhase snout;

    static PigExpression fromState(AvatarState state, bool blink, bool sniff,
                                   uint8_t sniffFrame, bool earTwitch);
};

// ==[ LIMB MODE ]== what the legs/hands are doing
enum class LimbMode : uint8_t {
    STANDING,         // idle U-legs (avatar.cpp idle screen)
    SITTING,          // soft-U sitting legs + U-hands (menu_pig stations)
    WALKING_IDLE,     // walking stubs (avatar.cpp grass walk)
    WALKING_MARCH,    // 8-frame animated march (menu_pig room walk)
    AIRBORNE,         // no legs (jump/spin)
    REAR_NUBS,        // rear-view chubby nubs below a free-standing body
    REAR_PLANTED      // rear hoof pads contained by the body's support row
};

// ==[ U-SHAPE ORIENTATION ]== for shared drawChubbyU primitive
enum class UOrientation : uint8_t {
    OPEN_LEFT,     // closed right, open left
    OPEN_RIGHT,    // closed left, open right
    OPEN_TOP,      // closed bottom, open top (downward-U hands)
    OPEN_BOTTOM    // closed top, open bottom
};

// ==[ BUMP LIGHTING ]== neon-driven highlight/shadow from room light sources
struct PigLight {
    int16_t x = 0, y = 0;   // screen-space position of light source
    uint16_t tint = 0;       // highlight tint color (0 = no light, skip bump)
};

// ==[ UNIFIED PIG POSE ]== single struct drives both idle screen and room pig renderers
struct PigRenderPose {
    int16_t x, y;              // body top-left position
    PigFacing facing;
    PigExpression expression;
    int16_t shakeY = 0;
    int16_t offsetX = 0;
    int16_t bellyBreathePx = 0;
    LimbMode limbMode = LimbMode::STANDING;
    uint8_t walkFrame = 0;
    char tailGlyph = 'z';
    bool tailOnLeft = false;   // which side the tail goes
    PigEyeLook eyeLook = PigEyeLook::NONE;
    uint16_t detailColor = 0;
    bool ramenEating = false;
    bool cupDrinking = false;
    int ingestHeadDip = 0;
    int scale = 2;             // shared PIG_PX cell size
    bool talking = false;      // mouth animates when speech bubble active
    PigLight light;            // room light source for bump shading (tint=0 → skip)
    bool blendRounding = false; // sample room pixels behind pig for edge rounding (avoids BG contour vs structures)
    bool noHeadwear = false;    // skip player headwear (NPC pigs)
};

// ==[ PIG RENDERER ]== unified pig body+limbs renderer, used by idle screen + rooms
namespace PigRenderer {
    // Draw filled body silhouette with face details (or featureless rear)
    void drawBody(M5Canvas& canvas, const PigRenderPose& pose, uint16_t fg, uint16_t bg);
    // Draw legs/hands based on LimbMode
    void drawLimbs(M5Canvas& canvas, const PigRenderPose& pose, uint16_t fg, uint16_t bg, uint32_t now);
}

class Avatar {
public:
    static void init();
    static void draw(M5Canvas& canvas);
    static void drawBodyOnly(M5Canvas& canvas, int x, int y,
                             uint16_t fg, uint16_t bg,
                             AvatarState state, bool blink, bool faceRight,
                             bool sniff, bool earTwitch = false,
                             PigEyeLook eyeLook = PigEyeLook::NONE,
                             uint16_t detailColor = 0,
                             char tailChar = 'z',
                             PigLight light = {},
                             bool ramenEating = false,
                             bool blendRounding = false,
                             uint8_t sniffFrameOverride = 0xFF);
    static void drawHairsAt(M5Canvas& canvas, int16_t startX, int16_t startY,
                            int16_t shakeY, bool faceRight,
                            AvatarState visualState, bool rearView);
    static uint16_t getHairAccentColor(uint16_t fallbackColor = 0);
    static bool usesFedora();
    static bool shouldRenderHypeHair();
    static void drawFedoraAt(M5Canvas& canvas, int16_t startX, int16_t startY,
                             bool faceRight, bool rearView,
                             uint16_t fill, uint16_t bg);
    static void drawConfiguredHeadwear(M5Canvas& canvas, int16_t startX, int16_t startY,
                                       bool faceRight, bool rearView,
                                       uint16_t fill, uint16_t bg);
    // Capture-only hooks for deterministic pig atlas export.
    static void setRenderTimeOverride(uint32_t now);
    static void clearRenderTimeOverride();
    static void resetCaptureState();
    static bool renderCaptureClip(M5Canvas& canvas, const char* clipId, uint32_t now);
    static void setState(AvatarState state);
    static AvatarState getState() { return currentState; }
    static bool isFacingRight();  // Get current facing direction
    static bool isTransitioning();  // True during walk transition (hide bubble)
    static int getCurrentX();  // Get current animated X position
    static void setCurrentX(int x);  // Set current X (capture renderer)
    static void getNosePosition(int16_t& x, int16_t& y);  // Nose coords (for bubble anchoring)

    // Phase 8: Intensity-based animation modifiers
    static void setMoodIntensity(int intensity);  // -100 to 100, affects blink/flip rates

    static void sniff();  // Trigger nose sniff animation (600ms animated cycle)
    static void wiggleEars();
    static void cuteJump();  // Trigger cute celebratory jump (higher than walk bounce)
    static void portalPull(int16_t targetX, uint32_t durationMs);  // Slide toward portal center
    static void cancelPortalPull();  // Stop portal pull (on mode change)

    // Event reaction animations
    static void perkUp();       // Ears pop up + quick upward bounce (new network)
    static void flinch();       // Duck down + horizontal jitter (error/heap critical)
    static void spin();         // Rapid direction flips (level-up celebration)
    static void pawScratch();   // Small X oscillation (bored)
    static void buttFlex();     // Rear view twerk wiggle (random idle)
    static bool facingAway;    // Rear view state (butt facing camera)
    static void triggerTailWiggle();  // Burst tail wag on celebrations
    // ==[ TAIL PHYSICS ]== springy coil, wobbles and bobs like a real pig tail
    struct TailState {
        float leanX = 0;       // horizontal wobble position
        float leanVelX = 0;    // horizontal wobble velocity
        float bobY = 0;        // vertical bounce position
        float bobVelY = 0;     // vertical bounce velocity
        float curlScale = 1.0f; // spiral tightness (stretches on motion)
    };
    static TailState tailPhys;

    // Curled pig tail with per-segment lighting + spring physics
    static void drawTailCurl(M5Canvas& canvas, int x, int y,
                             bool extRight, bool rear, uint16_t fg,
                             uint16_t bg = 0, PigLight light = {},
                             float leanX = 0, float droopY = 0);

    // Sparkle particles (achievements, level-up celebrations)
    static void triggerSparkles(uint8_t count = 6, int16_t startY = -1);

    // Attack shake (visual feedback for captures)
    static void setAttackShake(bool active, bool strong);
    static void setAttackShakeSmooth(bool active, float amplitude);

    // Attack hop (multi-hop pounce animation for captures)
    static void attackHop();

    // Tree attack (charge into tree, shake it, retreat)
    static void attackTree();

    // Thunder flash (invert colors for weather effect)
    static void setThunderFlash(bool active);

    // Hype gate: rainbow coloring locked until explosion (shuttle kill)
    static void unlockHype();

    // Wave ripple animation (radio activity feedback)
    // Burst-based: each call extends a 2700ms burst without resetting phase.
    // OUTGOING priority: active OUTGOING burst can't be overridden by INCOMING
    static void waveRipple(WaveMode mode, uint8_t intensity = 3);  // intensity: 1-5 rings
    static WaveMode getWaveMode() { return waveMode; }

    // Bird-wave collision check (called by Weather bird system)
    static bool checkBirdWaveCollision(int16_t bx, int16_t by);
    // Analytical wave occlusion (pig body + tree bounds)
    static bool isWaveOccluded(int16_t px, int16_t py);

    // Night sky star system (RTC-based)
    static bool isNightTime();           // check rtc for night hours, 20:00-06:00

    // Walk state machine (replaces scattered windup logic in hamlet.cpp)
    static void updateWalk(bool isWalkingNow, uint32_t now);

    // Fruit tree visualization (juicy channel indicator)
    static void showTree(uint8_t fruitCount);  // Triggers growth with N fruits
    static void hideTree();                     // Triggers collapse
    static bool isTreeVisible();               // True if not HIDDEN
    static void dropFruit();                   // Detach one fruit, start it falling

    // Hype rainbow color (plasma effects, mood > 70)
    static uint16_t getHypeColor(int16_t x, int16_t y);
    static bool isHypeVisualActive();

    // ==[ CINEMATIC OVERRIDE ]== Weather/Mood choreography layer
    static void setCinematicPose(const CinematicPose& pose);
    static void clearCinematic();

    // Grass/tree Y offset (shockwave ground collapse)
    static void setGrassYOffset(int16_t offset);

    // Grass animation control (direction: true=right, false=left)
    static void setGrassMoving(bool moving, bool directionRight = true);
    static bool isGrassMoving() { return grassMoving; }
    static bool isGrassDirectionRight() { return grassDirection; }
    static uint16_t getGrassSpeed() { return grassSpeed; }

    // ==[ SHARED LIMB PRIMITIVES ]== used by both idle screen and room pig
    // U-shaped limb: 3 sides of a rect, one side open. BG-outlined.
    static void drawChubbyU(M5Canvas& canvas, int16_t x, int16_t y,
                            int16_t w, int16_t h, UOrientation orient,
                            int16_t thickness, bool filled, uint16_t fg, uint16_t bg);
    // Small rounded nub (rear-view haunches/sitting nubs)
    static void drawLegNub(M5Canvas& canvas, int16_t x, int16_t y,
                           int16_t w, int16_t h, uint16_t fg, uint16_t bg);

private:
    // Demoted from public — internal-only callers
    static void blink();
    static bool isAttackHopping();
    static bool isCinematic();
    static void setFacingLeft();
    static void startWindupSlide(int targetX, bool faceRight = false);

    // Star system — extracted to pig_stars.cpp (PigStars namespace)
    static AvatarState currentState;
    static bool isSniffing;
    static int moodIntensity;  // Phase 8: -100 to 100

    // ==[ CINEMATIC OVERRIDE ]== supreme lock over body channel
    static CinematicPose cinematicPose;

    // ==[ BODY ANIM FSM ]== single slot replaces 7 bool+time pairs
    static BodySlot body;
    static constexpr uint16_t JUMP_DURATION_MS = 400;
    static constexpr int JUMP_HEIGHT = 5;
    static constexpr uint16_t ATTACK_HOP_MS = 250;
    static constexpr int16_t ATTACK_HOP_HEIGHT = 7;
    static constexpr uint16_t TREE_CHARGE_MS = 600;
    static constexpr uint16_t TREE_RETREAT_MS = 500;

    // Attack hop specifics (only used when body.anim == ATTACK_HOP || ATTACK_TREE)
    static uint8_t attackHopIndex;
    static uint8_t attackHopTotal;
    static int16_t attackHopOriginX;
    static int16_t attackHopTargets[5];

    // Walk transition animation
    static bool transitioning;
    static uint32_t transitionStartTime;
    static int transitionFromX;
    static int transitionToX;
    static bool transitionToFacingRight;
    static int currentX;  // Animated X position
    // Grass/tree Y offset (shockwave collapse)
    static int16_t grassYOffset;

    // Grass animation state
    static bool grassMoving;
    static bool grassDirection;  // true = grass scrolls right, false = scrolls left
    static bool pendingGrassStart;  // Wait for transition before starting grass
    static bool onRightSide;  // Track which side of screen pig is on
    static uint32_t lastGrassUpdate;
    static uint16_t grassSpeed;  // ms per shift

    // Grass blade system (triangle primitives)
    struct GrassBlade {
        uint8_t height;  // 6-20 px
        int8_t  lean;    // tip offset: -3 to +3
        uint8_t width;   // base half-width: 1-3
    };
    static constexpr uint8_t GRASS_BLADE_COUNT = 41;  // (320/8)+1 — full screen coverage
    static constexpr int16_t GRASS_STRIDE = 8;  // px spacing
    static GrassBlade grassBlades[GRASS_BLADE_COUNT];
    static int16_t grassOffset;  // smooth scroll pixel offset

    // Fruit tree state
    struct TreeFruit {
        int8_t  offsetX;    // X offset from canopy center (-20..+20)
        int8_t  offsetY;    // Y offset from canopy top (0..canopyH)
        uint8_t radius;     // 2-4px
        uint8_t bobPhase;   // Per-fruit bob phase (0-255)
    };

    struct TreeBranch {
        int8_t x1, y1;     // start relative to baseX, baseY
        int8_t x2, y2;     // end relative to baseX, baseY
        uint8_t thickness;  // 1-2px (tapers from parent)
    };

    struct TreeTrunk {
        int16_t baseX;         // ground position
        uint8_t trunkHeight;   // 67-81px
        uint8_t trunkWidth;    // half-width 2-3px
        int8_t  trunkLean;     // -3..+3
        uint8_t crownRadius;   // 10-16px canopy circle
    };

    static constexpr uint8_t MAX_BRANCHES = 18;
    static constexpr uint8_t MAX_TREE_FRUITS = 8;
    static constexpr uint16_t TREE_GROW_MS = 600;
    static constexpr uint16_t TREE_COLLAPSE_MS = 350;
    static constexpr uint16_t TREE_MIN_ALIVE_MS = 4000;  // minimum visible time

    static TreePhase treePhase;
    static float treeGrowth;
    static uint32_t treeAnimStart;
    static TreeTrunk treeTrunk;
    static TreeBranch treeBranches[MAX_BRANCHES];
    static uint8_t treeBranchCount;
    static TreeFruit treeFruits[MAX_TREE_FRUITS];
    static uint8_t treeFruitCount;
    static uint32_t treeSeed;
    static bool treePendingHide;
    static bool treePendingShow;
    static uint8_t treePendingFruits;
    static uint32_t treeAliveStart;
    static int32_t treeScrollOffset;  // cumulative grass scroll applied to tree X

    static void generateTree(uint8_t fruitCount);
    static void updateTree();
    static void drawTree(M5Canvas& canvas);

    static WaveMode waveMode;
    static uint32_t waveBurstStart;
    static uint32_t waveBurstEnd;
    static void drawWaveRipples(M5Canvas& canvas, bool faceRight, int startX, int startY);

    static void drawFrame(M5Canvas& canvas, bool blink = false, bool faceRight = true, bool sniff = false);
    static void updateAnimState(uint32_t now);
    static PigRenderPose buildIdlePose(uint32_t now, bool blink, bool faceRight, bool sniff);
    static void renderPigScene(M5Canvas& canvas, const PigRenderPose& pose, uint32_t now);
    static void drawGrassLayer(M5Canvas& canvas, uint32_t now, uint16_t color, int16_t baseY, int16_t scrollOffsetPx,
                               int16_t layerPx, uint8_t heightNum, uint8_t heightDen,
                               bool foregroundMask, bool drawGroundLine);
    static void drawGrass(M5Canvas& canvas);
    static void updateGrass();
    static int16_t getGrassOffsetLayer2();
    static int16_t getGrassOffsetLayer3();

    // Body anim duration/param constants
    static constexpr uint16_t PERK_UP_DURATION_MS = 200;
    static constexpr int PERK_UP_HEIGHT = 2;
    static constexpr uint16_t FLINCH_DURATION_MS = 300;
    static constexpr uint16_t SPIN_DURATION_MS = 600;
    static constexpr uint8_t SPIN_FLIPS = 4;
    static constexpr uint16_t PAW_SCRATCH_DURATION_MS = 800;
    static constexpr uint16_t BUTT_FLEX_DURATION_MS = 1200;

    // Priority gate for body channel
    static bool tryStartBody(BodyAnim anim);

    // Walk FSM state
    static WalkPhase walkPhase;
    static uint32_t walkPhaseStart;
    static bool wasWalking;

    // Tail wiggle burst animation (celebrations)
    static bool tailWiggleActive;
    static uint32_t tailWiggleStart;
    static constexpr uint16_t TAIL_WIGGLE_DURATION_MS = 800;

    // Sparkle particle system (achievements, level-up)
    struct SparkleParticle {
        int16_t x, y;
        int8_t vx, vy;
        uint8_t life;       // frames remaining (0 = inactive)
    };
    static constexpr uint8_t MAX_SPARKLES = 8;
    static SparkleParticle sparkles[MAX_SPARKLES];
    static void updateAndDrawSparkles(M5Canvas& canvas);

    // ==[ HAIR SYSTEM ]== (state in PSRAM — DRAM is full)
    struct HairState {
        float leanX;       // current lean offset (spring physics)
        float leanVelX;    // lean velocity
        float curlScale;   // curl tightness (emotion-reactive, 0-1.5)
        float droopY;      // vertical droop (positive = hang down)
    };
    static constexpr uint8_t HAIR_COUNT = 6;
    static HairState* hairs;  // PSRAM-allocated in init()
    static int16_t prevPigX;
    static int16_t prevShakeY;
    static void updateHairPhysics(int16_t pigX, int16_t shakeY,
                                  AvatarState visualState);

    static void drawHairs(M5Canvas& canvas, int16_t startX, int16_t startY,
                          int16_t shakeY, bool faceRight,
                          AvatarState visualState, bool rearView);
};
