/**
 * NOWFLOCK LSP-1 — local scoring profile (RFC-F0690-v3 §10.3).
 */
#pragma once

#include <stdint.h>
#include "nowflock_protocol.h"

namespace NowFlockLsp {

static constexpr int32_t TILE_GRID_E7 = 5000;
static constexpr uint32_t PASS_BUCKET_MS = 30000u;
static constexpr uint32_t DECAY_MS = 120000u;
static constexpr uint32_t MIN_PASS_GAP_MS = 4000u;
static constexpr uint32_t COTIME_MS = 5000u;

static constexpr uint16_t AUTH_SINGLE_MEDIUM = 1u << 2;
static constexpr uint16_t AUTH_NO_PAIR = 1u << 5;
static constexpr uint16_t AUTH_STALE = 1u << 6;
static constexpr uint16_t AUTH_NO_GPS = 1u << 0;
static constexpr uint16_t AUTH_STALE_GPS = 1u << 1;
static constexpr uint16_t AUTH_RANDOMIZED_BLE = 1u << 4;
static constexpr uint16_t AUTH_NO_UTC = 1u << 7;

static constexpr uint8_t TIER_MEDIUM = 2u;
static constexpr uint8_t STATE_DECAYED = 5u;

struct CandidateState {
    uint32_t candidateId = 0;
    int32_t tileLatE7 = 0;
    int32_t tileLonE7 = 0;
    uint16_t siteScore = 320;
    uint16_t evidenceBits = 0;
    uint16_t authorizationCaps = AUTH_SINGLE_MEDIUM | AUTH_NO_PAIR;
    uint16_t wifiEvents = 0;
    uint16_t bleEvents = 0;
    uint16_t pairEvents = 0;
    uint16_t passCount = 1;
    uint8_t confidence = 0;
    uint32_t lastSeenMs = 0;
    // Validity is carried by wifiEvents/bleEvents. Unsigned timestamps keep
    // co-time pairing correct across the millis() sign bit and full wrap.
    uint32_t lastWifiMs = 0;
    uint32_t lastBleMs = 0;
    uint16_t lastPairBucket = 0;
    bool hasPairBucket = false;
    uint16_t lastPassBucket = 0;
    bool gpsValid = false;
    bool exportable = false;
    uint8_t tier = TIER_MEDIUM;
    uint8_t state = 2;
    uint8_t packedFlags = 0;
};

int32_t quantizeTileE7(int32_t coordE7);
uint16_t passBucket(uint32_t nowMs);
uint8_t candidateTier(uint8_t confidence);
uint8_t candidateState(uint8_t confidence, bool exportable);
uint16_t capScore(uint16_t score, uint16_t authCaps);
bool exportGate(uint16_t authCaps, uint16_t pairEvents, uint16_t passCount, uint8_t confidence);
uint8_t packedFlags(uint8_t tier, uint8_t state, bool gpsValid, bool exportable);
uint32_t candidateId(int32_t tileLatE7, int32_t tileLonE7, uint32_t wifiHash, uint32_t bleHash);
void refreshFlags(CandidateState& c);
void observeWifiBle(CandidateState& c, bool isWifi, uint32_t nowMs, uint16_t baseScore,
                    uint8_t baseConfidence, uint16_t evidenceBits);
void tickCandidate(CandidateState& c, uint32_t nowMs);

} // namespace NowFlockLsp
