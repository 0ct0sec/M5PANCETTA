#include "wifi_observation_producer.h"

#include <WiFi.h>
#include <string.h>

#include "../core/gps.h"

namespace WifiObservationProducer {

static void stampLocation(Defense::WifiObservation& observation) {
    if (GPS::hasFix()) {
        observation.lat = (float)GPS::getLatitude();
        observation.lon = (float)GPS::getLongitude();
    }
}

size_t collectActiveScan(Defense::WifiObservation* out, size_t capacity,
                         int resultCount, uint32_t timestampMs) {
    if (!out || capacity == 0 || resultCount <= 0) return 0;
    const size_t count = (size_t)resultCount < capacity ? (size_t)resultCount : capacity;
    for (size_t i = 0; i < count; ++i) {
        Defense::WifiObservation& observation = out[i];
        memset(&observation, 0, sizeof(observation));
        observation.timestampMs = timestampMs;
        strncpy(observation.ssid, WiFi.SSID((int)i).c_str(), 32);
        observation.ssid[32] = '\0';
        const uint8_t* bssid = WiFi.BSSID((int)i);
        if (bssid) memcpy(observation.bssid, bssid, 6);
        observation.rssi = (int8_t)WiFi.RSSI((int)i);
        observation.channel = (uint8_t)WiFi.channel((int)i);
        observation.authMode = (uint8_t)WiFi.encryptionType((int)i);
        stampLocation(observation);
        observation.valid = true;
    }
    return count;
}

size_t collectWardrive(const wifi_ap_record_t* records, uint16_t resultCount,
                       Defense::WifiObservation* out, size_t capacity,
                       uint32_t timestampMs) {
    if (!records || !out || capacity == 0 || resultCount == 0) return 0;
    const size_t count = resultCount < capacity ? resultCount : capacity;
    for (size_t i = 0; i < count; ++i) {
        Defense::WifiObservation& observation = out[i];
        memset(&observation, 0, sizeof(observation));
        observation.timestampMs = timestampMs;
        strncpy(observation.ssid, (const char*)records[i].ssid, 32);
        observation.ssid[32] = '\0';
        memcpy(observation.bssid, records[i].bssid, 6);
        observation.rssi = records[i].rssi;
        observation.channel = records[i].primary;
        observation.authMode = (uint8_t)records[i].authmode;
        stampLocation(observation);
        observation.valid = true;
    }
    return count;
}

}  // namespace WifiObservationProducer
