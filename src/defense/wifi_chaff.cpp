/**
 * WiFi Chaff — Fake Handshake Injection Engine
 *
 * ==[ HOGWASH ]== deauth detected? inject garbage M1+M2 for same BSSID.
 * attacker captures valid-looking handshakes. hashcat eats them. GPU melts.
 * no password matches because the MIC came from /dev/urandom, not a PMK.
 */

#include "wifi_chaff.h"
#include "../core/config.h"
#include "../core/wsl_bypasser.h"
#include "../ui/display.h"
#include "../ui/defhog_terminal.h"
#include "../util/debug_log.h"
#include <esp_wifi.h>
#include <esp_random.h>
#include <string.h>

// forward declare Hunt accessor (avoids heavy header include)
namespace Hunt {
    bool isActive();
    uint8_t getCurrentChannel();
    const uint8_t* getCurrentTargetBSSID();
}

namespace WifiChaff {

// ==[ TUNING ]==
static constexpr uint8_t  FAKE_STAS_PER_BURST = 4;     // fake clients per deauth event
static constexpr uint32_t M1_M2_GAP_MS        = 50;    // delay between M1 and M2 (realistic timing)
static constexpr uint32_t BURST_DELAY_MS       = 200;   // delay after deauth detection before injecting
static constexpr uint32_t RATE_LIMIT_MS        = 5000;  // min time between bursts for same BSSID
static constexpr uint32_t GLOBAL_RATE_MS       = 10000; // max 3 bursts per this window
static constexpr uint8_t  MAX_GLOBAL_BURSTS    = 3;

// ==[ DEFERRED BURST ]== queued from Hunt/Spectrum callback path
struct DeferredBurst {
    uint8_t  targetBssid[6];
    uint8_t  channel;
    uint32_t queuedAt;
    bool     pending;
};
static DeferredBurst deferred[2] = {};

// ==[ RATE LIMITING ]==
// Was per-BSSID in a single slot; that slot rotated every burst, effectively
// disabling the per-target cooldown under a multi-victim deauth where the
// attacker hits BSSID_A, BSSID_B, BSSID_A in quick succession. Chaff transmits
// on a channel, not a BSSID — rate-limit per channel so multiple APs on the
// same channel share one cooldown and can't all coin-fire.
static uint32_t lastBurstPerChannel[14] = {0};  // index 0 unused, 1-13 valid
static uint32_t globalBurstTimes[MAX_GLOBAL_BURSTS] = {};
static uint8_t  globalBurstHead = 0;

// ==[ SESSION STATS ]==
static uint32_t totalInjections = 0;
static uint32_t totalBursts = 0;
static uint8_t  lastTargetBssid[6] = {};
static uint8_t  lastChannel = 0;
static bool     hasFired = false;

// ==[ HELPERS ]==

static bool isRateLimited(const uint8_t* bssid, uint8_t channel, uint32_t now) {
    (void)bssid;  // per-channel rate limit supersedes per-BSSID
    // per-channel rate limit
    if (channel >= 1 && channel <= 13) {
        uint32_t last = lastBurstPerChannel[channel];
        if (last != 0 && (now - last) < RATE_LIMIT_MS) return true;
    }
    // global rate limit: count bursts in window
    uint8_t recent = 0;
    for (uint8_t i = 0; i < MAX_GLOBAL_BURSTS; i++) {
        if (globalBurstTimes[i] > 0 && (now - globalBurstTimes[i]) < GLOBAL_RATE_MS) {
            recent++;
        }
    }
    return recent >= MAX_GLOBAL_BURSTS;
}

static void recordBurst(const uint8_t* bssid, uint8_t channel, uint32_t now) {
    (void)bssid;
    if (channel >= 1 && channel <= 13) {
        lastBurstPerChannel[channel] = now;
    }
    globalBurstTimes[globalBurstHead] = now;
    globalBurstHead = (globalBurstHead + 1) % MAX_GLOBAL_BURSTS;
}

static bool isHuntTarget(const uint8_t* bssid, uint8_t channel) {
    if (!Hunt::isActive()) return false;
    const uint8_t* huntBssid = Hunt::getCurrentTargetBSSID();
    if (huntBssid && memcmp(bssid, huntBssid, 6) == 0) return true;
    if (channel == Hunt::getCurrentChannel()) return true;
    return false;
}

static bool canTransmit() {
    wifi_mode_t mode;
    if (esp_wifi_get_mode(&mode) != ESP_OK) return false;
    return mode != WIFI_MODE_NULL;
}

// inject one complete fake handshake (M1 + M2) for a single fake STA
static bool injectOneFake(const uint8_t* bssid, uint64_t replayCounter) {
    uint8_t fakeSta[6];
    uint8_t anonce[32], snonce[32], fakeMic[16];

    // random locally-administered STA MAC
    esp_fill_random(fakeSta, 6);
    fakeSta[0] = (fakeSta[0] & 0xFC) | 0x02;  // locally administered, unicast

    // random crypto material (hardware RNG)
    esp_fill_random(anonce, 32);
    esp_fill_random(snonce, 32);
    esp_fill_random(fakeMic, 16);

    bool ok = WSLBypasser::sendFakeEAPOLM1(bssid, fakeSta, anonce, replayCounter);
    if (!ok) return false;

    // brief gap — real handshakes have ~10-100ms between M1 and M2
    delay(M1_M2_GAP_MS);

    ok = WSLBypasser::sendFakeEAPOLM2(bssid, fakeSta, snonce, fakeMic, replayCounter);
    return ok;
}

// ==[ PUBLIC API ]==

void injectBurst(const uint8_t* targetBssid, uint8_t channel, uint16_t frameCount) {
    if (!Config::getWifiChaffEnabled()) return;
    if (!canTransmit()) return;

    uint32_t now = millis();
    if (isRateLimited(targetBssid, channel, now)) return;
    if (isHuntTarget(targetBssid, channel)) return;

    // hop to target channel before injecting
    esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);

