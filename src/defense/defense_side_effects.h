/** Alert/chaff side-effect stage, sequenced after event-producing fusion. */
#pragma once
#include <stdint.h>

namespace DefenseSideEffects {
void update(uint32_t now);
}
