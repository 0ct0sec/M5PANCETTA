/**
 * C5Monster Protocol — Command/Response Definitions
 *
 * ==[ DUAL-BAND BRIDGE ]== JanOS CLI command strings and parsed response types.
 * Text-based protocol over UART at 115200 baud. Commands are newline-terminated.
 * Responses are multi-line text blocks ending with a known prompt or status line.
 */
#pragma once

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

namespace C5Protocol {

// ==[ COMMAND STRINGS ]== sent to C5Monster over UART
static constexpr const char* CMD_SCAN_NETWORKS         = "scan_networks";
static constexpr const char* CMD_INSPECT_NETWORK       = "inspect_network";
static constexpr const char* CMD_START_SNIFFER         = "start_sniffer";
static constexpr const char* CMD_START_SNIFFER_NOSCAN  = "start_sniffer_noscan";
static constexpr const char* CMD_START_SNIFFER_DOG     = "start_sniffer_dog";
static constexpr const char* CMD_START_DEAUTH          = "start_deauth";
static constexpr const char* CMD_START_EVIL_TWIN       = "start_evil_twin";
static constexpr const char* CMD_START_PORTAL          = "start_portal ";
static constexpr const char* CMD_SAE_OVERFLOW          = "sae_overflow";
static constexpr const char* CMD_START_KARMA           = "start_karma";
static constexpr const char* CMD_START_BLACKOUT        = "start_blackout";
static constexpr const char* CMD_START_WARDRIVE        = "start_wardrive";
static constexpr const char* CMD_START_WARDRIVE_PROMISC = "start_wardrive_promisc";
static constexpr const char* CMD_START_HANDSHAKE       = "start_handshake";
static constexpr const char* CMD_SAVE_HANDSHAKE        = "save_handshake";
static constexpr const char* CMD_START_BEACON_SPAM     = "start_beacon_spam";
static constexpr const char* CMD_START_ROGUEAP         = "start_rogueap";
static constexpr const char* CMD_SELECT_NETWORKS       = "select_networks";
static constexpr const char* CMD_SELECT_STATIONS       = "select_stations";
static constexpr const char* CMD_UNSELECT_NETWORKS     = "unselect_networks";
static constexpr const char* CMD_UNSELECT_STATIONS     = "unselect_stations";
static constexpr const char* CMD_WIFI_CONNECT          = "wifi_connect";
static constexpr const char* CMD_WIFI_DISCONNECT       = "wifi_disconnect";
static constexpr const char* CMD_LIST_HOSTS            = "list_hosts";
static constexpr const char* CMD_LIST_HOSTS_VENDOR     = "list_hosts_vendor";
static constexpr const char* CMD_ARP_BAN               = "arp_ban";
static constexpr const char* CMD_SCAN_BT               = "scan_bt";
static constexpr const char* CMD_SCAN_AIRTAG           = "scan_airtag";
static constexpr const char* CMD_GPS_SET               = "gps_set";
static constexpr const char* CMD_SET_GPS_POSITION      = "set_gps_position";
static constexpr const char* CMD_SET_GPS_POSITION_CAP  = "set_gps_position_cap";
static constexpr const char* CMD_START_GPS_RAW         = "start_gps_raw";
static constexpr const char* CMD_LIST_DIR              = "list_dir";
static constexpr const char* CMD_LIST_SD               = "list_sd";
static constexpr const char* CMD_FILE_DELETE           = "file_delete";
static constexpr const char* CMD_LIST_SSID             = "list_ssid";
static constexpr const char* CMD_SET_HTML              = "set_html";
static constexpr const char* CMD_SHOW_PASS             = "show_pass";
static constexpr const char* CMD_WPASEC_KEY            = "wpasec_key";
static constexpr const char* CMD_WPASEC_UPLOAD         = "wpasec_upload";
static constexpr const char* CMD_WIGLE_KEY             = "wigle_key";
static constexpr const char* CMD_WIGLE_UPLOAD          = "wigle_upload";
static constexpr const char* CMD_DEAUTH_DETECTOR       = "deauth_detector";
static constexpr const char* CMD_PACKET_MONITOR        = "packet_monitor";
static constexpr const char* CMD_CHANNEL_VIEW          = "channel_view";
static constexpr const char* CMD_CHANNEL_TIME          = "channel_time";
static constexpr const char* CMD_VENDOR                = "vendor";
static constexpr const char* CMD_DISPLAY               = "display";
static constexpr const char* CMD_BOOT_BUTTON           = "boot_button";
static constexpr const char* CMD_LED                   = "led";
static constexpr const char* CMD_OTA_CHECK             = "ota_check";
static constexpr const char* CMD_OTA_LIST              = "ota_list";
static constexpr const char* CMD_OTA_CHANNEL           = "ota_channel";
static constexpr const char* CMD_OTA_INFO              = "ota_info";
static constexpr const char* CMD_OTA_BOOT              = "ota_boot";
static constexpr const char* CMD_REBOOT                = "reboot";
static constexpr const char* CMD_PING                  = "ping";
static constexpr const char* CMD_DOWNLOAD              = "download";
static constexpr const char* CMD_STOP                  = "stop";
static constexpr const char* CMD_SHOW_SNIFFER          = "show_sniffer_results";
static constexpr const char* CMD_SHOW_SNIFFER_VENDOR   = "show_sniffer_results_vendor";
static constexpr const char* CMD_SHOW_PROBES           = "show_probes";
static constexpr const char* CMD_SHOW_PROBES_VENDOR    = "show_probes_vendor";
static constexpr const char* CMD_LIST_PROBES           = "list_probes";
static constexpr const char* CMD_LIST_PROBES_VENDOR    = "list_probes_vendor";
static constexpr const char* CMD_SNIFFER_DEBUG         = "sniffer_debug";
static constexpr const char* CMD_CLEAR_SNIFFER_RESULTS = "clear_sniffer_results";
static constexpr const char* CMD_SELECT_HTML           = "select_html ";
static constexpr const char* CMD_HELP                  = "help";

// ==[ SCAN RESULT COMMAND ]== must follow scan_networks to retrieve data
static constexpr const char* CMD_SHOW_SCAN_RESULTS  = "show_scan_results";
// Optional JanOS extension. Older firmware returns its normal command error;
// the host then keeps using scan snapshots without changing the wire grammar.
static constexpr const char* CMD_OBSERVE_BSSID      = "observe_bssid";

// ==[ RESPONSE MARKERS ]== used to detect completion / state
// JanOS CLI uses "> " (bare prompt) as the command-complete signal.
// Legacy markers are kept for backward compatibility but the prompt
// is the primary completion detector.
static constexpr const char* RESP_OK                = "OK";
static constexpr const char* RESP_ERROR             = "ERROR";
static constexpr const char* RESP_ERROR_CODE        = "Command returned non-zero error code:";
static constexpr const char* RESP_ERROR_USAGE       = "Usage:";
static constexpr const char* RESP_SCAN_IN_PROGRESS  = "Scan already in progress";
static constexpr const char* RESP_WIFI_UP           = "WiFi";
static constexpr const char* RESP_GPS_FIX           = "GPS fix";
static constexpr const char* RESP_GPS_LOST          = "GPS fix lost";
static constexpr const char* RESP_LOGGED            = "Logged";
static constexpr const char* RESP_SNIFFER_DONE      = "Sniffer stopped";
static constexpr const char* RESP_DEAUTH_DONE       = "Deauth stopped";
static constexpr const char* RESP_PORTAL_READY      = "Portal";
static constexpr const char* RESP_KARMA_READY       = "Karma";
static constexpr const char* RESP_SCAN_PRINTED      = "Scan results printed";

// ==[ SCAN RESULT PARSING ]== show_scan_results CSV format (JanOS CLI)
// Fields: index, SSID, <reserved>, BSSID, channel, auth_type, RSSI, band,
//         [latitude, longitude, hasGps]
// Examples:
//   "1","MySSID","","AA:BB:CC:DD:EE:FF","6","WPA2","-45","2.4GHz"
//   "5","","","CE:BA:BD:2E:FD:03","2","WPA2","-55","2.4GHz"
//   "10","StoreWiFi","","AA:BB:CC:DD:EE:11","44","WPA2","-61","5GHz","47.123456","8.765432","1"
struct ScanEntry {
    uint8_t sourceIndex;  // JanOS mixed-band, 1-based scan result index
    char ssid[33];
    uint8_t bssid[6];
    uint8_t channel;
    int8_t rssi;
    uint8_t authType;     // AuthType value below
    bool isHidden;
    bool is5GHz;          // channel > 14 or frequency > 2484
    double latitude;
    double longitude;
    bool hasGPS;
};

static constexpr uint8_t MAX_SCAN_ENTRIES = 48;

// ==[ OPTIONAL TARGET OBSERVATION ]==
// Backward-compatible JanOS response line:
// RF_OBS,<source_ms>,<bssid>,<channel>,<freq_mhz>,<rssi>,<noise>,
//        <samples>,<variance_dbm2>,<window_ms>,<PACKET|CSI>
enum class TargetEvidence : uint8_t {
    PACKET,
    CSI,
};

struct TargetObservation {
    uint32_t sourceTimestampMs;
    uint8_t bssid[6];
    uint8_t channel;
    uint16_t frequencyMHz;
    int8_t rssi;
    int8_t noiseFloor;
    uint16_t sampleCount;
    uint16_t varianceDbm2;
    uint32_t windowMs;
    TargetEvidence evidence;
};

static inline int hexNibble(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    return -1;
}

static inline bool parseBssidToken(const char* begin, const char* end,
                                   uint8_t out[6]) {
    if (!begin || !end || end - begin != 17) return false;
    for (uint8_t i = 0u; i < 6u; ++i) {
        const int hi = hexNibble(begin[i * 3u]);
        const int lo = hexNibble(begin[i * 3u + 1u]);
        if (hi < 0 || lo < 0) return false;
        if (i < 5u && begin[i * 3u + 2u] != ':') return false;
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

static inline bool nextCsvToken(const char*& cursor,
                                const char*& begin, const char*& end) {
    if (!cursor || !*cursor) return false;
    begin = cursor;
    while (*cursor && *cursor != ',') ++cursor;
    end = cursor;
    if (*cursor == ',') ++cursor;
    return end > begin;
}

static inline bool parseUnsignedToken(const char* begin, const char* end,
                                      uint32_t maxValue, uint32_t& out) {
    if (!begin || !end || end <= begin || end - begin >= 16) return false;
    char token[16];
    const size_t length = static_cast<size_t>(end - begin);
    memcpy(token, begin, length);
    token[length] = '\0';
    char* parsedEnd = nullptr;
    const unsigned long value = strtoul(token, &parsedEnd, 10);
    if (parsedEnd == token || *parsedEnd != '\0' || value > maxValue) {
        return false;
    }
    out = static_cast<uint32_t>(value);
    return true;
}

static inline bool parseSignedToken(const char* begin, const char* end,
                                    int32_t minValue, int32_t maxValue,
                                    int32_t& out) {
    if (!begin || !end || end <= begin || end - begin >= 16) return false;
    char token[16];
    const size_t length = static_cast<size_t>(end - begin);
    memcpy(token, begin, length);
    token[length] = '\0';
    char* parsedEnd = nullptr;
    const long value = strtol(token, &parsedEnd, 10);
    if (parsedEnd == token || *parsedEnd != '\0' ||
        value < minValue || value > maxValue) {
        return false;
    }
    out = static_cast<int32_t>(value);
    return true;
}

static inline bool parseTargetObservation(const char* line,
                                          TargetObservation& out) {
    static constexpr const char* PREFIX = "RF_OBS,";
    if (!line || strncmp(line, PREFIX, 7u) != 0) return false;
    const char* cursor = line + 7u;
    const char* begin = nullptr;
    const char* end = nullptr;
    uint32_t unsignedValue = 0u;
    int32_t signedValue = 0;
    TargetObservation parsed{};

    if (!nextCsvToken(cursor, begin, end) ||
        !parseUnsignedToken(begin, end, UINT32_MAX, unsignedValue)) {
        return false;
    }
    parsed.sourceTimestampMs = unsignedValue;
    if (!nextCsvToken(cursor, begin, end) ||
        !parseBssidToken(begin, end, parsed.bssid)) {
        return false;
    }
    if (!nextCsvToken(cursor, begin, end) ||
        !parseUnsignedToken(begin, end, 196u, unsignedValue) ||
        unsignedValue == 0u) {
        return false;
    }
    parsed.channel = static_cast<uint8_t>(unsignedValue);
    if (!nextCsvToken(cursor, begin, end) ||
        !parseUnsignedToken(begin, end, UINT16_MAX, unsignedValue)) {
        return false;
    }
    parsed.frequencyMHz = static_cast<uint16_t>(unsignedValue);
    if (!nextCsvToken(cursor, begin, end) ||
        !parseSignedToken(begin, end, -127, 0, signedValue)) {
        return false;
    }
    parsed.rssi = static_cast<int8_t>(signedValue);
    if (!nextCsvToken(cursor, begin, end) ||
        !parseSignedToken(begin, end, -127, 0, signedValue)) {
        return false;
    }
    parsed.noiseFloor = static_cast<int8_t>(signedValue);
    if (!nextCsvToken(cursor, begin, end) ||
        !parseUnsignedToken(begin, end, UINT16_MAX, unsignedValue) ||
        unsignedValue == 0u) {
        return false;
    }
    parsed.sampleCount = static_cast<uint16_t>(unsignedValue);
    if (!nextCsvToken(cursor, begin, end) ||
        !parseUnsignedToken(begin, end, UINT16_MAX, unsignedValue)) {
        return false;
    }
    parsed.varianceDbm2 = static_cast<uint16_t>(unsignedValue);
    if (!nextCsvToken(cursor, begin, end) ||
        !parseUnsignedToken(begin, end, 60000u, unsignedValue) ||
        unsignedValue == 0u) {
        return false;
    }
    parsed.windowMs = unsignedValue;
    if (!nextCsvToken(cursor, begin, end) || *cursor != '\0') return false;
    const size_t evidenceLength = static_cast<size_t>(end - begin);
    if (evidenceLength == 6u && strncmp(begin, "PACKET", 6u) == 0) {
        parsed.evidence = TargetEvidence::PACKET;
    } else if (evidenceLength == 3u && strncmp(begin, "CSI", 3u) == 0) {
        parsed.evidence = TargetEvidence::CSI;
    } else {
        return false;
    }

    out = parsed;
    return true;
}

// ==[ DEAUTH ALERT ]== parsed from sniffer/deauth output
struct DeauthAlert {
    uint8_t bssid[6];
    uint8_t clientMac[6];
    uint8_t channel;
    int8_t rssi;
    uint16_t reasonCode;
};

// ==[ WARDRIVE ENTRY ]== parsed from wardrive output lines
struct WardriveEntry {
    char ssid[33];
    uint8_t bssid[6];
    uint8_t channel;
    int8_t rssi;
    uint8_t authType;
    double latitude;
    double longitude;
    bool hasGPS;
};

// ==[ AUTH TYPE PARSE ]==
enum AuthType : uint8_t {
    AUTH_OPEN = 0,
    AUTH_WEP = 1,
    AUTH_WPA = 2,
    AUTH_WPA2 = 3,
    AUTH_WPA3 = 4,
    AUTH_WPA_WPA2_MIXED = 5,
    AUTH_WPA2_WPA3_MIXED = 6,
    AUTH_UNKNOWN = 7
};

static inline uint8_t parseAuthType(const char* s) {
    if (!s || !s[0]) return AUTH_UNKNOWN;
    // Match transition modes before their component substrings. Collapsing
    // WPA2/WPA3 to pure WPA3 makes Recon and Wardrive overstate protection.
    if (strstr(s, "WPA2/WPA3") != nullptr) return AUTH_WPA2_WPA3_MIXED;
    if (strstr(s, "WPA/WPA2") != nullptr) return AUTH_WPA_WPA2_MIXED;
    if (strstr(s, "WPA3") != nullptr) return AUTH_WPA3;
    if (strstr(s, "WPA2") != nullptr) return AUTH_WPA2;
    if (strstr(s, "WPA") != nullptr) return AUTH_WPA;
    if (strstr(s, "WEP") != nullptr) return AUTH_WEP;
    if (strstr(s, "OPEN") != nullptr || strstr(s, "OPN") != nullptr) return AUTH_OPEN;
    return AUTH_UNKNOWN;
}

static inline const char* authTypeLabel(uint8_t authType) {
    switch (authType) {
        case AUTH_OPEN: return "OPEN";
        case AUTH_WEP: return "WEP";
        case AUTH_WPA: return "WPA";
        case AUTH_WPA2: return "WPA2";
        case AUTH_WPA3: return "WPA3";
        case AUTH_WPA_WPA2_MIXED: return "WPA+2";
        case AUTH_WPA2_WPA3_MIXED: return "W2+3";
        default: return "UNK";
    }
}

static inline bool isChannel5GHz(uint8_t ch) {
    return ch > 14;
}

static inline bool isChannel5GHzbyFreq(uint16_t freq) {
    return freq > 2484;
}

} // namespace C5Protocol
