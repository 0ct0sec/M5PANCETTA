#pragma once

#include <stdint.h>

#include "pancetta_cat.h"
#include "pancetta_cat_rig.h"

namespace PancettaCat {
namespace Animation {

enum class Playback : uint8_t {
    LOOP,
    ONE_SHOT,
};

enum class Role : uint8_t {
    FOLLOW,
    OBSERVE,
    PROWL,
    ACTION,
    SIGNAL,
    TRANSITION,
};

enum class Tween : uint8_t {
    // HOLD preserves most of the interval as a dwell, then releases into the
    // next key. EASE uses the full interval for that transition.
    HOLD,
    EASE,
};

// Cross-activity clips keep the immutable cat anatomy continuous while the
// behavior FSM changes pose families.  Intermediate beats inside a long action
// (loafing, pouncing, landing) remain named here as reviewable source truth
// even when the owning activity clip plays them in one uninterrupted take.
enum class Transition : uint8_t {
    NONE,
    STAND_TO_CROUCH,
    CROUCH_TO_STAND,
    STAND_TO_SIT,
    SIT_TO_STAND,
    SIT_TO_LOAF,
    LOAF_TO_SIT,
    LOAF_TO_SLEEP,
    SLEEP_TO_STAND,
    CROUCH_TO_POUNCE,
    POUNCE_TO_LAND,
    LAND_TO_STAND,
    IDLE_TO_ARCH,
    ARCH_TO_IDLE,
    IDLE_TO_ZOOMIES,
    ZOOMIES_TO_SETTLE,
    IDLE_TO_GROOM,
    GROOM_TO_IDLE,
    IDLE_TO_HAIRBALL,
    HAIRBALL_TO_IDLE,
    SIT_TO_SIGNAL,
    SIGNAL_TO_IDLE,
    COUNT,
};

struct Keyframe {
    Rig::Pose pose;
    uint16_t durationMs;
    Tween tween;
};

struct Clip {
    const Keyframe* keys;
    uint8_t count;
    Playback playback;
    Role role;
};

// Two clips cover the only compound boundary the FSM needs: finish the old
// action to a grounded stand, then enter the new pose family.  No heap-backed
// queue or render-time decision is required.
struct TransitionPlan {
    Transition first = Transition::NONE;
    Transition second = Transition::NONE;
};

const Clip& clipFor(Activity activity, bool moving);
const Clip* clipFor(Transition transition);
TransitionPlan planTransition(Activity from, Activity to);
TransitionPlan planRestart(Activity activity);
const Clip& signalClip();
uint32_t clipDuration(const Clip& clip);
uint32_t actionDuration(Activity activity);
Rig::Pose sample(const Clip& clip, uint32_t elapsedMs);
Rig::Pose sample(Activity activity, bool moving, bool signaling,
                 uint32_t animationElapsedMs, uint32_t signalElapsedMs);
Rig::Pose blendPose(const Rig::Pose& from, const Rig::Pose& to,
                    uint32_t elapsedMs, uint16_t durationMs);
bool validateClips();

}  // namespace Animation
}  // namespace PancettaCat
