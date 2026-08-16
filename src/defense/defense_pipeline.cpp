/**
 * Defense pipeline — one custody chain from observation to published evidence.
 *
 * Producers retain bounded inputs. Acquisition and fusion work private tables.
 * Queue admission, forensic side effects, and immutable snapshot publication
 * happen in that order. Consumers get a generation-stamped statement, never a
 * key to the interview room.
 */
#define DEFENSE_PIPELINE_INTERNAL 1
#include "defense_pipeline.h"

#include <atomic>
#include <new>
#include <string.h>

#include "defense_event_queue.h"
#include "defense_acquisition.h"
#include "defense_fusion.h"
#include "defense_side_effects.h"
#include "recon_internal.h"
#include "xband.h"
#include "../radio/c5monster_uart.h"

namespace {

Defense::EventQueue<>* eventQueue = nullptr;
Defense::EventQueue<>* stagedEventQueue = nullptr;
Defense::DefenseEventData* admittedEvents = nullptr;
size_t admittedEventCount = 0;
bool collectingEvents = false;
C5Monster::ScanResults* pendingC5Scan = nullptr;
bool c5ScanPending = false;
Defense::DefenseSnapshot* snapshotBuffers[2] = {nullptr, nullptr};
std::atomic<const Defense::DefenseSnapshot*> publishedSnapshot{nullptr};
Defense::PublicationCursor publication;
Defense::OperatingState currentOperatingState = Defense::OperatingState::BACKGROUND;
bool initialized = false;

template <typename T>
static int boundedCount(int count, int capacity) {
    (void)sizeof(T);
    if (count < 0) return 0;
    return count > capacity ? capacity : count;
}

template <typename T>
static void copyRows(T* destination, const T* source, int count, int capacity) {
    const int safeCount = boundedCount<T>(count, capacity);
    if (source && safeCount > 0) memcpy(destination, source, safeCount * sizeof(T));
    if (safeCount < capacity) memset(destination + safeCount, 0, (capacity - safeCount) * sizeof(T));
}

}  // namespace

