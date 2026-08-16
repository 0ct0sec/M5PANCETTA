/**
 * teleport.cpp — unified particle decompose/reassemble teleport system
 *
 * extracted from menu_pig.cpp's room-to-room particle teleport.
 * now handles cross-mode transitions (IDLE<->MENU<->HUNT) + boot arrival.
 * pure math renderers shared by menu_pig room-to-room and boot intro.
 */

#include "teleport.h"
#include "menu_pig.h"
#include "menu_pig_render.h"
#include "display.h"
#include "../piglet/avatar.h"
#include <math.h>

using MenuPigRender::fastSinf;

namespace Teleport {

// ==[ CROSS-MODE STATE ]== driven by hamlet.cpp
static Phase cmPhase = Phase::NONE;
static Context cmContext = Context::ROOM_TO_ROOM;
static uint32_t cmStart = 0;
static float cmSrcCX, cmSrcCY;
static float cmPortalX, cmPortalY;
static float cmDstCX, cmDstCY;
static MenuPig::TeleportParticleSample cmSourceParticles[MAX_PARTICLES];
static MenuPig::TeleportParticleSample cmDestinationParticles[MAX_PARTICLES];
static uint8_t cmSourceParticleCount = 0;
static uint8_t cmDestinationParticleCount = 0;
static bool cmVoidReady = false;  // one-shot flag for VOID boundary

// ==[ PARTICLE SAMPLING ]== authored procedural pig-cell silhouette

static bool teleportSidePigCellSolid(int col, int row) {
    constexpr int COLS = 18;
    constexpr int ROWS = 11;
    if (col < 0 || col >= COLS || row < 0 || row >= ROWS) return false;

    float dx = (float)(col - 8) / 6.5f;
    float dy = (float)(row - 5) / 3.4f;
    bool body = dx * dx + dy * dy <= 1.0f;
    bool head = col >= 10 && col <= 15 && row >= 2 && row <= 7;
    bool snout = col >= 14 && col <= 17 && row >= 4 && row <= 6;
    bool ears = ((col == 10 || col == 11) && row <= 2) ||
                ((col == 13 || col == 14) && row >= 1 && row <= 2);
    bool legs = (col == 5 || col == 6 || col == 11 || col == 12) &&
                row >= 8 && row <= 10;
    bool tailCurl = (col == 2 && row == 4) ||
                    (col == 1 && row == 3) ||
                    (col == 0 && (row == 2 || row == 3)) ||
                    (col == 1 && row == 2) ||
                    (col == 2 && row == 3);
    return body || head || snout || ears || legs || tailCurl;
}

static bool teleportPigCellSolid(int col, int row, PigSilhouette silhouette) {
    constexpr int COLS = 18;
    constexpr int ROWS = 11;
    if (col < 0 || col >= COLS || row < 0 || row >= ROWS) return false;
    if (silhouette == PigSilhouette::SIDE_LEFT)
        return teleportSidePigCellSolid(COLS - 1 - col, row);
    if (silhouette == PigSilhouette::SIDE_RIGHT)
        return teleportSidePigCellSolid(col, row);

    // Rear view: symmetric ears/haunches, planted legs, and a centered curl.
    float dx = ((float)col - 8.5f) / 6.2f;
    float dy = ((float)row - 5.2f) / 3.6f;
    bool body = dx * dx + dy * dy <= 1.0f;
    bool ears = ((col >= 4 && col <= 6) || (col >= 11 && col <= 13)) &&
                row >= 1 && row <= 2;
    bool earTips = (col == 5 || col == 12) && row == 0;
    bool legs = ((col >= 4 && col <= 6) || (col >= 11 && col <= 13)) &&
                row >= 8 && row <= 10;
    bool tailCurl = (col == 9 && row == 3) ||
                    (col == 10 && row == 3) ||
                    (col == 11 && (row == 3 || row == 4)) ||
                    (col == 10 && row == 5) ||
                    (col == 9 && row == 5);
    return body || ears || earTips || legs || tailCurl;
}

static bool teleportPigCellBoundary(int col, int row,
                                    PigSilhouette silhouette) {
    if (!teleportPigCellSolid(col, row, silhouette)) return false;
    return !teleportPigCellSolid(col - 1, row, silhouette) ||
           !teleportPigCellSolid(col + 1, row, silhouette) ||
           !teleportPigCellSolid(col, row - 1, silhouette) ||
           !teleportPigCellSolid(col, row + 1, silhouette);
}

void samplePigParticles(MenuPig::TeleportParticleSample* out,
                        uint8_t& count, uint8_t maxCount,
                        PigSilhouette silhouette) {
    count = 0;
    if (!out || maxCount == 0) return;

    // Boundary first preserves ears, snout, tail curl and four planted hoof
    // columns even if a caller supplies a smaller particle budget. A sparse,
    // evenly distributed second pass gives the body readable interior mass.
    for (int pass = 0; pass < 2; ++pass) {
        for (int row = 0; row < 11; ++row) {
            for (int col = 0; col < 18; ++col) {
                bool boundary = teleportPigCellBoundary(col, row, silhouette);
                bool interior = teleportPigCellSolid(col, row, silhouette) && !boundary;
                bool selected = pass == 0
                    ? boundary
                    : (interior && ((col * 7 + row * 11) % 5 == 0));
                if (!selected) continue;
                if (count >= maxCount) return;

                uint32_t seed = (uint32_t)(col + 1) * 73u ^
                                (uint32_t)(row + 3) * 151u ^ 0xA7u ^
                                (uint32_t)silhouette * 0x53u;
                seed ^= seed >> 7;
                auto& p = out[count++];
                p.homeX = (int8_t)((col - 8) * 4);
                p.homeY = (int8_t)((row - 5) * 4);
                p.seed = (uint8_t)seed;
            }
        }
    }
}

// ==[ CAT SILHOUETTE ]== authored 13x7 cells over the companion's 52x28
// footprint. Bit 0 of each row is the left column of a right-facing cat: the
// raised tail sits at the left, the head and ears at the right, and the two
// leg pairs are kept apart so the shape still reads as four legs when the
// beam thins it out.
static constexpr int kCatCols = 13;
static constexpr int kCatRows = 7;
static constexpr uint16_t kCatCells[kCatRows] = {
    0x0A00,  // ear tips
    0x1E01,  // crown + muzzle bridge, tail tip
    0x1FF3,  // back line into the skull, tail shaft
    0x1FFE,  // body + snout
    0x0FFC,  // belly + chin
    0x060C,  // hind and fore legs
    0x060C,  // paws on the contact line
};

static bool teleportCatCellSolid(int col, int row, bool faceRight) {
    if (col < 0 || col >= kCatCols || row < 0 || row >= kCatRows) return false;
    const int c = faceRight ? col : (kCatCols - 1 - col);
    return ((kCatCells[row] >> c) & 1u) != 0u;
}

static bool teleportCatCellBoundary(int col, int row, bool faceRight) {
    if (!teleportCatCellSolid(col, row, faceRight)) return false;
    return !teleportCatCellSolid(col - 1, row, faceRight) ||
           !teleportCatCellSolid(col + 1, row, faceRight) ||
           !teleportCatCellSolid(col, row - 1, faceRight) ||
           !teleportCatCellSolid(col, row + 1, faceRight);
}

void sampleCatParticles(MenuPig::TeleportParticleSample* out,
                        uint8_t& count, uint8_t maxCount, bool faceRight) {
    count = 0;
    if (!out || maxCount == 0) return;

    // Same two-pass order as the pig: outline first so ears, tail and paws
    // survive a short budget, interior mass second.
    for (int pass = 0; pass < 2; ++pass) {
        for (int row = 0; row < kCatRows; ++row) {
            for (int col = 0; col < kCatCols; ++col) {
                const bool boundary =
                    teleportCatCellBoundary(col, row, faceRight);
                const bool interior =
                    teleportCatCellSolid(col, row, faceRight) && !boundary;
                const bool selected = pass == 0 ? boundary : interior;
                if (!selected) continue;
                if (count >= maxCount) return;

                uint32_t seed = (uint32_t)(col + 2) * 97u ^
                                (uint32_t)(row + 5) * 181u ^ 0x5Cu ^
                                (faceRight ? 0u : 0x39u);
                seed ^= seed >> 7;
                auto& p = out[count++];
                p.homeX = (int8_t)((col - 6) * 4);
                p.homeY = (int8_t)((row - 3) * 4);
                p.seed = (uint8_t)seed;
            }
        }
    }
}

// ==[ HASH + COLOR HELPERS ]==

static inline uint32_t portalHash(int x, int y, uint32_t salt) {
    uint32_t h = (uint32_t)(x * 73856093u) ^ (uint32_t)(y * 19349663u) ^ salt;
    h ^= (h >> 13);
    h *= 1274126177u;
    h ^= (h >> 16);
    return h;
}

static inline uint16_t particleColor(int16_t px, int16_t py) {
    return Avatar::getHypeColor(px, py);
}

// ==[ DECOMPOSE POSITION ]== particle burst + spiral-collapse to portal

static bool decomposePos(float tEff, float homeX, float homeY, float burstR,
                         float srcCX, float srcCY,
                         float portalCX, float portalCY,
                         float& outX, float& outY) {
    if (tEff >= 1.0f) return false;
    if (tEff < 0.0f) tEff = 0.0f;
    constexpr float PI_F = 3.14159265f;

    float tCenter = tEff * tEff;
    float cx = srcCX + (portalCX - srcCX) * tCenter;
    float cy = srcCY + (portalCY - srcCY) * tCenter;

    float ri = sqrtf(homeX * homeX + homeY * homeY);
    float thetaI = atan2f(homeY, homeX);

    float r, theta;
    if (tEff < 0.15f) {
        float bt = tEff / 0.15f;
        float ease = 1.0f - (1.0f - bt) * (1.0f - bt);
        r = ri + burstR * ease;
        theta = thetaI + 0.8f * PI_F * bt;
    } else {
        float st = (tEff - 0.15f) / 0.85f;
        float peakR = ri + burstR;
        r = peakR * (1.0f - st * st);
        theta = thetaI + 0.8f * PI_F + 3.0f * PI_F * st * (1.0f + st);
    }

    outX = cx + r * cosf(theta);
    outY = cy + r * fastSinf(theta);
    return true;
}

// ==[ REASSEMBLE POSITION ]== particles spiral from portal -> destination

static bool reassemblePos(float tEff, float homeX, float homeY,
                          float portalCX, float portalCY,
                          int destCX, int destCY,
                          float& outX, float& outY) {
    if (tEff <= 0.0f) return false;
    if (tEff > 1.0f) tEff = 1.0f;
    constexpr float PI_F = 3.14159265f;

    float tCenter = 1.0f - (1.0f - tEff) * (1.0f - tEff);
    float cx = portalCX + ((float)destCX - portalCX) * tCenter;
    float cy = portalCY + ((float)destCY - portalCY) * tCenter;

    float ri = sqrtf(homeX * homeX + homeY * homeY);
    float thetaI = atan2f(homeY, homeX);

    float r = ri * powf(tEff, 0.6f);
    float theta = thetaI - 2.0f * PI_F * (1.0f - tEff) * (2.0f - tEff);

    outX = cx + r * cosf(theta);
    outY = cy + r * fastSinf(theta);
    return true;
}

// ==[ PORTAL RING ]== 4px fat-pixel rainbow ring + shimmer edge

void drawPortalRing(M5Canvas& canvas, int cx, int cy, int radius) {
    if (radius < 2) return;
    int rOuter = radius;
    int rInner = radius - 6;
    if (rInner < 0) rInner = 0;
    int rrOut = rOuter * rOuter;
    int rrIn = rInner * rInner;

    for (int py = cy - rOuter; py <= cy + rOuter; py += 4) {
        for (int px = cx - rOuter; px <= cx + rOuter; px += 4) {
            int dx = (px + 2) - cx;
            int dy = (py + 2) - cy;
            int d2 = dx * dx + dy * dy;
            if (d2 > rrOut || d2 < rrIn) continue;
            bool edge = (d2 > (rOuter - 2) * (rOuter - 2));
            if (edge) {
                uint32_t h = (uint32_t)(px * 7919 + py * 6271 + 0x7E01u);
                h ^= h >> 13; h *= 1274126177u; h ^= h >> 16;
                if (h & 0x3u) continue;
            }
            uint16_t c = Avatar::getHypeColor((int16_t)px, (int16_t)py);
            canvas.fillRect(px, py, 4, 4, c);
        }
    }

    int detailOut = rOuter + 2;
    int detailIn = (rInner > 2) ? (rInner - 2) : 0;
    int rrDetailOut = detailOut * detailOut;
    int rrOuterInner = (rOuter > 2) ? (rOuter - 2) * (rOuter - 2) : 0;
    int rrInnerOuter = (rInner + 2) * (rInner + 2);
    int rrDetailIn = detailIn * detailIn;
    for (int py = cy - detailOut; py <= cy + detailOut; py += 4) {
        for (int px = cx - detailOut; px <= cx + detailOut; px += 4) {
            int dx = (px + 2) - cx;
            int dy = (py + 2) - cy;
            int d2 = dx * dx + dy * dy;
            bool onOuterEdge = (d2 <= rrDetailOut && d2 >= rrOuterInner);
            bool onInnerEdge = (rInner > 2 && d2 <= rrInnerOuter && d2 >= rrDetailIn);
            if (!onOuterEdge && !onInnerEdge) continue;
            if ((portalHash(px, py, 0x91E1u ^ (uint32_t)radius) & 0x7u) >= 2u) continue;
            canvas.fillRect(px, py, 4, 4, Avatar::getHypeColor((int16_t)px, (int16_t)py));
        }
    }
}

// ==[ DECOMPOSE PARTICLES ]== full decompose render pass

void drawDecomposeParticles(M5Canvas& canvas,
                            float sourceCenterX, float sourceCenterY,
                            float portalCenterX, float portalCenterY,
                            float collapseT,
                            const MenuPig::TeleportParticleSample* particles,
                            uint8_t particleCount,
                            uint16_t fg, uint16_t bg,
                            bool withPortalRing) {
    if (particleCount == 0) return;

    float t = collapseT;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    uint16_t mid = Display::lerpColor565(bg, fg, 0.5f);

    float maxR = 1.0f;
    for (uint8_t i = 0; i < particleCount; i++) {
        float ri = sqrtf((float)(particles[i].homeX * particles[i].homeX +
                                 particles[i].homeY * particles[i].homeY));
        if (ri > maxR) maxR = ri;
    }
    float burstDist = maxR * 0.6f;

    float portalGrow = t < 0.3f ? t / 0.3f : 1.0f;
    portalGrow = 1.0f - (1.0f - portalGrow) * (1.0f - portalGrow);
    int portalR = (int)((float)PORTAL_RING_R * portalGrow);

    for (int pass = 0; pass < 2; pass++) {
        for (uint8_t i = 0; i < particleCount; i++) {
            const auto& p = particles[i];
            float hx = (float)p.homeX;
            float hy = (float)p.homeY;
            float seedF = (float)p.seed / 255.0f;
            float burstR = burstDist * (0.5f + 0.5f * seedF);

            float ri = sqrtf(hx * hx + hy * hy);
            float stagger = 1.0f - (ri / maxR) * 0.3f - seedF * 0.1f;
            if (stagger < 0.5f) stagger = 0.5f;
            float tEff = t / stagger;

            if (pass == 0) {
                for (int trail = 1; trail <= 2; trail++) {
                    float tTrail = tEff - (0.05f + seedF * 0.03f) * (float)trail;
                    if (tTrail < 0.0f) continue;
                    float tx, ty;
                    if (decomposePos(tTrail, hx, hy, burstR,
                                     sourceCenterX, sourceCenterY,
                                     portalCenterX, portalCenterY, tx, ty)) {
                        int px = ((int)tx) & ~3;
                        int py = ((int)ty) & ~3;
                        uint16_t c = particleColor((int16_t)px, (int16_t)py);
                        (void)mid;
                        canvas.fillRect(px, py, 4, 4, c);
                    }
                }
            } else {
                float px, py;
                if (decomposePos(tEff, hx, hy, burstR,
                                 sourceCenterX, sourceCenterY,
                                 portalCenterX, portalCenterY, px, py)) {
                    int ix = ((int)px) & ~3;
                    int iy = ((int)py) & ~3;
                    uint16_t c = particleColor((int16_t)ix, (int16_t)iy);
                    canvas.fillRect(ix, iy, 4, 4, c);
                }
            }
        }
    }

    if (withPortalRing)
        drawPortalRing(canvas, (int)portalCenterX, (int)portalCenterY, portalR);
}

// ==[ REASSEMBLE PARTICLES ]== full reassemble render pass

void drawReassembleParticles(M5Canvas& canvas,
                             float portalCenterX, float portalCenterY,
                             int destCenterX, int destCenterY,
                             float t,
                             const MenuPig::TeleportParticleSample* particles,
                             int particleCount,
                             uint16_t fg, uint16_t bg,
                             bool withPortalRing) {
    if (particleCount == 0 || t <= 0.0f) return;
    if (t > 1.0f) t = 1.0f;
    uint16_t mid = Display::lerpColor565(bg, fg, 0.5f);

    float maxR = 1.0f;
    for (int i = 0; i < particleCount; i++) {
        float ri = sqrtf((float)(particles[i].homeX * particles[i].homeX +
                                 particles[i].homeY * particles[i].homeY));
        if (ri > maxR) maxR = ri;
    }

    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < particleCount; i++) {
            const auto& p = particles[i];
            float hx = (float)p.homeX;
            float hy = (float)p.homeY;
            float ri = sqrtf(hx * hx + hy * hy);
            float seedF = (float)p.seed / 255.0f;

            float stagger = (ri / maxR) * 0.25f + seedF * 0.1f;
            if (stagger > 0.7f) stagger = 0.7f;
            float tEff = (stagger < 1.0f) ? (t - stagger) / (1.0f - stagger) : 0.0f;

            if (pass == 0) {
                for (int trail = 1; trail <= 2; trail++) {
                    float tTrail = tEff - (0.05f + seedF * 0.03f) * (float)trail;
                    float tx, ty;
                    if (reassemblePos(tTrail, hx, hy,
                                      portalCenterX, portalCenterY,
                                      destCenterX, destCenterY, tx, ty)) {
                        int px = ((int)tx) & ~3;
                        int py = ((int)ty) & ~3;
                        uint16_t c = particleColor((int16_t)px, (int16_t)py);
                        (void)mid;
                        canvas.fillRect(px, py, 4, 4, c);
                    }
                }
            } else {
                float px, py;
                if (reassemblePos(tEff, hx, hy,
                                  portalCenterX, portalCenterY,
                                  destCenterX, destCenterY, px, py)) {
                    int ix = ((int)px) & ~3;
                    int iy = ((int)py) & ~3;
                    uint16_t c = particleColor((int16_t)ix, (int16_t)iy);
                    canvas.fillRect(ix, iy, 4, 4, c);
                }
            }
        }
    }

    // portal shrinks in last 25%
    if (withPortalRing) {
        int portalR = PORTAL_RING_R;
        if (t > 0.75f) {
            float shrinkT = (t - 0.75f) / 0.25f;
            portalR = (int)((float)PORTAL_RING_R * (1.0f - shrinkT * shrinkT));
        }
        drawPortalRing(canvas, (int)portalCenterX, (int)portalCenterY, portalR);
    }
}

