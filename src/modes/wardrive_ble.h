/**
 * Wardrive BLE — thin CSV adapter reading from Recon's BLE catalog
 *
 * ==[ WARTHOG BLE ]== Recon drives scanning via wardrive cadence mode.
 * this file handles session dedup + WiGLE CSV formatting only.
 * zero NimBLE lifecycle — Recon stays warm throughout wardrive.
 */
#pragma once

#include <Arduino.h>

namespace WardriveBle {

    // lifecycle (called from Wardrive::start/stop)
    void init();       // alloc PSRAM dedup, activate Recon wardrive BLE cadence
    void shutdown();   // deactivate Recon wardrive cadence, free PSRAM

    // called from Wardrive::update()
    void update();                  // check Recon for new BLE results, format CSV
    void onWifiSweepComplete();     // signal Recon: WiFi sweep done (BLE interleave)
    bool wantsScanWindow();         // true = Recon BLE scan due/active, hold off WiFi
    void onPause();                 // pause active wardrive BLE interleave window
    void onResume();                // resume wardrive BLE interleave window

    // stats for HUD
    bool isEnabled();               // config + compiled + init succeeded
    bool isScanning();              // Recon BLE scan active right now
    uint16_t getSessionNewDevices();

} // namespace WardriveBle
