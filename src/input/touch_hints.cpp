/**
 * TouchHints - gesture discovery system
 *
 * ==[ FINGER SCHOOL ]== two layers:
 * 1. speech bubble via Mood::queuePhrase()
 * 2. blinking bottom bar labels (4s window, 300ms toggle)
 *
 * First entry: always show. Repeat: every 5th visit.
 * NVS bitmask tracks which modes have been seen.
 */

#include "../hal/platform.h"

#if HAMLET_HAS_TOUCH

#include "touch_hints.h"
#include "../core/config.h"
#include "../modes/ble_scanner.h"
#include "../modes/spectrum.h"
#include "../piglet/mood.h"
#include "../ui/menu.h"
#include "../ui/menu_pig.h"

namespace TouchHints {

// ==[ TIMING ]==
static const uint32_t HINT_DURATION_MS = 4000;  // 4s hint window
static const uint32_t BLINK_INTERVAL_MS = 300;  // toggle every 300ms

// ==[ STATE ]==
static bool hintActive = false;
static uint32_t hintStartTime = 0;
static bool blinkOn = true;
static uint32_t lastBlinkTime = 0;
static HamletMode currentHintMode = HamletMode::IDLE;

// ==[ VISIT COUNTERS ]== RAM only, reset on reboot
static constexpr uint8_t MODE_COUNT = HAMLET_MODE_COUNT;
static uint8_t visitCount[MODE_COUNT] = {0};
static_assert(MODE_COUNT <= 32, "TouchHints seen mask needs widening");

// ==[ NVS SEEN MASK ]== bit per mode, persisted
static uint32_t seenMask = 0;

// ==[ MODE INDEX ]==
static uint8_t modeIndex(HamletMode mode) {
    return static_cast<uint8_t>(mode);
}

// ==[ PHRASE TABLES ]==
// ~40 chars max per phrase. first entry = first-time phrase.

static const char* phraseIDLE[] = {
    "[A][C] themes. [B] menu.",
    "snout accepts evidence. swipe themes.",
    "hold top strip: lock. swipe up: free."
};
static constexpr uint8_t phraseIDLE_N = sizeof(phraseIDLE) / sizeof(phraseIDLE[0]);

static const char* phraseMENU[] = {
    "[B] opens drawers and tools.",
    "[A][C] browse. [C+] goes back.",
    "six drawers. fourteen tools. one hub."
};
static constexpr uint8_t phraseMENU_N = sizeof(phraseMENU) / sizeof(phraseMENU[0]);

static const char* phraseMENU_ROAM[] = {
    "[B] next room. swipe L/R explores.",
    "the house moves. [B] picks the scene."
};
static constexpr uint8_t phraseMENU_ROAM_N = sizeof(phraseMENU_ROAM) / sizeof(phraseMENU_ROAM[0]);

static const char* phraseHUNT[] = {
    "[B] pause. [B+] closes the hunt.",
    "freeze the wire. hold [B] to go dark.",
    "[A] opens stats. hunt keeps receipts."
};
static constexpr uint8_t phraseHUNT_N = sizeof(phraseHUNT) / sizeof(phraseHUNT[0]);

static const char* phraseSPECTRUM[] = {
    "2.4 and 5G: [A/C] networks, [B] inspect.",
    "double tap graph: 2.4 MODEL / 5G ZOOM.",
    "5G detail: [A/C] actions. [B] runs one.",
    "5G SNAP holds until the next scan completes."
};
static constexpr uint8_t phraseSPECTRUM_N = sizeof(phraseSPECTRUM) / sizeof(phraseSPECTRUM[0]);

static const char* phraseLOOT[] = {
    "[A/C] evidence. [B] view. [B+] act.",
    "open a file. ship only with receipts.",
    "four tabs. one evidence chain."
};
static constexpr uint8_t phraseLOOT_N = sizeof(phraseLOOT) / sizeof(phraseLOOT[0]);

static const char* phraseFEEDING[] = {
    "[A/C] browse. [B] changes shelf.",
    "trophies remember what sleep forgot."
};
static constexpr uint8_t phraseFEEDING_N = sizeof(phraseFEEDING) / sizeof(phraseFEEDING[0]);

static const char* phraseWALK[] = {
    "[C+] closes the mileage report.",
    "shoe leather logged. legs have receipts."
};
static constexpr uint8_t phraseWALK_N = sizeof(phraseWALK) / sizeof(phraseWALK[0]);

static const char* phraseSETTINGS[] = {
    "[B] toggle. [A][C] scroll.",
    "switches have labels. sharp edges too."
};
static constexpr uint8_t phraseSETTINGS_N = sizeof(phraseSETTINGS) / sizeof(phraseSETTINGS[0]);

static const char* phraseFLOCK[] = {
    "[A/C] panes. PEER uses them to scroll.",
    "[B] asks summaries. raw evidence stays."
};
static constexpr uint8_t phraseFLOCK_N = sizeof(phraseFLOCK) / sizeof(phraseFLOCK[0]);

static const char* phrasePOWER[] = {
    "[A/C] pick. [B] selects.",
    "deep/off ask twice. [C+] cancels."
};
static constexpr uint8_t phrasePOWER_N = sizeof(phrasePOWER) / sizeof(phrasePOWER[0]);

static const char* phraseABOUT[] = {
    "[C+] closes the origin file.",
    "eighteen reports. one bad dawn. [C+]."
};
static constexpr uint8_t phraseABOUT_N = sizeof(phraseABOUT) / sizeof(phraseABOUT[0]);

static const char* phraseWEBCONFIG[] = {
    "browser submit saves. [C+] exits.",
    "portal screen open. hold [C] to close."
};
static constexpr uint8_t phraseWEBCONFIG_N = sizeof(phraseWEBCONFIG) / sizeof(phraseWEBCONFIG[0]);

static const char* phraseWARDRIVE[] = {
    "swipe L/R: cockpit or sensor tape.",
    "sensor tape dims and runs at 10Hz.",
    "Core/C5 GPS keeps SD receipts honest."
};
static constexpr uint8_t phraseWARDRIVE_N = sizeof(phraseWARDRIVE) / sizeof(phraseWARDRIVE[0]);

static const char* phraseBLE[] = {
    "[A/C] browse. [B] lock on.",
    "pick a tag. [B] starts the tail."
};
static constexpr uint8_t phraseBLE_N = sizeof(phraseBLE) / sizeof(phraseBLE[0]);

static const char* phraseDEFHOG[] = {
    "[A/C] panes. [B] pane action.",
    "five panes. one case. hold to leave."
};
static constexpr uint8_t phraseDEFHOG_N = sizeof(phraseDEFHOG) / sizeof(phraseDEFHOG[0]);

static const char* phraseXFER[] = {
    "scan QR. browse. [C+] or swipe L exits.",
    "local AP. browser gets the evidence."
};
static constexpr uint8_t phraseXFER_N = sizeof(phraseXFER) / sizeof(phraseXFER[0]);

// ==[ BAR TEXT PER MODE ]==
static const char* barIDLE       = "[A/C]THEMES [B]MENU [B+]TP [PWR/C+]PWR";
static const char* barMENU_ROOT  = "[A/C]GROUPS [B]OPEN [C+]EXIT";
static const char* barMENU_GROUP = "[A/C]TOOLS [B]GO [C+]BACK";
static const char* barMENU_ROAM  = "[B]ROOM  [C+]EXIT";
static const char* barHUNT       = "[A]OVERLAY  [B]PAUSE  [B+]EXIT";
static const char* barSPECTRUM   = "[A/C]NETS [B]INSPECT [B+/C+]BACK";
static const char* barSPECTRUM5G = "[A/C]NETS [B]INSPECT [B+/C+]2.4";
static const char* barLOOT       = "[A/C]SCROLL  [B]VIEW  [B+]UPLOAD  [C+]EXIT";
static const char* barFEEDING    = "[A/C]BROWSE  [B]TAB  [C+]EXIT";
static const char* barWALK       = "[C+]EXIT";
static const char* barSETTINGS   = "[A/C]SCROLL  [B]TOGGLE  [C+]EXIT";
static const char* barFLOCK      = "[A/C]PANE/SCR [B]P33R_R3Q [C+]EXIT";
static const char* barPOWER      = "[A/C]PICK  [B]SELECT  [C+]CANCEL";
static const char* barABOUT      = "[C+]EXIT";
static const char* barWEBCONFIG  = "[C+]EXIT";
static const char* barWARDRIVE   = "[SWIPE<>]TAPE [B]PAUSE [C]EXIT";
static const char* barBLE_LIST   = "[A/C]NAV [B]LOCK [B+/C+]EXIT";
static const char* barBLE_PING   = "[A]PING [B]STOP [B+]RECAL [C+]BACK";
static const char* barBLE_GATT   = "[A]GATT [B]STOP [B+]RECAL [C+]BACK";
static const char* barDEFHOG     = "[A/C]PANES [B]ACT [B+/C+]EXIT";
static const char* barXFER       = "[C+]EXIT";

static const char* phraseC5[] = {
    "C5 Monster UART bridge. dual-band 5GHz.",
    "select command. OK runs it. BACK scrolls."
};
static constexpr uint8_t phraseC5_N = sizeof(phraseC5) / sizeof(phraseC5[0]);
static const char* barC5          = "[B]RUN [B+]STOP [C+]EXIT";

static const char* phraseMAIL[] = {
    "[A/C] letters. [B] opens the file.",
    "Pig Post keeps choices with the case."
};
static constexpr uint8_t phraseMAIL_N = sizeof(phraseMAIL) / sizeof(phraseMAIL[0]);
static const char* barMAIL        = "[A/C]LETTERS [B]OPEN [C+]EXIT";

static const char* phraseMESH[] = {
    "[B] writes. [A/C] walk the scrollback.",
    "Tap the strip for the node roster.",
    "PROTO carries names, hops and acks."
};
static constexpr uint8_t phraseMESH_N = sizeof(phraseMESH) / sizeof(phraseMESH[0]);
static const char* barMESH        = "[A/C]SCR0LL [B]WR1T3 [B+]CL34R [C+]EXIT";

// ==[ HELPERS ]==

struct HintData {
    const char* const* phrases;
    uint8_t phraseCount;
    const char* barText;
};

static HintData getHintData(HamletMode mode) {
    switch (mode) {
        case HamletMode::IDLE:           return {phraseIDLE, phraseIDLE_N, barIDLE};
        case HamletMode::MENU:
            if (MenuPig::isRoaming()) return {phraseMENU_ROAM, phraseMENU_ROAM_N, barMENU_ROAM};
            return {phraseMENU, phraseMENU_N,
                    Menu::isAtRoot() ? barMENU_ROOT : barMENU_GROUP};
        case HamletMode::HUNT:           return {phraseHUNT, phraseHUNT_N, barHUNT};
        case HamletMode::SPECTRUM:
            return {phraseSPECTRUM, phraseSPECTRUM_N,
                    Spectrum::isShowing5GHz() ? barSPECTRUM5G : barSPECTRUM};
        case HamletMode::LOOT:           return {phraseLOOT, phraseLOOT_N, barLOOT};
        case HamletMode::FEEDING:        return {phraseFEEDING, phraseFEEDING_N, barFEEDING};
        case HamletMode::WALK_STATS:     return {phraseWALK, phraseWALK_N, barWALK};
        case HamletMode::SETTINGS:       return {phraseSETTINGS, phraseSETTINGS_N, barSETTINGS};
        case HamletMode::NOWFLOCK:       return {phraseFLOCK, phraseFLOCK_N, barFLOCK};
        case HamletMode::POWER_MENU:     return {phrasePOWER, phrasePOWER_N, barPOWER};
        case HamletMode::ABOUT:          return {phraseABOUT, phraseABOUT_N, barABOUT};
        case HamletMode::WEBCONFIG:      return {phraseWEBCONFIG, phraseWEBCONFIG_N, barWEBCONFIG};
        case HamletMode::WARDRIVE:       return {phraseWARDRIVE, phraseWARDRIVE_N, barWARDRIVE};
        case HamletMode::BLE_SCANNER:
            if (!BleScanner::isTracking()) return {phraseBLE, phraseBLE_N, barBLE_LIST};
            return {phraseBLE, phraseBLE_N,
                    BleScanner::canTriggerSound() ? barBLE_PING : barBLE_GATT};
        case HamletMode::DEFHOG4:        return {phraseDEFHOG, phraseDEFHOG_N, barDEFHOG};
        case HamletMode::XFER:           return {phraseXFER, phraseXFER_N, barXFER};
        case HamletMode::C5MONSTER:      return {phraseC5, phraseC5_N, barC5};
        case HamletMode::MAIL:           return {phraseMAIL, phraseMAIL_N, barMAIL};
        case HamletMode::MESH:           return {phraseMESH, phraseMESH_N, barMESH};
        default:                          return {nullptr, 0, nullptr};
    }
}

// ==[ PUBLIC API ]==

void onModeEnter(HamletMode mode) {
    uint8_t idx = modeIndex(mode);
    if (idx >= MODE_COUNT) return;

    visitCount[idx]++;
    bool firstTime = !(seenMask & (1u << idx));
    bool showHint = firstTime || (visitCount[idx] % 5 == 0);

    if (!showHint) {
        hintActive = false;
        return;
    }

    // mark seen
    if (firstTime) {
        seenMask |= (1u << idx);
        saveSeenMask();
    }

    HintData data = getHintData(mode);
    if (!data.phrases || data.phraseCount == 0) {
        // A mode with no hint table still ends the previous mode's window;
        // otherwise its bar text keeps blinking over an unrelated screen.
        hintActive = false;
        return;
    }

    // pick phrase: first-time = index 0, else random from pool
    uint8_t phraseIdx = firstTime ? 0 : (millis() % data.phraseCount);
    Mood::queuePhrase(data.phrases[phraseIdx], AvatarState::NEUTRAL);

    // activate bar hint
    currentHintMode = mode;
    hintActive = true;
    hintStartTime = millis();
    blinkOn = true;
    lastBlinkTime = millis();
}

void update(uint32_t now) {
    if (!hintActive) return;

    // timeout check
    if (now - hintStartTime >= HINT_DURATION_MS) {
        hintActive = false;
        return;
    }

    // blink toggle
    if (now - lastBlinkTime >= BLINK_INTERVAL_MS) {
        blinkOn = !blinkOn;
        lastBlinkTime = now;
    }
}

bool isHintActive() {
    return hintActive;
}

const char* getHintBar() {
    if (!hintActive || !blinkOn) return nullptr;

    HintData data = getHintData(currentHintMode);
    return data.barText;
}

void loadSeenMask() {
    seenMask = Config::getHintSeen();
}

void saveSeenMask() {
    Config::setHintSeen(seenMask);
}

}  // namespace TouchHints

#endif  // HAMLET_HAS_TOUCH
