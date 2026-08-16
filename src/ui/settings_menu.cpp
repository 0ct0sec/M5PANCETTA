/**
 * Settings Menu — grouped access to the device contract.
 *
 * The root list opens bounded modal groups for display, radio, storage,
 * motion, uplinks, and target-specific peripherals. Edits flow through
 * Config so validation and persistence stay outside the renderer.
 */

#include "settings_menu.h"
#include "pin_entry.h"
#include "soft_keyboard.h"
#include "display.h"
#include "frame_presenter.h"
#include "ui_measurements.h"
#include <M5Unified.h>
#include "../core/config.h"
#include "../core/power.h"
#include "../piglet/avatar.h"
#include "../audio/sfx.h"
#include "../audio/noir_jazz.h"
#include "../haptic/haptic.h"
#include "../led/ambient_led.h"
#include "../input/touch_hints.h"
#include "../core/gps.h"
#include "../core/gps_policy.h"
#include "../defense/recon.h"
#include "../defense/defense_pipeline.h"
#include "../sync/nowflock_transport.h"
#include "../net/ntp_sync.h"
#include "../net/wifi_client.h"
#include "../net/wpasec_client.h"
#include "../radio/c5monster_uart.h"
#include "../radio/c5_uart_policy.h"
#include "../radio/meshtastic_uart.h"
#include "../radio/mesh_uart_policy.h"
#include "../util/time_math.h"

