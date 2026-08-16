/**
 * Defhog4 — full-screen defense terminal mode
 *
 * ==[ D3FH0G4 ]== the pig's war room. 5 panes of cross-namespace
 * intelligence. real-time. scrollable. interactive. black screen,
 * neon text, fat cursor, alert blink. hacker terminal. no tab bar —
 * cycle panes with A/C. boot sequence on entry. smart pane selection.
 */

#include "defhog4.h"
#include "../hamlet.h"
#include "../ui/display.h"
#include "../ui/defhog_terminal.h"
#include "../defense/recon.h"
#include "../defense/xband.h"
#include "../defense/defense_pipeline.h"
#include "../modes/hunt.h"
#include "../modes/ble_scanner.h"
#include "../core/capture.h"
#include "../core/gps.h"
#include "../audio/sfx.h"
#include <esp_random.h>

namespace Defhog4 {

// ==[ LAYOUT ]== full-screen operator field panel. The shared status bar and
// a real tap target strip replace the terminal-only title treatment.
static constexpr int SCREEN_W     = 320;
static constexpr int SCREEN_H     = 240;
static constexpr int STATUS_H     = TOP_BAR_H;
static constexpr int TAB_Y        = STATUS_H + 2;
static constexpr int TAB_H        = 12;
static constexpr int SUMMARY_Y    = TAB_Y + TAB_H + 2;
static constexpr int SUMMARY_H    = 12;
static constexpr int HEADER_H     = SUMMARY_Y + SUMMARY_H + 2;
static constexpr int BOTTOM_H     = 14;
static constexpr int CONTENT_Y    = HEADER_H;
static constexpr int CONTENT_H    = SCREEN_H - HEADER_H - BOTTOM_H;  // 182px
static constexpr int CHAR_W       = 6;    // size 1 font
static constexpr int CHAR_H       = 8;
static constexpr int LINE_H       = 9;    // char + 1px gap
static constexpr int VISIBLE_LINES = CONTENT_H / LINE_H;  // 23
static constexpr int MAX_CHARS    = (SCREEN_W - 4) / CHAR_W;  // 52
static constexpr int TEXT_X       = 2;    // left padding

// ==[ RING BUFFER ]==
static constexpr int PANE_COUNT    = 5;
static constexpr int RING_LINES    = 64;
static constexpr int LINE_LEN      = 54;  // 53 visible + null

// ==[ COLOR ]== same theme-driven field palette as R1B R4CK / Spectrum.
// Threat state is carried by explicit labels and inversion, not a detached
// terminal palette that can drift away from the rest of the operator UI.
static inline uint16_t termBG()  { return Display::getColorBG(); }
static inline uint16_t termFG()  { return Display::getColorFG(); }
static inline uint16_t termDIM() { return Display::lerpColor565(termFG(), termBG(), 0.50f); }
static inline uint16_t termPri() { return termFG(); }
static inline uint16_t termAcc() { return termFG(); }

// ==[ LINE COLOR ]== matches DefhogTerminal::LineColor
enum class LC : uint8_t { FG, DIM, ALERT, BAR };

// ==[ PANE LINE ]==
struct PaneLine {
    char text[LINE_LEN];
    LC color;
    bool tappable;
    uint8_t actionId;
    // BLE tracker rows are rendered from a live table. Preserve the payload
    // identity that was actually shown so a later tap cannot follow whatever
    // happened to replace the same table slot.
    uint8_t trackerPayloadHash[4];
    bool hasTrackerIdentity;
    bool serialBleSuspect;
};

// ==[ PANE ENUM ]==
enum class Pane : uint8_t {
    SITREP = 0,
    RF,
    BLE,
    FUSION,
    LOG,
    COUNT
};

// ==[ PHASE ]==
enum class Phase : uint8_t {
    BOOT,
    LIVE,
    REVIEW,
};

// ==[ STATE ]==
static Phase phase = Phase::BOOT;
static Pane currentPane = Pane::SITREP;
static uint32_t enterTime = 0;
static uint32_t lastRefresh[PANE_COUNT] = {};
static bool paneHasUnseen[PANE_COUNT] = {};

// per-pane ring buffers (PSRAM)
static PaneLine* paneBuffers[PANE_COUNT] = {};
static uint8_t paneWriteHead[PANE_COUNT] = {};
static uint8_t paneLineCount[PANE_COUNT] = {};
static int8_t  paneScrollOffset[PANE_COUNT] = {};  // 0 = live (bottom)

// cursor blink
static uint32_t lastCursorToggle = 0;
static bool cursorVisible = true;

// alert interrupt
static char interruptMsg[LINE_LEN] = {};
static uint32_t interruptStart = 0;
static uint32_t interruptDuration = 0;

// interrupt edge detection (reset in enter())
static bool lastDeauthActive = false;
static bool lastEvilTwin = false;
static bool lastKarma = false;
static int  lastFollowing = 0;
static int  lastSerialBleSuspectCount = 0;

// tap-expand state for FUSION + LOG panes
static int expandedAttacker = -1;  // FUSION: which attacker profile is expanded (-1 = none)
static int expandedLogEntry = -1;  // LOG: raw forensic ring slot (-1 = none)
static uint32_t expandedLogTimestamp = 0;  // guards against ring-slot reuse

static void clearLogExpansion() {
    expandedLogEntry = -1;
    expandedLogTimestamp = 0;
}

// boot sequence
static constexpr int BOOT_LINES = 8;
static constexpr uint32_t BOOT_LINE_MS = 150;
static constexpr uint32_t BOOT_TOTAL_MS = BOOT_LINES * BOOT_LINE_MS + 300;

// flash effect — brief neon wash after boot
static uint32_t flashStart = 0;
static constexpr uint32_t FLASH_DURATION_MS = 200;

static const char* const paneTabLabels[] = {
    "SITREP", "SIGINT", "BLE", "FUSION", "LOG"
};

// threat labels
static const char* const threatLabels[] = {
    "GREEN", "YELLOW", "ORANGE", "RED"
};

// SITREP is the orientation surface, not a second unstructured log.  These
// actions make the current correlation one tap away while leaving the detailed
// pane as the source of record.
static constexpr uint8_t SITREP_ACTION_HUNT   = 0xFF;
static constexpr uint8_t SITREP_ACTION_SIGINT = 0xFE;
static constexpr uint8_t SITREP_ACTION_BLE    = 0xFD;
static constexpr uint8_t SITREP_ACTION_FUSION = 0xFC;
static constexpr uint8_t SITREP_ACTION_LOG    = 0xFB;

// refresh intervals (ms)
static const uint32_t refreshIntervals[] = {
    5000,  // SITREP
    3000,  // RF (SIGINT)
    2000,  // BLE
    5000,  // FUSION
    5000,  // LOG
};

// ==[ DEAUTH TOOL SIGNATURE ]== reason code + PPS + source count → probable tool
// duplicated from defhog_terminal.cpp — pure function, avoids coupling
static const char* toolSignature(uint16_t reason, uint16_t pps, uint8_t sources) {
    if (reason == 7 && sources == 1 && pps > 50) return "aircrk";
    if (reason == 2 && sources == 1)             return "flippr";
    if (reason == 1 && sources > 1)              return "mdk4";
    if (reason == 1 && sources == 1 && pps < 100) return "d8266";
    if (reason == 1 && pps > 100)                return "mdk";
    return "?";
}

static void pushLine(int p, LC color, const char* fmt, ...);
static void pushTappable(int p, LC color, uint8_t actionId, const char* fmt, ...);

static void appendToken(char* buf, size_t cap, const char* token) {
    if (!buf || !token || cap == 0) return;
    size_t len = strlen(buf);
    if (len >= cap - 1) return;
    snprintf(buf + len, cap - len, "%s", token);
}

static bool hasBssid(const uint8_t* bssid) {
    if (!bssid) return false;
    for (int i = 0; i < 6; i++) {
        if (bssid[i] != 0) return true;
    }
    return false;
}

static void formatAge(uint32_t timestamp, char* out, size_t outLen) {
    if (!out || outLen == 0) return;
    if (timestamp == 0) {
        snprintf(out, outLen, "--");
        return;
    }
    uint32_t age = (millis() - timestamp) / 1000;
    if (age < 60) snprintf(out, outLen, "%lus", (unsigned long)age);
    else if (age < 3600) snprintf(out, outLen, "%lum", (unsigned long)(age / 60));
    else snprintf(out, outLen, "%luh", (unsigned long)(age / 3600));
}

static const char* findSsidForBssid(const uint8_t* bssid) {
    if (!hasBssid(bssid)) return nullptr;

    uint16_t huntNets = Hunt::getNetworkCount();
    const auto* networks = Hunt::getNetworks();
    if (networks) {
        for (uint16_t n = 0; n < huntNets && n < MAX_HUNT_NETWORKS; n++) {
            if (memcmp(networks[n].bssid, bssid, 6) == 0 && networks[n].ssid[0]) {
                return networks[n].ssid;
            }
        }
    }

    int reconNets = DefensePipeline::snapshot().getWifiSnapshotCount();
    const auto* wifiAPs = DefensePipeline::snapshot().getWifiSnapshot();
    if (wifiAPs) {
        for (int n = 0; n < reconNets; n++) {
            if (memcmp(wifiAPs[n].bssid, bssid, 6) == 0 && wifiAPs[n].ssid[0]) {
                return wifiAPs[n].ssid;
            }
        }
    }
    return nullptr;
}

static uint8_t indicatorCount(uint8_t flags) {
    uint8_t count = 0;
    for (uint8_t b = flags; b; b >>= 1) count += (b & 1);
    return count;
}

static void buildIocString(uint8_t flags, const char* prefix, char* out, size_t outLen) {
    if (!out || outLen == 0) return;
    snprintf(out, outLen, "%s", prefix ? prefix : "");
    if (flags & Recon::IND_FINGERPRINT) appendToken(out, outLen, "FP ");
    if (flags & Recon::IND_SEQ_ANOMALY) appendToken(out, outLen, "SEQ ");
    if (flags & Recon::IND_RSSI_ANOMALY) appendToken(out, outLen, "RSSI ");
    if (flags & Recon::IND_EVIL_TWIN) appendToken(out, outLen, "TWIN ");
    if (flags & Recon::IND_KARMA) appendToken(out, outLen, "KARMA ");
    if (flags & Recon::IND_DEAUTH) appendToken(out, outLen, "DEAUTH ");
    if (flags & Recon::IND_BLE_ATTACK) appendToken(out, outLen, "BLE ");
}

static bool isCriticalEvent(Recon::ReconEvent ev) {
    switch (ev) {
        case Recon::ReconEvent::DEAUTH_DETECTED:
        case Recon::ReconEvent::EVIL_TWIN:
        case Recon::ReconEvent::KARMA_HONEYPOT:
        case Recon::ReconEvent::COORDINATED_ATTACK:
        case Recon::ReconEvent::ATTACKER_IDENTIFIED:
        case Recon::ReconEvent::DUAL_BAND_STALK:
        case Recon::ReconEvent::FOLLOWING_NETWORK_ID:
        case Recon::ReconEvent::KARMA_CONFIRMED:
        case Recon::ReconEvent::CANARY_TRIPPED:
        case Recon::ReconEvent::TOOL_IDENTIFIED:
        case Recon::ReconEvent::HOSTILE_CLIENT:
        case Recon::ReconEvent::LOW_ENTROPY_BEACON:
            return true;
        default:
            return false;
    }
}

static uint8_t collectDeauthBurstOrder(uint8_t* order, uint8_t maxOrder) {
    if (!order || maxOrder == 0) return 0;
    const auto* bursts = DefensePipeline::snapshot().getDeauthBurstHistory();
    if (!bursts) return 0;

    uint8_t n = 0;
    for (uint8_t i = 0; i < Recon::MAX_DEAUTH_HISTORY && n < maxOrder; i++) {
        if (bursts[i].timestamp == 0) continue;
        order[n++] = i;
    }

    uint32_t now = millis();
    for (uint8_t i = 0; i < n; i++) {
        for (uint8_t j = i + 1; j < n; j++) {
            if ((now - bursts[order[j]].timestamp) < (now - bursts[order[i]].timestamp)) {
                uint8_t tmp = order[i];
                order[i] = order[j];
                order[j] = tmp;
            }
        }
    }
    return n;
}

static void pushDeauthBurstRows(int p, uint8_t maxRows, bool withTarget) {
    uint8_t order[Recon::MAX_DEAUTH_HISTORY];
    uint8_t n = collectDeauthBurstOrder(order, Recon::MAX_DEAUTH_HISTORY);
    if (n == 0) {
        pushLine(p, LC::DIM, " no deauth bursts detected");
        return;
    }

    const auto* bursts = DefensePipeline::snapshot().getDeauthBurstHistory();
    pushLine(p, LC::DIM, " TIME   PPS CH RSSI SRC TOOL");
    for (uint8_t row = 0; row < n && row < maxRows; row++) {
        const auto& b = bursts[order[row]];
        char age[8];
        formatAge(b.timestamp, age, sizeof(age));
        const char* tool = toolSignature(b.dominantReason, b.pps, b.uniqueSources);
        pushLine(p, b.huntChannelMatch ? LC::ALERT : LC::FG,
            " %-5s %3u %02u %4d  %-2u %-6s%s",
            age, (unsigned)b.pps, b.dominantChannel, b.peakRSSI,
            (unsigned)b.uniqueSources, tool, b.huntChannelMatch ? " !H" : "");

        if (withTarget) {
            const char* ssid = findSsidForBssid(b.targetBssid);
            if (ssid && ssid[0]) {
                pushLine(p, LC::DIM, "   tgt:%02X:%02X:%02X \"%.*s\" r:%u",
                    b.targetBssid[3], b.targetBssid[4], b.targetBssid[5],
                    14, ssid, (unsigned)b.dominantReason);
            } else if (hasBssid(b.targetBssid)) {
                pushLine(p, LC::DIM, "   tgt:%02X:%02X:%02X:%02X:%02X:%02X r:%u",
                    b.targetBssid[0], b.targetBssid[1], b.targetBssid[2],
                    b.targetBssid[3], b.targetBssid[4], b.targetBssid[5],
                    (unsigned)b.dominantReason);
            } else {
                pushLine(p, LC::DIM, "   broadcast/unknown target r:%u",
                    (unsigned)b.dominantReason);
            }
        }
    }
}

static void pushTemporalHeatmap(int p) {
    Recon::HeatmapBucket buckets[10];
    DefensePipeline::snapshot().getTemporalHeatmap(buckets, 10, 300000);
    static const char ramp[] = " .:-=+*#%@";
    char d[16] = " D:";
    char w[16] = " W:";
    char b[16] = " B:";
    uint8_t flags = 0;

    for (int i = 0; i < 10; i++) {
        uint8_t di = buckets[i].deauthIntensity / 26;
        uint8_t wi = buckets[i].wifiIntensity / 26;
        uint8_t bi = buckets[i].bleIntensity / 26;
        if (di > 9) di = 9;
        if (wi > 9) wi = 9;
        if (bi > 9) bi = 9;
        d[3 + i] = ramp[di];
        w[3 + i] = ramp[wi];
        b[3 + i] = ramp[bi];
        flags |= buckets[i].indicatorFlags;
    }
    d[13] = w[13] = b[13] = '\0';
    pushLine(p, LC::DIM, "%s  last 5m", d);
    pushLine(p, LC::DIM, "%s", w);
    pushLine(p, LC::DIM, "%s", b);
    if (flags) {
        char ioc[48];
        buildIocString(flags, " IOC:", ioc, sizeof(ioc));
        pushLine(p, indicatorCount(flags) >= 3 ? LC::ALERT : LC::FG, "%s", ioc);
    }
}

static void pushVulnProbeRows(int p, uint8_t maxRows) {
    uint8_t count = DefensePipeline::snapshot().getVulnProbeCount();
    const auto* cache = DefensePipeline::snapshot().getVulnProbeCache();
    if (!cache || count == 0) {
        pushLine(p, LC::DIM, " no potfile probe matches");
        return;
    }

    uint8_t order[Recon::MAX_PROBE_VULN_CACHE];
    uint8_t n = 0;
    for (uint8_t i = 0; i < count && i < Recon::MAX_PROBE_VULN_CACHE; i++) {
        if (cache[i].lastSeen == 0) continue;
        order[n++] = i;
    }
    uint32_t now = millis();
    for (uint8_t i = 0; i < n; i++) {
        for (uint8_t j = i + 1; j < n; j++) {
            if ((now - cache[order[j]].lastSeen) < (now - cache[order[i]].lastSeen)) {
                uint8_t tmp = order[i];
                order[i] = order[j];
                order[j] = tmp;
            }
        }
    }

    pushLine(p, LC::FG, " %u vulnerable probe client(s)", (unsigned)n);
    for (uint8_t row = 0; row < n && row < maxRows; row++) {
        const auto& m = cache[order[row]];
        char age[8];
        formatAge(m.lastSeen, age, sizeof(age));
        pushLine(p, LC::ALERT,
            " %02X%02X%02X %-18.18s %4d %s",
            m.clientMac[3], m.clientMac[4], m.clientMac[5],
            m.ssid, m.rssi, age);
    }
}

static void pushHostileClientRows(int p, uint8_t maxRows) {
    uint8_t count = DefensePipeline::snapshot().getClientFingerprintCount();
    const auto* fps = DefensePipeline::snapshot().getClientFingerprints();
    if (!fps || count == 0) {
        pushLine(p, LC::DIM, " no hostile client fingerprints");
        return;
    }

    uint8_t order[Recon::MAX_CLIENT_FINGERPRINTS];
    uint8_t n = 0;
    for (uint8_t i = 0; i < count && i < Recon::MAX_CLIENT_FINGERPRINTS; i++) {
        if (fps[i].lastSeen == 0) continue;
        order[n++] = i;
    }
    uint32_t now = millis();
    for (uint8_t i = 0; i < n; i++) {
        for (uint8_t j = i + 1; j < n; j++) {
            const auto& a = fps[order[i]];
            const auto& b = fps[order[j]];
            bool swap = (b.hostileScore > a.hostileScore) ||
                        (b.hostileScore == a.hostileScore &&
                         (now - b.lastSeen) < (now - a.lastSeen));
            if (swap) {
                uint8_t tmp = order[i];
                order[i] = order[j];
                order[j] = tmp;
            }
        }
    }

    pushLine(p, LC::FG, " %u client fingerprint(s)", (unsigned)n);
    for (uint8_t row = 0; row < n && row < maxRows; row++) {
        const auto& cf = fps[order[row]];
        char age[8];
        formatAge(cf.lastSeen, age, sizeof(age));
        const char* tool = Recon::deauthToolLabel((Recon::DeauthTool)cf.toolSignature);
        pushLine(p, cf.hostileScore >= 60 ? LC::ALERT : LC::DIM,
            " %02X%02X%02X %-10.10s score:%3u ch%02u",
            cf.clientMac[3], cf.clientMac[4], cf.clientMac[5],
            cf.label[0] ? cf.label : "client",
            (unsigned)cf.hostileScore, (unsigned)cf.channel);
        pushLine(p, LC::DIM, "   %ddB %s tool:%s ie:%08lX",
            cf.rssi, age, tool, (unsigned long)cf.fingerprintHash);
    }
}

// ==[ RING BUFFER OPS ]==

static void clearPane(int p) {
    paneWriteHead[p] = 0;
    paneLineCount[p] = 0;
    paneScrollOffset[p] = 0;
    if (paneBuffers[p]) {
        memset(paneBuffers[p], 0, RING_LINES * sizeof(PaneLine));
    }
}

static void pushLine(int p, LC color, const char* fmt, ...) {
    if (!paneBuffers[p]) return;
    PaneLine& line = paneBuffers[p][paneWriteHead[p]];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line.text, LINE_LEN, fmt, args);
    va_end(args);
    line.color = color;
    line.tappable = false;
    line.actionId = 0;
    memset(line.trackerPayloadHash, 0, sizeof(line.trackerPayloadHash));
    line.hasTrackerIdentity = false;
    line.serialBleSuspect = false;
    paneWriteHead[p] = (paneWriteHead[p] + 1) % RING_LINES;
    if (paneLineCount[p] < RING_LINES) paneLineCount[p]++;
}

