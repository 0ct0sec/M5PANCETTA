/**
 * Hamlet - Core Implementation
 *
 * ==[ CASE DESK ]== one run loop owns mode changes, radio custody, input,
 * frame cadence, and mood plumbing. Subsystems bring evidence here; none gets
 * to invent a second clock or leave with the radio keys.
 */

#include "hamlet.h"
#include "hamlet_session.h"
#include <M5Unified.h>
#include "core/config.h"
#include "core/achievements.h"
#include "core/challenges.h"
#include "core/item_drops.h"
#include "core/capture.h"
#include "core/power.h"
#include "core/power_policy.h"
#include "core/frame_budget.h"
#include "core/gps.h"
#include "core/radio_policy.h"
#include "core/wsl_bypasser.h"
#include "hal/sd_storage.h"
#include "hal/platform.h"
#include "modes/hunt.h"
#include "modes/spectrum.h"
#include "ui/display.h"
#include "ui/ui_measurements.h"
#include "ui/frame_presenter.h"
#include "ui/scene_cache.h"
#include "ui/menu.h"
#include "ui/menu_pig.h"
#include "ui/teleport.h"
#include "ui/loot_menu.h"
#include "ui/feeding_menu.h"
#include "ui/mail_menu.h"
#include "core/mailbox.h"
#include "core/item_effects.h"
#include "core/bounty.h"
#include "ui/settings_menu.h"
#include "sync/nowflock_transport.h"
#include "activity/pedometer.h"
#include "audio/sfx.h"
#include "audio/noir_jazz.h"
#include "audio/bath_mic.h"
#include "haptic/haptic.h"
#include "input/touch.h"
#include "input/touch_hints.h"
#include "input/cardkb.h"
#include "ui/soft_keyboard.h"
#include "piglet/mood.h"
#include "piglet/avatar.h"
#include "piglet/weather.h"
#include "net/config_portal.h"
#include "net/xfer_server.h"
#include "net/wifi_client.h"
#include "modes/wardrive.h"
#include "modes/wardrive_scene.h"
#include "modes/wardrive_telemetry.h"
#include "modes/ble_scanner.h"
#include "modes/defhog4.h"
#include "defense/recon.h"
#include "defense/defense_pipeline.h"
#include "ui/c5monster_menu.h"
#include "radio/c5monster_uart.h"
#include "ui/mesh_menu.h"
#include "ui/mesh_notification.h"
#include "radio/meshtastic_uart.h"
#include "defense/noir_narrator.h"
#include "defense/potfile.h"
#include "ui/defhog_terminal.h"
#include "ui/ui_measurements.h"
#include "ui/npc/barman.h"
#include "led/ambient_led.h"
#include "util/debug_log.h"
#include "util/time_math.h"
#include <WiFi.h>
#include <math.h>
#include <atomic>

namespace Hamlet {

// ==[ MODE TRACKING ]==
static HamletMode currentMode = HamletMode::IDLE;

// Phase 3: 4-minute IDLE timeout for critical trigger
static uint32_t idleStartTime = 0;
static bool idleTimerChecked = false;  // prevents spam checks
static uint32_t lastModeChangeTime = 0;

// ==[ BUTTON STATE ]==
// Physical layout: BtnA(left)=prev, BtnB(mid)=OK/select, BtnC(right)=next+back
struct ButtonState {
    uint32_t okDownTime = 0;         // middle button (OK/select)
    uint32_t backDownTime = 0;       // right button (next/back)
    bool okHandled = false;
    bool backHandled = false;
    float holdProgress = 0.0f;       // button hold overlay progress
    float aHoldProgress = 0.0f;      // A-only hold progress (portal charge source)
    float aRawHoldProgress = 0.0f;   // pre-delay A-hold progress
    float gestureHoldProgress = 0.0f;
};
static ButtonState btn;
static uint32_t frameNow = 0;
static uint32_t powerWakeInputBlockUntil = 0;

// ==[ PRESS WINDOWS ]==
static const uint32_t LONG_PRESS_MS = 2000;
static const uint32_t PORTAL_CHARGE_DELAY_MS = 300;
static const uint32_t BACK_PRESS_MS = 1000;
static const uint32_t GESTURE_LONG_PRESS_MS = 1000;
static const uint32_t GESTURE_SUPER_LONG_PRESS_MS = 5000;

static constexpr uint32_t SCREEN_LOCK_HINT_TOAST_MS = 900;
static constexpr uint32_t SCREEN_LOCK_HINT_COOLDOWN_MS = 1400;
static constexpr uint32_t SCREEN_UNLOCK_TOAST_MS = 900;
static constexpr uint32_t SCREEN_LOCK_TOAST_MS = 1200;
static constexpr int16_t SCREEN_UNLOCK_MIN_UP_DY = 28;
static constexpr const char* SCREEN_LOCK_HINT_TEXT = "swipe up";
static constexpr const char* SCREEN_UNLOCK_TEXT = "SCREEN UNLOCKED";
static constexpr const char* SCREEN_LOCK_TEXT = "SCREEN LOCKED // SWIPE UP";
static bool screenTouchLocked = false;
static uint32_t screenTouchLockHintMs = 0;

static const int16_t PORTAL_CENTER_X = UIMeasurements::kScreenWidth / 2;
static const int16_t PORTAL_CENTER_Y = (UIMeasurements::kTopBarH + UIMeasurements::kScreenHeight - UIMeasurements::kBottomBarH) / 2;

static inline float syncHoldOverlayProgress() {
    float p = (btn.holdProgress > btn.gestureHoldProgress) ? btn.holdProgress : btn.gestureHoldProgress;
    if (p > 1.0f) p = 1.0f;
    Display::setHoldProgress(p);
    return p;
}

static inline uint32_t transitionStartNow() {
    return (frameNow != 0) ? frameNow : millis();
}

static Teleport::PigSilhouette avatarTeleportSilhouette() {
    if (Avatar::facingAway) return Teleport::PigSilhouette::REAR;
    return Avatar::isFacingRight() ? Teleport::PigSilhouette::SIDE_RIGHT
                                   : Teleport::PigSilhouette::SIDE_LEFT;
}

static void startWardriveExitTeleport() {
    if (Teleport::isActive()) {
        return;
    }

    // The exit effect is authored against the cockpit pig. Hand the canvas
    // back before starting it so leaving from the sensor tape does not hide
    // the entire decomposition behind a static instrument page.
    WardriveTelemetry::setVisible(false);

    int16_t srcCX, srcCY;
    WardriveScene::getCockpitPigCenter(srcCX, srcCY);

    float dstCX, dstCY;
    bool dstRear = false;
    bool dstFaceRight = true;
    MenuPig::prepareWardriveExitStation(dstCX, dstCY, dstRear, dstFaceRight);
    Teleport::PigSilhouette destination = dstRear
        ? Teleport::PigSilhouette::REAR
        : (dstFaceRight ? Teleport::PigSilhouette::SIDE_RIGHT
                        : Teleport::PigSilhouette::SIDE_LEFT);

    Teleport::startCrossMode(Teleport::Context::WARDRIVE_TO_MENU,
        (float)srcCX, (float)srcCY,
        160.0f, 80.0f,
        dstCX, dstCY, transitionStartNow(),
        Teleport::PigSilhouette::REAR, destination);

    HAMLET_LOGF("[WD-TP] started ctx=%d phase=%d\n",
        (int)Teleport::getContext(), (int)Teleport::getPhase());
}

// ==[ BOOT TICK ]==
static uint32_t bootTime = 0;
static constexpr uint8_t kBootStageTotal = 13;

// ==[ SESSION STATE ]== pure timing/reward bookkeeping shared with host tests
static HamletSession::State session;
// ==[ PARANOIA MODE STATE ]==
// deauth* fields written by triggerGlobalDeauth() from Hunt/Spectrum callbacks (WiFi task, core 0)
// read by updateParanoia() in main loop (core 1) — std::atomic required for cross-core visibility
struct ParanoiaState {
    std::atomic<bool> deauthDetected{false};
    std::atomic<int8_t> deauthRSSI{-100};
    std::atomic<uint8_t> deauthChannel{0};
    uint32_t toastStart = 0;               // toast birth timestamp
    bool toastActive = false;              // toast on-screen flag
    bool morsePlayedThisSession = false;   // Morse only once per boot
    bool huntPromptToastActive = false;    // true only while OUR hunt-prompt toast is live
};
static ParanoiaState paranoia;
static const uint32_t PARANOIA_TOAST_TIMEOUT = 5000;  // 5s auto-dismiss
// ==[ SERIAL DEBUG ]== minimal text commands for screenshot capture
static bool serialScreenshotPending = false;
static char serialCommandBuf[32] = {0};
static uint8_t serialCommandLen = 0;
// ==[ FORWARD DECLS ]==
static void updateButtons(uint32_t now);
static void updateKeypad(uint32_t now);
static void announceKeypadHotplug();
static void updateTouch(uint32_t now);
static void handlePowerAction(bool confirmed = false);
static void updateParanoia(uint32_t now);
static void processReconEvents(uint32_t now);
static void updateGestures(uint32_t now);
static void announceMeshArrival();
static void pollSerialCommands();
static bool isScreenTouchLocked();
static void syncScreenTouchLockEffects(HamletMode mode);
static void setScreenTouchLock(bool locked);
static void clearScreenTouchLock();
static void executeSelectedC5SpectrumAction();

static bool isPowerWakeInputBlocked(uint32_t now) {
    return static_cast<int32_t>(powerWakeInputBlockUntil - now) > 0;
}

static void suspendRadiosForPowerTransition() {
    NowFlock::releaseRadio();
    DefensePipeline::requestOperatingState(Defense::OperatingState::SUSPENDED_RELEASE_BLE);
    WiFi.scanDelete();
    WiFi.mode(WIFI_OFF);
}

static void resumeRadiosAfterPowerTransition() {
    DefensePipeline::requestOperatingState(Defense::OperatingState::BACKGROUND);
    // Wi-Fi power settings are restored at the actual driver-start seam in
    // NowFlock. The pipeline resume path only brings the BLE owner back here.
}

// ==[ MODE DISPATCH TABLE ]== collapse enter/exit/update/draw switches
struct ModeDescriptor {
    void (*enter)();     // nullable — called on enterMode
    void (*exit)();      // nullable — called on exitCurrentMode
    void (*update)();    // nullable — called every frame
    void (*draw)();      // nullable — called every frame after update
};

static constexpr int MODE_COUNT = HAMLET_MODE_COUNT;

static const ModeDescriptor MODE_TABLE[] = {
    /* IDLE           */ { nullptr,              nullptr,              nullptr,              Display::drawIdleScreen     },
    /* MENU           */ { nullptr,              nullptr,              Menu::update,         nullptr                     },
    /* HUNT           */ { Hunt::start,          Hunt::stop,           Hunt::update,         Display::drawHuntScreen     },
    /* SPECTRUM       */ { Spectrum::start,      Spectrum::stop,       Spectrum::update,     Display::drawSpectrumScreen },
    /* LOOT           */ { LootMenu::enter,      LootMenu::exit,       LootMenu::update,     nullptr                     },
    /* FEEDING        */ { FeedingMenu::enter,   nullptr,              FeedingMenu::update,  nullptr                     },
    /* WALK_STATS     */ { nullptr,              nullptr,              Pedometer::update,    Display::drawWalkStats      },
    /* SETTINGS       */ { SettingsMenu::enter,  SettingsMenu::exit,   SettingsMenu::update, nullptr                     },
    /* NOWFLOCK       */ { Display::flockOnEnter, NowFlock::stopSync,   NowFlock::update,     Display::drawFlockScreen    },
    /* POWER_MENU     */ { nullptr,              nullptr,              nullptr,              Display::drawPowerMenu      },
    /* ABOUT          */ { Display::onAboutEnter,nullptr,              nullptr,              Display::drawAboutScreen    },
    /* WEBCONFIG      */ { ConfigPortal::start,  ConfigPortal::stop,   ConfigPortal::update, Display::drawWebConfigScreen},
    /* WARDRIVE       */ { Wardrive::start,      Wardrive::stop,       Wardrive::update,     Display::drawWardriveScreen },
    /* BLE_SCANNER    */ { BleScanner::start,    BleScanner::stop,     BleScanner::update,   Display::drawBleScreen      },
    /* DEFHOG4        */ { Defhog4::enter,       Defhog4::exit,        Defhog4::update,      Display::drawDefhogScreen   },
    /* XFER           */ { Xfer::start,          Xfer::stop,           Xfer::update,         Display::drawXferScreen     },
    /* C5MONSTER      */ { C5Menu::enter,        C5Menu::exit,         C5Menu::update,       Display::drawC5MonsterScreen},
    /* MAIL           */ { MailMenu::enter,      nullptr,              MailMenu::update,     nullptr                     },
    /* MESH           */ { MeshMenu::enter,      MeshMenu::exit,       MeshMenu::update,     Display::drawMeshScreen     },
};
static_assert(sizeof(MODE_TABLE) / sizeof(MODE_TABLE[0]) == MODE_COUNT, "MODE_TABLE/HamletMode enum mismatch");

static bool equalsIgnoreCase(const char* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *b) {
        char ca = *a++;
        char cb = *b++;
        if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 'a' + 'A');
        if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 'a' + 'A');
        if (ca != cb) return false;
    }
    return (*a == '\0' && *b == '\0');
}

static bool startsWithIgnoreCase(const char* text, const char* prefix) {
    if (!text || !prefix) return false;
    while (*prefix) {
        char ct = *text++;
        char cp = *prefix++;
        if (ct >= 'a' && ct <= 'z') ct = (char)(ct - 'a' + 'A');
        if (cp >= 'a' && cp <= 'z') cp = (char)(cp - 'a' + 'A');
        if (ct != cp) return false;
    }
    return true;
}

static const char* skipSerialSpaces(const char* s) {
    while (s && (*s == ' ' || *s == '\t')) ++s;
    return s;
}

