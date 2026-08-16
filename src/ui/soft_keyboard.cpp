// Touchscreen soft keyboard for the shared 320x240 panel.
// Self-contained input — reads M5.Touch directly, no external input system.
// Three pages: lowercase → uppercase → symbols.
//
// A Unit CardKB on Grove Port A types into the same field. The on-screen
// layout stays live while it does, so a finger and the keypad can share one
// entry without either being told the other exists.

#include "soft_keyboard.h"
#include "../input/cardkb.h"
#include "display.h"

#include <ctype.h>
#include <string.h>

bool SoftKeyboard::active = false;
bool SoftKeyboard::done = false;
bool SoftKeyboard::accepted = false;
int SoftKeyboard::page = 0;
const char* SoftKeyboard::title = nullptr;
char* SoftKeyboard::targetBuf = nullptr;
size_t SoftKeyboard::targetCap = 0;
size_t SoftKeyboard::maxLen = 0;

// ==[ TOUCH STATE ]== self-contained tap/swipe detection
static bool touchActive = false;
static int16_t touchStartX = 0;
static int16_t touchStartY = 0;
static uint32_t touchStartMs = 0;

static constexpr uint32_t kTapMaxMs = 350;
static constexpr int16_t kTapMovePx = 10;
static constexpr int16_t kSwipeMinPx = 60;

static size_t safeStrlen(const char* s, size_t cap) {
    if (!s || cap == 0) return 0;
    size_t n = 0;
    while (n < cap && s[n] != '\0') n++;
    return n;
}

void SoftKeyboard::start(const char* t, char* buf, size_t bufCap, size_t maxLen_) {
    title = t;
    targetBuf = buf;
    targetCap = bufCap;
    maxLen = maxLen_;
    page = 0;
    done = false;
    accepted = false;
    touchActive = false;
    active = (targetBuf != nullptr && targetCap > 1);
    if (active) {
        targetBuf[targetCap - 1] = '\0';
    }
}

void SoftKeyboard::stop() {
    active = false;
    done = false;
    accepted = false;
    page = 0;
    title = nullptr;
    targetBuf = nullptr;
    targetCap = 0;
    maxLen = 0;
    touchActive = false;
}

bool SoftKeyboard::isActive() {
    return active;
}

bool SoftKeyboard::consumeDone(bool& outAccepted) {
    if (!done) return false;
    outAccepted = accepted;
    done = false;
    active = false;
    return true;
}

void SoftKeyboard::appendChar(char c) {
    if (!targetBuf || targetCap == 0) return;
    size_t len = safeStrlen(targetBuf, targetCap - 1);
    if (len >= maxLen) return;
    if (len + 1 >= targetCap) return;
    targetBuf[len] = c;
    targetBuf[len + 1] = '\0';
}

void SoftKeyboard::backspace() {
    if (!targetBuf || targetCap == 0) return;
    size_t len = safeStrlen(targetBuf, targetCap - 1);
    if (len == 0) return;
    targetBuf[len - 1] = '\0';
}

void SoftKeyboard::clear() {
    if (!targetBuf || targetCap == 0) return;
    targetBuf[0] = '\0';
}

static bool pointInRect(int16_t px, int16_t py, int x, int y, int w, int h) {
    return (px >= x && px < (x + w) && py >= y && py < (y + h));
}

// ==[ PAGE-DEPENDENT ROWS ]==
// Letters (pages 0-1): row0=numbers, rows 1-3=letters (+underscore)
// Symbols (page 2): all rows are special characters
static const char* kLetRow0 = "1234567890";
static const char* kLetRow1 = "QWERTYUIOP";
static const char* kLetRow2 = "ASDFGHJKL";
static const char* kLetRow3 = "ZXCVBNM_";
static const char* kSymRow0 = "!@#$%^&*()";
static const char* kSymRow1 = "-_+={}[]|\\";
static const char* kSymRow2 = ":;\"'<>,.?";
static const char* kSymRow3 = "/~`";

