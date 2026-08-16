/**
 * Spectrum Mode - Wi-Fi spectrum analyzer and RF target monitor.
 *
 * RF PERISCOPE: passive 2.4 GHz capture, C5-backed 5 GHz snapshots,
 * measured waterfall rendering, client tracking, and deauth detection.
 */
#pragma once

#include <Arduino.h>
#include <M5Unified.h>

#include "../core/config.h"

// Full definition lives in radio/c5monster_uart.h.
namespace C5Monster {
struct ScanResults;
}

// Tracking limits. These remain macros because existing project code may use
// them in compile-time array declarations outside this translation unit.
#define MAX_SPECTRUM_NETWORKS 48
#define MAX_SPECTRUM_CLIENTS 16
#define MAX_SPECTRUM_PROBES 64
#define MAX_ROGUE_ALERTS 8
#define MAX_BSSID_CLUSTERS 16

// Canonical analyzer buffer geometry for the 320x240 display.
#define SPECTRUM_WIDTH 290
#define WATERFALL_ROWS 22

// Deauthentication burst detector configuration.
#define DEAUTH_HISTORY_LEN 16
#define DEAUTH_BURST_THRESHOLD 5
#define DEAUTH_BURST_WINDOW 3000

// Runtime state for the deauthentication alert and hot/cold feedback.
struct ParanoidSwine {
    bool attackActive;
    uint8_t attackChannel;
    uint32_t attackStart;
    uint32_t lastDeauthTime;

    uint16_t frameCount;
    uint16_t framesInWindow;
    uint32_t windowStart;

    int8_t rssiHistory[DEAUTH_HISTORY_LEN];
    uint8_t historyIdx;
    int8_t rssiCurrent;
    int8_t rssiPeak;
    int8_t rssiTrend;

    uint32_t lastLedToggle;
    bool ledState;

    uint32_t lastToastToggle;
    bool toastInverted;

    bool detailViewActive;

    // Retained for state compatibility; Spectrum currently uses Geiger audio.
    bool morseActive;
    uint8_t morseElementIdx;
    uint32_t morseNextTime;
};

struct SpectrumClient {
    uint8_t mac[6];
    int8_t rssi;              // Client-transmitted frames only.
    int8_t snr;               // Client-transmitted frames only.
    uint32_t lastSeen;        // Either direction; presence and cleanup.
    uint32_t lastSignalSeen;  // Client -> AP only; RSSI and bearing evidence.
    uint32_t lastSignalRxUs;  // rx_ctrl.timestamp packet identity.
    bool hasSignal;           // Timestamp zero is valid after rollover.
};

struct SpectrumNetwork {
    uint8_t bssid[6];
    char ssid[33];
    uint8_t channel;
    int8_t rssi;
    uint32_t lastSeen;
    uint8_t authmode;
    bool hasPMF;       // RSN MFPR: protected management frames are required.
    bool isHidden;
    bool wasRevealed;
    uint8_t probeAttempts;

    // Main-loop intelligence fields populated from queued RX observations.
    uint8_t phyMode;       // 0=?, 1=11b, 2=11g, 3=HT, 4=VHT.
    uint8_t channelWidth;  // 0=20 MHz, 1=40 MHz (rx_ctrl.cwb).
    bool ftmResponder;     // Extended Capabilities bit 70.

    SpectrumClient clients[MAX_SPECTRUM_CLIENTS];
    uint8_t clientCount;
};

struct SpectrumProbeEntry {
    uint8_t clientMAC[6];
    char ssid[33];
    int8_t rssi;
    uint32_t lastSeen;
};

struct RogueAlert {
    // 0=LOW mesh, 1=MED channel mismatch, 2=HIGH OUI mismatch,
    // 3=CRITICAL authentication/PMF mismatch.
    uint8_t severity;
    uint8_t bssid1[6];
    uint8_t bssid2[6];
    char ssid[33];
    uint32_t detectedAt;
};

struct BSSIDCluster {
    uint8_t oui[3];
    uint8_t memberCount;
    uint16_t memberIndices[6];
};

