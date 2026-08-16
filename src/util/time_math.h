/**
 * Wrap-safe arithmetic for short millis() deadlines.
 *
 * ESP32 millis() wraps every 2^32 ms. Direct `now >= deadline` comparisons
 * invert around that boundary. Signed subtraction preserves ordering for
 * deadlines less than 2^31 ms (about 24.8 days) away, which covers every
 * UI/radio/storage deadline in HAMLET.
 */
#pragma once

#include <stdint.h>

namespace TimeMath {

constexpr bool reached(uint32_t now, uint32_t deadline) {
    return static_cast<int32_t>(now - deadline) >= 0;
}

constexpr bool before(uint32_t now, uint32_t deadline) {
    return static_cast<int32_t>(now - deadline) < 0;
}

// Zero is the common "no deadline armed" sentinel and should not suppress an
// action merely because uptime has crossed the signed half-range.
constexpr bool reachedOrUnset(uint32_t now, uint32_t deadline) {
    return deadline == 0u || reached(now, deadline);
}

constexpr bool active(uint32_t now, uint32_t deadline) {
    return deadline != 0u && before(now, deadline);
}

}  // namespace TimeMath
