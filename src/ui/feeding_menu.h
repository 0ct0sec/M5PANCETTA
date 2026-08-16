/**
 * R1B R4CK - level ledger, trophy flex, item stories, stats oracle.
 *
 * [A]/[C] scroll the current shelf. [B] cycles shelves.
 */

#ifndef FEEDING_MENU_H
#define FEEDING_MENU_H

#include <Arduino.h>

namespace FeedingMenu {
    void enter();
    void update();
    
    void next();
    void prev();
    void select();     // cycle shelf/tab
    void back();       // internal: cancel toast or exit menu
    void handleBtnB(); // B button handler (confirmation or exit)
    void consumeSelected();  // long B on the items shelf: burn the exhibit
}

#endif // FEEDING_MENU_H