namespace SettingsMenu {

using namespace UIMeasurements::SettingsLayout;

// ==[ SETTING ITEMS ]== All individual settings
enum SettingItem {
    // Text-entry items (keyboard-driven)
    SET_HAMLET_NAME,
    SET_HEAD_STYLE,
    SET_WIFI_SSID,
    SET_WIFI_PASS,
    SET_WPASEC_KEY,
    SET_WIGLE_USER,
    SET_WIGLE_TOKEN,
    // Look-and-feel items
    SET_THEME,
    SET_BRIGHT,
    SET_VOLUME,
    SET_MUSIC,
    SET_HAPTIC_INT,
    // Theme detail items
    SET_THEME_STYLE,
    SET_ACCENT_MODE,
    SET_LIGHT_INT,
    // Display group
    SET_DIM_AFTER,
    SET_DIM_LEVEL,
    SET_ROTATE_180,
    SET_LED_AMBIENT,
    SET_LED_COLOR,
    SET_LED_BRIGHT,
    SET_TILT_NAV,
    SET_SPECTRUM_TILT,
    SET_ROOM_PARALLAX,
    SET_BATH_MIC,
    // Power group
    SET_SHAKE_WAKE,
    SET_ALERT_WAKE,
    SET_BATT_ADAPT,
    SET_PWR_FPS,
    // Clock group
    SET_YEAR,
    SET_DAY,
    SET_MONTH,
    SET_HOUR,
    SET_MINUTE,
    SET_NTP_SYNC,
    // Hunt group (MUDBALL is 3-state: OFF/ON/AGGRO)
    SET_MUDBALL,
    SET_EAPOL_INJ,
    SET_CSA_HERD,
    SET_AUTH_FLOOD,
    SET_SAE_ATTACK,
    SET_AUTO_PROBE,
    SET_PROBE_RSSI,
    // Track group
    SET_AUTO_HUNT,
    SET_RSSI_SMOOTH,
    SET_GHOST_MARK,
    // Defense group
    SET_IPP_ENABLED,
    SET_IPP_BLE_SCAN,
    SET_IPP_WIFI_SCAN,
    SET_HOGWASH,
    SET_PARANOIA,
    SET_LOOT_PIN,
    SET_CANARY,
    SET_FORENSIC_EXPORT,
    // GPS group
    SET_WARDRIVE_BLE,
    SET_GPS_ENABLE,
    SET_GPS_ALWAYS_ON,
    SET_GPS_RX_PIN,
    SET_GPS_TX_PIN,
    SET_GPS_BAUD,
    // Watchlist group
    SET_WL_SLOT0,
    SET_WL_SLOT1,
    SET_WL_SLOT2,
    // Hazard group
    SET_REINCARNATE,
    // FLOCK / NOWFLOCK group
    SET_NF_ENABLED,
    SET_NF_GROUP_KEY,
    SET_NF_REPORT_S,
    SET_NF_PIGBROTHER,
    SET_NF_EXPORT_PROFILE,
    SET_NF_BLE_HEARTBEAT,
    // C5Monster group
    SET_C5_ENABLED,
    SET_C5_RX_PIN,
    SET_C5_TX_PIN,
    SET_C5_BAUD,
    // Mesh group — the C6L's two pins move as one port, so there is a single
    // route item rather than an RX and a TX that can disagree.
    SET_MESH_ENABLED,
    SET_MESH_PORT,
    SET_MESH_BAUD,
    SET_MESH_CODEC,
    // Non-selectable captions inside combined groups.
    SET_CAP_POWER,
    SET_CAP_CLOCK,
    SET_CAP_TRACKING,
    SET_CAP_WATCHLIST,
    SET_CAP_NETWORK,
    SET_CAP_SERVICES,
    SET_CAP_C5,
    SET_CAP_MESH,
    SETTING_COUNT
};

// ==[ ROOT MENU ]== Twelve peer categories; individual knobs live in groups.
enum RootItem {
    ROOT_PROFILE,
    ROOT_LOOK_FEEL,
    ROOT_DISPLAY_INPUT,
    ROOT_POWER_TIME,
    ROOT_TRACK_WATCH,
    ROOT_GPS,
    ROOT_ATTACK,
    ROOT_DEFENSE,
    ROOT_UPLINKS,
    ROOT_FLOCK,
    ROOT_ACCESSORIES,
    ROOT_DANGER,
    ROOT_COUNT
};

// ==[ GROUP DEFINITIONS ]==
enum GroupId {
    GROUP_NONE = -1,
    GROUP_PROFILE = 0,
    GROUP_LOOK_FEEL,
    GROUP_DISPLAY_INPUT,
    GROUP_POWER_TIME,
    GROUP_TRACK_WATCH,
    GROUP_GPS,
    GROUP_ATTACK,
    GROUP_DEFENSE,
    GROUP_UPLINKS,
    GROUP_FLOCK,
    GROUP_ACCESSORIES,
    GROUP_DANGER,
    GROUP_COUNT
};

// Group contents (indices into SettingItem). Combined groups carry caption rows
// so similar configuration stays together without becoming an unstructured list.
static const SettingItem GROUP_PROFILE_ITEMS[] = {
    SET_HAMLET_NAME, SET_HEAD_STYLE
};
static const SettingItem GROUP_LOOK_FEEL_ITEMS[] = {
    SET_THEME, SET_THEME_STYLE, SET_ACCENT_MODE, SET_LIGHT_INT,
    SET_BRIGHT, SET_VOLUME, SET_MUSIC, SET_HAPTIC_INT
};
#if defined(HAMLET_CORE3SE)
static const SettingItem GROUP_DISPLAY_INPUT_ITEMS[] = {
    SET_DIM_AFTER, SET_DIM_LEVEL, SET_ROTATE_180,
    SET_LED_AMBIENT, SET_LED_COLOR, SET_LED_BRIGHT,
    SET_TILT_NAV, SET_SPECTRUM_TILT, SET_ROOM_PARALLAX, SET_BATH_MIC
};
#else
static const SettingItem GROUP_DISPLAY_INPUT_ITEMS[] = {
    SET_DIM_AFTER, SET_DIM_LEVEL, SET_ROTATE_180,
    SET_LED_AMBIENT, SET_LED_COLOR, SET_LED_BRIGHT,
    SET_TILT_NAV, SET_SPECTRUM_TILT, SET_ROOM_PARALLAX
};
#endif
static const SettingItem GROUP_POWER_TIME_ITEMS[] = {
    SET_CAP_POWER, SET_SHAKE_WAKE, SET_ALERT_WAKE, SET_BATT_ADAPT, SET_PWR_FPS,
    SET_CAP_CLOCK, SET_YEAR, SET_MONTH, SET_DAY, SET_HOUR, SET_MINUTE, SET_NTP_SYNC
};
static const SettingItem GROUP_TRACK_WATCH_ITEMS[] = {
    SET_CAP_TRACKING, SET_AUTO_HUNT, SET_RSSI_SMOOTH, SET_GHOST_MARK,
    SET_CAP_WATCHLIST, SET_WL_SLOT0, SET_WL_SLOT1, SET_WL_SLOT2
};
static const SettingItem GROUP_GPS_ITEMS[] = {
    SET_GPS_ENABLE, SET_GPS_ALWAYS_ON, SET_GPS_RX_PIN,
    SET_GPS_TX_PIN, SET_GPS_BAUD, SET_WARDRIVE_BLE
};
static const SettingItem GROUP_ATTACK_ITEMS[] = {
    SET_MUDBALL, SET_EAPOL_INJ, SET_CSA_HERD, SET_AUTH_FLOOD,
    SET_SAE_ATTACK, SET_AUTO_PROBE, SET_PROBE_RSSI
};
static const SettingItem GROUP_DEFENSE_ITEMS[] = {
    SET_IPP_ENABLED, SET_IPP_BLE_SCAN, SET_IPP_WIFI_SCAN, SET_HOGWASH,
    SET_PARANOIA, SET_LOOT_PIN, SET_CANARY, SET_FORENSIC_EXPORT
};
static const SettingItem GROUP_UPLINKS_ITEMS[] = {
    SET_CAP_NETWORK, SET_WIFI_SSID, SET_WIFI_PASS,
    SET_CAP_SERVICES, SET_WPASEC_KEY, SET_WIGLE_USER, SET_WIGLE_TOKEN
};
static const SettingItem GROUP_FLOCK_ITEMS[] = {
    SET_NF_ENABLED, SET_NF_GROUP_KEY, SET_NF_REPORT_S, SET_NF_PIGBROTHER,
    SET_NF_EXPORT_PROFILE, SET_NF_BLE_HEARTBEAT
};
static const SettingItem GROUP_ACCESSORIES_ITEMS[] = {
    SET_CAP_C5, SET_C5_ENABLED, SET_C5_RX_PIN, SET_C5_TX_PIN, SET_C5_BAUD,
    SET_CAP_MESH, SET_MESH_ENABLED, SET_MESH_PORT, SET_MESH_BAUD, SET_MESH_CODEC
};
static const SettingItem GROUP_DANGER_ITEMS[] = {
    SET_REINCARNATE
};

// ==[ SIZES ]== derived from the arrays, never written by hand.
//
// This used to be a literal table kept in step by eye, and it silently lost:
// M3SH T4LK's C0D3C item was added to GROUP_MESH_ITEMS, the item drew and
// acted correctly, and the menu still showed three rows because the 14th
// number here still said 3. Nothing caught it — the asserts below check how
// many groups there are, not how long each one is. sizeof cannot drift, and
// the DISPLAY entry now picks up its own per-target length for free instead of
// needing a second #if that could disagree with the first.
#define GROUP_LEN(a) ((uint8_t)(sizeof(a) / sizeof((a)[0])))
static const uint8_t GROUP_SIZES[] = {
    GROUP_LEN(GROUP_PROFILE_ITEMS), GROUP_LEN(GROUP_LOOK_FEEL_ITEMS),
    GROUP_LEN(GROUP_DISPLAY_INPUT_ITEMS), GROUP_LEN(GROUP_POWER_TIME_ITEMS),
    GROUP_LEN(GROUP_TRACK_WATCH_ITEMS), GROUP_LEN(GROUP_GPS_ITEMS),
    GROUP_LEN(GROUP_ATTACK_ITEMS), GROUP_LEN(GROUP_DEFENSE_ITEMS),
    GROUP_LEN(GROUP_UPLINKS_ITEMS), GROUP_LEN(GROUP_FLOCK_ITEMS),
    GROUP_LEN(GROUP_ACCESSORIES_ITEMS), GROUP_LEN(GROUP_DANGER_ITEMS)
};
#undef GROUP_LEN
static const SettingItem* GROUP_ITEMS[] = {
    GROUP_PROFILE_ITEMS, GROUP_LOOK_FEEL_ITEMS, GROUP_DISPLAY_INPUT_ITEMS,
    GROUP_POWER_TIME_ITEMS, GROUP_TRACK_WATCH_ITEMS, GROUP_GPS_ITEMS,
    GROUP_ATTACK_ITEMS, GROUP_DEFENSE_ITEMS, GROUP_UPLINKS_ITEMS,
    GROUP_FLOCK_ITEMS, GROUP_ACCESSORIES_ITEMS, GROUP_DANGER_ITEMS
};
static const char* const GROUP_NAMES[] = {
    "PR0F1L3", "L00K & F33L", "SCR33N & 1NPUT", "P0W3R & T1M3",
    "TR4CK & W4TCH", "GPS N4V", "4TT4CK!", "D3F3NS3",
    "UPL1NKS", "N0W F0CK", "4CC3SS0R13S", "H4Z4RD!"
};
static const GroupId ROOT_GROUPS[] = {
    GROUP_PROFILE, GROUP_LOOK_FEEL, GROUP_DISPLAY_INPUT, GROUP_POWER_TIME,
    GROUP_TRACK_WATCH, GROUP_GPS, GROUP_ATTACK, GROUP_DEFENSE,
    GROUP_UPLINKS, GROUP_FLOCK, GROUP_ACCESSORIES, GROUP_DANGER
};

static_assert(sizeof(GROUP_SIZES) / sizeof(GROUP_SIZES[0]) == GROUP_COUNT,
              "group size table drift");
static_assert(sizeof(GROUP_ITEMS) / sizeof(GROUP_ITEMS[0]) == GROUP_COUNT,
              "group item table drift");
static_assert(sizeof(GROUP_NAMES) / sizeof(GROUP_NAMES[0]) == GROUP_COUNT,
              "group name table drift");
static_assert(sizeof(ROOT_GROUPS) / sizeof(ROOT_GROUPS[0]) == ROOT_COUNT,
              "root-to-group map drift");

// ==[ STATE ]==
static int rootIdx = 0;
static int rootScroll = 0;
static int modalIdx = 0;
static int modalScroll = 0;
static GroupId activeGroup = GROUP_NONE;
static bool showingLegalWarning = false;
static bool showingReincarnateWarning = false;
static M5Canvas* canvas = nullptr;
static uint32_t headPreviewUntil = 0;

enum ClockSyncState : uint8_t {
    CLOCK_SYNC_READY = 0,
    CLOCK_SYNC_CONNECTING,
    CLOCK_SYNC_RUNNING,
    CLOCK_SYNC_SUCCESS,
    CLOCK_SYNC_FAILED
};
static ClockSyncState clockSyncState = CLOCK_SYNC_READY;
static char clockSyncDetail[48] = "";

// ==[ TEXT EDITING STATE ]== keyboard-driven text entry
static bool textEditing = false;
static char textBuffer[80] = "";
static SettingItem textEditId = SET_HAMLET_NAME;

// ==[ PIN EDITING STATE ]== ABC button PIN entry
static bool pinEditing = false;

// Forward declarations
static void draw();
static void drawRoot();
static void drawModal();
static void drawLegalWarning();
static void drawReincarnateWarning();
static void drawHeadPreviewToast();
static void changeValue(SettingItem item);
static void getItemLabelValue(SettingItem item, char* label, char* value, char* hint);
static uint8_t c5DefaultRxPin();
static uint8_t c5DefaultTxPin();
static uint32_t c5DefaultBaud();
static uint8_t nextC5RxPin(uint8_t current);
static uint8_t nextC5TxPin(uint8_t current);
static uint32_t nextC5Baud(uint32_t current);
static void normalizeC5Pins(uint8_t& rxPin, uint8_t& txPin);
static void restartC5Bridge();
static void restartMeshBridge();

// ==[ HELPERS ]==
static bool isCaption(SettingItem item) {
    return item >= SET_CAP_POWER && item <= SET_CAP_MESH;
}

static bool isClockSyncActive() {
    return clockSyncState == CLOCK_SYNC_CONNECTING ||
           clockSyncState == CLOCK_SYNC_RUNNING;
}

static void setClockSyncFailure(const char* detail) {
    clockSyncState = CLOCK_SYNC_FAILED;
    snprintf(clockSyncDetail, sizeof(clockSyncDetail), "%s",
             detail && detail[0] ? detail : "SYNC FAILED");
}

static const char* captionText(SettingItem item) {
    switch (item) {
        case SET_CAP_POWER:     return "P0W3R";
        case SET_CAP_CLOCK:     return "CL0CK";
        case SET_CAP_TRACKING:  return "TR4CK1NG";
        case SET_CAP_WATCHLIST: return "W4TCHL1ST";
        case SET_CAP_NETWORK:   return "N3TW0RK";
        case SET_CAP_SERVICES:  return "S3RV1C3S";
        case SET_CAP_C5:        return "C5 M0NST3R";
        case SET_CAP_MESH:      return "M3SH T4LK";
        default:                return "";
    }
}

static int nextSelectableModal(int from, int dir) {
    const int groupSize = GROUP_SIZES[activeGroup];
    int next = from;
    do {
        next = (next + dir + groupSize) % groupSize;
    } while (isCaption(GROUP_ITEMS[activeGroup][next]) && next != from);
    return next;
}

static void keepSelectionVisible(int selected, int& scroll, int visibleCount) {
    if (selected >= scroll + visibleCount) {
        scroll = selected - visibleCount + 1;
    } else if (selected < scroll) {
        scroll = selected;
    }
    if (activeGroup != GROUP_NONE && selected > 0 &&
        isCaption(GROUP_ITEMS[activeGroup][selected - 1]) &&
        selected - 1 < scroll) {
        scroll = selected - 1;
    }
    if (selected == 0) scroll = 0;
}

static void openGroup(GroupId group) {
    activeGroup = group;
    modalIdx = nextSelectableModal(-1, 1);
    modalScroll = 0;
}

static const char* getHeadStyleName(Config::PigHeadStyle style) {
    switch (style) {
        case Config::PIG_HEAD_ROSE:   return "R0SE";
        case Config::PIG_HEAD_ICE:    return "1CE";
        case Config::PIG_HEAD_GOLD:   return "G0LD";
        case Config::PIG_HEAD_FEDORA: return "F3D0R4";
        case Config::PIG_HEAD_HYPE:   return "HYP3+";
        default:                      return "TH3ME";
    }
}

static void triggerHeadPreview() {
    headPreviewUntil = millis() + 2600;
}

static int snapPigPreview(int v) {
    return v & ~1;  // Avatar body/headwear align to the pig's 2px grid.
}

static uint8_t c5DefaultRxPin() {
    return C5UartPolicy::RX_PIN;
}

static uint8_t c5DefaultTxPin() {
    return C5UartPolicy::TX_PIN;
}

static uint32_t c5DefaultBaud() {
    return C5UartPolicy::DEFAULT_BAUD;
}

static uint8_t nextC5RxPin(uint8_t current) {
    return (current == C5UartPolicy::RX_PIN)
        ? C5UartPolicy::TX_PIN : C5UartPolicy::RX_PIN;
}

static uint8_t nextC5TxPin(uint8_t current) {
    return (current == C5UartPolicy::TX_PIN)
        ? C5UartPolicy::RX_PIN : C5UartPolicy::TX_PIN;
}

static uint32_t nextC5Baud(uint32_t current) {
    return (current == 115200u) ? 9600u : 115200u;
}

static void normalizeC5Pins(uint8_t& rxPin, uint8_t& txPin) {
    bool repaired = false;
    C5UartPolicy::sanitizePins(rxPin, txPin, repaired);
}

// Toggling the bridge or moving its port has to take effect without a reboot,
// because the whole point of the port item is trying the other connector to
// see whether it carries data on this core.
static void restartMeshBridge() {
    Mesh::stop();
    if (!Config::getMeshEnabled()) return;

    // Port C is the GPS module's route too, and on some cores the C5 bridge's.
    // Refusing here beats handing the driver two owners and reading the
    // resulting garbage as mesh traffic. Config::init() settles the same
    // question at boot — this is the same predicate, not a second opinion.
    if (const char* owner = Config::meshPinOwner()) {
        Config::setMeshEnabled(false);
        char toast[32];
        snprintf(toast, sizeof(toast), "%s 0WNS TH0S3 P1NS", owner);
        Display::showToast(toast, 1800);
        return;
    }

    if (!Mesh::begin(Config::getMeshRxPin(), Config::getMeshTxPin(),
                     Config::getMeshBaud(), Config::getMeshCodec())) {
        Display::showToast("M3SH BR1DG3 F41L3D", 1600);
    }
}

static void restartC5Bridge() {
    uint8_t rxPin = Config::getC5RxPin();
    uint8_t txPin = Config::getC5TxPin();
    normalizeC5Pins(rxPin, txPin);
    if (rxPin != Config::getC5RxPin()) Config::setC5RxPin(rxPin);
    if (txPin != Config::getC5TxPin()) Config::setC5TxPin(txPin);
    C5Monster::stop();
    if (!Config::getC5Enabled()) {
#if !HAMLET_TARGET_CORES3SE
        if (Config::getGPSEnabled() && Config::getGPSAlwaysOn()) {
            GPS::startUART();
        }
#endif
        return;
    }
    C5Monster::begin(Config::getC5RxPin(), Config::getC5TxPin(), Config::getC5Baud());
}

// ==[ TEXT EDITING HELPERS ]== launch keyboard for text fields
static void startTextEdit(SettingItem id, const char* title, const char* current,
                          size_t bufCap, size_t maxLen) {
    strncpy(textBuffer, current ? current : "", bufCap - 1);
    textBuffer[bufCap - 1] = '\0';
    textEditId = id;
    SoftKeyboard::start(title, textBuffer, bufCap, maxLen);
    textEditing = true;
}

static void finishTextEdit(bool accepted) {
    textEditing = false;
    if (!accepted) return;
    switch (textEditId) {
        case SET_HAMLET_NAME:  Config::setHamletName(textBuffer); break;
        case SET_WIFI_SSID:
            Config::setUploadWifiSsid(textBuffer);
            WifiClient::resetBackoff();
            break;
        case SET_WIFI_PASS:
            Config::setUploadWifiPass(textBuffer);
            WifiClient::resetBackoff();
            break;
        case SET_WPASEC_KEY:
            Config::setWpaSecKey(textBuffer);
            WpaSec::resetBackoff();
            break;
        case SET_WIGLE_USER:    Config::setWigleUsername(textBuffer); break;
        case SET_WIGLE_TOKEN:   Config::setWigleToken(textBuffer); break;
        case SET_CANARY:        DefensePipeline::setCanarySSID(textBuffer); break;
        case SET_WL_SLOT0: case SET_WL_SLOT1: case SET_WL_SLOT2: {
            uint8_t idx = textEditId - SET_WL_SLOT0;
            Config::WatchlistSlot slot;
            if (Config::getWatchlistSlot(idx, slot)) {
                if (textBuffer[0] == '\0') {
                    DefensePipeline::removeFromWatchlist(idx);
                    Config::clearWatchlistSlot(idx);
                } else {
                    Config::setWatchlistSlot(idx, slot.payloadHash, textBuffer);
                    DefensePipeline::updateWatchlistLabel(idx, textBuffer);
                }
            }
            break;
        }
        default: break;
    }
}

static bool isLeapYear(uint16_t year) {
    return ((year % 4) == 0 && (year % 100) != 0) || ((year % 400) == 0);
}

static uint8_t daysInMonth(uint16_t year, uint8_t month) {
    static const uint8_t days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) return 31;
    if (month == 2 && isLeapYear(year)) return 29;
    return days[month - 1];
}

