// Piglet avatar — grid mask body, bump lighting, direct face rendering
// Walk FSM, body anim FSM, position ownership, cinematic, hype, pose building.
// Scenery subsystems extracted: pig_tree.cpp, pig_waves.cpp, pig_grass.cpp, pig_stars.cpp
// Shared state/helpers: pig_scene_common.h, pig_face_timer.h

#include "pig_scene_common.h"
#include "pig_face_timer.h"
#include "pancetta_body_mask.h"
#include "pig_stars.h"
#include "weather.h"
#include "mood.h"
#include "../audio/sfx.h"
#include "../hamlet.h"
#include "../core/config.h"
#include "../util/debug_log.h"
#include <M5Unified.h>
#include <time.h>
#include <string.h>
#include <esp_random.h>

// Static members
AvatarState Avatar::currentState = AvatarState::NEUTRAL;
int Avatar::moodIntensity = 0;
static PigFaceTimer faceTimer;

// ==[ CINEMATIC OVERRIDE ]== supreme lock over body channel
CinematicPose Avatar::cinematicPose;

// ==[ BODY ANIM FSM ]== single slot replaces boolean soup
BodySlot Avatar::body;
uint8_t Avatar::attackHopIndex = 0;
uint8_t Avatar::attackHopTotal = 0;
int16_t Avatar::attackHopOriginX = 0;
int16_t Avatar::attackHopTargets[5] = {0};

// Tree attack impact trigger (fires once when charge hop lands)
static bool treeImpactTriggered = false;

// Walk transition state
bool Avatar::transitioning = false;
uint32_t Avatar::transitionStartTime = 0;
int Avatar::transitionFromX = 2;
int Avatar::transitionToX = 2;
bool Avatar::transitionToFacingRight = true;
int Avatar::currentX = 2;

// ==[ SNIFF STATE ]== nose animation
struct SniffState {
    bool active = false;
    uint32_t startTime = 0;
    uint8_t frame = 0;
    static constexpr uint32_t DURATION_MS = 600;
};
static SniffState sniffAnim;
bool Avatar::isSniffing = false;  // public alias — kept in sync with sniffAnim.active

// Walk transition timing
static const uint32_t TRANSITION_DURATION_MS = 1200;  // 1.2s slow relaxed walk

// Rest cooldown — moved to pig_grass.cpp

// Sparkle particle pool
Avatar::SparkleParticle Avatar::sparkles[MAX_SPARKLES] = {};

// Walk FSM state
WalkPhase Avatar::walkPhase = WalkPhase::STOPPED;
uint32_t Avatar::walkPhaseStart = 0;
bool Avatar::wasWalking = false;

// Overlay animations (concurrent, not body channel)
bool Avatar::tailWiggleActive = false;
uint32_t Avatar::tailWiggleStart = 0;
static bool renderTimeOverrideActive = false;
static uint32_t renderTimeOverrideMs = 0;

// Hair system state (hairs allocated in PSRAM via init())
Avatar::HairState* Avatar::hairs = nullptr;
int16_t Avatar::prevPigX = 0;
int16_t Avatar::prevShakeY = 0;
Avatar::TailState Avatar::tailPhys = {};

// Grass/tree Y offset (shockwave ground collapse)
int16_t Avatar::grassYOffset = 0;

// Rear view state
bool Avatar::facingAway = false;

// ==[ ATTACK STATE ]== shake feedback + hop residuals
struct AttackState {
    bool shakeActive = false;
    float shakeAmplitude = 0.0f;
    uint32_t shakeRefreshTime = 0;
};
static AttackState attack;

// Thunder flash state (weather effect - invert colors) — extern in pig_scene_common.h
bool thunderFlashActive = false;

// ==[ PORTAL STATE ]== smooth slide toward portal center during collapse
struct PortalState {
    bool active = false;
    int16_t fromX = 0;
    int16_t toX = 0;
    uint32_t start = 0;
    uint32_t duration = 400;
};
static PortalState portal;

// ==[ HYPE STATE ]== defined in pig_scene_common.h
HypeState hype;

// Night sky star system — moved to pig_stars.cpp

// ==[ WAVE STATE ]== defined in pig_scene_common.h
WaveState wave;

// Wave ripple state (class members)
WaveMode Avatar::waveMode = WaveMode::NONE;
uint32_t Avatar::waveBurstStart = 0;
uint32_t Avatar::waveBurstEnd = 0;

// Fruit tree state
TreePhase Avatar::treePhase = TreePhase::HIDDEN;
float Avatar::treeGrowth = 0.0f;
uint32_t Avatar::treeAnimStart = 0;
Avatar::TreeTrunk Avatar::treeTrunk = {};
Avatar::TreeBranch Avatar::treeBranches[MAX_BRANCHES] = {};
uint8_t Avatar::treeBranchCount = 0;
Avatar::TreeFruit Avatar::treeFruits[MAX_TREE_FRUITS] = {};
uint8_t Avatar::treeFruitCount = 0;
uint32_t Avatar::treeSeed = 0;
bool Avatar::treePendingHide = false;
bool Avatar::treePendingShow = false;
uint8_t Avatar::treePendingFruits = 0;
uint32_t Avatar::treeAliveStart = 0;
int32_t Avatar::treeScrollOffset = 0;

// ==[ COLLISION STATE ]== defined in pig_scene_common.h
CollisionState collision;

// ==[ POSITION OWNERSHIP ]== defined in pig_scene_common.h
PositionControl posControl;

// ==[ WALK LOOK STATE ]== facing, flip, look, grass wander timers
// (facingRight, lastFlipTime, flipInterval, lastLookTime, lookInterval
//  are defined further below near init() — grouped here logically)

// Dropping fruit / splash — defined in pig_scene_common.h
DroppingFruit droppingFruits[MAX_DROPPING] = {{0}};
FruitSplash fruitSplashes[FRUIT_SPLASH_COUNT] = {{0}};
uint8_t fruitSplashIdx = 0;

// Scenery fat-pixel grid — extern in pig_scene_common.h
int16_t PX = 4;
// Pig-specific constants (avatar.cpp only)
static constexpr int16_t PIG_PX = 2;
static constexpr int16_t PIG_FAT_PX = 4;
static constexpr int16_t PIG_GRID_W = 18;
static constexpr int16_t PIG_GRID_H = 10;
static constexpr int16_t PIG_DRAW_TOP_INSET = 2;

// ==[ PIG GEOMETRY ALIASES ]== map common header names to legacy names used internally
static constexpr int16_t PIG_BODY_W = PIG_BODY_W_CONST;
static constexpr int16_t PIG_BODY_H = PIG_BODY_H_CONST;
static constexpr int16_t PIG_MIN_X = PIG_MIN_X_CONST;
static constexpr int16_t PIG_MAX_X = PIG_MAX_X_CONST;
static constexpr int16_t PIG_Y = PIG_Y_CONST;
static constexpr int16_t PIG_CENTER_X = PIG_CENTER_X_CONST;
static constexpr int16_t GRASS_BASE_Y_CONST = GRASS_BASE_Y;

// snapToPx, snapPx — defined in pig_scene_common.h

static inline uint32_t avatarRenderMillis() {
    return renderTimeOverrideActive ? renderTimeOverrideMs : millis();
}


// hash8 — moved to pig_scene_common.h
// Iterative edge reflection
// reflectAxis, fatLinePx, fatLine, getDrawColor, getBGColor — in pig_scene_common.h

// Rainbow color helpers (hamlet-only: HYP3 mood rainbow pig)
static uint16_t rgbTo565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static uint16_t rainbow565(uint16_t phase) {
    phase %= 1536;
    uint8_t seg = (uint8_t)(phase >> 8);
    uint8_t t = (uint8_t)(phase & 0xFF);
    uint8_t r = 0, g = 0, b = 0;
    switch (seg) {
        case 0: r = 255; g = t;   b = 0;   break;
        case 1: r = (uint8_t)(255 - t); g = 255; b = 0;   break;
        case 2: r = 0;   g = 255; b = t;   break;
        case 3: r = 0;   g = (uint8_t)(255 - t); b = 255; break;
        case 4: r = t;   g = 0;   b = 255; break;
        default: r = 255; g = 0;  b = (uint8_t)(255 - t); break;
    }
    return rgbTo565(r, g, b);
}

// ==[ TRIPPY NOISE ]== smooth 2D value noise for organic rainbow morphing
// Hash-based pseudo-noise — no tables, just integer mixing
static inline float noiseHash(int ix, int iy) {
    uint32_t n = (uint32_t)ix * 374761393u + (uint32_t)iy * 668265263u;
    n = (n ^ (n >> 13)) * 1274126177u;
    n = n ^ (n >> 16);
    return (float)(n & 0xFFFF) / 65535.0f;
}

static float smoothNoise2D(float x, float y) {
    int ix = (int)floorf(x);
    int iy = (int)floorf(y);
    float fx = x - (float)ix;
    float fy = y - (float)iy;
    // Smoothstep interpolation
    float sx = fx * fx * (3.0f - 2.0f * fx);
    float sy = fy * fy * (3.0f - 2.0f * fy);
    float n00 = noiseHash(ix, iy);
    float n10 = noiseHash(ix + 1, iy);
    float n01 = noiseHash(ix, iy + 1);
    float n11 = noiseHash(ix + 1, iy + 1);
    float nx0 = n00 + (n10 - n00) * sx;
    float nx1 = n01 + (n11 - n01) * sx;
    return nx0 + (nx1 - nx0) * sy;
}

// Multi-octave fractal noise for richer patterns
static float trippyNoise(float x, float y) {
    return smoothNoise2D(x, y) * 0.6f
         + smoothNoise2D(x * 2.1f + 5.3f, y * 2.1f + 8.7f) * 0.3f
         + smoothNoise2D(x * 4.3f + 13.1f, y * 4.3f + 17.9f) * 0.1f;
}

// Trippy rainbow: screen-position + time → smooth morphing hue (extern in pig_scene_common.h)
uint16_t trippyRainbow(int16_t screenX, int16_t screenY) {
    float t = (float)millis() / 3000.0f;  // slow drift
    // Scale coords into noise space — larger divisor = bigger color blobs
    float nx = (float)screenX / 40.0f + t * 0.7f;
    float ny = (float)screenY / 35.0f + t * 0.5f;
    float n = trippyNoise(nx, ny);
    uint16_t hue = (uint16_t)(n * 1536.0f) % 1536u;
    return rainbow565(hue);
}

uint16_t Avatar::getHypeColor(int16_t x, int16_t y) {
    return trippyRainbow(x, y);
}

bool shouldUseHypeRainbow() {
    if (Config::isElder()) return true;  // elder = permanent rainbow
    if (!hype.unlocked) return false;  // need explosion first
    // block during shockwave expansion — hype activates during dissipation
    if (Weather::isShockwaveExpanding()) return false;
    HamletMode mode = Hamlet::getMode();
    if (mode != HamletMode::IDLE && mode != HamletMode::HUNT) return false;
    int mood = Mood::getEffectiveMood();
    if (mood <= 70) {
        hype.unlocked = false;  // sub hyped — re-lock until next explosion
        return false;
    }
    return true;  // HYP3 tier + explosion gate passed
}

bool Avatar::isHypeVisualActive() {
    return shouldUseHypeRainbow();
}

// update hype fill progress — bottom-to-top smoothstep front, same dither as sand
static void updateHypeFill() {
    if (!hype.fillActive) return;
    uint32_t elapsed = millis() - hype.fillStart;
    if (elapsed >= HypeState::FILL_MS) {
        hype.fillActive = false;
        // all rows fully filled
        memset(hype.fillCount, HypeState::FILL_COLS, sizeof(hype.fillCount));
        return;
    }
    float t = (float)elapsed / (float)HypeState::FILL_MS;
    // smoothstep easing
    float front = t * t * (3.0f - 2.0f * t);
    // bottom-to-top: front travels from beyond row 20 to below row 0.
    // range spans HypeState::FILL_ROWS + 4 (gradient width) so row 0 fully fills before t=1
    float range = (float)(HypeState::FILL_ROWS + 4);
    float frontRow = (float)HypeState::FILL_ROWS - front * range;
    for (uint8_t row = 0; row < HypeState::FILL_ROWS; row++) {
        // invert: row 20 fills first (bottom), row 0 fills last (top)
        float rowDist = (float)row - frontRow;
        if (rowDist < 0.0f) {
            hype.fillCount[row] = 0;  // front hasn't reached this row yet
        } else {
            float fill = rowDist / 4.0f;  // 4 rows behind front = fully filled
            if (fill > 1.0f) fill = 1.0f;
            hype.fillCount[row] = (uint8_t)(fill * (float)HypeState::FILL_COLS);
        }
    }
}

// ==[ TAIL CURL ]== compact cartoon pig corkscrew. Flat 2D spiral, curls UP from butt.
// Tiny footprint (~6px), 4px root → 2px tip. Physics wobble the whole curl.
void Avatar::drawTailCurl(M5Canvas& canvas, int x, int y,
                          bool extRight, bool rear, uint16_t fg,
                          uint16_t bg, PigLight light,
                          float leanX, float droopY) {
    // Wiggle: phase vibration scaled by t so root stays pinned to body
    float wigglePhase = 0;
    if (tailWiggleActive) {
        uint32_t elapsed = avatarRenderMillis() - tailWiggleStart;
        if (elapsed < TAIL_WIGGLE_DURATION_MS) {
            float fade = 1.0f - (float)elapsed / (float)TAIL_WIGGLE_DURATION_MS;
            wigglePhase = fade * 3.0f * sinf((float)elapsed * 0.05f);
        }
    }

    // Per-segment lighting
    auto litColor = [&](int16_t ax, int16_t ay, int16_t bx, int16_t by) -> uint16_t {
        if (light.tint == 0) return fg;
        float sdx = (float)(bx - ax), sdy = (float)(by - ay);
        float slen = sqrtf(sdx * sdx + sdy * sdy);
        if (slen < 0.5f) return fg;
        float nx = -sdy / slen, ny = sdx / slen;
        float mx = (ax + bx) * 0.5f, my = (ay + by) * 0.5f;
        float ldx = light.x - mx, ldy = light.y - my;
        float llen = sqrtf(ldx * ldx + ldy * ldy);
        if (llen > 1.0f) { ldx /= llen; ldy /= llen; }
        float dot = nx * ldx + ny * ldy;
        if (dot > 0.1f)  return Display::screenBlend565(fg, light.tint, (uint8_t)(dot * 0.45f * 255.0f));
        if (dot < -0.1f) return Display::lerpColor565(fg, bg, (-dot) * 0.40f);
        return fg;
    };

    if (rear) {
        // End-on: tiny spiral on butt, 1 coil
        const int STEPS = 10;
        const float R_START = 3.0f, R_END = 0.8f;
        float cx = (float)x + 4.0f, cy = (float)y + 2.0f;
        int16_t prevSX = 0, prevSY = 0;
        for (int i = 0; i <= STEPS; i++) {
            float t = (float)i / (float)STEPS;
            float angle = t * 2.0f * M_PI + wigglePhase * t;
            float r = R_START + (R_END - R_START) * t;
            int16_t sx = snapPx((int16_t)(cx + r * cosf(angle) + 0.5f));
            int16_t sy = snapPx((int16_t)(cy + r * sinf(angle) + 0.5f));
            if (i > 0) {
                int16_t pxSz = (t < 0.4f) ? 4 : PIG_PX;
                fatLinePx(canvas, prevSX, prevSY, sx, sy,
                          litColor(prevSX, prevSY, sx, sy), pxSz);
            }
            prevSX = sx; prevSY = sy;
        }
        return;
    }

    // ==[ SIDE VIEW ]== compact Archimedean corkscrew, curls UP from butt.
    // Center is on the extension side + slightly above root. Root hides under
    // body edge (2px overlap). Spiral emerges curling away and up.
    const int STEPS = 10;
    float sign = extRight ? 1.0f : -1.0f;
    // CCW for extRight, CW for extLeft — first arc sweeps UP from root
    float dir  = extRight ? 1.0f : -1.0f;

    // Spiral center: to the extension side, slightly above root.
    float cOffX = sign * 3.0f + leanX * 0.12f;
    float cOffY = -1.5f + droopY * 0.12f;
    float cx = (float)x + cOffX;
    float cy = (float)y + cOffY;
    float startAngle = atan2f(-cOffY, -cOffX);
    float startR = sqrtf(cOffX * cOffX + cOffY * cOffY);

    const float LOOPS = 1.25f;

    int16_t prevSX = 0, prevSY = 0;
    for (int i = 0; i <= STEPS; i++) {
        float t = (float)i / (float)STEPS;
        float angle = startAngle + dir * t * LOOPS * 2.0f * M_PI + wigglePhase * t;
        float r = startR * (1.0f - t * 0.7f);  // shrink to 30% at tip
        int16_t sx = snapPx((int16_t)(cx + r * cosf(angle) + 0.5f));
        int16_t sy = snapPx((int16_t)(cy + r * sinf(angle) + 0.5f));
        if (i > 0) {
            int16_t pxSz = (t < 0.4f) ? 4 : PIG_PX;
            fatLinePx(canvas, prevSX, prevSY, sx, sy,
                      litColor(prevSX, prevSY, sx, sy), pxSz);
        }
        prevSX = sx; prevSY = sy;
    }
}

