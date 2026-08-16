/**
 * wardrive_pig_render.h — rear pig rendering for WARTHOG cockpit
 *
 * Extracted from wardrive_scene.cpp. Implements the over-shoulder pig
 * view: body mask, 2-sphere bump lighting, hair sway, tail curl,
 * leg nubs, wheel grip with IK arms, comms bubble, and neon rim.
 *
 * All functions in namespace WardriveScene.
 */
#pragma once

#include <M5Unified.h>
#include <stdint.h>

namespace WardriveScene {

struct PigPose;

void drawPigRear(M5Canvas& c, uint32_t now, float motion, const PigPose& pose);
void drawPigCommsBubble(M5Canvas& c, uint32_t now, float motion, const PigPose& pose);

} // namespace WardriveScene
