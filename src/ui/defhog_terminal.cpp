/**
 * DefhogTerminal — hacker defense terminal
 *
 * ==[ DEFHOG4 ]== neon accent fg on black, theme fg as secondary.
 * procedural particle materialize: scatter → converge → logo → systemd boot.
 * inverted blinking alerts for critical threats. zero-storage particles.
 * 12 cross-namespace correlated dump types. forensic threat scoring.
 * ring buffer of 16 lines × 34 chars. full-width. auto-scroll.
 */

#include "defhog_terminal.h"
#include "ui_measurements.h"
#include "display.h"
#include "menu_pig_render.h"
#include "../defense/recon.h"
#include "../defense/xband.h"
#include "../defense/defense_pipeline.h"
#include "../defense/potfile.h"
#include "../defense/wifi_chaff.h"
// narrator belongs to pig bubbles, not terminal
#include "../modes/hunt.h"
#include "../modes/spectrum.h"
#include "../core/capture.h"
#include "../core/config.h"
#include "../core/achievements.h"
#include "../core/challenges.h"
#include "../piglet/mood.h"
#include "../audio/sfx.h"
#include "../build_info.h"
#include <cstdarg>

namespace DefhogTerminal {

using namespace UIMeasurements;
using namespace UIMeasurements::DefhogLayout;

// ==[ TERMINAL PALETTE — TOKYO NIGHT ]==
// Hardcoded opencode tokyonight dark. Decoupled from hamlet accent system.
using namespace UIMeasurements::TokyoNight;
static constexpr uint16_t TERM_BG      = BG;        // #1A1B26 dark navy
static constexpr uint16_t TERM_BG_P    = BG_PANEL;  // #1E2030 header/footer bg
static constexpr uint16_t TERM_FG_C    = FG;         // #C8D3F5 bright text
static constexpr uint16_t TERM_DIM_C   = DIM;        // #828BB8 muted text
static constexpr uint16_t TERM_PRI     = PRIMARY;    // #82AAFF blue accent
static constexpr uint16_t TERM_ACC     = ACCENT;     // #FF966C orange warning
static constexpr uint16_t TERM_ERR     = ERROR;      // #FF757F red error
static constexpr uint16_t TERM_OK      = SUCCESS;    // #C3E88D green success
static constexpr uint16_t TERM_BD      = BORDER;     // #545C7E border subtle

static inline uint16_t TERM_FG()  { return TERM_FG_C; }
static inline uint16_t TERM_DIM() { return TERM_DIM_C; }

// ==[ RING BUFFER ]==
static constexpr int MAX_LINES = 16;
static constexpr int LINE_LEN = 34;   // 33 visible + null

struct TermLine {
    char text[LINE_LEN];
    LineColor color;
};

static TermLine lines[MAX_LINES];
static uint8_t writeHead = 0;
static uint8_t lineCount = 0;

// ==[ STATE ]==
static State state = State::HIDDEN;
static uint32_t animStart = 0;
static uint8_t showStation = 0;
static uint8_t showRoom = 0;

// ==[ LINE REVEAL ]==
static uint32_t lastLineRevealTime = 0;
static bool lineRevealing = false;

// ==[ BOOT SEQUENCE ]==
static constexpr int BOOT_LINES_MAX = 6;
static uint8_t bootLineIdx = 0;
static uint32_t bootLineStartTime = 0;
static bool bootSequenceActive = false;

// ==[ DATA DUMP ]==
enum class DumpPhase : uint8_t {
    NONE,
    PAUSE,
    ACTIVE,
    COOLDOWN,  // random 1-3s pause between dumps
    DONE
};

static DumpPhase dumpPhase = DumpPhase::NONE;
static uint32_t dumpPauseStart = 0;
static DumpType currentDump = DumpType::COUNT;
static DumpType lastDumpType = DumpType::COUNT;

static constexpr int DUMP_MAX_LINES = 12;
static char dumpLines[12][LINE_LEN];
static LineColor dumpColors[12];
static uint8_t dumpLineCount = 0;
static uint8_t dumpLinePushed = 0;
static uint32_t dumpLastLinePush = 0;
static constexpr uint32_t DUMP_LINE_DELAY_MS = 120;
static bool dumpCommandPushed = false;
static uint8_t dumpCount = 0;  // tracks materializations for stats-last weighting
static uint32_t dumpCooldownStart = 0;
static uint16_t dumpCooldownMs = 0;
static uint8_t dumpsCompleted = 0;  // dumps fully witnessed (lines all pushed)
static uint8_t dumpXPAwards = 0;   // anti-farm: cap XP awards from dumps
static constexpr uint8_t MAX_DUMP_XP_AWARDS = 10;

// ==[ TIMERS ]==
static uint32_t lastCommentaryTime = 0;
static uint32_t lastQuietTime = 0;
static uint32_t terminalStartTime = 0;
static uint32_t lastEventTime = 0;
static uint32_t lastElapsedTime = 0;
static constexpr uint32_t COMMENTARY_INTERVAL = 25000;
static constexpr uint32_t QUIET_INTERVAL = 30000;
static constexpr uint32_t ELAPSED_INTERVAL = 60000;

// ==[ CURSOR ]==
static bool cursorVisible = true;
static uint32_t lastCursorToggle = 0;

// ==[ FLASH EFFECT ]== brief neon flash after materialize
static uint32_t flashStart = 0;
static constexpr uint32_t FLASH_DURATION_MS = 150;

// ==[ PROCEDURAL PARTICLES ]== portal-style hash noise, zero storage
static constexpr int MAT_PARTICLES = 40;
static uint32_t matSeed = 0;  // set on show(), used as deterministic seed

static inline uint32_t termHash(uint32_t a, uint32_t b, uint32_t salt) {
    uint32_t h = a * 73856093u ^ b * 19349663u ^ salt;
    h ^= (h >> 13);
    h *= 1274126177u;
    h ^= (h >> 16);
    return h;
}

// ==[ SCATTER PARTICLES (DISSOLVE) ]==
static constexpr int MAX_PARTICLES = 8;
struct ScatterParticle {
    int16_t x, y;
    int8_t vx, vy;
    uint8_t life;
};
static ScatterParticle particles[MAX_PARTICLES];
static uint8_t particleCount = 0;

// ==[ THREAT LEVEL ]==
static uint8_t threatLevel = 0;
static uint8_t lastThreatLevel = 0;
static uint32_t threatDowngradeSince = 0;
static constexpr uint32_t THREAT_DOWNGRADE_HOLDOFF_MS = 15000;

// ==[ UPTIME FORMATTER ]== T+MMmSSs session-relative timestamp

static void fmtUptime(char* buf, size_t len, uint32_t ms) {
    uint32_t s = ms / 1000;
    uint32_t m = s / 60;
    s %= 60;
    snprintf(buf, len, "T+%02lu:%02lu", (unsigned long)m, (unsigned long)s);
}

// ==[ DEAUTH TOOL SIGNATURE ]== reason code + PPS + source count → probable tool
// Source: Lab401 deauth analysis, aircrack-ng docs, Flipper Zero source

static const char* toolSignature(uint16_t reason, uint16_t pps, uint8_t sources,
                                  uint16_t deauthN, uint16_t disassocN) {
    if (reason == 7 && sources == 1 && pps > 50) return "aircrk";
    if (reason == 2 && sources == 1) return "flippr";
    if (reason == 1 && (sources > 1 || (disassocN > 0 && deauthN > 0))) return "mdk4";
    if (reason == 1 && sources == 1 && pps < 100) return "deauth8266";
    if (reason == 1 && pps > 100) return "mdk";
    return "?";
}

// ==[ FORENSIC LOG EVENT TAGS ]== 3-char abbreviation per ReconEvent

static const char* eventTag(Recon::ReconEvent ev) {
    switch (ev) {
        case Recon::ReconEvent::EVIL_TWIN: return "twn";
        case Recon::ReconEvent::KARMA_HONEYPOT: return "krm";
        case Recon::ReconEvent::FINGERPRINT_MISMATCH: return "fp";
        case Recon::ReconEvent::SEQ_ANOMALY: return "seq";
        case Recon::ReconEvent::RSSI_ANOMALY: return "rsi";
        case Recon::ReconEvent::DEAUTH_DETECTED: return "dth";
        case Recon::ReconEvent::COORDINATED_ATTACK: return "crd";
        case Recon::ReconEvent::ATTACKER_IDENTIFIED: return "atk";
        case Recon::ReconEvent::DUAL_BAND_STALK: return "stk";
        case Recon::ReconEvent::BLE_SPAM: return "spm";
        case Recon::ReconEvent::TRACKER_FOLLOWING: return "fol";
        case Recon::ReconEvent::PROBE_VULN_CLIENT: return "prb";
        case Recon::ReconEvent::KARMA_CONFIRMED: return "KRM";
        case Recon::ReconEvent::CANARY_TRIPPED: return "CAN";
        case Recon::ReconEvent::RELAY_SUSPECT: return "RLY";
        case Recon::ReconEvent::HOSTILE_CLIENT: return "HCL";
        case Recon::ReconEvent::TOOL_IDENTIFIED: return "TID";
        case Recon::ReconEvent::LOW_ENTROPY_BEACON: return "LEB";
        case Recon::ReconEvent::WATCHLIST_ENTER: return "W+";
        case Recon::ReconEvent::WATCHLIST_EXIT: return "W-";
        default: return "???";
    }
}

// ==[ INTERNAL HELPERS ]==

static void pushLineRaw(LineColor color, const char* text) {
    TermLine& line = lines[writeHead];
    strncpy(line.text, text, LINE_LEN - 1);
    line.text[LINE_LEN - 1] = '\0';
    line.color = color;
    writeHead = (writeHead + 1) % MAX_LINES;
    if (lineCount < MAX_LINES) lineCount++;
    lastEventTime = millis();
    lastLineRevealTime = millis();
    lineRevealing = true;
}

static void pushLineInternal(LineColor color, const char* fmt, va_list args) {
    char buf[LINE_LEN];
    vsnprintf(buf, LINE_LEN, fmt, args);
    pushLineRaw(color, buf);
}

static const char* devicePrompt() {
    static char buf[16];
    snprintf(buf, sizeof(buf), "%s#", Config::getHamletName());
    return buf;
}

static const char* behaviorTag(HuntBehavior b) {
    switch (b) {
        case HuntBehavior::CAMP:   return "CAMP";
        case HuntBehavior::PATROL: return "PATROL";
        case HuntBehavior::SPRINT: return "SPRINT";
        case HuntBehavior::LURK:   return "LURK";
    }
    return "???";
}

static void stageLine(LineColor color, const char* fmt, ...) {
    if (dumpLineCount >= DUMP_MAX_LINES) return;
    va_list args;
    va_start(args, fmt);
    vsnprintf(dumpLines[dumpLineCount], LINE_LEN, fmt, args);
    va_end(args);
    dumpColors[dumpLineCount] = color;
    dumpLineCount++;
}

// ==[ GEOMETRY HELPERS — forward decl for particle draw ]==
static int termX();
static int termY();
static int termW();
static int termH();
static int textX();
static int textW();
static int textAreaY();
static int visLines();

// ==[ PROCEDURAL MATERIALIZE — zero-storage particle draw ]==
// portal-style: compute each particle position from hash + elapsed time

static void drawProceduralParticles(M5Canvas& canvas, uint32_t elapsed) {
    int tX = termX(), tY = termY(), tW = termW(), tH = termH();
    int perim = 2 * (tW + tH);
    uint32_t scatterEnd = kMatScatterEnd;
    uint32_t convergeEnd = kMatConvergeEnd;

    for (int i = 0; i < MAT_PARTICLES; i++) {
        uint32_t h = termHash(i, matSeed, 0xCAFE);

        int delayMs = (int)(h & 0x0F) * 15;
        if ((int)elapsed < delayMs) continue;

        int sx = tX + (int)((h >> 4) % tW);
        int sy = tY + (int)((h >> 12) % tH);

        int tx, ty;
        if (i < 24) {
            int pos = (i * perim) / 24;
            if (pos < tW) { tx = tX + pos; ty = tY; }
            else if (pos < tW + tH) { tx = tX + tW - 1; ty = tY + (pos - tW); }
            else if (pos < 2 * tW + tH) { tx = tX + tW - 1 - (pos - tW - tH); ty = tY + tH - 1; }
            else { tx = tX; ty = tY + tH - 1 - (pos - 2 * tW - tH); }
        } else {
            int idx = i - 24;
            tx = tX + tW / 4 + (idx % 4) * (tW / 5);
            ty = tY + tH / 3 + (idx / 4) * (tH / 6);
        }

        int vx = (int)((h >> 20) % 7) - 3;
        int vy = (int)((h >> 24) % 5) - 2;

        int x, y;
        if (elapsed < scatterEnd) {
            int dt = (int)elapsed - delayMs;
            if (dt < 0) dt = 0;
            x = sx + (vx * dt) / 16;
            y = sy + (vy * dt) / 16;
        } else {
            int scatterDt = max(0, (int)scatterEnd - delayMs);
            int endX = sx + (vx * scatterDt) / 16;
            int endY = sy + (vy * scatterDt) / 16;
            endX = max(tX, min(endX, tX + tW - 2));
            endY = max(tY, min(endY, tY + tH - 2));

            float t = (float)(elapsed - scatterEnd) /
                      (float)(convergeEnd - scatterEnd);
            if (t > 1.0f) t = 1.0f;
            t = t * t * (3.0f - 2.0f * t);

            x = endX + (int)((float)(tx - endX) * t);
            y = endY + (int)((float)(ty - endY) * t);
        }

        x = max(tX, min(x, tX + tW - 2));
        y = max(tY, min(y, tY + tH - 2));
        canvas.fillRect(x, y, 2, 2, TERM_PRI);  // blue particles
    }
}

// ==[ THREAT LEVEL ]== composite scoring from Hunt + Recon correlation

uint8_t computeThreatLevel() {
    bool deauthActive = DefensePipeline::snapshot().isDeauthActive();
    uint8_t deauthCh = DefensePipeline::snapshot().getLastDeauthDominantChannel();
    if (deauthCh == 0) deauthCh = DefensePipeline::snapshot().getLastDeauthChannel();
    uint8_t huntCh = Hunt::isActive() ? Hunt::getCurrentChannel() : 0;
    bool evilTwinActive = DefensePipeline::snapshot().isEvilTwinActive();
    bool karmaActive = DefensePipeline::snapshot().isKarmaActive();
    bool fpActive = DefensePipeline::snapshot().isFingerprintMismatchActive();
    bool seqActive = DefensePipeline::snapshot().isSeqAnomalyActive();
    bool rssiActive = DefensePipeline::snapshot().isRssiAnomalyActive();
    int openAPs = DefensePipeline::snapshot().getOpenAPCount();
    int knownAPs = DefensePipeline::snapshot().getKnownAPCount();
    int knownProbeReq = DefensePipeline::snapshot().getKnownProbeRequestCount();
    int knownProbeClients = DefensePipeline::snapshot().getKnownProbeClientCount();
    int following = DefensePipeline::snapshot().getFollowingCount();
    int spam = DefensePipeline::snapshot().getSpamCount();

    // C2: deauth on hunt channel = immediate RED
    bool deauthOnHuntCh = deauthActive && (deauthCh == huntCh) && (huntCh > 0);

    bool dualBandStalk = DefensePipeline::snapshot().isDualBandStalkActive();
    bool activeAttacker = DefensePipeline::snapshot().hasActiveAttacker();

    // crowd context: deserted environment escalates personal threats
    bool deserted = DefensePipeline::snapshot().isDeserted();

    uint8_t targetLevel = 0;
    if (following > 0 || deauthOnHuntCh || (deauthActive && evilTwinActive) ||
        (fpActive && seqActive) ||
        (deauthActive && knownProbeReq >= 3) ||
        dualBandStalk ||
        // deserted escalation: lone deauth/twin/karma = you're the target
        (deserted && deauthActive) ||
        (deserted && evilTwinActive && karmaActive)) {
        targetLevel = 3;
    } else if (deauthActive || evilTwinActive || karmaActive ||
               fpActive || seqActive || rssiActive ||
               openAPs >= 5 || knownAPs >= 3 || spam > 0 ||
               knownProbeReq >= 3 || knownProbeClients >= 2 ||
               activeAttacker ||
               // deserted escalation: open/known APs → ORANGE (normally YELLOW)
               (deserted && (openAPs >= 3 || knownAPs > 0))) {
        targetLevel = 2;
    } else if (openAPs >= 3 || knownAPs > 0 || knownProbeReq > 0 || DefensePipeline::snapshot().getTrackerCount() > 0) {
        targetLevel = 1;
    }

    return targetLevel;
}

static void updateThreatLevel(uint32_t now) {
    uint8_t targetLevel = computeThreatLevel();
    bool deauthActive = DefensePipeline::snapshot().isDeauthActive();
    uint8_t deauthCh = DefensePipeline::snapshot().getLastDeauthDominantChannel();
    if (deauthCh == 0) deauthCh = DefensePipeline::snapshot().getLastDeauthChannel();
    uint8_t huntCh = Hunt::isActive() ? Hunt::getCurrentChannel() : 0;
    bool deauthOnHuntCh = deauthActive && (deauthCh == huntCh) && (huntCh > 0);
    bool dualBandStalk = DefensePipeline::snapshot().isDualBandStalkActive();
    bool deserted = DefensePipeline::snapshot().isDeserted();

    uint8_t newLevel = threatLevel;
    if (targetLevel > threatLevel) {
        newLevel = targetLevel;
        threatDowngradeSince = 0;
    } else if (targetLevel < threatLevel) {
        if (threatDowngradeSince == 0) {
            threatDowngradeSince = now;
        } else if (now - threatDowngradeSince >= THREAT_DOWNGRADE_HOLDOFF_MS) {
            newLevel = targetLevel;
            threatDowngradeSince = 0;
        }
    } else {
        threatDowngradeSince = 0;
    }

    if (newLevel != lastThreatLevel && state == State::ACTIVE) {
        if (newLevel >= 3) {
            if (dualBandStalk) {
                pushLineRaw(LineColor::ALERT, " !! DUAL-BAND TAILING !! ");
            } else if (deauthOnHuntCh) {
                pushLineRaw(LineColor::ALERT, " !! DEAUTH YOUR CH !! ");
            } else if (deserted && deauthActive) {
                pushLineRaw(LineColor::ALERT, " !! DEAUTH — YOURE ALONE !! ");
            } else {
                pushLineRaw(LineColor::ALERT, " !! THREAT RED !! ");
            }
        } else if (newLevel == 2) {
            pushLineRaw(LineColor::FG, "[!] threat: ORANGE");
        } else if (newLevel == 1) {
            pushLineRaw(LineColor::FG, "threat: YELLOW");
        } else {
            pushLineRaw(LineColor::DIM, "threat: GREEN");
        }
        lastThreatLevel = newLevel;
    }
    threatLevel = newLevel;
}

static const char* threatTag() {
    switch (threatLevel) {
        case 3: return "RED";
        case 2: return "ORG";
        case 1: return "YEL";
        default: return "GRN";
    }
}

// ==[ DATA DUMP GENERATORS ]== cross-namespace correlated dumps

// BLE TRACKERS — Recon+Hunt: tracker table, proximity, probe correlation
static void dumpBleTrackers() {
    int trkCount = DefensePipeline::snapshot().getTrackerCount();
    int followCount = DefensePipeline::snapshot().getFollowingCount();
    int spamCnt = DefensePipeline::snapshot().getSpamCount();
    const Recon::TrackerEntry* trackers = DefensePipeline::snapshot().getTrackers();
    int tblSize = DefensePipeline::snapshot().getTrackerTableSize();

    if (tblSize == 0 && DefensePipeline::snapshot().getTotalBLEDevicesSeen() == 0) {
        stageLine(LineColor::DIM, "no BLE activity");
        return;
    }

    // count step-following and watchlist for header
    int stepFollowCount = 0;
    uint8_t watchCount = DefensePipeline::snapshot().getWatchlistCount();
    for (int i = 0; i < tblSize; i++) {
        if (trackers[i].flags & Recon::FLAG_STEP_FOLLOWING) stepFollowCount++;
    }

    // header: compact threat summary
    if (stepFollowCount > 0) {
        stageLine(LineColor::ALERT, "%d trk %d fol %d STALK %d spam",
                  trkCount, followCount, stepFollowCount, spamCnt);
    } else if (watchCount > 0) {
        stageLine(LineColor::FG, "%d trk %d fol %d spam %dW",
                  trkCount, followCount, spamCnt, watchCount);
    } else {
        stageLine(LineColor::FG, "%d trk %d fol %d spam",
                  trkCount, followCount, spamCnt);
    }

    int rows = 0;
    uint32_t now = millis();

    // following trackers first (ALERT color) — STALK vs FOLLOW + distance
    for (int i = 0; i < tblSize && rows < 5; i++) {
        const Recon::TrackerEntry& te = trackers[i];
        if (!(te.flags & Recon::FLAG_FOLLOWING)) continue;
        const char* tn = Recon::deviceLabel(te);
        const char* tag = (te.flags & Recon::FLAG_STEP_FOLLOWING) ? "STALK" : "FOL";
        uint32_t dur = (now - te.firstSeen) / 60000;
        float dist = Recon::estimateDistance(te.rssiSmooth, te.txPower);
        if (dist > 0) {
            stageLine(LineColor::ALERT, " %s %ddB %s %s ~%.1fm",
                      tn, te.rssiSmooth,
                      Recon::proximityLabel(te.rssiSmooth),
                      tag, dist);
        } else {
            stageLine(LineColor::ALERT, " %s %ddB %s %lum",
                      tn, te.rssiSmooth, tag, (unsigned long)dur);
        }
        rows++;

        // MAC rotation rate (NEW — DeTagTive-style forensic data)
        if (te.macChangeCount > 0 && rows < 5) {
            uint32_t spanM = dur > 0 ? dur : 1;
            stageLine(LineColor::FG, " macx%d/%lum",
                      te.macChangeCount, (unsigned long)spanM);
            rows++;
        }

        // relay detection data (Feature 2)
        if ((te.flags & Recon::FLAG_RELAY_SUSPECT) && rows < 5) {
            const char* relayType = te.intervalVariance < 5 ? "SW-RELAY" : "NET-RELAY";
            stageLine(LineColor::ALERT, " RELAY: %s var:%d",
                      relayType, te.intervalVariance);
            rows++;
        }
    }

    // approximate total scan cycles from max seenCount (NEW)
    uint8_t maxSeen = 1;
    for (int i = 0; i < tblSize; i++) {
        if (trackers[i].seenCount > maxSeen) maxSeen = trackers[i].seenCount;
    }

    // other trackers — spam platform, manufacturer, persistence score
    for (int i = 0; i < tblSize && rows < 5; i++) {
        const Recon::TrackerEntry& te = trackers[i];
        if (te.flags & Recon::FLAG_FOLLOWING) continue;
        uint32_t dur = (now - te.firstSeen) / 60000;
        const char* tn;
        if (te.flags & Recon::FLAG_SPAM) {
            switch ((Recon::SpamPlatform)te.spamPlatform) {
                case Recon::SpamPlatform::IOS:     tn = "SP:i"; break;
                case Recon::SpamPlatform::WINDOWS: tn = "SP:W"; break;
                case Recon::SpamPlatform::SAMSUNG: tn = "SP:S"; break;
                case Recon::SpamPlatform::ANDROID: tn = "SP:A"; break;
                default: tn = "Spam"; break;
            }
        } else {
            tn = Recon::deviceLabel(te);
        }
        const char* wFlag = (te.flags & Recon::FLAG_WATCHLISTED) ? " W" : "";
        // persistence % (NEW) — seenCount / maxSeen approximates presence ratio
        int pct = (maxSeen > 1 && te.seenCount > 0) ? (te.seenCount * 100 / maxSeen) : 0;
        if (pct > 0 && pct <= 100) {
            stageLine(LineColor::FG, "%s %ddB%s %lum %d%%",
                      tn, te.rssiSmooth, wFlag,
                      (unsigned long)dur, pct);
        } else {
            stageLine(LineColor::FG, "%s %ddB %s%s %lum",
                      tn, te.rssiSmooth,
                      Recon::proximityLabel(te.rssiSmooth),
                      wFlag, (unsigned long)dur);
        }
        rows++;
    }

    // BLE↔WiFi probe correlation: match tracker.lastSeen±15s AND |rssi diff|<12
    const Hunt::HarvestedProbe* probes = Hunt::getHarvestedProbes();
    uint16_t probeCount = Hunt::getHarvestedCount();
    if (probes && probeCount > 0 && tblSize > 0) {
        for (int i = 0; i < tblSize && rows < 5; i++) {
            const Recon::TrackerEntry& te = trackers[i];
            for (uint16_t p = 0; p < probeCount; p++) {
                int32_t timeDiff = (int32_t)te.lastSeen - (int32_t)probes[p].lastSeen;
                if (timeDiff < 0) timeDiff = -timeDiff;
                int rssiDiff = (int)te.rssiSmooth - (int)probes[p].rssi;
                if (rssiDiff < 0) rssiDiff = -rssiDiff;
                if (timeDiff <= 15000 && rssiDiff < 12) {
                    const char* tn = Recon::deviceLabel(te);
                    stageLine(LineColor::FG, "[CORR] %s ~ %.14s",
                              tn, probes[p].ssid);
                    rows++;
                    break;  // one correlation per tracker
                }
            }
        }
    }

    // footer — watchlist presence or apple continuity
    if (watchCount > 0) {
        const Recon::WatchlistEntry* wl = DefensePipeline::snapshot().getWatchlist();
        int present = 0;
        for (int i = 0; i < Recon::MAX_WATCHLIST; i++) {
            if (wl[i].occupied && wl[i].present) present++;
        }
        stageLine(LineColor::DIM, "%u BLE devs | %d/%dW present",
                  DefensePipeline::snapshot().getTotalBLEDevicesSeen(), present, watchCount);
    } else {
        stageLine(LineColor::DIM, "%u BLE devs | %u apple cont",
                  DefensePipeline::snapshot().getTotalBLEDevicesSeen(),
                  DefensePipeline::snapshot().getAppleContinuityCount());
    }

    // adv interval anti-spoof: measured vs advertised mismatch
    for (int i = 0; i < tblSize && dumpLineCount < DUMP_MAX_LINES; i++) {
        const Recon::TrackerEntry& te = trackers[i];
        if (te.advInterval == 0 || te.measuredAdvIntervalMs == 0) continue;
        uint16_t advMs = (uint16_t)((float)te.advInterval * 0.625f);
        if (advMs == 0) continue;
        int diff = abs((int)te.measuredAdvIntervalMs - (int)advMs);
        if (diff > (int)advMs / 2) {
            const char* tn = Recon::deviceLabel(te);
            stageLine(LineColor::ALERT, " advInt spoof: %s %d/%dms",
                      tn, te.measuredAdvIntervalMs, advMs);
            break;  // one warning max
        }
    }

    // GPS location for following trackers (wardrive context)
    for (int i = 0; i < tblSize && dumpLineCount < DUMP_MAX_LINES; i++) {
        const Recon::TrackerEntry& te = trackers[i];
        if (!(te.flags & Recon::FLAG_FOLLOWING)) continue;
        if (te.lastLat == 0.0f && te.lastLon == 0.0f) continue;
        stageLine(LineColor::DIM, " %.4f,%.4f",
                  te.lastLat, te.lastLon);
        break;  // one GPS coord max
    }
}

// SITREP — Hunt + Recon + Capture + Config
// attack surface, deauth correlation, threat score
// uses Hunt data when available, falls back to Recon's live WiFi scan
static void dumpSitrep() {
    int huntCount = Hunt::getNetworkCount();
    const DetectedNetwork* huntNets = huntCount > 0 ? Hunt::getNetworks() : nullptr;
    const Recon::WifiAP* reconAPs = nullptr;
    int reconCount = 0;
    if (!huntNets) {
        reconAPs = DefensePipeline::snapshot().getWifiSnapshot();
        reconCount = DefensePipeline::snapshot().getWifiSnapshotCount();
    }
    int netCount = huntNets ? huntCount : reconCount;

    if (netCount == 0) {
        const auto pipe = DefensePipeline::snapshot().getWifiPipelineStatus();
        if (!huntNets && pipe.lastCompleteMs == 0) {
            stageLine(pipe.state == Recon::WifiPipelineState::FAILED
                          ? LineColor::ALERT : LineColor::DIM,
                      "wifi pipe: %s fail:%u",
                      Recon::wifiPipelineStateLabel(pipe.state),
                      (unsigned)pipe.consecutiveFailures);
        } else {
            stageLine(LineColor::DIM, "0 APs in last sweep");
        }
        stageLine(LineColor::DIM, "threat: GREEN");
        return;
    }

    // C1: attack surface walk — works with either data source
    int pwned = 0, hardened = 0, attackable = 0, pmfGap = 0;
    int wpsVuln = 0, hidden = 0, openCount = 0;
    int probesSent = 0, probesResp = 0;
    int totalClients = 0;

    for (int i = 0; i < netCount; i++) {
        uint8_t auth;
        if (huntNets) {
            const DetectedNetwork& n = huntNets[i];
            auth = (uint8_t)n.authmode;
            if (n.hasPMKID || n.hasHandshake) pwned++;
            if (n.hasPMF) hardened++;
            if (!n.hasPMF && auth >= 2) pmfGap++;
            if (!n.hasPMF && auth >= 2 && !n.hasPMKID && !n.hasHandshake) attackable++;
            if (n.wpsState > 0) wpsVuln++;
            if (n.isHidden) hidden++;
            if (n.probeAttempts > 0) { probesSent++; if (n.gotResponse) probesResp++; }
            totalClients += n.clientCount;
        } else {
            auth = reconAPs[i].authMode;
            // WPA2/WPA3 without PMF data — count non-open as potentially attackable
            if (auth >= 2) attackable++;
        }
        if (auth == 0) openCount++;
    }

    if (huntNets) {
        stageLine(LineColor::FG, "%d tgts: %d pwned %d hardened",
                  netCount, pwned, hardened);
    } else {
        stageLine(LineColor::FG, "%d APs scanned", netCount);
    }

    // secondary counts
    char extra[LINE_LEN] = {0};
    int pos = 0;
    if (attackable > 0) pos += snprintf(extra + pos, LINE_LEN - pos, "%d atk", attackable);
    if (wpsVuln > 0) pos += snprintf(extra + pos, LINE_LEN - pos, "%s%d wps", pos ? " | " : "", wpsVuln);
    if (hidden > 0) pos += snprintf(extra + pos, LINE_LEN - pos, "%s%d hid", pos ? " | " : "", hidden);
    if (openCount > 0) pos += snprintf(extra + pos, LINE_LEN - pos, "%s%d open", pos ? " | " : "", openCount);
    if (pos > 0) stageLine(LineColor::FG, "%s", extra);

    // probe success rate (hunt only)
    if (probesSent > 0) {
        int pct = (probesResp * 100) / probesSent;
        stageLine(LineColor::DIM, "probe: %d/%d (%d%%)", probesResp, probesSent, pct);
    }
    if (huntNets && netCount > 0) {
        int pmfGapPct = (pmfGap * 100) / netCount;
        stageLine(LineColor::DIM, "pmf gap: %d/%d (%d%%)", pmfGap, netCount, pmfGapPct);
    }
    if (totalClients > 0) {
        stageLine(LineColor::DIM, "clients: %d", totalClients);
    }

    const char* cadence = "N";
    switch (DefensePipeline::snapshot().getCadenceTier()) {
        case Recon::CadenceTier::AGGRESSIVE: cadence = "A"; break;
        case Recon::CadenceTier::ELEVATED:   cadence = "E"; break;
        default:                             cadence = "N"; break;
    }
    char reconState = 'I';
    if (!DefensePipeline::snapshot().isActive()) reconState = 'X';
    else if (DefensePipeline::snapshot().isParasiticMode()) reconState = 'P';
    else if (DefensePipeline::snapshot().isScanning()) reconState = 'S';
    uint32_t scanAgeMs = DefensePipeline::snapshot().getTimeSinceLastScan();
    uint32_t scanAgeS = (scanAgeMs == 0xFFFFFFFF) ? 0 : (scanAgeMs / 1000);
    uint32_t wifiScanS = DefensePipeline::snapshot().getCurrentWifiScanIntervalMs() / 1000;
    uint32_t deauthSniffS = DefensePipeline::snapshot().getCurrentDeauthSniffMs() / 1000;
    uint32_t sentinelS = DefensePipeline::snapshot().getCurrentSentinelIntervalMs() / 1000;
    if (sentinelS > 0) {
        stageLine(LineColor::DIM, "rcn %s/%c a:%lu w:%lu d:%lu s:%lu",
                  cadence, reconState,
                  (unsigned long)scanAgeS,
                  (unsigned long)wifiScanS,
                  (unsigned long)deauthSniffS,
                  (unsigned long)sentinelS);
    } else {
        stageLine(LineColor::DIM, "rcn %s/%c a:%lu w:%lu d:%lu",
                  cadence, reconState,
                  (unsigned long)scanAgeS,
                  (unsigned long)wifiScanS,
                  (unsigned long)deauthSniffS);
    }
    if (Hunt::isDualBandActive()) {
        uint16_t c5Nets = Hunt::getC5MonsterNetworkCount();
        uint8_t c5Ch = Hunt::getC5MonsterBestChannel();
        int8_t c5Rssi = Hunt::getC5MonsterBestRssi();
        uint32_t c5Age = Hunt::getC5MonsterScanAgeMs();
        if (c5Nets > 0) {
            uint32_t c5AgeS = (c5Age == UINT32_MAX) ? 0 : (c5Age / 1000);
            if (c5AgeS > 0) {
                stageLine(LineColor::DIM, "5G: %u APs ch%u %ddBm age%lus",
                          c5Nets, c5Ch, (int)c5Rssi, (unsigned long)c5AgeS);
            } else {
                stageLine(LineColor::DIM, "5G: %u APs ch%u %ddBm",
                          c5Nets, c5Ch, (int)c5Rssi);
            }
        } else {
            stageLine(LineColor::DIM, "5G: active no APs");
        }
    }

    // C5: nearby probe requests matching known SSIDs.
    uint16_t vulnProbes = DefensePipeline::snapshot().getKnownProbeRequestCount();
    uint16_t vulnProbeClients = DefensePipeline::snapshot().getKnownProbeClientCount();
    if (vulnProbes > 0) {
        stageLine(LineColor::FG, "vuln probes: %d req %d cli", vulnProbes, vulnProbeClients);
    }

    uint8_t fpCount = DefensePipeline::snapshot().getRecentFingerprintMismatchCount();
    uint8_t seqCount = DefensePipeline::snapshot().getRecentSeqAnomalyCount();
    uint8_t rssiCount = DefensePipeline::snapshot().getRecentRssiAnomalyCount();
    if (fpCount > 0 || seqCount > 0 || rssiCount > 0) {
        stageLine(LineColor::FG, "forn fp:%u seq:%u rs:%u",
                  fpCount, seqCount, rssiCount);
    }

    // BLE threat context — cross-domain awareness
    int bleSpamN = DefensePipeline::snapshot().getSpamCount();
    int bleFollowN = DefensePipeline::snapshot().getFollowingCount();
    if (bleSpamN > 0 && bleFollowN > 0) {
        stageLine(LineColor::FG, "BLE: %d spam %d follow",
                  bleSpamN, bleFollowN);
    } else if (bleSpamN > 0) {
        stageLine(LineColor::FG, "BLE: %d spam sources", bleSpamN);
    } else if (bleFollowN > 0) {
        stageLine(LineColor::FG, "BLE: %d trackers following",
                  bleFollowN);
    }

    // C2: deauth correlation
    bool deauthActive = DefensePipeline::snapshot().isDeauthActive();
    bool evilTwinActive = DefensePipeline::snapshot().isEvilTwinActive();
    bool karmaActive = DefensePipeline::snapshot().isKarmaActive();
    bool fpActive = DefensePipeline::snapshot().isFingerprintMismatchActive();
    bool seqActive = DefensePipeline::snapshot().isSeqAnomalyActive();
    bool rssiActive = DefensePipeline::snapshot().isRssiAnomalyActive();
    uint8_t huntCh = Hunt::isActive() ? Hunt::getCurrentChannel() : 0;
    uint8_t dchPeak = DefensePipeline::snapshot().getLastDeauthChannel();
    uint8_t dchDom = DefensePipeline::snapshot().getLastDeauthDominantChannel();
    bool deauthOnHuntCh = deauthActive && (dchDom == huntCh) && (huntCh > 0);

    if (deauthActive) {
        uint16_t dc = DefensePipeline::snapshot().getLastDeauthBurstCount();
        if (dc == 0) dc = DefensePipeline::snapshot().getDeauthCount();
        uint8_t srcn = DefensePipeline::snapshot().getLastDeauthUniqueSources();
        uint16_t pps = DefensePipeline::snapshot().getLastDeauthPPS();
        uint16_t dOnly = DefensePipeline::snapshot().getLastDeauthSubtypeCount();
        uint16_t aOnly = DefensePipeline::snapshot().getLastDisassocSubtypeCount();
        if (deauthOnHuntCh) {
            stageLine(LineColor::ALERT, " !! DEAUTH ch%02d YOUR CH !!",
                      dchDom);
        }

        // tool signature from burst history (NEW)
        uint8_t burstN = DefensePipeline::snapshot().getDeauthBurstHistoryCount();
        const Recon::DeauthBurstRecord* hist = DefensePipeline::snapshot().getDeauthBurstHistory();
        uint16_t reason = 1;
        if (hist && burstN > 0) {
            // find most recent burst
            uint32_t latestTs = 0;
            int latestIdx = 0;
            for (uint8_t b = 0; b < burstN; b++) {
                if (hist[b].timestamp > latestTs) {
                    latestTs = hist[b].timestamp;
                    latestIdx = b;
                }
            }
            reason = hist[latestIdx].dominantReason;
            const char* tool = toolSignature(reason, pps, srcn, dOnly, aOnly);
            stageLine(LineColor::FG, "sig: r:%d %dsrc %dpps %s",
                      reason, srcn, pps, tool);

            // Recon engine tool classification (Feature 6)
            Recon::DeauthTool dtool = DefensePipeline::snapshot().getLastDeauthTool();
            if (dtool != Recon::DeauthTool::UNKNOWN) {
                stageLine(LineColor::FG, "tool: %s",
                          Recon::deauthToolLabel(dtool));
            }

            // target BSSID + SSID (NEW)
            const uint8_t* tgtBssid = hist[latestIdx].targetBssid;
            bool hasTgt = false;
            for (int k = 0; k < 6; k++) { if (tgtBssid[k]) { hasTgt = true; break; } }
            if (hasTgt && huntNets) {
                // find SSID for target BSSID
                const char* tgtSSID = nullptr;
                for (int i = 0; i < huntCount; i++) {
                    if (memcmp(huntNets[i].bssid, tgtBssid, 6) == 0) {
                        tgtSSID = huntNets[i].ssid;
                        break;
                    }
                }
                if (tgtSSID && tgtSSID[0]) {
                    stageLine(LineColor::FG, "target: %02X:%02X \"%.*s\"",
                              tgtBssid[4], tgtBssid[5], 12, tgtSSID);
                } else {
                    stageLine(LineColor::FG, "target: %02X:%02X:%02X:%02X",
                              tgtBssid[2], tgtBssid[3], tgtBssid[4], tgtBssid[5]);
                }
            }
        } else if (!deauthOnHuntCh) {
            // fallback: old-style display when no burst history
            int8_t dr = DefensePipeline::snapshot().getLastDeauthRSSI();
            stageLine(LineColor::FG, "DEAUTH d%02d p%02d %ddB x%d",
                      dchDom, dchPeak, dr, dc);
        }
    }

    // Evidence chain summary for operator awareness.
    // 64 bytes: max ~48 chars ("ioc:" + 12 indicators) + null, LINE_LEN too small
    char ioc[64] = {0};
    int iocPos = snprintf(ioc, sizeof(ioc), "ioc:");
    auto iocAppend = [&](const char* tag) {
        if (iocPos < (int)sizeof(ioc) - 1)
            iocPos += snprintf(ioc + iocPos, sizeof(ioc) - iocPos, "%s", tag);
    };
    if (deauthActive) iocAppend(" dth");
    if (deauthOnHuntCh) iocAppend(" ctr");
    if (evilTwinActive) iocAppend(" twn");
    if (karmaActive) iocAppend(" krm");
    if (fpActive) {
        // IE change detail: show WHAT changed instead of generic "fp"
        // scan Hunt networks for the triggering mismatch
        uint8_t changeMask = 0;
        if (huntNets) {
            for (int i = 0; i < huntCount; i++) {
                if (huntNets[i].fingerprintMismatchStreak >= 2)
                    changeMask |= huntNets[i].ieChangeMask;
            }
        }
        if (changeMask & 0x02) iocAppend(" RSN");
        else if (changeMask & 0x01) iocAppend(" RATE");
        else if (changeMask & 0x08) iocAppend(" HTC");
        else if (changeMask & 0x04) iocAppend(" VND");
        else iocAppend(" fp");
    }
    if (seqActive) iocAppend(" seq");
    if (rssiActive) iocAppend(" rs");
    if (openCount >= 3) iocAppend(" opn");
    if (DefensePipeline::snapshot().getKnownAPCount() > 0) iocAppend(" knw");
    if (vulnProbes > 0) iocAppend(" prb");
    // BLE attack concurrency — spam/Flipper active within 30s during WiFi attack
    bool bleAttackActive = false;
    {
        const Recon::TrackerEntry* trkTbl = DefensePipeline::snapshot().getTrackers();
        int trkSz = DefensePipeline::snapshot().getTrackerTableSize();
        uint32_t nowMs = millis();
        for (int i = 0; i < trkSz; i++) {
            if (nowMs - trkTbl[i].lastSeen > 30000) continue;
            if ((trkTbl[i].flags & Recon::FLAG_SPAM) ||
                trkTbl[i].type == Recon::ThreatType::FLIPPER) {
                bleAttackActive = true;
                break;
            }
        }
    }
    if (bleAttackActive) iocAppend(" ble");
    if (iocPos <= 4) snprintf(ioc, sizeof(ioc), "ioc: none");
    stageLine((deauthOnHuntCh || (evilTwinActive && deauthActive) || (fpActive && seqActive)
               || (bleAttackActive && deauthActive))
                  ? LineColor::ALERT : LineColor::DIM,
              "%s", ioc);

    // Evidence chain confidence from forensic log
    // Scan Hunt networks with active anomalies for multi-indicator convergence
    if (huntNets && (fpActive || seqActive || rssiActive || evilTwinActive || karmaActive)) {
        uint8_t maxInd = 0;
        int bestNet = -1;
        for (int i = 0; i < huntCount; i++) {
            uint8_t ind = DefensePipeline::snapshot().countIndicatorsForBSSID(huntNets[i].bssid, 60000);
            if (ind > maxInd) { maxInd = ind; bestNet = i; }
        }
        if (maxInd >= 2 && bestNet >= 0) {
            const char* conf = maxInd >= 3 ? "HIGH" : "MED";
            const char* ssid = huntNets[bestNet].ssid;
            char shortSSID[13];
            if (strlen(ssid) > 12) { snprintf(shortSSID, sizeof(shortSSID), "%.9s...", ssid); ssid = shortSSID; }
            stageLine(maxInd >= 3 ? LineColor::ALERT : LineColor::FG,
                      "chain: %s %dind \"%s\"", conf, maxInd, ssid);
        }
    }

    // Deauth burst cadence analysis (improved: require 3+ for cadence)
    {
        uint8_t bcnt = DefensePipeline::snapshot().getDeauthBurstHistoryCount();
        const Recon::DeauthBurstRecord* bh = DefensePipeline::snapshot().getDeauthBurstHistory();
        if (bh && bcnt >= 3) {
            // sort timestamps to compute intervals
            uint32_t ts[8];
            for (uint8_t b = 0; b < bcnt && b < 8; b++) ts[b] = bh[b].timestamp;
            for (uint8_t i = 1; i < bcnt; i++)
                for (uint8_t j = i; j > 0 && ts[j] < ts[j-1]; j--) {
                    uint32_t tmp = ts[j]; ts[j] = ts[j-1]; ts[j-1] = tmp;
                }
            uint32_t iSum = 0;
            uint32_t iMin = UINT32_MAX, iMax = 0;
            for (uint8_t i = 1; i < bcnt; i++) {
                uint32_t dt = ts[i] - ts[i-1];
                iSum += dt;
                if (dt < iMin) iMin = dt;
                if (dt > iMax) iMax = dt;
            }
            uint32_t avgS = iSum / ((bcnt - 1) * 1000);
            uint32_t spread = iMax - iMin;
            bool isAuto = (spread < iSum / ((bcnt-1) * 3)) && (avgS < 60);
            stageLine(isAuto ? LineColor::FG : LineColor::DIM,
                      "dth: %d burst ~%lus %s",
                      bcnt, (unsigned long)avgS, isAuto ? "AUTO" : "MANUAL");
        } else if (bh && bcnt >= 2) {
            // just show count + span
            uint32_t tMin = UINT32_MAX, tMax = 0;
            for (uint8_t b = 0; b < bcnt; b++) {
                if (bh[b].timestamp < tMin) tMin = bh[b].timestamp;
                if (bh[b].timestamp > tMax) tMax = bh[b].timestamp;
            }
            uint32_t spanS = (tMax > tMin) ? (tMax - tMin) / 1000 : 0;
            stageLine(LineColor::DIM, "dth: %d bursts %lus",
                      bcnt, (unsigned long)spanS);
        }
    }

    // ==[ FORENSIC LOG TIMELINE ]== last 2 events chronological (NEW)
    {
        const Recon::ForensicLogEntry* flog = DefensePipeline::snapshot().getForensicLog();
        uint8_t fcount = DefensePipeline::snapshot().getForensicLogCount();
        uint8_t fhead = DefensePipeline::snapshot().getForensicLogHead();
        if (flog && fcount > 0 && dumpLineCount < DUMP_MAX_LINES - 2) {
            uint32_t now = millis();
            // collect recent events (last 10min)
            struct { uint32_t ts; Recon::ReconEvent ev; uint8_t ch; int8_t rssi; } recent[4];
            int rcount = 0;
            for (uint8_t i = 0; i < fcount && rcount < 4; i++) {
                int idx = ((int)fhead - 1 - i + Recon::MAX_FORENSIC_LOG) % Recon::MAX_FORENSIC_LOG;
                const Recon::ForensicLogEntry& e = flog[idx];
                if (now - e.timestamp > 600000) continue;
                recent[rcount++] = { e.timestamp, e.event, e.channel, e.rssi };
            }
            // show last 2 (oldest first)
            int showN = (rcount > 2) ? 2 : rcount;
            for (int i = showN - 1; i >= 0; i--) {
                char ts[12];
                fmtUptime(ts, sizeof(ts), recent[i].ts);
                stageLine(LineColor::DIM, "[%s] %s ch%02d",
                          ts, eventTag(recent[i].ev), recent[i].ch);
            }
            // event sequence narrative (NEW)
            if (rcount >= 2 && dumpLineCount < DUMP_MAX_LINES - 1) {
                char chain[LINE_LEN] = {0};
                int cp = 0;
                int chainN = (rcount > 4) ? 4 : rcount;
                for (int i = chainN - 1; i >= 0; i--) {
                    if (cp > 0 && cp < (int)sizeof(chain) - 4) {
                        chain[cp++] = '-';
                        chain[cp++] = '>';
                    }
                    const char* tag = eventTag(recent[i].ev);
                    int tlen = strlen(tag);
                    if (cp + tlen < (int)sizeof(chain) - 1) {
                        memcpy(chain + cp, tag, tlen);
                        cp += tlen;
                    }
                }
                chain[cp] = '\0';
                uint32_t spanM = (recent[0].ts > recent[rcount-1].ts)
                    ? (recent[0].ts - recent[rcount-1].ts) / 60000
                    : (recent[rcount-1].ts - recent[0].ts) / 60000;
                stageLine(LineColor::FG, "evt: %s %lum", chain, (unsigned long)spanM);
            }
        }
    }

    // threat score
    if (threatLevel >= 3) {
        stageLine(LineColor::ALERT, " !! THREAT RED !! ");
    } else if (threatLevel == 2) {
        stageLine(LineColor::FG, "[!] threat: ORANGE");
    } else if (threatLevel == 1) {
        stageLine(LineColor::FG, "threat: YELLOW");
    } else {
        stageLine(LineColor::DIM, "threat: GREEN");
    }
}

// YIELD — Hunt + Recon + Config
// D-UCB per-channel yield with lifetime career context
// falls back to Recon scan for per-channel AP breakdown
static void dumpYield() {
    struct ChRow { uint8_t ch; uint16_t pulls; uint16_t nets; uint8_t caps; float yield; uint16_t career; };
    ChRow rows[13];
    int count = 0;

    bool hasHuntData = Hunt::isActive();
    if (hasHuntData) {
        hasHuntData = false;
        for (uint8_t ch = 1; ch <= 13; ch++) {
            const ChannelStats* cs = Hunt::getChannelStats(ch);
            if (cs && cs->networkCount > 0) { hasHuntData = true; break; }
        }
    }

    if (hasHuntData) {
        // hunt mode: full D-UCB data
        for (uint8_t ch = 1; ch <= 13; ch++) {
            const ChannelStats* cs = Hunt::getChannelStats(ch);
            if (!cs || cs->networkCount == 0) continue;
            DUCBStats ducb = Hunt::getDUCBStats(ch, Hunt::getCurrentBehavior());
            uint16_t lifetime = Config::getChannelRewards(ch);
            uint8_t caps = cs->pmkidHits + cs->handshakeHits;
            float yld = (ducb.pulls > 0) ? ((float)caps / (float)ducb.pulls * 100.0f) : 0.0f;
            rows[count++] = { ch, ducb.pulls, cs->networkCount, caps, yld, lifetime };
        }
    } else {
        // recon mode: derive per-channel AP counts from live scan
        const Recon::WifiAP* aps = DefensePipeline::snapshot().getWifiSnapshot();
        int apCount = DefensePipeline::snapshot().getWifiSnapshotCount();
        uint16_t chNets[14] = {0};
        for (int i = 0; i < apCount; i++) {
            uint8_t ch = aps[i].channel;
            if (ch >= 1 && ch <= 13) chNets[ch]++;
        }
        for (uint8_t ch = 1; ch <= 13; ch++) {
            if (chNets[ch] == 0) continue;
            uint16_t lifetime = Config::getChannelRewards(ch);
            rows[count++] = { ch, 0, chNets[ch], 0, 0.0f, lifetime };
        }
    }

    if (count == 0) {
        stageLine(LineColor::DIM, "no channel data");
        return;
    }

    // sort by caps desc, then nets desc
    for (int i = 1; i < count; i++) {
        ChRow tmp = rows[i];
        int j = i - 1;
        while (j >= 0 && (rows[j].caps < tmp.caps ||
               (rows[j].caps == tmp.caps && rows[j].nets < tmp.nets))) {
            rows[j + 1] = rows[j];
            j--;
        }
        rows[j + 1] = tmp;
    }

    if (hasHuntData) {
        stageLine(LineColor::DIM, "ch pull cap yld%% career");
        int maxShow = (count > 5) ? 5 : count;
        for (int i = 0; i < maxShow; i++) {
            const ChRow& r = rows[i];
            stageLine(r.caps > 0 ? LineColor::HYPE : LineColor::FG,
                      "%02d %4d %3d %4.1f%% %4d",
                      r.ch, r.pulls, r.caps, r.yield, r.career);
        }
        if (Hunt::isActive()) {
            stageLine(LineColor::FG, "%s ch:%02d",
                      behaviorTag(Hunt::getCurrentBehavior()),
                      Hunt::getCurrentChannel());
        }
    } else {
        stageLine(LineColor::DIM, "ch  APs  career");
        int maxShow = (count > 6) ? 6 : count;
        for (int i = 0; i < maxShow; i++) {
            const ChRow& r = rows[i];
            stageLine(r.career > 0 ? LineColor::FG : LineColor::DIM,
                      "%02d  %3d   %4d",
                      r.ch, r.nets, r.career);
        }
    }
}

// STASH — Hunt + Capture + Config
// capture intel, hit rate, buffer usage
static void dumpStash() {
    uint32_t careerPmk = Config::getTotalPMKIDs();
    uint32_t careerHs = Config::getTotalHandshakes();
    uint16_t unsynced = Capture::getUnsyncedCount();
    uint8_t sesPmk = Config::getSessionPMKIDCount();
    uint8_t sesHs = Config::getSessionHSCount();
    uint8_t combo = Hunt::getCaptureComboCount();

    // career stash
    if (unsynced > 0) {
        stageLine(LineColor::HYPE, "stash: %lupmk %luhs (%d unsync)",
                  (unsigned long)careerPmk, (unsigned long)careerHs, unsynced);
    } else {
        stageLine(LineColor::FG, "stash: %lupmk %luhs",
                  (unsigned long)careerPmk, (unsigned long)careerHs);
    }

    // session captures + combo
    if (sesPmk > 0 || sesHs > 0) {
        if (combo > 1) {
            stageLine(LineColor::HYPE, "session: +%dpmk +%dhs Cx%d",
                      sesPmk, sesHs, combo);
        } else {
            stageLine(LineColor::HYPE, "session: +%dpmk +%dhs",
                      sesPmk, sesHs);
        }
    }

    // hit rate (pwned / total nets)
    int netCount = Hunt::getNetworkCount();
    const DetectedNetwork* nets = Hunt::getNetworks();
    int pwnedCount = 0;
    for (int i = 0; i < netCount; i++) {
        if (nets[i].hasPMKID || nets[i].hasHandshake) pwnedCount++;
    }
    if (netCount > 0) {
        int hitPct = (pwnedCount * 100) / netCount;
        stageLine(LineColor::FG, "pwned: %d/%d nets (%d%%)",
                  pwnedCount, netCount, hitPct);
    }

    // buffer usage bar
    uint32_t used = Capture::getUsedBytes();
    uint32_t free_b = Capture::getFreeBytes();
    uint32_t total = used + free_b;
    int pct = (total > 0) ? (int)((used * 100UL) / total) : 0;
    int filled = (pct * 16 + 50) / 100;
    if (filled > 16) filled = 16;
    char bar[20];
    bar[0] = '[';
    for (int i = 0; i < 16; i++) bar[i + 1] = (i < filled) ? '#' : '.';
    bar[17] = ']';
    bar[18] = '\0';
    float usedM = (float)used / (1024.0f * 1024.0f);
    stageLine(LineColor::DIM, "buf %s %.1fM", bar, usedM);
}

// CLIENTS — Recon + Hunt + Potfile (C5 cross-reference)
// probe harvest with vulnerable client alerts
static void dumpClients() {
    // C5: potfile cross-ref — from Recon (always available, even without Hunt)
    uint8_t vulnCount = DefensePipeline::snapshot().getVulnProbeCount();
    const Recon::ProbeVulnMatch* vulnCache = vulnCount > 0 ? DefensePipeline::snapshot().getVulnProbeCache() : nullptr;
    int vulnShown = 0;
    for (uint8_t v = 0; v < vulnCount && vulnShown < 2; v++) {
        const Recon::ProbeVulnMatch& m = vulnCache[v];
        char mac[9];
        snprintf(mac, sizeof(mac), "%02X:%02X:%02X",
                 m.clientMac[3], m.clientMac[4], m.clientMac[5]);
        char ssid[15];
        strncpy(ssid, m.ssid, 14);
        ssid[14] = '\0';
        stageLine(LineColor::ALERT, " [VULN] %s -> %s", mac, ssid);
        vulnShown++;
    }

    // Hunt harvested probes (only available during/after Hunt)
    uint16_t count = Hunt::getHarvestedCount();
    const Hunt::HarvestedProbe* probes = Hunt::getHarvestedProbes();
    uint16_t totalReqs = Hunt::getTotalProbeRequests();

    if (count == 0 || !probes) {
        if (vulnShown == 0) {
            stageLine(LineColor::DIM, "no probes harvested");
        }
        return;
    }

    stageLine(LineColor::FG, "%d clients | %d requests", count, totalReqs);

    // regular harvest entries
    int maxShow = (count > 4) ? 4 : count;
    for (int i = 0; i < maxShow; i++) {
        const Hunt::HarvestedProbe& p = probes[i];
        char mac[9];
        snprintf(mac, sizeof(mac), "%02X:%02X:%02X",
                 p.clientMac[3], p.clientMac[4], p.clientMac[5]);
        char ssid[15];
        strncpy(ssid, p.ssid[0] ? p.ssid : "<any>", 14);
        ssid[14] = '\0';
        stageLine(LineColor::FG, "%s %-14s%4d", mac, ssid, p.rssi);
    }

    // spectrum probes (when Spectrum active, supplements Hunt data)
    if (::Spectrum::isActive() && ::Spectrum::getProbeCount() > 0) {
        uint16_t sCount = ::Spectrum::getProbeCount();
        const SpectrumProbeEntry* sp = ::Spectrum::getProbeEntries();
        if (sp) {
            stageLine(LineColor::DIM, "-- spectrum probes: %d --", sCount);
            int sMax = (sCount > 3) ? 3 : sCount;
            for (int i = 0; i < sMax; i++) {
                char mac[9];
                snprintf(mac, sizeof(mac), "%02X:%02X:%02X",
                         sp[i].clientMAC[3], sp[i].clientMAC[4], sp[i].clientMAC[5]);
                char ssid[15];
                strncpy(ssid, sp[i].ssid[0] ? sp[i].ssid : "<any>", 14);
                ssid[14] = '\0';
                stageLine(LineColor::FG, "%s %-14s%4d", mac, ssid, sp[i].rssi);
            }
        }
    }
}

// RF POSTURE — Hunt + Spectrum + Recon
// RF environment overview. Uses Spectrum/Hunt when available, Recon scan otherwise.
static void dumpRfPosture() {
    uint8_t huntCh = Hunt::isActive() ? Hunt::getCurrentChannel() : 0;

    // spectrum data only valid while mode is running
    bool hasSpectrum = ::Spectrum::isActive();

    if (Hunt::isActive() && huntCh > 0) {
        if (hasSpectrum) {
            int8_t huntPeak = ::Spectrum::getChannelPeakRSSI(huntCh);
            uint16_t huntNets = ::Spectrum::getChannelNetworkCount(huntCh);
            stageLine(LineColor::FG, "hunt ch:%02d peak:%ddB %d APs",
                      huntCh, huntPeak, huntNets);
        } else {
            stageLine(LineColor::FG, "hunt ch:%02d", huntCh);
        }
    }

    if (hasSpectrum) {
        // full spectrum data — channel noise bars (1, 6, 11)
        for (uint8_t ch : {1, 6, 11}) {
            int8_t peak = ::Spectrum::getChannelPeakRSSI(ch);
            uint16_t nets = ::Spectrum::getChannelNetworkCount(ch);
            int fill = (peak + 100) * 11 / 70;
            if (fill < 0) fill = 0;
            if (fill > 11) fill = 11;
            char bar[12];
            for (int b = 0; b < 11; b++) bar[b] = (b < fill) ? '#' : '.';
            bar[11] = '\0';
            char marker = (ch == huntCh) ? '*' : ' ';
            stageLine(ch == huntCh ? LineColor::FG : LineColor::DIM,
                      "%c%02d %s%4d %d", marker, ch, bar, peak, nets);
        }
    } else {
        // recon scan fallback — derive per-channel from live WiFi scan
        const Recon::WifiAP* aps = DefensePipeline::snapshot().getWifiSnapshot();
        int apCount = DefensePipeline::snapshot().getWifiSnapshotCount();
        if (apCount == 0) {
            stageLine(LineColor::DIM, "no rf data");
            return;
        }

        // count APs and find strongest RSSI per channel
        uint16_t chNets[14] = {0};
        int8_t chPeak[14];
        for (int i = 0; i < 14; i++) chPeak[i] = -100;
        for (int i = 0; i < apCount; i++) {
            uint8_t ch = aps[i].channel;
            if (ch >= 1 && ch <= 13) {
                chNets[ch]++;
                if (aps[i].rssi > chPeak[ch]) chPeak[ch] = aps[i].rssi;
            }
        }

        stageLine(LineColor::FG, "%d APs across %d channels",
                  apCount, (int)(chNets[1]>0) + (int)(chNets[6]>0) + (int)(chNets[11]>0)
                  + (int)(chNets[2]>0) + (int)(chNets[3]>0) + (int)(chNets[4]>0)
                  + (int)(chNets[5]>0) + (int)(chNets[7]>0) + (int)(chNets[8]>0)
                  + (int)(chNets[9]>0) + (int)(chNets[10]>0) + (int)(chNets[12]>0)
                  + (int)(chNets[13]>0));

        // show non-overlapping channels (1, 6, 11)
        for (uint8_t ch : {1, 6, 11}) {
            if (chNets[ch] == 0 && chPeak[ch] <= -100) continue;
            int fill = (chPeak[ch] + 100) * 11 / 70;
            if (fill < 0) fill = 0;
            if (fill > 11) fill = 11;
            char bar[12];
            for (int b = 0; b < 11; b++) bar[b] = (b < fill) ? '#' : '.';
            bar[11] = '\0';
            stageLine(LineColor::FG, " %02d %s%4d %d",
                      ch, bar, chPeak[ch], chNets[ch]);
        }

        // show busiest non-standard channel if any
        uint8_t busyCh = 0;
        uint16_t busyMax = 0;
        for (uint8_t ch : {2, 3, 4, 5, 7, 8, 9, 10, 12, 13}) {
            if (chNets[ch] > busyMax) { busyMax = chNets[ch]; busyCh = ch; }
        }
        if (busyCh > 0) {
            stageLine(LineColor::DIM, " %02d %d APs (off-grid)",
                      busyCh, chNets[busyCh]);
        }
    }

    // alt channel suggestion (hunt data only, live hunt + spectrum required)
    if (Hunt::isActive() && huntCh > 0 && hasSpectrum) {
        uint8_t altCh = 0;
        int altScore = -999;
        for (uint8_t ch = 1; ch <= 13; ch++) {
            if (ch == huntCh) continue;
            const ChannelStats* cs = Hunt::getChannelStats(ch);
            if (!cs || cs->networkCount == 0) continue;
            int8_t peak = ::Spectrum::getChannelPeakRSSI(ch);
            int score = cs->attackableCount * 10 - (peak + 100);
            if (score > altScore) { altScore = score; altCh = ch; }
        }
        if (altCh > 0) {
            const ChannelStats* altCs = Hunt::getChannelStats(altCh);
            if (altCs) {
                stageLine(LineColor::HYPE, "alt: ch:%02d %d tgts less noise",
                          altCh, altCs->attackableCount);
            }
        }
    }

    // spectrum vuln tiers + readiness (only when Spectrum active)
    if (hasSpectrum) {
        const uint8_t* tiers = ::Spectrum::getVulnTiers();
        const uint8_t* ready = ::Spectrum::getReadinessScores();
        uint16_t nCount = ::Spectrum::getNetworkCount();
        if (tiers && ready && nCount > 0) {
            // count per tier
            uint8_t tierCount[8] = {0};
            for (uint16_t i = 0; i < nCount && i < 48; i++) {
                uint8_t t = tiers[i] < 8 ? tiers[i] : 7;
                tierCount[t]++;
            }
            // show tiers with counts (T0=most vuln, T7=unknown)
            char buf[32];
            int pos = 0;
            for (int t = 0; t <= 6 && pos < 28; t++) {
                if (tierCount[t] > 0) {
                    pos += snprintf(buf + pos, sizeof(buf) - pos, "T%d:%d ", t, tierCount[t]);
                }
            }
            if (pos > 0) {
                stageLine(LineColor::HYPE, "vuln %s", buf);
            }
            // show top readiness score
            uint8_t topReady = 0;
            for (uint16_t i = 0; i < nCount && i < 48; i++) {
                if (ready[i] > topReady) topReady = ready[i];
            }
            if (topReady > 30) {
                stageLine(LineColor::HYPE, "peak readiness: %d%%", topReady);
            }
        }

        // BSSID cluster summary
        uint16_t clCount = ::Spectrum::getClusterCount();
        if (clCount > 0) {
            stageLine(LineColor::DIM, "%d AP clusters (OUI groups)", clCount);
        }
    }

    // top vendor IE OUI among hunt-scanned APs — chipset fingerprint
    {
        const DetectedNetwork* hn = Hunt::getNetworks();
        int hnCount = Hunt::getNetworkCount();
        if (hn && hnCount > 1) {
            uint8_t topOui[3] = {0};
            uint8_t topCount = 0;
            for (int i = 0; i < hnCount; i++) {
                if (!hn[i].hasVendorOUI) continue;
                uint8_t cnt = 0;
                for (int j = 0; j < hnCount; j++) {
                    if (hn[j].hasVendorOUI &&
                        memcmp(hn[i].vendorOUI, hn[j].vendorOUI, 3) == 0) cnt++;
                }
                if (cnt > topCount) {
                    topCount = cnt;
                    memcpy(topOui, hn[i].vendorOUI, 3);
                }
            }
            if (topCount >= 2) {
                stageLine(LineColor::DIM, "vend %02X:%02X:%02X x%d",
                          topOui[0], topOui[1], topOui[2], topCount);
            }
        }
    }

    // beacon entropy anomaly — low IE diversity = likely fake AP
    const Recon::WifiAP* aps = DefensePipeline::snapshot().getWifiSnapshot();
    int apCount = DefensePipeline::snapshot().getWifiSnapshotCount();
    for (int i = 0; i < apCount && dumpLineCount < DUMP_MAX_LINES; i++) {
        if (aps[i].entropyScore > 0 && aps[i].entropyScore < 25) {
            stageLine(LineColor::ALERT, "lowIE:%d \"%.14s\" ch%d",
                      aps[i].entropyScore, aps[i].ssid, aps[i].channel);
            break;  // one warning max
        }
    }
}

// ==[ XBAND DUMP ]== cross-domain WiFi+BLE fusion intelligence

static void dumpXband() {
    int rows = 0;

    // ==[ FEATURE 2: DUAL-BAND STALK ]== highest severity first
    if (DefensePipeline::snapshot().isDualBandStalkActive()) {
        stageLine(LineColor::ALERT, " !! DUAL-BAND TAILING !! ");
        rows++;
        // show the following BLE tracker
        const Recon::TrackerEntry* trackers = DefensePipeline::snapshot().getTrackers();
        int trkCount = DefensePipeline::snapshot().getTrackerTableSize();
        for (int i = 0; i < trkCount && rows < 3; i++) {
            const Recon::TrackerEntry& te = trackers[i];
            if (!(te.flags & Recon::FLAG_FOLLOWING)) continue;
            stageLine(LineColor::ALERT, " BLE: %s %ddB %s",
                      Recon::deviceLabel(te), te.rssiSmooth, Recon::proximityLabel(te.rssiSmooth));
            rows++;
            break;
        }
        // show persistent WiFi client
        int pcCount = DefensePipeline::snapshot().getPersistentClientCount();
        if (pcCount > 0) {
            // find a cohort pair that's following for the WiFi context
            const XBand::CohortPair* pairs = DefensePipeline::snapshot().getCohortPairs();
            int pairCount = DefensePipeline::snapshot().getCohortCount();
            for (int i = 0; i < pairCount && rows < 4; i++) {
                if (!pairs[i].isFollowing) continue;
                stageLine(LineColor::ALERT, " WiFi: %02X:%02X probing %.10s",
                          pairs[i].wifiMac[4], pairs[i].wifiMac[5],
                          pairs[i].probeSSID);
                rows++;
                break;
            }
        }
        // coordinated multi-vector: BLE spam + dual-band stalk
        if (DefensePipeline::snapshot().getSpamCount() > 0 && rows < 5) {
            stageLine(LineColor::ALERT, " +%d spam src COORDINATED",
                      DefensePipeline::snapshot().getSpamCount());
            rows++;
        }
    }

    // ==[ FEATURE 1: ATTACKER FINGERPRINT ]==
    int atkCount = DefensePipeline::snapshot().getAttackerCount();
    const XBand::AttackerProfile* atks = DefensePipeline::snapshot().getAttackerProfiles();
    if (atkCount > 0 && atks) {
        for (int i = 0; i < atkCount && rows < 6; i++) {
            const XBand::AttackerProfile& ap = atks[i];
            uint32_t age = millis() - ap.lastCorrelated;
            if (age > 120000) continue;  // skip stale (>2min)
            const char* mfr = Recon::manufacturerLabel(ap.companyId);
            if (ap.estimatedDist > 0) {
                stageLine(LineColor::ALERT, "[ATK-ID] %s %ddB ~%.1fm",
                          (mfr[0] != '?' ? mfr : (ap.bleName[0] ? ap.bleName : "BLE")),
                          ap.bleRssi, ap.estimatedDist);
            } else {
                stageLine(LineColor::ALERT, "[ATK-ID] %s %ddB",
                          (mfr[0] != '?' ? mfr : (ap.bleName[0] ? ap.bleName : "BLE")),
                          ap.bleRssi);
            }
            rows++;
            if (ap.correlatedBursts > 1 && rows < 7) {
                // show correlation duration instead of hash (NEW)
                uint32_t corrDur = (millis() - ap.firstCorrelated) / 60000;
                stageLine(LineColor::FG, "  %d bursts corr %lum",
                          ap.correlatedBursts, (unsigned long)corrDur);
                rows++;
            }
        }
    }

    // ==[ FEATURE 4: FOLLOWING → NETWORK ]== (from cohort pairs with isFollowing)
    const XBand::CohortPair* pairs = DefensePipeline::snapshot().getCohortPairs();
    int pairCount = DefensePipeline::snapshot().getCohortCount();
    for (int i = 0; i < pairCount && rows < 8; i++) {
        if (!pairs[i].isFollowing) continue;
        // check if BLE device is watchlisted for named identification
        int wlSlot = DefensePipeline::snapshot().findWatchlistSlot(pairs[i].blePayloadHash);
        const char* wlLabel = nullptr;
        if (wlSlot >= 0) {
            const Recon::WatchlistEntry* wl = DefensePipeline::snapshot().getWatchlist();
            if (wl[wlSlot].label[0]) wlLabel = wl[wlSlot].label;
        }
        if (pairs[i].potfileMatch) {
            if (wlLabel) {
                stageLine(LineColor::ALERT, "[FOLLOW] \"%.8s\" -> pot",
                          wlLabel);
            } else {
                stageLine(LineColor::ALERT, "[FOL-NET] \"%.12s\"",
                          pairs[i].probeSSID);
            }
            rows++;
            stageLine(LineColor::ALERT, " !! POTFILE MATCH !!");
            rows++;
        } else if (pairs[i].probeSSID[0]) {
            if (wlLabel) {
                stageLine(LineColor::FG, "[FOLLOW] \"%.8s\" -> %.10s",
                          wlLabel, pairs[i].probeSSID);
            } else {
                stageLine(LineColor::FG, "[FOL-NET] \"%.14s\"",
                          pairs[i].probeSSID);
            }
            rows++;
        }
    }

    // ==[ FEATURE 3: COHORT PAIRS ]== non-following cohorts
    int cohortShown = 0;
    for (int i = 0; i < pairCount && rows < 10; i++) {
        if (pairs[i].isFollowing) continue;
        if (pairs[i].confidence < 2) continue;  // MED+ only
        const char* mfr = Recon::manufacturerLabel(pairs[i].companyId);
        const char* conf = (pairs[i].confidence >= 3) ? "HIGH" : "MED";
        stageLine(LineColor::FG, "[COHORT] %02X:%02X %s",
                  pairs[i].wifiMac[4], pairs[i].wifiMac[5], conf);
        rows++;
        if (pairs[i].probeSSID[0] && rows < 11) {
            stageLine(LineColor::DIM, "  probing \"%.16s\"", pairs[i].probeSSID);
            rows++;
        }
        cohortShown++;
        if (cohortShown >= 3) break;  // max 3 cohort pairs in dump
    }

    // ==[ FEATURE 5: CROWD DENSITY ]==
    const XBand::CrowdSnapshot* crowd = DefensePipeline::snapshot().getCurrentCrowd();
    if (crowd && rows < 12) {
        const char* trendStr = "???";
        switch (DefensePipeline::snapshot().getCrowdTrend()) {
            case XBand::CrowdTrend::GROWING:   trendStr = "GROWING"; break;
            case XBand::CrowdTrend::STABLE:    trendStr = "STABLE"; break;
            case XBand::CrowdTrend::SHRINKING: trendStr = "SHRINK"; break;
            default:                           trendStr = "---"; break;
        }
        const char* tierStr = "?";
        switch (DefensePipeline::snapshot().getCrowdTier()) {
            case XBand::CrowdTier::DESERTED: tierStr = "DESRT"; break;
            case XBand::CrowdTier::SPARSE:   tierStr = "SPRS";  break;
            case XBand::CrowdTier::BUSY:     tierStr = "BUSY";  break;
            case XBand::CrowdTier::CROWDED:  tierStr = "CRWD";  break;
            case XBand::CrowdTier::PACKED:   tierStr = "PACK";  break;
        }
        stageLine(LineColor::DIM, "%s ~%d %dble %dwf %s",
                  tierStr, crowd->estimatedPop,
                  crowd->blePhones + crowd->bleWatches + crowd->bleOther,
                  crowd->wifiClients, trendStr);
        rows++;
        if (rows < 13 && (crowd->blePhones || crowd->bleWatches || crowd->bleTags)) {
            stageLine(LineColor::DIM, "  %dph %dwa %dtag %dsns",
                      crowd->blePhones, crowd->bleWatches,
                      crowd->bleTags, crowd->bleSensors);
            rows++;
        }
    }

    // ==[ FEATURE 6: VENDOR CROSS-REF ]==
    int vcCount = DefensePipeline::snapshot().getVendorCorrelationCount();
    const XBand::VendorCorrelation* vcs = DefensePipeline::snapshot().getVendorCorrelations();
    if (vcCount > 0 && vcs && rows < 14) {
        for (int i = 0; i < vcCount && rows < 14; i++) {
            const XBand::VendorCorrelation& vc = vcs[i];
            if (vc.bleDeviceCount < 2) continue;  // skip singletons
            const char* mfr = Recon::manufacturerLabel(vc.companyId);
            if (mfr[0] == '?') continue;
            if (vc.wifiAPCount > 0) {
                stageLine(LineColor::DIM, "[ECO] %s: %dble %dap",
                          mfr, vc.bleDeviceCount, vc.wifiAPCount);
            } else if (vc.hasIoT) {
                stageLine(LineColor::FG, "[IoT] %s: %d devs nearby",
                          mfr, vc.bleDeviceCount);
            }
            rows++;
        }
    }

    // fallback if nothing to show
    if (rows == 0) {
        stageLine(LineColor::DIM, "xband: no cross-domain intel");
    }
}

// ==[ FORENSIC TIMELINE DUMP ]== chronological event log (NEW)

static void dumpForensicTimeline() {
    const Recon::ForensicLogEntry* flog = DefensePipeline::snapshot().getForensicLog();
    uint8_t fcount = DefensePipeline::snapshot().getForensicLogCount();
    uint8_t fhead = DefensePipeline::snapshot().getForensicLogHead();

    if (!flog || fcount == 0) {
        stageLine(LineColor::DIM, "no forensic events");
        return;
    }

    uint32_t now = millis();
    const DetectedNetwork* nets = Hunt::getNetworks();
    int netCount = Hunt::getNetworkCount();

    // show recent events (last 30min), newest first
    int shown = 0;
    for (uint8_t i = 0; i < fcount && shown < 7; i++) {
        int idx = ((int)fhead - 1 - i + Recon::MAX_FORENSIC_LOG) % Recon::MAX_FORENSIC_LOG;
        const Recon::ForensicLogEntry& e = flog[idx];
        if (now - e.timestamp > 1800000) continue;

        char ts[12];
        fmtUptime(ts, sizeof(ts), e.timestamp);
        const char* tag = eventTag(e.event);

        // look up SSID from Hunt networks if BSSID present
        const char* ssid = nullptr;
        bool hasBssid = false;
        for (int k = 0; k < 6; k++) { if (e.bssid[k]) { hasBssid = true; break; } }
        if (hasBssid && nets) {
            for (int n = 0; n < netCount; n++) {
                if (memcmp(nets[n].bssid, e.bssid, 6) == 0 && nets[n].ssid[0]) {
                    ssid = nets[n].ssid;
                    break;
                }
            }
        }

        int rpt = (int)e._pad[0];  // dedup repeat count (0 = single)
        // for M1 fp-mismatch, decode which IE category flipped (rates/rsn/vnd/htc)
        // from the matched hunt network so the event row names the cause
        const char* fpSub = "";
        if (e.event == Recon::ReconEvent::FINGERPRINT_MISMATCH && nets) {
            for (int n = 0; n < netCount; n++) {
                if (memcmp(nets[n].bssid, e.bssid, 6) == 0) {
                    uint8_t m = nets[n].ieChangeMask;
                    if      (m & 0x02) fpSub = " rsn";
                    else if (m & 0x01) fpSub = " rate";
                    else if (m & 0x08) fpSub = " htc";
                    else if (m & 0x04) fpSub = " vnd";
                    break;
                }
            }
        }
        if (ssid) {
            if (rpt > 0)
                stageLine(LineColor::FG, "[%s] %s%s ch%02d \"%.5s\" x%d",
                          ts, tag, fpSub, e.channel, ssid, rpt + 1);
            else
                stageLine(LineColor::FG, "[%s] %s%s ch%02d \"%.8s\"",
                          ts, tag, fpSub, e.channel, ssid);
        } else if (e.event == Recon::ReconEvent::SEQ_ANOMALY) {
            if (rpt > 0)
                stageLine(LineColor::FG, "[%s] %s ch%02d gap x%d",
                          ts, tag, e.channel, rpt + 1);
            else
                stageLine(LineColor::FG, "[%s] %s ch%02d gap",
                          ts, tag, e.channel);
        } else {
            if (rpt > 0)
                stageLine(LineColor::FG, "[%s] %s%s ch%02d %ddB x%d",
                          ts, tag, fpSub, e.channel, e.rssi, rpt + 1);
            else
                stageLine(LineColor::FG, "[%s] %s%s ch%02d %ddB",
                          ts, tag, fpSub, e.channel, e.rssi);
        }
        shown++;

        // decode indicator flags (multi-vector convergence context)
        if (e.indicatorFlags && shown < 7) {
            char flags[LINE_LEN] = {0};
            int fp = 0;
            if (e.indicatorFlags & 0x01) fp += snprintf(flags+fp, LINE_LEN-fp, "FP+");
            if (e.indicatorFlags & 0x02) fp += snprintf(flags+fp, LINE_LEN-fp, "SEQ+");
            if (e.indicatorFlags & 0x04) fp += snprintf(flags+fp, LINE_LEN-fp, "RSI+");
            if (e.indicatorFlags & 0x08) fp += snprintf(flags+fp, LINE_LEN-fp, "TWN+");
            if (e.indicatorFlags & 0x10) fp += snprintf(flags+fp, LINE_LEN-fp, "KRM+");
            if (e.indicatorFlags & 0x20) fp += snprintf(flags+fp, LINE_LEN-fp, "DTH+");
            if (e.indicatorFlags & 0x40) fp += snprintf(flags+fp, LINE_LEN-fp, "BLE");
            if (fp > 0 && flags[fp-1] == '+') flags[fp-1] = '\0'; // trim trailing +
            stageLine(LineColor::DIM, "  [%s]", flags);
            shown++;
        }
    }

    // indicator convergence: find BSSID with most distinct indicators
    if (fcount >= 2) {
        uint8_t bestInd = 0;
        uint8_t bestBssidIdx = 0;
        if (nets) {
            for (int n = 0; n < netCount; n++) {
                uint8_t ind = DefensePipeline::snapshot().countIndicatorsForBSSID(nets[n].bssid, 600000);
                if (ind > bestInd) { bestInd = ind; bestBssidIdx = n; }
            }
        }
        if (bestInd >= 2 && nets) {
            const char* conf = bestInd >= 3 ? "HIGH" : "MED";
            stageLine(bestInd >= 3 ? LineColor::ALERT : LineColor::FG,
                      "chain: %dind %s \"%.*s\"",
                      bestInd, conf, 14, nets[bestBssidIdx].ssid);
        }
    }
}

// ==[ HOSTILE CLIENTS ]== passive probe fingerprinting intelligence
static void dumpHostileClients() {
    uint8_t fpCount = DefensePipeline::snapshot().getClientFingerprintCount();
    const Recon::ClientFingerprint* fps = fpCount > 0 ? DefensePipeline::snapshot().getClientFingerprints() : nullptr;

    if (fpCount == 0 || !fps) {
        stageLine(LineColor::DIM, "no hostile clients profiled");
        return;
    }

    stageLine(LineColor::FG, "%d clients fingerprinted", fpCount);

    // sort by hostile score (display highest first) — inline selection
    uint8_t order[16];
    for (uint8_t i = 0; i < fpCount && i < 16; i++) order[i] = i;
    for (uint8_t i = 0; i < fpCount - 1 && i < 15; i++) {
        for (uint8_t j = i + 1; j < fpCount && j < 16; j++) {
            if (fps[order[j]].hostileScore > fps[order[i]].hostileScore) {
                uint8_t tmp = order[i]; order[i] = order[j]; order[j] = tmp;
            }
        }
    }

    int shown = 0;
    for (uint8_t i = 0; i < fpCount && shown < 6; i++) {
        const Recon::ClientFingerprint& f = fps[order[i]];
        const char* mac = "";
        char macBuf[10];
        snprintf(macBuf, sizeof(macBuf), "%02X:%02X:%02X",
                 f.clientMac[3], f.clientMac[4], f.clientMac[5]);
        mac = macBuf;

        LineColor c = (f.hostileScore >= 60) ? LineColor::ALERT : LineColor::FG;
        if (f.toolSignature != (uint8_t)Recon::DeauthTool::UNKNOWN) {
            stageLine(c, "%s H:%d %s [%s]",
                      mac, f.hostileScore, f.label,
                      Recon::deauthToolLabel((Recon::DeauthTool)f.toolSignature));
        } else {
            stageLine(c, "%s H:%d ch%d %s",
                      mac, f.hostileScore, f.channel, f.label);
        }
        shown++;
    }

    // canary + KARMA status
    if (DefensePipeline::snapshot().isCanaryTripped()) {
        stageLine(LineColor::ALERT, "!! CANARY TRIPPED: %s",
                  DefensePipeline::snapshot().getCanarySSID());
    }
    if (DefensePipeline::snapshot().isKarmaConfirmed()) {
        stageLine(LineColor::ALERT, "!! KARMA CONFIRMED");
    }
}

// ==[ TEMPORAL HEATMAP ]== threat intensity over time
static void dumpThreatHeatmap() {
    Recon::HeatmapBucket buckets[10];
    DefensePipeline::snapshot().getTemporalHeatmap(buckets, 10, 300000);  // 5 min window, 10 buckets (30s each)

    // check if any activity
    bool anyActivity = false;
    for (int i = 0; i < 10; i++) {
        if (buckets[i].deauthIntensity || buckets[i].bleIntensity || buckets[i].wifiIntensity) {
            anyActivity = true;
            break;
        }
    }
    if (!anyActivity) {
        stageLine(LineColor::DIM, "no threat activity (5min)");
        return;
    }

    stageLine(LineColor::FG, "threat heatmap (5min)");

    // render sparkline rows with intensity->char mapping
    char dthBar[12], wfiBar[12], bleBar[12];
    for (int i = 0; i < 10; i++) {
        auto toChar = [](uint8_t v) -> char {
            if (v == 0) return '.';
            if (v < 30) return '_';
            if (v < 80) return 'o';
            if (v < 150) return 'O';
            return '#';
        };
        dthBar[i] = toChar(buckets[i].deauthIntensity);
        wfiBar[i] = toChar(buckets[i].wifiIntensity);
        bleBar[i] = toChar(buckets[i].bleIntensity);
    }
    dthBar[10] = wfiBar[10] = bleBar[10] = '\0';

    stageLine(LineColor::FG,    " dth [%s]", dthBar);
    stageLine(LineColor::FG,    " wfi [%s]", wfiBar);
    stageLine(LineColor::FG,    " ble [%s]", bleBar);
    stageLine(LineColor::DIM,   "       old>>>new  30s/col");

    // flag summary: which indicators were active during the window
    uint8_t allFlags = 0;
    for (int i = 0; i < 10; i++) allFlags |= buckets[i].indicatorFlags;
    if (allFlags) {
        char flags[LINE_LEN] = {0};
        int fp = 0;
        if (allFlags & 0x01) fp += snprintf(flags+fp, LINE_LEN-fp, "FP ");
        if (allFlags & 0x02) fp += snprintf(flags+fp, LINE_LEN-fp, "SEQ ");
        if (allFlags & 0x04) fp += snprintf(flags+fp, LINE_LEN-fp, "RSI ");
        if (allFlags & 0x08) fp += snprintf(flags+fp, LINE_LEN-fp, "TWN ");
        if (allFlags & 0x10) fp += snprintf(flags+fp, LINE_LEN-fp, "KRM ");
        if (allFlags & 0x20) fp += snprintf(flags+fp, LINE_LEN-fp, "DTH ");
        if (allFlags & 0x40) fp += snprintf(flags+fp, LINE_LEN-fp, "BLE ");
        stageLine(LineColor::DIM, "flags: %s", flags);
    }
}

// ==[ ATTACK TIMELINE ]== reconstructed chronological attack narrative
static void dumpAttackTimeline() {
    // combine deauth burst history + forensic log into unified timeline
    uint8_t burstN = DefensePipeline::snapshot().getDeauthBurstHistoryCount();
    const Recon::DeauthBurstRecord* bursts = burstN > 0 ? DefensePipeline::snapshot().getDeauthBurstHistory() : nullptr;
    const Recon::ForensicLogEntry* flog = DefensePipeline::snapshot().getForensicLog();
    uint8_t fcount = DefensePipeline::snapshot().getForensicLogCount();
    uint8_t fhead = DefensePipeline::snapshot().getForensicLogHead();

    if ((burstN == 0 || !bursts) && fcount < 2) {
        stageLine(LineColor::DIM, "insufficient attack data");
        return;
    }

    stageLine(LineColor::FG, "-- attack timeline --");

    // merge events into a simple timeline (max 8 entries)
    struct TimeEvent {
        uint32_t ts;
        char desc[LINE_LEN];
        LineColor color;
    };
    TimeEvent events[8];
    int evCount = 0;

    // add deauth bursts
    if (bursts) {
        for (uint8_t i = 0; i < burstN && evCount < 8; i++) {
            TimeEvent& e = events[evCount];
            e.ts = bursts[i].timestamp;
            e.color = LineColor::FG;
            Recon::DeauthTool dtool = DefensePipeline::snapshot().getLastDeauthTool();
            if (dtool != Recon::DeauthTool::UNKNOWN && i == 0) {
                snprintf(e.desc, LINE_LEN, "DTH %dpps ch%d %s",
                         bursts[i].pps, bursts[i].dominantChannel,
                         Recon::deauthToolLabel(dtool));
            } else {
                snprintf(e.desc, LINE_LEN, "DTH %dpps %dsrc ch%d",
                         bursts[i].pps, bursts[i].uniqueSources,
                         bursts[i].dominantChannel);
            }
            evCount++;
        }
    }

    // add forensic log events (security-relevant only, not SCAN_COMPLETE)
    if (flog) {
        for (uint8_t i = 0; i < fcount && evCount < 8; i++) {
            int idx = ((int)fhead - 1 - i + Recon::MAX_FORENSIC_LOG) % Recon::MAX_FORENSIC_LOG;
            const Recon::ForensicLogEntry& fe = flog[idx];
            // skip scan-complete and duplicate deauth (already covered by burst history)
            if (fe.event == Recon::ReconEvent::SCAN_COMPLETE) continue;
            if (fe.event == Recon::ReconEvent::DEAUTH_DETECTED) continue;
            TimeEvent& e = events[evCount];
            e.ts = fe.timestamp;
            e.color = LineColor::FG;
            const char* tag = eventTag(fe.event);
            if (fe.channel > 0) {
                snprintf(e.desc, LINE_LEN, "%s ch%d %ddB", tag, fe.channel, fe.rssi);
            } else {
                snprintf(e.desc, LINE_LEN, "%s %ddB", tag, fe.rssi);
            }
            evCount++;
        }
    }

    // sort by timestamp (newest first) — bubble sort, tiny array
    for (int i = 0; i < evCount - 1; i++) {
        for (int j = i + 1; j < evCount; j++) {
            if (events[j].ts > events[i].ts) {
                TimeEvent tmp = events[i]; events[i] = events[j]; events[j] = tmp;
            }
        }
    }

    // display (max 9 entries to fit in 12 lines with header)
    int shown = 0;
    for (int i = 0; i < evCount && shown < 9; i++) {
        char ts[12];
        fmtUptime(ts, sizeof(ts), events[i].ts);
        stageLine(events[i].color, "[%s] %s", ts, events[i].desc);
        shown++;
    }

    // composite threat assessment
    bool dth = DefensePipeline::snapshot().isDeauthActive();
    bool twn = DefensePipeline::snapshot().isEvilTwinActive();
    bool krm = DefensePipeline::snapshot().isKarmaActive();
    int activeThreats = (int)dth + (int)twn + (int)krm;
    if (activeThreats >= 2) {
        stageLine(LineColor::ALERT, "MULTI-VECTOR ACTIVE (%d)", activeThreats);
    } else if (activeThreats == 1) {
        stageLine(LineColor::FG, "single threat active");
    }
}

// ==[ HOGWASH DUMP ]== fake handshake injection stats
static void dumpHogwash() {
    uint32_t bursts = WifiChaff::getTotalBursts();
    uint32_t fakes = WifiChaff::getTotalInjections();
    bool enabled = Config::getWifiChaffEnabled();

    stageLine(LineColor::FG, "==[ HOGWASH ]==");
    stageLine(LineColor::DIM, "WiFi handshake chaff engine");
    stageLine(LineColor::FG, "status: %s", enabled ? "ARMED" : "DISARMED");
    stageLine(LineColor::FG, "bursts: %lu  fakes: %lu",
                (unsigned long)bursts, (unsigned long)fakes);

    const uint8_t* lastBssid = WifiChaff::getLastTargetBSSID();
    if (lastBssid) {
        stageLine(LineColor::FG, "last: %02X:%02X:%02X:%02X:%02X:%02X",
                    lastBssid[0], lastBssid[1], lastBssid[2],
                    lastBssid[3], lastBssid[4], lastBssid[5]);
        stageLine(LineColor::DIM, " ch%d", WifiChaff::getLastChannel());
    }

    if (bursts > 0) {
        stageLine(LineColor::ALERT, "GPU cycles wasted: yes");
    } else {
        stageLine(LineColor::DIM, "no deauth detected yet");
    }
    stageLine(LineColor::DIM, "M1+M2 per burst: 4 fake STAs");
    stageLine(LineColor::DIM, "MIC: /dev/urandom. uncrackable.");
}

// ==[ DUMP COMMAND STRINGS ]==

static const char* dumpCommand(DumpType t) {
    switch (t) {
        case DumpType::SITREP:     return " sitrep";
        case DumpType::YIELD:      return " ducb --yield";
        case DumpType::STASH:      return " stash --intel";
        case DumpType::CLIENTS:    return " harvest --probes";
        case DumpType::RF_POSTURE: return " rf --posture";
        case DumpType::BLE_TRACKERS: return " ble --trackers";
        case DumpType::XBAND:        return " xband --fusion";
        case DumpType::FORENSIC_TIMELINE: return " forensic --timeline";
        case DumpType::HOGWASH:            return " hogwash --status";
        case DumpType::HOSTILE_CLIENTS:    return " recon --clients";
        case DumpType::THREAT_HEATMAP:     return " recon --heatmap";
        case DumpType::ATTACK_TIMELINE:    return " recon --timeline";
        default: return " status";
    }
}

// ==[ DUMP SELECTION ]== weighted random, SITREP heavier when threat > GREEN

static DumpType selectDumpType() {
    struct Candidate { DumpType type; uint8_t weight; };
    Candidate candidates[16];
    int numCandidates = 0;

    auto addCandidate = [&](DumpType t, uint8_t w) {
        if (t == lastDumpType) return;
        if (w == 0) return;
        if (numCandidates < 14) {
            candidates[numCandidates++] = { t, w };
        }
    };

    // all dumps always available — APIs return persisted data from last session
    // SITREP gets heavier weight when threats are active
    uint8_t sitrepWeight = (threatLevel >= 2) ? 4 : 2;
    addCandidate(DumpType::SITREP, sitrepWeight);
    addCandidate(DumpType::STASH, 2);
    addCandidate(DumpType::YIELD, 2);
    addCandidate(DumpType::CLIENTS, 2);
    addCandidate(DumpType::RF_POSTURE, 2);
    uint8_t bleWeight = (DefensePipeline::snapshot().getFollowingCount() > 0 || DefensePipeline::snapshot().getSpamCount() > 0) ? 4 : 2;
    addCandidate(DumpType::BLE_TRACKERS, bleWeight);
    uint8_t xbandWeight = DefensePipeline::snapshot().hasCriticalIntel() ? 6 :
                           DefensePipeline::snapshot().hasActiveIntel() ? 4 : 2;
    addCandidate(DumpType::XBAND, xbandWeight);
    // FORENSIC_TIMELINE: heavier when multiple forensic events recorded
    uint8_t ftmlWeight = (DefensePipeline::snapshot().getForensicLogCount() >= 3) ? 3 : 1;
    addCandidate(DumpType::FORENSIC_TIMELINE, ftmlWeight);
    // HOGWASH: only show when chaff has fired this session
    uint8_t hogwashWeight = (WifiChaff::getTotalBursts() > 0) ? 3 : 0;
    addCandidate(DumpType::HOGWASH, hogwashWeight);
    // hostile clients: only when fingerprints exist
    uint8_t hclWeight = (DefensePipeline::snapshot().getClientFingerprintCount() > 0) ? 3 : 0;
    addCandidate(DumpType::HOSTILE_CLIENTS, hclWeight);
    // threat heatmap: always available but heavier when forensic log has data
    uint8_t heatWeight = (DefensePipeline::snapshot().getForensicLogCount() >= 3) ? 3 : 1;
    addCandidate(DumpType::THREAT_HEATMAP, heatWeight);
    // attack timeline: only when burst history exists
    uint8_t atkWeight = (DefensePipeline::snapshot().getDeauthBurstHistoryCount() > 0 || DefensePipeline::snapshot().getForensicLogCount() >= 5) ? 4 : 0;
    addCandidate(DumpType::ATTACK_TIMELINE, atkWeight);

    if (numCandidates == 0) {
        return DumpType::SITREP;
    }

    // weighted random
    int totalWeight = 0;
    for (int i = 0; i < numCandidates; i++) totalWeight += candidates[i].weight;
    int roll = esp_random() % totalWeight;
    int acc = 0;
    for (int i = 0; i < numCandidates; i++) {
        acc += candidates[i].weight;
        if (roll < acc) return candidates[i].type;
    }
    return candidates[0].type;
}

static void generateDump(DumpType type) {
    dumpLineCount = 0;
    dumpLinePushed = 0;
    dumpCommandPushed = false;

    switch (type) {
        case DumpType::SITREP:     dumpSitrep();    break;
        case DumpType::YIELD:      dumpYield();      break;
        case DumpType::STASH:      dumpStash();      break;
        case DumpType::CLIENTS:    dumpClients();    break;
        case DumpType::RF_POSTURE: dumpRfPosture();  break;
        case DumpType::BLE_TRACKERS: dumpBleTrackers(); break;
        case DumpType::XBAND:        dumpXband();       break;
        case DumpType::FORENSIC_TIMELINE: dumpForensicTimeline(); break;
        case DumpType::HOGWASH:            dumpHogwash();          break;
        case DumpType::HOSTILE_CLIENTS:  dumpHostileClients();  break;
        case DumpType::THREAT_HEATMAP:   dumpThreatHeatmap();   break;
        case DumpType::ATTACK_TIMELINE:  dumpAttackTimeline();  break;
        default: break;
    }
}

// ==[ BOOT SEQUENCE — SYSTEMD BRACKETS ]==

static void pushBootLine(uint8_t idx, uint8_t station) {
    switch (idx) {
        case 0: pushLineRaw(LineColor::FG, "[  OK  ] rf subsystem"); break;
        case 1: pushLineRaw(LineColor::FG, "[  OK  ] passive monitor"); break;
        case 2:
            if (DefensePipeline::snapshot().isBleAvailable()) {
                pushLineRaw(LineColor::FG, "[  OK  ] ble scanner");
            } else {
                pushLineRaw(LineColor::DIM, "[ --   ] ble scanner");
            }
            break;
        case 3: pushLineRaw(LineColor::FG, "[  OK  ] wids engine"); break;
        case 4: pushLineRaw(LineColor::DIM, "[ >>>  ] scanning..."); break;
        case 5: pushLineRaw(LineColor::FG, devicePrompt()); break;
    }
}

static void startBootSequence() {
    bootLineIdx = 0;
    bootSequenceActive = true;
    bootLineStartTime = 0;
}

// ==[ ACTIVE GEOMETRY HELPERS ]==
static int termX()     { return kTermX; }
static int termY()     { return kTermY; }
static int termW()     { return kTermW; }
static int termH()     { return kTermH; }
static int textX()     { return kTermTextX; }
static int textW()     { return kTermTextW; }
static int textAreaY() { return kTextAreaY; }
static int visLines()  { return kTermVisibleLines; }

// ==[ DISSOLVE SCATTER PARTICLES ]==

static void spawnScatterParticles() {
    int tx = termX(), ty = termY(), tw = termW(), th = termH();
    particleCount = MAX_PARTICLES;
    for (int i = 0; i < particleCount; i++) {
        ScatterParticle& p = particles[i];
        bool fromEdge = (esp_random() & 1);
        if (fromEdge) {
            p.x = tx + (esp_random() % tw);
            p.y = (esp_random() & 1) ? ty : (ty + th);
        } else {
            p.x = (esp_random() & 1) ? tx : (tx + tw);
            p.y = ty + (esp_random() % th);
        }
        p.vx = (int8_t)((int)(esp_random() % 7) - 3);
        p.vy = (int8_t)((int)(esp_random() % 5) - 2);
        p.life = 8 + (esp_random() % 8);
    }
}

static void updateParticles() {
    for (int i = 0; i < particleCount; i++) {
        ScatterParticle& p = particles[i];
        if (p.life == 0) continue;
        p.x += p.vx;
        p.y += p.vy;
        p.life--;
    }
}

// ==[ DRAWING HELPERS ]==


static void drawHeader(M5Canvas& canvas) {
    int tx = textX();
    int tw = textW();
    int hdrY = termY() + kTermBorder + kTermPadT;

    // panel bg behind header — opencode style
    canvas.fillRect(termX() + 1, termY() + 1, termW() - 2, kCharHSize1 + 4, TERM_BG_P);

    canvas.setTextSize(1);
    canvas.setTextDatum(TL_DATUM);

    // opencode-style two-tone: "DEF" muted, "HOG4" bright
    canvas.setTextColor(TERM_DIM_C);
    canvas.drawString("DEF", tx, hdrY);
    canvas.setTextColor(TERM_FG_C);
    canvas.drawString("HOG4", tx + 3 * kCharWSize1, hdrY);

    // version right-aligned in DIM
    char verBuf[24];
    snprintf(verBuf, sizeof(verBuf), "v%s", BUILD_VERSION);
    int verW = strlen(verBuf) * kCharWSize1;
    canvas.setTextColor(TERM_DIM_C);
    canvas.drawString(verBuf, tx + tw - verW, hdrY);

    // no separator — whitespace gap between header and content (opencode style)
}

static void drawTermBorder(M5Canvas& canvas) {
    canvas.drawRect(termX(), termY(), termW(), termH(), TERM_BD);
}


// ==[ SHARED LINE DRAWING ]==

static void drawTermLines(M5Canvas& canvas) {
    canvas.setTextSize(1);
    canvas.setTextDatum(TL_DATUM);

    int tx = textX();
    int tw = textW();
    int startIdx = (lineCount < MAX_LINES) ? 0 : writeHead;
    int vl = min((int)lineCount, visLines());
    int firstVisible = lineCount - vl;
    int ty = textAreaY();
    bool alertBlink = ((millis() / 400) & 1) == 0;

    for (int i = 0; i < vl; i++) {
        int bufIdx = (startIdx + firstVisible + i) % MAX_LINES;
        const TermLine& line = lines[bufIdx];

        switch (line.color) {
            case LineColor::ALERT: {
                // opencode-style: accent orange left bar + orange text
                // blink: text toggles between accent and dim
                canvas.fillRect(tx, ty + 1, 2, kCharHSize1, TERM_ACC);
                canvas.setTextColor(alertBlink ? TERM_ACC : TERM_DIM_C);
                canvas.drawString(line.text, tx + 4, ty);
                break;
            }

            case LineColor::HYPE:
                canvas.setTextColor(TERM_FG_C);
                canvas.drawString(line.text, tx, ty);
                break;

            case LineColor::DIM:
                canvas.setTextColor(TERM_DIM_C);
                canvas.drawString(line.text, tx, ty);
                break;

            case LineColor::BAR: {
                // primary blue accent bar — 2px tall, centered in line slot
                int barY = ty + (kLineH - 2) / 2;
                canvas.fillRect(tx, barY, tw, 2, TERM_PRI);
                break;
            }

            default: // FG — bright text
                canvas.setTextColor(TERM_FG_C);
                canvas.drawString(line.text, tx, ty);
                break;
        }
        ty += kLineH;
    }

    // fat block cursor — primary blue, 500ms blink
    if (cursorVisible && ty < termY() + termH() - kTermPadB - kCharHSize1) {
        canvas.fillRect(tx, ty, kCharWSize1, kCharHSize1, TERM_PRI);
    }
}

// ==[ MATERIALIZE ANIMATION ]==

static void drawMaterialize(M5Canvas& canvas, uint32_t elapsed) {
    int tX = termX(), tY = termY(), tW = termW(), tH = termH();
    uint32_t scatterEnd = kMatScatterEnd;
    uint32_t convergeEnd = kMatConvergeEnd;
    uint32_t logoEnd = kMatLogoEnd;
    uint32_t bootEnd = kMatBootEnd;

    if (elapsed < scatterEnd) {
        drawProceduralParticles(canvas, elapsed);

    } else if (elapsed < convergeEnd) {
        float convergeT = (float)(elapsed - scatterEnd) /
                          (float)(convergeEnd - scatterEnd);
        int w = (int)(convergeT * tW);
        int h = (int)(convergeT * tH);
        canvas.drawFastHLine(tX, tY, w, TERM_PRI);
        canvas.drawFastHLine(tX + tW - w, tY + tH - 1, w, TERM_PRI);
        canvas.drawFastVLine(tX, tY, h, TERM_PRI);
        canvas.drawFastVLine(tX + tW - 1, tY + tH - h, h, TERM_PRI);
        int fillH = (int)(convergeT * (tH - 2));
        if (fillH > 0 && w > 4) {
            canvas.fillRect(tX + 1, tY + 1, min(w - 2, tW - 2), fillH, TERM_BG);
        }
        drawProceduralParticles(canvas, elapsed);

    } else if (elapsed < logoEnd) {
        canvas.fillRect(tX, tY, tW, tH, TERM_BG);
        drawTermBorder(canvas);
        float logoT = (float)(elapsed - convergeEnd) / (float)(logoEnd - convergeEnd);

        // two-tone logo: "DEF" muted, "HOG4" bright — opencode style
        int kbW = 7 * kCharWSize2;
        int kbX = tX + (tW - kbW) / 2;
        int kbY = kLogoCenterY;
        int ditherThresh = (int)(logoT * 4.0f);
        if (ditherThresh > 0) {
            canvas.setTextSize(2);
            canvas.setTextColor(TERM_DIM_C);
            canvas.drawString("DEF", kbX, kbY);
            canvas.setTextColor(TERM_FG_C);
            canvas.drawString("HOG4", kbX + 3 * kCharWSize2, kbY);
        }
        if (logoT > 0.5f) {
            char verBuf[LINE_LEN];
            snprintf(verBuf, sizeof(verBuf), "v%s-%s", BUILD_VERSION, BUILD_COMMIT);
            int verLen = strlen(verBuf);
            int verW = verLen * kCharWSize1;
            int verX = tX + (tW - verW) / 2;
            int verY = kbY + kCharHSize2 + 6;
            canvas.setTextColor(TERM_DIM_C);
            canvas.setTextSize(1);
            canvas.drawString(verBuf, verX, verY);
        }

    } else {
        canvas.fillRect(tX, tY, tW, tH, TERM_BG);
        drawTermBorder(canvas);

        // DEFHOG4: logo slides to header, boot lines
        float bootT = (float)(elapsed - logoEnd) / (float)(bootEnd - logoEnd);
        if (bootT > 1.0f) bootT = 1.0f;

        if (bootT < 0.5f) {
            float slideT = bootT / 0.5f;
            int startY = kLogoCenterY;
            int endY = kHeaderTextY;
            int curY = startY + (int)((float)(endY - startY) * slideT);

            if (slideT < 0.6f) {
                // "DEFHOG4" two-tone slides from center to header position
                int dW = 7 * kCharWSize2;
                int dX = tX + (tW - dW) / 2;
                int endX = kTermTextX;
                int curDX = dX + (int)((float)(endX - dX) * slideT);
                canvas.setTextSize(2);
                canvas.setTextColor(TERM_DIM_C);
                canvas.drawString("DEF", curDX, curY);
                canvas.setTextColor(TERM_FG_C);
                canvas.drawString("HOG4", curDX + 3 * kCharWSize2, curY);
            } else {
                // snap to clean opencode-style header
                drawHeader(canvas);
            }
        } else {
            drawHeader(canvas);
            drawTermLines(canvas);
        }
    }
}

// ==[ ACTIVE STATE DRAWING ]==

static void drawActive(M5Canvas& canvas) {
    canvas.fillRect(termX(), termY(), termW(), termH(), TERM_BG);
    drawTermBorder(canvas);
    drawHeader(canvas);
    drawTermLines(canvas);

    // flash effect — brief primary blue wash after materialize
    if (flashStart > 0) {
        uint32_t flashElapsed = millis() - flashStart;
        if (flashElapsed < FLASH_DURATION_MS) {
            canvas.fillRect(termX() + 1, termY() + 1, termW() - 2, termH() - 2, TERM_PRI);
        } else {
            flashStart = 0;
        }
    }
}

// ==[ DISSOLVE ANIMATION ]==

static void drawDissolve(M5Canvas& canvas, float t) {
    // dissolve uses snapshot geometry from when hide() was called
    // (pigSyncMode already cleared, but spawnScatterParticles captured positions)
    if (t < 0.33f) {
        drawActive(canvas);

        float gt = t / 0.33f;
        int glitchLines = (int)(gt * 10);
        int tX = termX(), tY = termY(), tH = termH();
        for (int i = 0; i < glitchLines; i++) {
            uint32_t h = MenuPigRender::wallHash(i, millis() / 50, 0xDEAD);
            int ly = tY + (h % tH);
            int offset = (int)(h >> 8) % 5 - 2;
            if (offset > 0) {
                canvas.fillRect(tX, ly, offset, 1, TERM_BG);
            }
            if ((h >> 16) & 1) {
                static const char garble[] = "#%@&*!~$^";
                char g = garble[(h >> 20) % 9];
                int gx = textX() + ((h >> 12) % 28) * kCharWSize1;
                canvas.setTextColor(TERM_PRI);  // blue glitch chars
                char buf[2] = { g, 0 };
                canvas.drawString(buf, gx, ly);
            }
        }
    } else if (t < 0.83f) {
        float dt = (t - 0.33f) / 0.5f;
        int tX = termX(), tY = termY(), tW = termW(), tH = termH();
        int cx = tX + tW / 2;
        int cy = tY + tH / 2;
        int shrinkW = (int)(tW * (1.0f - dt));
        int shrinkH = (int)(tH * (1.0f - dt));
        int sx = cx - shrinkW / 2;
        int sy = cy - shrinkH / 2;
        if (shrinkW > 0 && shrinkH > 0) {
            canvas.fillRect(sx, sy, shrinkW, shrinkH, TERM_BG);
            canvas.drawRect(sx, sy, shrinkW, shrinkH, TERM_BD);
        }
    }

    if (t > 0.67f) {
        for (int i = 0; i < particleCount; i++) {
            const ScatterParticle& p = particles[i];
            if (p.life == 0) continue;
            if ((p.life & 1) == 0) continue;
            int px = p.x;
            int py = p.y;
            if (px >= 0 && px < kScreenWidth - 1 && py >= 0 && py < kScreenHeight - 1) {
                canvas.fillRect(px, py, 2, 2, TERM_PRI);  // blue particles
            }
        }
    }
}

// ==[ PUBLIC API ]==

void init() {
    reset();
}

void reset() {
    writeHead = 0;
    lineCount = 0;
    state = State::HIDDEN;
    threatLevel = 0;
    lastThreatLevel = 0;
    threatDowngradeSince = 0;
    particleCount = 0;
    flashStart = 0;

    lastCommentaryTime = 0;
    lastQuietTime = 0;
    lastElapsedTime = 0;
    lastEventTime = 0;
    terminalStartTime = 0;
    lineRevealing = false;
    bootSequenceActive = false;
    dumpPhase = DumpPhase::NONE;
    dumpLineCount = 0;
    dumpLinePushed = 0;
    currentDump = DumpType::COUNT;
}

void pushLine(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    pushLineInternal(LineColor::FG, fmt, args);
    va_end(args);
}

void pushLineHype(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    pushLineInternal(LineColor::HYPE, fmt, args);
    va_end(args);
}

void pushLineDim(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    pushLineInternal(LineColor::DIM, fmt, args);
    va_end(args);
}

void pushLineAlert(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    pushLineInternal(LineColor::ALERT, fmt, args);
    va_end(args);
}

void pushLineBar() {
    pushLineRaw(LineColor::BAR, "");
}

void show(uint8_t station) {
    if (state == State::MATERIALIZING || state == State::ACTIVE) return;

    writeHead = 0;
    lineCount = 0;

    state = State::MATERIALIZING;
    animStart = millis();
    showStation = station;
    switch (station) {
        case 0: showRoom = 0; break;
        case 1: showRoom = 1; break;
        case 5: showRoom = 3; break;
        default: showRoom = 0; break;
    }

    terminalStartTime = millis();
    lastCommentaryTime = millis();
    lastQuietTime = millis();
    lastElapsedTime = millis();
    lastEventTime = millis();
    threatLevel = 0;
    lastThreatLevel = 0;
    threatDowngradeSince = 0;
    lineRevealing = false;

    // seed for procedural particles (deterministic from start time)
    matSeed = millis();

    // select and pre-generate dump
    currentDump = selectDumpType();
    lastDumpType = currentDump;
    dumpCount++;
    generateDump(currentDump);
    dumpPhase = DumpPhase::NONE;

    startBootSequence();
    SFX::play(SFX::RECON_ALERT);
}

void hide() {
    if (state == State::HIDDEN || state == State::DISSOLVING) return;
    state = State::DISSOLVING;
    animStart = millis();
    spawnScatterParticles();
    bootSequenceActive = false;
    dumpPhase = DumpPhase::DONE;

    if (esp_random() & 1) {
        pushLineDim("%s exit", devicePrompt());
    } else {
        pushLineDim("%s logout", devicePrompt());
    }
}

void hideImmediate() {
    state = State::HIDDEN;
    particleCount = 0;

    bootSequenceActive = false;
    dumpPhase = DumpPhase::DONE;
}

bool isVisible() {
    return state != State::HIDDEN;
}

State getState() {
    return state;
}

uint8_t getThreatLevel() {
    return threatLevel;
}
void update(uint32_t now, uint8_t station, uint8_t room) {
    if (state == State::HIDDEN) return;

    // cursor blink
    if (now - lastCursorToggle > 500) {
        cursorVisible = !cursorVisible;
        lastCursorToggle = now;
    }

    showStation = station;
    showRoom = room;

    // ==[ LINE REVEAL COOLDOWN ]==
    if (lineRevealing && now - lastLineRevealTime >= 40) {
        lineRevealing = false;
    }

    // ==[ MATERIALIZING ]==
    if (state == State::MATERIALIZING) {
        uint32_t elapsed = now - animStart;
        uint32_t matDuration = kMaterializeMs;
        uint32_t logoEnd = kMatLogoEnd;

        // boot sequence during phase 4
        if (elapsed >= logoEnd && bootSequenceActive) {
            if (bootLineStartTime == 0) {
                bootLineStartTime = now;
            }
            if (!lineRevealing && bootLineIdx < BOOT_LINES_MAX) {
                if (now - bootLineStartTime >= kBootLineDelayMs || bootLineIdx == 0) {
                    pushBootLine(bootLineIdx, showStation);
                    bootLineIdx++;
                    bootLineStartTime = now;
                    if (bootLineIdx >= BOOT_LINES_MAX) {
                        bootSequenceActive = false;
                    }
                }
            }
        }

        if (elapsed > matDuration && !bootSequenceActive && !lineRevealing) {
            state = State::ACTIVE;
            flashStart = now;  // trigger flash effect

            // DEFHOG4: session debrief + challenges + dump
            if (Mood::hasDebrief()) {
                Mood::injectDebrief();
            }
            {
                const ActiveChallenge* c0 = Challenges::getChallenge(0);
                if (c0 && c0->target > 0) {
                    uint8_t chalDone2 = Challenges::getCompletedCount();
                    char marks[4] = "   ";
                    for (int i = 0; i < 3; i++) {
                        const ActiveChallenge* c = Challenges::getChallenge(i);
                        marks[i] = (c && c->completed) ? 'X' : '_';
                    }
                    pushLineDim("TASKS %d/3 [%c%c%c]", chalDone2, marks[0], marks[1], marks[2]);
                }
            }
            dumpPhase = DumpPhase::PAUSE;
            dumpPauseStart = now;
        }
        return;
    }

    // ==[ DISSOLVING ]==
    if (state == State::DISSOLVING) {
        updateParticles();
        if (now - animStart > kDissolveMs) {
            state = State::HIDDEN;
            particleCount = 0;
            return;
        }
        return;
    }

    // ==[ ACTIVE ]==
    if (state != State::ACTIVE) return;

    // ==[ DATA DUMP STATE MACHINE ]==
    switch (dumpPhase) {
        case DumpPhase::PAUSE:
            if (now - dumpPauseStart >= 500) {
                dumpPhase = DumpPhase::ACTIVE;
                dumpLastLinePush = now;
            }
            break;

        case DumpPhase::ACTIVE:
            // push command line first
            if (!dumpCommandPushed && !lineRevealing) {
                char cmdBuf[LINE_LEN];
                snprintf(cmdBuf, LINE_LEN, "%s%s", devicePrompt(), dumpCommand(currentDump));
                pushLineRaw(LineColor::FG, cmdBuf);
                dumpCommandPushed = true;
                dumpLastLinePush = now;
                break;
            }
            // push staged dump lines one by one
            if (dumpLinePushed < dumpLineCount && !lineRevealing) {
                uint32_t delay = DUMP_LINE_DELAY_MS + (esp_random() % 80);
                if (now - dumpLastLinePush >= delay) {
                    pushLineRaw(dumpColors[dumpLinePushed], dumpLines[dumpLinePushed]);
                    dumpLinePushed++;
                    dumpLastLinePush = now;
                }
            }
            // all lines pushed → accent bar separator → cooldown for next dump
            if (dumpLinePushed >= dumpLineCount && !lineRevealing) {
                pushLineBar();
                pushLineDim("");
                dumpPhase = DumpPhase::COOLDOWN;
                dumpCooldownStart = now;
                dumpCooldownMs = 1000 + (esp_random() % 2001);  // 1-3s random pause

                // ==[ TERMINAL ENGAGEMENT REWARDS ]==
                dumpsCompleted++;
                Challenges::onDumpWitnessed(dumpsCompleted);

                // milestones: 1st +2 momentum, 3rd +5 XP, 5th +10 XP, every 5th +5 XP
                // capped at MAX_DUMP_XP_AWARDS to prevent idle farming
                if (dumpsCompleted == 1) Mood::addMomentum(2);
                if (dumpsCompleted == 3 && dumpXPAwards < MAX_DUMP_XP_AWARDS) {
                    Config::addXP(5); dumpXPAwards++;
                }
                if (dumpsCompleted == 5) {
                    if (dumpXPAwards < MAX_DUMP_XP_AWARDS) {
                        Config::addXP(10); dumpXPAwards++;
                    }
                    pushLineHype("4N4LYST: %d DUMPS R3V13W3D", dumpsCompleted);
                } else if (dumpsCompleted > 5 && (dumpsCompleted % 5) == 0 &&
                           dumpXPAwards < MAX_DUMP_XP_AWARDS) {
                    Config::addXP(5); dumpXPAwards++;
                }
                if (dumpsCompleted >= 10) Achievements::tryUnlock(Achievement::ANALYST);
            }
            break;

        case DumpPhase::COOLDOWN:
            // random pause between dumps — then select next and loop
            if (now - dumpCooldownStart >= dumpCooldownMs) {
                currentDump = selectDumpType();
                lastDumpType = currentDump;
                dumpCount++;
                generateDump(currentDump);
                dumpPhase = DumpPhase::PAUSE;
                dumpPauseStart = now;
            }
            break;

        case DumpPhase::DONE:
        case DumpPhase::NONE:
            break;
    }

    // ==[ MONITORING ]== live events, scan pulses, quiet wire
    if (dumpPhase == DumpPhase::COOLDOWN || dumpPhase == DumpPhase::DONE) {
        updateThreatLevel(now);

        // periodic scan pulse — enriched with threat tier
        if (now - lastCommentaryTime > COMMENTARY_INTERVAL) {
            lastCommentaryTime = now;
            int apCount = Hunt::isActive() ? Hunt::getNetworkCount() : DefensePipeline::snapshot().getWifiSnapshotCount();
            int cliCount = Hunt::isActive() ? Hunt::getClientCount() : 0;
            const auto pipe = DefensePipeline::snapshot().getWifiPipelineStatus();
            if (!Hunt::isActive() && apCount == 0 && pipe.lastCompleteMs == 0) {
                pushLineDim("[SCAN] %s th:%s",
                            Recon::wifiPipelineStateLabel(pipe.state), threatTag());
            } else {
                pushLineDim("[SCAN] %dAP %dcli th:%s",
                            apCount, cliCount, threatTag());
            }
        }

        // quiet wire — channel + behavior context
        if (now - lastEventTime > QUIET_INTERVAL && now - lastQuietTime > QUIET_INTERVAL) {
            lastQuietTime = now;
            uint8_t ch = Hunt::isActive() ? Hunt::getCurrentChannel() : 0;
            if (ch > 0) {
                pushLineDim("--- ch:%02d %s quiet ---",
                            ch, behaviorTag(Hunt::getCurrentBehavior()));
            } else {
                pushLineDim("--- recon active ---");
            }
        }

        // elapsed timestamp — with session captures + threat
        if (now - lastElapsedTime > ELAPSED_INTERVAL) {
            lastElapsedTime = now;
            uint32_t elapsed = (now - terminalStartTime) / 1000;
            int mins = elapsed / 60;
            int secs = elapsed % 60;
            uint8_t pmk = Config::getSessionPMKIDCount();
            uint8_t hs = Config::getSessionHSCount();
            pushLineDim("[%02d:%02d] %dpmk %dhs th:%s",
                        mins, secs, pmk, hs, threatTag());
        }
    }
}

void draw(M5Canvas& canvas, uint16_t fg, uint16_t bg) {
    if (state == State::HIDDEN) return;
    (void)fg; (void)bg;  // theme-derived via TERM_FG()/TERM_DIM()

    uint32_t now = millis();
    uint32_t elapsed = now - animStart;

    canvas.setTextSize(1);
    canvas.setTextDatum(TL_DATUM);

    switch (state) {
        case State::MATERIALIZING:
            drawMaterialize(canvas, elapsed);
            break;

        case State::ACTIVE:
            drawActive(canvas);
            break;

        case State::DISSOLVING: {
            float t = (float)elapsed / (float)kDissolveMs;
            if (t > 1.0f) t = 1.0f;
            drawDissolve(canvas, t);
            break;
        }

        default:
            break;
    }
}

}  // namespace DefhogTerminal
