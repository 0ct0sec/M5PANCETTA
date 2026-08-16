/**
 * FLOCKNOW transport — summaries cross the wire; raw evidence stays home.
 *
 * ESP-NOW callbacks work Core 0 while the case desk consumes their bounded
 * handoff on Core 1. Atomics guard custody, fixed frames guard memory, and radio
 * policy decides when this witness is allowed to speak.
 */
#include "nowflock_transport.h"
#include "nowflock_protocol.h"
#include "nowflock_feed.h"
#include "nowflock_graph.h"
#include "nowflock_state.h"
#include "nowflock_export.h"
#include "../core/config.h"
#include "../core/gps.h"
#include "../core/radio_policy.h"
#include "../defense/recon.h"
#include "../defense/defense_pipeline.h"
#include "../hamlet.h"
#include "../net/wifi_client.h"

#include <string.h>
#include <atomic>

#ifndef NATIVE_TEST
#include <Arduino.h>
#include <M5Unified.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <NimBLEDevice.h>
#include "../core/power.h"
#include "../defense/ble_chaff.h"
#endif

namespace NowFlock {

struct PendingFrame {
    std::atomic<int> len;
    uint8_t mac[6];
    uint8_t data[ESPNOW_MAX_BYTES];
    int8_t rssi;
};

static constexpr uint8_t RX_RING_SIZE = 4;
static PendingFrame rxRing[RX_RING_SIZE];
static std::atomic<uint16_t> sendFailCount{0};

static bool initialized = false;
// Written by the ESP-NOW send callback on Core 0 and consumed by the main
// loop on Core 1. Keep the handoff flags atomic; a failed send can coincide
// with a mode transition releasing the radio.
static std::atomic<bool> espNowNeedsReinit{false};
static std::atomic<bool> haveBroadcastPeer{false};
static bool isMaster = false;
static bool foreignTraffic = false;
static bool degraded = false;
static uint32_t nodeId = 0;
static uint32_t masterNodeId = 0;
static uint32_t groupKey = DEFAULT_GROUP_KEY;
static uint32_t lastHelloMs = 0;
static uint32_t lastAssignMs = 0;
static uint32_t lastSightingMs = 0;
static uint32_t lastSyncMs = 0;
static uint32_t lastMasterMs = 0;
static uint32_t promotedMs = 0;
static uint16_t seq = 1;
static uint16_t channelMask = ALL_CHANNELS_MASK;
static uint16_t localClaimedChannels = 0;
static uint16_t rxBad = 0;
static uint16_t rxDup = 0;
static uint16_t authFail = 0;
static uint8_t controlChannel = DEFAULT_CONTROL_CHANNEL;
static uint16_t reportIntervalS = 10;
static uint8_t lastFrameType = 0;
static bool peerReqPending = false;
static uint8_t peerReqMinTier = 2;
static SwarmTarget lastSwarmTarget = {};
static char lastCaptureAnnotation[NowFlock::CAPTURE_LINE_MAX + 1] = {};
static uint32_t lastExportMs = 0;

static const uint8_t BROADCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

#ifndef NATIVE_TEST
static void onDataRecv(const uint8_t* mac, const uint8_t* data, int len);
static void onDataSent(const uint8_t* mac, esp_now_send_status_t status);
#endif

static uint32_t nowMs() {
#ifndef NATIVE_TEST
    return millis();
#else
    return 0;
#endif
}

static uint8_t batteryPct() {
#ifndef NATIVE_TEST
    int level = M5.Power.getBatteryLevel();
    if (level < 0 || level > 100) return 0xFF;
    return (uint8_t)level;
#else
    return 0xFF;
#endif
}

static uint32_t utcEpochMin() {
    if (Config::hasTrustedClock()) {
        uint32_t epoch = Config::getTrustedEpoch();
        if (epoch) return epoch / 60u;
    }
    if (GPS::hasFix()) {
        uint32_t epoch = GPS::getEpochUtc();
        if (epoch) return epoch / 60u;
    }
    return 0;
}

static uint16_t groupId() {
    return (uint16_t)(groupKey & 0xFFFFu);
}

static uint16_t jitter(uint32_t baseMs, uint8_t salt) {
    uint32_t x = nodeId ^ ((uint32_t)salt << 24) ^ (baseMs * 2654435761u);
    x ^= x >> 16;
    x *= 2246822519u;
    x ^= x >> 13;
    return (uint16_t)(x % 997u);
}

static bool modeOwnsRadio() {
    return RadioPolicy::modeOwnsWifi(Hamlet::getMode()) ||
           WifiClient::ownsRadio();
}

static bool canTx(uint8_t type) {
    if (!Config::getNowFlockEnabled() || !initialized ||
        espNowNeedsReinit.load(std::memory_order_relaxed) ||
        modeOwnsRadio()) return false;
    uint8_t bat = batteryPct();
    if (bat != 0xFF && bat < 15) return type == TYPE_HELLO;
    if (bat != 0xFF && bat < 30) return type == TYPE_HELLO || type == TYPE_ASSIGN;
    return true;
}

static uint32_t fnv1aNodeId(const uint8_t mac[6]) {
    static const uint8_t salt[4] = {0x11, 0x0A, 0x5C, 0x9D};
    uint32_t h = 0x811C9DC5u;
    for (uint8_t b : salt) {
        h ^= b;
        h *= 0x01000193u;
    }
    for (uint8_t i = 0; i < 6; ++i) {
        h ^= mac[i];
        h *= 0x01000193u;
    }
    return h ? h : 1u;
}

static Header baseHeader(uint8_t type, uint8_t bodyLen) {
    Header h;
    h.type = type;
    h.seq = seq++;
    if (seq == 0) seq = 1;
    h.nodeId = nodeId;
    h.uptimeMs = nowMs();
    h.role = isMaster ? ROLE_MASTER : ROLE_CHILD;
    h.channel = controlChannel;
    h.batteryPct = batteryPct();
    h.bodyLen = bodyLen;
    return h;
}

static bool sendFrame(uint8_t type, const uint8_t* body, size_t bodyLen) {
#ifndef NATIVE_TEST
    if (!canTx(type)) return false;
    uint8_t frame[ESPNOW_MAX_BYTES];
    size_t len = encodeFrame(baseHeader(type, (uint8_t)bodyLen), body, bodyLen,
                             groupKey, frame, sizeof(frame));
    if (len == 0) return false;
    esp_err_t err = esp_now_send(BROADCAST_MAC, frame, len);
    if (err != ESP_OK) {
        sendFailCount.fetch_add(1, std::memory_order_relaxed);
        espNowNeedsReinit.store(true, std::memory_order_relaxed);
        haveBroadcastPeer.store(false, std::memory_order_relaxed);
        return false;
    }
    return true;
#else
    (void)type; (void)body; (void)bodyLen;
    return false;
#endif
}

static bool radioOnControlChannel() {
    uint8_t cur = 0;
    wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
    if (esp_wifi_get_channel(&cur, &second) != ESP_OK) return false;
    return cur == controlChannel;
}

static bool ensureEspNowReady() {
#ifdef NATIVE_TEST
    return false;
#else
    if (!DefensePipeline::snapshot().isBleInitialized()) {
        return false;
    }

    if (initialized && !espNowNeedsReinit.load(std::memory_order_relaxed) &&
        radioOnControlChannel()) {
        return true;
    }

    esp_wifi_set_promiscuous(false);
    if (initialized) {
        esp_now_deinit();
        initialized = false;
    }

    groupKey = Config::getNowFlockGroupKey();
    reportIntervalS = Config::getNowFlockReportIntervalS();

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();

    esp_err_t startErr = esp_wifi_start();
    if (startErr != ESP_OK && startErr != ESP_ERR_WIFI_CONN) {
        initialized = false;
        return false;
    }

    // esp_wifi_start() resets driver-owned policy. Restore the selected
    // profile only after the driver is live so TX power cannot fall back to
    // the SDK default after light sleep or an ESP-NOW reinitialization.
    Power::applyCurrentRadioSettings();

    esp_wifi_set_ps(DefensePipeline::snapshot().isBleInitialized() ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE);
    if (esp_wifi_set_channel(controlChannel, WIFI_SECOND_CHAN_NONE) != ESP_OK) {
        initialized = false;
        return false;
    }

    if (esp_now_init() != ESP_OK) {
        initialized = false;
        return false;
    }

    esp_now_register_recv_cb(onDataRecv);
    esp_now_register_send_cb(onDataSent);
    initialized = true;
    espNowNeedsReinit.store(false, std::memory_order_relaxed);
    haveBroadcastPeer.store(false, std::memory_order_relaxed);
    return true;
#endif
}

static bool ensureBroadcastPeer() {
#ifdef NATIVE_TEST
    return false;
#else
    if (haveBroadcastPeer.load(std::memory_order_relaxed)) {
        esp_now_peer_info_t info = {};
        if (esp_now_get_peer(BROADCAST_MAC, &info) == ESP_OK
            && info.channel == controlChannel) {
            return true;
        }
        esp_now_del_peer(BROADCAST_MAC);
        haveBroadcastPeer.store(false, std::memory_order_relaxed);
    }
    esp_now_peer_info_t peer;
    memset(&peer, 0, sizeof(peer));
    memcpy(peer.peer_addr, BROADCAST_MAC, 6);
    peer.channel = controlChannel;
    peer.encrypt = false;
    if (esp_now_add_peer(&peer) != ESP_OK && !esp_now_is_peer_exist(BROADCAST_MAC)) {
        return false;
    }
    haveBroadcastPeer.store(true, std::memory_order_relaxed);
    return true;
#endif
}

static uint8_t buildCapabilities() {
    uint8_t caps = CAP_LSP1;
    if (Config::getNowFlockBleHeartbeat()) caps |= CAP_BLE_HEARTBEAT;
    if (NowFlockExport::enabled()) caps |= CAP_PIGBROTHER;
    return caps;
}

static void sendHello(uint32_t now) {
    uint8_t body[HELLO_BODY_SIZE];
    HelloBody hb;
    hb.fgCandidates = NowFlockGraph::candidateCount();
    hb.fgExportable = NowFlockGraph::exportableCount();
    hb.mode = modeOwnsRadio() ? MODE_HUNT : MODE_IDLE;
    hb.capabilities = buildCapabilities();
    hb.claimedChannels = localClaimedChannels;
    hb.groupId = groupId();
    hb.reportIntervalS = reportIntervalS;
    encodeHelloBody(hb, body);
    if (sendFrame(TYPE_HELLO, body, sizeof(body))) lastHelloMs = now;
}

static void sendAssign(uint32_t now) {
    uint8_t body[ASSIGN_BODY_SIZE];
    AssignBody ab;
    ab.controlChannel = controlChannel;
    ab.maxChildren = 6;
    ab.reportIntervalS = reportIntervalS;
    uint16_t claimed = NowFlockState::unionClaimedChannels(now);
    ab.channelMask = (uint16_t)(ALL_CHANNELS_MASK & ~claimed);
    if (ab.channelMask == 0) ab.channelMask = ALL_CHANNELS_MASK;
    ab.utcEpochMin = utcEpochMin();
    ab.masterNodeId = nodeId;
    encodeAssignBody(ab, body);
    if (sendFrame(TYPE_ASSIGN, body, sizeof(body))) lastAssignMs = now;
}

static void sendSync(uint32_t now) {
    uint8_t body[SYNC_BODY_SIZE];
    SyncBody sb;
    sb.utcEpochMin = utcEpochMin();
    if (sb.utcEpochMin == 0) return;
    sb.uptimeRefMs = now;
    sb.accuracyS = 60;
    encodeSyncBody(sb, body);
    if (sendFrame(TYPE_SYNC, body, sizeof(body))) lastSyncMs = now;
}

static void sendPeerReq(uint32_t now) {
    uint8_t body[PEER_REQ_BODY_SIZE];
    PeerReqBody pr;
    pr.minTier = peerReqMinTier;
    encodePeerReqBody(pr, body);
    if (sendFrame(TYPE_PEER_REQ, body, sizeof(body))) {
        peerReqPending = false;
    }
    (void)now;
}

static void sendExportSnapshot(uint32_t now) {
    uint8_t line[EXPORT_LINE_MAX];
    uint8_t lineLen = 0;
    if (!NowFlockExport::dequeueLine(line, lineLen, EXPORT_LINE_MAX)) return;
    uint8_t body[1 + EXPORT_LINE_MAX];
    size_t bodyLen = encodeExportSnapshotBody(line, lineLen, body, sizeof(body));
    if (bodyLen == 0) return;
    if (sendFrame(TYPE_EXPORT_SNAPSHOT, body, bodyLen)) lastExportMs = now;
}

#ifndef NATIVE_TEST
static bool bleHeartbeatOwnsAdvertising = false;

static void stopBleHeartbeat() {
    if (!bleHeartbeatOwnsAdvertising) return;
    bleHeartbeatOwnsAdvertising = false;
    // BLE chaff may have replaced our payload on the shared advertiser. In
    // that case ownership already changed hands and FNOW must not stop it.
    if (BLEChaff::isActive() ||
        !DefensePipeline::snapshot().isBleInitialized()) return;
    NimBLEAdvertising* pAdv = NimBLEDevice::getAdvertising();
    if (pAdv->isAdvertising()) pAdv->stop();
}

static void updateBleHeartbeat(uint32_t now) {
    static uint32_t lastAdvMs = 0;
    if (!Config::getNowFlockBleHeartbeat()) {
        stopBleHeartbeat();
        return;
    }
    if (BLEChaff::isActive()) {
        bleHeartbeatOwnsAdvertising = false;
        return;
    }
    if (!DefensePipeline::snapshot().isBleInitialized()) return;
    if (now - lastAdvMs < 1000u) return;
    lastAdvMs = now;

    Status st = getStatus();
    uint8_t mfg[8];
    mfg[0] = 0xFF;
    mfg[1] = 0xFF;
    mfg[2] = st.candidates;
    mfg[3] = st.exportable;
    NowFlockSerial::writeU16(mfg + 4, localClaimedChannels);
    mfg[6] = batteryPct();
    uint8_t flags = st.active ? 0x01u : 0x00u;
    if (st.master) flags |= 0x02u;
    mfg[7] = flags;

    NimBLEAdvertisementData advData;
    advData.setFlags(0x06);
    advData.setManufacturerData(std::string((char*)mfg, sizeof(mfg)));
    NimBLEAdvertising* pAdv = NimBLEDevice::getAdvertising();
    if (pAdv->isAdvertising()) pAdv->stop();
    pAdv->setAdvertisementData(advData);
    pAdv->setMinInterval(0x20);
    pAdv->setMaxInterval(0x40);
    pAdv->start(0);
    bleHeartbeatOwnsAdvertising = true;
}
#else
static void stopBleHeartbeat() {}
static void updateBleHeartbeat(uint32_t) {}
#endif

static void sendSighting(uint32_t now) {
    CandidateWire rows[SIGHTING_MAX_CANDIDATES];
    uint8_t count = NowFlockGraph::encodeTopCandidates(rows, SIGHTING_MAX_CANDIDATES, now,
                                                         peerReqPending ? peerReqMinTier : 2);
    if (count == 0 && !peerReqPending) return;

    uint8_t body[CANDIDATE_BODY_SIZE + SIGHTING_MAX_CANDIDATES * CANDIDATE_WIRE_SIZE] = {};
    body[0] = count;
    body[1] = NowFlockGraph::candidateCount();
    body[2] = NowFlockGraph::exportableCount();
    body[3] = 0;
    for (uint8_t i = 0; i < count; ++i) {
        encodeCandidateWire(rows[i], body + CANDIDATE_BODY_SIZE + i * CANDIDATE_WIRE_SIZE);
    }
    if (sendFrame(TYPE_SIGHTING, body, CANDIDATE_BODY_SIZE + count * CANDIDATE_WIRE_SIZE)) {
        lastSightingMs = now;
        peerReqPending = false;
    }
}

static void elect(uint32_t now) {
    uint32_t high = NowFlockState::highestActiveNodeId(now, nodeId);
    if (high > nodeId) {
        if (!isMaster || (now - promotedMs) >= ELECTION_GUARD_MS) {
            isMaster = false;
        }
    } else if (high == nodeId) {
        if (!isMaster && (masterNodeId == 0 || (now - lastMasterMs) > MASTER_TIMEOUT_MS)) {
            isMaster = true;
            masterNodeId = nodeId;
            promotedMs = now;
        }
    }
    degraded = NowFlockState::activePeerCount(now) > 0 && masterNodeId == 0
        && (now - lastMasterMs) > MASTER_TIMEOUT_MS;
}

static void handleDecoded(const Header& hdr, const uint8_t* body, uint32_t now) {
    if (hdr.type == TYPE_HELLO) {
        HelloBody hb;
        if (!decodeHelloBody(body, hdr.bodyLen, hb)) return;
        if (hb.groupId != groupId()) {
            foreignTraffic = true;
            return;
        }
        if (!NowFlockState::acceptSequence(hdr.nodeId, hdr.seq, now)) {
            ++rxDup;
            return;
        }
        lastFrameType = hdr.type;
        NowFlockState::noteHello(hdr.nodeId, hdr, hb, now);
        return;
    }

    if (!NowFlockState::acceptSequence(hdr.nodeId, hdr.seq, now)) {
        ++rxDup;
        return;
    }
    lastFrameType = hdr.type;

    if (hdr.type == TYPE_ASSIGN) {
        AssignBody ab;
        if (!decodeAssignBody(body, hdr.bodyLen, ab)) return;
        NowFlockState::noteAssign(hdr.nodeId, hdr, now);
        if (ab.masterNodeId > nodeId || masterNodeId == 0) {
            masterNodeId = ab.masterNodeId;
            lastMasterMs = now;
            if (ab.controlChannel != controlChannel) {
                controlChannel = ab.controlChannel;
                haveBroadcastPeer.store(false, std::memory_order_relaxed);
            }
            reportIntervalS = ab.reportIntervalS < 2 ? 2 : (ab.reportIntervalS > 60 ? 60 : ab.reportIntervalS);
            channelMask = ab.channelMask ? ab.channelMask : ALL_CHANNELS_MASK;
            if (ab.utcEpochMin > 0 && !GPS::hasFix() && !Config::hasTrustedClock()) {
                Config::adoptSyncEpochMin(ab.utcEpochMin, hdr.uptimeMs, now);
            }
            if (ab.masterNodeId > nodeId) {
                if (!isMaster || (now - promotedMs) >= ELECTION_GUARD_MS) {
                    isMaster = false;
                }
            }
        } else if (isMaster && ab.masterNodeId > nodeId) {
            if ((now - promotedMs) >= ELECTION_GUARD_MS) {
                isMaster = false;
            }
        }
    } else if (hdr.type == TYPE_SYNC) {
        if (hdr.role != ROLE_MASTER || hdr.bodyLen != SYNC_BODY_SIZE) return;
        SyncBody sb;
        if (!decodeSyncBody(body, hdr.bodyLen, sb)) return;
        if (sb.utcEpochMin == 0 || sb.accuracyS > 120) return;
        if (!GPS::hasFix() && !Config::hasTrustedClock()) {
            Config::adoptSyncEpochMin(sb.utcEpochMin, sb.uptimeRefMs, now);
        }
    } else if (hdr.type == TYPE_PEER_REQ) {
        PeerReqBody pr;
        if (!decodePeerReqBody(body, hdr.bodyLen, pr)) return;
        NowFlockState::Peer* p = NowFlockState::findPeer(hdr.nodeId);
        if (p && (now - p->lastPeerReqMs) >= HELLO_INTERVAL_MS) {
            p->lastPeerReqMs = now;
            peerReqMinTier = pr.minTier ? pr.minTier : 2;
            peerReqPending = true;
        }
    } else if (hdr.type == TYPE_TARGET) {
        TargetBody tb;
        if (!decodeTargetBody(body, hdr.bodyLen, tb)) return;
        memcpy(lastSwarmTarget.bssid, tb.bssid, 6);
        lastSwarmTarget.channel = tb.channel;
        lastSwarmTarget.nodeId = hdr.nodeId;
        lastSwarmTarget.seenMs = now;
        lastSwarmTarget.valid = true;
    } else if (hdr.type == TYPE_CAPTURE) {
        const uint8_t* line = nullptr;
        uint8_t lineLen = 0;
        if (!decodeCaptureBody(body, hdr.bodyLen, line, lineLen)) return;
        if (lineLen > CAPTURE_LINE_MAX) return;
        memcpy(lastCaptureAnnotation, line, lineLen);
        lastCaptureAnnotation[lineLen] = 0;
    } else if (hdr.type == TYPE_EXPORT_SNAPSHOT) {
        const uint8_t* line = nullptr;
        uint8_t lineLen = 0;
        if (!decodeExportSnapshotBody(body, hdr.bodyLen, line, lineLen)) return;
        if (!exportLineHasProfile(line, lineLen)) return;
        NowFlockExport::ingestPeerLine(hdr.nodeId, line, lineLen, now);
    } else if (hdr.type == TYPE_SIGHTING) {
        if (hdr.bodyLen < CANDIDATE_BODY_SIZE) return;
        uint8_t count = body[0];
        if (count > SIGHTING_MAX_CANDIDATES) return;
        if (hdr.bodyLen != CANDIDATE_BODY_SIZE + count * CANDIDATE_WIRE_SIZE) return;
        NowFlockState::noteSighting(hdr.nodeId, hdr, now);
        for (uint8_t i = 0; i < count; ++i) {
            CandidateWire row;
            if (decodeCandidateWire(body + CANDIDATE_BODY_SIZE + i * CANDIDATE_WIRE_SIZE, CANDIDATE_WIRE_SIZE, row)) {
                NowFlockGraph::ingestPeerCandidate(hdr.nodeId, row, now);
            }
        }
    }
}

static void drainRx(uint32_t now) {
    for (uint8_t i = 0; i < RX_RING_SIZE; ++i) {
        int len = rxRing[i].len.exchange(0, std::memory_order_acq_rel);
        if (len <= 0) continue;

        Header hdr;
        const uint8_t* body = nullptr;
        uint16_t beforeBad = rxBad;
        if (!decodeFrame(rxRing[i].data, (size_t)len, groupKey, nodeId, hdr, body)) {
            Header maybe;
            if (decodeHeader(rxRing[i].data, (size_t)len, maybe) && (maybe.flags & FLAG_AUTH)) {
                ++authFail;
            } else {
                ++rxBad;
            }
            if (rxBad == beforeBad) {
                // bad auth is already counted separately
            }
            continue;
        }
        handleDecoded(hdr, body, now);
    }
}

void init() {
    if (initialized) return;
    NowFlockState::init();
    NowFlockGraph::init();
    NowFlockFeed::init();
    NowFlockExport::init();
    groupKey = Config::getNowFlockGroupKey();
    reportIntervalS = Config::getNowFlockReportIntervalS();
    // A blank device stays off-air. updateBackground() calls init() again
    // after the operator enables FNOW, so the gate is both safe at boot and
    // live without a reboot.
    if (!Config::getNowFlockEnabled()) return;
#ifndef NATIVE_TEST
    uint8_t mac[6] = {};
    WiFi.mode(WIFI_STA);
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    nodeId = fnv1aNodeId(mac);
#else
    nodeId = 1;
#endif
    uint32_t now = nowMs();
    promotedMs = now;
    lastHelloMs = now - HELLO_INTERVAL_MS + (nodeId & 0xFFu) * (HELLO_INTERVAL_MS / 256u);
    if (ensureEspNowReady()) {
        ensureBroadcastPeer();
    }
}

void deinit() {
#ifndef NATIVE_TEST
    if (initialized) {
        esp_now_deinit();
    }
#endif
    initialized = false;
    haveBroadcastPeer.store(false, std::memory_order_relaxed);
}

void releaseRadio() {
    // ESP-NOW must die while WiFi is still up. Owners call this before they
    // stop/reconfigure WiFi; the background loop rebuilds it after release.
    deinit();
    espNowNeedsReinit.store(true, std::memory_order_relaxed);
}

void stopSync() {
    // FLOCK is passive. Exit mode without ending background receive.
}

void markEspNowNeedsReinit() {
    espNowNeedsReinit.store(true, std::memory_order_relaxed);
    haveBroadcastPeer.store(false, std::memory_order_relaxed);
}

void requestPeerSummaries() {
    peerReqPending = true;
}

void update() {
    updateBackground();
}

void updateBackground() {
    uint32_t now = nowMs();
    drainRx(now);

    NowFlockFeed::tick(now);
    NowFlockGraph::tick(now);

    if (!Config::getNowFlockEnabled()) {
        stopBleHeartbeat();
        if (initialized) deinit();
        return;
    }
    if (nodeId == 0) {
        init();
        if (nodeId == 0) return;
    }

    // IPP WiFi scan/sniff owns the radio — reclaim only once it returns to SLEEPING
    if (DefensePipeline::snapshot().isScanning()) return;

    if (!ensureEspNowReady() || !ensureBroadcastPeer()) return;
    elect(now);

    if ((now - lastHelloMs) >= (HELLO_INTERVAL_MS + jitter(HELLO_INTERVAL_MS, TYPE_HELLO))) {
        sendHello(now);
    }
    if (isMaster && (now - lastAssignMs) >= (ASSIGN_INTERVAL_MS + jitter(ASSIGN_INTERVAL_MS, TYPE_ASSIGN))) {
        sendAssign(now);
    }
    if (isMaster && (now - lastSyncMs) >= SYNC_INTERVAL_MS) {
        sendSync(now);
    }
    if (peerReqPending) {
        sendPeerReq(now);
    }
    if ((now - lastSightingMs) >= (uint32_t)reportIntervalS * 1000u || peerReqPending) {
        sendSighting(now);
    }
    if (NowFlockExport::enabled() && (now - lastExportMs) >= (uint32_t)reportIntervalS * 1000u) {
        sendExportSnapshot(now);
    }
    updateBleHeartbeat(now);
}

void setClaimedChannels(uint16_t mask) {
    localClaimedChannels = (uint16_t)(mask & ALL_CHANNELS_MASK);
}

void noteHuntChannel(uint8_t channel) {
    if (channel >= 1 && channel <= 13) {
        localClaimedChannels |= (uint16_t)(1u << channel);
    }
}

void broadcastTarget(const uint8_t bssid[6], uint8_t channel) {
    if (!bssid || channel < 1 || channel > 13) return;
    uint8_t body[TARGET_BODY_SIZE];
    TargetBody tb;
    memcpy(tb.bssid, bssid, 6);
    tb.channel = channel;
    encodeTargetBody(tb, body);
    sendFrame(TYPE_TARGET, body, sizeof(body));
}

void broadcastCapture(const char* annotation) {
    if (!annotation) return;
    uint8_t lineLen = (uint8_t)strlen(annotation);
    if (lineLen == 0 || lineLen > CAPTURE_LINE_MAX) return;
    uint8_t body[1 + CAPTURE_LINE_MAX];
    size_t bodyLen = encodeCaptureBody((const uint8_t*)annotation, lineLen, body, sizeof(body));
    if (bodyLen == 0) return;
    sendFrame(TYPE_CAPTURE, body, bodyLen);
}

bool getLastSwarmTarget(SwarmTarget& out) {
    if (!lastSwarmTarget.valid) return false;
    out = lastSwarmTarget;
    return true;
}

const char* getLastCaptureAnnotation() {
    return lastCaptureAnnotation[0] ? lastCaptureAnnotation : nullptr;
}

uint8_t helloCapabilities() {
    return buildCapabilities();
}

Status getStatus() {
    uint32_t now = nowMs();
    Status s;
    s.enabled = Config::getNowFlockEnabled();
    s.initialized = initialized;
    s.active = NowFlockState::activePeerCount(now) > 0 || (now - lastMasterMs) <= REMOTE_ACTIVE_MS;
    s.master = isMaster;
    s.foreignTraffic = foreignTraffic;
    s.degraded = degraded;
    s.peerCount = NowFlockState::activePeerCount(now);
    s.channel = controlChannel;
    s.lastFrameType = lastFrameType;
    s.candidates = NowFlockGraph::candidateCount();
    s.exportable = NowFlockGraph::exportableCount();
    s.corroborated = NowFlockGraph::hasCorroboration();
    if (GPS::hasFix()) s.clockSource = 1;
    else if (Config::hasTrustedClock()) s.clockSource = 3;
    else if (isMaster) s.clockSource = 2;
    else s.clockSource = 0;
    s.nodeId = nodeId;
    s.masterNodeId = masterNodeId;
    s.channelMask = channelMask;
    s.rxBad = rxBad;
    s.rxDup = rxDup;
    s.authFail = authFail;
    s.txFail = sendFailCount.load(std::memory_order_relaxed);
    return s;
}

const char* frameTypeName(uint8_t type) {
    switch (type) {
        case TYPE_HELLO: return "HELLO";
        case TYPE_ASSIGN: return "ASSIGN";
        case TYPE_SIGHTING: return "SIGHT";
        case TYPE_TARGET: return "TARGET";
        case TYPE_CAPTURE: return "CAPTURE";
        case TYPE_SYNC: return "SYNC";
        case TYPE_PEER_REQ: return "PEER_REQ";
        case TYPE_EXPORT_SNAPSHOT: return "EXPORT";
        default: return "NONE";
    }
}

const char* roleName() {
    return isMaster ? "MASTER" : "CHILD";
}

#ifndef NATIVE_TEST
static void onDataRecv(const uint8_t* mac, const uint8_t* data, int len) {
    if (!data || len <= 0 || len > (int)ESPNOW_MAX_BYTES) return;
    for (uint8_t i = 0; i < RX_RING_SIZE; ++i) {
        int expected = 0;
        if (rxRing[i].len.compare_exchange_strong(expected, -1, std::memory_order_acq_rel)) {
            if (mac) memcpy(rxRing[i].mac, mac, 6);
            memcpy(rxRing[i].data, data, (size_t)len);
            rxRing[i].rssi = 0;
            rxRing[i].len.store(len, std::memory_order_release);
            return;
        }
    }
}

static void onDataSent(const uint8_t* mac, esp_now_send_status_t status) {
    (void)mac;
    if (status != ESP_NOW_SEND_SUCCESS) {
        sendFailCount.fetch_add(1, std::memory_order_relaxed);
        espNowNeedsReinit.store(true, std::memory_order_relaxed);
        haveBroadcastPeer.store(false, std::memory_order_relaxed);
    }
}
#endif

} // namespace NowFlock
