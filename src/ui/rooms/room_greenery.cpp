/** room_greenery.cpp - rooted, RF-fed living plants for the cozy rooms */

#include "../menu_pig_internal.h"

namespace MenuPig {

namespace {

struct GrowthRuntime {
    float growth = 0.08f;
    uint32_t lastNow = 0;
    uint32_t nextFruitStep = 0;
    uint32_t nextSwayStep = 0;
    uint8_t displayedFruits = 0;
    int8_t sway = 0;
    int8_t swayTarget = 0;
};

struct BranchSpec {
    uint8_t originPct;
    int8_t dx;
    int8_t lift;
};

static GrowthRuntime ramenGrowth;
static GrowthRuntime comfortGrowth;

static constexpr BranchSpec kBranches[] = {
    {44, -20, 14},
    {54,  20, 18},
    {66, -24, 18},
    {78,  24, 16},
    {90, -16, 12},
};

static int absInt(int v) { return v < 0 ? -v : v; }

static int snapRoomSpan(int v) {
    return v >= 0 ? (v & ~(kRoomPX - 1))
                  : -((-v) & ~(kRoomPX - 1));
}

static int snapRoomX(int x) { return snapRoomSpan(x); }

static int snapRoomY(int y) {
    return kRoomY + snapRoomSpan(y - kRoomY);
}

static float clamp01(float v) {
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

static bool deadlineReached(uint32_t now, uint32_t deadline) {
    return deadline == 0u || (int32_t)(now - deadline) >= 0;
}

static int updateTreeSway(GrowthRuntime& runtime, uint32_t now,
                          uint32_t seed) {
    int8_t target = 0;
    if (roomMood.rfActivity > 18 || debugRoamingFrame.active) {
        float wave = fastSinf((float)now * 0.00055f + (float)(seed & 7u));
        if (wave > 0.58f) target = (int8_t)kRoomPX;
        else if (wave < -0.58f) target = (int8_t)-kRoomPX;
        else if (wave > -0.22f && wave < 0.22f) target = 0;
        else target = runtime.swayTarget;
    }
    runtime.swayTarget = target;
    if (runtime.sway != target && deadlineReached(now, runtime.nextSwayStep)) {
        runtime.sway += runtime.sway < target ? kRoomPX : -kRoomPX;
        runtime.nextSwayStep = now + 300u;
    }
    return runtime.sway;
}

static uint8_t updateDisplayedFruits(GrowthRuntime& runtime, uint32_t now,
                                     float growth) {
    if (debugRoamingFrame.active) {
        runtime.displayedFruits = 4u;
        return runtime.displayedFruits;
    }
    uint8_t target = min((uint8_t)5, roomMood.rfFruitCount);
    if (growth < 0.66f) target = 0u;
    if (runtime.displayedFruits != target &&
        deadlineReached(now, runtime.nextFruitStep)) {
        bool rising = runtime.displayedFruits < target;
        if (rising) runtime.displayedFruits++;
        else runtime.displayedFruits--;
        runtime.nextFruitStep = now + (rising ? 420u : 760u);
    }
    return runtime.displayedFruits;
}

static float updateGrowth(GrowthRuntime& runtime, uint32_t now) {
    if (debugRoamingFrame.active) return 0.90f;

    if (runtime.lastNow == 0 || now < runtime.lastNow || now - runtime.lastNow > 5000u) {
        runtime.lastNow = now;
        return runtime.growth;
    }

    uint32_t dt = now - runtime.lastNow;
    runtime.lastNow = now;
    if (dt > 250u) dt = 250u;

    float target = 0.32f + ((float)roomMood.rfActivity / 255.0f) * 0.68f;
    if (roomMood.rfFruitCount > 0 && target < 0.56f) target = 0.56f;
    if (roomMood.captureCount > 0 && target < 0.48f) target = 0.48f;

    float step = (float)dt / (target > runtime.growth ? 24000.0f : 60000.0f);
    if (target > runtime.growth) {
        runtime.growth += step;
        if (runtime.growth > target) runtime.growth = target;
    } else {
        runtime.growth -= step;
        if (runtime.growth < target) runtime.growth = target;
    }
    return clamp01(runtime.growth);
}

static void drawGridStem(M5Canvas& canvas, int sx, int sy, int ex, int ey,
                         uint16_t color) {
    int dx = ex - sx;
    int dy = ey - sy;
    int span = absInt(dx) > absInt(dy) ? absInt(dx) : absInt(dy);
    int steps = span / kRoomPX;
    if (steps < 1) steps = 1;
    for (int i = 0; i <= steps; ++i) {
        int x = snapRoomX(sx + dx * i / steps);
        int y = snapRoomY(sy + dy * i / steps);
        canvas.fillRect(x, y, kRoomPX, kRoomPX, color);
    }
}

static void drawLeafCluster(M5Canvas& canvas, int x, int y, uint32_t seed,
                            uint16_t leaf, uint16_t leafHi) {
    x = snapRoomX(x);
    y = snapRoomY(y);
    canvas.fillRect(x, y, kRoomPX, kRoomPX, leafHi);
    canvas.fillRect(x - kRoomPX, y, kRoomPX, kRoomPX, leaf);
    canvas.fillRect(x + kRoomPX, y, kRoomPX, kRoomPX, leaf);
    canvas.fillRect(x, y - kRoomPX, kRoomPX, kRoomPX, leaf);
    if ((seed & 1u) != 0u)
        canvas.fillRect(x + kRoomPX, y - kRoomPX, kRoomPX, kRoomPX, leafHi);
    else
        canvas.fillRect(x - kRoomPX, y + kRoomPX, kRoomPX, kRoomPX, leafHi);
}

static void drawFruitAura(M5Canvas& canvas, int x, int y,
                          uint16_t color, uint32_t seed, uint32_t now) {
    const int pulse = (int)((now / 420u + seed) % 3u);
    const uint8_t strength = (uint8_t)(24 + pulse * 8);
    static constexpr int8_t kAura[][2] = {
        {-1, 0}, {1, 0}, {0, -1}, {0, 1},
    };
    for (const auto& cell : kAura) {
        int px = x + (int)cell[0] * kRoomPX;
        int py = y + (int)cell[1] * kRoomPX;
        if (px < 0 || px >= SCREEN_WIDTH || py < kRoomY || py >= kFloorY) continue;
        uint16_t base = fastReadPx(canvas, px, py);
        canvas.fillRect(px, py, kRoomPX, kRoomPX,
                        screenBlend565(base, color, strength));
    }
}

static void drawPlanter(M5Canvas& canvas, int rootX, int rootY, bool bathStyle) {
    uint16_t rim = RP::D_WARM;
    uint16_t body = bathStyle ? RP::D_WALL_NEAR : RP::D_FILL;
    canvas.fillRect(rootX - 12, rootY, 24, kRoomPX, rim);
    canvas.fillRect(rootX - 8, rootY + kRoomPX, 16, 8, body);
    canvas.fillRect(rootX - 4, rootY + 8, 8, kRoomPX, RP::D_DEEP);
}

static void drawRootedRfTree(M5Canvas& canvas, uint32_t now,
                             int rootX, int rootY, int maxHeight,
                             uint32_t seed, GrowthRuntime& runtime,
                             bool bathStyle) {
    float growth = updateGrowth(runtime, now);
    int visibleH = snapRoomSpan((int)((float)maxHeight * growth));
    if (visibleH < kRoomPX) visibleH = kRoomPX;

    uint16_t trunk = bathStyle ? RP::D_STRUCT : RP::D_WARM;
    uint16_t leaf = RP::GREEN_DK;
    uint16_t leafHi = bathStyle ? RP::WALL_MID : RP::D_WALL_NEAR;
    int sway = updateTreeSway(runtime, now, seed);

    drawPlanter(canvas, rootX, rootY, bathStyle);
    // A dedicated planter cell remembers real capture evidence even when RF
    // fruit count drops to zero; the dark recess remains when nothing is held.
    canvas.fillRect(rootX + kRoomPX, rootY + 2 * kRoomPX,
                    kRoomPX, kRoomPX,
                    roomMood.captureCount > 0 ? RP::FLUOR : RP::D_DEEP);
    canvas.fillRect(rootX, rootY - visibleH, kRoomPX, visibleH, trunk);
    if (growth > 0.70f)
        canvas.fillRect(rootX - kRoomPX, rootY - visibleH,
                        kRoomPX * 2, kRoomPX, trunk);

    const bool mirror = (seed & 1u) != 0u;
    int tipX[sizeof(kBranches) / sizeof(kBranches[0])] = {};
    int tipY[sizeof(kBranches) / sizeof(kBranches[0])] = {};
    bool tipReady[sizeof(kBranches) / sizeof(kBranches[0])] = {};

    for (uint8_t i = 0; i < sizeof(kBranches) / sizeof(kBranches[0]); ++i) {
        const BranchSpec& branch = kBranches[i];
        float gate = (float)branch.originPct / 100.0f;
        if (growth <= gate) continue;
        float branchT = clamp01((growth - gate) / (1.0f - gate));
        int dir = mirror ? -1 : 1;
        int sx = rootX;
        int branchRise = snapRoomSpan(maxHeight * branch.originPct / 100);
        if (visibleH < branchRise) continue;
        int sy = rootY - branchRise;
        int ex = sx + dir * branch.dx * branchT + sway;
        int ey = sy - branch.lift * branchT;
        drawGridStem(canvas, sx, sy, ex, ey, trunk);
        tipX[i] = ex;
        tipY[i] = ey;
        tipReady[i] = branchT > 0.58f;
        if (tipReady[i]) drawLeafCluster(canvas, ex, ey, seed + i * 17u, leaf, leafHi);
    }

    uint8_t fruits = updateDisplayedFruits(runtime, now, growth);
    uint16_t fruitColor = roomMood.spamActive || roomMood.alertLevel >= 3
        ? RP::SPARK : (roomMood.trackerPresent ? RP::CRT : RP::WARM);
    for (uint8_t i = 0; i < fruits; ++i) {
        uint8_t branchIdx = (uint8_t)((i * 2u + (seed & 1u)) % 5u);
        if (!tipReady[branchIdx]) continue;
        // Fruit stays on its real branch tip. The shared stem sway and aura
        // carry motion without a detached one-cell up/down pop.
        int fx = snapRoomX(tipX[branchIdx] + ((i & 1u) ? kRoomPX : -kRoomPX));
        int fy = snapRoomY(tipY[branchIdx] + kRoomPX);
        drawFruitAura(canvas, fx, fy, fruitColor, seed + i * 29u, now);
        canvas.fillRect(fx, fy, kRoomPX, kRoomPX, fruitColor);
        canvas.fillRect(fx + kPigPX, fy, kPigPX, kPigPX,
                        roomMood.captureCount > i ? RP::FLUOR : RP::D_DEEP);
    }
}

} // namespace

void drawRamenRfGreenery(M5Canvas& canvas, uint32_t now) {
    drawRootedRfTree(canvas, now,
                     kR3_RfRootX, kR3_RfRootY, kR3_RfTreeH,
                     0xA23Eu, ramenGrowth, false);
}

void drawComfortRfGreenery(M5Canvas& canvas, uint32_t now) {
    drawRootedRfTree(canvas, now,
                     kR6_RfRootX, kR6_RfRootY, kR6_RfTreeH,
                     0xB47Au, comfortGrowth, true);
}

} // namespace MenuPig
