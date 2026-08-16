// pig_grass.cpp — grass blade system, 3-layer parallax, trail particles, fruit splashes
// Implements Avatar:: grass methods. State lives in Avatar class (avatar.h).

#include "pig_scene_common.h"
#include "weather.h"

// ==[ GRASS PARALLAX STATE ]== (file-scope, not shared)
static int16_t grassOffsetLayer2 = 0;
static int16_t grassOffsetLayer3 = 0;
static uint8_t grassParallaxTick2 = 0;
static uint8_t grassParallaxTick3 = 0;

// ==[ GRASS Y OFFSET ]==

void Avatar::setGrassYOffset(int16_t offset) {
    grassYOffset = offset;
}

// ==[ GRASS MOVEMENT CONTROL ]==

void Avatar::setGrassMoving(bool moving, bool directionRight) {
    if (moving && (grassMoving || pendingGrassStart)) return;
    if (!moving && !grassMoving && !pendingGrassStart) return;

    static uint32_t lastGrassStopTime = 0;
    static const uint32_t GRASS_REST_COOLDOWN_MS = 3000;

    if (moving) {
        uint32_t now = millis();
        if (lastGrassStopTime > 0 && (now - lastGrassStopTime) < GRASS_REST_COOLDOWN_MS) {
            return;
        }

        grassDirection = directionRight;
        int targetX = directionRight ? PIG_MAX_X_CONST : PIG_MIN_X_CONST;

        if (transitioning) {
            if (transitionToX == PIG_MIN_X_CONST || transitionToX == PIG_MAX_X_CONST) return;
            pendingGrassStart = true;
            grassMoving = false;
        } else if (currentX != targetX) {
            startWindupSlide(targetX, directionRight);
            pendingGrassStart = true;
            grassMoving = false;
        } else {
            walkLook.facingRight = !directionRight;
            grassMoving = true;
            pendingGrassStart = false;
            posControl.claim(PosOwner::GRASS_WALK);
        }

        lastGrassStopTime = 0;
        walkLook.grassWanderTimer = millis();
        walkLook.grassWanderInterval = random(3000, 8000);
    } else {
        grassMoving = false;
        pendingGrassStart = false;
        posControl.release(PosOwner::GRASS_WALK);
        lastGrassStopTime = millis();
        walkLook.lastFlipTime = millis();
        startWindupSlide(2, false);
    }
}

// ==[ GRASS UPDATE ]== parallax scroll

void Avatar::updateGrass() {
    if (!grassMoving) return;

    uint32_t now = millis();
    uint32_t pixelInterval = grassSpeed / GRASS_STRIDE;
    if (pixelInterval < 1) pixelInterval = 1;
    if (now - lastGrassUpdate < pixelInterval) return;
    lastGrassUpdate = now;

    if (grassDirection) {
        grassOffset++;
        if (++grassParallaxTick2 >= 2) {
            grassParallaxTick2 = 0;
            grassOffsetLayer2++;
            if (grassOffsetLayer2 >= GRASS_STRIDE) grassOffsetLayer2 = 0;
        }
        if (++grassParallaxTick3 >= 3) {
            grassParallaxTick3 = 0;
            grassOffsetLayer3++;
            if (grassOffsetLayer3 >= GRASS_STRIDE) grassOffsetLayer3 = 0;
        }
        if (grassOffset >= GRASS_STRIDE) {
            grassOffset -= GRASS_STRIDE;
            treeScrollOffset += GRASS_STRIDE;
        }
    } else {
        grassOffset--;
        if (++grassParallaxTick2 >= 2) {
            grassParallaxTick2 = 0;
            grassOffsetLayer2--;
            if (grassOffsetLayer2 < 0) grassOffsetLayer2 += GRASS_STRIDE;
        }
        if (++grassParallaxTick3 >= 3) {
            grassParallaxTick3 = 0;
            grassOffsetLayer3--;
            if (grassOffsetLayer3 < 0) grassOffsetLayer3 += GRASS_STRIDE;
        }
        if (grassOffset < 0) {
            grassOffset += GRASS_STRIDE;
            treeScrollOffset -= GRASS_STRIDE;
        }
    }
}

// hash8 — in pig_scene_common.h

// ==[ GRASS LAYER DRAW ]==

