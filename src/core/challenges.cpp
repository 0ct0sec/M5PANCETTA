/**
 * Session Challenges — 3-slot system adapted from porkchop
 *
 * ==[ CHALLENGE ENGINE ]== 15 templates, Easy/Med/Hard.
 * Level-scaled targets: base * (1.0 + level * 0.02).
 * Session-scoped. Resets on boot. NO NVS.
 *
 * Uses HAL for random, XP, sound, haptic — fully testable on host.
 * Requires HalGlobal::set() at boot or explicit HAL* in generate().
 */

#include "challenges.h"
#include "achievements.h"
#include "item_drops.h"
#include "../audio/sfx.h"
#include "../hal/hal_interface.h"
#include "../ui/progression_text.h"

namespace Challenges {

static HAL* _hal = nullptr;
static ActiveChallenge slots[3];
static bool sweepBonusPending = false;
static uint8_t lastRoomMask = 0;

// ==[ TEMPLATE POOL ]== base values before level scaling
struct ChallengeTemplate {
    ChallengeType type;
    ChallengeDifficulty difficulty;
    uint16_t baseTarget;
    uint16_t baseXP;
    const char* name;
};

static const ChallengeTemplate EASY_POOL[] = {
    { ChallengeType::CAPTURE_PMKID,  ChallengeDifficulty::EASY, 3,    25, "PMKID HUNT" },
    { ChallengeType::WALK_STEPS,     ChallengeDifficulty::EASY, 1000, 20, "TR0TT3R" },
    { ChallengeType::WITNESS_DUMPS,  ChallengeDifficulty::EASY, 2,    20, "4N4LYST" },
    { ChallengeType::DEAUTH_COUNT,   ChallengeDifficulty::EASY, 5,    20, "MUDB4LL" },
    { ChallengeType::DETECT_TRACKER_TYPES, ChallengeDifficulty::EASY, 3, 25, "T4G HUNT3R" },
    { ChallengeType::WITNESS_THREATS, ChallengeDifficulty::EASY, 2,   20, "W4TCHTOW3R" },
    { ChallengeType::CLOSE_CASES,     ChallengeDifficulty::EASY, 2,   25, "C4S3 CL0S3R" },
};
static constexpr int EASY_COUNT = sizeof(EASY_POOL) / sizeof(EASY_POOL[0]);

static const ChallengeTemplate MEDIUM_POOL[] = {
    { ChallengeType::CAPTURE_HS,     ChallengeDifficulty::MEDIUM, 2,    50, "SH4K3D0WN" },
    { ChallengeType::VISIT_ROOMS,    ChallengeDifficulty::MEDIUM, 3,    40, "R04M3R" },
    { ChallengeType::CHAIN_X3,       ChallengeDifficulty::MEDIUM, 1,    45, "CH41N G4NG" },
    { ChallengeType::SCAN_MINUTES,   ChallengeDifficulty::MEDIUM, 5,    40, "SC4NN3R" },
};
static constexpr int MEDIUM_COUNT = sizeof(MEDIUM_POOL) / sizeof(MEDIUM_POOL[0]);

static const ChallengeTemplate HARD_POOL[] = {
    { ChallengeType::CHAIN_X5,       ChallengeDifficulty::HARD, 1,    100, "CH41N L0RD" },
    { ChallengeType::BIG_WALK,       ChallengeDifficulty::HARD, 5000, 90,  "L0NG H4UL" },
    { ChallengeType::MULTI_HS,       ChallengeDifficulty::HARD, 3,    120, "H4ND C0LL3CT0R" },
    { ChallengeType::FULL_CIRCUIT,   ChallengeDifficulty::HARD, 1,    100, "FULL C1RCU1T" },
};
static constexpr int HARD_COUNT = sizeof(HARD_POOL) / sizeof(HARD_POOL[0]);

// ==[ LEVEL SCALING ]== target * (1.0 + level * 0.02)
static uint16_t scaleTarget(uint16_t base, uint8_t level) {
    float mult = 1.0f + level * 0.02f;
    return (uint16_t)(base * mult);
}

static uint16_t targetFor(const ChallengeTemplate& challenge, uint8_t level) {
    switch (challenge.type) {
        case ChallengeType::CHAIN_X3:
        case ChallengeType::CHAIN_X5:
        case ChallengeType::FULL_CIRCUIT:
            return 1;  // event gates. scaling the display counter is fake difficulty.
        case ChallengeType::CLOSE_CASES:
            return challenge.baseTarget;  // encounter timers already set the pace.
        case ChallengeType::VISIT_ROOMS:
            return min((uint16_t)5, scaleTarget(challenge.baseTarget, level));
        default:
            return scaleTarget(challenge.baseTarget, level);
    }
}

static uint16_t scaleXP(uint16_t base, uint8_t level) {
    float mult = 1.0f + level * 0.015f;
    return (uint16_t)(base * mult);
}

void generate(uint8_t level, HAL* hal) {
    _hal = hal ? hal : HalGlobal::get();

    // Pick one from each pool
    uint8_t easyIdx  = _hal ? _hal->random(EASY_COUNT) : 0;
    uint8_t medIdx   = _hal ? _hal->random(MEDIUM_COUNT) : 0;
    uint8_t hardIdx  = _hal ? _hal->random(HARD_COUNT) : 0;

    auto fillSlot = [&](int slot, const ChallengeTemplate& t) {
        slots[slot].type = t.type;
        slots[slot].difficulty = t.difficulty;
        slots[slot].target = targetFor(t, level);
        slots[slot].progress = 0;
        slots[slot].xpReward = scaleXP(t.baseXP, level);
        slots[slot].completed = false;
        slots[slot].name = t.name;
    };

    fillSlot(0, EASY_POOL[easyIdx]);
    fillSlot(1, MEDIUM_POOL[medIdx]);
    fillSlot(2, HARD_POOL[hardIdx]);

    sweepBonusPending = false;
    lastRoomMask = 0;
}

void reset() {
    for (int i = 0; i < 3; i++) {
        slots[i] = {};
    }
    sweepBonusPending = false;
    lastRoomMask = 0;
}

// ==[ COMPLETION CHECK ]== shared logic
static void checkCompletion(int idx) {
    if (slots[idx].completed || slots[idx].progress < slots[idx].target) return;
    if (!_hal || !_hal->isSessionActive()) return;

    slots[idx].completed = true;

    _hal->addXP(slots[idx].xpReward);
    _hal->playSound((uint8_t)SFX::CHALLENGE_COMPLETE);
    _hal->hapticBuzz();
    ItemDrops::awardGuaranteed(ItemDrops::ItemDropSource::CHALLENGE,
                               (uint8_t)slots[idx].difficulty + 1);
    if (_hal->terminalIsVisible()) {
        const char* diff = "EASY";
        if (slots[idx].difficulty == ChallengeDifficulty::MEDIUM) diff = "MED";
        if (slots[idx].difficulty == ChallengeDifficulty::HARD) diff = "HARD";
        char gain[24];
        ProgressionText::formatGainAmount(gain, sizeof(gain), slots[idx].xpReward);
        _hal->terminalPush("CHALLENGE [%s] %s %s", diff, slots[idx].name, gain);
    }

    // check sweep
    if (isAllComplete() && !sweepBonusPending) {
        sweepBonusPending = true;
        _hal->addXP(50);
        _hal->playSound((uint8_t)SFX::CHALLENGE_SWEEP);
        Achievements::tryUnlock(Achievement::SWEEP);
        ItemDrops::awardGuaranteed(ItemDrops::ItemDropSource::SWEEP, 8);

        if (_hal->terminalIsVisible()) {
            char gain[24];
            ProgressionText::formatGainAmount(gain, sizeof(gain), 50, true);
            _hal->terminalPush("!! CL34N SW33P !! %s", gain);
        }
    }
}

void onSessionActivated() {
    for (int i = 0; i < 3; i++) checkCompletion(i);
}

// ==[ PROGRESS HOOKS ]==

static void checkFullCircuitOnCapture() {
    uint8_t rooms = 0;
    uint8_t b = lastRoomMask;
    while (b) { rooms += b & 1; b >>= 1; }
    if (rooms < 6) return;

    uint8_t captures = _hal ? (_hal->getSessionPMKIDCount() + _hal->getSessionHSCount()) : 0;
    if (captures < 2) return;

    for (int i = 0; i < 3; i++) {
        if (!slots[i].completed && slots[i].type == ChallengeType::FULL_CIRCUIT) {
            slots[i].progress = 1;
            checkCompletion(i);
        }
    }
}

void onPMKIDCapture() {
    for (int i = 0; i < 3; i++) {
        if (!slots[i].completed && slots[i].type == ChallengeType::CAPTURE_PMKID) {
            slots[i].progress++;
            checkCompletion(i);
        }
    }
    checkFullCircuitOnCapture();
}

void onHandshakeCapture() {
    for (int i = 0; i < 3; i++) {
        if (!slots[i].completed) {
            if (slots[i].type == ChallengeType::CAPTURE_HS ||
                slots[i].type == ChallengeType::MULTI_HS) {
                slots[i].progress++;
                checkCompletion(i);
            }
        }
    }
    checkFullCircuitOnCapture();
}

void onStepsUpdate(uint32_t totalSteps) {
    for (int i = 0; i < 3; i++) {
        if (!slots[i].completed) {
            if (slots[i].type == ChallengeType::WALK_STEPS ||
                slots[i].type == ChallengeType::BIG_WALK) {
                slots[i].progress = (uint16_t)min((uint32_t)65535, totalSteps);
                checkCompletion(i);
            }
        }
    }
}

void onDumpWitnessed(uint8_t totalDumps) {
    for (int i = 0; i < 3; i++) {
        if (!slots[i].completed && slots[i].type == ChallengeType::WITNESS_DUMPS) {
            slots[i].progress = totalDumps;
            checkCompletion(i);
        }
    }
}

void onRoomVisited(uint8_t roomBitmask) {
    lastRoomMask = roomBitmask;
    uint8_t rooms = 0;
    uint8_t b = roomBitmask;
    while (b) { rooms += b & 1; b >>= 1; }

    for (int i = 0; i < 3; i++) {
        if (!slots[i].completed) {
            if (slots[i].type == ChallengeType::VISIT_ROOMS) {
                slots[i].progress = rooms;
                checkCompletion(i);
            }
            if (slots[i].type == ChallengeType::FULL_CIRCUIT && rooms >= 6) {
                uint8_t captures = _hal ? (_hal->getSessionPMKIDCount() + _hal->getSessionHSCount()) : 0;
                if (captures >= 2) {
                    slots[i].progress = 1;
                    checkCompletion(i);
                }
            }
        }
    }
}

void onChainReached(uint8_t chainLen) {
    for (int i = 0; i < 3; i++) {
        if (!slots[i].completed) {
            if (slots[i].type == ChallengeType::CHAIN_X3 && chainLen >= 3) {
                slots[i].progress = slots[i].target;
                checkCompletion(i);
            }
            if (slots[i].type == ChallengeType::CHAIN_X5 && chainLen >= 5) {
                slots[i].progress = slots[i].target;
                checkCompletion(i);
            }
        }
    }
}

void onScanMinute(uint16_t totalMinutes) {
    for (int i = 0; i < 3; i++) {
        if (!slots[i].completed && slots[i].type == ChallengeType::SCAN_MINUTES) {
            slots[i].progress = totalMinutes;
            checkCompletion(i);
        }
    }
}

void onDeauth() {
    for (int i = 0; i < 3; i++) {
        if (!slots[i].completed && slots[i].type == ChallengeType::DEAUTH_COUNT) {
            slots[i].progress++;
            checkCompletion(i);
        }
    }
}

void onTrackerTypeDetected(uint32_t typesBitmask) {
    uint8_t types = 0;
    uint32_t b = typesBitmask;
    while (b) { types += b & 1; b >>= 1; }
    for (int i = 0; i < 3; i++) {
        if (!slots[i].completed && slots[i].type == ChallengeType::DETECT_TRACKER_TYPES) {
            slots[i].progress = types;
            checkCompletion(i);
        }
    }
}

void onThreatWitnessed() {
    for (int i = 0; i < 3; i++) {
        if (!slots[i].completed && slots[i].type == ChallengeType::WITNESS_THREATS) {
            slots[i].progress++;
            checkCompletion(i);
        }
    }
}

void onCaseClosed(uint8_t totalCases) {
    for (int i = 0; i < 3; i++) {
        if (!slots[i].completed && slots[i].type == ChallengeType::CLOSE_CASES) {
            slots[i].progress = totalCases;
            checkCompletion(i);
        }
    }
}

// ==[ QUERIES ]==

uint8_t getCompletedCount() {
    uint8_t c = 0;
    for (int i = 0; i < 3; i++) if (slots[i].completed) c++;
    return c;
}

bool isAllComplete() {
    return slots[0].completed && slots[1].completed && slots[2].completed;
}

const ActiveChallenge* getChallenge(uint8_t idx) {
    if (idx >= 3) return nullptr;
    return &slots[idx];
}

bool hasSweepBonus() { return sweepBonusPending; }
void consumeSweepBonus() { sweepBonusPending = false; }

}  // namespace Challenges
