/**
 * Mood - Full personality system with speech bubble
 * Ported from legacy stack with pedometer integration
 * Phase 1: Persistent mood + streak system
 */
#pragma once

#include <M5Unified.h>
#include "avatar.h"

// Mood tiers based on effective mood (base + session momentum)
enum class MoodTier {
    ECSTATIC,   // 80 to 100
    HAPPY,      // 60 to 79
    CONTENT,    // 40 to 59
    MEH,        // 20 to 39
    SAD,        // 0 to 19
    DEPRESSED,  // -20 to -1
    NEGLECTED   // -100 to -21
};

class Mood {
public:
    static void init();
    static void draw(M5Canvas& canvas, int steps = 0, float distance = 0.0f);
    
    // Event triggers
    static void onBoot();
    static void onWalkStart();        // Kid starts walking
    static void onHuntStart();
    static void onHuntStop();
    static void onHuntEnd();          // Alias for onHuntStop
    static void onPMKIDCapture();
    static void onPMKID();            // Alias for onPMKIDCapture
    static void onHandshakeCapture(const char* netName = nullptr);
    static void onFirstCapture();     // First PMKID or handshake of session
    static void onFirstDiscovery(uint8_t authType);  // first capture of a new auth type
    static void onNewNetwork();
    static void onProbeAttempt();
    static void onBufferFull();
    static void onBufferFilling();      // 80% buffer warning
    static void onSyncStart();
    static void onSyncComplete(bool success, uint16_t captureCount = 0);  // Phase 4A: scaled reward
    static void onLowBattery();
    static void onEmergencySync();            // 10% battery + unsaved captures warning
    static void onSleepBlocked();             // Sleep blocked due to captures in PSRAM
    static void onIdle(bool isHunting = false);
    static void onMilestone(int networks, int captures);
    static void onStepMilestone(int steps);
    static void onWalkMilestone(uint32_t steps);  // session reward/trophy milestones
    
    // Phase A: Mode-aware decay
    static uint32_t getDecayInterval();
    
    // Daily goal events (Phase 2)
    static void onGoalClose(uint32_t remaining);  // 80-99% progress
    static void onGoalComplete();                  // 100% goal met
    
    // New event triggers - tactical phrases with target info
    static void onDeauth(const char* ssid = nullptr);  // MUDBALL attack launched
    static void onSAEReject();             // SAE downgrade reject sent
    static void onCatching(const char* ssid = nullptr); // Waiting for 4-way
    static void onClientSpotted();       // New client detected
    static void onModeChange(uint8_t behavior); // 0=CAMP, 1=PATROL, 2=SPRINT, 3=LURK
    static void onPluggedIn();           // USB-C inserted - pig excited
    static void onProbeSuccess();        // Probe got response
    static void onProbeFail();           // Probe timed out
    static void onBackOnline();          // Networks found after dead air
    static void onDeadAir();             // No networks for a while
    
    // D-UCB learning events
    static void onChannelExploit(uint8_t channel);  // High-reward channel selected
    static void onChannelExplore();      // Exploration phase active
    static void onChannelLearned(uint8_t channel); // Channel just paid off (first reward)
    
    // Handshake progress visual feedback
    static void onHandshakeProgress(uint8_t mask, const char* ssid);  // M1234 pattern update
    static void onFourwayVictory();  // 4-way handshake complete shout
    
    // === XP LEVEL-UP ===
    static void onLevelUp(uint8_t newLevel);          // Level boundary crossed

    // === WEATHER EVENTS ===
    static void onBirdKill();                          // Bird zapped by wave ripple
    static void onShipKill();                          // Nostromo shuttle shot down
    static void onDebrisRain();                        // Nuclear fallout debris raining

    // === NEAR-MISS ===
    static void onNearMiss(uint8_t capturedMask, const char* ssid = nullptr);

    // === PET INTERACTION ===
    static void onPetted(uint8_t petCount);            // Touch pig — diminishing returns

    // === WARDRIVE EVENTS ===
    static void onWardriveNetwork();                   // new network mapped (every 10th)
    static void onWardriveMilestone(uint16_t count);   // 50/100/500/1000 milestone
    static void onWardriveEnd();                       // session debrief

