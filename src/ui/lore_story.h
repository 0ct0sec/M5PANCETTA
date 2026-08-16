#pragma once

#include <stddef.h>
#include <stdint.h>
#include "pancetta_cat_memory.h"

namespace LoreStory {

static constexpr size_t BODY_MAX_CHARS = 25;
static constexpr size_t BODY_MAX_LINES = 8;
static constexpr size_t TITLE_MAX_CHARS = 25;

struct Fragment {
    const char* stamp;
    const char* title;
    const char* body;
};

size_t count();
const Fragment& get(uint32_t sequence);
size_t memoryCount();
const Fragment& getMemory(PancettaCat::Memory memory);
bool firstUnreadMemory(uint32_t observedMask, uint32_t seenMask,
                       PancettaCat::Memory& out);

}  // namespace LoreStory
