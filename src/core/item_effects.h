/**
 * Item Effects - what burning a piece of evidence actually costs and buys.
 *
 * ==[ THE LOCKER OPENS ]== 42 trinkets, 42 consequences.
 * Every effect routes through a lever that already exists: XP, mood momentum,
 * the luck argument ItemDrops::award() already takes, the hunt effectiveness
 * multiplier, or the mood decay interval. Nothing here invents a reward the
 * firmware cannot pay.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

struct HAL;

namespace ItemEffects {

// The four gameplay levers, chosen because each one maps onto a value the
// firmware already reads every frame.
enum class Buff : uint8_t {
    NONE = 0,
    LUCK,    // adds to the luck argument on the next item rolls
    FOCUS,   // multiplies hunt effectiveness on top of mood
    CALM,    // stretches the mood decay interval
    COUNT,
};

struct Effect {
    int8_t momentum;     // instant mood delta
    uint8_t xp;          // instant XP, paid only while a session is live
    Buff buff;           // gameplay lever, NONE for instant-only items
    uint8_t magnitude;   // luck points / focus percent / calm percent
    uint16_t durationS;  // buff lifetime in seconds; 0 for instant-only
    const char* verdict; // what just happened, in Pancetta's voice
};

// Evidence is not infinite. Three burns per session keeps consumption a
// decision instead of a checklist.
constexpr uint8_t SESSION_BURN_LIMIT = 3;

void init(HAL* hal = nullptr);
void update();  // expires buffs
void save();

const Effect* effectFor(uint8_t itemId);

bool wasConsumed(uint8_t itemId);
uint8_t burnsRemaining();

// Applies the effect and writes the verdict line. An item is consumable when it
// is collected, unspent this session, and the burn budget still has room;
// returns false otherwise, with verdictOut explaining which rule refused.
bool consume(uint8_t itemId, char* verdictOut, size_t outLen);

// A fresh session restores the burn budget and clears the spent marks.
void onSessionStart();

// ==[ LIVE BUFF QUERIES ]== read by the systems the buffs actually steer.
uint8_t luckBonus();          // added to ItemDrops::award() luck
float focusMultiplier();      // 1.0 when no FOCUS buff is live
uint32_t calmDecayBonusMs();  // folded into Mood::getDecayInterval()
const char* buffLabel(Buff buff);

}  // namespace ItemEffects
