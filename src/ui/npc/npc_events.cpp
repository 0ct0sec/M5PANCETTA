#include "npc_events.h"

#include "npc_events_core.h"
#include "npc_portraits.h"
#include "../../audio/sfx.h"
#include "../../core/achievements.h"
#include "../../core/challenges.h"
#include "../../core/config.h"
#include "../../core/item_drops.h"
#include "../../core/mailbox.h"
#include "../../haptic/haptic.h"
#include "../../piglet/mood.h"
#include "../display.h"

#include <stdio.h>
#include <string.h>

namespace NpcEvents {

enum class Phase : uint8_t { IDLE = 0, BRIEFING, CHOICE, RESULT, CODA };
enum class ResultTone : uint8_t { OFFLINE = 0, BACKFIRE, CHAOS, WIN, JACKPOT };
static constexpr uint8_t MAX_CASE_FILES = 32;

static Phase phase = Phase::IDLE;
static bool initialized = false;
static int currentIndex = -1;
static int lastIndex = -1;
// 0 = root beat; otherwise the 1-based CaseNode the operator has walked to.
static uint8_t currentNode = 0;
// Letter backing the open card, or NO_MAIL when nothing filed it.
static constexpr uint8_t NO_MAIL = 0xFF;
static uint8_t mailIndex = NO_MAIL;
static uint8_t selectedChoice = 0;
static uint8_t openerVariant = 0;
static uint8_t replyVariant = 0;
static uint8_t priorChoice = NpcEventsCore::NO_CHOICE;
static uint8_t lastOpenerVariant[NpcEventsCore::CHARACTER_COUNT];
static uint8_t lastReplyVariant[NpcEventsCore::CHARACTER_COUNT][3];
static uint8_t lastStationKey = 0xFF;
static uint8_t misses = 0;
static uint32_t seenMask = 0;
static uint32_t closedCastMask = 0;
static uint32_t sessionClosedCastMask = 0;
static uint32_t choiceLedger = 0;
static uint8_t casesClosed = 0;
static uint32_t armAt = 0;
static uint32_t nextAllowedAt = 0;
static bool armPending = false;
static bool cooldownActive = false;
static uint32_t phaseStartedAt = 0;
static uint32_t resultStartedAt = 0;
static char rewardSummary[64] = "";
static uint8_t rewardReinforcement = 0;
static ResultTone resultTone = ResultTone::OFFLINE;
static bool resultTrophyUnlocked = false;
static bool pendingCoda = false;
// Set when the filed choice opens another beat. The RESULT card advances into
// it instead of closing the file.
static bool pendingFollowUp = false;
static uint8_t pendingNode = 0;
static NpcEventsCore::CaseEnding codaEnding =
    NpcEventsCore::CaseEnding::OPEN_CIRCUIT;

static constexpr uint16_t PORTRAIT_TRANSPARENT_KEY = 0x0001;
// ~31% pull toward the two-color theme: enough for the card to feel themed,
// light enough that the sepia painting survives it.
static constexpr uint8_t PORTRAIT_THEME_TINT = 80;
static constexpr uint32_t PORTRAIT_SLIDE_MS = 320;
static constexpr uint8_t PORTRAIT_DISPLAY_SCALE = 2;
static constexpr int PORTRAIT_W =
    NpcPortraits::PORTRAIT_FIRMWARE_WIDTH * PORTRAIT_DISPLAY_SCALE;
static constexpr int PORTRAIT_H =
    NpcPortraits::PORTRAIT_FIRMWARE_HEIGHT * PORTRAIT_DISPLAY_SCALE;
static_assert(PORTRAIT_W == 144 && PORTRAIT_H == 136,
              "case-card layout is authored around true 2x portraits");

// ==[ PAIRED WITNESS PLATE ]==
// D0H4M and 01NK5 share one plate, so at 2x each face only had room for the
// middle 36 of its 72 source columns. That crop ate 1160 opaque pixels of
// D0H4M and 1022 of 01NK5 — both witnesses lost their ears and shoulders.
// Halving to 1x is the exact integer step that makes two whole 72px grids fill
// the 144px plate, so the pair stays pixel perfect with nothing clipped.
static constexpr uint8_t PORTRAIT_PAIR_SCALE = 1;
static constexpr int PAIR_ART_W =
    NpcPortraits::PORTRAIT_FIRMWARE_WIDTH * PORTRAIT_PAIR_SCALE;
static constexpr int PAIR_ART_H =
    NpcPortraits::PORTRAIT_FIRMWARE_HEIGHT * PORTRAIT_PAIR_SCALE;
static_assert(PAIR_ART_W * 2 == PORTRAIT_W,
              "paired witnesses must tile the plate width with no crop");

// The plate is 136 tall but only its top 76 rows are ever guaranteed visible:
// on RESULT the verdict banner lands at screen y 134 and the grade panel at
// 165, both painted over the portrait. Centring the shrunken art in the full
// plate would have parked both faces under the stamp, so the pair is centred in
// the *safe* band instead — 68 rows of art with an even 4 above and below.
static constexpr int PAIR_SAFE_H = 134 - 58;
static constexpr int PAIR_TOP = (PAIR_SAFE_H - PAIR_ART_H) / 2;
static_assert(PAIR_TOP * 2 + PAIR_ART_H == PAIR_SAFE_H,
              "paired art must sit on even padding inside the safe band");
static_assert(PAIR_TOP + PAIR_ART_H <= PAIR_SAFE_H,
              "paired art must clear the verdict banner");

// The name reads as a dossier caption struck across the base of its own photo.
// A strip below the art would have collided with the banner.
static constexpr int PAIR_NAME_H = 12;
static constexpr int PAIR_NAME_Y = PAIR_TOP + PAIR_ART_H - PAIR_NAME_H;

struct PortraitCache {
    M5Canvas* sprite = nullptr;
    uint8_t portraitId = 0xFF;
    uint16_t themeFg = 0;
    uint16_t themeBg = 0;
    bool ready = false;
};

static PortraitCache portraitCache[2];

static bool preparePortraitSprite(uint8_t portraitId, uint8_t cacheSlot,
                                  uint16_t fg, uint16_t bg);
static void prepareEncounterPortraits(
    const NpcEventsCore::Encounter& encounter, uint16_t fg, uint16_t bg);

static uint32_t randomRange(uint32_t lo, uint32_t hi) {
    if (hi <= lo) return lo;
    return lo + (esp_random() % (hi - lo));
}

void init() {
    if (initialized) return;
    initialized = true;
    phase = Phase::IDLE;
    currentIndex = -1;
    lastIndex = -1;
    selectedChoice = 0;
    openerVariant = 0;
    replyVariant = 0;
    priorChoice = NpcEventsCore::NO_CHOICE;
    memset(lastOpenerVariant, 0xFF, sizeof(lastOpenerVariant));
    memset(lastReplyVariant, 0xFF, sizeof(lastReplyVariant));
    lastStationKey = 0xFF;
    misses = 0;
    seenMask = 0;
    closedCastMask = Config::getNpcClosedCastMask() &
                     NpcEventsCore::ALL_CHARACTER_MASK;
    sessionClosedCastMask = 0;
    choiceLedger = Config::getNpcChoiceLedger();
    casesClosed = 0;
    armAt = 0;
    nextAllowedAt = 0;
    armPending = false;
    cooldownActive = false;
    phaseStartedAt = 0;
    resultStartedAt = 0;
    rewardSummary[0] = '\0';
    rewardReinforcement = 0;
    resultTone = ResultTone::OFFLINE;
    resultTrophyUnlocked = false;
    pendingCoda = false;
    pendingFollowUp = false;
    pendingNode = 0;
    currentNode = 0;
    mailIndex = NO_MAIL;
    codaEnding = NpcEventsCore::CaseEnding::OPEN_CIRCUIT;
}

bool isActive() {
    return phase != Phase::IDLE;
}

// The beat the operator has walked to, or null at the root. A node index that
// has gone stale — content reshuffled under a saved letter — also reads as null
// so the file restarts at the root instead of indexing past the table.
static const NpcEventsCore::CaseNode* activeNode(
    const NpcEventsCore::Encounter& encounter, uint8_t node) {
    if (node == 0 || !encounter.nodes || node > encounter.nodeCount) {
        return nullptr;
    }
    return &encounter.nodes[node - 1];
}

// The active beat's three choices: the root table, or the node's.
static const NpcEventsCore::Choice* activeChoices(
    const NpcEventsCore::Encounter& encounter) {
    const auto* node = activeNode(encounter, currentNode);
    return node ? node->choices : encounter.choices;
}

// Delivery, not interruption. The witness leaves a file and the top bar carries
// the unread count until the operator walks into P1G P0ST on their own terms.
static void fileEncounter(uint8_t room) {
    int picked = NpcEventsCore::pick(esp_random(), room, Config::getLevel(),
                                     ItemDrops::getKHorseTranslationLevel(),
                                     lastIndex, seenMask);
    if (picked < 0) return;
    if (Mailbox::isFull()) return;

    const auto& encounter = NpcEventsCore::get((size_t)picked);
    uint8_t characterIndex = static_cast<uint8_t>(encounter.character);
    uint8_t variant = NpcEventsCore::pickVariant(
        esp_random(), lastOpenerVariant[characterIndex]);

    if (!Mailbox::deliver((uint8_t)picked, variant)) return;

    lastIndex = picked;
    if (picked < MAX_CASE_FILES) seenMask |= (1u << picked);
    lastOpenerVariant[characterIndex] = variant;

    // Arrival cue only. Quieter than the old card slam because nothing is being
    // taken away from the operator — the roam continues.
    SFX::play(SFX::TRANSMISSION_BURST);
    Haptic::tick();
}

bool openFromMail(uint8_t letterIndex) {
    init();
    Mailbox::Letter* letter = Mailbox::at(letterIndex);
    if (!letter) return false;
    if (letter->caseIndex >= NpcEventsCore::count()) return false;

    currentIndex = (int)letter->caseIndex;
    mailIndex = letterIndex;
    currentNode = letter->node;
    const auto& encounter = NpcEventsCore::get((size_t)currentIndex);
    if (!activeNode(encounter, currentNode)) {
        currentNode = 0;  // stale progress; restart the file at the root
    }

    // A filing action should never depend on which random row happened to be
    // selected when the card opened. Variation belongs in dialogue, not input.
    selectedChoice = 0;
    priorChoice = NpcEventsCore::recallChoice(choiceLedger, encounter.character);
    openerVariant = letter->openerVariant;
    if (openerVariant >= NpcEventsCore::DIALOGUE_VARIANTS) openerVariant = 0;
    replyVariant = 0;
    rewardSummary[0] = '\0';
    rewardReinforcement = 0;
    resultTone = ResultTone::OFFLINE;
    resultTrophyUnlocked = false;
    pendingCoda = false;
    pendingFollowUp = false;
    pendingNode = 0;
    // PNG decode and PSRAM allocation happen while the card is opening, before
    // draw() owns the frame. The render path only pushes ready sprites.
    prepareEncounterPortraits(encounter, Display::getColorFG(),
                              Display::getColorBG());
    phase = Phase::BRIEFING;
    phaseStartedAt = millis();
    // Opening a dossier changes the visible unread badge. Persist it now: a
    // power loss before the next choice must not revive a read file as new.
    Mailbox::markRead(letterIndex);
    Mailbox::save();
    SFX::click();
    Haptic::bump();
    return true;
}

void update(uint32_t now, uint8_t room, uint8_t station, bool roamingStable) {
    init();
    if (isActive() || !roamingStable || room >= 5) return;

    uint8_t stationKey = (uint8_t)(room * 16u + (station & 0x0Fu));
    if (stationKey != lastStationKey) {
        lastStationKey = stationKey;
        armPending = false;
        if (cooldownActive && !NpcEventsCore::deadlineReached(now, nextAllowedAt)) return;
        cooldownActive = false;

        // First contact is guaranteed so the system is discoverable. Later
        // arrivals use a rising pity chance, but character/dialogue stay random.
        uint8_t chance = NpcEventsCore::caseArrivalChance(lastIndex >= 0, misses);
        if ((esp_random() % 100) < chance) {
            armAt = now + randomRange(3500, 8000);
            armPending = true;
            misses = 0;
        } else if (misses < 3) {
            ++misses;
        }
    }

    if (armPending && NpcEventsCore::deadlineReached(now, armAt)) {
        armPending = false;
        fileEncounter(room);
        // Delivery is not an encounter, so the roam keeps running. Arm the
        // normal cooldown here instead of waiting for a card to be closed.
        nextAllowedAt = millis() + randomRange(45000, 90000);
        cooldownActive = true;
    }
}

void prevChoice() {
    if (phase != Phase::CHOICE) return;
    selectedChoice = (selectedChoice == 0) ? 2 : (uint8_t)(selectedChoice - 1);
    SFX::click();
    Haptic::tick();
}

void nextChoice() {
    if (phase != Phase::CHOICE) return;
    selectedChoice = (uint8_t)((selectedChoice + 1) % 3);
    SFX::click();
    Haptic::tick();
}

static void applyReward(const NpcEventsCore::Choice& choice) {
    bool session = Config::isSessionActive();
    uint32_t before = Config::getXP();
    int beforeMomentum = Mood::getMomentum();
    bool evidenceEarned = false;

    if (session) {
        Config::addXP(choice.xp, Config::RewardSource::XP_EVENT);
        Mood::addMomentum(choice.momentum);
        if (choice.lootChance > 0 &&
            (esp_random() % 100) < choice.lootChance) {
            evidenceEarned = ItemDrops::awardGuaranteed(
                ItemDrops::ItemDropSource::ENCOUNTER, 2);
        }
    }

    uint32_t actualXP = Config::getXP() - before;
    int actualMomentum = Mood::getMomentum() - beforeMomentum;
    // The grade describes the filed decision, not whether the XP lane is live.
    rewardReinforcement = choice.reinforcement <= 3
                              ? choice.reinforcement
                              : 3;
    if (!session) {
        resultTone = ResultTone::OFFLINE;
    } else if (actualMomentum < 0) {
        resultTone = ResultTone::BACKFIRE;
    } else if (evidenceEarned) {
        resultTone = ResultTone::JACKPOT;
    } else if (rewardReinforcement >= 2) {
        resultTone = ResultTone::WIN;
    } else {
        resultTone = ResultTone::CHAOS;
    }
    if (!session) {
        snprintf(rewardSummary, sizeof(rewardSummary), "CASE FILED // SESSION OFF");
    } else if (evidenceEarned) {
        snprintf(rewardSummary, sizeof(rewardSummary), "+%lu XP // %+d MOOD // EVIDENCE",
                 (unsigned long)actualXP, actualMomentum);
    } else {
        snprintf(rewardSummary, sizeof(rewardSummary), "+%lu XP // %+d MOOD",
                 (unsigned long)actualXP, actualMomentum);
    }
}

static uint8_t bitCount(uint32_t mask) {
    uint8_t count = 0;
    while (mask) {
        count += (uint8_t)(mask & 1u);
        mask >>= 1;
    }
    return count;
}

uint8_t getCasesClosed() {
    init();
    return casesClosed;
}

uint8_t getClosedCharacterCount() {
    init();
    return bitCount(closedCastMask & NpcEventsCore::ALL_CHARACTER_MASK);
}

uint8_t getCharacterCount() {
    return NpcEventsCore::CHARACTER_COUNT;
}

struct ClosureResult {
    bool trophyUnlocked;
    bool codaPending;
};

static ClosureResult recordCaseClosed() {
    if (casesClosed < 255) ++casesClosed;
    if (currentIndex >= 0 && (size_t)currentIndex < NpcEventsCore::count()) {
        const auto& encounter = NpcEventsCore::get((size_t)currentIndex);
        choiceLedger = NpcEventsCore::rememberChoice(
            choiceLedger, encounter.character, selectedChoice);
        closedCastMask = NpcEventsCore::markCharacterClosed(
            closedCastMask, encounter.character);
        sessionClosedCastMask = NpcEventsCore::markCharacterClosed(
            sessionClosedCastMask, encounter.character);
        Config::setNpcChoiceLedger(choiceLedger);
        Config::setNpcClosedCastMask(closedCastMask);
    }

    // Passive casework can fill a challenge meter, but its payout still waits
    // for the normal active-session settlement contract.
    Challenges::onCaseClosed(casesClosed);
    bool trophyUnlocked = Achievements::tryUnlock(Achievement::FIRST_CASE);
    if (NpcEventsCore::hasClosedWholeCast(sessionClosedCastMask)) {
        trophyUnlocked = Achievements::tryUnlock(Achievement::ROGUES_GALLERY) ||
                          trophyUnlocked;
    }
    bool codaPending = NpcEventsCore::shouldOfferCoda(
        closedCastMask, Config::getNpcCodaSeen());
    if (codaPending) {
        codaEnding = NpcEventsCore::endingFromScore(
            NpcEventsCore::scoreChoiceLedger(choiceLedger, closedCastMask));
    }
    return {trophyUnlocked, codaPending};
}

// `filed` distinguishes a finished case from a bail-out. A filed case leaves
// the inbox; a bail keeps the letter so the operator can come back to it.
static void closeEncounter(uint32_t cooldownLo, uint32_t cooldownHi,
                           bool filed) {
    if (mailIndex != NO_MAIL) {
        if (filed) {
            Mailbox::discard(mailIndex);
        } else {
            Mailbox::setProgress(mailIndex, currentNode);
        }
        Mailbox::save();
    }
    phase = Phase::IDLE;
    currentIndex = -1;
    currentNode = 0;
    mailIndex = NO_MAIL;
    priorChoice = NpcEventsCore::NO_CHOICE;
    pendingCoda = false;
    nextAllowedAt = millis() + randomRange(cooldownLo, cooldownHi);
    cooldownActive = true;
    Haptic::tick();
}

void select() {
    if (phase == Phase::BRIEFING) {
        phase = Phase::CHOICE;
        SFX::click();
        Haptic::tick();
        return;
    }

    if (phase == Phase::CHOICE && currentIndex >= 0) {
        const auto& encounter = NpcEventsCore::get((size_t)currentIndex);
        const NpcEventsCore::Choice* table = activeChoices(encounter);
        const auto& choice = table[selectedChoice];
        uint8_t characterIndex = static_cast<uint8_t>(encounter.character);
        replyVariant = NpcEventsCore::pickVariant(
            esp_random(), lastReplyVariant[characterIndex][selectedChoice]);
        // Follow-up beats only author FOLLOWUP_VARIANTS lines. Without this the
        // upper variants would all collapse onto reply[0] and read as a repeat.
        if (currentNode > 0) {
            replyVariant = (uint8_t)(replyVariant %
                                     NpcEventsCore::FOLLOWUP_VARIANTS);
        }
        lastReplyVariant[characterIndex][selectedChoice] = replyVariant;
        applyReward(choice);
        phase = Phase::RESULT;
        resultStartedAt = millis();
        switch (resultTone) {
            case ResultTone::JACKPOT:
                SFX::play(SFX::ACHIEVEMENT_UNLOCK);
                Haptic::doubleTap();
                break;
            case ResultTone::WIN:
                SFX::play(SFX::CHALLENGE_COMPLETE);
                Haptic::pulse();
                break;
            case ResultTone::CHAOS:
                SFX::play(SFX::CUTE_JUMP);
                Haptic::bump();
                break;
            case ResultTone::BACKFIRE:
                SFX::play(SFX::ERROR);
                Haptic::buzz();
                break;
            case ResultTone::OFFLINE:
                SFX::play(SFX::ERROR);
                Haptic::tick();
                break;
        }
        // A choice with a follow-up has not closed anything yet. Only settle the
        // ledger, trophies and coda when this decision actually ends the file.
        pendingFollowUp = NpcEventsCore::followUp(encounter, choice) != nullptr;
        if (pendingFollowUp) {
            pendingNode = choice.nextNode;
            resultTrophyUnlocked = false;
            pendingCoda = false;
            if (mailIndex != NO_MAIL) {
                Mailbox::setProgress(mailIndex, currentNode);
                Mailbox::save();
            }
            return;
        }

        // Let a simultaneous challenge completion own the final celebration
        // sound/haptic instead of being immediately overwritten by the case tone.
        ClosureResult closure = recordCaseClosed();
        resultTrophyUnlocked = closure.trophyUnlocked;
        pendingCoda = closure.codaPending;
        if (!Config::isSessionActive()) {
            snprintf(rewardSummary, sizeof(rewardSummary), "%s",
                     closure.trophyUnlocked ? "CASE FILED // TROPHY QUEUED"
                                            : "CASE FILED // NO CASE XP");
        }
        return;
    }

    if (phase == Phase::RESULT) {
        if (pendingFollowUp) {
            // Walk one beat deeper. The witness answers, then asks again.
            currentNode = pendingNode;
            pendingFollowUp = false;
            selectedChoice = 0;
            phase = Phase::BRIEFING;
            phaseStartedAt = millis();
            if (mailIndex != NO_MAIL) {
                Mailbox::setProgress(mailIndex, currentNode);
                Mailbox::save();
            }
            SFX::click();
            Haptic::tick();
            return;
        }
        if (pendingCoda) {
            phase = Phase::CODA;
            Config::setNpcCodaSeen(true);
            SFX::play(SFX::ACHIEVEMENT_UNLOCK);
            Haptic::doubleTap();
        } else {
            closeEncounter(60000, 110000, true);
        }
        return;
    }

    if (phase == Phase::CODA) {
        closeEncounter(60000, 110000, true);
    }
}

void cancel() {
    // Bail is a pre-filing action. Once the result owns the card, ignoring the
    // generic back gesture protects the filed consequence and one-time coda.
    if (phase != Phase::BRIEFING && phase != Phase::CHOICE) return;
    // The letter survives a bail, parked at the beat the operator reached.
    closeEncounter(30000, 60000, false);
}

void dismiss() {
    if (phase == Phase::IDLE) return;
    closeEncounter(30000, 60000, false);
}

static int drawWrapped(M5Canvas& canvas, const char* text, int x, int y,
                       uint8_t maxChars, uint8_t maxLines, uint16_t color) {
    if (!text || !text[0]) return y;
    canvas.setTextDatum(TL_DATUM);
    canvas.setTextSize(1);
    canvas.setTextColor(color);

    char line[48] = "";
    uint8_t lineLen = 0;
    uint8_t lines = 0;
    const char* p = text;
    while (*p && lines < maxLines) {
        while (*p == ' ') ++p;
        const char* start = p;
        while (*p && *p != ' ') ++p;
        uint8_t wordLen = (uint8_t)(p - start);
        if (wordLen == 0) break;

        if (lineLen > 0 && lineLen + 1 + wordLen > maxChars) {
            canvas.drawString(line, x, y);
            y += 9;
            ++lines;
            line[0] = '\0';
            lineLen = 0;
            if (lines >= maxLines) break;
        }

        if (lineLen > 0 && lineLen + 1 < sizeof(line)) {
            line[lineLen++] = ' ';
        }
        uint8_t copyLen = wordLen;
        if (lineLen + copyLen >= sizeof(line)) copyLen = sizeof(line) - lineLen - 1;
        memcpy(line + lineLen, start, copyLen);
        lineLen += copyLen;
        line[lineLen] = '\0';
    }
    if (lineLen > 0 && lines < maxLines) {
        canvas.drawString(line, x, y);
        y += 9;
    }
    return y;
}

static bool preparePortraitSprite(uint8_t portraitId, uint8_t cacheSlot,
                                  uint16_t fg, uint16_t bg) {
    if (cacheSlot >= 2) return false;
    PortraitCache& cache = portraitCache[cacheSlot];
    const NpcPortraits::PortraitPng* png = NpcPortraits::get(portraitId);
    if (!NpcPortraits::hasPixelArtContract(png)) return false;
    if (cache.ready && cache.portraitId == portraitId &&
        cache.themeFg == fg && cache.themeBg == bg) {
        return true;
    }

    if (!cache.sprite) {
        cache.sprite = new M5Canvas(&M5.Display);
        if (!cache.sprite) return false;
        cache.sprite->setPsram(true);
        cache.sprite->setColorDepth(16);
        if (!cache.sprite->createSprite(72, 68)) {
            delete cache.sprite;
            cache.sprite = nullptr;
            return false;
        }
    }

    cache.sprite->fillSprite(PORTRAIT_TRANSPARENT_KEY);
    cache.ready = cache.sprite->drawPng(png->data, png->length, 0, 0);
    if (cache.ready) {
        // The witnesses are painted in a four-step sepia ramp, so the full
        // two-color map used to delete the painting. A light wash keeps their
        // own palette and still seats them in the live theme.
        Display::themeTintSprite(*cache.sprite, PORTRAIT_TRANSPARENT_KEY,
                                 PORTRAIT_THEME_TINT);
    }
    cache.portraitId = portraitId;
    cache.themeFg = fg;
    cache.themeBg = bg;
    return cache.ready;
}

static void prepareEncounterPortraits(
    const NpcEventsCore::Encounter& encounter, uint16_t fg, uint16_t bg) {
    if (encounter.character == NpcEventsCore::Character::RASTA_HOOLIGAN) {
        preparePortraitSprite(NpcPortraits::RASTA, 0, fg, bg);
        preparePortraitSprite(NpcPortraits::HOOLIGAN, 1, fg, bg);
        return;
    }
    preparePortraitSprite((uint8_t)encounter.character, 0, fg, bg);
}

// The cached sprite, but only when it holds this portrait under the live theme.
// Null means the caller must fall back to the vector portrait.
static M5Canvas* readyPortrait(uint8_t portraitId, uint8_t cacheSlot,
                               uint16_t fg, uint16_t bg) {
    if (cacheSlot >= 2) return nullptr;
    PortraitCache& cache = portraitCache[cacheSlot];
    if (!cache.ready || cache.portraitId != portraitId ||
        cache.themeFg != fg || cache.themeBg != bg || !cache.sprite) {
        return nullptr;
    }
    return cache.sprite;
}

static bool pushPortraitArt(M5Canvas& canvas, uint8_t portraitId,
                            uint8_t cacheSlot, int centerX, int centerY,
                            uint16_t fg, uint16_t bg) {
    M5Canvas* sprite = readyPortrait(portraitId, cacheSlot, fg, bg);
    if (!sprite) return false;
    sprite->setPivot(NpcPortraits::PORTRAIT_FIRMWARE_WIDTH / 2,
                     NpcPortraits::PORTRAIT_FIRMWARE_HEIGHT / 2);
    // The non-AA path is deliberate: every source pixel becomes an exact 2x2
    // block instead of being softened back into an illustration.
    sprite->pushRotateZoom(&canvas, centerX, centerY, 0.0f,
                           (float)PORTRAIT_DISPLAY_SCALE,
                           (float)PORTRAIT_DISPLAY_SCALE,
                           PORTRAIT_TRANSPARENT_KEY);
    return true;
}

static void drawPortrait(M5Canvas& canvas, NpcEventsCore::Character who,
                         uint8_t portraitId, uint8_t cacheSlot,
                         int x, int y, int w, int h, uint16_t fg, uint16_t bg) {
    uint16_t dim = Display::lerpColor565(fg, bg, 0.45f);
    canvas.fillRoundRect(x, y, w, h, 4, bg);
    canvas.drawRoundRect(x, y, w, h, 4, dim);

    if (w == PORTRAIT_W && h == PORTRAIT_H &&
        pushPortraitArt(canvas, portraitId, cacheSlot,
                        x + w / 2, y + h / 2, fg, bg)) {
        return;
    }

    int cx = x + w / 2;
    int cy = y + h / 2;

    switch (who) {
        case NpcEventsCore::Character::K_HORSE:
            canvas.drawTriangle(cx - 19, cy - 16, cx + 10, cy - 21, cx + 20, cy + 3, fg);
            canvas.drawCircle(cx + 12, cy - 7, 2, fg);
            canvas.drawLine(cx - 15, cy + 2, cx - 20, cy + 20, fg);
            canvas.drawCircle(cx - 5, cy + 18, 13, dim);
            canvas.drawCircle(cx - 5, cy + 18, 7, fg);
            break;
        case NpcEventsCore::Character::WISE_PIG:
            canvas.drawCircle(cx, cy - 5, 21, fg);
            canvas.drawTriangle(cx - 18, cy - 20, cx - 11, cy - 32, cx - 5, cy - 18, fg);
            canvas.drawTriangle(cx + 18, cy - 20, cx + 11, cy - 32, cx + 5, cy - 18, fg);
            canvas.drawRect(cx - 10, cy - 2, 20, 10, bg);
            canvas.drawLine(cx - 15, cy + 13, cx, cy + 27, dim);
            canvas.drawLine(cx + 15, cy + 13, cx, cy + 27, dim);
            break;
        case NpcEventsCore::Character::COW:
            canvas.drawRoundRect(cx - 22, cy - 19, 44, 42, 8, fg);
            canvas.drawTriangle(cx - 20, cy - 15, cx - 33, cy - 25, cx - 18, cy - 5, fg);
            canvas.drawTriangle(cx + 20, cy - 15, cx + 33, cy - 25, cx + 18, cy - 5, fg);
            canvas.fillCircle(cx - 8, cy - 3, 4, dim);
            canvas.drawRoundRect(cx - 12, cy + 8, 24, 10, 4, fg);
            break;
        case NpcEventsCore::Character::DR_OCULUS:
            canvas.drawCircle(cx, cy, 25, fg);
            canvas.drawCircle(cx - 12, cy - 4, 11, fg);
            canvas.drawCircle(cx + 12, cy - 4, 11, fg);
            canvas.drawLine(cx - 1, cy - 4, cx + 1, cy - 4, fg);
            canvas.fillCircle(cx - 12, cy - 4, 3, dim);
            canvas.fillCircle(cx + 12, cy - 4, 3, dim);
            break;
        case NpcEventsCore::Character::RASTA_HOOLIGAN:
            canvas.drawCircle(cx, cy, 20, portraitId == NpcPortraits::RASTA ? fg : dim);
            canvas.drawString(portraitId == NpcPortraits::RASTA ? "R" : "H",
                              cx - 4, cy - 4);
            break;
        case NpcEventsCore::Character::BARMAN:
            canvas.drawCircle(cx, cy - 2, 23, fg);
            canvas.drawCircle(cx - 9, cy - 7, 8, bg);
            canvas.drawCircle(cx + 9, cy - 7, 8, bg);
            canvas.fillCircle(cx - 9, cy - 7, 3, dim);
            canvas.fillCircle(cx + 9, cy - 7, 3, dim);
            canvas.drawRoundRect(cx - 10, cy + 5, 20, 9, 3, bg);
            break;
    }

    canvas.setTextDatum(TL_DATUM);
}

// 1:1 blit. pushRotateZoom at scale 1 would resample around a pivot for no
// reason; pushSprite lands each source pixel on exactly one panel pixel.
static bool pushPortraitArt1x(M5Canvas& canvas, uint8_t portraitId,
                              uint8_t cacheSlot, int x, int y,
                              uint16_t fg, uint16_t bg) {
    M5Canvas* sprite = readyPortrait(portraitId, cacheSlot, fg, bg);
    if (!sprite) return false;
    sprite->pushSprite(&canvas, x, y, PORTRAIT_TRANSPARENT_KEY);
    return true;
}

static void drawPairedPortraits(M5Canvas& canvas, int x, int y,
                                uint16_t fg, uint16_t bg) {
    uint16_t dim = Display::lerpColor565(fg, bg, 0.45f);
    canvas.fillRoundRect(x, y, PORTRAIT_W, PORTRAIT_H, 4, bg);

    // Two whole 72px grids tile the 144px plate, so neither witness is cropped
    // and no divider is needed to excuse a missing ear — the captions carry the
    // separation instead.
    const int artY = y + PAIR_TOP;
    bool rastaReady = pushPortraitArt1x(canvas, NpcPortraits::RASTA, 0,
                                        x, artY, fg, bg);
    bool hoolReady = pushPortraitArt1x(canvas, NpcPortraits::HOOLIGAN, 1,
                                       x + PAIR_ART_W, artY, fg, bg);

    if (!rastaReady || !hoolReady) {
        canvas.setTextSize(2);
        canvas.setTextDatum(MC_DATUM);
        canvas.setTextColor(fg);
        if (!rastaReady) canvas.drawString("D", x + PAIR_ART_W / 2,
                                           artY + PAIR_ART_H / 2);
        if (!hoolReady) canvas.drawString("O", x + PAIR_ART_W * 3 / 2,
                                          artY + PAIR_ART_H / 2);
        canvas.setTextDatum(TL_DATUM);
        canvas.setTextSize(1);
    }

    // Caption struck across the base of each photo, inset 4px so the label sits
    // inside its own frame rather than running the full plate width.
    const int nameY = y + PAIR_NAME_Y;
    canvas.fillRect(x + 4, nameY, PAIR_ART_W - 8, PAIR_NAME_H, bg);
    canvas.fillRect(x + PAIR_ART_W + 4, nameY, PAIR_ART_W - 8, PAIR_NAME_H, bg);
    canvas.setTextSize(1);
    canvas.setTextColor(dim);
    canvas.setTextDatum(MC_DATUM);
    canvas.drawString("D0H4M", x + PAIR_ART_W / 2, nameY + PAIR_NAME_H / 2);
    canvas.drawString("01NK5", x + PAIR_ART_W * 3 / 2, nameY + PAIR_NAME_H / 2);
    canvas.setTextDatum(TL_DATUM);

    // Border last: the art tiles the plate edge to edge, so drawing the frame
    // first would let 01NK5's rightmost column overwrite it.
    canvas.drawRoundRect(x, y, PORTRAIT_W, PORTRAIT_H, 4, dim);
}

static int slidePortraitX(int from, int to) {
    uint32_t elapsed = millis() - phaseStartedAt;
    if (elapsed >= PORTRAIT_SLIDE_MS) return to;
    uint32_t eased = elapsed * (2 * PORTRAIT_SLIDE_MS - elapsed);
    uint32_t scale = PORTRAIT_SLIDE_MS * PORTRAIT_SLIDE_MS;
    return from + (int)(((int32_t)(to - from) * (int32_t)eased) /
                        (int32_t)scale);
}

// Two text rows in an even-height plate: 4px pad, 8px ink, 4px gutter,
// 8px ink, 4px pad. Shared by the grade panel and the coda verdict plate so
// both dossier plates sit on the same baseline grid.
static constexpr int PANEL_H = 28;
static constexpr int PANEL_ROW1 = 4;
static constexpr int PANEL_ROW2 = 16;
static constexpr int PANEL_Y = 165;

static void drawReinforcement(M5Canvas& canvas, uint8_t strength,
                              uint16_t fg, uint16_t bg, uint16_t dim,
                              int x, int y, int w) {
    static const char* LABELS[] = {"N0N3", "L0W", "M3D", "H1GH"};
    if (strength > 3) strength = 3;
    canvas.fillRect(x, y, w, PANEL_H, bg);
    canvas.drawRect(x, y, w, PANEL_H, dim);
    canvas.setTextDatum(TL_DATUM);
    canvas.setTextSize(1);
    canvas.setTextColor(dim);
    canvas.drawString("GR4D3", x + 5, y + PANEL_ROW1);
    for (uint8_t i = 0; i < 3; ++i) {
        int barX = x + 45 + i * 17;
        canvas.drawRect(barX, y + PANEL_ROW1, 13, 7, dim);
        if (i < strength) canvas.fillRect(barX + 2, y + PANEL_ROW1 + 2, 9, 3, fg);
    }
    canvas.setTextColor(strength > 0 ? fg : dim);
    canvas.drawString(LABELS[strength], x + 100, y + PANEL_ROW1);
    char caseProgress[28];
    snprintf(caseProgress, sizeof(caseProgress), "RUN %u/%u // L1F3 %u/%u",
             bitCount(sessionClosedCastMask),
             (unsigned)NpcEventsCore::CHARACTER_COUNT,
             bitCount(closedCastMask),
             (unsigned)NpcEventsCore::CHARACTER_COUNT);
    canvas.setTextColor(dim);
    canvas.drawString(caseProgress, x + 5, y + PANEL_ROW2);
    canvas.setTextDatum(TL_DATUM);
    canvas.setTextColor(fg);
}

static void drawResultBanner(M5Canvas& canvas, uint16_t fg, uint16_t bg,
                             uint16_t dim, int baseX, int y, int w) {
    constexpr int h = 28;
    uint32_t elapsed = millis() - resultStartedAt;
    int x = baseX;

    // Verdict hits the file like a rubber stamp. Bad news jitters instead.
    if (elapsed < 180) x -= (int)((180 - elapsed) * 36 / 180);
    if (resultTone == ResultTone::BACKFIRE && elapsed < 700) {
        x += ((elapsed / 45) & 1u) ? 2 : -2;
    }

    const char* verdict = "SESSION OFF";
    const char* tag = "NO CASE XP";
    bool filled = false;
    // Mid-tree the case is not filed and saying so would be a fake reward. The
    // payout is real; the closure is not.
    if (pendingFollowUp) {
        canvas.fillRoundRect(x, y, w, h, 3, bg);
        canvas.drawRoundRect(x, y, w, h, 3, dim);
        canvas.setTextSize(1);
        canvas.setTextDatum(TL_DATUM);
        canvas.setTextColor(fg);
        canvas.drawString("THREAD HOLDS", x + 10, y + 4);
        canvas.setTextColor(dim);
        canvas.drawString("CASE STILL OPEN", x + 10, y + 16);
        return;
    }
    switch (resultTone) {
        case ResultTone::JACKPOT:
            verdict = "EVIDENCE JACKPOT";
            // Guaranteed case evidence may be queued now or deferred safely.
            tag = "EVIDENCE SECURED";
            filled = true;
            break;
        case ResultTone::WIN:
            verdict = "CASE CRACKED";
            tag = "CLEAN PAYOUT";
            filled = true;
            break;
        case ResultTone::CHAOS:
            verdict = "CHAOS WIN";
            tag = "DIRTY PAYOUT";
            break;
        case ResultTone::BACKFIRE:
            verdict = "CASE BITES BACK";
            tag = "MOOD DAMAGE";
            break;
        case ResultTone::OFFLINE:
            if (resultTrophyUnlocked) tag = "TROPHY QUEUED";
            break;
    }

    if (filled) {
        canvas.fillRoundRect(x, y, w, h, 3, fg);
        if (elapsed < 900 && ((elapsed / 90) & 1u) == 0) {
            canvas.drawRoundRect(x - 2, y - 2, w + 4, h + 4, 4, fg);
        }
    } else {
        uint16_t edge = resultTone == ResultTone::OFFLINE ? dim : fg;
        // The plate lands on top of the portrait, so it has to carry its own
        // paper. Without this the verdict reads over the pig's face.
        canvas.fillRoundRect(x, y, w, h, 3, bg);
        canvas.drawRoundRect(x, y, w, h, 3, edge);
        if (resultTone == ResultTone::BACKFIRE) {
            canvas.drawRoundRect(x + 2, y + 2, w - 4, h - 4, 2, edge);
            canvas.setTextSize(1);
            canvas.setTextDatum(TL_DATUM);
            canvas.setTextColor(edge);
            // Tucked into the 4px margin either side; at x+7 the left mark
            // bled into the first glyph of the verdict.
            canvas.drawString("X", x + 4, y + 10);
            canvas.drawString("X", x + w - 9, y + 10);
        } else if (resultTone == ResultTone::CHAOS) {
            // Inset past the corner radius so the spine does not square off
            // the rounded edge it sits inside.
            canvas.fillRect(x + 1, y + 3, 7, h - 6, fg);
        }
    }

    canvas.setTextSize(1);
    canvas.setTextDatum(TL_DATUM);
    uint16_t labelColor = resultTone == ResultTone::OFFLINE ? dim : fg;
    canvas.setTextColor(filled ? bg : labelColor);
    canvas.drawString(verdict, x + 10, y + 4);
    canvas.setTextColor(filled ? bg : dim);
    canvas.drawString(tag, x + 10, y + 16);
    canvas.setTextDatum(TL_DATUM);
}

static const char* phaseLabel(Phase value) {
    switch (value) {
        case Phase::BRIEFING: return "BRIEF";
        case Phase::CHOICE: return "DECIDE";
        case Phase::RESULT: return "FILED";
        case Phase::CODA: return "CODA";
        case Phase::IDLE: break;
    }
    return "CASE";
}

static void drawPhaseBadge(M5Canvas& canvas, Phase value, uint16_t fg,
                           uint16_t bg, int rightX, int y) {
    const char* label = phaseLabel(value);
    const int w = (int)strlen(label) * 6 + 10;
    constexpr int h = 14;
    canvas.fillRoundRect(rightX - w, y, w, h, 3, fg);
    canvas.setTextDatum(MC_DATUM);
    canvas.setTextSize(1);
    canvas.setTextColor(bg);
    canvas.drawString(label, rightX - w / 2, y + h / 2);
    canvas.setTextDatum(TL_DATUM);
}

void draw(M5Canvas& canvas) {
    if (!isActive() || currentIndex < 0) return;
    const auto& encounter = NpcEventsCore::get((size_t)currentIndex);
    uint16_t fg = Display::getColorFG();
    uint16_t bg = Display::getColorBG();
    uint16_t dim = Display::lerpColor565(fg, bg, 0.48f);

    constexpr int boxX = 6;
    constexpr int boxY = 18;
    constexpr int boxW = 308;
    constexpr int boxH = 204;
    constexpr int portraitX = 12;
    constexpr int portraitY = 58;
    constexpr int copyX = 164;
    constexpr int copyChars = 23;
    constexpr int copyW = 142;
    canvas.fillRoundRect(boxX, boxY, boxW, boxH, 6, bg);
    canvas.drawRoundRect(boxX, boxY, boxW, boxH, 6, fg);
    canvas.drawLine(boxX + 1, 52, boxX + boxW - 2, 52, dim);

    canvas.setTextDatum(TL_DATUM);
    canvas.setTextSize(2);
    canvas.setTextColor(fg);
    canvas.drawString(phase == Phase::CODA ? "C4S3 C0D4"
                                           : encounter.name,
                      13, 24);
    drawPhaseBadge(canvas, phase, fg, bg, 306, 22);
    canvas.setTextSize(1);
    canvas.setTextColor(dim);
    canvas.setTextDatum(TR_DATUM);
    canvas.drawString(phase == Phase::CODA ? "CAST CONSEQUENCE"
                                           : encounter.tag,
                      306, 41);
    canvas.setTextDatum(TL_DATUM);

    const bool pairedPortraits =
        encounter.character == NpcEventsCore::Character::RASTA_HOOLIGAN;
    if (pairedPortraits) {
        drawPairedPortraits(canvas, slidePortraitX(-PORTRAIT_W, portraitX),
                            portraitY, fg, bg);
    } else {
        drawPortrait(canvas, encounter.character, (uint8_t)encounter.character, 0,
                     slidePortraitX(-PORTRAIT_W, portraitX), portraitY,
                     PORTRAIT_W, PORTRAIT_H, fg, bg);
    }

    if (phase == Phase::BRIEFING) {
        int y = 58;
        if (currentNode > 0) {
            // Deeper beat: the file is already open, so the prior-file callback
            // and the overheard caption would both be re-litigating the opener.
            const auto* node = activeNode(encounter, currentNode);
            canvas.setTextColor(dim);
            canvas.drawString("THE FILE CONTINUES", copyX, y);
            y += 10;
            canvas.setTextColor(fg);
            if (node) {
                drawWrapped(canvas,
                            NpcEventsCore::promptText(*node, replyVariant),
                            copyX, y, copyChars, 9, fg);
            }
        } else {
            if (priorChoice < 3) {
                canvas.setTextColor(dim);
                canvas.drawString("PRIOR FILE // THREAD", copyX, y);
                y += 10;
                y = drawWrapped(canvas, encounter.choices[priorChoice].thread,
                                copyX, y, copyChars, 3, fg);
                y += 3;
            }
            if (encounter.caption[openerVariant]) {
                canvas.setTextColor(dim);
                canvas.drawString("OVERHEARD", copyX, y);
                y += 10;
                y = drawWrapped(canvas, encounter.caption[openerVariant],
                                copyX, y, copyChars, 3, dim);
                y += 3;
            }
            canvas.setTextColor(fg);
            drawWrapped(canvas, encounter.opener[openerVariant], copyX, y,
                        copyChars, 7, fg);
        }
        canvas.setTextColor(dim);
        canvas.drawString("[B] OPTIONS", copyX, 204);
        canvas.setTextDatum(TR_DATUM);
        canvas.drawString("[C+] BAIL", 306, 204);
        canvas.setTextDatum(TL_DATUM);
    } else if (phase == Phase::CHOICE) {
        const NpcEventsCore::Choice* table = activeChoices(encounter);
        canvas.setTextColor(dim);
        canvas.drawString("PANCETTA'S MOVE", copyX, 58);
        for (uint8_t i = 0; i < 3; ++i) {
            int rowY = 70 + i * 21;
            bool selected = i == selectedChoice;
            uint16_t rowFg = selected ? bg : fg;
            if (selected) canvas.fillRoundRect(copyX - 3, rowY,
                                               copyW + 3, 18, 3, fg);
            canvas.setTextColor(rowFg);
            canvas.setTextSize(1);
            canvas.setTextDatum(TL_DATUM);
            canvas.drawString(table[i].label, copyX + 2, rowY + 5);
        }

        const auto& choice = table[selectedChoice];
        canvas.drawLine(copyX, 135, 306, 135, dim);
        canvas.setTextColor(dim);
        char threadLabel[24];
        snprintf(threadLabel, sizeof(threadLabel), "WIRE IF FILED // %+d",
                 choice.threadImpact);
        canvas.drawString(threadLabel, copyX, 140);
        drawWrapped(canvas, choice.thread, copyX, 151,
                    copyChars, 4, fg);
        canvas.setTextColor(dim);
        canvas.drawString("A/C PICK  B FILE  C+OUT", copyX, 204);
    } else if (phase == Phase::RESULT) {
        const NpcEventsCore::Choice* table = activeChoices(encounter);
        const auto& choice = table[selectedChoice];
        canvas.setTextColor(dim);
        canvas.drawString(choice.label, copyX, 58);
        int y = drawWrapped(canvas,
                            NpcEventsCore::replyText(choice, replyVariant),
                            copyX, 70, copyChars, 7, fg);
        if (y < 134) y = 134;
        canvas.drawLine(copyX, y, 306, y, dim);
        y += 5;
        char threadLabel[24];
        snprintf(threadLabel, sizeof(threadLabel), "WIRE FILED // %+d",
                 choice.threadImpact);
        canvas.setTextColor(dim);
        canvas.drawString(threadLabel, copyX, y);
        y += 10;
        y = drawWrapped(canvas, choice.thread, copyX, y,
                        copyChars, 3, fg);
        if (y < 181) y = 181;
        canvas.setTextColor(resultTone == ResultTone::BACKFIRE ? dim : fg);
        drawWrapped(canvas, rewardSummary, copyX, y, copyChars, 2,
                    resultTone == ResultTone::BACKFIRE ? dim : fg);

        // The stamp slides in from off-card; clip it to the card interior so
        // the entry animation cannot paint over the case-card border.
        canvas.setClipRect(boxX + 1, boxY + 1, boxW - 2, boxH - 2);
        drawResultBanner(canvas, fg, bg, dim, portraitX + 4, 134,
                         PORTRAIT_W - 8);
        canvas.clearClipRect();
        drawReinforcement(canvas, rewardReinforcement, fg, bg, dim,
                          portraitX + 4, PANEL_Y, PORTRAIT_W - 8);
        canvas.setTextSize(1);
        canvas.setTextColor(dim);
        const char* resultHint = "[B] RETURN TO THE 2.4GHZ BEAT";
        if (pendingFollowUp) resultHint = "[B] THE WITNESS ISN'T DONE";
        else if (pendingCoda) resultHint = "[B] READ CAST CONSEQUENCE";
        canvas.drawString(resultHint, 14, 207);
    } else if (phase == Phase::CODA) {
        int8_t score = NpcEventsCore::scoreChoiceLedger(choiceLedger,
                                                        closedCastMask);
        canvas.setTextColor(dim);
        canvas.drawString("SIX WITNESSES // ONE WIRE", copyX, 58);
        canvas.setTextColor(fg);
        canvas.setTextSize(2);
        canvas.drawString(NpcEventsCore::endingTitle(codaEnding), copyX, 71);
        canvas.setTextSize(1);
        drawWrapped(canvas, NpcEventsCore::endingText(codaEnding), copyX, 91,
                    copyChars, 9, fg);
        canvas.drawLine(copyX, 176, 306, 176, dim);
        char scoreLine[24];
        snprintf(scoreLine, sizeof(scoreLine), "WIRE SCORE %+d // FILED",
                 score);
        canvas.setTextColor(dim);
        canvas.drawString(scoreLine, copyX, 183);
        canvas.fillRect(portraitX + 4, PANEL_Y, PORTRAIT_W - 8, PANEL_H, bg);
        canvas.drawRect(portraitX + 4, PANEL_Y, PORTRAIT_W - 8, PANEL_H, dim);
        canvas.setTextColor(dim);
        canvas.drawString("L1F3 C4ST // 6/6", portraitX + 9,
                          PANEL_Y + PANEL_ROW1);
        canvas.setTextColor(fg);
        canvas.drawString(NpcEventsCore::endingTitle(codaEnding),
                          portraitX + 9, PANEL_Y + PANEL_ROW2);
        canvas.setTextColor(dim);
        canvas.drawString("[B] CLOSE THE FILE", copyX, 204);
    }

    canvas.setTextDatum(TL_DATUM);
    canvas.setTextSize(1);
    canvas.setTextColor(fg);
}

}  // namespace NpcEvents