// ==[ PIG EXPRESSION FACTORY ]== one switch replaces duplicated state→glyph mapping
PigExpression PigExpression::fromState(AvatarState state, bool blink, bool sniff,
                                       uint8_t sniffFrame, bool earTwitch) {
    PigExpression expr;
    // Ears from state
    switch (state) {
        case AvatarState::HAPPY:    expr.ears = EarShape::HAPPY;    break;
        case AvatarState::EXCITED:  expr.ears = EarShape::EXCITED;  break;
        case AvatarState::HUNTING:  expr.ears = EarShape::HUNTING;  break;
        case AvatarState::SLEEPY:   expr.ears = EarShape::SLEEPY;   break;
        case AvatarState::SAD:      expr.ears = EarShape::SAD;      break;
        case AvatarState::ANGRY:    expr.ears = EarShape::ANGRY;    break;
        default:                    expr.ears = EarShape::NEUTRAL;   break;
    }
    if (earTwitch) expr.ears = EarShape::TWITCH;

    // Eyes from state (blink overrides)
    if (blink) {
        expr.eyes = EyeShape::BLINK;
    } else {
        switch (state) {
            case AvatarState::HAPPY:    expr.eyes = EyeShape::HAPPY;      break;
            case AvatarState::EXCITED:  expr.eyes = EyeShape::EXCITED;    break;
            case AvatarState::HUNTING:  expr.eyes = EyeShape::HUNTING;    break;
            case AvatarState::SLEEPY:   expr.eyes = EyeShape::SLEEPY;     break;
            case AvatarState::SAD:      expr.eyes = EyeShape::SAD;        break;
            case AvatarState::ANGRY:    expr.eyes = EyeShape::ANGRY;      break;
            default:                    expr.eyes = EyeShape::OPEN_ROUND; break;
        }
    }

    // Snout
    if (sniff) {
        switch (sniffFrame) {
            case 1:  expr.snout = SnoutPhase::SNIFF_1; break;
            case 2:  expr.snout = SnoutPhase::SNIFF_2; break;
            default: expr.snout = SnoutPhase::IDLE;    break;
        }
    } else {
        expr.snout = SnoutPhase::IDLE;
    }
    return expr;
}

// ==[ SHARED LIMB PRIMITIVES ]== unified U-shape + nub for all pig leg/hand rendering
void Avatar::drawChubbyU(M5Canvas& canvas, int16_t x, int16_t y,
                          int16_t w, int16_t h, UOrientation orient,
                          int16_t thickness, bool filled, uint16_t fg, uint16_t bg) {
    const int16_t t = thickness;
    switch (orient) {
        case UOrientation::OPEN_LEFT:
            canvas.fillRect(x, y, w, h, fg);
            if (!filled) canvas.fillRect(x, y + t, w - t, h - 2 * t, bg);
            canvas.fillRect(x, y - t, w, t, bg);
            canvas.fillRect(x, y + h, w, t, bg);
            canvas.fillRect(x + w, y, t, h, bg);
            break;
        case UOrientation::OPEN_RIGHT:
            canvas.fillRect(x, y, w, h, fg);
            if (!filled) canvas.fillRect(x + t, y + t, w - t, h - 2 * t, bg);
            canvas.fillRect(x, y - t, w, t, bg);
            canvas.fillRect(x, y + h, w, t, bg);
            canvas.fillRect(x - t, y, t, h, bg);
            break;
        case UOrientation::OPEN_TOP: {
            // U-shape with open side up (lowercase 'u')
            // Draw solid U: two arms + bottom bar
            canvas.fillRect(x, y, t, h, fg);           // left arm
            canvas.fillRect(x + w - t, y, t, h, fg);   // right arm
            canvas.fillRect(x, y + h - t, w, t, fg);   // bottom bar (full width, connects arms)
            // Full bg outline around U perimeter
            canvas.fillRect(x - t, y - t, t, h + t, bg);  // left outside (extends above arm)
            canvas.fillRect(x + w, y - t, t, h + t, bg);  // right outside (extends above arm)
            canvas.fillRect(x - t, y + h, w + 2 * t, t, bg);  // bottom (full width including outsides)
            // Inside cavity (hollow part of U)
            int16_t cavityW = w - 2 * t;
            int16_t cavityH = h - t;
            if (cavityW > 0 && cavityH > 0) {
                canvas.fillRect(x + t, y, cavityW, cavityH, filled ? fg : bg);
            }
            break;
        }
        case UOrientation::OPEN_BOTTOM:
            canvas.fillRect(x, y, w, h, fg);
            if (!filled) canvas.fillRect(x + t, y + t, w - 2 * t, h - t, bg);
            canvas.fillRect(x - t, y - t, t, h + t, bg);
            canvas.fillRect(x + w, y - t, t, h + t, bg);
            canvas.fillRect(x, y - t, w, t, bg);
            break;
    }
}

void Avatar::drawLegNub(M5Canvas& canvas, int16_t x, int16_t y,
                         int16_t w, int16_t h, uint16_t fg, uint16_t bg) {
    const int16_t p = 2;
    canvas.fillRect(x, y, w, h, fg);
    canvas.fillRect(x + p, y - p, w - 2 * p, p, bg);
    canvas.fillRect(x - p, y, p, h, bg);
    canvas.fillRect(x + w, y, p, h, bg);
    canvas.fillRect(x + p, y + h, w - 2 * p, p, bg);
}

// ==[ FACE PIXEL PATTERNS ]== 6×7 bitmasks matching default GFX font glyphs
static bool isClosedEyeShape(EyeShape shape) {
    return shape == EyeShape::SLEEPY || shape == EyeShape::BLINK;
}

static int colorLuma565(uint16_t c) {
    int r = (c >> 11) & 0x1F;
    int g = (c >> 5) & 0x3F;
    int b = c & 0x1F;
    return r * 3 + g * 4 + b * 2;
}

static uint16_t pickDarker565(uint16_t a, uint16_t b) {
    return (colorLuma565(a) <= colorLuma565(b)) ? a : b;
}

static uint16_t pickBrighter565(uint16_t a, uint16_t b) {
    return (colorLuma565(a) >= colorLuma565(b)) ? a : b;
}

static uint16_t pickEyeWhite565(uint16_t bodyFill, uint16_t detailColor, uint16_t bg) {
    // pick brighter of bg/detail, then push hard toward theme fg
    uint16_t fg = Display::getColorFG();
    uint16_t base = (colorLuma565(detailColor) >= colorLuma565(bg)) ? detailColor : bg;
    uint16_t eyeWhite = Display::lerpColor565(base, fg, 0.70f);
    // push two tones toward pure white
    eyeWhite = Display::lerpColor565(eyeWhite, 0xFFFF, 0.30f);
    // guarantee contrast against body
    int contrast = colorLuma565(eyeWhite) - colorLuma565(bodyFill);
    if (contrast < 0) contrast = -contrast;
    if (contrast < 20) {
        eyeWhite = fg;  // full fg fallback
    }
    return eyeWhite;
}

static uint16_t pickSnoutColor565(uint16_t bodyFill, uint16_t detailColor) {
    uint16_t fg = Display::getColorFG();
    uint16_t base = Display::lerpColor565(bodyFill, detailColor, 0.34f);
    uint16_t lift = pickBrighter565(bodyFill, detailColor);
    uint16_t snout = Display::screenBlend565(base, lift, (uint8_t)(0.18f * 255.0f));
    if (colorLuma565(snout) <= colorLuma565(bodyFill) + 6) {
        snout = Display::screenBlend565(base, fg, (uint8_t)(0.10f * 255.0f));
    }
    return snout;
}

static void pigFillSpan(bool mask[PIG_GRID_H][PIG_GRID_W], int row, int col0, int col1) {
    if (row < 0 || row >= PIG_GRID_H) return;
    if (col0 > col1) return;
    if (col0 < 0) col0 = 0;
    if (col1 >= PIG_GRID_W) col1 = PIG_GRID_W - 1;
    for (int col = col0; col <= col1; ++col) {
        mask[row][col] = true;
    }
}

static void pigSetCell(bool mask[PIG_GRID_H][PIG_GRID_W], int row, int col) {
    if (row < 0 || row >= PIG_GRID_H || col < 0 || col >= PIG_GRID_W) return;
    mask[row][col] = true;
}

static bool pigHasCell(const bool mask[PIG_GRID_H][PIG_GRID_W], int row, int col) {
    if (row < 0 || row >= PIG_GRID_H || col < 0 || col >= PIG_GRID_W) return false;
    return mask[row][col];
}

static void pigDrawCell(M5Canvas& canvas, int16_t x, int16_t y, int row, int col, uint16_t color) {
    canvas.fillRect(x + col * PIG_FAT_PX, y + row * PIG_FAT_PX,
                    PIG_FAT_PX, PIG_FAT_PX, color);
}

static void stampPigEarMask(bool mask[PIG_GRID_H][PIG_GRID_W], EarShape ears) {
    switch (ears) {
        case EarShape::SLEEPY:
        case EarShape::SAD:
            pigSetCell(mask, 0, 4);
            pigSetCell(mask, 0, 13);
            pigSetCell(mask, 1, 4);
            pigSetCell(mask, 1, 5);
            pigSetCell(mask, 1, 12);
            pigSetCell(mask, 1, 13);
            break;
        case EarShape::ANGRY:
            pigSetCell(mask, 0, 5);
            pigSetCell(mask, 0, 12);
            pigSetCell(mask, 1, 4);
            pigSetCell(mask, 1, 5);
            pigSetCell(mask, 1, 12);
            pigSetCell(mask, 1, 13);
            break;
        case EarShape::HUNTING:
            pigSetCell(mask, 0, 4);
            pigSetCell(mask, 0, 13);
            pigSetCell(mask, 1, 4);
            pigSetCell(mask, 1, 13);
            break;
        default:
            pigSetCell(mask, 0, 4);
            pigSetCell(mask, 0, 5);
            pigSetCell(mask, 0, 12);
            pigSetCell(mask, 0, 13);
            pigSetCell(mask, 1, 3);
            pigSetCell(mask, 1, 4);
            pigSetCell(mask, 1, 13);
            pigSetCell(mask, 1, 14);
            break;
    }
}


static void buildReferencePigMask(bool mask[PIG_GRID_H][PIG_GRID_W], EarShape ears) {
    static_assert(PIG_GRID_W == PancettaBodyMask::kCols &&
                      PIG_GRID_H == PancettaBodyMask::kRows,
                  "Detective renderer must use the shared Pancetta mask");

    for (int row = 0; row < PIG_GRID_H; ++row) {
        if (PancettaBodyMask::kRowLeft[row] >= 0) {
            pigFillSpan(mask, row,
                        PancettaBodyMask::kRowLeft[row],
                        PancettaBodyMask::kRowRight[row]);
        }
    }
    stampPigEarMask(mask, ears);
}

static void buildSidePigMask(bool mask[PIG_GRID_H][PIG_GRID_W], bool faceRight, EarShape ears) {
    (void)faceRight;
    buildReferencePigMask(mask, ears);
}

static void buildRearPigMask(bool mask[PIG_GRID_H][PIG_GRID_W], EarShape ears) {
    buildReferencePigMask(mask, ears);
}

static void drawPigMaskFill(M5Canvas& canvas, int16_t x, int16_t y,
                            const bool mask[PIG_GRID_H][PIG_GRID_W], uint16_t fill) {
    for (int row = 0; row < PIG_GRID_H; ++row) {
        for (int col = 0; col < PIG_GRID_W; ++col) {
            if (mask[row][col]) pigDrawCell(canvas, x, y, row, col, fill);
        }
    }
}

// ==[ BG SNAPSHOT ]== capture canvas behind pig body before fill, so rounding
// can reveal room art instead of flat BG (prevents square contour in rooms)
static constexpr int BG_SNAP_H = PIG_GRID_H * PIG_FAT_PX;
static constexpr int BG_SNAP_SIZE = PIG_BODY_W_CONST * BG_SNAP_H;
static uint16_t* bgSnap = nullptr; // PSRAM-allocated during Avatar::init()
static int16_t   bgSnapX, bgSnapY;
static bool      bgSnapValid = false;

static void captureBodyBg(M5Canvas& canvas, int16_t x, int16_t y) {
    // IDLE/HUNT body animations can briefly push the pig beyond an edge.
    // Never retain an older substrate or read outside the canvas in that pose;
    // flat background rounding is the safe fallback for the clipped frame.
    bgSnapValid = false;
    if (!bgSnap) return; // OOM at init — fall back to flat bg

    // direct buffer read — readPixel() unconditionally byte-swaps (swap565_t),
    // so we must match: swap each pixel to produce same format as readPixel
    uint16_t* buf = static_cast<uint16_t*>(canvas.getBuffer());
    const int stride = canvas.width();
    const int canvasH = canvas.height();
    if (!buf || x < 0 || y < 0 ||
        x + PIG_BODY_W > stride || y + BG_SNAP_H > canvasH) {
        return;
    }

    for (int py = 0; py < BG_SNAP_H; py++) {
        const uint16_t* src = &buf[(y + py) * stride + x];
        uint16_t* dst = &bgSnap[py * PIG_BODY_W];
        for (int px = 0; px < PIG_BODY_W; px++)
            dst[px] = (src[px] << 8) | (src[px] >> 8);
    }
    bgSnapX = x; bgSnapY = y;
    bgSnapValid = true;
}

// sample from snapshot — returns flat bg if out of bounds or no snapshot
static inline uint16_t snapColor(int16_t px, int16_t py, uint16_t bg) {
    if (!bgSnapValid) return bg;
    int lx = px - bgSnapX, ly = py - bgSnapY;
    if (lx < 0 || lx >= PIG_BODY_W || ly < 0 || ly >= BG_SNAP_H)
        return bg;
    return bgSnap[ly * PIG_BODY_W + lx];
}

// Corner rounding — 2×2 cuts at exposed corners (top/bottom/side edges)
// When bgSnapValid, samples room art behind pig instead of flat bg
static void drawPigMaskRound(M5Canvas& canvas, int16_t x, int16_t y,
                             const bool mask[PIG_GRID_H][PIG_GRID_W], uint16_t bg) {
    constexpr int R = 2;
    for (int row = 0; row < PIG_GRID_H; ++row) {
        for (int col = 0; col < PIG_GRID_W; ++col) {
            if (!mask[row][col]) continue;
            int cx = x + col * PIG_FAT_PX;
            int cy = y + row * PIG_FAT_PX;
            bool hasTop    = pigHasCell(mask, row - 1, col);
            bool hasBottom = pigHasCell(mask, row + 1, col);
            bool hasLeft   = pigHasCell(mask, row, col - 1);
            bool hasRight  = pigHasCell(mask, row, col + 1);
            // Top-left: exposed top + exposed left
            if (!hasTop && !hasLeft)
                canvas.fillRect(cx, cy, R, R, snapColor(cx, cy, bg));
            // Top-right: exposed top + exposed right
            if (!hasTop && !hasRight)
                canvas.fillRect(cx + PIG_FAT_PX - R, cy, R, R, snapColor(cx + PIG_FAT_PX - R, cy, bg));
            // Bottom-left: exposed bottom + exposed left
            if (!hasBottom && !hasLeft)
                canvas.fillRect(cx, cy + PIG_FAT_PX - R, R, R, snapColor(cx, cy + PIG_FAT_PX - R, bg));
            // Bottom-right: exposed bottom + exposed right
            if (!hasBottom && !hasRight)
                canvas.fillRect(cx + PIG_FAT_PX - R, cy + PIG_FAT_PX - R, R, R,
                    snapColor(cx + PIG_FAT_PX - R, cy + PIG_FAT_PX - R, bg));
        }
    }
}

// ==[ EDGE CURVES ]== 3-step graduated sub-cell curves at body corners
// standard rounding gives 2×2 at convex corners (step 1). this adds:
//   step 2: full-width edge strip on the end cell (connects to side edge)
//   step 3: 2×2 diagonal cut on the second cell (extends the arc)
// only triggers when ≥2 consecutive cells share the exposed edge (skips waist transitions)
static void drawPigEdgeCurves(M5Canvas& canvas, int16_t x, int16_t y,
                               const bool mask[PIG_GRID_H][PIG_GRID_W], uint16_t bg) {
    constexpr int P = PIG_FAT_PX;  // 4px per cell
    constexpr int H = 2;            // PIG_PX sub-cell unit

    for (int row = 0; row < PIG_GRID_H; ++row) {
        int left = -1, right = -1;
        for (int c = 0; c < PIG_GRID_W; c++) {
            if (mask[row][c]) { if (left < 0) left = c; right = c; }
        }
        if (left < 0 || right - left < 3) continue;
        int cy = y + row * P;

        // -- bottom edge --
        if (!pigHasCell(mask, row + 1, left) && !pigHasCell(mask, row, left - 1)) {
            bool deep = (left + 1 <= right) && mask[row][left+1]
                        && !pigHasCell(mask, row + 1, left + 1);
            if (deep) {
                int sx = x + left * P;
                canvas.fillRect(sx, cy + P - H, P, H, snapColor(sx, cy + P - H, bg));
                canvas.fillRect(sx, cy, H, H, snapColor(sx, cy, bg));
                int sx2 = x + (left+1) * P;
                canvas.fillRect(sx2, cy + P - H, H, H, snapColor(sx2, cy + P - H, bg));
            }
        }
        if (!pigHasCell(mask, row + 1, right) && !pigHasCell(mask, row, right + 1)) {
            bool deep = (right - 1 >= left) && mask[row][right-1]
                        && !pigHasCell(mask, row + 1, right - 1);
            if (deep) {
                int sx = x + right * P;
                canvas.fillRect(sx, cy + P - H, P, H, snapColor(sx, cy + P - H, bg));
                canvas.fillRect(sx + P - H, cy, H, H, snapColor(sx + P - H, cy, bg));
                int sx2 = x + (right-1) * P;
                canvas.fillRect(sx2 + P - H, cy + P - H, H, H, snapColor(sx2 + P - H, cy + P - H, bg));
            }
        }

        // -- top edge --
        if (!pigHasCell(mask, row - 1, left) && !pigHasCell(mask, row, left - 1)) {
            bool deep = (left + 1 <= right) && mask[row][left+1]
                        && !pigHasCell(mask, row - 1, left + 1);
            if (deep) {
                int sx = x + left * P;
                canvas.fillRect(sx, cy, P, H, snapColor(sx, cy, bg));
                canvas.fillRect(sx, cy + P - H, H, H, snapColor(sx, cy + P - H, bg));
                int sx2 = x + (left+1) * P;
                canvas.fillRect(sx2, cy, H, H, snapColor(sx2, cy, bg));
            }
        }
        if (!pigHasCell(mask, row - 1, right) && !pigHasCell(mask, row, right + 1)) {
            bool deep = (right - 1 >= left) && mask[row][right-1]
                        && !pigHasCell(mask, row - 1, right - 1);
            if (deep) {
                int sx = x + right * P;
                canvas.fillRect(sx, cy, P, H, snapColor(sx, cy, bg));
                canvas.fillRect(sx + P - H, cy + P - H, H, H, snapColor(sx + P - H, cy + P - H, bg));
                int sx2 = x + (right-1) * P;
                canvas.fillRect(sx2 + P - H, cy, H, H, snapColor(sx2 + P - H, cy, bg));
            }
        }
    }
}

