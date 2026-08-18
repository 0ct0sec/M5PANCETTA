#include "nowflock_export.h"
#include "nowflock_protocol.h"
#include "nowflock_lsp.h"
#include "../core/config.h"
#include "../hal/sd_storage.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#ifndef NATIVE_TEST
#include <Arduino.h>
#endif

namespace NowFlockExport {

static constexpr uint8_t QUEUE_DEPTH = 4;
static constexpr uint8_t LINE_BUF = NowFlock::EXPORT_LINE_MAX;

struct QueuedLine {
    uint8_t len;
    uint8_t data[LINE_BUF];
};

static QueuedLine queue[QUEUE_DEPTH];
static uint8_t queueHead = 0;
static uint8_t queueTail = 0;
static char replayPath[48] = {0};
static bool replayReady = false;

static uint32_t fnv1a32(const uint8_t* data, size_t len) {
    uint32_t h = 0x811C9DC5u;
    for (size_t i = 0; i < len; ++i) {
        h ^= data[i];
        h *= 0x01000193u;
    }
    return h ? h : 1u;
}

static uint32_t hashMac(const uint8_t mac[6]) {
    static const uint8_t salt[4] = {0x11, 0x0A, 0x5C, 0x9D};
    uint8_t buf[10];
    memcpy(buf, salt, 4);
    memcpy(buf + 4, mac, 6);
    return fnv1a32(buf, sizeof(buf));
}

static uint32_t hashSsid(const char* ssid) {
    if (!ssid) return 0;
    return fnv1a32(reinterpret_cast<const uint8_t*>(ssid), strlen(ssid));
}

static bool enqueue(const uint8_t* line, uint8_t len) {
    if (!line || len == 0 || len > LINE_BUF) return false;
    uint8_t next = (uint8_t)((queueTail + 1) % QUEUE_DEPTH);
    if (next == queueHead) return false;
    queue[queueTail].len = len;
    memcpy(queue[queueTail].data, line, len);
    queueTail = next;
    return true;
}

#ifndef NATIVE_TEST
static void ensureReplayPath(uint32_t nowMs) {
    if (replayReady && replayPath[0]) return;
    uint32_t sec = nowMs / 1000u;
    snprintf(replayPath, sizeof(replayPath), "/hamlet/wardrive/FLOCK_REPLAY_%lu.csv", (unsigned long)sec);
    replayReady = true;
}
#endif

void init() {
    reset();
}

void reset() {
    queueHead = 0;
    queueTail = 0;
    replayPath[0] = 0;
    replayReady = false;
}

bool enabled() {
    return Config::getNowFlockPigbrother() && Config::getNowFlockExportProfile() != 0;
}

static const char* profileToken() {
    switch (Config::getNowFlockExportProfile()) {
        case 1: return "wigle-v1";
        case 2: return "fmh-v1";
        default: return "wigle-v1";
    }
}

static const char* authToken(const char* authMode) {
    if (!authMode) return "unknown";
    if (strstr(authMode, "WPA3")) return "wpa3";
    if (strstr(authMode, "WPA2-EAP")) return "wpa2e";
    if (strstr(authMode, "WPA2")) return "wpa2";
    if (strstr(authMode, "WPA-PSK") || strstr(authMode, "WPA]") || strstr(authMode, "WPA,")) return "wpa";
    if (strstr(authMode, "WEP")) return "wep";
    if (strstr(authMode, "ESS")) return "open";
    return "unknown";
}

void onWardriveRow(const uint8_t* bssid, const char* ssid, uint8_t channel, int8_t rssi,
                   const char* authMode, uint32_t epochS, int32_t latE7, int32_t lonE7,
                   uint32_t nowMs) {
    if (!enabled() || !bssid) return;

    char line[LINE_BUF];
    uint32_t bssidHash = hashMac(bssid);
    uint32_t ssidHash = hashSsid(ssid);
    uint32_t ts = epochS ? epochS : nowMs / 1000u;
    // EXPORT_SNAPSHOT crosses the FNOW boundary. Keep first-party Wardrive
    // precision in its own CSV; peer replay gets the same 50 m grid used by
    // SIGHTING so exact route coordinates never ride the coordination frame.
    int32_t coarseLatE7 = NowFlockLsp::quantizeTileE7(latE7);
    int32_t coarseLonE7 = NowFlockLsp::quantizeTileE7(lonE7);
    int n = snprintf(line, sizeof(line),
        "profile=%s,kind=wifi,ts=%lu,lat_e7=%ld,lon_e7=%ld,ssid_hash=%08lX,bssid_hash=%08lX,channel=%u,rssi=%d,auth=%s",
        profileToken(),
        (unsigned long)ts,
        (long)coarseLatE7,
        (long)coarseLonE7,
        (unsigned long)ssidHash,
        (unsigned long)bssidHash,
        (unsigned)channel,
        (int)rssi,
        authToken(authMode));
    if (n <= 0 || n >= (int)sizeof(line)) return;
    enqueue(reinterpret_cast<const uint8_t*>(line), static_cast<uint8_t>(n));
}

static bool parseU32(const char* s, uint32_t& out, int base = 10) {
    if (!s || !*s) return false;
    char* end = nullptr;
    unsigned long v = strtoul(s, &end, base);
    if (!end || *end != '\0') return false;
    out = (uint32_t)v;
    return true;
}

static bool parseI32(const char* s, int32_t& out) {
    if (!s || !*s) return false;
    char* end = nullptr;
    long v = strtol(s, &end, 10);
    if (!end || *end != '\0') return false;
    out = (int32_t)v;
    return true;
}

static bool parseI8(const char* s, int8_t& out) {
    int32_t v = 0;
    if (!parseI32(s, v) || v < -127 || v > 127) return false;
    out = (int8_t)v;
    return true;
}

static bool parseU8(const char* s, uint8_t& out) {
    uint32_t v = 0;
    if (!parseU32(s, v, 10) || v > 255u) return false;
    out = (uint8_t)v;
    return true;
}

static void copyToken(char* dst, size_t dstLen, const char* src) {
    if (!dst || dstLen == 0) return;
    if (!src) {
        dst[0] = 0;
        return;
    }
    size_t di = 0;
    while (src[di] && di + 1 < dstLen) {
        char c = src[di];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_')) {
            break;
        }
        dst[di] = c;
        ++di;
    }
    dst[di] = 0;
}

