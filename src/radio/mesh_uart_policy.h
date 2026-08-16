/**
 * Mesh UART policy shared by firmware and native tests.
 *
 * Hardware profile: M5Stack Unit C6L (ESP32-C6 + SX1262) running Meshtastic,
 * hanging off a Grove port on the M5GO Battery Bottom2.
 *
 * ==[ WHICH GROVE PORT ]== the unit's own HY2.0-4P breaks out C6 GPIO4 (white)
 * and GPIO5 (yellow) — the same two pins Meshtastic's variant declares as
 * GPS_RX_PIN/GPS_TX_PIN, so the port is already a UART in that firmware. On
 * the host side the Bottom2's ports land on M-Bus positions whose GPIOs differ
 * per core, exactly like the GPS module's DIP table in gps_policy.h.
 *
 * Port C (blue) is the default because it is the one route this project has
 * already proven on an S3 host: gps_policy.h drives a stacked M003-V21 through
 * M-Bus 15/16 -> G18/G17. Port B (black) is offered as the alternate, and it is
 * the interesting one — Port C is the *same electrical net* as the GPS module's
 * default DIP route, so a C6L and a GPS module cannot both sit there. Moving
 * the C6L to Port B is what buys you mesh and GPS at the same time, and it is
 * unverified on a Core2-era Bottom2 over an S3 host: the Bottom2 was built for
 * a core whose M-Bus carried data at those positions, and CoreS3 does not agree
 * everywhere (gps_policy.h found M-Bus pin 26 is HVIN — a power pin — there).
 * 5V and GND are common to every port, which is why an unpowered-looking data
 * link still lights the unit up.
 */
#pragma once

#include <stdint.h>

#include "../hal/platform.h"

