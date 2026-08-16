/**
 * recon_deauth.inl — deauth sniffing, tool classification, probe fingerprinting
 *
 * INCLUDED by recon.cpp — not a standalone translation unit.
 * All static state is defined in recon.cpp and visible here.
 */

// ==[ DEAUTH SNIFF ]== lightweight promiscuous listen between scans
// callback runs in WiFi driver task — must be non-blocking
// IRAM_ATTR removed to save ~300 bytes IRAM — flash exec fine for passive sniffing
static void deauthSniffCB(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_MGMT) return;
    const wifi_promiscuous_pkt_t* pkt = (wifi_promiscuous_pkt_t*)buf;
    const uint8_t* payload = pkt->payload;
    uint16_t sigLen = pkt->rx_ctrl.sig_len;
    if (sigLen < 24) return;

    uint8_t subtype = (payload[0] >> 4) & 0x0F;

    // ==[ FEATURE 3: BEACON IE CAPTURE ]== parse IE chain from beacon frames
    if (subtype == 0x08 && beaconCaptureBuf && sigLen > 36) {
        const uint8_t* bssidAddr = payload + 16;  // BSSID = addr3
        // BSSID dedup — every beacon from the same AP carries the same IE chain;
        // burning a fresh slot per beacon lets the loudest AP exhaust the 32-slot
        // buffer and starve rarer APs. Scan what we've already got first.
        int curCount = beaconCaptureCount.load(std::memory_order_acquire);
        if (curCount > MAX_BEACON_CAPTURES) curCount = MAX_BEACON_CAPTURES;
        for (int i = 0; i < curCount; i++) {
            if (beaconCaptureBuf[i].valid &&
                memcmp(beaconCaptureBuf[i].bssid, bssidAddr, 6) == 0) {
                return;  // already captured this BSSID
            }
        }
        int slot = beaconCaptureCount.load(std::memory_order_relaxed);
        if (slot < MAX_BEACON_CAPTURES) {
            slot = beaconCaptureCount.fetch_add(1, std::memory_order_acq_rel);
            if (slot < MAX_BEACON_CAPTURES) {
                BeaconIECapture& bc = beaconCaptureBuf[slot];
                memcpy(bc.bssid, bssidAddr, 6);
                bc.uniqueIETypes = 0;
                bc.vendorIECount = 0;
                bc.valid = true;
                // IE chain starts at offset 36 (24 hdr + 12 fixed fields)
                uint8_t ieTypeSeen[32] = {};  // bitmap for 256 IE types
                int pos = 36;
                while (pos + 1 < (int)sigLen) {
                    uint8_t ieType = payload[pos];
                    uint8_t ieLen = payload[pos + 1];
                    if (pos + 2 + ieLen > (int)sigLen) break;
                    // track unique IE types via bitmap
                    uint8_t byteIdx = ieType >> 3;
                    uint8_t bitMask = 1 << (ieType & 7);
                    if (!(ieTypeSeen[byteIdx] & bitMask)) {
                        ieTypeSeen[byteIdx] |= bitMask;
                        bc.uniqueIETypes++;
                    }
                    if (ieType == 0xDD) bc.vendorIECount++;
                    pos += 2 + ieLen;
                }
            } else {
                beaconCaptureCount.fetch_sub(1, std::memory_order_relaxed);
            }
        }
        return;
    }

    // ==[ FEATURE 5: PROBE REQUEST FINGERPRINT ]== parse IEs from probe requests
    if (subtype == 0x04 && probeCaptureBuf && sigLen > 24) {
        int slot = probeCaptureCount.load(std::memory_order_relaxed);
        if (slot < MAX_PROBE_CAPTURES) {
            slot = probeCaptureCount.fetch_add(1, std::memory_order_acq_rel);
            if (slot < MAX_PROBE_CAPTURES) {
                ProbeCapture& pc = probeCaptureBuf[slot];
                memcpy(pc.clientMac, payload + 10, 6);  // SA = addr2
                pc.rssi = pkt->rx_ctrl.rssi;
                pc.channel = pkt->rx_ctrl.channel;
                pc.ieCount = 0;
                pc.hasHTCaps = false;
                pc.hasVHTCaps = false;
                pc.hasExtCaps = false;
                pc.hasWPS = false;
                pc.valid = true;
                // IE chain starts at offset 24 (no fixed fields in probe req)
                uint32_t h = 0x811c9dc5;
                int pos = 24;
                while (pos + 1 < (int)sigLen) {
                    uint8_t ieType = payload[pos];
                    uint8_t ieLen = payload[pos + 1];
                    if (pos + 2 + ieLen > (int)sigLen) break;
                    pc.ieCount++;
                    h = (h ^ ieType) * 0x01000193;  // FNV-1a of IE type chain
                    if (ieType == 0x2D) pc.hasHTCaps = true;
                    if (ieType == 0xBF) pc.hasVHTCaps = true;
                    if (ieType == 0x7F) pc.hasExtCaps = true;
                    if (ieType == 0xDD && ieLen >= 4 &&
                        payload[pos+2]==0x00 && payload[pos+3]==0x50 &&
                        payload[pos+4]==0xF2 && payload[pos+5]==0x04) {
                        pc.hasWPS = true;  // WPS IE (OUI: 00:50:F2:04)
                    }
                    pos += 2 + ieLen;
                }
                pc.ieHash = h;
            } else {
                probeCaptureCount.fetch_sub(1, std::memory_order_relaxed);
            }
        }
        return;
    }

    // 0x0C = deauth, 0x0A = disassoc
    if (subtype != 0x0C && subtype != 0x0A) return;

    sniffHits.fetch_add(1, std::memory_order_relaxed);
    if (subtype == 0x0C) sniffSubtypeDeauth.fetch_add(1, std::memory_order_relaxed);
    else if (subtype == 0x0A) sniffSubtypeDisassoc.fetch_add(1, std::memory_order_relaxed);

    // RSSI/channel pair update: use CAS loop to atomically update both
    int8_t rssi = pkt->rx_ctrl.rssi;
    int8_t oldRSSI = sniffPeakRSSI.load(std::memory_order_relaxed);
    while (rssi > oldRSSI) {
        if (sniffPeakRSSI.compare_exchange_weak(oldRSSI, rssi, std::memory_order_relaxed)) {
            sniffPeakChan.store(pkt->rx_ctrl.channel, std::memory_order_relaxed);
            break;
        }
    }

    // Channel hits, source tracking, target BSSID, and reason code: protected by spinlock
    uint8_t ch = pkt->rx_ctrl.channel;
    taskENTER_CRITICAL_ISR(&sniffMux);
    if (ch >= 1 && ch <= 13) {
        if (sniffChannelHits[ch] < 0xFFFF) sniffChannelHits[ch]++;
    }

    // Track distinct deauth/disassoc transmitters (SA/addr2) for attribution quality.
    const uint8_t* srcMac = payload + 10;
    uint8_t srcCount = sniffSourceCount;
    bool foundSrc = false;
    for (uint8_t i = 0; i < srcCount; i++) {
        if (memcmp(sniffSources[i].mac, srcMac, 6) == 0) {
            if (sniffSources[i].hits < 0xFFFF) sniffSources[i].hits++;
            foundSrc = true;
            break;
        }
    }
    if (!foundSrc && srcCount < MAX_DEAUTH_SOURCES) {
        memcpy(sniffSources[srcCount].mac, srcMac, 6);
        sniffSources[srcCount].hits = 1;
        sniffSourceCount = srcCount + 1;
    }

    // target BSSID (addr1): store first seen in burst
    const uint8_t* targetBssid = payload + 4;
    if (sniffTargetBssid[0] == 0 && sniffTargetBssid[1] == 0 &&
        sniffTargetBssid[2] == 0 && sniffTargetBssid[3] == 0) {
        memcpy(sniffTargetBssid, targetBssid, 6);
    }

    // reason code: payload[24:25] (LE, after 24-byte mgmt header)
    if (sigLen >= 26) {
        uint16_t reason = payload[24] | (payload[25] << 8);
        if (reason > 0) {
            if (reason == sniffReasonCode) {
                if (sniffReasonHits < 0xFFFF) sniffReasonHits++;
            } else if (sniffReasonHits == 0) {
                sniffReasonCode = reason;
                sniffReasonHits = 1;
            }
        }
    }
    taskEXIT_CRITICAL_ISR(&sniffMux);
}

