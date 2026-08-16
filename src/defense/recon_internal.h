/**
 * recon_internal.h — shared internal state for recon.cpp / recon_ble.cpp
 *
 * NOT a public API. Only included by recon.cpp and recon_ble.cpp.
 * Follows the MenuPig internal header pattern: extern declarations for
 * shared state, shared type definitions, and cross-file function decls.
 *
 * State defined in recon.cpp, extern-declared here so recon_ble.cpp
 * can read/write the same data. All runs single-threaded on main loop
 * (except NimBLE callback — see callback safety rules in CONVENTIONS.md).
 */
#pragma once

#ifndef DEFENSE_PIPELINE_INTERNAL
#define DEFENSE_PIPELINE_INTERNAL 1
#endif
#include "recon.h"
#include <stdint.h>
#include <atomic>
#include <stddef.h>
#include "../radio/c5monster_uart.h"

// ==[ BLE CONDITIONAL ]== NimBLE: ~40KB DRAM (vs Bluedroid ~70KB).
// add -DRECON_BLE_SUPPORT=1 to build_flags when platform has headroom.
#if defined(RECON_BLE_SUPPORT) && RECON_BLE_SUPPORT
#define RECON_BLE_ENABLED 1
#else
#define RECON_BLE_ENABLED 0
#endif

