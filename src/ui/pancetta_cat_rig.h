#pragma once

#include <M5Unified.h>
#include <stdint.h>

#include "pancetta_cat.h"

namespace PancettaCat {
namespace Rig {

// The constructor is expressed in 2px cells. Detective Pancetta's compiled
// 18x10 mask supplies the row profile; the cat inserts one torso cell and
// owns feline ears, face, tail, and four-paw placement in the action envelope.
struct Definition {
    uint8_t cellPixels;
    uint8_t canvasCols;
    uint8_t canvasRows;
    int8_t groundRow;
    int8_t torsoX;
    int8_t torsoY;
    uint8_t torsoW;
    uint8_t torsoH;
    int8_t headX;
    int8_t headY;
    uint8_t headW;
    uint8_t headH;
    int8_t shoulderX;
    int8_t hipX;

    constexpr Definition(uint8_t cell, uint8_t cols, uint8_t rows,
                         int8_t ground,
                         int8_t bodyX, int8_t bodyY,
                         uint8_t bodyW, uint8_t bodyH,
                         int8_t skullX, int8_t skullY,
                         uint8_t skullW, uint8_t skullH,
                         int8_t shoulder, int8_t hip)
        : cellPixels(cell), canvasCols(cols), canvasRows(rows),
          groundRow(ground), torsoX(bodyX), torsoY(bodyY),
          torsoW(bodyW), torsoH(bodyH), headX(skullX), headY(skullY),
          headW(skullW), headH(skullH), shoulderX(shoulder), hipX(hip) {}
};

// The stretched half-scale body is centered low in the original action box.
// Four copies of the established fat oval stubby overlap its bottom row;
// there are no exposed one-cell leg shafts in grounded or airborne poses.
static constexpr Definition kWhiteCat(
    kCellPixels, kActionWidth / kCellPixels,
    kActionHeight / kCellPixels,
    kGroundRow,      // grounded paw row; no transparent bottom pad
    17, 16, 19, 10, // Detective row profile plus one feline torso cell
    21, 16, 12, 8,  // original 12x8 skull translated as one unit
    30, 21);         // translated shoulder / stable hip references
static constexpr int kStoppedBodyDropRows = 0;
static constexpr int kVisiblePawCount = 4;
static constexpr int kPawRows = 3;
static_assert(kVisiblePawCount == 4,
              "ordinary cat poses must retain all four paws");
static_assert(kWhiteCat.canvasCols == 52 && kWhiteCat.canvasRows == 28 &&
                  kWhiteCat.groundRow + 1 == kWhiteCat.canvasRows,
              "cat envelope must end on the physical paw-contact row");
static_assert(kWhiteCat.groundRow - kPawRows + 1 ==
                   kWhiteCat.torsoY + kWhiteCat.torsoH - 1,
              "fat stubbies must overlap the body's final row");
static_assert(kWhiteCat.torsoY + kWhiteCat.torsoH + kPawRows - 1 ==
                  kWhiteCat.groundRow + 1,
              "three-row paws must reach support without a thin leg shaft");
static_assert(kWhiteCat.torsoX >= 0 &&
                  kWhiteCat.torsoX + kWhiteCat.torsoW <=
                      kWhiteCat.canvasCols &&
                  kWhiteCat.headX >= 0 &&
                  kWhiteCat.headX + kWhiteCat.headW <=
                      kWhiteCat.canvasCols,
              "neutral cat anatomy must fit the authored envelope");

enum class Stance : uint8_t {
    STAND,
    CROUCH,
    SIT,
    LOAF,
    CURL,
    STRETCH,
    ARCH,
    SCRATCH,
};

enum class Gait : uint8_t {
    PLANT,        // stopped: all four fat stubbies remain readable on support
    STRIDE_PLANT, // moving: all four stubbies remain loaded between beats
    STEP_A,
    STEP_B,
    PAW_LIFT,
    SCRATCH_A,
    SCRATCH_B,
    BOUND_EXTEND,
    BOUND_TUCK,
};

enum class HeadPose : uint8_t {
    LEVEL,
    DOWN,
    UP,
    FRONT,
    GROOM_PAW,
    GROOM_FLANK,
    GAG,
};

enum class TailPose : uint8_t {
    TRAIL_LOW,
    LEVEL,
    UP,
    CURL,
    TUCK,
    PUFF,
};

enum class EarPose : uint8_t {
    NEUTRAL,
    FORWARD,
    BACK,
    ALERT,
};

// Every state owns the same 2x2-cell box; they differ in how much of it the
// iris fills and where the lid line sits. SQUEEZE is the happy shut-tight arc
// that belongs to a meow, and reads as the inverse of CLOSED's resting lid.
// Appended last so persisted clip values keep their meaning.
enum class EyePose : uint8_t {
    OPEN,
    SOFT,
    CLOSED,
    WIDE,
    SQUEEZE,
};

// The muzzle is the cat's only vocal channel. CLOSED keeps the established
// one-cell resting mark; CHIRP is a small two-row open for gagging, grooming,
// and phoneme beats; MEOW is the full cartoon oval with a rose interior.
enum class MouthPose : uint8_t {
    CLOSED,
    CHIRP,
    MEOW,
};

// Retained as a clip-language compatibility channel. The miniature renderer
// intentionally draws no fur or hair; navigation inertia affects the tail.
enum class FurPose : uint8_t {
    REST,
    BREATHE,
    DRAG_BACK,
    SWEEP_FORWARD,
    BRISTLE,
    COMPRESS,
};

struct Pose {
    Stance stance;
    Gait gait;
    HeadPose head;
    TailPose tail;
    EarPose ears;
    EyePose eyes;
    FurPose fur;
    int8_t bodyDx;
    int8_t bodyDy;
    int8_t headDx;
    int8_t headDy;
    int8_t breath;
    MouthPose mouth;

