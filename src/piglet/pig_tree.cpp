// pig_tree.cpp — fruit tree generation, growth/collapse, fruit drop, splash
// Implements Avatar:: tree methods. State lives in Avatar class (avatar.h).

#include "pig_scene_common.h"
#include "weather.h"
#include "../audio/sfx.h"
#include <esp_random.h>

// ==[ TREE HELPERS ]==

static uint32_t treeLCG(uint32_t& s) {
    s = s * 1664525u + 1013904223u;
    return s;
}

static const int16_t sin_lut[13] = {
    0, 66, 128, 181, 222, 248, 256, 248, 222, 181, 128, 66, 0
};

static int16_t lut_sin(uint8_t idx) {
    return (idx < 13) ? sin_lut[idx] : 0;
}

static int16_t lut_cos(uint8_t idx) {
    if (idx > 12) return 0;
    int8_t ci = 6 - (int8_t)idx;
    if (ci >= 0) return sin_lut[ci];
    return -sin_lut[-ci];
}

static void fatFruit(M5Canvas& canvas, int16_t cx, int16_t cy, int r,
                     uint16_t fill, uint16_t outline) {
    int16_t gx = snapPx(cx), gy = snapPx(cy);
    if (r >= 6) {
        static const int8_t ring[][2] = {
            {-1,-2},{0,-2},{1,-2},
            {-2,-1},{2,-1},
            {-2, 0},{2, 0},
            {-2, 1},{2, 1},
            {-1, 2},{0, 2},{1, 2}
        };
        for (auto& pt : ring)
            canvas.fillRect(gx + pt[0] * PX, gy + pt[1] * PX,
                            PX, PX, outline);
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++)
                canvas.fillRect(gx + dx * PX, gy + dy * PX,
                                PX, PX, fill);
    } else {
        const int16_t p = PX;
        canvas.fillRect(gx, gy - p, p, p, outline);
        canvas.fillRect(gx - p, gy, p, p, outline);
        canvas.fillRect(gx + p, gy, p, p, outline);
        canvas.fillRect(gx, gy + p, p, p, outline);
        canvas.fillRect(gx, gy, p, p, fill);
    }
}

// ==[ TREE GENERATION ]==

