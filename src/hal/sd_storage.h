/**
 * SD Storage HAL — fat32 stash for captures, stats, logs
 *
 * ==[ SILO ]== Board-local SD card. Core2 shares VSPI with the display;
 * CoreS3 SE uses its dedicated G36/G35/G37/G4 TF SPI route.
 * Graceful fallback: no card = PSRAM-only, no crash.
 */

#pragma once
#include <Arduino.h>

namespace SDStorage {

    // ==[ LIFECYCLE ]==
    bool init();                    // mount + mkdir tree. false = no card
    void update();                  // periodic health check (~5s). call from main loop
    void requestRemount();          // user-triggered probe; resets backoff
    bool isAvailable();             // card mounted and writable?

    // ==[ DEFERRED I/O ]== queue small appends; drain when frame has slack
    bool enqueueAppend(const char* path, const uint8_t* data, size_t len);
    void drainDeferred(uint32_t budgetMs);
    bool flushDeferred();          // bounded queue drain for controlled sleep/off
    void setDisplayBusLocked(bool locked);

    // ==[ FILE OPS ]== thin wrappers, auto-remount on fail
    bool writeFile(const char* path, const uint8_t* data, size_t len);
    bool appendFile(const char* path, const uint8_t* data, size_t len);
    bool readFile(const char* path, uint8_t* buf, size_t maxLen, size_t* bytesRead);
    bool exists(const char* path);
    bool remove(const char* path);
    bool renameFile(const char* from, const char* to);
    uint64_t freeBytes();           // 0 if unavailable
    bool beginWriteStream(const char* path, bool truncate = false);
    bool writeStream(const uint8_t* data, size_t len);
    bool flushStream();
    void endWriteStream();
    bool isWriteStreamOpen();

    // ==[ PCAP ]== libpcap file helpers
    bool pcapBegin(const char* path);                       // write global header
    bool pcapAppend(const char* path,                       // append packet record
                    const uint8_t* frame, uint16_t frameLen,
                    uint32_t tsSec, uint32_t tsUsec);

    // ==[ SESSION STATS ]== hunt session summary dump
    struct SessionStats {
        uint32_t timestamp;         // epoch
        uint32_t durationSec;       // hunt duration
        uint16_t pmkids;            // PMKIDs captured
        uint16_t handshakes;        // handshakes captured
        uint16_t networksFound;     // unique networks seen
        uint16_t clientsFound;      // unique clients seen
        uint16_t probesSent;        // PMKID probes sent
        uint16_t deauthsSent;       // deauth bursts sent
        uint32_t steps;             // steps during session
        uint8_t channelStats[13];   // per-channel capture count (ch 1-13)
    };
    bool dumpSessionStats(const SessionStats& stats);

    // ==[ DIRECTORY TREE ]==
    // /hamlet/
    //   captures/       — PCAP files (pmkid_YYYYMMDD.pcap, hs_YYYYMMDD.pcap)
    //   stats/          — session logs, D-UCB history
    //   export/         — WPA-SEC formatted output

}
