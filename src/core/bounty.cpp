/**
 * Bounty - Implementation
 */

#include "bounty.h"

#include <stdio.h>

#include "config.h"
#include "../audio/sfx.h"
#include "../haptic/haptic.h"
#include "../hal/hal_interface.h"
#include "../piglet/mood.h"

namespace Bounty {

static constexpr const char* NS = "sirloin";
static constexpr const char* KEY_SLOT = "bntSlot";
static constexpr const char* KEY_HITS = "bntHits";
static constexpr const char* KEY_PAID = "bntPaid";

static HAL* _hal = nullptr;
static uint32_t currentSlot = 0;
static uint8_t windowHits = 0;
static bool windowPaid = false;
static bool dirty = false;
static bool initialized = false;

static uint32_t slotNow() {
    uint32_t epoch = Config::getTrustedEpoch();
    return epoch ? (epoch / WINDOW_S) : 0;
}

void init(HAL* hal) {
    if (initialized) return;
    initialized = true;
    _hal = hal ? hal : HalGlobal::get();
    if (!_hal) return;
    currentSlot = _hal->storageGetUInt(NS, KEY_SLOT, 0);
    windowHits = (uint8_t)_hal->storageGetUInt(NS, KEY_HITS, 0);
    windowPaid = _hal->storageGetUInt(NS, KEY_PAID, 0) != 0;
    if (windowHits > TARGET) windowHits = TARGET;
    dirty = false;
}

void save() {
    if (!_hal || !dirty) return;
    _hal->storagePutUInt(NS, KEY_SLOT, currentSlot);
    _hal->storagePutUInt(NS, KEY_HITS, windowHits);
    _hal->storagePutUInt(NS, KEY_PAID, windowPaid ? 1 : 0);
    dirty = false;
}

bool isArmed() { return slotNow() != 0; }

uint8_t hits() { return windowHits; }

bool isPaid() { return windowPaid; }

uint32_t secondsRemaining() {
    uint32_t epoch = Config::getTrustedEpoch();
    if (!epoch) return 0;
    return WINDOW_S - (epoch % WINDOW_S);
}

bool statusText(char* out, size_t len) {
    if (!out || !len) return false;
    // A settled window has nothing left to chase. Checked before the clock so a
    // paid window costs nothing on the status-bar path.
    if (windowPaid) return false;

    // One clock read, not two: this runs on every status-bar frame, and
    // getTrustedEpoch() falls through to an I2C RTC read when the system clock
    // is cold. Dividing by WINDOW_S is exactly the isArmed() test.
    uint32_t epoch = Config::getTrustedEpoch();
    if (epoch / WINDOW_S == 0) return false;

    snprintf(out, len, "B%u/%u %02uh", (unsigned)windowHits, (unsigned)TARGET,
             (unsigned)((WINDOW_S - (epoch % WINDOW_S)) / 3600u));
    return true;
}

bool onCapture() {
    uint32_t slot = slotNow();
    // No trusted clock, no window. A bounty on an invisible deadline is worse
    // than no bounty: the operator cannot tell effort from luck.
    if (slot == 0) return false;

    if (slot != currentSlot) {
        currentSlot = slot;
        windowHits = 0;
        windowPaid = false;
        dirty = true;
    }

    if (windowHits < TARGET) {
        ++windowHits;
        dirty = true;
    }

    if (windowHits < TARGET || windowPaid) {
        save();
        return false;
    }

    windowPaid = true;
    dirty = true;
    save();

    // Same session contract as every other lane. The ceremony still fires so
    // the window reads as met; only the XP waits for a live session.
    if (Config::isSessionActive()) {
        Config::addXP(REWARD_XP, Config::RewardSource::GOAL);
    }
    // Queued, not set: the capture that closed the window has just put its own
    // line on screen and deserves to be read before the bounty lands.
    Mood::queuePhrase("B0UNTY CL34R3D. two in six hours.",
                      AvatarState::EXCITED);
    SFX::play(SFX::GOAL_COMPLETE);
    Haptic::pulse();
    return true;
}

}  // namespace Bounty
