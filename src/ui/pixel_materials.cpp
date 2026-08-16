/**
 * pixel_materials.cpp — surface material implementations
 */
#include "pixel_materials.h"
#include "pixel_primitives.h"
#include "../gfx/gfx.h"
#include "display.h"
#include "menu_pig_internal.h"
#include <math.h>

namespace PixelMat {

using namespace PixelPrim;

using namespace MenuPigRender;
using namespace UIMeasurements::MenuPigLayout;

// ==[ CONCRETE WALL ]==
//
// Poured-in-place concrete, and wear that behaves the way concrete wear
// behaves under a light.
//
// Polarity is the whole point of this pass. Damage on a wall is geometry, not
// "a brighter pixel": a crack is a recess, so its core is DARKER than the
// field and only the lip that faces the room's practicals catches anything; a
// spall is a bite out of the surface with brighter exposed aggregate sitting
// inside it; a water stain is WET, and wet concrete is darker than dry.
//
// The previous version had the polarity backwards on every layer that showed
// at all, and two layers that could not show. Its grit pass and its stain
// drips both drew in RP::WALL_FAR — the base fill colour — so they wrote the
// wall onto itself and rendered literally nothing, and its cracks drew in
// WALL_NEAR: after STRUCT the brightest value in the room, which reads as
// glowing scratches rather than damage.
//
// CONTRAST BUDGET (this is why that happened, and it constrains everything
// below). The non-emissive ladder is DEEP .10, SHADOW_E .12, SHADOW_C .18,
// WALL_FAR .20, FILL .22, WALL_MID .38, WALL_NEAR .55. Against a WALL_FAR
// field, SHADOW_C and FILL are two hundredths away and can never read at any
// coverage. Wall wear therefore has exactly two usable dark values (DEEP,
// SHADOW_E) and two usable bright ones (WALL_MID, WALL_NEAR). Do not
// reintroduce SHADOW_C or FILL here expecting to see them.
//
// The field is depth-graded on the same grammar as the Room 4 brick: value
// falls off toward the ceiling, because every practical in these rooms sits at
// or below eye level and nothing up there is lit. Bands are quantised so this
// stays fat-pixel masonry instead of smearing into a photographic gradient,
// and every wear layer is graded with the field so damage sinks into the dark
// with the wall it lives on rather than floating in front of it.
//
// `variant` reseeds every layer. Rooms 0, 1 and 2 call this with an identical
// rect; without it they render three pixel-identical walls.
void drawConcreteWall4(M5Canvas& c, int x, int y, int w, int h,
                       uint32_t variant) {
    x = q4(x); y = q4(y); w = (w + 3) & ~3; h = (h + 3) & ~3;
    if (w < kRoomPX || h < kRoomPX) return;
    const uint32_t vs = 0x9E3779B9u * (variant + 1u);

    // ==[ Field ]== quantised vertical grade, 5 authored bands. The floor of
    // the ramp is deliberately well off zero: crush the top band to DEEP and
    // the wear that lives up there loses its contrast budget and vanishes.
    auto lit8 = [&](int ly) -> uint8_t {
        int down = ((ly - y) * 255) / h;        // 0 ceiling -> 255 floor line
        int v = 96 + (down * 159) / 255;        // 96..255
        return (uint8_t)((v / 32) * 32);        // 96/128/160/192/224
    };
    auto graded = [&](int ly, uint16_t col) -> uint16_t {
        return lerpColor565_8(RP::DEEP, col, lit8(ly));
    };
    for (int ly = y; ly < y + h; ly += kRoomPX)
        c.fillRect(x, ly, w, kRoomPX, graded(ly, RP::WALL_FAR));

    // ==[ Form ties ]== the pour's own geometry. Snap-tie plugs sit on the
    // formwork grid, and that regularity is what separates a poured wall from
    // a noise field — it is the one layer here that is deliberately not
    // random. The plug stands proud of the surface, so it only takes a
    // highlight low down where floor bounce can actually reach it.
    for (int ty = y + 3 * kRoomPX; ty + kRoomPX <= y + h - kRoomPX; ty += 32) {
        for (int tx = x + 3 * kRoomPX; tx + kRoomPX <= x + w; tx += 24) {
            if ((wallHash(tx, ty, vs ^ 0x71C1u) & 0xFFu) < 56u) continue;
            c.fillRect(tx, ty, kRoomPX, kRoomPX, RP::SHADOW_E);
            if (lit8(ty) >= 192 && ty + 2 * kRoomPX <= y + h)
                c.fillRect(tx, ty + kRoomPX, kRoomPX, kRoomPX,
                           graded(ty, RP::WALL_MID));
        }
    }

    // ==[ Cold joint ]== the seam where one pour met the next, and the
    // efflorescence that always blooms out of it. Salt bloom is the only wear
    // on this wall that is honestly brighter than the field it sits on.
    {
        int jy = (y + (h * 2) / 5) & ~3;
        if (jy >= y + kRoomPX && jy + 3 * kRoomPX <= y + h) {
            for (int jx = x; jx + kRoomPX <= x + w; jx += kRoomPX) {
                uint32_t jh = wallHash(jx, jy, vs ^ 0x4A11u);
                if ((jh & 0xFFu) < 24u) continue;          // joint breaks up
                c.fillRect(jx, jy, kRoomPX, kRoomPX, RP::SHADOW_E);
                if (((jh >> 8) & 0xFFu) < 86u)
                    c.fillRect(jx, jy + kRoomPX, kRoomPX, kRoomPX,
                               graded(jy, RP::WALL_MID));
                if (((jh >> 16) & 0xFFu) < 30u)
                    c.fillRect(jx, jy + 2 * kRoomPX, kRoomPX, kRoomPX,
                               graded(jy, RP::WALL_MID));
            }
        }
    }

    // ==[ Water stains ]== ceiling leaks running down the face. The track
    // widens as it picks up volume and darkens where it soaks in.
    {
        const int drips = (w >= 200) ? 3 : (w >= 96 ? 2 : 1);
        const int span = w - 6 * kRoomPX;
        for (int d = 0; d < drips && span >= kRoomPX; ++d) {
            uint32_t ds = wallHash(d, 0, vs ^ 0xBC71u);
            int px = (x + 3 * kRoomPX + (int)(ds % (uint32_t)span)) & ~3;
            int len = h / 3 + (int)((ds >> 8) % (uint32_t)max(kRoomPX, h / 3));
            for (int dy = 0; dy < len; dy += kRoomPX) {
                int py = y + kRoomPX + dy;
                if (py + kRoomPX > y + h) break;
                uint32_t ph = wallHash(px, py,
                                       vs ^ (0x91C1u + (uint32_t)d * 0x3331u));
                uint32_t drift = (ph >> 12) & 0x7u;
                if (drift == 0u) px += kRoomPX;
                else if (drift == 1u) px -= kRoomPX;
                if (px < x) px = x;
                if (px + kRoomPX > x + w) px = (x + w - kRoomPX) & ~3;
                if ((ph & 0xFFu) < 56u) continue;           // broken track
                int pw = (dy * 2 > len && ((ph >> 4) & 0x3u) != 0u)
                    ? 2 * kRoomPX : kRoomPX;
                if (px + pw > x + w) pw = kRoomPX;
                c.fillRect(px, py, pw, kRoomPX, graded(py, RP::SHADOW_E));
            }
        }
    }

    // ==[ Spalls ]== a bite out of the surface: dark void, brighter exposed
    // aggregate along its lower lip where the up-light rakes across the break,
    // and rebar showing through the deep ones.
    for (int s = 0; s < 3; ++s) {
        uint32_t cs = wallHash(s, 77, vs ^ 0xF503u);
        if (w < 8 * kRoomPX || h < 8 * kRoomPX) break;
        int cx = (x + 2 * kRoomPX +
                  (int)(cs % (uint32_t)(w - 6 * kRoomPX))) & ~3;
        int cy = (y + 2 * kRoomPX +
                  (int)((cs >> 9) % (uint32_t)(h - 6 * kRoomPX))) & ~3;
        int deepest = 0;
        for (int py = 0; py < 4 * kRoomPX; py += kRoomPX) {
            for (int px = 0; px < 4 * kRoomPX; px += kRoomPX) {
                uint32_t ph = wallHash(px + s * 17, py + s * 13, vs ^ 0x33C1u);
                if ((ph & 0xFFu) >= 110u) continue;         // organic outline
                int fx = cx + px, fy = cy + py;
                if (fx + kRoomPX > x + w || fy + kRoomPX > y + h) continue;
                c.fillRect(fx, fy, kRoomPX, kRoomPX, RP::SHADOW_E);
                if (py > deepest) deepest = py;
                // Broken lower lip catches the room; the void above does not.
                if (((ph >> 8) & 0xFFu) < 100u &&
                    fy + 2 * kRoomPX <= y + h)
                    c.fillRect(fx, fy + kRoomPX, kRoomPX, kRoomPX,
                               graded(fy, RP::WALL_MID));
            }
        }
        // Rebar: one exposed bar, only in the spall that broke deepest.
        if (deepest >= 3 * kRoomPX && cy + 4 * kRoomPX <= y + h) {
            int rx = cx + 2 * kRoomPX;
            if (rx + kRoomPX <= x + w)
                c.fillRect(rx, cy + kRoomPX, kRoomPX, 3 * kRoomPX,
                           graded(cy, RP::WALL_MID));
        }
    }

    // ==[ Cracks ]== a recess and its lit lip. The core is the darkest value
    // on the wall; the lip is one cell to the side and one band brighter, so
    // the pair reads as relief. Only some steps carry a lip — a crack lit
    // evenly down its whole length reads as a drawn line, not a break.
    for (int k = 0; k < 2; ++k) {
        if (w < 12 * kRoomPX || h < 6 * kRoomPX) break;
        uint32_t cs = wallHash(k, 0, vs ^ 0x24F7u);
        int px = (x + 5 * kRoomPX +
                  (int)(cs % (uint32_t)(w - 10 * kRoomPX))) & ~3;
        int py = (y + kRoomPX + (int)((cs >> 9) % (uint32_t)(h / 3))) & ~3;
        int len = 12 + (int)((cs >> 16) % 11u);
        int lipDir = (cs & 0x100u) ? kRoomPX : -kRoomPX;
        for (int i = 0; i < len; ++i) {
            uint32_t step = wallHash(i, k, vs ^ 0x7199u);
            py += kRoomPX;
            px += ((int)(step & 0x3u) - 1) * kRoomPX;
            if (px < x || px + kRoomPX > x + w) break;
            if (py + kRoomPX > y + h) break;
            c.fillRect(px, py, kRoomPX, kRoomPX, RP::DEEP);
            if (((step >> 8) & 0xFFu) < 120u) {
                int lx = px + lipDir;
                if (lx >= x && lx + kRoomPX <= x + w)
                    c.fillRect(lx, py, kRoomPX, kRoomPX,
                               graded(py, RP::WALL_MID));
            }
            if ((step & 0xFFu) < 40u) {                     // branch
                int bx = px - lipDir;
                if (bx >= x && bx + kRoomPX <= x + w)
                    c.fillRect(bx, py, kRoomPX, kRoomPX, RP::DEEP);
            }
        }
    }

    // ==[ Kick plate + rising damp ]== the bottom course is the only part of
    // this wall a floor practical reliably reaches, so it keeps its value.
    // The damp band immediately above it is the reason it reads as a base and
    // not as a stripe.
    {
        int baseY = y + h - kRoomPX;
        for (int gx = x; gx + kRoomPX <= x + w; gx += kRoomPX) {
            if ((wallHash(gx, baseY, vs ^ 0x11B7u) & 0xFFu) < 26u) continue;
            c.fillRect(gx, baseY, kRoomPX, kRoomPX, graded(baseY, RP::WALL_MID));
        }
        for (int dy = kRoomPX; dy <= 3 * kRoomPX; dy += kRoomPX) {
            int py = baseY - dy;
            if (py < y) break;
            uint8_t thresh = (uint8_t)(150 - (dy / kRoomPX) * 44);
            for (int gx = x; gx + kRoomPX <= x + w; gx += kRoomPX) {
                if ((wallHash(gx, py, vs ^ 0x11B8u) & 0xFFu) >= thresh) continue;
                c.fillRect(gx, py, kRoomPX, kRoomPX, RP::SHADOW_E);
            }
        }
    }
}

// ==[ EXPOSED BRICK ]==
void drawBrickWall4(M5Canvas& c, int x, int y, int w, int h, int parallaxX) {
    x = q4(x + parallaxX); y = q4(y); w = (w + 3) & ~3; h = (h + 3) & ~3;
    for (int ly = y; ly < y + h; ly += 8) {
        int row = (ly - y) / 8;
        int offset = (row & 1) ? 4 : 0;
        for (int lx = x + offset; lx < x + w; lx += 12) {
            if (lx + 8 <= x + w)
                c.fillRect(lx, ly, 8, 4, RP::SHADOW_C);
        }
    }
    // Mortar lines
    for (int ly = y + 4; ly < y + h; ly += 8) {
        for (int lx = x; lx < x + w; lx += kRoomPX) {
            if ((wallHash(lx, ly, 22331) & 0xFF) < 30)
                c.fillRect(lx, ly, kRoomPX, 4, RP::DEEP);
        }
    }
    // Moisture streaks. Positions are fractions of the wall, not the three
    // absolute screen columns they used to be — those only landed inside the
    // masonry when the caller happened to be drawing a full-width wall, and
    // silently fell outside it for every other rect.
    // x already carries parallaxX from the snap above; adding it again here
    // is what made the streaks drift twice as fast as the masonry they run on.
    const int streakPos[] = { w / 8, w * 3 / 8, w * 7 / 10 };
    for (int s = 0; s < 3; s++) {
        int sx = q4(x + streakPos[s]);
        if (sx < x || sx + kRoomPX > x + w) continue;
        int startY = q4(y + 8 + (int)(wallHash(s, 0, 44891) & 0x0F));
        int streakH = 24 + (int)(wallHash(s, 1, 44892) & 0x0F);
        for (int sy = startY; sy < startY + streakH && sy < y + h - 4; sy += kRoomPX) {
            if ((wallHash(sx, sy, 44893 + s) & 0xFF) < 160)
                c.fillRect(sx, sy, kRoomPX, kRoomPX, RP::WALL_FAR);
        }
    }
}

// ==[ METAL FLOOR ]==
//
// Bolted steel deck plate seen almost edge-on, so its architecture has to be
// carried by the vertical divisions — the joints between plates — rather than
// by any surface pattern. Everything here lives on the kFloorY lattice
// (phase 2 mod 4, per the room-lattice contract) and is offset from it in
// whole 4px cells; do not anchor anything on the screen lattice or the plate
// joints will sit two pixels off their own bolts.
//
// The wear is deliberately a traffic story rather than uniform grunge: the
// deck is polished where feet actually cross it, rust blooms out of the wet
// spots, and one plate has lifted at its joint. `variant` reseeds it, because
// four rooms draw this same floor and a shared hash makes them one room.
void drawMetalFloor4(M5Canvas& c, uint32_t variant) {
    const uint32_t vs = 0x9E3779B9u * (variant + 1u);
    // Horizontal dashes (FLOOR_GRID)
    for (int gx = 0; gx < SCREEN_WIDTH; gx += 8) {
        uint32_t dh = wallHash(gx, 777, 54323);
        if ((dh & 0xFFF) < 100) continue;
        c.fillRect(gx, kFloorY, 4, kRoomPX, RP::FLOOR_GRID);
    }
    // Perpendicular ticks every 16px
    for (int gx = 0; gx < SCREEN_WIDTH; gx += 16) {
        c.fillRect(gx + 4, kFloorY - 4, kRoomPX, 4, RP::FLOOR_GRID);
    }
    // Wet substrate stays below emitter brightness; reflections are later casts.
    const uint16_t wet = Display::lerpColor565(RP::DEEP, RP::PUDDLE, 0.42f);
    c.fillRect(4, kFloorY - 4, 4, 4, wet);
    c.fillRect(8, kFloorY - 4, 4, 4, wet);
    c.fillRect(12, kFloorY - 4, 4, 4, wet);
    c.fillRect(SCREEN_WIDTH - 96, kFloorY - 4, 4, 4, wet);
    c.fillRect(SCREEN_WIDTH - 92, kFloorY - 4, 4, 4, wet);
    // Edge grime
    for (int gx = 0; gx < 20; gx += kRoomPX)
        if ((wallHash(gx, 888, 23917) & 0x3) == 0)
            c.fillRect(gx, kFloorY - 4, kRoomPX, kRoomPX, RP::FLOOR_GRIME);
    for (int gx = SCREEN_WIDTH - 20; gx < SCREEN_WIDTH; gx += kRoomPX)
        if ((wallHash(gx, 888, 23917) & 0x3) == 0)
            c.fillRect(gx, kFloorY - 4, kRoomPX, kRoomPX, RP::FLOOR_GRIME);
    // Depth gradient (Bayer dithered FLOOR_GRIME band)
    for (int gy = kFloorY - 8; gy < kFloorY + 8; gy += kRoomPX) {
        for (int gx = 0; gx < SCREEN_WIDTH; gx += kRoomPX) {
            float depth = (float)(gy - kFloorY + 8) / 16.0f;
            uint8_t thresh = (uint8_t)(depth * 60.0f);
            if (Gfx::bayer4[(gy/kRoomPX) & 3][(gx/kRoomPX) & 3] < thresh)
                c.fillRect(gx, gy, kRoomPX, kRoomPX, RP::FLOOR_GRIME);
        }
    }

    // ==[ Plate joints ]== the deck is bolted plate, not a continuous sheet.
    // Each joint is a dark seam with the weld bead proud of it and its bolt
    // heads sitting a cell out to either side. This is the only thing on a
    // near-edge-on floor that can carry a sense of module size.
    const int jointStart = 12 + (int)(vs % 3u) * 16;
    for (int jx = jointStart; jx < SCREEN_WIDTH; jx += 48) {
        c.fillRect(jx, kFloorY - kRoomPX, kRoomPX, 3 * kRoomPX, RP::SHADOW_E);
        c.fillRect(jx, kFloorY - kRoomPX, kRoomPX, kRoomPX, RP::WALL_MID);
        for (int side = -1; side <= 1; side += 2) {
            int bx = jx + side * 2 * kRoomPX;
            if (bx < 0 || bx + kRoomPX > SCREEN_WIDTH) continue;
            if ((wallHash(bx, kFloorY, vs ^ 0x5B11u) & 0xFFu) < 70u) continue;
            c.fillRect(bx, kFloorY, kRoomPX, kRoomPX, RP::WALL_MID);
        }
    }

    // ==[ Traffic polish ]== steel that gets walked on goes bright, and only
    // where it gets walked on. Coverage peaks across the middle of the deck
    // and dies out at the walls, so the room reads as used from one direction.
    for (int gx = 0; gx < SCREEN_WIDTH; gx += kRoomPX) {
        int fromEdge = min(gx, SCREEN_WIDTH - kRoomPX - gx);
        if (fromEdge <= 0) continue;
        int reach = min(255, fromEdge * 255 / 96);
        uint8_t cover = (uint8_t)((reach * reach) / 255 * 118 / 255);
        if ((wallHash(gx, kFloorY - 2, vs ^ 0x2C41u) & 0xFFu) >= cover) continue;
        c.fillRect(gx, kFloorY - kRoomPX, kRoomPX, kRoomPX, RP::WALL_NEAR);
        if ((wallHash(gx, kFloorY + 2, vs ^ 0x2C42u) & 0xFFu) < cover / 2u)
            c.fillRect(gx, kFloorY, kRoomPX, kRoomPX, RP::FLOOR_GRID);
    }

    // ==[ Rust bloom ]== the wet spots eat the plate around themselves. Placed
    // off the authored puddles above, not scattered, so cause reads as cause.
    {
        const int wetX[] = { 4, SCREEN_WIDTH - 96 };
        for (int i = 0; i < 2; ++i) {
            for (int rx = wetX[i] - 2 * kRoomPX;
                 rx <= wetX[i] + 5 * kRoomPX; rx += kRoomPX) {
                if (rx < 0 || rx + kRoomPX > SCREEN_WIDTH) continue;
                if ((wallHash(rx, kFloorY, vs ^ 0x7E31u) & 0xFFu) < 96u)
                    c.fillRect(rx, kFloorY, kRoomPX, kRoomPX, RP::FLOOR_GRIME);
                if ((wallHash(rx, kFloorY - 4, vs ^ 0x7E32u) & 0xFFu) < 60u)
                    c.fillRect(rx, kFloorY - kRoomPX, kRoomPX, kRoomPX,
                               RP::SHADOW_E);
            }
        }
    }

    // ==[ Lifted plate ]== one joint has let go. The raised lip takes a hard
    // highlight along its whole length and throws a hard shadow behind it —
    // this is the single piece of real relief on an otherwise flat deck, so it
    // is authored at one place rather than scattered by a hash.
    {
        int lx = (jointStart + 96) & ~3;
        if (lx + 4 * kRoomPX <= SCREEN_WIDTH) {
            c.fillRect(lx, kFloorY - 2 * kRoomPX, 3 * kRoomPX, kRoomPX,
                       RP::WALL_NEAR);
            c.fillRect(lx, kFloorY - kRoomPX, 3 * kRoomPX, kRoomPX, RP::DEEP);
            c.fillRect(lx + 3 * kRoomPX, kFloorY - 2 * kRoomPX,
                       kRoomPX, 2 * kRoomPX, RP::SHADOW_E);
        }
    }

    // Gutter edge
    for (int gx = 0; gx < SCREEN_WIDTH; gx += 8)
        c.fillRect(gx, kFloorY + 4, 4, kRoomPX, RP::FLOOR_GRIME);
}

// ==[ WET GLASS ]== (window pane interior)
void drawWetGlass4(M5Canvas& c, int x, int y, int w, int h,
                   uint16_t reflectionTint, uint8_t intensity, uint32_t now) {
    int ix = q4(x + kRoomPX);
    int iy = q4(y + kRoomPX);
    int iw = (w - kRoomPX * 2 + 3) & ~3;
    int ih = (h - kRoomPX * 2 + 3) & ~3;
    if (iw < 12 || ih < 12) return;
    if (reflectionTint == 0) reflectionTint = RP::PUDDLE;

    uint16_t waterBody = Display::lerpColor565(RP::SHAFT, RP::BG, 0.18f);
    uint16_t streakCol = Display::lerpColor565(waterBody, reflectionTint, 0.20f);
    uint16_t sparkCol = Display::lerpColor565(RP::SHAFT, RP::FLUOR, 0.18f);

    // Rain streaks (adapted from wardrive technique)
    int streakCount = 5 + (ih / 28);
    if (streakCount > 10) streakCount = 10;
    const uint32_t fallStep = 88u;
    int xSpan = max(kRoomPX, iw);
    int travelH = ih + kRoomPX * 10;

    for (int i = 0; i < streakCount; i++) {
        uint32_t seed = wallHash(i, 91, 0x2A71u);
        uint32_t cycle = 1450u + ((seed >> 7) % 1050u);
        uint32_t local = (now + (seed & 0x7FFu)) % cycle;
        uint32_t phase = local / fallStep;
        uint32_t travelQ8 = local * (uint32_t)travelH * 256u / cycle;
        int baseYQ8 = (iy - kRoomPX * 4) * 256 + (int)travelQ8;
        int rawX = (int)((seed >> 11) % (uint32_t)xSpan);
        int baseX = ix + ((rawX + (int)(phase * (1 + (i & 1u)))) % (xSpan + kRoomPX * 2));
        baseX = q4(baseX);
        int len = 3 + (int)((seed >> 4) & 0x03u);
        int lean = ((seed >> 2) & 1u) ? kRoomPX : -kRoomPX;

        for (int s = 0; s < len; s++) {
            int sx = baseX - (s / 2) * lean;
            int syQ8 = baseYQ8 - s * kRoomPX * 256;
            int syPx = syQ8 >= 0 ? syQ8 / 256 : -((-syQ8 + 255) / 256);
            int sy = q4(syPx);
            if (sx < ix || sx >= ix + iw || sy + kRoomPX < iy || sy >= iy + ih) continue;
            uint16_t segCol = (s == 0) ? sparkCol : streakCol;
            int rawA = 142 - s * 20 + ((s == 0) ? 18 : 0);
            rawA = max(28, min(220, rawA));
            if (s == 0) {
                int fracQ8 = ((syQ8 - sy * 256) * 255) / (kRoomPX << 8);
                uint8_t a0 = (uint8_t)(rawA * (255 - fracQ8) / 255);
                uint8_t a1 = (uint8_t)(rawA * fracQ8 / 255);
                if (sy >= iy) {
                    uint16_t b = Gfx::fastReadPx(c, sx, sy);
                    Gfx::fastFillBlock4(c, sx, sy, Gfx::screenBlend565(b, segCol, a0));
                }
                if (sy + kRoomPX < iy + ih) {
                    uint16_t b = Gfx::fastReadPx(c, sx, sy + kRoomPX);
                    Gfx::fastFillBlock4(c, sx, sy + kRoomPX, Gfx::screenBlend565(b, segCol, a1));
                }
            } else {
                int roundedY = sy + ((((syQ8 - sy * 256) >> 8) >= kRoomPX / 2) ? kRoomPX : 0);
                if (roundedY >= iy && roundedY < iy + ih) {
                    uint16_t b = Gfx::fastReadPx(c, sx, roundedY);
                    Gfx::fastFillBlock4(c, sx, roundedY, Gfx::screenBlend565(b, segCol, (uint8_t)rawA));
                }
            }
        }
    }

    // Stable condensation beads on glass
    int beadCount = 4 + intensity / 32;
    if (beadCount > 8) beadCount = 8;
    int creepMax = max(kRoomPX, min(ih / 3, kRoomPX * 5));
    int startSpan = max(kRoomPX, ih - creepMax - kRoomPX);

    for (int i = 0; i < beadCount; ++i) {
        uint32_t seed = wallHash(i, x ^ y, 0x6A51u);
        int bx;
        if (i < 2) bx = (i == 0) ? ix : ix + iw - kRoomPX;
        else bx = ix + (int)((seed >> 9) % (uint32_t)max(kRoomPX, iw - kRoomPX));
        int by = iy + (int)((seed >> 17) % (uint32_t)startSpan);
        uint32_t cycle = 5200u + (seed & 0x7FFu);
        uint32_t local = (now + ((seed >> 5) & 0xFFFu)) % cycle;
        uint32_t hold = cycle * (55u + ((seed >> 3) & 0x0Fu)) / 100u;
        int creepQ8 = 0;
        if (local > hold) {
            uint32_t tQ8 = (local - hold) * 255u / max(1u, cycle - hold);
            uint32_t easedQ8 = tQ8 * tQ8 / 255u;
            int span = kRoomPX + (int)((seed >> 21) % (uint32_t)creepMax);
            creepQ8 = (int)((uint32_t)span * easedQ8 * 256u / 255u);
        }
        int yQ8 = by * 256 + creepQ8;
        int py = q4(yQ8 >> 8);
        int frac = ((yQ8 - py * 256) * 255) / (kRoomPX << 8);

        uint32_t fadeInEnd = cycle / 10u;
        uint32_t fadeOutStart = cycle * 88u / 100u;
        uint8_t lifeAlpha = 255;
        if (local < fadeInEnd) lifeAlpha = (uint8_t)(local * 255u / max(1u, fadeInEnd));
        else if (local > fadeOutStart) lifeAlpha = (uint8_t)((cycle - local) * 255u / max(1u, cycle - fadeOutStart));

        uint8_t bodyA = (uint8_t)(((34u + intensity / 3u) * lifeAlpha) / 255u);
        // Body
        {
            uint16_t b = Gfx::fastReadPx(c, bx, py);
            Gfx::fastFillBlock4(c, bx, py, Gfx::screenBlend565(b, waterBody, (uint8_t)((int)bodyA * (255 - frac) / 255)));
            b = Gfx::fastReadPx(c, bx, py + kRoomPX);
            Gfx::fastFillBlock4(c, bx, py + kRoomPX, Gfx::screenBlend565(b, waterBody, (uint8_t)((int)bodyA * frac / 255)));
        }
        // Glint
        int glintX = bx + (((seed >> 2) & 1u) ? -kRoomPX : kRoomPX);
        uint8_t glintA = (uint8_t)(((18u + intensity / 5u) * lifeAlpha) / 255u);
        {
            uint16_t b = Gfx::fastReadPx(c, glintX, py);
            Gfx::fastFillBlock4(c, glintX, py, Gfx::screenBlend565(b, sparkCol, (uint8_t)((int)glintA * (255 - frac) / 255)));
            b = Gfx::fastReadPx(c, glintX, py + kRoomPX);
            Gfx::fastFillBlock4(c, glintX, py + kRoomPX, Gfx::screenBlend565(b, sparkCol, (uint8_t)((int)glintA * frac / 255)));
        }
    }

    // Two reflection ribbons (source-colored, slow pulse)
    for (int band = 0; band < 2; ++band) {
        uint32_t seed = wallHash(band, w, 0x6B71u);
        uint32_t phase = (now + (seed & 0x7FFu)) % (3600u + band * 900u);
        uint32_t half = (3600u + band * 900u) / 2u;
        uint8_t pulse = (uint8_t)(phase < half
            ? phase * 72u / max(1u, half)
            : (3600u + band * 900u - phase) * 72u / max(1u, half));
        int rx = ix + iw * (band == 0 ? 2 : 5) / 7;
        int ry = iy + kRoomPX * (2 + band * 3);
        for (int cell = 0; cell < 4; ++cell) {
            if ((wallHash(cell, band, seed) & 0x03u) == 0u) continue;
            int px = q4(rx);
            int py = q4(ry + cell * kRoomPX);
            if (px < ix || px >= ix + iw || py < iy || py >= iy + ih) continue;
            uint16_t b = Gfx::fastReadPx(c, px, py);
            Gfx::fastFillBlock4(c, px, py, Gfx::screenBlend565(b, reflectionTint, pulse));
        }
    }
}

// ==[ DEAD NEON MODULE ]==
void drawDeadNeonModule4(M5Canvas& c, int x, int y, uint32_t seed) {
    x = q4(x); y = q4(y);
    uint16_t col = ((wallHash(x, y, seed) & 0x03u) == 0u) ? RP::D_STRUCT : RP::D_FILL;
    c.fillRect(x, y, kRoomPX, kRoomPX, col);
}

// ==[ CRT STATIC ]==
void drawCRTStatic4(M5Canvas& c, int x, int y, int w, int h, uint32_t now, uint8_t rf) {
    x = q4(x); y = q4(y); w = (w + 3) & ~3; h = (h + 3) & ~3;
    c.fillRect(x, y, w, h, RP::BG);
    uint8_t snowThresh = (uint8_t)(76u + ((uint16_t)rf * 52u) / 255u);
    uint32_t snowStep = 140u - ((uint32_t)rf * 60u) / 255u;
    uint32_t snowSeed = now / snowStep;
    for (int py = y; py < y + h; py += kRoomPX) {
        for (int px = x; px < x + w; px += kRoomPX) {
            uint32_t gh = wallHash(px, py, snowSeed);
            if ((gh & 0xFFu) >= snowThresh) continue;
            uint16_t grain = RP::CRT;
            uint8_t accent = (uint8_t)((gh >> 8) & 0xFFu);
            // RF-driven accents
            if (MenuPig::roomMood.spamActive && accent < 34u) grain = RP::SPARK;
            else if (MenuPig::roomMood.trackerPresent && accent < 24u) grain = RP::NEON;
            c.fillRect(px, py, kRoomPX, kRoomPX, grain);
        }
    }
    // Scanline
    int scanY = q4(y + (int)((float)(now % 2400) / 2400.0f * (float)h));
    if (scanY >= y && scanY + kRoomPX <= y + h)
        c.fillRect(x, scanY, w, kRoomPX, RP::CRT);
    // Interference bar
    uint32_t noiseCycle = 3900u - ((uint32_t)rf * 1800u) / 255u;
    int noiseY = q4(y + (int)((float)(now % noiseCycle) / (float)noiseCycle * (float)(h + 8)) - 4);
    if (noiseY >= y && noiseY + 8 <= y + h)
        c.fillRect(x, noiseY, w, 8, RP::DEEP);
}

// ==[ GRATE / VENT ]==
void drawGrate4(M5Canvas& c, int x, int y, int w, int h) {
    x = q4(x); y = q4(y); w = (w + 3) & ~3; h = (h + 3) & ~3;
    // Frame
    c.fillRect(x, y, w, kRoomPX, RP::D_STRUCT);
    c.fillRect(x, y + h - kRoomPX, w, kRoomPX, RP::D_STRUCT);
    c.fillRect(x, y, kRoomPX, h, RP::D_STRUCT);
    c.fillRect(x + w - kRoomPX, y, kRoomPX, h, RP::D_STRUCT);
    // Interior slats
    for (int gy = y + kRoomPX; gy < y + h - kRoomPX; gy += 8) {
        for (int gx = x + kRoomPX; gx < x + w - kRoomPX; gx += 8) {
            c.fillRect(gx, gy, 4, 4, RP::WALL_MID);
        }
    }
}

void drawSmallVent4(M5Canvas& c, int x, int y) {
    x = q4(x); y = q4(y);
    c.fillRect(x, y, 12, 12, RP::WALL_MID);
    c.fillRect(x + 4, y + 4, 4, 4, RP::BG);
}

// ==[ PIPE / CONDUIT ]==
void drawPipeRun4(M5Canvas& c, int x, int y, int w, bool horizontal, bool withValve) {
    x = q4(x); y = q4(y); w = (w + 3) & ~3;
    // w/2 is only guaranteed even, not a multiple of four, so the valve body
    // and its stem have to be re-snapped or they land two pixels off the run
    // they are bolted to for every second length.
    const int mid = q4(w / 2);
    if (horizontal) {
        c.fillRect(x, y, w, kRoomPX, RP::WALL_MID);
        if (withValve) {
            c.fillRect(x + mid - kRoomPX, y - kRoomPX, 12, 12, RP::WALL_NEAR);
            c.fillRect(x + mid, y - 2 * kRoomPX, kRoomPX, kRoomPX, RP::WALL_MID);
        }
        // Rust weeping off the joint. SHADOW_C sits two hundredths from
        // WALL_FAR, so on a concrete wall it was never visible; SHADOW_E is.
        c.fillRect(x + 8, y + kRoomPX, kRoomPX, 8, RP::SHADOW_E);
    } else {
        c.fillRect(x, y, kRoomPX, w, RP::WALL_MID);
        if (withValve) {
            c.fillRect(x - kRoomPX, y + mid - kRoomPX, 12, 12, RP::WALL_NEAR);
            c.fillRect(x - 2 * kRoomPX, y + mid, kRoomPX, kRoomPX, RP::WALL_MID);
        }
    }
}

void drawConduitRun4(M5Canvas& c, int x, int y, int w) {
    x = q4(x); y = q4(y); w = (w + 3) & ~3;
    c.fillRect(x, y, w, kRoomPX, RP::WALL_MID);
    // Cable separators
    for (int gx = x + 8; gx < x + w; gx += 16)
        c.fillRect(gx, y, kRoomPX, kRoomPX, RP::SHADOW_E);
}

// ==[ CABLE COIL ]==
void drawCableCoil4(M5Canvas& c, int x, int y) {
    x = q4(x); y = q4(y);
    c.fillRect(x, y, 8, 4, RP::WALL_MID);
    c.fillRect(x, y + 4, 4, 4, RP::WALL_MID);
    c.fillRect(x + 4, y + 4, 4, 4, RP::SHADOW_E);
}

// ==[ WALL FIXTURES ]==
//
// Everything below is a fitting screwed to a building element, so it obeys the
// same lattice the wall does: whole 4px cells, offsets in multiples of 4. The
// previous pass drew 2px slots and ±2px offsets on top of a q4'd origin, which
// is both sub-grid and — at one cell across the short axis of a 2px detail on
// a 4px surface — under the minimum readable thickness. Several also drew
// their own detail in the colour of the surface behind them and rendered
// nothing at all; those are called out where they were.

// A box bolted to a wall, under a light, has a top edge that catches and a
// bottom edge that does not. The legacy MenuPigRender props have carried this
// relief for a while; the PixelMat set never did, which is why its fixtures
// read as decals printed on the concrete rather than as objects standing off
// it. Call it straight after the body fill, before any face detail.
static void boxRelief4(M5Canvas& c, int x, int y, int w, int h) {
    if (h < 3 * kRoomPX) return;   // too shallow to hold both edges and a face
    c.fillRect(x, y, w, kRoomPX, RP::WALL_NEAR);
    c.fillRect(x, y + h - kRoomPX, w, kRoomPX, RP::SHADOW_E);
}

// ==[ WALL OUTLET ]==
void drawWallOutlet4(M5Canvas& c, int x, int y) {
    x = q4(x); y = q4(y);
    c.fillRect(x, y, 12, 12, RP::D_STRUCT);                       // faceplate
    boxRelief4(c, x, y, 12, 12);
    c.fillRect(x + kRoomPX, y + kRoomPX, kRoomPX, kRoomPX, RP::BG); // socket
}

// ==[ FUSE BOX ]==
void drawFuseBox4(M5Canvas& c, int x, int y) {
    x = q4(x); y = q4(y);
    c.fillRect(x, y, 16, 20, RP::D_STRUCT);                       // enclosure
    boxRelief4(c, x, y, 16, 20);
    c.fillRect(x + kRoomPX, y + kRoomPX, 8, 12, RP::BG);          // open door
    c.fillRect(x + kRoomPX, y + kRoomPX, kRoomPX, kRoomPX, RP::WALL_MID);
    c.fillRect(x + kRoomPX, y + 3 * kRoomPX, kRoomPX, kRoomPX, RP::WALL_MID);
}

// ==[ SPRINKLER ]==
void drawSprinkler4(M5Canvas& c, int x, int y) {
    x = q4(x); y = q4(y);
    c.fillRect(x, y, kRoomPX, kRoomPX, RP::WALL_MID);             // drop pipe
    c.fillRect(x - kRoomPX, y + kRoomPX, 12, kRoomPX, RP::WALL_MID); // escutcheon
    c.fillRect(x, y + 2 * kRoomPX, kRoomPX, kRoomPX, RP::WALL_NEAR); // bulb
}

// ==[ CEILING STAIN / DRIP ]==
void drawCeilingStain4(M5Canvas& c, int x, int y) {
    x = q4(x); y = q4(y);
    // The soak drew in WALL_FAR, which is the concrete wall's own base fill —
    // it has never been visible in any room that calls it. Water darkens.
    c.fillRect(x, y, 12, kRoomPX, RP::SHADOW_E);                  // soaked patch
    c.fillRect(x + kRoomPX, y + kRoomPX, kRoomPX, kRoomPX, RP::DEEP); // drip point
}

// ==[ WALL POSTER ]==
void drawWallPoster4(M5Canvas& c, int x, int y, int w, int h) {
    x = q4(x); y = q4(y); w = (w + 3) & ~3; h = (h + 3) & ~3;
    c.fillRect(x, y, w, h, RP::D_STRUCT);
    c.fillRect(x + 4, y + 4, w - 8, h - 8, RP::BG);
    // Random lines
    for (int ly = y + 8; ly < y + h - 8; ly += 8) {
        if ((wallHash(ly, 0, 999) & 0xFF) < 100) continue;
        int lw = 8 + (wallHash(ly, 0, 888) & 0x1F) * 4;
        c.fillRect(x + 4, ly, min(lw, w - 8), 4, RP::D_STRUCT);
    }
}

// ==[ WALL CLOCK ]==
void drawWallClock4(M5Canvas& c, int x, int y) {
    x = q4(x); y = q4(y);
    c.fillRect(x, y, 12, 12, RP::D_STRUCT);                       // bezel
    c.fillRect(x + kRoomPX, y + kRoomPX, kRoomPX, kRoomPX, RP::BG); // dial
    // Hands at this size can only be markers; drawing them 2px wide made the
    // clock a smudge. One cell at 12 and one at 3 still reads as a clock face.
    c.fillRect(x + kRoomPX, y, kRoomPX, kRoomPX, RP::WALL_MID);
    c.fillRect(x + 2 * kRoomPX, y + kRoomPX, kRoomPX, kRoomPX, RP::WALL_MID);
}

// ==[ FIRE EXTINGUISHER ]==
void drawFireExtinguisher4(M5Canvas& c, int x, int y) {
    x = q4(x); y = q4(y);
    c.fillRect(x, y, 8, 16, RP::D_STRUCT);                        // body
    c.fillRect(x, y - kRoomPX, kRoomPX, kRoomPX, RP::WALL_NEAR);  // valve
    c.fillRect(x + kRoomPX, y - kRoomPX, kRoomPX, kRoomPX, RP::WALL_MID); // horn
    c.fillRect(x, y + kRoomPX, kRoomPX, kRoomPX, RP::SPARK);      // gauge/label
    c.fillRect(x + 8, y + 2 * kRoomPX, kRoomPX, kRoomPX, RP::WALL_MID); // bracket
}

// ==[ FLOOR DRAIN ]==
void drawFloorDrain4(M5Canvas& c, int x, int y) {
    x = q4(x); y = q4(y);
    c.fillRect(x, y, 12, 4, RP::D_STRUCT);
    c.fillRect(x + 4, y - 4, 4, 4, RP::SHADOW_C);
}

// ==[ FLOOR BOTTLE ]==
void drawFloorBottle4(M5Canvas& c, int x, int y) {
    x = q4(x); y = q4(y);
    c.fillRect(x, y - 2 * kRoomPX, kRoomPX, kRoomPX, RP::D_WALL_NEAR); // neck
    c.fillRect(x, y - kRoomPX, kRoomPX, 8, RP::SHAFT);                 // liquid
    c.fillRect(x - kRoomPX, y + kRoomPX, 12, kRoomPX, RP::D_STRUCT);   // base
}

// ==[ ASHTRAY ]==
void drawAshtray4(M5Canvas& c, int x, int y) {
    x = q4(x); y = q4(y);
    c.fillRect(x, y, 12, kRoomPX, RP::D_STRUCT);                       // dish
    c.fillRect(x, y - kRoomPX, kRoomPX, kRoomPX, RP::WALL_MID);        // ash
    c.fillRect(x + kRoomPX, y - kRoomPX, kRoomPX, kRoomPX, RP::D_STRUCT); // cig
    // The ember sits where Room 1's blinking overlay already fires, so the two
    // reinforce one coal instead of lighting two of them a cell apart.
    c.fillRect(x + 2 * kRoomPX, y - kRoomPX, kRoomPX, kRoomPX, RP::WARM);
}

// ==[ AC UNIT ]==
void drawACUnit4(M5Canvas& c, int x, int y) {
    x = q4(x); y = q4(y);
    c.fillRect(x, y, 20, 12, RP::D_STRUCT);
    boxRelief4(c, x, y, 20, 12);
    c.fillRect(x + kRoomPX, y + kRoomPX, 12, kRoomPX, RP::WALL_MID);   // louvres
    c.fillRect(x + kRoomPX, y - kRoomPX, kRoomPX, kRoomPX, RP::WALL_NEAR); // drip
}

// ==[ SERVICE CASE ]==
void drawServiceCase4(M5Canvas& c, int x, int y) {
    x = q4(x); y = q4(y);
    c.fillRect(x, y, 16, 16, RP::D_STRUCT);
    boxRelief4(c, x, y, 16, 16);
    c.fillRect(x + kRoomPX, y + kRoomPX, 8, 8, RP::BG);
    c.fillRect(x + kRoomPX, y + kRoomPX, kRoomPX, kRoomPX, RP::LED);
}

// ==[ PATCH PANEL ]==
void drawPatchPanel4(M5Canvas& c, int x, int y) {
    x = q4(x); y = q4(y);
    c.fillRect(x, y, 16, 16, RP::D_STRUCT);
    boxRelief4(c, x, y, 16, 16);
    c.fillRect(x + kRoomPX, y + kRoomPX, 8, 8, RP::BG);
    // Four ports, not all patched — a panel with every port lit reads as a
    // solid green block at this size.
    for (int i = 0; i < 4; i++) {
        bool live = ((wallHash(x, y, 0x5011u) >> (i * 3)) & 1u) != 0u;
        c.fillRect(x + kRoomPX + (i & 1) * kRoomPX,
                   y + kRoomPX + (i >> 1) * kRoomPX,
                   kRoomPX, kRoomPX, live ? RP::GREEN_DK : RP::SHADOW_E);
    }
}

// ==[ MENU BOARD ]==
void drawMenuBoard4(M5Canvas& c, int x, int y) {
    x = q4(x); y = q4(y);
    c.fillRect(x, y, 24, 16, RP::D_STRUCT);
    boxRelief4(c, x, y, 24, 16);
    c.fillRect(x + kRoomPX, y + kRoomPX, 16, 8, RP::BG);
    // The flicker line used to draw BG onto a BG face: it has never rendered.
    // A menu board that reads as lit needs its line in a value the face is not.
    int lineIdx = (int)((millis() / 2500u) % 2u);
    c.fillRect(x + kRoomPX, y + kRoomPX + lineIdx * kRoomPX, 16, kRoomPX,
               RP::D_WARM);
}

// ==[ NEON ARROW ]==
void drawNeonArrow4(M5Canvas& c, uint32_t now, int x, int y) {
    x = q4(x); y = q4(y);
    if ((now / 500) & 1) {
        c.fillRect(x + 4, y, 4, 4, RP::NEON);
        c.fillRect(x + 2, y + 4, 8, 4, RP::NEON);
        c.fillRect(x, y + 8, 12, 4, RP::NEON);
    }
}

} // namespace PixelMat
