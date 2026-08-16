#include "nowflock_feed.h"
#include "nowflock_graph.h"
#include "nowflock_lsp.h"
#include "../core/gps.h"
#include "../defense/recon.h"
#include "../defense/xband.h"
#include "../defense/defense_pipeline.h"
#include "nowflock_protocol.h"

#ifndef NATIVE_TEST
#include <Arduino.h>
#endif

namespace NowFlockFeed {

static uint32_t lastTickMs = 0;
static constexpr uint32_t TICK_MS = 1000;

static int32_t latToE7(double lat) {
    return (int32_t)(lat * 10000000.0);
}

static int32_t lonToE7(double lon) {
    return (int32_t)(lon * 10000000.0);
}

static uint16_t wifiEvidence(const Recon::WifiAP& ap) {
    uint16_t bits = NowFlock::EVID_WIFI_FAMILY;
    if (ap.entropyScore >= 50) bits |= NowFlock::EVID_WIFI_STRONG;
    return bits;
}

static uint16_t bleEvidence(const Recon::TrackerEntry& t) {
    uint16_t bits = NowFlock::EVID_BLE_FAMILY;
    if (t.type != Recon::ThreatType::UNKNOWN) bits |= NowFlock::EVID_BLE_STRONG;
    if (t.seenCount >= 2) bits |= NowFlock::EVID_BLE_REPEAT;
    return bits;
}

static uint32_t wifiHashFromAp(const Recon::WifiAP& ap) {
    uint32_t h = ap.bssid[0] ^ ((uint32_t)ap.bssid[3] << 8) ^ ((uint32_t)ap.channel << 16);
    return h ^ (uint32_t)ap.entropyScore;
}

void init() {
    lastTickMs = 0;
}

void tick(uint32_t nowMs) {
    if (lastTickMs != 0 && (nowMs - lastTickMs) < TICK_MS) return;
    lastTickMs = nowMs;

    bool gpsValid = GPS::hasFix();
    bool staleGps = false;
    if (gpsValid) {
        staleGps = GPS::getHdop() > 8.0f;
    }

    int32_t tileLat = 0;
    int32_t tileLon = 0;
    if (gpsValid) {
        tileLat = NowFlockLsp::quantizeTileE7(latToE7(GPS::getLatitude()));
        tileLon = NowFlockLsp::quantizeTileE7(lonToE7(GPS::getLongitude()));
    }

    int wifiCount = DefensePipeline::snapshot().getWifiSnapshotCount();
    const Recon::WifiAP* wifi = DefensePipeline::snapshot().getWifiSnapshot();
    for (int i = 0; i < wifiCount && wifi; ++i) {
        int32_t wLat = tileLat;
        int32_t wLon = tileLon;
        if (wifi[i].lat != 0.0f || wifi[i].lon != 0.0f) {
            wLat = NowFlockLsp::quantizeTileE7(latToE7(wifi[i].lat));
            wLon = NowFlockLsp::quantizeTileE7(lonToE7(wifi[i].lon));
        } else if (!gpsValid) {
            continue;
        }
        uint16_t score = (uint16_t)(320 + (wifi[i].rssi + 100) * 4);
        if (score > 980) score = 980;
        uint8_t conf = (uint8_t)(55 + (wifi[i].rssi + 90));
        if (conf > 69) conf = 69;
        NowFlockGraph::observeLocal(true, nowMs, wLat, wLon, wifiHashFromAp(wifi[i]), 0,
                                    score, conf, wifiEvidence(wifi[i]), gpsValid, staleGps);
    }

    const Recon::TrackerEntry* ble = DefensePipeline::snapshot().getBleDevices();
    int bleCount = DefensePipeline::snapshot().getBleDeviceTableSize();
    for (int i = 0; i < bleCount && ble; ++i) {
        int32_t bLat = tileLat;
        int32_t bLon = tileLon;
        if (ble[i].lastLat != 0.0f || ble[i].lastLon != 0.0f) {
            bLat = NowFlockLsp::quantizeTileE7(latToE7(ble[i].lastLat));
            bLon = NowFlockLsp::quantizeTileE7(lonToE7(ble[i].lastLon));
        } else if (!gpsValid) {
            continue;
        }
        uint32_t bleHash = ble[i].payloadHash[0]
            | ((uint32_t)ble[i].payloadHash[1] << 8)
            | ((uint32_t)ble[i].payloadHash[2] << 16)
            | ((uint32_t)ble[i].payloadHash[3] << 24);
        uint16_t score = (uint16_t)(320 + (ble[i].rssi + 100) * 4);
        if (score > 980) score = 980;
        uint8_t conf = (uint8_t)(55 + (ble[i].rssi + 90));
        if (conf > 69) conf = 69;
        NowFlockGraph::observeLocal(false, nowMs, bLat, bLon, 0, bleHash,
                                    score, conf, bleEvidence(ble[i]), gpsValid, staleGps);
    }

    const XBand::CohortPair* pairs = DefensePipeline::snapshot().getCohortPairs();
    int pairCount = DefensePipeline::snapshot().getCohortCount();
    for (int i = 0; i < pairCount && pairs && gpsValid; ++i) {
        if (pairs[i].confidence < 2) continue;
        uint32_t bleHash = pairs[i].blePayloadHash[0]
            | ((uint32_t)pairs[i].blePayloadHash[1] << 8)
            | ((uint32_t)pairs[i].blePayloadHash[2] << 16)
            | ((uint32_t)pairs[i].blePayloadHash[3] << 24);
        uint32_t wifiHash = pairs[i].wifiMac[3] | ((uint32_t)pairs[i].wifiMac[4] << 8);
        NowFlockGraph::observeLocal(true, nowMs, tileLat, tileLon, wifiHash, bleHash,
                                    520, 65, NowFlock::EVID_WIFI_FAMILY | NowFlock::EVID_BLE_FAMILY,
                                    gpsValid, staleGps);
        NowFlockGraph::observeLocal(false, nowMs, tileLat, tileLon, wifiHash, bleHash,
                                    520, 65, NowFlock::EVID_WIFI_FAMILY | NowFlock::EVID_BLE_FAMILY,
                                    gpsValid, staleGps);
    }
}

} // namespace NowFlockFeed
