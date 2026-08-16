// pig_waves.cpp — wave ripple rings, bird collision, tree-shake from waves
// Implements Avatar:: wave methods. State lives in Avatar class (avatar.h).

#include "pig_scene_common.h"
#include "weather.h"
#include "../audio/sfx.h"
#include "../util/time_math.h"

// ==[ WAVE OCCLUSION ]==

bool Avatar::isWaveOccluded(int16_t px, int16_t py) {
    // Pig body rect with 2px padding
    if (px >= currentX - 2 && px <= currentX + PIG_BODY_W_CONST + 2 &&
        py >= PIG_Y_CONST - 2 && py <= PIG_Y_CONST + PIG_BODY_H_CONST + 2)
        return true;

    // Tree crown circle + trunk rect (if visible)
    if (treePhase == TreePhase::ALIVE || treePhase == TreePhase::GROWING) {
        int16_t tbx = treeTrunk.baseX + treeScrollOffset;
        if (tbx < -40) tbx += SCREEN_WIDTH + 80;
        if (tbx > SCREEN_WIDTH + 40) tbx -= SCREEN_WIDTH + 80;
        int16_t treeTop = GRASS_BASE_Y - treeTrunk.trunkHeight;
        int16_t treeRight = tbx + treeTrunk.crownRadius + 4;
        int16_t treeLeft = tbx - treeTrunk.crownRadius - 4;
        if (px >= treeLeft && px <= treeRight &&
            py >= treeTop && py <= GRASS_BASE_Y) return true;
    }
    return false;
}

// ==[ WAVE CONTROL ]==

void Avatar::waveRipple(WaveMode mode, uint8_t intensity) {
    if (mode == WaveMode::NONE) {
        waveMode = WaveMode::NONE;
        return;
    }
    uint32_t now = millis();
    if (mode == WaveMode::INCOMING && waveMode == WaveMode::OUTGOING) {
        if (TimeMath::before(now, waveBurstEnd)) return;
    }
    bool alreadyActive = (waveMode != WaveMode::NONE &&
                          TimeMath::before(now, waveBurstEnd));
    waveMode = mode;
    waveBurstEnd = now + 4000;
    if (!alreadyActive) {
        waveBurstStart = now;
        wave.ringsInitialized = false;
    }
    wave.intensity = (intensity > WaveState::MAX_RINGS) ? WaveState::MAX_RINGS : intensity;
}

// ==[ CIRCLE RING DRAWING ]==

static void drawCircleRing(M5Canvas& canvas, int16_t cx, int16_t cy,
                            int16_t r, uint16_t color, bool reflect,
                            int16_t maxPxX, int16_t maxPxY, bool rainbow = false,
                            bool bendAroundSun = false, int16_t sunCX = 0,
                            int16_t sunCY = 0, int16_t sunRadius = 0) {
    int16_t gr = r / PX;
    if (gr < 1) return;
    cx = snapPx(cx);
    cy = snapPx(cy);
    int16_t gx = gr, gy = 0, d = 1 - gr;

    while (gx >= gy) {
        const int16_t ox[8] = { gx, (int16_t)-gx,  gx, (int16_t)-gx,  gy, (int16_t)-gy,  gy, (int16_t)-gy };
        const int16_t oy[8] = { gy,  gy, (int16_t)-gy, (int16_t)-gy,  gx,  gx, (int16_t)-gx, (int16_t)-gx };
        for (uint8_t p = 0; p < 8; p++) {
            int16_t px = cx + ox[p] * PX;
            int16_t py = cy + oy[p] * PX;
            if (reflect) {
                uint8_t bounces = 0;
                px = reflectAxis(px, maxPxX, bounces);
                py = reflectAxis(py, maxPxY, bounces);
                if (bounces >= 4) continue;
                if (bounces == 3 && ((px / PX + py / PX) & 1)) continue;
                if (bounces == 2 && ((px / PX + py / PX) % 3 == 0)) continue;
            } else {
                if (px < 0 || px > maxPxX || py < 0 || py > maxPxY) continue;
            }

            if (bendAroundSun && !reflect) {
                int32_t dxSun = (int32_t)(px + PX / 2) - sunCX;
                int32_t dySun = (int32_t)(py + PX / 2) - sunCY;
                int32_t dist2 = dxSun * dxSun + dySun * dySun;
                const int16_t coreR = sunRadius + PX;
                const int16_t bendR = sunRadius + PX * 4;
                const int32_t coreR2 = (int32_t)coreR * coreR;
                const int32_t bendR2 = (int32_t)bendR * bendR;

                if (dist2 <= coreR2) {
                    if ((((px / PX) + (py / PX) + (r / PX)) & 1) != 0) continue;
                }

                if (dist2 < bendR2) {
                    // fast integer sqrt (Newton's method, 2 iterations — good to ±1 for values <10000)
                    int32_t dist;
                    {
                        int32_t x = dist2;
                        int32_t g = (x > 100) ? x / 10 : 10;
                        g = (g + x / g) >> 1;
                        g = (g + x / g) >> 1;
                        dist = g;
                    }
                    if (dist < 1) continue;
                    int32_t inner = coreR;
                    int32_t outer = bendR;
                    // t = (outer - dist) / (outer - inner), Q8
                    int32_t range = outer - inner;
                    if (range < 1) range = 1;
                    int32_t tQ8 = ((outer - dist) << 8) / range;
                    if (tQ8 < 0) tQ8 = 0;
                    if (tQ8 > 256) tQ8 = 256;
                    // bend = t*t, Q8 (tQ8*tQ8 >> 8)
                    int32_t bendQ8 = (tQ8 * tQ8) >> 8;
                    int32_t targetDist = sunRadius + PX * 2;
                    // targetR = dist + bend * (targetDist - dist), all Q8
                    int32_t targetR = dist + ((bendQ8 * (targetDist - dist)) >> 8);
                    // scale = targetR / dist, applied directly
                    px = snapPx((int16_t)(sunCX + (dxSun * targetR) / dist));
                    py = snapPx((int16_t)(sunCY + (dySun * targetR) / dist));
                    if (px < 0 || px > maxPxX || py < 0 || py > maxPxY) continue;
                    if (tQ8 > 140 && (((px / PX) + (py / PX) + (r / PX)) % 3 == 0)) continue;  // 140/256 ≈ 0.55
                }
            }

            if (!reflect && Avatar::isWaveOccluded(px, py)) continue;

            uint16_t pixColor = rainbow ? trippyRainbow(px, py) : color;
            canvas.fillRect(px, py, PX, PX, pixColor);
        }
        gy++;
        if (d < 0) { d += 2 * gy + 1; }
        else       { gx--; d += 2 * (gy - gx) + 1; }
    }
}