void Avatar::generateTree(uint8_t fruitCount) {
    treeSeed = esp_random();
    treeScrollOffset = 0;
    uint32_t s = treeSeed;

    if (onRightSide) {
        treeTrunk.baseX = 30 + (int16_t)(treeLCG(s) % 40);
    } else {
        treeTrunk.baseX = SCREEN_WIDTH - 90 + (int16_t)(treeLCG(s) % 50);
    }

    int16_t gap = abs(treeTrunk.baseX - currentX);
    if (gap < 80) {
        treeTrunk.baseX = (currentX < SCREEN_WIDTH / 2) ? SCREEN_WIDTH - 80 + (int16_t)(treeLCG(s) % 50)
                                            : 20 + (int16_t)(treeLCG(s) % 50);
    }

    treeTrunk.trunkHeight = 66 + fruitCount + (uint8_t)(treeLCG(s) % 7);
    treeTrunk.trunkWidth = 2 + (uint8_t)(treeLCG(s) % 2);
    treeTrunk.trunkLean = (int8_t)(treeLCG(s) % 7) - 3;
    treeTrunk.crownRadius = 8 + treeTrunk.trunkHeight / 5 + (uint8_t)(treeLCG(s) % 3);

    const uint8_t mainCount = 3;
    treeBranchCount = 0;
    uint8_t sector = 7 / mainCount;
    const uint8_t originPcts[3] = { 50, 70, 100 };

    uint8_t subEndpoints[6];
    uint8_t subEndpointCount = 0;

    for (uint8_t m = 0; m < mainCount && treeBranchCount < MAX_BRANCHES; m++) {
        uint8_t sectorStart = 3 + m * sector;
        uint8_t angleIdx = sectorStart + (uint8_t)(treeLCG(s) % sector);
        if (angleIdx > 9) angleIdx = 9;

        int8_t originY = -(int8_t)(treeTrunk.trunkHeight * originPcts[m] / 100);
        int8_t originX = (int8_t)((int16_t)treeTrunk.trunkLean * originPcts[m] / 100);

        uint8_t length = 18 + (uint8_t)(treeLCG(s) % 15);

        int8_t dx = (int8_t)((int16_t)length * lut_cos(angleIdx) / 256);
        int8_t dy = (int8_t)(-(int16_t)length * lut_sin(angleIdx) / 256);

        TreeBranch& br = treeBranches[treeBranchCount];
        br.x1 = originX;
        br.y1 = originY;
        br.x2 = originX + dx;
        br.y2 = originY + dy;
        br.thickness = 2;
        treeBranchCount++;

        // Sub-branch from 2/3 point
        if (treeBranchCount < MAX_BRANCHES) {
            int8_t midX = br.x1 + (br.x2 - br.x1) * 2 / 3;
            int8_t midY = br.y1 + (br.y2 - br.y1) * 2 / 3;

            int8_t angleOff = 1 + (int8_t)(treeLCG(s) % 3);
            if (treeLCG(s) % 2 == 0) angleOff = -angleOff;
            int8_t subAngle = (int8_t)angleIdx + angleOff;
            if (subAngle < 3) subAngle = 3;
            if (subAngle > 9) subAngle = 9;

            uint8_t subLen = 12 + (uint8_t)(treeLCG(s) % 11);

            uint8_t subIdx = treeBranchCount;
            TreeBranch& sbr = treeBranches[subIdx];
            sbr.x1 = midX;
            sbr.y1 = midY;
            sbr.x2 = midX + (int8_t)((int16_t)subLen * lut_cos((uint8_t)subAngle) / 256);
            sbr.y2 = midY + (int8_t)(-(int16_t)subLen * lut_sin((uint8_t)subAngle) / 256);
            sbr.thickness = 1;
            treeBranchCount++;

            if (subEndpointCount < 6) {
                subEndpoints[subEndpointCount++] = subIdx;
            }
        }

        // Second sub-branch from 1/3 point
        if (treeBranchCount < MAX_BRANCHES) {
            int8_t thirdX = br.x1 + (br.x2 - br.x1) / 3;
            int8_t thirdY = br.y1 + (br.y2 - br.y1) / 3;

            int8_t angleOff2 = 1 + (int8_t)(treeLCG(s) % 3);
            if (treeLCG(s) % 2 == 0) angleOff2 = -angleOff2;
            int8_t subAngle2 = (int8_t)angleIdx + angleOff2;
            if (subAngle2 < 3) subAngle2 = 3;
            if (subAngle2 > 9) subAngle2 = 9;

            uint8_t subLen2 = 10 + (uint8_t)(treeLCG(s) % 9);

            uint8_t subIdx2 = treeBranchCount;
            TreeBranch& sbr2 = treeBranches[subIdx2];
            sbr2.x1 = thirdX;
            sbr2.y1 = thirdY;
            sbr2.x2 = thirdX + (int8_t)((int16_t)subLen2 * lut_cos((uint8_t)subAngle2) / 256);
            sbr2.y2 = thirdY + (int8_t)(-(int16_t)subLen2 * lut_sin((uint8_t)subAngle2) / 256);
            sbr2.thickness = 1;
            treeBranchCount++;

            if (subEndpointCount < 6) {
                subEndpoints[subEndpointCount++] = subIdx2;
            }
        }
    }

    // Tertiary branches from sub-branch endpoints
    for (uint8_t t = 0; t < subEndpointCount && treeBranchCount < MAX_BRANCHES; t++) {
        const TreeBranch& parent = treeBranches[subEndpoints[t]];
        int8_t angleOff = 1 + (int8_t)(treeLCG(s) % 3);
        if (treeLCG(s) % 2 == 0) angleOff = -angleOff;

        int8_t terAngle = 6 + angleOff;
        if (terAngle < 3) terAngle = 3;
        if (terAngle > 9) terAngle = 9;

        uint8_t terLen = 8 + (uint8_t)(treeLCG(s) % 9);

        TreeBranch& tbr = treeBranches[treeBranchCount];
        tbr.x1 = parent.x2;
        tbr.y1 = parent.y2;
        tbr.x2 = parent.x2 + (int8_t)((int16_t)terLen * lut_cos((uint8_t)terAngle) / 256);
        tbr.y2 = parent.y2 + (int8_t)(-(int16_t)terLen * lut_sin((uint8_t)terAngle) / 256);
        tbr.thickness = 1;
        treeBranchCount++;
    }

    // Fruits anchored to branch endpoints
    if (treeBranchCount == 0) { treeFruitCount = 0; return; }
    treeFruitCount = fruitCount > MAX_TREE_FRUITS ? MAX_TREE_FRUITS : fruitCount;
    uint8_t fruitRadius = (treeFruitCount <= 4) ? 6 : 4;
    uint8_t endpointCount = treeBranchCount;

    for (uint8_t i = 0; i < treeFruitCount; i++) {
        uint8_t bi = (uint8_t)(treeLCG(s) % endpointCount);
        const TreeBranch& br = treeBranches[bi];
        int8_t scatter = 3;
        treeFruits[i].offsetX = br.x2 + (int8_t)((treeLCG(s) % (scatter * 2 + 1)) - scatter);
        treeFruits[i].offsetY = br.y2 + (int8_t)((treeLCG(s) % (scatter * 2 + 1)) - scatter);
        treeFruits[i].radius = fruitRadius;
        treeFruits[i].bobPhase = (uint8_t)(treeLCG(s) & 0xFF);
    }
}

