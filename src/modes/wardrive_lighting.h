/**
 * wardrive_lighting.h — cockpit lighting passes for WARTHOG
 *
 * Three canopy/glass lighting effects extracted from wardrive_scene.cpp:
 *   drawCockpitLighting()  — lightning + traffic flare on dash
 *   drawWindshieldCurvature() — canopy curvature arcs + crown glow
 *   drawCanopyStreaks()    — diagonal rain streak reflections on glass
 */
#pragma once

#include "wardrive_shared.h"

namespace WardriveScene {

void drawCockpitLighting(M5Canvas& c, const PigPose& pose);
void drawWindshieldCurvature(M5Canvas& c, uint32_t now, float motion);
void drawCanopyStreaks(M5Canvas& c, uint32_t now, float motion);

} // namespace WardriveScene