namespace MeshUartPolicy {

// ==[ CODEC ]== the same cable carries either dialect and the C6L decides
// which by its serial.mode. They are not interchangeable and the difference is
// not cosmetic:
//
//   TEXTMSG — write bytes, they broadcast. Inbound arrives as "SHRT: body".
//     No names beyond four characters, no signal quality, no delivery acks, no
//     way to address one node. Its one virtue is that it needs nothing from
//     the host but a UART.
//
//   PROTO — the full client API behind 0x94 0xC3 framing. Node names, SNR,
//     RSSI, hop counts, battery, real end-to-end acks, and direct messages.
//     Costs a decoder and a want_config_id handshake.
//
// Both are kept because the setting lives on the radio, not here: a user with
// a C6L already in TEXTMSG should not have to reconfigure it to see a message.
enum class Codec : uint8_t { TEXTMSG = 0, PROTO = 1 };
static constexpr uint8_t CODEC_COUNT = 2;

inline const char* codecLabel(Codec c) {
    return (c == Codec::PROTO) ? "PR0T0" : "T3XT";
}

inline Codec nextCodec(Codec c) {
    return (c == Codec::TEXTMSG) ? Codec::PROTO : Codec::TEXTMSG;
}

inline bool isSupportedCodec(uint8_t raw) { return raw < CODEC_COUNT; }

// Meshtastic's SerialModule defaults to 38400 (BAUD_38400), so that is our
// default too: it is one fewer field to change on the C6L before first light.
static constexpr uint32_t DEFAULT_BAUD = 38400;

// The C6L never shares a UART peripheral with the other bridges. On CoreS3 SE
// the console runs over USB-CDC (ARDUINO_USB_CDC_ON_BOOT=1 in the board def),
// which leaves UART0 genuinely unclaimed; UART1 is the C5 bridge and UART2 is
// GPS. On Core2 the console *is* UART0, but nothing there uses UART1 — C5 and
// GPS both contend for UART2 instead.
#if HAMLET_TARGET_CORES3SE
static constexpr uint8_t UART_NUM = 0;
static constexpr uint8_t RX_PIN = 18;  // Port.C RX (M-Bus 15)
static constexpr uint8_t TX_PIN = 17;  // Port.C TX (M-Bus 16)
static constexpr uint8_t RX_PINS[] = {18, 8};
static constexpr uint8_t TX_PINS[] = {17, 9};
#else
static constexpr uint8_t UART_NUM = 1;
static constexpr uint8_t RX_PIN = 13;  // Port.C RX
static constexpr uint8_t TX_PIN = 14;  // Port.C TX
// G36 is input-only on the original ESP32, so the Port B pair is not
// swappable: 36 can only ever be the RX leg.
static constexpr uint8_t RX_PINS[] = {13, 36};
static constexpr uint8_t TX_PINS[] = {14, 26};
#endif

// Which connector on the Bottom2 each index is, in the order a reader would
// find them printed on the plastic. This lives beside the pin tables rather
// than in the screens that show it: two UI files were each deriving it as
// "index 0 or not", which a third port would silently render as Port B on both.
static constexpr const char* PORT_LABELS[] = {"P0RT C", "P0RT B"};

static constexpr uint8_t RX_PIN_COUNT = sizeof(RX_PINS) / sizeof(RX_PINS[0]);
static constexpr uint8_t TX_PIN_COUNT = sizeof(TX_PINS) / sizeof(TX_PINS[0]);
static constexpr uint8_t PORT_LABEL_COUNT =
    sizeof(PORT_LABELS) / sizeof(PORT_LABELS[0]);

// The three tables are parallel: index i is one Grove port's RX/TX pair and the
// name on its connector. Every lookup below reads them together, so a length
// mismatch would silently pair a port with the wrong partner rather than fail
// to compile.
static_assert(RX_PIN_COUNT == TX_PIN_COUNT,
              "mesh pin tables must describe the same ports");
static_assert(RX_PIN_COUNT == PORT_LABEL_COUNT,
              "every mesh port needs the name printed on its connector");

// ==[ SIZES, READ OUT OF THE FIRMWARE ]==
// meshtastic_Constants_DATA_PAYLOAD_LEN in src/mesh/generated/meshtastic/
// mesh.pb.h is 233, and it is a hard edge rather than a suggestion: the radio
// reads our bytes with readBytes(buf, DATA_PAYLOAD_LEN), so byte 234 is not
// truncated — it stays in the UART buffer and becomes the *next* mesh packet.
// This was 237 here, which meant a long message went out as a 233-byte packet
// followed by a four-character orphan.
static constexpr uint16_t MAX_PAYLOAD = 233;

// The sender the radio stamps on an inbound line is user.short_name, declared
// `char short_name[5]` in mesh.pb.h — four characters. Eight leaves slack for
// firmware that is less disciplined, and narrow is the safe direction: this
// number is also the window the "sender: body" split searches, so every
// character of slack is another chance for a colon in a body to be read as a
// name.
static constexpr uint8_t SENDER_LEN = 8;

// A body can hold a whole payload and nothing more, embedded newlines included.
static constexpr uint16_t BODY_LEN = MAX_PAYLOAD + 1;

// Longest well-formed line: 7 sender characters, ": ", a full payload.
static constexpr uint16_t LINE_CAP = 256;

// ==[ RX FRAMING ]== see sim/mesh_framing_sim.py.
// Messages are normally closed by structure, not by time — the radio brackets
// each one with println(). The quiet gap survives as the fallback for a line
// the radio never terminated, which is exactly what the echo path produces.
// A fixed gap was a bug: a full-length line takes 267ms to arrive at 9600
// baud, so a 300ms window expired while the message was still streaming and
// chopped it. Deriving the window from the baud rate holds the margin at >=4x
// full-line time across every selectable rate.
static constexpr uint32_t QUIET_FLUSH_FLOOR_MS = 300;
static constexpr uint8_t  QUIET_FLUSH_LINE_MULT = 4;

// ==[ TX PACING ]== see sim/mesh_tx_pacing_sim.py.
// The radio reads us with Arduino's Stream::readBytes, whose timeout is PER
// BYTE and whose default is 1000ms. The pin-configured branch of
// SerialModule::runOnce() never calls setTimeout(), so moduleConfig.serial
// .timeout is dead config on this path and that default is the only window
// there is. Two messages written less than a second apart therefore arrive as
// one mesh packet, with no error anywhere to say so. 250ms of margin covers
// millis() granularity on both boards, our frame-tick quantisation, and the
// radio's own 10ms runOnce cadence between finishing one read and starting the
// next.
static constexpr uint32_t TX_GAP_MS = 1250;

// Outbound messages waiting their turn at the gap. Four is a burst of taps on
// the send key; past that the composer refuses rather than silently merging.
static constexpr uint8_t TX_QUEUE_DEPTH = 4;

// ==[ PACING IS A TEXTMSG TAX ]== the gap above exists solely because
// readBytes() merges anything inside its one-second window into one packet.
// PROTO frames carry their own length, so the radio can never glue two of
// them together and there is nothing to wait for. Sending at once is not an
// optimisation here — it is the absence of a bug that only TEXTMSG has.
inline uint32_t txGapMs(Codec codec) {
    return (codec == Codec::PROTO) ? 0u : TX_GAP_MS;
}

// ==[ PROTO SESSION ]==
// The radio ignores want_config_id until its own serial module has come up,
// and it gives no error when it does. Retrying is the only way through, and
// the interval has to outlast a C6L boot without making a live link wait.
static constexpr uint32_t PROTO_HANDSHAKE_RETRY_MS = 4000;

// How many nodes the roster holds. The NodeDB on a busy mesh is far larger
// than a 320x240 list is ever going to show, and each entry is ~64 bytes of
// PSRAM; this is a screen budget, not a mesh one.
static constexpr uint8_t PROTO_MAX_NODES = 32;

// A node the radio has not mentioned in this long is shown as stale rather
// than dropped. Meshtastic's own NodeDB keeps them far longer than that, and
// a roster that forgets is worse than one that greys out.
static constexpr uint32_t NODE_STALE_MS = 900000;   // 15 minutes

// ==[ ECHO ]== with serial.echo set on the C6L, handleReceived() prints our own
// payload back once the router has accepted it. That is the only positive
// confirmation TEXTMSG can give, and it arrives essentially immediately — the
// loopback is local, not a round trip through the mesh. Past this window an
// unconfirmed message is simply never going to be confirmed.
static constexpr uint32_t ECHO_WINDOW_MS = 8000;

// ==[ "NOTHING PARSES" ]== see sim/mesh_baud_noise_sim.py.
// Bytes arriving that never resolve into a sender-prefixed line or an echo
// match. The rule is structural rather than statistical, because statistics do
// not work here: across every baud pairing the settings screen can produce, the
// quietest mismatch still delivers only 5.1% control bytes, which no threshold
// can separate from a clean link's 0% without also condemning any node whose
// short name is an emoji. Grammar separates them completely — mis-sampled
// bytes do not put ": " in the seventh column.
static constexpr uint16_t NOISE_MIN_SAMPLE = 64;

inline uint32_t quietFlushMs(uint32_t baud) {
    if (baud == 0) return QUIET_FLUSH_FLOOR_MS;
    // 8N1 is 10 bits per byte; keep it in integer microseconds so a slow rate
    // does not round its way back down into the unsafe window.
    const uint32_t lineUs = (uint32_t)((10ULL * 1000000ULL * LINE_CAP) / baud);
    const uint32_t derivedMs = (lineUs / 1000u) * QUIET_FLUSH_LINE_MULT;
    return derivedMs > QUIET_FLUSH_FLOOR_MS ? derivedMs : QUIET_FLUSH_FLOOR_MS;
}

inline bool isSupportedRxPin(uint8_t pin) {
    for (uint8_t i = 0; i < RX_PIN_COUNT; ++i) {
        if (RX_PINS[i] == pin) return true;
    }
    return false;
}

inline bool isSupportedTxPin(uint8_t pin) {
    for (uint8_t i = 0; i < TX_PIN_COUNT; ++i) {
        if (TX_PINS[i] == pin) return true;
    }
    return false;
}

inline bool isSupportedBaud(uint32_t baud) {
    return baud == 9600 || baud == 38400 || baud == 115200;
}

// Only the RX leg has a stepper. Selecting a route picks both legs of one
// connector, so Config::setMeshRxPin drags the TX partner along — offering the
// two independently only ever produced the half-Port-C, half-Port-B pair that
// isSamePort() below exists to reject.
inline uint8_t nextRxPin(uint8_t pin) {
    for (uint8_t i = 0; i < RX_PIN_COUNT; ++i) {
        if (RX_PINS[i] == pin) return RX_PINS[(i + 1) % RX_PIN_COUNT];
    }
    return RX_PIN;
}

// The connector an RX leg is on. Unknown pins read as the default route, which
// is the same repair sanitize() would make.
inline const char* portLabel(uint8_t rxPin) {
    for (uint8_t i = 0; i < RX_PIN_COUNT; ++i) {
        if (RX_PINS[i] == rxPin) return PORT_LABELS[i];
    }
    return PORT_LABELS[0];
}

// Whether the link is on its default connector, which is the one the GPS
// module also wants — see the file header.
inline bool isDefaultPort(uint8_t rxPin) { return rxPin == RX_PINS[0]; }

inline uint32_t nextBaud(uint32_t baud) {
    if (baud == 9600) return 38400;
    if (baud == 38400) return 115200;
    return 9600;
}

inline void sanitize(uint8_t& rxPin, uint8_t& txPin, uint32_t& baud,
                     bool& repaired) {
    const uint8_t fixedRx = isSupportedRxPin(rxPin) ? rxPin : RX_PIN;
    const uint8_t fixedTx = isSupportedTxPin(txPin) ? txPin : TX_PIN;
    const uint32_t fixedBaud = isSupportedBaud(baud) ? baud : DEFAULT_BAUD;
    repaired = repaired || fixedRx != rxPin || fixedTx != txPin ||
               fixedBaud != baud;
    rxPin = fixedRx;
    txPin = fixedTx;
    baud = fixedBaud;
}

// Two pins on the same Grove port. Mixing Port C's RX with Port B's TX would
// split the link across two connectors and read as a dead radio.
inline bool isSamePort(uint8_t rxPin, uint8_t txPin) {
    for (uint8_t i = 0; i < RX_PIN_COUNT && i < TX_PIN_COUNT; ++i) {
        if (RX_PINS[i] == rxPin) return TX_PINS[i] == txPin;
    }
    return false;
}

inline bool isValidConfig(uint8_t rxPin, uint8_t txPin, uint32_t baud) {
    return isSupportedRxPin(rxPin) && isSupportedTxPin(txPin) &&
           rxPin != txPin && isSamePort(rxPin, txPin) && isSupportedBaud(baud);
}

}  // namespace MeshUartPolicy