static bool startDeauthSniff(uint32_t durationMs, bool sentinel, uint8_t fixedChannel) {
    sniffHits.store(0, std::memory_order_relaxed);
    sniffPeakRSSI.store(-127, std::memory_order_relaxed);
    sniffPeakChan.store(0, std::memory_order_relaxed);
    sniffSubtypeDeauth.store(0, std::memory_order_relaxed);
    sniffSubtypeDisassoc.store(0, std::memory_order_relaxed);
    taskENTER_CRITICAL(&sniffMux);
    memset(sniffChannelHits, 0, sizeof(sniffChannelHits));
    sniffSourceCount = 0;
    memset(sniffSources, 0, sizeof(sniffSources));
    memset(sniffTargetBssid, 0, 6);
    sniffReasonCode = 0;
    sniffReasonHits = 0;
    taskEXIT_CRITICAL(&sniffMux);
    deauthSniffDurationMs = durationMs;
    deauthSniffSentinel = sentinel;
    deauthSniffFixedChannel = fixedChannel;
    if (sentinel) lastSentinelSniff = millis();
    // reset advanced capture buffers
    beaconCaptureCount.store(0, std::memory_order_relaxed);
    probeCaptureCount.store(0, std::memory_order_relaxed);

    if (!WiFi.mode(WIFI_STA)) {
        HAMLET_LOGLN("[RECON] WiFi STA mode failed; deauth sniff skipped");
        return false;
    }

    esp_err_t err = esp_wifi_set_promiscuous_rx_cb(deauthSniffCB);
    if (err != ESP_OK) {
        HAMLET_LOGF("[RECON] deauth cb failed: %s\n", esp_err_to_name(err));
        WiFi.mode(WIFI_OFF);
        return false;
    }
    wifi_promiscuous_filter_t filter = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT
    };
    err = esp_wifi_set_promiscuous_filter(&filter);
    if (err != ESP_OK) {
        HAMLET_LOGF("[RECON] deauth filter failed: %s\n", esp_err_to_name(err));
        esp_wifi_set_promiscuous_rx_cb(NULL);
        WiFi.mode(WIFI_OFF);
        return false;
    }

    err = esp_wifi_set_promiscuous(true);
    if (err != ESP_OK) {
        HAMLET_LOGF("[RECON] deauth promisc failed: %s\n", esp_err_to_name(err));
        esp_wifi_set_promiscuous_rx_cb(NULL);
        WiFi.mode(WIFI_OFF);
        return false;
    }

    err = esp_wifi_set_channel((fixedChannel > 0) ? fixedChannel : 1, WIFI_SECOND_CHAN_NONE);
    if (err != ESP_OK) {
        HAMLET_LOGF("[RECON] deauth channel failed: %s\n", esp_err_to_name(err));
        esp_wifi_set_promiscuous(false);
        esp_wifi_set_promiscuous_rx_cb(NULL);
        WiFi.mode(WIFI_OFF);
        return false;
    }

    HAMLET_LOGF("[RECON] deauth sniff started (%s %lums ch%d)\n",
                  sentinel ? "sentinel" : "full",
                  (unsigned long)durationMs,
                  (fixedChannel > 0) ? fixedChannel : 0);
    return true;
}