// ==[ WAVE RIPPLE DRAW ]==

void Avatar::drawWaveRipples(M5Canvas& canvas, bool faceRight, int startX, int startY) {
    if (waveMode == WaveMode::NONE) return;

    uint32_t now = millis();

    // Geiger-counter clicks while waves are actively bursting
    static uint32_t nextGeigerClick = 0;
    if (TimeMath::before(now, waveBurstEnd) &&
        TimeMath::reachedOrUnset(now, nextGeigerClick)) {
        uint16_t freq = (uint16_t)random(800, 1600);
        SFX::tone(freq, random(3, 8));
        nextGeigerClick = now + random(80, 300);
    }

    // Gradual fade after burst ends
    const uint16_t FADE_MS = 3600;
    float minProgress = 0.0f;
    if (TimeMath::reached(now, waveBurstEnd)) {
        uint32_t fadeElapsed = now - waveBurstEnd;
        if (fadeElapsed >= FADE_MS) { waveMode = WaveMode::NONE; return; }
        minProgress = (float)fadeElapsed / (float)FADE_MS * 0.80f;
    }
    uint16_t color = getDrawColor();

    const bool outgoing = (waveMode == WaveMode::OUTGOING);
    bool fastCadence = shouldUseHypeRainbow();

    const uint8_t  COUNT    = outgoing ? 2 : wave.intensity;
    const uint16_t CYCLE_MS = fastCadence ? 2400 : 3600;
    const int16_t  R_MIN    = 0;
    const int16_t  R_MAX    = 130;
    const int16_t  MAX_PX_X = SCREEN_WIDTH - PX;
    const int16_t  MAX_PX_Y = GRASS_BASE_Y - PX;
    int16_t sunCX = 0, sunCY = 0, sunRadius = 0;
    bool bendAroundSun = Weather::getSunDisc(sunCX, sunCY, sunRadius);

    const int16_t GRID_STEPS = (R_MAX - R_MIN) / PX;

    // Initialize all ring origins on first frame of burst
    if (!wave.ringsInitialized) {
        int16_t noseX = faceRight ? (int16_t)(startX + NOSE_RIGHT_X) : (int16_t)(startX + NOSE_LEFT_X);
        int16_t noseY = (int16_t)(startY + NOSE_Y);
        for (uint8_t i = 0; i < WaveState::MAX_RINGS; i++) {
            wave.rings[i] = { noseX, noseY };
            wave.ringLastCycle[i] = 0;
        }
        wave.ringsInitialized = true;
    }

    uint32_t elapsed = now - waveBurstStart;

    for (uint8_t i = 0; i < COUNT; i++) {
        uint32_t phaseOffset = i * (CYCLE_MS / COUNT);
        uint32_t cycleNum = (elapsed + phaseOffset) / CYCLE_MS;

        // Ring wrapped to new cycle — snapshot current pig nose
        if (cycleNum != wave.ringLastCycle[i]) {
            wave.rings[i].cx = faceRight ? (int16_t)(startX + NOSE_RIGHT_X) : (int16_t)(startX + NOSE_LEFT_X);
            wave.rings[i].cy = (int16_t)(startY + NOSE_Y);
            wave.ringLastCycle[i] = cycleNum;
        }

        int16_t waveCX = wave.rings[i].cx;
        int16_t waveCY = wave.rings[i].cy;

        uint32_t phase = (elapsed + phaseOffset) % CYCLE_MS;
        float progress = (float)phase / (float)CYCLE_MS;

        if (progress < minProgress) continue;
        if (progress > 0.80f) continue;
        float t = progress / 0.80f;

        int16_t gridStep = (int16_t)(t * GRID_STEPS);
        int16_t rRaw = R_MIN + gridStep * PX;
        int16_t r = outgoing
            ? snapPx(rRaw)
            : snapPx(R_MIN + R_MAX - rRaw);

        bool earlyLife = (t < 0.5f);

        if (outgoing) {
            drawCircleRing(canvas, waveCX, waveCY, r, color, false, MAX_PX_X, MAX_PX_Y, true,
                           bendAroundSun, sunCX, sunCY, sunRadius);
            if (earlyLife)
                drawCircleRing(canvas, waveCX, waveCY, r + PX, color, false, MAX_PX_X, MAX_PX_Y, true,
                               bendAroundSun, sunCX, sunCY, sunRadius);

            // Tree shake detection (per-ring center)
            if (!wave.treeShaking && (treePhase == TreePhase::ALIVE || treePhase == TreePhase::GROWING)) {
                int16_t tbx = treeTrunk.baseX + treeScrollOffset;
                while (tbx > SCREEN_WIDTH + 20) tbx -= (SCREEN_WIDTH + 80);
                while (tbx < -80) tbx += (SCREEN_WIDTH + 80);
                int32_t dx = tbx - waveCX;
                int32_t dy = GRASS_BASE_Y - waveCY;
                int32_t dist2 = dx * dx + dy * dy;
                int32_t rOuter = r + treeTrunk.crownRadius;
                int32_t rInner = r - treeTrunk.crownRadius;
                if (rInner < 0) rInner = 0;
                if (dist2 <= rOuter * rOuter && dist2 >= rInner * rInner) {
                    wave.treeShaking = true;
                    wave.treeShakeStart = now;
                }
            }
        } else {
            drawCircleRing(canvas, waveCX, waveCY, r, color, false, MAX_PX_X, MAX_PX_Y, true,
                           bendAroundSun, sunCX, sunCY, sunRadius);
            if (earlyLife)
                drawCircleRing(canvas, waveCX, waveCY, r + PX, color, false, MAX_PX_X, MAX_PX_Y, true,
                               bendAroundSun, sunCX, sunCY, sunRadius);
        }
    }
}

