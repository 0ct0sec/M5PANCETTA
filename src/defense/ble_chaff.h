/**
 * BLE Chaff — Decoy Tracker Beacon Broadcaster
 *
 * ==[ SMOKESCREEN ]== pollutes attacker's BLE scanner with fake tracker beacons.
 * rotates 5 chaff types every 2.5s: AirTag, SmartTag, Tile, FastPair, Flipper.
 * NimBLE GAP scheduler interleaves advertising with scanning automatically.
 * requires BROADCASTER role (CONFIG_BT_NIMBLE_ROLE_BROADCASTER_DISABLED removed).
 */

#pragma once

#include <stdint.h>

// free functions called from Recon via extern (avoids header coupling)
void bleChaff_toggle();
bool bleChaff_isActive();
void bleChaff_update(uint32_t now);
void bleChaff_stop();

namespace BLEChaff {

void start();               // begin chaff advertising
void stop();                // stop advertising
void update(uint32_t now);  // rotate chaff payloads (call every frame)
bool isActive();

}  // namespace BLEChaff
