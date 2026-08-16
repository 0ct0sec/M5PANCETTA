/**
 * Shared constants — single source of truth.
 *
 * ==[ GOSPEL ]== numbers used by multiple modules. one definition, no drift.
 */
#pragma once

// ==[ PEDOMETER ]== step → distance/calories conversion
constexpr float STEP_LENGTH_M = 0.75f;       // 75cm gait estimate
constexpr float CALORIES_PER_STEP = 0.04f;   // broscience. close enough.
