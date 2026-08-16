/**
 * Touch - gesture classifier for the shared 320×240 capacitive touchscreen
 *
 * ==[ FINGER BRAIN ]== thin layer on M5Unified touch_detail_t.
 * Classifies tap, long-press, swipe into zones.
 * Touch complements the three virtual button zones; it does not replace them.
 */

#ifndef TOUCH_H
#define TOUCH_H

#include <stdint.h>

namespace Touch {

enum class Zone : uint8_t {
    NONE,
    TOP_BAR,
    PLAYFIELD,
    BOTTOM_BAR,
    PIG_AREA
};

enum class Gesture : uint8_t {
    NONE,
    TAP,
    LONG_PRESS,
    SWIPE_LEFT,
    SWIPE_RIGHT,
    SWIPE_UP,
    SWIPE_DOWN
};

struct TouchEvent {
    Gesture gesture;
    Zone zone;
    int16_t x, y;       // touch coordinates at gesture start
    int16_t dx, dy;     // movement from gesture start to trigger/release
    bool active;         // true = fresh event this frame
};

// call after M5.update() every frame
void update(uint32_t now);

// drop an in-flight touch without emitting anything (mode changes).
// the current frame's event survives; only the tracker is cleared, so the
// finger that started the gesture cannot deliver a release into a new mode.
void reset();

// get latest classified event (valid for one frame)
const TouchEvent& getEvent();

// shorthand: event.active && event.gesture != NONE
bool hasEvent();

// only the scene that actually renders the idle avatar may claim PIG_AREA;
// elsewhere the pig rect is a phantom that would swallow playfield taps.
void setPigHitTestEnabled(bool enabled);

// pig hitbox test (uses Avatar::getCurrentX + body dims)
bool isTouchingPig(int16_t tx, int16_t ty);

// any display-area touch contact this frame (for dim reset)
bool wasTouched();

}  // namespace Touch

#endif  // TOUCH_H
