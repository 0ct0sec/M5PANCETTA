/**
 * Capture - PSRAM buffer implementation
 *
 * ==[ LOOT LARDER ]== serialize PMKIDs/handshakes into PSRAM for Pork.
 */

#include "capture.h"
#include "config.h"
#include <SD.h>
#include <esp_heap_caps.h>
#include <string.h>
#include <atomic>
#include "../piglet/mood.h"
#include "../hal/sd_storage.h"
#include "../sync/nowflock_transport.h"
#include "../modes/hunt.h"
#include "../util/debug_log.h"

namespace Capture {

// buffer level -> mood barks
void triggerBufferFilling() {
    Mood::onBufferFilling();
}

void triggerBufferFull() {
    Mood::onBufferFull();
}

// psram heap
static uint8_t* psramBuffer = nullptr;
static uint32_t writePos = 0;
static uint32_t totalSize = 0;
static bool initOK = false;  // set true at end of init(); guards all write paths

// index table (PSRAM — saves ~2.5KB DRAM)
static CaptureEntry* entries = nullptr;
static uint16_t entryCount = 0;
static uint16_t pmkidCount = 0;
static uint16_t handshakeCount = 0;

// warn flags. reset after purge
static bool warned80 = false;
static bool warned100 = false;
static bool warned80hs = false;
static bool warned100hs = false;

// cached last capture timestamp (for 4-min IDLE critical trigger)
static uint32_t lastCaptureTimestamp = 0;

// cached unsynced counts — O(1) reads instead of O(n) scans
static uint16_t unsyncedTotal = 0;
static uint16_t unsyncedPMKIDs = 0;
static uint16_t unsyncedHandshakes = 0;

// ==[ SD JOURNAL ]== binary stash for reboot persistence
static const char* JOURNAL_PATH = "/hamlet/captures/stash.bin";
static const char* JOURNAL_TEMP_PATH = "/hamlet/captures/stash.tmp";
static const char* JOURNAL_BACKUP_PATH = "/hamlet/captures/stash.bak";
static const uint32_t JOURNAL_MAGIC = 0x5354534A;  // "STSJ"
static const uint16_t JOURNAL_VERSION = 0x0002;
static bool journalDirty = false;
static bool journalRestoreActive = false;

static bool recoverJournalPromotion() {
    const bool haveJournal = SDStorage::exists(JOURNAL_PATH);
    const bool haveBackup = SDStorage::exists(JOURNAL_BACKUP_PATH);

    if (!haveJournal && haveBackup) {
        if (!SDStorage::renameFile(JOURNAL_BACKUP_PATH, JOURNAL_PATH)) return false;
    } else if (haveJournal && haveBackup) {
        if (!SDStorage::remove(JOURNAL_BACKUP_PATH)) return false;
    }

    // A temp file with no journal or backup is the only recoverable candidate
    // after power failed during the first-ever seal. The normal parser still
    // validates its header and stops safely at a truncated record.
    if (!SDStorage::exists(JOURNAL_PATH) &&
        SDStorage::exists(JOURNAL_TEMP_PATH)) {
        return SDStorage::renameFile(JOURNAL_TEMP_PATH, JOURNAL_PATH);
    }
    if (SDStorage::exists(JOURNAL_TEMP_PATH)) {
        return SDStorage::remove(JOURNAL_TEMP_PATH);
    }
    return true;
}

#pragma pack(push, 1)
struct JournalHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
};

struct JournalEntryHeader {
    uint8_t type;     // CaptureType enum value
    uint8_t synced;   // 0 or 1
    uint16_t len;     // serialized data length
};
#pragma pack(pop)

// forward declarations (defined after dedup hash + type indexes)
static void sdJournalAppend(CaptureType type, bool synced,
                            const uint8_t* data, uint16_t len);
static void sdRestoreJournal();
static void sdFlushHandshake(const CapturedHandshake* hs);

// ==[ DEDUP HASH TABLE ]== FNV-1a of handshake BSSIDs. O(1) dedup vs O(n) PSRAM scan.
// 256 slots × uint16_t (entry index) = 512 bytes PSRAM. 0xFFFF = empty.
// NOTE: uint16_t avoids sentinel collision — old uint8_t used 0xFF as "empty"
// which collided with entry index 255 (MAX_CAPTURES-1).
#define DEDUP_HASH_SIZE 256
#define DEDUP_EMPTY 0xFFFF
static uint16_t* dedupHashTable = nullptr;  // PSRAM-backed, allocated in init()

// ==[ TYPE INDEXES ]== direct index into entries[] by type. O(1) fetch by typed index.
// PSRAM-backed to spare DRAM. Allocated in init(). Declared before the dedup
// fallback because low-memory boots scan the handshake index when the optional
// hash allocation is unavailable.
static uint16_t* pmkidIndex = nullptr;    // pmkidIndex[i] → entries[] slot for i-th PMKID
static uint16_t* hsIndex = nullptr;       // hsIndex[i] → entries[] slot for i-th handshake

static uint32_t fnv1a_6(const uint8_t* data) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < 6; i++) h = (h ^ data[i]) * 16777619u;
    return h;
}

static void dedupHashInsert(const uint8_t* bssid, uint16_t entryIdx) {
    if (!dedupHashTable) return;
    uint32_t slot = fnv1a_6(bssid) % DEDUP_HASH_SIZE;
    for (uint16_t i = 0; i < DEDUP_HASH_SIZE; i++) {
        uint32_t probe = (slot + i) % DEDUP_HASH_SIZE;
        if (dedupHashTable[probe] == DEDUP_EMPTY) {
            dedupHashTable[probe] = entryIdx;
            return;
        }
    }
}

