/**
 * mood system. speech bubble brain.
 * HAMLET PANCETTA mood engine + pedometer hooks.
 * phase 1: persistent mood + streak.
 * phase A: mode-aware decay + empty sync penalty.
 */

#include "mood.h"
#include "mood_context.h"
#include "weather.h"
#include "../ui/display.h"
#include "../ui/menu_pig_render.h"
#include "../ui/pixel_street.h"
#include "../ui/scene_cache.h"
#include "../ui/teleport.h"
#include "../ui/ui_measurements.h"
#include "../core/config.h"
#include "../core/item_effects.h"
#include "../core/bounty.h"
#include "../core/capture.h"
#include "../core/item_drops.h"
#include "../core/time_of_day.h"
#include "../audio/sfx.h"
#include "../haptic/haptic.h"
#include "../hamlet.h"
#include "../modes/hunt.h"
#include "../activity/pedometer.h"
#include "../ui/defhog_terminal.h"
#include "../core/achievements.h"
#include "../core/challenges.h"
#include "../defense/recon.h"
#include "../defense/xband.h"
#include "../defense/defense_pipeline.h"
#include "../util/time_math.h"
#include <esp_random.h>
#include <stdarg.h>
#include <time.h>


// static guts
char Mood::currentPhrase[96] = {0};
AvatarState Mood::moodState = AvatarState::NEUTRAL;
int Mood::momentum = 0;
uint32_t Mood::momentumDecayTime = 0;
uint32_t Mood::lastPhraseTime = 0;
uint32_t Mood::phraseTimeout = 4000;  // 4s read window

char Mood::phraseQueue[5][64] = {{0}};
AvatarState Mood::stateQueue[5] = {AvatarState::NEUTRAL};
int Mood::queueHead = 0;
int Mood::queueTail = 0;

