#pragma once

#include <stdint.h>

// Hardware-independent cadence state for the bath microphone.  It only sees
// onset decisions; sample capture and energy thresholds remain in bath_mic.
namespace BathBeat {

class Tracker {
public:
    static constexpr uint16_t kMinPeriodMs = 280u;
    static constexpr uint16_t kMaxPeriodMs = 1200u;
    static constexpr uint16_t kDefaultPeriodMs = 520u;
    static constexpr uint32_t kMinimumHoldMs = 1500u;
    static constexpr uint8_t kHeldBeatPeriods = 3u;
    static constexpr uint8_t kBeatsToConfirm = 2u;

    void reset() {
        dancing_ = false;
        haveOnset_ = false;
        cadenceBeats_ = 0u;
        beatAnchorMs_ = 0u;
        beatPeriodMs_ = kDefaultPeriodMs;
        lastOnsetMs_ = 0u;
        cadenceDeadlineMs_ = 0u;
    }

    // Call this only when a completed microphone window has been evaluated.
    // A quiet 20 ms window is normal between kicks, so it cannot end a dance
    // on its own.  Expiry is checked against the learned cadence instead.
    void observe(bool onset, uint32_t now) {
        if (onset) observeOnset(now);

        if (dancing_ && deadlineReached(now, cadenceDeadlineMs_)) {
            dancing_ = false;
            cadenceBeats_ = 0u;
            haveOnset_ = false;
            cadenceDeadlineMs_ = 0u;
        }
    }

    bool isActive() const { return dancing_; }

    uint8_t phase(uint32_t now) const {
        if (!dancing_) return 0u;
        const uint32_t elapsed = now - beatAnchorMs_;
        return static_cast<uint8_t>(((elapsed % beatPeriodMs_) * 8u) /
                                    beatPeriodMs_);
    }

    uint16_t periodMs() const { return beatPeriodMs_; }

private:
    static bool deadlineReached(uint32_t now, uint32_t deadline) {
        return static_cast<int32_t>(now - deadline) >= 0;
    }

    void startCandidate(uint32_t now) {
        haveOnset_ = true;
        cadenceBeats_ = 1u;
        lastOnsetMs_ = now;
        beatAnchorMs_ = now;
    }

    void lockBeat(uint32_t now, bool establishTempo) {
        const uint32_t elapsed = now - beatAnchorMs_;
        if (establishTempo) {
            // The first plausible pair is the strongest tempo measurement we
            // have.  Starting from the old default made slow songs hop early.
            beatPeriodMs_ = static_cast<uint16_t>(elapsed);
        } else {
            // Later accepted peaks correct drift without letting one clap
            // make Pancetta rush ahead of the room.
            beatPeriodMs_ = static_cast<uint16_t>(
                ((uint32_t)beatPeriodMs_ * 3u + elapsed) / 4u);
        }
        beatAnchorMs_ = now;
    }

    void extendCadence(uint32_t now) {
        const uint32_t tempoHold =
            (uint32_t)beatPeriodMs_ * kHeldBeatPeriods;
        const uint32_t hold = tempoHold > kMinimumHoldMs
            ? tempoHold : kMinimumHoldMs;
        cadenceDeadlineMs_ = now + hold;
    }

    void observeOnset(uint32_t now) {
        if (!haveOnset_) {
            startCandidate(now);
            return;
        }

        const uint32_t interval = now - lastOnsetMs_;
        if (interval < kMinPeriodMs) {
            // Adjacent sample windows can rise around the same kick.  They
            // are one beat, not permission to pin the animation at phase 0.
            return;
        }

        if (interval > kMaxPeriodMs) {
            // A measured gap this large is a genuine loss of the established
            // cadence.  Keep the peak as a fresh candidate, but do not carry
            // the old dance into an unrelated sound.
            dancing_ = false;
            cadenceDeadlineMs_ = 0u;
            startCandidate(now);
            return;
        }

        const bool establishTempo = cadenceBeats_ == 1u;
        lastOnsetMs_ = now;
        if (cadenceBeats_ < 255u) ++cadenceBeats_;
        lockBeat(now, establishTempo);
        if (cadenceBeats_ >= kBeatsToConfirm) {
            dancing_ = true;
            extendCadence(now);
        }
    }

    bool dancing_ = false;
    bool haveOnset_ = false;
    uint8_t cadenceBeats_ = 0u;
    uint32_t beatAnchorMs_ = 0u;
    uint16_t beatPeriodMs_ = kDefaultPeriodMs;
    uint32_t lastOnsetMs_ = 0u;
    uint32_t cadenceDeadlineMs_ = 0u;
};

}  // namespace BathBeat