// ==[ SETTLE PARTICLES ]== jitter-snap into final positions

void drawSettleParticles(M5Canvas& canvas,
                         int destCenterX, int destCenterY,
                         float t,
                         const MenuPig::TeleportParticleSample* particles,
                         int particleCount,
                         uint16_t fg) {
    if (particleCount == 0) return;
    if (t > 1.0f) t = 1.0f;

    float omega = 8.0f;
    float offset = 3.0f * (1.0f + omega * t) * expf(-omega * t);

    for (int i = 0; i < particleCount; i++) {
        float hx = (float)particles[i].homeX;
        float hy = (float)particles[i].homeY;
        float ri = sqrtf(hx * hx + hy * hy);
        if (ri < 0.5f) ri = 0.5f;

        float scale = 1.0f + offset / ri;
        int px = (destCenterX + (int)(hx * scale)) & ~3;
        int py = (destCenterY + (int)(hy * scale)) & ~3;
        uint16_t c = particleColor((int16_t)px, (int16_t)py);
        (void)fg;
        canvas.fillRect(px, py, 4, 4, c);
    }
}

// ==[ VOID PORTAL ]== pulsing ring during context switch

void drawVoidPortal(M5Canvas& canvas, int portalX, int portalY, float t) {
    constexpr float PI_F = 3.14159265f;
    if (t > 1.0f) t = 1.0f;
    int portalR = PORTAL_RING_R + (int)(5.0f * fastSinf(8.0f * PI_F * t));
    drawPortalRing(canvas, portalX, portalY, portalR);
}

