/**
 * recon_forensic.inl — event queue, forensic logging, dedup, tracker following
 *
 * INCLUDED by recon.cpp — not a standalone translation unit.
 * All static state is defined in recon.cpp and visible here.
 */

static bool isDedupCooldown(DedupEntry* table, int& count, uint32_t hash, uint32_t cooldownMs) {
    uint32_t now = millis();
    // check existing entries
    for (int i = 0; i < count; i++) {
        if (table[i].hash == hash) {
            if (now - table[i].timestamp < cooldownMs) return true;  // still cooling down
            table[i].timestamp = now;  // refresh
            return false;
        }
    }
    // new entry
    if (count < MAX_DEDUP) {
        table[count].hash = hash;
        table[count].timestamp = now;
        count++;
    } else {
        // overwrite oldest
        int oldest = 0;
        for (int i = 1; i < MAX_DEDUP; i++) {
            if ((now - table[i].timestamp) > (now - table[oldest].timestamp)) oldest = i;
        }
        table[oldest].hash = hash;
        table[oldest].timestamp = now;
    }
    return false;
}

// ==[ DEDUP SWEEP ]== purge expired entries from a dedup table
// called from analyzeSnapshot() to reclaim slots; MAX_DEDUP is tiny (8)
// so the loop is effectively free. Compacts in-place to keep count accurate.
static void sweepDedupTable(DedupEntry* table, int& count, uint32_t maxAgeMs) {
    uint32_t now = millis();
    int write = 0;
    for (int read = 0; read < count; read++) {
        if (now - table[read].timestamp < maxAgeMs) {
            if (write != read) table[write] = table[read];
            write++;
        }
    }
    count = write;
}

// ==[ DEDUP SWEEP ALL ]== sweep every dedup table with its own cooldown
static void sweepAllDedupTables() {
    sweepDedupTable(dedupEvilTwin,   dedupEvilTwinCount,   EVIL_TWIN_COOLDOWN_MS);
    sweepDedupTable(dedupKarma,      dedupKarmaCount,      KARMA_COOLDOWN_MS);
    sweepDedupTable(dedupKnownAP,    dedupKnownAPCount,    KNOWN_AP_COOLDOWN_MS);
    sweepDedupTable(dedupFingerprint,dedupFingerprintCount, FINGERPRINT_EVENT_COOLDOWN_MS);
    sweepDedupTable(dedupSeq,        dedupSeqCount,        SEQ_EVENT_COOLDOWN_MS);
    sweepDedupTable(dedupRssi,       dedupRssiCount,       RSSI_EVENT_COOLDOWN_MS);
}

void pushEvent(const ReconEventData& ev) {
    DefensePipeline::emitEvent(ev);
}

void recordForensicEvent(const ReconEventData& ev) { appendForensicLog(ev); }

// ==[ FORENSIC LOG: BUILD INDICATOR FLAGS ]==
// Snapshot ALL currently active threat indicators — not just the triggering event.
static uint8_t buildIndicatorFlags() {
    uint8_t flags = 0;
    if (isFingerprintMismatchActive()) flags |= IND_FINGERPRINT;
    if (isSeqAnomalyActive())          flags |= IND_SEQ_ANOMALY;
    if (isRssiAnomalyActive())         flags |= IND_RSSI_ANOMALY;
    if (isEvilTwinActive())            flags |= IND_EVIL_TWIN;
    if (isKarmaActive())               flags |= IND_KARMA;
    if (isDeauthActive())              flags |= IND_DEAUTH;
    // check for active BLE threats (spam or Flipper seen in last 30s)
    uint32_t now = millis();
    for (int i = 0; i < trackerCount; i++) {
        if (now - trackerTable[i].lastSeen > 30000) continue;
        if ((trackerTable[i].flags & FLAG_SPAM) || trackerTable[i].type == ThreatType::FLIPPER) {
            flags |= IND_BLE_ATTACK;
            break;
        }
    }
    return flags;
}

