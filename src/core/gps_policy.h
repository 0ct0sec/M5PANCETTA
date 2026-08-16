/**
 * GPS policy shared by firmware and native tests.
 *
 * Hardware profile: M5Stack Module GPS v2.1 (M003-V21, AT6668) stacked on M-BUS.
 * The module's DIP switch selects TXD (switches 1-5) and RXD (switches 6-10) by
 * physical M-BUS pin position. Those positions carry different GPIOs on Core2 vs
 * CoreS3, so the pin/DIP tables below are per-target — see the note there.
 */
#pragma once

#include <stdint.h>

namespace GPSPolicy {

static constexpr uint32_t DEFAULT_BAUD = 115200;
static constexpr uint8_t DEFAULT_BAUD_INDEX = 3;
static constexpr uint32_t FEED_STALE_MS = 2500;

// The DIP switch routes RXD/TXD to fixed M-BUS pin positions, but the GPIO at
// each position moved between Core2 and CoreS3. RX_DIPS[i]/TX_DIPS[i] give the
// physical DIP number for RX_PINS[i]/TX_PINS[i]. Guarded on the board macro
// (not platform.h) so this header stays self-contained for the native host
// test, which never defines it and therefore compiles the Core2 branch.
#if defined(ARDUINO_M5STACK_CORES3)
// CoreS3 landings: DIP6->pin13->G44, DIP7->pin15->G18 (Port.C RX),
// DIP8->pin22->G7, DIP10->pin2->G10; TX DIP2->pin16->G17 (Port.C TX),
// DIP1->pin14->G43 (UART0).
// The Core2 default RX (DIP9->pin26) is HVIN on CoreS3 — a power pin, no data —
// which is exactly why the Core2 route reads dead here. Excluded as unsafe:
// RX DIP9->G14 (I2S DIN), TX DIP3->G13 (I2S), TX DIP5->G0 (boot strapping).
static constexpr uint8_t DEFAULT_RX_PIN = 18;       // DIP 7: CoreS3 Port.C RX
static constexpr uint8_t DEFAULT_TX_PIN = 17;       // DIP 2: CoreS3 Port.C TX
static constexpr uint8_t RX_PINS[] = {44, 18,  7, 10};
static constexpr uint8_t RX_DIPS[] = { 6,  7,  8, 10};
static constexpr uint8_t TX_PINS[] = {17, 43};
static constexpr uint8_t TX_DIPS[] = { 2,  1};
#else
// Core2 (and native host test): DIP number is a contiguous offset per side.
static constexpr uint8_t DEFAULT_RX_PIN = 34;       // DIP 9: Core2 RX <- GNSS TX
static constexpr uint8_t DEFAULT_TX_PIN = 14;       // DIP 2: Core2 TX -> GNSS RX
static constexpr uint8_t RX_PINS[] = {3, 13, 19, 34, 35};
static constexpr uint8_t RX_DIPS[] = {6,  7,  8,  9, 10};
static constexpr uint8_t TX_PINS[] = {1, 14, 2, 27, 0};
static constexpr uint8_t TX_DIPS[] = {1,  2, 3,  4, 5};
#endif
static constexpr uint8_t RX_PIN_COUNT = sizeof(RX_PINS) / sizeof(RX_PINS[0]);
static constexpr uint8_t TX_PIN_COUNT = sizeof(TX_PINS) / sizeof(TX_PINS[0]);

enum class FeedState : uint8_t {
    OFF,
    WAIT,
    RAW_DATA,
    VALID_NMEA,
    FIX,
};

static inline bool isSupportedRxPin(uint8_t pin) {
    for (uint8_t i = 0; i < RX_PIN_COUNT; ++i) {
        if (RX_PINS[i] == pin) return true;
    }
    return false;
}

static inline bool isSupportedTxPin(uint8_t pin) {
    for (uint8_t i = 0; i < TX_PIN_COUNT; ++i) {
        if (TX_PINS[i] == pin) return true;
    }
    return false;
}

static inline uint8_t nextRxPin(uint8_t pin) {
    for (uint8_t i = 0; i < RX_PIN_COUNT; ++i) {
        if (RX_PINS[i] == pin) return RX_PINS[(i + 1) % RX_PIN_COUNT];
    }
    return DEFAULT_RX_PIN;
}

static inline uint8_t nextTxPin(uint8_t pin) {
    for (uint8_t i = 0; i < TX_PIN_COUNT; ++i) {
        if (TX_PINS[i] == pin) return TX_PINS[(i + 1) % TX_PIN_COUNT];
    }
    return DEFAULT_TX_PIN;
}

static inline uint8_t rxDipSwitch(uint8_t pin) {
    for (uint8_t i = 0; i < RX_PIN_COUNT; ++i) {
        if (RX_PINS[i] == pin) return RX_DIPS[i];
    }
    return 0;
}

static inline uint8_t txDipSwitch(uint8_t pin) {
    for (uint8_t i = 0; i < TX_PIN_COUNT; ++i) {
        if (TX_PINS[i] == pin) return TX_DIPS[i];
    }
    return 0;
}

static inline bool isFeedFresh(bool seen, uint32_t nowMs, uint32_t lastSeenMs) {
    return seen && static_cast<uint32_t>(nowMs - lastSeenMs) <= FEED_STALE_MS;
}

static inline FeedState classifyFeed(bool initialized, bool rawData,
                                     bool validNmea, bool fix) {
    if (!initialized) return FeedState::OFF;
    if (fix) return FeedState::FIX;
    if (validNmea) return FeedState::VALID_NMEA;
    if (rawData) return FeedState::RAW_DATA;
    return FeedState::WAIT;
}

static inline const char* badgeFor(FeedState state) {
    switch (state) {
        case FeedState::WAIT:       return "WAIT";
        case FeedState::RAW_DATA:   return "DAT";
        case FeedState::VALID_NMEA: return "GPS";
        case FeedState::FIX:        return "LCK";
        case FeedState::OFF:
        default:                    return nullptr;
    }
}

} // namespace GPSPolicy
