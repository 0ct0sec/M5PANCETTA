/**
 * wardrive_internal.h — shared state for wardrive.cpp / wardrive_ble.cpp
 *
 * NOT a public API. Only included by wardrive engine files.
 * State owned by wardrive.cpp, extern-declared here so wardrive_ble.cpp
 * can read/write the same CSV buffer and GPS observation.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

namespace Wardrive {

// ==[ SHARED STATE ]== owned by wardrive.cpp, used by wardrive_ble.cpp

struct WigleObservation {
    char timestamp[24];
    uint32_t epochS;
    double lat;
    double lon;
    float alt;
    float acc;
};

extern char* writeBuf;
extern uint16_t writeBufLen;
extern bool sdReady;
extern bool csvHeaderWritten;
extern bool haveObservation;
extern WigleObservation currentObservation;

extern const uint16_t WRITE_BUF_SZ_VAL;

// ==[ SHARED HELPERS ]==
bool flushBuffer();
// Returns true only when the row is staged in the write buffer. Callers must
// not mark a record as logged unless this reports success.
bool appendRawCsvRow(const char* row, int len);
size_t escapeCsvField(const char* src, char* dst, size_t dstLen);

} // namespace Wardrive
