/**
 * Help Wiki -- Pancetta's field manual.
 * Multi-page entries per screen. Flash strings only.
 * Hard-wrapped at 49 chars. 15 body lines max per page.
 * Controls verified against hamlet.cpp input paths.
 */
#include "help_wiki.h"

// ==[ IDLE -- TH3 P3N ]== 3 pages

static const char IDLE_P0[] =
    "home base. one desk, bad lighting.\n"
    "the Core2 calls it an office.\n"
    "\n"
    "mood moves with captures, kills,\n"
    "and shoe leather on the meter.\n"
    "weather follows the case load.\n"
    "enough trouble starts the full\n"
    "nuclear chain. subtlety resigned.\n"
    "\n"
    "Recon works the background shift.\n"
    "BLE and WiFi scans trade turns.\n"
    "the deauth sniffer takes notes.\n"
    "the pen looks idle. the wire isn't.";

static const char IDLE_P1[] =
    "controls. nobody gets an alibi.\n"
    "\n"
    "tap pig = pet. mood boost.\n"
    "2s cooldown. first 10 animate.\n"
    "11-20 go quiet. 20+ get ignored.\n"
    "hold pig = spin. +5 momentum.\n"
    "\n"
    "[A] or swipe R = prev theme.\n"
    "[C] or swipe L = next theme.\n"
    "[B] = menu, no theatrics.\n"
    "[B+] = menu by teleport.\n"
    "[C+] = power options.\n"
    "tap playfield = menu. case open.";

static const char IDLE_P2[] =
    "condition report. weather included.\n"
    "\n"
    "7 mood tiers: ECSTATIC through\n"
    "NEGLECTED. momentum is -100..100\n"
    "and decays on mode-timed checks.\n"
    "NEGLECTED opens a 60s intervention.\n"
    "even the firmware calls somebody.\n"
    "\n"
    "clouds, rain, thunder, wind, birds.\n"
    "sun silhouettes keep their distance.\n"
    "nuclear: shuttle > debris > cloud >\n"
    "shockwave > sand > debris rain.\n"
    "pedometer files every step.\n"
    "walking can prompt auto-hunt.";

static const char* const IDLE_PAGES[] = { IDLE_P0, IDLE_P1, IDLE_P2 };

// ==[ MENU -- TH3 HUB ]== 4 pages

static const char MENU_P0[] =
    "six drawers. fourteen tools.\n"
    "every tool kept its old warrant.\n"
    "\n"
    "[A/C] or swipe U/D = browse.\n"
    "[B], tap, or swipe R = open.\n"
    "inside a drawer, swipe L or [C+]\n"
    "returns to the six-group root.\n"
    "from the root, the same move = home.\n"
    "\n"
    "Pancetta explains groups and tools.\n"
    "pick a drawer. then pick the case.";

static const char MENU_P1[] =
    "six drawers. fourteen tools.\n"
    " F13LD 0PS\n"
    "  TRUFFL3S / W4RDR1V3\n"
    " S1GN4L L4B\n"
    "  RF SC0PE / P1G 34RS\n"
    "  D3F H0G4 / C5 M0NST3R\n"
    " C0MS\n"
    "  N0W F0CK / M3SH T4LK\n"
    " C4S3 F1L3S\n"
    "  TH3 T4K3 / P1G P0ST\n"
    " P1G L1F3\n"
    "  R1B R4CK / TH3 L0R3\n"
    " SYST3M\n"
    "  TUN3 P1G / XF3RM0D3";

// Progression contract: the Cat reacts to mood, goals, trust.
static const char MENU_P2[] =
    "six rooms. one suspicious lease.\n"
    "\n"
    "10s idle hides the hub and sends\n"
    "Pancetta through the room beat:\n"
    " 1 cyberdeck lab. laptop glow.\n"
    " 2 noir flat. sofa, rain, debt.\n"
    " 3 ramen bar. steam and a car.\n"
    " 4 surveillance nest. antenna.\n"
    " 5 underground bar. guarded booth.\n"
    " 6 comfort balcony. rain and steam.\n"
    "\n"
    "Cat reacts to mood, goals, trust.\n"
    "six dossiers. seven masteries.\n"
    "DEFHOG4 appears at left stations.\n"
    "tap it for the five-pane war room.";

static const char MENU_P3[] =
    "roaming cases interrupt the rent.\n"
    "\n"
    "when the hub hides, witnesses can\n"
    "walk into the room beat uninvited.\n"
    "[A/C] choose. [B] commits.\n"
    "[C+] or swipe L = bail.\n"
    "\n"
    "session on pays real XP and mood,\n"
    "with possible evidence.\n"
    "session off still files cases and\n"
    "trophies, but pays no case XP.\n"
    "the ledger does not take bribes.";

static const char* const MENU_PAGES[] = { MENU_P0, MENU_P1, MENU_P2, MENU_P3 };

// ==[ HUNT -- TRUFFL3S ]== 4 pages

