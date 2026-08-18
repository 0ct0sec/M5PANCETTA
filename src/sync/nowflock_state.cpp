#include "nowflock_state.h"
#include <string.h>

namespace NowFlockState {

static Peer peers[NowFlock::PEER_MAX];

void init() {
    reset();
}

void reset() {
    memset(peers, 0, sizeof(peers));
}

Peer* findPeer(uint32_t nodeId) {
    for (auto& p : peers) {
        if (p.used && p.nodeId == nodeId) return &p;
    }
    return nullptr;
}

Peer* upsertPeer(uint32_t nodeId, uint32_t nowMs) {
    Peer* p = findPeer(nodeId);
    if (p) {
        p->lastSeenMs = nowMs;
        return p;
    }

    Peer* oldest = nullptr;
    uint32_t oldestAge = 0;
    for (auto& slot : peers) {
        if (!slot.used) {
            p = &slot;
            break;
        }
        const uint32_t age = nowMs - slot.lastSeenMs;
        if (!oldest || age > oldestAge) {
            oldest = &slot;
            oldestAge = age;
        }
    }
    if (!p) p = oldest;
    memset(p, 0, sizeof(*p));
    p->used = true;
    p->nodeId = nodeId;
    p->lastSeenMs = nowMs;
    p->batteryPct = 0xFF;
    p->channel = NowFlock::DEFAULT_CONTROL_CHANNEL;
    p->reportIntervalS = 10;
    return p;
}

bool acceptSequence(uint32_t nodeId, uint16_t seq, uint32_t nowMs) {
    // A replay is not evidence that a peer is still alive. Check freshness
    // before upsertPeer() refreshes lastSeenMs, otherwise a captured frame can
    // pin stale peers in election and channel-allocation state indefinitely.
    Peer* p = findPeer(nodeId);
    if (p && p->hasSeq && !NowFlock::seqIsFresh(seq, p->lastSeq)) {
        return false;
    }
    if (!p) p = upsertPeer(nodeId, nowMs);
    if (!p) return false;
    p->lastSeq = seq;
    p->hasSeq = true;
    p->lastSeenMs = nowMs;
    return true;
}

bool acceptPeerRequest(uint32_t nodeId, uint32_t nowMs, uint32_t rateLimitMs) {
    Peer* p = findPeer(nodeId);
    if (!p) return false;
    if (p->hasPeerReq && (nowMs - p->lastPeerReqMs) < rateLimitMs) return false;
    p->lastPeerReqMs = nowMs;
    p->hasPeerReq = true;
    return true;
}

uint8_t activePeerCount(uint32_t nowMs) {
    uint8_t count = 0;
    for (const auto& p : peers) {
        if (p.used && (nowMs - p.lastSeenMs) <= NowFlock::REMOTE_ACTIVE_MS) ++count;
    }
    return count;
}

static uint16_t ageS(uint32_t nowMs, uint32_t thenMs) {
    uint32_t age = (nowMs - thenMs) / 1000u;
    return (age > 0xFFFFu) ? 0xFFFFu : (uint16_t)age;
}

uint8_t getActivePeers(PeerView* out, uint8_t maxRows, uint32_t nowMs) {
    if (!out || maxRows == 0) return 0;
    uint8_t count = 0;
    for (const auto& p : peers) {
        if (!p.used || (nowMs - p.lastSeenMs) > NowFlock::REMOTE_ACTIVE_MS) continue;
        PeerView& v = out[count++];
        v.nodeId = p.nodeId;
        v.ageS = ageS(nowMs, p.lastSeenMs);
        v.sightingAgeS = p.lastSightingMs ? ageS(nowMs, p.lastSightingMs) : 0xFFFFu;
        v.sightingRx = p.sightingRx;
        v.claimedChannels = p.claimedChannels;
        v.reportIntervalS = p.reportIntervalS;
        v.role = p.role;
        v.batteryPct = p.batteryPct;
        v.channel = p.channel;
        v.sawSighting = p.lastSightingMs != 0;
        if (count >= maxRows) break;
    }
    return count;
}

uint32_t highestActiveNodeId(uint32_t nowMs, uint32_t ownNodeId) {
    uint32_t high = ownNodeId;
    for (const auto& p : peers) {
        if (!p.used || (nowMs - p.lastSeenMs) > NowFlock::REMOTE_ACTIVE_MS) continue;
        if (p.nodeId > high) high = p.nodeId;
    }
    return high;
}

void noteHello(uint32_t nodeId, const NowFlock::Header& hdr, const NowFlock::HelloBody& body, uint32_t nowMs) {
    Peer* p = upsertPeer(nodeId, nowMs);
    if (!p) return;
    p->role = hdr.role;
    p->batteryPct = hdr.batteryPct;
    p->channel = hdr.channel;
    p->claimedChannels = body.claimedChannels;
    p->reportIntervalS = body.reportIntervalS;
}

void noteAssign(uint32_t nodeId, const NowFlock::Header& hdr, uint32_t nowMs) {
    Peer* p = upsertPeer(nodeId, nowMs);
    if (!p) return;
    p->role = hdr.role;
    p->batteryPct = hdr.batteryPct;
    p->channel = hdr.channel;
}

void noteSighting(uint32_t nodeId, const NowFlock::Header& hdr, uint32_t nowMs) {
    Peer* p = upsertPeer(nodeId, nowMs);
    if (!p) return;
    p->role = hdr.role;
    p->batteryPct = hdr.batteryPct;
    p->channel = hdr.channel;
    p->lastSightingMs = nowMs;
    if (p->sightingRx < 0xFFFFu) ++p->sightingRx;
}

uint16_t unionClaimedChannels(uint32_t nowMs) {
    uint16_t mask = 0;
    for (const auto& p : peers) {
        if (!p.used || (nowMs - p.lastSeenMs) > NowFlock::REMOTE_ACTIVE_MS) continue;
        mask |= p.claimedChannels;
    }
    return (uint16_t)(mask & NowFlock::ALL_CHANNELS_MASK);
}

} // namespace NowFlockState
