#pragma once

#include <M5Unified.h>
#include <stddef.h>
#include <stdint.h>

#include "../piglet/avatar.h"
#include "pancetta_cat_memory.h"

namespace PancettaCat {

// Runtime identity stretches Detective Pancetta's 18x10 row profile by one
// torso cell on an internal 2px construction lattice. Pointed cat ears, the
// established compact face, thick tail, and four fat oval stubbies replace the
// pig details; there is no hair pass or exposed leg shaft. The completed pose
// always resolves to a 4px output grid without changing its measured footprint.
// The rig keeps internal motion clearance, while public placement uses only the
// measured pixel footprint.
static constexpr int kCellPixels = 2;
static constexpr int kActionWidth = 52 * kCellPixels;
static constexpr int kActionHeight = 28 * kCellPixels;
static constexpr int kMinimumSideClearance = 28;
static constexpr int kMinimumTopClearance = 30;
// Public placement keeps one complete construction cell around every animated
// side and above the ears. The 4px resolve may consume that internal guard but
// stays inside the same footprint. The support edge remains unpadded so
// furniture anchors land on physical contact rather than transparent space.
static constexpr int kFootprintGuard = kCellPixels;
static constexpr int kFootprintInsetX =
    kMinimumSideClearance - kFootprintGuard;
static constexpr int kFootprintInsetY =
    kMinimumTopClearance - kFootprintGuard;
static constexpr int kWidth =
    kActionWidth - 2 * kFootprintInsetX;
static constexpr int kHeight = kActionHeight - kFootprintInsetY;
static constexpr int kGroundRow = 27;
static constexpr int kActionGroundContactY =
    (kGroundRow + 1) * kCellPixels;
static constexpr int kGroundContactY =
    kActionGroundContactY - kFootprintInsetY;
static_assert(kGroundContactY == kHeight,
              "cat support contact must be the bottom of its footprint");
static_assert(kMinimumSideClearance >= kFootprintGuard &&
                  kMinimumTopClearance >= kFootprintGuard,
              "cat footprint guard must remain inside the action canvas");
static_assert(kWidth == 52 && kHeight == 28,
              "public cat footprint must include one-cell side/top guards");

static constexpr int rigOriginXForFootprint(int footprintX) {
    return footprintX - kFootprintInsetX;
}

static constexpr int rigOriginYForFootprint(int footprintY) {
    return footprintY - kFootprintInsetY;
}

static constexpr int originYForSupport(int supportY) {
    return supportY - kGroundContactY;
}

struct Pose {
    int16_t x = 0;
    int16_t y = 0;
    bool faceRight = true;
};

enum class Activity : uint8_t {
    FOLLOW,
    WATCH_CABLES,
    WATCH_RAIN,
    SNIFF_FOOD,
    STALK_SKY,
    PROWL_BAR,
    WATCH_WATER,
    SCRATCH,
    SLEEP,
    GROOM,
    HAIRBALL,
    ARCH,
    ZOOMIES,
    FACE_BUMP,
    HEAD_NAP,
    KNEAD,
    SLOW_BLINK,
    MEOW,
    COUNT,
};

// Cats default to their own room business. A visit-stable social plan may make
// the cat approach Pancetta for a bounded stay, and an exit plan may make him
// walk off-screen. Rendering consumes this intent; it never rerolls it.
enum class SceneIntent : uint8_t {
    ROOM_INTEREST,
    COMPANY,
    EXIT,
};

// MenuPig reduces station geometry to the social opportunity the cat needs.
// This keeps the behavior module independent from the room Station enum while
// preventing a head nap from being scheduled where there is no crown to sleep
// on: not when Pancetta is standing, and not when he is resting somewhere with
// a lid over him. SHELTERED is that second case - restful, reachable, closed
// on top.
enum class CompanyContext : uint8_t {
    ORDINARY,
    RESTING,
    SHELTERED,
    BATHING,
};

// ==[ ROOM STAGING ]==
// What the room can tell the companion about itself before he decides how to
// turn up in it. Pushed every frame, read on the frame a room changes: the
// arrival roll is the one place both facts matter at once.
struct RoomStaging {
    // The floor-standing mass he can wait behind, in screen space, and the
    // side he walks out on. The room owns this because only the room knows
    // which of its furniture the player already reads as solid, and where it
    // drifted to under parallax this frame.
    int16_t hideX = 0;
    int16_t hideWidth = 0;
    bool hideEmergeRight = true;
    bool hideValid = false;
    // 0..100 session warmth. A session going well buys an open arrival; a cold
    // one leaves him watching from cover. It moves the weights of the roll and
    // never replaces it - warmth is not a switch.
    uint8_t warmth = 50;
};
void setRoomStaging(const RoomStaging& staging);

void reset(uint32_t now);
void update(uint32_t now, bool channelClear, bool roomScene,
            uint8_t room, bool stationary, CompanyContext companyContext);

// ==[ PORTAL TRANSIT ]==
// Pancetta's portal takes the companion with him. While the particles own his
// silhouette he has no body, no navigation, and no voice, and the room he
// lands in is told to expect him instead of rolling a visit of its own. The
// caller owns the beam; this module owns only what the cat is during it.
void beginPortalTransit(uint32_t now);
void endPortalTransit(uint32_t now, const Pose& landing);
bool inPortalTransit();

// ==[ CONCEALED ARRIVAL ]==
// He was in the room before Pancetta walked into it - behind something. The
// room already drew that something, so rendering must not paint the part of
// him still behind it, and navigation must walk him past its edge before the
// room's own anchor becomes his destination.
// The span the room may paint him into. False once he owns his whole
// silhouette again, which is the frame the room restores its ordinary order.
bool concealClip(int16_t& x, int16_t& width);
// The floor spot just past the occluder edge, which is his only destination
// until he reaches it. False when nothing is emerging.
bool concealExitPose(Pose& pose);
bool isTransmitting();
bool isVisible();
SceneIntent sceneIntent();
bool exitsRight();
bool drawsAbovePig();
bool restsOnPig();
bool consumeMemory(Memory& memory);
// Latched on the frame the sampled mouth opens into a meow, so the caller can
// fire the voice in sync with the visible gape. One pulse per opening.
bool consumeMeowVoice();

// ==[ BATTED TRINKET ]==
// The room owns where the object sits, because only the room knows which
// surface the cat is visiting. This module owns everything it then does:
// rocking under paw beats, tipping over the edge, falling, and rolling to a
// stop. The first valid anchor of a visit is the one that sticks - the object
// belongs to that surface from then on, not to the cat. Offering a new anchor
// or an invalid one afterwards is ignored, so the object neither teleports
// after him as he changes station nor blinks out of the scene while he walks
// away or sits with Pancetta.
static constexpr int kTrinketWidth = 4;
// How far a paw reaches. The room places the object one small gap past the
// cat's footprint, so this only has to cover that gap - it exists to keep a
// scratch on the far side of the room from tipping something over here.
static constexpr int kTrinketReachPx = 12;
void setTrinketAnchor(int16_t x, int16_t supportY, bool valid);
bool trinketVisible();
// The centre of the drawn footprint, so the room can light the object from
// where it actually is. Borrowing the cat's key light made it his accessory
// again: latched under one practical it would take a different one across the
// room, and swap emitters every time he walked past. False when nothing is
// there to light.
bool trinketCenter(int16_t& x, int16_t& y);
// One pulse per paw contact that rocks the object without unseating it.
bool consumeTrinketBat();
// One pulse on the frame the object leaves its surface.
bool consumeTrinketKnock();
// One pulse on the frame it first meets the floor, which is up to 352ms after
// the knock. The impact sound belongs here, not on the knock.
bool consumeTrinketLanded();
void drawTrinket(M5Canvas& canvas, const PigLight& light);

// The behavior FSM may request an action destination before that action starts.
// The cat walks there in FOLLOW, then begins the tagged one-shot clip only
// after reaching it.
Activity desiredActivity(Activity roomDefault, bool pigMoving,
                         bool helperScene);

// Advances cat-owned world geometry toward an absolute scene target. Turning
// Pancetta may change the target, but can never directly relocate the cat.
void updateNavigation(uint32_t now, const Pose& target,
                      Activity roomDefault, bool pigMoving,
                      bool helperScene);
Pose currentPose();
bool isNavigating();
Activity animationActivity();

// Binary protocol is MSB-first ASCII: MEW=0, MRR=1. The destination buffer
// needs 32 bytes for two four-token lines plus the terminator.
void formatMeowByte(uint8_t value, char* out, size_t outSize);

void draw(M5Canvas& canvas, const Pose& pose, bool moving,
          Activity activity, uint32_t now, const PigLight& light);
void drawSignalBubble(M5Canvas& canvas, const Pose& pose, uint32_t now);

#ifdef HAMLET_SIM
// Exact production renderer atlas for simulator review; not linked into either
// device build.
void drawRuntimeSheet(M5Canvas& canvas, uint8_t page);
void forceSignalForCapture(uint32_t now, uint8_t messageIndex);
void forceStateForCapture(uint32_t now, const Pose& pose,
                          Activity activity, bool moving);
bool runStateMachineSelfTest();
#endif

}  // namespace PancettaCat