static const char HUNT_P0[] =
    "2.4GHz truffle beat. snout down.\n"
    "thirteen channels enter the lineup.\n"
    "\n"
    "WiFi collection hops all 13.\n"
    "D-UCB follows productive air.\n"
    "\n"
    "catches PMKIDs and crackable\n"
    "EAPOL pairs. motion changes policy.\n"
    "PSRAM holds evidence off-book.\n"
    "cut power and the witness vanishes.\n"
    "\n"
    "caps: 256 captures, 64 networks.\n"
    "the snout follows the receipts.";

static const char HUNT_P1[] =
    "motion arrives with four alibis.\n"
    "\n"
    "CAMP: still 10s+. longer dwell,\n"
    "targeted deauth, nearby AP focus.\n"
    "PATROL: walking. discovery and\n"
    "exploitation split the shift.\n"
    "SPRINT: fast motion. wide channel\n"
    "discovery, no lingering questions.\n"
    "LURK: near a high-value target.\n"
    "focused attack, channel locked.\n"
    "\n"
    "the pedometer moves the FSM.\n"
    "transitions file themselves.";

static const char HUNT_P2[] =
    "D-UCB remembers the whole beat.\n"
    "\n"
    "13 channels are 13 bandit arms.\n"
    "reward = captures per dwell time.\n"
    "discounted UCB1 ranks recent\n"
    "evidence above old testimony.\n"
    "\n"
    "CAMP and PATROL tune hunt policy.\n"
    "both read one shared arm ledger.\n"
    "motion changes policy, not memory.\n"
    "\n"
    "the hunt bar plots all thirteen.\n"
    "thirteen suspects. one notebook.";

static const char HUNT_P3[] =
    "field controls. sharp edge ahead.\n"
    "\n"
    "[A] = hunt stats overlay.\n"
    "[B] or [C] = pause/resume.\n"
    "[B+] or [C+] = exit hunt.\n"
    "tap = pause.\n"
    "hold or swipe L = exit.\n"
    "\n"
    "MUDBALL transmits deauth frames.\n"
    "40+ crowd = fewer clients per AP.\n"
    "crowds lower active exposure.\n"
    "\n"
    "use only on networks you own.\n"
    "the radio carries receipts.";

static const char* const HUNT_PAGES[] = { HUNT_P0, HUNT_P1, HUNT_P2, HUNT_P3 };

// ==[ SPECTRUM -- RF SC0PE ]== 6 pages

static const char SPEC_P0[] =
    "2.4GHz under glass. bad lighting.\n"
    "the band has no clean witnesses.\n"
    "\n"
    "completed sweeps own the evidence.\n"
    "waterfall keeps sweep history.\n"
    "MODEL adds reconstructed AP lobes.\n"
    "the header names the live layer.\n"
    "\n"
    "promiscuous mode takes frames on\n"
    "the current channel, then hops\n"
    "across all 13 channels.\n"
    "\n"
    "tracks up to 48 networks.\n"
    "every pixel enters evidence.";

static const char SPEC_P1[] =
    "layer priority. live state wins.\n"
    "\n"
    "[B]: paranoid > attack > client\n"
    "> DIAL > AP select.\n"
    "AP select opens client monitor.\n"
    "[B] there opens/closes detail.\n"
    "\n"
    "[A/C] detail = bearing recalibrate.\n"
    "swipe R detail = active poke.\n"
    "it transmits. own the target.\n"
    "[A/C] list = previous/next AP.\n"
    "swipe U/D browses the list.\n"
    "[C+] = back one layer.\n"
    "the stale proximity lead is closed.";

static const char SPEC_P2[] =
    "paranoid swine takes night watch.\n"
    "\n"
    "deauth radar watches 802.11\n"
    "deauth and disassoc frames.\n"
    "\n"
    "an attack flashes source BSSID,\n"
    "target, and RSSI history.\n"
    "\n"
    "[B] opens full attack telemetry.\n"
    "attacker and victim BSSIDs testify.\n"
    "first detection sends morse SOS.\n"
    "the alert brought names.";

static const char SPEC_P3[] =
    "RAD/THRU puts direction on file.\n"
    "\n"
    "2.4GHz inspects client frames.\n"
    "5GHz inspects a selected C5 AP.\n"
    "gyro tags pose. lock needs four\n"
    "sectors, 60 degrees, fresh RSSI.\n"
    "5GHz scan locks cap confidence.\n"
    "PPS never becomes target direction.\n"
    "\n"
    "DIAL maps tilt to channels 1-13.\n"
    "[B] locks the selected channel.\n"
    "\n"
    "tap list selects. double tap graph:\n"
    "2.4 toggles MODEL; 5GHz zooms.\n"
    "long press backs/exits.";

static const char SPEC_P4[] =
    "C5 puts 5GHz on a second radio.\n"
    "\n"
    "each SNAP lobe is one AP scan row.\n"
    "the old SNAP stays until the next\n"
    "completed scan replaces it.\n"
    "the strip prints its MHz viewport.\n"
    "pan arrows flag more band offscreen.\n"
    "2G|5G counts stay separate.\n"
    "\n"
    "[A/C] picks an AP; graph follows.\n"
    "[B] opens recon plus its actions.\n"
    "[B+] returns to the 2.4GHz radio.";

