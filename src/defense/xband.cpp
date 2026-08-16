/**
 * XBand — Cross-Domain WiFi + BLE Forensic Intelligence
 *
 * ==[ FUSION ENGINE ]== 6 cross-domain correlations.
 * runs in the DefensePipeline fusion stage. no callbacks. no radio.
 * ~1KB PSRAM + ~150B DRAM. all data read-only from Hunt/Recon/Potfile.
 */

#define DEFENSE_PIPELINE_INTERNAL 1
#include "xband.h"
#include "recon_internal.h"   // pushEvent, psramAlloc, fnvHash
#include "potfile.h"
#include "../modes/hunt.h"
#include "../activity/pedometer.h"
#include "../util/debug_log.h"
#include <string.h>
#include <math.h>
#include <limits.h>
#include <esp_heap_caps.h>

namespace XBand {

// ==[ PSRAM-BACKED TABLES ]==
static AttackerProfile*       attackerProfiles = nullptr;
static int                    attackerCount = 0;

static PersistentProbeClient* persistentClients = nullptr;
static int                    persistentClientCount = 0;

static CohortPair*            cohortPairs = nullptr;
static int                    cohortCount = 0;

// ==[ DRAM STATICS ]==
static CrowdSnapshot          crowdRing[CROWD_RING_SIZE];
static int                    crowdHead = 0;
static int                    crowdFilled = 0;
static uint32_t               lastCrowdUpdate = 0;

static VendorCorrelation      vendorCorrels[MAX_VENDOR_CORRELATIONS];
static int                    vendorCorrelCount = 0;

static bool                   dualBandStalkActive = false;
static uint32_t               lastFullUpdate = 0;
static uint32_t               lastAttackerBurstSeen = 0;
static uint16_t               sessionPeakPop = 0;
static uint16_t               sessionMinPop = UINT16_MAX;

// ==[ TIMING ]==
static constexpr uint32_t UPDATE_INTERVAL_MS       = 5000;   // run correlations every 5s
static constexpr uint32_t CROWD_INTERVAL_MS        = 30000;  // crowd snapshot every 30s
static constexpr uint32_t ATTACKER_CORREL_WINDOW   = 30000;  // 30s BLE↔deauth match window
static constexpr uint32_t COHORT_TIME_WINDOW       = 60000;  // 60s temporal co-presence
static constexpr uint32_t COHORT_RETENTION_MS      = 300000; // keep evidence after Hunt exits
// 20min persistence — 10min was too permissive in crowded venues (any laptop on
// battery with WiFi on stays that long), producing near-constant C2 false fires.
// Paired with the ≥2 unique SSIDs gate below, this requires both longevity AND
// reconnaissance-style probing before we call a client a dual-band stalker.
static constexpr uint32_t PERSISTENT_CLIENT_MIN_MS = 1200000;
static constexpr uint32_t PERSISTENT_CLIENT_STALE_MS = 1800000;  // 30min no-see expiry
// Dual-band stalker profile: device probed multiple networks while staying near
// us. A device parked on a single SSID is likely just using location services;
// 2+ SSIDs suggests active network enumeration or router-hunt behavior.
static constexpr uint8_t  PERSISTENT_CLIENT_MIN_SSIDS = 2;
static constexpr uint32_t ATTACKER_ACTIVE_WINDOW   = 60000;  // 60s for "active attacker" query
static constexpr int      RSSI_BAND_THRESHOLD      = 15;     // dB max diff for RSSI band match

// ==[ PROXIMITY TIER ]== maps RSSI to tier for band matching
static uint8_t rssiTier(int8_t rssi) {
    if (rssi > -50) return 3;  // CLOSE
    if (rssi > -70) return 2;  // NEAR
    if (rssi > -85) return 1;  // FAR
    return 0;                  // EDGE
}

static bool timestampAfter(uint32_t candidate, uint32_t baseline) {
    return baseline == 0 || (int32_t)(candidate - baseline) > 0;
}

// ==[ OUI VENDOR ECOSYSTEM ]== map BLE companyId to WiFi OUI ecosystem
// returns a small ecosystem ID: 0=unknown, 1=Apple, 2=Samsung, 3=Google, 4=Espressif
static uint8_t bleEcosystem(uint16_t companyId) {
    switch (companyId) {
        case 0x004C: return 1;  // Apple
        case 0x0075: return 2;  // Samsung
        case 0x00E0: return 3;  // Google
        case 0x02E5: return 4;  // Espressif
        default: return 0;
    }
}

// check if WiFi OUI matches BLE ecosystem
static bool ouiMatchesEcosystem(const uint8_t oui[3], uint8_t eco) {
    // Apple OUI prefixes: 00:03:93, 00:0A:27, 00:0D:93, 00:11:24, 00:17:F2,
    //   00:1C:B3, 00:1E:52, 00:1F:F3, 00:21:E9, 00:23:12, 00:25:00, 00:26:08,
    //   28:6A:BA, 3C:15:C2, 40:33:1A, 54:26:96, 70:56:81, 7C:D1:C3, 88:66:A5,
    //   A4:83:E7, AC:DE:48, DC:2B:2A, F0:B4:79 ... (too many to hard-code all)
    // Use rough heuristic: if OUI[0] common Apple prefix byte
    if (eco == 1) {
        // known Apple AirPort/router OUIs
        if (oui[0] == 0x00 && oui[1] == 0x17 && oui[2] == 0xF2) return true;
        if (oui[0] == 0x00 && oui[1] == 0x1F && oui[2] == 0xF3) return true;
        if (oui[0] == 0x7C && oui[1] == 0xD1 && oui[2] == 0xC3) return true;
        if (oui[0] == 0x40 && oui[1] == 0x33 && oui[2] == 0x1A) return true;
        if (oui[0] == 0xF0 && oui[1] == 0xB4 && oui[2] == 0x79) return true;
    }
    if (eco == 2) {
        // Samsung OUI: 00:07:AB, 00:12:FB, 00:16:32, 00:21:19, 00:26:37
        if (oui[0] == 0x00 && oui[1] == 0x07 && oui[2] == 0xAB) return true;
        if (oui[0] == 0x00 && oui[1] == 0x16 && oui[2] == 0x32) return true;
    }
    if (eco == 4) {
        // Espressif: 24:0A:C4, 30:AE:A4, 24:62:AB, AC:67:B2, A4:CF:12
        if (oui[0] == 0x24 && oui[1] == 0x0A && oui[2] == 0xC4) return true;
        if (oui[0] == 0x30 && oui[1] == 0xAE && oui[2] == 0xA4) return true;
        if (oui[0] == 0x24 && oui[1] == 0x62 && oui[2] == 0xAB) return true;
        if (oui[0] == 0xAC && oui[1] == 0x67 && oui[2] == 0xB2) return true;
        if (oui[0] == 0xA4 && oui[1] == 0xCF && oui[2] == 0x12) return true;
    }
    return false;
}

// ==[ INIT ]==
void init() {
    if (!attackerProfiles) {
        attackerProfiles = (AttackerProfile*)Recon::psramAlloc(
            MAX_ATTACKER_PROFILES * sizeof(AttackerProfile), "xbAtk");
        if (attackerProfiles) memset(attackerProfiles, 0,
            MAX_ATTACKER_PROFILES * sizeof(AttackerProfile));
    }
    if (!persistentClients) {
        persistentClients = (PersistentProbeClient*)Recon::psramAlloc(
            MAX_PERSISTENT_CLIENTS * sizeof(PersistentProbeClient), "xbPersist");
        if (persistentClients) memset(persistentClients, 0,
            MAX_PERSISTENT_CLIENTS * sizeof(PersistentProbeClient));
    }
    if (!cohortPairs) {
        cohortPairs = (CohortPair*)Recon::psramAlloc(
            MAX_COHORT_PAIRS * sizeof(CohortPair), "xbCohort");
        if (cohortPairs) memset(cohortPairs, 0,
            MAX_COHORT_PAIRS * sizeof(CohortPair));
    }
    memset(crowdRing, 0, sizeof(crowdRing));
    memset(vendorCorrels, 0, sizeof(vendorCorrels));
    // stagger vs SD health (5s) and Config debounce — avoid aligned 5s spikes
    lastFullUpdate = millis() - UPDATE_INTERVAL_MS + 1700;
    HAMLET_LOGLN("[XBAND] init: fusion engine online");
}

// ==[ FEATURE 1: ATTACKER PLATFORM FINGERPRINT ]==

static void correlateAttackerBurst(const Recon::DeauthBurstRecord& burst, uint32_t now) {
    if (!attackerProfiles) return;
    if (burst.timestamp == 0 || now - burst.timestamp > ATTACKER_CORREL_WINDOW) return;
    if (burst.peakRSSI <= -127) return;

    const Recon::TrackerEntry* bleDevs = Recon::getBleDevices();
    int bleCount = Recon::getBleDeviceTableSize();
    if (!bleDevs || bleCount == 0) return;

    // One burst has one best BLE attribution candidate. Ranking every nearby
    // beacon as an attacker inflated profiles badly in dense rooms.
    int bestDevice = -1;
    int bestScore = INT_MIN;
    for (int i = 0; i < bleCount; i++) {
        const Recon::TrackerEntry& dev = bleDevs[i];
        if (dev.lastSeen == 0 || now - dev.lastSeen > ATTACKER_CORREL_WINDOW) continue;
        int rssiDiff = (int)dev.rssiSmooth - (int)burst.peakRSSI;
        if (rssiDiff < 0) rssiDiff = -rssiDiff;
        if (rssiDiff > RSSI_BAND_THRESHOLD) continue;
        bool isKnownTool = (dev.type == Recon::ThreatType::FLIPPER ||
                            dev.companyId == 0x02E5 ||
                            dev.type == Recon::ThreatType::HID_DEVICE);
        if (!isKnownTool && dev.rssiSmooth < -70) continue;
        int score = (isKnownTool ? 100 : 0) +
                    (RSSI_BAND_THRESHOLD - rssiDiff) * 2 -
                    (int)((now - dev.lastSeen) / 1000u);
        if (score > bestScore) {
            bestScore = score;
            bestDevice = i;
        }
    }

    for (int i = 0; i < bleCount; i++) {
        if (i != bestDevice) continue;
        const Recon::TrackerEntry& dev = bleDevs[i];
        if (now - dev.lastSeen > ATTACKER_CORREL_WINDOW) continue;

        // RSSI proximity: attacker's BLE TX and WiFi TX from same device
        int rssiDiff = (int)dev.rssiSmooth - (int)burst.peakRSSI;
        if (rssiDiff < 0) rssiDiff = -rssiDiff;
        if (rssiDiff > RSSI_BAND_THRESHOLD) continue;

        // prioritize known attack tools
        bool isKnownTool = (dev.type == Recon::ThreatType::FLIPPER ||
                            dev.companyId == 0x02E5 ||  // Espressif
                            dev.type == Recon::ThreatType::HID_DEVICE);

        // for unknown devices: require CLOSE/NEAR proximity
        if (!isKnownTool && dev.rssiSmooth < -70) continue;

        // check if we already track this payload hash
        int slot = -1;
        for (int j = 0; j < attackerCount; j++) {
            if (memcmp(attackerProfiles[j].blePayloadHash, dev.payloadHash, 4) == 0) {
                slot = j;
                break;
            }
        }

        if (slot >= 0) {
            // One scheduler tick is not one attack. Count each source burst once.
            if (attackerProfiles[slot].lastBurstTimestamp == burst.timestamp) continue;
            if (attackerProfiles[slot].correlatedBursts < UINT8_MAX) {
                attackerProfiles[slot].correlatedBursts++;
            }
            attackerProfiles[slot].lastCorrelated = burst.timestamp;
            attackerProfiles[slot].lastBurstTimestamp = burst.timestamp;
            attackerProfiles[slot].bleRssi = dev.rssiSmooth;
            attackerProfiles[slot].wifiDeauthRssi = burst.peakRSSI;
            memcpy(attackerProfiles[slot].targetBssid, burst.targetBssid, 6);
            if (dev.txPower != 0) {
                attackerProfiles[slot].estimatedDist =
                    Recon::estimateDistance(dev.rssiSmooth, dev.txPower);
            }
        } else {
            // Keep collecting when the table fills: replace the stalest profile.
            int dst = attackerCount;
            if (attackerCount >= MAX_ATTACKER_PROFILES) {
                dst = 0;
                for (int j = 1; j < attackerCount; j++) {
                    if ((now - attackerProfiles[j].lastCorrelated) >
                        (now - attackerProfiles[dst].lastCorrelated)) {
                        dst = j;
                    }
                }
            }
            AttackerProfile& ap = attackerProfiles[dst];
            memset(&ap, 0, sizeof(ap));
            memcpy(ap.blePayloadHash, dev.payloadHash, 4);
            ap.bleRssi = dev.rssiSmooth;
            ap.wifiDeauthRssi = burst.peakRSSI;
            ap.bleType = (uint8_t)dev.type;
            ap.correlatedBursts = 1;
            ap.companyId = dev.companyId;
            ap.estimatedDist = (dev.txPower != 0) ?
                Recon::estimateDistance(dev.rssiSmooth, dev.txPower) : -1.0f;
            ap.firstCorrelated = burst.timestamp;
            ap.lastCorrelated = burst.timestamp;
            ap.lastBurstTimestamp = burst.timestamp;
            memcpy(ap.targetBssid, burst.targetBssid, 6);
            strncpy(ap.bleName, dev.name, 15);
            ap.bleName[15] = '\0';
            if (attackerCount < MAX_ATTACKER_PROFILES) attackerCount++;

            // emit event
            Recon::ReconEventData ev = {};
            ev.event = Recon::ReconEvent::ATTACKER_IDENTIFIED;
            ev.rssi = dev.rssiSmooth;
            ev.channel = burst.dominantChannel;
            memcpy(ev.bssid, burst.targetBssid, 6);
            const char* mfr = Recon::manufacturerLabel(dev.companyId);
            if (mfr[0] != '?') {
                snprintf(ev.detail, sizeof(ev.detail), "%s %ddB", mfr, dev.rssiSmooth);
            } else if (dev.name[0]) {
                snprintf(ev.detail, sizeof(ev.detail), "%.12s %ddB", dev.name, dev.rssiSmooth);
            } else {
                snprintf(ev.detail, sizeof(ev.detail), "BLE %ddB ch%d", dev.rssiSmooth, burst.dominantChannel);
            }
            Recon::pushEvent(ev);
            HAMLET_LOGF("[XBAND] attacker ID: %s\n", ev.detail);
        }
    }
}

static void updateAttackerCorrelation(uint32_t now) {
    const Recon::DeauthBurstRecord* bursts = Recon::getDeauthBurstHistory();
    if (!bursts || Recon::getDeauthBurstHistoryCount() == 0) return;

    uint8_t order[Recon::MAX_DEAUTH_HISTORY];
    uint8_t count = 0;
    for (uint8_t i = 0; i < Recon::MAX_DEAUTH_HISTORY; i++) {
        const auto& burst = bursts[i];
        if (burst.timestamp == 0 || now - burst.timestamp > ATTACKER_CORREL_WINDOW) continue;
        if (!timestampAfter(burst.timestamp, lastAttackerBurstSeen)) continue;
        order[count++] = i;
    }

    // Oldest first so multiple bursts arriving between 5s fusion ticks survive.
    for (uint8_t i = 0; i < count; i++) {
        for (uint8_t j = i + 1; j < count; j++) {
            if ((now - bursts[order[j]].timestamp) > (now - bursts[order[i]].timestamp)) {
                uint8_t tmp = order[i];
                order[i] = order[j];
                order[j] = tmp;
            }
        }
    }
    for (uint8_t i = 0; i < count; i++) {
        const auto& burst = bursts[order[i]];
        correlateAttackerBurst(burst, now);
        lastAttackerBurstSeen = burst.timestamp;
    }
}

// ==[ FEATURE 2: DUAL-BAND STALKER DETECTION ]==

static bool isFreshGlobalProbe(const Hunt::HarvestedProbe& pr, uint32_t now) {
    if (pr.lastSeen == 0) return false;
    if (now - pr.lastSeen > PERSISTENT_CLIENT_STALE_MS) return false;
    return (pr.clientMac[0] & 0x02) == 0;
}

static void expirePersistentProbeClients(uint32_t now) {
    if (!persistentClients) return;

    for (int j = persistentClientCount - 1; j >= 0; j--) {
        if (now - persistentClients[j].lastSeen > PERSISTENT_CLIENT_STALE_MS) {
            if (j < persistentClientCount - 1) {
                persistentClients[j] = persistentClients[persistentClientCount - 1];
            }
            persistentClientCount--;
        }
    }
}

static uint8_t countUniqueClientSsids(const Hunt::HarvestedProbe* probes,
                                      uint16_t probeCount,
                                      const uint8_t mac[6],
                                      uint32_t now) {
    uint8_t ssidCount = 0;
    for (uint16_t i = 0; i < probeCount && ssidCount < 255; i++) {
        const Hunt::HarvestedProbe& pr = probes[i];
        if (!isFreshGlobalProbe(pr, now)) continue;
        if (pr.ssid[0] == '\0') continue;
        if (memcmp(mac, pr.clientMac, 6) != 0) continue;

        bool seen = false;
        for (uint16_t k = 0; k < i; k++) {
            const Hunt::HarvestedProbe& prev = probes[k];
            if (!isFreshGlobalProbe(prev, now)) continue;
            if (memcmp(mac, prev.clientMac, 6) != 0) continue;
            if (strncmp(pr.ssid, prev.ssid, sizeof(pr.ssid)) == 0) {
                seen = true;
                break;
            }
        }
        if (!seen) ssidCount++;
    }
    return ssidCount;
}

static void updatePersistentProbeClients(uint32_t now) {
    if (!persistentClients) return;

    const Hunt::HarvestedProbe* probes = Hunt::getHarvestedProbes();
    uint16_t probeCount = Hunt::getHarvestedCount();
    expirePersistentProbeClients(now);
    if (!probes || probeCount == 0) return;

    uint32_t currentDist = Pedometer::getDistance();

    for (uint16_t i = 0; i < probeCount; i++) {
        const Hunt::HarvestedProbe& pr = probes[i];
        if (!isFreshGlobalProbe(pr, now)) continue;

        // check if already tracked
        int slot = -1;
        for (int j = 0; j < persistentClientCount; j++) {
            if (memcmp(persistentClients[j].mac, pr.clientMac, 6) == 0) {
                slot = j;
                break;
            }
        }

        if (slot >= 0) {
            // update
            if (pr.lastSeen > persistentClients[slot].lastSeen) {
                persistentClients[slot].lastSeen = pr.lastSeen;
            }
            persistentClients[slot].rssiSmooth =
                (int8_t)((int)persistentClients[slot].rssiSmooth * 7 / 10 +
                         (int)pr.rssi * 3 / 10);
        } else if (persistentClientCount < MAX_PERSISTENT_CLIENTS) {
            // new entry
            PersistentProbeClient& pc = persistentClients[persistentClientCount];
            memcpy(pc.mac, pr.clientMac, 6);
            pc.firstSeen = pr.lastSeen;
            pc.lastSeen = pr.lastSeen;
            pc.rssiSmooth = pr.rssi;
            pc.probeCount = 1;
            pc.firstDetectDist = (uint16_t)(currentDist > 65535 ? 65535 : currentDist);
            persistentClientCount++;
        }
    }

    // count unique SSIDs per persistent client. Duplicate probe rows from one
    // remembered SSID do not satisfy the stalker gate.
    for (int j = 0; j < persistentClientCount; j++) {
        persistentClients[j].probeCount =
            countUniqueClientSsids(probes, probeCount, persistentClients[j].mac, now);
    }

    expirePersistentProbeClients(now);
}

static void checkDualBandStalking(uint32_t now) {
    dualBandStalkActive = false;
    if (!persistentClients || !cohortPairs) return;

    const Recon::TrackerEntry* trackers = Recon::getTrackers();
    int trackerCount = Recon::getTrackerTableSize();
    if (!trackers || trackerCount == 0) return;

    for (int t = 0; t < trackerCount; t++) {
        const Recon::TrackerEntry& te = trackers[t];
        if (!(te.flags & Recon::FLAG_FOLLOWING)) continue;

        // check for a persistent WiFi probe client in same proximity band
        uint8_t bleTier = rssiTier(te.rssiSmooth);
        for (int p = 0; p < persistentClientCount; p++) {
            const PersistentProbeClient& pc = persistentClients[p];
            if (now - pc.lastSeen > PERSISTENT_CLIENT_STALE_MS) continue;
            uint32_t duration = (pc.lastSeen > pc.firstSeen) ?
                (pc.lastSeen - pc.firstSeen) : 0;
            if (duration < PERSISTENT_CLIENT_MIN_MS) continue;
            // Require reconnaissance-style probing (multiple SSIDs) so a
            // single-SSID laptop doesn't get labelled a stalker.
            if (pc.probeCount < PERSISTENT_CLIENT_MIN_SSIDS) continue;

            uint8_t wifiTier = rssiTier(pc.rssiSmooth);
            int tierDiff = (int)bleTier - (int)wifiTier;
            if (tierDiff < 0) tierDiff = -tierDiff;
            if (tierDiff > 1) continue;  // must be same or adjacent tier

            // dual-band match confirmed
            dualBandStalkActive = true;

            // emit event (once per session per pair — use cohort to dedup)
            static uint32_t lastDualStalkEvent = 0;
            if (now - lastDualStalkEvent < 300000) break;  // 5min cooldown
            lastDualStalkEvent = now;

            Recon::ReconEventData ev = {};
            ev.event = Recon::ReconEvent::DUAL_BAND_STALK;
            ev.rssi = te.rssiSmooth;
            snprintf(ev.detail, sizeof(ev.detail), "%02X:%02X BLE+WiFi",
                     pc.mac[4], pc.mac[5]);
            Recon::pushEvent(ev);
            HAMLET_LOGF("[XBAND] DUAL-BAND STALK: BLE %ddB + WiFi %ddB\n",
                          te.rssiSmooth, pc.rssiSmooth);
            break;
        }
        if (dualBandStalkActive) break;
    }
}

// ==[ FEATURE 3: DEVICE COHORT CORRELATION ]==

static void updateCohorts(uint32_t now) {
    if (!cohortPairs) return;

    const Recon::TrackerEntry* bleDevs = Recon::getBleDevices();
    int bleCount = Recon::getBleDeviceTableSize();
    const Hunt::HarvestedProbe* probes = Hunt::getHarvestedProbes();
    uint16_t probeCount = Hunt::getHarvestedCount();

    if (!bleDevs || bleCount == 0 || !probes || probeCount == 0) {
        int write = 0;
        for (int read = 0; read < cohortCount; read++) {
            if (now - cohortPairs[read].lastCorrelated > COHORT_RETENTION_MS) continue;
            if (write != read) cohortPairs[write] = cohortPairs[read];
            write++;
        }
        cohortCount = write;
        return;
    }

    cohortCount = 0;

    uint8_t usedWifiClients[MAX_COHORT_PAIRS][6] = {};
    uint8_t usedWifiCount = 0;

    // Following trackers get first claim on a client before generic crowd rows.
    uint8_t bleOrder[Recon::MAX_BLE_DEVICES];
    uint8_t bleOrderCount = 0;
    for (uint8_t pass = 0; pass < 2; pass++) {
        for (int b = 0; b < bleCount && bleOrderCount < Recon::MAX_BLE_DEVICES; b++) {
            bool isFollowing = (bleDevs[b].flags & Recon::FLAG_FOLLOWING) != 0;
            if ((pass == 0) == isFollowing) bleOrder[bleOrderCount++] = (uint8_t)b;
        }
    }

    for (uint8_t oi = 0; oi < bleOrderCount && cohortCount < MAX_COHORT_PAIRS; oi++) {
        int b = bleOrder[oi];
        const Recon::TrackerEntry& dev = bleDevs[b];
        if (dev.lastSeen == 0 || now - dev.lastSeen > COHORT_TIME_WINDOW) continue;
        // skip environmental beacons — they don't belong to people walking around
        if (dev.type == Recon::ThreatType::IBEACON ||
            dev.type == Recon::ThreatType::EDDYSTONE) continue;

        uint8_t bleTier = rssiTier(dev.rssiSmooth);
        uint8_t bleEco = bleEcosystem(dev.companyId);
        bool isFollowing = (dev.flags & Recon::FLAG_FOLLOWING) != 0;

        // find best WiFi probe match for this BLE device
        int bestProbe = -1;
        uint8_t bestConfidence = 0;
        uint32_t bestTimeDiff = UINT32_MAX;
        int bestRssiDiff = INT_MAX;

        for (uint16_t p = 0; p < probeCount; p++) {
            const Hunt::HarvestedProbe& pr = probes[p];
            if (!isFreshGlobalProbe(pr, now)) continue;
            if (now - pr.lastSeen > COHORT_TIME_WINDOW) continue;
            bool alreadyUsed = false;
            for (uint8_t u = 0; u < usedWifiCount; u++) {
                if (memcmp(usedWifiClients[u], pr.clientMac, 6) == 0) {
                    alreadyUsed = true;
                    break;
                }
            }
            if (alreadyUsed) continue;

            // temporal co-presence
            uint32_t bleAge = now - dev.lastSeen;
            uint32_t wifiAge = now - pr.lastSeen;
            uint32_t timeDiff = (bleAge > wifiAge) ? bleAge - wifiAge : wifiAge - bleAge;

            uint8_t confidence = 1;  // temporal match = LOW

            // RSSI band match
            uint8_t wifiTier = rssiTier(pr.rssi);
            int tierDiff = (int)bleTier - (int)wifiTier;
            if (tierDiff < 0) tierDiff = -tierDiff;
            if (tierDiff <= 1) confidence++;  // same/adjacent tier = +1
            int rssiDiff = (int)dev.rssiSmooth - (int)pr.rssi;
            if (rssiDiff < 0) rssiDiff = -rssiDiff;

            // vendor ecosystem coherence
            if (bleEco > 0) {
                // check if WiFi client OUI matches BLE ecosystem
                // (WiFi client MACs are globally unique if they passed the 0x02 filter)
                uint8_t wifiOui[3] = { pr.clientMac[0], pr.clientMac[1], pr.clientMac[2] };
                if (ouiMatchesEcosystem(wifiOui, bleEco)) confidence++;
            }

            if (confidence > bestConfidence ||
                (confidence == bestConfidence && timeDiff < bestTimeDiff) ||
                (confidence == bestConfidence && timeDiff == bestTimeDiff &&
                 rssiDiff < bestRssiDiff)) {
                bestConfidence = confidence;
                bestProbe = p;
                bestTimeDiff = timeDiff;
                bestRssiDiff = rssiDiff;
            }
        }

        // only store MED+ confidence (or any confidence for FOLLOWING trackers)
        if (bestProbe >= 0 && (bestConfidence >= 2 || isFollowing)) {
            const Hunt::HarvestedProbe& pr = probes[bestProbe];
            CohortPair& cp = cohortPairs[cohortCount];
            memcpy(cp.wifiMac, pr.clientMac, 6);
            memcpy(cp.blePayloadHash, dev.payloadHash, 4);
            cp.wifiRssi = pr.rssi;
            cp.bleRssi = dev.rssiSmooth;
            cp.confidence = bestConfidence;
            cp.timeDeltaSec = (uint16_t)((bestTimeDiff / 1000u) > UINT16_MAX
                ? UINT16_MAX : (bestTimeDiff / 1000u));
            cp.bleType = (uint8_t)dev.type;
            cp.companyId = dev.companyId;
            cp.lastCorrelated = now;
            strncpy(cp.probeSSID, pr.ssid, 16);
            cp.probeSSID[16] = '\0';
            cp.isFollowing = isFollowing;
            cp.potfileMatch = (pr.ssid[0] && Potfile::isKnown(pr.ssid));
            if (usedWifiCount < MAX_COHORT_PAIRS) {
                memcpy(usedWifiClients[usedWifiCount++], pr.clientMac, 6);
            }

            // Feature 4: emit event if following + potfile match
            if (isFollowing && cp.potfileMatch) {
                static uint32_t lastFollowNetEvent = 0;
                if (now - lastFollowNetEvent >= 300000) {
                    Recon::ReconEventData ev = {};
                    ev.event = Recon::ReconEvent::FOLLOWING_NETWORK_ID;
                    ev.rssi = dev.rssiSmooth;
                    snprintf(ev.detail, sizeof(ev.detail), "%.14s", pr.ssid);
                    snprintf(ev.ssid, sizeof(ev.ssid), "%s", pr.ssid);
                    Recon::pushEvent(ev);
                    lastFollowNetEvent = now;
                    HAMLET_LOGF("[XBAND] FOLLOW-NET: %s (potfile match)\n", pr.ssid);
                }
            }

            cohortCount++;
        }
    }
}

// ==[ FEATURE 5: CROWD DENSITY & COMPOSITION ]==

static void updateCrowdIntel(uint32_t now) {
    if (now - lastCrowdUpdate < CROWD_INTERVAL_MS) return;
    lastCrowdUpdate = now;

    CrowdSnapshot& snap = crowdRing[crowdHead];
    memset(&snap, 0, sizeof(CrowdSnapshot));
    snap.timestamp = now;

    // count BLE devices by appearance category
    const Recon::TrackerEntry* bleDevs = Recon::getBleDevices();
    int bleCount = Recon::getBleDeviceTableSize();
    if (bleDevs) {
        for (int i = 0; i < bleCount; i++) {
            const Recon::TrackerEntry& dev = bleDevs[i];
            if (now - dev.lastSeen > 60000) continue;  // only recent
            uint16_t app = dev.appearance;
            if (app >= 64 && app <= 79)       snap.blePhones++;    // phone category
            else if (app >= 192 && app <= 255) snap.bleWatches++;  // watch category
            else if (dev.type == Recon::ThreatType::AIRTAG ||
                     dev.type == Recon::ThreatType::SMARTTAG ||
                     dev.type == Recon::ThreatType::TILE)
                                               snap.bleTags++;
            else if (app >= 768 && app <= 895) snap.bleSensors++;  // sensor/thermometer
            else                               snap.bleOther++;
        }
    }

    // WiFi population is unique recent clients, not probe SSID rows.
    const Hunt::HarvestedProbe* probes = Hunt::getHarvestedProbes();
    uint16_t probeCount = Hunt::getHarvestedCount();
    for (uint16_t i = 0; probes && i < probeCount && snap.wifiClients < UINT8_MAX; i++) {
        const auto& pr = probes[i];
        if (!isFreshGlobalProbe(pr, now) || now - pr.lastSeen > 60000) continue;
        bool seen = false;
        for (uint16_t j = 0; j < i; j++) {
            if (!isFreshGlobalProbe(probes[j], now) || now - probes[j].lastSeen > 60000) continue;
            if (memcmp(probes[j].clientMac, pr.clientMac, 6) == 0) {
                seen = true;
                break;
            }
        }
        if (!seen) snap.wifiClients++;
    }
    // Hunt frees its probe table on exit; persistent fusion state carries the
    // same recent global clients long enough for crowd density to decay cleanly.
    for (int i = 0; persistentClients && i < persistentClientCount &&
                    snap.wifiClients < UINT8_MAX; i++) {
        const auto& pc = persistentClients[i];
        if (pc.lastSeen == 0 || now - pc.lastSeen > 60000) continue;
        bool seen = false;
        for (uint16_t p = 0; probes && p < probeCount; p++) {
            if (isFreshGlobalProbe(probes[p], now) && now - probes[p].lastSeen <= 60000 &&
                memcmp(probes[p].clientMac, pc.mac, 6) == 0) {
                seen = true;
                break;
            }
        }
        if (!seen) snap.wifiClients++;
    }
    snap.wifiAPs = (uint8_t)(Recon::getLastWifiAPCount() > 255 ? 255 : Recon::getLastWifiAPCount());
    snap.appleContinuity = (uint8_t)(Recon::getAppleContinuityCount() > 255 ? 255 : Recon::getAppleContinuityCount());

    // population estimate: avoid double-counting
    int bleTotal = snap.blePhones + snap.bleWatches + snap.bleOther;
    int wifiTotal = snap.wifiClients;
    int maxBand = (bleTotal > wifiTotal) ? bleTotal : wifiTotal;
    int minBand = (bleTotal < wifiTotal) ? bleTotal : wifiTotal;
    snap.estimatedPop = (uint16_t)(maxBand + minBand / 3);

    // track session extremes
    if (snap.estimatedPop > sessionPeakPop) sessionPeakPop = snap.estimatedPop;
    if (snap.estimatedPop > 0 && snap.estimatedPop < sessionMinPop) sessionMinPop = snap.estimatedPop;

    crowdHead = (crowdHead + 1) % CROWD_RING_SIZE;
    if (crowdFilled < CROWD_RING_SIZE) crowdFilled++;
}

// ==[ FEATURE 6: VENDOR ECOSYSTEM CROSS-REF ]==

static void updateVendorCorrelation(uint32_t now) {
    vendorCorrelCount = 0;

    const Recon::TrackerEntry* bleDevs = Recon::getBleDevices();
    int bleCount = Recon::getBleDeviceTableSize();
    if (!bleDevs || bleCount == 0) return;

    // build BLE vendor histogram (CLOSE/NEAR only)
    struct VendorBucket { uint16_t companyId; uint8_t count; int8_t bestRssi; };
    VendorBucket buckets[MAX_VENDOR_CORRELATIONS];
    int bucketCount = 0;

    for (int i = 0; i < bleCount; i++) {
        const Recon::TrackerEntry& dev = bleDevs[i];
        if (dev.companyId == 0) continue;
        if (dev.rssiSmooth < -70) continue;  // CLOSE/NEAR only
        if (now - dev.lastSeen > 60000) continue;

        int slot = -1;
        for (int j = 0; j < bucketCount; j++) {
            if (buckets[j].companyId == dev.companyId) { slot = j; break; }
        }
        if (slot >= 0) {
            buckets[slot].count++;
            if (dev.rssiSmooth > buckets[slot].bestRssi)
                buckets[slot].bestRssi = dev.rssiSmooth;
        } else if (bucketCount < MAX_VENDOR_CORRELATIONS) {
            buckets[bucketCount] = { dev.companyId, 1, dev.rssiSmooth };
            bucketCount++;
        }
    }

    // Cross-reference both live sources. Hunt has richer vendor IEs; Recon's
    // current snapshot keeps DEFHOG4 useful when Hunt is not foreground.
    const DetectedNetwork* nets = Hunt::getNetworks();
    uint16_t netCount = Hunt::getNetworkCount();
    const Recon::WifiAP* aps = Recon::getWifiSnapshot();
    int apCount = Recon::getWifiSnapshotCount();

    for (int v = 0; v < bucketCount && vendorCorrelCount < MAX_VENDOR_CORRELATIONS; v++) {
        VendorCorrelation& vc = vendorCorrels[vendorCorrelCount];
        vc.companyId = buckets[v].companyId;
        vc.bleDeviceCount = buckets[v].count;
        vc.closestBleRssi = buckets[v].bestRssi;
        vc.wifiAPCount = 0;
        vc.closestWifiRssi = -127;
        vc.hasIoT = false;

        uint8_t eco = bleEcosystem(buckets[v].companyId);

        if (nets && netCount > 0 && eco > 0) {
            for (uint16_t n = 0; n < netCount; n++) {
                const DetectedNetwork& net = nets[n];
                if (net.lastSeen == 0 || now - net.lastSeen > 60000) continue;
                if (!net.hasVendorOUI) continue;
                if (ouiMatchesEcosystem(net.vendorOUI, eco)) {
                    vc.wifiAPCount++;
                    if (net.rssi > vc.closestWifiRssi) vc.closestWifiRssi = net.rssi;
                    if (eco == 4 && !net.hasPMF &&
                        (net.authmode == WIFI_AUTH_OPEN || net.authmode == WIFI_AUTH_WEP ||
                         (net.wpsState > 0 && !net.wpsLocked))) {
                        vc.hasIoT = true;
                    }
                }
            }
        }

        if (aps && apCount > 0 && eco > 0) {
            for (int a = 0; a < apCount; a++) {
                const Recon::WifiAP& ap = aps[a];
                if (!ouiMatchesEcosystem(ap.bssid, eco)) continue;

                // Do not count a BSSID already represented by fresh Hunt intel.
                bool duplicate = false;
                for (uint16_t n = 0; nets && n < netCount; n++) {
                    if (nets[n].lastSeen != 0 && now - nets[n].lastSeen <= 60000 &&
                        memcmp(nets[n].bssid, ap.bssid, 6) == 0 && nets[n].hasVendorOUI &&
                        ouiMatchesEcosystem(nets[n].vendorOUI, eco)) {
                        duplicate = true;
                        break;
                    }
                }
                if (duplicate) continue;

                vc.wifiAPCount++;
                if (ap.rssi > vc.closestWifiRssi) vc.closestWifiRssi = ap.rssi;
                if (eco == 4 &&
                    (ap.authMode == WIFI_AUTH_OPEN || ap.authMode == WIFI_AUTH_WEP)) {
                    vc.hasIoT = true;
                }
            }
        }

        vendorCorrelCount++;
    }
}

// ==[ MAIN UPDATE ]==

void update(uint32_t now) {
    if (now - lastFullUpdate < UPDATE_INTERVAL_MS) return;
    lastFullUpdate = now;

    updateAttackerCorrelation(now);
    updatePersistentProbeClients(now);
    updateCohorts(now);              // Feature 3 + 4 (cohort + following→network)
    checkDualBandStalking(now);      // Feature 2 (uses cohort + persistent clients)
    updateCrowdIntel(now);           // Feature 5
    updateVendorCorrelation(now);    // Feature 6
}

// ==[ PUBLIC API ]==

int  getAttackerCount() { return attackerCount; }
const AttackerProfile* getAttackerProfiles() { return attackerProfiles; }
bool hasActiveAttacker() {
    if (!attackerProfiles) return false;
    uint32_t now = millis();
    for (int i = 0; i < attackerCount; i++) {
        if (now - attackerProfiles[i].lastCorrelated < ATTACKER_ACTIVE_WINDOW) return true;
    }
    return false;
}

bool isDualBandStalkActive() { return dualBandStalkActive; }
int  getPersistentClientCount() { return persistentClientCount; }

int  getCohortCount() { return cohortCount; }
const CohortPair* getCohortPairs() { return cohortPairs; }
int  getHighConfidenceCohortCount() {
    int count = 0;
    for (int i = 0; i < cohortCount; i++) {
        if (cohortPairs[i].confidence >= 3) count++;
    }
    return count;
}

const CrowdSnapshot* getCurrentCrowd() {
    if (crowdFilled == 0) return nullptr;
    int idx = (crowdHead - 1 + CROWD_RING_SIZE) % CROWD_RING_SIZE;
    return &crowdRing[idx];
}

CrowdTrend getCrowdTrend() {
    if (crowdFilled < 2) return CrowdTrend::UNKNOWN;
    int newest = (crowdHead - 1 + CROWD_RING_SIZE) % CROWD_RING_SIZE;
    int oldest = (crowdFilled >= CROWD_RING_SIZE) ?
        crowdHead : 0;
    int diff = (int)crowdRing[newest].estimatedPop - (int)crowdRing[oldest].estimatedPop;
    if (diff > 2) return CrowdTrend::GROWING;
    if (diff < -2) return CrowdTrend::SHRINKING;
    return CrowdTrend::STABLE;
}

uint16_t getEstimatedPopulation() {
    const CrowdSnapshot* cs = getCurrentCrowd();
    return cs ? cs->estimatedPop : 0;
}

int  getVendorCorrelationCount() { return vendorCorrelCount; }
const VendorCorrelation* getVendorCorrelations() { return vendorCorrels; }

bool hasActiveIntel() {
    return hasActiveAttacker() ||
           getHighConfidenceCohortCount() > 0 ||
           dualBandStalkActive;
}

bool hasCriticalIntel() {
    return dualBandStalkActive || hasActiveAttacker();
}

CrowdTier getCrowdTier() {
    uint16_t pop = getEstimatedPopulation();
    if (pop >= 100) return CrowdTier::PACKED;
    if (pop >= 40)  return CrowdTier::CROWDED;
    if (pop >= 15)  return CrowdTier::BUSY;
    if (pop >= 5)   return CrowdTier::SPARSE;
    return CrowdTier::DESERTED;
}

bool isDeserted() { return getEstimatedPopulation() < 5; }
bool isCrowded()  { return getEstimatedPopulation() >= 40; }
uint16_t getSessionPeakPop() { return sessionPeakPop; }
uint16_t getSessionMinPop()  { return sessionMinPop == UINT16_MAX ? 0 : sessionMinPop; }

}  // namespace XBand
