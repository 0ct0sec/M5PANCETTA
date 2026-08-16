/**
 * Loot Menu - View captured handshakes and PMKIDs
 */

#ifndef LOOT_MENU_H
#define LOOT_MENU_H

#include <Arduino.h>

namespace LootMenu {
    void enter();
    void exit();
    void update();

    void next();
    void prev();
    void select();
    void back();

    bool isInDetailView();

    // WPA-SEC upload
    void startUpload();      // Begin upload to WPA-SEC
    bool isUploading();      // Mid-network-op only (CONNECTING or SENDING)
    bool isUploadModalActive();  // Any upload modal up (incl. SUCCESS/FAILED banner)
    void cancelUpload();     // Cancel or dismiss upload modal

    // Tab cycling: LOOT_LIST → LOOT_CRACKED → LOOT_WIGLE → LOOT_LIST
    void cycleTab();

    // BtnA: action on CRACKED/WIGLE tabs, prev on LOOT_LIST
    void handleBtnA();

    // Context-sensitive action (A button): upload/download depending on current tab
    void triggerAction();

    // Potfile download trigger (from CRACKED view)
    void startPotfileDownload();

    // WiGLE upload trigger (from WIGLE view)
    void startWigleUpload();

    // PIN gate
    bool isPinEntry();                // true if showing PIN entry screen
    void pinInput(char btn);          // route A/B/C to PIN handler

    // Nuke confirmation
    bool isNukeConfirm();             // true if showing nuke warning
    void confirmNuke();               // execute nuke
    void cancelNuke();                // dismiss warning
    void showNuke();                  // trigger nuke confirmation
}

#endif // LOOT_MENU_H
