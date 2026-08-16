/**
 * NOWFLOCK peer state.
 */
#pragma once

#include <stdint.h>
#include "nowflock_protocol.h"

namespace NowFlockState {

struct Peer {
    bool used;
    uint32_t nodeId;
    uint16_t lastSeq;
    bool hasSeq;          // sequence 0 is valid; do not use it as a sentinel
    uint32_t lastSeenMs;
    uint32_t lastSightingMs;
    uint32_t lastPeerReqMs;
    uint16_t sightingRx;
    uint8_t role;
    uint8_t batteryPct;
    uint8_t channel;
    uint16_t claimedChannels;
    uint16_t reportIntervalS;
};

struct PeerView {
    uint32_t nodeId;
    uint16_t ageS;
    uint16_t sightingAgeS;
    uint16_t sightingRx;
    uint16_t claimedChannels;
    uint16_t reportIntervalS;
    uint8_t role;
    uint8_t batteryPct;
    uint8_t channel;
    bool sawSighting;
};

void init();
void reset();
Peer* upsertPeer(uint32_t nodeId, uint32_t nowMs);
Peer* findPeer(uint32_t nodeId);
bool acceptSequence(uint32_t nodeId, uint16_t seq, uint32_t nowMs);
uint8_t activePeerCount(uint32_t nowMs);
uint8_t getActivePeers(PeerView* out, uint8_t maxRows, uint32_t nowMs);
uint32_t highestActiveNodeId(uint32_t nowMs, uint32_t ownNodeId);
void noteHello(uint32_t nodeId, const NowFlock::Header& hdr, const NowFlock::HelloBody& body, uint32_t nowMs);
void noteAssign(uint32_t nodeId, const NowFlock::Header& hdr, uint32_t nowMs);
void noteSighting(uint32_t nodeId, const NowFlock::Header& hdr, uint32_t nowMs);
uint16_t unionClaimedChannels(uint32_t nowMs);

} // namespace NowFlockState