static void drawDirectEye(M5Canvas& canvas, int eyeX, int eyeY, bool faceRight,
                          EyeShape shape, PigEyeLook eyeLook,
                          uint16_t eyeWhiteColor, uint16_t eyeDotColor,
                           uint16_t eyelidColor) {
    int cx = eyeX + 6;
    int cy = eyeY + 7;
    if (isClosedEyeShape(shape)) {
        int bx = ((cx - 5) / 2) * 2;
        int by = (cy / 2) * 2;
        canvas.fillRect(bx, by, 10, 2, eyeDotColor); // blink line
        return;
    }

    // Eye white fill — smaller cross
    canvas.fillRect(cx - 3, cy - 5, 6, 10, eyeWhiteColor);  // vertical core
    canvas.fillRect(cx - 5, cy - 3, 10, 6, eyeWhiteColor);   // horizontal core

    // Pupil 4×4 fat pixel, snapped to PIG_PX=2 grid. CENTER puts the dot
    // squarely in the eye white so Pancetta can meet the viewer's gaze.
    if (eyeLook == PigEyeLook::NONE) eyeLook = PigEyeLook::BACK_DOWN;
    const bool eyeCenter = (eyeLook == PigEyeLook::CENTER);
    const bool eyeUp = (eyeLook == PigEyeLook::BACK_UP || eyeLook == PigEyeLook::FRONT_UP);
    const bool eyeFront = (eyeLook == PigEyeLook::FRONT_UP || eyeLook == PigEyeLook::FRONT_DOWN);
    int dotX = eyeCenter ? (cx - 2)
        : cx + (faceRight ? (eyeFront ? 2 : -4) : (eyeFront ? -4 : 2));
    int dotY = eyeCenter ? cy : cy + (eyeUp ? -4 : 2);
    if (eyeUp) {
        // The lid occupies the top and outside 2px cells. An extreme corner
        // pupil gets split into two diagonal remnants when that cap is drawn;
        // tuck upward looks one cell inward so the full 4x4 pupil survives
        // below the lid in both mirrored faces.
        dotX = cx + (faceRight
            ? (eyeFront ? -1 : -3)
            : (eyeFront ? -3 : -1));
        dotY = cy - 3;
    }
    dotX = (dotX / 2) * 2;
    dotY = (dotY / 2) * 2;
    canvas.fillRect(dotX, dotY, 4, 4, eyeDotColor);

    // Direct eye contact and upward attention keep a shallow body-colour lid.
    // The stepped 10x4px cap narrows the eye without a 1px anti-alias trick;
    // it is a change in face volume, not a dark eyelash. The fully closed
    // branch above remains the only dark eye line.
    if (eyeCenter || eyeUp) {
        canvas.fillRect(cx - 3, cy - 5, 6, 2, eyelidColor);
        canvas.fillRect(cx - 5, cy - 3, 2, 2, eyelidColor);
        canvas.fillRect(cx + 3, cy - 3, 2, 2, eyelidColor);
    }
}

static void drawDirectSnout(M5Canvas& canvas, int snoutX, int snoutY,
                            uint16_t fg, uint16_t bg,
                            bool faceRight,
                            bool firstExpanded, bool secondExpanded) {
    // Solid FG fill — fat-pixel rects approximating circle, no fine 1px edges
    int cx = snoutX + 12;
    int cy = snoutY + 7;
    canvas.fillRect(cx - 6, cy - 8, 12, 16, fg);   // vertical core
    canvas.fillRect(cx - 8, cy - 6, 16, 12, fg);    // horizontal core

    // Nostrils: BG 4×4 fat pixel dots snapped to PIG_PX=2 grid
    // bias = ±1 shifts pair center toward snout tip (right-facing → right, left → left)
    int bias = faceRight ? 1 : -1;
    int n1x = ((cx - 5 + bias) / 2) * 2;
    int n2x = ((cx + 1 + bias) / 2) * 2;
    int ny  = (cy / 2) * 2;
    canvas.fillRect(n1x, ny, 4, 4, bg);
    canvas.fillRect(n2x, ny, 4, 4, bg);

    // Sniff: extend nostrils to 4×8 each
    if (firstExpanded) canvas.fillRect(n1x, ny, 4, 8, bg);
    if (secondExpanded) canvas.fillRect(n2x, ny, 4, 8, bg);
}

static void drawDirectFace(M5Canvas& canvas, const PigRenderPose& pose,
                           uint16_t bodyFill, uint16_t detailColor, uint16_t bg) {
    if (pose.facing == PigFacing::REAR) return;

    const bool faceRight = (pose.facing == PigFacing::RIGHT);
    const int drawY = pose.y + PIG_DRAW_TOP_INSET;
    // Ramen eating: face leans toward bowl (-4px in facing direction)
    const int faceDx = pose.ramenEating ? (faceRight ? -4 : 4) : 0;
    // Both eye anchors are odd on the 2px pig lattice. That makes the 10px
    // white, 4px pupil, and stepped lid begin on whole pig cells rather than
    // one pixel beside them. The one-pixel inward nudge also keeps the gaze
    // tucked into the snout silhouette in either mirrored pose.
    // Back eye (behind snout)
    const int eyeX = pose.x + (faceRight ? 29 : 31) + faceDx;
    const int eyeY = drawY + 12;
    // Front eye (other side of snout)
    const int eye2X = pose.x + (faceRight ? 53 : 7) + faceDx;
    const int snoutX = pose.x + (faceRight ? 38 : 10) + faceDx;
    const int snoutY = drawY + 10;
    const int mouthX = ((snoutX + 10) / 2) * 2;
    const int mouthY = ((drawY + 26) / 2) * 2;
    const int talkOpenH = (pose.talking && ((avatarRenderMillis() / 200) & 1)) ? 8 : 4;
    const uint16_t eyeWhiteColor = pickEyeWhite565(bodyFill, detailColor, bg);
    const uint16_t snoutColor = pickSnoutColor565(bodyFill, detailColor);
    const uint16_t eyeDotColor = pickDarker565(detailColor, bg);

    // Back eye
    drawDirectEye(canvas, eyeX, eyeY, faceRight, pose.expression.eyes, pose.eyeLook,
                  eyeWhiteColor, eyeDotColor, bodyFill);
    // Front eye — synced blink/look, same Y
    drawDirectEye(canvas, eye2X, eyeY, faceRight, pose.expression.eyes, pose.eyeLook,
                  eyeWhiteColor, eyeDotColor, bodyFill);

    // Sniff mirrored: tip-side nostril flares first regardless of facing
    bool firstExpanded = false;
    bool secondExpanded = false;
    if (faceRight) {
        if (pose.expression.snout == SnoutPhase::SNIFF_1) secondExpanded = true;
        if (pose.expression.snout == SnoutPhase::SNIFF_2) firstExpanded = true;
    } else {
        if (pose.expression.snout == SnoutPhase::SNIFF_1) firstExpanded = true;
        if (pose.expression.snout == SnoutPhase::SNIFF_2) secondExpanded = true;
    }
    drawDirectSnout(canvas, snoutX, snoutY, snoutColor, bg, faceRight, firstExpanded, secondExpanded);

    // Snout contour shadow — opposite side from light source
    if (pose.light.tint != 0) {
        int snoutCX = snoutX + 12;
        float ldx = pose.light.x - (float)snoutCX;
        uint16_t shadowC = Display::lerpColor565(snoutColor, bg, 0.30f);
        uint16_t highlightC = Display::screenBlend565(snoutColor, pose.light.tint, (uint8_t)(0.25f * 255.0f));
        if (ldx > 0) {
            canvas.fillRect(snoutCX - 8, snoutY + 1, 2, 14, shadowC);
            canvas.fillRect(snoutCX + 6, snoutY + 1, 2, 14, highlightC);
        } else {
            canvas.fillRect(snoutCX + 6, snoutY + 1, 2, 14, shadowC);
            canvas.fillRect(snoutCX - 8, snoutY + 1, 2, 14, highlightC);
        }
    }

    if (pose.ramenEating) {
        // slurping: mouth twice wider, shifted left one fat pixel
        canvas.fillRect(mouthX - 4, mouthY, 8, talkOpenH, eyeDotColor);
    } else {
        canvas.fillRect(mouthX, mouthY, 4, talkOpenH, eyeDotColor);
    }
}

uint16_t Avatar::getHairAccentColor(uint16_t fallbackColor) {
    uint16_t fallback = fallbackColor ? fallbackColor : Display::getColorFG();
    switch (Config::getPigHeadStyle()) {
        case Config::PIG_HEAD_ROSE: return Display::hsvToRgb565(336, 220, 255);
        case Config::PIG_HEAD_ICE:  return Display::hsvToRgb565(196, 210, 255);
        case Config::PIG_HEAD_GOLD: return Display::hsvToRgb565(44, 220, 255);
        default:                    return fallback;
    }
}

bool Avatar::usesFedora() {
    return Config::getPigHeadStyle() == Config::PIG_HEAD_FEDORA;
}

bool Avatar::shouldRenderHypeHair() {
    return Config::getPigHeadStyle() == Config::PIG_HEAD_HYPE && shouldUseHypeRainbow();
}

static uint16_t getFedoraFillColor(uint16_t bodyFill, uint16_t bg) {
    uint16_t fg = Display::getColorFG();
    uint16_t hatFill = Display::lerpColor565(bg, fg, 0.78f);
    int diff = colorLuma565(hatFill) - colorLuma565(bodyFill);
    if (diff < 0) diff = -diff;
    if (diff < 18) {
        if (colorLuma565(bodyFill) > colorLuma565(bg) + 10) {
            hatFill = Display::lerpColor565(bodyFill, bg, 0.35f);
        } else {
            hatFill = Display::screenBlend565(bodyFill, fg, 96);
        }
    }
    return hatFill;
}

void Avatar::drawFedoraAt(M5Canvas& canvas, int16_t startX, int16_t startY,
                          bool faceRight, bool rearView,
                          uint16_t fill, uint16_t bg) {
    uint16_t hatFill = getFedoraFillColor(fill, bg);
    const int cx = startX + 36;
    const int tilt = rearView ? 0 : (faceRight ? 3 : -3);
    const int crownW = rearView ? 32 : 28;
    const int crownX = snapToPx((cx - crownW / 2) + (rearView ? 0 : tilt / 2), PIG_PX_CONST);
    const int crownY = snapToPx(startY - 4, PIG_PX_CONST);
    const int brimW = rearView ? 48 : 44;
    const int brimX = snapToPx((cx - brimW / 2) + tilt, PIG_PX_CONST);
    const int brimY = snapToPx(crownY + 8, PIG_PX_CONST);

    canvas.fillRect(crownX, crownY, crownW, 8, hatFill);
    if (rearView) {
        canvas.fillRect(crownX + 6, crownY, crownW - 12, 2, bg);
    } else {
        const int dentX = snapToPx((cx + tilt / 2) - 3, PIG_PX_CONST);
        canvas.fillRect(dentX, crownY, 6, 2, bg);
    }
    canvas.fillRect(brimX, brimY, brimW, 2, hatFill);
    canvas.fillRect(crownX + 2, brimY - 2, crownW - 4, 2, bg);
}

void Avatar::drawConfiguredHeadwear(M5Canvas& canvas, int16_t startX, int16_t startY,
                                    bool faceRight, bool rearView,
                                    uint16_t fill, uint16_t bg) {
    switch (Config::getPigHeadStyle()) {
        case Config::PIG_HEAD_FEDORA:
            drawFedoraAt(canvas, startX, startY, faceRight, rearView, fill, bg);
            break;
        default:
            break;
    }
}

// ==[ BUMP LIGHTING ]== two-sphere neon-tinted highlight/shadow on pig body
// head sphere (rows 0-2) + body sphere (rows 3-9), per-cell normal → dot product
// draws one 2×2 accent block per cell in the quadrant nearest/farthest from light
static void drawPigBumpShade(M5Canvas& canvas, int16_t x, int16_t drawY,
                              const bool mask[PIG_GRID_H][PIG_GRID_W],
                              const PigRenderPose& pose, uint16_t bodyFill, uint16_t bg) {
    if (pose.light.tint == 0) return;  // no light source → zero cost

    // light direction: pig center → light source
    const float pcx = x + PIG_BODY_W / 2.0f;
    const float pcy = drawY + (PIG_GRID_H * PIG_FAT_PX) / 2.0f;
    float dx = pose.light.x - pcx;
    float dy = pose.light.y - pcy;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 1.0f) len = 1.0f;
    float lx = dx / len, ly = dy / len, lz = 0.35f;
    float inv = 1.0f / sqrtf(lx * lx + ly * ly + lz * lz);
    lx *= inv; ly *= inv; lz *= inv;

    // nose bump cells (snout protrusion)
    const bool faceRight = (pose.facing == PigFacing::RIGHT);
    const int noseCol = faceRight ? 13 : 3;

    // sphere params: head (rows 0-2), body (rows 3-9)
    constexpr float headCX = 9.0f, headCY = 1.5f, headRX = 5.0f, headRY = 2.0f;
    constexpr float bodyCX = 9.0f, bodyCY = 6.0f, bodyRX = 7.5f, bodyRY = 3.5f;

    for (int row = 0; row < PIG_GRID_H; ++row) {
        for (int col = 0; col < PIG_GRID_W; ++col) {
            if (!mask[row][col]) continue;

            // pick sphere
            float cx, cy, rx, ry;
            if (row <= 2) { cx = headCX; cy = headCY; rx = headRX; ry = headRY; }
            else          { cx = bodyCX; cy = bodyCY; rx = bodyRX; ry = bodyRY; }

            float nx = (col + 0.5f - cx) / rx;
            float ny = (row + 0.5f - cy) / ry;
            float r2 = nx * nx + ny * ny;
            float nz = (r2 < 1.0f) ? sqrtf(1.0f - r2) : 0.05f;

            // nose bump
            if ((row == 2 || row == 3) && (col == noseCol || col == noseCol + 1))
                nz += 0.3f;
            // eye bump
            int eyeCol = faceRight ? 12 : 5;
            if (row == 1 && col == eyeCol)
                nz += 0.15f;

            // normalize
            float mag = sqrtf(nx * nx + ny * ny + nz * nz);
            if (mag > 0.001f) { nx /= mag; ny /= mag; nz /= mag; }

            float dot = nx * lx + ny * ly + nz * lz;

            // per-cell shading: blend entire cell based on dot product
            if (dot > 0.10f) {
                // highlight — screen blend (additive light) so tint always brightens
                // lerp fails on low-contrast themes where tint ≈ bodyFill
                float t = (dot - 0.10f) / 0.70f;  // ramp 0.10..0.80 → 0..1
                if (t > 1.0f) t = 1.0f;
                uint16_t c = Display::screenBlend565(bodyFill, pose.light.tint,
                                                     (uint8_t)(t * 0.45f * 255.0f));
                canvas.fillRect(x + col * PIG_FAT_PX, drawY + row * PIG_FAT_PX,
                                PIG_FAT_PX, PIG_FAT_PX, c);
            } else if (dot < -0.05f) {
                // shadow — lerp body → bg, intensity from -dot
                float t = (-dot - 0.05f) / 0.55f;  // ramp -0.05..-0.60 → 0..1
                if (t > 1.0f) t = 1.0f;
                uint16_t c = Display::lerpColor565(bodyFill, bg, t * 0.40f);
                canvas.fillRect(x + col * PIG_FAT_PX, drawY + row * PIG_FAT_PX,
                                PIG_FAT_PX, PIG_FAT_PX, c);
            }
        }
    }
}

