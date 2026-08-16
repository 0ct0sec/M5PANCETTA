/**
 * Haptic - Non-blocking vibration motor implementation
 *
 * ==[ RUMBLE ENGINE ]== {intensity, duration} step arrays + millis() ticking.
 * Same ring-buffer-less simplicity as SFX but without queue — latest pattern wins.
 * Motor latency ~5ms, patterns designed around 15ms minimum pulse.
 */

#include "haptic.h"
#include "../core/config.h"
#include <M5Unified.h>

namespace Haptic {

// ==[ STEP DEFINITION ]== intensity=0 with duration=0 = END sentinel
struct Step {
    uint8_t intensity;   // 0-255 raw motor power
    uint16_t durationMs; // ms for this step (0 = END)
};

// ==[ PATTERN DEFINITIONS ]==

// raindrop: 12ms light tap — minimal but above motor inertia threshold
static const Step PAT_RAINDROP[] = {
    {140, 12},
    {0, 0}  // END
};

// tick: 15ms micro — nav click, UI tap
static const Step PAT_TICK[] = {
    {200, 15},
    {0, 0}  // END
};

// bump: 30ms medium — confirm, mode enter
static const Step PAT_BUMP[] = {
    {220, 30},
    {0, 0}
};

// buzz: 80ms sustained — captures, alerts
static const Step PAT_BUZZ[] = {
    {180, 80},
    {0, 0}
};

// pulse: 150ms strong — PMKID, level-up (ramp feel)
static const Step PAT_PULSE[] = {
    {160, 30},
    {255, 90},
    {120, 30},
    {0, 0}
};

// doubleTap: 2×20ms with 40ms gap — handshake complete
static const Step PAT_DOUBLE_TAP[] = {
    {220, 20},
    {0, 40},    // silence gap
    {220, 20},
    {0, 0}
};

// ==[ STATE MACHINE ]==
static const Step* currentPattern = nullptr;
static uint8_t stepIdx = 0;
static uint32_t stepStartTime = 0;
static uint32_t lastPatternStartMs = 0;
// Reject repeat-fire if the last pattern began < MIN_GAP ago. Fast menu
// scrolling could fire tick() every frame (~17ms at 60fps), running the
// motor effectively continuously. 100ms floor = 10 patterns/sec ceiling,
// which is still snappier than a human's tactile-discrimination threshold.
static constexpr uint32_t MIN_GAP_MS = 100;

// ==[ CONFIG ]==
static float intensityScale = 1.0f;   // power profile scaling
static bool enabled = true;
static uint8_t intensity = 7;         // user knob 0-10

// ==[ HELPERS ]==

static void setMotor(uint8_t raw) {
    if (raw == 0) {
        M5.Power.setVibration(0);
        return;
    }
    // scale by user intensity (0-10) and power profile (0.0-1.0)
    float scaled = (float)raw * ((float)intensity / 10.0f) * intensityScale;
    if (scaled < 1.0f) scaled = 0.0f;
    if (scaled > 255.0f) scaled = 255.0f;
    M5.Power.setVibration((uint8_t)scaled);
}

static void startPattern(const Step* pat) {
    if (!enabled || !pat) return;
    uint32_t now = millis();
    if (lastPatternStartMs != 0 && (now - lastPatternStartMs) < MIN_GAP_MS) {
        return;  // rate-limit: prior pattern still "fresh"
    }
    currentPattern = pat;
    stepIdx = 0;
    stepStartTime = now;
    lastPatternStartMs = now;
    setMotor(pat[0].intensity);
}

// ==[ PUBLIC API ]==

void init() {
    enabled = Config::getHapticEnabled();
    intensity = Config::getHapticIntensity();
    M5.Power.setVibration(0);
}

void update() {
    if (!currentPattern) return;

    uint32_t now = millis();
    const Step& step = currentPattern[stepIdx];

    // END sentinel
    if (step.durationMs == 0 && step.intensity == 0) {
        M5.Power.setVibration(0);
        currentPattern = nullptr;
        stepIdx = 0;
        return;
    }

    // advance when duration elapsed
    if (now - stepStartTime >= step.durationMs) {
        stepIdx++;
        stepStartTime = now;

        const Step& next = currentPattern[stepIdx];
        if (next.durationMs == 0 && next.intensity == 0) {
            // sequence done
            M5.Power.setVibration(0);
            currentPattern = nullptr;
            stepIdx = 0;
        } else {
            setMotor(next.intensity);
        }
    }
}

void raindrop()  { startPattern(PAT_RAINDROP); }
void tick()      { startPattern(PAT_TICK); }
void bump()      { startPattern(PAT_BUMP); }
void buzz()      { startPattern(PAT_BUZZ); }
void pulse()     { startPattern(PAT_PULSE); }
void doubleTap() { startPattern(PAT_DOUBLE_TAP); }

void stop() {
    currentPattern = nullptr;
    stepIdx = 0;
    M5.Power.setVibration(0);
}

bool isActive() {
    return currentPattern != nullptr;
}

void setIntensityScale(float scale) {
    if (scale < 0.0f) scale = 0.0f;
    if (scale > 1.0f) scale = 1.0f;
    intensityScale = scale;
    if (currentPattern) {
        setMotor(currentPattern[stepIdx].intensity);
    }
}

float getIntensityScale() {
    return intensityScale;
}

void setEnabled(bool val) {
    enabled = val;
    Config::setHapticEnabled(val);
    if (!val) stop();
}

bool isEnabled() {
    return enabled;
}

void setIntensity(uint8_t val) {
    if (val > 10) val = 10;
    intensity = val;
    Config::setHapticIntensity(val);
}

uint8_t getIntensity() {
    return intensity;
}

}  // namespace Haptic
