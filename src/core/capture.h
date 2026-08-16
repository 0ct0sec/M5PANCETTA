/**
 * Capture - PSRAM buffer for handshakes and PMKIDs
 *
 * ==[ LOOT LOCKER ]== PSRAM-backed stash for FLOCKNOW peer export.
 */

#ifndef CAPTURE_H
#define CAPTURE_H

#include <Arduino.h>
#include <M5Unified.h>
#include <esp_wifi_types.h>
#include <time.h>
#include "config.h"

// ==[ LIMITS ]== stash caps + buffer sizing
#define MAX_CAPTURES 256

// maximum clients tracked per network
#define MAX_CLIENTS_PER_NETWORK 16

// Runtime sizing uses free PSRAM on either target. The 2MB ceiling covers 256
// worst-case handshakes plus journal headroom without starving RF structures.
#define CAPTURE_BUFFER_MIN (512 * 1024)   // 512KB minimum
#define CAPTURE_BUFFER_MAX (2 * 1024 * 1024)  // 2MB maximum (256 HS = 934KB + journal headroom)
#define CAPTURE_PSRAM_RESERVED (512 * 1024)   // reserve for other PSRAM users

// ==[ EAPOL FRAME ]== payload + metadata
struct EAPOLFrame {
    uint8_t data[512];          // EAPOL payload
    uint8_t fullFrame[300];     // full 802.11 frame
    uint16_t len;               // EAPOL length
    uint16_t fullFrameLen;      // full frame length
    uint8_t messageNum;         // message number (1-4)
    uint32_t timestamp;         // Unix epoch seconds
    int8_t rssi;                // signal strength
};

// RTC -> epoch helper (for M5.Rtc datetime structs)
inline uint32_t rtcToEpoch(const m5::rtc_datetime_t& dt) {
    struct tm t = {};
    t.tm_year = dt.date.year - 1900;
    t.tm_mon = dt.date.month - 1;
    t.tm_mday = dt.date.date;
    t.tm_hour = dt.time.hours;
    t.tm_min = dt.time.minutes;
    t.tm_sec = dt.time.seconds;
    t.tm_isdst = -1;
    return (uint32_t)mktime(&t);
}

// Get current epoch time - uses system time (works after NTP sync on all platforms)
// Only returns trusted time (manual RTC/NTP accepted via Config)
inline uint32_t getCurrentEpoch() {
    return Config::getTrustedEpoch();
}

// ==[ HANDSHAKE RECORD ]==
struct CapturedHandshake {
    uint8_t bssid[6];           // AP MAC
    uint8_t station[6];         // client MAC
    char ssid[33];              // SSID (null-terminated)
    EAPOLFrame frames[4];       // M1..M4
    uint8_t capturedMask;       // bitmask of captured messages
    uint32_t firstSeen;         // first capture time
    uint32_t lastSeen;          // last capture time
    bool synced;                // shipped to Pork yet?
    uint8_t* beaconData;        // beacon frame (PSRAM)
    uint16_t beaconLen;         // beacon length
    // quick completeness check (crackable pairs only)
    // Truth table: any adjacent pair OR M1+M4 has enough data to attempt crack.
    // M2 presence means reliable SNonce. Without M2, M3+M4 or M1+M4 may be
    // uncrackable (M4 often zero nonce) — caller checks hasReliableSNonce().
    bool isComplete() const {
        bool hasM1 = (capturedMask & 0x01) != 0;
        bool hasM2 = (capturedMask & 0x02) != 0;
        bool hasM3 = (capturedMask & 0x04) != 0;
        bool hasM4 = (capturedMask & 0x08) != 0;
        return (hasM1 && hasM2) ||  // M1+M2: always valid
               (hasM2 && hasM3) ||  // M2+M3: always valid
               (hasM3 && hasM4) ||  // M3+M4: valid (warn if no M2)
               (hasM1 && hasM4);    // M1+M4: valid (warn if no M2)
    }
    
    // check if specific message is captured
    bool hasMessage(uint8_t num) const {
        if (num < 1 || num > 4) return false;
        return (capturedMask & (1 << (num - 1))) != 0;
    }
    
    // is there an M2? required for reliable SNonce
    bool hasReliableSNonce() const {
        return hasMessage(2);
    }
};

// ==[ PMKID RECORD ]==
struct CapturedPMKID {
    uint8_t bssid[6];           // AP MAC
    uint8_t station[6];         // our MAC (when probed)
    char ssid[33];              // SSID
    uint8_t pmkid[16];          // PMKID value
    uint32_t timestamp;         // capture time
    bool synced;                // already synced?
};

// ==[ CLIENT RECORD ]==
struct DetectedClient {
    uint8_t mac[6];             // client MAC
    int8_t rssi;                // signal strength
    uint32_t lastSeen;          // last seen time
};

