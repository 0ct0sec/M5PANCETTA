#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

namespace C5GpsMath {

struct RmcFix {
    double latitude;
    double longitude;
    float speedKmh;
};

inline int hexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

inline const char* findSentence(const char* line) {
    return line ? strchr(line, '$') : nullptr;
}

inline bool hasValidChecksum(const char* sentence) {
    if (!sentence || sentence[0] != '$') return false;
    const char* star = strchr(sentence, '*');
    if (!star || star <= sentence + 1 || !star[1] || !star[2]) return false;

    const int hi = hexNibble(star[1]);
    const int lo = hexNibble(star[2]);
    if (hi < 0 || lo < 0) return false;

    uint8_t checksum = 0;
    for (const char* p = sentence + 1; p < star; ++p) {
        checksum ^= static_cast<uint8_t>(*p);
    }
    return checksum == static_cast<uint8_t>((hi << 4) | lo);
}

inline bool getField(const char* sentence, uint8_t wanted,
                     const char*& begin, size_t& length) {
    if (!sentence || sentence[0] != '$') return false;
    const char* cursor = sentence + 1;
    uint8_t field = 0;

    while (*cursor && *cursor != '*') {
        const char* end = cursor;
        while (*end && *end != ',' && *end != '*') ++end;
        if (field == wanted) {
            begin = cursor;
            length = static_cast<size_t>(end - cursor);
            return true;
        }
        if (*end != ',') break;
        cursor = end + 1;
        ++field;
    }
    return false;
}

inline bool parseNumber(const char* begin, size_t length, double& value) {
    if (!begin || length == 0 || length >= 24) return false;
    char token[24];
    memcpy(token, begin, length);
    token[length] = '\0';

    char* end = nullptr;
    value = strtod(token, &end);
    return end != token && *end == '\0';
}

inline bool parseCoordinate(const char* begin, size_t length,
                            double maxDegrees, double& decimalDegrees) {
    double ddmm = 0.0;
    if (!parseNumber(begin, length, ddmm) || ddmm < 0.0) return false;

    const int degrees = static_cast<int>(ddmm / 100.0);
    const double minutes = ddmm - static_cast<double>(degrees) * 100.0;
    if (minutes < 0.0 || minutes >= 60.0 ||
        static_cast<double>(degrees) > maxDegrees) {
        return false;
    }

    decimalDegrees = static_cast<double>(degrees) + minutes / 60.0;
    return decimalDegrees <= maxDegrees;
}

inline bool parseRmcFix(const char* line, RmcFix& fix) {
    const char* sentence = findSentence(line);
    if (!sentence || strlen(sentence) < 12) return false;
    // Accept any NMEA talker (GN/GP/BD/etc.), but only the RMC sentence.
    if (sentence[3] != 'R' || sentence[4] != 'M' || sentence[5] != 'C') {
        return false;
    }
    if (!hasValidChecksum(sentence)) return false;

    const char* field = nullptr;
    size_t length = 0;
    if (!getField(sentence, 2, field, length) ||
        length != 1 || field[0] != 'A') {
        return false;
    }

    double latitude = 0.0;
    double longitude = 0.0;
    if (!getField(sentence, 3, field, length) ||
        !parseCoordinate(field, length, 90.0, latitude)) {
        return false;
    }
    if (!getField(sentence, 4, field, length) || length != 1 ||
        (field[0] != 'N' && field[0] != 'S')) {
        return false;
    }
    if (field[0] == 'S') latitude = -latitude;

    if (!getField(sentence, 5, field, length) ||
        !parseCoordinate(field, length, 180.0, longitude)) {
        return false;
    }
    if (!getField(sentence, 6, field, length) || length != 1 ||
        (field[0] != 'E' && field[0] != 'W')) {
        return false;
    }
    if (field[0] == 'W') longitude = -longitude;

    double speedKnots = 0.0;
    if (!getField(sentence, 7, field, length) ||
        !parseNumber(field, length, speedKnots) || speedKnots < 0.0) {
        speedKnots = 0.0;
    }

    fix.latitude = latitude;
    fix.longitude = longitude;
    fix.speedKmh = static_cast<float>(speedKnots * 1.852);
    return true;
}

} // namespace C5GpsMath