// ==[ PIG RENDERER ]== direct 4px cell body, same 72px footprint, no coarse pass
static void drawPigBodyRaw(M5Canvas& canvas, const PigRenderPose& pose, uint16_t fg, uint16_t bg) {
    bool mask[PIG_GRID_H][PIG_GRID_W] = {};
    const uint16_t detailColor = pose.detailColor ? pose.detailColor : fg;
    const int drawY = pose.y + PIG_DRAW_TOP_INSET;

    // Snapshot canvas behind pig body — rounding will sample these colors
    // instead of flat bg when blendRounding is set (room mode)
    if (pose.blendRounding)
        captureBodyBg(canvas, pose.x, drawY);

    if (pose.facing == PigFacing::REAR) {
        buildRearPigMask(mask, pose.expression.ears);
        drawPigMaskFill(canvas, pose.x, drawY, mask, fg);
        drawPigBumpShade(canvas, pose.x, drawY, mask, pose, fg, bg);
        drawPigMaskRound(canvas, pose.x, drawY, mask, bg);
        drawPigEdgeCurves(canvas, pose.x, drawY, mask, bg);
        // tail curl on butt, lit by room light
        Avatar::drawTailCurl(canvas, pose.x + 30, drawY + 26 + pose.bellyBreathePx,
                             false, true, fg, bg, pose.light);
        if (!pose.noHeadwear)
            Avatar::drawConfiguredHeadwear(canvas, pose.x, pose.y, false, true, fg, bg);
        bgSnapValid = false;
        return;
    }

    const bool faceRight = (pose.facing == PigFacing::RIGHT);
    buildSidePigMask(mask, faceRight, pose.expression.ears);
    // 2px overlap — spiral curls AWAY from root, root hides under body edge
    int tailX = pose.tailOnLeft ? (pose.x - 2) : (pose.x + PIG_BODY_W - 2);
    bool tailExtRight = !pose.tailOnLeft;
    Avatar::drawTailCurl(canvas, tailX, drawY + 28 + pose.bellyBreathePx,
                         tailExtRight, false, fg, bg, pose.light,
                         Avatar::tailPhys.leanX, Avatar::tailPhys.bobY);
    drawPigMaskFill(canvas, pose.x, drawY, mask, fg);
    drawPigBumpShade(canvas, pose.x, drawY, mask, pose, fg, bg);
    drawPigMaskRound(canvas, pose.x, drawY, mask, bg);
    drawPigEdgeCurves(canvas, pose.x, drawY, mask, bg);
    drawDirectFace(canvas, pose, fg, detailColor, bg);
    if (!pose.noHeadwear)
        Avatar::drawConfiguredHeadwear(canvas, pose.x, pose.y, faceRight, false, fg, bg);
    bgSnapValid = false;
}

static void drawPigLimbsRaw(M5Canvas& canvas, const PigRenderPose& pose,
                            uint16_t fg, uint16_t bg, uint32_t now) {
    (void)now;
    const int t = 2;
    const int bodyH = PIG_BODY_H;
    const bool fr = (pose.facing == PigFacing::RIGHT);
    int legBaseY = pose.y + bodyH + pose.bellyBreathePx;

    switch (pose.limbMode) {
        case LimbMode::AIRBORNE:
            break;

        case LimbMode::REAR_NUBS:
            Avatar::drawLegNub(canvas, pose.x + 14, legBaseY, 8, 8, fg, bg);
            Avatar::drawLegNub(canvas, pose.x + 48, legBaseY, 8, 8, fg, bg);
            break;

        case LimbMode::REAR_PLANTED: {
            static_assert(PIG_BODY_H ==
                              PIG_DRAW_TOP_INSET + PIG_GRID_H * PIG_FAT_PX,
                          "rear hoof pads must occupy the body's final row");
            const int hoofY = legBaseY - PIG_FAT_PX;
            const uint16_t hoof = Display::lerpColor565(fg, bg, 0.24f);
            const uint16_t cleft = Display::lerpColor565(hoof, bg, 0.46f);
            auto drawRearHoof = [&](int lx) {
                // Grounded rear stations already place the body on the floor.
                // Keep the hoof plane inside its final 4px row so the two
                // cloven pads read without growing legs through the support.
                canvas.fillRect(lx, hoofY, 8, PIG_FAT_PX, hoof);
                canvas.fillRect(lx + 3, hoofY + PIG_PX,
                                PIG_PX, PIG_PX, cleft);
            };
            drawRearHoof(pose.x + 14);
            drawRearHoof(pose.x + 48);
            break;
        }

        case LimbMode::STANDING: {
            auto drawStandLeg = [&](int lx) {
                canvas.fillRect(lx, legBaseY, 8, 10, fg);
                canvas.fillRect(lx - t, legBaseY, t, 10, bg);
                canvas.fillRect(lx + 8, legBaseY, t, 10, bg);
                canvas.fillRect(lx, legBaseY + 10, 8, t, bg);
            };
            drawStandLeg(pose.x + 14);
            drawStandLeg(pose.x + 48);
            break;
        }

        case LimbMode::WALKING_IDLE: {
            // 8-frame march with sitting-style limbs (filled U-shape with contour)
            static const int8_t backDx[8]  = {0, -4, -4, 0, 0, 4, 4, 0};
            static const int8_t frontDx[8] = {0, 4, 4, 0, 0, -4, -4, 0};
            static const int8_t backDy[8]  = {0, 0, -4, -4, 0, 0, -4, -4};
            static const int8_t frontDy[8] = {-4, -4, 0, 0, -4, -4, 0, 0};
            int frame = (int)(pose.walkFrame % 8);
            int baseBackX = pose.x + 14;
            int baseFrontX = pose.x + 48;
            int bDx = fr ? backDx[frame] : -backDx[frame];
            int fDx = fr ? frontDx[frame] : -frontDx[frame];
            
            // Legs: sitting-style filled block, full-width hoof continuation
            auto drawWalkLeg = [&](int lx, int ly) {
                canvas.fillRect(lx, ly, 8, 10, fg);
                canvas.fillRect(lx - t, ly, t, 10, bg);
                canvas.fillRect(lx + 8, ly, t, 10, bg);
                canvas.fillRect(lx, ly + 10, 8, t, bg);
            };
            drawWalkLeg(baseBackX + bDx, legBaseY + backDy[frame]);
            drawWalkLeg(baseFrontX + fDx, legBaseY + frontDy[frame]);
            break;
        }

        case LimbMode::SITTING: {
            auto drawSittingLeg = [&](int lx) {
                canvas.fillRect(lx, legBaseY, 8, 10, fg);
                canvas.fillRect(lx - t, legBaseY, t, 10, bg);
                canvas.fillRect(lx + 8, legBaseY, t, 10, bg);
                canvas.fillRect(lx, legBaseY + 10, 8, t, bg);
            };
            int backLegX  = fr ? (pose.x + 14) : (pose.x + 48);
            int frontLegX = fr ? (pose.x + 48) : (pose.x + 14);
            drawSittingLeg(backLegX);
            drawSittingLeg(frontLegX);

            if (!pose.ramenEating) {
                int pawY = pose.y + 32;
                int backPawX  = fr ? (pose.x + 20) : (pose.x + 42);
                int frontPawX = fr ? (pose.x + 42) : (pose.x + 20);
                if (pose.cupDrinking) {
                    if (fr) Avatar::drawChubbyU(canvas, backPawX, pawY, 8, 4, UOrientation::OPEN_BOTTOM, t, false, fg, bg);
                    else Avatar::drawChubbyU(canvas, frontPawX, pawY, 8, 4, UOrientation::OPEN_BOTTOM, t, false, fg, bg);
                } else {
                    Avatar::drawChubbyU(canvas, backPawX, pawY, 8, 4,
                                        UOrientation::OPEN_BOTTOM, t, false, fg, bg);
                    Avatar::drawChubbyU(canvas, frontPawX, pawY, 8, 4,
                                        UOrientation::OPEN_BOTTOM, t, false, fg, bg);
                }
            }
            break;
        }

        case LimbMode::WALKING_MARCH: {
            static const int8_t backDx[8]  = {0, -4, -4, 0, 0, 4, 4, 0};
            static const int8_t frontDx[8] = {0, 4, 4, 0, 0, -4, -4, 0};
            static const int8_t backDy[8]  = {0, 0, -4, -4, 0, 0, -4, -4};
            static const int8_t frontDy[8] = {-4, -4, 0, 0, -4, -4, 0, 0};
            int frame = (int)(pose.walkFrame % 8);
            int baseBackX = pose.x + 14;
            int baseFrontX = pose.x + 48;
            int bDx = fr ? backDx[frame] : -backDx[frame];
            int fDx = fr ? frontDx[frame] : -frontDx[frame];
            // Legs: sitting leg form, full-width hoof continuation
            auto drawWalkLeg = [&](int lx, int ly) {
                canvas.fillRect(lx, ly, 8, 10, fg);
            };
            drawWalkLeg(baseBackX + bDx, legBaseY + backDy[frame]);
            drawWalkLeg(baseFrontX + fDx, legBaseY + frontDy[frame]);
            break;
        }
    }
}

void PigRenderer::drawBody(M5Canvas& canvas, const PigRenderPose& pose, uint16_t fg, uint16_t bg) {
    drawPigBodyRaw(canvas, pose, fg, bg);
}

void PigRenderer::drawLimbs(M5Canvas& canvas, const PigRenderPose& pose,
                            uint16_t fg, uint16_t bg, uint32_t now) {
    drawPigLimbsRaw(canvas, pose, fg, bg, now);
}

static void drawPigFull(M5Canvas& canvas, const PigRenderPose& pose,
                        uint16_t fg, uint16_t bg, uint32_t now) {
    drawPigBodyRaw(canvas, pose, fg, bg);

    if (pose.bellyBreathePx > 0) {
        // Waist bridge: fills gap at face-belly junction (startY+28), matching ASCII
        const int bw = 38;
        const int bx = pose.x + (PIG_BODY_W - bw) / 2;
        const int by = pose.y + 28;
        canvas.fillRect(bx, by, bw, pose.bellyBreathePx, fg);
    }

    drawPigLimbsRaw(canvas, pose, fg, bg, now);
}

// Grass animation state (parallax layers moved to pig_grass.cpp)
bool Avatar::grassMoving = false;
bool Avatar::grassDirection = true;
bool Avatar::pendingGrassStart = false;
uint32_t Avatar::lastGrassUpdate = 0;
uint16_t Avatar::grassSpeed = 80;
Avatar::GrassBlade Avatar::grassBlades[GRASS_BLADE_COUNT] = {{0}};
int16_t Avatar::grassOffset = 0;

// Trail particle system — defined in pig_scene_common.h
TrailParticle trailParticles[TRAIL_COUNT] = {{0}};
uint32_t lastTrailSpawn = 0;
uint32_t lastTrailUpdate = 0;
int trailSpawnIdx = 0;

// ==[ WALK LOOK STATE ]== defined in pig_scene_common.h
WalkLookState walkLook;
bool Avatar::onRightSide = false;

void Avatar::init() {
    currentState = AvatarState::NEUTRAL;
    isSniffing = false;
    hype.unlocked = false;
    posControl.owner = PosOwner::IDLE;
    faceTimer.init(millis(), 4000, 8000, 8000, 15001);
    facingAway = false;

    // Init direction — start at LEFT or RIGHT edge
    bool startRight = random(0, 2) == 0;
    onRightSide = startRight;
    currentX = startRight ? PIG_MAX_X : PIG_MIN_X;
    walkLook.facingRight = !startRight;
    walkLook.lastFlipTime = millis();
    walkLook.flipInterval = random(25000, 50000);
    walkLook.lastLookTime = millis();
    walkLook.lookInterval = random(3000, 8000);

    // Init grass blade system
    grassMoving = false;
    grassDirection = true;
    pendingGrassStart = false;
    grassSpeed = 80;
    lastGrassUpdate = millis();
    grassOffset = 0;
    for (int i = 0; i < GRASS_BLADE_COUNT; i++) {
        grassBlades[i].height = random(6, 20);
        grassBlades[i].lean = random(-3, 4);
        grassBlades[i].width = random(1, 4);
    }

    // Init tree state
    treePhase = TreePhase::HIDDEN;
    treeGrowth = 0.0f;
    treeBranchCount = 0;
    treeFruitCount = 0;
    treePendingHide = false;
    treePendingShow = false;
    treePendingFruits = 0;
    treeAliveStart = 0;
    treeScrollOffset = 0;

    // Init dropping fruits and splash particles
    for (uint8_t i = 0; i < MAX_DROPPING; i++) droppingFruits[i].active = false;
    for (uint8_t i = 0; i < FRUIT_SPLASH_COUNT; i++) fruitSplashes[i].active = false;
    fruitSplashIdx = 0;

    // Init star system (now in pig_stars.cpp)
    PigStars::init();

    // Allocate the contour substrate outside the render path. Rooms already
    // use it, and hunt now uses it too so curved cells reveal the live street
    // instead of painting a flat dark halo over grass and pavement.
    if (!bgSnap) {
        bgSnap = static_cast<uint16_t*>(
            heap_caps_malloc(BG_SNAP_SIZE * sizeof(uint16_t), MALLOC_CAP_SPIRAM));
    }
    bgSnapValid = false;

    // Init hair system — PSRAM, DRAM is maxed
    if (!hairs) {
        hairs = (HairState*)heap_caps_malloc(
            sizeof(HairState) * HAIR_COUNT, MALLOC_CAP_SPIRAM);
        if (!hairs) {
            // PSRAM alloc failed — fallback to regular heap
            hairs = (HairState*)malloc(sizeof(HairState) * HAIR_COUNT);
            HAMLET_LOGF("HAIR: PSRAM fail, malloc fallback=%p\n", hairs);
        }
    }
    if (hairs) {
        for (uint8_t i = 0; i < HAIR_COUNT; i++) {
            hairs[i] = { 0.0f, 0.0f, 1.0f, 0.0f };
        }
    }
    prevPigX = currentX;
    prevShakeY = 0;
    tailPhys = {};
    wave.ringsInitialized = false;
}

void Avatar::setState(AvatarState state) {
    currentState = state;
}

void Avatar::setMoodIntensity(int intensity) {
    moodIntensity = constrain(intensity, -100, 100);
}

bool Avatar::isFacingRight() {
    return walkLook.facingRight;
}

bool Avatar::isTransitioning() {
    return transitioning || body.anim == BodyAnim::ATTACK_HOP || body.anim == BodyAnim::ATTACK_TREE;
}

int Avatar::getCurrentX() {
    return currentX;
}

void Avatar::setCurrentX(int x) {
    currentX = x;
}

void Avatar::getNosePosition(int16_t& x, int16_t& y) {
    int16_t startX = currentX;
    int16_t startY = PIG_Y + prevShakeY;  // prevShakeY updated every frame in draw()
    if (facingAway) {
        // Body center when facing away
        x = startX + NOSE_AWAY_X;
        y = startY + NOSE_Y;
        return;
    }
    if (isFacingRight()) {
        x = startX + NOSE_RIGHT_X;
    } else {
        x = startX + NOSE_LEFT_X;
    }
    y = startY + NOSE_Y;
}

void Avatar::blink() {
    faceTimer.blinking = true;
    faceTimer.blinkStart = millis();
}

void Avatar::wiggleEars() {
    // trigger visible ear twitch (was toggling dead earsUp flag)
    if (!faceTimer.earTwitching && !transitioning && !body.active()) {
        faceTimer.earTwitching = true;
        faceTimer.earTwitchStart = millis();
    }
}

void Avatar::sniff() {
    if (!isSniffing) {
        sniffAnim.frame = 0;
        SFX::play(SFX::SNIFF);
    }
    isSniffing = true;
    sniffAnim.startTime = millis();
}

// ==[ BODY ANIM FSM ]== priority gate — higher enum preempts lower
bool Avatar::tryStartBody(BodyAnim anim) {
    if (cinematicPose.active && cinematicPose.suppressBody) return false;
    if (static_cast<uint8_t>(anim) <= static_cast<uint8_t>(body.anim))
        return false;
    body.anim = anim;
    body.startTime = millis();
    body.phase = 0;
    return true;
}

void Avatar::cuteJump() {
    if (!tryStartBody(BodyAnim::CUTE_JUMP)) return;
    SFX::play(SFX::CUTE_JUMP);
}

void Avatar::portalPull(int16_t targetX, uint32_t durationMs) {
    portal.active = true;
    portal.fromX = currentX;
    portal.toX = targetX;
    portal.start = millis();
    portal.duration = durationMs;
    walkLook.facingRight = (targetX > currentX);
    posControl.claim(PosOwner::PORTAL_PULL);
}

void Avatar::cancelPortalPull() {
    portal.active = false;
    posControl.release(PosOwner::PORTAL_PULL);
}

void Avatar::attackHop() {
    if (!tryStartBody(BodyAnim::ATTACK_HOP)) return;
    posControl.claim(PosOwner::ATTACK_HOP);
    attackHopIndex = 0;
    attackHopOriginX = currentX;
    attackHopTotal = random(3, 6);

    int16_t prevX = currentX;
    for (uint8_t i = 0; i < attackHopTotal; i++) {
        if (i == attackHopTotal - 1) {
            attackHopTargets[i] = attackHopOriginX;
        } else {
            int16_t offset = random(25, 56);
            if (random(0, 2) == 0) offset = -offset;
            int16_t target = prevX + offset;
            if (target < 2) target = 2;
            if (target > PIG_MAX_X) target = PIG_MAX_X;
            attackHopTargets[i] = target;
        }
        prevX = attackHopTargets[i];
    }
}

void Avatar::attackTree() {
    if (treePhase != TreePhase::ALIVE) return;
    if (!tryStartBody(BodyAnim::ATTACK_TREE)) return;
    posControl.claim(PosOwner::ATTACK_HOP);

    // tree screen X
    int16_t tbx = treeTrunk.baseX + treeScrollOffset;
    while (tbx > SCREEN_WIDTH + 20) tbx -= (SCREEN_WIDTH + 80);
    while (tbx < -80) tbx += (SCREEN_WIDTH + 80);

    treeImpactTriggered = false;
    attackHopIndex = 0;
    attackHopOriginX = currentX;
    attackHopTotal = 2;  // charge + retreat

    // charge target: offset so pig body overlaps tree trunk
    int16_t treeTarget = constrain((int16_t)(tbx - 30), (int16_t)PIG_MIN_X, (int16_t)PIG_MAX_X);
    attackHopTargets[0] = treeTarget;
    attackHopTargets[1] = attackHopOriginX;

    // dust burst on launch
    for (int b = 0; b < 4; b++) {
        TrailParticle& p = trailParticles[trailSpawnIdx];
        trailSpawnIdx = (trailSpawnIdx + 1) % TRAIL_COUNT;
        p.x = (float)(currentX + PIG_BODY_W * 5 / 9 + random(-15, 16));
        p.y = (float)(GRASS_BASE_Y_CONST - 10 + random(0, 8));
        p.vx = (float)(random(-20, 21)) / 10.0f;
        p.vy = -(0.5f + (float)random(0, 10) / 10.0f);
        p.startX = p.x;
        p.maxDist = 20.0f + (float)random(0, 21);
        p.baseSize = random(1, 3);
        p.active = true;
    }
}