static void clampRtcDate(m5::rtc_datetime_t& dt) {
    uint8_t maxDay = daysInMonth(dt.date.year, dt.date.month);
    if (dt.date.date > maxDay) dt.date.date = maxDay;
}

// ==[ PUBLIC API ]==
void enter() {
    showingLegalWarning = false;
    showingReincarnateWarning = false;
    // Stale flags from a prior session (sleep, mode switch, etc.) would block input
    // on re-entry. Reset everything UI-editable.
    textEditing = false;
    pinEditing = false;
    PinEntry::cancel();
    activeGroup = GROUP_NONE;
    rootIdx = 0;
    rootScroll = 0;
    modalIdx = 0;
    modalScroll = 0;
    headPreviewUntil = 0;
    clockSyncState = CLOCK_SYNC_READY;
    clockSyncDetail[0] = '\0';
    canvas = Display::getSharedCanvas();
    draw();
}

void exit() {
    // Drop the edit in progress rather than commit it: the user left without
    // pressing OK, and a half-typed password is not a decision.
    textEditing = false;
    SoftKeyboard::stop();
    pinEditing = false;
    PinEntry::cancel();
    if (isClockSyncActive()) WifiClient::disconnect();
    clockSyncState = CLOCK_SYNC_READY;
    clockSyncDetail[0] = '\0';
}

void update() {
    if (clockSyncState == CLOCK_SYNC_CONNECTING) {
        WifiClient::update();
        if (WifiClient::getState() == WifiClient::WIFI_CONNECTED) {
            clockSyncState = CLOCK_SYNC_RUNNING;
            snprintf(clockSyncDetail, sizeof(clockSyncDetail), "SYNCING TRUSTED UTC");
            draw();

            const bool synced = NtpSync::syncTime();
            if (synced) {
                clockSyncState = CLOCK_SYNC_SUCCESS;
                snprintf(clockSyncDetail, sizeof(clockSyncDetail), "RTC TRUSTED. UTC SYNCED.");
            } else {
                setClockSyncFailure(NtpSync::getLastError());
            }
            WifiClient::disconnect();
            draw();
            return;
        }
        if (WifiClient::getState() == WifiClient::WIFI_FAILED) {
            setClockSyncFailure(WifiClient::getLastError());
            WifiClient::disconnect();
            draw();
            return;
        }
    }

    // PIN entry tick (wrong-flash timeout)
    if (pinEditing) {
        bool wasFlash = PinEntry::isWrongFlash();
        PinEntry::update();
        if (wasFlash && !PinEntry::isWrongFlash()) draw();
        return;
    }

    // Keyboard owns the screen when active
    if (textEditing) {
        SoftKeyboard::update();
        bool acc = false;
        if (SoftKeyboard::consumeDone(acc)) {
            finishTextEdit(acc);
            draw();
        } else {
            // Redraw keyboard every frame (touch feedback)
            SoftKeyboard::draw(*canvas);
            Display::drawUiOverlaysTo(canvas);
            FramePresenter::present(*canvas);
        }
        return;
    }

    if (Display::needsOverlayRedraw()) {
        if (showingLegalWarning) drawLegalWarning();
        else if (showingReincarnateWarning) drawReincarnateWarning();
        else draw();
        return;
    }

    // Warning headers blink at ~2.5 Hz (now/400). Only push a sprite when the
    // phase actually flips; otherwise we'd full-canvas-redraw every tick.
    if (showingLegalWarning || showingReincarnateWarning) {
        uint8_t phase = (uint8_t)((millis() / 400) & 1);
        static uint8_t lastWarnPhase = 0xFF;
        if (phase != lastWarnPhase) {
            lastWarnPhase = phase;
            if (showingLegalWarning)            drawLegalWarning();
            else if (showingReincarnateWarning) drawReincarnateWarning();
        }
    }
}

bool isShowingWarning() {
    return showingLegalWarning || showingReincarnateWarning;
}

bool isDestructiveWarning() {
    // Require hold-to-confirm for destructive/irreversible warnings.
    // Legal warning = consent toggle (short tap fine).
    return showingReincarnateWarning;
}

void acceptWarning() {
    if (showingLegalWarning) {
        Config::setDeauthEnabled(true);
        showingLegalWarning = false;
        draw();
    } else if (showingReincarnateWarning) {
        Config::doPrestige();
        showingReincarnateWarning = false;
        draw();
    }
}

void declineWarning() {
    showingLegalWarning = false;
    showingReincarnateWarning = false;
    draw();
}

bool isTextEditing() {
    return textEditing;
}

bool isPinEditing() {
    return pinEditing;
}

void pinInput(char btn) {
    if (!pinEditing) return;
    PinEntry::handleButton(btn);
    if (PinEntry::isComplete()) {
        char code[5];
        PinEntry::getCode(code);
        Config::setLootPin(code);
        pinEditing = false;
        PinEntry::cancel();
    }
    draw();
}

void pinClearOrCancel() {
    if (!pinEditing) return;
    if (Config::hasLootPin()) {
        Config::clearLootPin();
    }
    pinEditing = false;
    PinEntry::cancel();
    draw();
}

void next() {
    if (textEditing || pinEditing || isClockSyncActive()) return;
    if (activeGroup == GROUP_NONE) {
        rootIdx = (rootIdx + 1) % ROOT_COUNT;
        keepSelectionVisible(rootIdx, rootScroll, kRootVisibleRows);
    } else {
        modalIdx = nextSelectableModal(modalIdx, 1);
        keepSelectionVisible(modalIdx, modalScroll, kModalVisibleRows);
    }
    draw();
}

void prev() {
    if (textEditing || pinEditing || isClockSyncActive()) return;
    if (activeGroup == GROUP_NONE) {
        rootIdx = (rootIdx - 1 + ROOT_COUNT) % ROOT_COUNT;
        keepSelectionVisible(rootIdx, rootScroll, kRootVisibleRows);
    } else {
        modalIdx = nextSelectableModal(modalIdx, -1);
        keepSelectionVisible(modalIdx, modalScroll, kModalVisibleRows);
    }
    draw();
}

void select() {
    if (textEditing || pinEditing || isClockSyncActive()) return;
    if (activeGroup == GROUP_NONE) {
        openGroup(ROOT_GROUPS[rootIdx]);
    } else {
        // Modal selection - change value
        SettingItem item = GROUP_ITEMS[activeGroup][modalIdx];
        changeValue(item);
        // startTextEdit() hands the surface to SoftKeyboard. Redrawing the
        // drawer here produces a one-frame flash over the keyboard handoff;
        // update() will render the keyboard on its next tick.
        if (textEditing) return;
        if (showingLegalWarning || showingReincarnateWarning) return;
    }
    draw();
}

// Close modal (called on BtnB long press)
bool closeModal() {
    if (textEditing) return true;  // keyboard owns screen, block exit
    if (pinEditing) {
        pinClearOrCancel();
        return true;
    }
    if (isClockSyncActive()) {
        WifiClient::disconnect();
        setClockSyncFailure("CANCELLED");
    }
    if (activeGroup != GROUP_NONE) {
        activeGroup = GROUP_NONE;
        draw();
        return true;
    }
    return false;
}

