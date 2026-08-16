/**
 * Radio ownership policy.
 *
 * ==[ ONE RF FRONTEND ]== a mode in this set owns WiFi channel/state.
 * FLOCK must release ESP-NOW before one of them starts, then stay dark until
 * that owner exits. Keep this list as the single source of truth.
 */
#pragma once

#include "../hamlet.h"

namespace RadioPolicy {

constexpr bool modeOwnsWifi(HamletMode mode) {
    return mode == HamletMode::HUNT ||
           mode == HamletMode::SPECTRUM ||
           mode == HamletMode::WEBCONFIG ||
           mode == HamletMode::WARDRIVE ||
           mode == HamletMode::XFER;
}

} // namespace RadioPolicy
