/**
 * Spectrum Mode — measured WiFi spectrum and client evidence.
 *
 * ==[ RF PERISCOPE ]== Native 2.4GHz dwell samples, optional C5 5GHz
 * observations, client detail, and the PARANOID SWINE deauth watch share one
 * display. Measured and modeled layers remain visibly distinct.
 */

#include "spectrum.h"
#include "spectrum_deauth_math.h"
#include "spectrum_rsn_math.h"
#include "spectrum_thru_math.h"
#include "../defense/defense_pipeline.h"
#include "../build_info.h"
#include <M5Unified.h>
#include <esp_wifi.h>
#include <esp_wifi_types.h>
#include <esp_heap_caps.h>
#if defined(HAMLET_CORE3SE)
#include <esp_event.h>
#endif
#include <atomic>
#include <new>
#include "../piglet/mood.h"
#include "../piglet/avatar.h"
#include "../ui/display.h"
#include "../activity/pedometer.h"
#include "../core/config.h"
#include "../core/gps.h"
#include "../audio/sfx.h"
#include "../locate/geiger.h"
#include "../util/rf_util.h"
#include "../util/rf_measurement.h"
#include "../util/rf_packet_ring.h"
#include "../util/rf_trace.h"
#include "../util/rf_route.h"
#include "../util/bearing.h"
#include "../util/csi_tracker.h"
#include "../ui/geiger_scan_math.h"
#include "../ui/geiger_scan_view.h"
#include "../sync/nowflock_transport.h"
#include "../core/wsl_bypasser.h"
#include "../core/power.h"
#include "hunt.h"  // CHANNEL_ORDER, CHANNEL_COUNT
#include "../defense/recon.h"
#include "../defense/potfile.h"
#include "../util/wifi_qr.h"
#include "../ui/ui_measurements.h"
#include "../input/touch_hints.h"
#include "../util/debug_log.h"
#include "../util/time_math.h"
#include "../radio/c5monster_uart.h"
#include "spectrum_c5_policy.h"

namespace Spectrum {

// ==[ TUNING CONSTANTS ]==

// --- Carrier/waterfall packet texture ---
static constexpr uint8_t WATERFALL_QUIET_FLOOR  = 28u;
static constexpr uint16_t PACKET_DENSITY_PPS_MAX = 400u;

// --- Proximity victory RSSI thresholds ---
static constexpr int8_t VICTORY_RSSI_SMOOTH      = -15;   // smoothed RSSI for close contact
static constexpr int8_t VICTORY_RSSI_RAW          = -20;   // raw RSSI confirmation

// --- Readiness score RSSI thresholds ---
static constexpr int8_t READINESS_RSSI_STRONG     = -50;   // +10 bonus
static constexpr int8_t READINESS_RSSI_GOOD       = -65;   // +5 bonus
static constexpr int8_t READINESS_RSSI_WEAK       = -80;   // -10 penalty

// --- Channel frequency conversion ---
static constexpr uint16_t FREQ_BASE               = 2407;  // ch1-13: base + ch*5
static constexpr uint8_t  FREQ_STEP               = 5;
static constexpr uint16_t FREQ_CH14               = 2484;  // Japan channel 14
static constexpr uint16_t FREQ_DEFAULT            = 2437;  // ch6 fallback
// ==[ 5GHz CHANNEL TABLE ]== exact JanOS channel_view channel set
static constexpr const uint8_t* CH5G_COMMON =
    SpectrumC5Policy::CHANNELS_5GHZ;
static constexpr uint8_t CH5G_COMMON_COUNT =
    SpectrumC5Policy::CHANNEL_5GHZ_COUNT;

// --- Shake detection ---
static constexpr float SHAKE_THRESHOLD_G          = 0.8f;  // accel delta for shake
static constexpr uint32_t SHAKE_DEBOUNCE_MS       = 300;

// --- Swept analyzer display response ---
// RF evidence changes only when a real dwell/sweep closes. These fixed-point
// display states interpolate that evidence between frames; they never inject
// sinusoidal amplitude, random topology, or IMU deformation.
static constexpr int16_t TRACE_FLOOR_X8 = -100 * 8;
struct MeasuredTraceState {
    int16_t liveTargetX8 = TRACE_FLOOR_X8;
    int16_t liveDisplayX8 = TRACE_FLOOR_X8;
    int16_t averageTargetX8 = TRACE_FLOOR_X8;
    int16_t averageDisplayX8 = TRACE_FLOOR_X8;
    int16_t maxHoldTargetX8 = TRACE_FLOOR_X8;
    int16_t maxHoldDisplayX8 = TRACE_FLOOR_X8;
    uint8_t spreadDb = 0u;
    uint8_t liveTargetAlpha = 0u;
    uint8_t liveDisplayAlpha = 0u;
    uint16_t sweepColumn = 0u;
    bool averageValid = false;
    bool maxHoldValid = false;
    bool covered = false;
};
static MeasuredTraceState measuredTrace[14] = {};
static uint32_t measuredTraceLastMs = 0u;
static constexpr uint16_t ANALYZER_SWEEP_DEFAULT_MS = 1300u;
static constexpr uint16_t ANALYZER_SWEEP_MIN_MS = 900u;
static constexpr uint16_t ANALYZER_SWEEP_MAX_MS = 1800u;
static constexpr uint16_t WATERFALL_RESPONSE_MS = 260u;
static constexpr uint16_t WATERFALL_FRAME_MS = 100u;
static uint32_t analyzerSweepStartedMs = 0u;
static uint32_t analyzerSweepLastCompletedMs = 0u;
static uint16_t analyzerSweepPeriodMs = ANALYZER_SWEEP_DEFAULT_MS;
static uint16_t analyzerSweepPhase = 0u;
static uint32_t waterfallLastFrameMs = 0u;

// ==[ PROGRESSIVE SPECTRUM REVEAL ]==
// The carrier plot band persists in the shared canvas between frames. Only the
// columns the sweep head newly crossed are repainted; everything left of the
// head keeps the prior reveal. A structural change (view pan/zoom, selection,
// network set, or leaving the plain 2.4GHz measured analyzer) forces one full
// band repaint so nothing ghosts. State lives here so the analyzer reset paths
// (start(), resetMeasuredTrace(), clearWaterfallHistory()) can invalidate it.
static bool spectrumPlotIncremental = false;  // this frame preserves the band
static int spectrumRevealPrevX = -1;          // sweep column painted last frame
struct SpectrumPlotSignature {
    float viewCenter = 0.0f;
    float viewWidth = 0.0f;
    int32_t selected = -2;
    uint32_t netCount = 0xFFFFFFFFu;
    uint32_t channelHash = 0u;
    bool modelOverlay = false;
    bool measured = false;
    bool band5g = false;    // 5GHz view paints different band content
    bool client = false;    // client monitor owns the whole viewport
};
static SpectrumPlotSignature spectrumLastPlotSig;

// ==[ STATE ]==
static std::atomic<bool> active{false};
static std::atomic<bool> paused{false};  // radio sharing problem
static bool clientMode = false;
static bool clientDetailActive = false;  // selected client's evidence dossier owns the viewport
static int16_t selectedIdx = 0;
static int16_t selectedClientIdx = 0;
static uint8_t selectedClientMac[6] = {};
static bool selectedClientIdentityValid = false;
static uint8_t lockedChannel = 0;  // 0 = hopping, 1-13 = locked
// ==[ SPECTRUM PAN STATE ]== frequency window shared by plot and touch mapping
static float viewCenterMHz = 2442.0f;  // default center (between ch6-7)
static float viewWidthMHz = 35.0f;     // view width - ~7 channels visible
static const float PAN_STEP_MHZ = 5.0f;  // one channel per pan
static const float MIN_CENTER_MHZ = 2412.0f;  // channel 1
static const float MAX_CENTER_MHZ = 2472.0f;  // channel 13

// 5GHz view constants
static const float VIEW_WIDTH_5GHZ_MAX = 200.0f;  // max 200 MHz for 5GHz
static const float VIEW_WIDTH_5GHZ_MIN = 100.0f;  // full lobe plus context

// 5GHz band constants
static constexpr uint16_t FREQ_BAND_5G_START =
    (uint16_t)SpectrumC5Policy::BAND_START_MHZ;
static constexpr uint16_t FREQ_BAND_5G_END =
    (uint16_t)SpectrumC5Policy::BAND_END_MHZ;

// ==[ CLIENT MONITORING ]== one BSSID lock, bounded discovery chime budget
static uint8_t monitoredBSSID[6] = {0};  // monitored network BSSID
static bool monitoredNetworkWasStale = false;
static uint8_t clientsDiscovered = 0;     // beep budget
static const uint8_t CLIENT_BEEP_LIMIT = 4;  // only beep for first N clients

#if defined(HAMLET_CORE3SE)
static constexpr uint32_t FTM_REQUEST_TIMEOUT_MS = 6000u;
static portMUX_TYPE ftmMux = portMUX_INITIALIZER_UNLOCKED;
static esp_event_handler_instance_t ftmEventInstance = nullptr;
static bool ftmEventRegistered = false;

struct FtmRuntime {
    FtmRangeStatus status = FtmRangeStatus::UNAVAILABLE;
    uint8_t targetBssid[6] = {};
    uint8_t resumeChannel = 1u;
    uint32_t requestStartedMs = 0u;
    uint32_t completedAtMs = 0u;
    uint64_t distanceSumCm = 0u;
    uint64_t distanceSquareSumCm2 = 0u;
    uint16_t sampleCount = 0u;
    bool responderAdvertised = false;
    bool active = false;
    bool resumeCapturePending = false;
};
static FtmRuntime ftmRuntime;
#endif
// ==[ POTFILE CACHE ]== avoid SPIFFS reads every frame in client monitor
static char potfileCacheSsid[33] = {0};
static char potfileCachePsk[65] = {0};
static void clearPotfileCache() {
    potfileCacheSsid[0] = '\0';
    potfileCachePsk[0] = '\0';
}
static bool lookupPotfileCred(const char* ssid, char* pskOut, size_t pskLen) {
    if (!ssid || !ssid[0] || !pskOut || pskLen == 0) return false;
    if (strcmp(potfileCacheSsid, ssid) != 0) {
        strncpy(potfileCacheSsid, ssid, 32);
        potfileCacheSsid[32] = '\0';
        potfileCachePsk[0] = '\0';
        Potfile::getPSK(ssid, potfileCachePsk, sizeof(potfileCachePsk));
    }
    if (!potfileCachePsk[0]) return false;
    strncpy(pskOut, potfileCachePsk, pskLen - 1);
    pskOut[pskLen - 1] = '\0';
    return true;
}
static std::atomic<bool> pendingClientBeep{false};    // deferred beep (callback-safe)
static std::atomic<bool> pendingSniff{false};          // deferred sniff animation (callback-safe)
// orientation now comes from Pedometer::isCachedFlat() — single source of truth

// ==[ NETWORKS ]== PSRAM-backed (saves ~12KB DRAM)
static SpectrumNetwork* networks = nullptr;
static uint16_t networkCount = 0;

// ==[ 5GHz NETWORK LIST ]== populated by feedC5MonsterScan for drawNetworkList
struct C5GHzNetwork {
    uint8_t sourceIndex;
    uint32_t sourceRevision;
    char ssid[33];
    uint8_t bssid[6];
    uint8_t channel;
    int8_t rssi;
    uint8_t authType;
    bool isHidden;
    uint32_t lastSeenMs;
};
static constexpr uint8_t MAX_C5G_NETWORKS = 32;
static constexpr uint32_t C5G_OBSERVATION_STALE_MS = 30000;
static C5GHzNetwork c5gNetworks[MAX_C5G_NETWORKS] = {};
static uint8_t c5gNetworkCount = 0;
static int8_t c5gSelectedIdx = -1;
static uint32_t c5gLastScanRevision = 0;
static uint32_t c5gLastScanMs = 0;
static bool c5SnapshotStripDirty = true;

static void focus5GHzNetwork(uint8_t channel) {
    if (channel < 36u || channel > 165u) return;
    viewCenterMHz =
        SpectrumC5Policy::focusedCenterMHz(channel, viewWidthMHz);
}

static void formatC5SnapshotAge(char* out, size_t outSize, uint32_t nowMs) {
    if (!out || outSize == 0) return;
    if (c5gLastScanMs == 0) {
        snprintf(out, outSize, "--");
        return;
    }
    const uint32_t ageSec = (nowMs - c5gLastScanMs) / 1000u;
    if (ageSec < 60u) {
        snprintf(out, outSize, "%lus", (unsigned long)ageSec);
    } else {
        snprintf(out, outSize, "%lum", (unsigned long)(ageSec / 60u));
    }
}

static int8_t findC5GHzNetworkByBssid(const uint8_t* bssid, uint8_t countLimit = 0xFF) {
    if (!bssid) return -1;
    uint8_t limit = (countLimit == 0xFF) ? c5gNetworkCount : countLimit;
    for (uint8_t i = 0; i < limit; i++) {
        if (memcmp(c5gNetworks[i].bssid, bssid, 6) == 0) return (int8_t)i;
    }
    return -1;
}

struct C5ArsenalCommand {
    const char* label;
    const char* cmd;
    enum class Kind : uint8_t {
        RECALIBRATE,
        RESCAN,
        TARGET_OBSERVE,
        COMMAND
    } kind;
    enum class Target : uint8_t {
        NONE,
        NETWORK,
        CHANNEL
    } target;
};
static const C5ArsenalCommand C5_ARSENAL_COMMANDS[] = {
    { "RECAL",       nullptr,                                C5ArsenalCommand::Kind::RECALIBRATE, C5ArsenalCommand::Target::NETWORK },
    { "OBSERVE",     C5Protocol::CMD_OBSERVE_BSSID,          C5ArsenalCommand::Kind::TARGET_OBSERVE, C5ArsenalCommand::Target::NETWORK },
    { "RESCAN",      C5Protocol::CMD_SCAN_NETWORKS,          C5ArsenalCommand::Kind::RESCAN,      C5ArsenalCommand::Target::NONE },
    { "PPS_LIVE",    C5Protocol::CMD_PACKET_MONITOR,         C5ArsenalCommand::Kind::COMMAND,     C5ArsenalCommand::Target::CHANNEL },
    { "CHAN_MAP",    C5Protocol::CMD_CHANNEL_VIEW,           C5ArsenalCommand::Kind::COMMAND,     C5ArsenalCommand::Target::NONE },
    { "DEAUTH",      C5Protocol::CMD_START_DEAUTH,           C5ArsenalCommand::Kind::COMMAND,     C5ArsenalCommand::Target::NETWORK },
    { "HANDSHAKE",   C5Protocol::CMD_START_HANDSHAKE,        C5ArsenalCommand::Kind::COMMAND,     C5ArsenalCommand::Target::NETWORK },
    { "SNIFF_AP",    C5Protocol::CMD_START_SNIFFER,          C5ArsenalCommand::Kind::COMMAND,     C5ArsenalCommand::Target::NETWORK },
    { "DEAUTH_WATCH", C5Protocol::CMD_DEAUTH_DETECTOR,       C5ArsenalCommand::Kind::COMMAND,     C5ArsenalCommand::Target::NONE },
    { "BLACKOUT",    C5Protocol::CMD_START_BLACKOUT,         C5ArsenalCommand::Kind::COMMAND,     C5ArsenalCommand::Target::NONE },
    { "DOG",         C5Protocol::CMD_START_SNIFFER_DOG,      C5ArsenalCommand::Kind::COMMAND,     C5ArsenalCommand::Target::NONE },
    { "EVIL_TWIN",   C5Protocol::CMD_START_EVIL_TWIN,        C5ArsenalCommand::Kind::COMMAND,     C5ArsenalCommand::Target::NETWORK },
    { "SAE",         C5Protocol::CMD_SAE_OVERFLOW,           C5ArsenalCommand::Kind::COMMAND,     C5ArsenalCommand::Target::NETWORK },
    { "WARDRIVE",    C5Protocol::CMD_START_WARDRIVE,         C5ArsenalCommand::Kind::COMMAND,     C5ArsenalCommand::Target::NONE },
    { "WDR_PROMISC", C5Protocol::CMD_START_WARDRIVE_PROMISC, C5ArsenalCommand::Kind::COMMAND,     C5ArsenalCommand::Target::NONE },
    { "STOP",        C5Protocol::CMD_STOP,                   C5ArsenalCommand::Kind::COMMAND,     C5ArsenalCommand::Target::NONE },
};
static constexpr uint8_t C5_ARSENAL_COMMAND_COUNT =
    (uint8_t)(sizeof(C5_ARSENAL_COMMANDS) / sizeof(C5_ARSENAL_COMMANDS[0]));
static uint8_t c5ArsenalSelectedIdx = 0;

static bool getFreshC5PacketRate(uint8_t channel, uint32_t& pps) {
    if (C5Monster::getActiveOperation() !=
        C5Monster::Operation::PACKET_MONITOR) {
        return false;
    }
    uint8_t monitoredChannel = 0;
    uint32_t ageMs = 0;
    if (!C5Monster::getPacketMonitorSample(monitoredChannel, pps, ageMs)) {
        return false;
    }
    return monitoredChannel == channel && ageMs <= 2500u;
}

// ==[ BSSID HASH TABLE ]== O(1) network lookup, PSRAM-backed
static constexpr uint16_t SPEC_HASH_SIZE = 64;
static constexpr uint16_t SPEC_HASH_EMPTY = 0xFFFF;
static uint16_t* specHashTable = nullptr;  // allocated in enterSpectrum()

static inline uint32_t specBssidHash(const uint8_t* bssid) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < 6; i++) h = (h ^ bssid[i]) * 16777619u;
    return h;
}

static uint16_t specHashFind(const uint8_t* bssid) {
    if (!specHashTable) {
        for (uint16_t i = 0; i < networkCount; i++) {
            if (memcmp(networks[i].bssid, bssid, 6) == 0) return i;
        }
        return SPEC_HASH_EMPTY;
    }
    uint32_t slot = specBssidHash(bssid) & (SPEC_HASH_SIZE - 1);
    for (uint16_t probe = 0; probe < SPEC_HASH_SIZE; probe++) {
        uint16_t idx = specHashTable[(slot + probe) & (SPEC_HASH_SIZE - 1)];
        if (idx == SPEC_HASH_EMPTY) return SPEC_HASH_EMPTY;
        if (memcmp(networks[idx].bssid, bssid, 6) == 0) return idx;
    }
    return SPEC_HASH_EMPTY;
}

static void specHashInsert(const uint8_t* bssid, uint16_t idx) {
    if (!specHashTable) return;
    uint32_t slot = specBssidHash(bssid) & (SPEC_HASH_SIZE - 1);
    for (uint16_t probe = 0; probe < SPEC_HASH_SIZE; probe++) {
        uint16_t& entry = specHashTable[(slot + probe) & (SPEC_HASH_SIZE - 1)];
        if (entry == SPEC_HASH_EMPTY || entry == idx) { entry = idx; return; }
    }
}

static void specHashRebuild() {
    if (!specHashTable) return;
    memset(specHashTable, 0xFF, sizeof(uint16_t) * SPEC_HASH_SIZE);
    for (uint16_t i = 0; i < networkCount; i++) {
        specHashInsert(networks[i].bssid, i);
    }
}

// ==[ CALLBACK-SAFE RF HANDOFF ]==
// The Wi-Fi callback copies one bounded observation and returns. Parsing and
// all network/state mutation happen on the main loop.
static constexpr uint16_t RF_EVENT_PAYLOAD_BYTES = 256u;
#if defined(HAMLET_CORE3SE)
static constexpr size_t RF_EVENT_RING_CAPACITY = 64u;
static constexpr uint16_t RF_EVENT_DRAIN_BUDGET = 96u;
static constexpr size_t RF_TRACE_CAPACITY = 64u;
#else
static constexpr size_t RF_EVENT_RING_CAPACITY = 6u;
static constexpr uint16_t RF_EVENT_DRAIN_BUDGET = 32u;
static constexpr size_t RF_TRACE_CAPACITY = 16u;
#endif

struct SpectrumPacketEvent {
    uint32_t observedMs = 0u;
    uint32_t rxTimestampUs = 0u;
    uint16_t frameLength = 0u;
    uint16_t payloadLength = 0u;
    int8_t rssi = -127;
    int8_t noiseFloor = -127;
    uint8_t channel = 0u;
    uint8_t sigMode = 0u;
    uint8_t phyRate = 0u;
    uint8_t channelWidth = 0u;
    uint8_t frameType = 0u;
    uint8_t frameSubtype = 0u;
    uint8_t payload[RF_EVENT_PAYLOAD_BYTES] = {};
};

using SpectrumEventRing =
    RfPacketRing::SpscRing<SpectrumPacketEvent, RF_EVENT_RING_CAPACITY>;
static SpectrumEventRing packetEvents;
using SpectrumTraceRing = RfTrace::Ring<RF_TRACE_CAPACITY>;
static SpectrumTraceRing* rfTrace = nullptr;
static uint32_t lastTracePacketMs = 0u;
// Main-loop-only measurement state lives in PSRAM. The callback ring remains
// internal DRAM because the ESP32 cache can be unavailable in radio context.
static RfMeasurement::Tracker* rfMeasurements = nullptr;
static RfMeasurement::SweepSnapshot* measuredSweep = nullptr;
static bool measuredSweepValid = false;
static bool modelOverlayEnabled = false;
static uint16_t currentHopIntervalMs = 100u;
static uint32_t lastReportedPacketDrops = 0u;
#if defined(HAMLET_CORE3SE)
static RfRoute::Tracker* gpsRoute = nullptr;
#endif

static void discardPartialMeasurementSweep(uint8_t channel) {
    if (rfMeasurements) {
        rfMeasurements->discardPartial(millis(), channel);
    }
}

// ==[ HIDDEN SSID PROBING ]== directed probes to reveal hidden nets
static const uint8_t MAX_PROBE_ATTEMPTS = 3;
static const uint32_t PROBE_SPACING_MS = 25;
static uint32_t lastProbeTime = 0;

// ==[ CHANNEL HOPPING ]==
static std::atomic<uint8_t> currentChannel{1};
static uint8_t channelIndex = 0;
static uint32_t lastHopTime = 0;  // millis() timestamp for channel hop cadence
// channel order from hunt.h (single definition)

// ==[ SINC LUT FOR REALISTIC RF LOBES ]==
// Formula: |sin(π * d / BW) / (π * d / BW)| where BW = 11 (WiFi channel half-bandwidth)
// Side lobes naturally decay: main lobe at 0, first nulls at ±11 MHz, side lobes between
// Extended range to ±22 MHz to show 2 side lobes per side
// Index 0-44 maps to distance -22 to +22 MHz
static const float SINC_LUT[45] = {
    // d = -22 to -18 (2nd side lobe region, left)
    0.0000f, 0.0650f, 0.1100f, 0.1300f, 0.1100f,
    // d = -17 to -13 (approaching 2nd null)
    0.0650f, 0.0000f, 0.0900f, 0.1500f, 0.1800f,
    // d = -12 to -8 (1st side lobe, left - peaks around -16)
    0.1500f, 0.0000f, 0.1700f, 0.2700f, 0.3300f,
    // d = -7 to -3 (main lobe rising)
    0.3700f, 0.5000f, 0.6500f, 0.8000f, 0.9100f,
    // d = -2 to 0 (main lobe peak)
    0.9700f, 0.9950f, 1.0000f,
    // d = 1 to 5 (main lobe falling)
    0.9950f, 0.9700f, 0.9100f, 0.8000f, 0.6500f,
    // d = 6 to 10 (main lobe edge to 1st null)
    0.5000f, 0.3700f, 0.3300f, 0.2700f, 0.1700f,
    // d = 11 to 15 (1st null and 1st side lobe, right)
    0.0000f, 0.1500f, 0.1800f, 0.1500f, 0.0900f,
    // d = 16 to 20 (2nd null region)
    0.0000f, 0.0650f, 0.1100f, 0.1300f, 0.1100f,
    // d = 21 to 22 (2nd side lobe tail)
    0.0650f, 0.0000f
};

// ==[ SPECTRUM ANALYZER BUFFERS ]== PSRAM
// spectrumBuffer owns only the current C5 snapshot profile. The native
// waterfall has its own completed-sweep history; unused persistence/peak
// allocations must not be able to disable either evidence surface.
static int8_t*  spectrumBuffer  = nullptr;  // [SPECTRUM_WIDTH] C5 snapshot profile
static uint8_t* waterfallBuffer = nullptr;  // [WATERFALL_ROWS * SPECTRUM_WIDTH] history
static uint8_t* waterfallTargetRow = nullptr;  // completed-sweep activity target
static int16_t* waterfallDisplayX8 = nullptr;  // VBW-style display response
static uint8_t waterfallWriteRow = 0;       // Current write position (circular)

// Get Sinc amplitude at distance d from center using LUT + interpolation
static float getSincAmplitude(float dist) {
    float lutPos = dist + 22.0f;  // Map -22..+22 to 0..44
    if (lutPos < 0.0f || lutPos > 44.0f) return 0.0f;
    int lutIdx = (int)lutPos;
    float frac = lutPos - lutIdx;
    if (lutIdx >= 44) return SINC_LUT[44];
    return SINC_LUT[lutIdx] + frac * (SINC_LUT[lutIdx + 1] - SINC_LUT[lutIdx]);
}

// ==[ CHANNEL STATS ]==
static volatile int8_t channelPeakRSSI[14];   // peak RSSI per channel — callback-written
static uint16_t channelNetCount[14];           // network count per channel
static volatile int8_t channelAvgRSSI[14];     // rolling average RSSI — callback-written
static uint16_t channelAttackableCount[14];  // attackable networks per channel (WPA/WPA2-PSK, non-PMF)
// ==[ RECON STATS ]==
static std::atomic<uint32_t> channelActivity[14];  // frame activity per channel (cumulative)
static uint32_t channelTimeMs[14];             // dwell time per channel (ms)
static uint32_t reconLastUpdateMs = 0;         // last recon update timestamp (ms)
static uint32_t lastChannelTimeUpdate = 0;
static uint8_t lastTimedChannel = 1;
// ==[ DISCOVERY TRACKING ]== bottom bar freshness indicator
static uint32_t lastNewNetworkTime = 0;   // when networkCount last increased
static uint16_t prevNetworkCount = 0;     // previous frame's network count
// ==[ RSSI HISTORY FOR WATERFALL ]==
#define RSSI_HISTORY_LEN 30
static int8_t* rssiHistory = nullptr;  // [14 * RSSI_HISTORY_LEN] per-channel history (PSRAM)
static uint8_t historyPos = 0;
static uint32_t lastHistoryUpdate = 0;
static const uint32_t HISTORY_INTERVAL = 200;  // update every 200ms
// ==[ MORSE \"DEAUTH\" ALERT ]==
// D=-.. E=. A=.- U=..- T=- H=....
// 0=dit, 1=dah, 0xFF=letter gap, 0xFE=end
static const uint8_t MORSE_DEAUTH[] = {
    1,0,0,        // D = -.. 
    0xFF,
    0,            // E = .
    0xFF,
    0,1,          // A = .-
    0xFF,
    0,0,1,        // U = ..-
    0xFF,
    1,            // T = -
    0xFF,
    0,0,0,0,      // H = ....
    0xFE          // end
};
static const uint16_t MORSE_DIT_MS = 60;      // dit duration
static const uint16_t MORSE_DAH_MS = 180;     // dah = 3× dit
static const uint16_t MORSE_GAP_MS = 60;      // inter-element gap
static const uint16_t MORSE_LETTER_MS = 180;  // inter-letter gap (3× dit)
static const uint16_t MORSE_FREQ = 800;       // classic morse tone
// ==[ PARANOID SWINE STATE ]==
static ParanoidSwine paranoid = {};
static std::atomic<uint32_t> pendingDeauthBatch{0u};
static std::atomic<bool> pendingNewNetworkMood{false};   // deferred Mood::onNewNetwork (callback-safe)
static std::atomic<uint8_t> pendingChannelRecount{0};    // deferred channel stats recount (callback-safe)

// ==[ DIAL MODE: GYRO CHANNEL SELECTION ]==
// When device goes UPRIGHT (UPS), dial mode activates automatically
// Gyro tilt left/right selects channel with smooth sliding indicator
static bool dialMode = false;              // auto-enabled when UPS (upright)
static bool dialLocked = false;            // channel lock (button A toggles)
static uint8_t dialChannel = 7;            // current dial channel (1-13)
static float dialPositionTarget = 7.0f;    // raw gyro position (1.0-13.0)
static float dialPositionSmooth = 7.0f;    // lerped display position (smooth)
static uint32_t lastDialUpdate = 0;        // timing for lerp
static std::atomic<uint32_t> ppsCounter{0};  // packet counter (callback increments)
static uint32_t displayPps = 0;            // displayed pps (updated per second)
static uint32_t lastPpsUpdate = 0;         // last pps calculation time
static void resetPpsWindow(uint32_t nowMs) {
    ppsCounter.store(0u, std::memory_order_release);
    displayPps = 0u;
    lastPpsUpdate = nowMs;
}
// ==[ CLIENT BEARING TRACKER ]== same math, different target
static Bearing::TrackerState clientBearing;
static Bearing::TrackerConfig clientBearingConfig;  // gyro magic for Geiger
static bool clientBearingConfigInited = false;
static uint32_t clientBearingLastSignalRxUs = 0;
static uint32_t clientBearingLastCsiRxUs = 0;
static uint32_t clientBearingLastPointRxUs = 0;
static bool clientBearingSignalConsumed = false;
static bool clientBearingCsiConsumed = false;
static bool clientBearingPointRxValid = false;
static int8_t clientRssiSmooth = -70;       // EMA smoothed client RSSI
static bool clientRssiSmoothValid = false;
static uint8_t clientBearingTargetMac[6] = {0};
static bool clientBearingTargetValid = false;
static uint32_t lastClientPokeMs = 0;       // last client keepalive attempt
static constexpr uint8_t kClientScanUsableCsiQuality = 6u;
static constexpr uint8_t kClientScanUsableCsiStability = 6u;
static constexpr uint8_t kClientScanUsableCsiConfidence = 4u;
static constexpr uint16_t kClientScanUsableCsiAgeMs = 4500u;
static constexpr uint16_t kClientScanYawConfidenceGate = 132u;
static constexpr uint32_t kClientPokeCooldownMs = 1500u;
static constexpr uint32_t kClientKeepaliveTargetAgeMs = 4500u;  // stale target cutoff
static constexpr float kDegToRad = 0.017453292519943295f;
static float clientScanRefHeading = 0.0f;
static bool clientScanRefValid = false;
static bool clientScanRefFlat = false;
// THRU rebases its acceleration map at a grip change. RAD must not: its SEEK
// ray and retained RF lock stay in the original world frame.
static float clientRadarRefHeading = 0.0f;
static bool clientRadarRefValid = false;

// ==[ C5 5GHz AP BEARING TRACKER ]==
// JanOS scan rows are AP snapshots, not client packets and not CSI. Native
// client detail and C5 AP detail are mutually exclusive, so reuse the large
// 192-point tracker storage but enforce an independent reset/revision
// lifecycle. A 2.4GHz lock can never leak into a selected 5GHz AP, and the
// latest completed scan is consumed on entry; only scans observed while this
// detail is open have a defensible device pose.
struct C5CarrierTarget {
    bool valid;
    char ssid[33];
    uint8_t bssid[6];
    uint8_t channel;
    int8_t rssi;
    int8_t rssiSmooth;
    uint8_t authType;
    bool isHidden;
    uint32_t lastSeenMs;
    uint32_t lastConsumedRevision;
    uint32_t lastTargetObservationRevision;
};

static bool c5CarrierDetailActive = false;
static C5CarrierTarget c5CarrierTarget = {};
static Bearing::TrackerState& c5CarrierBearing = clientBearing;
static Bearing::TrackerConfig c5CarrierBearingConfig;
static bool c5CarrierBearingConfigInited = false;
static float c5CarrierScanRefHeading = 0.0f;
static bool c5CarrierScanRefValid = false;
static bool c5CarrierScanRefFlat = false;
static float c5CarrierRadarRefHeading = 0.0f;
static bool c5CarrierRadarRefValid = false;
static bool c5CarrierLastKnownValid = false;
static uint16_t c5CarrierLastKnownHeadingDegX10 = 0;
static int16_t c5CarrierLastKnownElevDegX10 = 0;
static uint16_t c5CarrierLastKnownProximity = 0;
static int8_t c5CarrierLastKnownRssi = -127;
static float c5CarrierLastKnownObserverX = 0.0f;
static float c5CarrierLastKnownObserverY = 0.0f;
static uint32_t c5CarrierLastKnownSeenMs = 0;
static uint32_t c5CarrierLastKnownApproachConfirmCount = 0;
static uint32_t c5CarrierLastKnownLockGeneration = 0;
static constexpr uint8_t kC5ScanBearingConfidenceCap = 70u;

static void initClientBearingConfig() {
    if (clientBearingConfigInited) return;
    // Passive clients in the hardware trace arrived every 5-15s. Keep lock
    // decay outside that normal silence without weakening the sweep gate.
    clientBearingConfig.expectedCadenceMs = 8000u;
    clientBearingConfig.staleTimeout = 15000u;
    clientBearingConfig.stationarySettleMs = 300u;
    clientBearingConfig.stationaryBoostWhileIdle = 20u;
    clientBearingConfigInited = true;
}

static void initC5CarrierBearingConfig() {
    if (c5CarrierBearingConfigInited) return;
    c5CarrierBearingConfig.expectedCadenceMs = 8000u;
    c5CarrierBearingConfig.staleTimeout = C5G_OBSERVATION_STALE_MS;
    c5CarrierBearingConfig.stationarySettleMs = 300u;
    c5CarrierBearingConfig.stationaryBoostWhileIdle = 20u;
    // A full-band scan reports one coarse AP RSSI snapshot. Keep the normal
    // four-sector/five-sample lock gate and cap the UI confidence separately.
    c5CarrierBearingConfigInited = true;
}

// ==[ GHOST MARKER STATE ]== last known LOCK position when bearing drops
static bool clientLastKnownValid = false;
static uint16_t clientLastKnownHeadingDegX10 = 0;
static int16_t clientLastKnownElevDegX10 = 0;
static uint16_t clientLastKnownProximity = 0;
static int8_t clientLastKnownRssi = -127;
static float clientLastKnownObserverX = 0.0f;
static float clientLastKnownObserverY = 0.0f;
static uint32_t clientLastKnownSeenMs = 0;
static uint32_t clientLastKnownApproachConfirmCount = 0;
static uint32_t clientLastKnownLockGeneration = 0;

#if HAMLET_DEBUG_LOG
static uint32_t lastThruDebugMs = 0;
static bool lastThruDetailStable = false;
static bool lastThruDetailRetained = false;
static const uint32_t kThruDebugIntervalMs = 750u;
#endif

static float normalizeAngle180(float degrees) {
    while (degrees > 180.0f) degrees -= 360.0f;
    while (degrees < -180.0f) degrees += 360.0f;
    return degrees;
}

static uint16_t blendUInt16(uint16_t base, uint16_t blended, uint8_t ratio,
                            uint16_t maxValue) {
    const int16_t w = (int16_t)constrain((int)ratio, 0, 100);
    return (uint16_t)constrain(
        (int)((int32_t)base * (100 - w) + (int32_t)blended * w) / 100,
        0, maxValue);
}

static int roundFloatToInt(float v) {
    return (v >= 0.0f) ? (int)(v + 0.5f) : (int)(v - 0.5f);
}

static bool csiUsable(const CsiTracker::Snapshot& csi) {
    return csi.ageMs <= kClientScanUsableCsiAgeMs &&
            csi.quality >= kClientScanUsableCsiQuality &&
            csi.stability >= kClientScanUsableCsiStability &&
            csi.confidence >= kClientScanUsableCsiConfidence;
}

static bool observationPoseForRx(
    uint32_t rxTimestampUs, Bearing::ObservationPose& out) {
    out = {};
#if defined(HAMLET_CORE3SE)
    Pedometer::RfPoseSample pose{};
    if (!Pedometer::getRfPoseAt(rxTimestampUs, pose)) return false;
    out.valid = true;
    int headingX10 = (int)(pose.screenYawDeg * 10.0f + 0.5f);
    while (headingX10 >= 3600) headingX10 -= 3600;
    while (headingX10 < 0) headingX10 += 3600;
    out.headingDegX10 = (uint16_t)headingX10;
    out.elevationDegX10 = (int16_t)constrain(
        (int)(atan2f(
            pose.az,
            sqrtf(pose.ax * pose.ax + pose.ay * pose.ay)) *
              572.9578f),
        -900, 900);
    out.quality = pose.quality;
    out.flags = pose.flags;
    out.interpolationAgeUs = pose.interpolationAgeUs;
    out.yawDriftDegX10 = (int16_t)constrain(
        (int)(pose.yawDriftDeg * 10.0f + 0.5f), 0, 1800);
    return true;
#else
    (void)rxTimestampUs;
    return false;
#endif
}

static void updateBearingFromRfPose(
        Bearing::TrackerState& state,
        const Bearing::TrackerConfig& config,
        uint32_t nowMs) {
    Pedometer::RfPoseSample pose{};
    if (Pedometer::getLatestRfPose(pose)) {
        // Pedometer owns the high-rate screen-relative yaw integration. Using
        // its timestamped translation as well prevents a second render-cadence
        // integrator from drifting the live view away from packet-time poses.
        Bearing::AuthoritativePoseSample authoritative{};
        authoritative.timestampUs = pose.timestampUs;
        authoritative.headingDeg = pose.screenYawDeg;
        authoritative.linearAx = pose.linearAx;
        authoritative.linearAy = pose.linearAy;
        authoritative.velocityX = pose.velocityX;
        authoritative.velocityY = pose.velocityY;
        authoritative.positionX = pose.positionX;
        authoritative.positionY = pose.positionY;
        authoritative.stationary =
            (pose.flags & Pedometer::RF_POSE_STATIONARY) != 0u;
        Bearing::updateIMUAuthoritativePose(
            state, config,
            pose.gx, pose.gy, pose.gz,
            pose.ax, pose.ay, pose.az,
            authoritative, nowMs);
        return;
    }

    float ax, ay, az;
    float gx, gy, gz;
    Pedometer::getCachedAccel(ax, ay, az);
    Pedometer::getCachedGyro(gx, gy, gz);
    Bearing::updateIMU(
        state, config, gx, gy, gz, ax, ay, az, nowMs);
}

static void observeGpsRouteEvidence(int8_t rssi, uint32_t seenMs) {
#if defined(HAMLET_CORE3SE)
    if (!gpsRoute || !GPS::hasFix()) return;
    RfRoute::Fix fix{};
    fix.latitude = GPS::getLatitude();
    fix.longitude = GPS::getLongitude();
    fix.ageMs = GPS::getFixAgeMs();
    fix.hdop = GPS::getHdop();
    fix.speedKmh = GPS::getSpeedKmh();
    fix.courseDeg = GPS::getCourseDeg();
    fix.courseValid = GPS::hasCourse();
    gpsRoute->observe(seenMs, rssi, fix);
#else
    (void)rssi;
    (void)seenMs;
#endif
}

static void applyGpsRouteDiagnostics(
    GeigerScanView::ThroughTarget& target, uint32_t nowMs) {
#if defined(HAMLET_CORE3SE)
    if (!gpsRoute || gpsRoute->count() == 0u) return;
    RfRoute::Sample latest{};
    if (!gpsRoute->newest(latest)) return;
    const RfRoute::StrongestRegion region =
        gpsRoute->strongestRegion();
    target.gpsRouteValid = true;
    target.gpsEvidenceAgeMs = nowMs - latest.timestampMs;
    target.gpsFixAgeMs = latest.fixAgeMs;
    target.gpsHdopX10 = latest.hdopX10;
    target.gpsRouteSamples = gpsRoute->count();
    target.gpsStrongestRadiusM =
        region.valid ? region.radiusMeters : 0u;
#else
    (void)target;
    (void)nowMs;
#endif
}

static bool isClientBearingTarget(const SpectrumClient* client) {
    return client && clientBearingTargetValid &&
           memcmp(clientBearingTargetMac, client->mac, 6) == 0;
}

static void resetClientBearingState(const SpectrumClient* client);

static void ensureClientBearingTargetBound(const SpectrumClient* client) {
    if (!client) return;

    if (!isClientBearingTarget(client)) {
        resetClientBearingState(client);
        return;
    }

    if (!clientScanRefValid) {
        clientScanRefHeading = clientBearing.relativeHeading;
        clientScanRefFlat = Pedometer::isCachedFlat();
        clientScanRefValid = true;
    }
    if (!clientRadarRefValid) {
        clientRadarRefHeading = clientBearing.relativeHeading;
        clientRadarRefValid = true;
    }
}

static void assessClientHistoryConfidence(const Bearing::TrackerState& state,
                                        uint32_t nowMs,
                                        uint8_t& outDensity,
                                        uint8_t& outConsistency,
                                        uint8_t& outCadence,
                                        uint8_t& outConfidence,
                                        uint32_t historyWindowMs = 30000u,
                                        uint8_t historyTargetSamples = 8u);

static void maybeRebaseClientScanReference(const SpectrumClient* client) {
    if (!clientScanRefValid || !client || !isClientBearingTarget(client)) return;
    const bool isFlat = Pedometer::isCachedFlat();
    if (isFlat != clientScanRefFlat) {
        clientScanRefHeading = clientBearing.relativeHeading;
        clientScanRefFlat = isFlat;
    }
}

static bool pokeClientTarget() {
    if (!clientMode || !clientDetailActive) return false;
    const SpectrumNetwork* net = getSelectedNetwork();
    const SpectrumClient* client = getSelectedClient();
    if (!net || !client) return false;
    if (net->channel == 0u) return false;
    if (!isClientBearingTarget(client)) return false;
    const uint32_t nowMs = millis();
    if (SpectrumThruMath::observationAgeMs(
            nowMs, client->lastSeen) > kClientKeepaliveTargetAgeMs) {
        return false;
    }
    if (lastClientPokeMs != 0u && nowMs - lastClientPokeMs < kClientPokeCooldownMs) {
        return false;
    }

    lastClientPokeMs = nowMs;

#if HAMLET_DEBUG_LOG
    HAMLET_LOGF("[SPECTRUM] THRU poke=%02X:%02X:%02X:%02X:%02X:%02X ch=%u rssi=%d\n",
               client->mac[0], client->mac[1], client->mac[2],
               client->mac[3], client->mac[4], client->mac[5],
               net->channel, client->rssi);
#endif

    return WSLBypasser::sendEAPOLStart(net->bssid, client->mac);
}

static void assessClientHistoryConfidence(const Bearing::TrackerState& state,
                                        uint32_t nowMs,
                                        uint8_t& outDensity,
                                        uint8_t& outConsistency,
                                        uint8_t& outCadence,
                                        uint8_t& outConfidence,
                                        uint32_t historyWindowMs,
                                        uint8_t historyTargetSamples) {
    outDensity = 0u;
    outConsistency = 0u;
    outCadence = 0u;
    outConfidence = 0u;

    const uint8_t sampleCount = Bearing::getRfPointCount(state);
    if (sampleCount == 0) return;

    constexpr uint32_t kCadenceGoodMinMs = 220u;
    constexpr uint32_t kCadenceGoodMaxMs = 16000u;
    constexpr int     kConsistencyDeltaMax = 80;

    uint8_t pointsSeen = 0u;
    uint16_t confSum = 0u;
    uint16_t consistencyHits = 0u;
    uint16_t consistencyPairs = 0u;
    uint16_t cadenceHits = 0u;
    uint32_t oldestTs = nowMs;
    uint32_t newestTs = 0u;

    int16_t prevBearing = 0;
    uint8_t prevConfidence = 0u;
    uint32_t prevTs = 0u;
    bool havePrev = false;
    for (uint8_t i = 0; i < sampleCount; ++i) {
        Bearing::RfPoint p;
        if (!Bearing::getRfPointNewest(state, i, p)) break;

        const uint32_t ageMs = SpectrumThruMath::elapsedMs(nowMs, p.seenMs);
        if (ageMs > historyWindowMs) break;

        ++pointsSeen;
        oldestTs = p.seenMs;
        if (pointsSeen == 1u) newestTs = p.seenMs;
        confSum += p.confidence;
        if (havePrev) {
            if (prevConfidence >= 15u && p.confidence >= 15u) {
                consistencyPairs++;
                const int bearingDelta = abs(prevBearing - p.bearing);
                if (bearingDelta <= kConsistencyDeltaMax) consistencyHits++;
            }

            const uint32_t gapMs = SpectrumThruMath::elapsedMs(prevTs, p.seenMs);
            if (gapMs >= kCadenceGoodMinMs && gapMs <= kCadenceGoodMaxMs) {
                cadenceHits++;
            }
        }

        prevBearing = p.bearing;
        prevConfidence = p.confidence;
        prevTs = p.seenMs;
        havePrev = true;
    }

    if (pointsSeen == 0) return;

    const uint32_t spanMs = SpectrumThruMath::elapsedMs(newestTs, oldestTs);
    const uint8_t spanScore = historyWindowMs > 0u
        ? (uint8_t)constrain(
            (int)(spanMs * 100u / historyWindowMs), 0, 100)
        : 0u;

    const uint8_t densityTarget =
        historyTargetSamples > 0u ? historyTargetSamples : 1u;
    outDensity = constrain(
        (int)(pointsSeen * 100u / densityTarget), 0, 100);
    const uint8_t density = outDensity;

    const uint8_t avgConf = (uint8_t)constrain((int)(confSum / pointsSeen), 0, 100);

    if (pointsSeen >= 2) {
        outConsistency = consistencyPairs > 0u
            ? constrain((int)(consistencyHits * 100u / consistencyPairs), 0, 100)
            : 0u;
        outCadence = constrain((int)(cadenceHits * 100u / (pointsSeen - 1u)), 0, 100);
        outConfidence = (uint8_t)constrain(
            (int)(avgConf * 4 + (int)density * 2 +
                  (int)outConsistency * 2 + (int)outCadence * 1 +
                  (int)spanScore * 1) / 10,
            0, 100);
        if (pointsSeen < 4u) {
            outConfidence = (uint8_t)((outConfidence * pointsSeen) / 4u);
        }
    } else {
        outConsistency = (avgConf >= 35) ? 25u : 0u;
        outCadence = 0u;
        outConfidence = (uint8_t)(avgConf * 2 / 5);
    }
}

static void resetClientBearingState(const SpectrumClient* client) {
    const bool sameTarget = client && isClientBearingTarget(client);
    if (client) Pedometer::resetRfPoseOrigin();
    initClientBearingConfig();
    Bearing::reset(clientBearing);
#if defined(HAMLET_CORE3SE)
    if (gpsRoute) gpsRoute->reset();
#endif
    // Binding/recentering must not replay a cached observation at a new pose.
    clientBearingSignalConsumed = client && client->hasSignal;
    clientBearingLastSignalRxUs = clientBearingSignalConsumed
        ? client->lastSignalRxUs : 0u;
    clientBearingCsiConsumed = false;
    clientBearingLastCsiRxUs = 0u;
    if (sameTarget && CsiTracker::isTargetActive()) {
        CsiTracker::Snapshot csi{};
        if (CsiTracker::getSnapshot(csi) && csi.sampleCount > 0u) {
            clientBearingCsiConsumed = true;
            clientBearingLastCsiRxUs = csi.rxTimestampUs;
        }
    }
    clientBearingPointRxValid = false;
    clientBearingLastPointRxUs = 0u;
    clientRssiSmooth = client ? client->rssi : -70;
    clientRssiSmoothValid = client && client->hasSignal;
    clientLastKnownValid = false;
    clientLastKnownHeadingDegX10 = 0;
    clientLastKnownElevDegX10 = 0;
    clientLastKnownProximity = 0;
    clientLastKnownRssi = -127;
    clientLastKnownObserverX = 0.0f;
    clientLastKnownObserverY = 0.0f;
    clientLastKnownSeenMs = 0;
    clientLastKnownApproachConfirmCount = 0;
    clientLastKnownLockGeneration = 0;

    if (client) {
        memcpy(clientBearingTargetMac, client->mac, 6);
        clientBearingTargetValid = true;
        clientScanRefHeading = clientBearing.relativeHeading;
        clientScanRefFlat = Pedometer::isCachedFlat();
        clientScanRefValid = true;
        clientRadarRefHeading = clientBearing.relativeHeading;
        clientRadarRefValid = true;

#if HAMLET_DEBUG_LOG
        HAMLET_LOGF("[SPECTRUM] THRU reset=%02X:%02X:%02X:%02X:%02X:%02X\n",
                   client->mac[0], client->mac[1], client->mac[2],
                   client->mac[3], client->mac[4], client->mac[5]);
#endif
    } else {
        memset(clientBearingTargetMac, 0, sizeof(clientBearingTargetMac));
        clientBearingTargetValid = false;
        clientScanRefValid = false;
        clientScanRefFlat = false;
        clientScanRefHeading = 0.0f;
        clientRadarRefHeading = 0.0f;
        clientRadarRefValid = false;

#if HAMLET_DEBUG_LOG
        HAMLET_LOGF("[SPECTRUM] THRU reset clear\n");
#endif
    }
}

static void resetC5CarrierBearingState(const C5GHzNetwork* network) {
    if (network) Pedometer::resetRfPoseOrigin();
    initC5CarrierBearingConfig();
    Bearing::reset(c5CarrierBearing);
#if defined(HAMLET_CORE3SE)
    if (gpsRoute) gpsRoute->reset();
#endif
    memset(&c5CarrierTarget, 0, sizeof(c5CarrierTarget));
    c5CarrierLastKnownValid = false;
    c5CarrierLastKnownHeadingDegX10 = 0;
    c5CarrierLastKnownElevDegX10 = 0;
    c5CarrierLastKnownProximity = 0;
    c5CarrierLastKnownRssi = -127;
    c5CarrierLastKnownObserverX = 0.0f;
    c5CarrierLastKnownObserverY = 0.0f;
    c5CarrierLastKnownSeenMs = 0;
    c5CarrierLastKnownApproachConfirmCount = 0;
    c5CarrierLastKnownLockGeneration = 0;

    if (network) {
        c5CarrierTarget.valid = true;
        strncpy(c5CarrierTarget.ssid, network->ssid,
                sizeof(c5CarrierTarget.ssid) - 1u);
        c5CarrierTarget.ssid[sizeof(c5CarrierTarget.ssid) - 1u] = '\0';
        memcpy(c5CarrierTarget.bssid, network->bssid, sizeof(c5CarrierTarget.bssid));
        c5CarrierTarget.channel = network->channel;
        c5CarrierTarget.rssi = network->rssi;
        c5CarrierTarget.rssiSmooth = network->rssi;
        c5CarrierTarget.authType = network->authType;
        c5CarrierTarget.isHidden = network->isHidden;
        c5CarrierTarget.lastSeenMs = network->lastSeenMs;
        // Do not replay a scan completed before the target detail had a pose.
        c5CarrierTarget.lastConsumedRevision = c5gLastScanRevision;
        C5Monster::TargetObservationTelemetry targeted{};
        c5CarrierTarget.lastTargetObservationRevision =
            C5Monster::getTargetObservation(targeted)
                ? targeted.revision : 0u;
        c5CarrierScanRefHeading = c5CarrierBearing.relativeHeading;
        c5CarrierScanRefFlat = Pedometer::isCachedFlat();
        c5CarrierScanRefValid = true;
        c5CarrierRadarRefHeading = c5CarrierBearing.relativeHeading;
        c5CarrierRadarRefValid = true;
    } else {
        c5CarrierScanRefHeading = 0.0f;
        c5CarrierScanRefFlat = false;
        c5CarrierScanRefValid = false;
        c5CarrierRadarRefHeading = 0.0f;
        c5CarrierRadarRefValid = false;
    }
}

static void maybeRebaseC5CarrierScanReference() {
    if (!c5CarrierTarget.valid || !c5CarrierScanRefValid) return;
    const bool isFlat = Pedometer::isCachedFlat();
    if (isFlat != c5CarrierScanRefFlat) {
        c5CarrierScanRefHeading = c5CarrierBearing.relativeHeading;
        c5CarrierScanRefFlat = isFlat;
    }
}

static bool requestC5CarrierRescan() {
    if (!c5CarrierDetailActive || !c5CarrierTarget.valid ||
        !C5Monster::isConnected() || C5Monster::isBusy() ||
        C5Monster::hasActiveOperation()) {
        return false;
    }
    return C5Monster::sendCommand(C5Protocol::CMD_SCAN_NETWORKS);
}


// geiger needs landscape grip (cached IMU — no extra I2C read)
static void syncClientCsiTarget() {
#if defined(HAMLET_WIFI_CSI)
    if (clientMode && clientDetailActive) {
        const SpectrumClient* client = getSelectedClient();
        const SpectrumNetwork* net = getSelectedNetwork();
        if (client && net && net->channel != 0u) {
            CsiTracker::setTarget(CsiTracker::TARGET_WIFI_CLIENT, client->mac,
                                  net->channel);
#if HAMLET_DEBUG_LOG
            HAMLET_LOGF(
                "[SPECTRUM] THRU CSI target=%02X:%02X:%02X:%02X:%02X:%02X ch=%u\n",
                client->mac[0], client->mac[1], client->mac[2],
                client->mac[3], client->mac[4], client->mac[5],
                net->channel);
#endif
            return;
        }
    }
    CsiTracker::clearTarget();

#if HAMLET_DEBUG_LOG
    HAMLET_LOGF("[SPECTRUM] THRU CSI clear\n");
#endif
#else
    (void)0;
#endif
}

static bool isLandscapeOrientation() {
    float ax, ay, az;
    Pedometer::getCachedAccel(ax, ay, az);
    return fabsf(ax) > 0.5f;
}

static void drawClientScannerBottomBar(M5Canvas& canvas) {
    Display::drawBottomBar3To(&canvas, "[A/C]RECAL", "[SWIPE>]POKE",
                              "[B/C+]BACK");
}

static void drawC5CarrierScannerBottomBar(M5Canvas& canvas) {
    Display::drawBottomBar3To(&canvas, "[A]PREV", "[B]RUN",
                              "[C]NEXT [B+/C+]BACK");
}

// shake to toggle toast
static float lastAccelMag = 1.0f;
static uint32_t lastShakeTime = 0;
static bool detailToastVisible = false;
static uint32_t detailToastExpiry = 0;
static const uint32_t DETAIL_TOAST_LINGER = 3000;
// one trigger per shake (cached IMU)
static bool detectShakeEdge() {
    float ax, ay, az;
    Pedometer::getCachedAccel(ax, ay, az);
    float mag = sqrtf(ax*ax + ay*ay + az*az);
    float delta = fabsf(mag - lastAccelMag);
    lastAccelMag = mag;
    
    // shake threshold
    if (delta > SHAKE_THRESHOLD_G) {
        uint32_t now = millis();
        if (now - lastShakeTime > SHAKE_DEBOUNCE_MS) {  // debounce
            lastShakeTime = now;
            return true;
        }
    }
    return false;
}

// Check if detail toast should be visible (toggle + linger logic)
static bool isDetailToastVisible() {
    if (paranoid.attackActive) {
        // Attack ongoing: respect toggle state
        return detailToastVisible;
    } else {
        // Attack ended: check linger expiry
        return detailToastVisible && TimeMath::active(millis(), detailToastExpiry);
    }
}

// ==[ TIMING ]==
static uint32_t startTime = 0;
static uint32_t lastCleanup = 0;
static const uint32_t NETWORK_TIMEOUT = 30000;  // 30s stale timeout
static const uint32_t CLEANUP_INTERVAL = 5000;

// ==[ INTELLIGENCE: PROBE HARVEST ]== client preferred network list
struct PendingProbe {
    uint8_t clientMAC[6];
    char ssid[33];
    int8_t rssi;
    std::atomic<bool> ready{false};
};
static PendingProbe pendingProbe = {};
static SpectrumProbeEntry* probeTable = nullptr;  // PSRAM
static uint16_t probeCount = 0;

// ==[ INTELLIGENCE: PSRAM PARALLEL ARRAYS ]== indexed by network slot
static uint32_t* beaconPrevTimestamp = nullptr;  // last rx_ctrl.timestamp per network
static uint16_t* beaconIntervalTU = nullptr;     // measured interval in TU (1024us)
static uint8_t*  beaconAnomaly = nullptr;        // anomaly flags
static uint8_t*  vulnTier = nullptr;             // T0-T7 classification
static uint8_t*  readinessScore = nullptr;       // 0-100 attack readiness
static BSSIDCluster* clusters = nullptr;
static uint8_t   clusterCount = 0;
static uint8_t*  networkClusterId = nullptr;     // 0=unclustered, 1+=cluster group

// ==[ INTELLIGENCE: SPARKLINES ]== per-network RSSI micro-history
#define SPARKLINE_SAMPLES 30
#define SPARKLINE_INTERVAL_MS 500
static int8_t*  sparklineBuffers = nullptr;      // [MAX_SPECTRUM_NETWORKS * SPARKLINE_SAMPLES]
static uint8_t* sparklineIdx = nullptr;           // [MAX_SPECTRUM_NETWORKS]
static uint32_t lastSparklineUpdate = 0;

static void clearNetworkSidecars(uint16_t idx) {
    if (idx >= MAX_SPECTRUM_NETWORKS) return;
    if (beaconPrevTimestamp) beaconPrevTimestamp[idx] = 0;
    if (beaconIntervalTU)    beaconIntervalTU[idx] = 0;
    if (beaconAnomaly)       beaconAnomaly[idx] = 0;
    if (vulnTier)            vulnTier[idx] = 7;
    if (readinessScore)      readinessScore[idx] = 0;
    if (networkClusterId)    networkClusterId[idx] = 0;
    if (sparklineBuffers)    memset(sparklineBuffers + idx * SPARKLINE_SAMPLES, -100, SPARKLINE_SAMPLES);
    if (sparklineIdx)        sparklineIdx[idx] = 0;
}

static void moveNetworkSidecars(uint16_t dst, uint16_t src) {
    if (dst >= MAX_SPECTRUM_NETWORKS || src >= MAX_SPECTRUM_NETWORKS || dst == src) return;
    if (beaconPrevTimestamp) beaconPrevTimestamp[dst] = beaconPrevTimestamp[src];
    if (beaconIntervalTU)    beaconIntervalTU[dst] = beaconIntervalTU[src];
    if (beaconAnomaly)       beaconAnomaly[dst] = beaconAnomaly[src];
    if (vulnTier)            vulnTier[dst] = vulnTier[src];
    if (readinessScore)      readinessScore[dst] = readinessScore[src];
    if (networkClusterId)    networkClusterId[dst] = networkClusterId[src];
    if (sparklineBuffers)    memcpy(sparklineBuffers + dst * SPARKLINE_SAMPLES,
                                    sparklineBuffers + src * SPARKLINE_SAMPLES,
                                    SPARKLINE_SAMPLES);
    if (sparklineIdx)        sparklineIdx[dst] = sparklineIdx[src];
}

static void resetMeasuredTrace() {
    for (MeasuredTraceState& trace : measuredTrace) {
        trace = {};
        trace.liveTargetX8 = TRACE_FLOOR_X8;
        trace.liveDisplayX8 = TRACE_FLOOR_X8;
        trace.averageTargetX8 = TRACE_FLOOR_X8;
        trace.averageDisplayX8 = TRACE_FLOOR_X8;
        trace.maxHoldTargetX8 = TRACE_FLOOR_X8;
        trace.maxHoldDisplayX8 = TRACE_FLOOR_X8;
    }
    measuredTraceLastMs = 0u;
    analyzerSweepStartedMs = 0u;
    analyzerSweepLastCompletedMs = 0u;
    analyzerSweepPeriodMs = ANALYZER_SWEEP_DEFAULT_MS;
    analyzerSweepPhase = 0u;
    spectrumRevealPrevX = -1;  // measured traces reset -> full carrier repaint
    waterfallLastFrameMs = 0u;
    if (waterfallTargetRow) {
        memset(waterfallTargetRow, 0, SPECTRUM_WIDTH);
    }
    if (waterfallDisplayX8) {
        memset(waterfallDisplayX8, 0,
               sizeof(int16_t) * SPECTRUM_WIDTH);
    }
}

static void publishMeasuredTrace(
        const RfMeasurement::SweepSnapshot& sweep,
        uint32_t nowMs) {
    const uint32_t completedMs =
        sweep.completedAtMs != 0u ? sweep.completedAtMs : nowMs;
    if (analyzerSweepLastCompletedMs == 0u) {
        uint32_t measuredDurationMs = 0u;
        for (uint8_t channel = 1u; channel <= 13u; ++channel) {
            measuredDurationMs += sweep.channels[channel].dwellMs;
        }
        analyzerSweepPeriodMs = static_cast<uint16_t>(constrain(
            measuredDurationMs, static_cast<uint32_t>(ANALYZER_SWEEP_MIN_MS),
            static_cast<uint32_t>(ANALYZER_SWEEP_MAX_MS)));
    } else {
        analyzerSweepPeriodMs = SpectrumThruMath::smoothSweepPeriod(
            analyzerSweepPeriodMs, analyzerSweepLastCompletedMs, completedMs,
            ANALYZER_SWEEP_MIN_MS, ANALYZER_SWEEP_MAX_MS);
    }
    analyzerSweepLastCompletedMs = completedMs;
    analyzerSweepStartedMs = nowMs;
    analyzerSweepPhase = 0u;

    for (uint8_t channel = 1u; channel <= 13u; ++channel) {
        const RfMeasurement::ChannelEvidence& evidence =
            sweep.channels[channel];
        MeasuredTraceState& trace = measuredTrace[channel];
        trace.covered = evidence.dwellMs != 0u;
        trace.spreadDb = evidence.rssiSpread;
        if (!trace.covered || evidence.sampleCount == 0u) {
            trace.liveTargetAlpha = 0u;
            trace.liveTargetX8 = TRACE_FLOOR_X8;
            continue;
        }

        const int16_t liveX8 = static_cast<int16_t>(
            constrain((int)evidence.trimmedMeanRssi, -100, -20) * 8);
        const int16_t peakX8 = static_cast<int16_t>(
            constrain((int)evidence.peakRssi, -100, -20) * 8);
        trace.liveTargetX8 = liveX8;
        trace.liveTargetAlpha = 255u;

        if (!trace.averageValid) {
            trace.averageTargetX8 = liveX8;
            trace.averageDisplayX8 = TRACE_FLOOR_X8;
            trace.averageValid = true;
        } else {
            trace.averageTargetX8 = static_cast<int16_t>(
                ((int32_t)trace.averageTargetX8 * 3 + liveX8) / 4);
        }

        if (!trace.maxHoldValid || peakX8 > trace.maxHoldTargetX8) {
            trace.maxHoldTargetX8 = peakX8;
        }
        trace.maxHoldValid = true;
    }
}

static void updateMeasuredTraceAnimation(uint32_t nowMs) {
    const uint32_t elapsed = measuredTraceLastMs == 0u
        ? 16u
        : min(nowMs - measuredTraceLastMs, 250u);
    measuredTraceLastMs = nowMs;

    if (analyzerSweepStartedMs == 0u) {
        analyzerSweepStartedMs = nowMs;
    }
    analyzerSweepPhase = SpectrumThruMath::analyzerSweepPhase(
        nowMs, analyzerSweepStartedMs, analyzerSweepPeriodMs);
    const uint16_t sweepColumn = SpectrumThruMath::analyzerSweepColumn(
        analyzerSweepPhase, SPECTRUM_WIDTH);

    for (uint8_t channel = 1u; channel <= 13u; ++channel) {
        // Playback a completed sweep from left to right over the real sweep
        // cadence. Channels ahead of the head retain the prior display state;
        // target amplitude still changes only from completed RF evidence.
        MeasuredTraceState& trace = measuredTrace[channel];
        if (sweepColumn < trace.sweepColumn) continue;
        trace.liveDisplayX8 = SpectrumThruMath::approachTrace(
            trace.liveDisplayX8, trace.liveTargetX8, elapsed);
        trace.averageDisplayX8 = SpectrumThruMath::approachTrace(
            trace.averageDisplayX8, trace.averageTargetX8, elapsed, 260u);
        trace.maxHoldDisplayX8 = SpectrumThruMath::approachTrace(
            trace.maxHoldDisplayX8, trace.maxHoldTargetX8, elapsed, 220u);
        trace.liveDisplayAlpha = static_cast<uint8_t>(
            SpectrumThruMath::approachTrace(
                trace.liveDisplayAlpha, trace.liveTargetAlpha,
                elapsed, 180u));
    }

    if (!measuredSweepValid || !waterfallBuffer ||
        !waterfallTargetRow || !waterfallDisplayX8) {
        return;
    }

    for (uint16_t x = 0u; x <= sweepColumn; ++x) {
        waterfallDisplayX8[x] = SpectrumThruMath::approachTrace(
            waterfallDisplayX8[x],
            static_cast<int16_t>(waterfallTargetRow[x] * 8u),
            elapsed, WATERFALL_RESPONSE_MS);
    }

    if (nowMs - waterfallLastFrameMs < WATERFALL_FRAME_MS) return;
    waterfallLastFrameMs = nowMs;
    // Store the raw VBW-filtered intensity per cell. The ordered density
    // tier is chosen at render time (drawWaterfall) so a busy channel keeps
    // its full intensity here and reads as a denser sinking row on screen.
    uint8_t* row =
        waterfallBuffer + waterfallWriteRow * SPECTRUM_WIDTH;
    for (uint16_t x = 0u; x < SPECTRUM_WIDTH; ++x) {
        row[x] = static_cast<uint8_t>(
            constrain(waterfallDisplayX8[x] / 8, 0, 255));
    }
    waterfallWriteRow = static_cast<uint8_t>(
        (waterfallWriteRow + 1u) % WATERFALL_ROWS);
}

// ==[ INTELLIGENCE: ROGUE AP DETECTION ]==
static RogueAlert* rogueAlerts = nullptr;  // PSRAM — saves ~400 bytes DRAM
static uint8_t rogueAlertCount = 0;

// ==[ FWD DECLS ]==
static bool initPromiscuous();
static void stopPromiscuous();
#if defined(HAMLET_CORE3SE)
static void serviceFtmRange(uint32_t nowMs);
static void ftmEventHandler(void*, esp_event_base_t, int32_t, void*);
#endif
static void handleBeacon(const uint8_t* payload, uint16_t len, int8_t rssi,
                         uint8_t sigMode, uint8_t phyRate, uint8_t cwb,
                         uint32_t rxTimestamp, uint32_t observedMs);
static void handleProbeResponse(const uint8_t* payload, uint16_t len,
                                int8_t rssi, uint32_t observedMs);
static void handleProbeRequest(const uint8_t* payload, uint16_t len, int8_t rssi);
static void handleDataFrame(const uint8_t* payload, uint16_t len, int8_t rssi,
                            int8_t noiseFloor, uint32_t rxTimestampUs,
                            uint32_t observedMs);
static void handleDeauthFrame(const uint8_t* payload, uint16_t len, int8_t rssi, uint8_t rxChannel);
static void hopChannel();
static void cleanupStale();
static SpectrumNetwork* findOrCreateNetwork(const uint8_t* bssid);
static void updateChannelStats(uint8_t channel, int8_t rssi);
static void sortNetworksByRSSI();
static void updateParanoidSwine();
static void updateDialChannel();
static void drawGraphHeader(M5Canvas& canvas, uint16_t fg, uint16_t bg);
static void cacheFrameConstants();
static void stageWaterfallTarget(
    const RfMeasurement::SweepSnapshot& sweep);
static void drainPacketEvents();
static void discardPacketEvents();
static void updateMeasuredTraceAnimation(uint32_t nowMs);
static void drawNoiseFloor(M5Canvas& canvas, uint16_t fg);
static void drawWaterfall(M5Canvas& canvas, uint16_t fg, uint16_t bg);
// ==[ INTELLIGENCE FWD DECLS ]==
static void processProbeEntry();
static void classifyNetworks();
static void detectRogueAPs();
static void buildBSSIDClusters();
static void updateBeaconAnomalies();
static void updateSparklines();
static void drawSparkline(M5Canvas& canvas, int x, int y, uint16_t netIdx, uint16_t fg, uint16_t bg);
static void drawRogueAlertToast(M5Canvas& canvas, uint16_t fg, uint16_t bg);
static char getTierIcon(uint8_t tier);
static bool isRogueSSID(uint16_t netIdx);

// ==[ PROMISCUOUS CALLBACK ]== IRAM_ATTR dropped to save IRAM; Hunt gets priority
static void promiscuousCallback(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (!active.load(std::memory_order_relaxed) ||
        paused.load(std::memory_order_acquire) || !buf) {
        return;
    }
    
    // ==[ DIAL MODE: PPS COUNTER ]== count every packet before filtering
    ppsCounter.fetch_add(1u, std::memory_order_relaxed);
    
    (void)type;
    const wifi_promiscuous_pkt_t* pkt =
        static_cast<const wifi_promiscuous_pkt_t*>(buf);
    const uint8_t* payload = pkt->payload;
    const uint16_t len = pkt->rx_ctrl.sig_len;
    if (!payload || len == 0u) return;

    SpectrumPacketEvent event{};
    event.observedMs = millis();
    event.rxTimestampUs = pkt->rx_ctrl.timestamp;
    event.frameLength = len;
    event.payloadLength =
        len < RF_EVENT_PAYLOAD_BYTES ? len : RF_EVENT_PAYLOAD_BYTES;
    event.rssi = pkt->rx_ctrl.rssi;
    event.noiseFloor = pkt->rx_ctrl.noise_floor;
    // RX metadata channel is per-packet and more reliable than our hop bookkeeping.
    event.channel = pkt->rx_ctrl.channel;
    if (event.channel < 1u || event.channel > 13u) {
        event.channel = currentChannel.load(std::memory_order_relaxed);
    }
    // ==[ PHY METADATA ]== extract for device fingerprinting + beacon interval
    event.sigMode = pkt->rx_ctrl.sig_mode;
    event.phyRate = pkt->rx_ctrl.rate;
    event.channelWidth = pkt->rx_ctrl.cwb;
    event.frameType = payload[0] & 0x0Cu;
    event.frameSubtype = (payload[0] >> 4u) & 0x0Fu;
    memcpy(event.payload, payload, event.payloadLength);
    packetEvents.push(event);
}

static uint32_t firmwareRevisionWord() {
    uint32_t hash = 2166136261u;
    for (const char* p = BUILD_COMMIT; p && *p; ++p) {
        hash = (hash ^ static_cast<uint8_t>(*p)) * 16777619u;
    }
    return hash;
}

static void populateTraceContext(RfTrace::Record& record,
                                 uint32_t rxTimestampUs) {
#if defined(HAMLET_CORE3SE)
    Pedometer::RfPoseSample pose{};
    if (Pedometer::getRfPoseAt(rxTimestampUs, pose)) {
        record.poseAgeUs = pose.interpolationAgeUs;
        record.yawDegX10 = static_cast<int16_t>(constrain(
            static_cast<int>(pose.screenYawDeg * 10.0f), -1800, 3599));
        record.pitchDegX10 = static_cast<int16_t>(constrain(
            static_cast<int>(pose.pitchDeg * 10.0f), -900, 900));
        record.rollDegX10 = static_cast<int16_t>(constrain(
            static_cast<int>(pose.rollDeg * 10.0f), -1800, 1800));
        record.yawDriftDegX100 = static_cast<uint16_t>(constrain(
            static_cast<int>(pose.yawDriftDeg * 100.0f), 0, 18000));
        if (pose.flags & Pedometer::RF_POSE_CALIBRATED) {
            record.poseFlags |= RfTrace::POSE_CALIBRATED;
        }
        if (pose.flags & Pedometer::RF_POSE_STATIONARY) {
            record.poseFlags |= RfTrace::POSE_STATIONARY;
        }
        if (pose.flags & Pedometer::RF_POSE_ROTATING) {
            record.poseFlags |= RfTrace::POSE_ROTATING;
        }
        if (pose.flags & Pedometer::RF_POSE_TRANSLATING) {
            record.poseFlags |= RfTrace::POSE_TRANSLATING;
        }
        if (pose.flags & Pedometer::RF_POSE_INTERPOLATED) {
            record.poseFlags |= RfTrace::POSE_INTERPOLATED;
        }
    }
#else
    (void)rxTimestampUs;
#endif

    if (GPS::hasFix()) {
        const uint32_t ageMs = GPS::getFixAgeMs();
        const float hdop = GPS::getHdop();
        if (ageMs <= RfRoute::kMaxFixAgeMs &&
            hdop > 0.0f && hdop <= RfRoute::kMaxHdop) {
            record.latitudeE7 =
                RfRoute::degreesToE7(GPS::getLatitude());
            record.longitudeE7 =
                RfRoute::degreesToE7(GPS::getLongitude());
            record.gpsAgeMs = static_cast<uint16_t>(
                ageMs > UINT16_MAX ? UINT16_MAX : ageMs);
            record.hdopX100 = static_cast<uint16_t>(constrain(
                static_cast<int>(hdop * 100.0f + 0.5f), 0, 65535));
            record.speedCms = static_cast<uint16_t>(constrain(
                static_cast<int>(
                    GPS::getSpeedKmh() * 27.7778f + 0.5f),
                0, 65535));
            record.gpsFixQuality = GPS::getSatCount();
        }
    }
}

static void tracePacketEvent(
    const SpectrumPacketEvent& event,
    RfMeasurement::FrameClass frameClass) {
    if (!rfTrace) return;
    if (lastTracePacketMs != 0u &&
        event.observedMs - lastTracePacketMs < 100u) {
        return;
    }
    lastTracePacketMs =
        event.observedMs == 0u ? 1u : event.observedMs;

    RfTrace::Record record{};
    record.firmwareRevision = firmwareRevisionWord();
    record.rxTimestampUs = event.rxTimestampUs;
    record.observationMs = event.observedMs;
    record.queueDrops = packetEvents.drops();
    record.channelDwellUs =
        (event.observedMs - lastHopTime) * 1000u;
    record.frameBytes = event.frameLength;
    record.rssi = event.rssi;
    record.noiseFloor = event.noiseFloor;
#if defined(HAMLET_CORE3SE)
    record.board =
        static_cast<uint8_t>(RfTrace::Board::CORES3SE);
#else
    record.board = static_cast<uint8_t>(RfTrace::Board::CORE2);
#endif
    record.channel = event.channel;
    record.frameClass = static_cast<uint8_t>(frameClass);
    record.phyMode = event.sigMode;
    record.channelWidth = event.channelWidth;

    if (event.payloadLength >= 24u) {
        if (event.frameType == 0x08u) {
            const SpectrumThruMath::DataFrameRoute route =
                SpectrumThruMath::routeDataFrame(event.payload[1]);
            if (route.valid) {
                memcpy(record.identity,
                       event.payload + route.bssidOffset, 6u);
                memcpy(record.peer,
                       event.payload + route.clientOffset, 6u);
            }
        } else {
            memcpy(record.identity, event.payload + 16u, 6u);
            memcpy(record.peer, event.payload + 10u, 6u);
        }
    } else if (event.payloadLength >= 10u) {
        memcpy(record.identity, event.payload + 4u, 6u);
    }

    populateTraceContext(record, event.rxTimestampUs);

    rfTrace->push(record);
}

static void traceCsiSnapshot(const CsiTracker::Snapshot& csi) {
    if (!rfTrace || !csi.valid) return;
    RfTrace::Record record{};
    record.firmwareRevision = firmwareRevisionWord();
    record.rxTimestampUs = csi.rxTimestampUs;
    record.observationMs = csi.lastSeenMs;
    record.queueDrops = packetEvents.drops();
    record.csiOriginalLength = csi.originalLength;
    record.csiRetainedLength = csi.retainedLength;
    record.rssi = csi.rssi;
    record.noiseFloor = csi.noiseFloor;
#if defined(HAMLET_CORE3SE)
    record.board =
        static_cast<uint8_t>(RfTrace::Board::CORES3SE);
#else
    record.board = static_cast<uint8_t>(RfTrace::Board::CORE2);
#endif
    memcpy(record.identity, csi.mac, 6u);
    record.channel = csi.channel;
    record.frameClass =
        static_cast<uint8_t>(RfMeasurement::FrameClass::OTHER);
    record.phyMode = csi.signalMode;
    record.channelWidth = csi.channelBandwidth;
    record.secondaryChannel = csi.secondaryChannel;
    record.csiLayout = csi.ltfMask;
    populateTraceContext(record, csi.rxTimestampUs);
    rfTrace->push(record);
}

static void drainPacketEvents() {
    SpectrumPacketEvent event{};
    uint16_t drained = 0u;
    while (drained < RF_EVENT_DRAIN_BUDGET && packetEvents.pop(event)) {
        ++drained;

        RfMeasurement::FrameClass frameClass =
            RfMeasurement::FrameClass::OTHER;
        if (event.frameType == 0x00u) {
            frameClass = RfMeasurement::FrameClass::MANAGEMENT;
        } else if (event.frameType == 0x04u) {
            frameClass = RfMeasurement::FrameClass::CONTROL;
        } else if (event.frameType == 0x08u) {
            frameClass = RfMeasurement::FrameClass::DATA;
        }
        tracePacketEvent(event, frameClass);
        RfMeasurement::PacketObservation observation{};
        observation.rxTimestampUs = event.rxTimestampUs;
        observation.bytes = event.frameLength;
        observation.rssi = event.rssi;
        observation.noiseFloor = event.noiseFloor;
        observation.channel = event.channel;
        observation.frameClass = frameClass;
        if (rfMeasurements) rfMeasurements->observe(observation);
        updateChannelStats(event.channel, event.rssi);

        // Control frames and short/truncated headers are still valid channel
        // measurements, but do not carry enough bytes for higher-level
        // network/client parsing.
        if (event.payloadLength < 24u) continue;

        if (event.frameType == 0x00u) {
            if (event.frameSubtype == 0x08u) {
                handleBeacon(event.payload, event.payloadLength, event.rssi,
                             event.sigMode, event.phyRate,
                             event.channelWidth, event.rxTimestampUs,
                             event.observedMs);
            } else if (event.frameSubtype == 0x05u) {
                handleProbeResponse(event.payload, event.payloadLength,
                                    event.rssi, event.observedMs);
            } else if (event.frameSubtype == 0x04u) {
                handleProbeRequest(event.payload, event.payloadLength,
                                   event.rssi);
            } else if (event.frameSubtype == 0x0Cu ||
                       event.frameSubtype == 0x0Au) {
                handleDeauthFrame(event.payload, event.payloadLength,
                                  event.rssi, event.channel);
            }
        } else if (event.frameType == 0x08u) {
            handleDataFrame(event.payload, event.payloadLength, event.rssi,
                            event.noiseFloor, event.rxTimestampUs,
                            event.observedMs);
        }
    }

    const uint32_t drops = packetEvents.drops();
    if (drops != lastReportedPacketDrops) {
        HAMLET_LOGF(
            "[SPECTRUM] RF queue drops=%lu depth=%lu capacity=%u\n",
            (unsigned long)drops, (unsigned long)packetEvents.size(),
            (unsigned)RF_EVENT_RING_CAPACITY);
        lastReportedPacketDrops = drops;
    }
}

static void discardPacketEvents() {
    SpectrumPacketEvent ignored{};
    while (packetEvents.pop(ignored)) {
    }
}

static void clearSelectedClientIdentity() {
    memset(selectedClientMac, 0, sizeof(selectedClientMac));
    selectedClientIdentityValid = false;
    selectedClientIdx = 0;
}

static void bindSelectedClientIdentity(const SpectrumClient* client) {
    if (!client) {
        clearSelectedClientIdentity();
        return;
    }
    memcpy(selectedClientMac, client->mac, sizeof(selectedClientMac));
    selectedClientIdentityValid = true;
}

static bool rebindSelectedClientIdentity() {
    const SpectrumNetwork* net = getSelectedNetwork();
    if (!net || net->clientCount == 0u) return false;

    if (selectedClientIdentityValid) {
        const int16_t rebound = SpectrumThruMath::findMacIndex(
            &net->clients[0].mac[0], net->clientCount,
            sizeof(SpectrumClient), selectedClientMac);
        if (rebound < 0) return false;
        selectedClientIdx = rebound;
        return true;
    }

    selectedClientIdx = constrain(
        selectedClientIdx, 0, (int16_t)net->clientCount - 1);
    bindSelectedClientIdentity(&net->clients[selectedClientIdx]);
    return true;
}

// ==[ PUBLIC API ]==

void start() {
    if (active) return;
    
    // Reset state
    paused.store(false, std::memory_order_release);
    packetEvents.reset();
    lastTracePacketMs = 0u;
    measuredSweepValid = false;
    lastReportedPacketDrops = 0u;
    if (!rfMeasurements) {
        void* storage = heap_caps_malloc(
            sizeof(RfMeasurement::Tracker), MALLOC_CAP_SPIRAM);
        if (storage) {
            rfMeasurements =
                new (storage) RfMeasurement::Tracker();
        }
    }
    if (!measuredSweep) {
        void* storage = heap_caps_malloc(
            sizeof(RfMeasurement::SweepSnapshot), MALLOC_CAP_SPIRAM);
        if (storage) {
            measuredSweep =
                new (storage) RfMeasurement::SweepSnapshot();
        }
    }
    if (!rfTrace) {
        void* storage = heap_caps_malloc(
            sizeof(SpectrumTraceRing), MALLOC_CAP_SPIRAM);
        if (storage) {
            rfTrace = new (storage) SpectrumTraceRing();
        }
    }
    if (rfTrace) rfTrace->reset();
#if defined(HAMLET_CORE3SE)
    if (!gpsRoute) {
        void* storage = heap_caps_malloc(
            sizeof(RfRoute::Tracker), MALLOC_CAP_SPIRAM);
        if (storage) {
            gpsRoute = new (storage) RfRoute::Tracker();
        }
    }
    if (gpsRoute) gpsRoute->reset();
#endif
    if (measuredSweep) *measuredSweep = {};
    pendingClientBeep.store(false, std::memory_order_relaxed);
    pendingSniff.store(false, std::memory_order_relaxed);
    pendingDeauthBatch.store(0u, std::memory_order_relaxed);
    pendingNewNetworkMood.store(false, std::memory_order_relaxed);
    pendingChannelRecount.store(0u, std::memory_order_relaxed);
    pendingProbe.ready.store(false, std::memory_order_relaxed);
    networkCount = 0;
#if defined(HAMLET_CORE3SE)
    portENTER_CRITICAL(&ftmMux);
    ftmRuntime = FtmRuntime{};
    portEXIT_CRITICAL(&ftmMux);
#endif
    // ==[ PSRAM ALLOC ]== networks array (saves ~12KB DRAM)
    if (!networks) {
        networks = (SpectrumNetwork*)heap_caps_calloc(
            MAX_SPECTRUM_NETWORKS, sizeof(SpectrumNetwork), MALLOC_CAP_SPIRAM);
    } else {
        memset(networks, 0, sizeof(SpectrumNetwork) * MAX_SPECTRUM_NETWORKS);
    }
    if (!specHashTable) {
        specHashTable = (uint16_t*)heap_caps_malloc(
            sizeof(uint16_t) * SPEC_HASH_SIZE, MALLOC_CAP_SPIRAM);
    }
    if (specHashTable) memset(specHashTable, 0xFF, sizeof(uint16_t) * SPEC_HASH_SIZE);
    selectedIdx = 0;
    selectedClientIdx = 0;
    memset(selectedClientMac, 0, sizeof(selectedClientMac));
    selectedClientIdentityValid = false;
    clientMode = false;
    clientDetailActive = false;
    monitoredNetworkWasStale = false;
    c5CarrierDetailActive = false;
    c5ArsenalSelectedIdx = 0;
    lockedChannel = 0;
    dialMode = false;
    dialLocked = false;
    dialChannel = 1;
    dialPositionTarget = 1.0f;
    dialPositionSmooth = 1.0f;
    ppsCounter.store(0u, std::memory_order_relaxed);
    displayPps = 0u;
    detailToastVisible = false;
    detailToastExpiry = 0u;
    lastAccelMag = 1.0f;
    lastShakeTime = 0u;
    resetClientBearingState(nullptr);
    resetC5CarrierBearingState(nullptr);
    currentChannel = 1;
    channelIndex = 0;
    historyPos = 0;
    memset((void*)channelPeakRSSI, -100, sizeof(channelPeakRSSI));   // safe: callback not active yet
    memset(channelNetCount, 0, sizeof(channelNetCount));
    memset((void*)channelAvgRSSI, -100, sizeof(channelAvgRSSI));
    memset(channelAttackableCount, 0, sizeof(channelAttackableCount));
    for (uint8_t ch = 0; ch < 14; ch++) {
        channelActivity[ch] = 0;
        channelTimeMs[ch] = 0;
    }
    reconLastUpdateMs = 0;

    // ==[ PSRAM ALLOCATIONS ]== alloc on start, free on stop
    if (!rssiHistory) {
        rssiHistory = (int8_t*)heap_caps_malloc(14 * RSSI_HISTORY_LEN, MALLOC_CAP_SPIRAM);
    }
    if (rssiHistory) memset(rssiHistory, -100, 14 * RSSI_HISTORY_LEN);

    if (!spectrumBuffer) {
        spectrumBuffer  = (int8_t*)heap_caps_malloc(SPECTRUM_WIDTH, MALLOC_CAP_SPIRAM);
    }
    if (!waterfallBuffer) {
        waterfallBuffer = (uint8_t*)heap_caps_malloc(WATERFALL_ROWS * SPECTRUM_WIDTH, MALLOC_CAP_SPIRAM);
    }
    if (!waterfallTargetRow) {
        waterfallTargetRow = (uint8_t*)heap_caps_malloc(
            SPECTRUM_WIDTH, MALLOC_CAP_SPIRAM);
    }
    if (!waterfallDisplayX8) {
        waterfallDisplayX8 = (int16_t*)heap_caps_malloc(
            sizeof(int16_t) * SPECTRUM_WIDTH, MALLOC_CAP_SPIRAM);
    }
    if (!spectrumBuffer) {
        HAMLET_LOGLN("[SPEC] C5 snapshot buffer alloc failed");
    }
    if (!waterfallBuffer || !waterfallTargetRow || !waterfallDisplayX8) {
        HAMLET_LOGLN("[SPEC] waterfall buffer alloc failed");
    }
    // ==[ INTELLIGENCE PSRAM ALLOCATIONS ]==
    if (!probeTable) {
        probeTable = (SpectrumProbeEntry*)heap_caps_malloc(
            sizeof(SpectrumProbeEntry) * MAX_SPECTRUM_PROBES, MALLOC_CAP_SPIRAM);
    }
    if (probeTable) memset(probeTable, 0, sizeof(SpectrumProbeEntry) * MAX_SPECTRUM_PROBES);
    probeCount = 0;
    pendingProbe.ready.store(false, std::memory_order_relaxed);

    if (!beaconPrevTimestamp || !beaconIntervalTU || !beaconAnomaly) {
        if (beaconPrevTimestamp) { heap_caps_free(beaconPrevTimestamp); beaconPrevTimestamp = nullptr; }
        if (beaconIntervalTU) { heap_caps_free(beaconIntervalTU); beaconIntervalTU = nullptr; }
        if (beaconAnomaly) { heap_caps_free(beaconAnomaly); beaconAnomaly = nullptr; }
        beaconPrevTimestamp = (uint32_t*)heap_caps_malloc(MAX_SPECTRUM_NETWORKS * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
        beaconIntervalTU   = (uint16_t*)heap_caps_malloc(MAX_SPECTRUM_NETWORKS * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
        beaconAnomaly      = (uint8_t*) heap_caps_malloc(MAX_SPECTRUM_NETWORKS, MALLOC_CAP_SPIRAM);
        if (!beaconPrevTimestamp || !beaconIntervalTU || !beaconAnomaly) {
            if (beaconPrevTimestamp) { heap_caps_free(beaconPrevTimestamp); beaconPrevTimestamp = nullptr; }
            if (beaconIntervalTU) { heap_caps_free(beaconIntervalTU); beaconIntervalTU = nullptr; }
            if (beaconAnomaly) { heap_caps_free(beaconAnomaly); beaconAnomaly = nullptr; }
        }
    }
    if (beaconPrevTimestamp) memset(beaconPrevTimestamp, 0, MAX_SPECTRUM_NETWORKS * sizeof(uint32_t));
    if (beaconIntervalTU)   memset(beaconIntervalTU, 0, MAX_SPECTRUM_NETWORKS * sizeof(uint16_t));
    if (beaconAnomaly)      memset(beaconAnomaly, 0, MAX_SPECTRUM_NETWORKS);

    if (!vulnTier || !readinessScore) {
        if (vulnTier) { heap_caps_free(vulnTier); vulnTier = nullptr; }
        if (readinessScore) { heap_caps_free(readinessScore); readinessScore = nullptr; }
        vulnTier       = (uint8_t*)heap_caps_malloc(MAX_SPECTRUM_NETWORKS, MALLOC_CAP_SPIRAM);
        readinessScore = (uint8_t*)heap_caps_malloc(MAX_SPECTRUM_NETWORKS, MALLOC_CAP_SPIRAM);
        if (!vulnTier || !readinessScore) {
            if (vulnTier) { heap_caps_free(vulnTier); vulnTier = nullptr; }
            if (readinessScore) { heap_caps_free(readinessScore); readinessScore = nullptr; }
        }
    }
    if (vulnTier)       memset(vulnTier, 7, MAX_SPECTRUM_NETWORKS);  // default T7 (unknown)
    if (readinessScore) memset(readinessScore, 0, MAX_SPECTRUM_NETWORKS);

    if (!clusters || !networkClusterId) {
        if (clusters) { heap_caps_free(clusters); clusters = nullptr; }
        if (networkClusterId) { heap_caps_free(networkClusterId); networkClusterId = nullptr; }
        clusters         = (BSSIDCluster*)heap_caps_malloc(sizeof(BSSIDCluster) * MAX_BSSID_CLUSTERS, MALLOC_CAP_SPIRAM);
        networkClusterId = (uint8_t*)heap_caps_malloc(MAX_SPECTRUM_NETWORKS, MALLOC_CAP_SPIRAM);
        if (!clusters || !networkClusterId) {
            if (clusters) { heap_caps_free(clusters); clusters = nullptr; }
            if (networkClusterId) { heap_caps_free(networkClusterId); networkClusterId = nullptr; }
        }
    }
    clusterCount = 0;
    if (networkClusterId) memset(networkClusterId, 0, MAX_SPECTRUM_NETWORKS);

    resetMeasuredTrace();

    prevNetworkCount = 0;
    lastNewNetworkTime = 0;

    if (!sparklineBuffers || !sparklineIdx) {
        if (sparklineBuffers) { heap_caps_free(sparklineBuffers); sparklineBuffers = nullptr; }
        if (sparklineIdx) { heap_caps_free(sparklineIdx); sparklineIdx = nullptr; }
        sparklineBuffers = (int8_t*) heap_caps_malloc(MAX_SPECTRUM_NETWORKS * SPARKLINE_SAMPLES, MALLOC_CAP_SPIRAM);
        sparklineIdx     = (uint8_t*)heap_caps_malloc(MAX_SPECTRUM_NETWORKS, MALLOC_CAP_SPIRAM);
        if (!sparklineBuffers || !sparklineIdx) {
            if (sparklineBuffers) { heap_caps_free(sparklineBuffers); sparklineBuffers = nullptr; }
            if (sparklineIdx) { heap_caps_free(sparklineIdx); sparklineIdx = nullptr; }
        }
    }
    if (sparklineBuffers) memset(sparklineBuffers, -100, MAX_SPECTRUM_NETWORKS * SPARKLINE_SAMPLES);
    if (sparklineIdx)     memset(sparklineIdx, 0, MAX_SPECTRUM_NETWORKS);
    lastSparklineUpdate = 0;

    if (!rogueAlerts) {
        rogueAlerts = (RogueAlert*)heap_caps_malloc(sizeof(RogueAlert) * MAX_ROGUE_ALERTS, MALLOC_CAP_SPIRAM);
    }
    rogueAlertCount = 0;

    // Reset Paranoid Swine state
    memset(&paranoid, 0, sizeof(paranoid));
    memset(paranoid.rssiHistory, -100, sizeof(paranoid.rssiHistory));
    paranoid.rssiPeak = -100;
    paranoid.rssiCurrent = -100;
    M5.Power.setLed(false);  // Ensure LED off


    startTime = millis();
    lastHopTime = startTime;
    lastCleanup = startTime;
    lastHistoryUpdate = startTime;
    lastChannelTimeUpdate = startTime;
    lastTimedChannel = currentChannel;
    lastDialUpdate = startTime;
    lastPpsUpdate = startTime;
    if (!rfMeasurements || !measuredSweep) {
        HAMLET_LOGLN("[SPEC] RF measurement PSRAM alloc failed");
        active = false;
        return;
    }
    rfMeasurements->reset(startTime, 1u);
    currentHopIntervalMs =
        RfMeasurement::dwellDurationMs(1u, rfMeasurements->epoch());

    // Initialize spectrum analyzer buffers (v2 visuals)
    if (spectrumBuffer)  memset(spectrumBuffer, -95, SPECTRUM_WIDTH);
    if (waterfallBuffer) memset(waterfallBuffer, 0, WATERFALL_ROWS * SPECTRUM_WIDTH);
    if (waterfallTargetRow) memset(waterfallTargetRow, 0, SPECTRUM_WIDTH);
    if (waterfallDisplayX8) {
        memset(waterfallDisplayX8, 0,
               sizeof(int16_t) * SPECTRUM_WIDTH);
    }
    c5SnapshotStripDirty = true;
    waterfallWriteRow = 0;
    spectrumRevealPrevX = -1;  // force a full carrier-plot repaint on entry
    analyzerSweepStartedMs = startTime;
    waterfallLastFrameMs = startTime;
    
    // bail if critical PSRAM allocs failed — promiscuous callback would null-deref
    if (!networks || !specHashTable) {
        HAMLET_LOGLN("[SPEC] PSRAM alloc failed — spectrum disabled");
        active = false;
        return;
    }

    if (!initPromiscuous()) {
        active = false;
        paused = false;
        return;
    }

    active = true;
}

void stop() {
    if (!active) return;
    Pedometer::setRfPoseActive(false);

    const C5Monster::Operation c5Operation = C5Monster::getActiveOperation();
    if (c5Operation == C5Monster::Operation::PACKET_MONITOR ||
        c5Operation == C5Monster::Operation::CHANNEL_VIEW ||
        c5Operation == C5Monster::Operation::DEAUTH_DETECTOR) {
        C5Monster::emergencyStop();
    }
    c5CarrierDetailActive = false;
    resetC5CarrierBearingState(nullptr);
    
    stopPromiscuous();
    packetEvents.reset();
    pendingClientBeep.store(false, std::memory_order_relaxed);
    pendingSniff.store(false, std::memory_order_relaxed);
    pendingDeauthBatch.store(0u, std::memory_order_relaxed);
    pendingNewNetworkMood.store(false, std::memory_order_relaxed);
    pendingChannelRecount.store(0u, std::memory_order_relaxed);
    pendingProbe.ready.store(false, std::memory_order_relaxed);
    NowFlock::markEspNowNeedsReinit();
    
    // Reset Paranoid Swine
    paranoid.attackActive = false;
    M5.Power.setLed(false);
    

    // ==[ FREE PSRAM ]== reclaim buffers for other modes
    if (networks)       { heap_caps_free(networks);       networks = nullptr; }
    networkCount = 0;
    if (specHashTable)  { heap_caps_free(specHashTable);  specHashTable = nullptr; }
    if (rssiHistory)    { heap_caps_free(rssiHistory);    rssiHistory = nullptr; }
    if (spectrumBuffer) { heap_caps_free(spectrumBuffer); spectrumBuffer = nullptr; }
    if (waterfallBuffer){ heap_caps_free(waterfallBuffer); waterfallBuffer = nullptr; }
    if (waterfallTargetRow) {
        heap_caps_free(waterfallTargetRow);
        waterfallTargetRow = nullptr;
    }
    if (waterfallDisplayX8) {
        heap_caps_free(waterfallDisplayX8);
        waterfallDisplayX8 = nullptr;
    }
    // ==[ FREE INTELLIGENCE PSRAM ]==
    if (probeTable)         { heap_caps_free(probeTable);         probeTable = nullptr; }
    if (beaconPrevTimestamp) { heap_caps_free(beaconPrevTimestamp); beaconPrevTimestamp = nullptr; }
    if (beaconIntervalTU)   { heap_caps_free(beaconIntervalTU);   beaconIntervalTU = nullptr; }
    if (beaconAnomaly)      { heap_caps_free(beaconAnomaly);      beaconAnomaly = nullptr; }
    if (vulnTier)           { heap_caps_free(vulnTier);           vulnTier = nullptr; }
    if (readinessScore)     { heap_caps_free(readinessScore);     readinessScore = nullptr; }
    if (clusters)           { heap_caps_free(clusters);           clusters = nullptr; }
    if (networkClusterId)   { heap_caps_free(networkClusterId);   networkClusterId = nullptr; }
    if (sparklineBuffers)   { heap_caps_free(sparklineBuffers);   sparklineBuffers = nullptr; }
    if (sparklineIdx)       { heap_caps_free(sparklineIdx);       sparklineIdx = nullptr; }
    if (rogueAlerts)        { heap_caps_free(rogueAlerts);        rogueAlerts = nullptr; }
    if (rfMeasurements) {
        rfMeasurements->~Tracker();
        heap_caps_free(rfMeasurements);
        rfMeasurements = nullptr;
    }
    if (measuredSweep) {
        measuredSweep->~SweepSnapshot();
        heap_caps_free(measuredSweep);
        measuredSweep = nullptr;
    }
    if (rfTrace) {
        rfTrace->~SpectrumTraceRing();
        heap_caps_free(rfTrace);
        rfTrace = nullptr;
    }
#if defined(HAMLET_CORE3SE)
    if (gpsRoute) {
        gpsRoute->~Tracker();
        heap_caps_free(gpsRoute);
        gpsRoute = nullptr;
    }
#endif
    probeCount = 0;
    clusterCount = 0;
    rogueAlertCount = 0;

    resetMeasuredTrace();

    // clear channel summary arrays — prevents stale data leaking to DEFHOG4 RF_POSTURE
    memset((void*)channelPeakRSSI, 0x9C, sizeof(channelPeakRSSI));  // -100, safe: callback stopped
    memset(channelNetCount, 0, sizeof(channelNetCount));
    memset((void*)channelAvgRSSI, 0x9C, sizeof(channelAvgRSSI));   // -100

    paused.store(false, std::memory_order_release);
    active = false;
    clientMode = false;
    clientDetailActive = false;
    selectedClientIdentityValid = false;
    memset(selectedClientMac, 0, sizeof(selectedClientMac));
    lockedChannel = 0;
    resetClientBearingState(nullptr);
}

// ==[ HIDDEN SSID PROBING ]== one probe per call, 25ms spacing
static void probeHiddenNetworks() {
    uint32_t now = millis();
    if (now - lastProbeTime < PROBE_SPACING_MS) return;

    for (uint16_t i = 0; i < networkCount; i++) {
        if (networks[i].isHidden && !networks[i].wasRevealed &&
            networks[i].channel == currentChannel &&
            networks[i].probeAttempts < MAX_PROBE_ATTEMPTS) {
            WSLBypasser::sendProbeRequest(networks[i].bssid);
            networks[i].probeAttempts++;
            lastProbeTime = now;
            return;  // one per call, don't blast
        }
    }
}

void update() {
    if (!active) return;
    Pedometer::setRfPoseActive(
        clientDetailActive || c5CarrierDetailActive);
    
    uint32_t now = millis();

#if defined(HAMLET_CORE3SE)
    serviceFtmRange(now);
#endif

    if (paused.load(std::memory_order_acquire)) {
        // No callback samples are accepted while paused. Freezing the hop and
        // sweep clocks prevents that absence from being published as measured
        // quiet airtime.
        return;
    }

    // Consume callback publications before any state iteration. New arrivals
    // can continue filling the ring while the rest of this update runs.
    drainPacketEvents();
    // Handlers stamp records from callback observation time. Refresh the frame
    // clock anyway so no post-drain state can appear to come from the future.
    now = millis();

    // ==[ RECON TIME ]== accumulate dwell time for the last channel
    if (lastTimedChannel >= 1 && lastTimedChannel <= 13) {
        uint32_t delta = now - lastChannelTimeUpdate;
        channelTimeMs[lastTimedChannel] += delta;
    }
    lastChannelTimeUpdate = now;
    lastTimedChannel = currentChannel;
    reconLastUpdateMs = now;
    
    // ==[ DEFERRED CLIENT BEEP ]== callback-safe
    if (pendingClientBeep) {
        pendingClientBeep = false;
        SFX::play(SFX::CLIENT_NEW);  // Short high beep for new client
    }

    // ==[ DEFERRED SNIFF ]== Avatar::sniff() unsafe in promiscuous callback
    if (pendingSniff) {
        pendingSniff = false;
        Avatar::sniff();
    }

    // ==[ DEFERRED NEW NETWORK MOOD ]== callback-safe
    if (pendingNewNetworkMood) {
        pendingNewNetworkMood = false;
        Mood::onNewNetwork();
    }

    // ==[ DEFERRED CHANNEL RECOUNT ]== moved from callback to avoid O(n) in promisc context
    uint8_t rechannelIdx = pendingChannelRecount;
    if (rechannelIdx >= 1 && rechannelIdx <= 13) {
        pendingChannelRecount = 0;
        channelNetCount[rechannelIdx] = 0;
        channelAttackableCount[rechannelIdx] = 0;
        bool saeOn = Config::getSAEAttackEnabled();
        for (uint16_t i = 0; i < networkCount; i++) {
            if (networks[i].channel != rechannelIdx) continue;
            channelNetCount[rechannelIdx]++;
            bool deauthable = !networks[i].hasPMF &&
                (networks[i].authmode == WIFI_AUTH_WPA_PSK ||
                 networks[i].authmode == WIFI_AUTH_WPA2_PSK ||
                 networks[i].authmode == WIFI_AUTH_WPA_WPA2_PSK ||
                 networks[i].authmode == WIFI_AUTH_WPA2_WPA3_PSK);
            bool saeTarget = saeOn && networks[i].hasPMF &&
                networks[i].authmode == WIFI_AUTH_WPA3_PSK;
            if (deauthable || saeTarget) channelAttackableCount[rechannelIdx]++;
        }
    }

    // ==[ DEFERRED PROBE HARVEST ]== callback staged, process here
    if (pendingProbe.ready) {
        processProbeEntry();
        pendingProbe.ready = false;
    }

    // ==[ SPARKLINE UPDATE ]== internally rate-limited to 500ms
    updateSparklines();

    if (!clientMode && !isShowing5GHz()) {
        updateMeasuredTraceAnimation(now);
    }

    if (c5CarrierDetailActive) {
        if (!C5Monster::isConnected() || !c5CarrierTarget.valid) {
            c5CarrierDetailActive = false;
            resetC5CarrierBearingState(nullptr);
        } else {
            updateBearingFromRfPose(
                c5CarrierBearing, c5CarrierBearingConfig, now);
            maybeRebaseC5CarrierScanReference();

            C5Monster::TargetObservationTelemetry targeted{};
            if (C5Monster::getTargetObservation(targeted) &&
                targeted.revision !=
                    c5CarrierTarget.lastTargetObservationRevision &&
                targeted.ageMs <= 5000u &&
                memcmp(targeted.observation.bssid,
                       c5CarrierTarget.bssid, 6u) == 0) {
                const auto& observation = targeted.observation;
                c5CarrierTarget.rssi = observation.rssi;
                c5CarrierTarget.rssiSmooth = RFUtil::smoothIIR(
                    c5CarrierTarget.rssiSmooth, observation.rssi, 4u);
                c5CarrierTarget.lastSeenMs = targeted.receivedAtMs;
                const uint32_t observationAtMs =
                    targeted.receivedAtMs - observation.windowMs / 2u;
                const uint32_t observationAtUs =
                    targeted.receivedAtUs -
                    observation.windowMs * 500u;
                const uint8_t quality = (uint8_t)constrain(
                    55 + min((int)observation.sampleCount, 25) -
                        min((int)observation.varianceDbm2 * 3, 45) -
                        min((int)observation.windowMs / 100, 20),
                    10, 90);
                Bearing::ObservationPose pose{};
                const uint8_t evidenceFlags =
                    Bearing::RF_EVIDENCE_PASSIVE |
                    Bearing::RF_EVIDENCE_REMOTE |
                    (observation.evidence ==
                             C5Protocol::TargetEvidence::CSI
                         ? Bearing::RF_EVIDENCE_CSI
                         : 0u);
                // JanOS reports an aggregate window. Bind it to the midpoint,
                // not UART receipt; if that pose has already aged out, retain
                // the telemetry but do not invent a directional sample.
                if (observationPoseForRx(observationAtUs, pose)) {
                    Bearing::feedRSSIObservedPose(
                        c5CarrierBearing, c5CarrierBearingConfig,
                        observation.rssi, c5CarrierTarget.rssiSmooth,
                        observationAtMs, now, pose,
                        evidenceFlags, quality);
                }
                observeGpsRouteEvidence(
                    observation.rssi, observationAtMs);
                c5CarrierTarget.lastTargetObservationRevision =
                    targeted.revision;
            }
        }
    }

    // ==[ CLIENT MONITOR GEIGER ]== proximity feedback for selected client
    // Click based on proximity, but apply staleness penalty using packet age.
    // Important: do NOT stop/start Geiger on stale RSSI (causes "burst restarts").
    if (clientMode && clientDetailActive) {
        CsiTracker::update();
    } else if (CsiTracker::isTargetActive()) {
        CsiTracker::clearTarget();
    }
    if (clientMode && !paranoid.attackActive) {
        const SpectrumClient* client = getSelectedClient();
        if (client) {
            const uint32_t age = client->hasSignal
                ? SpectrumThruMath::observationAgeMs(
                      now, client->lastSignalSeen)
                : NETWORK_TIMEOUT + 1u;
            uint32_t geigerAgeMs = age;
            const bool newClientTarget = !isClientBearingTarget(client);
            if (newClientTarget ||
                !Geiger::isActive() ||
                Geiger::getSource() != Geiger::SOURCE_CLIENT) {
                Geiger::start(Geiger::SOURCE_CLIENT);
                resetClientBearingState(client);
                if (clientDetailActive) syncClientCsiTarget();
            }

            int8_t geigerRssi = client->rssi;
            CsiTracker::Snapshot csi{};
            bool haveCsi = false;
#if defined(HAMLET_WIFI_CSI)
            if (CsiTracker::isTargetActive() &&
                CsiTracker::getSnapshot(csi) &&
                csi.sampleCount > 0u) {
                haveCsi = true;
                if (csiUsable(csi)) {
                    geigerRssi = client->hasSignal
                        ? (int8_t)constrain(
                            ((int)geigerRssi * 3 + (int)csi.rssi) / 4,
                            -95, -20)
                        : csi.rssi;
                    geigerAgeMs = (csi.ageMs < age) ? csi.ageMs : age;
                }
            }
#endif
            Geiger::update(geigerRssi, geigerAgeMs);

            // ==[ CLIENT BEARING TRACKER ]== gyro every frame, RSSI on new data only
            if (clientDetailActive) {
                updateBearingFromRfPose(
                    clientBearing, clientBearingConfig, now);
                maybeRebaseClientScanReference(client);

                // Feed only real client-originated arrivals. Both callbacks
                // carry the same RX hardware timestamp, so dedup uses packet
                // identity rather than a time-proximity guess.
                const uint32_t clientSeen = client->lastSignalSeen;
                const bool clientNew = SpectrumThruMath::packetIsNew(
                    client->hasSignal, client->lastSignalRxUs,
                    clientBearingSignalConsumed,
                    clientBearingLastSignalRxUs);
                const uint32_t csiSeen = haveCsi ? csi.lastSeenMs : 0u;
                const bool csiNew = SpectrumThruMath::packetIsNew(
                    haveCsi, csi.rxTimestampUs, clientBearingCsiConsumed,
                    clientBearingLastCsiRxUs);
                const bool clientAlreadyFedByCsi =
                    SpectrumThruMath::alreadyConsumedByOther(
                        clientNew, client->lastSignalRxUs,
                        clientBearingCsiConsumed, clientBearingLastCsiRxUs);
                const bool csiAlreadyFedByClient =
                    SpectrumThruMath::alreadyConsumedByOther(
                        csiNew, csi.rxTimestampUs,
                        clientBearingSignalConsumed,
                        clientBearingLastSignalRxUs);
                const bool clientFeedable = clientNew &&
                    !clientAlreadyFedByCsi &&
                    age <= kClientScanUsableCsiAgeMs;
                const bool csiFeedable = csiNew &&
                    !csiAlreadyFedByClient &&
                    csi.ageMs <= kClientScanUsableCsiAgeMs;

                if (clientNew || csiNew) {
                    if (csiNew) traceCsiSnapshot(csi);
                    bool isFlat = Pedometer::isCachedFlat();
                    uint8_t baseRatio = 3;  // MED default
                    switch (Config::getRssiSmooth()) {
                        case Config::RSSI_SMOOTH_FAST: baseRatio = 2; break;
                        case Config::RSSI_SMOOTH_MED:  baseRatio = 3; break;
                        case Config::RSSI_SMOOTH_SLOW: baseRatio = 5; break;
                    }
                    uint8_t rssiRatio = isFlat ? (baseRatio + 1) : baseRatio;

                    auto feedObserved = [&](int8_t sampleRssi, uint32_t seenMs,
                                            uint32_t rxTimestampUs,
                                            uint8_t flags, uint8_t quality) {
                        if (!clientRssiSmoothValid) {
                            clientRssiSmooth = sampleRssi;
                            clientRssiSmoothValid = true;
                        } else {
                            clientRssiSmooth = RFUtil::smoothIIR(
                                clientRssiSmooth, sampleRssi, rssiRatio);
                        }
                        Bearing::ObservationPose pose{};
                        if (observationPoseForRx(rxTimestampUs, pose)) {
                            Bearing::feedRSSIObservedPose(
                                clientBearing, clientBearingConfig,
                                sampleRssi, clientRssiSmooth,
                                seenMs, now, pose, flags, quality);
                            clientBearingLastPointRxUs = rxTimestampUs;
                            clientBearingPointRxValid = true;
                        } else {
                            // The packet remains useful for cadence and RSSI
                            // trend, but without packet-time pose it has no
                            // defensible bearing and must not enter the cloud.
                            Bearing::feedRSSIScalarObserved(
                                clientBearing, clientBearingConfig,
                                sampleRssi, clientRssiSmooth,
                                seenMs, now);
                        }
                        observeGpsRouteEvidence(sampleRssi, seenMs);
                    };

                    const bool sameArrival = clientFeedable && csiFeedable &&
                        SpectrumThruMath::samePacket(
                            client->lastSignalRxUs, csi.rxTimestampUs);

                    if (csiAlreadyFedByClient &&
                        clientBearingPointRxValid &&
                        SpectrumThruMath::samePacket(
                            csi.rxTimestampUs, clientBearingLastPointRxUs)) {
                        Bearing::addNewestRfPointEvidence(
                            clientBearing, Bearing::RF_EVIDENCE_CSI,
                            csi.quality);
                    }

                    if (sameArrival) {
                        const uint32_t seenMs = SpectrumThruMath::firstBeforeSecond(
                            clientSeen, csiSeen) ? csiSeen : clientSeen;
                        feedObserved(client->rssi, seenMs,
                                     client->lastSignalRxUs,
                                     Bearing::RF_EVIDENCE_PASSIVE |
                                         Bearing::RF_EVIDENCE_CSI,
                                     csi.quality);
                    } else if (clientFeedable && csiFeedable) {
                        if (SpectrumThruMath::firstBeforeSecond(
                                client->lastSignalRxUs, csi.rxTimestampUs)) {
                            feedObserved(client->rssi, clientSeen,
                                         client->lastSignalRxUs,
                                         Bearing::RF_EVIDENCE_PASSIVE, 0u);
                            feedObserved(csi.rssi, csiSeen,
                                         csi.rxTimestampUs,
                                         Bearing::RF_EVIDENCE_PASSIVE |
                                             Bearing::RF_EVIDENCE_CSI,
                                         csi.quality);
                        } else {
                            feedObserved(csi.rssi, csiSeen,
                                         csi.rxTimestampUs,
                                         Bearing::RF_EVIDENCE_PASSIVE |
                                             Bearing::RF_EVIDENCE_CSI,
                                         csi.quality);
                            feedObserved(client->rssi, clientSeen,
                                         client->lastSignalRxUs,
                                         Bearing::RF_EVIDENCE_PASSIVE, 0u);
                        }
                    } else if (clientFeedable) {
                        feedObserved(client->rssi, clientSeen,
                                     client->lastSignalRxUs,
                                     Bearing::RF_EVIDENCE_PASSIVE, 0u);
                    } else if (csiFeedable) {
                        feedObserved(csi.rssi, csiSeen,
                                     csi.rxTimestampUs,
                                     Bearing::RF_EVIDENCE_PASSIVE |
                                         Bearing::RF_EVIDENCE_CSI,
                                     csi.quality);
                    }

                    if (clientNew) {
                        clientBearingLastSignalRxUs = client->lastSignalRxUs;
                        clientBearingSignalConsumed = true;
                    }
                    if (csiNew) {
                        clientBearingLastCsiRxUs = csi.rxTimestampUs;
                        clientBearingCsiConsumed = true;
                    }
                }
            }
        } else {
            if (Geiger::getSource() == Geiger::SOURCE_CLIENT) {
                Geiger::stop();
            }
            // Navigation owns the detail lifetime. A transiently absent target
            // is a stale/reacquiring state, never an implicit BACK action.
            if (CsiTracker::isTargetActive()) syncClientCsiTarget();
        }
    } else if (Geiger::getSource() == Geiger::SOURCE_CLIENT) {
        Geiger::stop();
        resetClientBearingState(nullptr);
    }

    // ==[ PARANOID SWINE UPDATE ]==
    updateParanoidSwine();

    // ==[ DIAL MODE UPDATE ]==
    updateDialChannel();

    // ==[ MONITORED AP FRESHNESS ]==
    // The AP/client screen is user-owned navigation. RF silence changes the
    // evidence state and emits one edge-triggered warning; it never pops the
    // user back to the main Spectrum screen.
    if (clientMode) {
        const uint16_t mi = specHashFind(monitoredBSSID);
        bool stale = true;
        if (mi != SPEC_HASH_EMPTY) {
            if (selectedIdx != (int16_t)mi) {
                selectedIdx = mi;
            }
            stale = SpectrumThruMath::observationAgeMs(
                        now, networks[mi].lastSeen) > NETWORK_TIMEOUT;
            rebindSelectedClientIdentity();
        }
        if (stale && !monitoredNetworkWasStale) {
            SFX::play(SFX::SIGNAL_LOST);
        }
        monitoredNetworkWasStale = stale;
    }

    // Channel hopping (unless locked OR in dial mode)
    if (lockedChannel == 0 && !dialMode) {
        if (now - lastHopTime >= currentHopIntervalMs) {
            hopChannel();
            lastHopTime = now;
        }
    }

    // ==[ HIDDEN SSID PROBING ]== reveal ghost signals during dwell
    probeHiddenNetworks();

    // Update RSSI history for waterfall
    if (now - lastHistoryUpdate >= HISTORY_INTERVAL) {
        historyPos = (historyPos + 1) % RSSI_HISTORY_LEN;
        for (int ch = 1; ch <= 13; ch++) {
            if (rssiHistory) rssiHistory[ch * RSSI_HISTORY_LEN + historyPos] = channelPeakRSSI[ch];
            // Gentle decay toward average (smoother than hard reset)
            if (channelPeakRSSI[ch] > channelAvgRSSI[ch]) {
                channelPeakRSSI[ch] = (channelPeakRSSI[ch] + channelAvgRSSI[ch]) / 2;
            }
        }
        lastHistoryUpdate = now;
    }

    // ==[ DISCOVERY TRACKING ]== detect new network arrivals
    if (networkCount > prevNetworkCount) {
        lastNewNetworkTime = now;
        prevNetworkCount = networkCount;
    }

    // Periodic cleanup + intelligence processing
    if (now - lastCleanup >= CLEANUP_INTERVAL) {
        cleanupStale();
        sortNetworksByRSSI();       // sort first (invalidates parallel array indices)
        // ==[ INTELLIGENCE ]== rebuild analytics on sorted network order
        classifyNetworks();         // auto-tier T0-T7 + readiness score
        detectRogueAPs();           // SSID collision → evil twin alerts
        buildBSSIDClusters();       // OUI grouping → infrastructure map
        updateBeaconAnomalies();    // beacon interval deviation flags
        // sparkline + beacon data now moved with networks during sort — no wipe needed
        lastCleanup = now;

        // Cleanup and sorting can move the selected records. Rebind before the
        // next draw so one frame never inherits another target's THRU cloud.
        if (clientMode) {
            const uint16_t monitoredIdx = specHashFind(monitoredBSSID);
            if (monitoredIdx != SPEC_HASH_EMPTY) {
                selectedIdx = (int16_t)monitoredIdx;
            }
            const SpectrumClient* reboundClient =
                rebindSelectedClientIdentity() ? getSelectedClient() : nullptr;
            if (!reboundClient) {
                if (Geiger::getSource() == Geiger::SOURCE_CLIENT) Geiger::stop();
            } else if (!isClientBearingTarget(reboundClient)) {
                if (Geiger::getSource() == Geiger::SOURCE_CLIENT) {
                    Geiger::start(Geiger::SOURCE_CLIENT);
                }
                resetClientBearingState(reboundClient);
                syncClientCsiTarget();
            }
        }
    }

    // Track current channel for the next dwell-time slice.
    lastTimedChannel = currentChannel;

    // ==[ V2 VISUALS: SPECTRUM BUFFERS + WATERFALL ]==
    // Skip spectrum buffer updates in 5GHz mode - 5GHz data comes from C5Monster scan snapshots,
    // not real-time promiscuous capture. The pane derives model lobes directly
    // from those snapshots and labels them accordingly.
    if (!clientMode && !isShowing5GHz()) {
        cacheFrameConstants();
        RfMeasurement::SweepSnapshot completed{};
        if (rfMeasurements &&
            measuredSweep &&
            rfMeasurements->consumeCompleted(completed)) {
            *measuredSweep = completed;
            measuredSweepValid = true;
            publishMeasuredTrace(*measuredSweep, now);
            stageWaterfallTarget(*measuredSweep);
        }
    }

    // ==[ C5 MONSTER AUTO-SCAN ]== periodic 5GHz scan when C5 connected
    // Scanning is centralized in hamlet.cpp to avoid racing with the C5 menu.
    // Spectrum only consumes data via feedC5MonsterScan().

}

bool isActive() { return active; }
bool isInClientMode() { return clientMode; }
uint8_t getCurrentChannel() { return currentChannel; }
uint8_t getLockedChannel() { return lockedChannel; }
uint32_t getRfQueueDrops() { return packetEvents.drops(); }
uint32_t getMeasurementSweepEpoch() {
    return measuredSweepValid && measuredSweep
        ? measuredSweep->epoch : 0u;
}
uint16_t getMeasurementCoverageMask() {
    if (measuredSweepValid && measuredSweep) {
        return measuredSweep->coverageMask;
    }
    return rfMeasurements ? rfMeasurements->coverageMask() : 0u;
}

void togglePause() {
    const bool wasPaused = paused.load(std::memory_order_acquire);
    if (wasPaused) {
        // Purge while the callback is still barred, then reopen the producer.
        // Using the consumer side keeps the SPSC ring safe; reset() is only
        // legal once promiscuous reception has stopped.
        discardPacketEvents();
        paused.store(false, std::memory_order_release);
    } else {
        paused.store(true, std::memory_order_release);
        // A callback already in flight may finish this push. It will remain
        // barred and is purged above before a later resume.
        discardPacketEvents();
    }
    const uint32_t now = millis();
    discardPartialMeasurementSweep(
        currentChannel.load(std::memory_order_relaxed));
    lastChannelTimeUpdate = now;
    lastHopTime = now;
    resetPpsWindow(now);
}

bool isPaused() { return paused; }

void nextNetwork() {
    if (clientMode) {
        // Navigate clients
        const SpectrumNetwork* net = getSelectedNetwork();
        if (net && net->clientCount > 0) {
            selectedClientIdx = (selectedClientIdx + 1) % net->clientCount;
            const SpectrumClient* client = &net->clients[selectedClientIdx];
            bindSelectedClientIdentity(client);
            if (!isClientBearingTarget(client)) {
                if (Geiger::getSource() == Geiger::SOURCE_CLIENT) {
                    Geiger::start(Geiger::SOURCE_CLIENT);
                }
                resetClientBearingState(client);
#if HAMLET_DEBUG_LOG
                if (client) {
                    HAMLET_LOGF("[SPECTRUM] THRU next client=%02X:%02X:%02X:%02X:%02X:%02X\n",
                               client->mac[0], client->mac[1], client->mac[2],
                               client->mac[3], client->mac[4], client->mac[5]);
                }
#endif
            }
            syncClientCsiTarget();
        }
    } else {
        // Navigate networks
        if (isShowing5GHz()) {
            if (c5gNetworkCount > 0) {
                c5gSelectedIdx = (c5gSelectedIdx + 1) % c5gNetworkCount;
                focus5GHzNetwork(c5gNetworks[c5gSelectedIdx].channel);
                if (c5CarrierDetailActive) {
                    resetC5CarrierBearingState(&c5gNetworks[c5gSelectedIdx]);
                }
            }
        } else if (networkCount > 0) {
            selectedIdx = (selectedIdx + 1) % networkCount;
        }
    }
}

void prevNetwork() {
    if (clientMode) {
        const SpectrumNetwork* net = getSelectedNetwork();
        if (net && net->clientCount > 0) {
            selectedClientIdx = (selectedClientIdx - 1 + net->clientCount) % net->clientCount;
            const SpectrumClient* client = &net->clients[selectedClientIdx];
            bindSelectedClientIdentity(client);
            if (!isClientBearingTarget(client)) {
                if (Geiger::getSource() == Geiger::SOURCE_CLIENT) {
                    Geiger::start(Geiger::SOURCE_CLIENT);
                }
                resetClientBearingState(client);
#if HAMLET_DEBUG_LOG
                if (client) {
                    HAMLET_LOGF("[SPECTRUM] THRU prev client=%02X:%02X:%02X:%02X:%02X:%02X\n",
                               client->mac[0], client->mac[1], client->mac[2],
                               client->mac[3], client->mac[4], client->mac[5]);
                }
#endif
            }
            syncClientCsiTarget();
        }
    } else {
        if (isShowing5GHz()) {
            if (c5gNetworkCount > 0) {
                c5gSelectedIdx = (c5gSelectedIdx - 1 + c5gNetworkCount) % c5gNetworkCount;
                focus5GHzNetwork(c5gNetworks[c5gSelectedIdx].channel);
                if (c5CarrierDetailActive) {
                    resetC5CarrierBearingState(&c5gNetworks[c5gSelectedIdx]);
                }
            }
        } else if (networkCount > 0) {
            selectedIdx = (selectedIdx - 1 + networkCount) % networkCount;
        }
    }
}

void selectNetwork() {
    if (isShowing5GHz()) {
        if (c5gSelectedIdx >= 0 && c5gSelectedIdx < c5gNetworkCount) {
            focus5GHzNetwork(c5gNetworks[c5gSelectedIdx].channel);
            c5CarrierDetailActive = true;
            c5ArsenalSelectedIdx = 0;
            resetC5CarrierBearingState(&c5gNetworks[c5gSelectedIdx]);
        }
        return;
    }
    if (selectedIdx >= 0 && selectedIdx < networkCount) {
        const uint8_t targetChannel = networks[selectedIdx].channel;
        const esp_err_t channelErr =
            esp_wifi_set_channel(targetChannel, WIFI_SECOND_CHAN_NONE);
        if (channelErr != ESP_OK) {
            HAMLET_LOGF("[SPECTRUM] channel set ch%d failed: 0x%x\n",
                        targetChannel, channelErr);
            Display::showToast("CHANNEL LOCK FAILED", 1500);
            return;
        }

        clientMode = true;
        clientDetailActive = false;
        clearSelectedClientIdentity();
        clientsDiscovered = 0;
        
        // Keep the BSSID, not the row index: cleanup may compact the table
        // while the locked network is still the same case.
        memcpy(monitoredBSSID, networks[selectedIdx].bssid, 6);
        monitoredNetworkWasStale =
            SpectrumThruMath::observationAgeMs(
                millis(), networks[selectedIdx].lastSeen) > NETWORK_TIMEOUT;
        if (networks[selectedIdx].clientCount > 0u) {
            bindSelectedClientIdentity(&networks[selectedIdx].clients[0]);
        }
        
        // Lock to network's channel
        lockedChannel = targetChannel;
        currentChannel = lockedChannel;
        resetPpsWindow(millis());
        discardPartialMeasurementSweep(
            currentChannel.load(std::memory_order_relaxed));

#if HAMLET_DEBUG_LOG
        HAMLET_LOGF(
            "[SPECTRUM] THRU enter idx=%d bssid=%02X:%02X:%02X:%02X:%02X:%02X ch=%u\n",
            selectedIdx,
            networks[selectedIdx].bssid[0], networks[selectedIdx].bssid[1], networks[selectedIdx].bssid[2],
            networks[selectedIdx].bssid[3], networks[selectedIdx].bssid[4], networks[selectedIdx].bssid[5],
            networks[selectedIdx].channel);
#endif
        
        resetClientBearingState(nullptr);
        clearPotfileCache();
        syncClientCsiTarget();
    }
}

void exitClientMode() {
    clientMode = false;
    clientDetailActive = false;
    clearSelectedClientIdentity();
    lockedChannel = 0;  // Resume hopping
    resetPpsWindow(millis());
    memset(monitoredBSSID, 0, 6);
    monitoredNetworkWasStale = false;
    clientsDiscovered = 0;
    discardPartialMeasurementSweep(
        currentChannel.load(std::memory_order_relaxed));
    resetClientBearingState(nullptr);
    clearPotfileCache();
    syncClientCsiTarget();

#if HAMLET_DEBUG_LOG
    HAMLET_LOGF("[SPECTRUM] THRU exit\n");
#endif
}

// ==[ CLIENT DETAIL ]== evidence panel layered over the locked-network view
bool isClientDetailActive() {
    return clientDetailActive || c5CarrierDetailActive;
}

bool isC5CarrierDetailActive() {
    return c5CarrierDetailActive;
}

void toggleClientDetail() {
    if (clientMode && getSelectedClient()) {
        clientDetailActive = !clientDetailActive;
        if (clientDetailActive) {
            const SpectrumClient* client = getSelectedClient();
            bindSelectedClientIdentity(client);
            ensureClientBearingTargetBound(client);
            // Frames received while detail was closed have no recorded pose.
            // Consume them; the cloud resumes on the next posed arrival.
            clientBearingSignalConsumed =
                client && client->hasSignal;
            clientBearingLastSignalRxUs = clientBearingSignalConsumed
                ? client->lastSignalRxUs : 0u;
            CsiTracker::begin();
            clientBearingCsiConsumed = false;
            clientBearingLastCsiRxUs = 0u;

#if HAMLET_DEBUG_LOG
            if (client) {
                HAMLET_LOGF("[SPECTRUM] THRU detail on %02X:%02X:%02X:%02X:%02X:%02X\n",
                           client->mac[0], client->mac[1], client->mac[2],
                           client->mac[3], client->mac[4], client->mac[5]);
            }
#endif
        } else {
#if HAMLET_DEBUG_LOG
            HAMLET_LOGF("[SPECTRUM] THRU detail off\n");
#endif
        }
        syncClientCsiTarget();
    }
}

void closeClientDetail() {
    clientDetailActive = false;
    if (c5CarrierDetailActive) {
        c5CarrierDetailActive = false;
        resetC5CarrierBearingState(nullptr);
    }
    syncClientCsiTarget();
}

bool pokeSelectedClient() {
    if (c5CarrierDetailActive) return requestC5CarrierRescan();
    return pokeClientTarget();
}

// ==[ BEARING LOCK RESET ]== user can reset bearing tracker to recalibrate
void toggleApBearing() {
    if (c5CarrierDetailActive && c5CarrierTarget.valid) {
        // The highlighted list row can move when the bound AP ages out of the
        // retained scan list. Recalibrate the detail's BSSID, never whatever
        // row happens to inherit its old index.
        C5GHzNetwork boundTarget = {};
        memcpy(boundTarget.bssid, c5CarrierTarget.bssid,
               sizeof(boundTarget.bssid));
        boundTarget.channel = c5CarrierTarget.channel;
        boundTarget.rssi = c5CarrierTarget.rssi;
        boundTarget.lastSeenMs = c5CarrierTarget.lastSeenMs;
        resetC5CarrierBearingState(&boundTarget);
        return;
    }
    if (clientMode && clientDetailActive) {
        // Reset client bearing tracker for fresh lock
        resetClientBearingState(getSelectedClient());
        // Drop CSI queued before the new pose origin, then re-arm the target.
        CsiTracker::clearTarget();
        clientBearingCsiConsumed = false;
        clientBearingLastCsiRxUs = 0u;
        syncClientCsiTarget();

#if HAMLET_DEBUG_LOG
        const SpectrumClient* client = getSelectedClient();
        if (client) {
            HAMLET_LOGF("[SPECTRUM] THRU recenter %02X:%02X:%02X:%02X:%02X:%02X\n",
                       client->mac[0], client->mac[1], client->mac[2],
                       client->mac[3], client->mac[4], client->mac[5]);
        } else {
            HAMLET_LOGF("[SPECTRUM] THRU recenter (no client)\n");
        }
#endif
    }
}

int16_t getSelectedIndex() { return selectedIdx; }
int16_t getSelectedClientIndex() { return selectedClientIdx; }

static int8_t getC5ActionTargetIndex() {
    if (c5CarrierDetailActive && c5CarrierTarget.valid) {
        return findC5GHzNetworkByBssid(c5CarrierTarget.bssid);
    }
    if (c5gSelectedIdx >= 0 && c5gSelectedIdx < c5gNetworkCount) {
        return c5gSelectedIdx;
    }
    return -1;
}

static const C5GHzNetwork* getC5ActionTargetNetwork() {
    const int8_t idx = getC5ActionTargetIndex();
    return idx >= 0 ? &c5gNetworks[idx] : nullptr;
}

static uint8_t get5GHzSelectionIndex1Based() {
    const C5GHzNetwork* selected = getC5ActionTargetNetwork();
    if (!selected) return 0u;
    return selected->sourceRevision == c5gLastScanRevision
        ? selected->sourceIndex
        : 0u;
}

const SpectrumNetwork* getSelectedNetwork() {
    if (isShowing5GHz()) return nullptr;
    if (selectedIdx >= 0 && selectedIdx < networkCount) {
        return &networks[selectedIdx];
    }
    return nullptr;
}

const SpectrumClient* getSelectedClient() {
    const SpectrumNetwork* net = getSelectedNetwork();
    if (!net || net->clientCount == 0u) return nullptr;
    if (!rebindSelectedClientIdentity()) return nullptr;
    if (selectedClientIdx >= 0 && selectedClientIdx < net->clientCount) {
        return &net->clients[selectedClientIdx];
    }
    return nullptr;
}

uint8_t getC5ArsenalCommandCount() {
    return C5_ARSENAL_COMMAND_COUNT;
}

uint8_t getSelectedC5ArsenalCommand() {
    return c5ArsenalSelectedIdx;
}

const char* getSelectedC5ArsenalCommandLabel() {
    return (C5_ARSENAL_COMMAND_COUNT > 0u)
        ? C5_ARSENAL_COMMANDS[c5ArsenalSelectedIdx].label
        : "";
}

bool isSelectedC5ArsenalCommandTargeted() {
    return C5_ARSENAL_COMMAND_COUNT > 0u
        && C5_ARSENAL_COMMANDS[c5ArsenalSelectedIdx].target
            != C5ArsenalCommand::Target::NONE;
}

bool hasC5TargetSelection() {
    if (!isShowing5GHz() || !getC5ActionTargetNetwork()) {
        return false;
    }
    if (C5_ARSENAL_COMMANDS[c5ArsenalSelectedIdx].target ==
        C5ArsenalCommand::Target::NETWORK) {
        return get5GHzSelectionIndex1Based() != 0u;
    }
    return true;
}

static bool isC5ArsenalActionAvailable(uint8_t actionIdx) {
    if (actionIdx >= C5_ARSENAL_COMMAND_COUNT) return false;
    const C5ArsenalCommand& entry = C5_ARSENAL_COMMANDS[actionIdx];

    if (entry.kind == C5ArsenalCommand::Kind::RECALIBRATE) {
        return c5CarrierDetailActive && c5CarrierTarget.valid;
    }
    if (entry.kind == C5ArsenalCommand::Kind::RESCAN) {
        return c5CarrierDetailActive && C5Monster::isConnected() &&
            !C5Monster::isBusy() && !C5Monster::hasActiveOperation();
    }

    if (entry.cmd && strcmp(entry.cmd, C5Protocol::CMD_STOP) == 0) {
        return C5Monster::isBusy() || C5Monster::hasActiveOperation();
    }
    if (!C5Monster::isConnected() || C5Monster::isBusy() ||
        C5Monster::hasActiveOperation()) {
        return false;
    }

    const C5GHzNetwork* target = getC5ActionTargetNetwork();
    if (entry.target != C5ArsenalCommand::Target::NONE && !target) {
        return false;
    }
    if (entry.cmd && strcmp(entry.cmd, C5Protocol::CMD_SAE_OVERFLOW) == 0) {
        return target &&
            (target->authType == C5Protocol::AUTH_WPA3 ||
             target->authType == C5Protocol::AUTH_WPA2_WPA3_MIXED);
    }
    return true;
}

bool isSelectedC5ArsenalCommandAvailable() {
    return isC5ArsenalActionAvailable(c5ArsenalSelectedIdx);
}

static void stepC5ArsenalAction(int8_t direction) {
    if (C5_ARSENAL_COMMAND_COUNT == 0u) return;
    uint8_t candidate = c5ArsenalSelectedIdx;
    for (uint8_t step = 0; step < C5_ARSENAL_COMMAND_COUNT; ++step) {
        candidate = direction > 0
            ? (uint8_t)((candidate + 1u) % C5_ARSENAL_COMMAND_COUNT)
            : (uint8_t)((candidate + C5_ARSENAL_COMMAND_COUNT - 1u) %
                        C5_ARSENAL_COMMAND_COUNT);
        if (isC5ArsenalActionAvailable(candidate)) {
            c5ArsenalSelectedIdx = candidate;
            return;
        }
    }
}

void nextC5ArsenalCommand() {
    stepC5ArsenalAction(1);
}

void prevC5ArsenalCommand() {
    stepC5ArsenalAction(-1);
}

bool executeSelectedC5ArsenalCommand() {
    if (C5_ARSENAL_COMMAND_COUNT == 0u ||
        !isC5ArsenalActionAvailable(c5ArsenalSelectedIdx)) {
        return false;
    }

    const C5ArsenalCommand& entry = C5_ARSENAL_COMMANDS[c5ArsenalSelectedIdx];
    if (entry.kind == C5ArsenalCommand::Kind::RECALIBRATE) {
        toggleApBearing();
        return true;
    }
    if (entry.kind == C5ArsenalCommand::Kind::RESCAN) {
        return requestC5CarrierRescan();
    }
    if (entry.kind == C5ArsenalCommand::Kind::TARGET_OBSERVE) {
        const C5GHzNetwork* target = getC5ActionTargetNetwork();
        if (!target) return false;
        char observeCommand[72];
        snprintf(
            observeCommand, sizeof(observeCommand),
            "%s %02X:%02X:%02X:%02X:%02X:%02X %u 500",
            C5Protocol::CMD_OBSERVE_BSSID,
            target->bssid[0], target->bssid[1], target->bssid[2],
            target->bssid[3], target->bssid[4], target->bssid[5],
            (unsigned)target->channel);
        return C5Monster::sendCommand(observeCommand, 5000u);
    }

    // Recovery must remain reachable after a command timeout marks the bridge
    // unhealthy; the UART may still be running a continuous JanOS operation.
    if (strcmp(entry.cmd, C5Protocol::CMD_STOP) == 0) {
        C5Monster::emergencyStop();
        return true;
    }
    if (!C5Monster::isConnected()) {
        return false;
    }

    if (entry.target == C5ArsenalCommand::Target::NONE) {
        return C5Monster::sendCommand(entry.cmd);
    }

    if (!hasC5TargetSelection()) {
        return false;
    }

    if (entry.target == C5ArsenalCommand::Target::CHANNEL) {
        const C5GHzNetwork* target = getC5ActionTargetNetwork();
        if (!target) return false;
        char monitorCmd[32];
        snprintf(monitorCmd, sizeof(monitorCmd), "%s %u",
                 entry.cmd, target->channel);
        return C5Monster::sendCommand(monitorCmd);
    }

    char selectCmd[32];
    snprintf(selectCmd, sizeof(selectCmd), "%s %u",
             C5Protocol::CMD_SELECT_NETWORKS,
             get5GHzSelectionIndex1Based());
    return C5Monster::sendCommandThen(selectCmd, entry.cmd);
}

const SpectrumNetwork* getNetworks() { return networks; }
uint16_t getNetworkCount() { return networkCount; }

int8_t getChannelPeakRSSI(uint8_t channel) {
    if (channel >= 1 && channel <= 13) {
        return channelPeakRSSI[channel];
    }
    return -100;
}

int8_t getChannelAvgRSSI(uint8_t channel) {
    if (channel >= 1 && channel <= 13) {
        return channelAvgRSSI[channel];
    }
    return -100;
}

uint16_t getChannelNetworkCount(uint8_t channel) {
    if (channel >= 1 && channel <= 13) {
        return channelNetCount[channel];
    }
    return 0;
}

const int8_t* getRSSIHistory(uint8_t channel) {
    if (channel < 1 || channel > 13) {
        return nullptr;
    }
    if (!rssiHistory) return nullptr;
    return rssiHistory + channel * RSSI_HISTORY_LEN;
}

uint8_t getHistoryPosition() {
    return historyPos;
}

uint32_t getRuntime() {
    return active ? (millis() - startTime) : 0;
}

void printRfTrace(bool clearAfter) {
    if (!rfTrace) {
        Serial.println("[RFTRACE] unavailable; enter Spectrum first");
        return;
    }

    Serial.printf(
        "[RFTRACE] abi=%u record=%u count=%u overwritten=%lu queueDrops=%lu\n",
        (unsigned)RfTrace::kVersion, (unsigned)sizeof(RfTrace::Record),
        (unsigned)rfTrace->count(),
        (unsigned long)rfTrace->overwritten(),
        (unsigned long)packetEvents.drops());
    Serial.println(
        "RF2,ver,board,fw,obs_ms,rx_us,ch,class,bytes,rssi,noise,"
        "id,peer,phy,cwb,dwell_us,drops,csi_layout,csi_orig,csi_keep,"
        "pose_flags,pose_age,yaw,pitch,roll,drift,gps_q,gps_age,"
        "hdop,speed,lat,lon");
    RfTrace::Record record{};
    for (size_t i = 0u; i < rfTrace->count(); ++i) {
        if (!rfTrace->chronological(i, record)) continue;
        Serial.printf(
            "RF2,%u,%u,%08lX,%lu,%lu,%u,%u,%u,%d,%d,"
            "%02X%02X%02X%02X%02X%02X,"
            "%02X%02X%02X%02X%02X%02X,"
            "%u,%u,%lu,%lu,%u,%u,%u,%u,%u,%d,%d,%d,%u,"
            "%u,%u,%u,%u,%ld,%ld\n",
            (unsigned)record.version, (unsigned)record.board,
            (unsigned long)record.firmwareRevision,
            (unsigned long)record.observationMs,
            (unsigned long)record.rxTimestampUs,
            (unsigned)record.channel, (unsigned)record.frameClass,
            (unsigned)record.frameBytes, (int)record.rssi,
            (int)record.noiseFloor,
            record.identity[0], record.identity[1], record.identity[2],
            record.identity[3], record.identity[4], record.identity[5],
            record.peer[0], record.peer[1], record.peer[2],
            record.peer[3], record.peer[4], record.peer[5],
            (unsigned)record.phyMode,
            (unsigned)record.channelWidth,
            (unsigned long)record.channelDwellUs,
            (unsigned long)record.queueDrops,
            (unsigned)record.csiLayout,
            (unsigned)record.csiOriginalLength,
            (unsigned)record.csiRetainedLength,
            (unsigned)record.poseFlags,
            (unsigned)record.poseAgeUs,
            (int)record.yawDegX10,
            (int)record.pitchDegX10,
            (int)record.rollDegX10,
            (unsigned)record.yawDriftDegX100,
            (unsigned)record.gpsFixQuality,
            (unsigned)record.gpsAgeMs,
            (unsigned)record.hdopX100,
            (unsigned)record.speedCms,
            (long)record.latitudeE7,
            (long)record.longitudeE7);
    }
    if (clearAfter) {
        rfTrace->reset();
        Serial.println("[RFTRACE] cleared");
    }
}

#if defined(HAMLET_CORE3SE)
static FtmRangeStatus mapFtmStatus(wifi_ftm_status_t status) {
    switch (status) {
        case FTM_STATUS_SUCCESS:
            return FtmRangeStatus::SUCCESS;
        case FTM_STATUS_UNSUPPORTED:
            return FtmRangeStatus::UNSUPPORTED;
        case FTM_STATUS_CONF_REJECTED:
            return FtmRangeStatus::REJECTED;
        case FTM_STATUS_NO_RESPONSE:
            return FtmRangeStatus::NO_RESPONSE;
        default:
            return FtmRangeStatus::FAILED;
    }
}

static void ftmEventHandler(void*, esp_event_base_t eventBase,
                            int32_t eventId, void* eventData) {
    if (eventBase != WIFI_EVENT || eventId != WIFI_EVENT_FTM_REPORT ||
        !eventData) {
        return;
    }

    auto* report = static_cast<wifi_event_ftm_report_t*>(eventData);
    const uint32_t nowMs = millis();
    portENTER_CRITICAL(&ftmMux);
    const bool targetMatches =
        memcmp(report->peer_mac, ftmRuntime.targetBssid, 6u) == 0;
    if (ftmRuntime.active && targetMatches) {
        ftmRuntime.status = mapFtmStatus(report->status);
        if (report->status == FTM_STATUS_SUCCESS &&
            report->dist_est > 0u) {
            // Keep one bounded calibration batch. A later explicit request
            // starts a fresh batch after 64 samples instead of overflowing.
            if (ftmRuntime.sampleCount >= 64u) {
                ftmRuntime.distanceSumCm = 0u;
                ftmRuntime.distanceSquareSumCm2 = 0u;
                ftmRuntime.sampleCount = 0u;
            }
            const uint64_t distanceCm = report->dist_est;
            ftmRuntime.distanceSumCm += distanceCm;
            ftmRuntime.distanceSquareSumCm2 += distanceCm * distanceCm;
            ++ftmRuntime.sampleCount;
            ftmRuntime.completedAtMs = nowMs == 0u ? 1u : nowMs;
        }
        ftmRuntime.active = false;
        ftmRuntime.resumeCapturePending = true;
    }
    portEXIT_CRITICAL(&ftmMux);

    // IDF 4.x transfers ownership of the optional raw report to the event
    // recipient. No raw RTT array is retained in firmware.
    if (report->ftm_report_data) {
        free(report->ftm_report_data);
        report->ftm_report_data = nullptr;
    }
}

static void serviceFtmRange(uint32_t nowMs) {
    bool timedOut = false;
    bool resumeCapture = false;
    uint8_t resumeChannel = 1u;

    portENTER_CRITICAL(&ftmMux);
    if (ftmRuntime.active &&
        nowMs - ftmRuntime.requestStartedMs > FTM_REQUEST_TIMEOUT_MS) {
        ftmRuntime.active = false;
        ftmRuntime.status = FtmRangeStatus::TIMEOUT;
        ftmRuntime.resumeCapturePending = true;
        timedOut = true;
    }
    if (ftmRuntime.resumeCapturePending) {
        ftmRuntime.resumeCapturePending = false;
        resumeCapture = true;
        resumeChannel = ftmRuntime.resumeChannel;
    }
    portEXIT_CRITICAL(&ftmMux);

    if (timedOut) {
        esp_wifi_ftm_end_session();
    }
    if (resumeCapture) {
        const esp_err_t channelErr =
            esp_wifi_set_channel(resumeChannel, WIFI_SECOND_CHAN_NONE);
        if (channelErr != ESP_OK) {
            HAMLET_LOGF("[SPECTRUM] FTM channel restore failed: %s\n",
                        esp_err_to_name(channelErr));
        }
        const esp_err_t err = esp_wifi_set_promiscuous(true);
        if (err != ESP_OK) {
            HAMLET_LOGF("[SPECTRUM] FTM capture resume failed: %s\n",
                        esp_err_to_name(err));
        }
        resetPpsWindow(nowMs);
        discardPartialMeasurementSweep(resumeChannel);
    }
}
#endif

bool startSelectedFtmRange() {
#if defined(HAMLET_CORE3SE)
    const SpectrumNetwork* net = getSelectedNetwork();
    if (!active || !clientMode || !net || !net->ftmResponder ||
        !ftmEventRegistered) {
        return false;
    }

    portENTER_CRITICAL(&ftmMux);
    if (ftmRuntime.active) {
        portEXIT_CRITICAL(&ftmMux);
        return false;
    }
    const uint8_t resumeChannel =
        currentChannel.load(std::memory_order_relaxed);
    if (memcmp(ftmRuntime.targetBssid, net->bssid, 6u) != 0) {
        ftmRuntime.distanceSumCm = 0u;
        ftmRuntime.distanceSquareSumCm2 = 0u;
        ftmRuntime.sampleCount = 0u;
        ftmRuntime.completedAtMs = 0u;
    }
    memcpy(ftmRuntime.targetBssid, net->bssid, 6u);
    ftmRuntime.status = FtmRangeStatus::REQUESTING;
    ftmRuntime.resumeChannel = resumeChannel;
    ftmRuntime.requestStartedMs = millis();
    ftmRuntime.responderAdvertised = true;
    ftmRuntime.active = true;
    ftmRuntime.resumeCapturePending = false;
    portEXIT_CRITICAL(&ftmMux);

    // FTM is intentionally active and exclusive: pause passive capture so any
    // gap is explicit instead of mixing request traffic into RF statistics.
    esp_wifi_set_promiscuous(false);
    resetPpsWindow(millis());
    discardPartialMeasurementSweep(net->channel);

    wifi_ftm_initiator_cfg_t cfg{};
    memcpy(cfg.resp_mac, net->bssid, 6u);
    cfg.channel = net->channel;
    cfg.frm_count = 16u;
    cfg.burst_period = 2u;
    const esp_err_t err = esp_wifi_ftm_initiate_session(&cfg);
    if (err == ESP_OK) {
        return true;
    }

    portENTER_CRITICAL(&ftmMux);
    ftmRuntime.active = false;
    ftmRuntime.status = FtmRangeStatus::FAILED;
    portEXIT_CRITICAL(&ftmMux);
    esp_wifi_set_channel(resumeChannel, WIFI_SECOND_CHAN_NONE);
    esp_wifi_set_promiscuous(true);
    resetPpsWindow(millis());
    discardPartialMeasurementSweep(resumeChannel);
    HAMLET_LOGF("[SPECTRUM] FTM start failed: %s\n",
                esp_err_to_name(err));
    return false;
#else
    return false;
#endif
}

bool getFtmRangeEvidence(FtmRangeEvidence& out) {
    out = {};
#if defined(HAMLET_CORE3SE)
    const SpectrumNetwork* net = getSelectedNetwork();
    if (!net) {
        out.status = FtmRangeStatus::UNAVAILABLE;
        return false;
    }

    uint64_t sum = 0u;
    uint64_t squareSum = 0u;
    uint32_t completedAtMs = 0u;
    uint8_t targetBssid[6] = {};
    portENTER_CRITICAL(&ftmMux);
    out.status = ftmRuntime.status;
    out.sampleCount = ftmRuntime.sampleCount;
    out.active = ftmRuntime.active;
    sum = ftmRuntime.distanceSumCm;
    squareSum = ftmRuntime.distanceSquareSumCm2;
    completedAtMs = ftmRuntime.completedAtMs;
    memcpy(targetBssid, ftmRuntime.targetBssid, sizeof(targetBssid));
    portEXIT_CRITICAL(&ftmMux);

    out.responderAdvertised = net->ftmResponder;
    if (memcmp(targetBssid, net->bssid, sizeof(targetBssid)) != 0) {
        // FTM evidence is AP-specific. A list selection change must not carry
        // the previous responder's range, activity, or failure state into the
        // new detail pane.
        out.status = net->ftmResponder
            ? FtmRangeStatus::READY
            : FtmRangeStatus::UNAVAILABLE;
        out.sampleCount = 0u;
        out.active = false;
        return out.responderAdvertised;
    }

    if (out.sampleCount > 0u) {
        out.distanceCm =
            static_cast<uint32_t>(sum / out.sampleCount);
        if (out.sampleCount > 1u) {
            const uint64_t varianceNumerator =
                static_cast<uint64_t>(out.sampleCount) * squareSum -
                sum * sum;
            const uint64_t varianceDenominator =
                static_cast<uint64_t>(out.sampleCount) *
                (out.sampleCount - 1u);
            const uint64_t variance =
                varianceNumerator / varianceDenominator;
            out.varianceCm2 = variance > UINT32_MAX
                ? UINT32_MAX : static_cast<uint32_t>(variance);
        }
        out.ageMs = millis() - completedAtMs;
        out.valid = out.status == FtmRangeStatus::SUCCESS &&
                    completedAtMs != 0u;
    }
    return out.responderAdvertised || out.active || out.sampleCount > 0u;
#else
    out.status = FtmRangeStatus::UNAVAILABLE;
    return false;
#endif
}

// ==[ PRIVATE FUNCTIONS ]==

static bool initPromiscuous() {
    // Recon/Xfer often leave the WiFi driver initialized after WIFI_OFF.
    // Re-init in that state trips coex/heap weirdness and can look like a brownout.
    wifi_mode_t existingMode = WIFI_MODE_NULL;
    bool driverReady = (esp_wifi_get_mode(&existingMode) == ESP_OK);
    if (!driverReady) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        esp_err_t ierr = esp_wifi_init(&cfg);
        if (ierr != ESP_OK) {
            HAMLET_LOGF("[SPECTRUM] esp_wifi_init failed: %s\n", esp_err_to_name(ierr));
            return false;
        }
        // BLE coex teardown is async on the BT controller task. If we re-init
        // WiFi before it propagates, the coex arbiter deadlocks and the board
        // brownout-resets 3-4s later (motor stays buzzing during reboot).
        delay(50);
    }

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        HAMLET_LOGF("[SPECTRUM] esp_wifi_set_mode failed: %s\n", esp_err_to_name(err));
        return false;
    }
    err = esp_wifi_start();
    if (err != ESP_OK) {
        HAMLET_LOGF("[SPECTRUM] esp_wifi_start failed: %s\n", esp_err_to_name(err));
        return false;
    }

#if defined(HAMLET_CORE3SE)
    if (!ftmEventRegistered) {
        err = esp_event_handler_instance_register(
            WIFI_EVENT, WIFI_EVENT_FTM_REPORT, &ftmEventHandler, nullptr,
            &ftmEventInstance);
        ftmEventRegistered = err == ESP_OK;
        if (!ftmEventRegistered) {
            HAMLET_LOGF("[SPECTRUM] FTM event registration failed: %s\n",
                        esp_err_to_name(err));
        } else {
            portENTER_CRITICAL(&ftmMux);
            ftmRuntime.status = FtmRangeStatus::READY;
            portEXIT_CRITICAL(&ftmMux);
        }
    }
#endif

    // ==[ POWER SAVE OFF ]== promiscuous capture needs continuous RX — no modem sleep.
    // Without this, radio sleeps between DTIM beacons and drops frames.
    // coex constraint: BLE active → WIFI_PS_NONE crashes (must use MIN_MODEM).
    Power::applyCurrentRadioSettings();

    // Register callback first
    esp_wifi_set_promiscuous_rx_cb(promiscuousCallback);

    // Set filter second
    wifi_promiscuous_filter_t filter = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT |
                       WIFI_PROMIS_FILTER_MASK_CTRL |
                       WIFI_PROMIS_FILTER_MASK_DATA
    };
    err = esp_wifi_set_promiscuous_filter(&filter);
    if (err != ESP_OK) {
        HAMLET_LOGF("[SPECTRUM] esp_wifi_set_promiscuous_filter failed: %s\n", esp_err_to_name(err));
    }

    // Enable promiscuous mode LAST (after callback and filter are registered)
    err = esp_wifi_set_promiscuous(true);
    if (err != ESP_OK) {
        HAMLET_LOGF("[SPECTRUM] esp_wifi_set_promiscuous failed: %s\n", esp_err_to_name(err));
        esp_wifi_set_promiscuous_rx_cb(NULL);
#if defined(HAMLET_CORE3SE)
        if (ftmEventRegistered) {
            esp_event_handler_instance_unregister(
                WIFI_EVENT, WIFI_EVENT_FTM_REPORT, ftmEventInstance);
            ftmEventRegistered = false;
            ftmEventInstance = nullptr;
        }
#endif
        esp_wifi_stop();
        return false;
    }

#if defined(HAMLET_WIFI_CSI)
    if (!CsiTracker::begin()) {
        HAMLET_LOGF("[SPECTRUM] CSI init failed\n");
    }
#endif

    err = esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
    if (err != ESP_OK) {
        HAMLET_LOGF("[SPECTRUM] channel set ch1 failed: 0x%x\n", err);
    } else {
        currentChannel = 1;
    }
    return true;
}

static void stopPromiscuous() {
#if defined(HAMLET_WIFI_CSI)
    CsiTracker::end();
#endif
#if defined(HAMLET_CORE3SE)
    bool ftmWasActive = false;
    portENTER_CRITICAL(&ftmMux);
    ftmWasActive = ftmRuntime.active;
    ftmRuntime.active = false;
    ftmRuntime.resumeCapturePending = false;
    portEXIT_CRITICAL(&ftmMux);
    if (ftmWasActive) {
        esp_wifi_ftm_end_session();
    }
    if (ftmEventRegistered) {
        esp_event_handler_instance_unregister(
            WIFI_EVENT, WIFI_EVENT_FTM_REPORT, ftmEventInstance);
        ftmEventRegistered = false;
        ftmEventInstance = nullptr;
    }
#endif
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(NULL);
    esp_wifi_stop();
}

static void hopChannel() {
    const uint8_t previousChannel =
        currentChannel.load(std::memory_order_relaxed);
    channelIndex = (channelIndex + 1) % CHANNEL_COUNT;
    uint8_t nextChannel = CHANNEL_ORDER[channelIndex];
    esp_err_t err = esp_wifi_set_channel(nextChannel, WIFI_SECOND_CHAN_NONE);
    if (err != ESP_OK) {
        HAMLET_LOGF("[SPECTRUM] channel set ch%d (hop) failed: 0x%x\n", nextChannel, err);
    } else {
        currentChannel = nextChannel;
        const uint32_t now = millis();
        if (rfMeasurements) {
            rfMeasurements->onChannelChange(
                previousChannel, nextChannel, now, packetEvents.drops());
        }
        currentHopIntervalMs = RfMeasurement::dwellDurationMs(
            nextChannel, rfMeasurements ? rfMeasurements->epoch() : 0u);
    }
}

static void updateChannelStats(uint8_t channel, int8_t rssi) {
    if (channel < 1 || channel > 13) return;
    
    if (channelActivity[channel] < 0xFFFFFFFFu) {
        channelActivity[channel]++;
    }

    // smooth peaks. no teleporting.
    if (rssi > channelPeakRSSI[channel]) {
        // fast rise. not instant.
        channelPeakRSSI[channel] = (channelPeakRSSI[channel] + rssi * 3) / 4;
    }
    
    // IIR filter. calm averages.
    channelAvgRSSI[channel] = RFUtil::smoothIIR(channelAvgRSSI[channel], rssi, 16);
}

static SpectrumNetwork* findOrCreateNetwork(const uint8_t* bssid) {
    if (!networks) return nullptr;
    // O(1) hash lookup
    uint16_t found = specHashFind(bssid);
    if (found != SPEC_HASH_EMPTY) return &networks[found];

    // Create new if space
    uint16_t slotIdx;
    bool evicted = false;
    if (networkCount >= MAX_SPECTRUM_NETWORKS) {
        // Preserve both user-owned identities. In the main pane the selected
        // AP owns the carrier/list focus; in THRU mode the monitored AP owns
        // the client-detail lifetime. Replacing either slot in place makes a
        // newly arrived BSSID inherit the old selection and sidecar state.
        int16_t weakestIdx = -1;
        int8_t weakestRSSI = 1;
        for (uint16_t i = 0; i < networkCount; i++) {
            const bool selected = selectedIdx == static_cast<int16_t>(i);
            const bool monitored =
                clientMode && memcmp(networks[i].bssid, monitoredBSSID, 6u) == 0;
            if (SpectrumThruMath::retainCatalogSlot(selected, monitored)) {
                continue;
            }
            if (networks[i].rssi < weakestRSSI) {
                weakestRSSI = networks[i].rssi;
                weakestIdx = i;
            }
        }
        if (weakestIdx < 0) return nullptr;
        memset(&networks[weakestIdx], 0, sizeof(SpectrumNetwork));
        slotIdx = weakestIdx;
        clearNetworkSidecars(slotIdx);
        evicted = true;
    } else {
        memset(&networks[networkCount], 0, sizeof(SpectrumNetwork));
        slotIdx = networkCount++;
        clearNetworkSidecars(slotIdx);
    }

    memcpy(networks[slotIdx].bssid, bssid, 6);
    // Rebuild only after the replacement BSSID is present. Rebuilding while
    // the slot is zeroed inserts a phantom all-zero key and can leave two hash
    // entries pointing at the same evicted slot.
    if (evicted) specHashRebuild();
    else specHashInsert(bssid, slotIdx);
    return &networks[slotIdx];
}

static uint8_t authModeFromKind(SpectrumRsnMath::AuthKind auth) {
    switch (auth) {
        case SpectrumRsnMath::AuthKind::OPEN: return WIFI_AUTH_OPEN;
        case SpectrumRsnMath::AuthKind::WEP: return WIFI_AUTH_WEP;
        case SpectrumRsnMath::AuthKind::WPA1_PSK: return WIFI_AUTH_WPA_PSK;
        case SpectrumRsnMath::AuthKind::WPA2_PSK: return WIFI_AUTH_WPA2_PSK;
        case SpectrumRsnMath::AuthKind::WPA_WPA2_PSK: return WIFI_AUTH_WPA_WPA2_PSK;
        case SpectrumRsnMath::AuthKind::WPA2_ENTERPRISE: return WIFI_AUTH_WPA2_ENTERPRISE;
        case SpectrumRsnMath::AuthKind::WPA3_SAE: return WIFI_AUTH_WPA3_PSK;
        case SpectrumRsnMath::AuthKind::WPA2_WPA3_TRANSITION: return WIFI_AUTH_WPA2_WPA3_PSK;
        case SpectrumRsnMath::AuthKind::UNKNOWN:
        default:
            return WIFI_AUTH_MAX;
    }
}

static SpectrumRsnMath::AuthKind authKindFromMode(uint8_t auth) {
    switch (auth) {
        case WIFI_AUTH_OPEN: return SpectrumRsnMath::AuthKind::OPEN;
        case WIFI_AUTH_WEP: return SpectrumRsnMath::AuthKind::WEP;
        case WIFI_AUTH_WPA_PSK: return SpectrumRsnMath::AuthKind::WPA1_PSK;
        case WIFI_AUTH_WPA2_PSK: return SpectrumRsnMath::AuthKind::WPA2_PSK;
        case WIFI_AUTH_WPA_WPA2_PSK: return SpectrumRsnMath::AuthKind::WPA_WPA2_PSK;
        case WIFI_AUTH_WPA2_ENTERPRISE: return SpectrumRsnMath::AuthKind::WPA2_ENTERPRISE;
        case WIFI_AUTH_WPA3_PSK: return SpectrumRsnMath::AuthKind::WPA3_SAE;
        case WIFI_AUTH_WPA2_WPA3_PSK: return SpectrumRsnMath::AuthKind::WPA2_WPA3_TRANSITION;
        default: return SpectrumRsnMath::AuthKind::UNKNOWN;
    }
}

static bool isSsidKnown(const SpectrumNetwork& net) {
    if (!net.ssid[0] || strcmp(net.ssid, "<hidden>") == 0) return false;
    return !net.isHidden || net.wasRevealed;
}

static void applyNetworkSecurity(SpectrumNetwork& net,
                                 const SpectrumRsnMath::SecurityEvidence& evidence) {
    const SpectrumRsnMath::AuthKind auth =
        SpectrumRsnMath::classify(evidence);
    net.authmode = authModeFromKind(auth);
    // Attackability requires PMF to be required, not merely advertised.
    net.hasPMF = evidence.sawRsn && evidence.rsn.valid &&
                 evidence.rsn.pmfRequired;
}

static void handleBeacon(const uint8_t* payload, uint16_t len, int8_t rssi,
                         uint8_t sigMode, uint8_t phyRate, uint8_t cwb,
                         uint32_t rxTimestamp, uint32_t observedMs) {
    if (len < 36) return;

    const uint8_t* bssid = payload + 16;

    SpectrumNetwork* net = findOrCreateNetwork(bssid);
    if (!net) return;
    bool isNew = (net->lastSeen == 0);

    memcpy(net->bssid, bssid, 6);

    // smooth RSSI. 3:1 IIR. hopping starves samples.
    net->rssi = isNew ? rssi : RFUtil::smoothIIR(net->rssi, rssi, 4);

    net->lastSeen = observedMs;

    // ==[ PHY FINGERPRINT ]== classify device generation from rx_ctrl metadata
    if (sigMode == 1)      net->phyMode = 3;  // HT = 802.11n
    else if (sigMode == 3) net->phyMode = 4;  // VHT = 802.11ac (rare at 2.4GHz)
    else                   net->phyMode = (phyRate <= 3) ? 1 : 2;  // 11b vs 11g
    net->channelWidth = cwb;

    // ==[ BEACON INTERVAL MEASUREMENT ]== from rx_ctrl.timestamp delta
    uint16_t slotIdx = (uint16_t)(net - networks);
    if (beaconPrevTimestamp && beaconIntervalTU && slotIdx < MAX_SPECTRUM_NETWORKS) {
        uint32_t prev = beaconPrevTimestamp[slotIdx];
        if (prev != 0) {
            uint32_t delta = rxTimestamp - prev;  // wraps correctly for uint32
            // only accept single-interval deltas (80-500 TU = ~82-512ms)
            // channel hopping causes multi-beacon gaps that corrupt the measurement
            if (delta > 81920 && delta < 512000) {  // 80*1024 .. 500*1024 us
                uint16_t measuredTU = (uint16_t)(delta / 1024);
                uint16_t existing = beaconIntervalTU[slotIdx];
                if (existing == 0) {
                    beaconIntervalTU[slotIdx] = measuredTU;
                } else {
                    // 3:1 IIR smooth
                    beaconIntervalTU[slotIdx] = (uint16_t)((existing * 3 + measuredTU) / 4);
                }
            }
        }
        beaconPrevTimestamp[slotIdx] = rxTimestamp;
    }
    
    // Parse each IE once. DS Parameter Set is more reliable than currentChannel
    // because the callback may run after a hop.
    uint8_t beaconChannel = currentChannel;  // Fallback
    SpectrumRsnMath::SecurityEvidence security{};
    security.privacy = (payload[34] & 0x10u) != 0u;
    bool ftmResponder = false;
    uint16_t pos = 36;
    while (pos + 2 <= len) {
        uint8_t ieType = payload[pos];
        uint8_t ieLen = payload[pos + 1];
        
        if (pos + 2 + ieLen > len) break;
        
        switch (ieType) {
            case 0x03:  // DS Parameter Set
                if (ieLen == 1u) {
                    const uint8_t advertisedChannel = payload[pos + 2u];
                    if (advertisedChannel >= 1u && advertisedChannel <= 13u) {
                        beaconChannel = advertisedChannel;
                    }
                }
                break;

            case 0x00:  // SSID
                if (ieLen > 0 && ieLen <= 32) {
                    // check nulls on raw payload BEFORE memcpy (don't nuke revealed SSIDs)
                    bool allNulls = true;
                    for (uint8_t j = 0; j < ieLen; j++) {
                        if (payload[pos + 2 + j] != '\0') { allNulls = false; break; }
                    }
                    if (allNulls) {
                        net->isHidden = true;
                        if (!net->wasRevealed) {
                            strncpy(net->ssid, "<hidden>", sizeof(net->ssid) - 1);
                            net->ssid[sizeof(net->ssid) - 1] = '\0';
                        }
                    } else {
                        // real SSID — always update (AP may have renamed)
                        memcpy(net->ssid, payload + pos + 2, ieLen);
                        net->ssid[ieLen] = '\0';
                        net->isHidden = false;
                    }
                } else if (ieLen == 0) {
                    net->isHidden = true;
                    if (!net->wasRevealed) {
                        strncpy(net->ssid, "<hidden>", sizeof(net->ssid) - 1);
                        net->ssid[sizeof(net->ssid) - 1] = '\0';
                    }
                }
                break;

            case 0x30:  // RSN
            case 0xDD:  // vendor-specific; may carry legacy WPA
                SpectrumRsnMath::observe(
                    security, ieType, payload + pos + 2u, ieLen);
                break;

            case 0x7F:  // Extended Capabilities: FTM responder is bit 70
                ftmResponder =
                    ieLen >= 9u && (payload[pos + 2u + 8u] & 0x40u) != 0u;
                break;

            default:
                break;
        }
        pos += 2 + ieLen;
    }
    net->channel = beaconChannel;
    net->ftmResponder = net->ftmResponder || ftmResponder;
    applyNetworkSecurity(*net, security);

    // Defer channel stats recount to update() (avoid O(n) loop in callback).
    pendingChannelRecount = beaconChannel;

    // Notify on new network occasionally — defer to update() (not callback-safe)
    if (isNew && (esp_random() % 10) == 0) {
        pendingNewNetworkMood = true;
    }
}

static void handleProbeResponse(const uint8_t* payload, uint16_t len,
                                int8_t rssi, uint32_t observedMs) {
    // Similar to beacon - reveals hidden SSIDs
    if (len < 36) return;
    
    const uint8_t* bssid = payload + 16;
    SpectrumNetwork* net = findOrCreateNetwork(bssid);
    if (!net) return;
    bool isNew = (net->lastSeen == 0);

    // 3:1 IIR like beacon
    net->rssi = isNew ? rssi : RFUtil::smoothIIR(net->rssi, rssi, 4);
    net->lastSeen = observedMs;
    
    uint8_t responseChannel = currentChannel;
    SpectrumRsnMath::SecurityEvidence security{};
    security.privacy = (payload[34] & 0x10u) != 0u;
    bool ftmResponder = false;

    // Probe responses carry the same channel/security IEs as beacons. Keep
    // scanning after SSID so a probe-only observation cannot remain OPEN.
    uint16_t pos = 36;
    while (pos + 2 <= len) {
        uint8_t ieType = payload[pos];
        uint8_t ieLen = payload[pos + 1];
        
        if (pos + 2 + ieLen > len) break;
        
        if (ieType == 0x03u && ieLen == 1u) {
            const uint8_t advertisedChannel = payload[pos + 2u];
            if (advertisedChannel >= 1u && advertisedChannel <= 13u) {
                responseChannel = advertisedChannel;
            }
        } else if (ieType == 0x00u && ieLen > 0u && ieLen <= 32u) {
            memcpy(net->ssid, payload + pos + 2, ieLen);
            net->ssid[ieLen] = '\0';
            if (net->isHidden) {
                net->wasRevealed = true;
            }
        } else if (ieType == 0x30u || ieType == 0xDDu) {
            SpectrumRsnMath::observe(
                security, ieType, payload + pos + 2u, ieLen);
        } else if (ieType == 0x7Fu) {
            ftmResponder =
                ieLen >= 9u && (payload[pos + 2u + 8u] & 0x40u) != 0u;
        }
        pos += 2 + ieLen;
    }
    net->channel = responseChannel;
    net->ftmResponder = net->ftmResponder || ftmResponder;
    applyNetworkSecurity(*net, security);
    pendingChannelRecount = responseChannel;
}

static void handleDataFrame(const uint8_t* payload, uint16_t len, int8_t rssi,
                            int8_t noiseFloor, uint32_t rxTimestampUs,
                            uint32_t observedMs) {
    // Extract source/dest to track clients
    if (len < 24) return;

    const SpectrumThruMath::DataFrameRoute route =
        SpectrumThruMath::routeDataFrame(payload[1]);
    if (!route.valid) return;
    const uint8_t* bssid = payload + route.bssidOffset;
    const uint8_t* clientMac = payload + route.clientOffset;
    const bool clientTx = route.clientTransmitted;

    // Find network via hash
    uint16_t ni = specHashFind(bssid);
    if (ni == SPEC_HASH_EMPTY) return;
    SpectrumNetwork* net = &networks[ni];

    // Skip broadcast/multicast
    if (clientMac[0] & 0x01) return;

    // SNR = RSSI - noise_floor. more stable in crowded spectrum.
    int8_t snr = rssi - noiseFloor;

    // Find or add client
    for (uint8_t i = 0; i < net->clientCount; i++) {
        if (memcmp(net->clients[i].mac, clientMac, 6) == 0) {
            net->clients[i].lastSeen = observedMs;
            if (clientTx) {
                // FromDS RSSI belongs to the AP. Never poison client bearing.
                if (!net->clients[i].hasSignal) {
                    net->clients[i].rssi = rssi;
                    net->clients[i].snr = snr;
                } else {
                    net->clients[i].rssi = RFUtil::smoothIIR(
                        net->clients[i].rssi, rssi, 4);
                    net->clients[i].snr = RFUtil::smoothIIR(
                        net->clients[i].snr, snr, 4);
                }
                net->clients[i].lastSignalSeen = observedMs;
                net->clients[i].lastSignalRxUs = rxTimestampUs;
                net->clients[i].hasSignal = true;
            }
            return;
        }
    }

    // Add new client
    if (net->clientCount < MAX_SPECTRUM_CLIENTS) {
        memcpy(net->clients[net->clientCount].mac, clientMac, 6);
        net->clients[net->clientCount].rssi = clientTx ? rssi : -95;
        net->clients[net->clientCount].snr = clientTx ? snr : 0;
        net->clients[net->clientCount].lastSeen = observedMs;
        net->clients[net->clientCount].lastSignalSeen =
            clientTx ? observedMs : 0u;
        net->clients[net->clientCount].lastSignalRxUs =
            clientTx ? rxTimestampUs : 0u;
        net->clients[net->clientCount].hasSignal = clientTx;
        net->clientCount++;
        
        // Chime only the first few arrivals on the locked network; a crowded
        // AP should become evidence, not an alarm clock.
        if (clientMode && memcmp(net->bssid, monitoredBSSID, 6) == 0) {
            if (clientsDiscovered < CLIENT_BEEP_LIMIT) {
                pendingClientBeep = true;
                clientsDiscovered++;
            }
            // defer sniff — promiscuous callback context
            pendingSniff = true;
        }
    }
}

static void cleanupStale() {
    uint32_t now = millis();
    
    // Cleanup networks
    for (uint16_t i = 0; i < networkCount; ) {
        const bool monitored =
            clientMode &&
            memcmp(networks[i].bssid, monitoredBSSID, 6u) == 0;
        if (!monitored &&
            SpectrumThruMath::observationAgeMs(
                now, networks[i].lastSeen) > NETWORK_TIMEOUT) {
            // Remove by shifting
            if (i < networkCount - 1) {
                memmove(&networks[i], &networks[i + 1], 
                        (networkCount - i - 1) * sizeof(SpectrumNetwork));
                for (uint16_t j = i; j < networkCount - 1; j++) {
                    moveNetworkSidecars(j, j + 1);
                }
            }
            networkCount--;
            clearNetworkSidecars(networkCount);
            if (selectedIdx >= networkCount && networkCount > 0) {
                selectedIdx = networkCount - 1;
            }
        } else {
            // Cleanup stale clients
            for (uint8_t c = 0; c < networks[i].clientCount; ) {
                const bool selectedClient =
                    monitored && selectedClientIdentityValid &&
                    memcmp(networks[i].clients[c].mac,
                           selectedClientMac, 6u) == 0;
                if (!selectedClient &&
                    SpectrumThruMath::observationAgeMs(
                        now, networks[i].clients[c].lastSeen) >
                            NETWORK_TIMEOUT) {
                    if (c < networks[i].clientCount - 1) {
                        memmove(&networks[i].clients[c], 
                                &networks[i].clients[c + 1],
                                (networks[i].clientCount - c - 1) * sizeof(SpectrumClient));
                    }
                    networks[i].clientCount--;
                } else {
                    c++;
                }
            }
            if (monitored) {
                selectedIdx = (int16_t)i;
                if (networks[i].clientCount == 0u) {
                    selectedClientIdx = 0;
                } else {
                    rebindSelectedClientIdentity();
                }
            }
            i++;
        }
    }
    // memmove shifts indices — rebuild hash table
    specHashRebuild();
}

// ==[ HUNTING PRIORITY SCORE ]==
// Higher = better target for client hunting
static int getHuntingScore(const SpectrumNetwork& net) {
    // ==[ TIER-ENHANCED SCORING ]== use vulnerability classification when available
    uint16_t idx = (uint16_t)(&net - networks);
    if (vulnTier && readinessScore && idx < networkCount) {
        // tier-based base: lower tier = higher score
        static const int16_t tierBase[] = {1500, 1200, 1000, 900, 800, 600, 300, 100};
        int score = tierBase[min(vulnTier[idx], (uint8_t)7)];
        score += readinessScore[idx] * 2;  // 0-200 readiness bonus
        if (net.clientCount > 0) score += 500 + net.clientCount * 50;
        score += (net.rssi + 100);  // RSSI tiebreaker
        return score;
    }

    // fallback: original logic (before first classification cycle)
    int score = 0;
    bool isDeauthable = !net.hasPMF &&
        (net.authmode == WIFI_AUTH_WPA_PSK ||
         net.authmode == WIFI_AUTH_WPA2_PSK ||
         net.authmode == WIFI_AUTH_WPA_WPA2_PSK ||
         net.authmode == WIFI_AUTH_WPA2_WPA3_PSK);
    if (isDeauthable) score += 1000;
    if (net.authmode == WIFI_AUTH_WPA3_PSK && net.hasPMF &&
        Config::getSAEAttackEnabled()) {
        score += 600;
    }
    if (net.clientCount > 0) score += 500 + net.clientCount * 50;
    if (net.authmode == WIFI_AUTH_OPEN) score += 200;
    score += (net.rssi + 100);
    return score;
}

static void sortNetworksByRSSI() {
    // Don't sort if empty or single element
    if (networkCount < 2) return;
    
    // Remember selected BSSID to restore index after sort
    uint8_t selectedBSSID[6] = {0};
    bool hadSelection = false;
    if (selectedIdx >= 0 && selectedIdx < (int16_t)networkCount) {
        memcpy(selectedBSSID, networks[selectedIdx].bssid, 6);
        hadSelection = true;
    }
    
    // pre-calculate scores once: O(n) instead of O(n²) recalc
    int scores[MAX_SPECTRUM_NETWORKS];
    for (uint16_t i = 0; i < networkCount; i++) {
        scores[i] = getHuntingScore(networks[i]);
    }

    // insertion sort (stable, O(n) for nearly-sorted data typical after cleanup)
    // sidecar arrays travel with their network. no orphan intel.
    int8_t tmpSparkline[SPARKLINE_SAMPLES];
    uint8_t tmpSparkIdx = 0;
    uint32_t tmpBeaconTs = 0;
    uint16_t tmpBeaconInterval = 0;
    uint8_t tmpBeaconAnom = 0;
    uint8_t tmpTier = 7;
    uint8_t tmpReady = 0;
    uint8_t tmpCluster = 0;
    for (uint16_t i = 1; i < networkCount; i++) {
        SpectrumNetwork tmpNet = networks[i];
        int tmpScore = scores[i];
        if (sparklineBuffers) memcpy(tmpSparkline, sparklineBuffers + i * SPARKLINE_SAMPLES, SPARKLINE_SAMPLES);
        if (sparklineIdx) tmpSparkIdx = sparklineIdx[i];
        if (beaconPrevTimestamp) tmpBeaconTs = beaconPrevTimestamp[i];
        if (beaconIntervalTU) tmpBeaconInterval = beaconIntervalTU[i];
        if (beaconAnomaly) tmpBeaconAnom = beaconAnomaly[i];
        if (vulnTier) tmpTier = vulnTier[i];
        if (readinessScore) tmpReady = readinessScore[i];
        if (networkClusterId) tmpCluster = networkClusterId[i];
        int16_t j = i - 1;
        while (j >= 0 && scores[j] < tmpScore) {
            networks[j + 1] = networks[j];
            scores[j + 1] = scores[j];
            moveNetworkSidecars(j + 1, j);
            j--;
        }
        networks[j + 1] = tmpNet;
        scores[j + 1] = tmpScore;
        if (sparklineBuffers) memcpy(sparklineBuffers + (j + 1) * SPARKLINE_SAMPLES, tmpSparkline, SPARKLINE_SAMPLES);
        if (sparklineIdx) sparklineIdx[j + 1] = tmpSparkIdx;
        if (beaconPrevTimestamp) beaconPrevTimestamp[j + 1] = tmpBeaconTs;
        if (beaconIntervalTU) beaconIntervalTU[j + 1] = tmpBeaconInterval;
        if (beaconAnomaly) beaconAnomaly[j + 1] = tmpBeaconAnom;
        if (vulnTier) vulnTier[j + 1] = tmpTier;
        if (readinessScore) readinessScore[j + 1] = tmpReady;
        if (networkClusterId) networkClusterId[j + 1] = tmpCluster;
    }
    
    // sort shifted indices — rebuild hash
    specHashRebuild();

    // Auto-select best target if nothing selected or in dial mode
    if (!hadSelection || dialMode) {
        selectedIdx = 0;  // Top network is best hunting target
    } else {
        // Restore selectedIdx via hash
        uint16_t si = specHashFind(selectedBSSID);
        if (si != SPEC_HASH_EMPTY) selectedIdx = si;
    }
}

// ==[ INTELLIGENCE: PROBE REQUEST HARVESTING ]==
// capture client preferred network lists. callback-safe: memcpy to staging only.

static void handleProbeRequest(const uint8_t* payload, uint16_t len, int8_t rssi) {
    if (len < 36) return;
    const uint8_t* clientMac = payload + 10;  // SA (source address, Addr2)
    // skip broadcast/multicast
    if (clientMac[0] & 0x01) return;
    // skip randomized MACs (locally-administered bit = useless for tracking)
    if (clientMac[0] & 0x02) return;

    // parse SSID IE (0x00)
    uint16_t pos = 24;
    while (pos + 2 <= len) {
        uint8_t ieType = payload[pos];
        uint8_t ieLen = payload[pos + 1];
        if (pos + 2 + ieLen > len) break;

        if (ieType == 0x00 && ieLen > 0 && ieLen <= 32) {
            // stage for main loop processing (callback-safe: memcpy only)
            if (!pendingProbe.ready) {
                memcpy(pendingProbe.clientMAC, clientMac, 6);
                memcpy(pendingProbe.ssid, payload + pos + 2, ieLen);
                pendingProbe.ssid[ieLen] = '\0';
                pendingProbe.rssi = rssi;
                pendingProbe.ready = true;
            }
            return;
        }
        pos += 2 + ieLen;
    }
}

// dedup + store staged probe entry into PSRAM table. called from update().
static void processProbeEntry() {
    if (!probeTable) return;
    // dedup: check if this MAC+SSID pair already exists
    for (uint16_t i = 0; i < probeCount; i++) {
        if (memcmp(probeTable[i].clientMAC, pendingProbe.clientMAC, 6) == 0 &&
            strcmp(probeTable[i].ssid, pendingProbe.ssid) == 0) {
            // update RSSI and timestamp
            probeTable[i].rssi = pendingProbe.rssi;
            probeTable[i].lastSeen = millis();
            return;
        }
    }
    // new entry
    if (probeCount < MAX_SPECTRUM_PROBES) {
        SpectrumProbeEntry& e = probeTable[probeCount++];
        memcpy(e.clientMAC, pendingProbe.clientMAC, 6);
        strncpy(e.ssid, pendingProbe.ssid, 32);
        e.ssid[32] = '\0';
        e.rssi = pendingProbe.rssi;
        e.lastSeen = millis();
    } else {
        // LRU eviction: replace oldest entry
        uint32_t oldest = UINT32_MAX;
        uint16_t oldIdx = 0;
        for (uint16_t i = 0; i < probeCount; i++) {
            if (probeTable[i].lastSeen < oldest) {
                oldest = probeTable[i].lastSeen;
                oldIdx = i;
            }
        }
        SpectrumProbeEntry& e = probeTable[oldIdx];
        memcpy(e.clientMAC, pendingProbe.clientMAC, 6);
        strncpy(e.ssid, pendingProbe.ssid, 32);
        e.ssid[32] = '\0';
        e.rssi = pendingProbe.rssi;
        e.lastSeen = millis();
    }
}

// ==[ INTELLIGENCE: VULNERABILITY CLASSIFICATION ]==
// 8-tier system: T0(OPEN) → T7(FORTRESS). runs in cleanup cycle.

static char getTierIcon(uint8_t tier) {
    static const char icons[] = {'O','!','*','P','D','S','E','#'};
    return (tier <= 7) ? icons[tier] : '?';
}

static const char* formatAuthShort(uint8_t authMode) {
    switch (authMode) {
        case WIFI_AUTH_OPEN:          return "OPEN";
        case WIFI_AUTH_WEP:           return "WEP";
        case WIFI_AUTH_WPA_PSK:       return "WPA";
        case WIFI_AUTH_WPA2_PSK:      return "WPA2";
        case WIFI_AUTH_WPA_WPA2_PSK:  return "WPA+2";
        case WIFI_AUTH_WPA2_ENTERPRISE: return "ENT";
        case WIFI_AUTH_WPA3_PSK:      return "WPA3";
        case WIFI_AUTH_WPA2_WPA3_PSK: return "W2+3";
        default:                      return "???";
    }
}

static void classifyNetworks() {
    if (!vulnTier || !readinessScore) return;
    for (uint16_t i = 0; i < networkCount; i++) {
        const SpectrumNetwork& net = networks[i];
        uint8_t tier;
        uint8_t score = 0;

        if (net.authmode == WIFI_AUTH_OPEN) {
            tier = 0; score = 95;
        } else if (net.authmode == WIFI_AUTH_WEP) {
            tier = 1; score = 90;
        } else if (net.authmode == WIFI_AUTH_WPA_PSK) {
            tier = 1; score = 85;
        } else if (!net.hasPMF && (net.authmode == WIFI_AUTH_WPA2_PSK ||
                                    net.authmode == WIFI_AUTH_WPA_WPA2_PSK)) {
            tier = 2; score = 70;
        } else if (net.authmode == WIFI_AUTH_WPA2_WPA3_PSK) {
            tier = 4; score = net.hasPMF ? 40 : 60;
        } else if (net.authmode == WIFI_AUTH_WPA3_PSK && net.hasPMF) {
            tier = Config::getSAEAttackEnabled() ? 5 : 6;
            score = Config::getSAEAttackEnabled() ? 25 : 10;
        } else if (net.authmode == WIFI_AUTH_WPA2_PSK && net.hasPMF) {
            tier = 6; score = 10;  // WPA2+PMF = hard
        } else {
            tier = 7; score = 5;
        }

        // readiness modifiers
        if (net.clientCount > 0 && tier <= 4) score = min(100, score + 15);
        if (net.clientCount >= 3)             score = min(100, score + 5);
        if (net.rssi > READINESS_RSSI_STRONG)  score = min(100, score + 10);
        else if (net.rssi > READINESS_RSSI_GOOD) score = min(100, score + 5);
        else if (net.rssi < READINESS_RSSI_WEAK)  score = max(0, score - 10);

        vulnTier[i] = tier;
        readinessScore[i] = score;
    }
}

// ==[ INTELLIGENCE: ROGUE AP / EVIL TWIN DETECTION ]==
// O(n²) SSID collision scan. n=48 max → 1128 comparisons → microseconds.

static bool isRogueSSID(uint16_t netIdx) {
    if (!rogueAlerts || !networks || netIdx >= networkCount) return false;
    for (uint8_t i = 0; i < rogueAlertCount; i++) {
        if (memcmp(rogueAlerts[i].bssid1, networks[netIdx].bssid, 6) == 0 ||
            memcmp(rogueAlerts[i].bssid2, networks[netIdx].bssid, 6) == 0) {
            return rogueAlerts[i].severity >= 2;
        }
    }
    return false;
}

static void detectRogueAPs() {
    rogueAlertCount = 0;
    if (!rogueAlerts || !networks) return;
    for (uint16_t i = 0; i < networkCount && rogueAlertCount < MAX_ROGUE_ALERTS; i++) {
        if (!isSsidKnown(networks[i])) continue;
        for (uint16_t j = i + 1; j < networkCount; j++) {
            if (strcmp(networks[i].ssid, networks[j].ssid) != 0) continue;
            if (memcmp(networks[i].bssid, networks[j].bssid, 6) == 0) continue;
            // same SSID, different BSSID — classify severity
            RogueAlert& a = rogueAlerts[rogueAlertCount];
            memcpy(a.bssid1, networks[i].bssid, 6);
            memcpy(a.bssid2, networks[j].bssid, 6);
            strncpy(a.ssid, networks[i].ssid, 32);
            a.ssid[32] = '\0';
            a.detectedAt = millis();
            // A security-profile mismatch is high-confidence evidence worth
            // investigating, but same-SSID collisions do not prove malice.
            const int16_t profileI = SpectrumRsnMath::securityProfile(
                authKindFromMode(networks[i].authmode), networks[i].hasPMF);
            const int16_t profileJ = SpectrumRsnMath::securityProfile(
                authKindFromMode(networks[j].authmode), networks[j].hasPMF);
            if (profileI >= 0 && profileJ >= 0 && profileI != profileJ) {
                a.severity = 3;  // CRITICAL: auth or PMF mismatch
            } else if (memcmp(networks[i].bssid, networks[j].bssid, 3) != 0) {
                a.severity = 2;  // HIGH: different OUI = different hardware
            } else if (networks[j].channel != networks[i].channel) {
                a.severity = 1;  // MEDIUM: different channel = possible roaming
            } else {
                a.severity = 0;  // LOW: same security + OUI = probably mesh
            }
            rogueAlertCount++;
            if (rogueAlertCount >= MAX_ROGUE_ALERTS) break;
        }
    }
}

// ==[ INTELLIGENCE: BSSID CLUSTERING ]==
// same OUI + sequential last byte = same physical AP, multiple virtual SSIDs

static void buildBSSIDClusters() {
    if (!clusters || !networkClusterId) return;
    clusterCount = 0;
    memset(networkClusterId, 0, MAX_SPECTRUM_NETWORKS);
    for (uint16_t i = 0; i < networkCount && clusterCount < MAX_BSSID_CLUSTERS; i++) {
        if (networkClusterId[i] != 0) continue;
        BSSIDCluster& c = clusters[clusterCount];
        memcpy(c.oui, networks[i].bssid, 3);
        c.memberCount = 1;
        c.memberIndices[0] = i;
        for (uint16_t j = i + 1; j < networkCount; j++) {
            if (networkClusterId[j] != 0) continue;
            if (memcmp(networks[i].bssid, networks[j].bssid, 3) != 0) continue;
            // check last 3 bytes within ±4 (virtual SSIDs on same AP)
            int32_t diff = abs((int32_t)networks[i].bssid[5] - (int32_t)networks[j].bssid[5]);
            if (diff <= 4 && c.memberCount < 6) {
                c.memberIndices[c.memberCount++] = j;
                networkClusterId[j] = clusterCount + 1;
            }
        }
        if (c.memberCount > 1) {
            networkClusterId[i] = clusterCount + 1;
            clusterCount++;
        }
    }
}

// ==[ INTELLIGENCE: BEACON INTERVAL ANOMALY ]==
// measure inter-beacon timing from rx_ctrl.timestamp. standard = 100 TU (102400 us).

static void updateBeaconAnomalies() {
    if (!beaconIntervalTU || !beaconAnomaly) return;
    for (uint16_t i = 0; i < networkCount; i++) {
        uint16_t interval = beaconIntervalTU[i];
        if (interval == 0) { beaconAnomaly[i] = 0; continue; }
        // standard: 100 TU. flag if >8% deviation (allow some jitter)
        int16_t deviation = abs((int16_t)interval - 100);
        beaconAnomaly[i] = (deviation > 8) ? 1 : 0;
    }
}

// ==[ INTELLIGENCE: RSSI SPARKLINES ]==
// per-network RSSI micro-history, sampled at 500ms intervals

static void updateSparklines() {
    uint32_t now = millis();
    if (now - lastSparklineUpdate < SPARKLINE_INTERVAL_MS) return;
    lastSparklineUpdate = now;
    if (!sparklineBuffers || !sparklineIdx) return;
    for (uint16_t i = 0; i < networkCount; i++) {
        uint8_t& idx = sparklineIdx[i];
        sparklineBuffers[i * SPARKLINE_SAMPLES + idx] = networks[i].rssi;
        idx = (idx + 1) % SPARKLINE_SAMPLES;
    }
}

// ==[ INTELLIGENCE: PUBLIC API ]==

uint16_t getProbeCount() { return probeCount; }
const SpectrumProbeEntry* getProbeEntries() { return probeTable; }
const uint8_t* getVulnTiers() { return vulnTier; }
const uint8_t* getReadinessScores() { return readinessScore; }
uint8_t getClusterCount() { return clusterCount; }
uint8_t getRogueAlertCount() { return rogueAlertCount; }
const RogueAlert* getRogueAlerts() { return rogueAlerts; }

// ==[ DIAL MODE: GYRO CHANNEL SELECTION ]==

// Convert channel to frequency (MHz)
static uint16_t channelToFreq(uint8_t ch) {
    if (ch >= 1 && ch <= 13) return FREQ_BASE + ch * FREQ_STEP;
    if (ch == 14) return FREQ_CH14;
    return FREQ_DEFAULT;  // default to ch6
}

static void updateDialChannel() {
    uint32_t now = millis();
    
    // ==[ PPS UPDATE ]== once per second
    const uint32_t ppsWindowMs = now - lastPpsUpdate;
    if (ppsWindowMs >= 1000u) {
        const uint32_t packets =
            ppsCounter.exchange(0u, std::memory_order_acq_rel);
        displayPps = static_cast<uint32_t>(
            (static_cast<uint64_t>(packets) * 1000u +
             ppsWindowMs / 2u) / ppsWindowMs);
        lastPpsUpdate = now;
    }

    // Client tracking still needs the PPS clock above; only the tilt/dial
    // channel-selection path is disabled in its fixed-channel view.
    if (clientMode) return;

    if (!Pedometer::hasIMU()) {
        // CoreS3 SE uses an optional Bottom2 IMU. If that runtime probe is
        // absent, the neutral 1 g fallback must not become a tilt command.
        dialMode = false;
        dialLocked = false;
        return;
    }
    
    // ==[ READ IMU ]== from pedometer cache (single source of truth)
    float ax, ay, az;
    Pedometer::getCachedAccel(ax, ay, az);

    // ==[ AUTO FLT/UPS MODE SWITCH ]== uses unified hysteresis from pedometer
    // FLT (flat): normal spectrum mode, auto-hopping
    // UPS (upright): dial mode activates, accelerometer controls channel
    bool deviceFlat = Pedometer::isCachedFlat();

    if (deviceFlat) {
        // Device flat - disable dial mode, return to normal hopping
        if (dialMode) {
            dialMode = false;
            discardPartialMeasurementSweep(
                currentChannel.load(std::memory_order_relaxed));
            // Don't change channel - let normal hopping resume
        }
        return;  // No dial update when flat
    } else {
        // Device upright - enable dial mode (if setting enabled)
        if (!dialMode && Config::getSpectrumTiltEnabled()) {
            dialMode = true;
            discardPartialMeasurementSweep(
                currentChannel.load(std::memory_order_relaxed));
            // Initialize smooth position to current channel
            dialPositionSmooth = (float)currentChannel;
            dialPositionTarget = dialPositionSmooth;
        }
    }

    // ==[ GUARD ]== no tilt processing unless dial mode active
    if (!dialMode) return;

    // ==[ DIAL LOCKED ]== skip gyro reading but keep channel
    if (dialLocked) {
        // Keep channel locked
        if (currentChannel != dialChannel) {
            esp_err_t err = esp_wifi_set_channel(dialChannel, WIFI_SECOND_CHAN_NONE);
            if (err == ESP_OK) {
                currentChannel = dialChannel;
                resetPpsWindow(now);
                discardPartialMeasurementSweep(dialChannel);
            } else {
                HAMLET_LOGF("[SPECTRUM] channel set ch%d (dial locked) failed: 0x%x\n", dialChannel, err);
            }
        }
        return;
    }
    
    // ==[ LANDSCAPE UPRIGHT JOG CONTROL ]==
    // JOG WHEEL behavior - tilt to scroll channels, level to stop.
    // Landscape mode: A button on RIGHT, screen facing you, device upright
    //
    // MPU6886 landscape frame: gravity on X, tilt component on Y (ay+ toward A)
    //
    const float DEADZONE = 0.08f;      // slightly larger deadzone for stability
    const float SCROLL_SPEED = 20.0f;  // smooth scrolling

    // Tilt right (toward A-button) = higher channels
    // Tilt component on Y (MPU6886): tilt toward A = ay positive
    float tilt = ay;
    
    // ==[ DISPLAY ROTATION COMPENSATION ]==
    // If display is rotated 180°, user holds device with A-button on LEFT
    // Tilt direction is inverted from user's perspective
    if (Config::getDisplayRotate180()) {
        tilt = -tilt;
    }
    
    // Apply deadzone
    if (fabsf(tilt) < DEADZONE) {
        tilt = 0.0f;
    } else {
        // Remove deadzone from value, preserve sign
        tilt = (tilt > 0) ? (tilt - DEADZONE) : (tilt + DEADZONE);
    }
    
    // Clamp to ±1 range (values beyond ±1g are extreme)
    tilt = constrain(tilt, -1.0f, 1.0f);
    
    // Calculate time delta
    float dt = (now - lastDialUpdate) / 1000.0f;
    if (dt > 0.1f) dt = 0.1f;  // cap to avoid jumps after pause
    if (dt < 0.001f) dt = 0.016f;  // minimum ~60fps equivalent
    
    // Apply scroll: tilt controls velocity
    // Positive tilt (tilt right, A-button down) → higher channels
    // Negative tilt (tilt left, A-button up) → lower channels
    dialPositionTarget += tilt * SCROLL_SPEED * dt;
    dialPositionTarget = constrain(dialPositionTarget, 1.0f, 13.0f);
    
    // ==[ SMOOTH INTERPOLATION ]== faster lerp for responsiveness
    dialPositionSmooth += (dialPositionTarget - dialPositionSmooth) * 0.3f;
    
    // ==[ CHANNEL FROM SMOOTH POSITION ]== rounded integer
    int newChannel = (int)roundf(dialPositionSmooth);
    newChannel = constrain(newChannel, 1, 13);  // WiFi channels 1-13 only
    
    // ==[ UPDATE CHANNEL IF CHANGED ]==
    if (newChannel != dialChannel) {
        dialChannel = newChannel;
        esp_err_t err = esp_wifi_set_channel(dialChannel, WIFI_SECOND_CHAN_NONE);
        if (err == ESP_OK) {
            currentChannel = dialChannel;
            resetPpsWindow(now);
            discardPartialMeasurementSweep(dialChannel);
            SFX::click();  // tick sound on channel change
        } else {
            HAMLET_LOGF("[SPECTRUM] channel set ch%d (dial update) failed: 0x%x\n", dialChannel, err);
        }
    }

    // Ensure we stay on dial channel
    if (currentChannel != dialChannel) {
        esp_err_t err = esp_wifi_set_channel(dialChannel, WIFI_SECOND_CHAN_NONE);
        if (err == ESP_OK) {
            currentChannel = dialChannel;
            resetPpsWindow(now);
            discardPartialMeasurementSweep(dialChannel);
        } else {
            HAMLET_LOGF("[SPECTRUM] channel set ch%d (dial ensure) failed: 0x%x\n", dialChannel, err);
        }
    }
    
    lastDialUpdate = now;
}

// ==[ PARANOID SWINE: DEAUTH + GEIGER ]==

static void handleDeauthFrame(const uint8_t* payload, uint16_t len, int8_t rssi, uint8_t rxChannel) {
    // Defer PARANOID SWINE processing to main loop (UI/Geiger not
    // callback-safe). Pack count + strongest observation into one atomic word
    // so bursts cannot collapse into one event or tear RSSI/channel payloads.
    uint32_t observed = pendingDeauthBatch.load(std::memory_order_relaxed);
    uint32_t desired;
    do {
        desired = SpectrumDeauthMath::accumulate(observed, rssi, rxChannel);
    } while (!pendingDeauthBatch.compare_exchange_weak(
        observed, desired, std::memory_order_release,
        std::memory_order_relaxed));

    // ==[ FORENSIC FEED ]== relay to Recon for threat scoring + evidence chain
    // callback-safe: Recon owns its fixed-size atomic publication path
    uint8_t subtype = (payload[0] >> 4) & 0x0F;
    const uint8_t* targetBssid = payload + 4;   // Addr1 = targeted BSSID
    const uint8_t* srcMac = payload + 10;       // Addr2 = transmitter
    uint16_t reason = (len >= 26) ? (payload[24] | (payload[25] << 8)) : 0;
    DefensePipeline::ingestDeauthObservation(rxChannel, rssi, subtype, srcMac,
                                   Recon::DeauthSourceOrigin::SPECTRUM_CALLBACK,
                                   targetBssid, reason);
}

static void updateParanoidSwine() {
    uint32_t now = millis();
    
    // ==[ DEFERRED DEAUTH ]==
    const SpectrumDeauthMath::Batch deauthBatch = SpectrumDeauthMath::unpack(
        pendingDeauthBatch.exchange(0u, std::memory_order_acquire));
    if (deauthBatch.count > 0u) {
        const int8_t rssi = deauthBatch.peakRssi;
        const uint8_t channel = deauthBatch.peakChannel;
        
        paranoid.frameCount = SpectrumDeauthMath::saturatingAdd(
            paranoid.frameCount, deauthBatch.count);
        paranoid.lastDeauthTime = now;
        paranoid.rssiCurrent = rssi;
        
        // Update RSSI history for trend
        paranoid.rssiHistory[paranoid.historyIdx] = rssi;
        paranoid.historyIdx = (paranoid.historyIdx + 1) % DEAUTH_HISTORY_LEN;
        
        // Track peak (closest approach)
        if (rssi > paranoid.rssiPeak) {
            paranoid.rssiPeak = rssi;
        }
        
        // Burst window tracking
        if (now - paranoid.windowStart > DEAUTH_BURST_WINDOW) {
            paranoid.windowStart = now;
            paranoid.framesInWindow = deauthBatch.count;
        } else {
            paranoid.framesInWindow = SpectrumDeauthMath::saturatingAdd(
                paranoid.framesInWindow, deauthBatch.count);
        }
        
        // Trigger attack state if burst threshold crossed
        if (paranoid.framesInWindow >= DEAUTH_BURST_THRESHOLD && !paranoid.attackActive) {
            paranoid.attackActive = true;
            paranoid.attackStart = now;
            paranoid.attackChannel = channel;
            
            // Spectrum speaks in Geiger clicks; the attack flag explicitly
            // silences the separate Morse notifier.
            paranoid.morseActive = false;
            
            // Wake screen if dimmed and alert wake enabled
            if (Config::getAlertWake() && Display::isDimmed()) {
                Display::resetDimTimer();
            }
        }
        
        // Update channel if attack ongoing
        if (paranoid.attackActive) {
            paranoid.attackChannel = channel;
        }
    }
    
    // ==[ ATTACK TIMEOUT ]== skip if paused (BLE sync can starve frames)
    if (paranoid.attackActive && !paused) {
        if (now - paranoid.lastDeauthTime > 5000) {
            // No deauth for 5 seconds = attack ended
            paranoid.attackActive = false;
            paranoid.detailViewActive = false;
            paranoid.morseActive = false;  // reset morse for next attack
            paranoid.rssiPeak = -100;
            paranoid.frameCount = 0;
            M5.Power.setLed(false);
            
            // Start linger timer for detail toast
            if (detailToastVisible) {
                detailToastExpiry = now + DETAIL_TOAST_LINGER;
            }
        }
    }
    
    // ==[ RSSI TREND CALC ]==
    if (paranoid.attackActive) {
        // Compare recent 4 samples vs older 4 samples
        int recent = 0, older = 0;
        for (int i = 0; i < 4; i++) {
            int recentIdx = (paranoid.historyIdx - 1 - i + DEAUTH_HISTORY_LEN) % DEAUTH_HISTORY_LEN;
            int olderIdx = (paranoid.historyIdx - 5 - i + DEAUTH_HISTORY_LEN) % DEAUTH_HISTORY_LEN;
            recent += paranoid.rssiHistory[recentIdx];
            older += paranoid.rssiHistory[olderIdx];
        }
        paranoid.rssiTrend = (recent - older) / 4;  // Positive = warming
    }
    
    // ==[ GEIGER AUDIO ]== delegated to Geiger module
    if (paranoid.attackActive) {
        if (!Geiger::isActive()) {
            Geiger::start(Geiger::SOURCE_DEAUTH);
        }
        Geiger::update(paranoid.rssiCurrent);
    } else if (Geiger::getSource() == Geiger::SOURCE_DEAUTH) {
        Geiger::stop();
    }
    
    // ==[ LED BLINK ]== landscape only
    if (paranoid.attackActive && isLandscapeOrientation()) {
        int8_t rssi = paranoid.rssiCurrent;
        rssi = constrain(rssi, -90, -30);
        int blinkInterval = map(rssi, -30, -90, 100, 1000);
        
        if (now - paranoid.lastLedToggle > (uint32_t)blinkInterval) {
            paranoid.ledState = !paranoid.ledState;
            M5.Power.setLed(paranoid.ledState);
            paranoid.lastLedToggle = now;
        }
    } else if (paranoid.attackActive && !isLandscapeOrientation()) {
        // Stealth mode - LED off when upright
        if (paranoid.ledState) {
            paranoid.ledState = false;
            M5.Power.setLed(false);
        }
    }
    
    // ==[ TOAST ANIMATION ]==
    if (paranoid.attackActive) {
        if (now - paranoid.lastToastToggle > 500) {
            paranoid.toastInverted = !paranoid.toastInverted;
            paranoid.lastToastToggle = now;
        }
    }
}

// ==[ PARANOID SWINE PUBLIC API ]==

const ParanoidSwine* getParanoidState() {
    return &paranoid;
}

bool isAttackActive() {
    return paranoid.attackActive;
}

void toggleParanoidDetail() {
    if (paranoid.attackActive) {
        paranoid.detailViewActive = !paranoid.detailViewActive;
    }
}

bool isParanoidDetailActive() {
    return paranoid.detailViewActive;
}

// ==[ DIAL MODE PUBLIC API ]==

bool isDialMode() {
    return dialMode;
}

void toggleDialMode() {
    dialMode = !dialMode;
    discardPartialMeasurementSweep(
        currentChannel.load(std::memory_order_relaxed));
    if (!dialMode) {
        // Exiting dial mode - resume channel hopping
        dialLocked = false;
        lockedChannel = 0;
    }
}

void toggleDialLock() {
    dialLocked = !dialLocked;
    if (!dialLocked) {
        // Unlocking - resume gyro control, smooth position already at current channel
        lastDialUpdate = millis();
    }
}

bool isDialLocked() {
    return dialLocked;
}

uint8_t getDialChannel() {
    return dialChannel;
}

uint32_t getDialPps() {
    return displayPps;
}

// ==[ D-UCB RECONNAISSANCE ]==

bool isDUCBReconMode() {
    // Recon runs whenever Spectrum is active and enabled.
    return active && Config::getIppEnabled();
}

void getReconChannelData(uint16_t* activity, uint32_t* time, uint16_t* networks, int8_t* peakRssi, uint16_t* attackable) {
    for (int i = 0; i < 13; i++) {
        uint8_t ch = (uint8_t)(i + 1);
        if (activity) {
            uint32_t dwellMs = channelTimeMs[ch];
            uint32_t frames = channelActivity[ch];
            uint16_t act = 0;
            if (dwellMs > 0) {
                uint64_t rate = ((uint64_t)frames * 1000u) / dwellMs;  // frames/sec
                if (rate > 0xFFFFu) rate = 0xFFFFu;
                act = (uint16_t)rate;
            }
            activity[i] = act;
        }
        if (time) {
            time[i] = channelTimeMs[ch];
        }
        if (networks) {
            networks[i] = channelNetCount[ch];
        }
        if (peakRssi) {
            peakRssi[i] = channelPeakRSSI[ch];
        }
        if (attackable) {
            attackable[i] = channelAttackableCount[ch];
        }
    }
}

uint32_t getReconLastUpdateMs() {
    return reconLastUpdateMs;
}

bool hasReconData() {
    for (uint8_t ch = 1; ch <= 13; ch++) {
        if (channelTimeMs[ch] > 0 || channelActivity[ch] > 0 || channelNetCount[ch] > 0) {
            return true;
        }
    }
    return false;
}

// ==[ SPECTRUM PAN CONTROLS ]==

static void clearWaterfallHistory() {
    if (waterfallBuffer) {
        memset(waterfallBuffer, 0, WATERFALL_ROWS * SPECTRUM_WIDTH);
    }
    if (waterfallTargetRow) {
        memset(waterfallTargetRow, 0, SPECTRUM_WIDTH);
    }
    if (waterfallDisplayX8) {
        memset(waterfallDisplayX8, 0,
               sizeof(int16_t) * SPECTRUM_WIDTH);
    }
    waterfallWriteRow = 0u;
    spectrumRevealPrevX = -1;  // history cleared -> full carrier repaint
    waterfallLastFrameMs = millis();
}

void panLeft() {
    const float previousCenter = viewCenterMHz;
    if (isShowing5GHz()) {
        // 5GHz: pan by ~20% of view width
        float panStep = viewWidthMHz * 0.2f;
        if (panStep < 20.0f) panStep = 20.0f;  // min 20 MHz step
        viewCenterMHz -= panStep;
        if (viewCenterMHz < (FREQ_BAND_5G_START + viewWidthMHz / 2.0f)) {
            viewCenterMHz = FREQ_BAND_5G_START + viewWidthMHz / 2.0f;
        }
    } else {
        // 2.4GHz: one channel per pan (5 MHz)
        if (viewCenterMHz > MIN_CENTER_MHZ) {
            viewCenterMHz -= PAN_STEP_MHZ;
            if (viewCenterMHz < MIN_CENTER_MHZ) viewCenterMHz = MIN_CENTER_MHZ;
        }
    }
    if (!isShowing5GHz() && viewCenterMHz != previousCenter) {
        // Rows are stored in display-space. Keeping them after a viewport move
        // would relabel old pixels as different frequencies.
        clearWaterfallHistory();
    }
}

void panRight() {
    const float previousCenter = viewCenterMHz;
    if (isShowing5GHz()) {
        // 5GHz: pan by ~20% of view width
        float panStep = viewWidthMHz * 0.2f;
        if (panStep < 20.0f) panStep = 20.0f;  // min 20 MHz step
        viewCenterMHz += panStep;
        if (viewCenterMHz > (FREQ_BAND_5G_END - viewWidthMHz / 2.0f)) {
            viewCenterMHz = FREQ_BAND_5G_END - viewWidthMHz / 2.0f;
        }
    } else {
        // 2.4GHz: one channel per pan (5 MHz)
        if (viewCenterMHz < MAX_CENTER_MHZ) {
            viewCenterMHz += PAN_STEP_MHZ;
            if (viewCenterMHz > MAX_CENTER_MHZ) viewCenterMHz = MAX_CENTER_MHZ;
        }
    }
    if (!isShowing5GHz() && viewCenterMHz != previousCenter) {
        clearWaterfallHistory();
    }
}

void zoomIn() {
    if (!isShowing5GHz()) return;
    viewWidthMHz = constrain(viewWidthMHz * 0.7f, VIEW_WIDTH_5GHZ_MIN, VIEW_WIDTH_5GHZ_MAX);
    // Re-clamp center after zoom
    if (viewCenterMHz < (FREQ_BAND_5G_START + viewWidthMHz / 2.0f)) {
        viewCenterMHz = FREQ_BAND_5G_START + viewWidthMHz / 2.0f;
    }
    if (viewCenterMHz > (FREQ_BAND_5G_END - viewWidthMHz / 2.0f)) {
        viewCenterMHz = FREQ_BAND_5G_END - viewWidthMHz / 2.0f;
    }
}

void zoomOut() {
    if (!isShowing5GHz()) return;
    viewWidthMHz = constrain(viewWidthMHz * 1.4f, VIEW_WIDTH_5GHZ_MIN, VIEW_WIDTH_5GHZ_MAX);
    // Re-clamp center after zoom
    if (viewCenterMHz < (FREQ_BAND_5G_START + viewWidthMHz / 2.0f)) {
        viewCenterMHz = FREQ_BAND_5G_START + viewWidthMHz / 2.0f;
    }
    if (viewCenterMHz > (FREQ_BAND_5G_END - viewWidthMHz / 2.0f)) {
        viewCenterMHz = FREQ_BAND_5G_END - viewWidthMHz / 2.0f;
    }
}

void zoomToggle() {
    if (!isShowing5GHz()) return;
    // Toggle between wide view (max) and zoomed view (min)
    if (viewWidthMHz > (VIEW_WIDTH_5GHZ_MAX + VIEW_WIDTH_5GHZ_MIN) / 2.0f) {
        zoomIn();
    } else {
        zoomOut();
    }
}

void toggleModelOverlay() {
    modelOverlayEnabled = !modelOverlayEnabled;
}

bool isModelOverlayEnabled() {
    return modelOverlayEnabled;
}

float getViewCenter() {
    return viewCenterMHz;
}

// ==[ SPECTRUM DRAWING ]== Rendering stays beside measurement state so one
// frequency mapping governs traces, carriers, touch, and labels. Canonical
// 320x240 geometry lives in UIMeasurements::Spectrum.
static const int SPECTRUM_LEFT = UIMeasurements::Spectrum::kGraphLeft;        // 26
static const int SPECTRUM_RIGHT = UIMeasurements::Spectrum::kGraphRight;      // 316, exclusive
static const int SPECTRUM_TOP = UIMeasurements::Spectrum::kGraphTop;          // 14
static const int SPECTRUM_BOTTOM = UIMeasurements::Spectrum::kGraphBottom;    // 94
static const int SPECTRUM_PLOT_TOP = UIMeasurements::Spectrum::kGraphPlotTop; // 24
static const int SPECTRUM_HEADER_H = UIMeasurements::Spectrum::kGraphHeaderH; // 10
static const int CHANNEL_LABEL_Y = UIMeasurements::Spectrum::kChannelLabelY;  // 120
static const int NETWORK_LIST_Y = UIMeasurements::Spectrum::kNetworkListY;    // 130
static_assert(SPECTRUM_WIDTH == UIMeasurements::Spectrum::kGraphWidth,
              "Spectrum buffers must match the visible graph width");

// ==[ VISUAL LANGUAGE ]== compact monochrome/TUI helpers shared by all views.
static constexpr int UI_FRAME_CORNER = 2;
static constexpr int UI_TEXT_H = 8;

static inline uint16_t uiTone(uint16_t fg, uint16_t bg, float amount) {
    return Display::lerpColor565(fg, bg, amount);
}

static void fitTextToWidth(M5Canvas& canvas, char* text, size_t capacity,
                           int maxWidth) {
    if (!text || capacity == 0u) return;
    if (maxWidth <= 0) {
        text[0] = '\0';
        return;
    }

    size_t length = 0u;
    while (length + 1u < capacity && text[length] != '\0') ++length;
    bool clipped = false;
    while (length > 0u && canvas.textWidth(text) > maxWidth) {
        text[--length] = '\0';
        clipped = true;
    }
    if (clipped && length > 0u) text[length - 1u] = '~';
}

static void drawBracketFrame(M5Canvas& canvas, int x, int y, int width,
                             int height, uint16_t color) {
    if (width < 2 * UI_FRAME_CORNER + 1 ||
        height < 2 * UI_FRAME_CORNER + 1) {
        return;
    }

    canvas.drawFastHLine(x + UI_FRAME_CORNER, y,
                         width - 2 * UI_FRAME_CORNER, color);
    canvas.drawFastHLine(x + UI_FRAME_CORNER, y + height - 1,
                         width - 2 * UI_FRAME_CORNER, color);
    canvas.drawFastVLine(x, y + UI_FRAME_CORNER,
                         height - 2 * UI_FRAME_CORNER, color);
    canvas.drawFastVLine(x + width - 1, y + UI_FRAME_CORNER,
                         height - 2 * UI_FRAME_CORNER, color);

    canvas.drawPixel(x + 1, y + 1, color);
    canvas.drawPixel(x + width - 2, y + 1, color);
    canvas.drawPixel(x + 1, y + height - 2, color);
    canvas.drawPixel(x + width - 2, y + height - 2, color);
}

static void drawDottedHLine(M5Canvas& canvas, int x, int y, int width,
                            uint16_t color, int dash = 3, int gap = 3) {
    if (width <= 0 || dash <= 0) return;
    const int step = dash + max(gap, 0);
    for (int cursor = x; cursor < x + width; cursor += step) {
        canvas.drawFastHLine(cursor, y, min(dash, x + width - cursor), color);
    }
}

static void drawSelectedRow(M5Canvas& canvas, int x, int y, int width,
                            int height, uint16_t fg) {
    if (x < 2 || width < 3 || height < 3) return;
    canvas.fillRect(x, y, width, height - 1, fg);
    canvas.drawFastVLine(x - 2, y + 2, max(1, height - 5), fg);
    canvas.drawPixel(x - 1, y + 1, fg);
    canvas.drawPixel(x - 1, y + height - 3, fg);
}

static void drawScrollRail(M5Canvas& canvas, int x, int y, int height,
                           int firstVisible, int visibleCount, int totalCount,
                           uint16_t fg, uint16_t bg) {
    if (height < 4 || totalCount <= visibleCount || visibleCount <= 0) return;

    const uint16_t track = uiTone(fg, bg, 0.62f);
    canvas.drawFastVLine(x, y, height, track);

    const int thumbHeight = constrain(
        (height * visibleCount) / totalCount, 3, height);
    const int travel = height - thumbHeight;
    const int denominator = max(1, totalCount - visibleCount);
    const int thumbY = y + (travel * firstVisible) / denominator;
    canvas.fillRect(x - 1, thumbY, 2, thumbHeight, fg);
}

static void formatAgeShort(char* out, size_t outSize, uint32_t ageMs) {
    if (!out || outSize == 0u) return;
    const uint32_t seconds = ageMs / 1000u;
    if (seconds < 60u) {
        snprintf(out, outSize, "%lus", (unsigned long)seconds);
    } else if (seconds < 3600u) {
        snprintf(out, outSize, "%lum", (unsigned long)(seconds / 60u));
    } else {
        const uint32_t hours = min(seconds / 3600u, 99u);
        snprintf(out, outSize, "%luh", (unsigned long)hours);
    }
}

static void drawLevelMeter(M5Canvas& canvas, int x, int y, int width,
                           int height, uint8_t level, uint16_t fg,
                           uint16_t bg) {
    if (width < 3 || height < 3) return;
    const uint16_t outline = uiTone(fg, bg, 0.44f);
    canvas.drawRect(x, y, width, height, outline);
    const int fillWidth = constrain(
        (static_cast<int>(level) * (width - 2)) / 100, 0, width - 2);
    if (fillWidth > 0) {
        canvas.fillRect(x + 1, y + 1, fillWidth, height - 2, fg);
    }
}

static void drawEmptyState(M5Canvas& canvas, int x, int y, int width,
                           int height, const char* title,
                           const char* subtitle, uint16_t fg, uint16_t bg) {
    canvas.fillRect(x, y, width, height, bg);
    const uint16_t frame = uiTone(fg, bg, 0.48f);
    drawBracketFrame(canvas, x + 6, y + 5, width - 12, height - 10, frame);

    canvas.setTextSize(1);
    canvas.setTextDatum(MC_DATUM);
    canvas.setTextColor(fg, bg);
    canvas.drawString(title ? title : "", x + width / 2, y + height / 2 - 6);
    canvas.setTextColor(uiTone(fg, bg, 0.54f), bg);
    canvas.drawString(subtitle ? subtitle : "",
                      x + width / 2, y + height / 2 + 7);
    canvas.setTextColor(fg, bg);
    canvas.setTextDatum(TL_DATUM);
}

// RSSI scale
static const int8_t RSSI_MIN = -95;
static const int8_t RSSI_MAX = -30;

// ==[ GAUSSIAN LUT ]== precomputed exp(-0.5 * d^2 / sigma^2) for sigma=6.6
// Index 0-30 maps to distance 0.0 to 15.0 MHz in 0.5 steps (symmetric)
static const float GAUSSIAN_LUT[31] = {
    1.0000f, 0.9971f, 0.9886f, 0.9744f, 0.9549f,  // d=0.0-2.0
    0.9305f, 0.9017f, 0.8690f, 0.8329f, 0.7940f,  // d=2.5-4.5
    0.7528f, 0.7100f, 0.6661f, 0.6217f, 0.5772f,  // d=5.0-7.0
    0.5333f, 0.4903f, 0.4487f, 0.4088f, 0.3708f,  // d=7.5-9.5
    0.3349f, 0.3012f, 0.2698f, 0.2406f, 0.2137f,  // d=10.0-12.0
    0.1889f, 0.1663f, 0.1458f, 0.1273f, 0.1107f,  // d=12.5-14.5
    0.0959f                                         // d=15.0
};

// Fast Gaussian amplitude using LUT + linear interpolation
static inline float getGaussianAmplitude(float dist) {
    float absDist = fabsf(dist);
    if (absDist > 15.0f) return 0.0f;
    float lutPos = absDist * 2.0f;  // 0.5 step -> index
    int idx = (int)lutPos;
    float frac = lutPos - idx;
    if (idx >= 30) return GAUSSIAN_LUT[30];
    return GAUSSIAN_LUT[idx] + frac * (GAUSSIAN_LUT[idx + 1] - GAUSSIAN_LUT[idx]);
}

static inline float channelToFreqF(int channel) {
    // 5GHz channels (36-165): freq = 5000 + channel * 5
    // 2.4GHz channels (1-13): freq = 2412 + (channel - 1) * 5
    if (channel >= 36 && channel <= 165) {
        return 5000.0f + channel * 5.0f;
    }
    return 2412.0f + (channel - 1) * 5.0f;
}

static inline uint16_t channelToDisplayFreq(uint8_t channel, bool is5gMode) {
    if (is5gMode) return (uint16_t)(channelToFreqF(channel) + 0.5f);
    return channelToFreq(channel);
}

static constexpr uint16_t FREQ_BAND_2G_START = 2407;  // left edge for 2.4GHz lane
static constexpr uint16_t FREQ_BAND_2G_END   = 2477;  // right edge for 2.4GHz lane

static float cachedDisplayLeftFreq = (float)FREQ_BAND_2G_START;
static float cachedDisplayRightFreq = (float)FREQ_BAND_2G_END;

static inline int freqToXCached(float freqMHz);

static void drawDashedBandEdge(M5Canvas& canvas, int x, int y1, int y2, uint16_t color) {
    if (x < SPECTRUM_LEFT || x >= SPECTRUM_RIGHT) return;
    for (int y = y1; y <= y2; y += 4) {
        int h = ((y + 2) <= y2) ? 2 : (y2 - y + 1);
        if (h > 0) canvas.drawFastVLine(x, y, h, color);
    }
}

static void drawClippedSpectrumHLine(M5Canvas& canvas, int x, int y,
                                     int width, uint16_t color) {
    int left = x;
    int right = x + width;
    if (left < SPECTRUM_LEFT) left = SPECTRUM_LEFT;
    if (right > SPECTRUM_RIGHT) right = SPECTRUM_RIGHT;
    if (right > left) {
        canvas.drawFastHLine(left, y, right - left, color);
    }
}

static void get5GHzDisplayWindow(float& leftFreqMHz, float& rightFreqMHz) {
    SpectrumC5Policy::displayWindowMHz(
        viewCenterMHz, viewWidthMHz, leftFreqMHz, rightFreqMHz);
}

static void drawBandSeparators(M5Canvas& canvas, bool is5g, uint16_t fg, uint16_t bg) {
    const uint16_t sepColor = Display::lerpColor565(fg, bg, 0.45f);
    const int guideTop = SPECTRUM_PLOT_TOP;
    const int guideBottom = SPECTRUM_BOTTOM - 2;

    const float bandStartFreq = is5g
        ? (float)FREQ_BAND_5G_START
        : (float)FREQ_BAND_2G_START;
    const float bandEndFreq = is5g
        ? (float)FREQ_BAND_5G_END
        : (float)FREQ_BAND_2G_END;
    const int bandStartX = freqToXCached(bandStartFreq);
    const int bandEndX   = freqToXCached(bandEndFreq);
    drawDashedBandEdge(canvas, bandStartX, guideTop, guideBottom, sepColor);
    drawDashedBandEdge(canvas, bandEndX, guideTop, guideBottom, sepColor);

    if (is5g) {
        // Add separators where UNII segments visibly jump (e.g., 64→100, 140→149).
        for (uint8_t i = 1; i < CH5G_COMMON_COUNT; i++) {
            if (CH5G_COMMON[i] <= CH5G_COMMON[i - 1] + 4) continue;
            float gapFreq = (channelToFreqF(CH5G_COMMON[i - 1]) + channelToFreqF(CH5G_COMMON[i])) * 0.5f;
            int gapX = freqToXCached(gapFreq);
            drawDashedBandEdge(canvas, gapX, guideTop, guideBottom, sepColor);
        }
    }
}

// ==[ FRAME CACHE ]== computed once per frame, used by all render functions
static float cachedLeftFreq;
static float cachedPixelsPerMHz;  // inverse
static int cachedSpectrumWidth;
static uint32_t cachedMillis;     // millis() snapshot for entire frame

static void cacheFrameConstants() {
    cachedSpectrumWidth = SPECTRUM_RIGHT - SPECTRUM_LEFT;
    float leftFreq = viewCenterMHz - viewWidthMHz / 2.0f;
    float rightFreq = viewCenterMHz + viewWidthMHz / 2.0f;

    if (isShowing5GHz()) {
        get5GHzDisplayWindow(leftFreq, rightFreq);
    }

    const float displayWidth = rightFreq - leftFreq;
    if (displayWidth <= 0.0f) {
        rightFreq = leftFreq + 1.0f;
    }

    cachedDisplayLeftFreq = leftFreq;
    cachedDisplayRightFreq = rightFreq;
    cachedLeftFreq = leftFreq;
    cachedPixelsPerMHz = (float)cachedSpectrumWidth / (cachedDisplayRightFreq - cachedDisplayLeftFreq);
    cachedMillis = millis();
    if (!isShowing5GHz()) {
        for (uint8_t channel = 1u; channel <= 13u; ++channel) {
            const int column =
                freqToXCached(channelToFreqF(channel) - 2.5f) -
                SPECTRUM_LEFT;
            measuredTrace[channel].sweepColumn = static_cast<uint16_t>(
                constrain(column, 0, SPECTRUM_WIDTH - 1));
        }
    }
}

// Fast freqToX using cached values
static inline int freqToXCached(float freqMHz) {
    return SPECTRUM_LEFT + (int)((freqMHz - cachedLeftFreq) * cachedPixelsPerMHz);
}

// Evaluate a carrier at the centre of a physical display column. Model and
// measured layers must use the same mapping as the waterfall, or a wide pane
// turns a smooth carrier into repeated samples of the same few pixels.
static inline float xToFreqCached(int x) {
    return cachedDisplayLeftFreq +
        (static_cast<float>(x - SPECTRUM_LEFT) + 0.5f) /
            cachedPixelsPerMHz;
}

static inline int rssiToY(int8_t rssi) {
    if (rssi < RSSI_MIN) rssi = RSSI_MIN;
    if (rssi > RSSI_MAX) rssi = RSSI_MAX;
    int height = SPECTRUM_BOTTOM - SPECTRUM_PLOT_TOP;
    return SPECTRUM_BOTTOM - (int)(((float)(rssi - RSSI_MIN) / (RSSI_MAX - RSSI_MIN)) * height);
}

static inline int traceX8ToY(int16_t rssiX8) {
    const int16_t minX8 = static_cast<int16_t>(RSSI_MIN * 8);
    const int16_t maxX8 = static_cast<int16_t>(RSSI_MAX * 8);
    rssiX8 = constrain(rssiX8, minX8, maxX8);
    const int32_t height = SPECTRUM_BOTTOM - SPECTRUM_PLOT_TOP;
    return SPECTRUM_BOTTOM -
        static_cast<int>(
            (static_cast<int32_t>(rssiX8 - minX8) * height) /
            static_cast<int32_t>(maxX8 - minX8));
}

// ==[ V2 VISUALS: WATERFALL + SINC LOBES ]==
// Waterfall layout (below spectrum bars)
static const int WATERFALL_TOP = SPECTRUM_BOTTOM + 2;     // 96
static const int8_t C5_SNAPSHOT_FLOOR_DB = -92;

static const SpectrumNetwork* strongestNativeNetworkOnChannel(
        uint8_t channel) {
    const SpectrumNetwork* strongest = nullptr;
    for (uint16_t i = 0u; i < networkCount; ++i) {
        if (networks[i].channel != channel) continue;
        if (!strongest || networks[i].rssi > strongest->rssi) {
            strongest = &networks[i];
        }
    }
    return strongest;
}

static float carrierProfileAmplitude(
        const SpectrumNetwork* network, float distanceMHz) {
    if (network && network->phyMode == 1u) {
        return getGaussianAmplitude(distanceMHz);
    }
    const float widthScale =
        network && network->channelWidth != 0u ? 1.82f : 1.0f;
    return getSincAmplitude(distanceMHz / widthScale);
}

static uint8_t carrierPacketTexture(
        const RfMeasurement::ChannelEvidence& evidence) {
    if (evidence.sampleCount == 0u || evidence.frames == 0u) return 0u;
    const uint32_t framesPerSecond =
        RfMeasurement::framesPerSecond(evidence);
    const uint32_t scaled = min(
        static_cast<uint32_t>(PACKET_DENSITY_PPS_MAX),
        framesPerSecond);
    return static_cast<uint8_t>(
        56u + (scaled * 160u) / PACKET_DENSITY_PPS_MAX);
}

// Stage a physical carrier profile from completed sweep evidence. Center
// frequency and amplitude remain measured; the strongest observed AP on the
// channel contributes its PHY family and 20/40 MHz width to the approximate
// envelope. A low stippled floor records quiet coverage without inventing RF
// energy, while packet cadence controls texture density inside active lobes.
static void stageWaterfallTarget(
        const RfMeasurement::SweepSnapshot& sweep) {
    if (!waterfallTargetRow) return;
    uint8_t* row = waterfallTargetRow;
    memset(row, WATERFALL_QUIET_FLOOR, SPECTRUM_WIDTH);

    for (uint8_t channel = 1u; channel <= 13u; ++channel) {
        const RfMeasurement::ChannelEvidence& evidence =
            sweep.channels[channel];
        if (evidence.dwellMs == 0u) continue;  // unknown, not synthetic quiet
        if (evidence.sampleCount == 0u) continue;

        const float centerFreq = channelToFreqF(channel);
        const SpectrumNetwork* network =
            strongestNativeNetworkOnChannel(channel);
        const float widthScale =
            network && network->channelWidth != 0u ? 1.82f : 1.0f;
        const bool dsss = network && network->phyMode == 1u;
        const float halfSpan = dsss ? 15.0f : 22.0f * widthScale;
        const float signalLeft = centerFreq - halfSpan;
        const float signalRight = centerFreq + halfSpan;
        if (signalRight < cachedDisplayLeftFreq ||
            signalLeft >= cachedDisplayRightFreq) {
            continue;
        }
        int x0 = freqToXCached(signalLeft) - SPECTRUM_LEFT;
        int x1 = freqToXCached(signalRight) - SPECTRUM_LEFT;
        x0 = constrain(x0, 0, SPECTRUM_WIDTH - 1);
        x1 = constrain(x1, 0, SPECTRUM_WIDTH - 1);
        if (x0 > x1) continue;

        const int strength = 48 + constrain(
            ((int)evidence.trimmedMeanRssi - RSSI_MIN) * 112 /
                (RSSI_MAX - RSSI_MIN),
            0, 112);
        const uint8_t packetTexture = carrierPacketTexture(evidence);
        const uint16_t crestIntensity = static_cast<uint16_t>(
            min(255, strength + static_cast<int>(packetTexture / 2u)));
        for (int x = x0; x <= x1; ++x) {
            const float frequency =
                cachedDisplayLeftFreq +
                (static_cast<float>(x) + 0.5f) / cachedPixelsPerMHz;
            const float amplitude =
                carrierProfileAmplitude(network, frequency - centerFreq);
            const uint8_t intensity = static_cast<uint8_t>(
                constrain(static_cast<int>(
                    crestIntensity * amplitude), 0, 255));
            if (intensity > row[x]) row[x] = intensity;
        }
    }
}

// Draw measured per-channel noise-floor markers. Unknown channels stay blank;
// the old animated "grass" was a model and looked more certain than the data.
static void drawNoiseFloor(M5Canvas& canvas, uint16_t fg) {
    if (!measuredSweepValid || !measuredSweep) return;
    for (uint8_t channel = 1u; channel <= 13u; ++channel) {
        const RfMeasurement::ChannelEvidence& evidence =
            measuredSweep->channels[channel];
        if (evidence.sampleCount == 0u ||
            evidence.medianNoiseFloor >= 0) {
            continue;
        }
        const int x = freqToXCached(channelToFreqF(channel));
        const int y = rssiToY(evidence.medianNoiseFloor);
        if (x < SPECTRUM_LEFT || x >= SPECTRUM_RIGHT) continue;
        drawClippedSpectrumHLine(canvas, x - 2, y, 5, fg);
    }
}

// Draw waterfall display - historical spectrum scrolling down
// Batches consecutive drawn pixels into drawFastHLine() runs
static void drawWaterfall(M5Canvas& canvas, uint16_t fg, uint16_t bg) {
    // Keep the cap in the same packet texture as the quiet carrier floor.
    // A solid separator reads as another waterfall observation once it moves
    // through the viewer's persistence.
    const uint16_t capColor = uiTone(fg, bg, 0.54f);
    for (int x = 0; x < SPECTRUM_WIDTH; ++x) {
        if (SpectrumThruMath::orderedDensityCellVisible(
                WATERFALL_QUIET_FLOOR,
                static_cast<uint16_t>(x), 0u)) {
            canvas.drawPixel(SPECTRUM_LEFT + x,
                             WATERFALL_TOP - 1, capColor);
        }
    }
    canvas.drawFastVLine(SPECTRUM_LEFT - 1, WATERFALL_TOP,
                         WATERFALL_ROWS, capColor);
    canvas.drawFastVLine(SPECTRUM_RIGHT, WATERFALL_TOP,
                         WATERFALL_ROWS, capColor);

    if (!waterfallBuffer || !waterfallTargetRow || !waterfallDisplayX8) {
        canvas.fillRect(SPECTRUM_LEFT, WATERFALL_TOP,
                        SPECTRUM_WIDTH, WATERFALL_ROWS, bg);
        canvas.setTextSize(1);
        canvas.setTextColor(fg, bg);
        canvas.setTextDatum(TL_DATUM);
        canvas.drawString("MEAS OK / WF BUFFER ERR",
                          SPECTRUM_LEFT + 2, WATERFALL_TOP + 2);
        return;
    }

    // Analyzer convention: the newest filtered display sample is at the top.
    // Targets change only on real completed sweeps; the 10 FPS rows are the
    // visible VBW response, not claims of additional RF acquisitions.
    for (int row = 0; row < WATERFALL_ROWS; row++) {
        const int bufRow = SpectrumThruMath::newestWaterfallRow(
            waterfallWriteRow, static_cast<uint8_t>(row), WATERFALL_ROWS);
        int screenY = WATERFALL_TOP + row;
        const uint8_t* rowBuf =
            waterfallBuffer + bufRow * SPECTRUM_WIDTH;

        // Ordered 5-tier density: stored intensity picks a deterministic dot
        // lattice, so busier channels read as denser sinking rows. Batch
        // consecutive drawn pixels into drawFastHLine() runs.
        int runStart = -1;
        for (int x = 0; x <= SPECTRUM_WIDTH; x++) {
            bool draw = false;
            if (x < SPECTRUM_WIDTH) {
                draw = SpectrumThruMath::orderedDensityCellVisible(
                    rowBuf[x], static_cast<uint16_t>(x),
                    static_cast<uint16_t>(row));
            }
            if (draw && runStart < 0) {
                runStart = x;
            } else if (!draw && runStart >= 0) {
                canvas.drawFastHLine(SPECTRUM_LEFT + runStart, screenY,
                                     x - runStart, fg);
                runStart = -1;
            }
        }
    }

    char label[32];
    if (measuredSweepValid && measuredSweep) {
        const uint32_t rawAgeSeconds =
            (cachedMillis - measuredSweep->completedAtMs) / 1000u;
        const uint16_t epoch = static_cast<uint16_t>(
            min(measuredSweep->epoch, 9999u));
        const uint16_t ageSeconds = static_cast<uint16_t>(
            min(rawAgeSeconds, 999u));
        const uint16_t drops = static_cast<uint16_t>(
            min(measuredSweep->queueDrops, 9999u));
        snprintf(label, sizeof(label), "SWP E%u%s A%u%s D%u%s",
                 (unsigned)epoch,
                 measuredSweep->epoch > epoch ? "+" : "",
                 (unsigned)ageSeconds,
                 rawAgeSeconds > ageSeconds ? "+" : "",
                 (unsigned)drops,
                 measuredSweep->queueDrops > drops ? "+" : "");
    } else {
        uint8_t covered = 0u;
        const uint16_t mask =
            rfMeasurements ? rfMeasurements->coverageMask() : 0u;
        for (uint8_t bit = 0u; bit < 13u; ++bit) {
            if ((mask & (1u << bit)) != 0u) ++covered;
        }
        snprintf(label, sizeof(label), "SWEEP %u/13", (unsigned)covered);
    }
    canvas.setTextSize(1);
    const int labelWidth = canvas.textWidth(label) + 4;
    canvas.fillRect(SPECTRUM_LEFT, WATERFALL_TOP + 1,
                    labelWidth, 9, bg);
    canvas.setTextColor(fg, bg);
    canvas.setTextDatum(TL_DATUM);
    canvas.drawString(label, SPECTRUM_LEFT + 2, WATERFALL_TOP + 2);
}

static void drawC5SnapshotStrip(M5Canvas& canvas, uint16_t fg, uint16_t bg) {
    // C5 scan RSSI is a snapshot, not a time-series waterfall. Reuse the
    // current-frame spectrum buffer for a deterministic density strip so a
    // band switch never displays stale 2.4GHz history as 5GHz evidence.
    if (!spectrumBuffer) {
        canvas.fillRect(SPECTRUM_LEFT, WATERFALL_TOP,
                        SPECTRUM_WIDTH, WATERFALL_ROWS, bg);
        canvas.drawFastHLine(SPECTRUM_LEFT, WATERFALL_TOP - 1,
                             SPECTRUM_WIDTH, uiTone(fg, bg, 0.50f));
        canvas.setTextSize(1);
        canvas.setTextColor(fg, bg);
        canvas.setTextDatum(TL_DATUM);
        canvas.drawString("C5 SNAP BUFFER ERR",
                          SPECTRUM_LEFT + 2, WATERFALL_TOP + 2);
        return;
    }

    static uint32_t cachedRevision = 0;
    static float cachedProfileLeft = 0.0f;
    static float cachedProfileRight = 0.0f;
    const bool rebuild = c5SnapshotStripDirty ||
        cachedRevision != c5gLastScanRevision ||
        cachedProfileLeft != cachedDisplayLeftFreq ||
        cachedProfileRight != cachedDisplayRightFreq;

    if (rebuild) {
        memset(spectrumBuffer, C5_SNAPSHOT_FLOOR_DB, SPECTRUM_WIDTH);
        for (uint8_t i = 0; i < c5gNetworkCount; ++i) {
            const C5GHzNetwork& network = c5gNetworks[i];

            const float centerFreq = channelToFreqF(network.channel);
            const float signalLeft = centerFreq - 22.0f;
            const float signalRight = centerFreq + 22.0f;
            if (signalRight < cachedDisplayLeftFreq ||
                signalLeft >= cachedDisplayRightFreq) {
                continue;
            }
            // The main plot evaluates lobes at display-column centres. Keep
            // this snapshot strip on that exact coordinate system so a band
            // switch cannot introduce a half-column carrier phase shift.
            int x0 = freqToXCached(signalLeft) - SPECTRUM_LEFT - 1;
            int x1 = freqToXCached(signalRight) - SPECTRUM_LEFT + 1;
            x0 = constrain(x0, 0, SPECTRUM_WIDTH - 1);
            x1 = constrain(x1, 0, SPECTRUM_WIDTH - 1);
            if (x0 > x1) continue;

            for (int x = x0; x <= x1; ++x) {
                const float freq = xToFreqCached(SPECTRUM_LEFT + x);
                const float amplitude =
                    getSincAmplitude(freq - centerFreq);
                if (amplitude < 0.05f) continue;
                const int8_t modeledRssi = (int8_t)(
                    C5_SNAPSHOT_FLOOR_DB +
                    ((int)network.rssi - C5_SNAPSHOT_FLOOR_DB) * amplitude);
                if (modeledRssi > spectrumBuffer[x]) {
                    spectrumBuffer[x] = modeledRssi;
                }
            }
        }
        cachedRevision = c5gLastScanRevision;
        cachedProfileLeft = cachedDisplayLeftFreq;
        cachedProfileRight = cachedDisplayRightFreq;
        c5SnapshotStripDirty = false;
    }

    canvas.fillRect(SPECTRUM_LEFT, WATERFALL_TOP,
                    SPECTRUM_WIDTH, WATERFALL_ROWS, bg);
    const uint16_t stripFrame = uiTone(fg, bg, 0.50f);
    canvas.drawFastHLine(SPECTRUM_LEFT, WATERFALL_TOP - 1,
                         SPECTRUM_WIDTH, stripFrame);
    canvas.drawFastVLine(SPECTRUM_LEFT - 1, WATERFALL_TOP,
                         WATERFALL_ROWS, stripFrame);
    canvas.drawFastVLine(SPECTRUM_RIGHT, WATERFALL_TOP,
                         WATERFALL_ROWS, stripFrame);
    for (int row = 0; row < WATERFALL_ROWS; ++row) {
        int runStart = -1;
        for (int x = 0; x <= SPECTRUM_WIDTH; ++x) {
            bool draw = false;
            if (x < SPECTRUM_WIDTH) {
                const int intensity = constrain(
                    ((int)spectrumBuffer[x] - RSSI_MIN) * 255 /
                        (RSSI_MAX - RSSI_MIN),
                    0, 255);
                draw = SpectrumThruMath::orderedDensityCellVisible(
                    static_cast<uint8_t>(intensity),
                    static_cast<uint16_t>(x),
                    static_cast<uint16_t>(row));
            }
            if (draw && runStart < 0) {
                runStart = x;
            } else if (!draw && runStart >= 0) {
                canvas.drawFastHLine(
                    SPECTRUM_LEFT + runStart, WATERFALL_TOP + row,
                    x - runStart, fg);
                runStart = -1;
            }
        }
    }
    char age[12];
    char label[28];
    formatC5SnapshotAge(age, sizeof(age), cachedMillis);
    snprintf(label, sizeof(label), "SNAP/M R%lu %s",
             (unsigned long)c5gLastScanRevision, age);
    canvas.setTextSize(1);
    canvas.setTextColor(fg, bg);
    canvas.setTextDatum(TL_DATUM);
    canvas.drawString(label, SPECTRUM_LEFT + 2, WATERFALL_TOP + 2);

    char range[20];
    snprintf(range, sizeof(range), "%u-%u",
             (unsigned)(cachedDisplayLeftFreq + 0.5f),
             (unsigned)(cachedDisplayRightFreq + 0.5f));
    const int rangeW = canvas.textWidth(range) + 4;
    canvas.fillRect(SPECTRUM_RIGHT - rangeW, WATERFALL_TOP + 1,
                    rangeW, 9, bg);
    canvas.setTextDatum(TR_DATUM);
    canvas.drawString(range, SPECTRUM_RIGHT - 2, WATERFALL_TOP + 2);
    canvas.setTextDatum(TL_DATUM);
}

static void drawC5PacketRateStrip(M5Canvas& canvas, uint16_t fg,
                                  uint16_t bg) {
    C5Monster::PacketMonitorTelemetry telemetry = {};
    const bool hasTelemetry =
        C5Monster::getPacketMonitorTelemetry(telemetry);
    const bool hasSamples = hasTelemetry && telemetry.historyCount > 0u;
    const bool fresh = hasSamples && telemetry.ageMs <= 2500u;

    canvas.fillRect(SPECTRUM_LEFT, WATERFALL_TOP,
                    SPECTRUM_WIDTH, WATERFALL_ROWS, bg);
    const uint16_t stripFrame = uiTone(fg, bg, 0.50f);
    canvas.drawFastHLine(SPECTRUM_LEFT, WATERFALL_TOP - 1,
                         SPECTRUM_WIDTH, stripFrame);
    canvas.drawFastVLine(SPECTRUM_LEFT - 1, WATERFALL_TOP,
                         WATERFALL_ROWS, stripFrame);
    canvas.drawFastVLine(SPECTRUM_RIGHT, WATERFALL_TOP,
                         WATERFALL_ROWS, stripFrame);
    canvas.setTextSize(1);

    const int plotLeft = SPECTRUM_LEFT + 1;
    const int plotRight = SPECTRUM_RIGHT - 1;
    const int plotTop = WATERFALL_TOP + 2;
    const int plotBottom = WATERFALL_TOP + WATERFALL_ROWS - 2;
    const int plotHeight = plotBottom - plotTop;

    uint32_t peak = 0u;
    if (hasSamples) {
        for (uint8_t i = 0; i < telemetry.historyCount; ++i) {
            if (telemetry.history[i] > peak) peak = telemetry.history[i];
        }
        const uint32_t ceiling =
            C5ObserverMath::packetRatePlotCeiling(peak);
        const int midY = plotBottom - plotHeight / 2;
        const uint16_t guide = uiTone(fg, bg, 0.66f);
        for (int x = plotLeft; x <= plotRight; x += 8) {
            canvas.drawFastHLine(x, midY, 4, guide);
        }

        const int slotWidth =
            (plotRight - plotLeft) /
            (C5Monster::PACKET_MONITOR_HISTORY_CAPACITY - 1u);
        for (uint8_t i = 0; i < telemetry.historyCount; ++i) {
            const int slotsFromNewest =
                (int)telemetry.historyCount - 1 - (int)i;
            const int x = plotRight - slotsFromNewest * slotWidth;
            const int barHeight = (int)(
                ((uint32_t)telemetry.history[i] * (uint32_t)plotHeight) /
                ceiling);
            const int y = plotBottom - constrain(barHeight, 0, plotHeight);
            const int barLeft = x > plotLeft ? x - 1 : plotLeft;
            const int barRight =
                x + 1 < plotRight ? x + 1 : plotRight;
            canvas.fillRect(barLeft, y, barRight - barLeft + 1,
                            plotBottom - y + 1, fg);
        }

        char scale[16];
        snprintf(scale, sizeof(scale), "MAX %lu",
                 (unsigned long)ceiling);
        const int scaleW = canvas.textWidth(scale) + 4;
        canvas.fillRect(SPECTRUM_RIGHT - scaleW, WATERFALL_TOP + 11,
                        scaleW, 9, bg);
        canvas.setTextColor(fg, bg);
        canvas.setTextDatum(TR_DATUM);
        canvas.drawString(scale, SPECTRUM_RIGHT - 2,
                          WATERFALL_TOP + 12);
    }

    char label[32];
    if (!hasTelemetry) {
        snprintf(label, sizeof(label), "WAIT");
    } else if (!hasSamples) {
        snprintf(label, sizeof(label), "CH%u WAIT",
                 (unsigned)telemetry.channel);
    } else if (!fresh) {
        snprintf(label, sizeof(label), "CH%u STALE",
                 (unsigned)telemetry.channel);
    } else {
        snprintf(label, sizeof(label), "CH%u %lu/s",
                 (unsigned)telemetry.channel,
                 (unsigned long)telemetry.packetsPerSecond);
    }
    const int labelW = canvas.textWidth(label) + 4;
    canvas.fillRect(SPECTRUM_LEFT + 1, WATERFALL_TOP + 1,
                    labelW, 9, bg);
    canvas.setTextColor(fg, bg);
    canvas.setTextDatum(TL_DATUM);
    canvas.drawString(label, SPECTRUM_LEFT + 3, WATERFALL_TOP + 2);

    char range[20];
    snprintf(range, sizeof(range), "%u-%u",
             (unsigned)(cachedDisplayLeftFreq + 0.5f),
             (unsigned)(cachedDisplayRightFreq + 0.5f));
    const int rangeW = canvas.textWidth(range) + 4;
    canvas.fillRect(SPECTRUM_RIGHT - rangeW, WATERFALL_TOP + 1,
                    rangeW, 9, bg);
    canvas.setTextDatum(TR_DATUM);
    canvas.drawString(range, SPECTRUM_RIGHT - 2, WATERFALL_TOP + 2);
    canvas.setTextDatum(TL_DATUM);
}

static void drawC5ChannelMapStrip(M5Canvas& canvas, uint16_t fg,
                                  uint16_t bg) {
    C5Monster::ChannelSurveyTelemetry telemetry = {};
    const bool hasTelemetry =
        C5Monster::getChannelSurveyTelemetry(telemetry);

    canvas.fillRect(SPECTRUM_LEFT, WATERFALL_TOP,
                    SPECTRUM_WIDTH, WATERFALL_ROWS, bg);
    const uint16_t stripFrame = uiTone(fg, bg, 0.50f);
    canvas.drawFastHLine(SPECTRUM_LEFT, WATERFALL_TOP - 1,
                         SPECTRUM_WIDTH, stripFrame);
    canvas.drawFastVLine(SPECTRUM_LEFT - 1, WATERFALL_TOP,
                         WATERFALL_ROWS, stripFrame);
    canvas.drawFastVLine(SPECTRUM_RIGHT, WATERFALL_TOP,
                         WATERFALL_ROWS, stripFrame);
    canvas.setTextSize(1);

    uint16_t peak = 0u;
    for (uint8_t i = 0; i < CH5G_COMMON_COUNT; ++i) {
        const uint16_t count =
            C5Monster::getObservedChannelNetworkCount(CH5G_COMMON[i]);
        if (count > peak) peak = count;
    }
    if (peak == 0u) peak = 1u;

    const int plotBottom = WATERFALL_TOP + WATERFALL_ROWS - 2;
    const int plotHeight = WATERFALL_ROWS - 4;
    for (uint8_t i = 0; i < CH5G_COMMON_COUNT; ++i) {
        const uint8_t channel = CH5G_COMMON[i];
        const int x = freqToXCached(channelToFreqF(channel));
        if (x < SPECTRUM_LEFT || x >= SPECTRUM_RIGHT) continue;
        const uint16_t count =
            C5Monster::getObservedChannelNetworkCount(channel);
        if (count == 0u) {
            drawClippedSpectrumHLine(
                canvas, x - 1, plotBottom, 3, uiTone(fg, bg, 0.62f));
            continue;
        }
        const int barHeight = constrain(
            ((int)count * plotHeight) / (int)peak, 1, plotHeight);
        const int barLeft =
            x - 2 < SPECTRUM_LEFT ? SPECTRUM_LEFT : x - 2;
        const int barRight =
            x + 2 > SPECTRUM_RIGHT ? SPECTRUM_RIGHT : x + 2;
        canvas.fillRect(barLeft, plotBottom - barHeight + 1,
                        barRight - barLeft, barHeight, fg);
    }

    char label[28];
    if (!hasTelemetry) {
        snprintf(label, sizeof(label), "WAIT");
    } else if (telemetry.capturing) {
        snprintf(label, sizeof(label), "SCANNING");
    } else if (!telemetry.hasCompletedSample) {
        snprintf(label, sizeof(label), "WAIT");
    } else if (telemetry.ageMs > 30000u) {
        snprintf(label, sizeof(label), "R%lu STALE",
                 (unsigned long)telemetry.revision);
    } else {
        snprintf(label, sizeof(label), "R%lu",
                 (unsigned long)telemetry.revision);
    }
    const int labelW = canvas.textWidth(label) + 4;
    canvas.fillRect(SPECTRUM_LEFT + 1, WATERFALL_TOP + 1,
                    labelW, 9, bg);
    canvas.setTextColor(fg, bg);
    canvas.setTextDatum(TL_DATUM);
    canvas.drawString(label, SPECTRUM_LEFT + 3, WATERFALL_TOP + 2);

    char range[20];
    snprintf(range, sizeof(range), "%u-%u",
             (unsigned)(cachedDisplayLeftFreq + 0.5f),
             (unsigned)(cachedDisplayRightFreq + 0.5f));
    const int rangeW = canvas.textWidth(range) + 4;
    canvas.fillRect(SPECTRUM_RIGHT - rangeW, WATERFALL_TOP + 1,
                    rangeW, 9, bg);
    canvas.setTextDatum(TR_DATUM);
    canvas.drawString(range, SPECTRUM_RIGHT - 2, WATERFALL_TOP + 2);
    canvas.setTextDatum(TL_DATUM);
}

static void drawC5ObserverStrip(M5Canvas& canvas, uint16_t fg,
                                uint16_t bg) {
    const C5Monster::Operation operation =
        C5Monster::getActiveOperation();
    if (operation == C5Monster::Operation::PACKET_MONITOR) {
        drawC5PacketRateStrip(canvas, fg, bg);
    } else if (operation == C5Monster::Operation::CHANNEL_VIEW) {
        drawC5ChannelMapStrip(canvas, fg, bg);
    } else {
        drawC5SnapshotStrip(canvas, fg, bg);
    }
}

// Sinc skirts below this fraction of the peak are a ragged 1px hairline at
// this resolution. Dropping them cleans the tails without touching the
// measured center energy.
static constexpr float SINC_TAIL_FLOOR = 0.025f;

// Draw sinc-based RF carrier wave lobe (realistic side lobes)
static void drawSincLobe(M5Canvas& canvas, float centerFreqMHz,
                        int8_t rssi, bool filled, uint16_t color,
                        float bandwidthScale = 1.0f,
                        int clipLeftX = SPECTRUM_LEFT,
                        int clipRightX = SPECTRUM_RIGHT - 1,
                        int baseY = SPECTRUM_BOTTOM) {
    int peakY = rssiToY(rssi);
    if (peakY >= baseY) return;

    int lobeHeight = baseY - peakY;
    if (lobeHeight <= 0) return;

    // Bound by the visible pane, then walk physical columns. A fixed 0.5MHz
    // walk samples each column more than five times at an 800MHz 5GHz view;
    // the same-x drawLines turn an outline into a misleading solid stripe.
    float visLeftFreq = cachedDisplayLeftFreq - 1.0f;
    float visRightFreq = cachedDisplayRightFreq + 1.0f;
    bandwidthScale = constrain(bandwidthScale, 0.75f, 2.0f);
    const float halfSpan = 22.0f * bandwidthScale;
    float startFreq = centerFreqMHz - halfSpan;
    float endFreq = centerFreqMHz + halfSpan;
    if (startFreq < visLeftFreq) startFreq = visLeftFreq;
    if (endFreq > visRightFreq) endFreq = visRightFreq;

    int x0 = max(freqToXCached(startFreq) - 1, clipLeftX);
    int x1 = min(freqToXCached(endFreq) + 1, clipRightX);
    x0 = max(x0, SPECTRUM_LEFT);
    x1 = min(x1, SPECTRUM_RIGHT - 1);
    if (x0 > x1) return;

    int prevX = -1;
    int prevY = baseY;

    for (int x = x0; x <= x1; ++x) {
        const float dist = xToFreqCached(x) - centerFreqMHz;
        const float amp = getSincAmplitude(dist / bandwidthScale);
        if (amp < SINC_TAIL_FLOOR) {
            // Land the skirt. Entering the lobe, the first above-floor sample
            // already draws up from prevY == baseY; leaving it, nothing was
            // drawn at all, so every outline's trailing edge stopped in
            // mid-air while its leading edge touched down.
            if (!filled && prevY < baseY &&
                prevX >= SPECTRUM_LEFT && prevX < SPECTRUM_RIGHT) {
                canvas.drawLine(prevX, prevY, x, baseY, color);
            }
            prevX = x;
            prevY = baseY;
            continue;
        }

        const int y = constrain(
            baseY - (int)(lobeHeight * amp),
            SPECTRUM_PLOT_TOP, baseY);
        
        if (filled) {
            // Filled carriers speak the waterfall's density grammar: the
            // column's share of the peak height picks the same deterministic
            // lattice a waterfall cell of that intensity would use. Renderer
            // only — the height still comes from the measured amplitude, and
            // nothing is ever painted above the crest.
            if (y < baseY) {
                const int columnH = baseY - y;
                const uint8_t intensity = static_cast<uint8_t>(
                    constrain((columnH * 255) / lobeHeight, 0, 255));
                // Crest stays solid so the silhouette survives the sparse
                // tiers instead of dissolving into the skirts.
                canvas.drawPixel(x, y, color);
                // Batch consecutive cells into drawFastVLine() runs.
                int runStart = -1;
                for (int py = y + 1; py <= baseY; ++py) {
                    const bool cell =
                        py < baseY &&
                        SpectrumThruMath::orderedDensityCellVisible(
                            intensity,
                            static_cast<uint16_t>(x - SPECTRUM_LEFT),
                            static_cast<uint16_t>(py));
                    if (cell && runStart < 0) {
                        runStart = py;
                    } else if (!cell && runStart >= 0) {
                        canvas.drawFastVLine(x, runStart,
                                             py - runStart, color);
                        runStart = -1;
                    }
                }
            }
        } else {
            // Outline: connect to previous point
            if (prevX >= SPECTRUM_LEFT && prevX < SPECTRUM_RIGHT && (prevY < baseY || y < baseY)) {
                canvas.drawLine(prevX, prevY, x, y, color);
            }
        }
        
        prevX = x;
        prevY = y;
    }
}

static void drawGaussianLobe(M5Canvas& canvas, float centerFreqMHz,
                             int8_t rssi, bool filled, uint16_t color,
                             int clipLeftX = SPECTRUM_LEFT,
                             int clipRightX = SPECTRUM_RIGHT - 1,
                             int baseY = SPECTRUM_BOTTOM) {
    int peakY = rssiToY(rssi);
    if (peakY >= baseY) return;

    // Match sinc outlines: a model sample belongs to one screen column, not
    // one arbitrary frequency step. This keeps the 11b fallback stable as the
    // viewport width changes.
    float visLeftFreq = cachedDisplayLeftFreq - 1.0f;
    float visRightFreq = cachedDisplayRightFreq + 1.0f;
    float startFreq = centerFreqMHz - 15.0f;
    float endFreq = centerFreqMHz + 15.0f;
    if (startFreq < visLeftFreq) startFreq = visLeftFreq;
    if (endFreq > visRightFreq) endFreq = visRightFreq;

    int x0 = max(freqToXCached(startFreq) - 1, clipLeftX);
    int x1 = min(freqToXCached(endFreq) + 1, clipRightX);
    x0 = max(x0, SPECTRUM_LEFT);
    x1 = min(x1, SPECTRUM_RIGHT - 1);
    if (x0 > x1) return;

    int lobeHeight = baseY - peakY;
    int prevX = -1;
    int prevY = baseY;

    for (int x = x0; x <= x1; ++x) {
        const float dist = xToFreqCached(x) - centerFreqMHz;
        const float amplitude = getGaussianAmplitude(dist);
        const int y = constrain(baseY - (int)(lobeHeight * amplitude),
                                SPECTRUM_PLOT_TOP, baseY);

        if (prevX >= SPECTRUM_LEFT && prevX < SPECTRUM_RIGHT) {
            if (filled) {
                if (y < baseY) {
                    canvas.drawFastVLine(x, y, baseY - y, color);
                }
            } else if (prevY < baseY || y < baseY) {
                // Same guard the sinc outline uses. The 11b envelope reaches
                // zero 15MHz out, so without it every legacy lobe repainted
                // the axis in the model colour across its whole span.
                canvas.drawLine(prevX, prevY, x, y, color);
            }
        }
        prevX = x;
        prevY = y;
    }
}

static void drawModeledNetworkLobe(
        M5Canvas& canvas, const SpectrumNetwork& network,
        uint16_t color,
        int clipLeftX = SPECTRUM_LEFT,
        int clipRightX = SPECTRUM_RIGHT - 1,
        int baseY = SPECTRUM_BOTTOM) {
    const float center = channelToFreqF(network.channel);
    if (network.phyMode == 1u) {
        // Legacy DSSS/11b is shown with the existing smooth envelope rather
        // than pretending it has the same OFDM skirt as HT/11g.
        drawGaussianLobe(
            canvas, center, network.rssi, false, color, clipLeftX,
            clipRightX, baseY);
        return;
    }

    const float widthScale = network.channelWidth != 0u ? 1.82f : 1.0f;
    drawSincLobe(
        canvas, center, network.rssi, false, color, widthScale,
        clipLeftX, clipRightX, baseY);
}

static void drawMeasuredCarrierLobe(
        M5Canvas& canvas, uint8_t channel,
        const MeasuredTraceState& trace,
        const RfMeasurement::ChannelEvidence& evidence,
        uint16_t color, int clipLeftX, int clipRightX) {
    const uint8_t packetTexture = carrierPacketTexture(evidence);
    if (packetTexture == 0u || trace.liveDisplayAlpha <= 8u) return;

    const SpectrumNetwork* network =
        strongestNativeNetworkOnChannel(channel);
    const float centerFreq = channelToFreqF(channel);
    const float widthScale =
        network && network->channelWidth != 0u ? 1.82f : 1.0f;
    const bool dsss = network && network->phyMode == 1u;
    const float halfSpan = dsss ? 15.0f : 22.0f * widthScale;

    // Column space, not frequency space -- the same walk stageWaterfallTarget()
    // uses to build a waterfall row. Stepping frequency by 0.5MHz lands a
    // 1px column every 4.1px at the 2.4GHz view scale (290px / 35MHz), so 3 of
    // every 4 columns stayed blank and the carrier field read as pinstripes
    // rather than texture. It also put the plot band on a different
    // column->frequency mapping than the waterfall directly below it, which
    // shares this x axis. Iterating x fixes both and lets the caller's reveal
    // slice bound the work instead of paying for the whole band every frame.
    int x0 = max(freqToXCached(centerFreq - halfSpan), clipLeftX);
    int x1 = min(freqToXCached(centerFreq + halfSpan), clipRightX);
    if (x0 < SPECTRUM_LEFT) x0 = SPECTRUM_LEFT;
    if (x1 > SPECTRUM_RIGHT - 1) x1 = SPECTRUM_RIGHT - 1;
    if (x0 > x1) return;

    const int baseY = SPECTRUM_BOTTOM;
    const int peakY = traceX8ToY(trace.liveDisplayX8);
    const int lobeHeight = baseY - peakY;
    if (lobeHeight <= 0) return;
    const uint8_t peakDensity = static_cast<uint8_t>(
        (static_cast<uint16_t>(packetTexture) *
         trace.liveDisplayAlpha) / 255u);

    for (int x = x0; x <= x1; ++x) {
        const float freq = xToFreqCached(x);
        const float amplitude =
            carrierProfileAmplitude(network, freq - centerFreq);
        if (amplitude <= 0.0f) continue;
        const int topY = constrain(
            baseY - static_cast<int>(lobeHeight * amplitude),
            SPECTRUM_PLOT_TOP, baseY);
        // The waterfall below grades the same profile from dense crest to
        // sparse skirt. Scale the carrier's ordered fill once per column so
        // both surfaces add the same nested dots as evidence strengthens.
        const uint8_t columnDensity = static_cast<uint8_t>(constrain(
            static_cast<int>(peakDensity * amplitude + 0.5f), 0, 255));
        // Coverage is the ordered lattice's job. The old y+=2 stride was a
        // second, undeclared density divider fighting the one density model.
        for (int y = topY; y < baseY; ++y) {
            if (SpectrumThruMath::orderedDensityCellVisible(
                    columnDensity,
                    static_cast<uint16_t>(x - SPECTRUM_LEFT),
                    static_cast<uint16_t>(y))) {
                canvas.drawPixel(x, y, color);
            }
        }
    }
}

// Return a stable per-channel noise floor Y anchor for selection emphasis.
static int selectedNetworkNoiseFloorY(uint8_t channel) {
    if (!measuredSweepValid || !measuredSweep ||
        channel < 1u || channel > 13u) {
        return SPECTRUM_BOTTOM;
    }

    const RfMeasurement::ChannelEvidence& evidence =
        measuredSweep->channels[channel];
    if (evidence.sampleCount == 0u ||
        evidence.medianNoiseFloor >= 0) {
        return SPECTRUM_BOTTOM;
    }

    return constrain(rssiToY(evidence.medianNoiseFloor),
                     SPECTRUM_PLOT_TOP, SPECTRUM_BOTTOM);
}

// Selection is UI chrome, not an amplitude claim. Draw it after the measured
// field so the chosen AP remains legible even when its completed-sweep texture
// is dense. The outline and cutoff brackets still state only the scan-derived
// centre/width model; completed evidence remains the filled carrier below it.
static void drawSelectedModelCarrier(M5Canvas& canvas,
                                     const SpectrumNetwork& network,
                                     uint16_t color, int clipLeftX,
                                     int clipRightX) {
    const int selectedFloorY = selectedNetworkNoiseFloorY(network.channel);
    const int selectedPeakY = rssiToY(network.rssi);
    const float selectedCenter = channelToFreqF(network.channel);
    drawModeledNetworkLobe(canvas, network, color, clipLeftX, clipRightX,
                           selectedFloorY);

    // Occupancy brackets make the scan-derived channel width explicit rather
    // than allowing the sinc skirt to masquerade as an RF measurement.
    const float halfOccupancyMHz =
        network.channelWidth != 0u ? 20.0f : 10.0f;
    const int bracketTop = constrain(min(selectedPeakY, selectedFloorY),
                                     SPECTRUM_PLOT_TOP, SPECTRUM_BOTTOM);
    const int bracketBottom = constrain(max(selectedPeakY, selectedFloorY),
                                        SPECTRUM_PLOT_TOP, SPECTRUM_BOTTOM);
    const int bracketSpan = bracketBottom - bracketTop;
    constexpr int kBracketSerif = 3;
    if (bracketSpan <= 0) return;

    const int leftX =
        freqToXCached(selectedCenter - halfOccupancyMHz);
    if (leftX >= clipLeftX && leftX <= clipRightX &&
        leftX >= SPECTRUM_LEFT && leftX < SPECTRUM_RIGHT) {
        const int foot = min(kBracketSerif, SPECTRUM_RIGHT - leftX);
        canvas.drawFastVLine(leftX, bracketTop, bracketSpan + 1, color);
        canvas.drawFastHLine(leftX, bracketTop, foot, color);
        canvas.drawFastHLine(leftX, bracketBottom, foot, color);
    }

    const int rightX =
        freqToXCached(selectedCenter + halfOccupancyMHz);
    if (rightX >= clipLeftX && rightX <= clipRightX &&
        rightX >= SPECTRUM_LEFT && rightX < SPECTRUM_RIGHT) {
        const int foot = min(kBracketSerif, rightX - SPECTRUM_LEFT + 1);
        canvas.drawFastVLine(rightX, bracketTop, bracketSpan + 1, color);
        canvas.drawFastHLine(rightX - foot + 1, bracketTop, foot, color);
        canvas.drawFastHLine(rightX - foot + 1, bracketBottom, foot, color);
    }
}

static void drawMeasuredEvidence(M5Canvas& canvas, uint16_t fg,
                                 uint16_t bg, int clipLeftX,
                                 int clipRightX, uint8_t selectedChannel) {
    // Progressive reveal is the caller's per-frame clip slice
    // (drawSpectrumCarriers), which confines the repaint to the columns the
    // sweep head just crossed so the rest of the band keeps its previous
    // reveal instead of blanking each lap. Columns outside that slice are
    // discarded by the clip rect anyway, so bound the loops by it rather than
    // walking the whole band and throwing the work away.
    const uint16_t quietColor =
        Display::lerpColor565(fg, bg, 0.54f);
    for (int x = clipLeftX; x <= clipRightX; ++x) {
        for (int y = SPECTRUM_BOTTOM - 2;
             y < SPECTRUM_BOTTOM; ++y) {
            if (SpectrumThruMath::orderedDensityCellVisible(
                    WATERFALL_QUIET_FLOOR,
                    static_cast<uint16_t>(x - SPECTRUM_LEFT),
                    static_cast<uint16_t>(y))) {
                canvas.drawPixel(x, y, quietColor);
            }
        }
    }
    if (!measuredSweepValid || !measuredSweep) return;

    // The selected AP's channel carries the full-strength field; every other
    // channel is knocked back so the one being inspected is legible as figure
    // against the rest of the band as ground. Selection is per-AP but measured
    // evidence is per-channel, so this lights the channel the selection sits
    // on -- co-channel neighbours are lit with it, which is honest: the
    // measurement cannot separate them. With nothing selected there is no
    // figure to separate, so the whole band stays lit rather than uniformly
    // knocked back.
    const bool hasSelection = selectedChannel != 0u;
    const uint16_t contextColor = Display::lerpColor565(fg, bg, 0.52f);
    for (uint8_t channel = 1u; channel <= 13u; ++channel) {
        const MeasuredTraceState& trace = measuredTrace[channel];
        drawMeasuredCarrierLobe(
            canvas, channel, trace,
            measuredSweep->channels[channel],
            (!hasSelection || channel == selectedChannel) ? fg : contextColor,
            clipLeftX, clipRightX);
    }

    const uint16_t averageColor =
        Display::lerpColor565(fg, bg, 0.40f);
    const uint16_t maxHoldColor =
        Display::lerpColor565(fg, bg, 0.18f);
    const uint16_t spreadColor =
        Display::lerpColor565(fg, bg, 0.62f);

    for (uint8_t channel = 1u; channel <= 13u; ++channel) {
        const MeasuredTraceState& trace = measuredTrace[channel];
        const int x = freqToXCached(channelToFreqF(channel));
        if (x < SPECTRUM_LEFT || x >= SPECTRUM_RIGHT ||
            !trace.covered) {
            continue;
        }

        // Session average and max hold remain visible through a quiet dwell.
        // A quiet packet dwell is not a calibrated power-floor sample, so it
        // does not erase prior measured evidence.
        if (trace.averageValid) {
            const int averageY = traceX8ToY(trace.averageDisplayX8);
            canvas.drawPixel(x - 1, averageY, averageColor);
            canvas.drawPixel(x + 1, averageY, averageColor);
        }
        if (trace.maxHoldValid) {
            const int maxY = traceX8ToY(trace.maxHoldDisplayX8);
            canvas.drawPixel(x, maxY, maxHoldColor);
            if (maxY > SPECTRUM_PLOT_TOP) {
                canvas.drawPixel(x, maxY - 1, maxHoldColor);
            }
        }

        if (trace.liveDisplayAlpha > 8u && trace.spreadDb > 0u) {
            const int halfSpreadX8 =
                constrain(static_cast<int>(trace.spreadDb) * 4, 4, 96);
            const int spreadTop = traceX8ToY(static_cast<int16_t>(
                trace.liveDisplayX8 + halfSpreadX8));
            const int spreadBottom = traceX8ToY(static_cast<int16_t>(
                trace.liveDisplayX8 - halfSpreadX8));
            if (spreadBottom > spreadTop) {
                canvas.drawPixel(x - 1, spreadTop, spreadColor);
                canvas.drawPixel(x - 1, spreadBottom, spreadColor);
            }
        }
    }
}

static void drawAnalyzerSweepHead(M5Canvas& canvas, uint16_t fg,
                                  uint16_t bg) {
    if (!rfMeasurements) return;

    const int x = SPECTRUM_LEFT +
        SpectrumThruMath::analyzerSweepColumn(
            analyzerSweepPhase, SPECTRUM_WIDTH);
    const uint16_t head = uiTone(fg, bg, 0.88f);
    const uint16_t trail = uiTone(fg, bg, 0.58f);

    // A crisp cap makes the acquisition head easy to find; the dotted body
    // remains visually secondary to measured carrier evidence.
    const int capLeft = max(SPECTRUM_LEFT, x - 2);
    const int capRight = min(SPECTRUM_RIGHT - 1, x + 2);
    canvas.drawFastHLine(capLeft, SPECTRUM_PLOT_TOP + 1,
                         capRight - capLeft + 1, head);
    for (int y = SPECTRUM_PLOT_TOP + 3;
         y < SPECTRUM_BOTTOM - 1; y += 4) {
        canvas.drawPixel(x, y, ((y / 4) & 1) ? head : trail);
    }
    canvas.drawPixel(x, SPECTRUM_BOTTOM - 1, head);
}

// Rebuild one physical slice of the carrier plot. Progressive reveal normally
// calls this for the newly crossed columns; on a sweep wrap it also restores
// the retired head column so the old acquisition cursor cannot linger as a
// second, false scan head.
static void drawSpectrumPlotSlice(M5Canvas& canvas, uint16_t fg,
                                  uint16_t bg, bool is5g,
                                  uint8_t selectedCh, uint16_t muted,
                                  uint16_t faint, int clipLeftX,
                                  int clipRightX, bool drawSweepHead) {
    // Y-axis line
    canvas.drawFastVLine(SPECTRUM_LEFT - 2, SPECTRUM_PLOT_TOP,
                         SPECTRUM_BOTTOM - SPECTRUM_PLOT_TOP + 1, fg);

    // dB labels
    canvas.setTextSize(1);
    canvas.setTextColor(fg);
    canvas.setTextDatum(MR_DATUM);
    for (int8_t rssi = -30; rssi >= -90; rssi -= 20) {
        int y = rssiToY(rssi);
        int labelY = constrain(y, SPECTRUM_PLOT_TOP + 4,
                              SPECTRUM_BOTTOM - 4);
        canvas.drawFastHLine(SPECTRUM_LEFT - 4, y, 3, fg);
        for (int x = SPECTRUM_LEFT; x < SPECTRUM_RIGHT; x += 8) {
            drawClippedSpectrumHLine(canvas, x, y, 4, faint);
        }
        char rssiBuf[8]; snprintf(rssiBuf, sizeof(rssiBuf), "%d", rssi);
        canvas.drawString(rssiBuf, SPECTRUM_LEFT - 5, labelY);
    }

    // Baseline
    canvas.drawFastHLine(SPECTRUM_LEFT, SPECTRUM_BOTTOM,
                         SPECTRUM_RIGHT - SPECTRUM_LEFT, muted);
    drawBandSeparators(canvas, is5g, fg, bg);

    if (is5g) {
        // ==[ 5GHz MODE: one snapshot-derived model per C5 scan row ]==
        // Do not collapse co-channel BSSIDs into a single peak: nested outlines
        // preserve each AP's reported RSSI. A scan row is a static, age-labelled
        // AP snapshot, not a measured power spectrum; packet rate never
        // animates the model height.
        for (uint8_t i = 0; i < c5gNetworkCount; ++i) {
            if ((int8_t)i == c5gSelectedIdx) {
                continue;
            }
            const uint8_t ch = c5gNetworks[i].channel;
            drawSincLobe(canvas, channelToFreqF(ch), c5gNetworks[i].rssi,
                         false, muted);
        }

        if (c5gSelectedIdx >= 0 && c5gSelectedIdx < c5gNetworkCount) {
            const C5GHzNetwork& selected = c5gNetworks[c5gSelectedIdx];
            drawSincLobe(canvas, channelToFreqF(selected.channel),
                         selected.rssi, false, fg);
            const int selectedX =
                freqToXCached(channelToFreqF(selected.channel));
            const int selectedY = rssiToY(selected.rssi);
            if (selectedX >= SPECTRUM_LEFT &&
                selectedX < SPECTRUM_RIGHT) {
                const int markerLeft = max(SPECTRUM_LEFT, selectedX - 1);
                const int markerTop = max(SPECTRUM_PLOT_TOP, selectedY - 1);
                const int markerRight = min(SPECTRUM_RIGHT - 1,
                                            selectedX + 1);
                const int markerBottom = min(SPECTRUM_BOTTOM,
                                             selectedY + 1);
                canvas.fillRect(markerLeft, markerTop,
                                markerRight - markerLeft + 1,
                                markerBottom - markerTop + 1, fg);
            }
        }
        return;
    }

    // ==[ 2.4GHz MODEL OVERLAY ]== individual AP outlines remain explicitly
    // secondary to the completed-sweep stippled carrier field.
    if (modelOverlayEnabled) {
        const uint16_t modelColor = Display::lerpColor565(fg, bg, 0.48f);
        for (uint16_t i = 0; i < networkCount; i++) {
            if ((int16_t)i == selectedIdx) continue;
            drawModeledNetworkLobe(canvas, networks[i], modelColor,
                                   clipLeftX, clipRightX);
        }
    }

    // Completed-sweep measured evidence owns amplitude, sample coverage,
    // spread, average, and max hold.
    drawMeasuredEvidence(canvas, fg, bg, clipLeftX, clipRightX, selectedCh);
    if (drawSweepHead) drawAnalyzerSweepHead(canvas, fg, bg);

    // Measured noise-floor markers belong to the same clipped slice, or a
    // drifting floor would accumulate between sweeps.
    drawNoiseFloor(canvas, fg);

    // The selected model is a thin final overlay. It identifies the selected
    // AP but never substitutes scan metadata for measured power.
    if (modelOverlayEnabled && selectedIdx >= 0 &&
        selectedIdx < (int16_t)networkCount) {
        drawSelectedModelCarrier(canvas, networks[selectedIdx], fg,
                                 clipLeftX, clipRightX);
    }
}

// Cheap structural hash of the current 2.4GHz network set. Only channels enter
// it: RSSI/trace easing changes every frame and must NOT trigger a repaint, or
// the progressive reveal would degrade into a full repaint every frame.
static uint32_t spectrumNetworkChannelHash() {
    uint32_t hash = 2166136261u;
    for (uint16_t i = 0u; i < networkCount; ++i) {
        hash = (hash ^ networks[i].channel) * 16777619u;
    }
    return hash;
}

// Decide whether the carrier plot band may persist this frame. Only the plain,
// swept 2.4GHz measured analyzer with an unchanged structural signature keeps
// the previous reveal; every other view or any structural change repaints in
// full. Always run once per frame (from beginFrame) so the stored signature
// tracks the immediately previous frame.
static bool spectrumEvaluatePlotReveal() {
    const uint32_t channelHash = spectrumNetworkChannelHash();
    const bool showing5g = isShowing5GHz();
    const bool eligible =
        !clientMode && !showing5g && !dialMode &&
        !paranoid.attackActive && !isDetailToastVisible() &&
        measuredSweepValid && !c5CarrierDetailActive &&
        !clientDetailActive && rogueAlertCount == 0u &&
        spectrumRevealPrevX >= 0;

    // A new sweep epoch is deliberately absent here: landing evidence is not a
    // structural change. Including it blanked the band and repainted all 13
    // channels in one frame, which is the pop the progressive reveal exists to
    // avoid. New evidence is staged per channel and painted by the head as it
    // crosses. The ordered carrier lattice is stable; only source-backed
    // amplitude and density change as evidence is released.
    const bool stable =
        spectrumLastPlotSig.viewCenter == viewCenterMHz &&
        spectrumLastPlotSig.viewWidth == viewWidthMHz &&
        spectrumLastPlotSig.selected == static_cast<int32_t>(selectedIdx) &&
        spectrumLastPlotSig.netCount == static_cast<uint32_t>(networkCount) &&
        spectrumLastPlotSig.channelHash == channelHash &&
        spectrumLastPlotSig.modelOverlay == modelOverlayEnabled &&
        spectrumLastPlotSig.measured == measuredSweepValid &&
        spectrumLastPlotSig.band5g == showing5g &&
        spectrumLastPlotSig.client == clientMode;

    // Refresh the signature for next frame's comparison regardless of outcome.
    spectrumLastPlotSig.viewCenter = viewCenterMHz;
    spectrumLastPlotSig.viewWidth = viewWidthMHz;
    spectrumLastPlotSig.selected = static_cast<int32_t>(selectedIdx);
    spectrumLastPlotSig.netCount = static_cast<uint32_t>(networkCount);
    spectrumLastPlotSig.channelHash = channelHash;
    spectrumLastPlotSig.modelOverlay = modelOverlayEnabled;
    spectrumLastPlotSig.measured = measuredSweepValid;
    spectrumLastPlotSig.band5g = showing5g;
    spectrumLastPlotSig.client = clientMode;

    return eligible && stable;
}

// Per-frame canvas clear for the spectrum screen. On a progressive-reveal frame
// the carrier plot band (SPECTRUM_PLOT_TOP..SPECTRUM_BOTTOM) is preserved in the
// shared sprite so the sweep can repaint only the columns it crossed; every
// other row is cleared as before. Otherwise the whole sprite is wiped, matching
// the historical behaviour. Called from Display::drawSpectrumScreen in place of
// the blanket fillSprite.
void beginFrame(M5Canvas& canvas) {
    const uint16_t bg = Display::getColorBG();
    // If the spectrum screen was not rendered last frame (mode switch, popup,
    // or any overlay reused the shared canvas), the preserved band is stale.
    // Force one full repaint so nothing ghosts through.
    static uint32_t lastBeginMs = 0u;
    const uint32_t now = millis();
    if (now - lastBeginMs > 150u) spectrumRevealPrevX = -1;
    lastBeginMs = now;

    // A popup that overlaps the plot band (paranoid detail toast, C5 carrier
    // detail) leaves an imprint when it clears. Force a full repaint on the
    // frame it disappears so the band is rebuilt clean.
    static bool bandOverlayWasUp = false;
    const bool bandOverlayUp = isDetailToastVisible() || c5CarrierDetailActive;
    if (bandOverlayWasUp && !bandOverlayUp) spectrumRevealPrevX = -1;
    bandOverlayWasUp = bandOverlayUp;

    spectrumPlotIncremental = spectrumEvaluatePlotReveal();
    if (!spectrumPlotIncremental) {
        canvas.fillSprite(bg);
        return;
    }
    // Preserve rows [SPECTRUM_PLOT_TOP..SPECTRUM_BOTTOM]; clear above and below.
    canvas.fillRect(0, 0, SCREEN_WIDTH, SPECTRUM_PLOT_TOP, bg);
    canvas.fillRect(0, SPECTRUM_BOTTOM + 1, SCREEN_WIDTH,
                    SCREEN_HEIGHT - (SPECTRUM_BOTTOM + 1), bg);
}

static void drawSpectrumCarriers(M5Canvas& canvas, uint16_t fg,
                                 uint16_t bg) {
    const bool is5g = isShowing5GHz();
    const bool hideEmpty5g = is5g && (c5gNetworkCount > 0);
    const bool hideEmpty2g = !is5g && (networkCount > 0);
    uint8_t selected5gCh = 0;
    if (c5gSelectedIdx >= 0 && c5gSelectedIdx < c5gNetworkCount) {
        selected5gCh = c5gNetworks[c5gSelectedIdx].channel;
    }
    uint8_t selectedCh = 0;
    if (selectedIdx >= 0 && selectedIdx < (int16_t)networkCount) {
        selectedCh = networks[selectedIdx].channel;
    }

    const uint16_t muted = uiTone(fg, bg, 0.52f);
    const uint16_t faint = uiTone(fg, bg, 0.68f);

    // ==[ PROGRESSIVE REVEAL ]== The carrier plot band persists between frames
    // (Spectrum::beginFrame preserved it). Repaint only the columns the sweep
    // head crossed since last frame; the header, waterfall, and label rows were
    // already cleared for us. A structural change, a sweep wrap, a large jump,
    // or any non-swept view falls back to a full plot-band repaint. The clip
    // rect confines every draw below to the revealed slice.
    const int plotBandH = SPECTRUM_BOTTOM - SPECTRUM_PLOT_TOP + 1;
    const int sweepHeadX = SPECTRUM_LEFT +
        SpectrumThruMath::analyzerSweepColumn(analyzerSweepPhase,
                                              SPECTRUM_WIDTH);
    constexpr int kRevealPad = 3;        // erases the sweep-head cap (+/-2)
    constexpr int kRevealMaxSlice = 96;  // wider jump -> full repaint is cheaper
    bool clipActive = false;
    int activeSliceL = 0;
    int activeSliceR = SCREEN_WIDTH - 1;
    // Column range the measured layer may actually paint into this frame.
    // A full repaint owns the whole plot width.
    int revealClipL = SPECTRUM_LEFT;
    int revealClipR = SPECTRUM_RIGHT - 1;
    // A head that moved backwards is a pass restarting, not a discontinuity:
    // publishMeasuredTrace() zeroes the sweep phase every time real evidence
    // lands. Repaint from the left edge up to the new head and let the rest of
    // the band keep showing the previous pass until this one sweeps over it.
    // Treating the restart as ineligible repainted all 13 channels in one
    // frame, which is exactly the band-wide pop the reveal exists to avoid.
    const int previousSweepHeadX = spectrumRevealPrevX;
    const bool sweepRestarted =
        previousSweepHeadX >= 0 && sweepHeadX < previousSweepHeadX;
    const int revealFromX =
        sweepRestarted ? SPECTRUM_LEFT : spectrumRevealPrevX;
    if (!is5g && spectrumPlotIncremental && spectrumRevealPrevX >= 0 &&
        sweepHeadX >= revealFromX &&
        (sweepHeadX - revealFromX) <= kRevealMaxSlice) {
        const int sliceL =
            constrain(revealFromX - kRevealPad, 0, SCREEN_WIDTH - 1);
        const int sliceR =
            constrain(sweepHeadX + kRevealPad, 0, SCREEN_WIDTH - 1);
        canvas.fillRect(sliceL, SPECTRUM_PLOT_TOP,
                        sliceR - sliceL + 1, plotBandH, bg);
        clipActive = true;
        activeSliceL = sliceL;
        activeSliceR = sliceR;
        revealClipL = max(sliceL, SPECTRUM_LEFT);
        revealClipR = min(sliceR, SPECTRUM_RIGHT - 1);
    } else {
        // Full plot-band repaint. Surrounding rows were cleared by beginFrame
        // (or the whole sprite was wiped when reveal was ineligible).
        canvas.fillRect(0, SPECTRUM_PLOT_TOP, SCREEN_WIDTH, plotBandH, bg);
    }
    spectrumRevealPrevX = sweepHeadX;

    if (sweepRestarted && clipActive) {
        // The new pass starts at the left edge, but the previous pass left its
        // cursor on the right. Restore that narrow old-head strip from the same
        // source layers before painting the new head, rather than briefly
        // showing two acquisition cursors or a cleared vertical scar.
        const int retiredSliceL = constrain(previousSweepHeadX - kRevealPad,
                                            0, SCREEN_WIDTH - 1);
        const int retiredSliceR = constrain(previousSweepHeadX + kRevealPad,
                                            0, SCREEN_WIDTH - 1);
        canvas.setClipRect(retiredSliceL, SPECTRUM_PLOT_TOP,
                           retiredSliceR - retiredSliceL + 1, plotBandH);
        canvas.fillRect(retiredSliceL, SPECTRUM_PLOT_TOP,
                        retiredSliceR - retiredSliceL + 1, plotBandH, bg);
        drawSpectrumPlotSlice(canvas, fg, bg, false, selectedCh, muted, faint,
                              max(retiredSliceL, SPECTRUM_LEFT),
                              min(retiredSliceR, SPECTRUM_RIGHT - 1), false);
    }

    if (clipActive) {
        canvas.setClipRect(activeSliceL, SPECTRUM_PLOT_TOP,
                           activeSliceR - activeSliceL + 1, plotBandH);
    }
    drawSpectrumPlotSlice(canvas, fg, bg, is5g, selectedCh, muted, faint,
                          revealClipL, revealClipR, !is5g);

    // Release the reveal slice: labels, dial box, pan arrows, and the paranoid
    // dagger sit outside the plot band or must refresh every frame.
    if (clipActive) canvas.clearClipRect();

    // ==[ DIAL MODE: SLIDING HIGHLIGHT BOX ]== (2.4GHz only)
    if (dialMode && !is5g) {
        float clampedPos = constrain(dialPositionSmooth, 1.0f, 13.0f);
        const float dialFreq = 2412.0f + (clampedPos - 1.0f) * 5.0f;
        const float xCenter = (float)freqToXCached(dialFreq);
        
        int boxW = 14;
        int boxH = 10;
        int boxY = CHANNEL_LABEL_Y - 1;
        int boxX = (int)(xCenter - boxW / 2);

        if (boxX + boxW > SPECTRUM_LEFT && boxX < SPECTRUM_RIGHT) {
            const int borderInset = dialLocked ? 1 : 0;
            boxX = constrain(boxX, SPECTRUM_LEFT + borderInset,
                             SPECTRUM_RIGHT - boxW - borderInset);
            canvas.fillRect(boxX, boxY, boxW, boxH, fg);

            if (dialLocked) {
                canvas.drawRect(boxX - 1, boxY - 1,
                                boxW + 2, boxH + 2, fg);
            }
        }
    }

// Channel markers - prevent label overlap
    canvas.setTextDatum(TC_DATUM);
    int lastLabelRight = -999;
    if (is5g) {
        // 5GHz channel labels: show common UNII channels
        for (uint8_t i = 0; i < CH5G_COMMON_COUNT; i++) {
            uint8_t ch = CH5G_COMMON[i];
            float freq = channelToFreqF(ch);
            int x = freqToXCached(freq);
            if (x >= SPECTRUM_LEFT && x < SPECTRUM_RIGHT) {
                bool hasSignal = (Spectrum::get5GHzChannelNetworkCount(ch) > 0);
                bool showChannel = (!hideEmpty5g || hasSignal || (selected5gCh == ch));
                if (!showChannel) continue;
                
                // Check for label overlap (textSize 1 = 6px/char, channel num up to 3 chars = 18px)
                int labelWidth = (ch >= 100 ? 18 : 12);
                int labelLeft = x - labelWidth / 2;
                if (labelLeft < SPECTRUM_LEFT ||
                    labelLeft + labelWidth > SPECTRUM_RIGHT) {
                    continue;
                }
                if (labelLeft < lastLabelRight + 2) continue;  // 2px gap minimum
                
                const bool selectedChannel = selected5gCh == ch;
                const uint16_t labelColor = selectedChannel ? fg : muted;
                canvas.drawFastVLine(x, CHANNEL_LABEL_Y - 1, 1, labelColor);
                canvas.setTextColor(labelColor, bg);
                char chBuf[4]; snprintf(chBuf, sizeof(chBuf), "%d", ch);
                canvas.drawString(chBuf, x, CHANNEL_LABEL_Y);
                lastLabelRight = labelLeft + labelWidth;
            }
        }
    } else {
        for (int ch = 1; ch <= 13; ch++) {
            float freq = channelToFreqF(ch);
            int x = freqToXCached(freq);
            if (x >= SPECTRUM_LEFT && x < SPECTRUM_RIGHT) {
                uint16_t observed = Spectrum::getChannelNetworkCount((uint8_t)ch);
                bool showChannel = (!hideEmpty2g || (observed > 0) || (ch == selectedCh));
                if (!showChannel) continue;
                
                // Check for label overlap
                int labelWidth = (ch >= 10 ? 12 : 6);
                int labelLeft = x - labelWidth / 2;
                if (labelLeft < SPECTRUM_LEFT ||
                    labelLeft + labelWidth > SPECTRUM_RIGHT) {
                    continue;
                }
                if (labelLeft < lastLabelRight + 2) continue;  // 2px gap minimum
                
                const bool selectedChannel = ch == selectedCh;
                const bool isDialSelected =
                    dialMode && (fabsf(dialPositionSmooth - (float)ch) < 0.6f);
                const uint16_t tickColor =
                    (selectedChannel || isDialSelected) ? fg : muted;
                canvas.drawFastVLine(x, CHANNEL_LABEL_Y - 1, 1, tickColor);
                if (isDialSelected) {
                    canvas.setTextColor(bg, fg);
                } else {
                    canvas.setTextColor(tickColor, bg);
                }
                char chBuf[4]; snprintf(chBuf, sizeof(chBuf), "%d", ch);
                canvas.drawString(chBuf, x, CHANNEL_LABEL_Y);
                lastLabelRight = labelLeft + labelWidth;
            }
        }
    }
    canvas.setTextColor(fg, bg);  // reset

    // Pan indicators
    float leftEdge = cachedDisplayLeftFreq;
    float rightEdge = cachedDisplayRightFreq;
    float bandMin = is5g ? (float)FREQ_BAND_5G_START : (float)FREQ_BAND_2G_START;
    float bandMax = is5g ? (float)FREQ_BAND_5G_END : (float)FREQ_BAND_2G_END;
    canvas.setTextDatum(ML_DATUM);
    const int panY = (SPECTRUM_PLOT_TOP + SPECTRUM_BOTTOM) / 2;
    if (leftEdge > bandMin) canvas.drawString("<", 2, panY);
    canvas.setTextDatum(MR_DATUM);
    if (rightEdge < bandMax) canvas.drawString(">", SPECTRUM_RIGHT + 1, panY);

    // ==[ PARANOID SWINE: CW DAGGER ]== marks deauth channel
    if (paranoid.attackActive) {
        float freq = channelToFreqF(paranoid.attackChannel);
        int x = freqToXCached(freq);
        if (x >= SPECTRUM_LEFT && x < SPECTRUM_RIGHT) {
            int peakY = rssiToY(paranoid.rssiCurrent);
            int baseY = SPECTRUM_BOTTOM;
            int halfW = 8;  // Half-width of dagger base
            if (x + halfW >= SPECTRUM_RIGHT) {
                halfW = SPECTRUM_RIGHT - 1 - x;
            }
            if (x - halfW < SPECTRUM_LEFT) {
                halfW = x - SPECTRUM_LEFT;
            }

            // Triangular dagger - inverted color fill
            // Peak at current RSSI, base at spectrum bottom
            canvas.fillTriangle(
                x, peakY,              // Peak point
                x - halfW, baseY,      // Bottom left
                x + halfW, baseY,      // Bottom right
                bg                     // Inverted fill
            );
            // Outline for visibility
            canvas.drawTriangle(
                x, peakY,
                x - halfW, baseY,
                x + halfW, baseY,
                fg
            );
        }
    }

    canvas.setTextDatum(TL_DATUM);
}

// ==[ SPARKLINE RENDERER ]== 30px-wide RSSI micro-graph, 8px tall
static void drawSparkline(M5Canvas& canvas, int x, int y, uint16_t netIdx,
                           uint16_t fg, uint16_t bg) {
    if (!sparklineBuffers || !sparklineIdx || netIdx >= networkCount) return;

    const uint16_t baseline = uiTone(fg, bg, 0.64f);
    canvas.drawFastHLine(x, y + 7, SPARKLINE_SAMPLES, baseline);

    const int8_t* data = sparklineBuffers + netIdx * SPARKLINE_SAMPLES;
    const uint8_t startIdx = sparklineIdx[netIdx];
    for (int col = 0; col < SPARKLINE_SAMPLES; ++col) {
        const int8_t rssi =
            data[(startIdx + col) % SPARKLINE_SAMPLES];
        if (rssi <= -100) continue;

        const int height = constrain(map(rssi, -90, -30, 1, 8), 1, 8);
        canvas.drawFastVLine(x + col, y + 8 - height, height, fg);
    }

    // A single cursor pixel marks the newest edge of the rolling trace.
    canvas.drawPixel(x + SPARKLINE_SAMPLES - 1, y + 7, fg);
}

// ==[ ROGUE AP ALERT TOAST ]== blinking inverted bar for HIGH/CRITICAL alerts
static void drawRogueAlertToast(M5Canvas& canvas, uint16_t fg,
                                uint16_t bg) {
    if (!rogueAlerts || rogueAlertCount == 0u) return;

    uint8_t maxSeverity = 0u;
    uint8_t alertIndex = 0u;
    for (uint8_t i = 0u; i < rogueAlertCount; ++i) {
        if (rogueAlerts[i].severity > maxSeverity) {
            maxSeverity = rogueAlerts[i].severity;
            alertIndex = i;
        }
    }
    if (maxSeverity < 2u) return;

    const int y = NETWORK_LIST_Y;
    const int height = 13;
    const int tagWidth = maxSeverity >= 3u ? 34 : 32;
    const uint16_t frame = uiTone(fg, bg, 0.42f);

    // Keep the message stable. Only the tiny edge marker pulses, avoiding the
    // full-screen flicker of the old inverted toast.
    canvas.fillRect(0, y, SCREEN_WIDTH, height, bg);
    drawBracketFrame(canvas, 2, y, SCREEN_WIDTH - 4, height, frame);
    canvas.fillRect(4, y + 2, tagWidth, height - 4, fg);
    canvas.setTextSize(1);
    canvas.setTextColor(bg, fg);
    canvas.setCursor(7, y + 3);
    canvas.print(maxSeverity >= 3u ? "CRIT" : "WARN");

    char message[48];
    snprintf(message, sizeof(message), "%s: %s",
             maxSeverity >= 3u ? "SEC PROFILE" : "TWIN RISK",
             rogueAlerts[alertIndex].ssid);
    canvas.setTextColor(fg, bg);
    fitTextToWidth(canvas, message, sizeof(message),
                   SCREEN_WIDTH - tagWidth - 20);
    canvas.setCursor(tagWidth + 8, y + 3);
    canvas.print(message);

    canvas.setTextDatum(TR_DATUM);
    canvas.drawString(((cachedMillis / 400u) & 1u) ? "*" : "+",
                      SCREEN_WIDTH - 6, y + 3);
    canvas.setTextDatum(TL_DATUM);
}

// ==[ ATTACK PATH HINT ]== bottom bar shows recommended action for selected network
static const char* getAttackPathHint(uint16_t idx) {
    if (!vulnTier || idx >= networkCount) return "";
    switch (vulnTier[idx]) {
        case 0: return "OPEN - SNIFF TRAFFIC";
        case 1: return "WEAK - KEY RECOVERY";
        case 2: return "DEAUTH > HS > CRACK";
        case 3: return "ASSOC > PMKID > CRACK";
        case 4: return "DOWNGRADE > DEAUTH > HS";
        case 5: return "SAE REJECT > FALLBACK";
        case 6: return "ENTERPRISE - EVIL TWIN";
        case 7: return "FORTRESS - NO ATTACK";
        default: return "";
    }
}

static void drawNetworkList(M5Canvas& canvas, uint16_t fg, uint16_t bg) {
    using namespace UIMeasurements::Spectrum;

    const int startY = NETWORK_LIST_Y;
    const int lineHeight = kNetworkRowH;
    const int maxLines = kNetworkVisibleRows;
    const int listHeight = lineHeight * maxLines;
    const int contentBottom = SCREEN_HEIGHT - BOTTOM_BAR_H;
    const int hintY = startY + listHeight + 2;
    const uint16_t rowRule = uiTone(fg, bg, 0.68f);
    const uint16_t muted = uiTone(fg, bg, 0.54f);

    // This region changes shape when switching bands, scrolling, or opening an
    // alert. Clear it explicitly so old rows never ghost through the new list.
    canvas.fillRect(0, startY, SCREEN_WIDTH, contentBottom - startY, bg);
    drawDottedHLine(canvas, 2, startY - 2, SCREEN_WIDTH - 4,
                    rowRule, 4, 4);
    canvas.setTextSize(1);
    canvas.setTextDatum(TL_DATUM);

    if (isShowing5GHz()) {
        const int count = static_cast<int>(c5gNetworkCount);
        if (count == 0) {
            drawEmptyState(
                canvas, 0, startY, SCREEN_WIDTH, listHeight,
                c5gLastScanRevision != 0u
                    ? "NO 5GHz APs IN SNAPSHOT"
                    : "C5 SNAPSHOT PENDING",
                c5gLastScanRevision != 0u
                    ? "MOVE, PAN, OR RESCAN"
                    : "WAITING FOR FIRST COMPLETE SCAN",
                fg, bg);
            return;
        }

        int scrollOffset = 0;
        if (c5gSelectedIdx >= maxLines) {
            scrollOffset = c5gSelectedIdx - maxLines + 1;
        }
        scrollOffset = constrain(scrollOffset, 0, max(0, count - maxLines));
        const int visibleCount = min(maxLines, count - scrollOffset);

        for (int row = 0; row < visibleCount; ++row) {
            const int idx = scrollOffset + row;
            const int y = startY + row * lineHeight;
            const bool selected = idx == c5gSelectedIdx;
            const uint32_t ageMs = SpectrumThruMath::observationAgeMs(
                cachedMillis, c5gNetworks[idx].lastSeenMs);
            const bool stale = ageMs > C5G_OBSERVATION_STALE_MS;
            const uint16_t rowFg = stale && !selected ? muted : fg;
            const uint16_t textFg = selected ? bg : rowFg;
            const uint16_t textBg = selected ? fg : bg;

            drawDottedHLine(canvas, 5, y + lineHeight - 1,
                            SCREEN_WIDTH - 11, rowRule, 2, 5);
            if (selected) {
                drawSelectedRow(canvas, 2, y, SCREEN_WIDTH - 5,
                                lineHeight, fg);
            }

            const int textY = y + (lineHeight - UI_TEXT_H) / 2;
            canvas.setTextColor(textFg, textBg);
            canvas.setCursor(kTierIconX, textY);
            canvas.print(stale ? "~" : "5");

            char ssidBuf[kSsidMax + 1];
            strncpy(ssidBuf,
                    c5gNetworks[idx].ssid[0]
                        ? c5gNetworks[idx].ssid
                        : "<HIDDEN>",
                    kSsidMax);
            ssidBuf[kSsidMax] = '\0';
            fitTextToWidth(canvas, ssidBuf, sizeof(ssidBuf),
                           kSparklineX - kSsidX - 4);
            canvas.setCursor(kSsidX, textY);
            canvas.print(ssidBuf);

            canvas.setCursor(kSparklineX, textY);
            canvas.printf("CH%u", (unsigned)c5gNetworks[idx].channel);

            char age[8];
            formatAgeShort(age, sizeof(age), ageMs);
            canvas.setCursor(kClientCountX, textY);
            canvas.print(age);

            char auth[14];
            strlcpy(auth, C5Protocol::authTypeLabel(
                                c5gNetworks[idx].authType),
                    sizeof(auth));
            fitTextToWidth(canvas, auth, sizeof(auth),
                           kRssiRightX - kInfoX - 8);
            canvas.setCursor(kInfoX, textY);
            canvas.print(auth);

            char rssi[8];
            snprintf(rssi, sizeof(rssi), "%ddB", c5gNetworks[idx].rssi);
            canvas.setTextDatum(TR_DATUM);
            canvas.drawString(rssi, kRssiRightX, textY);
            canvas.setTextDatum(TL_DATUM);
        }

        drawScrollRail(canvas, SCREEN_WIDTH - 2, startY + 1,
                       listHeight - 2, scrollOffset, visibleCount, count,
                       fg, bg);

        if (c5gSelectedIdx >= 0 && c5gSelectedIdx < count &&
            hintY + UI_TEXT_H <= contentBottom) {
            char hint[56] = {};
            const C5Monster::Operation operation =
                C5Monster::getActiveOperation();
            if (operation == C5Monster::Operation::PACKET_MONITOR) {
                uint8_t channel = 0u;
                uint32_t pps = 0u;
                uint32_t ageMs = 0u;
                if (C5Monster::getPacketMonitorSample(
                        channel, pps, ageMs) &&
                    ageMs <= 2500u) {
                    snprintf(hint, sizeof(hint),
                             "LIVE CH%u %luP/S  STOP TO END",
                             (unsigned)channel, (unsigned long)pps);
                } else {
                    strlcpy(hint, "LIVE PACKET RATE STARTING", sizeof(hint));
                }
            } else if (operation == C5Monster::Operation::CHANNEL_VIEW) {
                strlcpy(hint, "CHANNEL CENSUS ACTIVE  STOP TO END",
                        sizeof(hint));
            } else if (operation ==
                       C5Monster::Operation::DEAUTH_DETECTOR) {
                strlcpy(hint, "DEAUTH WATCH ACTIVE  STOP TO END",
                        sizeof(hint));
            } else {
                snprintf(hint, sizeof(hint),
                         "CH%u  [B] INSPECT + ACTIONS",
                         (unsigned)c5gNetworks[c5gSelectedIdx].channel);
            }

            fitTextToWidth(canvas, hint, sizeof(hint), SCREEN_WIDTH - 12);
            canvas.drawFastHLine(4, hintY - 2, SCREEN_WIDTH - 8, rowRule);
            canvas.setTextColor(fg, bg);
            canvas.setCursor(6, hintY);
            canvas.print(hint);
        }
        return;
    }

    const int count = static_cast<int>(networkCount);
    if (count == 0) {
        drawEmptyState(canvas, 0, startY, SCREEN_WIDTH, listHeight,
                       "LISTENING FOR 2.4GHz NETWORKS",
                       paused ? "CAPTURE PAUSED" : "HOPPING - MOVE OR WAIT",
                       fg, bg);
        return;
    }

    int scrollOffset = 0;
    if (selectedIdx >= maxLines) {
        scrollOffset = selectedIdx - maxLines + 1;
    }
    scrollOffset = constrain(scrollOffset, 0, max(0, count - maxLines));
    const int visibleCount = min(maxLines, count - scrollOffset);

    static const char* kPhyNames[] = {
        "?", "11B", "11G", "11N", "11AC"
    };

    for (int row = 0; row < visibleCount; ++row) {
        const int idx = scrollOffset + row;
        const int y = startY + row * lineHeight;
        const bool selected = idx == selectedIdx;
        const uint32_t ageMs = SpectrumThruMath::observationAgeMs(
            cachedMillis, networks[idx].lastSeen);
        const bool stale = ageMs > NETWORK_TIMEOUT / 2u;
        const uint16_t rowFg = stale && !selected ? muted : fg;
        const uint16_t textFg = selected ? bg : rowFg;
        const uint16_t textBg = selected ? fg : bg;

        drawDottedHLine(canvas, 5, y + lineHeight - 1,
                        SCREEN_WIDTH - 11, rowRule, 2, 5);

        const uint8_t readiness =
            readinessScore ? readinessScore[idx] : 0u;
        canvas.drawRect(0, y + 1, kReadinessBarW,
                        lineHeight - 3, rowRule);
        if (readiness > 0u) {
            const int fillHeight = constrain(
                (static_cast<int>(readiness) * (lineHeight - 5)) / 100,
                1, lineHeight - 5);
            canvas.fillRect(
                1, y + lineHeight - 3 - fillHeight,
                max(1, kReadinessBarW - 2), fillHeight, rowFg);
        }

        if (selected) {
            drawSelectedRow(canvas, kReadinessBarW + 1, y,
                            SCREEN_WIDTH - kReadinessBarW - 4,
                            lineHeight, fg);
        }

        const int textY = y + (lineHeight - UI_TEXT_H) / 2;
        const uint8_t tier = vulnTier ? vulnTier[idx] : 7u;
        const char icon = getTierIcon(tier);

        canvas.setTextColor(textFg, textBg);
        if (tier <= 2u && !selected) {
            canvas.fillRect(kTierIconX - 1, y + 2, 8,
                            lineHeight - 5, rowFg);
            canvas.setTextColor(bg, rowFg);
            canvas.setCursor(kTierIconX, textY);
            canvas.print(icon);
            canvas.setTextColor(textFg, textBg);
        } else {
            canvas.setCursor(kTierIconX, textY);
            canvas.print(icon);
        }

        char ssidBuf[kSsidMax + 1];
        strncpy(ssidBuf,
                networks[idx].ssid[0]
                    ? networks[idx].ssid
                    : "<HIDDEN>",
                kSsidMax);
        ssidBuf[kSsidMax] = '\0';
        fitTextToWidth(canvas, ssidBuf, sizeof(ssidBuf),
                       kSparklineX - kSsidX - 4);
        canvas.setCursor(kSsidX, textY);
        canvas.print(ssidBuf);

        drawSparkline(canvas, kSparklineX,
                      y + (lineHeight - UI_TEXT_H) / 2,
                      static_cast<uint16_t>(idx),
                      selected ? bg : rowFg,
                      selected ? fg : bg);

        if (networks[idx].clientCount > 0u) {
            canvas.setCursor(kClientCountX, textY);
            canvas.printf("C:%u", (unsigned)networks[idx].clientCount);
        }

        const uint8_t phy = networks[idx].phyMode;
        const char* phyName = phy <= 4u ? kPhyNames[phy] : "?";
        char info[12] = {};
        int infoPos = 0;
        for (int c = 0; phyName[c] && infoPos < 4; ++c) {
            info[infoPos++] = phyName[c];
        }
        if (beaconAnomaly && beaconAnomaly[idx] && infoPos < 10) {
            info[infoPos++] = '!';
        }
        if (isRogueSSID(static_cast<uint16_t>(idx)) && infoPos < 10) {
            info[infoPos++] = 'R';
        }
        if (networkClusterId && networkClusterId[idx] && infoPos < 10) {
            info[infoPos++] = '#';
        }
        info[infoPos] = '\0';
        canvas.setCursor(kInfoX, textY);
        canvas.print(info);

        char rssi[8];
        snprintf(rssi, sizeof(rssi), "%ddB", networks[idx].rssi);
        canvas.setTextDatum(TR_DATUM);
        canvas.drawString(rssi, kRssiRightX, textY);
        canvas.setTextDatum(TL_DATUM);
    }

    drawScrollRail(canvas, SCREEN_WIDTH - 2, startY + 1,
                   listHeight - 2, scrollOffset, visibleCount, count,
                   fg, bg);

    if (selectedIdx >= 0 && selectedIdx < count &&
        hintY + UI_TEXT_H <= contentBottom) {
        char hint[64];
        const uint8_t tier = vulnTier ? vulnTier[selectedIdx] : 7u;
        const uint8_t readiness =
            readinessScore ? readinessScore[selectedIdx] : 0u;
        const char* path = getAttackPathHint(
            static_cast<uint16_t>(selectedIdx));
        snprintf(hint, sizeof(hint), "T%c %u%%  %s",
                 getTierIcon(tier), (unsigned)readiness,
                 path && path[0] ? path : "OBSERVE TARGET");
        fitTextToWidth(canvas, hint, sizeof(hint), SCREEN_WIDTH - 12);
        canvas.drawFastHLine(4, hintY - 2, SCREEN_WIDTH - 8, rowRule);
        canvas.setTextColor(fg, bg);
        canvas.setCursor(6, hintY);
        canvas.print(hint);
    }

    canvas.setTextColor(fg, bg);
    canvas.setTextDatum(TL_DATUM);
}

static void drawClientMonitor(M5Canvas& canvas, uint16_t fg, uint16_t bg) {
    const SpectrumNetwork* net = getSelectedNetwork();
    if (!net) return;

    const uint16_t idx = static_cast<uint16_t>(selectedIdx);
    const bool hidden = !isSsidKnown(*net);
    const char* displaySsid = hidden ? "<hidden>" : net->ssid;
    const uint16_t muted = uiTone(fg, bg, 0.54f);
    const uint16_t rule = uiTone(fg, bg, 0.68f);
    const int contentTop = TOP_BAR_H;
    const int contentBottom = SCREEN_HEIGHT - BOTTOM_BAR_H;

    // Build credential/QR state before layout so every column has one owner.
    char potPsk[65] = {};
    const bool potKnown =
        !hidden && lookupPotfileCred(net->ssid, potPsk, sizeof(potPsk));
    char qrPayload[180] = {};
    bool qrReady = false;
    int qrEdge = 0;
    int qrX = SCREEN_WIDTH;
    int qrY = contentTop + 4;
    if (potKnown &&
        WifiQR::buildPayload(qrPayload, sizeof(qrPayload),
                             net->ssid, potPsk,
                             static_cast<wifi_auth_mode_t>(net->authmode),
                             hidden)) {
        qrEdge = WifiQR::edgeFor(qrPayload);
        if (qrEdge > 0) {
            qrX = SCREEN_WIDTH - qrEdge - 6;
            qrReady = qrX >= 120;
        }
    }

    const int textRight = qrReady ? qrX - 4 : SCREEN_WIDTH - 3;
    const int textWidth = max(40, textRight - 6);
    const int headerY = contentTop + 1;
    const int headerH = 61;

    canvas.fillRect(0, contentTop, SCREEN_WIDTH,
                    contentBottom - contentTop, bg);
    drawBracketFrame(canvas, 2, headerY, textRight - 2,
                     headerH, rule);
    canvas.setTextSize(1);
    canvas.setTextDatum(TL_DATUM);
    canvas.setTextColor(fg, bg);

    static const char* kPhyNames[] = {"?", "11B", "11G", "11N", "11AC"};
    const char* phy = net->phyMode <= 4u ? kPhyNames[net->phyMode] : "?";
    const uint8_t tier =
        (vulnTier && idx < networkCount) ? vulnTier[idx] : 7u;
    const uint8_t readiness =
        (readinessScore && idx < networkCount) ? readinessScore[idx] : 0u;
    const bool rogue = isRogueSSID(idx);
    const uint8_t cluster =
        (networkClusterId && idx < networkCount)
            ? networkClusterId[idx] : 0u;
    const bool anomaly =
        beaconAnomaly && idx < networkCount && beaconAnomaly[idx] != 0u;
    const uint32_t ageMs =
        SpectrumThruMath::observationAgeMs(cachedMillis, net->lastSeen);

    char ssid[34];
    strlcpy(ssid, displaySsid, sizeof(ssid));
    fitTextToWidth(canvas, ssid, sizeof(ssid), textWidth - 54);
    canvas.setCursor(6, headerY + 4);
    canvas.print(ssid);

    char channel[12];
    snprintf(channel, sizeof(channel), "CH%u",
             (unsigned)net->channel);
    canvas.setTextDatum(TR_DATUM);
    canvas.drawString(channel, textRight - 4, headerY + 4);
    canvas.setTextDatum(TL_DATUM);

    char bssid[18];
    RFUtil::formatMAC(bssid, net->bssid);
    canvas.setTextColor(muted, bg);
    canvas.setCursor(6, headerY + 15);
    canvas.print(bssid);

    char freshness[18];
    char age[8];
    formatAgeShort(age, sizeof(age), ageMs);
    snprintf(freshness, sizeof(freshness), "%s  %ddB", age, net->rssi);
    canvas.setTextDatum(TR_DATUM);
    canvas.drawString(freshness, textRight - 4, headerY + 15);
    canvas.setTextDatum(TL_DATUM);

    drawDottedHLine(canvas, 6, headerY + 25,
                    textRight - 12, rule, 3, 4);

    canvas.setTextColor(fg, bg);
    canvas.setCursor(6, headerY + 29);
    canvas.printf("%s  PMF:%c  %s/%uM",
                  formatAuthShort(net->authmode),
                  net->hasPMF ? 'Y' : 'N',
                  phy, net->channelWidth ? 40u : 20u);

    canvas.setCursor(6, headerY + 40);
    canvas.printf("T%c %u%%", getTierIcon(tier),
                  (unsigned)readiness);
    const int meterX = 52;
    const int meterW = max(18, textRight - meterX - 7);
    drawLevelMeter(canvas, meterX, headerY + 41,
                   meterW, 6, readiness, fg, bg);

    char flags[56] = {};
    if (rogue) strlcat(flags, "TWIN? ", sizeof(flags));
    if (cluster) {
        char clusterText[10];
        snprintf(clusterText, sizeof(clusterText), "CL#%u ",
                 (unsigned)cluster);
        strlcat(flags, clusterText, sizeof(flags));
    }
    if (hidden) strlcat(flags, "HIDDEN ", sizeof(flags));
    if (anomaly) strlcat(flags, "BCN! ", sizeof(flags));
    if (net->wasRevealed) strlcat(flags, "REVEALED ", sizeof(flags));
    if (potKnown) strlcat(flags, "KEY ", sizeof(flags));

    char detail[72] = {};
    if (potKnown) {
        snprintf(detail, sizeof(detail), "PSK %.28s", potPsk);
    } else if (flags[0]) {
        strlcpy(detail, flags, sizeof(detail));
    } else {
        const char* hint = getAttackPathHint(idx);
        strlcpy(detail, hint && hint[0] ? hint : "OBSERVE TARGET",
                sizeof(detail));
    }
    fitTextToWidth(canvas, detail, sizeof(detail), textWidth - 4);
    canvas.setTextColor(muted, bg);
    canvas.setCursor(6, headerY + 51);
    canvas.print(detail);

    if (qrReady) {
        const int qrFrameX = qrX - 2;
        const int qrFrameY = qrY - 2;
        const int qrFrameH = min(qrEdge + 14, headerH + 26);
        drawBracketFrame(canvas, qrFrameX, qrFrameY,
                         qrEdge + 4, qrFrameH, rule);
        WifiQR::draw(canvas, qrX, qrY, fg, bg, qrPayload, 0);
        canvas.setTextColor(fg, bg);
        canvas.setTextDatum(TC_DATUM);
        canvas.drawString("WIFI QR", qrX + qrEdge / 2,
                          qrY + qrEdge + 3);
        canvas.setTextDatum(TL_DATUM);
    }

    const int listX = 2;
    const int listRight = textRight;
    const int listWidth = listRight - listX;
    const int listTitleY = headerY + headerH + 4;
    const int rowsY = listTitleY + 12;
    const int rowHeight = 12;
    const int maxClients = qrReady ? 4 : 6;
    const int selectedClient = getSelectedClientIndex();
    const int clientCount = static_cast<int>(net->clientCount);

    canvas.setTextColor(fg, bg);
    canvas.setCursor(6, listTitleY);
    canvas.printf("CLIENTS %d", clientCount);
    if (clientCount > 0 && selectedClient >= 0) {
        char position[16];
        snprintf(position, sizeof(position), "%d/%d",
                 selectedClient + 1, clientCount);
        canvas.setTextDatum(TR_DATUM);
        canvas.drawString(position, listRight - 4, listTitleY);
        canvas.setTextDatum(TL_DATUM);
    }
    drawDottedHLine(canvas, 4, listTitleY + 9,
                    max(1, listWidth - 6), rule, 4, 4);

    if (clientCount == 0) {
        drawEmptyState(canvas, listX, rowsY, listWidth,
                       min(56, contentBottom - rowsY - 3),
                       "NO CLIENTS OBSERVED",
                       "KEEP THE AP LOCKED AND LISTEN",
                       fg, bg);
    } else {
        int scrollOffset = 0;
        if (selectedClient >= maxClients) {
            scrollOffset = selectedClient - maxClients + 1;
        }
        scrollOffset =
            constrain(scrollOffset, 0, max(0, clientCount - maxClients));
        const int visibleCount =
            min(maxClients, clientCount - scrollOffset);

        for (int row = 0; row < visibleCount; ++row) {
            const int clientIndex = scrollOffset + row;
            const int y = rowsY + row * rowHeight;
            const bool selected = clientIndex == selectedClient;
            const SpectrumClient& client = net->clients[clientIndex];
            const uint32_t signalAgeMs = client.hasSignal
                ? SpectrumThruMath::observationAgeMs(
                      cachedMillis, client.lastSignalSeen)
                : NETWORK_TIMEOUT + 1u;
            const bool fresh =
                client.hasSignal && signalAgeMs <= NETWORK_TIMEOUT;
            const uint16_t rowFg = fresh || selected ? fg : muted;
            const uint16_t textFg = selected ? bg : rowFg;
            const uint16_t textBg = selected ? fg : bg;

            drawDottedHLine(canvas, 6, y + rowHeight - 1,
                            max(1, listWidth - 12), rule, 2, 5);
            if (selected) {
                drawSelectedRow(canvas, 4, y,
                                max(3, listWidth - 6),
                                rowHeight, fg);
            }

            char mac[18];
            RFUtil::formatMAC(mac, client.mac);
            canvas.setTextColor(textFg, textBg);
            canvas.setCursor(7, y + 2);
            canvas.print(mac);

            char signal[24];
            if (fresh) {
                snprintf(signal, sizeof(signal), "%ddB / %dS",
                         client.rssi, client.snr);
            } else if (client.hasSignal) {
                char signalAge[8];
                formatAgeShort(signalAge, sizeof(signalAge), signalAgeMs);
                snprintf(signal, sizeof(signal), "STALE %s", signalAge);
            } else {
                strlcpy(signal, "RX --", sizeof(signal));
            }
            canvas.setTextDatum(TR_DATUM);
            canvas.drawString(signal, listRight - 5, y + 2);
            canvas.setTextDatum(TL_DATUM);
        }

        drawScrollRail(canvas, listRight - 2, rowsY + 1,
                       maxClients * rowHeight - 2,
                       scrollOffset, visibleCount, clientCount,
                       fg, bg);

        // Preferred-network observations for the selected client.
        if (probeTable && selectedClient >= 0 &&
            selectedClient < clientCount) {
            const int pnlY = rowsY + maxClients * rowHeight + 4;
            if (pnlY + 28 < contentBottom) {
                canvas.setTextColor(muted, bg);
                canvas.setCursor(6, pnlY);
                canvas.print("PNL / PROBE HISTORY");
                drawDottedHLine(canvas, 6, pnlY + 9,
                                max(1, listWidth - 12),
                                rule, 3, 4);

                const SpectrumClient& client =
                    net->clients[selectedClient];
                int shown = 0;
                for (uint16_t p = 0u;
                     p < probeCount && shown < 2; ++p) {
                    if (memcmp(probeTable[p].clientMAC,
                               client.mac, 6u) != 0) {
                        continue;
                    }
                    char preferred[34];
                    strlcpy(preferred, probeTable[p].ssid,
                            sizeof(preferred));
                    fitTextToWidth(canvas, preferred,
                                   sizeof(preferred),
                                   listWidth - 18);
                    canvas.setTextColor(fg, bg);
                    canvas.setCursor(12, pnlY + 12 + shown * 10);
                    canvas.print(preferred);
                    ++shown;
                }
                if (shown == 0) {
                    canvas.setTextColor(muted, bg);
                    canvas.setCursor(12, pnlY + 12);
                    canvas.print("NO DIRECTED PROBES YET");
                }
            }
        }
    }

    canvas.setTextColor(fg, bg);
    canvas.setTextDatum(TL_DATUM);
    Display::drawBottomBar3To(
        &canvas,
        net->clientCount > 0u ? "[A/C]CLIENTS" : "",
        net->clientCount > 0u ? "[B]GIGER" : "[B]---",
        "[C+]BACK");
}

static void drawClientDetailPopup(M5Canvas& canvas, uint16_t fg, uint16_t bg) {
    const SpectrumNetwork* net = getSelectedNetwork();
    const SpectrumClient* client = getSelectedClient();
    if (!net || !client) return;

    // ==[ GIGER MOTION TRACKER ]== RAD fan / THRU cloud scanner
    // Inverted Phrack-style: dark bg, light fg
    int boxW = 304, boxH = 170;
    int boxX = (SCREEN_WIDTH - boxW) / 2;
    int boxY = 36;

    uint16_t scanBg = bg;  // dark
    uint16_t scanFg = fg;  // light

    // ==[ TARGET ]== MAC address
    char macStr[18];
    RFUtil::formatMAC(macStr, client->mac);

    // Get RSSI data from Geiger (if active) or client
    int8_t smoothed = client->rssi;
    bool geigerActive = (Geiger::isActive() &&
                         Geiger::getSource() == Geiger::SOURCE_CLIENT);
    
    if (geigerActive) {
        smoothed = Geiger::getSmoothed();
    }

    const uint32_t nowMs = millis();
    uint8_t historyDensity = 0u;
    uint8_t historyConsistency = 0u;
    uint8_t historyCadence = 0u;
    uint8_t historyConfidence = 0u;
    assessClientHistoryConfidence(clientBearing, nowMs,
                                 historyDensity, historyConsistency,
                                 historyCadence, historyConfidence);

    // piecewise proximity mapping
    uint16_t fusedProx = constrain((uint16_t)RFUtil::mapProximity(smoothed), 0u, 1000u);
    uint32_t ageMs = client->hasSignal
        ? SpectrumThruMath::observationAgeMs(
              nowMs, client->lastSignalSeen)
        : NETWORK_TIMEOUT + 1u;
    const uint8_t bearingConfidence = GeigerScanMath::bearingConfidence(
        clientBearing.lockConfidence, historyConfidence);
    int16_t fusedScanX = clientBearing.thruScanX;
    int16_t fusedScanY = clientBearing.thruScanY;
    const uint8_t observerMotionHeat = clientBearing.thruMotionHeat;
    const uint8_t stationaryConfidence = clientBearing.stationaryConfidence;
    const bool csiTargetActive = CsiTracker::isTargetActive();
    bool csiPresent = false;
    bool csiUsableForScan = false;
    bool csiWaiting = false;
    uint8_t csiQuality = 0u;
    uint32_t csiAgeMs = 0u;
    uint8_t csiChannelChange = 0u;
    uint8_t csiFrequencySpread = 0u;
    uint8_t csiStability = 0u;
    uint8_t csiFade = 0u;

#if defined(HAMLET_WIFI_CSI)
    CsiTracker::Snapshot csi{};
    if (csiTargetActive) {
        csiWaiting = true;
        if (CsiTracker::getSnapshot(csi) && csi.sampleCount > 0u && csi.valid) {
            csiPresent = true;
            csiWaiting = false;
            csiUsableForScan = csiUsable(csi);
            csiAgeMs = csi.ageMs;
            if (csiUsableForScan) {
                const uint8_t csiBlend = (uint8_t)constrain(
                    (int)(((int)csi.quality + (int)csi.confidence) / 2u), 10, 55);
                smoothed = client->hasSignal
                    ? (int8_t)constrain(
                        ((int)smoothed * 3 + (int)csi.rssi) / 4, -95, -20)
                    : csi.rssi;
                fusedProx = blendUInt16(fusedProx, csi.proximity, csiBlend, 1000u);
                ageMs = (csi.ageMs < ageMs) ? csi.ageMs : ageMs;
                // Single-antenna CSI refines signal depth, never left/right.
            }
            csiQuality = csi.quality;
            csiChannelChange = csi.channelChange;
            csiFrequencySpread = csi.frequencySpread;
            csiStability = csi.stability;
            csiFade = csi.fade;
        }
    }
#endif

    if (!csiPresent) {
        csiQuality = 0u;
        csiAgeMs = 0u;
        csiChannelChange = 0u;
        csiFrequencySpread = 0u;
        csiStability = 0u;
        csiFade = 0u;
    }

    // RSSI-only motion is weak evidence. It becomes useful only while the IMU
    // says the observer is quiet; otherwise observer and target are confounded.
    const int trendMagnitude = abs((int)clientBearing.rssiTrendSmooth);
    const uint8_t rssiSceneMotion = (ageMs <= 3000u)
        ? (uint8_t)constrain((trendMagnitude - 2) * 8, 0, 45)
        : 0u;
    const uint8_t sceneMotionRaw = rssiSceneMotion;
    const uint8_t sceneMotionConf = constrain(
        (int)stationaryConfidence +
            (clientBearing.isMoving ? 0 : clientBearingConfig.stationaryBoostWhileIdle),
        0, 100);
    const uint8_t sceneMotionHeat = (uint8_t)constrain(
        ((int)sceneMotionRaw * (int)sceneMotionConf) / 100, 0, 100);
    const int16_t sceneScanX = 0;
    const int16_t sceneScanY = 0;

    int16_t finalScanX = fusedScanX;
    int16_t finalScanY = fusedScanY;
    if (clientBearing.gyroCalibrated && clientScanRefValid &&
        (uint16_t)(historyConfidence + bearingConfidence) > kClientScanYawConfidenceGate) {
        const float deltaYaw = normalizeAngle180(
            clientBearing.relativeHeading - clientScanRefHeading);
        const float c = cosf(-deltaYaw * kDegToRad);
        const float s = sinf(-deltaYaw * kDegToRad);
        finalScanX = (int16_t)constrain(
            roundFloatToInt((float)fusedScanX * c - (float)fusedScanY * s),
            -100, 100);
        finalScanY = (int16_t)constrain(
            roundFloatToInt((float)fusedScanX * s + (float)fusedScanY * c),
            -100, 100);
    }
    
    // No directional fallback: AP/client RSSI difference has no left/right sign.
    const int16_t fallbackBearing = 0;  // signal strength has no left/right sign

    const bool stablePosition = clientBearing.bearingLocked &&
                                bearingConfidence > 60u &&
                                !clientBearing.porkBehind &&
                                ageMs <= 30000u;
    const bool retainedPosition = clientLastKnownValid && !stablePosition;
    const bool shouldRefreshAnchor = GeigerScanMath::anchorNeedsRefresh(
        clientLastKnownValid, clientBearing.lockGeneration,
        clientLastKnownLockGeneration, clientBearing.approachConfirmCount,
        clientLastKnownApproachConfirmCount);
    if (stablePosition && shouldRefreshAnchor) {
        clientLastKnownHeadingDegX10 = GeigerScanMath::anchorHeadingX10(
            clientBearing.lastHeadingDegX10, clientBearing.bearingRaw);
        clientLastKnownElevDegX10 = clientBearing.isFlat
            ? 0 : clientBearing.lastElevDegX10;
        clientLastKnownProximity = fusedProx;
        clientLastKnownRssi = smoothed;
        clientLastKnownObserverX = clientBearing.observerPositionX;
        clientLastKnownObserverY = clientBearing.observerPositionY;
        clientLastKnownSeenMs = clientBearing.lastDirectionalFeedTime;
        clientLastKnownApproachConfirmCount =
            clientBearing.approachConfirmCount;
        clientLastKnownLockGeneration = clientBearing.lockGeneration;
        clientLastKnownValid = true;
#if HAMLET_DEBUG_LOG
        HAMLET_LOGF(
            "[SPECTRUM] THRU anchor=%u age=%lu conf=%u csi=%u q=%u s=%u flat=%u bear=%d\n",
            (unsigned)clientBearing.lockGeneration,
            (unsigned long)(nowMs - clientLastKnownSeenMs),
            (unsigned)bearingConfidence,
            (unsigned)csiUsableForScan,
            (unsigned)csiQuality,
            (unsigned)clientBearing.thruMotionHeat,
            (unsigned)Pedometer::isCachedFlat(),
            (int)clientBearing.bearing);
#endif
    }

#if HAMLET_DEBUG_LOG
    if (nowMs - lastThruDebugMs >= kThruDebugIntervalMs ||
        stablePosition != lastThruDetailStable ||
        retainedPosition != lastThruDetailRetained) {
        HAMLET_LOGF(
            "[SPECTRUM] THRU frame lock=%u stable=%u retained=%u pts=%u csi=%u/%u q=%u stab=%u hist=%u/%u/%u/%u conf=%u age=%lu\n",
            (unsigned)clientBearing.bearingLocked,
            (unsigned)stablePosition,
            (unsigned)retainedPosition,
            (unsigned)Bearing::getRfPointCount(clientBearing),
            (unsigned)csiTargetActive,
            (unsigned)csiPresent,
            (unsigned)csiQuality,
            (unsigned)csiStability,
            (unsigned)historyDensity,
            (unsigned)historyConsistency,
            (unsigned)historyCadence,
            (unsigned)historyConfidence,
            (unsigned)bearingConfidence,
            (unsigned long)ageMs);
        lastThruDebugMs = nowMs;
        lastThruDetailStable = stablePosition;
        lastThruDetailRetained = retainedPosition;
    }
#endif

    FtmRangeEvidence ftm{};
    getFtmRangeEvidence(ftm);
    const bool ftmFresh = ftm.valid && ftm.ageMs <= 30000u;

    GeigerScanView::ThroughTarget target = {};
    target.header = macStr;
    target.scope = "WIFI";
    target.evidenceLabel = ftmFresh
        ? "RX+FTM" : (csiUsableForScan ? "RX+CSI" : "RX");
    target.evidenceGrade = ftmFresh
        ? GeigerScanView::RfEvidenceGrade::FTM_RANGE
        : (csiUsableForScan
               ? GeigerScanView::RfEvidenceGrade::CSI_STABILITY
               : GeigerScanView::RfEvidenceGrade::PACKET_STREAM);
    target.rssi = smoothed;
    target.proximity = fusedProx;
    target.bearing = clientBearing.bearing;
    target.confidence = (uint8_t)bearingConfidence;
    target.trend = clientBearing.rssiTrendSmooth;
    target.ageMs = ageMs;
    target.dotCount = (uint16_t)(net->clientCount > 0 ? net->clientCount : 1);
    target.locked = clientBearing.bearingLocked;
    target.historyDensity = historyDensity;
    target.historyConsistency = historyConsistency;
    target.historyCadence = historyCadence;
    target.historyConfidence = historyConfidence;
    target.behind = clientBearing.porkBehind;
    target.moving = clientBearing.isMoving;
    target.flat = Pedometer::isCachedFlat();
    target.motionScreenSign = (int8_t)(Config::getDisplayRotate180() ? -1 : 1);
    target.scanX = finalScanX;
    target.scanY = finalScanY;
    target.motionHeat = observerMotionHeat;
    target.stationaryConfidence = stationaryConfidence;
    target.sceneScanX = sceneScanX;
    target.sceneScanY = sceneScanY;
    target.sceneMotionHeat = sceneMotionHeat;
    target.tracker = &clientBearing;
    target.fallbackBearing = fallbackBearing;
    target.seekHeadingDegX10 = (uint16_t)GeigerScanMath::normX10(
        roundFloatToInt(clientRadarRefHeading * 10.0f));
    target.seekHeadingValid = clientRadarRefValid;
    target.lastKnownHeadingDegX10 = clientLastKnownHeadingDegX10;
    target.lastKnownElevDegX10 = clientLastKnownElevDegX10;
    target.lastKnownProximity = clientLastKnownProximity;
    target.lastKnownObserverX = clientLastKnownObserverX;
    target.lastKnownObserverY = clientLastKnownObserverY;
    target.lastKnownAgeMs = clientLastKnownValid
        ? SpectrumThruMath::observationAgeMs(nowMs, clientLastKnownSeenMs)
        : 0u;
    target.lastKnownValid = Config::getGhostMarkerEnabled() &&
                            clientLastKnownValid;
    target.radarAnchorValid = clientLastKnownValid;
    target.lastKnownRssi = clientLastKnownRssi;

    target.csiValid = csiPresent;
    target.csiWaiting = csiWaiting;
    target.csiAgeMs = csiAgeMs;
    target.csiQuality = csiQuality;
    target.csiChannelChange = csiChannelChange;
    target.csiFrequencySpread = csiFrequencySpread;
    target.csiStability = csiStability;
    target.csiFade = csiFade;
    target.ftmResponder = net->ftmResponder;
    target.ftmActive = ftm.active;
    target.ftmValid = ftmFresh;
    target.ftmDistanceCm = ftm.distanceCm;
    target.ftmVarianceCm2 = ftm.varianceCm2;
    target.ftmSampleCount = ftm.sampleCount;
    target.ftmAgeMs = ftm.ageMs;
    target.band24Count = networkCount;
    target.band5Count = c5gNetworkCount;
    target.channelCount = getChannelNetworkCount(net->channel);
    target.channel = net->channel;
    target.channelPps = displayPps;
    target.channelPpsValid =
        lockedChannel == net->channel && lastPpsUpdate != 0u &&
        nowMs - lastPpsUpdate <= 2500u;
    applyGpsRouteDiagnostics(target, nowMs);

    if (Geiger::getViewMode() == Geiger::VIEW_THRU) {
        GeigerScanView::drawThroughScanner(canvas, boxX, boxY, boxW, boxH,
                                           scanFg, scanBg, target);
        drawClientScannerBottomBar(canvas);
        return;
    }

    GeigerScanView::drawRadarScanner(canvas, boxX, boxY, boxW, boxH,
                                     scanFg, scanBg, target);

    drawClientScannerBottomBar(canvas);
}

static void drawC5CarrierDetailPopup(M5Canvas& canvas,
                                     uint16_t fg, uint16_t bg) {
    if (!c5CarrierDetailActive || !c5CarrierTarget.valid) return;

    const int boxW = 304;
    const int boxH = 170;
    const int boxX = (SCREEN_WIDTH - boxW) / 2;
    const int boxY = 36;
    const uint32_t nowMs = millis();
    const uint32_t ageMs = c5CarrierTarget.lastSeenMs != 0u
        ? SpectrumThruMath::observationAgeMs(
              nowMs, c5CarrierTarget.lastSeenMs)
        : C5G_OBSERVATION_STALE_MS + 1u;

    // The opened AP owns an explicit recon header. Keep the scanner evidence
    // visual, but expose the identity/security/location facts JanOS actually
    // supplied instead of making the operator infer them from the list below.
    char displaySsid[17];
    strncpy(displaySsid,
            c5CarrierTarget.ssid[0] ? c5CarrierTarget.ssid : "<HIDDEN>",
            sizeof(displaySsid) - 1u);
    displaySsid[sizeof(displaySsid) - 1u] = '\0';
    char primaryMeta[24];
    snprintf(primaryMeta, sizeof(primaryMeta), "CH%u %s %ddB",
             (unsigned)c5CarrierTarget.channel,
             C5Protocol::authTypeLabel(c5CarrierTarget.authType),
             c5CarrierTarget.rssi);

    bool has24GHzSibling = false;
    if (c5CarrierTarget.ssid[0] && networks) {
        for (uint16_t i = 0; i < networkCount; ++i) {
            if (strncmp(networks[i].ssid, c5CarrierTarget.ssid,
                        sizeof(c5CarrierTarget.ssid)) == 0) {
                has24GHzSibling = true;
                break;
            }
        }
    }
    const uint8_t forensicIndicators =
        DefensePipeline::snapshot().countIndicatorsForBSSID(c5CarrierTarget.bssid, 300000u);
    bool hasReconLocation = false;
    float reconLatitude = 0.0f;
    float reconLongitude = 0.0f;
    const Recon::WifiAP* reconSnapshot = DefensePipeline::snapshot().getWifiSnapshot();
    const int reconSnapshotCount = DefensePipeline::snapshot().getWifiSnapshotCount();
    for (int i = 0; reconSnapshot && i < reconSnapshotCount; ++i) {
        if (memcmp(reconSnapshot[i].bssid, c5CarrierTarget.bssid, 6) != 0) {
            continue;
        }
        reconLatitude = reconSnapshot[i].lat;
        reconLongitude = reconSnapshot[i].lon;
        hasReconLocation =
            reconLatitude != 0.0f || reconLongitude != 0.0f;
        break;
    }
    char locationMeta[32];
    if (hasReconLocation) {
        snprintf(locationMeta, sizeof(locationMeta), "GPS %.4f,%.4f",
                 reconLatitude, reconLongitude);
    } else {
        snprintf(locationMeta, sizeof(locationMeta), "GPS --");
    }
    char reconMeta[24];
    snprintf(reconMeta, sizeof(reconMeta), "FOR:%u 2.4:%c AGE:%lus",
             (unsigned)forensicIndicators,
             has24GHzSibling ? 'Y' : 'N',
             (unsigned long)(ageMs / 1000u));

    canvas.fillRect(0, TOP_BAR_H, SCREEN_WIDTH, boxY - TOP_BAR_H, bg);
    canvas.setTextSize(1);
    canvas.setTextColor(fg, bg);
    canvas.setTextDatum(TL_DATUM);
    canvas.drawString(displaySsid, 4, TOP_BAR_H + 1);
    canvas.drawString(locationMeta, 4, TOP_BAR_H + 9);
    canvas.setTextDatum(TR_DATUM);
    canvas.drawString(primaryMeta, SCREEN_WIDTH - 4, TOP_BAR_H + 1);
    canvas.drawString(reconMeta, SCREEN_WIDTH - 4, TOP_BAR_H + 9);
    canvas.setTextDatum(TL_DATUM);

    char bssid[18];
    RFUtil::formatMAC(bssid, c5CarrierTarget.bssid);

    uint8_t historyDensity = 0u;
    uint8_t historyConsistency = 0u;
    uint8_t historyCadence = 0u;
    uint8_t historyConfidence = 0u;
    assessClientHistoryConfidence(
        c5CarrierBearing, nowMs, historyDensity, historyConsistency,
        historyCadence, historyConfidence, 90000u, 6u);

    const uint16_t proximity = constrain(
        (uint16_t)RFUtil::mapProximity(c5CarrierTarget.rssiSmooth),
        0u, 1000u);
    const uint8_t bearingConfidence =
        SpectrumThruMath::capScanBearingConfidence(
            GeigerScanMath::bearingConfidence(
                c5CarrierBearing.lockConfidence, historyConfidence),
            kC5ScanBearingConfidenceCap);

    int16_t finalScanX = c5CarrierBearing.thruScanX;
    int16_t finalScanY = c5CarrierBearing.thruScanY;
    if (c5CarrierBearing.gyroCalibrated && c5CarrierScanRefValid &&
        (uint16_t)(historyConfidence + bearingConfidence) >
            kClientScanYawConfidenceGate) {
        const float deltaYaw = normalizeAngle180(
            c5CarrierBearing.relativeHeading - c5CarrierScanRefHeading);
        const float c = cosf(-deltaYaw * kDegToRad);
        const float s = sinf(-deltaYaw * kDegToRad);
        finalScanX = (int16_t)constrain(
            roundFloatToInt(
                (float)c5CarrierBearing.thruScanX * c -
                (float)c5CarrierBearing.thruScanY * s),
            -100, 100);
        finalScanY = (int16_t)constrain(
            roundFloatToInt(
                (float)c5CarrierBearing.thruScanX * s +
                (float)c5CarrierBearing.thruScanY * c),
            -100, 100);
    }

    const int trendMagnitude = abs((int)c5CarrierBearing.rssiTrendSmooth);
    const uint8_t rssiSceneMotion = ageMs <= 12000u
        ? (uint8_t)constrain((trendMagnitude - 2) * 8, 0, 35)
        : 0u;
    const uint8_t sceneMotionConfidence = (uint8_t)constrain(
        (int)c5CarrierBearing.stationaryConfidence +
            (c5CarrierBearing.isMoving
                ? 0
                : c5CarrierBearingConfig.stationaryBoostWhileIdle),
        0, 100);
    const uint8_t sceneMotionHeat = (uint8_t)constrain(
        ((int)rssiSceneMotion * (int)sceneMotionConfidence) / 100,
        0, 100);

    const bool stablePosition =
        c5CarrierBearing.bearingLocked && bearingConfidence > 60u &&
        !c5CarrierBearing.porkBehind &&
        ageMs <= C5G_OBSERVATION_STALE_MS;
    const bool shouldRefreshAnchor = GeigerScanMath::anchorNeedsRefresh(
        c5CarrierLastKnownValid, c5CarrierBearing.lockGeneration,
        c5CarrierLastKnownLockGeneration,
        c5CarrierBearing.approachConfirmCount,
        c5CarrierLastKnownApproachConfirmCount);
    if (stablePosition && shouldRefreshAnchor) {
        c5CarrierLastKnownHeadingDegX10 =
            GeigerScanMath::anchorHeadingX10(
                c5CarrierBearing.lastHeadingDegX10,
                c5CarrierBearing.bearingRaw);
        c5CarrierLastKnownElevDegX10 = c5CarrierBearing.isFlat
            ? 0
            : c5CarrierBearing.lastElevDegX10;
        c5CarrierLastKnownProximity = proximity;
        c5CarrierLastKnownRssi = c5CarrierTarget.rssiSmooth;
        c5CarrierLastKnownObserverX = c5CarrierBearing.observerPositionX;
        c5CarrierLastKnownObserverY = c5CarrierBearing.observerPositionY;
        c5CarrierLastKnownSeenMs =
            c5CarrierBearing.lastDirectionalFeedTime;
        c5CarrierLastKnownApproachConfirmCount =
            c5CarrierBearing.approachConfirmCount;
        c5CarrierLastKnownLockGeneration = c5CarrierBearing.lockGeneration;
        c5CarrierLastKnownValid = true;
    }

    C5Monster::PacketMonitorTelemetry packetTelemetry = {};
    const bool packetTelemetryMatches =
        C5Monster::getActiveOperation() ==
            C5Monster::Operation::PACKET_MONITOR &&
        C5Monster::getPacketMonitorTelemetry(packetTelemetry) &&
        packetTelemetry.channel == c5CarrierTarget.channel;
    const uint32_t channelPps = packetTelemetryMatches
        ? packetTelemetry.packetsPerSecond
        : 0u;
    const bool channelPpsValid =
        packetTelemetryMatches && packetTelemetry.historyCount > 0u &&
        packetTelemetry.ageMs <= 2500u;

    C5Monster::TargetObservationTelemetry targeted{};
    const bool targetedMatches =
        C5Monster::getTargetObservation(targeted) &&
        targeted.ageMs <= 5000u &&
        memcmp(targeted.observation.bssid,
               c5CarrierTarget.bssid, 6u) == 0;
    const bool targetedCsi = targetedMatches &&
        targeted.observation.evidence ==
            C5Protocol::TargetEvidence::CSI;
    const uint8_t targetedQuality = targetedMatches
        ? (uint8_t)constrain(
              55 + min((int)targeted.observation.sampleCount, 25) -
                  min((int)targeted.observation.varianceDbm2 * 3, 45),
              10, 90)
        : 0u;

    GeigerScanView::ThroughTarget target = {};
    target.header = bssid;
    target.scope = "5G/AP";
    target.evidenceLabel = targetedMatches
        ? (targetedCsi ? "CSI" : "PACKET")
        : "SNAP";
    target.evidenceGrade = targetedMatches
        ? (targetedCsi
               ? GeigerScanView::RfEvidenceGrade::CSI_STABILITY
               : GeigerScanView::RfEvidenceGrade::PACKET_STREAM)
        : GeigerScanView::RfEvidenceGrade::SCAN_SNAPSHOT;
    target.rssi = c5CarrierTarget.rssiSmooth;
    target.proximity = proximity;
    target.bearing = c5CarrierBearing.bearing;
    target.confidence = bearingConfidence;
    target.trend = c5CarrierBearing.rssiTrendSmooth;
    target.ageMs = ageMs;
    target.dotCount = get5GHzChannelNetworkCount(c5CarrierTarget.channel);
    target.historyDensity = historyDensity;
    target.historyConsistency = historyConsistency;
    target.historyCadence = historyCadence;
    target.historyConfidence = historyConfidence;
    target.locked = c5CarrierBearing.bearingLocked;
    target.behind = c5CarrierBearing.porkBehind;
    target.moving = c5CarrierBearing.isMoving;
    target.flat = Pedometer::isCachedFlat();
    target.motionScreenSign =
        (int8_t)(Config::getDisplayRotate180() ? -1 : 1);
    target.scanX = finalScanX;
    target.scanY = finalScanY;
    target.motionHeat = c5CarrierBearing.thruMotionHeat;
    target.stationaryConfidence = c5CarrierBearing.stationaryConfidence;
    target.sceneScanX = 0;
    target.sceneScanY = 0;
    target.sceneMotionHeat = sceneMotionHeat;
    target.tracker = &c5CarrierBearing;
    target.fallbackBearing = 0;
    target.seekHeadingDegX10 = (uint16_t)GeigerScanMath::normX10(
        roundFloatToInt(c5CarrierRadarRefHeading * 10.0f));
    target.seekHeadingValid = c5CarrierRadarRefValid;
    target.lastKnownHeadingDegX10 = c5CarrierLastKnownHeadingDegX10;
    target.lastKnownElevDegX10 = c5CarrierLastKnownElevDegX10;
    target.lastKnownProximity = c5CarrierLastKnownProximity;
    target.lastKnownObserverX = c5CarrierLastKnownObserverX;
    target.lastKnownObserverY = c5CarrierLastKnownObserverY;
    target.lastKnownAgeMs = c5CarrierLastKnownValid
        ? SpectrumThruMath::observationAgeMs(nowMs,
                                              c5CarrierLastKnownSeenMs)
        : 0u;
    target.lastKnownValid =
        Config::getGhostMarkerEnabled() && c5CarrierLastKnownValid;
    target.radarAnchorValid = c5CarrierLastKnownValid;
    target.lastKnownRssi = c5CarrierLastKnownRssi;
    target.band24Count = networkCount;
    target.band5Count = c5gNetworkCount;
    target.channelCount =
        get5GHzChannelNetworkCount(c5CarrierTarget.channel);
    target.channel = c5CarrierTarget.channel;
    target.channelPps = channelPps;
    target.channelPpsValid = channelPpsValid;
    target.channelPpsHistory = packetTelemetryMatches
        ? packetTelemetry.history
        : nullptr;
    target.channelPpsHistoryCount = packetTelemetryMatches
        ? packetTelemetry.historyCount
        : 0u;
    target.csiValid = targetedCsi;
    target.csiAgeMs = targetedCsi ? targeted.ageMs : 0u;
    target.csiQuality = targetedQuality;
    target.csiStability = targetedCsi
        ? (uint8_t)constrain(
              100 - (int)targeted.observation.varianceDbm2 * 5,
              0, 100)
        : 0u;
    target.csiUnsupported = !targetedCsi;
    applyGpsRouteDiagnostics(target, nowMs);

    if (Geiger::getViewMode() == Geiger::VIEW_THRU) {
        GeigerScanView::drawThroughScanner(
            canvas, boxX, boxY, boxW, boxH, fg, bg, target);
    } else {
        GeigerScanView::drawRadarScanner(
            canvas, boxX, boxY, boxW, boxH, fg, bg, target);
    }

    const bool actionAvailable = isSelectedC5ArsenalCommandAvailable();
    const char* actionState = actionAvailable
        ? "READY"
        : ((!hasC5TargetSelection() &&
            isSelectedC5ArsenalCommandTargeted())
               ? "STALE"
               : ((C5Monster::isBusy() ||
                   C5Monster::hasActiveOperation())
                      ? "BUSY"
                      : "LOCKED"));
    char actionLine[48];
    snprintf(actionLine, sizeof(actionLine), "< %u/%u %s  %s >",
             (unsigned)(c5ArsenalSelectedIdx + 1u),
             (unsigned)C5_ARSENAL_COMMAND_COUNT,
             getSelectedC5ArsenalCommandLabel(), actionState);
    canvas.fillRect(0, boxY + boxH, SCREEN_WIDTH,
                    SCREEN_HEIGHT - BOTTOM_BAR_H - (boxY + boxH), bg);
    canvas.drawFastHLine(0, boxY + boxH, SCREEN_WIDTH, fg);
    canvas.setTextColor(fg, bg);
    canvas.setTextDatum(TC_DATUM);
    canvas.drawString(actionLine, SCREEN_WIDTH / 2, boxY + boxH + 2);
    canvas.setTextDatum(TL_DATUM);
    drawC5CarrierScannerBottomBar(canvas);
}

// ==[ PARANOID SWINE TOAST DRAWING ]==

// top bar toast: always visible during attack, simple blink
static void drawParanoidTopBar(M5Canvas& canvas, uint16_t fg,
                               uint16_t bg) {
    if (!paranoid.attackActive) return;

    const int barHeight = 12;
    const int tagWidth = 50;
    const uint16_t rule = uiTone(fg, bg, 0.44f);

    canvas.fillRect(0, 0, SCREEN_WIDTH, barHeight, bg);
    canvas.drawFastHLine(0, barHeight - 1, SCREEN_WIDTH, rule);
    canvas.fillRect(0, 0, tagWidth, barHeight - 1, fg);

    canvas.setTextSize(1);
    canvas.setTextColor(bg, fg);
    canvas.setCursor(5, 2);
    canvas.print("DEAUTH");

    char signal[28];
    snprintf(signal, sizeof(signal), "CH%u  %ddB",
             (unsigned)paranoid.attackChannel,
             (int)paranoid.rssiCurrent);
    canvas.setTextColor(fg, bg);
    canvas.setCursor(tagWidth + 7, 2);
    canvas.print(signal);

    char frames[20];
    snprintf(frames, sizeof(frames), "F:%u %c",
             (unsigned)paranoid.frameCount,
             paranoid.toastInverted ? '*' : '+');
    canvas.setTextDatum(TR_DATUM);
    canvas.drawString(frames, SCREEN_WIDTH - 5, 2);
    canvas.setTextDatum(TL_DATUM);
}

// detail toast: centered, shake-toggled
static void drawParanoidDetailToast(M5Canvas& canvas, uint16_t fg,
                                   uint16_t bg) {
    if (detectShakeEdge()) {
        detailToastVisible = !detailToastVisible;
    }
    if (!isDetailToastVisible()) return;

    const int width = 188;
    const int height = 76;
    const int x = (SCREEN_WIDTH - width) / 2;
    const int y = (SCREEN_HEIGHT - height) / 2;
    const int headerHeight = 14;
    const uint16_t rule = uiTone(fg, bg, 0.42f);
    const uint16_t muted = uiTone(fg, bg, 0.54f);

    canvas.fillRect(x, y, width, height, bg);
    drawBracketFrame(canvas, x, y, width, height, fg);
    canvas.fillRect(x + 2, y + 2, width - 4, headerHeight - 2, fg);

    canvas.setTextSize(1);
    canvas.setTextColor(bg, fg);
    canvas.setCursor(x + 8, y + 4);
    canvas.print(paranoid.attackActive ? "DEAUTH / ACTIVE"
                                       : "DEAUTH / LAST SEEN");

    char channel[14];
    snprintf(channel, sizeof(channel), "CH%u",
             (unsigned)paranoid.attackChannel);
    canvas.setTextDatum(TR_DATUM);
    canvas.drawString(channel, x + width - 8, y + 4);
    canvas.setTextDatum(TL_DATUM);

    const char* trend = "-- STABLE";
    if (paranoid.rssiTrend > 2) {
        trend = ">> WARMER";
    } else if (paranoid.rssiTrend < -2) {
        trend = "<< COLDER";
    }

    canvas.setTextColor(fg, bg);
    canvas.setCursor(x + 10, y + 21);
    canvas.printf("NOW %ddB", (int)paranoid.rssiCurrent);
    canvas.setTextDatum(TR_DATUM);
    canvas.drawString(trend, x + width - 10, y + 21);
    canvas.setTextDatum(TL_DATUM);

    const uint8_t signalLevel = static_cast<uint8_t>(constrain(
        map(constrain(paranoid.rssiCurrent, -90, -30),
            -90, -30, 0, 100),
        0, 100));
    drawLevelMeter(canvas, x + 10, y + 35,
                   width - 20, 10, signalLevel, fg, bg);
    for (int tick = 1; tick < 4; ++tick) {
        const int tickX = x + 10 + ((width - 20) * tick) / 4;
        canvas.drawPixel(tickX, y + 46, rule);
    }

    canvas.setTextColor(muted, bg);
    canvas.setCursor(x + 10, y + 52);
    canvas.printf("PEAK %ddB", (int)paranoid.rssiPeak);

    const uint32_t ageMs =
        SpectrumThruMath::observationAgeMs(cachedMillis,
                                           paranoid.lastDeauthTime);
    char age[8];
    formatAgeShort(age, sizeof(age), ageMs);
    char footer[30];
    snprintf(footer, sizeof(footer), "FRAMES %u  AGE %s",
             (unsigned)paranoid.frameCount, age);
    canvas.setTextDatum(TR_DATUM);
    canvas.drawString(footer, x + width - 10, y + 52);
    canvas.setTextDatum(TL_DATUM);

    drawDottedHLine(canvas, x + 10, y + height - 10,
                    width - 20, rule, 3, 4);
    canvas.setTextColor(fg, bg);
    canvas.setTextDatum(MC_DATUM);
    canvas.drawString("SHAKE TO HIDE",
                      x + width / 2, y + height - 7);
    canvas.setTextDatum(TL_DATUM);
}

// combined toast drawing entry point
static void drawParanoidToasts(M5Canvas& canvas, uint16_t fg, uint16_t bg) {
    // Top bar always visible during attack
    drawParanoidTopBar(canvas, fg, bg);
    
    // Detail toast only when shake-toggled (or lingering after attack)
    drawParanoidDetailToast(canvas, fg, bg);
}

// The graph caption lane has exactly one writer. Source/state lives on the
// left; the selected/current channel lives on the right.
static void drawGraphHeader(M5Canvas& canvas, uint16_t fg,
                            uint16_t bg) {
    char state[28] = {};
    char right[32] = {};
    const bool is5g = isShowing5GHz();
    const char* band = is5g ? "5G" : "2.4G";

    if (is5g) {
        switch (C5Monster::getActiveOperation()) {
            case C5Monster::Operation::PACKET_MONITOR:
                strlcpy(state, "LIVE PACKET RATE", sizeof(state));
                break;
            case C5Monster::Operation::CHANNEL_VIEW:
                strlcpy(state, "CHANNEL CENSUS", sizeof(state));
                break;
            case C5Monster::Operation::DEAUTH_DETECTOR:
                strlcpy(state, "DEAUTH WATCH", sizeof(state));
                break;
            default:
                strlcpy(state, "SNAPSHOT MODEL", sizeof(state));
                break;
        }

        if (c5gSelectedIdx >= 0 &&
            c5gSelectedIdx < c5gNetworkCount) {
            const uint8_t channel =
                c5gNetworks[c5gSelectedIdx].channel;
            const uint16_t frequency =
                channelToDisplayFreq(channel, true);
            uint32_t pps = 0u;
            if (getFreshC5PacketRate(channel, pps)) {
                snprintf(right, sizeof(right), "CH%u %uM %luP",
                         (unsigned)channel, (unsigned)frequency,
                         (unsigned long)pps);
            } else {
                snprintf(right, sizeof(right), "CH%u %uM N%u",
                         (unsigned)channel, (unsigned)frequency,
                         (unsigned)get5GHzChannelNetworkCount(channel));
            }
        } else {
            strlcpy(right, "NO AP", sizeof(right));
        }
    } else {
        if (!measuredSweepValid) {
            strlcpy(state, modelOverlayEnabled
                               ? "ACQUIRING + MODEL"
                               : "ACQUIRING",
                    sizeof(state));
        } else {
            strlcpy(state, modelOverlayEnabled
                               ? "MEASURED L/A/PK + M"
                               : "MEASURED L/A/PK",
                    sizeof(state));
        }

        const uint8_t channel = lockedChannel > 0u
            ? lockedChannel
            : (dialMode
                   ? dialChannel
                   : currentChannel.load(std::memory_order_relaxed));
        const uint16_t frequency =
            channelToDisplayFreq(channel, false);
        snprintf(right, sizeof(right), "%uM %luP",
                 (unsigned)frequency, (unsigned long)displayPps);
    }

    canvas.fillRect(SPECTRUM_LEFT, SPECTRUM_TOP,
                    SPECTRUM_WIDTH, SPECTRUM_HEADER_H, bg);
    const uint16_t divider = uiTone(fg, bg, 0.46f);
    canvas.drawFastHLine(
        SPECTRUM_LEFT,
        SPECTRUM_TOP + SPECTRUM_HEADER_H - 1,
        SPECTRUM_WIDTH, divider);
    canvas.setTextSize(1);

    const int tagX = SPECTRUM_LEFT + 1;
    const int tagY = SPECTRUM_TOP + 1;
    const int tagWidth = is5g ? 18 : 28;
    const int tagHeight = SPECTRUM_HEADER_H - 2;
    canvas.fillRect(tagX, tagY, tagWidth, tagHeight, fg);
    canvas.setTextColor(bg, fg);
    canvas.setTextDatum(MC_DATUM);
    canvas.drawString(band, tagX + tagWidth / 2,
                      tagY + tagHeight / 2);

    const int rightWidth = canvas.textWidth(right);
    const int stateX = tagX + tagWidth + 4;
    const int stateMaxWidth =
        SPECTRUM_RIGHT - 2 - rightWidth - 5 - stateX;
    fitTextToWidth(canvas, state, sizeof(state), stateMaxWidth);

    canvas.setTextColor(fg, bg);
    canvas.setTextDatum(TL_DATUM);
    canvas.drawString(state, stateX, SPECTRUM_TOP + 1);
    canvas.setTextDatum(TR_DATUM);
    canvas.drawString(right, SPECTRUM_RIGHT - 2, SPECTRUM_TOP + 1);
    canvas.setTextDatum(TL_DATUM);
}

static void drawSpectrumFooter(M5Canvas& canvas, uint16_t fg,
                               uint16_t bg) {
    if (Display::drawHintBottomBar(&canvas)) return;

    const int y = SCREEN_HEIGHT - BOTTOM_BAR_H;
    const int textY = y + 3;
    const uint16_t rule = uiTone(fg, bg, 0.44f);
    canvas.fillRect(0, y, SCREEN_WIDTH, BOTTOM_BAR_H, bg);
    canvas.drawFastHLine(0, y, SCREEN_WIDTH, rule);
    canvas.setTextSize(1);

    const char* band = isShowing5GHz() ? "5G" : "2.4G";
    const int tagWidth = isShowing5GHz() ? 18 : 28;
    canvas.fillRect(3, y + 2, tagWidth, BOTTOM_BAR_H - 4, fg);
    canvas.setTextColor(bg, fg);
    canvas.setTextDatum(MC_DATUM);
    canvas.drawString(band, 3 + tagWidth / 2,
                      y + BOTTOM_BAR_H / 2);

    char mode[20] = {};
    if (paused.load(std::memory_order_acquire)) {
        strlcpy(mode, "PAUSED", sizeof(mode));
    } else if (lockedChannel > 0u) {
        snprintf(mode, sizeof(mode), "LOCK %02u",
                 (unsigned)lockedChannel);
    } else if (dialMode) {
        snprintf(mode, sizeof(mode), "DIAL%s %02u",
                 dialLocked ? "*" : "",
                 (unsigned)dialChannel);
    } else {
        strlcpy(mode, "HOP", sizeof(mode));
    }

    const uint16_t bandCount =
        isShowing5GHz() ? c5gNetworkCount : networkCount;
    const int16_t bandIndex =
        isShowing5GHz() ? c5gSelectedIdx : selectedIdx;
    char left[36] = {};
    if (bandCount > 0u && bandIndex >= 0) {
        snprintf(left, sizeof(left), "%s  %d/%u",
                 mode, (int)(bandIndex + 1),
                 (unsigned)bandCount);
    } else {
        strlcpy(left, mode, sizeof(left));
    }

    char right[36] = {};
    if (isShowing5GHz()) {
        char age[8];
        formatAgeShort(
            age, sizeof(age),
            c5gLastScanMs != 0u
                ? SpectrumThruMath::observationAgeMs(
                      cachedMillis, c5gLastScanMs)
                : 0u);
        if (c5gLastScanRevision != 0u) {
            snprintf(right, sizeof(right), "C5 R%lu %s",
                     (unsigned long)c5gLastScanRevision, age);
        } else {
            strlcpy(right, "C5 WAIT", sizeof(right));
        }
    } else {
        char age[8] = {};
        if (lastNewNetworkTime > 0u) {
            formatAgeShort(
                age, sizeof(age),
                SpectrumThruMath::observationAgeMs(
                    cachedMillis, lastNewNetworkTime));
        }
        if (probeCount > 0u && age[0]) {
            snprintf(right, sizeof(right), "PRB %u  NEW %s",
                     (unsigned)probeCount, age);
        } else if (probeCount > 0u) {
            snprintf(right, sizeof(right), "PRB %u",
                     (unsigned)probeCount);
        } else if (age[0]) {
            snprintf(right, sizeof(right), "NEW %s", age);
        }
    }

    canvas.setTextColor(fg, bg);
    canvas.setTextDatum(TL_DATUM);
    const int leftX = 3 + tagWidth + 5;
    const int rightWidth = canvas.textWidth(right);
    fitTextToWidth(canvas, left, sizeof(left),
                   SCREEN_WIDTH - leftX - rightWidth - 10);
    canvas.drawString(left, leftX, textY);

    if (right[0]) {
        canvas.setTextDatum(TR_DATUM);
        canvas.drawString(right, SCREEN_WIDTH - 4, textY);
        canvas.setTextDatum(TL_DATUM);
    }
}

void draw(M5Canvas& canvas) {
    const uint16_t fg = Display::getColorFG();
    const uint16_t bg = Display::getColorBG();
    cacheFrameConstants();

    if (clientMode) {
        drawClientMonitor(canvas, fg, bg);
        if (clientDetailActive) {
            drawClientDetailPopup(canvas, fg, bg);
        }
        return;
    }

    drawSpectrumCarriers(canvas, fg, bg);
    if (isShowing5GHz()) {
        drawC5ObserverStrip(canvas, fg, bg);
    } else {
        // Waterfall rows are completed-sweep evidence not carried into the C5
        // viewport. Noise-floor markers are drawn inside drawSpectrumCarriers so
        // they stay within the progressive-reveal slice.
        drawWaterfall(canvas, fg, bg);
    }
    drawGraphHeader(canvas, fg, bg);
    drawNetworkList(canvas, fg, bg);
    drawSpectrumFooter(canvas, fg, bg);

    drawRogueAlertToast(canvas, fg, bg);
    drawParanoidToasts(canvas, fg, bg);

    if (c5CarrierDetailActive) {
        drawC5CarrierDetailPopup(canvas, fg, bg);
    }
}

// ==[ DUAL-BAND: 5GHz via C5Monster ]==

static bool band5gEnabled = false;
static bool showing5GHz = false;
static constexpr uint8_t CH5G_ARRAY_SIZE = 33;  // indices 0-32 for channels 36-165
static int8_t ch5gPeakRSSI[CH5G_ARRAY_SIZE] = {};
static uint16_t ch5gNetCount[CH5G_ARRAY_SIZE] = {};

bool is5GHzEnabled() {
    return band5gEnabled && Config::getC5Enabled() && C5Monster::isConnected();
}
void set5GHzEnabled(bool enabled) {
    if (!enabled && band5gEnabled) {
        const bool wasShowing5GHz = showing5GHz;
        c5CarrierDetailActive = false;
        showing5GHz = false;
        viewCenterMHz = 2442.0f;
        viewWidthMHz = 35.0f;
        resetC5CarrierBearingState(nullptr);
        if (wasShowing5GHz) clearWaterfallHistory();
    }
    band5gEnabled = enabled;
}
bool isShowing5GHz() { return showing5GHz && is5GHzEnabled(); }

void toggleBand() {
    if (!is5GHzEnabled()) return;
    if (showing5GHz) {
        c5CarrierDetailActive = false;
        resetC5CarrierBearingState(nullptr);
        const C5Monster::Operation operation =
            C5Monster::getActiveOperation();
        if (operation == C5Monster::Operation::PACKET_MONITOR ||
            operation == C5Monster::Operation::CHANNEL_VIEW ||
            operation == C5Monster::Operation::DEAUTH_DETECTOR) {
            C5Monster::emergencyStop();
        }
    }
    showing5GHz = !showing5GHz;
    // Switch view parameters for the selected band
    if (showing5GHz) {
        viewCenterMHz = 5500.0f;   // center of UNII bands
        viewWidthMHz = VIEW_WIDTH_5GHZ_MAX;  // 200 MHz max
        if (c5gSelectedIdx >= 0 && c5gSelectedIdx < c5gNetworkCount) {
            focus5GHzNetwork(c5gNetworks[c5gSelectedIdx].channel);
        }
    } else {
        viewCenterMHz = 2442.0f;   // between ch6-7
        viewWidthMHz = 35.0f;
        clearWaterfallHistory();
    }
    SFX::click();
}

int8_t get5GHzChannelPeakRSSI(uint8_t channel) {
    if (channel < 36 || channel > 165) return -128;
    uint8_t idx = (channel - 36) / 4;
    if (idx >= CH5G_ARRAY_SIZE) return -128;
    return ch5gPeakRSSI[idx];
}

uint16_t get5GHzChannelNetworkCount(uint8_t channel) {
    if (channel < 36 || channel > 165) return 0;
    uint8_t idx = (channel - 36) / 4;
    if (idx >= CH5G_ARRAY_SIZE) return 0;
    uint16_t count = ch5gNetCount[idx];
    if (C5Monster::isChannelSurveyFresh()) {
        const uint16_t observed =
            C5Monster::getObservedChannelNetworkCount(channel);
        if (observed > count) count = observed;
    }
    return count;
}

void feedC5MonsterScan(const C5Monster::ScanResults& results) {
    const uint32_t nowMs = millis();
    const uint32_t completedAtMs =
        results.timestampMs != 0u ? results.timestampMs : nowMs;

    // Preserve only the identity/index selection across the atomic completed
    // snapshot swap. The previous carriers remain visible while JanOS scans;
    // this function is called only after the new revision is published.
    const int16_t previousSelectedIdx = c5gSelectedIdx;
    uint8_t previousSelectedBssid[6] = {};
    const bool hadPreviousSelection =
        c5gSelectedIdx >= 0 && c5gSelectedIdx < (int8_t)c5gNetworkCount;
    if (hadPreviousSelection) {
        memcpy(previousSelectedBssid, c5gNetworks[c5gSelectedIdx].bssid,
               sizeof(previousSelectedBssid));
    }

    c5gLastScanRevision = results.revision;
    c5gLastScanMs = completedAtMs;
    c5SnapshotStripDirty = true;

    // Replace the visible carrier/list snapshot immediately on completion.
    memset(c5gNetworks, 0, sizeof(c5gNetworks));
    c5gNetworkCount = 0;
    memset(ch5gPeakRSSI, -128, sizeof(ch5gPeakRSSI));
    memset(ch5gNetCount, 0, sizeof(ch5gNetCount));

    for (uint8_t i = 0; i < results.count; i++) {
        const auto& entry = results.entries[i];
        if (!entry.is5GHz || entry.channel < 36 || entry.channel > 165) continue;

        int8_t idx = findC5GHzNetworkByBssid(entry.bssid);
        if (idx < 0) {
            idx = (int8_t)c5gNetworkCount;
            if (idx >= MAX_C5G_NETWORKS) continue;
            memset(&c5gNetworks[idx], 0, sizeof(c5gNetworks[idx]));
            c5gNetworkCount++;
        }

        // Network list entry
        C5GHzNetwork& net = c5gNetworks[idx];
        net.sourceIndex = entry.sourceIndex;
        net.sourceRevision = results.revision;
        strncpy(net.ssid, entry.ssid, sizeof(net.ssid) - 1);
        net.ssid[sizeof(net.ssid) - 1] = '\0';
        memcpy(net.bssid, entry.bssid, 6);
        net.channel = entry.channel;
        net.rssi = entry.rssi;
        net.authType = entry.authType;
        net.isHidden = entry.isHidden;
        net.lastSeenMs = completedAtMs;
        if (c5CarrierDetailActive && c5CarrierTarget.valid &&
            memcmp(entry.bssid, c5CarrierTarget.bssid,
                   sizeof(c5CarrierTarget.bssid)) == 0) {
            strncpy(c5CarrierTarget.ssid, entry.ssid,
                    sizeof(c5CarrierTarget.ssid) - 1u);
            c5CarrierTarget.ssid[sizeof(c5CarrierTarget.ssid) - 1u] = '\0';
            c5CarrierTarget.channel = entry.channel;
            c5CarrierTarget.rssi = entry.rssi;
            c5CarrierTarget.rssiSmooth = (int8_t)constrain(
                ((int)c5CarrierTarget.rssiSmooth * 3 + (int)entry.rssi) / 4,
                -127, 0);
            c5CarrierTarget.authType = entry.authType;
            c5CarrierTarget.isHidden = entry.isHidden;
            c5CarrierTarget.lastSeenMs = completedAtMs;
            if (results.timestampMs != 0u &&
                SpectrumThruMath::scanObservationIsNew(
                    results.revision,
                    c5CarrierTarget.lastConsumedRevision)) {
                Bearing::feedRSSIScalarObserved(
                    c5CarrierBearing, c5CarrierBearingConfig,
                    entry.rssi, c5CarrierTarget.rssiSmooth,
                    results.timestampMs, nowMs);
                observeGpsRouteEvidence(entry.rssi, results.timestampMs);
                c5CarrierTarget.lastConsumedRevision = results.revision;
            }
        }
    }

    // Rebuild 5GHz channel activity from this completed snapshot only.
    for (uint8_t i = 0; i < c5gNetworkCount; i++) {
        uint8_t idx = (c5gNetworks[i].channel - 36) / 4;
        if (idx >= CH5G_ARRAY_SIZE) continue;
        ch5gNetCount[idx]++;
        if (c5gNetworks[i].rssi > ch5gPeakRSSI[idx]) {
            ch5gPeakRSSI[idx] = c5gNetworks[i].rssi;
        }
    }

    const int8_t restoredSelectionIdx = hadPreviousSelection
        ? findC5GHzNetworkByBssid(previousSelectedBssid)
        : -1;
    c5gSelectedIdx = (int8_t)SpectrumC5Policy::selectionAfterSnapshot(
        previousSelectedIdx, restoredSelectionIdx, c5gNetworkCount);

    if (isShowing5GHz() &&
        c5gSelectedIdx >= 0 && c5gSelectedIdx < (int8_t)c5gNetworkCount) {
        focus5GHzNetwork(c5gNetworks[c5gSelectedIdx].channel);
    }
}

} // namespace Spectrum
