#include "tab5_compositor.h"

#include <M5Unified.h>
#include "display.h"
#include "display_profile.h"
#include "house_map.h"
#include "menu_pig.h"
#include "tab5_chrome.h"
#include "ui_measurements.h"
#include "../core/config.h"
#include "../hamlet.h"
#include "../util/psram_block.h"
#include <string.h>

#if defined(CONFIG_IDF_TARGET_ESP32P4)
#include "esp_cache.h"
#if __has_include("driver/ppa.h")
#include "driver/ppa.h"
#define HAMLET_TAB5_HAS_PPA 1
#endif
#endif

#ifndef HAMLET_TAB5_HAS_PPA
#define HAMLET_TAB5_HAS_PPA 0
#endif

namespace Tab5Compositor {

namespace {

static M5Canvas* logical = nullptr;
static uint16_t* rotated = nullptr;
static bool ready = false;

#if HAMLET_TAB5_HAS_PPA
static ppa_client_handle_t srmClient = nullptr;
#endif

static size_t logicalBytes() {
    return (size_t)DisplayProfile::kPanelLogicalW *
           (size_t)DisplayProfile::kPanelLogicalH * sizeof(uint16_t);
}

static size_t physicalBytes() {
    return (size_t)DisplayProfile::kPanelPhysicalW *
           (size_t)DisplayProfile::kPanelPhysicalH * sizeof(uint16_t);
}

static void rotate90Cw(const uint16_t* src, uint16_t* dst) {
    const int w = DisplayProfile::kPanelLogicalW;
    const int h = DisplayProfile::kPanelLogicalH;
    for (int y = 0; y < h; ++y) {
        const uint16_t* row = src + y * w;
        for (int x = 0; x < w; ++x) {
            dst[x * h + (h - 1 - y)] = row[x];
        }
    }
}

static void rotate90Ccw(const uint16_t* src, uint16_t* dst) {
    const int w = DisplayProfile::kPanelLogicalW;
    const int h = DisplayProfile::kPanelLogicalH;
    for (int y = 0; y < h; ++y) {
        const uint16_t* row = src + y * w;
        for (int x = 0; x < w; ++x) {
            dst[(w - 1 - x) * h + y] = row[x];
        }
    }
}

#if HAMLET_TAB5_HAS_PPA
static bool ppaRotate(const uint16_t* src, uint16_t* dst, bool ccw) {
    if (!srmClient) return false;
    const uint32_t srcSize = (uint32_t)logicalBytes();
    const uint32_t dstSize = (uint32_t)physicalBytes();
    esp_cache_msync((void*)src, srcSize, ESP_CACHE_MSYNC_DIR_C2M);
    ppa_srm_oper_config_t op = {};
    op.in.buffer = (void*)src;
    op.in.pic_w = DisplayProfile::kPanelLogicalW;
    op.in.pic_h = DisplayProfile::kPanelLogicalH;
    op.in.block_w = DisplayProfile::kPanelLogicalW;
    op.in.block_h = DisplayProfile::kPanelLogicalH;
    op.in.block_offset_x = 0;
    op.in.block_offset_y = 0;
    op.in.srm_color_mode = PPA_SRM_COLOR_MODE_RGB565;
    op.out.buffer = dst;
    op.out.buffer_size = dstSize;
    op.out.pic_w = DisplayProfile::kPanelPhysicalW;
    op.out.pic_h = DisplayProfile::kPanelPhysicalH;
    op.out.block_offset_x = 0;
    op.out.block_offset_y = 0;
    op.out.srm_color_mode = PPA_SRM_COLOR_MODE_RGB565;
    op.rotation_angle = ccw ? PPA_SRM_ROTATION_ANGLE_270
                            : PPA_SRM_ROTATION_ANGLE_90;
    op.scale_x = 1.0f;
    op.scale_y = 1.0f;
    op.mode = PPA_TRANS_MODE_BLOCKING;
    if (ppa_do_scale_rotate_mirror(srmClient, &op) != ESP_OK) return false;
    esp_cache_msync(dst, dstSize, ESP_CACHE_MSYNC_DIR_M2C);
    return true;
}
#endif

}  // namespace

void init() {
    if (ready) return;
#if !HAMLET_TARGET_TAB5
    ready = true;
    return;
#endif
    HouseMap::init();
    Tab5Chrome::init();

    logical = new M5Canvas(&M5.Display);
    if (logical) {
        logical->setPsram(true);
        logical->createSprite(DisplayProfile::kPanelLogicalW,
                              DisplayProfile::kPanelLogicalH);
    }
    rotated = static_cast<uint16_t*>(PsramBlock::alloc(physicalBytes()));
#if HAMLET_TAB5_HAS_PPA
    ppa_client_config_t cfg = {};
    cfg.oper_type = PPA_OPERATION_SRM;
    ppa_register_client(&cfg, &srmClient);
#endif
    ready = logical && logical->getBuffer() && rotated;
}

void invalidate() {}

void physicalToLogical(int16_t physX, int16_t physY,
                       int16_t& logicalX, int16_t& logicalY) {
    const bool flip = Config::getDisplayRotate180();
    if (!flip) {
        logicalX = physY;
        logicalY = (int16_t)(DisplayProfile::kPanelPhysicalW - 1 - physX);
    } else {
        logicalX = (int16_t)(DisplayProfile::kPanelPhysicalH - 1 - physY);
        logicalY = physX;
    }
}

const char* modeLabel(HamletMode mode) {
    switch (mode) {
        case HamletMode::IDLE:        return "1DL3";
        case HamletMode::MENU:        return "MENU";
        case HamletMode::HUNT:        return "TRUFFL3S";
        case HamletMode::SPECTRUM:    return "RF SC0PE";
        case HamletMode::LOOT:        return "TH3 T4K3";
        case HamletMode::FEEDING:     return "R1B R4CK";
        case HamletMode::WALK_STATS:  return "ST3PS";
        case HamletMode::SETTINGS:    return "TUN3 P1G";
        case HamletMode::NOWFLOCK:    return "N0W F0CK";
        case HamletMode::POWER_MENU:  return "P0W3R";
        case HamletMode::ABOUT:       return "TH3 L0R3";
        case HamletMode::WEBCONFIG:   return "W3B CFG";
        case HamletMode::WARDRIVE:    return "W4RDR1V3";
        case HamletMode::BLE_SCANNER: return "P1G 34RS";
        case HamletMode::DEFHOG4:     return "D3F H0G4";
        case HamletMode::XFER:        return "XF3RM0D3";
        case HamletMode::C5MONSTER:   return "C5 M0NST3R";
        case HamletMode::MAIL:        return "P1G P0ST";
        case HamletMode::MESH:        return "M3SH T4LK";
        default:                      return "HAMLET";
    }
}

TouchTarget mapTouch(int16_t physX, int16_t physY,
                     int16_t& uiX, int16_t& uiY, uint8_t& houseRoom) {
    houseRoom = 0xFF;
    int16_t lx = physX;
    int16_t ly = physY;
    physicalToLogical(physX, physY, lx, ly);
    const Tab5Chrome::Surface surf = Tab5Chrome::hitSurface(lx, ly);
    if (surf == Tab5Chrome::Surface::House) {
        const int hx = lx - DisplayProfile::kHouseX;
        const int hy = ly - DisplayProfile::kHouseY;
        houseRoom = HouseMap::roomAt(hx, hy);
        const uint8_t room = (houseRoom == 0xFF) ? 0 : houseRoom;
        uiX = (int16_t)(hx - HouseMap::originX(room));
        uiY = (int16_t)(hy - HouseMap::originY(room));
        return TouchTarget::House;
    }
    int16_t toolX = 0, toolY = 0;
    if (Tab5Chrome::mapToTool(lx, ly, toolX, toolY)) {
        uiX = toolX;
        uiY = toolY;
        return TouchTarget::Tool;
    }
    int16_t cx = 0, cy = 0;
    Tab5Chrome::mapToChrome(lx, ly, cx, cy);
    if (surf == Tab5Chrome::Surface::Status) {
        uiX = (int16_t)((int)cx * DisplayProfile::kTileW / DisplayProfile::kChromeW);
        uiY = (int16_t)((int)cy * UIMeasurements::kTopBarH /
                        DisplayProfile::kChromeStatusH);
        return TouchTarget::Status;
    }
    if (surf == Tab5Chrome::Surface::Footer) {
        const int footerY = DisplayProfile::kChromeH - DisplayProfile::kChromeFooterH;
        uiX = (int16_t)((int)cx * DisplayProfile::kTileW / DisplayProfile::kChromeW);
        uiY = (int16_t)(UIMeasurements::kScreenHeight - UIMeasurements::kBottomBarH +
                        (cy - footerY) * UIMeasurements::kBottomBarH /
                        DisplayProfile::kChromeFooterH);
        return TouchTarget::Footer;
    }
    uiX = (int16_t)(cx / DisplayProfile::kToolScale);
    uiY = (int16_t)(cy / DisplayProfile::kToolScale);
    return TouchTarget::Outside;
}

void present(M5Canvas& toolCanvas) {
    if (!ready) init();
    if (!logical || !logical->getBuffer() || !rotated) {
        toolCanvas.pushSprite(0, 0);
        return;
    }

    const uint32_t now = millis();
    if (HouseMap::atlas()) {
        HouseMap::render(*HouseMap::atlas(), now, MenuPig::getCurrentRoom());
    }

    Tab5Chrome::beginFrame();
    Tab5Chrome::drawStatus(modeLabel(Hamlet::getMode()));
    Tab5Chrome::blitTool2x(toolCanvas);
    Tab5Chrome::drawFooter("[TAP]SLCT", "[SWIPE]NAV", "[HOLD]B4CK");

    logical->fillSprite(Display::getColorBG());
    HouseMap::blitToPanel(*logical);
    Tab5Chrome::presentReady(*logical);

    const uint16_t* src = static_cast<const uint16_t*>(logical->getBuffer());
    const bool ccw = Config::getDisplayRotate180();
    bool rotatedOk = false;
#if HAMLET_TAB5_HAS_PPA
    rotatedOk = ppaRotate(src, rotated, ccw);
#endif
    if (!rotatedOk) {
        if (ccw) rotate90Ccw(src, rotated);
        else rotate90Cw(src, rotated);
    }
    M5.Display.pushImage(0, 0,
                         DisplayProfile::kPanelPhysicalW,
                         DisplayProfile::kPanelPhysicalH,
                         rotated);
}

}  // namespace Tab5Compositor