// ==[ TAP HANDLER ]== process a tap at screen coordinates
void SoftKeyboard::processTap(int16_t x, int16_t y) {
    if (y >= SCREEN_HEIGHT || y < 0) return;

    const int pad = 6;
    const int headerH = 40;
    const int keyAreaY = headerH;
    const int keyAreaH = SCREEN_HEIGHT - headerH;
    if (keyAreaH <= 0) return;

    const int rowGap = 3;
    const int colGap = 3;
    const int rows = 5;
    const int rowH = (keyAreaH - (rowGap * (rows - 1))) / rows;
    if (rowH < 20) return;

    const int keyW = (SCREEN_WIDTH - 2 * pad - colGap * 9) / 10;

    const bool sym = (page == 2);
    const char* row0 = sym ? kSymRow0 : kLetRow0;
    const char* row1 = sym ? kSymRow1 : kLetRow1;
    const char* row2 = sym ? kSymRow2 : kLetRow2;
    const char* row3 = sym ? kSymRow3 : kLetRow3;

    auto handleCharRow = [&](const char* letters, int rowIdx) -> bool {
        const int len = (int)strlen(letters);
        if (len <= 0) return false;
        const int y0 = keyAreaY + rowIdx * (rowH + rowGap);
        const int totalW = len * keyW + (len - 1) * colGap;
        const int leftPad = (SCREEN_WIDTH - totalW) / 2;
        for (int i = 0; i < len; i++) {
            int kx = leftPad + i * (keyW + colGap);
            if (pointInRect(x, y, kx, y0, keyW, rowH)) {
                char c = letters[i];
                // lowercase on page 0, letter rows only
                if (page == 0 && rowIdx > 0) c = (char)tolower((unsigned char)c);
                appendChar(c);
                return true;
            }
        }
        return false;
    };

    if (handleCharRow(row0, 0)) return;
    if (handleCharRow(row1, 1)) return;
    if (handleCharRow(row2, 2)) return;
    if (handleCharRow(row3, 3)) return;

    // ==[ COMMAND ROW ]== PAGE | BKSP | SPACE | OK | CANCEL
    const int cmdRowIdx = 4;
    const int y0 = keyAreaY + cmdRowIdx * (rowH + rowGap);
    const char* pageLabel = (page == 0) ? "ABC" : (page == 1) ? "!@#" : "abc";
    struct CmdKey { const char* label; int w; };
    CmdKey keys[] = {
        {pageLabel, 2},
        {"BKSP",    2},
        {"SPACE",   4},
        {"OK",      2},
        {"CANCEL",  2},
    };

    int units = 0;
    for (auto& k : keys) units += k.w;
    const int unitW = (SCREEN_WIDTH - (pad * 2) - (colGap * ((int)(sizeof(keys)/sizeof(keys[0])) - 1))) / units;
    int curX = pad;
    for (size_t i = 0; i < sizeof(keys)/sizeof(keys[0]); i++) {
        int kw = keys[i].w * unitW;
        if (pointInRect(x, y, curX, y0, kw, rowH)) {
            if (i == 0) {
                page = (page + 1) % 3;
            } else if (strcmp(keys[i].label, "BKSP") == 0) {
                backspace();
            } else if (strcmp(keys[i].label, "SPACE") == 0) {
                appendChar(' ');
            } else if (strcmp(keys[i].label, "OK") == 0) {
                accepted = true;
                done = true;
            } else if (strcmp(keys[i].label, "CANCEL") == 0) {
                accepted = false;
                done = true;
            }
            return;
        }
        curX += kw + colGap;
    }
}

// ==[ PHYSICAL KEY ]== the unit resolves Shift/Sym/Caps itself, so a character
// key arrives finished and no modifier state is tracked here. Cursor bytes are
// dropped: this field is append-only, it has no caret to move.
void SoftKeyboard::injectKey(uint8_t key) {
    if (!active || done) return;
    switch (key) {
        case CardKB::KEY_ENTER:
            accepted = true;
            done = true;
            break;
        case CardKB::KEY_ESC:
            accepted = false;
            done = true;
            break;
        case CardKB::KEY_BACKSPACE:
            backspace();
            break;
        case CardKB::KEY_TAB:
            // Keeps the drawn layout in step with a keypad user who reaches for
            // a symbol page out of habit.
            page = (page + 1) % 3;
            break;
        default:
            if (CardKB::isPrintable(key)) appendChar((char)key);
            break;
    }
}

void SoftKeyboard::update() {
    if (!active) return;

    // Keypad first: a physical Enter must close the field before the touch pass
    // can queue another tap into a buffer that is already handed back.
    while (CardKB::available()) {
        injectKey(CardKB::read());
        if (done) return;
    }

    auto td = M5.Touch.getDetail(0);

    if (td.wasPressed()) {
        touchActive = true;
        touchStartX = td.x;
        touchStartY = td.y;
        touchStartMs = millis();
    }

    if (td.wasReleased() && touchActive) {
        touchActive = false;
        uint32_t dt = millis() - touchStartMs;
        int16_t dx = td.x - touchStartX;
        int16_t dy = td.y - touchStartY;
        int16_t dist = abs(dx) + abs(dy);

        if (dt < kTapMaxMs && dist < kTapMovePx) {
            processTap(touchStartX, touchStartY);
        } else if (dx < -kSwipeMinPx) {
            accepted = false;
            done = true;
        }
    }
}

