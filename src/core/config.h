/**
 * Config - NVS-based configuration
 *
 * ==[ KNOB VAULT ]== no SD on Hamlet; all knobs live in NVS.
 */

#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>

// Forward-declared rather than included: config.h reaches nearly every
// translation unit in the project and has no business dragging the radio layer
// along with it. A scoped enum with a fixed underlying type is complete enough
// from this alone to appear in a signature.
namespace MeshUartPolicy { enum class Codec : uint8_t; }

namespace Config {
    // init NVS and load config
    void init();
    
    // save config to NVS
    void save();
    
    // deferred save tick (call from main loop)
    void update();
    
    // settings
    uint8_t getBrightness();      // 10-100% 
    void setBrightness(uint8_t val);
    
    uint8_t getDimLevel();        // 0-50% (0=off when dimmed)
    void setDimLevel(uint8_t val);
    
    uint16_t getDimTimeout();     // 0-300 seconds (0=never dim)
    void setDimTimeout(uint16_t val);

    bool getDisplayRotate180();   // rotate display 180 degrees
    void setDisplayRotate180(bool val);

    bool getLedAmbient();          // M5GO Bottom2 ambient LED glow (GPIO25, 10x SK6812)
    void setLedAmbient(bool val);
    uint8_t getLedColor();         // 0=AUTO(screen), 1=THEME(follow hue), 2-13=fixed hue preset
    void setLedColor(uint8_t val);
    uint8_t getLedBrightness();    // LED brightness 1-10 (independent of screen)
    void setLedBrightness(uint8_t val);
    
    bool getSoundEnabled();
    void setSoundEnabled(bool val);
    
    uint8_t getSfxVolume();       // SFX volume (0-10, default 2)
    void setSfxVolume(uint8_t val);
    uint8_t getMusicVolume();     // Music volume (0-10, default 5)
    void setMusicVolume(uint8_t val);
    // CoreS3 SE only: enable live bath sound reaction. Core2 always reports off.
    bool getBathMicEnabled();
    void setBathMicEnabled(bool val);

    bool getHapticEnabled();         // haptic motor on/off (default: true)
    void setHapticEnabled(bool val);
    uint8_t getHapticIntensity();    // haptic intensity 0-10 (default: 7)
    void setHapticIntensity(uint8_t val);
    
    int8_t getProbeThreshold();   // RSSI threshold for probing (-50 to -80)
    void setProbeThreshold(int8_t val);
    
    bool getAutoProbe();          // Auto-probe eligible networks
    void setAutoProbe(bool val);
    
    uint16_t getThemeHue();       // HSV hue 0-359
    void setThemeHue(uint16_t val);
    uint8_t getThemeStyle();     // 0=DARK, 1=INVERTED, 2=RETRO, 3=MONO, 4=N0STR0M0, 5=THE OG
    void setThemeStyle(uint8_t val);

    uint8_t getAccentMode();     // 0-5: light hue family preset
    void setAccentMode(uint8_t val);
    uint8_t getLightIntensity(); // 0-3: emissive boost / non-emissive suppress
    void setLightIntensity(uint8_t val);

    bool getRoomParallaxEnabled();  // IMU-driven depth motion in Pancetta's rooms
    void setRoomParallaxEnabled(bool val);

    const char* getDefaultHamletName(); // built-in 4-char FLOCK handle
    const char* getHamletName(); // 4-char FLOCK handle
    void setHamletName(const char* name);

    // ==[ PIG LOOK ]== root tune-pig cosmetic. append future hats here.
    enum PigHeadStyle : uint8_t {
        PIG_HEAD_THEME = 0,
        PIG_HEAD_ROSE,
        PIG_HEAD_ICE,
        PIG_HEAD_GOLD,
        PIG_HEAD_FEDORA,
        PIG_HEAD_HYPE,
        PIG_HEAD_STYLE_COUNT
    };
    PigHeadStyle getPigHeadStyle();
    void setPigHeadStyle(PigHeadStyle style);
    
    // ==[ ADAPTIVE HUNT MODE ]==
    bool getAdaptiveHunt();       // enable motion-aware hunting
    void setAdaptiveHunt(bool val);
    
    bool getDeauthEnabled();      // enable MUDBALL deauth attacks
    void setDeauthEnabled(bool val);
    
    bool getDeauthAggressive();   // PIG ANGRY - aggressive deauth mode
    void setDeauthAggressive(bool val);
    
