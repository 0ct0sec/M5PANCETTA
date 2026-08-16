/**
 * BLE Scanner — Bluetooth Device Tracker
 *
 * ==[ PIG EARS ]== Recon's tracker table rendered as a scrollable list.
 * Geiger clicks when you lock onto a device. closer = faster clicks.
 * spectrum vibes but for personal area network parasites.
 *
 * TRACKING MODE: full bearing blip scanner with RAD fan / THRU cloud,
 * expanding pulse, gyro-based direction lock, ghost markers, RSSI trend, orientation
 * compensation. 100% parity with Spectrum's Giger scanner, tuned for
 * BLE cadence (~20s between RSSI samples).
 *
 * rendering: sorted device list with type, MAC, RSSI bar, proximity,
 * signal sparkline. selected device gets Geiger tracking + radar scanner.
 */

#include "ble_scanner.h"
#include "../defense/recon.h"
#include "../defense/xband.h"
#include "../defense/defense_pipeline.h"
#include "../locate/geiger.h"
#include "../util/bearing.h"
#include "../util/rf_util.h"
#include "../audio/sfx.h"
#include "../ui/display.h"
#include "../ui/geiger_scan_math.h"
#include "../ui/geiger_scan_view.h"
#include "../activity/pedometer.h"
#include "../core/config.h"
#include "../haptic/haptic.h"
#include "../util/debug_log.h"
#include <M5Unified.h>
#include <algorithm>
#include <esp_heap_caps.h>
#include <math.h>
#if !defined(CONFIG_BT_NIMBLE_ROLE_CENTRAL_DISABLED)
#include <NimBLEDevice.h>
#endif

