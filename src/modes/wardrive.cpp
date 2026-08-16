/**
 * Wardrive — WiFi wardriving scan engine
 *
 * ==[ WARTHOG ]== async scan → Arduino-owned results → PSRAM dedup → WiGLE CSV.
 * no duplicate result allocation. ~1.4s per full 13-channel sweep.
 */

#include "wardrive.h"
#include "wardrive_internal.h"
#include "wardrive_ble.h"
#include "wardrive_policy.h"
#include "wardrive_scene.h"
#include "wardrive_telemetry.h"
#include "../core/config.h"
#include "../core/achievements.h"
#include "../core/gps.h"
#include "../piglet/mood.h"
#include "../hal/sd_storage.h"
#include "../hal/platform.h"
#include "../defense/xband.h"
#include "../sync/nowflock_export.h"
#include "../sync/nowflock_transport.h"
#include "../defense/recon.h"
#include "../defense/defense_pipeline.h"
#include "../net/upload_contracts.h"
#include "../build_info.h"
#include "../util/debug_log.h"
#include "../radio/c5monster_uart.h"
#include <WiFi.h>
#include <M5Unified.h>
#include <esp_wifi.h>
#include <math.h>
#include <time.h>

namespace Wardrive {

// ==[ LIMITS ]==
static const uint16_t DEDUP_SLOTS   = 8192;    // 32KB PSRAM hash table (32-bit FNV-1a)
static const uint16_t DEDUP_MASK    = 8191;     // slot mask (power of 2)
static const uint8_t  PROBE_LIMIT   = 3;        // linear probing steps
static const uint16_t DEDUP_FALLBACK_SLOTS = 128; // exact BSSID spillover cache
static const uint16_t WRITE_BUF_SZ  = 8192;     // 8KB PSRAM write buffer
static const uint16_t FLUSH_THRESH  = 4096;     // flush when buffer > 4KB
static const uint32_t FLUSH_INTERVAL_MS = 2000;
static const uint32_t SCAN_RETRY_MS = 750;      // cool-off after a failed scan start
static const uint64_t FALLBACK_EMPTY = UINT64_MAX; // sentinel for empty fallback slot
static const uint32_t DEDUP_EMPTY = 0;             // empty slot sentinel (hash 0 stored as DEDUP_ZERO)
static const uint32_t DEDUP_ZERO  = 0x80000000u;   // substitute when FNV-1a hashes to exact 0

// ==[ DEDUP HASH TABLE ]== 32-bit FNV-1a of 6-byte BSSID (<0.0002% FPR at 4K nets)
static uint32_t* dedupTable = nullptr;           // PSRAM, DEDUP_SLOTS entries
static uint64_t* dedupFallback = nullptr;        // exact 48-bit BSSID spillover cache
static uint8_t* dedupLoggedBits = nullptr;       // one bit per main-table slot: CSV row written
static bool* dedupFallbackLogged = nullptr;      // CSV row written for fallback slots

// ==[ WRITE BUFFER ]== CSV rows staged before SD flush (shared via wardrive_internal.h)
char* writeBuf = nullptr;                        // PSRAM, WRITE_BUF_SZ bytes
uint16_t writeBufLen = 0;
static uint32_t lastFlushMs = 0;
const uint16_t WRITE_BUF_SZ_VAL = WRITE_BUF_SZ;

// ==[ STATE ]==
static bool active = false;
static bool paused = false;
static bool scanPending = false;
static uint32_t sessionNewNets = 0;
static uint16_t sessionScanCycles = 0;
static uint32_t sessionStartTime = 0;

// ==[ SD FILE ]== (csvHeaderWritten, sdReady shared via wardrive_internal.h)
static char csvPath[48] = {0};
bool csvHeaderWritten = false;
bool sdReady = false;
static bool sdFailureLogged = false;
static bool dedupFallbackFullLogged = false;
static bool gpsHoldoffLogged = false;
static bool scanStartFailureLogged = false;
static uint32_t scanRetryAtMs = 0;   // 0 = no cool-off pending
static uint32_t lastSDRetry = 0;
static bool c5WardriveHasCoords = false;
static double c5WardriveLatitude = 0.0;
static double c5WardriveLongitude = 0.0;
static uint32_t c5WardriveCoordMs = 0;
static ScanTelemetry scanTelemetry = {};

// ==[ MILESTONE TRACKING ]==
static bool milestone50  = false;
static bool milestone100 = false;
static bool milestone500 = false;
static bool milestone1k  = false;

// ==[ GPS ]== delegated to GPS:: namespace (src/core/gps.cpp)

// WigleObservation struct defined in wardrive_internal.h
WigleObservation currentObservation = {};
bool haveObservation = false;

// ==[ INTERNAL HELPERS ]==

// defined with the rest of the dual-band state at the bottom of this file
static void resetDualBandStats();

static void copyScanSsid(char* dst, size_t cap, const char* src, size_t srcCap) {
    if (!dst || cap == 0) return;
    size_t n = 0;
    if (src) {
        while (n < srcCap && src[n] != '\0') ++n;
        if (n > cap - 1) n = cap - 1;
        memcpy(dst, src, n);
    }
    dst[n] = '\0';
}

static void resetScanTelemetry() {
    memset(&scanTelemetry, 0, sizeof(scanTelemetry));
    scanTelemetry.wifiStrongestRssi = -127;
    scanTelemetry.c5StrongestRssi = -127;
}

// FNV-1a 32-bit hash of 6-byte BSSID
static uint32_t hashBSSID(const uint8_t* bssid) {
    uint32_t h = 0x811C9DC5u;  // FNV-1a offset basis (32-bit)
    for (uint8_t i = 0; i < 6; i++) {
        h ^= bssid[i];
        h *= 0x01000193u;      // FNV-1a prime (32-bit)
    }
    return h;
}

static uint64_t packBSSID(const uint8_t* bssid) {
    uint64_t packed = 0;
    for (uint8_t i = 0; i < 6; i++) {
        packed = (packed << 8) | bssid[i];
    }
    return packed;
}

// Result of claiming a BSSID slot for this session.
//   firstSeen — never observed before; drives XP/counters.
//   needsRow  — no CSV row has been written for it yet.
//   located   — a slot (main table or fallback) holds it, so the logged bit
//               can be set later via markDedupRowWritten().
// The logged bit is deliberately NOT set here: a row that fails to reach the
// write buffer must stay eligible so a later sweep can still persist it.
struct DedupTouch {
    bool firstSeen;
    bool needsRow;
    bool located;
    bool fallback;
    uint16_t idx;
};

static bool isFreshC5WardriveCoords(uint32_t nowMs) {
    return WardrivePolicy::isUsableC5Coordinate(
        hasC5WardriveConnection(), c5WardriveHasCoords,
        c5WardriveCoordMs, nowMs);
}

static inline bool dedupLogged(uint16_t idx) {
    if (!dedupLoggedBits) return true;
    return (dedupLoggedBits[idx >> 3] & (uint8_t)(1u << (idx & 7))) != 0;
}

static inline void markDedupLogged(uint16_t idx) {
    if (!dedupLoggedBits) return;
    dedupLoggedBits[idx >> 3] |= (uint8_t)(1u << (idx & 7));
}

static DedupTouch dedupTouchFallback(const uint8_t* bssid) {
    DedupTouch res = {};
    if (!dedupFallback) return res;

    uint64_t packed = packBSSID(bssid);
    int16_t emptyIdx = -1;

    for (uint16_t i = 0; i < DEDUP_FALLBACK_SLOTS; i++) {
        if (dedupFallback[i] == packed) {
            bool wasLogged = dedupFallbackLogged ? dedupFallbackLogged[i] : true;
            res.needsRow = !wasLogged;
            res.located = true;
            res.fallback = true;
            res.idx = i;
            return res;
        }
        if (dedupFallback[i] == FALLBACK_EMPTY && emptyIdx < 0) {
            emptyIdx = (int16_t)i;
        }
    }

    if (emptyIdx >= 0) {
        dedupFallback[emptyIdx] = packed;
        if (dedupFallbackLogged) dedupFallbackLogged[emptyIdx] = false;
        res.firstSeen = true;
        res.needsRow = true;
        res.located = true;
        res.fallback = true;
        res.idx = (uint16_t)emptyIdx;
        return res;
    }

    if (!dedupFallbackFullLogged) {
        HAMLET_LOGLN("[WARDRIVE] dedup fallback saturated; dropping overflowed BSSIDs");
        Mood::setPhrase("dedup saturated. dense area.", AvatarState::NEUTRAL);
        dedupFallbackFullLogged = true;
    }
    return res;
}

static void noteSDWriteFailure(const char* phase) {
    SDStorage::endWriteStream();
    writeBufLen = 0;
    csvHeaderWritten = false;
    if (!sdFailureLogged) {
        HAMLET_LOGF("[WARDRIVE] SD write failed during %s; disabling CSV logging\n", phase);
        sdFailureLogged = true;
    }
    sdReady = false;
}

// Touch BSSID session state. firstSeen drives XP/counts; needsRow drives CSV.
static DedupTouch dedupTouch(const uint8_t* bssid) {
    DedupTouch res = {};
    if (!bssid) return res;
    if (!dedupTable) return dedupTouchFallback(bssid);
    uint32_t h = hashBSSID(bssid);
    uint16_t slot = h & DEDUP_MASK;
    uint32_t stored = (h == DEDUP_EMPTY) ? DEDUP_ZERO : h;  // avoid empty sentinel

    for (uint8_t probe = 0; probe < PROBE_LIMIT; probe++) {
        uint16_t idx = (slot + probe) & DEDUP_MASK;
        if (dedupTable[idx] == DEDUP_EMPTY) {
            // empty slot — new BSSID
            dedupTable[idx] = stored;
            res.firstSeen = true;
            res.needsRow = true;
            res.located = true;
            res.idx = idx;
            return res;
        }
        if (dedupTable[idx] == stored) {
            // already seen (full 32-bit FNV-1a: <0.0002% FPR at 4K nets)
            res.needsRow = !dedupLogged(idx);
            res.located = true;
            res.idx = idx;
            return res;
        }
    }
    return dedupTouchFallback(bssid);
}

// Record that a CSV row reached the write buffer. Only called after a
// successful append so a dropped row can be retried on a later sweep.
static void markDedupRowWritten(const DedupTouch& touch) {
    if (!touch.located) return;
    if (touch.fallback) {
        if (dedupFallbackLogged && touch.idx < DEDUP_FALLBACK_SLOTS) {
            dedupFallbackLogged[touch.idx] = true;
        }
        return;
    }
    markDedupLogged(touch.idx);
}

// format WiGLE auth mode string from ESP-IDF auth type
static const char* wigleAuthMode(wifi_auth_mode_t auth) {
    switch (auth) {
        case WIFI_AUTH_OPEN:            return "[ESS]";
        case WIFI_AUTH_WEP:             return "[WEP][ESS]";
        case WIFI_AUTH_WPA_PSK:         return "[WPA-PSK-CCMP][ESS]";
        case WIFI_AUTH_WPA2_PSK:        return "[WPA2-PSK-CCMP][ESS]";
        case WIFI_AUTH_WPA_WPA2_PSK:    return "[WPA-PSK-CCMP][WPA2-PSK-CCMP][ESS]";
        case WIFI_AUTH_WPA2_ENTERPRISE: return "[WPA2-EAP-CCMP][ESS]";
        case WIFI_AUTH_WPA3_PSK:        return "[WPA3-SAE-CCMP][ESS]";
        case WIFI_AUTH_WPA2_WPA3_PSK:   return "[WPA2-PSK-CCMP][WPA3-SAE-CCMP][ESS]";
        default:                        return "[UNKNOWN][ESS]";
    }
}

// channel → frequency
static uint16_t channelToFreq(uint8_t ch) {
    if (ch >= 1 && ch <= 13) return 2407 + ch * 5;
    if (ch == 14) return 2484;
    if (ch >= 36 && ch <= 165) return 5000 + ch * 5;  // 5GHz UNII
    return 2412;  // fallback
}

static bool getTimestamp(char* buf, size_t len, uint32_t* epochOut = nullptr) {
    if (!buf || len == 0) return false;
    if (epochOut) *epochOut = 0;

    uint32_t epoch = GPS::getEpochUtc();
    if (epoch <= 1704067200u) epoch = Config::getTrustedEpoch();
    if (epoch <= 1704067200u) return false;
    time_t t = (time_t)epoch;
    struct tm utcTm = {};
    if (gmtime_r(&t, &utcTm) == nullptr) return false;
    if (epochOut) *epochOut = epoch;
    return strftime(buf, len, "%Y-%m-%d %H:%M:%S", &utcTm) > 0;
}

static void generateFilename() {
    time_t now = time(nullptr);
    if (now > 1704067200) {
        struct tm utcTm = {};
        if (gmtime_r(&now, &utcTm) != nullptr) {
            snprintf(csvPath, sizeof(csvPath), "/hamlet/wardrive/WD_%04d%02d%02d_%02d%02d%02d.csv",
                     utcTm.tm_year + 1900, utcTm.tm_mon + 1, utcTm.tm_mday,
                     utcTm.tm_hour, utcTm.tm_min, utcTm.tm_sec);
            return;
        }
    }

    auto dt = M5.Rtc.getDateTime();
    snprintf(csvPath, sizeof(csvPath), "/hamlet/wardrive/WD_%04d%02d%02d_%02d%02d%02d.csv",
             dt.date.year, dt.date.month, dt.date.date,
             dt.time.hours, dt.time.minutes, dt.time.seconds);
}

static bool captureObservation(WigleObservation* obs) {
    if (!obs) return false;

    float coreAccuracy = 0.0f;
    if (GPS::hasFix()) {
        // HDOP is dimensionless. Convert it to an approximate horizontal
        // accuracy using a conservative 5 m user-equivalent range error before
        // labeling it AccuracyMeters in WiGLE CSV.
        const float hdop = GPS::getHdop();
        coreAccuracy = hdop > 0.0f
            ? fminf(999.0f, fmaxf(1.0f, hdop * 5.0f))
            : 0.0f;
    }
    const WardrivePolicy::CoordinateChoice coordinates =
        WardrivePolicy::chooseCoordinates(
            millis(),
            hasC5WardriveConnection(),
            c5WardriveHasCoords,
            c5WardriveCoordMs,
            c5WardriveLatitude,
            c5WardriveLongitude,
            GPS::hasFix(),
            GPS::getLatitude(),
            GPS::getLongitude(),
            GPS::getAltitude(),
            coreAccuracy);
    if (coordinates.source == WardrivePolicy::CoordinateSource::NONE) {
        return false;
    }
    if (!getTimestamp(obs->timestamp, sizeof(obs->timestamp), &obs->epochS)) return false;

    obs->lat = coordinates.latitude;
    obs->lon = coordinates.longitude;
    obs->alt = coordinates.altitudeMeters;
    obs->acc = coordinates.accuracyMeters;
    return true;
}

// Escape one CSV field. On overflow this bails out mid-write, so dst is reset
// to an empty string rather than left holding an unterminated partial field —
// callers feed the result straight to snprintf("%s").
size_t escapeCsvField(const char* src, char* dst, size_t dstLen) {
    if (!src || !dst || dstLen == 0) return 0;
    dst[0] = '\0';

    size_t srcLen = 0;
    bool needsQuotes = false;
    while (srcLen < 32 && src[srcLen]) {
        char c = src[srcLen];
        if (c == ',' || c == '"' || c == '\n' || c == '\r') {
            needsQuotes = true;
        }
        srcLen++;
    }

    if (srcLen > 0 && (src[0] == ' ' || src[srcLen - 1] == ' ')) {
        needsQuotes = true;
    }

    size_t di = 0;
    if (needsQuotes) {
        if (di + 1 >= dstLen) { dst[0] = '\0'; return 0; }
        dst[di++] = '"';
    }

    for (size_t i = 0; i < srcLen; i++) {
        unsigned char c = (unsigned char)src[i];
        if ((c < 0x20 && c != '\t') || c == 0x7F) {
            c = '_';
        }

        if (c == '"') {
            if (di + 2 >= dstLen) { dst[0] = '\0'; return 0; }
            dst[di++] = '"';
            dst[di++] = '"';
            continue;
        }

        if (di + 1 >= dstLen) { dst[0] = '\0'; return 0; }
        dst[di++] = (char)c;
    }

    if (needsQuotes) {
        if (di + 1 >= dstLen) { dst[0] = '\0'; return 0; }
        dst[di++] = '"';
    }

    dst[di] = '\0';
    return di;
}

// The ledger names the board that gathered it; provenance cannot be a theme.
static void writeHeader() {
    if (csvHeaderWritten || !sdReady) return;

    char hdr1[320];
#if HAMLET_TARGET_CORES3SE
    int hdr1Len = snprintf(
        hdr1, sizeof(hdr1),
        "WigleWifi-1.6,appRelease=PANCETTA-%s-%s,model=M5CoreS3SE,release=%s,"
        "device=PANCETTA,display=IPS320x240,board=ESP32-S3,brand=M5Stack,"
        "star=Sol,body=3,subBody=0\r\n",
        BUILD_VERSION, BUILD_COMMIT, BUILD_VERSION);
#else
    int hdr1Len = snprintf(
        hdr1, sizeof(hdr1),
        "WigleWifi-1.6,appRelease=PANCETTA-%s-%s,model=M5Core2,release=%s,"
        "device=PANCETTA,display=IPS320x240,board=ESP32,brand=M5Stack,"
        "star=Sol,body=3,subBody=0\r\n",
        BUILD_VERSION, BUILD_COMMIT, BUILD_VERSION);
#endif
    char hdr2[192];
    int hdr2Len = snprintf(hdr2, sizeof(hdr2), "%s\r\n",
                           UploadContracts::WIGLE_V16_COLUMNS);

    if (hdr1Len <= 0 || hdr1Len >= (int)sizeof(hdr1) ||
        hdr2Len <= 0 || hdr2Len >= (int)sizeof(hdr2)) {
        noteSDWriteFailure("header encode");
        return;
    }

    bool ok1 = SDStorage::beginWriteStream(csvPath, true);
    bool ok2 = ok1 && SDStorage::writeStream((const uint8_t*)hdr1, (size_t)hdr1Len);
    bool ok3 = ok2 && SDStorage::writeStream((const uint8_t*)hdr2, (size_t)hdr2Len);
    bool ok4 = ok3 && SDStorage::flushStream();

    if (!ok1 || !ok2 || !ok3 || !ok4) {
        noteSDWriteFailure("header write");
        return;
    }

    csvHeaderWritten = true;
}

// flush write buffer to SD (shared via wardrive_internal.h)
bool flushBuffer() {
    if (writeBufLen == 0 || !writeBuf) return true;
    if (!sdReady || !csvHeaderWritten) return false;
    if (!SDStorage::writeStream((const uint8_t*)writeBuf, writeBufLen)) {
        noteSDWriteFailure("buffer flush");
        return false;
    }
    if (!SDStorage::flushStream()) {
        noteSDWriteFailure("stream flush");
        return false;
    }
    writeBufLen = 0;
    lastFlushMs = millis();
    return true;
}

// append a CSV row to write buffer. Returns true only when the row is staged.
static bool appendRow(const uint8_t* bssid, const char* ssid,
                      wifi_auth_mode_t auth, uint8_t channel, int8_t rssi) {
    if (!writeBuf || !sdReady || !csvHeaderWritten || !bssid || !ssid || !haveObservation) return false;

    char safeSsid[70];
    safeSsid[0] = '\0';
    escapeCsvField(ssid, safeSsid, sizeof(safeSsid));
    // empty SSID is valid (hidden networks) — don't bail on empty result

    uint16_t freq = channelToFreq(channel);
    long altitudeM = lroundf(currentObservation.alt);

    char row[320];
    int len = snprintf(row, sizeof(row),
        "%02X:%02X:%02X:%02X:%02X:%02X,%s,%s,%s,%d,%d,%d,%.6f,%.6f,%ld,%.1f,,,WIFI\r\n",
        bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5],
        safeSsid, wigleAuthMode(auth), currentObservation.timestamp, channel, freq, rssi,
        currentObservation.lat, currentObservation.lon,
        altitudeM, currentObservation.acc);