// ==[ VALUE CHANGE ]==
static void changeValue(SettingItem item) {
    switch (item) {
        // Text-entry items — launch keyboard
        case SET_HAMLET_NAME:
            startTextEdit(SET_HAMLET_NAME, "P1G 1D", Config::getHamletName(), 5, 4);
            return;
        case SET_WIFI_SSID:
            startTextEdit(SET_WIFI_SSID, "WIFI SSID", Config::getUploadWifiSsid(), 33, 32);
            return;
        case SET_WIFI_PASS:
            startTextEdit(SET_WIFI_PASS, "WIFI PASS", Config::getUploadWifiPass(), 65, 64);
            return;
        case SET_WPASEC_KEY:
            startTextEdit(SET_WPASEC_KEY, "WPA-SEC KEY", Config::getWpaSecKey(), 33, 32);
            return;
        case SET_WIGLE_USER:
            startTextEdit(SET_WIGLE_USER, "WIGLE USER", Config::getWigleUsername(), 25, 24);
            return;
        case SET_WIGLE_TOKEN:
            startTextEdit(SET_WIGLE_TOKEN, "WIGLE TOKEN", Config::getWigleToken(), 65, 64);
            return;
        case SET_HEAD_STYLE: {
            const uint8_t maxStyle = Config::getMaxUnlockedPigStyle();
            const uint8_t next =
                ((uint8_t)Config::getPigHeadStyle() + 1u) % (maxStyle + 1u);
            Config::setPigHeadStyle(static_cast<Config::PigHeadStyle>(next));
            triggerHeadPreview();
            break;
        }
        // Look-and-feel group.
        case SET_THEME:
            Display::nextTheme();
            Config::setThemeHue(Display::getCurrentHue());
            break;
        case SET_THEME_STYLE:
            Display::nextStyle();
            Config::setThemeStyle(Display::getCurrentStyle());
            break;
        case SET_ACCENT_MODE: {
            uint8_t m = (Config::getAccentMode() + 1) % 6;
            Config::setAccentMode(m);
            Display::setThemeHSV(Display::getCurrentHue(), Display::getCurrentStyle());  // re-eval name (GLDRUN3R combo)
            break;
        }
        case SET_LIGHT_INT: {
            uint8_t i = (Config::getLightIntensity() + 1) % 4;
            Config::setLightIntensity(i);
            Display::setThemeHSV(Display::getCurrentHue(), Display::getCurrentStyle());  // re-eval name (GLDRUN3R combo)
            break;
        }
        case SET_BRIGHT: {
            uint8_t b = Config::getBrightness() + 10;
            if (b > 100) b = 10;
            Config::setBrightness(b);
            M5.Display.setBrightness(b * 255 / 100);
            break;
        }
        case SET_VOLUME: {
            const uint8_t v = (SFX::getVolume() + 1) % 11;
            SFX::setVolume(v);
            Config::setSoundEnabled(v > 0);
            break;
        }
        case SET_MUSIC:
            NoirJazz::setVolume((NoirJazz::getVolume() + 1) % 11);
            break;
        case SET_HAPTIC_INT: {
            const uint8_t intensity = (Haptic::getIntensity() + 1) % 11;
            Haptic::setIntensity(intensity);
            Haptic::setEnabled(intensity > 0);
            if (intensity > 0) Haptic::tick();
            break;
        }
        case SET_DIM_AFTER: {
            uint16_t t = Config::getDimTimeout();
            t += 10;
            if (t > 300) t = 0;
            Config::setDimTimeout(t);
            break;
        }
        case SET_DIM_LEVEL: {
            uint8_t d = Config::getDimLevel();
            const uint8_t maxDim = min((uint8_t)50, Config::getBrightness());
            d += 5;
            if (d > maxDim) d = 0;
            Config::setDimLevel(d);
            break;
        }
        case SET_ROTATE_180: {
            bool current = Config::getDisplayRotate180();
            Config::setDisplayRotate180(!current);
            Display::applyRotation();
            break;
        }
        case SET_LED_AMBIENT: {
            bool on = !Config::getLedAmbient();
            Config::setLedAmbient(on);
            if (!on) AmbientLED::off();
            break;
        }
        case SET_LED_COLOR: {
            uint8_t c = (Config::getLedColor() + 1) % 14;  // 0=AUTO, 1=THEME, 2-13=fixed
            Config::setLedColor(c);
            break;
        }
        case SET_LED_BRIGHT: {
            uint8_t b = Config::getLedBrightness() % 10 + 1;  // 1-10 wrap
            Config::setLedBrightness(b);
            break;
        }
        case SET_SHAKE_WAKE:
            Config::setShakeWake(!Config::getShakeWake());
            break;
        case SET_ALERT_WAKE:
            Config::setAlertWake(!Config::getAlertWake());
            break;
        case SET_PARANOIA:
            Config::setParanoiaEnabled(!Config::getParanoiaEnabled());
            break;
        case SET_HOGWASH:
            Config::setWifiChaffEnabled(!Config::getWifiChaffEnabled());
            break;
        case SET_LOOT_PIN:
            pinEditing = true;
            PinEntry::begin();
            break;
        case SET_CANARY:
            startTextEdit(SET_CANARY, "C4N4RY", DefensePipeline::snapshot().getCanarySSID(), 33, 32);
            return;  // keyboard takes over
        case SET_FORENSIC_EXPORT:
            DefensePipeline::setForensicExportEnabled(!DefensePipeline::snapshot().isForensicExportEnabled());
            break;
        case SET_BATT_ADAPT:
            Power::setBatteryAdaptation(!Power::getBatteryAdaptation());
            break;
        case SET_PWR_FPS:
            Config::setPwrFps60(!Config::getPwrFps60());
            break;
        case SET_TILT_NAV:
            Config::setTiltNavigationEnabled(!Config::getTiltNavigationEnabled());
            break;
        case SET_SPECTRUM_TILT:
            Config::setSpectrumTiltEnabled(!Config::getSpectrumTiltEnabled());
            break;
        case SET_ROOM_PARALLAX:
            Config::setRoomParallaxEnabled(!Config::getRoomParallaxEnabled());
            break;
        case SET_BATH_MIC:
            Config::setBathMicEnabled(!Config::getBathMicEnabled());
            break;
        case SET_YEAR: {
            auto dt = M5.Rtc.getDateTime();
            dt.date.year++;
            if (dt.date.year > 2099) dt.date.year = 2020;
            clampRtcDate(dt);
            M5.Rtc.setDateTime(dt);
            Config::markClockSynced();
            break;
        }
        case SET_DAY: {
            auto dt = M5.Rtc.getDateTime();
            uint8_t maxDay = daysInMonth(dt.date.year, dt.date.month);
            dt.date.date = (dt.date.date % maxDay) + 1;
            M5.Rtc.setDateTime(dt);
            Config::markClockSynced();
            break;
        }
        case SET_MONTH: {
            auto dt = M5.Rtc.getDateTime();
            dt.date.month = (dt.date.month % 12) + 1;
            clampRtcDate(dt);
            M5.Rtc.setDateTime(dt);
            Config::markClockSynced();
            break;
        }
        case SET_HOUR: {
            auto dt = M5.Rtc.getDateTime();
            dt.time.hours = (dt.time.hours + 1) % 24;
            M5.Rtc.setDateTime(dt);
            Config::markClockSynced();
            break;
        }
        case SET_MINUTE: {
            auto dt = M5.Rtc.getDateTime();
            dt.time.minutes = (dt.time.minutes + 1) % 60;
            dt.time.seconds = 0;
            M5.Rtc.setDateTime(dt);
            Config::markClockSynced();
            break;
        }
        case SET_NTP_SYNC:
            clockSyncDetail[0] = '\0';
            if (!Config::hasUploadWifi()) {
                setClockSyncFailure("SSID MISSING");
                break;
            }
            clockSyncState = CLOCK_SYNC_CONNECTING;
            snprintf(clockSyncDetail, sizeof(clockSyncDetail), "CONNECTING TO WIFI");
            if (!WifiClient::connect()) {
                setClockSyncFailure(WifiClient::getLastError());
                if (WifiClient::ownsRadio()) WifiClient::disconnect();
            }
            break;
        case SET_MUDBALL:
            // 3-state cycle: OFF → ON (legal warning) → AGGRO → OFF
            if (!Config::getDeauthEnabled()) {
                showingLegalWarning = true;           // OFF → ON
            } else if (!Config::getDeauthAggressive()) {
                Config::setDeauthAggressive(true);    // ON → AGGRO
            } else {
                Config::setDeauthEnabled(false);      // AGGRO → OFF
                Config::setDeauthAggressive(false);
            }
            break;
        case SET_AUTO_PROBE:
            Config::setAutoProbe(!Config::getAutoProbe());
            break;
        case SET_PROBE_RSSI: {
            int8_t r = Config::getProbeThreshold();
            r -= 5;
            if (r < -80) r = -50;
            Config::setProbeThreshold(r);
            break;
        }
        case SET_SAE_ATTACK:
            Config::setSAEAttackEnabled(!Config::getSAEAttackEnabled());
            break;
        case SET_EAPOL_INJ:
            Config::setEAPOLInjectionEnabled(!Config::getEAPOLInjectionEnabled());
            break;
        case SET_CSA_HERD:
            Config::setCSAEnabled(!Config::getCSAEnabled());
            break;
        case SET_AUTH_FLOOD:
            Config::setAuthFloodEnabled(!Config::getAuthFloodEnabled());
            break;
        case SET_AUTO_HUNT:
            Config::setAutoHuntEnabled(!Config::getAutoHuntEnabled());
            break;
        case SET_RSSI_SMOOTH: {
            Config::RssiSmooth s = Config::getRssiSmooth();
            s = static_cast<Config::RssiSmooth>((static_cast<uint8_t>(s) + 1) % 3);
            Config::setRssiSmooth(s);
            break;
        }
        case SET_GHOST_MARK:
            Config::setGhostMarkerEnabled(!Config::getGhostMarkerEnabled());
            break;
        case SET_REINCARNATE:
            if (Config::isElder() && Config::getLevel() >= 42) {
                showingReincarnateWarning = true;
            }
            break;
        case SET_NF_ENABLED: {
            const bool enabled = !Config::getNowFlockEnabled();
            Config::setNowFlockEnabled(enabled);
            if (enabled) NowFlock::markEspNowNeedsReinit();
            else NowFlock::releaseRadio();
            break;
        }
        case SET_NF_GROUP_KEY: {
            uint32_t k = Config::getNowFlockGroupKey();
            k += 0x01010101u;
            if (k == 0) k = 0xDEADB4D6u;
            Config::setNowFlockGroupKey(k);
            NowFlock::markEspNowNeedsReinit();
            break;
        }
        case SET_NF_REPORT_S: {
            uint8_t s = Config::getNowFlockReportIntervalS();
            s = (uint8_t)(s >= 60 ? 2 : s + 1);
            Config::setNowFlockReportIntervalS(s);
            NowFlock::markEspNowNeedsReinit();
            break;
        }
        case SET_NF_PIGBROTHER:
            Config::setNowFlockPigbrother(!Config::getNowFlockPigbrother());
            NowFlock::markEspNowNeedsReinit();
            break;
        case SET_NF_EXPORT_PROFILE: {
            uint8_t p = (uint8_t)((Config::getNowFlockExportProfile() + 1) % 3);
            Config::setNowFlockExportProfile(p);
            break;
        }
        case SET_NF_BLE_HEARTBEAT:
            Config::setNowFlockBleHeartbeat(!Config::getNowFlockBleHeartbeat());
            NowFlock::markEspNowNeedsReinit();
            break;
        case SET_IPP_ENABLED: {
            bool enable = !Config::getIppEnabled();
            Config::setIppEnabled(enable);
            if (enable) DefensePipeline::requestWifiScan();
            break;
        }
        case SET_IPP_BLE_SCAN:
            Config::setIppBLEScan(!Config::getIppBLEScan());
            break;
        case SET_IPP_WIFI_SCAN: {
            bool enable = !Config::getIppWifiScan();
            Config::setIppWifiScan(enable);
            if (enable) DefensePipeline::requestWifiScan();
            break;
        }
        case SET_WARDRIVE_BLE:
            Config::setWardriveBleScan(!Config::getWardriveBleScan());
            break;
        case SET_GPS_ENABLE: {
            bool wasEnabled = Config::getGPSEnabled();
            Config::setGPSEnabled(!wasEnabled);
            if (!wasEnabled && Config::getGPSAlwaysOn()) {
                GPS::startUART();  // hot-apply: turned ON + always-on → start now
            } else if (wasEnabled) {
                GPS::stopUART();   // hot-apply: turned OFF → kill UART
            }
            break;
        }
        case SET_GPS_ALWAYS_ON: {
            bool wasAlwaysOn = Config::getGPSAlwaysOn();
            Config::setGPSAlwaysOn(!wasAlwaysOn);
            if (!wasAlwaysOn && Config::getGPSEnabled()) {
                GPS::startUART();  // hot-apply: switched to always-on + enabled → start
            } else if (wasAlwaysOn) {
                GPS::stopUART();   // hot-apply: switched to sleep → stop (wardrive will restart)
            }
            break;
        }
        case SET_GPS_RX_PIN: {
            Config::setGPSRxPin(GPSPolicy::nextRxPin(Config::getGPSRxPin()));
            if (GPS::isInitialized()) { GPS::stopUART(); GPS::startUART(); }
            break;
        }
        case SET_GPS_TX_PIN: {
            Config::setGPSTxPin(GPSPolicy::nextTxPin(Config::getGPSTxPin()));
            if (GPS::isInitialized()) { GPS::stopUART(); GPS::startUART(); }
            break;
        }
        case SET_GPS_BAUD: {
            uint8_t idx = (Config::getGPSBaudIndex() + 1) % 4;
            Config::setGPSBaudIndex(idx);
            if (GPS::isInitialized()) { GPS::stopUART(); GPS::startUART(); }
            break;
        }
        case SET_C5_ENABLED: {
            Config::setC5Enabled(!Config::getC5Enabled());
            restartC5Bridge();
            break;
        }
        case SET_C5_RX_PIN: {
            uint8_t rxPin = nextC5RxPin(Config::getC5RxPin());
            uint8_t txPin = Config::getC5TxPin();
            normalizeC5Pins(rxPin, txPin);
            Config::setC5RxPin(rxPin);
            Config::setC5TxPin(txPin);
            restartC5Bridge();
            break;
        }
        case SET_C5_TX_PIN: {
            uint8_t rxPin = Config::getC5RxPin();
            uint8_t txPin = nextC5TxPin(Config::getC5TxPin());
            normalizeC5Pins(rxPin, txPin);
            Config::setC5RxPin(rxPin);
            Config::setC5TxPin(txPin);
            restartC5Bridge();
            break;
        }
        case SET_C5_BAUD: {
            Config::setC5Baud(nextC5Baud(Config::getC5Baud()));
            restartC5Bridge();
            break;
        }
        case SET_MESH_ENABLED: {
            Config::setMeshEnabled(!Config::getMeshEnabled());
            restartMeshBridge();
            break;
        }
        case SET_MESH_PORT: {
            // Setting either leg moves the pair, so one call swaps the port.
            Config::setMeshRxPin(
                MeshUartPolicy::nextRxPin(Config::getMeshRxPin()));
            restartMeshBridge();
            break;
        }
        case SET_MESH_BAUD: {
            Config::setMeshBaud(
                MeshUartPolicy::nextBaud(Config::getMeshBaud()));
            restartMeshBridge();
            break;
        }
        case SET_MESH_CODEC: {
            // The bridge has to come back up on the new dialect: the frame
            // assembler and the line assembler cannot share a session, and a
            // roster built by one means nothing to the other.
            Config::setMeshCodec(
                MeshUartPolicy::nextCodec(Config::getMeshCodec()));
            restartMeshBridge();
            // The matching setting lives on the far end, and nothing on this
            // device can read it or set it. Saying so here is the only warning
            // there is going to be.
            Display::showToast(
                Config::getMeshCodec() == MeshUartPolicy::Codec::PROTO
                    ? "S3T C6L S3R14L.M0D3=PR0T0"
                    : "S3T C6L S3R14L.M0D3=T3XTMSG",
                2200);
            break;
        }
        case SET_WL_SLOT0: case SET_WL_SLOT1: case SET_WL_SLOT2: {
            uint8_t idx = item - SET_WL_SLOT0;
            Config::WatchlistSlot slot;
            if (Config::getWatchlistSlot(idx, slot)) {
                startTextEdit(item, "W4TCH N4ME", slot.label, 16, 15);
            }
            // empty slot: no action (add from PIG EARS)
            break;
        }
        default:
            break;
    }
}

