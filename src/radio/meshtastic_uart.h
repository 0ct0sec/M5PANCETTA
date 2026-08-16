/**
 * Meshtastic UART Bridge — LoRa text messaging via an M5Stack Unit C6L
 *
 * ==[ THE LONG RANGE ]== the C6L runs stock Meshtastic and does every hard
 * thing itself: SX1262 modulation, mesh routing, retries, channel crypto. This
 * bridge only has to speak the plainest dialect that firmware offers.
 *
 * ==[ TEXTMSG, NOT PROTO ]== Meshtastic's SerialModule has two useful modes.
 * PROTO exposes the full client API — nodes, DMs, acks, per-packet SNR — behind
 * 0x94 0xC3 framed protobufs. TEXTMSG is a wire anyone can read. This file is
 * the TEXTMSG half. Everything the mode above it does — the ring, the link
 * state, the compose path — is codec independent, so PROTO is a swap of
 * commitLine()/send() and not a rewrite.
 *
 * ==[ WHAT THE FIRMWARE ACTUALLY DOES ]== this used to be guesswork and is now
 * read out of src/modules/SerialModule.cpp. Four things there shape everything
 * below, and three of them were being got wrong:
 *
 *   RX is bracketed, not merely terminated. handleReceived() does
 *     println(); printf("%s: %s", sender, payload); println();
 *   so the wire reads "\r\n" "SHRT: body" "\r\n" and two messages in a row have
 *   an EMPTY LINE between them. That blank line is a real boundary, which means
 *   a non-blank unprefixed line is a CONTINUATION — the payload had a newline
 *   in it — rather than a new message from an unknown sender.
 *
 *   The sender is user.short_name, `char short_name[5]` — four characters, not
 *   the twenty-four assumed here. When the node is not in the NodeDB yet the
 *   firmware prints the literal "???".
 *
 *   TX has no framing at all. runOnce() reads us with
 *     readBytes(serialBytes, meshtastic_Constants_DATA_PAYLOAD_LEN)
 *   whose limit is 233 and whose timeout is PER BYTE and one second. Newlines
 *   mean nothing to it. So a trailing '\n' is payload, byte 234 becomes the
 *   next packet, and two messages sent less than a second apart are delivered
 *   to the mesh as one. See mesh_uart_policy.h and sim/mesh_tx_pacing_sim.py.
 *
 *   The echo is a receipt. With serial.echo set, handleReceived() prints our
 *   own payload back once the router has accepted it. Matching that against
 *   what we sent is the only positive confirmation TEXTMSG can give that our
 *   text became a mesh packet.
 *
 * What TEXTMSG still cannot do, and what the UI must therefore not promise:
 * sending direct messages, channel selection, delivery acks from the far end,
 * node names beyond the short one stamped on each inbound line, or signal
 * quality. Inbound DMs do arrive, but indistinguishably from broadcasts.
 *
 * ==[ C6L SIDE ]== configured once over USB, because none of it is reachable
 * from here: serial.enabled=true, serial.mode=TEXTMSG, serial.rxd=4,
 * serial.txd=5, serial.baud to match, serial.echo=true for the delivery marks,
 * and position.gps_mode=NOT_PRESENT so the GPS driver does not sit on the very
 * pins we are talking to.
 */
#pragma once

#include <stdint.h>

#include "mesh_proto.h"
#include "mesh_uart_policy.h"

// ==[ WIRE TRACE ]== -DHAMLET_MESH_TRACE=1 builds a sniffer: every byte off the
// cable hexdumped to the USB console, every assembled line printed, a heartbeat
// saying what parsed, and the bridge forced up at boot even when the setting is
// off. Off by default — the dump is three console bytes per wire byte and has
// no business in a build anyone flies. Lives here rather than in the .cpp
// because hamlet.cpp's boot path is the other half of it.
#ifndef HAMLET_MESH_TRACE
#define HAMLET_MESH_TRACE 0
#endif