static const char SPEC_P5[] =
    "C5 live observers own its radio.\n"
    "\n"
    "PPS_LIVE plots all frames/sec on\n"
    "the selected channel, once a second.\n"
    "it is channel load, never AP motion\n"
    "or target bearing.\n"
    "\n"
    "CHAN_MAP repeats AP-count bars.\n"
    "counts are census, not airtime.\n"
    "\n"
    "inside recon [A/C] picks an action.\n"
    "[B] runs it. STOP ends the stream.\n"
    "normal C5 scans resume after STOP.";

static const char* const SPEC_PAGES[] = {
    SPEC_P0, SPEC_P1, SPEC_P2, SPEC_P3, SPEC_P4, SPEC_P5
};

// ==[ WARDRIVE -- W4RDR1V3 ]== 3 pages

static const char WARD_P0[] =
    "the warthog takes the road shift.\n"
    "WiFi gets mapped while you move.\n"
    "\n"
    "Core GPS or a fresh C5 GPS fix\n"
    "tags each network with coordinates.\n"
    "WiGLE v1.6 CSV stays on Core SD.\n"
    "the format keeps its paperwork.\n"
    "\n"
    "PIGBROTHER hashes stay separate in\n"
    "FLOCK_REPLAY sidecars, never WiGLE.\n"
    "\n"
    "solo car or swarm, same road.\n"
    "every beacon leaves a mile marker.";

static const char WARD_P1[] =
    "two live views. one scan engine.\n"
    "\n"
    "swipe left/right trades cockpit for\n"
    "a dim 10Hz fullscreen sensor tape.\n"
    "\n"
    "it shows source/fix, coordinates,\n"
    "satellites, HDOP, age, scan mix,\n"
    "strongest AP, C5 5GHz, BLE and SD.\n"
    "\n"
    "radio/GPS/SD stay live. swipe back\n"
    "and the same cockpit resumes.";

static const char WARD_P2[] =
    "road controls. spare gestures work.\n"
    "\n"
    "[B] = toggle pause.\n"
    "[B+] = exit (teleport out).\n"
    "[C] or [C+] = teleport out.\n"
    "swipe L/R = cockpit / sensor tape.\n"
    "hold top bar = global screen lock.\n"
    "swipe up = unlock.\n"
    "\n"
    "fix acquire plays a navigation-lock\n"
    "cadence. a lost held fix warns until\n"
    "it returns. no fix, no fake target.";

static const char* const WARD_PAGES[] = { WARD_P0, WARD_P1, WARD_P2 };

// ==[ BLE -- P1G 34RS ]== 6 pages

static const char BLE_P0[] =
    "the ears work the BLE district.\n"
    "advertisements enter the lineup.\n"
    "\n"
    "continuous NimBLE scanning.\n"
    "18 classifier slots: UNKNOWN plus\n"
    "17 named families. AirTag, Tile,\n"
    "SmartTag, Flipper, FMDN, FastPair,\n"
    "HID, iBeacon, Xiaomi, Sidewalk,\n"
    "GAEN, Eddystone, spam, and more.\n"
    "\n"
    "list view carries RSSI sparklines.\n"
    "proximity: CLOSE/NEAR/FAR/EDGE.\n"
    "TX power can estimate meters.\n"
    "R1B R4CK keeps the case count.";

static const char BLE_P1[] =
    "tracking gives one signal a chair.\n"
    "\n"
    "[B] locks a device and starts\n"
    "Geiger clicks. closer means faster.\n"
    "\n"
    "gyro RAD draws a fan and SEEK ray.\n"
    "turn across 4 sectors; fresh RSSI\n"
    "can earn a bearing lock.\n"
    "a ghost marks last-known position.\n"
    "behind-V needs live RF direction.\n"
    "\n"
    "haptic tick on bearing lock.\n"
    "RSSI updates about every 1-2s.\n"
    "the witness keeps moving.";

static const char BLE_P2[] =
    "classifier receipts. exact fields.\n"
    "\n"
    "FLIPPER: MAC 80:E1:26, 0C:FA:22;\n"
    "service 0x3081; company 0x038F.\n"
    "HID: service 0x1812.\n"
    "BLE_SPAM: rapid MAC rotation.\n"
    "HM-10: service 0xFFE0.\n"
    "FMDN: Google Find My Device.\n"
    "\n"
    "step-following adapts to crowd:\n"
    "deserted: 800m/10min/3 scans.\n"
    "busy: 1000m/12m30s/4 scans.\n"
    "crowded: 1200m/15min/5 scans.\n"
    "numbers first. panic denied.";