void Avatar::drawGrassLayer(M5Canvas& canvas, uint32_t now, uint16_t color, int16_t baseY, int16_t scrollOffsetPx,
                            int16_t layerPx, uint8_t heightNum, uint8_t heightDen,
                            bool foregroundMask, bool drawGroundLine) {
    if (heightDen == 0 || layerPx <= 0) return;

    bool pigOnGround = !isAttackHopping() && body.anim != BodyAnim::CUTE_JUMP;
    int pigLeft = currentX + PIG_PX_CONST * 3;
    int pigRight = currentX + PIG_BODY_W_CONST - PIG_PX_CONST * 3;
    int pigCenter = (pigLeft + pigRight) / 2;
    int pigHalf = (pigRight - pigLeft) / 2;
    if (pigHalf < 1) pigHalf = 1;

    auto isUnderPig = [&](int16_t xStart) -> bool {
        if (!foregroundMask || !pigOnGround) return false;
        int16_t xEnd = xStart + layerPx;
        return (xEnd > pigLeft && xStart < pigRight);
    };

    if (drawGroundLine) {
        int16_t lineY = snapToPx(baseY - 1, layerPx);
        for (int16_t x = 0; x < SCREEN_WIDTH; x += layerPx) {
            if (isUnderPig(x)) continue;   // no solid bar under pig
            canvas.fillRect(x, lineY, layerPx, layerPx, color);
        }

        if (layerPx == PX) {
            int16_t scrollCells = scrollOffsetPx / layerPx;
            for (int16_t x = 0; x < SCREEN_WIDTH; x += layerPx) {
                int16_t col = x / layerPx;
                uint8_t seed = hash8((uint16_t)((col / 3) + 41 + scrollCells));
                int8_t baseDepth = 2 + (seed % 3);
                int8_t sawPhase = (int8_t)((col + scrollCells) % 3);
                int8_t toothDepth = baseDepth - sawPhase;
                if (toothDepth < 1) toothDepth = 1;
                int16_t toothTop = lineY - toothDepth * layerPx;
                uint16_t tc = color;
                canvas.fillRect(x, toothTop, layerPx, toothDepth * layerPx, tc);
            }
        }
    }

    int16_t treeScreenX = treeTrunk.baseX + treeScrollOffset;
    while (treeScreenX > SCREEN_WIDTH + 20) treeScreenX -= (SCREEN_WIDTH + 80);
    while (treeScreenX < -80) treeScreenX += (SCREEN_WIDTH + 80);

    int16_t stemBaseY = drawGroundLine ? snapToPx(baseY, layerPx) : snapToPx(baseY - layerPx, layerPx);

    for (int i = 0; i < GRASS_BLADE_COUNT; i++) {
        int16_t cx = i * GRASS_STRIDE + scrollOffsetPx;
        if (cx < -GRASS_STRIDE) cx += SCREEN_WIDTH + GRASS_STRIDE;
        if (cx >= SCREEN_WIDTH) continue;

        const GrassBlade& b = grassBlades[i];
        int16_t drawHeight = (int16_t)(((int32_t)b.height * heightNum + (heightDen / 2)) / heightDen);
        if (drawHeight < layerPx) drawHeight = layerPx;
        int8_t drawLean = b.lean;

        // Ambient wind sway
        {
            uint32_t phase = now + (uint32_t)i * 197;
            int w = (int)(phase % 2500);
            int sway = (w < 1250) ? (w - 625) : (1875 - w);
            sway = sway * layerPx / 625;
            drawLean += (int8_t)sway;
        }

        // Bend grass under pig body (integer math — no floats)
        if (pigOnGround && cx >= pigLeft && cx <= pigRight) {
            int distFromCenter = cx - pigCenter;
            if (distFromCenter < 0) distFromCenter = -distFromCenter;
            // bend = (pigHalf - dist) / pigHalf, scaled to 0-256 (Q8)
            int bendQ8 = pigHalf > 0 ? ((pigHalf - distFromCenter) << 8) / pigHalf : 0;
            // bentHeight = height - height * 0.7 * bend = height * (256 - 179*bend/256) / 256
            int16_t bentHeight = b.height - (int16_t)(((int32_t)b.height * 179 * bendQ8) >> 16);
            if (bentHeight < layerPx) bentHeight = layerPx;
            drawHeight = (int16_t)(((int32_t)bentHeight * heightNum + (heightDen / 2)) / heightDen);
            if (drawHeight < layerPx) drawHeight = layerPx;
            int8_t leanPush = (int8_t)((4 * bendQ8) >> 8);
            drawLean = (cx < pigCenter) ? (b.lean - leanPush) : (b.lean + leanPush);
        }

        // Tree collision: radial jitter ripple from tree trunk (integer math)
        if (collision.treeColliding) {
            int16_t dist = cx > treeScreenX ? cx - treeScreenX : treeScreenX - cx;
            int16_t radius = (int16_t)treeTrunk.crownRadius * 3;
            if (dist < radius && radius > 0) {
                // falloff = (radius - dist) / radius, Q8
                int fallQ8 = ((radius - dist) << 8) / radius;
                uint32_t phase = now + (uint32_t)(dist * 7);
                int8_t jitter = ((phase / 33) % 2 == 0) ? layerPx : -layerPx;
                drawLean += (int8_t)((jitter * fallQ8) >> 8);
            }
        }

        int16_t tipX = snapToPx(cx + drawLean, layerPx);
        int16_t tipY = snapToPx(baseY - drawHeight, layerPx);
        uint16_t bladeColor = color;

        int16_t stemX = snapToPx(cx, layerPx);
        int16_t midY = snapToPx((stemBaseY + tipY) / 2, layerPx);
        int16_t baseMidX = snapToPx((stemX + tipX) / 2, layerPx);
        int16_t dir = (drawLean >= 0) ? 1 : -1;
        int16_t curl = layerPx;
        if (((i + (int)(now / 1400u)) & 3) == 0) curl += layerPx;
        int16_t midX = snapToPx(baseMidX + dir * curl, layerPx);

        fatLinePx(canvas, stemX, stemBaseY, midX, midY, bladeColor, layerPx);
        fatLinePx(canvas, midX, midY, tipX, tipY, bladeColor, layerPx);
    }
}