// ==[ TREE VISIBILITY CONTROL ]==

void Avatar::showTree(uint8_t fruitCount) {
    if (fruitCount == 0) return;

    if (treePhase == TreePhase::COLLAPSING) {
        treePendingShow = true;
        treePendingHide = false;
        treePendingFruits = fruitCount;
        return;
    }

    if (treePhase == TreePhase::GROWING) return;

    if (treePhase == TreePhase::ALIVE && treeFruitCount == fruitCount) return;

    if (treePhase == TreePhase::ALIVE) {
        uint32_t s = treeSeed + fruitCount;
        uint8_t fruitRadius = (fruitCount <= 4) ? 6 : 4;
        treeFruitCount = fruitCount > MAX_TREE_FRUITS ? MAX_TREE_FRUITS : fruitCount;
        uint8_t endpointCount = treeBranchCount > 0 ? treeBranchCount : 1;
        for (uint8_t i = 0; i < treeFruitCount; i++) {
            uint8_t bi = (uint8_t)(treeLCG(s) % endpointCount);
            const TreeBranch& br = treeBranches[bi];
            int8_t scatter = 3;
            treeFruits[i].offsetX = br.x2 + (int8_t)((treeLCG(s) % (scatter * 2 + 1)) - scatter);
            treeFruits[i].offsetY = br.y2 + (int8_t)((treeLCG(s) % (scatter * 2 + 1)) - scatter);
            treeFruits[i].radius = fruitRadius;
            treeFruits[i].bobPhase = (uint8_t)(treeLCG(s) & 0xFF);
        }
        return;
    }

    generateTree(fruitCount);
    treePhase = TreePhase::GROWING;
    treeGrowth = 0.0f;
    treeAnimStart = millis();
}

void Avatar::hideTree() {
    if (treePhase == TreePhase::HIDDEN || treePhase == TreePhase::COLLAPSING) return;

    if (treePhase == TreePhase::GROWING) {
        treePendingHide = true;
        treePendingShow = false;
        return;
    }

    if (treePhase == TreePhase::ALIVE && (millis() - treeAliveStart < TREE_MIN_ALIVE_MS)) {
        treePendingHide = true;
        treePendingShow = false;
        return;
    }

    treePhase = TreePhase::COLLAPSING;
    treeAnimStart = millis();
    treeGrowth = 1.0f;
}

bool Avatar::isTreeVisible() {
    return treePhase != TreePhase::HIDDEN;
}

// ==[ FRUIT DROP ]==

void Avatar::dropFruit() {
    if (treeFruitCount == 0 || treePhase != TreePhase::ALIVE) return;

    uint8_t idx = treeFruitCount - 1;
    const TreeFruit& f = treeFruits[idx];

    const int16_t baseY = GRASS_BASE_Y + grassYOffset;
    int16_t bx = treeTrunk.baseX + treeScrollOffset;
    while (bx > SCREEN_WIDTH + 20) bx -= (SCREEN_WIDTH + 80);
    while (bx < -80) bx += (SCREEN_WIDTH + 80);

    int8_t sway = 0;
    uint32_t now = millis();
    int w = (int)(now % 3000);
    sway = (w < 1500) ? (int8_t)(((w - 750) * PX) / 750)
                       : (int8_t)(((2250 - w) * PX) / 750);

    for (uint8_t i = 0; i < MAX_DROPPING; i++) {
        if (!droppingFruits[i].active) {
            droppingFruits[i].x = bx + f.offsetX + sway;
            int16_t dropY = (int16_t)baseY + (int16_t)f.offsetY;
            if (dropY < 0) dropY = 0;
            droppingFruits[i].y = dropY;
            droppingFruits[i].radius = f.radius;
            droppingFruits[i].dropStart = now;
            droppingFruits[i].active = true;
            break;
        }
    }

    treeFruitCount--;
    if (treeFruitCount == 0) hideTree();
}