static const char BLE_P3[] =
    "XBand cross-examines WiFi and BLE.\n"
    "\n"
    "six correlations take the stand:\n"
    " attacker: WiFi attack fingerprint\n"
    " dual stalk: BLE tail + WiFi probe\n"
    " cohort: same owner, both bands\n"
    " owner net: tail owner's SSID\n"
    " crowd: BLE + WiFi density\n"
    " vendor: ecosystem cross-reference\n"
    "\n"
    "STALK alerts need both witnesses.\n"
    "fusion is evidence, not prophecy.";

static const char BLE_P4[] =
    "controls. keep the hands clean.\n"
    "\n"
    "list [A/C] = prev/next device.\n"
    "[B] = track; [B+/C+] = exit.\n"
    "\n"
    "tracking: [B] = stop.\n"
    "[A] = AirTag ping or GATT read.\n"
    "[B+] or swipe U = recalibrate.\n"
    "[C+] = back to list.\n"
    "swipe D = add to watchlist.\n"
    "\n"
    "swipe R = cycle scan mode:\n"
    "passive > ACT > CHF > passive.\n"
    "watchlist holds 6 named devices.\n"
    "ACT/GATT transmit. own the device.";

static const char BLE_P5[] =
    "the case ledger pays on evidence.\n"
    "\n"
    "P1G_34RS: 25 BLE devices tagged.\n"
    "T4G_C0LL3CT0R: 5 tracker\n"
    "families linked.\n"
    "T41L_BR34K3R: persistent tail.\n"
    "XB4ND_GUMSH03: BLE+WiFi case.\n"
    "\n"
    "Recon XP has a per-session cap.\n"
    "idle scans pay nothing. evidence\n"
    "earns rewards. R1B R4CK shows\n"
    "the BLE case counts. evidence filed.";

static const char* const BLE_PAGES[] = {
    BLE_P0, BLE_P1, BLE_P2, BLE_P3, BLE_P4, BLE_P5
};

// ==[ LOOT -- TH3 T4K3 ]== 3 pages

static const char LOOT_P0[] =
    "the take. an evidence locker in RAM.\n"
    "the heap swears it has the room.\n"
    "\n"
    "PMKIDs and crackable EAPOL pairs\n"
    "live in PSRAM. mounted SD journals\n"
    "them; hard-off without SD clears it.\n"
    "\n"
    "four tabs:\n"
    " LIST: live/lifetime counts, pairs.\n"
    " CRACKED: WPA-SEC potfiles.\n"
    " WIGLE: CSV rows, bytes, receipts.\n"
    " NUKE: wipe every capture.\n"
    "four drawers. one has a red handle.";

static const char LOOT_P1[] =
    "actions. each tab keeps a blade.\n"
    "\n"
    "[B] opens detail: SSID, BSSID,\n"
    "STA MAC, sync state, file format.\n"
    "\n"
    "[B+] runs the tab action:\n"
    " LIST: ship handshake PCAP.\n"
    " CRACKED: download potfile.\n"
    " WIGLE: upload CSV.\n"
    " NUKE: hold to confirm the wipe.\n"
    "\n"
    "uploads need WiFi and API keys.\n"
    "success syncs the clock. SD is\n"
    "required for potfiles and CSVs.";

static const char LOOT_P2[] =
    "vault controls. count the drawers.\n"
    "\n"
    "swipe U/D or [A/C] = browse.\n"
    "swipe R cycles all four tabs:\n"
    "LIST > CRACKED > WIGLE > NUKE.\n"
    "[B] = detail view.\n"
    "[C+] or swipe L = exit.\n"
    "\n"
    "export moves evidence to SD.\n"
    "until then PSRAM holds the bag.\n"
    "evidence filed. power still objects.";

static const char* const LOOT_PAGES[] = { LOOT_P0, LOOT_P1, LOOT_P2 };

// ==[ FEEDING -- R1B R4CK ]== 2 pages

static const char FEED_P0[] =
    "rib rack. trophies under bad neon.\n"
    "the case ledger brought souvenirs.\n"
    "\n"
    "four shelves:\n"
    " LVL: XP, rank, mood, goals.\n"
    " TR0PHY: achievement case.\n"
    " ITEMS: trinkets and case lore.\n"
    " ST4TS: captures, steps, cases.\n"
    " select TASKS for live case goals.\n"
    "\n"
    "swipe U/D or [A/C] scrolls.\n"
    "[B] changes tabs.\n"
    "[C+] or swipe L = exit.";

static const char FEED_P1[] =
    "items carry the ouroboros file.\n"
    "\n"
    "hardware, casework, and bad food.\n"
    "every trinket remembers a scene.\n"
    "\n"
    "drop K-H0RS3 three times and the\n"
    "Barman's Korean starts translating.\n"
    "\n"
    "later K-H0RS3 drops buy advice.\n"
    "quality varies. noodles remain\n"
    "within the accepted error bars.";

static const char* const FEED_PAGES[] = { FEED_P0, FEED_P1 };

// ==[ WALK_STATS -- W4LK ST4TS ]== 2 pages