static void handleSerialCommand(const char* cmd) {
    if (!cmd || !cmd[0]) return;

    const char* trimmed = skipSerialSpaces(cmd);
    if (!trimmed[0]) return;

    if (startsWithIgnoreCase(trimmed, "FRAMESTATS")) {
#ifdef HAMLET_FRAME_PROFILE
        const char* arg = skipSerialSpaces(trimmed + 10);
        if (equalsIgnoreCase(arg, "OFF")) {
            FrameBudget::setProfiling(false);
            Serial.println("[FRAMESTATS] off");
        } else if (equalsIgnoreCase(arg, "RESET")) {
            FrameBudget::resetStats();
            FrameBudget::setProfiling(true);
            Serial.println("[FRAMESTATS] reset; profiling on");
        } else {
            if (!FrameBudget::isProfiling()) FrameBudget::resetStats();
            FrameBudget::setProfiling(true);
            FrameBudget::printStats();
        }
#else
        Serial.printf("[FRAMESTATS] last=%lums (build with -DHAMLET_FRAME_PROFILE for detail)\n",
                      (unsigned long)FrameBudget::lastFrameMs());
#endif
        return;
    }

    if (startsWithIgnoreCase(trimmed, "ROOMSTATS")) {
#ifdef HAMLET_FRAME_PROFILE
        const char* arg = skipSerialSpaces(trimmed + 9);
        if (equalsIgnoreCase(arg, "RESET")) {
            FrameBudget::resetStats();
            FrameBudget::setProfiling(true);
            Serial.println("[ROOMSTATS] reset; profiling on");
        } else {
            FrameBudget::printRoomStats();
        }
#else
        Serial.println("[ROOMSTATS] build m5stack-core2-profile for room telemetry");
#endif
        return;
    }

    if (startsWithIgnoreCase(trimmed, "PRESENTSTATS")) {
        const char* arg = skipSerialSpaces(trimmed + 12);
        if (equalsIgnoreCase(arg, "RESET")) {
            FramePresenter::resetStats();
            SceneCache::resetStats();
            Serial.println("[PRESENTSTATS] reset");
        } else {
            FramePresenter::printStats();
            SceneCache::printStats();
        }
        return;
    }

    if (startsWithIgnoreCase(trimmed, "FRAME")) {
        const char* arg = skipSerialSpaces(trimmed + 5);
        if (!arg[0] || equalsIgnoreCase(arg, "OFF") || equalsIgnoreCase(arg, "CLEAR")) {
            MenuPig::clearDebugRoamingFrame();
            if (currentMode == HamletMode::MENU) {
                Menu::enter();
            }
            Serial.println("[FRAME] cleared");
            return;
        }
        if (Menu::hasActiveEncounter()) Menu::dismissEncounter();
        if (!MenuPig::forceDebugRoamingFrame(arg)) {
            Serial.printf("[FRAME] unknown: %s\n", arg);
            return;
        }
        enterMode(HamletMode::MENU);
        Menu::enterRoaming();
        // MenuPig::enter() resets runtime pose state. Re-apply the validated
        // preset after mode entry so the requested room survives that reset.
        MenuPig::forceDebugRoamingFrame(arg);
        Serial.printf("[FRAME] forced %s\n", arg);
        return;
    }

    if (startsWithIgnoreCase(trimmed, "RFTRACE")) {
        const char* arg = skipSerialSpaces(trimmed + 7);
        Spectrum::printRfTrace(equalsIgnoreCase(arg, "CLEAR"));
        return;
    }

    if (startsWithIgnoreCase(trimmed, "GPS")) {
        const char* arg = skipSerialSpaces(trimmed + 3);
        if (equalsIgnoreCase(arg, "RESET")) {
            GPS::resetDiagnostics();
            Serial.println("[GPS] counters reset");
            return;
        }

        GPS::Diagnostics d;
        GPS::getDiagnostics(d);
        Serial.printf("[GPS] cfg en=%d alwaysOn=%d uart=%s rx=G%u/D%u tx=G%u/D%u "
                      "baud=%lu rxbuf=%lu mbus5v=%d\n",
                      d.enabled ? 1 : 0, d.alwaysOn ? 1 : 0,
                      d.initialized ? "up" : "down",
                      (unsigned)d.rxPin, (unsigned)d.rxDip,
                      (unsigned)d.txPin, (unsigned)d.txDip,
                      (unsigned long)d.baud, (unsigned long)d.rxBufferBytes,
                      M5.Power.getExtOutput() ? 1 : 0);
        Serial.printf("[GPS] feed bytes=%lu chars=%lu pass=%lu fail=%lu fixSent=%lu "
                      "peakPend=%u capHits=%lu\n",
                      (unsigned long)d.bytesDrained, (unsigned long)d.charsProcessed,
                      (unsigned long)d.passedChecksum, (unsigned long)d.failedChecksum,
                      (unsigned long)d.sentencesWithFix,
                      (unsigned)d.maxPendingBytes, (unsigned long)d.drainCapHits);
        Serial.printf("[GPS] state raw=%d(%lums) nmea=%d(%lums) fix=%d sats=%u "
                      "age=%lums hdop=%.1f lat=%.5f lon=%.5f\n",
                      d.rawFresh ? 1 : 0, (unsigned long)d.lastRawAgeMs,
                      d.nmeaFresh ? 1 : 0, (unsigned long)d.lastNmeaAgeMs,
                      d.fix ? 1 : 0, (unsigned)d.sats,
                      (unsigned long)d.fixAgeMs, d.hdop, d.lat, d.lon);

        char tail[128];
        if (GPS::getRawTail(tail, sizeof(tail)) > 0) {
            Serial.printf("[GPS] tail |%s|\n", tail);
        }

        // Name the fault instead of leaving the operator to read counters.
        // Backlog near the ring buffer is the early warning; capHits is the
        // confirmed overflow. Either one shreds sentences and blocks the fix.
        const bool starved = d.drainCapHits > 0 ||
                             (d.rxBufferBytes > 0 &&
                              d.maxPendingBytes >= (d.rxBufferBytes * 3u) / 4u);
        // A healthy feed fails the odd sentence on power-up, not one in four.
        const bool shredded = d.failedChecksum > 4 &&
                              d.failedChecksum > d.passedChecksum / 4;
        const char* verdict;
        if (!d.enabled)                   verdict = "GPS disabled in settings";
        else if (!d.initialized)          verdict = "UART down — enter wardrive or set always-on";
        else if (d.charsProcessed == 0)   verdict = "no bytes — wrong RX pin/DIP, or module unpowered";
        else if (d.passedChecksum == 0)   verdict = "bytes but no valid sentence — wrong baud";
        else if (starved)                 verdict = "loop starved the UART — sentences dropped, raise headroom";
        else if (shredded)                verdict = "high checksum failure rate — marginal baud or wiring";
        else if (d.sentencesWithFix == 0) verdict = "NMEA parses, no fix yet — antenna/sky or cold start";
        else if (!d.fix)                  verdict = "had a fix, lost it — check sats and antenna";
        else                              verdict = "locked";
        Serial.printf("[GPS] verdict: %s\n", verdict);
        return;
    }

    if (equalsIgnoreCase(trimmed, "FTM")) {
        Spectrum::FtmRangeEvidence evidence{};
        const bool started =
            currentMode == HamletMode::SPECTRUM &&
            Spectrum::startSelectedFtmRange();
        Spectrum::getFtmRangeEvidence(evidence);
        if (started) {
            Serial.println("[FTM] explicit range request started");
        } else if (!evidence.responderAdvertised) {
            Serial.println("[FTM] unavailable: selected AP does not advertise responder support");
        } else if (evidence.active) {
            Serial.println("[FTM] request already active");
        } else {
            Serial.println("[FTM] unavailable: enter native WiFi client detail on CoreS3 SE");
        }
        return;
    }

    if (equalsIgnoreCase(trimmed, "IMU")) {
        static constexpr uint8_t MPU6886_ADDR = 0x68;
        static constexpr uint8_t WHO_AM_I_REG = 0x75;
        static constexpr uint32_t IMU_I2C_FREQ = 400000;
        const bool ack = M5.In_I2C.scanID(MPU6886_ADDR, IMU_I2C_FREQ);
        uint8_t whoAmI = 0;
        const bool whoRead = ack &&
            M5.In_I2C.readRegister(MPU6886_ADDR, WHO_AM_I_REG, &whoAmI,
                                   sizeof(whoAmI), IMU_I2C_FREQ);
        float ax, ay, az, gx, gy, gz;
        Pedometer::getCachedAccel(ax, ay, az);
        Pedometer::getCachedGyro(gx, gy, gz);
        Serial.printf("[IMU] enabled=%d type=%d bus=In_I2C ack68=%d who=%s0x%02X\n",
                      M5.Imu.isEnabled() ? 1 : 0, (int)M5.Imu.getType(),
                      ack ? 1 : 0, whoRead ? "" : "read-failed/",
                      (unsigned)whoAmI);
        Serial.printf("[IMU] accel=%.3f,%.3f,%.3f gyro=%.2f,%.2f,%.2f flat=%d\n",
                      ax, ay, az, gx, gy, gz,
                      Pedometer::isCachedFlat() ? 1 : 0);
        return;
    }

    if (startsWithIgnoreCase(trimmed, "WD")) {
        const char* arg = skipSerialSpaces(trimmed + 2);
        if (equalsIgnoreCase(arg, "START")) {
            enterMode(HamletMode::WARDRIVE);
            Serial.println("[WD] mode started");
            return;
        }
        if (equalsIgnoreCase(arg, "STOP")) {
            if (currentMode == HamletMode::WARDRIVE) {
                enterMode(HamletMode::IDLE);
            }
            Serial.println("[WD] mode stopped");
            return;
        }
        Serial.printf("[WD] active=%d paused=%d scans=%u nets=%lu "
                      "sd=%d free=%.2fGB c5=%d 5g=%lu c5fix=%d gpsfix=%d\n",
                      Wardrive::isActive() ? 1 : 0,
                      Wardrive::isPaused() ? 1 : 0,
                      (unsigned)Wardrive::getSessionScanCycles(),
                      (unsigned long)Wardrive::getSessionNewNets(),
                      Wardrive::isSDReady() ? 1 : 0,
                      Wardrive::getSDFreeGB(),
                      Wardrive::isDualBandActive() ? 1 : 0,
                      (unsigned long)Wardrive::getSession5GHzNetworks(),
                      Wardrive::isUsingFreshC5WardriveCoords() ? 1 : 0,
                      Wardrive::hasGPSFix() ? 1 : 0);
        return;
    }

    if (equalsIgnoreCase(trimmed, "SD")) {
        Serial.printf("[SD] available=%d free=%llu stream=%d\n",
                      SDStorage::isAvailable() ? 1 : 0,
                      (unsigned long long)SDStorage::freeBytes(),
                      SDStorage::isWriteStreamOpen() ? 1 : 0);
        return;
    }

    // ==[ C5 BRIDGE ]== raw passthrough to the C5Monster UART
    if (startsWithIgnoreCase(trimmed, "C5")) {
        const char* arg = skipSerialSpaces(trimmed + 2);
        if (!arg[0]) {
            Serial.printf("[C5] status=%d connected=%d\n",
                          (int)C5Monster::getStatus(), C5Monster::isConnected() ? 1 : 0);
            const C5Monster::OutputLog& log = C5Monster::getOutputLog();
            for (uint8_t i = 0; i < log.count; i++) {
                uint8_t idx = (log.head + i) % C5Monster::OUTPUT_LOG_LINES;
                Serial.printf("[C5] >> %s\n", log.lines[idx]);
            }
            return;
        }
        if (equalsIgnoreCase(arg, "OFF") ||
            equalsIgnoreCase(arg, "DISABLE")) {
            Config::setC5Enabled(false);
            C5Monster::emergencyStop();
            C5Monster::stop();
            if (Config::getGPSEnabled() && Config::getGPSAlwaysOn()) {
                GPS::startUART();
            }
            Serial.println("[C5] disabled; UART released");
            return;
        }
        if (equalsIgnoreCase(arg, "ON") ||
            equalsIgnoreCase(arg, "ENABLE") ||
            equalsIgnoreCase(arg, "INIT")) {
            Config::setC5Enabled(true);
            C5Monster::begin(Config::getC5RxPin(), Config::getC5TxPin(), Config::getC5Baud());
            Serial.println("[C5] UART initialized");
            return;
        }
        if (equalsIgnoreCase(arg, "PROBE")) {
            C5Monster::clearOutputLog();
            bool ok = C5Monster::probe();
            Serial.printf("[C5] probe %s\n", ok ? "OK" : "FAIL");
            return;
        }
        if (equalsIgnoreCase(arg, "STOP")) {
            C5Monster::emergencyStop();
            Serial.println("[C5] emergency stop sent");
            return;
        }
        C5Monster::clearOutputLog();
        const bool sent = C5Monster::sendCommand(arg);
        Serial.printf("[C5] %s: %s\n", sent ? "sent" : "rejected", arg);
        return;
    }

    if (equalsIgnoreCase(trimmed, "SHOT") || equalsIgnoreCase(trimmed, "SCREENSHOT")) {
        if (!serialScreenshotPending) {
            serialScreenshotPending = true;
            Serial.println("[SHOT] queued");
        } else {
            Serial.println("[SHOT] busy");
        }
        return;
    }

    if (equalsIgnoreCase(trimmed, "HELP")) {
        Serial.println("[SERIAL] commands: SHOT, FRAME <preset>|OFF, FRAMESTATS, ROOMSTATS, PRESENTSTATS, GPS [RESET], IMU, SD, WD [START|STOP], C5 [ON|OFF|PROBE|STOP|<JanOS cmd>]");
        return;
    }

    Serial.printf("[SERIAL] unknown: %s\n", trimmed);
}

static void pollSerialCommands() {
    while (Serial.available() > 0) {
        int value = Serial.read();
        if (value < 0) break;

        char c = (char)value;
        if (c == '\r' || c == '\n') {
            if (serialCommandLen > 0) {
                serialCommandBuf[serialCommandLen] = '\0';
                handleSerialCommand(serialCommandBuf);
                serialCommandLen = 0;
                serialCommandBuf[0] = '\0';
            }
            continue;
        }

        if ((uint8_t)c < 32 || (uint8_t)c > 126) continue;
        if (serialCommandLen + 1 >= sizeof(serialCommandBuf)) {
            serialCommandLen = 0;
            serialCommandBuf[0] = '\0';
            continue;
        }
        serialCommandBuf[serialCommandLen++] = c;
    }
}

// ==[ PET INTERACTION ]== diminishing returns pig pats
static uint32_t lastPetTime = 0;
static uint8_t petCount = 0;
static uint32_t petResetMode = 0;  // mode change resets counter
static bool getPortalAnchorForMode(HamletMode mode, int16_t& x, int16_t& y);

static bool isMenuCinematicActive() {
    return currentMode == HamletMode::MENU && MenuPig::isMenuTransitionLocked();
}

static bool isMenuInputLocked() {
    return currentMode == HamletMode::MENU && MenuPig::isMenuTransitionLocked();
}

static bool isScreenTouchLocked() {
    return screenTouchLocked;
}

static void syncScreenTouchLockEffects(HamletMode mode) {
    if (mode == HamletMode::WARDRIVE) {
        WardriveScene::setParallaxEnabled(!screenTouchLocked);
    } else {
        WardriveScene::setParallaxEnabled(true);
    }
}

static void setScreenTouchLock(bool locked) {
    if (screenTouchLocked == locked) return;
    screenTouchLocked = locked;
    screenTouchLockHintMs = 0;
    syncScreenTouchLockEffects(currentMode);
    if (locked) {
        Display::showToast(SCREEN_LOCK_TEXT, SCREEN_LOCK_TOAST_MS);
        Haptic::tick();
    }
}

static void clearScreenTouchLock() {
    if (!screenTouchLocked) return;
    setScreenTouchLock(false);
    Display::showToast(SCREEN_UNLOCK_TEXT, SCREEN_UNLOCK_TOAST_MS);
    Haptic::tick();
}

static int16_t abs16(int16_t v) {
    return (v < 0) ? -v : v;
}

static bool isLockableTouchEvent(const Touch::TouchEvent& evt) {
    // Ignore synthetic or empty events, but keep zone-independent unlock
    // so edge-start touches can still be evaluated.
    return evt.gesture != Touch::Gesture::NONE;
}

static bool isScreenUnlockGesture(const Touch::TouchEvent& evt) {
    if (evt.gesture == Touch::Gesture::SWIPE_UP) return true;
    if (evt.dy >= 0) return false;
    if (evt.dy > -SCREEN_UNLOCK_MIN_UP_DY) return false;
    // Favor upward intent: require clear vertical dominance over horizontal.
    return (abs16(evt.dy) * 2u) >= abs16(evt.dx);
}

static void showScreenLockHintToast(uint32_t now) {
    if (screenTouchLockHintMs != 0 &&
        now - screenTouchLockHintMs < SCREEN_LOCK_HINT_COOLDOWN_MS) {
        return;
    }
    screenTouchLockHintMs = (now == 0) ? 1 : now;
    Display::showToast(SCREEN_LOCK_HINT_TEXT, SCREEN_LOCK_HINT_TOAST_MS);
}

static bool isMenuTiltMode(HamletMode mode) {
    switch (mode) {
        case HamletMode::MENU:
        case HamletMode::LOOT:
        case HamletMode::FEEDING:
        case HamletMode::MAIL:
        case HamletMode::SETTINGS:
        case HamletMode::POWER_MENU:
        case HamletMode::ABOUT:
            return true;
        default:
            return false;
    }
}

static inline void snapPortalAnchor(int16_t& x, int16_t& y) {
    if (x < 12) x = 12;
    if (x > 228) x = 228;
    if (y < 20) y = 20;
    if (y > 108) y = 108;
    x = (int16_t)(x & ~1);
    y = (int16_t)(y & ~1);
}

static bool getPortalAnchorForMode(HamletMode mode, int16_t& x, int16_t& y) {
    bool ok = true;
    switch (mode) {
        case HamletMode::IDLE:
        case HamletMode::HUNT: {
            // Body center from pig drawX + half body dims
            x = (int16_t)(Avatar::getCurrentX() + UIMeasurements::MenuPigLayout::kPigW / 2);
            int16_t nx, ny;
            Avatar::getNosePosition(nx, ny);
            y = ny;  // nose Y ≈ body center Y
            break;
        }
        case HamletMode::MENU:
            ok = MenuPig::getPortalAnchor(x, y);
            break;
        default:
            ok = false;
            break;
    }
    if (!ok) return false;
    snapPortalAnchor(x, y);
    return true;
}

static bool updatePortalOverlayState(uint32_t now) {
    // Teleport system handles transitions now.
    // Suppress hold ring during active teleport.
    if (Teleport::isActive()) {
        Display::setHoldProgress(0.0f);
        return true;
    }
    return false;
}

// Unified gesture system for tilt navigation
static void updateGestures(uint32_t now) {
    if (isPowerWakeInputBlocked(now) ||
        (currentMode == HamletMode::POWER_MENU && Display::isShowingSleepWarning())) {
        btn.gestureHoldProgress = 0.0f;
        return;
    }
    if (!isMenuTiltMode(currentMode)) {
        btn.gestureHoldProgress = 0.0f;
        return;
    }
    if (currentMode == HamletMode::SETTINGS && (SettingsMenu::isShowingWarning() || SettingsMenu::isTextEditing())) {
        btn.gestureHoldProgress = 0.0f;
        return;
    }

    // Check if tilt navigation is enabled in config
    if (!Config::getTiltNavigationEnabled()) {
        btn.gestureHoldProgress = 0.0f;
        return;
    }

    // Define navigation states
    enum class NavState {
        IDLE,
        NAVIGATION_OCCURRED,
        WAITING_NEUTRAL_CONFIRMED
    };

    // Define button gesture states
    enum class ButtonGestureState {
        IDLE,
        GESTURE_INITIATED_SHORT,  // Armed for short press
        GESTURE_INITIATED_LONG,  // Armed for long press
        GESTURE_INITIATED_SUPER_LONG, // Armed for super long press (power menu)
        WAITING_NEUTRAL_CONFIRMED
    };

    static uint32_t lastUpdate = 0;
    static uint32_t lastTiltNavAt = 0;
    static bool wasHorizontal = false;
    static float tiltAccumulator = 0.0f;
    static NavState navState = NavState::IDLE;
    static ButtonGestureState buttonState = ButtonGestureState::IDLE;
    static uint32_t gestureStartTime = 0;
    static const uint32_t TILT_NAV_MIN_MS = 200;

    if (isMenuInputLocked()) {
        btn.gestureHoldProgress = 0.0f;
        wasHorizontal = false;
        tiltAccumulator = 0.0f;
        navState = NavState::IDLE;
        buttonState = ButtonGestureState::IDLE;
        gestureStartTime = 0;
        return;
    }

    if (lastUpdate == 0) {
        lastUpdate = now;
        return;
    }
    if (now - lastUpdate < 16) return; // ~60Hz update rate

    float dt = (now - lastUpdate) / 1000.0f;
    lastUpdate = now;
    if (dt > 0.1f) dt = 0.1f;
    if (dt < 0.001f) dt = 0.016f;

    float ax, ay, az;
    Pedometer::getCachedAccel(ax, ay, az);

    // Orientation from unified cache (0.65/0.75 hysteresis)
    float absAz = fabsf(az);
    bool deviceFlat = Pedometer::isCachedFlat();

    bool wasHorizontalPrev = wasHorizontal;
    bool isHorizontal = (absAz < 0.5f);  // Device is flat/horizontal (lying down)
    bool isUprightTopUp = (az > 0.7f);   // Device is upright with top facing up
    wasHorizontal = isHorizontal;  // Update for next iteration
    float newGestureHoldProgress = 0.0f;

    // Handle button gesture logic first (UP-TILT)
    switch (buttonState) {
        case ButtonGestureState::IDLE:
            // Wait for UP-TILT gesture: transition from flat to upright with top facing up
            if (isUprightTopUp && wasHorizontalPrev) {
                // UP-TILT gesture initiated - start timing
                gestureStartTime = now;
                buttonState = ButtonGestureState::GESTURE_INITIATED_SHORT;
                SFX::tone(1500, 20); // Higher pitch tone when gesture is initiated (armed for short press)
            }
            break;

        case ButtonGestureState::GESTURE_INITIATED_SHORT:
            // Still in the gesture, check if it's becoming a long press or super long press
            if (isUprightTopUp) {
                uint32_t held = now - gestureStartTime;
                newGestureHoldProgress = (float)held / (float)GESTURE_LONG_PRESS_MS;
                if (newGestureHoldProgress > 1.0f) newGestureHoldProgress = 1.0f;
                // Still upright, check if we've exceeded long/super-long thresholds
                if (held >= GESTURE_SUPER_LONG_PRESS_MS) {
                    buttonState = ButtonGestureState::GESTURE_INITIATED_SUPER_LONG;
                    SFX::play(SFX::GESTURE_SUPER_LONG_PIP);
                } else if (held >= GESTURE_LONG_PRESS_MS) {
                    buttonState = ButtonGestureState::GESTURE_INITIATED_LONG;
                    SFX::play(SFX::GESTURE_LONG_PIP);
                }
            } else if (isHorizontal) {
                // Returned to horizontal before long press threshold
                // Execute short press B
                handleBtnBack(false);
                SFX::tone(1200, 15); // Different feedback tone when returning to neutral for short press
                buttonState = ButtonGestureState::WAITING_NEUTRAL_CONFIRMED;
            }
            break;

        case ButtonGestureState::GESTURE_INITIATED_LONG:
            // Still in the gesture, check if it's becoming a super long press
            if (isUprightTopUp) {
                newGestureHoldProgress = 1.0f;
                if (now - gestureStartTime >= GESTURE_SUPER_LONG_PRESS_MS) { // 5 second threshold
                    buttonState = ButtonGestureState::GESTURE_INITIATED_SUPER_LONG;
                    SFX::play(SFX::GESTURE_SUPER_LONG_PIP);
                }
            } else if (isHorizontal) {
                // Execute long press B
                handleBtnBack(true);
                SFX::tone(2000, 25); // Different feedback tone when returning to neutral for long press
                buttonState = ButtonGestureState::WAITING_NEUTRAL_CONFIRMED;
            }
            break;

        case ButtonGestureState::GESTURE_INITIATED_SUPER_LONG:
            // Waiting for return to horizontal to execute super long press (power menu)
            if (isUprightTopUp) {
                newGestureHoldProgress = 1.0f;
            }
            if (isHorizontal) {
                // Execute super long press - show power menu
                Hamlet::enterMode(HamletMode::POWER_MENU);
                SFX::tone(2500, 100); // Confirmation sound for power menu
                buttonState = ButtonGestureState::WAITING_NEUTRAL_CONFIRMED;
            }
            break;

        case ButtonGestureState::WAITING_NEUTRAL_CONFIRMED:
            // This state is the whole debounce: an executed gesture cannot fire
            // again until the device has held horizontal for 100ms straight, so
            // a wrist that wobbles through neutral does not re-arm on the way.
            static uint32_t buttonNeutralStartTime = 0;

            if (isHorizontal) {
                // Device is horizontal, start or continue the confirmation timer
                if (buttonNeutralStartTime == 0) {
                    buttonNeutralStartTime = now;
                } else if (now - buttonNeutralStartTime > 100) { // 100ms confirmation period
                    // Device has been consistently horizontal, confirm neutral position
                    buttonState = ButtonGestureState::IDLE;
                    buttonNeutralStartTime = 0;
                    gestureStartTime = 0;
                }
            } else {
                // Device is not horizontal, reset confirmation
                buttonNeutralStartTime = 0;
            }
            break;
    }
    btn.gestureHoldProgress = newGestureHoldProgress;

    // Handle navigation gesture logic (left/right tilt) - only if not in button gesture neutral wait
    if (deviceFlat) {
        tiltAccumulator = 0.0f;
        navState = NavState::IDLE; // Reset state when device is flat
        return;
    }

    // Check if we're waiting for neutral position after navigation
    if (navState == NavState::WAITING_NEUTRAL_CONFIRMED) {
        // We need to confirm the device is truly in neutral (upright) position
        // by checking that it's been consistently upright for a short period
        static uint32_t navNeutralStartTime = 0;

        if (!deviceFlat) {
            // Device is upright, start or continue the confirmation timer
            if (navNeutralStartTime == 0) {
                navNeutralStartTime = now;
            } else if (now - navNeutralStartTime > 100) { // 100ms confirmation period
                // Device has been consistently upright, confirm neutral position
                navState = NavState::IDLE;
                navNeutralStartTime = 0;
            }
        } else {
            // Device is not upright, reset confirmation
            navNeutralStartTime = 0;
        }
        // Don't process navigation while waiting for neutral confirmation
    } else {
        // Process navigation if not waiting for neutral
        const float DEADZONE = 0.05f;
        const float SCROLL_SPEED = 20.0f;

        // Core2 MPU6886: tilt on Y axis
        float tilt = ay;
        if (fabsf(tilt) < DEADZONE) {
            tilt = 0.0f;
        } else {
            tilt = (tilt > 0) ? (tilt - DEADZONE) : (tilt + DEADZONE);
        }
        tilt = constrain(tilt, -1.0f, 1.0f);

        tiltAccumulator += (-tilt) * SCROLL_SPEED * dt;

        while (tiltAccumulator >= 1.0f) {
            if (lastTiltNavAt > 0 && now - lastTiltNavAt < TILT_NAV_MIN_MS) {
                break;
            }
            tiltAccumulator -= 1.0f;

            // Only process navigation if we're in IDLE state
            if (navState == NavState::IDLE) {
                switch (currentMode) {
                    case HamletMode::MENU: Menu::next(); break;
                    case HamletMode::LOOT: LootMenu::next(); break;
                    case HamletMode::FEEDING: FeedingMenu::next(); break;
                    case HamletMode::MAIL: MailMenu::next(); break;
                    case HamletMode::SETTINGS: SettingsMenu::next(); break;
                    case HamletMode::POWER_MENU: Display::nextPowerOption(); break;
                    default: break;
                }
                lastTiltNavAt = now;
                if (currentMode != HamletMode::MENU || !Menu::hasActiveEncounter()) {
                    SFX::click();
                }
                navState = NavState::NAVIGATION_OCCURRED; // Mark that navigation happened
            }
            break; // Only process one navigation per update to allow state transition
        }

        while (tiltAccumulator <= -1.0f) {
            if (lastTiltNavAt > 0 && now - lastTiltNavAt < TILT_NAV_MIN_MS) {
                break;
            }
            tiltAccumulator += 1.0f;

            // Only process navigation if we're in IDLE state
            if (navState == NavState::IDLE) {
                switch (currentMode) {
                    case HamletMode::MENU: Menu::prev(); break;
                    case HamletMode::LOOT: LootMenu::prev(); break;
                    case HamletMode::FEEDING: FeedingMenu::prev(); break;
                    case HamletMode::MAIL: MailMenu::prev(); break;
                    case HamletMode::SETTINGS: SettingsMenu::prev(); break;
                    case HamletMode::POWER_MENU: Display::prevPowerOption(); break;
                    default: break;
                }
                lastTiltNavAt = now;
                if (currentMode != HamletMode::MENU || !Menu::hasActiveEncounter()) {
                    SFX::click();
                }
                navState = NavState::NAVIGATION_OCCURRED; // Mark that navigation happened
            }
            break; // Only process one navigation per update to allow state transition
        }

        // If navigation occurred and we're back near center, move to waiting neutral state
        if (navState == NavState::NAVIGATION_OCCURRED && fabsf(tilt) < DEADZONE * 2) {
            navState = NavState::WAITING_NEUTRAL_CONFIRMED;
        }
    }
}