static bool dedupHashContains(const uint8_t* bssid) {
    if (!bssid) return false;
    if (!dedupHashTable) {
        // The hash table is an optimization, not a correctness dependency.
        // On a tight PSRAM boot, retain deduplication with a bounded O(n) scan
        // over the handshake index rather than filing duplicate captures.
        for (uint16_t i = 0; i < handshakeCount; ++i) {
            const uint16_t entryIndex = hsIndex[i];
            if (entryIndex >= entryCount) continue;
            const CaptureEntry& entry = entries[entryIndex];
            if (entry.type != CaptureType::HANDSHAKE || entry.length < 6 ||
                entry.offset > writePos || entry.length > writePos - entry.offset) {
                continue;
            }
            if (memcmp(psramBuffer + entry.offset, bssid, 6) == 0) return true;
        }
        return false;
    }
    uint32_t slot = fnv1a_6(bssid) % DEDUP_HASH_SIZE;
    for (uint16_t i = 0; i < DEDUP_HASH_SIZE; i++) {
        uint32_t probe = (slot + i) % DEDUP_HASH_SIZE;
        if (dedupHashTable[probe] == DEDUP_EMPTY) return false;
        uint16_t idx = dedupHashTable[probe];
        if (idx < entryCount && entries[idx].type == CaptureType::HANDSHAKE) {
            uint8_t* ptr = psramBuffer + entries[idx].offset;
            if (memcmp(ptr, bssid, 6) == 0) return true;
        }
    }
    return false;
}

enum class HandshakeFormat : uint8_t {
    V1 = 1,
    V2 = 2
};

static constexpr uint16_t JOURNAL_VERSION_V1 = 0x0001;
static constexpr uint16_t JOURNAL_VERSION_V2 = 0x0002;
static constexpr uint16_t HANDSHAKE_HEADER_V1_LEN = 48;
static constexpr uint16_t HANDSHAKE_HEADER_V2_LEN = 56;
static constexpr uint16_t HANDSHAKE_ENTRY_MAX_LEN = 8192;

static bool isValidStoredEpoch(uint32_t epoch) {
    return epoch >= 1704067200U;
}

static uint32_t sanitizeStoredEpoch(uint32_t epoch) {
    return isValidStoredEpoch(epoch) ? epoch : 0;
}

static uint32_t getPMKIDTimestamp(const uint8_t* data, uint32_t len) {
    if (!data || len < 65) return 0;
    uint32_t ts = 0;
    memcpy(&ts, data + 61, 4);
    return sanitizeStoredEpoch(ts);
}

static uint32_t getHandshakeTimestampHint(const CapturedHandshake* hs) {
    if (!hs) return 0;

    uint32_t best = sanitizeStoredEpoch(hs->lastSeen);
    uint32_t first = sanitizeStoredEpoch(hs->firstSeen);
    if (best == 0 || (first != 0 && first > best)) best = first;

    for (int i = 0; i < 4; i++) {
        if (!hs->hasMessage(i + 1)) continue;
        uint32_t ts = sanitizeStoredEpoch(hs->frames[i].timestamp);
        if (ts > best) best = ts;
    }
    return best;
}

static void rebuildLastCaptureTimestamp();

static void updateLastCaptureTimestamp(const CapturedHandshake* hs) {
    uint32_t ts = getHandshakeTimestampHint(hs);
    if (ts > lastCaptureTimestamp) {
        lastCaptureTimestamp = ts;
    }
}

static uint32_t getSerializedHandshakeLength(const CapturedHandshake* hs) {
    if (!hs) return 0;

    uint16_t effectiveBeaconLen = (hs->beaconData != nullptr) ? hs->beaconLen : 0;
    uint32_t serializedLen = HANDSHAKE_HEADER_V2_LEN + effectiveBeaconLen;

    for (int i = 0; i < 4; i++) {
        if (hs->hasMessage(i + 1)) {
            serializedLen += 10 + hs->frames[i].len + hs->frames[i].fullFrameLen;
        }
    }
    return serializedLen;
}

static bool serializeHandshakeV2(const CapturedHandshake* hs, uint8_t* dst, uint32_t len) {
    if (!hs || !dst) return false;

    uint16_t effectiveBeaconLen = (hs->beaconData != nullptr) ? hs->beaconLen : 0;
    uint32_t serializedLen = getSerializedHandshakeLength(hs);
    if (serializedLen == 0 || serializedLen > len) return false;

    uint8_t* ptr = dst;
    memcpy(ptr, hs->bssid, 6); ptr += 6;
    memcpy(ptr, hs->station, 6); ptr += 6;

    const uint8_t ssidLen = static_cast<uint8_t>(
        strnlen(hs->ssid, sizeof(hs->ssid) - 1));
    *ptr++ = ssidLen;
    memcpy(ptr, hs->ssid, 32); ptr += 32;

    *ptr++ = hs->capturedMask;
    memcpy(ptr, &effectiveBeaconLen, 2); ptr += 2;

    uint32_t firstSeen = sanitizeStoredEpoch(hs->firstSeen);
    uint32_t lastSeen = sanitizeStoredEpoch(hs->lastSeen);
    memcpy(ptr, &firstSeen, 4); ptr += 4;
    memcpy(ptr, &lastSeen, 4); ptr += 4;

    if (effectiveBeaconLen > 0) {
        memcpy(ptr, hs->beaconData, effectiveBeaconLen);
        ptr += effectiveBeaconLen;
    }

    for (int i = 0; i < 4; i++) {
        if (!hs->hasMessage(i + 1)) continue;

        uint16_t frameLen = hs->frames[i].len;
        memcpy(ptr, &frameLen, 2); ptr += 2;
        memcpy(ptr, hs->frames[i].data, frameLen); ptr += frameLen;

        uint16_t fullLen = hs->frames[i].fullFrameLen;
        memcpy(ptr, &fullLen, 2); ptr += 2;
        if (fullLen > 0) {
            memcpy(ptr, hs->frames[i].fullFrame, fullLen);
            ptr += fullLen;
        }

        *ptr++ = hs->frames[i].messageNum;
        *ptr++ = (uint8_t)hs->frames[i].rssi;

        uint32_t ts = sanitizeStoredEpoch(hs->frames[i].timestamp);
        memcpy(ptr, &ts, 4); ptr += 4;
    }

    return (uint32_t)(ptr - dst) == serializedLen;
}