static const char WALK_P0[] =
    "shoe leather, reduced to evidence.\n"
    "the IMU counts what the shoes deny.\n"
    "\n"
    "accelerometer magnitude spikes\n"
    "cross a 1.6g threshold with\n"
    "250ms debounce. one step filed.\n"
    "\n"
    "steps, distance, calories.\n"
    "~0.75 meters per step.\n"
    "\n"
    "no GPS. pure IMU testimony.\n"
    "the shoes retain counsel.";

static const char WALK_P1[] =
    "motion state drives the next case.\n"
    "\n"
    "STATIONARY: still for 10s+.\n"
    "WALKING: 3+ steps detected.\n"
    "\n"
    "hunt maps stillness to CAMP and\n"
    "walking to PATROL.\n"
    "\n"
    "4+ steps inside 3s starts the\n"
    "visible walking animation.\n"
    "\n"
    "[C+] or swipe L = exit.\n"
    "the wire remembers the route.";

static const char* const WALK_PAGES[] = { WALK_P0, WALK_P1 };

// ==[ SETTINGS -- TUN3 P1G ]== 3 pages

static const char SETT_P0[] =
    "every knob enters the interview.\n"
    "some arrive with legal counsel.\n"
    "\n"
    "twelve drawers, one level deep:\n"
    "profile, look, screen/input,\n"
    "power/time, track/watch, GPS,\n"
    "attack, defense, uplinks, FLOCK,\n"
    "accessories, hazard.\n"
    "\n"
    "caption lines divide paired systems.\n"
    "[B] = toggle or edit value.\n"
    "swipe U/D or [A/C] = scroll.\n"
    "[C+] = back; at root = exit.\n"
    "changes persist in NVS; writes are\n"
    "debounced. values survive reboot.";

static const char SETT_P1[] =
    "notable switches. motives attached.\n"
    "\n"
    "auto-hunt prompts after walking.\n"
    "MUDBALL: OFF/ON/AGGRO deauth.\n"
    "NTP SYNC sets RTC from saved WiFi.\n"
    "haptic controls vibration feedback.\n"
    "SFX volume runs 0-10.\n"
    "GPS enables wardrive coordinates.\n"
    "IPP toggles background BLE + WiFi\n"
    "defense scans.\n"
    "\n"
    "transmit and destructive toggles\n"
    "show warnings. read them.\n"
    "authorization remains your case.";

static const char SETT_P2[] =
    "UPL1NKS keeps credentials together.\n"
    "\n"
    "WPA-SEC keys, WiFi, and WiGLE\n"
    "credentials use the on-screen\n"
    "keyboard. tap characters to type.\n"
    "\n"
    "C0NF1G portal code exists, but the\n"
    "current hub exposes no portal door.\n"
    "use the on-screen keyboard today.\n"
    "\n"
    "submitted values persist in NVS.\n"
    "the missing route is our suspect.";

static const char* const SETT_PAGES[] = { SETT_P0, SETT_P1, SETT_P2 };

// ==[ NOWFLOCK -- N0W F0CK ]== 2 pages

static const char SYNC_P0[] =
    "N0W F0CK keeps the wire room quiet.\n"
    "passive FNOW/3 runs over ESP-NOW.\n"
    "LSP-1 gates candidate quality.\n"
    "\n"
    "MESH overview. V1S sightings.\n"
    "PEER table. W1R protocol health.\n"
    "\n"
    "[A/C] cycles panes; in PEER it\n"
    "scrolls the list. swipe U/D too.\n"
    "swipe R = next pane.\n"
    "[B] requests peer summaries.\n"
    "[C+] or swipe L = exit.\n"
    "the flock listens. receipts follow.";

static const char SYNC_P1[] =
    "one Core2 radio. no second alibi.\n"
    "\n"
    "FNOW/3 peers meet on one channel.\n"
    "Hunt, Spectrum, Xfer, and portal\n"
    "take exclusive radio custody.\n"
    "\n"
    "callbacks only stage frames. no\n"
    "malloc, serial, NVS, UI, or haptic\n"
    "from the WiFi task. it has priors.\n"
    "\n"
    "wire rows omit raw MACs, BLE addrs,\n"
    "exact GPS, creds, and handshakes.\n"
    "corrupt WiFi frames die below app.\n"
    "privacy filed before atmosphere.";

static const char* const SYNC_PAGES[] = { SYNC_P0, SYNC_P1 };

// ==[ POWER -- PWR ]== 1 page

static const char PWR_P0[] =
    "power key opens; backs out.\n"
    "a 4s hard-off bypasses SD sealing.\n"
    "\n"
    "deep sleep: touch -> cold boot\n"
    "light sleep: touch -> PSRAM held\n"
    "power off: PMIC key -> cold boot\n"
    "cancel: return to idle unchanged\n"
    "\n"
    "settings save before sleep/off.\n"
    "deep/off must seal mounted SD first.\n"
    "deep/off clear live PSRAM.\n"
    "deep/off without SD lose captures.\n"
    "[A/C] or swipe U/D = choose.\n"
    "[B] selects; deep/off ask again.\n"
    "[C+] or swipe L = cancel.";

static const char* const PWR_PAGES[] = { PWR_P0 };