static bool stopDeauthSniff() {
    // stop promiscuous but keep WiFi alive for HOGWASH injection
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(NULL);

    // harvest results
    bool chaffFired = false;
    uint8_t dominantChan = 0;
    uint8_t burstTargetBssid[6] = {};
    uint16_t sniffHitsCopy = sniffHits.load(std::memory_order_relaxed);
    if (sniffHitsCopy > 0) {
        taskENTER_CRITICAL(&sniffMux);
        uint16_t dominantHits = 0;
        for (uint8_t ch = 1; ch <= 13; ch++) {
            uint16_t hits = sniffChannelHits[ch];
            if (hits > dominantHits) {
                dominantHits = hits;
                dominantChan = ch;
            }
        }
        uint8_t localSourceCount = sniffSourceCount;
        uint16_t localReasonCode = sniffReasonCode;
        uint16_t localReasonHits = sniffReasonHits;
        memcpy(burstTargetBssid, sniffTargetBssid, 6);
        taskEXIT_CRITICAL(&sniffMux);

        if (dominantChan == 0) dominantChan = sniffPeakChan.load(std::memory_order_relaxed);

        deauthTotal += sniffHitsCopy;
        deauthLastTime = millis();
        deauthLastBurstCount = sniffHitsCopy;
        deauthLastUniqueSources = localSourceCount;
        deauthLastPeakRSSI = sniffPeakRSSI.load(std::memory_order_relaxed);
        deauthLastPeakChan = sniffPeakChan.load(std::memory_order_relaxed);
        deauthLastDominantChan = dominantChan;
        uint32_t sniffMs = (deauthSniffDurationMs == 0) ? DEAUTH_SNIFF_MS : deauthSniffDurationMs;
        { uint32_t pps32 = (sniffHitsCopy * 1000u + (sniffMs / 2u)) / sniffMs;
          deauthLastPPS = (pps32 > UINT16_MAX) ? UINT16_MAX : (uint16_t)pps32; }
        deauthLastSubtypeDeauth = sniffSubtypeDeauth.load(std::memory_order_relaxed);
        deauthLastSubtypeDisassoc = sniffSubtypeDisassoc.load(std::memory_order_relaxed);
        deauthLastOrigin = DeauthSourceOrigin::RECON_SNIFF;
        HAMLET_LOGF("[RECON] sniff: %d deauth, %ddBm ch%d\n",
                     sniffHitsCopy, sniffPeakRSSI.load(std::memory_order_relaxed), sniffPeakChan.load(std::memory_order_relaxed));

        // emit event for narrator/terminal
        ReconEventData ev = {};
        ev.event = ReconEvent::DEAUTH_DETECTED;
        ev.rssi = sniffPeakRSSI.load(std::memory_order_relaxed);
        ev.channel = dominantChan;
        ev.count = (sniffHitsCopy > 255) ? 255 : (uint8_t)sniffHitsCopy;
        memcpy(ev.bssid, burstTargetBssid, 6);
        snprintf(ev.detail, sizeof(ev.detail), "%s s:%u p:%u d:%u a:%u",
                 deauthSniffSentinel ? "sn" : "fs",
                 (unsigned)deauthLastUniqueSources,
                 (unsigned)deauthLastPPS,
                 (unsigned)deauthLastSubtypeDeauth,
                 (unsigned)deauthLastSubtypeDisassoc);
        pushEvent(ev);
        lastDeauthEventReported = deauthLastTime;

        // ==[ SNIFF BURST HISTORY ]== append forensic record (parity with Hunt path)
        if (deauthHistory) {
            DeauthBurstRecord& rec = deauthHistory[deauthHistoryHead];
            rec.timestamp = deauthLastTime;
            rec.frameCount = deauthLastBurstCount;
            rec.pps = deauthLastPPS;
            rec.dominantChannel = dominantChan;
            rec.peakRSSI = deauthLastPeakRSSI;
            rec.uniqueSources = deauthLastUniqueSources;
            rec.huntChannelMatch = 0;
            if (Hunt::isActive()) {
                rec.huntChannelMatch = (dominantChan == Hunt::getCurrentChannel()) ? 1 : 0;
            }
            memcpy(rec.targetBssid, burstTargetBssid, 6);
            rec.dominantReason = localReasonCode;
            deauthHistoryHead = (deauthHistoryHead + 1) % MAX_DEAUTH_HISTORY;
            if (deauthHistoryCount < MAX_DEAUTH_HISTORY) deauthHistoryCount++;
        }

        // ==[ HOGWASH ]== inject fake handshakes while radio is still hot
        if (Config::getWifiChaffEnabled() && dominantChan > 0) {
            uint32_t prevBursts = WifiChaff::getTotalBursts();
            wifiChaff_injectBurst(burstTargetBssid, dominantChan, sniffHitsCopy);
            chaffFired = (WifiChaff::getTotalBursts() > prevBursts);
            if (chaffFired) {
                // save hold state — caller may enter HOGWASH_HOLD
                hogwashHoldChannel = dominantChan;
                memcpy(hogwashHoldBssid, burstTargetBssid, 6);
                hogwashLastInjectTime = millis();
                hogwashHoldNewHits = 0;
            }
        }
    }

    // ==[ FEATURE 6: TOOL CLASSIFICATION ]== classify attacker tool from burst behavior
    if (sniffHitsCopy >= 3) {
        lastDeauthTool = classifyDeauthTool(
            sniffHitsCopy, deauthLastPPS, deauthLastUniqueSources,
            deauthLastSubtypeDeauth, deauthLastSubtypeDisassoc,
            sniffReasonCode);
        if (lastDeauthTool != DeauthTool::UNKNOWN) {
            ReconEventData toolEv = {};
            toolEv.event = ReconEvent::TOOL_IDENTIFIED;
            toolEv.rssi = deauthLastPeakRSSI;
            toolEv.channel = deauthLastDominantChan;
            memcpy(toolEv.bssid, burstTargetBssid, 6);
            snprintf(toolEv.detail, sizeof(toolEv.detail), "%s %upps s:%u r:%u",
                     deauthToolLabel(lastDeauthTool), (unsigned)deauthLastPPS,
                     (unsigned)deauthLastUniqueSources, (unsigned)sniffReasonCode);
            pushEvent(toolEv);
            HAMLET_LOGF("[RECON] TOOL ID: %s\n", toolEv.detail);

            // ==[ CROSS-CORRELATE ]== stamp hostile clients on the same channel
            // with the identified tool signature. Recent high-hostility clients
            // on the deauth channel are likely operating the classified tool.
            uint32_t now = millis();
            for (int fi = 0; fi < clientFingerprintCount; fi++) {
                ClientFingerprint& cf = clientFingerprints[fi];
                if (cf.channel == deauthLastDominantChan &&
                    cf.hostileScore >= 50 &&
                    (now - cf.lastSeen) < 30000) {  // seen within last 30s
                    cf.toolSignature = (uint8_t)lastDeauthTool;
                    HAMLET_LOGF("[RECON] correlated %s → %02X:%02X:%02X\n",
                                  deauthToolLabel(lastDeauthTool),
                                  cf.clientMac[3], cf.clientMac[4], cf.clientMac[5]);
                }
            }
        }
    }

    // ==[ FEATURE 3+5: PROCESS SNIFF-SIDE CAPTURES ]==
    processBeaconCaptures();
    processProbeCaptures();

    if (!chaffFired) {
        // no hold — kill the radio now
        esp_wifi_stop();
    }
    // if chaffFired, caller keeps WiFi alive for HOGWASH_HOLD

    deauthSniffSentinel = false;
    deauthSniffFixedChannel = 0;
    deauthSniffDurationMs = DEAUTH_SNIFF_MS;
    return chaffFired;
}

