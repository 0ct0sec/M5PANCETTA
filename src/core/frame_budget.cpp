/**
 * Frame Budget — cooperative main-loop time governor
 */

#include "frame_budget.h"
#include "power.h"
#include <string.h>

namespace FrameBudget {

static uint32_t frameStartMs = 0;
static uint32_t frameStartUs = 0;
static uint32_t previousFrameStartUs = 0;
static uint32_t lastFrameDurationMs = 0;
static uint32_t lastFrameWorkUs = 0;
static uint32_t lastFramePeriodUs = 0;
static uint32_t frameBudgetMs = 67;
static uint32_t frameBudgetUs = 66667;
static uint32_t overrunCount = 0;

#ifdef HAMLET_FRAME_PROFILE
static bool profiling = false;
static char lastPhase[24] = "init";
static uint32_t phaseMarkUs = 0;
static uint32_t lastPhaseUs = 0;
static uint8_t profileRoom = 0xFFu;
static uint64_t totalWorkUs = 0;
static uint64_t totalPeriodUs = 0;
static uint32_t workSamples = 0;
static uint32_t periodSamples = 0;
static uint32_t maxWorkUs = 0;

struct RoomProfileStats {
    uint64_t totalWorkUs = 0;
    uint64_t totalPeriodUs = 0;
    uint32_t workSamples = 0;
    uint32_t periodSamples = 0;
    uint32_t maxWorkUs = 0;
    uint32_t overruns = 0;
};

static RoomProfileStats roomStats[6];
#endif

void beginFrame(uint32_t now) {
    const uint32_t startUs = micros();
    if (previousFrameStartUs != 0) {
        lastFramePeriodUs = startUs - previousFrameStartUs;
#ifdef HAMLET_FRAME_PROFILE
        if (profiling) {
            totalPeriodUs += lastFramePeriodUs;
            ++periodSamples;
            // profileRoom still names the frame whose paced period just ended.
            if (profileRoom < 6u) {
                RoomProfileStats& stats = roomStats[profileRoom];
                stats.totalPeriodUs += lastFramePeriodUs;
                ++stats.periodSamples;
            }
        }
#endif
    }
    previousFrameStartUs = startUs;
    frameStartMs = now;
    frameStartUs = startUs;
    const uint8_t fps = Power::getTargetFPS();
    frameBudgetMs = (fps > 0) ? (1000u / fps) : 67u;
    frameBudgetUs = (fps > 0) ? ((1000000u + fps - 1u) / fps) : 66667u;
#ifdef HAMLET_FRAME_PROFILE
    phaseMarkUs = startUs;
    // Keep the previous completed phase visible until this frame records its
    // first phase. Serial commands are polled before notePhase("sd").
    profileRoom = 0xFFu;
#endif
}

void endFrame() {
    if (frameStartUs == 0) return;
    lastFrameWorkUs = micros() - frameStartUs;
    lastFrameDurationMs = (lastFrameWorkUs + 999u) / 1000u;
    const bool overBudget = lastFrameWorkUs > frameBudgetUs;
    if (overBudget) ++overrunCount;
#ifdef HAMLET_FRAME_PROFILE
    if (profiling) {
        totalWorkUs += lastFrameWorkUs;
        ++workSamples;
        if (lastFrameWorkUs > maxWorkUs) maxWorkUs = lastFrameWorkUs;
        if (profileRoom < 6u) {
            RoomProfileStats& stats = roomStats[profileRoom];
            stats.totalWorkUs += lastFrameWorkUs;
            ++stats.workSamples;
            if (lastFrameWorkUs > stats.maxWorkUs) stats.maxWorkUs = lastFrameWorkUs;
            if (overBudget) ++stats.overruns;
        }
    }
#endif
}

void paceFrame() {
    if (frameStartUs == 0) return;

    uint32_t elapsedUs = micros() - frameStartUs;
    if (elapsedUs >= frameBudgetUs) return;

    uint32_t remainingUs = frameBudgetUs - elapsedUs;
    // Yield most of the slack to FreeRTOS, then use the microsecond clock for
    // the fractional millisecond. This keeps 60fps at 16.667ms instead of the
    // old integer 16ms / 62.5fps cadence and avoids cumulative phase drift.
    if (remainingUs > 2000u) {
        const uint32_t sleepMs = (remainingUs - 1000u) / 1000u;
        if (sleepMs > 0u) vTaskDelay(sleepMs / portTICK_PERIOD_MS);
    }

    elapsedUs = micros() - frameStartUs;
    if (elapsedUs < frameBudgetUs) {
        delayMicroseconds(frameBudgetUs - elapsedUs);
    }
}

bool hasTime(uint32_t budgetMs) {
    if (frameStartMs == 0) return true;
    return (millis() - frameStartMs) + budgetMs <= frameBudgetMs;
}

void notePhase(const char* tag) {
#ifdef HAMLET_FRAME_PROFILE
    if (!tag) return;
    const uint32_t now = micros();
    lastPhaseUs = now - phaseMarkUs;
    phaseMarkUs = now;
    strncpy(lastPhase, tag, sizeof(lastPhase) - 1);
    lastPhase[sizeof(lastPhase) - 1] = '\0';
#else
    (void)tag;
#endif
}

uint32_t lastFrameMs() {
    return lastFrameDurationMs;
}

uint32_t lastFrameUs() {
    return lastFrameWorkUs;
}

uint32_t frameElapsedMs() {
    if (frameStartMs == 0) return 0;
    return millis() - frameStartMs;
}

void recordOverrun() {
    ++overrunCount;
}

#ifdef HAMLET_FRAME_PROFILE
static uint32_t rateTenths(uint64_t elapsedUs, uint32_t samples) {
    if (elapsedUs == 0 || samples == 0) return 0;
    return (uint32_t)(((uint64_t)samples * 10000000ull) / elapsedUs);
}

void printStats() {
    const uint32_t avgWork = workSamples > 0
        ? (uint32_t)(totalWorkUs / workSamples) : 0u;
    const uint32_t actualFps10 = rateTenths(totalPeriodUs, periodSamples);
    const uint32_t capacityFps10 = rateTenths(totalWorkUs, workSamples);
    Serial.printf("[FRAME] work=%luus avg=%luus max=%luus budget=%luus "
                  "period=%luus actual=%lu.%lufps capacity=%lu.%lufps "
                  "overruns=%lu phase=%s/%luus\n",
                  (unsigned long)lastFrameWorkUs,
                  (unsigned long)avgWork,
                  (unsigned long)maxWorkUs,
                  (unsigned long)frameBudgetUs,
                  (unsigned long)lastFramePeriodUs,
                  (unsigned long)(actualFps10 / 10u),
                  (unsigned long)(actualFps10 % 10u),
                  (unsigned long)(capacityFps10 / 10u),
                  (unsigned long)(capacityFps10 % 10u),
                  (unsigned long)overrunCount,
                  lastPhase,
                  (unsigned long)lastPhaseUs);
}

void printRoomStats() {
    const unsigned targetFps = Power::getTargetFPS();
    for (uint8_t room = 0; room < 6u; ++room) {
        const RoomProfileStats& stats = roomStats[room];
        if (stats.workSamples == 0 && stats.periodSamples == 0) {
            Serial.printf("[ROOMFPS] room=%u no-samples\n", room);
            continue;
        }
        const uint32_t avgWork = stats.workSamples > 0
            ? (uint32_t)(stats.totalWorkUs / stats.workSamples) : 0u;
        const uint32_t actualFps10 = rateTenths(stats.totalPeriodUs,
                                                stats.periodSamples);
        const uint32_t capacityFps10 = rateTenths(stats.totalWorkUs,
                                                  stats.workSamples);
        Serial.printf("[ROOMFPS] room=%u nowTarget=%ufps samples=%lu avg=%luus max=%luus "
                      "actual=%lu.%lufps capacity=%lu.%lufps misses=%lu\n",
                      room,
                      targetFps,
                      (unsigned long)stats.workSamples,
                      (unsigned long)avgWork,
                      (unsigned long)stats.maxWorkUs,
                      (unsigned long)(actualFps10 / 10u),
                      (unsigned long)(actualFps10 % 10u),
                      (unsigned long)(capacityFps10 / 10u),
                      (unsigned long)(capacityFps10 % 10u),
                      (unsigned long)stats.overruns);
    }
}

void resetStats() {
    totalWorkUs = 0;
    totalPeriodUs = 0;
    workSamples = 0;
    periodSamples = 0;
    maxWorkUs = 0;
    overrunCount = 0;
    memset(roomStats, 0, sizeof(roomStats));
}

void setProfileRoom(uint8_t room) {
    profileRoom = room < 6u ? room : 0xFFu;
}

void setProfiling(bool on) {
    profiling = on;
}

bool isProfiling() {
    return profiling;
}
#endif

} // namespace FrameBudget
