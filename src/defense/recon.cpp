/**
 * Recon — Interdimensional Pig Pen (IPP) Defensive Awareness Engine
 *
 * ==[ THE WIRE ]== background intel during IDLE/MENU.
 * WiFi scan: async scanNetworks, evil twin / KARMA / potfile checks.
 * BLE scan: passive tracker detection (AirTag, SmartTag, Tile, FastPair, spam).
 * parasitic mode: feed from Hunt's network table (zero radio cost).
 *
 * state machine: SLEEPING → BLE_SCANNING → SLEEPING → WIFI_SCANNING → WIFI_PROCESSING → SLEEPING
 * BLE stays initialized (warm) — only scan start/stop, no deinit/reinit cycling
 *                PARASITIC: periodic analysis of Hunt's beacon table
 *
 * ==[ FIX LOG ]==
 * - IPP master switch now gates all scanning (was decorative)
 * - WiFi radio powered off after scan (was left in STA mode burning 70mA)
 * - event dedup with cooldowns (was spamming same alerts every 120s)
 * - evil twin: skip common SSIDs, require proximity, skip mesh (>3 APs same SSID)
 * - KARMA: require 3+ SSIDs from same BSSID (2 is normal dual-band AP)
 * - BLE tracker detection: AirTag, SmartTag, Tile, FastPair, Flipper spam
 */

#include "recon_internal.h"
#include <atomic>
#include "defense_pipeline.h"
#include "wifi_observation_producer.h"
#include "xband.h"
#include "potfile.h"
#include "../core/config.h"
#include "../core/gps.h"
#include "../hal/sd_storage.h"
#include "../modes/hunt.h"
#include "../activity/pedometer.h"
#include "../util/debug_log.h"
#include "../radio/c5monster_uart.h"
#include <WiFi.h>
#ifndef SIMULATOR
#endif
#include <esp_wifi.h>
#include <esp_heap_caps.h>
#include <string.h>
#include <math.h>

#if RECON_BLE_ENABLED
#include <NimBLEDevice.h>
#endif

// ==[ BLE CHAFF FREE FUNCTIONS ]== declared outside namespace to avoid mangling
extern void bleChaff_toggle();
extern bool bleChaff_isActive();
extern void bleChaff_update(uint32_t now);
extern void bleChaff_stop();

#include "wifi_chaff.h"