    if (len <= 0 || len >= (int)sizeof(row)) return false;

    // check buffer space
    if ((size_t)len >= WRITE_BUF_SZ) return false;

    if ((size_t)writeBufLen + (size_t)len >= WRITE_BUF_SZ) {
        if (!flushBuffer()) {
            return false;
        }
    }

    if ((size_t)writeBufLen + (size_t)len >= WRITE_BUF_SZ) {
        return false;
    }

    memcpy(writeBuf + writeBufLen, row, len);
    writeBufLen += len;

    NowFlockExport::onWardriveRow(
        bssid, ssid, channel, rssi, wigleAuthMode(auth),
        currentObservation.epochS,
        (int32_t)(currentObservation.lat * 10000000.0),
        (int32_t)(currentObservation.lon * 10000000.0),
        millis());
    return true;
}

// append a raw CSV row to write buffer (shared via wardrive_internal.h)
bool appendRawCsvRow(const char* row, int len) {
    if (!writeBuf || !sdReady || !csvHeaderWritten || len <= 0) return false;
    if ((size_t)len >= WRITE_BUF_SZ) return false;
    if ((size_t)writeBufLen + (size_t)len >= WRITE_BUF_SZ) {
        if (!flushBuffer()) return false;
    }
    if ((size_t)writeBufLen + (size_t)len >= WRITE_BUF_SZ) return false;
    memcpy(writeBuf + writeBufLen, row, len);
    writeBufLen += len;
    return true;
}

