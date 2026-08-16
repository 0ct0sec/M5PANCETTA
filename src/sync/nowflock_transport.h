/**
 * NOWFLOCK firmware surface.
 */
#pragma once

#include <stdint.h>

namespace NowFlock {

struct Status {
    bool enabled;
    bool initialized;
    bool active;
    bool master;
    bool foreignTraffic;
    bool degraded;
    bool corroborated;
    uint8_t peerCount;
    uint8_t channel;
    uint8_t lastFrameType;
    uint8_t candidates;
    uint8_t exportable;
    uint8_t clockSource; // 0=none 1=gps 2=master 3=rtc
    uint32_t nodeId;
    uint32_t masterNodeId;
    uint16_t channelMask;
    uint16_t rxBad;
    uint16_t rxDup;
    uint16_t authFail;
    uint16_t txFail;
};

struct SwarmTarget {
    uint8_t bssid[6];
    uint8_t channel;
    uint32_t nodeId;
    uint32_t seenMs;
    bool valid;
};

void init();
void deinit();
void releaseRadio();  // deinit ESP-NOW before another owner stops/reconfigures WiFi
void update();
void updateBackground();
void stopSync();
void markEspNowNeedsReinit();
void requestPeerSummaries();
void setClaimedChannels(uint16_t mask);
void noteHuntChannel(uint8_t channel);
void broadcastTarget(const uint8_t bssid[6], uint8_t channel);
void broadcastCapture(const char* annotation);
bool getLastSwarmTarget(SwarmTarget& out);
const char* getLastCaptureAnnotation();
Status getStatus();
const char* frameTypeName(uint8_t type);
const char* roleName();
uint8_t helloCapabilities();

} // namespace NowFlock
