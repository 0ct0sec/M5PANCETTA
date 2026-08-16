#pragma once

#include <stdint.h>

#include "../hal/platform.h"

namespace C5UartPolicy {

#if HAMLET_TARGET_CORES3SE
static constexpr bool DEFAULT_ENABLED = true;
static constexpr uint8_t UART_NUM = 1;
static constexpr uint8_t RX_PIN = 44;  // CoreS3 SE M-Bus RXD0
static constexpr uint8_t TX_PIN = 43;  // CoreS3 SE M-Bus TXD0
#else
static constexpr bool DEFAULT_ENABLED = false;
static constexpr uint8_t UART_NUM = 2;
// Monster Grove uses the Core2 Port A signals. G16/G17 are reserved for PSRAM.
static constexpr uint8_t RX_PIN = 33;  // Core2 Grove white <- C5Monster TX
static constexpr uint8_t TX_PIN = 32;  // Core2 Grove yellow -> C5Monster RX
#endif

static constexpr uint32_t DEFAULT_BAUD = 115200;

inline bool isSupportedPin(uint8_t pin) {
    return pin == RX_PIN || pin == TX_PIN;
}

inline bool isSupportedBaud(uint32_t baud) {
    return baud == 9600 || baud == DEFAULT_BAUD;
}

inline void sanitizePins(uint8_t& rxPin, uint8_t& txPin, bool& repaired) {
    const uint8_t fixedRx = isSupportedPin(rxPin) ? rxPin : RX_PIN;
    uint8_t fixedTx = isSupportedPin(txPin) ? txPin : TX_PIN;
    if (fixedRx == fixedTx) {
        fixedTx = (fixedRx == TX_PIN) ? RX_PIN : TX_PIN;
    }
    repaired = repaired || fixedRx != rxPin || fixedTx != txPin;
    rxPin = fixedRx;
    txPin = fixedTx;
}

inline bool isValidConfig(uint8_t rxPin, uint8_t txPin, uint32_t baud) {
    const bool pinsOk =
        (rxPin == RX_PIN && txPin == TX_PIN) ||
        (rxPin == TX_PIN && txPin == RX_PIN);
    return pinsOk && isSupportedBaud(baud) && rxPin != txPin;
}

}  // namespace C5UartPolicy
