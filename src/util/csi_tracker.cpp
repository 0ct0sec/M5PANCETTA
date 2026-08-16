/**
 * CSI Tracker - light, callback-safe Wi-Fi CSI sampler.
 *
 * ==[ LOW-RISK PATCH ]== callback captures only small payload and indexes;
 * task loop drains, extracts features, then exposes a compact snapshot.
 */

#include "csi_tracker.h"
#include "csi_features.h"
#include "csi_tracker_math.h"
#include <Arduino.h>
#include <cstring>
#include <esp_wifi.h>
#include <esp_wifi_types.h>
#include "debug_log.h"

namespace {

#if defined(HAMLET_CORE3SE)
static constexpr uint8_t kQueueDepth = 16;  // power of two for cheap wrap
static constexpr uint16_t kMaxIqBytes = 612;
static constexpr uint8_t kMaxSamplesPerUpdate = 12;
#else
static constexpr uint8_t kQueueDepth = 8;
static constexpr uint16_t kMaxIqBytes = 128;
static constexpr uint8_t kMaxSamplesPerUpdate = 6;
#endif
static constexpr uint8_t kQueueMask = kQueueDepth - 1;
static constexpr uint8_t kFeatureSamples = 2; // i/q bytes per subcarrier sample
static constexpr uint16_t kMaxFeaturePairs =
    kMaxIqBytes / kFeatureSamples;
static constexpr int8_t kRssiMin = -95;
static constexpr int8_t kRssiMax = -20;
static constexpr uint16_t kQueueDropLimit = 255;
static constexpr uint32_t kSnapshotMaxAgeMs = 16000;
#if HAMLET_DEBUG_LOG
static constexpr uint32_t kCsiTrackerDebugIntervalMs = 1000;
#endif

// CSI arrives on the Wi-Fi task while the app drains samples on Core 1.
// Protect the target snapshot and ring metadata; volatile is not a lock.
static portMUX_TYPE csiMux = portMUX_INITIALIZER_UNLOCKED;

struct RawSample {
    uint32_t seenMs = 0;
    uint32_t rxTimestampUs = 0;
    int8_t rssi = 0;
    int8_t noiseFloor = -127;
    uint8_t channel = 0;
    uint8_t signalMode = 0;
    uint8_t channelBandwidth = 0;
    uint8_t secondaryChannel = 0;
    uint8_t stbc = 0;
    bool firstWordInvalid = false;
    uint16_t originalLength = 0;
    uint16_t retainedLength = 0;
    uint8_t mac[6] = {0};
    uint8_t iq[kMaxIqBytes] = {};
};

struct Smoothed {
    bool seen = false;
    uint32_t lastSeenMs = 0;
    uint32_t lastRxTimestampUs = 0;
    uint16_t sampleCount = 0;
    int8_t noiseFloor = -127;
    int16_t proxEMA_x4 = 0;
    int16_t prevProxEMA_x4 = 0;
    int32_t rssiSmooth_x256 = -70 * 256;
    int8_t trend = 0;
    uint8_t quality = 0;
    uint8_t channelChange = 0;
    uint8_t frequencySpread = 0;
    uint8_t stability = 0;
    uint8_t fade = 0;
    uint8_t fadeShape = 0;
    uint8_t temporalCorrelation = 0;
    uint8_t confidence = 0;
    uint8_t staleDecaySteps = 0;
    uint16_t prevPairCount = 0;
    uint16_t prevNormPower[kMaxFeaturePairs] = {};
    uint32_t layoutKey = 0u;
    uint16_t incompatibleLayouts = 0u;
    CsiFeatures::Metadata metadata{};
    CsiFeatures::Layout layout{};
};

struct TargetState {
    CsiTracker::TargetKind kind = CsiTracker::TARGET_NONE;
    uint8_t channel = 0;
    uint8_t mac[6] = {0};
    bool active = false;
};

static RawSample rawQueue[kQueueDepth];
// Main-loop-only feature workspace. Keeping this fixed avoids a multi-kilobyte
// stack frame on S3 while preserving the compact Core2 profile.
static uint32_t featurePowers[kMaxFeaturePairs] = {};
static uint32_t featureMedianScratch[kMaxFeaturePairs] = {};
static uint16_t featureNormalizedPower[kMaxFeaturePairs] = {};
static uint8_t queueHead = 0;
static uint8_t queueTail = 0;
static uint16_t droppedSamples = 0;
static bool enabled = false;
static TargetState target;
static Smoothed smooth;
static bool configured = false;
#if HAMLET_DEBUG_LOG
static uint32_t callbackPackets = 0u;
static uint32_t callbackQueued = 0u;
static uint32_t callbackRejected = 0u;
static uint32_t callbackTargetMismatch = 0u;
static uint32_t callbackPayloadSkip = 0u;
static uint32_t lastDebugLogMs = 0u;
#endif

static int clampInt(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static int absInt(int v) {
    return (v < 0) ? -v : v;
}

template <typename T>
static auto enableAckCsi(T& config, int) ->
    decltype(config.dump_ack_en = true, void()) {
    config.dump_ack_en = true;
}

template <typename T>
static void enableAckCsi(T&, long) {}

static void clearQueueLocked() {
    queueHead = 0;
    queueTail = 0;
    droppedSamples = 0;
}

static void clearQueue() {
    portENTER_CRITICAL(&csiMux);
    clearQueueLocked();
    portEXIT_CRITICAL(&csiMux);
}

static void queuePushLocked(const RawSample& sample) {
    const uint8_t next = static_cast<uint8_t>((queueHead + 1u) & kQueueMask);
    if (next == queueTail) {
        queueTail = static_cast<uint8_t>((queueTail + 1u) & kQueueMask);
        if (droppedSamples < kQueueDropLimit) ++droppedSamples;
    }

    rawQueue[queueHead] = sample;
    queueHead = next;
}

static bool queuePop(RawSample& sample) {
    portENTER_CRITICAL(&csiMux);
    if (queueHead == queueTail) {
        portEXIT_CRITICAL(&csiMux);
        return false;
    }
    sample = rawQueue[queueTail];
    queueTail = static_cast<uint8_t>((queueTail + 1u) & kQueueMask);
    portEXIT_CRITICAL(&csiMux);
    return true;
}

#if HAMLET_DEBUG_LOG
static uint8_t queueSizeLocked() {
    return static_cast<uint8_t>((queueHead >= queueTail)
                                    ? (queueHead - queueTail)
                                    : (queueHead + kQueueDepth - queueTail));
}

static const char* kindName(uint8_t kind) {
    switch (static_cast<CsiTracker::TargetKind>(kind)) {
        case CsiTracker::TARGET_WIFI_CLIENT:
            return "client";
        case CsiTracker::TARGET_WIFI_AP:
            return "ap";
        case CsiTracker::TARGET_DEAUTH_SOURCE:
            return "deauth";
        default:
            return "none";
    }
}

static void logSnapshotFailure(const char* reason, const uint32_t nowMs,
                              uint32_t ageMs,
                              uint8_t reasonKind,
                              const bool targetActive,
                              const uint32_t sampleCount,
                              const uint16_t sampleQuality,
                              const uint16_t sampleStability,
                              const uint16_t sampleConfidence,
                              const uint16_t sampleFade,
                              const uint32_t queueSize) {
    if (nowMs - lastDebugLogMs < kCsiTrackerDebugIntervalMs) return;
    lastDebugLogMs = nowMs;

    const uint32_t lastSeen = smooth.lastSeenMs;
    const uint32_t sinceSeen = sampleCount > 0u
        ? CsiTrackerMath::elapsedMs(nowMs, lastSeen) : 0u;
    uint8_t targetChannel;
    bool configuredLocal;
    bool enabledLocal;
    uint16_t droppedLocal;
    uint32_t packetCount;
    uint32_t queuedCount;
    uint32_t targetMismatchCount;
    uint32_t rejectCount;
    portENTER_CRITICAL(&csiMux);
    targetChannel = target.channel;
    enabledLocal = enabled;
    configuredLocal = configured;
    droppedLocal = droppedSamples;
    packetCount = callbackPackets;
    queuedCount = callbackQueued;
    targetMismatchCount = callbackTargetMismatch;
    rejectCount = callbackRejected;
    portEXIT_CRITICAL(&csiMux);

    HAMLET_LOGF(
        "[CSI] snap_%s enabled=%u cfg=%u kind=%u/%s ch=%u active=%u seen=%u age=%lu last=%lu samp=%u qual=%u stab=%u conf=%u fade=%u q=%u/%u dropped=%u cb=%lu/%lu/%lu/%lu\n",
        reason,
        (unsigned)(enabledLocal && configuredLocal),
        (unsigned)configuredLocal,
        (unsigned)reasonKind,
        kindName(reasonKind),
        (unsigned)targetChannel,
        (unsigned)targetActive,
        (unsigned)(sampleCount > 0u),
        (unsigned long)sinceSeen,
        (unsigned long)lastSeen,
        (unsigned)sampleCount,
        (unsigned)sampleQuality,
        (unsigned)sampleStability,
        (unsigned)sampleConfidence,
        (unsigned)sampleFade,
        (unsigned)queueSize,
        (unsigned)kQueueDepth,
        (unsigned)droppedLocal,
        (unsigned long)packetCount,
        (unsigned long)queuedCount,
        (unsigned long)targetMismatchCount,
        (unsigned long)rejectCount);
}

static void logSnapshotState(uint32_t nowMs, uint32_t ageMs) {
    if (nowMs - lastDebugLogMs < kCsiTrackerDebugIntervalMs) return;
    lastDebugLogMs = nowMs;

    uint8_t kindLocal;
    uint8_t channelLocal;
    uint8_t quality;
    uint8_t stability;
    uint8_t confidence;
    uint8_t fade;
    uint16_t sampleCountLocal;
    uint32_t packetCount;
    uint32_t queuedCount;
    uint32_t targetMismatchCount;
    uint32_t rejectCount;
    uint8_t queueSize;
    uint16_t droppedLocal;
    portENTER_CRITICAL(&csiMux);
    kindLocal = target.kind;
    channelLocal = target.channel;
    quality = smooth.quality;
    stability = smooth.stability;
    confidence = smooth.confidence;
    fade = smooth.fade;
    sampleCountLocal = smooth.sampleCount;
    packetCount = callbackPackets;
    queuedCount = callbackQueued;
    targetMismatchCount = callbackTargetMismatch;
    rejectCount = callbackRejected;
    droppedLocal = droppedSamples;
    queueSize = queueSizeLocked();
    portEXIT_CRITICAL(&csiMux);
    HAMLET_LOGF(
        "[CSI] snap_ok kind=%u/%s ch=%u qual=%u stab=%u conf=%u fade=%u samples=%u age=%lu qsize=%u/%u dropped=%u cb=%lu/%lu/%lu/%lu\n",
        (unsigned)kindLocal,
        kindName(kindLocal),
        (unsigned)channelLocal,
        (unsigned)quality,
        (unsigned)stability,
        (unsigned)confidence,
        (unsigned)fade,
        (unsigned)sampleCountLocal,
        (unsigned long)ageMs,
        (unsigned)queueSize,
        (unsigned)kQueueDepth,
        (unsigned)droppedLocal,
        (unsigned long)packetCount,
        (unsigned long)queuedCount,
        (unsigned long)targetMismatchCount,
        (unsigned long)rejectCount);
}

static void logTargetState(const char* msg) {
    const uint32_t nowMs = millis();
    if (nowMs - lastDebugLogMs < kCsiTrackerDebugIntervalMs) return;
    lastDebugLogMs = nowMs;

    portENTER_CRITICAL(&csiMux);
    const CsiTracker::TargetKind kind = target.kind;
    const uint8_t ch = target.channel;
    const uint8_t kindActive = target.active ? 1u : 0u;
    const uint8_t queueSize = queueSizeLocked();
    const uint8_t mac0 = target.mac[0];
    const uint8_t mac1 = target.mac[1];
    const uint8_t mac2 = target.mac[2];
    const uint8_t mac3 = target.mac[3];
    const uint8_t mac4 = target.mac[4];
    const uint8_t mac5 = target.mac[5];
    const uint16_t droppedLocal = droppedSamples;
    portEXIT_CRITICAL(&csiMux);

    HAMLET_LOGF(
        "[CSI] target_%s kind=%u/%s ch=%u active=%u q=%u/%u dropped=%u mac=%02X:%02X:%02X:%02X:%02X:%02X\n",
        msg,
        (unsigned)kind,
        kindName(kind),
        (unsigned)ch,
        (unsigned)kindActive,
        (unsigned)queueSize,
        (unsigned)kQueueDepth,
        (unsigned)droppedLocal,
        (unsigned)mac0,
        (unsigned)mac1,
        (unsigned)mac2,
        (unsigned)mac3,
        (unsigned)mac4,
        (unsigned)mac5);
}
#endif

static bool targetMatchesLocked(const uint8_t* mac, uint8_t channel) {
    if (!target.active || target.kind == CsiTracker::TARGET_NONE) return false;
    if (channel != target.channel) return false;
    return memcmp(mac, target.mac, sizeof(target.mac)) == 0;
}

static uint16_t mapRssiToProximity(int8_t rssi) {
    const int cr = clampInt((int)rssi, kRssiMin, kRssiMax);
    return (uint16_t)clampInt(((cr - kRssiMin) * 920) / (kRssiMax - kRssiMin) + 80,
                              80, 1000);
}

static uint8_t iirU8(uint8_t a, uint8_t b, uint8_t ratio) {
    if (ratio == 0) return b;
    return (uint8_t)((((uint16_t)a * (uint16_t)(ratio - 1u)) + (uint16_t)b) / ratio);
}

static int16_t iirI16(int16_t a, int16_t b, uint8_t ratio) {
    if (ratio == 0) return b;
    return (int16_t)((((int32_t)a * (int32_t)(ratio - 1u)) + (int32_t)b) / (int32_t)ratio);
}

static void clearState() {
    smooth.seen = false;
    smooth.lastSeenMs = 0;
    smooth.lastRxTimestampUs = 0;
    smooth.sampleCount = 0;
    smooth.noiseFloor = -127;
    smooth.proxEMA_x4 = 0;
    smooth.prevProxEMA_x4 = 0;
    smooth.rssiSmooth_x256 = -70 * 256;
    smooth.trend = 0;
    smooth.quality = 0;
    smooth.channelChange = 0;
    smooth.frequencySpread = 0;
    smooth.stability = 0;
    smooth.fade = 0;
    smooth.fadeShape = 0;
    smooth.temporalCorrelation = 0;
    smooth.confidence = 0;
    smooth.staleDecaySteps = 0;
    smooth.prevPairCount = 0;
    memset(smooth.prevNormPower, 0, sizeof(smooth.prevNormPower));
    smooth.layoutKey = 0u;
    smooth.incompatibleLayouts = 0u;
    smooth.metadata = {};
    smooth.layout = {};
}

static void processRawSample(const RawSample& sample) {
    if (sample.retainedLength < 2u) return;

    CsiFeatures::Metadata metadata{};
    metadata.signalMode = sample.signalMode;
    metadata.channelBandwidth = sample.channelBandwidth;
    metadata.secondaryChannel = sample.secondaryChannel;
    metadata.stbc = sample.stbc;
    metadata.originalLength = sample.originalLength;
    metadata.retainedLength = sample.retainedLength;
    metadata.firstWordInvalid = sample.firstWordInvalid;
    const CsiFeatures::Layout layout =
        CsiFeatures::deriveLayout(metadata);
    if (!layout.valid || layout.usablePairs == 0u) return;

    CsiFeatures::PowerFeatures features{};
    if (!CsiFeatures::extractPower(
            reinterpret_cast<const int8_t*>(sample.iq),
            sample.retainedLength, metadata, layout,
            featurePowers, featureMedianScratch,
            featureNormalizedPower,
            kMaxFeaturePairs, features)) {
        return;
    }

    const bool hadPrevious = smooth.seen;
    const bool sameShape =
        hadPrevious &&
        smooth.layoutKey == layout.key &&
        smooth.prevPairCount == features.usablePairs;
    const uint32_t sampleGapMs = sameShape
        ? CsiTrackerMath::elapsedMs(sample.seenMs, smooth.lastSeenMs)
        : UINT32_MAX;
    const bool comparableFrame = sameShape &&
        sampleGapMs <= CsiTrackerMath::kTemporalComparableMaxMs;
    uint8_t temporalCorrelation = 0u;
    const uint8_t rawChannelChange = comparableFrame
        ? CsiFeatures::compareNormalized(
              smooth.prevNormPower, featureNormalizedPower,
              features.usablePairs, temporalCorrelation)
        : 0u;
    if (hadPrevious && !sameShape &&
        smooth.incompatibleLayouts < 65535u) {
        ++smooth.incompatibleLayouts;
    }
    memcpy(smooth.prevNormPower, featureNormalizedPower,
           (size_t)features.usablePairs *
               sizeof(smooth.prevNormPower[0]));
    smooth.prevPairCount = features.usablePairs;
    smooth.layoutKey = layout.key;

    const uint8_t coverage = features.retainedCoverage;
    const uint8_t signal = (uint8_t)clampInt(
        ((int)sample.rssi - kRssiMin) * 100 / (kRssiMax - kRssiMin), 0, 100);
    const uint8_t rawQuality = (uint8_t)((coverage * 3u + signal * 2u) / 5u);
    const uint8_t temporalWeight = CsiTrackerMath::temporalWeight(
        sameShape, sampleGapMs);
    // No comparable predecessor means unknown continuity, never 100% stable.
    const uint8_t rawStability = (uint8_t)(
        ((uint16_t)temporalCorrelation * temporalWeight) / 100u);
    const int priorRssi = (int)(smooth.rssiSmooth_x256 / 256);
    const uint8_t rawFade = hadPrevious
        ? (uint8_t)clampInt(absInt((int)sample.rssi - priorRssi) * 8, 0, 100)
        : 0u;
    const uint16_t prox = mapRssiToProximity(sample.rssi);
    const int16_t rawProx = (int16_t)clampInt((int)prox, 0, 1000);
    const int16_t proxEMA = hadPrevious
        ? iirI16(smooth.proxEMA_x4 / 4, rawProx, 4) * 4
        : rawProx * 4;

    if (hadPrevious) {
        smooth.rssiSmooth_x256 =
            (((int32_t)smooth.rssiSmooth_x256 * 3) +
             ((int32_t)sample.rssi * 256)) / 4;
        smooth.quality = iirU8(smooth.quality, rawQuality, 4);
        smooth.channelChange =
            iirU8(smooth.channelChange, rawChannelChange, 3);
        smooth.frequencySpread =
            iirU8(smooth.frequencySpread, features.frequencySpread, 3);
        smooth.stability = iirU8(smooth.stability, rawStability, 3);
        smooth.fade = iirU8(smooth.fade, rawFade, 3);
        smooth.fadeShape =
            iirU8(smooth.fadeShape, features.fadeShape, 3);
        smooth.temporalCorrelation =
            iirU8(smooth.temporalCorrelation, temporalCorrelation, 3);
    } else {
        // The first valid sample is the initial condition, not one quarter of
        // a fictional -70 dBm / zero-proximity history. Confidence still uses
        // sample maturity, so this does not promote a one-frame lock.
        smooth.rssiSmooth_x256 = (int32_t)sample.rssi * 256;
        smooth.quality = rawQuality;
        smooth.channelChange = 0u;
        smooth.frequencySpread = features.frequencySpread;
        smooth.stability = 0u;
        smooth.fade = 0u;
        smooth.fadeShape = features.fadeShape;
        smooth.temporalCorrelation = 0u;
    }

    if (!smooth.seen) {
        smooth.prevProxEMA_x4 = proxEMA;
        smooth.seen = true;
    }

    smooth.proxEMA_x4 = proxEMA;
    smooth.prevProxEMA_x4 = iirI16(smooth.prevProxEMA_x4, proxEMA, 4);
    smooth.trend = (int8_t)clampInt((int)(smooth.proxEMA_x4 - smooth.prevProxEMA_x4) / 4, -20, 20);
    const uint8_t maturity = (uint8_t)clampInt(
        ((int)smooth.sampleCount + 1) * 100 / 12, 0, 100);
    const uint8_t dropPenalty = (uint8_t)clampInt((int)droppedSamples * 2, 0, 35);
    const uint8_t layoutPenalty =
        hadPrevious && !sameShape ? 20u : 0u;
    smooth.confidence = (uint8_t)clampInt(
        ((int)smooth.quality * 45 + (int)smooth.stability * 35 +
         (int)maturity * 20) / 100 - dropPenalty - layoutPenalty,
        0, 100);
    if (smooth.sampleCount < 65535u) ++smooth.sampleCount;
    smooth.noiseFloor = sample.noiseFloor;
    smooth.metadata = metadata;
    smooth.layout = layout;
    smooth.lastSeenMs = sample.seenMs;
    smooth.lastRxTimestampUs = sample.rxTimestampUs;
    smooth.staleDecaySteps = 0u;
}

void csiRxCallback(void* ctx, wifi_csi_info_t* data) {
    (void)ctx;
#if !defined(HAMLET_WIFI_CSI)
    (void)data;
    return;
#else
    if (!data) return;
    portENTER_CRITICAL(&csiMux);
#if HAMLET_DEBUG_LOG
    ++callbackPackets;
#endif
    if (!data->buf || data->len == 0u) {
#if HAMLET_DEBUG_LOG
        ++callbackPayloadSkip;
        ++callbackRejected;
#endif
        portEXIT_CRITICAL(&csiMux);
        return;
    }

    const bool wantsSample = enabled &&
        targetMatchesLocked(data->mac, data->rx_ctrl.channel);
    if (!wantsSample) {
#if HAMLET_DEBUG_LOG
        ++callbackTargetMismatch;
        ++callbackRejected;
#endif
    }
    portEXIT_CRITICAL(&csiMux);
    if (!wantsSample) return;

    RawSample sample;
    sample.seenMs = millis();
    sample.rxTimestampUs = data->rx_ctrl.timestamp;
    sample.rssi = data->rx_ctrl.rssi;
    sample.noiseFloor = data->rx_ctrl.noise_floor;
    sample.channel = static_cast<uint8_t>(data->rx_ctrl.channel);
    sample.signalMode =
        static_cast<uint8_t>(data->rx_ctrl.sig_mode);
    sample.channelBandwidth =
        static_cast<uint8_t>(data->rx_ctrl.cwb);
    sample.secondaryChannel =
        static_cast<uint8_t>(data->rx_ctrl.secondary_channel);
    sample.stbc = static_cast<uint8_t>(data->rx_ctrl.stbc);
    sample.firstWordInvalid = data->first_word_invalid;
    sample.originalLength = data->len;
    sample.retainedLength = static_cast<uint16_t>(
        clampInt((int)data->len, 0, kMaxIqBytes));
    memcpy(sample.mac, data->mac, sizeof(sample.mac));
    memcpy(sample.iq, data->buf, sample.retainedLength);

    // Recheck under the same lock used for target changes. A retarget between
    // the early packet checks and enqueue must not poison the new target.
    portENTER_CRITICAL(&csiMux);
    bool pushed = false;
    if (enabled && targetMatchesLocked(data->mac, data->rx_ctrl.channel)) {
        queuePushLocked(sample);
#if HAMLET_DEBUG_LOG
        ++callbackQueued;
#endif
        pushed = true;
    }
    portEXIT_CRITICAL(&csiMux);
    if (!pushed) {
        portENTER_CRITICAL(&csiMux);
#if HAMLET_DEBUG_LOG
        ++callbackRejected;
        ++callbackTargetMismatch;
#endif
        portEXIT_CRITICAL(&csiMux);
    }
#endif
}

}  // namespace

namespace CsiTracker {

bool begin() {
#if defined(HAMLET_WIFI_CSI)
    if (isEnabled()) {
        clearTarget();
        return true;
    }

    clearQueue();
    clearState();

    wifi_csi_config_t csiConfig = {};
    csiConfig.lltf_en = true;
    csiConfig.htltf_en = true;
    csiConfig.stbc_htltf2_en = true;
    csiConfig.ltf_merge_en = true;
    csiConfig.channel_filter_en = false;
    csiConfig.manu_scale = false;
    csiConfig.shift = 0;
    enableAckCsi(csiConfig, 0);

    esp_err_t err = esp_wifi_set_csi_config(&csiConfig);
    if (err != ESP_OK) return false;
    err = esp_wifi_set_csi_rx_cb(csiRxCallback, nullptr);
    if (err != ESP_OK) return false;
    err = esp_wifi_set_csi(true);
    if (err != ESP_OK) {
        err = esp_wifi_set_csi_rx_cb(nullptr, nullptr);
        (void)err;
        return false;
    }

    portENTER_CRITICAL(&csiMux);
    enabled = true;
    configured = true;
    portEXIT_CRITICAL(&csiMux);
    #if HAMLET_DEBUG_LOG
    logTargetState("begin");
    #endif
    return true;
#else
    (void)configured;
    return false;
#endif
}

void end() {
#if defined(HAMLET_WIFI_CSI)
    if (!isEnabled()) return;

    esp_err_t err = esp_wifi_set_csi(false);
    (void)err;
    err = esp_wifi_set_csi_rx_cb(nullptr, nullptr);
    (void)err;
    portENTER_CRITICAL(&csiMux);
    enabled = false;
    configured = false;
    target = {};
    clearQueueLocked();
    portEXIT_CRITICAL(&csiMux);
    #if HAMLET_DEBUG_LOG
    logTargetState("end");
    #endif
    clearState();
#else
    return;
#endif
}

void setTarget(TargetKind kind, const uint8_t* mac, uint8_t channel) {
#if defined(HAMLET_WIFI_CSI)
    if (!isEnabled() || mac == nullptr || kind == TARGET_NONE || channel == 0u) {
        clearTarget();
        return;
    }

    portENTER_CRITICAL(&csiMux);
    if (target.active && target.kind == kind && target.channel == channel &&
        memcmp(target.mac, mac, sizeof(target.mac)) == 0) {
        portEXIT_CRITICAL(&csiMux);
        return;
    }

    target.kind = kind;
    target.channel = channel;
    memcpy(target.mac, mac, sizeof(target.mac));
    target.active = true;
    clearQueueLocked();
    portEXIT_CRITICAL(&csiMux);
    #if HAMLET_DEBUG_LOG
    logTargetState("set");
    #endif
    clearState();
#else
    (void)kind;
    (void)mac;
    (void)channel;
#endif
}

void clearTarget() {
#if defined(HAMLET_WIFI_CSI)
    portENTER_CRITICAL(&csiMux);
    target = {};
    clearQueueLocked();
    portEXIT_CRITICAL(&csiMux);
    #if HAMLET_DEBUG_LOG
    logTargetState("clear");
    #endif
    clearState();
#endif
}

void update() {
#if defined(HAMLET_WIFI_CSI)
    if (!isEnabled()) return;
    RawSample sample;
    uint8_t processed = 0u;
    while (processed < kMaxSamplesPerUpdate && queuePop(sample)) {
        processRawSample(sample);
        ++processed;
    }

    if (smooth.seen) {
        const uint32_t now = millis();
        const uint32_t ageMs = CsiTrackerMath::elapsedMs(now, smooth.lastSeenMs);
        if (ageMs > CsiTrackerMath::kStaleDecayStartMs) {
            const uint8_t dueSteps = CsiTrackerMath::staleDecaySteps(ageMs);
            while (smooth.staleDecaySteps < dueSteps) {
                smooth.confidence = (uint8_t)(smooth.confidence * 7u / 10u);
                smooth.channelChange = (uint8_t)(smooth.channelChange / 2u);
                smooth.stability = (uint8_t)(smooth.stability * 8u / 10u);
                ++smooth.staleDecaySteps;
            }
        }
    }
#else
    return;
#endif
}

bool getSnapshot(Snapshot& out) {
    memset(&out, 0, sizeof(out));
#if defined(HAMLET_WIFI_CSI)
    const uint32_t now = millis();
    const uint32_t age = smooth.seen
        ? CsiTrackerMath::elapsedMs(now, smooth.lastSeenMs) : 0u;

    if (!isEnabled()) {
        #if HAMLET_DEBUG_LOG
        uint32_t queueSize;
        portENTER_CRITICAL(&csiMux);
        queueSize = queueSizeLocked();
        portEXIT_CRITICAL(&csiMux);
        logSnapshotFailure("disabled", now, age, target.kind, target.active,
                           smooth.sampleCount, smooth.quality,
                           smooth.stability, smooth.confidence,
                           smooth.fade, queueSize);
        #endif
        return false;
    }

    if (!target.active || !smooth.seen || smooth.sampleCount == 0u) {
        #if HAMLET_DEBUG_LOG
        uint32_t queueSize;
        portENTER_CRITICAL(&csiMux);
        queueSize = queueSizeLocked();
        portEXIT_CRITICAL(&csiMux);
        logSnapshotFailure("not_ready", now, age, target.kind, target.active,
                           smooth.sampleCount, smooth.quality,
                           smooth.stability, smooth.confidence,
                           smooth.fade, queueSize);
        #endif
        return false;
    }
    if (age > kSnapshotMaxAgeMs) {
        #if HAMLET_DEBUG_LOG
        uint32_t queueSize;
        portENTER_CRITICAL(&csiMux);
        queueSize = queueSizeLocked();
        portEXIT_CRITICAL(&csiMux);
        logSnapshotFailure("stale", now, age, target.kind, target.active,
                           smooth.sampleCount, smooth.quality,
                           smooth.stability, smooth.confidence,
                           smooth.fade, queueSize);
        #endif
        return false;
    }

    #if HAMLET_DEBUG_LOG
    logSnapshotState(now, age);
    #endif

    if (target.kind == TARGET_NONE) {
        return false;
    }

    out.valid = true;
    out.kind = static_cast<uint8_t>(target.kind);
    memcpy(out.mac, target.mac, sizeof(out.mac));
    out.channel = target.channel;
    out.rssi = (int8_t)clampInt((int)(smooth.rssiSmooth_x256 / 256), kRssiMin, kRssiMax);
    out.noiseFloor = smooth.noiseFloor;
    out.signalMode = smooth.metadata.signalMode;
    out.channelBandwidth = smooth.metadata.channelBandwidth;
    out.secondaryChannel = smooth.metadata.secondaryChannel;
    out.stbc = smooth.metadata.stbc;
    out.ltfMask = smooth.layout.ltfMask;
    out.originalLength = smooth.metadata.originalLength;
    out.retainedLength = smooth.metadata.retainedLength;
    out.usablePairs = smooth.layout.usablePairs;
    out.lltfPairs = smooth.layout.lltfPairs;
    out.htltfPairs = smooth.layout.htltfPairs;
    out.stbcHtlftPairs = smooth.layout.stbcPairs;
    out.lastSeenMs = smooth.lastSeenMs;
    out.rxTimestampUs = smooth.lastRxTimestampUs;
    out.ageMs = age;
    out.sampleCount = smooth.sampleCount;
    out.quality = smooth.quality;
    out.channelChange = smooth.channelChange;
    out.frequencySpread = smooth.frequencySpread;
    out.stability = smooth.stability;
    out.fade = smooth.fade;
    out.fadeShape = smooth.fadeShape;
    out.temporalCorrelation = smooth.temporalCorrelation;
    out.confidence = smooth.confidence;
    out.incompatibleLayouts = smooth.incompatibleLayouts;
    portENTER_CRITICAL(&csiMux);
    out.droppedSamples = droppedSamples;
    portEXIT_CRITICAL(&csiMux);
    out.proximity = static_cast<uint16_t>(clampInt((int)smooth.proxEMA_x4 / 4, 0, 1000));
    return true;
#else
    return false;
#endif
}

bool isEnabled() {
    portENTER_CRITICAL(&csiMux);
    const bool result = enabled && configured;
    portEXIT_CRITICAL(&csiMux);
    return result;
}

bool isTargetActive() {
    portENTER_CRITICAL(&csiMux);
    const bool result = enabled && configured && target.active && target.kind != TARGET_NONE;
    portEXIT_CRITICAL(&csiMux);
    return result;
}

}
