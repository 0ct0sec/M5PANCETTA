/**
 * Item Drops - persistent trinkets from the hunt loop.
 */

#include "item_drops.h"
#include "../hal/hal_interface.h"
#include <stddef.h>

namespace ItemDrops {

static HAL* _hal = nullptr;
static LuckProvider luckProvider = nullptr;
static uint32_t collectedLo = 0;
static uint32_t collectedHi = 0;
static uint32_t totalDropCount = 0;
static uint32_t duplicateDropCount = 0;
static uint32_t kHorseDrops = 0;
static uint32_t kHorsePity = 0;
static bool collectedDirty = false;
static bool statsDirty = false;
static uint32_t lastChangeTime = 0;
static constexpr uint32_t SAVE_DELAY_MS = 5000;

static constexpr uint8_t AWARD_QUEUE_SIZE = 8;
static ItemAward awardQueue[AWARD_QUEUE_SIZE];
static uint8_t awardHead = 0;
static uint8_t awardTail = 0;

// Count exact source/luck tickets so earned payouts under reveal pressure do
// not collapse into one delayed drop. Capture/handshake rolls may fail; every
// source below represents a reward the player has already earned.
static constexpr ItemDropSource GUARANTEED_SOURCES[] = {
    ItemDropSource::LEVEL_UP,
    ItemDropSource::GOAL,
    ItemDropSource::CHALLENGE,
    ItemDropSource::SWEEP,
    ItemDropSource::ACHIEVEMENT,
    ItemDropSource::ENCOUNTER,
};
static constexpr uint8_t GUARANTEED_SOURCE_COUNT =
    sizeof(GUARANTEED_SOURCES) / sizeof(GUARANTEED_SOURCES[0]);
static constexpr uint8_t GUARANTEED_LUCK_BUCKETS = 11;
static uint16_t guaranteedPending[GUARANTEED_SOURCE_COUNT][GUARANTEED_LUCK_BUCKETS] = {};

static int guaranteedSourceIndex(ItemDropSource source) {
    for (uint8_t i = 0; i < GUARANTEED_SOURCE_COUNT; ++i) {
        if (GUARANTEED_SOURCES[i] == source) return i;
    }
    return -1;
}

static void materializeGuaranteedAward();
static void settleGuaranteedAwards();

static const ItemInfo ITEMS[] = {
    {"K3T4 FL4SK",    "cold case flask",
     "a dented hip flask from K-H0RS3's first antenna case. the cap seals; the alibi leaks.",
     ItemRarity::UNCOMMON,  0},
    {"D34UTH D0NUT",  "missing m2",
     "a glazed donut with M2 bitten from the ring. the client left; crumbs kept the sequence.",
     ItemRarity::COMMON,    1},
    {"PMK1D SL1C3",   "rsn receipt",
     "a cold PMKID slice on wax paper. the AP volunteered RSN evidence before anyone ordered.",
     ItemRarity::COMMON,    2},
    {"D4RK P4CK3T",   "raw frame",
     "a black evidence envelope holding one randomized-MAC frame. the address changed; the payload did not.",
     ItemRarity::UNCOMMON,  3},
    {"GUMMY W1TN3SS", "60pct credible",
     "two gummy worms bagged as informants. sixty percent matched the trace; both demanded full credit.",
     ItemRarity::COMMON,    4},
    {"R4M3N R3GR3T",  "3am broth",
     "a paper ramen cup from the channel 6 stakeout. broth went cold while the beacon kept talking.",
     ItemRarity::COMMON,    5},
    {"C0LD DNH",      "zero tx",
     "a steel coffee mug marked ZERO TX. the case stayed passive; the ENTER key remained a suspect.",
     ItemRarity::COMMON,    6},
    {"P4RT T4BL3",    "flash relic",
     "a brass partition-table gear from the flash autopsy. one offset was wrong; the whole board remembered.",
     ItemRarity::UNCOMMON,  7},
    {"BUG R3C31PT",   "issue ticket",
     "a paper issue ticket stamped with file and line. the bug denied everything until the stack trace arrived.",
     ItemRarity::COMMON,    8},
    {"FN0W JU1C3",    "sealed peer",
     "a foil-sealed juice box carrying one FNOW/3 peer frame. the auth tag checked out; the straw did not.",
     ItemRarity::RARE,      9},
    {"F0RB1D CH33S3", "fake xp",
     "a yellow cheese crystal cut from counterfeit XP. it shines in the ledger; the total refuses it.",
     ItemRarity::RARE,     10},
    {"0UR0 B4N4N4",   "ships anyway",
     "a bruised banana shaped like the release loop. code became bugs, bugs became notes; lunch shipped anyway.",
     ItemRarity::COMMON,   11},
    {"C0R3DUMP C00K13", "panic crumbs",
     "a cinnamon cookie packed around a coredump. the crash left a stack trace and one usable crumb.",
     ItemRarity::COMMON,   12},
    {"P1N SW1TCH",    "g15 not g13",
     "a red DIP switch labeled G15 over a crossed-out G13. the board map entered evidence without counsel.",
     ItemRarity::UNCOMMON, 13},
    {"H4SHC4T B4R",   "22000 sweet",
     "a dark chocolate bar wrapped in hashcat 22000. the handshake survived the freezer; the wrapper cracked first.",
     ItemRarity::COMMON,   14},
    {"3AM MUG",       "code vessel",
     "an enamel mug from the 03:00 heap watch. the crash cooled inside; coffee remained non-reproducible.",
     ItemRarity::COMMON,   15},
    {"R34DM3 H00D",   "docs receipt",
     "a tiny hood stitched from README pages. the documented fix was inside; the suspect skipped the lining.",
     ItemRarity::RARE,     16},
    {"0U1 C4CH3",     "450 prefixes",
     "a pocket index of 450 OUI prefixes in PROGMEM. vendors changed coats; lookup stayed O(1).",
     ItemRarity::COMMON,   17},
    {"C4RDPTR C4NDY", "tiny screen",
     "a green hard candy shaped like a Cardputer. no PSRAM inside; the heap requested smaller bites.",
     ItemRarity::UNCOMMON, 18},
    {"CR4SH FR4M3",   "coredump art",
     "a cracked glass frame around a coredump address. the fault stayed still long enough for a photograph.",
     ItemRarity::COMMON,   19},
    {"P1 F3D0R4",     "case mode",
     "a felt fedora soaked by channel rain. Pancetta put it on; the BSSID asked for representation.",
     ItemRarity::RARE,     20},
    {"D34UTH C4N",    "mudball fizz",
     "an aluminum can stamped MUDBALL. deauth transmits; open only on networks you own. the tab is evidence.",
     ItemRarity::UNCOMMON, 21},
    {"R4C3 C0ND1T",   "frozen suspect",
     "a domino bar wedged between two threads. both callbacks moved first; the heap found the body.",
     ItemRarity::COMMON,   22},
    {"W1GL3 SYN4P",   "map can",
     "a tin of WiGLE CSV rows from the road desk. upload accepted the map; the streetlights kept silent.",
     ItemRarity::COMMON,   23},
    {"BL4M3 S0UP",    "hash receipts",
     "a soup cup printed with commit hashes. git blame named the cook; the stack trace asked for seconds.",
     ItemRarity::UNCOMMON, 24},
    {"M3RCY KN0T",    "trusted list",
     "two hard candies tied with a mercy knot. trusted BSSIDs stay off the deauth list; restraint got a receipt.",
     ItemRarity::COMMON,   25},
    {"TH3 BR1CK",     "ota casualty",
     "a warm dev board hardened into a brick after OTA. the bootloader arrived; firmware missed the meeting.",
     ItemRarity::COMMON,   26},
    {"D0 N0 H4M",     "passive badge",
     "a crescent enamel badge stamped PASSIVE. it watched the dock without transmitting and billed zero frames.",
     ItemRarity::UNCOMMON, 27},
    {"H0TF1X T4P3",   "guards back",
     "a strip of hotfix tape over one race window. the guard held; the frozen suspect finally moved.",
     ItemRarity::COMMON,   28},
    {"ML TR41N B4R",  "model receipt",
     "a candy bar pressed from labeled training dots. the model learned the pattern; PSRAM kept the invoice.",
     ItemRarity::COMMON,   29},
    {"GPS ST1CKS",    "wardrive legs",
     "two sugar sticks shaped like GPS antennas. the fix drew circles first, then admitted where the road was.",
     ItemRarity::RARE,     30},
    {"K-H0RS3",       "barman codec",
     "a coiled antenna-hose tagged K-H0RS3. the third drop decodes the Barman; later copies buy advice.",
     ItemRarity::RARE,     31},
    {"5GHZ V0RT3X",   "offbeat band",
     "a rainbow RF funnel labeled 5 GHz, outside this radio's beat. Pancetta filed it under other jurisdiction.",
     ItemRarity::LEGENDARY,32},
    {"P4RT ST4CK",    "sd tower",
     "a stack of SD directory cards with /handshakes/ on top. one tenant had four messages and no password.",
     ItemRarity::UNCOMMON, 33},
    {"3CG SN1FF",     "heartbeat lead",
     "a lollipop stethoscope clipped to a client trace. RSSI supplied the pulse; timeout signed the certificate.",
     ItemRarity::RARE,     34},
    {"WP4 SQU33Z3",   "crack tube",
     "a squeeze tube packed with WPA captures. upload, wait, authenticate; latency took the fingerprints.",
     ItemRarity::COMMON,   35},
    {"XP CRYST4L",    "earned xp",
     "a faceted XP token bound to this device. honest captures lit it; counterfeit cheese stayed dark.",
     ItemRarity::RARE,     36},
    {"FN0W DUMP",     "peer packet",
     "a teal evidence pouch stamped FNOW/3. the peer signed its summary; raw MACs stayed outside.",
     ItemRarity::COMMON,   37},
    {"SP3CTRUM C0N3", "sinc scoop",
     "a black ice-cream cone charted with 45 LUT entries. RF scope filed each lobe; dessert melted at compile time.",
     ItemRarity::LEGENDARY,38},
    {"T3RM1N4L B0WL", "115200 soup",
     "a ramen bowl with a 115200 8N1 prompt at the rim. the serial suspect answered in heap addresses.",
     ItemRarity::UNCOMMON, 39},
    {"R41NB0W B4C0N", "band laminate",
     "an iridescent bacon strip laminated with band labels. it looked edible; the spectrum called it dielectric.",
     ItemRarity::LEGENDARY,40},
    {"H34P P4TCH V14L", "deterministic fix",
     "a glass vial holding three lines of C. the heap stopped fragmenting and demanded a smaller incident report.",
     ItemRarity::LEGENDARY,41},
};

static constexpr uint8_t ITEM_COUNT = sizeof(ITEMS) / sizeof(ITEMS[0]);
static constexpr uint8_t K_HORSE_ITEM_ID = 31;

static uint32_t rng(uint32_t max) {
    if (max == 0) return 0;
    return _hal ? _hal->random(max) : 0;
}

static uint8_t clampLuck(uint8_t luck) {
    return (luck > 10) ? 10 : luck;
}

static uint8_t effectiveLuck(uint8_t luck) {
    uint32_t boosted = (uint32_t)luck +
                       (luckProvider ? luckProvider() : 0);
    return clampLuck((uint8_t)(boosted > 255 ? 255 : boosted));
}

static bool awardQueueHasRoom() {
    uint8_t nextTail = (awardTail + 1) % AWARD_QUEUE_SIZE;
    return nextTail != awardHead;
}

static bool queueAward(uint8_t itemId, ItemDropSource source, bool firstTime,
                       uint32_t contextOrdinal) {
    if (!awardQueueHasRoom()) return false;
    awardQueue[awardTail] = { itemId, source, firstTime, contextOrdinal };
    awardTail = (awardTail + 1) % AWARD_QUEUE_SIZE;
    return true;
}

static void flushCollected() {
    if (!_hal || !collectedDirty) return;
    _hal->storagePutUInt("sirloin", "itm_lo", collectedLo);
    _hal->storagePutUInt("sirloin", "itm_hi", collectedHi);
    collectedDirty = false;
}

static void flushDropStats() {
    if (!_hal || !statsDirty) return;
    _hal->storagePutUInt("sirloin", "itm_drop", totalDropCount);
    _hal->storagePutUInt("sirloin", "itm_dup", duplicateDropCount);
    _hal->storagePutUInt("sirloin", "kh_drps", kHorseDrops);
    _hal->storagePutUInt("sirloin", "kh_pity", kHorsePity);
    statsDirty = false;
}

static void flushPersistence() {
    flushCollected();
    flushDropStats();
}

static void markPersistenceDirty(bool collectionChanged) {
    if (collectionChanged) collectedDirty = true;
    statsDirty = true;
    lastChangeTime = _hal ? _hal->millis() : 0;
}

static void markCollected(uint8_t id) {
    if (id >= ITEM_COUNT) return;
    if (id < 32) collectedLo |= (1UL << id);
    else collectedHi |= (1UL << (id - 32));
}

static bool canSummonKHorse(ItemDropSource source) {
    switch (source) {
        case ItemDropSource::HANDSHAKE:
        case ItemDropSource::CHALLENGE:
        case ItemDropSource::SWEEP:
        case ItemDropSource::LEVEL_UP:
        case ItemDropSource::ACHIEVEMENT:
        case ItemDropSource::GOAL:
        case ItemDropSource::ENCOUNTER:
            return true;
        case ItemDropSource::CAPTURE:
        default:
            return false;
    }
}

static int pickKHorseSpecial(ItemDropSource source, uint8_t luck) {
    if (!canSummonKHorse(source)) return -1;

    uint8_t chance = (kHorseDrops < 3) ? 12 : 4;
    switch (source) {
        case ItemDropSource::SWEEP:       chance += 14; break;
        case ItemDropSource::ACHIEVEMENT: chance += 10; break;
        case ItemDropSource::LEVEL_UP:    chance += 8;  break;
        case ItemDropSource::CHALLENGE:   chance += 4;  break;
        case ItemDropSource::GOAL:        chance += 3;  break;
        case ItemDropSource::ENCOUNTER:   chance += 5;  break;
        case ItemDropSource::HANDSHAKE:   chance += 2;  break;
        case ItemDropSource::CAPTURE:     break;
    }
    chance += clampLuck(luck);
    if (chance > 55) chance = 55;

    uint32_t pityLimit = (kHorseDrops < 3) ? 7 : 18;
    if (kHorsePity >= pityLimit) return K_HORSE_ITEM_ID;
    return (rng(100) < chance) ? K_HORSE_ITEM_ID : -1;
}

static ItemRarity rollRarity(ItemDropSource source, uint8_t luck) {
    uint8_t boost = clampLuck(luck) * 2;
    switch (source) {
        case ItemDropSource::SWEEP:
            return (rng(100) < 30) ? ItemRarity::LEGENDARY : ItemRarity::RARE;
        case ItemDropSource::ACHIEVEMENT: boost += 14; break;
        case ItemDropSource::LEVEL_UP:    boost += 11; break;
        case ItemDropSource::CHALLENGE:   boost += 6;  break;
        case ItemDropSource::GOAL:        boost += 5;  break;
        case ItemDropSource::ENCOUNTER:   boost += 7;  break;
        case ItemDropSource::HANDSHAKE:   boost += 2;  break;
        case ItemDropSource::CAPTURE:
        default: break;
    }

    uint16_t roll = (uint16_t)rng(100) + boost;
    if (roll >= 100) return ItemRarity::LEGENDARY;
    if (roll >= 88) return ItemRarity::RARE;
    if (roll >= 52) return ItemRarity::UNCOMMON;
    return ItemRarity::COMMON;
}

static int pickMatching(ItemRarity rarity, bool onlyNew) {
    int pick = -1;
    uint8_t seen = 0;
    for (uint8_t i = 0; i < ITEM_COUNT; i++) {
        if (ITEMS[i].rarity != rarity) continue;
        if (onlyNew && hasCollected(i)) continue;
        seen++;
        if (rng(seen) == 0) pick = i;
    }
    return pick;
}

static int pickNewFallback(ItemRarity rarity) {
    static const ItemRarity COMMON_ORDER[] = {
        ItemRarity::COMMON, ItemRarity::UNCOMMON, ItemRarity::RARE, ItemRarity::LEGENDARY
    };
    static const ItemRarity UNCOMMON_ORDER[] = {
        ItemRarity::UNCOMMON, ItemRarity::COMMON, ItemRarity::RARE, ItemRarity::LEGENDARY
    };
    static const ItemRarity RARE_ORDER[] = {
        ItemRarity::RARE, ItemRarity::UNCOMMON, ItemRarity::COMMON, ItemRarity::LEGENDARY
    };
    static const ItemRarity LEGENDARY_ORDER[] = {
        ItemRarity::LEGENDARY, ItemRarity::RARE, ItemRarity::UNCOMMON, ItemRarity::COMMON
    };

    const ItemRarity* order = COMMON_ORDER;
    switch (rarity) {
        case ItemRarity::COMMON:    order = COMMON_ORDER; break;
        case ItemRarity::UNCOMMON:  order = UNCOMMON_ORDER; break;
        case ItemRarity::RARE:      order = RARE_ORDER; break;
        case ItemRarity::LEGENDARY: order = LEGENDARY_ORDER; break;
    }

    for (uint8_t i = 0; i < 4; i++) {
        int picked = pickMatching(order[i], true);
        if (picked >= 0) return picked;
    }
    return -1;
}

static int pickAnyNew() {
    int pick = -1;
    uint8_t seen = 0;
    for (uint8_t i = 0; i < ITEM_COUNT; i++) {
        if (hasCollected(i)) continue;
        seen++;
        if (rng(seen) == 0) pick = i;
    }
    return pick;
}

void setLuckProvider(LuckProvider provider) {
    luckProvider = provider;
}

void init(HAL* hal) {
    _hal = hal ? hal : HalGlobal::get();
    awardHead = 0;
    awardTail = 0;
    collectedLo = _hal ? _hal->storageGetUInt("sirloin", "itm_lo", 0) : 0;
    collectedHi = _hal ? _hal->storageGetUInt("sirloin", "itm_hi", 0) : 0;
    totalDropCount = _hal ? _hal->storageGetUInt("sirloin", "itm_drop", 0) : 0;
    duplicateDropCount = _hal ? _hal->storageGetUInt("sirloin", "itm_dup", 0) : 0;
    kHorseDrops = _hal ? _hal->storageGetUInt("sirloin", "kh_drps", 0) : 0;
    kHorsePity = _hal ? _hal->storageGetUInt("sirloin", "kh_pity", 0) : 0;
    collectedDirty = false;
    statsDirty = false;
    lastChangeTime = 0;
    for (uint8_t source = 0; source < GUARANTEED_SOURCE_COUNT; ++source) {
        for (uint8_t luck = 0; luck < GUARANTEED_LUCK_BUCKETS; ++luck) {
            guaranteedPending[source][luck] = 0;
        }
    }
}

void update() {
    // Materialize at most one deferred payout per frame in earned-event order.
    // LEVEL precedes GOAL, matching the boot path where goal XP can cross a level.
    materializeGuaranteedAward();

    if (_hal && (collectedDirty || statsDirty) &&
        _hal->millis() - lastChangeTime >= SAVE_DELAY_MS) {
        flushPersistence();
    }
}

void save() {
    // Controlled shutdown cannot wait for reveal capacity. Materialize earned
    // Config tickets directly into the ledger, then flush the whole batch.
    settleGuaranteedAwards();
    flushPersistence();
}

uint8_t getItemCount() {
    return ITEM_COUNT;
}

const ItemInfo* getItem(uint8_t id) {
    if (id >= ITEM_COUNT) return nullptr;
    return &ITEMS[id];
}

const char* getRarityLabel(ItemRarity rarity) {
    switch (rarity) {
        case ItemRarity::COMMON:    return "COMMON";
        case ItemRarity::UNCOMMON:  return "UNCOMMON";
        case ItemRarity::RARE:      return "RARE";
        case ItemRarity::LEGENDARY: return "LEGEND";
    }
    return "???";
}

const char* getSourceLabel(ItemDropSource source) {
    switch (source) {
        case ItemDropSource::CAPTURE:     return "CAP";
        case ItemDropSource::HANDSHAKE:   return "HS";
        case ItemDropSource::CHALLENGE:   return "TASK";
        case ItemDropSource::SWEEP:       return "SWEEP";
        case ItemDropSource::LEVEL_UP:    return "LEVEL";
        case ItemDropSource::ACHIEVEMENT: return "ACH";
        case ItemDropSource::GOAL:        return "GOAL";
        case ItemDropSource::ENCOUNTER:   return "NPC";
    }
    return "DROP";
}

uint8_t getCollectedCount() {
    uint8_t count = 0;
    uint32_t lo = collectedLo;
    uint32_t hi = collectedHi;
    while (lo) { count += (uint8_t)(lo & 1U); lo >>= 1; }
    while (hi) { count += (uint8_t)(hi & 1U); hi >>= 1; }
    if (count > ITEM_COUNT) count = ITEM_COUNT;
    return count;
}

bool hasCollected(uint8_t id) {
    if (id >= ITEM_COUNT) return false;
    if (id < 32) return (collectedLo & (1UL << id)) != 0;
    return (collectedHi & (1UL << (id - 32))) != 0;
}

const char* getStory(uint8_t id) {
    const ItemInfo* item = getItem(id);
    return item ? item->story : "";
}

uint32_t getTotalDropCount() {
    return totalDropCount;
}

uint32_t getDuplicateDropCount() {
    return duplicateDropCount;
}

uint32_t getKHorseDropCount() {
    return kHorseDrops;
}

uint8_t getKHorseTranslationLevel() {
    return (kHorseDrops >= 3) ? 3 : (uint8_t)kHorseDrops;
}

uint8_t getKHorseItemId() {
    return K_HORSE_ITEM_ID;
}

static bool materializeAward(ItemDropSource source, uint8_t luck,
                             bool queueReveal) {
    if (queueReveal && !awardQueueHasRoom()) return false;

    int picked = pickKHorseSpecial(source, luck);
    if (picked < 0) {
        ItemRarity rarity = rollRarity(source, luck);
        picked = pickNewFallback(rarity);
        if (picked < 0) picked = pickMatching(rarity, false);
        if (picked < 0) picked = pickAnyNew();
        if (picked < 0) picked = (int)rng(ITEM_COUNT);
    }

    uint8_t itemId = (uint8_t)picked;
    bool firstTime = !hasCollected(itemId);
    uint32_t contextOrdinal = 0;
    totalDropCount++;
    if (!firstTime) duplicateDropCount++;
    if (itemId == K_HORSE_ITEM_ID) {
        kHorseDrops++;
        contextOrdinal = kHorseDrops;
        kHorsePity = 0;
    } else if (canSummonKHorse(source) && kHorsePity < 1000) {
        kHorsePity++;
    }
    if (firstTime) {
        markCollected(itemId);
    }
    markPersistenceDirty(firstTime);

    const ItemInfo* info = getItem(itemId);
    if (_hal && _hal->terminalIsVisible() && info) {
        if (itemId == K_HORSE_ITEM_ID) {
            _hal->terminalPush("ITEM %s [%s/%s] K:%lu%s",
                               info->name,
                               getRarityLabel(info->rarity),
                               getSourceLabel(source),
                               (unsigned long)kHorseDrops,
                               firstTime ? " NEW" : " AGAIN");
        } else {
            _hal->terminalPush("ITEM %s [%s/%s]%s",
                               info->name,
                               getRarityLabel(info->rarity),
                               getSourceLabel(source),
                               firstTime ? " NEW" : " AGAIN");
        }
    }

    if (!queueReveal) return true;
    return queueAward(itemId, source, firstTime, contextOrdinal);
}

bool award(ItemDropSource source, uint8_t luck) {
    return materializeAward(source, effectiveLuck(luck), true);
}

bool awardGuaranteed(ItemDropSource source, uint8_t luck) {
    // Freeze LUCK at earn time. A deferred ticket must not gain or lose odds
    // because a consumable expires before its reveal lane becomes available.
    uint8_t earnedLuck = effectiveLuck(luck);
    if (materializeAward(source, earnedLuck, true)) return true;

    int sourceIndex = guaranteedSourceIndex(source);
    if (sourceIndex < 0) return false;
    uint16_t& pending = guaranteedPending[sourceIndex][earnedLuck];
    if (pending == UINT16_MAX) return false;
    ++pending;
    return true;
}

static void materializeGuaranteedAward() {
    if (!awardQueueHasRoom()) return;
    for (uint8_t source = 0; source < GUARANTEED_SOURCE_COUNT; ++source) {
        for (uint8_t luck = 0; luck < GUARANTEED_LUCK_BUCKETS; ++luck) {
            if (guaranteedPending[source][luck] == 0) continue;
            ItemDropSource dropSource = GUARANTEED_SOURCES[source];
            if (materializeAward(dropSource, luck, true)) {
                --guaranteedPending[source][luck];
            }
            return;
        }
    }
}

static void settleGuaranteedAwards() {
    if (!_hal) return;
    for (uint8_t source = 0; source < GUARANTEED_SOURCE_COUNT; ++source) {
        for (uint8_t luck = 0; luck < GUARANTEED_LUCK_BUCKETS; ++luck) {
            ItemDropSource dropSource = GUARANTEED_SOURCES[source];
            while (guaranteedPending[source][luck] > 0) {
                if (!materializeAward(dropSource, luck, false)) return;
                --guaranteedPending[source][luck];
            }
        }
    }
}

bool canQueueAward() {
    return awardQueueHasRoom();
}

bool hasPendingAward() {
    return awardHead != awardTail;
}

bool popPendingAward(ItemAward& out) {
    if (awardHead == awardTail) return false;
    out = awardQueue[awardHead];
    awardHead = (awardHead + 1) % AWARD_QUEUE_SIZE;
    return true;
}

}  // namespace ItemDrops
