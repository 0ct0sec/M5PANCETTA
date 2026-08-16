/**
 * display.cpp — the shared two-color case desk.
 *
 * Mode screens, status chrome, transient overlays, and final canvas delivery
 * meet here. Domain modules own their evidence; this file decides how that
 * evidence reaches a 320x240 panel without two presenters claiming one frame.
 */

#include "display.h"
#include <M5Unified.h>
#include <math.h>
#include <time.h>
#include "../hamlet.h"
#include "../modes/hunt.h"
#include "../modes/spectrum.h"
#include "../core/capture.h"
#include "../core/config.h"
#include "../core/bounty.h"
#include "../core/mailbox.h"
#include "../core/power.h"
#include "../core/power_policy.h"
#include "../hal/platform.h"
#include "../hal/sd_storage.h"
#include "../core/item_drops.h"
#include "../activity/pedometer.h"
#include "../piglet/mood.h"
#include "../piglet/avatar.h"
#include "../piglet/weather.h"
#include "../sync/nowflock_transport.h"
#include "../sync/nowflock_graph.h"
#include "../sync/nowflock_state.h"
#include "../defense/defense_pipeline.h"
#include "../build_info.h"
#include "../audio/sfx.h"
#include "../net/config_portal.h"
#include "../modes/wardrive.h"
#include "../core/gps.h"
#include "../core/gps_policy.h"
#include "../modes/wardrive_scene.h"
#include "../modes/wardrive_telemetry.h"
#include "../modes/ble_scanner.h"
#include "../modes/defhog4.h"
#include "teleport.h"
#include "menu_pig.h"
#include "../led/ambient_led.h"
#include "../input/touch_hints.h"
#include "help_wiki.h"
#include "item_sprites.h"
#include "frame_presenter.h"
#include "scene_cache.h"
#include "ui_measurements.h"
#include "lore_story.h"
#include "../net/xfer_server.h"
#include "../util/wifi_qr.h"
#include "../util/time_math.h"
#include "../core/achievements.h"
#include "../defense/recon.h"
#include "../defense/xband.h"
#include "c5monster_menu.h"
#include "mesh_menu.h"
#include "../radio/meshtastic_uart.h"

using UIMeasurements::ToastLayout::kToastY;

// Helper to get current time - reads RTC directly for display
static bool getCurrentTime(uint8_t* hours, uint8_t* minutes) {
    if (!M5.Rtc.isEnabled()) return false;
    auto dt = M5.Rtc.getDateTime();
    if (dt.time.hours > 23 || dt.time.minutes > 59) return false;
    *hours = dt.time.hours;
    *minutes = dt.time.minutes;
    return true;
}

static int textPixelWidth(const char* text) {
    return (int)strlen(text) * 6;
}

static void drawStatusBadge(M5Canvas* target, int x, uint16_t fg, uint16_t bg, const char* label) {
    int badgeWidth = textPixelWidth(label) + 4;
    target->fillRect(x, 1, badgeWidth, TOP_BAR_H - 2, fg);
    target->setTextColor(bg);
    target->setCursor(x + 2, 3);
    target->print(label);
    target->setTextColor(fg);
}

// ==[ MODE PLATE ]== the left edge used to read as one undifferentiated run of
// mode, mood, and momentum text while the right side already had crisp status
// badges. Give the current surface the same visual weight without making the
// mood look like another machine state.
static int drawStatusLeft(M5Canvas* target, uint16_t fg, uint16_t bg,
                          const char* mode, const char* mood,
                          const char* momentum = "") {
    const int plateX = 1;
    const int plateW = textPixelWidth(mode) + 4;
    target->fillRect(plateX, 1, plateW, TOP_BAR_H - 2, fg);
    target->setTextColor(bg);
    target->setCursor(plateX + 2, 3);
    target->print(mode);

    const int moodX = plateX + plateW + 3;
    target->setTextColor(fg);
    target->setCursor(moodX, 3);
    target->print(mood);
    target->print(momentum);
    return moodX + textPixelWidth(mood) + textPixelWidth(momentum);
}

// ==[ ONE NAME PER MODE ]== both status bar paths need this and both used to
// carry their own copy of the switch. They had already drifted: P1G P0ST was
// missing from one of them, so the mode named itself correctly on the screens
// that pass an override and read "IDLE" everywhere else — with no build error,
// because a default: catches an unlisted mode in both.
static const char* modeLabel(HamletMode mode) {
    switch (mode) {
        case HamletMode::HUNT: return "HUNT";
        case HamletMode::SPECTRUM: return "SPECTRUM";
        case HamletMode::MENU: return "MENU";
        case HamletMode::LOOT: return "L00T B0X";
        case HamletMode::SETTINGS: return "C0NF1G";
        case HamletMode::NOWFLOCK: return "N0W F0CK";
        case HamletMode::WALK_STATS: return "W4LK ST4TS";
        case HamletMode::ABOUT: return "TH3 L0R3";
        case HamletMode::BLE_SCANNER: return "BLE SC4N";
        case HamletMode::POWER_MENU: return "PWR 0PT10NS";
        case HamletMode::WEBCONFIG: return "C0NF1G M0D3";
        case HamletMode::XFER: return "XF3R M0D3";
        case HamletMode::C5MONSTER: return "C5 M0NST3R";
        case HamletMode::MAIL: return "P1G P0ST";
        case HamletMode::MESH: return "M3SH T4LK";
        default: return "IDLE";
    }
}

// ==[ RIGHT-TO-LEFT BADGE PLACEMENT ]== returns the x to draw at, or -1 when
// the badge would crowd the mode/mood label on the left, and advances the
// cursor only when the badge actually lands. A dropped badge is recoverable;
// an unreadable top bar is not.
static int placeBadge(int& cursorX, int badgeWidth, int gap, int minX) {
    if (badgeWidth <= 0) return -1;
    const int candidateX = cursorX - gap - badgeWidth;
    if (candidateX < minX) return -1;
    cursorX = candidateX;
    return candidateX;
}

static int drawStatusRight(M5Canvas* target, uint16_t fg, uint16_t bg, int leftEndX, bool includeHuntStats) {
    uint8_t batt = Hamlet::getBatteryPercent();
    bool charging = Hamlet::isCharging();

    bool showPwr = Power::isExternalPowerPresent();
    const int pwrBadgeWidth = showPwr ? (4 * 6 + 4) : 0;

    const char* battPrefix = charging ? "+" : (batt <= 10 ? "!" : "");

    uint8_t hours = 0;
    uint8_t minutes = 0;
    bool rtcOk = getCurrentTime(&hours, &minutes);

    char timeBuf[6];
    if (rtcOk) snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d", hours, minutes);
    else strcpy(timeBuf, "--:--");

    char battBuf[12];
    snprintf(battBuf, sizeof(battBuf), "%s%d%%", battPrefix, batt);

    char statsBuf[24] = "";
    if (includeHuntStats) {
        // An unmet bounty replaces the network count while the window is live.
        // The deadline is the whole mechanic; a count the operator can read on
        // the hunt screen anyway is the cheaper thing to drop.
        char lead[16];
        if (!Bounty::statusText(lead, sizeof(lead))) {
            snprintf(lead, sizeof(lead), "N:%03d", Hunt::getActiveNetworkCount());
        }
        snprintf(statsBuf, sizeof(statsBuf), "%s P:%02d HS:%02d", lead,
                 Hunt::getSessionPMKIDs(), Hunt::getSessionHandshakes());
    }

    GPSPolicy::FeedState gpsState = GPSPolicy::classifyFeed(
        GPS::isInitialized(), GPS::hasUARTData(), GPS::hasNMEA(), GPS::hasFix());
    const char* gpsBadge = GPSPolicy::badgeFor(gpsState);
    int gpsBadgeWidth = 0;
    if (gpsBadge) {
        gpsBadgeWidth = textPixelWidth(gpsBadge) + 4;
    }

    // Unread case letters. The badge is the whole pull mechanic for P1G P0ST —
    // an arriving witness no longer seizes the screen, so this is the only
    // standing signal that a file is waiting.
    uint8_t unreadMail = Mailbox::unreadCount();
    char mailBuf[8] = "";
    int mailBadgeWidth = 0;
    if (unreadMail > 0) {
        snprintf(mailBuf, sizeof(mailBuf), "M%u", (unsigned)unreadMail);
        mailBadgeWidth = textPixelWidth(mailBuf) + 4;
    }

    // Traffic waiting on the C6L. The arrival toast is transient and a LoRa
    // message can land while the device is face-down in a pocket, so without
    // a standing mark the whole notification is missable. Carries the
    // menu row's own icon so the badge names where to go. Counts past 9 are
    // read as "several" — the exact number is on the mesh screen.
    uint8_t unreadMesh = Mesh::peekUnread();
    char meshBuf[8] = "";
    int meshBadgeWidth = 0;
    if (unreadMesh > 0) {
        if (unreadMesh > 9) snprintf(meshBuf, sizeof(meshBuf), "))+");
        else snprintf(meshBuf, sizeof(meshBuf), "))%u", (unsigned)unreadMesh);
        meshBadgeWidth = textPixelWidth(meshBuf) + 4;
    }

    const int timeWidth = textPixelWidth(timeBuf);
    const int battWidth = textPixelWidth(battBuf);
    const int statsWidth = textPixelWidth(statsBuf);

    int cursorX = SCREEN_WIDTH - 2;

    const int timeX = cursorX - timeWidth;
    cursorX = timeX;

    cursorX -= 6;
    const int battX = cursorX - battWidth;
    cursorX = battX;

    int pwrX = -1;
    if (showPwr) {
        cursorX -= 3;
        pwrX = cursorX - pwrBadgeWidth;
        cursorX = pwrX;
    }

    const int statsX = placeBadge(
        cursorX, (includeHuntStats && statsBuf[0] != '\0') ? statsWidth : 0, 6,
        leftEndX + 6);

    // The one badge with no left guard: GPS is a mode-defining fact rather than
    // a notification, so it goes on even when the bar is tight.
    int gpsX = -1;
    if (gpsBadge) {
        gpsX = cursorX - 3 - gpsBadgeWidth;
        cursorX = gpsX;
    }

    const int mailX = placeBadge(cursorX, mailBadgeWidth, 3, leftEndX + 4);

    // Furthest left, so this is the first badge to go when the bar fills up:
    // a dropped mesh mark still has the toast and the mesh screen behind it,
    // while P1G P0ST's badge is the only standing signal that mode has.
    const int meshX = placeBadge(cursorX, meshBadgeWidth, 3, leftEndX + 4);

    if (meshX >= 0) {
        drawStatusBadge(target, meshX, fg, bg, meshBuf);
    }

    if (mailX >= 0) {
        drawStatusBadge(target, mailX, fg, bg, mailBuf);
    }

    if (gpsBadge && gpsX >= 0) {
        drawStatusBadge(target, gpsX, fg, bg, gpsBadge);
    }

    if (statsX >= 0) {
        target->setCursor(statsX, 3);
        target->print(statsBuf);
    }

    if (showPwr && pwrX >= 0) {
        drawStatusBadge(target, pwrX, fg, bg, "VBUS");
    }

    target->setCursor(battX, 3);
    target->print(battBuf);

    target->setCursor(timeX, 3);
    target->print(timeBuf);

    return (gpsBadge && gpsX >= 0) ? gpsX : (statsX >= 0 ? statsX : (showPwr && pwrX >= 0 ? pwrX : battX));
}

