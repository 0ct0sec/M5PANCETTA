/**
 * WiFi Chaff — Fake Handshake Injection
 *
 * ==[ HOGWASH ]== pollutes attacker captures with uncrackable EAPOL M1+M2.
 * triggered by Recon deauth detection. injects structurally valid handshakes
 * with random MICs that hashcat will chew on forever.
 * WiFi equivalent of BLE SMOKESCREEN (ble_chaff.cpp).
 */

#pragma once

#include <stdint.h>

// free functions called from Recon via extern (avoids header coupling)

// synchronous: inject all fake STAs now (Recon sniff path — WiFi about to die)
void wifiChaff_injectBurst(const uint8_t* targetBssid, uint8_t channel,
                            uint16_t frameCount);

// deferred: queue burst for next update() (Hunt path — WiFi stays up)
void wifiChaff_onDeauthDetected(const uint8_t* targetBssid, uint8_t channel,
                                 uint16_t frameCount, int8_t peakRSSI);

void wifiChaff_update(uint32_t now);
void wifiChaff_stop();
bool wifiChaff_isActive();

namespace WifiChaff {

void injectBurst(const uint8_t* targetBssid, uint8_t channel, uint16_t frameCount);
void onDeauthDetected(const uint8_t* targetBssid, uint8_t channel,
                      uint16_t frameCount, int8_t peakRSSI);
void update(uint32_t now);
void stop();
bool isActive();

// stats for DEFHOG4 terminal
uint32_t getTotalInjections();   // total fake STAs injected this session
uint32_t getTotalBursts();       // total burst events triggered
const uint8_t* getLastTargetBSSID();  // nullptr if never fired
uint8_t getLastChannel();

}  // namespace WifiChaff
