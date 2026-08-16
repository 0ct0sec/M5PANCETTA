#include "pancetta_cat_rig.h"

#include "display.h"
#include "menu_pig_render.h"
#include "../piglet/pancetta_body_mask.h"

#include <math.h>
#include <stdlib.h>
#ifdef HAMLET_SIM
#include <stdio.h>
#endif

namespace PancettaCat {
namespace Rig {
namespace {

using namespace MenuPigRender;

struct Palette {
    uint16_t white;
    uint16_t light;
    uint16_t shade;
    uint16_t shadow;
    uint16_t ink;
    uint16_t eyeBase;
    uint16_t eyeIris;
    uint16_t pink;
    uint16_t mouthRim;
    uint16_t mouthInner;
};

enum class CoatPlane : uint8_t {
    NONE,
    SOFT,
    SHADE,
};

enum class RasterRole : uint8_t {
    SURFACE,
    COAT_SOFT,
    COAT_SHADE,
    EYE_BASE,
    INK,
    IRIS,
    ACCENT,
};
static_assert((uint8_t)RasterRole::ACCENT < 8u,
              "cat raster roles must fit three row bitplanes");

static CoatPlane coatPlaneForRole(RasterRole role) {
    if (role == RasterRole::COAT_SOFT) return CoatPlane::SOFT;
    if (role == RasterRole::COAT_SHADE) return CoatPlane::SHADE;
    return CoatPlane::NONE;
}

static uint8_t featurePriorityForRole(RasterRole role) {
    switch (role) {
        case RasterRole::ACCENT: return 6u;
        case RasterRole::IRIS: return 5u;
        case RasterRole::INK: return 4u;
        case RasterRole::EYE_BASE: return 3u;
        default: return 0u;
    }
}

// The canonical 4px cat is derived from the completed live pose, not from a
// second anatomy that could drift away from the animation rig. This buffer
// records the final painter's-order color and semantic role of the 26x14
// construction cells inside the public footprint; each 2x2 group becomes one
// 4px output cell. Roles stay separate from RGB565 because valid themed colors
// can quantize to the same value without becoming the same material.
static constexpr int kFineCols = kWidth / kCellPixels;
static constexpr int kFineRows = kHeight / kCellPixels;
static constexpr int kFootprintInsetCols =
    kFootprintInsetX / kCellPixels;
static constexpr int kFootprintInsetRows =
    kFootprintInsetY / kCellPixels;
static_assert(kFineCols == kOutputCanvasCols * 2 &&
                  kFineRows == kOutputCanvasRows * 2,
              "cat output must resolve the construction rig exactly 2:1");
static_assert(kFineCols <= 32,
              "cat construction rows must fit the occupancy bitmask");
static_assert(kFootprintInsetX % kCellPixels == 0 &&
                  kFootprintInsetY % kCellPixels == 0,
              "cat footprint must begin on the current-resolution lattice");

struct FineRaster {
    uint16_t color[kFineRows][kFineCols] = {};
    uint32_t occupiedRows[kFineRows] = {};
    uint32_t roleBit0Rows[kFineRows] = {};
    uint32_t roleBit1Rows[kFineRows] = {};
    uint32_t roleBit2Rows[kFineRows] = {};
#ifdef NATIVE_TEST
    uint32_t directionalRows[kFineRows] = {};
#endif

    void set(int row, int col, uint16_t value,
             RasterRole role = RasterRole::SURFACE,
             bool directional = false) {
        const uint32_t bit = (uint32_t)1u << col;
        const uint8_t roleBits = (uint8_t)role;
        color[row][col] = value;
        occupiedRows[row] |= bit;
        roleBit0Rows[row] = (roleBit0Rows[row] & ~bit) |
                            ((roleBits & 1u) ? bit : 0u);
        roleBit1Rows[row] = (roleBit1Rows[row] & ~bit) |
                            ((roleBits & 2u) ? bit : 0u);
        roleBit2Rows[row] = (roleBit2Rows[row] & ~bit) |
                            ((roleBits & 4u) ? bit : 0u);
#ifdef NATIVE_TEST
        directionalRows[row] &= ~bit;
#endif
#ifdef NATIVE_TEST
        if (directional) directionalRows[row] |= bit;
#else
        (void)directional;
#endif
    }

    bool occupied(int row, int col) const {
        return (occupiedRows[row] & ((uint32_t)1u << col)) != 0u;
    }