static bool deserializeHandshakeBlob(const uint8_t* data, uint32_t len,
                                     HandshakeFormat format,
                                     CapturedHandshake* out) {
    if (!data || !out) return false;
    memset(out, 0, sizeof(*out));

    const uint32_t headerLen = (format == HandshakeFormat::V2)
        ? HANDSHAKE_HEADER_V2_LEN : HANDSHAKE_HEADER_V1_LEN;
    if (len < headerLen) return false;

    const uint8_t* ptr = data;
    const uint8_t* end = data + len;

    memcpy(out->bssid, ptr, 6); ptr += 6;
    memcpy(out->station, ptr, 6); ptr += 6;

    uint8_t ssidLen = *ptr++;
    if (ssidLen > 32) ssidLen = 32;
    memcpy(out->ssid, ptr, ssidLen);
    out->ssid[ssidLen] = '\0';
    ptr += 32;

    out->capturedMask = *ptr++;
    memcpy(&out->beaconLen, ptr, 2); ptr += 2;

    if (format == HandshakeFormat::V2) {
        memcpy(&out->firstSeen, ptr, 4); ptr += 4;
        memcpy(&out->lastSeen, ptr, 4); ptr += 4;
    }

    out->firstSeen = sanitizeStoredEpoch(out->firstSeen);
    out->lastSeen = sanitizeStoredEpoch(out->lastSeen);

    if (ptr + out->beaconLen > end) return false;
    out->beaconData = (uint8_t*)ptr;
    ptr += out->beaconLen;

    for (int f = 0; f < 4; f++) {
        if (!out->hasMessage(f + 1)) continue;
        if (ptr + 2 > end) return false;

        uint16_t frameLen = 0;
        memcpy(&frameLen, ptr, 2); ptr += 2;
        if (ptr + frameLen > end) return false;
        out->frames[f].len = frameLen;
        if (out->frames[f].len > sizeof(out->frames[f].data)) {
            out->frames[f].len = sizeof(out->frames[f].data);
        }
        memcpy(out->frames[f].data, ptr, out->frames[f].len);
        ptr += frameLen;

        if (ptr + 2 > end) return false;
        uint16_t fullLen = 0;
        memcpy(&fullLen, ptr, 2); ptr += 2;
        if (ptr + fullLen > end) return false;
        out->frames[f].fullFrameLen = fullLen;
        if (out->frames[f].fullFrameLen > sizeof(out->frames[f].fullFrame)) {
            out->frames[f].fullFrameLen = sizeof(out->frames[f].fullFrame);
        }
        if (out->frames[f].fullFrameLen > 0) {
            memcpy(out->frames[f].fullFrame, ptr, out->frames[f].fullFrameLen);
        }
        ptr += fullLen;

        if (ptr + 6 > end) return false;
        out->frames[f].messageNum = *ptr++;
        out->frames[f].rssi = (int8_t)*ptr++;
        memcpy(&out->frames[f].timestamp, ptr, 4); ptr += 4;
        out->frames[f].timestamp = sanitizeStoredEpoch(out->frames[f].timestamp);
    }

    if (ptr != end) return false;

    if (format == HandshakeFormat::V1) {
        for (int i = 0; i < 4; i++) {
            if (!out->hasMessage(i + 1)) continue;
            uint32_t ts = out->frames[i].timestamp;
            if (ts == 0) continue;
            if (out->firstSeen == 0 || ts < out->firstSeen) out->firstSeen = ts;
            if (ts > out->lastSeen) out->lastSeen = ts;
        }
    }

    return true;
}

