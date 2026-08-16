/**
 * MenuPig — helper pig on menu + 6-room roaming
 *
 * two modes:
 * 1. helper — pig sits on tech crate in dark alley, neon sign flickers,
 *    pipe drips. explains menu items in speech bubbles.
 * 2. roaming — 6 full-screen cyberpunk noir rooms:
 *    cyberdeck lab, noir apartment, ramen bar, surveillance nest, underground bar,
 *    comfort balcony.
 *    pig walks between stations, naps, hacks, eats. lives its life.
 */
#pragma once

#include <M5Unified.h>

namespace MenuPig {
    struct MenuPigRenderOptions {
        bool includeDust = true;
        bool includeCupSteam = true;
        bool includeRamenSteam = true;
        bool includeCigSmoke = true;
    };

    struct TeleportParticleSample {
        int8_t homeX = 0;
        int8_t homeY = 0;
        uint8_t seed = 0;
    };

    void enter();
    void update(uint32_t now);

    // Helper mode: pig on bench, speech bubble for highlighted menu item
    void drawHelper(M5Canvas& canvas, const char* description);

    // Roaming mode: 6 rooms, pig wanders between stations
    void drawRoaming(M5Canvas& canvas);

    // Transitions
    void startRoaming();          // menu idle timeout → pig walks off bench
    void returnToHelper();        // user input → pig returns to bench
    bool isMenuTransitionLocked(); // modal local cinematic/transition lock for menu input
    bool isRoaming();             // true when pig is in room roaming mode
    bool isPigOnBench();          // true when pig is back on bench (safe to draw menu)
    // Bath-only audio reaction eligibility and the current debounced dance.
    bool isBathMicDanceEligible();
    bool isBathSoundDanceActive();
    bool isBathSoundDanceSurfaceActive(uint32_t now);
    uint8_t getBathSoundDanceBeatPhase(uint32_t now);
    void setEventHold(bool held); // freeze station clock while an NPC case card owns input
    void cycleRoom();             // BtnA while roaming → next room
    bool isHousePortalReady();    // true when roaming house pig is stable for transition
    bool getPortalAnchor(int16_t& x, int16_t& y);  // nose/mouth anchor for portal center
    void triggerPortalJump();     // short jump synced with portal transition

    // Shared render helpers for boot/home parity.
    void drawBootRamenPig(M5Canvas& canvas, int drawX, int drawY, uint32_t now, bool holdPose);
    void drawBootWardriveSceneBase(M5Canvas& canvas, uint32_t now, uint32_t introElapsedMs,
                                   float loadingT);
    void drawBootWardrivePigOverlay(M5Canvas& canvas, uint32_t now, uint32_t introElapsedMs);
    void getBootWardriveTeleportAnchors(int16_t& sourceX, int16_t& sourceY,
                                        int16_t& portalX, int16_t& portalY);
    uint32_t getBootWardriveSceneDurationMs();
    uint32_t getBootWardriveJumpStartMs();
    void drawTeleportPortalRing(M5Canvas& canvas, int cx, int cy, int radius);
    void sampleTeleportParticles(TeleportParticleSample* outParticles,
                                 uint8_t& outCount, uint8_t maxCount);
    void drawTeleportCollapseFrame(M5Canvas& canvas,
                                   int sourceCenterX, int sourceCenterY,
                                   int portalCenterX, int portalCenterY,
                                   float collapseT,
                                   const TeleportParticleSample* particles,
                                   uint8_t particleCount);

    // Rear cinematic — 5-sec pre-teleport comedy sequence
    bool isRearCinematicActive();
    bool isRearCinematicDone();
    void startRearCinematic();
    void clearRearCinematic();

    // Capture-only hooks for deterministic pig atlas export.
    void resetCaptureState();
    bool renderCaptureClip(M5Canvas& canvas, const char* clipId, uint32_t now,
                           const MenuPigRenderOptions& options);

    // DEFHOG4 DEFENSE: expose pig location for terminal overlay
    uint8_t getCurrentStation();   // Station enum as uint8_t
    uint8_t getCurrentRoom();      // room index 0-5
    uint8_t getCatBondTier();      // 0-4, derived from care and discoveries
    uint8_t getCatDossierCount();  // completed room routines, 0-6
    uint8_t getCatMasteryCount();  // completed behavior categories, 0-7
    const char* getCatCurrentRoutineName();
    bool forceDebugRoamingFrame(const char* frameId);  // temporary serial-driven screenshot preset
    bool isDebugRoamingFrameActive();
    void clearDebugRoamingFrame();

    void cleanupForModeExit();          // leaving MENU for a non-MENU mode

    // Wardrive mode trigger (menu flow teleports to Room 3)
    void startWardriveEntry();
    bool isWDCinematicRunning();
    void clearWDCinematic();
    void returnFromWardrive();    // exit WARDRIVE → restore pig at Room 3 AT_ANTENNA

    // Teleport-based wardrive exit: pre-select destination, then set up on VOID
    void prepareWardriveExitStation(float& dstCX, float& dstCY,
                                     bool& dstRear, bool& dstFaceRight);
    void returnFromWardriveViaTeleport();
}