// ==[ TREE UPDATE ]==

void Avatar::updateTree() {
    uint32_t now = millis();

    if (treePhase == TreePhase::ALIVE) {
        if (treePendingHide && (now - treeAliveStart >= TREE_MIN_ALIVE_MS)) {
            treePendingHide = false;
            treePhase = TreePhase::COLLAPSING;
            treeAnimStart = now;
            treeGrowth = 1.0f;
        }
        return;
    }

    if (treePhase == TreePhase::HIDDEN) return;

    uint32_t elapsed = now - treeAnimStart;

    if (treePhase == TreePhase::GROWING) {
        treeGrowth = (float)elapsed / (float)TREE_GROW_MS;
        if (treeGrowth >= 1.0f) {
            treeGrowth = 1.0f;
            if (treePendingHide) {
                treePendingHide = false;
                treePhase = TreePhase::COLLAPSING;
                treeAnimStart = now;
            } else {
                treePhase = TreePhase::ALIVE;
                treeAliveStart = now;
            }
        }
    } else if (treePhase == TreePhase::COLLAPSING) {
        treeGrowth = 1.0f - (float)elapsed / (float)TREE_COLLAPSE_MS;
        if (treeGrowth <= 0.0f) {
            treeGrowth = 0.0f;
            if (treePendingShow) {
                treePendingShow = false;
                generateTree(treePendingFruits);
                treePhase = TreePhase::GROWING;
                treeAnimStart = now;
            } else {
                treePhase = TreePhase::HIDDEN;
            }
        }
    }
}

// ==[ TREE DRAW ]==