namespace Defense {

struct SnapshotWriter {
    static void capture(DefenseSnapshot& out, uint32_t nextGeneration,
                        uint32_t now, size_t queuedEvents) {
        out.generation_ = nextGeneration;
        out.publishedAtMs_ = now;
        out.active_ = Recon::isActive();
        out.scanning_ = Recon::isScanning();
        out.bleAvailable_ = Recon::isBleAvailable();
        out.bleInitialized_ = Recon::isBleInitialized();
        out.parasitic_ = Recon::isParasiticMode();
        out.activeScanEnabled_ = Recon::isActiveScanEnabled();
        out.chaffActive_ = Recon::isChaffActive();
        out.forensicExportEnabled_ = Recon::isForensicExportEnabled();
        out.wifiPipeline_ = Recon::getWifiPipelineStatus();
        out.cadenceTier_ = Recon::getCadenceTier();
        out.wifiScanIntervalMs_ = Recon::getCurrentWifiScanIntervalMs();
        out.deauthSniffMs_ = Recon::getCurrentDeauthSniffMs();
        out.parasiticIntervalMs_ = Recon::getCurrentParasiticIntervalMs();
        out.sentinelIntervalMs_ = Recon::getCurrentSentinelIntervalMs();

        out.trackerCount_ = boundedCount<Recon::TrackerEntry>(
            Recon::getTrackerTableSize(), Recon::MAX_TRACKERS);
        copyRows(out.trackers_, Recon::getTrackers(), out.trackerCount_, Recon::MAX_TRACKERS);
        out.bleDeviceCount_ = boundedCount<Recon::TrackerEntry>(
            Recon::getBleDeviceTableSize(), Recon::MAX_BLE_DEVICES);
        copyRows(out.bleDevices_, Recon::getBleDevices(), out.bleDeviceCount_, Recon::MAX_BLE_DEVICES);
        out.followingCount_ = Recon::getFollowingCount();
        out.spamCount_ = Recon::getSpamCount();
        out.hasThreats_ = Recon::hasThreats();

        out.wifiCount_ = boundedCount<Recon::WifiAP>(
            Recon::getWifiSnapshotCount(), Recon::MAX_WIFI_SNAPSHOT);
        copyRows(out.wifi_, Recon::getWifiSnapshot(), out.wifiCount_, Recon::MAX_WIFI_SNAPSHOT);
        out.openAPCount_ = Recon::getOpenAPCount();
        out.knownAPCount_ = Recon::getKnownAPCount();
        out.knownProbeRequestCount_ = Recon::getKnownProbeRequestCount();
        out.knownProbeClientCount_ = Recon::getKnownProbeClientCount();

        out.lastBleScanMs_ = Recon::getLastBLEScanTime();
        out.lastWifiScanMs_ = Recon::getLastWifiScanTime();
        out.timeSinceLastScanMs_ = Recon::getTimeSinceLastScan();
        out.totalBleDevicesSeen_ = Recon::getTotalBLEDevicesSeen();
        out.appleContinuityCount_ = Recon::getAppleContinuityCount();
        out.bleScanIntervalMs_ = Recon::getCurrentBLEScanIntervalMs();
        out.bleScanDurationMs_ = Recon::getCurrentBLEScanDurationMs();

        out.deauthCount_ = Recon::getDeauthCount();
        out.lastDeauthBurstCount_ = Recon::getLastDeauthBurstCount();
        out.lastDeauthUniqueSources_ = Recon::getLastDeauthUniqueSources();
        out.lastDeauthRssi_ = Recon::getLastDeauthRSSI();
        out.lastDeauthChannel_ = Recon::getLastDeauthChannel();
        out.lastDeauthDominantChannel_ = Recon::getLastDeauthDominantChannel();
        out.lastDeauthPps_ = Recon::getLastDeauthPPS();
        out.lastDeauthSubtypeCount_ = Recon::getLastDeauthSubtypeCount();
        out.lastDisassocSubtypeCount_ = Recon::getLastDisassocSubtypeCount();
        out.lastDeauthTimeMs_ = Recon::getLastDeauthTime();
        out.lastDeauthTool_ = Recon::getLastDeauthTool();
        out.deauthActive_ = Recon::isDeauthActive();
        out.evilTwinActive_ = Recon::isEvilTwinActive();
        out.karmaActive_ = Recon::isKarmaActive();
        out.fingerprintMismatchActive_ = Recon::isFingerprintMismatchActive();
        out.seqAnomalyActive_ = Recon::isSeqAnomalyActive();
        out.rssiAnomalyActive_ = Recon::isRssiAnomalyActive();
        out.fingerprintMismatchCount_ = Recon::getRecentFingerprintMismatchCount();
        out.seqAnomalyCount_ = Recon::getRecentSeqAnomalyCount();
        out.rssiAnomalyCount_ = Recon::getRecentRssiAnomalyCount();

        out.forensicCount_ = Recon::getForensicLogCount();
        out.forensicHead_ = Recon::getForensicLogHead();
        copyRows(out.forensic_, Recon::getForensicLog(), Recon::MAX_FORENSIC_LOG,
                 Recon::MAX_FORENSIC_LOG);
        out.deauthHistoryCount_ = Recon::getDeauthBurstHistoryCount();
        copyRows(out.deauthHistory_, Recon::getDeauthBurstHistory(), Recon::MAX_DEAUTH_HISTORY,
                 Recon::MAX_DEAUTH_HISTORY);
        out.probeVulnCount_ = Recon::getVulnProbeCount();
        copyRows(out.probeVuln_, Recon::getVulnProbeCache(), out.probeVulnCount_,
                 Recon::MAX_PROBE_VULN_CACHE);
        out.clientFingerprintCount_ = Recon::getClientFingerprintCount();
        copyRows(out.clientFingerprints_, Recon::getClientFingerprints(),
                 out.clientFingerprintCount_, Recon::MAX_CLIENT_FINGERPRINTS);
        out.offlineScanCount_ = Recon::getOfflineScanCount();
        const char* canary = Recon::getCanarySSID();
        strncpy(out.canarySsid_, canary ? canary : "", sizeof(out.canarySsid_) - 1);
        out.canarySsid_[sizeof(out.canarySsid_) - 1] = '\0';
        out.canaryTripped_ = Recon::isCanaryTripped();
        out.karmaConfirmed_ = Recon::isKarmaConfirmed();
        out.watchlistCount_ = Recon::getWatchlistCount();
        copyRows(out.watchlist_, Recon::getWatchlist(), Recon::MAX_WATCHLIST,
                 Recon::MAX_WATCHLIST);

        out.attackerCount_ = boundedCount<XBand::AttackerProfile>(
            XBand::getAttackerCount(), XBand::MAX_ATTACKER_PROFILES);
        copyRows(out.attackers_, XBand::getAttackerProfiles(), out.attackerCount_,
                 XBand::MAX_ATTACKER_PROFILES);
        out.activeAttacker_ = XBand::hasActiveAttacker();
        out.dualBandStalk_ = XBand::isDualBandStalkActive();
        out.persistentClientCount_ = XBand::getPersistentClientCount();
        out.cohortCount_ = boundedCount<XBand::CohortPair>(
            XBand::getCohortCount(), XBand::MAX_COHORT_PAIRS);
        copyRows(out.cohorts_, XBand::getCohortPairs(), out.cohortCount_,
                 XBand::MAX_COHORT_PAIRS);
        out.highConfidenceCohortCount_ = XBand::getHighConfidenceCohortCount();
        const XBand::CrowdSnapshot* crowd = XBand::getCurrentCrowd();
        out.hasCrowd_ = crowd != nullptr;
        if (crowd) out.crowd_ = *crowd;
        else memset(&out.crowd_, 0, sizeof(out.crowd_));
        out.crowdTrend_ = XBand::getCrowdTrend();
        out.crowdTier_ = XBand::getCrowdTier();
        out.estimatedPopulation_ = XBand::getEstimatedPopulation();
        out.sessionPeakPopulation_ = XBand::getSessionPeakPop();
        out.sessionMinPopulation_ = XBand::getSessionMinPop();
        out.vendorCorrelationCount_ = boundedCount<XBand::VendorCorrelation>(
            XBand::getVendorCorrelationCount(), XBand::MAX_VENDOR_CORRELATIONS);
        copyRows(out.vendorCorrelations_, XBand::getVendorCorrelations(),
                 out.vendorCorrelationCount_, XBand::MAX_VENDOR_CORRELATIONS);
        out.activeIntel_ = XBand::hasActiveIntel();
        out.criticalIntel_ = XBand::hasCriticalIntel();
        out.queuedEventCount_ = queuedEvents;
    }
};

}  // namespace Defense

