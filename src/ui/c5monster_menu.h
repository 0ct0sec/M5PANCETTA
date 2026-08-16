/**
 * C5Monster Command Menu — Interactive UI for JanOS commands
 *
 * ==[ DUAL-BAND CONTROL ]== Dedicated menu to execute any C5Monster/JanOS
 * command on demand. Shows live output in terminal-style scroll area.
 */
#pragma once

#include <Arduino.h>
#include <M5Unified.h>

namespace C5Menu {

// ==[ LIFECYCLE ]==
void enter();
void exit();
void update();
void draw(M5Canvas& canvas);

// ==[ INPUT ]==
void handleBtnOK(bool longPress);
void handleBtnBack(bool longPress);
void handleTouch(int x, int y);
void scrollUp();
void scrollDown();

// ==[ STATUS ]==
bool isActive();

} // namespace C5Menu
