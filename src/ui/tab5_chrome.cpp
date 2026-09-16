#include "tab5_chrome.h"

#include <M5Unified.h>
#include "display.h"
#include "display_profile.h"
#include "../hamlet.h"
#include <string.h>

namespace Tab5Chrome {

namespace {
static M5Canvas* chrome = nullptr;
static bool nativeDraw = false;

static void fillBand(int y, int h, uint16_t color) {
    if (!chrome) return;
    chrome->fillRect(0, y, DisplayProfile::kChromeW, h, color);
}

}  // namespace

void init() {
    if (chrome) return;
#if HAMLET_TARGET_TAB5
    chrome = new M5Canvas(&M5.Display);
    if (!chrome) return;
    chrome->setPsram(true);
    chrome->createSprite(DisplayProfile::kChromeW, DisplayProfile::kChromeH);
    if (!chrome->getBuffer()) {
        delete chrome;
        chrome = nullptr;
    }
#endif
}

M5Canvas* canvas() {
    return chrome;
}

void beginFrame() {
    nativeDraw = false;
    if (!chrome) return;
    chrome->fillSprite(Display::getColorBG());
}

void drawStatus(const char* modeLabel) {
    if (!chrome) return;
    const uint16_t fg = Display::getColorFG();
    const uint16_t bg = Display::getColorBG();
    fillBand(0, DisplayProfile::kChromeStatusH, bg);
    chrome->drawFastHLine(0, DisplayProfile::kChromeStatusH - 1,
                          DisplayProfile::kChromeW, fg);
    chrome->setTextDatum(TL_DATUM);
    chrome->setTextColor(fg);
    chrome->setTextSize(2);
    chrome->drawString(modeLabel ? modeLabel : "HAMLET", 8, 12);

    char bat[12];
    snprintf(bat, sizeof(bat), "%u%%", (unsigned)Hamlet::getBatteryPercent());
    chrome->setTextDatum(TR_DATUM);
    chrome->drawString(bat, DisplayProfile::kChromeW - 8, 12);
    chrome->setTextDatum(TL_DATUM);
}

void drawFooter(const char* left, const char* center, const char* right) {
    if (!chrome) return;
    const uint16_t fg = Display::getColorFG();
    const uint16_t bg = Display::getColorBG();
    const int y = DisplayProfile::kChromeH - DisplayProfile::kChromeFooterH;
    fillBand(y, DisplayProfile::kChromeFooterH, bg);
    chrome->drawFastHLine(0, y, DisplayProfile::kChromeW, fg);
    chrome->setTextColor(fg);
    chrome->setTextSize(1);
    chrome->setTextDatum(TL_DATUM);
    if (left) chrome->drawString(left, 8, y + 18);
    chrome->setTextDatum(TC_DATUM);
    if (center) chrome->drawString(center, DisplayProfile::kChromeW / 2, y + 18);
    chrome->setTextDatum(TR_DATUM);
    if (right) chrome->drawString(right, DisplayProfile::kChromeW - 8, y + 18);
    chrome->setTextDatum(TL_DATUM);
}

void drawLauncherBackdrop() {
    nativeDraw = true;
}

void blitTool2x(M5Canvas& tool320) {
    if (!chrome || !tool320.getBuffer()) return;
    const int srcW = tool320.width();
    const int srcH = tool320.height();
    const uint16_t* src = static_cast<const uint16_t*>(tool320.getBuffer());
    uint16_t* dst = static_cast<uint16_t*>(chrome->getBuffer());
    const int dstW = DisplayProfile::kChromeW;
    const int scale = DisplayProfile::kToolScale;
    const int destY = DisplayProfile::kChromeStatusH;
    for (int y = 0; y < srcH; ++y) {
        uint16_t* row0 = dst + (destY + y * scale) * dstW;
        for (int x = 0; x < srcW; ++x) {
            const uint16_t c = src[y * srcW + x];
            const int dx = x * scale;
            row0[dx] = c;
            row0[dx + 1] = c;
        }
        memcpy(dst + (destY + y * scale + 1) * dstW,
               row0, (size_t)dstW * sizeof(uint16_t));
    }
}

void presentReady(M5Canvas& panel) {
    if (!chrome || !chrome->getBuffer() || !panel.getBuffer()) return;
    const int cw = DisplayProfile::kChromeW;
    const int ch = DisplayProfile::kChromeH;
    const int panelW = panel.width();
    const uint16_t* src = static_cast<const uint16_t*>(chrome->getBuffer());
    uint16_t* dst = static_cast<uint16_t*>(panel.getBuffer());
    for (int y = 0; y < ch; ++y) {
        memcpy(dst + y * panelW + DisplayProfile::kChromeX,
               src + y * cw,
               (size_t)cw * sizeof(uint16_t));
    }
}

Surface hitSurface(int logicalX, int logicalY) {
    if (logicalX < DisplayProfile::kHouseW) {
        if (logicalY < 0 || logicalY >= DisplayProfile::kHouseH) return Surface::Outside;
        return Surface::House;
    }
    const int cx = logicalX - DisplayProfile::kChromeX;
    const int cy = logicalY - DisplayProfile::kChromeY;
    if (cx < 0 || cx >= DisplayProfile::kChromeW ||
        cy < 0 || cy >= DisplayProfile::kChromeH) {
        return Surface::Outside;
    }
    if (cy < DisplayProfile::kChromeStatusH) return Surface::Status;
    if (cy >= DisplayProfile::kChromeH - DisplayProfile::kChromeFooterH)
        return Surface::Footer;
    if (!nativeDraw) return Surface::Tool;
    return Surface::Chrome;
}

bool mapToTool(int logicalX, int logicalY, int16_t& toolX, int16_t& toolY) {
    if (hitSurface(logicalX, logicalY) != Surface::Tool) return false;
    const int lx = logicalX - DisplayProfile::kToolX;
    const int ly = logicalY - DisplayProfile::kToolY;
    toolX = (int16_t)(lx / DisplayProfile::kToolScale);
    toolY = (int16_t)(ly / DisplayProfile::kToolScale);
    return toolX >= 0 && toolY >= 0 &&
           toolX < DisplayProfile::kTileW && toolY < DisplayProfile::kTileH;
}

bool mapToChrome(int logicalX, int logicalY, int16_t& chromeX, int16_t& chromeY) {
    chromeX = (int16_t)(logicalX - DisplayProfile::kChromeX);
    chromeY = (int16_t)(logicalY - DisplayProfile::kChromeY);
    return chromeX >= 0 && chromeY >= 0 &&
           chromeX < DisplayProfile::kChromeW &&
           chromeY < DisplayProfile::kChromeH;
}

}  // namespace Tab5Chrome
