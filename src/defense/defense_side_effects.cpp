#include "defense_side_effects.h"

#include "recon_internal.h"

namespace DefenseSideEffects {
void update(uint32_t now) { Recon::updateSideEffects(now); }
}  // namespace DefenseSideEffects
