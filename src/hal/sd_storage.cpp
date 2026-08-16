/**
 * SD Storage HAL — onboard Core SD card
 *
 * Core2 shares VSPI with the display; M5Unified initializes that bus.
 * CoreS3 SE has an independent TF SPI route and must initialize its explicit
 * G36/G35/G37/G4 pins before SD.begin().
 * Auto-remount on eject/reinsert. No card = silent fallback.
 */

#include "sd_storage.h"
#include "platform.h"

#if HAMLET_HAS_SD

#include <SD.h>
#include <SPI.h>
#include "../core/power.h"
#include "../util/debug_log.h"
#include "../util/time_math.h"

namespace SDStorage {

// ==[ STATE ]==
static bool mounted = false;
static uint32_t lastHealthCheck = 0;
static const uint32_t HEALTH_CHECK_MS = 5000;
static uint8_t mountFailCount = 0;
static uint32_t nextProbeAt = 0;
static bool remountRequested = false;
static bool displayBusLocked = false;
static File writeStreamFile;
static bool writeStreamDirty = false;

// ==[ DEFERRED APPEND QUEUE ]==
static constexpr uint8_t DEFERRED_SLOTS = 8;
static constexpr size_t DEFERRED_MAX_PAYLOAD = 384;
static constexpr size_t DEFERRED_PATH_LEN = 48;

struct DeferredSlot {
    char path[DEFERRED_PATH_LEN];
    uint8_t data[DEFERRED_MAX_PAYLOAD];
    uint16_t len;
    bool used;
};

static DeferredSlot deferredQueue[DEFERRED_SLOTS];
static uint8_t deferredHead = 0;
static uint8_t deferredTail = 0;
static uint8_t deferredCount = 0;

// ==[ SD PIN ]==
static const uint32_t SD_SPI_FREQ = 25000000;  // 25MHz safe clock
#if HAMLET_TARGET_CORES3SE
static bool sdBusStarted = false;
#endif

// ==[ PCAP MAGIC ]==
static const uint32_t PCAP_MAGIC    = 0xA1B2C3D4;
static const uint16_t PCAP_VER_MAJ  = 2;
static const uint16_t PCAP_VER_MIN  = 4;
static const uint32_t PCAP_SNAPLEN  = 65535;
static const uint32_t PCAP_LINKTYPE = 105;  // LINKTYPE_IEEE802_11

// ==[ DIRECTORY TREE ]==
static const char* DIRS[] = {
    "/hamlet",
    "/hamlet/captures",
    "/hamlet/stats",
    "/hamlet/export",
    "/hamlet/wardrive",
    "/recon"
};
static const uint8_t DIR_COUNT = sizeof(DIRS) / sizeof(DIRS[0]);

// ==[ INTERNAL ]==

static bool tryMount() {
    if (mounted) return true;
#if HAMLET_TARGET_CORES3SE
    if (!sdBusStarted) {
        SPI.begin(HAMLET_SD_SCLK_PIN,
                  HAMLET_SD_MISO_PIN,
                  HAMLET_SD_MOSI_PIN,
                  HAMLET_SD_CS_PIN);
        sdBusStarted = true;
    }
#endif
    if (SD.begin(HAMLET_SD_CS_PIN, SPI, SD_SPI_FREQ)) {
        mounted = true;
        mountFailCount = 0;
        nextProbeAt = 0;
        HAMLET_LOGF("[SD] mounted. type=%d size=%lluMB\n",
                      SD.cardType(), SD.cardSize() / (1024 * 1024));
    }
    return mounted;
}

static uint32_t probeBackoffMs() {
    if (mountFailCount == 0) return HEALTH_CHECK_MS;
    uint8_t shift = mountFailCount;
    if (shift > 6) shift = 6;
    uint32_t delay = HEALTH_CHECK_MS << shift;
    if (delay > 300000) delay = 300000;
    return delay;
}

static void noteMountFailure(uint32_t now) {
    mountFailCount++;
    nextProbeAt = now + probeBackoffMs();
    HAMLET_LOGF("[SD] mount failed — next probe in %lus\n",
                  (unsigned long)(probeBackoffMs() / 1000));
}

static void resetMountBackoff() {
    mountFailCount = 0;
    nextProbeAt = 0;
    remountRequested = false;
}

static void ensureDirs() {
    for (uint8_t i = 0; i < DIR_COUNT; i++) {
        if (!SD.exists(DIRS[i])) {
            SD.mkdir(DIRS[i]);
        }
    }
}

static void closeWriteStream() {
    if (writeStreamFile) {
        writeStreamFile.close();
        writeStreamFile = File();
    }
    writeStreamDirty = false;
}

// ==[ PUBLIC API ]==

bool init() {
    resetMountBackoff();
    if (!tryMount()) {
        noteMountFailure(millis());
        HAMLET_LOGLN("[SD] no card or mount failed — PSRAM-only mode");
        return false;
    }
    // migrate old /sirloin tree to /hamlet
    if (SD.exists("/sirloin") && !SD.exists("/hamlet")) {
        SD.rename("/sirloin", "/hamlet");
        HAMLET_LOGLN("[SD] migrated /sirloin → /hamlet");
    }
    ensureDirs();
    HAMLET_LOGF("[SD] ready. free=%lluMB\n",
                  (SD.totalBytes() - SD.usedBytes()) / (1024 * 1024));
    return true;
}

void update() {
    uint32_t now = millis();
    if (now - lastHealthCheck < HEALTH_CHECK_MS) return;
    lastHealthCheck = now;

    if (displayBusLocked) return;

    if (mounted) {
        // probe card presence - cardType() returns CARD_NONE if ejected
        if (SD.cardType() == CARD_NONE) {
            closeWriteStream();
            HAMLET_LOGLN("[SD] card ejected");
            SD.end();
            mounted = false;
            noteMountFailure(now);
        }
        return;
    }

    // no card: backoff remount — SD.begin() blocks VSPI shared with display
    if (Power::isCriticalMode() && !remountRequested) return;

    if (!remountRequested && TimeMath::active(now, nextProbeAt)) return;

    if (tryMount()) {
        resetMountBackoff();
        ensureDirs();
        HAMLET_LOGLN("[SD] card reinserted, remounted");
    } else {
        noteMountFailure(now);
    }
    remountRequested = false;
}

void requestRemount() {
    remountRequested = true;
    resetMountBackoff();
    nextProbeAt = 0;
    lastHealthCheck = 0;
}

void setDisplayBusLocked(bool locked) {
    displayBusLocked = locked;
}

bool enqueueAppend(const char* path, const uint8_t* data, size_t len) {
    if (!path || !data || len == 0 || len > DEFERRED_MAX_PAYLOAD) return false;
    if (deferredCount >= DEFERRED_SLOTS) return false;

    DeferredSlot& slot = deferredQueue[deferredHead];
    strncpy(slot.path, path, DEFERRED_PATH_LEN - 1);
    slot.path[DEFERRED_PATH_LEN - 1] = '\0';
    memcpy(slot.data, data, len);
    slot.len = (uint16_t)len;
    slot.used = true;

    deferredHead = (deferredHead + 1) % DEFERRED_SLOTS;
    deferredCount++;
    return true;
}

void drainDeferred(uint32_t budgetMs) {
    if (!isAvailable() || displayBusLocked || deferredCount == 0) return;

    uint32_t start = millis();
    while (deferredCount > 0 && (millis() - start) < budgetMs) {
        DeferredSlot& slot = deferredQueue[deferredTail];
        if (!slot.used) {
            deferredTail = (deferredTail + 1) % DEFERRED_SLOTS;
            deferredCount--;
            continue;
        }
        if (!appendFile(slot.path, slot.data, slot.len)) {
            break;
        }
        slot.used = false;
        deferredTail = (deferredTail + 1) % DEFERRED_SLOTS;
        deferredCount--;
    }
}

bool flushDeferred() {
    if (deferredCount == 0) return true;
    if (!isAvailable() || displayBusLocked) return false;

    // The queue is statically bounded to eight 384-byte slots. Controlled
    // sleep/off may synchronously drain it without creating an unbounded stall.
    while (deferredCount > 0) {
        DeferredSlot& slot = deferredQueue[deferredTail];
        if (slot.used && !appendFile(slot.path, slot.data, slot.len)) {
            return false;
        }
        slot.used = false;
        deferredTail = (deferredTail + 1) % DEFERRED_SLOTS;
        deferredCount--;
    }
    return true;
}

bool isAvailable() {
    return mounted && SD.cardType() != CARD_NONE;
}

bool writeFile(const char* path, const uint8_t* data, size_t len) {
    if (!isAvailable()) return false;
    File f = SD.open(path, FILE_WRITE);
    if (!f) return false;
    size_t written = f.write(data, len);
    f.close();
    return written == len;
}

bool appendFile(const char* path, const uint8_t* data, size_t len) {
    if (!isAvailable()) return false;
    File f = SD.open(path, FILE_APPEND);
    if (!f) return false;
    size_t written = f.write(data, len);
    f.flush();
    f.close();
    return written == len;
}

bool readFile(const char* path, uint8_t* buf, size_t maxLen, size_t* bytesRead) {
    if (!isAvailable()) return false;
    File f = SD.open(path, FILE_READ);
    if (!f) return false;
    size_t read = f.read(buf, maxLen);
    f.close();
    if (bytesRead) *bytesRead = read;
    return true;
}

bool exists(const char* path) {
    if (!isAvailable()) return false;
    return SD.exists(path);
}

bool remove(const char* path) {
    if (!isAvailable()) return false;
    return SD.remove(path);
}

bool renameFile(const char* from, const char* to) {
    if (!isAvailable() || !from || !from[0] || !to || !to[0]) return false;
    closeWriteStream();
    return SD.rename(from, to);
}

uint64_t freeBytes() {
    if (!isAvailable()) return 0;
    return SD.totalBytes() - SD.usedBytes();
}

bool beginWriteStream(const char* path, bool truncate) {
    if (!isAvailable() || !path || !path[0]) return false;
    closeWriteStream();

    writeStreamFile = SD.open(path, truncate ? FILE_WRITE : FILE_APPEND);
    if (!writeStreamFile) {
        return false;
    }

    writeStreamDirty = false;
    return true;
}

bool writeStream(const uint8_t* data, size_t len) {
    if (len == 0) return true;
    if (!isAvailable() || !writeStreamFile || !data) return false;

    size_t written = writeStreamFile.write(data, len);
    if (written != len) {
        closeWriteStream();
        return false;
    }

    writeStreamDirty = true;
    return true;
}

bool flushStream() {
    if (!writeStreamFile) return false;
    if (writeStreamDirty) {
        writeStreamFile.flush();
        writeStreamDirty = false;
    }
    return (bool)writeStreamFile;
}

void endWriteStream() {
    if (writeStreamFile) {
        if (writeStreamDirty) {
            writeStreamFile.flush();
        }
        closeWriteStream();
    }
}

bool isWriteStreamOpen() {
    return (bool)writeStreamFile;
}

// ==[ SESSION STATS ]== JSON dump to /hamlet/stats/

bool dumpSessionStats(const SessionStats& stats) {
    if (!isAvailable()) return false;

    // build date string for filename
    char dateBuf[12];
    if (stats.timestamp >= 1704067200) {
        time_t t = (time_t)stats.timestamp;
        struct tm* tm = localtime(&t);
        snprintf(dateBuf, sizeof(dateBuf), "%04d%02d%02d",
                 tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday);
    } else {
        strncpy(dateBuf, "00000000", sizeof(dateBuf));
    }

    // append to daily log (multiple sessions per day)
    char path[48];
    snprintf(path, sizeof(path), "/hamlet/stats/hunt_%s.log", dateBuf);

    // JSON-ish line (one object per line, easy to parse)
    char line[320];
    int len = snprintf(line, sizeof(line),
        "{\"ts\":%lu,\"dur\":%lu,\"pmk\":%u,\"hs\":%u,"
        "\"nets\":%u,\"cli\":%u,\"prb\":%u,\"dea\":%u,\"stp\":%lu,"
        "\"ch\":[%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u]}\n",
        (unsigned long)stats.timestamp, (unsigned long)stats.durationSec,
        stats.pmkids, stats.handshakes,
        stats.networksFound, stats.clientsFound,
        stats.probesSent, stats.deauthsSent,
        (unsigned long)stats.steps,
        stats.channelStats[0], stats.channelStats[1], stats.channelStats[2],
        stats.channelStats[3], stats.channelStats[4], stats.channelStats[5],
        stats.channelStats[6], stats.channelStats[7], stats.channelStats[8],
        stats.channelStats[9], stats.channelStats[10], stats.channelStats[11],
        stats.channelStats[12]);

    if (len <= 0) return false;
    return appendFile(path, (const uint8_t*)line, len);
}

// ==[ PCAP ]== libpcap format for direct Wireshark/hashcat import

bool pcapBegin(const char* path) {
    if (!isAvailable()) return false;

    // global header: 24 bytes
    struct __attribute__((packed)) {
        uint32_t magic;
        uint16_t vMajor;
        uint16_t vMinor;
        int32_t  thiszone;   // GMT offset (0)
        uint32_t sigfigs;    // timestamp accuracy (0)
        uint32_t snaplen;
        uint32_t network;    // link-layer type
    } hdr = {
        PCAP_MAGIC, PCAP_VER_MAJ, PCAP_VER_MIN,
        0, 0, PCAP_SNAPLEN, PCAP_LINKTYPE
    };

    return writeFile(path, (const uint8_t*)&hdr, sizeof(hdr));
}

bool pcapAppend(const char* path,
                const uint8_t* frame, uint16_t frameLen,
                uint32_t tsSec, uint32_t tsUsec) {
    if (!isAvailable() || !frame || frameLen == 0) return false;

    // Validate frame length against PCAP max snap length (65535 bytes)
    if (frameLen > 65535) {
        HAMLET_LOGF("[SD] pcapAppend: invalid frameLen %u (exceeds PCAP max)\n", frameLen);
        return false;
    }

    // packet record header: 16 bytes
    struct __attribute__((packed)) {
        uint32_t tsSec;
        uint32_t tsUsec;
        uint32_t inclLen;    // bytes saved
        uint32_t origLen;    // original length
    } recHdr = { tsSec, tsUsec, frameLen, frameLen };

    // write header + payload in one open
    File f = SD.open(path, FILE_APPEND);
    if (!f) return false;
    size_t w1 = f.write((const uint8_t*)&recHdr, sizeof(recHdr));
    size_t w2 = f.write(frame, frameLen);
    f.flush();
    f.close();

    return (w1 == sizeof(recHdr) && w2 == frameLen);
}

} // namespace SDStorage

