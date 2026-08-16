/**
 * Pure power-control policy shared by firmware dispatch and host tests.
 *
 * Keep action semantics here so the menu, confirmation flow, and hardware
 * dispatch cannot disagree about which choices clear live memory or return.
 */
#pragma once

#include <stdint.h>

namespace PowerPolicy {

enum class Action : uint8_t {
    DEEP_SLEEP = 0,
    LIGHT_SLEEP,
    POWER_OFF,
    CANCEL,
    COUNT
};

static constexpr Action SAFE_DEFAULT_ACTION = Action::CANCEL;
static constexpr uint8_t ACTION_COUNT = static_cast<uint8_t>(Action::COUNT);

constexpr Action actionFromIndex(int index) {
    return (index >= 0 && index < ACTION_COUNT)
        ? static_cast<Action>(index)
        : SAFE_DEFAULT_ACTION;
}

constexpr uint8_t actionIndex(Action action) {
    return static_cast<uint8_t>(action);
}

constexpr bool clearsLiveMemory(Action action) {
    return action == Action::DEEP_SLEEP || action == Action::POWER_OFF;
}

constexpr bool returnsInPlace(Action action) {
    return action == Action::LIGHT_SLEEP || action == Action::CANCEL;
}

constexpr bool endsSession(Action action) {
    return clearsLiveMemory(action);
}

constexpr bool requiresConfirmation(Action action) {
    return clearsLiveMemory(action);
}

constexpr bool isAvailable(Action action, bool hasTouchSleepWake) {
    return hasTouchSleepWake ||
        (action != Action::DEEP_SLEEP && action != Action::LIGHT_SLEEP);
}

struct BatteryState {
    bool low;
    bool critical;
};

static constexpr uint8_t LOW_ENTER_PCT = 30;
static constexpr uint8_t LOW_RECOVER_PCT = 33;
static constexpr uint8_t CRITICAL_ENTER_PCT = 10;
static constexpr uint8_t CRITICAL_RECOVER_PCT = 12;

inline BatteryState evaluateBatteryState(uint8_t batteryPct,
                                         bool adaptationEnabled,
                                         bool externalPower,
                                         BatteryState previous) {
    if (!adaptationEnabled || externalPower) return {false, false};

    const bool critical = previous.critical
        ? batteryPct < CRITICAL_RECOVER_PCT
        : batteryPct < CRITICAL_ENTER_PCT;
    const bool low = critical || (previous.low
        ? batteryPct < LOW_RECOVER_PCT
        : batteryPct < LOW_ENTER_PCT);
    return {low, critical};
}

}  // namespace PowerPolicy
