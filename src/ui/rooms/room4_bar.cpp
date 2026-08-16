/** room4_bar.cpp — Underground Bar (CRT terminal, THE PEN neon sign, corner booth) */

#include "../menu_pig_internal.h"
#include "../pixel_materials.h"
#include "../pixel_furniture.h"
#include "../smoke_volume.h"
#include "../npc/barman.h"

namespace MenuPig {

using namespace PixelMat;

static constexpr uint32_t kBarTermCRTCondensationSalt = 0xB401u;
static constexpr int kBarTermCRTCondensationCount = 1;
// THE PEN neon sign — wall-mounted tilted 5x7 glyphs, per-char flicker
// Glyphs: T H E (gap) P E N — 5 columns x 7 rows each, kRoomPX=4
static const uint8_t kPenGlyphs[6][7] = {
    {0b11111, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100},  // T
    {0b10001, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001},  // H
    {0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b11111},  // E
    {0b11110, 0b10001, 0b10001, 0b11110, 0b10000, 0b10000, 0b10000},  // P
    {0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b11111},  // E
    {0b10001, 0b11001, 0b10101, 0b10011, 0b10001, 0b10001, 0b10001},  // N
};
// X offsets per char from text origin (word gap between THE and PEN)
static const int kPenCharX[6] = { 0, 24, 48, 76, 100, 124 };

static bool isPenCharLit(uint32_t now, int ch) {
    bool charDim = ((wallHash(ch, 0, now / 200) & 0xFFu) < 20u);
    return isNeonOn(now) && !charDim;
}

static uint8_t penNeonEnergy8(uint32_t now) {
    if (isNeonOn(now)) return 255;

    const uint32_t phase = (now - neonCycleStart) % NEON_CYCLE_MS;
    uint32_t offAge = 0;
    if (phase >= 2600u) offAge = phase - 2600u;
    else if (phase >= 1060u) offAge = phase - 1060u;
    else if (phase >= 800u) offAge = phase - 800u;

    // A short gas-tube afterglow prevents key-direction chatter on the two
    // brief flickers; the long power-off beat still hands ownership to CRT.
    static constexpr uint32_t kAfterglowMs = 80u;
    if (offAge >= kAfterglowMs) return 0;
    return (uint8_t)(((kAfterglowMs - offAge) * 160u) / kAfterglowMs);
}

static uint64_t samplePenSignColumnMask(uint32_t now, int sx) {
    uint64_t mask = 0;
    for (int ch = 0; ch < 6; ch++) {
        if (!isPenCharLit(now, ch)) continue;
        int charBaseX = sx + 4 + kPenCharX[ch];
        for (int row = 0; row < 7; row++) {
            uint8_t bits = kPenGlyphs[ch][row];
            int tiltDx = row * 2;
            for (int col = 0; col < 5; col++) {
                if ((bits & (1 << (4 - col))) == 0) continue;
                int px = (charBaseX + col * kRoomPX + tiltDx) & ~3;
                if (px > sx + kR5_NeonW - kRoomPX) continue;
                int cell = (px - sx) / kRoomPX;
                if (cell >= 0 && cell < 64) mask |= (1ULL << cell);
            }
        }
    }
    return mask;
}

// Barman NPC — extracted to src/ui/npc/barman.cpp


// Room 4's practicals, packaged for the smoke passes. THE PEN and the CRT are
// what a plume crossing this room is allowed to catch — the shelf strips are
// too weak to register against them at this screen size.
static SmokeFx::Lighting barSmokeLighting(uint32_t now) {
    SmokeFx::Lighting lit;
    if (isNeonOn(now)) {
        PigLight neon;
        neon.x = (int16_t)(kR5_NeonX + parallaxFar + kR5_NeonW / 2);
        neon.y = (int16_t)(kR5_NeonY + kR5_NeonH / 2);
        neon.tint = RP::NEON;
        lit.add(neon, 150.0f, 150);
    }
    PigLight crt;
    crt.x = (int16_t)(kR5_TermX + parallaxMid + kR5_TermW / 2);
    crt.y = (int16_t)(kR5_TermY + kR5_TermH / 2);
    crt.tint = RP::CRT;
    lit.add(crt, 108.0f, 110);
    return lit;
}

static void drawBarmanSmoke(M5Canvas& canvas, uint32_t now) {
    // The barman smokes on the same Blade Runner beat as Pancetta, offset so
    // the two never inhale together. Drawn post-light so the volumes hang in
    // front of the backbar and pick up THE PEN.
    const int p = kPigPX;
    const int emberX = Barman::smokeEmberX();
    const int emberY = Barman::smokeEmberY();
    // Facing is not exported; the ember's side of the body says it plainly.
    const int dir = (emberX > kR5_BarmanX + parallaxMid + kPigW / 2) ? 1 : -1;
    const SmokeFx::BreathFrame breath = SmokeFx::sampleBreath(now, 2600u);

    SmokeFx::setWisp(SmokeFx::Source::BarmanCig, emberX, emberY - kRoomPX,
                     emberY - kRoomPX - 40, dir, 0xBA51u);
    SmokeFx::ExhaleParams ex;
    ex.x = emberX + dir * 3 * p;
    ex.y = emberY - 2 * p;
    ex.dirX = (int8_t)dir;
    ex.power = 175;   // he barely bothers — a tired, low exhale
    ex.seed = 0xBA51u;
    SmokeFx::driveExhale(SmokeFx::Source::BarmanCig, breath, ex);

    const SmokeFx::Lighting lit = barSmokeLighting(now);
    SmokeFx::drawWisp(canvas, SmokeFx::Source::BarmanCig, now,
                      RP::DUST, RP::SOFT, &lit);
    SmokeFx::draw(canvas, SmokeFx::Source::BarmanCig, RP::DUST, RP::SOFT, &lit);
}

// ==[ ROOM 4 GEOMETRY ]==
// Wall props deliberately live outside THE PEN's protected plate.
static constexpr int kBarPosterX = 72;
static constexpr int kBarPosterY = 28;
static constexpr int kBarPosterW = 16;
static constexpr int kBarPosterH = 20;
static constexpr int kBarFuseX = 92;   // full envelope starts 8px left
static constexpr int kBarFuseY = 52;
static constexpr int kBarSpeakerX = 284;
static constexpr int kBarSpeakerY = 72;
static constexpr int kBarPendantX = 296;

// ==[ BACK BAR ]== the room's architectural anchor.
// The old bay was 104x80 with a 72px-wide Barman parked in front of it, so the
// wall behind the two leads read as a crate barely larger than the pig in it.
// This one runs from the terminal's edge to the near divider and from THE PEN's
// underside to the counter — every pixel the room can legally give it — and is
// built as real joinery (cornice, stemware rail, deep mirror, two lit shelf
// tiers, plinth) so it holds the frame instead of sitting in it.
static constexpr int kBarBackbarX = 72;
static constexpr int kBarBackbarY = kR5_NeonY + kR5_NeonH + kRoomPX;   // 68
static constexpr int kBarBackbarW = 112;
static constexpr int kBarBackbarH = kR5_BarTopY - kBarBackbarY;        // 84
static constexpr int kBarBackbarCols = kBarBackbarW / kRoomPX;         // 28
static constexpr int kBarBackbarRows = kBarBackbarH / kRoomPX;         // 21
// Row plan, top → bottom. Everything in drawBackbar derives from these, so the
// joinery cannot drift out of register with the reflections or the bottles.
static constexpr int kBarCorniceRow = 0;   // + 1 row of lip
static constexpr int kBarStemRow = 2;      // hanging stemware rail
static constexpr int kBarMirrorTopRow = 3;
static constexpr int kBarMirrorRows = 6;   // rows 3..8 — the reflected room
static constexpr int kBarBottleLineRow = 9;   // 2 rows standing on plank A
static constexpr int kBarShelfARow = 11;
static constexpr int kBarShelfBRow = 17;
static constexpr int kBarPlinthRow = kBarBackbarRows - 1;   // 20
// Reflected patrons: 4 figures, 5 cells wide, spaced across the mirror.
static constexpr int kBarReflCount = 4;
static constexpr int kBarReflW = 5;
static constexpr int8_t kBarReflCol[kBarReflCount] = {2, 8, 15, 21};
// One cell of stagger. Four identical heads on one level read as a row of
// bowling pins; alternating them reads as some patrons standing and some on
// stools, which is what a bar looks like.
static constexpr int8_t kBarReflRow[kBarReflCount] = {0, 1, 0, 1};

static constexpr int kKaraokeAlcoveX = 196;
static constexpr int kKaraokeAlcoveY =
    kR5_NeonY + kR5_NeonH + kRoomPX;
static constexpr int kKaraokeAlcoveW = 80;
static constexpr int kKaraokeAlcoveH =
    (kR5_BoothY - 16) - kKaraokeAlcoveY;
static constexpr int kBarNearDividerX = 188;
static constexpr int kBarNearDividerY = kKaraokeAlcoveY;
static constexpr int kBarNearDividerW = 3 * kRoomPX;

// ==[ REAR KARAOKE ]== a pig, first, and a performer second.
// The previous singer failed on both counts: a small square head floating over
// a horizontal loaf, joined by a Bresenham stair that opened a corner-only
// diagonal on the belt beat and read as a decapitation. This rebuild fixes the
// silhouette (big head, floppy ears, long trapezoid snout, round rump, curl)
// and makes detachment structurally impossible — see the neck mass below.
static constexpr int kKaraokeX = 204;
static constexpr int kKaraokeY = 72;
static constexpr int kKaraokeW = 64;   // 16 cells
static constexpr int kKaraokeH = 72;   // 18 cells
// Cell rows the body owns. Everything else is derived from these.
static constexpr int kKarStageRow = 16;
static constexpr int kKarBodyTopRow = 9;
// ==[ AUDIENCE ]== near-black pig silhouettes between us and the stage. Backs
// to camera, rim-lit by the spot behind them. This is what turns a lit stage
// into a room with a crowd in it, and it costs four rows.
static void drawKaraokeAudience(M5Canvas& canvas, uint16_t spot,
                                const RoomLightLoopFrame& loop, int ax) {
    const int p = kRoomPX;
    const int baseY = kKaraokeAlcoveY + kKaraokeAlcoveH - 4 * p;
    static constexpr int8_t kHeadCol[3] = {2, 9, 15};
    const uint16_t rim = Display::screenBlend565(RP::D_STRUCT, spot,
                                                 (uint8_t)(40u + loop.energy / 2u));
    for (int i = 0; i < 3; ++i) {
        // A slow nod, one cell, staggered per head. Any faster and three dark
        // blobs bouncing in a corner pull focus off the singer.
        // Lift, never dip: the resting row already sits on the alcove's bottom
        // edge, and a downward beat would push shoulders out of the recess.
        const int nod = ((loop.phase + (uint32_t)i) & 3u) == 2u ? -p : 0;
        const int hx = ax + kHeadCol[i] * p;
        const int hy = baseY + nod;
        canvas.fillRect(hx, hy, p, p, RP::DEEP);             // ears
        canvas.fillRect(hx + 2 * p, hy, p, p, RP::DEEP);
        canvas.fillRect(hx, hy + p, 3 * p, p, rim);          // rim-lit crown
        canvas.fillRect(hx, hy + 2 * p, 4 * p, p, RP::DEEP); // head + snout
        canvas.fillRect(hx - p, hy + 3 * p, 5 * p, p, RP::DEEP);  // shoulders
    }
}

static uint8_t barEffectiveRfActivity() {
    uint8_t activity = roomMood.rfActivity;
    if (roomMood.trackerPresent && activity < 132u) activity = 132u;
    if (roomMood.spamActive && activity < 196u) activity = 196u;
    return activity;
}

static uint16_t barRfColor() {
    return roomMood.spamActive ? RP::SPARK
        : (roomMood.trackerPresent ? RP::CRT : RP::GREEN_DK);
}

// ==[ REFLECTED PATRONS ]== the rest of the bar, seen in the back bar mirror.
// This is where the room's crowd lives. Putting bodies in the shelf bay (the
// old approach) parked four pigs INSIDE the furniture; putting them in the
// glass puts them behind the camera where drinkers actually sit, costs no
// floor space, fights no station envelope, and is the single most Blade Runner
// shot in the building. Reflections are one value step down from the room and
// cut off at the chest by the bottle line, exactly as real ones are.
static void drawBackbarReflection(M5Canvas& canvas, uint32_t now,
                                  int bbx, int idx) {
    const int p = kRoomPX;
    const int col = kBarReflCol[idx];
    // Rows 4..8 of the bay: heads and shoulders, hidden below by the bottles.
    const int topRow = kBarMirrorTopRow + 1 + kBarReflRow[idx];
    const bool facesRight = (idx & 1) == 0;

    // Slow, whole-cell behaviour on staggered clocks. A mirror full of pigs
    // twitching in sync is worse than a still one.
    const uint32_t beat = (now + (uint32_t)idx * 1700u) % 7400u;
    const bool drinking = (idx == 1 || idx == 3) && beat < 1400u;
    const bool laughing = (idx == 2) && beat > 4200u && beat < 5300u;
    const int bob = laughing && ((now / 260u) & 1u) ? -p : 0;

    auto cell = [&](int c, int r, int wCells, int hCells, uint16_t v) {
        canvas.fillRect(bbx + (col + c) * p, kBarBackbarY + (topRow + r) * p + bob,
                        wCells * p, hCells * p, v);
    };

    // Value ladder, darkest mass to lightest feature. D_FILL is BELOW SHADOW_C
    // (0.40 toward BG), so the shoulders take it and the head sits a step up —
    // the reverse read the head as a hole punched in the torso.
    // Shoulders run to the last row of glass and stop. Letting them spill onto
    // the bottle-line row would leave a reflection hanging outside the mirror
    // wherever the bottle rhythm happens to leave a gap.
    const int glassRows = kBarMirrorTopRow + kBarMirrorRows - topRow;
    cell(0, 1, 5, glassRows - 1, RP::D_FILL);  // shoulder mass, cut by bottles
    cell(1, 1, 3, 3, RP::SHADOW_C);            // head, lifted off the torso
    cell(1, 0, 1, 1, RP::D_FILL);              // ears
    cell(3, 0, 1, 1, RP::D_FILL);
    cell(facesRight ? 4 : 0, 2, 1, 1, RP::D_WALL_NEAR);   // snout
    cell(facesRight ? 3 : 1, 1, 1, 1, RP::DEEP);          // eye
    if (laughing) cell(facesRight ? 4 : 0, 3, 1, 1, RP::DEEP);   // open mouth

    // Glass rides up beside the snout and back down to the bar. One cell, no
    // arm — and never on the snout row, which it would otherwise erase.
    if (idx == 1 || idx == 3)
        cell(facesRight ? 4 : 0, drinking ? 1 : 3, 1, 1, RP::PUDDLE);

    if (idx != 0) return;
    // The near reflection smokes, on its own beat. No ember thread here: the
    // glass gives it barely three cells of headroom, and a 4px wisp needs six.
    // Its exhale alone is enough at this size, and it goes through the same
    // pool as every other cigarette in the game, so it dissipates the same way.
    const SmokeFx::BreathFrame breath = SmokeFx::sampleBreath(now, 4100u);
    const int emberX = bbx + (col + (facesRight ? 5 : -1)) * p;
    const int emberY = kBarBackbarY + (topRow + 2) * p + bob;
    canvas.fillRect(emberX, emberY, p, p,
                    lerpColor565_8(RP::D_WARM, RP::SPARK, breath.emberHeat));
    SmokeFx::ExhaleParams ex;
    ex.x = emberX + (facesRight ? 2 * p : -2 * p);
    ex.y = emberY - p;
    ex.dirX = (int8_t)(facesRight ? 1 : -1);
    ex.power = 150;
    ex.scale = 46;    // a figure this far back gets a proportionally small slug
    ex.seed = 0x7A31u;
    SmokeFx::driveExhale(SmokeFx::Source::PatronCig, breath, ex);
}

static void drawBarRfDeck(M5Canvas& canvas, int tx, int ty, int tw) {
    uint8_t pips = (uint8_t)min(6u,
        ((uint32_t)roomMood.rfActivity + 42u) / 43u);
    for (uint8_t pip = 0; pip < 6u; ++pip) {
        canvas.fillRect(tx + kRoomPX + pip * kRoomPX, ty + kRoomPX,
                        kRoomPX, kRoomPX,
                        pip < pips ? RP::GREEN_DK : RP::D_DEEP);
    }
    uint16_t threat = roomMood.spamActive ? RP::SPARK
        : (roomMood.trackerPresent ? RP::CRT : RP::D_DEEP);
    canvas.fillRect(tx + tw - 3 * kRoomPX, ty + kRoomPX,
                    kRoomPX, kRoomPX, threat);
    canvas.fillRect(tx + tw - 2 * kRoomPX, ty + kRoomPX,
                    kRoomPX, kRoomPX,
                    roomMood.captureCount > 0 ? RP::FLUOR : RP::D_DEEP);
}

void restoreRoom4CategoricalSources(M5Canvas& canvas) {
    int tx = kR5_TermX + parallaxMid;
    drawBarRfDeck(canvas, tx, kR5_TermY, kR5_TermW);
}

// Backbar bottle glass. Low-key liquid, but it follows the room's emitters so
// the shelves belong to the same light as everything else.
static uint16_t barBottleLiquid(uint32_t h) {
    switch (h & 3u) {
        case 0:  return Display::lerpColor565(RP::DEEP, RP::CRT, 0.35f);   // green
        case 1:  return Display::lerpColor565(RP::DEEP, RP::SHAFT, 0.18f); // clear
        default: return Display::lerpColor565(RP::DEEP, RP::WARM, 0.35f);  // amber
    }
}

static void drawBackbar(M5Canvas& canvas, uint32_t now,
                        const RoomLightLoopFrame& loop) {
    const int p = kRoomPX;
    const int bbx = kBarBackbarX + parallaxMid;
    auto cell = [&](int col, int row, int wCells, int hCells, uint16_t c) {
        canvas.fillRect(bbx + col * p, kBarBackbarY + row * p,
                        wCells * p, hCells * p, c);
    };
    const int cols = kBarBackbarCols;

    // ==[ CARCASS ]== one solid mass first, then cut the joinery out of it.
    // Framing it the other way around is what left the old bay reading as an
    // outline with holes in it rather than a piece of furniture.
    cell(0, 0, cols, kBarBackbarRows, RP::D_STRUCT);
    cell(1, 1, cols - 2, kBarBackbarRows - 2, RP::D_DEEP);

    // ==[ CORNICE ]== heavy top plate, lit lip, concealed strip beneath. Most
    // of what makes a unit read as architecture happens in these two rows.
    cell(0, kBarCorniceRow, cols, 1, RP::D_STRUCT);
    cell(1, kBarCorniceRow + 1, cols - 2, 1, RP::WALL_MID);
    const uint16_t stripWarm = Display::screenBlend565(
        RP::WALL_MID, RP::D_WARM, (uint8_t)(110u + loop.energy));
    for (int c = 2; c < cols - 2; c += 2)
        cell(c, kBarCorniceRow + 1, 1, 1, stripWarm);

    // ==[ MIRROR ]== one uncut sheet, full width. Old silvering: dead at the
    // top where the room gives it nothing, lifting toward the bottle line.
    cell(1, kBarMirrorTopRow, cols - 2, 1, RP::DEEP);   // top reveal shadow
    for (int r = 1; r < kBarMirrorRows; ++r) {
        cell(1, kBarMirrorTopRow + r, cols - 2, 1,
             lerpColor565_8(RP::D_DEEP, RP::SHADOW_C,
                            (uint8_t)(16u + (uint32_t)r * 20u)));
    }
    cell(1, kBarMirrorTopRow, 1, kBarMirrorRows, RP::DEEP);          // jamb
    cell(cols - 2, kBarMirrorTopRow, 1, kBarMirrorRows, RP::SHADOW_C);

    for (int i = 0; i < kBarReflCount; ++i)
        drawBackbarReflection(canvas, now, bbx, i);

    // A tube's worth of room signage smeared across the glass surface, so it
    // goes over the reflected room, not behind it. Diagonal on purpose: a
    // straight band reads as a painted stripe, the step is what says "glass".
    if (isNeonOn(now)) {
        for (int r = 1; r < kBarMirrorRows; ++r) {
            const int c = 4 + r * 3;
            if (c + 3 >= cols - 1) break;
            for (int k = 0; k < 3; ++k) {
                const int px = bbx + (c + k) * p;
                const int py = kBarBackbarY + (kBarMirrorTopRow + r) * p;
                uint16_t base = fastReadPx(canvas, px, py);
                canvas.fillRect(px, py, p, p,
                                screenBlend565(base, RP::NEON,
                                               (uint8_t)(26 - r * 3)));
            }
        }
    }

    // Reflected smoke lives inside the glass: clipped hard to the mirror
    // opening, one value step down, and drawn before the stemware and the
    // bottle line so both still occlude it.
    {
        const SmokeFx::Lighting lit = barSmokeLighting(now);
        SmokeFx::ClipBox glass;
        glass.x = bbx + p;
        glass.y = kBarBackbarY + kBarMirrorTopRow * p;
        glass.w = (cols - 2) * p;
        glass.h = kBarMirrorRows * p;
        SmokeFx::draw(canvas, SmokeFx::Source::PatronCig,
                      RP::SHADOW_C, RP::D_WALL_NEAR, &lit, &glass);
    }

    // ==[ STEMWARE ]== hung off the cornice rail, in front of the glass. This
    // replaces the free-floating rack that used to hang across the mirror and
    // guillotine everything behind it.
    cell(1, kBarStemRow, cols - 2, 1, RP::WALL_MID);
    for (int g = 0; g < 5; ++g) {
        const int c = 3 + g * 5;
        cell(c, kBarStemRow, 1, 1, RP::D_WALL_NEAR);          // stem
        cell(c - 1, kBarStemRow + 1, 3, 1, RP::PUDDLE);       // wet rim
    }

    // ==[ BOTTLE LINE ]== short glass standing along the mirror sill. It is
    // what visually cuts the reflections off at the chest.
    for (int c = 1; c < cols - 1; ++c) {
        const uint32_t h = wallHash(c, 0, 0xB07Eu);
        if ((h & 3u) == 0u) continue;
        cell(c, kBarBottleLineRow + 1, 1, 1, barBottleLiquid(h >> 4));
        cell(c, kBarBottleLineRow, 1, 1, RP::D_STRUCT);
    }
    cell(1, kBarShelfARow, cols - 2, 1, RP::D_STRUCT);        // plank A

    // ==[ TALL DISPLAY ]== the good stuff, on the lower plank.
    for (int c = 1; c < cols - 1; ++c) {
        const uint32_t h = wallHash(c, 2, 0xB07Eu);
        if ((h & 7u) < 2u) continue;
        const int ht = 2 + (int)((h >> 3) & 2u);              // 2 or 4 cells
        cell(c, kBarShelfBRow - ht, 1, ht, barBottleLiquid(h >> 6));
        cell(c, kBarShelfBRow - ht, 1, 1, RP::D_STRUCT);      // cap
    }
    cell(1, kBarShelfBRow, cols - 2, 1, RP::D_STRUCT);        // plank B

    // Under-shelf strips. Every real back bar is lit from under its own
    // shelves; it is also the only light this side of the room casts upward.
    const uint16_t shelfGlow = Display::screenBlend565(
        RP::D_DEEP, RP::D_WARM, (uint8_t)(86u + loop.energy));
    for (int c = 2; c < cols - 2; ++c) {
        if ((wallHash(c, 3, 0x5E11u) & 3u) == 0u) continue;
        cell(c, kBarShelfARow + 1, 1, 1, shelfGlow);
        cell(c, kBarShelfBRow + 1, 1, 1, shelfGlow);
    }

    // ==[ PILASTERS ]== only through the shelving. Running them up through the
    // mirror would chop the reflected room into three unrelated pictures.
    cell(9, kBarBottleLineRow, 1, kBarPlinthRow - kBarBottleLineRow, RP::D_STRUCT);
    cell(18, kBarBottleLineRow, 1, kBarPlinthRow - kBarBottleLineRow, RP::D_STRUCT);

    // ==[ BASE ]== crates and a plinth. Mostly behind the Barman, which is why
    // it stays cheap.
    for (int c = 2; c < cols - 2; c += 5)
        cell(c, kBarPlinthRow - 1, 3, 1, RP::SHADOW_C);
    cell(0, kBarPlinthRow, cols, 1, RP::D_STRUCT);
}

static void drawBarDepthArchitecture(M5Canvas& canvas,
                                     const RoomLightLoopFrame& loop,
                                     uint16_t spot, uint32_t now) {
    drawBackbar(canvas, now, loop);

    // Far-plane karaoke alcove. Its deep back, brighter jambs, and visible
    // spotlight source create a clear tunnel behind the booth and counter.
    const int ax = kKaraokeAlcoveX + parallaxFar;
    canvas.fillRect(ax, kKaraokeAlcoveY,
                    kKaraokeAlcoveW, kKaraokeAlcoveH, RP::D_STRUCT);
    canvas.fillRect(ax + kRoomPX, kKaraokeAlcoveY + kRoomPX,
                    kKaraokeAlcoveW - 2 * kRoomPX,
                    kKaraokeAlcoveH - 2 * kRoomPX, RP::D_DEEP);
    canvas.fillRect(ax + 2 * kRoomPX, kKaraokeAlcoveY + 2 * kRoomPX,
                    kKaraokeAlcoveW - 4 * kRoomPX,
                    kRoomPX, RP::SHADOW_C);
    canvas.fillRect(ax + kRoomPX, kKaraokeAlcoveY + 5 * kRoomPX,
                    kRoomPX, kKaraokeAlcoveH - 7 * kRoomPX,
                    RP::WALL_FAR);
    canvas.fillRect(ax + kKaraokeAlcoveW - 2 * kRoomPX,
                    kKaraokeAlcoveY + 5 * kRoomPX,
                    kRoomPX, kKaraokeAlcoveH - 7 * kRoomPX,
                    RP::WALL_FAR);

    const int lampX =
        (ax + kKaraokeAlcoveW / 2 - kRoomPX) & ~(kRoomPX - 1);
    canvas.fillRect(lampX, kKaraokeAlcoveY + kRoomPX,
                    2 * kRoomPX, kRoomPX, RP::D_STRUCT);
    uint16_t lampFace = Display::screenBlend565(
        RP::D_STRUCT, spot, (uint8_t)(72u + loop.energy));
    canvas.fillRect(lampX + kRoomPX,
                    kKaraokeAlcoveY + 2 * kRoomPX,
                    kRoomPX, kRoomPX, lampFace);
}

static void drawBarBoothNearArm(M5Canvas& canvas, int bx, int by) {
    canvas.fillRect(bx, by - 32, 8, 32, RP::SOFT);
    canvas.fillRect(bx + 4, by - 28, 4, 24, RP::FILL);
    canvas.fillRect(bx, by - 32, 8, 4, RP::D_STRUCT);
}

void drawRoom4(M5Canvas& canvas, uint32_t now, RoomRenderPass pass) {
    bool neonOn = isNeonOn(now);
    // Glass stays low-key, but its liquid follows the active room emitters.
    const uint16_t kBottleAmber = Display::lerpColor565(RP::DEEP, RP::WARM, 0.35f);
    const uint16_t kBottleGreen = Display::lerpColor565(RP::DEEP, RP::CRT, 0.35f);
    const uint16_t kBottleClear = Display::lerpColor565(RP::DEEP, RP::SHAFT, 0.18f);

    if (pass == RoomRenderPass::BASE) {
        // Retained underground shell. Everything with a clock stays live.
        canvas.fillRect(0, kRoomY, SCREEN_WIDTH,
                        kFloorY - kRoomY + 8, RP::DEEP);
        // ==[ EXPOSED BRICK ]== depth-graded, far parallax.
        // A single flat SHADOW_C course across the whole wall gave the room no
        // recession at all: the brick behind the bar sat at exactly the value
        // of the brick by the near frame, so the back of the set read as
        // wallpaper pinned two feet behind the pig. The wall now falls off
        // toward the top and toward the centre — where the deep architecture
        // (back bar, stage tunnel) lives — and only keeps its light down at the
        // floor line and out at the side walls, where a practical could plausibly
        // graze it. Bands are quantised so this stays fat-pixel masonry and does
        // not smear into a photographic gradient.
        {
            int wx = 4 + parallaxFar, wy = kRoomY + 4;
            int ww = SCREEN_WIDTH - 8, wh = kFloorY - kRoomY - 8;
            const int cxWall = SCREEN_WIDTH / 2;
            auto nearness8 = [&](int lx, int ly) -> uint8_t {
                int down = ((ly - wy) * 255) / wh;              // 0 top → 255 floor
                int outX = lx + 4 - cxWall;
                if (outX < 0) outX = -outX;
                outX = (outX * 255) / cxWall;                   // 0 centre → 255 edge
                int n = (down * 3 + outX * 2) / 5;
                if (n > 255) n = 255;
                return (uint8_t)((n / 48) * 48);                // 6 authored bands
            };
            for (int ly = wy; ly < wy + wh; ly += 8) {
                for (int lx = wx; lx < wx + ww; lx += 12) {
                    int offset = ((ly / 8) & 1) ? 4 : 0;
                    int bx = (lx + offset) & ~3;
                    if (bx >= wx && bx + 8 <= wx + ww)
                        canvas.fillRect(bx, ly, 8, 4,
                                        lerpColor565_8(RP::DEEP, RP::SHADOW_C,
                                                       nearness8(bx, ly)));
                }
            }
            // Mortar: pushed to full black where the wall recedes, so the
            // courses themselves lose contrast with distance instead of
            // staying crisp all the way to the back of the room.
            for (int ly = wy + 4; ly < wy + wh; ly += 8) {
                for (int lx = wx; lx < wx + ww; lx += kRoomPX) {
                    if ((wallHash(lx, ly, 11171) & 0xFF) >= 80) continue;
                    canvas.fillRect(lx, ly, kRoomPX, 4,
                                    lerpColor565_8(RP::BG, RP::DEEP,
                                                   nearness8(lx, ly)));
                }
            }
            // Damp patches survive only in the near band along the floor.
            for (int ly = wy + wh - 30; ly < wy + wh; ly += kRoomPX) {
                for (int lx = wx + 40; lx < wx + 120; lx += kRoomPX) {
                    if ((wallHash(lx, ly, 22331) & 0xFF) < 30)
                        canvas.fillRect(lx, ly, kRoomPX, kRoomPX,
                                        lerpColor565_8(RP::SHADOW_C,
                                                       RP::WALL_FAR,
                                                       nearness8(lx, ly)));
                }
            }
        }
        // Moisture streaks — leaky basement, vertical drip lines down brick
        {
            const int streakPositions[] = {32, 108, 224};
            for (int s = 0; s < 3; s++) {
                int sx = (streakPositions[s] + parallaxFar) & ~3;
                int startY = kRoomY + 8 +
                    (int)(wallHash(s, 0, 44891) & 0x0F);
                int streakH = 24 +
                    (int)(wallHash(s, 1, 44892) & 0x0F);
                for (int sy = startY;
                     sy < startY + streakH && sy < kFloorY - 4;
                     sy += kRoomPX) {
                    if ((wallHash(sx, sy, 44893 + s) & 0xFF) < 160)
                        canvas.fillRect(sx, sy, kRoomPX, kRoomPX,
                                        RP::WALL_FAR);
                }
            }
        }
        // Dark tile floor with grime
        for (int gy = kFloorY; gy < kFloorY + 8; gy += kRoomPX) {
            for (int gx = 0; gx < SCREEN_WIDTH; gx += kRoomPX) {
                uint16_t col = ((gx / kRoomPX + gy / kRoomPX) & 1)
                    ? RP::FLOOR_GRIME : RP::DEEP;
                canvas.fillRect(gx, gy, kRoomPX, kRoomPX, col);
            }
        }
        for (int gx = 0; gx < 20; gx += kRoomPX)
            if ((wallHash(gx, kFloorY - 2, 33891) & 0xFF) < 60)
                canvas.fillRect(gx, kFloorY - 2,
                                kRoomPX, kRoomPX, RP::FLOOR_GRIME);
        for (int gx = SCREEN_WIDTH - 20; gx < SCREEN_WIDTH; gx += kRoomPX)
            if ((wallHash(gx, kFloorY - 2, 33892) & 0xFF) < 60)
                canvas.fillRect(gx, kFloorY - 2,
                                kRoomPX, kRoomPX, RP::FLOOR_GRIME);
        return;
    }

    // Room 4 owns its structure here. The legacy generated terminal, counter,
    // booth, and sign use older envelopes; underpainting them leaves duplicate
    // edges around the canonical kR5_* furniture and wrecks the depth read.
    // Rear spotlight underlay. The singer itself is composited after the broad
    // 4px wall washes so its fixed mic/face cells cannot be flood-filled away.
    const int karaokeX = kKaraokeX + parallaxFar;
    RoomLightLoopFrame barLoop = sampleRoomLightLoop(4, now);
    uint16_t spot = Display::screenBlend565(RP::NEON, RP::CRT,
                                             (uint8_t)(48u + barLoop.energy));
    drawBarDepthArchitecture(canvas, barLoop, spot, now);
    drawVolumetricDustBeam(canvas, now,
                           kKaraokeAlcoveX + parallaxFar +
                               kKaraokeAlcoveW / 2,
                           kKaraokeAlcoveY + 3 * kRoomPX, 8,
                           karaokeX + kKaraokeW / 2,
                           kKaraokeY + kKaraokeH - kRoomPX, 36,
                           spot, RP::DUST, 0x4B41u, &barLoop);
    // THE PEN's plate is emitted after the room grade. Its coordinates still
    // own every cast/reflection calculation below.
    int nx = kR5_NeonX + parallaxFar, ny = kR5_NeonY;
    int nw = kR5_NeonW, nh = kR5_NeonH;
    // Wall terminal structure. The phosphor face is restored after grading.
    int tx = kR5_TermX + parallaxMid;
    int ty = kR5_TermY, tw = kR5_TermW, th = kR5_TermH;
    {
        canvas.fillRect(tx, ty, tw, th, RP::D_STRUCT);
        canvas.fillRect(tx + 4, ty + 4, tw - 8, th - 8, RP::DEEP);
        // Shelf + drives (LEDs redrawn post-darken)
        canvas.fillRect(tx, ty + th, tw, 4, RP::WALL_MID);
        for (int i = 0; i < 3; i++) {
            int driveX = tx + 4 + i * 16;
            canvas.fillRect(driveX, ty + th + 4, 8, 4, RP::FILL);
        }
        canvas.fillRect(tx + tw / 2, ty + th + 8, kRoomPX, kFloorY - ty - th - 8, RP::WALL_MID);
        canvas.fillRect(tx + tw / 2 - 4, ty + th + 28, 8, 4, RP::D_STRUCT);
    }
    // Stool at terminal
    {
        int sx = kR5_StoolX + parallaxMid, sy = kR5_StoolSeatY;
        int sw = kR5_StoolW, sh = kR5_StoolSeatH;
        canvas.fillRect(sx, sy, sw, sh, RP::D_STRUCT);
        canvas.fillRect(sx + 4, sy + 4, sw - 8, sh - 4, RP::FILL);
        int postX = sx + sw / 2 - 2;
        canvas.fillRect(postX, sy + sh, kRoomPX, kFloorY - sy - sh, RP::WALL_MID);
        int footY = ((sy + sh + (kFloorY - sy - sh) / 2) & ~3);
        canvas.fillRect(sx + 4, footY, sw - 8, kRoomPX, RP::WALL_MID);
        canvas.fillRect(sx - 4, kFloorY - 4, sw + 8, 4, RP::WALL_MID);
    }
    // Environmental clutter (wall-mounted, far parallax). The old mid-wall
    // conduit run at x=72,y=76 now lives entirely behind the back bar, so it is
    // gone rather than buried — nothing renders under 112px of joinery.
    drawConduitRun4(canvas, 4 + parallaxFar, kRoomY + 14, 50);
    drawWallOutlet4(canvas, 56 + parallaxFar, 50);
    drawWallPoster4(canvas,
                   kBarPosterX + parallaxFar, kBarPosterY,
                   kBarPosterW, kBarPosterH);
    drawFuseBox4(canvas,
                kBarFuseX + parallaxFar, kBarFuseY);
    drawSmallVent4(canvas, 56 + parallaxFar, kRoomY + 4);
    drawSprinkler4(canvas, 100 + parallaxFar, kRoomY);
    drawSprinkler4(canvas, 200 + parallaxFar, kRoomY);
    canvas.fillRect(180 + parallaxFar, kRoomY + 8, 80, kRoomPX, RP::WALL_MID);
    canvas.fillRect(220 + parallaxFar, kRoomY + 8,
                    kRoomPX, kRoomPX, RP::WALL_MID);
    drawSmallVent4(canvas, 260 + parallaxFar, kRoomY + 4);
    canvas.fillRect(parallaxFar, kRoomY + 8, 140, kRoomPX, RP::WALL_MID);
    canvas.fillRect(60 + parallaxFar, kRoomY + 8, kRoomPX, 16, RP::WALL_MID);
    canvas.fillRect(120 + parallaxFar, kRoomY + 8,
                    kRoomPX, kRoomPX, RP::WALL_MID);
    drawPopOutline1px(canvas, tx, ty, tw, th + 10,
                      PopOutlineStyle::MIXED, 0xE001u);
    drawPopOutline1px(canvas, kBarPosterX + parallaxFar, kBarPosterY,
                      kBarPosterW, kBarPosterH,
                      PopOutlineStyle::MIXED, 0xE011u);
    // Outlines track the fixture footprints. The fuse box and the outlet both
    // grew when their 2px internals became real cells, and an outline cut for
    // the old size draws its right and bottom edges INSIDE the fixture,
    // punching BG holes through it.
    drawPopOutline1px(canvas, kBarFuseX + parallaxFar, kBarFuseY,
                      16, 20, PopOutlineStyle::SOLID, 0xE021u);
    drawPopOutline1px(canvas, 56 + parallaxFar, 50, 12, 12,
                      PopOutlineStyle::SOLID, 0xE041u);
    // Wall-mounted speaker — near booth, noir jazz source
    {
        int spkX = kBarSpeakerX + parallaxFar, spkY = kBarSpeakerY;
        canvas.fillRect(spkX, spkY, 12, 12, RP::D_STRUCT);               // housing
        canvas.fillRect(spkX + 2, spkY + 2, 8, 8, RP::FILL);             // grille
        canvas.fillRect(spkX + 4, spkY + 4, 4, 4, RP::DEEP);             // cone
        // RF cadence owns the cone; on quiet air it falls back to bar ambience.
        uint8_t rfActivity = barEffectiveRfActivity();
        uint32_t rfPeriod = 700u - ((uint32_t)rfActivity * 500u) / 255u;
        bool rfBeat = rfActivity >= 8u && (now % rfPeriod) < 100u;
        if (rfBeat)
            canvas.fillRect(spkX + 4, spkY + 4, 4, 4, barRfColor());
        else if (neonOn && ((now / 240u) & 1u))
            canvas.fillRect(spkX + 4, spkY + 4, 4, 4, RP::WALL_MID);
        // Mounting bracket
        canvas.fillRect(spkX + 4, spkY - 2, 4, 2, RP::WALL_MID);
        drawPopOutline1px(canvas, spkX, spkY, 12, 12, PopOutlineStyle::SOLID, 0xE0B1u);
    }
    // The stemware now hangs off the back bar's own cornice rail — see
    // drawBackbar. A separate floating rack in this plane used to cross the
    // mirror and cut every reflection behind it in half.
    // Bar counter (dark furniture — no light cast)
    int barBx = kR5_BarX + parallaxMid, barBy = kR5_BarTopY;
    int barBw = kR5_BarW, barBsh = kR5_BarSurfH;
    {
        canvas.fillRect(barBx, barBy, barBw, barBsh, RP::D_STRUCT);
        canvas.fillRect(barBx + 4, barBy + 4, barBw - 8, barBsh - 4, RP::FILL);
        canvas.fillRect(barBx, barBy + barBsh, barBw, kFloorY - barBy - barBsh, RP::WALL_MID);
        canvas.fillRect(barBx + 4, barBy + barBsh + 4, barBw - 8, kFloorY - barBy - barBsh - 8, RP::FILL);
        canvas.fillRect(barBx + 4, kFloorY - 4, barBw - 8, 4, RP::DEEP);
        canvas.fillRect(barBx, barBy, barBw, kRoomPX, RP::D_STRUCT);
        int bottleBase = barBy - 4;
        // Tall bottle 1 — amber whiskey
        canvas.fillRect(barBx + 8, bottleBase - 12, 4, 4, RP::D_WALL_NEAR);   // neck
        canvas.fillRect(barBx + 8, bottleBase - 8, 4, 12, kBottleAmber);       // liquid
        canvas.fillRect(barBx + 8, bottleBase - 12, 8, 4, RP::D_STRUCT);       // cap
        // Short bottle 2 — green absinthe
        canvas.fillRect(barBx + 20, bottleBase - 8, 4, 4, RP::D_WALL_NEAR);   // neck
        canvas.fillRect(barBx + 20, bottleBase - 4, 4, 8, kBottleGreen);       // liquid
        canvas.fillRect(barBx + 20, bottleBase - 8, 8, 4, RP::D_STRUCT);       // cap
        // Tall bottle 3 — clear vodka
        canvas.fillRect(barBx + 36, bottleBase - 12, 4, 8, kBottleClear);      // liquid
        canvas.fillRect(barBx + 36, bottleBase - 4, 4, 8, RP::D_WALL_NEAR);   // lower body
        // Short bottle 4 — amber bourbon
        canvas.fillRect(barBx + 52, bottleBase - 8, 4, 8, kBottleAmber);       // liquid
        canvas.fillRect(barBx + 52, bottleBase, 4, 4, RP::D_WALL_NEAR);       // base
        canvas.fillRect(barBx + 52, bottleBase - 8, 8, 4, RP::D_STRUCT);       // cap
        // Tumbler glass — short, wide
        canvas.fillRect(barBx + 68, bottleBase, 8, 4, RP::PUDDLE);
        canvas.fillRect(barBx + 68, bottleBase - 4, 8, 4, RP::D_WALL_NEAR);
        // Bar stools
        for (int si = 0; si < 2; si++) {
            int stX = barBx + 16 + si * 52;
            int stSeatY = kFloorY - 24;
            canvas.fillRect(stX, stSeatY, 12, 4, RP::D_STRUCT);
            canvas.fillRect(stX + 4, stSeatY + 4, 4, kFloorY - stSeatY - 4, RP::WALL_MID);
            canvas.fillRect(stX, kFloorY - 8, 12, kRoomPX, RP::WALL_MID);
        }
    }
    // Pendant light fixture (dead — no power, subtle sway from air movement)
    float pendantWave = fastSinf((float)now / 4000.0f * 6.28f);
    int pendantSway = (pendantWave > 0.5f) ? kRoomPX :
                       ((pendantWave < -0.5f) ? -kRoomPX : 0);
    int anchorX = (kBarPendantX + parallaxMid) & ~(kRoomPX - 1);
    int pendantLx = anchorX + pendantSway, pendantLy = kR5_LightY;
    {
        // cord stays nailed up top. dead lamp walks one 4px cell.
        int cordMidY = ((kRoomY + pendantLy) / 2) & ~(kRoomPX - 1);
        canvas.fillRect(anchorX, kRoomY, kRoomPX, cordMidY - kRoomY, RP::WALL_MID);
        int bridgeX = pendantLx < anchorX ? pendantLx : anchorX;
        int bridgeW = ((pendantLx < anchorX) ? (anchorX - pendantLx) :
                       (pendantLx - anchorX)) + kRoomPX;
        canvas.fillRect(bridgeX, cordMidY, bridgeW, kRoomPX, RP::WALL_MID);
        canvas.fillRect(pendantLx, cordMidY + kRoomPX, kRoomPX,
                        pendantLy - cordMidY - kRoomPX, RP::WALL_MID);
        // Shade + dead bulb
        canvas.fillRect(pendantLx - kRoomPX, pendantLy, 12, 8, RP::D_STRUCT);
        canvas.fillRect(pendantLx, pendantLy + 4, 4, 4, RP::FILL);
        canvas.fillRect(pendantLx, pendantLy + 8, 8, 4, RP::WALL_MID);  // dead bulb
    }
    // Corner booth (cushion, walls, table, drink)
    {
        int bx = kR5_BoothX + parallaxMid, by = kR5_BoothY;
        int bw = kR5_BoothW, bh = kR5_BoothH;
        canvas.fillRect(bx, by, bw - 16, bh, RP::SOFT);
        canvas.fillRect(bx + 4, by + 4, bw - 24, bh - 8, RP::FILL);
        canvas.fillRect(bx + 12, by + 4, 8, 4, RP::SHADOW_C);
        canvas.fillRect(bx + 32, by + 8, 8, 4, RP::SHADOW_C);
        canvas.fillRect(bx + 4, kFloorY - 4, bw - 24, 4, RP::DEEP);
        int backX = bx + bw - 20;
        canvas.fillRect(backX, by - 56, 16, bh + 56, RP::SOFT);
        canvas.fillRect(backX + 4, by - 52, 8, bh + 48, RP::FILL);
        canvas.fillRect(backX, by - 56, 16, 4, RP::D_STRUCT);
        drawBarBoothNearArm(canvas, bx, by);
        int tableW = 72, tableH = 8;
        int tableX = bx + 8, tableY = by - 16;
        canvas.fillRect(tableX, tableY, tableW, tableH, RP::D_STRUCT);
        canvas.fillRect(tableX + 4, tableY + 4, tableW - 8, 4, RP::FILL);
        canvas.fillRect(tableX + 12, tableY + tableH, kRoomPX, kFloorY - tableY - tableH, RP::WALL_MID);
        canvas.fillRect(tableX + tableW - 12, tableY + tableH, kRoomPX, kFloorY - tableY - tableH, RP::WALL_MID);
        // Napkin and condensation cells stay on the room grid beneath the
        // glass; the prior 2px strips read as UI aliasing on the chunky table.
        canvas.fillRect(tableX + 44, tableY - 4, 16, kRoomPX, RP::SOFT);
        canvas.fillRect(tableX + 44, tableY, kRoomPX, kRoomPX, RP::PUDDLE);
        canvas.fillRect(tableX + 56, tableY, kRoomPX, kRoomPX, RP::PUDDLE);
        // Main drink glass
        canvas.fillRect(tableX + 48, tableY - 8, 4, 8, RP::D_WALL_NEAR);
        canvas.fillRect(tableX + 48, tableY - 8, 8, 4, RP::PUDDLE);
        // Second glass — empty, lying on side (suggests long night)
        canvas.fillRect(tableX + 28, tableY - 4, 8, 4, RP::D_WALL_NEAR);
        canvas.fillRect(tableX + 24, tableY - 4,
                        kRoomPX, kRoomPX, RP::PUDDLE);  // rim opening
    }
    // kR5_* are roomY() values on the room lattice (y = 2 mod 4) while the
    // contour snaps to the screen lattice, so an exact-height outline rounds
    // its bottom row 2px back up into the cushion. One cell of slack puts it
    // in the gap below the upholstery, above the contact shadow.
    drawPopOutline1px(canvas, kR5_BoothX + parallaxMid, kR5_BoothY - 56,
                      kR5_BoothW, kR5_BoothH + 56 + kRoomPX,
                      PopOutlineStyle::MIXED, 0xE071u);
    // Floor props
    // Open strip between the terminal stool and bar-side stool. The old x=170
    // coordinate buried most of the 12px grate in the counter toe kick.
    drawFloorDrain4(canvas, 52, kFloorY - 4);
    {
        int stoolX = kR5_BarX + parallaxMid - 16;
        canvas.fillRect(stoolX, kFloorY - 8, 12, 4, RP::D_STRUCT);
        canvas.fillRect(stoolX + 8, kFloorY - 8, 4, 4, RP::SHADOW_C);
        canvas.fillRect(stoolX + 4, kFloorY - 4, kRoomPX, 4, RP::WALL_MID);
        canvas.fillRect(stoolX + 12, kFloorY - 8, kRoomPX, 8, RP::WALL_MID);
        canvas.fillRect(stoolX + 12, kFloorY - 12, kRoomPX, 4, RP::FILL);
    }
    // Enrichments (pre-darken — physical objects)
    {
        int ashX = kR5_BoothX + parallaxMid + 28, ashY = kR5_BoothY - 20;
        drawAshtray4(canvas, ashX, ashY);
    }

    // Furniture drawn with D_STRUCT/D_WALL_NEAR pre-darkened palette. No global darken pass.
    // Event color grades only the room plate; RF spill stays attached to the
    // terminal corner. Practical neon/CRT faces are restored immediately after.
    PixelFurn::drawCRTTerminal4(canvas, tx, ty, tw, th, now);

    // ==[ PHASE 3: CAST LIGHT — only these reveal objects on the darkened scene ]==
    // THE PEN neon sign cast (when on)
    if (neonOn) {
        PigLight neonL;
        neonL.x = (int16_t)(nx + nw / 2);
        neonL.y = (int16_t)(ny + nh / 2);
        neonL.tint = RP::NEON;
        // Sign halo + floor puddle
        drawLightPool(canvas, RP::NEON, nx - 8, ny - 8, nw + 16, nh + 16, 18, 99551);
        drawLightPool(canvas, RP::NEON, nx - 4, ny - 8, nw + 8, 4, 10, 99552);
        drawLightPoolGradient(canvas, RP::PUDDLE, nx - 4, kFloorY - 6, nw + 8, 6,
                              (uint8_t)25, 77551);
        // Keep the sign wash on the far wall. Let the backbar, counter, booth,
        // and near frame retain separate values instead of sharing one glow.
        drawFurnitureWash(canvas, nx - 8, kRoomY + 4,
                          nw + 16, 68,
                          neonL, 116.0f, 0.12f);
    }
    // Restore the authored 4px silhouette after the bounded far-wall wash.
    // The stage/body/mic stay planted while snout, head, and hooves perform.
    PixelFurn::drawKaraokeStage4(canvas, karaokeX, kKaraokeY, now);
    // Crowd between us and the stage, drawn last so it occludes the stage lip.
    drawKaraokeAudience(canvas, spot, barLoop,
                        kKaraokeAlcoveX + parallaxFar);
    // At the reachable (far=0, mid=+4) parallax pair the far stage overlaps
    // one cell of this nearer booth arm. Restore the near plane after the live
    // performer so depth never inverts during IMU motion.
    drawBarBoothNearArm(canvas, kR5_BoothX + parallaxMid, kR5_BoothY);
    // Under-bar bounce stays behind the Barman and counter face. The mirror
    // itself is restored after the animated Barman reclaims the surface.
    if (neonOn) {
        // Light spill under bar counter — neon bounce off floor.
        for (int ux = barBx + 8; ux < barBx + barBw - 8; ux += kRoomPX) {
            if ((wallHash(ux, kFloorY, 55891) & 0xFF) < 80) {
                uint16_t base = fastReadPx(canvas, ux, kFloorY - 4);
                canvas.fillRect(ux, kFloorY - 4, kRoomPX, kRoomPX,
                                screenBlend565(base, RP::PUDDLE, 18));
            }
        }
    }
    // Terminal CRT cast — wall glow above, shelf glow below, stool seat glow
    drawLightPool(canvas, RP::CRT, tx - 2, ty - 4, tw + 4, 4, 12, 88331);
    drawLightPool(canvas, RP::CRT, tx, ty + th, tw, 6, 16, 88332);
    drawLightPool(canvas, RP::CRT, kR5_StoolX + parallaxMid - 2,
                  kR5_StoolSeatY - 4,
                  kR5_StoolW + 4, 4, 6, 88340);
    // Drive LEDs (emissive indicators, post-darken so they pop)
    for (int i = 0; i < 3; i++) {
        int driveX = tx + 4 + i * 16;
        uint32_t ledCycle = 1200 + i * 400;
        if ((now % ledCycle) < 400)
            canvas.fillRect(driveX + 4, ty + th + 4, kRoomPX, kRoomPX, RP::LED);
    }

    // ==[ BARMAN NPC ]== drawn after washes — own bump shade is sole light pass
    // Broad room/event washes may tint furniture, never categorical sources.
    // Restore THE PEN tubes and the RF deck after every volume pass.
    PixelFurn::drawNeonSign_THEPEN4(canvas, nx, ny, now);
    drawBarRfDeck(canvas, tx, ty, tw);

    Barman::draw(canvas, now, barBx, barBw);
    // Restore the complete 8px counter surface. The Barman's turn-settle can
    // lower his body 2px; repainting only the first row left a pig-colored
    // strip through the inner half of the bar.
    canvas.fillRect(barBx, barBy, barBw, kRoomPX, RP::D_STRUCT);
    canvas.fillRect(barBx + kRoomPX, barBy + kRoomPX,
                    barBw - 2 * kRoomPX,
                    barBsh - kRoomPX, RP::FILL);
    // Redraw bottles occluded by barman body (bottles sit ON counter, in front)
    {
        int bottleBase = barBy - 4;
        // Short bottle 2 — green absinthe
        canvas.fillRect(barBx + 20, bottleBase - 8, 4, 4, RP::D_WALL_NEAR);
        canvas.fillRect(barBx + 20, bottleBase - 4, 4, 8, kBottleGreen);
        canvas.fillRect(barBx + 20, bottleBase - 8, 8, 4, RP::D_STRUCT);
        // Tall bottle 3 — clear vodka
        canvas.fillRect(barBx + 36, bottleBase - 12, 4, 8, kBottleClear);
        canvas.fillRect(barBx + 36, bottleBase - 4, 4, 8, RP::D_WALL_NEAR);
        // Short bottle 4 — amber bourbon
        canvas.fillRect(barBx + 52, bottleBase - 8, 4, 8, kBottleAmber);
        canvas.fillRect(barBx + 52, bottleBase, 4, 4, RP::D_WALL_NEAR);
        canvas.fillRect(barBx + 52, bottleBase - 8, 8, 4, RP::D_STRUCT);
        // Tumbler glass
        canvas.fillRect(barBx + 68, bottleBase, 8, 4, RP::PUDDLE);
        canvas.fillRect(barBx + 68, bottleBase - 4, 8, 4, RP::D_WALL_NEAR);
    }
    // Counter mirror: source glyph flicker moves; wet-surface breakup stays
    // put. Run this after the Barman repair so the second surface row does not
    // erase the reflection during his 2px turn-settle drop.
    if (neonOn) {
        uint64_t litColumns = samplePenSignColumnMask(now, nx);
        for (int rx = barBx + 4; rx < barBx + barBw - 4; rx += kRoomPX) {
            float signFrac = (float)(rx - barBx) / (float)barBw;
            int signX = nx + (int)(signFrac * (float)nw);
            int signCell = (signX - nx) / kRoomPX;
            bool litSign = signCell >= 0 && signCell < 64 &&
                           (litColumns & (1ULL << signCell)) != 0;
            if (litSign &&
                (wallHash(rx, barBy, 0x7E11u) & 0xFFu) < 144u) {
                uint16_t base = fastReadPx(canvas, rx, barBy + kRoomPX);
                canvas.fillRect(rx, barBy + kRoomPX,
                                kRoomPX, kRoomPX,
                                screenBlend565(base, RP::PUDDLE, 42));
            }
        }
    }
    // Re-apply neon wash to counter surface only (bottles too narrow to matter,
    // wider rect would spill onto barman's cigarette/scar/arm)
    if (neonOn) {
        PigLight ctrL;
        ctrL.x = (int16_t)(nx + nw / 2);
        ctrL.y = (int16_t)(ny + nh / 2);
        ctrL.tint = RP::NEON;
        drawFurnitureWash(canvas, barBx, barBy, barBw, kRoomPX, ctrL, 130.0f, 0.18f);
    }

    // ==[ PHASE 4: POST-CAST EMISSIVE PARTICLES ]==
    // CRT condensation (emissive droplets on self-lit screen)
    drawCondensation(canvas, now, tx + 4, ty + 4, tw - 8, th - 8,
                     kBarTermCRTCondensationCount, RP::CRT,
                     kBarTermCRTCondensationSalt);
    // Barman cigarette smoke (post-light so particles catch neon wash)
    drawBarmanSmoke(canvas, now);
}

static constexpr int kBoothTableY = kR5_BoothY - 16;
static constexpr int kBoothBackMinX =
    kR5_BoothX + kParallaxMidMin + kR5_BoothW - 20;
static constexpr int kPendantEnvelopeMinX =
    kBarPendantX + kParallaxMidMin - 2 * kRoomPX;
static constexpr int kPendantEnvelopeMaxX =
    kBarPendantX + kParallaxMidMax + 3 * kRoomPX;

static_assert(kKaraokeX + kParallaxFarMin >= 0 &&
              kKaraokeX + kParallaxFarMax + kKaraokeW <= SCREEN_WIDTH,
              "karaoke stage must stay inside the room at IMU extremes");
static_assert(roomObjectFitsShifted(kBarBackbarX, kBarBackbarY,
                                    kBarBackbarW, kBarBackbarH,
                                    kParallaxMidMin, kParallaxMidMax),
              "bar backbar recess must stay inside the room at IMU extremes");
static_assert(kBarBackbarX % kRoomPX == 0 && kBarBackbarY % kRoomPX == 0 &&
              kBarBackbarW % kRoomPX == 0 && kBarBackbarH % kRoomPX == 0,
              "back bar carcass must sit on the room grid");
static_assert(kR5_TermX + kR5_TermW + kRoomPX <= kBarBackbarX,
              "back bar must clear the wall terminal (both are mid parallax)");
static_assert(kBarBackbarY >= kR5_NeonY + kR5_NeonH &&
              kBarBackbarY + kBarBackbarH == kR5_BarTopY,
              "back bar must fill the wall from THE PEN's underside to the counter");
// Row plan integrity: every band the drawer indexes has to exist, in order,
// inside the carcass. Nudging one row constant otherwise silently paints the
// mirror over the shelving or the plinth outside the box.
static_assert(kBarCorniceRow + 1 < kBarStemRow &&
              kBarStemRow < kBarMirrorTopRow &&
              kBarMirrorTopRow + kBarMirrorRows <= kBarBottleLineRow &&
              kBarBottleLineRow + 1 < kBarShelfARow &&
              kBarShelfARow + 1 < kBarShelfBRow &&
              kBarShelfBRow + 1 < kBarPlinthRow &&
              kBarPlinthRow < kBarBackbarRows,
              "back bar row plan must stay ordered inside the carcass");
// Reflections are cut off at the chest by the bottle line, never floating.
static_assert(kBarReflCol[0] >= 1 &&
              kBarReflCol[kBarReflCount - 1] + kBarReflW <= kBarBackbarCols - 2,
              "mirror reflections must stay inside the glass opening");
static_assert(kBarMirrorRows >= 5,
              "mirror needs the rows the reflected heads and shoulders occupy");
// drawBackbarReflection derives the shoulder height as (glass rows left - 1);
// too much stagger drives that to zero and hands fillRect a negative height.
static_assert(kBarReflRow[0] >= 0 && kBarReflRow[0] <= kBarMirrorRows - 3 &&
              kBarReflRow[1] >= 0 && kBarReflRow[1] <= kBarMirrorRows - 3 &&
              kBarReflRow[2] >= 0 && kBarReflRow[2] <= kBarMirrorRows - 3 &&
              kBarReflRow[3] >= 0 && kBarReflRow[3] <= kBarMirrorRows - 3,
              "reflection stagger must leave head and shoulder rows in the glass");
static_assert(kKaraokeAlcoveX == kKaraokeX - 2 * kRoomPX &&
              kKaraokeAlcoveW == kKaraokeW + 4 * kRoomPX,
              "alcove must frame the stage with one cell of jamb per side");
static_assert(kKaraokeAlcoveH >= 5 * kRoomPX &&
              kKaraokeAlcoveY + kKaraokeAlcoveH <= kR5_BoothY - kRoomPX,
              "audience band needs five alcove rows clear of the booth");
static_assert(kKaraokeY + (kKarStageRow + 2) * kRoomPX ==
                  kKaraokeY + kKaraokeH,
              "stage rows must land exactly on the performer's lower edge");
static_assert(roomObjectFitsShifted(kKaraokeAlcoveX, kKaraokeAlcoveY,
                                    kKaraokeAlcoveW, kKaraokeAlcoveH,
                                    kParallaxFarMin, kParallaxFarMax),
              "karaoke alcove must stay inside the room at IMU extremes");
static_assert(roomObjectFits(kBarNearDividerX, kBarNearDividerY,
                             kBarNearDividerW,
                             kFloorY - kBarNearDividerY),
              "bar near divider must stay inside the room");
static_assert(kBarBackbarX + kParallaxMidMax + kBarBackbarW <=
                  kBarNearDividerX,
              "bar backbar must clear the near divider");
static_assert(kBarNearDividerX + kBarNearDividerW >=
                  kKaraokeAlcoveX + kParallaxFarMax,
              "near divider must overlap the far karaoke jamb at both IMU extremes");
static_assert(kKaraokeY >= kR5_NeonY + kR5_NeonH + kRoomPX,
              "karaoke silhouette must clear THE PEN below");
static_assert(kKaraokeY + kKaraokeH + kRoomPX <= kBoothTableY,
              "karaoke stage needs a full room cell above the booth table");
static_assert(kKaraokeX + kParallaxFarMax + kKaraokeW + kRoomPX <=
                  kBoothBackMinX,
              "karaoke stage needs a full room cell before the booth back");
static_assert(kBarPosterX + kBarPosterW + kRoomPX <= kR5_NeonX,
              "bar poster must clear THE PEN plate");
static_assert(kBarFuseX + 12 + kRoomPX <= kR5_NeonX,
              "bar fuse box must clear THE PEN plate");
static_assert(kBarFuseY + 16 <= kBarBackbarY,
              "bar fuse box must clear the back bar cornice");
static_assert(kBarSpeakerX >= kR5_NeonX + kR5_NeonW + kRoomPX,
              "bar speaker must clear THE PEN plate");
static_assert(kR5_NeonX + kParallaxFarMax + kR5_NeonW + kRoomPX <=
                  kPendantEnvelopeMinX,
              "pendant sway envelope must clear THE PEN plate");
static_assert(kPendantEnvelopeMaxX <= SCREEN_WIDTH,
              "pendant sway envelope must stay inside the room");
static_assert(kR5_BarmanX >= kR5_BarX &&
              kR5_BarmanX + kPigW <= kR5_BarX + kR5_BarW,
              "barman body must stay inside the counter span");
static_assert(kR5_BarmanY + kPigH - kR5_BarTopY == kRoomPX,
              "counter lip must occlude the barman's lower body row");
static_assert(kR5_BarmanY + kPigH + kPigPX <=
                  kR5_BarTopY + kR5_BarSurfH,
              "counter surface must contain the barman turn-settle drop");

// ==[ ROOM 4 KEY LIGHT ]== nearest practical owns every pig light layer
PigLight selectRoom4PigKeyLight(int pigDrawX, int pigDrawY, uint32_t now) {
    PigLight neonLight;
    neonLight.x = (int16_t)(kR5_NeonX + parallaxFar + kR5_NeonW / 2);
    neonLight.y = (int16_t)(kR5_NeonY + kR5_NeonH / 2);
    const uint8_t neonEnergy = penNeonEnergy8(now);
    neonLight.tint = neonEnergy == 0
        ? (uint16_t)0
        : lerpColor565_8(RP::BG, RP::NEON, neonEnergy);

    PigLight crtLight;
    crtLight.x = (int16_t)(kR5_TermX + parallaxMid + kR5_TermW / 2);
    crtLight.y = (int16_t)(kR5_TermY + kR5_TermH / 2);
    crtLight.tint = RP::CRT;

    float pigCX = (float)(pigDrawX + kPigW / 2);
    float pigCY = (float)(pigDrawY + kPigH / 2);
    PigLight key = crtLight;
    float keyS = room2EmitterScoreAt(crtLight, pigCX, pigCY, 120.0f, 0.8f);
    float neonS = room2EmitterScoreAt(neonLight, pigCX, pigCY, 200.0f,
                                      (float)neonEnergy / 255.0f);
    if (neonS > keyS) key = neonLight;
    return key;
}

// ==[ NOIR ON PIG: Room 4 Bar ]== directional half-Lambert volume shading
static void applyRoom4NoirToPig(M5Canvas& canvas, uint32_t) {
    static constexpr PigNoirProfile kProfile = {82, 210, 26, 90, 85};
    applyDirectionalPigNoir(canvas, kProfile);
}

void drawRoom4Foreground(M5Canvas& canvas, uint32_t now) {
    // ==[ NOIR ON PIG ]== must come before foreground furniture
    applyRoom4NoirToPig(canvas, now);
    // Sparse near-plane architecture: a ceiling soffit and one divider are
    // enough to overlap the far wall, backbar, and karaoke alcove. This same
    // foreground seam also runs during teleport reconstruction.
    canvas.fillRect(0, kRoomY, SCREEN_WIDTH, 2 * kRoomPX, RP::D_STRUCT);
    canvas.fillRect(kRoomPX, kRoomY + kRoomPX,
                    SCREEN_WIDTH - 2 * kRoomPX,
                    kRoomPX, RP::WALL_MID);
    canvas.fillRect(kBarNearDividerX, kBarNearDividerY,
                    kBarNearDividerW,
                    kFloorY - kBarNearDividerY, RP::D_STRUCT);
    canvas.fillRect(kBarNearDividerX, kBarNearDividerY + kRoomPX,
                    kRoomPX,
                    kFloorY - kBarNearDividerY - kRoomPX,
                    RP::WALL_MID);
    canvas.fillRect(kBarNearDividerX + kRoomPX, kBarNearDividerY,
                    kRoomPX,
                    kFloorY - kBarNearDividerY, RP::DEEP);
    if (isStationVisualActive(Station::AT_TERMINAL)) {
        // The rear pose overlaps the stool by one 4px cell. Redraw the seat so
        // the support edge owns that cell and the planted legs stay behind it.
        canvas.fillRect(kR5_StoolX + parallaxMid, kR5_StoolSeatY,
                        kR5_StoolW, kRoomPX, RP::D_STRUCT);
        canvas.fillRect(kR5_StoolX + parallaxMid + 4,
                        kR5_StoolSeatY + kRoomPX,
                        kR5_StoolW - 8, kRoomPX, RP::FILL);
    }
    // Booth table edge — pig sits "behind" wide table (foreground occlusion)
    if (isStationVisualActive(Station::AT_BOOTH)) {
        int tableX = kR5_BoothX + parallaxMid + 8;
        int tableY = kR5_BoothY - 16;
        int tableW = 72;
        canvas.fillRect(tableX, tableY, tableW, 4, RP::D_STRUCT);
        canvas.fillRect(tableX + 4, tableY + 4, tableW - 8, 4, RP::SHADOW_C);
        // table edge lip — extended depth bands to fully occlude pig lower body
        canvas.fillRect(tableX + 4, tableY + 8, tableW - 8, 4, RP::FILL);
        canvas.fillRect(tableX + 4, tableY + 12, tableW - 8, 4, RP::DEEP);
        canvas.fillRect(tableX + 8, tableY + 16, tableW - 16, 4, RP::DEEP);  // tapered edge
    }
}

} // namespace MenuPig