static bool storeHandshake(const CapturedHandshake* hs, bool synced,
                           bool flushSidecar, bool appendJournal) {
    if (!initOK || !hs || entryCount >= MAX_CAPTURES) {
        HAMLET_LOGF("[CAP] storeHS FAIL: init=%d hs=%p entries=%u/%u\n",
                      initOK, hs, entryCount, MAX_CAPTURES);
        return false;
    }
    if (dedupHashContains(hs->bssid)) {
        HAMLET_LOGF("[CAP] storeHS DEDUP: bssid=%02X:%02X:%02X:%02X:%02X:%02X ssid=%s\n",
                      hs->bssid[0], hs->bssid[1], hs->bssid[2],
                      hs->bssid[3], hs->bssid[4], hs->bssid[5], hs->ssid);
        return false;
    }

    uint32_t serializedLen = getSerializedHandshakeLength(hs);
    if (serializedLen == 0 || serializedLen > HANDSHAKE_ENTRY_MAX_LEN) {
        HAMLET_LOGF("[CAP] storeHS FAIL: serialLen=%u max=%u\n", serializedLen, HANDSHAKE_ENTRY_MAX_LEN);
        return false;
    }
    if (writePos + serializedLen > totalSize) {
        HAMLET_LOGF("[CAP] storeHS FAIL: writePos=%u + len=%u > total=%u\n", writePos, serializedLen, totalSize);
        return false;
    }

    float usage = (float)writePos / totalSize;
    if (usage >= 0.8f && !warned80hs) {
        warned80hs = true;
        triggerBufferFilling();
    }
    if (usage >= 0.99f && !warned100hs) {
        warned100hs = true;
        triggerBufferFull();
    }

    uint8_t* ptr = psramBuffer + writePos;
    if (!serializeHandshakeV2(hs, ptr, serializedLen)) return false;

    entries[entryCount].type = CaptureType::HANDSHAKE;
    entries[entryCount].offset = writePos;
    entries[entryCount].length = serializedLen;
    entries[entryCount].synced = synced;

    dedupHashInsert(hs->bssid, entryCount);
    hsIndex[handshakeCount] = entryCount;

    writePos += serializedLen;
    entryCount++;
    handshakeCount++;
    if (!synced) {
        unsyncedTotal++;
        unsyncedHandshakes++;
    }

    updateLastCaptureTimestamp(hs);

    if (flushSidecar) {
        sdFlushHandshake(hs);
    }
    if (appendJournal) {
        sdJournalAppend(CaptureType::HANDSHAKE, synced,
                        psramBuffer + entries[entryCount - 1].offset,
                        (uint16_t)serializedLen);
    }

    return true;
}

// ==[ SD JOURNAL IMPL ]== append/rewrite/restore

static void sdJournalAppend(CaptureType type, bool synced,
                            const uint8_t* data, uint16_t len) {
    // PSRAM remains the primary copy. Any failure here marks the journal dirty
    // so the next explicit save/power transition rewrites a complete snapshot
    // instead of silently treating a missing append as durable.
    if (!data || len == 0 || !SDStorage::isAvailable()) {
        journalDirty = true;
        return;
    }

    // If the journal does not exist yet, its header must be durable before an
    // entry can be queued or appended. A headerless entry stream is not
    // recoverable and must not be reported as a successful journal update.
    if (!SDStorage::exists(JOURNAL_PATH)) {
        JournalHeader hdr = { JOURNAL_MAGIC, JOURNAL_VERSION, 0 };
        if (!SDStorage::writeFile(JOURNAL_PATH,
                                  reinterpret_cast<const uint8_t*>(&hdr),
                                  sizeof(hdr))) {
            journalDirty = true;
            return;
        }
    }

    JournalEntryHeader eh;
    eh.type = (uint8_t)type;
    eh.synced = synced ? 1 : 0;
    eh.len = len;

    // Combine header + data into one buffer to minimize SD opens.
    const size_t totalLen = sizeof(eh) + len;
    uint8_t* buf = (uint8_t*)malloc(totalLen);
    if (!buf) {
        journalDirty = true;
        return;
    }
    memcpy(buf, &eh, sizeof(eh));
    memcpy(buf + sizeof(eh), data, len);
    if (totalLen <= 384 &&
        SDStorage::enqueueAppend(JOURNAL_PATH, buf, totalLen)) {
        free(buf);
        return;
    }
    if (!SDStorage::appendFile(JOURNAL_PATH, buf, totalLen)) {
        journalDirty = true;
    }
    free(buf);
}