// Arm the post-failure cool-off. update() runs once per frame, so retrying a
// refused scan immediately would re-enter the WiFi driver 30-60x a second and
// starve the render loop for as long as the radio stays unhappy.
static void deferScanRetry(uint32_t now) {
    scanRetryAtMs = now + SCAN_RETRY_MS;
    if (scanRetryAtMs == 0) scanRetryAtMs = 1;  // keep 0 as the "no cool-off" sentinel
}

// kick off async WiFi scan
static bool startScan() {
    if (paused || scanPending) return false;
    uint32_t now = millis();
    if (scanRetryAtMs != 0) {
        if ((int32_t)(now - scanRetryAtMs) < 0) return false;  // still cooling off
        scanRetryAtMs = 0;
    }
    if (!WiFi.mode(WIFI_STA)) {
        deferScanRetry(now);
        if (!scanStartFailureLogged) {
            HAMLET_LOGLN("[WARDRIVE] WiFi STA mode failed; scan retry pending");
            scanStartFailureLogged = true;
        }
        return false;
    }
    int rc = WiFi.scanNetworks(true, true);  // async=true, show_hidden=true
    if (rc == WIFI_SCAN_FAILED) {
        WiFi.scanDelete();
        deferScanRetry(now);
        if (!scanStartFailureLogged) {
            HAMLET_LOGF("[WARDRIVE] WiFi scan start failed (%d); retry pending\n", rc);
            scanStartFailureLogged = true;
        }
        return false;
    }
    scanStartFailureLogged = false;
    scanPending = true;
    scanTelemetry.wifiScanPending = true;
    return true;
}

