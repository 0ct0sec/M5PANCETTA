/**
 * smoke_volume.h — Blade Runner cigarette smoke
 *
 * The reference is the noodle-bar / Tyrell-office smoking beats: a smoker
 * pulls on the cigarette and the ember goes hot while NOTHING leaves the
 * mouth, holds the breath, then exhales a heavy slug of fog that shoots
 * forward, stalls, swells, and dissolves into the room haze. Between drags a
 * thin laminar thread crawls off the resting ember.
 *
 * That is a two-part effect and this module owns both:
 *   - wisp   : the ember thread. Stateless serpentine, but its VISIBILITY is
 *              stateful, so it can never blink out. Stop declaring it and the
 *              thread breaks into drifting volumes instead of vanishing.
 *   - puffs  : the exhale. A small pool of buoyant volumes with real drag and
 *              lifetime. They outlive their emitter by design — that is what
 *              makes a cut (pig submerging, hand lowering, room change is the
 *              one exception) read as smoke dissipating rather than smoke
 *              being switched off.
 *
 * Both passes are light-aware: cells inside an emitter's radius brighten AND
 * thicken, so a plume crossing a light shaft reads as volume catching light
 * instead of a grey decal.
 *
 * PIXEL SIZING — NON-NEGOTIABLE (PIXEL_ART_ARCHITECTURE.md §2.3):
 * every cell this module writes, puff AND wisp, is kRoomPX (4px) on the room
 * lattice. The cigarette is 2px pig detail; its smoke is not. Emitter grid
 * never licenses effect grid, and the cell size is deliberately NOT a caller
 * parameter — the ember thread shipped once at 2px and read as a hairline
 * scratch instead of smoke. Do not reintroduce a cellPx knob.
 */
#pragma once

#include <M5Unified.h>
#include "../piglet/avatar.h"   // PigLight

