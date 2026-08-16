#include "pancetta_cat_animation.h"

namespace PancettaCat {
namespace Animation {
namespace {

using S = Rig::Stance;
using G = Rig::Gait;
using H = Rig::HeadPose;
using T = Rig::TailPose;
using E = Rig::EarPose;
using I = Rig::EyePose;
using F = Rig::FurPose;
using M = Rig::MouthPose;

#define CAT_KEY(ms, tween, ...) \
    {Rig::Pose(__VA_ARGS__), (uint16_t)(ms), Tween::tween}

// Locomotion loads all four stubbies, takes one tiny diagonal hop, then lands
// on the opposite pair. The two-cell rise is measured from the compressed
// plant rather than lifting the neutral body out of its guarded envelope.
// This keeps the gait feline and buoyant without exposing articulated shins.
static constexpr Keyframe kFollowWalkKeys[] = {
    CAT_KEY(110, EASE, S::CROUCH, G::STRIDE_PLANT, H::DOWN, T::UP,
            E::FORWARD, I::OPEN, F::COMPRESS, 0, 1, 0, 1, -1, M::CLOSED),
    CAT_KEY(90, EASE, S::STRETCH, G::BOUND_EXTEND, H::LEVEL, T::LEVEL,
            E::FORWARD, I::WIDE, F::DRAG_BACK, 0, 0, 0, 0, 0, M::CLOSED),
    CAT_KEY(110, EASE, S::STAND, G::BOUND_TUCK, H::UP, T::LEVEL,
            E::ALERT, I::WIDE, F::DRAG_BACK, 0, 0, 0, -1, 0, M::CLOSED),
    CAT_KEY(90, EASE, S::CROUCH, G::STEP_A, H::LEVEL, T::UP,
            E::FORWARD, I::OPEN, F::BREATHE, 0, 1, 0, 0, 0, M::CLOSED),
    CAT_KEY(110, EASE, S::CROUCH, G::STRIDE_PLANT, H::DOWN, T::UP,
            E::FORWARD, I::SOFT, F::COMPRESS, 0, 1, 0, 1, -1, M::CLOSED),
    CAT_KEY(90, EASE, S::STRETCH, G::BOUND_EXTEND, H::LEVEL, T::LEVEL,
            E::FORWARD, I::WIDE, F::DRAG_BACK, 0, 0, 0, 0, 0, M::CLOSED),
    CAT_KEY(110, EASE, S::STAND, G::BOUND_TUCK, H::UP, T::LEVEL,
            E::ALERT, I::WIDE, F::DRAG_BACK, 1, 0, 0, -1, 0, M::CLOSED),
    CAT_KEY(90, EASE, S::CROUCH, G::STEP_B, H::LEVEL, T::UP,
            E::FORWARD, I::OPEN, F::BREATHE, 0, 1, 0, 0, 0, M::CLOSED),
};

static constexpr Keyframe kFollowIdleKeys[] = {
    CAT_KEY(4200, HOLD, S::STAND, G::PLANT, H::LEVEL, T::UP,
            E::NEUTRAL, I::OPEN, F::REST, 0, 0, 0, 0, 0, M::CLOSED),
    CAT_KEY(320, EASE, S::STAND, G::PLANT, H::LEVEL, T::UP,
            E::ALERT, I::WIDE, F::BREATHE, 0, 0, 0, -1, 1, M::CLOSED),
    CAT_KEY(3000, HOLD, S::STAND, G::PLANT, H::LEVEL, T::CURL,
            E::FORWARD, I::OPEN, F::REST, 0, 0, 0, 0, 0, M::CLOSED),
    CAT_KEY(420, EASE, S::STAND, G::PLANT, H::DOWN, T::CURL,
            E::FORWARD, I::SOFT, F::BREATHE, 0, 0, 0, 0, -1, M::CLOSED),
    CAT_KEY(3600, HOLD, S::STAND, G::PLANT, H::LEVEL, T::UP,
            E::NEUTRAL, I::OPEN, F::REST, 0, 0, 0, 0, 0, M::CLOSED),
    CAT_KEY(700, EASE, S::SIT, G::PLANT, H::FRONT, T::CURL,
            E::ALERT, I::SOFT, F::BREATHE, 0, 0, 0, 0, 1, M::CLOSED),
};

static constexpr Keyframe kCableKeys[] = {
    CAT_KEY(4200, HOLD, S::CROUCH, G::PLANT, H::DOWN, T::LEVEL,
            E::FORWARD, I::OPEN, F::REST, 0, 0, 0, 0, 0, M::CLOSED),
    CAT_KEY(460, EASE, S::CROUCH, G::PAW_LIFT, H::DOWN, T::LEVEL,
            E::ALERT, I::WIDE, F::SWEEP_FORWARD, 0, 0, 0, 0, 0, M::CLOSED),
    CAT_KEY(2800, HOLD, S::CROUCH, G::PLANT, H::LEVEL, T::UP,
            E::FORWARD, I::OPEN, F::REST, 0, 0, 0, 0, 0, M::CLOSED),
    CAT_KEY(520, EASE, S::CROUCH, G::PAW_LIFT, H::DOWN, T::CURL,
            E::FORWARD, I::SOFT, F::BREATHE, 0, 0, 0, 1, -1, M::CLOSED),
    CAT_KEY(3800, HOLD, S::CROUCH, G::PLANT, H::DOWN, T::LEVEL,
            E::NEUTRAL, I::OPEN, F::REST, 0, 0, 0, 0, 0, M::CLOSED),
};

static constexpr Keyframe kRainKeys[] = {
    CAT_KEY(5000, HOLD, S::SIT, G::PLANT, H::UP, T::CURL,
            E::ALERT, I::OPEN, F::REST, 0, 0, 0, 0, 0, M::CLOSED),
    CAT_KEY(600, EASE, S::SIT, G::PLANT, H::FRONT, T::CURL,
            E::FORWARD, I::SOFT, F::BREATHE, 0, 0, -1, 0, 1, M::CLOSED),
    CAT_KEY(3200, HOLD, S::SIT, G::PLANT, H::UP, T::UP,
            E::ALERT, I::OPEN, F::REST, 0, 0, 0, -1, 0, M::CLOSED),
    CAT_KEY(640, EASE, S::SIT, G::PLANT, H::LEVEL, T::CURL,
            E::BACK, I::WIDE, F::SWEEP_FORWARD, 0, 0, 0, 1, 0, M::CLOSED),
    CAT_KEY(4800, HOLD, S::SIT, G::PLANT, H::UP, T::CURL,
            E::ALERT, I::OPEN, F::REST, 0, 0, 0, 0, 0, M::CLOSED),
};

static constexpr Keyframe kFoodKeys[] = {
    CAT_KEY(2600, HOLD, S::CROUCH, G::PLANT, H::DOWN, T::LEVEL,
            E::FORWARD, I::OPEN, F::REST, 0, 0, 0, 0, 0, M::CLOSED),
    CAT_KEY(320, EASE, S::CROUCH, G::PLANT, H::DOWN, T::LEVEL,
            E::FORWARD, I::SOFT, F::BREATHE, 1, 1, 1, 1, -1, M::CLOSED),
    CAT_KEY(260, EASE, S::CROUCH, G::PLANT, H::DOWN, T::CURL,
            E::ALERT, I::OPEN, F::SWEEP_FORWARD, 0, 0, 2, 2, 0, M::CLOSED),
    CAT_KEY(520, EASE, S::CROUCH, G::PLANT, H::DOWN, T::LEVEL,
            E::FORWARD, I::SOFT, F::BREATHE, 1, 1, 1, 1, -1, M::CLOSED),
    CAT_KEY(300, EASE, S::CROUCH, G::PLANT, H::DOWN, T::CURL,
            E::ALERT, I::OPEN, F::SWEEP_FORWARD, 0, 0, 2, 2, 0, M::CLOSED),
    CAT_KEY(3400, HOLD, S::CROUCH, G::PLANT, H::LEVEL, T::UP,
            E::NEUTRAL, I::OPEN, F::REST, 0, 0, 0, 0, 0, M::CLOSED),
};

static constexpr Keyframe kSkyKeys[] = {
    CAT_KEY(3200, HOLD, S::CROUCH, G::PLANT, H::UP, T::TRAIL_LOW,
            E::ALERT, I::OPEN, F::REST, 0, 0, 0, 0, 0, M::CLOSED),
    CAT_KEY(420, EASE, S::CROUCH, G::STEP_A, H::UP, T::LEVEL,
            E::FORWARD, I::WIDE, F::DRAG_BACK, 0, -1, 0, -1, 0, M::CLOSED),
    CAT_KEY(360, EASE, S::STRETCH, G::BOUND_TUCK, H::UP, T::LEVEL,
            E::ALERT, I::WIDE, F::DRAG_BACK, 0, -2, 0, 0, 0, M::CLOSED),
    CAT_KEY(780, HOLD, S::CROUCH, G::PLANT, H::UP, T::UP,
            E::ALERT, I::OPEN, F::BREATHE, 0, 0, 0, 0, 1, M::CLOSED),
    CAT_KEY(420, EASE, S::CROUCH, G::STEP_B, H::LEVEL, T::LEVEL,
            E::FORWARD, I::SOFT, F::SWEEP_FORWARD, 0, 0, 0, 0, 0, M::CLOSED),
    CAT_KEY(3000, HOLD, S::CROUCH, G::PLANT, H::UP, T::TRAIL_LOW,
            E::ALERT, I::OPEN, F::REST, 0, 0, 0, 0, 0, M::CLOSED),
};

static constexpr Keyframe kProwlKeys[] = {
    CAT_KEY(280, EASE, S::CROUCH, G::STEP_A, H::LEVEL, T::LEVEL,
            E::FORWARD, I::OPEN, F::DRAG_BACK, 0, 0, 0, 0, 0, M::CLOSED),
    CAT_KEY(260, EASE, S::CROUCH, G::PLANT, H::DOWN, T::TRAIL_LOW,
            E::ALERT, I::OPEN, F::BREATHE, 0, 0, 0, 0, -1, M::CLOSED),
    CAT_KEY(280, EASE, S::CROUCH, G::STEP_B, H::LEVEL, T::LEVEL,
            E::FORWARD, I::OPEN, F::DRAG_BACK, 0, 0, 0, 0, 0, M::CLOSED),
    CAT_KEY(260, EASE, S::CROUCH, G::PLANT, H::LEVEL, T::CURL,
            E::NEUTRAL, I::SOFT, F::BREATHE, 0, 0, 0, 0, 1, M::CLOSED),
    CAT_KEY(280, EASE, S::CROUCH, G::STEP_A, H::LEVEL, T::LEVEL,
            E::FORWARD, I::OPEN, F::DRAG_BACK, 0, 0, 0, 0, 0, M::CLOSED),
    CAT_KEY(1000, HOLD, S::CROUCH, G::PLANT, H::FRONT, T::TRAIL_LOW,
            E::ALERT, I::WIDE, F::REST, 0, 0, 0, 0, 0, M::CLOSED),
};

static constexpr Keyframe kWaterKeys[] = {
    CAT_KEY(4200, HOLD, S::CROUCH, G::PLANT, H::DOWN, T::UP,
            E::FORWARD, I::OPEN, F::REST, 0, 0, 0, 0, 0, M::CLOSED),
    CAT_KEY(520, EASE, S::CROUCH, G::PAW_LIFT, H::DOWN, T::CURL,
            E::ALERT, I::WIDE, F::SWEEP_FORWARD, 0, 0, 0, 1, 0, M::CLOSED),
    CAT_KEY(1800, HOLD, S::CROUCH, G::PAW_LIFT, H::DOWN, T::CURL,
            E::FORWARD, I::SOFT, F::BREATHE, 0, 0, 0, 1, -1, M::CLOSED),
    CAT_KEY(480, EASE, S::CROUCH, G::PLANT, H::LEVEL, T::UP,
            E::BACK, I::WIDE, F::DRAG_BACK, -1, 0, -1, 0, 0, M::CLOSED),
    CAT_KEY(2600, HOLD, S::SIT, G::PLANT, H::DOWN, T::CURL,
            E::NEUTRAL, I::OPEN, F::REST, 0, 0, 0, 0, 0, M::CLOSED),
    CAT_KEY(3600, HOLD, S::CROUCH, G::PLANT, H::DOWN, T::UP,
            E::FORWARD, I::OPEN, F::REST, 0, 0, 0, 0, 0, M::CLOSED),
};

static constexpr Keyframe kScratchKeys[] = {
    CAT_KEY(700, HOLD, S::SIT, G::PLANT, H::UP, T::UP,
            E::FORWARD, I::OPEN, F::REST, 0, 0, 0, 0, 0, M::CLOSED),
    CAT_KEY(560, EASE, S::SCRATCH, G::SCRATCH_A, H::UP, T::UP,
            E::ALERT, I::OPEN, F::DRAG_BACK, 0, 0, 0, 0, 0, M::CLOSED),
    CAT_KEY(640, EASE, S::SCRATCH, G::SCRATCH_B, H::LEVEL, T::LEVEL,
            E::FORWARD, I::SOFT, F::BREATHE, 0, 0, 0, 1, -1, M::CLOSED),
    CAT_KEY(560, EASE, S::SCRATCH, G::SCRATCH_A, H::UP, T::UP,
            E::ALERT, I::OPEN, F::DRAG_BACK, 0, -1, 0, 0, 0, M::CLOSED),
    CAT_KEY(520, EASE, S::SCRATCH, G::SCRATCH_B, H::LEVEL, T::LEVEL,
            E::FORWARD, I::SOFT, F::BREATHE, 0, 0, 0, 1, 0, M::CLOSED),
    CAT_KEY(1200, HOLD, S::SIT, G::PLANT, H::FRONT, T::CURL,
            E::NEUTRAL, I::SOFT, F::REST, 0, 0, 0, 0, 0, M::CLOSED),
};

static constexpr Keyframe kSleepKeys[] = {
    CAT_KEY(1000, HOLD, S::SIT, G::PLANT, H::DOWN, T::CURL,
            E::NEUTRAL, I::SOFT, F::REST, 0, 0, 0, 0, 0, M::CLOSED),
    CAT_KEY(1600, HOLD, S::LOAF, G::PLANT, H::DOWN, T::TUCK,
            E::NEUTRAL, I::CLOSED, F::COMPRESS, 0, 0, 0, 0, 0, M::CLOSED),
    CAT_KEY(1800, EASE, S::CURL, G::PLANT, H::GROOM_FLANK, T::TUCK,
            E::NEUTRAL, I::CLOSED, F::BREATHE, 0, 0, 0, 0, 1, M::CLOSED),
    CAT_KEY(5200, HOLD, S::CURL, G::PLANT, H::GROOM_FLANK, T::TUCK,
            E::NEUTRAL, I::CLOSED, F::REST, 0, 0, 0, 0, 0, M::CLOSED),
    CAT_KEY(1600, EASE, S::CURL, G::PLANT, H::GROOM_FLANK, T::TUCK,
            E::NEUTRAL, I::CLOSED, F::BREATHE, 0, 0, 0, 0, -1, M::CLOSED),
    CAT_KEY(2400, HOLD, S::LOAF, G::PLANT, H::DOWN, T::CURL,
            E::NEUTRAL, I::CLOSED, F::REST, 0, 0, 0, 0, 0, M::CLOSED),
    CAT_KEY(4200, HOLD, S::LOAF, G::PLANT, H::DOWN, T::TUCK,
            E::NEUTRAL, I::CLOSED, F::BREATHE, 0, 0, 0, 0, 1, M::CLOSED),
};

static constexpr Keyframe kGroomKeys[] = {
    CAT_KEY(900, HOLD, S::SIT, G::PAW_LIFT, H::GROOM_PAW, T::CURL,
            E::NEUTRAL, I::SOFT, F::REST, 0, 0, 0, 0, 0, M::CLOSED),
    CAT_KEY(720, EASE, S::SIT, G::PAW_LIFT, H::GROOM_PAW, T::CURL,
            E::FORWARD, I::CLOSED, F::BREATHE, 0, 0, -1, 1, -1, M::CHIRP),
    CAT_KEY(760, EASE, S::SIT, G::PAW_LIFT, H::GROOM_PAW, T::CURL,
            E::NEUTRAL, I::SOFT, F::REST, 0, 0, 0, 0, 0, M::CLOSED),
    CAT_KEY(1100, HOLD, S::SIT, G::PLANT, H::GROOM_FLANK, T::UP,
            E::NEUTRAL, I::CLOSED, F::SWEEP_FORWARD, 0, 0, 0, 0, 0, M::CHIRP),
    CAT_KEY(700, EASE, S::SIT, G::PLANT, H::GROOM_FLANK, T::LEVEL,
            E::FORWARD, I::SOFT, F::BREATHE, 0, 0, 1, -1, 1, M::CLOSED),
    CAT_KEY(900, HOLD, S::SIT, G::PLANT, H::GROOM_FLANK, T::CURL,
            E::NEUTRAL, I::CLOSED, F::REST, 0, 0, 0, 0, 0, M::CHIRP),
    CAT_KEY(1800, HOLD, S::SIT, G::PLANT, H::FRONT, T::CURL,
            E::ALERT, I::OPEN, F::REST, 0, 0, 0, 0, 0, M::CLOSED),
};

static constexpr Keyframe kHairballKeys[] = {
    CAT_KEY(700, HOLD, S::CROUCH, G::PLANT, H::DOWN, T::TRAIL_LOW,
            E::BACK, I::SOFT, F::COMPRESS, 0, 1, 0, 0, 0, M::CLOSED),
    CAT_KEY(520, EASE, S::CROUCH, G::PLANT, H::GAG, T::TRAIL_LOW,
            E::BACK, I::CLOSED, F::SWEEP_FORWARD, -1, 1, 1, 1, -1, M::CHIRP),
    CAT_KEY(620, EASE, S::CROUCH, G::PLANT, H::GAG, T::TUCK,
            E::BACK, I::WIDE, F::BRISTLE, 0, 0, 2, 0, 0, M::CHIRP),
    CAT_KEY(760, EASE, S::CROUCH, G::PLANT, H::GAG, T::TRAIL_LOW,
            E::BACK, I::CLOSED, F::SWEEP_FORWARD, -1, 2, 2, 1, -1, M::CHIRP),
    CAT_KEY(680, EASE, S::CROUCH, G::PLANT, H::DOWN, T::TUCK,
            E::NEUTRAL, I::SOFT, F::COMPRESS, 0, 1, 0, 1, 0, M::CLOSED),
    CAT_KEY(560, EASE, S::SIT, G::PLANT, H::FRONT, T::CURL,
            E::ALERT, I::WIDE, F::BREATHE, 0, 0, 0, 0, 1, M::CLOSED),
    CAT_KEY(1400, HOLD, S::SIT, G::PLANT, H::LEVEL, T::CURL,
            E::NEUTRAL, I::SOFT, F::REST, 0, 0, 0, 0, 0, M::CLOSED),
};

static constexpr Keyframe kArchKeys[] = {
    CAT_KEY(600, HOLD, S::STAND, G::PLANT, H::LEVEL, T::UP,
            E::ALERT, I::WIDE, F::BREATHE, 0, 0, 0, 0, 0, M::CLOSED),
    CAT_KEY(560, EASE, S::ARCH, G::PLANT, H::LEVEL, T::PUFF,
            E::BACK, I::WIDE, F::BRISTLE, 0, 0, 0, 0, 0, M::CLOSED),
    CAT_KEY(920, HOLD, S::ARCH, G::STEP_A, H::FRONT, T::PUFF,
            E::BACK, I::WIDE, F::BRISTLE, 0, -1, 0, 0, 0, M::CHIRP),
    CAT_KEY(620, EASE, S::ARCH, G::STEP_B, H::LEVEL, T::PUFF,
            E::BACK, I::WIDE, F::BRISTLE, 0, 0, 0, 0, 0, M::CLOSED),
    CAT_KEY(520, EASE, S::STAND, G::PLANT, H::LEVEL, T::UP,
            E::ALERT, I::OPEN, F::SWEEP_FORWARD, 0, 0, 0, 0, -1, M::CLOSED),
    CAT_KEY(900, HOLD, S::STAND, G::PLANT, H::FRONT, T::CURL,
            E::NEUTRAL, I::SOFT, F::REST, 0, 0, 0, 0, 0, M::CLOSED),
};

static constexpr Keyframe kZoomKeys[] = {
    CAT_KEY(180, EASE, S::STRETCH, G::BOUND_EXTEND, H::LEVEL, T::LEVEL,
            E::BACK, I::WIDE, F::DRAG_BACK, 0, -2, 0, 0, 0, M::CLOSED),
    CAT_KEY(180, EASE, S::STRETCH, G::BOUND_TUCK, H::LEVEL, T::LEVEL,
            E::BACK, I::WIDE, F::DRAG_BACK, 1, -4, 0, 0, 0, M::CLOSED),
    CAT_KEY(200, EASE, S::STRETCH, G::BOUND_EXTEND, H::LEVEL, T::UP,
            E::FORWARD, I::WIDE, F::DRAG_BACK, 0, -3, 1, 0, 0, M::CLOSED),
    CAT_KEY(200, EASE, S::CROUCH, G::BOUND_TUCK, H::DOWN, T::LEVEL,
            E::BACK, I::OPEN, F::COMPRESS, 1, -1, 0, 0, -1, M::CLOSED),
    CAT_KEY(220, EASE, S::STRETCH, G::BOUND_EXTEND, H::LEVEL, T::LEVEL,
            E::FORWARD, I::WIDE, F::DRAG_BACK, 0, -2, 0, 0, 0, M::CLOSED),
    CAT_KEY(420, HOLD, S::CROUCH, G::PLANT, H::FRONT, T::CURL,
            E::ALERT, I::OPEN, F::SWEEP_FORWARD, 0, 0, 0, 0, 0, M::CLOSED),
};

// A face bump is one deliberate greeting, not a looped attack. The cat leans
// cheek-first, closes her eyes at contact, then keeps a soft crouch beside
// Pancetta for the remainder of the company visit.
static constexpr Keyframe kFaceBumpKeys[] = {
    CAT_KEY(700, HOLD, S::CROUCH, G::PLANT, H::LEVEL, T::UP,
            E::FORWARD, I::OPEN, F::REST, 0, 0, 0, 0, 0, M::CLOSED),
    CAT_KEY(520, EASE, S::CROUCH, G::STEP_A, H::LEVEL, T::CURL,
            E::FORWARD, I::SOFT, F::SWEEP_FORWARD, 1, 0, 2, 0, 0, M::CLOSED),
    CAT_KEY(900, HOLD, S::CROUCH, G::PLANT, H::LEVEL, T::CURL,
            E::NEUTRAL, I::CLOSED, F::BREATHE, 1, 0, 2, 0, 1, M::CLOSED),
    CAT_KEY(620, EASE, S::CROUCH, G::PLANT, H::FRONT, T::UP,
            E::FORWARD, I::SOFT, F::BREATHE, 0, 0, 0, 0, -1, M::CLOSED),
    CAT_KEY(3600, HOLD, S::CROUCH, G::PLANT, H::FRONT, T::CURL,
            E::NEUTRAL, I::SOFT, F::REST, 0, 0, 0, 0, 0, M::CLOSED),
};

// On a resting station the support is Pancetta's head. The clip settles from
// a careful sit into the same guarded curl as ordinary sleep and then stays
// there; navigation, not rendering, decides when the living pillow moves.
static constexpr Keyframe kHeadNapKeys[] = {
    CAT_KEY(800, HOLD, S::SIT, G::PLANT, H::DOWN, T::CURL,
            E::NEUTRAL, I::SOFT, F::REST, 0, 0, 0, 0, 0, M::CLOSED),
    CAT_KEY(900, EASE, S::LOAF, G::PLANT, H::DOWN, T::TUCK,
            E::FORWARD, I::CLOSED, F::COMPRESS, 0, 0, 0, 0, -1, M::CLOSED),
    CAT_KEY(1200, EASE, S::CURL, G::PLANT, H::GROOM_FLANK, T::TUCK,
            E::NEUTRAL, I::CLOSED, F::BREATHE, 0, 0, 0, 0, 1, M::CLOSED),
    CAT_KEY(5200, HOLD, S::CURL, G::PLANT, H::GROOM_FLANK, T::TUCK,
            E::NEUTRAL, I::CLOSED, F::REST, 0, 0, 0, 0, 0, M::CLOSED),
};

// Kneading alternates loaded front stubbies while the barrel stays low. The
// last key returns to a seated support pose so the ordinary sit-to-stand exit
// can take over without a loaf-to-standing silhouette pop.
static constexpr Keyframe kKneadKeys[] = {
    CAT_KEY(700, HOLD, S::SIT, G::PLANT, H::DOWN, T::CURL,
            E::FORWARD, I::SOFT, F::REST, 0, 0, 0, 0, 0, M::CLOSED),
    CAT_KEY(520, EASE, S::LOAF, G::STEP_A, H::DOWN, T::CURL,
            E::FORWARD, I::CLOSED, F::COMPRESS, 0, 1, 0, 0, -1, M::CLOSED),
    CAT_KEY(520, EASE, S::LOAF, G::STEP_B, H::DOWN, T::UP,
            E::NEUTRAL, I::SOFT, F::BREATHE, 0, 0, 0, 0, 1, M::CLOSED),
    CAT_KEY(520, EASE, S::LOAF, G::STEP_A, H::DOWN, T::CURL,
            E::FORWARD, I::CLOSED, F::COMPRESS, 0, 1, 0, 0, -1, M::CLOSED),
    CAT_KEY(520, EASE, S::LOAF, G::STEP_B, H::DOWN, T::UP,
            E::NEUTRAL, I::SOFT, F::BREATHE, 0, 0, 0, 0, 1, M::CLOSED),
    CAT_KEY(900, EASE, S::SIT, G::PLANT, H::FRONT, T::CURL,
            E::NEUTRAL, I::SOFT, F::REST, 0, 0, 0, 0, 0, M::CLOSED),
};

// Slow blinking is held eye contact with long quiet beats. It is intentionally
// less animated than a signal or reward ceremony; trust reads in the pause.
static constexpr Keyframe kSlowBlinkKeys[] = {
    CAT_KEY(2400, HOLD, S::SIT, G::PLANT, H::FRONT, T::CURL,
            E::FORWARD, I::OPEN, F::REST, 0, 0, 0, 0, 0, M::CLOSED),
    CAT_KEY(520, EASE, S::SIT, G::PLANT, H::FRONT, T::CURL,
            E::NEUTRAL, I::SOFT, F::BREATHE, 0, 0, 0, 0, 1, M::CLOSED),
    CAT_KEY(900, HOLD, S::SIT, G::PLANT, H::FRONT, T::CURL,
            E::NEUTRAL, I::CLOSED, F::REST, 0, 0, 0, 0, 0, M::CLOSED),
    CAT_KEY(620, EASE, S::SIT, G::PLANT, H::FRONT, T::UP,
            E::FORWARD, I::SOFT, F::BREATHE, 0, 0, 0, 0, -1, M::CLOSED),
    CAT_KEY(3200, HOLD, S::SIT, G::PLANT, H::FRONT, T::CURL,
            E::NEUTRAL, I::OPEN, F::REST, 0, 0, 0, 0, 0, M::CLOSED),
};

// A meow is the one beat where the cat addresses Pancetta out loud. On the
// canonical 4px output, shut-tight gape and the soft look back are the clearest
// vocal reads, so the clip spends its frames on anticipation, a double call,
// and the recovery instead of on in-between jaw positions that would be
// invisible. The chest carries the effort: breath rises before the first call
// and empties into the second.
static constexpr Keyframe kMeowKeys[] = {
    CAT_KEY(560, HOLD, S::SIT, G::PLANT, H::FRONT, T::CURL,
            E::FORWARD, I::OPEN, F::REST, 0, 0, 0, 0, 0, M::CLOSED),
    CAT_KEY(240, EASE, S::SIT, G::PLANT, H::UP, T::UP,
            E::ALERT, I::WIDE, F::BREATHE, 0, 0, 0, -1, 1, M::CLOSED),
    CAT_KEY(460, HOLD, S::SIT, G::PLANT, H::UP, T::UP,
            E::FORWARD, I::SQUEEZE, F::SWEEP_FORWARD, 0, 0, 0, -1, 1,
            M::MEOW),
    CAT_KEY(220, EASE, S::SIT, G::PLANT, H::UP, T::CURL,
            E::FORWARD, I::SOFT, F::BREATHE, 0, 0, 0, 0, 0, M::CLOSED),
    CAT_KEY(380, HOLD, S::SIT, G::PLANT, H::UP, T::UP,
            E::ALERT, I::SQUEEZE, F::SWEEP_FORWARD, 0, 0, 0, -1, 1,
            M::MEOW),
    CAT_KEY(260, EASE, S::SIT, G::PLANT, H::FRONT, T::CURL,
            E::NEUTRAL, I::SOFT, F::BREATHE, 0, 0, 0, 0, -1, M::CHIRP),
    CAT_KEY(1400, HOLD, S::SIT, G::PLANT, H::FRONT, T::CURL,
            E::FORWARD, I::OPEN, F::REST, 0, 0, 0, 0, 0, M::CLOSED),
};

static constexpr Keyframe kSignalKeys[] = {
    CAT_KEY(420, HOLD, S::SIT, G::PLANT, H::FRONT, T::CURL,
            E::ALERT, I::OPEN, F::REST, 0, 0, 0, 0, 0, M::CLOSED),
    CAT_KEY(260, EASE, S::SIT, G::PLANT, H::FRONT, T::CURL,
            E::FORWARD, I::WIDE, F::BREATHE, 0, 0, 0, 0, 1, M::CHIRP),
    CAT_KEY(240, EASE, S::SIT, G::PLANT, H::FRONT, T::UP,
            E::ALERT, I::OPEN, F::SWEEP_FORWARD, 0, 0, 0, -1, 0, M::CLOSED),
    CAT_KEY(240, EASE, S::SIT, G::PLANT, H::FRONT, T::UP,
            E::FORWARD, I::WIDE, F::BREATHE, 0, 0, 0, 0, -1, M::CHIRP),
    CAT_KEY(280, EASE, S::SIT, G::PLANT, H::FRONT, T::CURL,
            E::ALERT, I::OPEN, F::REST, 0, 0, 0, 0, 0, M::CLOSED),
    CAT_KEY(400, EASE, S::SIT, G::PAW_LIFT, H::FRONT, T::CURL,
            E::FORWARD, I::SOFT, F::BREATHE, 0, 0, 0, 0, 1, M::CHIRP),
    CAT_KEY(700, HOLD, S::SIT, G::PLANT, H::FRONT, T::CURL,
            E::NEUTRAL, I::OPEN, F::REST, 0, 0, 0, 0, 0, M::CLOSED),
};

// Cross-state clips are deliberately short and grounded. They carry the
// action prompt's anatomical continuity without turning the renderer into a
// transition planner or asking one frame to impersonate two stance families.
static constexpr Keyframe kStandToCrouchKeys[] = {
    CAT_KEY(120, EASE, S::STAND, G::PLANT, H::LEVEL, T::UP,
            E::NEUTRAL, I::OPEN, F::REST, 0, 0, 0, 0, 0, M::CLOSED),
    CAT_KEY(140, EASE, S::STAND, G::PLANT, H::DOWN, T::LEVEL,
            E::FORWARD, I::OPEN, F::BREATHE, 0, 1, 0, 1, -1, M::CLOSED),
    CAT_KEY(160, EASE, S::CROUCH, G::PLANT, H::LEVEL, T::LEVEL,
            E::FORWARD, I::OPEN, F::SWEEP_FORWARD, 0, 0, 0, 0, 0, M::CLOSED),
    CAT_KEY(180, HOLD, S::CROUCH, G::PLANT, H::DOWN, T::TRAIL_LOW,
            E::FORWARD, I::SOFT, F::REST, 0, 0, 0, 0, 0, M::CLOSED),
};

static constexpr Keyframe kCrouchToStandKeys[] = {
    CAT_KEY(120, EASE, S::CROUCH, G::PLANT, H::DOWN, T::TRAIL_LOW,
            E::FORWARD, I::SOFT, F::REST, 0, 0, 0, 0, 0, M::CLOSED),
    CAT_KEY(140, EASE, S::CROUCH, G::PLANT, H::LEVEL, T::LEVEL,
            E::ALERT, I::OPEN, F::BREATHE, 0, -1, 0, 0, 1, M::CLOSED),
    CAT_KEY(160, EASE, S::STAND, G::PLANT, H::LEVEL, T::LEVEL,
            E::FORWARD, I::OPEN, F::SWEEP_FORWARD, 0, 0, 0, 0, 0, M::CLOSED),
    CAT_KEY(180, HOLD, S::STAND, G::PLANT, H::LEVEL, T::UP,
            E::NEUTRAL, I::OPEN, F::REST, 0, 0, 0, 0, 0, M::CLOSED),
};

static constexpr Keyframe kStandToSitKeys[] = {
    CAT_KEY(140, EASE, S::STAND, G::PLANT, H::LEVEL, T::UP,
            E::NEUTRAL, I::OPEN, F::REST, 0, 0, 0, 0, 0, M::CLOSED),
    CAT_KEY(160, EASE, S::CROUCH, G::PLANT, H::LEVEL, T::LEVEL,
            E::FORWARD, I::OPEN, F::BREATHE, 0, 0, 0, 0, -1, M::CLOSED),
    CAT_KEY(180, EASE, S::SIT, G::PLANT, H::LEVEL, T::CURL,
            E::ALERT, I::SOFT, F::SWEEP_FORWARD, 0, -1, 0, 0, 0, M::CLOSED),
    CAT_KEY(220, HOLD, S::SIT, G::PLANT, H::FRONT, T::CURL,
            E::NEUTRAL, I::OPEN, F::REST, 0, 0, 0, 0, 0, M::CLOSED),
};

static constexpr Keyframe kSitToStandKeys[] = {
    CAT_KEY(140, EASE, S::SIT, G::PLANT, H::FRONT, T::CURL,
            E::NEUTRAL, I::OPEN, F::REST, 0, 0, 0, 0, 0, M::CLOSED),
    CAT_KEY(160, EASE, S::SIT, G::PLANT, H::LEVEL, T::LEVEL,
            E::ALERT, I::OPEN, F::BREATHE, 0, -1, 0, 0, 1, M::CLOSED),
    CAT_KEY(180, EASE, S::CROUCH, G::PLANT, H::LEVEL, T::UP,
            E::FORWARD, I::OPEN, F::SWEEP_FORWARD, 0, 0, 0, 0, 0, M::CLOSED),
    CAT_KEY(220, HOLD, S::STAND, G::PLANT, H::LEVEL, T::UP,
            E::NEUTRAL, I::OPEN, F::REST, 0, 0, 0, 0, 0, M::CLOSED),
};

static constexpr Keyframe kSitToLoafKeys[] = {
    CAT_KEY(160, EASE, S::SIT, G::PLANT, H::FRONT, T::CURL,
            E::NEUTRAL, I::SOFT, F::REST, 0, 0, 0, 0, 0, M::CLOSED),
    CAT_KEY(180, EASE, S::SIT, G::PLANT, H::DOWN, T::CURL,
            E::FORWARD, I::SOFT, F::BREATHE, 0, 0, 0, 1, -1, M::CLOSED),
    CAT_KEY(200, EASE, S::LOAF, G::PLANT, H::DOWN, T::CURL,
            E::NEUTRAL, I::SOFT, F::COMPRESS, 0, 0, 0, 0, 0, M::CLOSED),
    CAT_KEY(240, HOLD, S::LOAF, G::PLANT, H::DOWN, T::TUCK,
            E::NEUTRAL, I::CLOSED, F::REST, 0, 0, 0, 0, 0, M::CLOSED),
};

static constexpr Keyframe kLoafToSitKeys[] = {
    CAT_KEY(160, EASE, S::LOAF, G::PLANT, H::DOWN, T::TUCK,
            E::NEUTRAL, I::CLOSED, F::REST, 0, 0, 0, 0, 0, M::CLOSED),
    CAT_KEY(180, EASE, S::LOAF, G::PLANT, H::LEVEL, T::CURL,
            E::ALERT, I::SOFT, F::BREATHE, 0, -1, 0, -1, 1, M::CLOSED),
    CAT_KEY(200, EASE, S::SIT, G::PLANT, H::LEVEL, T::CURL,
            E::FORWARD, I::OPEN, F::SWEEP_FORWARD, 0, -1, 0, 0, 0, M::CLOSED),
    CAT_KEY(240, HOLD, S::SIT, G::PLANT, H::FRONT, T::CURL,
            E::NEUTRAL, I::OPEN, F::REST, 0, 0, 0, 0, 0, M::CLOSED),
};

static constexpr Keyframe kLoafToSleepKeys[] = {
    CAT_KEY(180, EASE, S::LOAF, G::PLANT, H::DOWN, T::CURL,
            E::NEUTRAL, I::SOFT, F::REST, 0, 0, 0, 0, 0, M::CLOSED),
    CAT_KEY(220, EASE, S::LOAF, G::PLANT, H::DOWN, T::TUCK,
            E::NEUTRAL, I::CLOSED, F::COMPRESS, 0, 0, 0, 0, -1, M::CLOSED),
    CAT_KEY(260, EASE, S::CURL, G::PLANT, H::GROOM_FLANK, T::TUCK,
            E::NEUTRAL, I::CLOSED, F::BREATHE, 0, 0, 0, 0, 1, M::CLOSED),
    CAT_KEY(300, HOLD, S::CURL, G::PLANT, H::GROOM_FLANK, T::TUCK,
            E::NEUTRAL, I::CLOSED, F::REST, 0, 0, 0, 0, 0, M::CLOSED),
};

static constexpr Keyframe kSleepToStandKeys[] = {
    CAT_KEY(200, EASE, S::LOAF, G::PLANT, H::DOWN, T::TUCK,
            E::NEUTRAL, I::CLOSED, F::BREATHE, 0, 0, 0, 0, 1, M::CLOSED),
    CAT_KEY(220, EASE, S::LOAF, G::PLANT, H::DOWN, T::CURL,
            E::ALERT, I::SOFT, F::BREATHE, 0, -1, 0, 0, 1, M::CLOSED),
    CAT_KEY(240, EASE, S::SIT, G::PLANT, H::FRONT, T::CURL,
            E::FORWARD, I::OPEN, F::SWEEP_FORWARD, 0, -1, 0, 0, 0, M::CLOSED),
    CAT_KEY(240, EASE, S::CROUCH, G::PLANT, H::LEVEL, T::LEVEL,
            E::ALERT, I::OPEN, F::BREATHE, 0, 0, 0, 0, 0, M::CLOSED),
    CAT_KEY(260, HOLD, S::STAND, G::PLANT, H::LEVEL, T::UP,
            E::NEUTRAL, I::OPEN, F::REST, 0, 0, 0, 0, 0, M::CLOSED),
};

static constexpr Keyframe kCrouchToPounceKeys[] = {
    CAT_KEY(100, EASE, S::CROUCH, G::PLANT, H::UP, T::TRAIL_LOW,
            E::ALERT, I::WIDE, F::REST, 0, 0, 0, 0, 0, M::CLOSED),
    CAT_KEY(120, EASE, S::CROUCH, G::STEP_A, H::UP, T::LEVEL,
            E::FORWARD, I::WIDE, F::DRAG_BACK, 0, -1, 0, -1, 0, M::CLOSED),
    CAT_KEY(140, EASE, S::STRETCH, G::BOUND_TUCK, H::UP, T::LEVEL,
            E::BACK, I::WIDE, F::DRAG_BACK, 0, -3, 0, 0, 0, M::CLOSED),
    CAT_KEY(150, HOLD, S::STRETCH, G::BOUND_EXTEND, H::LEVEL, T::LEVEL,
            E::BACK, I::WIDE, F::DRAG_BACK, 0, -2, 0, 0, 0, M::CLOSED),
};

static constexpr Keyframe kPounceToLandKeys[] = {
    CAT_KEY(90, EASE, S::STRETCH, G::BOUND_EXTEND, H::LEVEL, T::LEVEL,
            E::BACK, I::WIDE, F::DRAG_BACK, 0, -2, 0, 0, 0, M::CLOSED),
    CAT_KEY(110, EASE, S::STRETCH, G::BOUND_TUCK, H::DOWN, T::LEVEL,
            E::BACK, I::OPEN, F::DRAG_BACK, 1, -3, 0, 0, 0, M::CLOSED),
    CAT_KEY(130, EASE, S::CROUCH, G::BOUND_TUCK, H::DOWN, T::TRAIL_LOW,
            E::ALERT, I::WIDE, F::COMPRESS, 0, -1, 0, 0, -1, M::CLOSED),
    CAT_KEY(160, HOLD, S::CROUCH, G::PLANT, H::FRONT, T::CURL,
            E::ALERT, I::OPEN, F::SWEEP_FORWARD, 0, 0, 0, 0, 0, M::CLOSED),
};

static constexpr Keyframe kLandToStandKeys[] = {
    CAT_KEY(120, EASE, S::CROUCH, G::PLANT, H::FRONT, T::CURL,
            E::ALERT, I::OPEN, F::COMPRESS, 0, 0, 0, 0, -1, M::CLOSED),
    CAT_KEY(140, EASE, S::CROUCH, G::PLANT, H::LEVEL, T::LEVEL,
            E::FORWARD, I::SOFT, F::SWEEP_FORWARD, 0, -1, 0, 0, 0, M::CLOSED),
    CAT_KEY(160, EASE, S::STAND, G::PLANT, H::LEVEL, T::UP,
            E::ALERT, I::OPEN, F::BREATHE, 0, 0, 0, 0, 1, M::CLOSED),
    CAT_KEY(180, HOLD, S::STAND, G::PLANT, H::LEVEL, T::UP,
            E::NEUTRAL, I::OPEN, F::REST, 0, 0, 0, 0, 0, M::CLOSED),
};

static constexpr Keyframe kIdleToArchKeys[] = {
    CAT_KEY(120, EASE, S::STAND, G::PLANT, H::LEVEL, T::UP,
            E::NEUTRAL, I::OPEN, F::REST, 0, 0, 0, 0, 0, M::CLOSED),
    CAT_KEY(140, EASE, S::STAND, G::PLANT, H::FRONT, T::PUFF,
            E::ALERT, I::WIDE, F::BREATHE, 0, -1, 0, 0, 0, M::CLOSED),
    CAT_KEY(170, EASE, S::STAND, G::PLANT, H::LEVEL, T::PUFF,
            E::BACK, I::WIDE, F::BRISTLE, 0, -1, 0, 0, 0, M::CLOSED),
    CAT_KEY(190, HOLD, S::STAND, G::PLANT, H::LEVEL, T::UP,
            E::ALERT, I::WIDE, F::BREATHE, 0, 0, 0, 0, 0, M::CLOSED),
};

static constexpr Keyframe kArchToIdleKeys[] = {
    CAT_KEY(130, EASE, S::STAND, G::PLANT, H::FRONT, T::CURL,
            E::NEUTRAL, I::SOFT, F::REST, 0, 0, 0, 0, 0, M::CLOSED),
    CAT_KEY(160, EASE, S::STAND, G::PLANT, H::LEVEL, T::PUFF,
            E::ALERT, I::OPEN, F::BRISTLE, 0, -1, 0, 0, 0, M::CLOSED),
    CAT_KEY(180, EASE, S::STAND, G::PLANT, H::LEVEL, T::UP,
            E::FORWARD, I::SOFT, F::SWEEP_FORWARD, 0, 0, 0, 0, -1, M::CLOSED),
    CAT_KEY(210, HOLD, S::STAND, G::PLANT, H::LEVEL, T::CURL,
            E::NEUTRAL, I::OPEN, F::REST, 0, 0, 0, 0, 0, M::CLOSED),
};

static constexpr Keyframe kIdleToZoomiesKeys[] = {
    CAT_KEY(90, EASE, S::STAND, G::PLANT, H::LEVEL, T::UP,
            E::ALERT, I::WIDE, F::REST, 0, 0, 0, 0, 0, M::CLOSED),
    CAT_KEY(110, EASE, S::CROUCH, G::PLANT, H::UP, T::TRAIL_LOW,
            E::FORWARD, I::WIDE, F::COMPRESS, 0, 0, 0, 0, -1, M::CLOSED),
    CAT_KEY(120, EASE, S::STRETCH, G::BOUND_TUCK, H::UP, T::LEVEL,
            E::BACK, I::WIDE, F::DRAG_BACK, 0, -3, 0, 0, 0, M::CLOSED),
    CAT_KEY(130, HOLD, S::STRETCH, G::BOUND_EXTEND, H::LEVEL, T::LEVEL,
            E::BACK, I::WIDE, F::DRAG_BACK, 0, -2, 0, 0, 0, M::CLOSED),
};

static constexpr Keyframe kZoomiesToSettleKeys[] = {
    CAT_KEY(100, EASE, S::CROUCH, G::PLANT, H::FRONT, T::CURL,
            E::ALERT, I::OPEN, F::SWEEP_FORWARD, 0, 0, 0, 0, 0, M::CLOSED),
    CAT_KEY(120, EASE, S::CROUCH, G::PLANT, H::DOWN, T::LEVEL,
            E::BACK, I::OPEN, F::COMPRESS, 0, 0, 0, 0, -1, M::CLOSED),
    CAT_KEY(140, EASE, S::CROUCH, G::PLANT, H::FRONT, T::CURL,
            E::ALERT, I::OPEN, F::SWEEP_FORWARD, 0, 0, 0, 0, 0, M::CLOSED),
    CAT_KEY(160, EASE, S::STAND, G::PLANT, H::LEVEL, T::UP,
            E::FORWARD, I::SOFT, F::BREATHE, 0, 0, 0, 0, -1, M::CLOSED),
    CAT_KEY(180, HOLD, S::STAND, G::PLANT, H::LEVEL, T::CURL,
            E::NEUTRAL, I::OPEN, F::REST, 0, 0, 0, 0, 0, M::CLOSED),
};

static constexpr Keyframe kIdleToGroomKeys[] = {
    CAT_KEY(140, EASE, S::STAND, G::PLANT, H::LEVEL, T::UP,
            E::NEUTRAL, I::OPEN, F::REST, 0, 0, 0, 0, 0, M::CLOSED),
    CAT_KEY(170, EASE, S::SIT, G::PLANT, H::FRONT, T::CURL,
            E::ALERT, I::SOFT, F::BREATHE, 0, -1, 0, 0, 1, M::CLOSED),
    CAT_KEY(190, EASE, S::SIT, G::PAW_LIFT, H::GROOM_PAW, T::CURL,
            E::FORWARD, I::SOFT, F::SWEEP_FORWARD, 0, 0, -1, 1, 0, M::CLOSED),
    CAT_KEY(220, HOLD, S::SIT, G::PAW_LIFT, H::GROOM_PAW, T::CURL,
            E::NEUTRAL, I::SOFT, F::REST, 0, 0, 0, 0, 0, M::CLOSED),
};

static constexpr Keyframe kGroomToIdleKeys[] = {
    CAT_KEY(140, EASE, S::SIT, G::PLANT, H::FRONT, T::CURL,
            E::ALERT, I::OPEN, F::REST, 0, 0, 0, 0, 0, M::CLOSED),
    CAT_KEY(170, EASE, S::SIT, G::PLANT, H::FRONT, T::CURL,
            E::FORWARD, I::SOFT, F::BREATHE, 0, 0, 0, 0, -1, M::CLOSED),
    CAT_KEY(190, EASE, S::CROUCH, G::PLANT, H::LEVEL, T::LEVEL,
            E::ALERT, I::OPEN, F::SWEEP_FORWARD, 0, -1, 0, 0, 0, M::CLOSED),
    CAT_KEY(220, HOLD, S::STAND, G::PLANT, H::LEVEL, T::UP,
            E::NEUTRAL, I::OPEN, F::REST, 0, 0, 0, 0, 0, M::CLOSED),
};

static constexpr Keyframe kIdleToHairballKeys[] = {
    CAT_KEY(140, EASE, S::STAND, G::PLANT, H::LEVEL, T::UP,
            E::NEUTRAL, I::OPEN, F::REST, 0, 0, 0, 0, 0, M::CLOSED),
    CAT_KEY(170, EASE, S::CROUCH, G::PLANT, H::DOWN, T::TRAIL_LOW,
            E::BACK, I::SOFT, F::COMPRESS, 0, 1, 0, 0, -1, M::CLOSED),
    CAT_KEY(190, EASE, S::CROUCH, G::PLANT, H::DOWN, T::TUCK,
            E::BACK, I::CLOSED, F::COMPRESS, -1, 1, 1, 1, -1, M::CLOSED),
    CAT_KEY(220, HOLD, S::CROUCH, G::PLANT, H::DOWN, T::TRAIL_LOW,
            E::BACK, I::SOFT, F::COMPRESS, 0, 1, 0, 0, 0, M::CLOSED),
};

static constexpr Keyframe kHairballToIdleKeys[] = {
    CAT_KEY(140, EASE, S::SIT, G::PLANT, H::LEVEL, T::CURL,
            E::NEUTRAL, I::SOFT, F::REST, 0, 0, 0, 0, 0, M::CLOSED),
    CAT_KEY(180, EASE, S::SIT, G::PLANT, H::FRONT, T::CURL,
            E::ALERT, I::WIDE, F::BREATHE, 0, 0, 0, 0, 1, M::CLOSED),
    CAT_KEY(200, EASE, S::CROUCH, G::PLANT, H::LEVEL, T::LEVEL,
            E::FORWARD, I::OPEN, F::SWEEP_FORWARD, 0, -1, 0, 0, 0, M::CLOSED),
    CAT_KEY(230, HOLD, S::STAND, G::PLANT, H::LEVEL, T::UP,
            E::NEUTRAL, I::OPEN, F::REST, 0, 0, 0, 0, 0, M::CLOSED),
};

static constexpr Keyframe kSitToSignalKeys[] = {
    CAT_KEY(150, EASE, S::SIT, G::PLANT, H::FRONT, T::CURL,
            E::NEUTRAL, I::OPEN, F::REST, 0, 0, 0, 0, 0, M::CLOSED),
    CAT_KEY(180, EASE, S::SIT, G::PAW_LIFT, H::FRONT, T::CURL,
            E::ALERT, I::WIDE, F::BREATHE, 0, 0, 0, -1, 1, M::CLOSED),
    CAT_KEY(210, EASE, S::SIT, G::PLANT, H::FRONT, T::UP,
            E::FORWARD, I::WIDE, F::SWEEP_FORWARD, 0, 0, 0, 0, 0, M::CHIRP),
    CAT_KEY(240, HOLD, S::SIT, G::PLANT, H::FRONT, T::CURL,
            E::ALERT, I::OPEN, F::REST, 0, 0, 0, 0, 0, M::CLOSED),
};

static constexpr Keyframe kSignalToIdleKeys[] = {
    CAT_KEY(150, EASE, S::SIT, G::PLANT, H::FRONT, T::CURL,
            E::ALERT, I::OPEN, F::REST, 0, 0, 0, 0, 0, M::CLOSED),
    CAT_KEY(180, EASE, S::SIT, G::PLANT, H::LEVEL, T::CURL,
            E::FORWARD, I::SOFT, F::BREATHE, 0, 0, 0, 0, -1, M::CLOSED),
    CAT_KEY(210, EASE, S::CROUCH, G::PLANT, H::LEVEL, T::LEVEL,
            E::ALERT, I::OPEN, F::SWEEP_FORWARD, 0, -1, 0, 0, 0, M::CLOSED),
    CAT_KEY(240, HOLD, S::STAND, G::PLANT, H::LEVEL, T::UP,
            E::NEUTRAL, I::OPEN, F::REST, 0, 0, 0, 0, 0, M::CLOSED),
};

#undef CAT_KEY

#define CAT_CLIP(keys, playback, role) \
    {keys, (uint8_t)(sizeof(keys) / sizeof(keys[0])), \
     Playback::playback, Role::role}

static constexpr Clip kFollowIdleClip =
    CAT_CLIP(kFollowIdleKeys, LOOP, FOLLOW);
static constexpr Clip kSignalClip =
    CAT_CLIP(kSignalKeys, LOOP, SIGNAL);

static constexpr Clip kActivityClips[] = {
    CAT_CLIP(kFollowWalkKeys, LOOP, FOLLOW),
    CAT_CLIP(kCableKeys, LOOP, OBSERVE),
    CAT_CLIP(kRainKeys, LOOP, OBSERVE),
    CAT_CLIP(kFoodKeys, LOOP, OBSERVE),
    CAT_CLIP(kSkyKeys, LOOP, OBSERVE),
    CAT_CLIP(kProwlKeys, LOOP, PROWL),
    CAT_CLIP(kWaterKeys, LOOP, OBSERVE),
    CAT_CLIP(kScratchKeys, ONE_SHOT, ACTION),
    CAT_CLIP(kSleepKeys, ONE_SHOT, ACTION),
    CAT_CLIP(kGroomKeys, ONE_SHOT, ACTION),
    CAT_CLIP(kHairballKeys, ONE_SHOT, ACTION),
    CAT_CLIP(kArchKeys, ONE_SHOT, ACTION),
    CAT_CLIP(kZoomKeys, ONE_SHOT, ACTION),
    CAT_CLIP(kFaceBumpKeys, ONE_SHOT, ACTION),
    CAT_CLIP(kHeadNapKeys, ONE_SHOT, ACTION),
    CAT_CLIP(kKneadKeys, ONE_SHOT, ACTION),
    CAT_CLIP(kSlowBlinkKeys, LOOP, OBSERVE),
    CAT_CLIP(kMeowKeys, ONE_SHOT, ACTION),
};

static constexpr Clip kTransitionClips[] = {
    CAT_CLIP(kStandToCrouchKeys, ONE_SHOT, TRANSITION),
    CAT_CLIP(kCrouchToStandKeys, ONE_SHOT, TRANSITION),
    CAT_CLIP(kStandToSitKeys, ONE_SHOT, TRANSITION),
    CAT_CLIP(kSitToStandKeys, ONE_SHOT, TRANSITION),
    CAT_CLIP(kSitToLoafKeys, ONE_SHOT, TRANSITION),
    CAT_CLIP(kLoafToSitKeys, ONE_SHOT, TRANSITION),
    CAT_CLIP(kLoafToSleepKeys, ONE_SHOT, TRANSITION),
    CAT_CLIP(kSleepToStandKeys, ONE_SHOT, TRANSITION),
    CAT_CLIP(kCrouchToPounceKeys, ONE_SHOT, TRANSITION),
    CAT_CLIP(kPounceToLandKeys, ONE_SHOT, TRANSITION),
    CAT_CLIP(kLandToStandKeys, ONE_SHOT, TRANSITION),
    CAT_CLIP(kIdleToArchKeys, ONE_SHOT, TRANSITION),
    CAT_CLIP(kArchToIdleKeys, ONE_SHOT, TRANSITION),
    CAT_CLIP(kIdleToZoomiesKeys, ONE_SHOT, TRANSITION),
    CAT_CLIP(kZoomiesToSettleKeys, ONE_SHOT, TRANSITION),
    CAT_CLIP(kIdleToGroomKeys, ONE_SHOT, TRANSITION),
    CAT_CLIP(kGroomToIdleKeys, ONE_SHOT, TRANSITION),
    CAT_CLIP(kIdleToHairballKeys, ONE_SHOT, TRANSITION),
    CAT_CLIP(kHairballToIdleKeys, ONE_SHOT, TRANSITION),
    CAT_CLIP(kSitToSignalKeys, ONE_SHOT, TRANSITION),
    CAT_CLIP(kSignalToIdleKeys, ONE_SHOT, TRANSITION),
};

#undef CAT_CLIP

static_assert(sizeof(kActivityClips) / sizeof(kActivityClips[0]) ==
                  (uint8_t)Activity::COUNT,
              "every cat activity needs one authored rig clip");
static_assert(sizeof(kTransitionClips) / sizeof(kTransitionClips[0]) ==
                  (uint8_t)Transition::COUNT - 1u,
              "every named cat transition needs one authored rig clip");

static int8_t lerpI8(int8_t from, int8_t to, uint8_t t) {
    const int scaled = ((int)to - (int)from) * (int)t;
    // Round signed offsets symmetrically. An unconditional positive bias
    // prevents a one-cell negative tween from ever reaching its key pose.
    const int rounded = scaled >= 0
        ? (scaled + 127) / 255
        : (scaled - 127) / 255;
    return (int8_t)((int)from + rounded);
}

template <typename T>
static T chooseDiscrete(T from, T to, uint8_t t, uint8_t threshold) {
    return t < threshold ? from : to;
}

static Rig::Pose interpolate(const Rig::Pose& from,
                             const Rig::Pose& to, uint8_t t) {
    return Rig::Pose(
        // Expression leads, support settles last.  Giving each discrete plane
        // its own beat prevents a held sit from replacing gait, head, tail and
        // body silhouette on the same rendered frame.
        chooseDiscrete(from.stance, to.stance, t, 240u),
        chooseDiscrete(from.gait, to.gait, t, 224u),
        chooseDiscrete(from.head, to.head, t, 208u),
        chooseDiscrete(from.tail, to.tail, t, 192u),
        chooseDiscrete(from.ears, to.ears, t, 160u),
        chooseDiscrete(from.eyes, to.eyes, t, 176u),
        chooseDiscrete(from.fur, to.fur, t, 144u),
        lerpI8(from.bodyDx, to.bodyDx, t),
        lerpI8(from.bodyDy, to.bodyDy, t),
        lerpI8(from.headDx, to.headDx, t),
        lerpI8(from.headDy, to.headDy, t),
        lerpI8(from.breath, to.breath, t),
        // The jaw is the fastest part on the face, so a gape leads the head
        // lift instead of trailing it.
        chooseDiscrete(from.mouth, to.mouth, t, 128u));
}

static uint8_t easeQ8(uint8_t t) {
    const uint32_t q = t;
    return (uint8_t)(
        (q * q * (765u - 2u * q) + 32512u) / 65025u);
}

// A held pose owns the quiet part of its interval, then gives the next key a
// short runway.  Holding through the final millisecond used to replace the
// whole silhouette at the boundary: valid geometry, one-frame alibi.  Cap the
// release so multi-second observation beats stay quiet and short vocal/action
// holds still keep most of their authored dwell.
static uint32_t heldReleaseDuration(uint32_t durationMs) {
    static constexpr uint32_t kMaxReleaseMs = 320u;
    if (durationMs < 6u) return 0u;
    const uint32_t proportional = durationMs / 3u;
    return proportional < kMaxReleaseMs ? proportional : kMaxReleaseMs;
}

static bool samePose(const Rig::Pose& a, const Rig::Pose& b) {
    return a.stance == b.stance && a.gait == b.gait &&
           a.head == b.head && a.tail == b.tail &&
           a.ears == b.ears && a.eyes == b.eyes && a.fur == b.fur &&
           a.bodyDx == b.bodyDx && a.bodyDy == b.bodyDy &&
           a.headDx == b.headDx && a.headDy == b.headDy &&
           a.breath == b.breath && a.mouth == b.mouth;
}

static bool validateClipGeometry(const Clip& clip) {
    if (!clip.keys || !clip.count || !clipDuration(clip)) return false;
    uint32_t keyStart = 0;
    for (uint8_t key = 0; key < clip.count; ++key) {
        if (!clip.keys[key].durationMs ||
            !Rig::poseFitsEnvelope(clip.keys[key].pose))
            return false;
        uint8_t next = (uint8_t)(key + 1u);
        if (next < clip.count || clip.playback == Playback::LOOP) {
            if (next >= clip.count) next = 0;
            const uint32_t boundary =
                keyStart + clip.keys[key].durationMs - 1u;
            if (!samePose(sample(clip, boundary), clip.keys[next].pose))
                return false;
        }
        keyStart += clip.keys[key].durationMs;
    }

    // Discrete part modes switch during an eased interval while offsets are
    // still interpolating. Sampling the full clip catches those hybrid poses;
    // checking only authored endpoints missed the original clipping defect.
    const uint32_t duration = clipDuration(clip);
    for (uint32_t elapsed = 0; elapsed < duration; elapsed += 32u) {
        if (!Rig::poseFitsEnvelope(sample(clip, elapsed))) return false;
    }
    return Rig::poseFitsEnvelope(sample(clip, duration - 1u));
}

enum class PoseFamily : uint8_t {
    STAND,
    CROUCH,
    SIT,
};

static PoseFamily poseFamily(Activity activity) {
    switch (activity) {
        case Activity::WATCH_CABLES:
        case Activity::SNIFF_FOOD:
        case Activity::STALK_SKY:
        case Activity::PROWL_BAR:
        case Activity::WATCH_WATER:
            return PoseFamily::CROUCH;
        case Activity::WATCH_RAIN:
        case Activity::SCRATCH:
        case Activity::SLEEP:
        case Activity::HEAD_NAP:
        case Activity::KNEAD:
        case Activity::SLOW_BLINK:
        case Activity::MEOW:
            return PoseFamily::SIT;
        case Activity::FACE_BUMP:
            return PoseFamily::CROUCH;
        case Activity::FOLLOW:
        case Activity::GROOM:
        case Activity::HAIRBALL:
        case Activity::ARCH:
        case Activity::ZOOMIES:
        case Activity::COUNT:
        default:
            return PoseFamily::STAND;
    }
}

static Transition actionExit(Activity activity) {
    switch (activity) {
        case Activity::SCRATCH: return Transition::SIT_TO_STAND;
        case Activity::SLEEP: return Transition::SLEEP_TO_STAND;
        case Activity::HEAD_NAP: return Transition::SLEEP_TO_STAND;
        case Activity::KNEAD: return Transition::SIT_TO_STAND;
        case Activity::MEOW: return Transition::SIT_TO_STAND;
        case Activity::GROOM: return Transition::GROOM_TO_IDLE;
        case Activity::HAIRBALL: return Transition::HAIRBALL_TO_IDLE;
        case Activity::ARCH: return Transition::ARCH_TO_IDLE;
        case Activity::ZOOMIES: return Transition::ZOOMIES_TO_SETTLE;
        default: return Transition::NONE;
    }
}

static Transition actionEntry(Activity activity) {
    switch (activity) {
        case Activity::GROOM: return Transition::IDLE_TO_GROOM;
        case Activity::HAIRBALL: return Transition::IDLE_TO_HAIRBALL;
        case Activity::ARCH: return Transition::IDLE_TO_ARCH;
        case Activity::ZOOMIES: return Transition::IDLE_TO_ZOOMIES;
        default: return Transition::NONE;
    }
}

static Transition standTo(PoseFamily family) {
    switch (family) {
        case PoseFamily::CROUCH: return Transition::STAND_TO_CROUCH;
        case PoseFamily::SIT: return Transition::STAND_TO_SIT;
        case PoseFamily::STAND:
        default: return Transition::NONE;
    }
}

static Transition toStand(PoseFamily family) {
    switch (family) {
        case PoseFamily::CROUCH: return Transition::CROUCH_TO_STAND;
        case PoseFamily::SIT: return Transition::SIT_TO_STAND;
        case PoseFamily::STAND:
        default: return Transition::NONE;
    }
}

}  // namespace

const Clip& clipFor(Activity activity, bool moving) {
    if (activity == Activity::FOLLOW && !moving)
        return kFollowIdleClip;
    uint8_t index = (uint8_t)activity;
    if (index >= (uint8_t)Activity::COUNT) index = 0;
    return kActivityClips[index];
}

const Clip* clipFor(Transition transition) {
    const uint8_t index = (uint8_t)transition;
    if (index == (uint8_t)Transition::NONE ||
        index >= (uint8_t)Transition::COUNT)
        return nullptr;
    return &kTransitionClips[index - 1u];
}

TransitionPlan planTransition(Activity from, Activity to) {
    TransitionPlan plan;
    if (from == to) return plan;

    const Transition exit = actionExit(from);
    const Transition entry = actionEntry(to);
    if (exit != Transition::NONE) {
        plan.first = exit;
        plan.second = entry != Transition::NONE
            ? entry
            : standTo(poseFamily(to));
        return plan;
    }

    if (entry != Transition::NONE) {
        const Transition leaveFamily = toStand(poseFamily(from));
        if (leaveFamily != Transition::NONE) {
            plan.first = leaveFamily;
            plan.second = entry;
        } else {
            plan.first = entry;
        }
        return plan;
    }

    const PoseFamily fromFamily = poseFamily(from);
    const PoseFamily toFamily = poseFamily(to);
    if (fromFamily == toFamily) return plan;
    const Transition leaveFamily = toStand(fromFamily);
    const Transition enterFamily = standTo(toFamily);
    if (leaveFamily != Transition::NONE) {
        plan.first = leaveFamily;
        plan.second = enterFamily;
    } else {
        plan.first = enterFamily;
    }
    return plan;
}

TransitionPlan planRestart(Activity activity) {
    TransitionPlan plan;
    const Transition exit = actionExit(activity);
    if (exit == Transition::NONE) return plan;

    plan.first = exit;
    const Transition entry = actionEntry(activity);
    plan.second = entry != Transition::NONE
        ? entry
        : standTo(poseFamily(activity));
    return plan;
}

const Clip& signalClip() {
    return kSignalClip;
}

uint32_t clipDuration(const Clip& clip) {
    uint32_t total = 0;
    for (uint8_t i = 0; i < clip.count; ++i)
        total += clip.keys[i].durationMs;
    return total;
}

uint32_t actionDuration(Activity activity) {
    const Clip& clip = clipFor(activity, true);
    return clip.role == Role::ACTION ? clipDuration(clip) : 0u;
}

Rig::Pose sample(const Clip& clip, uint32_t elapsedMs) {
    if (!clip.keys || !clip.count) return Rig::Pose();
    const uint32_t total = clipDuration(clip);
    if (!total) return clip.keys[0].pose;
    uint32_t cursor = clip.playback == Playback::LOOP
        ? elapsedMs % total
        : (elapsedMs < total - 1u ? elapsedMs : total - 1u);
    for (uint8_t i = 0; i < clip.count; ++i) {
        const Keyframe& key = clip.keys[i];
        if (cursor < key.durationMs) {
            uint8_t nextIndex = (uint8_t)(i + 1u);
            if (nextIndex >= clip.count) {
                if (clip.playback == Playback::ONE_SHOT)
                    return key.pose;
                nextIndex = 0;
            }

            uint32_t tweenCursor = cursor;
            uint32_t tweenDuration = key.durationMs;
            if (key.tween == Tween::HOLD) {
                tweenDuration = heldReleaseDuration(key.durationMs);
                const uint32_t releaseAt = key.durationMs - tweenDuration;
                if (!tweenDuration || cursor < releaseAt)
                    return key.pose;
                tweenCursor = cursor - releaseAt;
            }
            if (tweenDuration < 2u) return key.pose;
            const uint32_t rawT =
                (tweenCursor * 255u) / (tweenDuration - 1u);
            const uint8_t t = (uint8_t)(rawT < 255u ? rawT : 255u);
            // Smoothstep in Q8 keeps feet and fur from moving linearly like UI.
            // HOLD keys use the same path only inside their bounded release.
            return interpolate(
                key.pose, clip.keys[nextIndex].pose, easeQ8(t));
        }
        cursor -= key.durationMs;
    }
    return clip.keys[clip.count - 1u].pose;
}

Rig::Pose blendPose(const Rig::Pose& from, const Rig::Pose& to,
                    uint32_t elapsedMs, uint16_t durationMs) {
    if (!durationMs || elapsedMs >= durationMs) return to;
    const uint8_t t = (uint8_t)((elapsedMs * 255u) / durationMs);
    return interpolate(from, to, easeQ8(t));
}

Rig::Pose sample(Activity activity, bool moving, bool signaling,
                 uint32_t animationElapsedMs, uint32_t signalElapsedMs) {
    return signaling
        ? sample(signalClip(), signalElapsedMs)
        : sample(clipFor(activity, moving), animationElapsedMs);
}

bool validateClips() {
    for (uint8_t activity = 0; activity < (uint8_t)Activity::COUNT;
         ++activity) {
        const Clip& clip = clipFor((Activity)activity, true);
        if (!validateClipGeometry(clip)) return false;
    }
    for (uint8_t transition = (uint8_t)Transition::NONE + 1u;
         transition < (uint8_t)Transition::COUNT; ++transition) {
        const Clip* clip = clipFor((Transition)transition);
        if (!clip || clip->playback != Playback::ONE_SHOT ||
            clip->role != Role::TRANSITION || !validateClipGeometry(*clip))
            return false;
    }
    if (!validateClipGeometry(kFollowIdleClip) ||
        !validateClipGeometry(kSignalClip))
        return false;
    // Observation loops must spend seconds watching before a motion beat.
    const TransitionPlan crouchToSit = planTransition(
        Activity::WATCH_CABLES, Activity::WATCH_RAIN);
    const TransitionPlan groomToWater = planTransition(
        Activity::GROOM, Activity::WATCH_WATER);
    const TransitionPlan restartSleep = planRestart(Activity::SLEEP);
    const TransitionPlan restartGroom = planRestart(Activity::GROOM);
    bool sawWalkLoad = false;
    bool sawWalkExtend = false;
    bool sawWalkTuck = false;
    bool sawWalkStepA = false;
    bool sawWalkStepB = false;
    for (const Keyframe& key : kFollowWalkKeys) {
        sawWalkLoad = sawWalkLoad ||
            (key.pose.stance == Rig::Stance::CROUCH &&
             key.pose.gait == Rig::Gait::STRIDE_PLANT);
        sawWalkExtend = sawWalkExtend ||
            key.pose.gait == Rig::Gait::BOUND_EXTEND;
        sawWalkTuck = sawWalkTuck ||
            key.pose.gait == Rig::Gait::BOUND_TUCK;
        sawWalkStepA = sawWalkStepA || key.pose.gait == Rig::Gait::STEP_A;
        sawWalkStepB = sawWalkStepB || key.pose.gait == Rig::Gait::STEP_B;
    }
    return kCableKeys[0].durationMs >= 3000u &&
           kRainKeys[0].durationMs >= 3000u &&
           kWaterKeys[0].durationMs >= 3000u &&
           sawWalkLoad && sawWalkExtend && sawWalkTuck &&
           sawWalkStepA && sawWalkStepB &&
           crouchToSit.first == Transition::CROUCH_TO_STAND &&
           crouchToSit.second == Transition::STAND_TO_SIT &&
           groomToWater.first == Transition::GROOM_TO_IDLE &&
           groomToWater.second == Transition::STAND_TO_CROUCH &&
           restartSleep.first == Transition::SLEEP_TO_STAND &&
           restartSleep.second == Transition::STAND_TO_SIT &&
           restartGroom.first == Transition::GROOM_TO_IDLE &&
           restartGroom.second == Transition::IDLE_TO_GROOM;
}

}  // namespace Animation
}  // namespace PancettaCat
