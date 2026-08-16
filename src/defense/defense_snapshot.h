/** Immutable, generation-stamped defense state published to consumers. */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "recon.h"
#include "xband.h"

namespace Defense {

template <typename T>
class BoundedView {
public:
    BoundedView() = default;
    BoundedView(const T* rows, size_t count, size_t capacity)
        : rows_(rows), count_(count), capacity_(capacity) {}

    const T* data() const { return rows_; }
    size_t size() const { return count_; }
    size_t capacity() const { return capacity_; }
    bool empty() const { return count_ == 0; }
    const T& operator[](size_t index) const { return rows_[index]; }
    const T* begin() const { return rows_; }
    const T* end() const { return rows_ + count_; }

private:
    const T* rows_ = nullptr;
    size_t count_ = 0;
    size_t capacity_ = 0;
};

struct SnapshotWriter;

class DefenseSnapshot {
public:
    DefenseSnapshot() = default;

    uint32_t generation() const { return generation_; }
    uint32_t publishedAtMs() const { return publishedAtMs_; }

    bool isActive() const { return active_; }
    bool isScanning() const { return scanning_; }
    bool isBleAvailable() const { return bleAvailable_; }
    bool isBleInitialized() const { return bleInitialized_; }
    bool isParasiticMode() const { return parasitic_; }
    bool isActiveScanEnabled() const { return activeScanEnabled_; }
    bool isChaffActive() const { return chaffActive_; }
    bool isForensicExportEnabled() const { return forensicExportEnabled_; }

    Recon::WifiPipelineStatus getWifiPipelineStatus() const { return wifiPipeline_; }
    Recon::CadenceTier getCadenceTier() const { return cadenceTier_; }
    uint32_t getCurrentWifiScanIntervalMs() const { return wifiScanIntervalMs_; }
    uint32_t getCurrentDeauthSniffMs() const { return deauthSniffMs_; }
    uint32_t getCurrentParasiticIntervalMs() const { return parasiticIntervalMs_; }
    uint32_t getCurrentSentinelIntervalMs() const { return sentinelIntervalMs_; }

    int getTrackerCount() const { return trackerCount_; }
    int getFollowingCount() const { return followingCount_; }
    int getSpamCount() const { return spamCount_; }
    bool hasThreats() const { return hasThreats_; }
    const Recon::TrackerEntry* getTrackers() const { return trackers_; }
    int getTrackerTableSize() const { return trackerCount_; }
    const Recon::TrackerEntry* getBleDevices() const { return bleDevices_; }
    int getBleDeviceTableSize() const { return bleDeviceCount_; }
    BoundedView<Recon::TrackerEntry> trackers() const {
        return {trackers_, (size_t)trackerCount_, Recon::MAX_TRACKERS};
    }
    BoundedView<Recon::TrackerEntry> bleDevices() const {
        return {bleDevices_, (size_t)bleDeviceCount_, Recon::MAX_BLE_DEVICES};
    }

    int getLastWifiAPCount() const { return wifiCount_; }
    int getOpenAPCount() const { return openAPCount_; }
    int getKnownAPCount() const { return knownAPCount_; }
    uint16_t getKnownProbeRequestCount() const { return knownProbeRequestCount_; }
    uint16_t getKnownProbeClientCount() const { return knownProbeClientCount_; }
    const Recon::WifiAP* getWifiSnapshot() const { return wifi_; }
    int getWifiSnapshotCount() const { return wifiCount_; }
    BoundedView<Recon::WifiAP> wifi() const {
        return {wifi_, (size_t)wifiCount_, Recon::MAX_WIFI_SNAPSHOT};
    }

    uint32_t getLastBLEScanTime() const { return lastBleScanMs_; }
    uint32_t getLastWifiScanTime() const { return lastWifiScanMs_; }
    uint32_t getTimeSinceLastScan() const { return timeSinceLastScanMs_; }
    uint16_t getTotalBLEDevicesSeen() const { return totalBleDevicesSeen_; }
    uint16_t getAppleContinuityCount() const { return appleContinuityCount_; }
    uint32_t getCurrentBLEScanIntervalMs() const { return bleScanIntervalMs_; }
    uint32_t getCurrentBLEScanDurationMs() const { return bleScanDurationMs_; }

