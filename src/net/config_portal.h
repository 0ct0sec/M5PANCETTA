/**
 * Config Portal - Web-based credential configuration
 *
 * ==[ SETUP MODE ]== AP + captive portal for phone config.
 */
#pragma once

#include <Arduino.h>

namespace ConfigPortal {
    // Portal states
    enum State : uint8_t {
        PORTAL_STOPPED = 0,
        PORTAL_STARTING,
        PORTAL_RUNNING,
        PORTAL_STOPPING
    };

    // Start the config portal (AP + DNS + HTTP)
    void start();

    // Stop the portal and return to normal mode
    void stop();

    // Update tick (call from main loop)
    void update();

    // Get current state
    State getState();
    bool isRunning();

    // Get AP info for display
    const char* getSSID();
    const char* getIP();
    const char* getPassword();  // per-session random WPA2 PSK (empty if portal stopped)
    const char* getLastError(); // non-empty when AP startup failed

    // Check if credentials were saved this session
    bool credentialsSaved();
    void clearSavedFlag();

    // Auto-timeout (default 5 minutes)
    void setAutoTimeout(uint32_t ms);
    uint32_t getRemainingTime();  // ms until auto-stop
}