void Avatar::drawTree(M5Canvas& canvas) {
    updateTree();

    bool hasDropping = false;
    for (uint8_t i = 0; i < MAX_DROPPING; i++) {
        if (droppingFruits[i].active) { hasDropping = true; break; }
    }

    if (treePhase == TreePhase::HIDDEN && !hasDropping) return;

    uint16_t fg = getDrawColor();
    uint16_t bg = getBGColor();
    uint32_t now = millis();

    const int16_t baseY = GRASS_BASE_Y + grassYOffset;
    int16_t bx = treeTrunk.baseX + treeScrollOffset;
    while (bx > SCREEN_WIDTH + 20) bx -= (SCREEN_WIDTH + 80);
    while (bx < -80) bx += (SCREEN_WIDTH + 80);

    // Ambient sway when alive
    int8_t sway = 0;
    if (treePhase == TreePhase::ALIVE) {
        int w = (int)(now % 3000);
        sway = (w < 1500) ? (int8_t)(((w - 750) * PX) / 750)
                           : (int8_t)(((2250 - w) * PX) / 750);
    }

    if (collision.treeColliding) sway += collision.treeCollisionShake;

    // Wave-tree shake
    if (wave.treeShaking) {
        uint32_t elapsed = now - wave.treeShakeStart;
        if (elapsed < 300) {
            int8_t jitter = ((elapsed / 33) % 2 == 0) ? PX : -PX;
            if (elapsed > 150) jitter = (jitter < 0) ? -PX : PX;
            sway += jitter;
        } else {
            wave.treeShaking = false;
        }
    }

    bool collapsing = (treePhase == TreePhase::COLLAPSING);
    float collapseT = collapsing ? (1.0f - treeGrowth) : 0.0f;
    float collapseT2 = collapseT * collapseT;

    // Phase 1: Trunk
    float trunkProgress = 1.0f;
    if (!collapsing && treeGrowth < 0.25f) {
        trunkProgress = treeGrowth / 0.25f;
    }

    if (trunkProgress > 0.0f) {
        int16_t trunkH = (int16_t)((float)treeTrunk.trunkHeight * trunkProgress);
        int8_t lean = treeTrunk.trunkLean + sway;

        int16_t trunkTopDrop = 0;
        if (collapsing) {
            trunkTopDrop = (int16_t)(collapseT2 * (float)treeTrunk.trunkHeight);
            trunkH -= trunkTopDrop;
            if (trunkH <= 0) trunkH = 0;
        }

        if (trunkH > 0) {
            int16_t trunkTop = baseY - trunkH;
            for (int16_t row = 0; row < trunkH; row += PX) {
                float t = (float)row / (float)(trunkH > 1 ? trunkH - 1 : 1);
                int16_t rowLean = snapPx(lean - (int16_t)((float)lean * t));
                int hwFat = 1 + (int)(t * 1.0f + 0.5f);
                int16_t h = (row + PX > trunkH) ? (trunkH - row) : PX;
                int16_t trunkX = snapPx(bx + rowLean);
                for (int dx = -hwFat; dx <= hwFat; dx++) {
                    canvas.fillRect(trunkX + dx * PX, trunkTop + row,
                                    PX, h, fg);
                }
            }
        }
    }

    // Phase 2: Crown + Branches + Leaves
    if (collapsing || treeGrowth >= 0.25f) {

    float branchProgress = 1.0f;
    if (!collapsing) {
        branchProgress = (treeGrowth - 0.25f) / 0.5f;
        if (branchProgress > 1.0f) branchProgress = 1.0f;
    }

    for (uint8_t i = 0; i < treeBranchCount; i++) {
        const TreeBranch& br = treeBranches[i];
        int16_t sx = bx + br.x1 + sway;
        int16_t sy = baseY + br.y1;
        int16_t fullEx = bx + br.x2 + sway;
        int16_t fullEy = baseY + br.y2;

        if (collapsing) {
            int16_t distFromGround = baseY - fullEy;
            if (distFromGround < 0) distFromGround = 0;
            int16_t dropY = (int16_t)(collapseT2 * (float)distFromGround);
            fullEy += dropY;
            sy += (int16_t)(collapseT2 * (float)(baseY - sy) * 0.3f);
            if (fullEy > baseY || sy > baseY) continue;
        }

        int16_t ex = sx + (int16_t)((float)(fullEx - sx) * branchProgress);
        int16_t ey = sy + (int16_t)((float)(fullEy - sy) * branchProgress);

        fatLine(canvas, sx, sy, ex, ey, fg);
    }

    // Phase 3: Fruits
    bool showFruits = collapsing || treeGrowth >= 0.75f;
    if (showFruits && treeFruitCount > 0) {
        float fruitProgress = 1.0f;
        if (!collapsing) {
            fruitProgress = (treeGrowth - 0.75f) / 0.25f;
            if (fruitProgress > 1.0f) fruitProgress = 1.0f;
        }

        uint8_t visibleFruits = (uint8_t)((float)treeFruitCount * fruitProgress + 0.5f);
        if (visibleFruits > treeFruitCount) visibleFruits = treeFruitCount;

        for (uint8_t i = 0; i < visibleFruits; i++) {
            const TreeFruit& f = treeFruits[i];
            int16_t fx = bx + f.offsetX + sway;
            int16_t fy = baseY + f.offsetY;

            if (collapsing) {
                int16_t distFromGround = baseY - fy;
                if (distFromGround < 0) distFromGround = 0;
                int16_t fruitDrop = (int16_t)(collapseT2 * (float)distFromGround * 1.5f);
                fy += fruitDrop;
                if (fy > baseY) continue;
            }

            if (treePhase == TreePhase::ALIVE) {
                uint32_t phase = now + (uint32_t)f.bobPhase * 8;
                int wave2 = (int)(phase % 2000);
                int bob = (wave2 < 1000) ? 0 : PX;
                fy += bob;
            }

            fatFruit(canvas, fx, fy, f.radius, bg, fg);
        }
    }

    } // end Phase 2-3

    // --- Dropping fruits ---
    for (uint8_t i = 0; i < MAX_DROPPING; i++) {
        if (!droppingFruits[i].active) continue;

        float t = (float)(now - droppingFruits[i].dropStart) / 1000.0f;
        int16_t fallDist = (int16_t)(0.5f * 800.0f * t * t);
        int16_t currentY = droppingFruits[i].y + fallDist;

        if (currentY >= GRASS_BASE_Y) {
            droppingFruits[i].active = false;

            uint8_t splashCount = 3 + (uint8_t)(esp_random() % 2);
            for (uint8_t s = 0; s < splashCount; s++) {
                FruitSplash& sp = fruitSplashes[fruitSplashIdx];
                fruitSplashIdx = (fruitSplashIdx + 1) % FRUIT_SPLASH_COUNT;
                sp.x = (float)droppingFruits[i].x;
                sp.y = (float)(GRASS_BASE_Y - 2);
                sp.vx = ((float)(esp_random() % 600) - 300.0f) / 100.0f;
                sp.vy = -2.0f - (float)(esp_random() % 300) / 100.0f;
                sp.size = 1 + (uint8_t)(esp_random() % (droppingFruits[i].radius / 2 + 1));
                sp.spawnTime = now;
                sp.active = true;
            }
            continue;
        }

        fatFruit(canvas, droppingFruits[i].x, currentY, droppingFruits[i].radius, bg, fg);
    }
}
