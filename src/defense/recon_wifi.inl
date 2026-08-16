/**
 * recon_wifi.inl — WiFi scanning, snapshot analysis, threat detection checks
 *
 * INCLUDED by recon.cpp — not a standalone translation unit.
 * All static state is defined in recon.cpp and visible here.
 */

// ==[ INTERNAL: COMMON SSID CHECK ]==
static bool isCommonSSID(const char* ssid) {
    if (!ssid || !ssid[0]) return true;  // empty = common
    for (int i = 0; i < COMMON_PREFIX_COUNT; i++) {
        if (strncasecmp(ssid, COMMON_SSID_PREFIXES[i], strlen(COMMON_SSID_PREFIXES[i])) == 0) {
            return true;
        }
    }
    return false;
}

// ==[ INTERNAL: COUNT SSID OCCURRENCES ]== >2 with same SSID = likely mesh, not evil twin
static int countSSIDOccurrences(const char* ssid) {
    int count = 0;
    for (int i = 0; i < wifiAPCount; i++) {
        if (strcmp(wifiSnapshot[i].ssid, ssid) == 0) count++;
    }
    return count;
}

// ==[ INTERNAL: WIFI SCAN ]==
// async scan: kick off with scanNetworks(true), poll scanComplete() each frame.
// no blocking, no stutter, no stuck haptic motor.
static bool startWifiScan() {
    lastWifiScanAttempt = millis();
    wifiScanResultCount = 0;
    if (!WiFi.mode(WIFI_STA)) {
        lastWifiScanResult = WIFI_SCAN_FAILED;
        if (wifiScanConsecutiveFailures < UINT16_MAX) wifiScanConsecutiveFailures++;
        HAMLET_LOGLN("[RECON] WiFi STA mode failed; scan skipped");
        return false;
    }
    WiFi.disconnect(false);  // clean state, keep stored creds

    int rc = WiFi.scanNetworks(true, true);  // async=true, show_hidden=true
    if (rc == WIFI_SCAN_FAILED) {
        WiFi.scanDelete();
        WiFi.mode(WIFI_OFF);
        wifiScanStarted = false;
        lastWifiScanResult = WIFI_SCAN_FAILED;
        if (wifiScanConsecutiveFailures < UINT16_MAX) wifiScanConsecutiveFailures++;
        HAMLET_LOGLN("[RECON] WiFi scan start failed");
        return false;
    }

    wifiScanStarted = true;
    lastWifiScanResult = WIFI_SCAN_RUNNING;
    HAMLET_LOGLN("[RECON] WiFi scan started (async)");
    return true;
}

// ==[ FUSE WIFI OBSERVATIONS ]==
// The producer owns radio/API reads. Only this deterministic stage mutates the
// fused Wi-Fi table used by detection and snapshot publication.
static void fuseWifiObservations(const Defense::WifiObservation* observations, int count) {
    openAPCount = 0;
    recentFingerprintMismatchCount = 0;
    recentSeqAnomalyCount = 0;
    recentRssiAnomalyCount = 0;

    for (int i = 0; i < count; i++) {
        WifiAP& ap = wifiSnapshot[i];
        memset(&ap, 0, sizeof(ap));
        if (!observations[i].valid) continue;
        strncpy(ap.ssid, observations[i].ssid, 32);
        ap.ssid[32] = '\0';
        memcpy(ap.bssid, observations[i].bssid, 6);
        ap.rssi = observations[i].rssi;
        ap.channel = observations[i].channel;
        ap.authMode = observations[i].authMode;
        ap.entropyScore = observations[i].entropyScore;
        ap.lat = observations[i].lat;
        ap.lon = observations[i].lon;

        if (ap.authMode == (uint8_t)WIFI_AUTH_OPEN || ap.authMode == (uint8_t)WIFI_AUTH_WEP) {
            openAPCount++;
        }
    }

    syncBLECatalogThreatState();
}

// ==[ POPULATE FROM HUNT ]==
static void populateFromHunt() {
    openAPCount = 0;

    const DetectedNetwork* nets = Hunt::getNetworks();
    int n = Hunt::getNetworkCount();
    int count = (n > MAX_WIFI_SNAPSHOT) ? MAX_WIFI_SNAPSHOT : n;

    for (int i = 0; i < count; i++) {
        WifiAP& ap = wifiSnapshot[i];
        memset(&ap, 0, sizeof(ap));
        strncpy(ap.ssid, nets[i].ssid, 32);
        ap.ssid[32] = '\0';
        memcpy(ap.bssid, nets[i].bssid, 6);
        ap.rssi = nets[i].rssi;
        ap.channel = nets[i].channel;
        ap.authMode = (uint8_t)nets[i].authmode;

        if (GPS::hasFix()) {
            ap.lat = (float)GPS::getLatitude();
            ap.lon = (float)GPS::getLongitude();
        } else {
            ap.lat = 0.0f;
            ap.lon = 0.0f;
        }

        if (ap.authMode == (uint8_t)WIFI_AUTH_OPEN || ap.authMode == (uint8_t)WIFI_AUTH_WEP) {
            openAPCount++;
        }
    }
    wifiAPCount = count;
    harvestHuntForensics();
}