static void pushTappable(int p, LC color, uint8_t actionId, const char* fmt, ...) {
    if (!paneBuffers[p]) return;
    PaneLine& line = paneBuffers[p][paneWriteHead[p]];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line.text, LINE_LEN, fmt, args);
    va_end(args);
    line.color = color;
    line.tappable = true;
    line.actionId = actionId;
    memset(line.trackerPayloadHash, 0, sizeof(line.trackerPayloadHash));
    line.hasTrackerIdentity = false;
    line.serialBleSuspect = false;
    paneWriteHead[p] = (paneWriteHead[p] + 1) % RING_LINES;
    if (paneLineCount[p] < RING_LINES) paneLineCount[p]++;
}

static void attachTrackerIdentity(int p, const Recon::TrackerEntry& tracker,
                                  bool serialBleSuspect) {
    if (!paneBuffers[p] || paneLineCount[p] == 0) return;

    const int lineIndex = (paneWriteHead[p] + RING_LINES - 1) % RING_LINES;
    PaneLine& line = paneBuffers[p][lineIndex];
    if (!line.tappable) return;

    memcpy(line.trackerPayloadHash, tracker.payloadHash,
           sizeof(line.trackerPayloadHash));
    line.hasTrackerIdentity = true;
    line.serialBleSuspect = serialBleSuspect;
}

// ==[ PSRAM ALLOC ]==

static bool allocBuffers() {
    for (int i = 0; i < PANE_COUNT; i++) {
        if (paneBuffers[i]) continue;
        paneBuffers[i] = (PaneLine*)heap_caps_malloc(
            RING_LINES * sizeof(PaneLine), MALLOC_CAP_SPIRAM);
        if (!paneBuffers[i]) return false;
        memset(paneBuffers[i], 0, RING_LINES * sizeof(PaneLine));
    }
    return true;
}

static void freeBuffers() {
    for (int i = 0; i < PANE_COUNT; i++) {
        if (paneBuffers[i]) {
            heap_caps_free(paneBuffers[i]);
            paneBuffers[i] = nullptr;
        }
        paneWriteHead[i] = 0;
        paneLineCount[i] = 0;
        paneScrollOffset[i] = 0;
    }
}

// ==[ SMART ENTRY PANE ]==

static Pane selectEntryPane() {
    if (DefensePipeline::snapshot().getFollowingCount() > 0) return Pane::BLE;
    if (DefensePipeline::snapshot().isDeauthActive() || DefensePipeline::snapshot().isEvilTwinActive()) return Pane::RF;
    if (DefensePipeline::snapshot().hasActiveAttacker()) return Pane::FUSION;
    if (DefensePipeline::snapshot().getForensicLogCount() >= 5) return Pane::LOG;
    return Pane::SITREP;
}

// ==[ SECTION HEADER HELPER ]== opencode-style accent bar

static void pushSection(int p, const char* title) {
    // blank line before section
    pushLine(p, LC::DIM, "");
    // accent bar (rendered as filled rect in drawContent)
    pushLine(p, LC::BAR, "%s", title);
    // blank line after section
    pushLine(p, LC::DIM, "");
}

// HM-10/0xFFE0 is a generic serial-BLE profile. It is useful ATM/POI
// inspection evidence, but it is not enough to identify a skimmer. Keep this
// signal recent so a device heard on a previous block does not remain a live
// warning while the operator has moved on.
static constexpr uint32_t SERIAL_BLE_RECENT_MS = 90000;

static bool isRecentSerialBleSuspect(const Recon::TrackerEntry& tracker,
                                     uint32_t now) {
    return tracker.type == Recon::ThreatType::SUSPICIOUS_PERIPHERAL &&
           tracker.lastSeen != 0 &&
           (now - tracker.lastSeen) <= SERIAL_BLE_RECENT_MS;
}

