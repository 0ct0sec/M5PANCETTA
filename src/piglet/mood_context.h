/**
 * Mood context - hostile radio labels made bubble-safe.
 *
 * ==[ COPY HYGIENE ]== flatten controls, trim noise, bound the evidence.
 */
#pragma once

#include <stddef.h>

namespace MoodContext {

inline bool copyDisplay(const char* input, char* output, size_t outputSize,
                        size_t maxChars) {
    if (!output || outputSize == 0) return false;
    output[0] = '\0';
    if (!input || maxChars == 0) return false;

    size_t limit = outputSize - 1;
    if (maxChars < limit) limit = maxChars;
    size_t used = 0;
    bool pendingSpace = false;
    bool replacementRun = false;

    for (const unsigned char* p =
             reinterpret_cast<const unsigned char*>(input);
         *p && used < limit; ++p) {
        unsigned char c = *p;
        if (c >= 0x20 && c <= 0x7E) {
            if (c == ' ') {
                pendingSpace = used > 0;
                replacementRun = false;
                continue;
            }
            // Do not merge distinct tokens when truncation cannot fit both the
            // separator and the next glyph. Identity-bearing labels stop here.
            if (pendingSpace) {
                if (used + 1 >= limit) break;
                output[used++] = ' ';
            }
            pendingSpace = false;
            if (used < limit) output[used++] = static_cast<char>(c);
            replacementRun = false;
            continue;
        }

        if (c < 0x80) {
            pendingSpace = used > 0;
            replacementRun = false;
            continue;
        }

        // The 1-bit bubble font is ASCII. Collapse each UTF-8 byte run to one
        // visible marker instead of leaking mojibake or eating the whole label.
        if (!replacementRun) {
            if (pendingSpace) {
                if (used + 1 >= limit) break;
                output[used++] = ' ';
            }
            pendingSpace = false;
            if (used < limit) output[used++] = '?';
        }
        replacementRun = true;
    }

    while (used > 0 && output[used - 1] == ' ') --used;
    output[used] = '\0';
    return used > 0;
}

}  // namespace MoodContext