static void sdRestoreJournal() {
    if (!SDStorage::isAvailable() || !SDStorage::exists(JOURNAL_PATH)) return;
    journalRestoreActive = true;

    // validate header
    size_t bytesRead = 0;
    JournalHeader hdr;
    if (!SDStorage::readFile(JOURNAL_PATH, (uint8_t*)&hdr, sizeof(hdr), &bytesRead)) {
        journalRestoreActive = false;
        return;
    }
    if (bytesRead < sizeof(hdr) || hdr.magic != JOURNAL_MAGIC ||
        (hdr.version != JOURNAL_VERSION_V1 && hdr.version != JOURNAL_VERSION_V2)) {
        HAMLET_LOGLN("[CAP] journal corrupt or wrong version — skipping");
        journalRestoreActive = false;
        return;
    }

    // read entire journal into PSRAM temp buffer
    // realistic max: PMKID=65B, handshake=48+512+4*812≈3760B, so ~2.5KB avg per entry
    File jf = SD.open(JOURNAL_PATH, FILE_READ);
    if (!jf) {
        journalRestoreActive = false;
        return;
    }

    size_t journalSize = jf.size();
    jf.close();
    if (journalSize < sizeof(JournalHeader)) {
        journalRestoreActive = false;
        return;
    }

    const size_t maxJournalSize =
        sizeof(JournalHeader) + ((size_t)MAX_CAPTURES * HANDSHAKE_ENTRY_MAX_LEN);
    if (journalSize > maxJournalSize) {
        journalSize = maxJournalSize;
        HAMLET_LOGLN("[CAP] journal larger than sane bound - truncating restore read");
    }

    uint8_t* jbuf = (uint8_t*)heap_caps_malloc(journalSize, MALLOC_CAP_SPIRAM);
    if (!jbuf) {
        HAMLET_LOGLN("[CAP] journal restore: no PSRAM for temp buffer");
        journalRestoreActive = false;
        return;
    }

    if (!SDStorage::readFile(JOURNAL_PATH, jbuf, journalSize, &bytesRead)) {
        heap_caps_free(jbuf);
        journalRestoreActive = false;
        return;
    }

    if (bytesRead < sizeof(JournalHeader)) {
        heap_caps_free(jbuf);
        journalRestoreActive = false;
        return;
    }

    uint32_t pos = sizeof(JournalHeader);
    uint16_t restored = 0;
    bool migratedLegacyEntries = (hdr.version == JOURNAL_VERSION_V1);
    HandshakeFormat handshakeFormat =
        (hdr.version == JOURNAL_VERSION_V2) ? HandshakeFormat::V2 : HandshakeFormat::V1;

    while (pos + sizeof(JournalEntryHeader) <= bytesRead) {
        JournalEntryHeader eh;
        memcpy(&eh, jbuf + pos, sizeof(eh));
        pos += sizeof(eh);

        // sanity check entry
        if (eh.len == 0 || eh.len > HANDSHAKE_ENTRY_MAX_LEN || pos + eh.len > bytesRead) {
            HAMLET_LOGF("[CAP] journal truncated at entry %d — stopping\n", restored);
            break;
        }

        uint8_t* data = jbuf + pos;
        pos += eh.len;

        CaptureType type = (CaptureType)eh.type;

        if (type == CaptureType::PMKID && eh.len >= 65) {
            // deserialize PMKID from raw PSRAM format
            CapturedPMKID pmkid;
            uint8_t* p = data;
            memcpy(pmkid.bssid, p, 6); p += 6;
            memcpy(pmkid.station, p, 6); p += 6;
            uint8_t ssidLen = *p++;
            if (ssidLen > 32) ssidLen = 32;
            memcpy(pmkid.ssid, p, ssidLen);
            pmkid.ssid[ssidLen] = '\0';
            p += 32;
            memcpy(pmkid.pmkid, p, 16); p += 16;
            memcpy(&pmkid.timestamp, p, 4);
            pmkid.timestamp = sanitizeStoredEpoch(pmkid.timestamp);
            pmkid.synced = (eh.synced != 0);

            if (addPMKID(&pmkid)) {
                restored++;
            }

        } else if (type == CaptureType::HANDSHAKE &&
                   eh.len >= ((handshakeFormat == HandshakeFormat::V2)
                       ? HANDSHAKE_HEADER_V2_LEN : HANDSHAKE_HEADER_V1_LEN)) {
            CapturedHandshake hs;
            if (!deserializeHandshakeBlob(data, eh.len, handshakeFormat, &hs)) {
                HAMLET_LOGF("[CAP] handshake entry %d corrupt - stopping\n", restored);
                break;
            }
            if (storeHandshake(&hs, eh.synced != 0, false, false)) {
                if (handshakeFormat == HandshakeFormat::V1) {
                    migratedLegacyEntries = true;
                }
                restored++;
            }
        }
    }

    heap_caps_free(jbuf);
    if (migratedLegacyEntries || hdr.version != JOURNAL_VERSION) {
        journalDirty = true;
    }
    journalRestoreActive = false;
    HAMLET_LOGF("[CAP] restored %d entries from SD journal\n", restored);
}

static void rebuildLastCaptureTimestamp() {
    lastCaptureTimestamp = 0;

    for (uint16_t i = 0; i < entryCount; i++) {
        const uint8_t* data = psramBuffer + entries[i].offset;
        if (entries[i].type == CaptureType::PMKID) {
            uint32_t ts = getPMKIDTimestamp(data, entries[i].length);
            if (ts > lastCaptureTimestamp) lastCaptureTimestamp = ts;
            continue;
        }

        CapturedHandshake hs;
        if (deserializeHandshakeBlob(data, entries[i].length, HandshakeFormat::V2, &hs)) {
            uint32_t ts = getHandshakeTimestampHint(&hs);
            if (ts > lastCaptureTimestamp) lastCaptureTimestamp = ts;
        }
    }
}

static void releaseCaptureStorage() {
    if (psramBuffer)    heap_caps_free(psramBuffer);
    if (entries)        heap_caps_free(entries);
    if (pmkidIndex)     heap_caps_free(pmkidIndex);
    if (hsIndex)        heap_caps_free(hsIndex);
    if (dedupHashTable) heap_caps_free(dedupHashTable);

    psramBuffer = nullptr;
    entries = nullptr;
    pmkidIndex = nullptr;
    hsIndex = nullptr;
    dedupHashTable = nullptr;
    totalSize = 0;
    writePos = 0;
    entryCount = 0;
    pmkidCount = 0;
    handshakeCount = 0;
    unsyncedTotal = 0;
    unsyncedPMKIDs = 0;
    unsyncedHandshakes = 0;
    lastCaptureTimestamp = 0;
    initOK = false;
}

