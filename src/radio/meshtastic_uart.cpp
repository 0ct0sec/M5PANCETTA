/**
 * Meshtastic UART Bridge — Implementation
 *
 * ==[ TWO LEVELS ]== bytes make lines, lines make messages, and the second
 * level is what this file gained once the firmware's actual output shape was
 * known. A line ends at a terminator or a quiet gap; a *message* ends at a
 * blank line, because the radio brackets every one of them with println().
 * Between those two rules a payload containing newlines stays one message
 * instead of arriving as three, two of them from nobody.
 *
 * Nothing here blocks and nothing here allocates after begin(), because this
 * runs inside the same frame budget as the compositor.
 */

#include "meshtastic_uart.h"

#include <Arduino.h>
#include <HardwareSerial.h>
#include <stdio.h>
#include <string.h>

#include "mesh_proto.h"
#include "../util/debug_log.h"
#include "../util/psram_block.h"

namespace Mesh {

using MeshUartPolicy::LINE_CAP;
using MeshUartPolicy::TX_QUEUE_DEPTH;
using MeshUartPolicy::ECHO_WINDOW_MS;
using MeshUartPolicy::NOISE_MIN_SAMPLE;

// ==[ UART ]==
static HardwareSerial meshSerial(MeshUartPolicy::UART_NUM);
static bool     started = false;
static uint8_t  activeRxPin = MeshUartPolicy::RX_PIN;
static uint8_t  activeTxPin = MeshUartPolicy::TX_PIN;
static uint32_t activeBaud = MeshUartPolicy::DEFAULT_BAUD;
static uint32_t quietFlushMs = MeshUartPolicy::QUIET_FLUSH_FLOOR_MS;

// The driver's own RX buffer only has to survive one frame's worth of
// arrivals; a single mesh text message is 240 bytes at the very most.
static constexpr size_t RX_BUFFER_BYTES = 512;

// A software TX buffer, so writing a full payload never stalls the frame. With
// none, write() blocks until the bytes fit the 128-byte hardware FIFO, which at
// 9600 baud is 109ms of the compositor's budget spent watching a shift register.
static constexpr size_t TX_BUFFER_BYTES = 320;

// A mismatched baud rate delivers bytes several times faster than they were
// sent, so the drain needs a ceiling that is not "whatever arrived". One RX
// buffer per call is plenty and bounds the worst frame.
static constexpr uint16_t MAX_DRAIN_PER_CALL = RX_BUFFER_BYTES;

// ==[ BRIDGE STATE ]== one PSRAM block, allocated once at begin().
// Core2's DRAM has no room for this: 64 messages plus the assembly buffer is
// ~16.5KB, and a static version of it overflowed dram0_0_seg by itself. Even
// the per-byte line buffer lives here — at 38400 baud that is four bytes per
// millisecond, which PSRAM does not notice.
struct BridgeState {
    Message ring[MAX_MESSAGES];
    char    rxLine[LINE_CAP];
    // ==[ PROTO ]== both live here rather than as statics for the same reason
    // the ring does: the frame assembler is another 512 bytes and the roster
    // another ~2.3KB, and Core2's dram0_0_seg has neither to spare.
    MeshProto::Stream proto;
    Node              nodes[MeshUartPolicy::PROTO_MAX_NODES];
    // The roster is stored in arrival order and *read* in a useful one. An
    // index rather than a sorted array because touchNode hands out pointers
    // into nodes[], and re-sorting under a live pointer would silently move
    // one node's telemetry onto another.
    uint8_t           nodeOrder[MeshUartPolicy::PROTO_MAX_NODES];
};
static BridgeState* state = nullptr;

// ==[ CODEC ]== which dialect the cable is carrying. Set at begin() and never
// mid-session: the frame assembler and the line assembler have incompatible
// ideas about what a byte means.
static Codec      activeCodec = Codec::TEXTMSG;
static ProtoState protoState = ProtoState::NA;
static uint32_t   myNodeNum = 0;
static uint8_t    nodeCount = 0;
static bool       nodeOrderDirty = true;
static uint32_t   handshakeSentMs = 0;
static uint32_t   handshakeId = 0;
// Outgoing packet ids have to be unique per session and are what an ack's
// request_id points back at. Seeded off millis() so a reboot mid-conversation
// does not reuse the ids the far end is still answering.
static uint32_t   nextPacketId = 0;

// ==[ THE TWO SMALL TABLES ]== both address messages by seq rather than
// carrying a copy of the text, because the ring already has the text and a
// second copy is 234 bytes per slot that can go out of step with the first.
//
// They are deliberately separate. A pending write blocks the queue; an echo
// watch must not, or four messages sent to a radio with serial.echo off would
// lock the composer for the full echo window with nothing coming.
struct TxSlot {
    uint32_t seq;
    bool     used;
};
struct EchoWatch {
    uint32_t seq;
    uint32_t atMs;
    bool     confirmed;
    bool     used;
};
static TxSlot    txQueue[TX_QUEUE_DEPTH];
static EchoWatch echoWatch[TX_QUEUE_DEPTH];

static uint16_t rxLineLen = 0;
static uint32_t lastByteMs = 0;
static bool     lastWasCR = false;
static uint8_t  ringHead = 0;   // next slot to write
static uint8_t  ringCount = 0;
// ==[ TWO COUNTERS ]== `revision` bumps on any change at all and is the cheap
// "redraw something" trigger. `ringRevision` bumps only when the scrollback
// itself moves — a message filed, extended, or its delivery mark changed.
//
// They are separate because the roster is far busier than the chat: every
// decoded packet refreshes a node's SNR and hop count, so a position broadcast
// from someone nobody has ever messaged bumps `revision`. The chat screen
// rebuilds a whole wrap index off its counter, and paying for that on traffic
// that cannot have changed a single chat row is the most expensive way to
// redraw nothing.
static uint32_t revision = 0;
static uint32_t ringRevision = 0;
// Doubles as "anything has ever been understood on this link": every writer
// sets it, begin() clears it, and 0 is therefore the whole of what
// getLinkState needs to tell "never heard" apart from "heard, then quiet".
static uint32_t lastRxMs = 0;
static uint8_t  unread = 0;
static uint8_t  unannounced = 0;
// Never reset, not even by clear(): a sequence handed out twice would let a
// parked reader re-anchor onto a different message than the one it was reading.
static uint32_t nextSeq = 1;

// The message a continuation line may extend, named by sequence so a ring wrap
// between the two lines cannot silently redirect it at a different message.
static uint32_t openSeq = 0;

// ==[ TX PACING ]== when the wire is next allowed to carry a message. Armed
// only after the first write, so the first message of a session goes at once.
static uint32_t txReadyAtMs = 0;
static bool     txArmed = false;

// ==[ DIAGNOSTICS ]==
static bool     echoSeen = false;
static bool     everParsed = false;
static uint32_t rxBytes = 0;
static uint16_t rxMessages = 0;
static uint16_t txMessages = 0;

// ==[ WIRE TRACE ]== -DHAMLET_MESH_TRACE=1 turns this bridge into a sniffer:
// every byte the C6L puts on the cable is hexdumped to the USB console, and a
// heartbeat says what the parser made of them. It answers the three questions
// the on-screen link state cannot tell apart, because all three look like a
// quiet mesh from up there:
//
//   no bytes at all      -> the cable is on the wrong Grove port, the polarity
//                           is RX<->RX, or serial.enabled is false on the C6L
//   bytes, none parse    -> serial.baud disagrees, or mode is not TEXTMSG
//   bytes that parse     -> the link is fine and the mesh is simply quiet
//
// The flag itself is declared in meshtastic_uart.h — hamlet.cpp's boot path
// reads it too, to force the bridge up when the setting is off.
#if HAMLET_MESH_TRACE
static constexpr uint8_t  TRACE_COLS = 16;
static constexpr uint32_t TRACE_BEAT_MS = 2000;
static uint8_t  traceBuf[TRACE_COLS];
static uint8_t  traceLen = 0;
static uint32_t traceOffset = 0;
static uint32_t traceBeatMs = 0;

static void traceFlush() {
    if (traceLen == 0) return;
    char hex[TRACE_COLS * 3 + 1];
    char asc[TRACE_COLS + 1];
    for (uint8_t i = 0; i < traceLen; ++i) {
        const uint8_t c = traceBuf[i];
        snprintf(hex + i * 3, 4, "%02X ", c);
        asc[i] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
    }
    hex[traceLen * 3] = '\0';
    asc[traceLen] = '\0';
    Serial.printf("[MESHRAW] %06lu  %-48s|%s|\n", (unsigned long)traceOffset,
                  hex, asc);
    traceOffset += traceLen;
    traceLen = 0;
}

static void traceByte(uint8_t c) {
    traceBuf[traceLen++] = c;
    if (traceLen == TRACE_COLS) traceFlush();
}

// Shared by both codecs, because "is anything arriving and is any of it
// understood" is the same question whichever dialect is on the cable. The
// PROTO half of the line is blank under TEXTMSG rather than zeroed — a frame
// count of 0 on a codec that has no frames is noise, not information.
static void traceHeartbeat(uint32_t now) {
    if ((uint32_t)(now - traceBeatMs) < TRACE_BEAT_MS) return;
    traceBeatMs = now;

    char extra[80];
    extra[0] = '\0';
    if (activeCodec == Codec::PROTO && state) {
        const char* st = "?";
        switch (protoState) {
            case ProtoState::OPENING: st = "OPENING"; break;
            case ProtoState::SYNCING: st = "SYNCING"; break;
            case ProtoState::READY:   st = "READY";   break;
            default: break;
        }
        snprintf(extra, sizeof(extra),
                 "  proto=%s frames=%lu drop=%lu big=%lu nodes=%u me=%08lx",
                 st, (unsigned long)state->proto.framesSeen,
                 (unsigned long)state->proto.bytesDropped,
                 (unsigned long)state->proto.oversizeSeen,
                 (unsigned)nodeCount, (unsigned long)myNodeNum);
    }

    Serial.printf(
        "[MESH?] %s rx=%lu bytes  msgs=%u  parsed=%s  ack=%s  tx=%u  "
        "quiet=%lums  rx=%u tx=%u @%lu%s\n",
        MeshUartPolicy::codecLabel(activeCodec),
        (unsigned long)rxBytes, (unsigned)rxMessages,
        everParsed ? "YES" : "no", echoSeen ? "YES" : "no",
        (unsigned)txMessages,
        lastRxMs ? (unsigned long)(now - lastRxMs) : 0UL,
        (unsigned)activeRxPin, (unsigned)activeTxPin,
        (unsigned long)activeBaud, extra);
}
#endif

// ==[ INTERNAL ]==

static bool ensureState() {
    if (state) return true;
    state = PsramBlock::allocFor<BridgeState>();
    if (!state) {
        HAMLET_LOGLN("[MESH] state alloc failed");
        return false;
    }
    return true;
}

// How long len bytes occupy the wire. 8N1 is ten bits per byte.
static uint32_t wireMs(uint16_t len) {
    if (activeBaud == 0) return 0;
    return (uint32_t)((10ULL * 1000ULL * len) / activeBaud);
}

// Anything that moves the scrollback bumps both, so a caller watching only
// `revision` still sees chat changes and one watching `ringRevision` never
// sees roster churn.
static void bumpRing() {
    ringRevision++;
    revision++;
}

static Message* findBySeq(uint32_t seq) {
    if (!state) return nullptr;
    for (uint8_t i = 0; i < ringCount; ++i) {
        const uint8_t idx =
            (uint8_t)((ringHead + MAX_MESSAGES - ringCount + i) % MAX_MESSAGES);
        if (state->ring[idx].seq == seq) return &state->ring[idx];
    }
    return nullptr;
}

// Returns the sequence assigned, or 0 when nothing was filed.
static uint32_t append(const char* sender, const char* body, uint32_t now,
                       bool outgoing) {
    if (!ensureState()) return 0;
    if (!body || !body[0]) return 0;

    Message& slot = state->ring[ringHead];
    strncpy(slot.sender, sender && sender[0] ? sender : "?",
            sizeof(slot.sender) - 1);
    slot.sender[sizeof(slot.sender) - 1] = '\0';
    strncpy(slot.body, body, sizeof(slot.body) - 1);
    slot.body[sizeof(slot.body) - 1] = '\0';
    slot.atMs = now;
    slot.seq = nextSeq++;
    slot.outgoing = outgoing;
    slot.txState = outgoing ? TxState::QUEUED : TxState::NONE;

    // The PROTO fields default to "we do not know", which is exactly right
    // under TEXTMSG and gets overwritten by the caller under PROTO. Leaving
    // stale values from the previous tenant of this ring slot would attach one
    // node's hop count to another node's message.
    slot.toNum = BROADCAST_ADDR;
    slot.packetId = 0;
    slot.hops = 0;
    slot.hasHops = false;
    slot.direct = false;

    openSeq = slot.seq;

    ringHead = (uint8_t)((ringHead + 1) % MAX_MESSAGES);
    if (ringCount < MAX_MESSAGES) ringCount++;
    bumpRing();

    // Your own message is not news to you.
    if (!outgoing) {
        rxMessages++;
        if (unread < 255) unread++;
        if (unannounced < 255) unannounced++;
    }
    return slot.seq;
}

// The payload had a newline in it. Extend the entry already filed rather than
// landing a second, senderless one — and do not touch the unread counters,
// because the reader was already told about this message when its first line
// arrived.
static void appendContinuation(const char* text) {
    if (!state || openSeq == 0) return;
    Message* msg = findBySeq(openSeq);
    if (!msg) {   // the ring wrapped over it
        openSeq = 0;
        return;
    }
    const size_t have = strlen(msg->body);
    if (have + 2 >= BODY_LEN) return;
    msg->body[have] = '\n';
    strncpy(msg->body + have + 1, text, BODY_LEN - have - 2);
    msg->body[BODY_LEN - 1] = '\0';
    bumpRing();
}

// ==[ A RECEIPT LANDED ]== the four things that are true the moment one of our
// messages is accounted for, whichever path said so. Both callers used to spell
// this out, which is four pieces of state that have to be updated together kept
// in two places.
//
// echoSeen is what lets expireEchoWatch() draw a conclusion from silence later:
// a receipt of any kind proves the path works here, so a later message that
// never gets one has actually failed rather than merely being unobservable.
static void confirmWatch(EchoWatch& w, Message& msg, TxState st) {
    w.confirmed = true;
    msg.txState = st;
    echoSeen = true;
    everParsed = true;
    bumpRing();
}

// The firmware's echo path prints our payload back with no sender prefix and no
// newline on either side, from inside handleReceived() — i.e. after the router
// has taken the packet. Checked before anything else and against the RAW line,
// because what we sent may itself have looked like "name: body" and must not
// come back as an inbound message from a node called "name".
static bool tryEcho(const char* text) {
    if (!state) return false;
    for (uint8_t i = 0; i < TX_QUEUE_DEPTH; ++i) {
        EchoWatch& w = echoWatch[i];
        if (!w.used) continue;
        Message* msg = findBySeq(w.seq);
        if (!msg || strcmp(msg->body, text) != 0) continue;

        if (w.confirmed) {
            // SerialModule.cpp's own comment: "we get the packet back twice
            // when we send out of the radio", deduped there on lastRxID. If
            // that dedupe misses, swallow the repeat rather than showing our
            // own message a second time.
            return true;
        }
        // An exact byte-for-byte match of text we composed cannot survive a
        // baud mismatch, so it proves the link every bit as well as a
        // well-formed inbound line does.
        confirmWatch(w, *msg, TxState::CONFIRMED);
        return true;
    }
    return false;
}

// A completed line. Empty means the gap between two println()-bracketed
// messages, which is the only message boundary this codec has.
static void commitLine(uint32_t now) {
    if (!state) return;
    const uint16_t len = rxLineLen;
    rxLineLen = 0;
    state->rxLine[len] = '\0';

    if (len == 0) {
#if HAMLET_MESH_TRACE
        Serial.println("[MESHLINE] <blank — message boundary>");
#endif
        openSeq = 0;
        return;
    }

#if HAMLET_MESH_TRACE
    Serial.printf("[MESHLINE] %u chars: \"%s\"\n", (unsigned)len,
                  state->rxLine);
#endif

    // Bytes arrived and were readable enough to assemble, whatever they say.
    lastRxMs = now;

    if (tryEcho(state->rxLine)) return;

    char* rxLine = state->rxLine;
    const char* cut = strstr(rxLine, ": ");
    if (cut && cut != rxLine && (size_t)(cut - rxLine) < SENDER_LEN - 1) {
        char senderBuf[SENDER_LEN];
        const size_t nameLen = (size_t)(cut - rxLine);
        memcpy(senderBuf, rxLine, nameLen);
        senderBuf[nameLen] = '\0';
        append(senderBuf, cut + 2, now, false);
        everParsed = true;
        return;
    }

    if (openSeq != 0) {
        appendContinuation(rxLine);
        return;
    }

    // Unprefixed and nothing open: an echo we could not match, or a radio in
    // DEFAULT mode writing bare payload bytes.
    if (!everParsed) {
        // ...or, far more likely on a link that has never once parsed, it is
        // mis-sampled bytes. Filing those costs twice: sixty-four of them evict
        // a scrollback the reader will still want after they fix the setting,
        // and a ring that is never empty hides the screen that explains what to
        // fix. They still count toward isUnparsed(), which is the honest place
        // for them to show up.
        return;
    }
    append("?", rxLine, now, false);
}

// ==[ ROSTER ]== the radio streams its whole NodeDB after the handshake and
// then mentions nodes again every time one transmits. Both paths land here.
//
// Eviction is by staleness rather than by arrival order: on a mesh larger than
// the roster, a ring would let a chatty distant repeater push out the direct
// neighbour you are actually talking to.
static Node* touchNode(uint32_t num, uint32_t now) {
    if (!state || num == 0 || num == BROADCAST_ADDR) return nullptr;

    // Every caller is about to change something the ordering reads — at the
    // very least lastSeenMs. Invalidating here rather than at each of the
    // dozen assignment sites is what keeps the two from drifting apart.
    nodeOrderDirty = true;

    for (uint8_t i = 0; i < nodeCount; ++i) {
        if (state->nodes[i].num == num) return &state->nodes[i];
    }

    if (nodeCount < MeshUartPolicy::PROTO_MAX_NODES) {
        Node& n = state->nodes[nodeCount++];
        memset(&n, 0, sizeof(n));
        n.num = num;
        n.lastSeenMs = now;
        revision++;
        return &n;
    }

    // Full. Evict the least recently heard, but never one we have heard from
    // directly in favour of one the NodeDB merely listed.
    uint8_t  victim = 0;
    uint32_t oldest = 0;
    bool     foundIndirect = false;
    for (uint8_t i = 0; i < nodeCount; ++i) {
        const Node& n = state->nodes[i];
        const uint32_t age = (uint32_t)(now - n.lastSeenMs);
        if (!n.heardDirect && !foundIndirect) {
            foundIndirect = true;
            victim = i;
            oldest = age;
            continue;
        }
        if (n.heardDirect && foundIndirect) continue;
        if (age > oldest) { oldest = age; victim = i; }
    }
    Node& n = state->nodes[victim];
    memset(&n, 0, sizeof(n));
    n.num = num;
    n.lastSeenMs = now;
    revision++;
    return &n;
}

// ==[ ORDER ]== who you can actually reach, first. A node heard directly
// outranks one the NodeDB merely listed; then fewer hops; then more recently
// heard. Rebuilt lazily because it changes far less often than it is read —
// once per arrival, versus once per frame for twenty visible rows.
static bool ranksAbove(const Node& a, const Node& b) {
    if (a.heardDirect != b.heardDirect) return a.heardDirect;
    // An unknown hop count is not zero hops. Sorting it as zero would put
    // every unheard NodeDB entry above the neighbour in the same room.
    const uint8_t ah = a.hasHops ? a.hopsAway : 0xFF;
    const uint8_t bh = b.hasHops ? b.hopsAway : 0xFF;
    if (ah != bh) return ah < bh;
    // Unsigned subtraction, so this stays correct across the millis() wrap.
    return (uint32_t)(a.lastSeenMs - b.lastSeenMs) < 0x80000000u;
}

static void rebuildNodeOrder() {
    if (!state || !nodeOrderDirty) return;
    nodeOrderDirty = false;
    for (uint8_t i = 0; i < nodeCount; ++i) state->nodeOrder[i] = i;
    // Insertion sort: nodeCount is at most 32 and very nearly sorted already,
    // which is the case this beats everything fancier at.
    for (uint8_t i = 1; i < nodeCount; ++i) {
        const uint8_t key = state->nodeOrder[i];
        int8_t j = (int8_t)(i - 1);
        while (j >= 0 &&
               ranksAbove(state->nodes[key], state->nodes[state->nodeOrder[j]])) {
            state->nodeOrder[j + 1] = state->nodeOrder[j];
            j--;
        }
        state->nodeOrder[j + 1] = key;
    }
}

// ==[ THE NAMES OFF A User ]== written by both roster paths — the radio's
// whole-NodeDB dump and a live NODEINFO broadcast — and they have to agree.
// Filling one and not the other gives a roster whose names refresh on resync
// but not from live traffic, which reads as a stale radio rather than a bug.
static void applyUser(Node& n, const MeshProto::NodeInfo& ni) {
    strncpy(n.shortName, ni.shortName, sizeof(n.shortName) - 1);
    n.shortName[sizeof(n.shortName) - 1] = '\0';
    strncpy(n.longName, ni.longName, sizeof(n.longName) - 1);
    n.longName[sizeof(n.longName) - 1] = '\0';
}

// The radio reports >100 percent for a node running on USB rather than a
// battery it can measure, and the roster draws that as "USB" — so the ceiling
// is the byte, not 100.
static uint8_t batteryPercent(uint32_t raw) {
    return (uint8_t)(raw > 255 ? 255 : raw);
}

// The name a message is stamped with. A node we have a roster entry for gets
// its short name; one we have not met yet gets its number, which is at least
// stable and matches what the Meshtastic apps show.
static void nodeLabel(uint32_t num, char* out, size_t cap) {
    const Node* n = findNode(num);
    if (n && n->shortName[0]) {
        strncpy(out, n->shortName, cap - 1);
        out[cap - 1] = '\0';
        return;
    }
    // Last four hex digits, the same tail the apps and the long names use.
    snprintf(out, cap, "%04lx", (unsigned long)(num & 0xFFFFu));
}

// An inbound ROUTING packet is the real delivery receipt — the thing TEXTMSG's
// echo could only ever approximate. error_reason 0 means the far end took it.
static void applyRouting(const MeshProto::Packet& pkt) {
    if (pkt.requestId == 0) return;
    for (uint8_t i = 0; i < TX_QUEUE_DEPTH; ++i) {
        EchoWatch& w = echoWatch[i];
        if (!w.used) continue;
        Message* msg = findBySeq(w.seq);
        if (!msg || msg->packetId != pkt.requestId) continue;

        // A real end-to-end verdict, so the watch is finished either way —
        // unlike an echo, nothing further is going to arrive about this one.
        confirmWatch(w, *msg, (pkt.routingError == 0) ? TxState::CONFIRMED
                                                      : TxState::NO_ECHO);
        w.used = false;
        return;
    }
}

static void handleProtoPacket(const MeshProto::Packet& pkt, uint32_t now) {
    // An encrypted packet is one whose channel key we do not have. It is not
    // corruption and not our business; counting it as unparsed would condemn a
    // perfectly good link for carrying somebody else's channel.
    if (!pkt.hasDecoded) return;

    everParsed = true;

    Node* n = touchNode(pkt.from, now);
    if (n) {
        n->lastSeenMs = now;
        n->heardDirect = true;
        n->snr = pkt.rxSnr;
        if (pkt.hopStart >= pkt.hopLimit) {
            n->hopsAway = pkt.hopsCrossed();
            n->hasHops = true;
        }
        revision++;
    }

    if (pkt.portnum == MeshProto::PORT_ROUTING) {
        applyRouting(pkt);
        return;
    }

    // ==[ LIVE ROSTER ]== a node's own broadcasts, which arrive continuously
    // and keep the list fresh between the radio's whole-NodeDB dumps. Without
    // these, a name or a battery is only ever as current as the last resync.
    if (pkt.portnum == MeshProto::PORT_NODEINFO && n && pkt.payload) {
        // A bare User: it has no node number of its own, which is why this
        // writes into the entry `from` already selected rather than a fresh
        // one. decodeUserPayload touches only the name fields.
        MeshProto::NodeInfo ni;
        memset(&ni, 0, sizeof(ni));
        if (MeshProto::decodeUserPayload(pkt.payload, pkt.payloadLen, ni)) {
            applyUser(*n, ni);
            revision++;
        }
        return;
    }
    if (pkt.portnum == MeshProto::PORT_TELEMETRY && n && pkt.payload) {
        MeshProto::Telemetry t;
        // Returns false for an environment-only node, which must not be
        // recorded as a battery of zero.
        if (MeshProto::decodeTelemetryPayload(pkt.payload, pkt.payloadLen, t)) {
            n->battery = batteryPercent(t.battery);
            revision++;
        }
        return;
    }

    if (pkt.portnum != MeshProto::PORT_TEXT) return;   // position, admin…
    if (!pkt.payload || pkt.payloadLen == 0) return;

    // Our own broadcast comes back to us off the mesh. Filing it would show
    // every sent message twice, once composed and once "received".
    if (myNodeNum && pkt.from == myNodeNum) return;

    char body[BODY_LEN];
    size_t len = pkt.payloadLen < (BODY_LEN - 1) ? pkt.payloadLen : (BODY_LEN - 1);
    memcpy(body, pkt.payload, len);
    body[len] = '\0';

    char sender[SENDER_LEN];
    nodeLabel(pkt.from, sender, sizeof(sender));

    const uint32_t seq = append(sender, body, now, false);
    if (seq == 0) return;
    Message* msg = findBySeq(seq);
    if (!msg) return;

    msg->toNum = pkt.to;
    msg->packetId = pkt.id;
    msg->hops = pkt.hopsCrossed();
    msg->hasHops = (pkt.hopStart >= pkt.hopLimit && pkt.hopStart != 0);
    // Addressed to us specifically rather than to everyone. TEXTMSG delivered
    // these too and had no way to say so.
    msg->direct = (myNodeNum != 0 && pkt.to == myNodeNum);
    // A PROTO message is closed by its frame, so nothing can continue it.
    openSeq = 0;
}

static void handleProtoNodeInfo(const MeshProto::NodeInfo& ni, uint32_t now) {
    everParsed = true;
    Node* n = touchNode(ni.num, now);
    if (!n) return;

    if (ni.hasUser) applyUser(*n, ni);
    // The NodeDB's snr is the last one the radio recorded, which for a node we
    // have not personally heard is still the best number available.
    if (!n->heardDirect) n->snr = ni.snr;
    if (ni.battery) n->battery = batteryPercent(ni.battery);
    if (ni.hasHopsAway) {
        n->hopsAway = (uint8_t)ni.hopsAway;
        n->hasHops = true;
    }
    revision++;
}

// One complete StreamAPI frame. Runs inside drainRx, on the compositor's
// thread, so it does exactly as much work as filing the result.
static void onProtoFrame(const uint8_t* payload, uint16_t len, void* user) {
    const uint32_t now = *(const uint32_t*)user;

    MeshProto::FromRadio fr;
    if (!MeshProto::decodeFromRadio(payload, len, fr)) return;

    // ==[ UNDERSTOOD ]== a frame that decoded is proof this cable carries
    // something we can read, whatever it turned out to say. That includes the
    // variants we have no screen for and packets encrypted on a channel we do
    // not hold the key to — those still travelled the whole framing layer
    // intact, which is exactly what the link state is asking about.
    lastRxMs = now;
    everParsed = true;

    switch (fr.kind) {
        case MeshProto::FromRadioKind::PACKET:
            handleProtoPacket(fr.packet, now);
            break;
        case MeshProto::FromRadioKind::NODE_INFO:
            handleProtoNodeInfo(fr.node, now);
            if (protoState == ProtoState::OPENING) protoState = ProtoState::SYNCING;
            break;
        case MeshProto::FromRadioKind::MY_INFO:
            myNodeNum = fr.myNodeNum;
            everParsed = true;
            if (protoState == ProtoState::OPENING) protoState = ProtoState::SYNCING;
            revision++;
            break;
        case MeshProto::FromRadioKind::CONFIG_COMPLETE:
            // The NodeDB stream has ended, so the roster is now as complete as
            // the radio can make it. Anything arriving after this is live.
            if (fr.configCompleteId == handshakeId || handshakeId == 0) {
                protoState = ProtoState::READY;
                everParsed = true;
                revision++;
            }
            break;
        case MeshProto::FromRadioKind::REBOOTED:
            // The radio restarted underneath us. Its NodeDB stream is gone and
            // our session id means nothing to it now — open a new one.
            protoState = ProtoState::OPENING;
            handshakeSentMs = 0;
            revision++;
            break;
        default:
            break;
    }
}

// ==[ ONE DRIVER CALL PER CHUNK ]== both drains used to ask available() and
// read() once per byte, and each of those takes and releases the UART driver's
// mutex. A NodeDB dump fills the 512-byte buffer every frame, which made it
// ~2000 driver round trips for 512 bytes of payload. read(buf, n) is the same
// non-blocking read in one call — readBytes() is the one that would wait out
// the Stream timeout. Same pattern, and the same reason, as GPS::service().
static size_t readChunk(uint8_t* chunk, size_t cap, uint16_t& budget) {
    if (budget == 0) return 0;
    const int avail = meshSerial.available();
    if (avail <= 0) return 0;
    size_t n = (size_t)avail;
    if (n > cap) n = cap;
    if (n > budget) n = budget;
    const size_t got = meshSerial.read(chunk, n);
    rxBytes += got;
    budget = (uint16_t)(budget - got);
    return got;
}

static void drainRxProto(uint32_t now) {
    uint16_t budget = MAX_DRAIN_PER_CALL;
    uint8_t  chunk[64];
    for (;;) {
        const size_t n = readChunk(chunk, sizeof(chunk), budget);
        if (n == 0) break;
        // lastByteMs is about the wire and belongs here. lastRxMs is about
        // being *understood* and does not: TEXTMSG only sets it once bytes
        // assemble into a line, and setting it per byte here would report a
        // radio spewing noise at the wrong baud as L1V3 while isUnparsed() was
        // simultaneously shouting N0 P4RS3 at the same reader. onProtoFrame
        // marks it, where a frame has actually decoded.
        lastByteMs = now;
#if HAMLET_MESH_TRACE
        for (size_t i = 0; i < n; ++i) traceByte(chunk[i]);
#endif
        state->proto.feed(chunk, n, onProtoFrame, (void*)&now);
    }
}

static void drainRx(uint32_t now) {
    uint16_t budget = MAX_DRAIN_PER_CALL;
    uint8_t  chunk[64];
    for (;;) {
        const size_t got = readChunk(chunk, sizeof(chunk), budget);
        if (got == 0) break;

        // Every byte, terminators and stripped controls included: this is
        // "when did the wire last carry anything", and both the line flush and
        // the continuation-run expiry below are questions about silence.
        lastByteMs = now;

        for (size_t i = 0; i < got; ++i) {
            const uint8_t c = chunk[i];
#if HAMLET_MESH_TRACE
            traceByte(c);
#endif
            if (c == '\r') {
                commitLine(now);
                lastWasCR = true;
                continue;
            }
            if (c == '\n') {
                // CRLF is ONE terminator. Treating the two halves separately
                // manufactures an empty line after every single message, which
                // destroys the only boundary signal the codec has.
                if (lastWasCR) {
                    lastWasCR = false;
                    continue;
                }
                commitLine(now);
                continue;
            }

            lastWasCR = false;
            if (c < 0x20 || c == 0x7F) {
                // C0 control bytes are not chat. They are what a baud mismatch
                // looks like, and they render as garbage in Font0.
                continue;
            }
            if (rxLineLen >= LINE_CAP - 1) {
                // Longer than anything the radio can carry. Close what we have
                // and keep the byte that overflowed rather than dropping it.
                commitLine(now);
            }
            state->rxLine[rxLineLen++] = (char)c;
        }
    }
}

static void expireEchoWatch(uint32_t now) {
    for (uint8_t i = 0; i < TX_QUEUE_DEPTH; ++i) {
        EchoWatch& w = echoWatch[i];
        if (!w.used) continue;
        if ((uint32_t)(now - w.atMs) < ECHO_WINDOW_MS) continue;
        w.used = false;

        // The verdict belongs here, not in the drawing code. This is the only
        // place that knows both when the window actually closed and whether
        // echoes were ever coming — a screen guessing at it from a compose
        // timestamp would mark a message unanswered while it was still sitting
        // in the send queue waiting out the pacing gap.
        //
        // Without a confirmed echo somewhere in this session there is nothing
        // to conclude: serial.echo is off on most radios, and silence from a
        // feature that was never enabled is not a failure.
        if (w.confirmed || !echoSeen) continue;
        Message* msg = findBySeq(w.seq);
        if (msg && msg->txState == TxState::SENT) {
            msg->txState = TxState::NO_ECHO;
            bumpRing();
        }
    }
}

static void watchForEcho(uint32_t seq, uint32_t now) {
    int8_t pick = -1;
    uint32_t oldest = 0xFFFFFFFFu;
    for (uint8_t i = 0; i < TX_QUEUE_DEPTH; ++i) {
        if (!echoWatch[i].used) { pick = (int8_t)i; break; }
        if (echoWatch[i].seq < oldest) { oldest = echoWatch[i].seq; pick = (int8_t)i; }
    }
    if (pick < 0) return;
    echoWatch[pick].seq = seq;
    echoWatch[pick].atMs = now;
    echoWatch[pick].confirmed = false;
    echoWatch[pick].used = true;
}

// One message per gap, oldest first. seq is monotonic, so it is also the
// queue order and no separate cursor is needed.
static void pumpTx(uint32_t now) {
    if (txArmed && (int32_t)(now - txReadyAtMs) < 0) return;

    int8_t pick = -1;
    uint32_t oldest = 0xFFFFFFFFu;
    for (uint8_t i = 0; i < TX_QUEUE_DEPTH; ++i) {
        if (!txQueue[i].used) continue;
        if (txQueue[i].seq < oldest) {
            oldest = txQueue[i].seq;
            pick = (int8_t)i;
        }
    }
    if (pick < 0) return;

    txQueue[pick].used = false;
    Message* msg = findBySeq(txQueue[pick].seq);
    if (!msg) return;   // the ring wrapped over it before we got to the wire

    uint16_t len = 0;
    if (activeCodec == Codec::PROTO) {
        // A framed packet carries its own length and its own destination, so
        // this is where a DM stops being a broadcast. want_ack is asked for
        // only on a DM — see encodeTextPacket.
        uint8_t frame[512];
        const size_t n = MeshProto::encodeTextPacket(
            frame, sizeof(frame), msg->toNum, msg->packetId, 0, msg->body,
            msg->toNum != BROADCAST_ADDR);
        if (n == 0) {
            // Refused rather than truncated. A half-written frame is still a
            // valid frame to the radio and would transmit a corrupted message.
            msg->txState = TxState::NO_ECHO;
            bumpRing();
            HAMLET_LOGLN("[MESH] proto encode refused");
            return;
        }
        meshSerial.write(frame, n);
        len = (uint16_t)n;
    } else {
        len = (uint16_t)strlen(msg->body);
        // No terminator: the radio's readBytes does not look for one, so a
        // '\n' here is simply payload — and at exactly MAX_PAYLOAD it is
        // payload that spills into a second packet carrying nothing but a
        // newline.
        meshSerial.write((const uint8_t*)msg->body, len);
    }

    msg->txState = TxState::SENT;
    bumpRing();
    txMessages++;

    // ==[ WHAT CAN ACTUALLY COME BACK ]== under TEXTMSG the radio echoes
    // everything, so everything is worth watching. Under PROTO only a DM
    // carries want_ack — a broadcast has no single node that could answer it,
    // and asking would flood the mesh. Watching one anyway would mark every
    // broadcast NO_ECHO eight seconds after the first DM ever got acked, which
    // is a failure mark on the most normal thing this screen does.
    if (activeCodec != Codec::PROTO || msg->toNum != BROADCAST_ADDR) {
        watchForEcho(msg->seq, now);
    }

    // The radio's per-byte timer starts on our LAST byte, so the gap runs from
    // when the write finishes leaving, not from when it was handed over. Under
    // PROTO the gap is zero — framed packets cannot be merged — and only the
    // wire time itself remains.
    const uint32_t gap = MeshUartPolicy::txGapMs(activeCodec);
    txReadyAtMs = now + wireMs(len) + gap;
    txArmed = true;

    HAMLET_LOGF("[MESH] tx %u bytes, next in %lums\n", (unsigned)len,
                (unsigned long)(wireMs(len) + gap));
}

// ==[ PUBLIC API ]==

bool begin(uint8_t rxPin, uint8_t txPin, uint32_t baud, Codec codec) {
    if (!MeshUartPolicy::isValidConfig(rxPin, txPin, baud)) {
        HAMLET_LOGF("[MESH] refusing rx=%u tx=%u baud=%lu\n",
                    (unsigned)rxPin, (unsigned)txPin, (unsigned long)baud);
        return false;
    }
    if (!ensureState()) return false;

    if (started) meshSerial.end();

    activeRxPin = rxPin;
    activeTxPin = txPin;
    activeBaud = baud;
    quietFlushMs = MeshUartPolicy::quietFlushMs(baud);

    rxLineLen = 0;
    lastByteMs = 0;
    lastWasCR = false;
    openSeq = 0;

    // ==[ WHAT A RESTART FORGETS ]== everything below describes THIS pin and
    // baud pairing, not the session. Carrying `everParsed` across a baud change
    // would let a link that used to work vouch for the one that replaced it,
    // which is precisely the case the setting exists to let someone test. The
    // scrollback is not a link fact and survives.
    everParsed = false;
    echoSeen = false;
    rxBytes = 0;
    lastRxMs = 0;
    txArmed = false;

    // ==[ CODEC ]== everything below describes the session, not the cable, and
    // a codec change invalidates all of it. The roster in particular: node
    // names only exist under PROTO, so switching to TEXTMSG must not leave a
    // stale list on screen implying the link can still resolve them.
    activeCodec = codec;
    state->proto.reset();
    nodeCount = 0;
    nodeOrderDirty = true;
    myNodeNum = 0;
    handshakeSentMs = 0;
    handshakeId = 0;
    protoState = (codec == Codec::PROTO) ? ProtoState::OPENING : ProtoState::NA;
    // Seeded off the clock so a reboot mid-conversation cannot reuse ids the
    // far end is still sending routing acks for.
    nextPacketId = (millis() << 8) | 1u;

    meshSerial.setRxBufferSize(RX_BUFFER_BYTES);
    meshSerial.setTxBufferSize(TX_BUFFER_BYTES);
    meshSerial.begin(baud, SERIAL_8N1, rxPin, txPin);
    started = true;

    HAMLET_LOGF("[MESH] UART%u rx=%u tx=%u baud=%lu codec=%s flush=%lums "
                "gap=%lums\n",
                (unsigned)MeshUartPolicy::UART_NUM, (unsigned)rxPin,
                (unsigned)txPin, (unsigned long)baud,
                MeshUartPolicy::codecLabel(codec),
                (unsigned long)quietFlushMs,
                (unsigned long)MeshUartPolicy::txGapMs(codec));
    return true;
}

Codec getCodec() { return activeCodec; }
ProtoState getProtoState() { return protoState; }
uint32_t getMyNodeNum() { return myNodeNum; }
uint8_t getNodeCount() { return nodeCount; }

const Node& getNode(uint8_t idx) {
    static const Node empty = {};
    if (!state || idx >= nodeCount) return empty;
    rebuildNodeOrder();
    return state->nodes[state->nodeOrder[idx]];
}

const Node* findNode(uint32_t num) {
    if (!state || num == 0) return nullptr;
    for (uint8_t i = 0; i < nodeCount; ++i) {
        if (state->nodes[i].num == num) return &state->nodes[i];
    }
    return nullptr;
}

void stop() {
    if (!started) return;
    meshSerial.end();
    started = false;
    rxLineLen = 0;
    lastWasCR = false;
    openSeq = 0;
    // Anything still queued has no wire to leave on, and the messages it names
    // are already in the ring reading QUEUED. Leave them saying so.
    for (uint8_t i = 0; i < TX_QUEUE_DEPTH; ++i) txQueue[i].used = false;
    // The session is over, so the roster is a claim we can no longer stand
    // behind. The scrollback survives — a message that arrived was still true.
    protoState = ProtoState::NA;
    nodeCount = 0;
    nodeOrderDirty = true;
    myNodeNum = 0;
    if (state) state->proto.reset();
    HAMLET_LOGLN("[MESH] bridge stopped");
}

bool isStarted() { return started; }

// ==[ HANDSHAKE ]== the radio ignores want_config_id until its own serial
// module is up, and says nothing when it does. Retrying is the only way
// through. Stops the moment the NodeDB stream starts arriving, so a live
// session never carries the cost.
static void pumpHandshake(uint32_t now) {
    if (activeCodec != Codec::PROTO) return;
    if (protoState != ProtoState::OPENING) return;
    if (handshakeSentMs != 0 &&
        (uint32_t)(now - handshakeSentMs) < MeshUartPolicy::PROTO_HANDSHAKE_RETRY_MS) {
        return;
    }

    uint8_t frame[32];
    // A fresh id each attempt, so config_complete_id identifies which attempt
    // the radio is answering rather than an earlier one we had given up on.
    handshakeId = (now ^ 0x5EED0000u) | 1u;
    const size_t n = MeshProto::encodeWantConfig(frame, sizeof(frame), handshakeId);
    if (n == 0) return;
    meshSerial.write(frame, n);
    handshakeSentMs = now;
    HAMLET_LOGF("[MESH] want_config_id=%08lx\n", (unsigned long)handshakeId);
}

void service(uint32_t now) {
    if (!started || !state) return;

    if (activeCodec == Codec::PROTO) {
        drainRxProto(now);
        pumpHandshake(now);
        expireEchoWatch(now);
        pumpTx(now);
#if HAMLET_MESH_TRACE
        traceHeartbeat(now);
#endif
        return;
    }

    drainRx(now);

    // The radio brackets messages with println(), so structure normally closes
    // them. The window is derived from the baud rate — see mesh_uart_policy.h.
    if ((uint32_t)(now - lastByteMs) >= quietFlushMs) {
        // Fallback for a line that was never terminated at all, which is
        // exactly what the echo path emits.
        if (rxLineLen > 0) commitLine(now);

        // Silence also ends the continuation run, and this has to be
        // unconditional rather than folded into the branch above. A message
        // closed by its own trailing CRLF leaves the line buffer EMPTY while
        // the run stays open for a continuation that may still be coming —
        // so if only a pending line could end the run, the run would outlive
        // its message indefinitely, and the next unprefixed line to arrive
        // (an unmatched echo, hours later) would be glued onto it.
        openSeq = 0;
#if HAMLET_MESH_TRACE
        // A partial dump line is more useful now than sixteen bytes from now:
        // the wire has gone quiet, so nothing is coming to complete it.
        traceFlush();
#endif
    }

#if HAMLET_MESH_TRACE
    traceHeartbeat(now);
#endif

    expireEchoWatch(now);
    pumpTx(now);
}

LinkState getLinkState(uint32_t now) {
    if (!started) return LinkState::OFF;
    if (lastRxMs == 0) return LinkState::WAITING;
    return ((uint32_t)(now - lastRxMs) <= LIVE_WINDOW_MS) ? LinkState::LIVE
                                                          : LinkState::WAITING;
}

uint32_t getLastRxMs() { return lastRxMs; }
uint8_t  getRxPin() { return activeRxPin; }
uint8_t  getTxPin() { return activeTxPin; }
uint32_t getBaud() { return activeBaud; }

bool send(const char* text) { return sendTo(text, BROADCAST_ADDR); }

bool sendTo(const char* text, uint32_t to) {
    if (!started || !state || !text || !text[0]) return false;

    // TEXTMSG has no destination field at all — the radio broadcasts whatever
    // bytes it reads. Quietly broadcasting something the user addressed to one
    // node is the single worst thing this module could do, so refuse and let
    // the UI say why.
    if (to != BROADCAST_ADDR && activeCodec != Codec::PROTO) {
        HAMLET_LOGLN("[MESH] DM refused: TEXTMSG cannot address a node");
        return false;
    }

    int8_t slot = -1;
    for (uint8_t i = 0; i < TX_QUEUE_DEPTH; ++i) {
        if (!txQueue[i].used) { slot = (int8_t)i; break; }
    }
    if (slot < 0) {
        HAMLET_LOGLN("[MESH] tx queue full");
        return false;
    }

    char clipped[BODY_LEN];
    size_t len = strlen(text);
    if (len > MAX_PAYLOAD) len = MAX_PAYLOAD;
    memcpy(clipped, text, len);
    clipped[len] = '\0';

    // Into the ring first, so the screen shows it the moment it is composed
    // rather than a second and a quarter later when the wire is free.
    const uint32_t seq = append("ME", clipped, millis(), true);
    if (seq == 0) return false;

    Message* msg = findBySeq(seq);
    if (msg) {
        msg->toNum = to;
        msg->direct = (to != BROADCAST_ADDR);
        // The id is assigned at compose time rather than at write time so the
        // ack correlation has something to match the moment the radio answers
        // — a routing packet can arrive before we have finished the frame.
        msg->packetId = (activeCodec == Codec::PROTO) ? nextPacketId++ : 0;
    }

    txQueue[slot].seq = seq;
    txQueue[slot].used = true;

    // Our own send ends any continuation run: whatever the radio was part-way
    // through printing, the next thing on the wire is not the rest of it.
    openSeq = 0;
    return true;
}

uint8_t getTxPending() {
    uint8_t n = 0;
    for (uint8_t i = 0; i < TX_QUEUE_DEPTH; ++i) {
        if (txQueue[i].used) n++;
    }
    return n;
}

bool canSend() {
    for (uint8_t i = 0; i < TX_QUEUE_DEPTH; ++i) {
        if (!txQueue[i].used) return true;
    }
    return false;
}

uint8_t getMessageCount() { return ringCount; }

const Message& getMessage(uint8_t idxFromOldest) {
    static Message empty = {};
    if (!state || idxFromOldest >= ringCount) return empty;
    // Oldest retained entry sits one past the head once the ring has wrapped.
    const uint8_t base =
        (uint8_t)((ringHead + MAX_MESSAGES - ringCount) % MAX_MESSAGES);
    return state->ring[(uint8_t)((base + idxFromOldest) % MAX_MESSAGES)];
}

uint32_t getRevision() { return revision; }
uint32_t getRingRevision() { return ringRevision; }

void clear() {
    ringHead = 0;
    ringCount = 0;
    bumpRing();
    unread = 0;
    unannounced = 0;
    rxLineLen = 0;
    lastWasCR = false;
    openSeq = 0;
    // Both tables name messages that no longer exist.
    for (uint8_t i = 0; i < TX_QUEUE_DEPTH; ++i) {
        txQueue[i].used = false;
        echoWatch[i].used = false;
    }
}

bool     isUnparsed() { return !everParsed && rxBytes >= NOISE_MIN_SAMPLE; }
uint32_t getRxBytes() { return rxBytes; }
uint16_t getRxMessages() { return rxMessages; }
uint16_t getTxMessages() { return txMessages; }

uint8_t consumeUnread() {
    const uint8_t n = unread;
    unread = 0;
    unannounced = 0;   // reading it beats being told about it
    return n;
}

uint8_t peekUnread() { return unread; }

uint8_t consumeUnannounced() {
    const uint8_t n = unannounced;
    unannounced = 0;
    return n;
}

}  // namespace Mesh
