/**
 * Meshtastic PROTO codec — implementation.
 *
 * Every field number below is quoted with the message it belongs to. They are
 * not guesses: sim/mesh_proto_sim.py builds real frames from the same numbers
 * and test_mesh_proto.cpp decodes those exact bytes.
 *
 * Nothing here allocates and nothing blocks. The frame callback runs inside
 * feed(), on the compositor's thread, inside the same budget as everything
 * else the pig is drawing.
 */

#include "mesh_proto.h"

#include <string.h>

namespace MeshProto {

// ==[ VARINT ]== up to ten bytes, because a negative int32 sign-extends to the
// full 64-bit width. rx_rssi is exactly that field, and a reader that stops at
// five bytes reads -95 as a large positive number and then desynchronises on
// the rest of the packet.
bool readVarint(const uint8_t* buf, size_t len, size_t& pos, uint64_t& out) {
    uint64_t result = 0;
    uint8_t  shift = 0;
    while (pos < len) {
        const uint8_t b = buf[pos++];
        result |= (uint64_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) {
            out = result;
            return true;
        }
        shift += 7;
        if (shift > 63) return false;   // malformed; refuse rather than wrap
    }
    return false;   // ran off the end mid-varint
}

bool Reader::next() {
    if (pos >= len) return false;

    uint64_t key = 0;
    if (!readVarint(buf, len, pos, key)) return false;
    field = (uint32_t)(key >> 3);
    wireType = (uint8_t)(key & 7);

    switch (wireType) {
        case WT_VARINT:
            return readVarint(buf, len, pos, varint);

        case WT_FIXED32: {
            if (pos + 4 > len) return false;
            // Little-endian, unlike the frame length four bytes earlier.
            fixed32 = (uint32_t)buf[pos] | ((uint32_t)buf[pos + 1] << 8) |
                      ((uint32_t)buf[pos + 2] << 16) |
                      ((uint32_t)buf[pos + 3] << 24);
            pos += 4;
            return true;
        }

        case WT_FIXED64:
            if (pos + 8 > len) return false;
            pos += 8;   // nothing we read is 64-bit; skip it intact
            return true;

        case WT_LEN: {
            uint64_t n = 0;
            if (!readVarint(buf, len, pos, n)) return false;
            if (n > len - pos) return false;
            bytes = buf + pos;
            bytesLen = (size_t)n;
            pos += (size_t)n;
            return true;
        }

        default:
            // Group wire types were removed from protobuf two decades ago, so
            // seeing one means the buffer is not what we think it is. Stopping
            // is the only safe response — there is no length to skip by.
            return false;
    }
}

float Reader::asFloat() const {
    float f;
    memcpy(&f, &fixed32, sizeof(f));
    return f;
}

void Reader::copyString(char* out, size_t cap) const {
    if (!cap) return;
    size_t n = bytesLen < (cap - 1) ? bytesLen : (cap - 1);
    memcpy(out, bytes, n);
    out[n] = '\0';
}

// ==[ WRITER ]==

void Writer::putVarint(uint64_t v) {
    do {
        if (pos >= cap) { overflow = true; return; }
        uint8_t b = (uint8_t)(v & 0x7F);
        v >>= 7;
        if (v) b |= 0x80;
        buf[pos++] = b;
    } while (v);
}

void Writer::putTag(uint32_t field, uint8_t wt) {
    putVarint(((uint64_t)field << 3) | wt);
}

void Writer::putUint32Field(uint32_t field, uint32_t v) {
    putTag(field, WT_VARINT);
    putVarint(v);
}

void Writer::putFixed32Field(uint32_t field, uint32_t v) {
    putTag(field, WT_FIXED32);
    if (pos + 4 > cap) { overflow = true; return; }
    buf[pos++] = (uint8_t)(v & 0xFF);
    buf[pos++] = (uint8_t)((v >> 8) & 0xFF);
    buf[pos++] = (uint8_t)((v >> 16) & 0xFF);
    buf[pos++] = (uint8_t)((v >> 24) & 0xFF);
}

void Writer::putRaw(const uint8_t* data, size_t len) {
    if (pos + len > cap) { overflow = true; return; }
    memcpy(buf + pos, data, len);
    pos += len;
}

void Writer::putBytesField(uint32_t field, const uint8_t* data, size_t len) {
    putTag(field, WT_LEN);
    putVarint(len);
    putRaw(data, len);
}

// ==[ STREAM ]==

void Stream::reset() {
    have = 0;
    want = 0;
    headerLen = 0;
    framesSeen = 0;
    bytesDropped = 0;
    oversizeSeen = 0;
}

void Stream::feed(const uint8_t* data, size_t len, FrameFn fn, void* user) {
    for (size_t i = 0; i < len; ++i) {
        const uint8_t c = data[i];

        // ==[ HUNTING ]== no header yet. A radio left in TEXTMSG mode emits
        // plain text here, and so does a boot log, so this state is normal
        // rather than exceptional and must not be expensive.
        if (headerLen == 0) {
            if (c == START1) {
                header[0] = c;
                headerLen = 1;
            } else {
                bytesDropped++;
            }
            continue;
        }

        if (headerLen == 1) {
            if (c == START2) {
                header[1] = c;
                headerLen = 2;
            } else if (c == START1) {
                // Two 0x94 in a row: the first was not a header, but this one
                // still might be. Drop exactly one byte, not the pair.
                bytesDropped++;
            } else {
                bytesDropped += 2;
                headerLen = 0;
            }
            continue;
        }

        if (headerLen == 2) { header[2] = c; headerLen = 3; continue; }

        if (headerLen == 3) {
            header[3] = c;
            // Big-endian, alone on this wire.
            const uint16_t declared = (uint16_t)((header[2] << 8) | header[3]);
            if (declared > MAX_FRAME) {
                // The radio cannot emit this, so the magic pair was a
                // coincidence inside somebody's payload. Resync from the byte
                // after the magic rather than swallowing four bytes on faith —
                // a real header may be standing inside what we just read.
                oversizeSeen++;
                bytesDropped++;
                headerLen = 0;
                // Re-examine the three bytes we had buffered, in order. Only
                // the first can start a header we have not already rejected.
                if (header[1] == START1) { header[0] = START1; headerLen = 1; }
                continue;
            }
            want = declared;
            have = 0;
            headerLen = 4;
            if (want == 0) {
                // A zero-length FromRadio is legal and empty. Deliver it so
                // the framing counters stay honest, then go back to hunting.
                framesSeen++;
                if (fn) fn(buf, 0, user);
                headerLen = 0;
            }
            continue;
        }

        // ==[ BODY ]== headerLen == 4.
        buf[have++] = c;
        if (have >= want) {
            framesSeen++;
            if (fn) fn(buf, want, user);
            headerLen = 0;
            have = 0;
            want = 0;
        }
    }
}

// ==[ DECODE ]==

// User, mesh.proto: id=1, long_name=2, short_name=3, hw_model=5.
static void decodeUser(Reader r, NodeInfo& out) {
    while (r.next()) {
        switch (r.field) {
            case 2:
                if (r.wireType == WT_LEN) r.copyString(out.longName,
                                                       sizeof(out.longName));
                break;
            case 3:
                if (r.wireType == WT_LEN) r.copyString(out.shortName,
                                                       sizeof(out.shortName));
                break;
            case 5:
                if (r.wireType == WT_VARINT) out.hwModel = (uint32_t)r.varint;
                break;
            default: break;
        }
    }
    out.hasUser = true;
}

// DeviceMetrics, telemetry.proto: battery_level=1.
static void decodeMetrics(Reader r, NodeInfo& out) {
    while (r.next()) {
        if (r.field == 1 && r.wireType == WT_VARINT) {
            out.battery = (uint32_t)r.varint;
        }
    }
}

// NodeInfo, mesh.proto: num=1, user=2, snr=4, last_heard=5,
// device_metrics=6, hops_away=9.
static void decodeNodeInfo(Reader r, NodeInfo& out) {
    memset(&out, 0, sizeof(out));
    while (r.next()) {
        switch (r.field) {
            case 1:
                if (r.wireType == WT_VARINT) out.num = (uint32_t)r.varint;
                break;
            case 2:
                if (r.wireType == WT_LEN) decodeUser(r.sub(), out);
                break;
            case 4:
                if (r.wireType == WT_FIXED32) out.snr = r.asFloat();
                break;
            case 5:
                if (r.wireType == WT_FIXED32) out.lastHeard = r.fixed32;
                break;
            case 6:
                if (r.wireType == WT_LEN) decodeMetrics(r.sub(), out);
                break;
            case 9:
                if (r.wireType == WT_VARINT) {
                    out.hopsAway = (uint32_t)r.varint;
                    // Absent and zero are different facts: absent means the
                    // radio does not know, zero means a direct neighbour.
                    out.hasHopsAway = true;
                }
                break;
            default: break;
        }
    }
}

// Routing, mesh.proto: error_reason=3. Absent means NONE, i.e. delivered.
static uint32_t decodeRoutingError(Reader r) {
    while (r.next()) {
        if (r.field == 3 && r.wireType == WT_VARINT) {
            return (uint32_t)r.varint;
        }
    }
    return 0;
}

// Data, mesh.proto: portnum=1, payload=2, request_id=6.
static void decodeData(Reader r, Packet& out) {
    while (r.next()) {
        switch (r.field) {
            case 1:
                if (r.wireType == WT_VARINT) out.portnum = (uint32_t)r.varint;
                break;
            case 2:
                if (r.wireType == WT_LEN) {
                    out.payload = r.bytes;
                    out.payloadLen = r.bytesLen;
                }
                break;
            case 6:
                if (r.wireType == WT_FIXED32) out.requestId = r.fixed32;
                break;
            default: break;
        }
    }
    out.hasDecoded = true;
    // The routing payload is a nested message and has to be read after the
    // portnum is known, which is why it is not folded into the loop above:
    // field order on the wire is not guaranteed.
    if (out.portnum == PORT_ROUTING && out.payload) {
        out.routingError = decodeRoutingError(Reader(out.payload,
                                                     out.payloadLen));
    }
}

// MeshPacket, mesh.proto: from=1, to=2, channel=3, decoded=4, encrypted=5,
// id=6, rx_time=7, rx_snr=8, hop_limit=9, rx_rssi=12, hop_start=15.
// Note which are fixed32 — reading them as varints is the classic bug here.
static void decodePacket(Reader r, Packet& out) {
    memset(&out, 0, sizeof(out));
    while (r.next()) {
        switch (r.field) {
            case 1:
                if (r.wireType == WT_FIXED32) out.from = r.fixed32;
                break;
            case 2:
                if (r.wireType == WT_FIXED32) out.to = r.fixed32;
                break;
            case 3:
                if (r.wireType == WT_VARINT) out.channel = (uint32_t)r.varint;
                break;
            case 4:
                if (r.wireType == WT_LEN) decodeData(r.sub(), out);
                break;
            case 6:
                if (r.wireType == WT_FIXED32) out.id = r.fixed32;
                break;
            case 7:
                if (r.wireType == WT_FIXED32) out.rxTime = r.fixed32;
                break;
            case 8:
                if (r.wireType == WT_FIXED32) out.rxSnr = r.asFloat();
                break;
            case 9:
                if (r.wireType == WT_VARINT) out.hopLimit = (uint32_t)r.varint;
                break;
            case 12:
                if (r.wireType == WT_VARINT) out.rxRssi = r.asInt32();
                break;
            case 15:
                if (r.wireType == WT_VARINT) out.hopStart = (uint32_t)r.varint;
                break;
            default:
                // Field 5 is `encrypted`, and landing here with it means the
                // packet is on a channel whose key we do not have. hasDecoded
                // stays false and the caller drops it rather than showing a
                // message with no body.
                break;
        }
    }
}

// FromRadio, mesh.proto: packet=2, my_info=3, node_info=4,
// config_complete_id=7, rebooted=8.
bool decodeFromRadio(const uint8_t* payload, size_t len, FromRadio& out) {
    out.kind = FromRadioKind::NONE;
    out.myNodeNum = 0;
    out.configCompleteId = 0;

    Reader r(payload, len);
    bool sawAny = false;
    while (r.next()) {
        sawAny = true;
        switch (r.field) {
            case 2:
                if (r.wireType == WT_LEN) {
                    decodePacket(r.sub(), out.packet);
                    out.kind = FromRadioKind::PACKET;
                }
                break;
            case 3:
                if (r.wireType == WT_LEN) {
                    Reader mi = r.sub();
                    while (mi.next()) {
                        if (mi.field == 1 && mi.wireType == WT_VARINT) {
                            out.myNodeNum = (uint32_t)mi.varint;
                        }
                    }
                    out.kind = FromRadioKind::MY_INFO;
                }
                break;
            case 4:
                if (r.wireType == WT_LEN) {
                    decodeNodeInfo(r.sub(), out.node);
                    out.kind = FromRadioKind::NODE_INFO;
                }
                break;
            case 7:
                if (r.wireType == WT_VARINT) {
                    out.configCompleteId = (uint32_t)r.varint;
                    out.kind = FromRadioKind::CONFIG_COMPLETE;
                }
                break;
            case 8:
                if (r.wireType == WT_VARINT && r.varint) {
                    out.kind = FromRadioKind::REBOOTED;
                }
                break;
            default:
                // Config, moduleConfig, channels, telemetry, log records. The
                // radio streams a great deal we have no screen for; ignoring
                // it is correct, not a gap.
                break;
        }
    }
    // An empty frame decodes cleanly to NONE. A frame that died mid-field did
    // not, and the difference matters for the "nothing parses" diagnostic.
    return sawAny || len == 0;
}

// ==[ APP PAYLOADS ]==

bool decodeUserPayload(const uint8_t* payload, size_t len, NodeInfo& out) {
    if (!payload || len == 0) return false;
    // Deliberately does NOT memset: the caller has a roster entry with a node
    // number, signal history and hop count already in it, and a bare User
    // knows none of that. Only the name fields are ours to write.
    out.shortName[0] = '\0';
    out.longName[0] = '\0';
    out.hasUser = false;
    decodeUser(Reader(payload, len), out);
    // A User with no names in it tells the roster nothing and must not be
    // allowed to blank a name we already had.
    return out.shortName[0] != '\0' || out.longName[0] != '\0';
}

// Telemetry, telemetry.proto: device_metrics=1.
bool decodeTelemetryPayload(const uint8_t* payload, size_t len,
                            Telemetry& out) {
    out.battery = 0;
    out.voltage = 0.0f;
    out.hasBattery = false;
    if (!payload || len == 0) return false;

    Reader r(payload, len);
    while (r.next()) {
        if (r.field != 1 || r.wireType != WT_LEN) continue;   // device_metrics
        Reader dm = r.sub();
        while (dm.next()) {
            if (dm.field == 1 && dm.wireType == WT_VARINT) {
                out.battery = (uint32_t)dm.varint;
                out.hasBattery = true;
            } else if (dm.field == 2 && dm.wireType == WT_FIXED32) {
                out.voltage = dm.asFloat();
            }
        }
        return out.hasBattery;
    }
    // Environment, air quality or power telemetry. Not a battery reading, and
    // not a failure either — simply nothing this roster can use.
    return false;
}

// ==[ ENCODE ]==

// ToRadio, mesh.proto: want_config_id=3.
size_t encodeWantConfig(uint8_t* out, size_t cap, uint32_t configId) {
    uint8_t body[16];
    Writer w(body, sizeof(body));
    w.putUint32Field(3, configId);
    if (!w.ok()) return 0;
    return frameToRadio(out, cap, body, w.pos);
}

// ToRadio.packet=1 wrapping MeshPacket{to=2, decoded=4, id=6, want_ack=10}
// wrapping Data{portnum=1, payload=2}.
//
// `from` is deliberately left unset: the radio stamps its own node number on
// anything arriving from a client, and a client that fills it in is asserting
// an identity it does not own.
size_t encodeTextPacket(uint8_t* out, size_t cap, uint32_t to, uint32_t id,
                        uint32_t channel, const char* text, bool wantAck) {
    if (!text || !text[0]) return 0;
    const size_t textLen = strlen(text);

    uint8_t data[256];
    Writer dw(data, sizeof(data));
    dw.putUint32Field(1, PORT_TEXT);
    dw.putBytesField(2, (const uint8_t*)text, textLen);
    if (!dw.ok()) return 0;

    uint8_t packet[300];
    Writer pw(packet, sizeof(packet));
    pw.putFixed32Field(2, to);
    if (channel) pw.putUint32Field(3, channel);
    pw.putBytesField(4, data, dw.pos);
    pw.putFixed32Field(6, id);
    // want_ack only makes sense for a DM. A broadcast cannot be acknowledged
    // by everyone, and asking for it floods the mesh with routing traffic.
    if (wantAck && to != BROADCAST) pw.putUint32Field(10, 1);
    if (!pw.ok()) return 0;

    uint8_t body[320];
    Writer bw(body, sizeof(body));
    bw.putBytesField(1, packet, pw.pos);
    if (!bw.ok()) return 0;

    return frameToRadio(out, cap, body, bw.pos);
}

size_t frameToRadio(uint8_t* out, size_t cap, const uint8_t* body, size_t len) {
    if (len > MAX_FRAME || cap < len + 4) return 0;
    out[0] = START1;
    out[1] = START2;
    out[2] = (uint8_t)((len >> 8) & 0xFF);   // big-endian, unlike the payload
    out[3] = (uint8_t)(len & 0xFF);
    memcpy(out + 4, body, len);
    return len + 4;
}

}  // namespace MeshProto