bool init() {
    if (initOK) return true;
    // A failed earlier attempt may have obtained the large buffer before an
    // index allocation failed. Return that PSRAM before retrying or continuing
    // boot with capture disabled.
    releaseCaptureStorage();

    // ==[ DYNAMIC PSRAM SIZING ]== both targets expose 8MB, but the free
    // amount is runtime evidence. Leave the shared reserve, take 75% of what
    // remains, then clamp to the journal-safe bounds.
    size_t psramFree = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    // Other boot owners may already have claims on PSRAM, so size from free.
    size_t available = (psramFree > CAPTURE_PSRAM_RESERVED) ? psramFree - CAPTURE_PSRAM_RESERVED : 0;

    // Clamp keeps low-memory boots viable and prevents an oversized journal.
    size_t bufferSize = (available * 3) / 4;
    if (bufferSize < CAPTURE_BUFFER_MIN) bufferSize = CAPTURE_BUFFER_MIN;
    if (bufferSize > CAPTURE_BUFFER_MAX) bufferSize = CAPTURE_BUFFER_MAX;

    psramBuffer = (uint8_t*)heap_caps_malloc(bufferSize, MALLOC_CAP_SPIRAM);

    if (psramBuffer == nullptr) {
        // One last smaller room in the evidence locker before boot gives up.
        bufferSize = CAPTURE_BUFFER_MIN;
        psramBuffer = (uint8_t*)heap_caps_malloc(bufferSize, MALLOC_CAP_SPIRAM);
        if (psramBuffer == nullptr) {
            releaseCaptureStorage();
            return false;
        }
    }

    totalSize = bufferSize;
    writePos = 0;
    entryCount = 0;
    pmkidCount = 0;
    handshakeCount = 0;
    lastCaptureTimestamp = 0;
    unsyncedTotal = 0;
    unsyncedPMKIDs = 0;
    unsyncedHandshakes = 0;

    // Index table in PSRAM too — saves ~2.5KB DRAM
    if (!entries) {
        entries = (CaptureEntry*)heap_caps_malloc(
            sizeof(CaptureEntry) * MAX_CAPTURES, MALLOC_CAP_SPIRAM);
    }
    if (entries) memset(entries, 0, sizeof(CaptureEntry) * MAX_CAPTURES);

    // type indexes in PSRAM (saves 1KB DRAM)
    if (!pmkidIndex) {
        pmkidIndex = (uint16_t*)heap_caps_malloc(sizeof(uint16_t) * MAX_CAPTURES, MALLOC_CAP_SPIRAM);
    }
    if (!hsIndex) {
        hsIndex = (uint16_t*)heap_caps_malloc(sizeof(uint16_t) * MAX_CAPTURES, MALLOC_CAP_SPIRAM);
    }
    if (pmkidIndex) memset(pmkidIndex, 0, sizeof(uint16_t) * MAX_CAPTURES);
    if (hsIndex) memset(hsIndex, 0, sizeof(uint16_t) * MAX_CAPTURES);

    // dedup hash in PSRAM (512 bytes — saves DRAM)
    if (!dedupHashTable) {
        dedupHashTable = (uint16_t*)heap_caps_malloc(sizeof(uint16_t) * DEDUP_HASH_SIZE, MALLOC_CAP_SPIRAM);
    }
    if (dedupHashTable) memset(dedupHashTable, 0xFF, sizeof(uint16_t) * DEDUP_HASH_SIZE);

    // bail if any index table failed — journal restore would null-deref
    if (!entries || !pmkidIndex || !hsIndex) {
        HAMLET_LOGLN("[CAP] index alloc failed — capture disabled");
        releaseCaptureStorage();
        return false;
    }

    // NOTE: SD journal restore deferred to restoreJournal() — SD not mounted yet at init()

    initOK = true;
    return true;
}

void restoreJournal() {
    // ==[ SD JOURNAL RESTORE ]== reload captures from last session
    // called after SDStorage::init() — SD must be mounted first
    if (!recoverJournalPromotion()) {
        HAMLET_LOGLN("[CAP] journal promotion recovery failed");
    }
    sdRestoreJournal();

    // rebuild cached unsynced counts from restored entries
    unsyncedTotal = 0;
    unsyncedPMKIDs = 0;
    unsyncedHandshakes = 0;
    for (uint16_t i = 0; i < entryCount; i++) {
        if (!entries[i].synced) {
            unsyncedTotal++;
            if (entries[i].type == CaptureType::PMKID) unsyncedPMKIDs++;
            else unsyncedHandshakes++;
        }
    }
    rebuildLastCaptureTimestamp();

    if (entryCount > 0) {
        HAMLET_LOGF("[CAP] THE TAKE: %d captures loaded (%d unsynced)\n",
                      entryCount, unsyncedTotal);
    }
}

// ==[ SD PIPELINE ]== flush captures to SD card alongside PSRAM

// date string for filenames: YYYYMMDD
static void dateStr(uint32_t epoch, char* buf, size_t len) {
    if (!buf || len == 0) return;
    if (epoch < 1704067200) {  // pre-2024 = no valid time
        strncpy(buf, "undated", len);
        buf[len - 1] = '\0';
        return;
    }
    time_t t = (time_t)epoch;
    struct tm* tm = localtime(&t);
    if (!tm || strftime(buf, len, "%Y%m%d", tm) == 0) {
        strncpy(buf, "undated", len);
        buf[len - 1] = '\0';
    }
}

// hex encode helper
static void hexEncode(const uint8_t* data, size_t len, char* out) {
    for (size_t i = 0; i < len; i++) {
        snprintf(out + i * 2, 3, "%02x", data[i]);
    }
}

// write PMKID in hashcat 22000 format (mode 22000, type 01)
// WPA*01*PMKID*MAC_AP*MAC_STA*ESSID_HEX***
static void sdFlushPMKID(const CapturedPMKID* pmkid) {
    if (!SDStorage::isAvailable()) return;

    char dateBuf[12];
    dateStr(pmkid->timestamp, dateBuf, sizeof(dateBuf));

    char path[48];
    snprintf(path, sizeof(path), "/hamlet/captures/pmkid_%s.22000", dateBuf);

    // build hashcat line
    char line[256];
    char pmkidHex[33], apHex[13], staHex[13];
    hexEncode(pmkid->pmkid, 16, pmkidHex);
    hexEncode(pmkid->bssid, 6, apHex);
    hexEncode(pmkid->station, 6, staHex);

    // ESSID as hex
    char essidHex[65];
    essidHex[0] = '\0';
    size_t ssidLen = strlen(pmkid->ssid);
    if (ssidLen > 32) ssidLen = 32;
    hexEncode((const uint8_t*)pmkid->ssid, ssidLen, essidHex);

    int lineLen = snprintf(line, sizeof(line),
        "WPA*01*%s*%s*%s*%s***\n",
        pmkidHex, apHex, staHex, essidHex);

    if (lineLen > 0) {
        SDStorage::appendFile(path, (const uint8_t*)line, lineLen);
    }
}

