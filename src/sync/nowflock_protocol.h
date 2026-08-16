/**
 * NOWFLOCK/FNOW/3 protocol core.
 *
 * broadcast first. callback safe. no raw IDs in summaries.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "nowflock_serial.h"

namespace NowFlock {

static constexpr uint32_t FNOW_MAGIC = 0x574F4E46u; // "FNOW" LE
static constexpr uint8_t FNOW_VERSION = 0x03;
static constexpr uint8_t FNOW_VERSION_V2 = 0x02;
static constexpr uint8_t FNOW_VERSION_V1 = 0x01;

static constexpr size_t ESPNOW_MAX_BYTES = 250;
static constexpr size_t HEADER_SIZE = 24;
static constexpr size_t AUTH_TAG_SIZE = 4;
static constexpr size_t HELLO_BODY_SIZE = 13;
static constexpr size_t ASSIGN_BODY_SIZE = 14;
static constexpr size_t CANDIDATE_BODY_SIZE = 4;
static constexpr size_t CANDIDATE_WIRE_SIZE = 30;
static constexpr size_t SIGHTING_MAX_CANDIDATES = 7;
static constexpr size_t SYNC_BODY_SIZE = 9;
static constexpr size_t PEER_REQ_BODY_SIZE = 1;
static constexpr size_t TARGET_BODY_SIZE = 8;
static constexpr size_t CAPTURE_LINE_MAX = 48;
static constexpr size_t EXPORT_LINE_MAX = 180;
static constexpr uint16_t ALL_CHANNELS_MASK = 0x3FFEu; // bits 1-13
static constexpr uint8_t DEFAULT_CONTROL_CHANNEL = 6;
static constexpr uint32_t DEFAULT_GROUP_KEY = 0xDEADB4D6u;

static constexpr uint32_t HELLO_INTERVAL_MS = 5000;
static constexpr uint32_t ASSIGN_INTERVAL_MS = 2000;
static constexpr uint32_t SIGHTING_INTERVAL_MS = 10000;
static constexpr uint32_t SYNC_INTERVAL_MS = 30000;
static constexpr uint32_t REMOTE_ACTIVE_MS = 15000;
static constexpr uint32_t MASTER_TIMEOUT_MS = 12000;
static constexpr uint32_t ELECTION_GUARD_MS = 6000;
static constexpr uint8_t PEER_MAX = 20;

enum MessageType : uint8_t {
    TYPE_HELLO = 1,
    TYPE_ASSIGN = 2,
    TYPE_SIGHTING = 3,
    TYPE_TARGET = 4,
    TYPE_CAPTURE = 5,
    TYPE_SYNC = 6,
    TYPE_PEER_REQ = 7,
    TYPE_EXPORT_SNAPSHOT = 8,
};

enum Role : uint8_t {
    ROLE_CHILD = 0,
    ROLE_MASTER = 1,
};

enum FrameFlags : uint8_t {
    FLAG_AUTH = 0x01,
    FLAG_LONG_RANGE = 0x02,
    FLAG_RELAY = 0x04,
    FLAG_ALLOWED_MASK = FLAG_AUTH | FLAG_LONG_RANGE | FLAG_RELAY,
};

enum ModeByte : uint8_t {
    MODE_IDLE = 0,
    MODE_HUNT = 1,
    MODE_MISCHIEF = 2,
    MODE_AUTO = 3,
};

enum Capabilities : uint8_t {
    CAP_LSP1 = 0x01,
    CAP_BLE_HEARTBEAT = 0x02,
    CAP_PIGBROTHER = 0x04,
    CAP_ALLOWED_MASK = CAP_LSP1 | CAP_BLE_HEARTBEAT | CAP_PIGBROTHER,
};

enum EvidenceBits : uint16_t {
    EVID_WIFI_FAMILY = 1u << 0,
    EVID_WIFI_STRONG = 1u << 1,
    EVID_WIFI_REPEAT = 1u << 2,
    EVID_BLE_FAMILY = 1u << 3,
    EVID_BLE_STRONG = 1u << 4,
    EVID_BLE_REPEAT = 1u << 5,
    EVID_COTILE = 1u << 6,
    EVID_COTIME = 1u << 7,
    EVID_REPEAT_PASS = 1u << 8,
    EVID_SITE_COHERENT = 1u << 9,
    EVID_PEER_CORROBORATION = 1u << 10,
};

struct Header {
    uint8_t version = FNOW_VERSION;
    uint8_t type = 0;
    uint16_t seq = 0;
    uint32_t nodeId = 0;
    uint32_t uptimeMs = 0;
    uint8_t role = ROLE_CHILD;
    uint8_t channel = DEFAULT_CONTROL_CHANNEL;
    uint8_t batteryPct = 0xFF;
    uint8_t flags = 0;
    uint8_t bodyLen = 0;
};

struct HelloBody {
    uint8_t maxNodes = PEER_MAX;
    uint8_t defaultControlNodes = DEFAULT_CONTROL_CHANNEL;
    uint8_t encryptedPeerLimit = 17;
    uint8_t fgCandidates = 0;
    uint8_t fgExportable = 0;
    uint8_t mode = MODE_IDLE;
    uint8_t capabilities = CAP_LSP1;
    uint16_t claimedChannels = 0;
    uint16_t groupId = 0;
    uint16_t reportIntervalS = 10;
};

struct AssignBody {
    uint8_t controlChannel = DEFAULT_CONTROL_CHANNEL;
    uint8_t maxChildren = DEFAULT_CONTROL_CHANNEL;
    uint16_t reportIntervalS = 10;
    uint16_t channelMask = ALL_CHANNELS_MASK;
    uint32_t utcEpochMin = 0;
    uint32_t masterNodeId = 0;
};

struct CandidateWire {
    uint32_t candidateId = 0;
    int32_t tileLatE7 = 0;
    int32_t tileLonE7 = 0;
    uint16_t siteScore = 0;
    uint16_t evidenceBits = 0;
    uint16_t authorizationCaps = 0;
    uint16_t wifiEvents = 0;
    uint16_t bleEvents = 0;
    uint16_t pairEvents = 0;
    uint16_t passCount = 0;
    uint16_t ageS = 0;
    uint8_t confidence = 0;
    uint8_t packedFlags = 0;
};

struct SyncBody {
    uint32_t utcEpochMin = 0;
    uint32_t uptimeRefMs = 0;
    uint8_t accuracyS = 0;
};

struct PeerReqBody {
    uint8_t minTier = 2;
};

struct TargetBody {
    uint8_t bssid[6] = {};
    uint8_t channel = 0;
    uint8_t pad = 0;
};

inline uint32_t authTag(uint32_t groupKey, uint32_t nodeId, uint16_t seq,
                        const uint8_t* body, size_t bodyLen) {
    uint8_t payload[12];
    NowFlockSerial::writeU32(payload + 0, groupKey ^ nodeId);
    NowFlockSerial::writeU32(payload + 4, (uint32_t)seq);
    NowFlockSerial::writeU32(payload + 8, NowFlockSerial::crc32(body, bodyLen));
    return NowFlockSerial::crc32(payload, sizeof(payload));
}

inline size_t encodedFrameSize(size_t bodyLen, bool auth) {
    return HEADER_SIZE + bodyLen + (auth ? AUTH_TAG_SIZE : 0);
}

inline bool seqIsFresh(uint16_t incomingSeq, uint16_t lastSeq) {
    uint16_t delta = (uint16_t)(incomingSeq - lastSeq);
    return delta != 0 && delta <= 0x7FFFu;
}

inline bool bodyLenValid(uint8_t type, uint8_t bodyLen) {
    switch (type) {
        case TYPE_HELLO:
            return bodyLen == HELLO_BODY_SIZE;
        case TYPE_ASSIGN:
            return bodyLen == ASSIGN_BODY_SIZE;
        case TYPE_SIGHTING:
            if (bodyLen < CANDIDATE_BODY_SIZE) return false;
            return ((bodyLen - CANDIDATE_BODY_SIZE) % CANDIDATE_WIRE_SIZE) == 0
                && ((bodyLen - CANDIDATE_BODY_SIZE) / CANDIDATE_WIRE_SIZE) <= SIGHTING_MAX_CANDIDATES;
        case TYPE_TARGET:
            return bodyLen == 8;
        case TYPE_CAPTURE:
            return bodyLen >= 1 && bodyLen <= 49;
        case TYPE_SYNC:
            return bodyLen == SYNC_BODY_SIZE;
        case TYPE_PEER_REQ:
            return bodyLen == PEER_REQ_BODY_SIZE;
        case TYPE_EXPORT_SNAPSHOT:
            return bodyLen >= 1 && bodyLen <= (1 + EXPORT_LINE_MAX);
        default:
            return false;
    }
}

inline void encodeHeader(const Header& h, uint8_t* out) {
    NowFlockSerial::writeU32(out + 0, FNOW_MAGIC);
    NowFlockSerial::writeU8(out + 4, h.version);
    NowFlockSerial::writeU8(out + 5, h.type);
    NowFlockSerial::writeU16(out + 6, h.seq);
    NowFlockSerial::writeU32(out + 8, h.nodeId);
    NowFlockSerial::writeU32(out + 12, h.uptimeMs);
    NowFlockSerial::writeU8(out + 16, h.role);
    NowFlockSerial::writeU8(out + 17, h.channel);
    NowFlockSerial::writeU8(out + 18, h.batteryPct);
    NowFlockSerial::writeU8(out + 19, h.flags);
    NowFlockSerial::writeU8(out + 20, h.bodyLen);
    out[21] = 0;
    out[22] = 0;
    out[23] = 0;
}

inline bool decodeHeader(const uint8_t* data, size_t len, Header& out) {
    if (!data || len < HEADER_SIZE) return false;
    if (NowFlockSerial::readU32(data + 0) != FNOW_MAGIC) return false;
    out.version = NowFlockSerial::readU8(data + 4);
    out.type = NowFlockSerial::readU8(data + 5);
    out.seq = NowFlockSerial::readU16(data + 6);
    out.nodeId = NowFlockSerial::readU32(data + 8);
    out.uptimeMs = NowFlockSerial::readU32(data + 12);
    out.role = NowFlockSerial::readU8(data + 16);
    out.channel = NowFlockSerial::readU8(data + 17);
    out.batteryPct = NowFlockSerial::readU8(data + 18);
    out.flags = NowFlockSerial::readU8(data + 19);
    out.bodyLen = NowFlockSerial::readU8(data + 20);
    return true;
}

inline bool encodeHelloBody(const HelloBody& b, uint8_t* out) {
    if (!out) return false;
    out[0] = b.maxNodes;
    out[1] = b.defaultControlNodes;
    out[2] = b.encryptedPeerLimit;
    out[3] = b.fgCandidates;
    out[4] = b.fgExportable;
    out[5] = b.mode;
    out[6] = b.capabilities;
    NowFlockSerial::writeU16(out + 7, b.claimedChannels);
    NowFlockSerial::writeU16(out + 9, b.groupId);
    NowFlockSerial::writeU16(out + 11, b.reportIntervalS);
    return true;
}

inline bool decodeHelloBody(const uint8_t* data, size_t len, HelloBody& out) {
    if (!data || len != HELLO_BODY_SIZE) return false;
    out.maxNodes = data[0];
    out.defaultControlNodes = data[1];
    out.encryptedPeerLimit = data[2];
    out.fgCandidates = data[3];
    out.fgExportable = data[4];
    out.mode = data[5];
    out.capabilities = data[6];
    out.claimedChannels = NowFlockSerial::readU16(data + 7);
    out.groupId = NowFlockSerial::readU16(data + 9);
    out.reportIntervalS = NowFlockSerial::readU16(data + 11);
    return true;
}

inline bool encodeAssignBody(const AssignBody& b, uint8_t* out) {
    if (!out) return false;
    out[0] = b.controlChannel;
    out[1] = b.maxChildren;
    NowFlockSerial::writeU16(out + 2, b.reportIntervalS);
    NowFlockSerial::writeU16(out + 4, b.channelMask);
    NowFlockSerial::writeU32(out + 6, b.utcEpochMin);
    NowFlockSerial::writeU32(out + 10, b.masterNodeId);
    return true;
}

inline bool decodeAssignBody(const uint8_t* data, size_t len, AssignBody& out) {
    if (!data || len != ASSIGN_BODY_SIZE) return false;
    out.controlChannel = data[0];
    out.maxChildren = data[1];
    out.reportIntervalS = NowFlockSerial::readU16(data + 2);
    out.channelMask = NowFlockSerial::readU16(data + 4);
    out.utcEpochMin = NowFlockSerial::readU32(data + 6);
    out.masterNodeId = NowFlockSerial::readU32(data + 10);
    return true;
}

inline bool decodeSyncBody(const uint8_t* data, size_t len, SyncBody& out) {
    if (!data || len != SYNC_BODY_SIZE) return false;
    out.utcEpochMin = NowFlockSerial::readU32(data + 0);
    out.uptimeRefMs = NowFlockSerial::readU32(data + 4);
    out.accuracyS = data[8];
    return true;
}

inline bool encodeSyncBody(const SyncBody& b, uint8_t* out) {
    if (!out) return false;
    NowFlockSerial::writeU32(out + 0, b.utcEpochMin);
    NowFlockSerial::writeU32(out + 4, b.uptimeRefMs);
    out[8] = b.accuracyS;
    return true;
}

inline bool encodeTargetBody(const TargetBody& b, uint8_t* out) {
    if (!out) return false;
    memcpy(out, b.bssid, 6);
    out[6] = b.channel;
    out[7] = 0;
    return true;
}

inline bool decodeTargetBody(const uint8_t* data, size_t len, TargetBody& out) {
    if (!data || len != TARGET_BODY_SIZE) return false;
    memcpy(out.bssid, data, 6);
    out.channel = data[6];
    out.pad = data[7];
    return out.channel >= 1 && out.channel <= 13;
}

inline bool encodePeerReqBody(const PeerReqBody& b, uint8_t* out) {
    if (!out) return false;
    out[0] = b.minTier;
    return true;
}

inline bool decodePeerReqBody(const uint8_t* data, size_t len, PeerReqBody& out) {
    if (!data || len != PEER_REQ_BODY_SIZE) return false;
    out.minTier = data[0];
    return true;
}

inline size_t encodeCaptureBody(const uint8_t* line, uint8_t lineLen, uint8_t* out, size_t outCap) {
    if (!out || lineLen > CAPTURE_LINE_MAX || outCap < (size_t)lineLen + 1u) return 0;
    out[0] = lineLen;
    if (lineLen > 0 && line) memcpy(out + 1, line, lineLen);
    return (size_t)lineLen + 1u;
}

inline bool decodeCaptureBody(const uint8_t* data, size_t len, const uint8_t*& line, uint8_t& lineLen) {
    line = nullptr;
    lineLen = 0;
    if (!data || len < 1) return false;
    lineLen = data[0];
    if (lineLen > CAPTURE_LINE_MAX || len != (size_t)lineLen + 1u) return false;
    line = data + 1;
    return true;
}

inline size_t encodeExportSnapshotBody(const uint8_t* line, uint8_t lineLen, uint8_t* out, size_t outCap) {
    if (!out || lineLen > EXPORT_LINE_MAX || outCap < (size_t)lineLen + 1u) return 0;
    out[0] = lineLen;
    if (lineLen > 0 && line) memcpy(out + 1, line, lineLen);
    return (size_t)lineLen + 1u;
}

inline bool decodeExportSnapshotBody(const uint8_t* data, size_t len, const uint8_t*& line, uint8_t& lineLen) {
    line = nullptr;
    lineLen = 0;
    if (!data || len < 1) return false;
    lineLen = data[0];
    if (lineLen > EXPORT_LINE_MAX || len != (size_t)lineLen + 1u) return false;
    line = data + 1;
    return true;
}

inline bool exportLineHasProfile(const uint8_t* line, uint8_t lineLen) {
    if (!line || lineLen < 8) return false;
    static const char prefix[] = "profile=";
    if (lineLen < sizeof(prefix) - 1) return false;
    for (size_t i = 0; i < sizeof(prefix) - 1; ++i) {
        if (line[i] != (uint8_t)prefix[i]) return false;
    }
    return true;
}

inline bool encodeCandidateWire(const CandidateWire& c, uint8_t* out) {
    if (!out) return false;
    NowFlockSerial::writeU32(out + 0, c.candidateId);
    NowFlockSerial::writeI32(out + 4, c.tileLatE7);
    NowFlockSerial::writeI32(out + 8, c.tileLonE7);
    NowFlockSerial::writeU16(out + 12, c.siteScore);
    NowFlockSerial::writeU16(out + 14, c.evidenceBits);
    NowFlockSerial::writeU16(out + 16, c.authorizationCaps);
    NowFlockSerial::writeU16(out + 18, c.wifiEvents);
    NowFlockSerial::writeU16(out + 20, c.bleEvents);
    NowFlockSerial::writeU16(out + 22, c.pairEvents);
    NowFlockSerial::writeU16(out + 24, c.passCount);
    NowFlockSerial::writeU16(out + 26, c.ageS);
    out[28] = c.confidence;
    out[29] = c.packedFlags;
    return true;
}

inline bool decodeCandidateWire(const uint8_t* data, size_t len, CandidateWire& out) {
    if (!data || len < CANDIDATE_WIRE_SIZE) return false;
    out.candidateId = NowFlockSerial::readU32(data + 0);
    out.tileLatE7 = NowFlockSerial::readI32(data + 4);
    out.tileLonE7 = NowFlockSerial::readI32(data + 8);
    out.siteScore = NowFlockSerial::readU16(data + 12);
    out.evidenceBits = NowFlockSerial::readU16(data + 14);
    out.authorizationCaps = NowFlockSerial::readU16(data + 16);
    out.wifiEvents = NowFlockSerial::readU16(data + 18);
    out.bleEvents = NowFlockSerial::readU16(data + 20);
    out.pairEvents = NowFlockSerial::readU16(data + 22);
    out.passCount = NowFlockSerial::readU16(data + 24);
    out.ageS = NowFlockSerial::readU16(data + 26);
    out.confidence = data[28];
    out.packedFlags = data[29];
    return true;
}

inline size_t encodeFrame(Header h, const uint8_t* body, size_t bodyLen,
                          uint32_t groupKey, uint8_t* out, size_t outLen) {
    if (!out || bodyLen > 255u || !bodyLenValid(h.type, (uint8_t)bodyLen)) return 0;
    bool auth = groupKey != 0;
    size_t total = encodedFrameSize(bodyLen, auth);
    if (total > ESPNOW_MAX_BYTES || outLen < total) return 0;
    h.version = FNOW_VERSION;
    h.bodyLen = (uint8_t)bodyLen;
    if (auth) h.flags |= FLAG_AUTH;
    else h.flags &= (uint8_t)~FLAG_AUTH;
    encodeHeader(h, out);
    for (size_t i = 0; i < bodyLen; ++i) {
        out[HEADER_SIZE + i] = body ? body[i] : 0;
    }
    if (auth) {
        uint32_t tag = authTag(groupKey, h.nodeId, h.seq, body, bodyLen);
        NowFlockSerial::writeU32(out + HEADER_SIZE + bodyLen, tag);
    }
    return total;
}

inline bool decodeFrame(const uint8_t* data, size_t len, uint32_t groupKey,
                        uint32_t ownNodeId, Header& hdr, const uint8_t*& body) {
    body = nullptr;
    if (!data || len < HEADER_SIZE || len > ESPNOW_MAX_BYTES) return false;
    if (!decodeHeader(data, len, hdr)) return false;
    if (hdr.version != FNOW_VERSION || hdr.nodeId == 0 || hdr.nodeId == ownNodeId) return false;
    if ((hdr.flags & ~FLAG_ALLOWED_MASK) != 0) return false;
    if (!bodyLenValid(hdr.type, hdr.bodyLen)) return false;
    bool auth = (hdr.flags & FLAG_AUTH) != 0;
    size_t total = encodedFrameSize(hdr.bodyLen, auth);
    if (total != len || total > ESPNOW_MAX_BYTES) return false;
    body = data + HEADER_SIZE;
    if (auth && groupKey != 0) {
        uint32_t got = NowFlockSerial::readU32(data + HEADER_SIZE + hdr.bodyLen);
        uint32_t want = authTag(groupKey, hdr.nodeId, hdr.seq, body, hdr.bodyLen);
        if (got != want) return false;
    }
    return true;
}

} // namespace NowFlock
