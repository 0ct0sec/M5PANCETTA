/**
 * Wardrive — WiFi wardriving engine
 *
 * ==[ WARTHOG ]== async WiFi scan → PSRAM dedup → WiGLE CSV to SD.
 * no promiscuous mode. no IRAM. no DRAM heap. all PSRAM.
 */
#pragma once

#include <Arduino.h>

// Forward declaration — full type in radio/c5monster_uart.h
namespace C5Monster {
    struct GPSFix;
    struct ScanResults;
}

namespace Wardrive {

    struct ScanTelemetry {
        bool wifiScanPending;
        bool wifiResultValid;
        uint32_t wifiCompletedMs;
        uint16_t wifiNetworks;
        uint16_t wifiOpenNetworks;
        int8_t wifiStrongestRssi;
        uint8_t wifiStrongestChannel;
        char wifiStrongestSsid[33];

        bool c5ResultValid;
        uint32_t c5CompletedMs;
        uint16_t c5Networks5GHz;
        int8_t c5StrongestRssi;
        uint8_t c5StrongestChannel;
    };

    // ==[ MODE CONTROL ]==
    void start();         // init scan engine, alloc PSRAM, open CSV
    void stop();          // flush SD, free PSRAM, finalize session
    void update();        // poll scan, process results, flush buffer

    // ==[ STATE ]==
    bool isActive();
    bool isPaused();
    void togglePause();

    // ==[ SESSION STATS ]==
    uint32_t getSessionNewNets();     // unique networks discovered this session
    uint16_t getSessionScanCycles();  // completed async scan passes this session
    uint32_t getSessionElapsedMs();   // live session uptime
    float getSessionDistanceKm();     // haversine-accumulated GPS distance
    uint32_t getLifetimeNets();       // total lifetime networks (from NVS)
    void getScanTelemetry(ScanTelemetry& out);

    // ==[ BLE STATS ]==
    uint16_t getSessionNewBleDevices();  // unique BLE devices this session
    bool isBleEnabled();                 // BLE interleave active

    // ==[ SD STATUS ]==
    bool isSDReady();                 // SD card mounted and writable
    float getSDFreeGB();              // free space in GB

    // ==[ GPS ]==
    bool isGPSRunning();
    bool hasGPSData();
    bool hasGPSNMEA();
    bool hasGPSFix();
    bool hasC5WardriveCoords();
    bool isUsingFreshC5WardriveCoords();
    bool hasC5WardriveConnection();
    double getC5WardriveLatitude();
    double getC5WardriveLongitude();
    uint32_t getC5WardriveCoordAgeMs();
    float getSpeedKmh();
    double getLatitude();
    double getLongitude();
    float getAltitude();
    uint8_t getSatCount();

    // ==[ DUAL-BAND: 5GHz via C5Monster ]==
    void feedC5MonsterGPSFix(const C5Monster::GPSFix& fix);
    void feedC5MonsterScan(const C5Monster::ScanResults& results);
    bool isDualBandActive();
    uint32_t getSession5GHzNetworks(); // current C5 scan count, not cumulative
}
