/**
 * NoirNarrator — hardboiled cyberpunk noir phrase library
 *
 * ==[ VOICE OF THE PIG ]== 120+ phrases across 11 categories.
 * station-aware ambient narration + event reactions + settings RF explanations.
 * all .rodata — no heap alloc. ~7.5KB flash footprint.
 *
 * voice rules:
 *   - terse. short lines. max 6 words per line, 4 lines max.
 *   - hardboiled detective meets cyberpunk hacker.
 *   - technical details woven into noir metaphor.
 *   - no exclamation marks. period or ellipsis only.
 *   - lowercase unless acronym or protocol name.
 */

#include "noir_narrator.h"
#include "recon.h"
#include "xband.h"
#include "defense_pipeline.h"
#include "../core/capture.h"
#include "../core/config.h"
#include "../hamlet.h"
#include "../activity/pedometer.h"
#include <M5Unified.h>
#include <esp_random.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>

namespace NoirNarrator {

// ==[ SELECTION UTIL ]== random with no immediate repeat
static int lastPhraseIdx[4] = { -1, -1, -1, -1 };
static int histPos = 0;

static const char* pick(const char* const* phrases, int count) {
    if (count <= 0) return "";
    if (count == 1) return phrases[0];

    int idx;
    int attempts = 0;
    do {
        idx = esp_random() % count;
        bool dup = false;
        for (int i = 0; i < 4; i++) {
            if (lastPhraseIdx[i] == idx) { dup = true; break; }
        }
        if (!dup) break;
    } while (++attempts < 8);

    lastPhraseIdx[histPos] = idx;
    histPos = (histPos + 1) % 4;
    return phrases[idx];
}

// ==[ FORMAT BUFFER ]==
static char fmtBuf[96];

const char* format(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(fmtBuf, sizeof(fmtBuf), fmt, args);
    va_end(args);
    return fmtBuf;
}

// ============================================================
// ==[ STATION IDLE PHRASES ]== ambient narration per station
// ============================================================

// AT_LAPTOP — cyberdeck lab
static const char* const LAPTOP_PHRASES[] = {
    "four frames wanted.\npartial handshakes bring fewer.\nprotocol has standards.",
    "vendor OUI table.\nMAC prefixes name makers.\nnot owners.",
    "cursor blinking.\ncallbacks choose arrival times.\nfirmware chose everything else.",
    "MAC prefix checked.\nvendor probable.\nowner still unknown.",
    "logs keep timestamps.\nclocks keep alibis.\nboth need cross-checking.",
    "BSSID gets a row.\nchannel gets a column.\nevidence likes forms.",
    "keyboard warm.\npacket filter colder.\nnoise gets questioned.",
    "scan report template.\nempty fields stay empty.\nzero invents nothing.",
    "terminal buffer blinking.\nwire has testimony.\npancetta takes notes.",
    "M1 still needs M2.\npartial handshakes stall.\ncold-case protocol.",
    "grep questions noise.\nsignal requests counsel.",
    "hex dump scrolling.\nfields cross-checked.\nbytes keep receipts.",
    "filter rebuilt.\nbeacons enter.\nexcuses do not.",
    "route tables list hops.\neach signs\na different alibi.",
};
static constexpr int LAPTOP_COUNT = sizeof(LAPTOP_PHRASES) / sizeof(LAPTOP_PHRASES[0]);

// AT_WINDOW — noir apartment
static const char* const WINDOW_PHRASES[] = {
    "rain on glass.\n2.4 GHz through glass.\nweather lacks firewall.",
    "phones below.\nprobe requests above.\nintent stays unproven.",
    "glass attenuates.\nRSSI changes anyway.\nrange needs witnesses.",
    "neon in puddles.\nMACs in cache.\nboth leave traces.",
    "third floor.\nRSSI ignores floor plans.\nphysics wants measurements.",
    "glass stays still.\nscanner changes channels.\ncity changes stories.",
    "blinds half-open.\nBSSID still broadcasts.\nprivacy chose curtains.",
    "streetlight flickers.\nbeacons keep intervals.\ntwo clocks, one alley.",
    "directed probes below.\nSSID names spoken\nwithout counsel.",
    "cold glass.\nradio warm.\nRF crosses anyway.",
    "rooftop antennas.\nownership unknown.\nno verdict without evidence.",
    "empty-looking alley.\nBLE still advertises.",
};
static constexpr int WINDOW_COUNT = sizeof(WINDOW_PHRASES) / sizeof(WINDOW_PHRASES[0]);

// COOKING — ramen bar
static const char* const COOKING_PHRASES[] = {
    "ramen boiling.\n2.4 GHz crowded.\nboth need watching.",
    "broth stirred.\nscan log sorted.\nonly one feeds detectives.",
    "noodles for one.\nAPs crowd thirteen channels.\nmath stays rude.",
    "steam fades.\nRSSI fades.\nonly one is edible.",
    "broth takes time.\npassive scans take longer.\nno shortcuts filed.",
    "salt. MSG. entropy.\ncase recipe stays classified.",
    "ramen at 03:00.\nchannel 6 still busy.\nusual crowd.",
    "chopsticks grip noodles.\nfilters sort packets.\nboth need practice.",
    "kitchen timer running.\nscan timer running.\nwatchdog unimpressed.",
    "one bowl.\nmany BSSIDs.\nno seating plan.",
    "broth needs simmer.\nradio needs dwell.\npatience does both.",
    "intel cools fast.\nramen does too.\nserve the facts.",
};
static constexpr int COOKING_COUNT = sizeof(COOKING_PHRASES) / sizeof(COOKING_PHRASES[0]);

// ON_SOFA — keep brief, pig is resting
static const char* const SOFA_PHRASES[] = {
    "zzz...",
    "* snrk *",
    "radio off.\ncouch on.",
    "five more minutes.\ncase can wait.",
    "dreaming in\ncrackable EAPOL pairs.",
    "* snore *",
    "between scans.\nfeet up.\nantenna down.",
    "couch has custody\nof the detective.",
};
static constexpr int SOFA_COUNT = sizeof(SOFA_PHRASES) / sizeof(SOFA_PHRASES[0]);

// IN_BED — coffin pod, minimal
static const char* const BED_PHRASES[] = {
    "zzz...",
    "* snrk *",
    "pod sealed.\nradio dark.\ncase suspended.",
    "lights out.\nWiFi quiet.\npancetta offline.",
    "* snore *",
    "deep sleep.\nno callbacks admitted.",
};
static constexpr int BED_COUNT = sizeof(BED_PHRASES) / sizeof(BED_PHRASES[0]);

// AT_ANTENNA — surveillance nest, signal hunting
static const char* const ANTENNA_PHRASES[] = {
    "antenna ready.\nchannels enter lineup.",
    "each frequency gets dwell.\nverdict waits.",
    "dish listening.\nbeacons speak in intervals.",
    "RSSI clears a threshold.\nthen gets cross-examined.",
    "dwell before verdict.\nwire gets time\nto contradict itself.",
    "rooftop watch.\nchannel map waiting.",
    "probe requests name SSIDs.\nwitness volunteers too much.",
    "2.4 GHz.\nthirteen channels.\none tiny courtroom.",
    "one detective.\none shared radio.\nFreeRTOS runs docket.",
    "BSSID enters ledger.\nOUI offers maker.\nnot owner.",
};
static constexpr int ANTENNA_COUNT = sizeof(ANTENNA_PHRASES) / sizeof(ANTENNA_PHRASES[0]);

// ON_LEDGE — rooftop edge, contemplative
static const char* const LEDGE_PHRASES[] = {
    "city below.\nchannel map above.\npancetta between.",
    "wind clears smoke.\nnot the spectrum.",
    "skyline lit.\nAPs beacon below.",
    "feet over ledge.\nRSSI under review.",
    "traffic below.\nprobe traffic above.",
    "roofline view.\nmostly rain.\nall RF.",
    "quiet roof.\nnoisy 2.4 GHz.",
    "every lit window\nlooks like an AP.\nscan decides.",
    "band survey open.\ncity refuses comment.",
    "city hums.\nchannel 6 overacts.",
};
static constexpr int LEDGE_COUNT = sizeof(LEDGE_PHRASES) / sizeof(LEDGE_PHRASES[0]);

// AT_TERMINAL — underground bar, data review
static const char* const TERMINAL_PHRASES[] = {
    "PMKID or handshake.\nlabel the evidence.\nnever mix bags.",
    "PCAP validation.\nweak records rejected.\nchain stays clean.",
    "SRAM wants speed.\nPSRAM wants evidence.\nheap wants contiguous rooms.",
    "terminal prints receipts.\nfirmware reads them.",
    "evidence review.\nBSSID by BSSID.",
    "PMKIDs.\nhandshakes.\nseparate evidence bags.",
    "every byte\ngets a case number.",
    "vendor OUI parsed.\nidentity remains probable.",
    "forensic-grade PCAP.\ncartoon pig terminal.\nchain still matters.",
    "inventory before export.\nmissing captures stay missing.",
};
static constexpr int TERMINAL_COUNT = sizeof(TERMINAL_PHRASES) / sizeof(TERMINAL_PHRASES[0]);

// AT_BOOTH — underground bar, noir detective mode
static const char* const BOOTH_PHRASES[] = {
    "back booth.\nwall behind.\nexit visible.",
    "case terms discussed\nunder low light.",
    "information changes hands.\nreceipts stay.",
    "glass cooling.\ncase warming.",
    "quiet booth.\nopen notebook.\nclosed mouth.",
    "every handshake\nneeds corroboration.",
    "pendant light flickers.\nBarman does not.",
    "evidence cards down.\nall sources named.",
    "detective enters.\nbar keeps counsel.",
    "hologram turns.\ncase theory follows.",
    "hamlet pancetta.\nprivate investigator.\n2.4 GHz beat.",
    "pancetta takes\nthe corner booth.\ncase open.",
};
static constexpr int BOOTH_COUNT = sizeof(BOOTH_PHRASES) / sizeof(BOOTH_PHRASES[0]);

const char* getStationPhrase(uint8_t station) {
    switch (station) {
        case STATION_LAPTOP:   return pick(LAPTOP_PHRASES, LAPTOP_COUNT);
        case STATION_SOFA:     return pick(SOFA_PHRASES, SOFA_COUNT);
        case STATION_WINDOW:   return pick(WINDOW_PHRASES, WINDOW_COUNT);
        case STATION_COOKING:  return pick(COOKING_PHRASES, COOKING_COUNT);
        case STATION_BED:      return pick(BED_PHRASES, BED_COUNT);
        case STATION_ANTENNA:  return pick(ANTENNA_PHRASES, ANTENNA_COUNT);
        case STATION_LEDGE:    return pick(LEDGE_PHRASES, LEDGE_COUNT);
        case STATION_TERMINAL: return pick(TERMINAL_PHRASES, TERMINAL_COUNT);
        case STATION_BOOTH:    return pick(BOOTH_PHRASES, BOOTH_COUNT);
        default: return "";
    }
}

// ============================================================
// ==[ TRACKER DETECTION PHRASES ]==
// ============================================================

static const char* const AIRTAG_PHRASES[] = {
    "FindMy payload.\nrotating key.\nshadow gets a file.",
    "manufacturer 0x004C.\nsubtype 0x12.\nAirTag enters lineup.",
    "FindMy beacon.\nluggage or lookout.\nevidence cannot infer motive.",
    "Apple manufacturer data.\nidentity rotates.\nradio habit remains.",
    "0x004C subtype 0x12.\nFindMy match.\nsighting logged.",
};
static constexpr int AIRTAG_COUNT = sizeof(AIRTAG_PHRASES) / sizeof(AIRTAG_PHRASES[0]);

static const char* const SMARTTAG_PHRASES[] = {
    "service UUID 0xFD5A.\nSamsung SmartTag.\nradio named itself.",
    "0xFD5A service.\nSmartTag identified.\npayload becomes identity.",
    "SmartTag service data.\nidentity hash logged.\ncase open.",
    "Samsung beacon.\nservice data testified.\nintent stayed silent.",
};
static constexpr int SMARTTAG_COUNT = sizeof(SMARTTAG_PHRASES) / sizeof(SMARTTAG_PHRASES[0]);

static const char* const TILE_PHRASES[] = {
    "0xFEED service.\nTile tracker.\nold protocol, fresh sighting.",
    "Tile beacon.\ncrowd network listening.\nintent unknown.",
    "Tile in range.\nluggage possible.\npattern decides.",
};
static constexpr int TILE_COUNT = sizeof(TILE_PHRASES) / sizeof(TILE_PHRASES[0]);

static const char* const FASTPAIR_PHRASES[] = {
    "Fast Pair service.\n0xFE2C.\naccessory enters ledger.",
    "0xFE2C beacon.\nmodel data spoke.\nintent stayed private.",
};
static constexpr int FASTPAIR_COUNT = sizeof(FASTPAIR_PHRASES) / sizeof(FASTPAIR_PHRASES[0]);

static const char* const GENERIC_TRACKER_PHRASES[] = {
    "recognized BLE beacon.\npayload indexed.\nintent not inferred.",
    "BLE signature matched.\nidentity enters table.\ncase open.",
};
static constexpr int GENERIC_COUNT = sizeof(GENERIC_TRACKER_PHRASES) / sizeof(GENERIC_TRACKER_PHRASES[0]);

static const char* const FLIPPER_PHRASES[] = {
    "Flipper Zero signature.\nradio tool nearby.\nactivity logged.",
    "known BLE pattern.\nFlipper Zero probable.\nevidence, not motive.",
    "UUID, OUI, or name.\nFlipper match.\nintent unknown.",
};
static constexpr int FLIPPER_COUNT = sizeof(FLIPPER_PHRASES) / sizeof(FLIPPER_PHRASES[0]);

static const char* const HID_PHRASES[] = {
    "UUID 0x1812.\nBLE HID service.\nownership decides risk.",
    "input device nearby.\nHID service exposed.\nintent not inferred.",
};
static constexpr int HID_COUNT = sizeof(HID_PHRASES) / sizeof(HID_PHRASES[0]);

const char* getTrackerPhrase(Recon::ThreatType type) {
    switch (type) {
        case Recon::ThreatType::AIRTAG:    return pick(AIRTAG_PHRASES, AIRTAG_COUNT);
        case Recon::ThreatType::SMARTTAG:  return pick(SMARTTAG_PHRASES, SMARTTAG_COUNT);
        case Recon::ThreatType::TILE:      return pick(TILE_PHRASES, TILE_COUNT);
        case Recon::ThreatType::FAST_PAIR: return pick(FASTPAIR_PHRASES, FASTPAIR_COUNT);
        case Recon::ThreatType::FLIPPER:   return pick(FLIPPER_PHRASES, FLIPPER_COUNT);
        case Recon::ThreatType::HID_DEVICE: return pick(HID_PHRASES, HID_COUNT);
        case Recon::ThreatType::SMARTTAG_UNREGISTERED: return pick(SMARTTAG_PHRASES, SMARTTAG_COUNT);
        case Recon::ThreatType::XIAOMI_TRACKER:
        case Recon::ThreatType::SIDEWALK_BEACON:
        case Recon::ThreatType::EXPOSURE_NOTIF:
        case Recon::ThreatType::FMDN:
        case Recon::ThreatType::IBEACON:
        case Recon::ThreatType::EDDYSTONE:
        case Recon::ThreatType::APPLE_NEARBY:
        case Recon::ThreatType::SUSPICIOUS_PERIPHERAL:
        default:                           return pick(GENERIC_TRACKER_PHRASES, GENERIC_COUNT);
    }
}

// ============================================================
// ==[ FOLLOWING DETECTION ]==
// ============================================================

static const char* const FOLLOWING_PHRASES[] = {
    "same tag.\n%s now.\ncoincidence lost counsel.",
    "persistent tag.\n%s.\nwe have a tail.",
    "tag remains.\n%s.\nproximity needs review.",
    "same beacon.\n%s.\nfollowing pattern holds.",
    "repeat sightings.\n%s.\ncase escalated.",
    "same signal.\n%s.\nambient theory rejected.",
    "still present.\n%s.\npattern confirmed.",
    "tag persists.\n%s.\nnot background traffic.",
    "moving with us.\n%s.\ncorrelation stays high.",
    "consistent return.\n%s.\nchance loses the vote.",
};
static constexpr int FOLLOWING_COUNT = sizeof(FOLLOWING_PHRASES) / sizeof(FOLLOWING_PHRASES[0]);

const char* getFollowingPhrase(const char* detail) {
    const char* tmpl = pick(FOLLOWING_PHRASES, FOLLOWING_COUNT);
    return format(tmpl, detail ? detail : "long time");
}

// ============================================================
// ==[ BLE SPAM ]==
// ============================================================

static const char* const SPAM_PHRASES[] = {
    "rapid MAC rotation.\nspam signature logged.",
    "rapid BLE churn.\nidentities expire\nevery heartbeat.",
    "BLE spam flood.\nsynthetic advertisers.\nairtime gets crowded.",
    "MAC count spiked.\nprivacy exception rejected.\nchurn left receipts.",
    "synthetic MACs cycling.\nspam gate tripped.\ncase open.",
};
static constexpr int SPAM_COUNT = sizeof(SPAM_PHRASES) / sizeof(SPAM_PHRASES[0]);

const char* getBleSpamPhrase() {
    return pick(SPAM_PHRASES, SPAM_COUNT);
}

// ============================================================
// ==[ COORDINATED ATTACK ]==
// ============================================================

static const char* const COORDINATED_PHRASES[] = {
    "cross-band correlation.\n%s.\ncoordination suspected.",
    "two radio events.\n%s.\none timeline.",
    "%s.\nBLE and WiFi aligned.\nproximity, not identity.",
};
static constexpr int COORDINATED_COUNT = sizeof(COORDINATED_PHRASES) / sizeof(COORDINATED_PHRASES[0]);

const char* getCoordinatedAttackPhrase(const char* detail) {
    const char* fmt = pick(COORDINATED_PHRASES, COORDINATED_COUNT);
    return format(fmt, detail);
}

// ============================================================
// ==[ EVIL TWIN ]==
// ============================================================

static const char* const EVIL_TWIN_PHRASES[] = {
    "same SSID.\ndifferent BSSID.\nevil twin suspected.",
    "'%s' repeats\nacross two BSSIDs.\ntrust neither.",
    "duplicate SSID.\ndifferent hardware.\ncredentials may be bait.",
    "twin APs.\none name.\nBSSIDs break the tie.",
};
static constexpr int EVIL_TWIN_COUNT = sizeof(EVIL_TWIN_PHRASES) / sizeof(EVIL_TWIN_PHRASES[0]);

const char* getEvilTwinPhrase(const char* ssid) {
    const char* tmpl = pick(EVIL_TWIN_PHRASES, EVIL_TWIN_COUNT);
    if (strchr(tmpl, '%')) {
        return format(tmpl, ssid ? ssid : "?");
    }
    return tmpl;
}

// ============================================================
// ==[ KARMA HONEYPOT ]==
// ============================================================

static const char* const KARMA_PHRASES[] = {
    "one BSSID.\nmany SSIDs.\nKARMA behavior suspected.",
    "same BSSID claims\n'%s' and others.\nKARMA case.",
    "one AP.\nmultiple SSIDs.\nalibi collapsed.",
    "KARMA responder.\nany requested SSID\ngets an answer.",
};
static constexpr int KARMA_COUNT = sizeof(KARMA_PHRASES) / sizeof(KARMA_PHRASES[0]);

const char* getKarmaPhrase(const char* ssid) {
    const char* tmpl = pick(KARMA_PHRASES, KARMA_COUNT);
    if (strchr(tmpl, '%')) {
        return format(tmpl, ssid ? ssid : "?");
    }
    return tmpl;
}

// ============================================================
// ==[ KNOWN AP (POTFILE MATCH) ]==
// ============================================================

static const char* const KNOWN_AP_PHRASES[] = {
    "potfile match.\n%s.\ncredential already on file.",
    "'%s' in range.\nprior crack confirmed.\nold case.",
    "familiar signal.\n%s.\npotfile remembers.",
    "known SSID.\n%s.\ncold file reopened.",
    "SSID recognized.\n%s.\nprior evidence attached.",
};
static constexpr int KNOWN_AP_COUNT = sizeof(KNOWN_AP_PHRASES) / sizeof(KNOWN_AP_PHRASES[0]);

const char* getKnownAPPhrase(const char* ssid) {
    const char* tmpl = pick(KNOWN_AP_PHRASES, KNOWN_AP_COUNT);
    return format(tmpl, ssid ? ssid : "???");
}

// ============================================================
// ==[ OPEN AP WARNING ]==
// ============================================================

static const char* const OPEN_AP_PHRASES[] = {
    "%d open networks.\nno encryption.\nintent remains unknown.",
    "%d APs.\nno lock.\naccess is not permission.",
    "%d unencrypted APs.\nopen air, closed consent.",
    "%d doors open.\nframes travel clear.\nauthorization still required.",
};
static constexpr int OPEN_AP_COUNT = sizeof(OPEN_AP_PHRASES) / sizeof(OPEN_AP_PHRASES[0]);

const char* getOpenAPPhrase(uint8_t count) {
    const char* tmpl = pick(OPEN_AP_PHRASES, OPEN_AP_COUNT);
    return format(tmpl, (int)count);
}

// ============================================================
// ==[ TOOL IDENTIFIED ]==
// ============================================================

static const char* const TOOL_ID_PHRASES[] = {
    "attack behavior matched.\n%s.\nsignature gets a file.",
    "signature match: %s.\ntool identified.\nsource still unknown.",
    "%s on the wire.\nbehavior supplied\nthe fingerprint.",
    "tool classified.\n%s.\nmethod has a name.",
};
static constexpr int TOOL_ID_COUNT = sizeof(TOOL_ID_PHRASES) / sizeof(TOOL_ID_PHRASES[0]);

const char* getToolIdentifiedPhrase(const char* toolName) {
    const char* tmpl = pick(TOOL_ID_PHRASES, TOOL_ID_COUNT);
    return format(tmpl, toolName ? toolName : "unknown");
}

// ============================================================
// ==[ HOSTILE CLIENT ]==
// ============================================================

static const char* const HOSTILE_CLIENT_PHRASES[] = {
    "hostile probe score.\n%s.\nclient under review.",
    "%s on the air.\ncapability IEs went missing.\nprofile looks tool-like.",
    "probe heuristic.\n%s.\nscore, not identity.",
    "suspicious probe pattern.\n%s.\ndistance stays prudent.",
};
static constexpr int HOSTILE_CLIENT_COUNT = sizeof(HOSTILE_CLIENT_PHRASES) / sizeof(HOSTILE_CLIENT_PHRASES[0]);

const char* getHostileClientPhrase(const char* label) {
    const char* tmpl = pick(HOSTILE_CLIENT_PHRASES, HOSTILE_CLIENT_COUNT);
    return format(tmpl, label ? label : "unknown");
}

// ============================================================
// ==[ QUIET WIRE ]==
// ============================================================

static const char* const QUIET_PHRASES[] = {
    "no tags.\nno twins.\nno spam this pass.",
    "alert ledger empty.\ncurrent scan only.",
    "BLE alerts: zero.\nWiFi alerts: zero.\nreceipt has limits.",
    "sweep filed.\ntracker table empty.\ncase rests.",
    "radio found no threats.\nabsence is not clearance.",
    "BLE table clear.\nWiFi alerts clear.\nthis pass only.",
    "scan closed.\nno alerts filed.",
    "alert ledger empty.\nquiet case.\ncurrent scan only.",
};
static constexpr int QUIET_COUNT = sizeof(QUIET_PHRASES) / sizeof(QUIET_PHRASES[0]);

const char* getQuietPhrase() {
    return pick(QUIET_PHRASES, QUIET_COUNT);
}

// ============================================================
// ==[ SCAN ACTIVE ]==
// ============================================================

static const char* const SCAN_PHRASES[] = {
    "scanning...",
    "RF sweep.\nBLE listening.",
    "WiFi scan active.\nchannels giving statements.",
    "checking BLE\nand WiFi.",
    "RF survey active.\nresults pending.",
};
static constexpr int SCAN_COUNT = sizeof(SCAN_PHRASES) / sizeof(SCAN_PHRASES[0]);

// ==[ RANK PHRASES ]== 20% chance: rank-gated commentary
static const char* getRankPhrase() {
    uint8_t level = Config::getLevel();
    if (level >= 42) return "level 42.\ncaseboard fully lit.\nwire still wants receipts.";
    if (level >= 35) return "level 35.\nwire knows the badge.\nstill checks credentials.";
    if (level >= 28) return "level 28.\npatterns confess sooner.\nusually to logs.";
    if (level >= 21) return "level 21.\nreceipts stay indexed.\ncoffee does not.";
    if (level >= 14) return "level 14.\nchannels look familiar.\nchannel 6 lies.";
    if (level >= 7)  return "level 7.\nlearning the beat.\nwire charges tuition.";
    return nullptr;
}

const char* getScanActivePhrase() {
    // 20% chance: inject rank-gated phrase
    if ((esp_random() % 5) == 0) {
        const char* rp = getRankPhrase();
        if (rp) return rp;
    }
    return pick(SCAN_PHRASES, SCAN_COUNT);
}

// ============================================================
// ==[ SETTINGS RF EXPLANATIONS ]==
// ============================================================

// indexed loosely by setting concept — called with setting index
// these are for the menu helper pig bubble
static const char* const SETTING_EXPLANATIONS[] = {
    // 0: MUDBALL (deauth)
    "transmits deauth frames.\nclients may disconnect.\nwritten scope only.",
    // 1: PIG_ANGRY (aggression)
    "MUDBALL intensity.\nhigher sends more frames\nper selected target.\nscope stays unchanged.",
    // 2: EAPOL_INJ (injection)
    "EAPOL Start/Logoff.\ndata frames can interrupt\nPMF client sessions.\nwritten scope only.",
    // 3: CSA_HERD
    "CSA announcement frames.\nasks clients to change\nchannel. compliance varies.\ntransmit requires scope.",
    // 4: AUTH_FLOOD
    "spoofed auth requests.\nmultiple fake clients.\nloud airtime pressure.\nlab scope only.",
    // 5: SAE_ATTACK
    "spoofed SAE rejection.\ntransition AP may fall\nback to WPA2.\nresult not guaranteed.",
    // 6: AUTO_PROBE
    "directed auth + assoc.\nrequests an RSN response.\nactive transmission.\nown the AP.",
    // 7: PROBE_RSSI
    "probe RSSI floor.\n-60 dBm is stricter.\n-90 dBm admits noise.\nmeasure your room.",
    // 8: COORD_ROLE
    "hunt coordination role.\nmaster publishes targets.\nslave follows selection.\nstandalone hunts alone.",
    // 9: RECON
    "Recon rotates BLE,\nWiFi scans, and deauth\nsniff windows.\nradio work is real.",
    // 10: IPP_ENABLED
    "IPP background defense.\nBLE + WiFi evidence.\ntrackers, twins, spam.\nalerts enter caseboard.",
    // 11: BLE_SCAN
    "BLE scan window.\npassive unless ACT set.\n8s about every 60s.\nSCAN_REQ when active.",
    // 12: WIFI_SCAN
    "WiFi environment scan.\nevil twins, KARMA,\nknown APs, crowd state.\nscan may transmit probes.",
};
static constexpr int EXPLANATION_COUNT = sizeof(SETTING_EXPLANATIONS) / sizeof(SETTING_EXPLANATIONS[0]);

const char* getSettingExplanation(uint8_t settingIndex) {
    if (settingIndex < EXPLANATION_COUNT) {
        return SETTING_EXPLANATIONS[settingIndex];
    }
    return nullptr;
}

// ============================================================
// ==[ CONTEXT-AWARE PHRASES ]== live data narration
// ============================================================

// station-flavored templates with live data slots
// voice: laptop=analytical, window=observational, cooking=metaphorical, sofa/bed=drowsy

// ==[ FOLLOWING — urgent, any station ]==
static const char* const CTX_FOLLOWING[] = {
    "same tag.\n%s.\nstill attached.",
    "persistent tail.\n%s.\nmovement correlated.",
    "shadow holds.\n%s.\ndistance matters.",
    "following flag.\n%s.\ncase escalated.",
    "tag remains.\n%s.\nnot ambient.",
    "moving with us.\n%s.\ncorrelation confirmed.",
    "tag persists.\n%s.\nwatch the route.",
    "no separation.\n%s.\nkeep evidence moving.",
};
static constexpr int CTX_FOLLOWING_COUNT = sizeof(CTX_FOLLOWING) / sizeof(CTX_FOLLOWING[0]);

// ==[ XBAND: ATTACKER IDENTIFIED ]==
static const char* const CTX_XBAND_ATTACKER[] = {
    "XBand match.\nBLE fingerprint fits\nthe deauth source.",
    "cross-band receipt.\ndevice and attack\nshare a timeline.",
    "BLE plus WiFi.\nsource correlation high.\none case.",
    "hardware catalog match.\nattack source probable.",
};
static constexpr int CTX_XBAND_ATTACKER_COUNT = sizeof(CTX_XBAND_ATTACKER) / sizeof(CTX_XBAND_ATTACKER[0]);

// ==[ XBAND: DUAL-BAND STALK ]==
static const char* const CTX_XBAND_DUALBAND[] = {
    "two bands.\none tail.\nXBand correlation holds.",
    "WiFi probes.\nBLE tail.\nsame moving shadow.",
    "persistent on WiFi.\npersistent on BLE.\ncorrelation rises.",
    "dual-band follow.\nXBand links the sightings.",
};
static constexpr int CTX_XBAND_DUALBAND_COUNT = sizeof(CTX_XBAND_DUALBAND) / sizeof(CTX_XBAND_DUALBAND[0]);

// ==[ XBAND: COHORT PAIR ]==
static const char* const CTX_XBAND_COHORT[] = {
    "WiFi and BLE paired.\nlikely one kit.\none route.",
    "correlated cohort.\nBLE device and\nWiFi client co-travel.",
};
static constexpr int CTX_XBAND_COHORT_COUNT = sizeof(CTX_XBAND_COHORT) / sizeof(CTX_XBAND_COHORT[0]);

// ==[ PROBE-VULN CLIENT ]==
static const char* const CTX_PROBE_VULN[] = {
    "vulnerable client probing.\nknown SSID exposed.\ncase noted.",
    "client seeks\na cracked network.\nprobe gives it away.",
    "probe request names\na known network.\nprivacy leaked.",
    "client calls\na compromised AP.\nopen testimony.",
};
static constexpr int CTX_PROBE_VULN_COUNT = sizeof(CTX_PROBE_VULN) / sizeof(CTX_PROBE_VULN[0]);

// ==[ LAPTOP — analytical ]==
static const char* const CTX_LAPTOP_THREAT[] = {
    "%d trackers logged.\n%d APs mapped.\nthreat ledger open.",
    "%d tags catalogued.\n%d networks charted.\ncaseboard filling.",
    "threat file current.\n%d trackers.\n%d APs.",
};
static constexpr int CTX_LAPTOP_THREAT_COUNT = sizeof(CTX_LAPTOP_THREAT) / sizeof(CTX_LAPTOP_THREAT[0]);

static const char* const CTX_LAPTOP_SCAN[] = {
    "%d tags. %d APs.\n%d open.\nsweep receipt filed.",
    "scan report.\n%d tags. %d APs.\n%d open.",
    "fresh evidence.\n%d trackers. %d networks.\n%d unencrypted.",
};
static constexpr int CTX_LAPTOP_SCAN_COUNT = sizeof(CTX_LAPTOP_SCAN) / sizeof(CTX_LAPTOP_SCAN[0]);

static const char* const CTX_LAPTOP_AMBIENT[] = {
    "%d BLE this session.\n%d captures filed.\nlaptop keeps receipts.",
    "%d devices logged.\n%.1f km walked.\ncasework has mileage.",
    "%d APs in range.\n%d known.\nold files reopen.",
};
static constexpr int CTX_LAPTOP_AMBIENT_COUNT = sizeof(CTX_LAPTOP_AMBIENT) / sizeof(CTX_LAPTOP_AMBIENT[0]);

// ==[ WINDOW — observational ]==
static const char* const CTX_WINDOW_THREAT[] = {
    "%d trackers below.\n%d APs beyond glass.\ncity leaves prints.",
    "shadows counted.\n%d trackers.\n%d APs exposed.",
};
static constexpr int CTX_WINDOW_THREAT_COUNT = sizeof(CTX_WINDOW_THREAT) / sizeof(CTX_WINDOW_THREAT[0]);

static const char* const CTX_WINDOW_SCAN[] = {
    "%d tags below.\n%d APs.\n%d open doors.",
    "glass-side sweep.\n%d tags. %d APs.\n%d open.",
};
static constexpr int CTX_WINDOW_SCAN_COUNT = sizeof(CTX_WINDOW_SCAN) / sizeof(CTX_WINDOW_SCAN[0]);

static const char* const CTX_WINDOW_AMBIENT[] = {
    "radio ledger: %d.\nblinds hold the line.",
    "%d signals indexed.\nwindow keeps watch.",
};
static constexpr int CTX_WINDOW_AMBIENT_COUNT = sizeof(CTX_WINDOW_AMBIENT) / sizeof(CTX_WINDOW_AMBIENT[0]);

// ==[ COOKING — metaphorical ]==
static const char* const CTX_COOKING_THREAT[] = {
    "field ledger.\n%d trackers. %d APs.\ncase runs hot.",
    "threat count.\n%d trackers. %d networks.\nradio stays busy.",
};
static constexpr int CTX_COOKING_THREAT_COUNT = sizeof(CTX_COOKING_THREAT) / sizeof(CTX_COOKING_THREAT[0]);

static const char* const CTX_COOKING_SCAN[] = {
    "fresh scan.\n%d tags. %d APs.\n%d open.",
    "scan receipt.\n%d tags. %d APs.\n%d unencrypted.",
};
static constexpr int CTX_COOKING_SCAN_COUNT = sizeof(CTX_COOKING_SCAN) / sizeof(CTX_COOKING_SCAN[0]);

static const char* const CTX_COOKING_AMBIENT[] = {
    "network ledger.\n%d APs. %d known.\nold cases return.",
    "%d captures filed.\n%.1f km walked.\nfieldwork leaves receipts.",
};
static constexpr int CTX_COOKING_AMBIENT_COUNT = sizeof(CTX_COOKING_AMBIENT) / sizeof(CTX_COOKING_AMBIENT[0]);

// ==[ SOFA/BED — drowsy, brief ]==
static const char* const CTX_SLEEP_THREAT[] = {
    "%d tags nearby.\npancetta notes them.\nthen snores.",
    "%d tags.\n%d APs.\ncase waits.",
};
static constexpr int CTX_SLEEP_THREAT_COUNT = sizeof(CTX_SLEEP_THREAT) / sizeof(CTX_SLEEP_THREAT[0]);

static const char* const CTX_SLEEP_SCAN[] = {
    "%d tags. %d networks.\nscan receipt under pillow.",
    "scan done.\n%d tags. %d APs.\nback to sleep.",
};
static constexpr int CTX_SLEEP_SCAN_COUNT = sizeof(CTX_SLEEP_SCAN) / sizeof(CTX_SLEEP_SCAN[0]);

static const char* const CTX_SLEEP_AMBIENT[] = {
    "%d radios indexed.\nradio takes night watch.",
    "%d signals logged.\nwire stays awake.\npancetta does not.",
};
static constexpr int CTX_SLEEP_AMBIENT_COUNT = sizeof(CTX_SLEEP_AMBIENT) / sizeof(CTX_SLEEP_AMBIENT[0]);

// ==[ CROWD DENSITY — environmental awareness ]==
static const char* const CTX_DESERTED[] = {
    "deserted tier.\nfewer than five.\nno crowd cover.",
    "under five estimated.\nany signal gains weight.",
    "thin RF crowd.\nevery beacon gets\na case number.",
    "deserted tier.\none detective.\nfew alibis.",
    "population below five.\nattack surface\nstill present.",
};
static constexpr int CTX_DESERTED_COUNT = sizeof(CTX_DESERTED) / sizeof(CTX_DESERTED[0]);

static const char* const CTX_CROWDED[] = {
    "crowded tier.\nforty or more.\nattribution gets harder.",
    "%d devices nearby.\nsource attribution gets harder.",
    "dense frequency.\nprobes overlap.\nalibis multiply.",
    "dense RF.\n%d devices.\nexcellent cover.",
    "crowded channels.\nour frames gain\nmore neighbors.",
};
static constexpr int CTX_CROWDED_COUNT = sizeof(CTX_CROWDED) / sizeof(CTX_CROWDED[0]);

static const char* const CTX_SHRINKING[] = {
    "crowd thinning.\nremaining signals\ngain weight.",
    "devices leaving.\nwhat stays gets watched.",
    "population dropping.\nstable tags\nlose excuses.",
    "band emptying.\nfewer devices.\ncleaner attribution.",
};
static constexpr int CTX_SHRINKING_COUNT = sizeof(CTX_SHRINKING) / sizeof(CTX_SHRINKING[0]);

static const char* const CTX_GROWING[] = {
    "population estimate rising.\nmore radios.\nmore alibis.",
    "crowd growing.\ndevice count rising.\ncover improves.",
    "population surge.\n%d and counting.\ncaseboard fills.",
};
static constexpr int CTX_GROWING_COUNT = sizeof(CTX_GROWING) / sizeof(CTX_GROWING[0]);

// helper: roll a percentage check
static bool chance(int pct) {
    return (int)(esp_random() % 100) < pct;
}

const char* getContextPhrase(uint8_t station) {
    // live data
    int trackers   = DefensePipeline::snapshot().getTrackerCount();
    int following  = DefensePipeline::snapshot().getFollowingCount();
    int apCount    = DefensePipeline::snapshot().getLastWifiAPCount();
    int openAPs    = DefensePipeline::snapshot().getOpenAPCount();
    int knownAPs   = DefensePipeline::snapshot().getKnownAPCount();
    uint32_t sinceLastScan = DefensePipeline::snapshot().getTimeSinceLastScan();
    uint16_t bleSeen = DefensePipeline::snapshot().getTotalBLEDevicesSeen();
    uint16_t captures = Capture::getTotalCount();
    float km       = Pedometer::getDistanceKm();

    bool isSleep = (station == STATION_SOFA || station == STATION_BED);

    // ==[ PRIORITY 1: following — always show ]==
    if (following > 0) {
        // build duration string from tracker table
        const char* detail = "a while";
        const Recon::TrackerEntry* t = DefensePipeline::snapshot().getTrackers();
        int tSize = DefensePipeline::snapshot().getTrackerTableSize();
        for (int i = 0; i < tSize; i++) {
            if (t[i].flags & Recon::FLAG_FOLLOWING) {
                uint32_t dur = (millis() - t[i].firstSeen) / 60000;
                if (dur < 1) detail = "<1 min";
                else {
                    static char durBuf[12];
                    snprintf(durBuf, sizeof(durBuf), "%lum", (unsigned long)dur);
                    detail = durBuf;
                }
                break;
            }
        }
        const char* tmpl = pick(CTX_FOLLOWING, CTX_FOLLOWING_COUNT);
        return format(tmpl, detail);
    }

    // ==[ PRIORITY 1.5: XBand high-confidence intel ]==
    if (DefensePipeline::snapshot().hasActiveAttacker() && chance(50))
        return pick(CTX_XBAND_ATTACKER, CTX_XBAND_ATTACKER_COUNT);
    if (DefensePipeline::snapshot().isDualBandStalkActive() && chance(50))
        return pick(CTX_XBAND_DUALBAND, CTX_XBAND_DUALBAND_COUNT);
    if (DefensePipeline::snapshot().getHighConfidenceCohortCount() > 0 && chance(10))
        return pick(CTX_XBAND_COHORT, CTX_XBAND_COHORT_COUNT);

    // ==[ PRIORITY 2: threats active — 30% chance ]==
    if (DefensePipeline::snapshot().hasThreats() && chance(30)) {
        const char* tmpl;
        if (isSleep) {
            tmpl = pick(CTX_SLEEP_THREAT, CTX_SLEEP_THREAT_COUNT);
        } else if (station == STATION_LAPTOP) {
            tmpl = pick(CTX_LAPTOP_THREAT, CTX_LAPTOP_THREAT_COUNT);
        } else if (station == STATION_WINDOW) {
            tmpl = pick(CTX_WINDOW_THREAT, CTX_WINDOW_THREAT_COUNT);
        } else {
            tmpl = pick(CTX_COOKING_THREAT, CTX_COOKING_THREAT_COUNT);
        }
        return format(tmpl, trackers, apCount, openAPs);
    }

    // ==[ PRIORITY 2.5: crowd density — 15% chance ]==
    XBand::CrowdTier crowd = DefensePipeline::snapshot().getCrowdTier();
    XBand::CrowdTrend trend = DefensePipeline::snapshot().getCrowdTrend();
    uint16_t pop = DefensePipeline::snapshot().getEstimatedPopulation();
    if (chance(15)) {
        const char* tmpl = nullptr;
        if (crowd == XBand::CrowdTier::DESERTED && !isSleep) {
            tmpl = pick(CTX_DESERTED, CTX_DESERTED_COUNT);
        } else if (crowd >= XBand::CrowdTier::CROWDED && !isSleep) {
            tmpl = pick(CTX_CROWDED, CTX_CROWDED_COUNT);
            if (strchr(tmpl, '%')) return format(tmpl, (int)pop);
        } else if (trend == XBand::CrowdTrend::SHRINKING && trackers > 0 && !isSleep) {
            tmpl = pick(CTX_SHRINKING, CTX_SHRINKING_COUNT);
        } else if (trend == XBand::CrowdTrend::GROWING && pop >= 20 && !isSleep) {
            tmpl = pick(CTX_GROWING, CTX_GROWING_COUNT);
            if (strchr(tmpl, '%')) return format(tmpl, (int)pop);
        }
        if (tmpl) return tmpl;
    }

    // ==[ PRIORITY 3: scan just completed (<10s) — 40% chance ]==
    if (sinceLastScan < 10000 && chance(40)) {
        const char* tmpl;
        if (isSleep) {
            tmpl = pick(CTX_SLEEP_SCAN, CTX_SLEEP_SCAN_COUNT);
        } else if (station == STATION_LAPTOP) {
            tmpl = pick(CTX_LAPTOP_SCAN, CTX_LAPTOP_SCAN_COUNT);
        } else if (station == STATION_WINDOW) {
            tmpl = pick(CTX_WINDOW_SCAN, CTX_WINDOW_SCAN_COUNT);
        } else {
            tmpl = pick(CTX_COOKING_SCAN, CTX_COOKING_SCAN_COUNT);
        }
        return format(tmpl, trackers, apCount, openAPs);
    }

    // ==[ PRIORITY 3.5: vulnerable probe clients — 10% chance ]==
    if (DefensePipeline::snapshot().getVulnProbeCount() > 0 && chance(10))
        return pick(CTX_PROBE_VULN, CTX_PROBE_VULN_COUNT);

    // ==[ PRIORITY 4: has recon data — 20% chance ]==
    bool hasData = (bleSeen > 0 || apCount > 0 || captures > 0);
    if (hasData && chance(20)) {
        const char* tmpl;
        if (isSleep) {
            tmpl = pick(CTX_SLEEP_AMBIENT, CTX_SLEEP_AMBIENT_COUNT);
            return format(tmpl, apCount > 0 ? apCount : (int)bleSeen, 0);
        } else if (station == STATION_LAPTOP) {
            // templates use mixed types — pick carefully
            int idx = esp_random() % CTX_LAPTOP_AMBIENT_COUNT;
            tmpl = CTX_LAPTOP_AMBIENT[idx];
            if (idx == 0) return format(tmpl, (int)bleSeen, captures);
            if (idx == 1) return format(tmpl, (int)bleSeen, km);
            return format(tmpl, apCount, knownAPs);
        } else if (station == STATION_WINDOW) {
            tmpl = pick(CTX_WINDOW_AMBIENT, CTX_WINDOW_AMBIENT_COUNT);
            return format(tmpl, apCount > 0 ? apCount : (int)bleSeen);
        } else {
            // cooking
            int idx = esp_random() % CTX_COOKING_AMBIENT_COUNT;
            tmpl = CTX_COOKING_AMBIENT[idx];
            if (idx == 0) return format(tmpl, apCount, knownAPs);
            return format(tmpl, captures, km);
        }
    }

    // ==[ PRIORITY 5: temporal/environmental context — 10% chance ]==
    if (chance(10)) {
        // night shift (0-5 AM)
        m5::rtc_datetime_t dt;
        if (M5.Rtc.getDateTime(&dt) && dt.date.year > 2024 && dt.time.hours < 6) {
            static const char* const NIGHT_PHRASES[] = {
                "graveyard shift.\nchannels stay awake.",
                "after midnight.\ntraffic gets honest.",
                "late watch.\nbeacons keep testifying.",
                "dark hours.\neach beacon\ngets logged.",
            };
            return pick(NIGHT_PHRASES, 4);
        }
        // low battery
        uint8_t batt = Hamlet::getBatteryPercent();
        if (batt > 0 && batt < 20) {
            static const char* const BATT_PHRASES[] = {
                "battery low.\n%d%% remaining.\nclose case cleanly.",
                "power reserve.\n%d%%.\nfiles need custody.",
                "battery fading.\n%d%% left.\nevery scan costs.",
            };
            return format(pick(BATT_PHRASES, 3), (int)batt);
        }
        // long session (>2 hours)
        if (millis() > 7200000) {
            static const char* const LONG_PHRASES[] = {
                "two hours in.\ncallbacks know\nthe office address.",
                "long session.\nuptime has\na case number.",
                "hours deep.\nCore2 still holding\nthe night shift.",
            };
            return pick(LONG_PHRASES, 3);
        }
    }

    // ==[ DEFAULT: static station phrase ]==
    return getStationPhrase(station);
}

}  // namespace NoirNarrator
