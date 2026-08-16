/**
 * Small, host-testable pieces of the two cloud upload contracts.
 *
 * Keep protocol truth here instead of scattering string guesses through the
 * transport clients. The endpoints still own final acceptance; these checks
 * only prevent us from calling an HTML/error page a successful receipt.
 */
#pragma once

#include <cctype>
#include <cstring>

namespace UploadContracts {

static const char WIGLE_V16_COLUMNS[] =
    "MAC,SSID,AuthMode,FirstSeen,Channel,Frequency,RSSI,"
    "CurrentLatitude,CurrentLongitude,AltitudeMeters,"
    "AccuracyMeters,RCOIs,MfgrId,Type";

inline bool wigleStatusSucceeded(char status) { return status == 'D'; }
inline bool wigleStatusRejected(char status) { return status == 'E'; }
inline bool wigleStatusPending(char status) {
    return !wigleStatusSucceeded(status) && !wigleStatusRejected(status);
}

inline const char* skipAsciiSpace(const char* text) {
    if (!text) return "";
    while (*text && std::isspace(static_cast<unsigned char>(*text))) ++text;
    return text;
}

inline bool startsWithIgnoreCase(const char* text, const char* prefix) {
    if (!text || !prefix) return false;
    while (*prefix) {
        if (!*text) return false;
        const unsigned char a = static_cast<unsigned char>(*text++);
        const unsigned char b = static_cast<unsigned char>(*prefix++);
        if (std::tolower(a) != std::tolower(b)) return false;
    }
    return true;
}

inline bool wpaSecCookieConfirmsKey(const char* cookie, const char* key) {
    const char* text = skipAsciiSpace(cookie);
    if (!key || !key[0] || !startsWithIgnoreCase(text, "key=")) return false;

    text += 4;
    const size_t keyLen = std::strlen(key);
    if (std::strncmp(text, key, keyLen) != 0) return false;

    const unsigned char terminator = static_cast<unsigned char>(text[keyLen]);
    return terminator == '\0' || terminator == ';' || std::isspace(terminator);
}

inline bool isFlockReplaySidecarName(const char* filename) {
    if (!filename) return false;
    const char* slash = std::strrchr(filename, '/');
    const char* base = slash ? slash + 1 : filename;
    return startsWithIgnoreCase(base, "FLOCK_REPLAY_");
}

inline bool wpaSecResponseIsSemanticError(const char* response) {
    const char* text = skipAsciiSpace(response);
    if (!text[0]) return true;

    // A captive portal or reverse-proxy error can still arrive as HTTP 200.
    // Never burn the local "synced" bit for a web page.
    if (startsWithIgnoreCase(text, "<!doctype html") ||
        startsWithIgnoreCase(text, "<html")) return true;

    static constexpr const char* ERRORS[] = {
        "Not a valid capture file",
        "Capture processing error",
        "This capture file was already submitted",
        "No valid handshakes/PMKIDs found",
        "Bad capture file",
        "No capture submitted"
    };
    for (const char* error : ERRORS) {
        if (startsWithIgnoreCase(text, error)) return true;
    }
    return false;
}

}  // namespace UploadContracts