bool parseSnapshotLine(const uint8_t* line, uint8_t lineLen, SnapshotRecord& out) {
    memset(&out, 0, sizeof(out));
    if (!line || lineLen == 0 || lineLen > LINE_BUF) return false;
    if (!NowFlock::exportLineHasProfile(line, lineLen)) return false;

    char buf[LINE_BUF + 1];
    memcpy(buf, line, lineLen);
    buf[lineLen] = 0;

    bool haveProfile = false;
    bool haveKind = false;
    bool haveTs = false;
    bool haveLat = false;
    bool haveLon = false;
    bool haveSsid = false;
    bool haveBssid = false;
    bool haveChannel = false;
    bool haveRssi = false;

    char* save = nullptr;
    for (char* tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(nullptr, ",", &save)) {
        char* eq = strchr(tok, '=');
        if (!eq || eq == tok) return false;
        *eq = 0;
        const char* key = tok;
        const char* val = eq + 1;

        if (strcmp(key, "profile") == 0) {
            copyToken(out.profile, sizeof(out.profile), val);
            haveProfile = (strcmp(out.profile, "wigle-v1") == 0 || strcmp(out.profile, "fmh-v1") == 0);
        } else if (strcmp(key, "kind") == 0) {
            copyToken(out.kind, sizeof(out.kind), val);
            haveKind = (strcmp(out.kind, "wifi") == 0 || strcmp(out.kind, "ble") == 0);
        } else if (strcmp(key, "ts") == 0) {
            haveTs = parseU32(val, out.ts, 10);
        } else if (strcmp(key, "lat_e7") == 0) {
            haveLat = parseI32(val, out.latE7);
        } else if (strcmp(key, "lon_e7") == 0) {
            haveLon = parseI32(val, out.lonE7);
        } else if (strcmp(key, "ssid_hash") == 0) {
            haveSsid = parseU32(val, out.ssidHash, 16);
        } else if (strcmp(key, "bssid_hash") == 0) {
            haveBssid = parseU32(val, out.bssidHash, 16);
        } else if (strcmp(key, "channel") == 0 || strcmp(key, "ch") == 0) {
            haveChannel = parseU8(val, out.channel);
        } else if (strcmp(key, "rssi") == 0) {
            haveRssi = parseI8(val, out.rssi);
        } else if (strcmp(key, "auth") == 0) {
            copyToken(out.auth, sizeof(out.auth), val);
        }
    }

    if (!haveProfile || !haveKind || !haveTs || !haveLat || !haveLon ||
        !haveSsid || !haveBssid || !haveChannel || !haveRssi) {
        return false;
    }
    if (out.channel < 1 || out.channel > 14) return false;
    if (out.auth[0] == 0) strcpy(out.auth, "unknown");
    return true;
}

bool dequeueLine(uint8_t* out, uint8_t& outLen, uint8_t maxLen) {
    outLen = 0;
    if (!out || queueHead == queueTail) return false;
    const QueuedLine& q = queue[queueHead];
    if (q.len > maxLen) return false;
    outLen = q.len;
    memcpy(out, q.data, q.len);
    queueHead = (uint8_t)((queueHead + 1) % QUEUE_DEPTH);
    return true;
}

void ingestPeerLine(uint32_t sourceNodeId, const uint8_t* line, uint8_t lineLen, uint32_t nowMs) {
    if (!line || lineLen == 0 || lineLen > LINE_BUF) return;
    if (!enabled()) return;
    SnapshotRecord rec;
    if (!parseSnapshotLine(line, lineLen, rec)) return;

    // Peer snapshots deliberately carry hashes, not observed BSSIDs/SSIDs.
    // Preserve them with peer provenance in the replay sidecar below; never
    // manufacture identities and mix them into an uploadable WiGLE CSV.
#ifndef NATIVE_TEST
    ensureReplayPath(nowMs);
    if (!SDStorage::isAvailable()) return;
    char buf[LINE_BUF + 32];
    int written = snprintf(buf, sizeof(buf), "%.*s,source_node=%08lX\n",
                           (int)lineLen, reinterpret_cast<const char*>(line),
                           (unsigned long)sourceNodeId);
    if (written <= 0 || written >= (int)sizeof(buf)) return;
    SDStorage::appendFile(replayPath, reinterpret_cast<const uint8_t*>(buf),
                          (size_t)written);
#else
    (void)sourceNodeId;
    (void)nowMs;
#endif
}

} // namespace NowFlockExport
