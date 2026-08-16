#include "defense_acquisition.h"

#include "recon_internal.h"

namespace DefenseAcquisition {
// Acquisition may question the radio and fill bounded work tables. It cannot
// classify the testimony, admit an event, or publish a consumer snapshot.
bool update(uint32_t now) { return Recon::updateAcquisition(now); }
void finalize(uint32_t now) { Recon::finalizeAcquisition(now); }
}  // namespace DefenseAcquisition
