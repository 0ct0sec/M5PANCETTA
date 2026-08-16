#pragma once

#include <stdint.h>

// Shared silhouette source for Detective Pancetta and the half-scale cat.
// Renderers choose their own cell size, face, and appendages; these row spans
// remain the single authored body shape.
namespace PancettaBodyMask {

static constexpr int kCols = 18;
static constexpr int kRows = 10;
static constexpr int8_t kRowLeft[kRows] = {
    -1, 3, 4, 1, 1, 1, 2, 1, 1, 1,
};
static constexpr int8_t kRowRight[kRows] = {
    -1, 14, 13, 16, 16, 16, 15, 16, 16, 16,
};

}  // namespace PancettaBodyMask