namespace SmokeFx {

// One slot per concurrent emitter. Pancetta's bath joint and window cigarette
// are never lit at the same time (different rooms), so they share a slot.
//
// Vents are the non-smoking emitters: a ramen bowl, an AC exhaust, a bath.
// Two slots because a set never shows more than two columns at once, and
// because every extra slot is pool budget taken away from the cigarettes,
// which are the effect people actually look at.
enum class Source : uint8_t {
    PigCig = 0,
    BarmanCig,
    PatronCig,
    Vent0,
    Vent1,
    Count
};

enum class Breath : uint8_t { Drag, Hold, Exhale, Rest };

struct BreathFrame {
    Breath   phase = Breath::Rest;
    uint16_t phaseElapsedMs = 0;
    uint16_t phaseDurationMs = 1;
    uint32_t cycleIndex = 0;
    uint8_t  emberHeat = 96;   // 0..255 — drive the ember cell from this
    uint8_t  exhale8 = 0;      // 0..255 — exhale envelope, for cheek/mouth poses
};

static constexpr uint32_t kBreathPeriodMs = 5200;

// Pure function of now — safe to sample from any pass, including mirrors.
BreathFrame sampleBreath(uint32_t now, uint32_t offsetMs = 0,
                         uint32_t periodMs = kBreathPeriodMs);

// ==[ VOLUMETRIC RESPONSE ]== up to two emitters per plume. More than two and
// the per-cell cost stops paying for itself at this screen size.
struct Lighting {
    PigLight light[2];
    float    radius[2];
    uint8_t  strength[2];
    uint8_t  count;
    Lighting() : radius{0.0f, 0.0f}, strength{0u, 0u}, count(0u) {}
    void add(const PigLight& l, float r, uint8_t s) {
        if (count >= 2u || l.tint == 0u || r < 1.0f || s == 0u) return;
        light[count] = l;
        radius[count] = r;
        strength[count] = s;
        ++count;
    }
};

// Smoke seen inside something — a mirror, a window, a lit doorway — must not
// crawl out of its frame. w <= 0 means no clip.
struct ClipBox {
    int x = 0, y = 0, w = 0, h = 0;
};

struct ExhaleParams {
    int      x = 0;          // mouth origin (the breath leaves here, not the ember)
    int      y = 0;
    int8_t   dirX = 1;       // which way the smoker is facing
    uint8_t  power = 210;    // 0..255 — how hard the breath is pushed
    uint8_t  scale = 100;    // 100 = full pig-scale cloud; lower = distant figure
    uint32_t seed = 0x5A11u;
};

// A vent is the opposite of a breath. There is no jet and no cycle: volumes
// just keep arriving, slowly, straight up, and the interesting part is what
// the room does to them on the way. Ramen bowl, AC exhaust, bath, manhole.
//
// rise and lifeMs together set how tall the column gets, and they are the two
// you actually tune. Verified against the integrator: rise=10/life=1400 climbs
// ~55px, rise=14/life=1800 climbs ~99px. Overshoot the ceiling and the column
// culls itself at the room edge, which reads as a hard cut, so size the climb
// to the headroom the prop actually has.
struct VentParams {
    int      x = 0;
    int      y = 0;
    uint16_t intervalMs = 420;   // gap between volumes leaving the mouth
    uint8_t  rise = 10;          // launch speed, 1/16 px per 16ms tick
    uint16_t lifeMs = 1500;
    uint8_t  spreadPx = 6;       // lateral scatter across the vent mouth
    uint8_t  scale = 70;         // 100 = full pig-scale volume
    uint8_t  opacity = 130;
    uint32_t seed = 0x7E27u;
};

// ==[ LIFECYCLE ]==
// update() integrates the pool and ages the wisp declarations. Call it once
// per rendered frame, before any smoke draw.
void update(uint32_t now);
// Rooms are hard cuts: smoke may not survive a teleport. Everything else must.
void setScene(uint32_t sceneKey);
void clear();
void clearSource(Source src);

// Call every frame; each ~130ms step of an Exhale phase spawns one volume, so
// the burst reads as a breath instead of a single popped blob.
void driveExhale(Source src, const BreathFrame& bf, const ExhaleParams& p);
// Call every frame a vent is running. Spawns on its own clock, so a stalled
// frame thins the column instead of dumping a backlog of volumes at once.
// Vents are rate-limited against the shared pool: a column can never starve a
// cigarette, it just gets sparser while the smokers are busy.
void driveVent(Source src, uint32_t now, const VentParams& p);
// Scatter what is already airborne (pig submerging, door slam). token dedups —
// pass the event's start time so a multi-frame phase disturbs exactly once.
void disturb(Source src, uint32_t token, uint8_t strength);
// Every volume on screen at once, from one event the whole set felt: a door,
// a teleport arrival, thunder, a car pulling up outside. This is the beat that
// ties the scenes together — the same shove reads the same everywhere, which
// is the entire point of one shared volume system.
// token dedups exactly like disturb(); pass the event's start time.
void gust(uint32_t token, uint8_t strength);

// Mints the token gust()/disturb() want for a source that publishes a *state*
// rather than an event: a thunder flag, a jump progress ratio. Latches on the
// rising edge, so every frame of one event yields the same token and the shove
// lands exactly once. Sources that already own a real event timestamp (the
// rooftop car) should pass that straight through instead.
//
// Returns 0 while idle, which is never a valid token — so the guarded form
//     if (uint32_t tk = latch.sample(active, now)) gust(tk, strength);
// is the whole usage. Never call gust(0, ...): it does not match a live
// dedup token and would re-shove the pool on every frame.
struct EdgeToken {
    uint32_t token = 0;
    uint32_t sample(bool active, uint32_t now) {
        if (!active) return token = 0;
        if (token == 0u) token = now ? now : 1u;
        return token;
    }
};

void draw(M5Canvas& canvas, Source src, uint16_t colNear, uint16_t colFar,
          const Lighting* lighting = nullptr, const ClipBox* clip = nullptr);

// ==[ EMBER THREAD ]==
// Declare the wisp every frame it should exist. The module fades it in, and on
// the frame you stop declaring it, converts what was on screen into puffs.
// baseX/baseY are the ember's own coordinates, at whatever grid the prop uses;
// drawWisp floor-snaps them onto the room lattice, which is what keeps the 4px
// thread covering the 2px coal. Pass the coal position and let it quantise.
// baseY must clear the prop by one FULL room cell (kRoomPX), not one pig cell:
// the floor snap can drop the base cell up to 3px below the y you pass, and a
// haze cell landing on the coal washes the ember out in half the phases.
void setWisp(Source src, int baseX, int baseY, int topY, int dir,
             uint32_t seed);
void drawWisp(M5Canvas& canvas, Source src, uint32_t now,
              uint16_t colNear, uint16_t colFar,
              const Lighting* lighting = nullptr,
              const ClipBox* clip = nullptr);

} // namespace SmokeFx
