/**
 * TouchHints - Gesture discovery for the shared capacitive touchscreen
 *
 * ==[ FINGER SCHOOL ]== pig walks up, tells you what taps do.
 * Blinking bottom bar labels + speech bubble phrases.
 * Compiles against the target capability gate, not a board name.
 */

#ifndef TOUCH_HINTS_H
#define TOUCH_HINTS_H

#include "../hal/platform.h"

// Both targets have FT6336U capacitive touch; gate on the capability
// rather than the Core2 board macro.
#if HAMLET_HAS_TOUCH

#include <stdint.h>
#include "../../src/hamlet.h"

namespace TouchHints {

// call on mode transition — triggers phrase + bar hint if due
void onModeEnter(HamletMode mode);

// call every frame from main loop
void update(uint32_t now);

// true during 4s hint window
bool isHintActive();

// blinking bar text for current mode (null if inactive)
const char* getHintBar();

// NVS load/save
void loadSeenMask();
void saveSeenMask();

}  // namespace TouchHints

#else  // target without touch — stub everything out

namespace TouchHints {
inline void onModeEnter(HamletMode) {}
inline void update(uint32_t) {}
inline bool isHintActive() { return false; }
inline const char* getHintBar() { return nullptr; }
inline void loadSeenMask() {}
inline void saveSeenMask() {}
}

#endif  // HAMLET_HAS_TOUCH

#endif  // TOUCH_HINTS_H