    // === TELEPORT EVENTS ===
    static void onTeleportArrival();                   // HOUSE_TO_HUNT portal completed

    // === IPP DEFENSE EVENTS ===
    static void onTrackerDetected(uint8_t threatType, int8_t rssi, const char* detail);
    static void onTrackerFollowing(const char* detail);
    static void onBleSpamDetected();
    static void onEvilTwin(const char* ssid);
    static void onKarmaDetected(const char* ssid);
    static void onKnownAPFound(const char* ssid);
    static void onReconScan(uint8_t trackerCount);
    static void onAttackerIdentified(const char* detail);
    static void onDualBandStalk(const char* detail);
    static void onCanaryTripped(const char* ssid);
    static void onKarmaConfirmed(const char* ssid);
    static void onToolIdentified(const char* toolName);

    // ==[ SESSION DEBRIEF ]== consumed by DEFHOG4 on next materialization
    static bool hasDebrief();
    static void injectDebrief();  // push debrief lines to DEFHOG4 terminal

    // State queries
    static bool hasPhrase();
    static const char* getCurrentPhrase();
    static void clearPhrase();
    static AvatarState getCurrentMoodState();
    
    // Momentum system
    static void addMomentum(int amount);
    static int getMomentum();
    static void decayMomentum();  // Explicit decay - call once per frame
    static void setMomentum(int value);  // DEBUG: Direct set for testing
    
    // Phase 1: Effective mood (base + session momentum)
    static int getEffectiveMood();
    static MoodTier getMoodTier();
    static MoodTier getMoodTier(int mood);
    static const char* getMoodLabel();  // HYP3/GUD/0K/M3H/S4D
    // Phase B: Mood label with charge indicator
    static const char* getMoodLabelWithCharge();  // "HYP3⚡" or "HYP3" etc
    // Phase C: Effectiveness multiplier for hunt parameters
    static float getEffectivenessMultiplier();
    
    // Phase D: Critical mood system (sync warning timer)
    static int getCriticalCountdown();  // -1 = inactive, 0+ = seconds remaining
    static bool isFirstCritical();      // true = first time, show full monologue
    static bool isInCriticalScene();    // true = scene active, grass/parallax frozen
    static void trigger4MinCritical();  // Phase 3: 4-min IDLE timeout trigger
    
    // Phase 4: Rib escape sequence (A+B hold during countdown)
    static void checkRibEscapeInput();  // called from hamlet.cpp update()
    static bool handleRibEscapeButton(char btn);  // returns true if escape complete
    
    // Phrase queue
    static void queuePhrase(const char* phrase, AvatarState state = AvatarState::NEUTRAL);
    
    // Direct phrase setting (for dynamic hunt messages)
    static void setPhrase(const char* phrase, AvatarState state);
    // Printf-style phrase with variables (SSID, count, etc)
    static void setPhraseFormatted(AvatarState state, const char* fmt, ...);
    
private:
    static char currentPhrase[96];
    static AvatarState moodState;
    static int momentum;
    static uint32_t momentumDecayTime;
    static uint32_t lastPhraseTime;
    static uint32_t phraseTimeout;
    
    // Phrase queue (up to 4 phrases — 5-slot circular buffer)
    static char phraseQueue[5][64];
    static AvatarState stateQueue[5];
    static int queueHead;
    static int queueTail;
    
    // History to avoid repeats
    static int lastPhraseIndex[8];
    static int historyPos;
    
    // Phrase selection
    static const char* selectPhrase(const char* const* phrases, int count, bool trackHistory = true);
    static void nextFromQueue();
    
    // Bubble drawing
    static void drawBubble(M5Canvas& canvas, const char* text);
    static int wrapText(const char* text, char lines[][28], int maxLines);

public:
    // Reusable bubble renderer (same style as idle/hunt speech bubble)
    static void drawBubbleAt(M5Canvas& canvas, const char* text,
                             int pigX, int pigBodyRight, int pigTop,
                             int noseX, int noseY, bool flipSide = false);
};
