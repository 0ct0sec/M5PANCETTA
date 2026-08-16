/** smoke_volume.cpp — Blade Runner cigarette smoke (see smoke_volume.h) */

#include "smoke_volume.h"

#include "ui_measurements.h"
#include "../gfx/gfx.h"

namespace SmokeFx {

using UIMeasurements::MenuPigLayout::kRoomY;
using UIMeasurements::MenuPigLayout::kFloorY;
using UIMeasurements::MenuPigLayout::kRoomPX;

namespace {

// Atmosphere lives on the scenery lattice. The cigarette is 2px detail; its
// smoke is not, and mixing the two makes the plume read as UI noise.
// This is the single cell size for EVERY draw in this file — puffs and the
// ember thread alike. See the sizing rule at the top of smoke_volume.h.
constexpr int      kCell = kRoomPX;
constexpr int      kPoolSize = 24;
constexpr int      kMaxRadiusQ2 = 26;      // quarter-cells → 6.5 cells ≈ 26px
constexpr uint16_t kExhaleStepMs = 130;
constexpr uint32_t kExhaleSteps = 5;
// Vents run continuously and cigarettes do not, so an unbounded vent would win
// every recycle in allocPuff() and quietly eat the smokers. Cap the columns
// instead: a busy set thins its steam, it never drops a drag.
constexpr int      kVentQuota = 5;

struct Puff {
    int16_t  xQ4 = 0, yQ4 = 0;     // position, 1/16 px
    int16_t  vxQ4 = 0, vyQ4 = 0;   // velocity, 1/16 px per 16ms tick
    uint16_t ageMs = 0;
    uint16_t lifeMs = 1;
    uint8_t  radiusQ2 = 0;         // birth radius, quarter-cells
    uint8_t  growQ2 = 0;           // quarter-cells gained per second
    uint8_t  opacity = 0;
    uint8_t  seed = 0;
    uint8_t  src = 0;
    uint8_t  swayPhase = 0;
    bool     alive = false;
};

struct SourceState {
    uint32_t lastStep = 0;
    uint32_t lastVent = 0;
    uint32_t lastDisturb = 0;
    uint32_t wispSeed = 0x51F7u;
    int16_t  wispX = 0, wispY = 0, wispTopY = 0;
    int8_t   wispDir = 0;
    uint8_t  wisp8 = 0;
    bool     wispRequested = false;
    bool     wispReleased = true;
};

Puff        pool[kPoolSize];
SourceState sources[(int)Source::Count];
uint32_t    sceneKey = 0;
uint32_t    lastUpdateMs = 0;
uint32_t    lastGust = 0;
bool        clockValid = false;

// The vents are the tail of the enum, so a new column never has to be added
// here as well.
inline bool isVent(uint8_t src) { return src >= (uint8_t)Source::Vent0; }

int liveVentCount() {
    int n = 0;
    for (int i = 0; i < kPoolSize; ++i) {
        if (pool[i].alive && isVent(pool[i].src)) ++n;
    }
    return n;
}

// Recycle the most-dissipated volume when the pool is full: a busy scene then
// degrades by dropping the puff nobody is looking at any more.
Puff* allocPuff() {
    Puff* worst = &pool[0];
    uint32_t worstScore = 0;
    for (int i = 0; i < kPoolSize; ++i) {
        if (!pool[i].alive) return &pool[i];
        const uint16_t life = pool[i].lifeMs ? pool[i].lifeMs : 1u;
        const uint32_t score = (uint32_t)pool[i].ageMs * 255u / life;
        if (score >= worstScore) { worstScore = score; worst = &pool[i]; }
    }
    return worst;
}

void integrate(Puff& q, uint32_t dt) {
    const uint32_t age = (uint32_t)q.ageMs + dt;
    if (age >= q.lifeMs) { q.alive = false; return; }
    q.ageMs = (uint16_t)age;

    q.xQ4 = (int16_t)(q.xQ4 + (int32_t)q.vxQ4 * (int32_t)dt / 16);
    q.yQ4 = (int16_t)(q.yQ4 + (int32_t)q.vyQ4 * (int32_t)dt / 16);

    // Drag kills the breath's forward jet in ~260ms. That stall — not the
    // launch — is what makes an exhale read as a slug of fog rather than a
    // squirt, so it is the one constant worth protecting here.
    // The floor of one unit is not a rounding nicety: proportional decay in
    // integers stops subtracting once vxQ4 * drag < 256, which parks the puff
    // at a permanent ~27px/s and sails the whole cloud off the side of the set.
    int32_t drag = (int32_t)dt * 256 / 260;
    if (drag > 256) drag = 256;
    int32_t dvx = (int32_t)q.vxQ4 * drag / 256;
    if (dvx == 0 && q.vxQ4 != 0) dvx = (q.vxQ4 > 0) ? 1 : -1;
    q.vxQ4 = (int16_t)(q.vxQ4 - dvx);

    // Buoyancy: vertical speed relaxes toward a slow, steady climb.
    constexpr int32_t kRiseQ4 = -7;   // ≈ 27px/s upward once the breath is spent
    int32_t lift = (int32_t)dt * 256 / 900;
    if (lift > 256) lift = 256;
    q.vyQ4 = (int16_t)(q.vyQ4 + ((kRiseQ4 - (int32_t)q.vyQ4) * lift) / 256);

    // NB: the lateral wander is a draw-time offset (see drawPuff), not an
    // impulse integrated here. Feeding a slow sine into velocity against a
    // 260ms drag builds an equilibrium of ~160px/s of sideways drift and the
    // whole cloud sails off across the room.
    const int px = q.xQ4 >> 4;
    const int py = q.yQ4 >> 4;
    if (px < -40 || px > SCREEN_WIDTH + 40 ||
        py < kRoomY - 48 || py > kFloorY + 24) q.alive = false;
}

inline bool clipRejects(const ClipBox* clip, int px, int py, int cellPx) {
    if (!clip || clip->w <= 0 || clip->h <= 0) return false;
    return px < clip->x || py < clip->y ||
           px + cellPx > clip->x + clip->w ||
           py + cellPx > clip->y + clip->h;
}

// Strongest emitter wins; returns 0..255 of light landing on this cell.
float sampleLight(const Lighting* lighting, int px, int py, uint16_t& tintOut) {
    tintOut = 0;
    if (!lighting || lighting->count == 0u) return 0.0f;
    const float cx = (float)px + (float)kCell * 0.5f;
    const float cy = (float)py + (float)kCell * 0.5f;
    float best = 0.0f;
    for (int i = 0; i < lighting->count; ++i) {
        const float dx = (float)lighting->light[i].x - cx;
        const float dy = (float)lighting->light[i].y - cy;
        const float d2 = dx * dx + dy * dy;
        const float rr = lighting->radius[i] * lighting->radius[i];
        if (d2 >= rr) continue;
        const float f = 1.0f - d2 / rr;
        const float s = (float)lighting->strength[i] * f * f;
        if (s > best) { best = s; tintOut = lighting->light[i].tint; }
    }
    if (best <= 0.0f) tintOut = 0;
    return best;
}

void drawPuff(M5Canvas& canvas, const Puff& q,
              uint16_t colNear, uint16_t colFar, const Lighting* lighting,
              const ClipBox* clip) {
    const float t = (float)q.ageMs / (float)(q.lifeMs ? q.lifeMs : 1u);

    int rQ2 = (int)q.radiusQ2 + (int)((uint32_t)q.growQ2 * q.ageMs / 1000u);
    if (rQ2 > kMaxRadiusQ2) rQ2 = kMaxRadiusQ2;
    if (rQ2 < 2) rQ2 = 2;
    const int rCells = (rQ2 + 3) / 4;

    // Volume is conserved: a swelling puff thins out. Without this the cloud
    // grows into a solid wall instead of dissolving into the room.
    float alpha = (float)q.opacity * (float)q.radiusQ2 / (float)rQ2;
    const float fade = 1.0f - t;
    alpha *= fade * fade * (0.45f + 0.55f * fade);
    if (t < 0.08f) alpha *= t / 0.08f;
    if (alpha < 3.0f) return;

    const uint16_t col = Gfx::lerpColor565_8(
        colNear, colFar, (uint8_t)(Gfx::clamp01(t * 1.4f) * 255.0f));
    // Bounded lateral wander, wider as the volume grows. Bounded because it is
    // an offset, not an accumulated impulse — the physics stays a clean
    // drag-plus-buoyancy solve and the puff cannot walk away from its own path.
    const float swayT = (float)q.ageMs * 0.0021f + (float)q.swayPhase * 0.049f;
    const int sway = (int)(Gfx::fastSinf(swayT) * (2.0f + (float)rQ2 * 0.22f));
    const int cx = ((q.xQ4 >> 4) + sway) & ~(kCell - 1);
    const int cy = (q.yQ4 >> 4) & ~(kCell - 1);
    // The coverage mask crawls upward through the volume instead of being
    // re-rolled, so the smoke boils rather than flickering like static.
    const int churnRow = (int)((uint32_t)q.ageMs / 300u);
    const uint32_t mask = (uint32_t)q.seed * 2654435761u + 0x9E3779B9u;
    const int r2 = rCells * rCells;

    for (int gy = -rCells; gy <= rCells; ++gy) {
        const int py = cy + gy * kCell;
        if (py < kRoomY || py > kFloorY - kCell) continue;
        for (int gx = -rCells; gx <= rCells; ++gx) {
            // Breath spreads sideways: the volume is wider than it is tall.
            const int d2 = gx * gx + gy * gy * 2;
            if (d2 > r2) continue;
            const int px = cx + gx * kCell;
            if (px < 0 || px > SCREEN_WIDTH - kCell) continue;
            if (clipRejects(clip, px, py, kCell)) continue;

            const float core = 1.0f - (float)d2 / (float)r2;
            float dens = alpha * (0.30f + 0.70f * core);

            uint16_t litTint = 0;
            const float lit = sampleLight(lighting, px, py, litTint);
            // Lit smoke reads THICKER, not just brighter. That density kick is
            // the whole trick behind a plume standing inside a light shaft.
            dens += lit * 0.35f;
            if (dens > 255.0f) dens = 255.0f;
            const uint8_t d8 = (uint8_t)dens;
            if (d8 < 6u) continue;

            uint32_t cov = (uint32_t)d8 * 8u / 5u;
            if (cov > 255u) cov = 255u;
            if ((Gfx::wallHash(gx, gy - churnRow, mask) & 0xFFu) >= cov) continue;

            uint16_t tinted = col;
            if (lit > 1.0f && litTint != 0u) {
                tinted = Gfx::screenBlend565(
                    col, litTint, (uint8_t)(lit > 255.0f ? 255.0f : lit));
            }
            const uint16_t base = Gfx::fastReadPx(canvas, px, py);
            // 118 is where a fresh slug's core lands. Above it the fog is
            // opaque enough to hide what is behind it, which is the difference
            // between an exhale and a highlight; below it, it only glows.
            const uint16_t out = (d8 > 118u)
                ? Gfx::lerpColor565_8(base, tinted, d8)   // core occludes
                : Gfx::screenBlend565(base, tinted, d8);  // fringe glows
            canvas.fillRect(px, py, kCell, kCell, out);
        }
    }
}

// A thread that stops being declared must not blink out: seed drifting volumes
// along the path it occupied so the eye follows the smoke instead of the cut.
void releaseWisp(Source src, SourceState& st) {
    const int span = (int)st.wispY - (int)st.wispTopY;
    if (span <= 4 || st.wisp8 < 24u) return;
    for (int i = 0; i < 3; ++i) {
        Puff* q = allocPuff();
        if (!q) return;
        const uint32_t h = Gfx::wallHash(st.wispX, (int)st.wispY + i,
                                         st.wispSeed + 0x51F7u);
        const int frac = 20 + i * 32;                    // 20% / 52% / 84% up
        const int y = (int)st.wispY - span * frac / 100;
        const int x = (int)st.wispX + st.wispDir * (span * frac / 400) +
                      (int)(h & 7u) - 3;
        q->xQ4 = (int16_t)(x * 16);
        q->yQ4 = (int16_t)(y * 16);
        q->vxQ4 = (int16_t)(st.wispDir * (2 + (int)((h >> 3) & 3u)));
        q->vyQ4 = -4;
        q->ageMs = 0;
        q->lifeMs = (uint16_t)(1500u + (uint32_t)i * 320u + ((h >> 6) & 255u));
        q->radiusQ2 = (uint8_t)(3 + i);
        q->growQ2 = 5;
        q->opacity = (uint8_t)(((uint32_t)st.wisp8 * (150u - (uint32_t)i * 22u)) / 255u);
        q->seed = (uint8_t)(h >> 12);
        q->src = (uint8_t)src;
        q->swayPhase = (uint8_t)(h >> 19);
        q->alive = true;
    }
}

void spawnExhalePuff(Source src, const ExhaleParams& e, uint32_t step) {
    Puff* q = allocPuff();
    if (!q) return;
    const int scale = e.scale ? (int)e.scale : 100;
    const uint32_t h = Gfx::wallHash(e.x + (int)step, e.y,
                                     e.seed + step * 0x9E37u);

    // Front of the breath is the jet: fast, tight, opaque. Each later step is
    // slower, wider and softer — the slug stalling out into a cloud.
    int speed = (40 - (int)step * 6) * scale / 100;
    speed = speed * (int)e.power / 255;
    if (speed < 4) speed = 4;

    q->xQ4 = (int16_t)(e.x * 16);
    q->yQ4 = (int16_t)(e.y * 16);
    q->vxQ4 = (int16_t)(e.dirX * speed);
    q->vyQ4 = (int16_t)(3 + (int)(h & 3u));   // warm breath sags, then lifts
    q->ageMs = 0;
    q->lifeMs = (uint16_t)(2400u + step * 420u + ((h >> 4) & 511u));
    q->radiusQ2 = (uint8_t)((5 + (int)step * 3) * scale / 100 + 1);
    q->growQ2 = (uint8_t)((5 + (int)step) * scale / 100 + 1);
    q->opacity = (uint8_t)(((235 - (int)step * 18) * scale) / 100);
    q->seed = (uint8_t)(h >> 13);
    q->src = (uint8_t)src;
    q->swayPhase = (uint8_t)(h >> 21);
    q->alive = true;
}

void spawnVentPuff(Source src, const VentParams& v, uint32_t now) {
    Puff* q = allocPuff();
    if (!q) return;
    const int scale = v.scale ? (int)v.scale : 100;
    const uint32_t h = Gfx::wallHash(v.x, v.y + (int)(now >> 7), v.seed);

    // The mouth is a slot, not a point: scatter the origin across it or the
    // column reads as a single stuttering pixel stack instead of a plume.
    const int spread = (int)v.spreadPx;
    const int ox = spread ? ((int)(h & 63u) * (spread * 2) / 63 - spread) : 0;

    q->xQ4 = (int16_t)((v.x + ox) * 16);
    q->yQ4 = (int16_t)(v.y * 16);
    // No jet. A trace of lateral bias only, so neighbouring volumes separate.
    q->vxQ4 = (int16_t)((int)((h >> 6) & 3u) - 1);
    q->vyQ4 = (int16_t)(-(int)v.rise);
    q->ageMs = 0;
    q->lifeMs = (uint16_t)(v.lifeMs + ((h >> 8) & 255u));
    q->radiusQ2 = (uint8_t)(4 * scale / 100 + 1);
    q->growQ2 = (uint8_t)(7 * scale / 100 + 1);
    q->opacity = (uint8_t)((int)v.opacity * scale / 100);
    q->seed = (uint8_t)(h >> 16);
    q->src = (uint8_t)src;
    q->swayPhase = (uint8_t)(h >> 23);
    q->alive = true;
}

// Shared impulse behind disturb() and gust(). i is the pool index, so the same
// event scatters every volume differently instead of translating the cloud.
void shove(Puff& q, int i, uint32_t token, uint8_t strength) {
    const uint32_t h = Gfx::wallHash(i, (int)token, 0xD157u);
    q.vxQ4 = (int16_t)(q.vxQ4 + (((int)(h & 31u) - 16) * (int)strength) / 128);
    q.vyQ4 = (int16_t)(q.vyQ4 - (int)((h >> 5) & 7u));
    int grow = (int)q.growQ2 + (int)strength / 24;
    q.growQ2 = (uint8_t)(grow > 24 ? 24 : grow);
    const uint32_t left = (uint32_t)q.lifeMs - q.ageMs;
    q.lifeMs = (uint16_t)(q.ageMs + left * (255u - strength / 2u) / 255u + 1u);
}

}  // namespace

// ==[ BREATH ]== drag / hold / exhale / rest, pure function of now.
BreathFrame sampleBreath(uint32_t now, uint32_t offsetMs, uint32_t periodMs) {
    BreathFrame bf;
    if (periodMs < 1600u) periodMs = 1600u;
    const uint32_t local = (now + offsetMs) % periodMs;
    bf.cycleIndex = (now + offsetMs) / periodMs;

    const uint32_t dragMs = 1100u;
    const uint32_t holdMs = 800u;
    const uint32_t exhaleMs = kExhaleStepMs * kExhaleSteps;   // 650
    const uint32_t dragEnd = dragMs;
    const uint32_t holdEnd = dragEnd + holdMs;
    const uint32_t exhaleEnd = holdEnd + exhaleMs;

    if (local < dragEnd) {
        bf.phase = Breath::Drag;
        bf.phaseElapsedMs = (uint16_t)local;
        bf.phaseDurationMs = (uint16_t)dragMs;
        // Ember climbs through the pull, with a live coal flicker on top. Signed
        // math: the flicker is bipolar and unsigned wrap would pin it to 255.
        int32_t heat = 110 + (int32_t)((local * 145u) / dragMs);
        heat += (int32_t)(Gfx::wallHash((int)(local / 90u), 0, 0xE3B1u) & 31u) - 15;
        if (heat > 255) heat = 255;
        if (heat < 0) heat = 0;
        bf.emberHeat = (uint8_t)heat;
        return bf;
    }
    if (local < holdEnd) {
        bf.phase = Breath::Hold;
        bf.phaseElapsedMs = (uint16_t)(local - dragEnd);
        bf.phaseDurationMs = (uint16_t)holdMs;
        bf.emberHeat = (uint8_t)(200u - (bf.phaseElapsedMs * 90u) / holdMs);
        return bf;
    }
    if (local < exhaleEnd) {
        bf.phase = Breath::Exhale;
        bf.phaseElapsedMs = (uint16_t)(local - holdEnd);
        bf.phaseDurationMs = (uint16_t)exhaleMs;
        bf.emberHeat = 104u;
        // Hard attack, long tail — the mouth opens fast and empties slowly.
        const uint32_t e = bf.phaseElapsedMs;
        bf.exhale8 = (e < 120u)
            ? (uint8_t)((e * 255u) / 120u)
            : (uint8_t)(255u - ((e - 120u) * 235u) / (exhaleMs - 120u));
        return bf;
    }
    bf.phase = Breath::Rest;
    bf.phaseElapsedMs = (uint16_t)(local - exhaleEnd);
    bf.phaseDurationMs = (uint16_t)(periodMs - exhaleEnd);
    // Idling coal breathes on its own convection.
    bf.emberHeat = (uint8_t)(92u + ((Gfx::wallHash((int)(local / 260u), 1,
                                                   0xE3B1u) & 31u)));
    return bf;
}

// ==[ LIFECYCLE ]==

void update(uint32_t now) {
    uint32_t dt = 0;
    if (clockValid) {
        dt = now - lastUpdateMs;
        // A frame stall must not teleport the smoke across the room.
        if (dt > 120u) dt = 120u;
    }
    const bool frozenClock = clockValid && dt == 0u;
    clockValid = true;
    lastUpdateMs = now;

    // Wisp declarations are consumed here. A source that stopped drawing last
    // frame starts breaking up on this one.
    for (int i = 0; i < (int)Source::Count; ++i) {
        SourceState& st = sources[i];
        if (st.wispRequested) {
            st.wispRequested = false;
            st.wispReleased = false;
            // A frozen clock (deterministic capture path) has no fade to run.
            uint32_t rise = frozenClock ? 255u : (dt * 255u) / 420u;
            uint32_t v = (uint32_t)st.wisp8 + rise;
            st.wisp8 = (uint8_t)(v > 255u ? 255u : v);
        } else if (st.wisp8 > 0u) {
            if (!st.wispReleased) {
                releaseWisp((Source)i, st);
                st.wispReleased = true;
            }
            const uint32_t fall = frozenClock ? 255u : (dt * 255u) / 560u;
            st.wisp8 = (fall >= st.wisp8) ? 0u : (uint8_t)(st.wisp8 - fall);
        }
    }

    if (dt == 0u) return;
    for (int i = 0; i < kPoolSize; ++i) {
        if (pool[i].alive) integrate(pool[i], dt);
    }
}

void clear() {
    for (int i = 0; i < kPoolSize; ++i) pool[i].alive = false;
    for (int i = 0; i < (int)Source::Count; ++i) sources[i] = SourceState();
    // A stale token here would swallow the first gust of the next scene, and
    // the arrival that triggers it is exactly the one worth seeing.
    lastGust = 0;
}

void clearSource(Source src) {
    for (int i = 0; i < kPoolSize; ++i) {
        if (pool[i].src == (uint8_t)src) pool[i].alive = false;
    }
    sources[(uint8_t)src] = SourceState();
}

void setScene(uint32_t key) {
    if (key == sceneKey) return;
    sceneKey = key;
    clear();
}

// ==[ EMISSION ]==

void driveExhale(Source src, const BreathFrame& bf, const ExhaleParams& p) {
    if (bf.phase != Breath::Exhale) return;
    uint32_t step = bf.phaseElapsedMs / kExhaleStepMs;
    if (step >= kExhaleSteps) step = kExhaleSteps - 1u;
    // +1 so a fresh SourceState (lastStep == 0) can never swallow the first
    // step of cycle 0.
    const uint32_t stepId = bf.cycleIndex * 16u + step + 1u;
    SourceState& st = sources[(uint8_t)src];
    if (st.lastStep == stepId) return;
    st.lastStep = stepId;
    spawnExhalePuff(src, p, step);
}

void driveVent(Source src, uint32_t now, const VentParams& p) {
    SourceState& st = sources[(uint8_t)src];
    const uint32_t interval = p.intervalMs ? p.intervalMs : 1u;

    // A cold state (lastVent == 0) must emit now, not wait out one interval,
    // or every room entry opens on a dead vent.
    if (st.lastVent != 0u) {
        const uint32_t since = now - st.lastVent;
        // Cheap reject first. The quota only matters for the handful of frames
        // that have already cleared the base interval, so the pool scan stays
        // off the common path — this runs twice a frame in the bath.
        if (since < interval) return;
        // Thin the column rather than steal from the smokers once the vents
        // have had their share of the pool.
        if (since < interval * 3u && liveVentCount() >= kVentQuota) return;
    }
    // Never replay a backlog after a stall: one volume per eligible frame.
    st.lastVent = now;
    spawnVentPuff(src, p, now);
}

void disturb(Source src, uint32_t token, uint8_t strength) {
    SourceState& st = sources[(uint8_t)src];
    if (st.lastDisturb == token) return;
    st.lastDisturb = token;
    if (strength == 0u) return;
    for (int i = 0; i < kPoolSize; ++i) {
        Puff& q = pool[i];
        if (!q.alive || q.src != (uint8_t)src) continue;
        shove(q, i, token, strength);
    }
}

void gust(uint32_t token, uint8_t strength) {
    if (lastGust == token) return;
    lastGust = token;
    if (strength == 0u) return;
    for (int i = 0; i < kPoolSize; ++i) {
        if (pool[i].alive) shove(pool[i], i, token, strength);
    }
}

// ==[ DRAW ]==

void draw(M5Canvas& canvas, Source src, uint16_t colNear, uint16_t colFar,
          const Lighting* lighting, const ClipBox* clip) {
    for (int i = 0; i < kPoolSize; ++i) {
        if (!pool[i].alive || pool[i].src != (uint8_t)src) continue;
        drawPuff(canvas, pool[i], colNear, colFar, lighting, clip);
    }
}

void setWisp(Source src, int baseX, int baseY, int topY, int dir,
             uint32_t seed) {
    SourceState& st = sources[(uint8_t)src];
    st.wispX = (int16_t)baseX;
    st.wispY = (int16_t)baseY;
    st.wispTopY = (int16_t)topY;
    st.wispDir = (int8_t)dir;
    st.wispSeed = seed;
    st.wispRequested = true;
}

void drawWisp(M5Canvas& canvas, Source src, uint32_t now,
              uint16_t colNear, uint16_t colFar, const Lighting* lighting,
              const ClipBox* clip) {
    SourceState& st = sources[(uint8_t)src];
    if (st.wisp8 == 0u) return;
    // The thread is atmosphere, so it is one room cell thick — never the 2px
    // cell of the coal that makes it. Floor-snapping baseX (not rounding) is
    // what guarantees the room cell still covers the 2px ember it grows out of.
    constexpr int p = kCell;
    const int baseX = (int)st.wispX & ~(p - 1);
    const int baseY = (int)st.wispY & ~(p - 1);
    const int topY = ((int)st.wispTopY + p - 1) & ~(p - 1);
    const int span = baseY - topY;
    if (span < 6 * p) return;

    // Wrap-safe travelling phase: modulus × rate lands on ~2π exactly, so the
    // serpentine never jumps when millis() rolls the modulus.
    const float risePhase = (float)(now % 5236u) * 0.0012f;
    const int laceScroll = (int)(now / 210u);

    for (int y = baseY; y > topY; y -= p) {
        if (y < kRoomY || y > kFloorY - p) continue;
        const float h = (float)(baseY - y) / (float)span;   // 0 ember → 1 top
        // Tight laminar thread at the coal, loosening into a slow S as it cools.
        const float sway =
            Gfx::fastSinf(h * 4.6f - risePhase) * (0.5f + h * 7.5f) +
            (float)st.wispDir * h * 9.0f;
        const int cx = (baseX + (int)floorf(sway)) & ~(p - 1);
        // One cell of core, and the side cells only in the top third: on the
        // room grid a side cell costs 4px per flank, so an earlier fray turns
        // the thread into a 12px wall instead of smoke coming apart.
        const int halfW = (h > 0.72f) ? 1 : 0;
        const uint16_t col =
            Gfx::lerpColor565_8(colNear, colFar, (uint8_t)(h * 255.0f));
        const int row = (baseY - y) / p;

        for (int c = -halfW; c <= halfW; ++c) {
            const int x = cx + c * p;
            if (x < 0 || x > SCREEN_WIDTH - p) continue;
            if (clipRejects(clip, x, y, p)) continue;
            // Solid at the coal, quadratically frayed as it dissolves.
            int keep = 256 - (int)(h * h * 245.0f);
            if (c != 0) keep /= 2;
            keep = keep * (int)st.wisp8 / 255;
            if (keep <= 2) continue;
            if ((Gfx::wallHash(c, row - laceScroll, st.wispSeed) & 0xFFu) >=
                (uint32_t)keep) continue;

            uint16_t litTint = 0;
            const float lit = sampleLight(lighting, x, y, litTint);
            uint16_t tinted = col;
            if (lit > 1.0f && litTint != 0u) {
                tinted = Gfx::screenBlend565(
                    col, litTint, (uint8_t)(lit > 255.0f ? 255.0f : lit));
            }
            int a = (190 - (int)(h * 120.0f)) * (int)st.wisp8 / 255 +
                    (int)(lit * 0.25f);
            if (a > 255) a = 255;
            if (a < 4) continue;
            const uint16_t base = Gfx::fastReadPx(canvas, x, y);
            canvas.fillRect(x, y, p, p,
                            Gfx::screenBlend565(base, tinted, (uint8_t)a));
        }
    }
}

}  // namespace SmokeFx