// ==[ CROSS-MODE STATE MACHINE ]==

void startCrossMode(Context ctx,
                    float srcCenterX, float srcCenterY,
                    float portalX, float portalY,
                    float dstCenterX, float dstCenterY,
                    uint32_t now,
                    PigSilhouette source,
                    PigSilhouette destination) {
    cmContext = ctx;
    cmSrcCX = srcCenterX;
    cmSrcCY = srcCenterY;
    cmPortalX = portalX;
    cmPortalY = portalY;
    cmDstCX = dstCenterX;
    cmDstCY = dstCenterY;
    cmStart = now;
    cmPhase = Phase::DECOMPOSE;
    cmVoidReady = false;

    samplePigParticles(cmSourceParticles, cmSourceParticleCount,
                       MAX_PARTICLES, source);
    samplePigParticles(cmDestinationParticles, cmDestinationParticleCount,
                       MAX_PARTICLES, destination);
}

void startBootArrival(float portalX, float portalY,
                      float dstCenterX, float dstCenterY,
                      uint32_t now,
                      PigSilhouette destination) {
    cmContext = Context::BOOT_ARRIVAL;
    cmSrcCX = portalX;
    cmSrcCY = portalY;
    cmPortalX = portalX;
    cmPortalY = portalY;
    cmDstCX = dstCenterX;
    cmDstCY = dstCenterY;
    // offset cmStart so update() timeline lands directly at REASSEMBLE start
    cmStart = now - DECOMPOSE_MS - VOID_MS;
    cmPhase = Phase::REASSEMBLE;
    cmVoidReady = false;

    cmSourceParticleCount = 0;
    samplePigParticles(cmDestinationParticles, cmDestinationParticleCount,
                       MAX_PARTICLES, destination);
}

