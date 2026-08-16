/**
 * Recon — Interdimensional Pig Pen (IPP) Defensive Awareness Engine
 *
 * ==[ BACKGROUND INTEL ]== passive WiFi + BLE scanning during IDLE/MENU.
 * WiFi: evil twins, KARMA honeypots, potfile matches, open network warnings.
 * BLE: AirTag, SmartTag, Tile, FastPair detection. Flipper spam detection.
 * tracker following: payload hash survives MAC rotation, 15min+3cycle = FOLLOWING.
 * parasitic mode: zero-radio analysis from Hunt's beacon table.
 *
 * gated by IPP master + per-radio BLE/WiFi switches.
 * suspend() on HUNT/SPECTRUM/WEBCONFIG/foreground radio entry.
 * PSRAM-backed snapshot arrays.
 */

#pragma once

#include <stdint.h>
#include <esp_wifi_types.h>
#include "defense_contracts.h"

namespace Recon {

// ==[ THREAT CLASSIFICATION ]==
using ThreatType = Defense::ThreatType;
static constexpr int THREAT_TYPE_COUNT = (int)ThreatType::_COUNT;

// ==[ SPAM PLATFORM ]== which OS/ecosystem a spam attack targets
using SpamPlatform = Defense::SpamPlatform;

// ==[ DEAUTH TOOL CLASSIFICATION ]== behavioral signature of attack tool
using DeauthTool = Defense::DeauthTool;

using DeauthSourceOrigin = Defense::DeauthSourceOrigin;

using CadenceTier = Defense::CadenceTier;

// ==[ TRACKER TABLE ENTRY ]== enriched BLE metadata for scanner/detail views
struct TrackerEntry {
    uint8_t  mac[6];           // last known MAC
    uint8_t  payloadHash[4];   // survives MAC rotation
    ThreatType type;           // classification
    int8_t   rssi;             // strongest seen this cycle
    int8_t   rssiSmooth;       // EMA-smoothed
    int8_t   txPower;          // AD type 0x0A / frame-specific TX hint when present
    uint8_t  seenCount;        // scan cycles detected in
    uint8_t  flags;            // bit0:alerted bit1:following bit2:spam
    uint8_t  advFlags;         // AD flags (0x01)
    uint8_t  advType;          // NimBLE advertisement type
    uint8_t  addrType;         // public/random/etc
    uint8_t  frameType;        // Apple/Eddystone subtype when known
    uint16_t companyId;        // manufacturer ID if present
    uint16_t primaryService;   // first/primary 16-bit service UUID when present
    uint16_t appearance;       // GAP appearance (0x19) — device class (phone, watch, beacon, etc.)
    uint16_t major;            // iBeacon major or aux field
    uint16_t minor;            // iBeacon minor or aux field
    uint32_t firstSeen;        // millis timestamp
    uint32_t lastSeen;
    uint32_t lastMacChange;    // rotation tracking
    uint8_t  macChangeCount;   // mac rotations observed
    uint8_t  identityCandidates; // distinct MACs sharing this identity this scan
    uint8_t  spamPlatform;    // SpamPlatform enum — which OS targeted (when FLAG_SPAM)
    uint16_t firstDetectDist; // pedometer distance (m) at first detection — step-distance following
    uint8_t  payloadLen;       // total adv payload bytes observed
    uint8_t  serviceCount;     // 16-bit service UUIDs / service-data hits parsed
    uint8_t  manufacturerCount;// manufacturer data blocks parsed
    uint8_t  payloadPreviewLen;// bytes copied into payloadPreview
    uint32_t classOfDevice;    // AD 0x0D — BT Classic CoD bits (phone/laptop/wearable/audio)
    uint16_t advInterval;      // AD 0x1A — advertised interval (×0.625ms units). Anti-spoof fingerprint.
    uint16_t measuredAdvIntervalMs; // actual measured delta between consecutive ads (ms). Anti-spoof.
    uint32_t lastAdvTimestamp; // millis() of last advertisement (for delta calc)
    uint16_t companyId2;       // second manufacturer data company ID (multi-block detection)
    uint16_t intervalVariance; // adv interval jitter variance — relay detection (0=unknown)
    float    lastLat;          // GPS lat at last scan (0 = no fix ever)
    float    lastLon;          // GPS lon at last scan (0 = no fix ever)
    char     name[16];         // best-effort truncated local name
    uint8_t  payloadPreview[16]; // leading advertisement bytes for hex inspector
};

// ==[ GATT DEVICE INFO ]== results from GATT service enumeration
struct GattDeviceInfo {
    char manufacturer[24];   // DIS 0x2A29
    char model[24];          // DIS 0x2A24
    char firmware[16];       // DIS 0x2A26
    char serial[20];         // DIS 0x2A25
    int8_t batteryLevel;     // Battery 0x2A19 (-1 = unknown)
    uint8_t serviceCount;    // total GATT services discovered
    bool valid;              // true if connect + discovery succeeded
};

// flag bits
static constexpr uint8_t FLAG_ALERTED        = 0x01;
static constexpr uint8_t FLAG_FOLLOWING      = 0x02;
static constexpr uint8_t FLAG_SPAM           = 0x04;
static constexpr uint8_t FLAG_STEP_FOLLOWING = 0x08;  // seen across 800m+ of movement
static constexpr uint8_t FLAG_WATCHLISTED    = 0x10;  // on user watchlist (eviction-protected)
static constexpr uint8_t FLAG_RELAY_SUSPECT  = 0x20;  // BLE relay/replay detected (jitter anomaly)

// ==[ RECON EVENTS ]== emitted to mood/narrator/toast
using ReconEvent = Defense::DefenseEvent;

// ==[ EVENT DATA ]== context for event handlers
//
// Field validity per ReconEvent (Y = populated, - = zero/unused):
//
// Event                 | threatType | rssi | channel | count | bssid | ssid | detail
// ----------------------|------------|------|---------|-------|-------|------|--------
// TRACKER_NEW           |     Y      |  Y   |    -    |   -   |   -   |  -   | device label
// TRACKER_FOLLOWING     |     Y      |  Y   |    -    |   Y   |   -   |  -   | device label
// BLE_SPAM              |     Y      |  Y   |    -    |   -   |   -   |  -   | platform label
// EVIL_TWIN             |     -      |  Y   |   Y     |   Y   |   Y   |  Y   | BSSID fragment
// KARMA_HONEYPOT        |     -      |  Y   |   Y     |   -   |   Y   |  Y   | SSID list fragment
// FINGERPRINT_MISMATCH  |     -      |  -   |   Y     |   -   |   Y   |  -   | hash info
// SEQ_ANOMALY           |     -      |  -   |   Y     |   -   |   Y   |  -   | gap info
// RSSI_ANOMALY          |     -      |  Y   |   Y     |   -   |   Y   |  -   | delta info
// KNOWN_AP              |     -      |  -   |    -    |   Y   |   -   |  Y   | SSID list
// OPEN_AP_WARNING       |     -      |  -   |    -    |   Y   |   -   |  -   | open AP info
// PROBE_VULN_CLIENT     |     -      |  -   |    -    |   -   |   -   |  -   | SSID probed
// DEAUTH_DETECTED       |     -      |  -   |   Y     |   Y   |   -   |  -   | frame count
// SCAN_COMPLETE         |     -      |  -   |    -    |   Y   |   -   |  -   | open/known counts
// COORDINATED_ATTACK    |     Y      |  -   |    -    |   -   |   -   |  -   | correlation info
// ATTACKER_IDENTIFIED   |     Y      |  -   |    -    |   -   |   -   |  -   | device + channel
// DUAL_BAND_STALK       |     Y      |  -   |    -    |   -   |   -   |  -   | correlation info
// FOLLOWING_NETWORK_ID  |     Y      |  -   |    -    |   -   |   -   |  Y   | SSID + device
// WATCHLIST_ENTER       |     Y      |  Y   |    -    |   -   |   -   |  -   | device name
// WATCHLIST_EXIT        |     Y      |  -   |    -    |   -   |   -   |  -   | device name
// KARMA_CONFIRMED       |     -      |  -   |    -    |   -   |   -   |  Y   | bait SSID
// CANARY_TRIPPED        |     -      |  -   |    -    |   -   |   -   |  Y   | canary SSID
// RELAY_SUSPECT         |     Y      |  Y   |    -    |   -   |   -   |  -   | variance info
// HOSTILE_CLIENT        |     -      |  Y   |   Y     |   -   |   -   |  -   | fingerprint label
// TOOL_IDENTIFIED       |     -      |  -   |   Y     |   -   |   -   |  -   | tool name
// LOW_ENTROPY_BEACON    |     -      |  Y   |   Y     |   -   |   Y   |  Y   | entropy score
//
using ReconEventData = Defense::DefenseEventData;

// ==[ WIFI SNAPSHOT ]== cached from last wifi scan
struct WifiAP {
    char     ssid[33];
    uint8_t  bssid[6];
    int8_t   rssi;
    uint8_t  channel;
    uint8_t  authMode;      // wifi_auth_mode_t cast
    uint8_t  entropyScore;  // beacon IE diversity 0-100 (higher=more legitimate, 0=not scored)
    float    lat;           // GPS lat at scan time (0 = no fix)
    float    lon;           // GPS lon at scan time (0 = no fix)
};

// ==[ WIFI PIPELINE HEALTH ]== cheap draw-path snapshot for DEFHOG4/status UI
enum class WifiPipelineState : uint8_t {
    OFF,            // IPP or WiFi scan toggle is off
    SUSPENDED,      // another mode owns the radio
    SUPPRESSED,     // BLE scanner / wardrive owns the cadence
    PARASITIC,      // Hunt owns radio; Recon consumes its beacon table
    WAITING,        // idle until next scheduled sweep
    SCANNING,       // Arduino async scan in flight
    PROCESSING,     // completed results are being committed
    READY,          // last sweep completed successfully
    FAILED,         // last sweep attempt failed; prior snapshot is retained
};

struct WifiPipelineStatus {
    WifiPipelineState state;
    int16_t  lastResult;           // AP count, WIFI_SCAN_FAILED, or -3 timeout
    uint16_t consecutiveFailures;
    uint32_t lastAttemptMs;
    uint32_t lastCompleteMs;
    uint32_t nextDueMs;
};

static constexpr int MAX_TRACKERS       = (int)Defense::MAX_TRACKERS;
static constexpr int MAX_BLE_DEVICES    = (int)Defense::MAX_BLE_OBSERVATIONS;
static constexpr int MAX_WIFI_SNAPSHOT  = (int)Defense::MAX_WIFI_OBSERVATIONS;
static constexpr int MAX_EVENT_QUEUE    = (int)Defense::MAX_EVENT_QUEUE;
static constexpr int MAX_WATCHLIST     = 6;
static constexpr int WATCHLIST_LABEL_LEN = 16;

// ==[ WATCHLIST ENTRY ]== runtime presence state for named BLE devices
struct WatchlistEntry {
    uint8_t  payloadHash[4];
    char     label[WATCHLIST_LABEL_LEN];
    bool     occupied;
    bool     present;          // true = currently in range
    uint32_t lastSeen;         // millis of last detection
};

// ==[ FORENSIC EVENT LOG ]== evidence chain history for multi-indicator correlation
// Each entry records ALL active indicators at event time, not just the trigger.
// countIndicatorsForBSSID() scans for distinct indicator bits on same BSSID in window.
struct ForensicLogEntry {
    uint32_t timestamp;         // millis
    ReconEvent event;           // triggering event type
    uint8_t bssid[6];          // associated BSSID (if applicable)
    int8_t rssi;               // signal context
    uint8_t channel;           // channel context
    uint8_t indicatorFlags;    // ALL active indicators at event time
                               // bit0=fp bit1=seq bit2=rssi bit3=twin bit4=karma bit5=deauth
    uint8_t _pad[2];           // align to 16B
};
static constexpr int MAX_FORENSIC_LOG = 64;

// indicator flag bits for ForensicLogEntry::indicatorFlags
static constexpr uint8_t IND_FINGERPRINT = 0x01;
static constexpr uint8_t IND_SEQ_ANOMALY = 0x02;
static constexpr uint8_t IND_RSSI_ANOMALY = 0x04;
static constexpr uint8_t IND_EVIL_TWIN   = 0x08;
static constexpr uint8_t IND_KARMA       = 0x10;
static constexpr uint8_t IND_DEAUTH      = 0x20;
static constexpr uint8_t IND_BLE_ATTACK  = 0x40;  // BLE spam/Flipper active during WiFi attack

// ==[ DEAUTH BURST HISTORY ]== forensic record of recent deauth attacks
struct DeauthBurstRecord {
    uint32_t timestamp;        // millis when burst detected
    uint16_t frameCount;       // total frames in burst
    uint16_t pps;              // packets/sec estimate
    uint8_t dominantChannel;   // channel with most frames
    int8_t peakRSSI;           // strongest source signal
    uint8_t uniqueSources;     // distinct transmitter MACs
    uint8_t huntChannelMatch;  // was this on our Hunt channel? (0/1)
    uint8_t targetBssid[6];   // addr1: targeted BSSID (who's being attacked)
    uint16_t dominantReason;  // most-seen deauth reason code (1=unspec, 7=class3, etc.)
};
static constexpr int MAX_DEAUTH_HISTORY = 8;

// ==[ PROBE-POTFILE MATCH CACHE ]== clients probing for known-vulnerable SSIDs
struct ProbeVulnMatch {
    uint8_t clientMac[6];      // client that's vulnerable
    char ssid[33];             // potfile-matched SSID being probed
    int8_t rssi;               // last seen signal
    uint32_t lastSeen;         // millis
};
static constexpr int MAX_PROBE_VULN_CACHE = 16;

// ==[ CLIENT FINGERPRINT ]== passive probe request IE analysis
struct ClientFingerprint {
    uint8_t  clientMac[6];
    uint32_t fingerprintHash;    // FNV-1a of IE chain
    int8_t   rssi;
    uint8_t  channel;
    uint8_t  hostileScore;       // 0-100 threat level
    uint8_t  toolSignature;      // DeauthTool cast — suspected tool
    uint32_t firstSeen;
    uint32_t lastSeen;
    char     label[12];          // "Kali", "ESP32", "Phone", etc.
};
static constexpr int MAX_CLIENT_FINGERPRINTS = 16;

// ==[ TEMPORAL HEATMAP ]== forensic timeline buckets
struct HeatmapBucket {
    uint8_t deauthIntensity;     // 0-255 scaled
    uint8_t bleIntensity;        // 0-255 (tracker/spam events)
    uint8_t wifiIntensity;       // 0-255 (evil twin/karma/fingerprint)
    uint8_t indicatorFlags;      // union of IND_* bits in this bucket
};
static constexpr int HEATMAP_BUCKETS = 30;

// Mutable engine ownership and state readers are implementation-only. Firmware
// consumers use DefensePipeline commands and DefenseSnapshot const accessors.
#if defined(DEFENSE_PIPELINE_INTERNAL)

// ==[ ENGINE API ]==
void init();                    // call once at boot
void suspend(bool releaseBle);  // pause scanning; optionally tear down BLE
void resume();                  // reset timers, re-enable scanning
void enterParasitic();          // feed from Hunt's beacon table (zero radio cost)
void ingestWardriveSnapshot(const wifi_ap_record_t* records, uint16_t count);  // wardrive WiFi parasitic feed

// ==[ BLE PRIORITY MODE ]== force aggressive BLE-only scanning for BLE_SCANNER mode
void forceBlePriority();        // skip WiFi scans, max BLE cadence (10s/8s)
void unforceBlePriority();      // return to normal adaptive cadence

// ==[ WARDRIVE BLE MODE ]== keep BLE alive, WiFi suppressed, sweep-paced scans
void forceWardriveBle();          // suppress WiFi, pace BLE from wardrive sweep count
void unforceWardriveBle();        // return to normal adaptive cadence
void onWardriveSweepComplete();   // wardrive signals WiFi sweep done
// true = wardrive BLE window is active or due
bool wardriveWantsBleWindow();
bool consumeWardriveBleReady();   // returns true once when BLE scan results ready
void pinBleDevice(const uint8_t* payloadHash);   // protect tracked BLE device from catalog eviction
void clearPinnedBleDevice();    // clear BLE_SCANNER pin protection
void setActiveScan(bool active);  // toggle active BLE scan (sends SCAN_REQ probes)
bool isActiveScanEnabled();       // current active scan state
void toggleChaff();               // cycle chaff advertising on/off
bool isChaffActive();             // chaff beacons broadcasting

// ==[ BLE WATCHLIST ]== user-named device presence alerts (max 6)
bool addToWatchlist(const uint8_t* payloadHash, const char* label);
bool removeFromWatchlist(uint8_t slotIdx);           // 0-5
void updateWatchlistLabel(uint8_t slotIdx, const char* label);
uint8_t getWatchlistCount();
const WatchlistEntry* getWatchlist();                // array of MAX_WATCHLIST
int findWatchlistSlot(const uint8_t* payloadHash);   // -1 if not found
bool isWatchlisted(const uint8_t* payloadHash);
void pauseBLEScanForGATT();       // pause BLE scan, keep BLE init for GATT client use
void resumeBLEScanFromGATT();     // restart BLE scan after GATT operation

// ==[ STATE QUERIES ]==
bool isActive();                // currently scanning or sleeping between scans
bool isScanning();              // BLE or WiFi scan in progress right now
bool isBleAvailable();          // compile-time: BLE stack present in build
bool isBleInitialized();        // runtime: BLE controller currently active (coex constraint)
bool isParasiticMode();         // currently feeding from Hunt instead of scanning
bool requestWifiScan();         // queue a sweep without stealing radio ownership
WifiPipelineStatus getWifiPipelineStatus();

#endif  // DEFENSE_PIPELINE_INTERNAL

// Pure presentation helper; carries no mutable engine state.
const char* wifiPipelineStateLabel(WifiPipelineState state);

#if defined(DEFENSE_PIPELINE_INTERNAL)

CadenceTier getCadenceTier();   // current adaptive recon cadence
void setForcedCadence(CadenceTier tier);  // override cadence (DEFHOG4 full-screen)
void clearForcedCadence();                // restore threat-adaptive cadence
uint32_t getCurrentWifiScanIntervalMs();
uint32_t getCurrentDeauthSniffMs();
uint32_t getCurrentParasiticIntervalMs();
uint32_t getCurrentSentinelIntervalMs();

int  getTrackerCount();         // total unique trackers seen this session
int  getFollowingCount();       // trackers flagged as following
int  getSpamCount();            // BLE spam sources detected
bool hasThreats();              // any active threats

const TrackerEntry* getTrackers();     // raw table access
int  getTrackerTableSize();            // entries in use
const TrackerEntry* getBleDevices();   // full BLE catalog for scanner/detail view
int  getBleDeviceTableSize();          // entries in BLE catalog

// wifi snapshot
int  getLastWifiAPCount();
int  getOpenAPCount();
int  getKnownAPCount();
uint16_t getKnownProbeRequestCount();    // recent probe requests matching known SSIDs
uint16_t getKnownProbeClientCount();     // unique client MACs in recent known-probe intel
const WifiAP* getWifiSnapshot();
int  getWifiSnapshotCount();

// ==[ EVENT QUEUE ]==
bool hasEvent();
ReconEventData popEvent();
void ingestDeauthObservation(uint8_t channel, int8_t rssi, uint8_t subtype,
                             const uint8_t* srcMac, DeauthSourceOrigin origin,
                             const uint8_t* targetBssid = nullptr, uint16_t reasonCode = 0);
// Hunt probe-response KARMA: same BSSID claims 3+ SSIDs in probe responses
void reportKarmaFromProbeResponse(const char* ssid, const char* detail);

// ==[ DEAUTH DETECTION ]== passive promiscuous sniff between scans
uint32_t getDeauthCount();             // total deauth frames detected this session
uint16_t getLastDeauthBurstCount();    // frames in last confirmed deauth burst
uint8_t  getLastDeauthUniqueSources(); // unique transmitter MACs in last burst
int8_t   getLastDeauthRSSI();          // strongest deauth RSSI from last confirmed burst
uint8_t  getLastDeauthChannel();       // channel of strongest from last confirmed burst
uint8_t  getLastDeauthDominantChannel(); // channel with most deauth/disassoc frames in last burst
uint16_t getLastDeauthPPS();             // packets/sec estimate for last burst window
uint16_t getLastDeauthSubtypeCount();    // subtype 0x0C frames in last burst
uint16_t getLastDisassocSubtypeCount();  // subtype 0x0A frames in last burst
uint32_t getLastDeauthTime();          // millis of last deauth detection (0 = never)
bool     isDeauthActive();             // detected deauths in recent sniff cycle

// ==[ WIFI THREAT RECENCY ]==
bool isEvilTwinActive();               // recent evil-twin detection
bool isKarmaActive();                  // recent KARMA detection
bool isFingerprintMismatchActive();    // recent fingerprint anomaly
bool isSeqAnomalyActive();             // recent sequence anomaly
bool isRssiAnomalyActive();            // recent RSSI anomaly
uint8_t getRecentFingerprintMismatchCount();
uint8_t getRecentSeqAnomalyCount();
uint8_t getRecentRssiAnomalyCount();

// ==[ SCAN STATS ]== for narrator
uint32_t getLastBLEScanTime();         // millis of last BLE scan
uint32_t getLastWifiScanTime();        // millis of last WiFi scan
uint32_t getTimeSinceLastScan();       // ms since any scan completed
uint16_t getTotalBLEDevicesSeen();     // lifetime BLE count this session
uint16_t getAppleContinuityCount();   // Nearby+AirDrop+Handoff+Hotspot last scan

#endif  // DEFENSE_PIPELINE_INTERNAL

// Pure metadata/math helpers shared by snapshot consumers.
const char* proximityLabel(int8_t rssi);  // "CLOSE"/"NEAR"/"FAR"/"EDGE"
float estimateDistance(int8_t rssi, int8_t txPower);  // log-distance model, meters (-1 = no txPower)
const char* appearanceLabel(uint16_t appearance);     // GAP appearance → human label
const char* manufacturerLabel(uint16_t companyId);    // company ID → vendor name
const char* classOfDeviceLabel(uint32_t cod);          // BT Classic CoD major class → label
const char* deviceLabel(const TrackerEntry& te);       // smart: appearance→name→CoD→threat
const char* threatTypeLabel(ThreatType type);           // raw ThreatType→4-char label

#if defined(DEFENSE_PIPELINE_INTERNAL)

uint32_t getCurrentBLEScanIntervalMs();   // tier-adaptive
uint32_t getCurrentBLEScanDurationMs();   // tier-adaptive

// ==[ FORENSIC EVENT LOG ]== evidence chain queries
uint8_t getForensicLogCount();
uint8_t getForensicLogHead();             // ring buffer head index
const ForensicLogEntry* getForensicLog(); // raw ring buffer (MAX_FORENSIC_LOG entries)
// Count distinct indicator types for a BSSID within a time window.
// Returns popcount of union of all indicatorFlags for matching entries.
uint8_t countIndicatorsForBSSID(const uint8_t* bssid, uint32_t windowMs);

// ==[ DEAUTH BURST HISTORY ]==
uint8_t getDeauthBurstHistoryCount();
const DeauthBurstRecord* getDeauthBurstHistory();  // ring buffer (MAX_DEAUTH_HISTORY entries)

// ==[ PROBE-POTFILE MATCH CACHE ]==
void cacheProbeVulnMatch(const uint8_t* clientMac, const char* ssid, int8_t rssi);
uint8_t getVulnProbeCount();
const ProbeVulnMatch* getVulnProbeCache();

// ==[ IDLE PULL ]== BLE scan cycles that ran while IDLE (Zeigarnik open loops on return)
uint8_t getOfflineScanCount();   // accumulated BLE scan cycles while IDLE
void clearOfflineScanCount();    // consume and reset (call when returning to IDLE)

// ==[ GHOST NETWORK CANARY (#7) ]== detect targeted probe replay attacks
const char* getCanarySSID();     // unique per-device canary SSID
void setCanarySSID(const char* ssid);  // set custom canary (NULL/empty resets to auto)
bool isCanaryTripped();          // someone broadcast our canary

// ==[ PHANTOM PROBE KARMA CONFIRMATION (#1) ]== active KARMA verification
bool isKarmaConfirmed();         // phantom probe was echoed back

// ==[ DEAUTH TOOL CLASSIFICATION (#6) ]== behavioral fingerprinting of attack tools
DeauthTool getLastDeauthTool();            // tool classification from last burst

#endif  // DEFENSE_PIPELINE_INTERNAL

const char* deauthToolLabel(DeauthTool tool); // human-readable tool label

#if defined(DEFENSE_PIPELINE_INTERNAL)

// ==[ CLIENT FINGERPRINTING (#5) ]== passive probe IE analysis
uint8_t getClientFingerprintCount();
const ClientFingerprint* getClientFingerprints();

// ==[ FORENSIC EXPORT ]== SD card evidence persistence
void exportForensicLog();                 // dump full forensic log to SD as JSONL
void exportSessionSummary();              // write session summary JSON on suspend
void setForensicExportEnabled(bool on);   // enable/disable live SD logging
bool isForensicExportEnabled();

#endif  // DEFENSE_PIPELINE_INTERNAL

}  // namespace Recon

// ==[ C5MONSTER 5GHz FEED ]== (outside Recon namespace — C5Monster is global)
namespace C5Monster { struct ScanResults; }
namespace Recon {
#if defined(DEFENSE_PIPELINE_INTERNAL)
void feedC5MonsterScan(const C5Monster::ScanResults& results);
#endif
}