namespace Spectrum {

// Lifecycle and drawing.
void start();
void stop();
void update();
// Per-frame canvas clear. Preserves the carrier plot band for progressive
// reveal when eligible, otherwise wipes the whole sprite. Call once at the top
// of the spectrum screen render, before draw().
void beginFrame(M5Canvas& canvas);
void draw(M5Canvas& canvas);

// Radio sharing and status.
void togglePause();
bool isPaused();
bool isActive();
bool isInClientMode();
uint8_t getCurrentChannel();
uint8_t getLockedChannel();
uint32_t getRuntime();
void printRfTrace(bool clearAfter = false);

// Spectrum viewport controls.
void panLeft();
void panRight();
void zoomIn();
void zoomOut();
void zoomToggle();
void toggleModelOverlay();
bool isModelOverlayEnabled();
float getViewCenter();

// Network and client navigation.
void nextNetwork();
void prevNetwork();
void selectNetwork();
void exitClientMode();
bool isClientDetailActive();
bool isC5CarrierDetailActive();
void toggleClientDetail();
void closeClientDetail();

// C5Monster action selector.
void nextC5ArsenalCommand();
void prevC5ArsenalCommand();
bool executeSelectedC5ArsenalCommand();
const char* getSelectedC5ArsenalCommandLabel();
uint8_t getSelectedC5ArsenalCommand();
uint8_t getC5ArsenalCommandCount();
bool hasC5TargetSelection();
bool isSelectedC5ArsenalCommandTargeted();
bool isSelectedC5ArsenalCommandAvailable();

// Bearing recalibration. Clock positions are screen-relative:
// 12=ahead, 3=right, 6=behind, 9=left.
enum ApBearing : uint8_t {
    AP_UNKNOWN = 0,
    AP_12 = 1,
    AP_3 = 2,
    AP_6 = 3,
    AP_9 = 4
};
void toggleApBearing();

// Conservative target keepalive request used by the client scanner.
bool pokeSelectedClient();

// Explicit CoreS3 SE FTM range action. Passive capture pauses for the request;
// FTM range never contributes directional evidence.
enum class FtmRangeStatus : uint8_t {
    UNAVAILABLE,
    READY,
    REQUESTING,
    SUCCESS,
    UNSUPPORTED,
    REJECTED,
    NO_RESPONSE,
    FAILED,
    TIMEOUT
};

struct FtmRangeEvidence {
    FtmRangeStatus status;
    uint32_t distanceCm;
    uint32_t varianceCm2;
    uint16_t sampleCount;
    uint32_t ageMs;
    bool responderAdvertised;
    bool active;
    bool valid;
};

bool startSelectedFtmRange();
bool getFtmRangeEvidence(FtmRangeEvidence& out);

// Deauthentication alert state.
bool isAttackActive();
void toggleParanoidDetail();
bool isParanoidDetailActive();
const ParanoidSwine* getParanoidState();

// Selection and captured data.
int16_t getSelectedIndex();
int16_t getSelectedClientIndex();
const SpectrumNetwork* getSelectedNetwork();
const SpectrumClient* getSelectedClient();
const SpectrumNetwork* getNetworks();
uint16_t getNetworkCount();
int8_t getChannelPeakRSSI(uint8_t channel);
int8_t getChannelAvgRSSI(uint8_t channel);
uint16_t getChannelNetworkCount(uint8_t channel);
const int8_t* getRSSIHistory(uint8_t channel);
uint8_t getHistoryPosition();

// Upright gyro channel dial.
bool isDialMode();
void toggleDialMode();
void toggleDialLock();
bool isDialLocked();
uint8_t getDialChannel();
uint32_t getDialPps();

// Intelligence sidecars.
uint16_t getProbeCount();
const SpectrumProbeEntry* getProbeEntries();
const uint8_t* getVulnTiers();        // [networkCount], T0-T7.
const uint8_t* getReadinessScores();  // [networkCount], 0-100.
uint8_t getClusterCount();
uint8_t getRogueAlertCount();
const RogueAlert* getRogueAlerts();

// D-UCB reconnaissance export.
bool isDUCBReconMode();
void getReconChannelData(uint16_t* activity, uint32_t* time,
                         uint16_t* networks, int8_t* peakRssi,
                         uint16_t* attackable = nullptr);
uint32_t getReconLastUpdateMs();
bool hasReconData();
uint32_t getRfQueueDrops();
uint32_t getMeasurementSweepEpoch();
uint16_t getMeasurementCoverageMask();

// Dual-band 5 GHz support through C5Monster.
bool is5GHzEnabled();
void set5GHzEnabled(bool enabled);
bool isShowing5GHz();
void toggleBand();
int8_t get5GHzChannelPeakRSSI(uint8_t channel);
uint16_t get5GHzChannelNetworkCount(uint8_t channel);
void feedC5MonsterScan(const C5Monster::ScanResults& results);

}  // namespace Spectrum