Phase update(uint32_t now) {
    if (cmPhase == Phase::NONE) return Phase::NONE;

    uint32_t elapsed = now - cmStart;

    // full sequence for all contexts: DECOMPOSE -> VOID -> REASSEMBLE -> SETTLE
    if (elapsed < DECOMPOSE_MS) {
        cmPhase = Phase::DECOMPOSE;
    } else if (elapsed < DECOMPOSE_MS + VOID_MS) {
        if (cmPhase != Phase::VOID) {
            cmPhase = Phase::VOID;
            cmVoidReady = true;  // one-shot: caller must enterMode()
        }
    } else if (elapsed < DECOMPOSE_MS + VOID_MS + REASSEMBLE_MS) {
        cmPhase = Phase::REASSEMBLE;
    } else if (elapsed < TOTAL_MS) {
        cmPhase = Phase::SETTLE;
    } else {
        cmPhase = Phase::NONE;
    }

    return cmPhase;
}

bool isActive() {
    return cmPhase != Phase::NONE;
}

Phase getPhase() {
    return cmPhase;
}

Context getContext() {
    return cmContext;
}

bool isPigHidden() {
    if (cmPhase == Phase::NONE) return false;
    if (cmPhase == Phase::VOID) return true;

    uint32_t elapsed = (cmStart != 0) ? (millis() - cmStart) : 0;

    if (cmPhase == Phase::DECOMPOSE) {
        float t = (float)elapsed / (float)DECOMPOSE_MS;
        // Keep the authored body under the first dense burst. Hand ownership
        // to particles only once the silhouette has enough mass to replace it.
        return t > 0.32f;
    }
    if (cmPhase == Phase::REASSEMBLE) {
        float t = (float)(elapsed - DECOMPOSE_MS - VOID_MS) / (float)REASSEMBLE_MS;
        // Bring the body back under the final quarter of the cell settle so
        // the particle outline does not collapse into a one-frame pop.
        return t < 0.76f;
    }
    return false;
}

