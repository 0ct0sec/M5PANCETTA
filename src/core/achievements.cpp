/**
 * Achievements — bitfield trophy case
 *
 * ==[ TROPHY CASE ]== adapted from porkchop's 64-bit bitfield.
 * Low 32 bits use legacy NVS key 'achs'; extended trophies use 'achs_hi'.
 *
 * Uses HAL for clock, storage, and feedback — fully testable on host.
 * Requires HalGlobal::set() at boot or explicit HAL* in init().
 */

#include "achievements.h"
#include "../hal/hal_interface.h"

namespace Achievements {

static_assert((uint8_t)Achievement::ACH_COUNT <= 64, "Achievement enum currently supports up to 64 slots");

static HAL* _hal = nullptr;
static uint32_t bitfieldLo = 0;
static uint32_t bitfieldHi = 0;
static bool dirty = false;

static inline uint8_t lowOrHiIndex(uint8_t bit) {
    return (bit < ACH_LOW_BITS) ? 0 : 1;
}

static inline uint8_t bitOffset(uint8_t bit) {
    return (bit < ACH_LOW_BITS) ? bit : (bit - ACH_LOW_BITS);
}

// ==[ CELEBRATION QUEUE ]== every trophy can wait, 600ms cooldown
// One spare slot keeps the circular-buffer full/empty states distinct. Unlocks
// are one-shot, so ACH_COUNT + 1 means no legal cascade gets dropped.
static constexpr uint8_t CELEB_QUEUE_SIZE = (uint8_t)Achievement::ACH_COUNT + 1;
static Achievement celebrationQueue[CELEB_QUEUE_SIZE];
static uint8_t celebQHead = 0;
static uint8_t celebQTail = 0;
static uint32_t lastCelebTime = 0;
static constexpr uint32_t CELEB_COOLDOWN = 600;

// ==[ ACHIEVEMENT INFO ]==
struct AchInfo {
    const char* name;
    const char* desc;
};

static const AchInfo ACH_INFO[] = {
    {"F1RST_BL00D",    "First capture. The case finally has a body."},
    {"HUNT3R",         "File 10 lifetime PMKIDs. Cold calls add up."},
    {"H4NDSH4K3R",     "File 10 lifetime handshakes. Four frames, no alibi."},
    {"C3NTUR10N",      "File 100 total captures. The locker wants zoning."},
    {"CH41N_x3",       "Reach capture chain x3. Three witnesses agree."},
    {"CH41N_x5",       "Reach capture chain x5. Now it is a conspiracy."},
    {"CH41N_x10",      "Reach capture chain x10. The wire signs under protest."},
    {"T0UCH_GR4SS",    "Walk 1K steps in one session. Shoe leather is evidence."},
    {"M4R4TH0N",       "Walk 10K steps in one session. The beat keeps moving."},
    {"ULTR4",          "Walk 30K steps in one session. The shoes retain counsel."},
    {"FULL_C1RCU1T",   "Visit every room. Every wall gave a statement."},
    {"4N4LYST",        "View 10 terminal dumps. Ten screens, one long night."},
    {"ST4K30UT",       "Hold one station for 5 minutes. Silence bills hourly."},
    {"D3D1C4T3D",      "Reach a 10-session streak. Habit gets paperwork."},
    {"L0Y4L",          "Reach a 25-session streak. The desk leaves your file open."},
    {"0BS3SS3D",       "Reach a 50-session streak. The chair knows your shape."},
    {"S0_CL0S3",       "Log 5 near-misses in one session. Five suspects walked."},
    {"P3RS1ST3NT",     "Log 10 near-misses in one session. That is a pattern."},
    {"SH0AT",          "Reach level 7. The shoat gets a desk."},
    {"B0AR",           "Reach level 14. The badge gains weight."},
    {"TUSK3R",         "Reach level 21. Tusk marks enter the file."},
    {"W4RTH0G",        "Reach level 28. Warthog jurisdiction."},
    {"R4Z0RB4CK",      "Reach level 35. Razorback, no appeal."},
    {"3LD3R",          "Reach level 42. Elder file. No higher shelf."},
    {"N1GHT_0WL",      "Clock in before 05:00. Dawn catches you working."},
    {"CLUTCH",         "Capture at 10% battery or less. The lights blink first."},
    {"SW33P",          "Complete all 3 challenges. Clean desk, dirty night."},
    {"G04L_STR34K",    "Meet 5 goals in a row. The calendar confesses."},
    {"C4RT0GR4PH3R",   "Log 100 nets in one wardrive. The map needs another page."},
    {"GR1D_W4LK3R",    "Log 1000 lifetime wardrive nets. The city runs out of aliases."},
    {"URB4N_JUNGL3",   "Capture in a 50+ crowd. Find one confession in the riot."},
    {"L0N3_W0LF",      "Capture while deserted. One signal. No witnesses."},
    {"P1G_34RS",       "Catalog 25 BLE devices in one run. The air has pockets."},
    {"T4G_C0LL3CT0R",  "Classify 5 tracker types in one run. Five tails line up."},
    {"T41L_BR34K3R",   "Catch a following tracker. The shadow moved when you did."},
    {"XB4ND_GUMSH03",  "Link BLE and WiFi evidence. Two bands, same suspect."},
    {"F1RST_C4S3",     "Close one roaming case. Somebody finally signed."},
    {"R0GU3S_G4LL3RY", "Close the whole cast in one session. Every portrait gets a file."},
    {"L1V1NG_P1LL0W",  "Let Pig sleep on Pancetta's head. Trust has weight."},
    {"P1G_R3M3MB3RS",  "Remember every one of Pig's seven living habits."},
};
static_assert(sizeof(ACH_INFO) / sizeof(ACH_INFO[0]) == (uint8_t)Achievement::ACH_COUNT,
              "ACH_INFO must match Achievement enum");

void init(HAL* hal) {
    _hal = hal ? hal : HalGlobal::get();

    // Reset queue state
    celebQHead = 0;
    celebQTail = 0;
    lastCelebTime = 0;
    dirty = false;

    bitfieldLo = _hal ? _hal->storageGetUInt("sirloin", "achs", 0) : 0;
    bitfieldHi = _hal ? _hal->storageGetUInt("sirloin", "achs_hi", 0) : 0;
}

bool has(Achievement ach) {
    uint8_t bit = (uint8_t)ach;
    if (bit >= (uint8_t)Achievement::ACH_COUNT) return false;
    const uint32_t word = (lowOrHiIndex(bit) == 0) ? bitfieldLo : bitfieldHi;
    return (word >> bitOffset(bit)) & 1U;
}

bool tryUnlock(Achievement ach) {
    if (has(ach)) return false;
    uint8_t bit = (uint8_t)ach;
    if (bit >= (uint8_t)Achievement::ACH_COUNT) return false;

    if (lowOrHiIndex(bit) == 0) bitfieldLo |= (1u << bitOffset(bit));
    else bitfieldHi |= (1u << bitOffset(bit));
    dirty = true;

    // queue celebration (circular buffer)
    uint8_t nextTail = (celebQTail + 1) % CELEB_QUEUE_SIZE;
    if (nextTail != celebQHead) {
        celebrationQueue[celebQTail] = ach;
        celebQTail = nextTail;
    }

    // Terminal push via HAL
    if (_hal && _hal->terminalIsVisible()) {
        _hal->terminalPush("!! %s UNLOCKED !!", getName(ach));
    }

    return true;
}

uint8_t getUnlockedCount() {
    uint8_t count = 0;
    uint32_t b = bitfieldLo;
    while (b) { count += b & 1; b >>= 1; }
    constexpr uint8_t highBits =
        (uint8_t)Achievement::ACH_COUNT > ACH_LOW_BITS
            ? (uint8_t)Achievement::ACH_COUNT - ACH_LOW_BITS
            : 0;
    constexpr uint32_t highMask = highBits >= 32
        ? 0xFFFFFFFFu
        : (highBits == 0 ? 0u : ((1u << highBits) - 1u));
    // Preserve unknown high bits for downgrade/upgrade compatibility, but do
    // not let them inflate the current firmware's visible trophy count.
    b = bitfieldHi & highMask;
    while (b) { count += b & 1; b >>= 1; }
    return count;
}

uint32_t getBitfield() { return bitfieldLo; }
uint32_t getBitfieldHi() { return bitfieldHi; }

const char* getName(Achievement ach) {
    uint8_t idx = (uint8_t)ach;
    if (idx >= (uint8_t)Achievement::ACH_COUNT) return "???";
    return ACH_INFO[idx].name;
}

const char* getDescription(Achievement ach) {
    uint8_t idx = (uint8_t)ach;
    if (idx >= (uint8_t)Achievement::ACH_COUNT) return "???";
    return ACH_INFO[idx].desc;
}

bool hasPendingCelebration() {
    if (celebQHead == celebQTail) return false;
    uint32_t now = _hal ? _hal->millis() : 0;
    return (now - lastCelebTime >= CELEB_COOLDOWN);
}

Achievement popPendingCelebration() {
    Achievement ach = celebrationQueue[celebQHead];
    celebQHead = (celebQHead + 1) % CELEB_QUEUE_SIZE;
    uint32_t now = _hal ? _hal->millis() : 0;
    lastCelebTime = now;
    return ach;
}

void save() {
    if (!dirty) return;
    if (_hal) {
        _hal->storagePutUInt("sirloin", "achs", bitfieldLo);
        _hal->storagePutUInt("sirloin", "achs_hi", bitfieldHi);
    }
    dirty = false;
}

bool needsSave() { return dirty; }

}  // namespace Achievements