static void noteNewNetwork() {
    sessionNewNets++;

    // Graduated XP: first 50 = 5XP, next 50 = 3XP, after 100 = 2XP.
    uint32_t xp = 5;
    if (sessionNewNets > 100) xp = 2;
    else if (sessionNewNets > 50) xp = 3;
    Config::addXP(xp);

    if (sessionNewNets % 10 == 0) {
        Mood::onWardriveNetwork();
    }

    if (sessionNewNets == 50 && !milestone50) {
        milestone50 = true;
        Config::addXP(20);
        Mood::onWardriveMilestone(50);
    } else if (sessionNewNets == 100 && !milestone100) {
        milestone100 = true;
        Config::addXP(50);
        Mood::onWardriveMilestone(100);
        Achievements::tryUnlock(Achievement::CARTOGRAPHER);
    } else if (sessionNewNets == 500 && !milestone500) {
        milestone500 = true;
        Config::addXP(100);
        Mood::onWardriveMilestone(500);
    } else if (sessionNewNets == 1000 && !milestone1k) {
        milestone1k = true;
        Config::addXP(200);
        Mood::onWardriveMilestone(1000);
    }
}

// process completed scan results
static void processScanResults() {
    int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) return;  // still going

    if (n < 0) {
        // scan failed or not started — clean up and retry. The BLE coordinator
        // still has to hear that the radio is free: its window is gated on the
        // sweep counter, so a run of failed scans would otherwise starve BLE
        // wardriving for the whole session.
        WiFi.scanDelete();
        scanPending = false;
        scanTelemetry.wifiScanPending = false;
        deferScanRetry(millis());  // don't respin the radio on the very next frame
        WardriveBle::onWifiSweepComplete();
        return;
    }

    // borrow Arduino's completed scan list — no duplicate IDF harvest
    const wifi_ap_record_t* apList = nullptr;
    uint16_t apCount = (n > 512) ? 512 : (uint16_t)n;  // sane cap (~40KB PSRAM)
    if (apCount > 0) {
        // Arduino already consumed the ESP-IDF list into its owned buffer.
        apList = static_cast<const wifi_ap_record_t*>(
            WiFiScanClass::getScanInfoByIndex(0));
        if (apList) {
            // feed Recon — WiFi threat analysis at zero radio cost
            DefensePipeline::ingestWardriveSnapshot(apList, apCount);
        } else {
            HAMLET_LOGF("[WARDRIVE] Arduino scan buffer missing for %u APs\n",
                        (unsigned)apCount);
        }
    } else {
        DefensePipeline::ingestWardriveSnapshot(nullptr, 0);
    }

    scanTelemetry.wifiScanPending = false;
    scanTelemetry.wifiResultValid = apCount == 0 || apList != nullptr;
    scanTelemetry.wifiCompletedMs = millis();
    scanTelemetry.wifiNetworks = apList ? apCount : 0;
    scanTelemetry.wifiOpenNetworks = 0;
    scanTelemetry.wifiStrongestRssi = -127;
    scanTelemetry.wifiStrongestChannel = 0;
    scanTelemetry.wifiStrongestSsid[0] = '\0';
    for (uint16_t i = 0; i < apCount && apList; ++i) {
        const wifi_ap_record_t& rec = apList[i];
        if (rec.authmode == WIFI_AUTH_OPEN) {
            ++scanTelemetry.wifiOpenNetworks;
        }
        if (scanTelemetry.wifiStrongestChannel == 0 ||
            rec.rssi > scanTelemetry.wifiStrongestRssi) {
            scanTelemetry.wifiStrongestRssi = rec.rssi;
            scanTelemetry.wifiStrongestChannel = rec.primary;
            copyScanSsid(scanTelemetry.wifiStrongestSsid,
                         sizeof(scanTelemetry.wifiStrongestSsid),
                         reinterpret_cast<const char*>(rec.ssid),
                         sizeof(rec.ssid));
        }
    }

    WigleObservation obs = {};
    bool canLogRows = captureObservation(&obs);
    haveObservation = canLogRows;
    if (canLogRows) {
        currentObservation = obs;
    }
    if (!canLogRows && sdReady && csvHeaderWritten && !gpsHoldoffLogged) {
        HAMLET_LOGLN("[WARDRIVE] waiting for valid GPS fix before writing WiGLE rows");
        gpsHoldoffLogged = true;
    } else if (canLogRows) {
        gpsHoldoffLogged = false;
    }

    for (uint16_t i = 0; i < apCount && apList; i++) {
        const wifi_ap_record_t& rec = apList[i];

        // seen drives stats; needsRow drives CSV so late GPS/SD can still
        // write. Re-check persistability every iteration: a failed flush
        // mid-sweep clears sdReady, and the remaining APs must stay unlogged
        // so SD recovery can pick them up.
        const bool canPersistRow =
            canLogRows && sdReady && csvHeaderWritten && writeBuf;

        DedupTouch dedup = dedupTouch(rec.bssid);
        if (!dedup.firstSeen && !dedup.needsRow) continue;

        if (dedup.firstSeen) {
            noteNewNetwork();
        }

        if (canPersistRow && dedup.needsRow &&
            appendRow(rec.bssid, (const char*)rec.ssid,
                      rec.authmode,
                      rec.primary,
                      rec.rssi)) {
            markDedupRowWritten(dedup);
        }
    }

    WiFi.scanDelete();
    scanPending = false;
    // Always notify BLE that the WiFi sweep ended. A missing result buffer
    // must not leave the coordinator waiting for a window that never opens.
    sessionScanCycles++;
    WardriveBle::onWifiSweepComplete();

    // periodic SD flush
    if (writeBufLen >= FLUSH_THRESH) {
        flushBuffer();
    }
}