bool consumeVoidReady() {
    if (cmVoidReady) {
        cmVoidReady = false;
        return true;
    }
    return false;
}

// ==[ DRAW ]== full cross-mode teleport rendering

void draw(M5Canvas& canvas, uint16_t fg, uint16_t bg, uint32_t now) {
    if (cmPhase == Phase::NONE) return;

    uint32_t elapsed = now - cmStart;

    // full sequence for all contexts
    switch (cmPhase) {
        case Phase::DECOMPOSE: {
            float t = (float)elapsed / (float)DECOMPOSE_MS;
            if (t > 1.0f) t = 1.0f;
            drawDecomposeParticles(canvas,
                                   cmSrcCX, cmSrcCY,
                                   cmPortalX, cmPortalY,
                                   t, cmSourceParticles, cmSourceParticleCount,
                                   fg, bg);
            break;
        }
        case Phase::VOID: {
            float t = (float)(elapsed - DECOMPOSE_MS) / (float)VOID_MS;
            drawVoidPortal(canvas, (int)cmPortalX, (int)cmPortalY, t);
            break;
        }
        case Phase::REASSEMBLE: {
            float t = (float)(elapsed - DECOMPOSE_MS - VOID_MS) / (float)REASSEMBLE_MS;
            if (t > 1.0f) t = 1.0f;
            drawReassembleParticles(canvas,
                                    cmPortalX, cmPortalY,
                                    (int)cmDstCX, (int)cmDstCY,
                                    t, cmDestinationParticles,
                                    (int)cmDestinationParticleCount,
                                    fg, bg);
            break;
        }
        case Phase::SETTLE: {
            float t = (float)(elapsed - DECOMPOSE_MS - VOID_MS - REASSEMBLE_MS) / (float)SETTLE_MS;
            if (t > 1.0f) t = 1.0f;
            drawSettleParticles(canvas,
                                (int)cmDstCX, (int)cmDstCY,
                                t, cmDestinationParticles,
                                (int)cmDestinationParticleCount, fg);
            break;
        }
        default:
            break;
    }
}