static void resetDeauthObservationState() {
    deauthTotal = 0;
    deauthLastTime = 0;
    deauthLastBurstCount = 0;
    deauthLastUniqueSources = 0;
    deauthLastPeakRSSI = -127;
    deauthLastPeakChan = 0;
    deauthLastDominantChan = 0;
    deauthLastPPS = 0;
    deauthLastSubtypeDeauth = 0;
    deauthLastSubtypeDisassoc = 0;
    deauthLastOrigin = DeauthSourceOrigin::RECON_SNIFF;
    lastDeauthEventReported = 0;
    sniffHits.store(0, std::memory_order_relaxed);
    sniffPeakRSSI.store(-127, std::memory_order_relaxed);
    sniffPeakChan.store(0, std::memory_order_relaxed);
    sniffSubtypeDeauth.store(0, std::memory_order_relaxed);
    sniffSubtypeDisassoc.store(0, std::memory_order_relaxed);
    taskENTER_CRITICAL(&sniffMux);
    memset(sniffChannelHits, 0, sizeof(sniffChannelHits));
    sniffSourceCount = 0;
    memset(sniffSources, 0, sizeof(sniffSources));
    memset(sniffTargetBssid, 0, 6);
    sniffReasonCode = 0;
    sniffReasonHits = 0;
    taskEXIT_CRITICAL(&sniffMux);
    huntDeauthWindowStart = 0;
    huntDeauthLastObs = 0;
    huntDeauthHits = 0;
    huntDeauthPeakRSSI = -127;
    huntDeauthPeakChan = 0;
    huntDeauthSubtypeDeauth = 0;
    huntDeauthSubtypeDisassoc = 0;
    huntDeauthSourceCount = 0;
    huntDeauthEventPending = false;
    memset((void*)huntDeauthSources, 0, sizeof(huntDeauthSources));
    memset((void*)huntDeauthTargetBssid, 0, 6);
    huntDeauthReasonCode = 0;
    huntDeauthReasonHits = 0;
}

