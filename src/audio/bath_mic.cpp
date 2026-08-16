#include "bath_mic.h"
#include "bath_beat_tracker.h"

#if defined(HAMLET_CORE3SE)

#include <M5Unified.h>
#include <string.h>

#include "noir_jazz.h"
#include "sfx.h"
#include "../core/config.h"

namespace BathMic {
namespace {

// A 20 ms mono window is enough for a responsive energy envelope, and it is
// overwritten before the next window. It is never persisted, transmitted, or
// interpreted as speech.
static constexpr uint32_t kSampleRate = 8000u;
static constexpr size_t kSampleCount = 160u;
// The ES7210 is a little conservative at the old threshold.  Lower all three
// gates by one quarter together, retaining the relative noise and transient
// protections while allowing quieter room music to establish a cadence.
static constexpr uint16_t kMinimumActivity = 420u;
static constexpr uint16_t kActivityMargin = 180u;
static constexpr uint16_t kBeatRise = 120u;

static int16_t sampleWindow[kSampleCount] = {};
static bool listening = false;
static bool samplePending = false;
static bool dancing = false;
static uint16_t ambientLevel = 0u;
static uint16_t previousLevel = 0u;
static bool wasAboveFloor = false;
static uint32_t retryAfterMs = 0u;
static BathBeat::Tracker beatTracker;

static void clearEnergyState() {
    samplePending = false;
    dancing = false;
    ambientLevel = 0u;
    previousLevel = 0u;
    wasAboveFloor = false;
    retryAfterMs = 0u;
    beatTracker.reset();
    memset(sampleWindow, 0, sizeof(sampleWindow));
}

static void restoreSpeaker() {
    M5.Mic.end();
    M5.Speaker.begin();
    SFX::init();
    listening = false;
    clearEnergyState();
}

static bool startListening(uint32_t now) {
    // M5Unified assigns the CoreS3 speaker and ES7210 capture stream to the
    // same I2S port. Make the handoff explicit instead of stealing the port
    // from live audio underneath its producers.
    NoirJazz::stopImmediate();
    SFX::stop();
    M5.Speaker.end();

    auto cfg = M5.Mic.config();
    cfg.sample_rate = kSampleRate;
    cfg.input_channel = m5::input_only_left;
    cfg.over_sampling = 1;
    cfg.noise_filter_level = 0;
    cfg.dma_buf_len = kSampleCount;
    cfg.dma_buf_count = 2;
    M5.Mic.config(cfg);

    listening = M5.Mic.begin();
    if (!listening) {
        M5.Speaker.begin();
        SFX::init();
        retryAfterMs = now + 1000u;
        return false;
    }

    clearEnergyState();
    return true;
}

static void consumeCompletedWindow(uint32_t now) {
    uint32_t level = 0u;
    for (size_t i = 0; i < kSampleCount; ++i) {
        const int32_t sample = sampleWindow[i];
        level += static_cast<uint32_t>(sample < 0 ? -sample : sample);
    }
    level /= kSampleCount;

    if (ambientLevel == 0u) ambientLevel = static_cast<uint16_t>(level);
    const uint16_t learnedThreshold = static_cast<uint16_t>(
        ambientLevel + kActivityMargin);
    const uint16_t threshold = learnedThreshold > kMinimumActivity
        ? learnedThreshold : kMinimumActivity;
    const bool aboveFloor = level >= threshold;
    const uint16_t rise = level > previousLevel
        ? static_cast<uint16_t>(level - previousLevel) : 0u;
    const bool onset = aboveFloor &&
        (!wasAboveFloor || rise >= kBeatRise);

    // Learn the room only below the active threshold, so a sustained beat
    // cannot train itself out of recognition.
    if (!aboveFloor) {
        ambientLevel = static_cast<uint16_t>(
            ((uint32_t)ambientLevel * 31u + level) / 32u);
    }

    beatTracker.observe(onset, now);
    dancing = beatTracker.isActive();

    previousLevel = static_cast<uint16_t>(level);
    wasAboveFloor = aboveFloor;
}

static void sampleAudio(uint32_t now) {
    if (samplePending && M5.Mic.isRecording() == 0u) {
        consumeCompletedWindow(now);
        samplePending = false;
    }
    if (!samplePending) {
        samplePending = M5.Mic.record(sampleWindow, kSampleCount,
                                      kSampleRate, false);
    }
}

}  // namespace

void update(bool bathEligible, uint32_t now) {
    const bool shouldListen = bathEligible && Config::getBathMicEnabled();
    if (shouldListen && !listening &&
        (retryAfterMs == 0u || (int32_t)(now - retryAfterMs) >= 0)) {
        startListening(now);
    }
    if (!shouldListen && listening) {
        restoreSpeaker();
        return;
    }
    if (listening) sampleAudio(now);
}

bool isAudioBusReserved() {
    return listening;
}

bool isDanceActive() {
    return listening && dancing;
}

uint8_t danceBeatPhase(uint32_t now) {
    if (!isDanceActive()) return 0u;
    return beatTracker.phase(now);
}

}  // namespace BathMic

#else

namespace BathMic {

void update(bool, uint32_t) {}
bool isAudioBusReserved() { return false; }
bool isDanceActive() { return false; }
uint8_t danceBeatPhase(uint32_t) { return 0u; }

}  // namespace BathMic

#endif
