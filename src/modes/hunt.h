/**
 * Hunt Mode - Active hunting with PMKID probing and deauth
 *
 * ==[ TRUFFLE OPS ]== DONOHAM + OINK fused; MUDBALL deauth; motion-aware CAMP/PATROL.
 */

#ifndef HUNT_H
#define HUNT_H

#include <Arduino.h>
#include "../core/capture.h"

// Forward declaration — full type in radio/c5monster_uart.h
namespace C5Monster { struct ScanResults; }

// ==[ LIMITS ]==
#define MAX_HUNT_NETWORKS 64

// ==[ PROBE RETRY ]== multi-probe with exponential backoff
#define PROBE_MAX_ATTEMPTS     3       // diminishing returns after 3
#define PROBE_BACKOFF_BASE     3000    // 3s, 6s exponential
#define PROBE_BACKOFF_MULT     2

// channel hop order (common first). constexpr = internal linkage, one copy per TU.
constexpr uint8_t CHANNEL_ORDER[] = {1, 6, 11, 2, 3, 4, 5, 7, 8, 9, 10, 12, 13};
constexpr uint8_t CHANNEL_COUNT = sizeof(CHANNEL_ORDER);

// ==[ PROBE STATE ]==
enum class ProbeState {
    IDLE,           // normal hop, looking for targets
    TUNING,         // switching to target channel
    AUTHING,        // sending Open System auth, waiting for auth response
    SENDING,        // sending association request
    WAITING         // waiting for assoc response (PMKID lives here)
};

// ==[ DEAUTH STATE ]== MUDBALL pipeline
enum class DeauthState {
    IDLE,           // not deauthing
    TARGETING,      // found client, preparing
    THROWING        // sending deauth burst; dwell extension listens afterward
};

// ==[ BEHAVIOR MODE ]== motion-adaptive 4-state FSM
enum class HuntBehavior {
    CAMP,           // stationary - deep exploitation, longer dwell
    PATROL,         // walking - balanced discovery/capture
    SPRINT,         // fast movement - pure discovery, minimal deauth, fastest hop
    LURK            // near high-value target - focused attack, minimal channel switching
};

// ==[ CHANNEL STATS ]== for adaptive hopping
struct ChannelStats {
    volatile uint16_t beaconCount;       // total beacons seen (callback-written)
    volatile uint16_t networkCount;      // unique networks (callback-written)
    volatile uint16_t attackableCount;   // PMF/WPA3-free networks (callback-written)
    uint16_t clientCount;                // unique clients
    volatile uint8_t pmkidHits;          // PMKIDs captured (callback-written)
    uint8_t handshakeHits;               // handshakes captured (main loop only)
    volatile uint32_t lastBeacon;        // time of last beacon (callback-written)
};

// ==[ D-UCB STATS ]== for brain menu
struct DUCBStats {
    uint16_t pulls;             // visits to this channel
    uint16_t rewards;           // captures from this channel
    float avgReward;            // rewards / pulls
};

namespace Hunt {
    // ==[ RADIO SOURCE ]== where scan data comes from
    enum RadioSource : uint8_t {
        RADIO_CORE2_2G,   // ESP32-D0WDQ6-V3 (2.4GHz only)
        RADIO_C5_5G,      // C5Monster via UART (5GHz WiFi 6)
    };

    // ==[ MODE CONTROL ]==
    void start();
    void stop();
    void update();
    
    // ==[ PAUSE ]==
    void togglePause();
    bool isPaused();
    
    // ==[ STATUS ]==
    bool isActive();
    uint8_t getCurrentChannel();
    const uint8_t* getCurrentTargetBSSID();  // current deauth target or nullptr
    uint16_t getNetworkCount();
    uint16_t getActiveNetworkCount();  // Networks seen in last 30s
    uint16_t getClientCount();
    
    // ==[ SESSION LOOT ]==
    uint16_t getSessionPMKIDs();
    uint16_t getSessionHandshakes();
    
    // ==[ NETWORK LIST ]==
    const DetectedNetwork* getNetworks();
    
    // ==[ PROBE CONTROL ]==
    ProbeState getProbeState();
    uint16_t getProbeCount();
    
    // ==[ DEAUTH CONTROL ]==
    DeauthState getDeauthState();
    uint16_t getDeauthCount();
    uint8_t getLastAttackTier();  // 0=deauth, 1=eapol, 2=flood
    
    // ==[ BEHAVIOR ]==
    HuntBehavior getCurrentBehavior();
    
    // ==[ CHANNEL STATS ]==
    const ChannelStats* getChannelStats(uint8_t channel);
    
    // ==[ D-UCB STATS ]==
    DUCBStats getDUCBStats(uint8_t channel, HuntBehavior mode);
    uint32_t getDUCBTotalPulls(HuntBehavior mode);
    uint16_t getDUCBTotalRewards(HuntBehavior mode);

    // D-UCB reset (nuke from orbit)
    void resetDUCB();

    // ==[ PROBE HARVEST ]== passive client probe request intel
    struct HarvestedProbe {
        uint8_t clientMac[6];
        char ssid[33];
        int8_t rssi;
        uint32_t lastSeen;
    };
    uint16_t getHarvestedCount();
    uint16_t getTotalProbeRequests();
    const HarvestedProbe* getHarvestedProbes();  // nullable (PSRAM, alloc in start())

    // ==[ CAPTURE COMBO ]== chain count for gamification
    uint8_t getCaptureComboCount();

    // Channel hopping state (for grass animation)
    bool isChannelHopping();

    // Coordination API
    void enableCoordination(bool enabled);
    bool isCoordinationEnabled();
    void setCoordinationRole(uint8_t role);
    uint8_t getCoordinationRole();
    void setAssignedChannel(uint8_t channel, uint32_t validUntil);
    void updatePriorityAdjustments(const uint8_t* adjustments);

    // ==[ DUAL-BAND: 5GHz via C5Monster ]==
    void feedC5MonsterScan(const C5Monster::ScanResults& results);
    bool isDualBandActive();
    uint16_t getC5MonsterNetworkCount();
    int8_t getC5MonsterBestRssi();
    uint8_t getC5MonsterBestChannel();
    uint32_t getC5MonsterScanAgeMs();
}

#endif // HUNT_H