static void emitPendingDeauthEvent() {
    if (Hunt::isActive()) return;
    if (!huntDeauthEventPending) return;
    // Spectrum promisc path only — hunt ignores deauth frames
    if (deauthLastOrigin != DeauthSourceOrigin::SPECTRUM_CALLBACK) return;
    if (deauthLastTime == 0 || deauthLastTime == lastDeauthEventReported) {
        huntDeauthEventPending = false;
        return;
    }

    ReconEventData ev = {};
    ev.event = ReconEvent::DEAUTH_DETECTED;
    ev.rssi = deauthLastPeakRSSI;
    ev.channel = deauthLastDominantChan ? deauthLastDominantChan : deauthLastPeakChan;
    ev.count = (deauthLastBurstCount > 255) ? 255 : (uint8_t)deauthLastBurstCount;
    memcpy(ev.bssid, (const void*)huntDeauthTargetBssid, 6);
    snprintf(ev.detail, sizeof(ev.detail), "cb s:%u p:%u d:%u a:%u",
             (unsigned)deauthLastUniqueSources,
             (unsigned)deauthLastPPS,
             (unsigned)deauthLastSubtypeDeauth,
             (unsigned)deauthLastSubtypeDisassoc);
    pushEvent(ev);
    lastDeauthEventReported = deauthLastTime;
    huntDeauthEventPending = false;

    // ==[ DEAUTH BURST HISTORY ]== append forensic record
    if (deauthHistory) {
        DeauthBurstRecord& rec = deauthHistory[deauthHistoryHead];
        rec.timestamp = deauthLastTime;
        rec.frameCount = deauthLastBurstCount;
        rec.pps = deauthLastPPS;
        rec.dominantChannel = deauthLastDominantChan ? deauthLastDominantChan : deauthLastPeakChan;
        rec.peakRSSI = deauthLastPeakRSSI;
        rec.uniqueSources = deauthLastUniqueSources;
        rec.huntChannelMatch = 0;
        memcpy(rec.targetBssid, (const void*)huntDeauthTargetBssid, 6);
        rec.dominantReason = huntDeauthReasonCode;
        deauthHistoryHead = (deauthHistoryHead + 1) % MAX_DEAUTH_HISTORY;
        if (deauthHistoryCount < MAX_DEAUTH_HISTORY) deauthHistoryCount++;
    }

    // ==[ HOGWASH ]== queue fake handshakes (deferred — Hunt WiFi is up)
    wifiChaff_onDeauthDetected((const uint8_t*)huntDeauthTargetBssid, ev.channel,
                                deauthLastBurstCount, deauthLastPeakRSSI);
}

static void checkAttackCorrelation(uint32_t now) {
    if (Hunt::isActive()) return;  // hunt TX pollutes deauth correlation
    if (!isDeauthActive() && !isEvilTwinActive()) return;  // no WiFi attack = nothing to correlate

    bool flipperPresent = false;
    bool spamPresent = false;
    bool stalkerPresent = false;

    for (int i = 0; i < trackerCount; i++) {
        const TrackerEntry& te = trackerTable[i];
        if (now - te.lastSeen > CORREL_WINDOW_MS) continue;
        if (te.type == ThreatType::FLIPPER) flipperPresent = true;
        if (te.flags & FLAG_SPAM) spamPresent = true;
        if (te.flags & FLAG_FOLLOWING) stalkerPresent = true;
    }

    auto emitCorrel = [&](uint32_t& lastTime, const char* tag) {
        if (now - lastTime < CORREL_COOLDOWN_MS) return;
        ReconEventData ev = {};
        ev.event = ReconEvent::COORDINATED_ATTACK;
        ev.rssi = deauthLastPeakRSSI;
        ev.channel = deauthLastPeakChan;
        snprintf(ev.detail, sizeof(ev.detail), "%s", tag);
        pushEvent(ev);
        lastTime = now;
        HAMLET_LOGF("[RECON] CORREL: %s\n", tag);
    };

    if (flipperPresent && isDeauthActive())
        emitCorrel(lastCorrelFlipDth, "FLIP+DTH");
    if (spamPresent && isDeauthActive())
        emitCorrel(lastCorrelSpamDth, "SPAM+DTH");
    if (spamPresent && isEvilTwinActive())
        emitCorrel(lastCorrelSpamTwn, "SPAM+TWN");
    if (stalkerPresent && isDeauthActive())
        emitCorrel(lastCorrelStlkDth, "STLK+DTH");
}

