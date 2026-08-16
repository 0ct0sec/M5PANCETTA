/**
 * Config - NVS implementation
 * Enhanced with adaptive hunt mode settings
 */

#include "config.h"
#include "../hal/platform.h"
#include "achievements.h"
#include "challenges.h"
#include "item_effects.h"
#include "item_drops.h"
#include "gps.h"
#include "gps_policy.h"
#include "../radio/c5_uart_policy.h"
#include "../radio/mesh_uart_policy.h"
#include <Preferences.h>
#include <M5Unified.h>  // For RTC access
#include <math.h>       // For powf in XP curve
#include <sys/time.h>
#include <ctype.h>
#include "../ui/display.h"
#include "../audio/sfx.h"
#include "../util/debug_log.h"
#include "constants.h"

namespace Config {

static Preferences prefs;

// ==[ XP PROGRESSION SYSTEM ]==
// Curve: XP(L) = 100 * L^1.8 — fast early, gentle late
// Max level: 42 (the answer)
static const uint8_t MAX_LEVEL = 42;
static const uint32_t SESSION_XP_SOFT_CAP = 500;
static const float SESSION_XP_OVERFLOW_MULT = 0.2f;  // 20% of excess
static const uint16_t SESSION_XP_STREAK_SHOWUP_BONUS = 20;
static const uint16_t SESSION_XP_STREAK_RECORD_BONUS = 100;
static const uint32_t SESSION_XP_GOAL_BASE_BONUS = 50;
static const uint32_t SESSION_XP_GOAL_STEP_DIV = 100;
static const uint32_t SESSION_XP_GOAL_STEP_BONUS = 250;
static const uint32_t SESSION_XP_GOAL_FAIL_DECAY = 125;
static const uint16_t SESSION_XP_GOAL_MIN_TARGET = 500;
static const uint16_t SESSION_XP_GOAL_MAX_TARGET = 6000;
static const uint16_t SESSION_XP_GOAL_INITIAL_TARGET = 2000;
static const uint16_t SESSION_XP_RECON_CAP = 100;
static const uint16_t SESSION_XP_STATION_CAP = 180;
static const uint16_t SESSION_XP_TRACKER_SURVIVAL_CAP = 40;
static const uint16_t SESSION_XP_WALK_MILESTONE_CAP = 330;
static const uint16_t SESSION_XP_EVENT_CAP = 120;
static const uint16_t SESSION_XP_CAT_MEMORY_CAP = 56;

// ==[ DIRTY TRACKING ]== group-level bitmask writes only the settings groups that changed
enum DirtyGroup : uint16_t {
    D_DISPLAY    = 1 << 0,   // brightness, dimLevel, dimTimeout, dispRot180, theme, roomParallax
    D_SOUND      = 1 << 1,   // sound, sfxVol
    D_HUNT       = 1 << 2,   // probeRSSI, autoProbe, adaptHunt, deauth*, excl*, sae*, eapol*, csa*, authFlood*, statHop, walkHop
    D_MOTION     = 1 << 3,   // statTime, walkSteps, autoHunt
    D_POWER      = 1 << 4,   // shakeWake, alertWake, paranoia, tilt*, specTilt, battAdapt
    D_CATCH      = 1 << 5,   // catchCamp, catchPatr
    D_TIMEOUT    = 1 << 6,   // actTimeCmp, actTimePat
    D_NOWFLOCK   = 1 << 7,   // FNOW master gate, group, cadence, exports, BLE heartbeat
    D_TRACKER    = 1 << 8,   // rssiSmooth, ghostMark
    D_UI         = 1 << 9,   // lore/help state
    D_STATS      = 1 << 10,  // pmkids, handshakes, steps, distance
    D_COORD      = 1 << 11,  // coordRole, reconEnabled, ipp*, gps*
    D_UPLOAD     = 1 << 12,  // wpaSecKey, wpaSecUrl, uplWifi*
    D_DUCB       = 1 << 13,  // ucb1_ch blob
    D_XP         = 1 << 14,  // xp, streak, bestStreak
    D_SESSION    = 1 << 15,  // sessActive, sessBtwn, sessSteps, goalTarget
};
static uint16_t dirtyGroups = 0;

// Deferred save state
static bool pendingSave = false;
static uint32_t lastChangeTime = 0;
static const uint32_t SAVE_DELAY = 5000;  // 5s debounce

// Default values (PORKCHOP-aligned)
static uint8_t brightness = 80;       // 0-100% (PORKCHOP default 80%)
static uint8_t dimLevel = 20;         // 0-100% dimmed brightness
static uint16_t dimTimeout = 30;      // Seconds before dimming (0=never)
static bool displayRotate180 = false; // rotate display 180 degrees
static bool ledAmbient = false;       // M5GO Bottom2 ambient LED glow (default off)
static uint8_t ledColor = 0;         // 0=AUTO, 1=THEME, 2-13=fixed hue preset
static uint8_t ledBrightness = 5;    // LED brightness 1-10 (default 5 = 50%)
static bool soundEnabled = true;
static uint8_t sfxVolume = 2;         // SFX volume 0-10 (default 2 = 20%)
static uint8_t musicVolume = 5;       // Music volume 0-10 (default 5 = 50%)
#if defined(HAMLET_CORE3SE)
static bool bathMicEnabled = true;    // Audio input is still cold until Pancetta reaches the bath.
#else
static bool bathMicEnabled = false;
#endif
static bool hapticEnabled = true;     // haptic motor on/off
static uint8_t hapticIntensity = 7;   // haptic intensity 0-10 (default 7 = 70%)
static int8_t probeThreshold = -70;
// ==[ FRESH-INSTALL RADIO POSTURE ]== a blank NVS may listen, but it must not
// volunteer frames or enter an active workflow before the operator turns a
// named control on. Stored choices still win on upgrades.
static constexpr bool DEFAULT_AUTO_PROBE = false;
static constexpr bool DEFAULT_DEAUTH = false;
static constexpr bool DEFAULT_DEAUTH_AGGRESSIVE = false;
static constexpr bool DEFAULT_EAPOL_INJECTION = false;
static constexpr bool DEFAULT_CSA = false;
static constexpr bool DEFAULT_AUTO_HUNT = false;
static constexpr bool DEFAULT_NOWFLOCK_ENABLED = false;
static_assert(!DEFAULT_AUTO_PROBE && !DEFAULT_DEAUTH &&
              !DEFAULT_DEAUTH_AGGRESSIVE && !DEFAULT_EAPOL_INJECTION &&
              !DEFAULT_CSA && !DEFAULT_AUTO_HUNT &&
              !DEFAULT_NOWFLOCK_ENABLED,
              "fresh-install radio transmitters must stay disarmed");

static bool autoProbe = DEFAULT_AUTO_PROBE;
static constexpr uint16_t DEFAULT_THEME_HUE = 120;
static constexpr uint8_t DEFAULT_THEME_STYLE = 5;  // THE OG
static_assert(DEFAULT_THEME_STYLE < THEME_STYLE_COUNT,
              "fresh-install theme must name a valid style");
static uint16_t themeHue = DEFAULT_THEME_HUE;
static uint8_t themeStyle = DEFAULT_THEME_STYLE;
static uint8_t accentMode = 0;        // 0-5: light hue family preset
static uint8_t lightIntensity = 0;    // 0-3: emissive boost level
static bool roomParallaxEnabled = true; // IMU depth motion in Pancetta's rooms
static constexpr char DEFAULT_HAMLET_NAME[] = "HAM1";
static char hamletName[5] = "HAM1";  // 4-char FLOCK handle
static PigHeadStyle pigHeadStyle = PIG_HEAD_THEME;

static char sanitizeNameChar(char c) {
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return c;
    return 'X';
}

static void sanitizeName(const char* src, char* dst) {
    if (!dst) return;
    for (uint8_t i = 0; i < 4; i++) {
        char c = (src && src[i]) ? src[i] : 'X';
        dst[i] = sanitizeNameChar(c);
    }
    dst[4] = '\0';
}

// === ADAPTIVE HUNT MODE SETTINGS ===
static bool adaptiveHunt = true;      // Motion-aware hunting enabled
static bool deauthEnabled = DEFAULT_DEAUTH;
static bool deauthAggressive = DEFAULT_DEAUTH_AGGRESSIVE;
static bool excludePMF = true;        // Skip PMF networks (can't deauth)
static bool excludeWPA3 = true;       // Skip WPA3 (SAE immune to deauth)
static bool saeAttackEnabled = false; // SAE reject/downgrade assist (off by default)
static bool eapolInjectionEnabled = DEFAULT_EAPOL_INJECTION;
static bool csaEnabled = DEFAULT_CSA;
static bool authFloodEnabled = false;      // Auth flood AP table exhaustion (off by default, last resort)
static uint8_t stationaryHopDelay = 150;  // Slower when stationary (dwell more)
static uint8_t walkingHopDelay = 80;      // Faster when walking (discover more)

// === MOTION DETECTION SETTINGS ===
static uint8_t stationaryTimeout = 10;    // Seconds before STATIONARY (default 10s)
static uint8_t walkingSteps = 3;          // Steps required for WALKING (default 3)
static bool autoHuntEnabled = DEFAULT_AUTO_HUNT;

// === POWER SETTINGS ===
static bool shakeWake = true;             // Shake gesture wakes dimmed screen
static bool alertWake = true;             // Deauth alert wakes screen
static bool paranoiaEnabled = false;      // Global deauth alert (interrupts any mode)
static bool tiltNavigationEnabled = false;  // Tilt navigation enabled (left/right/up tilt gestures)
static bool spectrumTiltEnabled = false;  // Spectrum dial mode auto-activation (tilt = channel select)
static bool batteryAdaptation = true;     // Auto-adjust power based on battery level
static bool pwrFps60 = true;             // Force 60fps when USB-C plugged in

// === CATCH WINDOW SETTINGS ===
static uint8_t catchWindowCamp = 8;       // Catch window in CAMP mode (8s)
static uint8_t catchWindowPatrol = 5;     // Catch window in PATROL mode (5s)

// === NETWORK ACTIVITY TIMEOUT SETTINGS ===
static uint8_t activeTimeoutCamp = 30;    // Network activity timeout in CAMP mode (30s)
static uint8_t activeTimeoutPatrol = 10;  // Network activity timeout in PATROL mode (10s)

// === TRACKER SETTINGS ===
static Config::RssiSmooth rssiSmooth = Config::RSSI_SMOOTH_MED;  // Default: MED (current behavior)
static bool ghostMarkerEnabled = true;    // Default: ON (show ghost marker)

// ==[ BLE WATCHLIST ]== 3 named BLE device presence alerts
static Config::WatchlistSlot watchlistSlots[Config::MAX_WATCHLIST] = {};

// === UI STATE ===
static uint32_t loreOpenCount = 0;        // next sequential 0ct0 case-file fragment
static uint32_t catMemoryMask = 0;        // observed Pig habits
static uint32_t catLoreSeenMask = 0;      // observed habits read in ABOUT
static uint32_t hintSeen = 0;            // Touch hint seen bitmask (1 bit per mode)
static uint32_t helpWikiSeen = 0;        // Help wiki seen bitmask (1 bit per mode)
static uint32_t npcChoiceLedger = 0;      // two-bit last decision per NPC
static uint32_t npcClosedCastMask = 0;    // characters ever resolved
static bool npcCodaSeen = false;          // whole-cast consequence delivered

// === XP PROGRESSION ===
static uint32_t totalXP = 0;             // Cumulative XP, never decreases
static uint32_t sessionXPGained = 0;     // XP actually awarded this session (for display)
static uint32_t sessionXPRaw = 0;        // XP attempted this session (for soft cap calc)
static uint8_t  pendingLevelUp     = 0;  // non-zero = level just crossed, consume in loop
static uint32_t pendingXPDisplay   = 0;  // XP accumulated since last Display frame pull
static uint32_t rewardSourceXPAwarded[static_cast<uint8_t>(RewardSource::COUNT)] = {0};  // per-source reward hard cap tracking
static uint16_t currentStreak = 0;       // Consecutive active sessions
static uint16_t bestStreak = 0;          // Lifetime best

// === SESSION ACTIVITY TRACKING ===
static bool lastSessionWasActive = false; // Was previous session active?
static uint8_t sessionsBetween = 0;       // Consecutive idle boots
static bool currentSessionActive = false; // Is THIS session active yet?

// === SESSION CAPTURE TRACKING ===
static uint8_t sessionPMKIDCount = 0;     // PMKIDs this session
static uint8_t sessionHSCount = 0;        // Handshakes this session

// === SESSION GOALS ===
static uint32_t sessionSteps = 0;         // Steps this session (RAM)
static uint32_t lastSessionSteps = 0;     // Last session's final step count (from NVS)
static uint16_t goalTarget = 2000;        // Adaptive goal (500-6000)
static bool goalWasMetLastSession = false; // For boot phrase
static bool goalCloseTriggered = false;   // 80% shown this session
static bool goalCompleteTriggered = false; // 100% shown this session
static uint8_t consecutiveGoalsMet = 0;   // for GOAL_STREAK_5 achievement

// Coordination settings
static uint8_t coordinationRole = 1;      // 0=master, 1=slave, 2=standalone (default=slave)

// Reconnaissance setting
static bool reconEnabled = false;          // Whether reconnaissance mode is enabled (default=off)

// ==[ IPP DEFENSE SETTINGS ]==
static bool ippEnabled = false;            // IPP master switch
static bool ippBLEScan = true;             // BLE passive scan (default on when IPP enabled)
static bool ippWifiScan = true;            // WiFi environment scan (default on when IPP enabled)
static bool wifiChaffEnabled = false;      // HOGWASH: fake handshake injection on deauth detect
static bool wardriveBleEnabled = true;    // BLE interleave during wardrive (default on)

// ==[ WPA-SEC CLOUD UPLOAD ]==
static char wpaSecKey[33] = "";            // WPA-SEC API key (32 chars + null)
static char wpaSecUrl[65] = "https://wpa-sec.stanev.org";  // API URL
static char uploadWifiSsid[33] = "";       // WiFi SSID for upload
static char uploadWifiPass[65] = "";       // WiFi password
static char wigleUsername[25] = "";        // WiGLE API name (24 chars + null)
static char wigleToken[65] = "";           // WiGLE API token (64 chars + null)
static char lootPin[5] = "";              // 4-char ABC PIN + null (empty = disabled)

// ==[ GPS SETTINGS ]==
static bool gpsEnabled = false;           // GPS module connected (default off)
static bool gpsAlwaysOn = false;         // GPS stays hot across all modes (default off)
static uint8_t gpsRxPin = GPSPolicy::DEFAULT_RX_PIN;
static uint8_t gpsTxPin = GPSPolicy::DEFAULT_TX_PIN;
static uint8_t gpsBaudIdx = GPSPolicy::DEFAULT_BAUD_INDEX;
static constexpr uint32_t GPS_BAUD_TABLE[] = { 9600, 38400, 57600, 115200 };
static const uint8_t GPS_BAUD_COUNT = 4;
static_assert(GPS_BAUD_TABLE[GPSPolicy::DEFAULT_BAUD_INDEX] == GPSPolicy::DEFAULT_BAUD,
              "M003-V21 default baud index must select 115200");

// ==[ C5MONSTER SETTINGS ]==
static bool c5Enabled = C5UartPolicy::DEFAULT_ENABLED;
static uint8_t c5RxPin = C5UartPolicy::RX_PIN;
static uint8_t c5TxPin = C5UartPolicy::TX_PIN;
static uint32_t c5Baud = C5UartPolicy::DEFAULT_BAUD;

// ==[ MESH SETTINGS ]==
static bool meshEnabled = false;
static uint8_t meshRxPin = MeshUartPolicy::RX_PIN;
static uint8_t meshTxPin = MeshUartPolicy::TX_PIN;
static uint32_t meshBaud = MeshUartPolicy::DEFAULT_BAUD;
// Which dialect the C6L is speaking. TEXTMSG is the default because it is the
// SerialModule default: a radio nobody has reconfigured speaks it.
static uint8_t meshCodec = (uint8_t)MeshUartPolicy::Codec::TEXTMSG;

// ==[ NOWFLOCK SETTINGS ]== cached with the rest of Config. The background
// loop asks for the master gate and BLE heartbeat every frame; opening NVS on
// each getter turned a Boolean read into persistent frame-time work.
static constexpr uint32_t DEFAULT_NOWFLOCK_GROUP_KEY = 0xDEADB4D6u;
static bool nowFlockEnabled = DEFAULT_NOWFLOCK_ENABLED;
static uint32_t nowFlockGroupKey = DEFAULT_NOWFLOCK_GROUP_KEY;
static uint8_t nowFlockReportS = 10;
static bool nowFlockPigbrother = false;
static uint8_t nowFlockExportProfile = 0;
static bool nowFlockBleHeartbeat = false;

// Wardrive stats
static uint32_t wdTotal = 0;             // lifetime unique networks
static uint16_t wdSessions = 0;          // total wardrive sessions

// Prestige / reincarnation
static uint8_t prestigeCount = 0;        // 0 = never, 1+ = reincarnated. never decreases

// Capture type collection (case files)
static uint16_t seenAuthTypes = 0;       // bitmask of wifi_auth_mode_t values ever captured

// Stats
static uint32_t totalPMKIDs = 0;
static uint32_t totalHandshakes = 0;
static uint32_t totalSteps = 0;
static uint32_t totalDistance = 0;

// Both supported cores expose a BM8563 through M5Unified.
static bool hasHardwareRTC = true;
static bool clockTrusted = false;

static void markDirty(uint16_t group);

static uint32_t rtcDateTimeToEpoch(const m5::rtc_datetime_t& dt) {
    struct tm t = {};
    t.tm_year = dt.date.year - 1900;
    t.tm_mon = dt.date.month - 1;
    t.tm_mday = dt.date.date;
    t.tm_hour = dt.time.hours;
    t.tm_min = dt.time.minutes;
    t.tm_sec = dt.time.seconds;
    t.tm_isdst = -1;
    return (uint32_t)mktime(&t);
}

static bool isRtcDateTimeValid(const m5::rtc_datetime_t& dt) {
    return dt.date.year >= 2024 && dt.date.year <= 2099 &&
           dt.date.month >= 1 && dt.date.month <= 12 &&
           dt.date.date >= 1 && dt.date.date <= 31 &&
           dt.time.hours <= 23 && dt.time.minutes <= 59 &&
           dt.time.seconds <= 59;
}

static bool readRtcDateTime(m5::rtc_datetime_t* dt) {
    if (!hasHardwareRTC || dt == nullptr) return false;
    *dt = {};
    if (!M5.Rtc.getDateTime(dt)) return false;
    return isRtcDateTimeValid(*dt);
}

static bool canUseTrustedRtc() {
    if (!clockTrusted) return false;
    m5::rtc_datetime_t dt = {};
    return readRtcDateTime(&dt);
}

static void syncSystemTimeFromRtc() {
    if (!clockTrusted) return;
    m5::rtc_datetime_t dt = {};
    if (!readRtcDateTime(&dt)) return;
    timeval tv = {};
    tv.tv_sec = (time_t)rtcDateTimeToEpoch(dt);
    settimeofday(&tv, nullptr);
}

// ==[ WHO HOLDS THE C6L's PINS ]== the C6L's default route is Port C, which is
// the same electrical net as the GPS module's DIP table and, on some cores, the
// C5 bridge. Two owners on one UART is not an error anything reports — it reads
// as a dead radio — so the answer has to be settled before the port is opened.
//
// Deliberately says nothing about whether mesh itself is enabled. The three
// callers ask at different moments and two of them ask precisely because mesh
// is currently off: init() arbitrating a stored config, the settings toggle
// bringing the bridge up, and the sniffer build deciding whether it may force
// the port open. Each used to hand-write this four-way overlap test, and the
// settings copy had already drifted — it checked GPS and forgot the C5 bridge.
const char* meshPinOwner() {
    if (gpsEnabled &&
        (gpsRxPin == meshRxPin || gpsRxPin == meshTxPin ||
         gpsTxPin == meshRxPin || gpsTxPin == meshTxPin)) {
        return "GPS";
    }
    if (c5Enabled &&
        (c5RxPin == meshRxPin || c5RxPin == meshTxPin ||
         c5TxPin == meshRxPin || c5TxPin == meshTxPin)) {
        return "C5";
    }
    return nullptr;
}

void init() {
    prefs.begin("sirloin", false);  // NVS namespace preserved as "sirloin" for settings continuity across rename

    // ==[ NVS INTEGRITY CHECK ]== detect first boot or corrupted data with version canary
    uint16_t nvsVersion = prefs.getUShort("nvsVer", 0);
    if (nvsVersion == 0) {
        // First boot — set version canary for future corruption detection
        prefs.putUShort("nvsVer", 1);
        HAMLET_LOGLN("[CONFIG] NVS canary initialized (first boot)");
    }

    // ==[ RTC INIT ]== seed the shared BM8563 only when its calendar is invalid
    auto rtcCheck = M5.Rtc.getDateTime();

    bool rtcInvalid = !isRtcDateTimeValid(rtcCheck);
    bool rtcBootstrapped = rtcInvalid;

    if (rtcInvalid) {
        m5::rtc_datetime_t dt;
        dt.date.year = 2026;
        dt.date.month = 2;
        dt.date.date = 4;
        dt.time.hours = 0;
        dt.time.minutes = 0;
        dt.time.seconds = 0;
        M5.Rtc.setDateTime(dt);

        delay(10);  // RTC latch
        m5::rtc_datetime_t verify = {};
        if (!M5.Rtc.getDateTime(&verify) || verify.date.year != 2026) {
            hasHardwareRTC = false;  // I2C failure
        }
    }

    clockTrusted = prefs.getBool("clkTrust", false);
    if (rtcBootstrapped || !canUseTrustedRtc()) {
        clockTrusted = false;
        prefs.putBool("clkTrust", false);
    } else {
        syncSystemTimeFromRtc();
    }

    // Load settings
    brightness = prefs.getUChar("brightness", 80);  // Default 80%
    dimLevel = prefs.getUChar("dimLevel", 20);      // Default 20%
    dimTimeout = prefs.getUShort("dimTimeout", 30); // Default 30s
    displayRotate180 = prefs.getBool("dispRot180", false);
    ledAmbient = prefs.getBool("ledAmb", false);
    ledColor = prefs.getUChar("ledCol", 0);
    if (ledColor > 13) ledColor = 0;
    ledBrightness = prefs.getUChar("ledBr", 5);
    if (ledBrightness < 1 || ledBrightness > 10) ledBrightness = 5;
    soundEnabled = prefs.getBool("sound", true);
    sfxVolume = prefs.getUChar("sfxVol", 2);
    musicVolume = prefs.getUChar("musVol", 5);
#if defined(HAMLET_CORE3SE)
    bathMicEnabled = prefs.getBool("bathMic", true);
#endif
    hapticEnabled = prefs.getBool("haptic", true);
    hapticIntensity = prefs.getUChar("hap_int", 7);
    probeThreshold = prefs.getChar("probeRSSI", -70);
    autoProbe = prefs.getBool("autoProbe", DEFAULT_AUTO_PROBE);
    // HSV theme — saved choices and legacy migrations win. Only a device with
    // no theme keys gets THE OG as its fresh-install presentation.
    if (prefs.isKey("themeHue")) {
        themeHue = prefs.getUShort("themeHue", DEFAULT_THEME_HUE);
        themeStyle = prefs.getUChar("themeStyl", 0);
    } else if (prefs.isKey("theme")) {
        // migrate old theme index → hue+style
        static const uint16_t OLD_HUES[]   = { 330, 30, 0, 0, 20, 120, 340, 0, 30, 120, 120, 140, 260 };
        static const uint8_t  OLD_STYLES[] = {   0,  0, 0, 3,  0,   0,   0, 1,  1,   2,   2,   0,   2 };
        uint8_t oldIdx = prefs.getUChar("theme", 5);
        if (oldIdx > 12) oldIdx = 5;
        themeHue = OLD_HUES[oldIdx];
        themeStyle = OLD_STYLES[oldIdx];
        prefs.remove("theme");
        prefs.putUShort("themeHue", themeHue);
        prefs.putUChar("themeStyl", themeStyle);
    } else {
        themeHue = DEFAULT_THEME_HUE;
        themeStyle = DEFAULT_THEME_STYLE;
    }
    accentMode = prefs.getUChar("accMode", 0);
    if (accentMode > 5) accentMode = 0;
    lightIntensity = prefs.getUChar("lightInt", 0);
    if (lightIntensity > 3) lightIntensity = 0;
    roomParallaxEnabled = prefs.getBool("roomParallax", true);
    {
        char buf[32] = {0};
        prefs.getString("srlName", buf, sizeof(buf));
        if (buf[0] == '\0') strncpy(buf, "SRL1", sizeof(buf) - 1);
        sanitizeName(buf, hamletName);
    }
    pigHeadStyle = static_cast<PigHeadStyle>(prefs.getUChar("pigHead", (uint8_t)PIG_HEAD_THEME));
    if ((uint8_t)pigHeadStyle >= (uint8_t)PIG_HEAD_STYLE_COUNT) pigHeadStyle = PIG_HEAD_THEME;
    
    // Load adaptive hunt settings
    adaptiveHunt = prefs.getBool("adaptHunt", true);
    deauthEnabled = prefs.getBool("deauth", DEFAULT_DEAUTH);
    deauthAggressive = prefs.getBool("deauthAggr", DEFAULT_DEAUTH_AGGRESSIVE);
    excludePMF = prefs.getBool("exclPMF", true);
    excludeWPA3 = prefs.getBool("exclWPA3", true);
    saeAttackEnabled = prefs.getBool("saeAtk", false);
    eapolInjectionEnabled = prefs.getBool("eapolInj", DEFAULT_EAPOL_INJECTION);
    csaEnabled = prefs.getBool("csaAtk", DEFAULT_CSA);
    authFloodEnabled = prefs.getBool("authFlood", false);
    stationaryHopDelay = prefs.getUChar("statHop", 150);
    walkingHopDelay = prefs.getUChar("walkHop", 80);
    
    // Load motion detection settings
    stationaryTimeout = prefs.getUChar("statTime", 10);
    walkingSteps = prefs.getUChar("walkSteps", 3);
    autoHuntEnabled = prefs.getBool("autoHunt", DEFAULT_AUTO_HUNT);
    
    // Load power settings
    shakeWake = prefs.getBool("shakeWake", true);
    alertWake = prefs.getBool("alertWake", true);
    paranoiaEnabled = prefs.getBool("paranoia", false);
    tiltNavigationEnabled = prefs.getBool("tiltNav", false);
    spectrumTiltEnabled = prefs.getBool("specTilt", false);
    batteryAdaptation = prefs.getBool("battAdapt", true);
    pwrFps60 = prefs.getBool("pwrFps60", true);

    // Load catch window settings
    catchWindowCamp = prefs.getUChar("catchCamp", 8);
    catchWindowPatrol = prefs.getUChar("catchPatr", 5);
    
    // Load network activity timeout settings
    activeTimeoutCamp = prefs.getUChar("actTimeCmp", 30);
    activeTimeoutPatrol = prefs.getUChar("actTimePat", 10);
    
    // Load tracker settings
    rssiSmooth = static_cast<Config::RssiSmooth>(prefs.getUChar("rssiSmooth", Config::RSSI_SMOOTH_MED));
    ghostMarkerEnabled = prefs.getBool("ghostMark", true);

    // Load watchlist
    {
        const char* hashKeys[] = { "wl0h", "wl1h", "wl2h", "wl3h", "wl4h", "wl5h" };
        const char* lblKeys[]  = { "wl0l", "wl1l", "wl2l", "wl3l", "wl4l", "wl5l" };
        for (int i = 0; i < MAX_WATCHLIST; i++) {
            memset(&watchlistSlots[i], 0, sizeof(WatchlistSlot));
            uint8_t h[4] = {0};
            size_t got = prefs.getBytes(hashKeys[i], h, 4);
            if (got == 4 && (h[0] | h[1] | h[2] | h[3])) {
                memcpy(watchlistSlots[i].payloadHash, h, 4);
                prefs.getString(lblKeys[i], watchlistSlots[i].label, WATCHLIST_LABEL_LEN);
                watchlistSlots[i].occupied = true;
            }
        }
    }

    // Load UI state
    loreOpenCount = prefs.getUInt("loreOpen", 0);
    catMemoryMask = prefs.getUInt("catMem", 0);
    catLoreSeenMask = prefs.getUInt("catLore", 0);
    // Migrate hintSeen from uint16 → uint32 (NVS type mismatch returns 0)
    uint16_t oldHint = prefs.getUShort("hintSeen", 0);
    if (oldHint != 0) {
        prefs.remove("hintSeen");
        hintSeen = (uint32_t)oldHint;
        prefs.putULong("hintSeen", hintSeen);  // migrate in-place during init
    } else {
        hintSeen = prefs.getULong("hintSeen", 0);
    }
    helpWikiSeen = prefs.getULong("helpWSeen", 0);
    npcChoiceLedger = prefs.getULong("npcChoices", 0);
    npcClosedCastMask = prefs.getULong("npcCast", 0);
    npcCodaSeen = prefs.getBool("npcCoda", false);

    // Load XP progression
    totalXP = prefs.getUInt("xp", 0);
    sessionXPGained = 0;
    sessionXPRaw = 0;
    pendingLevelUp = 0;
    currentStreak = prefs.getUShort("streak", 0);
    bestStreak = prefs.getUShort("bestStreak", 0);
    prestigeCount = prefs.getUChar("prestige", 0);

    // Load session activity tracking
    lastSessionWasActive = prefs.getUChar("sessActive", 0) != 0;
    sessionsBetween = prefs.getUChar("sessBtwn", 0);
    currentSessionActive = false;  // Fresh session starts inactive

    // Load session goals
    lastSessionSteps = prefs.getUInt("sessSteps", 0);
    sessionSteps = 0;  // Fresh session
    goalTarget = prefs.getUShort("goalTarget", SESSION_XP_GOAL_INITIAL_TARGET);
    goalWasMetLastSession = false;  // Reset per boot
    consecutiveGoalsMet = prefs.getUChar("goalConsec", 0);

    // ==[ MIGRATION: base happiness -> XP ]==
    // pre-load capture counts (needed before formal stats load below)
    totalPMKIDs = prefs.getUInt("pmkids", 0);
    totalHandshakes = prefs.getUInt("handshakes", 0);
    if (prefs.isKey("moodBase") && !prefs.isKey("xp")) {
        int8_t oldBase = prefs.getChar("moodBase", 50);
        uint32_t lifetimeCaptures = totalPMKIDs + totalHandshakes;
        totalXP = (uint32_t)max(0, (int)(oldBase * 22)) + lifetimeCaptures * 25;
        prefs.putUInt("xp", totalXP);
        prefs.remove("moodBase");
        prefs.remove("freezeTkn");
        prefs.remove("goalStreak");
    }
    // Clean old keys if they still exist (post-migration boot)
    if (prefs.isKey("moodBase")) prefs.remove("moodBase");
    if (prefs.isKey("freezeTkn")) prefs.remove("freezeTkn");
    if (prefs.isKey("goalStreak")) prefs.remove("goalStreak");

    // ==[ LEGACY MIGRATION ]== old day-based keys
    if (prefs.isKey("lastActDate")) {
        prefs.putUChar("sessActive", 1);
        lastSessionWasActive = true;
        prefs.remove("lastActDate");
        prefs.remove("lastStepDt");
        prefs.remove("todaySteps");
    }
    
    // Load wardrive stats
    wdTotal = prefs.getUInt("wdTotal", 0);
    wdSessions = prefs.getUShort("wdSess", 0);

    // Load capture type collection
    seenAuthTypes = prefs.getUShort("seenAuth", 0);

    // Load stats
    totalPMKIDs = prefs.getUInt("pmkids", 0);
    totalHandshakes = prefs.getUInt("handshakes", 0);
    totalSteps = prefs.getUInt("steps", 0);
    totalDistance = prefs.getUInt("distance", 0);

    // Load coordination settings
    coordinationRole = prefs.getUChar("coordRole", 1);  // Default to slave (1)

    // Load FNOW/3 settings once. Builds before the master gate wrote the
    // individual keys directly, so an explicit old customization is the only
    // upgrade signal allowed to keep coordination on. An untouched install
    // receives the new passive default.
    nowFlockGroupKey = prefs.getUInt("nfGroupKey", DEFAULT_NOWFLOCK_GROUP_KEY);
    nowFlockReportS = constrain(prefs.getUChar("nfReportS", 10),
                                (uint8_t)2, (uint8_t)60);
    nowFlockPigbrother = prefs.getBool("nfPigbrother", false);
    nowFlockExportProfile = prefs.getUChar("nfExportProf", 0);
    if (nowFlockExportProfile > 2) nowFlockExportProfile = 0;
    nowFlockBleHeartbeat = prefs.getBool("nfBleHb", false);
    const bool hadExplicitNowFlockConfig =
        prefs.isKey("nfGroupKey") || prefs.isKey("nfReportS") ||
        nowFlockPigbrother || nowFlockExportProfile != 0 ||
        nowFlockBleHeartbeat;
    nowFlockEnabled = prefs.getBool(
        "nfEnabled", hadExplicitNowFlockConfig ? true : DEFAULT_NOWFLOCK_ENABLED);

    // Load reconnaissance setting
    reconEnabled = prefs.getBool("reconEnabled", false);  // Default to disabled (false)

    // Load IPP defense settings
    ippEnabled = prefs.getBool("ippEnabled", false);
    ippBLEScan = prefs.getBool("ippBle", true);
    ippWifiScan = prefs.getBool("ippWifi", true);
    wifiChaffEnabled = prefs.getBool("wfChaff", false);
    wardriveBleEnabled = prefs.getBool("wdBle", true);

    // Load GPS settings
    gpsEnabled = prefs.getBool("gpsEn", false);
    gpsAlwaysOn = prefs.getBool("gpsAO", false);
    gpsRxPin = prefs.getUChar("gpsRx", GPSPolicy::DEFAULT_RX_PIN);
    gpsTxPin = prefs.getUChar("gpsTx", GPSPolicy::DEFAULT_TX_PIN);
    gpsBaudIdx = prefs.getUChar("gpsBaud", GPSPolicy::DEFAULT_BAUD_INDEX);
    bool gpsConfigRepaired = false;
#if HAMLET_TARGET_CORES3SE
    // Pin-map v0 incorrectly labelled the CoreS3SE G18 landing as physical
    // DIP 6. Preserve the user's selected DIP 6 route by moving it to the
    // actual pin-13 landing (G44), and restore the module's proven baud.
    static constexpr uint8_t GPS_S3_PINMAP_VERSION = 1;
    const uint8_t gpsS3PinmapVersion = prefs.getUChar("gpsS3Map", 0);
    if (gpsS3PinmapVersion < GPS_S3_PINMAP_VERSION) {
        if (gpsEnabled && gpsRxPin == 18) {
            gpsRxPin = 44;
            gpsBaudIdx = GPSPolicy::DEFAULT_BAUD_INDEX;
            gpsConfigRepaired = true;
            HAMLET_LOGLN("[CONFIG] Migrated CoreS3SE GPS DIP 6 route to G44/115200");
        }
        prefs.putUChar("gpsS3Map", GPS_S3_PINMAP_VERSION);
    }
#endif
    if (!GPSPolicy::isSupportedRxPin(gpsRxPin)) {
        HAMLET_LOGF("[CONFIG] GPS RX G%d is not a supported M003-V21 DIP route; using G%d\n",
                    gpsRxPin, GPSPolicy::DEFAULT_RX_PIN);
        gpsRxPin = GPSPolicy::DEFAULT_RX_PIN;
        gpsConfigRepaired = true;
    }
    if (!GPSPolicy::isSupportedTxPin(gpsTxPin)) {
        HAMLET_LOGF("[CONFIG] GPS TX G%d is not a supported M003-V21 DIP route; using G%d\n",
                    gpsTxPin, GPSPolicy::DEFAULT_TX_PIN);
        gpsTxPin = GPSPolicy::DEFAULT_TX_PIN;
        gpsConfigRepaired = true;
    }
    if (gpsBaudIdx >= GPS_BAUD_COUNT) {
        gpsBaudIdx = GPSPolicy::DEFAULT_BAUD_INDEX;
        gpsConfigRepaired = true;
    }
    if (gpsConfigRepaired) markDirty(D_COORD);

    // Load C5Monster settings
    c5Enabled = prefs.getBool("c5En", C5UartPolicy::DEFAULT_ENABLED);
    c5RxPin = prefs.getUChar("c5Rx", C5UartPolicy::RX_PIN);
    c5TxPin = prefs.getUChar("c5Tx", C5UartPolicy::TX_PIN);
    c5Baud = prefs.getUInt("c5Baud", C5UartPolicy::DEFAULT_BAUD);
    bool c5ConfigRepaired = false;
    C5UartPolicy::sanitizePins(c5RxPin, c5TxPin, c5ConfigRepaired);
    if (!C5UartPolicy::isSupportedBaud(c5Baud)) {
        c5Baud = C5UartPolicy::DEFAULT_BAUD;
        c5ConfigRepaired = true;
    }
#if HAMLET_TARGET_CORES3SE
    // Standalone GPS on DIP 6 and the C5 bridge both land on G44/G43. They
    // cannot own those pins simultaneously, so an enabled standalone GPS wins.
    if (gpsEnabled && c5Enabled &&
        (gpsRxPin == c5RxPin || gpsRxPin == c5TxPin ||
         gpsTxPin == c5RxPin || gpsTxPin == c5TxPin)) {
        c5Enabled = false;
        c5ConfigRepaired = true;
        HAMLET_LOGLN("[CONFIG] C5 disabled: standalone GPS owns its UART pins");
    }
#endif
    if (c5ConfigRepaired) markDirty(D_COORD);

    // Load Mesh (Unit C6L) settings
    meshEnabled = prefs.getBool("mshEn", false);
    meshRxPin = prefs.getUChar("mshRx", MeshUartPolicy::RX_PIN);
    meshTxPin = prefs.getUChar("mshTx", MeshUartPolicy::TX_PIN);
    meshBaud = prefs.getUInt("mshBaud", MeshUartPolicy::DEFAULT_BAUD);
    meshCodec = prefs.getUChar("mshCdc",
                               (uint8_t)MeshUartPolicy::Codec::TEXTMSG);
    bool meshConfigRepaired = false;
    if (!MeshUartPolicy::isSupportedCodec(meshCodec)) {
        meshCodec = (uint8_t)MeshUartPolicy::Codec::TEXTMSG;
        meshConfigRepaired = true;
    }
    MeshUartPolicy::sanitize(meshRxPin, meshTxPin, meshBaud, meshConfigRepaired);
    // A half-Port-C, half-Port-B pair is two connectors pretending to be one
    // link. Snap back to the default route rather than reading as a dead radio.
    if (!MeshUartPolicy::isSamePort(meshRxPin, meshTxPin)) {
        meshRxPin = MeshUartPolicy::RX_PIN;
        meshTxPin = MeshUartPolicy::TX_PIN;
        meshConfigRepaired = true;
    }
    // The C6L's default route is Port C, which is the same electrical net the
    // GPS module's DIP table lands on. They cannot share it, and a stacked GPS
    // is the older claim, so it wins — move the C6L to Port B to run both.
    if (meshEnabled) {
        if (const char* owner = meshPinOwner()) {
            meshEnabled = false;
            meshConfigRepaired = true;
            HAMLET_LOGF("[CONFIG] mesh disabled: %s owns its pins\n", owner);
        }
    }
    if (meshConfigRepaired) markDirty(D_COORD);

    // Load WPA-SEC cloud upload settings (stack buffers, zero heap alloc)
    prefs.getString("wpaSecKey", wpaSecKey, sizeof(wpaSecKey));
    if (wpaSecKey[0] == '\0') wpaSecKey[0] = '\0';  // already empty, no-op
    {
        size_t n = prefs.getString("wpaSecUrl", wpaSecUrl, sizeof(wpaSecUrl));
        if (n == 0) strncpy(wpaSecUrl, "https://wpa-sec.stanev.org", sizeof(wpaSecUrl));
    }
    prefs.getString("uplWifiSsid", uploadWifiSsid, sizeof(uploadWifiSsid));
    prefs.getString("uplWifiPass", uploadWifiPass, sizeof(uploadWifiPass));
    prefs.getString("wgl_user", wigleUsername, sizeof(wigleUsername));
    prefs.getString("wgl_tok", wigleToken, sizeof(wigleToken));
    prefs.getString("loot_pin", lootPin, sizeof(lootPin));

    // Load D-UCB channel rewards
    // v3: clear once on upgrade - PMF contamination cleanse
    uint8_t ucb1_ver = prefs.getUChar("ucb1_ver", 0);
    if (ucb1_ver < 3) {
        clearChannelRewards();
        prefs.putUChar("ucb1_ver", 3);
    } else {
        loadChannelRewards();
    }

    // ==[ NVS RANGE VALIDATION ]== guard against corrupted values. Use the
    // same ranges the setters enforce so a stale/corrupt NVS value can't
    // drive brightness to 255 (effectively permanent on) or dimLevel above
    // brightness (which would inverse the dim behavior).
    brightness = constrain(brightness, (uint8_t)10, (uint8_t)100);
    dimLevel = constrain(dimLevel, (uint8_t)0, (uint8_t)50);
    if (dimLevel > brightness) dimLevel = brightness;
    probeThreshold = constrain(probeThreshold, (int8_t)-90, (int8_t)-30);
    sfxVolume = constrain(sfxVolume, (uint8_t)0, (uint8_t)100);
    stationaryTimeout = max((uint8_t)1, stationaryTimeout);
    stationaryHopDelay = max((uint8_t)1, stationaryHopDelay);
    walkingHopDelay = max((uint8_t)1, walkingHopDelay);
    activeTimeoutCamp = max((uint8_t)1, activeTimeoutCamp);
    activeTimeoutPatrol = max((uint8_t)1, activeTimeoutPatrol);
    catchWindowCamp = constrain(catchWindowCamp, (uint8_t)1, (uint8_t)30);
    catchWindowPatrol = constrain(catchWindowPatrol, (uint8_t)1, (uint8_t)30);
    if (themeStyle >= 6) themeStyle = 0;
    hapticIntensity = constrain(hapticIntensity, (uint8_t)0, (uint8_t)255);
}

// forward decl — array defined below save()
uint16_t getChannelRewards(uint8_t channel);

void save() {
    // ==[ SELECTIVE NVS WRITE ]== only flush dirty groups
    // reduces flash wear from 68 keys/flush to ~4-8 keys/flush typical
    uint16_t d = dirtyGroups;
    if (d == 0) {
        Achievements::save();
        pendingSave = false;
        return;
    }

    if (d & D_DISPLAY) {
        prefs.putUChar("brightness", brightness);
        prefs.putUChar("dimLevel", dimLevel);
        prefs.putUShort("dimTimeout", dimTimeout);
        prefs.putBool("dispRot180", displayRotate180);
        prefs.putBool("ledAmb", ledAmbient);
        prefs.putUChar("ledCol", ledColor);
        prefs.putUChar("ledBr", ledBrightness);
        prefs.putUShort("themeHue", themeHue);
        prefs.putUChar("themeStyl", themeStyle);
        prefs.putUChar("accMode", accentMode);
        prefs.putUChar("lightInt", lightIntensity);
        prefs.putBool("roomParallax", roomParallaxEnabled);
        prefs.putString("srlName", hamletName);
        prefs.putUChar("pigHead", (uint8_t)pigHeadStyle);
    }
    if (d & D_SOUND) {
        prefs.putBool("sound", soundEnabled);
        prefs.putUChar("sfxVol", sfxVolume);
        prefs.putUChar("musVol", musicVolume);
#if defined(HAMLET_CORE3SE)
        prefs.putBool("bathMic", bathMicEnabled);
#endif
        prefs.putBool("haptic", hapticEnabled);
        prefs.putUChar("hap_int", hapticIntensity);
    }
    if (d & D_HUNT) {
        prefs.putChar("probeRSSI", probeThreshold);
        prefs.putBool("autoProbe", autoProbe);
        prefs.putBool("adaptHunt", adaptiveHunt);
        prefs.putBool("deauth", deauthEnabled);
        prefs.putBool("deauthAggr", deauthAggressive);
        prefs.putBool("exclPMF", excludePMF);
        prefs.putBool("exclWPA3", excludeWPA3);
        prefs.putBool("saeAtk", saeAttackEnabled);
        prefs.putBool("eapolInj", eapolInjectionEnabled);
        prefs.putBool("csaAtk", csaEnabled);
        prefs.putBool("authFlood", authFloodEnabled);
        prefs.putUChar("statHop", stationaryHopDelay);
        prefs.putUChar("walkHop", walkingHopDelay);
    }
    if (d & D_MOTION) {
        prefs.putUChar("statTime", stationaryTimeout);
        prefs.putUChar("walkSteps", walkingSteps);
        prefs.putBool("autoHunt", autoHuntEnabled);
    }
    if (d & D_POWER) {
        prefs.putBool("shakeWake", shakeWake);
        prefs.putBool("alertWake", alertWake);
        prefs.putBool("paranoia", paranoiaEnabled);
        prefs.putBool("tiltNav", tiltNavigationEnabled);
        prefs.putBool("specTilt", spectrumTiltEnabled);
        prefs.putBool("battAdapt", batteryAdaptation);
        prefs.putBool("pwrFps60", pwrFps60);
    }
    if (d & D_CATCH) {
        prefs.putUChar("catchCamp", catchWindowCamp);
        prefs.putUChar("catchPatr", catchWindowPatrol);
    }
    if (d & D_TIMEOUT) {
        prefs.putUChar("actTimeCmp", activeTimeoutCamp);
        prefs.putUChar("actTimePat", activeTimeoutPatrol);
    }
    if (d & D_NOWFLOCK) {
        prefs.putBool("nfEnabled", nowFlockEnabled);
        prefs.putUInt("nfGroupKey", nowFlockGroupKey);
        prefs.putUChar("nfReportS", nowFlockReportS);
        prefs.putBool("nfPigbrother", nowFlockPigbrother);
        prefs.putUChar("nfExportProf", nowFlockExportProfile);
        prefs.putBool("nfBleHb", nowFlockBleHeartbeat);
    }
    if (d & D_TRACKER) {
        prefs.putUChar("rssiSmooth", static_cast<uint8_t>(rssiSmooth));
        prefs.putBool("ghostMark", ghostMarkerEnabled);
        // watchlist persistence
        const char* hashKeys[] = { "wl0h", "wl1h", "wl2h", "wl3h", "wl4h", "wl5h" };
        const char* lblKeys[]  = { "wl0l", "wl1l", "wl2l", "wl3l", "wl4l", "wl5l" };
        for (int i = 0; i < MAX_WATCHLIST; i++) {
            if (watchlistSlots[i].occupied) {
                prefs.putBytes(hashKeys[i], watchlistSlots[i].payloadHash, 4);
                prefs.putString(lblKeys[i], watchlistSlots[i].label);
            } else {
                prefs.remove(hashKeys[i]);
                prefs.remove(lblKeys[i]);
            }
        }
    }
    if (d & D_UI) {
        prefs.putUInt("loreOpen", loreOpenCount);
        prefs.putUInt("catMem", catMemoryMask);
        prefs.putUInt("catLore", catLoreSeenMask);
        prefs.putULong("hintSeen", hintSeen);
        prefs.putULong("helpWSeen", helpWikiSeen);
        prefs.putULong("npcChoices", npcChoiceLedger);
        prefs.putULong("npcCast", npcClosedCastMask);
        prefs.putBool("npcCoda", npcCodaSeen);
        prefs.putBool("clkTrust", clockTrusted);
    }
    if (d & D_STATS) {
        prefs.putUInt("pmkids", totalPMKIDs);
        prefs.putUInt("handshakes", totalHandshakes);
        prefs.putUInt("steps", totalSteps);
        prefs.putUInt("distance", totalDistance);
        prefs.putUInt("wdTotal", wdTotal);
        prefs.putUShort("wdSess", wdSessions);
        prefs.putUShort("seenAuth", seenAuthTypes);
    }
    if (d & D_COORD) {
        prefs.putUChar("coordRole", coordinationRole);
        prefs.putBool("reconEnabled", reconEnabled);
        prefs.putBool("ippEnabled", ippEnabled);
        prefs.putBool("ippBle", ippBLEScan);
        prefs.putBool("ippWifi", ippWifiScan);
        prefs.putBool("wfChaff", wifiChaffEnabled);
        prefs.putBool("wdBle", wardriveBleEnabled);
        prefs.putBool("gpsEn", gpsEnabled);
        prefs.putBool("gpsAO", gpsAlwaysOn);
        prefs.putUChar("gpsRx", gpsRxPin);
        prefs.putUChar("gpsTx", gpsTxPin);
        prefs.putUChar("gpsBaud", gpsBaudIdx);
        prefs.putBool("c5En", c5Enabled);
        prefs.putUChar("c5Rx", c5RxPin);
        prefs.putUChar("c5Tx", c5TxPin);
        prefs.putUInt("c5Baud", c5Baud);
        prefs.putBool("mshEn", meshEnabled);
        prefs.putUChar("mshRx", meshRxPin);
        prefs.putUChar("mshTx", meshTxPin);
        prefs.putUInt("mshBaud", meshBaud);
        prefs.putUChar("mshCdc", meshCodec);
    }
    if (d & D_UPLOAD) {
        prefs.putString("wpaSecKey", wpaSecKey);
        prefs.putString("wpaSecUrl", wpaSecUrl);
        prefs.putString("uplWifiSsid", uploadWifiSsid);
        prefs.putString("uplWifiPass", uploadWifiPass);
        prefs.putString("wgl_user", wigleUsername);
        prefs.putString("wgl_tok", wigleToken);
        prefs.putString("loot_pin", lootPin);
    }
    if (d & D_DUCB) {
        uint16_t chRew[13];
        for (uint8_t i = 0; i < 13; i++) chRew[i] = getChannelRewards(i + 1);
        prefs.putBytes("ucb1_ch", chRew, sizeof(chRew));
    }
    if (d & D_XP) {
        prefs.putUInt("xp", totalXP);
        prefs.putUShort("streak", currentStreak);
        prefs.putUShort("bestStreak", bestStreak);
        prefs.putUChar("prestige", prestigeCount);
        Achievements::save();  // piggyback on D_XP group
    }
    if (d & D_SESSION) {
        prefs.putUChar("sessActive", currentSessionActive ? 1 : 0);
        prefs.putUChar("sessBtwn", sessionsBetween);
        prefs.putUInt("sessSteps", sessionSteps);
        prefs.putUShort("goalTarget", goalTarget);
        prefs.putUChar("goalConsec", consecutiveGoalsMet);
    }

    // ==[ ACHIEVEMENTS PERSISTENCE ]== ensure achievements are saved even if D_XP wasn't dirty.
    // Achievements can change independently (e.g., stat milestones) and must be flushed separately.
    Achievements::save();

    dirtyGroups = 0;
    pendingSave = false;
}

void update() {
    // Some trophies (passive NPC cases, defense witnesses) can unlock without
    // touching a Config dirty group. Arm the same 5s debounce for those bits.
    if (!pendingSave && Achievements::needsSave()) {
        pendingSave = true;
        lastChangeTime = millis();
    }

    // Deferred save: write after SAVE_DELAY ms of inactivity
    if (pendingSave && (millis() - lastChangeTime >= SAVE_DELAY)) {
        save();
    }
}

static void markDirty(uint16_t group) {
    dirtyGroups |= group;
    pendingSave = true;
    lastChangeTime = millis();
}

// markDirtyAll — marks ALL groups dirty (used by external save() calls)
static void markDirtyAll() {
    dirtyGroups = 0xFFFF;
    pendingSave = true;
    lastChangeTime = millis();
}

uint8_t getBrightness() { return brightness; }
void setBrightness(uint8_t val) {
    brightness = constrain(val, 10, 100);
    if (dimLevel > brightness) dimLevel = brightness;
    markDirty(D_DISPLAY);
}

uint8_t getDimLevel() { return dimLevel; }
void setDimLevel(uint8_t val) {
    dimLevel = constrain(val, 0, 50);
    if (dimLevel > brightness) dimLevel = brightness;
    markDirty(D_DISPLAY);
}

uint16_t getDimTimeout() { return dimTimeout; }
void setDimTimeout(uint16_t val) {
    dimTimeout = constrain(val, 0, 300);
    markDirty(D_DISPLAY);
}

bool getDisplayRotate180() { return displayRotate180; }
void setDisplayRotate180(bool val) {
    displayRotate180 = val;
    markDirty(D_DISPLAY);
}

bool getLedAmbient() { return ledAmbient; }
void setLedAmbient(bool val) {
    ledAmbient = val;
    markDirty(D_DISPLAY);
}
uint8_t getLedColor() { return ledColor; }
void setLedColor(uint8_t val) {
    ledColor = (val > 13) ? 0 : val;
    markDirty(D_DISPLAY);
}
uint8_t getLedBrightness() { return ledBrightness; }
void setLedBrightness(uint8_t val) {
    ledBrightness = (val < 1) ? 1 : (val > 10) ? 10 : val;
    markDirty(D_DISPLAY);
}

bool getSoundEnabled() { return soundEnabled; }
void setSoundEnabled(bool val) {
    soundEnabled = val;
    markDirty(D_SOUND);
}

uint8_t getSfxVolume() { return sfxVolume; }
void setSfxVolume(uint8_t val) {
    sfxVolume = (val > 10) ? 10 : val;
    markDirty(D_SOUND);
}

uint8_t getMusicVolume() { return musicVolume; }
void setMusicVolume(uint8_t val) {
    musicVolume = (val > 10) ? 10 : val;
    markDirty(D_SOUND);
}

bool getBathMicEnabled() {
#if defined(HAMLET_CORE3SE)
    return bathMicEnabled;
#else
    return false;
#endif
}

void setBathMicEnabled(bool val) {
#if defined(HAMLET_CORE3SE)
    bathMicEnabled = val;
    markDirty(D_SOUND);
#else
    (void)val;
#endif
}

bool getHapticEnabled() { return hapticEnabled; }
void setHapticEnabled(bool val) {
    hapticEnabled = val;
    markDirty(D_SOUND);
}

uint8_t getHapticIntensity() { return hapticIntensity; }
void setHapticIntensity(uint8_t val) {
    hapticIntensity = (val > 10) ? 10 : val;
    markDirty(D_SOUND);
}

int8_t getProbeThreshold() { return probeThreshold; }
void setProbeThreshold(int8_t val) {
    probeThreshold = constrain(val, -80, -50);
    markDirty(D_HUNT);
}

bool getAutoProbe() { return autoProbe; }
void setAutoProbe(bool val) {
    autoProbe = val;
    markDirty(D_HUNT);
}

uint16_t getThemeHue() { return themeHue; }
void setThemeHue(uint16_t val) {
    themeHue = val % 360;
    markDirty(D_DISPLAY);
}
uint8_t getThemeStyle() { return themeStyle; }
void setThemeStyle(uint8_t val) {
    themeStyle = val % THEME_STYLE_COUNT;
    markDirty(D_DISPLAY);
}

uint8_t getAccentMode() { return accentMode; }
void setAccentMode(uint8_t val) {
    accentMode = val % 6;
    markDirty(D_DISPLAY);
}
uint8_t getLightIntensity() { return lightIntensity; }
void setLightIntensity(uint8_t val) {
    lightIntensity = val % 4;
    markDirty(D_DISPLAY);
}

bool getRoomParallaxEnabled() { return roomParallaxEnabled; }
void setRoomParallaxEnabled(bool val) {
    roomParallaxEnabled = val;
    markDirty(D_DISPLAY);
}

const char* getDefaultHamletName() { return DEFAULT_HAMLET_NAME; }
const char* getHamletName() { return hamletName; }
void setHamletName(const char* name) {
    char sanitized[5];
    sanitizeName(name, sanitized);
    if (strncmp(hamletName, sanitized, 4) != 0) {
        memcpy(hamletName, sanitized, sizeof(hamletName));
        markDirty(D_DISPLAY);
    }
}

PigHeadStyle getPigHeadStyle() { return pigHeadStyle; }
void setPigHeadStyle(PigHeadStyle style) {
    if ((uint8_t)style >= (uint8_t)PIG_HEAD_STYLE_COUNT) style = PIG_HEAD_THEME;
    if (pigHeadStyle != style) {
        pigHeadStyle = style;
        markDirty(D_DISPLAY);
    }
}

// === ADAPTIVE HUNT MODE ===

bool getAdaptiveHunt() { return adaptiveHunt; }
void setAdaptiveHunt(bool val) { adaptiveHunt = val; markDirty(D_HUNT); }

bool getDeauthEnabled() { return deauthEnabled; }
void setDeauthEnabled(bool val) { deauthEnabled = val; markDirty(D_HUNT); }

bool getDeauthAggressive() { return deauthAggressive; }
void setDeauthAggressive(bool val) { deauthAggressive = val; markDirty(D_HUNT); }

bool getExcludePMF() { return excludePMF; }
void setExcludePMF(bool val) { excludePMF = val; markDirty(D_HUNT); }

bool getExcludeWPA3() { return excludeWPA3; }
void setExcludeWPA3(bool val) { excludeWPA3 = val; markDirty(D_HUNT); }

bool getSAEAttackEnabled() { return saeAttackEnabled; }
void setSAEAttackEnabled(bool val) { saeAttackEnabled = val; markDirty(D_HUNT); }

bool getEAPOLInjectionEnabled() { return eapolInjectionEnabled; }
void setEAPOLInjectionEnabled(bool val) { eapolInjectionEnabled = val; markDirty(D_HUNT); }

bool getCSAEnabled() { return csaEnabled; }
void setCSAEnabled(bool val) { csaEnabled = val; markDirty(D_HUNT); }

bool getAuthFloodEnabled() { return authFloodEnabled; }
void setAuthFloodEnabled(bool val) { authFloodEnabled = val; markDirty(D_HUNT); }

uint8_t getStationaryHopDelay() { return stationaryHopDelay; }
void setStationaryHopDelay(uint8_t val) { stationaryHopDelay = constrain(val, 50, 250); markDirty(D_HUNT); }

uint8_t getWalkingHopDelay() { return walkingHopDelay; }
void setWalkingHopDelay(uint8_t val) { walkingHopDelay = constrain(val, 30, 150); markDirty(D_HUNT); }

// === MOTION DETECTION SETTINGS ===

uint8_t getStationaryTimeout() { return stationaryTimeout; }
void setStationaryTimeout(uint8_t val) { stationaryTimeout = constrain(val, 5, 30); markDirty(D_MOTION); }

uint8_t getWalkingSteps() { return walkingSteps; }
void setWalkingSteps(uint8_t val) { walkingSteps = constrain(val, 1, 10); markDirty(D_MOTION); }

bool getAutoHuntEnabled() { return autoHuntEnabled; }
void setAutoHuntEnabled(bool val) { autoHuntEnabled = val; markDirty(D_MOTION); }

// === POWER SETTINGS ===

bool getShakeWake() { return shakeWake; }
void setShakeWake(bool val) { shakeWake = val; markDirty(D_POWER); }

bool getAlertWake() { return alertWake; }
void setAlertWake(bool val) { alertWake = val; markDirty(D_POWER); }

bool getParanoiaEnabled() { return paranoiaEnabled; }
void setParanoiaEnabled(bool val) { paranoiaEnabled = val; markDirty(D_POWER); }

bool getTiltNavigationEnabled() { return tiltNavigationEnabled; }
void setTiltNavigationEnabled(bool val) { tiltNavigationEnabled = val; markDirty(D_POWER); }

bool getSpectrumTiltEnabled() { return spectrumTiltEnabled; }
void setSpectrumTiltEnabled(bool val) { spectrumTiltEnabled = val; markDirty(D_POWER); }

bool getBatteryAdaptation() { return batteryAdaptation; }
void setBatteryAdaptation(bool val) { batteryAdaptation = val; markDirty(D_POWER); }

bool getPwrFps60() { return pwrFps60; }
void setPwrFps60(bool val) { pwrFps60 = val; markDirty(D_POWER); }

// === CATCH WINDOW SETTINGS ===

uint8_t getCatchWindowCamp() { return catchWindowCamp; }
void setCatchWindowCamp(uint8_t val) { catchWindowCamp = constrain(val, 4, 15); markDirty(D_CATCH); }

uint8_t getCatchWindowPatrol() { return catchWindowPatrol; }
void setCatchWindowPatrol(uint8_t val) { catchWindowPatrol = constrain(val, 2, 10); markDirty(D_CATCH); }

// === NETWORK ACTIVITY TIMEOUT SETTINGS ===

uint8_t getActiveTimeoutCamp() { return activeTimeoutCamp; }
void setActiveTimeoutCamp(uint8_t val) { activeTimeoutCamp = constrain(val, 10, 60); markDirty(D_TIMEOUT); }

uint8_t getActiveTimeoutPatrol() { return activeTimeoutPatrol; }
void setActiveTimeoutPatrol(uint8_t val) { activeTimeoutPatrol = constrain(val, 5, 30); markDirty(D_TIMEOUT); }

uint32_t getLoreOpenCount() { return loreOpenCount; }
void setLoreOpenCount(uint32_t val) { loreOpenCount = val; markDirty(D_UI); }

uint32_t getCatMemoryMask() { return catMemoryMask; }
void setCatMemoryMask(uint32_t val) {
    if (catMemoryMask == val) return;
    catMemoryMask = val;
    markDirty(D_UI);
}

uint32_t getCatLoreSeenMask() { return catLoreSeenMask; }
void setCatLoreSeenMask(uint32_t val) {
    if (catLoreSeenMask == val) return;
    catLoreSeenMask = val;
    markDirty(D_UI);
}

uint32_t getHintSeen() { return hintSeen; }
void setHintSeen(uint32_t val) { hintSeen = val; markDirty(D_UI); }

uint32_t getHelpWikiSeen() { return helpWikiSeen; }
void setHelpWikiSeen(uint32_t val) { helpWikiSeen = val; markDirty(D_UI); }

uint32_t getNpcChoiceLedger() { return npcChoiceLedger; }
void setNpcChoiceLedger(uint32_t val) {
    if (npcChoiceLedger == val) return;
    npcChoiceLedger = val;
    markDirty(D_UI);
}

uint32_t getNpcClosedCastMask() { return npcClosedCastMask; }
void setNpcClosedCastMask(uint32_t val) {
    if (npcClosedCastMask == val) return;
    npcClosedCastMask = val;
    markDirty(D_UI);
}

bool getNpcCodaSeen() { return npcCodaSeen; }
void setNpcCodaSeen(bool seen) {
    if (npcCodaSeen == seen) return;
    npcCodaSeen = seen;
    markDirty(D_UI);
}

uint32_t getTotalPMKIDs() { return totalPMKIDs; }
void incrementTotalPMKIDs() {
    totalPMKIDs++;
    markDirty(D_STATS);
    markSessionActive();
}

uint32_t getTotalHandshakes() { return totalHandshakes; }
void incrementTotalHandshakes() {
    totalHandshakes++;
    markDirty(D_STATS);
    markSessionActive();
}

uint32_t getTotalSteps() { return totalSteps; }
void addSteps(uint32_t steps) {
    totalSteps += steps;
    uint32_t meters = (uint32_t)(steps * STEP_LENGTH_M);
    totalDistance += meters;
    markDirty(D_STATS);
}

uint32_t getTotalDistance() { return totalDistance; }

// === WARDRIVE STATS ===
uint32_t getWDTotal() { return wdTotal; }
void addWDTotal(uint32_t count) { wdTotal += count; markDirty(D_STATS); }
uint16_t getWDSessions() { return wdSessions; }
void incrementWDSessions() { wdSessions++; markDirty(D_STATS); }

// === CAPTURE TYPE COLLECTION ===
uint16_t getSeenAuthTypes() { return seenAuthTypes; }
bool markAuthTypeSeen(uint8_t authType) {
    if (authType > 15) return false;  // wifi_auth_mode_t fits in 4 bits
    uint16_t bit = 1u << authType;
    if (seenAuthTypes & bit) return false;  // already seen
    seenAuthTypes |= bit;
    markDirty(D_STATS);
    return true;  // first discovery!
}

// === D-UCB Channel Rewards ===
// Store rewards for channels 1-13 to persist learning across reboots
static uint16_t channelRewards[14] = {0};  // Index 0 unused

uint16_t getChannelRewards(uint8_t channel) {
    if (channel < 1 || channel > 13) return 0;
    return channelRewards[channel];
}

void setChannelRewards(uint8_t channel, uint16_t rewards) {
    if (channel < 1 || channel > 13) return;
    channelRewards[channel] = rewards;
}

void saveChannelRewards() {
    markDirty(D_DUCB);
}

void loadChannelRewards() {
    size_t len = prefs.getBytesLength("ucb1_ch");
    if (len == 13 * sizeof(uint16_t)) {
        prefs.getBytes("ucb1_ch", &channelRewards[1], len);
    }
}

void clearChannelRewards() {
    // tabula rasa. pmf contamination cleansed
    memset(channelRewards, 0, sizeof(channelRewards));
    prefs.remove("ucb1_ch");
}

// === XP PROGRESSION ===

// XP curve: cumulative XP for level L = 100 * L^1.8
static uint32_t levelToXP(uint8_t level) {
    if (level <= 1) return 0;
    return (uint32_t)(100.0f * powf((float)level, 1.8f));
}

// Inverse: given cumulative XP, what level?
static uint8_t xpToLevel(uint32_t xp) {
    if (xp == 0) return 1;
    // solve L = (xp/100)^(1/1.8)
    uint8_t level = (uint8_t)powf((float)xp / 100.0f, 1.0f / 1.8f);
    // clamp and verify (powf rounding)
    if (level < 1) level = 1;
    while (level < MAX_LEVEL && levelToXP(level + 1) <= xp) level++;
    if (level > MAX_LEVEL) level = MAX_LEVEL;
    return level;
}

static void unlockRankAchievements(uint8_t level) {
    if (level >= 7)  Achievements::tryUnlock(Achievement::RANK_SHOAT);
    if (level >= 14) Achievements::tryUnlock(Achievement::RANK_BOAR);
    if (level >= 21) Achievements::tryUnlock(Achievement::RANK_TUSKER);
    if (level >= 28) Achievements::tryUnlock(Achievement::RANK_WARTHOG);
    if (level >= 35) Achievements::tryUnlock(Achievement::RANK_RAZORBACK);
    if (level >= 42) Achievements::tryUnlock(Achievement::RANK_ELDER);
}

static uint32_t rewardSourceCap(RewardSource source) {
    switch (source) {
        case RewardSource::RECON:
            return SESSION_XP_RECON_CAP;
        case RewardSource::STATION:
            return SESSION_XP_STATION_CAP;
        case RewardSource::TRACKER_SURVIVAL:
            return SESSION_XP_TRACKER_SURVIVAL_CAP;
        case RewardSource::WALK_MILESTONE:
            return SESSION_XP_WALK_MILESTONE_CAP;
        case RewardSource::XP_EVENT:
            return SESSION_XP_EVENT_CAP;
        case RewardSource::CAT_MEMORY:
            return SESSION_XP_CAT_MEMORY_CAP;
        default:
            return UINT32_MAX;
    }
}

static uint32_t clampRewardAmountBySource(uint32_t amount, RewardSource source) {
    if (source == RewardSource::UNKNOWN || source == RewardSource::COUNT) return amount;

    uint32_t cap = rewardSourceCap(source);
    if (cap == UINT32_MAX) return amount;

    uint8_t idx = static_cast<uint8_t>(source);
    if (idx >= static_cast<uint8_t>(RewardSource::COUNT)) return amount;
    uint32_t used = rewardSourceXPAwarded[idx];
    if (used >= cap) return 0;

    uint32_t remaining = cap - used;
    uint32_t grant = (amount > remaining) ? remaining : amount;
    rewardSourceXPAwarded[idx] = used + grant;
    return grant;
}

static void resetRewardSourceBudgets() {
    for (uint8_t i = 0; i < static_cast<uint8_t>(RewardSource::COUNT); i++) {
        rewardSourceXPAwarded[i] = 0;
    }
}

uint32_t getXP() { return totalXP; }

void addXP(uint32_t amount) {
    if (amount == 0) return;

    // soft cap: beyond 500 raw XP/session, awards reduced to 20%
    sessionXPRaw += amount;
    if (sessionXPRaw > SESSION_XP_SOFT_CAP) {
        uint32_t underCap = (sessionXPRaw - amount >= SESSION_XP_SOFT_CAP) ? 0 : (SESSION_XP_SOFT_CAP - (sessionXPRaw - amount));
        uint32_t overCap = amount - underCap;
        amount = underCap + (uint32_t)(overCap * SESSION_XP_OVERFLOW_MULT);
        if (amount == 0) amount = 1;  // always award at least 1
    }
    sessionXPGained += amount;

    pendingXPDisplay += amount;  // feed notification strip (consumed by Display each frame)

    uint8_t oldLevel = xpToLevel(totalXP);
    totalXP += amount;

    uint8_t newLevel = xpToLevel(totalXP);
    if (newLevel > oldLevel) {
        pendingLevelUp = newLevel;
        unlockRankAchievements(newLevel);
        uint8_t levelLuck = newLevel / 4;
        if (levelLuck > 10) levelLuck = 10;
        ItemDrops::awardGuaranteed(ItemDrops::ItemDropSource::LEVEL_UP, levelLuck);
    }

    markDirty(D_XP);
}

void addXP(uint32_t amount, RewardSource source) {
    if (amount == 0) return;

    uint32_t clamped = clampRewardAmountBySource(amount, source);
    if (clamped == 0) return;

    addXP(clamped);
}

uint8_t getLevel() { return xpToLevel(totalXP); }
bool isElder() { return getLevel() >= MAX_LEVEL || prestigeCount > 0; }

uint8_t getPrestigeCount() { return prestigeCount; }

// ==[ PRESTIGE / REINCARNATION ]==
// Cosmetic unlock ladder: L20 → ROSE. L30 → ICE. L42 → GOLD. prestige 1 → FEDORA. prestige 2+ → HYPE.
uint8_t getMaxUnlockedPigStyle() {
    if (prestigeCount >= 2) return (uint8_t)PIG_HEAD_HYPE;
    if (prestigeCount >= 1) return (uint8_t)PIG_HEAD_FEDORA;
    if (getLevel() >= MAX_LEVEL) return (uint8_t)PIG_HEAD_GOLD;
    if (getLevel() >= 30) return (uint8_t)PIG_HEAD_ICE;
    if (getLevel() >= 20) return (uint8_t)PIG_HEAD_ROSE;
    return (uint8_t)PIG_HEAD_THEME;
}

void doPrestige() {
    if (!isElder() || getLevel() < MAX_LEVEL) return;  // must currently be L42
    prestigeCount++;
    totalXP = 0;
    sessionXPGained = 0;
    sessionXPRaw = 0;
    pendingLevelUp = 0;
    pendingXPDisplay = 0;
    markDirty(D_XP);
}

uint8_t getXPProgress() {
    uint8_t level = getLevel();
    if (level >= MAX_LEVEL) return 100;
    uint32_t currentLevelXP = levelToXP(level);
    uint32_t nextLevelXP = levelToXP(level + 1);
    uint32_t delta = nextLevelXP - currentLevelXP;
    if (delta == 0) return 100;
    uint32_t progress = totalXP - currentLevelXP;
    uint32_t pct = (progress * 100) / delta;
    return (uint8_t)(pct > 100 ? 100 : pct);
}

uint32_t getXPForNextLevel() {
    uint8_t level = getLevel();
    if (level >= MAX_LEVEL) return 0;
    return levelToXP(level + 1) - totalXP;
}

uint32_t getSessionXPGained() { return sessionXPGained; }

static const char* rankForLevel(uint8_t level) {
    if (level >= 42) return "3LD3R";
    if (level >= 35) return "R4Z0RB4CK";
    if (level >= 28) return "W4RTH0G";
    if (level >= 21) return "TUSK3R";
    if (level >= 14) return "B0AR";
    if (level >= 7)  return "SH0AT";
    return "RUNT";
}

const char* getRankName() { return rankForLevel(getLevel()); }
const char* getRankName(uint8_t level) { return rankForLevel(level); }

bool hasLevelUp() { return pendingLevelUp > 0; }
uint8_t consumeLevelUp() {
    uint8_t lvl = pendingLevelUp;
    pendingLevelUp = 0;
    return lvl;
}
uint32_t consumePendingXPDisplay() {
    uint32_t v = pendingXPDisplay;
    pendingXPDisplay = 0;
    return v;
}

uint16_t getStreak() { return currentStreak; }

void setStreak(uint16_t val) { currentStreak = val; markDirty(D_XP); }

uint16_t getBestStreak() { return bestStreak; }
void setBestStreak(uint16_t val) { bestStreak = val; markDirty(D_XP); }

uint8_t getSessionPMKIDCount() { return sessionPMKIDCount; }
uint8_t getSessionHSCount() { return sessionHSCount; }

void incrementSessionPMKIDCount() { sessionPMKIDCount++; }
void incrementSessionHSCount() { sessionHSCount++; }

bool hasTrustedClock() {
    return clockTrusted && (getTrustedEpoch() > 0);
}

bool isRtcValid() {
    return hasTrustedClock();
}

uint32_t getTrustedEpoch() {
    if (!clockTrusted) return 0;

    time_t now = time(nullptr);
    if (now > 1704067200) {  // Jan 1 2024 sanity check
        return (uint32_t)now;
    }

    m5::rtc_datetime_t dt = {};
    if (!readRtcDateTime(&dt)) return 0;
    return rtcDateTimeToEpoch(dt);
}

uint32_t getRtcDate() {
    uint32_t epoch = getTrustedEpoch();
    if (epoch == 0) return 0;

    time_t now = (time_t)epoch;
    struct tm timeinfo;
    gmtime_r(&now, &timeinfo);
    return (uint32_t)(timeinfo.tm_year + 1900) * 10000 +
           (uint32_t)(timeinfo.tm_mon + 1) * 100 +
           (uint32_t)timeinfo.tm_mday;
}

void markClockSynced() {
    m5::rtc_datetime_t dt = {};
    if (!readRtcDateTime(&dt)) return;

    clockTrusted = true;
    syncSystemTimeFromRtc();
    markDirty(D_UI);
    // NTP callers use this seam to trust the hardware clock they just updated.
}

void adoptSyncEpochMin(uint32_t epochMin, uint32_t masterUptimeMs, uint32_t localNowMs) {
    if (GPS::hasFix() || hasTrustedClock()) return;
    if (epochMin == 0) return;

    uint32_t driftMin = 0;
    if (localNowMs >= masterUptimeMs) {
        driftMin = (localNowMs - masterUptimeMs) / 60000u;
    }
    uint32_t correctedMin = epochMin + driftMin;
    time_t epoch = (time_t)correctedMin * 60;

    struct tm timeinfo;
    gmtime_r(&epoch, &timeinfo);

#ifndef NATIVE_TEST
    if (hasHardwareRTC) {
        m5::rtc_datetime_t rtcTime = {};
        rtcTime.date.year = (uint16_t)(timeinfo.tm_year + 1900);
        rtcTime.date.month = (uint8_t)(timeinfo.tm_mon + 1);
        rtcTime.date.date = (uint8_t)timeinfo.tm_mday;
        rtcTime.time.hours = (uint8_t)timeinfo.tm_hour;
        rtcTime.time.minutes = (uint8_t)timeinfo.tm_min;
        rtcTime.time.seconds = (uint8_t)timeinfo.tm_sec;
        M5.Rtc.setDateTime(rtcTime);
    } else {
        timeval tv = {};
        tv.tv_sec = epoch;
        settimeofday(&tv, nullptr);
    }
#endif

    clockTrusted = true;
    syncSystemTimeFromRtc();
    markDirty(D_UI);
}

void updateSessionStreak() {
    // simplified: active session = streak++, XP. idle = silent reset after 2.

    if (lastSessionWasActive) {
        sessionsBetween = 0;
        currentStreak++;
        addXP(SESSION_XP_STREAK_SHOWUP_BONUS, RewardSource::STREAK);  // showing up bonus

        if (currentStreak > bestStreak) {
            bestStreak = currentStreak;
            addXP(SESSION_XP_STREAK_RECORD_BONUS, RewardSource::STREAK);  // new record bonus
        }
    } else {
        sessionsBetween++;
        if (sessionsBetween >= 3 && currentStreak > 0) {
            // harsh: lose 50% of streak
            uint16_t loss = currentStreak / 2;
            if (loss < 2) loss = 2;
            currentStreak = (currentStreak > loss) ? currentStreak - loss : 0;
        } else if (sessionsBetween >= 2 && currentStreak > 0) {
            // moderate: lose 25% of streak
            uint16_t loss = currentStreak / 4;
            if (loss < 1) loss = 1;
            currentStreak = (currentStreak > loss) ? currentStreak - loss : 0;
        }
    }

    // new session starts inactive
    currentSessionActive = false;

    // deferred save — batches all boot-time writes into single NVS flush
    markDirty(D_XP | D_SESSION);
}

void markSessionActive() {
    if (currentSessionActive) return;
    resetRewardSourceBudgets();
    currentSessionActive = true;
    markDirty(D_SESSION);
    SFX::play(SFX::SESSION_ACTIVE);

    // midnight boot is not a night shift. clock in after real engagement.
    m5::rtc_datetime_t dt = {};
    if (hasTrustedClock() && readRtcDateTime(&dt) && dt.time.hours < 5) {
        Achievements::tryUnlock(Achievement::NIGHT_OWL);
    }

    Challenges::onSessionActivated();
    // A live session restores the evidence burn budget, so the locker is a
    // per-session decision rather than a one-time spend.
    ItemEffects::onSessionStart();
}

bool isSessionActive() {
    return currentSessionActive;
}

bool isStreakAtRisk() {
    return sessionsBetween >= 1 && currentStreak > 0;
}

// === SESSION GOALS ===

void evaluateSessionGoal() {
    // evaluate last session's step goal. award XP directly.

    if (lastSessionSteps == 0 && goalTarget == SESSION_XP_GOAL_INITIAL_TARGET &&
        !lastSessionWasActive) {
        // first boot or fresh install
        sessionSteps = 0;
        goalTarget = SESSION_XP_GOAL_INITIAL_TARGET;
        goalWasMetLastSession = false;
        goalCloseTriggered = false;
        goalCompleteTriggered = false;
        return;
    }

    if (lastSessionSteps >= goalTarget) {
        // goal met! XP = 50 + goalTarget/100 (50-110 range)
        uint32_t xpReward = SESSION_XP_GOAL_BASE_BONUS + (goalTarget / SESSION_XP_GOAL_STEP_DIV);
        addXP(xpReward, RewardSource::GOAL);
        ItemDrops::awardGuaranteed(ItemDrops::ItemDropSource::GOAL, 3);

        // increase goal for next session (cap at 6000)
        goalTarget = min((uint16_t)SESSION_XP_GOAL_MAX_TARGET, (uint16_t)(goalTarget + SESSION_XP_GOAL_STEP_BONUS));
        goalWasMetLastSession = true;

        // ==[ GOAL STREAK ]== consecutive goals met
        consecutiveGoalsMet++;
        if (consecutiveGoalsMet >= 5) {
            Achievements::tryUnlock(Achievement::GOAL_STREAK_5);
        }
    } else if (lastSessionSteps > 0) {
        // had steps but missed - decrease (floor at 500)
        if (goalTarget <= SESSION_XP_GOAL_MIN_TARGET + SESSION_XP_GOAL_FAIL_DECAY) {
            goalTarget = SESSION_XP_GOAL_MIN_TARGET;
        } else {
            goalTarget = (uint16_t)(goalTarget - SESSION_XP_GOAL_FAIL_DECAY);
        }
        goalWasMetLastSession = false;
        consecutiveGoalsMet = 0;
    } else {
        goalWasMetLastSession = false;
        // An active radio/capture session with zero steps still missed the
        // walking goal. Truly idle boots continue to pause the chain.
        if (lastSessionWasActive) consecutiveGoalsMet = 0;
    }

    // reset for new session
    sessionSteps = 0;
    goalCloseTriggered = false;
    goalCompleteTriggered = false;

    // deferred save — batches goal-related writes into single NVS flush
    markDirty(D_SESSION);
}

uint32_t getSessionSteps() { return sessionSteps; }

void incrementSessionSteps() {
    sessionSteps++;
}

void persistSessionSteps() {
    markDirty(D_SESSION);
}

uint16_t getGoalTarget() { return goalTarget; }

uint8_t getGoalProgress() {
    if (sessionSteps >= goalTarget) return 100;

    // Prevent overflow: cast check before uint8_t conversion
    uint32_t progress32 = (sessionSteps * 100) / goalTarget;
    if (progress32 > 100) return 100;  // Clamp to 100%

    return (uint8_t)progress32;
}


bool wasGoalMetLastSession() { return goalWasMetLastSession; }

void clearGoalMetFlag() { goalWasMetLastSession = false; }

// Phase 2.5: Goal progress triggers
bool wasGoalCloseTriggered() { return goalCloseTriggered; }
void setGoalCloseTriggered() { goalCloseTriggered = true; }
bool wasGoalCompleteTriggered() { return goalCompleteTriggered; }
void setGoalCompleteTriggered() { goalCompleteTriggered = true; }

// ==[ TRACKER SETTINGS ]==
Config::RssiSmooth getRssiSmooth() { return rssiSmooth; }
void setRssiSmooth(Config::RssiSmooth val) { rssiSmooth = val; markDirty(D_TRACKER); }

bool getGhostMarkerEnabled() { return ghostMarkerEnabled; }
void setGhostMarkerEnabled(bool val) { ghostMarkerEnabled = val; markDirty(D_TRACKER); }

// ==[ BLE WATCHLIST ]==
uint8_t getWatchlistCount() {
    uint8_t n = 0;
    for (int i = 0; i < MAX_WATCHLIST; i++) if (watchlistSlots[i].occupied) n++;
    return n;
}

bool getWatchlistSlot(uint8_t idx, WatchlistSlot& out) {
    if (idx >= MAX_WATCHLIST) return false;
    out = watchlistSlots[idx];
    return out.occupied;
}

bool setWatchlistSlot(uint8_t idx, const uint8_t* hash, const char* label) {
    if (idx >= MAX_WATCHLIST || !hash || !label) return false;
    memcpy(watchlistSlots[idx].payloadHash, hash, 4);
    strncpy(watchlistSlots[idx].label, label, WATCHLIST_LABEL_LEN - 1);
    watchlistSlots[idx].label[WATCHLIST_LABEL_LEN - 1] = '\0';
    watchlistSlots[idx].occupied = true;
    markDirty(D_TRACKER);
    return true;
}

void clearWatchlistSlot(uint8_t idx) {
    if (idx >= MAX_WATCHLIST) return;
    memset(&watchlistSlots[idx], 0, sizeof(WatchlistSlot));
    markDirty(D_TRACKER);
}

// ==[ COORDINATION SETTINGS ]==
uint8_t getCoordinationRole() { return coordinationRole; }
void setCoordinationRole(uint8_t role) {
    if (role <= 2) {  // Validate: 0=master, 1=slave, 2=standalone
        coordinationRole = role;
        markDirty(D_COORD);
    }
}

// ==[ RECONNAISSANCE SETTINGS ]==
bool getReconEnabled() { return reconEnabled; }
void setReconEnabled(bool enabled) {
    reconEnabled = enabled;
    markDirty(D_COORD);
}

// ==[ IPP DEFENSE SETTINGS ]==
bool getIppEnabled() { return ippEnabled; }
void setIppEnabled(bool enabled) { ippEnabled = enabled; markDirty(D_COORD); }
bool getIppBLEScan() { return ippBLEScan; }
void setIppBLEScan(bool enabled) { ippBLEScan = enabled; markDirty(D_COORD); }
bool getIppWifiScan() { return ippWifiScan; }
void setIppWifiScan(bool enabled) { ippWifiScan = enabled; markDirty(D_COORD); }

// ==[ HOGWASH ]==
bool getWifiChaffEnabled() { return wifiChaffEnabled; }
void setWifiChaffEnabled(bool val) { wifiChaffEnabled = val; markDirty(D_COORD); }

// ==[ WARDRIVE BLE ]==
bool getWardriveBleScan() { return wardriveBleEnabled; }
void setWardriveBleScan(bool val) { wardriveBleEnabled = val; markDirty(D_COORD); }

// ==[ GPS SETTINGS ]==
bool getGPSEnabled() { return gpsEnabled; }
void setGPSEnabled(bool val) { gpsEnabled = val; markDirty(D_COORD); }
uint8_t getGPSRxPin() { return gpsRxPin; }
void setGPSRxPin(uint8_t pin) {
    if (!GPSPolicy::isSupportedRxPin(pin)) return;
    gpsRxPin = pin;
    markDirty(D_COORD);
}
uint8_t getGPSTxPin() { return gpsTxPin; }
void setGPSTxPin(uint8_t pin) {
    if (!GPSPolicy::isSupportedTxPin(pin)) return;
    gpsTxPin = pin;
    markDirty(D_COORD);
}
uint8_t getGPSBaudIndex() { return gpsBaudIdx; }
void setGPSBaudIndex(uint8_t idx) { if (idx < GPS_BAUD_COUNT) { gpsBaudIdx = idx; markDirty(D_COORD); } }
uint32_t getGPSBaud() { return GPS_BAUD_TABLE[gpsBaudIdx < GPS_BAUD_COUNT ? gpsBaudIdx : 0]; }
bool getGPSAlwaysOn() { return gpsAlwaysOn; }
void setGPSAlwaysOn(bool val) { gpsAlwaysOn = val; markDirty(D_COORD); }

// ==[ C5MONSTER SETTINGS ]==
bool getC5Enabled() { return c5Enabled; }
void setC5Enabled(bool val) { c5Enabled = val; markDirty(D_COORD); }
uint8_t getC5RxPin() { return c5RxPin; }
void setC5RxPin(uint8_t pin) {
    uint8_t nextRx = pin;
    uint8_t nextTx = c5TxPin;
    bool repaired = false;
    C5UartPolicy::sanitizePins(nextRx, nextTx, repaired);
    if (nextRx != c5RxPin || nextTx != c5TxPin) {
        c5RxPin = nextRx;
        c5TxPin = nextTx;
        markDirty(D_COORD);
    }
}
uint8_t getC5TxPin() { return c5TxPin; }
void setC5TxPin(uint8_t pin) {
    uint8_t nextRx = c5RxPin;
    uint8_t nextTx = pin;
    bool repaired = false;
    C5UartPolicy::sanitizePins(nextRx, nextTx, repaired);
    if (nextRx != c5RxPin || nextTx != c5TxPin) {
        c5RxPin = nextRx;
        c5TxPin = nextTx;
        markDirty(D_COORD);
    }
}
uint32_t getC5Baud() { return c5Baud; }
void setC5Baud(uint32_t baud) {
    if (!C5UartPolicy::isSupportedBaud(baud)) baud = C5UartPolicy::DEFAULT_BAUD;
    if (c5Baud == baud) return;
    c5Baud = baud;
    markDirty(D_COORD);
}

// ==[ MESH SETTINGS ]==
bool getMeshEnabled() { return meshEnabled; }
void setMeshEnabled(bool val) { meshEnabled = val; markDirty(D_COORD); }
uint8_t getMeshRxPin() { return meshRxPin; }
// The pins move as a pair. A Grove port is one connector, so selecting a route
// picks both legs of it — offering them independently only ever produced the
// half-Port-C, half-Port-B pair that init() has to repair.
void setMeshRxPin(uint8_t pin) {
    if (!MeshUartPolicy::isSupportedRxPin(pin) || pin == meshRxPin) return;
    for (uint8_t i = 0; i < MeshUartPolicy::RX_PIN_COUNT; ++i) {
        if (MeshUartPolicy::RX_PINS[i] != pin) continue;
        meshRxPin = pin;
        meshTxPin = MeshUartPolicy::TX_PINS[i];
        markDirty(D_COORD);
        return;
    }
}
uint8_t getMeshTxPin() { return meshTxPin; }
uint32_t getMeshBaud() { return meshBaud; }
MeshUartPolicy::Codec getMeshCodec() {
    return (MeshUartPolicy::Codec)meshCodec;
}
void setMeshCodec(MeshUartPolicy::Codec codec) {
    const uint8_t raw = (uint8_t)codec;
    if (raw == meshCodec || !MeshUartPolicy::isSupportedCodec(raw)) return;
    meshCodec = raw;
    markDirty(D_COORD);
}
void setMeshBaud(uint32_t baud) {
    if (!MeshUartPolicy::isSupportedBaud(baud)) baud = MeshUartPolicy::DEFAULT_BAUD;
    if (meshBaud == baud) return;
    meshBaud = baud;
    markDirty(D_COORD);
}

// ==[ WPA-SEC CLOUD UPLOAD ]==
const char* getWpaSecKey() { return wpaSecKey; }
void setWpaSecKey(const char* key) {
    if (key) {
        while (*key && isspace((unsigned char)*key)) key++;
        size_t len = strlen(key);
        while (len > 0 && isspace((unsigned char)key[len - 1])) len--;
        if (len <= 32) {
            memcpy(wpaSecKey, key, len);
            wpaSecKey[len] = '\0';
            for (size_t i = 0; i < len; i++) {
                wpaSecKey[i] = (char)tolower((unsigned char)wpaSecKey[i]);
            }
        } else {
            wpaSecKey[0] = '\0';
        }
    } else {
        wpaSecKey[0] = '\0';
    }
    markDirty(D_UPLOAD);
}
bool hasWpaSecKey() {
    if (strlen(wpaSecKey) != 32) return false;
    for (size_t i = 0; i < 32; i++) {
        if (!isxdigit((unsigned char)wpaSecKey[i])) return false;
    }
    return true;
}

const char* getWpaSecUrl() { return wpaSecUrl; }
void setWpaSecUrl(const char* url) {
    if (url && url[0]) {
        strncpy(wpaSecUrl, url, sizeof(wpaSecUrl) - 1);
        wpaSecUrl[sizeof(wpaSecUrl) - 1] = '\0';
    } else {
        snprintf(wpaSecUrl, sizeof(wpaSecUrl), "%s", "https://wpa-sec.stanev.org");
    }
    markDirty(D_UPLOAD);
}

const char* getUploadWifiSsid() { return uploadWifiSsid; }
void setUploadWifiSsid(const char* ssid) {
    if (ssid) {
        strncpy(uploadWifiSsid, ssid, sizeof(uploadWifiSsid) - 1);
        uploadWifiSsid[sizeof(uploadWifiSsid) - 1] = '\0';
    } else {
        uploadWifiSsid[0] = '\0';
    }
    markDirty(D_UPLOAD);
}
bool hasUploadWifi() { return uploadWifiSsid[0] != '\0'; }

const char* getUploadWifiPass() { return uploadWifiPass; }
void setUploadWifiPass(const char* pass) {
    if (pass) {
        strncpy(uploadWifiPass, pass, sizeof(uploadWifiPass) - 1);
        uploadWifiPass[sizeof(uploadWifiPass) - 1] = '\0';
    } else {
        uploadWifiPass[0] = '\0';
    }
    markDirty(D_UPLOAD);
}

void clearWpaSecCredentials() {
    wpaSecKey[0] = '\0';
    uploadWifiSsid[0] = '\0';
    uploadWifiPass[0] = '\0';
    // Keep URL as default
    snprintf(wpaSecUrl, sizeof(wpaSecUrl), "%s", "https://wpa-sec.stanev.org");
    markDirty(D_UPLOAD);
}

// ==[ WIGLE UPLOAD ]==
const char* getWigleUsername() { return wigleUsername; }
void setWigleUsername(const char* name) {
    if (name) {
        strncpy(wigleUsername, name, sizeof(wigleUsername) - 1);
        wigleUsername[sizeof(wigleUsername) - 1] = '\0';
    } else {
        wigleUsername[0] = '\0';
    }
    markDirty(D_UPLOAD);
}
const char* getWigleToken() { return wigleToken; }
void setWigleToken(const char* token) {
    if (token) {
        strncpy(wigleToken, token, sizeof(wigleToken) - 1);
        wigleToken[sizeof(wigleToken) - 1] = '\0';
    } else {
        wigleToken[0] = '\0';
    }
    markDirty(D_UPLOAD);
}
bool hasWigleCredentials() { return wigleUsername[0] != '\0' && wigleToken[0] != '\0'; }

// ==[ LOOT PIN ]==
bool hasLootPin() { return lootPin[0] != '\0'; }
void getLootPin(char* out) { memcpy(out, lootPin, 5); }
void setLootPin(const char* pin) {
    if (pin) {
        strncpy(lootPin, pin, 4);
        lootPin[4] = '\0';
    }
    markDirty(D_UPLOAD);
}
void clearLootPin() {
    lootPin[0] = '\0';
    markDirty(D_UPLOAD);
}

// ==[ NOWFLOCK ]== cached getters keep the frame loop away from NVS. Setters
// share Config's five-second debounce and shutdown flush.
bool getNowFlockEnabled() { return nowFlockEnabled; }
void setNowFlockEnabled(bool enabled) {
    nowFlockEnabled = enabled;
    markDirty(D_NOWFLOCK);
}

uint32_t getNowFlockGroupKey() { return nowFlockGroupKey; }
void setNowFlockGroupKey(uint32_t key) {
    nowFlockGroupKey = key;
    markDirty(D_NOWFLOCK);
}
void clearNowFlockGroupKey() {
    nowFlockGroupKey = DEFAULT_NOWFLOCK_GROUP_KEY;
    markDirty(D_NOWFLOCK);
}

uint8_t getNowFlockReportIntervalS() { return nowFlockReportS; }
void setNowFlockReportIntervalS(uint8_t seconds) {
    nowFlockReportS = constrain(seconds, (uint8_t)2, (uint8_t)60);
    markDirty(D_NOWFLOCK);
}

bool getNowFlockPigbrother() { return nowFlockPigbrother; }
void setNowFlockPigbrother(bool enabled) {
    nowFlockPigbrother = enabled;
    markDirty(D_NOWFLOCK);
}

uint8_t getNowFlockExportProfile() { return nowFlockExportProfile; }
void setNowFlockExportProfile(uint8_t profile) {
    nowFlockExportProfile = profile > 2 ? 0 : profile;
    markDirty(D_NOWFLOCK);
}

bool getNowFlockBleHeartbeat() { return nowFlockBleHeartbeat; }
void setNowFlockBleHeartbeat(bool enabled) {
    nowFlockBleHeartbeat = enabled;
    markDirty(D_NOWFLOCK);
}

} // namespace Config