    RasterRole role(int row, int col) const {
        const uint32_t bit = (uint32_t)1u << col;
        return (RasterRole)(
            ((roleBit0Rows[row] & bit) ? 1u : 0u) |
            ((roleBit1Rows[row] & bit) ? 2u : 0u) |
            ((roleBit2Rows[row] & bit) ? 4u : 0u));
    }

#ifdef NATIVE_TEST
    bool directional(int row, int col) const {
        return (directionalRows[row] & ((uint32_t)1u << col)) != 0u;
    }
#endif
};
#ifdef NATIVE_TEST
static constexpr size_t kFineRasterMaskCount = 5u;
#else
static constexpr size_t kFineRasterMaskCount = 4u;
#endif
static_assert(sizeof(FineRaster) ==
                  kFineRows * (kFineCols * sizeof(uint16_t) +
                               kFineRasterMaskCount * sizeof(uint32_t)),
              "cat scratch raster must stay compact and allocation-free");

#ifdef NATIVE_TEST
// Records the last color written to one local cell. Sampling through the real
// composition path is the only way an audit can assert that two poses differ
// visually without re-implementing the renderer and drifting from it.
struct CellProbe {
    int x = 0;
    int y = 0;
    uint16_t color = 0;
    bool hit = false;
};
#endif

struct DrawContext {
    M5Canvas* canvas;
    int originX;
    int originY;
    bool faceRight;
    int8_t keyLocalSide;
    Palette pal;
    Bounds* bounds;
    FineRaster* fineRaster;
#ifdef NATIVE_TEST
    CellProbe* probe = nullptr;
#endif
};

struct BodyGeometry {
    int x;
    int y;
};

static constexpr int kTorsoStretchCells = 1;
static constexpr int kTorsoStretchSeam = 8;
static constexpr int kCatTorsoCols =
    PancettaBodyMask::kCols + kTorsoStretchCells;
static constexpr int kHeadShiftCells = kTorsoStretchCells;
static constexpr int kCatChamferTopRow = 3;
static constexpr int kCatChamferBottomRow =
    PancettaBodyMask::kRows - 1;
static constexpr int kCatTopCornerInsetCells = 1;
static constexpr int kCatBellyCornerInsetCells = 2;
static constexpr int kHindFarStubbyX = 3;
static constexpr int kHindNearStubbyX = 7;
static constexpr int kFrontFarStubbyX = 12;
static constexpr int kFrontNearStubbyX = 16;
static constexpr int kPlantedPawWidth = 4;
static constexpr int kRaisedPawWidth = 3;
static constexpr int kEyeWidth = 2;
static constexpr int kEyeHeight = 2;
static constexpr int kMouthWidth = 1;
// A meow has to survive being 2px per cell. The open muzzle is therefore an
// authored oval rather than a scaled-up version of the resting mark: five
// cells across and three tall, centered on the same nose column so the face
// never slides off its established axis. Both spans stay inside the copied
// body mask, so opening the mouth cannot grow the measured footprint.
static constexpr int kMeowMouthHalfWidth = 2;
static constexpr int kMeowMouthRows = 3;
static constexpr int kChirpMouthHalfWidth = 1;
static constexpr int kChirpMouthRows = 2;
static_assert(kPlantedPawWidth == 4 && kRaisedPawWidth == 3,
              "cat paws must keep broad filled attachment rows");
static_assert(kEyeWidth == 2 && kEyeHeight == 2 && kMouthWidth == 1,
              "cat face must keep fixed eyes and a one-cell resting muzzle");
static_assert(kMeowMouthHalfWidth > kChirpMouthHalfWidth &&
                  kMeowMouthRows > kChirpMouthRows,
              "a meow must read as a larger opening than an ordinary chirp");
static constexpr float kCatToneStep = 0.04f;
static constexpr int kCatToneLiftSteps = 2;
static constexpr int kCatShadeSteps = 3;
static constexpr int kCatShadowSteps = 4;
static constexpr float kCatShadowDeepMix = 0.06f;
static constexpr float kCenteredKeyDeadbandPx = kCellPixels * 2.0f;
static_assert(kCatToneLiftSteps == 2,
              "companion coat must stay two tone steps above Pancetta");
static_assert(kCatShadeSteps < kCatShadowSteps && kCatShadowSteps <= 4,
              "cat fur depth must stay soft and close-valued");
static_assert(kCatShadowDeepMix <= 0.06f,
              "cat shadow must never become a dark outline");
static_assert(kCatTorsoCols == kWhiteCat.torsoW,
              "coat map must share the authored torso width");

struct CoatPatch {
    int8_t x;
    int8_t y;
    uint8_t width;
    CoatPlane plane;
};

// Canonical patches describe the side away from a key on the cat's local
// right. Rendering mirrors them when the sampled practical moves across the
// body. Short, staggered runs make cheek, shoulder, rib, haunch, and belly
// turn as fur masses; no full row can become a hard stripe.
static constexpr CoatPatch kSideCoatPatches[] = {
    {4, 1, 2, CoatPlane::SOFT},
    {4, 2, 1, CoatPlane::SOFT},
    {2, 3, 2, CoatPlane::SOFT},
    {1, 4, 3, CoatPlane::SOFT},
    {2, 5, 2, CoatPlane::SOFT},
    {2, 6, 1, CoatPlane::SOFT},
    {1, 7, 3, CoatPlane::SOFT},
    {2, 8, 2, CoatPlane::SOFT},
    {3, 9, 3, CoatPlane::SOFT},
    {2, 4, 1, CoatPlane::SHADE},
    {1, 5, 1, CoatPlane::SHADE},
    {2, 7, 1, CoatPlane::SHADE},
    {3, 8, 1, CoatPlane::SHADE},
    {5, 9, 1, CoatPlane::SHADE},
};

// These small underside islands stay anatomical instead of following the
// light laterally. Their broken rhythm rounds the chest and belly without a
// ruler-straight dark hem under the white silhouette.
static constexpr CoatPatch kUndersideCoatPatches[] = {
    {8, 7, 1, CoatPlane::SOFT},
    {6, 8, 2, CoatPlane::SOFT},
    {10, 8, 1, CoatPlane::SHADE},
    {13, 8, 2, CoatPlane::SOFT},
    {7, 9, 1, CoatPlane::SOFT},
    {8, 9, 2, CoatPlane::SHADE},
    {11, 9, 1, CoatPlane::SHADE},
    {12, 9, 1, CoatPlane::SOFT},
};

static constexpr int catBodyInsetForRow(int row) {
    return row == kCatChamferTopRow
        ? kCatTopCornerInsetCells
        : (row == kCatChamferBottomRow
            ? kCatBellyCornerInsetCells
            : 0);
}

static constexpr int catBodyLeftForRow(int row) {
    return PancettaBodyMask::kRowLeft[row] +
        (PancettaBodyMask::kRowLeft[row] > kTorsoStretchSeam
            ? kTorsoStretchCells : 0) +
        catBodyInsetForRow(row);
}

static constexpr int catBodyRightForRow(int row) {
    return PancettaBodyMask::kRowRight[row] +
        (PancettaBodyMask::kRowRight[row] > kTorsoStretchSeam
            ? kTorsoStretchCells : 0) -
        catBodyInsetForRow(row);
}

static_assert(
    catBodyRightForRow(kCatChamferBottomRow) -
            catBodyLeftForRow(kCatChamferBottomRow) + 1 >=
        kWhiteCat.torsoW / 2,
    "rounded belly must retain a broad load-bearing center");

static int clampCell(int value, int low, int high) {
    return value < low ? low : (value > high ? high : value);
}

static int halfMotion(int value) {
    if (value == 0) return 0;
    return value > 0 ? (value + 1) / 2 : -((-value + 1) / 2);
}

static int localToScreenX(const DrawContext& ctx, int x, int w = 1) {
    const int local = ctx.faceRight
        ? x
        : (int)kWhiteCat.canvasCols - x - w;
    return ctx.originX + local * kWhiteCat.cellPixels;
}

static int localToScreenY(const DrawContext& ctx, int y) {
    return ctx.originY + y * kWhiteCat.cellPixels;
}

static void fillCells(DrawContext& ctx, int x, int y, int w, int h,
                      uint16_t color,
                      RasterRole role = RasterRole::SURFACE,
                      bool directional = false) {
    if (w <= 0 || h <= 0) return;
#ifdef NATIVE_TEST
    if (ctx.probe && ctx.probe->x >= x && ctx.probe->x < x + w &&
        ctx.probe->y >= y && ctx.probe->y < y + h) {
        // Painter's order: a later layer legitimately overwrites an earlier
        // one, so the final write is the visible color.
        ctx.probe->color = color;
        ctx.probe->hit = true;
    }
#endif
    if (ctx.fineRaster) {
        const int localX = ctx.faceRight
            ? x
            : (int)kWhiteCat.canvasCols - x - w;
        for (int dy = 0; dy < h; ++dy) {
            const int row = y + dy - kFootprintInsetRows;
            if (row < 0 || row >= kFineRows) continue;
            for (int dx = 0; dx < w; ++dx) {
                const int col = localX + dx - kFootprintInsetCols;
                if (col < 0 || col >= kFineCols) continue;
                ctx.fineRaster->set(row, col, color, role, directional);
            }
        }
    }
    const int sx = localToScreenX(ctx, x, w);
    const int sy = localToScreenY(ctx, y);
    const int sw = w * kWhiteCat.cellPixels;
    const int sh = h * kWhiteCat.cellPixels;
    if (ctx.bounds) {
        if (!ctx.bounds->valid) {
            ctx.bounds->left = (int16_t)sx;
            ctx.bounds->top = (int16_t)sy;
            ctx.bounds->right = (int16_t)(sx + sw);
            ctx.bounds->bottom = (int16_t)(sy + sh);
            ctx.bounds->valid = true;
        } else {
            ctx.bounds->left = (int16_t)min((int)ctx.bounds->left, sx);
            ctx.bounds->top = (int16_t)min((int)ctx.bounds->top, sy);
            ctx.bounds->right =
                (int16_t)max((int)ctx.bounds->right, sx + sw);
            ctx.bounds->bottom =
                (int16_t)max((int)ctx.bounds->bottom, sy + sh);
        }
    }
    if (!ctx.canvas) return;
    if (sx >= ctx.canvas->width() || sy >= ctx.canvas->height() ||
        sx + sw <= 0 || sy + sh <= 0)
        return;
    ctx.canvas->fillRect(sx, sy, sw, sh, color);
}

static void fillCell(DrawContext& ctx, int x, int y, uint16_t color,
                     RasterRole role = RasterRole::SURFACE) {
    fillCells(ctx, x, y, 1, 1, color, role);
}

static void connectedCellLine(DrawContext& ctx, int x0, int y0,
                              int x1, int y1, uint16_t color) {
    int dx = abs(x1 - x0);
    const int sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0);
    const int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    while (true) {
        fillCell(ctx, x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        const int oldX = x0;
        const int oldY = y0;
        const int e2 = err * 2;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
        if (x0 != oldX && y0 != oldY)
            fillCell(ctx, x0, oldY, color);
    }
}

struct RibbonOffset {
    int x;
    int y;
};

static RibbonOffset tailRibbonOffset(int x0, int y0, int x1, int y1) {
    // Thickness belongs across the segment, never along it. A fixed X offset
    // makes horizontal runs overlap their first stroke and collapse to one
    // 2px row, which is why LEVEL and the lower half of curls looked clipped
    // even though the footprint still had its full side guard.
    return abs(x1 - x0) >= abs(y1 - y0)
        ? RibbonOffset{0, -1}
        : RibbonOffset{1, 0};
}

static BodyGeometry bodyGeometry(const Pose& pose) {
    // The old companion used pose offsets at full size. The cat halves those
    // offsets while retaining the roomy action box for tail, grooming, and
    // airborne motion.
    BodyGeometry body = {
        kWhiteCat.torsoX + clampCell(halfMotion(pose.bodyDx), -1, 1),
        kWhiteCat.torsoY + clampCell(halfMotion(pose.bodyDy), -1, 1),
    };
    int stanceDrop = 0;
    switch (pose.stance) {
        case Stance::CROUCH: stanceDrop = 1; break;
        case Stance::LOAF:
        case Stance::CURL: stanceDrop = 2; break;
        case Stance::SIT:
        case Stance::STAND:
        case Stance::STRETCH:
        case Stance::ARCH:
        case Stance::SCRATCH:
        default: break;
    }
    body.y += stanceDrop;
    const bool airborne = pose.gait == Gait::BOUND_EXTEND ||
                          pose.gait == Gait::BOUND_TUCK;
    if (!airborne) {
        // Grounded paws own the support row. Do not let a torso bob rise off
        // their attachment overlap and reveal the narrow top as a shin.
        body.y = max(body.y, (int)kWhiteCat.torsoY);
    }
    if (pose.gait == Gait::PLANT && pose.stance == Stance::STAND) {
        // A stopped body keeps a fixed shoulder line. The visible stubbies,
        // not a dropped belly, own support while head/tail beats animate.
        body.y = kWhiteCat.torsoY + kStoppedBodyDropRows;
    }
    body.y = min(body.y, (int)kWhiteCat.groundRow -
                           (int)kWhiteCat.torsoH + 1);
    return body;
}

static void drawCatEarMask(DrawContext& ctx, const Pose& pose,
                           const BodyGeometry& body) {
    // Preserve the established skull exactly. The torso extension translates
    // the whole head by one cell; it does not widen the eye or ear spacing.
    static constexpr int kRearBase = 4 + kHeadShiftCells;
    static constexpr int kFrontBase = 13 + kHeadShiftCells;
    int rearTip = kRearBase;
    int frontTip = kFrontBase;
    switch (pose.ears) {
        case EarPose::FORWARD: rearTip += 1; frontTip += 1; break;
        case EarPose::BACK: rearTip -= 1; frontTip -= 1; break;
        case EarPose::ALERT: break;
        case EarPose::NEUTRAL: default: break;
    }
    rearTip = clampCell(rearTip, kRearBase - 1, kRearBase + 1);
    frontTip = clampCell(frontTip, kFrontBase - 1, kFrontBase + 1);

    // Fixed three-cell bases plus a higher single-cell tip read as feline
    // triangles. The tips may lean, but the roots stay planted in the skull;
    // this avoids the paired round nubs that made the miniature read as a
    // mouse. Preserve the measured one-cell guard above every motion extreme.
    const int tipY = max(body.y - 1,
                         kMinimumTopClearance / kCellPixels);
    fillCells(ctx, body.x + kRearBase - 1, body.y, 3, 1, ctx.pal.light);
    fillCells(ctx, body.x + kFrontBase - 1, body.y, 3, 1, ctx.pal.light);
    connectedCellLine(ctx, body.x + rearTip, tipY,
                      body.x + kRearBase, body.y, ctx.pal.white);
    connectedCellLine(ctx, body.x + frontTip, tipY,
                      body.x + kFrontBase, body.y, ctx.pal.white);
    fillCell(ctx, body.x + kRearBase, body.y, ctx.pal.pink,
             RasterRole::ACCENT);
    fillCell(ctx, body.x + kFrontBase, body.y, ctx.pal.pink,
             RasterRole::ACCENT);
}

static int coatPatchX(const CoatPatch& patch, int8_t keyLocalSide) {
    // Patches are authored for shadow on local left, which means a key on the
    // local right. The torso is 19 construction cells wide while the canonical
    // footprint is an even 26. Mirroring only inside the odd torso puts the two
    // shadow maps on different halves of a 4px output cell. The one-cell phase
    // correction balances coarse coverage before facial features legitimately
    // replace some front-side fur.
    return keyLocalSide >= 0
        ? patch.x
        : (int)kWhiteCat.torsoW - 1 - patch.x - patch.width;
}

static int8_t localKeySide(bool faceRight, bool hasKey, float dx) {
    // A source within one scenery cell of the cat's center is overhead for
    // this 2px actor. Suppressing the lateral plane there avoids inventing a
    // left/right shadow and prevents a hard side swap as the cat crosses it.
    if (!hasKey || fabsf(dx) < kCenteredKeyDeadbandPx) return 0;
    return (dx > 0.0f) == faceRight ? (int8_t)1 : (int8_t)-1;
}

static uint16_t coatPatchColor(const DrawContext& ctx, CoatPlane plane) {
    return plane == CoatPlane::SHADE ? ctx.pal.shade : ctx.pal.light;
}

static RasterRole coatRasterRole(CoatPlane plane) {
    return plane == CoatPlane::SHADE
        ? RasterRole::COAT_SHADE : RasterRole::COAT_SOFT;
}

static void drawCoatPatch(DrawContext& ctx, const BodyGeometry& body,
                          const CoatPatch& patch, bool followsKey) {
    const int patchX = followsKey
        ? coatPatchX(patch, ctx.keyLocalSide)
        : patch.x;
    fillCells(ctx, body.x + patchX, body.y + patch.y,
              patch.width, 1, coatPatchColor(ctx, patch.plane),
              coatRasterRole(patch.plane), followsKey);
}

static void drawSoftCoatVolume(DrawContext& ctx,
                               const BodyGeometry& body) {
    if (ctx.keyLocalSide != 0) {
        for (const CoatPatch& patch : kSideCoatPatches)
            drawCoatPatch(ctx, body, patch, true);
    }
    for (const CoatPatch& patch : kUndersideCoatPatches)
        drawCoatPatch(ctx, body, patch, false);
}

static void drawMiniPancettaBody(DrawContext& ctx, const Pose& pose,
                                 const BodyGeometry& body) {
    static_assert(PancettaBodyMask::kCols + kTorsoStretchCells ==
                      kWhiteCat.torsoW &&
                      PancettaBodyMask::kRows == kWhiteCat.torsoH,
                  "cat torso must retain the Detective row-profile dimensions");
    drawCatEarMask(ctx, pose, body);
    for (int row = 1; row < PancettaBodyMask::kRows; ++row) {
        const int stretchedLeft = catBodyLeftForRow(row);
        const int stretchedRight = catBodyRightForRow(row);
        fillCells(ctx, body.x + stretchedLeft, body.y + row,
                  stretchedRight - stretchedLeft + 1, 1, ctx.pal.white);
    }

    // The shared body mask stops one cell short of the declared skull on its
    // upper-front row. Head-up plus a positive face offset could therefore
    // place the outer eye on background. Complete the authored 12-cell skull
    // before painting facial detail, then close the single-cell cheek notch
    // between two otherwise full head rows.
    const int headLeft = kWhiteCat.headX - kWhiteCat.torsoX;
    fillCells(ctx, body.x + headLeft, body.y + 2,
              kWhiteCat.headW, 1, ctx.pal.white);
    fillCell(ctx, body.x + catBodyRightForRow(5), body.y + 6,
             ctx.pal.white);

    // Volume stays inside the established cute silhouette. The sampled room
    // practical chooses the soft side; attached anatomical islands turn the
    // chest and belly without outlining the cat or drawing a straight band.
    drawSoftCoatVolume(ctx, body);
}

static void drawTail(DrawContext& ctx, const Pose& pose,
                     const BodyGeometry& body,
                     const SecondaryMotion& secondary) {
    const int sweep = clampCell(halfMotion(secondary.sweepCells), -1, 1);
    const int lift = clampCell(halfMotion(secondary.liftCells), -1, 1);
    const int rootX = body.x + 3;
    const int rootY = body.y + 7;
    const int outerX = max(kWhiteCat.torsoX - 3, body.x - 3);
    const int bendX = max(kWhiteCat.torsoX - 2, body.x - 1);
    // The root stays under the rump, but the outer joints must clear the
    // copied body mask. Keeping every point at body.x or body.x+1 left only a
    // single 2px cell after the body layer covered the tail.
    int tipX = outerX;
    int tipY = rootY;
    int elbowX = bendX;
    int elbowY = rootY;

    const auto drawTailRibbon = [&ctx](int x0, int y0, int x1, int y1) {
        const RibbonOffset offset = tailRibbonOffset(x0, y0, x1, y1);
        connectedCellLine(ctx, x0 + offset.x, y0 + offset.y,
                          x1 + offset.x, y1 + offset.y, ctx.pal.shadow);
        connectedCellLine(ctx, x0, y0, x1, y1, ctx.pal.light);
    };

    switch (pose.tail) {
        case TailPose::TRAIL_LOW:
            elbowX = bendX; elbowY = rootY + 2;
            tipX = outerX; tipY = rootY + 3;
            break;
        case TailPose::LEVEL:
            elbowX = bendX; elbowY = rootY;
            tipX = outerX; tipY = rootY - 1;
            break;
        case TailPose::UP:
            elbowX = bendX; elbowY = rootY - 3;
            tipX = outerX; tipY = body.y + 1;
            break;
        case TailPose::CURL:
            drawTailRibbon(rootX, rootY, bendX, rootY - 2);
            drawTailRibbon(bendX, rootY - 2, outerX, rootY - 4);
            drawTailRibbon(outerX, rootY - 4, bendX, rootY - 5);
            drawTailRibbon(bendX, rootY - 5, body.x, rootY - 3);
            fillCell(ctx, rootX, rootY, ctx.pal.shade);
            return;
        case TailPose::TUCK:
            elbowX = body.x + 1; elbowY = rootY + 2;
            tipX = body.x + 4; tipY = rootY + 3;
            break;
        case TailPose::PUFF:
            elbowX = bendX; elbowY = rootY - 3;
            tipX = outerX; tipY = body.y;
            break;
    }

    tipX = clampCell(tipX + sweep, kWhiteCat.torsoX - 3,
                     (int)kWhiteCat.canvasCols - 1);
    tipY = clampCell(tipY + lift,
                     kMinimumTopClearance / kCellPixels,
                     (int)kWhiteCat.canvasRows - 1);
    elbowX = clampCell(elbowX, 0, (int)kWhiteCat.canvasCols - 1);
    elbowY = clampCell(elbowY, 0, (int)kWhiteCat.canvasRows - 1);
    drawTailRibbon(rootX, rootY, elbowX, elbowY);
    drawTailRibbon(elbowX, elbowY, tipX, tipY);
    fillCell(ctx, rootX, rootY, ctx.pal.shade);
}

struct PawGeometry {
    int centerX;
    int bottomY;
};

static PawGeometry pawGeometry(const Pose& pose,
                               const BodyGeometry& body,
                               bool far, bool front) {
    // Duplicate the established two-stubby anchors symmetrically: 3/7 around
    // the old hind center at 5, and 12/16 around the old front center at 14.
    // Each depth pair remains separated by a full four-cell stubby width.
    int centerX = body.x +
        (front ? (far ? kFrontFarStubbyX : kFrontNearStubbyX)
               : (far ? kHindFarStubbyX : kHindNearStubbyX));
    int bottomY = kWhiteCat.groundRow;

    switch (pose.gait) {
        case Gait::STEP_A:
            if ((!front && far) || (front && !far))
                centerX += front ? 1 : -1;
            break;
        case Gait::STEP_B:
            if ((!front && !far) || (front && far))
                centerX += front ? -1 : 1;
            break;
        case Gait::PAW_LIFT:
            if (front && !far) {
                centerX = body.x + kWhiteCat.torsoW - 1;
                bottomY = body.y + kWhiteCat.torsoH - 1;
            }
            break;
        case Gait::SCRATCH_A:
            if (front && !far) {
                centerX = body.x + kWhiteCat.torsoW - 1;
                bottomY = body.y + kWhiteCat.torsoH - 1;
            }
            break;
        case Gait::SCRATCH_B:
            if (front && far) {
                centerX = body.x + kWhiteCat.torsoW - 1;
                bottomY = body.y + kWhiteCat.torsoH - 1;
            }
            break;
        case Gait::BOUND_EXTEND:
            centerX += front ? 1 : -1;
            bottomY = body.y + kWhiteCat.torsoH + 1;
            break;
        case Gait::BOUND_TUCK:
            bottomY = body.y + kWhiteCat.torsoH;
            break;
        case Gait::STRIDE_PLANT:
        case Gait::PLANT:
        default:
            break;
    }

    if (pose.stance == Stance::STRETCH &&
               pose.gait != Gait::BOUND_EXTEND &&
               pose.gait != Gait::BOUND_TUCK) {
        centerX += front ? 1 : -1;
    }

    centerX = clampCell(centerX, 2, (int)kWhiteCat.canvasCols - 2);
    bottomY = clampCell(bottomY, body.y + kWhiteCat.torsoH - 3,
                        kWhiteCat.groundRow);
    return {centerX, bottomY};
}

static bool raisedOnPlane(const Pose& pose, bool far) {
    return (!far && (pose.gait == Gait::PAW_LIFT ||
                     pose.gait == Gait::SCRATCH_A)) ||
           (far && pose.gait == Gait::SCRATCH_B);
}

static void drawFilledPaw(DrawContext& ctx, const PawGeometry& paw,
                          bool far, bool raised) {
    const uint16_t pawColor = far ? ctx.pal.shade : ctx.pal.white;
    const uint16_t edge = far ? ctx.pal.shadow : ctx.pal.light;
    if (raised) {
        // The lifted forepaw is the same filled bump rotated into the chest,
        // not a second articulated leg. Its broad body-side edge overlaps the
        // torso; no thin connector can turn it into a side snout.
        fillCells(ctx, paw.centerX - 1, paw.bottomY - 2,
                  kRaisedPawWidth, 2, pawColor);
        fillCells(ctx, paw.centerX - 1, paw.bottomY, 2, 1, edge);
        return;
    }

    // Both attachment and barrel rows stay four cells wide. The belly normally
    // masks the upper row, but a chamfer or eased pose may uncover it; keeping
    // that row filled preserves the cute bump instead of revealing a shin.
    fillCells(ctx, paw.centerX - 2, paw.bottomY - 2,
              kPlantedPawWidth, 2, pawColor);
    fillCells(ctx, paw.centerX - 1, paw.bottomY, 2, 1, edge);
}

static void drawPawLayer(DrawContext& ctx, const Pose& pose,
                         const BodyGeometry& body, bool far) {
    drawFilledPaw(ctx, pawGeometry(pose, body, far, false), far, false);
    if (!raisedOnPlane(pose, far))
        drawFilledPaw(ctx, pawGeometry(pose, body, far, true),
                      far, false);
}

static void drawRaisedForepaw(DrawContext& ctx, const Pose& pose,
                              const BodyGeometry& body, bool far) {
    if (!raisedOnPlane(pose, far)) return;
    // Genuine lift/groom/scratch beats repaint one joined bump after the body.
    // It remains below the muzzle and overlaps the chest by a full cell.
    const PawGeometry paw = pawGeometry(pose, body, far, true);
    drawFilledPaw(ctx, paw, far, true);
    fillCell(ctx, paw.centerX - 1, paw.bottomY - 1, ctx.pal.shadow);
}

struct FaceGeometry {
    int eyeY;
    int backEyeX;
    int frontEyeX;
    int noseX;
    int noseY;
};

static FaceGeometry faceGeometry(const Pose& pose,
                                 const BodyGeometry& body) {
    int dx = clampCell(halfMotion(pose.headDx), -1, 1);
    int dy = clampCell(halfMotion(pose.headDy), -1, 1);
    switch (pose.head) {
        case HeadPose::DOWN: dy += 1; break;
        case HeadPose::UP: dy -= 1; break;
        case HeadPose::GROOM_PAW: dx -= 1; dy += 1; break;
        case HeadPose::GROOM_FLANK: dx -= 1; dy += 1; break;
        case HeadPose::GAG: dy += 1; break;
        case HeadPose::FRONT:
        case HeadPose::LEVEL:
        default: break;
    }
    dx = clampCell(dx, -1, 1);
    dy = clampCell(dy, -1, 1);
    return {
        body.y + 3 + dy,
        body.x + 7 + kHeadShiftCells + dx,
        body.x + 12 + kHeadShiftCells + dx,
        body.x + 10 + kHeadShiftCells + dx,
        body.y + 5 + dy,
    };
}

static void drawEye(DrawContext& ctx, int x, int y,
                    EyePose eyes) {
    // Every state owns the same 2x2-cell box, so alertness can never change
    // the eye's outer dimensions or push facial ink off its coat underlay.
    // What varies is how much of that box the iris claims and which row the
    // lid line occupies. Holding the box fixed but varying the fill is what
    // lets SOFT, OPEN, and WIDE read as three different moods: previously all
    // three drew identical pixels, so every authored alertness beat was inert.
    fillCells(ctx, x, y, kEyeWidth, kEyeHeight, ctx.pal.eyeBase,
              RasterRole::EYE_BASE);
    switch (eyes) {
        case EyePose::CLOSED:
            // Resting lid: the dark line sits low, following the cheek.
            fillCells(ctx, x, y + 1, kEyeWidth, 1, ctx.pal.ink,
                      RasterRole::INK);
            return;
        case EyePose::SQUEEZE:
            // Shut tight and happy. Raising the same line to the upper row
            // inverts the arc, which is what sells a meow or a hard blink
            // against CLOSED's neutral sleep.
            fillCells(ctx, x, y, kEyeWidth, 1, ctx.pal.ink,
                      RasterRole::INK);
            return;
        case EyePose::SOFT:
            // Half-lidded: the pupil survives, the iris does not.
            fillCell(ctx, x + 1, y + 1, ctx.pal.ink, RasterRole::INK);
            return;
        case EyePose::WIDE:
            // Startled: the iris floods the whole box behind the pupil.
            fillCells(ctx, x, y, kEyeWidth, 1, ctx.pal.eyeIris,
                      RasterRole::IRIS);
            fillCell(ctx, x, y + 1, ctx.pal.eyeIris, RasterRole::IRIS);
            // A one-fine-cell pupil cannot live inside a 4px output cell. If
            // preserved there it becomes a detached black square beside the
            // iris whenever the eye straddles two coarse cells.
            if (!ctx.fineRaster)
                fillCell(ctx, x + 1, y + 1, ctx.pal.ink,
                         RasterRole::INK);
            return;
        case EyePose::OPEN:
        default:
            fillCell(ctx, x, y + 1, ctx.pal.eyeIris, RasterRole::IRIS);
            if (!ctx.fineRaster)
                fillCell(ctx, x + 1, y + 1, ctx.pal.ink,
                         RasterRole::INK);
            return;
    }
}

static void drawMouth(DrawContext& ctx, const FaceGeometry& face,
                      MouthPose mouth) {
    const int x = face.noseX;
    const int y = face.noseY + 1;
    switch (mouth) {
        case MouthPose::MEOW:
            // The authored oval: a dark rim carries the shape and a rose
            // interior gives it depth, so the open muzzle still reads as a
            // mouth rather than a hole punched in the coat.
            fillCells(ctx, x - 1, y, 3, 1, ctx.pal.mouthRim,
                      RasterRole::INK);
            fillCell(ctx, x - kMeowMouthHalfWidth, y + 1,
                     ctx.pal.mouthRim, RasterRole::INK);
            fillCells(ctx, x - 1, y + 1, 3, 1, ctx.pal.mouthInner,
                      RasterRole::ACCENT);
            fillCell(ctx, x + kMeowMouthHalfWidth, y + 1,
                     ctx.pal.mouthRim, RasterRole::INK);
            fillCells(ctx, x - 1, y + 2, 3, 1, ctx.pal.mouthRim,
                      RasterRole::INK);
            return;
        case MouthPose::CHIRP:
            fillCells(ctx, x - kChirpMouthHalfWidth, y,
                      kChirpMouthHalfWidth * 2 + 1, 1, ctx.pal.mouthRim,
                      RasterRole::INK);
            fillCell(ctx, x, y + 1, ctx.pal.mouthInner,
                     RasterRole::ACCENT);
            return;
        case MouthPose::CLOSED:
        default:
            fillCell(ctx, x, y, ctx.pal.shadow);
            return;
    }
}

static void drawCatFace(DrawContext& ctx, const Pose& pose,
                        const BodyGeometry& body) {
    const FaceGeometry face = faceGeometry(pose, body);

    drawEye(ctx, face.backEyeX, face.eyeY, pose.eyes);
    drawEye(ctx, face.frontEyeX, face.eyeY, pose.eyes);

    // The nose stays a single centered cell in every state, so the muzzle
    // keeps its frontal axis and no opening can become a projecting side
    // snout. The mouth grows downward from it into the chest, which is where
    // the coat is unbroken and an oval has room to read.
    fillCell(ctx, face.noseX, face.noseY, ctx.pal.pink,
             RasterRole::ACCENT);
    drawMouth(ctx, face, pose.mouth);
}

static void compose(DrawContext& ctx, const Pose& pose,
                    const SecondaryMotion& secondary) {
    const BodyGeometry body = bodyGeometry(pose);

    // Both depth pairs attach behind the belly. Every filled 4/4/2 bump stays
    // broad even where a belly chamfer uncovers its attachment row.
    drawTail(ctx, pose, body, secondary);
    drawPawLayer(ctx, pose, body, true);
    drawPawLayer(ctx, pose, body, false);
    drawMiniPancettaBody(ctx, pose, body);
    drawRaisedForepaw(ctx, pose, body, true);
    drawRaisedForepaw(ctx, pose, body, false);
    drawCatFace(ctx, pose, body);
}

static uint8_t outputCoatTieRank(uint16_t color, const Palette& pal) {
    // Coverage still decides between coat planes. This rank is consulted only
    // when two colors occupy the same number of fine cells, where preferring
    // the cleaner upper tone avoids a checkerboard and keeps a mirrored pose
    // from changing shade merely because screen-space scan order reversed.
    if (color == pal.white) return 4u;
    if (color == pal.light) return 3u;
    if (color == pal.shade) return 2u;
    if (color == pal.shadow) return 1u;
    return 0u;
}

static bool selectOutputCellColor(const FineRaster& raster,
                                  int outputCol, int outputRow,
                                  const Palette& pal,
                                  uint16_t& selected,
                                  CoatPlane* selectedPlane = nullptr,
                                  bool* selectedDirectional = nullptr) {
    uint16_t colors[4] = {};
    uint8_t featurePriorities[4] = {};
    uint8_t count = 0u;
    uint8_t softCount = 0u;
    uint8_t shadeCount = 0u;
#ifdef NATIVE_TEST
    uint8_t softDirectionalCount = 0u;
    uint8_t shadeDirectionalCount = 0u;
#else
    (void)selectedDirectional;
#endif
    for (int dy = 0; dy < 2; ++dy) {
        for (int dx = 0; dx < 2; ++dx) {
            const int row = outputRow * 2 + dy;
            const int col = outputCol * 2 + dx;
            if (!raster.occupied(row, col)) continue;
            colors[count] = raster.color[row][col];
            const RasterRole role = raster.role(row, col);
            featurePriorities[count] = featurePriorityForRole(role);
            ++count;
            const CoatPlane plane = coatPlaneForRole(role);
            if (plane == CoatPlane::SOFT) {
                ++softCount;
#ifdef NATIVE_TEST
                if (raster.directional(row, col))
                    ++softDirectionalCount;
#endif
            } else if (plane == CoatPlane::SHADE) {
                ++shadeCount;
#ifdef NATIVE_TEST
                if (raster.directional(row, col))
                    ++shadeDirectionalCount;
#endif
            }
        }
    }
    if (count == 0u) return false;

    uint16_t best = 0u;
    uint8_t bestPriority = 0u;
    uint8_t bestCount = 0u;
    uint8_t bestCoatRank = 0u;
    bool haveBest = false;
    for (uint8_t i = 0; i < count; ++i) {
        uint8_t same = 0u;
        for (uint8_t j = 0; j < count; ++j)
            if (colors[j] == colors[i]) ++same;
        const uint8_t priority = featurePriorities[i];
        const uint8_t coatRank = outputCoatTieRank(colors[i], pal);
        if (!haveBest || priority > bestPriority ||
            (priority == bestPriority &&
             (same > bestCount ||
              (same == bestCount &&
               (coatRank > bestCoatRank ||
                (coatRank == bestCoatRank && colors[i] < best)))))) {
            best = colors[i];
            bestPriority = priority;
            bestCount = same;
            bestCoatRank = coatRank;
            haveBest = true;
        }
    }

    // Color-majority alone erased every half-covered one-row fur plane when
    // the 2px construction cells resolved to 4px. Preserve a coat plane when
    // it owns at least half of a full output cell, or a strict majority of a
    // partially occupied silhouette cell. Feature channels keep priority, so
    // this cannot paint over an eye, ear interior, nose, or open mouth.
    CoatPlane resolvedPlane = CoatPlane::NONE;
#ifdef NATIVE_TEST
    bool resolvedDirectional = false;
#endif
    const uint8_t coatCount = (uint8_t)(softCount + shadeCount);
    const bool coherentPlane = coatCount >= 2u ||
        (coatCount > 0u && coatCount * 2u > count);
    if (bestPriority == 0u && coherentPlane) {
        if (shadeCount > softCount) {
            selected = pal.shade;
            resolvedPlane = CoatPlane::SHADE;
#ifdef NATIVE_TEST
            resolvedDirectional = shadeDirectionalCount != 0u;
#endif
        } else {
            selected = pal.light;
            resolvedPlane = CoatPlane::SOFT;
#ifdef NATIVE_TEST
            resolvedDirectional = softDirectionalCount != 0u;
#endif
        }
    }
    if (selectedPlane) *selectedPlane = resolvedPlane;
#ifdef NATIVE_TEST
    if (selectedDirectional) *selectedDirectional = resolvedDirectional;
#endif
    if (resolvedPlane == CoatPlane::NONE) selected = best;
    return true;
}

static void includeOutputCell(Bounds* bounds, int x, int y) {
    if (!bounds) return;
    if (!bounds->valid) {
        bounds->left = (int16_t)x;
        bounds->top = (int16_t)y;
        bounds->right = (int16_t)(x + kOutputCellPixels);
        bounds->bottom = (int16_t)(y + kOutputCellPixels);
        bounds->valid = true;
        return;
    }
    bounds->left = (int16_t)min((int)bounds->left, x);
    bounds->top = (int16_t)min((int)bounds->top, y);
    bounds->right = (int16_t)max((int)bounds->right,
                                 x + kOutputCellPixels);
    bounds->bottom = (int16_t)max((int)bounds->bottom,
                                  y + kOutputCellPixels);
}

static void emitOutputRaster(DrawContext& output, const FineRaster& raster) {
    const int footprintX = output.originX + kFootprintInsetX;
    const int footprintY = output.originY + kFootprintInsetY;
    for (int row = 0; row < kOutputCanvasRows; ++row) {
        for (int col = 0; col < kOutputCanvasCols; ++col) {
            uint16_t color = 0u;
            if (!selectOutputCellColor(raster, col, row,
                                       output.pal, color))
                continue;

            const int x = footprintX + col * kOutputCellPixels;
            const int y = footprintY + row * kOutputCellPixels;
            includeOutputCell(output.bounds, x, y);
            if (!output.canvas) continue;
            if (x >= output.canvas->width() || y >= output.canvas->height() ||
                x + kOutputCellPixels <= 0 ||
                y + kOutputCellPixels <= 0)
                continue;
            output.canvas->fillRect(
                x, y, kOutputCellPixels, kOutputCellPixels, color);
        }
    }
}

static void composeOutput(DrawContext& output, const Pose& pose,
                          const SecondaryMotion& secondary) {
    FineRaster raster;
    DrawContext fine = output;
    fine.canvas = nullptr;
    fine.originX = 0;
    fine.originY = 0;
    fine.bounds = nullptr;
    fine.fineRaster = &raster;
    compose(fine, pose, secondary);
    emitOutputRaster(output, raster);
}

static bool boundsFit(const Bounds& bounds) {
    return bounds.valid &&
           bounds.left >= kFootprintInsetX &&
           bounds.top >= kFootprintInsetY &&
           bounds.right <= kFootprintInsetX + kWidth &&
           bounds.bottom <= kFootprintInsetY + kHeight;
}

// Keep the authored construction anatomy inside the same public footprint.
// The 4px resolver deliberately clips its fine input to that footprint, so
// checking only emitted cells would let an escaped limb disappear silently.
static Bounds measureConstructionBounds(const Pose& pose, bool faceRight,
                                        const SecondaryMotion& secondary) {
    Bounds bounds;
    DrawContext ctx = {
        nullptr,
        0,
        0,
        faceRight,
        1,
        {},
        &bounds,
        nullptr,
    };
    compose(ctx, pose, secondary);
    return bounds;
}

static Palette makeCatPalette(uint16_t themeFG, uint16_t themeBG,
                              uint16_t shaft, uint16_t deep,
                              uint16_t wallNear,
                              const PigLight& light,
                              int centerX, int centerY) {
    const bool foregroundIsBrighter =
        Display::brightness565(themeFG) >= Display::brightness565(themeBG);
    const float towardBright = foregroundIsBrighter ? 1.0f : -1.0f;
    const float catBaseTone = Display::kPigBodyTone +
        towardBright * kCatToneStep * kCatToneLiftSteps;
    const auto coatTone = [themeBG, themeFG](float tone) {
        return Display::lerpColor565(themeBG, themeFG, tone);
    };
    const float dx = (float)light.x - (float)centerX;
    const float dy = (float)light.y - (float)centerY;
    const float distance = sqrtf(dx * dx + dy * dy);
    float reach = 1.0f - distance / 220.0f;
    if (reach < 0.12f) reach = 0.12f;
    if (reach > 1.0f) reach = 1.0f;
    const uint8_t key8 = light.tint == 0
        ? 0u
        : (uint8_t)(28.0f + reach * 68.0f);

    // The main coat is exactly two theme-tone steps lighter than Pancetta's
    // base in both normal and inverted themes. Lower planes walk back toward
    // the darker endpoint, with SHAFT supplying only a restrained cool cast.
    Palette pal = {
        coatTone(catBaseTone),
        Display::lerpColor565(
            coatTone(catBaseTone - towardBright * kCatToneStep),
            shaft, 0.08f),
        Display::lerpColor565(
            coatTone(catBaseTone - towardBright * kCatToneStep *
                      (float)kCatShadeSteps),
            shaft, 0.10f),
        Display::lerpColor565(
            coatTone(catBaseTone - towardBright * kCatToneStep *
                      (float)kCatShadowSteps),
            deep, kCatShadowDeepMix),
        Display::lerpColor565(deep, wallNear, 0.12f),
        coatTone(Display::kPigBodyTone),
        Display::hsvToRgb565(48, 220, 248),
        Display::hsvToRgb565(348, 130, 255),
        // An open muzzle is an interior surface, so it keeps its own fixed
        // rose pair instead of following the coat tone.
        Display::hsvToRgb565(344, 205, 112),
        Display::hsvToRgb565(342, 150, 205),
    };
    if (key8 == 0u) return pal;

    pal.white = screenBlend565(pal.white, light.tint, key8);
    pal.light = screenBlend565(pal.light, light.tint,
                               (uint8_t)min(255, key8 + 18));
    pal.shade = screenBlend565(pal.shade, light.tint,
                               (uint8_t)(key8 / 2u));
    pal.shadow = screenBlend565(pal.shadow, light.tint,
                                (uint8_t)(key8 / 4u));
    pal.eyeBase = screenBlend565(pal.eyeBase, light.tint,
                                 (uint8_t)(key8 / 5u));

    // Additive light barely changes near-white 565 values. Fold a small
    // amount of the practical's chroma back into the lifted fur so neon, CRT,
    // window, and bath sources remain visible without repainting a white cat.
    const float chroma = 0.06f + reach * 0.10f;
    pal.white = Display::lerpColor565(pal.white, light.tint, chroma);
    pal.light = Display::lerpColor565(pal.light, light.tint,
                                      chroma + 0.04f);
    pal.shade = Display::lerpColor565(pal.shade, light.tint,
                                      chroma * 0.62f);
    pal.shadow = Display::lerpColor565(pal.shadow, light.tint,
                                       chroma * 0.38f);
    return pal;
}

}  // namespace

void draw(M5Canvas& canvas, int rawX, int rawY, bool faceRight,
          const Pose& pose, const SecondaryMotion& secondary,
          const PigLight& light) {
    const uint16_t themeFG = Display::getColorFG();
    const uint16_t themeBG = Display::getColorBG();
    const int centerX = rawX + kFootprintInsetX + kWidth / 2;
    const int centerY = rawY + kFootprintInsetY + kHeight / 2;
    const float dx = (float)light.x - (float)centerX;
    DrawContext ctx = {
        &canvas,
        rawX & ~(kCellPixels - 1),
        rawY & ~(kCellPixels - 1),
        faceRight,
        localKeySide(faceRight, light.tint != 0, dx),
        makeCatPalette(themeFG, themeBG, RP::SHAFT, RP::DEEP,
                       RP::WALL_NEAR, light, centerX, centerY),
        nullptr,
        nullptr,
    };
    composeOutput(ctx, pose, secondary);
}

Point noseAnchor(int rawX, int rawY, bool faceRight, const Pose& pose) {
    const BodyGeometry body = bodyGeometry(pose);
    const FaceGeometry face = faceGeometry(pose, body);
    const int localX = faceRight
        ? face.noseX
        : (int)kWhiteCat.canvasCols - face.noseX - 1;
    Point point;
    point.x = (int16_t)((rawX & ~(kCellPixels - 1)) +
                        localX * kCellPixels);
    point.y = (int16_t)((rawY & ~(kCellPixels - 1)) +
                        face.noseY * kCellPixels);
    const int footprintX = (rawX & ~(kCellPixels - 1)) +
                           kFootprintInsetX;
    const int footprintY = (rawY & ~(kCellPixels - 1)) +
                           kFootprintInsetY;
    const int outputCol = clampCell(
        (point.x - footprintX) / kOutputCellPixels,
        0, kOutputCanvasCols - 1);
    const int outputRow = clampCell(
        (point.y - footprintY) / kOutputCellPixels,
        0, kOutputCanvasRows - 1);
    point.x = (int16_t)(footprintX + outputCol * kOutputCellPixels +
                        kOutputCellPixels / 2);
    point.y = (int16_t)(footprintY + outputRow * kOutputCellPixels +
                        kOutputCellPixels / 2);
    return point;
}

Bounds measurePoseBounds(const Pose& pose, bool faceRight,
                         const SecondaryMotion& secondary) {
    Bounds bounds;
    DrawContext ctx = {
        nullptr,
        0,
        0,
        faceRight,
        1,
        {},
        &bounds,
        nullptr,
    };
    composeOutput(ctx, pose, secondary);
    return bounds;
}

bool poseFitsEnvelope(const Pose& pose) {
    if ((uint8_t)pose.stance > (uint8_t)Stance::SCRATCH ||
        (uint8_t)pose.gait > (uint8_t)Gait::BOUND_TUCK ||
        (uint8_t)pose.head > (uint8_t)HeadPose::GAG ||
        (uint8_t)pose.tail > (uint8_t)TailPose::PUFF ||
        (uint8_t)pose.ears > (uint8_t)EarPose::ALERT ||
        (uint8_t)pose.eyes > (uint8_t)EyePose::SQUEEZE ||
        (uint8_t)pose.mouth > (uint8_t)MouthPose::MEOW ||
        (uint8_t)pose.fur > (uint8_t)FurPose::COMPRESS)
        return false;
    if (pose.bodyDx < -4 || pose.bodyDx > 4 ||
        pose.bodyDy < -5 || pose.bodyDy > 5 ||
        pose.headDx < -6 || pose.headDx > 6 ||
        pose.headDy < -6 || pose.headDy > 6 ||
        pose.breath < -1 || pose.breath > 1)
        return false;

    for (int sweep = -2; sweep <= 2; sweep += 4) {
        for (int lift = -1; lift <= 1; lift += 2) {
            SecondaryMotion secondary;
            secondary.sweepCells = (int8_t)sweep;
            secondary.liftCells = (int8_t)lift;
            const Bounds rightConstruction =
                measureConstructionBounds(pose, true, secondary);
            const Bounds leftConstruction =
                measureConstructionBounds(pose, false, secondary);
            const Bounds rightOutput =
                measurePoseBounds(pose, true, secondary);
            const Bounds leftOutput =
                measurePoseBounds(pose, false, secondary);
            if (!boundsFit(rightConstruction) ||
                !boundsFit(leftConstruction) ||
                !boundsFit(rightOutput) || !boundsFit(leftOutput)) {
#ifdef HAMLET_SIM
                fprintf(stderr,
                        "[CAT BOUNDS] stance=%u gait=%u head=%u "
                        "body=(%d,%d) face=(%d,%d) "
                        "fineR=%d,%d..%d,%d fineL=%d,%d..%d,%d "
                        "outR=%d,%d..%d,%d outL=%d,%d..%d,%d\n",
                        (unsigned)pose.stance, (unsigned)pose.gait,
                        (unsigned)pose.head,
                        (int)pose.bodyDx, (int)pose.bodyDy,
                        (int)pose.headDx, (int)pose.headDy,
                        (int)rightConstruction.left,
                        (int)rightConstruction.top,
                        (int)rightConstruction.right,
                        (int)rightConstruction.bottom,
                        (int)leftConstruction.left,
                        (int)leftConstruction.top,
                        (int)leftConstruction.right,
                        (int)leftConstruction.bottom,
                        (int)rightOutput.left, (int)rightOutput.top,
                        (int)rightOutput.right, (int)rightOutput.bottom,
                        (int)leftOutput.left, (int)leftOutput.top,
                        (int)leftOutput.right, (int)leftOutput.bottom);
#endif
                return false;
            }
        }
    }
    return true;
}

#ifdef NATIVE_TEST
SurfaceAudit auditSurfaceResponse(const Pose& pose) {
    struct OutputMap {
        uint8_t count = 0u;
        uint16_t columnSum = 0u;
        bool avoidsStraightBands = true;
    };
    // Deliberately collide SOFT with EYE_BASE. Monochrome and low-chroma theme
    // axes can quantize these channels to the same RGB565 value; the resolver
    // must still apply coat coverage instead of promoting fur as facial detail.
    const Palette probePal = {
        1u, 2u, 3u, 4u, 5u, 2u, 7u, 8u, 9u, 10u,
    };
    const auto sample = [&pose, &probePal](int8_t keyLocalSide) {
        FineRaster raster;
        DrawContext fine = {
            nullptr, 0, 0, true, keyLocalSide, probePal,
            nullptr, &raster, nullptr,
        };
        compose(fine, pose, SecondaryMotion{});

        OutputMap output;
        for (int row = 0; row < kOutputCanvasRows; ++row) {
            uint8_t run = 0u;
            for (int col = 0; col < kOutputCanvasCols; ++col) {
                uint16_t color = 0u;
                CoatPlane plane = CoatPlane::NONE;
                bool directional = false;
                if (!selectOutputCellColor(raster, col, row, probePal,
                                           color, &plane, &directional) ||
                    plane == CoatPlane::NONE || !directional) {
                    run = 0u;
                    continue;
                }
                ++output.count;
                output.columnSum = (uint16_t)(output.columnSum + col);
                ++run;
                if (run > 2u) output.avoidsStraightBands = false;
            }
        }
        return output;
    };

    const OutputMap keyRight = sample(1);
    const OutputMap centered = sample(0);
    const OutputMap keyLeft = sample(-1);
    SurfaceAudit audit;
    audit.keyRightDirectionalCells = keyRight.count;
    audit.keyLeftDirectionalCells = keyLeft.count;
    audit.directionalPlanesVisible = keyRight.count >= 3u &&
                                     keyLeft.count >= 3u;
    const uint8_t smallerCount = min(keyRight.count, keyLeft.count);
    const uint8_t largerCount = max(keyRight.count, keyLeft.count);
    // Facial channels legitimately replace front-side coat cells. Keep the
    // surviving far-side response in the same order of magnitude rather than
    // demanding false bilateral equality from an asymmetric cat.
    audit.directionalPlanesBalanced = smallerCount * 3u >= largerCount;
    audit.centeredKeyDropsLateralPlanes = centered.count == 0u;
    audit.directionTracksKey = keyRight.count != 0u && keyLeft.count != 0u &&
        (uint32_t)keyRight.columnSum * keyLeft.count <
        (uint32_t)keyLeft.columnSum * keyRight.count;
    audit.avoidsStraightBands = keyRight.avoidsStraightBands &&
                                keyLeft.avoidsStraightBands;
    return audit;
}

LightingAudit auditLightingResponse() {
    const uint16_t fg = 0xffffu;
    const uint16_t bg = 0x0000u;
    const uint16_t shaft = 0x8410u;
    const uint16_t deep = 0x0000u;
    const uint16_t wallNear = 0x3186u;
    const int centerX = 100;
    const int centerY = 100;
    PigLight offA;
    offA.x = -300;
    offA.y = 400;
    PigLight offB;
    offB.x = 100;
    offB.y = 100;
    PigLight nearKey;
    nearKey.x = 112;
    nearKey.y = 84;
    nearKey.tint = 0xf800u;
    PigLight farKey;
    farKey.x = 900;
    farKey.y = 100;
    farKey.tint = nearKey.tint;
    const Palette unlitA = makeCatPalette(
        fg, bg, shaft, deep, wallNear, offA, centerX, centerY);
    const Palette unlitB = makeCatPalette(
        fg, bg, shaft, deep, wallNear, offB, centerX, centerY);
    const Palette near = makeCatPalette(
        fg, bg, shaft, deep, wallNear, nearKey, centerX, centerY);
    const Palette far = makeCatPalette(
        fg, bg, shaft, deep, wallNear, farKey, centerX, centerY);
    const auto colorDistance = [](uint16_t a, uint16_t b) {
        const int ar = (a >> 11) & 0x1f;
        const int ag = (a >> 5) & 0x3f;
        const int ab = a & 0x1f;
        const int br = (b >> 11) & 0x1f;
        const int bgc = (b >> 5) & 0x3f;
        const int bb = b & 0x1f;
        return abs(ar - br) + abs(ag - bgc) + abs(ab - bb);
    };

    LightingAudit audit;
    audit.unlitIgnoresSourcePosition =
        unlitA.white == unlitB.white &&
        unlitA.light == unlitB.light &&
        unlitA.shade == unlitB.shade &&
        unlitA.shadow == unlitB.shadow;
    audit.emitterTintReachesCoat = near.white != unlitA.white &&
                                   near.light != unlitA.light;
    audit.distanceFalloffWorks =
        colorDistance(near.white, unlitA.white) >
        colorDistance(far.white, unlitA.white);
    audit.depthOrderSurvives =
        Display::brightness565(unlitA.white) >
            Display::brightness565(unlitA.light) &&
        Display::brightness565(unlitA.light) >
            Display::brightness565(unlitA.shade) &&
        Display::brightness565(unlitA.shade) >
            Display::brightness565(unlitA.shadow);
    audit.tintFallsAcrossDepth =
        colorDistance(near.white, unlitA.white) >
        colorDistance(near.shadow, unlitA.shadow);
    return audit;
}

AnatomyAudit auditPoseAnatomy(const Pose& pose) {
    const BodyGeometry body = bodyGeometry(pose);
    const PawGeometry paws[] = {
        pawGeometry(pose, body, true, false),
        pawGeometry(pose, body, false, false),
        pawGeometry(pose, body, true, true),
        pawGeometry(pose, body, false, true),
    };

    AnatomyAudit audit;
    audit.pawCount = (uint8_t)(sizeof(paws) / sizeof(paws[0]));
    audit.pawTipsDistinct = true;
    for (uint8_t i = 0; i < audit.pawCount; ++i) {
        if (paws[i].bottomY < kWhiteCat.groundRow)
            ++audit.elevatedPawCount;
        for (uint8_t j = (uint8_t)(i + 1u); j < audit.pawCount; ++j) {
            if (paws[i].bottomY == paws[j].bottomY &&
                abs(paws[i].centerX - paws[j].centerX) < 3)
                audit.pawTipsDistinct = false;
        }
    }

    const bool hasRaisedForepaw =
        pose.gait == Gait::PAW_LIFT ||
        pose.gait == Gait::SCRATCH_A ||
        pose.gait == Gait::SCRATCH_B;
    audit.raisedForepawIsJoinedBump = !hasRaisedForepaw;
    if (hasRaisedForepaw) {
        const bool far = pose.gait == Gait::SCRATCH_B;
        const PawGeometry raised = pawGeometry(pose, body, far, true);
        const int attachmentRow = raised.bottomY - 2 - body.y;
        if (attachmentRow >= 1 &&
            attachmentRow < PancettaBodyMask::kRows) {
            const int bodyRight = body.x +
                catBodyRightForRow(attachmentRow);
            // The broad lifted bump overlaps the chest instead of hanging from
            // it by a connector cell.
            audit.raisedForepawIsJoinedBump =
                raised.centerX - 1 <= bodyRight;
        }
    }
    const FaceGeometry face = faceGeometry(pose, body);
    const auto coatContains = [body](int x, int y) {
        const int row = y - body.y;
        if (row < 1 || row >= PancettaBodyMask::kRows) return false;
        int left = catBodyLeftForRow(row);
        int right = catBodyRightForRow(row);
        if (row == 2) {
            const int headLeft = kWhiteCat.headX - kWhiteCat.torsoX;
            left = min(left, headLeft);
            right = max(right, headLeft + (int)kWhiteCat.headW - 1);
        }
        if (row == 6)
            right = max(right, catBodyRightForRow(5));
        return x >= body.x + left && x <= body.x + right;
    };
    audit.faceHasCoatUnderlay = true;
    const int eyeXs[] = {face.backEyeX, face.frontEyeX};
    for (int eyeX : eyeXs) {
        for (int y = face.eyeY; y < face.eyeY + kEyeHeight; ++y) {
            for (int x = eyeX; x < eyeX + kEyeWidth; ++x)
                audit.faceHasCoatUnderlay =
                    audit.faceHasCoatUnderlay && coatContains(x, y);
        }
    }
    audit.faceHasCoatUnderlay = audit.faceHasCoatUnderlay &&
        coatContains(face.noseX, face.noseY);
    for (int x = face.noseX; x < face.noseX + kMouthWidth; ++x)
        audit.faceHasCoatUnderlay = audit.faceHasCoatUnderlay &&
            coatContains(x, face.noseY + 1);

    // The widest authored opening must still land entirely on coat. A meow
    // that overhung the chest would paint rose cells straight onto the room
    // behind the cat, and the outer bounds check cannot see that.
    audit.meowMouthHasCoatUnderlay = true;
    for (int row = 0; row < kMeowMouthRows; ++row) {
        const int half = row == 1 ? kMeowMouthHalfWidth : 1;
        for (int x = face.noseX - half; x <= face.noseX + half; ++x)
            audit.meowMouthHasCoatUnderlay =
                audit.meowMouthHasCoatUnderlay &&
                coatContains(x, face.noseY + 1 + row);
    }

    // SOFT, OPEN, and WIDE once rendered byte-identical pixels, which silently
    // discarded every alertness beat the clips author. Sample the eye through
    // the real composition path and require all five states to differ.
    static constexpr EyePose kEyeStates[] = {
        EyePose::OPEN, EyePose::SOFT, EyePose::CLOSED,
        EyePose::WIDE, EyePose::SQUEEZE,
    };
    static constexpr int kEyeStateCount =
        (int)(sizeof(kEyeStates) / sizeof(kEyeStates[0]));
    static constexpr int kEyeCells = kEyeWidth * kEyeHeight;
    Palette probePal = {};
    probePal.eyeBase = 1u;
    probePal.eyeIris = 2u;
    probePal.ink = 3u;
    uint16_t signature[kEyeStateCount][kEyeCells] = {};
    for (int state = 0; state < kEyeStateCount; ++state) {
        for (int cell = 0; cell < kEyeCells; ++cell) {
            CellProbe probe;
            probe.x = cell % kEyeWidth;
            probe.y = cell / kEyeWidth;
            DrawContext probeCtx = {
                nullptr, 0, 0, true, 1, probePal, nullptr, nullptr, &probe,
            };
            drawEye(probeCtx, 0, 0, kEyeStates[state]);
            signature[state][cell] = probe.hit ? probe.color : 0u;
        }
    }
    audit.eyePosesAreDistinct = true;
    for (int a = 0; a < kEyeStateCount; ++a) {
        for (int b = a + 1; b < kEyeStateCount; ++b) {
            bool identical = true;
            for (int cell = 0; cell < kEyeCells; ++cell)
                identical = identical &&
                    signature[a][cell] == signature[b][cell];
            if (identical) audit.eyePosesAreDistinct = false;
        }
    }
    const int backEyeCenter2 = face.backEyeX * 2 + kEyeWidth - 1;
    const int frontEyeCenter2 = face.frontEyeX * 2 + kEyeWidth - 1;
    audit.muzzleIsCenteredAndCompact =
        face.noseX * 2 ==
            (backEyeCenter2 + frontEyeCenter2) / 2;
    const RibbonOffset horizontal = tailRibbonOffset(0, 0, 4, 0);
    const RibbonOffset vertical = tailRibbonOffset(0, 0, 0, 4);
    audit.tailRibbonHasCrossAxisThickness =
        horizontal.x == 0 && abs(horizontal.y) == 1 &&
        abs(vertical.x) == 1 && vertical.y == 0;

    uint8_t keyRightMap[PancettaBodyMask::kRows][kCatTorsoCols] = {};
    uint8_t keyLeftMap[PancettaBodyMask::kRows][kCatTorsoCols] = {};
    uint8_t sideRightMap[PancettaBodyMask::kRows][kCatTorsoCols] = {};
    uint8_t sideLeftMap[PancettaBodyMask::kRows][kCatTorsoCols] = {};
    audit.coatPlanesStayInsideBody = true;
    const auto markPatch = [&audit](
            uint8_t (&map)[PancettaBodyMask::kRows][kCatTorsoCols],
            const CoatPatch& patch, int patchX) {
        const uint8_t tone = patch.plane == CoatPlane::SHADE ? 2u : 1u;
        for (int x = patchX; x < patchX + patch.width; ++x) {
            if (patch.y < 1 || patch.y >= PancettaBodyMask::kRows ||
                x < catBodyLeftForRow(patch.y) ||
                x > catBodyRightForRow(patch.y)) {
                audit.coatPlanesStayInsideBody = false;
                continue;
            }
            map[patch.y][x] = tone;
        }
    };
    for (const CoatPatch& patch : kSideCoatPatches) {
        const int keyRightX = coatPatchX(patch, 1);
        const int keyLeftX = coatPatchX(patch, -1);
        markPatch(keyRightMap, patch, keyRightX);
        markPatch(keyLeftMap, patch, keyLeftX);
        markPatch(sideRightMap, patch, keyRightX);
        markPatch(sideLeftMap, patch, keyLeftX);
    }
    for (const CoatPatch& patch : kUndersideCoatPatches) {
        markPatch(keyRightMap, patch, patch.x);
        markPatch(keyLeftMap, patch, patch.x);
    }

    audit.coatPlanesMirrorWithKey = true;
    for (int y = 1; y < PancettaBodyMask::kRows; ++y) {
        for (int x = 0; x < kCatTorsoCols; ++x) {
            // The canonical even output footprint owns the mirror phase. Its
            // axis sits one construction cell left of the odd torso's local
            // axis; coatPatchX applies the same correction before resolve.
            const int mirrorX = kCatTorsoCols - 2 - x;
            const uint8_t mirrored = mirrorX >= 0 && mirrorX < kCatTorsoCols
                ? sideLeftMap[y][mirrorX] : 0u;
            if (sideRightMap[y][x] != mirrored) {
                audit.coatPlanesMirrorWithKey = false;
            }
        }
    }

    audit.coatPlanesAvoidStraightBands = true;
    const auto auditRuns = [&audit](
            const uint8_t (&map)[PancettaBodyMask::kRows]
                                [kCatTorsoCols]) {
        for (int y = 1; y < PancettaBodyMask::kRows; ++y) {
            uint8_t previous = 0;
            uint8_t run = 0;
            for (int x = 0; x < kCatTorsoCols; ++x) {
                const uint8_t tone = map[y][x];
                if (tone != 0u && tone == previous) {
                    ++run;
                } else {
                    run = tone == 0u ? 0u : 1u;
                }
                previous = tone;
                if (run > 3u) audit.coatPlanesAvoidStraightBands = false;
            }
        }
    };
    auditRuns(keyRightMap);
    auditRuns(keyLeftMap);
    audit.coatDirectionTracksKey =
        localKeySide(true, true, 12.0f) == 1 &&
        localKeySide(true, true, -12.0f) == -1 &&
        localKeySide(false, true, 12.0f) == -1 &&
        localKeySide(false, true, -12.0f) == 1 &&
        localKeySide(true, true, 0.0f) == 0 &&
        localKeySide(false, true, 0.0f) == 0 &&
        localKeySide(true, false, 12.0f) == 0;
    return audit;
}
#endif

}  // namespace Rig
}  // namespace PancettaCat
