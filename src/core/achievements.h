/**
 * Achievements — bitfield unlockables
 *
 * ==[ TROPHY CASE ]== persistent unlockables.
 * Low 32 bits stay in legacy NVS key 'achs'; extended bits live in 'achs_hi'.
 */
#pragma once
#include <Arduino.h>

// Forward declare HAL for dependency injection
struct HAL;

// ==[ ACHIEVEMENT ENUM ]== bit positions 0-63
enum class Achievement : uint8_t {
    // hunt milestones
    FIRST_BLOOD     = 0,   // 1st capture ever
    HUNTER          = 1,   // 10 lifetime PMKIDs
    HANDSHAKER      = 2,   // 10 lifetime handshakes
    CENTURION       = 3,   // 100 total captures

    // chain achievements
    CHAIN_X3        = 4,   // first x3 chain
    CHAIN_X5        = 5,   // first x5 chain
    CHAIN_X10       = 6,   // legendary x10 chain

    // walk achievements
    TOUCH_GRASS     = 7,   // 1K steps in a session
    MARATHON        = 8,   // 10K steps in a session
    ULTRA           = 9,   // 30K steps in a session

    // room achievements
    FULL_CIRCUIT    = 10,  // visit all 6 rooms
    ANALYST         = 11,  // witness 10 terminal dumps
    STAKEOUT        = 12,  // spend 5min at one station

    // streak achievements
    DEDICATED       = 13,  // 10 session streak
    LOYAL           = 14,  // 25 session streak
    OBSESSED        = 15,  // 50 session streak

    // near-miss
    SO_CLOSE        = 16,  // 5 near-misses in one session
    PERSISTENT      = 17,  // 10 near-misses in one session

    // level milestones
    RANK_SHOAT      = 18,  // reach level 7
    RANK_BOAR       = 19,  // reach level 14
    RANK_TUSKER     = 20,  // reach level 21
    RANK_WARTHOG    = 21,  // reach level 28
    RANK_RAZORBACK  = 22,  // reach level 35
    RANK_ELDER      = 23,  // reach level 42

    // special
    NIGHT_OWL       = 24,  // session after midnight
    CLUTCH          = 25,  // capture at 10% battery or less

    // challenge achievements
    SWEEP           = 26,  // complete all 3 session challenges
    GOAL_STREAK_5   = 27,  // 5 consecutive goals met

    // wardrive
    CARTOGRAPHER    = 28,  // 100 unique networks in one wardrive session
    GRID_WALKER     = 29,  // 1000 lifetime wardrive networks

    // crowd density
    URBAN_JUNGLE    = 30,  // capture in 50+ estimated population
    LONE_WOLF       = 31,  // capture in <5 estimated population (deserted)

    // BLE / cross-band casework
    PIG_EARS        = 32,  // catalog 25 BLE devices in one run
    TAG_COLLECTOR   = 33,  // classify 5 distinct BLE tracker types in one run
    TAIL_BREAKER    = 34,  // catch a tracker following us
    XBAND_GUMSHOE   = 35,  // correlate BLE and WiFi into one case

    // roaming NPC casework
    FIRST_CASE      = 36,  // close one encounter case
    ROGUES_GALLERY  = 37,  // close a case on every encounter character

    // companion-cat memories
    LIVING_PILLOW   = 38,  // Pig sleeps on Pancetta's head
    PIG_REMEMBERS   = 39,  // observe every durable Pig memory

    ACH_COUNT       = 40   // total defined
};

constexpr uint8_t ACH_LOW_BITS = 32;

namespace Achievements {
    void init(HAL* hal = nullptr);                  // load from NVS (HAL for DI, nullptr = use global)
    bool has(Achievement ach);                      // check if unlocked
    bool tryUnlock(Achievement ach);                // unlock if not already — returns true if NEW
    uint8_t getUnlockedCount();                     // total unlocked
    uint32_t getBitfield();                         // raw low 32-bit bitfield
    uint32_t getBitfieldHi();                       // raw high 32-bit bitfield

    // achievement info
    const char* getName(Achievement ach);           // leet-speak name
    const char* getDescription(Achievement ach);    // how to earn

    // celebration queue (600ms cooldown between celebrations)
    bool hasPendingCelebration();                   // check queue
    Achievement popPendingCelebration();            // consume next

    // persistence
    void save();                                    // flush to NVS (called by Config::save)
    bool needsSave();                               // pending deferred persistence
}