// ==[ CHARGE RING ]== A-hold indicator drawn to M5.Display as overlay

void drawChargeRing(int16_t cx, int16_t cy, float progress) {
    if (progress <= 0.0f) return;
    if (progress > 1.0f) progress = 1.0f;

    const int rOut = 30;
    const int rIn = 20;
    const int rrOut = rOut * rOut;
    const int rrIn = rIn * rIn;
    const float twoPi = 2.0f * 3.14159265f;
    const float chargeMax = twoPi * progress;

    uint16_t fg = Display::getColorFG();
    uint16_t bg = Display::getColorBG();

    // BG outline for readability
    int outlineOut = rOut + 4;
    int outlineIn = rIn - 4;
    if (outlineIn < 0) outlineIn = 0;
    int rrOOut = outlineOut * outlineOut;
    int rrOIn = outlineIn * outlineIn;
    for (int y = cy - outlineOut; y <= cy + outlineOut; y += 4) {
        for (int x = cx - outlineOut; x <= cx + outlineOut; x += 4) {
            int dx = (x + 2) - cx;
            int dy = (y + 2) - cy;
            int d2 = dx * dx + dy * dy;
            if (d2 > rrOOut || d2 < rrOIn) continue;
            bool inRing = (d2 <= rrOut && d2 >= rrIn);
            if (!inRing) {
                float angle = atan2f((float)-dy, (float)dx);
                if (angle < 0.0f) angle += twoPi;
                float fromTop = angle + twoPi * 0.25f;
                if (fromTop >= twoPi) fromTop -= twoPi;
                if (fromTop <= chargeMax) {
                    M5.Display.fillRect(x, y, 4, 4, bg);
                }
            }
        }
    }

    // Hype-colored ring fill
    uint32_t ditherSalt = ((uint32_t)(cx & 0xFF) << 16) ^
                          ((uint32_t)(cy & 0xFF) << 8) ^
                          (uint32_t)lroundf(progress * 1024.0f);
    for (int y = cy - rOut; y <= cy + rOut; y += 4) {
        for (int x = cx - rOut; x <= cx + rOut; x += 4) {
            int dx = (x + 2) - cx;
            int dy = (y + 2) - cy;
            int d2 = dx * dx + dy * dy;
            if (d2 > rrOut || d2 < rrIn) continue;

            float angle = atan2f((float)-dy, (float)dx);
            if (angle < 0.0f) angle += twoPi;
            float fromTop = angle + twoPi * 0.25f;
            if (fromTop >= twoPi) fromTop -= twoPi;
            if (fromTop > chargeMax) continue;

            // edge dither
            bool isEdge = (d2 > (rOut - 2) * (rOut - 2)) ||
                          (d2 < (rIn + 2) * (rIn + 2));
            if (isEdge) {
                uint32_t h = portalHash(x, y, ditherSalt);
                if (h & 0x3u) continue;
            }

            uint16_t c = Avatar::getHypeColor((int16_t)x, (int16_t)y);
            M5.Display.fillRect(x, y, 4, 4, c);
        }
    }
}

}  // namespace Teleport