// ==[ RTC VALIDITY CHECK ]==
static bool isRtcValueValid(const m5::rtc_datetime_t& dt) {
    return (dt.date.year >= 2024 && dt.date.year <= 2099 &&
            dt.date.month >= 1 && dt.date.month <= 12 &&
            dt.date.date >= 1 && dt.date.date <= 31 &&
            dt.time.hours <= 23 && dt.time.minutes <= 59);
}

// ==[ LABEL/VALUE/HINT HELPERS ]==
static void getItemLabelValue(SettingItem item, char* label, char* value, char* hint) {
    memset(label, 0, 16);
    memset(value, 0, 16);
    memset(hint, 0, 48);

    auto dt = M5.Rtc.getDateTime();
    bool rtcOk = isRtcValueValid(dt);
    
    // helper: format text value for menu display
    auto fmtTextVal = [](char* out, const char* src, bool isMasked) {
        if (!src || src[0] == '\0') {
            snprintf(out, 16, "%s", "<empty>");
            return;
        }
        if (isMasked) {
            snprintf(out, 16, "%s", "****");
            return;
        }
        size_t len = strlen(src);
        if (len > 8) {
            snprintf(out, 16, "...%s", src + len - 5);
        } else {
            snprintf(out, 16, "%s", src);
        }
    };

    switch (item) {
        case SET_HAMLET_NAME:
            strcpy(label, "P1G 1D");
            strncpy(value, Config::getHamletName(), 15);
            value[15] = '\0';
            strcpy(hint, "4-CH4R H4ML3T T4G.");
            break;
        case SET_HEAD_STYLE:
            strcpy(label, "H34D F1T");
            strncpy(value, getHeadStyleName(Config::getPigHeadStyle()), 15);
            value[15] = '\0';
            strcpy(hint, "H41R C0L0R 0R F3D0R4.");
            break;
        case SET_WIFI_SSID:
            strcpy(label, "WIFI SSID");
            fmtTextVal(value, Config::getUploadWifiSsid(), false);
            strcpy(hint, "N3TW0RK F0R UPL04D.");
            break;
        case SET_WIFI_PASS:
            strcpy(label, "WIFI PASS");
            fmtTextVal(value, Config::getUploadWifiPass(), true);
            strcpy(hint, "W1F1 P4SSW0RD. M4SK3D.");
            break;
        case SET_WPASEC_KEY:
            strcpy(label, "API KEY");
            fmtTextVal(value, Config::getWpaSecKey(), true);
            strcpy(hint, "WP4-S3C 4PI K3Y.");
            break;
        case SET_WIGLE_USER:
            strcpy(label, "WIGLE USER");
            fmtTextVal(value, Config::getWigleUsername(), false);
            strcpy(hint, "W1GL3 4PI N4M3.");
            break;
        case SET_WIGLE_TOKEN:
            strcpy(label, "WIGLE TOKEN");
            fmtTextVal(value, Config::getWigleToken(), true);
            strcpy(hint, "W1GL3 4PI T0K3N. M4SK3D.");
            break;
        case SET_THEME:
            strcpy(label, "SK1N HU");
            strncpy(value, Display::getCurrentThemeName(), 15);
            strcpy(hint, "HU3 R0T4T10N.");
            break;
        case SET_THEME_STYLE: {
            strcpy(label, "SK1N ST");
            static const char* STYLE_NAMES[] = { "D4RK", "1NV3RT", "R3TR0", "M0N0", "N0STR0M0", "THE OG" };
            static_assert(sizeof(STYLE_NAMES) / sizeof(STYLE_NAMES[0]) == THEME_STYLE_COUNT,
                          "theme style names drifted");
            strncpy(value, STYLE_NAMES[Display::getCurrentStyle()], 15);
            value[15] = '\0';
            strcpy(hint, "B4S3 M0D3. D4RK 2 BL1ND.");
            break;
        }
        case SET_ACCENT_MODE: {
            strcpy(label, "GL0W TYP");
            strncpy(value, Display::getAccentModeName(), 15);
            value[15] = '\0';
            strcpy(hint, "L1GHT HU3 F4M1LY.");
            break;
        }
        case SET_LIGHT_INT: {
            strcpy(label, "GL0W LV");
            static const char* INT_NAMES[] = { "0FF", "L0", "M3D", "H1" };
            strncpy(value, INT_NAMES[Config::getLightIntensity()], 15);
            value[15] = '\0';
            strcpy(hint, "N30N BR1GHTN3SS B00ST.");
            break;
        }
        case SET_BRIGHT:
            strcpy(label, "BRIGHT");
            snprintf(value, 16, "%d%%", Config::getBrightness());
            strcpy(hint, "BURN RETINAS OR SAVE POWER.");
            break;
        case SET_VOLUME:
            strcpy(label, "SFX VOL");
            snprintf(value, 16, "%d", SFX::getVolume());
            strcpy(hint, "SFX V0LUM3 0-10. 0 = MUT3D.");
            break;
        case SET_MUSIC:
            strcpy(label, "MUS1C");
            snprintf(value, 16, "%d", NoirJazz::getVolume());
            strcpy(hint, "N01R JAZZ 0-10. 0 = S1L3NC3.");
            break;
        case SET_HAPTIC_INT:
            strcpy(label, "HAPTIC");
            snprintf(value, 16, "%d", Haptic::getIntensity());
            strcpy(hint, "RUMBL3 0-10. 0 = 0FF.");
            break;
        case SET_DIM_AFTER:
            strcpy(label, "DIM AFTER");
            if (Config::getDimTimeout() == 0) strcpy(value, "NEVER");
            else snprintf(value, 16, "%ds", Config::getDimTimeout());
            strcpy(hint, "AUTO-DIM DELAY. NEVER = OFF.");
            break;
        case SET_DIM_LEVEL:
            strcpy(label, "DIM LEVEL");
            if (Config::getDimLevel() == 0) strcpy(value, "OFF");
            else snprintf(value, 16, "%d%%", Config::getDimLevel());
            strcpy(hint, "HOW DARK IS DARK.");
            break;
        case SET_ROTATE_180:
            strcpy(label, "ROTATE");
            strcpy(value, Config::getDisplayRotate180() ? "180" : "0");
            strcpy(hint, "FLIP SCREEN 180 DEG.");
            break;
        case SET_LED_AMBIENT:
            strcpy(label, "LED GL0W");
            strcpy(value, Config::getLedAmbient() ? "ON" : "OFF");
            strcpy(hint, "M5G0 B0TT0M2 4MB1ENT L3DS.");
            break;
        case SET_LED_COLOR: {
            strcpy(label, "LED C0L0R");
            static const char* LED_COLOR_NAMES[] = {
                "4UT0", "TH3M3",
                "R3D", "AMB3R", "Y3LL0", "L1M3", "GR33N", "T34L",
                "CY4N", "4ZUR3", "BLU3", "V10L3T", "M4G3NTA", "R0S3"
            };
            uint8_t ci = Config::getLedColor();
            strncpy(value, LED_COLOR_NAMES[ci < 14 ? ci : 0], 15);
            value[15] = '\0';
            strcpy(hint, ci == 0 ? "S4MPL3 SCR33N 3DG3S." : ci == 1 ? "F0LL0W TH3M3 HU3." : "F1X3D C0L0R PR3S3T.");
            break;
        }
        case SET_LED_BRIGHT:
            strcpy(label, "LED LV");
            snprintf(value, 16, "%d", Config::getLedBrightness());
            strcpy(hint, "L3D BR1GHTN3SS 1-10.");
            break;
        case SET_SHAKE_WAKE:
            strcpy(label, "SHAKE WAKE");
            strcpy(value, Config::getShakeWake() ? "ON" : "OFF");
            strcpy(hint, "SHAKE TO WAKE FROM DIM.");
            break;
        case SET_ALERT_WAKE:
            strcpy(label, "ALERT WAKE");
            strcpy(value, Config::getAlertWake() ? "ON" : "OFF");
            strcpy(hint, "RECON ALERTS WAKE DIMMED SCREEN.");
            break;
        case SET_PARANOIA:
            strcpy(label, "PARANOIA");
            strcpy(value, Config::getParanoiaEnabled() ? "ON" : "OFF");
            strcpy(hint, "TRUST NO SSID. ALERT ALWAYS.");
            break;
        case SET_HOGWASH:
            strcpy(label, "HOGWASH");
            strcpy(value, Config::getWifiChaffEnabled() ? "ON" : "OFF");
            strcpy(hint, "F4K3 H4NDSH4K3 1NJ3CT10N.");
            break;
        case SET_LOOT_PIN:
            strcpy(label, "L00T P1N");
            strcpy(value, Config::hasLootPin() ? "ON" : "OFF");
            strcpy(hint, "4BC P1N G4T3S TH3 T4K3.");
            break;
        case SET_CANARY:
            strcpy(label, "C4N4RY");
            strncpy(value, DefensePipeline::snapshot().getCanarySSID(), 15);
            value[15] = '\0';
            strcpy(hint, "GH0ST B41T SS1D. TR4P.");
            break;
        case SET_FORENSIC_EXPORT:
            strcpy(label, "F0R3NS1C");
            strcpy(value, DefensePipeline::snapshot().isForensicExportEnabled() ? "ON" : "OFF");
            strcpy(hint, "SD C4RD 3V1D3NC3 L0G.");
            break;
        case SET_BATT_ADAPT:
            strcpy(label, "B4TT 4D4P");
            strcpy(value, Power::getBatteryAdaptation() ? "ON" : "OFF");
            strcpy(hint, "AUTO POWER LIMITS; BYPASSED ON VBUS.");
            break;
        case SET_PWR_FPS:
            strcpy(label, "VBUS 60");
            strcpy(value, Config::getPwrFps60() ? "ON" : "OFF");
            strcpy(hint, "FORCE 60 FPS WHILE VBUS IS PRESENT.");
            break;
        case SET_TILT_NAV:
            strcpy(label, "TILT NAV");
            strcpy(value, Config::getTiltNavigationEnabled() ? "ON" : "OFF");
            strcpy(hint, "LEFT/RIGHT/UP TILT GESTURES.");
            break;
        case SET_SPECTRUM_TILT:
            strcpy(label, "SPEC TILT");
            strcpy(value, Config::getSpectrumTiltEnabled() ? "ON" : "OFF");
            strcpy(hint, "UPRIGHT = TILT CHANNEL DIAL.");
            break;
        case SET_ROOM_PARALLAX:
            strcpy(label, "PARALLAX");
            strcpy(value, Config::getRoomParallaxEnabled() ? "ON" : "OFF");
            strcpy(hint, "IMU ROOM DEPTH. OFF HOLDS STILL.");
            break;
        case SET_BATH_MIC:
            strcpy(label, "B4TH M1C");
            strcpy(value, Config::getBathMicEnabled() ? "ON" : "OFF");
            strcpy(hint, "BATH LISTENS; PAUSES SPEAKER.");
            break;
        case SET_YEAR:
            strcpy(label, "YEAR");
            if (rtcOk) snprintf(value, 16, "%d", dt.date.year);
            else strcpy(value, "----");
            strcpy(hint, rtcOk ? "STREAK NEEDS CORRECT YEAR." : "RTC ERROR - CHECK HARDWARE");
            break;
        case SET_DAY:
            strcpy(label, "DAY");
            if (rtcOk) snprintf(value, 16, "%02d", dt.date.date);
            else strcpy(value, "--");
            strcpy(hint, "WHAT DAY EVEN IS IT.");
            break;
        case SET_MONTH:
            strcpy(label, "MONTH");
            if (rtcOk) snprintf(value, 16, "%02d", dt.date.month);
            else strcpy(value, "--");
            strcpy(hint, "MONTHS. THEY BLEND TOGETHER.");
            break;
        case SET_HOUR:
            strcpy(label, "HOUR");
            if (rtcOk) snprintf(value, 16, "%02d", dt.time.hours);
            else strcpy(value, "--");
            strcpy(hint, "TIME IS A LIE. SET ANYWAY.");
            break;
        case SET_MINUTE:
            strcpy(label, "MINUTE");
            if (rtcOk) snprintf(value, 16, "%02d", dt.time.minutes);
            else strcpy(value, "--");
            strcpy(hint, "MINUTES. SECONDS GET NUKED.");
            break;
        case SET_NTP_SYNC:
            strcpy(label, "NTP SYNC");
            switch (clockSyncState) {
                case CLOCK_SYNC_CONNECTING: strcpy(value, "[WAIT]"); break;
                case CLOCK_SYNC_RUNNING:    strcpy(value, "[SYNC]"); break;
                case CLOCK_SYNC_SUCCESS:    strcpy(value, "[OK]"); break;
                case CLOCK_SYNC_FAILED:     strcpy(value, "[FAIL]"); break;
                default:                    strcpy(value, "[EXEC]"); break;
            }
            if (clockSyncState == CLOCK_SYNC_SUCCESS) {
                strcpy(hint, "RTC TRUSTED. UTC SYNCED.");
            } else if (clockSyncState == CLOCK_SYNC_FAILED) {
                snprintf(hint, 48, "SYNC FAILED: %.34s", clockSyncDetail);
            } else if (isClockSyncActive()) {
                snprintf(hint, 48, "%s", clockSyncDetail);
            } else {
                strcpy(hint, "USE SAVED WIFI. SET RTC FROM UTC.");
            }
            break;
        case SET_MUDBALL:
            strcpy(label, "MUDBALL");
            if (!Config::getDeauthEnabled()) strcpy(value, "OFF");
            else if (Config::getDeauthAggressive()) strcpy(value, "AGGR0");
            else strcpy(value, "ON");
            strcpy(hint, "D34UTH: 0FF / 0N / 4GGR0.");
            break;
        case SET_AUTO_PROBE:
            strcpy(label, "AUTO PROBE");
            strcpy(value, Config::getAutoProbe() ? "ON" : "OFF");
            strcpy(hint, "POKE FIRST. ASK LATER.");
            break;
        case SET_PROBE_RSSI:
            strcpy(label, "PROBE dB");
            snprintf(value, 16, "%d", Config::getProbeThreshold());
            strcpy(hint, "TOO WEAK? DON'T BOTHER.");
            break;
        case SET_SAE_ATTACK:
            strcpy(label, "SAE REJ");
            strcpy(value, Config::getSAEAttackEnabled() ? "ON" : "OFF");
            strcpy(hint, "WPA3 REJECT. HUNT FALLBACK.");
            break;
        case SET_EAPOL_INJ:
            strcpy(label, "3P0L INJ");
            strcpy(value, Config::getEAPOLInjectionEnabled() ? "ON" : "OFF");
            strcpy(hint, "PMF BYP4SS. D4T4 FR4M3S.");
            break;
        case SET_CSA_HERD:
            strcpy(label, "CSA H3RD");
            strcpy(value, Config::getCSAEnabled() ? "ON" : "OFF");
            strcpy(hint, "CH4NN3L SW1TCH. H3RD 3M.");
            break;
        case SET_AUTH_FLOOD:
            strcpy(label, "4UTH FLD");
            strcpy(value, Config::getAuthFloodEnabled() ? "ON" : "OFF");
            strcpy(hint, "T4BL3 FL00D. L4ST R3S0RT.");
            break;
        case SET_AUTO_HUNT:
            strcpy(label, "AUTO HUNT");
            strcpy(value, Config::getAutoHuntEnabled() ? "ON" : "OFF");
            strcpy(hint, "WALK 30S = START HUNTING.");
            break;
        case SET_RSSI_SMOOTH:
            strcpy(label, "RSSI SMOOTH");
            switch (Config::getRssiSmooth()) {
                case Config::RSSI_SMOOTH_FAST: strcpy(value, "FAST"); break;
                case Config::RSSI_SMOOTH_MED:  strcpy(value, "MED"); break;
                case Config::RSSI_SMOOTH_SLOW: strcpy(value, "SLOW"); break;
            }
            strcpy(hint, "SIGNAL FILTER. FAST/SMOOTH.");
            break;
        case SET_GHOST_MARK:
            strcpy(label, "GHOST MARK");
            strcpy(value, Config::getGhostMarkerEnabled() ? "ON" : "OFF");
            strcpy(hint, "SHOW LAST KNOWN POSITION.");
            break;
        case SET_NF_ENABLED:
            strcpy(label, "FNOW/3");
            strcpy(value, Config::getNowFlockEnabled() ? "ON" : "OFF");
            strcpy(hint, "ESP-NOW COORDINATION. TRANSMITS.");
            break;
        case SET_NF_GROUP_KEY:
            strcpy(label, "GRP K3Y");
            snprintf(value, 16, "%08lX", (unsigned long)Config::getNowFlockGroupKey());
            strcpy(hint, "FNOW/3 GROUP TAG. TAP TO ROTATE.");
            break;
        case SET_NF_REPORT_S:
            strcpy(label, "R3P0RT S");
            snprintf(value, 16, "%us", (unsigned)Config::getNowFlockReportIntervalS());
            strcpy(hint, "S1GHT1NG 1NTERV4L 2-60S.");
            break;
        case SET_NF_PIGBROTHER:
            strcpy(label, "P1GBR0");
            strcpy(value, Config::getNowFlockPigbrother() ? "ON" : "OFF");
            strcpy(hint, "0PT-1N EXPORT_SNAPSHOT ROLE.");
            break;
        case SET_NF_EXPORT_PROFILE: {
            strcpy(label, "3XPORT");
            uint8_t p = Config::getNowFlockExportProfile();
            if (p == 1) strcpy(value, "HASH");
            else if (p == 2) strcpy(value, "FMH");
            else strcpy(value, "OFF");
            strcpy(hint, "H4SH3D P33R R3PL4Y PR0F1L3.");
            break;
        }
        case SET_NF_BLE_HEARTBEAT:
            strcpy(label, "BLE HB");
            strcpy(value, Config::getNowFlockBleHeartbeat() ? "ON" : "OFF");
            strcpy(hint, "M4NUF AD B34C0N WH3N HUNT G4TES TX.");
            break;
        case SET_REINCARNATE: {
            strcpy(label, "R31NC4RN");
            uint8_t pc = Config::getPrestigeCount();
            bool ready = Config::getLevel() >= 42;
            if (!ready) {
                strcpy(value, "L0CK3D");
                strcpy(hint, "R34CH L3V3L 42 T0 R31NC4RN4T3.");
            } else if (pc == 0) {
                strcpy(value, "[G0]");
                strcpy(hint, "XP R3S3T. L1F3T1M3 ST4TS K3PT. G0LD+ ST4TS.");
            } else {
                snprintf(value, 16, "P%d [G0]", pc);
                strcpy(hint, "XP R3S3T 4G41N. N3XT C0SM3T1C UNL0CK3D.");
            }
            break;
        }
        case SET_IPP_ENABLED:
            strcpy(label, "IPP");
            strcpy(value, Config::getIppEnabled() ? "ON" : "OFF");
            strcpy(hint, "D3F3NS3 M4ST3R G4T3. BLE+W1F1.");
            break;
        case SET_IPP_BLE_SCAN:
            strcpy(label, "IPP BLE");
            strcpy(value, Config::getIppBLEScan() ? "ON" : "OFF");
            strcpy(hint, "B4CKGR0UND TR4CK3R SW33PS.");
            break;
        case SET_IPP_WIFI_SCAN:
            strcpy(label, "IPP WIFI");
            strcpy(value, Config::getIppWifiScan() ? "ON" : "OFF");
            strcpy(hint, "B4CKGR0UND AP + W1DS SW33PS.");
            break;
        case SET_WARDRIVE_BLE:
            strcpy(label, "WD BLE");
            strcpy(value, Config::getWardriveBleScan() ? "ON" : "OFF");
            strcpy(hint, "BLE WARDRIVE. W1GL3 CSV.");
            break;
        case SET_GPS_ENABLE:
            strcpy(label, "R3C31V3R");
            strcpy(value, Config::getGPSEnabled() ? "ON" : "OFF");
#if !HAMLET_TARGET_CORES3SE
            if (Config::getC5Enabled()) {
                strcpy(hint, "C5 OWNS UART2. DISABLE C5.");
            } else {
                strcpy(hint, "M003-V21 AT6668 M-BUS.");
            }
#else
            strcpy(hint, "M003-V21 AT6668 M-BUS.");
#endif
            break;
        case SET_GPS_ALWAYS_ON:
            strcpy(label, "P0W3R");
            strcpy(value, Config::getGPSAlwaysOn() ? "ALWAYS" : "SLEEP");
            strcpy(hint, "ALWAYS=HOT. SLEEP=WD ONLY.");
            break;
        case SET_GPS_RX_PIN:
            strcpy(label, "RX P1N");
            snprintf(value, 16, "G%d D%d", Config::getGPSRxPin(),
                     GPSPolicy::rxDipSwitch(Config::getGPSRxPin()));
            strcpy(hint, "MODULE TX ROUTE. DIP 6-10.");
            break;
        case SET_GPS_TX_PIN:
            strcpy(label, "TX P1N");
            snprintf(value, 16, "G%d D%d", Config::getGPSTxPin(),
                     GPSPolicy::txDipSwitch(Config::getGPSTxPin()));
            strcpy(hint, "MODULE RX ROUTE. DIP 1-5.");
            break;
        case SET_GPS_BAUD:
            strcpy(label, "B4UD");
            snprintf(value, 16, "%lu", (unsigned long)Config::getGPSBaud());
            strcpy(hint, "AT6668 DEFAULT 115200.");
            break;
        case SET_C5_ENABLED:
            strcpy(label, "BR1DG3");
            strcpy(value, Config::getC5Enabled() ? "ON" : "OFF");
#if HAMLET_TARGET_CORES3SE
            strcpy(hint, "C5M0NST3R UART BR1DGE.");
#else
            strcpy(hint, "USES UART2. DISABLE FOR GPS.");
#endif
            break;
        case SET_C5_RX_PIN:
            strcpy(label, "RX P1N");
            snprintf(value, 16, "G%d", Config::getC5RxPin());
            snprintf(hint, 48, "ESP32 RX. G%u D3F4ULT.", c5DefaultRxPin());
            break;
        case SET_C5_TX_PIN:
            strcpy(label, "TX P1N");
            snprintf(value, 16, "G%d", Config::getC5TxPin());
            snprintf(hint, 48, "ESP32 TX. G%u D3F4ULT.", c5DefaultTxPin());
            break;
        case SET_C5_BAUD:
            strcpy(label, "B4UD");
            snprintf(value, 16, "%lu", (unsigned long)Config::getC5Baud());
            snprintf(hint, 48, "%lu D3F4ULT.", (unsigned long)c5DefaultBaud());
            break;
        case SET_MESH_ENABLED:
            strcpy(label, "BR1DG3");
            strcpy(value, Config::getMeshEnabled() ? "ON" : "OFF");
            strcpy(hint, "UN1T C6L L0R4 BR1DG3.");
            break;
        case SET_MESH_PORT:
            strcpy(label, "P0RT");
            strcpy(value,
                   MeshUartPolicy::isDefaultPort(Config::getMeshRxPin()) ? "C"
                                                                         : "B");
            snprintf(hint, 48, "G%u/G%u. C SH4R3S GPS P1NS.",
                     Config::getMeshRxPin(), Config::getMeshTxPin());
            break;
        case SET_MESH_BAUD:
            strcpy(label, "B4UD");
            snprintf(value, 16, "%lu", (unsigned long)Config::getMeshBaud());
            strcpy(hint, "MUST M4TCH S3R14L.BAUD.");
            break;
        case SET_MESH_CODEC:
            strcpy(label, "C0D3C");
            strcpy(value, MeshUartPolicy::codecLabel(Config::getMeshCodec()));
            strcpy(hint, "PR0T0: N0D3S, S1GN4L, 4CKS, DMS.");
            break;
        case SET_WL_SLOT0: case SET_WL_SLOT1: case SET_WL_SLOT2: {
            uint8_t idx = item - SET_WL_SLOT0;
            Config::WatchlistSlot slot;
            snprintf(label, 16, "#%d", idx + 1);
            if (Config::getWatchlistSlot(idx, slot)) {
                strncpy(value, slot.label, 15);
                value[15] = '\0';
                strcpy(hint, "EDIT NAME. CLEAR = REMOVE.");
            } else {
                strcpy(value, "[EMPTY]");
                strcpy(hint, "ADD IN PIG EARS (SWIPE DN).");
            }
            break;
        }
    }
}

