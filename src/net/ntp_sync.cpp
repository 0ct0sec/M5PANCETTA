/**
 * NTP Sync - Time synchronization implementation
 *
 * ==[ CLOCK TICK ]== configTime() -> getLocalTime() -> M5.Rtc.setDateTime()
 */

#include "ntp_sync.h"
#include "wifi_client.h"
#include "../core/config.h"
#include "../core/gps.h"
#include "../util/debug_log.h"
#include <WiFi.h>
#include <M5Unified.h>
#include <time.h>

namespace NtpSync {

// NTP servers - pool.ntp.org rotates to nearest
static const char* ntpServer1 = "pool.ntp.org";
static const char* ntpServer2 = "time.nist.gov";
static const char* ntpServer3 = "time.google.com";

// GMT offset: 0 (UTC). User can adjust RTC manually if needed.
static const long gmtOffset = 0;
static const int daylightOffset = 0;

// Sync state
static bool timeSynced = false;
static uint32_t lastSyncEpoch = 0;
static char lastError[48] = "";

static bool applyUtcEpoch(uint32_t epoch, const char* source) {
    if (epoch <= 1704067200u) return false;
    time_t t = (time_t)epoch;
    struct tm utc = {};
    if (gmtime_r(&t, &utc) == nullptr) return false;

    m5::rtc_datetime_t rtcTime = {};
    rtcTime.date.year = utc.tm_year + 1900;
    rtcTime.date.month = utc.tm_mon + 1;
    rtcTime.date.date = utc.tm_mday;
    rtcTime.time.hours = utc.tm_hour;
    rtcTime.time.minutes = utc.tm_min;
    rtcTime.time.seconds = utc.tm_sec;
    M5.Rtc.setDateTime(rtcTime);
    Config::markClockSynced();
    if (!Config::hasTrustedClock()) return false;

    timeSynced = true;
    lastSyncEpoch = epoch;
    lastError[0] = '\0';
    HAMLET_LOGF("[UPLINK] trusted clock from %s: %lu\n",
                source ? source : "time source", (unsigned long)epoch);
    return true;
}

bool syncTime() {
    if (!WifiClient::isConnected()) {
        snprintf(lastError, sizeof(lastError), "NO WIFI");
        return false;
    }

    lastError[0] = '\0';

    // A live GPS fix is already an authenticated-by-physics UTC source for
    // this instrument. Use it before depending on UDP/123, which guest and
    // phone hotspots commonly block even while HTTPS works.
    uint32_t gpsEpoch = GPS::getEpochUtc();
    if (gpsEpoch > 1704067200u && applyUtcEpoch(gpsEpoch, "GPS")) return true;

    // Separate DNS failure from an NTP timeout; both used to collapse into the
    // same CL0CK/TLS message and made a healthy HTTPS route look broken.
    IPAddress resolved;
    bool dnsReady = WiFi.hostByName(ntpServer1, resolved) == 1 ||
                    WiFi.hostByName(ntpServer3, resolved) == 1 ||
                    WiFi.hostByName(ntpServer2, resolved) == 1;
    if (!dnsReady) {
        snprintf(lastError, sizeof(lastError), "NTP DNS FAIL");
        HAMLET_LOGLN("[UPLINK] NTP DNS resolution failed");
        return false;
    }

    // Configure NTP (non-blocking, starts SNTP client)
    configTime(gmtOffset, daylightOffset, ntpServer1, ntpServer2, ntpServer3);

    // Wait for time to sync (max 15 seconds; mobile hotspots can be slow).
    struct tm timeinfo;
    memset(&timeinfo, 0, sizeof(timeinfo));
    int retries = 0;
    const int maxRetries = 30;  // 30 x 500ms = 15s

    while (retries < maxRetries) {
        if (getLocalTime(&timeinfo, 500)) {
            // Validate we got a real time (year > 2024)
            if (timeinfo.tm_year + 1900 >= 2024) {
                break;  // Got valid time
            }
        }
        retries++;
    }

    if (retries >= maxRetries || timeinfo.tm_year + 1900 < 2024) {
        snprintf(lastError, sizeof(lastError), "NTP TIMEOUT");
        HAMLET_LOGLN("[UPLINK] NTP timed out; TLS credentials were not sent");
        return false;
    }

    // configTime/getLocalTime already set the system clock. Read the epoch
    // directly instead of round-tripping UTC through local-time mktime().
    time_t epoch = time(nullptr);
    if (epoch <= (time_t)1704067200u || !applyUtcEpoch((uint32_t)epoch, "NTP")) {
        snprintf(lastError, sizeof(lastError), "CLOCK COMMIT FAIL");
        return false;
    }
    return true;
}

bool isTimeSynced() {
    return timeSynced;
}

uint32_t getLastSyncTime() {
    return lastSyncEpoch;
}

const char* getLastError() {
    return lastError;
}

} // namespace NtpSync
