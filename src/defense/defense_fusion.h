/** Deterministic, side-effect-free correlation stage. */
#pragma once
#include <stdint.h>

namespace C5Monster { struct ScanResults; }

namespace DefenseFusion {
void fuseC5Monster(const C5Monster::ScanResults& results);
void fuseAcquired(uint32_t now);
void correlate(uint32_t now);
}