// ==[ DRAW FUNCTIONS ]==
static void draw() {
    if (!canvas) return;
    uint16_t fg = Display::getColorFG();
    uint16_t bg = Display::getColorBG();
    
    canvas->fillSprite(bg);
    canvas->setTextColor(fg);
    
    // Top bar
    Display::drawStatusBarTo(canvas, "TUN3 P1G");

    // PIN entry owns the whole surface. Settling that before the root list runs
    // keeps the frame from rendering every settings row and then burying it
    // under a second full-canvas clear.
    if (pinEditing) {
        PinEntry::draw(canvas, Config::hasLootPin() ? "N3W P1N" : "S3T P1N");
        Display::drawUiOverlaysTo(canvas);
        FramePresenter::present(*canvas);
        return;
    }

    // A category drawer owns the body. Drawing the root first only left clipped
    // chevrons and scroll marks peeking around the opaque modal frame.
    if (activeGroup == GROUP_NONE) {
        drawRoot();
    } else {
        drawModal();
    }

    // The preview belongs to the profile flow. Keep it on the root as a short
    // after-view when the drawer closes, but never cover an unrelated drawer.
    if ((activeGroup == GROUP_NONE || activeGroup == GROUP_PROFILE) &&
        TimeMath::active(millis(), headPreviewUntil)) {
        drawHeadPreviewToast();
    }

    // Overlay stack
    Display::drawUiOverlaysTo(canvas);

    FramePresenter::present(*canvas);
}

