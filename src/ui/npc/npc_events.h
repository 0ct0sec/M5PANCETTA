#pragma once

#include <M5Unified.h>
#include <stdint.h>

namespace NpcEvents {

void init();
// Roaming no longer opens a card. When a case arrives it is filed to the
// Mailbox and the operator is told; this only drives arrival timing now.
void update(uint32_t now, uint8_t room, uint8_t station, bool roamingStable);
void draw(M5Canvas& canvas);

// Opens a filed letter as a live case card. Returns false when the letter is
// gone or its content no longer resolves.
bool openFromMail(uint8_t letterIndex);

bool isActive();
uint8_t getCasesClosed();
uint8_t getClosedCharacterCount();
uint8_t getCharacterCount();
void prevChoice();
void nextChoice();
void select();
void cancel();
void dismiss();

}  // namespace NpcEvents
