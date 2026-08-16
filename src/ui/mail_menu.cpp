/**
 * P1G P0ST - Implementation
 *
 * ==[ THE INBOX ]== filed case letters, newest at the bottom of the stack.
 * The list is chrome; the case card belongs to NpcEvents, which owns every
 * beat of the decision tree once a file is open.
 */

#include "mail_menu.h"
#include "display.h"
#include "frame_presenter.h"
#include "ui_measurements.h"
#include "npc/npc_events.h"
#include "npc/npc_events_core.h"
#include "../core/mailbox.h"
#include "../haptic/haptic.h"
#include "../audio/sfx.h"
#include <M5Unified.h>

namespace MailMenu {

using namespace UIMeasurements;

static constexpr int kRowH = 26;
static constexpr int kListTop = TOP_BAR_H + 20;
static constexpr int kVisibleRows = 6;

static int currentIdx = 0;
static int scrollOffset = 0;
static M5Canvas* canvas = nullptr;

static void draw();

static void clampSelection() {
    uint8_t total = Mailbox::count();
    if (total == 0) {
        currentIdx = 0;
        scrollOffset = 0;
        return;
    }
    if (currentIdx >= total) currentIdx = total - 1;
    if (currentIdx < 0) currentIdx = 0;
    if (currentIdx < scrollOffset) scrollOffset = currentIdx;
    if (currentIdx >= scrollOffset + kVisibleRows) {
        scrollOffset = currentIdx - kVisibleRows + 1;
    }
    if (scrollOffset < 0) scrollOffset = 0;
}

void enter() {
    currentIdx = 0;
    scrollOffset = 0;
    canvas = Display::getSharedCanvas();
    clampSelection();
    draw();
}

bool hasOpenCase() {
    return NpcEvents::isActive();
}

void update() {
    draw();
}

void next() {
    if (hasOpenCase()) {
        NpcEvents::nextChoice();
        return;
    }
    if (Mailbox::count() == 0) return;
    currentIdx = (currentIdx + 1) % Mailbox::count();
    if (currentIdx == 0) scrollOffset = 0;
    clampSelection();
    SFX::click();
    Haptic::tick();
}

void prev() {
    if (hasOpenCase()) {
        NpcEvents::prevChoice();
        return;
    }
    uint8_t total = Mailbox::count();
    if (total == 0) return;
    currentIdx = (currentIdx == 0) ? total - 1 : currentIdx - 1;
    clampSelection();
    SFX::click();
    Haptic::tick();
}

void select() {
    if (hasOpenCase()) {
        NpcEvents::select();
        return;
    }
    if (Mailbox::count() == 0) return;
    if (!NpcEvents::openFromMail((uint8_t)currentIdx)) {
        // The letter no longer resolves to a case file. Bin it rather than
        // leaving a row that cannot be opened.
        Mailbox::discard((uint8_t)currentIdx);
        Mailbox::save();
        clampSelection();
    }
}

void back() {
    if (hasOpenCase()) {
        NpcEvents::cancel();
        clampSelection();
    }
}

// ==[ DRAW ]==

static const char* statusFor(const Mailbox::Letter& letter) {
    if (letter.flags & Mailbox::FLAG_OPEN) return "0P3N";
    if (letter.flags & Mailbox::FLAG_READ) return "R34D";
    return "N3W";
}

static void drawEmptyBox(uint16_t dim) {
    canvas->setTextDatum(MC_DATUM);
    canvas->setTextSize(2);
    canvas->setTextColor(dim);
    canvas->drawString("N0 M41L", SCREEN_WIDTH / 2, 96);
    canvas->setTextSize(1);
    canvas->setTextColor(dim);
    canvas->drawString("nobody filed. nobody called.",
                       SCREEN_WIDTH / 2, 122);
    canvas->drawString("walk the rooms and the wire finds you.",
                       SCREEN_WIDTH / 2, 136);
    canvas->setTextDatum(TL_DATUM);
}

static void drawList(uint16_t fg, uint16_t bg, uint16_t dim) {
    uint8_t total = Mailbox::count();

    canvas->setTextDatum(TL_DATUM);
    canvas->setTextSize(1);
    canvas->setTextColor(dim);
    char header[32];
    snprintf(header, sizeof(header), "%u F1L3D // %u UNR34D",
             (unsigned)total, (unsigned)Mailbox::unreadCount());
    canvas->drawString(header, 6, TOP_BAR_H + 6);
    canvas->drawLine(6, TOP_BAR_H + 17, SCREEN_WIDTH - 6, TOP_BAR_H + 17, dim);

    for (int row = 0; row < kVisibleRows; ++row) {
        int idx = scrollOffset + row;
        if (idx >= total) break;
        const Mailbox::Letter* letter = Mailbox::at((uint8_t)idx);
        if (!letter) continue;
        if (letter->caseIndex >= NpcEventsCore::count()) continue;
        const auto& encounter = NpcEventsCore::get(letter->caseIndex);

        int y = kListTop + row * kRowH;
        bool selected = (idx == currentIdx);
        if (selected) {
            canvas->fillRoundRect(4, y, SCREEN_WIDTH - 8, kRowH - 2, 3, fg);
        }
        uint16_t rowFg = selected ? bg : fg;
        uint16_t rowDim = selected ? bg : dim;

        // Unread files carry a filled tab so the stack reads at a glance.
        bool unread = (letter->flags & Mailbox::FLAG_READ) == 0;
        if (unread) {
            canvas->fillRect(8, y + 5, 4, kRowH - 12, rowFg);
        } else {
            canvas->drawRect(8, y + 5, 4, kRowH - 12, rowDim);
        }

        canvas->setTextDatum(TL_DATUM);
        canvas->setTextSize(1);
        canvas->setTextColor(rowDim);
        char fileNo[10];
        snprintf(fileNo, sizeof(fileNo), "#%03u", (unsigned)letter->fileNo);
        canvas->drawString(fileNo, 18, y + 4);

        canvas->setTextColor(rowFg);
        canvas->drawString(encounter.name, 50, y + 4);

        canvas->setTextColor(rowDim);
        canvas->drawString(encounter.tag, 18, y + 14);

        canvas->setTextDatum(TR_DATUM);
        canvas->setTextColor(rowFg);
        canvas->drawString(statusFor(*letter), SCREEN_WIDTH - 10, y + 4);
        // Depth reached, so a half-walked tree advertises itself.
        if (letter->node > 0) {
            canvas->setTextColor(rowDim);
            char beat[12];
            uint8_t nodeBeat = NpcEventsCore::beatForNode(encounter,
                                                            letter->node);
            if (nodeBeat > 0) {
                snprintf(beat, sizeof(beat), "B34T %u",
                         (unsigned)nodeBeat);
            } else {
                snprintf(beat, sizeof(beat), "R3S3T");
            }
            canvas->drawString(beat, SCREEN_WIDTH - 10, y + 14);
        }
        canvas->setTextDatum(TL_DATUM);
    }

    canvas->setTextSize(1);
    canvas->setTextColor(dim);
    if (scrollOffset > 0) {
        canvas->drawString("^", SCREEN_WIDTH - 10, kListTop - 10);
    }
    if (scrollOffset + kVisibleRows < total) {
        canvas->drawString("v", SCREEN_WIDTH - 10,
                           kListTop + kVisibleRows * kRowH);
    }
}

static void draw() {
    if (!canvas) return;
    uint16_t fg = Display::getColorFG();
    uint16_t bg = Display::getColorBG();
    uint16_t dim = Display::lerpColor565(fg, bg, 0.48f);

    canvas->fillSprite(bg);
    canvas->setTextColor(fg);

    if (hasOpenCase()) {
        // The card is a full-bleed dossier; the list chrome would fight it.
        NpcEvents::draw(*canvas);
        Display::drawUiOverlaysTo(canvas);
        FramePresenter::present(*canvas);
        return;
    }

    Display::drawStatusBarTo(canvas, "P1G P0ST");

    if (Mailbox::count() == 0) {
        drawEmptyBox(dim);
    } else {
        drawList(fg, bg, dim);
    }

    if (!Display::drawHintBottomBar(canvas)) {
        Display::drawBottomBar3To(canvas, "[A/C]SCR", "[B]0P3N", "[C+]3X1T");
    }

    Display::drawUiOverlaysTo(canvas);
    FramePresenter::present(*canvas);
}

} // namespace MailMenu
