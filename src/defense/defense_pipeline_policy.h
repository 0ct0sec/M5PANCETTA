/** Pure policy helpers for cadence and lifecycle characterization. */
#pragma once

#include "defense_contracts.h"

namespace Defense {

enum class OperatingState : uint8_t {
    BACKGROUND = 0,
    PARASITIC,
    SUSPENDED_KEEP_BLE,
    SUSPENDED_RELEASE_BLE,
    BLE_PRIORITY,
    WARDRIVE
};

enum LifecycleAction : uint16_t {
    ACTION_NONE = 0,
    ACTION_UNFORCE_BLE_PRIORITY = 1u << 0,
    ACTION_UNFORCE_WARDRIVE = 1u << 1,
    ACTION_RESUME = 1u << 2,
    ACTION_ENTER_PARASITIC = 1u << 3,
    ACTION_SUSPEND_KEEP_BLE = 1u << 4,
    ACTION_SUSPEND_RELEASE_BLE = 1u << 5,
    ACTION_FORCE_BLE_PRIORITY = 1u << 6,
    ACTION_FORCE_WARDRIVE = 1u << 7,
};

inline uint16_t lifecycleActions(OperatingState from, OperatingState to) {
    if (from == to) return ACTION_NONE;
    uint16_t actions = ACTION_NONE;
    if (from == OperatingState::BLE_PRIORITY) actions |= ACTION_UNFORCE_BLE_PRIORITY;
    if (from == OperatingState::WARDRIVE) actions |= ACTION_UNFORCE_WARDRIVE;

    const bool needsResume = from == OperatingState::PARASITIC ||
        from == OperatingState::SUSPENDED_KEEP_BLE ||
        from == OperatingState::SUSPENDED_RELEASE_BLE;
    if ((to == OperatingState::BACKGROUND || to == OperatingState::BLE_PRIORITY ||
         to == OperatingState::WARDRIVE) && needsResume) actions |= ACTION_RESUME;

    switch (to) {
        case OperatingState::PARASITIC: actions |= ACTION_ENTER_PARASITIC; break;
        case OperatingState::SUSPENDED_KEEP_BLE: actions |= ACTION_SUSPEND_KEEP_BLE; break;
        case OperatingState::SUSPENDED_RELEASE_BLE: actions |= ACTION_SUSPEND_RELEASE_BLE; break;
        case OperatingState::BLE_PRIORITY: actions |= ACTION_FORCE_BLE_PRIORITY; break;
        case OperatingState::WARDRIVE: actions |= ACTION_FORCE_WARDRIVE; break;
        default: break;
    }
    return actions;
}

struct Cadence {
    uint32_t wifiIntervalMs;
    uint32_t bleIntervalMs;
    uint32_t bleDurationMs;
    uint32_t deauthSniffMs;
    uint32_t parasiticIntervalMs;
    uint32_t sentinelIntervalMs;
    uint32_t sentinelSniffMs;
};

inline Cadence cadenceFor(CadenceTier tier, bool blePriority, bool wardrive) {
    if (wardrive) return {120000, 0, 4000, 3000, 30000, 0, 0};
    if (blePriority) return {120000, 10000, 8000, 3000, 30000, 0, 0};
    switch (tier) {
        case CadenceTier::AGGRESSIVE:
            return {30000, 20000, 10000, 6000, 10000, 5000, 700};
        case CadenceTier::ELEVATED:
            return {60000, 40000, 8000, 4000, 15000, 10000, 400};
        default:
            return {120000, 60000, 8000, 3000, 30000, 0, 0};
    }
}

}  // namespace Defense
