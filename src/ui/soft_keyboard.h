// Touchscreen soft keyboard for the shared 320x240 panel.
// Self-contained touch — no external input system dependency.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <M5Unified.h>

class SoftKeyboard {
public:
    static void start(const char* title, char* buf, size_t bufCap, size_t maxLen);
    static void stop();
    static bool isActive();

    // Returns true once when finished (accepted or canceled).
    // If accepted, the edited text is already written back to the target buffer.
    static bool consumeDone(bool& accepted);

    static void update();          // Self-contained input: reads M5.Touch and CardKB directly.
    static void draw(M5Canvas& canvas);

private:
    // A physical CardKB byte, already resolved through Shift/Sym by the unit.
    static void injectKey(uint8_t key);

    static bool active;
    static bool done;
    static bool accepted;
    static int page;  // 0=lower, 1=upper, 2=sym

    static const char* title;
    static char* targetBuf;
    static size_t targetCap;
    static size_t maxLen;

    static void appendChar(char c);
    static void backspace();
    static void clear();
    static void processTap(int16_t x, int16_t y);
};