int Mood::lastPhraseIndex[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
int Mood::historyPos = 0;

// ==[ SESSION DEBRIEF ]== stashed on hunt exit, consumed by DEFHOG4
struct HuntDebrief {
    uint8_t pmkids;
    uint8_t handshakes;
    uint16_t probes;
    uint16_t deauths;
    uint8_t maxCombo;
    uint32_t xpGained;
    bool pending;
};
static HuntDebrief debrief = {};

// rain monologue state. fires once per rain
static bool wasRaining = false;
static int monologueIndex = 0;
static uint32_t lastMonologueTime = 0;

// ==[ MOOD PITCH TRACKING ]== per-tier SFX pitch shift
static MoodTier lastMoodTier = MoodTier::CONTENT;

// Phase D: critical mood state (once per boot) - separate from rain
static bool criticalMonologueShown = false;  // first time: full monologue + toast
static bool criticalStateActive = false;     // sync warning timer running
static bool criticalMonologueComplete = false; // monologue finished (36s elapsed)
static uint32_t criticalStartTime = 0;       // when timer started
static int criticalMonologueIndex = 0;       // separate from rain monologue
static uint32_t lastCriticalPhraseTime = 0;
static int lastTickSecond = -1;              // track last tick for per-second audio
static constexpr uint32_t CRITICAL_TIMEOUT = 60000;  // 60s to act
static constexpr uint32_t MONOLOGUE_DURATION = 36000; // 36s monologue before toast
static bool inCriticalScene = false;         // scene lock: prevents cancellation during monologue
static uint32_t nextRainTickTime = 0;       // randomized tick timing
static uint32_t rainTickInterval = 0;       // current interval (180-450ms base)

static TimeOfDay::State currentIdleTimeOfDay(uint32_t now) {
    static bool clockSampleValid = false;
    static uint32_t clockSampleAt = 0;
    static uint64_t clockSampleLocalMs = 0;
    const uint32_t sampleAge = now - clockSampleAt;
    if (clockSampleValid && sampleAge < 1000u)
        return TimeOfDay::compute(clockSampleLocalMs + sampleAge);

    // Convert trusted local clock fields to a monotonic-within-year millisecond
    // value. Sample wall/RTC time at 1 Hz, then extrapolate with millis so the
    // hot render path does not poll the RTC over I2C every frame.
    uint64_t localMs = 0;
    bool trustedClock = false;
    time_t sysTime = time(nullptr);
    if (sysTime > 1704067200) {
        struct tm local = {};
        localtime_r(&sysTime, &local);
        localMs = ((uint64_t)local.tm_yday * 24ull * 3600ull +
                   (uint64_t)local.tm_hour * 3600ull +
                   (uint64_t)local.tm_min * 60ull +
                   (uint64_t)local.tm_sec) * 1000ull + now % 1000u;
        trustedClock = true;
    }

    if (!trustedClock && M5.Rtc.isEnabled()) {
        auto dt = M5.Rtc.getDateTime();
        if (dt.date.year >= 2024 &&
            dt.date.month >= 1 && dt.date.month <= 12 &&
            dt.date.date >= 1 && dt.date.date <= 31 &&
            dt.time.hours < 24 && dt.time.minutes < 60 && dt.time.seconds < 60) {
            uint32_t localDay = (uint32_t)dt.date.month * 31u + dt.date.date;
            localMs = ((uint64_t)localDay * 24ull * 3600ull +
                       (uint64_t)dt.time.hours * 3600ull +
                       (uint64_t)dt.time.minutes * 60ull +
                       (uint64_t)dt.time.seconds) * 1000ull + now % 1000u;
            trustedClock = true;
        }
    }

    // An unset clock should still produce a readable scene, not boot into an
    // arbitrary midnight palette.
    if (!trustedClock)
        localMs = 12ull * 3600ull * 1000ull + now;

    clockSampleAt = now;
    clockSampleLocalMs = localMs;
    clockSampleValid = true;
    return TimeOfDay::compute(localMs);
}

// ==[ MOMENTUM AWARDS ]==
static constexpr int MOMENTUM_WALK_START          = 5;
static constexpr int MOMENTUM_HUNT_START           = 10;
static constexpr int MOMENTUM_PMKID_BASE           = 20;
static constexpr int MOMENTUM_HANDSHAKE_BASE       = 35;
static constexpr int MOMENTUM_FIRST_CAPTURE        = 15;
static constexpr int MOMENTUM_FIRST_DISCOVERY      = 15;
static constexpr int MOMENTUM_FOURWAY_VICTORY      = 15;
static constexpr int MOMENTUM_SYNC_SUCCESS         = 15;
static constexpr int MOMENTUM_EMPTY_SYNC_PENALTY   = -10;
static constexpr int MOMENTUM_SYNC_FAIL_PENALTY    = -10;
static constexpr int MOMENTUM_GOAL_COMPLETE        = 20;
static constexpr int MOMENTUM_LEVEL_UP             = 30;
static constexpr int MOMENTUM_BACK_ONLINE          = 10;
static constexpr int MOMENTUM_CHANNEL_LEARNED      = 10;
static constexpr int MOMENTUM_TELEPORT_ARRIVAL     = 10;
static constexpr int MOMENTUM_DEAUTH               = 5;
static constexpr int MOMENTUM_SAE_REJECT           = 8;
static constexpr int MOMENTUM_NEAR_MISS            = 5;
static constexpr int MOMENTUM_BIRD_KILL            = 5;
static constexpr int MOMENTUM_PLUGGED_IN           = 5;
static constexpr int MOMENTUM_CHANNEL_EXPLOIT      = 5;
static constexpr int MOMENTUM_KNOWN_AP             = 5;
static constexpr int MOMENTUM_TRACKER_DETECTED     = 5;
static constexpr int MOMENTUM_TRACKER_FOLLOWING    = 15;
static constexpr int MOMENTUM_BLE_SPAM             = 8;
static constexpr int MOMENTUM_EVIL_TWIN            = 12;
static constexpr int MOMENTUM_KARMA_DETECTED       = 12;
static constexpr int MOMENTUM_ATTACKER_IDENTIFIED  = 18;
static constexpr int MOMENTUM_DUAL_BAND_STALK      = 18;
static constexpr int MOMENTUM_CANARY_TRIPPED       = 25;
static constexpr int MOMENTUM_KARMA_CONFIRMED      = 15;
static constexpr int MOMENTUM_TOOL_IDENTIFIED      = 10;
static constexpr int MOMENTUM_WARDRIVE_NETWORK     = 2;
static constexpr int MOMENTUM_WARDRIVE_MILESTONE   = 15;
static constexpr int MOMENTUM_PROBE_SUCCESS        = 3;
static constexpr int MOMENTUM_PET_FIRST            = 5;
static constexpr int MOMENTUM_PET_REPEAT           = 2;
static constexpr int MOMENTUM_PET_SPAM             = 1;
static constexpr int MOMENTUM_DEBRIEF_BONUS        = 5;
static constexpr int MOMENTUM_RIB_ESCAPE           = 25;
static constexpr int MOMENTUM_SHIP_KILL            = 100;  // setMomentum, not addMomentum

// ==[ MOOD TIER BOUNDARIES ]==
static constexpr int MOOD_TIER_ECSTATIC_MIN  = 80;
static constexpr int MOOD_TIER_HAPPY_MIN     = 60;
static constexpr int MOOD_TIER_CONTENT_MIN   = 40;
static constexpr int MOOD_TIER_MEH_MIN       = 20;
static constexpr int MOOD_TIER_SAD_MIN       = 0;
static constexpr int MOOD_TIER_DEPRESSED_MIN = -20;

// ==[ MOMENTUM DECAY ]==
static constexpr uint32_t DECAY_INTERVAL_HUNT_MS        = 4000;
static constexpr uint32_t DECAY_INTERVAL_FLOCK_MS       = 10000;
static constexpr uint32_t DECAY_INTERVAL_IDLE_HIGH_MS   = 1000;
static constexpr uint32_t DECAY_INTERVAL_IDLE_MID_MS    = 2000;
static constexpr uint32_t DECAY_INTERVAL_IDLE_LOW_MS    = 3000;
static constexpr uint32_t DECAY_INTERVAL_DEFAULT_MS     = 4000;
static constexpr uint32_t IDLE_EROSION_THRESHOLD_MS     = 300000;
static constexpr uint32_t IDLE_EROSION_DIVISOR_MS       = 60000;
static constexpr int      IDLE_EROSION_PER_MINUTE       = 5;
static constexpr int      MOMENTUM_TIER_HIGH_THRESHOLD  = 50;
static constexpr int      MOMENTUM_TIER_MID_THRESHOLD   = 20;

// ==[ CRITICAL MOOD ]==
static constexpr int      MOMENTUM_FLASH_POSITIVE       = 15;
static constexpr int      MOMENTUM_FLASH_NEGATIVE       = -25;
static constexpr int      CHARGED_NOTIFICATION_THRESHOLD = 70;

// Phase 4: Escape sequence state (repurposed for XP)
static char ribEscapeSequence[7] = {0};
static uint8_t ribEscapeIdx = 0;
static bool ribEscapeDialogActive = false;
static uint32_t ribEscapeHoldStart = 0;

// phrase libraries. noir drip

// boot phrases by mood tier (phase 1)
// ecstatic (80-100) rare joy
static const char* const BOOT_ECSTATIC[] = {
    "streak %d.\nledger has teeth.",
    "streak hot.\nNVS brought receipts.",
    "boot clean.\nradio already pacing.",
    "momentum high.\nwatchdog looks nervous.",
    "Core2 awake.\ntiny screen, large case.",
    "back on the beat.\n13 channels object."
};
static const int BOOT_ECSTATIC_COUNT = 6;

static const char* const BOOT_ECSTATIC_F[] = {
    "numbers climbed.\npig claims method.",
    "NVS kept every footprint.",
    "antenna wants overtime.",
    "FreeRTOS holds the coat.",
    "confidence: evidence-backed.",
    "good run.\nweather still unpaid."
};

// happy (60-79) consistent hunter
static const char* const BOOT_HAPPY[] = {
    "streak %d.\nsignal holds.",
    "streak intact.\nNVS did its job.",
    "boot complete.\nradio knows the route.",
    "momentum positive.\nrare, documented.",
    "another shift.\nchannels already talking.",
    "Core2 ready.\ncoffee remains theoretical."
};
static const int BOOT_HAPPY_COUNT = 6;

static const char* const BOOT_HAPPY_F[] = {
    "logs agree. suspiciously.",
    "the watchdog found no body.",
    "antenna kept its alibi.",
    "steady beats dramatic.",
    "one clean boot. take it.",
    "pig competent.\npixels unconvinced."
};

// content (40-59) neutral, functional
static const char* const BOOT_CONTENT[] = {
    "boot complete.\nall suspects present.",
    "radio warming.\nno urgency, no excuses.",
    "2.4 GHz ready.\nweather undecided.",
    "streak %d.\nclock moved.",
    "snout calibrated.\nRSSI may still lie.",
    "systems nominal.\npig needs coffee."
};
static const int BOOT_CONTENT_COUNT = 6;

static const char* const BOOT_CONTENT_F[] = {
    "adequate has receipts.",
    "heap found contiguous rent.",
    "callbacks remain employed.",
    "NVS returned the file.",
    "watchdog says proceed.",
    "small screen. full shift."
};

// meh (20-39) falling behind
static const char* const BOOT_MEH[] = {
    "boot complete.\nmomentum running thin.",
    "radio awake.\nenthusiasm on debounce.",
    "low-mood profile.\nstill operational.",
    "streak cooled.\nNVS kept the timestamp.",
    "Core2 up.\nweather moved indoors.",
    "streak %d.\ncase remains open."
};
static const int BOOT_MEH_COUNT = 6;

static const char* const BOOT_MEH_F[] = {
    "slow is still running.",
    "the antenna showed up.",
    "callbacks do not need morale.",
    "one boot. zero speeches.",
    "radio takes the ugly shift.",
    "function beats theater."
};

// sad (0-19) low momentum, still functioning
static const char* const BOOT_SAD[] = {
    "boot from cold state.\nradio still answers.",
    "momentum below zero.\ncase above ground.",
    "rain profile loaded.\nantenna stays out.",
    "NVS returned old weather.",
    "quiet boot.\nwatchdog counts breaths.",
    "low signal inside.\nreceiver stays open."
};
static const int BOOT_SAD_COUNT = 6;

static const char* const BOOT_SAD_F[] = {
    "world-weary is not offline.",
    "the band owes no comfort.",
    "cold starts still start.",
    "radio works the late desk.",
    "no optimism required.",
    "pig stays on the clock."
};

// depressed (-20 to -1) deep low-momentum tier
static const char* const BOOT_DEPRESSED[] = {
    "boot complete.\nmomentum deep below zero.",
    "radio ready.\nweather moved indoors.",
    "NVS opened the cold file.",
    "low-mood profile.\nfunctions intact.",
    "screen lit.\nroom stayed dark.",
    "receiver awake.\nverdict postponed."
};
static const int BOOT_DEPRESSED_COUNT = 6;

static const char* const BOOT_DEPRESSED_F[] = {
    "operational beats cheerful.",
    "the callback still arrives.",
    "watchdog keeps bad hours.",
    "rain is data with timing.",
    "antenna does not need hope.",
    "case open. quietly."
};

// neglected (-100 to -21) lowest tier, still operational
static const char* const BOOT_NEGLECTED[] = {
    "cold boot.\nmomentum hit the basement.",
    "NVS kept the long silence.",
    "radio woke alone.\nstill woke.",
    "low-mood floor reached.\nno false alarm.",
    "screen on.\nweather has the office.",
    "receiver open.\ncity owes nothing."
};
static const int BOOT_NEGLECTED_COUNT = 6;

static const char* const BOOT_NEGLECTED_F[] = {
    "the firmware stays honest.",
    "basement still has power.",
    "silence got a timestamp.",
    "watchdog found a pulse.",
    "no comfort. full function.",
    "pig takes the night shift."
};

// rain monologue. wifi melancholy on mood crash
// fires once when rain starts. queued for drama.
static const char* const RAIN_MONOLOGUE[] = {
    "rain started.\nnoise floor took the window.",
    "beacons smear near -90 dBm.",
    "RSSI tells the truth slowly.",
    "callbacks bring wet footprints.",
    "partial frames crowd the desk.",
    "PSRAM keeps their statements.",
    "the city fades.\nthe band does not.",
    "receiver stays open."
};
static const int RAIN_MONOLOGUE_COUNT = 8;

// legacy boot phrases. fallback
static const char* const BOOT_PHRASES[] = {
    "boot nominal.\ncase file reopened.",
    "Core2 awake.\nradio takes attendance.",
    "NVS returned the old ledger.",
    "watchdog on duty.\nweather unhelpful.",
    "screen lit.\n13 channels remain guilty.",
    "startup clean.\nno witnesses lost.",
    "antenna ready.\ncity already lying.",
    "firmware up.\ncoffee still theoretical."
};
static const int BOOT_COUNT = 8;

static const char* const BOOT_PHRASES_F[] = {
    "the logs kept their story.",
    "one clean boot. documented.",
    "heap found enough office space.",
    "callbacks clocked in.",
    "the receiver wants statements.",
    "facts first. weather later.",
    "small screen. full jurisdiction.",
    "radio has the night desk."
};

// phase B: charged notification (HYP3 reached)
static const char* const CHARGED_PHRASES[] = {
    "HYP3 charged.\nmomentum above 70.",
    "momentum hot.\nradio wants a case.",
    "positive stack peaked.",
    "systems hot.\nwatchdog clears throat.",
    "snout up.\nchannels owe interest.",
    "charge banked.\nspend it on evidence."
};
static const int CHARGED_COUNT = 6;

static const char* const CHARGED_F[] = {
    "momentum has no brakes.",
    "tiny pig. unreasonable confidence.",
    "the meter found the ceiling."
};
static const int CHARGED_F_COUNT = 3;

// hunt start phrases
static const char* const HUNT_START_PHRASES[] = {
    "promiscuous mode.\ncase open.",
    "channel hop started.\n13 suspects.",
    "beacons hit the wire.\ni take statements.",
    "radio awake.\ncallbacks clocked in.",
    "BSSIDs enter the lineup.",
    "EAPOL desk is open.",
    "probe desk open.\nAPs hate questions.",
    "2.4 GHz beat.\nrain optional.",
    "sniffer up.\nheap keeps the coat.",
    "promiscuous RX.\nevery frame testifies.",
    "channels rotating.\nquietly.",
    "antenna on duty.\npig underpaid.",
    "hunt started.\n13 channels owe rent.",
    "beacons first.\nverdict later.",
    "clients leave footprints.",
    "monitoring traffic.\nnot innocence.",
    "D-UCB sharpens its pencil.",
    "frames incoming.\nPSRAM takes coats.",
    "channel 1 checked.\n12 owe answers.",
    "radio takes the night shift.",
    "capture buffer ready.\nPSRAM has custody.",
    "PMKID desk open.\nclients optional.",
    "M1 through M4.\nget the full set.",
    "the wire is live.\nchannels testify.",
    "Core 0 hears it.\nCore 1 gets paperwork."
};
static const int HUNT_START_COUNT = 25;

static const char* const HUNT_START_F[] = {
    "beacons rarely lawyer up.",
    "the callback has one job.",
    "13 channels. no favorites.",
    "RSSI is a nervous witness.",
    "SSID names mean nothing.",
    "BSSID keeps fingerprints.",
    "the heap wants adjoining rooms.",
    "one radio. 13 interviews.",
    "listen first. transmit later.",
    "partial frames stay partial.",
    "D-UCB remembers who paid.",
    "the watchdog wants brevity.",
    "clients make useful witnesses.",
    "promiscuous, not careless.",
    "the wire remembers."
};
static const int HUNT_START_F_COUNT = 15;

static const char* const HUNT_STOP_PHRASES[] = {
    "promiscuous mode off.\nzero captures.",
    "hunt closed.\nno new evidence.",
    "channel hop stopped.\nlead went cold.",
    "radio off duty.\nno receipt tonight.",
    "no capture.\nlead stays open."
};
static const int HUNT_STOP_COUNT = 5;

static const char* const HUNT_STOP_F[] = {
    "hashcat got no paperwork.",
    "silence is still a result.",
    "old captures keep their chairs."
};
static const int HUNT_STOP_F_COUNT = 3;

// pmkid captured
static const char* const PMKID_PHRASES[] = {
    "PMKID captured.\nclient not required.",
    "RSN brought one PMKID.\nclient skipped court.",
    "AP disclosed PMKID.\nclient never entered.",
    "clientless capture.\nrouter testified.",
    "PMKID bagged.\nhashcat gets night shift.",
    "one PMKID.\nno client traffic needed.",
    "AP left a key-shaped print.",
    "PMKID saved.\nrouter signed in hex.",
    "RSN exchange.\n16 bytes talked."
};
static const int PMKID_COUNT = 9;

static const char* const PMKID_F[] = {
    "client stayed home. fine.",
    "16 bytes. very cooperative.",
    "the AP wrote its name in hex.",
    "less traffic. same hashcat.",
    "clientless. not toothless."
};
static const int PMKID_F_COUNT = 5;

// handshake captured
static const char* const HANDSHAKE_PHRASES[] = {
    "crackable EAPOL pair.\nmask tells the damage.",
    "EAPOL pair bagged.\nPSRAM grunts.",
    "valid pair.\ndamn useful.",
    "EAPOL pair captured.\nhashcat ready.",
    "key exchange caught.\nmath can question it.",
    "valid message pair.\nradio earned its coffee.",
    "handshake saved.\nmask tells the rest.",
    "AP and client spoke.\nwe kept the tape.",
    "capture accepted.\nmask stays honest.",
    "key exchange saved.\nno embellishment."
};
static const int HANDSHAKE_COUNT = 10;

static const char* const HANDSHAKE_F[] = {
    "captured mask keeps score.",
    "quality rides with the mask.",
    "both endpoints left frames.",
    "hashcat gets what arrived.",
    "partial stays partial."
};
static const int HANDSHAKE_F_COUNT = 5;

// new network spotted
static const char* const NEW_NET_PHRASES[] = {
    "new BSSID.\nfresh fingerprints.",
    "beacon found.\nSSID noted.",
    "fresh AP on the beat.",
    "BSSID added.\nno assumptions.",
    "new beacon.\nchannel recorded.",
    "AP entered the lineup.",
    "SSID talks.\nBSSID identifies.",
    "new network.\nD-UCB starts asking."
};
static const int NEW_NET_COUNT = 8;

// probing attempts
static const char* const PROBE_PHRASES[] = {
    "open auth passed.\nassoc request sent.",
    "AP answered auth.\nrequesting PMKID.",
    "association request out.",
    "RSN request sent.\nwaiting.",
    "auth accepted.\nassoc on wire."
};
static const int PROBE_COUNT = 5;

// probe success/fail
static const char* const PROBE_SUCCESS_PHRASES[] = {
    "assoc response received.",
    "AP answered.\nPMKID kept its coat.",
    "association replied.\nthree XP, one shrug.",
    "response frame bagged.",
    "AP took the question.",
    "RSN replied.\nPMKID lawyered up."
};
static const int PROBE_SUCCESS_COUNT = 6;

static const char* const PROBE_FAIL_PHRASES[] = {
    "auth timed out.\nAP kept the door shut.",
    "association timed out.\nclock had final word.",
    "AP kept silent.\nrequest died waiting.",
    "no response frame.\nradio has no witness.",
    "probe window closed.\nchair still warm.",
    "auth or assoc: no reply.",
    "silence is not success.\nmove."
};
static const int PROBE_FAIL_COUNT = 7;

// buffer full
static const char* const BUFFER_PHRASES[] = {
    "capture buffer 99%.\nnot one damn hallway.",
    "PSRAM at 99%.\nintake meets the wall.",
    "99% occupied.\nnext write wants counsel.",
    "buffer high-water: 99%.\nsync required.",
    "one percent left.\nPSRAM ate the office."
};
static const int BUFFER_COUNT = 5;

// buffer filling (80% warning)
static const char* const BUFFER_FILLING_PHRASES[] = {
    "capture buffer crossed 80%.\nstill breathing.",
    "PSRAM over 80%.\nwalls moving inward.",
    "80% high-water crossed.\nintake smiles anyway.",
    "buffer past 80%.\nplan the handoff.",
    "one warning.\nPSRAM keeps eating."
};
static const int BUFFER_FILLING_COUNT = 5;

// sync handoff
static const char* const SYNC_START_PHRASES[] = {
    "sync opened.\nbytes leave under watch.",
    "handoff started.\npeer checks the guest list.",
    "capture payload moving.",
    "peer link up.\nPSRAM shows its pockets.",
    "buffer reporting.\ncounts under oath.",
    "transfer started.\nACK still outside."
};
static const int SYNC_START_COUNT = 6;

static const char* const SYNC_START_F[] = {
    "ACK has not signed in.",
    "the wire has the parcel.",
    "bytes travel without alibis."
};
static const int SYNC_START_F_COUNT = 3;

static const char* const SYNC_SUCCESS_PHRASES[] = {
    "sync complete.\npeer said yes.",
    "captures transferred.\ncounts match.",
    "handoff accepted.\nACK came home.",
    "buffer reported.\npeer stayed awake.",
    "transfer complete.\nboring. perfect."
};
static const int SYNC_SUCCESS_COUNT = 5;

static const char* const SYNC_SUCCESS_F[] = {
    "custody changed. facts did not.",
    "the wire remembers.",
    "ACK beats optimism."
};
static const int SYNC_SUCCESS_F_COUNT = 3;

static const char* const SYNC_FAIL_PHRASES[] = {
    "sync failed.\nlink left no note.",
    "handoff broke.\nbytes came home.",
    "peer went silent.\nRF kept the secret.",
    "link closed early.\ntransfer did not.",
    "remote never acknowledged."
};
static const int SYNC_FAIL_COUNT = 5;

static const char* const SYNC_FAIL_F[] = {
    "no ACK. no transfer.",
    "the wire dropped the statement.",
    "bytes came back unsworn."
};
static const int SYNC_FAIL_F_COUNT = 3;

// phase A: empty sync penalty
static const char* const EMPTY_SYNC_PHRASES[] = {
    "sync complete.\nzero captures.",
    "handoff opened.\nPSRAM showed lint.",
    "nothing transferred.\npeer noticed.",
    "zero captures.\nmath stayed bored.",
    "empty buffer.\nlink moved pure air.",
    "peer asked.\nPSRAM shrugged."
};
static const int EMPTY_SYNC_COUNT = 6;

// battery
static const char* const LOW_BATTERY_PHRASES[] = {
    "battery low.\nfinish the handoff.",
    "power falling.\nradio costs milliamps.",
    "low battery.\nunsaved data gets priority.",
    "power reserve thin.\nshorten the shift.",
    "battery low.\nkeep the exit clean."
};
static const int LOW_BATTERY_COUNT = 5;

// emergency sync - 10% battery with unsaved captures
static const char* const EMERGENCY_SYNC_PHRASES[] = {
    "SYNC NOW.\n10% BATTERY, CAPTURES UNSAVED.",
    "10% POWER.\nPSRAM DATA IS VOLATILE.",
    "CAPTURES UNSAVED.\nSYNC BEFORE POWER LOSS.",
    "EMERGENCY SYNC.\nDATA DIES WITH POWER.",
    "10% BATTERY.\nTRANSFER OR LOSE CAPTURES."
};
static const int EMERGENCY_SYNC_COUNT = 5;

// sleep blocked - captures exist
static const char* const SLEEP_BLOCKED_PHRASES[] = {
    "SLEEP BLOCKED.\nCAPTURES ARE UNSAVED.",
    "SYNC FIRST.\nSLEEP CLEARS PSRAM.",
    "CAPTURES IN VOLATILE MEMORY.",
    "NO SLEEP.\nTRANSFER DATA FIRST.",
    "PSRAM DATA WILL BE LOST.\nSYNC NOW."
};
static const int SLEEP_BLOCKED_COUNT = 5;

// idle phrases. waiting noise
static const char* const IDLE_PHRASES[] = {
    "radio idle.\nnoise floor still billing.",
    "13 channels.\nnone volunteered.",
    "channel 6 has the crowded alibi.",
    "beacons repeat.\nliars need rehearsal.",
    "RSSI is distance in a cheap suit.",
    "BSSID keeps the fingerprint.",
    "SSID is only the name on the door.",
    "dead air still enters the ledger.",
    "the receiver hears.\nit does not pardon.",
    "promiscuous mode waits offstage.",
    "D-UCB keeps old favors discounted.",
    "uncertainty wants another interview.",
    "reward history has a long memory.",
    "one radio.\nthirteen interview rooms.",
    "2.4 GHz never closes.",
    "the antenna works without applause.",
    "noise floor owns the basement.",
    "weak signals tell expensive stories.",
    "strong RSSI is not innocence.",
    "hidden SSID.\nvisible habits.",
    "MAC addresses wear thin disguises.",
    "clients leave association footprints.",
    "beacons advertise.\nradio takes notes.",
    "probe requests ask old questions.",
    "EAPOL waits for both parties.",
    "partial frames stay partial.",
    "message masks do not improvise.",
    "PMKID can arrive without a client.",
    "PSRAM holds the unsent exhibits.",
    "volatile means save it or lose it.",
    "NVS keeps the long-term ledger.",
    "flash remembers.\nerase cycles remember too.",
    "callbacks travel light.",
    "Core 0 hears.\nCore 1 files.",
    "malloc stays out of the hot alley.",
    "the watchdog hates long speeches.",
    "FreeRTOS schedules the night shift.",
    "atomics pass notes under the door.",
    "the heap wants contiguous rent.",
    "DMA knows which rooms are forbidden.",
    "one bad pointer.\nwhole precinct dark.",
    "millis keeps the ugly clock.",
    "debounce: patience with a timestamp.",
    "last-seen ages without sentiment.",
    "the clock moves.\nthe case does not.",
    "battery pays for every question.",
    "USB-C brings electrons, not answers.",
    "ten percent changes the tone.",
    "sleep erases volatile witnesses.",
    "sync is chain of custody.",
    "an ACK beats hopeful arithmetic.",
    "no ACK.\nno transfer.",
    "counts match or the handoff lies.",
    "the buffer keeps eating the office.",
    "eighty percent is a warning.",
    "ninety-nine percent needs action.",
    "rain paints static on the glass.",
    "weather reaches the signal first.",
    "IMU feels the room move.",
    "steps turn a scanner into patrol.",
    "CAMP lets the channel finish talking.",
    "PATROL keeps every lead moving.",
    "SPRINT trades depth for reach.",
    "LURK gives one suspect the chair.",
    "deauth TX leaves its own receipt.",
    "transmit frames carry consequences.",
    "listen first.\nTX signs paperwork.",
    "SAE may reject the forged rejection.",
    "fallback is possible, never promised.",
    "WPA3 keeps a harder door.",
    "old WEP ghosts still haunt the band.",
    "channel plans have borders.",
    "RF crosses walls.\npermission does not.",
    "a scan result is not a verdict.",
    "no beacons proves only silence.",
    "interference is not intent.",
    "correlation wants more witnesses.",
    "one capture is evidence, not prophecy.",
    "facts survive better than swagger.",
    "tiny pixels.\nfull case load.",
    "the screen cannot fit every suspect.",
    "320 by 240.\nno room for alibis.",
    "touch glass keeps greasy testimony.",
    "haptics knock inside the desk.",
    "the speaker files one sharp objection.",
    "Hamlet works the wireless night.",
    "Pancetta keeps the evidence literal."
};
static const int IDLE_COUNT = 87;

// idle continuations — generic second beats that pair with any idle phrase
static const char* const IDLE_F[] = {
    "the timestamp signs underneath.",
    "no poetry in the counter.",
    "the math kept its coat on.",
    "rain gets no veto.",
    "radio writes in raw frames.",
    "the buffer charges by the byte.",
    "watchdog dislikes improvisation.",
    "NVS keeps old receipts.",
    "the channel has another story.",
    "silence entered the report.",
    "one fact. fewer ghosts.",
    "signal first. motive later.",
    "the antenna heard worse.",
    "small office. ugly hours.",
    "the clock remains unsympathetic.",
    "evidence earns the last word.",
    "RSSI declined further comment.",
    "the heap has zoning laws.",
    "callbacks leave no luggage.",
    "PSRAM keeps temporary custody.",
    "nothing saved itself.",
    "ACK or it never arrived.",
    "thirteen doors. one receiver.",
    "the beat stays crowded.",
    "every byte wants a witness.",
    "the city emits continuously.",
    "no alibi survives a timestamp.",
    "pig files it under weather."
};
static const int IDLE_F_COUNT = 28;

// usb-c inserted. external power notification
static const char* const PLUG_IN_PHRASES[] = {
    "USB-C connected.\npower has an alibi.",
    "external power.\nbattery gets a chair.",
    "power coupling confirmed.",
    "electrons checked in.",
    "charging current on the beat."
};
static const int PLUG_IN_COUNT = 5;

static const char* const PLUG_IN_F[] = {
    "voltage came with receipts.",
    "the port earned its keep.",
    "battery gets breathing room."
};
static const int PLUG_IN_F_COUNT = 3;

// Walk start phrases - movement
static const char* const WALK_START_PHRASES[] = {
    "motion detected.\nPATROL has legs.",
    "steps moving.\nradio takes the beat.",
    "walking profile active.",
    "scanner mobile.\nchannels come along.",
    "IMU voted for field work."
};
static const int WALK_START_COUNT = 5;

static const char* const WALK_START_F[] = {
    "the map just got larger.",
    "motion changes the method.",
    "operational range expands."
};
static const int WALK_START_F_COUNT = 3;

// Step milestones
static const char* const STEP_MILESTONE_PHRASES[] = {
    "step milestone.\ncounter signed it.",
    "distance logged.\nradio kept pace.",
    "steps crossed the next line.",
    "another marker.\nsoles did paperwork.",
    "movement verified.\nno speech required."
};
static const int STEP_MILESTONE_COUNT = 5;

// Phase 3: Graduated walk milestones (5K/10K/20K/30K)
static const char* const WALK_MILESTONE_PHRASES[] = {
    "distance milestone.\nlegs operational.",
    "long patrol.\ncounter has receipts.",
    "step total climbed.\ncity got smaller.",
    "distance booked.\nradio worked overtime.",
    "milestone reached.\nfeet file expenses."
};
static const int WALK_MILESTONE_COUNT = 5;

// XP level-up celebration
static const char* const LEVEL_UP_PHRASES[] = {
    "LEVEL UP.\nnew rank on the file.",
    "NEW LEVEL.\nXP cleared the threshold.",
    "level advanced.\nledger confirms.",
    "XP threshold crossed.",
    "stronger profile.\nsame ugly band."
};
static const int LEVEL_UP_COUNT = 5;

static const char* const LEVEL_UP_F[] = {
    "the ledger moved upward.",
    "new level. old weather.",
    "progress, now documented."
};
static const int LEVEL_UP_F_COUNT = 3;

// crackable handshake pair captured
static const char* const FOURWAY_VICTORY_PHRASES[] = {
    "VALID PAIR. MASK INCLUDED",
    "KEY EXCHANGE BAGGED",
    "CRACKABLE EAPOL PAIR",
    "AP/CLIENT ON THE RECORD",
    "PSRAM TAKES THE EXHIBIT"
};
static const int FOURWAY_VICTORY_COUNT = 5;

// Capture milestones
static const char* const MILESTONE_PHRASES[] = {
    "numbers crossed a line.\npig noticed first.",
    "counts verified.\nno confetti budget.",
    "capture stack grew.\nPSRAM felt it.",
    "numbers moved.\nD-UCB took notes.",
    "milestone hit.\nwatchdog unimpressed."
};
static const int MILESTONE_COUNT = 5;

// new event phrase bins

// deauth/mudball. ssid fmt lives here
// format: %.10s = ssid (max 10 chars)
static const char* const DEAUTH_PHRASES[] = {
    "attack cycle -> %.10s",
    "%.10s: TX path selected",
    "radio focuses on %.10s",
    "%.10s enters the attack cycle",
    "target cycle -> %.10s",
    "%.10s: client action pending",
    "radio action -> %.10s",
    "%.10s: reconnect lead selected",
    "target selected: %.10s",
    "%.10s gets the interview chair",
    "TX decision -> %.10s"
};
static const int DEAUTH_COUNT = 11;

// SAE downgrade reject — cunning, calculated, protocol-level
static const char* const SAE_REJECT_PHRASES[] = {
    "SAE reject sent.\nfallback requested.",
    "WPA3 commit denied.\nwatch for WPA2.",
    "Dragonfly got a forged no.",
    "SAE status frame sent.",
    "transition AP nudged.\nPSK waits downstairs.",
    "WPA3 rejected.\nno fallback promised.",
    "spoofed reject.\nrouter owns next move.",
    "SAE door closed.\nRSN checks the list.",
    "reject frame out.\nwatch the fallback.",
    "SAE -> reject.\nWPA2 may take the bait."
};
static const int SAE_REJECT_COUNT = 10;

// catching = waiting for 4-way handshake
static const char* const CATCHING_PHRASES[] = {
    "%.10s: M1-M4 pending",
    "watching %.10s for EAPOL",
    "%.10s: message mask open",
    "waiting on %.10s frames",
    "%.10s: client testimony due",
    "EAPOL watch -> %.10s",
    "%.10s: partials already talking",
    "holding channel for %.10s",
    "%.10s: handshake watch stays hot"
};
static const int CATCHING_COUNT = 9;

// handshake captured
static const char* const SUCCESS_PHRASES[] = {
    "%.10s: pair bagged",
    "EAPOL caught: %.10s",
    "%.10s: handshake stored",
    "valid pair: %.10s",
    "%.10s -> hashcat",
    "%.10s: AP/client on tape",
    "key exchange: %.10s",
    "%.10s: mask tells truth",
    "%.10s: PSRAM takes it"
};
static const int SUCCESS_COUNT = 9;

// client spotted
static const char* const CLIENT_PHRASES[] = {
    "client observed.\nMAC added.",
    "new station on the BSSID.",
    "client frame.\nwitness located.",
    "station active.\nBSSID has company.",
    "client detected.\nno verdict yet.",
    "new MAC.\nold radio, fresh trouble.",
    "association traffic.\nclient present.",
    "new client.\nEAPOL may follow."
};
static const int CLIENT_COUNT = 8;

// mode flips (camp/patrol)
static const char* const CAMP_MODE_PHRASES[] = {
    "CAMP: longer dwell.\nradio settles.",
    "stationary profile.\nchannels get time.",
    "CAMP mode.\nlisten before moving.",
    "dwell widened.\npatience earns data.",
    "radio holds ground.\nbeacons do the walking.",
    "slow hops.\nmore listen time.",
    "fixed beat.\nmore time per channel.",
    "CAMP active.\ntraffic can arrive."
};
static const int CAMP_MODE_COUNT = 8;

static const char* const CAMP_MODE_F[] = {
    "long dwell. fewer guesses.",
    "stillness lowers the churn.",
    "the channel gets a hearing.",
    "patience is measurable.",
    "one beat. full attention."
};
static const int CAMP_MODE_F_COUNT = 5;

static const char* const PATROL_MODE_PHRASES[] = {
    "PATROL: balanced hops.",
    "walking profile.\nscan keeps pace.",
    "PATROL active.\nchannels rotate.",
    "balanced dwell.\nbeat continues.",
    "radio moving.\nD-UCB packs light.",
    "motion detected.\npatrol rules.",
    "channels turn.\nno rush.",
    "PATROL mode.\nlisten and move."
};
static const int PATROL_MODE_COUNT = 8;

static const char* const PATROL_MODE_F[] = {
    "balanced means no favorite.",
    "the beat changes slowly.",
    "motion updates the method.",
    "every channel gets a look.",
    "D-UCB carries the notebook."
};
static const int PATROL_MODE_F_COUNT = 5;

// sprint mode (fast walk/jog — pure discovery)
static const char* const SPRINT_MODE_PHRASES[] = {
    "SPRINT: discovery only.",
    "fast hops.\ndeauth stands down.",
    "speed profile.\nbeacons first.",
    "SPRINT active.\nwide search.",
    "moving fast.\nchannel dwell cut.",
    "discovery pass.\nno long interview.",
    "radio scans at foot speed.",
    "fast beat.\nnew BSSIDs wanted."
};
static const int SPRINT_MODE_COUNT = 8;

static const char* const SPRINT_MODE_F[] = {
    "motion traded depth for reach.",
    "short dwell. broad lineup.",
    "beacons survive the sprint.",
    "more channels.\nless interrogation.",
    "discovery has the wheel."
};
static const int SPRINT_MODE_F_COUNT = 5;

// lurk mode (locked on high-value target)
static const char* const LURK_MODE_PHRASES[] = {
    "LURK: channel locked.",
    "high-value AP.\nhold position.",
    "target selected.\nhops suspended.",
    "one channel.\nfull attention.",
    "LURK active.\nwait for EAPOL.",
    "channel fixed.\nclient watch open.",
    "best lead found.\nradio stays.",
    "targeted dwell.\nAP gets no recess."
};
static const int LURK_MODE_COUNT = 8;

static const char* const LURK_MODE_F[] = {
    "zero hops. one suspect.",
    "signal strong.\npatience stronger.",
    "the AP gets the whole shift.",
    "message mask stays open.",
    "hold the channel.\nlet it talk."
};
static const int LURK_MODE_F_COUNT = 5;

// network churn back online
static const char* const BACK_ONLINE_PHRASES[] = {
    "beacons returned.\nradio confirms.",
    "dead air ended.\nBSSID found.",
    "network activity restored.",
    "last-seen moved.\nclock has a pulse.",
    "signal returned.\nradio quits sulking.",
    "RF traffic back.\n15 s case dismissed.",
    "beacon after silence.\nnoted.",
    "the band answered.\nlead reopened."
};
static const int BACK_ONLINE_COUNT = 8;

static const char* const BACK_ONLINE_F[] = {
    "silence had an expiry.",
    "one beacon breaks dead air.",
    "last-seen has a pulse.",
    "the wire remembers."
};
static const int BACK_ONLINE_F_COUNT = 4;

// dead air (only when actually empty)
static const char* const DEAD_AIR_PHRASES[] = {
    "15 s without a beacon.",
    "dead air.\nlast-seen is stale.",
    "no beacon in 15 s.\nnoted.",
    "band quiet.\nBSSID count unchanged.",
    "channel hop finds silence.",
    "beacon clock stopped moving.",
    "no AP testimony.\nkeep scanning.",
    "beacons missed the shift.",
    "spectrum present.\nAPs absent.",
    "15 s.\nno beacon signed in.",
    "radio hears an empty beat.",
    "beacon air empty.\nscanner stays honest.",
    "last network went cold.",
    "nothing advertised.\nradio keeps walking.",
    "no BSSID entered the lineup.",
    "beacon drought.\nchannels rotate.",
    "last-seen clock keeps running.",
    "dead air confirmed.\nno fake ghosts.",
    "zero fresh beacons.\nmove the beat.",
    "the channel owes a statement.",
    "15 s of RF silence.",
    "no beacon brought an alibi.",
    "scanner waits.\ncity says nothing."
};
static const int DEAD_AIR_COUNT = 23;

static const char* const DEAD_AIR_F[] = {
    "silence is a measurement.",
    "last-seen keeps timestamp.",
    "no beacon. no invention.",
    "timestamp stays honest.",
    "D-UCB keeps rotating.",
    "quiet channels still count.",
    "15 seconds made the report.",
    "absence means absence.",
    "RSSI cannot testify here.",
    "antenna has no witness.",
    "move. listen. verify.",
    "keep the receiver open."
};
static const int DEAD_AIR_F_COUNT = 12;

// d-ucb exploit: channel paid before
static const char* const DUCB_EXPLOIT_PHRASES[] = {
    "known channel.\nold reward still decays.",
    "discounted reward points back.",
    "same lead. third pass.",
    "D-UCB favors earned signal.",
    "productive arm.\nexploit, dont worship.",
    "channel paid before.\nmath returns.",
    "reward average beat uncertainty.",
    "old reward. lower weight.\nstill talking."
};
static const int DUCB_EXPLOIT_COUNT = 8;

// d-ucb explore: forced wandering
static const char* const DUCB_EXPLORE_PHRASES[] = {
    "D-UCB questions weak history.",
    "confidence term won the vote.",
    "uncertain channel.\nconfidence leads.",
    "exploration pass.\nreward history thin.",
    "less-tested arm gets a hearing.",
    "old rewards stepped aside.",
    "channel sampled.\nuncertainty ate first.",
    "uncertainty outranked habit."
};
static const int DUCB_EXPLORE_COUNT = 8;

// d-ucb learned: fresh reward
static const char* const DUCB_LEARNED_PHRASES[] = {
    "first reward.\nchannel remembered.",
    "capture paid.\narm got heavier.",
    "productive channel.\nNVS takes the name.",
    "first reward.\nD-UCB stops guessing.",
    "reward posted.\nmath grows a preference."
};
static const int DUCB_LEARNED_COUNT = 5;

// first catch this session
static const char* const FIRST_CATCH_PHRASES[] = {
    "first capture.\nsession finally has teeth.",
    "session count: one.\npig looks smug.",
    "first exhibit.\nvault stops echoing.",
    "first capture saved.\nnight earns its coffee.",
    "session count moved: +1.",
    "first capture.\nzero no longer owns us.",
    "one capture.\nnight officially started.",
    "opening catch.\nradio found its nerve."
};
static const int FIRST_CATCH_COUNT = 8;

static const char* const FIRST_CATCH_F[] = {
    "the empty session had it coming.",
    "one fact beats ten leads.",
    "PSRAM has its first witness.",
    "hashcat gets breakfast.",
    "one catch is an alibi."
};
static const int FIRST_CATCH_F_COUNT = 5;

// daily goal phrases (phase 2)

// goal met last session (boot)
static const char* const GOAL_MET_PHRASES[] = {
    "last run met its step goal.",
    "prior shift closed on target.",
    "steps counted.\nstreak advanced.",
    "goal met last run.\nledger confirms.",
    "last run paid the distance bill."
};
static const int GOAL_MET_COUNT = 5;

// goal close (80-99% in session)
static const char* const GOAL_CLOSE_PHRASES[] = {
    "%lu steps remain.\nfinish line visible.",
    "close case: %lu steps left.",
    "almost there.\nkeep the pace.",
    "%lu steps left.\nclose the file.",
    "goal near.\ncounter keeps moving."
};
static const int GOAL_CLOSE_COUNT = 5;

// goal complete (100% in session)
static const char* const GOAL_COMPLETE_PHRASES[] = {
    "GOAL COMPLETE.\nTARGET VERIFIED.",
    "daily distance closed.",
    "target hit.\nledger signed.",
    "steps booked.\nshift earns a chair.",
    "goal achieved.\nno missing pages.",
    "QUEST COMPLETE. XP BANKED.",
    "goal hit.\nnext case can wait."
};
static const int GOAL_COMPLETE_COUNT = 7;

// walk milestone phrases (phase 4a)

// Every 1000 steps - encouraging, variable
static const char* const WALK_1K_PHRASES[] = {
    "another 1K.\ncounter has receipts.",
    "one thousand more on patrol.",
    "1K logged.\nradio came along.",
    "distance paid in small steps.",
    "another thousand.\ncity gives no refund.",
    "1K marker crossed.\ncase keeps moving."
};
static const int WALK_1K_COUNT = 6;

// every 5000 steps
static const char* const WALK_5K_PHRASES[] = {
    "5K MILESTONE.\nPATROL VERIFIED.",
    "five thousand steps.\nserious field work.",
    "5K logged.\ncounter needs no witness.",
    "long patrol.\nsoles filed overtime.",
    "five thousand.\ncity still not finished.",
    "5K ACHIEVEMENT.\nDISTANCE BANKED.",
    "5K on the books.\nradio stayed employed.",
    "distance excessive.\nevidence excellent."
};
static const int WALK_5K_COUNT = 8;

void Mood::init() {
    memset(currentPhrase, 0, sizeof(currentPhrase));
    moodState = AvatarState::NEUTRAL;
    momentum = 0;
    momentumDecayTime = 0;
    lastPhraseTime = 0;
    queueHead = queueTail = 0;
    historyPos = 0;
    for (int i = 0; i < 8; i++) lastPhraseIndex[i] = -1;
    
    Avatar::init();
    Weather::init();
}

const char* Mood::selectPhrase(const char* const* phrases, int count, bool trackHistory) {
    if (count == 0) return "";
    
    // avoid replaying last picks
    int attempts = 0;
    int idx;
    do {
        idx = esp_random() % count;
        attempts++;
    } while (attempts < 5 && trackHistory && 
             (lastPhraseIndex[(historyPos - 1 + 8) % 8] == idx ||
              lastPhraseIndex[(historyPos - 2 + 8) % 8] == idx));
    
    if (trackHistory) {
        lastPhraseIndex[historyPos] = idx;
        historyPos = (historyPos + 1) % 8;
    }
    
    return phrases[idx];
}

void Mood::setPhrase(const char* phrase, AvatarState state) {
    strncpy(currentPhrase, phrase, sizeof(currentPhrase) - 1);
    currentPhrase[sizeof(currentPhrase) - 1] = '\0';
    moodState = state;
    Avatar::setState(state);
    lastPhraseTime = millis();
}

void Mood::setPhraseFormatted(AvatarState state, const char* fmt, ...) {
    char buf[96];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    setPhrase(buf, state);
}

void Mood::queuePhrase(const char* phrase, AvatarState state) {
    int nextTail = (queueTail + 1) % 5;
    if (nextTail != queueHead) {
        strncpy(phraseQueue[queueTail], phrase, 63);
        phraseQueue[queueTail][63] = '\0';
        stateQueue[queueTail] = state;
        queueTail = nextTail;
    }
}

void Mood::nextFromQueue() {
    if (queueHead != queueTail) {
        setPhrase(phraseQueue[queueHead], stateQueue[queueHead]);
        phraseTimeout = 3000;  // Default timeout for queued phrases
        queueHead = (queueHead + 1) % 5;
    } else {
        memset(currentPhrase, 0, sizeof(currentPhrase));
        moodState = AvatarState::NEUTRAL;
        Avatar::setState(AvatarState::NEUTRAL);
    }
}

bool Mood::hasPhrase() {
    return currentPhrase[0] != '\0';
}

const char* Mood::getCurrentPhrase() {
    return currentPhrase;
}

void Mood::clearPhrase() {
    memset(currentPhrase, 0, sizeof(currentPhrase));
}

AvatarState Mood::getCurrentMoodState() {
    return moodState;
}

void Mood::addMomentum(int amount) {
    int oldMomentum = momentum;
    int oldMood = getEffectiveMood();
    momentum = constrain(momentum + amount, -100, 100);
    momentumDecayTime = millis() + 30000;  // 30 second decay
    Avatar::setMoodIntensity(momentum);
    
    // flash face when momentum crosses bands
    // positive: flash when crossing +15 upward
    if (oldMomentum < MOMENTUM_FLASH_POSITIVE && momentum >= MOMENTUM_FLASH_POSITIVE) {
        Display::triggerMomentumFlash(true);  // 3 cycles
    }
    // negative: flash when crossing -25 downward
    else if (oldMomentum > MOMENTUM_FLASH_NEGATIVE && momentum <= MOMENTUM_FLASH_NEGATIVE) {
        Display::triggerMomentumFlash(false);  // 1 cycle
    }
    
    // Phase B: Charged notification when crossing into HYP3 (1 in 3 chance)
    int newMood = getEffectiveMood();
    if (oldMood <= CHARGED_NOTIFICATION_THRESHOLD && newMood > CHARGED_NOTIFICATION_THRESHOLD && (esp_random() % 3) == 0) {
        setPhrase(selectPhrase(CHARGED_PHRASES, CHARGED_COUNT), AvatarState::EXCITED);
        if ((esp_random() % 2) == 0) {
            queuePhrase(selectPhrase(CHARGED_F, CHARGED_F_COUNT), AvatarState::EXCITED);
        }
        phraseTimeout = 2500;
    }
}

int Mood::getMomentum() {
    // pure getter - no side effects
    return momentum;
}

// Phase A: Get decay interval based on current context
static uint32_t baseDecayInterval() {
    // Context-aware decay rates:
    // TIERED MOMENTUM DECAY (Phase 1 - Mood Tuning)
    // Hunt/Spectrum: 4000ms (-15/min, protected during active work)
    // Sync:          10000ms (-6/min, fully protected during Pork connection)
    // IDLE:          Tiered by momentum level + urgency multiplier
    //   High (>50):  1000ms (-60/min)
    //   Mid (20-50): 2000ms (-30/min)
    //   Low (1-19):  3000ms (-20/min)
    //   Zero:        0ms (no decay)
    //   Urgency:     2min+ idle = 1.5x, 5min+ = 2.0x
    // Menu/etc:      4000ms (-15/min, same as hunt — player is engaged)
    
    HamletMode mode = Hamlet::getMode();
    
    // Hunt/Spectrum: protected (slower decay)
    if (mode == HamletMode::HUNT || mode == HamletMode::SPECTRUM) {
        return DECAY_INTERVAL_HUNT_MS;  // -15/min (was -30/min)
    }

    // FLOCK: passive radio work, mostly protected
    if (mode == HamletMode::NOWFLOCK) {
        return DECAY_INTERVAL_FLOCK_MS;  // -6/min (was -12/min)
    }
    
    // IDLE: tiered by momentum level + urgency
    if (mode == HamletMode::IDLE) {
        int currentMomentum = Mood::getMomentum();
        uint32_t baseInterval;
        
        // Tiered decay based on momentum magnitude
        int absMomentum = abs(currentMomentum);
        if (absMomentum > MOMENTUM_TIER_HIGH_THRESHOLD) {
            baseInterval = DECAY_INTERVAL_IDLE_HIGH_MS;  // High: -60/min
        } else if (absMomentum > MOMENTUM_TIER_MID_THRESHOLD) {
            baseInterval = DECAY_INTERVAL_IDLE_MID_MS;  // Mid: -30/min
        } else if (currentMomentum != 0) {
            baseInterval = DECAY_INTERVAL_IDLE_LOW_MS;  // Low: -20/min
        } else {
            return 0;  // At zero: no decay
        }
        
        // Urgency multiplier based on idle duration
        uint32_t idleDuration = Hamlet::getIdleDuration();
        if (idleDuration > IDLE_EROSION_THRESHOLD_MS) {  // 5+ min
            baseInterval = baseInterval / 2;  // 2.0x urgency
        } else if (idleDuration > 120000) {  // 2-5 min
            baseInterval = (baseInterval * 2) / 3;  // 1.5x urgency
        }
        
        return baseInterval;
    }
    
    // Menu/Loot/Brain/etc: match hunt rate (player is engaged with UI)
    return DECAY_INTERVAL_DEFAULT_MS;
}

uint32_t Mood::getDecayInterval() {
    uint32_t base = baseDecayInterval();
    // Zero is the "no decay" sentinel, not a duration — stretching it would
    // turn a frozen mood into a slowly decaying one.
    if (base == 0) return 0;
    // CALM stretches the gap between decay steps rather than freezing it. It
    // belongs here for the same reason FOCUS lives inside the effectiveness
    // multiplier: every reader should see the interval the pig actually decays
    // on, not the un-buffed rule.
    return base + ItemEffects::calmDecayBonusMs();
}

void Mood::decayMomentum() {
    // decay momentum over time (wraparound-safe)
    // ticked once per frame from main loop
    // Phase A: mode-aware decay intervals
    if (momentum != 0 && (int32_t)(millis() - momentumDecayTime) >= 0) {
        if (momentum > 0) momentum--;
        else momentum++;
        momentumDecayTime = millis() + getDecayInterval();
        Avatar::setMoodIntensity(momentum);
    }

    // mood pitch: shift creature/ambient SFX per tier
    MoodTier tier = getMoodTier();
    if (tier != lastMoodTier) {
        lastMoodTier = tier;
        // ECSTATIC +2st, HAPPY +1, CONTENT 0, MEH -1, SAD -2, DEPRESSED -3, NEGLECTED -4
        static const float pitchMap[] = { 1.12f, 1.06f, 1.00f, 0.94f, 0.89f, 0.84f, 0.79f };
        SFX::setMoodPitch(pitchMap[(int)tier]);
    }
}

void Mood::setMomentum(int value) {
    momentum = constrain(value, -100, 100);
    momentumDecayTime = millis() + 1000;  // reset decay timer
    Avatar::setMoodIntensity(momentum);
}

// ==[ LEVEL FLOOR ]==
// stable resting mood from XP level. max 60 (HAPPY). reaching HYP3 requires momentum.
static int levelFloor(uint8_t level) {
    // Lv1: -10, Lv5: 10, Lv10: 25, Lv20: 42, Lv30: 52, Lv42: 60
    if (level <= 1) return -10;
    if (level >= 42) return 60;
    // linear interpolation across key points
    const struct { uint8_t lv; int floor; } pts[] = {
        {1, -10}, {5, 10}, {10, 25}, {20, 42}, {30, 52}, {42, 60}
    };
    for (int i = 0; i < 5; i++) {
        if (level >= pts[i].lv && level <= pts[i+1].lv) {
            float t = (float)(level - pts[i].lv) / (float)(pts[i+1].lv - pts[i].lv);
            return pts[i].floor + (int)(t * (pts[i+1].floor - pts[i].floor));
        }
    }
    return 60;
}

int Mood::getEffectiveMood() {
    // level floor + session momentum
    int base = levelFloor(Config::getLevel());

    // idle erosion: high floors erode during neglect (transient, not permanent)
    // prevents high-level players from being permanently happy. resets on activity.
    uint32_t idleDuration = Hamlet::getIdleDuration();
    if (idleDuration > IDLE_EROSION_THRESHOLD_MS && base > 0) {  // 5+ min idle, positive floors only
        int erosion = (int)((idleDuration - IDLE_EROSION_THRESHOLD_MS) / IDLE_EROSION_DIVISOR_MS) * IDLE_EROSION_PER_MINUTE;  // -5 per minute
        if (erosion > 30) erosion = 30;  // cap: Lv42 floor 60 → 30 after 11min
        base -= erosion;
    }

    return constrain(base + momentum, -100, 100);
}

// effectiveness multiplier: level baseline + momentum bonus + goal bonus
// clamped 0.5x-1.5x
float Mood::getEffectivenessMultiplier() {
    // level baseline (0.9x-1.2x)
    uint8_t level = Config::getLevel();
    float levelMult;
    if (level >= 35) levelMult = 1.2f;
    else if (level >= 21) levelMult = 1.1f;
    else if (level >= 7)  levelMult = 1.0f;
    else levelMult = 0.9f;

    // momentum bonus (-0.2x to +0.3x)
    float momBonus = 0.0f;
    if (momentum > 50)       momBonus = 0.3f;
    else if (momentum > 20)  momBonus = 0.15f;
    else if (momentum > 0)   momBonus = 0.05f;
    else if (momentum < -30) momBonus = -0.2f;
    else if (momentum < -10) momBonus = -0.1f;

    // goal bonus (0-0.1x)
    uint8_t goalProgress = Config::getGoalProgress();
    float goalBonus = 0.0f;
    if (goalProgress >= 100) goalBonus = 0.1f;
    else if (goalProgress >= 80) goalBonus = 0.05f;

    float result = levelMult + momBonus + goalBonus;
    // A burned FOCUS exhibit stacks on top of level and mood. It is clamped
    // with everything else, so evidence can push toward the ceiling but never
    // through it.
    result *= ItemEffects::focusMultiplier();
    if (result < 0.5f) result = 0.5f;
    if (result > 1.5f) result = 1.5f;
    return result;
}

MoodTier Mood::getMoodTier() {
    return getMoodTier(getEffectiveMood());
}

MoodTier Mood::getMoodTier(int mood) {
    if (mood >= MOOD_TIER_ECSTATIC_MIN) return MoodTier::ECSTATIC;
    if (mood >= MOOD_TIER_HAPPY_MIN) return MoodTier::HAPPY;
    if (mood >= MOOD_TIER_CONTENT_MIN) return MoodTier::CONTENT;
    if (mood >= MOOD_TIER_MEH_MIN) return MoodTier::MEH;
    if (mood >= MOOD_TIER_SAD_MIN) return MoodTier::SAD;
    if (mood >= MOOD_TIER_DEPRESSED_MIN) return MoodTier::DEPRESSED;
    return MoodTier::NEGLECTED;
}

const char* Mood::getMoodLabel() {
    int mood = getEffectiveMood();
    if (mood > 70) return "HYP3";
    if (mood > 30) return "GUD";
    if (mood > -10) return "0K";
    if (mood > -50) return "M3H";
    return "S4D";
}

// Phase B: Mood label with charge indicator
const char* Mood::getMoodLabelWithCharge() {
    int mood = getEffectiveMood();
    // HYP3 tier shows lightning bolt (charged status)
    if (mood > 70) return "HYP3\xE2\x9A\xA1";  // ⚡ UTF-8 encoded
    if (mood > 30) return "GUD";
    if (mood > -10) return "0K";
    if (mood > -50) return "M3H";
    return "S4D";
}

// event triggers

static void reconcileBootAchievements() {
    uint16_t currentStreak = Config::getStreak();
    if (currentStreak >= 10) Achievements::tryUnlock(Achievement::DEDICATED);
    if (currentStreak >= 25) Achievements::tryUnlock(Achievement::LOYAL);
    if (currentStreak >= 50) Achievements::tryUnlock(Achievement::OBSESSED);

    uint8_t level = Config::getLevel();
    if (level >= 7)  Achievements::tryUnlock(Achievement::RANK_SHOAT);
    if (level >= 14) Achievements::tryUnlock(Achievement::RANK_BOAR);
    if (level >= 21) Achievements::tryUnlock(Achievement::RANK_TUSKER);
    if (level >= 28) Achievements::tryUnlock(Achievement::RANK_WARTHOG);
    if (level >= 35) Achievements::tryUnlock(Achievement::RANK_RAZORBACK);
    if (level >= 42) Achievements::tryUnlock(Achievement::RANK_ELDER);

    uint32_t lifetimePMKID = Config::getTotalPMKIDs();
    uint32_t lifetimeHS = Config::getTotalHandshakes();
    uint32_t lifetimeTotal = lifetimePMKID + lifetimeHS;
    if (lifetimeTotal >= 1)   Achievements::tryUnlock(Achievement::FIRST_BLOOD);
    if (lifetimePMKID >= 10)  Achievements::tryUnlock(Achievement::HUNTER);
    if (lifetimeHS >= 10)     Achievements::tryUnlock(Achievement::HANDSHAKER);
    if (lifetimeTotal >= 100) Achievements::tryUnlock(Achievement::CENTURION);
}

void Mood::onBoot() {
    // Goal-success copy returns early, so reconcile trophies before it branches.
    reconcileBootAchievements();

    // goal met last session? pat before mood select
    if (Config::wasGoalMetLastSession()) {
        setPhrase(selectPhrase(GOAL_MET_PHRASES, GOAL_MET_COUNT), AvatarState::HAPPY);
        phraseTimeout = 4000;
        Config::clearGoalMetFlag();
        {
            char nextBuf[32];
            snprintf(nextBuf, sizeof(nextBuf), "next step goal: %u", Config::getGoalTarget());
            queuePhrase(nextBuf, AvatarState::NEUTRAL);
        }
        return;
    }

    // select phrase based on tier
    MoodTier tier = getMoodTier();
    uint16_t streak = Config::getStreak();
    char buf[64] = {0};
    const char* phrase = nullptr;
    AvatarState state = AvatarState::NEUTRAL;

    switch (tier) {
        case MoodTier::ECSTATIC:
            phrase = selectPhrase(BOOT_ECSTATIC, BOOT_ECSTATIC_COUNT);
            state = AvatarState::EXCITED;
            break;
        case MoodTier::HAPPY:
            phrase = selectPhrase(BOOT_HAPPY, BOOT_HAPPY_COUNT);
            state = AvatarState::HAPPY;
            break;
        case MoodTier::CONTENT:
            phrase = selectPhrase(BOOT_CONTENT, BOOT_CONTENT_COUNT);
            state = AvatarState::NEUTRAL;
            break;
        case MoodTier::MEH:
            phrase = selectPhrase(BOOT_MEH, BOOT_MEH_COUNT);
            state = AvatarState::SLEEPY;
            break;
        case MoodTier::SAD:
            phrase = selectPhrase(BOOT_SAD, BOOT_SAD_COUNT);
            state = AvatarState::SAD;
            break;
        case MoodTier::DEPRESSED:
            phrase = selectPhrase(BOOT_DEPRESSED, BOOT_DEPRESSED_COUNT);
            state = AvatarState::SAD;
            break;
        case MoodTier::NEGLECTED:
            phrase = selectPhrase(BOOT_NEGLECTED, BOOT_NEGLECTED_COUNT);
            state = AvatarState::SAD;
            break;
    }

    if (phrase && strstr(phrase, "%d")) {
        snprintf(buf, sizeof(buf), phrase, streak);
        setPhrase(buf, state);
    } else if (phrase) {
        setPhrase(phrase, state);
    } else {
        setPhrase(selectPhrase(BOOT_PHRASES, BOOT_COUNT), AvatarState::HAPPY);
    }
    phraseTimeout = 4000;

    // ==[ BOOT CONTINUATION ]== 50% chance: queue paired second beat
    if ((esp_random() % 2) == 0) {
        const char* const* followArr = nullptr;
        int followCount = 0;
        switch (tier) {
            case MoodTier::ECSTATIC:  followArr = BOOT_ECSTATIC_F;  followCount = 6; break;
            case MoodTier::HAPPY:     followArr = BOOT_HAPPY_F;     followCount = 6; break;
            case MoodTier::CONTENT:   followArr = BOOT_CONTENT_F;   followCount = 6; break;
            case MoodTier::MEH:       followArr = BOOT_MEH_F;       followCount = 6; break;
            case MoodTier::SAD:       followArr = BOOT_SAD_F;       followCount = 6; break;
            case MoodTier::DEPRESSED: followArr = BOOT_DEPRESSED_F; followCount = 6; break;
            case MoodTier::NEGLECTED: followArr = BOOT_NEGLECTED_F; followCount = 6; break;
        }
        if (followArr) {
            queuePhrase(selectPhrase(followArr, followCount), state);
        }
    }

    // streak at risk warning (queued after boot phrase)
    if (Config::isStreakAtRisk()) {
        static const char* const STREAK_WARNING[] = {
            "last run missed goal.\nstreak at risk.",
            "idle boot logged.\nnext miss decays streak.",
            "streak warning.\nledger stays literal.",
            "one more miss.\nstreak count falls.",
            "streak on thin ice.\nclock keeps moving."
        };
        queuePhrase(selectPhrase(STREAK_WARNING, 5), AvatarState::NEUTRAL);
    }
}

void Mood::onWalkStart() {
    setPhrase(selectPhrase(WALK_START_PHRASES, WALK_START_COUNT), AvatarState::HAPPY);
    if ((esp_random() % 2) == 0) {
        queuePhrase(selectPhrase(WALK_START_F, WALK_START_F_COUNT), AvatarState::HAPPY);
    }
    phraseTimeout = 3000;
    addMomentum(MOMENTUM_WALK_START);
}

void Mood::onHuntStart() {
    setPhrase(selectPhrase(HUNT_START_PHRASES, HUNT_START_COUNT), AvatarState::HUNTING);
    if ((esp_random() % 2) == 0) {
        queuePhrase(selectPhrase(HUNT_START_F, HUNT_START_F_COUNT), AvatarState::HUNTING);
    }
    // grass anim now tied to walking in hamlet.cpp, not mode
    phraseTimeout = 3000;
    addMomentum(MOMENTUM_HUNT_START);
    Avatar::sniff();  // eager sniff when hunt begins
}

void Mood::onHuntStop() {
    // ==[ SESSION DEBRIEF ]== stash for DEFHOG4 injection
    debrief.pmkids = Config::getSessionPMKIDCount();
    debrief.handshakes = Config::getSessionHSCount();
    debrief.probes = Hunt::getProbeCount();
    debrief.deauths = Hunt::getDeauthCount();
    debrief.maxCombo = Hunt::getCaptureComboCount();
    debrief.xpGained = Config::getSessionXPGained();
    debrief.pending = true;

    // peak-end: if session had captures, acknowledge the haul
    uint8_t pmkids = Config::getSessionPMKIDCount();
    uint8_t shakes = Config::getSessionHSCount();
    uint8_t total = pmkids + shakes;
    if (total > 0) {
        setPhraseFormatted(AvatarState::HAPPY, "%d captures.\nPSRAM looks smug.", total);
    } else {
        setPhrase(selectPhrase(HUNT_STOP_PHRASES, HUNT_STOP_COUNT), AvatarState::NEUTRAL);
        if ((esp_random() % 2) == 0) {
            queuePhrase(selectPhrase(HUNT_STOP_F, HUNT_STOP_F_COUNT), AvatarState::NEUTRAL);
        }
    }
    phraseTimeout = 3000;
}

void Mood::onHuntEnd() {
    // Alias for onHuntStop
    onHuntStop();
}

void Mood::onPMKIDCapture() {
    uint8_t combo = Hunt::getCaptureComboCount();

    setPhrase(selectPhrase(PMKID_PHRASES, PMKID_COUNT), AvatarState::EXCITED);
    if ((esp_random() % 2) == 0) {
        queuePhrase(selectPhrase(PMKID_F, PMKID_F_COUNT), AvatarState::EXCITED);
    }

    // ==[ HEAT CHAIN ]== combo multiplier on momentum
    int momBonus = (combo >= 2) ? min((int)(combo - 1), 4) * 3 : 0;
    addMomentum(MOMENTUM_PMKID_BASE + momBonus);

    // graduated XP: 30/20/10 based on session count
    uint8_t sessionCount = Config::getSessionPMKIDCount();
    uint32_t xp = 30;
    if (sessionCount >= 3 && sessionCount < 10) xp = 20;
    else if (sessionCount >= 10) xp = 10;

    // ==[ HEAT CHAIN ]== combo multiplier on XP (cap 1.5x)
    if (combo >= 2) {
        float mult = 1.0f + 0.1f * min((int)(combo - 1), 5);
        xp = (uint32_t)(xp * mult);
    }
    Config::addXP(xp);

    // ==[ LOOT DROP ]== 15% bonus XP on any capture
    if ((esp_random() % 100) < 15) {
        uint32_t bonusXP = 10;
        Config::addXP(bonusXP);
        ItemDrops::award(ItemDrops::ItemDropSource::CAPTURE, combo);
        Avatar::triggerSparkles(4);
        Haptic::buzz();
        if (DefhogTerminal::isVisible()) {
            DefhogTerminal::pushLineHype("L00T DR0P! +%dXP", (int)bonusXP);
        }
    }

    // ==[ HEAT CHAIN ]== visual escalation
    if (combo >= 2) Avatar::triggerSparkles(combo);

    // DEFHOG4 chain status
    if (combo >= 2 && DefhogTerminal::isVisible()) {
        DefhogTerminal::pushLineHype("CHAIN x%d +%luXP", combo, xp);
    }

    phraseTimeout = 4000;
    Avatar::sniff();
    Avatar::attackHop();
    Avatar::triggerTailWiggle();

    // ==[ BOUNTY WINDOW ]== two captures inside one rolling 6h slot
    Bounty::onCapture();

    // ==[ ACHIEVEMENT CHECKS ]== capture milestones + chain
    uint32_t lifetimePMKID = Config::getTotalPMKIDs();
    uint32_t lifetimeTotal = lifetimePMKID + Config::getTotalHandshakes();
    if (lifetimeTotal == 1) Achievements::tryUnlock(Achievement::FIRST_BLOOD);
    if (lifetimePMKID >= 10) Achievements::tryUnlock(Achievement::HUNTER);
    if (lifetimeTotal >= 100) Achievements::tryUnlock(Achievement::CENTURION);
    if (combo >= 3) Achievements::tryUnlock(Achievement::CHAIN_X3);
    if (combo >= 5) Achievements::tryUnlock(Achievement::CHAIN_X5);
    if (combo >= 10) Achievements::tryUnlock(Achievement::CHAIN_X10);
    if (M5.Power.getBatteryLevel() <= 10) Achievements::tryUnlock(Achievement::CLUTCH);
    // crowd density achievements
    if (DefensePipeline::snapshot().getEstimatedPopulation() >= 50) Achievements::tryUnlock(Achievement::URBAN_JUNGLE);
    if (DefensePipeline::snapshot().isDeserted()) Achievements::tryUnlock(Achievement::LONE_WOLF);

    // ==[ CHALLENGE PROGRESS ]==
    Challenges::onPMKIDCapture();
    if (combo >= 2) Challenges::onChainReached(combo);
}

void Mood::onPMKID() {
    // alias for onPMKIDCapture
    onPMKIDCapture();
}

void Mood::onHandshakeCapture(const char* netName) {
    uint8_t combo = Hunt::getCaptureComboCount();

    char context[12];
    if (MoodContext::copyDisplay(netName, context, sizeof(context), 10)) {
        char buf[64];
        snprintf(buf, sizeof(buf), selectPhrase(SUCCESS_PHRASES, SUCCESS_COUNT), context);
        setPhrase(buf, AvatarState::EXCITED);
    } else {
        setPhrase(selectPhrase(HANDSHAKE_PHRASES, HANDSHAKE_COUNT), AvatarState::EXCITED);
    }
    if ((esp_random() % 2) == 0) {
        queuePhrase(selectPhrase(HANDSHAKE_F, HANDSHAKE_F_COUNT), AvatarState::EXCITED);
    }

    // ==[ HEAT CHAIN ]== combo multiplier on momentum
    int momBonus = (combo >= 2) ? min((int)(combo - 1), 4) * 3 : 0;
    addMomentum(MOMENTUM_HANDSHAKE_BASE + momBonus);

    // graduated XP: 40/30/20/10 based on session count
    uint8_t sessionCount = Config::getSessionHSCount();
    uint32_t xp = 40;
    if (sessionCount >= 3 && sessionCount < 8) xp = 30;
    else if (sessionCount >= 8 && sessionCount < 15) xp = 20;
    else if (sessionCount >= 15) xp = 10;

    // ==[ HEAT CHAIN ]== combo multiplier on XP (cap 1.5x)
    if (combo >= 2) {
        float mult = 1.0f + 0.1f * min((int)(combo - 1), 5);
        xp = (uint32_t)(xp * mult);
    }
    Config::addXP(xp);

    // ==[ LOOT DROP ]== 15% bonus XP on handshake capture
    if ((esp_random() % 100) < 15) {
        uint32_t bonusXP = 15;  // HS loot drop slightly higher
        Config::addXP(bonusXP);
        ItemDrops::award(ItemDrops::ItemDropSource::HANDSHAKE, combo + 1);
        Avatar::triggerSparkles(4);
        Haptic::buzz();
        if (DefhogTerminal::isVisible()) {
            DefhogTerminal::pushLineHype("L00T DR0P! +%dXP", (int)bonusXP);
        }
    }

    // ==[ HEAT CHAIN ]== visual escalation
    if (combo >= 2) Avatar::triggerSparkles(combo);

    // DEFHOG4 chain status
    if (combo >= 2 && DefhogTerminal::isVisible()) {
        DefhogTerminal::pushLineHype("CHAIN x%d +%luXP", combo, xp);
    }

    phraseTimeout = 5000;
    Avatar::wiggleEars();
    Avatar::cuteJump();

    // ==[ BOUNTY WINDOW ]== two captures inside one rolling 6h slot
    Bounty::onCapture();

    // ==[ ACHIEVEMENT CHECKS ]== handshake milestones + chain
    uint32_t lifetimeHS = Config::getTotalHandshakes();
    uint32_t lifetimeTotal = Config::getTotalPMKIDs() + lifetimeHS;
    if (lifetimeTotal == 1) Achievements::tryUnlock(Achievement::FIRST_BLOOD);
    if (lifetimeHS >= 10) Achievements::tryUnlock(Achievement::HANDSHAKER);
    if (lifetimeTotal >= 100) Achievements::tryUnlock(Achievement::CENTURION);
    if (combo >= 3) Achievements::tryUnlock(Achievement::CHAIN_X3);
    if (combo >= 5) Achievements::tryUnlock(Achievement::CHAIN_X5);
    if (combo >= 10) Achievements::tryUnlock(Achievement::CHAIN_X10);
    if (M5.Power.getBatteryLevel() <= 10) Achievements::tryUnlock(Achievement::CLUTCH);
    // crowd density achievements
    if (DefensePipeline::snapshot().getEstimatedPopulation() >= 50) Achievements::tryUnlock(Achievement::URBAN_JUNGLE);
    if (DefensePipeline::snapshot().isDeserted()) Achievements::tryUnlock(Achievement::LONE_WOLF);

    // ==[ CHALLENGE PROGRESS ]==
    Challenges::onHandshakeCapture();
    if (combo >= 2) Challenges::onChainReached(combo);
}

// ==[ WARDRIVE EVENTS ]== wardriving mood + phrases

static const char* const WARDRIVE_PHRASES[] = {
    "new BSSID mapped.\ngrid gets another scar.",
    "beacon geotagged.\ncity signs in hex.",
    "map gained one witness.",
    "atlas expands.\nSSID stays only a name.",
    "grid cell paid in beacons.",
    "BSSID booked.\ncoordinates keep custody.",
    "one more AP on the city file.",
    "signal mapped.\nroad keeps moving.",
};
static constexpr int WARDRIVE_PHRASE_COUNT = sizeof(WARDRIVE_PHRASES) / sizeof(WARDRIVE_PHRASES[0]);

static const char* const WARDRIVE_MILESTONE_PHRASES[] = {
    "100 mapped.\ncity file gets heavy.",
    "500 mapped.\ngrid knows the pig.",
    "1000 mapped.\ncartography gets teeth.",
    "50 mapped.\nengine is warm.",
};
static constexpr int WARDRIVE_MILESTONE_COUNT = sizeof(WARDRIVE_MILESTONE_PHRASES) / sizeof(WARDRIVE_MILESTONE_PHRASES[0]);

static const char* const WARDRIVE_END_PHRASES[] = {
    "wardrive closed.\nmap updated.",
    "session logged.\nroute has receipts.",
    "wardrive complete.\nradio leaves the road.",
    "mapping stopped.\ndata stays on file.",
};
static constexpr int WARDRIVE_END_COUNT = sizeof(WARDRIVE_END_PHRASES) / sizeof(WARDRIVE_END_PHRASES[0]);

void Mood::onWardriveNetwork() {
    setPhrase(selectPhrase(WARDRIVE_PHRASES, WARDRIVE_PHRASE_COUNT), AvatarState::HAPPY);
    addMomentum(MOMENTUM_WARDRIVE_NETWORK);
    phraseTimeout = 2500;
}

void Mood::onWardriveMilestone(uint16_t count) {
    // pick milestone-appropriate phrase
    int idx = 0;
    if (count >= 1000)     idx = 2;
    else if (count >= 500) idx = 1;
    else if (count >= 100) idx = 0;
    else                   idx = 3;

    if (idx < WARDRIVE_MILESTONE_COUNT) {
        setPhrase(WARDRIVE_MILESTONE_PHRASES[idx], AvatarState::EXCITED);
    }
    addMomentum(MOMENTUM_WARDRIVE_MILESTONE);
    Avatar::triggerSparkles(4);
    SFX::play(SFX::LEVEL_UP);
    phraseTimeout = 4000;
}

void Mood::onWardriveEnd() {
    setPhrase(selectPhrase(WARDRIVE_END_PHRASES, WARDRIVE_END_COUNT), AvatarState::HAPPY);
    phraseTimeout = 3000;
}

// ==[ NEAR-MISS ]== partial handshake expired without M4
static uint32_t lastNearMissTime = UINT32_MAX;  // UINT32_MAX = never fired; first near-miss always passes
static uint8_t sessionNearMissCount = 0;

static const char* const NEAR_MISS_PHRASES[] = {
    "partial EAPOL.\nmessage missing.",
    "handshake incomplete.\nmask shows the wound.",
    "EAPOL partial.\nnot crackable.",
    "target left frames short.",
    "message set incomplete.",
    "frames arrived.\ncrackability didnt.",
    "partial seen.\nnot called a win.",
    "M1-M4 mask incomplete.",
};
static constexpr int NEAR_MISS_COUNT = sizeof(NEAR_MISS_PHRASES) / sizeof(NEAR_MISS_PHRASES[0]);

void Mood::onNearMiss(uint8_t capturedMask, const char* ssid) {
    uint32_t now = millis();
    // 30s cooldown to prevent spam
    if (now - lastNearMissTime < 30000) return;
    // suppress during active chain (don't dilute chain signal)
    if (Hunt::getCaptureComboCount() >= 2) return;

    lastNearMissTime = now;

    // count captured messages
    uint8_t msgCount = 0;
    for (uint8_t b = 0; b < 4; b++) if (capturedMask & (1 << b)) msgCount++;

    char context[16];
    bool hasContext = MoodContext::copyDisplay(ssid, context, sizeof(context), 14);
    if (hasContext) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%d/4 frames on %s.\nnot crackable.", msgCount, context);
        setPhrase(buf, AvatarState::SAD);
    } else {
        setPhrase(selectPhrase(NEAR_MISS_PHRASES, NEAR_MISS_COUNT), AvatarState::SAD);
    }

    // positive momentum — partial win framing (Clark 2009)
    addMomentum(MOMENTUM_NEAR_MISS);
    phraseTimeout = 3000;
    Avatar::flinch();
    SFX::play(SFX::NEAR_MISS);                                    // descending 900→380Hz chirp
    Display::showAlertToast("SIGNAL LOST", 1200, 3);              // flash 3× — aversive tension hook
    sessionNearMissCount++;

    // ==[ NEAR-MISS ACHIEVEMENTS ]==
    if (sessionNearMissCount >= 5) Achievements::tryUnlock(Achievement::SO_CLOSE);
    if (sessionNearMissCount >= 10) Achievements::tryUnlock(Achievement::PERSISTENT);

    // DEFHOG4 near-miss alert
    if (DefhogTerminal::isVisible()) {
        if (hasContext) {
            DefhogTerminal::pushLineAlert("NEAR-MISS %s %d/4 frames", context, msgCount);
        } else {
            DefhogTerminal::pushLineAlert("NEAR-MISS %d/4 frames", msgCount);
        }
    }
}