namespace BleScanner {

// ==[ LAYOUT CONSTANTS ]==
static constexpr int16_t LIST_TOP       = 28;    // below status bar + summary
static constexpr int16_t ROW_HEIGHT     = 16;
static constexpr int16_t VISIBLE_ROWS   = 9;     // leave room for richer detail pane
static constexpr int16_t DETAIL_TOP     = LIST_TOP + VISIBLE_ROWS * ROW_HEIGHT + 2;
static constexpr int16_t DETAIL_HEIGHT  = 52;

// ==[ RSSI SPARKLINE ]==
static constexpr int SPARK_SAMPLES = 24;
static constexpr uint32_t SPARK_INTERVAL_MS = 500;

// ==[ DETAIL PANE ]==
static constexpr uint32_t DETAIL_PAGES = 7;
static constexpr uint32_t DETAIL_PAGE_MS = 2500;

// ==[ SORT/VIEW STATE ]==
static int16_t selectedIdx = 0;
static int16_t scrollOffset = 0;
static bool tracking = false;             // Geiger active on selected device
static uint8_t trackingPayloadHash[4];    // survive MAC rotation
static uint8_t selectedPayloadHash[4];    // selection survives RSSI re-sorts
static bool selectedPayloadValid = false;

// sorted index: maps display row → Recon tracker table index
static uint8_t sortedIndices[Recon::MAX_BLE_DEVICES];
static int16_t sortedCount = 0;

// RSSI history is mode-local scratch; keep it off permanent internal BSS.
static int8_t* rssiHistory = nullptr;  // [MAX_BLE_DEVICES * SPARK_SAMPLES]
static uint8_t rssiHistoryIdx = 0;
static uint32_t lastSparkUpdate = 0;

// ==[ BEARING TRACKER STATE ]== gyro PDR + RSSI gradient lock
static Bearing::TrackerState bleBearing;
// ==[ BLE BEARING CONFIG ]== tuned for streaming BLE cadence (~1s between samples)
// Fields must match TrackerConfig declaration order in bearing.h
static Bearing::TrackerConfig bleBearingConfig;
static void initBearingConfig() {
    bleBearingConfig.stillMin = 0.95f;
    bleBearingConfig.stillMax = 1.05f;
    bleBearingConfig.calSamples = 50;
    bleBearingConfig.varianceEnter = 0.0008f;
    bleBearingConfig.varianceExit = 0.0003f;
    bleBearingConfig.motionHoldMs = 300;
    bleBearingConfig.rotationThreshold = 8.0f;
    bleBearingConfig.minRotationMs = 500;
    // Lock — streaming cadence, WiFi-like responsiveness
    bleBearingConfig.trendThreshold = 3;       // EMA-delta thresh (well-tested)
    bleBearingConfig.minMotionMs = 800;        // same as WiFi — enough samples now
    bleBearingConfig.initialConfidence = 70;   // higher — streaming gives real signal
    bleBearingConfig.reinforceBoost = 5;       // WiFi-like — samples arrive fast
    bleBearingConfig.minConfidence = 15;
    // Decay — WiFi-like rates (streaming = no more 20s gaps)
    bleBearingConfig.decayRateWrong = 100;
    bleBearingConfig.decayRateStale = 500;
    bleBearingConfig.staleTimeout = 60000;     // 1 min (streaming should keep refreshing)
    bleBearingConfig.iirRatioFlat = 4;
    bleBearingConfig.iirRatioUpright = 3;
    bleBearingConfig.stationarySettleMs = 300u;
    bleBearingConfig.stationaryBoostWhileIdle = 20u;
    // Cadence-aware features (streaming BLE ~1s between adv frames)
    bleBearingConfig.expectedCadenceMs = 2000; // ~1-2s between advertisements
    bleBearingConfig.deltaEmaAlpha = 0.3f;     // WiFi-like alpha — fast cadence
    bleBearingConfig.deltaEmaScale = 3;
}
static bool configInited = false;

// ==[ RAD SEEK REFERENCE ]== view-space origin while RF direction is cold.
// It proves the IMU is turning; it does not claim target AoA.
static float bleScanRefHeading = 0.0f;
static bool bleScanRefValid = false;
static bool bleScanRefFlat = false;

static void resetBleScanReference() {
    bleScanRefHeading = bleBearing.relativeHeading;
    bleScanRefFlat = Pedometer::isCachedFlat();
    bleScanRefValid = true;
}

static void maybeRebaseBleScanReference() {
    if (!bleScanRefValid) return;
    const bool isFlat = Pedometer::isCachedFlat();
    if (isFlat != bleScanRefFlat) {
        bleScanRefHeading = bleBearing.relativeHeading;
        bleScanRefFlat = isFlat;
    }
}

// ==[ GHOST MARKER STATE ]== last known LOCK position when bearing drops
static bool lastKnownValid = false;
static uint16_t lastKnownHeadingDegX10 = 0;
static int16_t lastKnownElevDegX10 = 0;
static uint16_t lastKnownProximity = 0;
static float lastKnownObserverX = 0.0f;
static float lastKnownObserverY = 0.0f;
static uint32_t lastKnownSeenMs = 0;
static uint32_t lastKnownApproachConfirmCount = 0;
static uint32_t lastKnownLockGeneration = 0;

static void resetGhostMarker() {
    lastKnownValid = false;
    lastKnownHeadingDegX10 = 0;
    lastKnownElevDegX10 = 0;
    lastKnownProximity = 0;
    lastKnownObserverX = 0.0f;
    lastKnownObserverY = 0.0f;
    lastKnownSeenMs = 0;
    lastKnownApproachConfirmCount = 0;
    lastKnownLockGeneration = 0;
}

// ==[ GATT ENUMERATION STATE ]==
static Recon::GattDeviceInfo lastGattInfo;
static bool hasGattInfo = false;
static uint8_t gattTargetHash[4] = {};  // track which device the GATT info belongs to

// ==[ RSSI FEED DEDUP ]== only feed Bearing when lastSeen changes
static uint32_t lastFedTimestamp = 0;

// ==[ TARGET LOST ]== detect tracked device disappearance
static bool deviceLost = false;
static bool identityAmbiguous = false;

// ==[ BEARING HAPTIC ]== detect lock transitions
static bool wasBearingLocked = false;

// ==[ BLE PROXIMITY MAPPING ]== RSSI → proximity score (0-1000)
// 3-segment piecewise: close range gets 3x resolution of far range
// Derived from log-distance model with n=3, d₀=1m, RSSI₀=-45dBm
static inline int16_t mapBleProximity(int8_t rssi) {
    int8_t clamped = constrain(rssi, -95, -25);
    if (clamped >= -45) {
        return (int16_t)map(clamped, -45, -25, 500, 1000);  // close: 25 pts/dB
    } else if (clamped >= -70) {
        return (int16_t)map(clamped, -70, -45, 200, 500);   // mid: 12 pts/dB
    } else {
        return (int16_t)map(clamped, -95, -70, 0, 200);     // far: 8 pts/dB
    }
}

// BLE scans arrive in long bursts separated by idle gaps.
// Keep the bearing tracker aware of real walking sessions so RSSI samples
// landing a few seconds after a step burst can still produce a walking lock.
static void syncWalkingMotionToBearing(uint32_t now) {
    if (Pedometer::getMotionState() != MotionState::WALKING) return;

    uint32_t walkingDuration = Pedometer::getWalkingDuration();
    uint32_t walkingStart = (walkingDuration > now) ? 0 : (now - walkingDuration);

    bleBearing.motionState = true;
    bleBearing.isMoving = true;
    if (bleBearing.accelMotionEnergy < 70.0f) {
        bleBearing.accelMotionEnergy = 70.0f;
    }
    if (bleBearing.thruMotionHeat < 70u) bleBearing.thruMotionHeat = 70u;
    bleBearing.stationaryConfidence = 0u;
    bleBearing.lastHighVariance = now;
    bleBearing.lastMotionTime = now;

    if (bleBearing.motionStartTime == 0 ||
        (walkingStart > 0 && bleBearing.motionStartTime > walkingStart)) {
        bleBearing.motionStartTime = walkingStart;
    }
}

static bool ensureSparkHistory() {
    if (rssiHistory) return true;
    size_t bytes = Recon::MAX_BLE_DEVICES * SPARK_SAMPLES * sizeof(int8_t);
    rssiHistory = (int8_t*)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (!rssiHistory) rssiHistory = (int8_t*)malloc(bytes);
    if (!rssiHistory) {
        HAMLET_LOGF("[BLE] spark history alloc failed (%u bytes)\n", (unsigned)bytes);
        return false;
    }
    memset(rssiHistory, -100, bytes);
    return true;
}

static void freeSparkHistory() {
    if (!rssiHistory) return;
    heap_caps_free(rssiHistory);
    rssiHistory = nullptr;
}

static inline int8_t& sparkSample(int row, int sample) {
    return rssiHistory[row * SPARK_SAMPLES + sample];
}

// ==[ SPAM PLATFORM LABELS ]== 4-char max
static const char* spamLabel(uint8_t platform) {
    switch ((Recon::SpamPlatform)platform) {
        case Recon::SpamPlatform::IOS:     return "SP:i";
        case Recon::SpamPlatform::WINDOWS: return "SP:W";
        case Recon::SpamPlatform::SAMSUNG: return "SP:S";
        case Recon::SpamPlatform::ANDROID: return "SP:A";
        default:                           return "Spam";
    }
}

static void keepSelectionVisible() {
    if (sortedCount <= 0) {
        selectedIdx = 0;
        scrollOffset = 0;
        return;
    }

    if (selectedIdx < 0) selectedIdx = 0;
    if (selectedIdx >= sortedCount) selectedIdx = sortedCount - 1;

    int16_t maxScroll = sortedCount - VISIBLE_ROWS;
    if (maxScroll < 0) maxScroll = 0;
    if (scrollOffset < 0) scrollOffset = 0;
    if (scrollOffset > maxScroll) scrollOffset = maxScroll;
    if (selectedIdx < scrollOffset) scrollOffset = selectedIdx;
    if (selectedIdx >= scrollOffset + VISIBLE_ROWS) {
        scrollOffset = selectedIdx - VISIBLE_ROWS + 1;
    }
}

static void rememberSelectedDevice(const Recon::TrackerEntry* table) {
    if (!table || selectedIdx < 0 || selectedIdx >= sortedCount) {
        selectedPayloadValid = false;
        return;
    }
    memcpy(selectedPayloadHash, table[sortedIndices[selectedIdx]].payloadHash,
           sizeof(selectedPayloadHash));
    selectedPayloadValid = true;
}

// ==[ SORT ]== strongest signal first, FOLLOWING pinned to top
static void rebuildSortedList() {
    const Recon::TrackerEntry* table = DefensePipeline::snapshot().getBleDevices();
    int tableSize = DefensePipeline::snapshot().getBleDeviceTableSize();
    sortedCount = 0;

    // PSRAM pressure can leave Recon without its catalog. Keep the menu alive
    // long enough to say so instead of dereferencing a null table.
    if (!table || tableSize <= 0) {
        selectedPayloadValid = false;
        selectedIdx = 0;
        scrollOffset = 0;
        return;
    }

    for (int i = 0; i < tableSize && i < Recon::MAX_BLE_DEVICES; i++) {
        sortedIndices[sortedCount++] = i;
    }

    // sort: FOLLOWING first, then spam/known tracker types, then by smoothed RSSI descending
    std::sort(sortedIndices, sortedIndices + sortedCount,
        [&table](int16_t a, int16_t b) {
            uint8_t aScore = 0;
            uint8_t bScore = 0;
            bool aFollow = (table[a].flags & Recon::FLAG_FOLLOWING) != 0;
            bool bFollow = (table[b].flags & Recon::FLAG_FOLLOWING) != 0;
            if (aFollow) aScore = 3;
            else if (table[a].flags & Recon::FLAG_SPAM) aScore = 2;
            else if (table[a].type != Recon::ThreatType::UNKNOWN) aScore = 1;
            if (bFollow) bScore = 3;
            else if (table[b].flags & Recon::FLAG_SPAM) bScore = 2;
            else if (table[b].type != Recon::ThreatType::UNKNOWN) bScore = 1;
            if (aScore != bScore) return aScore > bScore;
            return table[a].rssiSmooth > table[b].rssiSmooth;
        });

    // The catalog is deliberately re-ranked every frame. Keep selection tied to
    // the payload identity, not the old display row, or an RSSI shuffle can lock
    // the operator onto a different device.
    if (selectedPayloadValid) {
        for (int i = 0; i < sortedCount; i++) {
            if (memcmp(table[sortedIndices[i]].payloadHash,
                       selectedPayloadHash, sizeof(selectedPayloadHash)) == 0) {
                selectedIdx = i;
                break;
            }
        }
    }
    keepSelectionVisible();
    rememberSelectedDevice(table);
}

// ==[ SPARKLINE UPDATE ]== sample RSSI for each visible device
static void updateSparklines(uint32_t now) {
    if (now - lastSparkUpdate < SPARK_INTERVAL_MS) return;
    lastSparkUpdate = now;
    if (!rssiHistory) return;

    const Recon::TrackerEntry* table = DefensePipeline::snapshot().getBleDevices();
    if (!table) return;
    for (int i = 0; i < sortedCount; i++) {
        sparkSample(sortedIndices[i], rssiHistoryIdx) = table[sortedIndices[i]].rssiSmooth;
    }
    rssiHistoryIdx = (rssiHistoryIdx + 1) % SPARK_SAMPLES;
}

// ==[ TRACKING ]== find tracked device even after MAC rotation
static int16_t findTrackedDevice() {
    if (!tracking) return -1;
    const Recon::TrackerEntry* table = DefensePipeline::snapshot().getBleDevices();
    if (!table) return -1;
    for (int i = 0; i < sortedCount; i++) {
        const auto& te = table[sortedIndices[i]];
        if (memcmp(te.payloadHash, trackingPayloadHash, 4) == 0) return i;
    }
    return -1;  // lost
}

// ==[ PUBLIC API ]==

void start() {
    if (!configInited) { initBearingConfig(); configInited = true; }

    selectedIdx = 0;
    scrollOffset = 0;
    tracking = false;
    selectedPayloadValid = false;
    sortedCount = 0;
    rssiHistoryIdx = 0;
    lastSparkUpdate = 0;
    lastFedTimestamp = 0;
    ensureSparkHistory();
    if (rssiHistory) {
        memset(rssiHistory, -100, Recon::MAX_BLE_DEVICES * SPARK_SAMPLES * sizeof(int8_t));
    }

    Bearing::reset(bleBearing);
    resetBleScanReference();
    identityAmbiguous = false;
    resetGhostMarker();

    DefensePipeline::clearPinnedBleDevice();
    DefensePipeline::requestOperatingState(Defense::OperatingState::BLE_PRIORITY);
    rebuildSortedList();
}

void stop() {
    if (tracking) {
        Geiger::stop();
        tracking = false;
    }
    DefensePipeline::clearPinnedBleDevice();
    freeSparkHistory();
    DefensePipeline::requestOperatingState(Defense::OperatingState::BACKGROUND);
}

void update() {
    uint32_t now = millis();

    rebuildSortedList();
    updateSparklines(now);

    // ==[ BEARING IMU FEED ]== every frame, regardless of RSSI arrival
    if (tracking) {
        float gx, gy, gz, ax, ay, az;
        Pedometer::getCachedGyro(gx, gy, gz);
        Pedometer::getCachedAccel(ax, ay, az);
        Bearing::updateIMU(bleBearing, bleBearingConfig, gx, gy, gz, ax, ay, az);
        maybeRebaseBleScanReference();
        syncWalkingMotionToBearing(now);
    }

    // ==[ GEIGER + BEARING RSSI FEED ]== pump RSSI into Geiger + Bearing for tracked device
    if (tracking) {
        int16_t tIdx = findTrackedDevice();
        if (tIdx >= 0) {
            if (deviceLost) {
                deviceLost = false;
                Display::showToast("TARGET REACQUIRED", 2000);
            }

            const Recon::TrackerEntry* table = DefensePipeline::snapshot().getBleDevices();
            const auto& te = table[sortedIndices[tIdx]];
            uint32_t age = now - te.lastSeen;
            Geiger::update(te.rssiSmooth, age);

            // Feed Bearing ONLY when new scan data arrives (dedup on lastSeen)
            const bool ambiguousNow = te.identityCandidates > 1u;
            if (ambiguousNow && !identityAmbiguous) {
                Bearing::reset(bleBearing);
                resetBleScanReference();
                Display::showToast("MULTIPLE MATCHES - BEARING HOLD", 2200);
            }
            identityAmbiguous = ambiguousNow;
            if (!identityAmbiguous && te.lastSeen != lastFedTimestamp) {
                Bearing::feedRSSI(bleBearing, bleBearingConfig,
                                  te.rssi, te.rssiSmooth);
                lastFedTimestamp = te.lastSeen;
            } else if (identityAmbiguous) {
                lastFedTimestamp = te.lastSeen;
            }

            // keep selection following the tracked device
            selectedIdx = tIdx;
        } else {
            // device lost — keep Geiger in stale mode
            if (!deviceLost) {
                deviceLost = true;
                Display::showToast("TARGET LOST", 3000);
                Haptic::buzz();
            }
            Geiger::update(-90, 6000);
        }

        // ==[ BEARING HAPTIC ]== tactile feedback on lock transitions
        bool isLocked = bleBearing.bearingLocked &&
                        bleBearing.lockConfidence > bleBearingConfig.minConfidence;
        if (isLocked && !wasBearingLocked) Haptic::tick();
        if (!isLocked && wasBearingLocked) Haptic::doubleTap();
        wasBearingLocked = isLocked;
    }
}

void nextDevice() {
    if (sortedCount == 0) return;
    selectedIdx = (selectedIdx + 1) % sortedCount;
    keepSelectionVisible();
    rememberSelectedDevice(DefensePipeline::snapshot().getBleDevices());
}

void prevDevice() {
    if (sortedCount == 0) return;
    selectedIdx = (selectedIdx - 1 + sortedCount) % sortedCount;
    keepSelectionVisible();
    rememberSelectedDevice(DefensePipeline::snapshot().getBleDevices());
}

void selectDevice() {
    // Input can arrive between Recon committing a fresh scan and the next mode
    // update. Rebuild once here so the selected payload, not a stale row number,
    // is what gets pinned and tracked.
    rebuildSortedList();
    if (sortedCount == 0) return;

    if (tracking) {
        // already tracking — toggle off
        exitDetail();
        return;
    }

    const Recon::TrackerEntry* table = DefensePipeline::snapshot().getBleDevices();
    if (!table) return;
    const auto& te = table[sortedIndices[selectedIdx]];
    memcpy(trackingPayloadHash, te.payloadHash, 4);
    tracking = true;
    lastFedTimestamp = 0;
    deviceLost = false;
    wasBearingLocked = false;
    hasGattInfo = false;  // clear GATT info for new target

    Bearing::reset(bleBearing);
    resetBleScanReference();
    resetGhostMarker();

    DefensePipeline::pinBleDevice(trackingPayloadHash);
    Geiger::start(Geiger::SOURCE_BLE);
}

bool trackDeviceByPayloadHash(const uint8_t* payloadHash) {
    if (!payloadHash) return false;

    // A DEFHOG4 action can arrive immediately after Recon commits a fresh
    // scan. Rebuild so the durable payload identity—not a stale display row—
    // becomes the selected target.
    rebuildSortedList();
    const Recon::TrackerEntry* table = DefensePipeline::snapshot().getBleDevices();
    if (!table || sortedCount <= 0) return false;

    for (int i = 0; i < sortedCount; ++i) {
        const auto& candidate = table[sortedIndices[i]];
        if (memcmp(candidate.payloadHash, payloadHash,
                   sizeof(trackingPayloadHash)) != 0) {
            continue;
        }

        if (tracking) exitDetail();
        selectedIdx = i;
        keepSelectionVisible();
        rememberSelectedDevice(table);
        selectDevice();
        return tracking;
    }
    return false;
}

void exitDetail() {
    if (tracking) {
        Geiger::stop();
        tracking = false;
    }
    DefensePipeline::clearPinnedBleDevice();
}

void resetBearing() {
    Bearing::reset(bleBearing);
    resetBleScanReference();
    resetGhostMarker();
    lastFedTimestamp = 0;
}

void toggleActiveScan() {
    if (!DefensePipeline::snapshot().isBleAvailable()) {
        Display::showToast("BLE RADIO UNAVAILABLE", 2000);
        return;
    }
    if (!DefensePipeline::snapshot().isBleInitialized()) {
        Display::showToast("BLE ENGINE STARTING", 1500);
        return;
    }

    // 3-state cycle: passive → active → active+chaff → passive
    if (DefensePipeline::snapshot().isChaffActive()) {
        // chaff on → back to passive
        DefensePipeline::toggleChaff();
        DefensePipeline::setActiveScan(false);
        Display::showToast("PASSIVE SCAN", 1500);
    } else if (DefensePipeline::snapshot().isActiveScanEnabled()) {
        // active → active+chaff
        DefensePipeline::toggleChaff();
        Display::showToast("ACTIVE + CHAFF", 1500);
    } else {
        // passive → active
        DefensePipeline::setActiveScan(true);
        Display::showToast("ACTIVE SCAN", 1500);
    }
}

bool canTriggerSound() {
    if (!tracking || sortedCount == 0) return false;
    int16_t idx = findTrackedDevice();
    if (idx < 0) return false;
    const Recon::TrackerEntry* table = DefensePipeline::snapshot().getBleDevices();
    if (!table) return false;
    const auto& te = table[sortedIndices[idx]];
    return te.type == Recon::ThreatType::AIRTAG &&
           (te.flags & Recon::FLAG_FOLLOWING) &&
           (millis() - te.lastSeen < 30000);  // seen within 30s
}

void triggerAirTagSound() {
#if !defined(CONFIG_BT_NIMBLE_ROLE_CENTRAL_DISABLED)
    if (!canTriggerSound()) return;

    // 5-second cooldown — each attempt blocks the main loop for up to 6s
    // via GATT connect; without this, holding BtnA chains GATT timeouts.
    static uint32_t lastTriggerMs = 0;
    uint32_t now = millis();
    if (lastTriggerMs != 0 && (now - lastTriggerMs) < 5000) {
        Display::showToast("C00LD0WN. W41T.", 1500);
        return;
    }
    lastTriggerMs = now;

    int16_t idx = findTrackedDevice();
    if (idx < 0) return;
    const Recon::TrackerEntry* table = DefensePipeline::snapshot().getBleDevices();
    const auto& te = table[sortedIndices[idx]];

    // show feedback before blocking operation
    Display::showToast("PINGING AIRTAG...", 5000);
    Display::drawBleScreen();  // force render — GATT blocks main loop

    // pause scan for GATT client connection
    DefensePipeline::pauseBLEScanForGATT();

    bool success = false;
    NimBLEClient* pClient = NimBLEDevice::createClient();
    if (pClient) {
        pClient->setConnectTimeout(6);
        uint8_t macCopy[6];
        memcpy(macCopy, te.mac, 6);
        NimBLEAddress addr(macCopy, te.addrType);
        if (pClient->connect(addr)) {
            NimBLERemoteService* pSvc = pClient->getService(
                NimBLEUUID("7DFC9000-7D1C-4951-86AA-8D9728F8D66C"));
            if (pSvc) {
                NimBLERemoteCharacteristic* pChr = pSvc->getCharacteristic(
                    NimBLEUUID("7DFC9001-7D1C-4951-86AA-8D9728F8D66C"));
                if (pChr && pChr->canWrite()) {
                    uint8_t cmd = 0x01;
                    success = pChr->writeValue(&cmd, 1);
                }
            }
            pClient->disconnect();
        }
        NimBLEDevice::deleteClient(pClient);
    }

    // resume scanning
    DefensePipeline::resumeBLEScanFromGATT();

    if (success) {
        Display::showAlertToast("AIRTAG PINGED.", 3000);
        SFX::play(SFX::RECON_ALERT);
        HAMLET_LOGLN("[BLE] AirTag sound triggered");
    } else {
        Display::showToast("FAILED - OUT OF RANGE", 3000);
        HAMLET_LOGLN("[BLE] AirTag sound FAILED");
    }
#else
    Display::showToast("GATT CLIENT DISABLED", 2000);
#endif
}

void enumerateGattDevice() {
#if !defined(CONFIG_BT_NIMBLE_ROLE_CENTRAL_DISABLED)
    int16_t idx = findTrackedDevice();
    if (idx < 0) return;
    const Recon::TrackerEntry* table = DefensePipeline::snapshot().getBleDevices();
    const auto& te = table[sortedIndices[idx]];
    if (millis() - te.lastSeen > 30000) {
        Display::showToast("STALE - NOT SEEN 30s", 2000);
        return;
    }
    // advType 2=SCAN_IND, 3=NONCONN_IND — device won't accept connections
    if (te.advType >= 2 && te.advType <= 3) {
        Display::showToast("NON-CONNECTABLE DEV", 2000);
        HAMLET_LOGF("[BLE] GATT skip: advType=%d (non-connectable)\n", te.advType);
        return;
    }

    Display::showToast("GATT PROBING...", 8000);
    Display::drawBleScreen();  // force render — GATT blocks main loop
    DefensePipeline::pauseBLEScanForGATT();

    Recon::GattDeviceInfo info = {};
    info.batteryLevel = -1;

    // retry once — first attempt can fail if controller still settling
    for (int attempt = 0; attempt < 2 && !info.valid; attempt++) {
        if (attempt > 0) {
            delay(200);
            HAMLET_LOGLN("[BLE] GATT retry...");
        }

    NimBLEClient* pClient = NimBLEDevice::createClient();
    if (pClient) {
        pClient->setConnectTimeout(6);
        uint8_t macCopy[6];
        memcpy(macCopy, te.mac, 6);
        NimBLEAddress addr(macCopy, te.addrType);

        if (pClient->connect(addr)) {
            // Device Information Service (0x180A)
            NimBLERemoteService* dis = pClient->getService(NimBLEUUID((uint16_t)0x180A));
            if (dis) {
                auto readStr = [&](uint16_t uuid, char* dst, size_t len) {
                    NimBLERemoteCharacteristic* c = dis->getCharacteristic(NimBLEUUID(uuid));
                    if (c && c->canRead()) {
                        std::string val = c->readValue();
                        size_t copyLen = val.length() < len - 1 ? val.length() : len - 1;
                        memcpy(dst, val.c_str(), copyLen);
                        dst[copyLen] = '\0';
                    }
                };
                readStr(0x2A29, info.manufacturer, sizeof(info.manufacturer));
                readStr(0x2A24, info.model, sizeof(info.model));
                readStr(0x2A26, info.firmware, sizeof(info.firmware));
                readStr(0x2A25, info.serial, sizeof(info.serial));
            }

            // Battery Service (0x180F)
            NimBLERemoteService* bas = pClient->getService(NimBLEUUID((uint16_t)0x180F));
            if (bas) {
                NimBLERemoteCharacteristic* batt = bas->getCharacteristic(
                    NimBLEUUID((uint16_t)0x2A19));
                if (batt && batt->canRead()) {
                    std::string val = batt->readValue();
                    if (val.length() >= 1)
                        info.batteryLevel = (int8_t)(uint8_t)val[0];
                }
            }

            info.valid = true;
            pClient->disconnect();
        }
        NimBLEDevice::deleteClient(pClient);
    }
    } // retry loop

    DefensePipeline::resumeBLEScanFromGATT();

    if (info.valid) {
        lastGattInfo = info;
        hasGattInfo = true;
        memcpy(gattTargetHash, te.payloadHash, 4);
        Display::showAlertToast("GATT OK", 2000);
        SFX::play(SFX::RECON_ALERT);
        HAMLET_LOGF("[BLE] GATT: mfg=%s mod=%s fw=%s bat=%d%%\n",
                      info.manufacturer, info.model,
                      info.firmware, info.batteryLevel);
    } else {
        hasGattInfo = false;
        Display::showToast("GATT FAILED", 2000);
        HAMLET_LOGLN("[BLE] GATT connect failed");
    }
#else
    Display::showToast("GATT DISABLED", 2000);
#endif
}

void addToWatchlist() {
    if (!tracking || sortedCount == 0) return;

    int16_t idx = findTrackedDevice();
    if (idx < 0) return;

    const Recon::TrackerEntry* table = DefensePipeline::snapshot().getBleDevices();
    const auto& te = table[sortedIndices[idx]];

    // already on watchlist?
    if (DefensePipeline::snapshot().findWatchlistSlot(te.payloadHash) >= 0) {
        Display::showToast("ALREADY WATCHED", 2000);
        return;
    }

    // auto-generate label: prefer BLE name, fallback to type + MAC suffix
    char label[16];
    if (te.name[0]) {
        strncpy(label, te.name, 15);
        label[15] = '\0';
    } else {
        snprintf(label, sizeof(label), "%s %02X%02X",
                 Recon::deviceLabel(te), te.mac[4], te.mac[5]);
    }

    if (DefensePipeline::addToWatchlist(te.payloadHash, label)) {
        char msg[32];
        snprintf(msg, sizeof(msg), "WATCHING: %s", label);
        Display::showAlertToast(msg, 3000);
        SFX::play(SFX::RECON_ALERT);
    } else {
        char fullMsg[32];
        snprintf(fullMsg, sizeof(fullMsg), "WATCHLIST FULL (%d/%d)",
                 Recon::MAX_WATCHLIST, Recon::MAX_WATCHLIST);
        Display::showToast(fullMsg, 2000);
    }
}

bool isTracking() { return tracking; }
int16_t getSelectedIndex() { return selectedIdx; }
int16_t getDeviceCount() { return sortedCount; }
bool isRadioAvailable() { return DefensePipeline::snapshot().isBleAvailable(); }
bool isRadioReady() { return DefensePipeline::snapshot().isBleInitialized(); }
const char* getScanModeHint() {
    if (!DefensePipeline::snapshot().isBleInitialized()) return "";
    if (DefensePipeline::snapshot().isChaffActive()) return "[SWIPE>]PASS";
    if (DefensePipeline::snapshot().isActiveScanEnabled()) return "[SWIPE>]CHF";
    return "[SWIPE>]ACT";
}

// ==[ RENDERING ]==

static const Recon::TrackerEntry* getDetailTracker(int16_t& sortIdx) {
    if (sortedCount <= 0) {
        sortIdx = -1;
        return nullptr;
    }

    sortIdx = tracking ? findTrackedDevice() : selectedIdx;
    if (sortIdx < 0 || sortIdx >= sortedCount) sortIdx = selectedIdx;
    if (sortIdx < 0 || sortIdx >= sortedCount) {
        sortIdx = -1;
        return nullptr;
    }

    const Recon::TrackerEntry* table = DefensePipeline::snapshot().getBleDevices();
    if (!table) {
        sortIdx = -1;
        return nullptr;
    }
    return &table[sortedIndices[sortIdx]];
}

static void formatMac(char* out, size_t outSize, const uint8_t* mac) {
    snprintf(out, outSize, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void formatAgeCompact(char* out, size_t outSize, uint32_t msAgo) {
    uint32_t age = msAgo / 1000;
    if (age < 60) snprintf(out, outSize, "%lus", age);
    else if (age < 3600) snprintf(out, outSize, "%lum", age / 60);
    else snprintf(out, outSize, "%luh", age / 3600);
}

static const char* addrTypeLabel(uint8_t addrType) {
    switch (addrType) {
        case 0: return "PUB";
        case 1: return "RND";
        case 2: return "PID";
        case 3: return "RID";
        default: return "?";
    }
}

static const char* advTypeLabel(uint8_t advType) {
    switch (advType) {
        case 0: return "IND";
        case 1: return "DIR";
        case 2: return "SCAN";
        case 3: return "NON";
        case 4: return "RSP";
        default: return "?";
    }
}

static const char* frameLabel(const Recon::TrackerEntry& te) {
    switch (te.type) {
        case Recon::ThreatType::AIRTAG:
            return "FMY";
        case Recon::ThreatType::IBEACON:
            return "iBCN";
        case Recon::ThreatType::EDDYSTONE:
            switch (te.frameType) {
                case 0x00: return "UID";
                case 0x10: return "URL";
                case 0x20: return "TLM";
                case 0x30: return "EID";
                default:   return "EDDY";
            }
        case Recon::ThreatType::SUSPICIOUS_PERIPHERAL:
            return "HM10";
        case Recon::ThreatType::FAST_PAIR:
            return "PAIR";
        case Recon::ThreatType::FLIPPER:
            return "FZro";
        case Recon::ThreatType::HID_DEVICE:
            return "HID";
        case Recon::ThreatType::SMARTTAG_UNREGISTERED:
            return "UNREG";
        case Recon::ThreatType::XIAOMI_TRACKER:
            return "MiBcn";
        case Recon::ThreatType::SIDEWALK_BEACON:
            return "SDWK";
        case Recon::ThreatType::EXPOSURE_NOTIF:
            return "GAEN";
        case Recon::ThreatType::FMDN:
            return "GFMD";
        default:
            break;
    }

    if (te.frameType == 0) return "--";
    return "SUB";
}

static const char* vendorLabel(const Recon::TrackerEntry& te) {
    // try company ID lookup first (25+ vendors)
    const char* mfg = Recon::manufacturerLabel(te.companyId);
    if (mfg[0] != '?') return mfg;

    // fallback: service UUID → vendor
    switch (te.primaryService) {
        case 0xFE2C: return "Google";
        case 0xFEAA: return "Google";
        case 0xFEED: return "Tile";
        case 0xFFE0: return "HM-10";
        case 0xFE95: return "Xiaomi";
        case 0xFD82: return "Amazon";
        case 0xFD6F: return "GAEN";
        default: break;
    }

    // fallback: threat type → vendor
    switch (te.type) {
        case Recon::ThreatType::AIRTAG:
        case Recon::ThreatType::IBEACON:
            return "Apple";
        case Recon::ThreatType::SMARTTAG:
            return "Samsung";
        case Recon::ThreatType::FAST_PAIR:
        case Recon::ThreatType::EDDYSTONE:
            return "Google";
        case Recon::ThreatType::TILE:
            return "Tile";
        case Recon::ThreatType::SUSPICIOUS_PERIPHERAL:
            return "HM-10";
        case Recon::ThreatType::FLIPPER:
            return "Flipper";
        case Recon::ThreatType::XIAOMI_TRACKER:
            return "Xiaomi";
        case Recon::ThreatType::SIDEWALK_BEACON:
            return "Amazon";
        case Recon::ThreatType::EXPOSURE_NOTIF:
            return "GAEN";
        case Recon::ThreatType::FMDN:
            return "Google";
        default:
            break;
    }

    return "--";
}

static void formatOui(char* out, size_t outSize, const uint8_t* mac) {
    snprintf(out, outSize, "%02X:%02X:%02X", mac[0], mac[1], mac[2]);
}

static void formatPayloadHex(char* out, size_t outSize, const uint8_t* data,
                             uint8_t len, uint8_t start, uint8_t count) {
    if (!out || outSize == 0) return;
    out[0] = '\0';
    if (!data || len == 0 || start >= len) {
        strncpy(out, "--", outSize - 1);
        out[outSize - 1] = '\0';
        return;
    }

    size_t pos = 0;
    uint8_t end = start + count;
    if (end > len) end = len;
    for (uint8_t i = start; i < end; i++) {
        int written = snprintf(out + pos, outSize - pos, "%02X", data[i]);
        if (written <= 0 || (size_t)written >= outSize - pos) break;
        pos += (size_t)written;
        if (i + 1 < end && pos + 1 < outSize) {
            out[pos++] = ' ';
            out[pos] = '\0';
        }
    }
}

static void drawSummaryBar(M5Canvas& c) {
    uint16_t fg = Display::getColorFG();
    uint16_t bg = Display::getColorBG();

    int devCount = DefensePipeline::snapshot().getBleDeviceTableSize();
    int trkCount = DefensePipeline::snapshot().getTrackerCount();
    int followCount = DefensePipeline::snapshot().getFollowingCount();
    uint16_t totalBle = DefensePipeline::snapshot().getTotalBLEDevicesSeen();

    // summary bar at y=14..27 (14px)
    c.fillRect(0, 14, 320, 14, bg);
    c.setTextSize(1);
    c.setTextDatum(textdatum_t::top_left);

    char buf[54];
    snprintf(buf, sizeof(buf), "%d/%d DEV | %d THR | %d FOL | %u TOT",
             devCount, Recon::MAX_BLE_DEVICES, trkCount, followCount, totalBle);
    c.setTextColor(fg);
    c.drawString(buf, 2, 15);

    // Mode badges + scan liveness. A static "..." was too subtle when the
    // scanner was warming up or had fallen out of the BLE build.
    if (DefensePipeline::snapshot().isChaffActive()) {
        c.drawString("CHF", 238, 15);  // chaff broadcasting
    }
    if (DefensePipeline::snapshot().isActiveScanEnabled()) {
        c.drawString("ACT", 268, 15);  // active probe mode
    }
    if (DefensePipeline::snapshot().isScanning()) {
        c.fillRect(294, 15, 24, 12, fg);
        c.setTextColor(bg);
        c.setTextDatum(textdatum_t::middle_center);
        c.drawString("SCN", 306, 21);
    } else {
        const char* state = !DefensePipeline::snapshot().isBleAvailable() ? "OFF" :
                            !DefensePipeline::snapshot().isBleInitialized() ? "BOOT" : "ARM";
        c.setTextDatum(textdatum_t::top_right);
        c.drawString(state, 318, 15);
    }
    c.setTextColor(fg);
    c.setTextDatum(textdatum_t::top_left);
}

static void drawDeviceRow(M5Canvas& c, int displayRow, int sortIdx, bool selected) {
    const Recon::TrackerEntry* table = DefensePipeline::snapshot().getBleDevices();
    if (!table || sortIdx < 0 || sortIdx >= sortedCount) return;
    const auto& te = table[sortedIndices[sortIdx]];
    uint16_t fg = Display::getColorFG();
    uint16_t bg = Display::getColorBG();

    int16_t y = LIST_TOP + displayRow * ROW_HEIGHT;

    // selected row: inverted
    uint16_t textColor = selected ? bg : fg;
    uint16_t rowBg = selected ? fg : bg;
    c.fillRect(0, y, 320, ROW_HEIGHT, rowBg);

    bool isFollowing = (te.flags & Recon::FLAG_FOLLOWING) != 0;
    bool isSpam = (te.flags & Recon::FLAG_SPAM) != 0;
    bool isTracked = tracking && memcmp(te.payloadHash, trackingPayloadHash, 4) == 0;

    // ==[ THREAT BAR ]== 2px left edge — theme-derived intensity
    if (isFollowing) {
        uint16_t alertCol = Display::lerpColor565(bg, fg, 0.85f);
        c.fillRect(0, y, 2, ROW_HEIGHT, selected ? bg : alertCol);
    } else if (isSpam) {
        uint16_t warnCol = Display::lerpColor565(bg, fg, 0.60f);
        c.fillRect(0, y, 2, ROW_HEIGHT, selected ? bg : warnCol);
    }

    // ==[ XBAND ATTACKER MARKER ]== 2px right edge — identified attack source
    {
        const XBand::AttackerProfile* atks = DefensePipeline::snapshot().getAttackerProfiles();
        int atkCount = DefensePipeline::snapshot().getAttackerCount();
        for (int a = 0; a < atkCount; a++) {
            if (memcmp(atks[a].blePayloadHash, te.payloadHash, 4) == 0) {
                uint16_t atkCol = Display::lerpColor565(bg, fg, 0.90f);
                c.fillRect(318, y, 2, ROW_HEIGHT, selected ? bg : atkCol);
                break;
            }
        }
    }

    c.setTextSize(1);
    c.setTextDatum(textdatum_t::top_left);
    c.setTextColor(textColor);

    // ==[ TYPE ]== x=3, 4-char label (appearance→name→CoD→threat cascade)
    const char* tl = Recon::deviceLabel(te);
    c.drawString(tl, 3, y + 4);

    // ==[ MAC ]== x=32, XX:XX:XX:XX:XX:XX (17ch = 102px)
    char mac[18];
    formatMac(mac, sizeof(mac), te.mac);
    c.drawString(mac, 32, y + 4);

    // ==[ SPARKLINE ]== x=138, 24px wide, 10px tall
    {
        int16_t sx = 138;
        int16_t sy = y + 3;
        int16_t sw = SPARK_SAMPLES;
        int16_t sh = 10;
        for (int s = 0; rssiHistory && s < sw; s++) {
            int idx = (rssiHistoryIdx + s) % SPARK_SAMPLES;
            int8_t rssi = sparkSample(sortedIndices[sortIdx], idx);
            if (rssi <= -100) continue;
            // map -95..-25 → 0..sh (match tracking sparkline range)
            int h = map(constrain(rssi, -95, -25), -95, -25, 1, sh);
            c.drawFastVLine(sx + s, sy + sh - h, h, textColor);
        }
    }

    // ==[ PROXIMITY LABEL ]== x=165
    const char* prox = Recon::proximityLabel(te.rssiSmooth);
    c.drawString(prox, 165, y + 4);

    // ==[ AGE ]== x=205 (compact: "12s", "5m", "1h")
    {
        char ageBuf[8];
        formatAgeCompact(ageBuf, sizeof(ageBuf), millis() - te.lastSeen);
        c.drawString(ageBuf, 205, y + 4);
    }

    // ==[ MAC ROTATIONS ]== x=235 (if any)
    if (te.macChangeCount > 0) {
        char rotBuf[8];
        snprintf(rotBuf, sizeof(rotBuf), "R%u", te.macChangeCount);
        c.drawString(rotBuf, 235, y + 4);
    }

    // ==[ RSSI dBm ]== right-aligned x=312
    {
        char rssiBuf[8];
        snprintf(rssiBuf, sizeof(rssiBuf), "%ddB", te.rssiSmooth);
        c.setTextDatum(textdatum_t::top_right);
        c.drawString(rssiBuf, 316, y + 4);
        c.setTextDatum(textdatum_t::top_left);
    }

    // ==[ TRACKING INDICATOR ]== blink dot
    if (isTracked) {
        bool blink = (millis() / 400) % 2;
        if (blink) {
            c.fillCircle(318, y + 8, 2, textColor);
        }
    }
}

// ==[ BEARING BLIP SCANNER ]== full Giger-parity radar when tracking a BLE device
static void drawBearingScanner(M5Canvas& canvas) {
    int16_t tIdx = -1;
    const Recon::TrackerEntry* te = getDetailTracker(tIdx);
    if (!te) return;

    uint16_t fg = Display::getColorFG();
    uint16_t bg = Display::getColorBG();

    // ==[ SCANNER BOX ]== full-width Wildcard-style RF panel
    int boxW = 304, boxH = 170;
    int boxX = (320 - boxW) / 2;
    int boxY = 36;

    uint16_t scanBg = bg;
    uint16_t scanFg = fg;

    // ==[ TARGET ]== type + MAC + vendor
    char macStr[18];
    formatMac(macStr, sizeof(macStr), te->mac);
    char headerBuf[40];
    snprintf(headerBuf, sizeof(headerBuf), "%s %s %s",
             Recon::deviceLabel(*te), macStr, vendorLabel(*te));

    // Get RSSI data from Geiger (if active) or tracker entry
    int8_t smoothed = te->rssiSmooth;
    bool geigerActive = (Geiger::isActive() &&
                         Geiger::getSource() == Geiger::SOURCE_BLE);

    if (geigerActive) {
        smoothed = Geiger::getSmoothed();
    }

    // BLE-specific proximity mapping (wider range, 3-segment piecewise)
    int proxLevel = mapBleProximity(smoothed);
    const uint32_t nowMs = millis();
    const uint32_t targetAgeMs = nowMs - te->lastSeen;
    const bool stablePosition = !identityAmbiguous &&
                                 bleBearing.bearingLocked &&
                                 bleBearing.lockConfidence > 60u &&
                                 !bleBearing.porkBehind &&
                                 targetAgeMs <= 30000u;
    const bool shouldRefreshAnchor = GeigerScanMath::anchorNeedsRefresh(
        lastKnownValid, bleBearing.lockGeneration, lastKnownLockGeneration,
        bleBearing.approachConfirmCount, lastKnownApproachConfirmCount);
    if (stablePosition && shouldRefreshAnchor) {
        lastKnownHeadingDegX10 = GeigerScanMath::anchorHeadingX10(
            bleBearing.lastHeadingDegX10, bleBearing.bearingRaw);
        lastKnownElevDegX10 = bleBearing.isFlat
            ? 0 : bleBearing.lastElevDegX10;
        lastKnownProximity = (uint16_t)constrain(proxLevel, 0, 1000);
        lastKnownObserverX = bleBearing.observerPositionX;
        lastKnownObserverY = bleBearing.observerPositionY;
        lastKnownSeenMs = bleBearing.lastDirectionalFeedTime;
        lastKnownApproachConfirmCount = bleBearing.approachConfirmCount;
        lastKnownLockGeneration = bleBearing.lockGeneration;
        lastKnownValid = true;
    }

    GeigerScanView::ThroughTarget target = {};
    target.header = headerBuf;
    target.scope = identityAmbiguous ? "BLE?" : "BLE";
    target.rssi = smoothed;
    target.proximity = (uint16_t)constrain(proxLevel, 0, 1000);
    target.bearing = bleBearing.bearing;
    target.confidence = identityAmbiguous ? 0u : bleBearing.lockConfidence;
    target.trend = bleBearing.rssiTrendSmooth;
    target.ageMs = targetAgeMs;
    target.dotCount = te->seenCount;
    target.locked = !identityAmbiguous && bleBearing.bearingLocked;
    target.behind = bleBearing.porkBehind;
    target.moving = bleBearing.isMoving;
    target.flat = Pedometer::isCachedFlat();
    target.motionScreenSign = (int8_t)(Config::getDisplayRotate180() ? -1 : 1);
    target.scanX = bleBearing.thruScanX;
    target.scanY = bleBearing.thruScanY;
    target.motionHeat = bleBearing.thruMotionHeat;
    target.stationaryConfidence = bleBearing.stationaryConfidence;
    const int bleTrendMagnitude = abs((int)bleBearing.rssiTrendSmooth);
    const uint8_t bleSceneRaw = (target.ageMs <= 3000u)
        ? (uint8_t)constrain((bleTrendMagnitude - 2) * 8, 0, 45)
        : 0u;
    const uint8_t sceneMotionConf = constrain(
        (int)bleBearing.stationaryConfidence +
            (bleBearing.isMoving ? 0 : bleBearingConfig.stationaryBoostWhileIdle),
        0, 100);
    target.sceneScanX = 0;
    target.sceneScanY = 0;
    target.sceneMotionHeat = (uint8_t)constrain(
        ((int)bleSceneRaw * (int)sceneMotionConf) / 100,
        0, 100);
    target.tracker = &bleBearing;
    target.fallbackBearing = 0;
    target.seekHeadingDegX10 = (uint16_t)GeigerScanMath::normX10(
        (int)(bleScanRefHeading * 10.0f + 0.5f));
    target.seekHeadingValid = bleScanRefValid;
    target.lastKnownHeadingDegX10 = lastKnownHeadingDegX10;
    target.lastKnownElevDegX10 = lastKnownElevDegX10;
    target.lastKnownProximity = lastKnownProximity;
    target.lastKnownObserverX = lastKnownObserverX;
    target.lastKnownObserverY = lastKnownObserverY;
    target.lastKnownAgeMs = lastKnownValid ? nowMs - lastKnownSeenMs : 0u;
    target.lastKnownValid = Config::getGhostMarkerEnabled() && lastKnownValid;
    // BLE tracker uses BLE RSSI/bearing only; CSI path is Wi-Fi-only.
    target.csiWaiting = false;
    target.csiValid = false;
    target.csiAgeMs = 0u;
    target.csiQuality = 0u;
    target.csiChannelChange = 0u;
    target.csiFrequencySpread = 0u;
    target.csiStability = 0u;
    target.csiFade = 0u;

    if (Geiger::getViewMode() == Geiger::VIEW_THRU) {
        GeigerScanView::drawThroughScanner(canvas, boxX, boxY, boxW, boxH,
                                           scanFg, scanBg, target);
        return;
    }

    GeigerScanView::drawRadarScanner(canvas, boxX, boxY, boxW, boxH,
                                     scanFg, scanBg, target);
}

// ==[ DEVICE DETAIL PANE ]== compact info panel (list view, no tracking)
static void drawDeviceDetail(M5Canvas& c) {
    uint16_t fg = Display::getColorFG();
    uint16_t bg = Display::getColorBG();

    // detail pane: always show selected/tracked device inspector
    c.fillRect(0, DETAIL_TOP, 320, DETAIL_HEIGHT, fg);
    c.setTextSize(1);
    c.setTextColor(bg);
    c.setTextDatum(textdatum_t::top_left);

    int16_t tIdx = -1;
    const Recon::TrackerEntry* te = getDetailTracker(tIdx);
    if (!te) {
        c.setTextDatum(textdatum_t::middle_center);
        char totals[40];
        snprintf(totals, sizeof(totals), "SESSION:%u  APPLE:%u",
                 DefensePipeline::snapshot().getTotalBLEDevicesSeen(), DefensePipeline::snapshot().getAppleContinuityCount());
        c.drawString(totals, 160, DETAIL_TOP + 15);
        const char* state = !DefensePipeline::snapshot().isBleAvailable() ? "RADIO: UNAVAILABLE" :
                            !DefensePipeline::snapshot().isBleInitialized() ? "RADIO: STARTING" :
                            DefensePipeline::snapshot().isChaffActive() ? "MODE: ACTIVE + CHAFF" :
                            DefensePipeline::snapshot().isActiveScanEnabled() ? "MODE: ACTIVE PROBE" :
                            "MODE: PASSIVE LISTEN";
        c.drawString(state, 160, DETAIL_TOP + 33);
        c.setTextDatum(textdatum_t::top_left);
        return;
    }

    // ==[ SIGNAL BAR ]== visual RSSI strength
    int8_t rssi = te->rssiSmooth;
    int barW = map(constrain(rssi, -90, -30), -90, -30, 2, 180);
    int16_t barY = DETAIL_TOP + 2;
    c.fillRect(2, barY, barW, 6, bg);

    // pulse effect synced to Geiger clicks
    bool pulse = (millis() / 150) % 2;
    if (pulse && Geiger::isActive()) {
        c.fillRect(2 + barW - 4, barY, 4, 6, fg);  // flicker tip
    }

    char macBuf[18];
    char ouiBuf[10];
    char ageBuf[8];
    char durBuf[8];
    char payload0[32];
    char payload1[32];
    formatMac(macBuf, sizeof(macBuf), te->mac);
    formatOui(ouiBuf, sizeof(ouiBuf), te->mac);
    formatAgeCompact(ageBuf, sizeof(ageBuf), millis() - te->lastSeen);
    formatAgeCompact(durBuf, sizeof(durBuf), millis() - te->firstSeen);
    formatPayloadHex(payload0, sizeof(payload0), te->payloadPreview, te->payloadPreviewLen, 0, 8);
    formatPayloadHex(payload1, sizeof(payload1), te->payloadPreview, te->payloadPreviewLen, 8, 8);

    uint32_t hash32 = 0;
    memcpy(&hash32, te->payloadHash, sizeof(hash32));

    const uint32_t page = (millis() / DETAIL_PAGE_MS) % DETAIL_PAGES;
    const char* trendStr = "--";
    if (tracking) {
        // use bearing OLS trend (stable) instead of Geiger delta (noisy)
        int8_t trendVal = bleBearing.rssiTrendSmooth;
        if (trendVal > 4)       trendStr = ">>>";
        else if (trendVal > 2)  trendStr = ">>";
        else if (trendVal > 0)  trendStr = ">";
        else if (trendVal < -4) trendStr = "<<<";
        else if (trendVal < -2) trendStr = "<<";
        else if (trendVal < 0)  trendStr = "<";
    }

    // distance estimate from TX power (log-distance model)
    char distBuf[10] = "";
    float distM = Recon::estimateDistance(rssi, te->txPower);
    if (distM >= 0) {
        if (distM < 10.0f) snprintf(distBuf, sizeof(distBuf), "~%.1fm", distM);
        else snprintf(distBuf, sizeof(distBuf), "~%dm", (int)distM);
    }

    char line1[48];
    snprintf(line1, sizeof(line1), "%s %s %s %ddB %s %s",
             tracking ? "LOCK" : "SEL",
             Recon::deviceLabel(*te),
             Recon::proximityLabel(rssi),
             rssi,
             distBuf,
             trendStr);
    c.drawString(line1, 2, DETAIL_TOP + 11);

    char line2[56];
    snprintf(line2, sizeof(line2), "Nm:%s",
             te->name[0] ? te->name : "<none>");

    char line3[56];
    snprintf(line3, sizeof(line3), "MAC:%s Age:%s D:%s", macBuf, ageBuf, durBuf);

    char line4[56];
    switch (page) {
        case 0:
            if (te->type == Recon::ThreatType::IBEACON) {
                snprintf(line4, sizeof(line4), "Co:%04X Mj:%u Mn:%u Tx:%d",
                         te->companyId, te->major, te->minor, te->txPower);
            } else {
                snprintf(line4, sizeof(line4), "Co:%04X S:%04X Tx:%d Fr:%s",
                         te->companyId, te->primaryService, te->txPower, frameLabel(*te));
            }
            break;
        case 1: {
            const char* appLbl = Recon::appearanceLabel(te->appearance);
            const char* codLbl = Recon::classOfDeviceLabel(te->classOfDevice);
            if (appLbl && codLbl)
                snprintf(line4, sizeof(line4), "AF:%02X Ty:%s App:%s CoD:%s",
                         te->advFlags, advTypeLabel(te->advType), appLbl, codLbl);
            else if (appLbl)
                snprintf(line4, sizeof(line4), "AF:%02X Ty:%s Ad:%s App:%s",
                         te->advFlags, advTypeLabel(te->advType),
                         addrTypeLabel(te->addrType), appLbl);
            else if (codLbl)
                snprintf(line4, sizeof(line4), "AF:%02X Ty:%s Ad:%s CoD:%s",
                         te->advFlags, advTypeLabel(te->advType),
                         addrTypeLabel(te->addrType), codLbl);
            else
                snprintf(line4, sizeof(line4), "AF:%02X Ty:%s Ad:%s Pay:%u",
                         te->advFlags, advTypeLabel(te->advType),
                         addrTypeLabel(te->addrType), te->payloadLen);
            break;
        }
        case 2: {
            // measured ad interval (anti-spoofing) > AD-advertised interval
            char intBuf[20] = "";
            if (te->measuredAdvIntervalMs > 0) {
                // spoof check: AirTag with <5s interval is suspicious
                bool spoofFlag = (te->type == Recon::ThreatType::AIRTAG &&
                                  te->measuredAdvIntervalMs < 5000);
                snprintf(intBuf, sizeof(intBuf), " ADV:%ums%s",
                         te->measuredAdvIntervalMs, spoofFlag ? "!" : "");
            } else if (te->advInterval) {
                uint32_t intMs = (uint32_t)te->advInterval * 625 / 1000;
                snprintf(intBuf, sizeof(intBuf), " I:%lums", (unsigned long)intMs);
            }
            // relay variance (only when relay suspect flagged)
            if ((te->flags & Recon::FLAG_RELAY_SUSPECT) && te->intervalVariance > 0) {
                char varBuf[12];
                snprintf(varBuf, sizeof(varBuf), " V:%u", te->intervalVariance);
                strncat(intBuf, varBuf, sizeof(intBuf) - strlen(intBuf) - 1);
            }
            const char* mfg2 = te->companyId2 ? Recon::manufacturerLabel(te->companyId2) : nullptr;
            if (mfg2 && mfg2[0] != '?')
                snprintf(line4, sizeof(line4), "Seen:%u Rot:%u +%s%s",
                         te->seenCount, te->macChangeCount, mfg2, intBuf);
            else
                snprintf(line4, sizeof(line4), "Seen:%u Rot:%u Svc:%u Mfg:%u%s",
                         te->seenCount, te->macChangeCount,
                         te->serviceCount, te->manufacturerCount, intBuf);
            break;
        }
        case 3: {
            // walking distance since first contact — step-stalk forensic
            uint32_t curDist = Pedometer::getDistance();
            uint32_t walkDelta = (curDist > te->firstDetectDist)
                                 ? (curDist - te->firstDetectDist) : 0;
            snprintf(line2, sizeof(line2), "Vn:%s OUI:%s",
                     vendorLabel(*te), ouiBuf);
            snprintf(line3, sizeof(line3), "Hash:%08lX walk:%lum",
                     (unsigned long)hash32, (unsigned long)walkDelta);
            snprintf(line4, sizeof(line4), "Flg:%s%s%s%s%s",
                     (te->type != Recon::ThreatType::UNKNOWN) ? "KNOWN " : "",
                     (te->flags & Recon::FLAG_FOLLOWING) ? "FOLLOW " : "",
                     (te->flags & Recon::FLAG_STEP_FOLLOWING) ? "STALK " : "",
                     (te->flags & Recon::FLAG_RELAY_SUSPECT) ? "RELAY " : "",
                     (te->flags & Recon::FLAG_SPAM) ? spamLabel(te->spamPlatform) : "");
            break;
        }
        case 4:
            snprintf(line2, sizeof(line2), "Vn:%s OUI:%s",
                     vendorLabel(*te), ouiBuf);
            snprintf(line3, sizeof(line3), "P0:%s", payload0);
            snprintf(line4, sizeof(line4), "P8:%s", payload1);
            break;
        case 5: {
            // XBAND correlation page
            const XBand::AttackerProfile* atks = DefensePipeline::snapshot().getAttackerProfiles();
            int atkCount = DefensePipeline::snapshot().getAttackerCount();
            const XBand::CohortPair* cohorts = DefensePipeline::snapshot().getCohortPairs();
            int cohortCount = DefensePipeline::snapshot().getCohortCount();
            bool foundAtk = false, foundCohort = false;
            for (int a = 0; a < atkCount && !foundAtk; a++) {
                if (memcmp(atks[a].blePayloadHash, te->payloadHash, 4) == 0) {
                    snprintf(line2, sizeof(line2), "XBAND: ATK SOURCE x%d",
                             atks[a].correlatedBursts);
                    snprintf(line3, sizeof(line3), "WiFi:%ddB BLE:%ddB D:%s",
                             atks[a].wifiDeauthRssi, atks[a].bleRssi,
                             atks[a].estimatedDist >= 0 ?
                                 (atks[a].estimatedDist < 10 ? "<10m" : ">10m") : "?");
                    snprintf(line4, sizeof(line4), "Co:%04X %s",
                             atks[a].companyId, atks[a].bleName);
                    foundAtk = true;
                }
            }
            if (!foundAtk) {
                for (int c2 = 0; c2 < cohortCount && !foundCohort; c2++) {
                    if (memcmp(cohorts[c2].blePayloadHash, te->payloadHash, 4) == 0) {
                        const char* conf = cohorts[c2].confidence >= 3 ? "HIGH" :
                                           cohorts[c2].confidence >= 2 ? "MED" : "LOW";
                        snprintf(line2, sizeof(line2), "COHORT: WiFi+BLE %s", conf);
                        snprintf(line3, sizeof(line3), "SSID:%.16s%s",
                                 cohorts[c2].probeSSID,
                                 cohorts[c2].potfileMatch ? " POT" : "");
                        snprintf(line4, sizeof(line4), "%s%s",
                                 cohorts[c2].isFollowing ? "FOLLOW " : "",
                                 DefensePipeline::snapshot().isDualBandStalkActive() ? "DUAL-BAND" : "");
                        foundCohort = true;
                    }
                }
            }
            if (!foundAtk && !foundCohort) {
                snprintf(line2, sizeof(line2), "XBAND: no correlation");
                snprintf(line3, sizeof(line3), "no WiFi-BLE match");
                line4[0] = '\0';
            }
            break;
        }
        default: {
            // GATT info page
            bool gattMatch = hasGattInfo &&
                memcmp(gattTargetHash, te->payloadHash, 4) == 0;
            if (gattMatch) {
                if (lastGattInfo.batteryLevel >= 0)
                    snprintf(line2, sizeof(line2), "GATT OK  Bat:%d%%",
                             lastGattInfo.batteryLevel);
                else
                    snprintf(line2, sizeof(line2), "GATT OK  Bat:--");
                snprintf(line3, sizeof(line3), "Mfg:%.20s",
                         lastGattInfo.manufacturer[0] ? lastGattInfo.manufacturer : "--");
                snprintf(line4, sizeof(line4), "Mod:%.12s FW:%.10s",
                         lastGattInfo.model[0] ? lastGattInfo.model : "--",
                         lastGattInfo.firmware[0] ? lastGattInfo.firmware : "--");
            } else {
                snprintf(line2, sizeof(line2), "GATT: no data");
                snprintf(line3, sizeof(line3), "B+ to interrogate");
                snprintf(line4, sizeof(line4), " ");
            }
            break;
        }
    }
    c.drawString(line2, 2, DETAIL_TOP + 21);
    c.drawString(line3, 2, DETAIL_TOP + 31);
    c.drawString(line4, 2, DETAIL_TOP + 41);

    static const char* pageTitle[] = {"RADIO","META","STATS","ID","PAY","XBAND","GATT"};
    char rightBuf[16];
    snprintf(rightBuf, sizeof(rightBuf), "%s %lu/%lu",
             pageTitle[page], (unsigned long)(page + 1), (unsigned long)DETAIL_PAGES);
    c.setTextDatum(textdatum_t::top_right);
    c.drawString(rightBuf, 318, DETAIL_TOP + 11);
    if (tracking) {
        c.drawString("GGR", 318, DETAIL_TOP + 21);
    }
    c.setTextDatum(textdatum_t::top_left);
}

void draw(M5Canvas& canvas) {
    // ==[ TRACKING MODE ]== full-screen bearing scanner takeover
    if (tracking) {
        drawSummaryBar(canvas);
        drawBearingScanner(canvas);

        // bottom bar handled by Display::drawBleScreen
        return;
    }

    // ==[ LIST MODE ]== device list + detail pane
    drawSummaryBar(canvas);

    // ==[ DEVICE LIST ]==
    uint16_t bg = Display::getColorBG();
    int16_t listH = VISIBLE_ROWS * ROW_HEIGHT;
    canvas.fillRect(0, LIST_TOP, 320, listH, bg);

    if (sortedCount == 0) {
        const char* headline = !DefensePipeline::snapshot().isBleAvailable() ? "BLE RADIO UNAVAILABLE" :
                               !DefensePipeline::snapshot().isBleInitialized() ? "BLE ENGINE STARTING..." :
                               DefensePipeline::snapshot().isScanning() ? "LISTENING FOR BLE DEVICES..." :
                               "NO BLE DEVICES HEARD";
        const char* cta = !DefensePipeline::snapshot().isBleAvailable() ? "FLASH A BLE BUILD. EARS STAY SHUT." :
                          !DefensePipeline::snapshot().isBleInitialized() ? "THE EARS ARE WAKING UP." :
                          DefensePipeline::snapshot().isChaffActive() ? "CHAFF IS ON. SWIPE RIGHT: PASSIVE." :
                          DefensePipeline::snapshot().isActiveScanEnabled() ? "ACTIVE PROBES. SWIPE RIGHT: CHAFF." :
                          "PASSIVE SWEEP. SWIPE RIGHT: ACTIVE.";
        canvas.setTextSize(1);
        canvas.setTextColor(Display::getColorFG());
        canvas.setTextDatum(textdatum_t::middle_center);
        canvas.drawString(headline, 160, LIST_TOP + listH / 2 - 8);
        canvas.drawString(cta, 160, LIST_TOP + listH / 2 + 8);
        canvas.setTextDatum(textdatum_t::top_left);
    } else {
        int endRow = min((int)(scrollOffset + VISIBLE_ROWS), (int)sortedCount);
        for (int i = scrollOffset; i < endRow; i++) {
            bool sel = (i == selectedIdx);
            drawDeviceRow(canvas, i - scrollOffset, i, sel);
        }

        // scroll indicators + position counter
        uint16_t fg = Display::getColorFG();
        if (scrollOffset > 0) {
            canvas.setTextDatum(textdatum_t::top_right);
            canvas.setTextColor(fg);
            canvas.drawString("^", 318, LIST_TOP);
        }
        if (scrollOffset + VISIBLE_ROWS < sortedCount) {
            canvas.setTextDatum(textdatum_t::top_right);
            canvas.setTextColor(fg);
            canvas.drawString("v", 318, LIST_TOP + listH - 10);
        }
        // position counter
        if (sortedCount > VISIBLE_ROWS) {
            char posBuf[8];
            snprintf(posBuf, sizeof(posBuf), "%d/%d", selectedIdx + 1, sortedCount);
            canvas.setTextDatum(textdatum_t::top_right);
            canvas.setTextColor(fg);
            canvas.drawString(posBuf, 316, LIST_TOP + listH / 2 - 4);
        }
        canvas.setTextDatum(textdatum_t::top_left);
    }

    // ==[ DEVICE DETAIL PANE ]==
    drawDeviceDetail(canvas);
}

}  // namespace BleScanner
