/**
 * Item Effects - Implementation
 *
 * The table below is ordered by item id and must stay that way; a static_assert
 * pins its length to ItemDrops' catalogue so a new trinket cannot ship without
 * a consequence.
 */

#include "item_effects.h"

#include "item_drops.h"
#include "config.h"
#include "../hal/hal_interface.h"
#include "../piglet/mood.h"

#include <stdio.h>

namespace ItemEffects {

// ==[ THE CONSEQUENCE TABLE ]==
// Rarity sets the ceiling, but the flavour sets the lever: food restores mood,
// receipts and documentation pay XP, anything that sharpened Pancetta's aim
// becomes FOCUS, anything that changed his odds becomes LUCK, and anything that
// told him to slow down becomes CALM.
static const Effect EFFECTS[] = {
    // 0 K3T4 FL4SK — the cap seals; the alibi leaks.
    {6, 0, Buff::CALM, 20, 600,
     "the flask holds. so does Pancetta. the shakes file for leave."},
    // 1 D34UTH D0NUT — M2 bitten from the ring.
    {4, 0, Buff::NONE, 0, 0,
     "sugar lands. M2 is still missing. the sequence forgives nothing."},
    // 2 PMK1D SL1C3 — the AP volunteered.
    {2, 6, Buff::NONE, 0, 0,
     "cold, sweet, admissible. the RSN receipt goes down easy."},
    // 3 D4RK P4CK3T — the address changed; the payload did not.
    {0, 4, Buff::FOCUS, 12, 300,
     "one randomized MAC, read end to end. the eye sharpens."},
    // 4 GUMMY W1TN3SS — sixty percent matched.
    {5, 0, Buff::LUCK, 2, 300,
     "two informants, sixty percent credible. the odds tilt anyway."},
    // 5 R4M3N R3GR3T — broth went cold on the channel 6 stakeout.
    {9, 0, Buff::NONE, 0, 0,
     "cold broth, warm memory. the stakeout stops aching."},
    // 6 C0LD DNH — ZERO TX.
    {3, 0, Buff::CALM, 25, 900,
     "zero transmit, full cup. the ENTER key stays a suspect."},
    // 7 P4RT T4BL3 — one offset was wrong.
    {-2, 12, Buff::NONE, 0, 0,
     "the offset is corrected in ink. the board still remembers."},
    // 8 BUG R3C31PT — file and line.
    {1, 8, Buff::NONE, 0, 0,
     "the ticket closes. the stack trace signed for it."},
    // 9 FN0W JU1C3 — the auth tag checked out; the straw did not.
    {7, 10, Buff::NONE, 0, 0,
     "sealed peer, honest sugar. the straw remains under review."},
    // 10 F0RB1D CH33S3 — counterfeit XP. The ledger refuses it.
    {-8, 0, Buff::NONE, 0, 0,
     "counterfeit XP tastes correct and totals zero. the ledger holds."},
    // 11 0UR0 B4N4N4 — lunch shipped anyway.
    {6, 3, Buff::NONE, 0, 0,
     "bruised, looping, shipped. the release ate itself and kept going."},
    // 12 C0R3DUMP C00K13 — one usable crumb.
    {4, 5, Buff::NONE, 0, 0,
     "cinnamon and panic. one crumb of the crash is still legible."},
    // 13 P1N SW1TCH — G15 over a crossed-out G13.
    {0, 9, Buff::FOCUS, 15, 300,
     "the pin map corrects itself. G13 finally gets counsel."},
    // 14 H4SHC4T B4R — the handshake survived the freezer.
    {3, 7, Buff::NONE, 0, 0,
     "22000, dark and unbroken. the wrapper cracked first."},
    // 15 3AM MUG — coffee remained non-reproducible.
    {8, 0, Buff::FOCUS, 10, 600,
     "the 03:00 watch resumes. the heap gets a second reader."},
    // 16 R34DM3 H00D — the documented fix was inside.
    {2, 16, Buff::NONE, 0, 0,
     "the documented fix was in the lining the whole time."},
    // 17 0U1 C4CH3 — 450 prefixes, O(1).
    {0, 6, Buff::FOCUS, 12, 600,
     "450 prefixes, one lookup. vendors stop changing coats."},
    // 18 C4RDPTR C4NDY — the heap requested smaller bites.
    {5, 4, Buff::NONE, 0, 0,
     "no PSRAM inside. the heap approves of the portion size."},
    // 19 CR4SH FR4M3 — the fault held still for a photograph.
    {-1, 8, Buff::NONE, 0, 0,
     "the fault sat still long enough to be filed. framed and dated."},
    // 20 P1 F3D0R4 — the BSSID asked for representation.
    {10, 0, Buff::FOCUS, 25, 900,
     "hat on. the case gets a detective and the BSSID gets a lawyer."},
    // 21 D34UTH C4N — MUDBALL. Deauth transmits.
    {-4, 6, Buff::LUCK, 4, 300,
     "the tab is evidence. open only on networks you own."},
    // 22 R4C3 C0ND1T — both callbacks moved first.
    {-3, 7, Buff::NONE, 0, 0,
     "two threads, one domino. the heap found the body first."},
    // 23 W1GL3 SYN4P — upload accepted the map.
    {3, 6, Buff::NONE, 0, 0,
     "the map uploads. the streetlights keep their silence."},
    // 24 BL4M3 S0UP — git blame named the cook.
    {-2, 11, Buff::NONE, 0, 0,
     "git blame named the cook. the stack trace asked for seconds."},
    // 25 M3RCY KN0T — trusted BSSIDs stay off the list.
    {7, 4, Buff::CALM, 20, 600,
     "restraint gets a receipt. the trusted list stays untouched."},
    // 26 TH3 BR1CK — the bootloader arrived; firmware missed it.
    {-9, 0, Buff::NONE, 0, 0,
     "warm, rectangular, unbootable. OTA took the meeting alone."},
    // 27 D0 N0 H4M — watched the dock, billed zero frames.
    {6, 5, Buff::CALM, 30, 900,
     "passive badge on. zero frames billed, nothing owed."},
    // 28 H0TF1X T4P3 — the guard held.
    {5, 6, Buff::NONE, 0, 0,
     "one race window taped shut. the frozen suspect finally moved."},
    // 29 ML TR41N B4R — PSRAM kept the invoice.
    {2, 7, Buff::FOCUS, 12, 300,
     "the model learned the pattern. PSRAM still wants paying."},
    // 30 GPS ST1CKS — the fix drew circles, then admitted the road.
    {4, 12, Buff::LUCK, 3, 600,
     "the fix stops drawing circles and names the street."},
    // 31 K-H0RS3 — the third drop decodes the Barman.
    {8, 14, Buff::NONE, 0, 0,
     "the hose uncoils one more word of the Barman's language."},
    // 32 5GHZ V0RT3X — outside this radio's beat.
    {-5, 0, Buff::NONE, 0, 0,
     "other jurisdiction. this radio files it and walks away."},
    // 33 P4RT ST4CK — one tenant had four messages and no password.
    {1, 9, Buff::LUCK, 3, 300,
     "the directory opens. one tenant, four messages, no password."},
    // 34 3CG SN1FF — RSSI supplied the pulse.
    {2, 10, Buff::FOCUS, 18, 600,
     "RSSI gives the pulse. timeout signs the certificate."},
    // 35 WP4 SQU33Z3 — latency took the fingerprints.
    {0, 8, Buff::NONE, 0, 0,
     "upload, wait, authenticate. latency kept the prints."},
    // 36 XP CRYST4L — honest captures lit it.
    {6, 25, Buff::NONE, 0, 0,
     "earned XP, bound to this device. the counterfeit stays dark."},
    // 37 FN0W DUMP — the peer signed its summary.
    {2, 6, Buff::NONE, 0, 0,
     "the peer signed for it. raw MACs never left the pouch."},
    // 38 SP3CTRUM C0N3 — 45 LUT entries, melted at compile time.
    {9, 15, Buff::FOCUS, 30, 900,
     "every lobe charted. dessert melted at compile time."},
    // 39 T3RM1N4L B0WL — the serial suspect answered in heap addresses.
    {4, 8, Buff::NONE, 0, 0,
     "115200 8N1 at the rim. the suspect answers in hex."},
    // 40 R41NB0W B4C0N — the spectrum called it dielectric.
    {12, 10, Buff::LUCK, 5, 900,
     "it looked edible. the spectrum calls it dielectric anyway."},
    // 41 H34P P4TCH V14L — three lines of C.
    {10, 20, Buff::CALM, 40, 900,
     "three lines of C. the heap stops fragmenting and stops writing."},
};

static constexpr uint8_t EFFECT_COUNT =
    sizeof(EFFECTS) / sizeof(EFFECTS[0]);
static_assert(EFFECT_COUNT == 42,
              "every trinket in the ItemDrops catalogue needs a consequence");

// ==[ RUNTIME ]==
static constexpr const char* NS = "sirloin";
static constexpr const char* KEY_LO = "eff_lo";
static constexpr const char* KEY_HI = "eff_hi";
static constexpr const char* KEY_BURNS = "eff_brn";

static HAL* _hal = nullptr;
static uint32_t consumedLo = 0;
static uint32_t consumedHi = 0;
static uint8_t burnsUsed = 0;
static bool dirty = false;
static bool initialized = false;

struct ActiveBuff {
    uint8_t magnitude = 0;
    uint32_t expiresAt = 0;  // hal millis
    bool live = false;
};
static ActiveBuff active[(uint8_t)Buff::COUNT];

static uint32_t nowMs() { return _hal ? _hal->millis() : 0; }

// Signed compare so a buff cannot outlive a millis() rollover.
static bool expired(uint32_t deadline) {
    return (int32_t)(nowMs() - deadline) >= 0;
}

void init(HAL* hal) {
    if (initialized) return;
    initialized = true;
    _hal = hal ? hal : HalGlobal::get();
    consumedLo = _hal ? _hal->storageGetUInt(NS, KEY_LO, 0) : 0;
    consumedHi = _hal ? _hal->storageGetUInt(NS, KEY_HI, 0) : 0;
    burnsUsed = _hal ? (uint8_t)_hal->storageGetUInt(NS, KEY_BURNS, 0) : 0;
    if (burnsUsed > SESSION_BURN_LIMIT) burnsUsed = SESSION_BURN_LIMIT;
    dirty = false;
    for (auto& buff : active) buff = ActiveBuff{};
    // Register the LUCK lever with the locker rather than having the locker
    // link against this module.
    ItemDrops::setLuckProvider(&luckBonus);
}

void save() {
    if (!_hal || !dirty) return;
    _hal->storagePutUInt(NS, KEY_LO, consumedLo);
    _hal->storagePutUInt(NS, KEY_HI, consumedHi);
    _hal->storagePutUInt(NS, KEY_BURNS, burnsUsed);
    dirty = false;
}

void update() {
    for (auto& buff : active) {
        if (buff.live && expired(buff.expiresAt)) buff = ActiveBuff{};
    }
}

const Effect* effectFor(uint8_t itemId) {
    if (itemId >= EFFECT_COUNT) return nullptr;
    return &EFFECTS[itemId];
}

bool wasConsumed(uint8_t itemId) {
    if (itemId >= EFFECT_COUNT) return false;
    return (itemId < 32) ? (consumedLo & (1UL << itemId)) != 0
                         : (consumedHi & (1UL << (itemId - 32))) != 0;
}

static void markConsumed(uint8_t itemId) {
    if (itemId < 32) consumedLo |= (1UL << itemId);
    else consumedHi |= (1UL << (itemId - 32));
}

uint8_t burnsRemaining() {
    return (burnsUsed >= SESSION_BURN_LIMIT)
               ? 0
               : (uint8_t)(SESSION_BURN_LIMIT - burnsUsed);
}

void onSessionStart() {
    consumedLo = 0;
    consumedHi = 0;
    burnsUsed = 0;
    dirty = true;
    save();
}

bool consume(uint8_t itemId, char* verdictOut, size_t outLen) {
    auto say = [&](const char* text) {
        if (verdictOut && outLen) snprintf(verdictOut, outLen, "%s", text);
    };

    if (itemId >= EFFECT_COUNT) {
        say("no such exhibit.");
        return false;
    }
    if (!ItemDrops::hasCollected(itemId)) {
        say("not in the locker. find it first.");
        return false;
    }
    if (wasConsumed(itemId)) {
        say("already burned this session. the locker remembers.");
        return false;
    }
    if (burnsRemaining() == 0) {
        say("three exhibits a session. chain of custody has limits.");
        return false;
    }

    const Effect& effect = EFFECTS[itemId];

    // Mood is always paid: the pig reacts whether or not a session is live.
    if (effect.momentum) Mood::addMomentum(effect.momentum);

    // XP follows the same contract as every other reward lane — no session,
    // no payout. Saying otherwise on the toast would be a fake receipt.
    if (effect.xp && Config::isSessionActive()) {
        Config::addXP(effect.xp, Config::RewardSource::XP_EVENT);
    }

    if (effect.buff != Buff::NONE && effect.durationS) {
        ActiveBuff& slot = active[(uint8_t)effect.buff];
        slot.magnitude = effect.magnitude;
        slot.expiresAt = nowMs() + (uint32_t)effect.durationS * 1000UL;
        slot.live = true;
    }

    markConsumed(itemId);
    if (burnsUsed < SESSION_BURN_LIMIT) ++burnsUsed;
    dirty = true;
    save();

    say(effect.verdict);
    return true;
}

// A buff's magnitude while it is live, 0 otherwise — the one place a slot is
// tested before it is read. update() sweeps expired slots every frame; the
// deadline re-check here keeps a buff from being read live in the frame it
// expires, before the sweep runs.
static uint8_t liveMagnitude(Buff buff) {
    if (buff == Buff::NONE || buff >= Buff::COUNT) return 0;
    const ActiveBuff& slot = active[(uint8_t)buff];
    if (!slot.live || expired(slot.expiresAt)) return 0;
    return slot.magnitude;
}

uint8_t luckBonus() { return liveMagnitude(Buff::LUCK); }

float focusMultiplier() {
    return 1.0f + (float)liveMagnitude(Buff::FOCUS) / 100.0f;
}

uint32_t calmDecayBonusMs() {
    // Magnitude is a percentage of a minute, so CALM 40 buys 24 extra seconds
    // before the mood decays another step.
    return (uint32_t)liveMagnitude(Buff::CALM) * 600UL;
}

const char* buffLabel(Buff buff) {
    switch (buff) {
        case Buff::LUCK:  return "LUCK";
        case Buff::FOCUS: return "F0CUS";
        case Buff::CALM:  return "C4LM";
        case Buff::NONE:
        case Buff::COUNT: break;
    }
    return "";
}

}  // namespace ItemEffects