static void drawRoot() {
    uint16_t fg = Display::getColorFG();
    uint16_t bg = Display::getColorBG();
    uint16_t dim = Display::lerpColor565(fg, bg, 0.58f);

    // Status bar owns "TUN3 P1G" — no duplicate body title.
    canvas->setTextDatum(TL_DATUM);
    
    // Every root row is a peer category. The same name table titles both the
    // root row and its drawer, so vocabulary cannot drift between levels.

    const int hintY = SCREEN_HEIGHT - BOTTOM_BAR_H;
    
    canvas->setTextSize(2);
    
    for (int i = 0; i < kRootVisibleRows && (rootScroll + i) < ROOT_COUNT; i++) {
        int idx = rootScroll + i;
        int y = kRootStartY + i * kRootItemH;
        
        bool isSelected = (idx == rootIdx) && (activeGroup == GROUP_NONE);
        
        if (isSelected) {
            canvas->fillRect(kRootSelectX, y, kRootSelectW, kRootItemH, fg);
            canvas->setTextColor(bg);
            canvas->setCursor(kRootTextX, y + 1);
            canvas->print("> ");
        } else {
            canvas->setTextColor(fg);
            canvas->setCursor(kRootTextX, y + 1);
            canvas->print("  ");
        }
        canvas->print(GROUP_NAMES[ROOT_GROUPS[idx]]);

        canvas->setCursor(kRootValueRightX - 12, y + 1);
        canvas->print(">");
    }
    
    // Scroll indicators (near right edge)
    canvas->setTextSize(1);
    if (rootScroll > 0) {
        canvas->setTextColor(rootIdx == rootScroll ? bg : fg);
        canvas->setCursor(kScrollX, kRootStartY + 3);
        canvas->print("^");
    }
    if (rootScroll + kRootVisibleRows < ROOT_COUNT) {
        const int bottomIdx = rootScroll + kRootVisibleRows - 1;
        canvas->setTextColor(rootIdx == bottomIdx ? bg : fg);
        canvas->setCursor(kScrollX,
                          kRootStartY + (kRootVisibleRows - 1) * kRootItemH + 3);
        canvas->print("v");
    }
    
    // Selected-item cue lives above the replaceable control strip. Gesture
    // discovery may temporarily own the strip, but it must not erase the one
    // line that explains the selected drawer.
    static const char* const hints[] = {
        "P1G 1D 4ND H34D F1T.",
        "TH3M3, GL0W, S0UND, RUMBL3.",
        "SCR33N, L3DS, T1LT, M1C.",
        "W4K3, B4TT3RY, VBUS, CL0CK.",
        "4UT0HUNT, F1LT3RS, W4TCHL1ST.",
        "GPS M0DUL3 4ND W4RDR1V3 SC4N.",
        "4UTH0R1Z3D 4CT1V3 T00LS.",
        "THR34T D3T3CT10N 4ND 3V1D3NC3.",
        "W1F1, WP4-S3C, W1GL3 CR3DS.",
        "M3SH K3Y, 3XP0RT, H34RTB34T.",
        "C5 4ND C6L S3R14L L1NKS.",
        "D3STRUCT1V3 4ND D3BUG 4CT10NS."
    };

    static_assert(sizeof(hints) / sizeof(hints[0]) == ROOT_COUNT,
                  "root hint map drift");

    const char* hint = (rootIdx < ROOT_COUNT) ? hints[rootIdx] : "";
    char position[16];
    snprintf(position, sizeof(position), "%02d/%02d", rootIdx + 1, ROOT_COUNT);
    const int cueY = hintY - 14;
    canvas->fillRect(5, cueY - 5, 38, 11, fg);
    canvas->setTextColor(bg);
    canvas->setTextSize(1);
    canvas->setTextDatum(MC_DATUM);
    canvas->drawString(position, 24, cueY);
    canvas->setTextColor(dim);
    canvas->drawString(hint, 181, cueY);

    if (!Display::drawHintBottomBar(canvas)) {
        Display::drawBottomBar3To(canvas, "[A]UP", "[B]S3L", "[C]DN [C+]3X1T");
    }
    canvas->setTextColor(fg);
    canvas->setTextDatum(TL_DATUM);
}

static void drawHeadPreviewToast() {
    uint16_t fg = Display::getColorFG();
    uint16_t bg = Display::getColorBG();

    const int boxW = 132;
    const int boxH = 116;
    const int boxX = 94;
    const int boxY = 70;
    const int artX = boxX + 4;
    const int artY = boxY + 16;
    const int artW = boxW - 8;
    const int artH = boxH - 34;

    canvas->fillRoundRect(boxX, boxY, boxW, boxH, 6, fg);
    canvas->fillRoundRect(boxX + 2, boxY + 2, boxW - 4, boxH - 4, 5, bg);
    canvas->drawRoundRect(boxX, boxY, boxW, boxH, 6, bg);

    canvas->setTextColor(fg);
    canvas->setTextDatum(MC_DATUM);
    canvas->setTextSize(1);
    canvas->drawString("L00K CH3CK", boxX + boxW / 2, boxY + 9);

    canvas->setClipRect(artX, artY, artW, artH);
    canvas->fillRect(artX, artY, artW, artH, bg);
    uint32_t now = millis();
    Display::PigPalette pigPalette = Display::makePigPalette(fg, bg);
    constexpr int kPreviewPigW = 72;
    constexpr int kPreviewPigH = 52;

    PigRenderPose pose;
    pose.x = snapPigPreview(artX + (artW - kPreviewPigW) / 2);
    pose.y = snapPigPreview(artY + (artH - kPreviewPigH) / 2);
    pose.facing = PigFacing::RIGHT;
    pose.expression = PigExpression::fromState(AvatarState::NEUTRAL, false, false, 0, false);
    pose.limbMode = LimbMode::STANDING;
    pose.tailGlyph = 'z';
    pose.tailOnLeft = true;
    pose.eyeLook = PigEyeLook::FRONT_DOWN;
    pose.detailColor = pigPalette.detail;
    pose.scale = 2;

    Avatar::setRenderTimeOverride(now);
    PigRenderer::drawBody(*canvas, pose, pigPalette.bodyFill, bg);
    PigRenderer::drawLimbs(*canvas, pose, pigPalette.bodyFill, bg, now);
    if (!Avatar::usesFedora()) {
        Avatar::drawHairsAt(*canvas, pose.x, pose.y, 0, true,
                            AvatarState::NEUTRAL, false);
    }
    Avatar::clearRenderTimeOverride();
    canvas->clearClipRect();

    // PigRenderer may have touched datum — re-assert before the caption draw.
    canvas->setTextDatum(MC_DATUM);
    canvas->setTextColor(fg);
    canvas->setTextSize(1);
    canvas->drawString(getHeadStyleName(Config::getPigHeadStyle()),
                       boxX + boxW / 2, boxY + boxH - 11);
    canvas->setTextDatum(TL_DATUM);
}

