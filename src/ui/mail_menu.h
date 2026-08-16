/**
 * P1G P0ST - the inbox for filed case letters.
 *
 * [A]/[C] scroll the stack. [B] opens the file. [C+] leaves.
 * Once a file is open the case card owns the buttons.
 */

#ifndef MAIL_MENU_H
#define MAIL_MENU_H

#include <Arduino.h>

namespace MailMenu {
    void enter();
    void update();

    void next();
    void prev();
    void select();   // open the highlighted letter, or advance the open card
    void back();     // bail out of an open card, else exit the menu
    bool hasOpenCase();
}

#endif // MAIL_MENU_H