namespace Recon {

// ==[ STATE MACHINE ]==
enum class ScanState : uint8_t {
    SLEEPING,
    BLE_SCANNING,
    WIFI_SCANNING,
    WIFI_PROCESSING,
    DEAUTH_SNIFF,      // promiscuous listen for deauth/disassoc frames
    HOGWASH_HOLD,      // camp on attacker channel, keep injecting fake handshakes
    PARASITIC,
    SUSPENDED
};

// ==[ BLE SCAN BUFFER ]== temp storage built in NimBLE callback, processed on complete
#if RECON_BLE_ENABLED

static constexpr int BLE_SCAN_BUF_SIZE              = MAX_BLE_DEVICES;
static constexpr uint8_t BLE_SCAN_MACS_PER_HIT      = 4;
static constexpr uint8_t BLE_SCAN_UNIQUE_MAC_CAP     = 128;
static constexpr size_t BLE_NAME_MAX                 = 16;
static constexpr size_t BLE_PAYLOAD_PREVIEW_MAX      = 16;
static constexpr uint8_t BLE_UNKNOWN_BG_LIMIT        = 24;
static constexpr uint8_t BLE_UNKNOWN_PRIORITY_LIMIT  = 48;
static constexpr int8_t BLE_UNKNOWN_BG_MIN_RSSI      = -85;
static constexpr int8_t BLE_UNKNOWN_PRIORITY_MIN_RSSI = -90;
static constexpr int8_t BLE_CATALOG_REPLACE_MARGIN_DB = 4;

using BLERawHit = Defense::BleObservation;

// ==[ BLE ADVERTISEMENT PARSING CONSTANTS ]==
static constexpr uint16_t APPLE_COMPANY_ID         = 0x004C;
static constexpr uint8_t  APPLE_FINDMY_TYPE        = 0x12;
static constexpr uint16_t SAMSUNG_SVC_UUID         = 0xFD5A;
static constexpr uint16_t TILE_SVC_UUID            = 0xFEED;
static constexpr uint16_t FASTPAIR_SVC_UUID        = 0xFE2C;
static constexpr uint8_t  APPLE_IBEACON_TYPE       = 0x02;
static constexpr uint8_t  APPLE_NEARBY_TYPE        = 0x10;
static constexpr uint8_t  APPLE_AIRDROP_TYPE       = 0x05;
static constexpr uint8_t  APPLE_HANDOFF_TYPE       = 0x0C;
static constexpr uint8_t  APPLE_HOTSPOT_TYPE       = 0x0D;
static constexpr uint16_t EDDYSTONE_SVC_UUID       = 0xFEAA;
static constexpr uint8_t  EDDYSTONE_UID_FRAME      = 0x00;
static constexpr uint8_t  EDDYSTONE_URL_FRAME      = 0x10;
static constexpr uint8_t  EDDYSTONE_TLM_FRAME      = 0x20;
static constexpr uint8_t  EDDYSTONE_EID_FRAME      = 0x30;
static constexpr uint16_t HM10_SVC_UUID            = 0xFFE0;

// Flipper Zero detection
static constexpr uint16_t FLIPPER_COMPANY_ID       = 0x038F;
static constexpr uint8_t  FLIPPER_MAC_PREFIX_1[]   = {0x80, 0xE1, 0x26};
static constexpr uint8_t  FLIPPER_MAC_PREFIX_2[]   = {0x80, 0xE1, 0x27};
static constexpr uint8_t  FLIPPER_MAC_PREFIX_3[]   = {0x0C, 0xFA, 0x22};
static constexpr uint16_t FLIPPER_SVC_BLACK        = 0x3081;
static constexpr uint16_t FLIPPER_SVC_WHITE        = 0x3082;
static constexpr uint16_t FLIPPER_SVC_TRANSPARENT  = 0x3083;

// Samsung unregistered SmartTag (setup mode — potentially planted)
static constexpr uint16_t SAMSUNG_UNREG_SVC_UUID   = 0xFD59;

// BLE HID (rubber ducky detection)
static constexpr uint16_t BLE_HID_SVC_UUID         = 0x1812;

// Apple Continuity spam subtypes
static constexpr uint8_t  APPLE_NEARBY_ACTION_TYPE = 0x07;
static constexpr uint8_t  APPLE_ACTION_MODAL_TYPE  = 0x0F;

// Xiaomi MiBeacon ecosystem
static constexpr uint16_t XIAOMI_MIBEACON_SVC_UUID = 0xFE95;

// Amazon Sidewalk (Ring, Echo BLE beacons)
static constexpr uint16_t AMAZON_SIDEWALK_SVC_UUID = 0xFD82;

// Google/Apple Exposure Notification (GAEN — still deployed on billions)
static constexpr uint16_t GAEN_SVC_UUID            = 0xFD6F;

// DULT cross-platform tracker detection (draft standard, 128-bit UUID little-endian)
static const uint8_t DULT_SVC_UUID_128[] = {
    0x85, 0x2A, 0x9F, 0x57, 0xC5, 0x2A, 0xED, 0x88,
    0x26, 0xC2, 0xF4, 0x12, 0x01, 0x00, 0x19, 0x15
};

// device type label from metadata cascade (shared by recon.cpp + recon_ble.cpp)
const char* deviceLabelFromMeta(ThreatType type, uint16_t appearance,
                                uint32_t classOfDevice, const char* name);

#endif // RECON_BLE_ENABLED

// ==[ SHARED STATE ]== defined in recon.cpp, extern here for recon_ble.cpp
//
// Threading model:
//   - trackerTable, trackerCount, followingCount, spamCount: main loop only
//   - bleDeviceTable: main-loop fusion only
//   - bleDeviceCount: atomic for main-loop readers and publication fences
//   - totalBLEDevices, appleContinuityCount: volatile — written by NimBLE callback
//   - state, blePriorityMode, bleWardriveMode: main loop only
//   - bleScanActive, processingBLE: atomic — cross-core synchronization
//   - bleScanBufCount: atomic — NimBLE callback writes, main loop reads
//   - wardriveBleReady: volatile — set by NimBLE callback, read by main loop
//

// --- Group 1: tracker & device tables (both WiFi and BLE analysis) ---
extern TrackerEntry* trackerTable;       // PSRAM, MAX_TRACKERS entries
extern int trackerCount;
extern TrackerEntry* bleDeviceTable;     // PSRAM, MAX_BLE_DEVICES entries
extern std::atomic<int> bleDeviceCount;
extern int followingCount;
extern int spamCount;
extern volatile uint16_t totalBLEDevices;
extern volatile uint16_t appleContinuityCount;

// --- Group 2: state machine & mode flags ---
extern ScanState state;
extern bool blePriorityMode;
extern bool bleWardriveMode;
extern uint8_t wardriveSweepCount;
extern volatile bool wardriveBleReady;
extern bool blePinnedPayloadValid;
extern uint8_t blePinnedPayloadHash[4];

#if RECON_BLE_ENABLED
// --- Group 3: BLE engine lifecycle (only when NimBLE compiled in) ---
extern bool bleInitialized;
extern std::atomic<bool> bleScanActive;     // cross-core: NimBLE callback ↔ main loop
extern std::atomic<bool> processingBLE;     // main loop is compacting/fusing catalog
extern bool bleActiveScanMode;              // user toggle: passive vs active scan
extern uint32_t bleScanStart;
extern uint32_t lastBLEScan;
extern uint8_t offlineScanCount;

// --- Group 4: BLE scan buffer (NimBLE callback → main loop) ---
extern BLERawHit* bleScanBuf;               // PSRAM, BLE_SCAN_BUF_SIZE entries
extern std::atomic<int> bleScanBufCount;    // atomic slot claiming
extern uint16_t* bleLiveFusedCounts;        // fusion cursor per producer slot
extern uint8_t (*bleSeenMacs)[6];           // scratch: unique MACs per scan cycle
extern uint8_t bleSeenMacCount;
extern uint8_t (*bleContinuityMacs)[6];     // scratch: Apple Continuity MACs
extern uint8_t bleContinuityMacCount;
#endif // RECON_BLE_ENABLED

// ==[ BEACON IE CAPTURE ]== populated during deauth sniff, processed on main loop
struct BeaconIECapture {
    uint8_t bssid[6];
    uint8_t uniqueIETypes;       // count of distinct IE type IDs
    uint8_t vendorIECount;       // count of vendor-specific IEs (type 0xDD)
    bool    valid;
};
static constexpr int MAX_BEACON_CAPTURES = 32;

// ==[ PROBE FINGERPRINT CAPTURE ]== populated during deauth sniff
struct ProbeCapture {
    uint8_t clientMac[6];
    int8_t  rssi;
    uint8_t channel;
    uint32_t ieHash;             // FNV-1a of sorted IE type chain
    uint8_t  ieCount;
    bool     hasHTCaps;          // 0x2D
    bool     hasVHTCaps;         // 0xBF
    bool     hasExtCaps;         // 0x7F
    bool     hasWPS;             // 0xDD with WPS OUI
    bool     valid;
};
static constexpr int MAX_PROBE_CAPTURES = 16;

// ==[ SHARED HELPER FUNCTIONS ]== defined in recon.cpp, called from recon_ble.cpp

void pushEvent(const ReconEventData& ev);
void recordForensicEvent(const ReconEventData& ev);
void* psramAlloc(size_t size, const char* label);
uint32_t fnvHash(const void* data, int len);
bool updateAcquisition(uint32_t now);
void fuseAcquiredObservations(uint32_t now);
void finalizeAcquisition(uint32_t now);
void updateFusion(uint32_t now);
void updateSideEffects(uint32_t now);

// ==[ BLE CADENCE FUNCTIONS ]== defined in recon.cpp, needed by BLE scan logic

uint32_t currentBLEScanIntervalMs();
uint32_t currentBLEScanDurationMs();

// ==[ BLE FUNCTIONS ]== defined in recon_ble.cpp when enabled, stubs in recon.cpp when disabled

void initBLE();
void deinitBLE();
bool startBLEScan();
void stopBLEScan();
void processBLEResults();
void fuseLiveBLEObservations(uint32_t now);
void syncBLECatalogThreatState();

// ==[ C5MONSTER 5GHz FEED ]==
void feedC5MonsterScan(const C5Monster::ScanResults& results);

} // namespace Recon