static int getRecentSerialBleSuspectCount() {
    const auto* trackers = DefensePipeline::snapshot().getTrackers();
    const int tableSize = DefensePipeline::snapshot().getTrackerTableSize();
    if (!trackers || tableSize <= 0) return 0;

    const uint32_t now = millis();
    int count = 0;
    for (int i = 0; i < tableSize; ++i) {
        if (isRecentSerialBleSuspect(trackers[i], now)) ++count;
    }
    return count;
}

// A single, ordered explanation is more usable at entry than a raw list of
// every active flag.  The detailed panes retain every individual observation;
// this only tells the operator which correlation currently deserves attention.
static void pushCurrentCorrelation(int p, uint8_t threat) {
    const int following = DefensePipeline::snapshot().getFollowingCount();
    const int serialBle = getRecentSerialBleSuspectCount();
    const int attackers = DefensePipeline::snapshot().getAttackerCount();
    const int highCohorts = DefensePipeline::snapshot().getHighConfidenceCohortCount();

    if (DefensePipeline::snapshot().isDualBandStalkActive()) {
        pushLine(p, LC::ALERT, " WHY: DUAL-BAND STALK CORRELATION");
        pushTappable(p, LC::ALERT, SITREP_ACTION_FUSION,
                     " NEXT: inspect fused profile");
    } else if (following > 0) {
        pushLine(p, LC::ALERT, " WHY: %d BLE FOLLOWING", following);
        pushTappable(p, LC::ALERT, SITREP_ACTION_BLE,
                     " NEXT: inspect / track BLE");
    } else if (DefensePipeline::snapshot().isDeauthActive()) {
        uint8_t channel = DefensePipeline::snapshot().getLastDeauthDominantChannel();
        if (channel == 0) channel = DefensePipeline::snapshot().getLastDeauthChannel();
        const bool huntMatch = channel > 0 && channel == Hunt::getCurrentChannel();
        pushLine(p, LC::ALERT, " WHY: DEAUTH ACTIVE ch%02u%s", channel,
                 huntMatch ? " / HUNT" : "");
        pushTappable(p, LC::ALERT, SITREP_ACTION_SIGINT,
                     " NEXT: inspect deauth burst");
    } else if (DefensePipeline::snapshot().isEvilTwinActive()) {
        pushLine(p, LC::ALERT, " WHY: EVIL TWIN ACTIVE");
        pushTappable(p, LC::ALERT, SITREP_ACTION_LOG,
                     " NEXT: inspect forensic event");
    } else if (DefensePipeline::snapshot().isKarmaConfirmed() || DefensePipeline::snapshot().isKarmaActive()) {
        pushLine(p, LC::ALERT, " WHY: KARMA HONEYPOT%s",
                 DefensePipeline::snapshot().isKarmaConfirmed() ? " CONFIRMED" : " ACTIVE");
        pushTappable(p, LC::ALERT, SITREP_ACTION_LOG,
                     " NEXT: inspect forensic event");
    } else if (attackers > 0) {
        pushLine(p, threat >= 2 ? LC::ALERT : LC::FG,
                 " WHY: %d FUSED ATTACKER PROFILE%s", attackers,
                 attackers == 1 ? "" : "S");
        pushTappable(p, LC::FG, SITREP_ACTION_FUSION,
                     " NEXT: inspect fused profile");
    } else if (highCohorts > 0) {
        pushLine(p, LC::FG, " WHY: %d HIGH-CONF COHORT%s", highCohorts,
                 highCohorts == 1 ? "" : "S");
        pushTappable(p, LC::FG, SITREP_ACTION_FUSION,
                     " NEXT: inspect cohort evidence");
    } else if (serialBle > 0) {
        // Serial BLE is an inspection cue, never a positive skimmer label.
        pushLine(p, LC::FG, " WHY: %d SERIAL-BLE PROFILE%s", serialBle,
                 serialBle == 1 ? "" : "S");
        pushTappable(p, LC::FG, SITREP_ACTION_BLE,
                     " NEXT: inspect / track BLE");
    } else {
        pushLine(p, LC::DIM, " WHY: NO CURRENT HIGH-RISK CORRELATION");
        pushTappable(p, LC::FG, SITREP_ACTION_HUNT,
                     " NEXT: map current RF surface");
    }
}

static void pushSitrepSection(int p, const char* title) {
    pushLine(p, LC::BAR, "%s", title);
}

// ============================================================
// ==[ PANE REFRESH FUNCTIONS ]================================
// ============================================================

static void refreshSitrep() {
    int p = (int)Pane::SITREP;
    clearPane(p);

    uint8_t threat = DefhogTerminal::computeThreatLevel();
    pushLine(p, threat >= 2 ? LC::ALERT : LC::FG,
        " THREAT POSTURE: %s", threatLabels[threat]);

    // Keep orientation data in one 20-row field.  The old SITREP could grow
    // past the viewport, leaving its posture and evidence freshness above the
    // live scroll position on entry.
    pushSitrepSection(p, "NOW // SOURCE HEALTH");
    const auto pipe = DefensePipeline::snapshot().getWifiPipelineStatus();
    const int reconAPs = DefensePipeline::snapshot().getWifiSnapshotCount();
    char wifiAge[8];
    formatAge(pipe.lastCompleteMs, wifiAge, sizeof(wifiAge));
    const LC pipeColor = pipe.state == Recon::WifiPipelineState::FAILED ? LC::ALERT : LC::FG;
    pushLine(p, pipeColor, " WIFI:%d AP  %s  last:%s", reconAPs,
             Recon::wifiPipelineStateLabel(pipe.state), wifiAge);

    const int trackers = DefensePipeline::snapshot().getTrackerCount();
    const int following = DefensePipeline::snapshot().getFollowingCount();
    pushLine(p, following > 0 ? LC::ALERT : LC::FG,
             " BLE:%d tracked  %d following", trackers, following);

    if (GPS::isInitialized()) {
        if (GPS::hasFix()) {
            pushLine(p, LC::FG, " GPS:LOCK %u sat  age:%lus",
                (unsigned)GPS::getSatCount(),
                (unsigned long)(GPS::getFixAgeMs() / 1000));
        } else {
            pushLine(p, LC::DIM, " GPS:%s %u sat%s",
                GPS::hasNMEA() ? "NMEA" : "WAIT", (unsigned)GPS::getSatCount(),
                GPS::hasNMEA() ? " / no fix" : "");
        }
    } else {
        pushLine(p, LC::DIM, " GPS:OFF (no active feed)");
    }

    pushSitrepSection(p, "WHY // NEXT EVIDENCE");
    pushCurrentCorrelation(p, threat);

    const uint32_t deauths = DefensePipeline::snapshot().getDeauthCount();
    if (deauths > 0) {
        pushLine(p, DefensePipeline::snapshot().isDeauthActive() ? LC::ALERT : LC::DIM,
                 " DEAUTH:%lu frame%s%s", (unsigned long)deauths,
                 deauths == 1 ? "" : "s",
                 DefensePipeline::snapshot().isDeauthActive() ? " / ACTIVE" : "");
    } else {
        pushLine(p, LC::DIM, " DEAUTH: no session frames");
    }

    const int attackers = DefensePipeline::snapshot().getAttackerCount();
    int cohorts = DefensePipeline::snapshot().getCohortCount();
    int highCohorts = DefensePipeline::snapshot().getHighConfidenceCohortCount();
    int persistent = DefensePipeline::snapshot().getPersistentClientCount();
    if (attackers > 0 || cohorts > 0 || persistent > 0 || DefensePipeline::snapshot().isDualBandStalkActive()) {
        pushLine(p, DefensePipeline::snapshot().hasCriticalIntel() ? LC::ALERT : LC::FG,
            " XBAND: atk:%d cohort:%d/%d persist:%d%s",
            attackers, highCohorts, cohorts, persistent,
            DefensePipeline::snapshot().isDualBandStalkActive() ? " STALK" : "");
    } else {
        pushLine(p, LC::DIM, " XBAND: no active fusion correlation");
    }

    char flags[LINE_LEN] = " FLAGS:";
    if (DefensePipeline::snapshot().isEvilTwinActive()) appendToken(flags, sizeof(flags), "TWIN ");
    if (DefensePipeline::snapshot().isKarmaActive()) appendToken(flags, sizeof(flags), "KARMA ");
    if (DefensePipeline::snapshot().isKarmaConfirmed()) appendToken(flags, sizeof(flags), "KCONF ");
    if (DefensePipeline::snapshot().isCanaryTripped()) appendToken(flags, sizeof(flags), "CANARY ");
    if (DefensePipeline::snapshot().isFingerprintMismatchActive()) appendToken(flags, sizeof(flags), "FP ");
    if (DefensePipeline::snapshot().isSeqAnomalyActive()) appendToken(flags, sizeof(flags), "SEQ ");
    if (DefensePipeline::snapshot().isRssiAnomalyActive()) appendToken(flags, sizeof(flags), "RSSI ");
    if (DefensePipeline::snapshot().getVulnProbeCount() > 0) appendToken(flags, sizeof(flags), "PROBE ");
    if (DefensePipeline::snapshot().getClientFingerprintCount() > 0) appendToken(flags, sizeof(flags), "HOSTILE ");
    if (strcmp(flags, " FLAGS:") != 0) {
        pushLine(p, threat >= 2 ? LC::ALERT : LC::FG, "%s", flags);
    } else {
        pushLine(p, LC::DIM, " FLAGS: no active forensic indicator");
    }

    pushSitrepSection(p, "SURROUNDINGS // CROWD");

    uint16_t pop = DefensePipeline::snapshot().getEstimatedPopulation();
    auto tier = DefensePipeline::snapshot().getCrowdTier();
    auto trend = DefensePipeline::snapshot().getCrowdTrend();
    const char* tierStr = "?";
    switch (tier) {
        case XBand::CrowdTier::DESERTED: tierStr = "DESERTED"; break;
        case XBand::CrowdTier::SPARSE:   tierStr = "SPARSE"; break;
        case XBand::CrowdTier::BUSY:     tierStr = "BUSY"; break;
        case XBand::CrowdTier::CROWDED:  tierStr = "CROWDED"; break;
        case XBand::CrowdTier::PACKED:   tierStr = "PACKED"; break;
    }
    const char* trendStr = "=";
    switch (trend) {
        case XBand::CrowdTrend::GROWING:   trendStr = "\x18"; break;  // up arrow
        case XBand::CrowdTrend::SHRINKING: trendStr = "\x19"; break;  // down arrow
        default: break;
    }
    pushLine(p, LC::FG, " EST POP: %d (%s %s)", pop, tierStr, trendStr);
    const auto* crowd = DefensePipeline::snapshot().getCurrentCrowd();
    if (crowd) {
        pushLine(p, LC::DIM, " BLE:%dph %dwa %dta WiFi:%dcl %dAP",
            crowd->blePhones, crowd->bleWatches, crowd->bleTags,
            crowd->wifiClients, crowd->wifiAPs);
        pushLine(p, LC::DIM, " RANGE: min:%d peak:%d",
            DefensePipeline::snapshot().getSessionMinPop(), DefensePipeline::snapshot().getSessionPeakPop());
    } else {
        pushLine(p, LC::DIM, " CROWD: no current fusion snapshot");
        pushLine(p, LC::DIM, " RANGE: awaiting enough observations");
    }

    pushSitrepSection(p, "SURFACE // YIELD");

    // Prefer Hunt's richer table for exposure details, but label the provenance
    // rather than treating a retained table as a fresh Recon sweep.
    const uint16_t huntNets = Hunt::getNetworkCount();
    const auto* networks = Hunt::getNetworks();
    const bool hasHuntTable = huntNets > 0 && networks;
    const int surfaceCount = hasHuntTable ? huntNets : reconAPs;
    const auto* wifiAPs = DefensePipeline::snapshot().getWifiSnapshot();
    int openCount = 0, wpsCount = 0, pmfCount = 0;
    if (hasHuntTable) {
        for (uint16_t i = 0; i < huntNets && i < MAX_HUNT_NETWORKS; ++i) {
            if (networks[i].authmode == 0) ++openCount;
            if (networks[i].wpsState > 0 && !networks[i].wpsLocked) ++wpsCount;
            if (networks[i].hasPMF) ++pmfCount;
        }
        pushLine(p, LC::FG, " AP:%d CLIENT:%d  HUNT TABLE", surfaceCount,
                 (int)Hunt::getClientCount());
        pushLine(p, LC::DIM, " OPEN:%d WPS:%d PMF gap:%d/%d", openCount, wpsCount,
                 surfaceCount - pmfCount, surfaceCount);
    } else {
        for (int i = 0; wifiAPs && i < reconAPs; ++i) {
            if (wifiAPs[i].authMode == 0) ++openCount;
        }
        pushLine(p, LC::FG, " AP:%d  RECON SNAPSHOT", surfaceCount);
        pushLine(p, LC::DIM, " OPEN:%d  PMF/WPS: need HUNT", openCount);
    }

    const uint16_t sPmk = Hunt::getSessionPMKIDs();
    const uint16_t sHs  = Hunt::getSessionHandshakes();
    const uint16_t tPmk = Capture::getPMKIDCount();
    const uint16_t tHs  = Capture::getHandshakeCount();
    pushLine(p, LC::FG, " SESSION:+%d pmk +%d hs", sPmk, sHs);
    pushLine(p, LC::DIM, " CAREER:%d pmk %d hs (%d total)",
             tPmk, tHs, tPmk + tHs);
}

