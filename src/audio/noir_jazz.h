/**
 * Noir Jazz — Procedural ambient music generator
 *
 * PCM synthesis engine: 8kHz mono 8-bit signed, double-buffered bar generation.
 * Two modes:
 *   NOIR_JAZZ     — bar-room jazz (menu, hunt, spectrum)
 *   BLADE_RUNNER  — Vangelis-inspired F#m synth ambient (wardrive)
 * Plays on M5.Speaker channel 1 (SFX stays on channel 0).
 * PSRAM-allocated buffers (~50KB for two bars).
 */

#ifndef NOIR_JAZZ_H
#define NOIR_JAZZ_H

#include <stdint.h>

namespace NoirJazz {

/// Music mode
enum class Mode : uint8_t {
    NOIR_JAZZ = 0,    // walking bass, pad, brush, sax — Am pentatonic
    BLADE_RUNNER = 1  // CS-80 synth lead, string pad, rain wash — F#m blues
};

/// Call once at boot (allocates PSRAM buffers, builds sine table)
void init();

/// Begin playback — fades in over ~2 bars
void start();

/// Stop playback — fades out over ~1 bar then releases speaker channel
void stop();

/// Immediate hardware-safe stop for controlled sleep/power transitions.
void stopImmediate();

/// Pump from main loop (fills next buffer, feeds speaker). Call every frame.
/// Returns true while music is audible.
bool update();

/// Set the music mode. Takes effect at next bar boundary.
void setMode(Mode m);

/// Game-state tension [0.0 = calm booth, 1.0 = peak hunt].
/// Affects BPM (68-88 noir, 60-68 blade runner), percussion density.
void setTension(float t);

/// Lead/sax layer — fades in slowly when true, fades out when false.
void setSaxActive(bool active);

/// Music volume (0-10, persisted in Config)
uint8_t getVolume();
void setVolume(uint8_t vol);

/// Is the generator currently producing audio?
bool isPlaying();

/// Returns true once per bar when a brush backbeat hit was rendered.
/// Only fires in NOIR_JAZZ mode. Resets on read.
bool consumeBackbeatHit();

/// Returns true once when a rain drop noise was rendered.
/// Only fires in NOIR_JAZZ mode. Resets on read.
bool consumeRainDrop();

}  // namespace NoirJazz

#endif  // NOIR_JAZZ_H