    bool getExcludePMF();         // skip networks with PMF (can't deauth)
    void setExcludePMF(bool val);
    
    bool getExcludeWPA3();        // skip WPA3 networks (SAE immune)
    void setExcludeWPA3(bool val);

    bool getSAEAttackEnabled();   // SAE reject/downgrade assist for WPA3 transition targets
    void setSAEAttackEnabled(bool val);

    bool getEAPOLInjectionEnabled();  // PMF bypass: EAPOL-Start/Logoff injection (data frames)
    void setEAPOLInjectionEnabled(bool val);

    bool getCSAEnabled();             // CSA channel herding: spoofed beacon with Channel Switch Announcement
    void setCSAEnabled(bool val);

    bool getAuthFloodEnabled();       // Auth flood: random-MAC auth frames to exhaust AP STA table
    void setAuthFloodEnabled(bool val);
    
    uint8_t getStationaryHopDelay();   // hop delay when stationary (ms)
    void setStationaryHopDelay(uint8_t val);
    
    uint8_t getWalkingHopDelay();      // hop delay when walking (ms)
    void setWalkingHopDelay(uint8_t val);

    // ==[ MOTION DETECTION ]==
    uint8_t getStationaryTimeout();      // seconds before STATIONARY (5-30)
    void setStationaryTimeout(uint8_t val);

    uint8_t getWalkingSteps();           // steps required for WALKING (1-10)
    void setWalkingSteps(uint8_t val);

    bool getAutoHuntEnabled();           // auto-start hunt when walking
    void setAutoHuntEnabled(bool val);

    // ==[ POWER ]==
    bool getShakeWake();                 // shake wakes dimmed screen
    void setShakeWake(bool val);
    
    bool getAlertWake();                 // deauth alert wakes screen
    void setAlertWake(bool val);
    
    bool getParanoiaEnabled();           // global deauth alert (interrupts any mode)
    void setParanoiaEnabled(bool val);
    
    bool getTiltNavigationEnabled();      // tilt navigation enabled (left/right/up tilt gestures)
    void setTiltNavigationEnabled(bool val);

    bool getSpectrumTiltEnabled();        // spectrum dial mode auto-activation (upright = tilt channel select)
    void setSpectrumTiltEnabled(bool val);

    bool getBatteryAdaptation();          // auto-adjust power based on battery level
    void setBatteryAdaptation(bool val);

    bool getPwrFps60();                   // force 60fps when USB-C connected
    void setPwrFps60(bool val);

    // ==[ CATCH WINDOW ]==
    uint8_t getCatchWindowCamp();        // catch window in CAMP mode (4-15s)
    void setCatchWindowCamp(uint8_t val);

    uint8_t getCatchWindowPatrol();      // catch window in PATROL mode (2-10s)
    void setCatchWindowPatrol(uint8_t val);

    // ==[ NETWORK ACTIVITY TIMEOUTS ]==
    uint8_t getActiveTimeoutCamp();      // activity timeout in CAMP (10-60s)
    void setActiveTimeoutCamp(uint8_t val);

    uint8_t getActiveTimeoutPatrol();    // activity timeout in PATROL (5-30s)
    void setActiveTimeoutPatrol(uint8_t val);

    // ==[ TRACKER SETTINGS ]==
    enum RssiSmooth : uint8_t {
        RSSI_SMOOTH_FAST = 0,  // 50/50 mix, 200ms response
        RSSI_SMOOTH_MED  = 1,  // 67/33 mix, 300ms response (default)
        RSSI_SMOOTH_SLOW = 2   // 80/20 mix, 500ms response
    };
    
    RssiSmooth getRssiSmooth();          // RSSI smoothing aggression
    void setRssiSmooth(RssiSmooth val);
    
    bool getGhostMarkerEnabled();        // show ghost at last known position
    void setGhostMarkerEnabled(bool val);

    // Statistics (persisted)
    uint32_t getTotalPMKIDs();
    void incrementTotalPMKIDs();
    
    uint32_t getTotalHandshakes();
    void incrementTotalHandshakes();
    
    uint32_t getTotalSteps();
    void addSteps(uint32_t steps);
    
    uint32_t getTotalDistance();  // meters
    
    // ==[ D-UCB CHANNEL REWARDS ]== persisted
    uint16_t getChannelRewards(uint8_t channel);   // reward count for channel 1-13
    void setChannelRewards(uint8_t channel, uint16_t rewards);  // set reward count
    void saveChannelRewards();  // persist to NVS
    void loadChannelRewards();  // load from NVS
    void clearChannelRewards(); // wipe learning data - start fresh
    