// ==[ BIRD-WAVE COLLISION ]==

bool Avatar::checkBirdWaveCollision(int16_t bx, int16_t by) {
    if (waveMode != WaveMode::OUTGOING) return false;

    uint32_t now = millis();
    if (TimeMath::reached(now, waveBurstEnd)) return false;

    bool hyped = shouldUseHypeRainbow();
    const uint16_t CYCLE_MS = hyped ? 2400 : 3600;
    const uint8_t  COUNT = 2;
    uint32_t elapsed = now - waveBurstStart;

    for (uint8_t i = 0; i < COUNT; i++) {
        int32_t dx = (int32_t)bx - wave.rings[i].cx;
        int32_t dy = (int32_t)by - wave.rings[i].cy;
        int32_t dist2 = dx * dx + dy * dy;

        uint32_t phaseOffset = i * (CYCLE_MS / COUNT);
        uint32_t phase = (elapsed + phaseOffset) % CYCLE_MS;
        float progress = (float)phase / (float)CYCLE_MS;
        if (progress > 0.80f) continue;
        float t = progress / 0.80f;

        const int16_t GRID_STEPS_C = 130 / PX;
        int16_t gridStep = (int16_t)(t * GRID_STEPS_C);
        int16_t r = gridStep * PX;
        int32_t rOuter = r + PX;
        int32_t rInner = r - PX;
        if (rInner < 0) rInner = 0;
        if (dist2 <= rOuter * rOuter && dist2 >= rInner * rInner) {
            return true;
        }
    }
    return false;
}
