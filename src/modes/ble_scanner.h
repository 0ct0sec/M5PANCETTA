/**
 * BLE Scanner — Bluetooth Device Tracker
 *
 * ==[ PIG EARS ]== dedicated BLE recon screen. lists every device Recon finds,
 * sorted by signal strength. select a device for Geiger proximity tracking
 * with full bearing blip scanner — radar grid, RSSI trend, gyro lock, ghost
 * markers. same motion tracker as Spectrum's Giger scanner, tuned for BLE
 * cadence (~20s between samples).
 *
 * Recon stays active (not suspended). forceBlePriority() = aggressive BLE cadence,
 * WiFi scans suppressed. Geiger feeds from selected tracker's RSSI.
 */

#pragma once

#include <Arduino.h>
#include <M5GFX.h>

namespace BleScanner {

// ==[ MODE CONTROL ]==
void start();
void stop();
void update();

// ==[ RENDERING ]== called by Display::drawBleScreen
void draw(M5Canvas& canvas);

// ==[ NAVIGATION ]==
void nextDevice();
void prevDevice();
void selectDevice();       // toggle Geiger tracking on selected device
// Enter bearing tracking for an identity selected by another surface. The
// payload hash survives BLE MAC rotation; false means the catalog has expired.
bool trackDeviceByPayloadHash(const uint8_t* payloadHash);
void exitDetail();         // stop Geiger, return to list
void resetBearing();       // recalibrate gyro bearing lock
void toggleActiveScan();   // cycle passive → active → active+chaff → passive
bool canTriggerSound();    // tracked device is FOLLOWING AIRTAG
void triggerAirTagSound(); // connect and trigger AirTag speaker via GATT
void enumerateGattDevice(); // GATT service discovery + DIS + battery read
void addToWatchlist();      // add tracked device to watchlist (max 6)

// ==[ STATE ]==
bool isTracking();         // Geiger active on a selected device
int16_t getSelectedIndex();
int16_t getDeviceCount();  // visible devices (from Recon BLE catalog)
bool isRadioAvailable();
bool isRadioReady();
const char* getScanModeHint();  // touch affordance for passive/active/chaff cycle

}  // namespace BleScanner
