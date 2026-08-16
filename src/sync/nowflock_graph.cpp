#include "nowflock_graph.h"
#include "nowflock_lsp.h"
#include <string.h>

namespace NowFlockGraph {

struct Candidate {
    bool used = false;
    bool local = false;
    uint32_t originNodeId = 0;
    uint32_t peerSeenMask = 0;
    NowFlockLsp::CandidateState lsp{};
};

static Candidate table[MAX_CANDIDATES];
static bool corroborated = false;

static uint16_t ageSeconds(uint32_t nowMs, uint32_t thenMs) {
    uint32_t age = (nowMs - thenMs) / 1000u;
    return (age > 0xFFFFu) ? 0xFFFFu : (uint16_t)age;
}

static uint8_t peerBit(uint32_t peerNodeId) {
    return (uint8_t)(peerNodeId & 31u);
}

static uint8_t popcount32(uint32_t v) {
    uint8_t n = 0;
    while (v) {
        v &= (v - 1u);
        ++n;
    }
    return n;
}

static int findLocalByTile(int32_t lat, int32_t lon) {
    for (uint8_t i = 0; i < MAX_CANDIDATES; ++i) {
        if (!table[i].used || !table[i].local) continue;
        if (table[i].lsp.tileLatE7 == lat && table[i].lsp.tileLonE7 == lon) return i;
    }
    return -1;
}

static int findMergeableByTile(uint32_t peerNodeId, int32_t lat, int32_t lon) {
    for (uint8_t i = 0; i < MAX_CANDIDATES; ++i) {
        if (!table[i].used) continue;
        if (table[i].lsp.tileLatE7 != lat || table[i].lsp.tileLonE7 != lon) continue;
        if (!table[i].local && table[i].originNodeId == peerNodeId) continue;
        return i;
    }
    return -1;
}

static int findFreeOrWeakest() {
    int freeIdx = -1;
    int weakest = 0;
    for (uint8_t i = 0; i < MAX_CANDIDATES; ++i) {
        if (!table[i].used) {
            freeIdx = i;
            break;
        }
        if (table[i].lsp.siteScore < table[weakest].lsp.siteScore) weakest = i;
    }
    return (freeIdx >= 0) ? freeIdx : weakest;
}

static void copyToWire(const Candidate& c, NowFlock::CandidateWire& row, uint32_t nowMs) {
    const NowFlockLsp::CandidateState& s = c.lsp;
    row.candidateId = s.candidateId;
    row.tileLatE7 = s.tileLatE7;
    row.tileLonE7 = s.tileLonE7;
    row.siteScore = s.siteScore;
    row.evidenceBits = s.evidenceBits;
    row.authorizationCaps = s.authorizationCaps;
    row.wifiEvents = s.wifiEvents;
    row.bleEvents = s.bleEvents;
    row.pairEvents = c.local ? s.pairEvents : 0;
    row.passCount = c.local ? s.passCount : 0;
    row.ageS = ageSeconds(nowMs, s.lastSeenMs);
    row.confidence = s.confidence;
    row.packedFlags = s.packedFlags;
}

void init() {
    reset();
}

void reset() {
    // CandidateState has meaningful non-zero defaults (first pass, baseline
    // score, authorization caps). Byte-zeroing the table silently discarded
    // those defaults and delayed exportability by an extra field pass.
    for (auto& candidate : table) candidate = Candidate{};
    corroborated = false;
}

void tick(uint32_t nowMs) {
    for (auto& c : table) {
        if (!c.used) continue;
        NowFlockLsp::tickCandidate(c.lsp, nowMs);
    }
}

uint8_t candidateCount() {
    uint8_t n = 0;
    for (const auto& c : table) if (c.used) ++n;
    return n;
}

uint8_t exportableCount() {
    uint8_t n = 0;
    for (const auto& c : table) if (c.used && c.lsp.exportable) ++n;
    return n;
}

void observeLocal(bool isWifi, uint32_t nowMs, int32_t tileLatE7, int32_t tileLonE7,
                  uint32_t wifiHash, uint32_t bleHash, uint16_t baseScore, uint8_t baseConfidence,
                  uint16_t evidenceBits, bool gpsValid, bool staleGps) {
    int idx = findLocalByTile(tileLatE7, tileLonE7);
    if (idx < 0) {
        idx = findFreeOrWeakest();
        Candidate& c = table[idx];
        c = Candidate{};
        c.used = true;
        c.local = true;
        c.originNodeId = 0;
        c.lsp.tileLatE7 = tileLatE7;
        c.lsp.tileLonE7 = tileLonE7;
        c.lsp.candidateId = NowFlockLsp::candidateId(tileLatE7, tileLonE7, wifiHash, bleHash);
        c.lsp.gpsValid = gpsValid;
        c.lsp.lastPassBucket = NowFlockLsp::passBucket(nowMs);
        if (!gpsValid) c.lsp.authorizationCaps |= NowFlockLsp::AUTH_NO_GPS;
        if (staleGps) c.lsp.authorizationCaps |= NowFlockLsp::AUTH_STALE_GPS;
    }

    Candidate& c = table[idx];
    c.lsp.gpsValid = gpsValid;
    if (!gpsValid) c.lsp.authorizationCaps |= NowFlockLsp::AUTH_NO_GPS;
    else c.lsp.authorizationCaps &= (uint16_t)~NowFlockLsp::AUTH_NO_GPS;
    if (staleGps) c.lsp.authorizationCaps |= NowFlockLsp::AUTH_STALE_GPS;
    else c.lsp.authorizationCaps &= (uint16_t)~NowFlockLsp::AUTH_STALE_GPS;

    NowFlockLsp::observeWifiBle(c.lsp, isWifi, nowMs, baseScore, baseConfidence, evidenceBits);
}

void ingestPeerCandidate(uint32_t peerNodeId, const NowFlock::CandidateWire& row, uint32_t nowMs) {
    int idx = findMergeableByTile(peerNodeId, row.tileLatE7, row.tileLonE7);

    if (idx >= 0) {
        Candidate& merged = table[idx];
        merged.lsp.evidenceBits |= NowFlock::EVID_PEER_CORROBORATION;
        merged.peerSeenMask |= (1u << peerBit(peerNodeId));
        if (row.confidence >= 70 && row.authorizationCaps == 0) {
            uint16_t boost = (uint16_t)(row.siteScore / 10u);
            if (boost > 60u) boost = 60u;
            uint32_t boosted = (uint32_t)merged.lsp.siteScore + boost;
            merged.lsp.siteScore = boosted > 1000u ? 1000u : (uint16_t)boosted;
        }
        merged.lsp.lastSeenMs = nowMs;
        NowFlockLsp::refreshFlags(merged.lsp);
        corroborated = true;
        return;
    }

    idx = findFreeOrWeakest();
    Candidate& c = table[idx];
    c = Candidate{};
    c.used = true;
    c.local = false;
    c.originNodeId = peerNodeId;
    c.lsp.candidateId = row.candidateId;
    c.lsp.tileLatE7 = row.tileLatE7;
    c.lsp.tileLonE7 = row.tileLonE7;
    c.lsp.siteScore = row.siteScore;
    c.lsp.evidenceBits = (uint16_t)(row.evidenceBits | NowFlock::EVID_PEER_CORROBORATION);
    c.lsp.authorizationCaps = row.authorizationCaps;
    c.lsp.wifiEvents = row.wifiEvents;
    c.lsp.bleEvents = row.bleEvents;
    c.lsp.confidence = row.confidence;
    c.lsp.lastSeenMs = nowMs;
    c.lsp.packedFlags = (uint8_t)(row.packedFlags & ~0x80u);
    c.peerSeenMask = (1u << peerBit(peerNodeId));
    NowFlockLsp::refreshFlags(c.lsp);
    corroborated = true;
}

static int selectBestUnused(bool used[MAX_CANDIDATES], uint32_t nowMs, uint8_t minTier) {
    int best = -1;
    for (uint8_t i = 0; i < MAX_CANDIDATES; ++i) {
        if (!table[i].used || used[i]) continue;
        const NowFlockLsp::CandidateState& s = table[i].lsp;
        uint8_t tier = s.tier;
        bool exportable = s.exportable;
        bool decayed = s.state == NowFlockLsp::STATE_DECAYED;
        if (decayed || (!exportable && tier < minTier)) continue;
        (void)nowMs;
        if (best < 0 || s.siteScore > table[best].lsp.siteScore) best = i;
    }
    return best;
}

uint8_t encodeTopCandidates(NowFlock::CandidateWire* out, uint8_t maxRows, uint32_t nowMs,
                            uint8_t minTier) {
    if (!out || maxRows == 0) return 0;
    bool used[MAX_CANDIDATES] = {};
    uint8_t written = 0;
    while (written < maxRows) {
        int best = selectBestUnused(used, nowMs, minTier);
        if (best < 0) break;
        used[best] = true;
        copyToWire(table[best], out[written++], nowMs);
    }
    return written;
}

uint8_t getTopCandidates(CandidateView* out, uint8_t maxRows, uint32_t nowMs) {
    if (!out || maxRows == 0) return 0;
    NowFlock::CandidateWire rows[NowFlock::SIGHTING_MAX_CANDIDATES];
    if (maxRows > NowFlock::SIGHTING_MAX_CANDIDATES) maxRows = NowFlock::SIGHTING_MAX_CANDIDATES;
    uint8_t n = encodeTopCandidates(rows, maxRows, nowMs, 2);
    for (uint8_t i = 0; i < n; ++i) {
        int idx = -1;
        for (uint8_t j = 0; j < MAX_CANDIDATES; ++j) {
            if (!table[j].used) continue;
            if (table[j].lsp.candidateId == rows[i].candidateId
                && table[j].lsp.tileLatE7 == rows[i].tileLatE7
                && table[j].lsp.tileLonE7 == rows[i].tileLonE7) {
                idx = j;
                break;
            }
        }
        out[i].candidateId = rows[i].candidateId;
        out[i].originNodeId = (idx >= 0) ? table[idx].originNodeId : 0;
        out[i].tileLatE7 = rows[i].tileLatE7;
        out[i].tileLonE7 = rows[i].tileLonE7;
        out[i].siteScore = rows[i].siteScore;
        out[i].confidence = rows[i].confidence;
        out[i].tier = (uint8_t)(rows[i].packedFlags & 0x07u);
        out[i].peerCoverage = (idx >= 0) ? popcount32(table[idx].peerSeenMask) : 0;
        out[i].local = (idx >= 0) ? table[idx].local : false;
        out[i].exportable = (rows[i].packedFlags & 0x80u) != 0;
        out[i].peerCorroborated = (rows[i].evidenceBits & NowFlock::EVID_PEER_CORROBORATION) != 0;
    }
    return n;
}

bool hasCorroboration() {
    return corroborated;
}

} // namespace NowFlockGraph