bool Avatar::isAttackHopping() {
    return body.anim == BodyAnim::ATTACK_HOP || body.anim == BodyAnim::ATTACK_TREE;
}

void Avatar::perkUp() {
    if (!tryStartBody(BodyAnim::PERK_UP)) return;
}

void Avatar::flinch() {
    if (!tryStartBody(BodyAnim::FLINCH)) return;
}

void Avatar::spin() {
    if (!tryStartBody(BodyAnim::SPIN)) return;
}

void Avatar::pawScratch() {
    if (transitioning) return;  // no scratch during walk transition
    if (!tryStartBody(BodyAnim::PAW_SCRATCH)) return;
}

void Avatar::buttFlex() {
    if (transitioning) return;
    if (!tryStartBody(BodyAnim::BUTT_FLEX)) return;
    // wiggle-only — no longer sets facingAway (rear view is independent)
}

void Avatar::triggerTailWiggle() {
    tailWiggleActive = true;
    tailWiggleStart = millis();
}

void Avatar::triggerSparkles(uint8_t count, int16_t startY) {
    if (startY < 0) startY = PIG_Y;
    int cx = currentX + PIG_BODY_W * 5 / 9;
    int cy = startY - PIG_PX * 9;  // sparkle origin above pig
    int spread = PIG_PX * 5;
    for (uint8_t i = 0; i < MAX_SPARKLES && count > 0; i++) {
        if (sparkles[i].life == 0) {
            sparkles[i].x = cx + random(-spread, spread + 1);
            sparkles[i].y = cy + random(-spread, spread + 1);
            sparkles[i].vx = random(-3, 4);
            sparkles[i].vy = random(-4, 1);
            sparkles[i].life = random(10, 18);
            count--;
        }
    }
}

void Avatar::updateAndDrawSparkles(M5Canvas& canvas) {
    uint16_t fg = Display::getColorFG();
    for (uint8_t i = 0; i < MAX_SPARKLES; i++) {
        if (sparkles[i].life == 0) continue;
        sparkles[i].x += sparkles[i].vx;
        sparkles[i].y += sparkles[i].vy;
        sparkles[i].life--;
        if (sparkles[i].life > 6) {
            canvas.fillRect(snapPx(sparkles[i].x), snapPx(sparkles[i].y), PX, PX, fg);
        } else if (sparkles[i].life & 1) {
            // temporal dither: flicker-fade on dying sparkles (2-color = no alpha)
            canvas.fillRect(snapPx(sparkles[i].x), snapPx(sparkles[i].y), PX, PX, fg);
        }
    }
}

// ==[ HAIR SYSTEM ]==

// Emotion params indexed by (int)AvatarState — fixed-point x100 to save DRAM
struct HairEmotionParams {
    int8_t targetCurlScale;  // x10: 10=1.0, 14=1.4
    int8_t targetDroopY;     // raw pixels
    int8_t spreadMul;        // x10: 10=1.0
    int8_t swaySpeed;        // x10: 10=1.0
    int8_t swayAmp;          // x10: 10=1.0
    int8_t stiffness;        // x100: 15=0.15
};

static const PROGMEM HairEmotionParams HAIR_EMOTIONS[] = {
    // NEUTRAL:  gentle default curl
    { 10,  0, 10, 10, 10, 15 },
    // HAPPY:    perky, bouncy
    { 12, -2, 10, 12, 11, 20 },
    // EXCITED:  very perky, rapid sway, tight curls
    { 14, -3, 11, 20, 15, 25 },
    // HUNTING:  alert, stiff, minimal sway
    {  8, -1,  9,  3,  3, 40 },
    // SLEEPY:   droopy, slow sway, loose curls
    {  6,  4,  8,  5,  6,  8 },
    // SAD:      wilted, hang down
    {  4,  6,  7,  4,  4,  6 },
    // ANGRY:    bristled stiff, spread apart, micro-tremor
    {  3, -4, 14, 30,  5, 50 },
};

// Read PROGMEM emotion params into locals
static void readHairEmo(int stateIdx, float& curlScale, float& droopY,
                        float& spreadMul, float& swaySpeed, float& swayAmp,
                        float& stiffness) {
    constexpr int NUM_EMOTIONS = sizeof(HAIR_EMOTIONS) / sizeof(HAIR_EMOTIONS[0]);
    if (stateIdx < 0 || stateIdx >= NUM_EMOTIONS) stateIdx = 0;
    HairEmotionParams raw;
    memcpy_P(&raw, &HAIR_EMOTIONS[stateIdx], sizeof(raw));
    curlScale = (float)raw.targetCurlScale / 10.0f;
    droopY    = (float)raw.targetDroopY;
    spreadMul = (float)raw.spreadMul / 10.0f;
    swaySpeed = (float)raw.swaySpeed / 10.0f;
    swayAmp   = (float)raw.swayAmp / 10.0f;
    stiffness = (float)raw.stiffness / 100.0f;
}

void Avatar::updateHairPhysics(int16_t pigX, int16_t shakeY,
                               AvatarState visualState) {
    if (!hairs) return;
    int16_t velX = pigX - prevPigX;
    int16_t velY = shakeY - prevShakeY;
    // clamp to prevent hair fling on context jumps (IDLE↔room)
    if (velX >  6) velX =  6; if (velX < -6) velX = -6;
    if (velY >  6) velY =  6; if (velY < -6) velY = -6;
    prevPigX = pigX;
    prevShakeY = shakeY;

    float emoCurl, emoDroop, emoSpread, emoSwaySpd, emoSwayAmp, emoStiff;
    readHairEmo((int)visualState, emoCurl, emoDroop, emoSpread,
                emoSwaySpd, emoSwayAmp, emoStiff);

    for (uint8_t i = 0; i < HAIR_COUNT; i++) {
        HairState& h = hairs[i];

        // Inertia: hair leans opposite to pig movement
        float inertiaX = (float)(-velX) * 0.6f;

        // Spring return to center
        float springForce = -h.leanX * emoStiff;
 
        // Damping
        float damping = -h.leanVelX * 0.3f;

        h.leanVelX += inertiaX + springForce + damping;
        if (h.leanVelX > 8.0f) h.leanVelX = 8.0f;
        if (h.leanVelX < -8.0f) h.leanVelX = -8.0f;

        h.leanX += h.leanVelX;
        if (h.leanX > 15.0f) h.leanX = 15.0f;
        if (h.leanX < -15.0f) h.leanX = -15.0f;

        // Smooth emotion transitions
        h.curlScale += (emoCurl - h.curlScale) * 0.08f;
        h.droopY += (emoDroop - h.droopY) * 0.08f;

        // Jump reactivity: rapid shakeY changes kick droop
        if (velY < -2) {
            h.droopY += 1.5f;  // going up fast: hairs lag behind
        } else if (velY > 2) {
            h.droopY -= 1.0f;  // coming down: hairs compress upward
        }
    }

    // Tail physics — springy coil, not droopy hair.
    // Stiff spring + low damping = wobble that overshoots and oscillates.
    // Horizontal wobble
    float tInertia = (float)(-velX) * 1.2f;
    float tSpring  = -tailPhys.leanX * 0.28f;   // stiff — snaps back fast
    float tDamp    = -tailPhys.leanVelX * 0.10f; // low — lets it ring
    tailPhys.leanVelX += tInertia + tSpring + tDamp;
    if (tailPhys.leanVelX > 12.0f) tailPhys.leanVelX = 12.0f;
    if (tailPhys.leanVelX < -12.0f) tailPhys.leanVelX = -12.0f;
    tailPhys.leanX += tailPhys.leanVelX;
    if (tailPhys.leanX > 12.0f) tailPhys.leanX = 12.0f;
    if (tailPhys.leanX < -12.0f) tailPhys.leanX = -12.0f;

    // Vertical bounce — spring, not gravity droop
    float tBobInertia = (float)(-velY) * 1.0f;
    float tBobSpring  = -tailPhys.bobY * 0.30f;
    float tBobDamp    = -tailPhys.bobVelY * 0.12f;
    tailPhys.bobVelY += tBobInertia + tBobSpring + tBobDamp;
    if (tailPhys.bobVelY > 10.0f) tailPhys.bobVelY = 10.0f;
    if (tailPhys.bobVelY < -10.0f) tailPhys.bobVelY = -10.0f;
    tailPhys.bobY += tailPhys.bobVelY;
    if (tailPhys.bobY > 10.0f) tailPhys.bobY = 10.0f;
    if (tailPhys.bobY < -10.0f) tailPhys.bobY = -10.0f;

    // Curl tightness — stretches on fast motion, snaps tight at rest
    float speed = sqrtf((float)(velX * velX + velY * velY));
    float targetCurl = 1.0f + speed * 0.06f;
    if (targetCurl > 1.4f) targetCurl = 1.4f;
    tailPhys.curlScale += (targetCurl - tailPhys.curlScale) * 0.15f;

    // Idle wiggle — pig tail has restless energy even when still
    {
        uint32_t now = avatarRenderMillis();
        float wiggle = sinf((float)now * 0.008f) * 0.6f;
        tailPhys.leanVelX += wiggle;
    }
}

void Avatar::drawHairs(M5Canvas& canvas, int16_t startX, int16_t startY,
                       int16_t sy, bool faceRight,
                       AvatarState visualState, bool rearView) {
    (void)sy;
    if (!hairs || rearView || usesFedora()) return;  // hat styles replace hair
    uint32_t now = avatarRenderMillis();
    float emoCurl, emoDroop, emoSpread, emoSwaySpd, emoSwayAmp, emoStiff;
    readHairEmo((int)visualState, emoCurl, emoDroop, emoSpread,
                emoSwaySpd, emoSwayAmp, emoStiff);
    bool rainbow = shouldRenderHypeHair();

    int16_t baseY = startY + 11;
    const int16_t headFillLeft = startX + 12;
    const int16_t headFillRight = startX + PIG_BODY_W - 12;

    // Root X offsets from startX (right-facing, matching ASCII positions)
    // 6 hairs centered on head with face-side bias
    const int16_t ROOT_OFFSETS[6] = {
        29, 33, 36, 39, 43, 47
    };

    // Neutral curl shapes: 4 points each (dx, dy). dy negative = upward. Raw pixel values.
    const int8_t CURL_SHAPES[6][4][2] = {
        { {0, 0}, {-3, -9},  {3, -15},  {6, -12} },
        { {0, 0}, {-2, -8},  {2, -14},  {5, -11} },
        { {0, 0}, {3, -10},  {-3, -17}, {-5, -13} },
        { {0, 0}, {2, -9},   {-2, -15}, {-4, -12} },
        { {0, 0}, {3, -9},   {-3, -15}, {-6, -12} },
        { {0, 0}, {4, -8},   {-1, -14}, {-3, -10} },
    };

    for (uint8_t i = 0; i < HAIR_COUNT; i++) {
        const HairState& h = hairs[i];

        // Root position
        int16_t rootOffset = ROOT_OFFSETS[i];
        if (!faceRight) rootOffset = PIG_BODY_W - rootOffset;
        int16_t rootX = startX + rootOffset;
        rootX = constrain(rootX, headFillLeft, headFillRight);
        int16_t rootY = baseY;

        // Spread modifier (angry = spread, sad = huddle)
        float spreadFromCenter = (float)(ROOT_OFFSETS[i] - PIG_BODY_W / 2);
        float spreadX = spreadFromCenter * (emoSpread - 1.0f) * 0.3f;

        // Total lean: physics + spread
        // Pre-negate physics lean for left-facing so the global mirror
        // restores correct world-space direction (trail behind movement)
        float physLean = faceRight ? h.leanX : -h.leanX;
        float totalLeanX = physLean + spreadX;

        // Idle sway: triangle wave, per-hair phase offset
        {
            uint32_t phase = now + (uint32_t)i * 831;
            uint32_t period = (uint32_t)(2500.0f / emoSwaySpd);
            if (period < 500) period = 500;
            int wave = (int)(phase % period);
            int half = (int)(period / 2);
            int quarter = (int)(period / 4);
            int swayVal = (wave < half) ? (wave - quarter) : ((half + quarter) - wave);
            float swayPx = (float)swayVal * PIG_PX_CONST * emoSwayAmp / (float)quarter;
            totalLeanX += swayPx;
        }

        // Hunting: lean forward
        if (visualState == AvatarState::HUNTING) {
            totalLeanX += 3.0f;
        }

        // Read curl shape from PROGMEM and build actual points
        int8_t shapes[4][2];
        memcpy_P(shapes, CURL_SHAPES[i], sizeof(shapes));

        int16_t px[4], py[4];
        for (uint8_t p = 0; p < 4; p++) {
            float dx = (float)shapes[p][0];
            float dy = (float)shapes[p][1];

            // Curl scale on upper control points
            if (p >= 2) {
                float lateralBase = (float)(shapes[p][0] - shapes[p-1][0]);
                dx = (float)shapes[p-1][0] + lateralBase * h.curlScale;
            }

            // Progressive lean from root to tip
            float leanFactor = (float)p / 3.0f;
            dx += totalLeanX * leanFactor;

            // Progressive droop from root to tip
            float droopFactor = (float)p / 3.0f;
            dy += h.droopY * droopFactor;

            // Mirror when facing left
            if (!faceRight) dx = -dx;

            px[p] = snapPx(rootX + (int16_t)lroundf(dx));
            py[p] = snapPx(rootY + (int16_t)lroundf(dy));
        }

        // Draw 3 segments
        for (uint8_t s = 0; s < 3; s++) {
            uint16_t segColor;
            if (rainbow) {
                int16_t mx = (int16_t)((px[s] + px[s+1]) / 2);
                int16_t my = (int16_t)((py[s] + py[s+1]) / 2);
                segColor = trippyRainbow(mx, my);
            } else {
                segColor = getHairAccentColor(getDrawColor());
            }
            fatLine(canvas, px[s], py[s], px[s+1], py[s+1], segColor);
        }
    }
}

// ==[ BODY-ONLY RENDERER ]== for MenuPig — no grass, trees, stars, waves
void Avatar::drawBodyOnly(M5Canvas& canvas, int x, int y,
                          uint16_t fg, uint16_t bg,
                          AvatarState state, bool blink, bool faceRight,
                          bool sniff, bool earTwitch,
                          PigEyeLook eyeLook, uint16_t detailColor, char tailChar,
                          PigLight light, bool ramenEating,
                          bool blendRounding, uint8_t sniffFrameOverride) {
    // Sniff frame for expression factory
    uint8_t sf = 0;
    if (sniff) {
        sf = sniffFrameOverride <= 2
            ? sniffFrameOverride
            : (uint8_t)((avatarRenderMillis() / 200) % 3);
    }
    uint16_t bodyFill = fg;
    uint16_t bodyDetail = detailColor;
    if (bodyDetail == 0) {
        Display::PigPalette palette = Display::makePigPalette(fg, bg);
        bodyFill = palette.bodyFill;
        bodyDetail = palette.detail;
    }

    PigRenderPose pose;
    pose.x = x;
    pose.y = y;
    pose.facing = faceRight ? PigFacing::RIGHT : PigFacing::LEFT;
    pose.expression = PigExpression::fromState(state, blink, sniff, sf, earTwitch);
    pose.tailGlyph = tailChar;
    pose.tailOnLeft = faceRight;
    pose.eyeLook = eyeLook;
    pose.detailColor = bodyDetail;
    pose.bellyBreathePx = 0;
    pose.scale = PIG_PX;
    pose.talking = Mood::hasPhrase();
    pose.light = light;
    pose.ramenEating = ramenEating;
    pose.blendRounding = blendRounding;

    PigRenderer::drawBody(canvas, pose, bodyFill, bg);
}

// ==[ HAIR WRAPPER ]== for external callers sharing PSRAM hair state
void Avatar::drawHairsAt(M5Canvas& canvas, int16_t startX, int16_t startY,
                         int16_t shakeY, bool faceRight,
                         AvatarState visualState, bool rearView) {
    updateHairPhysics(startX, shakeY, visualState);
    drawHairs(canvas, startX, startY, shakeY, faceRight,
              visualState, rearView);
}

void Avatar::setRenderTimeOverride(uint32_t now) {
    renderTimeOverrideActive = true;
    renderTimeOverrideMs = now;
}

void Avatar::clearRenderTimeOverride() {
    renderTimeOverrideActive = false;
}