// ==[ ABOUT -- TH3 L0R3 ]== 1 page

static const char ABOUT_P0[] =
    "one case file opens per visit.\n"
    "0ct0 left signals, not closure.\n"
    "\n"
    "Pancetta works the 2.4GHz beat:\n"
    "beacons lie, tags tail, packets\n"
    "remember half the crime.\n"
    "\n"
    "K-H0RS3 cracks the Horse's wall.\n"
    "the Barman keeps the other ledger.\n"
    "\n"
    "footer shows version + commit hash.\n"
    "case sequence persists in NVS.\n"
    "[C+] or swipe L = exit.\n"
    "the wire remembers.";

static const char* const ABOUT_PAGES[] = { ABOUT_P0 };

// ==[ WEBCONFIG -- C0NF1G ]== 1 page

static const char WCFG_P0[] =
    "reserved portal. missing door.\n"
    "current hub has no C0NF1G route.\n"
    "\n"
    "if firmware enters this mode, it\n"
    "spawns an AP and shows its IP.\n"
    "\n"
    "browser form edits WiFi, WPA-SEC,\n"
    "and WiGLE credentials. SUBMIT saves\n"
    "changed fields to NVS. unsent edits\n"
    "remain in the browser.\n"
    "\n"
    "[C] = exit to SETTINGS.\n"
    "[C+] = flush loaded config + exit.\n"
    "[B] does nothing. route dormant.";

static const char* const WCFG_PAGES[] = { WCFG_P0 };

// ==[ XFER -- XF3RM0D3 ]== 2 pages

static const char XFER_P0[] =
    "the drop opens its own evidence AP.\n"
    "no upstream WiFi gets a vote.\n"
    "\n"
    "join PANCETTA_XFER by QR or with\n"
    "the shown password. browser:\n"
    "http://192.168.4.1\n"
    "\n"
    "browse SD, upload, download, delete.\n"
    "WPA-SEC potfiles and WiGLE CSVs\n"
    "open directly in the browser.\n"
    "\n"
    "the SD card is the evidence room.\n"
    "port 80 keeps the office hours.";

static const char XFER_P1[] =
    "web commander. deletion is literal.\n"
    "\n"
    "the screen shows AP, IP, password,\n"
    "clients, TX/RX, uploads, downloads.\n"
    "\n"
    "the port-80 browser can upload,\n"
    "download, and delete SD files.\n"
    "delete means delete. no noir escape.\n"
    "\n"
    "[C] or [C+] = exit to menu.\n"
    "swipe L = exit to menu.\n"
    "[B] has no action.\n"
    "the courier leaves with receipts.";

static const char* const XFER_PAGES[] = { XFER_P0, XFER_P1 };

// ==[ C5MONSTER ]== dual-band C5Monster command menu
static const char C5_P0[] =
    "C5 M0NST3R\n"
    "---\n"
    "ESP32-C5 UART BR1DGE\n"
    "P1NS: S33 C5 C0NF1G\n"
    "C2 UART2 0R GPS; S3 UART1.\n"
    "\n"
    "DUAL-BAND: 2.4+5GHz\n"
    "JanOS F1RMW4R3.\n"
    "SCAN SNIFF DEAUTH.\n"
    "EV1L TW1N K4RM4.";
static const char C5_P1[] =
    "C5 C0MM4NDS\n"
    "---\n"
    "OK = r0un cmd\n"
    "BCK = scroll up\n"
    "L0NG BCK = 3X1T\n"
    "L0NG OK = 3M3RGENCY\n"
    "STOP (4ll 0ps).\n"
    "0UTPUT = live UART.";
static const char* const C5_PAGES[] = { C5_P0, C5_P1 };

// ==[ MAIL -- P1G P0ST ]== 2 pages

static const char MAIL_P0[] =
    "witnesses used to kick the door in.\n"
    "now they file and wait.\n"
    "\n"
    "a case that finds you while the pig\n"
    "roams lands here instead of seizing\n"
    "the screen. the top bar carries M<n>\n"
    "while anything is unread.\n"
    "\n"
    "8 slots. one open file per witness.\n"
    "a full box drops new arrivals, so\n"
    "clear it or the wire stops calling.";

static const char MAIL_P1[] =
    "a file is a tree, not a question.\n"
    "\n"
    "each decision opens the next beat.\n"
    "three beats deep, three ways out of\n"
    "every one. XP and mood settle per\n"
    "decision; the case only closes when\n"
    "a choice ends it.\n"
    "\n"
    "A/C = pick. B = file it.\n"
    "L0NG BCK = bail. the letter keeps\n"
    "your progress and waits.";

static const char* const MAIL_PAGES[] = { MAIL_P0, MAIL_P1 };

// ==[ MESH -- M3SH T4LK ]== 3 pages

