#include "defense_fusion.h"

#include "recon_internal.h"
#include "xband.h"

namespace DefenseFusion {
// Fusion is the interview room: retained producers enter in established order,
// Recon updates private state, and XBand correlates only after that testimony
// is complete. Publication remains the pipeline's separate final act.
void fuseC5Monster(const C5Monster::ScanResults& results) {
    Recon::feedC5MonsterScan(results);
}

void fuseAcquired(uint32_t now) {
    Recon::fuseAcquiredObservations(now);
}

void correlate(uint32_t now) {
    Recon::updateFusion(now);
    XBand::update(now);
}
}  // namespace DefenseFusion
