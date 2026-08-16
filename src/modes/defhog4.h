/**
 * Defhog4 — full-screen defense terminal mode
 *
 * ==[ D3FH0G4 DEFENSE TERMINAL ]== interactive command center.
 * 5 panes: SITREP / SIGINT / BLE / FUSION / LOG.
 * real-time updating, scroll history, alert interrupts, smart entry.
 * consumes Recon + XBand + Hunt + Capture data. zero radio ownership.
 */
#pragma once

#include <M5Unified.h>

namespace Defhog4 {

// ==[ MODE LIFECYCLE ]==
void enter();
void exit();
void update();
void draw(M5Canvas& canvas);

// ==[ INPUT ]==
void nextPane();
void prevPane();
void action();              // button B short: pane-dependent (HUNT/SPECTRUM/BLE_SCANNER/refresh)
void scrollUp();
void scrollDown();
void onTap(int16_t x, int16_t y);

// ==[ ALERTS ]==
void triggerInterrupt(const char* msg, uint32_t durationMs = 2500);

}  // namespace Defhog4
