/**
 * Touch - Gesture classification implementation
 *
 * ==[ FINGER BRAIN ]== reads M5.Touch.getDetail(0), classifies into zones + gestures.
 * One event per frame max. Swipe threshold 40px. Long-press 500ms.
 */

#include "touch.h"
#include "../piglet/avatar.h"
#include "../piglet/pig_scene_common.h"
#include "../ui/display.h"
#include "../ui/ui_measurements.h"
#include <M5Unified.h>

namespace Touch {

// ==[ THRESHOLDS ]==
static constexpr int16_t SWIPE_THRESHOLD = 40;     // px on primary axis
static constexpr uint32_t LONG_PRESS_MS = 500;      // hold time for long-press

// M5Unified reports the Core2 virtual button strip below the LCD. BtnA/B/C own
// that territory, so gesture tracking stops at the display edge.
static constexpr int16_t DISPLAY_BOTTOM = UIMeasurements::kScreenHeight;

// ==[ STATE ]==
static TouchEvent currentEvent = {Gesture::NONE, Zone::NONE, 0, 0, 0, 0, false};
static bool touchedThisFrame = false;

// tracking ongoing touch
static bool tracking = false;
static int16_t startX = 0, startY = 0;
static int16_t lastX = 0, lastY = 0;
static uint32_t startTime = 0;
static bool gestureEmitted = false;

// the idle avatar is the only pig this hitbox knows; every other scene draws
// its own and must not have playfield taps stolen by a stale rect.
static bool pigHitTestEnabled = true;

// ==[ ZONE CLASSIFICATION ]==
static Zone classifyZone(int16_t x, int16_t y) {
    if (y < UIMeasurements::kTopBarH) return Zone::TOP_BAR;
    if (y >= UIMeasurements::kScreenHeight - UIMeasurements::kBottomBarH) return Zone::BOTTOM_BAR;

    // pig area check
    if (pigHitTestEnabled && isTouchingPig(x, y)) return Zone::PIG_AREA;

    return Zone::PLAYFIELD;
}

static int16_t abs16(int16_t v) {
    return (v < 0) ? -v : v;
}

static Gesture classifySwipe(int16_t dx, int16_t dy) {
    int16_t absDx = abs16(dx);
    int16_t absDy = abs16(dy);
    if (absDx > absDy) {
        return (dx > 0) ? Gesture::SWIPE_RIGHT : Gesture::SWIPE_LEFT;
    }
    return (dy > 0) ? Gesture::SWIPE_DOWN : Gesture::SWIPE_UP;
}

static void emitGesture(Gesture gesture, int16_t dx, int16_t dy) {
    currentEvent.gesture = gesture;
    currentEvent.zone = classifyZone(startX, startY);
    currentEvent.x = startX;
    currentEvent.y = startY;
    currentEvent.dx = dx;
    currentEvent.dy = dy;
    currentEvent.active = true;
}

// ==[ PUBLIC API ]==

void update(uint32_t now) {
    // clear previous frame event
    currentEvent.active = false;
    currentEvent.gesture = Gesture::NONE;
    currentEvent.dx = 0;
    currentEvent.dy = 0;
    touchedThisFrame = false;

    auto td = M5.Touch.getDetail(0);

    // display contact only; virtual button strip has its own input path.
    if (td.isPressed() && td.y < DISPLAY_BOTTOM) {
        touchedThisFrame = true;
    }

    if (td.wasPressed()) {
        if (td.y >= DISPLAY_BOTTOM) {
            tracking = false;
            return;
        }
        tracking = true;
        startX = td.x;
        startY = td.y;
        lastX = td.x;
        lastY = td.y;
        startTime = now;
        gestureEmitted = false;
        return;
    }

    // while held: swipe first, then long-press.
    if (tracking && td.isPressed() && !gestureEmitted) {
        lastX = td.x;
        lastY = td.y;
        int16_t dx = lastX - startX;
        int16_t dy = lastY - startY;
        int16_t absDx = abs16(dx);
        int16_t absDy = abs16(dy);
        int32_t dist2 = (int32_t)dx * dx + (int32_t)dy * dy;

        // radio go brrr; release coordinates do not always. emit once on travel.
        if (absDx >= SWIPE_THRESHOLD || absDy >= SWIPE_THRESHOLD) {
            emitGesture(classifySwipe(dx, dy), dx, dy);
            gestureEmitted = true;
            return;
        }

        // long press: held without much movement
        if ((now - startTime) >= LONG_PRESS_MS && dist2 < (SWIPE_THRESHOLD * SWIPE_THRESHOLD)) {
            emitGesture(Gesture::LONG_PRESS, dx, dy);
            gestureEmitted = true;
            return;
        }
    }

    // touch end — classify tap, swipe, or a hold the in-flight path skipped
    if (tracking && td.wasReleased()) {
        tracking = false;

        if (gestureEmitted) return;  // already fired while held

        int16_t dx = lastX - startX;
        int16_t dy = lastY - startY;
        int16_t absDx = abs16(dx);
        int16_t absDy = abs16(dy);

        Gesture g = Gesture::NONE;

        if (absDx >= SWIPE_THRESHOLD || absDy >= SWIPE_THRESHOLD) {
            // swipe — dominant axis wins
            g = classifySwipe(dx, dy);
        } else if ((now - startTime) >= LONG_PRESS_MS) {
            // A hold that drifted past the long-press radius but never crossed
            // the swipe threshold reaches release with nothing emitted. It was
            // still a hold — reporting TAP here turned "exit hunt" into
            // "pause hunt" and "spin the pig" into "pet the pig".
            g = Gesture::LONG_PRESS;
        } else {
            // short movement + short hold = tap
            g = Gesture::TAP;
        }

        emitGesture(g, dx, dy);
    }
}

void reset() {
    // Leave currentEvent alone: the dispatcher may be mid-switch on it, and
    // clearing it here would yank the reference out from under the caller.
    tracking = false;
    gestureEmitted = false;
}

const TouchEvent& getEvent() {
    return currentEvent;
}

bool hasEvent() {
    return currentEvent.active && currentEvent.gesture != Gesture::NONE;
}

void setPigHitTestEnabled(bool enabled) {
    pigHitTestEnabled = enabled;
}

bool isTouchingPig(int16_t tx, int16_t ty) {
    int pigX = Avatar::getCurrentX();
    // IDLE avatar hitbox. Room/MenuPig uses its own interaction model, so the
    // body budget comes from the idle pig's own header — not the room layout.
    constexpr int16_t PAD = 10;
    constexpr int16_t PIG_W = PIG_BODY_W_CONST;
    constexpr int16_t PIG_H = PIG_BODY_H_CONST;
    constexpr int16_t PIG_TOP = PIG_Y_CONST;

    return tx >= (pigX - PAD) && tx <= (pigX + PIG_W + PAD) &&
           ty >= (PIG_TOP - PAD) && ty <= (PIG_TOP + PIG_H + PAD);
}

bool wasTouched() {
    return touchedThisFrame;
}

}  // namespace Touch
