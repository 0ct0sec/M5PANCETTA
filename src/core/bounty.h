/**
 * Bounty - the rolling six-hour window.
 *
 * ==[ THE CONTRACT ]== two captures inside one 6h slot pays a bounty.
 * The retention sim ranked the window length as the single highest-sensitivity
 * parameter in the whole reward model, ahead of the payout itself: a deadline
 * the operator can still reach is worth more than a bigger prize.
 *
 * Slot index is epoch/21600, so the window is wall-clock aligned and survives
 * reboots. Without a trusted clock the window simply does not arm — a bounty
 * that silently used uptime would pay on a schedule the operator cannot see.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

struct HAL;

namespace Bounty {

constexpr uint32_t WINDOW_S = 21600;  // 6h, from the 6.71h sim optimum
constexpr uint8_t TARGET = 2;         // captures needed inside the window
constexpr uint8_t REWARD_XP = 65;

void init(HAL* hal = nullptr);
void save();

// Call once per capture (PMKID or handshake). Rolls the window when the slot
// advances, counts the hit, and pays the bounty on the qualifying capture.
// Returns true only on the frame the bounty is actually paid.
bool onCapture();

uint8_t hits();            // captures banked in the current window
bool isPaid();             // this window's bounty already settled
bool isArmed();            // false without a trusted clock
uint32_t secondsRemaining();  // 0 when unarmed

// Writes the status-bar line for a live, unmet window ("B1/2 04h") and returns
// true. Returns false when there is nothing to claim the slot — unarmed, or
// already settled — leaving out untouched. Whether the window outranks whatever
// else wants that space is the caller's call; whether it is worth showing is
// this module's.
bool statusText(char* out, size_t len);

}  // namespace Bounty
