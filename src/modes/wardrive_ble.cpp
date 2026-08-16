/**
 * Wardrive BLE — thin CSV adapter reading from Recon's BLE catalog
 *
 * ==[ WARTHOG BLE ]== Recon drives BLE scanning via wardrive cadence mode.
 * this file only does session dedup + WiGLE CSV formatting.
 * zero NimBLE lifecycle — Recon stays warm throughout wardrive.
 */

#include "wardrive_ble.h"
#include "wardrive_internal.h"
#include "../core/config.h"
#include "../defense/recon.h"
#include "../defense/defense_pipeline.h"
#include "../util/debug_log.h"

#include <esp_heap_caps.h>
#include <math.h>
#include <string.h>

namespace WardriveBle {

// ==[ CONSTANTS ]==
static constexpr uint16_t BLE_DEDUP_SLOTS  = 4096;   // 16KB PSRAM hash table
static constexpr uint16_t BLE_DEDUP_MASK   = 4095;
static constexpr uint8_t BLE_PROBE_LIMIT   = 3;
static constexpr uint32_t BLE_DEDUP_EMPTY  = 0;
static constexpr uint32_t BLE_DEDUP_ZERO   = 0x80000000u;

// ==[ STATE ]==
static bool bleActive = false;
static bool blePaused = false;
static bool reconForced = false;
static uint16_t sessionBleDevices = 0;

static inline bool isOperational() {
    return bleActive && !blePaused;
}

// ==[ BUFFERS ]== PSRAM session dedup only (no scan buffer needed)
static uint32_t* bleDedupTable = nullptr;
static uint8_t* bleDedupLoggedBits = nullptr;

// ==[ APPEARANCE → WIGLE DEVICE TYPE ]==
static const char* appearanceToWigle(uint16_t appearance) {
    if (appearance == 0) return "Misc";
    uint8_t cat = (appearance >> 6) & 0x3FF;
    switch (cat) {
        case 1:  return "Phone";
        case 2:  return "Computer";
        case 3:  return "Watch";
        case 4:  return "Clock";
        case 5:  return "Display";
        case 6:  return "Camera";
        case 7:  return "Wearable";
        case 8:  return "Toy";
        case 9:  return "Health";
        case 10: return "Sensor";
        default: return "Misc";
    }
}

// ==[ BLE DEDUP ]== session-wide, keyed on payload hash from Recon catalog
// The logged bit is set only after appendBleRow() confirms the row reached the
// write buffer, so a device seen while SD is down stays eligible for a retry.
struct BleDedupTouch {
    bool firstSeen;
    bool needsRow;
    bool located;
    uint16_t idx;
};

static inline bool bleDedupLogged(uint16_t idx) {
    if (!bleDedupLoggedBits) return true;
    return (bleDedupLoggedBits[idx >> 3] & (uint8_t)(1u << (idx & 7))) != 0;
}

static inline void markBleDedupLogged(uint16_t idx) {
    if (!bleDedupLoggedBits) return;
    bleDedupLoggedBits[idx >> 3] |= (uint8_t)(1u << (idx & 7));
}

static BleDedupTouch bleDedupTouch(const uint8_t* payloadHash) {
    BleDedupTouch res = {};
    if (!bleDedupTable || !payloadHash) return res;
    uint32_t h;
    memcpy(&h, payloadHash, 4);
    uint32_t stored = (h == BLE_DEDUP_EMPTY) ? BLE_DEDUP_ZERO : h;
    uint16_t slot = h & BLE_DEDUP_MASK;

    for (uint8_t probe = 0; probe < BLE_PROBE_LIMIT; probe++) {
        uint16_t idx = (slot + probe) & BLE_DEDUP_MASK;
        if (bleDedupTable[idx] == BLE_DEDUP_EMPTY) {
            bleDedupTable[idx] = stored;
            res.firstSeen = true;
            res.needsRow = true;
            res.located = true;
            res.idx = idx;
            return res;
        }
        if (bleDedupTable[idx] == stored) {
            res.needsRow = !bleDedupLogged(idx);
            res.located = true;
            res.idx = idx;
            return res;
        }
    }
    return res;
}

// ==[ CSV ROW FORMATTING ]== reads TrackerEntry from Recon catalog
// Returns true only when the row is staged in the shared write buffer.
static bool appendBleRow(const Recon::TrackerEntry& te) {
    if (!Wardrive::writeBuf || !Wardrive::sdReady || !Wardrive::csvHeaderWritten || !Wardrive::haveObservation) return false;

    char safeName[36];
    safeName[0] = '\0';
    Wardrive::escapeCsvField(te.name, safeName, sizeof(safeName));

    const char* authLabel = appearanceToWigle(te.appearance);

    // WiGLE overloads Frequency for the Bluetooth device-type code. The BLE
    // Appearance value is that assigned-number code; leave it blank when the
    // advertiser did not provide one. RCOIs are WiFi roaming-consortium IDs,
    // not a spare field for BLE transmit power.
    char deviceType[8] = "";
    if (te.appearance != 0) {
        snprintf(deviceType, sizeof(deviceType), "%u", (unsigned)te.appearance);
    }

    char row[256];
    long altitudeM = lroundf(Wardrive::currentObservation.alt);
    int len = snprintf(row, sizeof(row),
        "%02X:%02X:%02X:%02X:%02X:%02X,%s,%s [LE],%s,0,%s,%d,%.6f,%.6f,%ld,%.1f,,%u,BLE\r\n",
        te.mac[0], te.mac[1], te.mac[2], te.mac[3], te.mac[4], te.mac[5],
        safeName, authLabel,
        Wardrive::currentObservation.timestamp, deviceType,
        (int)te.rssi,
        Wardrive::currentObservation.lat, Wardrive::currentObservation.lon,
        altitudeM, Wardrive::currentObservation.acc,
        (unsigned)te.companyId);

    if (len <= 0 || len >= (int)sizeof(row)) return false;
    return Wardrive::appendRawCsvRow(row, len);
}

// ==[ PROCESS RECON CATALOG ]== iterate BLE devices, dedup + format CSV
static void processReconCatalog() {
    const Recon::TrackerEntry* devices = DefensePipeline::snapshot().getBleDevices();
    int count = DefensePipeline::snapshot().getBleDeviceTableSize();
    int newDevices = 0;

    for (int i = 0; i < count; i++) {
        const Recon::TrackerEntry& te = devices[i];

        // Must match appendBleRow()'s own preconditions. Marking on
        // haveObservation alone burned the logged bit for every device seen
        // while SD was missing, so nothing was written after SD recovery.
        const bool canPersistRow =
            Wardrive::haveObservation && Wardrive::sdReady &&
            Wardrive::csvHeaderWritten && Wardrive::writeBuf;

        BleDedupTouch dedup = bleDedupTouch(te.payloadHash);
        if (!dedup.firstSeen && !dedup.needsRow) continue;

        if (dedup.firstSeen) {
            sessionBleDevices++;
            newDevices++;
        }

        if (canPersistRow && dedup.needsRow && appendBleRow(te)) {
            markBleDedupLogged(dedup.idx);
        }
    }

    if (newDevices > 0) {
        HAMLET_LOGF("[WD-BLE] %d new from recon catalog (total %d)\n",
                      newDevices, sessionBleDevices);
    }
}

// ==[ PUBLIC API ]==

void init() {
    bleActive = false;
    blePaused = false;
    sessionBleDevices = 0;

    if (!Config::getWardriveBleScan()) return;

    // alloc PSRAM session dedup table
    bleDedupTable = (uint32_t*)heap_caps_malloc(BLE_DEDUP_SLOTS * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
    if (!bleDedupTable) {
        HAMLET_LOGLN("[WD-BLE] PSRAM alloc failed — BLE disabled");
        return;
    }
    memset(bleDedupTable, 0, BLE_DEDUP_SLOTS * sizeof(uint32_t));
    bleDedupLoggedBits = (uint8_t*)heap_caps_malloc((BLE_DEDUP_SLOTS + 7) / 8, MALLOC_CAP_SPIRAM);
    if (bleDedupLoggedBits) {
        memset(bleDedupLoggedBits, 0, (BLE_DEDUP_SLOTS + 7) / 8);
    } else {
        HAMLET_LOGLN("[WD-BLE] PSRAM alloc failed: logged bits");
    }

    // enable BLE CSV interleave after buffers are ready and force the scan callback
    DefensePipeline::requestOperatingState(Defense::OperatingState::WARDRIVE);
    reconForced = true;
    bleActive = true;

    HAMLET_LOGF("[WD-BLE] initialized (recon adapter). dedup=%p\n", bleDedupTable);
}

void shutdown() {
    if (reconForced) {
        DefensePipeline::requestOperatingState(Defense::OperatingState::BACKGROUND);
        reconForced = false;
    }

    if (bleDedupTable) { heap_caps_free(bleDedupTable); bleDedupTable = nullptr; }
    if (bleDedupLoggedBits) { heap_caps_free(bleDedupLoggedBits); bleDedupLoggedBits = nullptr; }

    bleActive = false;
    blePaused = false;

    HAMLET_LOGF("[WD-BLE] shutdown. %d BLE devices total\n", sessionBleDevices);
}

void update() {
    if (!isOperational()) return;

    // Recon sets wardriveBleReady when a BLE scan cycle completes
    if (DefensePipeline::consumeWardriveBleReady()) {
        processReconCatalog();
    }
}

void onWifiSweepComplete() {
    if (bleActive) DefensePipeline::onWardriveSweepComplete();
}

bool wantsScanWindow() {
    return isOperational() && DefensePipeline::wardriveWantsBleWindow();
}

void onPause() {
    if (blePaused) return;
    blePaused = true;
    DefensePipeline::pauseBLEScanForGATT();
}

void onResume() {
    if (!blePaused) return;
    blePaused = false;
}

bool isEnabled() {
    return bleActive;
}

bool isScanning() {
    if (!isOperational()) return false;
    return DefensePipeline::snapshot().isScanning() && DefensePipeline::wardriveWantsBleWindow();
}

uint16_t getSessionNewDevices() {
    return sessionBleDevices;
}

} // namespace WardriveBle
