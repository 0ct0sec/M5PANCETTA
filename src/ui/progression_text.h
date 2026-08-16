#pragma once

#include <stddef.h>
#include <stdio.h>
#include <stdint.h>

namespace ProgressionText {

inline void formatGainAmount(char* out, size_t outLen, uint32_t amount, bool bonus = false) {
    if (!out || outLen == 0) return;
    if (bonus) {
        snprintf(out, outLen, "+%lu XP (BONUS)", (unsigned long)amount);
    } else {
        snprintf(out, outLen, "+%lu XP", (unsigned long)amount);
    }
}

}  // namespace ProgressionText