    uint32_t getDeauthCount() const { return deauthCount_; }
    uint16_t getLastDeauthBurstCount() const { return lastDeauthBurstCount_; }
    uint8_t getLastDeauthUniqueSources() const { return lastDeauthUniqueSources_; }
    int8_t getLastDeauthRSSI() const { return lastDeauthRssi_; }
    uint8_t getLastDeauthChannel() const { return lastDeauthChannel_; }
    uint8_t getLastDeauthDominantChannel() const { return lastDeauthDominantChannel_; }
    uint16_t getLastDeauthPPS() const { return lastDeauthPps_; }
    uint16_t getLastDeauthSubtypeCount() const { return lastDeauthSubtypeCount_; }
    uint16_t getLastDisassocSubtypeCount() const { return lastDisassocSubtypeCount_; }
    uint32_t getLastDeauthTime() const { return lastDeauthTimeMs_; }
    Recon::DeauthTool getLastDeauthTool() const { return lastDeauthTool_; }
    bool isDeauthActive() const { return deauthActive_; }

    bool isEvilTwinActive() const { return evilTwinActive_; }
    bool isKarmaActive() const { return karmaActive_; }
    bool isFingerprintMismatchActive() const { return fingerprintMismatchActive_; }
    bool isSeqAnomalyActive() const { return seqAnomalyActive_; }
    bool isRssiAnomalyActive() const { return rssiAnomalyActive_; }
    uint8_t getRecentFingerprintMismatchCount() const { return fingerprintMismatchCount_; }
    uint8_t getRecentSeqAnomalyCount() const { return seqAnomalyCount_; }
    uint8_t getRecentRssiAnomalyCount() const { return rssiAnomalyCount_; }

    uint8_t getForensicLogCount() const { return forensicCount_; }
    uint8_t getForensicLogHead() const { return forensicHead_; }
    const Recon::ForensicLogEntry* getForensicLog() const { return forensic_; }
    uint8_t countIndicatorsForBSSID(const uint8_t* bssid, uint32_t windowMs) const {
        if (!bssid || forensicCount_ == 0) return 0;
        uint8_t flags = 0;
        for (uint8_t i = 0; i < forensicCount_; ++i) {
            const int index = ((int)forensicHead_ - 1 - i + Recon::MAX_FORENSIC_LOG) %
                              Recon::MAX_FORENSIC_LOG;
            const Recon::ForensicLogEntry& entry = forensic_[index];
            if (entry.timestamp == 0 ||
                static_cast<uint32_t>(publishedAtMs_ - entry.timestamp) > windowMs) continue;
            if (memcmp(entry.bssid, bssid, 6) == 0) flags |= entry.indicatorFlags;
        }
        uint8_t count = 0;
        for (; flags; flags >>= 1) count += flags & 1;
        return count;
    }
    uint8_t getDeauthBurstHistoryCount() const { return deauthHistoryCount_; }
    const Recon::DeauthBurstRecord* getDeauthBurstHistory() const { return deauthHistory_; }
    uint8_t getVulnProbeCount() const { return probeVulnCount_; }
    const Recon::ProbeVulnMatch* getVulnProbeCache() const { return probeVuln_; }
    uint8_t getClientFingerprintCount() const { return clientFingerprintCount_; }
    const Recon::ClientFingerprint* getClientFingerprints() const { return clientFingerprints_; }
    void getTemporalHeatmap(Recon::HeatmapBucket* out, uint8_t count,
                            uint32_t windowMs) const {
        if (!out || count == 0) return;
        memset(out, 0, count * sizeof(Recon::HeatmapBucket));

        auto bump = [](uint8_t& value, uint8_t delta) {
            const uint16_t sum = (uint16_t)value + delta;
            value = sum > 255 ? 255 : (uint8_t)sum;
        };

        // Rebuild the caller's requested bucket geometry from the immutable
        // forensic ring. Copying a pre-bucketed 30-row heatmap made 10-row
        // callers read only the oldest third of the five-minute window.
        for (uint8_t i = 0; i < forensicCount_; ++i) {
            const int index = ((int)forensicHead_ - 1 - i + Recon::MAX_FORENSIC_LOG) %
                              Recon::MAX_FORENSIC_LOG;
            const Recon::ForensicLogEntry& entry = forensic_[index];
            const size_t bucket = temporalBucketIndex(
                publishedAtMs_, entry.timestamp, count, windowMs);
            if (bucket >= count) continue;
            Recon::HeatmapBucket& heat = out[bucket];
            heat.indicatorFlags |= entry.indicatorFlags;

            switch (entry.event) {
                case Recon::ReconEvent::DEAUTH_DETECTED:
                    bump(heat.deauthIntensity, 30);
                    break;
                case Recon::ReconEvent::EVIL_TWIN:
                case Recon::ReconEvent::KARMA_HONEYPOT:
                case Recon::ReconEvent::KARMA_CONFIRMED:
                case Recon::ReconEvent::CANARY_TRIPPED:
                case Recon::ReconEvent::FINGERPRINT_MISMATCH:
                case Recon::ReconEvent::SEQ_ANOMALY:
                case Recon::ReconEvent::RSSI_ANOMALY:
                case Recon::ReconEvent::LOW_ENTROPY_BEACON:
                    bump(heat.wifiIntensity, 25);
                    break;
                case Recon::ReconEvent::TRACKER_FOLLOWING:
                case Recon::ReconEvent::COORDINATED_ATTACK:
                case Recon::ReconEvent::ATTACKER_IDENTIFIED:
                case Recon::ReconEvent::DUAL_BAND_STALK:
                case Recon::ReconEvent::FOLLOWING_NETWORK_ID:
                case Recon::ReconEvent::RELAY_SUSPECT:
                    bump(heat.bleIntensity, 40);
                    break;
                case Recon::ReconEvent::PROBE_VULN_CLIENT:
                case Recon::ReconEvent::HOSTILE_CLIENT:
                    bump(heat.wifiIntensity, 15);
                    break;
                case Recon::ReconEvent::TOOL_IDENTIFIED:
                    bump(heat.deauthIntensity, 20);
                    break;
                default:
                    break;
            }
        }
    }

