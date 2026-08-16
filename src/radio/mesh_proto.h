/**
 * Meshtastic PROTO codec — the wire under M3SH T4LK's rich half.
 *
 * ==[ WHY THIS EXISTS ]== TEXTMSG hands the screen four characters of sender
 * and a body, and that is the whole of it. Who the node is, how strong it
 * arrived, how many hops it crossed, whether anybody acknowledged ours — the
 * radio knows all of it and TEXTMSG carries none of it. PROTO does. This file
 * is the smallest decoder that gets that data onto a 320x240 screen.
 *
 * ==[ NOT NANOPB ]== Meshtastic generates its own bindings and we deliberately
 * do not. We read a fixed handful of fields out of four message types and write
 * two; the generated code for that is a dependency, a build step, and a few KB
 * of descriptor tables to do what a 200-line reader does. The cost of the
 * choice is that field numbers live here as constants instead of in a schema,
 * so they are all quoted with their .proto line and verified in
 * sim/mesh_proto_sim.py against real bytes.
 *
 * ==[ THE THREE THINGS THAT BITE ]== all three are checked by the sim and by
 * test_mesh_proto.cpp against golden frames:
 *
 *   Half of MeshPacket is fixed32, not varint. `from`, `to`, `id` and
 *   `rx_time` are wire type 5, and `rx_snr` is a float in the same width.
 *   Reading them as varints yields numbers that look like node IDs and are
 *   not, which is worse than failing.
 *
 *   The stream is framed, and a UART does not respect frames. 0x94 0xC3 and a
 *   big-endian length arrive in whatever chunks the driver felt like, so this
 *   is a resumable state machine, not a parse-a-buffer function. It also has
 *   to resync: a radio still in TEXTMSG emits plain text, and the magic pair
 *   can appear inside a legitimate payload.
 *
 *   The schema gains fields every release. Anything unrecognised is skipped by
 *   wire type alone, so a firmware update adds unknowns rather than breaking
 *   every packet.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace MeshProto {

// ==[ FRAMING ]== StreamAPI. The length is big-endian, alone among every
// integer on this wire, and 512 is the radio's own ceiling — a larger one is
// proof the magic was a coincidence rather than a header.
static constexpr uint8_t  START1 = 0x94;
static constexpr uint8_t  START2 = 0xC3;
static constexpr uint16_t MAX_FRAME = 512;

// ==[ PORTNUMS ]== portnums.proto. Only the ones this screen reacts to.
static constexpr uint32_t PORT_TEXT = 1;
static constexpr uint32_t PORT_POSITION = 3;
static constexpr uint32_t PORT_NODEINFO = 4;
static constexpr uint32_t PORT_ROUTING = 5;
static constexpr uint32_t PORT_TELEMETRY = 67;

static constexpr uint32_t BROADCAST = 0xFFFFFFFFu;

// ==[ WIRE TYPES ]==
static constexpr uint8_t WT_VARINT = 0;
static constexpr uint8_t WT_FIXED64 = 1;
static constexpr uint8_t WT_LEN = 2;
static constexpr uint8_t WT_FIXED32 = 5;

// ==[ READER ]== one pass over a buffer, no allocation, no copy. `next()`
// returns false at the end or on the first malformed byte — a truncated field
// ends the walk rather than reading past the buffer.
struct Reader {
    const uint8_t* buf;
    size_t         len;
    size_t         pos;

    Reader(const uint8_t* b, size_t l) : buf(b), len(l), pos(0) {}

    uint32_t       field;    // valid after next() returns true
    uint8_t        wireType;
    uint64_t       varint;   // WT_VARINT
    const uint8_t* bytes;    // WT_LEN
    size_t         bytesLen;
    uint32_t       fixed32;  // WT_FIXED32

    bool next();

    // A length-delimited field read as a nested message.
    Reader sub() const { return Reader(bytes, bytesLen); }

    float    asFloat() const;
    int32_t  asInt32() const { return (int32_t)(uint32_t)varint; }
    // Copies at most cap-1 bytes and always terminates. Protobuf strings are
    // not NUL-terminated on the wire, so this is the only safe way out.
    void     copyString(char* out, size_t cap) const;
};

bool readVarint(const uint8_t* buf, size_t len, size_t& pos, uint64_t& out);

// ==[ WRITER ]== bounded, and it tells you when it overflowed rather than
// truncating a packet into something the radio would still transmit.
struct Writer {
    uint8_t* buf;
    size_t   cap;
    size_t   pos;
    bool     overflow;

    Writer(uint8_t* b, size_t c) : buf(b), cap(c), pos(0), overflow(false) {}

    void putVarint(uint64_t v);
    void putTag(uint32_t field, uint8_t wt);
    void putUint32Field(uint32_t field, uint32_t v);
    void putFixed32Field(uint32_t field, uint32_t v);
    void putBytesField(uint32_t field, const uint8_t* data, size_t len);
    void putRaw(const uint8_t* data, size_t len);
    bool ok() const { return !overflow; }
};

// ==[ STREAM ]== the resumable half. Feed it whatever the UART gave you; it
// calls back once per complete frame. The callback runs inside the feed, so it
// must not block — this is on the compositor's thread.
using FrameFn = void (*)(const uint8_t* payload, uint16_t len, void* user);

struct Stream {
    // Sized for the radio's own ceiling plus a header. Anything larger is not
    // a frame we failed to read, it is not a frame.
    uint8_t  buf[MAX_FRAME];
    uint16_t have;      // bytes held in buf
    uint16_t want;      // payload length once the header is complete, else 0
    uint8_t  header[4];
    uint8_t  headerLen;

    // ==[ DIAGNOSTICS ]== the whole point of counting these is that "nothing
    // is happening" and "everything is being discarded" look identical on a
    // screen that only shows messages.
    uint32_t framesSeen;
    uint32_t bytesDropped;   // discarded hunting for the magic
    uint32_t oversizeSeen;   // a length that cannot be true

    void reset();
    void feed(const uint8_t* data, size_t len, FrameFn fn, void* user);
};

// ==[ DECODED SHAPES ]== flat structs rather than the protobuf nesting,
// because the screen wants a row and not a tree.

struct NodeInfo {
    uint32_t num;
    char     shortName[8];
    char     longName[40];
    uint32_t hwModel;
    float    snr;
    uint32_t lastHeard;
    uint32_t battery;      // percent; >100 means "charging/plugged"
    uint32_t hopsAway;
    bool     hasHopsAway;  // absent and zero are different facts
    bool     hasUser;
};

struct Packet {
    uint32_t from;
    uint32_t to;
    uint32_t id;
    uint32_t channel;
    uint32_t portnum;
    uint32_t rxTime;
    float    rxSnr;
    int32_t  rxRssi;
    uint32_t hopLimit;
    uint32_t hopStart;
    uint32_t requestId;    // ROUTING: which of our packets this answers
    uint32_t routingError;  // ROUTING: 0 = delivered
    // Borrowed view into the frame passed to decodeFromRadio(). The caller
    // must consume or copy it before that frame buffer is released or reused.
    const uint8_t* payload;
    size_t         payloadLen;
    bool     hasDecoded;   // false = encrypted, i.e. not our channel

    // hop_start is what the sender set out with, hop_limit is what survived.
    uint8_t hopsCrossed() const {
        return (hopStart > hopLimit) ? (uint8_t)(hopStart - hopLimit) : 0;
    }
};

// FromRadio variants this project acts on. Anything else decodes to NONE and
// is discarded without complaint — the radio streams plenty we do not want.
enum class FromRadioKind : uint8_t { NONE, PACKET, MY_INFO, NODE_INFO,
                                     CONFIG_COMPLETE, REBOOTED };

struct FromRadio {
    FromRadioKind kind;
    Packet        packet;
    NodeInfo      node;
    uint32_t      myNodeNum;
    uint32_t      configCompleteId;
};

// Decodes one frame payload. Returns false only when the frame is malformed;
// an unrecognised variant is a successful decode of kind NONE.
bool decodeFromRadio(const uint8_t* payload, size_t len, FromRadio& out);

// ==[ APP PAYLOADS ]== a node's own broadcasts, which keep the roster fresh
// between the radio's whole-NodeDB dumps.

// NODEINFO_APP carries a bare `User`, NOT a `NodeInfo`. There is no node
// number inside it — that comes from the enclosing packet's `from`, and a
// decoder that read field 1 as the number would get the id *string* instead.
// Fills only the name/hwModel fields; the caller owns everything else.
bool decodeUserPayload(const uint8_t* payload, size_t len, NodeInfo& out);

// TELEMETRY_APP. Returns false when the packet carries no device_metrics at
// all — an environment-only sensor node — because reading that as a battery of
// zero would show a healthy node as flat.
struct Telemetry {
    uint32_t battery;
    float    voltage;
    bool     hasBattery;
};
bool decodeTelemetryPayload(const uint8_t* payload, size_t len, Telemetry& out);

// ==[ ENCODE ]== the two things this project ever sends.
// want_config_id opens the session and makes the radio stream its NodeDB;
// without it the client sees packets but never learns anyone's name.
size_t encodeWantConfig(uint8_t* out, size_t cap, uint32_t configId);

// A text message. `to` is BROADCAST or a node number for a DM — the thing
// TEXTMSG could never do. Returns bytes written, 0 if it would not fit.
size_t encodeTextPacket(uint8_t* out, size_t cap, uint32_t to, uint32_t id,
                        uint32_t channel, const char* text, bool wantAck);

// Wraps a ToRadio body in the StreamAPI header, in place.
size_t frameToRadio(uint8_t* out, size_t cap, const uint8_t* body, size_t len);

}  // namespace MeshProto
