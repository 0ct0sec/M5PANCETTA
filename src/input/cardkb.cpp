/**
 * CardKB - Unit CardKB v1.1 poller
 *
 * ==[ CARD IN THE SLOT ]== one address, one byte, no registers. Every tick
 * either probes for the unit or lifts one keystroke off it and files it in the
 * FIFO. Nothing here blocks: a 1-byte transaction at 100kHz costs roughly
 * 200us of bus time, and an absent unit costs one unacknowledged address.
 */

#include "cardkb.h"

#include <M5Unified.h>

#include "../core/config.h"
#include "../hal/platform.h"
#include "../util/debug_log.h"

namespace CardKB {

// ==[ BUS ]== vendor-fixed address and rate (User Manual, I2C Information).
static constexpr uint8_t  I2C_ADDR = 0x5F;
static constexpr uint32_t I2C_FREQ = 100000;

// ==[ CADENCE ]== the unit remembers one keystroke, so the poll interval is
// the ceiling on typing speed: 20ms holds bursts up to 50 keys/second intact.
// Probing is a hundred times lazier because an empty port has nothing to say.
static constexpr uint32_t POLL_MS  = 20;
static constexpr uint32_t PROBE_MS = 2000;

// A single dropped ACK is a rattled cable, not an unplug. Three in a row on a
// 20ms cadence is 60ms of silence, which no keypress can produce.
static constexpr uint8_t MISS_LIMIT = 3;

// ==[ QUEUE ]== power-of-two ring; a burst deeper than this outran the frame.
static constexpr uint8_t QUEUE_SIZE = 16;
static constexpr uint8_t QUEUE_MASK = QUEUE_SIZE - 1;

static uint8_t  queue[QUEUE_SIZE];
static uint8_t  queueHead = 0;   // next slot to write
static uint8_t  queueTail = 0;   // next slot to read
static uint8_t  queueCount = 0;

static bool     busReady = false;
static bool     present = false;
static uint8_t  missStreak = 0;
static uint32_t nextPollMs = 0;

static bool     attachPending = false;
static bool     attachState = false;

// Port A carries UART2 for the C5 bridge on Core2. Sharing the pins between a
// running serial link and an I2C driver corrupts both, so the keypad yields.
// CoreS3 SE reaches C5 over the M-Bus and keeps Port A to itself.
static bool portAvailable() {
#if HAMLET_TARGET_CORES3SE
    return true;
#else
    return !Config::getC5Enabled();
#endif
}

static void pushKey(uint8_t key) {
    if (queueCount >= QUEUE_SIZE) {
        // Oldest key loses. A full queue means no consumer drained this frame,
        // and the newest intent is the one the user is still waiting on.
        queueTail = (uint8_t)((queueTail + 1) & QUEUE_MASK);
        queueCount--;
    }
    queue[queueHead] = key;
    queueHead = (uint8_t)((queueHead + 1) & QUEUE_MASK);
    queueCount++;
}

static void setPresent(bool nowPresent) {
    if (present == nowPresent) return;
    present = nowPresent;
    attachState = nowPresent;
    attachPending = true;
    if (!nowPresent) flush();
    HAMLET_LOGF("[CARDKB] %s\n", nowPresent ? "linked" : "gone");
}

static bool ensureBus() {
    if (busReady) return true;
    // setPort() ran inside M5.begin(); a port number below zero means this
    // board exposes no external I2C at all.
    if (!M5.Ex_I2C.isEnabled()) return false;
    busReady = M5.Ex_I2C.begin();
    return busReady;
}

static void closeBus() {
    if (!busReady) return;
    M5.Ex_I2C.release();
    busReady = false;
}

// One transaction: address the unit for reading and take its single byte.
// Returns false when the address goes unacknowledged - that is the unplugged
// case, and it is the only signal the unit gives.
static bool readKeyByte(uint8_t& out) {
    if (!M5.Ex_I2C.start(I2C_ADDR, true, I2C_FREQ)) {
        M5.Ex_I2C.stop();
        return false;
    }
    const bool ok = M5.Ex_I2C.read(&out, 1, true);  // NACK the last byte
    M5.Ex_I2C.stop();
    return ok;
}

// ==[ PUBLIC API ]==

void begin() {
    queueHead = queueTail = queueCount = 0;
    busReady = false;
    present = false;
    missStreak = 0;
    nextPollMs = 0;
    attachPending = false;
    attachState = false;
}

void update(uint32_t now) {
    if (!portAvailable()) {
        // The bridge took the port mid-session: hand the pins back before its
        // UART driver claims them.
        setPresent(false);
        closeBus();
        return;
    }

    if ((int32_t)(now - nextPollMs) < 0) return;
    nextPollMs = now + (present ? POLL_MS : PROBE_MS);

    if (!ensureBus()) return;

    uint8_t key = KEY_NONE;
    if (!readKeyByte(key)) {
        if (present && ++missStreak >= MISS_LIMIT) {
            setPresent(false);
        }
        return;
    }

    missStreak = 0;
    setPresent(true);

    if (key != KEY_NONE) {
        pushKey(key);
        // A held key keeps producing bytes; poll again next tick rather than
        // waiting out a probe interval that no longer applies.
        nextPollMs = now + POLL_MS;
    }
}

bool isPresent() { return present; }

bool consumeAttachEvent(bool& attached) {
    if (!attachPending) return false;
    attached = attachState;
    attachPending = false;
    return true;
}

bool available() { return queueCount > 0; }

uint8_t read() {
    if (queueCount == 0) return KEY_NONE;
    const uint8_t key = queue[queueTail];
    queueTail = (uint8_t)((queueTail + 1) & QUEUE_MASK);
    queueCount--;
    return key;
}

void flush() {
    queueHead = queueTail = queueCount = 0;
}

}  // namespace CardKB