    uint8_t getOfflineScanCount() const { return offlineScanCount_; }
    const char* getCanarySSID() const { return canarySsid_; }
    bool isCanaryTripped() const { return canaryTripped_; }
    bool isKarmaConfirmed() const { return karmaConfirmed_; }
    uint8_t getWatchlistCount() const { return watchlistCount_; }
    const Recon::WatchlistEntry* getWatchlist() const { return watchlist_; }
    bool isWatchlisted(const uint8_t* payloadHash) const {
        if (!payloadHash) return false;
        for (size_t i = 0; i < Recon::MAX_WATCHLIST; ++i) {
            if (watchlist_[i].occupied &&
                memcmp(watchlist_[i].payloadHash, payloadHash, 4) == 0) return true;
        }
        return false;
    }
    int findWatchlistSlot(const uint8_t* payloadHash) const {
        if (!payloadHash) return -1;
        for (size_t i = 0; i < Recon::MAX_WATCHLIST; ++i) {
            if (watchlist_[i].occupied &&
                memcmp(watchlist_[i].payloadHash, payloadHash, 4) == 0) return (int)i;
        }
        return -1;
    }

    int getAttackerCount() const { return attackerCount_; }
    const XBand::AttackerProfile* getAttackerProfiles() const { return attackers_; }
    bool hasActiveAttacker() const { return activeAttacker_; }
    bool isDualBandStalkActive() const { return dualBandStalk_; }
    int getPersistentClientCount() const { return persistentClientCount_; }
    int getCohortCount() const { return cohortCount_; }
    const XBand::CohortPair* getCohortPairs() const { return cohorts_; }
    int getHighConfidenceCohortCount() const { return highConfidenceCohortCount_; }
    const XBand::CrowdSnapshot* getCurrentCrowd() const {
        return hasCrowd_ ? &crowd_ : nullptr;
    }
    XBand::CrowdTrend getCrowdTrend() const { return crowdTrend_; }
    XBand::CrowdTier getCrowdTier() const { return crowdTier_; }
    uint16_t getEstimatedPopulation() const { return estimatedPopulation_; }
    bool isDeserted() const { return estimatedPopulation_ < 5; }
    bool isCrowded() const { return estimatedPopulation_ >= 40; }
    uint16_t getSessionPeakPop() const { return sessionPeakPopulation_; }
    uint16_t getSessionMinPop() const { return sessionMinPopulation_; }
    int getVendorCorrelationCount() const { return vendorCorrelationCount_; }
    const XBand::VendorCorrelation* getVendorCorrelations() const { return vendorCorrelations_; }
    bool hasActiveIntel() const { return activeIntel_; }
    bool hasCriticalIntel() const { return criticalIntel_; }

    size_t queuedEventCount() const { return queuedEventCount_; }

private:
    friend struct SnapshotWriter;

    uint32_t generation_ = 0;
    uint32_t publishedAtMs_ = 0;
    bool active_ = false;
    bool scanning_ = false;
    bool bleAvailable_ = false;
    bool bleInitialized_ = false;
    bool parasitic_ = false;
    bool activeScanEnabled_ = false;
    bool chaffActive_ = false;
    bool forensicExportEnabled_ = false;
    Recon::WifiPipelineStatus wifiPipeline_ = {};
    Recon::CadenceTier cadenceTier_ = Recon::CadenceTier::NORMAL;
    uint32_t wifiScanIntervalMs_ = 0;
    uint32_t deauthSniffMs_ = 0;
    uint32_t parasiticIntervalMs_ = 0;
    uint32_t sentinelIntervalMs_ = 0;