// write handshake EAPOL frames as PCAP records
// beacon (if available) + each captured EAPOL fullFrame
static void sdFlushHandshake(const CapturedHandshake* hs) {
    if (!SDStorage::isAvailable()) return;

    char dateBuf[12];
    uint32_t ts = hs->firstSeen;
    if (ts == 0) ts = getCurrentEpoch();
    dateStr(ts, dateBuf, sizeof(dateBuf));

    char path[48];
    snprintf(path, sizeof(path), "/hamlet/captures/hs_%s.pcap", dateBuf);

    // create PCAP file if it doesn't exist
    if (!SDStorage::exists(path)) {
        if (!SDStorage::pcapBegin(path)) return;
    }

    // write beacon frame first (helps Wireshark/hashcat identify the network)
    if (hs->beaconLen > 0 && hs->beaconData != nullptr) {
        SDStorage::pcapAppend(path, hs->beaconData, hs->beaconLen, ts, 0);
    }

    // write each captured EAPOL full 802.11 frame
    for (int i = 0; i < 4; i++) {
        if (hs->hasMessage(i + 1) && hs->frames[i].fullFrameLen > 0) {
            SDStorage::pcapAppend(path,
                hs->frames[i].fullFrame, hs->frames[i].fullFrameLen,
                hs->frames[i].timestamp, 0);
        }
    }
}

bool addPMKID(const CapturedPMKID* pmkid) {
    if (!initOK || !pmkid || entryCount >= MAX_CAPTURES) {
        return false;
    }
    
    // PMKID to buffer: bssid(6) + sta(6) + ssid_len(1) + ssid(32) + pmkid(16) + ts(4) = 65 bytes
    const uint16_t serializedLen = 65;
    
    if (writePos + serializedLen > totalSize) {
        return false;
    }
    
    // buffer warnings -> mood barks
    float usage = (float)writePos / totalSize;
    if (usage >= 0.8f && !warned80) {
        warned80 = true;
        triggerBufferFilling();
    }
    if (usage >= 0.99f && !warned100) {
        warned100 = true;
        triggerBufferFull();
    }
    
    uint8_t* ptr = psramBuffer + writePos;
    
    memcpy(ptr, pmkid->bssid, 6);
    ptr += 6;
    
    memcpy(ptr, pmkid->station, 6);
    ptr += 6;
    
    const uint8_t ssidLen = static_cast<uint8_t>(
        strnlen(pmkid->ssid, sizeof(pmkid->ssid) - 1));
    *ptr++ = ssidLen;
    
    memcpy(ptr, pmkid->ssid, 32);
    ptr += 32;
    
    memcpy(ptr, pmkid->pmkid, 16);
    ptr += 16;
    
    uint32_t ts = sanitizeStoredEpoch(pmkid->timestamp);
    memcpy(ptr, &ts, 4);
    
    // Keep this monotonic when restored/imported records arrive out of order.
    if (ts > lastCaptureTimestamp) lastCaptureTimestamp = ts;
    
    // index entry
    entries[entryCount].type = CaptureType::PMKID;
    entries[entryCount].offset = writePos;
    entries[entryCount].length = serializedLen;
    entries[entryCount].synced = pmkid->synced;

    // type index: O(1) fetch by PMKID index
    pmkidIndex[pmkidCount] = entryCount;

    writePos += serializedLen;
    entryCount++;
    pmkidCount++;
    if (!pmkid->synced) {
        unsyncedTotal++;
        unsyncedPMKIDs++;
    }

    if (!journalRestoreActive) {
        // SD sidecar: hashcat 22000 format (fire-and-forget, PSRAM is primary)
        sdFlushPMKID(pmkid);

        char ann[49];
        snprintf(ann, sizeof(ann), "PMKID oui=%02X%02X%02X ch=%u conf=high",
                 pmkid->bssid[0], pmkid->bssid[1], pmkid->bssid[2],
                 (unsigned)Hunt::getCurrentChannel());
        NowFlock::broadcastCapture(ann);

        // SD journal: append for reboot persistence
        sdJournalAppend(CaptureType::PMKID, pmkid->synced,
                        psramBuffer + entries[entryCount - 1].offset, serializedLen);
    }

    return true;
}

bool addHandshake(const CapturedHandshake* hs) {
    return storeHandshake(hs, hs ? hs->synced : false, true, true);
}

uint16_t getPMKIDCount() {
    return pmkidCount;
}

uint16_t getHandshakeCount() {
    return handshakeCount;
}

uint16_t getTotalCount() {
    return entryCount;
}

uint16_t getUnsyncedCount() {
    return unsyncedTotal;
}

uint16_t getUnsyncedPMKIDCount() {
    return unsyncedPMKIDs;
}

uint16_t getUnsyncedHandshakeCount() {
    return unsyncedHandshakes;
}

bool getPMKID(uint16_t index, CapturedPMKID* out) {
    if (!initOK || !out || index >= pmkidCount) return false;
    uint16_t i = pmkidIndex[index];  // O(1) type index lookup

    uint8_t* ptr = psramBuffer + entries[i].offset;
    memcpy(out->bssid, ptr, 6); ptr += 6;
    memcpy(out->station, ptr, 6); ptr += 6;
    uint8_t ssidLen = *ptr++;
    if (ssidLen > 32) ssidLen = 32;
    memcpy(out->ssid, ptr, ssidLen);
    out->ssid[ssidLen] = '\0';
    ptr += 32;
    memcpy(out->pmkid, ptr, 16); ptr += 16;
    memcpy(&out->timestamp, ptr, 4);
    out->timestamp = sanitizeStoredEpoch(out->timestamp);
    out->synced = entries[i].synced;
    return true;
}

