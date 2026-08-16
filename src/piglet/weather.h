// Weather effects module - clouds, rain, thunder, wind
// Mood-tied weather system ported from Porkchop
#pragma once

#include <M5Unified.h>

namespace Weather {

// === INITIALIZATION ===
void init();

// === WEATHER STATE CONTROL ===
// Call from Mood system to set weather based on momentum
void setMoodLevel(int momentum);  // -100 to 100, affects rain/storm probability


// === ANIMATION UPDATES ===
// Call each frame to update weather effects
void update();

// === DRAWING ===
// Draw ordinary open-air rain/wind (L2). Call before Avatar::draw().
void draw(M5Canvas& canvas, uint16_t colorFG, uint16_t colorBG);

// Draw just clouds (parallax layer, call before avatar if desired)
// Two-tone cloud body. The caller owns atmospheric palette selection;
// this renderer keeps both the lit cap and lower mass on the 4px sky grid.
void drawClouds(M5Canvas& canvas, uint16_t capColor, uint16_t shadeColor);

// Sun: 2-pass rendering for object silhouette inversion
// Pass 1: compute sun mask BEFORE other objects (no drawing)
void drawSunBase(M5Canvas& canvas, uint16_t colorFG);
// Pass 2: mask foreign pixels to BG AFTER all objects drawn
void drawSun(M5Canvas& canvas, uint16_t colorFG);
// True only between drawSunBase() and drawSun(); used by wave renderer.
bool getSunDisc(int16_t& x, int16_t& y, int16_t& radius);

// Draw ambient pixel birds (call between Avatar::draw and drawClouds)
void drawBirds(M5Canvas& canvas, uint16_t colorFG);

// === NUCLEAR SHOCKWAVE ===
// Expanding hype-rainbow circle (call LAST, draws over everything)
void drawShockwave(M5Canvas& canvas);
// Post-shockwave debris rain from sky
void drawDebrisRain(M5Canvas& canvas, uint16_t colorFG);
// True during shockwave expansion (pig stays non-hype)
bool isShockwaveExpanding();
// True when dome has filled screen (pig buried under shockwave)
bool isShockwaveCoveringPig();

// === THUNDER FLASH ===
// Query for thunder flash state (affects screen colors)
bool isThunderFlashing();
bool isRaining();

// === EXPLOSION STATE ===
// True during shuttle destruction through end of debris rain
bool isExplosionActive();
// True during explosion + 60s post-nuke cooldown (suppress trees)
bool isTreeSuppressed();


}  // namespace Weather