// ==[ FEATURE 1: PHANTOM PROBE KARMA CONFIRMATION ]===
// Build and inject a raw 802.11 probe request frame for bait SSID.
// WiFi must be in STA or promiscuous mode.
static void injectPhantomProbe(const char* ssid) {
    if (!ssid || !ssid[0]) return;
    uint8_t ssidLen = (uint8_t)strlen(ssid);
    if (ssidLen > 32) ssidLen = 32;

    // 802.11 probe request: 24-byte header + tagged params
    uint8_t frame[24 + 2 + 32 + 4] = {};  // header + SSID tag + rates tag
    // frame control: probe request (0x0040)
    frame[0] = 0x40;
    frame[1] = 0x00;
    // duration
    frame[2] = 0x00;  frame[3] = 0x00;
    // DA: broadcast
    memset(&frame[4], 0xFF, 6);
    // SA: use device MAC
    esp_wifi_get_mac(WIFI_IF_STA, &frame[10]);
    // BSSID: broadcast
    memset(&frame[16], 0xFF, 6);
    // seq ctrl
    frame[22] = 0x00;  frame[23] = 0x00;

    // SSID tag
    int pos = 24;
    frame[pos++] = 0x00;       // tag: SSID
    frame[pos++] = ssidLen;
    memcpy(&frame[pos], ssid, ssidLen);
    pos += ssidLen;

    // Supported rates tag (minimal — looks like a real client)
    frame[pos++] = 0x01;  // tag: supported rates
    frame[pos++] = 0x04;  // length
    frame[pos++] = 0x82;  frame[pos++] = 0x84;
    frame[pos++] = 0x8B;  frame[pos++] = 0x96;

    esp_wifi_80211_tx(WIFI_IF_AP, frame, pos, false);
    HAMLET_LOGF("[RECON] phantom probe injected: '%s'\n", ssid);
}

// Generate a bait SSID that looks plausibly real but is unique enough to confirm.
static void generateBaitSSID(char* out, size_t outSz, const uint8_t* bssid) {
    uint32_t h = fnvHash(bssid, 6) ^ (uint32_t)millis();
    snprintf(out, outSz, "Guest_%04lX", (unsigned long)(h & 0xFFFF));
}

// Called after KARMA detection — inject phantom probe to confirm.
static void triggerPhantomProbe(const uint8_t* bssid) {
    if (karmaBaitPending) return;  // one at a time
    generateBaitSSID(karmaBaitSSID, 33, bssid);
    memcpy(karmaBaitBSSID, bssid, 6);
    karmaBaitTime = millis();
    karmaBaitPending = true;
    injectPhantomProbe(karmaBaitSSID);
}

// Called during next scan — check if KARMA AP echoed our bait.
static void checkPhantomProbeConfirmation() {
    if (!karmaBaitPending) return;
    // timeout after 2 WiFi scan cycles (~4 minutes)
    if (millis() - karmaBaitTime > 240000) {
        karmaBaitPending = false;
        return;
    }
    for (int i = 0; i < wifiAPCount; i++) {
        if (memcmp(wifiSnapshot[i].bssid, karmaBaitBSSID, 6) == 0 &&
            strcmp(wifiSnapshot[i].ssid, karmaBaitSSID) == 0) {
            // CONFIRMED: this BSSID echoed a SSID we made up.
            karmaConfirmed = true;
            karmaBaitPending = false;
            ReconEventData ev = {};
            ev.event = ReconEvent::KARMA_CONFIRMED;
            ev.rssi = wifiSnapshot[i].rssi;
            ev.channel = wifiSnapshot[i].channel;
            memcpy(ev.bssid, karmaBaitBSSID, 6);
            strncpy(ev.ssid, karmaBaitSSID, 32);
            ev.ssid[32] = '\0';
            snprintf(ev.detail, sizeof(ev.detail), "BAIT ECHOED");
            pushEvent(ev);
            HAMLET_LOGF("[RECON] !! KARMA CONFIRMED: BSSID echoed bait '%s' !!\n", karmaBaitSSID);
            return;
        }
    }
}

// ==[ FEATURE 6: DEAUTH TOOL BEHAVIORAL CLASSIFICATION ]===
// Classify attack tool from deauth burst behavioral signature.
// Called at end of stopDeauthSniff() or emitPendingDeauthEvent().
static DeauthTool classifyDeauthTool(uint16_t frameCount, uint16_t pps,
                                      uint8_t uniqueSources, uint16_t deauthFrames,
                                      uint16_t disassocFrames, uint16_t reasonCode) {
    if (frameCount < 3) return DeauthTool::UNKNOWN;

    float deauthRatio = (float)deauthFrames / (float)(deauthFrames + disassocFrames + 1);
    bool highPPS = (pps >= 200);
    bool mediumPPS = (pps >= 50 && pps < 200);
    bool lowPPS = (pps < 50);

    // MDK3/MDK4: very high rate, many random source MACs, mixed subtypes
    if (highPPS && uniqueSources >= 4) {
        return DeauthTool::MDK3;
    }

    // Flipper Zero: bursts from 1-2 sources at medium+ rate, reason 2 or 8.
    // Previous ≤60 frame cap missed sustained jams; widened to 5-300. The PPS
    // gate (≥50) excludes ambient reason-2/8 traffic from legit AP maintenance,
    // which would otherwise get Flipper-labelled on frame-count + reason alone.
    if (frameCount >= 5 && frameCount <= 300 &&
        uniqueSources <= 2 && (mediumPPS || highPPS) &&
        (reasonCode == 2 || reasonCode == 8)) {
        return DeauthTool::FLIPPER;
    }

    // Pwnagotchi: low rate, targeted (1 source, 1 channel), reason 1
    if (lowPPS && uniqueSources == 1 && reasonCode == 1) {
        return DeauthTool::PWNAGOTCHI;
    }

    // bettercap: medium rate, single source, deauth-only
    if (mediumPPS && uniqueSources == 1 && deauthRatio > 0.9f) {
        return DeauthTool::BETTERCAP;
    }

    // Airgeddon: slower, reason 7 (class 3 frame from non-associated STA)
    if (mediumPPS && reasonCode == 7) {
        return DeauthTool::AIRGEDDON;
    }

    // ESP32 deauther: medium rate, 1-3 sources, mixed reasons
    if (mediumPPS && uniqueSources <= 3) {
        return DeauthTool::ESP32_DEAUTHER;
    }

    // couldn't match a known signature
    if (frameCount >= 10) return DeauthTool::CUSTOM;
    return DeauthTool::UNKNOWN;
}

