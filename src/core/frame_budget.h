/**
 * Frame Budget — cooperative main-loop time governor
 *
 * ==[ SLICE ]== tracks per-frame elapsed time; gates non-critical work.
 */

#pragma once
#include <Arduino.h>

namespace FrameBudget {

    void beginFrame(uint32_t now);
    void endFrame();
    void paceFrame();
    bool hasTime(uint32_t budgetMs);
    void notePhase(const char* tag);
    uint32_t lastFrameMs();
    uint32_t lastFrameUs();
    uint32_t frameElapsedMs();
    void recordOverrun();

#ifdef HAMLET_FRAME_PROFILE
    void printStats();
    void printRoomStats();
    void resetStats();
    void setProfileRoom(uint8_t room);
    void setProfiling(bool on);
    bool isProfiling();
#endif

} // namespace FrameBudget