namespace Mesh {

// ==[ LINK ]== there is no handshake to lean on. TEXTMSG is a one-way pipe in
// each direction, so "connected" can only ever mean "said something recently".
enum class LinkState : uint8_t {
    OFF,      // bridge not started (disabled, or begin() failed)
    WAITING,  // started, nothing heard yet — normal for a quiet mesh
    LIVE      // bytes arrived inside LIVE_WINDOW_MS
};

// ==[ OUTGOING ]== how far along the only path we can actually observe.
// SENT means the bytes left this device. CONFIRMED means the radio handed the
// packet back to us from inside its own router, which is as close to a receipt
// as this codec gets — and only if serial.echo is set on the C6L. It is never
// evidence that anyone received the message.
enum class TxState : uint8_t {
    NONE,       // inbound message
    QUEUED,     // composed, waiting out the pacing gap
    SENT,       // written to the cable
    CONFIRMED,  // echoed back by the radio
    NO_ECHO     // echoes demonstrably work here, and this one never came back
};

static constexpr uint8_t  SENDER_LEN = MeshUartPolicy::SENDER_LEN;
static constexpr uint16_t BODY_LEN = MeshUartPolicy::BODY_LEN;
static constexpr uint16_t MAX_PAYLOAD = MeshUartPolicy::MAX_PAYLOAD;

// A quiet mesh is the normal state, so this window is generous: it answers
// "has this radio ever been alive on this cable", not "is a peer in range".
static constexpr uint32_t LIVE_WINDOW_MS = 120000;

// Bounded scrollback. Sized for the screen's appetite, not the mesh's.
static constexpr uint8_t MAX_MESSAGES = 64;

struct Message {
    char     sender[SENDER_LEN];  // "?" unprefixed, "???" = node not in NodeDB
    char     body[BODY_LEN];      // may contain '\n' — the payload can
    uint32_t atMs;
    // Monotonic across the whole session, never reused. The ring index of a
    // message shifts every time the oldest one falls off the front, so a reader
    // parked in the history needs something that does not move under it. The
    // TX queue and the echo watch address messages by it for the same reason.
    uint32_t seq;
    bool     outgoing;            // true = composed here, echoed into the ring
    TxState  txState;