void init() {
    bootTime = millis();
    HamletSession::reset(session);
    uint8_t bootStage = 0;
    auto advanceBootStage = [&bootStage]() {
        if (bootStage < kBootStageTotal) bootStage++;
        Display::advanceBootIntro(bootStage);
    };
    
    // ==[ CAPTURE RAM ]== fill PSRAM pantry before the hunt
    if (!Capture::init()) {
        HAMLET_LOGLN("[HAMLET] PSRAM capture buffer alloc FAILED - hunt captures disabled");
    }
    bootStage++;
    
    // ==[ CONFIG LOAD ]== NVS -> runtime knobs
    Config::init();
    Achievements::init();
    ItemDrops::init();
    ItemEffects::init();
    Bounty::init();
    Mailbox::init();
    TouchHints::loadSeenMask();
    bootStage++;
    bootStage++;

    // ==[ POWER PROFILES ]== adaptive CPU/TX/FPS based on mode + battery
    Power::init();
    bootStage++;

    // ==[ GPS ]== UART2 — starts if always-on enabled
    GPS::init();

    // ==[ C5MONSTER ]== UART bridge for dual-band (5GHz) if enabled
    if (Config::getC5Enabled()) {
        C5Monster::begin(Config::getC5RxPin(), Config::getC5TxPin(), Config::getC5Baud());
    }

    // ==[ MESH ]== Unit C6L on a Grove port. Started at boot rather than on
    // mode entry so a message arriving while the pig is elsewhere still lands
    // in the scrollback — Config::init() already resolved any pin fight with
    // GPS or the C5 bridge, so an enabled bridge here owns its pins outright.
    if (Config::getMeshEnabled()) {
        Mesh::begin(Config::getMeshRxPin(), Config::getMeshTxPin(),
                    Config::getMeshBaud(), Config::getMeshCodec());
    }
#if HAMLET_MESH_TRACE
    // ==[ SNIFFER BUILD ]== the bridge is off by default and Config::init()
    // also switches it off on a pin fight, so a trace build that respected
    // that flag would print an unbroken silence and blame the radio for it.
    // Say which it is, and open the port anyway when nothing else holds those
    // pins — the whole point of this build is to see the wire.
    Serial.printf("[MESH?] boot: enabled=%s rx=%u tx=%u baud=%lu gps=%s(%u/%u) "
                  "c5=%s(%u/%u)\n",
                  Config::getMeshEnabled() ? "YES" : "no",
                  (unsigned)Config::getMeshRxPin(),
                  (unsigned)Config::getMeshTxPin(),
                  (unsigned long)Config::getMeshBaud(),
                  Config::getGPSEnabled() ? "on" : "off",
                  (unsigned)Config::getGPSRxPin(),
                  (unsigned)Config::getGPSTxPin(),
                  Config::getC5Enabled() ? "on" : "off",
                  (unsigned)Config::getC5RxPin(),
                  (unsigned)Config::getC5TxPin());
    if (!Mesh::isStarted()) {
        const uint8_t rx = Config::getMeshRxPin();
        const uint8_t tx = Config::getMeshTxPin();
        // The same predicate Config::init() arbitrated with, so the reason
        // printed here is the reason the bridge is actually down.
        if (const char* owner = Config::meshPinOwner()) {
            Serial.printf("[MESH?] NOT forcing: %s already owns rx=%u tx=%u\n",
                          owner, (unsigned)rx, (unsigned)tx);
        } else {
            Serial.println("[MESH?] bridge was off — forcing it up to listen");
            Mesh::begin(rx, tx, Config::getMeshBaud(),
                        Config::getMeshCodec());
        }
    }
#endif

    // ==[ SFX BUS ]== non-blocking sound queue; init early for boot sound
    SFX::init();
    bootStage++;

    // ==[ HAPTIC ]== vibration motor patterns
    Haptic::init();

    // ==[ KEYPAD ]== Grove Port A CardKB, probed on the run loop. Ordered after
    // the C5 bridge so the Core2 port-sharing verdict is already settled.
    CardKB::begin();

    
    // ==[ SESSION STREAK ]== adjust mood before first phrase
    Config::updateSessionStreak();
    bootStage++;

    // ==[ SESSION CHALLENGES ]== generate 3 fresh tasks
    Challenges::generate(Config::getLevel());

    // ==[ SESSION GOAL ]== evaluate last session's steps; sprinkle rewards
    Config::evaluateSessionGoal();
    bootStage++;
    
    // ==[ DISPLAY ]== canvas + theme init
    Display::init();
    AmbientLED::init();
    bootStage++;
    Display::beginBootIntro(bootStage, kBootStageTotal);

    // ==[ SD CARD ]== mount after display (shared VSPI bus, display CS must be high)
    SDStorage::init();  // silent fail = PSRAM-only mode

    // ==[ STASH RESTORE ]== reload captures from SD journal (must be after SD mount)
    if (SDStorage::isAvailable()) {
        Capture::restoreJournal();
    } else {
        HAMLET_LOGLN("[HAMLET] no SD — journal restore skipped");
    }

    // ==[ PHRASE CACHE ]== load curated lore
    Mood::init();
    advanceBootStage();
    
    // ==[ AVATAR PHYSICS ]== clouds, grass, wiggle states
    Avatar::init();
    advanceBootStage();
    
    // ==[ IMU INIT ]== pedometer ready for grass-touch metrics
    Pedometer::init();
    advanceBootStage();
    
    // ==[ IPP DEFENSE ]== BLE must init before WiFi/ESP-NOW for coex
    Potfile::init();
    DefensePipeline::init();
    advanceBootStage();

    // ==[ NOWFLOCK ]== passive ESP-NOW flocking; Recon BLE already registered
    NowFlock::init();
    advanceBootStage();
    
    Display::finishBootIntro();

    // ==[ BOOT BARK ]== emit phrase after intro handoff
    Mood::onBoot();

    // ==[ IDLE ENTRY ]== drop into home screen
    enterMode(HamletMode::IDLE);

    // ==[ BOOT ARRIVAL ]== pig materializes from particles (REASSEMBLE+SETTLE)
    {
        // idle pig body center — use display.h bar height (14px), not UIMeasurements (20px menu bars)
        constexpr int pigH = UIMeasurements::MenuPigLayout::kPigH;  // 42
        int16_t dstX = (int16_t)(Avatar::getCurrentX() + UIMeasurements::MenuPigLayout::kPigW / 2);
        int16_t dstY = (int16_t)(SCREEN_HEIGHT - BOTTOM_BAR_H - pigH - 5 + pigH / 2);
        int16_t bootSourceX = 0;
        int16_t bootSourceY = 0;
        int16_t bootPortalX = 0;
        int16_t bootPortalY = 0;
        MenuPig::getBootWardriveTeleportAnchors(bootSourceX, bootSourceY,
                                                bootPortalX, bootPortalY);
        (void)bootSourceX;
        (void)bootSourceY;
        Teleport::startBootArrival((float)bootPortalX, (float)bootPortalY,
                                   (float)dstX, (float)dstY, millis(),
                                   avatarTeleportSilhouette());
    }
}

