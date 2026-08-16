/**
 * Mesh Talk — LoRa text messaging screen for the Unit C6L
 *
 * ==[ THE LONG WIRE ]== a scrollback of everything the mesh has said, and a
 * composer that borrows the same SoftKeyboard every other text field uses, so
 * the CardKB and a fingertip reach it identically.
 */
#pragma once

#include <Arduino.h>
#include <M5Unified.h>

namespace MeshMenu {

// ==[ LIFECYCLE ]==
void enter();
void exit();
void update();
void draw(M5Canvas& canvas);

// ==[ INPUT ]==
void handleBtnOK(bool longPress);
void handleBtnBack(bool longPress);
void handleTouch(int x, int y);
void scrollUp();     // toward older messages
void scrollDown();   // back toward the newest
void togglePane();   // scrollback <-> roster; the tabs and a right swipe

// ==[ STATUS ]==
bool isActive();
bool isComposing();

}  // namespace MeshMenu
