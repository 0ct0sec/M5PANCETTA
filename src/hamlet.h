/**
 * Hamlet - Core Header
 *
 * ==[ STATE RANCH ]== mode table + forward declarations for the pig brain.
 */

#ifndef HAMLET_H
#define HAMLET_H

#include <Arduino.h>

// ==[ BUILD TAGS ]== defaults; platformio.ini overrides in real builds
#ifndef HAMLET_VERSION
#define HAMLET_VERSION "0.1.0"
#endif

#ifndef HAMLET_BUILD_DATE
#define HAMLET_BUILD_DATE __DATE__
#endif

// ==[ HAMLET MODES ]== every screen the pig can inhabit
enum class HamletMode {
    IDLE,           // idle canvas; waiting for button pokes
    MENU,           // navigation list
    HUNT,           // truffle hunt: active WiFi capture
    SPECTRUM,       // 2.4GHz analyzer + paranoid swine overlay
    LOOT,           // capture vault: PMKIDs/handshakes
    FEEDING,        // mood breakdown + rib inventory
    WALK_STATS,     // pedometer stats
    SETTINGS,       // device settings
    NOWFLOCK,       // passive FLOCK / FNOW/3 coordination
    POWER_MENU,     // sleep/shutdown bar
    ABOUT,          // about screen
    WEBCONFIG,      // web portal for WPA-SEC credentials
    WARDRIVE,       // wardriving: WiFi scan → WiGLE CSV to SD
    BLE_SCANNER,    // BLE device tracker: scan + list + Geiger signal
    DEFHOG4,        // full-screen defense terminal: 5-pane interactive command center
    XFER,           // AP file manager: PANCETTA_XFER hotspot + web commander
    C5MONSTER,      // C5Monster dual-band command menu
    MAIL,           // P1G P0ST: filed case letters + decision trees
    MESH            // M3SH T4LK: LoRa text over a Meshtastic Unit C6L
};

// ==[ MODE COUNT ]== one definition, because three separate tables keyed by
// this enum each rolled their own — and each one silently stopped covering the
// newest mode the moment it was added. A table that misses an entry does not
// fail to compile; it just quietly stops answering for one screen.
static constexpr uint8_t HAMLET_MODE_COUNT =
    static_cast<uint8_t>(HamletMode::MESH) + 1;

// ==[ API SURFACE ]== main loop + helpers
namespace Hamlet {
    // ==[ BOOTSTRAP ]==
    void init();
    
    // ==[ HEARTBEAT ]==
    void update();
    
    // ==[ MODE SWITCHING ]==
    HamletMode getMode();
    void setMode(HamletMode mode);
    void enterMode(HamletMode mode);
    void exitCurrentMode();
    
    // ==[ BUTTONS ]==
    void handleBtnOK(bool longPress);
    void handleBtnBack(bool longPress);
    
    // ==[ SYSTEM INFO ]==
    uint32_t getUptimeSeconds();
    uint8_t getBatteryPercent();
    bool isCharging();
    
    // ==[ PSRAM INFO ]==
    uint32_t getPSRAMFree();
    uint32_t getPSRAMTotal();
    
    // ==[ PARANOIA MODE ]== global deauth alert hooks
    void triggerGlobalDeauth(int8_t rssi, uint8_t channel);  // fed by the active deauth detector
    bool isParanoiaToastActive();
    int8_t getParanoiaRSSI();
    uint8_t getParanoiaChannel();
    
    // ==[ IDLE TRACKING ]== for mood decay urgency
    uint32_t getIdleDuration();
}

#endif // HAMLET_H
