/**
 * Pure BLE catalog merge policy shared by firmware and native tests.
 *
 * Keep this header free of Arduino/NimBLE dependencies so policy regressions
 * can be caught on the host before a hardware build.
 */
#pragma once

#include <stdint.h>

namespace ReconBleMath {

// Prefer a more complete preview. When completeness is equal or lower, prefer
// the packet heard more strongly because it is less likely to be truncated or
// corrupted by a marginal receive. A zero-length preview never replaces data.
constexpr bool shouldReplacePayloadPreview(uint8_t incomingLength,
                                           uint8_t currentLength,
                                           int8_t incomingRssi,
                                           int8_t previousRssi) {
    return incomingLength > currentLength ||
           (incomingLength > 0 && incomingRssi > previousRssi);
}

}  // namespace ReconBleMath
