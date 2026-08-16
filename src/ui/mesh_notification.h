/**
 * Mesh arrival notification formatting.
 *
 * Kept free of Arduino/UI dependencies so the exact text that reaches the
 * shared toast can be exercised on the host. Payload text is evidence: normal
 * printable text stays literal; only layout controls and excess length move.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace MeshNotification {

// Header (at worst "M3SH DM // ???? // 255 W41T1NG") plus one newline and
// this much body stays below Display's 96-byte quick-toast buffer.
static constexpr size_t BODY_PREVIEW_CHARS = 58;

static inline bool isSpace(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' ||
           c == '\v';
}

static inline void cleanSender(char out[5], const char* sender) {
    size_t w = 0;
    if (sender) {
        for (size_t i = 0; sender[i] && w < 4; ++i) {
            const unsigned char c = (unsigned char)sender[i];
            if (isSpace(c) || c < 0x20 || c == 0x7F) continue;
            out[w++] = (char)c;
        }
    }
    if (w == 0) out[w++] = '?';
    out[w] = '\0';
}

// Flatten line breaks and repeated whitespace so a payload cannot take over
// the toast layout. Non-whitespace control bytes get a visible replacement;
// printable bytes, including UTF-8, remain byte-for-byte the sender's text.
static inline void cleanBody(char out[BODY_PREVIEW_CHARS + 1],
                             const char* body) {
    size_t w = 0;
    bool pendingSpace = false;
    bool clipped = false;

    if (body) {
        for (size_t i = 0; body[i]; ++i) {
            const unsigned char c = (unsigned char)body[i];
            if (isSpace(c)) {
                if (w > 0) pendingSpace = true;
                continue;
            }

            const size_t need = pendingSpace ? 2u : 1u;
            if (w + need > BODY_PREVIEW_CHARS) {
                clipped = true;
                break;
            }
            if (pendingSpace) out[w++] = ' ';
            out[w++] = (c < 0x20 || c == 0x7F) ? '?' : (char)c;
            pendingSpace = false;
        }
    }

    if (w == 0) {
        memcpy(out, "(blank)", 8);
        return;
    }

    if (clipped) {
        if (w <= BODY_PREVIEW_CHARS - 3) {
            memcpy(out + w, "...", 3);
            w += 3;
        } else {
            const size_t ellipsisAt = (w >= 3) ? (w - 3) : 0;
            memcpy(out + ellipsisAt, "...", 3);
            w = ellipsisAt + 3;
        }
    }
    out[w] = '\0';
}

inline void build(char* out, size_t cap, const char* sender, const char* body,
                  uint8_t unreadCount, bool direct) {
    if (!out || cap == 0) return;

    char who[5];
    char preview[BODY_PREVIEW_CHARS + 1];
    char header[40];
    cleanSender(who, sender);
    cleanBody(preview, body);

    if (unreadCount > 1) {
        snprintf(header, sizeof(header), direct
                     ? "M3SH DM // %s // %u W41T1NG"
                     : "M3SH // %s // %u W41T1NG",
                 who, (unsigned)unreadCount);
    } else {
        snprintf(header, sizeof(header), direct ? "M3SH DM // %s"
                                                 : "M3SH // %s",
                 who);
    }

    snprintf(out, cap, "%s\n%s", header, preview);
}

}  // namespace MeshNotification
