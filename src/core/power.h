/**
 * Power - Adaptive power management
 *
 * ==[ JUICE BOX ]== mode-aware profiles, CPU scaling, TX power control.
 * small battery = every mA counts. adapt or die.
 */

#ifndef POWER_H
#define POWER_H

#include <Arduino.h>

// forward declare to avoid circular include
enum class HamletMode;

namespace Power {
    // ==[ POWER PROFILES ]== match activity to juice budget
    enum class Profile : uint8_t {
        IDLE,        // 240MHz, no WiFi, 60fps - avatar + street animation
        MENU,        // 240MHz, no WiFi, 60fps - room scene go brrr
        HUNT,        // 240MHz, max TX, 60fps - capture mode go brrr
        SPECTRUM,    // 240MHz, max TX, 60fps - waterfall needs refresh
        FLOCK,       // 160MHz, medium TX, 15fps - passive ESP-NOW
        WEBCONFIG,   // 160MHz, medium TX, 15fps - STA mode
        WARDRIVE,    // 240MHz, medium TX, 60fps - cockpit scene go brrr
        BLE_SCANNER, // 160MHz, min TX, 30fps - radar + sparklines
        LOW_BATTERY  // 80MHz, low TX, 10fps - emergency conservation
    };

    // ==[ CPU FREQUENCY ]== ESP32/S3: 80/160/240 MHz
    // WiFi needs 80MHz minimum when active
    enum class CpuFreq : uint8_t {
        SLOW = 80,
        MED = 160,
        FAST = 240
    };

    // ==[ TX POWER ]== 0.25dBm units (8=2dBm, 80=20dBm)
    enum class TxPower : uint8_t {
        TX_MIN = 8,      // 2 dBm - stealth mode
        TX_LOW = 20,     // 5 dBm - reduced range
        TX_MED = 40,     // 10 dBm - balanced
        TX_HIGH = 60,    // 15 dBm - extended range
        TX_MAX = 80      // 20 dBm - full power
    };

    // ==[ PROFILE CONFIG ]== what each profile gets
    struct ProfileConfig {
        CpuFreq cpuFreq;
        TxPower txPower;
        uint8_t targetFps;    // base frame rate (battery scales down)
        bool allowDimming;    // display auto-dim ok?
        bool wifiPowerSave;   // WiFi modem sleep ok for this profile?
    };

    // ==[ INIT ]== call once at boot
    void init();

    // ==[ MODE HOOKS ]== called by hamlet.cpp on transitions
    void onModeEnter(HamletMode mode);
    void onModeExit(HamletMode mode);
    void reevalWiFiPS(HamletMode mode);  // re-evaluate WiFi power save after BLE state change
    void applyCurrentRadioSettings();     // re-apply TX/PS after WiFi starts

    // ==[ PROFILE CONTROL ]==
    void setProfile(Profile profile);
    Profile getCurrentProfile();
    const ProfileConfig& getProfileConfig(Profile profile);

    // ==[ DIRECT CONTROL ]== for manual override
    void setCpuFrequency(CpuFreq freq);
    void setTxPower(TxPower power);

    // ==[ GETTERS ]== for other modules
    int8_t getTxPowerDbm();       // current TX power in dBm
    uint32_t getCpuFreqMHz();     // current CPU frequency

    // ==[ BATTERY ADAPTATION ]==
    void updateBatteryPolicy();   // call every 30s from main loop
    bool isLowPowerMode();        // reduced battery (20-30%)
    bool isCriticalMode();        // emergency battery (<10%)
    bool allowTransitionHaptic();  // false when the battery sag guard is active
    bool isExternalPowerPresent(); // VBUS present, including a full battery
    bool isDimmingAllowed();       // current effective profile allows backlight dimming
    const char* getAdaptationStateLabel(); // OFF/BYPASS/ARMED/LOW/CRIT

    // ==[ FRAME BUDGET ]== target FPS by profile
    uint8_t getTargetFPS();
    void setTargetFPSOverride(uint8_t fps); // 0 returns custody to the mode profile

    // ==[ CONFIG ACCESSORS ]== backed by NVS via Config::
    bool getBatteryAdaptation();
    void setBatteryAdaptation(bool enabled);
}

#endif // POWER_H