static void refreshSigint() {
    int p = (int)Pane::RF;
    clearPane(p);

    uint8_t huntCh = Hunt::getCurrentChannel();

    pushSection(p, "CHANNEL YIELD");

    // show channels with data, sorted by captures
    struct ChEntry { uint8_t ch; uint16_t nets; uint16_t caps; int8_t rssi; };
    ChEntry entries[14];
    int entryCount = 0;
    for (uint8_t ch = 1; ch <= 13; ch++) {
        const auto* cs = Hunt::getChannelStats(ch);
        if (!cs || cs->networkCount == 0) continue;
        entries[entryCount++] = {ch, cs->networkCount,
            (uint16_t)(cs->pmkidHits + cs->handshakeHits), 0};
    }

    if (entryCount > 0) {
        // sort by captures desc
        for (int i = 0; i < entryCount - 1; i++) {
            for (int j = i + 1; j < entryCount; j++) {
                if (entries[j].caps > entries[i].caps) {
                    ChEntry tmp = entries[i];
                    entries[i] = entries[j];
                    entries[j] = tmp;
                }
            }
        }

        pushLine(p, LC::DIM, " CH  NETS  CAPS  HUNT");
        for (int i = 0; i < entryCount && i < 8; i++) {
            const auto& e = entries[i];
            pushLine(p, e.ch == huntCh ? LC::FG : LC::DIM,
                " %s%02d  %-4d  %-4d  %s",
                e.ch == huntCh ? "*" : " ",
                e.ch, e.nets, e.caps,
                e.ch == huntCh ? "<<<" : "");
        }
    } else {
        // no hunt data — show Recon WiFi per-channel distribution
        int wifiCount = DefensePipeline::snapshot().getWifiSnapshotCount();
        const auto* wifiAPs = DefensePipeline::snapshot().getWifiSnapshot();
        uint8_t chCount[14] = {};
        for (int i = 0; i < wifiCount; i++) {
            uint8_t ch = wifiAPs[i].channel;
            if (ch >= 1 && ch <= 13) chCount[ch]++;
        }
        pushLine(p, LC::DIM, " CH  APs  (recon scan)");
        for (uint8_t ch = 1; ch <= 13; ch++) {
            if (chCount[ch] == 0) continue;
            pushLine(p, LC::DIM, "  %02d  %-4d", ch, chCount[ch]);
        }
        if (wifiCount == 0) pushLine(p, LC::DIM, " no scan data yet");
    }

    pushSection(p, "DEAUTH HISTORY");

    pushDeauthBurstRows(p, 6, false);

    pushSection(p, "WIFI CENSUS");

    int wifiCount = DefensePipeline::snapshot().getWifiSnapshotCount();
    const auto* wifiAPs = DefensePipeline::snapshot().getWifiSnapshot();
    pushLine(p, LC::FG, " %d APs in last scan", wifiCount);
    // show top 6 by RSSI
    for (int i = 0; i < wifiCount && i < 6; i++) {
        const auto& ap = wifiAPs[i];
        char ssidBuf[18];
        strncpy(ssidBuf, ap.ssid[0] ? ap.ssid : "<hidden>", 17);
        ssidBuf[17] = '\0';
        pushLine(p, LC::DIM, " %02d %-17s %ddBm %s",
            ap.channel, ssidBuf, ap.rssi,
            ap.authMode == 0 ? "OPEN" : "");
    }

    pushSection(p, "RF POSTURE");

    int openCount = 0;
    int lowEntropy = 0;
    int weakAuthLowEntropy = 0;
    for (int i = 0; i < wifiCount; i++) {
        const auto& ap = wifiAPs[i];
        bool open = (ap.authMode == 0);
        bool low = (ap.entropyScore > 0 && ap.entropyScore < 50);
        if (open) openCount++;
        if (low) lowEntropy++;
        if (open && low) weakAuthLowEntropy++;
    }
    pushLine(p, (lowEntropy > 0 || weakAuthLowEntropy > 0) ? LC::ALERT : LC::FG,
        " open:%d known:%d lowIE:%d weak+low:%d",
        openCount, DefensePipeline::snapshot().getKnownAPCount(), lowEntropy, weakAuthLowEntropy);
    pushLine(p, LC::DIM, " scan:%lus sniff:%lums sentinel:%lums",
        (unsigned long)(DefensePipeline::snapshot().getCurrentWifiScanIntervalMs() / 1000),
        (unsigned long)DefensePipeline::snapshot().getCurrentDeauthSniffMs(),
        (unsigned long)DefensePipeline::snapshot().getCurrentSentinelIntervalMs());

    int entropyShown = 0;
    for (int i = 0; i < wifiCount && entropyShown < 4; i++) {
        const auto& ap = wifiAPs[i];
        if (ap.entropyScore == 0 || ap.entropyScore >= 50) continue;
        pushLine(p, LC::ALERT, " lowIE:%02d %-18.18s e:%d %ddB",
            ap.channel, ap.ssid[0] ? ap.ssid : "<hidden>",
            ap.entropyScore, ap.rssi);
        entropyShown++;
    }

    pushSection(p, "PROBE RISK");
    pushVulnProbeRows(p, 4);

    pushSection(p, "HOSTILE CLIENTS");
    pushHostileClientRows(p, 4);
}