// ==[ FORENSIC LOG: APPEND ]==
static void appendForensicLog(const ReconEventData& ev) {
    if (!forensicLog) return;
    // only log security-relevant events (not SCAN_COMPLETE, KNOWN_AP etc.)
    switch (ev.event) {
        case ReconEvent::EVIL_TWIN:
        case ReconEvent::KARMA_HONEYPOT:
        case ReconEvent::FINGERPRINT_MISMATCH:
        case ReconEvent::SEQ_ANOMALY:
        case ReconEvent::RSSI_ANOMALY:
        case ReconEvent::DEAUTH_DETECTED:
        case ReconEvent::PROBE_VULN_CLIENT:
        case ReconEvent::TRACKER_FOLLOWING:
        case ReconEvent::COORDINATED_ATTACK:
        case ReconEvent::ATTACKER_IDENTIFIED:
        case ReconEvent::DUAL_BAND_STALK:
        case ReconEvent::FOLLOWING_NETWORK_ID:
        case ReconEvent::KARMA_CONFIRMED:
        case ReconEvent::CANARY_TRIPPED:
        case ReconEvent::TOOL_IDENTIFIED:
        case ReconEvent::HOSTILE_CLIENT:
        case ReconEvent::RELAY_SUSPECT:
        case ReconEvent::LOW_ENTROPY_BEACON:
            break;
        default:
            return;  // skip non-forensic events
    }
    uint32_t now = millis();
    uint8_t newFlags = buildIndicatorFlags();
    // dedup: if the last row matches (event, bssid, indicator set) within
    // a 2-min window, just refresh timestamp + bump repeat count in _pad[0].
    // long-lived threats no longer burn ring slots with identical rows.
    if (forensicLogCount > 0) {
        uint8_t lastIdx = (forensicLogHead + MAX_FORENSIC_LOG - 1) % MAX_FORENSIC_LOG;
        ForensicLogEntry& last = forensicLog[lastIdx];
        if (last.event == ev.event &&
            memcmp(last.bssid, ev.bssid, 6) == 0 &&
            last.indicatorFlags == newFlags &&
            now - last.timestamp < 120000) {
            last.timestamp = now;
            if (last._pad[0] < 255) last._pad[0]++;
            return;
        }
    }
    ForensicLogEntry& entry = forensicLog[forensicLogHead];
    entry.timestamp = now;
    entry.event = ev.event;
    memcpy(entry.bssid, ev.bssid, 6);
    entry.rssi = ev.rssi;
    entry.channel = ev.channel;
    entry.indicatorFlags = newFlags;
    entry._pad[0] = entry._pad[1] = 0;
    forensicLogHead = (forensicLogHead + 1) % MAX_FORENSIC_LOG;
    if (forensicLogCount < MAX_FORENSIC_LOG) forensicLogCount++;

    // live SD export — deferred append via SDStorage HAL
#ifndef SIMULATOR
    if (forensicExportEnabled && SDStorage::isAvailable()) {
        const ForensicLogEntry& e = forensicLog[forensicLogHead == 0 ? MAX_FORENSIC_LOG - 1 : forensicLogHead - 1];
        char line[96];
        int lineLen = snprintf(line, sizeof(line),
            "{\"t\":%lu,\"ev\":%d,\"ch\":%d,\"rssi\":%d,\"flags\":%d}\n",
            (unsigned long)e.timestamp, (int)e.event, e.channel, e.rssi,
            e.indicatorFlags);
        if (lineLen > 0) {
            if (!SDStorage::enqueueAppend("/recon/live.jsonl",
                                          (const uint8_t*)line, (size_t)lineLen)) {
                SDStorage::appendFile("/recon/live.jsonl",
                                      (const uint8_t*)line, (size_t)lineLen);
            }
        }
    }
#endif
}

