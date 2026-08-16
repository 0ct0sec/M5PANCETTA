/**
 * Settings Menu
 */

#ifndef SETTINGS_MENU_H
#define SETTINGS_MENU_H

#include <Arduino.h>

namespace SettingsMenu {
    void enter();
    // Tears down the modal editors. The mode can be left from outside its own
    // input path — the PMIC key opens the power menu from anywhere — and an
    // abandoned soft keyboard would keep claiming input it can no longer draw.
    void exit();
    void update();
    
    void next();
    void prev();
    void select();
    
    // Modal control (returns true if modal was open and closed)
    bool closeModal();
    
    // Legal warning toast handlers
    bool isShowingWarning();
    bool isDestructiveWarning();   // true for reincarnate — gate behind long-press
    void acceptWarning();
    void declineWarning();

    // Keyboard text editing (blocks gestures/buttons in hamlet.cpp)
    bool isTextEditing();

    // PIN editing (blocks normal button dispatch)
    bool isPinEditing();
    void pinInput(char btn);           // route A/B/C to PIN entry
    void pinClearOrCancel();           // C long-press: clear existing PIN or cancel
}

#endif // SETTINGS_MENU_H
