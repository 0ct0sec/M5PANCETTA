/**
 * wardrive_city_backdrop.h — city skyline, tower grid, fog, traffic, comms bubble
 *
 * Extracted from wardrive_scene.cpp: world-space tower grid (PSRAM),
 * 3-layer parallax city, sky gradient, fog/haze, window lights, neon signs,
 * traffic headlights, air traffic volumetric beams, steam vents, searchlight,
 * horizon shadow pass, tower comms speech bubble, backdrop cache management.
 *
 * All functions operate on the glass region (WD_GLASS_T .. WD_GLASS_B).
 * PSRAM tower slots allocated in WardriveScene::reset(), freed in shutdown().
 */
#pragma once

#include "wardrive_shared.h"

namespace WardriveScene {

// ==[ CACHE MANAGEMENT ]==
bool prepareSceneBackdropCache();
bool isSceneBackdropCacheReady();
void releaseSceneBackdropCache();

// ==[ CITY PALETTE ]==
void updateCityPalette();

// ==[ TOWER GRID ]==
// Public wrapper: generates tower grid at given steer/pitch (uses internal full-param version)
int collectCityTowers(float steer, float pitch);

// ==[ RENDERING ]==
// The retained base owns sky, towers, haze, and structural shadows. Live
// motion restores time-driven emitters/effects after the base is copied.
void drawGlassBackdropBase(M5Canvas& c, uint32_t now, float motion);
void drawGlassBackdropMotion(M5Canvas& c, uint32_t now, float motion);
void drawGlassBackdrop(M5Canvas& c, uint32_t now, float motion);
void drawHorizonShadowPass(M5Canvas& c, uint32_t now, int horizonY);

} // namespace WardriveScene
