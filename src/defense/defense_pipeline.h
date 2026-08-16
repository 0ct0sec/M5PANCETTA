/**
 * Sole coordinator for defense lifecycle, radio custody, event admission, and
 * snapshot publication. Mutable Recon/XBand tables stay behind this case desk;
 * consumers read the published statement and return it before the next update.
 */
#pragma once

#include <stdint.h>

#include "defense_contracts.h"
#include "defense_pipeline_policy.h"
#include "defense_snapshot.h"

namespace C5Monster { struct ScanResults; }

namespace DefensePipeline {

void init();
void update(uint32_t now);
void requestOperatingState(Defense::OperatingState state);
Defense::OperatingState operatingState();

const Defense::DefenseSnapshot& snapshot();

bool emitEvent(const Defense::DefenseEventData& event);
bool hasEvent();
Defense::DefenseEventData popEvent();

void feedC5MonsterScan(const C5Monster::ScanResults& results);
void ingestWardriveSnapshot(const wifi_ap_record_t* records, uint16_t count);
void ingestDeauthObservation(uint8_t channel, int8_t rssi, uint8_t subtype,
                             const uint8_t* sourceMac,
                             Defense::DeauthSourceOrigin origin,
                             const uint8_t* targetBssid = nullptr,
                             uint16_t reasonCode = 0);
void cacheProbeVulnMatch(const uint8_t* clientMac, const char* ssid, int8_t rssi);
void reportKarmaFromProbeResponse(const char* ssid, const char* detail);

void clearOfflineScanCount();
bool requestWifiScan();
void setForcedCadence(Defense::CadenceTier tier);
void clearForcedCadence();
void pinBleDevice(const uint8_t* payloadHash);
void clearPinnedBleDevice();
void setActiveScan(bool active);
void toggleChaff();
void pauseBLEScanForGATT();
void resumeBLEScanFromGATT();
bool addToWatchlist(const uint8_t* payloadHash, const char* label);
bool removeFromWatchlist(uint8_t slot);
void updateWatchlistLabel(uint8_t slot, const char* label);
void setCanarySSID(const char* ssid);
void setForensicExportEnabled(bool enabled);
void onWardriveSweepComplete();
bool wardriveWantsBleWindow();
bool consumeWardriveBleReady();

}  // namespace DefensePipeline
