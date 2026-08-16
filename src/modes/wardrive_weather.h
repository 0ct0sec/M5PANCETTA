/**
 * wardrive_weather.h — canopy rain/weather rendering for WARTHOG cockpit
 *
 * Exterior falling rain is drawn before cockpit structure. Adhered canopy
 * water is drawn after occupants/teleport, before curvature/HUD.
 */
#pragma once

#include "wardrive_shared.h"

namespace WardriveScene {

void drawExteriorRainMotion(M5Canvas& canvas, uint32_t now, float motion);
void drawCanopySurfaceWater(M5Canvas& canvas, uint32_t now, float motion);

} // namespace WardriveScene
