/**
 * Geiger - RSSI cadence and pitch feedback
 *
 * ==[ SIGNAL IN THE VENTS ]== Packets arrive with noisy RSSI, not distance.
 * This module clamps abrupt samples, filters them with a fixed-point EMA, and
 * maps the result to click cadence. A short-window delta bends pitch up or
 * down; sample age progressively slows the cadence and eventually removes
 * trend evidence.
 *
 * The sound is an operator cue, not ranging or direction finding. Environment,
 * antenna orientation, multipath, and transmitter power all change RSSI.
 * RAD/THRU selection comes from the cached device posture and falls back to RAD
 * when pose evidence is absent or stale.
 */

#include "geiger.h"
#include "../audio/sfx.h"
#include "../core/config.h"
#include "../activity/pedometer.h"
#include <M5Unified.h>
#include <math.h>

namespace Geiger {

// ==[ CASE STATE ]== one active source owns the clicker at a time
static bool active = false;
static Source currentSource = SOURCE_NONE;
static uint32_t lastClickTime = 0;
static uint32_t lastSampleTime = 0;

// ==[ RSSI FILTER ]== absolute endpoints keep the cue stable across sessions
// -85dBm maps to the slow end; -15dBm maps to the fast end. These are display
// anchors, not physical near/far thresholds.
static constexpr int8_t RSSI_MIN = -85;
static constexpr int8_t RSSI_MAX = -15;
static constexpr uint8_t EMA_ALPHA = 32;  // 32/256 = 0.125 sample weight
static int16_t smoothedRssi_x256 = -70 * 256;  // Q8 RSSI accumulator
static int8_t lastSmoothedRssi = -70;
static int8_t lastEffectiveRssi = -70;  // previous clamped sample
static bool firstSample = true;         // first sample establishes the baseline

// ==[ TREND ]== a 5Hz delta tints pitch without claiming velocity
static constexpr uint32_t SAMPLE_INTERVAL = 200;
static int8_t rssiTrend = 0;  // positive is stronger; negative is weaker
static constexpr int8_t TREND_THRESHOLD = 4;  // ignore changes within ±4dB

// ==[ AUDIO MAP ]== stronger RSSI produces faster, higher clicks
static constexpr uint16_t INTERVAL_NEAR = 60;     // ~16.7Hz ceiling
static constexpr uint16_t INTERVAL_FAR  = 1000;   // 1Hz floor
static constexpr uint16_t FREQ_NEAR = 3600;
static constexpr uint16_t FREQ_FAR  = 1800;
// Short pulses leave room for alerts and keep continuous tracking unobtrusive.
static constexpr uint8_t CLICK_NEAR_MS = 8;
static constexpr uint8_t CLICK_FAR_MS  = 6;

// ==[ EXPONENTIAL CADENCE ]== each dB step changes perceived urgency smoothly
// interval = 1000 * (60/1000)^t where t = (rssi - RSSI_MIN) / (RSSI_MAX - RSSI_MIN)
// 71 entries, 142 bytes. Index = constrain(rssi - RSSI_MIN, 0, 70)
static constexpr uint16_t INTERVAL_LUT[71] = {
    1000, 961, 923, 886, 851, 818, 786, 755, 725, 696,  // -85 to -76
     669, 643, 617, 593, 570, 547, 526, 505, 485, 466,  // -75 to -66
     448, 430, 413, 397, 381, 366, 352, 338, 325, 312,  // -65 to -56
     299, 288, 276, 265, 255, 245, 235, 226, 217, 209,  // -55 to -46
     200, 192, 185, 178, 171, 164, 157, 151, 145, 140,  // -45 to -36
     134, 129, 124, 119, 114, 110, 105, 101,  97,  93,  // -35 to -26
      90,  86,  83,  79,  76,  73,  70,  68,  65,  62,  // -25 to -16
      60                                                  // -15
};

// ==[ STALENESS ]== old evidence slows to a neutral heartbeat
static constexpr uint32_t STALE_SILENCE_MS = 5000;
static constexpr uint16_t STALE_INTERVAL = 1000;
static constexpr uint16_t STALE_FREQ = 1600;
static constexpr uint8_t STALE_CLICK_MS = 6;

// ==[ RF VIEW POSTURE ]== flat or untrusted pose selects RAD; upright selects THRU
static constexpr uint32_t POSE_HOLD_MS = 1500;
static bool poseCacheValid = false;
static bool poseFlatCached = true;
static uint32_t poseCacheTime = 0;

static bool accelPoseLooksValid(float ax, float ay, float az) {
    const float mag2 = ax * ax + ay * ay + az * az;
    return mag2 > 0.16f && mag2 < 3.24f;  // accept 0.4g..1.8g magnitude
}

static bool resolveFlatForView(uint32_t now) {
    float ax, ay, az;
    Pedometer::getCachedAccel(ax, ay, az);
    if (accelPoseLooksValid(ax, ay, az)) {
        poseFlatCached = Pedometer::isCachedFlat();
        poseCacheTime = now;
        poseCacheValid = true;
        return poseFlatCached;
    }

    if (poseCacheValid && (now - poseCacheTime) <= POSE_HOLD_MS) {
        return poseFlatCached;
    }
    return true;  // RAD is the evidence-safe fallback without valid pose
}

// ==[ TREND FILTER ]== fixed-cadence delta favors stability over velocity estimates
static void updateTrend(int8_t currentSmoothed) {
    if (firstSample) {
        // First contact establishes a baseline; one sample cannot show a trend.
        firstSample = false;
        lastSmoothedRssi = currentSmoothed;
        rssiTrend = 0;
        return;
    }
    
    // Sampling is already cadence-gated, so a raw delta is less noisy than a
    // time-normalized estimate and makes no false speed claim.
    rssiTrend = (int8_t)constrain(currentSmoothed - lastSmoothedRssi, -20, 20);
    lastSmoothedRssi = currentSmoothed;
}

// ==[ PUBLIC API ]== open the case, feed evidence, close the case

void start(Source src) {
    active = true;
    currentSource = src;
    lastClickTime = 0;
    lastSampleTime = 0;
    smoothedRssi_x256 = -70 * 256;  // neutral starting point until first sample
    lastSmoothedRssi = -70;
    lastEffectiveRssi = -70;
    rssiTrend = 0;
    firstSample = true;
}

void stop() {
    active = false;
    currentSource = SOURCE_NONE;
}

bool isActive() {
    return active;
}

Source getSource() {
    return currentSource;
}

bool update(int8_t rssi) {
    return update(rssi, 0);
}

bool update(int8_t rssi, uint32_t ageMs) {
    if (!active) return false;
    if (!Config::getSoundEnabled()) return false;
    
    uint32_t now = millis();

    // ==[ OUTLIER CLAMP ]== one bad frame may bend the trace, never break it
    // Limit each accepted change to ±6dB before it reaches the EMA.
    int8_t rssiIn = rssi;
    if (!firstSample) {
        rssiIn = (int8_t)constrain((int)rssiIn, (int)lastEffectiveRssi - 6, (int)lastEffectiveRssi + 6);
    }
    
    // ==[ Q8 EMA ]== smoothed = alpha*raw + (1-alpha)*previous
    int32_t emaTemp = EMA_ALPHA * (int32_t)rssiIn * 256 + (256 - EMA_ALPHA) * (int32_t)smoothedRssi_x256;
    // Division truncates toward zero, so bias both signs before returning to Q8.
    emaTemp += (emaTemp >= 0) ? 128 : -128;
    smoothedRssi_x256 = (int16_t)(emaTemp / 256);
    int16_t tmp = smoothedRssi_x256;
    tmp += (tmp >= 0) ? 128 : -128;
    int8_t effectiveRssi = (int8_t)(tmp / 256);
    lastEffectiveRssi = effectiveRssi;
    
    // Trend is sampled at a fixed cadence so frame time does not change its gain.
    if (now - lastSampleTime >= SAMPLE_INTERVAL) {
        updateTrend(effectiveRssi);
        lastSampleTime = now;
    }
    
    // ==[ CADENCE LOOKUP ]== clamp RSSI into the 71-entry exponential table
    int8_t clamped = (int8_t)constrain((int)effectiveRssi, (int)RSSI_MIN, (int)RSSI_MAX);
    uint16_t interval = INTERVAL_LUT[clamped - RSSI_MIN];
    
    interval = constrain(interval, INTERVAL_NEAR, INTERVAL_FAR);

    // ==[ AGE GATE ]== the trail goes cold gradually instead of snapping
    if (ageMs > 0) {
        if (ageMs <= 250) {
            // Fresh samples keep the RSSI-derived cadence.
        } else if (ageMs >= 5000) {
            // At five seconds, retain only a neutral presence tick.
            interval = STALE_INTERVAL;
            rssiTrend = 0;
        } else {
            // Between fresh and stale, a linear floor reduces claimed urgency.
            uint16_t minInterval;
            if (ageMs <= 1500) {
                minInterval = (uint16_t)map((int)ageMs, 250, 1500, 120, 400);
            } else {
                minInterval = (uint16_t)map((int)ageMs, 1500, 5000, 400, STALE_INTERVAL);
            }
            if (interval < minInterval) interval = minInterval;
        }
    }
    
    // ==[ TEXTURE ]== bounded wobble prevents a synthetic metronome sound
    int8_t jitterPct = (int8_t)(esp_random() % 17) - 8;  // -8%..+8%
    interval = (uint16_t)((interval * (100 + jitterPct)) / 100);
    interval = constrain(interval, 40, 1500);

    if (now - lastClickTime < interval) {
        return false;
    }
    
    // ==[ SFX ARBITRATION ]== alerts outrank the tracker
    // Advance the clock even when suppressed; otherwise the clicker fires a
    // misleading catch-up burst as soon as the higher-priority sound ends.
    if (SFX::isPlaying()) {
        lastClickTime = now;
        return false;
    }
    
    // ==[ PITCH ]== RSSI sets the base; trend and texture add bounded offsets
    uint16_t freq = (uint16_t)map(clamped, RSSI_MIN, RSSI_MAX, FREQ_FAR, FREQ_NEAR);
    
    // Trend modifier: warming = shift up, cooling = shift down (max ±160Hz)
    if (rssiTrend > TREND_THRESHOLD) {
        int shift = min(rssiTrend - TREND_THRESHOLD, 8) * 20;
        freq = freq + shift;
    } else if (rssiTrend < -TREND_THRESHOLD) {
        int shift = min(-rssiTrend - TREND_THRESHOLD, 8) * 20;
        freq = freq - shift;
    }
    
    // A ±40Hz wobble gives successive clicks texture without changing the cue.
    freq += (int16_t)((esp_random() % 81) - 40);

    // Stronger signals receive a slightly longer, still-bounded pulse.
    uint8_t clickMs = (uint8_t)map(clamped, RSSI_MIN, RSSI_MAX, CLICK_FAR_MS, CLICK_NEAR_MS);

    // Stale evidence always uses the same quiet tick, independent of old RSSI.
    if (ageMs >= STALE_SILENCE_MS) {
        freq = STALE_FREQ;
        clickMs = STALE_CLICK_MS;
    }

    // The night answers once; record the timestamp before another frame asks.
    SFX::tone(freq, clickMs);
    lastClickTime = now;
    
    return true;
}

// ==[ DIAGNOSTICS ]== expose the filtered evidence used by the audible cue
int8_t getTrend() { return rssiTrend; }
int8_t getSmoothed() {
    int16_t t = smoothedRssi_x256;
    t += (t >= 0) ? 128 : -128;
    return (int8_t)(t / 256);
}

ViewMode getViewMode() {
    return resolveFlatForView(millis()) ? VIEW_RAD : VIEW_THRU;
}

const char* getViewLabel() {
    return getViewMode() == VIEW_THRU ? "THRU" : "RAD";
}

}  // namespace Geiger
