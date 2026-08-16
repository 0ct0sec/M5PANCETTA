/**
 * Hamlet Session - Extracted session state tracking
 *
 * ==[ SESSION PIGLET ]== step milestones, hunt time accumulation,
 * and hunt-time achievement checks. Previously inline in hamlet.cpp.
 *
 * Pure state + logic — no hardware dependencies. Fully testable.
 */
#pragma once

#include <cstdint>

namespace HamletSession {

// ==[ SESSION STATE ]== runtime tracking (resets on boot)
struct State {
    uint32_t lastMomentumStep = 0;         // last 100-step mood charge checkpoint
    uint32_t lastMilestoneRewarded = 0;    // graduated reward milestone
    uint32_t huntStart = 0;                // when current hunt/spectrum entered
    uint32_t huntAccumulated = 0;          // total ms in hunt/spectrum this session
    bool huntActive = false;               // true while in hunt/spectrum mode
    bool huntTimeMarked = false;           // 2-minute engagement already credited
};

// Reset session state (call on boot)
void reset(State& state);

// Called when entering a hunt/spectrum mode
void onHuntEnter(State& state, uint32_t now);

// Called when exiting a hunt/spectrum mode
void onHuntExit(State& state, uint32_t now);

// Get total hunt time in milliseconds (including current session if active)
uint32_t getHuntTimeMs(const State& state, uint32_t now);

// Get total hunt time in minutes
uint16_t getHuntTimeMinutes(const State& state, uint32_t now);

// ==[ STEP MILESTONES ]== graduated rewards
struct StepMilestone {
    uint32_t threshold;
    uint16_t xpReward;
};

// Pop one newly crossed milestone. Repeated calls drain a multi-threshold jump
// without collapsing its player-facing ceremonies into one aggregate payout.
bool popStepMilestone(State& state, uint32_t totalSteps, StepMilestone& out);

// First milestone strictly above totalSteps, or nullptr after the final tier.
// Used by R1B R4CK so its next-payout copy shares the runtime reward table.
const StepMilestone* nextStepMilestone(uint32_t totalSteps);

}  // namespace HamletSession