void SoftKeyboard::draw(M5Canvas& canvas) {
    if (!active) return;

    const uint16_t fg = Display::getColorFG();
    const uint16_t bg = Display::getColorBG();

    canvas.fillSprite(bg);
    canvas.setTextColor(fg);
    canvas.setFont(&fonts::Font0);

    // Header — title
    canvas.setTextDatum(TL_DATUM);
    canvas.setTextSize(2);
    if (title && title[0]) {
        canvas.drawString(title, 6, 2);
    } else {
        canvas.drawString("INPUT", 6, 2);
    }

    // Input value preview — always plaintext
    canvas.setTextSize(2);
    char viewBuf[28];
    viewBuf[0] = '\0';
    if (targetBuf) {
        size_t len = safeStrlen(targetBuf, targetCap - 1);
        const char* src = targetBuf;
        if (len > 24) src = targetBuf + (len - 24);
        strncpy(viewBuf, src, sizeof(viewBuf) - 1);
        viewBuf[sizeof(viewBuf) - 1] = '\0';
    }
    canvas.drawRect(4, 20, SCREEN_WIDTH - 8, 20, fg);
    canvas.setTextDatum(TL_DATUM);
    canvas.drawString(viewBuf, 8, 22);

    // Key area
    const int pad = 6;
    const int headerH = 40;
    const int keyAreaY = headerH;
    const int keyAreaH = SCREEN_HEIGHT - headerH;
    const int rowGap = 3;
    const int colGap = 3;
    const int rows = 5;
    const int rowH = (keyAreaH - (rowGap * (rows - 1))) / rows;

    const int keyW = (SCREEN_WIDTH - 2 * pad - colGap * 9) / 10;

    const bool sym = (page == 2);
    const char* row0 = sym ? kSymRow0 : kLetRow0;
    const char* row1 = sym ? kSymRow1 : kLetRow1;
    const char* row2 = sym ? kSymRow2 : kLetRow2;
    const char* row3 = sym ? kSymRow3 : kLetRow3;

    auto drawCharRow = [&](const char* letters, int rowIdx) {
        const int len = (int)strlen(letters);
        const int y0 = keyAreaY + rowIdx * (rowH + rowGap);
        const int totalW = len * keyW + (len - 1) * colGap;
        const int leftPad = (SCREEN_WIDTH - totalW) / 2;
        canvas.setTextSize(2);
        canvas.setTextDatum(MC_DATUM);
        for (int i = 0; i < len; i++) {
            int x0 = leftPad + i * (keyW + colGap);
            canvas.drawRect(x0, y0, keyW, rowH, fg);
            char c = letters[i];
            if (page == 0 && rowIdx > 0) c = (char)tolower((unsigned char)c);
            char s[2] = {c, 0};
            canvas.drawString(s, x0 + keyW / 2, y0 + rowH / 2);
        }
    };

    drawCharRow(row0, 0);
    drawCharRow(row1, 1);
    drawCharRow(row2, 2);
    drawCharRow(row3, 3);

    // Command row
    const char* pageLabel = (page == 0) ? "ABC" : (page == 1) ? "!@#" : "abc";
    const int y0 = keyAreaY + 4 * (rowH + rowGap);
    struct CmdKey { const char* label; int w; };
    CmdKey keys[] = {
        {pageLabel, 2},
        {"BKSP",    2},
        {"SPACE",   4},
        {"OK",      2},
        {"CANCEL",  2},
    };
    int units = 0;
    for (auto& k : keys) units += k.w;
    const int unitW = (SCREEN_WIDTH - (pad * 2) - (colGap * ((int)(sizeof(keys)/sizeof(keys[0])) - 1))) / units;
    int curX = pad;
    canvas.setTextSize(1);
    canvas.setTextDatum(MC_DATUM);
    for (size_t i = 0; i < sizeof(keys)/sizeof(keys[0]); i++) {
        int kw = keys[i].w * unitW;
        uint16_t fill = bg;
        uint16_t text = fg;
        // highlight page key when not on default (lowercase) page
        if (i == 0 && page != 0) {
            fill = fg;
            text = bg;
        }
        canvas.fillRect(curX, y0, kw, rowH, fill);
        canvas.drawRect(curX, y0, kw, rowH, fg);
        canvas.setTextColor(text, fill);
        canvas.drawString(keys[i].label, curX + kw / 2, y0 + rowH / 2);
        canvas.setTextColor(fg, bg);
        curX += kw + colGap;
    }
}
