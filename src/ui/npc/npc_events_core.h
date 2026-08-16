#pragma once

#include <stddef.h>
#include <stdint.h>

namespace NpcEventsCore {

constexpr uint8_t DIALOGUE_VARIANTS = 4;

enum class Character : uint8_t {
    K_HORSE = 0,
    WISE_PIG,
    COW,
    DR_OCULUS,
    RASTA_HOOLIGAN,
    BARMAN,
    COUNT,
};

constexpr uint8_t CHARACTER_COUNT = static_cast<uint8_t>(Character::COUNT);
static_assert(CHARACTER_COUNT < 32, "cast mask supports at most 31 characters");
static_assert(CHARACTER_COUNT * 2 <= 32,
              "choice ledger stores two bits per character");
constexpr uint32_t ALL_CHARACTER_MASK = (1u << CHARACTER_COUNT) - 1u;
constexpr uint8_t NO_CHOICE = 0xFF;

constexpr uint32_t characterBit(Character character) {
    return 1u << static_cast<uint8_t>(character);
}

constexpr uint32_t markCharacterClosed(uint32_t mask, Character character) {
    return mask | characterBit(character);
}

constexpr bool hasClosedWholeCast(uint32_t mask) {
    return (mask & ALL_CHARACTER_MASK) == ALL_CHARACTER_MASK;
}

constexpr bool shouldOfferCoda(uint32_t mask, bool codaSeen) {
    return hasClosedWholeCast(mask) && !codaSeen;
}

// A choice that ends the case files immediately. Anything else is a 1-based
// index into Encounter::nodes. Zero is the terminal value on purpose: a choice
// that forgets to name a follow-up closes the file instead of silently
// jumping to node 0.
constexpr uint8_t CASE_CLOSED = 0;

// Follow-up prompts carry two variants rather than four. The root sets the
// scene and earns the rotation; by the second question the operator is inside
// one specific decision and wants the thread, not the scenery.
constexpr uint8_t FOLLOWUP_VARIANTS = 2;

// Deepest chain the walker will follow before it forces the file shut. Guards
// against a content typo turning into an unclosable case.
constexpr uint8_t MAX_CASE_DEPTH = 4;

struct Choice {
    const char* label;
    const char* thread;
    const char* reply[DIALOGUE_VARIANTS];
    uint8_t xp;
    int8_t momentum;
    uint8_t lootChance;
    uint8_t reinforcement;
    int8_t threadImpact;
    uint8_t nextNode;  // CASE_CLOSED, or 1-based index into Encounter::nodes
};

// One follow-up beat. The prompt reports what the witness did with the last
// decision; the choices are Pancetta pushing the case one step further.
struct CaseNode {
    const char* prompt[FOLLOWUP_VARIANTS];
    Choice choices[3];
};

struct Encounter {
    Character character;
    const char* name;
    const char* tag;
    uint8_t roomMask;
    uint8_t minLevel;
    uint8_t minKHorse;
    const char* caption[DIALOGUE_VARIANTS];
    const char* opener[DIALOGUE_VARIANTS];
    Choice choices[3];
    const CaseNode* nodes;  // follow-up beats; null for a flat one-shot case
    uint8_t nodeCount;
};

// Resolves a choice's follow-up. Returns null when the choice closes the case
// or names a node outside the encounter's table.
const CaseNode* followUp(const Encounter& encounter, const Choice& choice);

// Reply text for a variant, falling back to variant 0. Root choices carry the
// full four-line rotation; follow-up choices only author FOLLOWUP_VARIANTS and
// leave the rest null, so the render path must never index them blind.
const char* replyText(const Choice& choice, uint8_t variant);

// Prompt text for a node variant, with the same fallback contract.
const char* promptText(const CaseNode& node, uint8_t variant);

// Longest chain of follow-ups reachable from the encounter's root, counting the
// root beat itself. A flat case is depth 1.
uint8_t caseDepth(const Encounter& encounter);

// Human-facing beat number for a saved 1-based node index. Root is beat 1;
// zero means that node cannot be reached from this file's root.
uint8_t beatForNode(const Encounter& encounter, uint8_t node);

// True when every declared node is reachable from the root and no chain
// exceeds MAX_CASE_DEPTH. Content invariant, proved on host.
bool caseTreeIsSound(const Encounter& encounter);

size_t count();
const Encounter& get(size_t index);

// Weighted picker: unseen files get 3x weight and the last character is
// suppressed when another eligible character exists. Returns -1 when empty.
int pick(uint32_t roll, uint8_t room, uint8_t level, uint8_t kHorseLevel,
         int lastIndex, uint32_t seenMask);

// First contact always lands. Later arrivals climb toward a 92% pity ceiling.
uint8_t caseArrivalChance(bool hasOpenedCase, uint8_t misses);

// Signed delta keeps short deadlines honest across millis() rollover.
bool deadlineReached(uint32_t now, uint32_t deadline);

// Maps a random roll onto the dialogue pool without repeating the previous
// line when it is valid. Used by the firmware and proved on host.
uint8_t pickVariant(uint32_t roll, uint8_t previous);

// Two-bit persisted relationship ledger. Zero means no prior decision;
// encoded values 1..3 map to the three choices.
uint32_t rememberChoice(uint32_t ledger, Character character, uint8_t choice);
uint8_t recallChoice(uint32_t ledger, Character character);
int8_t scoreChoiceLedger(uint32_t ledger, uint32_t closedMask);

enum class CaseEnding : uint8_t {
    LIVE_WIRE = 0,
    OPEN_CIRCUIT,
    CLEAN_WIRE,
};

CaseEnding endingFromScore(int8_t score);
const char* endingTitle(CaseEnding ending);
const char* endingText(CaseEnding ending);

}  // namespace NpcEventsCore
