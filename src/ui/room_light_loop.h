/**
 * room_light_loop.h — deterministic four-beat room practical animation
 *
 * Kept free of display dependencies so the timing, signed drift, and room
 * fallback contract can be exercised by the native test suite.
 */
#pragma once

#include <stdint.h>

namespace MenuPigRender {

constexpr int8_t kRoomLightLoopCellPx = 4;
constexpr uint16_t kVolumetricLightLoopPeriodMs = 1680;

struct RoomLightLoopFrame {
    uint8_t energy = 0;
    uint8_t reflection = 0;
    int8_t driftX = 0;
    uint8_t phase = 0;
    uint8_t phaseBlend = 0;
    // Normalized 0..256 out-and-back travel. Unlike the raw four-beat phase,
    // this closes at the same edge and cannot teleport when the loop wraps.
    uint16_t sweepQ8 = 0;
};

RoomLightLoopFrame sampleRoomLightLoop(uint8_t room, uint32_t now);
RoomLightLoopFrame sampleVolumetricLightLoop(uint32_t now);

} // namespace MenuPigRender