#else
// ==[ NO SD ]== capability fallback for a future target without storage
namespace SDStorage {
    bool init()       { return false; }
    void update()     {}
    void requestRemount() {}
    bool isAvailable(){ return false; }
    bool enqueueAppend(const char*, const uint8_t*, size_t) { return false; }
    void drainDeferred(uint32_t) {}
    bool flushDeferred() { return true; }
    void setDisplayBusLocked(bool) {}
    bool writeFile(const char*, const uint8_t*, size_t)           { return false; }
    bool appendFile(const char*, const uint8_t*, size_t)          { return false; }
    bool readFile(const char*, uint8_t*, size_t, size_t*)         { return false; }
    bool exists(const char*)  { return false; }
    bool remove(const char*)  { return false; }
    bool renameFile(const char*, const char*) { return false; }
    uint64_t freeBytes()      { return 0; }
    bool beginWriteStream(const char*, bool) { return false; }
    bool writeStream(const uint8_t*, size_t) { return false; }
    bool flushStream() { return false; }
    void endWriteStream() {}
    bool isWriteStreamOpen() { return false; }
    bool pcapBegin(const char*) { return false; }
    bool pcapAppend(const char*, const uint8_t*, uint16_t, uint32_t, uint32_t) { return false; }
    bool dumpSessionStats(const SessionStats&) { return false; }
}
#endif
