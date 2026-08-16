/**
 * Power - Adaptive power management implementation
 *
 * ==[ JUICE BOX ]== CPU scaling, TX control, battery adaptation.
 * 200mAh battery + 2.4GHz radio = power diet mandatory.
 */

#include "power.h"
#include "power_policy.h"
#include "config.h"
#include "../hamlet.h"
#include "../haptic/haptic.h"
#include <M5Unified.h>
#include <esp_wifi.h>
#include "../defense/recon.h"
#include "../defense/defense_pipeline.h"

namespace Power {

// ==[ PROFILE CONFIGS ]== indexed by Profile enum
//                              CPU             TX              FPS  DIM    WIFI PS
static const ProfileConfig profileConfigs[] = {
    { CpuFreq::FAST, TxPower::TX_MIN,  60, true,  true  },  // IDLE
    { CpuFreq::FAST, TxPower::TX_MIN,  60, true,  true  },  // MENU / rooms
    { CpuFreq::FAST, TxPower::TX_MAX,  60, false, false },  // HUNT
    { CpuFreq::FAST, TxPower::TX_MAX,  60, false, false },  // SPECTRUM
    { CpuFreq::MED,  TxPower::TX_MED,  15, false, false },  // FLOCK
    { CpuFreq::MED,  TxPower::TX_MED,  15, false, false },  // WEBCONFIG
    { CpuFreq::FAST, TxPower::TX_MED,  60, false, false },  // WARDRIVE
    { CpuFreq::MED,  TxPower::TX_MIN,  30, false, false },  // BLE_SCANNER
    { CpuFreq::SLOW, TxPower::TX_LOW,  10, true,  true  },  // LOW_BATTERY
};

// ==[ STATE ]==
static Profile currentProfile = Profile::IDLE;
static CpuFreq currentCpuFreq = CpuFreq::FAST;
static TxPower currentTxPower = TxPower::TX_MAX;
static bool inLowPowerMode = false;
static bool inCriticalMode = false;
static bool batteryAdaptation = true;
static uint8_t lastBatteryPct = 100;
static bool lastUsbPowered = false;
static uint8_t targetFpsOverride = 0;

static constexpr uint8_t BROWNOUT_GUARD_THRESHOLD = 35;

// ==[ INTERNAL HELPERS ]==

// map HamletMode to power Profile
static Profile modeToProfile(HamletMode mode) {
    switch (mode) {
        case HamletMode::IDLE:
        case HamletMode::WALK_STATS:
        case HamletMode::ABOUT:
        case HamletMode::FEEDING:
            return Profile::IDLE;

        case HamletMode::BLE_SCANNER:
            return Profile::BLE_SCANNER;

        case HamletMode::MENU:
        case HamletMode::LOOT:
        case HamletMode::SETTINGS:
        case HamletMode::POWER_MENU:
        case HamletMode::DEFHOG4:
        case HamletMode::MAIL:
        case HamletMode::MESH:
            return Profile::MENU;

        case HamletMode::HUNT:
            return Profile::HUNT;

        case HamletMode::SPECTRUM:
            return Profile::SPECTRUM;

        case HamletMode::NOWFLOCK:
            return Profile::FLOCK;

        case HamletMode::WEBCONFIG:
            return Profile::WEBCONFIG;

        case HamletMode::WARDRIVE:
            return Profile::WARDRIVE;

        case HamletMode::XFER:
            return Profile::WEBCONFIG;

        case HamletMode::C5MONSTER:
            return Profile::IDLE;

        default:
            return Profile::IDLE;
    }
}

// check if WiFi is currently active
static bool isWiFiActive() {
    wifi_mode_t mode;
    if (esp_wifi_get_mode(&mode) != ESP_OK) {
        return false;
    }
    return mode != WIFI_MODE_NULL;
}

static bool isHighDrawRadioProfile(Profile profile) {
    return profile == Profile::HUNT || profile == Profile::SPECTRUM;
}

static bool isCadenceCriticalProfile(Profile profile) {
    return profile == Profile::HUNT ||
           profile == Profile::SPECTRUM ||
           profile == Profile::WARDRIVE ||
           profile == Profile::BLE_SCANNER;
}

static TxPower capTxPower(TxPower requested, TxPower cap) {
    return (static_cast<uint8_t>(requested) > static_cast<uint8_t>(cap)) ? cap : requested;
}

bool isExternalPowerPresent() {
    const int16_t vbusMv = M5.Power.getVBUSVoltage();
    if (vbusMv >= 4000) return true;
    if (vbusMv >= 0) return false;
    return M5.Power.isCharging() == m5::Power_Class::is_charging;
}

static uint8_t readBatteryPercent() {
    const int32_t raw = M5.Power.getBatteryLevel();
    if (raw < 0) return 100;  // unsupported/unknown must not trigger emergency policy
    if (raw > 100) return 100;
    return static_cast<uint8_t>(raw);
}

static void refreshBatteryState() {
    lastBatteryPct = readBatteryPercent();
    lastUsbPowered = isExternalPowerPresent();

    const PowerPolicy::BatteryState next = PowerPolicy::evaluateBatteryState(
        lastBatteryPct, batteryAdaptation, lastUsbPowered,
        {inLowPowerMode, inCriticalMode});
    inLowPowerMode = next.low;
    inCriticalMode = next.critical;
}

static bool brownoutGuardActive() {
    return batteryAdaptation &&
           !lastUsbPowered &&
           lastBatteryPct <= BROWNOUT_GUARD_THRESHOLD;
}

static ProfileConfig effectiveProfileConfig(Profile profile) {
    uint8_t pidx = static_cast<uint8_t>(profile);
    if (pidx >= sizeof(profileConfigs) / sizeof(profileConfigs[0])) pidx = 0;
    ProfileConfig config = profileConfigs[pidx];

    if (!batteryAdaptation || lastUsbPowered) return config;

    // Radio/UI hot paths need cadence. Save current by shaving TX and haptics,
    // not by starving the loop that feeds M5.update(), audio, callbacks, and UI.
    if (isCadenceCriticalProfile(profile)) {
        if (inCriticalMode) {
            config.txPower = capTxPower(config.txPower, TxPower::TX_LOW);
        } else if (brownoutGuardActive() && isHighDrawRadioProfile(profile)) {
            config.txPower = capTxPower(config.txPower, inLowPowerMode ? TxPower::TX_MED : TxPower::TX_HIGH);
        }
    } else if (inCriticalMode) {
        return profileConfigs[(uint8_t)Profile::LOW_BATTERY];
    } else if (profile == Profile::WEBCONFIG && inLowPowerMode) {
        config.txPower = TxPower::TX_LOW;
    }

    return config;
}

static void applyWiFiPowerSave(const ProfileConfig& config) {
    if (!isWiFiActive()) return;

    if (config.wifiPowerSave || DefensePipeline::snapshot().isBleInitialized()) {
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    } else {
        esp_wifi_set_ps(WIFI_PS_NONE);
    }
}

static void applyHapticScale(Profile profile) {
    if (inCriticalMode) {
        Haptic::setIntensityScale(0.0f);
        return;
    }

    if (brownoutGuardActive()) {
        Haptic::setIntensityScale(0.25f);
        return;
    }

    switch (profile) {
        case Profile::LOW_BATTERY:
            Haptic::setIntensityScale(0.3f);
            break;
        case Profile::HUNT:
        case Profile::SPECTRUM:
            Haptic::setIntensityScale(1.0f);
            break;
        default:
            Haptic::setIntensityScale(0.6f);
            break;
    }
}

// ==[ PUBLIC API ]==

void init() {
    // load battery adaptation setting from NVS
    batteryAdaptation = Config::getBatteryAdaptation();
    refreshBatteryState();

    // start at default profile
    currentProfile = Profile::IDLE;
    currentCpuFreq = CpuFreq::FAST;  // boot at full speed
    currentTxPower = TxPower::TX_MAX;
    inLowPowerMode = false;
    inCriticalMode = false;
    targetFpsOverride = 0;
    refreshBatteryState();
}

void setCpuFrequency(CpuFreq freq) {
    uint8_t mhz = static_cast<uint8_t>(freq);

    // WiFi requires 80MHz minimum when active
    if (isWiFiActive() && mhz < 80) {
        mhz = 80;
    }

    // setCpuFrequencyMhz is Arduino ESP32 built-in
    if (setCpuFrequencyMhz(mhz)) {
        currentCpuFreq = freq;
    }
}

void setTxPower(TxPower power) {
    // NOTE: no "if (power == currentTxPower) return" short-circuit here.
    // currentTxPower is a *requested* cache, not a hardware-applied cache.
    // The typical flow is:
    //   1. setProfile() → setTxPower() while WiFi is still off → caches value
    //   2. Mode's enter() fires → esp_wifi_start() → WiFi becomes active
    //   3. onModeEnter() re-calls setTxPower() with the same value
    // If we early-exit on a value match, step 3 skips esp_wifi_set_max_tx_power,
    // so the cached value never lands on the hardware — WiFi stays at ESP-IDF's
    // default TX power instead of the profile's. Always call through when WiFi
    // is active.

    refreshBatteryState();
    TxPower effectivePower = power;
    if (batteryAdaptation && !lastUsbPowered) {
        if (inCriticalMode) {
            effectivePower = capTxPower(effectivePower, TxPower::TX_LOW);
        } else if (brownoutGuardActive() && isHighDrawRadioProfile(currentProfile)) {
            effectivePower = capTxPower(effectivePower, inLowPowerMode ? TxPower::TX_MED : TxPower::TX_HIGH);
        }
    }

    if (!isWiFiActive()) {
        currentTxPower = effectivePower;  // cache for when WiFi starts
        return;
    }

    int8_t powerUnits = static_cast<int8_t>(effectivePower);

    // esp_wifi_set_max_tx_power takes 0.25dBm units
    esp_err_t err = esp_wifi_set_max_tx_power(powerUnits);

    if (err == ESP_OK) {
        currentTxPower = effectivePower;
    }
}

void setProfile(Profile profile) {
    refreshBatteryState();

    currentProfile = profile;
    ProfileConfig config = effectiveProfileConfig(profile);

    // apply CPU frequency
    setCpuFrequency(config.cpuFreq);

    // apply TX power (will be cached if WiFi not active)
    setTxPower(config.txPower);

    // ==[ WIFI POWER SAVE ]== modem sleep in idle profiles, full power for radio modes
    // idle/menu: radio sleeps between DTIM beacons. ~130-210mA savings.
    // hunt/spectrum: radio needs continuous RX for promiscuous capture.
    // coex constraint: BLE active → WIFI_PS_NONE crashes (must use MIN_MODEM).
    applyWiFiPowerSave(config);

    // ==[ HAPTIC SCALING ]== motor intensity tracks power profile
    applyHapticScale(profile);
}

Profile getCurrentProfile() {
    return currentProfile;
}

const ProfileConfig& getProfileConfig(Profile profile) {
    uint8_t pidx = static_cast<uint8_t>(profile);
    if (pidx >= sizeof(profileConfigs) / sizeof(profileConfigs[0])) pidx = 0;
    return profileConfigs[pidx];
}

void onModeEnter(HamletMode mode) {
    // View-specific overrides belong to the mode being left. The entering view
    // may claim a new one after its own state has been initialized.
    targetFpsOverride = 0;
    Profile targetProfile = modeToProfile(mode);
    setProfile(targetProfile);
    applyCurrentRadioSettings();
}

void onModeExit(HamletMode mode) {
    (void)mode;
}

void reevalWiFiPS(HamletMode mode) {
    // re-evaluate WiFi power save after a defense lifecycle handoff deinitializes BLE.
    // lighter than onModeEnter — skips CPU freq, TX power, FPS, haptic.
    if (!isWiFiActive()) return;
    refreshBatteryState();
    ProfileConfig config = effectiveProfileConfig(modeToProfile(mode));
    if (config.wifiPowerSave || DefensePipeline::snapshot().isBleInitialized()) {
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    } else {
        esp_wifi_set_ps(WIFI_PS_NONE);
    }
}

void applyCurrentRadioSettings() {
    refreshBatteryState();
    ProfileConfig config = effectiveProfileConfig(currentProfile);
    setTxPower(config.txPower);
    applyWiFiPowerSave(config);
}

int8_t getTxPowerDbm() {
    return static_cast<int8_t>(currentTxPower) / 4;
}

uint32_t getCpuFreqMHz() {
    return getCpuFrequencyMhz();  // Arduino built-in
}

uint8_t getTargetFPS() {
    if (targetFpsOverride > 0) return targetFpsOverride;
    // ==[ USB-C FPS BOOST ]== cable in + setting on = 60fps everywhere
    if (Config::getPwrFps60() && isExternalPowerPresent()) return 60;

    ProfileConfig config = effectiveProfileConfig(currentProfile);
    uint8_t baseFps = config.targetFps;

    // ==[ BATTERY SCALING ]== passive modes can coast; active radio modes cannot.
    if (batteryAdaptation && !isCadenceCriticalProfile(currentProfile)) {
        if (inCriticalMode)       return min(baseFps, (uint8_t)15);  // <10%: survival
        else if (inLowPowerMode)  return min(baseFps, (uint8_t)30);  // 10-30%: conserve
    }

    return baseFps;
}

void setTargetFPSOverride(uint8_t fps) {
    targetFpsOverride = fps;
}

void updateBatteryPolicy() {
    if (!batteryAdaptation) return;
    refreshBatteryState();

    setProfile(currentProfile);
}

bool isLowPowerMode() {
    return inLowPowerMode;
}

bool isCriticalMode() {
    return inCriticalMode;
}

bool allowTransitionHaptic() {
    return !brownoutGuardActive();
}

bool isDimmingAllowed() {
    return effectiveProfileConfig(currentProfile).allowDimming;
}

const char* getAdaptationStateLabel() {
    if (!batteryAdaptation) return "OFF";
    if (isExternalPowerPresent()) return "BYPASS";
    if (inCriticalMode) return "CRIT";
    if (inLowPowerMode) return "LOW";
    return "ARMED";
}

bool getBatteryAdaptation() {
    return batteryAdaptation;
}

void setBatteryAdaptation(bool enabled) {
    batteryAdaptation = enabled;
    Config::setBatteryAdaptation(enabled);
    refreshBatteryState();
    setProfile(currentProfile);  // apply TX/haptic/CPU policy immediately
}

} // namespace Power