static void refreshBle() {
    int p = (int)Pane::BLE;
    clearPane(p);
    uint32_t now = millis();

    int trackerCount = DefensePipeline::snapshot().getTrackerCount();
    int following = DefensePipeline::snapshot().getFollowingCount();
    int spam = DefensePipeline::snapshot().getSpamCount();

    pushSection(p, "BLE TRACKERS");

    // summary: counts + cadence tier
    const char* cadStr = "NORM";
    switch (DefensePipeline::snapshot().getCadenceTier()) {
        case Recon::CadenceTier::AGGRESSIVE: cadStr = "AGG"; break;
        case Recon::CadenceTier::ELEVATED:   cadStr = "ELEV"; break;
        default: break;
    }
    pushLine(p, LC::FG, " %d tracked  %d follow  %d spam [%s]",
        trackerCount, following, spam, cadStr);

    // scan status: ble init + last scan age + total seen
    char bleAge[8] = "--";
    uint32_t lastBle = DefensePipeline::snapshot().getLastBLEScanTime();
    if (lastBle > 0) {
        uint32_t ago = (now - lastBle) / 1000;
        if (ago < 60) snprintf(bleAge, sizeof(bleAge), "%ds", (int)ago);
        else if (ago < 3600) snprintf(bleAge, sizeof(bleAge), "%dm", (int)(ago / 60));
        else snprintf(bleAge, sizeof(bleAge), "%dh", (int)(ago / 3600));
    }
    pushLine(p, LC::DIM, " ble:%s  last:%s ago  seen:%d",
        DefensePipeline::snapshot().isBleInitialized() ? "ON" : "OFF",
        bleAge, DefensePipeline::snapshot().getTotalBLEDevicesSeen());

    const auto* trackers = DefensePipeline::snapshot().getTrackers();
    int tableSize = DefensePipeline::snapshot().getTrackerTableSize();
    const int serialBleSuspects = getRecentSerialBleSuspectCount();

    // spam platform short tag
    auto spamPlat = [](uint8_t sp) -> const char* {
        switch ((Recon::SpamPlatform)sp) {
            case Recon::SpamPlatform::IOS:     return "SP:i";
            case Recon::SpamPlatform::WINDOWS: return "SP:W";
            case Recon::SpamPlatform::SAMSUNG: return "SP:S";
            case Recon::SpamPlatform::ANDROID: return "SP:A";
            default: return "";
        }
    };

    // detail line: seenCount, age, MAC rotations, distance/manufacturer
    auto pushDetail = [&](const Recon::TrackerEntry& t) {
        // age since last seen
        char age[8] = "--";
        if (t.lastSeen > 0) {
            uint32_t ago = (now - t.lastSeen) / 1000;
            if (ago < 60) snprintf(age, sizeof(age), "%ds", (int)ago);
            else if (ago < 3600) snprintf(age, sizeof(age), "%dm", (int)(ago / 60));
            else snprintf(age, sizeof(age), "%dh", (int)(ago / 3600));
        }

        // extra: distance > manufacturer > spam platform > appearance
        char extra[24] = "";
        if (t.txPower != 0) {
            float dist = Recon::estimateDistance(t.rssi, t.txPower);
            if (dist > 0 && dist < 100)
                snprintf(extra, sizeof(extra), "~%.1fm", dist);
        }
        if (extra[0] == '\0' && t.companyId != 0) {
            const char* mfr = Recon::manufacturerLabel(t.companyId);
            if (mfr[0] != '?') snprintf(extra, sizeof(extra), "%.12s", mfr);
        }
        if (extra[0] == '\0' && t.appearance != 0) {
            const char* app = Recon::appearanceLabel(t.appearance);
            if (app[0] != '?') snprintf(extra, sizeof(extra), "%.12s", app);
        }
        // append spam platform tag
        if (t.flags & Recon::FLAG_SPAM) {
            const char* sp = spamPlat(t.spamPlatform);
            if (sp[0]) {
                size_t len = strlen(extra);
                if (len > 0 && len < sizeof(extra) - 6) { extra[len] = ' '; len++; }
                snprintf(extra + len, sizeof(extra) - len, "%s", sp);
            }
        }

        pushLine(p, LC::DIM, "   x%-3d %-5s rot:%-2d %s",
            t.seenCount, age, t.macChangeCount, extra);
        if (t.flags & Recon::FLAG_RELAY_SUSPECT) {
            pushLine(p, LC::ALERT, "   relay jitter:%ums adv:%ums",
                (unsigned)t.intervalVariance,
                (unsigned)t.measuredAdvIntervalMs);
        }
    };

    // ==[ SERIAL BLE / POINT-OF-SALE CHECK ]==
    // A serial-BLE (HM-10 / FFE0) advertisement is a useful physical-inspection
    // cue around an ATM or payment terminal, never a positive skimmer verdict.
    // It is intentionally the first actionable block in the BLE pane.
    pushSection(p, "SERIAL BLE / POI CHECK");
    if (serialBleSuspects == 0) {
        pushLine(p, LC::DIM, " no nearby serial-BLE evidence");
    } else {
        pushLine(p, LC::ALERT,
            " %d serial BLE nearby -- inspect site", serialBleSuspects);
        for (int i = 0; i < tableSize; ++i) {
            const auto& t = trackers[i];
            if (!isRecentSerialBleSuspect(t, now)) continue;

            char nameBuf[12];
            if (t.name[0]) snprintf(nameBuf, sizeof(nameBuf), "\"%.8s\"", t.name);
            else snprintf(nameBuf, sizeof(nameBuf), "..%02X%02X%02X",
                          t.mac[3], t.mac[4], t.mac[5]);

            pushTappable(p, LC::ALERT, i,
                " SERIAL BLE %4ddB %-5s %-10s",
                t.rssiSmooth, Recon::proximityLabel(t.rssiSmooth), nameBuf);
            attachTrackerIdentity(p, t, true);
            pushLine(p, LC::DIM,
                "   svc:%04X seen:%u rot:%u -- TAP TRACK",
                t.primaryService, (unsigned)t.seenCount,
                (unsigned)t.macChangeCount);
        }
    }

    // 2-line per tracker: main (tappable) + detail
    int shown = 0;
    constexpr int MAX_SHOWN = 10;

    // pass 1: following trackers (ALERT)
    for (int i = 0; i < tableSize && shown < MAX_SHOWN; i++) {
        const auto& t = trackers[i];
        if (t.type == Recon::ThreatType::UNKNOWN && t.seenCount == 0) continue;
        if (t.type == Recon::ThreatType::SUSPICIOUS_PERIPHERAL) continue;
        if (!(t.flags & Recon::FLAG_FOLLOWING)) continue;

        char nameBuf[12];
        if (t.name[0]) snprintf(nameBuf, sizeof(nameBuf), "\"%.8s\"", t.name);
        else nameBuf[0] = '\0';

        pushTappable(p, LC::ALERT, i,
            " %-6s %4ddB %-5s %-10s FOLLOW",
            Recon::deviceLabel(t), t.rssi,
            Recon::proximityLabel(t.rssi), nameBuf);
        attachTrackerIdentity(p, t, false);
        pushDetail(t);
        shown++;
    }
    // pass 2: others
    for (int i = 0; i < tableSize && shown < MAX_SHOWN; i++) {
        const auto& t = trackers[i];
        if (t.type == Recon::ThreatType::UNKNOWN && t.seenCount == 0) continue;
        if (t.type == Recon::ThreatType::SUSPICIOUS_PERIPHERAL) continue;
        if (t.flags & Recon::FLAG_FOLLOWING) continue;
        bool isSpam = (t.flags & Recon::FLAG_SPAM);
        bool isWatch = (t.flags & Recon::FLAG_WATCHLISTED);
        bool isRelay = (t.flags & Recon::FLAG_RELAY_SUSPECT);
        bool isStep = (t.flags & Recon::FLAG_STEP_FOLLOWING);

        char nameBuf[12];
        if (t.name[0]) snprintf(nameBuf, sizeof(nameBuf), "\"%.8s\"", t.name);
        else nameBuf[0] = '\0';

        char flags[24] = "";
        if (isSpam) strcat(flags, "SPAM ");
        if (isRelay) strcat(flags, "RELAY ");
        if (isStep) strcat(flags, "STEP ");
        if (isWatch) strcat(flags, "WATCH");

        pushTappable(p, (isSpam || isRelay || isStep) ? LC::FG : LC::DIM, i,
            " %-6s %4ddB %-5s %-10s %s",
            Recon::deviceLabel(t), t.rssi,
            Recon::proximityLabel(t.rssi), nameBuf, flags);
        attachTrackerIdentity(p, t, false);
        pushDetail(t);
        shown++;
    }

    // ==[ BLE CATALOG ]== full device table (includes unclassified devices)
    const auto* catalog = DefensePipeline::snapshot().getBleDevices();
    int catSize = DefensePipeline::snapshot().getBleDeviceTableSize();
    int catShown = 0;
    constexpr int MAX_CAT = 8;

    if (catSize > 0) {
        pushSection(p, "BLE CATALOG");
        pushLine(p, LC::FG, " %d devices in range", catSize);

        for (int i = 0; i < catSize && catShown < MAX_CAT; i++) {
            const auto& d = catalog[i];
            if (d.seenCount == 0) continue;

            // skip entries already shown in tracker table
            bool inTrackerTable = false;
            for (int t = 0; t < tableSize; t++) {
                if (memcmp(trackers[t].payloadHash, d.payloadHash, 4) == 0) {
                    inTrackerTable = true;
                    break;
                }
            }
            if (inTrackerTable) continue;

            // name or manufacturer or appearance or MAC tail
            char label[16] = "";
            if (d.name[0]) {
                snprintf(label, sizeof(label), "\"%.10s\"", d.name);
            } else if (d.companyId != 0) {
                const char* mfr = Recon::manufacturerLabel(d.companyId);
                if (mfr[0] != '?') snprintf(label, sizeof(label), "%.12s", mfr);
            }
            if (label[0] == '\0' && d.appearance != 0) {
                const char* app = Recon::appearanceLabel(d.appearance);
                if (app[0] != '?') snprintf(label, sizeof(label), "%.12s", app);
            }
            if (label[0] == '\0') {
                snprintf(label, sizeof(label), "..%02X:%02X:%02X",
                    d.mac[3], d.mac[4], d.mac[5]);
            }

            // type from metadata cascade, "BLE" for unclassified
            const char* tl = Recon::deviceLabel(d);

            pushLine(p, LC::DIM, " %-5s %4ddB %-5s %s",
                tl, d.rssi, Recon::proximityLabel(d.rssi), label);
            catShown++;
        }
    }

    // watchlist section
    pushSection(p, "WATCHLIST");
    uint8_t wlCount = DefensePipeline::snapshot().getWatchlistCount();
    const auto* wl = DefensePipeline::snapshot().getWatchlist();
    if (wlCount == 0) {
        pushLine(p, LC::DIM, " (empty -- manage in PIG EARS)");
    } else {
        for (int i = 0; i < Recon::MAX_WATCHLIST; i++) {
            if (wl[i].occupied) {
                pushTappable(p, wl[i].present ? LC::FG : LC::DIM,
                    0x80 | i,  // high bit = watchlist action
                    " [%d] \"%s\" %s  [x]",
                    i, wl[i].label,
                    wl[i].present ? "PRESENT" : "absent");
            }
        }
    }
}

static void refreshFusion() {
    int p = (int)Pane::FUSION;
    clearPane(p);

    pushSection(p, "XBAND INTEL");

    bool dualBand = DefensePipeline::snapshot().isDualBandStalkActive();
    if (dualBand) {
        pushLine(p, LC::ALERT, " !! DUAL-BAND TAILING DETECTED !!");
    }

    int attackerCount = DefensePipeline::snapshot().getAttackerCount();
    if (attackerCount > 0) {
        pushLine(p, LC::FG, " %d attacker(s) fingerprinted", attackerCount);
        const auto* profiles = DefensePipeline::snapshot().getAttackerProfiles();
        for (int i = 0; i < attackerCount && i < 4; i++) {
            const auto& a = profiles[i];
            char dist[12];
            if (a.estimatedDist > 0.0f) snprintf(dist, sizeof(dist), "~%.0fm", a.estimatedDist);
            else snprintf(dist, sizeof(dist), "dist:?");
            const char* type = Recon::threatTypeLabel((Recon::ThreatType)a.bleType);
            pushTappable(p, LC::ALERT, i,
                " [ATK-%d] %-4s %-10.10s %ddB %s",
                i, type, a.bleName[0] ? a.bleName : "???",
                a.bleRssi, dist);
            if (expandedAttacker == i) {
                char firstAge[8];
                char lastAge[8];
                formatAge(a.firstCorrelated, firstAge, sizeof(firstAge));
                formatAge(a.lastCorrelated, lastAge, sizeof(lastAge));
                pushLine(p, LC::DIM, "   bursts: %d  WiFi: %ddBm",
                    a.correlatedBursts, a.wifiDeauthRssi);
                const char* vendor = Recon::manufacturerLabel(a.companyId);
                pushLine(p, LC::DIM, "   vendor: %s  first:%s last:%s",
                    (vendor[0] != '?') ? vendor : "?",
                    firstAge, lastAge);
                if (hasBssid(a.targetBssid)) {
                    const char* ssid = findSsidForBssid(a.targetBssid);
                    pushLine(p, LC::DIM, "   target:%02X:%02X:%02X %.*s",
                        a.targetBssid[3], a.targetBssid[4], a.targetBssid[5],
                        14, (ssid && ssid[0]) ? ssid : "?");
                }
                if (a.estimatedDist > 0.0f) {
                    pushLine(p, LC::DIM, "   hash:%02X%02X%02X%02X dist:%.1fm",
                        a.blePayloadHash[0], a.blePayloadHash[1],
                        a.blePayloadHash[2], a.blePayloadHash[3],
                        a.estimatedDist);
                } else {
                    pushLine(p, LC::DIM, "   hash:%02X%02X%02X%02X dist:unknown",
                    a.blePayloadHash[0], a.blePayloadHash[1],
                    a.blePayloadHash[2], a.blePayloadHash[3]);
                }
            } else {
                pushLine(p, LC::DIM, "   %d burst(s) correlated",
                    a.correlatedBursts);
            }
        }
    } else {
        pushLine(p, LC::DIM, " no attackers fingerprinted");
    }

    pushSection(p, "COHORT PAIRS");

    int cohortCount = DefensePipeline::snapshot().getCohortCount();
    int highConf = DefensePipeline::snapshot().getHighConfidenceCohortCount();
    pushLine(p, LC::FG, " %d pairs (%d high)  persistent:%d",
        cohortCount, highConf, DefensePipeline::snapshot().getPersistentClientCount());

    const auto* cohorts = DefensePipeline::snapshot().getCohortPairs();
    for (int i = 0; i < cohortCount && i < 6; i++) {
        const auto& c = cohorts[i];
        const char* confidence = c.confidence >= 3 ? "HIGH" :
                                 (c.confidence == 2 ? "MED" : "LOW");
        char correlationAge[8];
        formatAge(c.lastCorrelated, correlationAge, sizeof(correlationAge));
        pushLine(p, c.isFollowing ? LC::ALERT : LC::DIM,
            " WiFi:%02X%02X%02X BLE:%02X%02X%02X%02X %-4s",
            c.wifiMac[3], c.wifiMac[4], c.wifiMac[5],
            c.blePayloadHash[0], c.blePayloadHash[1],
            c.blePayloadHash[2], c.blePayloadHash[3],
            confidence);
        pushLine(p, LC::DIM, "   evidence:%d/%ddB dt:%us age:%s",
            c.wifiRssi, c.bleRssi, (unsigned)c.timeDeltaSec, correlationAge);
        if (c.probeSSID[0] || c.isFollowing || c.potfileMatch) {
            pushLine(p, c.potfileMatch ? LC::ALERT : LC::DIM,
                "   ssid:%-16.16s%s%s",
                c.probeSSID[0] ? c.probeSSID : "?",
                c.potfileMatch ? " POT" : "",
                c.isFollowing ? " FOLLOW" : "");
        }
    }

    pushSection(p, "VENDOR ECOSYSTEMS");

    int vendorCount = DefensePipeline::snapshot().getVendorCorrelationCount();
    const auto* vendors = DefensePipeline::snapshot().getVendorCorrelations();
    if (vendorCount == 0 || !vendors) {
        pushLine(p, LC::DIM, " no close vendor clusters");
    } else {
        for (int i = 0; i < vendorCount && i < 6; i++) {
            const auto& v = vendors[i];
            const char* vendor = Recon::manufacturerLabel(v.companyId);
            pushLine(p, v.hasIoT ? LC::ALERT : LC::DIM,
                " %-10.10s BLE:%d AP:%d %d/%ddB%s",
                (vendor[0] != '?') ? vendor : "vendor",
                v.bleDeviceCount, v.wifiAPCount,
                v.closestBleRssi, v.closestWifiRssi,
                v.hasIoT ? " IOT" : "");
        }
    }

    pushSection(p, "CROWD DENSITY");

    const auto* crowd = DefensePipeline::snapshot().getCurrentCrowd();
    if (crowd) {
        const char* trend = "?";
        switch (DefensePipeline::snapshot().getCrowdTrend()) {
            case XBand::CrowdTrend::GROWING: trend = "GROW"; break;
            case XBand::CrowdTrend::STABLE: trend = "FLAT"; break;
            case XBand::CrowdTrend::SHRINKING: trend = "DROP"; break;
            default: break;
        }
        pushLine(p, LC::FG, " EST POP: %d  %s  min:%d peak:%d",
            crowd->estimatedPop, trend,
            DefensePipeline::snapshot().getSessionMinPop(), DefensePipeline::snapshot().getSessionPeakPop());
        pushLine(p, LC::DIM, " BLE: %dph %dwa %dta %dse %dot",
            crowd->blePhones, crowd->bleWatches, crowd->bleTags,
            crowd->bleSensors, crowd->bleOther);
        pushLine(p, LC::DIM, " WiFi: %d clients  %d APs",
            crowd->wifiClients, crowd->wifiAPs);
        if (crowd->appleContinuity > 0) {
            pushLine(p, LC::DIM, " Apple continuity: %d",
                crowd->appleContinuity);
        }
    } else {
        pushLine(p, LC::DIM, " awaiting crowd data...");
    }
}

