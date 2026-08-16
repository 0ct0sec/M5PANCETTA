/** sky_volume.cpp — rooftop sky: cloud deck, spinner lanes, ad blimp (see .h) */

#include "sky_volume.h"

#include "menu_pig_render.h"   // RP palette + blend helpers
#include "ui_measurements.h"
#include "../gfx/gfx.h"

#include <string.h>

namespace SkyFx {

using UIMeasurements::MenuPigLayout::kRoomPX;
using MenuPigRender::RP::BG;

namespace {

// Sky is scenery. One cell size for every draw in this file — see the sizing
// contract at the top of sky_volume.h.
constexpr int kCell = kRoomPX;

// ==[ CLOUD SHAPES ]==
// Per column: the first and last occupied row, measured down from the mass's
// own top-left. Cumulus have flat bases and lumpy tops, and that asymmetry is
// the whole silhouette read — a blob ragged on every edge is smoke, not
// weather. Bases stay near-flat and only break at the ends.
struct CloudShape {
    uint8_t cols;
    uint8_t rows;
    const uint8_t* top;
    const uint8_t* bot;   // inclusive
};

const uint8_t kBankTop[] = {3,3,2,2,1,0,0,1,1,2,1,2,3,3,3,4};
const uint8_t kBankBot[] = {3,4,4,4,4,4,4,4,4,4,4,4,4,4,3,4};
const uint8_t kTowerTop[] = {5,4,2,1,0,0,1,2,3,4};
const uint8_t kTowerBot[] = {5,5,5,5,5,5,5,5,5,4};
const uint8_t kScudTop[] = {2,2,1,1,0,1,1,0,0,1,1,2,1,1,0,1,2,2,2,2};
const uint8_t kScudBot[] = {2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2};
const uint8_t kPuffTop[] = {3,2,1,0,0,1,2,3};
const uint8_t kPuffBot[] = {3,3,3,3,3,3,3,3};

const CloudShape kShapes[] = {
    {16, 5, kBankTop,  kBankBot},
    {10, 6, kTowerTop, kTowerBot},
    {20, 3, kScudTop,  kScudBot},
    { 8, 4, kPuffTop,  kPuffBot},
};

// ==[ MASSES ]==
// baseX is a world coordinate on a wrap wider than the screen, so masses enter
// and leave instead of popping at the frame edge. rowOff is in cells from the
// top of the viewport. Bands are separated vertically on purpose: the deck
// resolves overlap by draw order, and keeping the bands apart means the rare
// crossing is the only place that ordering has to be right.
struct Mass {
    uint8_t shape;
    uint8_t band;      // 0 far, 1 mid, 2 near
    uint16_t baseX;
    uint8_t rowOff;
};

constexpr int kWorld = 480;      // wrap width
constexpr int kMargin = 88;      // lead-in/lead-out beyond the viewport

const Mass kMasses[] = {
    {2, 0,   0, 0},
    {0, 0, 190, 2},
    {3, 0, 350, 1},
    {0, 1,  60, 6},
    {1, 1, 250, 5},
    {3, 1, 410, 8},
    {2, 2, 130, 17},
    {0, 2, 330, 16},
    // Two low scud banks. Without them the deck stops dead at a third of the
    // way down and leaves 50px of flat sky between the weather and the city —
    // the thin shape is deliberate, because cloud this near the horizon is
    // torn sheet, not the cumulus that sits above it.
    {2, 2,  60, 23},
    {2, 2, 300, 21},
};
constexpr int kMassCount = (int)(sizeof(kMasses) / sizeof(kMasses[0]));

// px/s per band. Slow enough that a 4px cell step lands every ~0.35-1.3s,
// which is about as fast as a fat-pixel cloud can move before the step itself
// becomes the effect you are looking at.
const uint8_t kBandSpeed[3] = {3, 6, 11};

// ==[ AIRSHIP ]==
constexpr uint32_t kShipCrossMs = 34000u;
constexpr uint32_t kShipIdleMinMs = 62000u;
constexpr uint32_t kShipIdleSpanMs = 48000u;
constexpr int kHullCols = 18;
constexpr int kHullRows = 4;
// Hull profile per row: first and last occupied column. A lozenge, fatter
// below the waist, because the belly carries the lit surface.
const uint8_t kHullL[kHullRows] = {5, 2, 1, 3};
const uint8_t kHullR[kHullRows] = {13, 15, 16, 14};

struct Airship {
    bool     active = false;
    uint32_t startMs = 0;
    uint32_t nextMs = 0;
    int16_t  rowOff = 1;
    int8_t   dir = 1;
    uint16_t seed = 0x31A7u;
};

Airship  ship;
uint32_t sceneKeyCur = 0;
bool     clockValid = false;

// Published searchlight impact, refreshed by drawSky.
bool    beamOn = false;
int     beamGroundX = 0;
int     beamGroundY = 0;
uint8_t beamStrength = 0;

// ==[ COVERAGE ]==
// Which cells this module has already painted this frame, one bit each. Near
// bands are allowed to paint over far ones, so they need to know a cloud cell
// from an architecture cell — and the cheap version of that test, comparing
// the pixel against the deck's own colours, is wrong: the near band's crown is
// lerp(BG, DEEP, 255), which IS RP::DEEP, and RP::DEEP is also the shack door
// and its window. Colour equality would have let a low cloud eat them. 340
// bytes of explicit coverage cannot make that mistake.
constexpr int kMaskCols = 80;                 // 320 / kCell
constexpr int kMaskRows = 34;
constexpr int kMaskStride = (kMaskCols + 7) / 8;
uint8_t deckMask[kMaskRows][kMaskStride];

void clearCoverage() { memset(deckMask, 0, sizeof(deckMask)); }

inline bool cellIndex(const Params& p, int px, int py, int& col, int& row) {
    col = (px - p.x) / kCell;
    row = (py - p.y) / kCell;
    return col >= 0 && col < kMaskCols && row >= 0 && row < kMaskRows;
}

inline bool isCovered(const Params& p, int px, int py) {
    int col, row;
    if (!cellIndex(p, px, py, col, row)) return false;
    return (deckMask[row][col >> 3] & (uint8_t)(1u << (col & 7))) != 0u;
}

inline void cover(const Params& p, int px, int py) {
    int col, row;
    if (!cellIndex(p, px, py, col, row)) return;
    deckMask[row][col >> 3] |= (uint8_t)(1u << (col & 7));
}

// A cell is writable by the sky if the retained architecture has not claimed
// it. The rooftop's mast, shack and city plate are drawn before this module
// runs, and nothing up here is allowed to eat them.
inline bool skyWritable(M5Canvas& c, const Params& p, int px, int py) {
    if (isCovered(p, px, py)) return true;
    return MenuPigRender::fastReadPx(c, px, py) == BG;
}

// X only. Y is never snapped in this file: every vertical coordinate here is
// p.y plus a whole number of cells, so it already sits on whatever lattice the
// caller's viewport uses. Room 3's playfield is the room lattice (kRoomY = 14,
// phase 2 mod 4) and forcing `& ~3` onto it would drop every cloud, spinner
// and hull row two pixels off the architecture they have to occlude against.
inline int snapX(int v) { return v & ~(kCell - 1); }

uint16_t applyTint(uint16_t c, const Params& p) {
    if (!p.tintActive || p.tintIntensity == 0u) return c;
    return MenuPigRender::mixColor565(c, p.tintColor565,
                                      (uint8_t)(p.tintIntensity / 8u));
}

struct BandStyle {
    uint16_t crown;   // top edge: unlit, or blown out when backlit
    uint16_t body;
    uint16_t belly;   // city glow on the cloud base
    uint8_t  fray;    // 0..255 — how much of the ragged edges dissolves
};

BandStyle bandStyle(uint8_t band, const Params& p) {
    using namespace MenuPigRender;
    BandStyle s;
    // Depth: the far band sits nearer the sky value at every stop, so the deck
    // recedes instead of stacking three equally present layers.
    const uint8_t depth8 = (band == 0) ? 118u : (band == 1 ? 186u : 255u);
    const uint16_t glow = Display::lerpColor565(RP::SHADOW_C, RP::WARM, 0.45f);

    if (p.thunder) {
        // Backlit: the cell that was darkest on the mass becomes the
        // brightest, because the light is now behind the deck instead of under
        // it. Nothing else about the silhouette changes.
        s.crown = lerpColor565_8(RP::SHADOW_C, RP::FLUOR, depth8);
        s.body  = lerpColor565_8(RP::BG, RP::WALL_MID, depth8);
        s.belly = lerpColor565_8(RP::BG, RP::DEEP, depth8);
        s.fray  = 96u;
        return s;
    }
    s.crown = lerpColor565_8(RP::BG, RP::DEEP, depth8);
    s.body  = lerpColor565_8(RP::BG, RP::SHADOW_C, depth8);
    s.belly = lerpColor565_8(RP::BG, glow, depth8);
    s.fray  = 128u;
    return s;
}

// ==[ ONE MASS ]==
void drawMass(M5Canvas& c, int mx, int my, const CloudShape& sh,
              const BandStyle& st, const Params& p, uint32_t seed) {
    const int vx0 = p.x, vx1 = p.x + p.w;
    const int vy0 = p.y, vy1 = p.y + p.h;
    for (int col = 0; col < (int)sh.cols; ++col) {
        const int px = mx + col * kCell;
        if (px < vx0 || px + kCell > vx1) continue;
        const int topRow = (int)sh.top[col];
        const int botRow = (int)sh.bot[col];
        if (botRow < topRow) continue;
        const bool endCol = (col == 0 || col == (int)sh.cols - 1);
        for (int row = topRow; row <= botRow; ++row) {
            const int py = my + row * kCell;
            if (py < vy0 || py + kCell > vy1) continue;

            // Fray the crown and the two end columns. A mass with a hard edge
            // all the way round reads as a cut-out, and the base is the only
            // edge a real cumulus keeps sharp.
            if (row == topRow || endCol) {
                uint32_t f = MenuPigRender::wallHash(px, py, seed ^ 0x5B31u);
                if ((f & 0xFFu) < st.fray) continue;
            }
            if (!skyWritable(c, p, px, py)) continue;

            uint16_t ink = (row == topRow) ? st.crown
                         : (row == botRow) ? st.belly : st.body;
            MenuPigRender::fastFillBlock4(c, px, py, applyTint(ink, p));
            cover(p, px, py);
        }
    }
}

void drawDeck(M5Canvas& c, uint32_t now, const Params& p, uint8_t band) {
    const BandStyle st = bandStyle(band, p);
    const uint32_t drift =
        (uint32_t)(((uint64_t)now * (uint64_t)kBandSpeed[band]) / 1000u);
    // Parallax scales with depth: the near band answers the camera hardest.
    const int cam = (int)p.parallaxX * (band == 0 ? 1 : (band == 1 ? 2 : 3)) / 2;

    for (int i = 0; i < kMassCount; ++i) {
        const Mass& m = kMasses[i];
        if (m.band != band) continue;
        const CloudShape& sh = kShapes[m.shape];
        const int wx = (int)(((uint32_t)m.baseX + (uint32_t)kWorld -
                              (drift % (uint32_t)kWorld)) % (uint32_t)kWorld);
        const int mx = snapX(p.x + wx - kMargin + cam);
        const int my = p.y + (int)m.rowOff * kCell;
        if (mx >= p.x + p.w || mx + (int)sh.cols * kCell <= p.x) continue;
        if (my + (int)sh.rows * kCell > p.y + p.h) continue;
        drawMass(c, mx, my, sh, st, p, 0x7A11u + (uint32_t)i * 0x3F1u);
    }
}

// ==[ SPINNERS ]== traffic, at altitude. A cloud with a car under it has a
// scale; a cloud on its own is a texture.
struct Lane {
    uint8_t rowOff;
    int8_t  dir;
    uint16_t periodMs;
    uint8_t  len;      // body length in cells
    uint8_t  band;     // depth slot against the deck
};

const Lane kLanes[] = {
    { 3, -1,  9200u, 2, 0},
    {14,  1, 13600u, 3, 1},
    {24, -1,  6800u, 2, 2},
};
constexpr int kLaneCount = (int)(sizeof(kLanes) / sizeof(kLanes[0]));

void drawSpinners(M5Canvas& c, uint32_t now, const Params& p, uint8_t band) {
    using namespace MenuPigRender;
    for (int i = 0; i < kLaneCount; ++i) {
        const Lane& ln = kLanes[i];
        if (ln.band != band) continue;
        const int hy = p.y + (int)ln.rowOff * kCell;
        if (hy < p.y || hy + kCell > p.y + p.h) continue;

        const int travel = p.w + 2 * kMargin;
        const uint32_t ph = (now + (uint32_t)i * 2300u) % ln.periodMs;
        const int along = (int)((uint32_t)travel * ph / ln.periodMs);
        const int hx = snapX((ln.dir > 0) ? (p.x - kMargin + along)
                                          : (p.x + p.w + kMargin - along));

        // Body, a nose lamp facing travel, and a strobe on the tail. At two or
        // three cells the strobe is most of what the eye resolves, which is
        // how a distant aircraft reads at night anyway.
        const uint16_t hull = (band == 0) ? RP::SHADOW_C : RP::WALL_MID;
        for (int s = 0; s < (int)ln.len; ++s) {
            const int px = hx - ln.dir * s * kCell;
            if (px < p.x || px + kCell > p.x + p.w) continue;
            if (!skyWritable(c, p, px, hy)) continue;
            fastFillBlock4(c, px, hy, applyTint(hull, p));
            cover(p, px, hy);
        }
        const int nose = hx + ln.dir * kCell;
        if (nose >= p.x && nose + kCell <= p.x + p.w &&
            skyWritable(c, p, nose, hy)) {
            fastFillBlock4(c, nose, hy, applyTint(RP::WARM, p));
            cover(p, nose, hy);
        }
        const int tail = hx - ln.dir * (int)ln.len * kCell;
        if (((now / 420u + (uint32_t)i) & 1u) &&
            tail >= p.x && tail + kCell <= p.x + p.w &&
            skyWritable(c, p, tail, hy)) {
            fastFillBlock4(c, tail, hy, applyTint(RP::SPARK, p));
            cover(p, tail, hy);
        }
    }
}

// ==[ AIRSHIP ]==
int shipX(uint32_t now, const Params& p) {
    const uint32_t el = now - ship.startMs;
    const int travel = p.w + 2 * kHullCols * kCell;
    const int along = (int)((uint64_t)travel * el / kShipCrossMs);
    const int x = (ship.dir > 0) ? (p.x - kHullCols * kCell + along)
                                 : (p.x + p.w + kHullCols * kCell - along);
    return snapX(x);
}

void drawAirship(M5Canvas& c, uint32_t now, const Params& p) {
    using namespace MenuPigRender;
    if (!ship.active) return;
    const int hx = shipX(now, p);
    const int hy = p.y + (int)ship.rowOff * kCell;
    if (hy < p.y || hy + kHullRows * kCell > p.y + p.h) return;
    if (hx >= p.x + p.w || hx + kHullCols * kCell <= p.x) return;

    const uint16_t crown = applyTint(RP::DEEP, p);
    const uint16_t body  = applyTint(RP::SHADOW_C, p);
    const uint16_t belly = applyTint(
        Display::lerpColor565(RP::SHADOW_C, RP::WARM, 0.55f), p);
    // The panel and the lamps are only allowed onto hull the hull pass
    // actually laid down. Without that test, a hull cell skipped because the
    // shack was behind it still gets an advertisement painted on the shack —
    // and the test has to be coverage, not colour, because the hull's own
    // crown value IS RP::DEEP and so is that shack's doorway.

    for (int row = 0; row < kHullRows; ++row) {
        const int py = hy + row * kCell;
        const uint16_t ink = (row == 0) ? crown
                           : (row == kHullRows - 1) ? belly : body;
        for (int col = (int)kHullL[row]; col <= (int)kHullR[row]; ++col) {
            const int px = hx + col * kCell;
            if (px < p.x || px + kCell > p.x + p.w) continue;
            if (!skyWritable(c, p, px, py)) continue;
            fastFillBlock4(c, px, py, ink);
            cover(p, px, py);
        }
    }

    // Tail fins, at the trailing end whichever way it is heading.
    {
        const int finCol = (ship.dir > 0) ? 1 : kHullCols - 2;
        const int fx = hx + finCol * kCell;
        if (fx >= p.x && fx + kCell <= p.x + p.w) {
            const int upper = hy - kCell;
            const int lower = hy + kHullRows * kCell;
            if (upper >= p.y && skyWritable(c, p, fx, upper))
                fastFillBlock4(c, fx, upper, body);
            if (lower + kCell <= p.y + p.h &&
                skyWritable(c, p, fx, lower))
                fastFillBlock4(c, fx, lower, body);
        }
    }

    // ==[ HOLO PANEL ]== the ad. Two rows on the flank running a scrolling
    // pattern: the only saturated thing in the sky, and what makes the
    // silhouette read as advertising rather than as a whale.
    {
        const uint32_t step = now / 190u;
        for (int row = 1; row <= 2; ++row) {
            const int py = hy + row * kCell;
            for (int col = 5; col <= 12; ++col) {
                const int px = hx + col * kCell;
                if (px < p.x || px + kCell > p.x + p.w) continue;
                if (!isCovered(p, px, py)) continue;
                const uint32_t cellPhase = (uint32_t)(col + row * 3) + step;
                const uint32_t g = wallHash((int)(cellPhase % 23u), row,
                                            (uint32_t)ship.seed ^ 0x2C71u);
                if ((g & 0xFFu) < 90u) continue;
                fastFillBlock4(c, px, py,
                               ((g >> 9) & 3u) == 0u ? RP::VEND : RP::NEON);
            }
        }
    }

    // Nose lamp on the hull, anti-collision strobe on the spine above it.
    {
        const int noseCol = (ship.dir > 0) ? kHullR[2] : kHullL[2];
        const int nx = hx + noseCol * kCell;
        const int ny = hy + 2 * kCell;
        if (nx >= p.x && nx + kCell <= p.x + p.w &&
            isCovered(p, nx, ny))
            fastFillBlock4(c, nx, ny, RP::LED);
        const int sx = hx + (kHullCols / 2) * kCell;
        const int sy = hy - kCell;
        if ((now % 1600u) < 130u && sx >= p.x && sx + kCell <= p.x + p.w &&
            sy >= p.y && skyWritable(c, p, sx, sy))
            fastFillBlock4(c, sx, sy, RP::SPARK);
    }
}

// The beam burns through the middle of the crossing, so it has an arrival and
// a departure instead of being on for the whole pass. Sweep is a slow triangle
// across the ship's own track.
bool beamGeometry(uint32_t now, const Params& p, int groundY,
                  int& originX, int& originY, int& groundX, uint8_t& strength) {
    if (!ship.active) return false;
    const uint32_t el = now - ship.startMs;
    const uint32_t on0 = kShipCrossMs / 4u;
    const uint32_t on1 = (kShipCrossMs * 3u) / 4u;
    if (el < on0 || el >= on1) return false;

    const uint32_t span = on1 - on0;
    const uint32_t t = el - on0;
    // Fade in and out over the first and last eighth so it never snaps on.
    uint32_t env = 255u;
    const uint32_t edge = span / 8u;
    if (edge > 0u) {
        if (t < edge) env = t * 255u / edge;
        else if (t > span - edge) env = (span - t) * 255u / edge;
    }
    strength = (uint8_t)env;

    originX = snapX(shipX(now, p) + (kHullCols / 2) * kCell);
    originY = p.y + (int)ship.rowOff * kCell + kHullRows * kCell;
    if (originY >= groundY) return false;

    const uint32_t sweepPeriod = 7000u;
    const uint32_t sp = now % sweepPeriod;
    const int half = (int)(sweepPeriod / 2u);
    const int off = (sp < (uint32_t)half)
        ? (int)((uint32_t)sp * 80u / (uint32_t)half) - 40
        : 40 - (int)((sp - (uint32_t)half) * 80u / (uint32_t)half);
    groundX = snapX(originX + off);
    return true;
}

void drawBeam(M5Canvas& c, uint32_t now, const Params& p, int groundY) {
    using namespace MenuPigRender;
    int ox = 0, oy = 0, gx = 0;
    uint8_t env = 0;
    beamOn = beamGeometry(now, p, groundY, ox, oy, gx, env);
    if (!beamOn) { beamStrength = 0; return; }
    beamGroundX = gx;
    beamGroundY = groundY;
    beamStrength = env;

    const int rows = (groundY - oy) / kCell;
    if (rows <= 0) { beamOn = false; beamStrength = 0; return; }
    for (int r = 0; r <= rows; ++r) {
        const int py = oy + r * kCell;
        if (py < p.y || py >= groundY) continue;
        const int t8 = r * 255 / rows;
        // The axis walks from the gondola to the impact point and the cone
        // opens as it goes, so the shaft is brightest where it leaves the hull
        // rather than uniformly lit down its whole length.
        const int axis = snapX(ox + (gx - ox) * t8 / 255);
        const int halfCells = 1 + (t8 * 4) / 255;
        // Not named `cover`: that is the coverage-mask writer, and shadowing it
        // inside a draw loop is exactly how a later edit silently stops
        // marking cells.
        const int density = ((190 - t8 / 2) * (int)env) / 255;
        for (int k = -halfCells; k <= halfCells; ++k) {
            const int px = axis + k * kCell;
            if (px < p.x || px + kCell > p.x + p.w) continue;
            const int ak = k < 0 ? -k : k;
            const int fall = 255 - (ak * 255) / (halfCells + 1);
            const uint8_t th = (uint8_t)((density * fall) / 255);
            if ((wallHash(px, py, 0x6D31u) & 0xFFu) >= th) continue;
            const uint16_t base = fastReadPx(c, px, py);
            fastFillBlock4(c, px, py,
                           screenBlend565(base, RP::SHAFT,
                                          (uint8_t)((70 * (int)env) / 255)));
        }
    }
}

} // namespace

// ==[ PUBLIC ]==

void clear() {
    ship.active = false;
    ship.nextMs = 0;
    // Drop the clock too: the next update() reschedules off the new one, which
    // is what stops a cleared module from either firing instantly or never
    // firing again.
    clockValid = false;
    beamOn = false;
    beamStrength = 0;
}

void setScene(uint32_t sceneKey) {
    if (sceneKey == sceneKeyCur) return;
    sceneKeyCur = sceneKey;
    clear();
}

void update(uint32_t now) {
    if (!clockValid) {
        clockValid = true;
        // Never open on a blimp. The first crossing has to be something the
        // player arrives at, not the establishing shot.
        ship.nextMs = now + kShipIdleMinMs;
        return;
    }
    if (ship.active) {
        if (now - ship.startMs >= kShipCrossMs) {
            ship.active = false;
            ship.nextMs = now + kShipIdleMinMs +
                (MenuPigRender::wallHash((int)(now >> 4), 7, 0x51C3u) %
                 kShipIdleSpanMs);
        }
        return;
    }
    if ((int32_t)(now - ship.nextMs) >= 0) {
        ship.active = true;
        ship.startMs = now;
        const uint32_t h = MenuPigRender::wallHash((int)(now >> 5), 3, 0x77A1u);
        ship.dir = (h & 1u) ? 1 : -1;
        ship.rowOff = (int16_t)(1u + (h >> 3) % 4u);
        ship.seed = (uint16_t)(h >> 7);
    }
}

void drawSky(M5Canvas& canvas, uint32_t now, const Params& p) {
    if (p.w < kCell || p.h < kCell) return;
    clearCoverage();

    // Depth order. Far traffic sits behind its own band's cloud; near traffic
    // crosses in front of everything except the airship, which owns the sky.
    drawSpinners(canvas, now, p, 0);
    drawDeck(canvas, now, p, 0);
    drawDeck(canvas, now, p, 1);
    drawSpinners(canvas, now, p, 1);
    drawAirship(canvas, now, p);
    drawDeck(canvas, now, p, 2);
    drawSpinners(canvas, now, p, 2);
    // The beam runs to the roof, not to the bottom of the cloud viewport.
    drawBeam(canvas, now, p, p.groundY > p.y ? p.groundY : p.y + p.h);
}

void drawGroundPool(M5Canvas& canvas, uint32_t now, const Params& p,
                    int groundY) {
    using namespace MenuPigRender;
    (void)now;
    (void)p;
    if (!beamOn || beamStrength == 0u) return;
    const int cx = beamGroundX;
    const int gy = groundY & ~(kCell - 1);
    // A flat ellipse: the beam meets the roof at a shallow angle, so the
    // footprint is wide and only two cells deep.
    for (int row = 0; row < 2; ++row) {
        const int py = gy - row * kCell;
        const int half = (row == 0) ? 6 : 4;
        for (int k = -half; k <= half; ++k) {
            const int px = cx + k * kCell;
            if (px < 0 || px + kCell > UIMeasurements::kScreenWidth) continue;
            const int ak = k < 0 ? -k : k;
            const int fall = 255 - (ak * 255) / (half + 1);
            const uint8_t a = (uint8_t)(((uint32_t)fall *
                                         (uint32_t)beamStrength * 96u) /
                                        (255u * 255u));
            if (a == 0u) continue;
            const uint16_t base = fastReadPx(canvas, px, py);
            fastFillBlock4(canvas, px, py, screenBlend565(base, RP::FLUOR, a));
        }
    }
}

bool sampleSearchlight(int& outX, int& outY, uint8_t& outStrength) {
    if (!beamOn) return false;
    outX = beamGroundX;
    outY = beamGroundY;
    outStrength = beamStrength;
    return true;
}

} // namespace SkyFx