void Mood::onFirstCapture() {
    setPhrase(selectPhrase(FIRST_CATCH_PHRASES, FIRST_CATCH_COUNT), AvatarState::EXCITED);
    if ((esp_random() % 2) == 0) {
        queuePhrase(selectPhrase(FIRST_CATCH_F, FIRST_CATCH_F_COUNT), AvatarState::EXCITED);
    }
    addMomentum(MOMENTUM_FIRST_CAPTURE);
    Config::addXP(20);  // first capture of session bonus
    phraseTimeout = 4000;
    Avatar::sniff();
}

void Mood::onFirstDiscovery(uint8_t authType) {
    // first capture of an auth type. case file unlocked. +25 XP.
    const char* typeName = "UNKNOWN";
    switch (authType) {
        case 0: typeName = "OPEN"; break;
        case 1: typeName = "WEP"; break;
        case 2: typeName = "WPA-PSK"; break;
        case 3: typeName = "WPA2-PSK"; break;
        case 4: typeName = "WPA/WPA2"; break;
        case 5: typeName = "WPA2-ENT"; break;
        case 6: typeName = "WPA3-PSK"; break;
        case 7: typeName = "WPA2/WPA3"; break;
        case 8: typeName = "WAPI"; break;
        case 9: typeName = "OWE"; break;
    }
    char buf[64];
    snprintf(buf, sizeof(buf), "NEW CASE FILE:\n%s +25XP", typeName);
    setPhrase(buf, AvatarState::EXCITED);
    queuePhrase("auth family logged.\n+25 XP booked.", AvatarState::HAPPY);
    phraseTimeout = 4000;
    Config::addXP(25);
    addMomentum(MOMENTUM_FIRST_DISCOVERY);
    Avatar::triggerSparkles(6);
    SFX::play(SFX::ACHIEVEMENT_UNLOCK);
    Haptic::pulse();
}