// ==[ WARDRIVE PARASITIC WIFI FEED ]==
// wardrive scans WiFi independently (~1.4s sweep). feed results here for
// evil twin / KARMA / potfile / open AP detection at zero radio cost.
// NOT calling full analyzeSnapshot() — that emits SCAN_COMPLETE events and
// updates lastWifiScanComplete, which would spam mood phrases + terminal
// lines every 1.4s. run threat checks only.
void ingestWardriveSnapshot(const wifi_ap_record_t* records, uint16_t count) {
    if (!wifiSnapshot || !wifiObservationBuffer) return;
    pendingWardriveObservations = true;
    if (!records || count == 0) {
        pendingWardriveObservationCount = 0;
        return;
    }
    pendingWardriveObservationCount = (int)WifiObservationProducer::collectWardrive(
        records, count, wifiObservationBuffer, MAX_WIFI_SNAPSHOT, millis());
}

static void fuseWardriveObservations() {
    if (!pendingWardriveObservations) return;
    pendingWardriveObservations = false;
    if (pendingWardriveObservationCount <= 0) {
        wifiAPCount = 0;
        openAPCount = 0;
        knownAPCount = 0;
        return;
    }

    fuseWifiObservations(wifiObservationBuffer, pendingWardriveObservationCount);
    wifiAPCount = pendingWardriveObservationCount;
    pendingWardriveObservationCount = 0;

    // threat checks only — no SCAN_COMPLETE event, no timing update
    knownAPCount = 0;
    checkEvilTwin();
    checkKarmaHoneypot();
    checkPotfileMatches();
    checkOpenAPs();
    // skip updateKnownProbeIntel — Hunt not running during wardrive
}

static void harvestHuntForensics() {
    recentFingerprintMismatchCount = 0;
    recentSeqAnomalyCount = 0;
    recentRssiAnomalyCount = 0;

    const DetectedNetwork* nets = Hunt::getNetworks();
    int n = Hunt::getNetworkCount();
    if (!nets || n <= 0) return;

    uint32_t now = millis();

    for (int i = 0; i < n; i++) {
        const DetectedNetwork& net = nets[i];
        if (net.bssid[0] == 0 && net.bssid[1] == 0 && net.bssid[2] == 0 &&
            net.bssid[3] == 0 && net.bssid[4] == 0 && net.bssid[5] == 0) {
            continue;
        }

        uint32_t hash = fnvHash(net.bssid, 6);

        if (net.lastFingerprintAnomaly > 0 &&
            now - net.lastFingerprintAnomaly < FINGERPRINT_ACTIVE_MS) {
            if (recentFingerprintMismatchCount < 0xFF) recentFingerprintMismatchCount++;
            if (net.lastFingerprintAnomaly > lastFingerprintTime) {
                lastFingerprintTime = net.lastFingerprintAnomaly;
            }

            if (!isDedupCooldown(dedupFingerprint, dedupFingerprintCount, hash,
                                 FINGERPRINT_EVENT_COOLDOWN_MS)) {
                ReconEventData ev = {};
                ev.event = ReconEvent::FINGERPRINT_MISMATCH;
                ev.rssi = net.rssi;
                ev.channel = net.channel;
                memcpy(ev.bssid, net.bssid, 6);
                strncpy(ev.ssid, net.ssid[0] ? net.ssid : "<hidden>", 32);
                ev.ssid[32] = '\0';
                snprintf(ev.detail, sizeof(ev.detail), "b:%02X%02X%02X",
                         net.bssid[3], net.bssid[4], net.bssid[5]);
                pushEvent(ev);
            }
        }

        if (net.seqAnomalyCount >= 3 &&
            net.lastSeqAnomaly > 0 &&
            now - net.lastSeqAnomaly < SEQ_ACTIVE_MS) {
            if (recentSeqAnomalyCount < 0xFF) recentSeqAnomalyCount++;
            if (net.lastSeqAnomaly > lastSeqTime) {
                lastSeqTime = net.lastSeqAnomaly;
            }

            if (!isDedupCooldown(dedupSeq, dedupSeqCount, hash, SEQ_EVENT_COOLDOWN_MS)) {
                ReconEventData ev = {};
                ev.event = ReconEvent::SEQ_ANOMALY;
                ev.rssi = net.rssi;
                ev.channel = net.channel;
                ev.count = net.seqAnomalyCount;
                memcpy(ev.bssid, net.bssid, 6);
                strncpy(ev.ssid, net.ssid[0] ? net.ssid : "<hidden>", 32);
                ev.ssid[32] = '\0';
                snprintf(ev.detail, sizeof(ev.detail), "b:%02X%02X%02X",
                         net.bssid[3], net.bssid[4], net.bssid[5]);
                pushEvent(ev);
            }
        }

        if (net.rssiAnomalyCount >= 2 &&
            net.lastRssiAnomaly > 0 &&
            now - net.lastRssiAnomaly < RSSI_ACTIVE_MS) {
            if (recentRssiAnomalyCount < 0xFF) recentRssiAnomalyCount++;
            if (net.lastRssiAnomaly > lastRssiTime) {
                lastRssiTime = net.lastRssiAnomaly;
            }

            if (!isDedupCooldown(dedupRssi, dedupRssiCount, hash, RSSI_EVENT_COOLDOWN_MS)) {
                ReconEventData ev = {};
                ev.event = ReconEvent::RSSI_ANOMALY;
                ev.rssi = net.rssi;
                ev.channel = net.channel;
                ev.count = net.rssiAnomalyCount;
                memcpy(ev.bssid, net.bssid, 6);
                strncpy(ev.ssid, net.ssid[0] ? net.ssid : "<hidden>", 32);
                ev.ssid[32] = '\0';
                snprintf(ev.detail, sizeof(ev.detail), "b:%02X%02X%02X",
                         net.bssid[3], net.bssid[4], net.bssid[5]);
                pushEvent(ev);
            }
        }
    }
}