// ==[ FRONT GRASS + TRAIL + SPLASH ]==

void Avatar::drawGrass(M5Canvas& canvas) {
    uint32_t now = millis();
    uint16_t color = getDrawColor();
    const int16_t baseY = GRASS_BASE_Y + grassYOffset;

    // Front grass layer: fat blades with body cutout
    drawGrassLayer(canvas, now, color, baseY, grassOffset, PX, 1, 1, true, true);

    bool pigOnGround = !isAttackHopping() && body.anim != BodyAnim::CUTE_JUMP;

    // === Trail particles (dust from running pig) ===
    bool isRunning = transitioning || grassMoving;

    if (isRunning && pigOnGround && now - lastTrailSpawn > 70) {
        lastTrailSpawn = now;
        TrailParticle& p = trailParticles[trailSpawnIdx];
        trailSpawnIdx = (trailSpawnIdx + 1) % TRAIL_COUNT;

        if (walkLook.facingRight) {
            p.x = (float)(currentX + random(0, 21));
            p.vx = -(1.0f + (float)random(0, 20) / 10.0f);
        } else {
            p.x = (float)(currentX + PIG_BODY_W_CONST - 21 + random(0, 21));
            p.vx = 1.0f + (float)random(0, 20) / 10.0f;
        }
        p.y = (float)(GRASS_BASE_Y - 10 + random(0, 10));
        p.vy = -(0.2f + (float)random(0, 10) / 20.0f);
        p.startX = p.x;
        p.maxDist = 30.0f + (float)random(0, 31);
        p.baseSize = random(1, 3);
        p.active = true;
    }

    // Update trail particles
    if (now - lastTrailUpdate > 50) {
        lastTrailUpdate = now;
        for (int i = 0; i < TRAIL_COUNT; i++) {
            if (!trailParticles[i].active) continue;
            trailParticles[i].x += trailParticles[i].vx;
            trailParticles[i].y += trailParticles[i].vy;
            float dx = trailParticles[i].x - trailParticles[i].startX;
            if (dx < 0) dx = -dx;
            if (dx >= trailParticles[i].maxDist) {
                trailParticles[i].active = false;
            }
        }
    }

    // Draw trail particles
    for (int i = 0; i < TRAIL_COUNT; i++) {
        if (!trailParticles[i].active) continue;
        int tpx = snapPx((int16_t)trailParticles[i].x);
        int tpy = snapPx((int16_t)trailParticles[i].y);
        if (tpx < 0 || tpx >= SCREEN_WIDTH) continue;

        float tdx = trailParticles[i].x - trailParticles[i].startX;
        if (tdx < 0) tdx = -tdx;
        if (tdx >= trailParticles[i].maxDist) continue;

        canvas.fillRect(tpx, tpy, PX, PX, color);
    }

    // === Fruit splash particles ===
    static uint32_t lastSplashUpdate = 0;
    if (now - lastSplashUpdate > 50) {
        lastSplashUpdate = now;
        for (uint8_t i = 0; i < FRUIT_SPLASH_COUNT; i++) {
            if (!fruitSplashes[i].active) continue;
            fruitSplashes[i].x += fruitSplashes[i].vx;
            fruitSplashes[i].y += fruitSplashes[i].vy;
            fruitSplashes[i].vy += 0.15f;
            if (now - fruitSplashes[i].spawnTime > 500) {
                fruitSplashes[i].active = false;
            }
        }
    }

    for (uint8_t i = 0; i < FRUIT_SPLASH_COUNT; i++) {
        if (!fruitSplashes[i].active) continue;
        int spx = snapPx((int16_t)fruitSplashes[i].x);
        int spy = snapPx((int16_t)fruitSplashes[i].y);
        if (spx < 0 || spx >= SCREEN_WIDTH) continue;

        float progress = (float)(now - fruitSplashes[i].spawnTime) / 500.0f;
        if (progress >= 1.0f) continue;

        canvas.fillRect(spx, spy, PX, PX, color);
    }
}

// ==[ BACK-LAYER PARALLAX ACCESS ]==
// Provides layer offsets to renderPigScene() in avatar.cpp

int16_t Avatar::getGrassOffsetLayer2() { return grassOffsetLayer2; }
int16_t Avatar::getGrassOffsetLayer3() { return grassOffsetLayer3; }
