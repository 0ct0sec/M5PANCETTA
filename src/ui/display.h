/**
 * Display - shared 320×240 UI compositor for CoreS3 SE and Core2
 *
 * PORKCHOP-compliant 2-color theme system
 */
#pragma once

#include <Arduino.h>
#include <M5GFX.h>  // For M5Canvas forward reference
#include "../core/item_drops.h"
#include "../gfx/gfx.h"

enum class HamletMode;

// Both supported panels share this landscape geometry.
#define SCREEN_WIDTH 320
#define SCREEN_HEIGHT 240

// UI zones — slim bars, maximize playfield
#define TOP_BAR_H 14
#define BOTTOM_BAR_H 14
#define MAIN_H (SCREEN_HEIGHT - TOP_BAR_H - BOTTOM_BAR_H)  // 212px
#define MAIN_AREA_TOP TOP_BAR_H
#define MAIN_AREA_HEIGHT MAIN_H


// === PORKCHOP 2-COLOR THEME SYSTEM ===
// HSV-based procedural generator. infinite themes, zero hardcoded colors.
// 6 style modes: DARK, INVERTED, RETRO, MONO, N0STR0M0, THE OG
// GLDRUN3R = named combo (DARK + RED hue + CL4SH glow + H1 glow level)
#define THEME_STYLE_COUNT 6

namespace Display {
    // Pancetta's neutral body sits on the theme axis rather than at raw FG.
    // Character palettes that need a deliberate relationship to Pancetta use
    // this shared tone instead of duplicating the value in another renderer.
    static constexpr float kPigBodyTone = 0.72f;

    struct PigPalette {
        uint16_t bodyFill;
        uint16_t detail;
        uint16_t voidColor;
    };

    uint16_t getColorFG();
    uint16_t getColorBG();
    bool isInvertedTheme();       // true when bg is brighter than fg
    bool isTheOgTheme();          // fixed Ridley-noir style slot
    uint16_t getCurrentHue();     // 0-359 hue angle
    uint16_t getAccentBaseHue();  // style-aware hue for derived emissive colors
    uint8_t getCurrentStyle();    // 0-5 style mode
    const char* getCurrentThemeName();
    void nextTheme();             // golden angle hue step
    void prevTheme();
    void nextStyle();             // cycle DARK→INVERTED→RETRO→MONO→N0STR0M0→THE OG
    void setThemeHSV(uint16_t hue, uint8_t style);
    // ==[ ACCENT MODE ]== light hue family presets
    struct AccentOffsets { int16_t neon, warm, crt, vend; };
    const AccentOffsets& getAccentOffsets();
    uint8_t getAccentMode();
    uint8_t getLightIntensity();
    const char* getAccentModeName();
    inline uint8_t brightness565(uint16_t c) { return Gfx::brightness565(c); }
    inline uint16_t hsvToRgb565(uint16_t h, uint8_t s, uint8_t v) { return Gfx::hsvToRgb565(h, s, v); }
    inline uint16_t lerpColor565(uint16_t c1, uint16_t c2, float t) { return Gfx::lerpColor565(c1, c2, t); }
    inline uint16_t screenBlend565(uint16_t base, uint16_t light, uint8_t strength8) { return Gfx::screenBlend565(base, light, strength8); }
    PigPalette makePigPalette(uint16_t fg, uint16_t bg);

    // Convert opaque sprite pixels into the active two-color theme. Source
    // brightness becomes ink density; pixels matching transparentKey stay clear.
    void themeMapSprite(M5Canvas& sprite, uint16_t transparentKey);

    // Keep the artwork's own palette and only wash it toward the theme.
    // strength8 is the pull toward the two-color map: 0 leaves the art alone,
    // 255 is themeMapSprite, which forwards here. Portraits are painted, not
    // iconography — a full map throws the painting away, so they take a light
    // wash instead.
    void themeTintSprite(M5Canvas& sprite, uint16_t transparentKey,
                         uint8_t strength8);
}

#define COLOR_DIM 0x7BEF  // Gray for dimmed/inactive elements (works with any theme)

namespace Display {
    // Initialize display
    void init();
    void applyRotation();
    
    // Screen draws
    void drawIdleScreen();
    void drawHuntScreen();
    void drawSpectrumScreen();
    void drawWalkStats();
    void drawSyncScreen();       // FLOCKNOW-compatible sync status alias
    void drawFlockScreen();      // NOWFLOCK passive status
    void flockOnEnter();         // reset pane/scroll on mode enter
    void flockNextPane();
    void flockPrevPane();
    void flockScrollPeers(int delta);
    void flockBtnPrev();         // pane prev or peer scroll up
    void flockBtnNext();         // pane next or peer scroll down
    void drawPowerMenu();
    void drawAboutScreen();
    void onAboutEnter();        // serve unread Pig memory, else advance 0ct0 lore
    void drawWebConfigScreen(); // Web config portal screen
    void drawWardriveScreen();  // Wardrive windshield POV
    void drawBleScreen();       // BLE scanner device list + Geiger
    void drawDefhogScreen();    // DEFHOG4 full-screen defense terminal
    void drawXferScreen();      // AP file xfer: PANCETTA_XFER + web commander
    void drawC5MonsterScreen(); // C5Monster dual-band command menu
    void drawMeshScreen();      // M3SH T4LK: LoRa scrollback + composer
    bool dumpScreenshotToSerial();  // Raw RGB565LE frame dump with serial framing markers

    // Common elements
    void drawStatusBar();
    void drawBottomBar(const char* left, const char* right);
    void drawBottomBar3(const char* left, const char* center, const char* right);
    void drawBottomBar3To(M5Canvas* targetCanvas,
                          const char* left, const char* center, const char* right);
    