void Avatar::resetCaptureState() {
    clearRenderTimeOverride();
    posControl.owner = PosOwner::IDLE;
    currentState = AvatarState::NEUTRAL;
    faceTimer.reset();
    isSniffing = false;
    sniffAnim.startTime = 0;
    sniffAnim.frame = 0;
    tailWiggleActive = false;
    tailWiggleStart = 0;
    facingAway = false;
    body.clear();
    attackHopIndex = 0;
    attackHopTotal = 0;
    attackHopOriginX = PIG_CENTER_X;
    memset(attackHopTargets, 0, sizeof(attackHopTargets));
    transitioning = false;
    transitionStartTime = 0;
    transitionFromX = PIG_CENTER_X;
    transitionToX = PIG_CENTER_X;
    transitionToFacingRight = true;
    currentX = PIG_CENTER_X;
    grassMoving = false;
    grassDirection = true;
    pendingGrassStart = false;
    onRightSide = false;
    attack.shakeActive = false;
    attack.shakeAmplitude = 0.0f;
    attack.shakeRefreshTime = 0;
    thunderFlashActive = false;
    portal.active = false;
    portal.fromX = PIG_CENTER_X;
    portal.toX = PIG_CENTER_X;
    portal.start = 0;
    portal.duration = 400;
    cinematicPose = {};
    prevPigX = PIG_CENTER_X;
    prevShakeY = 0;
    waveMode = WaveMode::NONE;
    waveBurstStart = 0;
    waveBurstEnd = 0;
    wave.treeShaking = false;
    wave.treeShakeStart = 0;
    treePhase = TreePhase::HIDDEN;
    treeGrowth = 0.0f;
    treePendingHide = false;
    treePendingShow = false;
    treePendingFruits = 0;
    treeAliveStart = 0;
    treeScrollOffset = 0;
    treeImpactTriggered = false;
    collision.treeColliding = false;
    collision.wasTreeColliding = false;
    collision.lastTreeShakeSparkle = 0;
    hype.unlocked = false;
    hype.fillActive = false;
    hype.fillStart = 0;
    memset(hype.fillCount, 0, sizeof(hype.fillCount));
    hype.fillSeed = 0;
    if (hairs) {
        for (uint8_t i = 0; i < HAIR_COUNT; ++i) hairs[i] = {0.0f, 0.0f, 1.0f, 0.0f};
    }
    for (uint8_t i = 0; i < MAX_SPARKLES; ++i) sparkles[i] = {};
    for (int i = 0; i < TRAIL_COUNT; ++i) trailParticles[i] = {};
    trailSpawnIdx = 0;
    lastTrailSpawn = 0;
    lastTrailUpdate = 0;
}

static void drawCaptureShadow(M5Canvas& canvas, int16_t pigX, int16_t pigY,
                              uint16_t fg, uint16_t bg, uint32_t now) {
    int breathe = (int)((now / 200) % 2) ? 0 : -2;
    int sw = 48 + breathe;
    int sh = 6;
    int sx = pigX + (PIG_BODY_W - sw) / 2;
    int sy = pigY + PIG_BODY_H;
    uint16_t core = Display::lerpColor565(bg, fg, 0.18f);
    uint16_t edge = Display::lerpColor565(bg, fg, 0.12f);

    float cx = (float)sx + (float)sw * 0.5f;
    float cy = (float)sy + (float)sh * 0.5f;
    float rx = (float)sw * 0.5f;
    float ry = (float)sh * 0.5f;
    for (int y = sy; y < sy + sh; y += PIG_PX) {
        for (int x = sx; x < sx + sw; x += PIG_PX) {
            float dx = ((float)x + 1.0f - cx) / rx;
            float dy = ((float)y + 1.0f - cy) / ry;
            float dist = dx * dx + dy * dy;
            if (dist > 1.0f) continue;
            if (dist < 0.45f || (((x >> 1) ^ (y >> 1)) & 1) == 0) {
                canvas.fillRect(x, y, PIG_PX, PIG_PX, dist < 0.72f ? core : edge);
            }
        }
    }
}

static PigEyeLook captureDefaultEyeLook(AvatarState state, bool rear, bool actionUp) {
    if (rear || state == AvatarState::SLEEPY) return PigEyeLook::NONE;
    if (actionUp || state == AvatarState::HUNTING || state == AvatarState::ANGRY ||
        state == AvatarState::EXCITED) {
        return PigEyeLook::FRONT_UP;
    }
    if (state == AvatarState::HAPPY) return PigEyeLook::BACK_UP;
    return PigEyeLook::BACK_DOWN;
}

static bool matchCaptureClip(const char* clipId, const char* expected) {
    return clipId && strcmp(clipId, expected) == 0;
}

static void renderAvatarCapturePig(M5Canvas& canvas, uint32_t now, AvatarState state,
                                   bool faceRightNow, bool facingRearNow,
                                   bool blink, bool sniff, bool earTwitch, bool talking,
                                   PigEyeLook eyeLook, LimbMode limbMode,
                                   int16_t drawX, int16_t drawY, int16_t shakeY,
                                   int16_t bellyBreathePx, uint8_t walkFrame,
                                   char tailGlyph, bool tailOnLeft) {
    // Grid discipline: capture-clip anims (avatar_anim_butt_flex /
    // cute_jump / spin / flinch) add sub-cell hop/shake offsets to
    // drawX/drawY. Snap the body origin to the pig grid (PIG_PX) so it
    // lands on the same 2px lattice as buildIdlePose() and never at an
    // odd sub-grid offset.
    drawX = snapToPx(drawX, PIG_PX);
    drawY = snapToPx(drawY, PIG_PX);
    uint16_t fg = Display::getColorFG();
    uint16_t bg = Display::getColorBG();
    Display::PigPalette palette = Display::makePigPalette(fg, bg);
    uint8_t sf = sniff ? (uint8_t)((avatarRenderMillis() / 200) % 3) : 0;

    Avatar::setState(state);
    Avatar::facingAway = facingRearNow;
    Avatar::setCurrentX(drawX);

    drawCaptureShadow(canvas, drawX, drawY, fg, bg, now);

    PigRenderPose pose;
    pose.x = drawX;
    pose.y = drawY;
    pose.facing = facingRearNow ? PigFacing::REAR : (faceRightNow ? PigFacing::RIGHT : PigFacing::LEFT);
    pose.expression = PigExpression::fromState(state, blink, sniff, sf, earTwitch);
    pose.bellyBreathePx = bellyBreathePx;
    pose.limbMode = limbMode;
    pose.walkFrame = walkFrame;
    pose.tailGlyph = tailGlyph;
    pose.tailOnLeft = tailOnLeft;
    pose.eyeLook = eyeLook;
    pose.detailColor = palette.detail;
    pose.scale = PIG_PX;
    pose.talking = talking;

    drawPigFull(canvas, pose, palette.bodyFill, palette.voidColor, now);
    Avatar::drawHairsAt(canvas, drawX, drawY, shakeY, faceRightNow,
                        state, facingRearNow);
}

static bool renderAvatarCaptureClipImpl(M5Canvas& canvas, const char* clipId, uint32_t now) {
    static constexpr uint32_t kCaptureTailWiggleMs = 800;
    static constexpr uint32_t kCapturePerkUpMs = 200;
    static constexpr int kCapturePerkUpHeight = 2;
    static constexpr uint32_t kCaptureFlinchMs = 300;
    static constexpr uint32_t kCapturePawScratchMs = 800;
    static constexpr uint32_t kCaptureButtFlexMs = 1200;
    static constexpr uint32_t kCaptureJumpMs = 400;
    static constexpr int kCaptureJumpHeight = 5;
    static constexpr uint32_t kCaptureSpinMs = 600;
    static constexpr uint8_t kCaptureSpinFlips = 4;
    static constexpr uint32_t kCaptureAttackHopMs = 250;
    static constexpr int16_t kCaptureAttackHopHeight = 7;
    static constexpr uint32_t kCaptureTreeChargeMs = 600;
    static constexpr uint32_t kCaptureTreeRetreatMs = 500;
    const int16_t baseX = PIG_CENTER_X;
    const int16_t baseY = 116;
    AvatarState state = AvatarState::NEUTRAL;
    bool faceRightNow = true;
    bool facingRearNow = false;
    bool blink = false;
    bool sniff = false;
    bool earTwitch = false;
    bool talking = false;
    PigEyeLook eyeLook = PigEyeLook::BACK_DOWN;
    LimbMode limbMode = LimbMode::STANDING;
    int16_t drawX = baseX;
    int16_t drawY = baseY;
    int16_t shakeY = 0;
    int16_t bellyBreathePx = 0;
    uint8_t walkFrame = 0;
    char tailGlyph = 'z';
    bool tailOnLeft = true;
    bool handled = true;

    if (matchCaptureClip(clipId, "avatar_static_neutral_right")) {
        eyeLook = PigEyeLook::BACK_DOWN;
    } else if (matchCaptureClip(clipId, "avatar_static_neutral_left")) {
        faceRightNow = false;
        tailOnLeft = false;
        eyeLook = PigEyeLook::BACK_DOWN;
    } else if (matchCaptureClip(clipId, "avatar_static_neutral_rear")) {
        facingRearNow = true;
        tailOnLeft = false;
        eyeLook = PigEyeLook::NONE;
        limbMode = LimbMode::REAR_NUBS;
    } else if (matchCaptureClip(clipId, "avatar_static_happy_right")) {
        state = AvatarState::HAPPY;
        eyeLook = PigEyeLook::BACK_UP;
    } else if (matchCaptureClip(clipId, "avatar_static_excited_right")) {
        state = AvatarState::EXCITED;
        eyeLook = PigEyeLook::FRONT_UP;
    } else if (matchCaptureClip(clipId, "avatar_static_hunting_right")) {
        state = AvatarState::HUNTING;
        eyeLook = PigEyeLook::FRONT_UP;
    } else if (matchCaptureClip(clipId, "avatar_static_sleepy_right")) {
        state = AvatarState::SLEEPY;
        eyeLook = PigEyeLook::NONE;
    } else if (matchCaptureClip(clipId, "avatar_static_sad_right")) {
        state = AvatarState::SAD;
        eyeLook = PigEyeLook::BACK_DOWN;
    } else if (matchCaptureClip(clipId, "avatar_static_angry_right")) {
        state = AvatarState::ANGRY;
        eyeLook = PigEyeLook::FRONT_UP;
    } else if (matchCaptureClip(clipId, "avatar_static_eye_back_up")) {
        eyeLook = PigEyeLook::BACK_UP;
    } else if (matchCaptureClip(clipId, "avatar_static_eye_front_up")) {
        eyeLook = PigEyeLook::FRONT_UP;
    } else if (matchCaptureClip(clipId, "avatar_static_eye_back_down")) {
        eyeLook = PigEyeLook::BACK_DOWN;
    } else if (matchCaptureClip(clipId, "avatar_static_eye_front_down")) {
        eyeLook = PigEyeLook::FRONT_DOWN;
    } else if (matchCaptureClip(clipId, "avatar_static_eye_center")) {
        eyeLook = PigEyeLook::CENTER;
    } else if (matchCaptureClip(clipId, "avatar_static_walk_stub_right")) {
        eyeLook = PigEyeLook::FRONT_UP;
        limbMode = LimbMode::WALKING_IDLE;
    } else if (matchCaptureClip(clipId, "avatar_anim_blink")) {
        blink = (now >= 120 && now < 240);
        eyeLook = PigEyeLook::BACK_DOWN;
    } else if (matchCaptureClip(clipId, "avatar_anim_sniff")) {
        sniff = true;
        eyeLook = PigEyeLook::FRONT_UP;
    } else if (matchCaptureClip(clipId, "avatar_anim_ear_twitch")) {
        earTwitch = (now >= 120 && now < 200);
        eyeLook = PigEyeLook::BACK_DOWN;
    } else if (matchCaptureClip(clipId, "avatar_anim_talk")) {
        talking = true;
        eyeLook = PigEyeLook::BACK_UP;
    } else if (matchCaptureClip(clipId, "avatar_anim_tail_wiggle_rear")) {
        facingRearNow = true;
        tailOnLeft = false;
        eyeLook = PigEyeLook::NONE;
        limbMode = LimbMode::REAR_NUBS;
        if (now < kCaptureTailWiggleMs && ((now / 120) & 1)) tailGlyph = '~';
    } else if (matchCaptureClip(clipId, "avatar_anim_perk_up")) {
        earTwitch = (now < kCapturePerkUpMs);
        eyeLook = PigEyeLook::FRONT_UP;
        if (now < kCapturePerkUpMs) {
            float t = (float)now / (float)kCapturePerkUpMs;
            shakeY = -(int16_t)(4.0f * t * (1.0f - t) * (float)kCapturePerkUpHeight);
            drawY += shakeY;
        }
    } else if (matchCaptureClip(clipId, "avatar_anim_flinch")) {
        eyeLook = PigEyeLook::FRONT_UP;
        if (now < 150) shakeY = 3;
        else if (now < kCaptureFlinchMs) shakeY = (((now - 150) / 60) & 1) ? 2 : -2;
        drawY += shakeY;
    } else if (matchCaptureClip(clipId, "avatar_anim_paw_scratch")) {
        eyeLook = PigEyeLook::BACK_DOWN;
        if (now < kCapturePawScratchMs) drawX += (((now / 100) & 1) == 0) ? 2 : -2;
    } else if (matchCaptureClip(clipId, "avatar_anim_butt_flex")) {
        facingRearNow = true;
        tailOnLeft = false;
        eyeLook = PigEyeLook::NONE;
        limbMode = LimbMode::REAR_NUBS;
        if (now < kCaptureButtFlexMs) {
            float phase = (float)now / 200.0f;
            drawX += (int16_t)lroundf(sinf(phase * 3.14159f) * 4.0f);
            float hopPhase = fmodf(phase, 2.0f);
            shakeY = -(int16_t)lroundf(sinf(hopPhase * 3.14159f) * 3.0f);
            drawY += shakeY;
        }
    } else if (matchCaptureClip(clipId, "avatar_anim_cute_jump")) {
        eyeLook = PigEyeLook::FRONT_UP;
        if (now < kCaptureJumpMs) {
            float t = (float)now / (float)kCaptureJumpMs;
            shakeY = -(int16_t)(4.0f * t * (1.0f - t) * (float)kCaptureJumpHeight);
            drawY += shakeY;
            limbMode = LimbMode::AIRBORNE;
        }
    } else if (matchCaptureClip(clipId, "avatar_anim_spin")) {
        eyeLook = PigEyeLook::FRONT_UP;
        if (now < kCaptureSpinMs) {
            float t = (float)now / (float)kCaptureSpinMs;
            shakeY = -(int16_t)(4.0f * t * (1.0f - t) * (float)kCaptureJumpHeight);
            drawY += shakeY;
            limbMode = LimbMode::AIRBORNE;
            uint8_t flipPhase = now / (kCaptureSpinMs / kCaptureSpinFlips);
            faceRightNow = ((flipPhase % 2) == 0);
        } else {
            faceRightNow = false;
            tailOnLeft = false;
        }
    } else if (matchCaptureClip(clipId, "avatar_anim_attack_hop")) {
        eyeLook = PigEyeLook::FRONT_UP;
        static const int16_t kTargets[4] = {156, 104, 148, 124};
        const uint32_t totalMs = 4 * kCaptureAttackHopMs;
        if (now < totalMs) {
            uint8_t hopIdx = (uint8_t)(now / kCaptureAttackHopMs);
            uint32_t hopLocal = now % kCaptureAttackHopMs;
            float t = (float)hopLocal / (float)kCaptureAttackHopMs;
            float smoothT = t * t * (3.0f - 2.0f * t);
            int16_t fromX = (hopIdx == 0) ? baseX : kTargets[hopIdx - 1];
            int16_t toX = kTargets[hopIdx];
            drawX = fromX + (int16_t)lroundf((float)(toX - fromX) * smoothT);
            faceRightNow = (toX > fromX);
            tailOnLeft = faceRightNow;
            float arc = 4.0f * t * (1.0f - t);
            shakeY = -(int16_t)lroundf(arc * (float)kCaptureAttackHopHeight);
            drawY += shakeY;
        } else {
            drawX = baseX;
            faceRightNow = false;
            tailOnLeft = false;
        }
    } else if (matchCaptureClip(clipId, "avatar_anim_attack_tree_body")) {
        eyeLook = PigEyeLook::FRONT_UP;
        static const int16_t kTargets[2] = {172, 124};
        const uint32_t totalMs = kCaptureTreeChargeMs + kCaptureTreeRetreatMs;
        if (now < totalMs) {
            bool retreat = now >= kCaptureTreeChargeMs;
            uint32_t hopLocal = retreat ? (now - kCaptureTreeChargeMs) : now;
            uint16_t hopMs = retreat ? kCaptureTreeRetreatMs : kCaptureTreeChargeMs;
            float t = (float)hopLocal / (float)hopMs;
            float smoothT = t * t * (3.0f - 2.0f * t);
            int16_t fromX = retreat ? kTargets[0] : baseX;
            int16_t toX = retreat ? kTargets[1] : kTargets[0];
            drawX = fromX + (int16_t)lroundf((float)(toX - fromX) * smoothT);
            faceRightNow = retreat ? false : (toX > fromX);
            tailOnLeft = faceRightNow;
            float arc = 4.0f * t * (1.0f - t);
            int16_t hopHeight = retreat ? 6 : kCaptureAttackHopHeight;
            shakeY = -(int16_t)lroundf(arc * (float)hopHeight);
            drawY += shakeY;
        } else {
            drawX = baseX;
            faceRightNow = false;
            tailOnLeft = false;
        }
    } else {
        handled = false;
    }

    if (!handled) return false;
    if (facingRearNow) {
        eyeLook = PigEyeLook::NONE;
        tailOnLeft = false;
    }
    if (!matchCaptureClip(clipId, "avatar_anim_sniff") &&
        !matchCaptureClip(clipId, "avatar_anim_talk") &&
        !matchCaptureClip(clipId, "avatar_static_eye_back_up") &&
        !matchCaptureClip(clipId, "avatar_static_eye_front_up") &&
        !matchCaptureClip(clipId, "avatar_static_eye_back_down") &&
        !matchCaptureClip(clipId, "avatar_static_eye_front_down") &&
        !matchCaptureClip(clipId, "avatar_static_eye_center")) {
        eyeLook = captureDefaultEyeLook(state, facingRearNow,
                                        matchCaptureClip(clipId, "avatar_anim_perk_up") ||
                                        matchCaptureClip(clipId, "avatar_anim_flinch") ||
                                        matchCaptureClip(clipId, "avatar_anim_spin") ||
                                        matchCaptureClip(clipId, "avatar_anim_attack_hop") ||
                                        matchCaptureClip(clipId, "avatar_anim_attack_tree_body"));
    }

    renderAvatarCapturePig(canvas, now, state, faceRightNow, facingRearNow, blink, sniff,
                           earTwitch, talking, eyeLook, limbMode, drawX, drawY, shakeY,
                           bellyBreathePx, walkFrame, tailGlyph, tailOnLeft);
    return true;
}