// check for trackers that have been following (seen 3+ cycles over 15+ minutes)
static void updateTrackerFollowing(uint32_t now) {
    // age out stale trackers first so FOLLOWING counts don't linger for an extra scan.
    for (int t = trackerCount - 1; t >= 0; t--) {
        if (now - trackerTable[t].lastSeen > 1800000) {
            if (trackerTable[t].flags & FLAG_SPAM) spamCount--;
            // shift remaining entries
            for (int k = t; k < trackerCount - 1; k++) {
                trackerTable[k] = trackerTable[k + 1];
            }
            trackerCount--;
        }
    }

    followingCount = 0;

    // crowd-adaptive following thresholds:
    // dense crowds → require longer persistence (reduce false positives)
    // deserted → standard thresholds (threat scoring escalates independently)
    XBand::CrowdTier crowd = XBand::getCrowdTier();
    uint32_t timeThreshold = 900000;   // 15min default
    uint32_t stepTimeThreshold = 600000; // 10min default
    uint32_t distThreshold = 800;      // 800m default
    uint8_t  seenThreshold = 3;
    if (crowd >= XBand::CrowdTier::CROWDED) {
        // 40+ people: harder to confirm — require 25min, 1200m, 5 scans
        timeThreshold = 1500000;
        stepTimeThreshold = 900000;
        distThreshold = 1200;
        seenThreshold = 5;
    } else if (crowd == XBand::CrowdTier::BUSY) {
        // 15-39: slightly stricter — 20min, 1000m, 4 scans
        timeThreshold = 1200000;
        stepTimeThreshold = 750000;
        distThreshold = 1000;
        seenThreshold = 4;
    }

    for (int t = 0; t < trackerCount; t++) {
        TrackerEntry& te = trackerTable[t];

        bool wasFollowing = (te.flags & FLAG_FOLLOWING) != 0;
        // ==[ CONFIDENCE CASCADE ]== a tracker sitting at CLOSE proximity
        // (>-50dBm) for 5+ minutes is high-confidence regardless of crowd
        // density: attackers don't park an AirTag 3m away by accident. Skip
        // the crowd-adjusted 25min wait in that case so we alert promptly.
        bool closeConfident = (te.rssiSmooth > -50) &&
                              (now - te.firstSeen >= 300000) &&
                              (te.seenCount >= 3);
        bool isFollowing = closeConfident ||
                           ((te.seenCount >= seenThreshold) &&
                            (now - te.firstSeen >= timeThreshold));

        // step-distance following: seen across movement over time
        uint32_t currentDist = Pedometer::getDistance();
        uint32_t distDelta = (currentDist > te.firstDetectDist)
                             ? (currentDist - te.firstDetectDist) : 0;
        bool isStepFollowing = (distDelta >= distThreshold) &&
                               (now - te.firstSeen >= stepTimeThreshold) &&
                               (te.seenCount >= seenThreshold);
        if (isStepFollowing) te.flags |= FLAG_STEP_FOLLOWING;

        if (isFollowing || isStepFollowing) {
            te.flags |= FLAG_FOLLOWING;
            followingCount++;

            if (!wasFollowing && !(te.flags & FLAG_ALERTED)) {
                te.flags |= FLAG_ALERTED;
                ReconEventData ev = {};
                ev.event = ReconEvent::TRACKER_FOLLOWING;
                ev.threatType = te.type;
                ev.rssi = te.rssiSmooth;
                snprintf(ev.detail, sizeof(ev.detail), "%s %ddB %s",
                        deviceLabel(te),
                        te.rssiSmooth,
                        proximityLabelInternal(te.rssiSmooth));
                pushEvent(ev);
                HAMLET_LOGF("[RECON] FOLLOWING: %s for %ds\n", ev.detail,
                             (int)(now - te.firstSeen) / 1000);
            }
        }
    }
}

uint8_t getForensicLogCount() { return forensicLogCount; }
uint8_t getForensicLogHead() { return forensicLogHead; }
const ForensicLogEntry* getForensicLog() { return forensicLog; }

uint8_t countIndicatorsForBSSID(const uint8_t* bssid, uint32_t windowMs) {
    if (!forensicLog || !bssid || forensicLogCount == 0) return 0;
    uint32_t now = millis();
    uint8_t unionFlags = 0;
    // scan ring buffer for matching BSSID entries within window
    for (uint8_t i = 0; i < forensicLogCount; i++) {
        int idx = ((int)forensicLogHead - 1 - i + MAX_FORENSIC_LOG) % MAX_FORENSIC_LOG;
        const ForensicLogEntry& e = forensicLog[idx];
        if (e.timestamp == 0 || now - e.timestamp > windowMs) continue;
        if (memcmp(e.bssid, bssid, 6) == 0) {
            unionFlags |= e.indicatorFlags;
        }
    }
    // popcount of distinct indicator bits
    uint8_t count = 0;
    for (uint8_t b = unionFlags; b; b >>= 1) count += (b & 1);
    return count;
}

// ==[ DEAUTH BURST HISTORY API ]==

uint8_t getDeauthBurstHistoryCount() { return deauthHistoryCount; }
const DeauthBurstRecord* getDeauthBurstHistory() { return deauthHistory; }

// ==[ PROBE-POTFILE MATCH CACHE API ]==

