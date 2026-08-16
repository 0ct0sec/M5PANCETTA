/**
 * XferServer — AP file manager. no network needed.
 *
 * ==[ FILE XFER ]== opens PANCETTA_XFER hotspot. serves browser commander
 * at 192.168.4.1. files, wpasec potfile viewer, wigle csv viewer.
 */
#pragma once

#include <Arduino.h>

namespace Xfer {
    void start();   // WiFi.softAP → DNS → WebServer.begin()
    void stop();    // server.stop → softAPdisconnect → WiFi off
    void update();  // dns.processNextRequest + server.handleClient

    bool     isRunning();
    uint8_t  getClientCount();
    uint32_t getTxBytes();
    uint32_t getRxBytes();
    const char* getSSID();
    const char* getPassword();
    const char* getIP();
    uint32_t getUploadCount();
    uint32_t getDownloadCount();
}
