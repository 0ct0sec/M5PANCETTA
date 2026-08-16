#pragma once

#include <stdint.h>

// CoreS3 SE bath-only sound reaction. The feature deliberately owns the shared
// audio bus only while Pancetta is settled in the tub; it retains no samples.
namespace BathMic {

// Called once per frame after the menu state machine has updated.
void update(bool bathEligible, uint32_t now);

// True while the I2S microphone owns the shared CoreS3 SE audio bus.
bool isAudioBusReserved();

// True after at least two plausible energy onsets establish a current beat.
bool isDanceActive();

// Eight grid-animation keys per detected beat. The phase is re-anchored by
// microphone energy peaks so the bath dance follows the room instead of a
// separate wall-clock loop.
uint8_t danceBeatPhase(uint32_t now);

}  // namespace BathMic