// ==[ SNAPSHOT HASH ]==
static uint32_t computeSnapshotHash() {
    uint32_t h = 0x811c9dc5;
    for (int i = 0; i < wifiAPCount; i++) {
        for (int j = 0; j < 6; j++) h = (h ^ wifiSnapshot[i].bssid[j]) * 0x01000193;
        for (int j = 0; j < 33 && wifiSnapshot[i].ssid[j]; j++) h = (h ^ wifiSnapshot[i].ssid[j]) * 0x01000193;
    }
    return h;
}

// ==[ ANALYZE SNAPSHOT ]==
static void analyzeSnapshot() {
    knownAPCount = 0;
    sweepAllDedupTables();           // reclaim expired dedup slots before analysis

    checkCanarySSID();               // Feature 7: ghost canary
    checkPhantomProbeConfirmation(); // Feature 1: KARMA confirmation
    checkEvilTwin();
    checkKarmaHoneypot();
    checkPotfileMatches();
    checkOpenAPs();
    updateKnownProbeIntel(true);

    ReconEventData ev = {};
    ev.event = ReconEvent::SCAN_COMPLETE;
    ev.count = (uint8_t)wifiAPCount;
    snprintf(ev.detail, sizeof(ev.detail), "open:%d known:%d", openAPCount, knownAPCount);
    pushEvent(ev);

    lastWifiScanComplete = millis();
}

static void processWifiResults() {
    const int produced = (int)WifiObservationProducer::collectActiveScan(
        wifiObservationBuffer, MAX_WIFI_SNAPSHOT, wifiScanResultCount, millis());
    fuseWifiObservations(wifiObservationBuffer, produced);
    wifiAPCount = produced;
    analyzeSnapshot();
}

