/**
 * FLOCKGRAPH local observation feed (Recon/XBand/GPS -> LSP-1).
 */
#pragma once

#include <stdint.h>

namespace NowFlockFeed {

void init();
void tick(uint32_t nowMs);

} // namespace NowFlockFeed