    // Helper to draw status bar to any canvas (for submenus)
    void drawStatusBarTo(M5Canvas* targetCanvas, const char* modeOverride = nullptr);

    // ==[ SHARED COMPOSITION SURFACE ]== the one 320x240 PSRAM canvas, created
    // during init(). Only one mode renders at a time and every secondary screen
    // repaints the full frame before presenting, so borrowing this beats each
    // menu holding its own 150KB copy for the life of the boot. Callers must
    // tolerate nullptr: a board that could not allocate it has no canvas at all.
    M5Canvas* getSharedCanvas();
    
    // Screen dimming (PORKCHOP parity)
    void resetDimTimer();      // Call on any user input
    void updateDimming();      // Call in update loop
    bool isDimmed();           // Check if screen is dimmed
    void wakeFromDim();        // Force wake from dimmed state
    void setLowPowerDimmed(bool enabled); // Wardrive telemetry owns dim state
    bool isLowPowerDimmed();
    
    // Paranoia mode overlay (global deauth alert)
    void drawParanoiaOverlayTo(M5Canvas* targetCanvas);  // For menu-specific canvases
    void drawQuickToastTo(M5Canvas* targetCanvas);       // Toast on external canvas (prevents overlay flicker)
    void drawHelpOverlayTo(M5Canvas* targetCanvas);      // Help wiki on external canvas (prevents flicker)
    void drawUiOverlaysTo(M5Canvas* targetCanvas);       // Reward -> paranoia -> help -> toast

    // Alert collision query — let overlays avoid the quick toast rect
    bool hasActiveQuickToast();
    bool getActiveToastRect(int16_t& x, int16_t& y, int16_t& w, int16_t& h);
    
    // Momentum indicator flash
    void triggerMomentumFlash(bool positive);  // positive=3 cycles, negative=1 cycle
    
    // Hunt overlay
    void toggleHuntOverlay();
    bool isOverlayVisible();
    
    // Power menu
    void nextPowerOption();
    void prevPowerOption();
    int getPowerOption();
    void resetPowerMenu();                // safe CANCEL selection on each entry
    bool isShowingSleepWarning();      // Check if warning toast active
    void showSleepWarning();           // Show warning toast
    void acceptSleepWarning();         // Proceed with selected power action
    void declineSleepWarning();        // Cancel warning and return to power menu
    
    // Hold progress overlay (long press feedback)
    void setHoldProgress(float progress);  // 0.0 = not held, 0.0-1.0 = filling
    void drawHoldOverlay();                // Draw to M5.Display after pushSprite
    bool needsOverlayRedraw();             // For throttled submenus: redraw once to clear overlays cleanly.

    // Item drop ceremony - native PNG sprite, grid-snapped fly-in + settle. No scaling.
    void showItemDrop(uint8_t itemId, bool firstTime, ItemDrops::ItemDropSource source);
    bool isItemDropActive();

    // Session end ceremony — "CASE CLOSED" summary before poweroff
    void drawCaseClosed();  // renders + pushes directly to display, blocking 2.5s

    // Help wiki overlay — tap status bar to open, tap to advance, swipe L to dismiss
    void showHelpOverlay(HamletMode mode);
    void dismissHelpOverlay();
    bool advanceHelpPage();       // advance to next page, returns false (+ dismisses) on last
    bool isHelpOverlayActive();

    // ==[ SCENERY PIXEL GRID ]== runtime-tunable fat-pixel size for home screen scenery
    // Covers: grass, tree, waves, sparkles, fruits, birds, sun, wind, clouds
    // Excludes: ship scene, stars, rain (fixed at their own grids)
    int16_t getSceneryPX();
    void setSceneryPX(int16_t px);  // clamp 2-6

    // Boot intro
    void beginBootIntro(uint8_t completedStages, uint8_t totalStages);
    void advanceBootIntro(uint8_t completedStages);
    void finishBootIntro();
    void showProgress(const char* msg, uint8_t pct); // Legacy progress bar
    // DEFHOG4 mini console. \n splits lines, long lines wrap to the 30-char budget.
    // durationMs is a *floor* on the steady window: the copy itself buys 3-5s of
    // hold, and the toast flickers in and out around it (~1.7s total, respawn
    // cadence). flashCount adds that many hard strobes before the settle.
    // Pass nullptr to kill an active toast.
    void showToast(const char* msg, uint32_t durationMs = 2000, uint8_t flashCount = 0);
    void showAlertToast(const char* msg, uint32_t durationMs = 2000, uint8_t flashCount = 0);  // adds the noir header line
    void setTopBarMessage(const char* msg, uint32_t durationMs = 3000);  // Temporary status bar override
    void clearTopBarMessage();
    void showRibSacrifice(uint8_t remaining);       // Rib consumed splash (blocking)

    // Touch hint bottom bar — returns true if hint was drawn (skip normal bar)
    bool drawHintBottomBar(M5Canvas* targetCanvas);

    // ==[ STATUS PILL ]== shared two-state badge for menu rows.
    // Unresolved/active -> loud inverted pill with `unresolvedLabel`.
    // Resolved/done     -> dim right-aligned `resolvedLabel`.
    // `selected` flips row-local colors so the pill stays readable on an
    // inverted selection row. Labels are text-size 1; pillW computed from strlen.
    // See docs/firmware/menu-design.md "Status Pills".
    void drawStatusPillTo(M5Canvas* targetCanvas,
                          int rightX, int rowY,
                          const char* unresolvedLabel,
                          const char* resolvedLabel,
                          bool resolved,
                          bool selected);
}
