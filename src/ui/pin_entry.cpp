/**
 * PinEntry — 4-box ABC PIN entry widget
 *
 * ==[ PIN GATE ]== A/B/C buttons fill 4 slots sequentially.
 * auto-signals completion after 4th press. caller validates.
 */

#include "pin_entry.h"
#include "display.h"
#include "../haptic/haptic.h"

namespace PinEntry {

static char slots[4];
static uint8_t cursor;
static bool active;
static bool complete;
static bool wrongFlash;
static uint32_t wrongTime;

static const uint32_t WRONG_FLASH_MS = 800;

void begin() {
    memset(slots, 0, sizeof(slots));
    cursor = 0;
    active = true;
    complete = false;
    wrongFlash = false;
}

void handleButton(char btn) {
    if (!active || complete || wrongFlash) return;
    if (btn != 'A' && btn != 'B' && btn != 'C') return;
    if (cursor >= 4) return;   // defense in depth — slots has exactly 4 entries

    slots[cursor] = btn;
    cursor++;
    Haptic::tick();

    if (cursor >= 4) {
        complete = true;
    }
}

bool isComplete() { return complete; }

void getCode(char* out) {
    memcpy(out, slots, 4);
    out[4] = '\0';
}

void setWrong() {
    wrongFlash = true;
    wrongTime = millis();
    Haptic::buzz();
    // reset for retry
    memset(slots, 0, sizeof(slots));
    cursor = 0;
    complete = false;
}

void update() {
    if (wrongFlash && (millis() - wrongTime > WRONG_FLASH_MS)) {
        wrongFlash = false;
    }
}

bool isWrongFlash() { return wrongFlash; }

bool isActive() { return active; }

void cancel() {
    active = false;
    complete = false;
    wrongFlash = false;
}

// ==[ DRAW ]== 4 boxes centered on screen

void draw(M5Canvas* c, const char* title, const char* note) {
    uint16_t fg = Display::getColorFG();
    uint16_t bg = Display::getColorBG();

    // title
    c->setTextDatum(TC_DATUM);
    c->setTextSize(2);
    c->setTextColor(fg);
    c->drawString(title, SCREEN_WIDTH / 2, TOP_BAR_H + 2);
    c->drawLine(60, TOP_BAR_H + 18, SCREEN_WIDTH - 60, TOP_BAR_H + 18, fg);

    if (note && note[0]) {
        c->setTextSize(1);
        c->setTextDatum(MC_DATUM);
        c->drawString(note, SCREEN_WIDTH / 2, TOP_BAR_H + 34);
    }

    // boxes: 4 × 28px wide, 22px tall, 8px gap
    static const int BOX_W = 28;
    static const int BOX_H = 22;
    static const int GAP = 8;
    static const int TOTAL_W = BOX_W * 4 + GAP * 3;  // 140px
    int startX = (SCREEN_WIDTH - TOTAL_W) / 2;
    int boxY = SCREEN_HEIGHT / 2 - BOX_H / 2 - 6;

    c->setTextSize(2);
    c->setTextDatum(MC_DATUM);

    for (int i = 0; i < 4; i++) {
        int bx = startX + i * (BOX_W + GAP);
        bool isCursor = (i == cursor && !complete && !wrongFlash);
        bool filled = (slots[i] != 0);

        // box background
        if (isCursor) {
            c->fillRect(bx, boxY, BOX_W, BOX_H, fg);
            c->setTextColor(bg);
        } else {
            c->drawRect(bx, boxY, BOX_W, BOX_H, fg);
            c->setTextColor(fg);
        }

        // slot content
        char label[2] = { filled ? slots[i] : '_', '\0' };
        c->drawString(label, bx + BOX_W / 2, boxY + BOX_H / 2);
    }

    // wrong flash message
    if (wrongFlash) {
        c->setTextColor(fg);
        c->setTextDatum(MC_DATUM);
        c->setTextSize(2);
        c->drawString("WR0NG", SCREEN_WIDTH / 2, boxY + BOX_H + 20);
    }

    // bottom bar
    int bottomY = SCREEN_HEIGHT - BOTTOM_BAR_H;
    c->fillRect(0, bottomY, SCREEN_WIDTH, BOTTOM_BAR_H, fg);
    c->setTextColor(bg);
    c->setTextSize(1);
    c->setTextDatum(ML_DATUM);
    c->drawString("[A]  [B]  [C]", 4, bottomY + BOTTOM_BAR_H / 2);
    c->setTextDatum(MR_DATUM);
    c->drawString("[C+]BACK", SCREEN_WIDTH - 4, bottomY + BOTTOM_BAR_H / 2);
    c->setTextDatum(TL_DATUM);
}

} // namespace PinEntry
