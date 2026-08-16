/**
 * sky_volume.h — the thing above the rooftop
 *
 * The rooftop's open sky was 104px × 320px of per-cell hash noise: every cell
 * rolled a threshold against a broad world grid, so the result was a field of
 * grain that ping-ponged left and right forever. It had no masses, no depth,
 * no light and nothing that ever happened in it. Standing on a roof in this
 * city and looking up got you television static.
 *
 * This module owns what replaces it, and it is stateful for the same reason
 * SmokeFx is: the interesting things in a sky have a beginning and an end, and
 * a pure function of `now` cannot have those. An airship crossing is an event.
 *
 * WHAT LIVES HERE
 *   deck        Three parallax bands of real cloud masses with flat bases and
 *               ragged tops. Lit from BELOW — this city throws enough light at
 *               its own overcast that the base of every cloud carries a sodium
 *               rim and the top of it is the darkest thing on screen. Thunder
 *               inverts that: the deck goes backlit and the rim jumps to the
 *               top edge.
 *   spinners    Traffic, at altitude, in lanes. Small, fast, strobing. They
 *               are what gives the deck a scale — a cloud with a car under it
 *               is a cloud, a cloud alone is a texture.
 *   airship     The ad blimp. Rare, slow, enormous, with a holo panel running
 *               down its flank and a searchlight it rakes across the roof. It
 *               is the one thing up there you are meant to stop and watch.
 *
 * The searchlight is published back out through sampleSearchlight() so the
 * room can key Pancetta off it: when the beam crosses her she is lit by the
 * blimp, which is the entire point of putting a blimp over a rooftop.
 *
 * PIXEL SIZING — same contract as every other effect in this project
 * (PIXEL_ART_ARCHITECTURE.md §2.3): every cell written here is kRoomPX on the
 * room lattice, and the cell size is not a caller parameter. Sky is scenery.
 *
 * OCCLUSION: the deck only writes cells that are currently sky. The rooftop's
 * architecture is drawn in the retained pass and this runs over the top of it,
 * so a cloud can never eat the antenna mast or the shack.
 */
#pragma once

#include <M5Unified.h>
#include <stdint.h>

namespace SkyFx {

struct Params {
    int x = 0, y = 0, w = 0, h = 0;  // open-sky viewport, room lattice
    // Where the searchlight lands. The beam runs past the bottom of the cloud
    // viewport down to the deck plate, so it needs its own ground plane.
    int groundY = 0;
    int8_t parallaxX = 0;            // camera; bands scale it by depth
    bool thunder = false;
    bool tintActive = false;
    uint16_t tintColor565 = 0;
    uint8_t tintIntensity = 0;
};

// ==[ LIFECYCLE ]==
// update() advances the airship schedule. Call once per rendered frame before
// any draw. Rooms are hard cuts: an airship may not survive a teleport.
void update(uint32_t now);
void setScene(uint32_t sceneKey);
void clear();

// Deck + spinners + airship + its beam, in depth order. One call.
void drawSky(M5Canvas& canvas, uint32_t now, const Params& p);

// Where the searchlight lands. Composited with the roof, not with the sky, so
// it is a separate call — the beam is behind the rooftop hardware and the pool
// it throws is in front of the deck plate.
void drawGroundPool(M5Canvas& canvas, uint32_t now, const Params& p,
                    int groundY);

// True while the beam is on. outX/outY are the ground impact point and outS is
// 0..255 falloff, for use as a room key light.
bool sampleSearchlight(int& outX, int& outY, uint8_t& outStrength);

} // namespace SkyFx