namespace DefensePipeline {

static void publish(uint32_t now) {
    if (!snapshotBuffers[0] || !snapshotBuffers[1]) return;
    const uint8_t targetBuffer = publication.writeBuffer;
    const uint32_t generation = publication.advance();
    Defense::DefenseSnapshot& next = *snapshotBuffers[targetBuffer];
    Defense::SnapshotWriter::capture(next, generation, now,
                                     eventQueue ? eventQueue->size() : 0);
    publishedSnapshot.store(&next, std::memory_order_release);
}

void init() {
    if (initialized) return;
    void* queueMemory = Recon::psramAlloc(sizeof(Defense::EventQueue<>), "defEventQ");
    if (queueMemory) eventQueue = new (queueMemory) Defense::EventQueue<>();
    void* stagedQueueMemory = Recon::psramAlloc(
        sizeof(Defense::EventQueue<>), "defStageQ");
    if (stagedQueueMemory) {
        stagedEventQueue = new (stagedQueueMemory) Defense::EventQueue<>();
    }
    admittedEvents = (Defense::DefenseEventData*)Recon::psramAlloc(
        Defense::MAX_EVENT_ADMISSIONS_PER_BATCH * sizeof(Defense::DefenseEventData),
        "defAdmitted");
    pendingC5Scan = (C5Monster::ScanResults*)Recon::psramAlloc(
        sizeof(C5Monster::ScanResults), "defC5Input");
    Recon::init();
    XBand::init();
    if (eventQueue) eventQueue->clear();
    for (int i = 0; i < 2; ++i) {
        void* memory = Recon::psramAlloc(sizeof(Defense::DefenseSnapshot),
                                         i == 0 ? "defSnapA" : "defSnapB");
        if (memory) snapshotBuffers[i] = new (memory) Defense::DefenseSnapshot();
    }
    initialized = true;
    publish(millis());
}

void update(uint32_t now) {
    if (!initialized) return;
    admittedEventCount = 0;
    collectingEvents = eventQueue && stagedEventQueue && admittedEvents;
    if (collectingEvents) *stagedEventQueue = *eventQueue;
    // The C5 producer publishes before Hamlet reaches this update. Fuse its
    // retained input first to preserve the former cross-source event order.
    if (c5ScanPending && pendingC5Scan) {
        DefenseFusion::fuseC5Monster(*pendingC5Scan);
        c5ScanPending = false;
    }
    const bool ranAcquisition = DefenseAcquisition::update(now);
    if (ranAcquisition) {
        DefenseFusion::fuseAcquired(now);
        DefenseAcquisition::finalize(now);
        DefenseFusion::correlate(now);
    }
    collectingEvents = false;

    // Commit the staged queue only after fusion. The admission journal records
    // each successful ordered push, including an event later displaced by a
    // higher-priority event, matching the queue's established semantics.
    if (eventQueue && stagedEventQueue && admittedEvents) {
        *eventQueue = *stagedEventQueue;
        for (size_t i = 0; i < admittedEventCount; ++i) {
            Recon::recordForensicEvent(admittedEvents[i]);
        }
    }
    admittedEventCount = 0;

    if (ranAcquisition) DefenseSideEffects::update(now);
    publish(millis());
}

void requestOperatingState(Defense::OperatingState requested) {
    if (!initialized || requested == currentOperatingState) return;
    const uint16_t actions = Defense::lifecycleActions(currentOperatingState, requested);
    if (actions & Defense::ACTION_UNFORCE_BLE_PRIORITY) Recon::unforceBlePriority();
    if (actions & Defense::ACTION_UNFORCE_WARDRIVE) Recon::unforceWardriveBle();
    if (actions & Defense::ACTION_RESUME) Recon::resume();
    if (actions & Defense::ACTION_ENTER_PARASITIC) Recon::enterParasitic();
    if (actions & Defense::ACTION_SUSPEND_KEEP_BLE) Recon::suspend(false);
    if (actions & Defense::ACTION_SUSPEND_RELEASE_BLE) Recon::suspend(true);
    if (actions & Defense::ACTION_FORCE_BLE_PRIORITY) Recon::forceBlePriority();
    if (actions & Defense::ACTION_FORCE_WARDRIVE) Recon::forceWardriveBle();
    currentOperatingState = requested;
    publish(millis());
}

Defense::OperatingState operatingState() { return currentOperatingState; }

const Defense::DefenseSnapshot& snapshot() {
    const Defense::DefenseSnapshot* current =
        publishedSnapshot.load(std::memory_order_acquire);
    if (current) return *current;
    static const Defense::DefenseSnapshot empty;
    return empty;
}

bool emitEvent(const Defense::DefenseEventData& event) {
    if (collectingEvents) {
        if (!stagedEventQueue || !admittedEvents ||
            admittedEventCount >= Defense::MAX_EVENT_ADMISSIONS_PER_BATCH) {
            return false;
        }
        if (!stagedEventQueue->push(event)) return false;
        admittedEvents[admittedEventCount++] = event;
        return true;
    }
    if (!eventQueue || !eventQueue->push(event)) return false;
    Recon::recordForensicEvent(event);
    return true;
}

bool hasEvent() { return eventQueue && !eventQueue->empty(); }

Defense::DefenseEventData popEvent() {
    Defense::DefenseEventData event = {};
    if (eventQueue) eventQueue->pop(event);
    return event;
}

void feedC5MonsterScan(const C5Monster::ScanResults& results) {
    if (!pendingC5Scan) return;
    *pendingC5Scan = results;
    c5ScanPending = true;
}

void ingestWardriveSnapshot(const wifi_ap_record_t* records, uint16_t count) {
    Recon::ingestWardriveSnapshot(records, count);
}

void ingestDeauthObservation(uint8_t channel, int8_t rssi, uint8_t subtype,
                             const uint8_t* sourceMac,
                             Defense::DeauthSourceOrigin origin,
                             const uint8_t* targetBssid, uint16_t reasonCode) {
    Recon::ingestDeauthObservation(channel, rssi, subtype, sourceMac, origin,
                                   targetBssid, reasonCode);
}

void cacheProbeVulnMatch(const uint8_t* clientMac, const char* ssid, int8_t rssi) {
    Recon::cacheProbeVulnMatch(clientMac, ssid, rssi);
}

void reportKarmaFromProbeResponse(const char* ssid, const char* detail) {
    Recon::reportKarmaFromProbeResponse(ssid, detail);
}

void clearOfflineScanCount() { Recon::clearOfflineScanCount(); }
bool requestWifiScan() { return Recon::requestWifiScan(); }
void setForcedCadence(Defense::CadenceTier tier) { Recon::setForcedCadence(tier); }
void clearForcedCadence() { Recon::clearForcedCadence(); }
void pinBleDevice(const uint8_t* payloadHash) { Recon::pinBleDevice(payloadHash); }
void clearPinnedBleDevice() { Recon::clearPinnedBleDevice(); }
void setActiveScan(bool active) { Recon::setActiveScan(active); }
void toggleChaff() { Recon::toggleChaff(); }
void pauseBLEScanForGATT() { Recon::pauseBLEScanForGATT(); }
void resumeBLEScanFromGATT() { Recon::resumeBLEScanFromGATT(); }
bool addToWatchlist(const uint8_t* payloadHash, const char* label) {
    return Recon::addToWatchlist(payloadHash, label);
}
bool removeFromWatchlist(uint8_t slot) { return Recon::removeFromWatchlist(slot); }
void updateWatchlistLabel(uint8_t slot, const char* label) {
    Recon::updateWatchlistLabel(slot, label);
}
void setCanarySSID(const char* ssid) { Recon::setCanarySSID(ssid); }
void setForensicExportEnabled(bool enabled) { Recon::setForensicExportEnabled(enabled); }
void onWardriveSweepComplete() { Recon::onWardriveSweepComplete(); }
bool wardriveWantsBleWindow() { return Recon::wardriveWantsBleWindow(); }
bool consumeWardriveBleReady() { return Recon::consumeWardriveBleReady(); }

}  // namespace DefensePipeline