namespace Display {

// pork calling state
static uint32_t lastRingTime = 0;
static bool ringToggle = false;
static const uint32_t RING_INTERVAL = 400;

// ==[ HSV THEME GENERATOR ]== infinite themes. golden angle hue walk.
// styles: 0=DARK, 1=INVERTED, 2=RETRO, 3=MONO, 4=N0STR0M0, 5=THE OG
// GLDRUN3R = named combo (DARK + RED hue + CL4SH glow + H1 glow level)

struct GeneratedTheme {
    uint16_t fg;
    uint16_t bg;
    char name[9];  // 8 chars + null
};

static uint16_t currentHue = 120;   // 0-359, default green (N0STR0M0 heir)
static uint8_t currentStyle = 0;    // 0-5
static GeneratedTheme cachedTheme = { 0x39E7, 0x0000, "GR33N" };
static bool themeDirty = true;


// leet-speak name from hue angle
static const char* const HUE_NAMES[] = {
    "R3D", "AMB3R", "Y3LL0", "L1M3", "GR33N", "T34L",
    "CY4N", "4ZUR3", "BLU3", "V10L3T", "M4G3NTA", "R0S3"
};


// ==[ ACCENT MODE TABLE ]== cinematic emissive families. idle/truffles stay 2-color.
// Slots: neon sign, practical warmth, CRT phosphor, vending spill.
static constexpr AccentOffsets ACCENT_TABLE[6] = {
    {  0,  35, 185, 305},  // TH3M3 - hue + amber + cyan + magenta
    {  0,   0,   0,   0},  // N30N  - one wet neon tube, no hue noise
    {  0, 180, 150, 210},  // SPL1T - neon vs sodium; CRT/vend bridge the cut
    { 18,  38, 195, 330},  // W4RM  - sodium/peach practicals, cyan CRT, rose spill
    {195, 225, 170, 265},  // C00L  - cyan/blue/violet surveillance light
    {185,  28, 305, 145},  // CL4SH - cyan + amber + magenta + toxic green
};
static const char* const ACCENT_NAMES[] = {
    "TH3M3", "N30N", "SPL1T", "W4RM", "C00L", "CL4SH"
};
static_assert(sizeof(ACCENT_TABLE) / sizeof(ACCENT_TABLE[0]) ==
              sizeof(ACCENT_NAMES) / sizeof(ACCENT_NAMES[0]),
              "accent tables drifted");

const AccentOffsets& getAccentOffsets() {
    uint8_t m = Config::getAccentMode();
    return ACCENT_TABLE[m < 6 ? m : 0];
}
uint8_t getAccentMode() { return Config::getAccentMode(); }
uint8_t getLightIntensity() { return Config::getLightIntensity(); }
const char* getAccentModeName() {
    uint8_t m = Config::getAccentMode();
    return ACCENT_NAMES[m < 6 ? m : 0];
}

static GeneratedTheme generateTheme(uint16_t hue, uint8_t style) {
    GeneratedTheme t;
    hue = hue % 360;

    switch (style) {
        case 0:  // DARK — tinted near-black bg, vivid fg
            t.bg = hsvToRgb565(hue, 80, 25);
            t.fg = hsvToRgb565(hue, 200, 230);
            break;
        case 1:  // INVERTED — vivid bg, near-black fg
            t.bg = hsvToRgb565(hue, 180, 220);
            t.fg = hsvToRgb565(hue, 30, 10);
            break;
        case 2:  // RETRO — muted dark bg, split-complement fg
            t.bg = hsvToRgb565(hue, 120, 55);
            t.fg = hsvToRgb565((hue + 150) % 360, 140, 200);
            break;
        case 3:  // MONO — pure black & white. GH0ST mode
            t.bg = hsvToRgb565(hue, 0, 0);
            t.fg = hsvToRgb565(hue, 0, 255);
            break;
        case 4:  // N0STR0M0 — dimmed CRT phosphor, neon rose pop
            t.bg = hsvToRgb565(225, 45, 18);   // dark blue-gray phosphor
            t.fg = hsvToRgb565(325, 235, 255);  // warm rose-pink neon
            break;
        case 5:  // THE OG — oil-black industrial noir + tobacco amber
            t.bg = hsvToRgb565(210, 80, 18);    // cold oil-black
            t.fg = hsvToRgb565(30, 120, 210);   // aged sodium / tobacco amber
            break;
        default:
            t.bg = hsvToRgb565(hue, 0, 0);
            t.fg = hsvToRgb565(hue, 0, 255);
            break;
    }

    // contrast safety net — ensure fg/bg are visually distinct
    int bfg = brightness565(t.fg);
    int bbg = brightness565(t.bg);
    int contrast = (bfg > bbg) ? (bfg - bbg) : (bbg - bfg);
    if (contrast < 80) {
        if (style == 1) {
            // inverted: darken fg more
            t.fg = hsvToRgb565(hue, 30, 5);
        } else if (style == 2) {
            // retro: brighten fg at split-complement hue
            t.fg = hsvToRgb565((hue + 150) % 360, 140, 255);
        } else if (style == 4) {
            t.fg = hsvToRgb565(325, 255, 255);
        } else if (style == 5) {
            t.fg = hsvToRgb565(30, 120, 255);
        } else {
            // dark: brighten fg
            t.fg = hsvToRgb565(hue, 200, 255);
        }
    }

    // generate name
    if (style == 3) {
        strncpy(t.name, "GH0ST", sizeof(t.name));
    } else if (style == 4) {
        strncpy(t.name, "N0STR0M0", sizeof(t.name));
    } else if (style == 5) {
        strncpy(t.name, "THE OG", sizeof(t.name));
    } else {
        uint8_t nameIdx = (hue * 12) / 360;
        if (nameIdx > 11) nameIdx = 11;
        // GLDRUN3R: DARK + RED hue + CL4SH glow + H1 glow level → terracotta + cyan
        if (style == 0 && nameIdx == 0 && Config::getAccentMode() == 5 && Config::getLightIntensity() == 3) {
            strncpy(t.name, "GLDRUN3R", sizeof(t.name));
            t.bg = hsvToRgb565(185, 40, 10);     // near-black cold tint
            t.fg = hsvToRgb565(20, 220, 200);    // dark terracotta accent
        } else {
            strncpy(t.name, HUE_NAMES[nameIdx], sizeof(t.name));
        }
    }
    t.name[8] = '\0';
    return t;
}

static void ensureTheme() {
    if (themeDirty) {
        cachedTheme = generateTheme(currentHue, currentStyle);
        themeDirty = false;
    }
}

// theme getters
uint16_t getColorFG() {
    ensureTheme();
    return cachedTheme.fg;
}

uint16_t getColorBG() {
    ensureTheme();
    return cachedTheme.bg;
}

bool isInvertedTheme() {
    return currentStyle == 1;
}

bool isTheOgTheme() {
    return currentStyle == 5;
}

uint16_t getCurrentHue() {
    return currentHue;
}

uint16_t getAccentBaseHue() {
    if (currentStyle == 4) return 325;  // N0STR0M0 rose phosphor
    if (currentStyle == 5) return 350;  // THE OG: red neon, sodium, teal CRT, violet spill
    return currentHue;
}

uint8_t getCurrentStyle() {
    return currentStyle;
}

// ==[ SCENERY PIXEL GRID ]== runtime-tunable, default 4px
static int16_t sceneryPX = 4;

int16_t getSceneryPX() { return sceneryPX; }
void setSceneryPX(int16_t px) {
    if (px < 2) px = 2;
    if (px > 6) px = 6;
    sceneryPX = px;
}

const char* getCurrentThemeName() {
    ensureTheme();
    return cachedTheme.name;
}

// golden angle (~137°) — maximal perceptual spread across the wheel
static constexpr uint16_t GOLDEN_STEP = 137;

void nextTheme() {
    currentHue = (currentHue + GOLDEN_STEP) % 360;
    themeDirty = true;
}

void prevTheme() {
    currentHue = (currentHue + 360 - GOLDEN_STEP) % 360;
    themeDirty = true;
}

void nextStyle() {
    currentStyle = (currentStyle + 1) % THEME_STYLE_COUNT;
    themeDirty = true;
}

void setThemeHSV(uint16_t hue, uint8_t style) {
    currentHue = hue % 360;
    currentStyle = style % THEME_STYLE_COUNT;
    themeDirty = true;
}


PigPalette makePigPalette(uint16_t fg, uint16_t bg) {
    PigPalette palette;
    palette.bodyFill = lerpColor565(bg, fg, kPigBodyTone);
    palette.detail = fg;
    palette.voidColor = bg;
    return palette;
}

void themeTintSprite(M5Canvas& sprite, uint16_t transparentKey,
                     uint8_t strength8) {
    uint16_t* pixels = static_cast<uint16_t*>(sprite.getBuffer());
    if (!pixels) return;

    const uint16_t fg = getColorFG();
    const uint16_t bg = getColorBG();
    const int count = sprite.width() * sprite.height();
    for (int i = 0; i < count; i++) {
        // Canvas pixels are byte-swapped in memory; color math is not.
        uint16_t raw = pixels[i];
        uint16_t source = (raw << 8) | (raw >> 8);
        if (source == transparentKey) continue;

        // Keep the darkest source details off the exact background so outlines
        // survive while every visible color remains inside the live theme, then
        // pull only part of the way so the source palette still reads under it.
        uint8_t ink = 24 + ((uint16_t)brightness565(source) * 208u) / 255u;
        uint16_t themed = Gfx::lerpColor565_8(bg, fg, ink);
        pixels[i] = Gfx::toBufferFmt(
            Gfx::lerpColor565_8(source, themed, strength8));
    }
}

// Full map: the source palette is discarded entirely. lerpColor565_8 returns
// the themed color unchanged at strength 255, so this is the tint at its limit.
void themeMapSprite(M5Canvas& sprite, uint16_t transparentKey) {
    themeTintSprite(sprite, transparentKey, 255);
}

// canvas. lazy init. trust issues with statics
static M5Canvas* canvas = nullptr;
static bool overlayVisible = false;
static int powerOption = PowerPolicy::actionIndex(PowerPolicy::SAFE_DEFAULT_ACTION);
static bool showingSleepWarning = false;

// dimming state
static uint32_t lastActivityTime = 0;
static bool dimmed = false;
static bool lowPowerDimmed = false;

enum class BootIntroPhase : uint8_t {
    INACTIVE = 0,
    CATCH_UP,
    LOOP,
    HOLD_FULL,
    TP_CHARGE,
    TP_COLLAPSE,
    TP_VOID
};

struct BootIntroState {
    bool active = false;
    BootIntroPhase phase = BootIntroPhase::INACTIVE;
    uint32_t introStart = 0;
    uint32_t phaseStart = 0;
    uint32_t progressStart = 0;
    uint32_t progressDuration = 0;
    float progressFrom = 0.0f;
    float progressTo = 0.0f;
    float displayedStages = 0.0f;
    uint8_t totalStages = 1;
    uint8_t targetStages = 0;
    uint8_t particleCount = 0;
    uint32_t textSlideOutMs = 0;  // intro-elapsed ms when the title lockup starts leaving
    MenuPig::TeleportParticleSample particles[64] = {};
};

static BootIntroState bootIntro;

static constexpr int kBootLineX = 40;
static constexpr int kBootLineY = 196;
static constexpr int kBootLineW = 240;
static constexpr uint32_t kBootCatchUpMs = 420;
static constexpr uint32_t kBootProgressStepMs = 120;
static constexpr uint32_t kBootMinVisibleMs = 5000;
static constexpr uint32_t kBootFinalSceneHoldMs = 420;
static constexpr uint32_t kBootTeleportChargeMs = 180;
static constexpr uint32_t kBootTeleportCollapseMs = 800;  // match room-to-room DECOMPOSE_MS
static constexpr uint32_t kBootTeleportVoidMs = 150;      // match room-to-room VOID_MS
static constexpr uint32_t kBootTextSlideOutMs = 700;      // duration for HAMLET/PANCETTA exit

// status bar layout cache (used by Spectrum extra widgets)
static int statusBarLeftEndX = 0;
static int statusBarRightStartX = SCREEN_WIDTH;
static uint16_t statusBarLastFg = 0;
static uint16_t statusBarLastBg = 0;

static void drawNetworkOverlay();
void drawSleepWarning();
static void evaluateEffects();
static void drawStatusBarInternal(bool transparent);
static void drawEffectToast(M5Canvas* c, bool transparent = false);
static void formatCompactCount(char* out, size_t outSize, uint32_t value);

// ==[ TOP BAR MESSAGE ]== temporary status override
static char topBarMsg[64] = {0};
static uint32_t topBarMsgStart = 0;
static uint32_t topBarMsgDuration = 0;

// ==[ EFFECT TOAST ]== cycles active buffs/debuffs in status bar
struct ActiveEffect {
    char label[20];
    bool positive;
};
static ActiveEffect activeEffects[4];
static uint8_t effectCount = 0;
static uint32_t lastEffectEval = 0;
static uint32_t effectCycleStart = 0;
static const uint32_t EFFECT_SHOW_MS = 2500;   // per effect
static const uint32_t EFFECT_PAUSE_MS = 8000;  // gap between rotations

// goal progress bar. gamification of walking
static void drawGoalProgressBar(int barH) {
    uint8_t progress = Config::getGoalProgress();

    // Outlined bar, just above bottom bar.
    const int barY = SCREEN_HEIGHT - BOTTOM_BAR_H - barH;
    const int barW = SCREEN_WIDTH;

    // bg clear + fill (no outline — outline breaks PX grid)
    canvas->fillRect(0, barY, barW, barH, getColorBG());
    uint16_t filledWidth = (progress * (uint16_t)barW) / 100;
    if (filledWidth > (uint16_t)barW) filledWidth = barW;
    if (filledWidth > 0) {
        canvas->fillRect(0, barY, filledWidth, barH, getColorFG());
    }
}

// ==[ IDLE INFO BAR ]== inverted strip below top bar — 3-page rotating ticker (4s each)
// page 0: L{n} RANK [XP████░] ~{streak}R
// page 1: P:{pmkids} H:{hs} WD:{nets}  (lifetime captures)
// page 2: {steps}ST {dist}M {files}CF {ach}ACH        (lifetime activity)
static void drawIdleInfoBar() {
    const int barY = TOP_BAR_H;
    const int barH = 11;
    uint16_t fg = getColorFG();
    uint16_t bg = getColorBG();

    // inverted fill
    canvas->fillRect(0, barY, SCREEN_WIDTH, barH, fg);
    canvas->setTextColor(bg);
    canvas->setTextSize(1);
    canvas->setCursor(3, barY + 2);

    uint8_t page = (millis() / 4000) % 5;

    // ==[ THREAT PRIORITY ]== force Recon page when active threats
    if (Config::getIppEnabled() &&
        (DefensePipeline::snapshot().getFollowingCount() > 0 || DefensePipeline::snapshot().hasActiveAttacker() ||
         DefensePipeline::snapshot().isDualBandStalkActive())) {
        page = 4;
    }

    if (page == 0) {
        // identity: level + rank + XP progress bar + streak
        uint8_t level = Config::getLevel();
        const char* rank = Config::getRankName();
        uint8_t xpProg = Config::getXPProgress();
        uint16_t streak = Config::getStreak();

        char leftBuf[20];
        snprintf(leftBuf, sizeof(leftBuf), "L%d %s", level, rank);
        canvas->print(leftBuf);

        char rightBuf[12];
        if (streak > 0) snprintf(rightBuf, sizeof(rightBuf), "~%dR", streak);
        else rightBuf[0] = '\0';
        int rightW = strlen(rightBuf) * 6;

        int leftW = strlen(leftBuf) * 6 + 3;
        int barStartX = leftW + 6;
        int barEndX = SCREEN_WIDTH - rightW - 8;
        int xpBarW = barEndX - barStartX;
        if (xpBarW > 20) {
            int xpBarY = barY + 3;
            int xpBarH = 5;
            canvas->drawRect(barStartX, xpBarY, xpBarW, xpBarH, bg);
            int fillW = ((xpBarW - 2) * xpProg) / 100;
            if (fillW > 0) canvas->fillRect(barStartX + 1, xpBarY + 1, fillW, xpBarH - 2, bg);
        }
        if (rightBuf[0]) {
            canvas->setCursor(SCREEN_WIDTH - rightW - 3, barY + 2);
            canvas->print(rightBuf);
        }

    } else if (page == 1) {
        // lifetime captures: PMKIDs, handshakes, wardrive nets
        char pmkBuf[10], hsBuf[10], wdBuf[10];
        formatCompactCount(pmkBuf, sizeof(pmkBuf), Config::getTotalPMKIDs());
        formatCompactCount(hsBuf,  sizeof(hsBuf),  Config::getTotalHandshakes());
        formatCompactCount(wdBuf,  sizeof(wdBuf),  Config::getWDTotal());
        char buf[64];
        snprintf(buf, sizeof(buf), "P:%s H:%s WD:%s", pmkBuf, hsBuf, wdBuf);
        canvas->print(buf);

    } else if (page == 2) {
        // lifetime activity: steps, distance, case files, achievements
        char stBuf[10], distBuf[10];
        formatCompactCount(stBuf,   sizeof(stBuf),   Config::getTotalSteps());
        formatCompactCount(distBuf, sizeof(distBuf), Config::getTotalDistance());
        uint8_t caseFiles = __builtin_popcount(Config::getSeenAuthTypes());
        uint8_t achCount  = Achievements::getUnlockedCount();
        char buf[48];
        snprintf(buf, sizeof(buf), "%sST %sM %dCF %dACH", stBuf, distBuf, caseFiles, achCount);
        canvas->print(buf);

    } else if (page == 4 && Config::getIppEnabled()) {
        // ==[ RECON STATUS ]== defense telemetry
        int bleSeen = DefensePipeline::snapshot().getTotalBLEDevicesSeen();
        int trackers = DefensePipeline::snapshot().getTrackerCount();
        int following = DefensePipeline::snapshot().getFollowingCount();
        int apCount = DefensePipeline::snapshot().getLastWifiAPCount();
        char buf[54];
        if (DefensePipeline::snapshot().hasActiveAttacker()) {
            snprintf(buf, sizeof(buf), "!! ATK ACTIVE !! TK:%d AP:%d", trackers, apCount);
            // blink: inverted at 500ms
            if ((millis() / 500) & 1) {
                canvas->fillRect(0, barY, SCREEN_WIDTH, barH, bg);
                canvas->setTextColor(fg);
                canvas->setCursor(3, barY + 2);
            }
        } else if (following > 0) {
            snprintf(buf, sizeof(buf), "!! %d FOLLOWING !! TK:%d AP:%d",
                     following, trackers, apCount);
        } else {
            snprintf(buf, sizeof(buf), "BLE:%d TK:%d FOL:%d AP:%d",
                     bleSeen, trackers, following, apCount);
        }
        canvas->print(buf);

    } else {
        // session capture tally, uptime fallback
        uint8_t pmkids = Config::getSessionPMKIDCount();
        uint8_t hs = Config::getSessionHSCount();
        char buf[48];
        if (pmkids > 0 || hs > 0) {
            snprintf(buf, sizeof(buf), "SES: %dP %dH", pmkids, hs);
        } else {
            uint32_t upSec = millis() / 1000;
            uint8_t h = (uint8_t)(upSec / 3600);
            uint8_t m = (uint8_t)((upSec % 3600) / 60);
            snprintf(buf, sizeof(buf), "UP: %dh%02dm", h, m);
        }
        canvas->print(buf);
    }
}

// paranoia toast state
static bool paranoiaToastInverted = false;
static uint32_t lastParanoiaToggle = 0;

// momentum flash
static bool momentumFlashActive = false;
static uint8_t momentumFlashCycles = 0;
static uint32_t lastMomentumFlash = 0;
static const uint32_t MOMENTUM_FLASH_INTERVAL = 100;

// atk latch. brief state needs sticky display
static uint32_t lastATKTime = 0;
static const uint32_t ATK_STICKY_DURATION = 500;

static void drawParanoiaToastTo(M5Canvas* target) {
    if (!target) return;
    if (!Hamlet::isParanoiaToastActive()) return;
    
    uint32_t now = millis();
    
    // Marker blink (500ms). bar stays solid — no full-inversion strobe.
    if (now - lastParanoiaToggle > 500) {
        paranoiaToastInverted = !paranoiaToastInverted;
        lastParanoiaToggle = now;
    }

    const int barH = 12;
    uint16_t barFg = getColorFG();
    uint16_t barBg = getColorBG();

    target->fillRect(0, 0, SCREEN_WIDTH, barH, barBg);

    // Only the marker blinks — bar background stays constant
    const char* marker = paranoiaToastInverted ? "|" : "*";
    char text[32];
    snprintf(text, sizeof(text), "%s DEAUTH DETECTED %s", marker, marker);
    
    target->setTextSize(1);
    target->setTextColor(barFg);
    target->setTextDatum(MC_DATUM);
    target->drawString(text, SCREEN_WIDTH / 2, barH/2);
    target->setTextDatum(TL_DATUM);

    // hint bar. channel + button. no blink
    const int hintH = 10;
    target->fillRect(0, barH, SCREEN_WIDTH, hintH, getColorFG());
    char hint[32];
    snprintf(hint, sizeof(hint), "CH%d %ddB - PRESS BTN",
             Hamlet::getParanoiaChannel(), Hamlet::getParanoiaRSSI());
    target->setTextColor(getColorBG());
    target->setTextDatum(MC_DATUM);
    target->drawString(hint, SCREEN_WIDTH / 2, barH + hintH/2);
    target->setTextDatum(TL_DATUM);
}

static void drawParanoiaToast() {
    drawParanoiaToastTo(canvas);
}

void triggerMomentumFlash(bool positive) {
    momentumFlashActive = true;
    momentumFlashCycles = positive ? 6 : 2;  // 3x blink if good. 1x if bad
    lastMomentumFlash = millis();
}

static void formatCompactCount(char* out, size_t outSize, uint32_t value) {
    if (outSize == 0) return;
    if (value < 1000) {
        snprintf(out, outSize, "%lu", (unsigned long)value);
        return;
    }

    static const char* const suffixes[] = {"", "k", "M", "B"};
    uint32_t divisor = 1000;
    int suffixIdx = 1;

    if (value >= 1000000000UL) {
        divisor = 1000000000UL;
        suffixIdx = 3;
    } else if (value >= 1000000UL) {
        divisor = 1000000UL;
        suffixIdx = 2;
    }

    uint32_t whole = value / divisor;
    uint32_t rem = value % divisor;

    // <100: one decimal. >=100: integer. ux brevity
    if (whole < 100) {
        uint32_t decimal = (rem * 10UL + divisor / 2UL) / divisor;
        if (decimal >= 10) {
            whole += 1;
            decimal = 0;
        }
        // 999.9k -> 1.0M. boundary rollover
        if (whole >= 1000 && suffixIdx < 3) {
            whole = 1;
            decimal = 0;
            suffixIdx++;
        }
        snprintf(out, outSize, "%lu.%lu%s", (unsigned long)whole, (unsigned long)decimal, suffixes[suffixIdx]);
    } else {
        uint32_t rounded = (value + divisor / 2UL) / divisor;
        if (rounded >= 1000 && suffixIdx < 3) {
            rounded = 1;
            suffixIdx++;
        }
        snprintf(out, outSize, "%lu%s", (unsigned long)rounded, suffixes[suffixIdx]);
    }
}

void init() {
    applyRotation();
    
    // theme first. canvas + boot intro need colors
    currentHue = Config::getThemeHue();
    currentStyle = Config::getThemeStyle();
    themeDirty = true;

    M5.Display.fillScreen(getColorBG());
    
    // canvas now. after M5.begin or crash. PSRAM sprite — 150KB at 320×240×16bit
    canvas = new M5Canvas(&M5.Display);
    canvas->setPsram(true);
    canvas->createSprite(SCREEN_WIDTH, SCREEN_HEIGHT);
    canvas->setTextColor(getColorFG());
    canvas->setTextSize(1);
    FramePresenter::init();
    SceneCache::init();

    // brightness. percent to 0-255
    M5.Display.setBrightness(Config::getBrightness() * 255 / 100);
    lastActivityTime = millis();
    dimmed = false;
    lowPowerDimmed = false;
}

M5Canvas* getSharedCanvas() {
    return canvas;
}

void applyRotation() {
    FramePresenter::invalidate();
    uint8_t rotation = Config::getDisplayRotate180() ? 3 : 1;
    M5.Display.setRotation(rotation);
}

static void drawStatusBarInternal(bool transparent) {
    uint32_t now = millis();
    
    // momentum flash. dopamine blink
    bool invertColors = false;
    if (momentumFlashActive) {
        if (now - lastMomentumFlash > MOMENTUM_FLASH_INTERVAL) {
            momentumFlashCycles--;
            lastMomentumFlash = now;
            if (momentumFlashCycles == 0) {
                momentumFlashActive = false;
            }
        }
        // odd cycle = inverted
        invertColors = (momentumFlashCycles % 2 == 1);
    }
    
    uint16_t barBg = invertColors ? getColorFG() : getColorBG();
    uint16_t barFg = invertColors ? getColorBG() : getColorFG();
    statusBarLastFg = barFg;
    statusBarLastBg = barBg;

    if (!transparent) {
        canvas->fillRect(0, 0, SCREEN_WIDTH, TOP_BAR_H, barBg);
    }
    canvas->setTextColor(barFg);
    canvas->setTextSize(1);

    // expire top bar message
    if (topBarMsg[0] != '\0' && topBarMsgDuration > 0) {
        if ((millis() - topBarMsgStart) > topBarMsgDuration) topBarMsg[0] = '\0';
    }

    // top bar message overrides normal content (paranoia toast still wins via overlay)
    if (topBarMsg[0] != '\0') {
        if (!transparent) {
            canvas->fillRect(0, 0, SCREEN_WIDTH, TOP_BAR_H, barFg);
            canvas->setTextColor(barBg);
            statusBarLastFg = barBg;  // message inverts: text=barBg becomes the "fg"
            statusBarLastBg = barFg;  // background=barFg becomes the "bg"
        } else {
            // Sun-composited path: keep top area transparent and render glyphs only.
            canvas->setTextColor(barFg);
            statusBarLastFg = barFg;
            statusBarLastBg = barBg;
        }
        canvas->setTextSize(1);
        canvas->setTextDatum(MC_DATUM);
        canvas->drawString(topBarMsg, SCREEN_WIDTH / 2, TOP_BAR_H / 2);
        canvas->setTextDatum(TL_DATUM);
        return;
    }

    // left: mode + mood + momentum
    const char* modeStr = modeLabel(Hamlet::getMode());

    const char* moodLabel = Mood::getMoodLabel();
    int momentum = Mood::getMomentum();
    const char* momentumIndicator = "";
    
    // hunt mode hides momentum. stats take priority
    if (Hamlet::getMode() != HamletMode::HUNT) {
        if (momentum >= 15) {
            momentumIndicator = " +";
        } else if (momentum <= -25) {
            momentumIndicator = " -";
        }
    }

    statusBarLeftEndX = drawStatusLeft(
        canvas, barFg, barBg, modeStr, moodLabel, momentumIndicator);
    
    statusBarRightStartX = drawStatusRight(
        canvas, barFg, barBg, statusBarLeftEndX,
        Hamlet::getMode() == HamletMode::HUNT);

    // ==[ HELP HINT PILL ]== blinking [?] until user taps help for this mode
    {
        HamletMode hMode = Hamlet::getMode();
        uint8_t hIdx = static_cast<uint8_t>(hMode);
        if (hIdx < HAMLET_MODE_COUNT && !(Config::getHelpWikiSeen() & (1u << hIdx))) {
            int gap = statusBarRightStartX - statusBarLeftEndX;
            if (gap >= 20) {
                bool blinkOn = ((now / 500) % 2) == 0;
                if (blinkOn) {
                    int pillW = 14;
                    int pillH = TOP_BAR_H - 2;
                    int pillX = statusBarLeftEndX + (gap - pillW) / 2;
                    int pillY = 1;
                    canvas->fillRect(pillX, pillY, pillW, pillH, barFg);
                    canvas->setTextColor(barBg);
                    canvas->setCursor(pillX + 4, 3);
                    canvas->print("?");
                    canvas->setTextColor(barFg);
                }
            }
        }
    }

    // effect toast — cycles active buffs/debuffs over normal bar
    drawEffectToast(canvas, transparent);
}

void drawStatusBar() {
    drawStatusBarInternal(false);
}

// status bar helper. for submenus with their own canvas
void drawStatusBarTo(M5Canvas* targetCanvas, const char* modeOverride) {
    uint16_t fg = getColorFG();
    uint16_t bg = getColorBG();
    
    targetCanvas->fillRect(0, 0, SCREEN_WIDTH, TOP_BAR_H, bg);
    targetCanvas->setTextColor(fg);
    targetCanvas->setTextSize(1);

    // expire top bar message
    if (topBarMsg[0] != '\0' && topBarMsgDuration > 0) {
        if ((millis() - topBarMsgStart) > topBarMsgDuration) topBarMsg[0] = '\0';
    }

    // top bar message overrides normal content
    if (topBarMsg[0] != '\0') {
        targetCanvas->fillRect(0, 0, SCREEN_WIDTH, TOP_BAR_H, fg);
        targetCanvas->setTextColor(bg);
        targetCanvas->setTextSize(1);
        targetCanvas->setTextDatum(MC_DATUM);
        targetCanvas->drawString(topBarMsg, SCREEN_WIDTH / 2, TOP_BAR_H / 2);
        targetCanvas->setTextDatum(TL_DATUM);
        return;
    }

    const char* modeStr = modeOverride ? modeOverride : modeLabel(Hamlet::getMode());

    const char* moodLabel = Mood::getMoodLabel();
    int leftEndX = drawStatusLeft(targetCanvas, fg, bg, modeStr, moodLabel);
    drawStatusRight(targetCanvas, fg, bg, leftEndX, false);
}

static int drawBottomBarPlate(M5Canvas* targetCanvas) {
    const int y = SCREEN_HEIGHT - BOTTOM_BAR_H;
    targetCanvas->fillRect(0, y, SCREEN_WIDTH, BOTTOM_BAR_H, getColorFG());
    // One dark scanline separates controls from evidence without spending
    // vertical space on another frame or weakening the high-contrast plate.
    targetCanvas->fillRect(0, y, SCREEN_WIDTH, 1, getColorBG());
    return y;
}

void drawBottomBar(const char* left, const char* right) {
    int y = drawBottomBarPlate(canvas);
    canvas->setTextColor(getColorBG());
    canvas->setTextSize(1);
    
    canvas->setCursor(4, y + 3);
    canvas->print(left);
    
    int rightLen = strlen(right) * 6;
    canvas->setCursor(SCREEN_WIDTH - rightLen - 4, y + 3);
    canvas->print(right);
}

// 3-column bottom bar: A=left, B=center, C=right
void drawBottomBar3To(M5Canvas* targetCanvas,
                      const char* left, const char* center, const char* right) {
    if (!targetCanvas) return;
    int y = drawBottomBarPlate(targetCanvas);
    targetCanvas->setTextColor(getColorBG());
    targetCanvas->setTextSize(1);

    if (left && left[0]) {
        targetCanvas->setTextDatum(ML_DATUM);
        targetCanvas->drawString(left, 4, y + BOTTOM_BAR_H / 2);
    }
    if (center && center[0]) {
        targetCanvas->setTextDatum(MC_DATUM);
        targetCanvas->drawString(center, SCREEN_WIDTH / 2, y + BOTTOM_BAR_H / 2);
    }
    if (right && right[0]) {
        targetCanvas->setTextDatum(MR_DATUM);
        targetCanvas->drawString(right, SCREEN_WIDTH - 4, y + BOTTOM_BAR_H / 2);
    }
    targetCanvas->setTextDatum(TL_DATUM);
}

void drawBottomBar3(const char* left, const char* center, const char* right) {
    drawBottomBar3To(canvas, left, center, right);
}

static void drawBottomBarCentered(const char* center) {
    int y = drawBottomBarPlate(canvas);
    canvas->setTextColor(getColorBG());
    canvas->setTextSize(1);

    int centerWidth = strlen(center) * 6;
    canvas->setCursor((SCREEN_WIDTH - centerWidth) / 2, y + 3);
    canvas->print(center);
}

// ==[ TOUCH HINT BOTTOM BAR ]== blinking gesture labels during hint window
// returns true if hint was drawn (caller should skip normal bar)
static bool drawHintBottomBarTo(M5Canvas* c) {
    if (!TouchHints::isHintActive()) return false;
    const char* barText = TouchHints::getHintBar();
    if (!barText) {
        // blink off phase — draw empty bar
        drawBottomBarPlate(c);
        return true;
    }
    int y = drawBottomBarPlate(c);
    c->setTextColor(getColorBG());
    c->setTextSize(1);
    int textWidth = strlen(barText) * 6;
    c->setCursor((SCREEN_WIDTH - textWidth) / 2, y + 3);
    c->print(barText);
    return true;
}

bool drawHintBottomBar(M5Canvas* targetCanvas) {
    return drawHintBottomBarTo(targetCanvas);
}

// ==[ STATUS PILL ]== loud/quiet two-state badge (see Menu-Design.md)
void drawStatusPillTo(M5Canvas* targetCanvas,
                      int rightX, int rowY,
                      const char* unresolvedLabel,
                      const char* resolvedLabel,
                      bool resolved,
                      bool selected) {
    if (!targetCanvas) return;
    uint16_t fg = getColorFG();
    uint16_t bg = getColorBG();
    uint16_t dim = lerpColor565(fg, bg, 0.5f);
    uint16_t rowFg = selected ? bg : fg;
    uint16_t rowBg = selected ? fg : bg;

    targetCanvas->setTextSize(1);
    if (resolved) {
        // quiet dim text (rowFg on selected rows to stay readable)
        targetCanvas->setTextColor(selected ? rowFg : dim);
        targetCanvas->setTextDatum(TR_DATUM);
        targetCanvas->drawString(resolvedLabel, rightX, rowY);
    } else {
        // loud inverted pill
        int pillW = (int)strlen(unresolvedLabel) * 6 + 4;
        int pillX = rightX - pillW;
        int pillY = rowY - 1;
        targetCanvas->fillRect(pillX, pillY, pillW, 10, selected ? rowFg : fg);
        targetCanvas->setTextColor(selected ? rowBg : bg);
        targetCanvas->setTextDatum(MC_DATUM);
        targetCanvas->drawString(unresolvedLabel, pillX + pillW / 2, pillY + 5);
    }
    targetCanvas->setTextColor(fg);
    targetCanvas->setTextDatum(TL_DATUM);
}

// ==[ HOLD PROGRESS OVERLAY ]== long press feedback
static float holdProgress = 0.0f;
static bool holdRingDrawnOnCanvasFrame = false;

// ==[ QUICK TOAST OVERLAY ]== DEFHOG4 mini console (drawn last)
static char quickToastMsg[96] = {0};
static uint32_t quickToastStart  = 0;   // millis latched at show time
static uint32_t quickToastHoldMs = 0;   // solid readable window
static uint32_t quickToastLifeMs = 0;   // flicker-in + hold + flicker-out
static bool quickToastDrawnOnCanvasFrame = false;
static bool quickToastWasDrawnToDisplay = false;
static bool quickToastNeedsClearRedraw = false;
static bool quickToastIsAlert = false;  // true = render with header as first line
static int16_t quickToastX = 0, quickToastY = 0;
static int16_t quickToastW = 0, quickToastH = 0;
static uint8_t quickToastCorner = 1;  // 0=UL, 1=UR, 2=LL, 3=LR — latched at show time
static uint8_t quickToastUrgent = 0;  // extra strobe cycles ahead of the settle
static bool quickToastPrevVisible = false;  // blink state the panel currently holds
static char quickToastClock[8] = {0};  // header timestamp, latched — the RTC is an I2C trip

// ==[ TOAST FLICKER ]== respawn-invulnerability cadence.
// The toast materializes strobing, the gaps thin out until it sits dead solid
// for the whole readable window, then the gaps crowd back in until it is gone.
// Only the *rate* changes — gap length stays near the 55-70ms floor, because a
// slow blink would hide the copy for a quarter second at a time.
struct ToastFlicker { uint16_t onMs, offMs; };

// 630ms, 7.1 -> 3.4 Hz — decelerating into the hold
static constexpr ToastFlicker kToastFlickIn[] = {
    {70, 70}, {130, 70}, {220, 70},
};
// 1071ms, 2.7 -> 8.8 Hz — accelerating out of it, last gap never reopens
static constexpr ToastFlicker kToastFlickOut[] = {
    {300, 70}, {195, 65}, {125, 60}, {85, 58}, {58, 55},
};
static constexpr int kToastFlickInCount  = sizeof(kToastFlickIn)  / sizeof(kToastFlickIn[0]);
static constexpr int kToastFlickOutCount = sizeof(kToastFlickOut) / sizeof(kToastFlickOut[0]);

// flashCount from callers becomes this many hard strobes before the settle
static constexpr uint16_t kToastUrgentOn  = 60;
static constexpr uint16_t kToastUrgentOff = 60;

static constexpr uint32_t flickerSpan(const ToastFlicker* f, int n) {
    return n <= 0 ? 0u : (uint32_t)f[0].onMs + f[0].offMs + flickerSpan(f + 1, n - 1);
}
static constexpr uint32_t kToastFlickInMs  = flickerSpan(kToastFlickIn,  kToastFlickInCount);
static constexpr uint32_t kToastFlickOutMs = flickerSpan(kToastFlickOut, kToastFlickOutCount);

// Readable window derived from the copy: a terse status ping sits at the floor,
// a wrapped multi-line alert at the ceiling.
static constexpr uint32_t kToastHoldMin  = 3000;
static constexpr uint32_t kToastHoldMax  = 5000;
static constexpr uint32_t kToastHoldBase = 2200;
static constexpr uint32_t kToastHoldPerChar = 34;
static constexpr uint32_t kToastHoldPerLine = 260;

// ==[ ALERT TOAST HEADER ]== noir phrase pool
static const char* const ALERT_HEADERS[] = {
    "pig sense.", "trust issues.", "stay sharp.", "wire tense.", "case open.",
    "heads up.", "flagged.", "incoming.", "eyes on.", "on the radar.", "new intel.", "noted."
};
static constexpr int ALERT_HEADER_COUNT = 12;
static uint8_t lastAlertHeader = 0xFF;  // no-repeat tracking
static bool holdRingWasDrawnToDisplay = false;
static bool holdRingNeedsClearRedraw = false;

// ==[ XP NOTIFICATION STRIP ]== global blink-in/stay/blink-out info strip at y=TOP_BAR_H
// Triggered by any addXP() call. Accumulates over 10s cooldown window.
// Format: "L<n> <RANK> ====== +<n> XP"  threshold: >3 XP always, ≤3 XP 50% chance.
enum class XPNotifPhase : uint8_t { IDLE, BLINK_IN, STAY, BLINK_OUT };
static XPNotifPhase xpNotifPhase      = XPNotifPhase::IDLE;
static uint32_t     xpNotifAmount     = 0;    // XP shown in current notification
static uint32_t     xpNotifPending    = 0;    // accumulating from Config, not yet displayed
static bool         xpNotifVisible    = false; // current blink state (true=draw)
static uint8_t      xpNotifBlinks     = 0;    // half-cycles remaining in blink phase
static uint32_t     xpNotifBlinkNext  = 0;    // millis of next blink state flip
static uint32_t     xpNotifPhaseEnd   = 0;    // millis when STAY phase ends
static uint32_t     xpNotifCooldown   = 0;    // millis: don't fire notification before this
static bool         xpNotifOnCanvas   = false; // set when drawn on canvas this frame

// ==[ HELP WIKI OVERLAY ]== tap status bar to open, tap to dismiss
static bool helpOverlayActive = false;
static HamletMode helpOverlayMode = HamletMode::IDLE;
static uint8_t helpOverlayPage = 0;
static bool helpOverlayDrawnOnCanvasFrame = false;

// ==[ DIRECT PANEL DRAWS ]== overlays that paint onto the LCD after a screen
// has already been presented live outside the tile presenter's hash history.
// The history only ever sees canvas bytes, so the frame that stops drawing one
// of these would compare a settled canvas against itself, mark the tiles clean,
// and leave the overlay burned on the panel. Reporting the footprint forces
// those tiles back through the diff on the next present().
static inline void noteDirectPanelDraw(int x, int y, int w, int h) {
    FramePresenter::invalidateRect(x, y, w, h);
}

// 12-slot hold ring. center stays empty.
static const int HOLD_RING_SEGMENTS = 12;
// Keep ring clear of goal bar (y=215-220).
static const int HOLD_RING_INNER_RADIUS = 18;
static const int HOLD_RING_OUTER_RADIUS = 24;

static inline void drawHoldSegmentToCanvas(M5Canvas* c, float a, uint16_t color) {
    if (!c) return;
    int cx = SCREEN_WIDTH / 2;
    int cy = SCREEN_HEIGHT / 2;
    int x0 = (int)roundf((float)cx + cosf(a) * (float)HOLD_RING_INNER_RADIUS);
    int y0 = (int)roundf((float)cy + sinf(a) * (float)HOLD_RING_INNER_RADIUS);
    int x1 = (int)roundf((float)cx + cosf(a) * (float)HOLD_RING_OUTER_RADIUS);
    int y1 = (int)roundf((float)cy + sinf(a) * (float)HOLD_RING_OUTER_RADIUS);
    c->drawLine(x0, y0, x1, y1, color);
}

static inline void drawHoldSegmentToDisplay(float a, uint16_t color) {
    int cx = SCREEN_WIDTH / 2;
    int cy = SCREEN_HEIGHT / 2;
    int x0 = (int)roundf((float)cx + cosf(a) * (float)HOLD_RING_INNER_RADIUS);
    int y0 = (int)roundf((float)cy + sinf(a) * (float)HOLD_RING_INNER_RADIUS);
    int x1 = (int)roundf((float)cx + cosf(a) * (float)HOLD_RING_OUTER_RADIUS);
    int y1 = (int)roundf((float)cy + sinf(a) * (float)HOLD_RING_OUTER_RADIUS);
    M5.Display.drawLine(x0, y0, x1, y1, color);
}

static inline int holdRingFilledSegments(float progress) {
    if (progress <= 0.0f) return 0;
    int filled = (int)ceilf(progress * (float)HOLD_RING_SEGMENTS);
    if (filled < 1) filled = 1;
    if (filled > HOLD_RING_SEGMENTS) filled = HOLD_RING_SEGMENTS;
    return filled;
}

static void drawHoldRingToCanvas(M5Canvas* c) {
    if (!c) return;
    int filled = holdRingFilledSegments(holdProgress);
    if (filled == 0) return;

    uint16_t active = getColorFG();
    float step = (2.0f * PI) / (float)HOLD_RING_SEGMENTS;
    // Offset by half-step so every segment reads like a slash (no pure | or -).
    float start = -PI * 0.5f + (step * 0.5f);

    // Draw only filled segments.
    for (int i = 0; i < filled; i++) {
        float a = start + step * (float)i;
        drawHoldSegmentToCanvas(c, a, active);
    }
}

static void drawHoldRingToDisplay() {
    int filled = holdRingFilledSegments(holdProgress);
    if (filled == 0) return;
    holdRingWasDrawnToDisplay = true;
    // Segment endpoints sit exactly on the outer radius and drawLine paints
    // them, so the footprint spans [c - r, c + r] inclusive — hence the +1.
    noteDirectPanelDraw(SCREEN_WIDTH / 2 - HOLD_RING_OUTER_RADIUS,
                        SCREEN_HEIGHT / 2 - HOLD_RING_OUTER_RADIUS,
                        HOLD_RING_OUTER_RADIUS * 2 + 1,
                        HOLD_RING_OUTER_RADIUS * 2 + 1);

    uint16_t active = getColorFG();
    float step = (2.0f * PI) / (float)HOLD_RING_SEGMENTS;
    float start = -PI * 0.5f + (step * 0.5f);

    // Draw only filled segments.
    for (int i = 0; i < filled; i++) {
        float a = start + step * (float)i;
        drawHoldSegmentToDisplay(a, active);
    }
}

// ==[ ITEM DROP OVERLAY ]== native PNG sprite; fly in, bounce, center.
static constexpr uint16_t ITEM_DROP_TRANSPARENT_KEY = 0x0001;
static constexpr uint32_t ITEM_DROP_DURATION_MS = 2600;
static constexpr uint32_t ITEM_DROP_FLY_MS = 850;
static constexpr uint32_t ITEM_DROP_BOUNCE_MS = 650;
static constexpr int ITEM_DROP_GRID_PX = 2;
static constexpr int ITEM_DROP_DIALOGUE_PANEL_X = 8;
static constexpr int ITEM_DROP_DIALOGUE_PANEL_W = SCREEN_WIDTH - 16;
static constexpr int ITEM_DROP_DIALOGUE_PANEL_H = 44;
static constexpr int ITEM_DROP_DIALOGUE_PANEL_Y =
    SCREEN_HEIGHT - BOTTOM_BAR_H - ITEM_DROP_DIALOGUE_PANEL_H - 2;
static constexpr int ITEM_DROP_DIALOGUE_BADGE_SIZE = 36;
static constexpr int ITEM_DROP_DIALOGUE_BADGE_X = ITEM_DROP_DIALOGUE_PANEL_X + 5;
static constexpr int ITEM_DROP_DIALOGUE_BADGE_Y = ITEM_DROP_DIALOGUE_PANEL_Y + 4;
static M5Canvas* itemDropSprite = nullptr;
static bool itemDropActive = false;
static bool itemDropFirstTime = false;
static bool itemDropSpriteReady = false;
static bool itemDropDrawnOnCanvasFrame = false;
static bool itemDropNeedsClearRedraw = false;
static uint8_t itemDropItemId = 0;
static uint8_t itemDropSpriteId = 0;
static uint8_t itemDropDecodedId = 0xFF;
static uint16_t itemDropThemeFg = 0;
static uint16_t itemDropThemeBg = 0;
static ItemDrops::ItemDropSource itemDropSource = ItemDrops::ItemDropSource::CAPTURE;
static uint32_t itemDropStartMs = 0;

static float easeOutBackItem(float t) {
    t = Gfx::clamp01(t);
    float u = t - 1.0f;
    return 1.0f + 2.45f * u * u * u + 1.45f * u * u;
}

static int snapItemDropPx(int v) {
    if (ITEM_DROP_GRID_PX <= 1) return v;
    int half = ITEM_DROP_GRID_PX / 2;
    if (v >= 0) {
        return ((v + half) / ITEM_DROP_GRID_PX) * ITEM_DROP_GRID_PX;
    }
    return -(((-v + half) / ITEM_DROP_GRID_PX) * ITEM_DROP_GRID_PX);
}

static bool itemDropStillActive() {
    if (!itemDropActive) return false;
    uint32_t elapsed = millis() - itemDropStartMs;
    if (elapsed < ITEM_DROP_DURATION_MS) return true;
    itemDropActive = false;
    itemDropSpriteReady = false;
    itemDropNeedsClearRedraw = true;
    return false;
}

bool isItemDropActive() {
    return itemDropStillActive();
}

void showItemDrop(uint8_t itemId, bool firstTime, ItemDrops::ItemDropSource source) {
    const ItemDrops::ItemInfo* item = ItemDrops::getItem(itemId);
    uint8_t spriteId = item ? item->spriteId : itemId;
    if (!ItemSprites::get(spriteId)) return;
    itemDropItemId = itemId;
    itemDropSpriteId = spriteId;
    itemDropFirstTime = firstTime;
    itemDropSource = source;
    itemDropActive = true;
    itemDropSpriteReady = false;
    itemDropStartMs = millis();
    itemDropNeedsClearRedraw = false;
}

static bool ensureItemDropSprite() {
    const ItemSprites::SpritePng* png = ItemSprites::get(itemDropSpriteId);
    if (!ItemSprites::hasPixelArtContract(png)) return false;
    uint16_t fg = getColorFG();
    uint16_t bg = getColorBG();
    if (itemDropSpriteReady && itemDropDecodedId == itemDropSpriteId &&
        itemDropThemeFg == fg && itemDropThemeBg == bg) return true;

    if (!itemDropSprite) {
        itemDropSprite = new M5Canvas(&M5.Display);
        if (!itemDropSprite) return false;
        itemDropSprite->setPsram(true);
        itemDropSprite->setColorDepth(16);
    } else {
        itemDropSprite->deleteSprite();
    }

    if (!itemDropSprite->createSprite(png->width, png->height)) {
        itemDropSpriteReady = false;
        return false;
    }
    itemDropSprite->fillSprite(ITEM_DROP_TRANSPARENT_KEY);
    itemDropSpriteReady = itemDropSprite->drawPng(png->data, png->length, 0, 0);
    if (itemDropSpriteReady) themeMapSprite(*itemDropSprite, ITEM_DROP_TRANSPARENT_KEY);
    itemDropDecodedId = itemDropSpriteId;
    itemDropThemeFg = fg;
    itemDropThemeBg = bg;
    return itemDropSpriteReady;
}

static void computeItemDropPose(int& x, int& y, int& w, int& h, uint32_t& elapsed) {
    const ItemSprites::SpritePng* png = ItemSprites::get(itemDropSpriteId);
    w = png ? png->width : 56;
    h = png ? png->height : 56;
    x = snapItemDropPx((SCREEN_WIDTH - w) / 2);
    int targetY = snapItemDropPx((SCREEN_HEIGHT - h) / 2);
    int startY = snapItemDropPx(-h - 10);
    elapsed = millis() - itemDropStartMs;

    if (elapsed < ITEM_DROP_FLY_MS) {
        float t = (float)elapsed / (float)ITEM_DROP_FLY_MS;
        float e = easeOutBackItem(t);
        y = snapItemDropPx(startY + (int)lroundf((float)(targetY - startY) * e));
        return;
    }

    y = targetY;
    uint32_t bounceElapsed = elapsed - ITEM_DROP_FLY_MS;
    if (bounceElapsed < ITEM_DROP_BOUNCE_MS) {
        float t = (float)bounceElapsed / (float)ITEM_DROP_BOUNCE_MS;
        float amp = (1.0f - t) * 8.0f;
        y = snapItemDropPx(y + (int)lroundf(sinf(t * PI * 4.0f) * amp));
    }
}

template<typename T>
static void drawItemDropLabel(T& target, const char* text, int cx, int cy, bool inverted) {
    if (!text || !text[0]) return;
    uint16_t fg = getColorFG();
    uint16_t bg = getColorBG();
    int w = (int)strlen(text) * 6 + 10;
    if (w > SCREEN_WIDTH - 12) w = SCREEN_WIDTH - 12;
    int x = snapItemDropPx(cx - w / 2);
    if (x < 6) x = 6;
    if (x + w > SCREEN_WIDTH - 6) x = SCREEN_WIDTH - 6 - w;
    x = snapItemDropPx(x);
    cy = snapItemDropPx(cy);

    target.fillRect(x, cy - 6, w, 12, inverted ? fg : bg);
    target.drawRect(x, cy - 6, w, 12, fg);
    target.setTextSize(1);
    target.setTextDatum(MC_DATUM);
    target.setTextColor(inverted ? bg : fg);
    target.drawString(text, x + w / 2, cy);
    target.setTextDatum(TL_DATUM);
}

template<typename T>
static void drawItemDropChrome(T& target, int x, int y, int w, int h, uint32_t elapsed) {
    uint16_t fg = getColorFG();
    uint16_t bg = getColorBG();
    uint16_t dim = lerpColor565(fg, bg, 0.45f);
    int pulse = ((elapsed / 120) & 1U) ? ITEM_DROP_GRID_PX : 0;
    int pad = 6 + pulse;
    int left = snapItemDropPx(x - pad);
    int top = snapItemDropPx(y - pad);
    int right = snapItemDropPx(x + w + pad);
    int bottom = snapItemDropPx(y + h + pad);
    int tick = 14;
    int thick = ITEM_DROP_GRID_PX;

    // Grid brackets. no diagonal fizz; item pixels stay the hero.
    target.fillRect(left, top, tick, thick, fg);
    target.fillRect(left, top, thick, tick, fg);
    target.fillRect(right - tick, top, tick, thick, fg);
    target.fillRect(right - thick, top, thick, tick, fg);
    target.fillRect(left, bottom - thick, tick, thick, fg);
    target.fillRect(left, bottom - tick, thick, tick, fg);
    target.fillRect(right - tick, bottom - thick, tick, thick, fg);
    target.fillRect(right - thick, bottom - tick, thick, tick, fg);

    int cx = snapItemDropPx(x + w / 2);
    int cy = snapItemDropPx(y + h / 2);
    static const int8_t SPARKS[8][2] = {
        {-36, -28}, {-18, -42}, {22, -40}, {40, -18},
        {-42,  12}, {-24, 34}, {26,  32}, {44,  10}
    };
    for (int i = 0; i < 8; i++) {
        int sx = snapItemDropPx(cx + SPARKS[i][0] + ((i & 1) ? pulse : -pulse));
        int sy = snapItemDropPx(cy + SPARKS[i][1] + ((i & 2) ? pulse : -pulse));
        target.fillRect(sx, sy, ITEM_DROP_GRID_PX, ITEM_DROP_GRID_PX, (i & 1) ? fg : dim);
    }
}

template<typename T>
static void drawItemDropLabels(T& target, int x, int y, int w, int h, bool skipBottom = false) {
    int cx = x + w / 2;
    const ItemDrops::ItemInfo* item = ItemDrops::getItem(itemDropItemId);
    const char* rarity = item ? ItemDrops::getRarityLabel(item->rarity) : "DROP";
    const char* source = ItemDrops::getSourceLabel(itemDropSource);
    const char* name = item ? item->name : "ITEM";
    const char* tag = (item && item->tag) ? item->tag : "";

    char top[32];
    snprintf(top, sizeof(top), "%s // %s // %s", itemDropFirstTime ? "N3W" : "DUP3", rarity, source);
    int topY = y - 15;
    if (topY < TOP_BAR_H + 8) topY = TOP_BAR_H + 8;
    drawItemDropLabel(target, top, cx, topY, true);

    if (skipBottom) return;

    char bottom[48];
    if (tag[0]) snprintf(bottom, sizeof(bottom), "%s // %s", name, tag);
    else snprintf(bottom, sizeof(bottom), "%s", name);
    int bottomY = y + h + 14;
    int maxBottomY = SCREEN_HEIGHT - BOTTOM_BAR_H - 8;
    if (bottomY > maxBottomY) bottomY = maxBottomY;
    drawItemDropLabel(target, bottom, cx, bottomY, false);
}

static void buildItemDropDialogueLines(const ItemDrops::ItemInfo* item,
                                       char* line1, size_t line1Size,
                                       char* line2, size_t line2Size) {
    const char* name = item ? item->name : "ITEM";
    const char* tag = (item && item->tag) ? item->tag : "";

    snprintf(line1, line1Size, "%s",
             itemDropFirstTime ? "new evidence bagged." : "duplicate evidence logged.");
    if (tag[0]) snprintf(line2, line2Size, "%s // %s", name, tag);
    else snprintf(line2, line2Size, "%s", name);
}

template<typename T>
static void drawItemDropDialoguePanel(T& target, uint16_t fg, uint16_t bg,
                                      uint16_t dim, uint16_t fill) {
    target.fillRect(ITEM_DROP_DIALOGUE_PANEL_X, ITEM_DROP_DIALOGUE_PANEL_Y,
                    ITEM_DROP_DIALOGUE_PANEL_W, ITEM_DROP_DIALOGUE_PANEL_H, fill);
    target.drawRect(ITEM_DROP_DIALOGUE_PANEL_X, ITEM_DROP_DIALOGUE_PANEL_Y,
                    ITEM_DROP_DIALOGUE_PANEL_W, ITEM_DROP_DIALOGUE_PANEL_H, fg);
    target.drawRect(ITEM_DROP_DIALOGUE_BADGE_X - 2,
                    ITEM_DROP_DIALOGUE_BADGE_Y - 2,
                    ITEM_DROP_DIALOGUE_BADGE_SIZE + 4,
                    ITEM_DROP_DIALOGUE_BADGE_SIZE + 4, dim);
    target.fillRect(ITEM_DROP_DIALOGUE_BADGE_X - 1,
                    ITEM_DROP_DIALOGUE_BADGE_Y - 1,
                    ITEM_DROP_DIALOGUE_BADGE_SIZE + 2,
                    ITEM_DROP_DIALOGUE_BADGE_SIZE + 2, bg);
}

template<typename T>
static void drawItemDropEvidenceBadge(T& target, uint16_t fg, uint16_t dim) {
    const int cx = ITEM_DROP_DIALOGUE_BADGE_X + ITEM_DROP_DIALOGUE_BADGE_SIZE / 2;

    // Evidence stamp, not a tiny portrait: this must read cleanly at one glance.
    target.fillRect(ITEM_DROP_DIALOGUE_BADGE_X + 5,
                    ITEM_DROP_DIALOGUE_BADGE_Y + 5, 26, 2, dim);
    target.fillRect(ITEM_DROP_DIALOGUE_BADGE_X + 5,
                    ITEM_DROP_DIALOGUE_BADGE_Y + 29, 26, 2, dim);
    target.setTextSize(1);
    target.setTextDatum(MC_DATUM);
    target.setTextColor(dim);
    target.drawString("EVID", cx, ITEM_DROP_DIALOGUE_BADGE_Y + 12);
    target.setTextColor(fg);
    target.drawString(itemDropFirstTime ? "NEW" : "DUP", cx,
                      ITEM_DROP_DIALOGUE_BADGE_Y + 23);
    target.setTextDatum(TL_DATUM);
}

template<typename T>
static void drawItemDropDialogueCopy(T& target, const char* line1, const char* line2,
                                     uint16_t fg, uint16_t dim, uint16_t text) {
    const int textX = ITEM_DROP_DIALOGUE_BADGE_X +
                      ITEM_DROP_DIALOGUE_BADGE_SIZE + 8;
    const int textY = ITEM_DROP_DIALOGUE_PANEL_Y + 8;

    target.setTextDatum(TL_DATUM);
    target.setTextSize(1);
    target.setTextColor(text);
    target.drawString(line1, textX, textY);
    target.setTextColor(dim);
    target.drawString(line2, textX, textY + 15);
    target.setTextColor(fg);
}

template<typename T>
static bool drawItemDropDialogue(T& target) {
    const ItemDrops::ItemInfo* item = ItemDrops::getItem(itemDropItemId);

    uint16_t fg = getColorFG();
    uint16_t bg = getColorBG();
    uint16_t dim = lerpColor565(fg, bg, 0.45f);
    uint16_t fill = isInvertedTheme() ? fg : bg;
    uint16_t text = isInvertedTheme() ? bg : fg;

    drawItemDropDialoguePanel(target, fg, bg, dim, fill);
    drawItemDropEvidenceBadge(target, fg, dim);

    char line1[36];
    char line2[44];
    buildItemDropDialogueLines(item, line1, sizeof(line1), line2, sizeof(line2));

    drawItemDropDialogueCopy(target, line1, line2, fg, dim, text);
    return true;
}

template<typename T>
static void drawItemDropFallbackTo(T& target, int x, int y, int w, int h) {
    uint16_t fg = getColorFG();
    uint16_t bg = getColorBG();
    int cx = snapItemDropPx(x + w / 2);
    int cy = snapItemDropPx(y + h / 2);

    target.fillRect(x, y, w, h, bg);
    target.drawRect(x, y, w, h, fg);
    target.fillRect(cx - 8, cy - 18, 16, ITEM_DROP_GRID_PX, fg);
    target.fillRect(cx + 6, cy - 16, ITEM_DROP_GRID_PX, 10, fg);
    target.fillRect(cx - 2, cy - 6, 8, ITEM_DROP_GRID_PX, fg);
    target.fillRect(cx - 2, cy + 2, ITEM_DROP_GRID_PX * 2, ITEM_DROP_GRID_PX * 2, fg);
}

static void drawItemDropFallback(M5Canvas* target, int x, int y, int w, int h) {
    if (!target) return;
    drawItemDropFallbackTo(*target, x, y, w, h);
}

static bool drawItemDropToCanvas(M5Canvas* target) {
    if (!target || !itemDropStillActive()) return false;
    int x, y, w, h;
    uint32_t elapsed;
    computeItemDropPose(x, y, w, h, elapsed);
    drawItemDropChrome(*target, x, y, w, h, elapsed);
    if (ensureItemDropSprite()) {
        itemDropSprite->pushSprite(target, x, y, ITEM_DROP_TRANSPARENT_KEY);
    } else {
        drawItemDropFallback(target, x, y, w, h);
    }
    bool drewDialogue = drawItemDropDialogue(*target);
    drawItemDropLabels(*target, x, y, w, h, drewDialogue);
    itemDropDrawnOnCanvasFrame = true;
    return true;
}

static bool drawItemDropToDisplay() {
    if (!itemDropStillActive()) return false;
    int x, y, w, h;
    uint32_t elapsed;
    computeItemDropPose(x, y, w, h, elapsed);
    drawItemDropChrome(M5.Display, x, y, w, h, elapsed);
    if (ensureItemDropSprite()) {
        itemDropSprite->pushSprite(x, y, ITEM_DROP_TRANSPARENT_KEY);
    } else {
        drawItemDropFallbackTo(M5.Display, x, y, w, h);
    }
    bool drewDialogue = drawItemDropDialogue(M5.Display);
    drawItemDropLabels(M5.Display, x, y, w, h, drewDialogue);
    // Chrome sparks, floating labels, and the bottom dialogue panel scatter far
    // outside the sprite rect, and the sprite itself flies in from above the
    // surface. Claim the frame rather than chase five moving sub-rects.
    noteDirectPanelDraw(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
    return true;
}

static inline bool isQuickToastActive() {
    if (quickToastMsg[0] == '\0') return false;
    if ((uint32_t)(millis() - quickToastStart) >= quickToastLifeMs) {
        // Last gap closed for good.
        if (quickToastWasDrawnToDisplay) {
            quickToastNeedsClearRedraw = true;
            quickToastWasDrawnToDisplay = false;
        }
        quickToastMsg[0] = '\0';
        quickToastLifeMs = 0;
        quickToastPrevVisible = false;
        return false;
    }
    return true;
}

// Walk the flicker schedule for this instant. Urgent strobes, then the
// decelerating settle, then the solid hold, then the accelerating exit.
static bool toastVisibleNow() {
    if (quickToastMsg[0] == '\0') return false;
    uint32_t t = (uint32_t)(millis() - quickToastStart);

    for (uint8_t i = 0; i < quickToastUrgent; i++) {
        if (t < kToastUrgentOn) return true;
        t -= kToastUrgentOn;
        if (t < kToastUrgentOff) return false;
        t -= kToastUrgentOff;
    }
    for (int i = 0; i < kToastFlickInCount; i++) {
        if (t < kToastFlickIn[i].onMs) return true;
        t -= kToastFlickIn[i].onMs;
        if (t < kToastFlickIn[i].offMs) return false;
        t -= kToastFlickIn[i].offMs;
    }
    if (t < quickToastHoldMs) return true;
    t -= quickToastHoldMs;
    for (int i = 0; i < kToastFlickOutCount; i++) {
        if (t < kToastFlickOut[i].onMs) return true;
        t -= kToastFlickOut[i].onMs;
        if (t < kToastFlickOut[i].offMs) return false;
        t -= kToastFlickOut[i].offMs;
    }
    return false;
}

// ==[ TOAST LAYOUT ]== mini DEFHOG4 console: 1px border, panel header strip,
// wrapped body lines, block cursor. Palette is the terminal's, not the theme's.
namespace ToastTN = UIMeasurements::TokyoNight;

static constexpr int kToastCharW    = UIMeasurements::kCharWSize1;   // 6
static constexpr int kToastCharH    = UIMeasurements::kCharHSize1;   // 8
static constexpr int kToastPadX     = 4;
static constexpr int kToastHdrH     = kToastCharH + 4;   // 12 — 2px above and below the glyphs
static constexpr int kToastGapT     = 3;                 // header strip to first body line
static constexpr int kToastPadB     = 3;                 // last body line to bottom border
static constexpr int kToastLineH    = 9;                 // terminal line pitch
static constexpr int kToastRailW    = 2;                 // alert rail, terminal-style
static constexpr int kToastRailGap  = 4;
static constexpr int kToastMaxLines = 5;
static constexpr int kToastWrapChars = 30;  // fits 208px with rail + cursor reserved
static constexpr int kToastMinW = 140;
static constexpr int kToastMaxW = UIMeasurements::DefhogLayout::kTermW;  // 208 — console width
// SCREEN_WIDTH - kTermX - kTermW == 6: the console's own right margin
static constexpr int kToastMarginX = 6;

struct ToastLayout {
    char buf[128];              // wrapping inserts terminators, so oversize the copy
    char* lines[kToastMaxLines];
    bool  lineRail[kToastMaxLines];  // accent rail — alert body lines
    bool  lineDim[kToastMaxLines];   // dim commentary — the noir header phrase
    int lineCount;
    int maxLineW;
    int toastW, toastH;
    int toastX, toastY;
    char prompt[12];   // "SRL1# "
    char kind[8];      // "alert" / "status"
    char clock[8];     // "HH:MM" from the RTC, else session MM:SS
};

static void computeToastLayout(ToastLayout& L) {
    L.lineCount = 0;
    L.maxLineW = 0;

    // alert header phrase becomes the console's dim commentary line
    if (quickToastIsAlert && lastAlertHeader < ALERT_HEADER_COUNT) {
        L.lines[L.lineCount]    = (char*)ALERT_HEADERS[lastAlertHeader];
        L.lineRail[L.lineCount] = false;
        L.lineDim[L.lineCount]  = true;
        L.lineCount++;
    }

    strncpy(L.buf, quickToastMsg, sizeof(L.buf) - 1);
    L.buf[sizeof(L.buf) - 1] = '\0';

    // split on \n, wrap each segment to the console's char budget. Never cache a
    // pointer past `p` — a hard break memmoves everything behind it.
    size_t used = strlen(L.buf);
    char* p = L.buf;
    while (*p && L.lineCount < kToastMaxLines) {
        int len = 0;
        while (p[len] && p[len] != '\n') len++;
        bool hitNewline = (p[len] == '\n');
        int brk = len;

        if (len > kToastWrapChars) {
            brk = kToastWrapChars;
            while (brk > 0 && p[brk] != ' ') brk--;
            if (brk == 0) {
                // one word wider than the console — hard break, make room for a
                // terminator rather than eating a character
                brk = kToastWrapChars;
                if (used + 1 < sizeof(L.buf)) {
                    size_t tail = used - (size_t)(p - L.buf) - (size_t)brk + 1;
                    memmove(p + brk + 1, p + brk, tail);
                    used++;
                }
            }
            hitNewline = false;
        }

        bool consumesSeparator = hitNewline || (brk < len);
        p[brk] = '\0';
        L.lines[L.lineCount]    = p;
        L.lineRail[L.lineCount] = quickToastIsAlert;
        L.lineDim[L.lineCount]  = false;
        L.lineCount++;
        p += brk + (consumesSeparator ? 1 : 0);
    }

    for (int i = 0; i < L.lineCount; i++) {
        int w = (int)strlen(L.lines[i]) * kToastCharW;
        if (L.lineRail[i]) w += kToastRailGap;  // rail pushes the text right
        if (i == L.lineCount - 1) w += kToastCharW + 3;  // room for the prompt cursor
        if (w > L.maxLineW) L.maxLineW = w;
    }

    // header strip: "SRL1# alert" left, timestamp right
    snprintf(L.prompt, sizeof(L.prompt), "%s# ", Config::getHamletName());
    strncpy(L.kind, quickToastIsAlert ? "alert" : "status", sizeof(L.kind) - 1);
    L.kind[sizeof(L.kind) - 1] = '\0';
    strncpy(L.clock, quickToastClock, sizeof(L.clock) - 1);
    L.clock[sizeof(L.clock) - 1] = '\0';
    int headerW = (int)(strlen(L.prompt) + strlen(L.kind) + strlen(L.clock)) * kToastCharW
                  + kToastCharW;  // one blank cell between command and clock

    int innerW = (L.maxLineW > headerW) ? L.maxLineW : headerW;
    L.toastW = innerW + 2 * kToastPadX + 2;
    if (L.toastW < kToastMinW) L.toastW = kToastMinW;
    if (L.toastW > kToastMaxW) L.toastW = kToastMaxW;
    L.toastH = 1 + kToastHdrH + kToastGapT
               + (L.lineCount * kToastLineH - 1)  // no trailing gap under the last line
               + kToastPadB + 1;

    // Position from latched corner. Margins match the console's, so a
    // full-width toast in the upper-right lands on its exact footprint.
    const int mx = kToastMarginX;
    const int loY = SCREEN_HEIGHT - BOTTOM_BAR_H - L.toastH - mx;
    switch (quickToastCorner) {
        case 0: L.toastX = mx; L.toastY = kToastY; break;                                 // upper-left
        case 1: L.toastX = SCREEN_WIDTH - L.toastW - mx; L.toastY = kToastY; break;       // upper-right
        case 2: L.toastX = mx; L.toastY = loY; break;                                     // lower-left
        case 3: L.toastX = SCREEN_WIDTH - L.toastW - mx; L.toastY = loY; break;           // lower-right
        default: L.toastX = SCREEN_WIDTH - L.toastW - mx; L.toastY = kToastY; break;
    }

    // border is inside the rect now — the frame *is* the footprint
    quickToastX = L.toastX;
    quickToastY = L.toastY;
    quickToastW = L.toastW;
    quickToastH = L.toastH;
}

// shared render — same frame, header, rails and cursor as the room console
template<typename T>
static void renderQuickToastTo(T& tgt, const ToastLayout& L) {
    const int x = L.toastX, y = L.toastY, w = L.toastW, h = L.toastH;

    tgt.fillRect(x, y, w, h, ToastTN::BG);
    tgt.drawRect(x, y, w, h, ToastTN::BORDER);
    tgt.fillRect(x + 1, y + 1, w - 2, kToastHdrH, ToastTN::BG_PANEL);

    tgt.setTextSize(1);
    tgt.setTextDatum(TL_DATUM);

    // header: prompt muted, command lit, timestamp right-aligned and muted
    const int hx = x + 1 + kToastPadX;
    const int hy = y + 3;
    tgt.setTextColor(ToastTN::DIM);
    tgt.drawString(L.prompt, hx, hy);
    tgt.setTextColor(quickToastIsAlert ? ToastTN::ACCENT : ToastTN::PRIMARY);
    tgt.drawString(L.kind, hx + (int)strlen(L.prompt) * kToastCharW, hy);
    tgt.setTextColor(ToastTN::DIM);
    tgt.drawString(L.clock,
                   x + w - 1 - kToastPadX - (int)strlen(L.clock) * kToastCharW, hy);

    // body — alert lines carry the terminal's accent rail
    const bool cursorOn = ((millis() / 500) & 1) == 0;
    int ty = y + 1 + kToastHdrH + kToastGapT;
    for (int i = 0; i < L.lineCount; i++) {
        int tx = x + 1 + kToastPadX;
        if (L.lineRail[i]) {
            tgt.fillRect(tx, ty, kToastRailW, kToastCharH, ToastTN::ACCENT);
            tx += kToastRailGap;
            tgt.setTextColor(ToastTN::ACCENT);
        } else {
            tgt.setTextColor(L.lineDim[i] ? ToastTN::DIM : ToastTN::FG);
        }
        tgt.drawString(L.lines[i], tx, ty);

        if (i == L.lineCount - 1 && cursorOn) {
            int cx = tx + (int)strlen(L.lines[i]) * kToastCharW + 3;
            if (cx + kToastCharW <= x + w - 1 - kToastPadX) {
                tgt.fillRect(cx, ty, kToastCharW, kToastCharH, ToastTN::PRIMARY);
            }
        }
        ty += kToastLineH;
    }
    tgt.setTextDatum(TL_DATUM);
}

static void drawQuickToastToCanvas(M5Canvas* target) {
    if (!target) return;
    if (!isQuickToastActive()) return;

    quickToastPrevVisible = toastVisibleNow();
    if (!quickToastPrevVisible) return;  // gap — the scene shows through

    ToastLayout L;
    computeToastLayout(L);
    renderQuickToastTo(*target, L);
}

static void drawQuickToastToDisplay() {
    if (!isQuickToastActive()) return;

    quickToastPrevVisible = toastVisibleNow();
    if (!quickToastPrevVisible) {
        // A gap only reads as a gap if the panel gives the pixels back.
        if (quickToastWasDrawnToDisplay) {
            noteDirectPanelDraw(quickToastX, quickToastY, quickToastW, quickToastH);
            quickToastNeedsClearRedraw = true;
            quickToastWasDrawnToDisplay = false;
        }
        return;
    }
    quickToastWasDrawnToDisplay = true;

    ToastLayout L;
    computeToastLayout(L);
    noteDirectPanelDraw(quickToastX, quickToastY, quickToastW, quickToastH);
    renderQuickToastTo(M5.Display, L);
}

// ==[ HELP WIKI OVERLAY RENDERING ]==

void showHelpOverlay(HamletMode mode) {
    helpOverlayActive = true;
    helpOverlayMode = mode;
    helpOverlayPage = 0;
    // mark help as seen for this mode — stops [?] blinking
    uint8_t idx = static_cast<uint8_t>(mode);
    if (idx < HAMLET_MODE_COUNT) {
        uint32_t mask = Config::getHelpWikiSeen();
        mask |= (1u << idx);
        Config::setHelpWikiSeen(mask);
    }
}

void dismissHelpOverlay() {
    helpOverlayActive = false;
    helpOverlayPage = 0;
}

bool advanceHelpPage() {
    if (!helpOverlayActive) return false;
    uint8_t total = HelpWiki::getPageCount(helpOverlayMode);
    if (helpOverlayPage + 1 < total) {
        helpOverlayPage++;
        return true;  // advanced to next page
    }
    // past last page — dismiss
    helpOverlayActive = false;
    helpOverlayPage = 0;
    return false;
}

bool isHelpOverlayActive() {
    return helpOverlayActive;
}

// shared draw logic — works on any M5GFX target (canvas or display)
template<typename T>
static void drawHelpOverlayTo(T& target) {
    if (!helpOverlayActive) return;

    const char* title = HelpWiki::getHelpTitle(helpOverlayMode);
    uint8_t totalPages = HelpWiki::getPageCount(helpOverlayMode);
    if (helpOverlayPage >= totalPages) helpOverlayPage = 0;
    const char* text = HelpWiki::getHelpPage(helpOverlayMode, helpOverlayPage);

    const int lineH = 10;
    const int padX = 8;
    const int padY = 6;
    const int border = 4;
    const int maxChars = 49;

    // count lines in text
    int lineCount = 1;  // title
    lineCount++;        // blank separator
    const char* p = text;
    int bodyLines = 1;
    while (*p) { if (*p == '\n') bodyLines++; p++; }
    lineCount += bodyLines;
    lineCount++;  // hint line at bottom

    // panel dimensions
    int panelW = SCREEN_WIDTH - 8;  // 312px
    int panelH = padY * 2 + lineCount * lineH;
    int panelX = 4;
    int panelY = kToastY;

    // clamp to playfield
    int maxH = SCREEN_HEIGHT - BOTTOM_BAR_H - panelY - 2 - border;
    if (panelH > maxH) panelH = maxH;

    // colors — straight from theme
    uint16_t fillColor   = getColorBG();
    uint16_t borderColor = getColorFG();

    // border + fill
    target.fillRect(panelX - border, panelY - border,
                    panelW + border * 2, panelH + border * 2, borderColor);
    target.fillRect(panelX, panelY, panelW, panelH, fillColor);

    // text
    target.setTextColor(borderColor);
    target.setTextSize(1);
    target.setTextDatum(TL_DATUM);

    int textX = panelX + padX;
    int textY = panelY + padY;
    int maxY  = panelY + panelH - lineH - 4;  // reserve space for hint

    // title line
    target.drawString(title, textX, textY);

    // page counter in top-right of panel — "1/3" format
    if (totalPages > 1) {
        char pgBuf[8];
        // helpOverlayPage is 0-indexed internally; display 1-indexed
        snprintf(pgBuf, sizeof(pgBuf), "%u/%u", helpOverlayPage + 1, totalPages);
        target.setTextDatum(TR_DATUM);
        target.drawString(pgBuf, panelX + panelW - padX, textY);
        target.setTextDatum(TL_DATUM);
    }
    textY += lineH;

    // blank separator
    textY += lineH;

    // body lines — split on \n
    char lineBuf[52];
    p = text;
    while (*p && textY < maxY) {
        const char* nl = p;
        while (*nl && *nl != '\n') nl++;
        int len = nl - p;
        if (len > maxChars) len = maxChars;
        memcpy(lineBuf, p, len);
        lineBuf[len] = '\0';
        target.drawString(lineBuf, textX, textY);
        textY += lineH;
        p = (*nl) ? nl + 1 : nl;
    }

    // hint at bottom — context-aware
    bool lastPage = (helpOverlayPage + 1 >= totalPages);
    const char* hint = lastPage
        ? "[ tap to close / swipe L ]"
        : "[ tap for more / swipe L close ]";
    target.drawString(hint, textX, panelY + panelH - lineH - 2);

    target.setTextDatum(TL_DATUM);
}

static void drawHelpOverlayToCanvas(M5Canvas* c) {
    if (!c || !helpOverlayActive) return;
    drawHelpOverlayTo(*c);
}

static void drawHelpOverlayToDisplay() {
    if (!helpOverlayActive) return;
    drawHelpOverlayTo(M5.Display);
    // The panel is page-sized, so claim its clamp envelope instead of
    // recomputing the line count here: full width from the toast line down to
    // the bottom bar is exactly what drawHelpOverlayTo can never exceed.
    noteDirectPanelDraw(0, kToastY - 4, SCREEN_WIDTH,
                        SCREEN_HEIGHT - BOTTOM_BAR_H - (kToastY - 4));
}

void setHoldProgress(float progress) {
    bool wasActive = (holdProgress > 0.0f);
    holdProgress = progress;
    bool isActive = (holdProgress > 0.0f);

    // If ring was drawn directly to display (submenu), we need one redraw to clear it.
    if (wasActive && !isActive && holdRingWasDrawnToDisplay) {
        holdRingNeedsClearRedraw = true;
        holdRingWasDrawnToDisplay = false;
    }
}

// ==[ XP NOTIFICATION STRIP ]== state machine + render
// Ticked from canvas draw path (drawXPNotifToCanvas). Display path just reads state.

static void updateXPNotifState() {
    // drain pending XP accumulated in Config since last frame
    xpNotifPending += Config::consumePendingXPDisplay();
    uint32_t now = millis();

    switch (xpNotifPhase) {
    case XPNotifPhase::IDLE:
        if (xpNotifPending > 0 && TimeMath::reachedOrUnset(now, xpNotifCooldown)) {
            bool fire = (xpNotifPending > 3) || ((esp_random() & 1) == 0);
            if (fire) {
                xpNotifAmount    = xpNotifPending;
                xpNotifPending   = 0;
                xpNotifPhase     = XPNotifPhase::BLINK_IN;
                xpNotifBlinks    = 6;   // 3 full blinks × 80ms = 480ms
                xpNotifVisible   = true;
                xpNotifBlinkNext = now + 80;
            }
        }
        break;
    case XPNotifPhase::BLINK_IN:
        if (TimeMath::reached(now, xpNotifBlinkNext)) {
            xpNotifVisible = !xpNotifVisible;
            if (xpNotifBlinks > 0) xpNotifBlinks--;
            xpNotifBlinkNext = now + 80;
            if (xpNotifBlinks == 0) {
                xpNotifVisible  = true;
                xpNotifPhase    = XPNotifPhase::STAY;
                xpNotifPhaseEnd = now + 1800;
            }
        }
        break;
    case XPNotifPhase::STAY:
        if (TimeMath::reached(now, xpNotifPhaseEnd)) {
            xpNotifPhase   = XPNotifPhase::BLINK_OUT;
            xpNotifBlinks  = 6;
            xpNotifVisible = true;
            xpNotifBlinkNext = now + 80;
        }
        break;
    case XPNotifPhase::BLINK_OUT:
        if (TimeMath::reached(now, xpNotifBlinkNext)) {
            xpNotifVisible = !xpNotifVisible;
            if (xpNotifBlinks > 0) xpNotifBlinks--;
            xpNotifBlinkNext = now + 80;
            if (xpNotifBlinks == 0) {
                xpNotifVisible  = false;
                xpNotifPhase    = XPNotifPhase::IDLE;
                xpNotifCooldown = now + 10000;  // 10s gap between notifications
            }
        }
        break;
    }
}

// shared render — full parity with drawIdleInfoBar page 0, right side replaced with +XP
template<typename T>
static void renderXPNotifStripTo(T& tgt) {
    uint8_t level    = Config::getLevel();
    const char* rank = Config::getRankName();
    uint8_t xpProg   = Config::getXPProgress();

    char leftBuf[20], rightBuf[14];
    snprintf(leftBuf,  sizeof(leftBuf),  "L%d %s", level, rank);
    snprintf(rightBuf, sizeof(rightBuf), "+%lu XP", (unsigned long)xpNotifAmount);

    const int barY = TOP_BAR_H;
    const int barH = 11;
    uint16_t fg = getColorFG();
    uint16_t bg = getColorBG();

    tgt.fillRect(0, barY, SCREEN_WIDTH, barH, fg);
    tgt.setTextColor(bg);
    tgt.setTextSize(1);
    tgt.setCursor(3, barY + 2);
    tgt.print(leftBuf);

    int leftW     = (int)strlen(leftBuf)  * 6 + 3;  // same as drawIdleInfoBar
    int rightW    = (int)strlen(rightBuf) * 6;
    int barStartX = leftW + 6;
    int barEndX   = SCREEN_WIDTH - rightW - 8;
    int xpBarW    = barEndX - barStartX;
    if (xpBarW > 20) {
        int xpBarY = barY + 3;
        int xpBarH = 5;
        tgt.drawRect(barStartX, xpBarY, xpBarW, xpBarH, bg);
        int fillW = ((xpBarW - 2) * xpProg) / 100;
        if (fillW > 0) tgt.fillRect(barStartX + 1, xpBarY + 1, fillW, xpBarH - 2, bg);
    }
    tgt.setCursor(SCREEN_WIDTH - rightW - 3, barY + 2);
    tgt.print(rightBuf);
}

// canvas path: tick state + draw if visible. returns true if strip was drawn.
static bool drawXPNotifToCanvas(M5Canvas* target) {
    if (!target) return false;
    updateXPNotifState();
    if (xpNotifPhase == XPNotifPhase::IDLE || !xpNotifVisible) return false;
    renderXPNotifStripTo(*target);
    return true;
}

// display path: draw directly to M5.Display if visible (no tick — canvas already ticked)
static void drawXPNotifToDisplay() {
    if (xpNotifPhase == XPNotifPhase::IDLE || !xpNotifVisible) return;
    renderXPNotifStripTo(M5.Display);
    // Strip geometry is fixed in renderXPNotifStripTo: full width, 11px tall,
    // seated directly under the status bar.
    noteDirectPanelDraw(0, TOP_BAR_H, SCREEN_WIDTH, 11);
}

// draw hold ring on canvas, then push. replaces raw pushSprite calls.
static void pushCanvas() {
    holdRingDrawnOnCanvasFrame = false;
    quickToastDrawnOnCanvasFrame = false;
    helpOverlayDrawnOnCanvasFrame = false;
    itemDropDrawnOnCanvasFrame = false;
    xpNotifOnCanvas = false;

    // XP strip + item drop sit below help/toast so urgent copy stays readable.
    if (drawXPNotifToCanvas(canvas)) {
        xpNotifOnCanvas = true;
    }
    drawItemDropToCanvas(canvas);

    // Help covers the playfield; toast draws after it so urgent alerts stay visible.
    if (helpOverlayActive) {
        drawHelpOverlayToCanvas(canvas);
        helpOverlayDrawnOnCanvasFrame = true;
    }
    if (isQuickToastActive()) {
        drawQuickToastToCanvas(canvas);
        quickToastDrawnOnCanvasFrame = true;
        quickToastWasDrawnToDisplay = false;
    }
    if (holdProgress > 0.0f) {
        drawHoldRingToCanvas(canvas);
        holdRingDrawnOnCanvasFrame = true;
        holdRingWasDrawnToDisplay = false;
    }
    FramePresenter::present(*canvas);
    AmbientLED::update(canvas);
}


static void sampleBootIntroParticles() {
    MenuPig::sampleTeleportParticles(bootIntro.particles,
                                     bootIntro.particleCount,
                                     (uint8_t)(sizeof(bootIntro.particles) / sizeof(bootIntro.particles[0])));
}

static void updateBootIntroProgress(uint32_t now) {
    if (!bootIntro.active || bootIntro.progressDuration == 0) return;
    float t = (float)(now - bootIntro.progressStart) / (float)bootIntro.progressDuration;
    if (t >= 1.0f) {
        bootIntro.displayedStages = bootIntro.progressTo;
        bootIntro.progressDuration = 0;
        if (bootIntro.phase == BootIntroPhase::CATCH_UP) {
            bootIntro.phase = BootIntroPhase::LOOP;
            bootIntro.phaseStart = now;
        }
        return;
    }
    float e = Gfx::smoothstep01(t);
    bootIntro.displayedStages = bootIntro.progressFrom + (bootIntro.progressTo - bootIntro.progressFrom) * e;
}

static void startBootProgressAnimation(uint8_t completedStages, uint32_t minDurationMs) {
    if (!bootIntro.active) return;
    if (completedStages > bootIntro.totalStages) completedStages = bootIntro.totalStages;

    bootIntro.targetStages = completedStages;
    bootIntro.progressFrom = bootIntro.displayedStages;
    bootIntro.progressTo = (float)completedStages;
    bootIntro.progressStart = millis();
    if (bootIntro.progressTo <= bootIntro.progressFrom) {
        bootIntro.progressDuration = 0;
        bootIntro.displayedStages = bootIntro.progressTo;
        return;
    }
    bootIntro.progressDuration = minDurationMs;
    bootIntro.phase = BootIntroPhase::CATCH_UP;
    bootIntro.phaseStart = bootIntro.progressStart;
}

static int snapBootGrid(int v) {
    return v & ~3;
}

// Authored 5x7 boot alphabet. Every lit bit is one chunky scene-grid cell;
// the intro title never falls back to the library font.
static uint8_t bootGlyphRow(char c, uint8_t row) {
    static constexpr uint8_t A[7] = {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11};
    static constexpr uint8_t C[7] = {0x0F, 0x10, 0x10, 0x10, 0x10, 0x10, 0x0F};
    static constexpr uint8_t E[7] = {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F};
    static constexpr uint8_t G[7] = {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0E};
    static constexpr uint8_t H[7] = {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11};
    static constexpr uint8_t I[7] = {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1F};
    static constexpr uint8_t L[7] = {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F};
    static constexpr uint8_t M[7] = {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11};
    static constexpr uint8_t N[7] = {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11};
    static constexpr uint8_t P[7] = {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10};
    static constexpr uint8_t T[7] = {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04};
    static constexpr uint8_t DOT[7] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x04};
    if (row >= 7) return 0;
    switch (c) {
        case 'A': return A[row];
        case 'C': return C[row];
        case 'E': return E[row];
        case 'G': return G[row];
        case 'H': return H[row];
        case 'I': return I[row];
        case 'L': return L[row];
        case 'M': return M[row];
        case 'N': return N[row];
        case 'P': return P[row];
        case 'T': return T[row];
        case '.': return DOT[row];
        default: return 0;
    }
}

static int bootBlockTextWidth(const char* text, int cell) {
    if (!text || !text[0]) return 0;
    return ((int)strlen(text) * 6 - 1) * cell;
}

static void drawBootBlockText(const char* text, int centerX, int centerY,
                              int cell, uint16_t color) {
    if (!canvas || !text || cell <= 0) return;
    int x0 = snapBootGrid(centerX - bootBlockTextWidth(text, cell) / 2);
    int y0 = snapBootGrid(centerY - (7 * cell) / 2);
    for (size_t i = 0; text[i] != '\0'; ++i) {
        int gx = x0 + (int)i * 6 * cell;
        for (uint8_t row = 0; row < 7; ++row) {
            uint8_t bits = bootGlyphRow(text[i], row);
            for (uint8_t col = 0; col < 5; ++col) {
                if ((bits & (1u << (4u - col))) == 0) continue;
                canvas->fillRect(gx + col * cell, y0 + row * cell,
                                 cell, cell, color);
            }
        }
    }
}

static float easeOutBackBoot(float t) {
    t = Gfx::clamp01(t);
    float u = t - 1.0f;
    return 1.0f + 2.70158f * u * u * u + 1.70158f * u * u;
}

static void drawBootIntroTitleLine(const char* text, int finalCenterX, int y,
                                   int cell, bool fromLeft, uint32_t introElapsed,
                                   uint32_t delayMs, uint32_t slideMs,
                                   uint16_t core, uint16_t accentA, uint16_t accentB,
                                   float slideOutT = 0.0f) {
    if (!text || slideMs == 0) return;

    // slide-out: accelerate back the way it came
    if (slideOutT >= 1.0f) return;  // fully off-screen
    int travel = 220 + bootBlockTextWidth(text, cell);
    int drawCenterX;

    if (slideOutT > 0.0f) {
        float exitEase = slideOutT * slideOutT;  // ease-in quad
        drawCenterX = finalCenterX + (fromLeft ? -1 : 1) * (int)lroundf((float)travel * exitEase);
    } else {
        if (introElapsed + 150 < delayMs) return;
        float t = (introElapsed <= delayMs)
            ? 0.0f
            : Gfx::clamp01((float)(introElapsed - delayMs) / (float)slideMs);
        float e = easeOutBackBoot(t);
        int startCenterX = finalCenterX + (fromLeft ? -travel : travel);
        drawCenterX = (int)lroundf((float)startCenterX + (float)(finalCenterX - startCenterX) * e);
    }
    drawCenterX = snapBootGrid(drawCenterX);

    drawBootBlockText(text, snapBootGrid(drawCenterX + 8), snapBootGrid(y + 8), cell, accentB);
    drawBootBlockText(text, snapBootGrid(drawCenterX - 4), snapBootGrid(y + 4), cell, accentB);
    drawBootBlockText(text, snapBootGrid(drawCenterX + 4), snapBootGrid(y + 4), cell, accentA);
    drawBootBlockText(text, snapBootGrid(drawCenterX + 4), snapBootGrid(y - 4), cell, accentA);
    drawBootBlockText(text, drawCenterX, snapBootGrid(y), cell, core);
}

static void drawBootIntroTitle(uint32_t now) {
    if (bootIntro.phase == BootIntroPhase::TP_VOID) return;

    uint32_t introElapsed = now - bootIntro.introStart;
    uint16_t titleCore = getColorFG();
    uint16_t titleAccentA = lerpColor565(getColorFG(), getColorBG(), 0.30f);
    uint16_t titleAccentB = lerpColor565(getColorBG(), getColorFG(), 0.08f);

    // compute text slide-out trigger: 1s before pig jump
    if (bootIntro.textSlideOutMs == 0) {
        uint32_t jumpMs = MenuPig::getBootWardriveJumpStartMs();
        if (jumpMs > 1000) bootIntro.textSlideOutMs = jumpMs - 1000;
        else bootIntro.textSlideOutMs = 1;
    }

    // One exit clock owns every title line (0 = staged, 1 = off-screen).
    float slideOutT = 0.0f;
    if (introElapsed >= bootIntro.textSlideOutMs) {
        slideOutT = Gfx::clamp01((float)(introElapsed - bootIntro.textSlideOutMs)
                                / (float)kBootTextSlideOutMs);
    }

    // accent bars fade with text
    if (slideOutT < 1.0f) {
        uint16_t barColor = (slideOutT > 0.0f)
            ? lerpColor565(titleAccentA, getColorBG(), slideOutT)
            : titleAccentA;
        canvas->fillRect(20, 8, 52, 4, barColor);
        canvas->fillRect(248, 36, 52, 4, barColor);
    }

    drawBootIntroTitleLine("P.I.G.", 104, 36, 4, true, introElapsed, 0, 520,
                           titleCore, titleAccentA, titleAccentB, slideOutT);
    drawBootIntroTitleLine("HAMLET", 164, 86, 4, false, introElapsed, 140, 720,
                           titleCore, titleAccentA, titleAccentB, slideOutT);
    drawBootIntroTitleLine("PANCETTA", 166, 138, 4, true, introElapsed, 300, 820,
                           titleCore, titleAccentA, titleAccentB, slideOutT);
}

static void drawBootIntroScene(uint32_t now) {
    float loadingT = bootIntro.displayedStages / (float)bootIntro.totalStages;
    loadingT = Gfx::clamp01(loadingT);
    MenuPig::drawBootWardriveSceneBase(*canvas, now, now - bootIntro.introStart, loadingT);
}

static void drawBootIntroFrame(uint32_t now) {
    if (!canvas) return;

    canvas->fillSprite(getColorBG());
    drawBootIntroScene(now);

    // hide pig during decompose/void — particles replace it
    if (bootIntro.phase != BootIntroPhase::TP_COLLAPSE &&
        bootIntro.phase != BootIntroPhase::TP_VOID) {
        MenuPig::drawBootWardrivePigOverlay(*canvas, now, now - bootIntro.introStart);
    }

    // Branding is boot UI chrome, so it stays above the boarding character.
    // Drawing it first let the pig punch moving holes through PANCETTA.
    drawBootIntroTitle(now);

    int16_t srcX = 0;
    int16_t srcY = 0;
    int16_t tpX = 0;
    int16_t tpY = 0;
    MenuPig::getBootWardriveTeleportAnchors(srcX, srcY, tpX, tpY);

    switch (bootIntro.phase) {
        case BootIntroPhase::CATCH_UP:
        case BootIntroPhase::LOOP:
        case BootIntroPhase::HOLD_FULL:
            break;
        case BootIntroPhase::TP_CHARGE: {
            float t = Gfx::clamp01((float)(now - bootIntro.phaseStart) / (float)kBootTeleportChargeMs);
            MenuPig::drawTeleportPortalRing(*canvas, srcX, srcY,
                                            4 + (int)lroundf(10.0f * Gfx::smoothstep01(t)));
            break;
        }
        case BootIntroPhase::TP_COLLAPSE: {
            float t = Gfx::clamp01((float)(now - bootIntro.phaseStart) / (float)kBootTeleportCollapseMs);
            MenuPig::drawTeleportCollapseFrame(*canvas,
                                               srcX, srcY,
                                               tpX, tpY,
                                               t,
                                               bootIntro.particles,
                                               bootIntro.particleCount);
            break;
        }
        case BootIntroPhase::TP_VOID: {
            float t = Gfx::clamp01((float)(now - bootIntro.phaseStart) / (float)kBootTeleportVoidMs);
            int pulse = (int)lroundf(sinf(t * 9.42477f) * 2.0f);
            MenuPig::drawTeleportPortalRing(*canvas, tpX, tpY, 18 + pulse);
            break;
        }
        case BootIntroPhase::INACTIVE:
        default:
            break;
    }

    canvas->pushSprite(0, 0);
}

static void pumpBootIntroFrame() {
    uint32_t now = millis();
    updateBootIntroProgress(now);
    drawBootIntroFrame(now);
    SFX::update();
    M5.update();
    delay(16);
}

static void runBootIntroUntilProgressDone(uint32_t minMs) {
    uint32_t start = millis();
    bool drewAnyFrame = false;
    while (!drewAnyFrame || bootIntro.progressDuration != 0 || millis() - start < minMs) {
        pumpBootIntroFrame();
        drewAnyFrame = true;
    }
}

static void runBootIntroPhase(BootIntroPhase phase, uint32_t durationMs) {
    bootIntro.phase = phase;
    bootIntro.phaseStart = millis();
    if (phase == BootIntroPhase::TP_COLLAPSE) {
        sampleBootIntroParticles();
    }
    bool drewAnyFrame = false;
    while (!drewAnyFrame || millis() - bootIntro.phaseStart < durationMs) {
        pumpBootIntroFrame();
        drewAnyFrame = true;
    }
}

void beginBootIntro(uint8_t completedStages, uint8_t totalStages) {
    if (!canvas) return;

    bootIntro = {};
    bootIntro.active = true;
    bootIntro.phase = BootIntroPhase::CATCH_UP;
    bootIntro.introStart = millis();
    bootIntro.phaseStart = bootIntro.introStart;
    bootIntro.totalStages = (totalStages == 0) ? 1 : totalStages;
    // Display hardware necessarily initializes after the early boot stages.
    // Show those stages as already complete on the very first visible frame;
    // animating 0 -> completedStages made real pre-display work look fabricated.
    uint8_t alreadyDone = min(completedStages, bootIntro.totalStages);
    bootIntro.displayedStages = (float)alreadyDone;
    bootIntro.targetStages = alreadyDone;
    bootIntro.progressFrom = bootIntro.displayedStages;
    bootIntro.progressTo = bootIntro.displayedStages;
    bootIntro.progressDuration = 0;
    SFX::play(SFX::HAMLET_BOOT);
    runBootIntroUntilProgressDone(kBootCatchUpMs);
    bootIntro.phase = BootIntroPhase::LOOP;
    bootIntro.phaseStart = millis();
}

void advanceBootIntro(uint8_t completedStages) {
    if (!bootIntro.active) return;
    if (completedStages <= bootIntro.targetStages) {
        drawBootIntroFrame(millis());
        return;
    }
    uint8_t deltaStages = completedStages - bootIntro.targetStages;
    startBootProgressAnimation(completedStages, (uint32_t)deltaStages * kBootProgressStepMs);
    runBootIntroUntilProgressDone((uint32_t)deltaStages * kBootProgressStepMs);
    bootIntro.phase = BootIntroPhase::LOOP;
    bootIntro.phaseStart = millis();
}

void finishBootIntro() {
    if (!bootIntro.active) return;

    if (bootIntro.targetStages < bootIntro.totalStages) {
        advanceBootIntro(bootIntro.totalStages);
    }
    uint32_t minVisibleMs = kBootMinVisibleMs;
    uint32_t sceneVisibleMs = MenuPig::getBootWardriveSceneDurationMs();
    if (sceneVisibleMs > minVisibleMs) minVisibleMs = sceneVisibleMs;
    while (millis() - bootIntro.introStart < minVisibleMs) {
        pumpBootIntroFrame();
    }
    runBootIntroPhase(BootIntroPhase::HOLD_FULL, kBootFinalSceneHoldMs);
    runBootIntroPhase(BootIntroPhase::TP_CHARGE, kBootTeleportChargeMs);
    runBootIntroPhase(BootIntroPhase::TP_COLLAPSE, kBootTeleportCollapseMs);
    runBootIntroPhase(BootIntroPhase::TP_VOID, kBootTeleportVoidMs);
    bootIntro.active = false;
    bootIntro.phase = BootIntroPhase::INACTIVE;
    // no fillScreen — first idle frame's pushSprite overwrites cleanly
}

void drawHoldOverlay() {
    // kept for sub-menu canvases that don't use pushCanvas()
    // draws directly to M5.Display after their pushSprite
    bool ringOnCanvas  = holdRingDrawnOnCanvasFrame;
    bool toastOnCanvas = quickToastDrawnOnCanvasFrame;
    bool helpOnCanvas  = helpOverlayDrawnOnCanvasFrame;
    bool itemOnCanvas  = itemDropDrawnOnCanvasFrame;
    bool xpOnCanvas    = xpNotifOnCanvas;
    holdRingDrawnOnCanvasFrame = false;
    quickToastDrawnOnCanvasFrame = false;
    helpOverlayDrawnOnCanvasFrame = false;
    itemDropDrawnOnCanvasFrame = false;
    xpNotifOnCanvas = false;

    // Mirror pushCanvas ordering for overlay paths that draw after pushSprite.
    if (!xpOnCanvas) {
        drawXPNotifToDisplay();
    }
    if (!itemOnCanvas) {
        drawItemDropToDisplay();
    }
    if (helpOverlayActive && !helpOnCanvas) {
        drawHelpOverlayToDisplay();
    }
    if (!toastOnCanvas) {
        drawQuickToastToDisplay();
    }
    if (!ringOnCanvas) {
        drawHoldRingToDisplay();
    }
}

bool needsOverlayRedraw() {
    XPNotifPhase xpPhaseBefore = xpNotifPhase;
    updateXPNotifState();
    if (xpNotifPhase != XPNotifPhase::IDLE || xpPhaseBefore != XPNotifPhase::IDLE) {
        return true;
    }
    if (itemDropStillActive()) return true;
    // Toast flicker: throttled screens only repaint when told to, so every
    // gap edge has to be reported or the blink freezes on whatever it caught.
    if (isQuickToastActive() && toastVisibleNow() != quickToastPrevVisible) return true;
    if (quickToastNeedsClearRedraw || holdRingNeedsClearRedraw || itemDropNeedsClearRedraw) {
        quickToastNeedsClearRedraw = false;
        holdRingNeedsClearRedraw = false;
        itemDropNeedsClearRedraw = false;
        return true;
    }
    return false;
}

void drawIdleScreen() {
    canvas->fillSprite(getColorBG());
    drawStatusBarInternal(true);
    
    // avatar + mood. the piglet. the speech bubble. the vibe
    Mood::draw(*canvas);

    // info bar. inverted strip below top bar: level, rank, XP progress, streak
    drawIdleInfoBar();

    // goal bar. slightly chunkier in idle so it reads through the grass.
    drawGoalProgressBar(6);
    
    // bottom bar. walk stats + captures (or touch hint)
    if (!drawHintBottomBarTo(canvas)) {
    int bottomY = SCREEN_HEIGHT - BOTTOM_BAR_H;
    canvas->fillRect(0, bottomY, SCREEN_WIDTH, BOTTOM_BAR_H, getColorFG());
    canvas->setTextColor(getColorBG());
    canvas->setTextSize(1);

    // left: steps + capture counts
    uint32_t steps = Pedometer::getSteps();
    char stepsBuf[12];
    formatCompactCount(stepsBuf, sizeof(stepsBuf), steps);
    uint16_t pmkids = Capture::getPMKIDCount();
    uint16_t handshakes = Capture::getHandshakeCount();
    char leftBuf[32];
    snprintf(leftBuf, sizeof(leftBuf), "ST:%s P:%d HS:%d", stepsBuf, pmkids, handshakes);
    canvas->setCursor(4, bottomY + 3);
    canvas->print(leftBuf);

    // right: best D-UCB channel + streak (runs) + distance
    char channelHint[8] = "";
    {
        float bestReward = 0;
        uint8_t bestCh = 0;
        for (uint8_t ch = 1; ch <= 13; ch++) {
            DUCBStats stats = Hunt::getDUCBStats(ch, HuntBehavior::CAMP);
            if (stats.avgReward > bestReward) {
                bestReward = stats.avgReward;
                bestCh = ch;
            }
        }
        if (bestCh > 0 && bestReward > 0) {
            snprintf(channelHint, sizeof(channelHint), "C%d ", bestCh);
        }
    }
    uint32_t dist = Config::getTotalDistance();
    uint16_t streak = Config::getStreak();
    char rightBuf[32];
    if (dist >= 1000) {
        snprintf(rightBuf, sizeof(rightBuf), "%s~%dR %.1fKM", channelHint, streak, dist / 1000.0f);
    } else {
        snprintf(rightBuf, sizeof(rightBuf), "%s~%dR %luM", channelHint, streak, (unsigned long)dist);
    }
    int rightWidth = strlen(rightBuf) * 6;
    canvas->setCursor(SCREEN_WIDTH - rightWidth - 4, bottomY + 3);
    canvas->print(rightBuf);
    } // end hint check
    
    // paranoia toast if active
    drawParanoiaToast();

    // Phase D: critical countdown handled by Mood::draw overlay (no duplicate toast)

    // ==[ TELEPORT PARTICLES ]== cross-mode decompose/reassemble overlay
    if (Teleport::isActive()) {
        Teleport::draw(*canvas, getColorFG(), getColorBG(), millis());
    }

    // Keep Akira shockwave above bars/overlays so blast can occupy full frame.
    Weather::drawShockwave(*canvas);

    pushCanvas();
}

void drawHuntScreen() {
    canvas->fillSprite(getColorBG());
    drawStatusBarInternal(true);

    // attack shake. visualize the violence
    {
        DeauthState ds = Hunt::getDeauthState();
        bool activeAttack = (ds == DeauthState::TARGETING || ds == DeauthState::THROWING);
        bool strongAttack = (ds == DeauthState::THROWING);
        Avatar::setAttackShake(activeAttack, strongAttack);
    }
    
    // avatar + grass animation
    Mood::draw(*canvas);
    
    drawGoalProgressBar(6);
    
    // bottom bar. walk stats + state machine + profile (or touch hint)
    if (!drawHintBottomBarTo(canvas)) {
    int huntBottomY = SCREEN_HEIGHT - BOTTOM_BAR_H;
    canvas->fillRect(0, huntBottomY, SCREEN_WIDTH, BOTTOM_BAR_H, getColorFG());
    canvas->setTextColor(getColorBG());
    canvas->setTextSize(1);

    // left: steps
    uint32_t steps = Pedometer::getSteps();
    char stepsBuf[12];
    formatCompactCount(stepsBuf, sizeof(stepsBuf), steps);
    canvas->setCursor(4, huntBottomY + 3);
    uint8_t combo = Hunt::getCaptureComboCount();
    if (combo >= 2) {
        canvas->printf("Cx%d ST:%s", combo, stepsBuf);
    } else {
        canvas->printf("ST:%s", stepsBuf);
    }

    // center: state machine + profile
    const char* stateStr = "SCAN";
    ProbeState ps = Hunt::getProbeState();
    DeauthState ds = Hunt::getDeauthState();
    uint32_t now = millis();

    // sticky ATK. latch for 500ms so humans can see it. tier-aware.
    if (ds == DeauthState::TARGETING || ds == DeauthState::THROWING) {
        lastATKTime = now;
        uint8_t tier = Hunt::getLastAttackTier();
        stateStr = (tier == 2) ? "FLD" : (tier == 1) ? "3PL" : "ATK";
    } else if (now - lastATKTime < ATK_STICKY_DURATION) {
        uint8_t tier = Hunt::getLastAttackTier();
        stateStr = (tier == 2) ? "FLD" : (tier == 1) ? "3PL" : "ATK";
    } else if (ps == ProbeState::TUNING || ps == ProbeState::AUTHING || ps == ProbeState::SENDING) {
        stateStr = "PROBE";
    } else if (ps == ProbeState::WAITING) {
        stateStr = "WAIT";
    }

    const char* profileStr;
    switch (Hunt::getCurrentBehavior()) {
        case HuntBehavior::CAMP:    profileStr = "CAMP"; break;
        case HuntBehavior::PATROL:  profileStr = "PTRL"; break;
        case HuntBehavior::SPRINT:  profileStr = "SPNT"; break;
        case HuntBehavior::LURK:    profileStr = "LURK"; break;
        default:                    profileStr = "CAMP"; break;
    }

    char midBuf[24];
    snprintf(midBuf, sizeof(midBuf), "%s/%s/CH:%02d", profileStr, stateStr, Hunt::getCurrentChannel());
    int midWidth = strlen(midBuf) * 6;
    canvas->setCursor((SCREEN_WIDTH - midWidth) / 2, huntBottomY + 3);
    canvas->print(midBuf);

    // right: session distance (compact) — draw first to get X position
    uint32_t dist = Config::getTotalDistance();
    char distBuf[16];
    if (dist >= 1000) {
        snprintf(distBuf, sizeof(distBuf), "%.1fK", dist / 1000.0f);
    } else {
        snprintf(distBuf, sizeof(distBuf), "%luM", dist);
    }
    int distWidth = strlen(distBuf) * 6;
    int distX = SCREEN_WIDTH - distWidth - 4;
    canvas->setCursor(distX, huntBottomY + 3);
    canvas->print(distBuf);

    // D-UCB micro-bar: 13ch × (2px bar + 1px gap) = 38px wide, 10px tall
    // snapped LEFT of distance text, separated by 6px (one space)
    {
        HuntBehavior behavior = Hunt::getCurrentBehavior();
        float maxReward = 0.001f;
        float rewards[13];
        for (int ch = 1; ch <= 13; ch++) {
            DUCBStats stats = Hunt::getDUCBStats(ch, behavior);
            rewards[ch-1] = stats.avgReward;
            if (stats.avgReward > maxReward) maxReward = stats.avgReward;
        }

        int ucbW = 12 * 3 + 2;  // 38px total (13 bars: 2px wide, 1px gap)
        int barX = distX - 6 - ucbW;  // 6px space left of distance text
        int barY = huntBottomY + 2;
        int barH = 10;

        for (int i = 0; i < 13; i++) {
            int intensity = (int)((rewards[i] / maxReward) * barH);
            if (intensity < 1 && rewards[i] > 0) intensity = 1;
            int x = barX + i * 3;
            if (intensity > 0) {
                canvas->fillRect(x, barY + barH - intensity, 2, intensity, getColorBG());
            }
        }
    }
    } // end hunt hint check

    // network overlay if visible
    if (overlayVisible) {
        drawNetworkOverlay();
    }
    
    // paranoia toast
    drawParanoiaToast();

    // ==[ TELEPORT PARTICLES ]== cross-mode decompose/reassemble overlay
    if (Teleport::isActive()) {
        Teleport::draw(*canvas, getColorFG(), getColorBG(), millis());
    }

    // Keep Akira shockwave above bars/overlays so blast can occupy full frame.
    Weather::drawShockwave(*canvas);

    pushCanvas();
}

static void drawNetworkOverlay() {
    uint16_t fg = getColorFG();
    uint16_t bg = getColorBG();

    // inverted overlay box. taller for 6 networks
    canvas->fillRect(10, 25, SCREEN_WIDTH - 20, 85, bg);
    canvas->fillRect(12, 27, SCREEN_WIDTH - 24, 81, fg);

    const DetectedNetwork* nets = Hunt::getNetworks();
    uint16_t count = Hunt::getNetworkCount();

    // count overlay stats
    uint16_t withClients = 0;
    uint16_t totalClients = 0;
    for (uint16_t i = 0; i < count; i++) {
        if (nets[i].clientCount > 0) withClients++;
        totalClients += nets[i].clientCount;
    }
    uint16_t probes = Hunt::getHarvestedCount();

    canvas->setTextColor(bg);
    canvas->setCursor(16, 30);
    canvas->printf("NETS %d/%d C:%d PRB:%d", withClients, count, totalClients, probes);

    int y = 42;
    uint32_t now = millis();
    for (int i = 0; i < min((int)count, 6); i++) {
        bool hasClients = nets[i].clientCount > 0;

        // highlight attackable networks
        // H=8 at y: fill y..y+7 matches 8-px glyph exactly (0/0 pad). Row pitch 9
        // leaves a 1-row fg stripe between consecutive highlights — acts as a
        // separator. H=9 at y-1 would be odd-height and bias padding by 1 px.
        if (hasClients) {
            canvas->fillRect(14, y, SCREEN_WIDTH - 28, 8, bg);
            canvas->setTextColor(fg);
        } else {
            canvas->setTextColor(bg);
        }

        const char* ssidDisplay;
        if (nets[i].ssid[0] >= ' ' && nets[i].ssid[0] <= '~') {
            ssidDisplay = nets[i].ssid;
        } else if (nets[i].ssid[0] == '\0' || nets[i].isHidden) {
            ssidDisplay = "<hidden>";
        } else {
            ssidDisplay = "<?>";
        }

        // ==[ PRIORITY TAGS ]== first match wins, 3-4 chars max
        const char* tag = "";
        bool tagHighlight = false;
        if (hasClients) {
            // client count shown in format below (C:N)
        } else if (nets[i].hasHandshake) {
            tag = "HS!"; tagHighlight = true;
        } else if (nets[i].hasPMKID) {
            tag = "PMK"; tagHighlight = true;
        } else if (nets[i].probeAttempts > 0 && nets[i].gotResponse) {
            tag = "PR?";  // got assoc response but no PMKID (PMF or bad luck)
        } else if (nets[i].probeAttempts > 0 && !nets[i].gotResponse &&
                   now - nets[i].lastProbeTime < 5000) {
            tag = "PR*";  // probe in-flight (within 5s backoff window)
        } else if (nets[i].probeAttempts > 0 && !nets[i].gotResponse) {
            tag = "PR-";  // probed, no response (AP dead / silent / out of range)
        } else if (nets[i].wpsState == 1 && !nets[i].wpsLocked) {
            tag = "WPS"; tagHighlight = true;   // unconfigured + unlocked = attackable
        } else if (nets[i].wpsState == 1) {
            tag = "WPL";                         // WPS present but AP Setup Locked
        } else if (nets[i].hasPMF && Config::getEAPOLInjectionEnabled()) {
            tag = "3PL"; tagHighlight = true;
        } else if (nets[i].authmode == WIFI_AUTH_WPA3_PSK && nets[i].hasPMF &&
                   Config::getSAEAttackEnabled()) {
            tag = "SAE";
        } else if (nets[i].hasPMF) {
            tag = "PMF";
        } else if (nets[i].seqAnomalyCount > 0) {
            tag = "SEQ"; tagHighlight = true;  // M2 replay/injection marker
        } else if (nets[i].rssiAnomalyCount > 0) {
            tag = "RSS"; tagHighlight = true;  // M3 position drift
        }

        // highlight captured / WPS-vulnerable / EAPOL-injectable
        if (tagHighlight && !hasClients) {
            canvas->fillRect(14, y, SCREEN_WIDTH - 28, 8, bg);
            canvas->setTextColor(fg);
        }

        canvas->setCursor(16, y);
        char line[48];
        // +/- = approach/retreat over last ~2-4s ring, blank = stable/cold
        char trendCh = ' ';
        if (nets[i].rssiHistoryCount >= 2) {
            if (nets[i].rssiTrend >= 2) trendCh = '+';
            else if (nets[i].rssiTrend <= -2) trendCh = '-';
        }
        if (hasClients) {
            snprintf(line, sizeof(line), "%-11.11s %02X%02X-%02X%02X %c%3ddB/%-2d C:%d",
                     ssidDisplay,
                     nets[i].bssid[0], nets[i].bssid[1],
                     nets[i].bssid[4], nets[i].bssid[5],
                     trendCh, nets[i].rssi, nets[i].channel, nets[i].clientCount);
        } else if (tag[0] != '\0') {
            snprintf(line, sizeof(line), "%-11.11s %02X%02X-%02X%02X %c%3ddB/%-2d %s",
                     ssidDisplay,
                     nets[i].bssid[0], nets[i].bssid[1],
                     nets[i].bssid[4], nets[i].bssid[5],
                     trendCh, nets[i].rssi, nets[i].channel, tag);
        } else {
            snprintf(line, sizeof(line), "%-11.11s %02X%02X-%02X%02X %c%3ddB/%-2d",
                     ssidDisplay,
                     nets[i].bssid[0], nets[i].bssid[1],
                     nets[i].bssid[4], nets[i].bssid[5],
                     trendCh, nets[i].rssi, nets[i].channel);
        }
        canvas->print(line);

        y += 9;  // tighter line spacing for 6 networks
    }

    canvas->setTextColor(bg);
}

void drawSpectrumScreen() {
    // Spectrum owns its frame clear: it preserves the carrier plot band between
    // frames for progressive reveal, or wipes the whole sprite when ineligible.
    Spectrum::beginFrame(*canvas);
    drawStatusBar();

    // Native recon activity bar - only truthful in the 2.4GHz viewport.
    // The 5GHz viewport is owned by C5 snapshot/observer evidence below.
    // Layout: "D-UCB:" label + 3px gap + 13-channel bar, centered in middle area
    // Left text ends ~x=86, right text starts ~x=178, middle space = 92px
    if (!Spectrum::isShowing5GHz()) {
        uint16_t activity[13] = {0};
        uint32_t timeMs[13] = {0};
        uint16_t networks[13] = {0};
        int8_t peakRssi[13] = {0};
        Spectrum::getReconChannelData(activity, timeMs, networks, peakRssi);

        uint16_t maxActivity = 1;
        for (int i = 0; i < 13; i++) {
            if (activity[i] > maxActivity) maxActivity = activity[i];
        }

        // Position safely between left/right status text (no overlaps).
        const char* label = "D-UCB:";
        int labelW = (int)strlen(label) * 6;
        int barW = (13 - 1) * 3 + 2;  // 13 bars, 2px width, 1px gap
        int gapPx = 3;
        int totalW = labelW + gapPx + barW;

        // Leave a small gutter from both status texts.
        int safeL = statusBarLeftEndX + 6;
        int safeR = statusBarRightStartX - 6;

        // If the bar can't fit, skip rather than collide.
        if (safeR - safeL >= totalW) {
            int labelX = (safeL + safeR - totalW) / 2;
            if (labelX < safeL) labelX = safeL;
            if (labelX > safeR - totalW) labelX = safeR - totalW;
            int barX = labelX + labelW + gapPx;
            int barY = 2;
            int barH = TOP_BAR_H - 4;

            // Draw label
            uint16_t fg = statusBarLastFg ? statusBarLastFg : getColorFG();
            canvas->setTextColor(fg);
            canvas->setTextSize(1);
            canvas->setCursor(labelX, 3);
            canvas->print(label);

            // Draw bars
            for (int i = 0; i < 13; i++) {
                int intensity = (activity[i] * barH) / maxActivity;
                if (intensity < 1 && activity[i] > 0) intensity = 1;
                int x = barX + i * 3;
                if (intensity > 0) {
                    canvas->fillRect(x, barY + barH - intensity, 2, intensity, fg);
                }
            }
        }
    }

    Spectrum::draw(*canvas);

    pushCanvas();
}
void drawWalkStats() {
    uint16_t fg = getColorFG();
    uint16_t bg = getColorBG();
    canvas->fillSprite(bg);
    drawStatusBar();  // status bar owns "W4LK ST4TS" — no duplicate body title

    canvas->setTextColor(fg);
    canvas->setTextSize(1);
    canvas->setTextDatum(TL_DATUM);

    // stats — start tight under status bar
    int y = TOP_BAR_H + 8;
    canvas->setCursor(10, y);
    canvas->printf("STEPS      %d", Pedometer::getSteps());

    canvas->setCursor(10, y + 18);
    canvas->printf("DISTANCE   %.2f KM", Pedometer::getDistanceKm());

    canvas->setCursor(10, y + 36);
    canvas->printf("CALORIES   %d", Pedometer::getCalories());

    // bottom bar
    if (!drawHintBottomBarTo(canvas)) {
        drawBottomBar3("", "", "[C+] 3X1T");
    }

    pushCanvas();
}

// ==[ FLOCK UI ]== four-pane operator dashboard (FNOW/3)
enum class FlockPane : uint8_t { MESH = 0, VISION = 1, PEERS = 2, WIRE = 3, COUNT = 4 };

static FlockPane flockPane = FlockPane::MESH;
static int flockPeerScroll = 0;

static const int FLOCK_BREAD_Y = TOP_BAR_H + 2;   // 16 — breadcrumb row top
static const int FLOCK_BREAD_H = 12;              // breadcrumb row height
static const int FLOCK_STATS_Y = TOP_BAR_H + 16;  // 30 — stats row baseline
static const int FLOCK_LIST_Y = TOP_BAR_H + 30;   // 44 — first content row

static const char* flockClockName(uint8_t clockSource) {
    static const char* const names[] = {"NONE", "GPS", "MASTER", "RTC"};
    return (clockSource <= 3) ? names[clockSource] : "NONE";
}

void flockOnEnter() {
    flockPane = FlockPane::MESH;
    flockPeerScroll = 0;
}

void flockNextPane() {
    flockPane = (FlockPane)(((uint8_t)flockPane + 1u) % (uint8_t)FlockPane::COUNT);
    flockPeerScroll = 0;
}

void flockPrevPane() {
    uint8_t p = (uint8_t)flockPane;
    p = (p == 0) ? ((uint8_t)FlockPane::COUNT - 1u) : (p - 1u);
    flockPane = (FlockPane)p;
    flockPeerScroll = 0;
}

void flockScrollPeers(int delta) {
    if (flockPane != FlockPane::PEERS) return;
    flockPeerScroll += delta;
    if (flockPeerScroll < 0) flockPeerScroll = 0;
}

void flockBtnPrev() {
    if (flockPane == FlockPane::PEERS) flockScrollPeers(-1);
    else flockPrevPane();
}

void flockBtnNext() {
    if (flockPane == FlockPane::PEERS) flockScrollPeers(1);
    else flockNextPane();
}

static void drawFlockBreadcrumb(uint8_t activePane) {
    uint16_t fg = getColorFG();
    uint16_t bg = getColorBG();
    uint16_t dim = lerpColor565(fg, bg, 0.5f);
    static const char* const tabs[4] = {"MESH", "V1S", "PEER", "W1R"};

    int widths[4];
    int totalW = 0;
    for (int i = 0; i < 4; ++i) {
        widths[i] = (int)strlen(tabs[i]) * 6 + 6;
        totalW += widths[i];
    }
    totalW += 3 * 3;

    int x = (SCREEN_WIDTH - totalW) / 2;
    canvas->setTextSize(1);
    canvas->setTextDatum(MC_DATUM);
    for (int i = 0; i < 4; ++i) {
        if ((uint8_t)i == activePane) {
            canvas->fillRect(x, FLOCK_BREAD_Y, widths[i], FLOCK_BREAD_H, fg);
            canvas->setTextColor(bg);
        } else {
            canvas->setTextColor(fg);
        }
        canvas->drawString(tabs[i], x + widths[i] / 2, FLOCK_BREAD_Y + FLOCK_BREAD_H / 2);
        x += widths[i] + 3;
    }
    canvas->setTextColor(fg);
    canvas->setTextDatum(TL_DATUM);
}

static void formatTileComponent(int32_t e7, char* out, size_t len) {
    bool neg = e7 < 0;
    uint32_t a = neg ? (uint32_t)(-(int64_t)e7) : (uint32_t)e7;
    uint32_t whole = a / 10000000u;
    uint32_t frac = (a / 1000000u) % 10u;
    snprintf(out, len, "%s%lu.%lu", neg ? "-" : "", (unsigned long)whole, (unsigned long)frac);
}

static void formatTileCoord(int32_t latE7, int32_t lonE7, char* out, size_t len) {
    char lat[12];
    char lon[12];
    formatTileComponent(latE7, lat, sizeof(lat));
    formatTileComponent(lonE7, lon, sizeof(lon));
    snprintf(out, len, "%s/%s", lat, lon);
}

static void drawFlockCandidateRow(int y, const NowFlockGraph::CandidateView& c, bool compact) {
    uint16_t fg = getColorFG();
    uint16_t dim = lerpColor565(fg, getColorBG(), 0.5f);
    uint16_t rowColor = c.exportable || c.peerCorroborated ? fg : dim;
    char line[48];
    char tile[20];

    if (compact) {
        int barW = 28;
        int fillW = (int)((uint32_t)barW * c.confidence / 100u);
        canvas->drawRect(8, y + 1, barW, 8, rowColor);
        if (fillW > 0) {
            canvas->fillRect(9, y + 2, fillW > barW - 2 ? barW - 2 : fillW, 6, rowColor);
        }
        formatTileCoord(c.tileLatE7, c.tileLonE7, tile, sizeof(tile));
        snprintf(line, sizeof(line), "%cT%u %s +%u",
                 c.local ? 'L' : 'P', c.tier, tile, c.peerCoverage);
        canvas->setTextColor(rowColor);
        canvas->setCursor(40, y);
        canvas->print(line);
        drawStatusPillTo(canvas, SCREEN_WIDTH - 8, y + 1,
                         c.exportable ? "EXP" : (c.peerCorroborated ? "ASM" : "RAW"),
                         "OK", false, false);
        return;
    }

    formatTileCoord(c.tileLatE7, c.tileLonE7, tile, sizeof(tile));
    canvas->setTextColor(c.local ? fg : dim);
    canvas->setCursor(8, y);
    canvas->print(c.local ? ">" : " ");
    canvas->setTextColor(rowColor);
    canvas->setCursor(16, y);
    snprintf(line, sizeof(line), "%04lX", (unsigned long)(c.candidateId & 0xFFFFu));
    canvas->print(line);
    canvas->setCursor(46, y);
    snprintf(line, sizeof(line), "T%u", c.tier);
    canvas->print(line);
    canvas->setCursor(64, y);
    canvas->print(tile);
    canvas->setCursor(150, y);
    snprintf(line, sizeof(line), "%u/%u", c.siteScore, c.confidence);
    canvas->print(line);
    canvas->setCursor(210, y);
    snprintf(line, sizeof(line), "+%up", c.peerCoverage);
    canvas->print(line);
    if (!c.local && c.originNodeId) {
        snprintf(line, sizeof(line), "v%02lX", (unsigned long)(c.originNodeId & 0xFFu));
        canvas->setCursor(238, y);
        canvas->print(line);
    }
    drawStatusPillTo(canvas, SCREEN_WIDTH - 8, y + 1,
                     c.exportable ? "EXP" : (c.peerCorroborated ? "ASM" : "RAW"),
                     "OK", false, false);
}

static void drawFlockMeshPane(const NowFlock::Status& st, uint32_t nowMs) {
    uint16_t fg = getColorFG();
    uint16_t dim = lerpColor565(fg, getColorBG(), 0.5f);
    char line[56];
    const char* clk = flockClockName(st.clockSource);

    snprintf(line, sizeof(line), "%s %08lX clk:%s ch:%u",
             NowFlock::roleName(), (unsigned long)st.nodeId, clk, st.channel);
    canvas->setTextColor(fg);
    canvas->setCursor(8, FLOCK_STATS_Y);
    canvas->print(line);
    snprintf(line, sizeof(line), "p:%u c:%u e:%u%s",
             st.peerCount, st.candidates, st.exportable,
             st.corroborated ? " ASM" : "");
    canvas->setCursor(210, FLOCK_STATS_Y);
    canvas->print(line);

    NowFlockState::PeerView peers[5];
    uint8_t peerN = NowFlockState::getActivePeers(peers, 5, nowMs);
    int railY = FLOCK_LIST_Y + 14;
    canvas->setTextColor(dim);
    canvas->setCursor(8, FLOCK_LIST_Y);
    snprintf(line, sizeof(line), "links:%u mst:%08lX", st.peerCount, (unsigned long)st.masterNodeId);
    canvas->print(line);

    int localX = 22;
    canvas->drawFastHLine(8, railY - 12, SCREEN_WIDTH - 16, dim);
    canvas->drawCircle(localX, railY, 8, fg);
    canvas->fillCircle(localX, railY, 3, fg);
    canvas->setTextColor(fg);
    canvas->setCursor(localX - 9, railY + 11);
    canvas->print("ME");

    if (peerN == 0) {
        canvas->setTextColor(dim);
        canvas->setCursor(48, railY - 4);
        canvas->print("WAITING FOR HELLO");
    }
    uint8_t railShow = peerN > 4 ? 4 : peerN;
    for (uint8_t i = 0; i < railShow; ++i) {
        int px = 64 + i * 58;
        uint16_t pc = peers[i].sawSighting ? fg : dim;
        canvas->drawLine(localX + 9, railY, px - 9, railY, pc);
        if (peers[i].sawSighting) {
            canvas->fillRect(px - 9, railY - 7, 18, 14, fg);
            canvas->setTextColor(getColorBG());
        } else {
            canvas->drawRect(px - 9, railY - 7, 18, 14, dim);
            canvas->setTextColor(dim);
        }
        snprintf(line, sizeof(line), "%02lX", (unsigned long)(peers[i].nodeId & 0xFFu));
        canvas->setCursor(px - 6, railY - 3);
        canvas->print(line);
        canvas->setTextColor(pc);
        snprintf(line, sizeof(line), "%c%us",
                 peers[i].role == NowFlock::ROLE_MASTER ? 'M' : 'C',
                 peers[i].ageS > 99 ? 99 : peers[i].ageS);
        canvas->setCursor(px - 12, railY + 11);
        canvas->print(line);
    }
    if (peerN > 4) {
        canvas->setTextColor(dim);
        canvas->setCursor(64 + 4 * 58, railY - 4);
        snprintf(line, sizeof(line), "+%u", (unsigned)(peerN - 4));
        canvas->print(line);
    }

    int visionY = railY + 28;
    canvas->setTextColor(fg);
    canvas->setCursor(8, visionY);
    canvas->print("TOP S1GHT1NGS");
    NowFlockGraph::CandidateView top[3];
    uint8_t topN = NowFlockGraph::getTopCandidates(top, 3, nowMs);
    int y = visionY + 12;
    for (uint8_t i = 0; i < topN; ++i) {
        drawFlockCandidateRow(y, top[i], true);
        y += 12;
    }
    if (topN == 0) {
        canvas->setTextColor(dim);
        canvas->setCursor(8, y);
        canvas->print("3MPTY. RECON FEEDS LSP-1.");
        y += 12;
        canvas->setCursor(8, y);
        canvas->print("HUNT/DEFHOG BUILDS CANDIDATES.");
        y += 12;
    }

    y += 4;
    canvas->drawFastHLine(8, y, SCREEN_WIDTH - 16, dim);
    y += 6;
    drawStatusPillTo(canvas, 72, y, "GATE", "LIVE", st.active, false);
    drawStatusPillTo(canvas, 148, y, "DEG", "RF OK", !st.degraded, false);
    drawStatusPillTo(canvas, SCREEN_WIDTH - 8, y, "FORGN", "CLR", !st.foreignTraffic, false);
    y += 14;
    canvas->setTextColor(fg);
    canvas->setCursor(8, y);
    snprintf(line, sizeof(line), "last:%s txF:%u mask:%03X",
             NowFlock::frameTypeName(st.lastFrameType), st.txFail, st.channelMask & 0x1FFF);
    canvas->print(line);
}

static void drawFlockVisionPane(uint32_t nowMs) {
    uint16_t fg = getColorFG();
    uint16_t dim = lerpColor565(fg, getColorBG(), 0.5f);
    NowFlockGraph::CandidateView top[8];
    uint8_t topN = NowFlockGraph::getTopCandidates(top, 8, nowMs);

    canvas->setTextColor(dim);
    canvas->setCursor(8, FLOCK_STATS_Y);
    canvas->print("src id  T  tile        sc/cf  p  gate");
    int y = FLOCK_LIST_Y;
    for (uint8_t i = 0; i < topN; ++i) {
        drawFlockCandidateRow(y, top[i], false);
        y += 14;
    }
    if (topN == 0) {
        canvas->setTextColor(fg);
        canvas->setCursor(8, FLOCK_LIST_Y + 20);
        canvas->print("3MPTY. SK1LL 1SSU3?");
        canvas->setTextColor(dim);
        canvas->setCursor(8, FLOCK_LIST_Y + 34);
        canvas->print("H1T HUNT. L3T DEFHOG WATCH.");
    }
}

static void drawFlockPeersPane(uint32_t nowMs) {
    uint16_t fg = getColorFG();
    uint16_t dim = lerpColor565(fg, getColorBG(), 0.5f);
    NowFlockState::PeerView peers[NowFlock::PEER_MAX];
    uint8_t peerN = NowFlockState::getActivePeers(peers, NowFlock::PEER_MAX, nowMs);
    static const int ROW_H = 14;
    static const int VISIBLE = 9;

    if (flockPeerScroll > (int)peerN - VISIBLE) {
        flockPeerScroll = (int)peerN - VISIBLE;
        if (flockPeerScroll < 0) flockPeerScroll = 0;
    }

    canvas->setTextColor(fg);
    canvas->setCursor(8, FLOCK_STATS_Y);
    char stats[32];
    snprintf(stats, sizeof(stats), "active:%u", peerN);
    canvas->print(stats);
    if (peerN > 0) {
        snprintf(stats, sizeof(stats), "%d/%d", flockPeerScroll + 1,
                 flockPeerScroll + VISIBLE > (int)peerN ? peerN : flockPeerScroll + VISIBLE);
        canvas->setCursor(250, FLOCK_STATS_Y);
        canvas->print(stats);
    }

    if (peerN == 0) {
        canvas->setTextColor(fg);
        canvas->setCursor(8, FLOCK_LIST_Y + 20);
        canvas->print("N0 P33RS. BR04DC4ST L1ST3NS.");
        canvas->setTextColor(dim);
        canvas->setCursor(8, FLOCK_LIST_Y + 34);
        canvas->print("[B] P33R_R3Q NUDGES SUMMARIES.");
        return;
    }

    int start = flockPeerScroll;
    int end = start + VISIBLE;
    if (end > (int)peerN) end = peerN;
    int y = FLOCK_LIST_Y;
    char line[56];
    for (int i = start; i < end; ++i) {
        const NowFlockState::PeerView& p = peers[i];
        uint16_t rowFg = p.sawSighting ? fg : dim;
        canvas->setTextColor(rowFg);
        const char* bat = (p.batteryPct == 0xFF) ? "--" : nullptr;
        char batBuf[8];
        if (!bat) {
            snprintf(batBuf, sizeof(batBuf), "%u%%", p.batteryPct);
            bat = batBuf;
        }
        char sightBuf[14];
        if (p.sawSighting) {
            uint16_t sightAge = p.sightingAgeS > 999u ? 999u : p.sightingAgeS;
            snprintf(sightBuf, sizeof(sightBuf), "%u@%us", p.sightingRx, sightAge);
        } else {
            strcpy(sightBuf, "--");
        }
        snprintf(line, sizeof(line), "%c %02lX %c bat:%s ch:%u sg:%s",
                 p.sawSighting ? '>' : ' ',
                 (unsigned long)(p.nodeId & 0xFFu),
                 p.role == NowFlock::ROLE_MASTER ? 'M' : 'C',
                 bat, p.channel, sightBuf);
        canvas->setCursor(8, y);
        canvas->print(line);
        y += ROW_H;
    }
    if (start > 0) {
        canvas->setTextColor(dim);
        canvas->setCursor(SCREEN_WIDTH - 12, FLOCK_LIST_Y + 2);
        canvas->print("^");
    }
    if (end < (int)peerN) {
        canvas->setTextColor(dim);
        canvas->setCursor(SCREEN_WIDTH - 12, FLOCK_LIST_Y + VISIBLE * ROW_H - 10);
        canvas->print("v");
    }
}

static void drawFlockWirePane(const NowFlock::Status& st) {
    uint16_t fg = getColorFG();
    uint16_t dim = lerpColor565(fg, getColorBG(), 0.5f);
    char line[56];
    int y = FLOCK_LIST_Y;

    const char* clk = flockClockName(st.clockSource);

    canvas->setTextColor(fg);
    canvas->setCursor(8, FLOCK_STATS_Y);
    snprintf(line, sizeof(line), "FNOW/3 init:%s grp:%s",
             st.initialized ? "Y" : "N", st.foreignTraffic ? "FOREIGN" : "OK");
    canvas->print(line);

    const char* rows[] = {
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr
    };
    char rowBuf[8][48];
    snprintf(rowBuf[0], sizeof(rowBuf[0]), "node:%08lX role:%s",
             (unsigned long)st.nodeId, NowFlock::roleName());
    snprintf(rowBuf[1], sizeof(rowBuf[1]), "master:%08lX clk:%s",
             (unsigned long)st.masterNodeId, clk);
    snprintf(rowBuf[2], sizeof(rowBuf[2]), "ctrl ch:%u mask:%03X peers:%u",
             st.channel, st.channelMask & 0x1FFF, st.peerCount);
    snprintf(rowBuf[3], sizeof(rowBuf[3]), "graph:%u cand %u export %s",
             st.candidates, st.exportable, st.corroborated ? "ASM" : "solo");
    snprintf(rowBuf[4], sizeof(rowBuf[4]), "last frame:%s",
             NowFlock::frameTypeName(st.lastFrameType));
    snprintf(rowBuf[5], sizeof(rowBuf[5]), "rx bad:%u dup:%u auth:%u",
             st.rxBad, st.rxDup, st.authFail);
    snprintf(rowBuf[6], sizeof(rowBuf[6]), "tx fail:%u link:%s rf:%s",
             st.txFail, st.active ? "live" : "gated",
             st.degraded ? "degraded" : "nominal");
    snprintf(rowBuf[7], sizeof(rowBuf[7]), "cap:%s",
             NowFlock::getLastCaptureAnnotation() ? NowFlock::getLastCaptureAnnotation() : "none");
    for (int i = 0; i < 8; ++i) rows[i] = rowBuf[i];

    for (int i = 0; i < 8; ++i) {
        canvas->setTextColor(i >= 5 ? dim : fg);
        canvas->setCursor(8, y);
        canvas->print(rows[i]);
        y += 14;
    }

    y += 6;
    canvas->drawFastHLine(8, y, SCREEN_WIDTH - 16, dim);
    y += 8;
    canvas->setTextColor(dim);
    canvas->setCursor(8, y);
    canvas->print("LSP-1: no raw MAC/BLE/GPS on wire");
    y += 12;
    canvas->setCursor(8, y);
    canvas->print("radio gated during hunt/spectrum/xfer");
}

static void drawFlockBottomBar(uint8_t pane) {
    static const char* left[] = {"[A]W1R", "[A]MESH", "[A]V1S", "[A]PEER"};
    static const char* right[] = {"[C]V1S", "[C]PEER", "[C]W1R", "[C]MESH"};
    if (!drawHintBottomBarTo(canvas)) {
        if (pane == (uint8_t)FlockPane::PEERS) {
            drawBottomBar3("[A/C]SCR", "[B]P33R_R3Q", "[C+]EXIT");
        } else {
            drawBottomBar3(left[pane], "[B]P33R_R3Q", right[pane]);
        }
    }
}

void drawFlockScreen() {
    NowFlock::Status st = NowFlock::getStatus();
    uint32_t nowMs = millis();

    canvas->fillSprite(getColorBG());
    drawStatusBarInternal(true);

    drawFlockBreadcrumb((uint8_t)flockPane);

    if (!st.enabled) {
        const uint16_t fg = getColorFG();
        const uint16_t dim = lerpColor565(fg, getColorBG(), 0.5f);
        canvas->setTextDatum(MC_DATUM);
        canvas->setTextSize(2);
        canvas->setTextColor(fg);
        canvas->drawString("FNOW/3 IS OFF", SCREEN_WIDTH / 2, 92);
        canvas->setTextSize(1);
        canvas->setTextColor(dim);
        canvas->drawString("TUN3 P1G > N0W F0CK", SCREEN_WIDTH / 2, 121);
        canvas->drawString("ENABLE TO TRANSMIT COORDINATION", SCREEN_WIDTH / 2, 138);
        drawBottomBar3("", "", "[C+] EXIT");
        canvas->setTextDatum(TL_DATUM);
        pushCanvas();
        return;
    }

    switch (flockPane) {
        case FlockPane::MESH:  drawFlockMeshPane(st, nowMs); break;
        case FlockPane::VISION: drawFlockVisionPane(nowMs); break;
        case FlockPane::PEERS: drawFlockPeersPane(nowMs); break;
        case FlockPane::WIRE:  drawFlockWirePane(st); break;
        default: break;
    }

    drawFlockBottomBar((uint8_t)flockPane);
    canvas->setTextDatum(TL_DATUM);
    pushCanvas();
}

void drawSyncScreen() {
    drawFlockScreen();
}

void drawPowerMenu() {
    if (showingSleepWarning) {
        drawSleepWarning();
        return;
    }
    
    canvas->fillSprite(getColorBG());
    drawStatusBar();  // status bar owns "PWR 0PT10NS" — no duplicate body title

    canvas->setTextColor(getColorFG());

    canvas->setTextSize(1);
    canvas->setTextDatum(TL_DATUM);

    const char* options[] = {
#if HAMLET_HAS_TOUCH_SLEEP_WAKE
        "DEEP SLEEP", "LIGHT SLEEP",
#else
        "DEEP SLEEP [CORE2]", "LIGHT SLEEP [CORE2]",
#endif
        "POWER OFF", "CANCEL"
    };
    const char* actionDetails[] = {
        "ENDS SESSION // COLD BOOT",
        "PSRAM HELD // TOUCH WAKE",
        "ENDS SESSION // FULL SHUTDOWN",
        "RETURN TO IDLE // NO CHANGE"
    };
    static_assert(sizeof(options) / sizeof(options[0]) == PowerPolicy::ACTION_COUNT,
                  "power option labels drifted from PowerPolicy::Action");
    static_assert(sizeof(actionDetails) / sizeof(actionDetails[0]) == PowerPolicy::ACTION_COUNT,
                  "power action details drifted from PowerPolicy::Action");

    int startY = TOP_BAR_H + 8;  // tight under status bar (no body title)

    for (int i = 0; i < PowerPolicy::ACTION_COUNT; i++) {
        int y = startY + i * 18;
        const bool available = PowerPolicy::isAvailable(
            PowerPolicy::actionFromIndex(i), HAMLET_HAS_TOUCH_SLEEP_WAKE);
        if (i == powerOption) {
            canvas->fillRect(5, y, SCREEN_WIDTH - 10, 18, getColorFG());
            canvas->setTextColor(getColorBG());
        } else {
            canvas->setTextColor(getColorFG());
        }
        // size-1 text (8px) vertically centered in 18-row fill -> 5/5 pad
        canvas->setCursor(10, y + 5);
        if (i == powerOption) {
            canvas->print("> ");
        } else if (!available) {
            canvas->print("- ");
        } else {
            canvas->print("  ");
        }
        canvas->print(options[i]);
    }

    const PowerPolicy::Action selectedAction =
        PowerPolicy::actionFromIndex(powerOption);
    const uint16_t totalCaptures = Capture::getTotalCount();
    const uint16_t pendingUploads = Capture::getUnsyncedCount();
    const bool sdMounted = SDStorage::isAvailable();

    // Selected-action dossier. This area is intentionally stable while the
    // first-visit touch hint blinks; power semantics and controls must not hide.
    const int detailY = startY + PowerPolicy::ACTION_COUNT * 18 + 8;
    const int detailH = 94;
    canvas->drawRect(5, detailY, SCREEN_WIDTH - 10, detailH, getColorFG());
    canvas->fillRect(5, detailY, SCREEN_WIDTH - 10, 16, getColorFG());
    canvas->setTextColor(getColorBG());
    canvas->setTextDatum(MC_DATUM);
    canvas->drawString(actionDetails[powerOption], SCREEN_WIDTH / 2, detailY + 8);

    char line[64];
    canvas->setTextColor(getColorFG());
    snprintf(line, sizeof(line), "VBUS:%s  ADAPT:%s  FPS:%u",
             Power::isExternalPowerPresent() ? "ON" : "OFF",
             Power::getAdaptationStateLabel(),
             static_cast<unsigned>(Power::getTargetFPS()));
    canvas->drawString(line, SCREEN_WIDTH / 2, detailY + 29);
    snprintf(line, sizeof(line), "CAP:%u  PEND:%u  SD:%s",
             static_cast<unsigned>(totalCaptures),
             static_cast<unsigned>(pendingUploads),
             sdMounted ? "MOUNTED" : "NONE");
    canvas->drawString(line, SCREEN_WIDTH / 2, detailY + 44);

    canvas->drawFastHLine(14, detailY + 57, SCREEN_WIDTH - 28, getColorFG());
    const char* outcome = nullptr;
    if (PowerPolicy::clearsLiveMemory(selectedAction)) {
        if (totalCaptures > 0 && !sdMounted) {
            outcome = "NO SD: LIVE CAPTURES WILL CLEAR";
        } else if (sdMounted) {
            outcome = "SECOND B + SD SEAL REQUIRED";
        } else {
            outcome = "SECOND B CONFIRMS SESSION END";
        }
    } else if (selectedAction == PowerPolicy::Action::LIGHT_SLEEP) {
        outcome = "NO CASE CLOSE. SESSION RESUMES.";
    } else {
        outcome = "SAFE DEFAULT ON EVERY ENTRY.";
    }
    canvas->drawString(outcome, SCREEN_WIDTH / 2, detailY + 73);
    canvas->setTextDatum(TL_DATUM);

    drawBottomBar3("[A/C] P1CK", "[B] S3L3CT", "[C+] C4NC3L");

    pushCanvas();
}

void drawSleepWarning() {
    uint16_t fg = getColorFG();
    uint16_t bg = getColorBG();
    uint32_t now = millis();
    
    canvas->fillSprite(bg);
    canvas->setTextColor(fg);
    
    // top bar
    drawStatusBar();
    
    // Sleep warning spans the shared 320px panel.
    int boxW = 260;
    int boxH = 90;
    int boxX = (SCREEN_WIDTH - boxW) / 2;
    int boxY = 26;
    
    // toast bg + border
    canvas->fillRoundRect(boxX, boxY, boxW, boxH, 6, fg);
    canvas->drawRoundRect(boxX, boxY, boxW, boxH, 6, bg);
    
    canvas->setTextColor(bg);
    canvas->setTextDatum(MC_DATUM);
    canvas->setTextSize(1);
    
    const PowerPolicy::Action action = PowerPolicy::actionFromIndex(powerOption);
    const char* actionName = (action == PowerPolicy::Action::POWER_OFF)
        ? "POWER OFF" : "DEEP SLEEP";
    const uint16_t totalCaptures = Capture::getTotalCount();
    const uint16_t unsyncedCaptures = Capture::getUnsyncedCount();
    const bool sdReady = SDStorage::isAvailable();

    // blinking header
    int lineY = boxY + 12;
    char line[48];
    if ((now / 400) % 2 == 0) {
        snprintf(line, sizeof(line), "!! CONFIRM %s !!", actionName);
    } else {
        snprintf(line, sizeof(line), "** CONFIRM %s **", actionName);
    }
    canvas->drawString(line, boxX + boxW/2, lineY);
    
    // Line 2-3: exact persistence outcome for this attempt.
    lineY += 14;
    if (totalCaptures == 0) {
        canvas->drawString("LIVE RAM WILL CLEAR.", boxX + boxW/2, lineY);
    } else if (sdReady) {
        snprintf(line, sizeof(line), "%u CAPTURES WILL SEAL TO SD.", totalCaptures);
        canvas->drawString(line, boxX + boxW/2, lineY);
    } else {
        snprintf(line, sizeof(line), "%u CAPTURES ARE PSRAM-ONLY.", totalCaptures);
        canvas->drawString(line, boxX + boxW/2, lineY);
    }
    lineY += 11;
    if (totalCaptures == 0) {
        canvas->drawString("SAVED SETTINGS RETURN ON BOOT.", boxX + boxW/2, lineY);
    } else if (sdReady) {
        snprintf(line, sizeof(line), "%u PENDING UPLOAD; SD KEEPS THEM.", unsyncedCaptures);
        canvas->drawString(line, boxX + boxW/2, lineY);
    } else {
        canvas->drawString("THIS ACTION WILL ERASE THEM.", boxX + boxW/2, lineY);
    }

    // Line 4: recovery guidance
    lineY += 14;
    canvas->drawString(sdReady ? "SD SEAL MUST PASS BEFORE EXIT."
                               : "CANCEL IF LIVE CAPTURES MATTER.",
                       boxX + boxW/2, lineY);
    
    // Line 5: Buttons — A=left B=center C=right
    lineY += 14;
    canvas->setTextDatum(MC_DATUM);
    canvas->drawString("[B] PR0C33D", SCREEN_WIDTH / 2, lineY);
    canvas->setTextDatum(MR_DATUM);
    canvas->drawString("[C+] C4NC3L", boxX + boxW - 8, lineY);

    canvas->setTextDatum(TL_DATUM);
    pushCanvas();
}

// ==[ CASE CLOSED ]== session end ceremony. peak-end rule (Kahneman 1993).
void drawCaseClosed() {
    uint16_t fg = getColorFG();
    uint16_t bg = getColorBG();

    canvas->fillSprite(bg);

    // inverted header bar
    canvas->fillRect(0, 0, SCREEN_WIDTH, 18, fg);
    canvas->setTextColor(bg);
    canvas->setTextSize(2);
    canvas->setTextDatum(MC_DATUM);
    canvas->drawString("CASE CLOSED", SCREEN_WIDTH / 2, 9);

    // accent line
    canvas->drawFastHLine(20, 22, SCREEN_WIDTH - 40, fg);

    canvas->setTextColor(fg);
    canvas->setTextSize(1);
    canvas->setTextDatum(TL_DATUM);

    int y = 30;
    const int lineH = 14;
    const int labelX = 10;
    const int valX = 180;

    // session stats
    uint32_t sessionXP = Config::getSessionXPGained();
    uint32_t steps = Pedometer::getSteps();
    uint16_t pmkids = Capture::getPMKIDCount();
    uint16_t handshakes = Capture::getHandshakeCount();
    uint8_t level = Config::getLevel();
    uint8_t xpProg = Config::getXPProgress();
    const char* rank = Config::getRankName();
    uint16_t streak = Config::getStreak();
    uint8_t achCount = Achievements::getUnlockedCount();

    char valBuf[32];

    canvas->setCursor(labelX, y); canvas->print("SESSION XP");
    snprintf(valBuf, sizeof(valBuf), "+%lu", (unsigned long)sessionXP);
    canvas->setCursor(valX, y); canvas->print(valBuf);
    y += lineH;

    canvas->setCursor(labelX, y); canvas->print("RANK");
    snprintf(valBuf, sizeof(valBuf), "L%d %s (%d%%)", level, rank, xpProg);
    canvas->setCursor(valX, y); canvas->print(valBuf);
    y += lineH;

    canvas->setCursor(labelX, y); canvas->print("CAPTURES");
    snprintf(valBuf, sizeof(valBuf), "P:%d HS:%d", pmkids, handshakes);
    canvas->setCursor(valX, y); canvas->print(valBuf);
    y += lineH;

    canvas->setCursor(labelX, y); canvas->print("STEPS");
    snprintf(valBuf, sizeof(valBuf), "%lu", (unsigned long)steps);
    canvas->setCursor(valX, y); canvas->print(valBuf);
    y += lineH;

    canvas->setCursor(labelX, y); canvas->print("STREAK");
    snprintf(valBuf, sizeof(valBuf), "~%dR", streak);
    canvas->setCursor(valX, y); canvas->print(valBuf);
    y += lineH;

    canvas->setCursor(labelX, y); canvas->print("ACHIEVEMENTS");
    snprintf(valBuf, sizeof(valBuf), "%d/%u", achCount,
             (unsigned)Achievement::ACH_COUNT);
    canvas->setCursor(valX, y); canvas->print(valBuf);
    y += lineH + 6;

    // closing phrase — noir sign-off
    static const char* const CLOSERS[] = {
        "another case in the files.",
        "the pig rests. for now.",
        "signals fade. pig endures.",
        "logs sealed. sleep tight.",
        "2.4ghz goes quiet.",
        "packets filed. lights out.",
        "case closed. pig out.",
        "void returns. pig persists.",
    };
    const char* closer = CLOSERS[esp_random() % 8];

    // inverted closer bar at bottom
    int closerY = SCREEN_HEIGHT - 22;
    canvas->fillRect(0, closerY, SCREEN_WIDTH, 22, fg);
    canvas->setTextColor(bg);
    canvas->setTextDatum(MC_DATUM);
    canvas->drawString(closer, SCREEN_WIDTH / 2, closerY + 11);
    canvas->setTextDatum(TL_DATUM);

    pushCanvas();
    delay(2500);
}

void toggleHuntOverlay() {
    overlayVisible = !overlayVisible;
}

bool isOverlayVisible() {
    return overlayVisible;
}

void nextPowerOption() {
    if (showingSleepWarning) return;
    do {
        powerOption = (powerOption + 1) % PowerPolicy::ACTION_COUNT;
    } while (!PowerPolicy::isAvailable(PowerPolicy::actionFromIndex(powerOption),
                                       HAMLET_HAS_TOUCH_SLEEP_WAKE));
}

void prevPowerOption() {
    if (showingSleepWarning) return;
    do {
        powerOption = (powerOption + PowerPolicy::ACTION_COUNT - 1) %
                      PowerPolicy::ACTION_COUNT;
    } while (!PowerPolicy::isAvailable(PowerPolicy::actionFromIndex(powerOption),
                                       HAMLET_HAS_TOUCH_SLEEP_WAKE));
}

int getPowerOption() {
    return powerOption;
}

void resetPowerMenu() {
    powerOption = PowerPolicy::actionIndex(PowerPolicy::SAFE_DEFAULT_ACTION);
    showingSleepWarning = false;
}

bool isShowingSleepWarning() {
    return showingSleepWarning;
}

void showSleepWarning() {
    showingSleepWarning = true;
}

void acceptSleepWarning() {
    showingSleepWarning = false;
    // sleep handled by hamlet.cpp handlePowerAction
}

void declineSleepWarning() {
    showingSleepWarning = false;
    // no action. back to power menu
}

// ==[ TH3 L0R3 ]== one sequential 0ct0 case-file transmission per entry
static uint32_t cachedLoreSequence = 0;
static const LoreStory::Fragment* cachedLoreFragment = nullptr;
static bool cachedLoreIsPigMemory = false;
static PancettaCat::Memory cachedPigMemory = PancettaCat::Memory::COUNT;

static void drawLoreBody(const char* text, int x, int y, int maxChars,
                         int maxLines, int lineHeight) {
    if (!text || maxChars < 2 || maxLines < 1) return;

    char line[56] = {0};
    int lineLen = 0;
    int lineCount = 0;
    const char* cursor = text;

    while (*cursor && lineCount < maxLines) {
        while (*cursor == ' ') cursor++;
        if (!*cursor) break;

        const char* word = cursor;
        int wordLen = 0;
        while (cursor[wordLen] && cursor[wordLen] != ' ') wordLen++;

        if (lineLen > 0 && lineLen + 1 + wordLen > maxChars) {
            canvas->drawString(line, x, y + lineCount * lineHeight);
            line[0] = '\0';
            lineLen = 0;
            lineCount++;
            if (lineCount >= maxLines) break;
        }

        if (lineLen > 0) line[lineLen++] = ' ';
        int copyLen = wordLen;
        if (copyLen > maxChars - lineLen) copyLen = maxChars - lineLen;
        memcpy(line + lineLen, word, copyLen);
        lineLen += copyLen;
        line[lineLen] = '\0';
        cursor += wordLen;
    }

    if (lineLen > 0 && lineCount < maxLines) {
        canvas->drawString(line, x, y + lineCount * lineHeight);
    }
}

void onAboutEnter() {
    const uint32_t observed = Config::getCatMemoryMask();
    const uint32_t seen = Config::getCatLoreSeenMask();
    cachedLoreIsPigMemory =
        LoreStory::firstUnreadMemory(observed, seen, cachedPigMemory);
    if (cachedLoreIsPigMemory) {
        cachedLoreFragment = &LoreStory::getMemory(cachedPigMemory);
        Config::setCatLoreSeenMask(
            seen | PancettaCat::memoryBit(cachedPigMemory));
    } else {
        cachedLoreSequence = Config::getLoreOpenCount();
        cachedLoreFragment = &LoreStory::get(cachedLoreSequence);
        Config::setLoreOpenCount(cachedLoreSequence + 1u);
        cachedPigMemory = PancettaCat::Memory::COUNT;
    }
    SFX::play(SFX::TRANSMISSION_BURST);
}

void drawAboutScreen() {
    canvas->fillSprite(getColorBG());
    drawStatusBar();

    const uint16_t fg = getColorFG();
    const uint16_t bg = getColorBG();
    const uint16_t dim = lerpColor565(fg, bg, 0.45f);
    const LoreStory::Fragment& fragment = cachedLoreFragment
        ? *cachedLoreFragment
        : LoreStory::get(0);

    canvas->setTextDatum(TL_DATUM);
    canvas->setTextSize(1);
    canvas->setTextColor(dim);
    canvas->drawString(fragment.stamp, 6, 18);

    char caseBuf[20];
    if (cachedLoreIsPigMemory) {
        snprintf(caseBuf, sizeof(caseBuf), "P1G %02u/%02u",
                 (unsigned)((uint8_t)cachedPigMemory + 1u),
                 (unsigned)LoreStory::memoryCount());
    } else {
        snprintf(caseBuf, sizeof(caseBuf), "CASE %02u/%02u",
                 (unsigned)(cachedLoreSequence % LoreStory::count() + 1u),
                 (unsigned)LoreStory::count());
    }
    canvas->setTextDatum(TR_DATUM);
    canvas->drawString(caseBuf, SCREEN_WIDTH - 6, 18);

    canvas->setTextDatum(TL_DATUM);
    canvas->setTextColor(fg);
    canvas->setTextSize(2);
    canvas->drawString(fragment.title, 6, 31);
    canvas->drawLine(6, 50, SCREEN_WIDTH - 6, 50, dim);

    // Full-size terminal copy. The content test guards this 25x8 viewport.
    canvas->setTextSize(2);
    drawLoreBody(fragment.body, 8, 55,
                 LoreStory::BODY_MAX_CHARS,
                 LoreStory::BODY_MAX_LINES, 17);

    canvas->drawLine(6, 193, SCREEN_WIDTH - 6, 193, dim);
    canvas->setTextDatum(TC_DATUM);
    canvas->setTextColor(fg);
    canvas->drawString("HAMLET PANCETTA // BY 0ct0", SCREEN_WIDTH / 2, 199);
    canvas->setTextColor(dim);
    char buildBuf[64];
    snprintf(buildBuf, sizeof(buildBuf), "V%s %s // 0CT0SEC/M5HAMLET",
             HAMLET_VERSION, BUILD_COMMIT);
    canvas->drawString(buildBuf, SCREEN_WIDTH / 2, 210);

    canvas->setTextDatum(TL_DATUM);
    canvas->setTextSize(1);
    canvas->setTextColor(fg);
    if (!drawHintBottomBarTo(canvas)) {
        drawBottomBar3("", "", "[C+] 3X1T");
    }
    pushCanvas();
}

void drawWebConfigScreen() {
    canvas->fillSprite(getColorBG());
    drawStatusBar();  // status bar owns "C0NF1G M0D3"

    if (!ConfigPortal::isRunning()) {
        canvas->setTextColor(getColorFG());
        canvas->setTextDatum(MC_DATUM);
        canvas->setTextSize(2);
        const char* err = ConfigPortal::getLastError();
        canvas->drawString((err && err[0]) ? err : "P0RT4L D0WN",
                           SCREEN_WIDTH / 2, SCREEN_HEIGHT / 2 - 8);
        canvas->setTextSize(1);
        canvas->drawString("[B] 3X1T. TRY 4G41N.",
                           SCREEN_WIDTH / 2, SCREEN_HEIGHT / 2 + 14);
        canvas->setTextDatum(TL_DATUM);
        if (!drawHintBottomBarTo(canvas)) drawBottomBarCentered("[B] 3X1T");
        pushCanvas();
        return;
    }

    canvas->setTextColor(getColorFG());
    canvas->setTextDatum(TC_DATUM);
    canvas->setTextSize(1);

    // compact layout — title gone, content starts just under status bar
    int y = TOP_BAR_H + 6;                             // 20
    canvas->drawString("CONNECT TO WIFI:", SCREEN_WIDTH / 2, y);
    y += 12;                                           // 32
    canvas->setTextSize(2);
    canvas->drawString(ConfigPortal::getSSID(), SCREEN_WIDTH / 2, y);
    y += 20;                                           // 52
    canvas->setTextSize(1);
    canvas->drawString("PASSWORD:", SCREEN_WIDTH / 2, y);
    y += 12;                                           // 64
    canvas->setTextSize(2);
    canvas->drawString(ConfigPortal::getPassword(), SCREEN_WIDTH / 2, y);
    y += 20;                                           // 84
    canvas->setTextSize(1);
    canvas->drawString("THEN OPEN BROWSER:", SCREEN_WIDTH / 2, y);
    y += 12;                                           // 96
    canvas->setTextSize(2);
    canvas->drawString(ConfigPortal::getIP(), SCREEN_WIDTH / 2, y);
    y += 22;                                           // 118

    // saved indicator
    if (ConfigPortal::credentialsSaved()) {
        canvas->setTextSize(1);
        canvas->setTextColor(getColorFG());
        canvas->drawString("* SAVED *", SCREEN_WIDTH / 2, y);
        y += 12;
    }

    // timeout countdown — user on slow cellular should know when portal auto-stops
    uint32_t remainingMs = ConfigPortal::getRemainingTime();
    if (remainingMs > 0) {
        char buf[20];
        uint32_t secs = remainingMs / 1000;
        snprintf(buf, sizeof(buf), "t-%lu:%02lu", (unsigned long)(secs / 60), (unsigned long)(secs % 60));
        canvas->setTextSize(1);
        canvas->drawString(buf, SCREEN_WIDTH / 2, y);
    }

    canvas->setTextDatum(TL_DATUM);  // reset
    if (!drawHintBottomBarTo(canvas)) {
        drawBottomBarCentered("[B] 3X1T");
    }
    pushCanvas();
}

void drawWardriveScreen() {
    const uint32_t now = millis();
    if (WardriveTelemetry::isVisible()) {
        WardriveTelemetry::draw(*canvas, now);
    } else {
        WardriveScene::drawScene(*canvas, now);
    }
    pushCanvas();
}

void drawBleScreen() {
    canvas->fillSprite(getColorBG());
    drawStatusBar();
    BleScanner::draw(*canvas);

    // bottom bar — A=left B=center C=right
    if (!drawHintBottomBarTo(canvas)) {
        if (BleScanner::isTracking()) {
            if (BleScanner::canTriggerSound()) {
                drawBottomBar3("[A]PING", "[B+]RECAL [B]STOP", "[C+]BACK");
            } else {
                drawBottomBar3("[A]GATT", "[B+]RECAL [B]STOP", "[C+]BACK");
            }
        } else if (BleScanner::getDeviceCount() > 0) {
            drawBottomBar3("[A]UP", "[B]TRACK", "[C]DN [C+]BACK");
        } else if (BleScanner::isRadioAvailable()) {
            drawBottomBar3(BleScanner::getScanModeHint(),
                           BleScanner::isRadioReady() ? "LISTEN" : "STARTING",
                           "[C+]BACK");
        } else {
            drawBottomBar3("", "BLE OFFLINE", "[C+]BACK");
        }
    }

    pushCanvas();
}

void drawDefhogScreen() {
    canvas->fillSprite(getColorBG());
    Defhog4::draw(*canvas);
    pushCanvas();
}

void drawXferScreen() {
    canvas->fillSprite(getColorBG());
    drawStatusBar();  // status bar owns "XF3R M0D3"

    canvas->setTextColor(getColorFG());
    canvas->setTextSize(1);

    if (!Xfer::isRunning()) {
        canvas->setTextDatum(TC_DATUM);
        canvas->drawString("AP START FAILED", SCREEN_WIDTH / 2, TOP_BAR_H + 70);
        canvas->drawString("BACK OUT. TRY XFER AGAIN.", SCREEN_WIDTH / 2, TOP_BAR_H + 88);
        canvas->setTextDatum(TL_DATUM);
        drawBottomBarCentered("[B] 3X1T");
        pushCanvas();
        return;
    }

    // AP info block — shifted up to reclaim title space
    int y = TOP_BAR_H + 10;
    const int LX = 16;

    canvas->setTextDatum(TL_DATUM);
    canvas->drawString("AP:", LX, y);
    canvas->setTextSize(2);
    canvas->drawString(Xfer::getSSID(), LX + 30, y - 4);
    canvas->setTextSize(1);
    y += 24;

    canvas->drawString("IP:", LX, y);
    canvas->drawString(Xfer::getIP(), LX + 30, y);
    y += 14;

    char pwBuf[16];
    snprintf(pwBuf, sizeof(pwBuf), "PW: %s", Xfer::getPassword());
    canvas->drawString(pwBuf, LX, y);
    y += 20;

    // Standard Wi-Fi QR: phone joins the XFER AP without typing the password.
    char qrPayload[96];
    if (WifiQR::buildPayload(qrPayload, sizeof(qrPayload),
                             Xfer::getSSID(), Xfer::getPassword(),
                             WIFI_AUTH_WPA2_PSK, false)) {
        constexpr uint8_t QR_SCALE = 3;
        int edge = WifiQR::edgeFor(qrPayload, QR_SCALE);
        if (edge > 0) {
            int qrX = SCREEN_WIDTH - edge - 14;
            int qrY = TOP_BAR_H + 10;
            WifiQR::draw(*canvas, qrX, qrY, getColorFG(), getColorBG(),
                         qrPayload, QR_SCALE);
            canvas->setTextDatum(TC_DATUM);
            canvas->drawString("SCAN TO JOIN", qrX + edge / 2, qrY + edge + 4);
            canvas->setTextDatum(TL_DATUM);
        }
    }

    // stats block
    char buf[40];
    uint8_t clients = Xfer::getClientCount();
    snprintf(buf, sizeof(buf), "CLIENTS: %u", clients);
    canvas->drawString(buf, LX, y);
    y += 14;

    uint32_t tx = Xfer::getTxBytes();
    uint32_t rx = Xfer::getRxBytes();
    if (tx < 10240) {
        snprintf(buf, sizeof(buf), "TX: %lu B", tx);
    } else {
        snprintf(buf, sizeof(buf), "TX: %lu KB", tx / 1024);
    }
    canvas->drawString(buf, LX, y);
    y += 14;

    if (rx < 10240) {
        snprintf(buf, sizeof(buf), "RX: %lu B", rx);
    } else {
        snprintf(buf, sizeof(buf), "RX: %lu KB", rx / 1024);
    }
    canvas->drawString(buf, LX, y);
    y += 14;

    snprintf(buf, sizeof(buf), "UL: %lu  DL: %lu",
             Xfer::getUploadCount(), Xfer::getDownloadCount());
    canvas->drawString(buf, LX, y);
    y += 18;

    // hint
    canvas->setTextDatum(TC_DATUM);
    canvas->drawString("http://192.168.4.1", SCREEN_WIDTH / 2, y);

    canvas->setTextDatum(TL_DATUM);  // reset
    if (!drawHintBottomBarTo(canvas)) {
        drawBottomBarCentered("[B] 3X1T");
    }
    pushCanvas();
}

void drawC5MonsterScreen() {
    canvas->fillSprite(getColorBG());
    C5Menu::draw(*canvas);
    pushCanvas();
}

// No fillSprite here: every path through MeshMenu::draw clears the sprite
// itself, and the composer's keyboard does its own. A second full-canvas fill
// is 153KB of PSRAM writes for nothing, every frame.
void drawMeshScreen() {
    MeshMenu::draw(*canvas);
    pushCanvas();
}

// ==[ DIRECT PROGRESS ]== Boot/setup callers use the panel before the shared
// canvas loop is available, so this deliberately writes straight to Display.

void showProgress(const char* msg, uint8_t pct) {
    M5.Display.fillScreen(getColorBG());
    M5.Display.setTextColor(getColorFG());
    M5.Display.setTextSize(1);
    
    int msgLen = strlen(msg) * 6;  // approx 6px per char
    M5.Display.setCursor((SCREEN_WIDTH - msgLen) / 2, 50);
    M5.Display.print(msg);
    
    int barX = 30;
    int barY = 72;
    int barW = SCREEN_WIDTH - 60;
    int barH = 10;
    
    M5.Display.drawRect(barX, barY, barW, barH, getColorFG());
    
    int fillW = (barW - 4) * pct / 100;
    if (fillW > 0) {
        M5.Display.fillRect(barX + 2, barY + 2, fillW, barH - 4, getColorFG());
    }
    
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", pct);
    int pctLen = strlen(buf) * 6;
    M5.Display.setCursor((SCREEN_WIDTH - pctLen) / 2, barY + barH + 8);
    M5.Display.print(buf);
}

// latch toast corner based on pig position (IDLE) or default upper-right
static void latchToastCorner() {
    if (Hamlet::getMode() == HamletMode::IDLE) {
        int pigCenter = Avatar::getCurrentX() + 36;
        quickToastCorner = (pigCenter < 160) ? 1 : 0;  // pig left → UR, pig right → UL
    } else {
        quickToastCorner = 1;  // upper-right default
    }
}

// How long the toast holds still. Reading time comes from the copy, clamped to
// 3-5s; a caller asking for longer (progress pings that outlive their own text)
// keeps its own window.
static uint32_t toastHoldFor(const ToastLayout& L, uint32_t requested) {
    uint32_t chars = 0;
    for (int i = 0; i < L.lineCount; i++) chars += (uint32_t)strlen(L.lines[i]);

    uint32_t hold = kToastHoldBase + chars * kToastHoldPerChar
                    + (uint32_t)(L.lineCount - 1) * kToastHoldPerLine;
    if (hold < kToastHoldMin) hold = kToastHoldMin;
    if (hold > kToastHoldMax) hold = kToastHoldMax;
    return (requested > hold) ? requested : hold;
}

static void clearQuickToast() {
    if (quickToastWasDrawnToDisplay) {
        quickToastNeedsClearRedraw = true;
        quickToastWasDrawnToDisplay = false;
    }
    quickToastMsg[0] = '\0';
    quickToastLifeMs = 0;
    quickToastHoldMs = 0;
    quickToastUrgent = 0;
    quickToastPrevVisible = false;
}

static void startQuickToast(const char* msg, uint32_t durationMs,
                            uint8_t flashCount, bool alert) {
    strncpy(quickToastMsg, msg, sizeof(quickToastMsg) - 1);
    quickToastMsg[sizeof(quickToastMsg) - 1] = '\0';
    quickToastIsAlert = alert;

    if (alert) {
        // pick random header, avoid repeat
        uint8_t idx;
        do { idx = esp_random() % ALERT_HEADER_COUNT; } while (idx == lastAlertHeader);
        lastAlertHeader = idx;
    }
    latchToastCorner();

    // stamp the header once — wall clock if the RTC has one, session clock otherwise
    uint8_t hh, mm;
    if (getCurrentTime(&hh, &mm)) {
        snprintf(quickToastClock, sizeof(quickToastClock), "%02u:%02u",
                 (unsigned)hh, (unsigned)mm);
    } else {
        uint32_t s = millis() / 1000;
        snprintf(quickToastClock, sizeof(quickToastClock), "%02u:%02u",
                 (unsigned)((s / 60) % 100), (unsigned)(s % 60));
    }

    // hold needs the wrapped line count, so lay the console out first
    ToastLayout L;
    computeToastLayout(L);

    quickToastUrgent = flashCount;
    quickToastHoldMs = toastHoldFor(L, durationMs);
    quickToastStart  = millis();
    quickToastLifeMs = (uint32_t)flashCount * (kToastUrgentOn + kToastUrgentOff)
                       + kToastFlickInMs + quickToastHoldMs + kToastFlickOutMs;
    quickToastPrevVisible = false;  // first frame counts as a flip
}

void showToast(const char* msg, uint32_t durationMs, uint8_t flashCount) {
    if (!msg || !msg[0]) {
        clearQuickToast();
        return;
    }
    startQuickToast(msg, durationMs, flashCount, false);
}

void showAlertToast(const char* msg, uint32_t durationMs, uint8_t flashCount) {
    if (!msg || !msg[0]) {
        clearQuickToast();
        return;
    }
    startQuickToast(msg, durationMs, flashCount, true);
}

// rib sacrifice splash (phase 3)
// pig cracks a rib for your laziness. guilt-driven fitness
void showRibSacrifice(uint8_t remaining) {
    uint16_t fg = getColorFG();
    uint16_t bg = getColorBG();
    uint8_t fullBright = Config::getBrightness() * 255 / 100;
    uint8_t halfBright = fullBright / 2;
    if (halfBright < 30) halfBright = 30;  // floor so screen isn't invisible
    
    // draw the message first so flash has something to show
    M5.Display.fillScreen(bg);
    M5.Display.setTextColor(fg);
    M5.Display.setTextDatum(MC_DATUM);
    
    M5.Display.setTextSize(2);
    M5.Display.drawString("RIB CRACKED", SCREEN_WIDTH / 2, 28);
    
    M5.Display.setTextSize(1);
    M5.Display.drawString("streak saved. barely.", SCREEN_WIDTH / 2, 55);
    
    char ribBuf[32];
    if (remaining == 0) {
        snprintf(ribBuf, sizeof(ribBuf), "last rib. no more safety net.");
    } else if (remaining == 1) {
        snprintf(ribBuf, sizeof(ribBuf), "1 rib left. walk more.");
    } else {
        snprintf(ribBuf, sizeof(ribBuf), "%d ribs left. dont waste em.", remaining);
    }
    M5.Display.drawString(ribBuf, SCREEN_WIDTH / 2, 75);
    
    M5.Display.drawString("one rib paid. streak lives.", SCREEN_WIDTH / 2, 95);
    
    // one button. pick poison
    M5.Display.drawString("(one button. pick your poison.)", SCREEN_WIDTH / 2, 118);
    
    // flash brightness 3x for drama
    for (int i = 0; i < 3; i++) {
        M5.Display.setBrightness(halfBright);
        delay(150);
        M5.Display.setBrightness(fullBright);
        delay(150);
    }
    
    // wait for button press (blocking)
    while (true) {
        M5.update();
        if (M5.BtnA.wasReleased()) break;
        delay(10);
    }
}

// ==[ SCREEN DIMMING ]== Brightness changes only; scene state keeps running.

void resetDimTimer() {
    lastActivityTime = millis();
    if (lowPowerDimmed) return;
    if (dimmed) {
        // restore full brightness
        dimmed = false;
        uint8_t brightness = Config::getBrightness();
        M5.Display.setBrightness(brightness * 255 / 100);
    }
}

void updateDimming() {
    if (lowPowerDimmed) {
        if (!dimmed) {
            dimmed = true;
            M5.Display.setBrightness(Config::getDimLevel() * 255 / 100);
        }
        return;
    }
    // A destructive confirmation must never become a blind wake-and-accept
    // target. Keep the modal visible until it is explicitly accepted/cancelled.
    if (showingSleepWarning) {
        wakeFromDim();
        return;
    }
    if (!Power::isDimmingAllowed()) {
        wakeFromDim();
        return;
    }
    uint16_t timeout = Config::getDimTimeout();
    if (timeout == 0) return;  // dimming disabled
    
    uint32_t elapsed = (millis() - lastActivityTime) / 1000;
    
    if (!dimmed && elapsed >= timeout) {
        // time to dim
        dimmed = true;
        uint8_t dimLevel = Config::getDimLevel();
        M5.Display.setBrightness(dimLevel * 255 / 100);
    }
}

bool isDimmed() {
    return dimmed;
}

void wakeFromDim() {
    if (lowPowerDimmed) return;
    if (dimmed) {
        dimmed = false;
        uint8_t brightness = Config::getBrightness();
        M5.Display.setBrightness(brightness * 255 / 100);
        lastActivityTime = millis();
    }
}


void setLowPowerDimmed(bool enabled) {
    if (lowPowerDimmed == enabled) return;
    lowPowerDimmed = enabled;
    dimmed = enabled;
    const uint8_t level = enabled ? Config::getDimLevel() : Config::getBrightness();
    M5.Display.setBrightness(level * 255 / 100);
    lastActivityTime = millis();
}

bool isLowPowerDimmed() {
    return lowPowerDimmed;
}

void drawParanoiaOverlayTo(M5Canvas* targetCanvas) {
    drawParanoiaToastTo(targetCanvas);
}

void drawQuickToastTo(M5Canvas* targetCanvas) {
    if (!targetCanvas) return;
    // XP notif strip on external canvas (wardrive, menu) — ticks state for these render paths
    if (drawXPNotifToCanvas(targetCanvas)) {
        xpNotifOnCanvas = true;
    }
    if (!isQuickToastActive()) return;
    drawQuickToastToCanvas(targetCanvas);
    // signal overlay path: toast already on canvas, don't double-draw to M5.Display
    quickToastDrawnOnCanvasFrame = true;
    quickToastWasDrawnToDisplay = false;
}

void drawHelpOverlayTo(M5Canvas* targetCanvas) {
    if (!targetCanvas || !helpOverlayActive) return;
    drawHelpOverlayToCanvas(targetCanvas);
    helpOverlayDrawnOnCanvasFrame = true;
}

void drawUiOverlaysTo(M5Canvas* targetCanvas) {
    if (!targetCanvas) return;
    if (drawXPNotifToCanvas(targetCanvas)) {
        xpNotifOnCanvas = true;
    }
    drawItemDropToCanvas(targetCanvas);
    drawParanoiaOverlayTo(targetCanvas);
    drawHelpOverlayTo(targetCanvas);
    if (isQuickToastActive()) {
        drawQuickToastToCanvas(targetCanvas);
        quickToastDrawnOnCanvasFrame = true;
        quickToastWasDrawnToDisplay = false;
    }
}

bool hasActiveQuickToast() {
    return isQuickToastActive();
}

bool getActiveToastRect(int16_t& x, int16_t& y, int16_t& w, int16_t& h) {
    if (!isQuickToastActive()) return false;
    x = quickToastX;
    y = quickToastY;
    w = quickToastW;
    h = quickToastH;
    return true;
}

// ==[ TOP BAR MESSAGE ]== temporary status override
void setTopBarMessage(const char* msg, uint32_t durationMs) {
    if (!msg || !msg[0]) { topBarMsg[0] = '\0'; return; }
    strncpy(topBarMsg, msg, sizeof(topBarMsg) - 1);
    topBarMsg[sizeof(topBarMsg) - 1] = '\0';
    topBarMsgStart = millis();
    topBarMsgDuration = durationMs;
}

void clearTopBarMessage() { topBarMsg[0] = '\0'; }

// ==[ EFFECT EVALUATION ]== derives active buffs/debuffs from game state
static void evaluateEffects() {
    effectCount = 0;
    int mom = Mood::getMomentum();

    // momentum buff/debuff (maps to effectiveness multiplier ranges)
    if (mom > 50 && effectCount < 4) {
        snprintf(activeEffects[effectCount].label, 20, "SNOUT +30%%");
        activeEffects[effectCount++].positive = true;
    } else if (mom > 20 && effectCount < 4) {
        snprintf(activeEffects[effectCount].label, 20, "SNOUT +15%%");
        activeEffects[effectCount++].positive = true;
    } else if (mom < -30 && effectCount < 4) {
        snprintf(activeEffects[effectCount].label, 20, "SNOUT -20%%");
        activeEffects[effectCount++].positive = false;
    } else if (mom < -10 && effectCount < 4) {
        snprintf(activeEffects[effectCount].label, 20, "SNOUT -10%%");
        activeEffects[effectCount++].positive = false;
    }

    // streak buff
    uint16_t streak = Config::getStreak();
    if (streak >= 3 && effectCount < 4) {
        snprintf(activeEffects[effectCount].label, 20, "STREAK x%d", streak);
        activeEffects[effectCount++].positive = true;
    }

    // goal progress buff
    uint8_t goalProg = Config::getGoalProgress();
    if (goalProg >= 100 && effectCount < 4) {
        snprintf(activeEffects[effectCount].label, 20, "GOAL +10%%");
        activeEffects[effectCount++].positive = true;
    } else if (goalProg >= 80 && effectCount < 4) {
        snprintf(activeEffects[effectCount].label, 20, "GOAL +5%%");
        activeEffects[effectCount++].positive = true;
    }

    // level baseline (only notable extremes)
    uint8_t level = Config::getLevel();
    if (level >= 35 && effectCount < 4) {
        snprintf(activeEffects[effectCount].label, 20, "RANK +20%%");
        activeEffects[effectCount++].positive = true;
    } else if (level < 7 && effectCount < 4) {
        snprintf(activeEffects[effectCount].label, 20, "RANK -10%%");
        activeEffects[effectCount++].positive = false;
    }
}

// ==[ EFFECT TOAST DRAW ]== inverted bar takeover during show phase
static void drawEffectToast(M5Canvas* c, bool transparent) {
    HamletMode mode = Hamlet::getMode();
    if (mode != HamletMode::IDLE && mode != HamletMode::HUNT) return;
    if (topBarMsg[0] != '\0') return;  // top bar message takes priority
    if (effectCount == 0) return;

    uint32_t now = millis();
    if (now - lastEffectEval >= 1000) {
        evaluateEffects();
        lastEffectEval = now;
    }

    static uint8_t prevEffectCount = 0;
    if (effectCount != prevEffectCount) {
        effectCycleStart = now;
        prevEffectCount = effectCount;
    }

    uint32_t totalCycle = (uint32_t)effectCount * EFFECT_SHOW_MS + EFFECT_PAUSE_MS;
    uint32_t cyclePos = (now - effectCycleStart) % totalCycle;

    // in pause period? don't show
    if (cyclePos >= (uint32_t)effectCount * EFFECT_SHOW_MS) return;

    uint8_t showIdx = (uint8_t)(cyclePos / EFFECT_SHOW_MS);
    if (showIdx >= effectCount) return;

    const char* pfx = activeEffects[showIdx].positive ? "[+]" : "[-]";
    char buf[28];
    snprintf(buf, sizeof(buf), "%s %s", pfx, activeEffects[showIdx].label);

    // Filled takeover on classic bars, glyph-only on sun-composited transparent bars.
    if (!transparent) {
        c->fillRect(0, 0, SCREEN_WIDTH, TOP_BAR_H, getColorFG());
        c->setTextColor(getColorBG());
    } else {
        c->setTextColor(getColorFG());
    }
    c->setTextSize(1);
    c->setTextDatum(MC_DATUM);
    c->drawString(buf, SCREEN_WIDTH / 2, TOP_BAR_H / 2);
    c->setTextDatum(TL_DATUM);
}

static uint32_t fnv1a32(const uint8_t* data, size_t len, uint32_t hash) {
    for (size_t i = 0; i < len; ++i) {
        hash ^= data[i];
        hash *= 16777619u;
    }
    return hash;
}

bool dumpScreenshotToSerial() {
    if (!Serial) return false;

    // Screenshot dumping is a serial-only diagnostic on the 16 KiB Arduino
    // loop stack. Do not reserve another 640 bytes of scarce Core2 DRAM for a
    // command that is normally asleep.
    uint16_t rowBuf[SCREEN_WIDTH];
    constexpr uint32_t rowBytes = SCREEN_WIDTH * sizeof(uint16_t);
    constexpr uint32_t totalBytes = SCREEN_WIDTH * SCREEN_HEIGHT * sizeof(uint16_t);
    uint32_t crc = 2166136261u;

    Serial.printf("[[HAMLET_SCREENSHOT_BEGIN]] RGB565LE %d %d %lu\n",
                  SCREEN_WIDTH, SCREEN_HEIGHT, (unsigned long)totalBytes);
    Serial.flush();

    for (int y = 0; y < SCREEN_HEIGHT; ++y) {
        M5.Display.readRect(0, y, SCREEN_WIDTH, 1, rowBuf);
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(rowBuf);
        crc = fnv1a32(bytes, rowBytes, crc);
        Serial.write(bytes, rowBytes);
        if ((y & 7) == 7) delay(0);
    }

    Serial.printf("\n[[HAMLET_SCREENSHOT_END]] %08lX\n", (unsigned long)crc);
    Serial.flush();
    return true;
}

} // namespace Display
