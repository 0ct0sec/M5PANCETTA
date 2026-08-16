/**
 * PinEntry — 4-char ABC button PIN entry widget
 *
 * ==[ PIN GATE ]== shoulder-surf deterrent. 3^4 = 81 combos.
 * shared by loot_menu (gate) and settings_menu (config).
 */
#pragma once

#include <M5Unified.h>

namespace PinEntry {
    void begin();                              // reset slots, cursor=0, active=true
    void handleButton(char btn);               // 'A'/'B'/'C' → fill slot, advance
    bool isComplete();                         // all 4 slots filled (check after handleButton)
    void getCode(char* out);                   // copy 4-char code + null terminator
    void setWrong();                           // trigger wrong-flash + haptic
    bool isWrongFlash();                       // in error display state
    void draw(M5Canvas* c, const char* title, const char* note = nullptr); // render PIN entry overlay
    void update();                             // tick wrong-flash timeout
    bool isActive();                           // entry in progress
    void cancel();                             // abort, deactivate
}