namespace Recon {

// ==[ SCAN INTERVALS ]==
static constexpr uint32_t WIFI_SCAN_INTERVAL_MS            = 120000;  // quiet baseline
static constexpr uint32_t WIFI_SCAN_INTERVAL_ELEVATED_MS   = 60000;   // YELLOW-ish cadence
static constexpr uint32_t WIFI_SCAN_INTERVAL_AGGRESSIVE_MS = 30000;   // ORANGE/RED cadence
static constexpr uint32_t WIFI_SCAN_TIMEOUT_MS             = 10000;   // bail after 10s if async scan hangs
static constexpr uint32_t BLE_SCAN_DURATION_MS             = 8000;    // 8s BLE scan window
static constexpr uint32_t BLE_SCAN_INTERVAL_MS             = 60000;   // 60s between BLE scans
static constexpr uint32_t DEAUTH_SNIFF_MS                  = 3000;    // baseline post-scan sniff
static constexpr uint32_t DEAUTH_SNIFF_ELEVATED_MS         = 4000;
static constexpr uint32_t DEAUTH_SNIFF_AGGRESSIVE_MS       = 6000;
static constexpr uint8_t  DEAUTH_SNIFF_CHANNELS            = 13;      // hop 1-13
static constexpr uint32_t DEAUTH_SENTINEL_INTERVAL_ELEVATED_MS = 10000;
static constexpr uint32_t DEAUTH_SENTINEL_INTERVAL_AGGRESSIVE_MS = 5000;
static constexpr uint32_t DEAUTH_SENTINEL_SNIFF_ELEVATED_MS = 400;
static constexpr uint32_t DEAUTH_SENTINEL_SNIFF_AGGRESSIVE_MS = 700;
static constexpr uint32_t PARASITIC_INTERVAL_MS            = 30000;
static constexpr uint32_t PARASITIC_INTERVAL_ELEVATED_MS   = 15000;
static constexpr uint32_t PARASITIC_INTERVAL_AGGRESSIVE_MS = 10000;
static constexpr uint32_t DEAUTH_ACTIVE_WINDOW_MS          = 30000;   // last confirmed burst is still "active"
static constexpr uint8_t  WD_SWEEPS_PER_BLE                = 3;       // wardrive: BLE window every N WiFi sweeps
static constexpr uint32_t WD_BLE_SCAN_DURATION_MS           = 4000;   // wardrive: 4s BLE scan window
static constexpr uint8_t  SENTINEL_CHANNELS[]              = {1, 6, 11};

// ==[ EVENT DEDUP COOLDOWNS ]==
static constexpr uint32_t EVIL_TWIN_COOLDOWN_MS   = 600000;  // 10min per SSID
static constexpr uint32_t KARMA_COOLDOWN_MS       = 600000;  // 10min per BSSID
static constexpr uint32_t OPEN_AP_COOLDOWN_MS     = 600000;  // 10min
static constexpr uint32_t TRACKER_ALERT_COOLDOWN  = 300000;  // 5min per tracker (cooldown applied in hamlet.cpp event consumer)
static constexpr uint32_t EVIL_TWIN_ACTIVE_MS     = 180000;  // 3min "hot" window
static constexpr uint32_t KARMA_ACTIVE_MS         = 180000;  // 3min "hot" window
static constexpr int EVENT_CRITICAL_RESERVE        = 4;       // keep room for high-priority events
static constexpr uint32_t PROBE_VULN_COOLDOWN_MS  = 180000;  // 3min probe-intel event cooldown
static constexpr uint32_t PROBE_INTEL_MAX_AGE_MS  = 300000;  // only keep probe intel from last 5min
static constexpr uint32_t FINGERPRINT_EVENT_COOLDOWN_MS = 60000;
static constexpr uint32_t SEQ_EVENT_COOLDOWN_MS         = 30000;
static constexpr uint32_t RSSI_EVENT_COOLDOWN_MS        = 30000;
static constexpr uint32_t KNOWN_AP_COOLDOWN_MS          = 300000;  // 5min (was infinite — caused slot exhaustion)
static constexpr uint32_t FINGERPRINT_ACTIVE_MS         = 60000;
static constexpr uint32_t SEQ_ACTIVE_MS                 = 30000;
static constexpr uint32_t RSSI_ACTIVE_MS                = 30000;
static constexpr uint32_t HUNT_DEAUTH_WINDOW_MS         = 1500;

// ==[ COMMON SSID DENYLIST ]== evil twin false positive suppression
// prefix match: "DIRECT-" matches "DIRECT-xx-HP", etc.
static const char* COMMON_SSID_PREFIXES[] = {
    "xfinitywifi", "XFINITY", "attwifi", "ATT-WIFI", "CableWiFi",
    "DIRECT-", "HP-Print-", "NETGEAR", "linksys", "default",
    "FreeWiFi", "FREE_Wi-Fi", "Fon_", "eduroam", "AndroidAP",
    "iPhone", "Galaxy", "HUAWEI",
};
static constexpr int COMMON_PREFIX_COUNT = sizeof(COMMON_SSID_PREFIXES) / sizeof(COMMON_SSID_PREFIXES[0]);

// ScanState enum defined in recon_internal.h

// ==[ PSRAM-BACKED ARRAYS ]==
static WifiAP* wifiSnapshot = nullptr;
static Defense::WifiObservation* wifiObservationBuffer = nullptr;
static int wifiAPCount = 0;
static int openAPCount = 0;
static int knownAPCount = 0;
static uint16_t knownProbeReqCount = 0;
static uint16_t knownProbeClientCount = 0;
static uint32_t lastProbeVulnAlert = 0;
static uint16_t lastProbeVulnAlertCount = 0;

// ==[ BLE TRACKER TABLE ]== PSRAM-backed, MAX_TRACKERS entries
// extern-declared in recon_internal.h for recon_ble.cpp access
TrackerEntry* trackerTable = nullptr;
int trackerCount = 0;
TrackerEntry* bleDeviceTable = nullptr;
std::atomic<int> bleDeviceCount{0};
int followingCount = 0;
int spamCount = 0;
volatile uint16_t totalBLEDevices = 0;
volatile uint16_t appleContinuityCount = 0;  // per-scan Apple continuity counter

#if RECON_BLE_ENABLED
// ==[ BLE ENGINE STATE ]== types/constants in recon_internal.h, storage here
bool bleInitialized = false;
std::atomic<bool> bleScanActive{false};
std::atomic<bool> processingBLE{false};  // guard: main loop processing catalog
bool bleActiveScanMode = false;  // user-toggled: passive (stealth) vs active (SCAN_REQ)
uint32_t bleScanStart = 0;
uint32_t lastBLEScan = 0;

// ==[ IDLE PULL ]== Zeigarnik open loops: count BLE scan cycles while IDLE (sim: #6 sensitivity)
uint8_t offlineScanCount = 0;

// temp buffer for current BLE scan cycle (built in callback, processed on complete)
BLERawHit* bleScanBuf = nullptr;
std::atomic<int> bleScanBufCount{0};
uint16_t* bleLiveFusedCounts = nullptr;
uint8_t (*bleSeenMacs)[6] = nullptr;
uint8_t bleSeenMacCount = 0;
uint8_t (*bleContinuityMacs)[6] = nullptr;
uint8_t bleContinuityMacCount = 0;
#else
bool bleInitialized = false;
uint32_t lastBLEScan = 0;
uint8_t offlineScanCount = 0;
#endif

// ==[ EVENT DEDUP STATE ]== PSRAM-backed to save DRAM
struct DedupEntry {
    uint32_t hash;       // FNV-1a of identifier (SSID for twin, BSSID for KARMA, etc.)
    uint32_t timestamp;  // last event time
};
static constexpr int MAX_DEDUP = 8;
static DedupEntry* dedupEvilTwin = nullptr;   // allocated in init()
static DedupEntry* dedupKarma = nullptr;
static DedupEntry* dedupKnownAP = nullptr;
static DedupEntry* dedupFingerprint = nullptr;
static DedupEntry* dedupSeq = nullptr;
static DedupEntry* dedupRssi = nullptr;
static int dedupEvilTwinCount = 0;
static int dedupKarmaCount = 0;
static int dedupKnownAPCount = 0;
static int dedupFingerprintCount = 0;
static int dedupSeqCount = 0;
static int dedupRssiCount = 0;
static uint32_t lastOpenAPAlert = 0;

// ==[ PSRAM ALLOCATION HELPER ]==
void* psramAlloc(size_t size, const char* label) {
    void* p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (!p) {
        p = malloc(size);
        HAMLET_LOGF("[RECON] %s: DRAM fallback (%d bytes)\n", label, (int)size);
    } else {
        HAMLET_LOGF("[RECON] %s: PSRAM (%d bytes)\n", label, (int)size);
    }
    return p;
}

// ==[ ENGINE STATE ]==
ScanState state = ScanState::SLEEPING;
static bool initialized = false;
static uint32_t stateEnteredAt = 0;
static uint32_t lastWifiScan = 0;
static uint32_t lastWifiScanComplete = 0;
static uint32_t lastWifiScanAttempt = 0;
static bool wifiScanStarted = false;
static int wifiScanResultCount = 0;
static int16_t lastWifiScanResult = WIFI_SCAN_FAILED;
static uint16_t wifiScanConsecutiveFailures = 0;
static uint32_t lastSentinelSniff = 0;
static bool deauthSniffSentinel = false;
static uint32_t deauthSniffDurationMs = DEAUTH_SNIFF_MS;
static uint8_t deauthSniffFixedChannel = 0;
static uint8_t sentinelChannelIdx = 0;
static bool pendingBleObservations = false;
static bool pendingWifiObservations = false;
static bool pendingParasiticObservations = false;
static bool pendingWardriveObservations = false;
static int pendingWardriveObservationCount = 0;
bool blePriorityMode = false;   // BLE_SCANNER forced aggressive BLE cadence
bool bleWardriveMode = false;   // wardrive BLE cadence — sweep-paced, WiFi suppressed
uint8_t wardriveSweepCount = 0;
volatile bool wardriveBleReady = false;
bool blePinnedPayloadValid = false;
uint8_t blePinnedPayloadHash[4] = {0};

// ==[ BLE WATCHLIST ]== runtime presence tracking for named devices
static WatchlistEntry* watchlist = nullptr;
static constexpr uint32_t WATCHLIST_EXIT_TIMEOUT_MS = 60000;  // 60s absent = exit
static constexpr int8_t WATCHLIST_ENTER_RSSI = -85;           // EDGE threshold

// ==[ DEAUTH SNIFF ]== atomic for callback↔main-loop cross-core safety
static std::atomic<uint16_t> sniffHits{0};          // callback-safe frame counter (gate flag)
static std::atomic<int8_t>   sniffPeakRSSI{-127};    // strongest this cycle — atomic for cross-core safety
static std::atomic<uint8_t>  sniffPeakChan{0};       // channel of strongest — atomic for cross-core safety
static std::atomic<uint16_t> sniffSubtypeDeauth{0};  // subtype 0x0C — atomic for cross-core safety
static std::atomic<uint16_t> sniffSubtypeDisassoc{0}; // subtype 0x0A — atomic for cross-core safety
static uint16_t sniffChannelHits[14] = {0};         // per-channel — protected by portMUX
static constexpr uint8_t MAX_DEAUTH_SOURCES = 8;  // source-MAC diversity for attribution
struct DeauthSource {
    uint8_t mac[6];
    uint16_t hits;
};
static portMUX_TYPE sniffMux = portMUX_INITIALIZER_UNLOCKED;  // protects sniffSources[], sniffSourceCount, sniffChannelHits[], sniffTargetBssid, sniffReasonCode/Hits
static DeauthSource sniffSources[MAX_DEAUTH_SOURCES];
static uint8_t sniffSourceCount = 0;
static uint8_t sniffTargetBssid[6] = {};   // first target BSSID in sniff burst
static uint16_t sniffReasonCode = 0;      // dominant reason code in sniff burst
static uint16_t sniffReasonHits = 0;      // hits for dominant reason
// volatile: written from Hunt/Spectrum promiscuous callbacks via ingestDeauthObservation(),
// read from main loop via getters (DefhogTerminal, XBand, Display, isDeauthActive)
static std::atomic<uint32_t> deauthTotal{0};                 // session accumulator
static std::atomic<uint32_t> deauthLastTime{0};              // millis of last detection
static volatile uint16_t deauthLastBurstCount = 0;          // frames in last confirmed burst
static volatile uint8_t  deauthLastUniqueSources = 0;       // unique source MACs in last burst
static volatile int8_t   deauthLastPeakRSSI = -127;         // strongest RSSI from last burst
static volatile uint8_t  deauthLastPeakChan = 0;            // strongest channel from last burst
static volatile uint8_t  deauthLastDominantChan = 0;        // channel with most frames in burst
static volatile uint16_t deauthLastPPS = 0;                 // packets/sec estimate in sniff window
static volatile uint16_t deauthLastSubtypeDeauth = 0;       // subtype 0x0C in last burst
static volatile uint16_t deauthLastSubtypeDisassoc = 0;     // subtype 0x0A in last burst
static volatile DeauthSourceOrigin deauthLastOrigin = DeauthSourceOrigin::RECON_SNIFF;
static volatile uint32_t lastDeauthEventReported = 0;

// ==[ HOGWASH HOLD ]== camp on attacker channel after deauth burst
static constexpr uint32_t HOGWASH_HOLD_MAX_MS    = 15000;  // max hold duration
static constexpr uint32_t HOGWASH_HOLD_QUIET_MS  = 8000;   // shut down after no new deauths
static constexpr uint32_t HOGWASH_REINJECT_MS    = 3000;   // re-inject interval
static uint8_t  hogwashHoldChannel = 0;
static uint8_t  hogwashHoldBssid[6] = {};
static uint32_t hogwashLastInjectTime = 0;
static uint16_t hogwashHoldNewHits = 0;       // deauths seen during hold

// ==[ HUNT DEAUTH INGEST ]== current-channel burst state fed from Hunt callback
static volatile uint32_t huntDeauthWindowStart = 0;
static volatile uint32_t huntDeauthLastObs = 0;
static volatile uint16_t huntDeauthHits = 0;
static volatile int8_t   huntDeauthPeakRSSI = -127;
static volatile uint8_t  huntDeauthPeakChan = 0;
static volatile uint16_t huntDeauthSubtypeDeauth = 0;
static volatile uint16_t huntDeauthSubtypeDisassoc = 0;
static volatile DeauthSource huntDeauthSources[MAX_DEAUTH_SOURCES];
static volatile uint8_t  huntDeauthSourceCount = 0;
static std::atomic<bool> huntDeauthEventPending{false};
// target BSSID (addr1) + reason code forensics
static volatile uint8_t  huntDeauthTargetBssid[6] = {};   // most-seen target BSSID in burst
static volatile uint16_t huntDeauthReasonCode = 0; // last reason code seen
static volatile uint16_t huntDeauthReasonHits = 0; // hits for dominant reason

// ==[ WIFI THREAT RECENCY ]==
static uint32_t lastEvilTwinTime = 0;
static uint32_t lastKarmaTime = 0;
static uint32_t lastFingerprintTime = 0;
static uint32_t lastSeqTime = 0;
static uint32_t lastRssiTime = 0;
static uint8_t recentFingerprintMismatchCount = 0;
static uint8_t recentSeqAnomalyCount = 0;
static uint8_t recentRssiAnomalyCount = 0;

// ==[ ATTACK CORRELATION ]== multi-vector dedup timestamps
static uint32_t lastCorrelFlipDth = 0;   // FLIPPER + DEAUTH
static uint32_t lastCorrelSpamDth = 0;   // BLE_SPAM + DEAUTH
static uint32_t lastCorrelSpamTwn = 0;   // BLE_SPAM + EVIL_TWIN
static uint32_t lastCorrelStlkDth = 0;   // TRACKER_FOLLOWING + DEAUTH
static constexpr uint32_t CORREL_COOLDOWN_MS = 60000;  // 60s dedup per type
static constexpr uint32_t CORREL_WINDOW_MS   = 30000;  // events within 30s

// ==[ FORENSIC EVENT LOG ]== PSRAM-backed evidence chain history
static ForensicLogEntry* forensicLog = nullptr;
static uint8_t forensicLogHead = 0;
static uint8_t forensicLogCount = 0;

// ==[ DEAUTH BURST HISTORY ]== PSRAM-backed ring of recent bursts
static DeauthBurstRecord* deauthHistory = nullptr;
static uint8_t deauthHistoryHead = 0;
static uint8_t deauthHistoryCount = 0;

// ==[ PROBE-POTFILE MATCH CACHE ]== PSRAM-backed vuln client cache
static ProbeVulnMatch* probeVulnCache = nullptr;
static uint8_t probeVulnCount = 0;

// ==[ FORENSIC EXPORT ]== SD card evidence persistence
static bool forensicExportEnabled = false;
static bool forensicSDReady = false;

// ==[ PARASITIC MODE ]==
static uint32_t lastParasiticAnalysis = 0;
static uint32_t lastSnapshotHash = 0;

// ==[ FEATURE 7: GHOST NETWORK CANARY ]==
static char* canarySSID = nullptr;  // fixed 33-byte PSRAM buffer
static bool canaryTripped = false;

// ==[ FEATURE 1: PHANTOM PROBE KARMA CONFIRMATION ]==
static char* karmaBaitSSID = nullptr;  // fixed 33-byte PSRAM buffer
static uint8_t karmaBaitBSSID[6] = {};   // BSSID we're testing
static uint32_t karmaBaitTime = 0;       // millis of injection
static bool karmaBaitPending = false;    // waiting for confirmation scan
static bool karmaConfirmed = false;

// ==[ FEATURE 6: DEAUTH TOOL CLASSIFICATION ]==
static DeauthTool lastDeauthTool = DeauthTool::UNKNOWN;

// ==[ FEATURE 5: CLIENT FINGERPRINTING ]==
static ClientFingerprint* clientFingerprints = nullptr;
static uint8_t clientFingerprintCount = 0;
// sniff-side capture buffers
static ProbeCapture* probeCaptureBuf = nullptr;
static std::atomic<int> probeCaptureCount{0};

// ==[ FEATURE 3: BEACON ENTROPY SCORING ]==
static BeaconIECapture* beaconCaptureBuf = nullptr;
static std::atomic<int> beaconCaptureCount{0};

// ==[ FORWARD DECLARATIONS ]==
static void setState(ScanState s);
static bool startWifiScan();
static void processWifiResults();
static void fuseWardriveObservations();
static void fuseWifiObservations(const Defense::WifiObservation* observations, int count);
static void analyzeSnapshot();
static void populateFromHunt();
static uint32_t computeSnapshotHash();
static CadenceTier computeCadenceTier();
static uint32_t currentWifiScanIntervalMs();
static uint32_t currentDeauthSniffMs();
static uint32_t currentParasiticIntervalMs();
static uint32_t currentSentinelIntervalMs();
static uint32_t currentSentinelSniffMs();
// currentBLEScanIntervalMs/DurationMs: declared in recon_internal.h (shared with recon_ble.cpp)
static const char* proximityLabelInternal(int8_t rssi);
static float estimateDistanceInternal(int8_t rssi, int8_t txPower);
static const char* appearanceLabelInternal(uint16_t appearance);
static const char* manufacturerLabelInternal(uint16_t companyId);
static const char* classOfDeviceLabelInternal(uint32_t cod);
static bool shouldStartSentinelSniff(uint32_t now);
static uint8_t pickSentinelChannel();
// pushEvent: declared in recon_internal.h (non-static, shared with recon_ble.cpp)
static void appendForensicLog(const ReconEventData& ev);
static uint8_t buildIndicatorFlags();
static void checkEvilTwin();
static void checkKarmaHoneypot();
static void checkPotfileMatches();
static void checkOpenAPs();
static void updateKnownProbeIntel(bool emitEvent);
static void harvestHuntForensics();
static void pruneProbeVulnCache(uint32_t now);
static bool isCommonSSID(const char* ssid);
static int countSSIDOccurrences(const char* ssid);
static bool isDedupCooldown(DedupEntry* table, int& count, uint32_t hash, uint32_t cooldownMs);
static void updateTrackerFollowing(uint32_t now);
static bool startDeauthSniff(uint32_t durationMs = DEAUTH_SNIFF_MS,
                             bool sentinel = false,
                             uint8_t fixedChannel = 0);
static bool stopDeauthSniff();  // returns true if HOGWASH chaff was injected
static void emitPendingDeauthEvent();
static void checkAttackCorrelation(uint32_t now);
static void resetDeauthObservationState();
static void deauthSniffCB(void* buf, wifi_promiscuous_pkt_type_t type);
// advanced features
static void checkCanarySSID();
static void triggerPhantomProbe(const uint8_t* bssid);
static void checkPhantomProbeConfirmation();
static DeauthTool classifyDeauthTool(uint16_t frameCount, uint16_t pps,
                                      uint8_t uniqueSources, uint16_t deauthFrames,
                                      uint16_t disassocFrames, uint16_t reasonCode);
static void processProbeCaptures();
static void processBeaconCaptures();

// BLE stubs when NimBLE not compiled in — real implementations in recon_ble.cpp
#if !RECON_BLE_ENABLED
void initBLE() {}
void deinitBLE() {}
bool startBLEScan() { return false; }
void stopBLEScan() {}
void processBLEResults() {}
void fuseLiveBLEObservations(uint32_t) {}
void syncBLECatalogThreatState() {}
#endif

// ==[ INIT ]==
void init() {
    if (!watchlist) {
        watchlist = (WatchlistEntry*)psramAlloc(
            MAX_WATCHLIST * sizeof(WatchlistEntry), "watchlist");
    }
    if (!canarySSID) canarySSID = (char*)psramAlloc(33, "canarySSID");
    if (!karmaBaitSSID) karmaBaitSSID = (char*)psramAlloc(33, "karmaBait");
    if (!wifiSnapshot) {
        wifiSnapshot = (WifiAP*)psramAlloc(MAX_WIFI_SNAPSHOT * sizeof(WifiAP), "wifiSnap");
    }
    if (!wifiObservationBuffer) {
        wifiObservationBuffer = (Defense::WifiObservation*)psramAlloc(
            MAX_WIFI_SNAPSHOT * sizeof(Defense::WifiObservation), "wifiObs");
    }
    if (!trackerTable) {
        trackerTable = (TrackerEntry*)psramAlloc(MAX_TRACKERS * sizeof(TrackerEntry), "trackers");
    }
    if (!bleDeviceTable) {
        bleDeviceTable = (TrackerEntry*)psramAlloc(MAX_BLE_DEVICES * sizeof(TrackerEntry), "bleDevs");
    }
    // dedup tables: small but saves DRAM by going to PSRAM
    size_t dedupSize = MAX_DEDUP * sizeof(DedupEntry);
    if (!dedupEvilTwin) dedupEvilTwin = (DedupEntry*)psramAlloc(dedupSize, "dedupTwin");
    if (!dedupKarma) dedupKarma = (DedupEntry*)psramAlloc(dedupSize, "dedupKarma");
    if (!dedupKnownAP) dedupKnownAP = (DedupEntry*)psramAlloc(dedupSize, "dedupKnown");
    if (!dedupFingerprint) dedupFingerprint = (DedupEntry*)psramAlloc(dedupSize, "dedupFP");
    if (!dedupSeq) dedupSeq = (DedupEntry*)psramAlloc(dedupSize, "dedupSeq");
    if (!dedupRssi) dedupRssi = (DedupEntry*)psramAlloc(dedupSize, "dedupRssi");
#if RECON_BLE_ENABLED
    if (!bleScanBuf) {
        bleScanBuf = (BLERawHit*)psramAlloc(BLE_SCAN_BUF_SIZE * sizeof(BLERawHit), "bleBuf");
    }
    if (!bleLiveFusedCounts) {
        bleLiveFusedCounts = (uint16_t*)psramAlloc(
            BLE_SCAN_BUF_SIZE * sizeof(uint16_t), "bleFuseCursor");
    }
    if (!bleSeenMacs) {
        bleSeenMacs = (uint8_t (*)[6])psramAlloc(BLE_SCAN_UNIQUE_MAC_CAP * 6, "bleSeen");
    }
    if (!bleContinuityMacs) {
        bleContinuityMacs = (uint8_t (*)[6])psramAlloc(BLE_SCAN_UNIQUE_MAC_CAP * 6, "bleCont");
    }
#endif
    // ==[ FORENSIC STRUCTURES ]== evidence chains, deauth history, probe-vuln cache
    if (!forensicLog) {
        forensicLog = (ForensicLogEntry*)psramAlloc(
            MAX_FORENSIC_LOG * sizeof(ForensicLogEntry), "forensicLog");
    }
    if (!deauthHistory) {
        deauthHistory = (DeauthBurstRecord*)psramAlloc(
            MAX_DEAUTH_HISTORY * sizeof(DeauthBurstRecord), "deauthHist");
    }
    if (!probeVulnCache) {
        probeVulnCache = (ProbeVulnMatch*)psramAlloc(
            MAX_PROBE_VULN_CACHE * sizeof(ProbeVulnMatch), "probeVuln");
    }
    // ==[ ADVANCED FEATURE ALLOCATIONS ]==
    if (!clientFingerprints) {
        clientFingerprints = (ClientFingerprint*)psramAlloc(
            MAX_CLIENT_FINGERPRINTS * sizeof(ClientFingerprint), "cliFP");
    }
    if (!probeCaptureBuf) {
        probeCaptureBuf = (ProbeCapture*)psramAlloc(
            MAX_PROBE_CAPTURES * sizeof(ProbeCapture), "probeCap");
    }
    if (!beaconCaptureBuf) {
        beaconCaptureBuf = (BeaconIECapture*)psramAlloc(
            MAX_BEACON_CAPTURES * sizeof(BeaconIECapture), "bcnCap");
    }

    if (!wifiSnapshot || !wifiObservationBuffer || !trackerTable || !bleDeviceTable || !watchlist ||
        !canarySSID || !karmaBaitSSID ||
        !dedupEvilTwin || !dedupKarma || !dedupKnownAP ||
        !dedupFingerprint || !dedupSeq || !dedupRssi) {
        HAMLET_LOGLN("[RECON] FATAL: allocation failed. recon disabled.");
        state = ScanState::SUSPENDED;
        initialized = false;
        return;
    }

    memset(wifiSnapshot, 0, MAX_WIFI_SNAPSHOT * sizeof(WifiAP));
    memset(wifiObservationBuffer, 0,
           MAX_WIFI_SNAPSHOT * sizeof(Defense::WifiObservation));
    memset(trackerTable, 0, MAX_TRACKERS * sizeof(TrackerEntry));
    memset(bleDeviceTable, 0, MAX_BLE_DEVICES * sizeof(TrackerEntry));
    memset(watchlist, 0, MAX_WATCHLIST * sizeof(WatchlistEntry));
    memset(canarySSID, 0, 33);
    memset(karmaBaitSSID, 0, 33);
    memset(dedupEvilTwin, 0, dedupSize);
    memset(dedupKarma, 0, dedupSize);
    memset(dedupKnownAP, 0, dedupSize);
    memset(dedupFingerprint, 0, dedupSize);
    memset(dedupSeq, 0, dedupSize);
    memset(dedupRssi, 0, dedupSize);
#if RECON_BLE_ENABLED
    if (bleScanBuf) memset(bleScanBuf, 0, BLE_SCAN_BUF_SIZE * sizeof(BLERawHit));
    if (bleLiveFusedCounts) {
        memset(bleLiveFusedCounts, 0, BLE_SCAN_BUF_SIZE * sizeof(uint16_t));
    }
    if (bleSeenMacs) memset(bleSeenMacs, 0, BLE_SCAN_UNIQUE_MAC_CAP * 6);
    if (bleContinuityMacs) memset(bleContinuityMacs, 0, BLE_SCAN_UNIQUE_MAC_CAP * 6);
#endif
    // forensic structures are optional — recon works without them
    if (forensicLog) memset(forensicLog, 0, MAX_FORENSIC_LOG * sizeof(ForensicLogEntry));
    if (deauthHistory) memset(deauthHistory, 0, MAX_DEAUTH_HISTORY * sizeof(DeauthBurstRecord));
    if (probeVulnCache) memset(probeVulnCache, 0, MAX_PROBE_VULN_CACHE * sizeof(ProbeVulnMatch));
    if (clientFingerprints) memset(clientFingerprints, 0, MAX_CLIENT_FINGERPRINTS * sizeof(ClientFingerprint));
    if (probeCaptureBuf) memset(probeCaptureBuf, 0, MAX_PROBE_CAPTURES * sizeof(ProbeCapture));
    if (beaconCaptureBuf) memset(beaconCaptureBuf, 0, MAX_BEACON_CAPTURES * sizeof(BeaconIECapture));
    clientFingerprintCount = 0;
    probeCaptureCount.store(0, std::memory_order_relaxed);
    beaconCaptureCount.store(0, std::memory_order_relaxed);
    canaryTripped = false;
    karmaConfirmed = false;
    karmaBaitPending = false;
    lastDeauthTool = DeauthTool::UNKNOWN;

    // ==[ GHOST CANARY SSID ]== generate unique canary from device MAC
    {
        uint8_t mac[6] = {};
        esp_wifi_get_mac(WIFI_IF_STA, mac);
        uint32_t seed = fnvHash(mac, 6) ^ 0xDEADCAFE;
        snprintf(canarySSID, 33, "_c%08lX", (unsigned long)seed);
        HAMLET_LOGF("[RECON] canary SSID: %s\n", canarySSID);
    }

    forensicLogHead = forensicLogCount = 0;
    deauthHistoryHead = deauthHistoryCount = 0;
    probeVulnCount = 0;
    wifiAPCount = 0;
    openAPCount = 0;
    knownAPCount = 0;
    knownProbeReqCount = 0;
    knownProbeClientCount = 0;
    lastProbeVulnAlert = 0;
    lastProbeVulnAlertCount = 0;
    trackerCount = 0;
    bleDeviceCount.store(0, std::memory_order_relaxed);
    followingCount = 0;
    spamCount = 0;
    totalBLEDevices = 0;
    appleContinuityCount = 0;
    clearPinnedBleDevice();
    // load watchlist from NVS into runtime state
    for (int i = 0; i < MAX_WATCHLIST; i++) {
        Config::WatchlistSlot slot;
        if (Config::getWatchlistSlot(i, slot)) {
            memcpy(watchlist[i].payloadHash, slot.payloadHash, 4);
            strncpy(watchlist[i].label, slot.label, WATCHLIST_LABEL_LEN - 1);
            watchlist[i].label[WATCHLIST_LABEL_LEN - 1] = '\0';
            watchlist[i].occupied = true;
            watchlist[i].present = false;
            watchlist[i].lastSeen = 0;
        } else {
            memset(&watchlist[i], 0, sizeof(WatchlistEntry));
        }
    }
    lastWifiScan = millis();   // defer first scan — blocking scan on frame 1 jams haptic motor
    lastWifiScanComplete = 0;
    lastWifiScanAttempt = 0;
    wifiScanStarted = false;
    wifiScanResultCount = 0;
    lastWifiScanResult = WIFI_SCAN_FAILED;
    wifiScanConsecutiveFailures = 0;
    lastSentinelSniff = 0;
    deauthSniffSentinel = false;
    deauthSniffDurationMs = DEAUTH_SNIFF_MS;
    deauthSniffFixedChannel = 0;
    sentinelChannelIdx = 0;
    lastBLEScan = millis();
    dedupEvilTwinCount = dedupKarmaCount = dedupKnownAPCount = 0;
    dedupFingerprintCount = dedupSeqCount = dedupRssiCount = 0;
    lastOpenAPAlert = 0;
    resetDeauthObservationState();
    lastEvilTwinTime = 0;
    lastKarmaTime = 0;
    lastFingerprintTime = 0;
    lastSeqTime = 0;
    lastRssiTime = 0;
    recentFingerprintMismatchCount = 0;
    recentSeqAnomalyCount = 0;
    recentRssiAnomalyCount = 0;

#if RECON_BLE_ENABLED
    initBLE();   // early init — BLE must register with coex before WiFi starts
#endif

    // forensic export uses SDStorage HAL (mounted by Hamlet::init before Recon::init)
#ifndef SIMULATOR
    forensicSDReady = SDStorage::isAvailable();
    if (forensicSDReady) {
        HAMLET_LOGLN("[RECON] SD card ready for forensic export");
    }
#endif

    state = ScanState::SLEEPING;
    stateEnteredAt = millis();
    initialized = true;
    HAMLET_LOGLN("[RECON] initialized. standing by.");
}

static void checkWatchlistPresence(uint32_t now);  // forward decl

// ==[ UPDATE ]== called every frame from main loop
bool updateAcquisition(uint32_t now) {
    if (!initialized || state == ScanState::SUSPENDED) return false;

    const bool ippEnabled = Config::getIppEnabled();
    const bool bleEnabled = blePriorityMode || bleWardriveMode ||
                            (ippEnabled && Config::getIppBLEScan());
    const bool wifiEnabled = ippEnabled && Config::getIppWifiScan() &&
                             !blePriorityMode && !bleWardriveMode;

    // ==[ IPP MASTER SWITCH ]== gate ALL scanning on master toggle
    if (!ippEnabled && !blePriorityMode && !bleWardriveMode) {
        // IPP off: don't scan, but stay in SLEEPING (not SUSPENDED)
        // so resume/enterParasitic still work when toggled back on
        if (state == ScanState::BLE_SCANNING) {
            stopBLEScan();
            setState(ScanState::SLEEPING);
        } else if (state == ScanState::WIFI_SCANNING ||
                   state == ScanState::WIFI_PROCESSING) {
            if (wifiScanStarted) {
                WiFi.scanDelete();
                WiFi.mode(WIFI_OFF);
                wifiScanStarted = false;
            }
            wifiScanResultCount = 0;
            lastWifiScan = 0;
            setState(ScanState::SLEEPING);
        } else if (state == ScanState::DEAUTH_SNIFF) {
            stopDeauthSniff();
            WiFi.mode(WIFI_OFF);
            setState(ScanState::SLEEPING);
        } else if (state == ScanState::HOGWASH_HOLD) {
            esp_wifi_set_promiscuous(false);
            esp_wifi_set_promiscuous_rx_cb(NULL);
            esp_wifi_stop();
            WiFi.mode(WIFI_OFF);
            setState(ScanState::SLEEPING);
        }
        return false;
    }

    // Individual scan toggles are live policy, not decorative settings.
    if (!bleEnabled && state == ScanState::BLE_SCANNING) {
        stopBLEScan();
        setState(ScanState::SLEEPING);
    }

    if (!wifiEnabled) {
        if (state == ScanState::WIFI_SCANNING || state == ScanState::WIFI_PROCESSING) {
            if (wifiScanStarted) {
                WiFi.scanDelete();
                wifiScanStarted = false;
            }
            WiFi.mode(WIFI_OFF);
            wifiScanResultCount = 0;
            lastWifiScan = 0;
            setState(ScanState::SLEEPING);
        } else if (state == ScanState::DEAUTH_SNIFF) {
            stopDeauthSniff();
            WiFi.mode(WIFI_OFF);
            setState(ScanState::SLEEPING);
        } else if (state == ScanState::HOGWASH_HOLD) {
            esp_wifi_set_promiscuous(false);
            esp_wifi_set_promiscuous_rx_cb(NULL);
            esp_wifi_stop();
            WiFi.mode(WIFI_OFF);
            setState(ScanState::SLEEPING);
        }
    }

    switch (state) {
        case ScanState::SLEEPING: {
#if RECON_BLE_ENABLED
            // BLE scan scheduling — wardrive uses sweep count, normal uses timer
            bool bleDue;
            if (bleWardriveMode) {
                bleDue = (wardriveSweepCount >= WD_SWEEPS_PER_BLE);
            } else {
                bleDue = (lastBLEScan == 0 || (now - lastBLEScan >= currentBLEScanIntervalMs()));
            }

            if (bleEnabled && bleDue) {
                if (!bleInitialized) initBLE();
                if (bleInitialized) {
                    if (startBLEScan()) {
                        if (bleWardriveMode) wardriveSweepCount = 0;
                        setState(ScanState::BLE_SCANNING);
                    } else {
                        if (bleWardriveMode) wardriveSweepCount = 0;
                        lastBLEScan = now;
                    }
                    break;
                }
            }
#endif
            // BLE priority / wardrive: skip WiFi scans entirely
            if (wifiEnabled) {
                bool wifiDue = (lastWifiScan == 0 || (now - lastWifiScan >= currentWifiScanIntervalMs()));
                if (wifiDue) {
                    if (startWifiScan()) {
                        setState(ScanState::WIFI_SCANNING);
                    } else {
                        lastWifiScan = now;
                    }
                } else if (shouldStartSentinelSniff(now)) {
                    if (startDeauthSniff(currentSentinelSniffMs(), true, pickSentinelChannel())) {
                        setState(ScanState::DEAUTH_SNIFF);
                    } else {
                        lastSentinelSniff = now;
                    }
                }
            }
            break;
        }

        case ScanState::BLE_SCANNING: {
#if RECON_BLE_ENABLED
            {
                uint32_t scanDur = bleWardriveMode ? WD_BLE_SCAN_DURATION_MS : currentBLEScanDurationMs();
                if (!bleScanActive || (now - bleScanStart >= scanDur)) {
                    stopBLEScan();
                    pendingBleObservations = true;
                    lastBLEScan = now;
                    if (offlineScanCount < 255) offlineScanCount++;
                }
            }
#else
            setState(ScanState::SLEEPING);
#endif
            break;
        }

        case ScanState::WIFI_SCANNING: {
            int n = WiFi.scanComplete();
            if (n >= 0) {
                // scan done — harvest results
                wifiScanResultCount = (n > MAX_WIFI_SNAPSHOT) ? MAX_WIFI_SNAPSHOT : n;
                lastWifiScanResult = (int16_t)n;
                wifiScanConsecutiveFailures = 0;
                HAMLET_LOGF("[RECON] WiFi scan found %d APs\n", n);
                setState(ScanState::WIFI_PROCESSING);
            } else if (n == WIFI_SCAN_FAILED || (now - stateEnteredAt >= WIFI_SCAN_TIMEOUT_MS)) {
                // failed or timed out
                const bool timedOut = (n != WIFI_SCAN_FAILED);
                HAMLET_LOGF("[RECON] WiFi scan %s; keeping %d-AP snapshot\n",
                    timedOut ? "timed out" : "failed", wifiAPCount);
                wifiScanResultCount = 0;
                lastWifiScanResult = timedOut ? -3 : WIFI_SCAN_FAILED;
                if (wifiScanConsecutiveFailures < UINT16_MAX) wifiScanConsecutiveFailures++;
                WiFi.scanDelete();
                WiFi.mode(WIFI_OFF);
                wifiScanStarted = false;
                lastWifiScan = now;
                setState(ScanState::SLEEPING);
            }
            // else n == WIFI_SCAN_RUNNING — keep polling
            break;
        }

        case ScanState::WIFI_PROCESSING: {
            pendingWifiObservations = true;
            break;
        }

        case ScanState::DEAUTH_SNIFF: {
            uint32_t elapsed = now - stateEnteredAt;
            uint32_t sniffMs = (deauthSniffDurationMs == 0) ? DEAUTH_SNIFF_MS : deauthSniffDurationMs;
            if (deauthSniffFixedChannel > 0) {
                esp_wifi_set_channel(deauthSniffFixedChannel, WIFI_SECOND_CHAN_NONE);
            } else {
                uint32_t hopInterval = sniffMs / DEAUTH_SNIFF_CHANNELS;
                if (hopInterval == 0) hopInterval = 1;
                uint8_t wantCh = (uint8_t)(elapsed / hopInterval) + 1;
                if (wantCh > DEAUTH_SNIFF_CHANNELS) wantCh = DEAUTH_SNIFF_CHANNELS;
                esp_wifi_set_channel(wantCh, WIFI_SECOND_CHAN_NONE);
            }
            if (elapsed >= sniffMs) {
                bool holdActive = stopDeauthSniff();
                if (holdActive) {
                    // ==[ HOGWASH HOLD ]== camp on attacker channel
                    // reset sniff counters for hold monitoring
                    sniffHits.store(0, std::memory_order_relaxed);
                    memset(sniffChannelHits, 0, sizeof(sniffChannelHits));
                    esp_wifi_set_channel(hogwashHoldChannel, WIFI_SECOND_CHAN_NONE);
                    esp_err_t cbErr = esp_wifi_set_promiscuous_rx_cb(deauthSniffCB);
                    esp_err_t promiscErr = (cbErr == ESP_OK) ? esp_wifi_set_promiscuous(true) : cbErr;
                    if (promiscErr == ESP_OK) {
                        setState(ScanState::HOGWASH_HOLD);
                        HAMLET_LOGF("[HOGWASH] hold ch%d - camping on attacker\n", hogwashHoldChannel);
                    } else {
                        esp_wifi_set_promiscuous(false);
                        esp_wifi_set_promiscuous_rx_cb(NULL);
                        esp_wifi_stop();
                        WiFi.mode(WIFI_OFF);
                        setState(ScanState::SLEEPING);
                        HAMLET_LOGF("[HOGWASH] hold start failed: %s\n", esp_err_to_name(promiscErr));
                    }
                } else {
                    // stopDeauthSniff() already called esp_wifi_stop()
                    setState(ScanState::SLEEPING);
                }
            }
            break;
        }

        case ScanState::HOGWASH_HOLD: {
            uint32_t holdElapsed = now - stateEnteredAt;

            // check for new deauths during hold (callback increments sniffHits)
            if (sniffHits > hogwashHoldNewHits) {
                hogwashHoldNewHits = sniffHits;
                // attacker still active — reset quiet timer by updating inject time
                // (we'll re-inject below)
            }

            // re-inject every HOGWASH_REINJECT_MS
            if ((now - hogwashLastInjectTime) >= HOGWASH_REINJECT_MS) {
                // atomically drain sniffHits so callback increments landed since
                // the read at line 788 fold into the inject count instead of
                // getting clobbered by a plain `sniffHits = 0` store.
                uint16_t drained = sniffHits.exchange(0, std::memory_order_acq_rel);
                if (drained > hogwashHoldNewHits) hogwashHoldNewHits = drained;
                wifiChaff_injectBurst(hogwashHoldBssid, hogwashHoldChannel, hogwashHoldNewHits);
                hogwashLastInjectTime = now;
                hogwashHoldNewHits = 0;
            }

            // exit conditions: max hold time or quiet period
            bool maxReached = (holdElapsed >= HOGWASH_HOLD_MAX_MS);
            bool quiet = (hogwashHoldNewHits == 0) &&
                         ((now - hogwashLastInjectTime) >= HOGWASH_HOLD_QUIET_MS);
            if (maxReached || quiet) {
                esp_wifi_set_promiscuous(false);
                esp_wifi_set_promiscuous_rx_cb(NULL);
                esp_wifi_stop();
                WiFi.mode(WIFI_OFF);
                setState(ScanState::SLEEPING);
                HAMLET_LOGF("[HOGWASH] hold ended (%s) after %lums\n",
                              maxReached ? "max" : "quiet", (unsigned long)holdElapsed);
            }
            break;
        }

        case ScanState::PARASITIC: {
            if (now - lastParasiticAnalysis >= currentParasiticIntervalMs()) {
                lastParasiticAnalysis = now;
                pendingParasiticObservations = true;
            }
            break;
        }

        case ScanState::SUSPENDED:
            break;
    }

    return true;
}

void fuseAcquiredObservations(uint32_t now) {
    // Wardrive observations are produced by the mode before this frame's
    // pipeline update. Fuse them first to preserve their former event order.
    fuseWardriveObservations();
    if (pendingBleObservations) {
        processBLEResults();
        checkWatchlistPresence(now);
        updateTrackerFollowing(now);
    }
    if (pendingWifiObservations) {
        processWifiResults();
    }
    if (pendingParasiticObservations) {
        populateFromHunt();
        const uint32_t hash = computeSnapshotHash();
        if (hash != lastSnapshotHash) {
            lastSnapshotHash = hash;
            analyzeSnapshot();
            HAMLET_LOGF("[RECON] parasitic: %d APs, %d open, %d known\n",
                         wifiAPCount, openAPCount, knownAPCount);
        }
    }
}

void finalizeAcquisition(uint32_t now) {
    if (pendingBleObservations) {
        if (bleWardriveMode) {
            wardriveBleReady = true;
            setState(ScanState::SLEEPING);
        } else if (blePriorityMode && blePinnedPayloadValid && bleInitialized) {
            // Pinned streaming restarts immediately after the completed batch
            // has been fused, preserving the former no-SLEEPING-gap behavior.
            if (!startBLEScan()) setState(ScanState::SLEEPING);
        } else {
            setState(ScanState::SLEEPING);
        }
        pendingBleObservations = false;
    }

    if (pendingWifiObservations) {
        WiFi.scanDelete();
        wifiScanStarted = false;
        lastWifiScan = now;
        HAMLET_LOGF("[RECON] scan done. %d APs, %d open, %d known\n",
                     wifiAPCount, openAPCount, knownAPCount);
        // Reuse the radio for deauth listening only after the observation batch
        // has been fused, exactly where the former WIFI_PROCESSING branch did.
        if (startDeauthSniff(currentDeauthSniffMs(), false, 0)) {
            setState(ScanState::DEAUTH_SNIFF);
        } else {
            WiFi.mode(WIFI_OFF);
            setState(ScanState::SLEEPING);
        }
        pendingWifiObservations = false;
    }

    pendingParasiticObservations = false;
}

void updateFusion(uint32_t now) {
    fuseLiveBLEObservations(now);
    emitPendingDeauthEvent();
    checkAttackCorrelation(now);
}

void updateSideEffects(uint32_t now) {
    // chaff rotation (BLE advertising + WiFi fake handshakes)
    bleChaff_update(now);
    wifiChaff_update(now);
}

// ==[ SUSPEND / RESUME ]==
void suspend(bool releaseBle) {
    exportSessionSummary();
    bleChaff_stop();
    wifiChaff_stop();
    stopBLEScan();
    if (releaseBle) deinitBLE();
    if (state == ScanState::DEAUTH_SNIFF) {
        stopDeauthSniff();
        if (releaseBle) WiFi.mode(WIFI_OFF);
        else WiFi.mode(WIFI_STA);
    } else if (state == ScanState::HOGWASH_HOLD) {
        // WiFi.mode() handles promiscuous disable + stop + deinit internally.
        // Explicit esp_wifi_stop() before it caused double-stop coex crashes
        // when Spectrum followed immediately after HOGWASH teardown.
        if (releaseBle) WiFi.mode(WIFI_OFF);
        else WiFi.mode(WIFI_STA);
    }
    if (wifiScanStarted) {
        WiFi.scanDelete();
        // AP/STA handoffs keep NimBLE warm. Dropping the shared controller to
        // WIFI_OFF while BT is alive can hit coexistence un-init timeouts.
        if (releaseBle) WiFi.mode(WIFI_OFF);
        wifiScanStarted = false;
    }
    setState(ScanState::SUSPENDED);
    HAMLET_LOGLN("[RECON] suspended — radio conflict avoidance");
}

void resume() {
    if (!wifiSnapshot) return;
    initialized = true;
#if RECON_BLE_ENABLED
    if (!bleInitialized) initBLE();  // re-init before WiFi — coex needs BLE first
    lastBLEScan = millis() - BLE_SCAN_INTERVAL_MS + 5000;  // BLE scan in 5s
#endif
    lastWifiScan = millis() - currentWifiScanIntervalMs() + 5000;  // delay first scan 5s after resume
    lastSentinelSniff = millis();  // defer sentinel sniff — was 0 which bypasses interval check
    // reset watchlist timestamps — prevent spurious EXIT after mode gap
    uint32_t now = millis();
    for (int i = 0; i < MAX_WATCHLIST; i++) {
        if (watchlist[i].occupied && watchlist[i].present) {
            watchlist[i].lastSeen = now;
        }
    }
    setState(ScanState::SLEEPING);
    HAMLET_LOGLN("[RECON] resumed");
}

void enterParasitic() {
    if (!wifiSnapshot) return;
    bleChaff_stop();
    wifiChaff_stop();
    stopBLEScan();
    deinitBLE();
    resetDeauthObservationState();
    if (state == ScanState::DEAUTH_SNIFF) {
        stopDeauthSniff();
        WiFi.mode(WIFI_OFF);
    } else if (state == ScanState::HOGWASH_HOLD) {
        esp_wifi_set_promiscuous(false);
        esp_wifi_set_promiscuous_rx_cb(NULL);
        esp_wifi_stop();
        WiFi.mode(WIFI_OFF);
    }
    if (wifiScanStarted) {
        WiFi.scanDelete();
        WiFi.mode(WIFI_OFF);
        wifiScanStarted = false;
    }
    initialized = true;
    lastParasiticAnalysis = 0;
    lastSnapshotHash = 0;
    lastSentinelSniff = 0;
    setState(ScanState::PARASITIC);
    HAMLET_LOGLN("[RECON] parasitic — feeding from hunt");
}

void forceBlePriority() {
    blePriorityMode = true;

    if (state == ScanState::WIFI_SCANNING || state == ScanState::WIFI_PROCESSING) {
        if (wifiScanStarted) {
            WiFi.scanDelete();
            wifiScanStarted = false;
        }
        WiFi.mode(WIFI_OFF);
        setState(ScanState::SLEEPING);
    } else if (state == ScanState::DEAUTH_SNIFF) {
        stopDeauthSniff();
        WiFi.mode(WIFI_OFF);
        setState(ScanState::SLEEPING);
    } else if (state == ScanState::HOGWASH_HOLD) {
        esp_wifi_set_promiscuous(false);
        esp_wifi_set_promiscuous_rx_cb(NULL);
        esp_wifi_stop();
        WiFi.mode(WIFI_OFF);
        setState(ScanState::SLEEPING);
    }

    // Force immediate BLE scan on next update cycle
    lastBLEScan = 0;
    HAMLET_LOGLN("[RECON] BLE priority ON — WiFi scans suppressed");
}

void unforceBlePriority() {
    blePriorityMode = false;
    clearPinnedBleDevice();
    HAMLET_LOGLN("[RECON] BLE priority OFF — normal cadence");
}

// ==[ WARDRIVE BLE MODE ]== sweep-paced BLE, WiFi suppressed (wardrive owns radio)
void forceWardriveBle() {
    bleWardriveMode = true;
    wardriveSweepCount = 0;
    wardriveBleReady = false;

    // kill any in-progress WiFi/deauth (same pattern as forceBlePriority)
    if (state == ScanState::WIFI_SCANNING || state == ScanState::WIFI_PROCESSING) {
        if (wifiScanStarted) { WiFi.scanDelete(); wifiScanStarted = false; }
        WiFi.mode(WIFI_OFF);
        setState(ScanState::SLEEPING);
    } else if (state == ScanState::DEAUTH_SNIFF) {
        stopDeauthSniff();
        WiFi.mode(WIFI_OFF);
        setState(ScanState::SLEEPING);
    } else if (state == ScanState::HOGWASH_HOLD) {
        esp_wifi_set_promiscuous(false);
        esp_wifi_set_promiscuous_rx_cb(NULL);
        esp_wifi_stop();
        WiFi.mode(WIFI_OFF);
        setState(ScanState::SLEEPING);
    }

#if RECON_BLE_ENABLED
    if (!bleInitialized) initBLE();
#endif
    lastBLEScan = 0;
    HAMLET_LOGLN("[RECON] wardrive BLE ON — WiFi suppressed, sweep-paced");
}

void unforceWardriveBle() {
    bleWardriveMode = false;
    wardriveSweepCount = 0;
    wardriveBleReady = false;
    HAMLET_LOGLN("[RECON] wardrive BLE OFF — normal cadence");
}

void onWardriveSweepComplete() {
    if (bleWardriveMode) wardriveSweepCount++;
}

bool wardriveWantsBleWindow() {
    if (!bleWardriveMode) return false;
#if RECON_BLE_ENABLED
    if (!bleInitialized) return false;
    if (state == ScanState::BLE_SCANNING) return true;
    return wardriveSweepCount >= WD_SWEEPS_PER_BLE;
#else
    return false;
#endif
}

bool consumeWardriveBleReady() {
    if (!wardriveBleReady) return false;
    wardriveBleReady = false;
    return true;
}

void pinBleDevice(const uint8_t* payloadHash) {
    if (!payloadHash) {
        clearPinnedBleDevice();
        return;
    }
    memcpy(blePinnedPayloadHash, payloadHash, sizeof(blePinnedPayloadHash));
    blePinnedPayloadValid = true;
}

void clearPinnedBleDevice() {
    blePinnedPayloadValid = false;
    memset(blePinnedPayloadHash, 0, sizeof(blePinnedPayloadHash));
}

void setActiveScan(bool active) {
#if RECON_BLE_ENABLED
    bleActiveScanMode = active;
    HAMLET_LOGF("[RECON] BLE active scan %s\n", active ? "ON — sending SCAN_REQ" : "OFF — passive stealth");
#endif
}
bool isActiveScanEnabled() {
#if RECON_BLE_ENABLED
    return bleActiveScanMode;
#else
    return false;
#endif
}

void toggleChaff() {
#if RECON_BLE_ENABLED
    bleChaff_toggle();
#endif
}
bool isChaffActive() {
#if RECON_BLE_ENABLED
    return bleChaff_isActive();
#else
    return false;
#endif
}

// ==[ BLE WATCHLIST API ]==

bool isWatchlisted(const uint8_t* payloadHash) {
    if (!payloadHash) return false;
    for (int i = 0; i < MAX_WATCHLIST; i++) {
        if (watchlist[i].occupied &&
            memcmp(watchlist[i].payloadHash, payloadHash, 4) == 0)
            return true;
    }
    return false;
}

int findWatchlistSlot(const uint8_t* payloadHash) {
    if (!payloadHash) return -1;
    for (int i = 0; i < MAX_WATCHLIST; i++) {
        if (watchlist[i].occupied &&
            memcmp(watchlist[i].payloadHash, payloadHash, 4) == 0)
            return i;
    }
    return -1;
}

uint8_t getWatchlistCount() {
    uint8_t n = 0;
    for (int i = 0; i < MAX_WATCHLIST; i++) if (watchlist[i].occupied) n++;
    return n;
}

const WatchlistEntry* getWatchlist() { return watchlist; }

bool addToWatchlist(const uint8_t* payloadHash, const char* label) {
    if (!payloadHash || !label) return false;
    // already on watchlist?
    if (findWatchlistSlot(payloadHash) >= 0) return false;
    // find first empty slot
    for (int i = 0; i < MAX_WATCHLIST; i++) {
        if (!watchlist[i].occupied) {
            memcpy(watchlist[i].payloadHash, payloadHash, 4);
            strncpy(watchlist[i].label, label, WATCHLIST_LABEL_LEN - 1);
            watchlist[i].label[WATCHLIST_LABEL_LEN - 1] = '\0';
            watchlist[i].occupied = true;
            watchlist[i].present = false;
            watchlist[i].lastSeen = 0;
            Config::setWatchlistSlot(i, payloadHash, label);
            HAMLET_LOGF("[RECON] watchlist[%d] = '%s'\n", i, label);
            return true;
        }
    }
    return false;  // full
}

bool removeFromWatchlist(uint8_t slotIdx) {
    if (slotIdx >= MAX_WATCHLIST || !watchlist[slotIdx].occupied) return false;
    HAMLET_LOGF("[RECON] watchlist[%d] '%s' removed\n", slotIdx, watchlist[slotIdx].label);
    memset(&watchlist[slotIdx], 0, sizeof(WatchlistEntry));
    Config::clearWatchlistSlot(slotIdx);
    return true;
}

void updateWatchlistLabel(uint8_t slotIdx, const char* label) {
    if (slotIdx >= MAX_WATCHLIST || !watchlist[slotIdx].occupied || !label) return;
    strncpy(watchlist[slotIdx].label, label, WATCHLIST_LABEL_LEN - 1);
    watchlist[slotIdx].label[WATCHLIST_LABEL_LEN - 1] = '\0';
    Config::setWatchlistSlot(slotIdx, watchlist[slotIdx].payloadHash, label);
}

static void checkWatchlistPresence(uint32_t now) {
    for (int w = 0; w < MAX_WATCHLIST; w++) {
        if (!watchlist[w].occupied) continue;

        // scan catalog for matching payloadHash
        bool seen = false;
        int8_t rssi = -127;
        int dc = bleDeviceCount.load(std::memory_order_relaxed);
        for (int i = 0; i < dc; i++) {
            if (memcmp(bleDeviceTable[i].payloadHash, watchlist[w].payloadHash, 4) == 0) {
                seen = true;
                rssi = bleDeviceTable[i].rssiSmooth;
                watchlist[w].lastSeen = bleDeviceTable[i].lastSeen;
                break;
            }
        }

        bool wasPresent = watchlist[w].present;

        // ENTER: seen with RSSI above threshold, was absent
        if (seen && rssi > WATCHLIST_ENTER_RSSI && !wasPresent) {
            watchlist[w].present = true;
            ReconEventData ev = {};
            ev.event = ReconEvent::WATCHLIST_ENTER;
            ev.rssi = rssi;
            snprintf(ev.detail, sizeof(ev.detail), "%s", watchlist[w].label);
            pushEvent(ev);
        }

        // EXIT: was present, not seen for 60s
        if (wasPresent && watchlist[w].lastSeen > 0 &&
            (now - watchlist[w].lastSeen > WATCHLIST_EXIT_TIMEOUT_MS)) {
            watchlist[w].present = false;
            ReconEventData ev = {};
            ev.event = ReconEvent::WATCHLIST_EXIT;
            snprintf(ev.detail, sizeof(ev.detail), "%s", watchlist[w].label);
            pushEvent(ev);
        }
    }
}

void pauseBLEScanForGATT() {
#if RECON_BLE_ENABLED
    if (bleScanActive) {
        NimBLEScan* pScan = NimBLEDevice::getScan();
        pScan->stop();
        // do NOT clearResults here — NimBLE uses cached scan data for
        // peer address resolution during GATT connect. clearing it
        // before connect causes connection failure.
        bleScanActive = false;
        delay(120);  // let controller finish scan→idle transition
        HAMLET_LOGLN("[RECON] BLE scan paused for GATT");
    }
#endif
}
void resumeBLEScanFromGATT() {
#if RECON_BLE_ENABLED
    if (bleInitialized && !bleScanActive) {
        NimBLEScan* pScan = NimBLEDevice::getScan();
        pScan->clearResults();  // clear stale results before restarting
        startBLEScan();
        HAMLET_LOGLN("[RECON] BLE scan resumed after GATT");
    }
#endif
}

// ==[ STATE QUERIES ]==
bool isActive() { return initialized && state != ScanState::SUSPENDED; }
bool isScanning() { return state == ScanState::WIFI_SCANNING || state == ScanState::BLE_SCANNING || state == ScanState::DEAUTH_SNIFF || state == ScanState::HOGWASH_HOLD; }
bool isBleAvailable() { return RECON_BLE_ENABLED != 0; }
bool isBleInitialized() { return bleInitialized; }
bool isParasiticMode() { return state == ScanState::PARASITIC; }
bool requestWifiScan() {
    if (!initialized || state == ScanState::SUSPENDED || state == ScanState::PARASITIC) return false;
    if (!Config::getIppEnabled() || !Config::getIppWifiScan()) return false;
    if (blePriorityMode || bleWardriveMode) return false;
    lastWifiScan = 0;
    return true;
}

WifiPipelineStatus getWifiPipelineStatus() {
    WifiPipelineStatus status = {};
    status.lastResult = lastWifiScanResult;
    status.consecutiveFailures = wifiScanConsecutiveFailures;
    status.lastAttemptMs = lastWifiScanAttempt;
    status.lastCompleteMs = lastWifiScanComplete;

    if (!initialized || state == ScanState::SUSPENDED) {
        status.state = WifiPipelineState::SUSPENDED;
    } else if (!Config::getIppEnabled() || !Config::getIppWifiScan()) {
        status.state = WifiPipelineState::OFF;
    } else if (state == ScanState::PARASITIC) {
        status.state = WifiPipelineState::PARASITIC;
    } else if (blePriorityMode || bleWardriveMode) {
        status.state = WifiPipelineState::SUPPRESSED;
    } else if (state == ScanState::WIFI_SCANNING) {
        status.state = WifiPipelineState::SCANNING;
    } else if (state == ScanState::WIFI_PROCESSING) {
        status.state = WifiPipelineState::PROCESSING;
    } else if (wifiScanConsecutiveFailures > 0) {
        status.state = WifiPipelineState::FAILED;
    } else if (lastWifiScanComplete > 0) {
        status.state = WifiPipelineState::READY;
    } else {
        status.state = WifiPipelineState::WAITING;
    }

    if (status.state == WifiPipelineState::WAITING ||
        status.state == WifiPipelineState::READY ||
        status.state == WifiPipelineState::FAILED) {
        uint32_t interval = currentWifiScanIntervalMs();
        uint32_t elapsed = millis() - lastWifiScan;
        status.nextDueMs = (lastWifiScan == 0 || elapsed >= interval) ? 0 : interval - elapsed;
    }
    return status;
}

const char* wifiPipelineStateLabel(WifiPipelineState pipelineState) {
    switch (pipelineState) {
        case WifiPipelineState::OFF:       return "OFF";
        case WifiPipelineState::SUSPENDED:  return "SUSP";
        case WifiPipelineState::SUPPRESSED: return "HOLD";
        case WifiPipelineState::PARASITIC:  return "PARA";
        case WifiPipelineState::WAITING:    return "WAIT";
        case WifiPipelineState::SCANNING:   return "SCAN";
        case WifiPipelineState::PROCESSING: return "PROC";
        case WifiPipelineState::READY:      return "LIVE";
        case WifiPipelineState::FAILED:     return "ERR";
        default:                            return "?";
    }
}
CadenceTier getCadenceTier() { return computeCadenceTier(); }
uint32_t getCurrentWifiScanIntervalMs() { return currentWifiScanIntervalMs(); }
uint32_t getCurrentDeauthSniffMs() { return currentDeauthSniffMs(); }
uint32_t getCurrentParasiticIntervalMs() { return currentParasiticIntervalMs(); }
uint32_t getCurrentSentinelIntervalMs() { return currentSentinelIntervalMs(); }

int getTrackerCount() { return trackerCount; }
int getFollowingCount() { return followingCount; }
int getSpamCount() { return spamCount; }
bool hasThreats() {
    return followingCount > 0 ||
           spamCount > 0 ||
           knownAPCount > 0 ||
           knownProbeReqCount > 0 ||
           openAPCount >= 3 ||
           isDeauthActive() ||
           isFingerprintMismatchActive() ||
           isSeqAnomalyActive() ||
           isRssiAnomalyActive() ||
           isEvilTwinActive() ||
           isKarmaActive();
}
const TrackerEntry* getTrackers() { return trackerTable; }
int getTrackerTableSize() { return trackerCount; }
const TrackerEntry* getBleDevices() { return bleDeviceTable; }
int getBleDeviceTableSize() { return bleDeviceCount.load(std::memory_order_relaxed); }

int getLastWifiAPCount() { return wifiAPCount; }
int getOpenAPCount() { return openAPCount; }
int getKnownAPCount() { return knownAPCount; }
uint16_t getKnownProbeRequestCount() { return knownProbeReqCount; }
uint16_t getKnownProbeClientCount() { return knownProbeClientCount; }
const WifiAP* getWifiSnapshot() { return wifiSnapshot; }
int getWifiSnapshotCount() { return wifiAPCount; }

bool hasEvent() { return DefensePipeline::hasEvent(); }
ReconEventData popEvent() {
    return DefensePipeline::popEvent();
}

void ingestDeauthObservation(uint8_t channel, int8_t rssi, uint8_t subtype,
                             const uint8_t* srcMac, DeauthSourceOrigin origin,
                             const uint8_t* targetBssid, uint16_t reasonCode) {
    // Hunt owns promiscuous RX — deauth forensics run via Recon sniff outside hunt only.
    if (Hunt::isActive()) return;

    uint32_t now = millis();
    if (channel > 13) channel = 0;

    bool resetWindow = (huntDeauthHits == 0) ||
                       (huntDeauthLastObs == 0) ||
                       (now - huntDeauthLastObs > HUNT_DEAUTH_WINDOW_MS);
    if (resetWindow) {
        huntDeauthWindowStart = now;
        huntDeauthHits = 0;
        huntDeauthPeakRSSI = -127;
        huntDeauthPeakChan = channel;
        huntDeauthSubtypeDeauth = 0;
        huntDeauthSubtypeDisassoc = 0;
        huntDeauthSourceCount = 0;
        huntDeauthEventPending = false;
        memset((void*)huntDeauthSources, 0, sizeof(huntDeauthSources));
        memset((void*)huntDeauthTargetBssid, 0, 6);
        huntDeauthReasonCode = 0;
        huntDeauthReasonHits = 0;
    }

    if (huntDeauthHits < 0xFFFF) huntDeauthHits++;
    if (subtype == 0x0C && huntDeauthSubtypeDeauth < 0xFFFF) huntDeauthSubtypeDeauth++;
    if (subtype == 0x0A && huntDeauthSubtypeDisassoc < 0xFFFF) huntDeauthSubtypeDisassoc++;

    if (srcMac) {
        uint8_t srcCount = huntDeauthSourceCount;  // snapshot volatile
        bool foundSrc = false;
        for (uint8_t i = 0; i < srcCount; i++) {
            if (memcmp((const void*)huntDeauthSources[i].mac, srcMac, 6) == 0) {
                if (huntDeauthSources[i].hits < 0xFFFF) huntDeauthSources[i].hits++;
                foundSrc = true;
                break;
            }
        }
        if (!foundSrc && srcCount < MAX_DEAUTH_SOURCES) {
            memcpy((void*)huntDeauthSources[srcCount].mac, srcMac, 6);
            huntDeauthSources[srcCount].hits = 1;
            huntDeauthSourceCount = srcCount + 1;
        }
    }

    if (rssi > huntDeauthPeakRSSI) {
        huntDeauthPeakRSSI = rssi;
        huntDeauthPeakChan = channel;
    }

    // target BSSID: store first seen (most likely the actual target in a focused attack)
    if (targetBssid && huntDeauthTargetBssid[0] == 0 && huntDeauthTargetBssid[1] == 0) {
        memcpy((void*)huntDeauthTargetBssid, targetBssid, 6);
    }

    // reason code: track dominant (most-seen) reason in burst
    if (reasonCode > 0) {
        if (reasonCode == huntDeauthReasonCode) {
            if (huntDeauthReasonHits < 0xFFFF) huntDeauthReasonHits++;
        } else if (huntDeauthReasonHits == 0) {
            huntDeauthReasonCode = reasonCode;
            huntDeauthReasonHits = 1;
        }
    }

    huntDeauthLastObs = now;
    deauthTotal++;
    deauthLastTime = now;
    deauthLastBurstCount = huntDeauthHits;
    deauthLastUniqueSources = huntDeauthSourceCount;
    deauthLastPeakRSSI = huntDeauthPeakRSSI;
    deauthLastPeakChan = huntDeauthPeakChan;
    deauthLastDominantChan = (channel > 0) ? channel : huntDeauthPeakChan;
    uint32_t windowMs = (huntDeauthWindowStart > 0) ? (now - huntDeauthWindowStart + 1) : 1;
    deauthLastPPS = (uint16_t)((huntDeauthHits * 1000u + (windowMs / 2u)) / windowMs);
    deauthLastSubtypeDeauth = huntDeauthSubtypeDeauth;
    deauthLastSubtypeDisassoc = huntDeauthSubtypeDisassoc;
    deauthLastOrigin = origin;

    if (huntDeauthHits == 2) {
        huntDeauthEventPending = true;
    }
}

void reportKarmaFromProbeResponse(const char* ssid, const char* detail) {
    lastKarmaTime = millis();
    ReconEventData ev = {};
    ev.event = ReconEvent::KARMA_HONEYPOT;
    if (ssid) { strncpy(ev.ssid, ssid, 32); ev.ssid[32] = '\0'; }
    if (detail) { strncpy(ev.detail, detail, sizeof(ev.detail) - 1); }
    pushEvent(ev);
    HAMLET_LOGF("[RECON] KARMA from probe-resp: '%s' %s\n", ssid ? ssid : "?", detail ? detail : "");
}

uint32_t getLastBLEScanTime() { return lastBLEScan; }
uint32_t getLastWifiScanTime() { return lastWifiScanComplete; }
uint32_t getTimeSinceLastScan() {
    uint32_t latest = lastWifiScanComplete;
    if (lastBLEScan > latest) latest = lastBLEScan;
    if (latest == 0) return 0xFFFFFFFF;
    return millis() - latest;
}
uint16_t getTotalBLEDevicesSeen() { return totalBLEDevices; }
uint16_t getAppleContinuityCount() { return appleContinuityCount; }
const char* proximityLabel(int8_t rssi) { return proximityLabelInternal(rssi); }
float estimateDistance(int8_t rssi, int8_t txPower) { return estimateDistanceInternal(rssi, txPower); }
const char* appearanceLabel(uint16_t appearance) { return appearanceLabelInternal(appearance); }
const char* manufacturerLabel(uint16_t companyId) { return manufacturerLabelInternal(companyId); }
const char* classOfDeviceLabel(uint32_t cod) { return classOfDeviceLabelInternal(cod); }

// ==[ THREAT TYPE → LABEL ]== raw ThreatType → 4-char display label
static const char* threatTypeLabelInternal(ThreatType type) {
    switch (type) {
        case ThreatType::UNKNOWN:              return "BLE";
        case ThreatType::AIRTAG:               return "ATg";
        case ThreatType::SMARTTAG:             return "SmTg";
        case ThreatType::TILE:                 return "Tile";
        case ThreatType::FAST_PAIR:            return "FPr";
        case ThreatType::BLE_SPAM:             return "Spam";
        case ThreatType::GENERIC_TRACKER:      return "Trk";
        case ThreatType::IBEACON:              return "iBcn";
        case ThreatType::EDDYSTONE:            return "Eddy";
        case ThreatType::APPLE_NEARBY:         return "aNr";
        case ThreatType::SUSPICIOUS_PERIPHERAL: return "SUS!";
        case ThreatType::FLIPPER:              return "Flip";
        case ThreatType::HID_DEVICE:           return "HID!";
        case ThreatType::SMARTTAG_UNREGISTERED: return "SmT!";
        case ThreatType::XIAOMI_TRACKER:       return "XiTk";
        case ThreatType::SIDEWALK_BEACON:      return "SWlk";
        case ThreatType::EXPOSURE_NOTIF:       return "GAEN";
        case ThreatType::FMDN:                 return "FMDN";
        default:                               return "BLE";
    }
}
const char* threatTypeLabel(ThreatType type) { return threatTypeLabelInternal(type); }

// ==[ DEVICE LABEL FROM METADATA ]== appearance→name→CoD→threatType cascade
// shared with recon_ble.cpp via recon_internal.h
const char* deviceLabelFromMeta(ThreatType type, uint16_t appearance,
                                uint32_t classOfDevice, const char* name) {
    // 1. GAP Appearance (most reliable when present)
    if (appearance != 0) {
        uint16_t cat = appearance >> 6;
        switch (cat) {
            case 1:  return "Phn";    // phone
            case 2:  return "PC";     // computer/laptop
            case 3:  return "Wtch";   // watch
            case 4:  return "Clk";    // clock
            case 5:  return "Disp";   // display
            case 6:  return "Rmt";    // remote control
            case 7:  return "Glss";   // glasses/wearable
            case 8:  return "Tag";    // tag/beacon
            case 9:  return "Key";    // keychain
            case 10: return "Mdia";   // media player
            case 15: return "HID";    // HID device
            case 49: return "Snsr";   // sensor
            case 52: return "Lock";   // smart lock
            default: break;
        }
        if (appearance == 0x0541) return "Bcn";   // location beacon
        if (appearance == 0x0580) return "Hear";   // hearing aid
    }

    // 2. Name heuristics (common BLE device names)
    if (name && name[0]) {
        if (strncasecmp(name, "AirPod", 6) == 0)       return "APod";
        if (strncasecmp(name, "Apple Watch", 11) == 0)  return "AWch";
        if (strncasecmp(name, "Galaxy Watch", 12) == 0) return "GWch";
        if (strncasecmp(name, "Galaxy Buds", 11) == 0)  return "GBud";
        if (strncasecmp(name, "Pixel Buds", 10) == 0)   return "PBud";
        if (strncasecmp(name, "Pixel Watch", 11) == 0)  return "PWch";
        if (strncasecmp(name, "JBL", 3) == 0)           return "Spkr";
        if (strncasecmp(name, "Bose", 4) == 0)          return "Spkr";
        if (strncasecmp(name, "Sonos", 5) == 0)         return "Spkr";
        if (strncasecmp(name, "Buds", 4) == 0)          return "Buds";
    }

    // 3. Class of Device (BT Classic CoD major class)
    if (classOfDevice != 0) {
        uint8_t major = (classOfDevice >> 8) & 0x1F;
        switch (major) {
            case 1: return "PC";
            case 2: return "Phn";
            case 4: return "Aud";    // audio/headset
            case 5: return "Prph";   // peripheral
            default: break;
        }
    }

    // 4. ThreatType fallback
    return threatTypeLabelInternal(type);
}

// ==[ DEVICE LABEL ]== smart label for TrackerEntry — used by all display consumers
const char* deviceLabel(const TrackerEntry& te) {
    // spam overrides everything (security-critical display)
    if (te.flags & FLAG_SPAM) {
        switch ((SpamPlatform)te.spamPlatform) {
            case SpamPlatform::IOS:     return "SP:i";
            case SpamPlatform::WINDOWS: return "SP:W";
            case SpamPlatform::SAMSUNG: return "SP:S";
            case SpamPlatform::ANDROID: return "SP:A";
            default:                    return "Spam";
        }
    }
    return deviceLabelFromMeta(te.type, te.appearance, te.classOfDevice, te.name);
}

uint32_t getCurrentBLEScanIntervalMs() { return currentBLEScanIntervalMs(); }
uint32_t getCurrentBLEScanDurationMs() { return currentBLEScanDurationMs(); }

// ==[ DEAUTH DETECTION API ]==
uint32_t getDeauthCount() { return deauthTotal; }
uint16_t getLastDeauthBurstCount() { return deauthLastBurstCount; }
uint8_t  getLastDeauthUniqueSources() { return deauthLastUniqueSources; }
int8_t   getLastDeauthRSSI() { return deauthLastPeakRSSI; }
uint8_t  getLastDeauthChannel() { return deauthLastPeakChan; }
uint8_t  getLastDeauthDominantChannel() { return deauthLastDominantChan; }
uint16_t getLastDeauthPPS() { return deauthLastPPS; }
uint16_t getLastDeauthSubtypeCount() { return deauthLastSubtypeDeauth; }
uint16_t getLastDisassocSubtypeCount() { return deauthLastSubtypeDisassoc; }
uint32_t getLastDeauthTime() { return deauthLastTime; }
bool     isDeauthActive() {
    return deauthLastTime > 0 && (millis() - deauthLastTime < DEAUTH_ACTIVE_WINDOW_MS);
}

bool isEvilTwinActive() {
    return lastEvilTwinTime > 0 && (millis() - lastEvilTwinTime < EVIL_TWIN_ACTIVE_MS);
}

bool isKarmaActive() {
    return lastKarmaTime > 0 && (millis() - lastKarmaTime < KARMA_ACTIVE_MS);
}

bool isFingerprintMismatchActive() {
    return recentFingerprintMismatchCount > 0 ||
           (lastFingerprintTime > 0 && (millis() - lastFingerprintTime < FINGERPRINT_ACTIVE_MS));
}
bool isSeqAnomalyActive() {
    return recentSeqAnomalyCount > 0 ||
           (lastSeqTime > 0 && (millis() - lastSeqTime < SEQ_ACTIVE_MS));
}
bool isRssiAnomalyActive() {
    return recentRssiAnomalyCount > 0 ||
           (lastRssiTime > 0 && (millis() - lastRssiTime < RSSI_ACTIVE_MS));
}
uint8_t getRecentFingerprintMismatchCount() { return recentFingerprintMismatchCount; }
uint8_t getRecentSeqAnomalyCount() { return recentSeqAnomalyCount; }
uint8_t getRecentRssiAnomalyCount() { return recentRssiAnomalyCount; }

// ==[ FORCED CADENCE ]== external override (DEFHOG4 full-screen mode)
static int8_t forcedCadence = -1;  // -1 = no override, 0+ = CadenceTier value

void setForcedCadence(CadenceTier tier) { forcedCadence = (int8_t)tier; }
void clearForcedCadence() { forcedCadence = -1; }

static CadenceTier computeCadenceTier() {
    if (forcedCadence >= 0) return (CadenceTier)forcedCadence;
    // PARASITIC: hunt owns the radio — ignore stale deauth sniff state (would be our TX)
    const bool deauthThreat = (state != ScanState::PARASITIC) && isDeauthActive();
    if (deauthThreat ||
        isEvilTwinActive() ||
        isKarmaActive() ||
        isFingerprintMismatchActive() ||
        knownProbeReqCount >= 3 ||
        knownProbeClientCount >= 2 ||
        knownAPCount >= 3 ||
        openAPCount >= 5 ||
        followingCount > 0 ||
        spamCount > 0) {
        return CadenceTier::AGGRESSIVE;
    }

    if (knownProbeReqCount > 0 ||
        knownAPCount > 0 ||
        isSeqAnomalyActive() ||
        isRssiAnomalyActive() ||
        openAPCount >= 3 ||
        trackerCount > 0) {
        return CadenceTier::ELEVATED;
    }

    return CadenceTier::NORMAL;
}

static uint32_t currentWifiScanIntervalMs() {
    switch (computeCadenceTier()) {
        case CadenceTier::AGGRESSIVE: return WIFI_SCAN_INTERVAL_AGGRESSIVE_MS;
        case CadenceTier::ELEVATED:   return WIFI_SCAN_INTERVAL_ELEVATED_MS;
        default:                      return WIFI_SCAN_INTERVAL_MS;
    }
}

static uint32_t currentDeauthSniffMs() {
    switch (computeCadenceTier()) {
        case CadenceTier::AGGRESSIVE: return DEAUTH_SNIFF_AGGRESSIVE_MS;
        case CadenceTier::ELEVATED:   return DEAUTH_SNIFF_ELEVATED_MS;
        default:                      return DEAUTH_SNIFF_MS;
    }
}

static uint32_t currentParasiticIntervalMs() {
    switch (computeCadenceTier()) {
        case CadenceTier::AGGRESSIVE: return PARASITIC_INTERVAL_AGGRESSIVE_MS;
        case CadenceTier::ELEVATED:   return PARASITIC_INTERVAL_ELEVATED_MS;
        default:                      return PARASITIC_INTERVAL_MS;
    }
}

static uint32_t currentSentinelIntervalMs() {
    switch (computeCadenceTier()) {
        case CadenceTier::AGGRESSIVE: return DEAUTH_SENTINEL_INTERVAL_AGGRESSIVE_MS;
        case CadenceTier::ELEVATED:   return DEAUTH_SENTINEL_INTERVAL_ELEVATED_MS;
        default:                      return 0;
    }
}

static uint32_t currentSentinelSniffMs() {
    switch (computeCadenceTier()) {
        case CadenceTier::AGGRESSIVE: return DEAUTH_SENTINEL_SNIFF_AGGRESSIVE_MS;
        case CadenceTier::ELEVATED:   return DEAUTH_SENTINEL_SNIFF_ELEVATED_MS;
        default:                      return 0;
    }
}

uint32_t currentBLEScanIntervalMs() {
    if (blePriorityMode) return 10000;  // 10s between scans in BLE_SCANNER mode
    switch (computeCadenceTier()) {
        case CadenceTier::AGGRESSIVE: return 20000;
        case CadenceTier::ELEVATED:   return 40000;
        default:                      return 60000;
    }
}

uint32_t currentBLEScanDurationMs() {
    if (blePriorityMode) return 8000;   // 8s scan window in BLE_SCANNER mode
    if (bleWardriveMode) return WD_BLE_SCAN_DURATION_MS;  // 4s wardrive interleave window
    return (computeCadenceTier() == CadenceTier::AGGRESSIVE) ? 10000 : 8000;
}

static const char* proximityLabelInternal(int8_t rssi) {
    if (rssi > -50) return "CLOSE";
    if (rssi > -70) return "NEAR";
    if (rssi > -85) return "FAR";
    return "EDGE";
}

// ==[ TX POWER DISTANCE MODEL ]== log-distance path loss: d = 10^((txPower - rssi) / (10*n))
// n=2.5 typical indoor, reference distance 1m. returns meters, -1 if no txPower available.
static float estimateDistanceInternal(int8_t rssi, int8_t txPower) {
    if (txPower == -127 || txPower == 0) return -1.0f;
    float diff = (float)(txPower - rssi);
    if (diff < 0) return 0.1f;  // closer than reference
    return powf(10.0f, diff / 25.0f);  // 10*n = 10*2.5 = 25
}

// ==[ GAP APPEARANCE → LABEL ]== BLE Assigned Numbers §2.6 (top categories)
static const char* appearanceLabelInternal(uint16_t appearance) {
    if (appearance == 0) return nullptr;
    uint16_t cat = appearance >> 6;  // top 10 bits = category
    switch (cat) {
        case 1:  return "PHONE";
        case 2:  return "PC";
        case 3:  return "WATCH";
        case 4:  return "CLOCK";
        case 5:  return "DISPLAY";
        case 6:  return "REMOTE";
        case 7:  return "GLASSES";
        case 8:  return "TAG";
        case 9:  return "KEYCHAIN";
        case 10: return "MEDIA";
        case 12: return "BARCODE";
        case 15: return "HID";
        case 17: return "GLUCOSE";
        case 18: return "PULSE";
        case 19: return "HEART";
        case 20: return "BP";        // blood pressure
        case 21: return "THERMO";
        case 22: return "WEIGHT";
        case 49: return "SENSOR";
        case 50: return "LIGHT";
        case 52: return "LOCK";
        case 53: return "GPS";
        case 54: return "VEHICLE";
        default: break;
    }
    // sub-category fallbacks for common flat values
    switch (appearance) {
        case 0x0080: return "GENERIC";   // generic category=0
        case 0x0541: return "BEACON";    // location beacon
        case 0x0580: return "HEARING";   // hearing aid
        default: break;
    }
    return "DEV";
}

// ==[ COMPANY ID → VENDOR ]== BLE SIG company identifiers (most common in wild)
static const char* manufacturerLabelInternal(uint16_t companyId) {
    switch (companyId) {
        case 0x004C: return "Apple";
        case 0x0006: return "MSFT";
        case 0x000D: return "TI";
        case 0x000F: return "Broadcom";
        case 0x001D: return "Qualcomm";
        case 0x0059: return "Nordic";
        case 0x0075: return "Samsung";
        case 0x00E0: return "Google";
        case 0x010F: return "Xiaomi";
        case 0x0157: return "Huawei";
        case 0x02E5: return "Espressif";
        case 0x038F: return "Flipper";
        case 0x0171: return "Amazon";
        case 0x00D2: return "Dialog";
        case 0x0087: return "Garmin";
        case 0x02FF: return "Sonos";
        case 0x0310: return "Bose";
        case 0x0131: return "Fitbit";
        case 0x0046: return "Sony";
        case 0x01DA: return "LEGO";
        case 0x0154: return "Anker";
        case 0x00AB: return "JBL";
        case 0x0499: return "Ruuvi";
        case 0x0822: return "Govee";
        case 0x0672: return "SwitchBot";
        default: break;
    }
    return "?";
}

// ==[ CLASS OF DEVICE → LABEL ]== BT Classic major device class (CoD bits 12:8)
static const char* classOfDeviceLabelInternal(uint32_t cod) {
    if (cod == 0) return nullptr;
    uint8_t major = (cod >> 8) & 0x1F;
    switch (major) {
        case 1: return "PC";
        case 2: return "PHONE";
        case 3: return "LAN/AP";
        case 4: return "AUDIO";
        case 5: return "PERIPH";
        case 6: return "CAMERA";
        case 7: return "PRINTER";
        case 8: return "TOY";
        case 9: return "HEALTH";
        default: return nullptr;
    }
}

static bool shouldStartSentinelSniff(uint32_t now) {
    if (!Config::getIppEnabled()) return false;
    uint32_t sentinelInterval = currentSentinelIntervalMs();
    if (sentinelInterval == 0) return false;
    if (state != ScanState::SLEEPING) return false;
    if (lastSentinelSniff != 0 && (now - lastSentinelSniff < sentinelInterval)) return false;
    if (lastWifiScan == 0) return false;
    if (now - lastWifiScan >= currentWifiScanIntervalMs()) return false;

    uint32_t dueIn = currentWifiScanIntervalMs() - (now - lastWifiScan);
    if (dueIn <= 3000) return false;  // a full scan is close enough

    return true;
}

static uint8_t pickSentinelChannel() {
    if (isDeauthActive() && deauthLastDominantChan > 0) {
        return deauthLastDominantChan;
    }

    // build dynamic channel pool: {1,6,11} baseline + channels with known APs
    // interleave: odd rounds pick from baseline, even rounds pick AP channels
    static constexpr uint8_t SENTINEL_BASE_COUNT = sizeof(SENTINEL_CHANNELS) / sizeof(SENTINEL_CHANNELS[0]);
    static uint8_t apChannelIdx = 0;

    bool useApChannel = (sentinelChannelIdx & 1) && wifiAPCount > 0;
    if (useApChannel) {
        // collect unique AP channels not already in baseline
        uint8_t apChannels[13];
        uint8_t apChannelCount = 0;
        for (int i = 0; i < wifiAPCount && apChannelCount < 13; i++) {
            uint8_t ch = wifiSnapshot[i].channel;
            if (ch < 1 || ch > 13) continue;
            if (ch == 1 || ch == 6 || ch == 11) continue;  // skip baseline
            bool dup = false;
            for (uint8_t j = 0; j < apChannelCount; j++) {
                if (apChannels[j] == ch) { dup = true; break; }
            }
            if (!dup) apChannels[apChannelCount++] = ch;
        }
        if (apChannelCount > 0) {
            uint8_t ch = apChannels[apChannelIdx % apChannelCount];
            apChannelIdx = (apChannelIdx + 1) % apChannelCount;
            sentinelChannelIdx++;
            return ch;
        }
        // no non-baseline AP channels — fall through to baseline
    }

    uint8_t ch = SENTINEL_CHANNELS[sentinelChannelIdx % SENTINEL_BASE_COUNT];
    sentinelChannelIdx++;
    return ch;
}

// ==[ INTERNAL: STATE MANAGEMENT ]==
// Validate state transitions to prevent radio conflicts. Every legal transition
// is listed explicitly — an unlisted transition is a bug, not a feature.
static bool canTransition(ScanState from, ScanState to) {
    switch (from) {
        case ScanState::SLEEPING:
            return to == ScanState::BLE_SCANNING ||
                   to == ScanState::WIFI_SCANNING ||
                   to == ScanState::DEAUTH_SNIFF ||
                   to == ScanState::PARASITIC ||
                   to == ScanState::SUSPENDED;
        case ScanState::BLE_SCANNING:
            return to == ScanState::SLEEPING ||
                   to == ScanState::SUSPENDED;
        case ScanState::WIFI_SCANNING:
            return to == ScanState::WIFI_PROCESSING ||
                   to == ScanState::SLEEPING ||     // scan fail/timeout/IPP-off
                   to == ScanState::SUSPENDED;
        case ScanState::WIFI_PROCESSING:
            return to == ScanState::DEAUTH_SNIFF ||
                   to == ScanState::SLEEPING ||     // forceBlePriority interrupts
                   to == ScanState::SUSPENDED;
        case ScanState::DEAUTH_SNIFF:
            return to == ScanState::HOGWASH_HOLD ||
                   to == ScanState::SLEEPING ||
                   to == ScanState::SUSPENDED;
        case ScanState::HOGWASH_HOLD:
            return to == ScanState::SLEEPING ||
                   to == ScanState::SUSPENDED;
        case ScanState::PARASITIC:
            return to == ScanState::SLEEPING ||     // resume() exits parasitic
                   to == ScanState::SUSPENDED;
        case ScanState::SUSPENDED:
            return to == ScanState::SLEEPING ||     // resume()
                   to == ScanState::PARASITIC;      // enterParasitic()
        default:
            return false;
    }
}

static void setState(ScanState s) {
    if (state != s && !canTransition(state, s)) {
        HAMLET_LOGF("[RECON] BUG: invalid transition %d->%d\n", (int)state, (int)s);
        return;  // refuse invalid transition — protect radio coex
    }
    state = s;
    stateEnteredAt = millis();
}

// ==[ INTERNAL: FNV-1a HASH ]==
uint32_t fnvHash(const void* data, int len) {
    uint32_t h = 0x811c9dc5;
    const uint8_t* p = (const uint8_t*)data;
    for (int i = 0; i < len; i++) h = (h ^ p[i]) * 0x01000193;
    return h;
}

// ==[ FORENSIC EXPORT IMPLEMENTATION ]==
void setForensicExportEnabled(bool on) { forensicExportEnabled = on; }
bool isForensicExportEnabled() { return forensicExportEnabled; }

#ifndef SIMULATOR
void exportForensicLog() {
    if (!SDStorage::isAvailable() || !forensicLog) return;
    char path[40];
    snprintf(path, sizeof(path), "/recon/flog_%lu.jsonl", (unsigned long)(millis() / 1000));
    if (!SDStorage::beginWriteStream(path, true)) {
        HAMLET_LOGLN("[RECON] SD write failed");
        return;
    }

    uint8_t count = forensicLogCount;
    uint8_t head = forensicLogHead;
    for (uint8_t i = 0; i < count; i++) {
        int idx = ((int)head - count + i + MAX_FORENSIC_LOG) % MAX_FORENSIC_LOG;
        const ForensicLogEntry& e = forensicLog[idx];
        char line[128];
        int lineLen = snprintf(line, sizeof(line),
            "{\"t\":%lu,\"ev\":%d,\"ch\":%d,\"rssi\":%d,\"bssid\":\"%02X%02X%02X%02X%02X%02X\",\"flags\":%d}\n",
            (unsigned long)e.timestamp, (int)e.event, e.channel, e.rssi,
            e.bssid[0], e.bssid[1], e.bssid[2], e.bssid[3], e.bssid[4], e.bssid[5],
            e.indicatorFlags);
        if (lineLen > 0) {
            SDStorage::writeStream((const uint8_t*)line, (size_t)lineLen);
        }
    }
    SDStorage::endWriteStream();
    HAMLET_LOGF("[RECON] Forensic log exported: %s (%d entries)\n", path, count);
}

void exportSessionSummary() {
    if (!SDStorage::isAvailable()) return;
    pruneProbeVulnCache(millis());

    char path[48];
    snprintf(path, sizeof(path), "/recon/session_%lu.json", (unsigned long)(millis() / 1000));
    if (!SDStorage::beginWriteStream(path, true)) return;

    auto ws = [](const char* s) {
        if (s) SDStorage::writeStream((const uint8_t*)s, strlen(s));
    };

    // Long JSON keys plus a 10-digit counter do not fit in 24 bytes. Keep the
    // scratch buffer comfortably above the longest bounded numeric member so
    // session receipts remain valid instead of silently losing punctuation.
    char numBuf[48];
    ws("{\n");
    snprintf(numBuf, sizeof(numBuf), "  \"uptimeMs\": %lu,\n", (unsigned long)millis());
    ws(numBuf);
    snprintf(numBuf, sizeof(numBuf), "  \"totalDeauths\": %lu,\n", (unsigned long)getDeauthCount());
    ws(numBuf);
    snprintf(numBuf, sizeof(numBuf), "  \"deauthBursts\": %d,\n", deauthHistoryCount);
    ws(numBuf);
    ws(isEvilTwinActive() ? "  \"evilTwinActive\": true,\n" : "  \"evilTwinActive\": false,\n");
    ws(isKarmaActive() ? "  \"karmaActive\": true,\n" : "  \"karmaActive\": false,\n");
    ws(karmaConfirmed ? "  \"karmaConfirmed\": true,\n" : "  \"karmaConfirmed\": false,\n");
    ws(canaryTripped ? "  \"canaryTripped\": true,\n" : "  \"canaryTripped\": false,\n");
    snprintf(numBuf, sizeof(numBuf), "  \"totalBLEDevices\": %d,\n", (int)totalBLEDevices);
    ws(numBuf);
    snprintf(numBuf, sizeof(numBuf), "  \"trackersFollowing\": %d,\n", followingCount);
    ws(numBuf);
    snprintf(numBuf, sizeof(numBuf), "  \"hostileClients\": %d,\n", clientFingerprintCount);
    ws(numBuf);
    snprintf(numBuf, sizeof(numBuf), "  \"forensicEvents\": %d,\n", forensicLogCount);
    ws(numBuf);
    char toolBuf[64];
    snprintf(toolBuf, sizeof(toolBuf), "  \"lastTool\": \"%s\",\n", deauthToolLabel(lastDeauthTool));
    ws(toolBuf);
    snprintf(numBuf, sizeof(numBuf), "  \"wifiAPCount\": %d,\n", wifiAPCount);
    ws(numBuf);
    snprintf(numBuf, sizeof(numBuf), "  \"vulnProbeClients\": %d,\n", probeVulnCount);
    ws(numBuf);
    ws("  \"vulnProbes\": [");
    for (uint8_t v = 0; v < probeVulnCount; v++) {
        const ProbeVulnMatch& m = probeVulnCache[v];
        char probeLine[128];
        snprintf(probeLine, sizeof(probeLine),
            "%s\n    {\"ssid\":\"%s\",\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\",\"rssi\":%d}",
            v > 0 ? "," : "",
            m.ssid,
            m.clientMac[0], m.clientMac[1], m.clientMac[2],
            m.clientMac[3], m.clientMac[4], m.clientMac[5],
            m.rssi);
        ws(probeLine);
    }
    ws("\n  ]\n");
    ws("}\n");
    SDStorage::endWriteStream();
    HAMLET_LOGF("[RECON] Session summary exported: %s\n", path);
}
#else
void exportForensicLog() {}
void exportSessionSummary() {}
#endif

// ==[ INCLUDED SUBSYSTEMS ]== split for maintainability
// All three .inl files share the static state defined above.
// Order matters: forensic first (used by deauth/wifi), deauth second (used by wifi), wifi last.
#include "recon_forensic.inl"
#include "recon_deauth.inl"
#include "recon_wifi.inl"

uint8_t getVulnProbeCount() {
    pruneProbeVulnCache(millis());
    return probeVulnCount;
}

const ProbeVulnMatch* getVulnProbeCache() {
    pruneProbeVulnCache(millis());
    return probeVulnCache;
}

uint8_t getOfflineScanCount() { return offlineScanCount; }
void clearOfflineScanCount()  { offlineScanCount = 0; }

// ==[ C5MONSTER 5GHz FEED ]==

void feedC5MonsterScan(const C5Monster::ScanResults& results) {
    if (!wifiSnapshot) return;

    auto c5AuthToWifiAuth = [](uint8_t authType) -> uint8_t {
        switch (authType) {
            case C5Protocol::AUTH_OPEN:
                return (uint8_t)WIFI_AUTH_OPEN;
            case C5Protocol::AUTH_WEP:
                return (uint8_t)WIFI_AUTH_WEP;
            case C5Protocol::AUTH_WPA:
                return (uint8_t)WIFI_AUTH_WPA_PSK;
            case C5Protocol::AUTH_WPA2:
                return (uint8_t)WIFI_AUTH_WPA2_PSK;
            case C5Protocol::AUTH_WPA3:
                return (uint8_t)WIFI_AUTH_WPA3_PSK;
            case C5Protocol::AUTH_WPA_WPA2_MIXED:
                return (uint8_t)WIFI_AUTH_WPA_WPA2_PSK;
            case C5Protocol::AUTH_WPA2_WPA3_MIXED:
                return (uint8_t)WIFI_AUTH_WPA2_WPA3_PSK;
            default:
                return (uint8_t)WIFI_AUTH_MAX;
        }
    };

    // Preserve native scan/parasitic 2.4GHz entries, replace any prior 5GHz
    // rows with the current C5Monster sweep to avoid stale dual-band intel.
    int nextCount = 0;
    for (int i = 0; i < wifiAPCount && nextCount < MAX_WIFI_SNAPSHOT; i++) {
        if (wifiSnapshot[i].channel > 14) continue;
        if (i != nextCount) wifiSnapshot[nextCount] = wifiSnapshot[i];
        nextCount++;
    }
    wifiAPCount = nextCount;

    openAPCount = 0;
    knownAPCount = 0;

    for (uint8_t i = 0; i < results.count; i++) {
        const auto& entry = results.entries[i];
        if (!entry.is5GHz || entry.channel < 36 || entry.channel > 165) continue;

        int slot = -1;
        for (int j = 0; j < wifiAPCount; j++) {
            if (memcmp(wifiSnapshot[j].bssid, entry.bssid, 6) == 0) {
                slot = j;
                break;
            }
        }

        if (slot < 0) {
            if (wifiAPCount >= MAX_WIFI_SNAPSHOT) break;
            slot = wifiAPCount++;
        }

        WifiAP& ap = wifiSnapshot[slot];
        memset(&ap, 0, sizeof(ap));
        strncpy(ap.ssid, entry.ssid, 32);
        ap.ssid[32] = '\0';
        memcpy(ap.bssid, entry.bssid, 6);
        ap.rssi = entry.rssi;
        ap.channel = entry.channel;
        ap.authMode = c5AuthToWifiAuth(entry.authType);

        if (entry.hasGPS &&
            entry.latitude >= -90.0 && entry.latitude <= 90.0 &&
            entry.longitude >= -180.0 && entry.longitude <= 180.0) {
            ap.lat = (float)entry.latitude;
            ap.lon = (float)entry.longitude;
        } else if (GPS::hasFix()) {
            ap.lat = (float)GPS::getLatitude();
            ap.lon = (float)GPS::getLongitude();
        } else {
            ap.lat = 0.0f;
            ap.lon = 0.0f;
        }
    }

    for (int i = 0; i < wifiAPCount; i++) {
        if (wifiSnapshot[i].authMode == (uint8_t)WIFI_AUTH_OPEN ||
            wifiSnapshot[i].authMode == (uint8_t)WIFI_AUTH_WEP) {
            openAPCount++;
        }
    }

    if (wifiAPCount > 0) {
        knownAPCount = 0;
        checkEvilTwin();
        checkKarmaHoneypot();
        checkPotfileMatches();
        checkOpenAPs();
    }
}

}  // namespace Recon