void Mood::onNewNetwork() {
    Config::addXP(2);  // discovery drip — new SSID = earned signal (sim: #8 sensitivity)
    // 1 in 3 chance
    if ((esp_random() % 3) == 0) {
        setPhrase(selectPhrase(NEW_NET_PHRASES, NEW_NET_COUNT), AvatarState::HAPPY);
        phraseTimeout = 2000;
        Avatar::sniff();
        Avatar::perkUp();   // ears pop — "new signal!"
    }
}

void Mood::onProbeAttempt() {
    // Effort drip (sim: #7 sensitivity). Deliberately silent — no sound, no
    // haptic, no phrase of its own. The only feedback is the XP bar moving,
    // which is what makes a dry patch feel like work instead of dead air.
    Config::addXP(3);

    // 1 in 4 chance
    if ((esp_random() % 4) == 0) {
        setPhrase(selectPhrase(PROBE_PHRASES, PROBE_COUNT), AvatarState::HUNTING);
        phraseTimeout = 1500;
    }
}

void Mood::onBufferFull() {
    setPhrase(selectPhrase(BUFFER_PHRASES, BUFFER_COUNT), AvatarState::SAD);
    phraseTimeout = 4000;
}

void Mood::onBufferFilling() {
    setPhrase(selectPhrase(BUFFER_FILLING_PHRASES, BUFFER_FILLING_COUNT), AvatarState::NEUTRAL);
    phraseTimeout = 3000;
}

