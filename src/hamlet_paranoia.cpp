/**
 * Hamlet Paranoia - Deauth detection implementation
 *
 * ==[ PARANOID SWINE ]== pure state machine, no hardware deps.
 */

#include "hamlet_paranoia.h"

namespace HamletParanoia {

void reset(State& state) {
    state.deauthDetected.store(false);
    state.deauthRSSI.store(-100);
    state.deauthChannel.store(0);
    state.toastStart = 0;
    state.toastActive = false;
}

void triggerDeauth(State& state, int8_t rssi, uint8_t channel) {
    state.deauthRSSI.store(rssi);
    state.deauthChannel.store(channel);
    state.deauthDetected.store(true, std::memory_order_release);
}

bool update(State& state, uint32_t now) {
    // Check if new deauth was detected (cross-core flag). Use exchange so a
    // second triggerDeauth() that lands between load() and store() cannot be
    // silently dropped: the previous load/store pair created a TOCTOU window
    // where the newer flag was overwritten by our own clear. exchange returns
    // the prior value and atomically resets — either we see it and re-arm the
    // toast, or the next update() will.
    if (state.deauthDetected.exchange(false, std::memory_order_acq_rel)) {
        state.toastStart = now;
        state.toastActive = true;
    }

    // Auto-dismiss toast after timeout
    if (state.toastActive && (now - state.toastStart >= TOAST_TIMEOUT_MS)) {
        state.toastActive = false;
    }

    return state.toastActive;
}

bool isToastActive(const State& state) {
    return state.toastActive;
}

int8_t getDeauthRSSI(const State& state) {
    return state.deauthRSSI.load(std::memory_order_acquire);
}

uint8_t getDeauthChannel(const State& state) {
    return state.deauthChannel.load(std::memory_order_acquire);
}

}  // namespace HamletParanoia
