/**
 * NTP Sync - Time synchronization via NTP
 *
 * ==[ CLOCK TICK ]== sync RTC when WiFi up. no drift, no lies.
 */
#pragma once

#include <Arduino.h>

namespace NtpSync {
    // Sync system time with NTP server, then update M5 RTC
    // Requires active WiFi connection (WifiClient::isConnected())
    // Returns true if sync successful
    bool syncTime();

    // Check if time has been synced this session
    bool isTimeSynced();

    // Get last sync timestamp (0 = never synced)
    uint32_t getLastSyncTime();

    // Safe, credential-free detail for the latest clock bootstrap failure.
    const char* getLastError();
}
