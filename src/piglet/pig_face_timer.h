// PigFaceTimer — shared blink, eye-gaze and ear-twitch micro-animation
// Eliminates duplicated timer logic between avatar.cpp and menu_pig.cpp
#pragma once

#include <stdint.h>
#include "../util/time_math.h"

// ESP-IDF exports this from C. Keep the declaration at file scope so inline
// timer code links on both Xtensa targets instead of requesting a C++ symbol.
extern "C" uint32_t esp_random(void);

// Pupil offsets relative to Pancetta's 2 px eye grid. CENTER is the only
// direct-to-viewer pose: the dark dot sits in the middle of the eye white.
enum class PigEyeLook : uint8_t {
    BACK_UP = 0,
    FRONT_UP,
    BACK_DOWN,
    FRONT_DOWN,
    CENTER,
    NONE
};

struct PigFaceTimer {
    // State output — read these after update()
    bool blinking = false;
    bool earTwitching = false;
    PigEyeLook eyeLook = PigEyeLook::CENTER;

    // Internal timers
    uint32_t blinkStart = 0;
    uint32_t lastBlinkTime = 0;
    uint32_t blinkInterval = 4000;
    uint32_t earTwitchStart = 0;
    uint32_t nextEarTwitch = 0;
    uint32_t nextEyeLookAt = 0;
    uint32_t eyeLookDuration = 0;
    uint32_t sensoryGazeUntil = 0;

    static constexpr uint32_t BLINK_DURATION_MS = 120;
    static constexpr uint32_t EAR_TWITCH_DURATION_MS = 80;

    void init(uint32_t now, uint32_t blinkMin = 3000, uint32_t blinkMax = 7000,
              uint32_t earMin = 6000, uint32_t earMax = 12000) {
        blinking = false;
        earTwitching = false;
        blinkStart = 0;
        lastBlinkTime = now;
        blinkInterval = randomRange(blinkMin, blinkMax);
        earTwitchStart = 0;
        nextEarTwitch = now + randomRange(earMin, earMax);
        eyeLook = PigEyeLook::CENTER;
        eyeLookDuration = eyeLookDurationFor(eyeLook);
        nextEyeLookAt = now + eyeLookDuration;
        sensoryGazeUntil = 0;
    }

    // Update blink and ear twitch timers.
    // moodIntensity: -100..100, modulates blink frequency (0 = neutral)
    // suppressEarTwitch: true to block new ear twitches (e.g. during body anim or transition)
    void update(uint32_t now, int moodIntensity = 0, bool suppressEarTwitch = false) {
        // Blink timing — mood intensity adjusts interval
        float blinkMod = 1.0f - (moodIntensity / 200.0f);
        uint32_t minBlink = (uint32_t)(4000 * blinkMod);
        uint32_t maxBlink = (uint32_t)(8000 * blinkMod);

        if (now - lastBlinkTime > blinkInterval) {
            blinking = true;
            blinkStart = now;
            lastBlinkTime = now;
            blinkInterval = randomRange(minBlink, maxBlink);
        }
        if (blinking && (now - blinkStart >= BLINK_DURATION_MS)) {
            blinking = false;
        }

        // A real sensor event gets a short, bounded attention hold. It does
        // not start a new animation system or issue sensor work; callers only
        // feed it published evidence that already exists.
        if (sensoryGazeUntil != 0 && (int32_t)(now - sensoryGazeUntil) >= 0) {
            sensoryGazeUntil = 0;
        }

        // Gaze is its own timeline. It deliberately does not sample music,
        // gait, or bounce phases: those move the body, not the eyeballs.
        if (sensoryGazeUntil == 0 &&
            (nextEyeLookAt == 0 || (int32_t)(now - nextEyeLookAt) >= 0)) {
            PigEyeLook nextLook = pickEyeLook();
            for (uint8_t tries = 0; nextLook == eyeLook && tries < 3; ++tries) {
                nextLook = pickEyeLook();
            }
            if (nextLook == eyeLook) {
                // A guaranteed change keeps a long idle from appearing stuck
                // when the random source happens to repeat the same bucket.
                nextLook = (eyeLook == PigEyeLook::CENTER)
                    ? PigEyeLook::FRONT_DOWN : PigEyeLook::CENTER;
            }
            eyeLook = nextLook;
            eyeLookDuration = eyeLookDurationFor(eyeLook);
            nextEyeLookAt = now + eyeLookDuration;
        }

        // Ear twitch micro-animation
        if (earTwitching) {
            if (now - earTwitchStart >= EAR_TWITCH_DURATION_MS) {
                earTwitching = false;
                nextEarTwitch = now + randomRange(8000, 15001);
            }
        } else if (!suppressEarTwitch &&
                   TimeMath::reachedOrUnset(now, nextEarTwitch)) {
            earTwitching = true;
            earTwitchStart = now;
        }
    }

    // Force an immediate ear twitch (e.g. on dialogue switch)
    void triggerEarTwitch(uint32_t now) {
        if (!earTwitching) {
            earTwitching = true;
            earTwitchStart = now;
        }
    }

    void reset() {
        blinking = false;
        earTwitching = false;
        blinkStart = 0;
        lastBlinkTime = 0;
        blinkInterval = 4000;
        earTwitchStart = 0;
        nextEarTwitch = 0;
        eyeLook = PigEyeLook::CENTER;
        nextEyeLookAt = 0;
        eyeLookDuration = 0;
        sensoryGazeUntil = 0;
    }

    // Let a completed physical observation take Pancetta's attention for a
    // moment. The normal gaze schedule resumes when the hold expires.
    void noticeSensoryEvent(uint32_t now, PigEyeLook look = PigEyeLook::FRONT_UP,
                            uint32_t holdMs = 650) {
        if (look == PigEyeLook::NONE) return;
        eyeLook = look;
        sensoryGazeUntil = now + holdMs;
    }

private:
    // Platform-independent random range (avoids Arduino random() dependency in header)
    static uint32_t randomRange(uint32_t lo, uint32_t hi) {
        if (lo >= hi) return lo;
        return lo + (esp_random() % (hi - lo));
    }

    static PigEyeLook pickEyeLook() {
        // Viewer contact is common enough to feel intentional; upward glances
        // are quick checks, so Pancetta no longer parks at 45 degrees up.
        const uint32_t roll = randomRange(0, 100);
        if (roll < 36) return PigEyeLook::CENTER;
        if (roll < 58) return PigEyeLook::FRONT_DOWN;
        if (roll < 78) return PigEyeLook::BACK_DOWN;
        if (roll < 90) return PigEyeLook::FRONT_UP;
        return PigEyeLook::BACK_UP;
    }

    static uint32_t eyeLookDurationFor(PigEyeLook look) {
        switch (look) {
            case PigEyeLook::CENTER:
                return randomRange(2200, 4401); // direct acknowledgement
            case PigEyeLook::FRONT_DOWN:
            case PigEyeLook::BACK_DOWN:
                return randomRange(900, 2201);  // relaxed side glance
            case PigEyeLook::FRONT_UP:
            case PigEyeLook::BACK_UP:
                return randomRange(450, 1051);  // brief attention check
            case PigEyeLook::NONE:
                return randomRange(900, 1601);
        }
        return 1200;
    }
};
