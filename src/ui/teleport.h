/**
 * teleport.h — unified particle decompose/reassemble teleport system
 *
 * one teleport to rule them all. pig shatters into 64 rainbow particles,
 * spirals through portal, reassembles at destination. used for:
 *   - room-to-room (menu pig)
 *   - cross-mode (IDLE<->MENU<->HUNT)
 *   - boot arrival (REASSEMBLE+SETTLE only)
 */
#pragma once

#include <M5Unified.h>
#include <stdint.h>

// forward-declare to avoid circular includes
namespace MenuPig {
    struct TeleportParticleSample;
}

namespace Teleport {

// ==[ PHASES ]== particle lifecycle
enum class Phase : uint8_t {
    NONE = 0,
    DECOMPOSE,    // pig -> particles spiral to portal
    VOID,         // portal pulses alone, context switch
    REASSEMBLE,   // particles spiral from portal -> pig shape
    SETTLE        // jitter-snap into final positions
};

// ==[ TIMING ]== milliseconds per phase
static constexpr uint32_t DECOMPOSE_MS  = 800;
static constexpr uint32_t VOID_MS       = 150;
static constexpr uint32_t REASSEMBLE_MS = 600;
static constexpr uint32_t SETTLE_MS     = 150;
static constexpr uint32_t TOTAL_MS      = DECOMPOSE_MS + VOID_MS + REASSEMBLE_MS + SETTLE_MS;
static constexpr int PORTAL_RING_R      = 20;
static constexpr int MAX_PARTICLES      = 64;
// The companion's 52x28 footprint resolves to 13x7 four-pixel cells, of which
// 48 are solid. Budgeting exactly that many keeps his whole shape in the beam.
static constexpr int MAX_CAT_PARTICLES  = 48;

// Particle homes preserve the pose that owns each end of a cross-mode jump.
enum class PigSilhouette : uint8_t {
    SIDE_RIGHT = 0,
    SIDE_LEFT,
    REAR
};

// ==[ CONTEXT ]== what triggered this teleport
enum class Context : uint8_t {
    ROOM_TO_ROOM = 0,   // menu pig room transition (internal to menu_pig)
    HELPER_TO_ROOM,     // bench -> roaming mode (internal to menu_pig)
    IDLE_TO_MENU,       // cross-mode: IDLE -> MENU
    MENU_TO_HUNT,       // cross-mode: MENU -> HUNT
    MENU_TO_WARDRIVE,   // cross-mode: MENU -> WARDRIVE (car roof -> cockpit)
    WARDRIVE_TO_MENU,   // cross-mode: WARDRIVE -> MENU (cockpit -> random room)
    BOOT_ARRIVAL        // boot -> IDLE (REASSEMBLE+SETTLE only)
};

// ==[ PARTICLE SAMPLING ]== extract pig body shape into particles
void samplePigParticles(MenuPig::TeleportParticleSample* out,
                        uint8_t& count, uint8_t maxCount,
                        PigSilhouette silhouette = PigSilhouette::SIDE_RIGHT);

// The companion rides the same portal, so he needs his own shape inside it.
// A scaled pig outline at this cell size just reads as a smaller pig; ears, a
// raised tail and two leg pairs are what make the beam read as the cat.
void sampleCatParticles(MenuPig::TeleportParticleSample* out,
                        uint8_t& count, uint8_t maxCount, bool faceRight);

// ==[ CROSS-MODE TRANSITIONS ]== driven by hamlet.cpp

// full DECOMPOSE -> VOID -> REASSEMBLE -> SETTLE sequence
void startCrossMode(Context ctx,
                    float srcCenterX, float srcCenterY,
                    float portalX, float portalY,
                    float dstCenterX, float dstCenterY,
                    uint32_t now,
                    PigSilhouette source = PigSilhouette::SIDE_RIGHT,
                    PigSilhouette destination = PigSilhouette::SIDE_RIGHT);

// REASSEMBLE -> SETTLE only (boot arrival — pig materializes from nothing)
void startBootArrival(float portalX, float portalY,
                      float dstCenterX, float dstCenterY,
                      uint32_t now,
                      PigSilhouette destination = PigSilhouette::SIDE_RIGHT);

// tick state machine — call every frame from hamlet.cpp update()
Phase update(uint32_t now);

// ==[ QUERIES ]==
bool isActive();
Phase getPhase();
Context getContext();

// should the destination pig be hidden? (decompose late + void + reassemble early)
bool isPigHidden();

// fires once at VOID boundary — caller must enterMode() when this returns true
bool consumeVoidReady();

// ==[ RENDERING ]== draw particles onto canvas

// full cross-mode teleport effect (dispatches to current phase)
void draw(M5Canvas& canvas, uint16_t fg, uint16_t bg, uint32_t now);

// charge ring indicator (A-hold progress, drawn as display overlay)
void drawChargeRing(int16_t cx, int16_t cy, float progress);

// ==[ SHARED RENDERERS ]== used by menu_pig.cpp room-to-room + boot intro

void drawPortalRing(M5Canvas& canvas, int cx, int cy, int radius);

// One portal serves every body that goes through it, so a second actor in the
// same jump passes withPortalRing = false and lets the primary pass own the
// ring. Two passes would paint the identical ring twice for nothing.
void drawDecomposeParticles(M5Canvas& canvas,
                            float srcCenterX, float srcCenterY,
                            float portalCenterX, float portalCenterY,
                            float t,
                            const MenuPig::TeleportParticleSample* particles,
                            uint8_t particleCount,
                            uint16_t fg, uint16_t bg,
                            bool withPortalRing = true);

void drawReassembleParticles(M5Canvas& canvas,
                             float portalCenterX, float portalCenterY,
                             int destCenterX, int destCenterY,
                             float t,
                             const MenuPig::TeleportParticleSample* particles,
                             int particleCount,
                             uint16_t fg, uint16_t bg,
                             bool withPortalRing = true);

void drawSettleParticles(M5Canvas& canvas,
                         int destCenterX, int destCenterY,
                         float t,
                         const MenuPig::TeleportParticleSample* particles,
                         int particleCount,
                         uint16_t fg);

void drawVoidPortal(M5Canvas& canvas, int portalX, int portalY, float t);

}  // namespace Teleport