    explicit constexpr Pose(Stance body = Stance::STAND,
                            Gait legs = Gait::PLANT,
                            HeadPose skull = HeadPose::LEVEL,
                            TailPose tailShape = TailPose::UP,
                            EarPose earShape = EarPose::NEUTRAL,
                            EyePose eyeShape = EyePose::OPEN,
                            FurPose coat = FurPose::REST,
                            int8_t bodyOffsetX = 0,
                            int8_t bodyOffsetY = 0,
                            int8_t headOffsetX = 0,
                            int8_t headOffsetY = 0,
                            int8_t breathe = 0,
                            MouthPose muzzle = MouthPose::CLOSED)
        : stance(body), gait(legs), head(skull), tail(tailShape),
          ears(earShape), eyes(eyeShape), fur(coat),
          bodyDx(bodyOffsetX), bodyDy(bodyOffsetY),
          headDx(headOffsetX), headDy(headOffsetY), breath(breathe),
          mouth(muzzle) {}
};

struct SecondaryMotion {
    int8_t sweepCells = 0;
    int8_t liftCells = 0;
};

struct Point {
    int16_t x = 0;
    int16_t y = 0;
};

// Pixel bounds use an exclusive right/bottom edge. They are measured from the
// exact same composition path as drawing, so clip validation cannot drift from
// the renderer when a limb, muzzle, tail, or fur tip changes.
struct Bounds {
    int16_t left = 0;
    int16_t top = 0;
    int16_t right = 0;
    int16_t bottom = 0;
    bool valid = false;
};

// The authored 2px construction lattice always resolves into this canonical
// 13x7 grid of four-pixel output cells inside the same physical footprint.
static constexpr int kOutputCellPixels = 4;
static constexpr int kOutputCanvasCols = kWidth / kOutputCellPixels;
static constexpr int kOutputCanvasRows = kHeight / kOutputCellPixels;
static_assert(kOutputCanvasCols == 13 && kOutputCanvasRows == 7,
              "cat output must retain the 52x28 footprint on a 4px grid");

void draw(M5Canvas& canvas, int rawX, int rawY, bool faceRight,
          const Pose& pose, const SecondaryMotion& secondary,
          const PigLight& light);
Point noseAnchor(int rawX, int rawY, bool faceRight, const Pose& pose);
Bounds measurePoseBounds(const Pose& pose, bool faceRight,
                         const SecondaryMotion& secondary);
bool poseFitsEnvelope(const Pose& pose);

#ifdef NATIVE_TEST
// Test-only semantic audit produced by the same geometry helpers as draw().
// This catches readable-anatomy failures that an outer bounds check cannot:
// merged paw tips, a swallowed raised paw, or facial ink without coat below it.
struct AnatomyAudit {
    uint8_t pawCount = 0;
    uint8_t elevatedPawCount = 0;
    bool pawTipsDistinct = false;
    bool raisedForepawIsJoinedBump = false;
    bool faceHasCoatUnderlay = false;
    bool meowMouthHasCoatUnderlay = false;
    bool eyePosesAreDistinct = false;
    bool muzzleIsCenteredAndCompact = false;
    bool tailRibbonHasCrossAxisThickness = false;
    bool coatPlanesStayInsideBody = false;
    bool coatPlanesAvoidStraightBands = false;
    bool coatPlanesMirrorWithKey = false;
    bool coatDirectionTracksKey = false;
};

// The material audit resolves the authored 2px coat map through the same 4px
// semantic voter used by firmware. Construction-space symmetry alone is not
// evidence that a bump plane survived onto the display.
struct SurfaceAudit {
    uint8_t keyRightDirectionalCells = 0;
    uint8_t keyLeftDirectionalCells = 0;
    bool directionalPlanesVisible = false;
    bool directionalPlanesBalanced = false;
    bool centeredKeyDropsLateralPlanes = false;
    bool directionTracksKey = false;
    bool avoidsStraightBands = false;
};

struct LightingAudit {
    bool unlitIgnoresSourcePosition = false;
    bool emitterTintReachesCoat = false;
    bool distanceFalloffWorks = false;
    bool depthOrderSurvives = false;
    bool tintFallsAcrossDepth = false;
};

AnatomyAudit auditPoseAnatomy(const Pose& pose);
SurfaceAudit auditSurfaceResponse(const Pose& pose);
LightingAudit auditLightingResponse();
#endif

}  // namespace Rig
}  // namespace PancettaCat
