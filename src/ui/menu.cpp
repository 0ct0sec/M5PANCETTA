/**
 * Menu - Implementation
 *
 * ==[ HUB ]== grouped mode selector with helper pig + idle roaming.
 * The root exposes six drawers; each drawer contains a small, related tool
 * list. The rendering language stays identical to the original flat lineup,
 * while back navigation now closes one drawer before leaving the hub.
 * After 10s idle the list clears and the six-room compositor takes the case;
 * any input recalls the pig and restores the exact navigation depth.
 */

#include "menu.h"
#include "menu_pig.h"
#include "display.h"
#include "frame_presenter.h"
#include "ui_measurements.h"
#include "npc/npc_events.h"
#include <M5Unified.h>
#include "../hamlet.h"
#include "../core/capture.h"
#include "../core/config.h"
#include "../core/item_drops.h"
#include <stddef.h>
#include <string.h>

namespace Menu {

using namespace UIMeasurements;
using namespace UIMeasurements::MainMenu;
using namespace UIMeasurements::MenuPigLayout;

// ==[ MENU CATALOG ]==
// Leaf order stays stable for status suffixes, help text, and mode coverage.
// Groups only reference these leaves; no mode is cloned or hidden by a second
// dispatch table.

struct MenuItem {
    const char* icon;
    const char* label;
    HamletMode mode;
    const char* helper;
};

static const MenuItem items[] = {
    {"/&", "TRUFFL3S", HamletMode::HUNT,
     "PMKID + EAPOL.\nD-UCB works the\n13-channel lineup.\ncase open."},
    {">>", "W4RDR1V3", HamletMode::WARDRIVE,
     "GPS + WiFi patrol.\nWiGLE CSV keeps\nthe street receipts."},
    {"~~", "RF SC0PE", HamletMode::SPECTRUM,
     "sinc lobes. waterfall.\nAPs enter the lineup.\ndeauth leaves a trail."},
    {"((", "P1G 34RS", HamletMode::BLE_SCANNER,
     "tracker lineup.\nRSSI history.\nGeiger works the tail."},
    {"#!", "D3F H0G4", HamletMode::DEFHOG4,
     "five-pane war room.\nWiFi and BLE compare\nnotes. threat gets filed."},
    {"F>", "N0W F0CK", HamletMode::NOWFLOCK,
     "FNOW/3 stakeout.\npeers trade summaries.\nraw evidence stays home."},
    {"C#", "TH3 T4K3", HamletMode::LOOT,
     "PMKIDs. handshakes.\nPSRAM evidence locker.\nship before lights out."},
    {"@>", "P1G P0ST", HamletMode::MAIL,
     "witnesses file here.\neight slots. every file\nbranches three deep."},
    {"~>", "R1B R4CK", HamletMode::FEEDING,
     "ranks. trophies. items.\ncasework pays the wall.\nvanity keeps receipts."},
    {"==", "TUN3 P1G", HamletMode::SETTINGS,
     "radio. power. display.\nevery sharp edge gets\na labeled switch."},
    {":?", "TH3 L0R3", HamletMode::ABOUT,
     "origin file. build hash.\none damaged report\nper visit."},
    {"<>", "XF3RM0D3", HamletMode::XFER,
     "local AP. QR entry.\nbrowse SD at port 80.\nleave with receipts."},
    {"C5", "C5 M0NST3R", HamletMode::C5MONSTER,
     "dual-band bridge.\nC5 Monster brings\nthe 5GHz arsenal."},
    {"))", "M3SH T4LK", HamletMode::MESH,
     "LoRa on the C6L.\ntype, it goes out to\nthe mesh. broadcast\nonly. no acks."}
};

static_assert(sizeof(items) / sizeof(items[0]) == ITEM_COUNT,
              "menu item table must match Menu::ITEM_COUNT");

// Stable leaf indexes keep the grouping table compact and make it easy to
// prove every original mode remains present exactly once.
static const uint8_t FIELD_OPS_ITEMS[]  = {0, 1};
static const uint8_t SIGNAL_LAB_ITEMS[] = {2, 3, 4, 12};
static const uint8_t COMMS_ITEMS[]      = {5, 13};
static const uint8_t CASE_FILES_ITEMS[] = {6, 7};
static const uint8_t PIG_LIFE_ITEMS[]   = {8, 10};
static const uint8_t SYSTEM_ITEMS[]     = {9, 11};

struct MenuGroup {
    const char* icon;
    const char* label;
    const char* helper;
    const uint8_t* itemIndexes;
    uint8_t itemCount;
};

static const MenuGroup groups[] = {
    {"/&", "F13LD 0PS",
     "boots and wheels.\nhunt the air or\nwork the street.\ncase starts outside.",
     FIELD_OPS_ITEMS, sizeof(FIELD_OPS_ITEMS) / sizeof(FIELD_OPS_ITEMS[0])},
    {"~~", "S1GN4L L4B",
     "spectrum, BLE,\ndefense and C5.\nall RF desks live\nbehind this drawer.",
     SIGNAL_LAB_ITEMS, sizeof(SIGNAL_LAB_ITEMS) / sizeof(SIGNAL_LAB_ITEMS[0])},
    {"))", "C0MS",
     "local peers or LoRa.\nshare summaries,\nmessages and names.\nraw evidence stays.",
     COMMS_ITEMS, sizeof(COMMS_ITEMS) / sizeof(COMMS_ITEMS[0])},
    {"C#", "C4S3 F1L3S",
     "captures and letters.\nevidence shares one\ndrawer. read it before\nlights out.",
     CASE_FILES_ITEMS, sizeof(CASE_FILES_ITEMS) / sizeof(CASE_FILES_ITEMS[0])},
    {"~>", "P1G L1F3",
     "rank, trophies,\nitems and the origin\nfile. Pancetta keeps\npersonal receipts.",
     PIG_LIFE_ITEMS, sizeof(PIG_LIFE_ITEMS) / sizeof(PIG_LIFE_ITEMS[0])},
    {"==", "SYST3M",
     "settings and transfer.\nchange the instrument\nor move its files.\nsharp edges labeled.",
     SYSTEM_ITEMS, sizeof(SYSTEM_ITEMS) / sizeof(SYSTEM_ITEMS[0])}
};

static_assert(sizeof(groups) / sizeof(groups[0]) == GROUP_COUNT,
              "menu group table must match Menu::GROUP_COUNT");
static_assert(sizeof(FIELD_OPS_ITEMS) / sizeof(FIELD_OPS_ITEMS[0]) +
                  sizeof(SIGNAL_LAB_ITEMS) / sizeof(SIGNAL_LAB_ITEMS[0]) +
                  sizeof(COMMS_ITEMS) / sizeof(COMMS_ITEMS[0]) +
                  sizeof(CASE_FILES_ITEMS) / sizeof(CASE_FILES_ITEMS[0]) +
                  sizeof(PIG_LIFE_ITEMS) / sizeof(PIG_LIFE_ITEMS[0]) +
                  sizeof(SYSTEM_ITEMS) / sizeof(SYSTEM_ITEMS[0]) == ITEM_COUNT,
              "menu groups must account for every menu item exactly once");

static constexpr int VISIBLE_ITEMS = kMenuVisibleRows;
static int activeGroup = -1;  // -1=root, otherwise index into groups[]
static int currentIdx = 0;    // index within the current level
static int scrollOffset = 0;

static M5Canvas* canvas = nullptr;
// ==[ IDLE STATE ]==
static uint32_t lastInputTime = 0;
static bool menuVisible = true;  // false when pig is roaming


// forward
static void draw();
static void initMenuState(bool visible);
static void buildMenuTitle(char* title, size_t titleSize);
static int entryCount();
static const MenuGroup* selectedGroup();
static const MenuItem* selectedItem();
static const char* selectedHelper();
static void ensureSelectionVisible();

// ==[ LEVEL HELPERS ]==

static int entryCount() {
    if (activeGroup < 0) return GROUP_COUNT;
    if (activeGroup >= GROUP_COUNT) return 0;
    return groups[activeGroup].itemCount;
}

static const MenuGroup* selectedGroup() {
    if (activeGroup >= 0 || currentIdx < 0 || currentIdx >= GROUP_COUNT) return nullptr;
    return &groups[currentIdx];
}

static const MenuItem* selectedItem() {
    if (activeGroup < 0 || activeGroup >= GROUP_COUNT) return nullptr;
    const MenuGroup& group = groups[activeGroup];
    if (currentIdx < 0 || currentIdx >= group.itemCount) return nullptr;
    const uint8_t itemIdx = group.itemIndexes[currentIdx];
    if (itemIdx >= ITEM_COUNT) return nullptr;
    return &items[itemIdx];
}

static const char* selectedHelper() {
    if (activeGroup < 0) {
        const MenuGroup* group = selectedGroup();
        return group ? group->helper : nullptr;
    }
    const MenuItem* item = selectedItem();
    return item ? item->helper : nullptr;
}

static void ensureSelectionVisible() {
    const int count = entryCount();
    if (count <= 0) {
        currentIdx = 0;
        scrollOffset = 0;
        return;
    }
    if (currentIdx < 0) currentIdx = 0;
    if (currentIdx >= count) currentIdx = count - 1;

    if (currentIdx < scrollOffset) {
        scrollOffset = currentIdx;
    } else if (currentIdx >= scrollOffset + VISIBLE_ITEMS) {
        scrollOffset = currentIdx - VISIBLE_ITEMS + 1;
    }

    int maxOffset = count - VISIBLE_ITEMS;
    if (maxOffset < 0) maxOffset = 0;
    if (scrollOffset > maxOffset) scrollOffset = maxOffset;
    if (scrollOffset < 0) scrollOffset = 0;
}

// ==[ PUBLIC API ]==

static void initMenuState(bool visible) {
    activeGroup = -1;
    currentIdx = 0;
    scrollOffset = 0;
    lastInputTime = millis();
    menuVisible = visible;

    canvas = Display::getSharedCanvas();
}

static void buildMenuTitle(char* title, size_t titleSize) {
    if (!title || titleSize == 0) return;

    if (activeGroup >= 0 && activeGroup < GROUP_COUNT) {
        snprintf(title, titleSize, "%s", groups[activeGroup].label);
        return;
    }

    const char* hamletName = Config::getHamletName();
    if (strcmp(hamletName, Config::getDefaultHamletName()) == 0) {
        snprintf(title, titleSize, "PANCETTA");
        return;
    }

    snprintf(title, titleSize, "%s PANCETTA", hamletName);
}

void enter() {
    initMenuState(true);
    NpcEvents::init();
    MenuPig::enter();
    draw();
}

void enterRoaming() {
    initMenuState(false);
    NpcEvents::init();
    // pig already placed by returnFromWardriveViaTeleport — just init menu chrome
    draw();
}

void onInput() {
    lastInputTime = millis();
    if (NpcEvents::isActive()) return;
    if (MenuPig::isMenuTransitionLocked()) return;
    if (!menuVisible) {
        MenuPig::returnToHelper();
        menuVisible = true;
    }
}

bool isMenuHidden() {
    return !menuVisible;
}

bool isAtRoot() {
    return activeGroup < 0;
}

bool hasActiveEncounter() {
    return NpcEvents::isActive();
}

void cancelEncounter() {
    NpcEvents::cancel();
}

void dismissEncounter() {
    NpcEvents::dismiss();
}

void next() {
    if (NpcEvents::isActive()) {
        NpcEvents::nextChoice();
        draw();
        return;
    }
    // onInput() calls the pig back and un-hides the chrome, so the hidden
    // state has to be read before it runs. That press is spent waking the
    // menu; letting it fall through moved the cursor under an unseen lineup.
    const bool wasHidden = !menuVisible;
    onInput();
    if (MenuPig::isMenuTransitionLocked()) return;
    if (wasHidden) return;

    const int count = entryCount();
    if (count <= 0) return;
    currentIdx++;
    if (currentIdx >= count) {
        currentIdx = 0;
        scrollOffset = 0;
    }
    ensureSelectionVisible();
    draw();
}

void prev() {
    if (NpcEvents::isActive()) {
        NpcEvents::prevChoice();
        draw();
        return;
    }
    const bool wasHidden = !menuVisible;
    onInput();
    if (MenuPig::isMenuTransitionLocked()) return;
    if (wasHidden) return;

    const int count = entryCount();
    if (count <= 0) return;
    currentIdx--;
    if (currentIdx < 0) {
        currentIdx = count - 1;
        scrollOffset = count - VISIBLE_ITEMS;
        if (scrollOffset < 0) scrollOffset = 0;
    }
    ensureSelectionVisible();
    draw();
}

void select() {
    if (NpcEvents::isActive()) {
        NpcEvents::select();
        draw();
        return;
    }
    const bool wasHidden = !menuVisible;
    onInput();
    if (MenuPig::isMenuTransitionLocked()) return;
    if (wasHidden) return;

    if (activeGroup < 0) {
        if (currentIdx < 0 || currentIdx >= GROUP_COUNT) return;
        activeGroup = currentIdx;
        currentIdx = 0;
        scrollOffset = 0;
        lastInputTime = millis();
        draw();
        return;
    }

    const MenuItem* item = selectedItem();
    if (!item) return;
    if (item->mode == HamletMode::WARDRIVE) {
        // wardrive enters via car cinematic, not direct mode switch
        menuVisible = false;
        MenuPig::startWardriveEntry();
        return;
    }
    Hamlet::enterMode(item->mode);
}

bool back() {
    if (!menuVisible || MenuPig::isMenuTransitionLocked() || activeGroup < 0) {
        return false;
    }

    const int previousGroup = activeGroup;
    activeGroup = -1;
    currentIdx = previousGroup;
    scrollOffset = 0;
    lastInputTime = millis();
    ensureSelectionVisible();
    draw();
    return true;
}

// ==[ UPDATE ]==

void update() {
    uint32_t now = millis();

    // Idle timeout -> start roaming
    uint32_t timeout = kIdleTimeoutMs;
    if (menuVisible && !MenuPig::isRoaming() &&
        !MenuPig::isMenuTransitionLocked() &&
        now - lastInputTime >= timeout) {
        menuVisible = false;
        MenuPig::startRoaming();
    }

    MenuPig::setEventHold(NpcEvents::isActive());
    MenuPig::update(now);
    // Capture presets are frozen evidence frames, not live casework. Letting
    // the roaming timer arm here can replace the requested room with an NPC.
    bool roamingStable = !MenuPig::isDebugRoamingFrameActive() &&
                         !menuVisible && MenuPig::isHousePortalReady();
    NpcEvents::update(now, MenuPig::getCurrentRoom(),
                      MenuPig::getCurrentStation(), roamingStable);
    MenuPig::setEventHold(NpcEvents::isActive());
    draw();
}

// ==[ DRAW ]==

static void draw() {
    if (!canvas) return;

    uint16_t fg = Display::getColorFG();
    uint16_t bg = Display::getColorBG();

    canvas->fillSprite(bg);
    canvas->setTextColor(fg);

    // Top bar
    Display::drawStatusBarTo(canvas, "MENU");

    if (menuVisible) {
        // ==[ GROUPED MENU LIST ]== same dimensions and selection treatment as
        // the original flat list. Only the catalog depth changes.
        char title[24];
        buildMenuTitle(title, sizeof(title));
        canvas->setTextDatum(TL_DATUM);
        canvas->setTextSize(2);
        canvas->drawString(title, 4, kMenuTitleY);
        canvas->drawLine(4, kMenuDividerY, 155, kMenuDividerY, fg);

        const int count = entryCount();
        canvas->setTextDatum(TL_DATUM);
        canvas->setTextSize(2);
        for (int i = 0; i < VISIBLE_ITEMS && (scrollOffset + i) < count; i++) {
            const int idx = scrollOffset + i;
            const int y = kMenuStartY + i * kMenuRowH;
            const bool selected = idx == currentIdx;

            if (selected) {
                canvas->fillRect(kSelectX, y, kSelectW, kMenuRowH, fg);
                canvas->setTextColor(bg);
            } else {
                canvas->setTextColor(fg);
            }

            canvas->setCursor(kTextX, y + 2);
            if (activeGroup < 0) {
                const MenuGroup& group = groups[idx];
                canvas->print(group.icon);
                canvas->print(" ");
                canvas->print(group.label);
                // Count plus chevron makes root rows read as drawers, not modes.
                canvas->setCursor(kSelectX + kSelectW - 30, y + 2);
                canvas->printf("%u>", static_cast<unsigned int>(group.itemCount));
            } else {
                const MenuGroup& group = groups[activeGroup];
                const uint8_t itemIdx = group.itemIndexes[idx];
                const MenuItem& item = items[itemIdx];
                canvas->print(item.icon);
                canvas->print(" ");
                canvas->print(item.label);

                // Status suffixes stay attached to the same leaf modes.
                if (item.mode == HamletMode::LOOT) {
                    uint16_t total = Capture::getTotalCount();
                    if (total > 0) canvas->printf(" (%d)", total);
                } else if (item.mode == HamletMode::FEEDING) {
                    canvas->printf(" L%d I%u", Config::getLevel(),
                                   ItemDrops::getCollectedCount());
                }
            }
        }

        // Scroll indicators remain generic even though the initial grouping
        // keeps every drawer below the eight-row viewport.
        canvas->setTextColor(fg);
        canvas->setTextSize(1);
        if (scrollOffset > 0) {
            canvas->setCursor(kScrollX, kMenuStartY + 3);
            canvas->print("^");
        }
        if (scrollOffset + VISIBLE_ITEMS < count) {
            canvas->setCursor(kScrollX,
                              kMenuStartY + (VISIBLE_ITEMS - 1) * kMenuRowH + 3);
            canvas->print("v");
        }

        // Helper pig keeps the visual style; the active row now supplies its
        // own text so group drawers and leaf tools can both explain themselves.
        MenuPig::drawHelper(*canvas, selectedHelper());

        // Bottom bar — root exits, a drawer first returns to the root.
        if (!Display::drawHintBottomBar(canvas)) {
            if (activeGroup < 0) {
                Display::drawBottomBar3To(canvas, "[A/C]GRP", "[B]0P3N", "[C+]3X1T");
            } else {
                Display::drawBottomBar3To(canvas, "[A/C]SCR", "[B]G0", "[C+]B4CK");
            }
        }
    } else {
        // ==[ ROAMING ]== pig in rooms, no menu
        MenuPig::drawRoaming(*canvas);
        NpcEvents::draw(*canvas);
    }

    Display::drawUiOverlaysTo(canvas);

    FramePresenter::present(*canvas);
}

} // namespace Menu
