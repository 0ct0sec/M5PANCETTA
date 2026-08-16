/**
 * C5Monster observer parsing helpers.
 *
 * Kept header-only and platform-neutral so the live JanOS text grammar can be
 * regression-tested without Arduino or a UART.
 */
#pragma once

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

namespace C5ObserverMath {

static constexpr uint8_t PACKET_RATE_HISTORY_CAPACITY = 48;

struct PacketRateHistory {
    uint16_t samples[PACKET_RATE_HISTORY_CAPACITY];
    uint8_t head;
    uint8_t count;
};

static inline void resetPacketRateHistory(PacketRateHistory& history) {
    memset(&history, 0, sizeof(history));
}

static inline void pushPacketRate(PacketRateHistory& history,
                                  uint32_t packetsPerSecond) {
    const uint16_t bounded = packetsPerSecond > UINT16_MAX
        ? UINT16_MAX
        : (uint16_t)packetsPerSecond;
    history.samples[history.head] = bounded;
    history.head =
        (uint8_t)((history.head + 1u) % PACKET_RATE_HISTORY_CAPACITY);
    if (history.count < PACKET_RATE_HISTORY_CAPACITY) history.count++;
}

static inline uint8_t copyPacketRatesChronological(
    const PacketRateHistory& history, uint16_t* output,
    uint8_t outputCapacity) {
    if (!output || outputCapacity == 0u || history.count == 0u) return 0u;

    const uint8_t copyCount =
        history.count < outputCapacity ? history.count : outputCapacity;
    const uint8_t skip = (uint8_t)(history.count - copyCount);
    const uint8_t oldest = (uint8_t)(
        (history.head + PACKET_RATE_HISTORY_CAPACITY - history.count) %
        PACKET_RATE_HISTORY_CAPACITY);
    for (uint8_t i = 0; i < copyCount; ++i) {
        const uint8_t source = (uint8_t)(
            (oldest + skip + i) % PACKET_RATE_HISTORY_CAPACITY);
        output[i] = history.samples[source];
    }
    return copyCount;
}

// Returns a readable 1/2/5 x power-of-ten ceiling for PPS plots.
static inline uint32_t packetRatePlotCeiling(uint32_t peak) {
    if (peak <= 10u) return 10u;

    uint32_t scale = 1u;
    while (peak > 50u * scale && scale < 10000u) scale *= 10u;
    if (peak <= 10u * scale) return 10u * scale;
    if (peak <= 20u * scale) return 20u * scale;
    return 50u * scale;
}

static inline const char* skipPrefix(const char* line) {
    if (!line) return nullptr;
    while (isspace((unsigned char)*line)) line++;
    if (*line == '>') {
        line++;
        while (isspace((unsigned char)*line)) line++;
    }
    return line;
}

static inline bool commandEquals(const char* command, const char* token) {
    if (!command || !token || !token[0]) return false;
    command = skipPrefix(command);
    const size_t tokenLen = strlen(token);
    if (strncmp(command, token, tokenLen) != 0) return false;
    const char tail = command[tokenLen];
    return tail == '\0' || isspace((unsigned char)tail);
}

// JanOS echoes commands and some streaming samples with a leading prompt
// marker ("> scan_networks", "> 50pkts"). Only a marker with no payload is
// the command-complete prompt.
static inline bool isBarePrompt(const char* line) {
    if (!line) return false;
    while (isspace((unsigned char)*line)) line++;
    if (*line++ != '>') return false;
    while (isspace((unsigned char)*line)) line++;
    return *line == '\0';
}

// JanOS packet_monitor output observed live as "> 50pkts".
static inline bool parsePacketRate(const char* line, uint32_t& packetsPerSecond) {
    const char* p = skipPrefix(line);
    if (!p || !isdigit((unsigned char)*p)) return false;

    char* end = nullptr;
    const unsigned long value = strtoul(p, &end, 10);
    if (end == p || value > UINT32_MAX) return false;
    while (isspace((unsigned char)*end)) end++;
    if (strcmp(end, "pkts") != 0) return false;

    packetsPerSecond = (uint32_t)value;
    return true;
}

// JanOS channel_view output observed live as "ch36:5".
static inline bool parseChannelCount(const char* line, uint8_t& channel,
                                     uint16_t& networkCount) {
    const char* p = skipPrefix(line);
    if (!p || p[0] != 'c' || p[1] != 'h' ||
        !isdigit((unsigned char)p[2])) {
        return false;
    }

    char* end = nullptr;
    const unsigned long parsedChannel = strtoul(p + 2, &end, 10);
    if (end == p + 2 || *end != ':' ||
        parsedChannel == 0 || parsedChannel > 196) {
        return false;
    }

    p = end + 1;
    if (!isdigit((unsigned char)*p)) return false;
    const unsigned long parsedCount = strtoul(p, &end, 10);
    while (isspace((unsigned char)*end)) end++;
    if (*end != '\0' || parsedCount > UINT16_MAX) return false;

    channel = (uint8_t)parsedChannel;
    networkCount = (uint16_t)parsedCount;
    return true;
}

} // namespace C5ObserverMath