void cacheProbeVulnMatch(const uint8_t* clientMac, const char* ssid, int8_t rssi) {
    if (!probeVulnCache || !clientMac || !ssid) return;
    uint32_t now = millis();
    pruneProbeVulnCache(now);

    // check for existing entry (same client+SSID) — update RSSI/timestamp
    for (uint8_t i = 0; i < probeVulnCount; i++) {
        if (memcmp(probeVulnCache[i].clientMac, clientMac, 6) == 0 &&
            strcmp(probeVulnCache[i].ssid, ssid) == 0) {
            probeVulnCache[i].rssi = rssi;
            probeVulnCache[i].lastSeen = now;
            return;
        }
    }
    // new entry — add or evict oldest
    uint8_t slot;
    if (probeVulnCount < MAX_PROBE_VULN_CACHE) {
        slot = probeVulnCount++;
    } else {
        // evict oldest
        slot = 0;
        for (uint8_t i = 0; i < MAX_PROBE_VULN_CACHE; i++) {
            if ((now - probeVulnCache[i].lastSeen) >
                (now - probeVulnCache[slot].lastSeen)) {
                slot = i;
            }
        }
    }
    memcpy(probeVulnCache[slot].clientMac, clientMac, 6);
    strncpy(probeVulnCache[slot].ssid, ssid, 32);
    probeVulnCache[slot].ssid[32] = '\0';
    probeVulnCache[slot].rssi = rssi;
    probeVulnCache[slot].lastSeen = now;
}

static void pruneProbeVulnCache(uint32_t now) {
    if (!probeVulnCache || probeVulnCount == 0) return;

    uint8_t write = 0;
    for (uint8_t read = 0; read < probeVulnCount; read++) {
        const ProbeVulnMatch& m = probeVulnCache[read];
        if (m.lastSeen == 0 || (now - m.lastSeen > PROBE_INTEL_MAX_AGE_MS)) {
            continue;
        }
        if (write != read) probeVulnCache[write] = m;
        write++;
    }
    for (uint8_t i = write; i < probeVulnCount; i++) {
        memset(&probeVulnCache[i], 0, sizeof(ProbeVulnMatch));
    }
    probeVulnCount = write;
}

// getVulnProbeCount() and getVulnProbeCache() defined in recon.cpp

// ============================================================================
// ==[ FEATURE 7: GHOST NETWORK CANARY ]======================================
// ============================================================================

static void checkCanarySSID() {
    if (canarySSID[0] == '\0') return;
    for (int i = 0; i < wifiAPCount; i++) {
        if (strcmp(wifiSnapshot[i].ssid, canarySSID) == 0) {
            if (!canaryTripped) {
                canaryTripped = true;
                ReconEventData ev = {};
                ev.event = ReconEvent::CANARY_TRIPPED;
                ev.rssi = wifiSnapshot[i].rssi;
                ev.channel = wifiSnapshot[i].channel;
                memcpy(ev.bssid, wifiSnapshot[i].bssid, 6);
                strncpy(ev.ssid, canarySSID, 32);
                ev.ssid[32] = '\0';
                snprintf(ev.detail, sizeof(ev.detail), "PROBE REPLAY!");
                pushEvent(ev);
                HAMLET_LOGF("[RECON] !! CANARY TRIPPED: '%s' ch%d %ddBm !!\n",
                              canarySSID, wifiSnapshot[i].channel, wifiSnapshot[i].rssi);
            }
            return;
        }
    }
}

const char* getCanarySSID() { return canarySSID; }
bool isCanaryTripped() { return canaryTripped; }

void setCanarySSID(const char* ssid) {
    if (!ssid || ssid[0] == '\0') {
        // reset to auto-generated canary from device MAC
        uint8_t mac[6] = {};
#ifndef SIMULATOR
        esp_wifi_get_mac(WIFI_IF_STA, mac);
#endif
        uint32_t seed = fnvHash(mac, 6) ^ 0xDEADCAFE;
        snprintf(canarySSID, 33, "_c%08lX", (unsigned long)seed);
    } else {
        strncpy(canarySSID, ssid, 32);
        canarySSID[32] = '\0';
    }
    canaryTripped = false;  // reset trip state on SSID change
    HAMLET_LOGF("[RECON] canary SSID set: %s\n", canarySSID);
}

// ============================================================================
// ==[ FEATURE 1: PHANTOM PROBE KARMA CONFIRMATION ]==========================
// ============================================================================

bool isKarmaConfirmed() { return karmaConfirmed; }