void update() {
    uint32_t now = millis();
    frameNow = now;
    FrameBudget::beginFrame(now);
    pollSerialCommands();

    // SD: mounted = cheap cardType probe; unmounted = backoff remount (VSPI shared w/ LCD)
    if (FrameBudget::hasTime(50)) {
        SDStorage::update();
        SDStorage::drainDeferred(6);
    }
    FrameBudget::notePhase("sd");

    GPS::update();        // feed NMEA bytes if UART is hot
    C5Monster::service(); // C5Monster UART bridge — non-blocking parse
    if (Config::getC5Enabled()) {
        C5Monster::maintainConnection(now);
    }
    Mesh::service(now);   // Meshtastic UART bridge — non-blocking line parse
    announceMeshArrival();

// ==[ C5 MONSTER FEED ]== dispatch parsed scan results to subsystems
    {
        static uint32_t lastC5FeedRevision = 0;
        static uint32_t lastC5GpsFeedRevision = 0;
        static uint32_t lastC5ScanMs = 0;
        static uint8_t c5ScanPhase = 0;
        static uint32_t c5ScanPhaseMs = 0;
        static bool c5InitialScanDone = false;
        static uint8_t c5GpsPhase = 0;  // 0=idle, 1=acquire, 2=stopping
        static uint32_t c5GpsStartMs = 0;
        static uint32_t c5GpsStopMs = 0;
        static uint32_t c5GpsStartRevision = 0;
        static constexpr uint32_t C5_SCAN_INTERVAL_MS = 8000;
        static constexpr uint32_t C5_SCAN_STALE_TIMEOUT_MS = 22000;
        static constexpr uint32_t C5_GPS_ACQUIRE_TIMEOUT_MS = 4000;
        static constexpr uint32_t C5_GPS_STOP_GUARD_MS = 1500;

        // Enable/disable 5GHz band based on C5 settings + bridge health.
        Spectrum::set5GHzEnabled(Config::getC5Enabled() && C5Monster::isConnected());
        const C5Monster::ScanResults& c5r = C5Monster::getScanResults();
        const C5Monster::GPSFix& c5Fix = C5Monster::getGPSFix();
        if (c5Fix.revision != 0 &&
            c5Fix.revision != lastC5GpsFeedRevision) {
            lastC5GpsFeedRevision = c5Fix.revision;
            Wardrive::feedC5MonsterGPSFix(c5Fix);
            // A scan published immediately before this fix may have deferred
            // its rows for lack of coordinates. Replay only Wardrive ingest;
            // dedup marks already-written rows and persists the held ones to
            // the CoreS3 SD with the new position.
            if (Wardrive::isActive() && c5r.revision != 0) {
                Wardrive::feedC5MonsterScan(c5r);
            }
        }
        if (c5r.revision != 0 && c5r.revision != lastC5FeedRevision) {
            lastC5FeedRevision = c5r.revision;
            // A scan may be initiated through raw serial/C5 menu as well as
            // this scheduler. Any published revision owns the backoff clock.
            lastC5ScanMs = now;
            c5ScanPhase = 0;
            c5ScanPhaseMs = 0;
            Spectrum::feedC5MonsterScan(c5r);
            Hunt::feedC5MonsterScan(c5r);
            Wardrive::feedC5MonsterScan(c5r);
            DefensePipeline::feedC5MonsterScan(c5r);
        }

        // Centralized C5 scan trigger (avoids spectrum + menu racing).
        // Current JanOS publishes the CSV set when its background scan ends,
        // so 0 = idle and 1 = waiting for that explicit completion/footer.
        const bool c5Ready =
            Config::getC5Enabled() && C5Monster::isConnected();
        bool c5GpsOwnsFrame = false;
        if (!c5Ready) {
            c5GpsPhase = 0;
            c5GpsStartMs = 0;
            c5GpsStopMs = 0;
            c5GpsStartRevision = 0;
        } else if (c5GpsPhase == 1) {
            c5GpsOwnsFrame = true;
            const bool receivedNewFix =
                c5Fix.revision != c5GpsStartRevision;
            const bool timedOut =
                now - c5GpsStartMs >= C5_GPS_ACQUIRE_TIMEOUT_MS;
            if (receivedNewFix || timedOut || !Wardrive::isActive()) {
                if (C5Monster::sendCommand(C5Protocol::CMD_STOP, 4000)) {
                    c5GpsPhase = 2;
                } else {
                    C5Monster::emergencyStop();
                    c5GpsPhase = 2;
                }
                c5GpsStopMs = now;
            }
        } else if (c5GpsPhase == 2) {
            c5GpsOwnsFrame = true;
            if (!C5Monster::isBusy() &&
                now - c5GpsStopMs >= C5_GPS_STOP_GUARD_MS) {
                c5GpsPhase = 0;
                c5GpsStartMs = 0;
                c5GpsStopMs = 0;
                if (Wardrive::isActive() &&
                    C5Monster::getActiveOperation() ==
                        C5Monster::Operation::NONE &&
                    C5Monster::sendCommand(
                        C5Protocol::CMD_SCAN_NETWORKS)) {
                    c5ScanPhase = 1;
                    c5ScanPhaseMs = now;
                    c5InitialScanDone = true;
                }
            }
        } else if (Wardrive::isActive() && c5ScanPhase == 0 &&
                   !C5Monster::isBusy()) {
            const C5Monster::Operation operation =
                C5Monster::getActiveOperation();
            if (operation == C5Monster::Operation::NONE) {
                c5GpsStartRevision = c5Fix.revision;
                c5GpsStartMs = now;
                if (C5Monster::sendCommand(
                        C5Protocol::CMD_START_GPS_RAW, 4000)) {
                    c5GpsPhase = 1;
                } else {
                    c5GpsStartMs = 0;
                }
                // Never bypass the GPS gate with a same-frame scan. Retry the
                // acquisition on the next idle frame if the command failed.
                c5GpsOwnsFrame = true;
            }
        }

        if (!c5Ready) {
            c5ScanPhase = 0;
            c5ScanPhaseMs = 0;
            lastC5ScanMs = now;
            c5InitialScanDone = false;
        } else if (c5GpsOwnsFrame) {
            // Starting/stopping the reader owns this scheduler frame.
        } else if (C5Monster::hasActiveOperation()) {
            // Continuous JanOS observers/operations own the C5 radio until
            // STOP. Do not interleave the periodic scan state machine.
            c5ScanPhase = 0;
            c5ScanPhaseMs = 0;
            lastC5ScanMs = now;
        } else if (!c5InitialScanDone && !C5Monster::isBusy()) {
            // Trigger immediate initial scan on first connection
            if (C5Monster::sendCommand(C5Protocol::CMD_SCAN_NETWORKS)) {
                c5ScanPhase = 1;
                c5ScanPhaseMs = now;
                c5InitialScanDone = true;
            }
        } else if (c5ScanPhase == 0) {
            if (now - lastC5ScanMs > C5_SCAN_INTERVAL_MS && !C5Monster::isBusy()) {
                if (C5Monster::sendCommand(C5Protocol::CMD_SCAN_NETWORKS)) {
                    c5ScanPhase = 1;
                    c5ScanPhaseMs = now;
                } else {
                    lastC5ScanMs = now;  // back off on send failure
                }
            }
        } else if (c5ScanPhase == 1) {
            if (!C5Monster::isBusy()) {
                lastC5ScanMs = now;
                c5ScanPhase = 0;
            } else if (now - c5ScanPhaseMs > C5_SCAN_STALE_TIMEOUT_MS) {
                lastC5ScanMs = now;
                c5ScanPhase = 0;
            }
        }
    }

    // ==[ GPS TOASTS ]== transition feedback with 10s shared cooldown
    {
        static uint32_t gpsToastCooldown = 0;
        bool gotFix   = GPS::consumeFixAcquired();
        bool lostFix  = GPS::consumeFixLost();
        bool gotNmea  = GPS::consumeNmeaDetected();

        if (gotFix && TimeMath::reachedOrUnset(now, gpsToastCooldown)) {
            char gBuf[32];
            snprintf(gBuf, sizeof(gBuf), "GPS LOCKED // %d SAT", GPS::getSatCount());
            Display::showToast(gBuf, 2500);
            gpsToastCooldown = now + 10000;
        } else if (lostFix && TimeMath::reachedOrUnset(now, gpsToastCooldown)) {
            Display::showToast("GPS LOCK LOST", 3000, 2);
            gpsToastCooldown = now + 10000;
        } else if (gotNmea) {
            Display::showToast("GPS SIGNAL", 1500);
        }
    }

    Weather::update();    // weather ticks every loop
    Pedometer::updateBackground();  // refresh shared IMU/step cache before mode logic + draw

    // ==[ MOOD DECAY ]== decay per frame
    Mood::decayMomentum();

    // ==[ LEVEL-UP CHECK ]== fire event if XP crossed a boundary
    if (Config::hasLevelUp()) {
        uint8_t newLevel = Config::consumeLevelUp();
        Mood::onLevelUp(newLevel);
    }

    // ==[ DEFERRED EARNED LOOT ]== queue-full payouts get first refusal.
    ItemDrops::update();
    ItemEffects::update();

    // ==[ ACHIEVEMENT CELEBRATION ]== consume unlock queue with cooldown
    // Busy copy lane or full loot queue? trophy waits. no invisible flex.
    bool trophyLaneClear = !Display::isItemDropActive() &&
                           !Display::hasActiveQuickToast() &&
                           !Display::isHelpOverlayActive() &&
                           !Menu::hasActiveEncounter();
    if (trophyLaneClear && ItemDrops::canQueueAward() &&
        Achievements::hasPendingCelebration()) {
        Achievement ach = Achievements::popPendingCelebration();
        SFX::play(SFX::ACHIEVEMENT_UNLOCK);
        Haptic::pulse();
        // toast phrase via mood
        char achBuf[48];
        snprintf(achBuf, sizeof(achBuf), "!! %s !!", Achievements::getName(ach));
        Mood::setPhrase(achBuf, AvatarState::EXCITED);
        // Toast is global; room/menu screens do not always render Mood bubbles.
        // It also holds the item ceremony until the trophy name gets read.
        Display::showToast(achBuf, 2200, 1);
        Avatar::triggerSparkles(5);
        ItemDrops::awardGuaranteed(ItemDrops::ItemDropSource::ACHIEVEMENT, 4);
    }

    // ==[ ITEM DROP CEREMONY ]== one native sprite reveal at a time.
    // wait for clear copy lanes; case loot should be seen, not buried.
    if (!Display::isItemDropActive() &&
        !Display::hasActiveQuickToast() &&
        !Display::isHelpOverlayActive() &&
        !Menu::hasActiveEncounter() &&
        ItemDrops::hasPendingAward()) {
        ItemDrops::ItemAward award;
        if (ItemDrops::popPendingAward(award)) {
            Barman::onItemDropped(award.itemId, award.contextOrdinal, now);
            Display::showItemDrop(award.itemId, award.firstTime, award.source);
        }
    }
    
    // ==[ PHASE B: STEP-BASED MOMENTUM GAIN ]== +4 per 100 steps (only when NOT hunting)
    // Charges pig for hunting. Walking = physical activity = game power.
    if (currentMode != HamletMode::HUNT && currentMode != HamletMode::SPECTRUM) {
        bool sessionActive = Config::isSessionActive();
        uint32_t currentSteps = Pedometer::getSteps();
        if (currentSteps >= session.lastMomentumStep + 100) {
            Mood::addMomentum(4);  // +4 per 100 steps (revised from +2)
            session.lastMomentumStep = currentSteps;
        }
        
        // ==[ CHALLENGE: STEPS ]== update step-based challenges
        Challenges::onStepsUpdate(Config::getSessionSteps());

        // Walk milestones are session-scoped. The pure helper owns the table so
        // runtime payouts, host tests, and R1B R4CK cannot drift apart again.
        uint32_t todaySteps = Config::getSessionSteps();
        HamletSession::StepMilestone milestone{};
        if (HamletSession::popStepMilestone(session, todaySteps, milestone) &&
            sessionActive) {
            Config::addXP(milestone.xpReward,
                          Config::RewardSource::WALK_MILESTONE);
            Mood::onWalkMilestone(milestone.threshold);
        }
    } else if (currentMode == HamletMode::HUNT || currentMode == HamletMode::SPECTRUM) {
        // Reset milestone when entering hunt (don't double-count steps during hunting)
        session.lastMomentumStep = Pedometer::getSteps();

        // check accumulated hunt/spectrum time for session active
        if (session.huntActive) {
            uint32_t total = HamletSession::getHuntTimeMs(session, now);
            if (!session.huntTimeMarked && total >= 120000) {
                Config::markSessionActive();
                session.huntTimeMarked = true;
            }
            // ==[ CHALLENGE: SCAN MINUTES ]==
            Challenges::onScanMinute((uint16_t)(total / 60000));
        }
    }
    
    // ==[ IMU HUNT PROMPT ]== Fogg B=MAP: high motivation at moment of motion
    // Show toast when walking in IDLE — low friction tap-to-hunt CTA (sim: #5 sensitivity 0.0072)
    if (currentMode == HamletMode::IDLE) {
        static uint32_t huntPromptNext    = 0;
        static bool     huntPromptShown   = false;
        static bool     huntPromptPending = false;  // true only while OUR toast is active
        if (Pedometer::isVisuallyWalking()) {
            if (!huntPromptShown && TimeMath::reachedOrUnset(now, huntPromptNext)) {
                Display::showToast("walk detected // hunt?", 3000);
                huntPromptShown   = true;
                huntPromptPending = true;
                huntPromptNext    = now + 30000;
            }
        } else {
            huntPromptShown   = false;
            huntPromptPending = false;
        }
        // clear pending flag if toast expired on its own
        if (huntPromptPending && !Display::hasActiveQuickToast()) {
            huntPromptPending = false;
        }
        paranoia.huntPromptToastActive = huntPromptPending;
    }

    // ==[ CONFIG TICK ]== flush deferred saves
    Config::update();

    // ==[ INPUT POLL ]==
    // The keypad is polled before it is dispatched, and dispatched after the
    // buttons so a frame carrying both resolves in the same order a finger
    // would have produced.
    CardKB::update(now);
    announceKeypadHotplug();
    updateButtons(now);
    updateKeypad(now);
    updateGestures(now);
    Touch::update(now);
    TouchHints::update(now);
    if (Touch::wasTouched()) Display::resetDimTimer();
    // The screen-lock nag is updateTouch()'s: only a classified gesture can
    // tell an unlock swipe from a stray poke. Raw contact cannot, and nagging
    // on it flashed "swipe up" at a finger that was already swiping up.
    Haptic::update();
    updateTouch(now);
    syncHoldOverlayProgress();

    // ==[ TELEPORT SYSTEM ]== particle decompose/reassemble state machine
    {
        Teleport::Phase prevPhase = Teleport::getPhase();
        Teleport::update(now);
        Teleport::Phase curPhase = Teleport::getPhase();
        if (prevPhase != curPhase && Teleport::getContext() == Teleport::Context::WARDRIVE_TO_MENU) {
            HAMLET_LOGF("[WD-TP] phase %d->%d elapsed=%lu\n",
                (int)prevPhase, (int)curPhase, now);
        }
        // VOID boundary: enter destination mode
        if (Teleport::consumeVoidReady()) {
            Teleport::Context ctx = Teleport::getContext();
            if (ctx == Teleport::Context::IDLE_TO_MENU) {
                enterMode(HamletMode::MENU);
            } else if (ctx == Teleport::Context::MENU_TO_HUNT) {
                enterMode(HamletMode::HUNT);
            } else if (ctx == Teleport::Context::MENU_TO_WARDRIVE) {
                MenuPig::clearWDCinematic();
                enterMode(HamletMode::WARDRIVE);
            } else if (ctx == Teleport::Context::WARDRIVE_TO_MENU) {
                enterMode(HamletMode::MENU);
            }
        }
        // Transition complete: pig arrived
        if (prevPhase != Teleport::Phase::NONE && Teleport::getPhase() == Teleport::Phase::NONE) {
            Teleport::Context ctx = Teleport::getContext();
            // BOOT_ARRIVAL already owns a particle settle. Starting another
            // jump here reversed that landing and briefly removed the legs.
            if (ctx == Teleport::Context::IDLE_TO_MENU) {
                MenuPig::triggerPortalJump();
            } else if (ctx == Teleport::Context::MENU_TO_HUNT) {
                Avatar::cuteJump();
                Mood::onTeleportArrival();
            } else if (ctx == Teleport::Context::MENU_TO_WARDRIVE) {
                WardriveScene::triggerChairSettle(millis());
            } else if (ctx == Teleport::Context::WARDRIVE_TO_MENU) {
                MenuPig::triggerPortalJump();
                Mood::onTeleportArrival();
            }
        }
    }

    updatePortalOverlayState(now);
    
    // ==[ DIMMER ]== PORKCHOP parity
    Display::updateDimming();

    // ==[ BATTERY DRAMA ]== 30s check, three nag tiers
    static uint32_t lastBatteryCheck = 0;
    static bool warned20 = false;
    static bool warned10 = false;
    static bool warned5 = false;
    if (now - lastBatteryCheck > 30000) {  // 30s check
        lastBatteryCheck = now;

        // adapt power profile based on battery level
        Power::updateBatteryPolicy();

        uint8_t batt = getBatteryPercent();

        if (batt <= 5 && !warned5) {
            warned5 = true;
            Mood::onLowBattery();
        } else if (batt <= 10 && !warned10) {
            warned10 = true;
            // 10% + loot = panic sync
            if (Capture::getTotalCount() > 0 && Capture::getUnsyncedCount() > 0) {
                Mood::onEmergencySync();
            } else {
                Mood::onLowBattery();
            }
        } else if (batt <= 20 && !warned20) {
            warned20 = true;
            Mood::onLowBattery();
        }
        
        // charging? reset warnings.
        if (batt > 20) {
            warned20 = warned10 = warned5 = false;
        }
    }
    
    // ==[ USB-C INSERTION ]== pig excited when plugged
    // AXP2101 bouncy. 500ms debounce.
    static bool wasExternalPower = false;
    static uint32_t chargingStartTime = 0;
    static bool chargeEventFired = false;
    bool nowExternalPower = Power::isExternalPowerPresent();
    
    if (nowExternalPower != wasExternalPower) {
        // Do not wait for the 30s battery tick to lift/apply cable policy.
        Power::updateBatteryPolicy();
    }

    if (nowExternalPower && !wasExternalPower) {
        // rising edge. debounce.
        chargingStartTime = now;
        chargeEventFired = false;
    } else if (nowExternalPower && !chargeEventFired && chargingStartTime > 0) {
        // debounce passed. fire.
        if (now - chargingStartTime > 500) {
            Mood::onPluggedIn();
            chargeEventFired = true;
        }
    } else if (!nowExternalPower) {
        // unplugged. reset.
        chargingStartTime = 0;
        chargeEventFired = false;
    }
    wasExternalPower = nowExternalPower;
    
    // ==[ NOWFLOCK ]== passive flocking only; no call/session UI takeover.
    
    // ==[ AUTO-HUNT ]== 30s walk triggers hunt
    if (!Teleport::isActive() &&
        currentMode == HamletMode::IDLE &&
        Config::getAutoHuntEnabled() &&
        Pedometer::shouldAutoHunt()) {
        enterMode(HamletMode::HUNT);
        // Mood::onHuntStart() already called inside Hunt::start() via enterMode
    }
    
    // ==[ 4-MIN IDLE CRITICAL TRIGGER ]== Phase 3
    if (currentMode == HamletMode::IDLE) {
        if (idleStartTime == 0) {
            idleStartTime = now;
            idleTimerChecked = false;
        }
        
        // Check once when timer expires
        if (!idleTimerChecked && (now - idleStartTime >= 240000)) {  // 4 minutes
            idleTimerChecked = true;
            
            uint16_t unsynced = Capture::getUnsyncedCount();
            uint32_t lastCapture = Capture::getLastCaptureTime();
            
            if (unsynced > 5 && lastCapture > 0) {
                // Check freshness (<10min ago)
                uint32_t nowEpoch = getCurrentEpoch();
                uint32_t captureFreshness = (nowEpoch > 0) ? (nowEpoch - lastCapture) : 9999;
                
                if (captureFreshness < 600) {  // <10 minutes
                    Mood::trigger4MinCritical();
                }
            }
        }
    } else {
        // Reset timer when leaving IDLE
        idleStartTime = 0;
        idleTimerChecked = false;
    }
    
    // ==[ WALK PARALLAX ]== FSM in Avatar, Hamlet just feeds inputs
    if (Mood::isInCriticalScene()) {
        Avatar::setGrassMoving(false, false);
    } else if (currentMode == HamletMode::HUNT || currentMode == HamletMode::IDLE) {
        bool isWalkingNow = Pedometer::isVisuallyWalking();
        bool huntGrass = (currentMode == HamletMode::HUNT &&
                          Hunt::isChannelHopping() &&
                          Hunt::getCurrentBehavior() == HuntBehavior::PATROL);
        Avatar::updateWalk(isWalkingNow || huntGrass, now);
    }
    
    // ==[ DRAW ROUTER ]== render based on mode (table dispatch)
    {
        const auto& desc = MODE_TABLE[(int)currentMode];
        if (desc.update) desc.update();
        if (desc.draw) desc.draw();
    }

    // ==[ MODE-SPECIFIC POST-UPDATE ]== special cases that can't be in the table
    if (currentMode == HamletMode::MENU) {
        // Rear cinematic done → particle teleport to hunt
        if (MenuPig::isRearCinematicDone()) {
            MenuPig::clearRearCinematic();
            int16_t srcX = PORTAL_CENTER_X, srcY = PORTAL_CENTER_Y;
            MenuPig::getPortalAnchor(srcX, srcY);
            int16_t dstX = PORTAL_CENTER_X, dstY = PORTAL_CENTER_Y;
            getPortalAnchorForMode(HamletMode::IDLE, dstX, dstY);  // hunt uses same pig pos
            Teleport::startCrossMode(Teleport::Context::MENU_TO_HUNT,
                (float)srcX, (float)srcY,
                (float)PORTAL_CENTER_X, (float)PORTAL_CENTER_Y,
                (float)dstX, (float)dstY, transitionStartNow(),
                Teleport::PigSilhouette::REAR,
                avatarTeleportSilhouette());
            MenuPig::triggerPortalJump();
        }
        // WD entry now uses cross-mode teleport (MENU_TO_WARDRIVE VOID dispatch)
    }

    // ==[ HOLD BAR ]== overlay drawn directly to display.
    // startWrite() batches SPI so pushSprite→overlay gap doesn't tear.
    SDStorage::setDisplayBusLocked(true);
    M5.Display.startWrite();
    Display::drawHoldOverlay();
    M5.Display.endWrite();
    SDStorage::setDisplayBusLocked(false);
    FrameBudget::notePhase("draw");
    if (serialScreenshotPending) {
        serialScreenshotPending = false;
        if (!Display::dumpScreenshotToSerial()) {
            Serial.println("[SHOT] failed");
        }
    }

    // Update paranoia mode (global deauth alert)
    if (!Teleport::isActive()) {
        updateParanoia(now);
    }

    // ==[ RECON ]== background WiFi scan + event dispatch (always active for terminal)
    DefensePipeline::update(now);
    processReconEvents(now);

    // Reclaim ESP-NOW after IPP releases WiFi. The foreground FLOCK screen
    // already ticks its transport through MODE_TABLE, so background work must
    // not run it a second time in the same frame.
    if (currentMode != HamletMode::NOWFLOCK &&
        !RadioPolicy::modeOwnsWifi(currentMode) && !WifiClient::ownsRadio()) {
        NowFlock::updateBackground();
    }

    // CoreS3 SE has one shared I2S bus for the ES7210 microphone and speaker.
    // BathMic takes it only while Pancetta is stationary in the tub; no mic is
    // initialized or sampled outside that scene contract.
    const bool bathMicEligible =
        currentMode == HamletMode::MENU && MenuPig::isBathMicDanceEligible();
    BathMic::update(bathMicEligible, now);

    if (!BathMic::isAudioBusReserved()) {
        // Update non-blocking SFX audio (must run every frame while speaker owns I2S).
        SFX::update();

        // ==[ NOIR JAZZ / BLADE RUNNER ]== global music pump — plays in ALL modes
        {
            // Auto-start on first frame after init
            if (!NoirJazz::isPlaying()) {
                NoirJazz::start();
            }

            // Mode selects music style: wardrive = Blade Runner, everything else = noir jazz
            if (currentMode == HamletMode::WARDRIVE) {
                NoirJazz::setMode(NoirJazz::Mode::BLADE_RUNNER);
            } else {
                NoirJazz::setMode(NoirJazz::Mode::NOIR_JAZZ);
            }

            // Mode-aware tension
            float t = 0.0f;
            if (currentMode == HamletMode::HUNT) {
                t = 0.6f;
            } else if (currentMode == HamletMode::WARDRIVE) {
                t = 0.35f;
            } else if (currentMode == HamletMode::SPECTRUM) {
                t = 0.4f;
            }
            NoirJazz::setTension(t);

            // Lead/sax active in wardrive + wherever menu_pig sets it
            if (currentMode == HamletMode::WARDRIVE) {
                NoirJazz::setSaxActive(true);
            }

            NoirJazz::update();

            // Haptic sync — only in noir jazz mode (Blade Runner has no percussive haptics)
            if (currentMode != HamletMode::WARDRIVE) {
                if (NoirJazz::consumeBackbeatHit()) {
                    Haptic::tick();
                }
                if (NoirJazz::consumeRainDrop()) {
                    Haptic::raindrop();
                }
            }
        }
    }

    // Phase 4: Check rib escape A+B hold during critical countdown
    Mood::checkRibEscapeInput();
    
    // Update mood system (phrase timeouts, momentum decay, idle phrases)
    // Pass isHunting to suppress idle phrases during active hunt
    bool isHunting = (currentMode == HamletMode::HUNT);  
    Mood::onIdle(isHunting);
}

// Keyboards and PIN pads paint over the whole panel, top bar included. The
// help overlay must not answer a tap on a key that happens to sit up there.
static bool isModalInputSurfaceActive() {
    if (currentMode == HamletMode::SETTINGS) {
        return SettingsMenu::isTextEditing() || SettingsMenu::isPinEditing();
    }
    if (currentMode == HamletMode::LOOT) {
        return LootMenu::isPinEntry();
    }
    // The composer is the same SoftKeyboard, and its 40px header puts the title
    // strip inside the TOP_BAR tap zone — without this, reaching for the top of
    // the keyboard opens the help wiki on top of a half-typed message.
    if (currentMode == HamletMode::MESH) {
        return MeshMenu::isComposing();
    }
    return false;
}

