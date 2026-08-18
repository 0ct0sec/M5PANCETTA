/**
 * FLOCKGRAPH summary table.
 *
 * local truth in, privacy-safe rows out.
 */
#pragma once

#include <stdint.h>
#include "nowflock_protocol.h"

namespace NowFlockGraph {

static constexpr uint8_t MAX_CANDIDATES = 16;

struct CandidateView {
    uint32_t candidateId;
    uint32_t originNodeId;
    int32_t tileLatE7;
    int32_t tileLonE7;
    uint16_t siteScore;
    uint8_t confidence;
    uint8_t tier;
    uint8_t peerCoverage;
    bool local;
    bool exportable;
    bool peerCorroborated;
};

void init();
void reset();
void tick(uint32_t nowMs);
uint8_t candidateCount();
uint8_t exportableCount();
uint8_t encodeTopCandidates(NowFlock::CandidateWire* out, uint8_t maxRows, uint32_t nowMs,
                            uint8_t minTier = 2);
uint8_t getTopCandidates(CandidateView* out, uint8_t maxRows, uint32_t nowMs);
void ingestPeerCandidate(uint32_t peerNodeId, const NowFlock::CandidateWire& row, uint32_t nowMs);
bool hasCorroboration();
void observeLocal(bool isWifi, uint32_t nowMs, int32_t tileLatE7, int32_t tileLonE7,
                  uint32_t wifiHash, uint32_t bleHash, uint16_t baseScore, uint8_t baseConfidence,
                  uint16_t evidenceBits, bool gpsValid, bool staleGps,
                  bool utcValid = true, uint16_t observationCaps = 0);

} // namespace NowFlockGraph