bool Avatar::renderCaptureClip(M5Canvas& canvas, const char* clipId, uint32_t now) {
    setRenderTimeOverride(now);
    bool ok = renderAvatarCaptureClipImpl(canvas, clipId, now);
    clearRenderTimeOverride();
    return ok;
}

static PigEyeLook resolveIdleEyeLook(uint32_t now, AvatarState state, bool facingRear,
                                     bool isWalking, bool sniff, bool blink,
                                     bool talking, bool actionLookUp) {
    (void)now;
    (void)isWalking;
    if (facingRear || state == AvatarState::SLEEPY || blink) return PigEyeLook::NONE;
    if (sniff) return PigEyeLook::FRONT_DOWN;
    if (actionLookUp || state == AvatarState::HUNTING ||
        state == AvatarState::ANGRY || state == AvatarState::EXCITED) {
        return PigEyeLook::FRONT_UP;
    }
    if (talking) return PigEyeLook::CENTER;
    return faceTimer.eyeLook;
}

// Forward declaration — table-driven body anim duration lookup (defined below drawFrame)
static uint16_t getBodyAnimDuration(BodyAnim anim);

void Avatar::draw(M5Canvas& canvas) {
    PX = Display::getSceneryPX();  // refresh per frame
    uint32_t now = millis();

    // Sniff animation timeout
    if (isSniffing) {
        if (now - sniffAnim.startTime > SniffState::DURATION_MS) {
            isSniffing = false;
            sniffAnim.frame = 0;
        } else {
            sniffAnim.frame = ((now - sniffAnim.startTime) / 100) % 3;
        }
    }

    // Body anim auto-expire — table-driven duration check
    if (body.active()) {
        uint16_t dur = getBodyAnimDuration(body.anim);
        if (dur > 0 && (now - body.startTime) >= dur) {
            body.clear();
        }
        // ATTACK_HOP/ATTACK_TREE expire in updateAnimState
    }

    // Handle walk transition animation
    if (transitioning) {
        uint32_t elapsed = now - transitionStartTime;
        if (elapsed >= TRANSITION_DURATION_MS) {
            transitioning = false;
            posControl.release(PosOwner::WALK_TRANS);
            currentX = transitionToX;
            walkLook.facingRight = transitionToFacingRight;
            onRightSide = (currentX > PIG_CENTER_X);

            if (pendingGrassStart) {
                grassMoving = true;
                pendingGrassStart = false;
                posControl.claim(PosOwner::GRASS_WALK);
                walkLook.facingRight = !grassDirection;
            } else if (!grassMoving) {
                // Post-walk random behavior
                int arrivalRoll = random(0, 100);
                if (arrivalRoll < 20) {
                    walkLook.facingRight = !walkLook.facingRight;
                } else if (arrivalRoll < 35) {
                    sniff();
                } else if (arrivalRoll < 45) {
                    wiggleEars();
                } else if (arrivalRoll < 55) {
                    walkLook.facingRight = !transitionToFacingRight;
                }
            }

            walkLook.lastLookTime = now;
            walkLook.lookInterval = random(1500, 6000);
        } else {
            float t = (float)elapsed / TRANSITION_DURATION_MS;
            // Quintic ease: 6t^5 - 15t^4 + 10t^3
            float smoothT = t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
            currentX = transitionFromX + (int)((transitionToX - transitionFromX) * smoothT);
        }
    }

    // Portal pull: override currentX during collapse phase
    if (portal.active) {
        uint32_t elapsed = now - portal.start;
        if (elapsed >= portal.duration) {
            currentX = portal.toX;
            portal.active = false;
            posControl.release(PosOwner::PORTAL_PULL);
        } else {
            float pt = (float)elapsed / (float)portal.duration;
            // ease-in: accelerate into the portal
            float e = pt * pt;
            currentX = portal.fromX + (int16_t)((float)(portal.toX - portal.fromX) * e);
        }
    }

    // Blink + ear twitch — shared timer drives both
    faceTimer.update(now, moodIntensity, transitioning || body.active());

    // Intensity-adjusted intervals
    float flipMod = 1.0f - (moodIntensity / 300.0f);
    uint32_t minWalk = (uint32_t)(30000 * flipMod);
    uint32_t maxWalk = (uint32_t)(75000 * flipMod);
    uint32_t minLook = (uint32_t)(4000 * flipMod);
    uint32_t maxLook = (uint32_t)(15000 * flipMod);

    // === ORGANIC RANDOM BEHAVIORS ===
    if (!transitioning && !grassMoving && !pendingGrassStart && !isAttackHopping() && !isCinematic()) {

        // --- LOOK BEHAVIOR ---
        if (now - walkLook.lastLookTime > walkLook.lookInterval) {
            int lookRoll = random(0, 100);

            if (lookRoll < 35) {
                walkLook.facingRight = !walkLook.facingRight;
            } else if (lookRoll < 55) {
                walkLook.facingRight = !walkLook.facingRight;
                walkLook.lookInterval = random(800, 1500);
                walkLook.lastLookTime = now;
                goto skip_look_reset;
            } else if (lookRoll < 70) {
                walkLook.facingRight = random(0, 2) == 0;
                sniff();
            } else if (lookRoll < 82) {
                wiggleEars();
            } else if (lookRoll < 95) {
                blink();
            }

            walkLook.lastLookTime = now;
            if (random(0, 5) == 0) {
                walkLook.lookInterval = random(1500, 4000);
            } else {
                walkLook.lookInterval = random(minLook, maxLook);
            }
        }
        skip_look_reset:

        // --- WALK BEHAVIOR ---
        if (now - walkLook.lastFlipTime > walkLook.flipInterval) {
            int walkRoll = random(0, 100);
            int targetX;

            const int LEFT_EDGE = PIG_MIN_X;
            const int RIGHT_EDGE = PIG_MAX_X;

            if (walkRoll < 50) {
                targetX = onRightSide ? LEFT_EDGE : RIGHT_EDGE;
            } else if (walkRoll < 85) {
                targetX = random(0, 2) == 0 ? LEFT_EDGE : RIGHT_EDGE;
            } else if (walkRoll < 95) {
                if (onRightSide) {
                    targetX = random(RIGHT_EDGE - 38, RIGHT_EDGE + 1);
                } else {
                    targetX = random(LEFT_EDGE, LEFT_EDGE + 38);
                }
            } else {
                walkLook.facingRight = !walkLook.facingRight;
                walkLook.lastFlipTime = now;
                walkLook.flipInterval = random(minWalk / 2, maxWalk / 2);
                goto skip_walk;
            }

            if (abs(targetX - currentX) > 15) {
                bool goingRight = targetX > currentX;
                transitioning = true;
                transitionStartTime = now;
                transitionFromX = currentX;
                transitionToX = targetX;
                transitionToFacingRight = goingRight;
                walkLook.facingRight = goingRight;

                walkLook.lastFlipTime = now;
                if (random(0, 4) == 0) {
                    walkLook.flipInterval = random(15000, 30000);
                } else {
                    walkLook.flipInterval = random(minWalk, maxWalk);
                }
            } else {
                walkLook.facingRight = targetX > currentX;
                walkLook.lastFlipTime = now;
                walkLook.flipInterval = random(minWalk / 3, minWalk);
            }
        }
        skip_walk:;
    }

    // === GRASS WANDER ===
    if (!transitioning && grassMoving && !isAttackHopping() && !isCinematic()) {
        if (now - walkLook.grassWanderTimer > walkLook.grassWanderInterval) {
            int homeX = grassDirection ? PIG_MAX_X : PIG_MIN_X;
            int centerX = SCREEN_WIDTH / 2;
            int distFromHome = abs(currentX - homeX);

            if (distFromHome < 20) {
                if (random(0, 100) < 35) {
                    int lo = (homeX < centerX) ? homeX + 10 : centerX;
                    int hi = (homeX < centerX) ? centerX : homeX - 10;
                    int target = random(lo, hi + 1);
                    if (abs(target - currentX) > 10) {
                        transitioning = true;
                        transitionStartTime = now;
                        transitionFromX = currentX;
                        transitionToX = target;
                        transitionToFacingRight = !grassDirection;
                    }
                }
            } else {
                if (random(0, 100) < 45) {
                    transitioning = true;
                    transitionStartTime = now;
                    transitionFromX = currentX;
                    transitionToX = homeX;
                    transitionToFacingRight = !grassDirection;
                }
            }

            walkLook.grassWanderTimer = now;
            walkLook.grassWanderInterval = random(3000, 8000);
        }
    }

    bool shouldBlink = faceTimer.blinking && currentState != AvatarState::SLEEPY;
    drawFrame(canvas, shouldBlink, walkLook.facingRight, isSniffing);
}

// ==[ BODY ANIM TABLE ]== data-driven body animation definitions
// Curve types for evaluateBodyAnim
enum class AnimCurve : uint8_t { NONE, ARC, JITTER, OSCILLATE, STEP };

struct BodyAnimDef {
    uint16_t durationMs;    // 0 = custom lifetime (ATTACK_HOP/ATTACK_TREE manage expiry)
    int8_t maxShakeY;       // peak Y offset (negative = up)
    int8_t maxOffsetX;      // peak X offset
    AnimCurve yCurve;       // how Y offset evolves over time
    AnimCurve xCurve;       // how X offset evolves over time
    bool airborne;          // suppress legs
    bool flipsFacing;       // SPIN alternates direction
    uint8_t flipCount;      // number of direction flips (SPIN)
};

static int deterministicShake(uint32_t bucket, int amplitude, uint32_t salt) {
    uint32_t h = bucket * 0x9E3779B9u ^ salt;
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    return (h & 1u) ? amplitude : -amplitude;
}

// Table indexed by BodyAnim enum value
static constexpr BodyAnimDef BODY_ANIM_DEFS[] = {
    // IDLE:        no motion
    { 0, 0, 0, AnimCurve::NONE, AnimCurve::NONE, false, false, 0 },
    // BUTT_FLEX:   rear view twerk wiggle
    { 1200, -3, 4, AnimCurve::OSCILLATE, AnimCurve::OSCILLATE, false, false, 0 },
    // PAW_SCRATCH: X oscillation when bored
    { 800, 0, 2, AnimCurve::NONE, AnimCurve::JITTER, false, false, 0 },
    // PERK_UP:     ears pop + bounce
    { 200, -2, 0, AnimCurve::ARC, AnimCurve::NONE, false, false, 0 },
    // CUTE_JUMP:   celebration bounce
    { 400, -5, 0, AnimCurve::ARC, AnimCurve::NONE, true, false, 0 },
    // FLINCH:      duck + jitter
    { 300, 3, 0, AnimCurve::STEP, AnimCurve::JITTER, false, false, 0 },
    // SPIN:        rapid direction flips (level-up)
    { 600, -5, 0, AnimCurve::ARC, AnimCurve::NONE, true, true, 4 },
    // ATTACK_HOP:  custom managed
    { 0, 0, 0, AnimCurve::NONE, AnimCurve::NONE, false, false, 0 },
    // ATTACK_TREE: custom managed
    { 0, 0, 0, AnimCurve::NONE, AnimCurve::NONE, false, false, 0 },
};

// Evaluate body anim offsets from table definition
// Returns true if this anim handled shakeY/startX (bodyHandledShake)
static bool evaluateBodyAnim(const BodySlot& slot, uint32_t now,
                             int& shakeY, int& startX) {
    if (!slot.active()) return false;

    uint8_t idx = (uint8_t)slot.anim;
    if (idx >= sizeof(BODY_ANIM_DEFS) / sizeof(BODY_ANIM_DEFS[0])) return false;

    const BodyAnimDef& def = BODY_ANIM_DEFS[idx];

    // Custom anims (ATTACK_HOP, ATTACK_TREE) not handled by table
    if (def.durationMs == 0 && slot.anim != BodyAnim::IDLE) {
        // ATTACK_HOP/ATTACK_TREE handled separately in buildIdlePose
        if (slot.anim == BodyAnim::ATTACK_HOP || slot.anim == BodyAnim::ATTACK_TREE)
            return true;  // signal "handled" — actual values computed in buildIdlePose attack section
        return false;
    }
    if (slot.anim == BodyAnim::IDLE) return false;

    uint32_t elapsed = now - slot.startTime;
    float t = (def.durationMs > 0) ? (float)elapsed / (float)def.durationMs : 0.0f;
    if (t > 1.0f) t = 1.0f;

    // Y curve
    switch (def.yCurve) {
        case AnimCurve::ARC:
            shakeY = -(int)(4.0f * t * (1.0f - t) * (float)(-def.maxShakeY));
            break;
        case AnimCurve::OSCILLATE: {
            float phase = (float)elapsed / 200.0f;
            float hopPhase = fmodf(phase, 2.0f);
            shakeY = (int)(sinf(hopPhase * 3.14159f) * (float)def.maxShakeY);
            break;
        }
        case AnimCurve::STEP:
            if (elapsed < 150) {
                shakeY = def.maxShakeY;
            } else {
                int amp = def.maxShakeY * 2 / 3;
                shakeY = deterministicShake((elapsed - 150u) / 45u, amp,
                                             slot.startTime ^ 0xF11C4u);
            }
            break;
        default:
            break;
    }

    // X curve
    switch (def.xCurve) {
        case AnimCurve::JITTER:
            startX += ((elapsed / 100) % 2 == 0) ? def.maxOffsetX : -def.maxOffsetX;
            break;
        case AnimCurve::OSCILLATE: {
            float phase = (float)elapsed / 200.0f;
            startX += (int)(sinf(phase * 3.14159f) * (float)def.maxOffsetX);
            break;
        }
        default:
            break;
    }

    // Spin facing flips
    if (def.flipsFacing && def.flipCount > 0) {
        uint16_t flipMs = def.durationMs / def.flipCount;
        if (flipMs > 0) {
            uint8_t flipPhase = elapsed / flipMs;
            walkLook.facingRight = (flipPhase % 2 == 0);
        }
    }

    return true;
}

// Get duration from table for auto-expiry
static uint16_t getBodyAnimDuration(BodyAnim anim) {
    uint8_t idx = (uint8_t)anim;
    if (idx >= sizeof(BODY_ANIM_DEFS) / sizeof(BODY_ANIM_DEFS[0])) return 0;
    return BODY_ANIM_DEFS[idx].durationMs;
}

// ==[ PHASE 3a: ANIM STATE UPDATE ]== pure simulation, no canvas
void Avatar::updateAnimState(uint32_t now) {
    // Watchdog: auto-disable attack shake after 250ms
    if (attack.shakeRefreshTime == 0 || (now - attack.shakeRefreshTime) > 250) {
        attack.shakeActive = false;
        attack.shakeAmplitude = 0.0f;
    }

    // === Attack hop animation update (body channel: ATTACK_HOP / ATTACK_TREE) ===
    bool isTreeAttack = (Avatar::body.anim == BodyAnim::ATTACK_TREE);
    if (Avatar::body.anim == BodyAnim::ATTACK_HOP || isTreeAttack) {
        uint32_t hopElapsed = now - Avatar::body.startTime;

        auto getHopMs = [isTreeAttack](uint8_t idx) -> uint16_t {
            if (!isTreeAttack) return Avatar::ATTACK_HOP_MS;
            return (idx == 0) ? Avatar::TREE_CHARGE_MS : Avatar::TREE_RETREAT_MS;
        };

        uint32_t totalHopTime = 0;
        for (uint8_t h = 0; h < Avatar::attackHopTotal; h++) totalHopTime += getHopMs(h);

        if (hopElapsed >= totalHopTime) {
            Avatar::body.clear();
            posControl.release(PosOwner::ATTACK_HOP);
            treeImpactTriggered = false;
            Avatar::currentX = Avatar::attackHopOriginX;
            if (Avatar::grassMoving) walkLook.facingRight = !Avatar::grassDirection;
        } else {
            uint32_t acc = 0;
            uint8_t hopIdx = 0;
            for (uint8_t h = 0; h < Avatar::attackHopTotal; h++) {
                uint32_t dur = getHopMs(h);
                if (hopElapsed < acc + dur) { hopIdx = h; break; }
                acc += dur;
            }
            Avatar::attackHopIndex = hopIdx;

            uint16_t hopMs = getHopMs(hopIdx);
            float hopT = (float)(hopElapsed - acc) / (float)hopMs;
            float smoothT = hopT * hopT * (3.0f - 2.0f * hopT);
            int16_t fromX = (hopIdx == 0) ? Avatar::attackHopOriginX : Avatar::attackHopTargets[hopIdx - 1];
            int16_t toX = Avatar::attackHopTargets[hopIdx];
            Avatar::currentX = fromX + (int)((toX - fromX) * smoothT);
            walkLook.facingRight = (toX > fromX);

            if (isTreeAttack && hopIdx == 0 && hopT > 0.85f && !treeImpactTriggered) {
                treeImpactTriggered = true;
                Avatar::waveRipple(WaveMode::OUTGOING, 3);
                SFX::play(SFX::OINK_GRUNT);
            }
            if (isTreeAttack && hopIdx == 1) {
                walkLook.facingRight = (Avatar::attackHopOriginX > Avatar::attackHopTargets[0]);
            }
        }
    }

    // --- Tree-pig collision detection ---
    collision.treeColliding = false;
    collision.treeCollisionShake = 0;
    if (Avatar::treePhase == TreePhase::ALIVE || Avatar::treePhase == TreePhase::GROWING) {
        int16_t tbx = Avatar::treeTrunk.baseX + Avatar::treeScrollOffset;
        while (tbx > SCREEN_WIDTH + 20) tbx -= (SCREEN_WIDTH + 80);
        while (tbx < -80) tbx += (SCREEN_WIDTH + 80);
        int16_t treeLeft  = tbx - Avatar::treeTrunk.crownRadius;
        int16_t treeRight = tbx + Avatar::treeTrunk.crownRadius;
        int16_t pigL = Avatar::currentX + PIG_PX * 6;
        int16_t pigR = Avatar::currentX + PIG_BODY_W - PIG_PX * 6;
        if (pigR > treeLeft && pigL < treeRight) {
            collision.treeColliding = true;
            collision.treeCollisionShake = ((now / 33) % 2 == 0) ? PX : -PX;
        }
    }

    if (collision.treeColliding && !collision.wasTreeColliding) {
        SFX::play(SFX::OINK_GRUNT);
    }
    collision.wasTreeColliding = collision.treeColliding;
}

