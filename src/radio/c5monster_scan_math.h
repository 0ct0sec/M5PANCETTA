/**
 * C5Monster scan parsing helpers.
 *
 * Header-only and platform-neutral so captured JanOS CSV lines can be tested
 * on the host without Arduino or a UART.
 */
#pragma once

#include "c5monster_protocol.h"
#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

namespace C5ScanMath {

static inline const char* skipWhitespace(const char* p) {
    while (p && isspace((unsigned char)*p)) p++;
    return p;
}

static inline bool parseQuotedField(const char*& p, char* out,
                                    size_t outSize) {
    if (!p || !out || outSize == 0) return false;
    p = skipWhitespace(p);
    if (*p != '"') return false;
    p++;

    size_t written = 0;
    bool closed = false;
    while (*p) {
        if (*p == '"') {
            // Standard CSV escapes a literal quote by doubling it.
            if (p[1] == '"') {
                if (written + 1 < outSize) out[written++] = '"';
                p += 2;
                continue;
            }
            p++;
            closed = true;
            break;
        }
        if (written + 1 < outSize) out[written++] = *p;
        p++;
    }
    out[written] = '\0';
    if (!closed) return false;

    p = skipWhitespace(p);
    if (*p == ',') {
        p++;
    } else if (*p != '\0') {
        return false;
    }
    return true;
}

static inline bool parseSignedInteger(const char* text, int32_t& value) {
    text = skipWhitespace(text);
    if (!text || !text[0]) return false;
    char* end = nullptr;
    const long parsed = strtol(text, &end, 10);
    if (end == text || parsed < INT32_MIN || parsed > INT32_MAX) return false;
    const char* trailing = skipWhitespace(end);
    if (*trailing != '\0') return false;
    value = (int32_t)parsed;
    return true;
}

static inline bool parseUnsignedInteger(const char* text, uint32_t& value) {
    int32_t parsed = 0;
    if (!parseSignedInteger(text, parsed) || parsed < 0) return false;
    value = (uint32_t)parsed;
    return true;
}

static inline bool parseDouble(const char* text, double& value) {
    text = skipWhitespace(text);
    if (!text || !text[0]) return false;
    char* end = nullptr;
    value = strtod(text, &end);
    if (end == text || !isfinite(value)) return false;
    return *skipWhitespace(end) == '\0';
}

static inline bool parseBssid(const char* text, uint8_t* bssid) {
    if (!text || !bssid) return false;
    const char* p = skipWhitespace(text);
    if (!p || !p[0]) return false;

    for (uint8_t i = 0; i < 6; i++) {
        char* end = nullptr;
        const long value = strtol(p, &end, 16);
        if (end == p || value < 0 || value > 255) return false;
        bssid[i] = (uint8_t)value;
        if (i < 5) {
            if (*end != ':' && *end != '-') return false;
            p = end + 1;
        } else {
            if (*skipWhitespace(end) != '\0') return false;
        }
    }
    return true;
}

static inline bool parseScanLine(const char* line,
                                 C5Protocol::ScanEntry& entry) {
    if (!line) return false;
    line = skipWhitespace(line);
    if (!line || line[0] != '"') return false;

    memset(&entry, 0, sizeof(entry));
    entry.authType = C5Protocol::AUTH_UNKNOWN;

    const char* p = line;
    char field[48] = {};

    if (!parseQuotedField(p, field, sizeof(field))) return false;
    uint32_t sourceIndex = 0;
    if (!parseUnsignedInteger(field, sourceIndex) ||
        sourceIndex == 0 || sourceIndex > UINT8_MAX) {
        return false;
    }
    entry.sourceIndex = (uint8_t)sourceIndex;

    if (!parseQuotedField(p, field, sizeof(field))) return false;
    entry.isHidden = field[0] == '\0';
    if (!entry.isHidden) {
        strncpy(entry.ssid, field, sizeof(entry.ssid) - 1);
        entry.ssid[sizeof(entry.ssid) - 1] = '\0';
    }

    // Reserved JanOS field.
    if (!parseQuotedField(p, field, sizeof(field))) return false;

    if (!parseQuotedField(p, field, sizeof(field)) ||
        !parseBssid(field, entry.bssid)) {
        return false;
    }

    if (!parseQuotedField(p, field, sizeof(field))) return false;
    uint32_t channel = 0;
    if (!parseUnsignedInteger(field, channel) ||
        channel == 0 || channel > 196) {
        return false;
    }
    entry.channel = (uint8_t)channel;

    if (!parseQuotedField(p, field, sizeof(field))) return false;
    entry.authType = C5Protocol::parseAuthType(field);

    if (!parseQuotedField(p, field, sizeof(field))) return false;
    int32_t rssi = 0;
    if (!parseSignedInteger(field, rssi) || rssi < -128 || rssi > 0) {
        return false;
    }
    entry.rssi = (int8_t)rssi;

    // Consume and validate the mandatory band field before optional GPS data.
    // The previous parser left this token in place and tried to parse "5GHz"
    // as latitude, making all trailing coordinates unreachable.
    if (!parseQuotedField(p, field, sizeof(field))) return false;
    const bool bandSays5GHz = strcmp(field, "5GHz") == 0;
    const bool bandSays24GHz = strcmp(field, "2.4GHz") == 0;
    if (!bandSays5GHz && !bandSays24GHz) return false;
    entry.is5GHz = C5Protocol::isChannel5GHz(entry.channel);
    if (entry.is5GHz != bandSays5GHz) return false;

    entry.hasGPS = false;
    entry.latitude = 0.0;
    entry.longitude = 0.0;

    const char* optional = skipWhitespace(p);
    if (!optional || optional[0] == '\0') return true;

    char latitudeField[48] = {};
    char longitudeField[48] = {};
    if (!parseQuotedField(p, latitudeField, sizeof(latitudeField)) ||
        !parseQuotedField(p, longitudeField, sizeof(longitudeField))) {
        // A malformed optional suffix must not discard an otherwise valid AP.
        return true;
    }

    double latitude = 0.0;
    double longitude = 0.0;
    if (!parseDouble(latitudeField, latitude) ||
        !parseDouble(longitudeField, longitude) ||
        latitude < -90.0 || latitude > 90.0 ||
        longitude < -180.0 || longitude > 180.0) {
        return true;
    }

    bool gpsValid = true;
    optional = skipWhitespace(p);
    if (optional && optional[0] != '\0') {
        char validField[12] = {};
        uint32_t valid = 0;
        if (!parseQuotedField(p, validField, sizeof(validField)) ||
            !parseUnsignedInteger(validField, valid) || valid > 1) {
            gpsValid = false;
        } else {
            gpsValid = valid == 1;
        }
    }

    if (gpsValid) {
        entry.latitude = latitude;
        entry.longitude = longitude;
        entry.hasGPS = true;
    }
    return true;
}

} // namespace C5ScanMath
