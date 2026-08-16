#include "cat_behavior_director.h"

#include <stddef.h>

namespace CatBehavior {
namespace {

// ==[ ROUTINE CASEBOOK ]==
// Context proposes the case, observed playback supplies the witness, and only
// a completed authored sequence earns progression. Timers alone have no alibi.

constexpr uint8_t kRoomCount = 6;
constexpr uint8_t kMaxPhases = 4;
constexpr uint8_t kCompletionCapacity = 4;
constexpr uint32_t kObservedDeltaCapMs = 250;
constexpr uint32_t kRoutineTimeoutMs = 48000;
constexpr uint32_t kInitialDelayMinMs = 2400;
constexpr uint32_t kInitialDelayJitterMs = 2600;
constexpr uint32_t kBetweenRoutineMinMs = 6500;
constexpr uint32_t kBetweenRoutineJitterMs = 9000;

constexpr uint8_t intentBit(Intent intent) {
    return static_cast<uint8_t>(1u << static_cast<uint8_t>(intent));
}

constexpr uint8_t kIntentRoom = intentBit(Intent::ROOM_INTEREST);
constexpr uint8_t kIntentCompany = intentBit(Intent::COMPANY);
constexpr uint8_t kIntentRoomOrCompany = kIntentRoom | kIntentCompany;

struct Phase {
    Action action;
    uint16_t observedMs;
    uint8_t minBond;
    bool needsChannel;
};

struct RoutineDef {
    Routine routine;
    Category category;
    uint8_t intentMask;
    uint8_t minBond;
    uint8_t phaseCount;
    bool roomDossier;
    Phase phases[kMaxPhases];
};

constexpr Phase phase(Action action, uint16_t observedMs,
                      uint8_t minBond = 0, bool needsChannel = false) {
    return {action, observedMs, minBond, needsChannel};
}

constexpr Phase noPhase() {
    return {Action::FOLLOW, 0, 0, false};
}

// High-level routines intentionally reuse the proven PancettaCat clips. The
// novelty is in context, sequencing, unlock gates, and completion semantics —
// not in adding another renderer or duplicating the low-level state machine.
constexpr RoutineDef kRoutines[] = {
    {Routine::SHADOW_PATROL, Category::SOCIAL, kIntentRoomOrCompany, 0, 2, false,
     {phase(Action::FOLLOW, 1500), phase(Action::SLOW_BLINK, 1500, 1),
      noPhase(), noPhase()}},
    {Routine::CABLE_FORENSICS, Category::CURIOSITY, kIntentRoom, 0, 3, true,
     {phase(Action::WATCH_CABLES, 3200), phase(Action::MEOW, 900, 1, true),
      phase(Action::SCRATCH, 1800, 1), noPhase()}},
    {Routine::RAIN_STAKEOUT, Category::VIGILANCE, kIntentRoom, 0, 3, true,
     {phase(Action::WATCH_RAIN, 3600), phase(Action::SLOW_BLINK, 1400, 1),
      phase(Action::GROOM, 1900, 1), noPhase()}},
    {Routine::RAMEN_INSPECTION, Category::CURIOSITY, kIntentRoom, 0, 3, true,
     {phase(Action::SNIFF_FOOD, 3000), phase(Action::MEOW, 850, 1, true),
      phase(Action::GROOM, 1700, 1), noPhase()}},
    {Routine::SKY_AMBUSH, Category::VIGILANCE, kIntentRoom, 0, 3, true,
     {phase(Action::STALK_SKY, 3000), phase(Action::ARCH, 1250, 1),
      phase(Action::ZOOMIES, 2400, 2), noPhase()}},
    {Routine::BAR_PATROL, Category::VIGILANCE, kIntentRoom, 0, 3, true,
     {phase(Action::PROWL_BAR, 3200), phase(Action::MEOW, 850, 1, true),
      phase(Action::SLEEP, 2100, 1), noPhase()}},
    {Routine::WATER_TRIBUNAL, Category::CURIOSITY, kIntentRoom, 0, 3, true,
     {phase(Action::WATCH_WATER, 3200), phase(Action::ARCH, 1100, 1),
      phase(Action::GROOM, 1800, 1), noPhase()}},
    {Routine::CLAW_MAINTENANCE, Category::SELF_CARE, kIntentRoom, 1, 2, false,
     {phase(Action::SCRATCH, 2100, 1), phase(Action::GROOM, 2100, 1),
      noPhase(), noPhase()}},
    {Routine::FALSE_ALARM, Category::VIGILANCE, kIntentRoom, 1, 3, false,
     {phase(Action::ARCH, 1250, 1), phase(Action::MEOW, 850, 1, true),
      phase(Action::GROOM, 1700, 1), noPhase()}},
    {Routine::MIDNIGHT_ZOOMIES, Category::MISCHIEF, kIntentRoom, 2, 3, false,
     {phase(Action::STALK_SKY, 1300, 2), phase(Action::ZOOMIES, 3000, 2),
      phase(Action::SLEEP, 1800, 2), noPhase()}},
    {Routine::LOW_MOOD_COMFORT, Category::COMFORT, kIntentCompany, 2, 3, false,
     {phase(Action::SLOW_BLINK, 1700, 2), phase(Action::FACE_BUMP, 1500, 2),
      phase(Action::HEAD_NAP, 3000, 3), noPhase()}},
    {Routine::GOAL_CELEBRATION, Category::MISCHIEF, kIntentRoomOrCompany, 1, 3, false,
     {phase(Action::MEOW, 950, 1, true), phase(Action::ZOOMIES, 2800, 2),
      phase(Action::SLOW_BLINK, 1600, 1), noPhase()}},
    {Routine::STREAK_RITUAL, Category::SOCIAL, kIntentCompany, 2, 3, false,
     {phase(Action::KNEAD, 2300, 2), phase(Action::FACE_BUMP, 1500, 2),
      phase(Action::HEAD_NAP, 2700, 3), noPhase()}},
    {Routine::TRUST_GREETING, Category::SOCIAL, kIntentCompany, 1, 2, false,
     {phase(Action::SLOW_BLINK, 1700, 1), phase(Action::FACE_BUMP, 1500, 2),
      noPhase(), noPhase()}},
    {Routine::QUIET_COMPANY, Category::COMFORT, kIntentCompany, 2, 2, false,
     {phase(Action::KNEAD, 2200, 2), phase(Action::SLEEP, 3000, 2),
      noPhase(), noPhase()}},
    {Routine::SIGNAL_REPORT, Category::VOICE, kIntentRoomOrCompany, 1, 2, false,
     {phase(Action::MEOW, 1000, 1, true), phase(Action::WATCH_CABLES, 2200, 1),
      noPhase(), noPhase()}},
    {Routine::TRINKET_MISCHIEF, Category::MISCHIEF, kIntentRoom, 2, 3, false,
     {phase(Action::STALK_SKY, 1500, 2), phase(Action::ZOOMIES, 2500, 2),
      phase(Action::MEOW, 800, 2, true), noPhase()}},
    {Routine::POWER_NAP, Category::COMFORT, kIntentRoomOrCompany, 1, 2, false,
     {phase(Action::GROOM, 1700, 1), phase(Action::SLEEP, 3200, 1),
      noPhase(), noPhase()}},
    {Routine::HAIRBALL_INCIDENT, Category::SELF_CARE, kIntentRoom, 1, 3, false,
     {phase(Action::HAIRBALL, 1250, 1), phase(Action::GROOM, 1900, 1),
      phase(Action::SLOW_BLINK, 1200, 1), noPhase()}},
    {Routine::BATH_SUPERVISOR, Category::CURIOSITY, kIntentRoomOrCompany, 1, 2, false,
     {phase(Action::WATCH_WATER, 2800, 1), phase(Action::SLOW_BLINK, 1500, 1),
      noPhase(), noPhase()}},
    {Routine::CASE_REVIEW, Category::VOICE, kIntentRoom, 1, 3, false,
     {phase(Action::MEOW, 900, 1, true), phase(Action::SLOW_BLINK, 1400, 1),
      phase(Action::GROOM, 1700, 1), noPhase()}},
};

static_assert(sizeof(kRoutines) / sizeof(kRoutines[0]) + 1 ==
                  static_cast<size_t>(Routine::COUNT),
              "every CatBehavior routine needs a definition");

struct State {
    bool initialized = false;
    uint32_t rng = 0xC47B0A7Du;
    uint32_t lastUpdateMs = 0;
    uint32_t nextRoutineMs = 0;
    uint32_t routineStartMs = 0;
    uint32_t observedMs = 0;
    Routine routine = Routine::NONE;
    uint8_t phase = 0;
    uint8_t routineRoom = 0;
    uint8_t lastRoom = 0;
    uint8_t lastGoalProgress = 0;
    uint8_t lastStreak = 0;
    uint8_t lastBondTier = 0;
    Reward trigger = Reward::NONE;
    bool pendingGoal = false;
    bool pendingStreak = false;
    bool pendingBond = false;
    Routine forcedRoutine = Routine::NONE;
    bool forcedPending = false;
};

State state;
Completion completionQueue[kCompletionCapacity];
uint8_t completionHead = 0;
uint8_t completionTail = 0;
uint8_t completionCount = 0;

bool reached(uint32_t now, uint32_t deadline) {
    return static_cast<int32_t>(now - deadline) >= 0;
}

uint32_t nextRandom() {
    uint32_t x = state.rng;
    if (x == 0) x = 0x6D2B79F5u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    state.rng = x;
    return x;
}

uint32_t randomBelow(uint32_t bound) {
    return bound == 0 ? 0 : nextRandom() % bound;
}

const RoutineDef* definition(Routine routine) {
    if (routine == Routine::NONE || routine >= Routine::COUNT) return nullptr;
    const size_t index = static_cast<size_t>(routine) - 1;
    if (index >= sizeof(kRoutines) / sizeof(kRoutines[0])) return nullptr;
    return &kRoutines[index];
}

bool intentAllowed(const RoutineDef& def, Intent intent) {
    return (def.intentMask & intentBit(intent)) != 0;
}

bool phaseAllowed(const Phase& candidate, const Context& context,
                  uint8_t bondTier) {
    if (candidate.observedMs == 0) return false;
    if (candidate.minBond > bondTier) return false;
    if (candidate.needsChannel && !context.channelClear) return false;
    return true;
}

bool seekAllowedPhase(const RoutineDef& def, const Context& context,
                      uint8_t bondTier, uint8_t start, uint8_t& outPhase) {
    for (uint8_t i = start; i < def.phaseCount; ++i) {
        if (phaseAllowed(def.phases[i], context, bondTier)) {
            outPhase = i;
            return true;
        }
    }
    return false;
}

bool isActiveContext(const Context& context) {
    return !context.helperScene && !context.pigMoving && context.stationary &&
           context.visible && !context.eventHold &&
           context.intent != Intent::FOLLOW && context.intent != Intent::EXIT;
}

void scheduleNext(uint32_t now) {
    state.nextRoutineMs = now + kBetweenRoutineMinMs +
        randomBelow(kBetweenRoutineJitterMs + 1u);
}

void clearRoutine(uint32_t now, bool shortRetry) {
    state.routine = Routine::NONE;
    state.phase = 0;
    state.observedMs = 0;
    state.routineStartMs = 0;
    state.trigger = Reward::NONE;
    state.nextRoutineMs = now + (shortRetry ? 1200u :
        kBetweenRoutineMinMs + randomBelow(kBetweenRoutineJitterMs + 1u));
}

bool queueCompletion(const Completion& completion) {
    if (completionCount >= kCompletionCapacity) return false;
    completionQueue[completionTail] = completion;
    completionTail = static_cast<uint8_t>((completionTail + 1u) %
                                          kCompletionCapacity);
    ++completionCount;
    return true;
}

Routine roomRoutine(uint8_t room) {
    switch (room % kRoomCount) {
        case 0: return Routine::CABLE_FORENSICS;
        case 1: return Routine::RAIN_STAKEOUT;
        case 2: return Routine::RAMEN_INSPECTION;
        case 3: return Routine::SKY_AMBUSH;
        case 4: return Routine::BAR_PATROL;
        case 5: return Routine::WATER_TRIBUNAL;
        default: return Routine::CABLE_FORENSICS;
    }
}

bool startRoutine(Routine routine, Reward trigger, const Context& context,
                  uint8_t bondTier) {
    const RoutineDef* def = definition(routine);
    if (!def || def->minBond > bondTier ||
        !intentAllowed(*def, context.intent)) return false;

    uint8_t firstPhase = 0;
    if (!seekAllowedPhase(*def, context, bondTier, 0, firstPhase)) return false;

    state.routine = routine;
    state.phase = firstPhase;
    state.observedMs = 0;
    state.routineStartMs = context.nowMs;
    state.routineRoom = context.room % kRoomCount;
    state.trigger = trigger;
    return true;
}

Routine chooseRoomRoutine(const Context& context, uint8_t bondTier) {
    const uint8_t room = context.room % kRoomCount;
    const uint8_t roomBit = static_cast<uint8_t>(1u << room);
    if ((context.dossierMask & roomBit) == 0 && randomBelow(100) < 72)
        return roomRoutine(room);

    const uint32_t roll = randomBelow(100);
    if (context.mood >= 45 && bondTier >= 2 && roll < 24)
        return Routine::MIDNIGHT_ZOOMIES;
    if (context.mood <= -35 && roll < 34)
        return Routine::POWER_NAP;
    if (bondTier >= 2 && roll < 42)
        return Routine::TRINKET_MISCHIEF;
    if (bondTier >= 1 && roll < 56)
        return Routine::CLAW_MAINTENANCE;
    if (bondTier >= 1 && roll < 68)
        return Routine::FALSE_ALARM;
    if (bondTier >= 1 && roll < 78)
        return Routine::HAIRBALL_INCIDENT;
    if (bondTier >= 1 && roll < 88)
        return Routine::CASE_REVIEW;
    return roomRoutine(room);
}

Routine chooseCompanyRoutine(const Context& context, uint8_t bondTier) {
    if (context.mood <= -25 && bondTier >= 2)
        return Routine::LOW_MOOD_COMFORT;

    // Existing station enum values place sofa, bed, and bath late in the table;
    // exact station identities remain owned by MenuPig. The room and mood still
    // provide stable context here without importing its private enum.
    if (context.room == 5 && bondTier >= 1)
        return Routine::BATH_SUPERVISOR;

    const uint32_t roll = randomBelow(100);
    if (bondTier >= 3 && roll < 28) return Routine::STREAK_RITUAL;
    if (bondTier >= 2 && roll < 58) return Routine::QUIET_COMPANY;
    if (bondTier >= 1 && roll < 84) return Routine::TRUST_GREETING;
    return Routine::SHADOW_PATROL;
}

bool startPendingTrigger(const Context& context, uint8_t bondTier) {
    if (state.pendingGoal) {
        if (startRoutine(Routine::GOAL_CELEBRATION, Reward::DAILY_GOAL,
                         context, bondTier)) return true;
    }
    if (state.pendingStreak) {
        Routine routine = context.intent == Intent::COMPANY
            ? Routine::STREAK_RITUAL : Routine::GOAL_CELEBRATION;
        if (startRoutine(routine, Reward::STREAK, context, bondTier))
            return true;
    }
    if (state.pendingBond) {
        Routine routine = context.intent == Intent::COMPANY
            ? Routine::TRUST_GREETING : Routine::SIGNAL_REPORT;
        if (startRoutine(routine, Reward::BOND, context, bondTier))
            return true;
    }
    return false;
}

void emitTriggerReward(Reward reward, Routine routine,
                       const RoutineDef& def, uint8_t room) {
    Completion completion;
    completion.reward = reward;
    completion.routine = routine;
    completion.category = def.category;
    completion.room = room;
    switch (reward) {
        case Reward::DAILY_GOAL:
            completion.xp = 3;
            completion.momentum = 2;
            completion.label = "goal celebration";
            state.pendingGoal = false;
            break;
        case Reward::STREAK:
            completion.xp = 4;
            completion.momentum = 2;
            completion.label = "streak ritual";
            state.pendingStreak = false;
            break;
        case Reward::BOND:
            completion.xp = 2;
            completion.momentum = 1;
            completion.label = "bond advanced";
            state.pendingBond = false;
            break;
        default:
            return;
    }
    queueCompletion(completion);
}

void finishRoutine(const Context& context) {
    const Routine completedRoutine = state.routine;
    const RoutineDef* def = definition(completedRoutine);
    const Reward trigger = state.trigger;
    const uint8_t room = state.routineRoom;
    if (!def) {
        clearRoutine(context.nowMs, false);
        return;
    }

    if (def->roomDossier && room < kRoomCount && context.room == room) {
        const uint8_t roomBit = static_cast<uint8_t>(1u << room);
        if ((context.dossierMask & roomBit) == 0) {
            Completion completion;
            completion.reward = Reward::ROOM_DOSSIER;
            completion.routine = completedRoutine;
            completion.category = def->category;
            completion.room = room;
            completion.xp = 4;
            completion.momentum = 1;
            completion.label = "room dossier";
            queueCompletion(completion);
        }
    }

    const uint8_t category = static_cast<uint8_t>(def->category);
    if (category < static_cast<uint8_t>(Category::COUNT)) {
        const uint8_t categoryBit = static_cast<uint8_t>(1u << category);
        if ((context.masteryMask & categoryBit) == 0) {
            Completion completion;
            completion.reward = Reward::BEHAVIOR_MASTERY;
            completion.routine = completedRoutine;
            completion.category = def->category;
            completion.room = room;
            completion.xp = 2;
            completion.momentum = 1;
            completion.label = categoryName(def->category);
            queueCompletion(completion);
        }
    }

    if (trigger != Reward::NONE)
        emitTriggerReward(trigger, completedRoutine, *def, room);

    state.routine = Routine::NONE;
    state.phase = 0;
    state.observedMs = 0;
    state.routineStartMs = 0;
    state.trigger = Reward::NONE;
    scheduleNext(context.nowMs);
}

Decision currentDecision(uint8_t bondTier) {
    Decision decision;
    decision.bondTier = bondTier;
    decision.routine = state.routine;
    decision.phase = state.phase;
    const RoutineDef* def = definition(state.routine);
    if (def && state.phase < def->phaseCount) {
        decision.action = def->phases[state.phase].action;
        decision.overrideBase = true;
    }
    return decision;
}

}  // namespace

uint8_t popcount8(uint8_t value) {
    uint8_t count = 0;
    while (value != 0) {
        value = static_cast<uint8_t>(value & static_cast<uint8_t>(value - 1u));
        ++count;
    }
    return count;
}

uint8_t bondTierFor(uint8_t memories, uint8_t dossierMask,
                    uint8_t masteryMask, uint8_t streak,
                    uint8_t goalProgress) {
    const uint16_t score = static_cast<uint16_t>(memories) * 9u +
        static_cast<uint16_t>(popcount8(dossierMask & 0x3Fu)) * 7u +
        static_cast<uint16_t>(popcount8(masteryMask & 0x7Fu)) * 5u +
        static_cast<uint16_t>(streak > 7 ? 7 : streak) * 2u +
        static_cast<uint16_t>(goalProgress > 100 ? 100 : goalProgress) / 10u;
    if (score >= 92u) return 4;
    if (score >= 62u) return 3;
    if (score >= 36u) return 2;
    if (score >= 14u) return 1;
    return 0;
}

void reset(uint32_t nowMs, uint32_t seed) {
    state = {};
    state.rng = seed != 0 ? seed : 0xC47B0A7Du;
    state.lastUpdateMs = nowMs;
    state.nextRoutineMs = nowMs + kInitialDelayMinMs +
        (state.rng % (kInitialDelayJitterMs + 1u));
    completionHead = 0;
    completionTail = 0;
    completionCount = 0;
}

Decision update(const Context& context, Action observedAction) {
    state.rng ^= context.entropy + 0x9E3779B9u +
        static_cast<uint32_t>(context.room) * 0x45D9F3Bu;

    const uint8_t bondTier = bondTierFor(
        context.memories, context.dossierMask, context.masteryMask,
        context.streak, context.goalProgress);

    if (!state.initialized) {
        state.initialized = true;
        state.lastUpdateMs = context.nowMs;
        state.lastRoom = context.room;
        state.lastGoalProgress = context.goalProgress;
        state.lastStreak = context.streak;
        state.lastBondTier = bondTier;
        return currentDecision(bondTier);
    }

    uint32_t delta = context.nowMs - state.lastUpdateMs;
    state.lastUpdateMs = context.nowMs;
    if (delta > kObservedDeltaCapMs) delta = kObservedDeltaCapMs;

    if (context.sessionActive && state.lastGoalProgress < 100u &&
        context.goalProgress >= 100u)
        state.pendingGoal = true;
    if (context.sessionActive && context.streak > state.lastStreak &&
        ((state.lastStreak < 3u && context.streak >= 3u) ||
         (state.lastStreak < 7u && context.streak >= 7u)))
        state.pendingStreak = true;
    if (bondTier > state.lastBondTier)
        state.pendingBond = true;

    state.lastGoalProgress = context.goalProgress;
    state.lastStreak = context.streak;
    state.lastBondTier = bondTier;

    const bool active = isActiveContext(context);
    const RoutineDef* def = definition(state.routine);

    if (def && (context.room != state.routineRoom ||
                !intentAllowed(*def, context.intent))) {
        // The reward trigger remains pending. Only the interrupted visual plan
        // is discarded, so room changes cannot silently eat a goal/streak cue.
        clearRoutine(context.nowMs, true);
        def = nullptr;
    }

    if (def && context.nowMs - state.routineStartMs >= kRoutineTimeoutMs) {
        clearRoutine(context.nowMs, true);
        def = nullptr;
    }

    if (def && active && !context.navigating && !context.transmitting &&
        state.phase < def->phaseCount) {
        const Phase& requested = def->phases[state.phase];
        if (observedAction == requested.action) {
            state.observedMs = UINT32_MAX - state.observedMs < delta
                ? UINT32_MAX
                : state.observedMs + delta;
        }

        if (state.observedMs >= requested.observedMs) {
            uint8_t nextPhase = 0;
            if (seekAllowedPhase(*def, context, bondTier,
                                 static_cast<uint8_t>(state.phase + 1u),
                                 nextPhase)) {
                state.phase = nextPhase;
                state.observedMs = 0;
            } else {
                finishRoutine(context);
                def = nullptr;
            }
        }
    }

    if (state.routine == Routine::NONE && active && !context.navigating &&
        !context.transmitting) {
        bool started = false;
        const bool forcedWasPending = state.forcedPending;
        if (forcedWasPending) {
            started = startRoutine(state.forcedRoutine, Reward::NONE,
                                   context, bondTier);
            if (started) state.forcedPending = false;
        }
        if (!started && !forcedWasPending)
            started = startPendingTrigger(context, bondTier);
        if (!started && !forcedWasPending &&
            reached(context.nowMs, state.nextRoutineMs)) {
            const Routine selected = context.intent == Intent::COMPANY
                ? chooseCompanyRoutine(context, bondTier)
                : chooseRoomRoutine(context, bondTier);
            started = startRoutine(selected, Reward::NONE, context, bondTier);
            if (!started) scheduleNext(context.nowMs);
        }
    }

    state.lastRoom = context.room;
    return currentDecision(bondTier);
}

bool consumeCompletion(Completion& out) {
    if (completionCount == 0) return false;
    out = completionQueue[completionHead];
    completionHead = static_cast<uint8_t>((completionHead + 1u) %
                                          kCompletionCapacity);
    --completionCount;
    return true;
}

bool forceRoutine(Routine routine, uint32_t nowMs) {
    if (definition(routine) == nullptr) return false;
    state.forcedRoutine = routine;
    state.forcedPending = true;
    state.nextRoutineMs = nowMs;
    return true;
}

const char* routineName(Routine routine) {
    switch (routine) {
        case Routine::NONE: return "idle";
        case Routine::SHADOW_PATROL: return "shadow patrol";
        case Routine::CABLE_FORENSICS: return "cable forensics";
        case Routine::RAIN_STAKEOUT: return "rain stakeout";
        case Routine::RAMEN_INSPECTION: return "ramen inspection";
        case Routine::SKY_AMBUSH: return "sky ambush";
        case Routine::BAR_PATROL: return "bar patrol";
        case Routine::WATER_TRIBUNAL: return "water tribunal";
        case Routine::CLAW_MAINTENANCE: return "claw maintenance";
        case Routine::FALSE_ALARM: return "false alarm";
        case Routine::MIDNIGHT_ZOOMIES: return "midnight zoomies";
        case Routine::LOW_MOOD_COMFORT: return "low mood comfort";
        case Routine::GOAL_CELEBRATION: return "goal celebration";
        case Routine::STREAK_RITUAL: return "streak ritual";
        case Routine::TRUST_GREETING: return "trust greeting";
        case Routine::QUIET_COMPANY: return "quiet company";
        case Routine::SIGNAL_REPORT: return "signal report";
        case Routine::TRINKET_MISCHIEF: return "trinket mischief";
        case Routine::POWER_NAP: return "power nap";
        case Routine::HAIRBALL_INCIDENT: return "hairball incident";
        case Routine::BATH_SUPERVISOR: return "bath supervisor";
        case Routine::CASE_REVIEW: return "case review";
        case Routine::COUNT: break;
    }
    return "unknown";
}

const char* categoryName(Category category) {
    switch (category) {
        case Category::CURIOSITY: return "curiosity mastery";
        case Category::VIGILANCE: return "vigilance mastery";
        case Category::SOCIAL: return "social mastery";
        case Category::COMFORT: return "comfort mastery";
        case Category::MISCHIEF: return "mischief mastery";
        case Category::SELF_CARE: return "self-care mastery";
        case Category::VOICE: return "voice mastery";
        case Category::COUNT: break;
    }
    return "cat mastery";
}

}  // namespace CatBehavior
