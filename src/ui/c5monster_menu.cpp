/**
 * C5Monster Command Menu — Implementation
 *
 * ==[ DUAL-BAND CONTROL ]== Interactive menu for JanOS CLI commands.
 * The left pane sends a reviewed command subset; the right pane shows bounded
 * UART scrollback. Touch and button navigation share the global two-color
 * theme, while the bridge owns connection and timeout truth.
 */

#include "c5monster_menu.h"
#include "display.h"
#include "ui_measurements.h"
#include "../radio/c5monster_uart.h"
#include "../radio/c5monster_protocol.h"
#include "../core/config.h"
#include "../audio/sfx.h"
#include "../util/debug_log.h"
#include "../hamlet.h"

namespace C5Menu {

// ==[ MENU ITEMS ]==
enum MenuItem : uint8_t {
    MI_SCAN,
    MI_SNIFFER,
    MI_SNIFFER_NOSCAN,
    MI_DEAUTH,
    MI_DEAUTH_DETECTOR,
    MI_HANDSHAKE,
    MI_SAVE_HANDSHAKE,
    MI_EVIL_TWIN,
    MI_PORTAL,
    MI_SAE_OVERFLOW,
    MI_KARMA,
    MI_BLACKOUT,
    MI_SNIFFER_DOG,
    MI_LIST_HOSTS,
    MI_LIST_PROBES,
    MI_SCAN_BT,
    MI_CHANNEL_VIEW,
    MI_WARDRIVE,
    MI_STOP,
    MI_COUNT
};

struct MenuEntry {
    const char* label;
    const char* cmd;
    bool needsSSID;
};

static const MenuEntry MENU_ENTRIES[MI_COUNT] = {
    { "Scan Networks (5G)",    C5Protocol::CMD_SCAN_NETWORKS,      false },
    { "Start Sniffer",         C5Protocol::CMD_START_SNIFFER,      false },
    { "Start Sniffer NoScan",   C5Protocol::CMD_START_SNIFFER_NOSCAN, false },
    { "Start Deauth",          C5Protocol::CMD_START_DEAUTH,       false },
    { "Deauth Detector",       C5Protocol::CMD_DEAUTH_DETECTOR,    false },
    { "Start Handshake",       C5Protocol::CMD_START_HANDSHAKE,    false },
    { "Save Handshake",        C5Protocol::CMD_SAVE_HANDSHAKE,     false },
    { "Evil Twin",             C5Protocol::CMD_START_EVIL_TWIN,    false },
    { "Portal (custom SSID)",  nullptr,                             true  },
    { "WPA3 SAE Overflow",     C5Protocol::CMD_SAE_OVERFLOW,       false },
    { "Karma Attack",          C5Protocol::CMD_START_KARMA,        false },
    { "Blackout Mode",         C5Protocol::CMD_START_BLACKOUT,     false },
    { "SnifferDog",            C5Protocol::CMD_START_SNIFFER_DOG,  false },
    { "List Hosts",            C5Protocol::CMD_LIST_HOSTS,         false },
    { "List Probes",           C5Protocol::CMD_LIST_PROBES,        false },
    { "Scan BT",               C5Protocol::CMD_SCAN_BT,            false },
    { "Channel View",          C5Protocol::CMD_CHANNEL_VIEW,       false },
    { "Start Wardrive",        C5Protocol::CMD_START_WARDRIVE,     false },
    { ">>> STOP <<<",          C5Protocol::CMD_STOP,               false },
};

// ==[ LAYOUT ]==
static constexpr int MENU_X = 4;
static constexpr int MENU_Y = UIMeasurements::kTopBarH + 2;
static constexpr int MENU_W = 150;
static constexpr int MENU_ROW_H = 18;
static constexpr int MENU_VISIBLE = 9;
static constexpr int OUTPUT_X = 160;
static constexpr int OUTPUT_Y = UIMeasurements::kTopBarH + 2;
static constexpr int OUTPUT_W = 156;
static constexpr int OUTPUT_H = 200;
static constexpr int OUTPUT_PAD = 4;

// ==[ STATE ]==
static bool active = false;
static int8_t selectedIdx = 0;
static int8_t scrollOffset = 0;
static uint32_t lastScanRequestMs = 0;
static constexpr uint32_t SCAN_INTERVAL_MS = 8000;
static constexpr uint32_t SCAN_RESULTS_DELAY_MS = 4000;  // wait for scan to complete
static bool scanPending = false;

// ==[ INTERNAL ]==

void scrollUp() {
    if (selectedIdx > 0) {
        selectedIdx--;
        if (selectedIdx < scrollOffset) scrollOffset = selectedIdx;
    }
}

void scrollDown() {
    if (selectedIdx < MI_COUNT - 1) {
        selectedIdx++;
        if (selectedIdx >= scrollOffset + MENU_VISIBLE) scrollOffset = selectedIdx - MENU_VISIBLE + 1;
    }
}

static void executeCommand(uint8_t idx) {
    if (idx >= MI_COUNT) return;
    if (!Config::getC5Enabled()) {
        Display::showToast("ENABLE C5 IN CONFIG", 1600);
        return;
    }
    
    const MenuEntry& entry = MENU_ENTRIES[idx];
    bool sent = false;
    
    if (entry.needsSSID) {
        const char* name = Config::getHamletName();
        if (!name || !name[0]) name = "PANCETTA";
        char cmd[64];
        snprintf(cmd, sizeof(cmd), "%s%s", C5Protocol::CMD_START_PORTAL, name);
        sent = C5Monster::sendCommand(cmd);
    } else if (entry.cmd) {
        sent = C5Monster::sendCommand(entry.cmd);
    }
    
    if (sent) {
        SFX::click();
    } else {
        Display::showToast("C5 COMMAND NOT SENT", 1400);
    }
}

// ==[ PUBLIC API ]==

void enter() {
    active = true;
    selectedIdx = 0;
    scrollOffset = 0;
    scanPending = false;
    lastScanRequestMs = 0;
    C5Monster::clearOutputLog();
    
    // The settings toggle owns UART activation. On Core2 this also makes the
    // UART2 choice between C5 and local GPS explicit.
    if (Config::getC5Enabled() && !C5Monster::isConnected()) {
        Serial.println("[C5MENU] starting bridge & probing...");
        C5Monster::begin(Config::getC5RxPin(), Config::getC5TxPin(), Config::getC5Baud());
        C5Monster::probe();
    }
}

void exit() {
    active = false;
}

void update() {
    if (!active) return;
    // Scan triggering is centralized in hamlet.cpp to avoid racing
    // with spectrum auto-scan. The menu only displays results.
}

void draw(M5Canvas& canvas) {
    if (!active) return;
    
    uint16_t fg = Display::getColorFG();
    uint16_t bg = Display::getColorBG();
    uint16_t dim = Display::lerpColor565(fg, bg, 0.48f);
    
    canvas.fillSprite(bg);
    canvas.setTextDatum(TL_DATUM);
    
    // ==[ HEADER ]== - use standard status bar
    Display::drawStatusBarTo(&canvas, "C5 M0NST3R");
    
    // ==[ MENU LIST ]==
    for (uint8_t i = 0; i < MENU_VISIBLE && (scrollOffset + i) < MI_COUNT; i++) {
        uint8_t idx = scrollOffset + i;
        int y = MENU_Y + 12 + i * MENU_ROW_H;
        
        bool isSelected = (idx == selectedIdx);
        
        if (idx == MI_STOP) {
            if (isSelected) {
                canvas.fillRect(MENU_X - 2, y - 1, MENU_W, MENU_ROW_H - 1, fg);
                canvas.setTextColor(bg, fg);
            } else {
                canvas.setTextColor(fg, bg);
            }
        } else if (isSelected) {
            canvas.fillRect(MENU_X - 2, y - 1, MENU_W, MENU_ROW_H - 1, fg);
            canvas.setTextColor(bg, fg);
        } else {
            canvas.setTextColor(fg, bg);
        }
        
        canvas.drawString(MENU_ENTRIES[idx].label, MENU_X + 2, y + 3);
    }
    
    // Scroll indicators
    if (scrollOffset > 0) {
        canvas.setTextColor(dim, bg);
        canvas.drawString("^", MENU_X + MENU_W - 8, MENU_Y + 12);
    }
    if (scrollOffset + MENU_VISIBLE < MI_COUNT) {
        canvas.setTextColor(dim, bg);
        canvas.drawString("v", MENU_X + MENU_W - 8, MENU_Y + 12 + MENU_VISIBLE * MENU_ROW_H - 4);
    }
    
    // ==[ OUTPUT PANEL ]==
    canvas.drawRect(OUTPUT_X - 1, OUTPUT_Y + 10, OUTPUT_W + 2, OUTPUT_H, dim);
    
    canvas.setTextColor(dim, bg);
    canvas.drawString("OUTPUT", OUTPUT_X + 2, OUTPUT_Y + 12);
    
    const C5Monster::OutputLog& log = C5Monster::getOutputLog();
    int lineY = OUTPUT_Y + 24;
    int lineH = 9;
    int visibleLines = (OUTPUT_H - 16) / lineH;

    if (!Config::getC5Enabled()) {
        canvas.setTextColor(fg, bg);
        canvas.drawString("ENABLE C5 IN CONFIG", OUTPUT_X + OUTPUT_PAD, lineY);
    } else if (!C5Monster::isConnected() && log.count == 0) {
        canvas.setTextColor(fg, bg);
        canvas.drawString("C5 OFFLINE", OUTPUT_X + OUTPUT_PAD, lineY);
    }
    
    uint8_t startIdx = 0;
    if (log.count > (uint8_t)visibleLines) {
        startIdx = log.count - visibleLines;
    }
    
    for (uint8_t i = 0; i < visibleLines && startIdx + i < log.count; i++) {
        uint8_t lineIdx = (log.head + startIdx + i) % C5Monster::OUTPUT_LOG_LINES;
        const char* line = log.lines[lineIdx];
        
        if (strstr(line, "5G")) {
            canvas.setTextColor(Display::hsvToRgb565(Display::getAccentBaseHue(), 180, 255), bg);
        } else if (strstr(line, "ERROR")) {
            canvas.setTextColor(fg, bg);
        } else {
            canvas.setTextColor(fg, bg);
        }
        
        canvas.drawString(line, OUTPUT_X + OUTPUT_PAD, lineY + i * lineH);
    }
    
    // ==[ BOTTOM BAR ]== - use standard 3-column bar with touch hint support
    if (!Display::drawHintBottomBar(&canvas)) {
        uint8_t count5g = C5Monster::get5GHzNetworkCount();
        char leftBuf[32];
        snprintf(leftBuf, sizeof(leftBuf), "5G: %d nets", count5g);
        Display::drawBottomBar3To(&canvas, leftBuf, "OK=run  BACK=exit", nullptr);
    }
    
    // ==[ OVERLAYS ]== - XP notifications, paranoia, help, toasts
    Display::drawUiOverlaysTo(&canvas);
}

void handleBtnOK(bool longPress) {
    if (!active) return;
    
    if (longPress) {
        C5Monster::emergencyStop();
        SFX::tone(440, 200);
    } else {
        executeCommand(selectedIdx);
    }
}

void handleBtnBack(bool longPress) {
    if (!active) return;
    
    if (longPress || selectedIdx >= MI_COUNT) {
        // enterMode owns the handoff: it runs our exit hook, releases the radio
        // per policy, and — the part a bare setMode skipped — actually enters
        // the menu. Leaving here used to land on a MENU that never woke up.
        Hamlet::enterMode(HamletMode::MENU);
    } else {
        scrollUp();
    }
}

void handleTouch(int x, int y) {
    if (!active) return;

    if (x >= MENU_X && x <= MENU_X + MENU_W) {
        for (uint8_t i = 0; i < MENU_VISIBLE && (scrollOffset + i) < MI_COUNT; i++) {
            int rowY = MENU_Y + 12 + i * MENU_ROW_H;
            if (y >= rowY && y < rowY + MENU_ROW_H) {
                selectedIdx = scrollOffset + i;
                executeCommand(selectedIdx);
                return;
            }
        }
    }

    // The output pane used to fake a drag by comparing against the previous
    // tap's Y. Taps are discrete, so the comparison was against a coordinate
    // from an unrelated moment, and it moved the *command* selection rather
    // than the log. Vertical swipes own list movement now.
}

bool isActive() {
    return active;
}

} // namespace C5Menu