    // ==[ PROTO ONLY ]== TEXTMSG has no field for any of this.
    uint32_t toNum;     // BROADCAST, or us for an inbound DM
    uint32_t packetId;  // what an ack's request_id points at
    // hop_start - hop_limit; 0 = direct neighbour. `hasHops` is the honest
    // gate: under TEXTMSG it is false, and a screen that drew 0 hops would be
    // inventing a measurement rather than reporting one.
    uint8_t  hops;
    bool     hasHops;
    bool     direct;    // addressed to this node rather than broadcast
};

// ==[ ROSTER ]== who is out there, which TEXTMSG could never say. Fed by the
// NodeDB the radio streams after the handshake and refreshed by every packet
// that arrives, so a node's signal is as fresh as its last transmission.
struct Node {
    uint32_t num;
    char     shortName[SENDER_LEN];
    char     longName[40];
    float    snr;
    uint8_t  battery;      // percent; >100 is the radio saying "on USB"
    uint8_t  hopsAway;
    bool     hasHops;
    uint32_t lastSeenMs;   // millis(), local — the only clock that is ours
    bool     heardDirect;  // it has sent us a packet, not merely been listed
};

using MeshUartPolicy::Codec;

// ==[ PROTO SESSION ]== there is a handshake now, and the screen has to be
// able to say which half of it we are in — "no traffic yet" and "the radio has
// never answered our want_config_id" are different problems with different
// fixes, and under TEXTMSG they were the same blank screen.
enum class ProtoState : uint8_t {
    NA,         // TEXTMSG: no session to be in
    OPENING,    // want_config_id sent, waiting for the NodeDB stream
    SYNCING,    // node info arriving
    READY       // config_complete_id seen; the roster is whole
};

// ==[ LIFECYCLE ]==
bool begin(uint8_t rxPin, uint8_t txPin, uint32_t baud, Codec codec);
void stop();
bool isStarted();
Codec getCodec();

// Non-blocking RX drain, quiet-gap flush, and paced TX drain. Once per frame.
void service(uint32_t now);

// ==[ LINK STATE ]==
LinkState getLinkState(uint32_t now);
uint32_t  getLastRxMs();
uint8_t   getRxPin();
uint8_t   getTxPin();
uint32_t  getBaud();

// ==[ SEND ]== enqueues rather than writes: the radio merges anything arriving
// inside its one-second read window into a single mesh packet, so messages
// have to be spaced on the wire whatever the fingers do. Returns false when the
// bridge is down, the text is empty, or the queue is full — the last of which
// the UI must report, because silently dropping a composed message is worse
// than refusing it. Text past MAX_PAYLOAD is truncated rather than refused.
bool    send(const char* text);
// ==[ DIRECT MESSAGES ]== `to` is a node number, or BROADCAST_ADDR. Under
// TEXTMSG a non-broadcast target is refused rather than silently broadcast:
// sending something meant for one node to the whole mesh is the one failure
// mode worth being loud about.
bool    sendTo(const char* text, uint32_t to);
// The wire's own broadcast address, re-exported rather than re-declared: this
// is a Meshtastic constant, and two independent spellings of it could drift
// into a "DM" addressed to everybody.
static constexpr uint32_t BROADCAST_ADDR = MeshProto::BROADCAST;
uint8_t getTxPending();       // composed, not yet on the wire
// Whether a compose would be accepted. The queue depth is the transport's
// business — a screen that compared getTxPending() against it itself would be
// keeping a second copy of the same rule.
bool    canSend();

// ==[ SCROLLBACK ]==
uint8_t        getMessageCount();
const Message& getMessage(uint8_t idxFromOldest);  // 0 = oldest retained
uint32_t       getRevision();   // bumps on every change; cheap redraw trigger
// Bumps only when the scrollback itself moves. The roster is far busier — every
// decoded packet refreshes a node — so anything rebuilding a structure over the
// messages should watch this instead and not pay for passing beacons.
uint32_t       getRingRevision();
void           clear();

// ==[ ROSTER ]== PROTO only; empty under TEXTMSG, which is itself the honest
// answer to "who is out there" on a codec that cannot say.
uint8_t     getNodeCount();
const Node& getNode(uint8_t idx);          // sorted: closest and freshest first
const Node* findNode(uint32_t num);        // nullptr when unknown
uint32_t    getMyNodeNum();                // 0 until my_info arrives
ProtoState  getProtoState();

// ==[ DIAGNOSTICS ]==
// Bytes are arriving and none of them parse: no "SHRT: body" line has ever
// been seen and no echo has ever matched. Almost always the C6L's serial.baud
// disagreeing with ours, sometimes its serial.mode not being TEXTMSG. Both look
// exactly like a quiet mesh without this. See sim/mesh_baud_noise_sim.py.
bool     isUnparsed();
uint32_t getRxBytes();
uint16_t getRxMessages();
uint16_t getTxMessages();

// ==[ UNREAD, IN TWO HALVES ]== a toast and a badge want opposite lifetimes,
// and one counter cannot serve both: the toast fires once and must not repeat,
// the badge must stand until someone actually reads the scrollback. Sharing a
// counter meant whichever fired first wiped the other.
//
//   unannounced — drained by the toast + chirp, once per arrival
//   unread      — drained only by the MESH screen, and it drains both, because
//                 reading a message is also the strongest form of announcing it
uint8_t consumeUnread();
uint8_t peekUnread();
uint8_t consumeUnannounced();

}  // namespace Mesh
