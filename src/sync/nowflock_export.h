/**
 * PIGBROTHER EXPORT_SNAPSHOT queue + sanitizer.
 */
#pragma once

#include <stdint.h>

namespace NowFlockExport {

struct SnapshotRecord {
    char profile[12];
    char kind[8];
    uint32_t ts;
    int32_t latE7;
    int32_t lonE7;
    uint32_t ssidHash;
    uint32_t bssidHash;
    uint8_t channel;
    int8_t rssi;
    char auth[12];
};

void init();
void reset();
bool enabled();
void onWardriveRow(const uint8_t* bssid, const char* ssid, uint8_t channel, int8_t rssi,
                   const char* authMode, uint32_t epochS, int32_t latE7, int32_t lonE7,
                   uint32_t nowMs);
bool dequeueLine(uint8_t* out, uint8_t& outLen, uint8_t maxLen);
bool parseSnapshotLine(const uint8_t* line, uint8_t lineLen, SnapshotRecord& out);
void ingestPeerLine(uint32_t sourceNodeId, const uint8_t* line, uint8_t lineLen, uint32_t nowMs);

} // namespace NowFlockExport