    // ==[ UI STATE ]== persisted
    uint32_t getLoreOpenCount();       // next 0ct0 case-file fragment
    void setLoreOpenCount(uint32_t val);
    uint32_t getCatMemoryMask();       // observed Pig habits, append-only bits
    void setCatMemoryMask(uint32_t val);
    uint32_t getCatLoreSeenMask();     // observed habits already read in ABOUT
    void setCatLoreSeenMask(uint32_t val);

    uint32_t getHintSeen();              // touch hint seen bitmask (1 bit per mode)
    void setHintSeen(uint32_t val);

    uint32_t getHelpWikiSeen();          // help wiki seen bitmask (1 bit per mode)
    void setHelpWikiSeen(uint32_t val);

    // ==[ NPC CASE THREADS ]== persisted
    uint32_t getNpcChoiceLedger();       // two-bit last choice per character
    void setNpcChoiceLedger(uint32_t val);
    uint32_t getNpcClosedCastMask();     // characters ever resolved
    void setNpcClosedCastMask(uint32_t val);
    bool getNpcCodaSeen();                // whole-cast consequence delivered
    void setNpcCodaSeen(bool seen);

    // ==[ XP PROGRESSION ]==
    uint32_t getXP();                 // cumulative XP, never decreases
    enum class RewardSource : uint8_t {
        UNKNOWN = 0,
        STREAK,
        GOAL,
        WALK_MILESTONE,
        RECON,
        STATION,
        TRACKER_SURVIVAL,
        CHALLENGE,
        CHALLENGE_SWEEP,
        ROOM_CIRCUIT,
        DEBRIEF,
        XP_EVENT,
        CAT_MEMORY,
        COUNT
    };
    void addXP(uint32_t amount);      // add XP with soft cap, level-up detection
    void addXP(uint32_t amount, RewardSource source); // reward-aware addXP path for anti-farm tracking
    uint8_t getLevel();               // derived from XP via curve (1-42)
    bool isElder();                   // level 42 OR has ever prestiged — permanent hype
    uint8_t getPrestigeCount();       // 0 = never prestiged, 1+ = reincarnated
    void doPrestige();                // reset XP to L1, increment prestige, unlock next cosmetic
    uint8_t getMaxUnlockedPigStyle(); // max PigHeadStyle index available at current prestige
    uint8_t getXPProgress();          // 0-100 percentage toward next level
    uint32_t getXPForNextLevel();     // XP needed for next level
    uint32_t getSessionXPGained();    // XP gained this session
    const char* getRankName();        // rank tier name (RUNT, SH0AT, etc.)
    const char* getRankName(uint8_t level);  // rank for specific level

    // level-up event flag (checked in main loop)
    bool hasLevelUp();                // true if level crossed boundary since last check
    uint8_t consumeLevelUp();         // returns new level and clears flag
    uint32_t consumePendingXPDisplay(); // drain accumulated XP for the notification strip (called by Display each frame)

    uint16_t getStreak();             // current consecutive active sessions
    void setStreak(uint16_t val);

    uint16_t getBestStreak();         // lifetime record
    void setBestStreak(uint16_t val);

    // boot-time session streak update (call once after init)
    void updateSessionStreak();       // calculates streak from session activity, awards XP

    // ==[ SESSION GOALS ]==
    void evaluateSessionGoal();       // evaluate last session's goal + reset (call after updateSessionStreak)

    uint32_t getSessionSteps();       // steps this session
    void incrementSessionSteps();     // RAM update (every step)
    void persistSessionSteps();       // NVS save (batched)

    uint16_t getGoalTarget();         // adaptive goal (500-6000)
    uint8_t getGoalProgress();        // 0-100 percentage
    // session engagement tracking
    void markSessionActive();         // call when engagement threshold met (50 steps, 1 capture, 2min hunt)
    bool isSessionActive();           // query session active state
    bool isStreakAtRisk();            // true if 1+ idle boots with active streak

    bool wasGoalMetLastSession();     // for boot phrase
    void clearGoalMetFlag();          // after showing phrase

    // goal progress triggers
    bool wasGoalCloseTriggered();     // 80% shown this session
    void setGoalCloseTriggered();
    bool wasGoalCompleteTriggered();  // 100% shown this session
    void setGoalCompleteTriggered();