void Mood::onSyncStart() {
    setPhrase(selectPhrase(SYNC_START_PHRASES, SYNC_START_COUNT), AvatarState::NEUTRAL);
    if ((esp_random() % 2) == 0) {
        queuePhrase(selectPhrase(SYNC_START_F, SYNC_START_F_COUNT), AvatarState::NEUTRAL);
    }
    phraseTimeout = 2000;
}

void Mood::onSyncComplete(bool success, uint16_t captureCount) {
    static uint32_t lastSyncTime = 0;
    uint32_t now = millis();
    
    if (success) {
        if (captureCount == 0) {
            uint32_t timeSinceLastSync = now - lastSyncTime;
            if (timeSinceLastSync < 1800000) {
                setPhrase(selectPhrase(EMPTY_SYNC_PHRASES, EMPTY_SYNC_COUNT), AvatarState::NEUTRAL);
            } else {
                setPhrase("zero captures. case went cold.", AvatarState::SAD);
                addMomentum(MOMENTUM_EMPTY_SYNC_PENALTY);
            }
        } else {
            setPhrase(selectPhrase(SYNC_SUCCESS_PHRASES, SYNC_SUCCESS_COUNT), AvatarState::HAPPY);
            if ((esp_random() % 2) == 0) {
                queuePhrase(selectPhrase(SYNC_SUCCESS_F, SYNC_SUCCESS_F_COUNT), AvatarState::HAPPY);
            }
            addMomentum(MOMENTUM_SYNC_SUCCESS);
            // scaled sync XP: 30 + 10*min(N,10)
            uint32_t syncXP = 30 + 10 * min((uint16_t)10, captureCount);
            Config::addXP(syncXP);
        }

        lastSyncTime = now;
    } else {
        setPhrase(selectPhrase(SYNC_FAIL_PHRASES, SYNC_FAIL_COUNT), AvatarState::SAD);
        if ((esp_random() % 2) == 0) {
            queuePhrase(selectPhrase(SYNC_FAIL_F, SYNC_FAIL_F_COUNT), AvatarState::SAD);
        }
        addMomentum(MOMENTUM_SYNC_FAIL_PENALTY);
    }
    phraseTimeout = 3000;
}

void Mood::onLowBattery() {
    setPhrase(selectPhrase(LOW_BATTERY_PHRASES, LOW_BATTERY_COUNT), AvatarState::SLEEPY);
    phraseTimeout = 4000;
}

void Mood::onEmergencySync() {
    setPhrase(selectPhrase(EMERGENCY_SYNC_PHRASES, EMERGENCY_SYNC_COUNT), AvatarState::SAD);
    phraseTimeout = 6000;  // long timeout - critical
}

void Mood::onSleepBlocked() {
    setPhrase(selectPhrase(SLEEP_BLOCKED_PHRASES, SLEEP_BLOCKED_COUNT), AvatarState::SAD);
    phraseTimeout = 5000;
}

// ==[ ACHIEVEMENT PROGRESS HINTS ]== zeigarnik effect: hint at near-complete achievements
// returns true if hint was written to outBuf
static bool buildAchievementHint(char* outBuf, size_t outLen) {
    // gather candidates: achievements >50% progressed but not yet unlocked
    struct HintCandidate { uint32_t have; uint32_t need; const char* name; };
    HintCandidate pool[8];
    int poolCount = 0;

    auto addCandidate = [&](Achievement ach, uint32_t current, uint32_t target, const char* name) {
        if (!Achievements::has(ach) && current >= target / 2 && current < target && poolCount < 8) {
            pool[poolCount++] = {current, target, name};
        }
    };

    uint32_t pmkids = Config::getTotalPMKIDs();
    uint32_t hs = Config::getTotalHandshakes();
    uint32_t total = pmkids + hs;
    uint16_t streak = Config::getStreak();
    uint32_t wd = Config::getWDTotal();
    uint32_t bleSeen = DefensePipeline::snapshot().getTotalBLEDevicesSeen();

    addCandidate(Achievement::HUNTER,       pmkids, 10,   "HUNT3R");
    addCandidate(Achievement::HANDSHAKER,   hs,     10,   "H4NDSH4K3R");
    addCandidate(Achievement::CENTURION,    total,  100,  "C3NTUR10N");
    addCandidate(Achievement::DEDICATED,    streak, 10,   "D3D1C4T3D");
    addCandidate(Achievement::LOYAL,        streak, 25,   "L0Y4L");
    addCandidate(Achievement::GRID_WALKER,  wd,     1000, "GR1D_W4LK3R");
    addCandidate(Achievement::PIG_EARS,     bleSeen, 25,  "P1G_34RS");
    // TAG_COLLECTOR tracks distinct threat families in Hamlet's session mask,
    // not the current tracker count. No fake progress hint without that mask.

    if (poolCount == 0) return false;

    // pick random candidate
    HintCandidate& pick = pool[esp_random() % poolCount];
    snprintf(outBuf, outLen, "%lu logged.\n%lu more closes %s.",
             (unsigned long)pick.have, (unsigned long)(pick.need - pick.have), pick.name);
    return true;
}

// ==[ CONTEXT-AWARE HUNT PHRASES ]==
// reads live hunt state, picks weighted category, formats into buf
static bool buildContextHuntPhrase(char* buf, size_t len, char* buf2, size_t len2) {
    if (!Hunt::isActive()) return false;

    uint8_t ch = Hunt::getCurrentChannel();
    uint16_t nets = Hunt::getNetworkCount();
    uint16_t clients = Hunt::getClientCount();
    uint16_t loot = Hunt::getSessionPMKIDs() + Hunt::getSessionHandshakes();
    uint16_t pmkids = Hunt::getSessionPMKIDs();
    uint16_t hs = Hunt::getSessionHandshakes();
    uint16_t deauths = Hunt::getDeauthCount();
    uint16_t probes = Hunt::getProbeCount();
    uint16_t harvested = Hunt::getHarvestedCount();
    float km = Pedometer::getDistanceKm();
    uint32_t steps = Config::getSessionSteps();
    HuntBehavior behavior = Hunt::getCurrentBehavior();

    const ChannelStats* cstats = Hunt::getChannelStats(ch);
    uint16_t chNets = cstats ? cstats->networkCount : 0;
    uint16_t chClients = cstats ? cstats->clientCount : 0;

    // category weights: build pool
    // 0=SESSION 1=CHANNEL 2=BEHAVIOR 3=DROUGHT 4=STREAK 5=DISTANCE 6=ATTACK 7=DRY_WIT
    uint8_t pool[16];
    uint8_t poolSize = 0;

    // always-on categories (weight 1)
    pool[poolSize++] = 0; // session stats
    pool[poolSize++] = 1; // channel
    pool[poolSize++] = 2; // behavior
    pool[poolSize++] = 7; // dry wit

    // conditional double-weight
    if (loot == 0 && nets > 3) { pool[poolSize++] = 3; pool[poolSize++] = 3; } // drought x2
    if (loot > 0) { pool[poolSize++] = 4; pool[poolSize++] = 4; }              // streak x2
    if (km > 0.05f || steps > 50) { pool[poolSize++] = 5; }                    // distance
    if (deauths > 0) { pool[poolSize++] = 6; if (deauths > 10) pool[poolSize++] = 6; } // attack

    uint8_t cat = pool[esp_random() % poolSize];
    uint8_t pick;

    switch (cat) {
    case 0: { // SESSION_STATS
        static const char* const T[] = {
            "ch%d. %d nets. %d captures.\nradio keeps books.",
            "%d nets questioned. %d talked.",
            "%d probes sent. %d captures.\ndifferent ledgers.",
            "session: %d nets, %d captures.\nPSRAM knows.",
            "%d APs seen. %d harvested.\nfilter earned rent.",
            "%d nets. %d captures.\nmath stays unsentimental.",
            "ch%d. %d signals. %d captures.\nsmall screen, real work."
        };
        pick = esp_random() % 7;
        switch (pick) {
        case 0: snprintf(buf, len, T[0], ch, nets, loot); break;
        case 1: snprintf(buf, len, T[1], nets, loot); break;
        case 2: snprintf(buf, len, T[2], probes, loot); break;
        case 3: snprintf(buf, len, T[3], nets, loot); break;
        case 4: snprintf(buf, len, T[4], nets, harvested); break;
        case 5: snprintf(buf, len, T[5], nets, loot); break;
        case 6: snprintf(buf, len, T[6], ch, nets, loot); break;
        }
        break;
    }
    case 1: { // CHANNEL
        static const char* const T[] = {
            "ch%d: %d APs, %d clients.\ncrowded office.",
            "ch%d held. %d nets total.\nD-UCB chose the chair.",
            "ch%d: %d local APs.\nbeacons keep arriving.",
            "ch%d has %d clients.\nEAPOL desk stays open.",
            "parked on ch%d.\n%d APs breathing.",
            "ch%d. %d clients total.\ncity has company.",
            "ch%d paid before.\n%d APs still testify."
        };
        pick = esp_random() % 7;
        switch (pick) {
        case 0: snprintf(buf, len, T[0], ch, chNets, chClients); break;
        case 1: snprintf(buf, len, T[1], ch, nets); break;
        case 2: snprintf(buf, len, T[2], ch, chNets); break;
        case 3: snprintf(buf, len, T[3], ch, chClients); break;
        case 4: snprintf(buf, len, T[4], ch, chNets); break;
        case 5: snprintf(buf, len, T[5], ch, clients); break;
        case 6: snprintf(buf, len, T[6], ch, chNets); break;
        }
        break;
    }
    case 2: { // BEHAVIOR
        switch (behavior) {
        case HuntBehavior::CAMP: {
            static const char* const T[] = {
                "CAMP on ch%d.\n%d APs get a full hearing.",
                "ch%d held.\n%d nets total, dwell widened.",
                "CAMP: ch%d locked.\npatience clocks in.",
                "camped on ch%d.\n%d local APs.",
                "stationary ch%d.\n%d nets across the case.",
                "planted on ch%d.\n%d APs keep talking."
            };
            pick = esp_random() % 6;
            switch (pick) {
            case 0: snprintf(buf, len, T[0], ch, chNets); break;
            case 1: snprintf(buf, len, T[1], ch, nets); break;
            case 2: snprintf(buf, len, T[2], ch); break;
            case 3: snprintf(buf, len, T[3], ch, chNets); break;
            case 4: snprintf(buf, len, T[4], ch, nets); break;
            case 5: snprintf(buf, len, T[5], ch, chNets); break;
            }
            break;
        }
        case HuntBehavior::PATROL: {
            static const char* const T[] = {
                "odometer %.1f km.\n%d nets this session.",
                "walking the wire.\n%d nets, %d captures.",
                "on the move.\n%d APs in the ledger.",
                "PATROL: %d steps, %d captures.",
                "roaming ch%d.\n%d nets on the case.",
                "odometer %.1f km.\n%d captures this session."
            };
            pick = esp_random() % 6;
            switch (pick) {
            case 0: snprintf(buf, len, T[0], km, nets); break;
            case 1: snprintf(buf, len, T[1], nets, loot); break;
            case 2: snprintf(buf, len, T[2], nets); break;
            case 3: snprintf(buf, len, T[3], steps, loot); break;
            case 4: snprintf(buf, len, T[4], ch, nets); break;
            case 5: snprintf(buf, len, T[5], km, loot); break;
            }
            break;
        }
        case HuntBehavior::SPRINT: {
            static const char* const T[] = {
                "SPRINT. fast hops.\n%d nets logged.",
                "speed profile.\n%d APs, deauth disabled.",
                "fast hop.\n%d nets, discovery first.",
                "channels blur.\n%d nets found.",
                "odometer %.1f km.\nshort dwell, wide beat.",
                "scanning fast.\n%d nets, depth traded."
            };
            pick = esp_random() % 6;
            switch (pick) {
            case 0: snprintf(buf, len, T[0], nets); break;
            case 1: snprintf(buf, len, T[1], nets); break;
            case 2: snprintf(buf, len, T[2], nets); break;
            case 3: snprintf(buf, len, T[3], nets); break;
            case 4: snprintf(buf, len, T[4], km); break;
            case 5: snprintf(buf, len, T[5], nets); break;
            }
            break;
        }
        case HuntBehavior::LURK: {
            static const char* const T[] = {
                "LURK: ch%d locked.\none suspect gets the chair.",
                "ch%d held.\n%d clients may bring EAPOL.",
                "focused on ch%d.\none lead, full shift.",
                "ch%d locked.\nhigh-value lead under watch.",
                "LURK on ch%d.\nsnout near the glass.",
                "ch%d under watch.\n%d local APs."
            };
            pick = esp_random() % 6;
            switch (pick) {
            case 0: snprintf(buf, len, T[0], ch); break;
            case 1: snprintf(buf, len, T[1], ch, chClients); break;
            case 2: snprintf(buf, len, T[2], ch); break;
            case 3: snprintf(buf, len, T[3], ch); break;
            case 4: snprintf(buf, len, T[4], ch); break;
            case 5: snprintf(buf, len, T[5], ch, chNets); break;
            }
            break;
        }
        }
        break;
    }
    case 3: { // DROUGHT (loot==0, nets>3)
        static const char* const T[] = {
            "zero captures.\n%d nets questioned.",
            "%d APs. no capture.\nlead stays cold.",
            "scanning %d nets.\nnone signed EAPOL.",
            "zero captures.\n%d nets keep counsel.",
            "%d targets. zero captures.\ncase stays open.",
            "%d nets. empty vault.\nradio keeps listening.",
            "ch%d. %d APs. zero captures.\nno ghosts added."
        };
        pick = esp_random() % 7;
        switch (pick) {
        case 0: snprintf(buf, len, T[0], nets); break;
        case 1: snprintf(buf, len, T[1], nets); break;
        case 2: snprintf(buf, len, T[2], nets); break;
        case 3: snprintf(buf, len, T[3], nets); break;
        case 4: snprintf(buf, len, T[4], nets); break;
        case 5: snprintf(buf, len, T[5], nets); break;
        case 6: snprintf(buf, len, T[6], ch, nets); break;
        }
        break;
    }
    case 4: { // STREAK (loot>0)
        static const char* const T[] = {
            "%d captures and counting.\nvault fed.",
            "%d catches.\nreceiver found teeth.",
            "%d captures this run.\ncase has weight.",
            "session haul: %d.\nPSRAM wants a larger desk.",
            "%d in custody.\nkeep the chain clean.",
            "%d catches.\nradio earns overtime.",
            "%d PMKID, %d HS.\nledger separates facts."
        };
        pick = esp_random() % 7;
        switch (pick) {
        case 0: snprintf(buf, len, T[0], loot); break;
        case 1: snprintf(buf, len, T[1], loot); break;
        case 2: snprintf(buf, len, T[2], loot); break;
        case 3: snprintf(buf, len, T[3], loot); break;
        case 4: snprintf(buf, len, T[4], loot); break;
        case 5: snprintf(buf, len, T[5], loot); break;
        case 6: snprintf(buf, len, T[6], pmkids, hs); break;
        }
        break;
    }
    case 5: { // DISTANCE
        static const char* const T[] = {
            "odometer %.1f km.\nradio kept pace.",
            "%d steps.\nPATROL paid in soles.",
            "%.1f km lifetime.\n%d nets this session.",
            "%d steps in.\ncase got wider.",
            "odometer %.1f km.\nfeet object, radio continues.",
            "%.1f km lifetime.\n%d captures this session."
        };
        pick = esp_random() % 6;
        switch (pick) {
        case 0: snprintf(buf, len, T[0], km); break;
        case 1: snprintf(buf, len, T[1], steps); break;
        case 2: snprintf(buf, len, T[2], km, nets); break;
        case 3: snprintf(buf, len, T[3], steps); break;
        case 4: snprintf(buf, len, T[4], km); break;
        case 5: snprintf(buf, len, T[5], km, loot); break;
        }
        break;
    }
    case 6: { // ATTACK
        static const char* const T[] = {
            "%d attack actions.\nTX kept receipts.",
            "%d attack actions.\nconsequences stay real.",
            "attack counter: %d.\nmode names stay separate.",
            "attack count: %d.\nno victory assumed.",
            "%d TX actions.\nresult not inferred.",
            "%d attack actions.\ntransmit is not a verdict.",
            "%d attacks, %d captures.\nseparate ledgers."
        };
        pick = esp_random() % 7;
        switch (pick) {
        case 0: snprintf(buf, len, T[0], deauths); break;
        case 1: snprintf(buf, len, T[1], deauths); break;
        case 2: snprintf(buf, len, T[2], deauths); break;
        case 3: snprintf(buf, len, T[3], deauths); break;
        case 4: snprintf(buf, len, T[4], deauths); break;
        case 5: snprintf(buf, len, T[5], deauths); break;
        case 6: snprintf(buf, len, T[6], deauths, loot); break;
        }
        break;
    }
    case 7: { // DRY_WIT
        static const char* const T[] = {
            "%d APs.\nall with counsel.",
            "ch%d. %d nets.\ncity keeps talking.",
            "%d signals.\nzero sworn statements.",
            "monitoring %d nets.\nno motives invented.",
            "%d nets.\nnot one owes a confession.",
            "ch%d mood: static with paperwork.",
            "%d clients.\nall leaving fingerprints."
        };
        pick = esp_random() % 7;
        switch (pick) {
        case 0: snprintf(buf, len, T[0], nets); break;
        case 1: snprintf(buf, len, T[1], ch, nets); break;
        case 2: snprintf(buf, len, T[2], nets); break;
        case 3: snprintf(buf, len, T[3], nets); break;
        case 4: snprintf(buf, len, T[4], nets); break;
        case 5: snprintf(buf, len, T[5], ch); break;
        case 6: snprintf(buf, len, T[6], clients); break;
        }
        break;
    }
    }

    // ==[ CONTINUATION PHRASES ]==
    // category-specific second beats pair with any primary in the same category
    static const char* const F0[] = { // SESSION_STATS
        "session totals stay under oath.",
        "the numbers do not improvise.",
        "PSRAM keeps the rough ledger.",
        "captures, not wishful thinking.",
        "one counter per kind of trouble.",
        "the session clock keeps moving.",
        "facts survive the summary.",
        "the ledger has no mood."
    };
    static const char* const F1[] = { // CHANNEL
        "channel density is not guilt.",
        "crowded air. precise counts.",
        "beacons keep office hours.",
        "clients arrive without appointments.",
        "local counts, no invented ghosts.",
        "the dwell has a reason.",
        "one channel. many statements.",
        "D-UCB picked this room."
    };
    static const char* const F2[] = { // BEHAVIOR
        "motion changes dwell, not truth.",
        "the profile follows the feet.",
        "depth and reach settle accounts.",
        "CAMP listens. PATROL moves.",
        "SPRINT leaves deauth disabled.",
        "LURK spends time on one lead.",
        "behavior follows measured motion.",
        "the radio adapts without theater."
    };
    static const char* const F3[] = { // DROUGHT
        "zero stays zero. no ghosts.",
        "silence still made the report.",
        "empty vault, honest counter.",
        "the lead can stay cold.",
        "no capture is not no data.",
        "receiver remains on duty.",
        "the drought gets a timestamp.",
        "patience has measurable cost."
    };
    static const char* const F4[] = { // STREAK
        "custody grows. counts stay split.",
        "one capture at a time.",
        "the vault has receipts.",
        "success does not edit the mask.",
        "momentum finally brought evidence.",
        "PSRAM now charges extra rent.",
        "captures earned the optimism.",
        "the pig files every win."
    };
    static const char* const F5[] = { // DISTANCE
        "distance changes the radio view.",
        "each step moves the noise floor.",
        "the beat widened underfoot.",
        "legs object. counter overrules.",
        "PATROL makes distance useful.",
        "motion keeps the case fresh.",
        "kilometers have timestamps.",
        "the city charges by the step."
    };
    static const char* const F6[] = { // ATTACK
        "TX is action, not decoration.",
        "sent does not mean effective.",
        "the counter claims no outcome.",
        "management frames keep receipts.",
        "transmit deserves a sober ledger.",
        "scope cannot fit in a joke.",
        "results belong to capture count.",
        "radio power has consequences."
    };
    static const char* const F7[] = { // DRY_WIT
        "dry wit, wet pavement.",
        "the timestamp lacked sympathy.",
        "radio heard worse excuses.",
        "the city keeps bad hours.",
        "pixels make a narrow courtroom.",
        "noise floor declined comment.",
        "antenna stays professionally nosy.",
        "the case outlives the punchline."
    };

    static const char* const* const FOLLOW[] = { F0, F1, F2, F3, F4, F5, F6, F7 };

    // 50% chance: no continuation — variety matters
    if ((esp_random() % 2) == 0) {
        buf2[0] = '\0';
    } else {
        strncpy(buf2, FOLLOW[cat][esp_random() % 8], len2 - 1);
        buf2[len2 - 1] = '\0';
    }

    return true;
}

