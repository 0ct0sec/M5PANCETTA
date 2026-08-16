#include "wardrive_glass.h"

namespace WardriveScene {

static constexpr float PI_F = 3.14159265f;

static constexpr int GLASS_ROWS = (WD_GLASS_B - WD_GLASS_T) / PX + 1;
static int16_t cachedGlassL[GLASS_ROWS];
static int16_t cachedGlassR[GLASS_ROWS];
static int16_t cachedGlassMid[GLASS_ROWS];

void precomputeGlassBounds() {
    for (int i = 0; i < GLASS_ROWS; i++) {
        int y = WD_GLASS_T + i * PX;
        float t = glassProfileT(y);
        float eased = smoothstep01(t);
        float belly = sinf(t * PI_F);
        float curveDelta = CANOPY_CURVE - 1.0f;
        int rawHalfW = 84
                      + (int)(24.0f * t * (1.0f + curveDelta * 0.12f))
                      + (int)(24.0f * eased * (1.0f + curveDelta * 0.18f))
                      + (int)(16.0f * (1.0f + curveDelta * 0.45f) * belly
                              * (1.0f - t * (0.45f - curveDelta * 0.06f)));
        int halfW = q((int)(rawHalfW * 1.3f));  // 30% wider opening
        int left = WD_CANOPY_CX - halfW + SHELL_COVER;
        int right = WD_CANOPY_CX + halfW - SHELL_COVER;
        if (left < 0) left = 0;
        if (right > 320) right = 320;
        if (right < left + PX * 2) {
            int mid = (left + right) / 2;
            left = max(0, mid - PX);
            right = min(320, mid + PX);
        }
        cachedGlassL[i] = (int16_t)left;
        cachedGlassR[i] = (int16_t)right;
        cachedGlassMid[i] = (int16_t)((left + right) / 2);
    }
}

static inline int glassRowIdx(int y) {
    int idx = (clampi(y, WD_GLASS_T, WD_GLASS_B) - WD_GLASS_T) / PX;
    return (idx >= GLASS_ROWS) ? (GLASS_ROWS - 1) : idx;
}

float glassRowTAtY(int y) {
    return glassProfileT(y);
}

void glassBounds(int y, int& left, int& right) {
    int idx = glassRowIdx(y);
    left = cachedGlassL[idx];
    right = cachedGlassR[idx];
}

int glassCenterX(int y) {
    return cachedGlassMid[glassRowIdx(y)];
}

bool insideGlass(int x, int y) {
    if (y < glassOpenTop() || y >= WD_GLASS_B) return false;
    int left, right;
    glassBounds(y, left, right);
    return x >= left && x < right;
}

int snapRainXToPane(int x, int y) {
    x = floorDivPositive(x, PX) * PX;
    if (y < WD_GLASS_T || y >= WD_GLASS_B) return x;

    int left, right;
    glassBounds(y, left, right);
    int minX = left + PX;
    int maxX = right - PX * 2;
    if (x < minX) x = minX;
    if (x > maxX) x = maxX;
    return floorDivPositive(x, PX) * PX;
}

} // namespace WardriveScene