    HAMLET_LOGF("[HOGWASH] injecting %d fakes for %02X:%02X:%02X:%02X:%02X:%02X ch%d\n",
                  FAKE_STAS_PER_BURST,
                  targetBssid[0], targetBssid[1], targetBssid[2],
                  targetBssid[3], targetBssid[4], targetBssid[5], channel);

    uint64_t replayCounter = esp_random();
    for (uint8_t i = 0; i < FAKE_STAS_PER_BURST; i++) {
        injectOneFake(targetBssid, replayCounter + i);
    }

    recordBurst(targetBssid, channel, now);
    totalInjections += FAKE_STAS_PER_BURST;
    totalBursts++;
    memcpy(lastTargetBssid, targetBssid, 6);
    lastChannel = channel;
    hasFired = true;

    // notify user
    char msg[48];
    snprintf(msg, sizeof(msg), "HOGWASH ch%d\n%d fake handshakes", channel, FAKE_STAS_PER_BURST);
    Display::showToast(msg, 3000);
    DefhogTerminal::pushLine("HOGWASH ch%d: %d fakes TX", channel, FAKE_STAS_PER_BURST);
}

void onDeauthDetected(const uint8_t* targetBssid, uint8_t channel,
                      uint16_t frameCount, int8_t peakRSSI) {
    if (!Config::getWifiChaffEnabled()) return;

    uint32_t now = millis();
    if (isRateLimited(targetBssid, channel, now)) return;
    if (isHuntTarget(targetBssid, channel)) return;

    // find free deferred slot
    for (uint8_t i = 0; i < 2; i++) {
        if (!deferred[i].pending) {
            memcpy(deferred[i].targetBssid, targetBssid, 6);
            deferred[i].channel = channel;
            deferred[i].queuedAt = now;
            deferred[i].pending = true;
            return;
        }
    }
    // both slots full — skip this burst
}

void update(uint32_t now) {
    for (uint8_t i = 0; i < 2; i++) {
        if (!deferred[i].pending) continue;
        if ((now - deferred[i].queuedAt) < BURST_DELAY_MS) continue;

        // fire deferred burst
        injectBurst(deferred[i].targetBssid, deferred[i].channel, 0);
        deferred[i].pending = false;
    }
}

void stop() {
    for (uint8_t i = 0; i < 2; i++) deferred[i].pending = false;
}

bool isActive() {
    for (uint8_t i = 0; i < 2; i++) {
        if (deferred[i].pending) return true;
    }
    return false;
}

uint32_t getTotalInjections() { return totalInjections; }
uint32_t getTotalBursts() { return totalBursts; }
const uint8_t* getLastTargetBSSID() { return hasFired ? lastTargetBssid : nullptr; }
uint8_t getLastChannel() { return lastChannel; }

}  // namespace WifiChaff

// ==[ FREE FUNCTION WRAPPERS ]== called from Recon via extern
void wifiChaff_injectBurst(const uint8_t* targetBssid, uint8_t channel,
                            uint16_t frameCount) {
    WifiChaff::injectBurst(targetBssid, channel, frameCount);
}
void wifiChaff_onDeauthDetected(const uint8_t* targetBssid, uint8_t channel,
                                 uint16_t frameCount, int8_t peakRSSI) {
    WifiChaff::onDeauthDetected(targetBssid, channel, frameCount, peakRSSI);
}
void wifiChaff_update(uint32_t now) { WifiChaff::update(now); }
void wifiChaff_stop() { WifiChaff::stop(); }
bool wifiChaff_isActive() { return WifiChaff::isActive(); }