void Mood::onIdle(bool isHunting) {
    // check phrase timeout (wraparound-safe)
    if (hasPhrase() && (int32_t)(millis() - lastPhraseTime) >= (int32_t)phraseTimeout) {
        nextFromQueue();
    }
    
    // idle chatter sometimes
    // hunt: 1 in 200 (~30s avg, keeps pig alive between events)
    // chill: 1 in 100, nothing else happening
    if (!hasPhrase()) {
        uint32_t chance = isHunting ? 200 : 100;
        if ((esp_random() % chance) == 0) {
            // ==[ ACHIEVEMENT PROGRESS HINTS ]== 10% chance near-complete hints
            bool hintShown = false;
            if (!isHunting && (esp_random() % 10) == 0) {
                char hintBuf[80];
                if (buildAchievementHint(hintBuf, sizeof(hintBuf))) {
                    setPhrase(hintBuf, AvatarState::SLEEPY);
                    hintShown = true;
                }
            }
            if (!hintShown) {
            if (isHunting) {
                // 60% context-aware, 40% personality
                if ((esp_random() % 5) < 3) {
                    char buf[96];
                    char buf2[64];
                    if (buildContextHuntPhrase(buf, sizeof(buf), buf2, sizeof(buf2))) {
                        setPhrase(buf, AvatarState::HUNTING);
                        if (buf2[0]) queuePhrase(buf2, AvatarState::HUNTING);
                    } else {
                        setPhrase(selectPhrase(IDLE_PHRASES, IDLE_COUNT), AvatarState::HUNTING);
                        if ((esp_random() % 2) == 0) {
                            queuePhrase(selectPhrase(IDLE_F, IDLE_F_COUNT), AvatarState::HUNTING);
                        }
                    }
                } else {
                    setPhrase(selectPhrase(IDLE_PHRASES, IDLE_COUNT), AvatarState::HUNTING);
                    if ((esp_random() % 2) == 0) {
                        queuePhrase(selectPhrase(IDLE_F, IDLE_F_COUNT), AvatarState::HUNTING);
                    }
                }
            } else {
                setPhrase(selectPhrase(IDLE_PHRASES, IDLE_COUNT), AvatarState::SLEEPY);
                if ((esp_random() % 2) == 0) {
                    queuePhrase(selectPhrase(IDLE_F, IDLE_F_COUNT), AvatarState::SLEEPY);
                }
            }
            } // end !hintShown
            phraseTimeout = 3000;
        }
    }
    
    // random cute jump when mood > 20 (0.5%)
    if (!isHunting && momentum > 20 && (esp_random() % 200) == 0) {
        Avatar::cuteJump();
    }
    
    // momentum decay handled in main loop (Mood::decayMomentum())
}

void Mood::onMilestone(int networks, int captures) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%d NETS\n%d CAPTURES", networks, captures);
    setPhrase(buf, AvatarState::HAPPY);
    queuePhrase(selectPhrase(MILESTONE_PHRASES, MILESTONE_COUNT), AvatarState::EXCITED);
    phraseTimeout = 3000;
}

void Mood::onStepMilestone(int steps) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%d STEPS LOGGED", steps);
    setPhrase(buf, AvatarState::HAPPY);
    queuePhrase(selectPhrase(STEP_MILESTONE_PHRASES, STEP_MILESTONE_COUNT), AvatarState::EXCITED);
    phraseTimeout = 3000;
}

// walk milestone (phase 3 - graduated)
// fires at 5000/10000/20000/30000 step intervals

void Mood::onWalkMilestone(uint32_t steps) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%lu STEPS\nMILESTONE VERIFIED", steps);
    setPhrase(buf, AvatarState::EXCITED);
    queuePhrase(selectPhrase(WALK_MILESTONE_PHRASES, WALK_MILESTONE_COUNT), AvatarState::EXCITED);
    SFX::play(SFX::WALK_MILESTONE);
    phraseTimeout = 4000;

    // ==[ WALK ACHIEVEMENTS ]==
    if (steps >= 1000)  Achievements::tryUnlock(Achievement::TOUCH_GRASS);
    if (steps >= 10000) Achievements::tryUnlock(Achievement::MARATHON);
    if (steps >= 30000) Achievements::tryUnlock(Achievement::ULTRA);

    // DEFHOG4 walk status push
    if (DefhogTerminal::isVisible()) {
        float km = steps * 0.6f / 1000.0f;
        DefhogTerminal::pushLine("%lu steps. %.1fkm.", steps, km);
    }
}

// daily goal events (phase 2)

void Mood::onGoalClose(uint32_t remaining) {
    const char* phrase = selectPhrase(GOAL_CLOSE_PHRASES, GOAL_CLOSE_COUNT);
    if (strstr(phrase, "%lu")) {
        char buf[64];
        snprintf(buf, sizeof(buf), phrase, (unsigned long)remaining);
        setPhrase(buf, AvatarState::HAPPY);
    } else {
        setPhrase(phrase, AvatarState::HAPPY);
    }
    phraseTimeout = 3000;
}

void Mood::onGoalComplete() {
    setPhrase(selectPhrase(GOAL_COMPLETE_PHRASES, GOAL_COMPLETE_COUNT), AvatarState::EXCITED);
    phraseTimeout = 4000;
    addMomentum(MOMENTUM_GOAL_COMPLETE);

    // celebrate with shake + warm chime
    Avatar::setAttackShake(true, true);
    SFX::play(SFX::GOAL_COMPLETE);
    Haptic::buzz();  // sustained goal buzz

    // DEFHOG4 goal push
    if (DefhogTerminal::isVisible()) {
        DefhogTerminal::pushLineHype("GOAL CLOSED. PAYOUT NEXT BOOT.");
    }
}

void Mood::onLevelUp(uint8_t newLevel) {
    setPhrase(selectPhrase(LEVEL_UP_PHRASES, LEVEL_UP_COUNT), AvatarState::EXCITED);
    if ((esp_random() % 2) == 0) {
        queuePhrase(selectPhrase(LEVEL_UP_F, LEVEL_UP_F_COUNT), AvatarState::EXCITED);
    }
    phraseTimeout = 4000;
    addMomentum(MOMENTUM_LEVEL_UP);
    Avatar::spin();
    Avatar::triggerSparkles(6);
    SFX::play(SFX::LEVEL_UP);
    Haptic::pulse();  // strong level-up rumble

    // ==[ RANK ACHIEVEMENTS ]== level milestones
    if (newLevel >= 7)  Achievements::tryUnlock(Achievement::RANK_SHOAT);
    if (newLevel >= 14) Achievements::tryUnlock(Achievement::RANK_BOAR);
    if (newLevel >= 21) Achievements::tryUnlock(Achievement::RANK_TUSKER);
    if (newLevel >= 28) Achievements::tryUnlock(Achievement::RANK_WARTHOG);
    if (newLevel >= 35) Achievements::tryUnlock(Achievement::RANK_RAZORBACK);
    if (newLevel >= 42) Achievements::tryUnlock(Achievement::RANK_ELDER);

    bool rankChanged = newLevel == 7 || newLevel == 14 || newLevel == 21 ||
                       newLevel == 28 || newLevel == 35 || newLevel == 42;
    // DEFHOG4 tells level truth; only milestone levels claim a new rank.
    if (DefhogTerminal::isVisible()) {
        if (rankChanged) {
            DefhogTerminal::pushLineHype("RANK UP: L%d %s", newLevel,
                                         Config::getRankName(newLevel));
        } else {
            DefhogTerminal::pushLineHype("LEVEL UP: L%d", newLevel);
        }
    }
}

// new event triggers

void Mood::onDeauth(const char* ssid) {
    char context[12];
    if (MoodContext::copyDisplay(ssid, context, sizeof(context), 10)) {
        char buf[64];
        snprintf(buf, sizeof(buf), selectPhrase(DEAUTH_PHRASES, DEAUTH_COUNT), context);
        setPhrase(buf, AvatarState::EXCITED);
    } else {
        setPhrase("attack cycle started.", AvatarState::EXCITED);
    }
    phraseTimeout = 2000;
    addMomentum(MOMENTUM_DEAUTH);
    Challenges::onDeauth();
}

void Mood::onSAEReject() {
    // SAE downgrade: spoofed reject sent to force WPA2 fallback
    // rare + tactical = higher reward than generic deauth
    setPhrase(selectPhrase(SAE_REJECT_PHRASES, SAE_REJECT_COUNT), AvatarState::HUNTING);
    phraseTimeout = 2500;
    addMomentum(MOMENTUM_SAE_REJECT);
}

void Mood::onCatching(const char* ssid) {
    char context[12];
    if (MoodContext::copyDisplay(ssid, context, sizeof(context), 10)) {
        char buf[64];
        snprintf(buf, sizeof(buf), selectPhrase(CATCHING_PHRASES, CATCHING_COUNT), context);
        setPhrase(buf, AvatarState::HUNTING);
    } else {
        setPhrase("EAPOL watch open.", AvatarState::HUNTING);
    }
    phraseTimeout = 3000;
}

void Mood::onClientSpotted() {
    // click sound for every new client (subtle feedback)
    SFX::play(SFX::CLIENT_NEW);
    
    // 1 in 3 chance to avoid spam
    if ((esp_random() % 3) == 0) {
        setPhrase(selectPhrase(CLIENT_PHRASES, CLIENT_COUNT), AvatarState::HUNTING);
        phraseTimeout = 1500;
        Avatar::sniff();  // Sniff when detecting new client
    }
}

void Mood::onModeChange(uint8_t behavior) {
    // 0=CAMP, 1=PATROL, 2=SPRINT, 3=LURK
    bool wantFollow = ((esp_random() % 2) == 0);
    switch (behavior) {
        case 0:
            setPhrase(selectPhrase(CAMP_MODE_PHRASES, CAMP_MODE_COUNT), AvatarState::HUNTING);
            if (wantFollow) queuePhrase(selectPhrase(CAMP_MODE_F, CAMP_MODE_F_COUNT), AvatarState::HUNTING);
            SFX::play(SFX::HUNT_CAMP);
            break;
        case 2:
            setPhrase(selectPhrase(SPRINT_MODE_PHRASES, SPRINT_MODE_COUNT), AvatarState::HUNTING);
            if (wantFollow) queuePhrase(selectPhrase(SPRINT_MODE_F, SPRINT_MODE_F_COUNT), AvatarState::HUNTING);
            SFX::play(SFX::HUNT_SPRINT);
            break;
        case 3:
            setPhrase(selectPhrase(LURK_MODE_PHRASES, LURK_MODE_COUNT), AvatarState::HUNTING);
            if (wantFollow) queuePhrase(selectPhrase(LURK_MODE_F, LURK_MODE_F_COUNT), AvatarState::HUNTING);
            SFX::play(SFX::HUNT_LURK);
            break;
        default:
            setPhrase(selectPhrase(PATROL_MODE_PHRASES, PATROL_MODE_COUNT), AvatarState::HUNTING);
            if (wantFollow) queuePhrase(selectPhrase(PATROL_MODE_F, PATROL_MODE_F_COUNT), AvatarState::HUNTING);
            SFX::play(SFX::HUNT_PATROL);
            break;
    }
    phraseTimeout = 2000;
}

void Mood::onProbeSuccess() {
    Config::addXP(3);  // micro-reward drip — XP bar advances without capture (sim: #7 sensitivity)
    setPhrase(selectPhrase(PROBE_SUCCESS_PHRASES, PROBE_SUCCESS_COUNT), AvatarState::HAPPY);
    phraseTimeout = 1500;
    addMomentum(MOMENTUM_PROBE_SUCCESS);
}

void Mood::onProbeFail() {
    // 1 in 4 chance to avoid spam
    if ((esp_random() % 4) == 0) {
        setPhrase(selectPhrase(PROBE_FAIL_PHRASES, PROBE_FAIL_COUNT), AvatarState::NEUTRAL);
        phraseTimeout = 1500;
        Avatar::flinch();   // duck — probe rejected
    }
}

void Mood::onBackOnline() {
    setPhrase(selectPhrase(BACK_ONLINE_PHRASES, BACK_ONLINE_COUNT), AvatarState::HAPPY);
    if ((esp_random() % 2) == 0) {
        queuePhrase(selectPhrase(BACK_ONLINE_F, BACK_ONLINE_F_COUNT), AvatarState::HAPPY);
    }
    phraseTimeout = 2500;
    addMomentum(MOMENTUM_BACK_ONLINE);
}

void Mood::onDeadAir() {
    setPhrase(selectPhrase(DEAD_AIR_PHRASES, DEAD_AIR_COUNT), AvatarState::SAD);
    if ((esp_random() % 2) == 0) {
        queuePhrase(selectPhrase(DEAD_AIR_F, DEAD_AIR_F_COUNT), AvatarState::SAD);
    }
    phraseTimeout = 3000;
    Avatar::pawScratch();   // bored scratch — dead channel
}

void Mood::onChannelExploit(uint8_t channel) {
    // 1 in 5 chance when landing on productive channel
    if ((esp_random() % 5) == 0) {
        char buf[64];
        snprintf(buf, sizeof(buf), "ch%d paid before.\nmath circles back.", channel);
        setPhrase(buf, AvatarState::HAPPY);
        phraseTimeout = 2500;
        addMomentum(MOMENTUM_CHANNEL_EXPLOIT);
    }
}

void Mood::onChannelExplore() {
    // 1 in 8 chance during exploration
    if ((esp_random() % 8) == 0) {
        setPhrase(selectPhrase(DUCB_EXPLORE_PHRASES, DUCB_EXPLORE_COUNT), AvatarState::HUNTING);
        phraseTimeout = 2000;
    }
}

void Mood::onChannelLearned(uint8_t channel) {
    // always show - first reward counts
    char buf[64];
    snprintf(buf, sizeof(buf), "ch%d %s", channel, selectPhrase(DUCB_LEARNED_PHRASES, DUCB_LEARNED_COUNT));
    setPhrase(buf, AvatarState::HAPPY);
    phraseTimeout = 2500;
    addMomentum(MOMENTUM_CHANNEL_LEARNED);
}

void Mood::onPluggedIn() {
    // usb-c inserted - external power notification
    setPhrase(selectPhrase(PLUG_IN_PHRASES, PLUG_IN_COUNT), AvatarState::EXCITED);
    if ((esp_random() % 2) == 0) {
        queuePhrase(selectPhrase(PLUG_IN_F, PLUG_IN_F_COUNT), AvatarState::EXCITED);
    }
    SFX::play(SFX::PLUG_IN);
    phraseTimeout = 3000;
    addMomentum(MOMENTUM_PLUGGED_IN);
}

void Mood::onHandshakeProgress(uint8_t mask, const char* ssid) {
    // show M1234 pattern as frames get captured
    // mask: bit0=M1, bit1=M2, bit2=M3, bit3=M4
    char context[14];
    char pattern[32];
    bool hasContext = MoodContext::copyDisplay(ssid, context, sizeof(context), 12);
    if (hasContext) {
        snprintf(pattern, sizeof(pattern), "%s M%c%c%c%c", context,
                 (mask & 0x01) ? '1' : '-',
                 (mask & 0x02) ? '2' : '-',
                 (mask & 0x04) ? '3' : '-',
                 (mask & 0x08) ? '4' : '-');
    } else {
        snprintf(pattern, sizeof(pattern), "LOOT M%c%c%c%c",
                 (mask & 0x01) ? '1' : '-',
                 (mask & 0x02) ? '2' : '-',
                 (mask & 0x04) ? '3' : '-',
                 (mask & 0x08) ? '4' : '-');
    }
    
    setPhrase(pattern, AvatarState::HUNTING);
    phraseTimeout = 2000;
    Avatar::sniff();
}

void Mood::onFourwayVictory() {
    // crackable EAPOL pair captured - immediate partial reward (rest on storage)
    setPhrase(selectPhrase(FOURWAY_VICTORY_PHRASES, FOURWAY_VICTORY_COUNT), AvatarState::EXCITED);
    phraseTimeout = 2000;
    addMomentum(MOMENTUM_FOURWAY_VICTORY);
}