// ==[ TOUCH DISPATCHER ]== parallel input channel alongside buttons
static void updateTouch(uint32_t now) {
    if (isPowerWakeInputBlocked(now)) return;
    if (!Touch::hasEvent()) return;
    if (Teleport::isActive()) return;
    const auto& evt = Touch::getEvent();

    if (isScreenTouchLocked()) {
        if (!isLockableTouchEvent(evt)) {
            return;
        }

        if (isScreenUnlockGesture(evt)) {
            clearScreenTouchLock();
        } else {
            showScreenLockHintToast(now);
        }
        return;
    }

    if (isMenuInputLocked()) return;

    // rib escape via touch during critical scenes — TAP=A, SWIPE_LEFT=B
    if (Mood::isInCriticalScene()) {
        if (evt.gesture == Touch::Gesture::TAP) {
            Mood::handleRibEscapeButton('A');
        } else if (evt.gesture == Touch::Gesture::SWIPE_LEFT) {
            Mood::handleRibEscapeButton('B');
        }
        return;
    }
    auto g = evt.gesture;
    auto z = evt.zone;

    // Destructive confirmation is physical-B only. Freeze the hidden
    // selection and let swipe-left retain its established cancel meaning.
    if (currentMode == HamletMode::POWER_MENU && Display::isShowingSleepWarning()) {
        if (g == Touch::Gesture::SWIPE_LEFT) Display::declineSleepWarning();
        return;
    }

    // ==[ HELP WIKI ]== tap = next page (dismiss on last), swipe L = dismiss
    if (Display::isHelpOverlayActive()) {
        if (g == Touch::Gesture::SWIPE_LEFT) {
            Display::dismissHelpOverlay();
        } else {
            Display::advanceHelpPage();  // tap/any = next page, auto-dismiss on last
        }
        Haptic::tick();
        return;
    }
    // A held top strip is unclaimed by every normal mode. Keep tap-for-help and
    // give that spare gesture one global job: pocket-proof the touch surface.
    if (g == Touch::Gesture::LONG_PRESS && z == Touch::Zone::TOP_BAR &&
        !isModalInputSurfaceActive()) {
        setScreenTouchLock(true);
        return;
    }
    if (g == Touch::Gesture::TAP && z == Touch::Zone::TOP_BAR &&
        !isModalInputSurfaceActive()) {
        Display::showHelpOverlay(currentMode);
        Haptic::tick();
        return;
    }

    switch (currentMode) {
        case HamletMode::IDLE: {
            // ==[ PIG PET ]== tap pig for interaction
            if (g == Touch::Gesture::TAP && z == Touch::Zone::PIG_AREA) {
                if (now - lastPetTime >= 2000) {  // 2s cooldown
                    lastPetTime = now;
                    petCount++;

                    if (petCount <= 10) {
                        Avatar::cuteJump();
                        SFX::play(SFX::CUTE_JUMP);
                        Haptic::tick();
                        Mood::onPetted(petCount);
                    } else if (petCount <= 20) {
                        Haptic::tick();
                        Mood::onPetted(petCount);
                    }
                    // 20+ ignored
                }
                break;
            }
            // ==[ LONG-PRESS PIG ]== spin animation
            if (g == Touch::Gesture::LONG_PRESS && z == Touch::Zone::PIG_AREA) {
                if (now - lastPetTime >= 5000) {
                    lastPetTime = now;
                    Avatar::spin();
                    Mood::addMomentum(5);
                    Haptic::doubleTap();
                }
                break;
            }
            // ==[ SWIPE THEMES ]==
            if (g == Touch::Gesture::SWIPE_LEFT) {
                Display::nextTheme();
                Haptic::tick();
                break;
            }
            if (g == Touch::Gesture::SWIPE_RIGHT) {
                Display::prevTheme();
                Haptic::tick();
                break;
            }
            // ==[ TAP PLAYFIELD ]== enter menu (or confirm hunt prompt if toast active)
            if (g == Touch::Gesture::TAP && z == Touch::Zone::PLAYFIELD) {
                if (paranoia.huntPromptToastActive) {
                    // tap while "walk detected // hunt?" toast is showing → go straight to hunt
                    paranoia.huntPromptToastActive = false;
                    Display::showToast(nullptr);  // clear toast
                    SFX::play(SFX::HUNT_PATROL);
                    enterMode(HamletMode::HUNT);
                } else {
                    enterMode(HamletMode::MENU);
                }
                break;
            }
            break;
        }

        case HamletMode::MENU: {
            if (Menu::isMenuHidden()) {
                if (Menu::hasActiveEncounter()) {
                    if (g == Touch::Gesture::TAP && z == Touch::Zone::PLAYFIELD) {
                        Menu::select();
                        break;
                    }
                    if (g == Touch::Gesture::SWIPE_UP) {
                        Menu::prev();
                        break;
                    }
                    if (g == Touch::Gesture::SWIPE_DOWN) {
                        Menu::next();
                        break;
                    }
                    if (g == Touch::Gesture::SWIPE_LEFT) {
                        Menu::cancelEncounter();
                        break;
                    }
                    break;
                }
                // Hidden menu tap actions take priority over room cycling
                if (g == Touch::Gesture::TAP && z == Touch::Zone::PLAYFIELD) {
                    if (DefhogTerminal::isVisible()) {
                        // tap terminal overlay → enter full-screen DEFHOG4 mode
                        enterMode(HamletMode::DEFHOG4);
                        Haptic::tick();
                        break;
                    }
                }
                // roaming mode — swipe to cycle rooms
                if (g == Touch::Gesture::SWIPE_LEFT || g == Touch::Gesture::SWIPE_RIGHT) {
                    MenuPig::cycleRoom();
                    Haptic::tick();
                }
                break;
            }
            // menu visible
            if (g == Touch::Gesture::TAP && z == Touch::Zone::PLAYFIELD) {
                Menu::select();
                Haptic::tick();
                break;
            }
            if (g == Touch::Gesture::SWIPE_UP) {
                Menu::prev();
                Haptic::tick();
                break;
            }
            if (g == Touch::Gesture::SWIPE_DOWN) {
                Menu::next();
                Haptic::tick();
                break;
            }
            if (g == Touch::Gesture::SWIPE_LEFT) {
                handleBtnBack(true);  // back/exit
                break;
            }
            if (g == Touch::Gesture::SWIPE_RIGHT) {
                Menu::select();  // enter selected mode
                Haptic::tick();
                break;
            }
            break;
        }

        case HamletMode::HUNT: {
            if (g == Touch::Gesture::TAP) {
                handleBtnOK(false);  // pause hunt
                Haptic::tick();
                break;
            }
            if (g == Touch::Gesture::LONG_PRESS) {
                handleBtnOK(true);  // exit hunt
                break;
            }
            if (g == Touch::Gesture::SWIPE_LEFT) {
                handleBtnBack(true);  // exit hunt (back)
                break;
            }
            break;
        }

case HamletMode::SPECTRUM: {
            // Graph double-tap: 5GHz zoom, 2.4GHz MODEL overlay.
            static uint32_t lastGraphTap = 0;
            const bool graphTouch =
                evt.y >= UIMeasurements::Spectrum::kGraphTop &&
                evt.y <= UIMeasurements::Spectrum::kGraphBottom;
            if (g == Touch::Gesture::TAP) {
                if (Spectrum::isC5CarrierDetailActive()) {
                    lastGraphTap = 0;
                    executeSelectedC5SpectrumAction();
                    Haptic::tick();
                } else if (Spectrum::isClientDetailActive()) {
                    // The detail owns the popup surface, including the graph
                    // coordinates underneath it.
                    lastGraphTap = 0;
                    handleBtnOK(false);
                    Haptic::tick();
                } else if (graphTouch) {
                    // `now` is the dispatcher's frame clock — a second millis()
                    // read here just gave the double-tap window its own timebase.
                    if (lastGraphTap != 0 && now - lastGraphTap < 300) {
                        if (Spectrum::isShowing5GHz()) {
                            Spectrum::zoomToggle();
                        } else {
                            Spectrum::toggleModelOverlay();
                            Display::showToast(
                                Spectrum::isModelOverlayEnabled()
                                    ? "MODEL OVERLAY ON"
                                    : "MEASURED ONLY",
                                1200);
                        }
                        Haptic::tick();
                        lastGraphTap = 0;  // prevent triple-tap
                    } else {
                        // Arm the graph action without opening target detail.
                        // Selection remains a list tap / B action.
                        lastGraphTap = now;
                    }
                } else {
                    lastGraphTap = 0;
                    handleBtnOK(false);  // unified — respects paranoid/client/dial priority
                    Haptic::tick();
                }
                break;
            }
            if (g == Touch::Gesture::LONG_PRESS) {
                if (Spectrum::isClientDetailActive()) {
                    Spectrum::FtmRangeEvidence evidence{};
                    const bool started = Spectrum::startSelectedFtmRange();
                    Spectrum::getFtmRangeEvidence(evidence);
                    Display::showToast(
                        started
                            ? "FTM RANGE STARTED"
                            : (evidence.responderAdvertised
                                   ? "FTM BUSY/FAILED"
                                   : "FTM NOT ADVERTISED"),
                        1700);
                    Haptic::tick();
                } else {
                    handleBtnOK(true);  // long press = exit spectrum
                }
                break;
            }
            if (g == Touch::Gesture::SWIPE_LEFT) {
                if (Spectrum::isClientDetailActive()) {
                    Spectrum::closeClientDetail();
                    Haptic::tick();
                } else if (graphTouch) {
                    Spectrum::panLeft();
                    Haptic::tick();
                } else {
                    handleBtnBack(true);  // back navigation (client→spectrum→idle)
                }
                break;
            }
            if (g == Touch::Gesture::SWIPE_RIGHT) {
                if (Spectrum::isC5CarrierDetailActive()) {
                    executeSelectedC5SpectrumAction();
                } else if (Spectrum::isClientDetailActive()) {
                    const bool sent = Spectrum::pokeSelectedClient();
                    Display::showToast(
                        sent ? "ACTIVE POKE SENT" : "POKE NOT READY",
                        1500);
                } else if (graphTouch) {
                    // With no detail overlay, the graph owns horizontal swipes.
                    Spectrum::panRight();
                } else if (Spectrum::isShowing5GHz()) {
                    Spectrum::toggleBand();
                    Display::showToast("2.4GHz BAND", 1200);
                } else if (Spectrum::is5GHzEnabled()) {
                    Spectrum::toggleBand();
                    Display::showToast(Spectrum::isShowing5GHz() ? "5GHz BAND" : "2.4GHz BAND", 1200);
                }
                Haptic::tick();
                break;
            }
            // Up = prev, down = next — the same reading as every other list.
            if (g == Touch::Gesture::SWIPE_UP) {
                if (Spectrum::isC5CarrierDetailActive()) {
                    Spectrum::prevC5ArsenalCommand();
                } else {
                    Spectrum::prevNetwork();
                }
                Haptic::tick();
                break;
            }
            if (g == Touch::Gesture::SWIPE_DOWN) {
                if (Spectrum::isC5CarrierDetailActive()) {
                    Spectrum::nextC5ArsenalCommand();
                } else {
                    Spectrum::nextNetwork();
                }
                Haptic::tick();
                break;
            }
            break;
        }

        case HamletMode::XFER: {
            if (g == Touch::Gesture::SWIPE_LEFT) {
                enterMode(HamletMode::MENU);
                break;
            }
            break;
        }

        // ==[ LIST MENUS ]== unified swipe/tap dispatch
        case HamletMode::LOOT:
        case HamletMode::FEEDING:
        case HamletMode::MAIL:
        case HamletMode::SETTINGS:
        case HamletMode::POWER_MENU:
        case HamletMode::ABOUT: {
            if (g == Touch::Gesture::SWIPE_LEFT) {
                handleBtnBack(true);  // long B = back/exit
                break;
            }
            if (g == Touch::Gesture::TAP) {
                handleBtnOK(false);  // select
                Haptic::tick();
                break;
            }
            if (g == Touch::Gesture::SWIPE_UP) {
                // prev — direct per-mode call (inline C handles "next")
                switch (currentMode) {
                    case HamletMode::LOOT: LootMenu::prev(); break;
                    case HamletMode::FEEDING: FeedingMenu::prev(); break;
                    case HamletMode::MAIL: MailMenu::prev(); break;
                    case HamletMode::SETTINGS: SettingsMenu::prev(); break;
                    case HamletMode::POWER_MENU: Display::prevPowerOption(); break;
                    default: break;
                }
                Haptic::tick();
                break;
            }
            if (g == Touch::Gesture::SWIPE_DOWN) {
                // next — direct per-mode call
                switch (currentMode) {
                    case HamletMode::LOOT: LootMenu::next(); break;
                    case HamletMode::FEEDING: FeedingMenu::next(); break;
                    case HamletMode::MAIL: MailMenu::next(); break;
                    case HamletMode::SETTINGS: SettingsMenu::next(); break;
                    case HamletMode::POWER_MENU: Display::nextPowerOption(); break;
                    default: break;
                }
                Haptic::tick();
                break;
            }
            if (g == Touch::Gesture::SWIPE_RIGHT) {
                if (currentMode == HamletMode::LOOT) {
                    LootMenu::cycleTab();
                } else if (currentMode == HamletMode::FEEDING) {
                    FeedingMenu::select();
                } else {
                    break;
                }
                Haptic::tick();
                break;
            }
            break;
        }

        case HamletMode::WALK_STATS: {
            if (g == Touch::Gesture::SWIPE_LEFT) {
                handleBtnBack(true);  // exit to idle
                break;
            }
            break;
        }

        case HamletMode::NOWFLOCK: {
            if (g == Touch::Gesture::TAP) {
                handleBtnOK(false);  // request peer summaries
                Haptic::tick();
                break;
            }
            if (g == Touch::Gesture::SWIPE_RIGHT) {
                Display::flockNextPane();
                Haptic::tick();
                break;
            }
            if (g == Touch::Gesture::SWIPE_UP) {
                Display::flockScrollPeers(-1);
                Haptic::tick();
                break;
            }
            if (g == Touch::Gesture::SWIPE_DOWN) {
                Display::flockScrollPeers(1);
                Haptic::tick();
                break;
            }
            if (g == Touch::Gesture::SWIPE_LEFT) {
                handleBtnBack(false);  // exit
                break;
            }
            break;
        }

        case HamletMode::BLE_SCANNER: {
            if (g == Touch::Gesture::TAP) {
                handleBtnOK(false);  // select/track device
                Haptic::tick();
                break;
            }
            if (g == Touch::Gesture::LONG_PRESS) {
                handleBtnOK(true);   // exit to idle
                break;
            }
            if (g == Touch::Gesture::SWIPE_LEFT) {
                handleBtnBack(true);   // back (exit detail or exit mode)
                break;
            }
            if (g == Touch::Gesture::SWIPE_RIGHT) {
                BleScanner::toggleActiveScan();
                Haptic::tick();
                break;
            }
            // Up = prev, down = next — the same reading as every other list.
            if (g == Touch::Gesture::SWIPE_UP) {
                if (BleScanner::isTracking()) {
                    BleScanner::resetBearing();
                    Display::showToast("BEARING RESET", 1500);
                } else {
                    BleScanner::prevDevice();
                }
                Haptic::tick();
                break;
            }
            if (g == Touch::Gesture::SWIPE_DOWN) {
                if (BleScanner::isTracking()) {
                    BleScanner::addToWatchlist();
                } else {
                    BleScanner::nextDevice();
                }
                Haptic::tick();
                break;
            }
            break;
        }

        case HamletMode::DEFHOG4: {
            if (g == Touch::Gesture::SWIPE_LEFT) {
                Defhog4::prevPane();
                Haptic::tick();
                break;
            }
            if (g == Touch::Gesture::SWIPE_RIGHT) {
                Defhog4::nextPane();
                Haptic::tick();
                break;
            }
            if (g == Touch::Gesture::SWIPE_UP) {
                Defhog4::scrollUp();
                Haptic::tick();
                break;
            }
            if (g == Touch::Gesture::SWIPE_DOWN) {
                Defhog4::scrollDown();
                Haptic::tick();
                break;
            }
            if (g == Touch::Gesture::TAP) {
                const auto& te = Touch::getEvent();
                Defhog4::onTap(te.x, te.y);
                Haptic::tick();
                break;
            }
            if (g == Touch::Gesture::LONG_PRESS) {
                enterMode(HamletMode::MENU);
                break;
            }
            break;
        }

        case HamletMode::MESH: {
            // Same reading as every other vertical list. Up walks back into
            // the history because that is the direction the older lines went.
            if (g == Touch::Gesture::SWIPE_UP) {
                MeshMenu::scrollUp();
                Haptic::tick();
                break;
            }
            if (g == Touch::Gesture::SWIPE_DOWN) {
                MeshMenu::scrollDown();
                Haptic::tick();
                break;
            }
            if (g == Touch::Gesture::SWIPE_LEFT) {
                MeshMenu::handleBtnBack(true);  // back/exit
                Haptic::tick();
                break;
            }
            // The tabs are the visible way between the scrollback and the
            // roster; this is the one for a thumb already resting on the pane.
            if (g == Touch::Gesture::SWIPE_RIGHT) {
                MeshMenu::togglePane();
                Haptic::tick();
                break;
            }
            if (g == Touch::Gesture::TAP) {
                const auto& te = Touch::getEvent();
                MeshMenu::handleTouch(te.x, te.y);
                Haptic::tick();
                break;
            }
            break;
        }

        case HamletMode::C5MONSTER: {
            // Vertical list, vertical swipes — every other mode reads the same
            // way. Left kept its established back meaning instead of scrolling.
            if (g == Touch::Gesture::SWIPE_UP) {
                C5Menu::scrollUp();
                Haptic::tick();
                break;
            }
            if (g == Touch::Gesture::SWIPE_DOWN) {
                C5Menu::scrollDown();
                Haptic::tick();
                break;
            }
            if (g == Touch::Gesture::SWIPE_LEFT) {
                C5Menu::handleBtnBack(true);  // back/exit
                Haptic::tick();
                break;
            }
            if (g == Touch::Gesture::TAP) {
                const auto& te = Touch::getEvent();
                C5Menu::handleTouch(te.x, te.y);
                Haptic::tick();
                break;
            }
            if (g == Touch::Gesture::LONG_PRESS) {
                C5Menu::handleBtnBack(true);
                Haptic::tick();
                break;
            }
            break;
        }

        case HamletMode::WARDRIVE: {
            if (g == Touch::Gesture::SWIPE_LEFT ||
                g == Touch::Gesture::SWIPE_RIGHT) {
                WardriveTelemetry::toggle();
                Haptic::tick();
            }
            break;
        }

        default:
            break;
    }
}

// ==[ NAVIGATION VERBS ]== BtnA/BtnC own these switches, and the CardKB rides
// them so a key can only reach what a finger could already reach. The guards
// that precede each call site differ between the two paths and stay there.

// A roaming case card in MENU owns its own feedback; the nav tick would stack
// on top of it. Both verbs open with this, so no call site can move the cursor
// silently or buzz twice by remembering it separately.
static void navHaptic() {
    if (currentMode != HamletMode::MENU || !Menu::hasActiveEncounter()) {
        Haptic::tick();
    }
}

