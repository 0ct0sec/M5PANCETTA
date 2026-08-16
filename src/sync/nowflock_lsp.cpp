#include "nowflock_lsp.h"
#include "nowflock_serial.h"
#include <string.h>

namespace NowFlockLsp {

int32_t quantizeTileE7(int32_t coordE7) {
    int32_t q = TILE_GRID_E7;
    int32_t half = q / 2;
    int32_t numer = coordE7 + half;
    int32_t div = numer / q;
    if (numer < 0 && (numer % q) != 0) {
        --div;
    }
    return div * q;
}

uint16_t passBucket(uint32_t nowMs) {
    return (uint16_t)(nowMs / PASS_BUCKET_MS);
}

uint8_t candidateTier(uint8_t confidence) {
    if (confidence >= 90) return 4;
    if (confidence >= 75) return 3;
    if (confidence >= 45) return TIER_MEDIUM;
    return 1;
}

uint8_t candidateState(uint8_t confidence, bool exportable) {
    if (exportable) return 3;
    return confidence >= 45 ? 2 : 1;
}

uint16_t capScore(uint16_t score, uint16_t authCaps) {
    return authCaps ? (score > 540u ? 540u : score) : score;
}

bool exportGate(uint16_t authCaps, uint16_t pairEvents, uint16_t passCount, uint8_t confidence) {
    return authCaps == 0 && pairEvents > 0 && passCount >= 2 && confidence >= 75;
}

uint8_t packedFlags(uint8_t tier, uint8_t state, bool gpsValid, bool exportable) {
    uint8_t packed = (uint8_t)((tier & 7u) | ((state & 7u) << 3));
    if (gpsValid) packed |= 0x40u;
    if (exportable) packed |= 0x80u;
    return packed;
}

static uint32_t fnv1a32(const uint8_t* data, size_t len) {
    uint32_t h = 0x811C9DC5u;
    for (size_t i = 0; i < len; ++i) {
        h ^= data[i];
        h *= 0x01000193u;
    }
    return h ? h : 1u;
}

uint32_t candidateId(int32_t tileLatE7, int32_t tileLonE7, uint32_t wifiHash, uint32_t bleHash) {
    static const uint8_t salt[4] = {0x11, 0x0A, 0x5C, 0x9D};
    uint8_t buf[20];
    memcpy(buf, salt, 4);
    NowFlockSerial::writeI32(buf + 4, tileLatE7);
    NowFlockSerial::writeI32(buf + 8, tileLonE7);
    NowFlockSerial::writeU32(buf + 12, wifiHash);
    NowFlockSerial::writeU32(buf + 16, bleHash);
    return fnv1a32(buf, sizeof(buf));
}

void refreshFlags(CandidateState& c) {
    c.exportable = exportGate(c.authorizationCaps, c.pairEvents, c.passCount, c.confidence);
    c.tier = candidateTier(c.confidence);
    c.state = candidateState(c.confidence, c.exportable);
    c.packedFlags = packedFlags(c.tier, c.state, c.gpsValid, c.exportable);
}

void observeWifiBle(CandidateState& c, bool isWifi, uint32_t nowMs, uint16_t baseScore,
                    uint8_t baseConfidence, uint16_t evidenceBits) {
    bool hadWifi = c.wifiEvents > 0;
    bool hadBle = c.bleEvents > 0;
    uint16_t bucket = passBucket(nowMs);
    bool newMedium = (isWifi && !hadWifi) || (!isWifi && !hadBle);
    bool newPass = c.lastPassBucket != bucket
        && c.lastSeenMs > 0
        && (nowMs - c.lastSeenMs) > MIN_PASS_GAP_MS;
    if (!newMedium && !newPass && c.lastSeenMs > 0) {
        c.lastSeenMs = nowMs;
        c.authorizationCaps &= (uint16_t)~AUTH_STALE;
        refreshFlags(c);
        return;
    }

    c.lastSeenMs = nowMs;
    c.lastPassBucket = bucket;
    if (isWifi) {
        if (c.wifiEvents < 65535u) ++c.wifiEvents;
        c.lastWifiMs = nowMs;
    } else {
        if (c.bleEvents < 65535u) ++c.bleEvents;
        c.lastBleMs = nowMs;
    }

    if (newPass) {
        if (c.passCount < 65535u) ++c.passCount;
        c.evidenceBits |= NowFlock::EVID_REPEAT_PASS;
    }

    c.evidenceBits |= evidenceBits;

    // The current medium was just stamped with nowMs. Compare it to the
    // previous observation of the other medium using unsigned subtraction;
    // this remains correct when millis() crosses 0x80000000 or wraps to zero.
    bool cotime = isWifi
        ? (hadBle && (nowMs - c.lastBleMs) <= COTIME_MS)
        : (hadWifi && (nowMs - c.lastWifiMs) <= COTIME_MS);
    if (c.wifiEvents > 0 && c.bleEvents > 0 && cotime && newMedium) {
        if (c.pairEvents < 65535u) ++c.pairEvents;
    }

    bool paired = c.wifiEvents > 0 && c.bleEvents > 0 && (c.pairEvents > 0 || cotime);
    if (paired) {
        c.evidenceBits |= NowFlock::EVID_COTILE | NowFlock::EVID_COTIME | NowFlock::EVID_SITE_COHERENT;
        c.authorizationCaps &= (uint16_t)~(AUTH_SINGLE_MEDIUM | AUTH_NO_PAIR);
    } else {
        c.authorizationCaps |= AUTH_SINGLE_MEDIUM | AUTH_NO_PAIR;
    }

    uint16_t boost = paired ? 120u : 0u;
    uint32_t raw = (uint32_t)c.siteScore;
    uint32_t target = (uint32_t)baseScore + boost;
    if (target > raw) raw = target;
    if (raw < 320u) raw = 320u;
    if (raw > 980u) raw = 980u;
    c.siteScore = capScore((uint16_t)raw, c.authorizationCaps);

    if (c.authorizationCaps) {
        if (baseConfidence > c.confidence && baseConfidence < 70) c.confidence = baseConfidence;
        else if (c.confidence > 69) c.confidence = 69;
    } else {
        uint8_t bonus = c.passCount >= 2 ? 8u : 0u;
        uint16_t next = (uint16_t)baseConfidence + bonus;
        if (next > c.confidence) c.confidence = next > 100u ? 100u : (uint8_t)next;
    }
    refreshFlags(c);
}

void tickCandidate(CandidateState& c, uint32_t nowMs) {
    if (c.lastSeenMs == 0) return;
    if ((nowMs - c.lastSeenMs) > DECAY_MS) {
        c.state = STATE_DECAYED;
        c.exportable = false;
        c.packedFlags = packedFlags(c.tier, STATE_DECAYED, c.gpsValid, false);
        return;
    }
    if ((nowMs - c.lastSeenMs) > 15000u) {
        c.authorizationCaps |= AUTH_STALE;
        refreshFlags(c);
    }
}

} // namespace NowFlockLsp
