/**
 * WiFi Client - STA mode implementation
 *
 * ==[ UPLINK ]== pause promiscuous, connect to AP, do HTTP, disconnect, resume.
 */

#include "wifi_client.h"
#include "../core/config.h"
#include "../core/power.h"
#include "../defense/recon.h"
#include "../defense/defense_pipeline.h"
#include "../sync/nowflock_transport.h"
#include "../util/debug_log.h"
#include <WiFi.h>
#include <esp_wifi.h>

namespace WifiClient {

static State currentState = WIFI_IDLE;
static uint32_t connectStartTime = 0;
static uint32_t connectTimeout = 15000;  // 15s default (S3 can be slow)
static bool wasPromiscuous = false;
static bool reconSuspendedByClient = false;
// ==[ BACKOFF ]== misconfigured creds (wrong PSK, SSID typo) used to busy-loop
// reconnect at WiFi.begin() cadence — heats the SoC and drains the 390mAh cell.
// Exponential schedule: 10s, 20s, 40s, 80s, 160s, 320s, capped at 640s (~10min).
// Cleared on a successful CONNECTED transition; callers can force-reset if the
// user edits creds.
static uint32_t lastFailTime = 0;
static uint8_t consecutiveFails = 0;
static char lastError[48] = "";
static constexpr uint8_t BACKOFF_MAX_SHIFT = 6;   // 2^6 = 64x base
static constexpr uint32_t BACKOFF_BASE_MS  = 10000;  // 10s

static uint32_t computeBackoffMs() {
    uint8_t shift = consecutiveFails > 0 ? (uint8_t)(consecutiveFails - 1) : 0;
    if (shift > BACKOFF_MAX_SHIFT) shift = BACKOFF_MAX_SHIFT;
    return BACKOFF_BASE_MS * (1u << shift);
}

static void setFailure(const char* detail) {
    currentState = WIFI_FAILED;
    lastFailTime = millis();
    if (consecutiveFails < 255) consecutiveFails++;
    snprintf(lastError, sizeof(lastError), "%s", detail ? detail : "uplink failed");
    HAMLET_LOGF("[UPLINK] association failed: %s (wl=%d)\n",
                lastError, (int)WiFi.status());
}

static void clearFailure() {
    lastError[0] = '\0';
}

static void applyBleSafePowerSave() {
    esp_wifi_set_ps(DefensePipeline::snapshot().isBleInitialized() ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE);
}

void init() {
    currentState = WIFI_IDLE;
    lastFailTime = 0;
    consecutiveFails = 0;
    reconSuspendedByClient = false;
    clearFailure();
}

void resetBackoff() {
    consecutiveFails = 0;
    lastFailTime = 0;
    if (currentState == WIFI_FAILED) currentState = WIFI_IDLE;
    clearFailure();
}

bool connect() {
    const char* ssid = Config::getUploadWifiSsid();
    const char* pass = Config::getUploadWifiPass();

    if (!ssid || ssid[0] == '\0') {
        currentState = WIFI_FAILED;
        snprintf(lastError, sizeof(lastError), "SSID MISSING");
        return false;
    }

    return connect(ssid, pass);
}

bool connect(const char* ssid, const char* password) {
    if (currentState == WIFI_CONNECTING || currentState == WIFI_CONNECTED) {
        snprintf(lastError, sizeof(lastError), "UPLINK BUSY");
        return false;
    }
    // Honor backoff: stay in WIFI_FAILED until the cooldown elapses. Without
    // this, callers would retry every frame and peg the radio on bad creds.
    if (consecutiveFails > 0) {
        uint32_t elapsed = millis() - lastFailTime;
        if (elapsed < computeBackoffMs()) {
            currentState = WIFI_FAILED;
            uint32_t seconds = (computeBackoffMs() - elapsed + 999u) / 1000u;
            snprintf(lastError, sizeof(lastError), "RETRY IN %lus", (unsigned long)seconds);
            return false;
        }
    }

    clearFailure();

    // ESP-NOW must deinit before this client resets/reconfigures WiFi.
    NowFlock::releaseRadio();

    // Recon runs globally, including while LOOT is visible. Claim it before
    // association so a pending IPP scan cannot hop channels or stop STA under
    // us. Keep NimBLE warm; the keep-BLE pipeline state preserves the shared
    // WiFi controller for a coexistence-safe in-place handoff.
    reconSuspendedByClient = DefensePipeline::snapshot().isActive();
    if (reconSuspendedByClient) {
        DefensePipeline::requestOperatingState(Defense::OperatingState::SUSPENDED_KEEP_BLE);
    }

    // Check if promiscuous mode is active
    bool promiscActive = false;
    esp_wifi_get_promiscuous(&promiscActive);
    if (promiscActive) {
        wasPromiscuous = true;
        esp_wifi_set_promiscuous(false);
        delay(100);  // Let radio settle
    } else {
        wasPromiscuous = false;
    }

    // AP/promiscuous -> STA in-place. WIFI_OFF while NimBLE is warm races the
    // coexistence controller and has produced wifi-uninit timeouts on Core2.
    WiFi.disconnect(false);
    delay(25);
    if (!WiFi.mode(WIFI_STA)) {
        setFailure("RADIO MODE FAIL");
        if (reconSuspendedByClient) {
            DefensePipeline::requestOperatingState(Defense::OperatingState::BACKGROUND);
            reconSuspendedByClient = false;
        }
        return false;
    }
    Power::applyCurrentRadioSettings();
    applyBleSafePowerSave();

    // Start connection
    if (password && password[0]) {
        WiFi.begin(ssid, password);
    } else {
        WiFi.begin(ssid);  // Open network
    }

    currentState = WIFI_CONNECTING;
    connectStartTime = millis();

    return true;
}

void disconnect() {
    if (currentState == WIFI_IDLE && !reconSuspendedByClient) return;
    currentState = WIFI_DISCONNECTING;

    // Idempotent when connect() already released it; protects future callers
    // that need to tear down an active STA session directly.
    NowFlock::releaseRadio();

    WiFi.disconnect(false);
    delay(25);
    WiFi.mode(WIFI_STA);
    Power::applyCurrentRadioSettings();

    // Re-enable promiscuous mode if it was active
    if (wasPromiscuous) {
        applyBleSafePowerSave();
        esp_wifi_set_promiscuous(true);
        wasPromiscuous = false;
    }

    currentState = WIFI_IDLE;
    NowFlock::markEspNowNeedsReinit();
    if (reconSuspendedByClient) {
        DefensePipeline::requestOperatingState(Defense::OperatingState::BACKGROUND);
        reconSuspendedByClient = false;
    }
}

State getState() {
    return currentState;
}

bool isConnected() {
    return currentState == WIFI_CONNECTED && WiFi.status() == WL_CONNECTED;
}

bool isConnecting() {
    return currentState == WIFI_CONNECTING;
}

bool ownsRadio() {
    return currentState == WIFI_CONNECTING ||
           currentState == WIFI_CONNECTED ||
           currentState == WIFI_DISCONNECTING ||
           currentState == WIFI_FAILED;
}

String getIP() {
    if (isConnected()) {
        return WiFi.localIP().toString();
    }
    return "0.0.0.0";
}

int8_t getRSSI() {
    if (isConnected()) {
        return WiFi.RSSI();
    }
    return -127;
}

void update() {
    if (currentState == WIFI_CONNECTING) {
        wl_status_t status = WiFi.status();
        uint32_t elapsed = millis() - connectStartTime;

        if (status == WL_CONNECTED) {
            currentState = WIFI_CONNECTED;
            consecutiveFails = 0;  // success — clear the backoff ratchet
            clearFailure();
            HAMLET_LOGF("[UPLINK] associated; IP=%s RSSI=%d\n",
                        WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
        } else if (status == WL_NO_SSID_AVAIL) {
            // SSID not found - fail immediately
            setFailure("SSID NOT FOUND (2.4G?)");
        } else if (status == WL_CONNECT_FAILED) {
            // Wrong password or other auth failure
            setFailure("AP AUTH FAILED");
        } else if (elapsed > connectTimeout) {
            char detail[40];
            snprintf(detail, sizeof(detail), "AP TIMEOUT WL=%d", (int)status);
            setFailure(detail);
            WiFi.disconnect(false);
            NowFlock::markEspNowNeedsReinit();
        }
    } else if (currentState == WIFI_CONNECTED) {
        // Check if still connected
        if (WiFi.status() != WL_CONNECTED) {
            char detail[40];
            snprintf(detail, sizeof(detail), "LINK LOST WL=%d", (int)WiFi.status());
            setFailure(detail);
        }
    }
}

void setConnectTimeout(uint32_t ms) {
    connectTimeout = ms;
}

const char* getLastError() {
    return lastError;
}

uint32_t getBackoffRemainingMs() {
    if (consecutiveFails == 0 || lastFailTime == 0) return 0;
    uint32_t elapsed = millis() - lastFailTime;
    uint32_t backoff = computeBackoffMs();
    return elapsed < backoff ? backoff - elapsed : 0;
}

} // namespace WifiClient