// ==[ NETWORK RECORD ]== Hunt tracking
struct DetectedNetwork {
    uint8_t bssid[6];           // AP MAC
    char ssid[33];              // SSID
    int8_t rssi;                // signal strength
    uint8_t channel;            // WiFi channel
    wifi_auth_mode_t authmode;  // auth mode
    uint32_t lastSeen;          // last seen time
    uint16_t beaconCount;       // beacon tally
    bool isTarget;              // selected as target
    bool hasPMF;                // PMF enabled
    bool hasHandshake;          // captured handshake yet?
    bool hasPMKID;              // captured PMKID yet?
    bool isHidden;              // hidden SSID
    bool wasRevealed;           // hidden SSID revealed
    uint8_t probeAttempts;      // 0-3 probe attempts made
    bool gotPMKIDResponse;      // PMKID extracted (terminal — stop probing)
    bool gotResponse;           // probe assoc response received
    bool probeXPAwarded;        // anti-farm: only award probe XP once per network per session
    uint32_t lastProbeTime;     // millis() of last probe (backoff timer)
    // RSSI trend tracking (motion-aware hunt)
    int8_t rssiHistory[4];      // ring buffer (~2-4s of samples)
    uint8_t rssiIdx;            // current position
    uint8_t rssiHistoryCount;   // valid RSSI samples in ring
    int8_t rssiTrend;           // +ve approaching, -ve retreating
    uint32_t rssiLastSample;    // last sample timestamp (500ms rate limit)
    // ==[ SESSION FORENSICS ]== Hunt-sourced M1/M3/M4 state (session only)
    uint32_t beaconFingerprint; // canonical M1 IE/capability hash (all IEs combined)
    bool fingerprintSet;        // baseline fingerprint established
    uint8_t fingerprintMismatchStreak;  // consecutive mismatching beacons
    // ==[ IE CATEGORY SUB-HASHES ]== SNAPPY-style per-category comparison
    // On mismatch, tells us WHAT changed (RSN downgrade vs vendor IE vs radio hardware)
    uint32_t ieHashRates;       // FNV-1a of Supported+Extended Rates (IE 0x01, 0x32)
    uint32_t ieHashRSN;         // FNV-1a of RSN IE (IE 0x30) — cipher/AKM config
    uint32_t ieHashVendor;      // FNV-1a of all Vendor Specific IEs (IE 0xDD)
    uint32_t ieHashCaps;        // FNV-1a of HT Capabilities (IE 0x2D) + Capabilities field
    uint8_t ieChangeMask;       // bit0=rates bit1=rsn bit2=vendor bit3=caps (set on fp mismatch)
    uint16_t lastSeqNum;        // last 12-bit beacon sequence number
    uint8_t seqAnomalyCount;    // M2 anomaly streak in active window
    uint8_t rssiAnomalyCount;   // M3 anomaly streak in active window
    uint32_t lastFingerprintAnomaly;  // last confirmed M1 timestamp
    uint32_t lastSeqAnomaly;    // last M2 anomaly timestamp
    uint32_t lastRssiAnomaly;   // last M3 anomaly timestamp
    DetectedClient clients[MAX_CLIENTS_PER_NETWORK];
    uint8_t clientCount;        // client count
    // ==[ ENHANCED IE DATA ]== passive intel from beacons
    uint8_t wpsState;           // 0=none, 1=unconfigured, 2=configured
    bool wpsLocked;             // WPS locked out (AP Setup Locked)
    uint8_t vendorOUI[3];       // first vendor-specific OUI (router manufacturer)
    bool hasVendorOUI;          // true if vendor OUI extracted
    // Beacon frame storage (PCAP export via Pork)
    // stored in PSRAM to spare DRAM
    uint8_t* beaconFrame;       // first beacon frame (PSRAM)
    uint16_t beaconLen;         // length (0 = not captured)
};

// ==[ CAPTURE TYPE TAG ]==
enum class CaptureType : uint8_t {
    PMKID = 0,
    HANDSHAKE = 1
};

// ==[ BUFFER ENTRY ]== PSRAM index metadata (packed to save DRAM)
struct __attribute__((packed)) CaptureEntry {
    uint32_t offset;            // PSRAM offset
    uint32_t length;            // serialized length (uint32 to prevent overflow on corrupted frames)
    CaptureType type;           // PMKID or Handshake
    bool synced;                // already sent to Pork?
};

// ==[ CAPTURE API ]== PSRAM stash manager
namespace Capture {
    // init PSRAM buffer
    bool init();
    
    // add new captures
    bool addPMKID(const CapturedPMKID* pmkid);
    bool addHandshake(const CapturedHandshake* hs);
    
    // count helpers
    uint16_t getPMKIDCount();
    uint16_t getHandshakeCount();
    uint16_t getTotalCount();
    uint16_t getUnsyncedCount();
    uint16_t getUnsyncedPMKIDCount();
    uint16_t getUnsyncedHandshakeCount();
    
    // fetch by index
    bool getPMKID(uint16_t index, CapturedPMKID* out);
    bool getHandshake(uint16_t index, CapturedHandshake* out);
    
    // mark as synced
    void markPMKIDSynced(uint16_t index);
    void markHandshakeSynced(uint16_t index);

    // nuke all captures (PSRAM + SD journal)
    void clearAll();

    // buffer stats
    uint32_t getUsedBytes();
    uint32_t getFreeBytes();
    bool isFull();
    
    // last capture timestamp (for 4-min IDLE critical trigger)
    uint32_t getLastCaptureTime();  // returns Unix epoch, 0 if none

    // SD journal: restore captures from stash (call after SDStorage::init)
    void restoreJournal();
    // SD journal: flush dirty sync flags to disk (call on mode exit)
    void saveJournal();
    // Controlled sleep/off: drain queued appends and synchronously snapshot
    // the exact PSRAM capture set. False means mounted-SD persistence failed.
    bool sealJournal();
}

#endif // CAPTURE_H
