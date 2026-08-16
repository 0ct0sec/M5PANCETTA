/**
 * Mailbox - Implementation
 *
 * Each letter packs into one uint32 so the whole box is eight NVS words plus a
 * count. No blobs, no versioning headache, and a corrupt word costs one letter
 * rather than the inbox.
 */

#include "mailbox.h"
#include "../hal/hal_interface.h"

namespace Mailbox {

// ==[ PACKING ]== 5 + 2 + 3 + 3 + 16 = 29 bits used of 32.
static constexpr uint32_t CASE_MASK = 0x1Fu;
static constexpr uint8_t VARIANT_SHIFT = 5;
static constexpr uint32_t VARIANT_MASK = 0x03u;
static constexpr uint8_t NODE_SHIFT = 7;
static constexpr uint32_t NODE_MASK = 0x07u;
static constexpr uint8_t FLAG_SHIFT = 10;
static constexpr uint32_t FLAG_MASK = 0x07u;
static constexpr uint8_t FILE_SHIFT = 13;
static constexpr uint32_t FILE_MASK = 0xFFFFu;

static constexpr const char* NS = "sirloin";
static constexpr const char* KEY_COUNT = "mbx_n";
static constexpr const char* KEY_NEXT = "mbx_seq";
// Eight fixed keys: NVS wants literals, and a slot loop reads better than
// snprintf'ing key names on every save.
static const char* const SLOT_KEYS[CAPACITY] = {
    "mbx0", "mbx1", "mbx2", "mbx3", "mbx4", "mbx5", "mbx6", "mbx7",
};

static HAL* _hal = nullptr;
static Letter letters[CAPACITY];
static uint8_t letterCount = 0;
static uint16_t nextFileNo = 1;
static bool dirty = false;
static bool initialized = false;

static uint16_t incrementFileNo(uint16_t fileNo) {
    return fileNo == 0xFFFFu ? 1u : (uint16_t)(fileNo + 1u);
}

static bool fileNoInUse(uint16_t fileNo) {
    for (uint8_t i = 0; i < letterCount; ++i) {
        if (letters[i].fileNo == fileNo) return true;
    }
    return false;
}

// A damaged sequence word must not make the next delivery impersonate a
// surviving case file. There can only be eight live files, so this loop is
// bounded well below a frame's worth of work.
static void advancePastLiveFileNos() {
    if (nextFileNo == 0) nextFileNo = 1;
    while (fileNoInUse(nextFileNo)) {
        nextFileNo = incrementFileNo(nextFileNo);
    }
}

static uint32_t pack(const Letter& l) {
    return ((uint32_t)l.caseIndex & CASE_MASK) |
           (((uint32_t)l.openerVariant & VARIANT_MASK) << VARIANT_SHIFT) |
           (((uint32_t)l.node & NODE_MASK) << NODE_SHIFT) |
           (((uint32_t)l.flags & FLAG_MASK) << FLAG_SHIFT) |
           (((uint32_t)l.fileNo & FILE_MASK) << FILE_SHIFT);
}

static Letter unpack(uint32_t word) {
    Letter l;
    l.caseIndex = (uint8_t)(word & CASE_MASK);
    l.openerVariant = (uint8_t)((word >> VARIANT_SHIFT) & VARIANT_MASK);
    l.node = (uint8_t)((word >> NODE_SHIFT) & NODE_MASK);
    l.flags = (uint8_t)((word >> FLAG_SHIFT) & FLAG_MASK);
    l.fileNo = (uint16_t)((word >> FILE_SHIFT) & FILE_MASK);
    return l;
}

void init(HAL* hal) {
    if (initialized) return;
    initialized = true;
    _hal = hal ? hal : HalGlobal::get();
    letterCount = 0;
    nextFileNo = 1;
    dirty = false;
    if (!_hal) return;

    uint32_t stored = _hal->storageGetUInt(NS, KEY_COUNT, 0);
    if (stored > CAPACITY) stored = CAPACITY;
    for (uint32_t i = 0; i < stored; ++i) {
        uint32_t word = _hal->storageGetUInt(NS, SLOT_KEYS[i], 0);
        if (word == 0) continue;  // never-written slot; skip rather than trust
        letters[letterCount++] = unpack(word);
    }
    uint32_t storedNext = _hal->storageGetUInt(NS, KEY_NEXT, 0);
    if (storedNext == 0) {
        // The sequence key may be missing after an interrupted first save.
        // Slot order is arrival order, so the newest retained file gives us a
        // monotonic recovery point without renumbering the operator's inbox.
        nextFileNo = letterCount == 0
                         ? 1
                         : incrementFileNo(letters[letterCount - 1].fileNo);
        dirty = true;
    } else {
        nextFileNo = (uint16_t)storedNext;
        if (nextFileNo == 0) {
            nextFileNo = 1;
            dirty = true;
        }
    }
    uint16_t recoveredNext = nextFileNo;
    advancePastLiveFileNos();
    if (nextFileNo != recoveredNext) dirty = true;
}

void save() {
    if (!_hal || !dirty) return;
    for (uint8_t i = 0; i < letterCount; ++i) {
        _hal->storagePutUInt(NS, SLOT_KEYS[i], pack(letters[i]));
    }
    _hal->storagePutUInt(NS, KEY_COUNT, letterCount);
    _hal->storagePutUInt(NS, KEY_NEXT, nextFileNo);
    dirty = false;
}

uint8_t count() { return letterCount; }

bool isFull() { return letterCount >= CAPACITY; }

bool hasCaseFrom(uint8_t caseIndex) {
    for (uint8_t i = 0; i < letterCount; ++i) {
        if (letters[i].caseIndex == caseIndex) return true;
    }
    return false;
}

uint8_t unreadCount() {
    uint8_t unread = 0;
    for (uint8_t i = 0; i < letterCount; ++i) {
        if ((letters[i].flags & FLAG_READ) == 0) ++unread;
    }
    return unread;
}

bool deliver(uint8_t caseIndex, uint8_t openerVariant) {
    if (letterCount >= CAPACITY) return false;
    // One open file per witness. A second letter from the same character would
    // read as a duplicate, not a new case.
    if (hasCaseFrom(caseIndex)) return false;
    if (caseIndex > CASE_MASK) return false;

    // Claim an identifier before extending the live range: the next array slot
    // may still contain a discarded file number in RAM.
    advancePastLiveFileNos();
    Letter& l = letters[letterCount++];
    l.caseIndex = caseIndex;
    l.openerVariant = (uint8_t)(openerVariant & VARIANT_MASK);
    l.node = 0;
    l.flags = 0;
    l.fileNo = nextFileNo;
    nextFileNo = incrementFileNo(nextFileNo);
    dirty = true;
    return true;
}

Letter* at(uint8_t index) {
    if (index >= letterCount) return nullptr;
    return &letters[index];
}

void markRead(uint8_t index) {
    if (index >= letterCount) return;
    if (letters[index].flags & FLAG_READ) return;
    letters[index].flags |= FLAG_READ;
    dirty = true;
}

void setProgress(uint8_t index, uint8_t node) {
    if (index >= letterCount) return;
    node &= NODE_MASK;
    if (letters[index].node == node && (letters[index].flags & FLAG_OPEN)) return;
    letters[index].node = node;
    letters[index].flags |= (uint8_t)(FLAG_READ | FLAG_OPEN);
    dirty = true;
}

void discard(uint8_t index) {
    if (index >= letterCount) return;
    for (uint8_t i = index; i + 1 < letterCount; ++i) {
        letters[i] = letters[i + 1];
    }
    --letterCount;
    dirty = true;
    // The vacated tail slot still holds its old word in NVS. Zero it so a
    // later init() cannot resurrect a filed case from a stale key.
    if (_hal) _hal->storagePutUInt(NS, SLOT_KEYS[letterCount], 0);
}

}  // namespace Mailbox