static void refreshLog() {
    int p = (int)Pane::LOG;
    clearPane(p);

    pushSection(p, "FORENSIC TIMELINE");

    uint8_t logCount = DefensePipeline::snapshot().getForensicLogCount();
    const auto* log = DefensePipeline::snapshot().getForensicLog();
    if (logCount == 0 || !log) {
        pushLine(p, LC::DIM, " no forensic events recorded");
    } else {
    uint8_t logHead = DefensePipeline::snapshot().getForensicLogHead();
    // event type labels
    auto eventLabel = [](Recon::ReconEvent ev) -> const char* {
        switch (ev) {
            case Recon::ReconEvent::DEAUTH_DETECTED:      return "DEAUTH";
            case Recon::ReconEvent::EVIL_TWIN:             return "EVIL_TWIN";
            case Recon::ReconEvent::KARMA_HONEYPOT:        return "KARMA";
            case Recon::ReconEvent::FINGERPRINT_MISMATCH:  return "FP_MISMATCH";
            case Recon::ReconEvent::SEQ_ANOMALY:           return "SEQ_ANOMALY";
            case Recon::ReconEvent::RSSI_ANOMALY:          return "RSSI_ANOMALY";
            case Recon::ReconEvent::TRACKER_FOLLOWING:     return "FOLLOWING";
            case Recon::ReconEvent::TRACKER_NEW:           return "NEW_TRACKER";
            case Recon::ReconEvent::BLE_SPAM:              return "BLE_SPAM";
            case Recon::ReconEvent::COORDINATED_ATTACK:    return "COORD_ATK";
            case Recon::ReconEvent::KNOWN_AP:              return "KNOWN_AP";
            case Recon::ReconEvent::OPEN_AP_WARNING:       return "OPEN_AP";
            case Recon::ReconEvent::PROBE_VULN_CLIENT:     return "VULN_CLIENT";
            case Recon::ReconEvent::ATTACKER_IDENTIFIED:   return "ATK_ID";
            case Recon::ReconEvent::DUAL_BAND_STALK:       return "DUAL_STALK";
            case Recon::ReconEvent::FOLLOWING_NETWORK_ID:  return "FOLLOW_NET";
            case Recon::ReconEvent::KARMA_CONFIRMED:       return "KARMA_CONF";
            case Recon::ReconEvent::CANARY_TRIPPED:        return "CANARY";
            case Recon::ReconEvent::RELAY_SUSPECT:         return "RELAY";
            case Recon::ReconEvent::HOSTILE_CLIENT:        return "HOSTILE";
            case Recon::ReconEvent::TOOL_IDENTIFIED:       return "TOOL_ID";
            case Recon::ReconEvent::LOW_ENTROPY_BEACON:    return "LOW_ENT";
            default: return "EVENT";
        }
    };

    // show newest first (up to 16 entries) — ring buffer: walk backwards from head
    int shown = 0;
    bool expandedLogStillVisible = false;
    for (int k = 0; k < logCount && shown < 16; k++) {
        int idx = (logHead - 1 - k + Recon::MAX_FORENSIC_LOG) % Recon::MAX_FORENSIC_LOG;
        const auto& e = log[idx];
        if (e.timestamp == 0) continue;
        char ageStr[12];
        formatAge(e.timestamp, ageStr, sizeof(ageStr));

        pushTappable(p, isCriticalEvent(e.event) ? LC::ALERT : LC::FG, (uint8_t)idx,
            " %-6s %-12s CH%02d %ddBm",
            ageStr, eventLabel(e.event), e.channel, e.rssi);

        // expanded detail on tap
        if (expandedLogEntry == idx && expandedLogTimestamp == e.timestamp) {
            expandedLogStillVisible = true;
            // BSSID
            bool hasEventBssid = hasBssid(e.bssid);
            if (hasEventBssid) {
                const char* ssid = findSsidForBssid(e.bssid);
                if (ssid && ssid[0]) {
                    pushLine(p, LC::DIM, "   BSSID:%02X:%02X:%02X \"%.*s\"",
                        e.bssid[3], e.bssid[4], e.bssid[5], 16, ssid);
                } else {
                    pushLine(p, LC::DIM, "   BSSID:%02X:%02X:%02X:%02X:%02X:%02X",
                        e.bssid[0], e.bssid[1], e.bssid[2],
                        e.bssid[3], e.bssid[4], e.bssid[5]);
                }
            }
            if (e._pad[0] > 0) {
                pushLine(p, LC::DIM, "   repeat:%u collapsed dupes",
                    (unsigned)e._pad[0]);
            }
            // IOC chain detail
            if (e.indicatorFlags) {
                char iocBuf[48];
                buildIocString(e.indicatorFlags, "   IOC:", iocBuf, sizeof(iocBuf));
                pushLine(p, LC::DIM, "%s", iocBuf);
                // multi-indicator correlation count
                uint8_t iocCount = hasEventBssid
                    ? DefensePipeline::snapshot().countIndicatorsForBSSID(e.bssid, 300000)
                    : indicatorCount(e.indicatorFlags);
                if (iocCount > 1) {
                    pushLine(p, LC::FG, "   %u indicator types (5min)",
                        (unsigned)iocCount);
                }
            }
        } else {
            // collapsed: show IOC flags inline if present
            if (e.indicatorFlags) {
                char iocBuf[48];
                buildIocString(e.indicatorFlags, " IOC:", iocBuf, sizeof(iocBuf));
                pushLine(p, indicatorCount(e.indicatorFlags) >= 3 ? LC::ALERT : LC::DIM,
                    "%s", iocBuf);
            }
        }
        shown++;
    }
    // The log is a live ring. Do not let a selected slot silently expand
    // a newer event after the original record is displaced or scrolled out.
    if (expandedLogEntry >= 0 && !expandedLogStillVisible) {
        clearLogExpansion();
    }
    }

    pushSection(p, "THREAT HEATMAP");
    pushTemporalHeatmap(p);

    pushSection(p, "DEAUTH BURSTS");

    uint8_t order[Recon::MAX_DEAUTH_HISTORY];
    uint8_t burstCount = collectDeauthBurstOrder(order, Recon::MAX_DEAUTH_HISTORY);
    pushLine(p, LC::FG, " %u burst(s) this session", (unsigned)burstCount);

    if (burstCount > 0) {
        uint32_t totalFrames = 0;
        uint16_t peakPps = 0;
        const auto* bursts = DefensePipeline::snapshot().getDeauthBurstHistory();
        for (uint8_t i = 0; i < burstCount; i++) {
            const auto& b = bursts[order[i]];
            totalFrames += b.frameCount;
            if (b.pps > peakPps) peakPps = b.pps;
        }
        pushLine(p, LC::DIM, " total frames: %d  peak: %dfps",
            (int)totalFrames, peakPps);
        pushDeauthBurstRows(p, 4, true);
    }
}

// ==[ REFRESH DISPATCHER ]==

static void refreshPane(int p) {
    switch ((Pane)p) {
        case Pane::SITREP: refreshSitrep(); break;
        case Pane::RF: refreshSigint(); break;
        case Pane::BLE:    refreshBle(); break;
        case Pane::FUSION: refreshFusion(); break;
        case Pane::LOG:    refreshLog(); break;
        default: break;
    }
}

// ============================================================
// ==[ DRAWING ]===============================================
// ============================================================

static int paneTabWidth(int paneIndex) {
    if (paneIndex < 0 || paneIndex >= PANE_COUNT) return 0;
    return (int)strlen(paneTabLabels[paneIndex]) * CHAR_W + 8;
}

static void drawOperatorPill(M5Canvas& canvas, int x, int y, int w,
                             const char* label, bool active, bool attention,
                             int height = TAB_H) {
    const uint16_t fg = termFG();
    const uint16_t bg = termBG();
    const uint16_t dim = termDIM();
    const uint16_t edge = (active || attention) ? fg : dim;

    if (active) {
        canvas.fillRect(x, y, w, height, fg);
        canvas.setTextColor(bg);
    } else {
        canvas.drawRect(x, y, w, height, edge);
        canvas.setTextColor(fg);
        if (attention) canvas.fillRect(x + 2, y + height - 3, w - 4, 2, fg);
    }
    canvas.setTextSize(1);
    canvas.setTextDatum(MC_DATUM);
    canvas.drawString(label, x + w / 2, y + height / 2);
    canvas.setTextDatum(TL_DATUM);
}

static int paneTabHit(int16_t x, int16_t y) {
    if (y < TAB_Y || y >= TAB_Y + TAB_H) return -1;

    int left = 4;
    for (int i = 0; i < PANE_COUNT; ++i) {
        const int w = paneTabWidth(i);
        if (x >= left && x < left + w) return i;
        left += w + 3;
    }
    return -1;
}

static void drawHeader(M5Canvas& canvas) {
    const uint16_t fg = termFG();
    const uint16_t bg = termBG();
    const uint16_t dim = termDIM();

    // The system bar is the same shared chrome used by R1B R4CK. Everything
    // below it is a rendered control or a current-status readout.
    Display::drawStatusBarTo(&canvas, "D4 DEFENSE");
    canvas.fillRect(0, STATUS_H, SCREEN_W, HEADER_H - STATUS_H, bg);

    int x = 4;
    for (int i = 0; i < PANE_COUNT; ++i) {
        const int w = paneTabWidth(i);
        drawOperatorPill(canvas, x, TAB_Y, w, paneTabLabels[i],
                         i == (int)currentPane,
                         i != (int)currentPane && paneHasUnseen[i]);
        x += w + 3;
    }

    canvas.drawFastHLine(0, SUMMARY_Y - 1, SCREEN_W, dim);
    canvas.setTextSize(1);
    canvas.setTextDatum(TL_DATUM);
    canvas.setTextColor(dim);
    canvas.drawString("POSTURE", 4, SUMMARY_Y + 2);

    const uint8_t threat = DefhogTerminal::computeThreatLevel();
    Display::drawStatusPillTo(&canvas, 112, SUMMARY_Y + 2,
                              threatLabels[threat], "CLEAR", threat == 0,
                              false);

    if (phase == Phase::REVIEW) {
        const int pane = (int)currentPane;
        char review[20];
        snprintf(review, sizeof(review), "REVIEW %d/%d",
                 paneLineCount[pane] + paneScrollOffset[pane],
                 paneLineCount[pane]);
        canvas.setTextColor(dim);
        canvas.drawString(review, 122, SUMMARY_Y + 2);
    } else {
        // Every detailed pane can be old while its source is waiting or
        // retained.  Keep the radio state and completed-scan age in the
        // common header so the operator never has to infer freshness from a
        // count alone.  BLE is a current tracker-table count, not a claim
        // about device identity or distance.
        const auto pipe = DefensePipeline::snapshot().getWifiPipelineStatus();
        char age[8];
        char source[28];
        formatAge(pipe.lastCompleteMs, age, sizeof(age));
        // Keep the RF provenance and catalog count clear of the loud
        // SERIAL BLE pill at the right edge.  `age` is capped at seven chars
        // and pipeline labels at four, so this compact form stays inside the
        // 126 px header lane even with a two-digit tracker count.
        snprintf(source, sizeof(source), "RF:%s/%s B:%d",
                 Recon::wifiPipelineStateLabel(pipe.state), age,
                 DefensePipeline::snapshot().getTrackerCount());
        canvas.setTextColor(dim);
        canvas.drawString(source, 122, SUMMARY_Y + 2);
    }

    // This pill is a real BLE-pane entry point. A generic serial-BLE profile
    // is source evidence, not a positive skimmer identification; the detail
    // pane makes the inspection boundary and TRACK action explicit.
    const int serialBleSuspects = getRecentSerialBleSuspectCount();
    if (serialBleSuspects > 0) {
        Display::drawStatusPillTo(&canvas, SCREEN_W - 4, SUMMARY_Y + 2,
                                  "SERIAL BLE", "", false, false);
    }
    canvas.drawFastHLine(0, HEADER_H - 1, SCREEN_W, dim);
    canvas.setTextColor(fg);
}

