/**
 * Capture Export - PCAP and hc22000 format implementation
 *
 * ==[ FORMAT FACTORY ]== hashcat-ready output. no BS.
 *
 * Now with Result-based error propagation alongside legacy wrappers.
 */

#include "capture_export.h"
#include <string.h>
#include <stdio.h>

namespace CaptureExport {

// ==[ PCAP STRUCTURES ]==
#pragma pack(push, 1)
struct PcapGlobalHeader {
    uint32_t magic_number;   // 0xa1b2c3d4
    uint16_t version_major;  // 2
    uint16_t version_minor;  // 4
    int32_t  thiszone;       // GMT offset (0)
    uint32_t sigfigs;        // timestamp accuracy (0)
    uint32_t snaplen;        // max packet length
    uint32_t network;        // link-layer type (105 = IEEE 802.11)
};

struct PcapPacketHeader {
    uint32_t ts_sec;         // timestamp seconds
    uint32_t ts_usec;        // timestamp microseconds
    uint32_t incl_len;       // captured length
    uint32_t orig_len;       // original length
};
#pragma pack(pop)

// Helper: hex encode bytes to string
static void hexEncode(const uint8_t* data, uint16_t len, char* out) {
    static const char hex[] = "0123456789abcdef";
    for (uint16_t i = 0; i < len; i++) {
        out[i * 2] = hex[data[i] >> 4];
        out[i * 2 + 1] = hex[data[i] & 0x0F];
    }
    out[len * 2] = '\0';
}

static void macToHex(const uint8_t* mac, char* out) {
    hexEncode(mac, 6, out);
}

// ==[ RESULT-BASED API ]==

Result<uint32_t> handshakeToPCAPResult(const CapturedHandshake* hs, uint8_t* buffer, uint32_t maxLen) {
    if (!hs || !buffer) return HalError::INVALID_ARGUMENT;
    if (maxLen < sizeof(PcapGlobalHeader)) return HalError::BUFFER_TOO_SMALL;

    uint32_t offset = 0;

    PcapGlobalHeader* gh = (PcapGlobalHeader*)buffer;
    gh->magic_number = 0xa1b2c3d4;
    gh->version_major = 2;
    gh->version_minor = 4;
    gh->thiszone = 0;
    gh->sigfigs = 0;
    gh->snaplen = 65535;
    gh->network = 105;
    offset += sizeof(PcapGlobalHeader);

    // Beacon frame
    if (hs->beaconData && hs->beaconLen > 0) {
        if (offset + sizeof(PcapPacketHeader) + hs->beaconLen > maxLen) {
            return HalError::BUFFER_TOO_SMALL;
        }
        PcapPacketHeader* ph = (PcapPacketHeader*)(buffer + offset);
        ph->ts_sec = hs->firstSeen;
        ph->ts_usec = 0;
        ph->incl_len = hs->beaconLen;
        ph->orig_len = hs->beaconLen;
        offset += sizeof(PcapPacketHeader);
        memcpy(buffer + offset, hs->beaconData, hs->beaconLen);
        offset += hs->beaconLen;
    }

    // EAPOL frames
    for (int i = 0; i < 4; i++) {
        if (hs->hasMessage(i + 1)) {
            const EAPOLFrame* frame = &hs->frames[i];
            const uint8_t* frameData;
            uint16_t frameLen;

            if (frame->fullFrameLen > 0) {
                frameData = frame->fullFrame;
                frameLen = frame->fullFrameLen;
            } else if (frame->len > 0) {
                frameData = frame->data;
                frameLen = frame->len;
            } else {
                continue;
            }

            if (offset + sizeof(PcapPacketHeader) + frameLen > maxLen) {
                return HalError::BUFFER_TOO_SMALL;
            }

            PcapPacketHeader* ph = (PcapPacketHeader*)(buffer + offset);
            ph->ts_sec = frame->timestamp;
            ph->ts_usec = 0;
            ph->incl_len = frameLen;
            ph->orig_len = frameLen;
            offset += sizeof(PcapPacketHeader);
            memcpy(buffer + offset, frameData, frameLen);
            offset += frameLen;
        }
    }

    return Result<uint32_t>(offset);
}

Result<uint16_t> pmkidToHC22000Result(const CapturedPMKID* pmkid, char* buffer, uint16_t maxLen) {
    if (!pmkid || !buffer) return HalError::INVALID_ARGUMENT;
    if (maxLen < 150) return HalError::BUFFER_TOO_SMALL;

    char pmkidHex[33], apMac[13], staMac[13], ssidHex[65];
    hexEncode(pmkid->pmkid, 16, pmkidHex);
    macToHex(pmkid->bssid, apMac);
    macToHex(pmkid->station, staMac);
    hexEncode((const uint8_t*)pmkid->ssid, strlen(pmkid->ssid), ssidHex);

    int len = snprintf(buffer, maxLen, "WPA*01*%s*%s*%s*%s***",
                       pmkidHex, apMac, staMac, ssidHex);

    if (len > 0 && len < maxLen) return Result<uint16_t>((uint16_t)len);
    return HalError::BUFFER_TOO_SMALL;
}

Result<uint16_t> handshakeToHC22000Result(const CapturedHandshake* hs, char* buffer, uint16_t maxLen) {
    if (!hs || !buffer) return HalError::INVALID_ARGUMENT;
    if (maxLen < 500) return HalError::BUFFER_TOO_SMALL;

    int anonce_msg = -1, snonce_msg = -1;
    uint8_t message_pair = 0;

    if (hs->hasMessage(1) && hs->hasMessage(2)) {
        anonce_msg = 0; snonce_msg = 1; message_pair = 0;
    } else if (hs->hasMessage(2) && hs->hasMessage(3)) {
        anonce_msg = 2; snonce_msg = 1; message_pair = 2;
    } else if (hs->hasMessage(3) && hs->hasMessage(4)) {
        anonce_msg = 2; snonce_msg = 3; message_pair = 5;
    } else {
        return HalError::SERIALIZE_FAIL;
    }

    const EAPOLFrame* anonceFrame = &hs->frames[anonce_msg];
    const EAPOLFrame* snonceFrame = &hs->frames[snonce_msg];

    if (anonceFrame->len < 45) return HalError::SERIALIZE_FAIL;
    const uint8_t* anonce = anonceFrame->data + 13;

    if (snonceFrame->len < 93) return HalError::SERIALIZE_FAIL;
    const uint8_t* mic = snonceFrame->data + 77;

    char micHex[33], apMac[13], staMac[13], ssidHex[65], anonceHex[65];
    hexEncode(mic, 16, micHex);
    macToHex(hs->bssid, apMac);
    macToHex(hs->station, staMac);
    hexEncode((const uint8_t*)hs->ssid, strlen(hs->ssid), ssidHex);
    hexEncode(anonce, 32, anonceHex);

    uint8_t eapolCopy[512];
    memcpy(eapolCopy, snonceFrame->data, snonceFrame->len);
    memset(eapolCopy + 77, 0, 16);

    char eapolHex[1025];
    if (snonceFrame->len * 2 >= sizeof(eapolHex)) return HalError::BUFFER_TOO_SMALL;
    hexEncode(eapolCopy, snonceFrame->len, eapolHex);

    int len = snprintf(buffer, maxLen, "WPA*02*%s*%s*%s*%s*%s*%s*%02x",
                       micHex, apMac, staMac, ssidHex, anonceHex, eapolHex, message_pair);

    if (len > 0 && len < maxLen) return Result<uint16_t>((uint16_t)len);
    return HalError::BUFFER_TOO_SMALL;
}

// ==[ LEGACY WRAPPERS ]== return 0 on error for backward compat

uint32_t handshakeToPCAP(const CapturedHandshake* hs, uint8_t* buffer, uint32_t maxLen) {
    auto r = handshakeToPCAPResult(hs, buffer, maxLen);
    return r.ok() ? r.value : 0;
}

uint16_t pmkidToHC22000(const CapturedPMKID* pmkid, char* buffer, uint16_t maxLen) {
    auto r = pmkidToHC22000Result(pmkid, buffer, maxLen);
    return r.ok() ? r.value : 0;
}

uint16_t handshakeToHC22000(const CapturedHandshake* hs, char* buffer, uint16_t maxLen) {
    auto r = handshakeToHC22000Result(hs, buffer, maxLen);
    return r.ok() ? r.value : 0;
}

uint32_t exportAllHC22000(char* buffer, uint32_t maxLen) {
    if (!buffer || maxLen == 0) return 0;

    uint32_t offset = 0;
    char line[1200];

    uint16_t pmkidCount = Capture::getPMKIDCount();
    for (uint16_t i = 0; i < pmkidCount; i++) {
        CapturedPMKID pmkid;
        if (Capture::getPMKID(i, &pmkid) && !pmkid.synced) {
            uint16_t len = pmkidToHC22000(&pmkid, line, sizeof(line));
            if (len > 0 && offset + len + 2 < maxLen) {
                memcpy(buffer + offset, line, len);
                offset += len;
                buffer[offset++] = '\n';
            }
        }
    }

    uint16_t hsCount = Capture::getHandshakeCount();
    for (uint16_t i = 0; i < hsCount; i++) {
        CapturedHandshake hs;
        if (Capture::getHandshake(i, &hs) && !hs.synced) {
            uint16_t len = handshakeToHC22000(&hs, line, sizeof(line));
            if (len > 0 && offset + len + 2 < maxLen) {
                memcpy(buffer + offset, line, len);
                offset += len;
                buffer[offset++] = '\n';
            }
        }
    }

    buffer[offset] = '\0';
    return offset;
}

uint32_t getRequiredHC22000Size() {
    uint16_t pmkidCount = Capture::getUnsyncedPMKIDCount();
    uint16_t hsCount = Capture::getUnsyncedHandshakeCount();
    return (pmkidCount * 200) + (hsCount * 1200) + 100;
}

}  // namespace CaptureExport
