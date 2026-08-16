/**
 * Wardrive telemetry tape — low-power fullscreen navigation and scan evidence.
 *
 * The tape is a view over the live Wardrive engine. It never pauses radio,
 * GPS, BLE, or SD custody, so returning to the cockpit resumes the same case.
 */
#pragma once

#include <M5Unified.h>

namespace WardriveTelemetry {

void reset();
void shutdown();
void update(uint32_t nowMs);
void draw(M5Canvas& canvas, uint32_t nowMs);

bool isVisible();
void setVisible(bool visible);
void toggle();

} // namespace WardriveTelemetry