    // RTC helpers
    uint32_t getRtcDate();            // get today as YYYYMMDD
    bool isRtcValid();                // trusted RTC/system time available
    bool hasTrustedClock();           // trusted time source is available
    uint32_t getTrustedEpoch();       // trusted Unix epoch, 0 if unavailable
    void markClockSynced();           // accept current RTC as trusted + sync system clock
    void adoptSyncEpochMin(uint32_t epochMin, uint32_t masterUptimeMs, uint32_t localNowMs);

    // === SESSION CAPTURE TRACKING ===
    uint8_t getSessionPMKIDCount();   // PMKIDs caught this session
    uint8_t getSessionHSCount();      // Handshakes caught this session
    void incrementSessionPMKIDCount();
    void incrementSessionHSCount();

    // ==[ COORDINATION SETTINGS ]==
    uint8_t getCoordinationRole();    // 0=master, 1=slave, 2=standalone
    void setCoordinationRole(uint8_t role);

    // ==[ RECONNAISSANCE SETTINGS ]==
    bool getReconEnabled();            // Whether reconnaissance mode is enabled
    void setReconEnabled(bool enabled);

    // ==[ WPA-SEC CLOUD UPLOAD ]==
    const char* getWpaSecKey();           // WPA-SEC API key (32 chars max)
    void setWpaSecKey(const char* key);
    bool hasWpaSecKey();                  // Check if key is configured

    const char* getWpaSecUrl();           // API URL (default: https://wpa-sec.stanev.org)
    void setWpaSecUrl(const char* url);

    const char* getUploadWifiSsid();      // WiFi SSID for upload connection
    void setUploadWifiSsid(const char* ssid);
    bool hasUploadWifi();                 // Check if WiFi is configured

    const char* getUploadWifiPass();      // WiFi password
    void setUploadWifiPass(const char* pass);

    void clearWpaSecCredentials();        // Clear all WPA-SEC related credentials

    // ==[ WIGLE UPLOAD ]==
    const char* getWigleUsername();       // WiGLE API name (24 chars max)
    void setWigleUsername(const char* name);
    const char* getWigleToken();          // WiGLE API token (64 chars max)
    void setWigleToken(const char* token);
    bool hasWigleCredentials();           // true if both username+token set

    // ==[ LOOT PIN ]==
    bool hasLootPin();                    // non-empty PIN stored
    void getLootPin(char* out);           // copy 4-char PIN + null terminator (out must be >=5 bytes)
    void setLootPin(const char* pin);     // save 4-char string to NVS
    void clearLootPin();                  // remove PIN from NVS

    // ==[ IPP DEFENSE SETTINGS ]==
    bool getIppEnabled();              // Interdimensional Pig Pen master switch
    void setIppEnabled(bool enabled);
    bool getIppBLEScan();              // BLE passive scan for trackers
    void setIppBLEScan(bool enabled);
    bool getIppWifiScan();             // WiFi environment scan
    void setIppWifiScan(bool enabled);
    bool getWifiChaffEnabled();        // HOGWASH: fake handshake injection on deauth
    void setWifiChaffEnabled(bool val);

    // ==[ BLE WATCHLIST ]== up to 6 named BLE device presence alerts
    static constexpr int MAX_WATCHLIST = 6;
    static constexpr int WATCHLIST_LABEL_LEN = 16;
    struct WatchlistSlot {
        uint8_t payloadHash[4];
        char label[WATCHLIST_LABEL_LEN];
        bool occupied;
    };
    uint8_t getWatchlistCount();                                    // number of occupied slots
    bool getWatchlistSlot(uint8_t idx, WatchlistSlot& out);         // 0..MAX_WATCHLIST-1
    bool setWatchlistSlot(uint8_t idx, const uint8_t* hash, const char* label);
    void clearWatchlistSlot(uint8_t idx);

    // ==[ CAPTURE TYPE COLLECTION ("CASE FILES") ]==
    // bitmask of wifi_auth_mode_t values ever captured. first-discovery = special event.
    uint16_t getSeenAuthTypes();              // bitfield of seen auth types
    bool markAuthTypeSeen(uint8_t authType);  // returns true if NEW (first discovery)

    // ==[ WARDRIVE STATS ]==
    uint32_t getWDTotal();                // lifetime unique networks mapped
    void addWDTotal(uint32_t count);      // add to lifetime total
    uint16_t getWDSessions();             // total wardrive sessions
    void incrementWDSessions();           // +1 session counter

