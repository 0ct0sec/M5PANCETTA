/**
 * CatBehavior — deterministic high-level routine director for Pancetta's cat.
 *
 * This layer deliberately does not render or move the actor. It proposes
 * authored routines using the existing PancettaCat activities, observes what
 * the low-level state machine actually played, and only emits progression
 * events after the requested phases were visibly completed.
 *
 * No allocation, Arduino dependency, wall clock, or persistence lives here.
 */
#pragma once

#include <stdint.h>

namespace CatBehavior {

enum class Intent : uint8_t {
    FOLLOW = 0,
    ROOM_INTEREST,
    COMPANY,
    EXIT,
};

enum class Action : uint8_t {
    FOLLOW = 0,
    WATCH_CABLES,
    WATCH_RAIN,
    SNIFF_FOOD,
    STALK_SKY,
    PROWL_BAR,
    WATCH_WATER,
    SCRATCH,
    SLEEP,
    GROOM,
    HAIRBALL,
    ARCH,
    ZOOMIES,
    FACE_BUMP,
    HEAD_NAP,
    KNEAD,
    SLOW_BLINK,
    MEOW,
    COUNT,
};

enum class Category : uint8_t {
    CURIOSITY = 0,
    VIGILANCE,
    SOCIAL,
    COMFORT,
    MISCHIEF,
    SELF_CARE,
    VOICE,
    COUNT,
};

enum class Routine : uint8_t {
    NONE = 0,
    SHADOW_PATROL,
    CABLE_FORENSICS,
    RAIN_STAKEOUT,
    RAMEN_INSPECTION,
    SKY_AMBUSH,
    BAR_PATROL,
    WATER_TRIBUNAL,
    CLAW_MAINTENANCE,
    FALSE_ALARM,
    MIDNIGHT_ZOOMIES,
    LOW_MOOD_COMFORT,
    GOAL_CELEBRATION,
    STREAK_RITUAL,
    TRUST_GREETING,
    QUIET_COMPANY,
    SIGNAL_REPORT,
    TRINKET_MISCHIEF,
    POWER_NAP,
    HAIRBALL_INCIDENT,
    BATH_SUPERVISOR,
    CASE_REVIEW,
    COUNT,
};

enum class Reward : uint8_t {
    NONE = 0,
    ROOM_DOSSIER,
    BEHAVIOR_MASTERY,
    DAILY_GOAL,
    STREAK,
    BOND,
};

struct Context {
    uint32_t nowMs = 0;
    uint32_t entropy = 0;
    int16_t mood = 0;              // expected range -100..100
    uint8_t room = 0;              // 0..5
    uint8_t station = 0;
    uint8_t goalProgress = 0;      // 0..100
    uint8_t streak = 0;
    uint8_t memories = 0;
    uint8_t dossierMask = 0;       // six low bits, one per room
    uint8_t masteryMask = 0;       // seven low bits, one per Category
    Intent intent = Intent::FOLLOW;
    bool helperScene = false;
    bool pigMoving = false;
    bool stationary = false;
    bool visible = false;
    bool navigating = false;
    bool transmitting = false;
    bool channelClear = false;
    bool eventHold = false;
    bool sessionActive = false;
};

struct Decision {
    Action action = Action::FOLLOW;
    Routine routine = Routine::NONE;
    uint8_t phase = 0;
    uint8_t bondTier = 0;
    bool overrideBase = false;
};

struct Completion {
    Reward reward = Reward::NONE;
    Routine routine = Routine::NONE;
    Category category = Category::CURIOSITY;
    uint8_t room = 0;
    uint8_t xp = 0;
    int8_t momentum = 0;
    const char* label = nullptr;
};

// Reset volatile routine state. Persistent dossier/mastery state is supplied in
// Context on every update and therefore cannot be accidentally erased here.
void reset(uint32_t nowMs, uint32_t seed = 0xC47B0A7Du);

// Advance the high-level director. observedAction must be the activity the
// low-level PancettaCat state machine actually rendered on the previous frame.
Decision update(const Context& context, Action observedAction);

// Fixed-size event queue; false means no completed routine is waiting.
bool consumeCompletion(Completion& out);

// Deterministic simulator/test hook. The routine still obeys intent, movement,
// visibility, bond, and channel gates, and rewards only after observed playback.
bool forceRoutine(Routine routine, uint32_t nowMs);

uint8_t bondTierFor(uint8_t memories, uint8_t dossierMask,
                    uint8_t masteryMask, uint8_t streak,
                    uint8_t goalProgress);
uint8_t popcount8(uint8_t value);
const char* routineName(Routine routine);
const char* categoryName(Category category);

}  // namespace CatBehavior
