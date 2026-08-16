/**
 * Hamlet Paranoia - Deauth detection and alert system
 *
 * ==[ PARANOID SWINE ]== cross-core deauth detection with toast overlay.
 * Previously inline in hamlet.cpp. Now testable on host.
 *
 * Uses std::atomic for cross-core safety (WiFi task → main loop).
 * On native tests, atomics still work but are single-threaded.
 */
#pragma once

#include <cstdint>
#include <atomic>

namespace HamletParanoia {

// ==[ PARANOIA STATE ]== cross-core deauth detection
struct State {
    std::atomic<bool> deauthDetected{false};
    std::atomic<int8_t> deauthRSSI{-100};
    std::atomic<uint8_t> deauthChannel{0};
    uint32_t toastStart = 0;
    bool toastActive = false;
};

static constexpr uint32_t TOAST_TIMEOUT_MS = 5000;

// Reset state
void reset(State& state);

// Called from WiFi task (core 0) when deauth detected
void triggerDeauth(State& state, int8_t rssi, uint8_t channel);

// Called from main loop (core 1) — checks flag, manages toast lifecycle
// Returns true if toast is currently visible
bool update(State& state, uint32_t now);

// Query toast state
bool isToastActive(const State& state);
int8_t getDeauthRSSI(const State& state);
uint8_t getDeauthChannel(const State& state);

}  // namespace HamletParanoia