static void navPrev() {
    navHaptic();
    switch (currentMode) {
        case HamletMode::IDLE: Display::prevTheme(); break;
        case HamletMode::MENU: Menu::prev(); break;
        case HamletMode::HUNT: Display::toggleHuntOverlay(); break;
        case HamletMode::SPECTRUM:
            if (Spectrum::isClientDetailActive()) Spectrum::toggleApBearing();
            else Spectrum::prevNetwork();
            break;
        case HamletMode::LOOT:
            if (LootMenu::isPinEntry()) LootMenu::pinInput('A');
            else LootMenu::handleBtnA();
            break;
        case HamletMode::FEEDING: FeedingMenu::prev(); break;
        case HamletMode::MAIL: MailMenu::prev(); break;
        // A scrollback reads like a list whose cursor is the window: prev walks
        // toward the older lines, the same direction swipe up already goes.
        case HamletMode::MESH: MeshMenu::scrollUp(); break;
        case HamletMode::SETTINGS:
            if (SettingsMenu::isPinEditing()) SettingsMenu::pinInput('A');
            else SettingsMenu::prev();
            break;
        case HamletMode::POWER_MENU: Display::prevPowerOption(); break;
        case HamletMode::BLE_SCANNER:
            if (BleScanner::isTracking()) {
                if (BleScanner::canTriggerSound()) {
                    BleScanner::triggerAirTagSound();
                } else {
                    BleScanner::enumerateGattDevice();
                }
            } else {
                BleScanner::prevDevice();
            }
            break;
        case HamletMode::DEFHOG4: Defhog4::prevPane(); break;
        case HamletMode::NOWFLOCK: Display::flockBtnPrev(); break;
        default: break;
    }
}

static void navNext() {
    navHaptic();
    switch (currentMode) {
        case HamletMode::IDLE: Display::nextTheme(); break;
        case HamletMode::MENU: Menu::next(); break;
        case HamletMode::HUNT: Hunt::togglePause(); break;
        case HamletMode::SPECTRUM:
            if (Spectrum::isC5CarrierDetailActive()) {
                Spectrum::nextC5ArsenalCommand();
            } else if (Spectrum::isClientDetailActive()) {
                Spectrum::toggleApBearing();
            }
            else Spectrum::nextNetwork();
            break;
        case HamletMode::LOOT:
            if (LootMenu::isPinEntry()) LootMenu::pinInput('C');
            else if (LootMenu::isNukeConfirm()) LootMenu::cancelNuke();
            else if (LootMenu::isInDetailView()) LootMenu::back();
            else LootMenu::next();
            break;
        case HamletMode::FEEDING: FeedingMenu::next(); break;
        case HamletMode::MAIL: MailMenu::next(); break;
        case HamletMode::MESH: MeshMenu::scrollDown(); break;
        case HamletMode::SETTINGS:
            if (SettingsMenu::isPinEditing()) SettingsMenu::pinInput('C');
            else SettingsMenu::next();
            break;
        case HamletMode::POWER_MENU: Display::nextPowerOption(); break;
        // A tail in progress owns the screen, and the detail view reads the
        // tracked device rather than the list cursor. Moving that cursor here
        // is invisible until exitDetail() lands the user on a stranger's row,
        // so tracking freezes it — matching navPrev and the [C+]BACK hint bar.
        case HamletMode::BLE_SCANNER:
            if (!BleScanner::isTracking()) BleScanner::nextDevice();
            break;
        case HamletMode::DEFHOG4: Defhog4::nextPane(); break;
        case HamletMode::NOWFLOCK: Display::flockBtnNext(); break;
        case HamletMode::WARDRIVE: startWardriveExitTeleport(); break;
        default: break;
    }
}

static void updateButtons(uint32_t now) {
    // ==[ LAYOUT: BtnA=prev(left), BtnB=OK/select(mid), BtnC=next+back(right) ]==

    if (isPowerWakeInputBlocked(now)) {
        btn.okHandled = M5.BtnB.isPressed();
        btn.backHandled = M5.BtnC.isPressed();
        btn.okDownTime = now;
        btn.backDownTime = now;
        btn.holdProgress = 0.0f;
        btn.aHoldProgress = 0.0f;
        btn.aRawHoldProgress = 0.0f;
        return;
    }

    // The physical PMIC key opens the safe menu; it never dispatches a power
    // action directly. A second click backs out/declines.
    if (M5.BtnPWR.wasClicked() && !Teleport::isActive()) {
        Display::resetDimTimer();
        if (currentMode == HamletMode::POWER_MENU) {
            if (Display::isShowingSleepWarning()) Display::declineSleepWarning();
            else enterMode(HamletMode::IDLE);
        } else {
            enterMode(HamletMode::POWER_MENU);
        }
        return;
    }

    if (isMenuInputLocked()) {
        if (M5.BtnA.wasPressed() || M5.BtnB.wasPressed() || M5.BtnC.wasPressed()) {
            Display::resetDimTimer();
        }
        btn.okHandled = M5.BtnB.isPressed();
        btn.backHandled = M5.BtnC.isPressed();
        btn.okDownTime = now;
        btn.backDownTime = now;
        btn.holdProgress = 0.0f;
        btn.aHoldProgress = 0.0f;
        btn.aRawHoldProgress = 0.0f;
        return;
    }

    // ==[ HELP WIKI ]== any button while help active: advance/dismiss
    if (Display::isHelpOverlayActive()) {
        if (M5.BtnA.wasPressed() || M5.BtnB.wasPressed() || M5.BtnC.wasPressed()) {
            Display::resetDimTimer();
            Display::advanceHelpPage();
            Haptic::tick();
        }
        // consume all button state while help is visible
        btn.okHandled = M5.BtnB.isPressed();
        btn.backHandled = M5.BtnC.isPressed();
        return;
    }

    // Button A (left) — prev navigation, no long press
    if (M5.BtnA.wasPressed()) {
        Display::resetDimTimer();
        if (Teleport::isActive()) return;
        navPrev();
    }

    // Button B (middle) — OK/select with long press
    if (M5.BtnB.wasPressed()) {
        btn.okDownTime = now;
        btn.okHandled = false;
        Display::resetDimTimer();
    }

    if (M5.BtnB.isPressed() && !btn.okHandled) {
        uint32_t held = now - btn.okDownTime;
        if (held >= LONG_PRESS_MS) {
            handleBtnOK(true);  // Long OK = long action (enter mode, exit hunt, etc.)
            btn.okHandled = true;
        }
    }

    if (M5.BtnB.wasReleased() && !btn.okHandled) {
        handleBtnOK(false);  // Short OK = select/action
    }

    // Button C (right) — next (short) + back/exit (long)
    if (M5.BtnC.wasPressed()) {
        btn.backDownTime = now;
        btn.backHandled = false;
        Display::resetDimTimer();
    }

    if (M5.BtnC.isPressed() && !btn.backHandled) {
        uint32_t held = now - btn.backDownTime;
        if (held >= BACK_PRESS_MS) {
            handleBtnBack(true);  // Long right = back/exit
            btn.backHandled = true;
        }
    }

    if (M5.BtnC.wasReleased() && !btn.backHandled) {
        // Short right = next. The teleport verdict comes before the tick, the
        // way BtnA above already orders it: a suppressed press that still buzzes
        // reads as a navigation the screen then refuses to show.
        if (Teleport::isActive()) return;
        navNext();
    }

    // ==[ HOLD PROGRESS ]== feed longest active hold to display
    float maxProgress = 0.0f;
    float maxAProgress = 0.0f;
    float maxARawProgress = 0.0f;
    if (M5.BtnB.isPressed() && !btn.okHandled) {
        uint32_t held = now - btn.okDownTime;
        float p = (float)held / (float)LONG_PRESS_MS;
        if (p > maxProgress) maxProgress = p;
        if (p > maxARawProgress) maxARawProgress = p;
        if (held > PORTAL_CHARGE_DELAY_MS && LONG_PRESS_MS > PORTAL_CHARGE_DELAY_MS) {
            float ap = (float)(held - PORTAL_CHARGE_DELAY_MS) /
                       (float)(LONG_PRESS_MS - PORTAL_CHARGE_DELAY_MS);
            if (ap > maxAProgress) maxAProgress = ap;
        }
    }
    if (M5.BtnC.isPressed() && !btn.backHandled) {
        float p = (float)(now - btn.backDownTime) / BACK_PRESS_MS;
        if (p > maxProgress) maxProgress = p;
    }
    if (maxProgress > 1.0f) maxProgress = 1.0f;
    if (maxAProgress > 1.0f) maxAProgress = 1.0f;
    if (maxARawProgress > 1.0f) maxARawProgress = 1.0f;
    btn.holdProgress = maxProgress;
    btn.aHoldProgress = maxAProgress;
    btn.aRawHoldProgress = maxARawProgress;
}

// A LoRa message can arrive minutes after anyone last looked at the device, and
// the mesh screen is not where the pig usually lives. Announce it wherever the
// user actually is; the MESH screen is its own notification and drains the
// counter itself, so it deliberately says nothing.
//
// The toast drains the *unannounced* half, not the unread one. A toast is
// transient and a message can land while the device is in a pocket, so the
// unread count has to survive being announced — it is what feeds the standing
// badge in the top bar.
static void announceMeshArrival() {
    if (currentMode == HamletMode::MESH) return;
    if (Mesh::consumeUnannounced() == 0) return;

    // Total waiting, not the size of this batch, so the toast and the badge it
    // leaves behind never disagree about how much is unread.
    const uint8_t count = Mesh::peekUnread();

    // The ring can end in one of our queued messages, so walk backward to the
    // newest arrival instead of assuming the last slot is inbound. This is a
    // read-only peek: the standing unread badge survives the notification and
    // still clears only when the user opens M3SH T4LK.
    const Mesh::Message* latest = nullptr;
    for (uint8_t i = Mesh::getMessageCount(); i > 0; --i) {
        const Mesh::Message& candidate = Mesh::getMessage((uint8_t)(i - 1));
        if (!candidate.outgoing) {
            latest = &candidate;
            break;
        }
    }

    char toast[96];
    if (latest) {
        MeshNotification::build(toast, sizeof(toast), latest->sender,
                                latest->body, count, latest->direct);
    } else if (count > 1) {
        snprintf(toast, sizeof(toast), "M3SH // %u W41T1NG", (unsigned)count);
    } else {
        snprintf(toast, sizeof(toast), "M3SH MSG");
    }
    Display::showToast(toast, 3200);
    SFX::play(SFX::RING);
}

// Hot-plug is the normal case for a Grove unit: say so, because nothing else on
// screen changes when a keypad appears.
static void announceKeypadHotplug() {
    bool attached = false;
    if (!CardKB::consumeAttachEvent(attached)) return;
    Display::showToast(attached ? "CARDKB LINKED" : "CARDKB UNPLUGGED", 1400);
    SFX::click();
}

// ==[ KEYPAD DISPATCH ]== a Unit CardKB on Grove Port A speaks the same three
// verbs the hardware buttons do, so it opens no door a finger cannot:
//   Up / Left     -> prev            Down / Right -> next
//   Enter         -> select (short B)  Tab        -> the hold-B action
//   Esc / Backspace -> back/exit (the hold-C action)
// The hold actions arrive as their own keys because a keypad has no hold: the
// unit reports a press, never a duration.
static void updateKeypad(uint32_t now) {
    if (!CardKB::available()) return;

    // An open text field owns every byte. Letters typed into a WiFi password
    // must never also drive the menu underneath it.
    if (SoftKeyboard::isActive()) return;

    if (isPowerWakeInputBlocked(now) || isMenuInputLocked() || Teleport::isActive()) {
        CardKB::flush();
        Display::resetDimTimer();
        return;
    }

    while (CardKB::available()) {
        const uint8_t key = CardKB::read();
        Display::resetDimTimer();

        if (Display::isHelpOverlayActive()) {
            Display::advanceHelpPage();
            Haptic::tick();
            continue;
        }

        // The C5 list is the one screen the buttons never learned to scroll —
        // it answers to vertical swipes instead. The cursor keys take that
        // route rather than the button route, so the keypad still reaches only
        // what a finger reaches.
        if (currentMode == HamletMode::C5MONSTER &&
            (key == CardKB::KEY_UP || key == CardKB::KEY_DOWN)) {
            if (key == CardKB::KEY_UP) C5Menu::scrollUp();
            else C5Menu::scrollDown();
            Haptic::tick();
            continue;
        }

        // MESH needs no shortcut of its own: navPrev/navNext already route to
        // MeshMenu::scrollUp/scrollDown and handleBtnOK already routes Enter to
        // the composer. Intercepting them here would only re-do that — and
        // Enter would skip the rib-escape and critical-scene gates at the head
        // of handleBtnOK, which every other mode's Enter respects.

        switch (key) {
            case CardKB::KEY_UP:
            case CardKB::KEY_LEFT:
                navPrev();
                break;
            case CardKB::KEY_DOWN:
            case CardKB::KEY_RIGHT:
                navNext();
                break;
            case CardKB::KEY_ENTER:
                handleBtnOK(false);
                break;
            case CardKB::KEY_TAB:
                handleBtnOK(true);
                break;
            case CardKB::KEY_ESC:
            case CardKB::KEY_BACKSPACE:
                handleBtnBack(true);
                break;
            default:
                break;
        }

        // That key may have opened a text field or started a transition. The
        // rest of the burst belongs to whoever owns input now, not to this loop.
        if (SoftKeyboard::isActive() || Teleport::isActive() || isMenuInputLocked()) break;
    }
}

static void executeSelectedC5SpectrumAction() {
    const char* label = Spectrum::getSelectedC5ArsenalCommandLabel();
    const bool targeted = Spectrum::isSelectedC5ArsenalCommandTargeted();
    const bool targetReady = Spectrum::hasC5TargetSelection();
    const bool available =
        Spectrum::isSelectedC5ArsenalCommandAvailable();
    const bool executed = Spectrum::executeSelectedC5ArsenalCommand();

    char toast[36];
    if (executed) {
        snprintf(toast, sizeof(toast), "%s OK",
                 label && label[0] ? label : "C5 ACTION");
    } else if (targeted && !targetReady) {
        snprintf(toast, sizeof(toast), "TARGET LEFT SNAP");
    } else if (!C5Monster::isConnected()) {
        snprintf(toast, sizeof(toast), "C5 OFFLINE");
    } else if (C5Monster::isBusy() ||
               C5Monster::hasActiveOperation()) {
        snprintf(toast, sizeof(toast), "C5 RADIO BUSY");
    } else if (!available) {
        snprintf(toast, sizeof(toast), "ACTION LOCKED");
    } else {
        snprintf(toast, sizeof(toast), "ACTION FAILED");
    }
    Display::showToast(toast, 1400);
}

void handleBtnOK(bool longPress) {
    if (Teleport::isActive()) {
        return;
    }
    if (isMenuInputLocked()) {
        return;
    }

    // PHASE 4: rib escape sequence takes priority
    if (Mood::handleRibEscapeButton('A')) {
        return;  // escape in progress or complete
    }
    
    // PHASE D: block menu during critical scene
    if (Mood::isInCriticalScene()) {
        return;  // no buttons during tears in rain
    }
    
    switch (currentMode) {
        case HamletMode::IDLE:
            if (longPress) {
                // particle teleport: pig decomposes on IDLE, reassembles in MENU
                int16_t srcX = PORTAL_CENTER_X, srcY = PORTAL_CENTER_Y;
                getPortalAnchorForMode(HamletMode::IDLE, srcX, srcY);
                float dstCX = (float)UIMeasurements::MenuPigLayout::kHelperPigX + 36.0f;
                float dstCY = (float)UIMeasurements::MenuPigLayout::kHelperPigY + 21.0f;
                Teleport::startCrossMode(Teleport::Context::IDLE_TO_MENU,
                    (float)srcX, (float)srcY,
                    (float)PORTAL_CENTER_X, (float)PORTAL_CENTER_Y,
                    dstCX, dstCY, transitionStartNow(),
                    avatarTeleportSilhouette(),
                    Teleport::PigSilhouette::SIDE_RIGHT);
                Avatar::cuteJump();
            } else {
                // instant menu entry (no transition anim)
                enterMode(HamletMode::MENU);
            }
            break;
            
        case HamletMode::MENU:
            // Roaming cases own B even while the menu chrome is hidden.
            // Every case-card page explicitly advertises its B action.
            if (Menu::hasActiveEncounter()) {
                Menu::select();
            } else if (!Menu::isMenuHidden()) {
                Menu::select();
            } else if (longPress) {
                if (MenuPig::isHousePortalReady()) {
                    MenuPig::startRearCinematic();
                } else {
                    Display::showToast("portal busy", 1200);
                    SFX::click();
                    Haptic::tick();
                }
            } else if (!longPress) {
                MenuPig::cycleRoom();
            }
            break;
            
        case HamletMode::HUNT:
            if (longPress) {
                // Stop hunting — enterMode handles exitCurrentMode + power profile
                // (Hunt::stop() already calls Mood::onHuntStop() internally)
                enterMode(HamletMode::IDLE);
            } else {
                Hunt::togglePause();
            }
            break;
            
        case HamletMode::SPECTRUM:
            if (longPress) {
                if (Spectrum::isClientDetailActive()) {
                    Spectrum::closeClientDetail();
                } else if (Spectrum::isShowing5GHz()) {
                    Spectrum::toggleBand();
                    Display::showToast("2.4GHz BAND", 1200);
                } else {
                    // Exit spectrum — enterMode handles teardown + power profile.
                    enterMode(HamletMode::IDLE);
                }
            } else {
                // Paranoid Swine detail view takes priority
                if (Spectrum::isParanoidDetailActive()) {
                    Spectrum::toggleParanoidDetail();  // Exit detail view
                } else if (Spectrum::isAttackActive()) {
                    Spectrum::toggleParanoidDetail();  // Enter detail view
                }
            // C5 AP detail owns B as its visible action-menu RUN key.
            else if (Spectrum::isC5CarrierDetailActive()) {
                executeSelectedC5SpectrumAction();
            } else if (Spectrum::isClientDetailActive()) {
                Spectrum::closeClientDetail();
            } else if (Spectrum::isInClientMode()) {
                Spectrum::toggleClientDetail();
            } else if (Spectrum::isShowing5GHz()) {
                Spectrum::selectNetwork();
            }
            // DIAL Mode: toggle channel lock (when already locked, A unlocks)
            // When unlocked, A selects network as usual
            else if (Spectrum::isDialMode() && Spectrum::isDialLocked()) {
                Spectrum::toggleDialLock();  // unlock
                }
                // Select network (enter client mode) - also locks dial if in dial mode
                else {
                    if (Spectrum::isDialMode()) {
                        Spectrum::toggleDialLock();  // lock channel when selecting
                    }
                    Spectrum::selectNetwork();
                }
            }
            break;
            
        case HamletMode::LOOT:
            if (LootMenu::isPinEntry()) {
                if (!longPress) LootMenu::pinInput('B');
            } else if (LootMenu::isNukeConfirm()) {
                // Require HOLD to commit nuke; short tap is a no-op by design
                if (longPress) LootMenu::confirmNuke();
            } else if (longPress) {
                // Long B = context action: upload/download for current tab
                // Modal (incl. result banner) must be dismissed first.
                if (!LootMenu::isUploadModalActive()) {
                    LootMenu::triggerAction();
                }
            } else {
                // Short press = view/next (list tab only)
                if (LootMenu::isInDetailView()) {
                    LootMenu::next();  // Already viewing - advance to next
                } else {
                    LootMenu::select();  // Not viewing - enter detail view
                }
            }
            break;
            
        case HamletMode::FEEDING:
            // Short B cycles shelves; long B burns the highlighted exhibit.
            // Consumption is destructive for the session, so it wants the
            // deliberate press rather than the one used for navigation.
            if (longPress) FeedingMenu::consumeSelected();
            else FeedingMenu::select();
            break;

        case HamletMode::MAIL:
            // Short B opens the letter or advances the beat; the case card owns
            // every phase transition once a file is open.
            if (!longPress) MailMenu::select();
            break;
        
        case HamletMode::SETTINGS:
            if (SettingsMenu::isTextEditing()) break;  // keyboard owns input
            if (SettingsMenu::isPinEditing()) {
                if (!longPress) SettingsMenu::pinInput('B');
                break;
            }
            // Check for legal warning toast
            if (SettingsMenu::isShowingWarning()) {
                // Destructive warnings (e.g. Reincarnate) require HOLD to commit.
                if (SettingsMenu::isDestructiveWarning() && !longPress) break;
                SettingsMenu::acceptWarning();  // B = accept/yes
            } else {
                // Toggle setting
                SettingsMenu::select();
            }
            break;
            
        case HamletMode::NOWFLOCK:
            NowFlock::requestPeerSummaries();
            Display::showToast("N0W F0CK P33R_R3Q", 1200);
            break;
            
        case HamletMode::POWER_MENU:
            // Check if warning toast is active
            if (Display::isShowingSleepWarning()) {
                Display::acceptSleepWarning();  // physical B = accept
                // Now execute power action
                handlePowerAction(true);
            } else {
                // Execute power action (may trigger warning)
                handlePowerAction();
            }
            break;

        case HamletMode::WARDRIVE: {
            if (Teleport::isActive()) break;
            if (longPress) {
                startWardriveExitTeleport();
            } else {
                Wardrive::togglePause();
            }
            break;
        }

        case HamletMode::BLE_SCANNER:
            if (longPress) {
                if (BleScanner::isTracking()) {
                    BleScanner::resetBearing();
                    Display::showToast("BEARING RESET", 1500);
                } else {
                    enterMode(HamletMode::IDLE);
                }
            } else {
                BleScanner::selectDevice();  // toggle Geiger tracking
            }
            break;

        case HamletMode::DEFHOG4:
            if (longPress) {
                enterMode(HamletMode::MENU);
            } else {
                Defhog4::action();
            }
            break;

        case HamletMode::MESH:
            MeshMenu::handleBtnOK(longPress);
            break;

        default:
            break;
    }
}