DeauthTool getLastDeauthTool() { return lastDeauthTool; }

const char* deauthToolLabel(DeauthTool tool) {
    switch (tool) {
        case DeauthTool::MDK3:           return "mdk3";
        case DeauthTool::AIRGEDDON:      return "airgeddon";
        case DeauthTool::FLIPPER:        return "Flipper";
        case DeauthTool::BETTERCAP:      return "bettercap";
        case DeauthTool::ESP32_DEAUTHER: return "ESP-DTH";
        case DeauthTool::PWNAGOTCHI:     return "pwngotchi";
        case DeauthTool::CUSTOM:         return "custom";
        default:                         return "?";
    }
}

// ==[ FEATURE 5: PASSIVE CLIENT FINGERPRINTING ]========

// Known hostile IE fingerprint patterns
struct IEProfile {
    uint32_t ieHash;
    uint8_t  hostileScore;
    const char* label;
};
// Known hostile IE fingerprint patterns — learning mode:
// hashes start at 0 and are populated on first match with a high-hostility
// client whose guessClientLabel() matches the profile label. Once learned,
// future probes with the same ieHash get the profile's boosted score.
static IEProfile KNOWN_HOSTILE_PROFILES[] = {
    // Kali airmon-ng default: rates + HT caps, no VHT/extended/WPS
    {0, 95, "Kali"},     // hash learned from first hostile Kali-labeled client
    // ESP32 scanner: minimal IE set (rates only, no HT)
    {0, 85, "ESP32"},    // hash learned from first hostile ESP32-labeled client
};
static constexpr int KNOWN_PROFILE_COUNT = sizeof(KNOWN_HOSTILE_PROFILES) / sizeof(KNOWN_HOSTILE_PROFILES[0]);

// Heuristic hostile scoring based on IE presence/absence.
// Real phones have rich IE sets (HT, VHT, ext caps, WPS). Attack tools have bare minimum.
static uint8_t scoreProbeHostility(const ProbeCapture& pc) {
    uint8_t score = 0;
    // minimal IE count = suspicious (phones have 8-15 IEs, tools have 2-4)
    if (pc.ieCount <= 3) score += 40;
    else if (pc.ieCount <= 5) score += 20;
    // no HT capabilities = ancient device or attack tool
    if (!pc.hasHTCaps) score += 20;
    // no VHT = not modern phone (but could be IoT)
    if (!pc.hasVHTCaps) score += 10;
    // no extended capabilities = suspicious
    if (!pc.hasExtCaps) score += 15;
    // has WPS = probably legitimate device
    if (pc.hasWPS && score > 15) score -= 15;
    // clamp
    return (score > 100) ? 100 : score;
}

static const char* guessClientLabel(const ProbeCapture& pc) {
    if (pc.ieCount <= 2 && !pc.hasHTCaps) return "ESP32";
    if (pc.ieCount <= 4 && pc.hasHTCaps && !pc.hasVHTCaps && !pc.hasWPS) return "Kali";
    if (pc.ieCount <= 5 && !pc.hasExtCaps) return "aircrack";
    if (pc.hasHTCaps && pc.hasVHTCaps && pc.hasExtCaps) return "Phone";
    if (pc.hasHTCaps && pc.hasWPS) return "Laptop";
    return "Device";
}

