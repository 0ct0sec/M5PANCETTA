/**
 * wardrive_hud.h — HUD telemetry, comms bubbles, attitude indicator
 *
 * Extracted from wardrive_scene.cpp. Contains tower comms state machine,
 * telemetry text formatting, attitude indicator, and speed helpers.
 *
 * tower comms state variables are DEFINED here (not just extern).
 * All functions live in namespace WardriveScene.
 */
#pragma once

#include <M5Unified.h>

namespace WardriveScene {

// ==[ SPEED HELPERS ]==
float getEffectSpeedKmh();
float getDisplaySpeedKmh();
float getMotion();

// ==[ TOWER COMMS ]==
void  generateCallsign(bool isRecon);
bool  generateReconTowerMsg(char* buf, size_t bufLen);
void  generateTowerMsg(char* buf, size_t bufLen);
void  generatePigMsg(char* buf, size_t bufLen);
void  updateTowerComms(uint32_t now);

// ==[ HUD RENDERING ]==
void sampleAttitudeDegrees(float& rollDeg, float& pitchDeg);
void drawAttitudeIndicator(M5Canvas& c);
void drawTelemetryText(M5Canvas& c, uint32_t now);

} // namespace WardriveScene