// ==[ PHASE 3b: BUILD POSE ]== populate PigRenderPose from current state
PigRenderPose Avatar::buildIdlePose(uint32_t now, bool blink, bool faceRight, bool sniff) {
    // ==[ BODY ANIM ]== table-driven Y/X offset computation
    int shakeY = 0;
    int startX = Avatar::currentX;
    bool bodyHandledShake = evaluateBodyAnim(Avatar::body, now, shakeY, startX);

    // Attack hop/tree: custom arc Y (depends on multi-hop index)
    if (bodyHandledShake && (Avatar::body.anim == BodyAnim::ATTACK_HOP ||
                             Avatar::body.anim == BodyAnim::ATTACK_TREE)) {
        uint32_t elapsed = now - Avatar::body.startTime;
        bool isTree = (Avatar::body.anim == BodyAnim::ATTACK_TREE);
        uint32_t hopStart = 0;
        for (uint8_t h = 0; h < Avatar::attackHopIndex; h++) {
            hopStart += isTree ? (h == 0 ? Avatar::TREE_CHARGE_MS : Avatar::TREE_RETREAT_MS) : Avatar::ATTACK_HOP_MS;
        }
        uint16_t hopMs = isTree
            ? (Avatar::attackHopIndex == 0 ? Avatar::TREE_CHARGE_MS : Avatar::TREE_RETREAT_MS)
            : Avatar::ATTACK_HOP_MS;
        uint32_t hopLocal = elapsed - hopStart;
        float t = (float)hopLocal / (float)hopMs;
        float arc = 4.0f * t * (1.0f - t);
        int16_t height = Avatar::ATTACK_HOP_HEIGHT;
        if (isTree && Avatar::attackHopIndex == 1) height = 6;
        shakeY = -(int)(arc * height);
    }

    const uint8_t walkFrame = (uint8_t)((now / 120u) % 8u);

    // fallback shakeY when body channel idle (overlays + ambient)
    if (!bodyHandledShake) {
        if (attack.shakeActive) {
            int amp = 2 + (int)(attack.shakeAmplitude * 6.0f);
            amp = (amp / PIG_PX) * PIG_PX;
            if (amp < PIG_PX) amp = PIG_PX;
            shakeY = deterministicShake(now / 40u, amp, 0xA77AC4u);
        } else if (collision.treeColliding) {
            shakeY = ((now / 40) % 3 == 0) ? -2 : ((now / 40) % 3 == 1) ? 2 : 0;
        } else if (Avatar::transitioning || Avatar::grassMoving) {
            static constexpr int bouncePattern[8] = {
                0, -PIG_PX, 0, -PIG_PX, 0, -PIG_PX, 0, -PIG_PX
            };
            shakeY = bouncePattern[walkFrame];
        } else {
            shakeY = 0;
        }
    }

    if (collision.treeColliding) {
        startX += ((now / 50) % 2 == 0) ? PIG_PX : -PIG_PX;

        if (attack.shakeActive && now - collision.lastTreeShakeSparkle >= 250) {
            collision.lastTreeShakeSparkle = now;
            int16_t tbx = Avatar::treeTrunk.baseX + Avatar::treeScrollOffset;
            while (tbx > SCREEN_WIDTH + 20) tbx -= (SCREEN_WIDTH + 80);
            while (tbx < -80) tbx += (SCREEN_WIDTH + 80);
            int16_t spawnX = (Avatar::currentX + PIG_BODY_W / 2 + tbx) / 2;
            Avatar::triggerSparkles(2);
            for (int i = 0; i < Avatar::MAX_SPARKLES; i++) {
                if (Avatar::sparkles[i].life > 14) {
                    Avatar::sparkles[i].x = spawnX + random(-PIG_PX * 4, PIG_PX * 4 + 1);
                    Avatar::sparkles[i].y = PIG_Y + PIG_PX * 4 + random(-PIG_PX * 5, PIG_PX * 4);
                }
            }
        }
    }

    // Walk / idle state for legs + lean
    bool isWalking = (Avatar::transitioning || Avatar::grassMoving || Avatar::pendingGrassStart);
    // currentX is the shared body anchor for both mirrored poses. Do not nudge
    // stationary frames by facing or a look flip becomes a 4px position jump.

    // ==[ CINEMATIC OVERRIDE ]== supreme lock — position/facing/shake
    if (Avatar::cinematicPose.active) {
        if (Avatar::cinematicPose.forceX >= 0) Avatar::currentX = Avatar::cinematicPose.forceX;
        if (Avatar::cinematicPose.facingDir != 0) walkLook.facingRight = (Avatar::cinematicPose.facingDir > 0);
        if (Avatar::cinematicPose.shakeY != 0) shakeY = Avatar::cinematicPose.shakeY;
        startX = Avatar::currentX + Avatar::cinematicPose.offsetX;
    }

    startX = snapToPx((int16_t)startX, PIG_PX);
    int startY = snapToPx((int16_t)(PIG_Y + shakeY), PIG_PX);
    shakeY = startY - PIG_Y;

    int bellyBreathePx = 0;  // idle breathe disabled — pig has enough life without it

    updateHypeFill();

    bool actionEyeUp =
        (Avatar::body.anim == BodyAnim::PERK_UP ||
         Avatar::body.anim == BodyAnim::FLINCH ||
         Avatar::body.anim == BodyAnim::SPIN ||
         Avatar::body.anim == BodyAnim::ATTACK_HOP ||
         Avatar::body.anim == BodyAnim::ATTACK_TREE ||
         Avatar::getWaveMode() == WaveMode::OUTGOING);

    // ==[ BUILD PIG RENDER POSE ]==
    PigRenderPose renderPose;
    renderPose.x = startX;
    renderPose.y = startY;
    renderPose.facing = Avatar::facingAway ? PigFacing::REAR : (faceRight ? PigFacing::RIGHT : PigFacing::LEFT);
    renderPose.expression = PigExpression::fromState(Avatar::currentState, blink, sniff, sniffAnim.frame,
                                                      faceTimer.earTwitching || Avatar::body.anim == BodyAnim::PERK_UP);
    renderPose.shakeY = (int16_t)shakeY;
    renderPose.bellyBreathePx = (int16_t)bellyBreathePx;
    renderPose.scale = PIG_PX;
    renderPose.walkFrame = walkFrame;
    renderPose.talking = Mood::hasPhrase();
    // IDLE/HUNT now render over the layered street, not a flat theme fill.
    // Sample that substrate for rounded body cuts so it cannot become a dark
    // halo. Foreground grass is composited after the pig and keeps the planted
    // contact edge intact.
    renderPose.blendRounding = true;
    renderPose.eyeLook = resolveIdleEyeLook(now, Avatar::currentState, Avatar::facingAway, isWalking,
                                            sniff, blink, renderPose.talking,
                                            actionEyeUp);
    Display::PigPalette pigPalette = Display::makePigPalette(getDrawColor(), getBGColor());
    renderPose.detailColor = pigPalette.detail;

    // Determine tail glyph and side
    if (Avatar::facingAway) {
        renderPose.tailGlyph = (Avatar::tailWiggleActive && ((now / 120) % 2 == 0)) ? '~' : 'z';
        renderPose.tailOnLeft = false;
    } else {
        renderPose.tailGlyph = 'z';
        if (Avatar::grassMoving || Avatar::pendingGrassStart) {
            renderPose.tailOnLeft = faceRight;
        } else if (Avatar::transitioning) {
            renderPose.tailOnLeft = (Avatar::transitionToX > Avatar::transitionFromX);
        } else {
            if (Avatar::tailWiggleActive) {
                if ((now - Avatar::tailWiggleStart) < Avatar::TAIL_WIGGLE_DURATION_MS) {
                    renderPose.tailGlyph = ((now / 120) % 2 == 0) ? 'z' : '~';
                } else {
                    Avatar::tailWiggleActive = false;
                }
            }
            renderPose.tailOnLeft = faceRight;
        }
    }

    // Determine limb mode
    bool isAirborne = (Avatar::body.anim == BodyAnim::CUTE_JUMP || Avatar::body.anim == BodyAnim::SPIN);
    if (isAirborne) {
        renderPose.limbMode = LimbMode::AIRBORNE;
    } else if (Avatar::facingAway) {
        renderPose.limbMode = LimbMode::REAR_NUBS;
    } else if (isWalking) {
        renderPose.limbMode = LimbMode::WALKING_IDLE;
    } else {
        renderPose.limbMode = LimbMode::STANDING;
    }

    return renderPose;
}

// ==[ PHASE 3c: RENDER PIG SCENE ]== composite all layers onto canvas
void Avatar::renderPigScene(M5Canvas& canvas, const PigRenderPose& pose, uint32_t now) {
    Avatar::drawTree(canvas);  // Fruit tree behind pig

    canvas.setTextDatum(TL_DATUM);
    canvas.setTextSize(PIG_PX);
    canvas.setTextColor(getDrawColor());

    bool faceRight = (pose.facing == PigFacing::RIGHT);

    // Draw wave ripples behind pig
    Avatar::drawWaveRipples(canvas, faceRight, pose.x, pose.y);

    // Advance grass once, then draw parallax back-layers behind pig
    Avatar::updateGrass();
    {
        uint32_t grassNow = millis();
        uint16_t grassColor = getDrawColor();
        const int16_t baseY = GRASS_BASE_Y_CONST + Avatar::grassYOffset;
        const int16_t backLayerLift = 6;
        const int16_t baseY2 = baseY - backLayerLift;
        const int16_t baseY3 = baseY - (backLayerLift * 2);
        int16_t layer2Offset = Avatar::getGrassOffsetLayer2() + (Avatar::GRASS_STRIDE / 2);
        if (layer2Offset >= Avatar::GRASS_STRIDE) layer2Offset -= Avatar::GRASS_STRIDE;
        int16_t layer3Offset = Avatar::getGrassOffsetLayer3() + (Avatar::GRASS_STRIDE / 4);
        if (layer3Offset >= Avatar::GRASS_STRIDE) layer3Offset -= Avatar::GRASS_STRIDE;
        Avatar::drawGrassLayer(canvas, grassNow, grassColor, baseY3, layer3Offset, PX, 1, 1, false, false);
        Avatar::drawGrassLayer(canvas, grassNow, grassColor, baseY2, layer2Offset, PX, 1, 1, false, false);
    }

    Display::PigPalette pigPalette = Display::makePigPalette(getDrawColor(), getBGColor());

    // ==[ RENDER ]== composite body+bridge+limbs in one coarse pass
    drawPigFull(canvas, pose, pigPalette.bodyFill, pigPalette.voidColor, now);

    // ==[ CURLY HAIRS ]==
    bool rearView = pose.facing == PigFacing::REAR;
    Avatar::updateHairPhysics(Avatar::currentX, pose.shakeY, Avatar::currentState);
    Avatar::drawHairs(canvas, pose.x, pose.y, pose.shakeY, faceRight,
                      Avatar::currentState, rearView);

    // Draw sparkle particles
    Avatar::updateAndDrawSparkles(canvas);

    // Draw grass below piglet
    Avatar::drawGrass(canvas);
}

// ==[ drawFrame — orchestrator ]== 3-phase pipeline
void Avatar::drawFrame(M5Canvas& canvas, bool blink, bool faceRight, bool sniff) {
    uint32_t now = millis();
    updateAnimState(now);
    currentX = snapToPx((int16_t)currentX, PIG_PX);
    PigRenderPose pose = buildIdlePose(now, blink, faceRight, sniff);
    renderPigScene(canvas, pose, now);
}


// ==[ CINEMATIC OVERRIDE ]== Weather/Mood choreography layer
void Avatar::setCinematicPose(const CinematicPose& pose) {
    cinematicPose = pose;
    if (pose.suppressBody) body.clear();
    posControl.claim(PosOwner::CINEMATIC);
}

void Avatar::clearCinematic() {
    cinematicPose.active = false;
    posControl.release(PosOwner::CINEMATIC);
}

bool Avatar::isCinematic() {
    return cinematicPose.active;
}

// --- Night time forwarding (PigStars owns the logic) ---
bool Avatar::isNightTime() { return PigStars::isNightTime(); }

// --- Direction control ---
void Avatar::setFacingLeft() {
    walkLook.facingRight = false;
}

// --- Attack shake control ---
void Avatar::setAttackShake(bool active, bool strong) {
    attack.shakeActive = active;
    attack.shakeAmplitude = strong ? 1.0f : 0.4f;
    attack.shakeRefreshTime = active ? millis() : 0;
}

void Avatar::setAttackShakeSmooth(bool active, float amplitude) {
    attack.shakeActive = active;
    attack.shakeAmplitude = amplitude;
    attack.shakeRefreshTime = active ? millis() : 0;
}

// --- Thunder flash control ---
void Avatar::setThunderFlash(bool active) {
    thunderFlashActive = active;
}

// --- Hype gate control ---
void Avatar::unlockHype() {
    hype.unlocked = true;
    // start bottom-to-top rainbow fill sweep across pig body
    hype.fillActive = true;
    hype.fillStart = millis();
    hype.fillSeed = (uint8_t)esp_random();
    memset(hype.fillCount, 0, sizeof(hype.fillCount));
}


// --- Windup slide for coast-back ---
void Avatar::startWindupSlide(int targetX, bool faceRight) {
    if (currentX != targetX) {
        transitioning = true;
        transitionFromX = currentX;
        transitionToX = targetX;
        transitionStartTime = millis();
        transitionToFacingRight = faceRight;
        posControl.claim(PosOwner::WALK_TRANS);

        // Dust burst on walk start
        for (int b = 0; b < 4; b++) {
            TrailParticle& p = trailParticles[trailSpawnIdx];
            trailSpawnIdx = (trailSpawnIdx + 1) % TRAIL_COUNT;
            p.x = (float)(currentX + PIG_BODY_W * 5 / 9 + random(-15, 16));
            p.y = (float)(GRASS_BASE_Y_CONST - 10 + random(0, 8));
            p.vx = (float)(random(-20, 21)) / 10.0f;
            p.vy = -(0.5f + (float)random(0, 10) / 10.0f);
            p.startX = p.x;
            p.maxDist = 20.0f + (float)random(0, 21);
            p.baseSize = random(1, 3);
            p.active = true;
        }
    }
    walkLook.facingRight = faceRight;
}

// ==[ WALK FSM ]== replaces scattered windup logic from hamlet.cpp
void Avatar::updateWalk(bool isWalkingNow, uint32_t now) {
    // detect walk start
    if (isWalkingNow && !wasWalking && walkPhase == WalkPhase::STOPPED) {
        walkPhase = WalkPhase::SLIDE;
        walkPhaseStart = now;
        Mood::onWalkStart();
    }

    // walk stopped mid-windup — abort
    if (!isWalkingNow && walkPhase != WalkPhase::STOPPED && walkPhase != WalkPhase::WALKING) {
        walkPhase = WalkPhase::STOPPED;
        setGrassMoving(false, true);
    }

    switch (walkPhase) {
        case WalkPhase::SLIDE: {
            uint32_t e = now - walkPhaseStart;
            if (e < 450) {
                if (!transitioning) {
                    startWindupSlide(PIG_CENTER_X, PIG_CENTER_X > currentX);
                }
            } else {
                walkPhase = WalkPhase::FLIP;
                walkPhaseStart = now;
                setFacingLeft();
            }
            break;
        }
        case WalkPhase::FLIP:
            if (now - walkPhaseStart >= 50) {
                walkPhase = WalkPhase::WALKING;
                setGrassMoving(true, true);
                setFacingLeft();
            }
            break;
        case WalkPhase::WALKING:
            if (!isWalkingNow) {
                walkPhase = WalkPhase::STOPPED;
                setGrassMoving(false, true);
            }
            break;
        case WalkPhase::STOPPED:
        default:
            // steady state — tie parallax directly to input
            if (isWalkingNow) {
                setGrassMoving(true, true);
            } else {
                setGrassMoving(false, true);
            }
            break;
    }

    wasWalking = isWalkingNow;
}