// Process probe captures from sniff buffer — client fingerprint table.
static void processProbeCaptures() {
    int count = probeCaptureCount.load(std::memory_order_acquire);
    if (count == 0 || !probeCaptureBuf || !clientFingerprints) return;
    uint32_t now = millis();

    for (int i = 0; i < count; i++) {
        ProbeCapture& pc = probeCaptureBuf[i];
        if (!pc.valid) continue;

        uint8_t hostileScore = scoreProbeHostility(pc);
        const char* label = guessClientLabel(pc);

        // ==[ KNOWN HOSTILE PROFILE LOOKUP ]== check ieHash against learned profiles
        for (int k = 0; k < KNOWN_PROFILE_COUNT; k++) {
            IEProfile& prof = KNOWN_HOSTILE_PROFILES[k];
            if (prof.ieHash != 0 && pc.ieHash == prof.ieHash) {
                // known hostile hash match — boost score to profile floor
                if (prof.hostileScore > hostileScore) hostileScore = prof.hostileScore;
                label = prof.label;
                break;
            }
        }

        // ==[ LEARNING MODE ]== first high-hostility match learns the hash for its profile
        if (hostileScore >= 60 && pc.ieHash != 0) {
            for (int k = 0; k < KNOWN_PROFILE_COUNT; k++) {
                IEProfile& prof = KNOWN_HOSTILE_PROFILES[k];
                if (prof.ieHash == 0 && strcmp(label, prof.label) == 0) {
                    prof.ieHash = pc.ieHash;
                    HAMLET_LOGF("[RECON] learned hostile profile '%s' hash: 0x%08lX\n",
                                  prof.label, (unsigned long)prof.ieHash);
                    break;
                }
            }
        }

        // upsert into fingerprint table
        int existing = -1;
        for (int j = 0; j < clientFingerprintCount; j++) {
            if (memcmp(clientFingerprints[j].clientMac, pc.clientMac, 6) == 0) {
                existing = j;
                break;
            }
        }

        if (existing >= 0) {
            ClientFingerprint& cf = clientFingerprints[existing];
            cf.rssi = pc.rssi;
            cf.channel = pc.channel;
            cf.lastSeen = now;
            cf.hostileScore = hostileScore;
            strncpy(cf.label, label, sizeof(cf.label) - 1);
        } else if (clientFingerprintCount < MAX_CLIENT_FINGERPRINTS) {
            ClientFingerprint& cf = clientFingerprints[clientFingerprintCount];
            memset(&cf, 0, sizeof(ClientFingerprint));
            memcpy(cf.clientMac, pc.clientMac, 6);
            cf.fingerprintHash = pc.ieHash;
            cf.rssi = pc.rssi;
            cf.channel = pc.channel;
            cf.hostileScore = hostileScore;
            cf.toolSignature = (uint8_t)DeauthTool::UNKNOWN;
            cf.firstSeen = now;
            cf.lastSeen = now;
            strncpy(cf.label, label, sizeof(cf.label) - 1);
            clientFingerprintCount++;

            // emit event for high-hostility clients
            if (hostileScore >= 60) {
                ReconEventData ev = {};
                ev.event = ReconEvent::HOSTILE_CLIENT;
                ev.rssi = pc.rssi;
                ev.channel = pc.channel;
                snprintf(ev.detail, sizeof(ev.detail), "%s score:%d ie:%d",
                         label, hostileScore, pc.ieCount);
                pushEvent(ev);
                HAMLET_LOGF("[RECON] HOSTILE CLIENT: %s %ddBm score:%d\n",
                              label, pc.rssi, hostileScore);
            }
        }
    }

    // clear capture buffer
    probeCaptureCount.store(0, std::memory_order_release);
}

uint8_t getClientFingerprintCount() { return clientFingerprintCount; }
const ClientFingerprint* getClientFingerprints() { return clientFingerprints; }

// ==[ FEATURE 3: BEACON ENTROPY SCORING ]======
// Process beacon captures — score wifiSnapshot entries.
static void processBeaconCaptures() {
    int count = beaconCaptureCount.load(std::memory_order_acquire);
    if (count == 0 || !beaconCaptureBuf) return;

    for (int i = 0; i < count; i++) {
        BeaconIECapture& bc = beaconCaptureBuf[i];
        if (!bc.valid) continue;

        // find matching AP in snapshot
        for (int j = 0; j < wifiAPCount; j++) {
            if (memcmp(wifiSnapshot[j].bssid, bc.bssid, 6) == 0) {
                // entropy score: map IE diversity to 0-100
                // real APs: 8-20 unique IE types, 2-5 vendor IEs — score 70-100
                // hostapd/fake: 3-5 IE types, 0-1 vendor IEs — score 10-40
                uint8_t score = 0;
                if (bc.uniqueIETypes >= 12) score = 90;
                else if (bc.uniqueIETypes >= 8) score = 70;
                else if (bc.uniqueIETypes >= 6) score = 50;
                else if (bc.uniqueIETypes >= 4) score = 30;
                else score = 10;

                // vendor IE bonus: real APs have vendor extensions
                if (bc.vendorIECount >= 3) score += 10;
                else if (bc.vendorIECount >= 1) score += 5;
                if (score > 100) score = 100;

                wifiSnapshot[j].entropyScore = score;

                // emit threat event for suspiciously low-entropy beacons
                // (hostapd, airbase-ng, mana — minimal IE sets)
                if (score <= 30 && wifiSnapshot[j].ssid[0] != '\0') {
                    ReconEventData ev = {};
                    ev.event = ReconEvent::LOW_ENTROPY_BEACON;
                    ev.rssi = wifiSnapshot[j].rssi;
                    ev.channel = wifiSnapshot[j].channel;
                    memcpy(ev.bssid, wifiSnapshot[j].bssid, 6);
                    strncpy(ev.ssid, wifiSnapshot[j].ssid, 32);
                    ev.ssid[32] = '\0';
                    snprintf(ev.detail, sizeof(ev.detail), "ent:%u ie:%u vnd:%u",
                             score, bc.uniqueIETypes, bc.vendorIECount);
                    pushEvent(ev);
                    HAMLET_LOGF("[RECON] LOW ENTROPY: '%s' ch%d score:%u\n",
                                  wifiSnapshot[j].ssid, wifiSnapshot[j].channel, score);
                }
                break;
            }
        }
    }

    beaconCaptureCount.store(0, std::memory_order_release);
}