// ==[ EVIL TWIN DETECTION ]== same SSID, different BSSID
// filters: skip common SSIDs, skip >2 APs with same SSID (mesh), same channel,
// at least one AP nearby. Twins operate on the target's channel to hijack
// probes/auth; legit dual-radio setups split across bands or 1/6/11.
static void checkEvilTwin() {
    for (int i = 0; i < wifiAPCount; i++) {
        if (wifiSnapshot[i].ssid[0] == '\0') continue;
        if (isCommonSSID(wifiSnapshot[i].ssid)) continue;

        // skip mesh/enterprise (>2 APs with same SSID is intentional multi-AP)
        if (countSSIDOccurrences(wifiSnapshot[i].ssid) > 2) continue;

        for (int j = i + 1; j < wifiAPCount; j++) {
            if (strcmp(wifiSnapshot[i].ssid, wifiSnapshot[j].ssid) == 0 &&
                memcmp(wifiSnapshot[i].bssid, wifiSnapshot[j].bssid, 6) != 0) {

                // same channel — legit dual-AP deployments (router+extender,
                // dual-band) almost always split channels; a co-channel twin
                // is the signature of an intercept attack.
                if (wifiSnapshot[i].channel != wifiSnapshot[j].channel) continue;

                // at least one AP within ~-70dBm. A weak-on-both pair is more
                // likely SSID collision with a neighbor than a local attacker;
                // an attack-capable twin must be close enough to beat the real
                // AP's signal for the victim's association.
                if (wifiSnapshot[i].rssi < -70 && wifiSnapshot[j].rssi < -70) continue;

                // dedup: skip if reported recently
                uint32_t hash = fnvHash(wifiSnapshot[i].ssid, strlen(wifiSnapshot[i].ssid));
                if (isDedupCooldown(dedupEvilTwin, dedupEvilTwinCount, hash, EVIL_TWIN_COOLDOWN_MS)) continue;

                ReconEventData ev = {};
                ev.event = ReconEvent::EVIL_TWIN;
                strncpy(ev.ssid, wifiSnapshot[i].ssid, 32);
                ev.ssid[32] = '\0';
                ev.rssi = wifiSnapshot[i].rssi;
                ev.channel = wifiSnapshot[i].channel;
                pushEvent(ev);
                lastEvilTwinTime = millis();
                HAMLET_LOGF("[RECON] EVIL TWIN: '%s' on 2 BSSIDs\n", ev.ssid);
                return;  // one per scan cycle
            }
        }
    }
}

// ==[ KARMA HONEYPOT DETECTION ]== same BSSID, multiple SSIDs
// filter: require 3+ SSIDs from same BSSID (2 is normal guest+primary)
static void checkKarmaHoneypot() {
    for (int i = 0; i < wifiAPCount; i++) {
        // count unique SSIDs for this BSSID
        int ssidCount = 1;
        int firstOther = -1;
        for (int j = i + 1; j < wifiAPCount; j++) {
            if (memcmp(wifiSnapshot[i].bssid, wifiSnapshot[j].bssid, 6) == 0 &&
                strcmp(wifiSnapshot[i].ssid, wifiSnapshot[j].ssid) != 0) {
                ssidCount++;
                if (firstOther < 0) firstOther = j;
            }
        }

        // 2 SSIDs = normal dual-band AP. 3+ = suspicious KARMA behavior.
        if (ssidCount >= 3 && firstOther >= 0) {
            uint32_t hash = fnvHash(wifiSnapshot[i].bssid, 6);
            if (isDedupCooldown(dedupKarma, dedupKarmaCount, hash, KARMA_COOLDOWN_MS)) continue;

            ReconEventData ev = {};
            ev.event = ReconEvent::KARMA_HONEYPOT;
            strncpy(ev.ssid, wifiSnapshot[i].ssid, 32);
            ev.ssid[32] = '\0';
            snprintf(ev.detail, sizeof(ev.detail), "+%d SSIDs", ssidCount - 1);
            ev.rssi = wifiSnapshot[i].rssi;
            pushEvent(ev);
            lastKarmaTime = millis();
            HAMLET_LOGF("[RECON] KARMA: BSSID claims %d SSIDs\n", ssidCount);
            // Feature 1: inject phantom probe to confirm KARMA
            triggerPhantomProbe(wifiSnapshot[i].bssid);
            return;  // one per scan cycle
        }
    }
}

// ==[ POTFILE MATCH ]==
static void checkPotfileMatches() {
    knownAPCount = 0;
    uint8_t newlyKnownCount = 0;
    int8_t strongestNewRSSI = -127;
    uint8_t strongestNewChannel = 0;
    char firstNewSSID[33] = {0};

    for (int i = 0; i < wifiAPCount; i++) {
        if (Potfile::isKnown(wifiSnapshot[i].ssid)) {
            knownAPCount++;

            // Dedup per SSID across session. Aggregate to one event to avoid queue flooding.
            uint32_t hash = fnvHash(wifiSnapshot[i].ssid, strlen(wifiSnapshot[i].ssid));
            if (isDedupCooldown(dedupKnownAP, dedupKnownAPCount, hash, KNOWN_AP_COOLDOWN_MS)) continue;
            if (newlyKnownCount == 0) {
                strncpy(firstNewSSID, wifiSnapshot[i].ssid, 32);
                firstNewSSID[32] = '\0';
            }
            if (wifiSnapshot[i].rssi > strongestNewRSSI) {
                strongestNewRSSI = wifiSnapshot[i].rssi;
                strongestNewChannel = wifiSnapshot[i].channel;
            }
            if (newlyKnownCount < 0xFF) newlyKnownCount++;
        }
    }

    if (newlyKnownCount > 0) {
        ReconEventData ev = {};
        ev.event = ReconEvent::KNOWN_AP;
        ev.count = newlyKnownCount;
        ev.rssi = strongestNewRSSI;
        ev.channel = strongestNewChannel;
        strncpy(ev.ssid, firstNewSSID, 32);
        ev.ssid[32] = '\0';
        if (newlyKnownCount > 1) {
            snprintf(ev.detail, sizeof(ev.detail), "+%u more", (unsigned)(newlyKnownCount - 1));
        }
        pushEvent(ev);
    }
}

