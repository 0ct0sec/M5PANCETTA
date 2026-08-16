/** Radio acquisition stage owned exclusively by DefensePipeline. */
#pragma once
#include <stdint.h>

namespace DefenseAcquisition {
bool update(uint32_t now);
void finalize(uint32_t now);
}