void handleBtnBack(bool longPress) {
    if (Teleport::isActive()) {
        return;
    }
    if (isMenuInputLocked()) {
        return;
    }

    // PHASE 4: rib escape sequence takes priority
    if (Mood::handleRibEscapeButton('B')) {
        return;  // escape in progress or complete
    }
    
    if (longPress) {
        // Long press B = back/exit
        switch (currentMode) {
            case HamletMode::IDLE:
                // Power menu
                enterMode(HamletMode::POWER_MENU);
                break;

            case HamletMode::MENU:
                if (Menu::hasActiveEncounter()) {
                    Menu::cancelEncounter();
                } else if (!Menu::isMenuHidden() && Menu::back()) {
                    // A grouped drawer owns the first back action. The next
                    // one, from the root, exits to the home scene as before.
                } else {
                    enterMode(HamletMode::IDLE);
                }
                break;
            case HamletMode::MAIL:
                // An open case card takes the back gesture first: bail out of
                // the file, keep the letter, stay in the inbox.
                if (MailMenu::hasOpenCase()) {
                    MailMenu::back();
                } else {
                    enterMode(HamletMode::IDLE);
                }
                break;

            case HamletMode::LOOT:
            case HamletMode::FEEDING:
            case HamletMode::WALK_STATS:
            case HamletMode::ABOUT:
                enterMode(HamletMode::IDLE);
                break;

            case HamletMode::POWER_MENU:
                if (Display::isShowingSleepWarning()) {
                    Display::declineSleepWarning();
                } else {
                    enterMode(HamletMode::IDLE);
                }
                break;

            case HamletMode::XFER:
                enterMode(HamletMode::MENU);
                break;

            case HamletMode::C5MONSTER:
                C5Menu::handleBtnBack(true);
                break;

            case HamletMode::MESH:
                MeshMenu::handleBtnBack(true);
                break;

            case HamletMode::WEBCONFIG:
                // Exit config portal and return to settings
                Config::save();  // flush portal changes to NVS
                enterMode(HamletMode::SETTINGS);
                break;

            case HamletMode::WARDRIVE: {
                startWardriveExitTeleport();
                break;
            }

            case HamletMode::SETTINGS:
                if (SettingsMenu::isTextEditing()) break;  // keyboard owns input
                // Try closing modal first, if no modal then exit
                if (!SettingsMenu::closeModal()) {
                    Config::save();  // flush pending settings to NVS before leaving
                    Capture::saveJournal();  // flush dirty sync flags to SD
                    enterMode(HamletMode::IDLE);
                }
                break;
                
            case HamletMode::NOWFLOCK:
                enterMode(HamletMode::IDLE);
                break;

            case HamletMode::HUNT:
                // Exit hunt — Hunt::stop() calls Mood::onHuntStop() internally
                Capture::saveJournal();  // flush dirty sync flags to SD
                enterMode(HamletMode::IDLE);
                break;

            case HamletMode::BLE_SCANNER:
                if (BleScanner::isTracking()) {
                    BleScanner::exitDetail();  // stop Geiger, back to list
                } else {
                    enterMode(HamletMode::IDLE);
                }
                break;

            case HamletMode::DEFHOG4:
                enterMode(HamletMode::MENU);
                break;

            case HamletMode::SPECTRUM:
                if (Spectrum::isClientDetailActive()) {
                    // Long B in GIGER scanner → close detail, back to client monitor
                    Spectrum::closeClientDetail();
                } else if (Spectrum::isInClientMode()) {
                    // Long B in client monitor → exit client mode, back to network list
                    Spectrum::exitClientMode();
                } else if (Spectrum::isShowing5GHz()) {
                    Spectrum::toggleBand();
                    Display::showToast("2.4GHz BAND", 1200);
                } else {
                    // Long B in network list → exit spectrum, back to idle
                    enterMode(HamletMode::IDLE);
                }
                break;
        }
    } else {
        // ==[ SHORT B ]== prev actions (BtnC handles next)
        switch (currentMode) {
            case HamletMode::IDLE:
                break;

            case HamletMode::MENU:
                Menu::prev();
                break;

            case HamletMode::HUNT:
                // toggle hunt overlay
                Display::toggleHuntOverlay();
                break;

            case HamletMode::SPECTRUM:
                if (Spectrum::isC5CarrierDetailActive()) {
                    Spectrum::prevC5ArsenalCommand();
                } else if (Spectrum::isClientDetailActive()) {
                    // Toggle AP bearing lock in client detail
                    Spectrum::toggleApBearing();
                } else {
                    Spectrum::prevNetwork();
                }
                break;

            case HamletMode::LOOT:
                if (LootMenu::isPinEntry() || LootMenu::isNukeConfirm()) break;
                if (LootMenu::isUploadModalActive()) {
                    LootMenu::cancelUpload();  // cancel in-flight or dismiss result banner
                } else if (LootMenu::isInDetailView()) {
                    LootMenu::back();
                } else {
                    LootMenu::prev();
                }
                break;

            case HamletMode::FEEDING:
                FeedingMenu::prev();
                break;

            case HamletMode::MAIL:
                MailMenu::prev();
                break;

            case HamletMode::SETTINGS:
                if (SettingsMenu::isTextEditing()) break;  // keyboard owns input
                if (SettingsMenu::isPinEditing()) break;   // PIN entry blocks nav
                // legal toast gets first refusal
                if (SettingsMenu::isShowingWarning()) {
                    SettingsMenu::declineWarning();  // decline/nah
                } else {
                    SettingsMenu::prev();
                }
                break;

            case HamletMode::POWER_MENU:
                // sleep warning toast overrides cycling
                if (Display::isShowingSleepWarning()) {
                    Display::declineSleepWarning();  // decline
                } else {
                    Display::prevPowerOption();
                }
                break;
                
            case HamletMode::NOWFLOCK:
                enterMode(HamletMode::IDLE);
                break;

            case HamletMode::WEBCONFIG:
                // short B = exit config portal
                enterMode(HamletMode::SETTINGS);
                break;

            case HamletMode::WARDRIVE: {
                startWardriveExitTeleport();
                break;
            }

            case HamletMode::BLE_SCANNER:
                // Same freeze as navNext: the tail, not the list, owns the view.
                if (!BleScanner::isTracking()) BleScanner::prevDevice();
                break;

            case HamletMode::DEFHOG4:
                enterMode(HamletMode::MENU);
                break;

            case HamletMode::XFER:
                enterMode(HamletMode::MENU);
                break;

            case HamletMode::C5MONSTER:
                C5Menu::handleBtnBack(false);
                break;

            case HamletMode::MESH:
                MeshMenu::handleBtnBack(false);
                break;

            default:
                break;
        }
    }
}

HamletMode getMode() {
    return currentMode;
}

void setMode(HamletMode mode) {
    currentMode = mode;
    syncScreenTouchLockEffects(mode);
    // IDLE is the only scene that draws the avatar this hitbox describes.
    // Everywhere else the rect was a phantom sitting over the lower playfield,
    // and PIG_AREA taps there resolve to nothing.
    Touch::setPigHitTestEnabled(mode == HamletMode::IDLE);
    // A finger still down from the old mode must not deliver its release here.
    Touch::reset();
    lastModeChangeTime = millis();
}

// ==[ PARANOIA MODE PUBLIC API ]==

void triggerGlobalDeauth(int8_t rssi, uint8_t channel) {
    // callbacks raise this when deauth spotted (WiFi task, core 0)
    paranoia.deauthRSSI.store(rssi, std::memory_order_relaxed);
    paranoia.deauthChannel.store(channel, std::memory_order_relaxed);
    paranoia.deauthDetected.store(true, std::memory_order_release);  // flag last — acquire-load sees rssi/channel
}

bool isParanoiaToastActive() {
    return paranoia.toastActive;
}

int8_t getParanoiaRSSI() {
    return paranoia.deauthRSSI.load(std::memory_order_relaxed);
}

uint8_t getParanoiaChannel() {
    return paranoia.deauthChannel.load(std::memory_order_relaxed);
}

// can we jump to Spectrum? Passive FLOCK does not own the radio.
static bool canSwitchToParanoidSpectrum() {
    if (currentMode == HamletMode::WARDRIVE || isMenuCinematicActive()) {
        return false;
    }
    // already there? stay put
    if (currentMode == HamletMode::SPECTRUM) {
        return false;
    }
    return true;
}

// paranoia state machine tick
static void updateParanoia(uint32_t now) {
    if (!Config::getParanoiaEnabled()) {
        // paranoia off -> clear toast/flags
        if (paranoia.toastActive) {
            paranoia.toastActive = false;
            paranoia.deauthDetected.store(false, std::memory_order_relaxed);
        }
        return;
    }

    // already watching Spectrum? skip toast
    if (currentMode == HamletMode::SPECTRUM) {
        paranoia.toastActive = false;
        paranoia.deauthDetected.store(false, std::memory_order_relaxed);
        return;
    }

    // check for fresh deauth
    if (paranoia.deauthDetected.load(std::memory_order_acquire) && !paranoia.toastActive) {
        // new deauth -> raise toast
        paranoia.toastActive = true;
        paranoia.toastStart = now;
        paranoia.deauthDetected.store(false, std::memory_order_relaxed);  // consume flag
        // Morse DEAUTH only once per session; later alerts are visual only
        if (!paranoia.morsePlayedThisSession) {
            SFX::play(SFX::PARANOIA_ALERT);
            paranoia.morsePlayedThisSession = true;
        }

        // nudge screen if dimmed
        if (Config::getAlertWake()) {
            Display::wakeFromDim();
        }
    }

    // drive active toast
    if (paranoia.toastActive) {
        // check for button press
        if (canSwitchToParanoidSpectrum() && M5.BtnB.wasPressed()) {
            SFX::stop();  // kill Morse if still playing
            paranoia.toastActive = false;

            // switch to Spectrum with detail view
            enterMode(HamletMode::SPECTRUM);
            Spectrum::toggleParanoidDetail();  // open detail view
            btn.okHandled = true;
        }

        // toast timeout
        if (now - paranoia.toastStart > PARANOIA_TOAST_TIMEOUT) {
            paranoia.toastActive = false;
        }

        // refresh timestamp if another deauth hits during toast
        if (paranoia.deauthDetected.load(std::memory_order_acquire)) {
            paranoia.deauthDetected.store(false, std::memory_order_relaxed);
            paranoia.toastStart = now;  // reset timeout
        }
    }
}