static void drawModal() {
    uint16_t fg = Display::getColorFG();
    uint16_t bg = Display::getColorBG();
    uint16_t dim = Display::lerpColor565(fg, bg, 0.52f);
    
    // Dark evidence card with one bright title cap. The old full-foreground
    // slab made seven rows compete with the selected row and overwhelmed dark
    // themes; this keeps the same geometry while restoring hierarchy.
    canvas->fillRoundRect(kModalX, kModalY, kModalW, kModalH, 6, bg);
    canvas->drawRoundRect(kModalX, kModalY, kModalW, kModalH, 6, fg);
    canvas->fillRoundRect(kModalX + 1, kModalY + 1, kModalW - 2,
                          kModalTitleH, 5, fg);
    canvas->fillRect(kModalX + 1, kModalY + kModalTitleH - 4,
                     kModalW - 2, 5, fg);
    
    // Modal title — size-2 text (16px) centered in the 20-row title band (boxY..boxY+19)
    // so both top and bottom of the glyph have 2 rows of breathing room before the divider.
    canvas->setTextColor(bg);
    canvas->setTextDatum(TC_DATUM);
    canvas->setTextSize(2);
    canvas->drawString(GROUP_NAMES[activeGroup], kModalX + kModalW / 2,
                       kModalY + 2);
    canvas->drawLine(kModalX + 10, kModalY + kModalTitleH,
                     kModalX + kModalW - 10, kModalY + kModalTitleH, bg);
    canvas->setTextDatum(TL_DATUM);
    
    uint8_t groupSize = GROUP_SIZES[activeGroup];
    canvas->setTextSize(2);
    
    char label[16], value[16], hint[48];
    
    for (int i = 0; i < kModalVisibleRows && (modalScroll + i) < groupSize; i++) {
        int idx = modalScroll + i;
        int y = kModalItemStartY + i * kModalItemH;
        
        SettingItem item = GROUP_ITEMS[activeGroup][idx];

        if (isCaption(item)) {
            const char* caption = captionText(item);
            const int captionW = strlen(caption) * 6;
            const int centerX = kModalX + kModalW / 2;
            const int lineY = y + kModalItemH / 2;
            canvas->setTextColor(dim);
            canvas->drawLine(kModalX + 10, lineY,
                             centerX - captionW / 2 - 7, lineY, dim);
            canvas->drawLine(centerX + captionW / 2 + 7, lineY,
                             kModalX + kModalW - 10, lineY, dim);
            canvas->setTextDatum(MC_DATUM);
            canvas->setTextSize(1);
            canvas->drawString(caption, centerX, lineY);
            canvas->setTextDatum(TL_DATUM);
            canvas->setTextSize(2);
            continue;
        }

        getItemLabelValue(item, label, value, hint);
        
        bool isSelected = (idx == modalIdx);
        
        if (isSelected) {
            canvas->fillRect(kModalX + kModalItemPadX, y,
                             kModalW - (kModalItemPadX * 2), kModalItemH, fg);
            canvas->setTextColor(bg);
            canvas->setCursor(kModalX + kModalTextIndent, y + 1);
            canvas->print("> ");
        } else {
            canvas->setTextColor(fg);
            canvas->setCursor(kModalX + kModalTextIndent, y + 1);
            canvas->print("  ");
        }

        // Truncate label if needed for size 2
        char shortLabel[11];
        strncpy(shortLabel, label, 10);
        shortLabel[10] = '\0';
        canvas->print(shortLabel);

        // Right-align value with collision guard
        int valueTextSize = 2;
        int valWidth = strlen(value) * 12;
        int labelWidth = (strlen(shortLabel) + 2) * 12;  // +2 for "> " prefix
        int labelEndX = kModalX + kModalTextIndent + labelWidth;
        int minGap = 6;
        int valX = kModalX + kModalW - kModalValueMargin - valWidth;
        // Preserve the full value without crossing the frame. Long user-authored
        // values (notably canary/watch labels) step down to the 6px font only
        // when the regular row font cannot keep the label gap.
        if (valX < labelEndX + minGap) {
            valueTextSize = 1;
            valWidth = strlen(value) * 6;
            valX = kModalX + kModalW - kModalValueMargin - valWidth;
        }
        if (valX < labelEndX + minGap) valX = labelEndX + minGap;
        canvas->setTextSize(valueTextSize);
        canvas->setCursor(valX, y + (valueTextSize == 2 ? 1 : 5));
        canvas->print(value);
        canvas->setTextSize(2);
    }
    
    // Scroll indicators inside modal (right side)
    canvas->setTextSize(1);
    if (modalScroll > 0) {
        canvas->setTextColor(modalIdx == modalScroll ? bg : fg);
        canvas->setCursor(kModalX + kModalW - 12, kModalItemStartY + 4);
        canvas->print("^");
    }
    if (modalScroll + kModalVisibleRows < groupSize) {
        const int bottomIdx = modalScroll + kModalVisibleRows - 1;
        canvas->setTextColor(modalIdx == bottomIdx ? bg : fg);
        canvas->setCursor(kModalX + kModalW - 12,
                          kModalItemStartY + (kModalVisibleRows - 1) * kModalItemH + 4);
        canvas->print("v");
    }

    // Reclaim the detail/control lane below the framed category list.
    const int bottomY = SCREEN_HEIGHT - BOTTOM_BAR_H;
    const int detailTop = kModalY + kModalH + 2;
    canvas->fillRect(0, kModalY + kModalH, SCREEN_WIDTH,
                     bottomY - (kModalY + kModalH), bg);

    char selectedLabel[16], selectedValue[16], selectedHint[48];
    SettingItem selectedItem = GROUP_ITEMS[activeGroup][modalIdx];
    getItemLabelValue(selectedItem, selectedLabel, selectedValue, selectedHint);
    canvas->setTextColor(Display::lerpColor565(fg, bg, 0.58f));
    canvas->setTextSize(1);
    canvas->setTextDatum(MC_DATUM);
    canvas->drawString(selectedHint, SCREEN_WIDTH / 2, detailTop + 7);

    if (selectedItem >= SET_SHAKE_WAKE && selectedItem <= SET_PWR_FPS) {
        char status[48];
        snprintf(status, sizeof(status), "VBUS:%s  ADAPT:%s  FPS:%u",
                 Power::isExternalPowerPresent() ? "ON" : "OFF",
                 Power::getAdaptationStateLabel(),
                 static_cast<unsigned>(Power::getTargetFPS()));
        canvas->setTextColor(fg);
        canvas->drawString(status, SCREEN_WIDTH / 2, detailTop + 23);
    }

    if (selectedItem == SET_NTP_SYNC && isClockSyncActive()) {
        Display::drawBottomBar3To(canvas, "", "W41T...", "[C+]C4NC3L");
    } else {
        Display::drawBottomBar3To(canvas, "[A]UP", "[B]CH4NG3", "[C]DN [C+]B4CK");
    }
    canvas->setTextColor(fg);
    canvas->setTextDatum(TL_DATUM);
}

static void drawLegalWarning() {
    uint16_t fg = Display::getColorFG();
    uint16_t bg = Display::getColorBG();
    uint32_t now = millis();
    
    canvas->fillSprite(bg);
    canvas->setTextColor(fg);
    Display::drawStatusBarTo(canvas, "TUN3 P1G");

    int boxW = 260;
    int boxH = 100;
    int boxX = (SCREEN_WIDTH - boxW) / 2;
    int boxY = 20;

    canvas->fillRoundRect(boxX, boxY, boxW, boxH, 6, fg);
    canvas->drawRoundRect(boxX, boxY, boxW, boxH, 6, bg);

    canvas->setTextColor(bg);
    canvas->setTextDatum(MC_DATUM);
    canvas->setTextSize(1);

    int lineY = boxY + 14;
    if ((now / 400) % 2 == 0) {
        canvas->drawString("!! L3G4L W4RN1NG !!", boxX + boxW/2, lineY);
    } else {
        canvas->drawString("** L3G4L W4RN1NG **", boxX + boxW/2, lineY);
    }

    lineY += 14;
    canvas->drawString("MUDB4LL = D34UTH FR4M3S.", boxX + boxW/2, lineY);
    lineY += 11;
    canvas->drawString("Y0UR N3TW0RK. Y0UR L4B.", boxX + boxW/2, lineY);
    lineY += 11;
    canvas->drawString("Y0UR L4WY3RS. N0T 0URS.", boxX + boxW/2, lineY);
    lineY += 14;
    canvas->drawString("\"P1G H4S 4RT1CL3 5.\"", boxX + boxW/2, lineY);
    lineY += 14;
    canvas->drawString("[B] 1 C0NS3NT", SCREEN_WIDTH / 2, lineY);
    canvas->setTextDatum(TR_DATUM);
    canvas->drawString("[C+] N4H", SCREEN_WIDTH - 4, lineY);

    canvas->setTextDatum(TL_DATUM);
    Display::drawUiOverlaysTo(canvas);
    FramePresenter::present(*canvas);
}

static void drawReincarnateWarning() {
    uint16_t fg = Display::getColorFG();
    uint16_t bg = Display::getColorBG();
    uint32_t now = millis();

    canvas->fillSprite(bg);
    canvas->setTextColor(fg);
    Display::drawStatusBarTo(canvas, "R31NC4RN4T3");

    int boxW = 270;
    int boxH = 120;
    int boxX = (SCREEN_WIDTH - boxW) / 2;
    int boxY = 22;

    canvas->fillRoundRect(boxX, boxY, boxW, boxH, 6, fg);
    canvas->drawRoundRect(boxX, boxY, boxW, boxH, 6, bg);

    canvas->setTextColor(bg);
    canvas->setTextDatum(MC_DATUM);
    canvas->setTextSize(1);

    int cx = boxX + boxW / 2;
    int lineY = boxY + 13;
    if ((now / 400) % 2 == 0) {
        canvas->drawString("!! R31NC4RN4T10N !!", cx, lineY);
    } else {
        canvas->drawString("** R31NC4RN4T10N **", cx, lineY);
    }

    uint8_t pc = Config::getPrestigeCount();
    char pBuf[32];
    snprintf(pBuf, sizeof(pBuf), "PR3ST1G3 %d -> %d", pc, pc + 1);
    lineY += 13;
    canvas->drawString(pBuf, cx, lineY);

    // show what cosmetic unlocks next
    const char* unlock = nullptr;
    if (pc == 0) unlock = "G0LD + F3D0R4 ST4T";
    else if (pc == 1) unlock = "HYP3+ R41NB0W ST4T";
    else unlock = "P3RM4N3NT 3LD3R HYP3";
    lineY += 11;
    canvas->drawString(unlock, cx, lineY);

    lineY += 12;
    canvas->drawString("XP R3S3T. T0T4L ST4TS K3PT.", cx, lineY);
    lineY += 11;
    canvas->drawString("C0SM3T1CS K3PT. N0 UND0.", cx, lineY);
    lineY += 14;
    // Destructive: hold-to-confirm (see hamlet.cpp SettingsMenu::acceptWarning gate).
    canvas->drawString("[B+] R31NC4RN4T3", cx, lineY);
    canvas->setTextDatum(MR_DATUM);
    canvas->drawString("[C+] N0P3", SCREEN_WIDTH - 4, lineY);
    canvas->setTextDatum(MC_DATUM);

    canvas->setTextDatum(TL_DATUM);
    Display::drawUiOverlaysTo(canvas);
    FramePresenter::present(*canvas);
}

} // namespace SettingsMenu