// check lifetime achievement
static void checkLifetimeAchievements(uint32_t lifetime) {
    if (lifetime >= 1000) {
        Achievements::tryUnlock(Achievement::GRID_WALKER);
    }
}

// ==[ PUBLIC API ]==

void start() {
    if (active) return;  // idempotent — double start would leak the prior PSRAM allocs

    // alloc PSRAM buffers
    dedupTable = (uint32_t*)heap_caps_malloc(DEDUP_SLOTS * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
    dedupFallback = (uint64_t*)heap_caps_malloc(DEDUP_FALLBACK_SLOTS * sizeof(uint64_t), MALLOC_CAP_SPIRAM);
    dedupLoggedBits = (uint8_t*)heap_caps_malloc((DEDUP_SLOTS + 7) / 8, MALLOC_CAP_SPIRAM);
    dedupFallbackLogged = (bool*)heap_caps_malloc(DEDUP_FALLBACK_SLOTS * sizeof(bool), MALLOC_CAP_SPIRAM);
    writeBuf = (char*)heap_caps_malloc(WRITE_BUF_SZ, MALLOC_CAP_SPIRAM);

    if (dedupTable) memset(dedupTable, 0, DEDUP_SLOTS * sizeof(uint32_t));
    else HAMLET_LOGLN("[WARDRIVE] PSRAM alloc failed: dedupTable");
    if (dedupFallback) memset(dedupFallback, 0xFF, DEDUP_FALLBACK_SLOTS * sizeof(uint64_t));
    else HAMLET_LOGLN("[WARDRIVE] PSRAM alloc failed: dedupFallback");
    if (dedupLoggedBits) memset(dedupLoggedBits, 0, (DEDUP_SLOTS + 7) / 8);
    else HAMLET_LOGLN("[WARDRIVE] PSRAM alloc failed: dedupLoggedBits");
    if (dedupFallbackLogged) memset(dedupFallbackLogged, 0, DEDUP_FALLBACK_SLOTS * sizeof(bool));
    else HAMLET_LOGLN("[WARDRIVE] PSRAM alloc failed: dedupFallbackLogged");
    if (!writeBuf) {
        HAMLET_LOGLN("[WARDRIVE] PSRAM alloc failed: writeBuf — CSV disabled");
        sdReady = false;
    }
    SDStorage::endWriteStream();
    writeBufLen = 0;
    lastFlushMs = millis();

    // reset session state
    active = true;
    paused = false;
    scanPending = false;
    sessionNewNets = 0;
    sessionScanCycles = 0;
    sessionStartTime = millis();
    csvHeaderWritten = false;
    sdFailureLogged = false;
    dedupFallbackFullLogged = false;
    gpsHoldoffLogged = false;
    scanStartFailureLogged = false;
    haveObservation = false;
    c5WardriveHasCoords = false;
    c5WardriveLatitude = 0.0;
    c5WardriveLongitude = 0.0;
    c5WardriveCoordMs = 0;
    lastSDRetry = 0;
    scanRetryAtMs = 0;
    resetDualBandStats();
    resetScanTelemetry();
    milestone50 = milestone100 = milestone500 = milestone1k = false;

    // GPS setup — delegate to GPS:: namespace
    GPS::resetDistance();
    if (Config::getGPSEnabled() && !GPS::isInitialized()) {
        GPS::startUART();
    }

    // SD setup
    sdReady = (writeBuf != nullptr) && SDStorage::isAvailable();
    if (sdReady) {
        generateFilename();
        writeHeader();
        if (csvHeaderWritten) {
            HAMLET_LOGF("[WARDRIVE] CSV: %s\n", csvPath);
        }
    } else {
        HAMLET_LOGLN("[WARDRIVE] no SD — scan-only mode");
    }

    // init BLE interleave — activates Recon wardrive BLE cadence (BLE already warm)
    WardriveBle::init();

    // init WiFi STA for active scan
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(false);
    NowFlock::markEspNowNeedsReinit();

    // BLE coex requires MIN_MODEM (WIFI_PS_NONE + active BLE = abort)
    // Recon BLE is warm → always need MIN_MODEM
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

    // init scene renderer
    WardriveScene::reset();
    WardriveTelemetry::reset();

    // first scan
    startScan();

    HAMLET_LOGF("[WARDRIVE] started. PSRAM dedup=%p buf=%p\n", dedupTable, writeBuf);
}

void stop() {
    if (!active) return;  // idempotent guard

    // shutdown BLE interleave before WiFi teardown
    WardriveBle::shutdown();

    // flush remaining data
    flushBuffer();
    SDStorage::endWriteStream();

    // first-wardrive bonus — gated by NVS session count (survives reboot)
    if (Config::getWDSessions() == 0) {
        Config::addXP(20);
    }

    // finalize session stats
    if (sessionNewNets > 0) {
        Config::addWDTotal(sessionNewNets);
    }
    Config::incrementWDSessions();

    // session active if >=20 new networks
    if (sessionNewNets >= 20) {
        Config::markSessionActive();
    }

    // lifetime achievement check
    checkLifetimeAchievements(Config::getWDTotal());

    // mood debrief
    Mood::onWardriveEnd();

    // free PSRAM — engine buffers + scene buffers
    if (dedupTable) { heap_caps_free(dedupTable); dedupTable = nullptr; }
    if (dedupFallback) { heap_caps_free(dedupFallback); dedupFallback = nullptr; }
    if (dedupLoggedBits) { heap_caps_free(dedupLoggedBits); dedupLoggedBits = nullptr; }
    if (dedupFallbackLogged) { heap_caps_free(dedupFallbackLogged); dedupFallbackLogged = nullptr; }
    if (writeBuf)   { heap_caps_free(writeBuf);   writeBuf = nullptr;   }
    WardriveTelemetry::shutdown();
    WardriveScene::shutdown();

    // kill GPS (respects always-on — deinit() is a no-op if always-on)
    GPS::deinit();
    haveObservation = false;

    // BLE stays warm (Recon never deinited) — safe to fully shut down WiFi
    WiFi.scanDelete();
    WiFi.mode(WIFI_OFF);
    NowFlock::markEspNowNeedsReinit();

    active = false;
    paused = false;
    scanPending = false;
    scanRetryAtMs = 0;
    resetDualBandStats();
    c5WardriveHasCoords = false;
    c5WardriveLatitude = 0.0;
    c5WardriveLongitude = 0.0;
    c5WardriveCoordMs = 0;

    HAMLET_LOGF("[WARDRIVE] stopped. %lu new networks, %d scan cycles\n",
                  (unsigned long)sessionNewNets, sessionScanCycles);
}

void update() {
    if (!active) return;
    if (sdReady && !SDStorage::isAvailable()) {
        noteSDWriteFailure("card unavailable");
    }

    // ==[ SD RECOVERY ]== re-probe every 10s after failure (SDStorage auto-remounts)
    if (writeBuf && !sdReady && millis() - lastSDRetry >= 10000) {
        lastSDRetry = millis();
        if (SDStorage::isAvailable()) {
            sdReady = true;
            sdFailureLogged = false;
            csvHeaderWritten = false;
            generateFilename();
            writeHeader();
            if (csvHeaderWritten) {
                HAMLET_LOGF("[WARDRIVE] SD recovered — new CSV: %s\n", csvPath);
            }
        }
    }

    // GPS::update() runs once per frame in the main loop (hamlet.cpp). The two
    // calls below are not duplicates: harvesting a 512-AP scan and flushing the
    // CSV to SD can each hold this loop for longer than the UART ring buffer
    // covers at 115200, and bytes the driver drops mid-sentence cost a fix.
    WardriveTelemetry::update(millis());
    if (paused) return;

    // poll scan completion
    if (scanPending) {
        GPS::service();
        processScanResults();
    }

    // pump BLE state machine
    WardriveBle::update();

    if (writeBufLen > 0 && millis() - lastFlushMs >= FLUSH_INTERVAL_MS) {
        flushBuffer();
    }

    // restart scan — BLE window takes priority when due
    if (!scanPending) {
        if (WardriveBle::wantsScanWindow()) {
            // BLE wants radio — don't start WiFi scan
        } else {
            startScan();
        }
    }

    // Empty the UART before the cockpit render, the longest phase in the frame.
    GPS::service();
}

bool isActive() { return active; }
bool isPaused() { return paused; }

void togglePause() {
    if (!active) return;
    paused = !paused;
    if (paused) {
        WiFi.scanDelete();
        scanPending = false;
        scanTelemetry.wifiScanPending = false;
        // update() returns before the flush timer while paused, so staged rows
        // would sit in PSRAM for the whole pause — and a pause is exactly when
        // the user is likely to power the device down. Persist them now.
        flushBuffer();
        WardriveBle::onPause();
    } else {
        scanRetryAtMs = 0;  // resume scanning at once, don't serve a stale cool-off
        WardriveBle::onResume();
    }
}

uint32_t getSessionNewNets() { return sessionNewNets; }

uint16_t getSessionScanCycles() { return sessionScanCycles; }

uint32_t getSessionElapsedMs() {
    if (!active || sessionStartTime == 0) return 0;
    return millis() - sessionStartTime;
}

float getSessionDistanceKm() {
    return GPS::getDistanceKm();
}

uint32_t getLifetimeNets() {
    return Config::getWDTotal() + (active ? sessionNewNets : 0);
}

void getScanTelemetry(ScanTelemetry& out) {
    out = scanTelemetry;
    out.wifiScanPending = scanPending;
    if (!WardrivePolicy::isFreshC5ScanTelemetry(
            hasC5WardriveConnection(), out.c5CompletedMs, millis())) {
        out.c5ResultValid = false;
        out.c5Networks5GHz = 0;
    }
}

bool isSDReady() { return sdReady; }

float getSDFreeGB() {
    if (!sdReady) return 0.0f;
    return SDStorage::freeBytes() / (1024.0f * 1024.0f * 1024.0f);
}

uint16_t getSessionNewBleDevices() { return WardriveBle::getSessionNewDevices(); }
bool isBleEnabled() { return WardriveBle::isEnabled(); }

bool isGPSRunning() { return GPS::isInitialized(); }
bool hasGPSData() { return GPS::hasUARTData(); }
bool hasGPSNMEA() { return GPS::hasNMEA(); }
bool hasGPSFix() { return GPS::hasFix(); }
bool hasC5WardriveCoords() { return c5WardriveHasCoords; }
bool isUsingFreshC5WardriveCoords() {
    return isFreshC5WardriveCoords(millis());
}
bool hasC5WardriveConnection() {
    return Config::getC5Enabled() && C5Monster::isConnected();
}
double getC5WardriveLatitude() { return c5WardriveLatitude; }
double getC5WardriveLongitude() { return c5WardriveLongitude; }
uint32_t getC5WardriveCoordAgeMs() {
    if (!c5WardriveHasCoords || c5WardriveCoordMs == 0) return UINT32_MAX;
    return millis() - c5WardriveCoordMs;
}
float getSpeedKmh() { return GPS::getSpeedKmh(); }
double getLatitude() { return GPS::getLatitude(); }
double getLongitude() { return GPS::getLongitude(); }
float getAltitude() { return GPS::getAltitude(); }
uint8_t getSatCount() { return GPS::getSatCount(); }

// ==[ DUAL-BAND: 5GHz via C5Monster ]==

static bool wdDualBandActive = false;
static uint32_t wd5GHzNetworks = 0;

// Both are snapshots of the last C5 scan, not cumulative counters. Without an
// explicit reset the previous session's 5GHz count stays on the HUD until the
// first C5 scan of the new session lands.
static void resetDualBandStats() {
    wdDualBandActive = false;
    wd5GHzNetworks = 0;
    scanTelemetry.c5ResultValid = false;
    scanTelemetry.c5CompletedMs = 0;
    scanTelemetry.c5Networks5GHz = 0;
    scanTelemetry.c5StrongestRssi = -127;
    scanTelemetry.c5StrongestChannel = 0;
}

static wifi_auth_mode_t c5AuthMode(uint8_t authType) {
    switch (authType) {
        case C5Protocol::AUTH_OPEN: return WIFI_AUTH_OPEN;
        case C5Protocol::AUTH_WEP: return WIFI_AUTH_WEP;
        case C5Protocol::AUTH_WPA: return WIFI_AUTH_WPA_PSK;
        case C5Protocol::AUTH_WPA2: return WIFI_AUTH_WPA2_PSK;
        case C5Protocol::AUTH_WPA3: return WIFI_AUTH_WPA3_PSK;
        case C5Protocol::AUTH_WPA_WPA2_MIXED:
            return WIFI_AUTH_WPA_WPA2_PSK;
        case C5Protocol::AUTH_WPA2_WPA3_MIXED:
            return WIFI_AUTH_WPA2_WPA3_PSK;
        default: return WIFI_AUTH_MAX;
    }
}

void feedC5MonsterGPSFix(const C5Monster::GPSFix& fix) {
    if (fix.timestampMs == 0 ||
        fix.latitude < -90.0 || fix.latitude > 90.0 ||
        fix.longitude < -180.0 || fix.longitude > 180.0) {
        return;
    }
    c5WardriveHasCoords = true;
    c5WardriveLatitude = fix.latitude;
    c5WardriveLongitude = fix.longitude;
    c5WardriveCoordMs = fix.timestampMs;
}

void feedC5MonsterScan(const C5Monster::ScanResults& results) {
    wdDualBandActive = C5Monster::isConnected();
    // Snapshot: count 5GHz networks in this scan (not cumulative).
    wd5GHzNetworks = 0;
    scanTelemetry.c5ResultValid = true;
    scanTelemetry.c5CompletedMs = results.timestampMs ? results.timestampMs : millis();
    scanTelemetry.c5Networks5GHz = 0;
    scanTelemetry.c5StrongestRssi = -127;
    scanTelemetry.c5StrongestChannel = 0;
    bool hasCoords = false;
    double lat = 0.0;
    double lon = 0.0;

    for (uint8_t i = 0; i < results.count; i++) {
        const auto& entry = results.entries[i];
        if (WardrivePolicy::isC5WardriveChannel(
                entry.is5GHz, entry.channel)) {
            wd5GHzNetworks++;
            ++scanTelemetry.c5Networks5GHz;
            if (scanTelemetry.c5StrongestChannel == 0 ||
                entry.rssi > scanTelemetry.c5StrongestRssi) {
                scanTelemetry.c5StrongestRssi = entry.rssi;
                scanTelemetry.c5StrongestChannel = entry.channel;
            }
        }
        if (!hasCoords && entry.hasGPS &&
            entry.latitude >= -90.0 && entry.latitude <= 90.0 &&
            entry.longitude >= -180.0 && entry.longitude <= 180.0) {
            lat = entry.latitude;
            lon = entry.longitude;
            hasCoords = true;
        }
    }
    if (hasCoords) {
        c5WardriveHasCoords = true;
        c5WardriveLatitude = lat;
        c5WardriveLongitude = lon;
        c5WardriveCoordMs = results.timestampMs ? results.timestampMs : millis();
    }
    // Keep last valid C5 coords for the duration of TTL.
    // A miss in a single scan must not instantly force GPS fallback.

    // The Core owns the Wardrive file even when the C5 supplies 5GHz scans.
    // JanOS show_scan_results rows normally have no coordinates, so prefer
    // optional fresh C5 metadata when present and otherwise use the Core GPS.
    // If neither source is ready, do not mark the BSSID logged: a later scan
    // can still persist it after a fix or SD recovery.
    if (!active || paused) return;

    WigleObservation obs = {};
    const bool canLogRows = captureObservation(&obs);
    haveObservation = canLogRows;
    if (canLogRows) {
        currentObservation = obs;
        gpsHoldoffLogged = false;
    } else if (sdReady && csvHeaderWritten && !gpsHoldoffLogged) {
        HAMLET_LOGLN("[WARDRIVE] C5 rows waiting for valid coordinates");
        gpsHoldoffLogged = true;
    }

    for (uint8_t i = 0; i < results.count; i++) {
        const C5Protocol::ScanEntry& entry = results.entries[i];
        if (!WardrivePolicy::isC5WardriveChannel(
                entry.is5GHz, entry.channel)) {
            continue;
        }

        const bool canPersistRow =
            canLogRows && sdReady && csvHeaderWritten && writeBuf;

        DedupTouch dedup = dedupTouch(entry.bssid);
        if (dedup.firstSeen) {
            noteNewNetwork();
        }
        if (canPersistRow && dedup.needsRow &&
            appendRow(entry.bssid, entry.ssid, c5AuthMode(entry.authType),
                      entry.channel, entry.rssi)) {
            markDedupRowWritten(dedup);
        }
    }
}

bool isDualBandActive() {
    return Config::getC5Enabled() && wdDualBandActive && C5Monster::isConnected();
}

uint32_t getSession5GHzNetworks() {
    return isDualBandActive() ? wd5GHzNetworks : 0;
}

} // namespace Wardrive
