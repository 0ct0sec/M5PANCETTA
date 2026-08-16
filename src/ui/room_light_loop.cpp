/** room_light_loop.cpp — authored room practical timing */

#include "room_light_loop.h"

namespace MenuPigRender {

static RoomLightLoopFrame sampleFourBeatLoop(
        uint32_t loopNow, uint32_t period,
        const uint8_t energy[4], const uint8_t reflection[4],
        const int8_t drift[4]) {
    const uint16_t phaseQ10 = (uint16_t)((loopNow * 1024u) / period);
    const uint8_t phase = (uint8_t)(phaseQ10 >> 8);
    const uint8_t next = (uint8_t)((phase + 1u) & 3u);
    const uint8_t frac = (uint8_t)(phaseQ10 & 0xFFu);
    auto lerpU8 = [frac](uint8_t a, uint8_t b) -> uint8_t {
        return (uint8_t)((int)a + (((int)b - (int)a) * (int)frac) / 255);
    };

    RoomLightLoopFrame frame;
    frame.energy = lerpU8(energy[phase], energy[next]);
    frame.reflection = lerpU8(reflection[phase], reflection[next]);

    // Interpolate before snapping to the 4px room grid. Keeping this signed is
    // essential: the negative leg is a real leftward drift, not uint8_t 252.
    const int driftQ8 = (int)drift[phase] * 255 +
        ((int)drift[next] - (int)drift[phase]) * (int)frac;
    constexpr int kCellQ8 = kRoomLightLoopCellPx * 255;
    constexpr int kHalfCellQ8 = kCellQ8 / 2;
    const int driftCells = driftQ8 >= 0
        ? (driftQ8 + kHalfCellQ8) / kCellQ8
        : -((-driftQ8 + kHalfCellQ8) / kCellQ8);
    frame.driftX = (int8_t)(driftCells * kRoomLightLoopCellPx);
    frame.phase = phase;
    frame.phaseBlend = frac;
    frame.sweepQ8 = phaseQ10 <= 512u
        ? (uint16_t)(phaseQ10 / 2u)
        : (uint16_t)((1024u - phaseQ10) / 2u);
    return frame;
}

RoomLightLoopFrame sampleRoomLightLoop(uint8_t room, uint32_t now) {
    static constexpr uint16_t kPeriodMs[6] = {
        3200, 4200, 3600, 5200, 3000, 4000
    };
    static constexpr uint8_t kEnergy[6][4] = {
        {58, 82, 100, 74},  // lab CRT cone
        {46, 74, 100, 62},  // apartment blind slats
        {62, 84, 100, 78},  // ramen sign + steam
        {34, 48, 100, 42},  // rooftop storm air
        {52, 80, 100, 72},  // bar karaoke + neon
        {56, 76, 100, 84},  // bath steam + caustics
    };
    static constexpr uint8_t kReflection[6][4] = {
        {34, 46, 70, 52}, {28, 44, 76, 58}, {42, 58, 82, 66},
        {22, 30, 92, 44}, {38, 54, 86, 70}, {44, 62, 88, 78},
    };
    static constexpr int8_t kDrift[6][4] = {
        {-4, 0, 4, 0}, {-4, -4, 4, 0}, {-4, 0, 4, 0},
        {-4, 0, 4, 0}, {-4, 4, 4, 0}, {-4, 0, 4, 0},
    };

    if (room >= 6) room = 0;
    const uint32_t period = kPeriodMs[room];
    const uint32_t loopNow = (now + (uint32_t)room * 317u) % period;
    return sampleFourBeatLoop(loopNow, period, kEnergy[room],
                              kReflection[room], kDrift[room]);
}

RoomLightLoopFrame sampleVolumetricLightLoop(uint32_t now) {
    static constexpr uint8_t kEnergy[4] = {76, 100, 116, 92};
    static constexpr int8_t kDrift[4] = {-4, 0, 4, 0};
    const uint32_t loopNow = now % kVolumetricLightLoopPeriodMs;
    return sampleFourBeatLoop(loopNow, kVolumetricLightLoopPeriodMs,
                              kEnergy, kEnergy, kDrift);
}

} // namespace MenuPigRender
