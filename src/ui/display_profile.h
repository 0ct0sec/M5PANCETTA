#pragma once
/**
 * DisplayProfile — panel vs world geometry.
 *
 * SCREEN_WIDTH/HEIGHT stay 320x240 (each room tile / Core panel).
 * Tab5 adds a landscape panel, a 2x3 house, and a chrome pane.
 */
#include "display.h"
#include "../hal/platform.h"

namespace DisplayProfile {

static constexpr int kTileW = SCREEN_WIDTH;   // 320
static constexpr int kTileH = SCREEN_HEIGHT;  // 240

#if HAMLET_TARGET_TAB5
static constexpr int kPanelLogicalW = 1280;
static constexpr int kPanelLogicalH = 720;
static constexpr int kPanelPhysicalW = 720;
static constexpr int kPanelPhysicalH = 1280;
static constexpr int kHouseCols = 2;
static constexpr int kHouseRows = 3;
static constexpr int kHouseW = kHouseCols * kTileW;  // 640
static constexpr int kHouseH = kHouseRows * kTileH;  // 720
static constexpr int kHouseX = 0;
static constexpr int kHouseY = 0;
static constexpr int kChromeX = kHouseW;             // 640
static constexpr int kChromeY = 0;
static constexpr int kChromeW = kPanelLogicalW - kHouseW;  // 640
static constexpr int kChromeH = kPanelLogicalH;            // 720
static constexpr int kChromeStatusH = 48;
static constexpr int kChromeFooterH = 48;
static constexpr int kMinHit = 48;
static constexpr int kToolScale = 2;
static constexpr int kToolW = kTileW * kToolScale;   // 640
static constexpr int kToolH = kTileH * kToolScale;   // 480
static constexpr int kToolX = kChromeX;
static constexpr int kToolY = kChromeStatusH;        // 48
static constexpr bool kAlwaysLandscape = true;
#else
static constexpr int kPanelLogicalW = SCREEN_WIDTH;
static constexpr int kPanelLogicalH = SCREEN_HEIGHT;
static constexpr int kPanelPhysicalW = SCREEN_WIDTH;
static constexpr int kPanelPhysicalH = SCREEN_HEIGHT;
static constexpr int kHouseCols = 1;
static constexpr int kHouseRows = 1;
static constexpr int kHouseW = kTileW;
static constexpr int kHouseH = kTileH;
static constexpr int kHouseX = 0;
static constexpr int kHouseY = 0;
static constexpr int kChromeX = 0;
static constexpr int kChromeY = 0;
static constexpr int kChromeW = SCREEN_WIDTH;
static constexpr int kChromeH = SCREEN_HEIGHT;
static constexpr int kChromeStatusH = TOP_BAR_H;
static constexpr int kChromeFooterH = BOTTOM_BAR_H;
static constexpr int kMinHit = 14;
static constexpr int kToolScale = 1;
static constexpr int kToolW = kTileW;
static constexpr int kToolH = kTileH;
static constexpr int kToolX = 0;
static constexpr int kToolY = TOP_BAR_H;
static constexpr bool kAlwaysLandscape = false;
#endif

inline constexpr bool houseMapEnabled() {
    return HAMLET_HOUSE_MAP != 0;
}

}  // namespace DisplayProfile