void Mood::onBirdKill() {
    // wave zapped a bird - small celebration
    static const char* const BIRD_KILL_PHRASES[] = {
        "WINGED BOGEY DOWN", "FEATHERS IN THE LEDGER", "SKY CONTACT CLEARED",
        "BIRD HIT CONFIRMED", "ONE LESS RADAR ECHO"
    };
    setPhrase(selectPhrase(BIRD_KILL_PHRASES, 5), AvatarState::EXCITED);
    phraseTimeout = 2000;
    addMomentum(MOMENTUM_BIRD_KILL);
}

void Mood::onShipKill() {
    static const char* const SHIP_KILL_PHRASES[] = {
        "BOGEY DOWN.\nDAMN CLEAN IMPACT.",
        "SHUTTLE BROKE ORBIT.",
        "SHIP KILL CONFIRMED.",
        "SKY CASE CLOSED.",
        "HULL DOWN.\nWEATHER GETS UGLY.",
        "THAT SHIP PICKED\nTHE WRONG PIG."
    };
    setPhrase(selectPhrase(SHIP_KILL_PHRASES, 6), AvatarState::EXCITED);
    phraseTimeout = 4000;
    setMomentum(MOMENTUM_SHIP_KILL);
    Config::addXP(200);  // ship kill = rare peak event
}

void Mood::onDebrisRain() {
    static const char* const DEBRIS_RAIN_PHRASES[] = {
        "DEBRIS FIELD ACTIVE",
        "HULL FRAGMENTS INBOUND",
        "SKY FULL OF RECEIPTS",
        "IMPACT WEATHER CONTINUES",
        "FALLING METAL.\nKEEP LOW.",
        "THE SHIP KEEPS ARRIVING"
    };
    setPhrase(selectPhrase(DEBRIS_RAIN_PHRASES, 6), AvatarState::EXCITED);
    phraseTimeout = 3000;
}

// ==[ PET INTERACTION ]== diminishing returns pig pats

void Mood::onPetted(uint8_t petCount) {
    static const char* const PET_PHRASES[] = {
        "pat logged.\nsnout accepts.",
        "snout contact confirmed.",
        "pat received.\nmood ticks upward.",
        "touch event.\npig checks the hand.",
        "ears remain evidence.",
        "that is a pig, not a button.",
        "input accepted.\nstack remains intact.",
        "snout boop entered the ledger.",
        "contact accepted.\nno incident filed.",
        "ticklish. classified."
    };
    static const int PET_COUNT = 10;

    if (petCount <= 1) {
        setPhrase(selectPhrase(PET_PHRASES, PET_COUNT), AvatarState::HAPPY);
        addMomentum(MOMENTUM_PET_FIRST);
        phraseTimeout = 3000;
    } else if (petCount <= 10) {
        setPhrase(selectPhrase(PET_PHRASES, PET_COUNT), AvatarState::NEUTRAL);
        addMomentum(MOMENTUM_PET_REPEAT);
        phraseTimeout = 2000;
    } else if (petCount <= 20) {
        static const char* const SPAM_PHRASES[] = {
            "pat counter says enough.",
            "snout input rate-limited.",
            "touch queue saturated.\npause.",
            "diminishing returns confirmed."
        };
        setPhrase(selectPhrase(SPAM_PHRASES, 4), AvatarState::NEUTRAL);
        addMomentum(MOMENTUM_PET_SPAM);
        phraseTimeout = 2000;
    }
    // 20+ silently ignored (hamlet.cpp blocks the call)
}

// ==[ TELEPORT EVENTS ]==

void Mood::onTeleportArrival() {
    static const char* const TERMINATOR_ARRIVAL_PHRASES[] = {
        "arrival complete.\nstatic still smoking.",
        "teleport closed.\npig remains intact.",
        "coordinates accepted.\nreality objects.",
        "particle custody restored.",
        "case resumed.\ndistance cheated.",
        "pig reconstructed.\nchecksum has questions.",
    };
    setPhrase(selectPhrase(TERMINATOR_ARRIVAL_PHRASES, 6), AvatarState::HUNTING);
    phraseTimeout = 4000;
    addMomentum(MOMENTUM_TELEPORT_ARRIVAL);
}

// ==[ IPP DEFENSE EVENTS ]==

void Mood::onTrackerDetected(uint8_t threatType, int8_t rssi, const char* detail) {
    static const char* const TRACKER_PHRASES[] = {
        "new BLE device.\nradar opens a file.",
        "BLE signal.\nlogging RSSI.",
        "BLE beacon noted.\nidentity unproven.",
        "new RF contact.\nclock starts.",
    };
    char context[20];
    if (MoodContext::copyDisplay(detail, context, sizeof(context), 18)) {
        const char* type = Recon::threatTypeLabel(
            static_cast<Recon::ThreatType>(threatType));
        setPhraseFormatted(AvatarState::HUNTING, "%s: %s\n%d dBm // %s",
                           type, context, (int)rssi,
                           Recon::proximityLabel(rssi));
    } else {
        setPhrase(selectPhrase(TRACKER_PHRASES, 4), AvatarState::HUNTING);
    }
    phraseTimeout = 6500;  // survives the 4s global alert toast
    addMomentum(MOMENTUM_TRACKER_DETECTED);
    Avatar::sniff();
}

void Mood::onTrackerFollowing(const char* detail) {
    static const char* const FOLLOW_PHRASES[] = {
        "TRACKER PERSISTENCE",
        "SAME TAG.\nSTILL PRESENT.",
        "BLE TAIL CONFIRMED",
        "PERSISTENT RF SHADOW",
        "REPEAT SIGHTING",
        "TAG REMAINS IN RANGE",
        "SHADOW HAS NOT\nDROPPED",
        "FOLLOWING PATTERN",
    };
    char context[24];
    if (MoodContext::copyDisplay(detail, context, sizeof(context), 22)) {
        setPhraseFormatted(AvatarState::ANGRY, "TAIL LOCK:\n%s", context);
    } else {
        setPhrase(selectPhrase(FOLLOW_PHRASES, 8), AvatarState::ANGRY);
    }
    phraseTimeout = 7500;  // survives the 5s global alert toast
    addMomentum(MOMENTUM_TRACKER_FOLLOWING);
    Avatar::flinch();
}

void Mood::onBleSpamDetected() {
    static const char* const SPAM_PHRASES[] = {
        "BLE SPAM DETECTED",
        "KNOWN SPAM PATTERN",
        "MAC FLOOD",
        "SYNTHETIC BLE STORM",
    };
    setPhrase(selectPhrase(SPAM_PHRASES, 4), AvatarState::EXCITED);
    phraseTimeout = 3000;
    addMomentum(MOMENTUM_BLE_SPAM);
    Avatar::perkUp();
}

void Mood::onEvilTwin(const char* ssid) {
    static const char* const TWIN_PHRASES[] = {
        "EVIL TWIN SPOTTED",
        "SSID CLONE DETECTED",
        "DUPLICATE AP IDENTITY",
        "ROGUE TWIN ON AIR",
    };
    char context[20];
    if (MoodContext::copyDisplay(ssid, context, sizeof(context), 17)) {
        setPhraseFormatted(AvatarState::HUNTING, "TWIN: %s\nSSID CLONE", context);
    } else {
        setPhrase(selectPhrase(TWIN_PHRASES, 4), AvatarState::HUNTING);
    }
    phraseTimeout = 7000;  // survives the 4.5s combined-threat alert
    addMomentum(MOMENTUM_EVIL_TWIN);
    Avatar::flinch();
}

void Mood::onKarmaDetected(const char* ssid) {
    static const char* const KARMA_PHRASES[] = {
        "KARMA PATTERN",
        "ROGUE AP.\nMULTIPLE SSIDS.",
        "MULTI-SSID BEHAVIOR",
    };
    char context[20];
    if (MoodContext::copyDisplay(ssid, context, sizeof(context), 16)) {
        setPhraseFormatted(AvatarState::HUNTING, "KARMA: %s\nMULTIPLE SSIDS", context);
    } else {
        setPhrase(selectPhrase(KARMA_PHRASES, 3), AvatarState::HUNTING);
    }
    phraseTimeout = 7000;  // survives the 4.5s global alert toast
    addMomentum(MOMENTUM_KARMA_DETECTED);
    Avatar::flinch();
}

void Mood::onKnownAPFound(const char* ssid) {
    char context[22];
    if (MoodContext::copyDisplay(ssid, context, sizeof(context), 20)) {
        setPhraseFormatted(AvatarState::HAPPY, "known AP:\n%s", context);
    } else {
        setPhrase("known AP nearby.", AvatarState::HAPPY);
    }
    phraseTimeout = 3000;
    addMomentum(MOMENTUM_KNOWN_AP);
}

void Mood::onAttackerIdentified(const char* detail) {
    static const char* const ATK_PHRASES[] = {
        "HARDWARE CORRELATION",
        "RF PROFILE\nMATCHED",
        "XBAND HIT.\nPROFILE MATCHED.",
        "SOURCE CLASS MATCH",
    };
    char context[24];
    if (MoodContext::copyDisplay(detail, context, sizeof(context), 22)) {
        setPhraseFormatted(AvatarState::ANGRY, "XBAND PROFILE:\n%s", context);
    } else {
        setPhrase(selectPhrase(ATK_PHRASES, 4), AvatarState::ANGRY);
    }
    phraseTimeout = 7000;  // leave a read window after the global toast
    addMomentum(MOMENTUM_ATTACKER_IDENTIFIED);
    Avatar::flinch();
}

void Mood::onDualBandStalk(const char* detail) {
    static const char* const STALK_PHRASES[] = {
        "BOTH BANDS.\nPERSISTENCE MATCH.",
        "WIFI + BLE.\nPATTERNS CORRELATE.",
        "DUAL-BAND CORRELATION",
        "PERSISTENT ON\nBOTH BANDS",
    };
    char context[22];
    if (MoodContext::copyDisplay(detail, context, sizeof(context), 20)) {
        setPhraseFormatted(AvatarState::ANGRY, "DUAL-BAND CORR:\n%s", context);
    } else {
        setPhrase(selectPhrase(STALK_PHRASES, 4), AvatarState::ANGRY);
    }
    phraseTimeout = 7500;  // survives the 5s global alert toast
    addMomentum(MOMENTUM_DUAL_BAND_STALK);
    Avatar::flinch();
}

void Mood::onCanaryTripped(const char* ssid) {
    static const char* const CANARY_PHRASES[] = {
        "CANARY TRIPPED.\nTARGETING INDICATOR.",
        "GHOST NET HIT.\nPROBE REPLAY.",
        "BAIT TAKEN.\nREPLAY OBSERVED.",
        "CANARY TRIPPED.\nPROBE REPLAY.",
    };
    char context[20];
    if (MoodContext::copyDisplay(ssid, context, sizeof(context), 15)) {
        setPhraseFormatted(AvatarState::ANGRY, "CANARY: %s\nPROBE REPLAY", context);
    } else {
        setPhrase(selectPhrase(CANARY_PHRASES, 4), AvatarState::ANGRY);
    }
    phraseTimeout = 8500;  // survives the 6s global alert toast
    addMomentum(MOMENTUM_CANARY_TRIPPED);
    Avatar::flinch();
}

void Mood::onKarmaConfirmed(const char* ssid) {
    static const char* const KARMA_CONF_PHRASES[] = {
        "KARMA CONFIRMED.\nPHANTOM PROBE HIT.",
        "BAIT ECHO.\nKARMA AP CONFIRMED.",
        "ROGUE AP CONFIRMED.\nKARMA ACTIVE.",
    };
    char context[18];
    if (MoodContext::copyDisplay(ssid, context, sizeof(context), 14)) {
        setPhraseFormatted(AvatarState::ANGRY, "BAIT ECHO: %s\nKARMA CONFIRMED", context);
    } else {
        setPhrase(selectPhrase(KARMA_CONF_PHRASES, 3), AvatarState::ANGRY);
    }
    phraseTimeout = 7500;  // survives the 5s global alert toast
    addMomentum(MOMENTUM_KARMA_CONFIRMED);
    Avatar::flinch();
}

void Mood::onToolIdentified(const char* toolName) {
    static const char* const TOOL_PHRASES[] = {
        "TOOL-LIKE SIGNATURE.",
        "BEHAVIOR PROFILE\nMATCHED.",
        "ATTACK PATTERN\nCLASSIFIED.",
        "BEHAVIORAL MATCH.",
    };
    char context[24];
    if (MoodContext::copyDisplay(toolName, context, sizeof(context), 22)) {
        setPhraseFormatted(AvatarState::HUNTING, "TOOL PROFILE:\n%s", context);
    } else {
        setPhrase(selectPhrase(TOOL_PHRASES, 4), AvatarState::HUNTING);
    }
    phraseTimeout = 7000;  // survives the 4.5s global alert toast
    addMomentum(MOMENTUM_TOOL_IDENTIFIED);
}

void Mood::onReconScan(uint8_t trackerCount) {
    // low probability ambient awareness — 1 in 6 chance
    if ((esp_random() % 6) != 0) return;

    if (trackerCount == 0) {
        static const char* const CLEAR_PHRASES[] = {
            "recon clear.",
            "BLE watch quiet.",
            "no tracker pattern.",
        };
        setPhrase(selectPhrase(CLEAR_PHRASES, 3), AvatarState::NEUTRAL);
    } else {
        setPhraseFormatted(AvatarState::HUNTING, "%d tracker tags\non the wire.", trackerCount);
    }
    phraseTimeout = 2500;
}

// ==[ SESSION DEBRIEF ]== inject hunt stats into DEFHOG4

bool Mood::hasDebrief() { return debrief.pending; }

void Mood::injectDebrief() {
    if (!debrief.pending) return;
    debrief.pending = false;

    uint8_t total = debrief.pmkids + debrief.handshakes;
    if (total == 0 && debrief.probes == 0) return;

    DefhogTerminal::pushLineAlert("--- HUNT DEBRIEF ---");
    DefhogTerminal::pushLine("pmkid:%d hs:%d probes:%d attacks:%d",
        debrief.pmkids, debrief.handshakes, debrief.probes, debrief.deauths);
    if (debrief.maxCombo >= 2)
        DefhogTerminal::pushLine("best chain: x%d", debrief.maxCombo);
    if (debrief.xpGained > 0)
        DefhogTerminal::pushLineHype("+%luXP this session", (unsigned long)debrief.xpGained);

    // debrief reviewed bonus: +15 XP if hunt had captures
    if (total > 0) {
        Config::addXP(15);
        DefhogTerminal::pushLineDim("+15 XP debrief review");
        addMomentum(MOMENTUM_DEBRIEF_BONUS);
    }
}

// bubble drawing