// ==[ IPP EVENT DISPATCH ]== route recon events to mood/toast/sfx
static void processReconEvents(uint32_t now) {
    static uint8_t lastFingerprintBSSID[6] = {0};
    static uint8_t lastSeqBSSID[6] = {0};
    static uint32_t lastFingerprintSeen = 0;
    static uint32_t lastSeqSeen = 0;
    static constexpr uint32_t FP_SEQ_CORRELATION_MS = 60000;
    // ==[ CHALLENGE TRACKING ]== unique tracker types seen (bitmask)
    static uint32_t seenTrackerTypes = 0;

    auto countBits32 = [](uint32_t v) -> uint8_t {
        uint8_t count = 0;
        while (v) { count += (uint8_t)(v & 1U); v >>= 1; }
        return count;
    };

    auto awardReconXP = [&](uint16_t amount) {
        if (!Config::isSessionActive()) return;
        Config::addXP(amount, Config::RewardSource::RECON);
    };

    auto checkBleCatalogTrophy = [&]() {
        if (DefensePipeline::snapshot().getTotalBLEDevicesSeen() >= 25) {
            Achievements::tryUnlock(Achievement::PIG_EARS);
        }
    };

    while (DefensePipeline::hasEvent()) {
        Defense::DefenseEventData ev = DefensePipeline::popEvent();
        bool fpToSeqCorrelated = false;
        bool seqToFpCorrelated = false;
        if (ev.bssid[0] || ev.bssid[1] || ev.bssid[2] || ev.bssid[3] || ev.bssid[4] || ev.bssid[5]) {
            fpToSeqCorrelated = (lastSeqSeen > 0) &&
                                (now - lastSeqSeen <= FP_SEQ_CORRELATION_MS) &&
                                (memcmp(lastSeqBSSID, ev.bssid, 6) == 0);
            seqToFpCorrelated = (lastFingerprintSeen > 0) &&
                                (now - lastFingerprintSeen <= FP_SEQ_CORRELATION_MS) &&
                                (memcmp(lastFingerprintBSSID, ev.bssid, 6) == 0);
        }

        switch (ev.event) {
            case Recon::ReconEvent::TRACKER_NEW: {
                bool isThreat = (ev.threatType != Recon::ThreatType::IBEACON &&
                                 ev.threatType != Recon::ThreatType::EDDYSTONE);
                Mood::onTrackerDetected((uint8_t)ev.threatType, ev.rssi, ev.detail);
                // per-type cooldown: max 1 toast+SFX per threat type per 2min
                static uint32_t lastTypeAlert[Recon::THREAT_TYPE_COUNT] = {0};
                int ti = (int)ev.threatType;
                bool cooldownOk = (ti >= 0 && ti < Recon::THREAT_TYPE_COUNT && now - lastTypeAlert[ti] >= 120000);
                if (isThreat && cooldownOk) {
                    Display::showAlertToast(NoirNarrator::getTrackerPhrase(ev.threatType), 4000);
                    SFX::play(SFX::RECON_ALERT);
                    if (Config::getAlertWake()) Display::wakeFromDim();
                    lastTypeAlert[ti] = now;
                }
                DefhogTerminal::pushLine("[TRACK] %.18s %ddB", ev.detail, ev.rssi);
                // challenge: track unique threat types
                if (isThreat && (int)ev.threatType >= 0 && (int)ev.threatType < 32) {
                    seenTrackerTypes |= (1u << (int)ev.threatType);
                    Challenges::onTrackerTypeDetected(seenTrackerTypes);
                    if (countBits32(seenTrackerTypes) >= 5) {
                        Achievements::tryUnlock(Achievement::TAG_COLLECTOR);
                    }
                }
                if (isThreat) awardReconXP(2);
                checkBleCatalogTrophy();
                break;
            }

            case Recon::ReconEvent::TRACKER_FOLLOWING:
                Mood::onTrackerFollowing(ev.detail);
                Achievements::tryUnlock(Achievement::TAIL_BREAKER);
                Display::showAlertToast(NoirNarrator::getFollowingPhrase(ev.detail), 5000);
                SFX::play(SFX::TRACKER_FOLLOWING);
                if (Config::getAlertWake()) Display::wakeFromDim();
                DefhogTerminal::pushLineAlert(" !! STALK: %.14s !! ", ev.detail);
                awardReconXP(8);
                checkBleCatalogTrophy();
                break;

            case Recon::ReconEvent::BLE_SPAM:
                Mood::onBleSpamDetected();
                Display::showAlertToast(NoirNarrator::getBleSpamPhrase(), 4000);
                SFX::play(SFX::RECON_ALERT);
                if (Config::getAlertWake()) Display::wakeFromDim();
                DefhogTerminal::pushLineHype("[SPAM] %s", ev.detail);
                awardReconXP(4);
                checkBleCatalogTrophy();
                break;

            case Recon::ReconEvent::COORDINATED_ATTACK:
                Display::showAlertToast(
                    NoirNarrator::getCoordinatedAttackPhrase(ev.detail), 5000);
                SFX::play(SFX::PARANOIA_ALERT);
                Haptic::bump();
                if (Config::getAlertWake()) Display::wakeFromDim();
                DefhogTerminal::pushLineAlert(" !! MULTI: %s !! ", ev.detail);
                Achievements::tryUnlock(Achievement::XBAND_GUMSHOE);
                awardReconXP(10);
                break;

            case Recon::ReconEvent::ATTACKER_IDENTIFIED:
                Display::showAlertToast("attacker device identified", 4000);
                SFX::play(SFX::PARANOIA_ALERT);
                Haptic::bump();
                Mood::onAttackerIdentified(ev.detail);
                if (Config::getAlertWake()) Display::wakeFromDim();
                DefhogTerminal::pushLineAlert(" ATK-ID: %.20s ", ev.detail);
                Achievements::tryUnlock(Achievement::XBAND_GUMSHOE);
                awardReconXP(15);
                Challenges::onThreatWitnessed();
                break;

            case Recon::ReconEvent::DUAL_BAND_STALK:
                Display::showAlertToast("dual-band tailing detected", 5000);
                SFX::play(SFX::TRACKER_FOLLOWING);
                Haptic::bump();
                Mood::onDualBandStalk(ev.detail);
                if (Config::getAlertWake()) Display::wakeFromDim();
                DefhogTerminal::pushLineAlert(" !! DUAL-BAND STALK !! ");
                Achievements::tryUnlock(Achievement::XBAND_GUMSHOE);
                awardReconXP(15);
                Challenges::onThreatWitnessed();
                break;

            case Recon::ReconEvent::FOLLOWING_NETWORK_ID:
                Display::showAlertToast("tracker owner network found", 4000);
                SFX::play(SFX::RECON_ALERT);
                if (Config::getAlertWake()) Display::wakeFromDim();
                DefhogTerminal::pushLineHype("[XBAND] FOLLOW-NET: %.14s", ev.detail);
                break;

            case Recon::ReconEvent::WATCHLIST_ENTER: {
                char msg[40];
                snprintf(msg, sizeof(msg), "%s NEARBY", ev.detail);
                Display::showAlertToast(msg, 4000);
                SFX::play(SFX::RECON_ALERT);
                if (Config::getAlertWake()) Display::wakeFromDim();
                DefhogTerminal::pushLineAlert(" >> %s NEAR << ", ev.detail);
                break;
            }
            case Recon::ReconEvent::WATCHLIST_EXIT: {
                char msg[40];
                snprintf(msg, sizeof(msg), "%s GONE", ev.detail);
                Display::showToast(msg, 3000);
                SFX::play(SFX::RECON_ALERT);
                DefhogTerminal::pushLine("[WATCH] %s gone", ev.detail);
                break;
            }

            case Recon::ReconEvent::EVIL_TWIN:
                Mood::onEvilTwin(ev.ssid);
                DefhogTerminal::pushLineAlert(" !! TWIN: \"%.12s\" !! ", ev.ssid);
                if (DefensePipeline::snapshot().isDeauthActive()) {
                    Display::showAlertToast("deauth + evil twin", 4500);
                    SFX::play(SFX::PARANOIA_ALERT);
                    Haptic::bump();
                    if (Config::getAlertWake()) Display::wakeFromDim();
                }
                awardReconXP(8);
                Challenges::onThreatWitnessed();
                break;

            case Recon::ReconEvent::KARMA_HONEYPOT:
                Mood::onKarmaDetected(ev.ssid);
                Display::showAlertToast(NoirNarrator::getKarmaPhrase(ev.ssid), 4500);
                SFX::play(SFX::RECON_ALERT);
                if (Config::getAlertWake()) Display::wakeFromDim();
                DefhogTerminal::pushLineAlert(" !! KARMA: \"%.12s\" !! ", ev.ssid);
                awardReconXP(8);
                Challenges::onThreatWitnessed();
                break;

            case Recon::ReconEvent::KNOWN_AP:
                Mood::onKnownAPFound(ev.ssid);
                if (ev.count > 1) {
                    DefhogTerminal::pushLine("[POTMATCH] \"%.12s\" +%d",
                        ev.ssid, ev.count - 1);
                } else {
                    DefhogTerminal::pushLine("[POTMATCH] \"%.14s\"", ev.ssid);
                }
                break;

            case Recon::ReconEvent::OPEN_AP_WARNING:
                DefhogTerminal::pushLine("[OPEN] %d unauth nets", ev.count);
                break;

            case Recon::ReconEvent::PROBE_VULN_CLIENT:
                if (ev.detail[0]) {
                    DefhogTerminal::pushLineAlert(" PROBE-RISK %s ", ev.detail);
                } else {
                    DefhogTerminal::pushLineAlert(" PROBE-RISK x%d ", ev.count);
                }
                awardReconXP(3);
                break;

            case Recon::ReconEvent::FINGERPRINT_MISMATCH:
                memcpy(lastFingerprintBSSID, ev.bssid, 6);
                lastFingerprintSeen = now;
                if (ev.detail[0]) {
                    DefhogTerminal::pushLine("[FP] %.10s %s", ev.ssid, ev.detail);
                } else {
                    DefhogTerminal::pushLine("[FP] %.16s", ev.ssid);
                }
                if (fpToSeqCorrelated) {
                    Display::showAlertToast("beacon spoof pattern", 4000);
                    SFX::play(SFX::RECON_ALERT);
                    Haptic::bump();
                    if (Config::getAlertWake()) Display::wakeFromDim();
                }
                break;

            case Recon::ReconEvent::SEQ_ANOMALY:
                memcpy(lastSeqBSSID, ev.bssid, 6);
                lastSeqSeen = now;
                if (ev.detail[0]) {
                    DefhogTerminal::pushLine("[SEQ] %.9s %s", ev.ssid, ev.detail);
                } else {
                    DefhogTerminal::pushLine("[SEQ] %.15s", ev.ssid);
                }
                if (seqToFpCorrelated) {
                    Display::showAlertToast("beacon spoof pattern", 4000);
                    SFX::play(SFX::RECON_ALERT);
                    Haptic::bump();
                    if (Config::getAlertWake()) Display::wakeFromDim();
                }
                break;

            case Recon::ReconEvent::RSSI_ANOMALY:
                if (ev.detail[0]) {
                    DefhogTerminal::pushLine("[RSSI] %.8s %s", ev.ssid, ev.detail);
                } else {
                    DefhogTerminal::pushLine("[RSSI] %.14s", ev.ssid);
                }
                break;

            case Recon::ReconEvent::DEAUTH_DETECTED:
                // Hunt MUDBALL TX triggers false positives — Recon sniffs deauth outside hunt only.
                if (currentMode != HamletMode::HUNT) {
                    if (Config::getParanoiaEnabled()) {
                        triggerGlobalDeauth(ev.rssi, ev.channel);
                    }
                    if (ev.detail[0]) {
                        DefhogTerminal::pushLineAlert(" DEAUTH ch%d %ddB x%d %s ",
                            ev.channel, ev.rssi, ev.count, ev.detail);
                    } else {
                        DefhogTerminal::pushLineAlert(" DEAUTH ch%d %ddB x%d ",
                            ev.channel, ev.rssi, ev.count);
                    }
                    if (DefensePipeline::snapshot().isEvilTwinActive()) {
                        Display::showAlertToast("deauth + evil twin", 4000);
                        SFX::play(SFX::PARANOIA_ALERT);
                        Haptic::bump();
                        if (Config::getAlertWake()) Display::wakeFromDim();
                    }
                }
                break;

            case Recon::ReconEvent::SCAN_COMPLETE:
                Mood::onReconScan(ev.count);
                if (ev.detail[0]) {
                    DefhogTerminal::pushLineDim("[SCAN] %d APs %s",
                        DefensePipeline::snapshot().getLastWifiAPCount(), ev.detail);
                } else {
                    DefhogTerminal::pushLineDim("[SCAN] %d APs %d open",
                        DefensePipeline::snapshot().getLastWifiAPCount(), DefensePipeline::snapshot().getOpenAPCount());
                }
                checkBleCatalogTrophy();
                break;

            // ==[ ADVANCED DETECTION EVENTS ]== wired to mood/toast/SFX

            case Recon::ReconEvent::CANARY_TRIPPED:
                // CRITICAL: proof of targeted probe replay against this device
                Display::showAlertToast("CANARY TRIPPED", 6000);
                SFX::play(SFX::PARANOIA_ALERT);
                Haptic::bump();
                if (Config::getAlertWake()) Display::wakeFromDim();
                Mood::onCanaryTripped(ev.ssid);
                DefhogTerminal::pushLineAlert(" !! CANARY: \"%.12s\" !! ", ev.ssid);
                awardReconXP(15);
                Challenges::onThreatWitnessed();
                break;

            case Recon::ReconEvent::KARMA_CONFIRMED:
                // HIGH: phantom probe injection confirmed active KARMA AP
                Display::showAlertToast("KARMA CONFIRMED", 5000);
                SFX::play(SFX::PARANOIA_ALERT);
                Haptic::bump();
                if (Config::getAlertWake()) Display::wakeFromDim();
                Mood::onKarmaConfirmed(ev.ssid);
                DefhogTerminal::pushLineAlert(" !! KARMA-CONFIRM: \"%.10s\" !! ", ev.ssid);
                awardReconXP(10);
                Challenges::onThreatWitnessed();
                break;

            case Recon::ReconEvent::TOOL_IDENTIFIED:
                // attack tool classified by behavioral signature
                Display::showAlertToast(
                    NoirNarrator::getToolIdentifiedPhrase(ev.detail), 4500);
                SFX::play(SFX::RECON_ALERT);
                if (Config::getAlertWake()) Display::wakeFromDim();
                Mood::onToolIdentified(ev.detail);
                DefhogTerminal::pushLineAlert(" TOOL: %s ch%d ", ev.detail, ev.channel);
                awardReconXP(5);
                Challenges::onThreatWitnessed();
                break;

            case Recon::ReconEvent::HOSTILE_CLIENT:
                // hostile client profiled via probe IE fingerprint
                Display::showAlertToast(
                    NoirNarrator::getHostileClientPhrase(ev.detail), 4000);
                SFX::play(SFX::RECON_ALERT);
                if (Config::getAlertWake()) Display::wakeFromDim();
                DefhogTerminal::pushLineAlert(" HOSTILE: %s %ddB ", ev.detail, ev.rssi);
                awardReconXP(5);
                Challenges::onThreatWitnessed();
                break;

            case Recon::ReconEvent::RELAY_SUSPECT:
                // BLE tracker relay/replay detected via jitter anomaly
                Display::showAlertToast("BLE relay detected", 4000);
                SFX::play(SFX::RECON_ALERT);
                if (Config::getAlertWake()) Display::wakeFromDim();
                DefhogTerminal::pushLineAlert(" RELAY: %.16s %ddB ", ev.detail, ev.rssi);
                awardReconXP(6);
                Challenges::onThreatWitnessed();
                break;

            case Recon::ReconEvent::LOW_ENTROPY_BEACON:
                // beacon IE diversity too low — likely fake AP
                DefhogTerminal::pushLine("[FAKE-AP] \"%.10s\" ch%d %s",
                    ev.ssid, ev.channel, ev.detail);
                awardReconXP(3);
                Challenges::onThreatWitnessed();
                break;

            default:
                break;
        }
    }
}

void enterMode(HamletMode mode) {
    HAMLET_LOGF("[HAMLET] enterMode %d — PSRAM free: %u\n",
                  (int)mode, (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    // dismiss help overlay on mode switch
    Display::dismissHelpOverlay();
    // Some secondary screens still draw directly to the LCD. Their pixels are
    // outside the tile presenter's history, so the next optimized frame must
    // establish a fresh panel baseline.
    FramePresenter::invalidate();

    if (currentMode == HamletMode::MENU && mode != HamletMode::MENU) {
        if (Menu::hasActiveEncounter()) Menu::dismissEncounter();
        MenuPig::cleanupForModeExit();
    }

    // exit current mode
    exitCurrentMode();

    // ESP-NOW teardown precedes every WiFi-mode handoff. A radio owner must
    // never inherit an initialized FLOCK transport on the shared frontend.
    if (RadioPolicy::modeOwnsWifi(mode)) {
        NowFlock::releaseRadio();
    }

    if (mode != HamletMode::HUNT) {
        MenuPig::clearRearCinematic();
    }
    if (mode != HamletMode::WARDRIVE) {
        MenuPig::clearWDCinematic();
    }

    // apply power profile for new mode
    Power::onModeEnter(mode);
    if (Power::allowTransitionHaptic()) {
        Haptic::bump();  // tactile mode transition
    }

    // reset pet counter on mode change
    petCount = 0;

    // ==[ RECON RADIO MANAGEMENT ]== hunt: parasitic (feeds from beacon data). others: full suspend.
    if (mode == HamletMode::HUNT) {
        DefensePipeline::requestOperatingState(Defense::OperatingState::PARASITIC);
        // BLE now deinited — re-eval WiFi PS so Hunt::start() inherits WIFI_PS_NONE intent.
        Power::reevalWiFiPS(mode);
    } else if (mode == HamletMode::SPECTRUM ||
               mode == HamletMode::WEBCONFIG || mode == HamletMode::XFER) {
        // Spectrum needs the controller gone for continuous promiscuous RX.
        // AP modes only stop Recon activity: keeping NimBLE warm avoids stale
        // FreeRTOS callouts across repeated AP enter/exit cycles.
        DefensePipeline::requestOperatingState(
            mode == HamletMode::SPECTRUM
                ? Defense::OperatingState::SUSPENDED_RELEASE_BLE
                : Defense::OperatingState::SUSPENDED_KEEP_BLE);
        // Re-evaluate WiFi PS after the BLE ownership decision above.
        Power::reevalWiFiPS(mode);
    }
    // WARDRIVE: Recon stays alive — BLE warm via forceWardriveBle() in WardriveBle::init()

    // enter new mode (table dispatch)
    {
        const auto& desc = MODE_TABLE[(int)mode];
        if (desc.enter) desc.enter();
    }

    // ==[ MODE-SPECIFIC ENTER LOGIC ]== cases with extra setup beyond the table
    switch (mode) {
        case HamletMode::HUNT:
        case HamletMode::SPECTRUM:
            HamletSession::onHuntEnter(session, millis());
            break;

        case HamletMode::MENU:
            if (Teleport::isActive() && Teleport::getContext() == Teleport::Context::WARDRIVE_TO_MENU) {
                Menu::enterRoaming();  // pig already at destination station
            } else {
                Menu::enter();
            }
            break;

        case HamletMode::POWER_MENU:
            Display::resetPowerMenu();
            break;

        case HamletMode::IDLE: {
            // ==[ IDLE PULL ]== Zeigarnik open loops from offline BLE scans (sim: #6 sensitivity)
            uint8_t queued = DefensePipeline::snapshot().getOfflineScanCount();
            if (queued > 0) {
                DefensePipeline::clearOfflineScanCount();
                char buf[48];
                snprintf(buf, sizeof(buf), "ipp queued: %d signals while dark.", queued);
                Mood::setPhrase(buf, AvatarState::NEUTRAL);
                Avatar::perkUp();
                if (DefhogTerminal::isVisible())
                    DefhogTerminal::pushLine("IPP QUEUED: %d signals / dark time", queued);
            }
            break;
        }

        default:
            break;
    }

    TouchHints::onModeEnter(mode);
    setMode(mode);
}

void exitCurrentMode() {
    Avatar::cancelPortalPull();

    // accumulate hunt/spectrum time for session active detection
    if ((currentMode == HamletMode::HUNT || currentMode == HamletMode::SPECTRUM) &&
        session.huntActive) {
        HamletSession::onHuntExit(session, millis());
        if (!session.huntTimeMarked && session.huntAccumulated >= 120000) {
            Config::markSessionActive();
            session.huntTimeMarked = true;
        }
    }

    // table dispatch
    {
        const auto& desc = MODE_TABLE[(int)currentMode];
        if (desc.exit) desc.exit();
    }

    // mode-specific exit logic beyond the table
    if (currentMode == HamletMode::WARDRIVE) {
        // Wardrive::stop() called via MODE_TABLE above
        if (Teleport::isActive() && Teleport::getContext() == Teleport::Context::WARDRIVE_TO_MENU) {
            MenuPig::returnFromWardriveViaTeleport();
        } else {
            MenuPig::returnFromWardrive();
        }
    }

    // ==[ RECON RESUME ]== reactivate recon when returning to idle/menu
    // WARDRIVE excluded — Recon stays alive via wardrive BLE cadence, unforced in Wardrive::stop()
    if (currentMode == HamletMode::HUNT || currentMode == HamletMode::SPECTRUM ||
        currentMode == HamletMode::WEBCONFIG ||
        currentMode == HamletMode::XFER) {
        DefensePipeline::requestOperatingState(Defense::OperatingState::BACKGROUND);
    }
}

static void handlePowerAction(bool confirmed) {
    const PowerPolicy::Action action =
        PowerPolicy::actionFromIndex(Display::getPowerOption());

    // The CoreS3 SE board profile has no registered wake GPIO in M5Unified.
    // Refuse a stale or corrupted selection here as a second lock: without a
    // timer, either sleep call would close the case with no touch route back.
    if (!PowerPolicy::isAvailable(action, HAMLET_HAS_TOUCH_SLEEP_WAKE)) {
        Display::showAlertToast("NO SLEEP WAKE ROUTE\nACTION BLOCKED", 2600, 2);
        return;
    }

    if (action == PowerPolicy::Action::CANCEL) {
        enterMode(HamletMode::IDLE);
        return;
    }

    // Every terminal action gets a fresh, action-locked confirmation. This is
    // intentionally session-local; acknowledging yesterday's warning must not
    // authorize erasing today's evidence.
    if (PowerPolicy::requiresConfirmation(action) && !confirmed) {
        Display::showSleepWarning();
        return;
    }

    // ESP-IDF manual light sleep requires WiFi and Bluetooth to be stopped.
    // Quiescing first also prevents a capture callback from racing the exact
    // shutdown journal snapshot. Kill high-draw outputs before SD writes to
    // lower the brownout/corruption risk on a depleted battery.
    SFX::stop();
    NoirJazz::stopImmediate();
    Haptic::stop();
    AmbientLED::off();
    suspendRadiosForPowerTransition();

    Config::save();
    ItemDrops::save();
    ItemEffects::save();
    Bounty::save();
    Mailbox::save();

    // The normal journal path contains deferred appends. A controlled power
    // transition gets a synchronous exact snapshot; if a mounted card fails,
    // stay awake so the operator can recover/export instead of claiming success.
    if (PowerPolicy::clearsLiveMemory(action) &&
        SDStorage::isAvailable() && !Capture::sealJournal()) {
        resumeRadiosAfterPowerTransition();
        Display::showAlertToast("SD SEAL FAILED\nPOWER ACTION CANCELLED", 3200, 2);
        return;
    }

    if (PowerPolicy::endsSession(action)) {
        Display::drawCaseClosed();
    }

    switch (action) {
        case PowerPolicy::Action::DEEP_SLEEP:
            M5.Power.deepSleep(0);  // full boot after wake
            break;

        case PowerPolicy::Action::LIGHT_SLEEP:
            M5.Display.sleep();
            M5.Display.waitDisplay();
            M5.Power.lightSleep(0);  // returns with PSRAM/live state retained
            M5.Display.wakeup();
            Display::resetDimTimer();
            powerWakeInputBlockUntil = millis() + 500;
            resumeRadiosAfterPowerTransition();
            enterMode(HamletMode::IDLE);
            break;

        case PowerPolicy::Action::POWER_OFF:
            M5.Power.powerOff();
            break;

        case PowerPolicy::Action::CANCEL:
        case PowerPolicy::Action::COUNT:
            enterMode(HamletMode::IDLE);
            break;
    }
}

uint32_t getUptimeSeconds() {
    return (millis() - bootTime) / 1000;
}

uint8_t getBatteryPercent() {
    const int32_t raw = M5.Power.getBatteryLevel();
    if (raw < 0) return 100;
    if (raw > 100) return 100;
    return static_cast<uint8_t>(raw);
}

bool isCharging() {
    return M5.Power.isCharging() == m5::Power_Class::is_charging;
}

uint32_t getIdleDuration() {
    if (currentMode == HamletMode::IDLE) {
        return millis() - lastModeChangeTime;
    }
    return 0;
}

uint32_t getPSRAMFree() {
    return heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
}

uint32_t getPSRAMTotal() {
    return heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
}

} // namespace Hamlet
