/**
 * Mailbox - the case files wait for Pancetta instead of ambushing him.
 *
 * ==[ P1G P0ST ]== 8 slots, one packed uint32 each, NVS-backed.
 * A witness arriving mid-roam used to seize the screen. Now it files a letter
 * and the top bar carries the unread count until the operator decides.
 */
#pragma once

#include <stdint.h>

struct HAL;

namespace Mailbox {

// Eight is the whole cast plus two. Past that the box is a backlog, not an
// inbox, and the Zeigarnik pull turns into guilt.
constexpr uint8_t CAPACITY = 8;

constexpr uint8_t FLAG_READ = 0x01;   // operator has opened the briefing
constexpr uint8_t FLAG_OPEN = 0x02;   // decisions started, case not filed yet

struct Letter {
    uint8_t caseIndex;      // index into NpcEventsCore
    uint8_t openerVariant;  // frozen at delivery so the file reads the same twice
    uint8_t node;           // 0 = root beat, else 1-based CaseNode index
    uint8_t flags;
    uint16_t fileNo;        // monotonic arrival ordinal, shown as the case number
};

void init(HAL* hal = nullptr);
void save();

// Files a new letter. Returns false when the box is full — the caller should
// treat that as "no delivery happened" and keep its cooldown running.
bool deliver(uint8_t caseIndex, uint8_t openerVariant);

uint8_t count();
uint8_t unreadCount();
bool isFull();

// Null when index is past the end. The pointer is stable until the next
// deliver()/discard() call.
Letter* at(uint8_t index);

void markRead(uint8_t index);
// Records how far into the decision tree this file has been walked, so closing
// the menu mid-case does not rewind the operator's decisions.
void setProgress(uint8_t index, uint8_t node);
// Removes the letter. Called when the case is filed or explicitly binned.
void discard(uint8_t index);

// True when this character already has a letter waiting. Keeps one witness
// from filling the box while the rest of the cast stays unseen.
bool hasCaseFrom(uint8_t caseIndex);

}  // namespace Mailbox
