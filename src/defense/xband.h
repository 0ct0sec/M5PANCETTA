/**
 * XBand — Cross-Domain WiFi + BLE Forensic Intelligence
 *
 * ==[ FUSION ENGINE ]== correlates WiFi 2.4GHz and BLE observations
 * to produce intelligence neither band could generate alone.
 *
 * 6 features:
 *  1. Attacker platform fingerprint (WHO is running WiFi attacks?)
 *  2. Dual-band stalker detection (BLE following + WiFi probe persistence)
 *  3. Device cohort correlation (same person across both bands)
 *  4. Following tracker → owner's WiFi network
 *  5. Crowd density & composition
 *  6. Vendor ecosystem cross-reference
 *
 * All processing in main loop via update(). ~1KB PSRAM + ~150B DRAM.
 */

#pragma once

#include <stdint.h>

namespace XBand {

// ==[ ATTACKER PROFILE ]== BLE device fingerprinted as WiFi attacker source
struct AttackerProfile {
    uint8_t  blePayloadHash[4]; // BLE identity (survives MAC rotation)
    int8_t   bleRssi;           // BLE signal at correlation time
    int8_t   wifiDeauthRssi;    // deauth burst peak RSSI
    uint8_t  bleType;           // Recon::ThreatType
    uint8_t  correlatedBursts;  // deauth bursts matched to this device
    uint16_t companyId;         // BLE manufacturer
    float    estimatedDist;     // meters from txPower model (-1 = unknown)
    uint32_t firstCorrelated;   // millis
    uint32_t lastCorrelated;
    uint32_t lastBurstTimestamp;// source burst already counted (dedup)
    uint8_t  targetBssid[6];    // latest attacked BSSID, if observed
    char     bleName[16];       // device name if available
};
static constexpr int MAX_ATTACKER_PROFILES = 4;

// ==[ PERSISTENT PROBE CLIENT ]== WiFi client seen >10min (non-randomized MAC)
struct PersistentProbeClient {
    uint8_t  mac[6];
    uint32_t firstSeen;
    uint32_t lastSeen;
    int8_t   rssiSmooth;
    uint8_t  probeCount;       // unique SSIDs probed
    uint16_t firstDetectDist;  // pedometer meters at first detection
};
static constexpr int MAX_PERSISTENT_CLIENTS = 8;

// ==[ COHORT PAIR ]== BLE device + WiFi client correlated to same person
struct CohortPair {
    uint8_t  wifiMac[6];       // WiFi client MAC (last 3 octets for display)
    uint8_t  blePayloadHash[4];// BLE device identity
    int8_t   wifiRssi;
    int8_t   bleRssi;
    uint8_t  confidence;       // 1=LOW 2=MED 3=HIGH
    uint16_t timeDeltaSec;     // BLE/WiFi observation separation
    uint8_t  bleType;          // Recon::ThreatType
    uint16_t companyId;        // BLE manufacturer
    uint32_t lastCorrelated;   // millis
    char     probeSSID[17];    // truncated best-match SSID
    bool     isFollowing;      // BLE device has FLAG_FOLLOWING
    bool     potfileMatch;     // probed SSID in potfile
};
static constexpr int MAX_COHORT_PAIRS = 16;

// ==[ CROWD SNAPSHOT ]== environmental population estimate
struct CrowdSnapshot {
    uint8_t  blePhones;
    uint8_t  bleWatches;
    uint8_t  bleTags;
    uint8_t  bleSensors;
    uint8_t  bleOther;
    uint8_t  wifiClients;
    uint8_t  wifiAPs;
    uint8_t  appleContinuity;
    uint16_t estimatedPop;
    uint32_t timestamp;
};
static constexpr int CROWD_RING_SIZE = 6;

// ==[ VENDOR CORRELATION ]== BLE vendor histogram matched to WiFi AP OUIs
struct VendorCorrelation {
    uint16_t companyId;
    uint8_t  bleDeviceCount;
    uint8_t  wifiAPCount;
    int8_t   closestBleRssi;
    int8_t   closestWifiRssi;
    bool     hasIoT;           // Espressif/SwitchBot/etc near weak-auth AP
    uint8_t  _pad;
};
static constexpr int MAX_VENDOR_CORRELATIONS = 8;

// ==[ CROWD TREND ]==
enum class CrowdTrend : uint8_t {
    UNKNOWN = 0,
    GROWING,
    STABLE,
    SHRINKING
};

// ==[ CROWD TIER ]== environmental density classification
enum class CrowdTier : uint8_t {
    DESERTED = 0,  // <5 estimated population
    SPARSE,        // 5-14
    BUSY,          // 15-39
    CROWDED,       // 40-99
    PACKED         // 100+
};

// Engine ownership/readers are private to DefensePipeline and its sinks.
#if defined(DEFENSE_PIPELINE_INTERNAL)

// ==[ ENGINE API ]==
void init();                    // allocate PSRAM, owned by DefensePipeline
void update(uint32_t now);      // run correlations in DefensePipeline fusion stage

// ==[ FEATURE 1: ATTACKER FINGERPRINT ]==
int  getAttackerCount();
const AttackerProfile* getAttackerProfiles();
bool hasActiveAttacker();       // any profile with correlatedBursts > 0 in last 60s

// ==[ FEATURE 2: DUAL-BAND STALK ]==
bool isDualBandStalkActive();   // BLE following + WiFi probe persistence
int  getPersistentClientCount();

// ==[ FEATURE 3: COHORT CORRELATION ]==
int  getCohortCount();
const CohortPair* getCohortPairs();
int  getHighConfidenceCohortCount();

// ==[ FEATURE 4: FOLLOWING → NETWORK ]==
// Reuses CohortPair with isFollowing=true. Query via getCohortPairs().

// ==[ FEATURE 5: CROWD DENSITY ]==
const CrowdSnapshot* getCurrentCrowd();  // most recent snapshot (nullable)
CrowdTrend getCrowdTrend();
CrowdTier getCrowdTier();              // classified density from estimatedPop
uint16_t getEstimatedPopulation();
bool isDeserted();                      // shorthand: estimatedPop < 5
bool isCrowded();                       // shorthand: estimatedPop >= 40
uint16_t getSessionPeakPop();           // highest estimatedPop this session
uint16_t getSessionMinPop();            // lowest non-zero estimatedPop this session

// ==[ FEATURE 6: VENDOR CROSS-REF ]==
int  getVendorCorrelationCount();
const VendorCorrelation* getVendorCorrelations();

// ==[ COMPOSITE QUERIES ]== for terminal dump weighting
bool hasActiveIntel();          // any HIGH-confidence cohort or active attacker
bool hasCriticalIntel();        // dual-band stalk or attacker identified

#endif  // DEFENSE_PIPELINE_INTERNAL

}  // namespace XBand