namespace {

static bool isBubbleBreak(char c) {
    return c == '\n' || c == '\r';
}

static bool isBubbleSpace(char c) {
    return c == ' ' || c == '\t';
}

static int clampInt(int value, int low, int high) {
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

static int snap4(int value) {
    return value & ~3;
}

static int consumeBubbleBreaks(const char* text, int len, int pos, int* extraBreaks = nullptr) {
    int breaks = 0;
    while (pos < len && isBubbleBreak(text[pos])) {
        if (text[pos] == '\r' && pos + 1 < len && text[pos + 1] == '\n') {
            pos += 2;
        } else {
            pos++;
        }
        breaks++;
    }
    if (extraBreaks) *extraBreaks = breaks;
    return pos;
}

static void drawPixelBubblePanel(M5Canvas& canvas, int x, int y, int w, int h, uint16_t color) {
    if (w <= 0 || h <= 0) return;
    if (w < 12 || h < 12) {
        canvas.fillRect(x, y, w, h, color);
        return;
    }
    // 4px-snapped rounded rectangle: inset corners by 4px
    canvas.fillRect(x + 4, y, w - 8, h, color);
    canvas.fillRect(x, y + 4, w, h - 8, color);
}

// Fixed stepped tail — always points down, no adaptive rotation.
// 4px-grid stepped pyramid: predictable shape, no deformation.
static void drawPixelBubbleTail(M5Canvas& canvas,
                                int rootX, int rootY,
                                uint16_t outline, uint16_t fill) {
    // Snap everything to 4px grid
    int rx = snap4(rootX);
    int ry = snap4(rootY);
    // Outline (bg): 3-row stepped pyramid, 1 cell wider each side
    //   row 0: 16px wide (4 cells)
    //   row 1: 8px wide  (2 cells)
    //   row 2: 4px wide  (1 cell) — tip
    canvas.fillRect(rx - 8,  ry,     16, 4, outline);
    canvas.fillRect(rx - 4,  ry + 4,  8, 4, outline);
    canvas.fillRect(rx - 2,  ry + 8,  4, 4, outline);
    // Fill (fg): inset by 1 cell on each side
    //   row 0: 8px wide  (2 cells)
    //   row 1: 4px wide  (1 cell)
    canvas.fillRect(rx - 4, ry,     8, 4, fill);
    canvas.fillRect(rx - 2, ry + 4, 4, 4, fill);
}

}  // namespace

int Mood::wrapText(const char* text, char lines[][28], int maxLines) {
    if (!text || maxLines <= 0) return 0;

    int lineCount = 0;
    const int maxCharsPerLine = 25;  // 180px max bubble / 6px per char - padding
    int len = strlen(text);
    int pos = 0;

    while (pos < len && lineCount < maxLines) {
        while (pos < len && isBubbleSpace(text[pos])) pos++;
        if (pos >= len) break;

        int hardEnd = pos;
        while (hardEnd < len && !isBubbleBreak(text[hardEnd])) hardEnd++;

        if (hardEnd == pos) {
            lines[lineCount][0] = '\0';
            lineCount++;
            pos = consumeBubbleBreaks(text, len, hardEnd);
            continue;
        }

        while (pos < hardEnd && lineCount < maxLines) {
            int lineEnd = pos + maxCharsPerLine;
            if (lineEnd >= hardEnd) {
                int remaining = hardEnd - pos;
                if (remaining > 27) remaining = 27;
                strncpy(lines[lineCount], text + pos, remaining);
                lines[lineCount][remaining] = '\0';
                lineCount++;
                pos = hardEnd;
                break;
            }

            int breakAt = lineEnd;
            while (breakAt > pos && !isBubbleSpace(text[breakAt])) breakAt--;
            if (breakAt <= pos) breakAt = lineEnd;

            int lineLen = breakAt - pos;
            while (lineLen > 0 && isBubbleSpace(text[pos + lineLen - 1])) lineLen--;
            if (lineLen > 27) lineLen = 27;
            strncpy(lines[lineCount], text + pos, lineLen);
            lines[lineCount][lineLen] = '\0';
            lineCount++;

            pos = breakAt;
            while (pos < hardEnd && isBubbleSpace(text[pos])) pos++;
            if (lineCount >= maxLines) break;
        }

        if (pos < len && isBubbleBreak(text[pos])) {
            int breakCount = 0;
            pos = consumeBubbleBreaks(text, len, pos, &breakCount);
            while (breakCount > 1 && lineCount < maxLines) {
                lines[lineCount][0] = '\0';
                lineCount++;
                breakCount--;
            }
        }
    }

    return lineCount;
}

void Mood::drawBubble(M5Canvas& canvas, const char* text) {
    if (!text || text[0] == '\0') return;

    int pX = Avatar::getCurrentX();
    int16_t nX, nY;
    Avatar::getNosePosition(nX, nY);
    int pigTop = nY - UIMeasurements::MenuPigLayout::kPigH / 2;
    drawBubbleAt(canvas, text, pX, pX + UIMeasurements::MenuPigLayout::kPigW, pigTop, nX, nY);
}

void Mood::drawBubbleAt(M5Canvas& canvas, const char* text,
                         int pigX, int pigBodyRight, int pigTop,
                         int noseX, int noseY, bool flipSide) {
    if (!text || text[0] == '\0') return;

    // word wrap
    char lines[5][28];
    int numLines = wrapText(text, lines, 5);
    if (numLines == 0) return;

    // bubble dimensions — compact to content, font-aware (CJK safe)
    canvas.setTextSize(1);  // must set before measuring — Avatar::draw() leaves textSize at PIG_PX(2)
    int lineHeight = canvas.fontHeight() + 1;

    // measure actual max line width via font metrics (works for CJK + ASCII)
    int maxPixW = 0;
    for (int i = 0; i < numLines; i++) {
        int pixW = canvas.textWidth(lines[i]);
        if (pixW > maxPixW) maxPixW = pixW;
    }
    int bubbleW = maxPixW + 10;  // 5px padding each side
    if (bubbleW < 32) bubbleW = 32;     // minimum readable width
    if (bubbleW > 180) bubbleW = 180;   // scaled max for 320px screen
    bubbleW = snap4(bubbleW);            // snap to 4px grid
    int bubbleH = 8 + (numLines * lineHeight);
    if (bubbleH > 76) bubbleH = 76;
    bubbleH = snap4(bubbleH);            // snap to 4px grid

    // ==[ COMIC BUBBLE POSITION ]== lower anchor, opposite side, tail to nose
    int bubbleY = snap4(pigTop - bubbleH);
    if (bubbleY < TOP_BAR_H) bubbleY = TOP_BAR_H;

    // bubble X: opposite side from pig, biased toward nose X
    int bubbleX;
    bool pigOnLeft = (pigX + UIMeasurements::MenuPigLayout::kPigW / 2 < SCREEN_WIDTH / 2);
    if (flipSide) pigOnLeft = !pigOnLeft;

    if (pigOnLeft) {
        bubbleX = pigBodyRight - 16;
        if (bubbleX + bubbleW > SCREEN_WIDTH - 4) bubbleX = SCREEN_WIDTH - 4 - bubbleW;
    } else {
        bubbleX = pigX - bubbleW + 16;
        if (bubbleX < 4) bubbleX = 4;
    }

    // final clamp + snap
    if (bubbleX < 4) bubbleX = 4;
    if (bubbleX + bubbleW > SCREEN_WIDTH - 4) bubbleX = SCREEN_WIDTH - 4 - bubbleW;
    if (bubbleY + bubbleH > pigTop) bubbleY = snap4(pigTop - bubbleH);
    bubbleX = snap4(bubbleX);

    // ==[ TAIL GEOMETRY ]== fixed stepped pyramid, always points down.
    // X tracks nose (clamped to bubble), shape never deforms.
    int tailRootX = snap4(clampInt(noseX, bubbleX + 12, bubbleX + bubbleW - 12));
    int tailRootY = bubbleY + bubbleH;

    // avoid overlap with toast rect (dynamic position)
    int16_t tx, ty, tw, th;
    if (Display::getActiveToastRect(tx, ty, tw, th)) {
        int bBottom = tailRootY + 12;
        int bLeft = bubbleX - 4;
        int bRight = bubbleX + bubbleW + 4;
        int bTop = bubbleY - 4;
        if (bBottom > ty && bTop < ty + th && bRight > tx && bLeft < tx + tw) {
            return;  // toast wins — suppress bubble
        }
    }

    // colors: theme bg fill + theme fg text, inverted themes flip
    bool flashing = Weather::isThunderFlashing();
    bool inverted = Display::isInvertedTheme() ^ flashing;  // flash toggles sense
    uint16_t fg = Display::getColorFG();
    uint16_t bg = Display::getColorBG();
    uint16_t fill = inverted ? bg : Display::lerpColor565(fg, 0xFFFF, 0.15f);  // bubble interior — non-inverted gets one step brighter
    uint16_t border = inverted ? fg : bg;   // silhouette + text (inverted)

    // 1. border silhouette — stepped panel + stepped tail
    drawPixelBubblePanel(canvas, bubbleX - 4, bubbleY - 4, bubbleW + 8, bubbleH + 8, border);
    drawPixelBubbleTail(canvas, tailRootX, tailRootY, border, fill);

    // 2. fill interior — inset panel fill
    drawPixelBubblePanel(canvas, bubbleX, bubbleY, bubbleW, bubbleH, fill);

    // ==[ TEXT ]== border color on fill (contrast guaranteed)
    canvas.setTextDatum(TL_DATUM);
    canvas.setTextColor(border);

    int textX = bubbleX + 5;
    int textY = bubbleY + 4;

    for (int i = 0; i < numLines; i++) {
        canvas.drawString(lines[i], textX, textY + i * lineHeight);
    }
}

static uint64_t streetCacheKey(const TimeOfDay::State& state) {
    uint64_t key = 1469598103934665603ull;
    auto mix = [&key](uint32_t value) {
        key = (key ^ value) * 1099511628211ull;
    };
    mix((uint32_t)state.phase);
    // Five-minute palette buckets preserve real day/night progression without
    // rebuilding the expensive skyline on every 60Hz frame.
    mix((uint32_t)(state.dayProgress * 288.0f));
    mix(Display::getColorFG());
    mix(Display::getColorBG());
    mix(Display::isInvertedTheme() ? 1u : 0u);
    return key;
}

static void drawStreetBackdropCached(M5Canvas& canvas,
                                     const TimeOfDay::State& state,
                                     uint32_t now) {
    const uint64_t key = streetCacheKey(state);
    const int pigX = Avatar::getCurrentX();
    bool restored =
        SceneCache::restore(canvas, SceneCache::Owner::STREET, key,
                            TOP_BAR_H, MAIN_H);

    if (!restored) {
        M5Canvas* base = SceneCache::rebuildTarget();
        if (!base) {
            PixelStreet::drawIdleBackdrop(canvas, state, now, pigX);
            return;
        }

        base->fillSprite(Display::getColorBG());
        PixelStreet::drawIdleBackdropBase(*base, state, pigX);
        SceneCache::commit(SceneCache::Owner::STREET, key);
        restored =
            SceneCache::restore(canvas, SceneCache::Owner::STREET, key,
                                TOP_BAR_H, MAIN_H);
        if (!restored)
            PixelStreet::drawIdleBackdropBase(canvas, state, pigX);
    }

    // The cache owns only stable scenery. Every clocked sky/furniture source
    // must repaint after restore so IDLE and HUNT never freeze on a cache hit.
    PixelStreet::drawIdleBackdropMotion(canvas, state, now, pigX);
}

void Mood::draw(M5Canvas& canvas, int steps, float distance) {
    int mood = getEffectiveMood();

    // weather from momentum (visual rain + thunder)
    Weather::setMoodLevel(mood);
    bool rainingNow = Weather::isRaining() || inCriticalScene;

    uint32_t now = millis();

    // === CRITICAL SYSTEM: only 4-min IDLE timeout trigger (mood-based removed) ===
    // trigger4MinCritical() is called from hamlet.cpp; no mood-based trigger here.
    if (false && !criticalStateActive) {  // disabled: mood-based trigger removed
        if (!criticalMonologueShown) {
            // FIRST TIME: full monologue, then toast after 36s + 60s timer
            criticalMonologueIndex = 0;
            lastCriticalPhraseTime = now;
            // atmospheric rain ticks: 180-450ms base interval
            rainTickInterval = random(180, 450);
            nextRainTickTime = now + rainTickInterval;
            setPhrase(RAIN_MONOLOGUE[0], AvatarState::SAD);
            phraseTimeout = 4000;
            criticalMonologueIndex = 1;
            criticalMonologueShown = true;
            inCriticalScene = true;  // lock scene
            Avatar::setGrassMoving(false, false);  // freeze grass
            { CinematicPose cp; cp.active = true; Avatar::setCinematicPose(cp); }
        } else {
            // SUBSEQUENT TIMES: "time to deauth" phrase, then toast + 60s timer
            setPhrase("rain ended.\ncase stays open.", AvatarState::SAD);
            phraseTimeout = 4000;
        }
        criticalStateActive = true;
        criticalMonologueComplete = false;
        criticalStartTime = now;
        lastTickSecond = -1;  // reset tick tracking
    }
    
    // Randomized atmospheric rain ticks during scene (2-5 ticks/sec)
    if (inCriticalScene && TimeMath::reached(now, nextRainTickTime)) {
        // low rumble clicks (100-300Hz range) with ±10Hz jitter
        uint16_t baseFreq = random(100, 300);
        int16_t jitter = (int16_t)((esp_random() % 21) - 10);  // ±10Hz
        uint16_t freq = constrain(baseFreq + jitter, 90, 310);
        // longer duration for low Hz (20-40ms rumble)
        uint8_t duration = random(20, 41);
        SFX::tone(freq, duration);
        // next interval with ±15% jitter
        rainTickInterval = random(180, 450);
        int16_t intervalJitter = (int16_t)(rainTickInterval * (random(-15, 16)) / 100);
        rainTickInterval = constrain(rainTickInterval + intervalJitter, 150, 500);
        nextRainTickTime = now + rainTickInterval;
    }
    
    // Continue critical monologue (first time only)
    if (criticalStateActive && !criticalMonologueComplete && criticalMonologueIndex > 0 && criticalMonologueIndex < RAIN_MONOLOGUE_COUNT) {
        if (now - lastCriticalPhraseTime >= 4000 && !hasPhrase()) {
            setPhrase(RAIN_MONOLOGUE[criticalMonologueIndex], AvatarState::SAD);
            phraseTimeout = 4000;
            lastCriticalPhraseTime = now;
            criticalMonologueIndex++;
        }
    }
    
    // Mark monologue complete after 36s, unlock scene
    if (criticalStateActive && !criticalMonologueComplete && (now - criticalStartTime >= MONOLOGUE_DURATION)) {
        criticalMonologueComplete = true;
        inCriticalScene = false;  // unlock scene - now recovery allowed
        Avatar::clearCinematic();
    }
    
    // Accelerating tick during countdown (after monologue complete)
    // Goal gradient: every 5s (first 12s), every 2s (next 8s), every 1s (final 4s)
    if (criticalStateActive && criticalMonologueComplete) {
        uint32_t elapsed = now - criticalStartTime - MONOLOGUE_DURATION;
        uint32_t tickInterval;
        if (elapsed < 12000) tickInterval = 5000;       // slow dread
        else if (elapsed < 20000) tickInterval = 2000;   // building
        else tickInterval = 1000;                         // urgent
        int currentTick = (int)(elapsed / tickInterval);
        if (currentTick != lastTickSecond) {
            SFX::play(SFX::CRITICAL_TICK);
            lastTickSecond = currentTick;
        }
    }
    
    // Cancel critical state if user acts (enters any mode)
    if (criticalStateActive && !inCriticalScene) {
        HamletMode mode = Hamlet::getMode();
        if (mode != HamletMode::IDLE) {
            criticalStateActive = false;
            criticalMonologueComplete = false;
            ribEscapeDialogActive = false;
            ribEscapeIdx = 0;
            ribEscapeHoldStart = 0;
            Avatar::clearCinematic();
            Avatar::setGrassMoving(true, true);  // unfreeze
        }
    }
    
    // Timeout: nag expires. Never hard-poweroff from mood logic.
    if (criticalStateActive && (now - criticalStartTime > CRITICAL_TIMEOUT)) {
        Config::save();
        criticalStateActive = false;
        criticalMonologueComplete = false;
        inCriticalScene = false;
        ribEscapeDialogActive = false;
        ribEscapeIdx = 0;
        ribEscapeHoldStart = 0;
        Avatar::clearCinematic();
        Avatar::setGrassMoving(true, true);
            setPhrase("countdown expired.\nno shutdown performed.", AvatarState::SAD);
        phraseTimeout = 4000;
    }
    
    // === ORIGINAL RAIN MONOLOGUE SYSTEM (momentum-based weather) ===
    // Only trigger if NOT in critical state (avoid conflict)
    if (!criticalStateActive && rainingNow && !wasRaining) {
        // rain just started - monologue begins
        monologueIndex = 0;
        lastMonologueTime = now;
        setPhrase(RAIN_MONOLOGUE[0], AvatarState::SAD);
        phraseTimeout = 4000;
        monologueIndex = 1;
    } else if (!criticalStateActive && rainingNow && monologueIndex > 0 && monologueIndex < RAIN_MONOLOGUE_COUNT) {
        // continue monologue every 4 seconds
        if (now - lastMonologueTime >= 4000 && !hasPhrase()) {
            setPhrase(RAIN_MONOLOGUE[monologueIndex], AvatarState::SAD);
            phraseTimeout = 4000;
            lastMonologueTime = now;
            monologueIndex++;
        }
    }
    wasRaining = rainingNow;  // track state for next frame
    
    // feed thunder flash state to avatar (Weather handles timing)
    Avatar::setThunderFlash(Weather::isThunderFlashing());

    // Source-truth idle far/structure/furniture layers. Avatar still owns the
    // tree, back/front grass and pig; Weather still owns clouds, rain and the
    // thunder wash, so this wiring cannot duplicate limbs or precipitation.
    MenuPigRender::RP::update();
    TimeOfDay::State timeOfDay = currentIdleTimeOfDay(now);
    TimeOfDay::applyToRP(timeOfDay);
    // Prime the legacy RF-wave avoidance mask before the street draws its
    // theme-aware celestial layer. The old late monochrome sun compositor is
    // intentionally not used on the colored sky.
    Weather::drawSunBase(canvas, Display::getColorFG());
    drawStreetBackdropCached(canvas, timeOfDay, now);

    // inverted background if thunder flashing
    if (Weather::isThunderFlashing()) {
        canvas.fillRect(0, TOP_BAR_H, SCREEN_WIDTH, MAIN_H, Display::getColorFG());
    }

    // shockwave dome: behind pig while expanding, covers pig when filled
    if (!Weather::isShockwaveCoveringPig()) {
        Weather::drawShockwave(canvas);
    }

    // debris rain behind avatar (fragments fall behind pig)
    Weather::drawDebrisRain(canvas, Display::getColorFG());

    // L2 open-air precipitation stays behind Pancetta and the avatar-owned
    // foreground grass. HUD and speech remain clean above it.
    Weather::draw(canvas, Display::getColorFG(), Display::getColorBG());

    // avatar layer (stars/bounds/pig/grass — draws over sky objects)
    // Suppressed during teleport so pig stays hidden until particles converge.
    if (!Teleport::isPigHidden()) {
        Avatar::draw(canvas);
    }

    // shockwave dome: over pig when screen filled (eats the pig)
    if (Weather::isShockwaveCoveringPig()) {
        Weather::drawShockwave(canvas);
    }

    // === CRITICAL COUNTDOWN OVERLAY === (Phase 8)
    if (criticalStateActive && criticalMonologueComplete) {
        uint16_t fg = Display::getColorFG();
        uint16_t bg = Display::getColorBG();
        
        int elapsed = (millis() - criticalStartTime) / 1000;
        int remaining = 60 - elapsed;
        if (remaining < 0) remaining = 0;
        
        // Countdown display (top center)
        canvas.setTextSize(2);
        canvas.setTextDatum(MC_DATUM);
        canvas.setTextColor(fg);

        char countdownText[32];
        snprintf(countdownText, sizeof(countdownText), "%d SECONDS", remaining);
        canvas.drawString(countdownText, SCREEN_WIDTH / 2, TOP_BAR_H + 10);  // MC_DATUM, textSize 2

        // Action hints (bottom, small text)
        canvas.setTextColor(bg);
        canvas.setTextSize(1);
        canvas.setTextDatum(BC_DATUM);
        
        if (ribEscapeDialogActive) {
            // Show escape sequence progress (XP reward)
            char seqDisplay[32];
            int pos = snprintf(seqDisplay, sizeof(seqDisplay), "[B]A [C]B: ");
            for (int i = 0; i < 6 && pos < (int)sizeof(seqDisplay) - 3; i++) {
                if (i < ribEscapeIdx) {
                    pos += snprintf(seqDisplay + pos, sizeof(seqDisplay) - pos, "X ");
                } else {
                    pos += snprintf(seqDisplay + pos, sizeof(seqDisplay) - pos, "%c ", ribEscapeSequence[i]);
                }
            }
            canvas.drawString(seqDisplay, SCREEN_WIDTH / 2, SCREEN_HEIGHT - BOTTOM_BAR_H - 2);
        } else {
            canvas.drawString("[A+B HOLD]=escape sequence", SCREEN_WIDTH / 2, SCREEN_HEIGHT - BOTTOM_BAR_H - 2);
        }
        
        canvas.setTextDatum(TL_DATUM);
    }
    
    // draw speech bubble after avatar (stay above box)
    // suppress during explosion sequence (shuttle destruction → debris rain)
    // suppress during teleport (pig not yet visible)
    if (!Avatar::isTransitioning() && !Weather::isExplosionActive() &&
        !Teleport::isPigHidden()) {
        if (hasPhrase()) {
            drawBubble(canvas, currentPhrase);
        }
    }

    // shockwave draw moved to layer-aware position above
}

// Phase D: critical mood countdown for display system
int Mood::getCriticalCountdown() {
    // Only show countdown after monologue completes (36s delay)
    if (!criticalStateActive || !criticalMonologueComplete) return -1;
    uint32_t elapsed = millis() - criticalStartTime;
    if (elapsed >= CRITICAL_TIMEOUT) return 0;
    return (int)((CRITICAL_TIMEOUT - elapsed) / 1000);  // seconds remaining
}

bool Mood::isFirstCritical() {
    return criticalStateActive && !criticalMonologueShown;
}

bool Mood::isInCriticalScene() {
    return inCriticalScene;
}

static void generateRibEscapeSequence() {
    // Random 6-char A/B sequence (same logic as feeding menu)
    bool allSame = true;
    while (allSame) {
        for (int i = 0; i < 6; i++) {
            ribEscapeSequence[i] = (esp_random() % 2 == 0) ? 'A' : 'B';
        }
        ribEscapeSequence[6] = '\0';
        
        allSame = true;
        for (int i = 1; i < 6; i++) {
            if (ribEscapeSequence[i] != ribEscapeSequence[0]) {
                allSame = false;
                break;
            }
        }
    }
    ribEscapeIdx = 0;
}

void Mood::trigger4MinCritical() {
    // Phase 3: 4-min IDLE timeout triggers critical scene
    // Reuse existing critical scene logic (-80 mood path)
    if (!criticalStateActive) {
        uint32_t now = millis();
        
        // Trigger critical scene with simplified phrase
        setPhrase("UNSAVED CAPTURES.\nIDLE LIMIT REACHED.", AvatarState::SAD);
        phraseTimeout = 4000;
        
        criticalStateActive = true;
        criticalMonologueComplete = true;  // skip monologue for 4-min trigger
        monologueIndex = 0;  // reset rain monologue so it restarts cleanly
        inCriticalScene = false;  // allow action immediately
        criticalStartTime = now;
        lastTickSecond = -1;
        ribEscapeDialogActive = false;  // clear stale escape state
        ribEscapeIdx = 0;
        ribEscapeHoldStart = 0;
        Avatar::setGrassMoving(false, false);
    }
}

void Mood::checkRibEscapeInput() {
    // escape sequence trigger during countdown (after monologue)
    // repurposed: awards XP instead of consuming a rib
    if (!criticalStateActive || !criticalMonologueComplete || ribEscapeDialogActive) {
        return;
    }

    // A+B hold trigger
    bool triggerPressed = M5.BtnA.isPressed() && M5.BtnB.isPressed();
    uint32_t holdDuration = 1000;  // 1 second hold
    
    if (triggerPressed) {
        if (ribEscapeHoldStart == 0) {
            ribEscapeHoldStart = millis();
        } else if (millis() - ribEscapeHoldStart >= holdDuration) {
            // Hold complete - activate rib escape dialog
            ribEscapeDialogActive = true;
            ribEscapeHoldStart = 0;
            generateRibEscapeSequence();
            // no SFX - visual feedback only (overlay shows sequence)
        }
    } else {
        ribEscapeHoldStart = 0;
    }
}

bool Mood::handleRibEscapeButton(char btn) {
    // Phase 4: Handle A/B input during rib escape sequence
    if (!ribEscapeDialogActive) return false;
    
    // Check button against sequence
    if (ribEscapeIdx >= 6) return false;
    
    if (ribEscapeSequence[ribEscapeIdx] == btn) {
        // Correct button
        ribEscapeIdx++;
        // no SFX - visual feedback only
        
        if (ribEscapeIdx >= 6) {
            // Sequence complete - XP from panic!
            Config::addXP(50);
            addMomentum(MOMENTUM_RIB_ESCAPE);

            // Cancel critical state
            criticalStateActive = false;
            criticalMonologueComplete = false;
            inCriticalScene = false;
            ribEscapeDialogActive = false;
            ribEscapeSequence[0] = '\0';
            ribEscapeIdx = 0;
            Avatar::clearCinematic();
            Avatar::setGrassMoving(true, true);

            setPhrase("ESCAPE COMPLETE.\n+50 XP, TIMER CLEARED.", AvatarState::EXCITED);
            phraseTimeout = 4000;
            SFX::play(SFX::RIB_ESCAPE);
            return true;
        }
        return true;   // consume button — don't let normal handlers fire
    } else {
        // Wrong button - cancel dialog
        ribEscapeDialogActive = false;
        ribEscapeSequence[0] = '\0';
        ribEscapeIdx = 0;
        SFX::play(SFX::ERROR);
        setPhrase("sequence mismatch.\ntimer still active.", AvatarState::SAD);
        phraseTimeout = 2000;
        return true;   // consume button — wrong press still ate the input
    }
}