    Recon::TrackerEntry trackers_[Recon::MAX_TRACKERS] = {};
    Recon::TrackerEntry bleDevices_[Recon::MAX_BLE_DEVICES] = {};
    int trackerCount_ = 0;
    int bleDeviceCount_ = 0;
    int followingCount_ = 0;
    int spamCount_ = 0;
    bool hasThreats_ = false;

    Recon::WifiAP wifi_[Recon::MAX_WIFI_SNAPSHOT] = {};
    int wifiCount_ = 0;
    int openAPCount_ = 0;
    int knownAPCount_ = 0;
    uint16_t knownProbeRequestCount_ = 0;
    uint16_t knownProbeClientCount_ = 0;

    uint32_t lastBleScanMs_ = 0;
    uint32_t lastWifiScanMs_ = 0;
    uint32_t timeSinceLastScanMs_ = 0;
    uint16_t totalBleDevicesSeen_ = 0;
    uint16_t appleContinuityCount_ = 0;
    uint32_t bleScanIntervalMs_ = 0;
    uint32_t bleScanDurationMs_ = 0;

    uint32_t deauthCount_ = 0;
    uint16_t lastDeauthBurstCount_ = 0;
    uint8_t lastDeauthUniqueSources_ = 0;
    int8_t lastDeauthRssi_ = -127;
    uint8_t lastDeauthChannel_ = 0;
    uint8_t lastDeauthDominantChannel_ = 0;
    uint16_t lastDeauthPps_ = 0;
    uint16_t lastDeauthSubtypeCount_ = 0;
    uint16_t lastDisassocSubtypeCount_ = 0;
    uint32_t lastDeauthTimeMs_ = 0;
    Recon::DeauthTool lastDeauthTool_ = Recon::DeauthTool::UNKNOWN;
    bool deauthActive_ = false;
    bool evilTwinActive_ = false;
    bool karmaActive_ = false;
    bool fingerprintMismatchActive_ = false;
    bool seqAnomalyActive_ = false;
    bool rssiAnomalyActive_ = false;
    uint8_t fingerprintMismatchCount_ = 0;
    uint8_t seqAnomalyCount_ = 0;
    uint8_t rssiAnomalyCount_ = 0;

    Recon::ForensicLogEntry forensic_[Recon::MAX_FORENSIC_LOG] = {};
    uint8_t forensicCount_ = 0;
    uint8_t forensicHead_ = 0;
    Recon::DeauthBurstRecord deauthHistory_[Recon::MAX_DEAUTH_HISTORY] = {};
    uint8_t deauthHistoryCount_ = 0;
    Recon::ProbeVulnMatch probeVuln_[Recon::MAX_PROBE_VULN_CACHE] = {};
    uint8_t probeVulnCount_ = 0;
    Recon::ClientFingerprint clientFingerprints_[Recon::MAX_CLIENT_FINGERPRINTS] = {};
    uint8_t clientFingerprintCount_ = 0;
    uint8_t offlineScanCount_ = 0;
    char canarySsid_[33] = {};
    bool canaryTripped_ = false;
    bool karmaConfirmed_ = false;
    Recon::WatchlistEntry watchlist_[Recon::MAX_WATCHLIST] = {};
    uint8_t watchlistCount_ = 0;

    XBand::AttackerProfile attackers_[XBand::MAX_ATTACKER_PROFILES] = {};
    int attackerCount_ = 0;
    bool activeAttacker_ = false;
    bool dualBandStalk_ = false;
    int persistentClientCount_ = 0;
    XBand::CohortPair cohorts_[XBand::MAX_COHORT_PAIRS] = {};
    int cohortCount_ = 0;
    int highConfidenceCohortCount_ = 0;
    XBand::CrowdSnapshot crowd_ = {};
    bool hasCrowd_ = false;
    XBand::CrowdTrend crowdTrend_ = XBand::CrowdTrend::STABLE;
    XBand::CrowdTier crowdTier_ = XBand::CrowdTier::DESERTED;
    uint16_t estimatedPopulation_ = 0;
    uint16_t sessionPeakPopulation_ = 0;
    uint16_t sessionMinPopulation_ = 0;
    XBand::VendorCorrelation vendorCorrelations_[XBand::MAX_VENDOR_CORRELATIONS] = {};
    int vendorCorrelationCount_ = 0;
    bool activeIntel_ = false;
    bool criticalIntel_ = false;
    size_t queuedEventCount_ = 0;
};

}  // namespace Defense
