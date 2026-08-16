/**
 * Menu - Grouped button/touch navigation hub
 *
 * ==[ HUB HEADER ]== six root drawers, fourteen preserved mode targets,
 * idle slide + pig room expansion.
 */

#ifndef MENU_H
#define MENU_H

#include <Arduino.h>

namespace Menu {
    // All original launch targets remain present once across the six groups.
    static constexpr uint8_t ITEM_COUNT = 14;
    static constexpr uint8_t GROUP_COUNT = 6;

    void enter();
    void enterRoaming();   // menu init for teleport-based return (pig already placed)
    void update();

    void next();
    void prev();
    void select();
    bool back();           // close current group; false when already at root

    void onInput();        // reset idle timer, trigger slide-in if hidden
    bool isMenuHidden();   // true when menu has slid off-screen
    bool isAtRoot();       // true when the six group drawers are visible
    bool hasActiveEncounter();
    void cancelEncounter();
    void dismissEncounter();
}

#endif // MENU_H
