/**
 * recon_ble.cpp — BLE scanning, classification, and catalog management
 *
 * Extracted from recon.cpp. NimBLE-based tracker detection: AirTag, SmartTag,
 * Tile, FastPair, Flipper, iBeacon, Eddystone, HID, spam detection.
 * PSRAM-backed catalog with ranked eviction.
 */

#include "recon_internal.h"
#include "recon_ble_math.h"
#include "../activity/pedometer.h"
#include "../core/gps.h"
#include "../util/debug_log.h"
#include <string.h>

#if RECON_BLE_ENABLED
#include <NimBLEDevice.h>

// btInUse() override — prevent Arduino from releasing BT controller memory
extern "C" bool btInUse() { return true; }

extern void bleChaff_stop();

namespace Recon {

// The NimBLE host task coalesces observations while the main loop reads live
// pinned-device updates. Protect the multi-field PSRAM records themselves;
// an atomic row count alone cannot prevent a torn observation copy.
static portMUX_TYPE bleObservationMux = portMUX_INITIALIZER_UNLOCKED;

class BleObservationGuard {
public:
    BleObservationGuard() { portENTER_CRITICAL(&bleObservationMux); }
    ~BleObservationGuard() { portEXIT_CRITICAL(&bleObservationMux); }

    BleObservationGuard(const BleObservationGuard&) = delete;
    BleObservationGuard& operator=(const BleObservationGuard&) = delete;
};

// forward declaration — defined later in this file
static void classifyBLEDevice(const uint8_t* addr, int8_t rssi, const uint8_t* advData, int advLen,
                              uint8_t advType, uint8_t addrType);

// ==[ NimBLE SCAN CALLBACKS ]== runs in NimBLE host task, keep fast
class ReconScanCallbacks : public NimBLEAdvertisedDeviceCallbacks {
    void onResult(NimBLEAdvertisedDevice* device) override {
        if (bleScanBufCount.load(std::memory_order_relaxed) < BLE_SCAN_BUF_SIZE) {
            classifyBLEDevice(
                device->getAddress().getNative(),
                (int8_t)device->getRSSI(),
                device->getPayload(),
                (int)device->getPayloadLength(),
                device->getAdvType(),
                device->getAddressType()
            );
        }
    }
};
static ReconScanCallbacks scanCallbacks;

static void onScanCompleteCb(NimBLEScanResults results) {
    bleScanActive = false;
}

// ==[ FORWARD DECLARATIONS ]== BLE-internal functions
static void classifyBLEDevice(const uint8_t* addr, int8_t rssi, const uint8_t* advData, int advLen,
                              uint8_t advType, uint8_t addrType);
static bool recordUniqueMac(uint8_t (*table)[6], uint8_t& count, uint8_t capacity, const uint8_t* mac);
static void copyBLEName(char* dst, size_t dstSize, const uint8_t* src, int srcLen);
static void mergeBLEName(char* dst, size_t dstSize, const char* src);
static void copyBLEPayloadPreview(uint8_t* dst, uint8_t& dstLen, const uint8_t* src, int srcLen);
static void accumulateBLEHit(const BLERawHit& sample);
static bool isPinnedBlePayload(const uint8_t* payloadHash);
static uint8_t bleCatalogEntryRank(const TrackerEntry& dev);
static uint8_t bleCatalogIncomingRank(const BLERawHit& hit);
static int8_t bleCatalogEntrySignal(const TrackerEntry& dev);
static int bleCatalogUnknownBudget();
static int8_t bleCatalogUnknownMinRssi();
static int bleCatalogUnknownCount();
static int findBleCatalogVictim(const BLERawHit& hit, uint8_t incomingRank, bool unknownOnly);
static void writeBleCatalogEntry(TrackerEntry& dev, const BLERawHit& hit, uint32_t now);
static void upsertBLECatalogEntry(const BLERawHit& hit, uint32_t now);


// ==[ BLE HELPER FUNCTIONS — CATALOG MANAGEMENT ]==

static bool recordUniqueMac(uint8_t (*table)[6], uint8_t& count, uint8_t capacity, const uint8_t* mac) {
    if (!mac) return false;
    for (uint8_t i = 0; i < count; i++) {
        if (memcmp(table[i], mac, 6) == 0) return false;
    }
    if (count >= capacity) return false;
    memcpy(table[count], mac, 6);
    count++;
    return true;
}

static void copyBLEName(char* dst, size_t dstSize, const uint8_t* src, int srcLen) {
    if (!dst || dstSize == 0) return;
    dst[0] = '\0';
    if (!src || srcLen <= 0) return;

    size_t out = 0;
    for (int i = 0; i < srcLen && out + 1 < dstSize; i++) {
        char c = (char)src[i];
        if (c == '\0') break;
        if ((uint8_t)c < 32 || (uint8_t)c > 126) c = '?';
        dst[out++] = c;
    }
    dst[out] = '\0';
}

static void mergeBLEName(char* dst, size_t dstSize, const char* src) {
    if (!dst || dstSize == 0 || !src || !src[0]) return;
    if (!dst[0] || strlen(src) > strlen(dst)) {
        strncpy(dst, src, dstSize - 1);
        dst[dstSize - 1] = '\0';
    }
}

static void copyBLEPayloadPreview(uint8_t* dst, uint8_t& dstLen, const uint8_t* src, int srcLen) {
    dstLen = 0;
    if (!dst || !src || srcLen <= 0) return;
    size_t copyLen = (srcLen < (int)BLE_PAYLOAD_PREVIEW_MAX) ? (size_t)srcLen : BLE_PAYLOAD_PREVIEW_MAX;
    memcpy(dst, src, copyLen);
    if (copyLen < BLE_PAYLOAD_PREVIEW_MAX) {
        memset(dst + copyLen, 0, BLE_PAYLOAD_PREVIEW_MAX - copyLen);
    }
    dstLen = (uint8_t)copyLen;
}

static void accumulateBLEHit(const BLERawHit& sample) {
    if (!bleScanBuf || !sample.valid) return;
    const BleObservationGuard guard;

    for (int i = 0; i < bleScanBufCount.load(std::memory_order_relaxed); i++) {
        BLERawHit& hit = bleScanBuf[i];
        if (!hit.valid) continue;
        if (memcmp(hit.payloadHash, sample.payloadHash, 4) != 0) continue;

        if (hit.repeatCount < UINT16_MAX) hit.repeatCount++;
        if (hit.lastAdvTimestamp != 0) {
            const uint32_t delta = sample.timestampMs - hit.lastAdvTimestamp;
            if (delta > 50 && delta < 30000) {
                if (hit.measuredAdvIntervalMs == 0) {
                    hit.measuredAdvIntervalMs = (uint16_t)delta;
                } else {
                    const int32_t diff = (int32_t)delta - (int32_t)hit.measuredAdvIntervalMs;
                    const uint32_t absDiff = diff < 0 ? (uint32_t)-diff : (uint32_t)diff;
                    hit.intervalVariance = (uint16_t)(
                        hit.intervalVariance * 7 / 10 + absDiff * 3 / 10);
                    hit.measuredAdvIntervalMs = (uint16_t)(
                        hit.measuredAdvIntervalMs * 7 / 10 + delta * 3 / 10);
                }
            }
        }
        hit.lastAdvTimestamp = sample.timestampMs;
        hit.timestampMs = sample.timestampMs;

        const int8_t previousRssi = hit.rssi;
        if (sample.rssi > previousRssi) {
            hit.rssi = sample.rssi;
            memcpy(hit.addr, sample.addr, 6);
        }
        if (hit.type == ThreatType::GENERIC_TRACKER && sample.type != ThreatType::GENERIC_TRACKER) {
            hit.type = sample.type;
        } else if (hit.type == ThreatType::UNKNOWN) {
            hit.type = sample.type;
        }
        if (sample.txPower != -127 && (hit.txPower == -127 || sample.rssi >= hit.rssi)) hit.txPower = sample.txPower;
        hit.advFlags |= sample.advFlags;
        hit.advType = sample.advType;
        hit.addrType = sample.addrType;
        if (sample.companyId != 0) hit.companyId = sample.companyId;
        if (sample.primaryService != 0) hit.primaryService = sample.primaryService;
        if (sample.major != 0) hit.major = sample.major;
        if (sample.minor != 0) hit.minor = sample.minor;
        if (sample.frameType != 0 || hit.frameType == 0) hit.frameType = sample.frameType;
        if (sample.spamPlatform != 0) hit.spamPlatform = sample.spamPlatform;
        if (sample.payloadLen > hit.payloadLen) hit.payloadLen = sample.payloadLen;
        if (sample.serviceCount > hit.serviceCount) hit.serviceCount = sample.serviceCount;
        if (sample.manufacturerCount > hit.manufacturerCount) hit.manufacturerCount = sample.manufacturerCount;
        if (ReconBleMath::shouldReplacePayloadPreview(
                sample.payloadPreviewLen, hit.payloadPreviewLen,
                sample.rssi, previousRssi)) {
            hit.payloadPreviewLen = sample.payloadPreviewLen;
            memcpy(hit.payloadPreview, sample.payloadPreview, BLE_PAYLOAD_PREVIEW_MAX);
        }
        mergeBLEName(hit.name, sizeof(hit.name), sample.name);
        recordUniqueMac(hit.macs, hit.macCount, BLE_SCAN_MACS_PER_HIT, sample.addr);
        return;
    }

    // Atomically claim a slot — prevents TOCTOU race when two NimBLE
    // callbacks run concurrently on ESP32 dual-core.
    int idx = bleScanBufCount.fetch_add(1, std::memory_order_relaxed);
    if (idx >= BLE_SCAN_BUF_SIZE) {
        bleScanBufCount.fetch_sub(1, std::memory_order_relaxed);  // undo claim
        return;
    }

    BLERawHit& hit = bleScanBuf[idx];
    hit = sample;
    hit.repeatCount = 1;
    hit.lastAdvTimestamp = sample.timestampMs;
    recordUniqueMac(hit.macs, hit.macCount, BLE_SCAN_MACS_PER_HIT, sample.addr);
}

static bool isPinnedBlePayload(const uint8_t* payloadHash) {
    if (blePinnedPayloadValid && payloadHash &&
        memcmp(payloadHash, blePinnedPayloadHash, sizeof(blePinnedPayloadHash)) == 0)
        return true;
    return Recon::isWatchlisted(payloadHash);
}

static uint8_t bleCatalogEntryRank(const TrackerEntry& dev) {
    if (isPinnedBlePayload(dev.payloadHash)) return 4;
    if (dev.flags & (FLAG_FOLLOWING | FLAG_SPAM)) return 3;
    if (dev.type != ThreatType::UNKNOWN) return 2;
    return 1;
}

static uint8_t bleCatalogIncomingRank(const BLERawHit& hit) {
    if (isPinnedBlePayload(hit.payloadHash)) return 4;
    if (hit.type != ThreatType::UNKNOWN) return 2;
    return 1;
}

static int8_t bleCatalogEntrySignal(const TrackerEntry& dev) {
    return (dev.seenCount > 1) ? dev.rssiSmooth : dev.rssi;
}

static int bleCatalogUnknownBudget() {
    return blePriorityMode ? BLE_UNKNOWN_PRIORITY_LIMIT : BLE_UNKNOWN_BG_LIMIT;
}

static int8_t bleCatalogUnknownMinRssi() {
    return blePriorityMode ? BLE_UNKNOWN_PRIORITY_MIN_RSSI : BLE_UNKNOWN_BG_MIN_RSSI;
}

static int bleCatalogUnknownCount() {
    int count = 0;
    int dc = bleDeviceCount.load(std::memory_order_relaxed);
    for (int i = 0; i < dc; i++) {
        if (bleCatalogEntryRank(bleDeviceTable[i]) == 1) count++;
    }
    return count;
}

static int findBleCatalogVictim(const BLERawHit& hit, uint8_t incomingRank, bool unknownOnly) {
    int victim = -1;
    uint8_t victimRank = 0;
    int8_t victimSignal = 127;
    uint32_t victimLastSeen = 0;

    int dc = bleDeviceCount.load(std::memory_order_relaxed);
    for (int i = 0; i < dc; i++) {
        const TrackerEntry& dev = bleDeviceTable[i];
        uint8_t rank = bleCatalogEntryRank(dev);
        if (isPinnedBlePayload(dev.payloadHash)) continue;
        if (unknownOnly && rank != 1) continue;
        if (!unknownOnly && rank > incomingRank) continue;

        int8_t signal = bleCatalogEntrySignal(dev);
        if (victim < 0 ||
            rank < victimRank ||
            (rank == victimRank && signal < victimSignal) ||
            (rank == victimRank && signal == victimSignal && dev.lastSeen < victimLastSeen)) {
            victim = i;
            victimRank = rank;
            victimSignal = signal;
            victimLastSeen = dev.lastSeen;
        }
    }

    if (victim < 0) return -1;
    if (victimRank == incomingRank &&
        hit.rssi < (int8_t)(victimSignal + BLE_CATALOG_REPLACE_MARGIN_DB)) {
        return -1;
    }
    return victim;
}

static void writeBleCatalogEntry(TrackerEntry& dev, const BLERawHit& hit, uint32_t now) {
    memset(&dev, 0, sizeof(TrackerEntry));
    memcpy(dev.mac, hit.addr, 6);
    memcpy(dev.payloadHash, hit.payloadHash, 4);
    dev.type = hit.type;
    dev.rssi = hit.rssi;
    dev.rssiSmooth = hit.rssi;
    dev.txPower = hit.txPower;
    dev.seenCount = 1;
    dev.identityCandidates = max((uint8_t)1u, hit.macCount);
    dev.advFlags = hit.advFlags;
    dev.advType = hit.advType;
    dev.addrType = hit.addrType;
    dev.frameType = hit.frameType;
    dev.companyId = hit.companyId;
    dev.primaryService = hit.primaryService;
    dev.appearance = hit.appearance;
    dev.major = hit.major;
    dev.minor = hit.minor;
    dev.firstSeen = now;
    dev.lastSeen = now;
    dev.spamPlatform = hit.spamPlatform;
    dev.firstDetectDist = (uint16_t)min((uint32_t)Pedometer::getDistance(), (uint32_t)65535);
    if (GPS::hasFix()) {
        dev.lastLat = (float)GPS::getLatitude();
        dev.lastLon = (float)GPS::getLongitude();
    }
    dev.classOfDevice = hit.classOfDevice;
    dev.advInterval = hit.advInterval;
    dev.companyId2 = hit.companyId2;
    dev.payloadLen = hit.payloadLen;
    dev.serviceCount = hit.serviceCount;
    dev.manufacturerCount = hit.manufacturerCount;
    dev.payloadPreviewLen = hit.payloadPreviewLen;
    memcpy(dev.payloadPreview, hit.payloadPreview, BLE_PAYLOAD_PREVIEW_MAX);
    if (hit.name[0]) {
        strncpy(dev.name, hit.name, sizeof(dev.name) - 1);
        dev.name[sizeof(dev.name) - 1] = '\0';
    }
    if (hit.macCount > 1) {
        dev.macChangeCount = hit.macCount - 1;
        dev.lastMacChange = now;
    }
}

static void upsertBLECatalogEntry(const BLERawHit& hit, uint32_t now) {
    if (!bleDeviceTable || !hit.valid) return;

    int devCount = bleDeviceCount.load(std::memory_order_acquire);
    int existing = -1;
    for (int i = 0; i < devCount; i++) {
        if (memcmp(bleDeviceTable[i].payloadHash, hit.payloadHash, 4) == 0) {
            existing = i;
            break;
        }
    }

    if (existing < 0) {
        uint8_t incomingRank = bleCatalogIncomingRank(hit);
        bool unknownOnly = (incomingRank == 1);

        if (unknownOnly && hit.rssi <= bleCatalogUnknownMinRssi()) return;

        if (devCount < MAX_BLE_DEVICES &&
            (!unknownOnly || bleCatalogUnknownCount() < bleCatalogUnknownBudget())) {
            existing = devCount;
            bleDeviceCount.store(devCount + 1, std::memory_order_release);
        } else {
            existing = findBleCatalogVictim(hit, incomingRank, unknownOnly);
            if (existing < 0) return;
        }

        writeBleCatalogEntry(bleDeviceTable[existing], hit, now);
        return;
    }

    TrackerEntry& dev = bleDeviceTable[existing];
    int8_t prevRssi = dev.rssi;
    if (dev.type == ThreatType::UNKNOWN && hit.type != ThreatType::UNKNOWN) dev.type = hit.type;
    dev.rssi = hit.rssi;
    if (dev.seenCount < 255) dev.seenCount++;
    dev.lastSeen = now;
    dev.identityCandidates = max((uint8_t)1u, hit.macCount);
    if (GPS::hasFix()) {
        dev.lastLat = (float)GPS::getLatitude();
        dev.lastLon = (float)GPS::getLongitude();
    }
    dev.advFlags |= hit.advFlags;
    dev.advType = hit.advType;
    dev.addrType = hit.addrType;
    if (hit.txPower != -127 && (dev.txPower == -127 || hit.rssi >= prevRssi)) dev.txPower = hit.txPower;
    if (hit.companyId != 0) dev.companyId = hit.companyId;
    if (hit.primaryService != 0) dev.primaryService = hit.primaryService;
    if (hit.appearance != 0) dev.appearance = hit.appearance;
    if (hit.classOfDevice && !dev.classOfDevice) dev.classOfDevice = hit.classOfDevice;
    if (hit.advInterval && !dev.advInterval) dev.advInterval = hit.advInterval;
    if (hit.companyId2 && !dev.companyId2) dev.companyId2 = hit.companyId2;
    if (hit.major != 0) dev.major = hit.major;
    if (hit.minor != 0) dev.minor = hit.minor;
    if (hit.frameType != 0 || dev.frameType == 0) dev.frameType = hit.frameType;
    if (hit.spamPlatform != 0) dev.spamPlatform = hit.spamPlatform;
    if (hit.payloadLen > dev.payloadLen) dev.payloadLen = hit.payloadLen;
    if (hit.serviceCount > dev.serviceCount) dev.serviceCount = hit.serviceCount;
    if (hit.manufacturerCount > dev.manufacturerCount) dev.manufacturerCount = hit.manufacturerCount;
    if (hit.payloadPreviewLen > dev.payloadPreviewLen ||
        (hit.rssi > prevRssi && hit.payloadPreviewLen > 0)) {
        dev.payloadPreviewLen = hit.payloadPreviewLen;
        memcpy(dev.payloadPreview, hit.payloadPreview, BLE_PAYLOAD_PREVIEW_MAX);
    }
    if (hit.name[0] && (!dev.name[0] || strlen(hit.name) > strlen(dev.name))) {
        strncpy(dev.name, hit.name, sizeof(dev.name) - 1);
        dev.name[sizeof(dev.name) - 1] = '\0';
    }

    uint8_t macChanges = 0;
    for (uint8_t m = 0; m < hit.macCount; m++) {
        if (memcmp(hit.macs[m], dev.mac, 6) != 0) macChanges++;
    }
    if (macChanges > 0) {
        memcpy(dev.mac, hit.addr, 6);
        dev.lastMacChange = now;
        uint16_t nextCount = dev.macChangeCount + macChanges;
        dev.macChangeCount = (nextCount > 255) ? 255 : (uint8_t)nextCount;
    }

    dev.rssiSmooth = (int8_t)(dev.rssiSmooth * 0.7f + hit.rssi * 0.3f);
}

void syncBLECatalogThreatState() {
    if (!bleDeviceTable) return;

    int dc = bleDeviceCount.load(std::memory_order_relaxed);
    for (int i = 0; i < dc; i++) {
        bleDeviceTable[i].flags &= (uint8_t)~(FLAG_ALERTED | FLAG_FOLLOWING | FLAG_SPAM | FLAG_STEP_FOLLOWING | FLAG_WATCHLISTED);
    }

    for (int t = 0; t < trackerCount; t++) {
        const TrackerEntry& src = trackerTable[t];
        for (int i = 0; i < dc; i++) {
            if (memcmp(bleDeviceTable[i].payloadHash, src.payloadHash, 4) != 0) continue;
            bleDeviceTable[i].type = src.type;
            bleDeviceTable[i].flags = src.flags;
            bleDeviceTable[i].rssi = src.rssi;
            bleDeviceTable[i].rssiSmooth = src.rssiSmooth;
            bleDeviceTable[i].macChangeCount = src.macChangeCount;
            bleDeviceTable[i].lastMacChange = src.lastMacChange;
            bleDeviceTable[i].firstSeen = src.firstSeen;
            bleDeviceTable[i].lastSeen = src.lastSeen;
            bleDeviceTable[i].identityCandidates = src.identityCandidates;
            memcpy(bleDeviceTable[i].mac, src.mac, 6);
            break;
        }
    }

    // sync watchlist flag
    for (int i = 0; i < dc; i++) {
        if (Recon::isWatchlisted(bleDeviceTable[i].payloadHash)) {
            bleDeviceTable[i].flags |= FLAG_WATCHLISTED;
        }
    }
}

// ==[ CLASSIFY BLE DEVICE ]== parse advertisement data for tracker signatures
static void classifyBLEDevice(const uint8_t* addr, int8_t rssi,
                              const uint8_t* advData, int advLen,
                              uint8_t advType, uint8_t addrType) {
    if (!advData || advLen < 4) return;

    if (bleSeenMacs &&
        recordUniqueMac(bleSeenMacs, bleSeenMacCount, BLE_SCAN_UNIQUE_MAC_CAP, addr) &&
        totalBLEDevices < 0xFFFF) {
        totalBLEDevices++;
    }

    BLERawHit sample = {};
    sample.timestampMs = millis();
    memcpy(sample.addr, addr, 6);
    sample.rssi = rssi;
    sample.type = ThreatType::UNKNOWN;
    sample.txPower = -127;
    sample.advType = advType;
    sample.addrType = addrType;
    sample.payloadLen = (advLen > 255) ? 255 : (uint8_t)advLen;
    copyBLEPayloadPreview(sample.payloadPreview, sample.payloadPreviewLen, advData, advLen);
    sample.valid = true;

    ThreatType detected = ThreatType::UNKNOWN;
    const uint8_t* identPayload = advData;  // for hash
    int identLen = advLen;

    // walk AD structures: [len][type][data...]
    int pos = 0;
    while (pos < advLen - 1) {
        uint8_t adLen = advData[pos];
        if (adLen == 0 || pos + adLen >= advLen) break;
        uint8_t adType = advData[pos + 1];
        const uint8_t* adData = &advData[pos + 2];
        int adDataLen = adLen - 1;

        if (adType == 0x01 && adDataLen >= 1) {
            sample.advFlags |= adData[0];
        } else if ((adType == 0x08 || adType == 0x09) && adDataLen > 0) {
            char parsedName[BLE_NAME_MAX] = {};
            copyBLEName(parsedName, sizeof(parsedName), adData, adDataLen);
            if (adType == 0x09 || !sample.name[0]) mergeBLEName(sample.name, sizeof(sample.name), parsedName);
        } else if (adType == 0x0A && adDataLen >= 1) {
            sample.txPower = (int8_t)adData[0];
        } else if (adType == 0x19 && adDataLen >= 2) {
            sample.appearance = adData[0] | (adData[1] << 8);  // GAP appearance (little-endian)
        } else if (adType == 0x0D && adDataLen >= 3) {
            sample.classOfDevice = adData[0] | (adData[1] << 8) | (adData[2] << 16);
        } else if (adType == 0x1A && adDataLen >= 2) {
            sample.advInterval = adData[0] | (adData[1] << 8);
        }

        // ==[ MANUFACTURER SPECIFIC DATA (0xFF) ]==
        if (adType == 0xFF && adDataLen >= 4) {
            uint16_t companyId = adData[0] | (adData[1] << 8);
            if (sample.manufacturerCount < 255) sample.manufacturerCount++;
            if (sample.companyId == 0) sample.companyId = companyId;
            else if (sample.companyId2 == 0 && companyId != sample.companyId)
                sample.companyId2 = companyId;

            // Apple FindMy tracker (AirTag, AirPods, etc.)
            if (companyId == APPLE_COMPANY_ID && adDataLen >= 3) {
                uint8_t subtype = adData[2];
                sample.frameType = subtype;
                if (subtype == APPLE_FINDMY_TYPE) {
                    detected = ThreatType::AIRTAG;
                    identPayload = adData + 3;  // rotating key
                    identLen = adDataLen - 3;
                }
                else if (subtype == APPLE_IBEACON_TYPE && adDataLen >= 25) {
                    detected = ThreatType::IBEACON;
                    identPayload = adData + 4;   // UUID starts after subtype + len
                    identLen = 20;               // UUID(16) + major(2) + minor(2)
                    sample.major = (uint16_t)((adData[20] << 8) | adData[21]);
                    sample.minor = (uint16_t)((adData[22] << 8) | adData[23]);
                    if (sample.txPower == -127) sample.txPower = (int8_t)adData[24];
                }
                else if (subtype == APPLE_NEARBY_ACTION_TYPE ||
                         subtype == APPLE_ACTION_MODAL_TYPE) {
                    // spam indicator — popup/modal attack
                    sample.spamPlatform = (uint8_t)SpamPlatform::IOS;
                    if (bleContinuityMacs &&
                        recordUniqueMac(bleContinuityMacs, bleContinuityMacCount,
                                        BLE_SCAN_UNIQUE_MAC_CAP, addr) &&
                        appleContinuityCount < 0xFFFF) {
                        appleContinuityCount++;
                    }
                }
                else if (subtype == APPLE_NEARBY_TYPE ||
                         subtype == APPLE_AIRDROP_TYPE ||
                         subtype == APPLE_HANDOFF_TYPE ||
                         subtype == APPLE_HOTSPOT_TYPE) {
                    if (bleContinuityMacs &&
                        recordUniqueMac(bleContinuityMacs, bleContinuityMacCount,
                                        BLE_SCAN_UNIQUE_MAC_CAP, addr) &&
                        appleContinuityCount < 0xFFFF) {
                        appleContinuityCount++;
                    }
                    // don't set detected — stays UNKNOWN, won't enter tracker table
                }
            }
            // Samsung mfg data — DON'T classify as SMARTTAG here.
            // Real SmartTags identified by service UUID 0xFD5A (below).
            else if (companyId == 0x0075 && adDataLen >= 4) {
                // Samsung Buds popup spam pattern: 75 00 42 09
                if (adData[2] == 0x42 && adData[3] == 0x09)
                    sample.spamPlatform = (uint8_t)SpamPlatform::SAMSUNG;
            }
            // Flipper Zero: company ID 0x038F
            else if (companyId == FLIPPER_COMPANY_ID) {
                detected = ThreatType::FLIPPER;
                identPayload = adData + 2;
                identLen = adDataLen - 2;
            }
            // Microsoft SwiftPair spam: company 0x0006, payload 03 00 80
            else if (companyId == 0x0006 && adDataLen >= 5 &&
                     adData[2] == 0x03 && adData[3] == 0x00 && adData[4] == 0x80) {
                sample.spamPlatform = (uint8_t)SpamPlatform::WINDOWS;
            }
        }
        // ==[ SERVICE DATA (0x16) ]==
        else if (adType == 0x16 && adDataLen >= 2) {
            uint16_t svcUUID = adData[0] | (adData[1] << 8);
            if (sample.serviceCount < 255) sample.serviceCount++;
            if (sample.primaryService == 0) sample.primaryService = svcUUID;

            if (svcUUID == SAMSUNG_SVC_UUID) {
                detected = ThreatType::SMARTTAG;
                identPayload = adData + 2;
                identLen = adDataLen - 2;
            } else if (svcUUID == TILE_SVC_UUID) {
                detected = ThreatType::TILE;
                identPayload = adData + 2;
                identLen = adDataLen - 2;
            } else if (svcUUID == FASTPAIR_SVC_UUID) {
                // FMDN: frame type 0x40-0x45, regular FastPair: model ID (3 bytes)
                if (adDataLen >= 3 && adData[2] >= 0x40 && adData[2] <= 0x45) {
                    detected = ThreatType::FMDN;
                    sample.frameType = adData[2];
                } else {
                    detected = ThreatType::FAST_PAIR;
                }
                identPayload = adData + 2;
                identLen = adDataLen - 2;
            } else if (svcUUID == EDDYSTONE_SVC_UUID) {
                detected = ThreatType::EDDYSTONE;
                identPayload = adData + 2;
                identLen = adDataLen - 2;
                if (adDataLen >= 3) sample.frameType = adData[2];
                if (sample.txPower == -127 && adDataLen >= 4) sample.txPower = (int8_t)adData[3];
            } else if (svcUUID == HM10_SVC_UUID) {
                detected = ThreatType::SUSPICIOUS_PERIPHERAL;
                identPayload = adData + 2;
                identLen = adDataLen - 2;
            } else if (svcUUID == SAMSUNG_UNREG_SVC_UUID) {
                detected = ThreatType::SMARTTAG_UNREGISTERED;
                identPayload = adData + 2;
                identLen = adDataLen - 2;
            } else if (svcUUID == BLE_HID_SVC_UUID) {
                detected = ThreatType::HID_DEVICE;
                identPayload = adData + 2;
                identLen = adDataLen - 2;
            } else if (svcUUID == FLIPPER_SVC_BLACK ||
                       svcUUID == FLIPPER_SVC_WHITE ||
                       svcUUID == FLIPPER_SVC_TRANSPARENT) {
                detected = ThreatType::FLIPPER;
                identPayload = adData + 2;
                identLen = adDataLen - 2;
            } else if (svcUUID == XIAOMI_MIBEACON_SVC_UUID) {
                detected = ThreatType::XIAOMI_TRACKER;
                identPayload = adData + 2;
                identLen = adDataLen - 2;
            } else if (svcUUID == AMAZON_SIDEWALK_SVC_UUID) {
                detected = ThreatType::SIDEWALK_BEACON;
                identPayload = adData + 2;
                identLen = adDataLen - 2;
            } else if (svcUUID == GAEN_SVC_UUID) {
                detected = ThreatType::EXPOSURE_NOTIF;
                identPayload = adData + 2;
                identLen = adDataLen - 2;
            }
        }
        // ==[ 16-BIT SERVICE UUID LISTS (0x02 incomplete, 0x03 complete) ]==
        else if ((adType == 0x02 || adType == 0x03) && adDataLen >= 2) {
            uint8_t uuidCount = (uint8_t)(adDataLen / 2);
            uint16_t nextServiceCount = sample.serviceCount + uuidCount;
            sample.serviceCount = (nextServiceCount > 255) ? 255 : nextServiceCount;
            for (int u = 0; u + 1 < adDataLen && detected == ThreatType::UNKNOWN; u += 2) {
                uint16_t uuid16 = adData[u] | (adData[u + 1] << 8);
                if (sample.primaryService == 0) sample.primaryService = uuid16;
                if (uuid16 == HM10_SVC_UUID) {
                    detected = ThreatType::SUSPICIOUS_PERIPHERAL;
                    identPayload = advData;
                    identLen = advLen;
                } else if (uuid16 == EDDYSTONE_SVC_UUID) {
                    detected = ThreatType::EDDYSTONE;
                    identPayload = advData;
                    identLen = advLen;
                } else if (uuid16 == SAMSUNG_UNREG_SVC_UUID) {
                    detected = ThreatType::SMARTTAG_UNREGISTERED;
                    identPayload = advData;
                    identLen = advLen;
                } else if (uuid16 == BLE_HID_SVC_UUID) {
                    detected = ThreatType::HID_DEVICE;
                    identPayload = advData;
                    identLen = advLen;
                } else if (uuid16 == FLIPPER_SVC_BLACK ||
                           uuid16 == FLIPPER_SVC_WHITE ||
                           uuid16 == FLIPPER_SVC_TRANSPARENT) {
                    detected = ThreatType::FLIPPER;
                    identPayload = advData;
                    identLen = advLen;
                } else if (uuid16 == XIAOMI_MIBEACON_SVC_UUID) {
                    detected = ThreatType::XIAOMI_TRACKER;
                    identPayload = advData;
                    identLen = advLen;
                } else if (uuid16 == AMAZON_SIDEWALK_SVC_UUID) {
                    detected = ThreatType::SIDEWALK_BEACON;
                    identPayload = advData;
                    identLen = advLen;
                } else if (uuid16 == GAEN_SVC_UUID) {
                    detected = ThreatType::EXPOSURE_NOTIF;
                    identPayload = advData;
                    identLen = advLen;
                }
            }
        }

        // ==[ SOLICITATION UUIDS (0x14 incomplete, 0x15 complete) ]==
        // device requesting connection to specific services — reveals intent
        if ((adType == 0x14 || adType == 0x15) && adDataLen >= 2) {
            uint8_t cnt = (uint8_t)(adDataLen / 2);
            uint16_t next = sample.serviceCount + cnt;
            sample.serviceCount = (next > 255) ? 255 : (uint8_t)next;
            for (int u = 0; u + 1 < adDataLen && detected == ThreatType::UNKNOWN; u += 2) {
                uint16_t uuid16 = adData[u] | (adData[u + 1] << 8);
                if (sample.primaryService == 0) sample.primaryService = uuid16;
                if (uuid16 == BLE_HID_SVC_UUID) detected = ThreatType::HID_DEVICE;
            }
        }

        // ==[ 128-BIT SERVICE UUID LISTS (0x06 incomplete, 0x07 complete) ]==
        // DULT cross-platform tracker detection (draft standard)
        if ((adType == 0x06 || adType == 0x07) && adDataLen >= 16 &&
            detected == ThreatType::UNKNOWN) {
            for (int u = 0; u + 15 < adDataLen; u += 16) {
                if (memcmp(adData + u, DULT_SVC_UUID_128, 16) == 0) {
                    detected = ThreatType::GENERIC_TRACKER;
                    identPayload = advData;
                    identLen = advLen;
                    break;
                }
            }
        }

        // ==[ 128-BIT SERVICE DATA (0x21) ]==
        // DULT trackers may embed data keyed by 128-bit UUID here
        if (adType == 0x21 && adDataLen >= 16 && detected == ThreatType::UNKNOWN) {
            if (memcmp(adData, DULT_SVC_UUID_128, 16) == 0) {
                detected = ThreatType::GENERIC_TRACKER;
                identPayload = adData + 16;
                identLen = adDataLen - 16;
            }
        }

        pos += adLen + 1;
    }

    // ==[ POST-WALK FALLBACKS ]== MAC prefix + name matching
    if (detected == ThreatType::UNKNOWN) {
        // Flipper Zero MAC OUI prefixes
        if (memcmp(addr, FLIPPER_MAC_PREFIX_1, 3) == 0 ||
            memcmp(addr, FLIPPER_MAC_PREFIX_2, 3) == 0 ||
            memcmp(addr, FLIPPER_MAC_PREFIX_3, 3) == 0) {
            detected = ThreatType::FLIPPER;
        }
        // Flipper Zero name prefix (case-insensitive)
        else if (sample.name[0] && strncasecmp(sample.name, "Flipper", 7) == 0) {
            detected = ThreatType::FLIPPER;
        }
    }

    sample.type = detected;
    // Unknown devices stay MAC-scoped. Recognized rotating-address formats use
    // the complete identity payload; hashing only the first 16 bytes merged
    // distinct transmitters that shared a common protocol prefix.
    uint32_t h = (detected == ThreatType::UNKNOWN || identLen <= 0)
        ? fnvHash(addr, 6)
        : fnvHash(identPayload, identLen);
    memcpy(sample.payloadHash, &h, sizeof(sample.payloadHash));

    accumulateBLEHit(sample);
}

// Main-loop fusion for live pinned observations. The NimBLE producer never
// mutates catalog/fusion tables; it only appends or coalesces BleObservation.
void fuseLiveBLEObservations(uint32_t now) {
    if (!blePriorityMode || !bleScanBuf || !bleLiveFusedCounts ||
        processingBLE.load(std::memory_order_acquire)) return;

    int count = 0;
    {
        const BleObservationGuard guard;
        count = bleScanBufCount.load(std::memory_order_relaxed);
    }
    const int deviceCount = bleDeviceCount.load(std::memory_order_acquire);
    for (int i = 0; i < count && i < BLE_SCAN_BUF_SIZE; ++i) {
        BLERawHit hit = {};
        {
            const BleObservationGuard guard;
            hit = bleScanBuf[i];
        }
        if (!hit.valid || !isPinnedBlePayload(hit.payloadHash) ||
            hit.repeatCount <= bleLiveFusedCounts[i]) continue;

        for (int t = 0; t < deviceCount; ++t) {
            if (memcmp(bleDeviceTable[t].payloadHash, hit.payloadHash, 4) != 0) continue;
            TrackerEntry& entry = bleDeviceTable[t];
            const int8_t previousRssi = entry.rssi;
            entry.rssi = hit.rssi;
            entry.rssiSmooth = (int8_t)(entry.rssiSmooth * 0.7f + hit.rssi * 0.3f);
            entry.lastSeen = hit.timestampMs ? hit.timestampMs : now;
            entry.lastAdvTimestamp = hit.lastAdvTimestamp;
            const uint16_t newSamples = hit.repeatCount - bleLiveFusedCounts[i];
            const uint16_t nextSeen = entry.seenCount + newSamples;
            entry.seenCount = nextSeen > 255 ? 255 : (uint8_t)nextSeen;
            if (hit.measuredAdvIntervalMs != 0) {
                entry.measuredAdvIntervalMs = hit.measuredAdvIntervalMs;
                entry.intervalVariance = hit.intervalVariance;
            }
            if (hit.txPower != -127 &&
                (entry.txPower == -127 || hit.rssi >= previousRssi)) {
                entry.txPower = hit.txPower;
            }
            break;
        }
        bleLiveFusedCounts[i] = hit.repeatCount;
    }
}

// ==[ PROCESS BLE RESULTS ]== scan buffer → tracker table + catalog
void processBLEResults() {
    processingBLE.store(true, std::memory_order_release);  // fence before catalog mutation
    uint32_t now = millis();

    // skip stale eviction while streaming fast path or wardrive active — array shifts
    // race with NimBLE callback iterating bleDeviceTable on Core 0
    if (!blePriorityMode && !bleWardriveMode) {
        int count = bleDeviceCount.load(std::memory_order_acquire);
        for (int i = count - 1; i >= 0; i--) {
            if (now - bleDeviceTable[i].lastSeen > 1800000) {
                for (int k = i; k < count - 1; k++) {
                    bleDeviceTable[k] = bleDeviceTable[k + 1];
                }
                count--;
            }
        }
        bleDeviceCount.store(count, std::memory_order_release);
    }

    int bufCount = bleScanBufCount.load(std::memory_order_acquire);
    for (int i = 0; i < bufCount; i++) {
        BLERawHit& hit = bleScanBuf[i];
        if (!hit.valid) continue;

        upsertBLECatalogEntry(hit, now);
        if (hit.type == ThreatType::UNKNOWN) continue;

        // find existing tracker by payload hash (survives MAC rotation)
        int existing = -1;
        for (int t = 0; t < trackerCount; t++) {
            if (memcmp(trackerTable[t].payloadHash, hit.payloadHash, 4) == 0) {
                existing = t;
                break;
            }
        }

        if (existing >= 0) {
            // update existing tracker
            TrackerEntry& te = trackerTable[existing];
            int8_t prevRssi = te.rssi;  // save before overwrite — txPower gate needs old value
            if (te.type == ThreatType::GENERIC_TRACKER && hit.type != ThreatType::GENERIC_TRACKER) {
                te.type = hit.type;
            } else if (te.type == ThreatType::UNKNOWN) {
                te.type = hit.type;
            }
            te.rssi = hit.rssi;
            if (te.seenCount < 255) te.seenCount++;  // cap at 255 — prevents wrap-to-0 breaking "following" detection
            te.lastSeen = now;
            te.identityCandidates = max((uint8_t)1u, hit.macCount);
            te.advFlags |= hit.advFlags;
            te.advType = hit.advType;
            te.addrType = hit.addrType;
            if (hit.txPower != -127 && (te.txPower == -127 || hit.rssi >= prevRssi)) te.txPower = hit.txPower;
            if (hit.companyId != 0) te.companyId = hit.companyId;
            if (hit.primaryService != 0) te.primaryService = hit.primaryService;
            if (hit.appearance != 0) te.appearance = hit.appearance;
            if (hit.spamPlatform != 0) te.spamPlatform = hit.spamPlatform;
            if (hit.major != 0) te.major = hit.major;
            if (hit.minor != 0) te.minor = hit.minor;
            if (hit.frameType != 0 || te.frameType == 0) te.frameType = hit.frameType;
            if (hit.payloadLen > te.payloadLen) te.payloadLen = hit.payloadLen;
            if (hit.serviceCount > te.serviceCount) te.serviceCount = hit.serviceCount;
            if (hit.manufacturerCount > te.manufacturerCount) te.manufacturerCount = hit.manufacturerCount;
            if (ReconBleMath::shouldReplacePayloadPreview(
                    hit.payloadPreviewLen, te.payloadPreviewLen,
                    hit.rssi, prevRssi)) {
                te.payloadPreviewLen = hit.payloadPreviewLen;
                memcpy(te.payloadPreview, hit.payloadPreview, BLE_PAYLOAD_PREVIEW_MAX);
            }
            if (hit.name[0] && (!te.name[0] || strlen(hit.name) > strlen(te.name))) {
                strncpy(te.name, hit.name, sizeof(te.name) - 1);
                te.name[sizeof(te.name) - 1] = '\0';
            }

            uint8_t macChanges = 0;
            for (uint8_t m = 0; m < hit.macCount; m++) {
                if (memcmp(hit.macs[m], te.mac, 6) != 0) macChanges++;
            }
            if (macChanges > 0) {
                memcpy(te.mac, hit.addr, 6);
                te.lastMacChange = now;
                uint16_t nextCount = te.macChangeCount + macChanges;
                te.macChangeCount = (nextCount > 255) ? 255 : (uint8_t)nextCount;
            }

            // EMA-smoothed RSSI (alpha=0.3)
            te.rssiSmooth = (int8_t)(te.rssiSmooth * 0.7f + hit.rssi * 0.3f);

            // ==[ FEATURE 2: RELAY DETECTION ]== check interval jitter anomaly
            // Real trackers (AirTag, SmartTag) have hardware-constrained intervals
            // with low jitter (<100ms variance). Relayed/replayed signals via nRF
            // Connect or Proxmark show either zero jitter (software timer) or high
            // jitter (>200ms from network latency). Flag when coefficient of variation
            // deviates significantly from expected device profile.
            if (te.measuredAdvIntervalMs > 0 && te.seenCount >= 5 &&
                !(te.flags & FLAG_RELAY_SUSPECT) &&
                (te.type == ThreatType::AIRTAG || te.type == ThreatType::SMARTTAG ||
                 te.type == ThreatType::TILE || te.type == ThreatType::FMDN)) {
                // expected jitter profiles (ms variance):
                // AirTag: 2000ms interval, ~50ms jitter → variance ~50
                // SmartTag: 1000ms interval, ~40ms jitter → variance ~40
                // relayed: either <5ms (software timer) or >200ms (network)
                bool suspiciouslyPrecise = (te.intervalVariance < 5 && te.seenCount >= 8);
                bool suspiciouslyJittery = (te.intervalVariance > 200);
                if (suspiciouslyPrecise || suspiciouslyJittery) {
                    te.flags |= FLAG_RELAY_SUSPECT;
                    ReconEventData ev = {};
                    ev.event = ReconEvent::RELAY_SUSPECT;
                    ev.threatType = te.type;
                    ev.rssi = te.rssiSmooth;
                    snprintf(ev.detail, sizeof(ev.detail), "%s var:%u int:%u %s",
                             deviceLabel(te), te.intervalVariance,
                             te.measuredAdvIntervalMs,
                             suspiciouslyPrecise ? "PRECISE" : "JITTERY");
                    pushEvent(ev);
                    HAMLET_LOGF("[RECON] RELAY SUSPECT: %s\n", ev.detail);
                }
            }

            // ==[ MAC ROTATION ANALYSIS ]== distinguish privacy rotation from spam
            // iBeacons/Eddystone use static MACs — never spam.
            // Apple/Android privacy: random addr (type 1), moderate rotation rate.
            // Flipper spam: extreme rotation rate OR public MAC rotation (type 0).
            if (te.type != ThreatType::IBEACON &&
                te.type != ThreatType::EDDYSTONE &&
                te.macChangeCount >= 3 && (now - te.firstSeen < 30000) &&
                !(te.flags & FLAG_SPAM)) {
                uint32_t elapsed = now - te.firstSeen;
                if (elapsed == 0) elapsed = 1;
                // rate: changes per 10 seconds
                uint32_t ratePer10s = (uint32_t)te.macChangeCount * 10000UL / elapsed;
                bool isPublicMac = (te.addrType == 0);  // public addr rotation = very suspicious
                bool extremeRate = (ratePer10s >= 5);    // 5+ changes per 10s = spam
                // privacy rotation: random addr, moderate rate, known vendor
                bool likelyPrivacy = (!isPublicMac && !extremeRate &&
                    (te.companyId == APPLE_COMPANY_ID || te.companyId == 0x00E0 /*Google*/));
                if (!likelyPrivacy) {
                    te.flags |= FLAG_SPAM;
                    spamCount++;
                    // infer spam platform from hit data or type
                    if (hit.spamPlatform != 0) te.spamPlatform = hit.spamPlatform;
                    else if (te.spamPlatform == 0 && te.type == ThreatType::FAST_PAIR)
                        te.spamPlatform = (uint8_t)SpamPlatform::ANDROID;
                    ReconEventData ev = {};
                    ev.event = ReconEvent::BLE_SPAM;
                    ev.count = te.macChangeCount;
                    ev.rssi = hit.rssi;
                    snprintf(ev.detail, sizeof(ev.detail), "%d MAC/%lus %s",
                             te.macChangeCount, elapsed / 1000,
                             isPublicMac ? "PUB!" : extremeRate ? "FLOOD" : "ROT");
                    pushEvent(ev);
                    HAMLET_LOGF("[RECON] BLE SPAM: %s\n", ev.detail);
                }
            }
        } else if (trackerCount < MAX_TRACKERS) {
            // new tracker
            TrackerEntry& te = trackerTable[trackerCount];
            memset(&te, 0, sizeof(TrackerEntry));
            memcpy(te.mac, hit.addr, 6);
            memcpy(te.payloadHash, hit.payloadHash, 4);
            te.type = hit.type;
            te.rssi = hit.rssi;
            te.rssiSmooth = hit.rssi;
            te.txPower = hit.txPower;
            te.seenCount = 1;
            te.identityCandidates = max((uint8_t)1u, hit.macCount);
            te.advFlags = hit.advFlags;
            te.advType = hit.advType;
            te.addrType = hit.addrType;
            te.frameType = hit.frameType;
            te.companyId = hit.companyId;
            te.primaryService = hit.primaryService;
            te.appearance = hit.appearance;
            te.classOfDevice = hit.classOfDevice;
            te.advInterval = hit.advInterval;
            te.companyId2 = hit.companyId2;
            te.spamPlatform = hit.spamPlatform;
            te.firstDetectDist = (uint16_t)min((uint32_t)Pedometer::getDistance(), (uint32_t)65535);
            te.major = hit.major;
            te.minor = hit.minor;
            te.firstSeen = now;
            te.lastSeen = now;
            te.payloadLen = hit.payloadLen;
            te.serviceCount = hit.serviceCount;
            te.manufacturerCount = hit.manufacturerCount;
            te.payloadPreviewLen = hit.payloadPreviewLen;
            memcpy(te.payloadPreview, hit.payloadPreview, BLE_PAYLOAD_PREVIEW_MAX);
            if (hit.name[0]) {
                strncpy(te.name, hit.name, sizeof(te.name) - 1);
                te.name[sizeof(te.name) - 1] = '\0';
            }
            if (hit.macCount > 1) {
                te.macChangeCount = hit.macCount - 1;
                te.lastMacChange = now;
            }
            trackerCount++;

            // new tracker with 3+ MACs in one scan = likely spam (unless Apple/Google privacy)
            if (te.type != ThreatType::IBEACON &&
                te.type != ThreatType::EDDYSTONE &&
                te.macChangeCount >= 3) {
                bool likelyPrivacy = (te.addrType != 0 &&
                    (te.companyId == APPLE_COMPANY_ID || te.companyId == 0x00E0));
                if (!likelyPrivacy) {
                    te.flags |= FLAG_SPAM;
                    spamCount++;
                    if (te.spamPlatform == 0 && te.type == ThreatType::FAST_PAIR)
                        te.spamPlatform = (uint8_t)SpamPlatform::ANDROID;
                    ReconEventData spamEv = {};
                    spamEv.event = ReconEvent::BLE_SPAM;
                    spamEv.count = te.macChangeCount;
                    spamEv.rssi = hit.rssi;
                    snprintf(spamEv.detail, sizeof(spamEv.detail), "%d MACs %s",
                             te.macChangeCount, te.addrType == 0 ? "PUB!" : "FLOOD");
                    pushEvent(spamEv);
                    HAMLET_LOGF("[RECON] BLE SPAM: %s\n", spamEv.detail);
                }
            }

            // emit TRACKER_NEW — tracker table prevents dupes by payloadHash
            ReconEventData ev = {};
            ev.event = ReconEvent::TRACKER_NEW;
            ev.threatType = hit.type;
            ev.rssi = hit.rssi;
            snprintf(ev.detail, sizeof(ev.detail), "%s",
                    deviceLabelFromMeta(hit.type, hit.appearance,
                                        hit.classOfDevice, hit.name));
            if (hit.type == ThreatType::SUSPICIOUS_PERIPHERAL) {
                snprintf(ev.detail, sizeof(ev.detail), "SUS:SerialBLE");
            }
            pushEvent(ev);
            HAMLET_LOGF("[RECON] NEW TRACKER: %s %ddBm\n", ev.detail, hit.rssi);
        }
    }

    syncBLECatalogThreatState();

    // reset scan buffer — store with release so NimBLE callback sees 0
    bleScanBufCount.store(0, std::memory_order_release);
    memset(bleScanBuf, 0, BLE_SCAN_BUF_SIZE * sizeof(BLERawHit));
    if (bleLiveFusedCounts) {
        memset(bleLiveFusedCounts, 0, BLE_SCAN_BUF_SIZE * sizeof(uint16_t));
    }

    processingBLE.store(false, std::memory_order_release);  // allow streaming fast path
}

// ==[ NimBLE INIT / DEINIT ]== managed lifecycle to share radio
void initBLE() {
    if (bleInitialized) return;
    NimBLEDevice::init("");
    bleInitialized = true;
    HAMLET_LOGLN("[RECON] BLE initialized (NimBLE)");
}

void deinitBLE() {
    if (!bleInitialized) return;
    if (bleScanActive) stopBLEScan();
    bleChaff_stop();
    NimBLEDevice::deinit(true);  // true = clear all NimBLE state (clients, bonds)
    bleInitialized = false;
    HAMLET_LOGLN("[RECON] BLE deinitialized");
}

bool startBLEScan() {
    if (!bleInitialized || bleScanActive) return false;
    if (!bleSeenMacs || !bleContinuityMacs) {
        HAMLET_LOGLN("[RECON] BLE scratch buffers unavailable; skipping scan");
        return false;
    }
    bleScanBufCount.store(0, std::memory_order_release);
    appleContinuityCount = 0;
    bleSeenMacCount = 0;
    bleContinuityMacCount = 0;
    memset(bleSeenMacs, 0, BLE_SCAN_UNIQUE_MAC_CAP * 6);
    memset(bleContinuityMacs, 0, BLE_SCAN_UNIQUE_MAC_CAP * 6);
    if (bleScanBuf) memset(bleScanBuf, 0, BLE_SCAN_BUF_SIZE * sizeof(BLERawHit));
    if (bleLiveFusedCounts) {
        memset(bleLiveFusedCounts, 0, BLE_SCAN_BUF_SIZE * sizeof(uint16_t));
    }

    NimBLEScan* pScan = NimBLEDevice::getScan();
    pScan->setAdvertisedDeviceCallbacks(&scanCallbacks, true);  // true = want dupes
    pScan->setActiveScan(bleActiveScanMode);  // passive (stealth) or active (SCAN_REQ)
    pScan->setInterval(50);           // 50ms
    pScan->setWindow(30);             // 30ms (60% duty)
    pScan->setDuplicateFilter(false); // see all adv packets

    if (!pScan->start(currentBLEScanDurationMs() / 1000, onScanCompleteCb, false)) {
        bleScanActive = false;
        HAMLET_LOGLN("[RECON] BLE scan start failed");
        return false;
    }

    bleScanActive = true;
    bleScanStart = millis();
    HAMLET_LOGF("[RECON] BLE %s scan started\n", bleActiveScanMode ? "active" : "passive");
    return true;
}

void stopBLEScan() {
    bool wasActive = bleScanActive;
    bleScanActive = false;
    if (!bleInitialized) return;
    NimBLEScan* pScan = NimBLEDevice::getScan();
    if (wasActive) pScan->stop();
    pScan->clearResults();
}

} // namespace Recon
#endif // RECON_BLE_ENABLED