// ==[ OPEN AP WARNING ]==
static void checkOpenAPs() {
    if (openAPCount >= 3) {
        uint32_t now = millis();
        if (lastOpenAPAlert != 0 && (now - lastOpenAPAlert < OPEN_AP_COOLDOWN_MS)) return;
        lastOpenAPAlert = now;

        ReconEventData ev = {};
        ev.event = ReconEvent::OPEN_AP_WARNING;
        ev.count = (uint8_t)openAPCount;
        pushEvent(ev);
    }
}

// ==[ KNOWN PROBE INTEL ]==
// Cross-reference recently harvested probe requests with potfile SSIDs.
// This highlights nearby clients likely searching for previously compromised networks.
static void updateKnownProbeIntel(bool emitEvent) {
    knownProbeReqCount = 0;
    knownProbeClientCount = 0;

    const Hunt::HarvestedProbe* probes = Hunt::getHarvestedProbes();
    uint16_t harvested = Hunt::getHarvestedCount();
    if (!probes || harvested == 0) {
        lastProbeVulnAlertCount = 0;
        return;
    }

    uint32_t now = millis();
    int8_t strongest = -127;
    char firstSSID[33] = {0};

    static constexpr uint8_t MAX_UNIQUE_PROBE_CLIENTS = 32;
    uint8_t uniqueClients[MAX_UNIQUE_PROBE_CLIENTS][6];
    uint8_t uniqueCount = 0;

    for (uint16_t i = 0; i < harvested; i++) {
        const Hunt::HarvestedProbe& hp = probes[i];
        if (!hp.ssid[0]) continue;
        if (hp.lastSeen > 0 && (now - hp.lastSeen > PROBE_INTEL_MAX_AGE_MS)) continue;
        if (!Potfile::isKnown(hp.ssid)) continue;

        if (knownProbeReqCount < 0xFFFF) knownProbeReqCount++;
        if (!firstSSID[0]) {
            strncpy(firstSSID, hp.ssid, 32);
            firstSSID[32] = '\0';
        }
        if (hp.rssi > strongest) strongest = hp.rssi;

        bool seen = false;
        for (uint8_t c = 0; c < uniqueCount; c++) {
            if (memcmp(uniqueClients[c], hp.clientMac, 6) == 0) {
                seen = true;
                break;
            }
        }
        if (!seen && uniqueCount < MAX_UNIQUE_PROBE_CLIENTS) {
            memcpy(uniqueClients[uniqueCount], hp.clientMac, 6);
            uniqueCount++;
        }
    }

    knownProbeClientCount = uniqueCount;
    if (knownProbeReqCount == 0) {
        lastProbeVulnAlertCount = 0;
        return;
    }
    if (!emitEvent) return;

    bool shouldAlert = false;
    if (lastProbeVulnAlert == 0) {
        shouldAlert = true;
    } else if (knownProbeReqCount > lastProbeVulnAlertCount) {
        shouldAlert = true;
    } else if (now - lastProbeVulnAlert >= PROBE_VULN_COOLDOWN_MS) {
        shouldAlert = true;
    }
    if (!shouldAlert) return;

    ReconEventData ev = {};
    ev.event = ReconEvent::PROBE_VULN_CLIENT;
    ev.count = (knownProbeReqCount > 255) ? 255 : (uint8_t)knownProbeReqCount;
    ev.rssi = strongest;
    strncpy(ev.ssid, firstSSID, 32);
    ev.ssid[32] = '\0';
    snprintf(ev.detail, sizeof(ev.detail), "req:%u cli:%u",
             (unsigned)knownProbeReqCount, (unsigned)knownProbeClientCount);
    pushEvent(ev);

    lastProbeVulnAlert = now;
    lastProbeVulnAlertCount = knownProbeReqCount;
}
