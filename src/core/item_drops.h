/**
 * Item Drops - tiny persistent trinket ledger.
 *
 * ==[ LOOT PANTRY ]== 42 sheet sprites, 64-bit collect mask.
 * Queue is session-only. Collection survives in NVS.
 */
#pragma once

#include <stdint.h>

struct HAL;

namespace ItemDrops {

enum class ItemRarity : uint8_t {
    COMMON = 0,
    UNCOMMON,
    RARE,
    LEGENDARY,
};

enum class ItemDropSource : uint8_t {
    CAPTURE = 0,
    HANDSHAKE,
    CHALLENGE,
    SWEEP,
    LEVEL_UP,
    ACHIEVEMENT,
    GOAL,
    ENCOUNTER,
};

struct ItemInfo {
    const char* name;
    const char* tag;
    const char* story;
    ItemRarity rarity;
    uint8_t spriteId;
};

struct ItemAward {
    uint8_t itemId;
    ItemDropSource source;
    bool firstTime;
    uint32_t contextOrdinal;  // source-specific reveal context; 0 when unused
};

// Extra luck folded into every award() roll. The locker has no opinion about
// where a bonus comes from, so buff systems register themselves here rather
// than being linked in — which also keeps the host tests free of the UI stack.
using LuckProvider = uint8_t (*)();
void setLuckProvider(LuckProvider provider);

void init(HAL* hal = nullptr);
void update();
void save();
uint8_t getItemCount();
const ItemInfo* getItem(uint8_t id);
const char* getRarityLabel(ItemRarity rarity);
const char* getSourceLabel(ItemDropSource source);
uint8_t getCollectedCount();
bool hasCollected(uint8_t id);
const char* getStory(uint8_t id);
uint32_t getTotalDropCount();
uint32_t getDuplicateDropCount();
uint32_t getKHorseDropCount();
uint8_t getKHorseTranslationLevel();
uint8_t getKHorseItemId();
bool award(ItemDropSource source, uint8_t luck = 0);
// Earned payouts use this path so reveal-queue pressure cannot erase them.
// Deferred tickets materialize during update() or settle into the ledger on save().
bool awardGuaranteed(ItemDropSource source, uint8_t luck = 0);
bool canQueueAward();
bool hasPendingAward();
bool popPendingAward(ItemAward& out);

}  // namespace ItemDrops