static const char* actionLabelForLine(const PaneLine& line) {
    switch (currentPane) {
        case Pane::SITREP:
            switch (line.actionId) {
                case SITREP_ACTION_SIGINT: return "SIGINT";
                case SITREP_ACTION_BLE:    return "BLE";
                case SITREP_ACTION_FUSION: return "FUSION";
                case SITREP_ACTION_LOG:    return "LOG";
                case SITREP_ACTION_HUNT:
                default:                   return "HUNT";
            }
        case Pane::BLE:    return (line.actionId & 0x80u) ? "DROP" : "TRACK";
        case Pane::FUSION: return "VIEW";
        case Pane::LOG:    return "VIEW";
        default:           return "OPEN";
    }
}

static void drawContent(M5Canvas& canvas) {
    int p = (int)currentPane;
    if (!paneBuffers[p]) return;

    uint16_t fg = termFG();
    uint16_t dim = termDIM();
    uint16_t pri = termPri();
    uint16_t acc = termAcc();
    bool alertBlink = ((millis() / 400) & 1) == 0;

    int lineCount = paneLineCount[p];
    int scrollOff = paneScrollOffset[p];

    // calculate visible window
    int totalLines = lineCount;
    int startLine = totalLines - VISIBLE_LINES + scrollOff;
    if (startLine < 0) startLine = 0;
    int endLine = startLine + VISIBLE_LINES;
    if (endLine > totalLines) endLine = totalLines;

    // ring buffer start index — when the buffer wraps (lineCount == RING_LINES
    // and writes have advanced past the first slot), oldest line is at writeHead,
    // not at 0. `<=` made this branch unreachable since lineCount is capped at RING_LINES.
    int ringStart = (lineCount < RING_LINES)
        ? 0
        : paneWriteHead[p];

    int ty = CONTENT_Y;
    for (int i = startLine; i < endLine; i++) {
        int bufIdx = (ringStart + i) % RING_LINES;
        const PaneLine& line = paneBuffers[p][bufIdx];

        switch (line.color) {
            case LC::ALERT: {
                // A loud left rail retains alert scanning at a glance; words
                // and the action pill carry the exact operation.
                canvas.fillRect(TEXT_X, ty + 1, 2, CHAR_H, acc);
                canvas.setTextColor(alertBlink ? acc : dim);
                break;
            }
            case LC::BAR: {
                // A named section bar is faster to parse than the old unnamed
                // line. It is presentation only, never a hidden tap target.
                canvas.fillRect(TEXT_X, ty + 1, SCREEN_W - TEXT_X * 2, CHAR_H, termFG());
                canvas.setTextColor(termBG());
                canvas.setTextDatum(TL_DATUM);
                canvas.setTextSize(1);
                canvas.drawString(line.text, TEXT_X + 4, ty + 1);
                ty += LINE_H;
                continue;
            }
            case LC::DIM:
                canvas.setTextColor(dim);
                break;
            case LC::FG:
            default:
                canvas.setTextColor(fg);
                break;
        }

        int maxChars = MAX_CHARS;
        if (line.tappable) {
            const char* action = actionLabelForLine(line);
            const int actionW = (int)strlen(action) * CHAR_W + 8;
            const int actionX = SCREEN_W - actionW - 4;
            maxChars = (actionX - TEXT_X - 4) / CHAR_W;
            if (maxChars < 1) maxChars = 1;

        }

        char clipped[LINE_LEN];
        snprintf(clipped, sizeof(clipped), "%.*s", maxChars, line.text);
        canvas.setTextDatum(TL_DATUM);
        canvas.setTextSize(1);
        canvas.drawString(clipped, TEXT_X, ty + 1);
        if (line.tappable) {
            const char* action = actionLabelForLine(line);
            const int actionW = (int)strlen(action) * CHAR_W + 8;
            const int actionX = SCREEN_W - actionW - 4;
            // The action exists in the hit map and is visibly a button—there
            // are no implicit text-row actions in this screen any more.
            drawOperatorPill(canvas, actionX, ty + 1, actionW, action,
                             true, false, CHAR_H);
        }
        ty += LINE_H;
    }

    // live cursor (only in LIVE phase) — primary blue
    if (phase == Phase::LIVE && ty < SCREEN_H - BOTTOM_H - CHAR_H) {
        if (cursorVisible) {
            canvas.fillRect(TEXT_X, ty + 1, CHAR_W, CHAR_H, pri);
        }
    }
}

static void drawBottomBar(M5Canvas& canvas) {
    if (Display::drawHintBottomBar(&canvas)) return;

    int y = SCREEN_H - BOTTOM_H;

    canvas.fillRect(0, y, SCREEN_W, BOTTOM_H, termBG());
    canvas.drawFastHLine(0, y, SCREEN_W, termDIM());

    const char* action = "REFRESH";
    switch (currentPane) {
        case Pane::SITREP: action = "HUNT"; break;
        case Pane::RF:     action = "SPECTRUM"; break;
        case Pane::BLE:    action = "PIG EARS"; break;
        default: break;
    }

    drawOperatorPill(canvas, 4, y + 1, 64, "< PANE", false, false);
    const int actionW = (int)strlen(action) * CHAR_W + 12;
    drawOperatorPill(canvas, (SCREEN_W - actionW) / 2, y + 1,
                     actionW, action, true, false);
    drawOperatorPill(canvas, SCREEN_W - 68, y + 1, 64, "PANE >", false, false);
}

static void drawBoot(M5Canvas& canvas) {
    uint16_t fg = termFG();
    uint16_t dim = termDIM();
    uint16_t pri = termPri();
    uint32_t elapsed = millis() - enterTime;

    // Keep boot in the same operator chassis as the live field view.
    drawHeader(canvas);
    canvas.setTextSize(1);
    canvas.setTextDatum(TL_DATUM);

    int ty = HEADER_H + 8;

    // opencode-style two-tone logo: "DEF" muted, "HOG4" bright
    if (elapsed > 0) {
        canvas.setTextSize(2);
        canvas.setTextColor(dim);
        canvas.drawString("DEF", TEXT_X, ty);
        canvas.setTextColor(fg);
        canvas.drawString("HOG4", TEXT_X + 3 * CHAR_W * 2, ty);  // size 2 = 12px/char
        int afterLogo = TEXT_X + 7 * CHAR_W * 2 + 4;
        canvas.setTextSize(1);
        canvas.setTextColor(dim);
        canvas.drawString("defense terminal", afterLogo, ty + 5);
        ty += 16 + 4;
    }

    // whitespace gap (opencode style — no drawn separator)
    if (elapsed > BOOT_LINE_MS) {
        ty += LINE_H;
    }

    // subsystem lines — opencode-style brackets with color coding
    struct BootLine { const char* label; bool ok; };
    static const BootLine bootLines[] = {
        { "recon subsystem",  true },
        { "ble scanner",      true },
        { "xband fusion",     true },
        { "forensic log",     true },
        { "threat engine",    true },
    };

    for (int i = 0; i < 5; i++) {
        if (elapsed > BOOT_LINE_MS * (i + 2)) {
            canvas.setTextColor(fg);
            canvas.drawString("[", TEXT_X, ty);
            canvas.setTextColor(bootLines[i].ok ? fg : pri);
            canvas.drawString(bootLines[i].ok ? "OK" : "--", TEXT_X + CHAR_W, ty);
            canvas.setTextColor(fg);
            canvas.drawString("]", TEXT_X + 3 * CHAR_W, ty);
            canvas.setTextColor(dim);
            canvas.drawString(bootLines[i].label, TEXT_X + 4 * CHAR_W, ty);
            ty += LINE_H;
        }
    }

    // whitespace gap before summary (opencode style)
    if (elapsed > BOOT_LINE_MS * 7) {
        ty += LINE_H;
    }
    if (elapsed > BOOT_LINE_MS * 8) {
        int wifiAPs = DefensePipeline::snapshot().getLastWifiAPCount();
        int bleCount = DefensePipeline::snapshot().getTrackerCount();
        uint8_t threat = DefhogTerminal::computeThreatLevel();
        const auto pipe = DefensePipeline::snapshot().getWifiPipelineStatus();
        canvas.setTextColor(fg);
        char buf[54];
        snprintf(buf, sizeof(buf), "APs:%d BLE:%d WIFI:%s GPS:%s",
            wifiAPs, bleCount, Recon::wifiPipelineStateLabel(pipe.state),
            GPS::hasFix() ? "LOCK" : (GPS::hasNMEA() ? "NMEA" : "--"));
        canvas.drawString(buf, TEXT_X, ty);
        ty += LINE_H;
        // The status word is explicit; the shared theme carries the visual role.
        canvas.drawString("THREAT:", TEXT_X, ty);
        canvas.setTextColor(fg);
        canvas.drawString(threatLabels[threat], TEXT_X + 42, ty);
        canvas.setTextColor(dim);
        canvas.drawString(" // LOCK", TEXT_X + 42 + strlen(threatLabels[threat]) * CHAR_W, ty);
    }

    // bottom bar always visible during boot
    drawBottomBar(canvas);
}

static void drawInterrupt(M5Canvas& canvas) {
    if (interruptDuration == 0) return;
    uint32_t elapsed = millis() - interruptStart;
    if (elapsed > interruptDuration) {
        interruptDuration = 0;
        return;
    }

    uint16_t acc = termAcc();
    bool blink = ((elapsed / 300) % 2) == 0;

    int y = CONTENT_Y + 2;
    int h = LINE_H + 6;
    if (blink) {
        canvas.fillRect(0, y, SCREEN_W, h, acc);
        canvas.setTextColor(termBG());
    } else {
        canvas.fillRect(0, y, SCREEN_W, h, termBG());
        canvas.drawRect(0, y, SCREEN_W, h, acc);
        canvas.setTextColor(acc);
    }
    canvas.setTextSize(1);
    canvas.setTextDatum(TC_DATUM);
    canvas.drawString(interruptMsg, SCREEN_W / 2, y + 4);
    canvas.setTextColor(acc);  // restore
}

static void selectPane(Pane pane) {
    const int next = (int)pane;
    if (next < 0 || next >= PANE_COUNT) return;

    currentPane = pane;
    paneHasUnseen[next] = false;
    paneScrollOffset[next] = 0;
    phase = Phase::LIVE;
    expandedAttacker = -1;
    clearLogExpansion();
    refreshPane(next);
    lastRefresh[next] = millis();
    SFX::click();
}

