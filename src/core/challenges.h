/**
 * Session Challenges — 3 per boot, level-scaled targets
 *
 * ==[ CHALLENGE SYSTEM ]== adapted from porkchop's 3-slot pattern.
 * Easy/Medium/Hard. 15 templates. Level-scaled targets.
 * Session-scoped (resets on boot). NO NVS — ephemeral.
 */
#pragma once
#include <Arduino.h>

// Forward declare HAL for dependency injection
struct HAL;

// ==[ CHALLENGE TYPE ]== what to track
enum class ChallengeType : uint8_t {
    CAPTURE_PMKID,    // capture N PMKIDs
    CAPTURE_HS,       // capture N handshakes
    WALK_STEPS,       // walk N steps
    WITNESS_DUMPS,    // view N terminal dumps
    VISIT_ROOMS,      // visit N different rooms
    CHAIN_X3,         // reach chain x3
    CHAIN_X5,         // reach chain x5
    SCAN_MINUTES,     // hunt for N minutes
    FULL_CIRCUIT,     // visit all 6 rooms + land 2 captures
    BIG_WALK,         // walk 5K+ steps
    MULTI_HS,         // capture 3+ handshakes
    DEAUTH_COUNT,     // send N deauths
    DETECT_TRACKER_TYPES, // detect N different BLE tracker types
    WITNESS_THREATS,      // witness N defense threat events
    CLOSE_CASES,          // resolve N roaming NPC cases
    TYPE_COUNT
};

// ==[ DIFFICULTY ]==
enum class ChallengeDifficulty : uint8_t {
    EASY,
    MEDIUM,
    HARD
};

// ==[ ACTIVE CHALLENGE ]== runtime state
struct ActiveChallenge {
    ChallengeType type;
    ChallengeDifficulty difficulty;
    uint16_t target;          // level-scaled target
    uint16_t progress;        // current progress
    uint16_t xpReward;        // level-scaled XP reward
    bool completed;           // done this session
    const char* name;         // display name
};

namespace Challenges {
    void generate(uint8_t level, HAL* hal = nullptr);  // create 3 challenges (HAL for DI, nullptr = use global)
    void reset();                      // clear all (boot)
    void onSessionActivated();         // settle any passive progress now eligible for rewards

    // progress hooks (call from mood/hunt/etc)
    void onPMKIDCapture();
    void onHandshakeCapture();
    void onStepsUpdate(uint32_t totalSteps);
    void onDumpWitnessed(uint8_t totalDumps);
    void onRoomVisited(uint8_t roomBitmask);
    void onChainReached(uint8_t chainLen);
    void onScanMinute(uint16_t totalMinutes);
    void onDeauth();
    void onTrackerTypeDetected(uint32_t typesBitmask);  // bitmask of ThreatType enum
    void onThreatWitnessed();
    void onCaseClosed(uint8_t totalCases);

    // queries
    uint8_t getCompletedCount();      // 0-3
    bool isAllComplete();             // CLEAN SWEEP?
    const ActiveChallenge* getChallenge(uint8_t idx);  // 0-2

    // sweep bonus flag (consumed once)
    bool hasSweepBonus();
    void consumeSweepBonus();
}
