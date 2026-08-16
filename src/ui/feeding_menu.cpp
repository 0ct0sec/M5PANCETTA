/**
 * R1B R4CK - gamification flex wall.
 *
 * ==[ GRATIFICATION INTERFACE ]== level ledger, trophy case,
 * item stories, and the K-H0RS3 barman oracle.
 */

#include "feeding_menu.h"
#include "display.h"
#include "frame_presenter.h"
#include "item_sprites.h"
#include "pancetta_cat_memory.h"
#include <M5Unified.h>
#include <stdio.h>
#include <string.h>
#include "../core/config.h"
#include "../core/achievements.h"
#include "../core/challenges.h"
#include "../core/item_drops.h"
#include "../core/item_effects.h"
#include "../defense/recon.h"
#include "../defense/defense_pipeline.h"
#include "../piglet/mood.h"
#include "../audio/sfx.h"
#include "../haptic/haptic.h"
#include "../hamlet.h"
#include "../hamlet_session.h"

namespace FeedingMenu {

static M5Canvas* canvas = nullptr;

enum RibTab : uint8_t {
    TAB_LEVEL = 0,
    TAB_TROPHY,
    TAB_ITEMS,
    TAB_STATS,
    TAB_COUNT
};

static RibTab currentTab = TAB_LEVEL;
static int selected[TAB_COUNT] = {0, 0, 0, 0};

// Burn verdict, shown in place of the item story so the consequence lands where
// the operator was already reading.
static char consumeToast[80] = "";
static uint32_t consumeToastAt = 0;
static constexpr uint32_t CONSUME_TOAST_MS = 4200;

static constexpr int BREAD_Y = 16;
static constexpr int BREAD_H = 12;
static constexpr int STATS_Y = 30;
static constexpr int LIST_Y = 44;
static constexpr int ROW_H = 14;
static constexpr int VISIBLE_ROWS = 8;
static constexpr int DETAIL_Y = LIST_Y + VISIBLE_ROWS * ROW_H + 5;
static constexpr int DETAIL_H = SCREEN_HEIGHT - BOTTOM_BAR_H - DETAIL_Y - 2;
static constexpr int ITEM_GRID_PX = 2;
static constexpr int ITEM_LIST_W = 212;
static constexpr int ITEM_PREVIEW_X = 218;
// Shares the list column's vertical band exactly: box 44..143, badge 146..155.
static constexpr int ITEM_PREVIEW_Y = LIST_Y;
static constexpr int ITEM_PREVIEW_BOX = 100;
static constexpr int ITEM_PREVIEW_INNER = 96;
static constexpr uint16_t ITEM_PREVIEW_TRANSPARENT_KEY = 0x0001;
// Left edge the right-hand value column may not cross, and the strip the
// scroll carets own on scrolling tabs so they never land on a value.
static constexpr int ROW_RIGHT_MIN = 202;
static constexpr int ITEM_RIGHT_MIN = 166;
static constexpr int TICK_GUTTER = 8;

static const char* const TAB_LABELS[TAB_COUNT] = { "LVL", "TR0PHY", "ITEMS", "ST4TS" };

static M5Canvas* itemPreviewSprite = nullptr;
static uint8_t itemPreviewDecodedId = 0xFF;
static bool itemPreviewReady = false;
static uint16_t itemPreviewThemeFg = 0;
static uint16_t itemPreviewThemeBg = 0;

static void draw();

void enter() {
    currentTab = TAB_LEVEL;

    canvas = Display::getSharedCanvas();
    draw();
}

void update() {
    if (Display::needsOverlayRedraw()) {
        draw();
    }
}

static int tabRows(RibTab tab) {
    switch (tab) {
        case TAB_LEVEL:  return 6;
        case TAB_TROPHY: return (int)Achievement::ACH_COUNT;
        case TAB_ITEMS:  return ItemDrops::getItemCount();
        case TAB_STATS:  return 9;
        default:         return 1;
    }
}

static void clampSelected() {
    int rows = tabRows(currentTab);
    if (rows <= 0) {
        selected[currentTab] = 0;
        return;
    }
    if (selected[currentTab] < 0) selected[currentTab] = 0;
    if (selected[currentTab] >= rows) selected[currentTab] = rows - 1;
}

void next() {
    selected[currentTab]++;
    clampSelected();
    draw();
}

void prev() {
    selected[currentTab]--;
    clampSelected();
    draw();
}

void select() {
    currentTab = (RibTab)(((uint8_t)currentTab + 1) % (uint8_t)TAB_COUNT);
    clampSelected();
    draw();
}

void back() {
    // outer mode handles exit.
}

void handleBtnB() {
    select();
}

void consumeSelected() {
    // Only the items shelf holds anything burnable; the other tabs are ledgers.
    if (currentTab != TAB_ITEMS) return;
    uint8_t id = (uint8_t)selected[TAB_ITEMS];
    bool burned = ItemEffects::consume(id, consumeToast, sizeof(consumeToast));
    consumeToastAt = millis();
    if (burned) {
        SFX::play(SFX::ACHIEVEMENT_UNLOCK);
        Haptic::doubleTap();
    } else {
        SFX::play(SFX::ERROR);
        Haptic::tick();
    }
    draw();
}

static uint16_t dimColor(uint16_t fg, uint16_t bg) {
    return Display::lerpColor565(fg, bg, 0.48f);
}

static const char* shortRarity(ItemDrops::ItemRarity rarity) {
    switch (rarity) {
        case ItemDrops::ItemRarity::COMMON:    return "COM";
        case ItemDrops::ItemRarity::UNCOMMON:  return "UNC";
        case ItemDrops::ItemRarity::RARE:      return "RAR";
        case ItemDrops::ItemRarity::LEGENDARY: return "LEG";
    }
    return "???";
}

static uint16_t flexScore() {
    uint16_t score = 0;
    score += (uint16_t)Config::getLevel() * 3;
    score += (uint16_t)Config::getPrestigeCount() * 30;
    score += (uint16_t)Achievements::getUnlockedCount() * 5;
    score += (uint16_t)ItemDrops::getCollectedCount() * 3;
    score += (uint16_t)ItemDrops::getKHorseTranslationLevel() * 11;
    if (Config::isElder()) score += 42;
    return score;
}

static void truncateTo(char* out, size_t outLen, const char* text, uint8_t maxChars) {
    if (!out || outLen == 0) return;
    if (!text) text = "";
    size_t len = strlen(text);
    if (len > maxChars) len = maxChars;
    if (len >= outLen) len = outLen - 1;
    memcpy(out, text, len);
    out[len] = '\0';
}

static void drawProgressBar(int x, int y, int w, int h, uint8_t pct, uint16_t fg, uint16_t bg, bool inverted) {
    uint16_t line = inverted ? bg : fg;
    uint16_t fill = inverted ? bg : fg;
    uint16_t empty = inverted ? fg : bg;
    canvas->drawRect(x, y, w, h, line);
    int inner = w - 2;
    int fillW = (pct >= 100) ? inner : (pct * inner) / 100;
    if (fillW > 0) canvas->fillRect(x + 1, y + 1, fillW, h - 2, fill);
    if (fillW < inner) canvas->fillRect(x + 1 + fillW, y + 1, inner - fillW, h - 2, empty);
}

static void drawTabBreadcrumb(uint16_t fg, uint16_t bg) {
    canvas->setTextSize(1);
    canvas->setTextDatum(TL_DATUM);
    int x = 4;
    for (uint8_t i = 0; i < (uint8_t)TAB_COUNT; i++) {
        const char* label = TAB_LABELS[i];
        int w = (int)strlen(label) * 6 + 8;
        bool active = (i == (uint8_t)currentTab);
        if (active) {
            canvas->fillRect(x, BREAD_Y, w, BREAD_H, fg);
            canvas->setTextColor(bg);
        } else {
            canvas->setTextColor(fg);
        }
        canvas->setCursor(x + 4, BREAD_Y + 2);
        canvas->print(label);
        x += w + 3;
    }
    canvas->setTextColor(fg);
}

static void drawStatsLine(uint16_t fg, uint16_t bg) {
    uint16_t dim = dimColor(fg, bg);
    char line[64];
    switch (currentTab) {
        case TAB_LEVEL:
            snprintf(line, sizeof(line), "LV%u %s  XP:%lu  FLEX:%u",
                     Config::getLevel(), Config::getRankName(),
                     (unsigned long)Config::getXP(), flexScore());
            break;
        case TAB_TROPHY:
            snprintf(line, sizeof(line), "TR0PH13S %u/%u  N30N BR4G R4CK",
                     Achievements::getUnlockedCount(), (uint8_t)Achievement::ACH_COUNT);
            break;
        case TAB_ITEMS:
            snprintf(line, sizeof(line), "1T3MS %u/%u  DR0PS:%lu DUP:%lu",
                     ItemDrops::getCollectedCount(), ItemDrops::getItemCount(),
                     (unsigned long)ItemDrops::getTotalDropCount(),
                     (unsigned long)ItemDrops::getDuplicateDropCount());
            break;
        case TAB_STATS:
        default:
            snprintf(line, sizeof(line), "B4C0N C1TY L3DG3R  K-H0RS3:%lu",
                     (unsigned long)ItemDrops::getKHorseDropCount());
            break;
    }
    canvas->setTextSize(1);
    canvas->setTextColor(dim);
    canvas->setCursor(6, STATS_Y);
    canvas->print(line);
    canvas->setTextColor(fg);
}

static int listStartFor(int idx, int rows) {
    if (rows <= VISIBLE_ROWS) return 0;
    int start = idx - VISIBLE_ROWS / 2;
    if (start < 0) start = 0;
    int maxStart = rows - VISIBLE_ROWS;
    if (start > maxStart) start = maxStart;
    return start;
}

static void drawRow(int row, const char* mark, const char* label, const char* right,
                    bool isSelected, bool isDim, uint16_t fg, uint16_t bg,
                    bool reserveTick = false) {
    int y = LIST_Y + row * ROW_H;
    uint16_t dim = dimColor(fg, bg);
    uint16_t rowFg = isSelected ? bg : (isDim ? dim : fg);
    uint16_t rowBg = isSelected ? fg : bg;

    if (isSelected) canvas->fillRect(0, y, SCREEN_WIDTH, ROW_H, fg);

    canvas->setTextSize(1);
    canvas->setTextColor(rowFg, rowBg);
    canvas->setCursor(4, y + 3);
    canvas->print(mark ? mark : " ");

    char mid[34];
    truncateTo(mid, sizeof(mid), label, 27);
    canvas->setCursor(18, y + 3);
    canvas->print(mid);

    int rightEdge = SCREEN_WIDTH - 4 - (reserveTick ? TICK_GUTTER : 0);
    int maxRight = (rightEdge - ROW_RIGHT_MIN) / 6;
    if (maxRight > 19) maxRight = 19;
    char rhs[21];
    truncateTo(rhs, sizeof(rhs), right, (uint8_t)maxRight);
    int rx = rightEdge - (int)strlen(rhs) * 6;
    if (rx < ROW_RIGHT_MIN) rx = ROW_RIGHT_MIN;
    canvas->setCursor(rx, y + 3);
    canvas->print(rhs);

    canvas->setTextColor(fg, bg);
}

static void drawScrollTicks(int start, int rows, uint16_t fg, uint16_t bg) {
    uint16_t dim = dimColor(fg, bg);
    canvas->setTextSize(1);
    canvas->setTextColor(dim);
    if (start > 0) {
        canvas->setCursor(SCREEN_WIDTH - 8, LIST_Y + 1);
        canvas->print("^");
    }
    if (start + VISIBLE_ROWS < rows) {
        canvas->setCursor(SCREEN_WIDTH - 8, LIST_Y + (VISIBLE_ROWS - 1) * ROW_H + 2);
        canvas->print("v");
    }
    canvas->setTextColor(fg);
}

static int snapItemPx(int v) {
    if (ITEM_GRID_PX <= 1) return v;
    int half = ITEM_GRID_PX / 2;
    if (v >= 0) return ((v + half) / ITEM_GRID_PX) * ITEM_GRID_PX;
    return -(((-v + half) / ITEM_GRID_PX) * ITEM_GRID_PX);
}

static bool ensureItemPreviewSprite(uint8_t spriteId) {
    const ItemSprites::SpritePng* png = ItemSprites::get(spriteId);
    if (!ItemSprites::hasPixelArtContract(png)) return false;
    uint16_t fg = Display::getColorFG();
    uint16_t bg = Display::getColorBG();
    if (itemPreviewReady && itemPreviewDecodedId == spriteId &&
        itemPreviewThemeFg == fg && itemPreviewThemeBg == bg) return true;

    if (!itemPreviewSprite) {
        itemPreviewSprite = new M5Canvas(&M5.Display);
        if (!itemPreviewSprite) return false;
        itemPreviewSprite->setPsram(true);
        itemPreviewSprite->setColorDepth(16);
    } else {
        itemPreviewSprite->deleteSprite();
    }

    if (!itemPreviewSprite->createSprite(png->width, png->height)) {
        itemPreviewReady = false;
        itemPreviewDecodedId = 0xFF;
        return false;
    }
    itemPreviewSprite->fillSprite(ITEM_PREVIEW_TRANSPARENT_KEY);
    itemPreviewReady = itemPreviewSprite->drawPng(png->data, png->length, 0, 0);
    if (itemPreviewReady) Display::themeMapSprite(*itemPreviewSprite, ITEM_PREVIEW_TRANSPARENT_KEY);
    itemPreviewDecodedId = spriteId;
    itemPreviewThemeFg = fg;
    itemPreviewThemeBg = bg;
    return itemPreviewReady;
}

static void drawItemRow(int row, const char* mark, const char* label, const char* right,
                        bool isSelected, bool isDim, uint16_t fg, uint16_t bg,
                        bool reserveTick = false) {
    int y = LIST_Y + row * ROW_H;
    uint16_t dim = dimColor(fg, bg);
    uint16_t rowFg = isSelected ? bg : (isDim ? dim : fg);
    uint16_t rowBg = isSelected ? fg : bg;

    if (isSelected) canvas->fillRect(0, y, ITEM_LIST_W, ROW_H, fg);

    canvas->setTextSize(1);
    canvas->setTextColor(rowFg, rowBg);
    canvas->setCursor(4, y + 3);
    canvas->print(mark ? mark : " ");

    char mid[26];
    truncateTo(mid, sizeof(mid), label, 20);
    canvas->setCursor(18, y + 3);
    canvas->print(mid);

    int rightEdge = ITEM_LIST_W - 4 - (reserveTick ? TICK_GUTTER : 0);
    int maxRight = (rightEdge - ITEM_RIGHT_MIN) / 6;
    if (maxRight > 6) maxRight = 6;
    char rhs[8];
    truncateTo(rhs, sizeof(rhs), right, (uint8_t)maxRight);
    int rx = rightEdge - (int)strlen(rhs) * 6;
    if (rx < ITEM_RIGHT_MIN) rx = ITEM_RIGHT_MIN;
    canvas->setCursor(rx, y + 3);
    canvas->print(rhs);

    canvas->setTextColor(fg, bg);
}

static void drawItemScrollTicks(int start, int rows, uint16_t fg, uint16_t bg) {
    uint16_t dim = dimColor(fg, bg);
    canvas->setTextSize(1);
    canvas->setTextColor(dim);
    int x = ITEM_LIST_W - 8;
    if (start > 0) {
        canvas->setCursor(x, LIST_Y + 1);
        canvas->print("^");
    }
    if (start + VISIBLE_ROWS < rows) {
        canvas->setCursor(x, LIST_Y + (VISIBLE_ROWS - 1) * ROW_H + 2);
        canvas->print("v");
    }
    canvas->setTextColor(fg);
}

static void drawItemPreview(uint8_t itemId, bool collected, uint16_t fg, uint16_t bg) {
    const ItemDrops::ItemInfo* item = ItemDrops::getItem(itemId);
    uint16_t dim = dimColor(fg, bg);
    int frameX = snapItemPx(ITEM_PREVIEW_X);
    int frameY = snapItemPx(ITEM_PREVIEW_Y);
    int innerX = frameX + ITEM_GRID_PX;
    int innerY = frameY + ITEM_GRID_PX;

    canvas->fillRect(frameX, frameY, ITEM_PREVIEW_BOX, ITEM_PREVIEW_BOX, bg);
    canvas->drawRect(frameX, frameY, ITEM_PREVIEW_BOX, ITEM_PREVIEW_BOX, dim);
    canvas->fillRect(frameX, frameY, 18, ITEM_GRID_PX, fg);
    canvas->fillRect(frameX, frameY, ITEM_GRID_PX, 18, fg);
    canvas->fillRect(frameX + ITEM_PREVIEW_BOX - 18, frameY, 18, ITEM_GRID_PX, fg);
    canvas->fillRect(frameX + ITEM_PREVIEW_BOX - ITEM_GRID_PX, frameY, ITEM_GRID_PX, 18, fg);
    canvas->fillRect(frameX, frameY + ITEM_PREVIEW_BOX - ITEM_GRID_PX, 18, ITEM_GRID_PX, fg);
    canvas->fillRect(frameX, frameY + ITEM_PREVIEW_BOX - 18, ITEM_GRID_PX, 18, fg);
    canvas->fillRect(frameX + ITEM_PREVIEW_BOX - 18, frameY + ITEM_PREVIEW_BOX - ITEM_GRID_PX, 18, ITEM_GRID_PX, fg);
    canvas->fillRect(frameX + ITEM_PREVIEW_BOX - ITEM_GRID_PX, frameY + ITEM_PREVIEW_BOX - 18, ITEM_GRID_PX, 18, fg);

    if (item) {
        const ItemSprites::SpritePng* png = ItemSprites::get(item->spriteId);
        if (collected && png && ensureItemPreviewSprite(item->spriteId)) {
            int iconX = snapItemPx(innerX + (ITEM_PREVIEW_INNER - (int)png->width) / 2);
            int iconY = snapItemPx(innerY + (ITEM_PREVIEW_INNER - (int)png->height) / 2);
            itemPreviewSprite->pushSprite(canvas, iconX, iconY, ITEM_PREVIEW_TRANSPARENT_KEY);
        } else {
            int fx = innerX + 26;
            int fy = innerY + 26;
            if (!collected) {
                canvas->drawRect(fx, fy, 44, 44, dim);
                canvas->fillRect(fx + 10, fy + 10, 24, ITEM_GRID_PX, dim);
                canvas->fillRect(fx + 32, fy + 12, ITEM_GRID_PX, 12, dim);
                canvas->fillRect(fx + 18, fy + 24, 14, ITEM_GRID_PX, dim);
                canvas->fillRect(fx + 20, fy + 34, ITEM_GRID_PX * 2, ITEM_GRID_PX * 2, dim);
                canvas->fillRect(frameX + 20, frameY + 44, 60, 12, fg);
                canvas->setTextSize(1);
                canvas->setTextDatum(MC_DATUM);
                canvas->setTextColor(bg);
                canvas->drawString("SH4D0W", frameX + ITEM_PREVIEW_BOX / 2, frameY + 50);
                canvas->setTextDatum(TL_DATUM);
            } else {
                canvas->drawRect(fx, fy, 44, 44, fg);
                canvas->fillRect(fx + 14, fy + 10, 16, ITEM_GRID_PX, fg);
                canvas->fillRect(fx + 28, fy + 12, ITEM_GRID_PX, 10, fg);
                canvas->fillRect(fx + 20, fy + 22, 10, ITEM_GRID_PX, fg);
                canvas->fillRect(fx + 20, fy + 32, ITEM_GRID_PX * 2, ITEM_GRID_PX * 2, fg);
            }
        }

        char badge[16];
        snprintf(badge, sizeof(badge), "%02u %s", (unsigned)(itemId + 1),
                 collected ? shortRarity(item->rarity) : "???");
        int badgeW = (int)strlen(badge) * 6 + 8;
        int badgeX = frameX + (ITEM_PREVIEW_BOX - badgeW) / 2;
        int badgeY = frameY + ITEM_PREVIEW_BOX + 2;
        canvas->fillRect(badgeX, badgeY, badgeW, 10, collected ? fg : bg);
        canvas->drawRect(badgeX, badgeY, badgeW, 10, fg);
        canvas->setTextSize(1);
        canvas->setTextDatum(MC_DATUM);
        canvas->setTextColor(collected ? bg : fg);
        canvas->drawString(badge, badgeX + badgeW / 2, badgeY + 5);
        canvas->setTextDatum(TL_DATUM);
        canvas->setTextColor(fg, bg);
    }
}

static void drawWrapped(const char* text, int x, int y, int w, int maxLines, uint16_t color) {
    if (!text || !text[0]) return;
    int maxChars = w / 6;
    if (maxChars < 4) return;
    const char* p = text;
    canvas->setTextSize(1);
    canvas->setTextColor(color);
    for (int line = 0; line < maxLines && *p; line++) {
        while (*p == ' ') p++;
        int len = 0;
        int lastSpace = -1;
        while (p[len] && p[len] != '\n' && len < maxChars) {
            if (p[len] == ' ') lastSpace = len;
            len++;
        }
        int take = len;
        if (p[len] && p[len] != '\n' && lastSpace > 0) take = lastSpace;
        if (take <= 0) take = len;
        char buf[58];
        if (take > (int)sizeof(buf) - 1) take = sizeof(buf) - 1;
        memcpy(buf, p, take);
        buf[take] = '\0';
        canvas->setCursor(x, y + line * 10);
        canvas->print(buf);
        p += take;
        if (*p == '\n') p++;
        while (*p == ' ') p++;
    }
}

static void drawDetail(const char* title, const char* subtitle, const char* body,
                       uint16_t fg, uint16_t bg) {
    uint16_t dim = dimColor(fg, bg);
    char titleBuf[50];
    char subBuf[50];
    truncateTo(titleBuf, sizeof(titleBuf), title, 48);
    truncateTo(subBuf, sizeof(subBuf), subtitle, 48);

    canvas->drawRect(4, DETAIL_Y, SCREEN_WIDTH - 8, DETAIL_H, fg);
    canvas->fillRect(5, DETAIL_Y + 1, SCREEN_WIDTH - 10, 12, fg);
    canvas->setTextSize(1);
    canvas->setTextColor(bg);
    canvas->setCursor(9, DETAIL_Y + 3);
    canvas->print(titleBuf);
    canvas->setTextColor(dim);
    canvas->setCursor(9, DETAIL_Y + 17);
    canvas->print(subBuf);
    drawWrapped(body, 9, DETAIL_Y + 29, SCREEN_WIDTH - 18, 3, fg);
    canvas->setTextColor(fg, bg);
}

static const char* kHorseState() {
    uint8_t lvl = ItemDrops::getKHorseTranslationLevel();
    if (lvl == 0) return "B4RM4N: ???";
    if (lvl == 1) return "B4RM4N: ...BARN?";
    if (lvl == 2) return "B4RM4N: BARN OK?";
    return "B4RM4N: ADV1C3 UNL0CK3D";
}

static void formatChallengeLedger(char* out, size_t outLen) {
    if (!out || outLen == 0) return;
    out[0] = '\0';
    size_t used = 0;
    for (int i = 0; i < 3 && used < outLen - 1; i++) {
        const ActiveChallenge* c = Challenges::getChallenge(i);
        if (!c || !c->name) continue;
        char difficulty = c->difficulty == ChallengeDifficulty::EASY ? 'E' :
                          c->difficulty == ChallengeDifficulty::MEDIUM ? 'M' : 'H';
        char state = c->completed ? 'X' : (c->progress >= c->target ? '!' : '-');
        uint16_t shownProgress = (c->progress > c->target) ? c->target : c->progress;
        int written = snprintf(out + used, outLen - used, "%c[%c] %s %u/%u +%uXP%s",
                               difficulty, state, c->name, shownProgress, c->target,
                               c->xpReward, i < 2 ? "\n" : "");
        if (written < 0) break;
        size_t added = (size_t)written;
        used += (added < outLen - used) ? added : (outLen - used - 1);
    }
    if (used == 0) snprintf(out, outLen, "N0 C4S3 F1L3S. R3B00T TH3 D3SK.");
}

static void drawLevelTab(uint16_t fg, uint16_t bg) {
    static const char* const labels[] = { "XP", "RANK", "MOOD", "STREAK", "GOAL", "PRESTIGE" };
    char values[6][34] = {};

    uint32_t xp = Config::getXP();
    uint32_t sessionXP = Config::getSessionXPGained();
    uint8_t level = Config::getLevel();
    uint8_t progress = Config::getXPProgress();
    int momentum = Mood::getMomentum();
    float eff = Mood::getEffectivenessMultiplier();

    snprintf(values[0], sizeof(values[0]), "%lu +%lu", (unsigned long)xp, (unsigned long)sessionXP);
    snprintf(values[1], sizeof(values[1]), "LV%u %s %u%%", level, Config::getRankName(), progress);
    snprintf(values[2], sizeof(values[2]), "%s %+d %.1fx", Mood::getMoodLabel(), momentum, eff);
    snprintf(values[3], sizeof(values[3]), "%u BEST:%u", Config::getStreak(), Config::getBestStreak());
    snprintf(values[4], sizeof(values[4]), "%lu/%u %u%%",
             (unsigned long)Config::getSessionSteps(), Config::getGoalTarget(), Config::getGoalProgress());
    snprintf(values[5], sizeof(values[5]), "P%u STYLE:%u",
             Config::getPrestigeCount(), Config::getMaxUnlockedPigStyle());

    for (int i = 0; i < 6; i++) {
        drawRow(i, (i == selected[TAB_LEVEL]) ? ">" : " ", labels[i], values[i],
                i == selected[TAB_LEVEL], false, fg, bg);
    }

    const char* title = labels[selected[TAB_LEVEL]];
    const char* sub = "F0RB1DD3N CH33S3, BUT H0N3ST";
    const char* body = "Real captures, walks, goals, and trophies turn into flex. Fake cheese gets audited by the pig.";
    if (selected[TAB_LEVEL] == 1) {
        sub = "R4NK L4DD3R";
        body = (level >= 42) ? "Elder form. The city has no higher legal brag shelf."
                             : "Next tier waits at the old milestones: 7,14,21,28,35,42.";
    } else if (selected[TAB_LEVEL] == 2) {
        sub = "V1B3S M4TT3R";
        body = "Mood changes hunt power. The pig remembers kindness, neglect, and suspiciously timed snacks.";
    } else if (selected[TAB_LEVEL] == 3) {
        sub = Config::isStreakAtRisk() ? "STR34K 4T R1SK" : "SH0W UP B0NUS";
        body = "Streaks are loyalty receipts. Miss too many sessions and the rack gets quiet.";
    } else if (selected[TAB_LEVEL] == 4) {
        sub = "D41LY G04L";
        body = Config::getGoalProgress() >= 100
            ? "Goal banked. XP and the evidence drop settle on next boot."
            : "Walk the adaptive target now. XP and the evidence drop settle on next boot.";
    } else if (selected[TAB_LEVEL] == 5) {
        sub = "R31NC4RN4T10N";
        body = "Prestige resets XP at elder form and opens stranger heads. The pig returns with better hats.";
    }
    drawDetail(title, sub, body, fg, bg);

    // Stops short of ROW_RIGHT_MIN so a full-width value (LV41 R4Z0RB4CK 100%)
    // clamps against the column edge without the bar eating its first glyph.
    if (selected[TAB_LEVEL] == 1) {
        drawProgressBar(146, LIST_Y + selected[TAB_LEVEL] * ROW_H + 5, 52, 4, progress, fg, bg, true);
    }
    if (selected[TAB_LEVEL] == 4) {
        drawProgressBar(146, LIST_Y + selected[TAB_LEVEL] * ROW_H + 5, 52, 4, Config::getGoalProgress(), fg, bg, true);
    }
}

static void drawTrophyTab(uint16_t fg, uint16_t bg) {
    int rows = tabRows(TAB_TROPHY);
    int idx = selected[TAB_TROPHY];
    int start = listStartFor(idx, rows);
    bool scrolls = rows > VISIBLE_ROWS;

    for (int r = 0; r < VISIBLE_ROWS && start + r < rows; r++) {
        int achIdx = start + r;
        Achievement ach = (Achievement)achIdx;
        bool unlocked = Achievements::has(ach);
        drawRow(r, unlocked ? "*" : ".", Achievements::getName(ach),
                unlocked ? "FLEX" : "LOCK", achIdx == idx, !unlocked, fg, bg,
                scrolls);
    }
    drawScrollTicks(start, rows, fg, bg);

    Achievement ach = (Achievement)idx;
    char sub[44];
    snprintf(sub, sizeof(sub), "%s // %u/%u UNL0CK3D",
             Achievements::has(ach) ? "TR0PHY L1T" : "C4S3 P3ND1NG",
             Achievements::getUnlockedCount(), (uint8_t)Achievement::ACH_COUNT);
    const char* body = Achievements::getDescription(ach);
    drawDetail(Achievements::getName(ach), sub, body, fg, bg);
}

static void drawItemsTab(uint16_t fg, uint16_t bg) {
    int rows = tabRows(TAB_ITEMS);
    int idx = selected[TAB_ITEMS];
    int start = listStartFor(idx, rows);
    bool scrolls = rows > VISIBLE_ROWS;

    for (int r = 0; r < VISIBLE_ROWS && start + r < rows; r++) {
        int itemIdx = start + r;
        const ItemDrops::ItemInfo* item = ItemDrops::getItem((uint8_t)itemIdx);
        if (!item) continue;
        bool got = ItemDrops::hasCollected((uint8_t)itemIdx);
        char hiddenName[18];
        snprintf(hiddenName, sizeof(hiddenName), "SH4D0W %02d", itemIdx + 1);
        drawItemRow(r, got ? "*" : ".", got ? item->name : hiddenName,
                    got ? shortRarity(item->rarity) : "LOCK",
                    itemIdx == idx, !got, fg, bg, scrolls);
    }
    drawItemScrollTicks(start, rows, fg, bg);

    const ItemDrops::ItemInfo* item = ItemDrops::getItem((uint8_t)idx);
    if (!item) return;
    bool got = ItemDrops::hasCollected((uint8_t)idx);
    drawItemPreview((uint8_t)idx, got, fg, bg);

    // Burn state replaces the rarity tag: once an exhibit is consumable, what
    // it does matters more than how rare it was.
    const ItemEffects::Effect* effect = ItemEffects::effectFor((uint8_t)idx);
    char sub[54];
    if (!got) {
        snprintf(sub, sizeof(sub), "UNF0UND // SH4D0W");
    } else if ((uint8_t)idx == ItemDrops::getKHorseItemId()) {
        snprintf(sub, sizeof(sub), "%s // K:%lu // %s",
                 "C0LL3CT3D",
                 (unsigned long)ItemDrops::getKHorseDropCount(), kHorseState());
    } else if (ItemEffects::wasConsumed((uint8_t)idx)) {
        snprintf(sub, sizeof(sub), "BURN3D // %s", item->tag);
    } else if (effect && effect->buff != ItemEffects::Buff::NONE) {
        snprintf(sub, sizeof(sub), "%s // %s %us // BURN%u",
                 item->tag, ItemEffects::buffLabel(effect->buff),
                 (unsigned)effect->durationS,
                 (unsigned)ItemEffects::burnsRemaining());
    } else {
        snprintf(sub, sizeof(sub), "%s // BURN%u",
                 item->tag, (unsigned)ItemEffects::burnsRemaining());
    }

    // A fresh verdict owns the story panel for a few seconds, then the file
    // goes back to reading like a case note.
    bool toastLive = consumeToast[0] &&
                     (millis() - consumeToastAt) < CONSUME_TOAST_MS;
    const char* body;
    if (toastLive) {
        body = consumeToast;
    } else if (got) {
        body = item->story;
    } else {
        body = "Find it in the field. The rack keeps quiet until the evidence lands.";
    }
    drawDetail(got ? item->name : "SH4D0W DR0P", sub, body, fg, bg);
}

static void challengeMarks(char* out, size_t len) {
    if (!out || len < 4) return;
    out[0] = out[1] = out[2] = '_';
    out[3] = '\0';
    for (int i = 0; i < 3; i++) {
        const ActiveChallenge* c = Challenges::getChallenge(i);
        out[i] = (c && c->completed) ? 'X' : '_';
    }
}

static void drawStatsTab(uint16_t fg, uint16_t bg) {
    static const char* const labels[] = {
        "FLEX", "CAPTURE", "STEPS", "W4RDR1V3", "P1G 34RS", "DR0PS", "TASKS", "B4RM4N", "P1G M3M"
    };
    char values[9][34] = {};
    char marks[4] = "___";
    char taskBody[180] = {};
    challengeMarks(marks, sizeof(marks));
    formatChallengeLedger(taskBody, sizeof(taskBody));

    snprintf(values[0], sizeof(values[0]), "%u BR4G", flexScore());
    snprintf(values[1], sizeof(values[1]), "P:%lu HS:%lu",
             (unsigned long)Config::getTotalPMKIDs(), (unsigned long)Config::getTotalHandshakes());
    snprintf(values[2], sizeof(values[2]), "%lu / %lum",
             (unsigned long)Config::getTotalSteps(), (unsigned long)Config::getTotalDistance());
    snprintf(values[3], sizeof(values[3]), "%lu NETS %u RUNS",
             (unsigned long)Config::getWDTotal(), Config::getWDSessions());
    snprintf(values[4], sizeof(values[4]), "BLE:%u TK:%d F:%d",
             DefensePipeline::snapshot().getTotalBLEDevicesSeen(), DefensePipeline::snapshot().getTrackerCount(), DefensePipeline::snapshot().getFollowingCount());
    snprintf(values[5], sizeof(values[5]), "%lu DUP:%lu",
             (unsigned long)ItemDrops::getTotalDropCount(),
             (unsigned long)ItemDrops::getDuplicateDropCount());
    snprintf(values[6], sizeof(values[6]), "%u/3 [%s]", Challenges::getCompletedCount(), marks);
    snprintf(values[7], sizeof(values[7]), "%s", kHorseState());
    const uint32_t catMemories =
        Config::getCatMemoryMask() & PancettaCat::kAllMemoryBits;
    snprintf(values[8], sizeof(values[8]), "%u/%u F1L3D",
             (unsigned)PancettaCat::memoryCount(catMemories),
             (unsigned)PancettaCat::Memory::COUNT);

    const int rows = tabRows(TAB_STATS);
    const int idx = selected[TAB_STATS];
    const int start = listStartFor(idx, rows);
    for (int r = 0; r < VISIBLE_ROWS && start + r < rows; r++) {
        const int i = start + r;
        drawRow(r, (i == idx) ? ">" : " ", labels[i], values[i],
                i == idx, false, fg, bg, true);
    }
    drawScrollTicks(start, rows, fg, bg);

    const char* body = "The rack tallies what the pig did, what the pig proved, and which lies looked tasty.";
    const char* sub = "B4C0N C1TY 4CC0UNT1NG";
    if (selected[TAB_STATS] == 1) {
        body = "PMKIDs and handshakes are real cheese. If it did not happen on air, it does not flex here.";
    } else if (selected[TAB_STATS] == 2) {
        static char stepSub[44];
        const HamletSession::StepMilestone* next =
            HamletSession::nextStepMilestone(Config::getSessionSteps());
        if (next) {
            snprintf(stepSub, sizeof(stepSub), "N3XT %lu // +%u XP",
                     (unsigned long)next->threshold,
                     (unsigned)next->xpReward);
            sub = stepSub;
            body = "Session shoe leather pays at each filed milestone. The goal and lifetime odometer keep separate receipts.";
        } else {
            sub = "4LL W4LK P4Y0UTS F1L3D";
            body = "Every session walking milestone is paid. The adaptive goal and lifetime odometer keep counting.";
        }
    } else if (selected[TAB_STATS] == 3) {
        body = "Wardrive networks and runs draw the city back onto itself. The map loves receipts.";
    } else if (selected[TAB_STATS] == 4) {
        sub = "BLE C4S3W0RK";
        body = "P1G 34RS catalogs nearby devices, fingerprints tracker families, and turns persistent tails into case files.";
    } else if (selected[TAB_STATS] == 5) {
        body = "Duplicates are encore trophies. The item already loved you and came back messy.";
    } else if (selected[TAB_STATS] == 6) {
        sub = Config::isSessionActive() ? "S3SS10N C4S3S // L1V3"
                                        : "3NG4G3 T0 CL41M R3W4RDS";
        body = taskBody;
    } else if (selected[TAB_STATS] == 7) {
        sub = kHorseState();
        body = (ItemDrops::getKHorseTranslationLevel() >= 3)
            ? "Advice unlocked: chase honest captures, read the evidence, and never trust fake cheese."
            : "He is speaking Korean at the bar. The horse bottle is the subtitle track.";
    } else if (selected[TAB_STATS] == 8) {
        const uint32_t read = Config::getCatLoreSeenMask() & catMemories;
        static char catSub[40];
        snprintf(catSub, sizeof(catSub), "%u R34D // %u UNR34D",
                 (unsigned)PancettaCat::memoryCount(read),
                 (unsigned)PancettaCat::memoryCount(catMemories & ~read));
        sub = catSub;
        body = "Pig's habits become permanent files only when they really happen. Unread memories wait in TH3 L0R3.";
    }
    drawDetail(labels[selected[TAB_STATS]], sub, body, fg, bg);
}

static void drawBottomBar() {
    if (Display::drawHintBottomBar(canvas)) return;
    // The items shelf is the only place long B does anything, so it is the only
    // place that advertises it.
    if (currentTab == TAB_ITEMS) {
        Display::drawBottomBar3To(canvas, "[A/C]SCR", "[B]TAB", "[B+]BURN");
        return;
    }
    Display::drawBottomBar3To(canvas, "[A/C]SCR", "[B]TAB", "[C+]3X1T");
}

static void draw() {
    if (!canvas) return;
    uint16_t fg = Display::getColorFG();
    uint16_t bg = Display::getColorBG();

    canvas->fillSprite(bg);
    canvas->setTextColor(fg);
    canvas->setTextDatum(TL_DATUM);
    canvas->setTextSize(1);

    Display::drawStatusBarTo(canvas, "R1B R4CK");
    drawTabBreadcrumb(fg, bg);
    drawStatsLine(fg, bg);

    switch (currentTab) {
        case TAB_LEVEL:  drawLevelTab(fg, bg); break;
        case TAB_TROPHY: drawTrophyTab(fg, bg); break;
        case TAB_ITEMS:  drawItemsTab(fg, bg); break;
        case TAB_STATS:  drawStatsTab(fg, bg); break;
        default: break;
    }

    drawBottomBar();
    Display::drawUiOverlaysTo(canvas);
    FramePresenter::present(*canvas);
}

} // namespace FeedingMenu
