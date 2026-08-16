/**
 * Haptic - Non-blocking vibration motor patterns
 *
 * ==[ RUMBLE ENGINE ]== step-array state machine, update() pumped per frame.
 * Core2 exposes a motor through M5.Power.setVibration(); CoreS3 SE does not.
 * No built-in duration control — we own the timing.
 *
 * Clones SFX:: architecture: fire-and-forget, main-loop-safe.
 */

#ifndef HAPTIC_H
#define HAPTIC_H

#include <stdint.h>

namespace Haptic {

// init motor, load NVS prefs
void init();

// pump state machine (call every frame after M5.update)
void update();

// ==[ PATTERNS ]== fire-and-forget, non-blocking
void raindrop();    // 8ms whisper — rain sync, absolute minimum
void tick();        // 15ms micro — nav, UI taps
void bump();        // 30ms medium — confirm, mode enter
void buzz();        // 80ms sustained — captures, alerts
void pulse();       // 150ms strong — PMKID, level-up
void doubleTap();   // 2×20ms with 40ms gap — handshake

// kill current pattern
void stop();

// is motor running?
bool isActive();

// ==[ POWER SCALING ]== set by Power:: per profile
void setIntensityScale(float scale);  // 0.0-1.0
float getIntensityScale();

// ==[ NVS MASTER SWITCH ]== user toggle
void setEnabled(bool enabled);
bool isEnabled();

// ==[ INTENSITY ]== user knob 0-10
void setIntensity(uint8_t val);
uint8_t getIntensity();

}  // namespace Haptic

#endif  // HAPTIC_H
