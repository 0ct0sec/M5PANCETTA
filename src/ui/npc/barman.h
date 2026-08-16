/** barman.h — Barman NPC (DEFHOG1) for Room 4 underground bar */
#pragma once

#include <M5Unified.h>

namespace Barman {

// Draw the barman behind the counter. barBx/barBw = counter X/width for wipe arm range.
void draw(M5Canvas& canvas, uint32_t now, int barBx, int barBw);
// Explicit room lifecycle. Frame stalls must never masquerade as re-entry.
void setRoomVisible(bool visible, uint32_t now);

// Smoke particle origin — valid after draw() called this frame
int smokeEmberX();
int smokeEmberY();

// ==[ DIALOGUE ]== responds separately to Pancetta and his cat, Pig
void onPancettaSpoke(uint32_t now);                // reply to room narration
void onCatSpoke(uint32_t now);                     // reply to Pig's audible meow
void onItemDropped(uint8_t itemId, uint32_t contextOrdinal,
                   uint32_t now);  // K-H0RS3 subtitle ladder
void clearDialogue();                               // clear state (room exit, mode switch)
bool drawBubble(M5Canvas& canvas, uint32_t now);   // render bubble, true if drawn
bool isSpeaking(uint32_t now);                      // for talking anim sync

} // namespace Barman
