/**
 * CardKB - M5Stack Unit CardKB v1.1 (ATmega8A) QWERTY keypad on Grove Port A
 *
 * ==[ CARD IN THE SLOT ]== 50 keys behind one I2C register at 0x5F, 100kHz.
 * The unit holds a single byte: the value of the most recent keypress, or
 * 0x00 for "nothing pressed". Reading consumes it, so this poller must be the
 * only reader on the bus; every consumer takes keys out of the FIFO below.
 *
 * Because the unit buffers exactly one keystroke, the poll interval is the
 * typing speed ceiling. POLL_MS is the budget that keeps a fast burst intact.
 *
 * Port A is shared iron. On Core2 the C5 bridge drives G32/G33 as UART2, so
 * the keypad stays dormant for as long as that bridge owns the port. CoreS3 SE
 * lands Port A on G2/G1 and routes C5 through the M-Bus, so no conflict exists
 * there. Neither target's GPS touches Port A - it rides the M-Bus header.
 *
 * Key codes are the vendor table (Unit CardKB v1.1 User Manual, "Key Code
 * Mapping Table"). Shift/Sym/Fn are resolved inside the unit, so a character
 * key arrives as the finished ASCII byte and needs no modifier tracking here.
 */
#ifndef CARDKB_H
#define CARDKB_H

#include <stdint.h>

namespace CardKB {

// ==[ VENDOR KEY CODES ]== normal and Caps/Sym modes emit plain ASCII; only
// the four cursor keys leave the printable range.
static constexpr uint8_t KEY_NONE      = 0x00;
static constexpr uint8_t KEY_BACKSPACE = 0x08;
static constexpr uint8_t KEY_TAB       = 0x09;
static constexpr uint8_t KEY_ENTER     = 0x0D;
static constexpr uint8_t KEY_ESC       = 0x1B;
static constexpr uint8_t KEY_SPACE     = 0x20;
static constexpr uint8_t KEY_LEFT      = 0xB4;
static constexpr uint8_t KEY_UP        = 0xB5;
static constexpr uint8_t KEY_DOWN      = 0xB6;
static constexpr uint8_t KEY_RIGHT     = 0xB7;

// ==[ LIFECYCLE ]==
// begin() only arms the state machine. The Port A bus stays closed until the
// first probe, because M5.begin() assigns Port A's pins to Ex_I2C without ever
// installing the driver unless an external RTC or IMU was declared.
void begin();

// Poll/probe tick. Call once per frame, before anything drains the FIFO.
void update(uint32_t now);

// ==[ PRESENCE ]==
bool isPresent();

// One-shot link/unlink edge, for the UI that announces the unit.
bool consumeAttachEvent(bool& attached);

// ==[ KEY QUEUE ]==
bool available();
uint8_t read();   // KEY_NONE when the queue is empty
void flush();     // drop pending keys (mode changes, blocked input)

// A character the unit already resolved through Shift/Sym; SPACE included.
inline bool isPrintable(uint8_t key) {
    return key >= 0x20 && key < 0x7F;
}

}  // namespace CardKB

#endif  // CARDKB_H