static void trackBleTarget(const uint8_t* payloadHash, bool serialBleSuspect) {
    if (!payloadHash) return;
    uint8_t targetPayloadHash[4] = {};
    memcpy(targetPayloadHash, payloadHash, sizeof(targetPayloadHash));

    // PIG EARS owns the durable payload pin, Geiger cadence, and bearing UI.
    // Do not silently persist a watchlist entry from a single ambiguous BLE
    // pattern; active tracking is the one-tap response, then the operator can
    // promote it to a watchlist item after inspection.
    Hamlet::enterMode(HamletMode::BLE_SCANNER);
    if (BleScanner::trackDeviceByPayloadHash(targetPayloadHash)) {
        Display::showToast(serialBleSuspect ? "SERIAL BLE TRACKING"
                                            : "BLE TARGET TRACKING", 1800);
    } else {
        Display::showToast("TARGET EXPIRED - RESCAN", 2200);
    }
}

// ============================================================
// ==[ PUBLIC API ]============================================
// ============================================================

void triggerInterrupt(const char* msg, uint32_t durationMs) {
    strncpy(interruptMsg, msg, LINE_LEN - 1);
    interruptMsg[LINE_LEN - 1] = '\0';
    interruptStart = millis();
    interruptDuration = durationMs;
    SFX::play(SFX::Event::RECON_ALERT);
}

void enter() {
    enterTime = millis();
    phase = Phase::BOOT;
    flashStart = 0;

    allocBuffers();

    for (int i = 0; i < PANE_COUNT; i++) {
        clearPane(i);
        lastRefresh[i] = 0;
        paneHasUnseen[i] = false;
    }

    interruptDuration = 0;
    currentPane = selectEntryPane();
    cursorVisible = true;

    // reset interrupt edge detection
    lastDeauthActive = DefensePipeline::snapshot().isDeauthActive();
    lastEvilTwin = DefensePipeline::snapshot().isEvilTwinActive();
    lastKarma = DefensePipeline::snapshot().isKarmaActive();
    lastFollowing = DefensePipeline::snapshot().getFollowingCount();
    lastSerialBleSuspectCount = getRecentSerialBleSuspectCount();

    // reset expand state
    expandedAttacker = -1;
    clearLogExpansion();
    lastCursorToggle = millis();

    // war room = aggressive recon cadence
    DefensePipeline::setForcedCadence(Recon::CadenceTier::AGGRESSIVE);
    DefensePipeline::requestWifiScan();

    SFX::play(SFX::Event::SIGNAL_LOCK);
}

void exit() {
    DefensePipeline::clearForcedCadence();
    freeBuffers();
    phase = Phase::BOOT;
}

void update() {
    uint32_t now = millis();

    // boot → live transition
    if (phase == Phase::BOOT) {
        if (now - enterTime >= BOOT_TOTAL_MS) {
            phase = Phase::LIVE;
            flashStart = now;  // trigger flash effect
            // initial refresh all panes
            for (int i = 0; i < PANE_COUNT; i++) {
                refreshPane(i);
                lastRefresh[i] = now;
            }
        }
        return;
    }

    // cursor blink
    if (now - lastCursorToggle > 500) {
        cursorVisible = !cursorVisible;
        lastCursorToggle = now;
    }

    // stagger refreshes — only refresh current pane + check alerts on others
    int cp = (int)currentPane;
    if (phase == Phase::LIVE && now - lastRefresh[cp] >= refreshIntervals[cp]) {
        refreshPane(cp);
        lastRefresh[cp] = now;
    }

    // ==[ CROSS-PANE ALERT DETECTION ]== mark unseen when critical events fire
    if (DefensePipeline::snapshot().isDeauthActive() && cp != (int)Pane::RF) {
        paneHasUnseen[(int)Pane::RF] = true;
    }
    if (DefensePipeline::snapshot().getFollowingCount() > 0 && cp != (int)Pane::BLE) {
        paneHasUnseen[(int)Pane::BLE] = true;
    }
    const int serialBleSuspects = getRecentSerialBleSuspectCount();
    if (serialBleSuspects > 0 && cp != (int)Pane::BLE) {
        paneHasUnseen[(int)Pane::BLE] = true;
    }
    if (DefensePipeline::snapshot().hasActiveAttacker() && cp != (int)Pane::FUSION) {
        paneHasUnseen[(int)Pane::FUSION] = true;
    }
    if (DefensePipeline::snapshot().isEvilTwinActive() && cp != (int)Pane::LOG) {
        paneHasUnseen[(int)Pane::LOG] = true;
    }

    // ==[ INTERRUPT TRIGGERS ]== edge-detect critical Recon events → flash banner
    bool deauthNow  = DefensePipeline::snapshot().isDeauthActive();
    bool twinNow    = DefensePipeline::snapshot().isEvilTwinActive();
    bool karmaNow   = DefensePipeline::snapshot().isKarmaActive();
    int  followNow  = DefensePipeline::snapshot().getFollowingCount();

    if (deauthNow && !lastDeauthActive) triggerInterrupt("!! DEAUTH STORM DETECTED !!");
    if (twinNow && !lastEvilTwin)       triggerInterrupt("!! EVIL TWIN ACTIVE !!");
    if (karmaNow && !lastKarma)         triggerInterrupt("!! KARMA HONEYPOT !!");
    if (followNow > lastFollowing)      triggerInterrupt("!! NEW FOLLOWING TRACKER !!");
    if (serialBleSuspects > lastSerialBleSuspectCount) {
        triggerInterrupt("SERIAL BLE NEARBY - TAP BLE");
    }

    lastDeauthActive = deauthNow;
    lastEvilTwin     = twinNow;
    lastKarma        = karmaNow;
    lastFollowing    = followNow;
    lastSerialBleSuspectCount = serialBleSuspects;
}

void draw(M5Canvas& canvas) {
    if (phase == Phase::BOOT) {
        drawBoot(canvas);
        return;
    }

    drawHeader(canvas);
    drawContent(canvas);
    drawBottomBar(canvas);
    drawInterrupt(canvas);

    // flash effect — brief primary blue wash after boot
    if (flashStart > 0) {
        uint32_t flashElapsed = millis() - flashStart;
        if (flashElapsed < FLASH_DURATION_MS) {
            canvas.fillRect(0, HEADER_H, SCREEN_W, SCREEN_H - HEADER_H - BOTTOM_H, termPri());
        } else {
            flashStart = 0;
        }
    }
}

void nextPane() {
    selectPane((Pane)(((int)currentPane + 1) % PANE_COUNT));
}

void prevPane() {
    selectPane((Pane)(((int)currentPane + PANE_COUNT - 1) % PANE_COUNT));
}

void action() {
    // B short: context-dependent — launch related mode or force refresh
    switch (currentPane) {
        case Pane::SITREP:
            Hamlet::enterMode(HamletMode::HUNT);
            return;
        case Pane::RF:
            Hamlet::enterMode(HamletMode::SPECTRUM);
            return;
        case Pane::BLE:
            Hamlet::enterMode(HamletMode::BLE_SCANNER);
            return;
        default:
            break;
    }
    // FUSION / LOG: force refresh
    int cp = (int)currentPane;
    refreshPane(cp);
    lastRefresh[cp] = millis();
}

void scrollUp() {
    int p = (int)currentPane;
    int maxScroll = -(paneLineCount[p] - VISIBLE_LINES);
    if (maxScroll >= 0) return;  // not enough lines to scroll

    paneScrollOffset[p]--;
    if (paneScrollOffset[p] < maxScroll) paneScrollOffset[p] = maxScroll;
    phase = Phase::REVIEW;
}

void scrollDown() {
    int p = (int)currentPane;
    if (paneScrollOffset[p] >= 0) return;

    paneScrollOffset[p]++;
    if (paneScrollOffset[p] >= 0) {
        paneScrollOffset[p] = 0;
        phase = Phase::LIVE;
    }
}

void onTap(int16_t x, int16_t y) {
    const int tappedPane = paneTabHit(x, y);
    if (tappedPane >= 0) {
        selectPane((Pane)tappedPane);
        return;
    }

    // The visible serial-BLE pill is an explicit BLE-pane shortcut.
    if (y >= SUMMARY_Y && y < SUMMARY_Y + SUMMARY_H &&
        x >= SCREEN_W - 72 && getRecentSerialBleSuspectCount() > 0) {
        selectPane(Pane::BLE);
        return;
    }

    // Bottom controls mirror their drawn pill geometry: left/right switch the
    // field pane; the centre opens its labelled context action.
    if (y >= SCREEN_H - BOTTOM_H) {
        if (x < 80) prevPane();
        else if (x > SCREEN_W - 80) nextPane();
        else action();
        return;
    }

    // Only data rows inside the content field can be pressed from here.
    if (y < CONTENT_Y) return;

    int p = (int)currentPane;
    if (!paneBuffers[p]) return;

    int lineIdx = (y - CONTENT_Y) / LINE_H;
    int lineCount = paneLineCount[p];
    int scrollOff = paneScrollOffset[p];

    int startLine = lineCount - VISIBLE_LINES + scrollOff;
    if (startLine < 0) startLine = 0;
    int targetLine = startLine + lineIdx;
    if (targetLine >= lineCount) return;

    // same wraparound math as drawContent — `<` not `<=`
    int ringStart = (lineCount < RING_LINES) ? 0 : paneWriteHead[p];
    int bufIdx = (ringStart + targetLine) % RING_LINES;
    const PaneLine& line = paneBuffers[p][bufIdx];

    if (!line.tappable) return;

    // ==[ SITREP ]== the explicit NEXT pill opens the evidence pane that
    // explains the current posture.  It never invents an action from row text.
    if (currentPane == Pane::SITREP) {
        switch (line.actionId) {
            case SITREP_ACTION_SIGINT: selectPane(Pane::RF); return;
            case SITREP_ACTION_BLE:    selectPane(Pane::BLE); return;
            case SITREP_ACTION_FUSION: selectPane(Pane::FUSION); return;
            case SITREP_ACTION_LOG:    selectPane(Pane::LOG); return;
            case SITREP_ACTION_HUNT:
                Hamlet::enterMode(HamletMode::HUNT);
                return;
            default:
                return;
        }
    }

    // ==[ BLE ]== one tap opens durable payload-identity tracking. The old
    // watchlist-only action made a row look actionable without actually
    // launching the operator's tracking tool.
    if (currentPane == Pane::BLE) {
        if (line.actionId & 0x80) {
            // watchlist entry tap → remove
            uint8_t slot = line.actionId & 0x7F;
            DefensePipeline::removeFromWatchlist(slot);
            SFX::click();
        } else {
            // Track the payload identity rendered in this row, not its old
            // tracker-table index. The table can change while DEFHOG4 is live.
            if (line.hasTrackerIdentity) {
                uint8_t payloadHash[sizeof(line.trackerPayloadHash)] = {};
                memcpy(payloadHash, line.trackerPayloadHash, sizeof(payloadHash));
                trackBleTarget(payloadHash, line.serialBleSuspect);
                return;
            }
        }
        refreshBle();
        lastRefresh[(int)Pane::BLE] = millis();
        return;
    }

    // ==[ FUSION ]== toggle attacker detail expand
    if (currentPane == Pane::FUSION) {
        expandedAttacker = (expandedAttacker == line.actionId) ? -1 : line.actionId;
        refreshFusion();
        lastRefresh[(int)Pane::FUSION] = millis();
        SFX::click();
        return;
    }

    // ==[ LOG ]== toggle forensic event detail expand
    if (currentPane == Pane::LOG) {
        const auto* log = DefensePipeline::snapshot().getForensicLog();
        uint8_t logSlot = line.actionId;
        if (!log || logSlot >= Recon::MAX_FORENSIC_LOG || log[logSlot].timestamp == 0) {
            return;
        }
        if (expandedLogEntry == logSlot && expandedLogTimestamp == log[logSlot].timestamp) {
            clearLogExpansion();
        } else {
            expandedLogEntry = logSlot;
            expandedLogTimestamp = log[logSlot].timestamp;
        }
        refreshLog();
        lastRefresh[(int)Pane::LOG] = millis();
        SFX::click();
        return;
    }
}

}  // namespace Defhog4
