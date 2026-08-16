/**
 * wardrive_cockpit_shell.h — cockpit shell, dash, chair, yoke, scanlines, hints
 *
 * Extracted from wardrive_scene.cpp: the structural cockpit frame (canopy glass,
 * pillars, roof, dashboard), console layer, monitors, steering wheel (butterfly
 * yoke), chair back/headrest/seat base/rails, dash reflections, CRT scanlines,
 * and dash hint labels.
 */
#pragma once

#include <M5Unified.h>
#include "wardrive_shared.h"

namespace WardriveScene {

// ==[ PANEL LIGHTING ]==
// Directional light on frame borders — used by shell, yoke, HUD panes.
uint16_t litFrame(uint16_t frame, uint16_t glow, int px, int py, int cx, int cy, float halfW, float halfH);

// ==[ CHAIR SETTLE ]==
// Damped spring offset when pig teleports into seat.
float getChairSettleOffset(uint32_t now);

// ==[ LAYER 1: STRUCTURAL ]==
// Canopy glass, A-pillars, roof, dashboard body, instrument bridge.
void drawCockpitShell(M5Canvas& c, uint32_t now, float motion);

// ==[ LAYER 3: CONSOLE ]==
// Console electronics (conduits, modules, patch panels, cable coils).
void drawConsoleLayer(M5Canvas& c, uint32_t now);

// ==[ LAYER 3: CABLES ]==
// Cable runs behind glass (optional, off by default).
void drawDashboardCables(M5Canvas& c, uint32_t now, float motion);

// ==[ LAYER 3: MONITORS ]==
// Auxiliary monitor on dash (speed readout).
void drawDashboardMonitors(M5Canvas& c, uint32_t now);

// ==[ LAYER 3: YOKE ]==
// Butterfly flight stick with IMU rotation.
void drawSteeringWheel(M5Canvas& c, int x, int y, int w, int h);

// ==[ LAYER 3: CHAIR ]==
// Racing seat — headrest, back, bolsters, base, rails.
void drawChairPass(M5Canvas& c, const PigPose& pose, bool isForeground);

// ==[ LAYER 4: REFLECTIONS ]==
// Neon/amber reflection streaks on dash and glass.
void drawDashReflections(M5Canvas& c, uint32_t now, float motion);

// ==[ LAYER 4: SCANLINES ]==
// Horizontal CRT scanlines over dashboard.
void drawCRTScanlines(M5Canvas& c);

// ==[ LAYER 7: HINTS ]==
// Control label hints etched into bottom dashboard.
void drawDashHints(M5Canvas& c, uint32_t now);

} // namespace WardriveScene
