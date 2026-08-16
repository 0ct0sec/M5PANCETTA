/**
 * Pancetta's cat — a deterministic actor, not decorative weather.
 *
 * This state machine owns visits, navigation, concealment, authored actions,
 * and the four-byte phoneme signal. Rooms provide geometry; the rig provides
 * anatomy; neither may improvise the cat's support line or visible history.
 */
#include "pancetta_cat.h"

#include "pancetta_cat_animation.h"
#include "pancetta_cat_rig.h"
#include "menu_pig_render.h"
#include "ui_measurements.h"
#include "../piglet/mood.h"

#include <math.h>
#include <string.h>
#include <time.h>

namespace PancettaCat {
namespace {

static constexpr int kPx = kCellPixels;
static constexpr uint32_t kFirstSignalDelayMs = 9000;
static constexpr uint32_t kSignalRetryMs = 12000;
static constexpr uint32_t kSignalRestMs = 48000;
static constexpr uint32_t kByteMs = 1300;
static constexpr uint8_t kMessageBytes = 4;
static constexpr uint32_t kSignalDurationMs = kByteMs * kMessageBytes;
static constexpr uint32_t kCompanyMinMs = 16000;
static constexpr uint32_t kCompanyJitterMs = 12000;
static constexpr uint32_t kFirstActionDelayMs = 22000;
static constexpr float kWalkSpeedPxPerSec = 66.0f;
static constexpr float kArrivalSpeedPxPerSec = 82.0f;
static constexpr float kTargetEpsilonPx = 1.25f;
static constexpr uint32_t kNavigationMaxStepMs = 80;
static constexpr float kVerticalArcPx = 8.0f;
static constexpr int kSurfaceJumpSpanPx = 13 * kPx;
static constexpr int kSurfaceHeightThresholdPx = 4 * kPx;
static constexpr uint16_t kPoseHandoffMs = 180;
static constexpr uint16_t kLocomotionHandoffMs = 90;

static_assert(kActionWidth == 52 * kPx && kActionHeight == 28 * kPx &&
                  kWidth == 26 * kPx && kHeight == 14 * kPx,
              "cat placement includes its one-cell guard band");
static_assert(kCompanyMinMs > kFirstSignalDelayMs + kSignalDurationMs,
              "a voluntary company visit must fit one complete transmission");

// The plaintext never appears on screen. Each byte is emitted as two rows of
// four cat phonemes so the player can recover these short ASCII messages.
static constexpr char kCipherMessages[][kMessageBytes + 1] = {
    "LOOK",
    "SAFE",
    "HOME",
    "EYES",
    "WAIT",
    "PING",
};
static constexpr uint8_t kCipherMessageCount =
    sizeof(kCipherMessages) / sizeof(kCipherMessages[0]);

static uint32_t nextSignalAt = 0;
static uint32_t signalStartedAt = 0;
static uint8_t signalMessage = 0;
static bool signalActive = false;

enum class SignalPhase : uint8_t {
    REST,
    SEEKING_SIT,
    ENTERING,
    TRANSMITTING,
    EXITING,
};

static SignalPhase signalPhase = SignalPhase::REST;
static bool signalAbortRequested = false;

enum class ArrivalPlan : uint8_t {
    SKIP_VISIT,
    ALREADY_THERE,
    LATE_ENTRY,
    CONCEALED,
};

enum class VisitPhase : uint8_t {
    ABSENT,
    ARRIVING,
    INDEPENDENT,
    APPROACHING_COMPANY,
    COMPANY,
    RETURNING_TO_INTEREST,
    DEPARTING,
};

static ArrivalPlan arrivalPlan = ArrivalPlan::ALREADY_THERE;
static VisitPhase visitPhase = VisitPhase::INDEPENDENT;

// Pushed by the room every frame; only the arrival roll and an emergence in
// progress ever read it.
static RoomStaging roomStaging = {};

// A concealed arrival is the only state where the room's own render order is
// not the whole truth about the cat, so the exposed span lives here beside the
// plan that created it rather than in the renderer.
static bool concealActive = false;
static int16_t concealX = 0;
static int16_t concealWidth = 0;
static bool concealEmergeRight = true;

// A cat inside the beam. He is not absent - the room he is arriving in has
// already been told he is coming - he simply has no body yet.
static bool portalTransit = false;

static bool roomSceneActive = false;
static bool catVisible = true;
static bool arrivalComplete = true;
static bool arrivalFromLeft = true;
static bool departureToRight = true;
static bool stayForVisit = true;
static bool companyPlanned = false;
static bool lastStationary = false;
static CompanyContext currentCompanyContext = CompanyContext::ORDINARY;
static uint8_t sceneRoom = 0xFFu;
static uint8_t roomVisit = 0;
static uint8_t actionSerial = 0;
static uint32_t revealAt = 0;
static uint32_t companyAt = 0;
static uint32_t companyUntil = 0;
static uint32_t departAt = 0;
static uint32_t nextActionAt = 0;
static uint32_t actionStartedAt = 0;
static Activity companyActivity = Activity::SLEEP;
static Activity plannedAction = Activity::FOLLOW;
static bool plannedActionNight = false;
static bool actionPending = false;
static bool actionActive = false;
static Memory pendingMemory = Memory::COUNT;
static bool memoryPending = false;

static bool clockSampleValid = false;
static bool clockSampleNight = false;
static uint32_t clockSampleAt = 0;

// Cat-owned scene geometry. Targets may be derived from room or Pancetta
// geometry, but only this navigation state is rendered.
static bool navigationInitialized = false;
static bool navigationNeedsSceneInit = true;
static bool navigationMoving = false;
static float navigationX = 0.0f;
static float navigationY = 0.0f;
static Pose navigationPose = {};
static uint32_t navigationUpdatedAt = 0;
static Activity currentAnimation = Activity::FOLLOW;
static bool currentAnimationMoving = false;
static uint32_t animationStartedAt = 0;
static Animation::Transition activeAnimationTransition =
    Animation::Transition::NONE;
static Animation::Transition queuedAnimationTransition =
    Animation::Transition::NONE;
static uint32_t animationTransitionStartedAt = 0;
static Rig::Pose animationHandoffFrom = Rig::Pose();
static uint32_t animationHandoffStartedAt = 0;
static uint16_t animationHandoffDurationMs = kPoseHandoffMs;
static bool animationHandoffActive = false;
static float zoomStartX = 0.0f;
static float zoomEndX = 0.0f;
static float zoomY = 0.0f;

enum class SurfaceTransitionPhase : uint8_t {
    NONE,
    LOADING,
    AIRBORNE,
    LANDING,
};

static bool verticalTransitionActive = false;
static SurfaceTransitionPhase verticalTransitionPhase =
    SurfaceTransitionPhase::NONE;
static uint32_t verticalTransitionStartedAt = 0;
static float verticalFromX = 0.0f;
static float verticalFromY = 0.0f;
static float verticalToX = 0.0f;
static float verticalToY = 0.0f;
static bool verticalFlightFaceRight = true;
static bool verticalLandingFaceRight = true;
static bool verticalTouchesPig = false;
static bool verticalPigIsDestination = false;

// Tiny fixed-point spring for attached coat tips.  The animation clip supplies
// the intent (rest, sweep, bristle); navigation supplies inertia.  Rendering
// consumes this snapshot and never mutates character state.
static int16_t furSweepQ8 = 0;
static int16_t furVelocityQ8 = 0;
static int16_t furLastX = 0;
static int16_t furLastY = 0;
static bool furInitialized = false;
static Rig::SecondaryMotion furMotion = {};
static Rig::MouthPose lastVoicedMouth = Rig::MouthPose::CLOSED;
static bool voicePending = false;

// ==[ BATTED TRINKET ]==
// Cats do not leave small objects alone. The room supplies an anchor on the
// surface the cat is visiting; this module owns everything the object then
// does. Constants are the ones validated by scripts/sim_cat_trinket.py, which
// checks settle time, bounce decay, roll travel, and playfield containment.
// Position and velocity are both Q4: integrating a sub-pixel roll into an
// integer position truncates to zero every tick and the object never moves.
static constexpr int kTrinketW = kTrinketWidth;
static constexpr int kTrinketH = 6;
// A topple is a quarter turn, so the drawn extents swap. Nothing lands on its
// base after being batted off a shelf, and a cylinder on its side is also the
// only reason the thing rolls at all.
static constexpr int kTrinketLyingW = kTrinketH;
static constexpr int kTrinketLyingH = kTrinketW;
static constexpr uint32_t kTrinketTickMs = 16;
static constexpr uint32_t kTrinketMaxCatchUpMs = 96;
static constexpr int kTrinketGravityQ4 = 5;
static constexpr int kTrinketMaxFallQ4 = 96;
static constexpr int kTrinketBounceNum = 2;
static constexpr int kTrinketBounceDen = 5;
static constexpr int kTrinketSettleQ4 = 12;
static constexpr uint8_t kTrinketMaxBounces = 2;
static constexpr int kTrinketRollQ4 = 28;
static constexpr int kTrinketRollFrictionNum = 15;
static constexpr int kTrinketRollFrictionDen = 16;
// The rock is a spring whose amplitude never approaches its clamp, so the
// impulse and the lean threshold - not the damping - decide whether a paw beat
// is visible at all. At impulse 6 / threshold 4 the tin tipped one way for two
// frames and never came back, which reads as a glitch rather than a rock.
static constexpr int kTrinketWobbleImpulse = 8;
static constexpr int kTrinketWobbleLimit = 8;
static constexpr int kTrinketWobbleVelLimit = 12;
static constexpr int kTrinketWobbleDampNum = 7;
static constexpr int kTrinketWobbleDampDen = 8;
static constexpr int kTrinketLeanThreshold = 3;

static bool trinketPresent = false;
static bool trinketKnockable = false;
static bool trinketKnocked = false;
static bool trinketFalling = false;
static bool trinketLying = false;
static bool trinketCapRight = true;
static bool trinketAnchorValid = false;
static int16_t trinketAnchorX = 0;
static int16_t trinketSupportY = 0;
static int32_t trinketXQ4 = 0;
static int32_t trinketYQ4 = 0;
static int16_t trinketFallQ4 = 0;
static int16_t trinketRollQ4 = 0;
static int8_t trinketWobble = 0;
static int8_t trinketWobbleVel = 0;
static uint8_t trinketBounces = 0;
static uint8_t trinketBatsSeen = 0;
static uint8_t trinketBatsToKnock = 2;
static bool trinketPawWasRaised = false;
static Rig::Gait trinketLastPawGait = Rig::Gait::PLANT;
static uint32_t trinketAccumMs = 0;
static uint32_t trinketUpdatedAt = 0;
static bool trinketBatPending = false;
static bool trinketKnockPending = false;
static bool trinketLandPending = false;

static bool deadlineReached(uint32_t now, uint32_t deadline) {
    return (int32_t)(now - deadline) >= 0;
}

static int clampInt(int value, int low, int high) {
    return value < low ? low : (value > high ? high : value);
}

static int snap2(int value) {
    return value & ~(kPx - 1);
}

static uint32_t transitionDuration(Animation::Transition transition) {
    const Animation::Clip* clip = Animation::clipFor(transition);
    return clip ? Animation::clipDuration(*clip) : 0u;
}

static Rig::Pose sampleAnimation(Activity activity, bool moving,
                                 bool signaling, uint32_t now,
                                 uint32_t signalElapsedMs);
static void queueMemory(Memory memory);

static Rig::Pose sampleRuntimeAnimation(uint32_t now) {
    return sampleAnimation(
        currentAnimation, currentAnimationMoving, signalActive, now,
        signalActive ? now - signalStartedAt : 0u);
}

static void beginPoseHandoff(const Rig::Pose& from, uint32_t now,
                             uint16_t durationMs = kPoseHandoffMs) {
    animationHandoffFrom = from;
    animationHandoffStartedAt = now;
    animationHandoffDurationMs = durationMs;
    animationHandoffActive = true;
}

static void beginAnimationTransition(
    const Animation::TransitionPlan& plan, uint32_t now,
    const Rig::Pose& from) {
    activeAnimationTransition = plan.first;
    queuedAnimationTransition = plan.second;
    animationTransitionStartedAt = now;
    animationStartedAt = now + transitionDuration(plan.first) +
                         transitionDuration(plan.second);
    beginPoseHandoff(from, now);
}

static bool signalInProgress() {
    return signalPhase != SignalPhase::REST;
}

static void beginNamedTransition(Animation::Transition transition,
                                 uint32_t now) {
    const Rig::Pose from = sampleRuntimeAnimation(now);
    Animation::TransitionPlan plan;
    plan.first = transition;
    beginAnimationTransition(plan, now, from);
}

static void beginSignalExit(uint32_t now) {
    // Capture the phoneme pose before clearing signalActive. A cancelled byte
    // may be anywhere in the signal loop, not only at its authored first key.
    const Rig::Pose from = sampleRuntimeAnimation(now);
    signalActive = false;
    signalStartedAt = 0;
    signalAbortRequested = false;
    signalPhase = SignalPhase::EXITING;
    Animation::TransitionPlan plan;
    plan.first = Animation::Transition::SIGNAL_TO_IDLE;
    beginAnimationTransition(plan, now, from);
}

static void cancelSignal(uint32_t now) {
    if (signalPhase == SignalPhase::REST ||
        signalPhase == SignalPhase::EXITING)
        return;
    nextSignalAt = now + kSignalRetryMs;
    if (signalPhase == SignalPhase::ENTERING) {
        // Finish the entry pose before reversing it. Replacing a half-sampled
        // clip with the exit start was the remaining visible signal pop.
        signalAbortRequested = true;
        return;
    }
    if (signalPhase == SignalPhase::TRANSMITTING) {
        beginSignalExit(now);
        return;
    }
    signalActive = false;
    signalStartedAt = 0;
    signalAbortRequested = false;
    signalPhase = SignalPhase::REST;
}

static void setCurrentAnimation(Activity activity, bool moving,
                                uint32_t now) {
    const bool activityMoving = activity == Activity::FOLLOW && moving;
    if (currentAnimation == activity &&
        currentAnimationMoving == activityMoving)
        return;
    const Rig::Pose from = sampleRuntimeAnimation(now);
    const bool locomotionToggle = currentAnimation == Activity::FOLLOW &&
                                  activity == Activity::FOLLOW;
    const Animation::TransitionPlan plan =
        Animation::planTransition(currentAnimation, activity);
    currentAnimation = activity;
    currentAnimationMoving = activityMoving;
    beginAnimationTransition(plan, now, from);
    if (locomotionToggle)
        animationHandoffDurationMs = kLocomotionHandoffMs;
}

static void restartCurrentAnimation(Activity activity, uint32_t now) {
    if (currentAnimation != activity) {
        setCurrentAnimation(activity, false, now);
        return;
    }
    // A scheduled one-shot may match the room's ambient activity (sleep is
    // common). Leave its parked final pose through the authored exit and entry
    // corridor instead of snapping straight back to the first action key.
    const Rig::Pose from = sampleRuntimeAnimation(now);
    currentAnimationMoving = false;
    beginAnimationTransition(Animation::planRestart(activity), now, from);
}

static void advanceAnimationTransition(uint32_t now) {
    while (activeAnimationTransition != Animation::Transition::NONE) {
        const uint32_t duration =
            transitionDuration(activeAnimationTransition);
        const uint32_t endAt = animationTransitionStartedAt + duration;
        if (!deadlineReached(now, endAt)) return;
        const Rig::Pose boundary = sampleRuntimeAnimation(endAt);
        if (queuedAnimationTransition != Animation::Transition::NONE) {
            activeAnimationTransition = queuedAnimationTransition;
            queuedAnimationTransition = Animation::Transition::NONE;
            animationTransitionStartedAt = endAt;
            beginPoseHandoff(boundary, endAt);
            continue;
        }
        activeAnimationTransition = Animation::Transition::NONE;
        animationTransitionStartedAt = 0;
        beginPoseHandoff(boundary, endAt);
    }
}

static void advanceSignalAnimation(uint32_t now) {
    if (activeAnimationTransition != Animation::Transition::NONE) return;

    if (signalPhase == SignalPhase::SEEKING_SIT &&
        currentAnimation == Activity::WATCH_RAIN) {
        signalPhase = SignalPhase::ENTERING;
        beginNamedTransition(Animation::Transition::SIT_TO_SIGNAL, now);
        return;
    }

    if (signalPhase == SignalPhase::ENTERING) {
        if (signalAbortRequested) {
            beginSignalExit(now);
            return;
        }
        signalPhase = SignalPhase::TRANSMITTING;
        signalActive = true;
        signalStartedAt = now;
        queueMemory(Memory::BINARY_MEOW);
        return;
    }

    if (signalPhase == SignalPhase::EXITING)
        signalPhase = SignalPhase::REST;
}

static Rig::Pose sampleAnimation(Activity activity, bool moving,
                                 bool signaling, uint32_t now,
                                 uint32_t signalElapsedMs) {
    Rig::Pose target;
    if (signaling) {
        target = Animation::sample(Animation::signalClip(), signalElapsedMs);
    } else {
        const Animation::Clip* transition =
            Animation::clipFor(activeAnimationTransition);
        if (transition) {
            target = Animation::sample(
                *transition, now - animationTransitionStartedAt);
        } else {
            const uint32_t elapsed = deadlineReached(now, animationStartedAt)
                ? now - animationStartedAt
                : 0u;
            target = Animation::sample(
                activity, moving, false, elapsed, 0u);
        }
    }

    if (!animationHandoffActive ||
        deadlineReached(now, animationHandoffStartedAt +
                             animationHandoffDurationMs))
        return target;
    return Animation::blendPose(
        animationHandoffFrom, target,
        now - animationHandoffStartedAt, animationHandoffDurationMs);
}

static int8_t furIntent(Rig::FurPose pose) {
    switch (pose) {
        case Rig::FurPose::DRAG_BACK: return -1;
        case Rig::FurPose::SWEEP_FORWARD: return 1;
        case Rig::FurPose::BRISTLE: return 0;
        case Rig::FurPose::REST:
        case Rig::FurPose::BREATHE:
        case Rig::FurPose::COMPRESS:
        default: return 0;
    }
}

// The voice belongs to the sampled frame, not to the schedule that requested
// the action. Latching on the mouth's own transition keeps audio locked to the
// visible gape even when a handoff blend or an interrupted clip shifts when
// that gape actually lands.
static void updateVoice(const Rig::Pose& pose) {
    if (pose.mouth == Rig::MouthPose::MEOW &&
        lastVoicedMouth != Rig::MouthPose::MEOW)
        voicePending = true;
    lastVoicedMouth = pose.mouth;
}

static bool pawIsRaised(const Rig::Pose& pose) {
    return pose.gait == Rig::Gait::PAW_LIFT ||
           pose.gait == Rig::Gait::SCRATCH_A ||
           pose.gait == Rig::Gait::SCRATCH_B;
}

static void planTrinketForVisit(uint32_t hash) {
    // Most visits have something to fiddle with; only some of those end with
    // it on the floor. A trinket that always fell would turn a running gag
    // into a chore.
    trinketPresent = (hash % 100u) < 72u;
    trinketKnockable = ((hash >> 7) % 100u) < 46u;
    trinketBatsToKnock = (uint8_t)(2u + ((hash >> 17) % 3u));
    trinketKnocked = false;
    trinketFalling = false;
    trinketLying = false;
    trinketCapRight = true;
    trinketAnchorValid = false;
    trinketFallQ4 = 0;
    trinketRollQ4 = 0;
    trinketWobble = 0;
    trinketWobbleVel = 0;
    trinketBounces = 0;
    trinketBatsSeen = 0;
    trinketPawWasRaised = false;
    trinketLastPawGait = Rig::Gait::PLANT;
    trinketAccumMs = 0;
    trinketUpdatedAt = 0;
    trinketBatPending = false;
    trinketKnockPending = false;
    trinketLandPending = false;
}

static int trinketWidth() {
    return trinketLying ? kTrinketLyingW : kTrinketW;
}

static int trinketHeight() {
    return trinketLying ? kTrinketLyingH : kTrinketH;
}

// Containment is measured against the drawn footprint, which grows by two
// pixels the moment the tin goes onto its side.
static void clampTrinketX() {
    const int32_t maxX = (int32_t)(SCREEN_WIDTH - trinketWidth()) * 16;
    if (trinketXQ4 < 0) {
        trinketXQ4 = 0;
        trinketRollQ4 = 0;
    } else if (trinketXQ4 > maxX) {
        trinketXQ4 = maxX;
        trinketRollQ4 = 0;
    }
}

// Every path into this function is the object meeting the floor: a fall that
// ran out of height, or a knock delivered at floor level with nowhere to fall.
// It runs once because the lying flag latches, which makes it the one honest
// place to announce the impact.
static void toppleTrinket() {
    if (trinketLying) return;
    trinketLying = true;
    trinketLandPending = true;
    // On its side it is no longer balanced on anything, so the rock stops.
    trinketWobble = 0;
    trinketWobbleVel = 0;
    // Direction comes from the latch set at knock time, not from the live roll
    // velocity: a tin that reached a wall mid-fall has already had its roll
    // zeroed and would otherwise land facing back the way it came.
    if (!trinketCapRight)
        trinketXQ4 -= (int32_t)(kTrinketLyingW - kTrinketW) * 16;
    trinketYQ4 = (int32_t)(trinketSupportY - kTrinketLyingH) * 16;
    clampTrinketX();
}

// A paw beat only disturbs what the paw can actually reach. The object is
// latched to its surface for the whole visit, so without this a rake at the
// far side of the room would tip over something the cat is nowhere near.
static bool trinketWithinPawReach() {
    const int catLeft = navigationPose.x;
    const int catRight = navigationPose.x + kWidth;
    const int nearX = (int)(trinketXQ4 / 16);
    const int farX = nearX + trinketWidth();
    int gap = 0;
    if (nearX > catRight) gap = nearX - catRight;
    else if (catLeft > farX) gap = catLeft - farX;
    if (gap > kTrinketReachPx) return false;
    // Standing beside it is not the same as standing over it. Paw and object
    // have to share one contact line, or a cat up on the counter reaches down
    // through the cabinet to bat something on the floor.
    const int rise = (navigationPose.y + kGroundContactY) - trinketSupportY;
    return rise <= kPx && rise >= -kPx;
}

static void stepTrinketPhysics() {
    if (trinketFalling) {
        trinketFallQ4 = (int16_t)min((int)trinketFallQ4 + kTrinketGravityQ4,
                                     kTrinketMaxFallQ4);
        trinketYQ4 += trinketFallQ4;
        int32_t floorQ4 =
            (int32_t)(trinketSupportY - trinketHeight()) * 16;
        if (trinketYQ4 >= floorQ4) {
            // Contact puts it on its side before it rests, so the bounce that
            // follows already answers to the lying floor line.
            if (!trinketLying) {
                toppleTrinket();
                floorQ4 = (int32_t)(trinketSupportY - trinketHeight()) * 16;
            }
            trinketYQ4 = floorQ4;
            if (trinketFallQ4 > kTrinketSettleQ4 &&
                trinketBounces < kTrinketMaxBounces) {
                trinketFallQ4 = (int16_t)(-(trinketFallQ4 *
                    kTrinketBounceNum / kTrinketBounceDen));
                ++trinketBounces;
            } else {
                trinketFallQ4 = 0;
                trinketFalling = false;
            }
        }
    }

    // Roll survives the landing. Coupling it to the fall meant a knock at
    // floor level settled on the first tick and never travelled at all.
    if (trinketRollQ4 != 0) {
        trinketXQ4 += trinketRollQ4;
        trinketRollQ4 = (int16_t)(trinketRollQ4 *
            kTrinketRollFrictionNum / kTrinketRollFrictionDen);
        clampTrinketX();
    }

    // A trinket still on its surface rocks from the last paw contact. Once it
    // is over the edge the fall and the roll are the whole motion.
    if (trinketKnocked) return;
    trinketWobbleVel = (int8_t)(trinketWobbleVel - trinketWobble);
    trinketWobbleVel = (int8_t)(trinketWobbleVel * kTrinketWobbleDampNum /
                                kTrinketWobbleDampDen);
    int wobble = trinketWobble + trinketWobbleVel;
    trinketWobble = (int8_t)clampInt(wobble, -kTrinketWobbleLimit,
                                     kTrinketWobbleLimit);
    // Integer damping has fixed points that are not zero, so an untouched
    // spring parks one step off centre and the tin keeps a permanent lean.
    // Snapping the last step is what makes a resting tin exactly upright.
    if (trinketWobble >= -1 && trinketWobble <= 1 &&
        trinketWobbleVel >= -1 && trinketWobbleVel <= 1) {
        trinketWobble = 0;
        trinketWobbleVel = 0;
    }
}

static void updateTrinket(uint32_t now, const Rig::Pose& pose) {
    if (!trinketPresent) return;

    // Placement belongs to the room, which knows what surface the cat is
    // visiting. Until it supplies an anchor there is nothing to bat.
    if (!trinketAnchorValid && !trinketKnocked) {
        trinketUpdatedAt = now;
        return;
    }
    if (!trinketKnocked) {
        trinketXQ4 = (int32_t)trinketAnchorX * 16;
        trinketYQ4 = (int32_t)(trinketSupportY - kTrinketH) * 16;
    }

    // An absent cat has no paw in the room. His rig keeps advancing off-screen
    // so he does not re-enter mid-clip, and none of what it does may reach a
    // thing he has walked out on.
    const bool raised = catVisible && pawIsRaised(pose);
    // Every stroke is its own contact. A scratch alternates SCRATCH_A and
    // SCRATCH_B without ever putting the paw back down, so latching on the
    // raised edge alone scored a four-stroke rake as a single bat: the tin
    // twitched once and then sat still while the cat kept working on it.
    const bool freshBeat =
        raised && (!trinketPawWasRaised || pose.gait != trinketLastPawGait);
    if (freshBeat && !trinketKnocked && trinketWithinPawReach()) {
        // A fresh paw beat rocks the object, and enough of them tip it over.
        trinketWobbleVel = (int8_t)clampInt(
            trinketWobbleVel + kTrinketWobbleImpulse,
            -kTrinketWobbleVelLimit, kTrinketWobbleVelLimit);
        // Every beat that only rocks it gets its own tick. The rock was the
        // one part of the gag with nothing audible attached, so a cat working
        // a tin through three strokes did it in total silence.
        trinketBatPending = true;
        if (++trinketBatsSeen >= trinketBatsToKnock && trinketKnockable) {
            trinketBatPending = false;
            trinketKnocked = true;
            trinketKnockPending = true;
            trinketFalling = trinketSupportY <
                UIMeasurements::MenuPigLayout::kFloorY;
            trinketRollQ4 = (int16_t)(navigationPose.faceRight
                ? kTrinketRollQ4 : -kTrinketRollQ4);
            trinketCapRight = navigationPose.faceRight;
            // Once it leaves the surface it answers to the floor, not to the
            // shelf, rim, or sofa arm it started on.
            trinketSupportY =
                (int16_t)UIMeasurements::MenuPigLayout::kFloorY;
            // A knock at floor level has nowhere to fall, so it goes over
            // where it stands rather than waiting for a landing that never
            // comes.
            if (!trinketFalling) toppleTrinket();
        }
    }
    trinketPawWasRaised = raised;
    if (raised) trinketLastPawGait = pose.gait;

    uint32_t elapsed = trinketUpdatedAt ? now - trinketUpdatedAt : 0u;
    trinketUpdatedAt = now;
    if (elapsed > kTrinketMaxCatchUpMs) elapsed = kTrinketMaxCatchUpMs;
    trinketAccumMs += elapsed;
    while (trinketAccumMs >= kTrinketTickMs) {
        trinketAccumMs -= kTrinketTickMs;
        stepTrinketPhysics();
    }
}

static void updateFurMotion(const Rig::Pose& pose) {
    if (!furInitialized) {
        furLastX = navigationPose.x;
        furLastY = navigationPose.y;
        furInitialized = true;
    }
    int dxCells = (navigationPose.x - furLastX) / kPx;
    int dyCells = (navigationPose.y - furLastY) / kPx;
    dxCells = max(-2, min(2, dxCells));
    dyCells = max(-1, min(1, dyCells));
    furLastX = navigationPose.x;
    furLastY = navigationPose.y;

    const int16_t targetQ8 =
        (int16_t)(max(-2, min(2, (int)furIntent(pose.fur) - dxCells)) * 256);
    furVelocityQ8 += (targetQ8 - furSweepQ8) / 4;
    furVelocityQ8 = (int16_t)(furVelocityQ8 * 3 / 4);
    furSweepQ8 += furVelocityQ8;
    furSweepQ8 = max((int16_t)-512, min((int16_t)512, furSweepQ8));
    furMotion.sweepCells = (int8_t)(furSweepQ8 >= 0
        ? (furSweepQ8 + 128) / 256
        : (furSweepQ8 - 128) / 256);
    furMotion.liftCells = (int8_t)-dyCells;
}

static uint8_t currentSignalByte(uint32_t now) {
    if (!signalActive) return 0;
    uint32_t elapsed = now - signalStartedAt;
    uint8_t byteIndex = (uint8_t)(elapsed / kByteMs);
    if (byteIndex >= kMessageBytes) byteIndex = kMessageBytes - 1;
    return (uint8_t)kCipherMessages[signalMessage][byteIndex];
}

static uint32_t visitHash(uint8_t room, uint8_t visit, uint32_t salt) {
    uint32_t v = (uint32_t)room * 0x45D9F3Bu ^
                 (uint32_t)visit * 0x9E3779B9u ^ salt;
    v ^= v >> 16;
    v *= 0x7FEB352Du;
    v ^= v >> 15;
    return v;
}

static void queueMemory(Memory memory) {
    if ((uint8_t)memory >= (uint8_t)Memory::COUNT || memoryPending) return;
    pendingMemory = memory;
    memoryPending = true;
}

static Memory memoryForActivity(Activity activity) {
    switch (activity) {
        case Activity::FACE_BUMP: return Memory::FACE_BUMP;
        case Activity::HEAD_NAP: return Memory::HEAD_NAP;
        case Activity::KNEAD: return Memory::KNEAD;
        case Activity::SLOW_BLINK: return Memory::SLOW_BLINK;
        case Activity::HAIRBALL: return Memory::HAIRBALL;
        case Activity::MEOW: return Memory::MEOW;
        default: return Memory::COUNT;
    }
}

static bool sampleNight(uint32_t now) {
    if (clockSampleValid && now - clockSampleAt < 60000u)
        return clockSampleNight;

    int hour = -1;
    time_t sysTime = time(nullptr);
    if (sysTime > 1704067200) {
        struct tm local = {};
        localtime_r(&sysTime, &local);
        hour = local.tm_hour;
    }
    if (hour < 0 && M5.Rtc.isEnabled()) {
        auto dt = M5.Rtc.getDateTime();
        if (dt.date.year >= 2024 && dt.time.hours < 24)
            hour = dt.time.hours;
    }

    // An unset clock does not invent night behavior. Once local time is
    // trusted, NIGHT matches the project TimeOfDay contract: 21:00-05:00.
    clockSampleNight = hour >= 21 || (hour >= 0 && hour < 5);
    clockSampleAt = now;
    clockSampleValid = true;
    return clockSampleNight;
}

static uint32_t actionDuration(Activity activity) {
    return Animation::actionDuration(activity);
}

static Activity actionForRoom(uint8_t room, uint8_t serial, bool night) {
    const uint8_t roll = (uint8_t)(
        visitHash(room, roomVisit, 0xF3111E5u + serial) % 24u);
    // Zoomies and defensive arches are real but rare. Maintenance behaviors
    // dominate, matching a relaxed cat's long rest/groom/explore rhythm.
    if (night && roll < 3u) return Activity::ZOOMIES;
    if (roll == 23u) return Activity::ARCH;
    if (roll == 22u) return Activity::HAIRBALL;
    if (roll == 20u || roll == 21u) return Activity::KNEAD;
    // Cats announce themselves to empty rooms. Keeping this rare stops the
    // ambient loop from turning into a demo reel for the loudest clip.
    if (roll == 18u || roll == 19u) return Activity::MEOW;
    switch (room) {
        case 0: return (roll & 1u) ? Activity::GROOM : Activity::SCRATCH;
        case 1: return Activity::SLEEP;
        case 2: return (roll & 1u) ? Activity::GROOM : Activity::SLEEP;
        case 3: return (roll & 1u) ? Activity::GROOM : Activity::SCRATCH;
        case 4: return (roll & 1u) ? Activity::GROOM : Activity::SCRATCH;
        case 5: return (roll & 1u) ? Activity::GROOM : Activity::SLEEP;
        default: return Activity::GROOM;
    }
}

static Activity companyActivityFor(CompanyContext context, uint32_t hash) {
    const uint8_t roll = (uint8_t)(hash % 8u);
    if (context == CompanyContext::RESTING) {
        if (roll < 4u) return Activity::HEAD_NAP;
        if (roll < 6u) return Activity::KNEAD;
        return Activity::SLOW_BLINK;
    }
    // Resting under a lid: everything a settled cat does beside Pancetta
    // except the one thing that has to sit on top of his head.
    if (context == CompanyContext::SHELTERED)
        return roll < 4u ? Activity::KNEAD : Activity::SLOW_BLINK;
    if (context == CompanyContext::BATHING)
        return roll < 5u ? Activity::SLOW_BLINK : Activity::GROOM;
    // Walking over to say something out loud is the warmest thing the cat
    // does, so it shares top billing with the face bump on ordinary company.
    if (roll < 3u) return Activity::FACE_BUMP;
    if (roll < 5u) return Activity::MEOW;
    if (roll < 7u) return Activity::SLOW_BLINK;
    return Activity::GROOM;
}

static void scheduleNextAction(uint32_t now) {
    const uint32_t h = visitHash(sceneRoom, roomVisit,
                                 0xC47A6E5Du + actionSerial);
    nextActionAt = now + 36000u + (h % 26000u);
}

static void syncNavigationPose() {
    navigationPose.x = (int16_t)snap2((int)lroundf(navigationX));
    navigationPose.y = (int16_t)snap2((int)lroundf(navigationY));
}

static bool hasSurfaceContact() {
    return !verticalTransitionActive ||
           verticalTransitionPhase != SurfaceTransitionPhase::AIRBORNE;
}

static void clearSurfaceTransition(uint32_t now) {
    const bool wasActive = verticalTransitionActive;
    verticalTransitionActive = false;
    verticalTransitionPhase = SurfaceTransitionPhase::NONE;
    verticalTouchesPig = false;
    verticalPigIsDestination = false;
    if (!wasActive) return;
    activeAnimationTransition = Animation::Transition::NONE;
    queuedAnimationTransition = Animation::Transition::NONE;
    animationTransitionStartedAt = 0;
    animationHandoffActive = false;
    currentAnimation = Activity::FOLLOW;
    currentAnimationMoving = false;
    animationStartedAt = now;
}

static void beginPlannedAction(uint32_t now) {
    if (!actionPending) return;
    actionPending = false;
    actionActive = true;
    restartCurrentAnimation(plannedAction, now);
    // The action clock starts after its update-owned entry transition. This
    // prevents a sit, arch, or pounce lead-in from stealing the action's last
    // keys or moving the zoom path before the cat has loaded its feet.
    actionStartedAt = animationStartedAt;

    Memory memory = memoryForActivity(plannedAction);
    if (plannedAction == Activity::ZOOMIES && plannedActionNight)
        memory = Memory::NIGHT_ZOOMIES;
    queueMemory(memory);

    if (plannedAction == Activity::ZOOMIES) {
        zoomStartX = navigationX;
        zoomY = navigationY;
        zoomEndX =
            navigationX + kWidth / 2 < SCREEN_WIDTH / 2
                ? (float)(SCREEN_WIDTH - kWidth - kPx)
                : (float)kPx;
        navigationPose.faceRight = zoomEndX > zoomStartX;
        navigationMoving = true;
    }
}

static void finishActiveAction(uint32_t now) {
    if (plannedAction == Activity::ZOOMIES) {
        navigationX = zoomEndX;
        navigationY = zoomY;
        syncNavigationPose();
    }
    clearSurfaceTransition(now);
    actionActive = false;
    actionPending = false;
    plannedAction = Activity::FOLLOW;
    plannedActionNight = false;
    ++actionSerial;
    scheduleNextAction(now);
}

static void advanceNavigation(uint32_t now, const Pose& target) {
    uint32_t dt = navigationUpdatedAt ? now - navigationUpdatedAt : 0u;
    navigationUpdatedAt = now;
    if (dt > kNavigationMaxStepMs) dt = kNavigationMaxStepMs;

    const float dx = (float)target.x - navigationX;
    const float dy = (float)target.y - navigationY;
    const float distance = sqrtf(dx * dx + dy * dy);
    if (distance <= kTargetEpsilonPx) {
        navigationX = (float)target.x;
        navigationY = (float)target.y;
        navigationPose.faceRight = target.faceRight;
        navigationMoving = false;
        syncNavigationPose();
        return;
    }

    const float speed =
        arrivalComplete ? kWalkSpeedPxPerSec : kArrivalSpeedPxPerSec;
    const float step = speed * (float)dt / 1000.0f;
    if (step >= distance) {
        navigationX = (float)target.x;
        navigationY = (float)target.y;
        navigationPose.faceRight = target.faceRight;
        navigationMoving = false;
    } else if (step > 0.0f) {
        navigationX += dx / distance * step;
        navigationY += dy / distance * step;
        if (fabsf(dx) >= kPx)
            navigationPose.faceRight = dx > 0.0f;
        navigationMoving = true;
    } else {
        navigationMoving = true;
    }
    syncNavigationPose();
}

static void beginVerticalTransition(uint32_t now, float toX, float toY,
                                    bool faceRight) {
    const Rig::Pose from = sampleRuntimeAnimation(now);
    verticalTransitionActive = true;
    verticalTransitionPhase = SurfaceTransitionPhase::LOADING;
    verticalTransitionStartedAt = now;
    verticalFromX = navigationX;
    verticalFromY = navigationY;
    verticalToX = toX;
    verticalToY = toY;
    verticalFlightFaceRight = fabsf(toX - navigationX) >= kPx
        ? toX > navigationX
        : faceRight;
    verticalLandingFaceRight = faceRight;
    verticalTouchesPig = companyActivity == Activity::HEAD_NAP &&
        (visitPhase == VisitPhase::APPROACHING_COMPANY ||
         visitPhase == VisitPhase::RETURNING_TO_INTEREST);
    verticalPigIsDestination = verticalTouchesPig && toY < navigationY;
    navigationPose.faceRight = verticalFlightFaceRight;
    navigationMoving = true;

    // Surface travel owns a real three-part pose corridor. The old path moved
    // world Y immediately under the ordinary walk loop, so the cat trotted
    // vertically through the furniture and became settled before his paws had
    // landed. Load first, fly with the authored pounce-to-land clip, then keep
    // navigation busy until the landing recovery has planted all four paws.
    currentAnimation = Activity::FOLLOW;
    currentAnimationMoving = false;
    Animation::TransitionPlan plan;
    plan.first = Animation::Transition::CROUCH_TO_POUNCE;
    beginAnimationTransition(plan, now, from);
}

static void advanceVerticalTransition(uint32_t now) {
    switch (verticalTransitionPhase) {
        case SurfaceTransitionPhase::LOADING: {
            const uint32_t loadMs = transitionDuration(
                Animation::Transition::CROUCH_TO_POUNCE);
            if (!deadlineReached(now,
                                 verticalTransitionStartedAt + loadMs))
                break;
            verticalTransitionPhase = SurfaceTransitionPhase::AIRBORNE;
            verticalTransitionStartedAt = now;
            beginNamedTransition(Animation::Transition::POUNCE_TO_LAND, now);
            break;
        }
        case SurfaceTransitionPhase::AIRBORNE: {
            const uint32_t flightMs = transitionDuration(
                Animation::Transition::POUNCE_TO_LAND);
            float t = flightMs
                ? (float)(now - verticalTransitionStartedAt) /
                      (float)flightMs
                : 1.0f;
            if (t >= 1.0f) {
                navigationX = verticalToX;
                navigationY = verticalToY;
                navigationPose.faceRight = verticalLandingFaceRight;
                verticalTransitionPhase = SurfaceTransitionPhase::LANDING;
                verticalTransitionStartedAt = now;
                beginNamedTransition(Animation::Transition::LAND_TO_STAND,
                                     now);
            } else {
                if (t < 0.0f) t = 0.0f;
                const float eased = t * t * (3.0f - 2.0f * t);
                navigationX =
                    verticalFromX + (verticalToX - verticalFromX) * eased;
                navigationY =
                    verticalFromY + (verticalToY - verticalFromY) * eased -
                    sinf(t * 3.14159265f) * kVerticalArcPx;
                navigationPose.faceRight = verticalFlightFaceRight;
            }
            break;
        }
        case SurfaceTransitionPhase::LANDING: {
            const uint32_t settleMs = transitionDuration(
                Animation::Transition::LAND_TO_STAND);
            if (!deadlineReached(now,
                                 verticalTransitionStartedAt + settleMs))
                break;
            verticalTransitionActive = false;
            verticalTransitionPhase = SurfaceTransitionPhase::NONE;
            verticalTouchesPig = false;
            verticalPigIsDestination = false;
            navigationPose.faceRight = verticalLandingFaceRight;
            navigationMoving = false;
            break;
        }
        case SurfaceTransitionPhase::NONE:
        default:
            verticalTransitionActive = false;
            verticalTouchesPig = false;
            verticalPigIsDestination = false;
            navigationMoving = false;
            break;
    }
    navigationUpdatedAt = now;
    syncNavigationPose();
}

static int surfaceApproachX(const Pose& target) {
    const int minX = kPx;
    const int maxX = SCREEN_WIDTH - kWidth - kPx;
    const int preferred = clampInt(
        target.x + (target.faceRight ? -kSurfaceJumpSpanPx
                                    : kSurfaceJumpSpanPx),
        minX, maxX);
    const int alternate = clampInt(
        target.x + (target.faceRight ? kSurfaceJumpSpanPx
                                    : -kSurfaceJumpSpanPx),
        minX, maxX);
    const int preferredSpan = abs(preferred - target.x);
    const int alternateSpan = abs(alternate - target.x);
    return snap2(alternateSpan > preferredSpan ? alternate : preferred);
}

static float surfaceDropX(const Pose& target) {
    float dx = (float)target.x - navigationX;
    int direction = fabsf(dx) >= kPx
        ? (dx > 0.0f ? 1 : -1)
        : (target.faceRight ? 1 : -1);
    int landing = clampInt(
        (int)lroundf(navigationX) + direction * kSurfaceJumpSpanPx,
        kPx, SCREEN_WIDTH - kWidth - kPx);
    if (abs(landing - (int)lroundf(navigationX)) < kPx) {
        landing = clampInt(
            (int)lroundf(navigationX) - direction * kSurfaceJumpSpanPx,
            kPx, SCREEN_WIDTH - kWidth - kPx);
    }
    return (float)snap2(landing);
}

static void advanceSceneNavigation(uint32_t now, const Pose& target,
                                   bool pigMoving, bool helperScene) {
    if (verticalTransitionActive) {
        advanceVerticalTransition(now);
        return;
    }

    const bool supportedRoomTravel = !helperScene;
    (void)pigMoving;
    const float dy = (float)target.y - navigationY;
    // A target can move by one or two cat cells as nearby actors and authored
    // idle offsets settle. Treating every 2px correction as a new support made
    // the cat repeatedly crouch and jump at ordinary floor-level anchors.
    // Real furniture/crown changes are much taller than this threshold.
    if (!supportedRoomTravel || fabsf(dy) < kSurfaceHeightThresholdPx) {
        advanceNavigation(now, target);
        return;
    }

    if (dy < 0.0f) {
        // Approach from the side that matches the authored landing pose. The
        // previous exact-X approach made every climb a straight rise through
        // the receiving surface, even though the world path called it a jump.
        Pose approach = target;
        approach.x = (int16_t)surfaceApproachX(target);
        approach.y = (int16_t)snap2((int)lroundf(navigationY));
        approach.faceRight = target.x >= approach.x;
        advanceNavigation(now, approach);
        if (!navigationMoving) {
            beginVerticalTransition(now, (float)target.x,
                                    (float)target.y, target.faceRight);
        }
        return;
    }

    // Clear the edge in the direction of travel before walking away. A pure
    // vertical drop put the whole body through the support it had just left.
    beginVerticalTransition(now, surfaceDropX(target), (float)target.y,
                            target.faceRight);
}

static Activity chooseAnimation(Activity roomDefault, bool pigMoving,
                                bool helperScene) {
    // The signal owns one seated corridor: settle, enter, transmit, and exit.
    // Keeping WATCH_RAIN as its stable seat prevents company sleep/groom from
    // replacing either authored signal transition halfway through.
    if (signalInProgress()) return Activity::WATCH_RAIN;
    if (actionActive) return plannedAction;
    if (visitPhase == VisitPhase::COMPANY) return companyActivity;
    if (visitPhase == VisitPhase::ARRIVING ||
        visitPhase == VisitPhase::APPROACHING_COMPANY ||
        visitPhase == VisitPhase::RETURNING_TO_INTEREST ||
        visitPhase == VisitPhase::DEPARTING)
        return Activity::FOLLOW;
    if (navigationMoving || actionPending || helperScene) {
        if (!actionPending && !helperScene &&
            roomDefault == Activity::PROWL_BAR)
            return Activity::PROWL_BAR;
        return Activity::FOLLOW;
    }
    (void)pigMoving;
    return roomDefault;
}

// Warmth moves the weights of the arrival roll; it never replaces the roll.
// A perfect session can still walk in on a cat who was already behind the
// sofa, and a cold one can still get a cat waiting in the open - the point is
// that the room feels different when the session is going well, not that it
// becomes predictable. Hiding needs somewhere to hide: with no occluder the
// weight folds into a late entrance instead of vanishing from the table.
static ArrivalPlan planArrival(uint8_t roll) {
    const int warmth = clampInt((int)roomStaging.warmth, 0, 100);
    const int skipW = 22 - warmth * 12 / 100;
    const int concealW =
        roomStaging.hideValid ? (34 - warmth * 22 / 100) : 0;
    const int alreadyW = 20 + warmth * 20 / 100;
    if (roll < skipW) return ArrivalPlan::SKIP_VISIT;
    if (roll < skipW + concealW) return ArrivalPlan::CONCEALED;
    if (roll < skipW + concealW + alreadyW) return ArrivalPlan::ALREADY_THERE;
    return ArrivalPlan::LATE_ENTRY;
}

}  // namespace

void setRoomStaging(const RoomStaging& staging) {
    roomStaging = staging;
    // Room furniture drifts with parallax, so an emergence already in progress
    // tracks the edge it is actually coming out from instead of the snapshot
    // taken when the plan was rolled.
    if (concealActive && staging.hideValid) {
        concealX = staging.hideX;
        concealWidth = staging.hideWidth;
    }
}

void reset(uint32_t now) {
    signalActive = false;
    signalPhase = SignalPhase::REST;
    signalAbortRequested = false;
    signalStartedAt = 0;
    signalMessage = 0;
    nextSignalAt = now + kFirstSignalDelayMs;
    arrivalPlan = ArrivalPlan::ALREADY_THERE;
    visitPhase = VisitPhase::INDEPENDENT;
    roomSceneActive = false;
    catVisible = true;
    concealActive = false;
    concealX = 0;
    concealWidth = 0;
    concealEmergeRight = true;
    portalTransit = false;
    arrivalComplete = true;
    arrivalFromLeft = true;
    departureToRight = true;
    stayForVisit = true;
    companyPlanned = false;
    lastStationary = false;
    currentCompanyContext = CompanyContext::ORDINARY;
    sceneRoom = 0xFFu;
    roomVisit = 0;
    actionSerial = 0;
    revealAt = now;
    companyAt = 0;
    companyUntil = 0;
    departAt = 0;
    nextActionAt = now + kFirstActionDelayMs;
    actionStartedAt = 0;
    companyActivity = Activity::SLEEP;
    plannedAction = Activity::FOLLOW;
    plannedActionNight = false;
    actionPending = false;
    actionActive = false;
    pendingMemory = Memory::COUNT;
    memoryPending = false;
    clockSampleValid = false;
    clockSampleNight = false;
    clockSampleAt = 0;
    navigationInitialized = false;
    navigationNeedsSceneInit = true;
    navigationMoving = false;
    navigationX = 0.0f;
    navigationY = 0.0f;
    navigationPose = {};
    navigationUpdatedAt = now;
    currentAnimation = Activity::FOLLOW;
    currentAnimationMoving = false;
    animationStartedAt = now;
    activeAnimationTransition = Animation::Transition::NONE;
    queuedAnimationTransition = Animation::Transition::NONE;
    animationTransitionStartedAt = 0;
    animationHandoffFrom = Rig::Pose();
    animationHandoffStartedAt = 0;
    animationHandoffDurationMs = kPoseHandoffMs;
    animationHandoffActive = false;
    zoomStartX = 0.0f;
    zoomEndX = 0.0f;
    zoomY = 0.0f;
    verticalTransitionActive = false;
    verticalTransitionPhase = SurfaceTransitionPhase::NONE;
    verticalTransitionStartedAt = 0;
    verticalFromX = 0.0f;
    verticalFromY = 0.0f;
    verticalToX = 0.0f;
    verticalToY = 0.0f;
    verticalFlightFaceRight = true;
    verticalLandingFaceRight = true;
    verticalTouchesPig = false;
    verticalPigIsDestination = false;
    furSweepQ8 = 0;
    furVelocityQ8 = 0;
    furLastX = 0;
    furLastY = 0;
    furInitialized = false;
    furMotion = {};
    lastVoicedMouth = Rig::MouthPose::CLOSED;
    voicePending = false;
    planTrinketForVisit(0u);
    trinketPresent = false;
}

void update(uint32_t now, bool channelClear, bool roomScene,
            uint8_t room, bool stationary, CompanyContext companyContext) {
    currentCompanyContext = companyContext;
    if (!roomScene) {
        roomSceneActive = false;
        // A companion inside the beam has no body in the scene he is leaving
        // either. Everything else about a non-room scene still resets.
        catVisible = !portalTransit;
        concealActive = false;
        arrivalComplete = true;
        visitPhase = VisitPhase::INDEPENDENT;
        companyPlanned = false;
        actionPending = false;
        actionActive = false;
        plannedAction = Activity::FOLLOW;
        plannedActionNight = false;
        signalActive = false;
        signalPhase = SignalPhase::REST;
        signalAbortRequested = false;
        signalStartedAt = 0;
        trinketPresent = false;
        trinketAnchorValid = false;
    } else if (!roomSceneActive || sceneRoom != room) {
        roomSceneActive = true;
        sceneRoom = room;
        ++roomVisit;
        actionSerial = 0;
        concealActive = false;
        actionPending = false;
        actionActive = false;
        plannedAction = Activity::FOLLOW;
        plannedActionNight = false;
        lastStationary = false;
        navigationNeedsSceneInit = true;
        clearSurfaceTransition(now);
        signalActive = false;
        signalPhase = SignalPhase::REST;
        signalAbortRequested = false;
        signalStartedAt = 0;
        nextSignalAt = now + kFirstSignalDelayMs;

        const uint32_t h = visitHash(room, roomVisit, 0xA11CE55u);
        planTrinketForVisit(visitHash(room, roomVisit, 0x7B17E75u));
        arrivalPlan = planArrival((uint8_t)(h % 100u));
        arrivalFromLeft = (h & 2u) == 0u;
        departureToRight = (h & 4u) != 0u;
        stayForVisit = ((h >> 8) % 100u) < 55u;
        companyPlanned = arrivalPlan != ArrivalPlan::SKIP_VISIT &&
                         ((h >> 15) % 100u) < 38u;
        companyActivity = Activity::SLOW_BLINK;
        revealAt = now + 3800u + ((h >> 3) % 12u) * 420u;
        companyAt = revealAt + 16000u + ((h >> 11) % 24000u);
        departAt = revealAt + 56000u + ((h >> 19) % 48000u);

        if (arrivalPlan == ArrivalPlan::ALREADY_THERE) {
            catVisible = true;
            arrivalComplete = true;
            revealAt = now;
            visitPhase = VisitPhase::INDEPENDENT;
        } else if (arrivalPlan == ArrivalPlan::LATE_ENTRY) {
            catVisible = false;
            arrivalComplete = false;
            visitPhase = VisitPhase::ABSENT;
        } else if (arrivalPlan == ArrivalPlan::CONCEALED) {
            // He was here before Pancetta was. Nothing of him is on screen
            // until the reveal, and even then only the part that has cleared
            // the furniture he was waiting behind.
            catVisible = false;
            arrivalComplete = false;
            visitPhase = VisitPhase::ABSENT;
            concealActive = true;
            concealX = roomStaging.hideX;
            concealWidth = roomStaging.hideWidth;
            concealEmergeRight = roomStaging.hideEmergeRight;
        } else {
            catVisible = false;
            arrivalComplete = true;
            visitPhase = VisitPhase::ABSENT;
            companyPlanned = false;
        }
        nextActionAt = revealAt + kFirstActionDelayMs;

        if (portalTransit) {
            // He came through with Pancetta, so this room does not get to
            // decide whether he showed up - only what he does once the
            // particles put him down.
            arrivalPlan = ArrivalPlan::ALREADY_THERE;
            concealActive = false;
            catVisible = false;
            arrivalComplete = true;
            visitPhase = VisitPhase::INDEPENDENT;
            revealAt = now;
            nextActionAt = now + kFirstActionDelayMs;
        }
    }

    if (portalTransit) {
        // Nothing in the beam has a station, a channel, or an opinion about
        // the room yet. Landing is what starts the visit.
        lastStationary = stationary;
        cancelSignal(now);
        return;
    }

    if (roomSceneActive) {
        // A staged entrance is owed exactly once per visit. `arrivalComplete`
        // is that debt: false only between the room cut and the frame he
        // finishes walking in. Without it the same plan re-armed behind a cat
        // who had already left, walked him back on screen, and stranded him in
        // an ARRIVING phase that can no longer complete - so no action,
        // company, or transmission is ever scheduled again this visit.
        if (!catVisible && !arrivalComplete &&
            (arrivalPlan == ArrivalPlan::LATE_ENTRY ||
             arrivalPlan == ArrivalPlan::CONCEALED) &&
            visitPhase == VisitPhase::ABSENT &&
            deadlineReached(now, revealAt)) {
            catVisible = true;
            visitPhase = VisitPhase::ARRIVING;
        }

        // Company is voluntary and low intensity. If Pancetta leaves, the cat
        // does not chase; he returns to his room interest on his own path.
        if (!stationary && lastStationary &&
            (visitPhase == VisitPhase::APPROACHING_COMPANY ||
             visitPhase == VisitPhase::COMPANY)) {
            visitPhase = VisitPhase::RETURNING_TO_INTEREST;
            companyPlanned = false;
            companyUntil = 0;
            cancelSignal(now);
        }

        if (actionActive && deadlineReached(now, actionStartedAt) &&
            now - actionStartedAt >= actionDuration(plannedAction)) {
            finishActiveAction(now);
        }

        if (visitPhase == VisitPhase::COMPANY &&
            companyUntil && deadlineReached(now, companyUntil)) {
            visitPhase = VisitPhase::RETURNING_TO_INTEREST;
            companyPlanned = false;
            companyUntil = 0;
        }

        if (visitPhase == VisitPhase::INDEPENDENT && companyPlanned &&
            stationary && !actionPending && !actionActive &&
            !signalInProgress() &&
            deadlineReached(now, companyAt)) {
            companyActivity = companyActivityFor(
                currentCompanyContext,
                visitHash(sceneRoom, roomVisit, 0xFACEB00Cu + actionSerial));
            visitPhase = VisitPhase::APPROACHING_COMPANY;
        }

        if (!stayForVisit && visitPhase == VisitPhase::INDEPENDENT &&
            !actionPending && !actionActive && !signalInProgress() &&
            deadlineReached(now, departAt)) {
            visitPhase = VisitPhase::DEPARTING;
            companyPlanned = false;
        }

        if (!actionPending && !actionActive && !signalInProgress() &&
            stationary &&
            catVisible && arrivalComplete &&
            visitPhase == VisitPhase::INDEPENDENT &&
            deadlineReached(now, nextActionAt)) {
            plannedActionNight = sampleNight(now);
            plannedAction =
                actionForRoom(sceneRoom, actionSerial, plannedActionNight);
            actionPending = true;
        }
    }
    lastStationary = stationary;

    const bool effectiveChannelClear =
        channelClear && catVisible && arrivalComplete &&
        visitPhase == VisitPhase::COMPANY &&
        !actionPending && !actionActive && !navigationMoving;
    if (!effectiveChannelClear) {
        cancelSignal(now);
        return;
    }

    if (signalPhase == SignalPhase::TRANSMITTING) {
        if (now - signalStartedAt >= kSignalDurationMs) {
            signalMessage = (uint8_t)((signalMessage + 1) % kCipherMessageCount);
            nextSignalAt = now + kSignalRestMs;
            beginSignalExit(now);
        }
        return;
    }

    if (signalPhase == SignalPhase::REST &&
        deadlineReached(now, nextSignalAt))
        signalPhase = SignalPhase::SEEKING_SIT;
}

bool isTransmitting() {
    return signalActive;
}

bool isVisible() {
    return catVisible;
}

void beginPortalTransit(uint32_t now) {
    portalTransit = true;
    catVisible = false;
    concealActive = false;
    cancelSignal(now);
    actionPending = false;
    actionActive = false;
    plannedAction = Activity::FOLLOW;
    plannedActionNight = false;
    clearSurfaceTransition(now);
    navigationMoving = false;
    // Whatever he was batting around belongs to the room he is leaving. It is
    // furniture's business, not his, and it does not ride the beam.
    trinketPresent = false;
    trinketAnchorValid = false;
}

void endPortalTransit(uint32_t now, const Pose& landing) {
    if (!portalTransit) return;
    portalTransit = false;
    catVisible = true;
    arrivalComplete = true;
    visitPhase = VisitPhase::INDEPENDENT;
    // However the destination room happened to roll its own visit, a cat who
    // stepped out of the portal is standing in the open, not behind anything.
    arrivalPlan = ArrivalPlan::ALREADY_THERE;
    concealActive = false;
    // The particles decided where he is; navigation continues from there
    // rather than snapping to whatever anchor the new room had in mind.
    navigationX = (float)landing.x;
    navigationY = (float)landing.y;
    navigationPose.faceRight = landing.faceRight;
    navigationInitialized = true;
    navigationNeedsSceneInit = false;
    // He is put down beside Pancetta, not on whatever the room wanted him to
    // be doing, so he still has somewhere to walk. The next navigation tick
    // settles this the moment he actually has not — claiming settled here
    // would offer the room a surface at a spot he has not reached.
    navigationMoving = true;
    navigationUpdatedAt = now;
    clearSurfaceTransition(now);
    syncNavigationPose();
    scheduleNextAction(now);
}

bool inPortalTransit() {
    return portalTransit;
}

bool concealClip(int16_t& x, int16_t& width) {
    if (!concealActive) return false;
    if (concealEmergeRight) {
        const int edge = clampInt(concealX + concealWidth, 0, SCREEN_WIDTH);
        x = (int16_t)edge;
        width = (int16_t)(SCREEN_WIDTH - edge);
    } else {
        x = 0;
        width = (int16_t)clampInt(concealX, 0, SCREEN_WIDTH);
    }
    return true;
}

bool concealExitPose(Pose& pose) {
    if (!concealActive || !catVisible) return false;
    // One clear cat cell past the edge: far enough that the release test has
    // already fired when he stops, close enough that he is not crossing the
    // room before the room's own anchor gets a say again.
    constexpr int kClearancePx = kPx * 4;
    const int exitX = concealEmergeRight
        ? concealX + concealWidth + kClearancePx
        : concealX - kClearancePx - kWidth;
    pose.x = (int16_t)snap2(
        clampInt(exitX, kPx, SCREEN_WIDTH - kPx - kWidth));
    pose.y = (int16_t)snap2(originYForSupport(
        UIMeasurements::MenuPigLayout::kFloorY));
    pose.faceRight = concealEmergeRight;
    return true;
}

SceneIntent sceneIntent() {
    if (visitPhase == VisitPhase::APPROACHING_COMPANY ||
        visitPhase == VisitPhase::COMPANY)
        return SceneIntent::COMPANY;
    if (visitPhase == VisitPhase::DEPARTING)
        return SceneIntent::EXIT;
    return SceneIntent::ROOM_INTEREST;
}

bool exitsRight() {
    return departureToRight;
}

// A social visit is one continuous piece of staging: he crosses to Pancetta,
// he is with him, he goes back to his own business. The plane he is drawn on
// belongs to the whole of that, not only to the half of it after contact -
// arriving on the far plane put him behind Pancetta for the entire approach
// and then popped him in front on the frame he landed.
bool drawsAbovePig() {
    const bool nearCompany =
        companyActivity == Activity::FACE_BUMP ||
        companyActivity == Activity::HEAD_NAP;
    const bool nearPhase =
        visitPhase == VisitPhase::APPROACHING_COMPANY ||
        visitPhase == VisitPhase::COMPANY ||
        visitPhase == VisitPhase::RETURNING_TO_INTEREST;
    return catVisible &&
           ((currentAnimation == Activity::FACE_BUMP ||
             currentAnimation == Activity::HEAD_NAP) ||
            (nearCompany && nearPhase));
}

// Pancetta is the cat's support only while the cat is actually up there: the
// climb, the nap, and the drop back down. The floor walks at either end are
// ordinary room travel and owe the floor an ordinary contact shadow, so the
// phase alone cannot answer this - a nap approach that claimed the crown early
// dragged a shadow up across Pancetta's face, and one that gave it back late
// left the cat crossing the room on nothing.
bool restsOnPig() {
    if (!catVisible) return false;
    if (currentAnimation == Activity::HEAD_NAP) return true;
    if (companyActivity != Activity::HEAD_NAP) return false;
    // Seated on the crown. Read through companyActivity because a transmission
    // replaces the nap clip with its own seated corridor without moving him.
    if (visitPhase == VisitPhase::COMPANY) return true;
    if (!verticalTransitionActive || !verticalTouchesPig) return false;
    // Physical endpoints own support even if Pancetta starts moving and flips
    // the visit from APPROACHING to RETURNING before this jump has finished.
    // Loading belongs to the launch support; touchdown belongs to the receiver.
    return verticalPigIsDestination
        ? verticalTransitionPhase == SurfaceTransitionPhase::LANDING
        : verticalTransitionPhase == SurfaceTransitionPhase::LOADING;
}

void setTrinketAnchor(int16_t x, int16_t supportY, bool valid) {
    // A knocked trinket has left the shelf; the room no longer owns it.
    if (trinketKnocked) return;
    // The first valid placement of a visit is the one that sticks. Re-placing
    // the object every time the cat settled somewhere new made it teleport
    // across the room after him, and dropping it the moment he stood up made
    // a thing sitting on a shelf blink out of the scene - for the whole
    // company phase, every time he went to sit with Pancetta.
    if (trinketAnchorValid || !valid) return;
    // A station hard against a wall has no room beside it: the object would be
    // clamped back under the cat or off the playfield. Nothing is placed there;
    // the next station the cat settles at gets the chance instead.
    if (x < 0 || x + kTrinketW > SCREEN_WIDTH) return;
    trinketAnchorX = (int16_t)snap2(x);
    trinketSupportY = supportY;
    trinketAnchorValid = true;
}

bool trinketVisible() {
    return trinketPresent && (trinketAnchorValid || trinketKnocked);
}

bool consumeTrinketBat() {
    if (!trinketBatPending) return false;
    trinketBatPending = false;
    return true;
}

bool consumeTrinketKnock() {
    if (!trinketKnockPending) return false;
    trinketKnockPending = false;
    return true;
}

bool consumeTrinketLanded() {
    if (!trinketLandPending) return false;
    trinketLandPending = false;
    return true;
}

// The one place the Q4 state becomes a drawn rectangle. The lighting seam and
// the renderer have to agree on it exactly, or the object is shaded for a
// position two pixels from the one it occupies.
static void trinketDrawOrigin(int& x, int& y) {
    x = clampInt(snap2((int)(trinketXQ4 / 16)), 0,
                 SCREEN_WIDTH - trinketWidth());
    y = snap2((int)(trinketYQ4 / 16));
}

bool trinketCenter(int16_t& x, int16_t& y) {
    if (!trinketVisible()) return false;
    int originX = 0;
    int originY = 0;
    trinketDrawOrigin(originX, originY);
    x = (int16_t)(originX + trinketWidth() / 2);
    y = (int16_t)(originY + trinketHeight() / 2);
    return true;
}

void drawTrinket(M5Canvas& canvas, const PigLight& light) {
    if (!trinketVisible()) return;
    const int w = trinketWidth();
    int x = 0;
    int y = 0;
    trinketDrawOrigin(x, y);

    // A small tin: bright cap, body, and a grounded base. It borrows the
    // room's practical the same way the cat's coat does, so it never reads as
    // a sticker pasted over the scene.
    uint16_t body = Display::lerpColor565(
        MenuPigRender::RP::WALL_NEAR, MenuPigRender::RP::DEEP, 0.35f);
    uint16_t cap = Display::lerpColor565(
        MenuPigRender::RP::WARM, MenuPigRender::RP::WALL_NEAR, 0.30f);
    uint16_t base = Display::lerpColor565(
        MenuPigRender::RP::DEEP, MenuPigRender::RP::SHADOW_C, 0.25f);
    if (light.tint != 0) {
        body = MenuPigRender::screenBlend565(body, light.tint, 40);
        cap = MenuPigRender::screenBlend565(cap, light.tint, 64);
    }

    // The contact shadow belongs to the receiving surface, exactly as the
    // cat's does, and stays on the floor the tin is falling toward. Without it
    // a 4px object on a 2px lattice has nothing to sit on and reads as pasted.
    {
        const int centerX = x + w / 2;
        const int away = light.tint == 0 ? 0
            : (light.x < centerX ? kPx : -kPx);
        const int shadowW = trinketFalling ? w : w + 2 * kPx;
        const int shadowX = clampInt(
            (centerX - shadowW / 2 + away) & ~(kPx - 1), 0,
            SCREEN_WIDTH - shadowW);
        const uint16_t shadow = Display::lerpColor565(
            MenuPigRender::RP::SHADOW_C, MenuPigRender::RP::BG,
            trinketFalling ? 0.58f : 0.34f);
        canvas.fillRect(shadowX, trinketSupportY - kPx, shadowW, kPx, shadow);
    }

    static_assert(kTrinketH == kPx * 3 && kTrinketW == kPx * 2,
                  "trinket cells must tile its declared extents exactly");
    static_assert(kTrinketLyingW == kTrinketH && kTrinketLyingH == kTrinketW,
                  "a topple is a quarter turn: the extents must swap");
    if (trinketLying) {
        // On its side the lit end cap points along the travel and the whole
        // underside is the part in contact, so it carries the dark tone.
        const int capX = trinketCapRight ? x + w - kPx : x;
        canvas.fillRect(x, y, w, kPx, body);
        canvas.fillRect(capX, y, kPx, kPx, cap);
        canvas.fillRect(x, y + kPx, w, kPx, base);
        return;
    }

    // The rock pivots on the base rather than sliding the whole tin sideways:
    // a translated silhouette reads as a jump cut, a planted contact line
    // reads as something being nudged.
    const int lean = trinketWobble >= kTrinketLeanThreshold ? kPx
        : (trinketWobble <= -kTrinketLeanThreshold ? -kPx : 0);
    const int capX = clampInt(x + lean, 0, SCREEN_WIDTH - w);
    canvas.fillRect(capX, y, w, kPx, cap);
    canvas.fillRect(x, y + kPx, w, kPx, body);
    canvas.fillRect(x, y + kPx * 2, w, kPx, base);
}

bool consumeMeowVoice() {
    if (!voicePending) return false;
    voicePending = false;
    return true;
}

bool consumeMemory(Memory& memory) {
    if (!memoryPending) return false;
    memory = pendingMemory;
    pendingMemory = Memory::COUNT;
    memoryPending = false;
    return true;
}

Activity desiredActivity(Activity base, bool pigMoving, bool helperScene) {
    if (helperScene || !catVisible)
        return Activity::FOLLOW;
    if (visitPhase == VisitPhase::COMPANY) return companyActivity;
    if (visitPhase == VisitPhase::ARRIVING ||
        visitPhase == VisitPhase::APPROACHING_COMPANY ||
        visitPhase == VisitPhase::RETURNING_TO_INTEREST ||
        visitPhase == VisitPhase::DEPARTING)
        return Activity::FOLLOW;
    if (actionPending || actionActive) return plannedAction;
    (void)pigMoving;
    return base;
}

void updateNavigation(uint32_t now, const Pose& target,
                      Activity roomDefault, bool pigMoving,
                      bool helperScene) {
    advanceAnimationTransition(now);
    if (!navigationInitialized || navigationNeedsSceneInit) {
        const bool lateRoomEntry =
            roomSceneActive && arrivalPlan == ArrivalPlan::LATE_ENTRY;
        const bool concealedEntry =
            roomSceneActive && arrivalPlan == ArrivalPlan::CONCEALED;
        const float floorY = (float)originYForSupport(
            UIMeasurements::MenuPigLayout::kFloorY);
        if (concealedEntry) {
            // Centred in the occluder, so the whole of him is behind it no
            // matter which way he leaves it.
            navigationX = (float)clampInt(
                concealX + (concealWidth - kWidth) / 2,
                0, SCREEN_WIDTH - kWidth);
            navigationY = floorY;
            navigationPose.faceRight = concealEmergeRight;
        } else if (lateRoomEntry) {
            navigationX = arrivalFromLeft ? (float)-kWidth
                                          : (float)SCREEN_WIDTH;
            navigationY = floorY;
            navigationPose.faceRight = arrivalFromLeft;
        } else {
            navigationX = (float)target.x;
            navigationY = (float)target.y;
            navigationPose.faceRight = target.faceRight;
        }
        navigationInitialized = true;
        navigationNeedsSceneInit = false;
        navigationUpdatedAt = now;
        // Both staged entrances are seeded away from their destination, so
        // they are travelling from the frame they become visible. Saying
        // otherwise would let the room read them as settled and hand them a
        // surface to put something down on, halfway to where they are going.
        navigationMoving = lateRoomEntry || concealedEntry;
        arrivalComplete = !lateRoomEntry && !concealedEntry;
        syncNavigationPose();
    }

    if (!catVisible) {
        navigationUpdatedAt = now;
        setCurrentAnimation(Activity::FOLLOW, false, now);
        // An off-screen cat still advances its rig, but it must not leave a
        // stale open mouth behind that fires a meow on its next entrance.
        const Rig::Pose hidden = sampleRuntimeAnimation(now);
        lastVoicedMouth = hidden.mouth;
        updateFurMotion(hidden);
        // The object stays behind when he goes. Its physics has to run on this
        // path too, or a tin knocked on the way out hangs in the air and a
        // rock still in progress freezes mid-lean for the rest of the visit -
        // the object belonging to the cat again, this time in the clock.
        updateTrinket(now, hidden);
        return;
    }

    if (actionActive && plannedAction == Activity::ZOOMIES &&
        !deadlineReached(now, actionStartedAt)) {
        // Hold the loaded crouch at the action anchor until the explicit
        // idle-to-zoomies transition has finished.
        navigationMoving = false;
        navigationUpdatedAt = now;
    } else if (actionActive && plannedAction == Activity::ZOOMIES) {
        const uint32_t duration = actionDuration(Activity::ZOOMIES);
        float t = duration
            ? (float)(now - actionStartedAt) / (float)duration
            : 1.0f;
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
        const float eased = t * t * (3.0f - 2.0f * t);
        navigationX = zoomStartX + (zoomEndX - zoomStartX) * eased;
        navigationY = zoomY;
        navigationPose.faceRight = zoomEndX > zoomStartX;
        navigationMoving = t < 1.0f;
        navigationUpdatedAt = now;
        syncNavigationPose();
    } else {
        advanceSceneNavigation(now, target, pigMoving, helperScene);
        const bool reachedTarget =
            fabsf(navigationX - (float)target.x) <= kTargetEpsilonPx &&
            fabsf(navigationY - (float)target.y) <= kTargetEpsilonPx;
        if (concealActive) {
            // He owns his whole silhouette again the moment his footprint is
            // clear of the edge, and the room goes back to its ordinary render
            // order on that same frame. Standing still at the end of the walk
            // releases it too: a room whose furniture cannot satisfy the edge
            // test may cost the effect, never the cat.
            const int left = (int)lroundf(navigationX);
            const bool clearedEdge = concealEmergeRight
                ? left >= concealX + concealWidth
                : left + kWidth <= concealX;
            if (clearedEdge || (reachedTarget && !navigationMoving))
                concealActive = false;
        }
        if (!arrivalComplete && reachedTarget && !navigationMoving) {
            arrivalComplete = true;
            visitPhase = VisitPhase::INDEPENDENT;
        }
        if (reachedTarget && !navigationMoving &&
            visitPhase == VisitPhase::APPROACHING_COMPANY) {
            visitPhase = VisitPhase::COMPANY;
            companyPlanned = false;
            queueMemory(memoryForActivity(companyActivity));
            const uint32_t h = visitHash(
                sceneRoom, roomVisit, 0xC0A94E5u + actionSerial);
            companyUntil = now + kCompanyMinMs + (h % kCompanyJitterMs);
            nextSignalAt = now + kFirstSignalDelayMs;
        } else if (reachedTarget && !navigationMoving &&
                   visitPhase == VisitPhase::RETURNING_TO_INTEREST) {
            visitPhase = VisitPhase::INDEPENDENT;
            scheduleNextAction(now);
        } else if (reachedTarget && !navigationMoving &&
                   visitPhase == VisitPhase::DEPARTING) {
            catVisible = false;
            visitPhase = VisitPhase::ABSENT;
        }
        if (actionPending && reachedTarget && !navigationMoving &&
            visitPhase == VisitPhase::INDEPENDENT)
            beginPlannedAction(now);
    }

    // The surface corridor owns CROUCH_TO_POUNCE -> POUNCE_TO_LAND ->
    // LAND_TO_STAND. Selecting the ordinary FOLLOW walk here used to replace
    // that authored sequence on the same frame it began.
    if (!verticalTransitionActive) {
        setCurrentAnimation(
            chooseAnimation(roomDefault, pigMoving, helperScene),
            navigationMoving, now);
    }
    advanceSignalAnimation(now);
    const Rig::Pose sampled = sampleRuntimeAnimation(now);
    updateVoice(sampled);
    updateFurMotion(sampled);
    updateTrinket(now, sampled);
}

Pose currentPose() {
    return navigationPose;
}

bool isNavigating() {
    return navigationMoving;
}

Activity animationActivity() {
    return currentAnimation;
}

void formatMeowByte(uint8_t value, char* out, size_t outSize) {
    if (!out || outSize == 0) return;
    if (outSize < 32) {
        out[0] = '\0';
        return;
    }

    size_t pos = 0;
    for (int bit = 7; bit >= 0; --bit) {
        const char* token = (value & (1u << bit)) ? "MRR" : "MEW";
        memcpy(out + pos, token, 3);
        pos += 3;
        if (bit == 4) out[pos++] = '\n';
        else if (bit != 0) out[pos++] = ' ';
    }
    out[pos] = '\0';
}

void draw(M5Canvas& canvas, const Pose& pose, bool moving,
          Activity activity, uint32_t now, const PigLight& light) {
    if (!catVisible) return;
    // Activity and locomotion are update-owned. The arguments remain part of
    // the public draw seam so callers can pass their matching snapshot, but a
    // render may not swap clips independently between two update ticks.
    (void)moving;
    const Rig::Pose rigPose = sampleRuntimeAnimation(now);

    // A small support shadow is part of the receiver, not an ambient decal.
    // It shifts away from the same practical that tints the coat. A contact
    // shadow cannot travel through open air with the paws: loading keeps it on
    // the launch support, airborne frames suppress it, and touchdown restores
    // it on the receiving support.
    if (hasSurfaceContact() &&
        activity != Activity::HEAD_NAP && !restsOnPig()) {
        const int centerX = pose.x + kWidth / 2;
        const int away = light.tint == 0 ? 0 : (light.x < centerX ? 2 : -2);
        const bool airborne = rigPose.gait == Rig::Gait::BOUND_EXTEND ||
                              rigPose.gait == Rig::Gait::BOUND_TUCK;
        const int shadowW = airborne ? 14 : 22;
        const int supportY = pose.y + kGroundContactY;
        const int shadowX = (centerX - shadowW / 2 + away) & ~(kPx - 1);
        const uint16_t shadow = Display::lerpColor565(
            MenuPigRender::RP::SHADOW_C, MenuPigRender::RP::BG,
            airborne ? 0.58f : 0.34f);
        canvas.fillRect(shadowX, supportY - kPx, shadowW, kPx, shadow);
    }
    Rig::draw(canvas, rigOriginXForFootprint(pose.x),
              rigOriginYForFootprint(pose.y),
              pose.faceRight, rigPose, furMotion, light);
}

void drawSignalBubble(M5Canvas& canvas, const Pose& pose, uint32_t now) {
    if (!signalActive) return;

    char encoded[32];
    formatMeowByte(currentSignalByte(now), encoded, sizeof(encoded));
    if (!encoded[0]) return;

    canvas.setFont(&fonts::Font0);
    canvas.setTextSize(1);
    const Rig::Pose rigPose = sampleRuntimeAnimation(now);
    const Rig::Point nose = Rig::noseAnchor(
        rigOriginXForFootprint(pose.x),
        rigOriginYForFootprint(pose.y), pose.faceRight, rigPose);
    Mood::drawBubbleAt(canvas, encoded, pose.x, pose.x + kWidth,
                       pose.y, nose.x, nose.y);
}

#ifdef HAMLET_SIM
void drawRuntimeSheet(M5Canvas& canvas, uint8_t page) {
    // Four activities per page, then one trailing page for the three clips
    // that have no Activity slot. Growing the repertoire must push those onto
    // their own page rather than silently shadowing a real activity row.
    page %= 6u;
    const Rig::SecondaryMotion stillFur = {};
    for (uint8_t row = 0; row < 4u; ++row) {
        const bool signalRow = page == 5u && row == 0u;
        const bool idleRow = page == 5u && row == 1u;
        const bool walkRow = page == 5u && row == 2u;
        const uint8_t activityIndex = (uint8_t)(page * 4u + row);
        if (activityIndex >= (uint8_t)Activity::COUNT &&
            !signalRow && !idleRow && !walkRow)
            continue;
        const Activity activity = activityIndex < (uint8_t)Activity::COUNT
            ? (Activity)activityIndex
            : Activity::FOLLOW;
        const Animation::Clip& clip = signalRow
            ? Animation::signalClip()
            : Animation::clipFor(activity, walkRow || !idleRow);
        const uint32_t duration = Animation::clipDuration(clip);
        for (uint8_t col = 0; col < 4u; ++col) {
            const uint32_t sampleAt = duration
                ? (duration * col) / 4u
                : 0u;
            const Rig::Pose key = Animation::sample(clip, sampleAt);
            static constexpr int16_t kReviewKeyDx[4] = {38, 0, -38, 0};
            PigLight light;
            light.x = (int16_t)(col * kWidth + kWidth / 2 +
                                kReviewKeyDx[col]);
            light.y = (int16_t)(row * kHeight - 8);
            // Each diagnostic row samples key-right, centered, key-left, and
            // unlit material response without changing the animation sample.
            light.tint = col == 3u ? 0 : MenuPigRender::RP::NEON;
            Rig::draw(canvas, rigOriginXForFootprint(col * kWidth),
                      rigOriginYForFootprint(row * kHeight),
                      true, key, stillFur, light);
        }
    }
}

void forceSignalForCapture(uint32_t now, uint8_t messageIndex) {
    signalActive = true;
    signalPhase = SignalPhase::TRANSMITTING;
    signalAbortRequested = false;
    signalStartedAt = now;
    signalMessage = (uint8_t)(messageIndex % kCipherMessageCount);
}

void forceStateForCapture(uint32_t now, const Pose& pose,
                          Activity activity, bool moving) {
    catVisible = true;
    // A capture asks for exactly this pose, whole and where it says. Neither a
    // beam nor a piece of furniture left over from the previous frame gets to
    // take a bite out of it.
    portalTransit = false;
    concealActive = false;
    arrivalComplete = true;
    visitPhase = VisitPhase::INDEPENDENT;
    companyPlanned = false;
    actionPending = false;
    actionActive = false;
    plannedAction = Activity::FOLLOW;
    plannedActionNight = false;
    navigationInitialized = true;
    navigationNeedsSceneInit = false;
    navigationMoving = moving;
    navigationX = (float)pose.x;
    navigationY = (float)pose.y;
    navigationPose = pose;
    navigationUpdatedAt = now;
    verticalTransitionActive = false;
    verticalTransitionPhase = SurfaceTransitionPhase::NONE;
    verticalTouchesPig = false;
    verticalPigIsDestination = false;
    currentAnimation = activity;
    currentAnimationMoving = activity == Activity::FOLLOW && moving;
    animationStartedAt = now;
    activeAnimationTransition = Animation::Transition::NONE;
    queuedAnimationTransition = Animation::Transition::NONE;
    animationTransitionStartedAt = 0;
    animationHandoffFrom = Rig::Pose();
    animationHandoffStartedAt = 0;
    animationHandoffDurationMs = kPoseHandoffMs;
    animationHandoffActive = false;
    furSweepQ8 = 0;
    furVelocityQ8 = 0;
    furLastX = pose.x;
    furLastY = pose.y;
    furInitialized = true;
    furMotion = {};
    lastVoicedMouth = Rig::MouthPose::CLOSED;
    voicePending = false;
    pendingMemory = Memory::COUNT;
    memoryPending = false;
}

bool runStateMachineSelfTest() {
    bool ok = true;
    uint32_t now = 1000u;
    const auto sameRigPose = [](const Rig::Pose& a, const Rig::Pose& b) {
        return a.stance == b.stance && a.gait == b.gait &&
               a.head == b.head && a.tail == b.tail &&
               a.ears == b.ears && a.eyes == b.eyes && a.fur == b.fur &&
               a.bodyDx == b.bodyDx && a.bodyDy == b.bodyDy &&
               a.headDx == b.headDx && a.headDy == b.headDy &&
               a.breath == b.breath && a.mouth == b.mouth;
    };

    // Every activity must own a non-empty, envelope-safe code-rig clip.  The
    // animation module also verifies long observation holds and signal keys.
    const bool clipsValid = Animation::validateClips();
    if (!clipsValid) ok = false;

    // The complete social repertoire must remain reachable from its authored
    // context. Hash ranges are deterministic, so this proves that a future
    // weighting edit cannot silently strand a memory forever.
    bool sawFaceBump = false;
    bool sawHeadNap = false;
    bool sawKnead = false;
    bool sawSlowBlink = false;
    bool sawMeow = false;
    // A sheltered station is restful but closed on top. It must still reach
    // both of its own beats and must never reach the one that needs a crown.
    bool sawShelteredKnead = false;
    bool sawShelteredBlink = false;
    bool shelteredCrown = false;
    for (uint32_t h = 0; h < 8u; ++h) {
        sawMeow |= companyActivityFor(CompanyContext::ORDINARY, h) ==
                   Activity::MEOW;
        sawFaceBump |= companyActivityFor(CompanyContext::ORDINARY, h) ==
                       Activity::FACE_BUMP;
        sawHeadNap |= companyActivityFor(CompanyContext::RESTING, h) ==
                      Activity::HEAD_NAP;
        sawKnead |= companyActivityFor(CompanyContext::RESTING, h) ==
                    Activity::KNEAD;
        sawSlowBlink |= companyActivityFor(CompanyContext::BATHING, h) ==
                        Activity::SLOW_BLINK;
        const Activity sheltered =
            companyActivityFor(CompanyContext::SHELTERED, h);
        sawShelteredKnead |= sheltered == Activity::KNEAD;
        sawShelteredBlink |= sheltered == Activity::SLOW_BLINK;
        shelteredCrown |= sheltered == Activity::HEAD_NAP;
    }
    printf("[CAT SHELTERED] knead=%d blink=%d crown=%d\n",
           sawShelteredKnead ? 1 : 0, sawShelteredBlink ? 1 : 0,
           shelteredCrown ? 1 : 0);
    if (!sawShelteredKnead || !sawShelteredBlink || shelteredCrown)
        ok = false;
    // A room roll must also be able to reach the ambient meow, otherwise the
    // clip is only ever seen by players who happen to earn a company visit.
    bool sawAmbientMeow = false;
    for (uint8_t serial = 0; serial < 32u && !sawAmbientMeow; ++serial) {
        for (uint8_t room = 0; room < 6u && !sawAmbientMeow; ++room)
            sawAmbientMeow = actionForRoom(room, serial, false) ==
                             Activity::MEOW;
    }
    if (!sawFaceBump || !sawHeadNap || !sawKnead || !sawSlowBlink ||
        !sawMeow || !sawAmbientMeow ||
        memoryCount(kAllMemoryBits) != (uint8_t)Memory::COUNT)
        ok = false;

    // Every public promise this module makes about where the cat is - the
    // concealment clip span, the playfield clamp, the gap the room keeps from
    // Pancetta, the paw's reach to a batted object - is measured against the
    // declared footprint. The rig composes in a larger action canvas, so a
    // single authored pose that paints outside that footprint invalidates all
    // of them at once, and does it silently. Sample every clip the runtime can
    // reach, both facings, at the extremes of the coat spring.
    {
        static constexpr int8_t kFurSweeps[] = {-2, 0, 2};
        static constexpr int8_t kFurLifts[] = {-1, 0, 1};
        int overLeft = 0;
        int overRight = 0;
        int overTop = 0;
        int overBottom = 0;
        bool boundsValid = true;
        const auto measureClip = [&](const Animation::Clip& clip) {
            const uint32_t duration = Animation::clipDuration(clip);
            for (uint32_t at = 0; at < duration; at += 32u) {
                const Rig::Pose key = Animation::sample(clip, at);
                for (uint8_t face = 0; face < 2u; ++face) {
                    for (int8_t sweep : kFurSweeps) {
                        for (int8_t lift : kFurLifts) {
                            Rig::SecondaryMotion fur;
                            fur.sweepCells = sweep;
                            fur.liftCells = lift;
                            const Rig::Bounds b = Rig::measurePoseBounds(
                                key, face == 0, fur);
                            if (!b.valid) {
                                boundsValid = false;
                                continue;
                            }
                            overLeft = max(overLeft,
                                (int)(kFootprintInsetX - b.left));
                            overRight = max(overRight,
                                (int)(b.right - (kFootprintInsetX + kWidth)));
                            overTop = max(overTop,
                                (int)(kFootprintInsetY - b.top));
                            overBottom = max(overBottom,
                                (int)(b.bottom -
                                      (kFootprintInsetY + kHeight)));
                        }
                    }
                }
            }
        };
        for (uint8_t activity = 0; activity < (uint8_t)Activity::COUNT;
             ++activity) {
            measureClip(Animation::clipFor((Activity)activity, true));
            measureClip(Animation::clipFor((Activity)activity, false));
        }
        for (uint8_t transition = (uint8_t)Animation::Transition::NONE + 1u;
             transition < (uint8_t)Animation::Transition::COUNT;
             ++transition) {
            const Animation::Clip* clip =
                Animation::clipFor((Animation::Transition)transition);
            if (clip) measureClip(*clip);
        }
        measureClip(Animation::signalClip());
        printf("[CAT FOOTPRINT] valid=%d over L=%d R=%d T=%d B=%d\n",
               boundsValid ? 1 : 0, overLeft, overRight, overTop, overBottom);
        if (!boundsValid || overLeft > 0 || overRight > 0 ||
            overTop > 0 || overBottom > 0)
            ok = false;
    }

    // The miniature keeps a wider-than-tall cat body with four fat stubbies,
    // and its measured paw contact ends exactly on the physical support anchor.
    const Rig::Pose neutralRig;
    const Rig::SecondaryMotion stillFur;
    const Rig::Bounds neutralBounds =
        Rig::measurePoseBounds(neutralRig, true, stillFur);
    const int neutralW = neutralBounds.right - neutralBounds.left;
    const int neutralH = neutralBounds.bottom - neutralBounds.top;
    const Rig::Pose movingNeutral(
        Rig::Stance::STAND, Rig::Gait::STRIDE_PLANT);
    const Rig::Bounds movingBounds =
        Rig::measurePoseBounds(movingNeutral, true, stillFur);
    const int movingH = movingBounds.bottom - movingBounds.top;
    const Rig::Pose crouchedWalk(
        Rig::Stance::CROUCH, Rig::Gait::STRIDE_PLANT,
        Rig::HeadPose::LEVEL, Rig::TailPose::TUCK);
    const Rig::Pose airborneWalk(
        Rig::Stance::STAND, Rig::Gait::BOUND_TUCK,
        Rig::HeadPose::LEVEL, Rig::TailPose::TUCK);
    const Rig::Bounds crouchedBounds =
        Rig::measurePoseBounds(crouchedWalk, true, stillFur);
    const Rig::Bounds airborneBounds =
        Rig::measurePoseBounds(airborneWalk, true, stillFur);
    const bool neutralOnOutputGrid =
        (neutralBounds.left - kFootprintInsetX) %
                Rig::kOutputCellPixels == 0 &&
        (neutralBounds.top - kFootprintInsetY) %
                Rig::kOutputCellPixels == 0 &&
        (neutralBounds.right - kFootprintInsetX) %
                Rig::kOutputCellPixels == 0 &&
        (neutralBounds.bottom - kFootprintInsetY) %
                Rig::kOutputCellPixels == 0;
    // The 4px resolve can quantize both poses into the same bottom cell; the
    // airborne read is carried by its visibly higher top edge.
    const bool airborneReadsHigher =
        crouchedBounds.valid && airborneBounds.valid &&
        crouchedBounds.top > airborneBounds.top;
    printf("[CAT GEOMETRY] clips=%s neutral=%d,%d..%d,%d ratio=%d:%d\n",
           clipsValid ? "PASS" : "FAIL",
           (int)neutralBounds.left, (int)neutralBounds.top,
           (int)neutralBounds.right, (int)neutralBounds.bottom,
           neutralW, neutralH);
    if (!neutralBounds.valid ||
        neutralBounds.left < kFootprintInsetX ||
        neutralBounds.right > kFootprintInsetX + kWidth ||
        neutralBounds.top < kFootprintInsetY ||
        !neutralOnOutputGrid ||
        neutralBounds.bottom - kFootprintInsetY != kGroundContactY ||
        !movingBounds.valid || movingBounds.left != neutralBounds.left ||
        movingBounds.top != neutralBounds.top ||
        movingBounds.right != neutralBounds.right ||
        movingBounds.bottom != neutralBounds.bottom || movingH != neutralH ||
        !airborneReadsHigher ||
        neutralW * 5 < neutralH * 6 ||
        originYForSupport(218) + neutralBounds.bottom -
                kFootprintInsetY != 218)
        ok = false;

    const Pose start = {40, (int16_t)originYForSupport(218), true};
    forceStateForCapture(now, start, Activity::FOLLOW, false);

    // Reversing Pancetta's desired flank changes only the target. One 16 ms
    // update may advance one 2px cell; it may never swap sides instantly.
    const Pose farTarget = {220, 184, true};
    Pose before = currentPose();
    updateNavigation(now + 16u, farTarget, Activity::FOLLOW, true, false);
    Pose after = currentPose();
    if (abs(after.x - before.x) > kPx * 2 ||
        abs(after.y - before.y) > kPx * 2)
        ok = false;

    // FOLLOW owns two clips but one public Activity value. Starting and
    // stopping navigation must retain the exact pose sampled at each switch;
    // render-time selection used to replace idle with walk in one frame.
    now += 32u;
    forceStateForCapture(now, start, Activity::FOLLOW, false);
    const Rig::Pose beforeWalk = sampleRuntimeAnimation(now + 16u);
    updateNavigation(now + 16u, farTarget, Activity::FOLLOW, true, false);
    const Rig::Pose afterWalk = sampleRuntimeAnimation(now + 16u);
    if (!currentAnimationMoving ||
        !sameRigPose(beforeWalk, afterWalk))
        ok = false;
    now += 32u;
    const Pose stopTarget = currentPose();
    const Rig::Pose beforeStop = sampleRuntimeAnimation(now);
    updateNavigation(now, stopTarget, Activity::FOLLOW, false, false);
    const Rig::Pose afterStop = sampleRuntimeAnimation(now);
    if (currentAnimationMoving ||
        !sameRigPose(beforeStop, afterStop))
        ok = false;

    // Elevated supports use a side approach and the complete authored jump
    // corridor; they are not reached by a diagonal float, a straight vertical
    // rise, or an anchor snap. Arrival remains busy through landing recovery.
    now += 32u;
    forceStateForCapture(now, start, Activity::FOLLOW, false);
    const Pose elevatedTarget = {100, 140, true};
    Pose previous = currentPose();
    uint16_t verticalSteps = 0;
    bool sawLoad = false;
    bool sawFlight = false;
    bool sawLanding = false;
    bool sawHorizontalFlight = false;
    int16_t flightStartX = 0;
    for (; verticalSteps < 200 &&
           (isNavigating() ||
            currentPose().x != elevatedTarget.x ||
            currentPose().y != elevatedTarget.y);
         ++verticalSteps) {
        now += 16u;
        updateNavigation(now, elevatedTarget, Activity::WATCH_RAIN,
                         false, false);
        Pose sampled = currentPose();
        if (abs(sampled.x - previous.x) > kPx * 2 ||
            abs(sampled.y - previous.y) > kPx * 2)
            ok = false;
        if (verticalTransitionActive) {
            if (currentAnimationMoving) ok = false;
            if (verticalTransitionPhase == SurfaceTransitionPhase::LOADING) {
                sawLoad = true;
                if (!hasSurfaceContact() ||
                    activeAnimationTransition !=
                        Animation::Transition::CROUCH_TO_POUNCE)
                    ok = false;
            } else if (verticalTransitionPhase ==
                       SurfaceTransitionPhase::AIRBORNE) {
                if (!sawFlight) flightStartX = sampled.x;
                sawFlight = true;
                sawHorizontalFlight |= sampled.x != flightStartX;
                if (hasSurfaceContact() ||
                    activeAnimationTransition !=
                        Animation::Transition::POUNCE_TO_LAND)
                    ok = false;
            } else if (verticalTransitionPhase ==
                       SurfaceTransitionPhase::LANDING) {
                sawLanding = true;
                if (!hasSurfaceContact() ||
                    sampled.x != elevatedTarget.x ||
                    sampled.y != elevatedTarget.y ||
                    activeAnimationTransition !=
                        Animation::Transition::LAND_TO_STAND)
                    ok = false;
            }
        }
        previous = sampled;
    }
    if (currentPose().x != elevatedTarget.x ||
        currentPose().y != elevatedTarget.y ||
        !sawLoad || !sawFlight || !sawHorizontalFlight || !sawLanding ||
        verticalTransitionActive)
        ok = false;

    // Room 5 keeps the cat on an elevated dry rim. Night zoomies must descend
    // to the floor target before the crossing clip starts; beginning at the
    // rim would make the actor sprint through open air at support height.
    now += 32u;
    const Pose elevatedZoomStart = {
        120, (int16_t)originYForSupport(150), true,
    };
    forceStateForCapture(now, elevatedZoomStart,
                         Activity::WATCH_WATER, false);
    plannedAction = Activity::ZOOMIES;
    actionPending = true;
    actionActive = false;
    const Pose floorZoomTarget = {
        120, (int16_t)originYForSupport(218), true,
    };
    for (uint16_t step = 0; step < 140 && !actionActive; ++step) {
        now += 16u;
        updateNavigation(now, floorZoomTarget, Activity::WATCH_WATER,
                         false, false);
        if (actionActive && currentPose().y != floorZoomTarget.y)
            ok = false;
    }
    if (!actionActive || currentPose().y != floorZoomTarget.y)
        ok = false;

    // A distant action anchor with a small grid-level Y correction must stay
    // ordinary FOLLOW travel, then enter the tagged action only after physical
    // arrival. It is not a new surface and may not arm the pounce corridor.
    now += 32u;
    forceStateForCapture(now, start, Activity::FOLLOW, false);
    plannedAction = Activity::SLEEP;
    actionPending = true;
    actionActive = false;
    const Pose sleepTarget = {200, 184, true};
    bool shallowCorrectionJumped = false;
    for (uint16_t step = 0; step < 220 && !actionActive; ++step) {
        now += 16u;
        updateNavigation(now, sleepTarget, Activity::WATCH_RAIN,
                         false, false);
        shallowCorrectionJumped |= verticalTransitionActive;
        if (!actionActive && animationActivity() != Activity::FOLLOW)
            ok = false;
    }
    if (!actionActive || animationActivity() != Activity::SLEEP ||
        currentPose().x != sleepTarget.x ||
        currentPose().y != sleepTarget.y || shallowCorrectionJumped)
        ok = false;

    // Visit planning must retain every feline outcome the rooms rely on:
    // sometimes absent, sometimes already present, sometimes a later entrance.
    // A room with nothing to hide behind may never plan a concealed one.
    // Bounds verified against the real hash by scripts/sim_cat_arrival.py.
    uint8_t skipped = 0;
    uint8_t waiting = 0;
    uint8_t late = 0;
    uint8_t concealedPlans = 0;
    uint8_t companyVisits = 0;
    uint8_t stayingVisits = 0;
    reset(now);
    setRoomStaging(RoomStaging{});
    for (uint8_t visit = 0; visit < 60u; ++visit) {
        now += 1000u;
        update(now, true, true, visit % 6u, true,
               CompanyContext::ORDINARY);
        if (arrivalPlan == ArrivalPlan::SKIP_VISIT) ++skipped;
        else if (arrivalPlan == ArrivalPlan::ALREADY_THERE) ++waiting;
        else if (arrivalPlan == ArrivalPlan::CONCEALED) ++concealedPlans;
        else ++late;
        if (companyPlanned) ++companyVisits;
        if (stayForVisit) ++stayingVisits;
    }
    if (!skipped || !waiting || !late || concealedPlans ||
        companyVisits < 8u || companyVisits > 36u ||
        stayingVisits < 20u || stayingVisits > 45u)
        ok = false;

    // Offer somewhere to hide and the plan has to start using it, and warmth
    // has to move the weights in the direction it claims: a session going well
    // is met in the open, a cold one is watched from behind the furniture.
    RoomStaging staged;
    staged.hideX = 80;
    staged.hideWidth = 96;
    staged.hideEmergeRight = true;
    staged.hideValid = true;
    uint8_t coldConcealed = 0;
    uint8_t warmConcealed = 0;
    for (uint8_t pass = 0; pass < 2u; ++pass) {
        staged.warmth = pass == 0 ? 0u : 100u;
        reset(now);
        setRoomStaging(staged);
        for (uint8_t visit = 0; visit < 60u; ++visit) {
            now += 1000u;
            update(now, true, true, visit % 6u, true,
                   CompanyContext::ORDINARY);
            if (arrivalPlan != ArrivalPlan::CONCEALED) continue;
            if (pass == 0) ++coldConcealed;
            else ++warmConcealed;
        }
    }
    if (!coldConcealed || coldConcealed <= warmConcealed) ok = false;

    // A concealed arrival owes the room three things: nothing on screen before
    // the reveal, a clipped span while he is crossing the edge, and no clip at
    // all once his footprint is past it.
    now += 32u;
    reset(now);
    staged.warmth = 0;
    setRoomStaging(staged);
    update(now, true, true, 0u, true, CompanyContext::ORDINARY);
    arrivalPlan = ArrivalPlan::CONCEALED;
    catVisible = false;
    arrivalComplete = false;
    visitPhase = VisitPhase::ABSENT;
    concealActive = true;
    concealX = staged.hideX;
    concealWidth = staged.hideWidth;
    concealEmergeRight = true;
    navigationNeedsSceneInit = true;
    const int16_t concealEdge = (int16_t)(staged.hideX + staged.hideWidth);
    int16_t clipX = 0;
    int16_t clipW = 0;
    Pose emergeTarget;
    if (!concealClip(clipX, clipW) || clipX != concealEdge ||
        clipW != (int16_t)(SCREEN_WIDTH - concealEdge))
        ok = false;
    // Nothing is emerging while he is still hidden, so the room has no
    // emergence destination to honour yet.
    if (concealExitPose(emergeTarget)) ok = false;
    // Seed navigation the way a room change does: the scene init runs while he
    // is still hidden and the invisible path returns before anything can move
    // him, so whatever it wrote is what the room reads at the reveal.
    updateNavigation(now, Pose{}, Activity::WATCH_CABLES, false, false);
    if (currentPose().x < concealX ||
        currentPose().x + kWidth > concealX + concealWidth)
        ok = false;
    catVisible = true;
    visitPhase = VisitPhase::ARRIVING;
    // Read before the next navigation tick, exactly where the room reads it.
    // He is behind furniture with his destination past it, so he owes that
    // walk; claiming settled here offers him a surface at the wrong spot and
    // strands a batted object out of his own reach for the whole visit.
    if (!isNavigating()) ok = false;
    if (!concealExitPose(emergeTarget) || emergeTarget.x < concealEdge ||
        emergeTarget.x + kWidth > SCREEN_WIDTH)
        ok = false;
    for (uint16_t step = 0; step < 400u && concealActive; ++step) {
        now += 16u;
        updateNavigation(now, emergeTarget, Activity::WATCH_CABLES,
                         false, false);
    }
    if (concealActive || currentPose().x < concealEdge ||
        concealClip(clipX, clipW))
        ok = false;

    // A visit the cat walks out of is over. The reveal that staged his
    // entrance may not re-arm behind him: it fired again the moment he was
    // off screen, walked him straight back in, and left him parked in
    // ARRIVING for the rest of the visit - a phase that never completes once
    // the arrival it belongs to already has, and where no action, company, or
    // transmission can ever be scheduled again.
    now += 32u;
    reset(now);
    setRoomStaging(RoomStaging{});
    update(now, false, true, 0u, true, CompanyContext::ORDINARY);
    arrivalPlan = ArrivalPlan::LATE_ENTRY;
    catVisible = false;
    arrivalComplete = false;
    visitPhase = VisitPhase::ABSENT;
    stayForVisit = false;
    companyPlanned = false;
    navigationNeedsSceneInit = true;
    revealAt = now;
    // Nothing else may claim the visit while the exit is under test.
    nextActionAt = now + 600000u;
    const Pose departAnchor = {
        120,
        (int16_t)originYForSupport(UIMeasurements::MenuPigLayout::kFloorY),
        true,
    };
    for (uint16_t step = 0; step < 600u && !arrivalComplete; ++step) {
        now += 16u;
        update(now, false, true, 0u, true, CompanyContext::ORDINARY);
        updateNavigation(now, departAnchor, Activity::WATCH_CABLES,
                         false, false);
    }
    const bool departArrived = arrivalComplete && isVisible() &&
                               visitPhase == VisitPhase::INDEPENDENT;
    departAt = now;
    const Pose departExit = {
        (int16_t)SCREEN_WIDTH, departAnchor.y, true,
    };
    for (uint16_t step = 0; step < 600u && isVisible(); ++step) {
        now += 16u;
        update(now, false, true, 0u, true, CompanyContext::ORDINARY);
        updateNavigation(now,
                         sceneIntent() == SceneIntent::EXIT
                             ? departExit : departAnchor,
                         Activity::WATCH_CABLES, false, false);
    }
    const bool departed = !isVisible() &&
                          visitPhase == VisitPhase::ABSENT;
    bool cameBack = false;
    for (uint16_t step = 0; step < 400u && !cameBack; ++step) {
        now += 16u;
        update(now, false, true, 0u, true, CompanyContext::ORDINARY);
        updateNavigation(now, departAnchor, Activity::WATCH_CABLES,
                         false, false);
        cameBack = isVisible();
    }
    printf("[CAT DEPART] arrived=%d departed=%d back=%d phase=%u\n",
           departArrived ? 1 : 0, departed ? 1 : 0, cameBack ? 1 : 0,
           (unsigned)visitPhase);
    if (!departArrived || !departed || cameBack) ok = false;

    // Climbing onto Pancetta's crown is part of the nap, not part of the room.
    // Load still belongs to the launch support, flight has no contact shadow,
    // and landing belongs to the receiving support. The same ownership runs in
    // reverse on the way down. Once a jump starts, changing the visit intent
    // may not swap those physical endpoints underneath it.
    now += 32u;
    forceStateForCapture(now, start, Activity::FOLLOW, false);
    companyActivity = Activity::HEAD_NAP;
    visitPhase = VisitPhase::APPROACHING_COMPANY;
    const bool walkUpAbove = drawsAbovePig();
    const bool walkUpShadow = !restsOnPig();
    verticalTransitionActive = true;
    verticalTransitionPhase = SurfaceTransitionPhase::LOADING;
    verticalTouchesPig = true;
    verticalPigIsDestination = true;
    const bool climbAbove = drawsAbovePig();
    const bool climbLoadShadow = hasSurfaceContact() && !restsOnPig();
    // Pancetta starts walking while the cat is committed to the ascent. The
    // visit now wants to return, but the jump still physically lands on him.
    visitPhase = VisitPhase::RETURNING_TO_INTEREST;
    verticalTransitionPhase = SurfaceTransitionPhase::AIRBORNE;
    const bool climbAirShadow = hasSurfaceContact() && !restsOnPig();
    verticalTransitionPhase = SurfaceTransitionPhase::LANDING;
    const bool climbLandShadow = hasSurfaceContact() && !restsOnPig();
    verticalTransitionActive = false;
    verticalTransitionPhase = SurfaceTransitionPhase::NONE;
    verticalTouchesPig = false;
    verticalPigIsDestination = false;
    visitPhase = VisitPhase::COMPANY;
    // A transmission borrows the seated corridor without moving him off the
    // crown, so the nap clip is not what proves he is up there.
    const bool nappingAbove = drawsAbovePig();
    const bool nappingShadow = !restsOnPig();
    visitPhase = VisitPhase::RETURNING_TO_INTEREST;
    verticalTransitionActive = true;
    verticalTransitionPhase = SurfaceTransitionPhase::LOADING;
    verticalTouchesPig = true;
    verticalPigIsDestination = false;
    const bool dropLoadShadow = hasSurfaceContact() && !restsOnPig();
    verticalTransitionPhase = SurfaceTransitionPhase::AIRBORNE;
    const bool dropAirShadow = hasSurfaceContact() && !restsOnPig();
    verticalTransitionPhase = SurfaceTransitionPhase::LANDING;
    const bool dropLandShadow = hasSurfaceContact() && !restsOnPig();
    verticalTransitionActive = false;
    verticalTransitionPhase = SurfaceTransitionPhase::NONE;
    verticalTouchesPig = false;
    verticalPigIsDestination = false;
    const bool walkAwayAbove = drawsAbovePig();
    const bool walkAwayShadow = !restsOnPig();
    printf("[CAT CROWN] walk=%d/%d climb=%d/%d/%d/%d nap=%d/%d "
           "drop=%d/%d/%d walkAway=%d/%d\n",
           walkUpAbove ? 1 : 0, walkUpShadow ? 1 : 0,
           climbAbove ? 1 : 0, climbLoadShadow ? 1 : 0,
           climbAirShadow ? 1 : 0, climbLandShadow ? 1 : 0,
           nappingAbove ? 1 : 0, nappingShadow ? 1 : 0,
           dropLoadShadow ? 1 : 0, dropAirShadow ? 1 : 0,
           dropLandShadow ? 1 : 0,
           walkAwayAbove ? 1 : 0, walkAwayShadow ? 1 : 0);
    if (!walkUpAbove || !walkUpShadow ||
        !climbAbove || !climbLoadShadow || climbAirShadow ||
        climbLandShadow ||
        !nappingAbove || nappingShadow ||
        dropLoadShadow || dropAirShadow || !dropLandShadow ||
        !walkAwayAbove || !walkAwayShadow)
        ok = false;

    // The beam owns the body. He leaves the room he was in, stays off screen
    // across the room cut, and the room he lands in may not re-roll him away.
    now += 32u;
    reset(now);
    update(now, true, true, 0u, true, CompanyContext::ORDINARY);
    forceStateForCapture(now, start, Activity::FOLLOW, false);
    for (uint16_t step = 0; step < 80u && !verticalTransitionActive;
         ++step) {
        now += 16u;
        updateNavigation(now, elevatedTarget, Activity::WATCH_RAIN,
                         false, false);
    }
    const bool interruptedJump = verticalTransitionActive;
    beginPortalTransit(now);
    if (!interruptedJump || isVisible() || !inPortalTransit() ||
        verticalTransitionActive ||
        verticalTransitionPhase != SurfaceTransitionPhase::NONE ||
        verticalTouchesPig || verticalPigIsDestination ||
        activeAnimationTransition != Animation::Transition::NONE)
        ok = false;
    now += 800u;
    update(now, true, true, 3u, true, CompanyContext::ORDINARY);
    if (isVisible() || !inPortalTransit()) ok = false;
    now += 750u;
    const Pose landing = {
        180, (int16_t)originYForSupport(
                 UIMeasurements::MenuPigLayout::kFloorY), false,
    };
    endPortalTransit(now, landing);
    if (!isVisible() || inPortalTransit() ||
        currentPose().x != landing.x || currentPose().y != landing.y ||
        currentPose().faceRight != landing.faceRight ||
        sceneIntent() != SceneIntent::ROOM_INTEREST)
        ok = false;
    // The portal put him beside Pancetta, not on the room's anchor, so he owes
    // that walk and must say so until a navigation tick proves otherwise.
    if (!isNavigating()) ok = false;
    const Pose farAnchor = {40, landing.y, false};
    for (uint16_t step = 0; step < 400u && isNavigating(); ++step) {
        now += 16u;
        updateNavigation(now, farAnchor, Activity::WATCH_CABLES, false, false);
    }
    if (isNavigating() || currentPose().x != farAnchor.x) ok = false;

    visitPhase = VisitPhase::INDEPENDENT;
    if (sceneIntent() != SceneIntent::ROOM_INTEREST) ok = false;
    visitPhase = VisitPhase::APPROACHING_COMPANY;
    if (sceneIntent() != SceneIntent::COMPANY) ok = false;
    visitPhase = VisitPhase::DEPARTING;
    if (sceneIntent() != SceneIntent::EXIT) ok = false;

    // Transmission owns a complete seated corridor. Scheduling a byte may not
    // expose the signal pose immediately; both named boundary clips must be
    // observed before the cat returns to its company activity.
    now += 32u;
    forceStateForCapture(now, start, Activity::SLEEP, false);
    roomSceneActive = true;
    sceneRoom = 0u;
    visitPhase = VisitPhase::COMPANY;
    companyActivity = Activity::SLEEP;
    nextSignalAt = now;
    update(now, true, true, sceneRoom, true, CompanyContext::ORDINARY);
    if (isTransmitting() || signalPhase != SignalPhase::SEEKING_SIT)
        ok = false;
    bool sawSignalEntry = false;
    bool sawTransmission = false;
    bool sawSignalExit = false;
    for (uint16_t step = 0; step < 900u; ++step) {
        now += 16u;
        update(now, true, true, sceneRoom, true,
               CompanyContext::ORDINARY);
        updateNavigation(now, start, Activity::WATCH_RAIN, false, false);
        if (activeAnimationTransition ==
            Animation::Transition::SIT_TO_SIGNAL)
            sawSignalEntry = true;
        if (isTransmitting()) sawTransmission = true;
        if (activeAnimationTransition ==
            Animation::Transition::SIGNAL_TO_IDLE)
            sawSignalExit = true;
        if (sawSignalExit && signalPhase == SignalPhase::REST) break;
    }
    if (!sawSignalEntry || !sawTransmission || !sawSignalExit ||
        signalPhase != SignalPhase::REST || isTransmitting())
        ok = false;
    printf("[CAT SIGNAL] entry=%d tx=%d exit=%d rest=%d\n",
           sawSignalEntry ? 1 : 0, sawTransmission ? 1 : 0,
           sawSignalExit ? 1 : 0,
           signalPhase == SignalPhase::REST ? 1 : 0);

    // Losing the dialogue channel can interrupt any phoneme beat. The exit
    // handoff must begin from that sampled beat, not signalClip()[0].
    now += 32u;
    forceStateForCapture(now, start, Activity::WATCH_RAIN, false);
    forceSignalForCapture(now, 0u);
    now += 800u;
    const Rig::Pose beforeSignalCancel = sampleRuntimeAnimation(now);
    beginSignalExit(now);
    const Rig::Pose afterSignalCancel = sampleRuntimeAnimation(now);
    if (!sameRigPose(beforeSignalCancel, afterSignalCancel)) ok = false;

    // The batted trinket must reproduce the trajectory validated by
    // scripts/sim_cat_trinket.py: a knock from an elevated support lands on
    // the floor, goes onto its side, bounces a bounded number of times,
    // travels horizontally, and comes to rest inside the playfield. Both
    // facings are replayed because C truncation and the topple offset make the
    // two directions genuinely different arithmetic, not mirror images.
    const Rig::Pose restPaw(Rig::Stance::SIT, Rig::Gait::PLANT);
    const Rig::Pose liftPaw(Rig::Stance::SIT, Rig::Gait::PAW_LIFT);
    const int16_t shelfY = 150;
    // MenuPig sits the object one gap past the cat's footprint on the support
    // he is standing on. Reproducing that placement rather than dropping the
    // cat at the origin is what puts the reach test inside the replay instead
    // of making it something the replay has to be excused from.
    const int trinketGap = kPx * 3;
    auto stationCat = [&](int trinketX, int16_t supportY, bool faceRight) {
        navigationPose.x = (int16_t)(faceRight
            ? trinketX - kWidth - trinketGap
            : trinketX + trinketGap + kTrinketW);
        navigationPose.y = (int16_t)originYForSupport(supportY);
        navigationPose.faceRight = faceRight;
    };
    for (int pass = 0; pass < 2; ++pass) {
        const bool faceRight = pass == 0;
        now += 32u;
        reset(now);
        planTrinketForVisit(0u);
        trinketPresent = true;
        trinketKnockable = true;
        trinketBatsToKnock = 1u;
        stationCat(160, shelfY, faceRight);
        setTrinketAnchor(160, shelfY, true);

        updateTrinket(now, restPaw);
        const int32_t shelfXQ4 = trinketXQ4;
        now += 16u;
        updateTrinket(now, liftPaw);
        if (!trinketKnocked || !trinketFalling) ok = false;
        // The impact sound belongs to the impact. A tin knocked off a shelf is
        // still in the air on the frame the paw leaves it.
        if (consumeTrinketLanded()) ok = false;

        uint16_t trinketTicks = 0;
        uint16_t landTick = 0;
        uint8_t landPulses = 0;
        for (; trinketTicks < 200u &&
               (trinketFalling || trinketRollQ4 != 0); ++trinketTicks) {
            now += 16u;
            updateTrinket(now, restPaw);
            if (consumeTrinketLanded()) {
                if (landPulses == 0) landTick = (uint16_t)(trinketTicks + 1u);
                ++landPulses;
            }
        }
        const int landedY = (int)(trinketYQ4 / 16) + trinketHeight();
        const int drift = (int)((trinketXQ4 - shelfXQ4) / 16);
        const int restX = (int)(trinketXQ4 / 16);
        printf("[CAT TRINKET] face=%s settled=%ums y=%d drift=%d bounces=%u "
               "lying=%d w=%d land=%ums pulses=%u\n",
               faceRight ? "R" : "L", (unsigned)(trinketTicks * 16u),
               landedY, drift, (unsigned)trinketBounces,
               trinketLying ? 1 : 0, trinketWidth(),
               (unsigned)(landTick * 16u), (unsigned)landPulses);
        if (trinketFalling || trinketRollQ4 != 0 || !trinketLying ||
            landedY != UIMeasurements::MenuPigLayout::kFloorY ||
            trinketBounces > kTrinketMaxBounces ||
            (faceRight ? drift <= 0 : drift >= 0) ||
            trinketTicks * 16u > 900u ||
            restX < 0 || restX + trinketWidth() > SCREEN_WIDTH)
            ok = false;
        // One pulse, on the contact frame the simulation puts at 320ms for a
        // tub-rim drop. Anything at tick zero is the old single-sound bug back.
        if (landPulses != 1u || landTick * 16u < 240u ||
            landTick * 16u > 400u)
            ok = false;
        // The lit end cap has to point along the travel, or the tin reads as
        // having landed facing back the way it came.
        if (trinketCapRight != faceRight) ok = false;

        // A knocked trinket has left the surface for good: a later room anchor
        // must not teleport it back onto the shelf mid-visit.
        setTrinketAnchor(40, shelfY, true);
        if ((int)(trinketXQ4 / 16) == 40) ok = false;
    }

    // A knock delivered at floor level has nowhere to fall, so contact is the
    // same frame as the beat and the clatter must still be announced exactly
    // once. This is the path where the topple happens outside the fall.
    now += 32u;
    reset(now);
    planTrinketForVisit(0u);
    trinketPresent = true;
    trinketKnockable = true;
    trinketBatsToKnock = 1u;
    const int16_t floorY = (int16_t)UIMeasurements::MenuPigLayout::kFloorY;
    stationCat(160, floorY, true);
    setTrinketAnchor(160, floorY, true);
    updateTrinket(now, restPaw);
    now += 16u;
    updateTrinket(now, liftPaw);
    const bool floorLanded = consumeTrinketLanded();
    printf("[CAT FLOOR KNOCK] falling=%d lying=%d land=%d\n",
           trinketFalling ? 1 : 0, trinketLying ? 1 : 0,
           floorLanded ? 1 : 0);
    if (trinketFalling || !trinketLying || !floorLanded ||
        consumeTrinketLanded())
        ok = false;

    // The object is latched to its surface for the visit. Losing the station
    // must not delete a thing that is sitting on a shelf, and settling at a
    // new station must not drag it across the room.
    now += 32u;
    reset(now);
    planTrinketForVisit(0u);
    trinketPresent = true;
    trinketKnockable = false;
    stationCat(160, shelfY, true);
    setTrinketAnchor(160, shelfY, true);
    const bool placed = trinketVisible();
    setTrinketAnchor(0, 0, false);
    const bool survivedWalkAway = trinketVisible();
    setTrinketAnchor(60, shelfY, true);
    const int latchedX = trinketAnchorX;
    printf("[CAT TRINKET LATCH] placed=%d kept=%d x=%d\n",
           placed ? 1 : 0, survivedWalkAway ? 1 : 0, latchedX);
    if (!placed || !survivedWalkAway || latchedX != 160) ok = false;

    // Latched to a surface means a beat struck somewhere else cannot move it.
    // Without the reach test a rake on the far side of the room would tip over
    // a tin the cat is nowhere near.
    stationCat(160, shelfY, true);
    navigationPose.x = (int16_t)(navigationPose.x - 64);
    updateTrinket(now, restPaw);
    now += 16u;
    updateTrinket(now, liftPaw);
    const uint8_t batsFromAcrossTheRoom = trinketBatsSeen;
    // Standing beside it is not standing over it either.
    stationCat(160, shelfY, true);
    navigationPose.y = (int16_t)originYForSupport(shelfY - 40);
    now += 16u;
    updateTrinket(now, restPaw);
    now += 16u;
    updateTrinket(now, liftPaw);
    const uint8_t batsFromAbove = trinketBatsSeen;
    printf("[CAT TRINKET REACH] far=%u above=%u\n",
           (unsigned)batsFromAcrossTheRoom, (unsigned)batsFromAbove);
    if (batsFromAcrossTheRoom != 0u || batsFromAbove != 0u ||
        consumeTrinketBat())
        ok = false;

    // The room lights the object from the centre this reports, so it has to be
    // the drawn footprint's centre and it has to travel. Reporting the anchor
    // would shade a tin in mid-air for the shelf it has already left.
    now += 32u;
    reset(now);
    planTrinketForVisit(0u);
    trinketPresent = true;
    trinketKnockable = true;
    trinketBatsToKnock = 1u;
    int16_t centerX = 0;
    int16_t centerY = 0;
    const bool centerBeforePlacement = trinketCenter(centerX, centerY);
    stationCat(160, shelfY, true);
    setTrinketAnchor(160, shelfY, true);
    updateTrinket(now, restPaw);
    const bool centerPlaced = trinketCenter(centerX, centerY);
    int originX = 0;
    int originY = 0;
    trinketDrawOrigin(originX, originY);
    const bool centerMatchesDrawn =
        centerX == originX + trinketWidth() / 2 &&
        centerY == originY + trinketHeight() / 2;
    const int16_t shelfCenterY = centerY;
    now += 16u;
    updateTrinket(now, liftPaw);
    for (uint16_t step = 0; step < 8u; ++step) {
        now += 16u;
        updateTrinket(now, restPaw);
    }
    trinketCenter(centerX, centerY);
    printf("[CAT TRINKET LIGHT] placed=%d drawn=%d shelf=%d falling=%d\n",
           centerPlaced ? 1 : 0, centerMatchesDrawn ? 1 : 0,
           (int)shelfCenterY, (int)centerY);
    if (centerBeforePlacement || !centerPlaced || !centerMatchesDrawn ||
        centerY <= shelfCenterY)
        ok = false;

    // The object stays behind when the cat goes. Its physics has to run on the
    // off-screen path too, or a tin knocked on his way out hangs in the air
    // for the rest of the visit.
    now += 32u;
    reset(now);
    planTrinketForVisit(0u);
    trinketPresent = true;
    trinketKnockable = true;
    trinketBatsToKnock = 1u;
    stationCat(160, shelfY, true);
    setTrinketAnchor(160, shelfY, true);
    updateTrinket(now, restPaw);
    now += 16u;
    updateTrinket(now, liftPaw);
    const bool knockedOnTheWayOut = trinketKnocked && trinketFalling;
    catVisible = false;
    const Pose exitTarget{
        0, (int16_t)originYForSupport(
               UIMeasurements::MenuPigLayout::kFloorY), false};
    uint16_t unattendedTicks = 0;
    for (; unattendedTicks < 200u &&
           (trinketFalling || trinketRollQ4 != 0); ++unattendedTicks) {
        now += 16u;
        updateNavigation(now, exitTarget, Activity::FOLLOW, false, false);
    }
    const bool landedUnattended = consumeTrinketLanded();
    const int unattendedY = (int)(trinketYQ4 / 16) + trinketHeight();
    printf("[CAT TRINKET UNATTENDED] knocked=%d ticks=%u bottom=%d land=%d\n",
           knockedOnTheWayOut ? 1 : 0, (unsigned)unattendedTicks,
           unattendedY, landedUnattended ? 1 : 0);
    if (!knockedOnTheWayOut || unattendedTicks >= 200u || !landedUnattended ||
        !trinketLying ||
        unattendedY != UIMeasurements::MenuPigLayout::kFloorY)
        ok = false;

    // Running off-screen is not the same as reaching off-screen. His rig keeps
    // advancing out there so he does not re-enter mid-clip, and none of what
    // it does may touch a thing he has walked out on.
    now += 32u;
    reset(now);
    planTrinketForVisit(0u);
    trinketPresent = true;
    trinketKnockable = false;
    stationCat(160, shelfY, true);
    setTrinketAnchor(160, shelfY, true);
    catVisible = false;
    updateTrinket(now, restPaw);
    now += 16u;
    updateTrinket(now, liftPaw);
    printf("[CAT TRINKET ABSENT] bats=%u wobble=%d\n",
           (unsigned)trinketBatsSeen, (int)trinketWobbleVel);
    if (trinketBatsSeen != 0u || trinketWobbleVel != 0 || consumeTrinketBat())
        ok = false;
    catVisible = true;

    // A paw beat that does not knock the tin over still has to read: the rock
    // must tip both ways on the 2px lattice and return to exactly upright.
    now += 32u;
    reset(now);
    planTrinketForVisit(0u);
    trinketPresent = true;
    trinketKnockable = false;
    stationCat(160, shelfY, true);
    setTrinketAnchor(160, shelfY, true);
    updateTrinket(now, restPaw);
    now += 16u;
    updateTrinket(now, liftPaw);
    // A beat the object survives is still a contact, and it used to be the one
    // part of the gag with nothing audible attached.
    if (!consumeTrinketBat() || consumeTrinketKnock()) ok = false;
    int leanLeft = 0;
    int leanRight = 0;
    for (uint16_t step = 0; step < 60u; ++step) {
        now += 16u;
        updateTrinket(now, restPaw);
        if (trinketWobble >= kTrinketLeanThreshold) ++leanRight;
        else if (trinketWobble <= -kTrinketLeanThreshold) ++leanLeft;
    }
    printf("[CAT ROCK] right=%d left=%d rest=%d vel=%d\n",
           leanRight, leanLeft, (int)trinketWobble, (int)trinketWobbleVel);
    if (leanRight == 0 || leanLeft == 0 ||
        trinketWobble != 0 || trinketWobbleVel != 0)
        ok = false;

    // Beats are counted off the real SCRATCH clip, not a synthetic pose pair.
    // The clip rakes four times without ever planting the paw between strokes,
    // so a latch that only watches raised-vs-planted scores the whole rake as
    // one bat and the tin sits still through three visible contacts.
    now += 32u;
    reset(now);
    planTrinketForVisit(0u);
    trinketPresent = true;
    trinketKnockable = false;
    setTrinketAnchor((int16_t)(160 + kWidth + trinketGap), shelfY, true);
    forceStateForCapture(now, Pose{160, (int16_t)originYForSupport(shelfY),
                                   true},
                         Activity::SCRATCH, false);
    const uint32_t scratchMs = actionDuration(Activity::SCRATCH);
    for (uint32_t elapsed = 0; elapsed < scratchMs; elapsed += 16u) {
        now += 16u;
        updateTrinket(now, sampleRuntimeAnimation(now));
    }
    printf("[CAT BEATS] scratch=%ums bats=%u\n",
           (unsigned)scratchMs, (unsigned)trinketBatsSeen);
    // Four strokes in the clip; the rake has to register more than the single
    // beat the raised-edge latch used to see.
    if (trinketBatsSeen < 4u) ok = false;

    reset(now);
    return ok;
}
#endif

}  // namespace PancettaCat