static const char MESH_P0[] =
    "the C6L does the radio. this screen\n"
    "does the talking.\n"
    "\n"
    "a Unit C6L on Grove runs stock\n"
    "Meshtastic and keeps the hard parts:\n"
    "modulation, routes, retries, channel\n"
    "crypto. this side is the head unit\n"
    "a window the size of a window.\n"
    "\n"
    "two dialects. the C6L picks one\n"
    "by its serial.mode. under TEXTMSG a\n"
    "line out goes to the whole primary\n"
    "channel and a line in arrives as\n"
    "sender, then body. that is all.";

static const char MESH_P1[] =
    "controls. the wire keeps no secrets.\n"
    "\n"
    "[B]=write. [B+]=clear scrollback.\n"
    "[A] = older. [C] = newer.\n"
    "swipe U/D = scroll. tap = write.\n"
    "[C+] or swipe L = exit.\n"
    "\n"
    "64 retained. yours carry the bright\n"
    "gutter; arrivals come dimmed. a rule\n"
    "marks where the mesh went quiet.\n"
    "\n"
    "runs from boot: an arrival away\n"
    "shows sender + a preview, then waits\n"
    "as )) up top. L1V3 means heard\n"
    "within 2min. W41T is quiet, not bad.";

static const char MESH_P2[] =
    "PROTO. what TEXTMSG threw away.\n"
    "\n"
    "set serial.mode=PROTO on the C6L and\n"
    "set C0D3C in TUN3 P1G. the radio\n"
    "then hands over its whole node list.\n"
    "\n"
    "N0D = roster: name, SNR, hops,\n"
    "heard, battery. [B] on a row aims\n"
    "the composer at that node; the same\n"
    "tap again aims back at everyone.\n"
    "\n"
    "a DM asks for ack. + on the rail\n"
    "is a real receipt from the far end.\n"
    "an arrival digit is its hop count.";

static const char* const MESH_PAGES[] = { MESH_P0, MESH_P1, MESH_P2 };

// ==[ DEFHOG4 -- D3FH0G4 ]== 5 pages

static const char DH4_P0[] =
    "DEFHOG4 turns the room into a desk.\n"
    "five panes. nobody gets omniscience.\n"
    "\n"
    "Recon, XBand, Hunt, and Capture\n"
    "bring live cross-namespace intel.\n"
    "\n"
    "entry chooses the hottest file:\n"
    "following tracker -> BLE.\n"
    "deauth storm -> SIGINT.\n"
    "attacker fingerprint -> FUSION.\n"
    "quiet air -> SITREP.\n"
    "the terminal triages before coffee.";

static const char DH4_P1[] =
    "five panes take sworn statements.\n"
    "\n"
    "SITREP: posture, source age, why,\n"
    "next evidence, crowd, surface.\n"
    "Recon fills in for Hunt.\n"
    "SIGINT: channel yield, deauth burst\n"
    "history, WiFi AP census.\n"
    "BLE: trackers, following, spam,\n"
    "watchlist.\n"
    "FUSION: fingerprints, cohorts,\n"
    "dual-band stalking.\n"
    "LOG: IOC flags and deauth timeline.\n"
    "all five brought documentation.";

static const char DH4_P2[] =
    "war-room controls. live state first.\n"
    "\n"
    "[A] = previous pane.\n"
    "[C] = next pane.\n"
    "swipe L/R = cycle panes.\n"
    "[B]: SITREP -> Hunt.\n"
    "tap NEXT: open its evidence pane.\n"
    "SIGINT -> Scope. BLE -> scanner.\n"
    "FUSION/LOG -> refresh.\n"
    "[B+/C+] or hold = exit to menu.\n"
    "\n"
    "swipe U enters REVIEW history and\n"
    "refresh pauses. swipe D returns.\n"
    "bottom of history resumes LIVE.\n"
    "the archive charges by the scroll.";

static const char DH4_P3[] =
    "each pane keeps its own watch.\n"
    "\n"
    "SITREP: every 5 seconds.\n"
    "SIGINT: every 3 seconds.\n"
    "BLE: every 2 seconds.\n"
    "FUSION: every 5 seconds.\n"
    "LOG: every 5 seconds.\n"
    "\n"
    "header badges flag unseen deauth,\n"
    "following trackers, fingerprints,\n"
    "and evil twins on other panes.\n"
    "the clock cross-examines everybody.";

static const char DH4_P4[] =
    "interactive. six chairs at the desk.\n"
    "\n"
    "tap a BLE tracker to open that exact\n"
    "rendered identity in P1G 34RS.\n"
    "expired rows ask for a rescan.\n"
    "\n"
    "entry points:\n"
    "MENU: select D3FH0G4.\n"
    "ROOM: tap a materialized terminal\n"
    "at a left-side station.\n"
    "\n"
    "DEFHOG4 adds no radio work. Recon\n"
    "scans; this desk only reads.\n"
    "seventh witness waits outside.";

static const char* const DH4_PAGES[] = { DH4_P0, DH4_P1, DH4_P2, DH4_P3, DH4_P4 };

// ==[ TITLES ]== mode names for the header line

