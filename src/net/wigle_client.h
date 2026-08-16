/**
 * WiGLE Client — wardrive CSV upload to api.wigle.net
 *
 * ==[ WARDROVE YIELD ]== push CSVs, get ranked. basic auth. chunked stream.
 */
#pragma once

#include <Arduino.h>

namespace WiGLE {
    enum class FileState : uint8_t {
        LOCAL = 0,  // valid local file, never submitted
        PENDING,    // receipt/in-flight transaction, not terminal yet
        CONFIRMED,  // transaction reached WiGLE status D
        REJECTED    // transaction reached terminal status E
    };

    enum class UploadError : uint8_t {
        NONE = 0,
        NOT_CONNECTED,
        NO_CREDENTIALS,
        NO_MEMORY,
        SD_IO,
        TRANSPORT,
        TIMEOUT,
        AUTH,
        RATE_LIMIT,
        HTTP_CLIENT,
        HTTP_SERVER,
        BAD_JSON,
        API_REJECTED,
        NO_TRANS_ID,
        LEDGER_IO
    };

    struct SyncResult {
        uint16_t submitted; // accepted by WiGLE with a durable transId
        uint16_t confirmed; // pending transactions reconciled to D this run
        uint16_t pending;   // still queued/processing/ambiguous
        uint16_t rejected;  // reconciled to terminal E this run
        uint16_t skipped;   // already submitted/terminal
        uint16_t failed;
        uint16_t empty;     // header-only / malformed WiGLE CSVs
        uint16_t oversize;  // over WiGLE's documented 180 MiB ceiling
        UploadError error;
        int16_t httpCode;
    };

    struct UserStats {
        uint32_t rank;
        uint32_t wifiNets;
        uint32_t totalNets;
        bool     valid;
    };

    typedef void (*ProgressCallback)(uint16_t current, uint16_t total);

    bool       hasCredentials();         // username + token both set
    SyncResult uploadAll(ProgressCallback cb = nullptr);
    UserStats  getCachedStats();         // read from /hamlet/wardrive/.wigle_stats.json
    bool       refreshStats();           // GET api.wigle.net/api/v2/stats/user → cache to SD
    int        getLastHttpCode();
    const char* getLastError();          // safe response/transport summary; no credentials

    // ==[ BATCH/STREAM API ]==
    void       freeUploadedListMemory();   // free PSRAM tracking list, reset lazy flag
    FileState  getFileState(const char* filename); // truthful local/pending/terminal state
    bool       isUploaded(const char* filename);   // O(log n) sorted search
    void       markAsUploaded(const char* filename);
    uint16_t   getUploadedCount();
}
