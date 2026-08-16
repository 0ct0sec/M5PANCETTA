/**
 * WiFi Client - STA mode for upload connectivity
 *
 * ==[ UPLINK ]== connect to AP for WPA-SEC uploads. pause hunt first.
 */
#pragma once

#include <Arduino.h>

namespace WifiClient {
    // Connection states
    enum State : uint8_t {
        WIFI_IDLE = 0,
        WIFI_CONNECTING,
        WIFI_CONNECTED,
        WIFI_DISCONNECTING,
        WIFI_FAILED
    };

    // Initialize (call once at boot)
    void init();

    // Connect to configured AP (from Config)
    // Returns false if no credentials configured
    bool connect();

    // Connect to specific AP
    bool connect(const char* ssid, const char* password);

    // Disconnect and return to monitor mode
    void disconnect();

    // Check connection status
    State getState();
    bool isConnected();
    bool isConnecting();
    bool ownsRadio();  // STA connect/transfer holds the channel from NOWFLOCK

    // Last association/link failure. This deliberately contains no SSID or
    // password, so it is safe to show on the device and in debug logs.
    const char* getLastError();
    uint32_t getBackoffRemainingMs();

    // Get IP address (valid when connected)
    String getIP();

    // Get RSSI of current connection
    int8_t getRSSI();

    // Update tick (call from main loop when in upload mode)
    void update();

    // Timeout config
    void setConnectTimeout(uint32_t ms);  // default 10000ms

    // Clear the exponential reconnect backoff. Call after the user edits the
    // SSID/password via the config portal so we retry immediately instead of
    // waiting out the cooldown from earlier failures.
    void resetBackoff();
}