static const char* const HELP_TITLES[] = {
    "TH3 P3N",       // IDLE
    "TH3 HUB",       // MENU
    "TRUFFL3S",      // HUNT
    "RF SC0PE",      // SPECTRUM
    "TH3 T4K3",      // LOOT
    "R1B R4CK",      // FEEDING
    "W4LK ST4TS",    // WALK_STATS
    "TUN3 P1G",      // SETTINGS
    "N0W F0CK",      // NOWFLOCK
    "PWR",           // POWER_MENU
    "TH3 L0R3",      // ABOUT
    "C0NF1G",        // WEBCONFIG
    "W4RDR1V3",      // WARDRIVE
    "P1G 34RS",      // BLE_SCANNER
    "D3FH0G4",       // DEFHOG4
    "XF3RM0D3",      // XFER
    "C5 M0NST3R",    // C5MONSTER
    "P1G P0ST",      // MAIL
    "M3SH T4LK"      // MESH
};
static constexpr uint8_t MODE_COUNT = HAMLET_MODE_COUNT;
static_assert(sizeof(HELP_TITLES) / sizeof(HELP_TITLES[0]) == MODE_COUNT,
              "HELP_TITLES/HamletMode enum mismatch");

// ==[ PAGE ARRAYS + COUNTS ]== indexed by HamletMode ordinal

struct WikiEntry {
    const char* const* pages;
    uint8_t count;
};

static const WikiEntry WIKI[] = {
    { IDLE_PAGES,   sizeof(IDLE_PAGES)   / sizeof(IDLE_PAGES[0])   },  // IDLE
    { MENU_PAGES,   sizeof(MENU_PAGES)   / sizeof(MENU_PAGES[0])   },  // MENU
    { HUNT_PAGES,   sizeof(HUNT_PAGES)   / sizeof(HUNT_PAGES[0])   },  // HUNT
    { SPEC_PAGES,   sizeof(SPEC_PAGES)   / sizeof(SPEC_PAGES[0])   },  // SPECTRUM
    { LOOT_PAGES,   sizeof(LOOT_PAGES)   / sizeof(LOOT_PAGES[0])   },  // LOOT
    { FEED_PAGES,   sizeof(FEED_PAGES)   / sizeof(FEED_PAGES[0])   },  // FEEDING
    { WALK_PAGES,   sizeof(WALK_PAGES)   / sizeof(WALK_PAGES[0])   },  // WALK_STATS
    { SETT_PAGES,   sizeof(SETT_PAGES)   / sizeof(SETT_PAGES[0])   },  // SETTINGS
    { SYNC_PAGES,   sizeof(SYNC_PAGES)   / sizeof(SYNC_PAGES[0])   },  // NOWFLOCK
    { PWR_PAGES,    sizeof(PWR_PAGES)    / sizeof(PWR_PAGES[0])    },  // POWER_MENU
    { ABOUT_PAGES,  sizeof(ABOUT_PAGES)  / sizeof(ABOUT_PAGES[0])  },  // ABOUT
    { WCFG_PAGES,   sizeof(WCFG_PAGES)   / sizeof(WCFG_PAGES[0])   },  // WEBCONFIG
    { WARD_PAGES,   sizeof(WARD_PAGES)   / sizeof(WARD_PAGES[0])   },  // WARDRIVE
    { BLE_PAGES,    sizeof(BLE_PAGES)    / sizeof(BLE_PAGES[0])    },  // BLE_SCANNER
    { DH4_PAGES,    sizeof(DH4_PAGES)    / sizeof(DH4_PAGES[0])    },  // DEFHOG4
    { XFER_PAGES,   sizeof(XFER_PAGES)   / sizeof(XFER_PAGES[0])   },  // XFER
    { C5_PAGES,     sizeof(C5_PAGES)     / sizeof(C5_PAGES[0])     },  // C5MONSTER
    { MAIL_PAGES,   sizeof(MAIL_PAGES)   / sizeof(MAIL_PAGES[0])   },  // MAIL
    { MESH_PAGES,   sizeof(MESH_PAGES)   / sizeof(MESH_PAGES[0])   },  // MESH
};
static_assert(sizeof(WIKI) / sizeof(WIKI[0]) == MODE_COUNT,
              "WIKI/HamletMode enum mismatch");

static const uint8_t WIKI_COUNT = sizeof(WIKI) / sizeof(WIKI[0]);

const char* HelpWiki::getHelpTitle(HamletMode mode) {
    uint8_t idx = static_cast<uint8_t>(mode);
    if (idx >= sizeof(HELP_TITLES) / sizeof(HELP_TITLES[0])) return "???";
    return HELP_TITLES[idx];
}

const char* HelpWiki::getHelpPage(HamletMode mode, uint8_t page) {
    uint8_t idx = static_cast<uint8_t>(mode);
    if (idx >= WIKI_COUNT) return "";
    const WikiEntry& e = WIKI[idx];
    if (page >= e.count) return "";
    return e.pages[page];
}

uint8_t HelpWiki::getPageCount(HamletMode mode) {
    uint8_t idx = static_cast<uint8_t>(mode);
    if (idx >= WIKI_COUNT) return 0;
    return WIKI[idx].count;
}