    // ==[ WARDRIVE BLE ]==
    bool getWardriveBleScan();         // BLE interleave during wardrive (default true)
    void setWardriveBleScan(bool val);

    // ==[ GPS SETTINGS ]==
    bool getGPSEnabled();              // GPS module connected (default false)
    void setGPSEnabled(bool val);
    // Pins are validated against the per-target M-BUS DIP routes in
    // gps_policy.h; the setters reject anything else and init() repairs a
    // stored value that is not a route for this board.
    uint8_t getGPSRxPin();             // ESP32 RX <- GPS TX (GPSPolicy::DEFAULT_RX_PIN)
    void setGPSRxPin(uint8_t pin);
    uint8_t getGPSTxPin();             // ESP32 TX -> GPS RX (GPSPolicy::DEFAULT_TX_PIN)
    void setGPSTxPin(uint8_t pin);
    uint8_t getGPSBaudIndex();         // index into baud table (default 3 = 115200)
    void setGPSBaudIndex(uint8_t idx);
    uint32_t getGPSBaud();             // resolved baud rate from index
    bool getGPSAlwaysOn();             // GPS stays hot across all modes (default false)
    void setGPSAlwaysOn(bool val);

    // ==[ C5MONSTER SETTINGS ]==
    bool getC5Enabled();                // C5 bridge enabled (CoreS3SE on, Core2 off by default)
    void setC5Enabled(bool val);
    uint8_t getC5RxPin();              // ESP32 RX <- C5Monster TX (Core2 default 33)
    void setC5RxPin(uint8_t pin);
    uint8_t getC5TxPin();              // ESP32 TX -> C5Monster RX (Core2 default 32)
    void setC5TxPin(uint8_t pin);
    uint32_t getC5Baud();              // baud rate (default 115200)
    void setC5Baud(uint32_t baud);

    // ==[ MESH SETTINGS ]== Unit C6L running Meshtastic on a Grove port.
    // Pins are validated against the per-target Grove routes in
    // mesh_uart_policy.h. Off by default: the default route (Port C) is the
    // same electrical net as the GPS module, so this cannot be opt-out.
    bool getMeshEnabled();
    void setMeshEnabled(bool val);
    // The pins move as a pair — a Grove port is one connector — so there is one
    // setter for both legs and it takes the RX one.
    uint8_t getMeshRxPin();            // ESP32 RX <- C6L TX (CoreS3SE default 18)
    void setMeshRxPin(uint8_t pin);
    uint8_t getMeshTxPin();            // ESP32 TX -> C6L RX (CoreS3SE default 17)
    // Who else is holding those pins — "GPS", "C5", or nullptr when free. Ask
    // before opening the port: two owners on one UART reads as a dead radio
    // rather than as an error. Independent of getMeshEnabled().
    const char* meshPinOwner();
    uint32_t getMeshBaud();            // baud rate (default 38400, Meshtastic's)
    void setMeshBaud(uint32_t baud);
    // TEXTMSG (the SerialModule default) or PROTO. PROTO is the only one that
    // carries node names, signal quality, delivery acks or direct messages —
    // and it requires serial.mode=PROTO on the C6L, so it cannot be the
    // default without breaking every radio nobody has reconfigured.
    MeshUartPolicy::Codec getMeshCodec();
    void setMeshCodec(MeshUartPolicy::Codec codec);

    // ==[ NOWFLOCK GROUP KEY ]==
    bool getNowFlockEnabled();            // master ESP-NOW coordination gate (fresh install off)
    void setNowFlockEnabled(bool enabled);
    uint32_t getNowFlockGroupKey();       // 32-bit FNOW/3 group tag key, 0 disables tag check
    void setNowFlockGroupKey(uint32_t key);
    void clearNowFlockGroupKey();         // restore compiled lab default
    uint8_t getNowFlockReportIntervalS(); // SIGHTING interval (2-60s, default 10)
    void setNowFlockReportIntervalS(uint8_t seconds);
    bool getNowFlockPigbrother();         // EXPORT_SNAPSHOT role (default off)
    void setNowFlockPigbrother(bool enabled);
    uint8_t getNowFlockExportProfile();   // 0=off 1=wigle-v1 2=fmh-v1
    void setNowFlockExportProfile(uint8_t profile);
    bool getNowFlockBleHeartbeat();       // optional BLE presence beacon
    void setNowFlockBleHeartbeat(bool enabled);
}

#endif // CONFIG_H
