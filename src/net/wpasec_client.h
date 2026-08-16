/**
 * WPA-SEC Client - HTTP upload to wpa-sec.stanev.org
 *
 * ==[ CLOUD DUMP ]== ship captures to the hive. free cracking.
 */
#pragma once

#include <Arduino.h>

namespace WpaSec {
    // Upload result codes
    enum Result : uint8_t {
        UPLOAD_OK = 0,
        UPLOAD_NO_WIFI,
        UPLOAD_NO_KEY,
        UPLOAD_NO_DATA,
        UPLOAD_HTTP_ERROR,
        UPLOAD_TIMEOUT,
        UPLOAD_PARTIAL,
        UPLOAD_AUTH_FAIL,    // 401/403 — key rejected, prompt user to rotate
        UPLOAD_RATE_LIMITED  // 429/503 — server pushback, caller must back off
    };

    // Upload progress callback
    // current = current capture index, total = total captures
    typedef void (*ProgressCallback)(uint16_t current, uint16_t total);

    // Set progress callback
    void setProgressCallback(ProgressCallback cb);

    // Upload single capture (PCAP format)
    // Returns result code
    Result uploadPCAP(const uint8_t* data, uint32_t len);

    // Upload all unsynced captures
    // Returns result code, sets uploadedCount
    Result uploadAll(uint16_t* uploadedCount);

    // Get last HTTP response code
    int getLastHttpCode();

    // Get last error message
    const char* getLastError();

    // Clear server-driven auth/throttle suppression after credentials change.
    void resetBackoff();

    // Potfile download result codes
    enum DownloadResult : uint8_t {
        DL_OK = 0,
        DL_NO_WIFI,
        DL_NO_KEY,
        DL_HTTP_ERROR,
        DL_EMPTY
    };

    // Download cracked potfile from wpa-sec and save to SD.
    // GET {configured-base}/?api&dl=1 with Cookie:key=KEY.
    // Returns count of valid lines via linesOut if non-null.
    DownloadResult downloadPotfile(const char* savePath = "/hamlet/export/wpasec.pot",
                                   uint32_t* linesOut = nullptr);
}
