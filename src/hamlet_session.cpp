/**
 * Hamlet Session - Session state tracking implementation
 *
 * ==[ SESSION PIGLET ]== pure logic, no hardware.
 */

#include "hamlet_session.h"

namespace HamletSession {

void reset(State& state) {
    state.lastMomentumStep = 0;
    state.lastMilestoneRewarded = 0;
    state.huntStart = 0;
    state.huntAccumulated = 0;
    state.huntActive = false;
    state.huntTimeMarked = false;
}

void onHuntEnter(State& state, uint32_t now) {
    state.huntStart = now;
    state.huntActive = true;
}

void onHuntExit(State& state, uint32_t now) {
    if (state.huntActive) {
        uint32_t delta = now - state.huntStart;
        // Cap at 24h to prevent millis() wraparound corruption (~49 day cycle)
        if (delta > 86400000UL) delta = 0;
        state.huntAccumulated += delta;
        state.huntStart = 0;
        state.huntActive = false;
    }
}

uint32_t getHuntTimeMs(const State& state, uint32_t now) {
    uint32_t total = state.huntAccumulated;
    if (state.huntActive) {
        uint32_t delta = now - state.huntStart;
        if (delta <= 86400000UL) total += delta;  // ignore if wraparound
    }
    return total;
}

uint16_t getHuntTimeMinutes(const State& state, uint32_t now) {
    return (uint16_t)(getHuntTimeMs(state, now) / 60000);
}

// ==[ STEP MILESTONES ]== the single runtime/test/UI reward contract
static const StepMilestone MILESTONES[] = {
    {  1000,  30 },
    {  5000,  50 },
    { 10000,  70 },
    { 20000,  80 },
    { 30000, 100 },
};
static constexpr int MILESTONE_COUNT = sizeof(MILESTONES) / sizeof(MILESTONES[0]);

bool popStepMilestone(State& state, uint32_t totalSteps, StepMilestone& out) {
    for (int i = 0; i < MILESTONE_COUNT; i++) {
        if (totalSteps >= MILESTONES[i].threshold &&
            state.lastMilestoneRewarded < MILESTONES[i].threshold) {
            state.lastMilestoneRewarded = MILESTONES[i].threshold;
            out = MILESTONES[i];
            return true;
        }
    }
    return false;
}

const StepMilestone* nextStepMilestone(uint32_t totalSteps) {
    for (int i = 0; i < MILESTONE_COUNT; i++) {
        if (MILESTONES[i].threshold > totalSteps) return &MILESTONES[i];
    }
    return nullptr;
}

}  // namespace HamletSession
