/**
 * Capture Export - PCAP and hc22000 format conversion
 *
 * ==[ FORMAT FACTORY ]== convert captures for WPA-SEC upload.
 *
 * Result-based API: callers get error codes instead of bare 0.
 */
#pragma once

#include <Arduino.h>
#include "../core/capture.h"
#include "../hal/hal_interface.h"

namespace CaptureExport {

    // ==[ PCAP FORMAT ]== generate PCAP from handshake (beacon + EAPOL frames)
    Result<uint32_t> handshakeToPCAPResult(const CapturedHandshake* hs, uint8_t* buffer, uint32_t maxLen);
    uint32_t handshakeToPCAP(const CapturedHandshake* hs, uint8_t* buffer, uint32_t maxLen);  // legacy

    // ==[ HC22000 FORMAT ]== hashcat-ready output for WPA-SEC upload
    Result<uint16_t> pmkidToHC22000Result(const CapturedPMKID* pmkid, char* buffer, uint16_t maxLen);
    uint16_t pmkidToHC22000(const CapturedPMKID* pmkid, char* buffer, uint16_t maxLen);        // legacy

    Result<uint16_t> handshakeToHC22000Result(const CapturedHandshake* hs, char* buffer, uint16_t maxLen);
    uint16_t handshakeToHC22000(const CapturedHandshake* hs, char* buffer, uint16_t maxLen);   // legacy

    // ==[ BATCH EXPORT ]== all unsynced captures in one buffer
    uint32_t exportAllHC22000(char* buffer, uint32_t maxLen);
    uint32_t getRequiredHC22000Size();

}