bool getHandshake(uint16_t index, CapturedHandshake* out) {
    if (!initOK || !out || index >= handshakeCount) return false;
    uint16_t i = hsIndex[index];  // O(1) type index lookup
    if (!deserializeHandshakeBlob(psramBuffer + entries[i].offset,
                                  entries[i].length,
                                  HandshakeFormat::V2,
                                  out)) {
        return false;
    }
    out->synced = entries[i].synced;
    return true;
}

void markPMKIDSynced(uint16_t index) {
    if (index < pmkidCount && !entries[pmkidIndex[index]].synced) {
        entries[pmkidIndex[index]].synced = true;
        journalDirty = true;
        unsyncedTotal--;
        unsyncedPMKIDs--;
    }
}

void markHandshakeSynced(uint16_t index) {
    if (index < handshakeCount && !entries[hsIndex[index]].synced) {
        entries[hsIndex[index]].synced = true;
        journalDirty = true;
        unsyncedTotal--;
        unsyncedHandshakes--;
    }
}

void clearAll() {
    writePos = 0;
    entryCount = 0;
    pmkidCount = 0;
    handshakeCount = 0;
    unsyncedTotal = 0;
    unsyncedPMKIDs = 0;
    unsyncedHandshakes = 0;
    lastCaptureTimestamp = 0;
    warned80 = false;
    warned100 = false;
    warned80hs = false;
    warned100hs = false;

    if (psramBuffer) memset(psramBuffer, 0, totalSize);
    if (entries) memset(entries, 0, sizeof(CaptureEntry) * MAX_CAPTURES);
    if (pmkidIndex) memset(pmkidIndex, 0, sizeof(uint16_t) * MAX_CAPTURES);
    if (hsIndex) memset(hsIndex, 0, sizeof(uint16_t) * MAX_CAPTURES);
    if (dedupHashTable) memset(dedupHashTable, 0xFF, sizeof(uint16_t) * DEDUP_HASH_SIZE);

    // A clear is a persistence mutation. The next seal replaces any old or
    // queued journal records with a valid empty snapshot.
    journalDirty = true;
}

uint32_t getUsedBytes() {
    return writePos;
}

uint32_t getFreeBytes() {
    return totalSize - writePos;
}

bool isFull() {
    // full = <1KB free or max entries
    return (getFreeBytes() < 1024) || (entryCount >= MAX_CAPTURES);
}

void saveJournal() {
    if (journalDirty) {
        // Sync-flag rewrites need the same exact, failure-safe snapshot used by
        // controlled shutdown. sealJournal() leaves journalDirty set on error.
        (void)sealJournal();
    }
}

bool sealJournal() {
    if (!SDStorage::isAvailable()) return false;

    // Drain older queued appends before truncating the journal. Otherwise a
    // stale unsynced record can land ahead of the rewritten synced record and
    // win dedup on the next boot.
    if (!SDStorage::flushDeferred()) return false;
    if (!recoverJournalPromotion()) return false;

    JournalHeader hdr = { JOURNAL_MAGIC, JOURNAL_VERSION, 0 };
    if (!SDStorage::beginWriteStream(JOURNAL_TEMP_PATH, true)) return false;

    bool ok = SDStorage::writeStream(
        reinterpret_cast<const uint8_t*>(&hdr), sizeof(hdr));
    for (uint16_t i = 0; ok && i < entryCount; ++i) {
        const CaptureEntry& entry = entries[i];
        if (entry.length == 0 || entry.length > UINT16_MAX ||
            entry.offset > writePos || entry.length > writePos - entry.offset) {
            ok = false;
            break;
        }

        JournalEntryHeader eh = {
            static_cast<uint8_t>(entry.type),
            static_cast<uint8_t>(entry.synced ? 1 : 0),
            static_cast<uint16_t>(entry.length)
        };
        ok = SDStorage::writeStream(
                 reinterpret_cast<const uint8_t*>(&eh), sizeof(eh)) &&
             SDStorage::writeStream(psramBuffer + entry.offset, entry.length);
    }

    if (ok) ok = SDStorage::flushStream();
    SDStorage::endWriteStream();
    if (!ok) {
        SDStorage::remove(JOURNAL_TEMP_PATH);
        return false;
    }

    // Keep the last known-good journal until the complete temp snapshot is in
    // place. The backup also lets restoreJournal() recover a mid-promotion cut.
    const bool hadJournal = SDStorage::exists(JOURNAL_PATH);
    if (hadJournal &&
        !SDStorage::renameFile(JOURNAL_PATH, JOURNAL_BACKUP_PATH)) {
        SDStorage::remove(JOURNAL_TEMP_PATH);
        return false;
    }
    if (!SDStorage::renameFile(JOURNAL_TEMP_PATH, JOURNAL_PATH)) {
        if (hadJournal) {
            SDStorage::renameFile(JOURNAL_BACKUP_PATH, JOURNAL_PATH);
        }
        return false;
    }
    if (hadJournal && SDStorage::exists(JOURNAL_BACKUP_PATH)) {
        SDStorage::remove(JOURNAL_BACKUP_PATH);
    }

    journalDirty = false;
    return true;
}

uint32_t getLastCaptureTime() {
    return lastCaptureTimestamp;
}

} // namespace Capture
