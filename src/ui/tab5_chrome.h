#pragma once
/**
 * Tab5Chrome — right-pane launcher / tool host.
 *
 * The 640x720 chrome pane keeps menus off the house. MENU draws a native
 * 48px-row launcher here. Other modes blit their 320x240 canvas 2x.
 */
#include <M5GFX.h>
#include "../hamlet.h"
#include "display_profile.h"

namespace Tab5Chrome {

void init();
M5Canvas* canvas();

enum class Surface : uint8_t {
    House,
    Chrome,
    Tool,     // 2x 320x240 tool rect
    Status,
    Footer,
    Outside
};

void beginFrame();
void drawStatus(const char* modeLabel);
void drawFooter(const char* left, const char* center, const char* right);
void drawLauncherBackdrop();
void blitTool2x(M5Canvas& tool320);
void presentReady(M5Canvas& panel);  // copy chrome canvas onto logical panel

Surface hitSurface(int logicalX, int logicalY);
// Map a logical panel point into 320x240 tool coords. Returns false if outside.
bool mapToTool(int logicalX, int logicalY, int16_t& toolX, int16_t& toolY);
bool mapToChrome(int logicalX, int logicalY, int16_t& chromeX, int16_t& chromeY);

}  // namespace Tab5Chrome
