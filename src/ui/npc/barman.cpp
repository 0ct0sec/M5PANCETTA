/** barman.cpp — Barman NPC (DEFHOG1) for Room 4 underground bar
 *
 * Deaf barman. Pancetta clone rendered via PigRenderer::drawBody.
 * Blink, breathe, head turn, eye roam, short white hair, scar, cigarette, wipe arm.
 * Drawn behind the counter — room handles counter/bottle redraw on top.
 */

#include "barman.h"
#include "../menu_pig_internal.h"
#include "../../piglet/mood.h"
#include "../../core/item_drops.h"

using namespace MenuPig;

// ==[ SCAR ]== vertical on RIGHT cheek, past snout edge (col 16 = bx+64)
// RIGHT-facing snout spans bx+38..bx+62, front eye at bx+54 — col 16 clears both
// Rows 3-5 (drawY+12..20) sit between eye level and cigarette at drawY+24
static constexpr struct { int8_t col, row; float nx, ny; } kScarCells[] = {
    {16, 3, -0.6f, -0.3f},  // upper cheek at eye level (past eye X)
    {16, 4, -0.5f,  0.0f},  // mid cheek
    {16, 5, -0.4f,  0.2f},  // lower cheek (above cigarette)
};
static constexpr int kScarCount = sizeof(kScarCells) / sizeof(kScarCells[0]);

// ==[ PERSISTENT STATE ]== survives across frames, init once
static PigFaceTimer barmanFace;
static bool barmanStateInit = false;

// Head turn FSM
static bool barmanFaceRight = true;
static uint32_t barmanTurnTimer = 0;
static uint32_t barmanTurnDuration = 0;
static bool barmanTurnPending = false;
static uint32_t barmanTurnLeadAt = 0;
static constexpr uint32_t BARMAN_TURN_LEAD_MS = 260;

// A room visit starts with hands planted; counter work never appears mid-stroke.
static bool barmanRoomVisible = false;
static uint32_t barmanVisibleSince = 0;

// Eye look cycle (independent of head turn)
static PigEyeLook barmanEyeLook = PigEyeLook::FRONT_DOWN;
static uint32_t barmanEyeTimer = 0;
static uint32_t barmanEyeDuration = 0;

// Neon catches the lenses on turns and small head/glance movements.
static uint32_t barmanLensGlintAt = 0;
static bool barmanLensGlintActive = false;
static constexpr uint32_t BARMAN_LENS_GLINT_MS = 420;

// Smoke origin — set each frame by draw(), read by room for emissive particles
static int cachedEmberX = 0;
static int cachedEmberY = 0;

// Cached body position — set each frame by draw(), used by drawBubble()
static int cachedBx = 0;
static int cachedBy = 0;
static bool cachedFaceRight = true;

// ==[ DIALOGUE STATE ]== clipped replies to Pancetta and his cat, Pig
static char barmanBubbleText[96] = "";
static uint32_t barmanSpokeAt = 0;
static bool barmanWaitingToRespond = false;
static uint32_t barmanTriggerAt = 0;
static uint32_t barmanResponseDelay = 0;
static bool barmanFourthWall = false;

enum class BarmanResponseCue : uint8_t {
    NONE = 0,
    PANCETTA,
    CAT_PIG,
};

static BarmanResponseCue barmanResponseCue = BarmanResponseCue::NONE;
static uint32_t barmanCatFocusUntil = 0;

static constexpr uint32_t BARMAN_RESPONSE_DELAY_MIN = 1200;
static constexpr uint32_t BARMAN_RESPONSE_DELAY_MAX = 1800;
static constexpr uint32_t BARMAN_BUBBLE_DURATION = 4500;

// History dedup — 4-entry rolling, same pattern as Mood::selectPhrase
static int barmanLastIdx[4] = { -1, -1, -1, -1 };
static int barmanHistPos = 0;
static int barmanLastCatIdx = -1;

// ==[ KOREAN PHRASE POOL ]== Hangul responses — barman speaks Korean
// Guarded fixer cadence, noodle-bar economy, and K-Hole horse foreshadowing.
// Fourth wall phrases prefixed with '*' — triggers FRONT_UP eye contact.
static const char* const BARMAN_KR[] = {
    // ==[ BAR SERVICE ]== he reads the room, not long speeches
    "뭐 먹을래?",              // what'll you have?
    "같은 거?",                // the usual?
    "주문해.\n사연은 빼고.",      // order; leave out the story.
    "말은 짧게.\n잔은 비워.",     // keep words short; empty the glass.
    "국수 먹어.",               // eat your noodles.
    "계산은 나중에.",            // settle up later.
    "조용히 먹어.",              // eat quietly.
    "앉아.\n비 맞았잖아.",        // sit; you got rained on.
    // ==[ GUARDED FIXER ]== terse noir, useful only by accident
    "장부는 기억해.\n나는 안 해.",   // the ledger remembers; I don't.
    "여기선 이름보다\n버릇을 봐.",    // here, habits matter more than names.
    "신호는 거짓말해.\n흔적은 덜 해.", // signals lie; traces lie less.
    "질문은 하나.\n대답은 반만.",     // one question; half an answer.
    "밤이 길다.\n서두르지 마.",       // the night is long; don't rush.
    "손님 없어.\n목격자만 있어.",      // no customers; only witnesses.
    "문은 열려 있어.\n빚도 그래.",     // the door is open; so is the debt.
    // ==[ K-HOLE / HORSE ]== foreshadowing. 말 = horse = word.
    "말 한 마리\n왔어.",          // a horse came once.
    "깊은 구멍.\n조심해.",        // deep hole; careful.
    "구멍에 빠지면\n못 나와.",     // fall in and you do not get out.
    "꿈인가.\n현실인가.",         // dream or reality.
    // ==[ FOURTH WALL ]== one dry glance, never a language label
    "*거기 화면 앞.\n잔 비었어.",    // you at the screen; your glass is empty.
    "*보고 있지?\n증언은 안 돼.",    // watching? you cannot testify.
    "*자막 없어도\n표정은 보이잖아.", // no subtitles needed to read a face.
};
static constexpr int BARMAN_KR_COUNT = sizeof(BARMAN_KR) / sizeof(BARMAN_KR[0]);

// Pig is the cat's name. Before the subtitle ladder completes, the Barman
// addresses him in Korean; afterwards the same relationship reads in English.
// Every bubble is self-evidently one language, so none carries an EN/KR tag.
static const char* const BARMAN_CAT_KR[] = {
    "피그야.\n카운터에서 내려.",
    "피그, 잔은\n건드리지 마.",
    "피그 물은\n늘 여기 있어.",
    "피그 간식은\n외상이다.",
    "피그가 먼저\n단서를 찾았네.",
    "피그야.\n의자 긁지 마.",
    "피그는 조용해.\n너와 달리.",
    "피그 자리는\n비워 뒀어.",
    "피그한테\n참치 줘.",
};
static constexpr int BARMAN_CAT_KR_COUNT =
    sizeof(BARMAN_CAT_KR) / sizeof(BARMAN_CAT_KR[0]);

static const char* const BARMAN_CAT_EN[] = {
    "Pig. Paws off\nthe clean glass.",
    "Pig gets water.\nSame place as always.",
    "One tuna for Pig.\nNoodles for you.",
    "Pig found the clue.\nYou found the menu.",
    "Pig can stay.\nYou still owe me.",
    "That stool is Pig's.\nHe won it quietly.",
    "Pig checked the bar.\nNothing confessed.",
    "Pig gets the warm seat.\nHe pays in silence.",
    "Leave Pig's chair.\nFind your own alibi.",
};
static constexpr int BARMAN_CAT_EN_COUNT =
    sizeof(BARMAN_CAT_EN) / sizeof(BARMAN_CAT_EN[0]);

static const char* const BARMAN_KH_LEAK_1[] = {
    "말...\n...barn?",
    "구멍...\n...deep?",
    "국수.\n...signal?",
};
static constexpr int BARMAN_KH_LEAK_1_COUNT = sizeof(BARMAN_KH_LEAK_1) / sizeof(BARMAN_KH_LEAK_1[0]);

static const char* const BARMAN_KH_LEAK_2[] = {
    "말 두 번.\nBARN... OK?",
    "깊은 구멍.\nDEEP H0L3. WAIT.",
    "국수 먼저.\nN00DL3S THEN HUNT.",
};
static constexpr int BARMAN_KH_LEAK_2_COUNT = sizeof(BARMAN_KH_LEAK_2) / sizeof(BARMAN_KH_LEAK_2[0]);

static const char* const BARMAN_KH_ADVICE[] = {
    "Real XP only.\nNo counterfeit wins.",
    "Same SSID?\nCheck the BSSID.",
    "RSSI is not identity.",
    "No authorization?\nNo deauth.",
    "PMF means back off.",
    "Fake cheese glitters.\nEvidence does not.",
    "Tags are habits\nin motion.",
    "BLE plus WiFi\nis motive, not proof.",
    "One target.\nOne charge.",
};
static constexpr int BARMAN_KH_ADVICE_COUNT = sizeof(BARMAN_KH_ADVICE) / sizeof(BARMAN_KH_ADVICE[0]);

static void setBarmanBubble(const char* text, uint32_t now, bool fourthWall = false) {
    if (!text || !text[0]) return;
    strncpy(barmanBubbleText, text, sizeof(barmanBubbleText) - 1);
    barmanBubbleText[sizeof(barmanBubbleText) - 1] = '\0';
    barmanSpokeAt = now;
    barmanFourthWall = fourthWall;
    barmanWaitingToRespond = false;
    barmanResponseCue = BarmanResponseCue::NONE;
    barmanCatFocusUntil = 0;
}

// phrase selection with history dedup
static const char* pickBarmanPhrase(bool& isFourthWall) {
    uint8_t kh = ItemDrops::getKHorseTranslationLevel();
    if (kh > 0) {
        uint32_t roll = esp_random() % 100;
        if (kh == 1 && roll < 35 && BARMAN_KH_LEAK_1_COUNT > 0) {
            isFourthWall = false;
            return BARMAN_KH_LEAK_1[esp_random() % BARMAN_KH_LEAK_1_COUNT];
        }
        if (kh == 2 && roll < 55 && BARMAN_KH_LEAK_2_COUNT > 0) {
            isFourthWall = false;
            return BARMAN_KH_LEAK_2[esp_random() % BARMAN_KH_LEAK_2_COUNT];
        }
        if (kh >= 3 && roll < 75 && BARMAN_KH_ADVICE_COUNT > 0) {
            isFourthWall = false;
            return BARMAN_KH_ADVICE[esp_random() % BARMAN_KH_ADVICE_COUNT];
        }
    }

    if (BARMAN_KR_COUNT <= 0) { isFourthWall = false; return ""; }
    int idx = 0;
    int attempts = 0;
    do {
        idx = esp_random() % BARMAN_KR_COUNT;
        bool dup = false;
        for (int i = 0; i < 4; i++) {
            if (barmanLastIdx[i] == idx) { dup = true; break; }
        }
        if (!dup) break;
    } while (++attempts < 8);
    barmanLastIdx[barmanHistPos] = idx;
    barmanHistPos = (barmanHistPos + 1) % 4;
    const char* phrase = BARMAN_KR[idx];
    isFourthWall = (phrase[0] == '*');
    return isFourthWall ? phrase + 1 : phrase;
}

static const char* pickBarmanCatPhrase() {
    const bool translated = ItemDrops::getKHorseTranslationLevel() >= 3;
    const char* const* pool = translated ? BARMAN_CAT_EN : BARMAN_CAT_KR;
    const int count = translated ? BARMAN_CAT_EN_COUNT : BARMAN_CAT_KR_COUNT;
    if (count <= 0) return "";

    int idx = esp_random() % count;
    if (count > 1 && idx == barmanLastCatIdx)
        idx = (idx + 1 + (esp_random() % (count - 1))) % count;
    barmanLastCatIdx = idx;
    return pool[idx];
}

// ==[ STATE MACHINE ]==

static void initBarmanState(uint32_t now) {
    if (barmanStateInit) return;
    barmanStateInit = true;
    barmanFace.init(now + 2500, 4000, 9000, 8000, 16000);
    barmanFaceRight = true;
    barmanTurnTimer = now;
    barmanTurnDuration = randomRange(8000, 12000);
    barmanEyeLook = PigEyeLook::FRONT_DOWN;
    barmanEyeTimer = now;
    barmanEyeDuration = randomRange(3000, 6000);
}

static void updateBarmanState(uint32_t now) {
    initBarmanState(now);
    barmanFace.update(now, 0, false);
    bool headMoved = false;
    const bool watchingCat = barmanCatFocusUntil != 0 &&
        static_cast<int32_t>(now - barmanCatFocusUntil) < 0;
    if (!watchingCat) barmanCatFocusUntil = 0;

    // ==[ HEAD TURN ]== eyes lead, shoulders settle, then the mirrored pose commits.
    if (barmanTurnPending && now - barmanTurnLeadAt >= BARMAN_TURN_LEAD_MS) {
        barmanFaceRight = !barmanFaceRight;
        barmanTurnPending = false;
        headMoved = true;
        barmanTurnTimer = now;
        barmanTurnDuration = barmanFaceRight
            ? randomRange(8000, 12000)
            : randomRange(5000, 8000);
        barmanEyeLook = PigEyeLook::FRONT_DOWN;
        barmanEyeTimer = now;
        barmanEyeDuration = randomRange(1200, 2200);
    } else if (!watchingCat && !barmanTurnPending &&
               now - barmanTurnTimer >= barmanTurnDuration) {
        barmanTurnPending = true;
        barmanTurnLeadAt = now;
        headMoved = true;
        // Pre-glance in the old direction before the body commits.
        barmanEyeLook = PigEyeLook::BACK_DOWN;
        barmanEyeTimer = now;
        barmanEyeDuration = BARMAN_TURN_LEAD_MS;
    }

    // ==[ EYE LOOK CYCLE ]== weighted random: FRONT_DOWN 60%, BACK_DOWN 25%, FRONT_UP 15%
    if (!watchingCat && !barmanTurnPending &&
        now - barmanEyeTimer >= barmanEyeDuration) {
        PigEyeLook oldLook = barmanEyeLook;
        barmanEyeTimer = now;
        uint32_t roll = randomRange(0, 100);
        if (roll < 60) {
            barmanEyeLook = PigEyeLook::FRONT_DOWN;
            barmanEyeDuration = randomRange(3000, 6000);
        } else if (roll < 85) {
            barmanEyeLook = PigEyeLook::BACK_DOWN;
            barmanEyeDuration = randomRange(1500, 3000);
        } else {
            barmanEyeLook = PigEyeLook::FRONT_UP;
            barmanEyeDuration = randomRange(800, 1500);
        }
        headMoved = headMoved || (barmanEyeLook != oldLook);
    }

    // Pig gets the Barman's downward attention for the reply instead of a
    // generic screen-facing stare. The meow latch, not a timer guess, starts it.
    if (watchingCat && barmanEyeLook != PigEyeLook::FRONT_DOWN) {
        barmanEyeLook = PigEyeLook::FRONT_DOWN;
        headMoved = true;
    }

    // ==[ FOURTH WALL ]== override eye direction when addressing viewer
    if (barmanFourthWall && barmanBubbleText[0] &&
        barmanEyeLook != PigEyeLook::FRONT_UP) {
        barmanEyeLook = PigEyeLook::FRONT_UP;
        headMoved = true;
    }

    if (headMoved) {
        barmanLensGlintAt = now;
        barmanLensGlintActive = true;
    }
}

void Barman::setRoomVisible(bool visible, uint32_t now) {
    if (visible == barmanRoomVisible) return;
    barmanRoomVisible = visible;
    if (!visible) return;

    initBarmanState(now);
    barmanVisibleSince = now;
    barmanTurnPending = false;
    barmanTurnLeadAt = 0;
    barmanTurnTimer = now;
    barmanTurnDuration = randomRange(8000, 12000);
    barmanEyeLook = PigEyeLook::FRONT_DOWN;
    barmanEyeTimer = now;
    barmanEyeDuration = randomRange(3000, 6000);
    barmanLensGlintActive = false;
}

static uint8_t calcBarmanLensGlint(uint32_t now) {
    if (!barmanLensGlintActive) return 0;
    uint32_t age = now - barmanLensGlintAt;
    if (age >= BARMAN_LENS_GLINT_MS) {
        barmanLensGlintActive = false;
        return 0;
    }
    if (age < 80) return (uint8_t)(96 + age * 159 / 80);
    return (uint8_t)(255 - (age - 80) * 255 / (BARMAN_LENS_GLINT_MS - 80));
}

// ==[ BREATHING ]== 3400ms cycle (offset from main pig's 3000ms), -2..0 range
static int calcBarmanBreathe(uint32_t now) {
    static constexpr uint32_t BREATHE_MS = 3400;
    uint32_t bp = now % BREATHE_MS;
    uint32_t half = BREATHE_MS / 2;
    int raw;
    if (bp < half) raw = -(int)(bp * 2 / half);
    else           raw = -(int)((BREATHE_MS - bp) * 2 / half);
    return raw & ~1;
}

// ==[ SHORT WHITE HAIR ]== 4 static curved strands using fatLine at PX=4
// Each strand: 4 control points → 3 connected fatLine segments = smooth curve
// Strand deltas stay on the room grid; their roots ride the body's exact pose.

static void drawHair(M5Canvas& canvas, int bx, int by, bool faceRight) {
    int baseY = by + 10;  // preserves the rest pose while following every body step
    uint16_t hairColor = RP::FLUOR;  // bright white/silver for old barman

    // Control point offsets from (rootX, baseY): {dx, dy}, dy negative = upward
    // Shorter, stiffer curves than main pig — old barman's cropped white hair
    // Designed on 4px grid: all deltas divisible by 4, no degenerate segments
    static constexpr int8_t kStrands[4][4][2] = {
        { {0, 0}, {-4, -8},  { 0, -12}, { 4,  -8} },  // rootOffset 28: C-curve right
        { {0, 0}, { 4, -8},  { 4, -16}, { 0, -12} },  // rootOffset 36: rightward hook
        { {0, 0}, {-4, -8},  {-4, -16}, { 0, -12} },  // rootOffset 44: leftward hook
        { {0, 0}, { 4, -8},  { 0, -12}, {-4,  -8} },  // rootOffset 48: C-curve left
    };
    static constexpr int8_t kRootOffsets[4] = { 28, 36, 44, 48 };
    static constexpr int16_t kHairPx = 4;  // room pixel — matches main pig hair fatLine width

    for (int i = 0; i < 4; i++) {
        int rootOffset = kRootOffsets[i];
        int rootX = faceRight
            ? bx + rootOffset
            : bx + 72 - rootOffset;

        int16_t px[4], py[4];
        for (int p = 0; p < 4; p++) {
            int dx = kStrands[i][p][0];
            if (!faceRight) dx = -dx;
            px[p] = (int16_t)(rootX + dx);
            py[p] = (int16_t)(baseY + kStrands[i][p][1]);
        }

        for (int s = 0; s < 3; s++)
            Gfx::fatLinePx(canvas, px[s], py[s], px[s+1], py[s+1], hairColor, kHairPx);
    }
}

// ==[ DRAW ]==

void Barman::draw(M5Canvas& canvas, uint32_t now, int barBx, int barBw) {
    // Direct capture/test callers may bypass MenuPig::drawRoaming(). Treat
    // that first explicit draw as entry, but never infer entry from elapsed ms.
    if (!barmanRoomVisible) Barman::setRoomVisible(true, now);
    updateBarmanState(now);
    const bool faceRight = barmanFaceRight;
    int bx = (kR5_BarmanX + parallaxMid) & ~1;
    int breatheY = calcBarmanBreathe(now);
    int turnSettleY = 0;
    if (barmanTurnPending) {
        uint32_t turnAge = now - barmanTurnLeadAt;
        if (turnAge < BARMAN_TURN_LEAD_MS / 2u) turnSettleY = kPigPX;
    }
    int by = (kR5_BarmanY + breatheY + turnSettleY) & ~1;
    cachedBx = bx;
    cachedBy = by;
    cachedFaceRight = faceRight;
    uint16_t fg = Display::getColorFG();
    uint16_t bg = Display::getColorBG();
    Display::PigPalette pal = Display::makePigPalette(fg, bg);
    // Share Room 4's live-source arbitration with Pancetta. During THE PEN's
    // long dropout the CRT owns both pigs instead of invisible neon continuing
    // to shade only the Barman.
    PigLight keyLight = selectRoom4PigKeyLight(bx, by, now);

    // ==[ BODY ]== Pancetta clone via PigRenderer — animated facing, blink, eye look
    PigRenderPose pose;
    pose.x = (int16_t)bx;
    pose.y = (int16_t)by;
    pose.facing = faceRight ? PigFacing::RIGHT : PigFacing::LEFT;
    pose.expression = PigExpression::fromState(AvatarState::NEUTRAL,
        barmanFace.blinking, false, 0, barmanFace.earTwitching);
    pose.limbMode = LimbMode::AIRBORNE;  // legs stay behind the counter
    pose.tailGlyph = 'z';
    pose.tailOnLeft = faceRight;
    pose.eyeLook = barmanEyeLook;
    pose.detailColor = pal.detail;
    pose.light = keyLight;
    pose.blendRounding = true;
    pose.noHeadwear = true;
    pose.talking = Barman::isSpeaking(now);   // synced to speech bubble
    // NPC tail: zero player physics so barman tail doesn't mirror player movement
    float savedLeanX = Avatar::tailPhys.leanX;
    float savedBobY = Avatar::tailPhys.bobY;
    Avatar::tailPhys.leanX = 0.0f;
    Avatar::tailPhys.bobY = 0.0f;
    PigRenderer::drawBody(canvas, pose, pal.bodyFill, bg);
    Avatar::tailPhys.leanX = savedLeanX;
    Avatar::tailPhys.bobY = savedBobY;

    // ==[ SHORT WHITE HAIR ]== after body, before eye trim
    drawHair(canvas, bx, by, faceRight);

    // ==[ RED EYES + GLASSES ]== permanent tell, including the blink.
    int drawY = by + 2;  // PIG_DRAW_TOP_INSET
    {
        int eyeXs[2], eyeY = drawY + 12;
        if (faceRight) { eyeXs[0] = bx + 28; eyeXs[1] = bx + 54; }
        else                 { eyeXs[0] = bx + 32; eyeXs[1] = bx + 6;  }
        int eyeCx[2] = { eyeXs[0] + 6, eyeXs[1] + 6 };
        int cy = eyeY + 7;
        uint16_t redEye = Display::hsvToRgb565(0, 255, 255);
        uint16_t frame = RP::FLUOR;
        bool neonReflecting = isNeonOn(now);
        uint8_t turnGlint = neonReflecting ? calcBarmanLensGlint(now) : 0;
        int neonCx = kR5_NeonX + parallaxFar + kR5_NeonW / 2;

        // Pig detail bottoms out at 2px. The glasses own one more pixel.
        constexpr int GLASSES_FRAME_PX = 2;
        for (int i = 0; i < 2; i++) {
            int cx = eyeCx[i];
            uint16_t bodyC = fastReadPx(canvas, cx, cy + 7);
            canvas.fillRect(cx - 4, cy - 5, 6, 10, bodyC);
            canvas.fillRect(cx - 6, cy - 3, 10, 6, bodyC);

            // The lenses are actual glass planes, not empty eye holes. A low
            // cyan base separates glass from skin while keeping the eyes live.
            uint16_t glass = Display::screenBlend565(bodyC, RP::CRT,
                                                      neonReflecting ? 34 : 22);
            canvas.fillRect(cx - 8, cy - 7, 16, 14, glass);

            if (barmanFace.blinking) {
                int blX = ((cx - 5) / 2) * 2;
                int blY = (cy / 2) * 2;
                canvas.fillRect(blX, blY, 10, 2, redEye);
            } else {
                bool eyeUp = (barmanEyeLook == PigEyeLook::BACK_UP ||
                              barmanEyeLook == PigEyeLook::FRONT_UP);
                bool eyeFront = (barmanEyeLook == PigEyeLook::FRONT_UP ||
                                 barmanEyeLook == PigEyeLook::FRONT_DOWN);
                int dotX = cx + (faceRight ? (eyeFront ? 2 : -4) : (eyeFront ? -4 : 2));
                int dotY = cy + (eyeUp ? -4 : 2);
                dotX = (dotX / 2) * 2;
                dotY = (dotY / 2) * 2;
                canvas.fillRect(dotX - 1, dotY - 1, 6, 6, redEye);
            }
            // THE PEN crosses each pane as a broad diagonal reflection band.
            // Never collapse this into pinprick glints: isolated dots violate
            // the fat-pixel material language and make the lens read as skin.
            if (neonReflecting) {
                bool neonOnRight = neonCx >= cx;
                uint8_t steadyMix = (uint8_t)(58 + (((now / 160) + i) & 1) * 14);
                uint16_t steady = Display::screenBlend565(glass, RP::NEON, steadyMix);
                int bandX = neonOnRight ? cx - 6 : cx + 2;
                int bandStep = neonOnRight ? 2 : -2;
                canvas.fillRect(bandX, cy - 7, 8, 2, steady);
                canvas.fillRect(bandX + bandStep, cy - 5, 8, 2, steady);
                canvas.fillRect(bandX + bandStep * 2, cy - 3, 6, 2, steady);
                if (turnGlint > 0) {
                    uint16_t flash = Display::screenBlend565(RP::NEON, RP::FLUOR, turnGlint);
                    canvas.fillRect(cx - 8, cy - 7, 14, 2, flash);
                    canvas.fillRect(cx - 6 + bandStep, cy - 5, 10, 2, flash);
                }
            }
            // Stack outward so the lens opening stays readable. One skinny
            // vector outline looked detached from the fat-pixel pig.
            // Fat-pixel border (2px blocks, even-snapped) replaces the 1px
            // drawRoundRect so the frame reads as fat-pixel, not a thin vector.
            const int fx = cx - 10, fy = cy - 9, fw = 20, fh = 18, ft = GLASSES_FRAME_PX;
            canvas.fillRect(fx, fy, fw, ft, frame);
            canvas.fillRect(fx, fy + fh - ft, fw, ft, frame);
            canvas.fillRect(fx, fy, ft, fh, frame);
            canvas.fillRect(fx + fw - ft, fy, ft, fh, frame);
        }

        int leftEye = (eyeCx[0] < eyeCx[1]) ? eyeCx[0] : eyeCx[1];
        int rightEye = (eyeCx[0] > eyeCx[1]) ? eyeCx[0] : eyeCx[1];
        canvas.fillRect(leftEye + 10, cy - 1,
                        rightEye - leftEye - 20, GLASSES_FRAME_PX, frame);

        // Keep the pig's nose readable: the front lens overlaps the snout, so
        // restamp the two nostrils on top (matches drawDirectSnout geometry).
        const int snCx = bx + (faceRight ? 38 : 10) + 12;
        const int nbias = faceRight ? 1 : -1;
        const int nY = ((by + 2 + 10 + 7) / 2) * 2;   // snoutY=drawY+10, +7 centre
        canvas.fillRect(((snCx - 5 + nbias) / 2) * 2, nY, 4, 4, bg);
        canvas.fillRect(((snCx + 1 + nbias) / 2) * 2, nY, 4, 4, bg);
    }

    // ==[ SCAR ]== bump-mapped vertical on right cheek — only visible facing RIGHT
    if (faceRight && keyLight.tint != 0) {
        constexpr int FAT = 4;
        float pcx = bx + 36.0f, pcy = drawY + 20.0f;
        float lx = keyLight.x - pcx, ly = keyLight.y - pcy;
        float len = sqrtf(lx * lx + ly * ly);
        if (len < 1.0f) len = 1.0f;
        lx /= len; ly /= len;

        for (int i = 0; i < kScarCount; i++) {
            int sx = bx + kScarCells[i].col * FAT;
            int sy = drawY + kScarCells[i].row * FAT;
            uint16_t base = fastReadPx(canvas, sx, sy);
            if (isNearBG(base)) continue;

            float dot = kScarCells[i].nx * lx + kScarCells[i].ny * ly;
            uint16_t bumped = base;
            if (dot > 0.0f)
                bumped = screenBlend565(base, keyLight.tint, (uint8_t)(dot * 0.55f * 255));
            else
                bumped = lerpColor565_8(base, RP::DEEP, (uint8_t)(-dot * 0.45f * 255));
            canvas.fillRect(sx, sy, FAT, FAT, bumped);
            uint16_t scarTint = Display::lerpColor565(bumped, RP::WARM, 0.20f);
            canvas.fillRect(sx + 1, sy + 1, FAT - 2, FAT - 2, scarTint);
        }
    }

    // ==[ CIGARETTE ]== inner end IN the mouth, extends outward past snout
    // Attached to lower lip — bobs down when mouth opens (talking)
    // RIGHT: mouthX ≈ bx+48, snout edge bx+58, cig runs bx+48..bx+70 (paper+ember)
    // LEFT:  mouthX ≈ bx+22, snout edge bx+14, cig runs bx+2..bx+24 (ember+paper)
    int snoutX = faceRight ? (bx + 38) : (bx + 10);
    int mouthX = ((snoutX + 10) / 2) * 2;
    int mouthTop = ((drawY + 26) / 2) * 2;
    bool cigTalk = pose.talking && ((now / 200) & 1);
    int lipH = cigTalk ? 8 : 4;
    int cigY = ((mouthTop + lipH) / 2) * 2;  // lower lip, snapped
    if (faceRight) {
        int cigX = mouthX + 2;               // inner end at mouth corner
        canvas.fillRect(cigX, cigY, 18, 2, RP::WALL_NEAR);      // paper through snout, out
        canvas.fillRect(cigX + 18, cigY, 4, 2, RP::WARM);       // ember at tip
        cachedEmberX = (cigX + 20) & ~3;
    } else {
        int cigX = mouthX - 20;              // ember tip far left
        canvas.fillRect(cigX + 4, cigY, 18, 2, RP::WALL_NEAR);  // paper through snout, out
        canvas.fillRect(cigX, cigY, 4, 2, RP::WARM);             // ember at tip
        cachedEmberX = (cigX + 2) & ~3;
    }
    cachedEmberY = cigY;

    // ==[ HANDS ]== sitting-pose hands on counter, wipe arm on front side
    const int pawW = 16, pawH = 10;
    int pawY = (by + 30) & ~1;  // sitting pose convention, 2px snap
    bool fr = faceRight;
    int backPawX  = (fr ? (bx + 20) : (bx + 34)) & ~1;
    int frontPawX = (fr ? (bx + 42) : (bx + 12)) & ~1;

    int armY = kR5_BarTopY - 4;
    uint32_t workPhase = (now - barmanVisibleSince) % 7000u;
    static constexpr uint32_t kWipeStartMs = 1800u;
    static constexpr uint32_t kWipeReachMs = 300u;
    static constexpr uint32_t kWipeSweepMs = 1600u;
    static constexpr uint32_t kWipeRetractMs = 300u;
    static constexpr uint32_t kWipeEndMs =
        kWipeStartMs + kWipeReachMs + kWipeSweepMs + kWipeRetractMs;
    bool wiping = workPhase >= kWipeStartMs && workPhase < kWipeEndMs;
    uint32_t wipePhase = wiping ? workPhase - kWipeStartMs : 0u;
    int bodyL = bx + 8, bodyR = bx + 64;

    // Back hand always visible (resting on counter)
    drawFilledUHand(canvas, pal.bodyFill, bg, backPawX, pawY, fr, pawW, pawH, keyLight, 1.5f);
    drawHandBridge(canvas, pal.bodyFill, backPawX, pawY, fr, pawW);

    if (wiping) {
        constexpr int kWipeHandW = 12;
        constexpr int kWipeHandH = 10;
        constexpr int kRagW = 8;
        constexpr int kWipeReach = 3 * kRoomPX;
        int wipeNearX = fr ? bodyR : bodyL - kRagW;
        int wipeFarX = fr ? wipeNearX + kWipeReach : wipeNearX - kWipeReach;
        int counterFarX = fr ? barBx + barBw - kRagW : barBx;
        if (fr && wipeFarX > counterFarX) wipeFarX = counterFarX;
        if (!fr && wipeFarX < counterFarX) wipeFarX = counterFarX;

        int restHandX = frontPawX;
        int restRagX = fr ? restHandX + 8 : restHandX - 4;
        float ragXf = (float)restRagX;
        auto smooth01 = [](float t) {
            if (t < 0.0f) t = 0.0f;
            if (t > 1.0f) t = 1.0f;
            return t * t * (3.0f - 2.0f * t);
        };
        if (wipePhase < kWipeReachMs) {
            float t = smooth01((float)wipePhase / (float)kWipeReachMs);
            ragXf += ((float)wipeNearX - ragXf) * t;
        } else if (wipePhase < kWipeReachMs + kWipeSweepMs) {
            float t = (float)(wipePhase - kWipeReachMs) / (float)kWipeSweepMs;
            float outAndBack = t < 0.5f ? t * 2.0f : 2.0f - t * 2.0f;
            outAndBack = smooth01(outAndBack);
            ragXf = (float)wipeNearX +
                ((float)wipeFarX - (float)wipeNearX) * outAndBack;
        } else {
            float t = smooth01((float)(wipePhase - kWipeReachMs - kWipeSweepMs) /
                               (float)kWipeRetractMs);
            ragXf = (float)wipeNearX +
                ((float)restRagX - (float)wipeNearX) * t;
        }
        int ragX = ((int)lroundf(ragXf) / kPigPX) * kPigPX;
        int wipeHandX = fr ? ragX - 8 : ragX + 4;
        int handAttachX = fr ? wipeHandX : wipeHandX + kWipeHandW;
        if (fr && handAttachX > bodyR)
            canvas.fillRect(bodyR, armY, handAttachX - bodyR, kRoomPX, pal.bodyFill);
        if (!fr && handAttachX < bodyL)
            canvas.fillRect(handAttachX, armY, bodyL - handAttachX, kRoomPX, pal.bodyFill);

        // Hand and rag never disappear into the torso: the whole stroke stays
        // between the outward body edge and its matching end of the counter.
        drawFilledUHand(canvas, pal.bodyFill, bg, wipeHandX, pawY, fr,
                        kWipeHandW, kWipeHandH, keyLight, 1.5f);
        drawHandBridge(canvas, pal.bodyFill, wipeHandX, pawY, fr, kWipeHandW);
        canvas.fillRect(ragX, armY, kRagW, kRoomPX, RP::FILL);
    } else {
        // Front hand and rag rest together, so pickup/release has no hard cut.
        drawFilledUHand(canvas, pal.bodyFill, bg, frontPawX, pawY, !fr, pawW, pawH, keyLight, 1.5f);
        drawHandBridge(canvas, pal.bodyFill, frontPawX, pawY, !fr, pawW);
        int restRagX = fr ? frontPawX + 8 : frontPawX - 4;
        canvas.fillRect(restRagX, armY, 8, kRoomPX, RP::FILL);
    }
}

int Barman::smokeEmberX() { return cachedEmberX; }
int Barman::smokeEmberY() { return cachedEmberY; }

// ==[ DIALOGUE API ]==

static bool queueBarmanResponse(BarmanResponseCue cue, uint32_t now,
                                uint32_t delayMin, uint32_t delayMax) {
    if (Barman::isSpeaking(now)) return false;

    // A live cat cue can replace a generic Pancetta reply, but repeated frame
    // calls and generic chatter cannot keep pushing Pig's response into future.
    if (barmanWaitingToRespond) {
        if (barmanResponseCue == BarmanResponseCue::CAT_PIG ||
            cue == BarmanResponseCue::PANCETTA)
            return false;
    }

    barmanWaitingToRespond = true;
    barmanResponseCue = cue;
    barmanTriggerAt = now;
    const uint32_t spread = delayMax > delayMin ? delayMax - delayMin : 0;
    barmanResponseDelay = delayMin + (spread ? esp_random() % spread : 0);
    barmanBubbleText[0] = '\0';
    barmanFourthWall = false;
    return true;
}

void Barman::onPancettaSpoke(uint32_t now) {
    queueBarmanResponse(BarmanResponseCue::PANCETTA, now,
                        BARMAN_RESPONSE_DELAY_MIN,
                        BARMAN_RESPONSE_DELAY_MAX);
}

void Barman::onCatSpoke(uint32_t now) {
    if (!queueBarmanResponse(BarmanResponseCue::CAT_PIG, now, 450, 850))
        return;

    barmanCatFocusUntil = now + barmanResponseDelay + BARMAN_BUBBLE_DURATION;
    barmanEyeLook = PigEyeLook::FRONT_DOWN;
    barmanEyeTimer = now;
    barmanEyeDuration = barmanResponseDelay + BARMAN_BUBBLE_DURATION;
}

void Barman::onItemDropped(uint8_t itemId, uint32_t contextOrdinal, uint32_t now) {
    if (itemId != ItemDrops::getKHorseItemId()) return;

    // Award-time ordinal survives a backed-up reveal queue. Falling back keeps
    // direct callers sane while the queued path stays historically accurate.
    uint32_t drops = contextOrdinal ? contextOrdinal : ItemDrops::getKHorseDropCount();
    if (drops <= 1) {
        setBarmanBubble("말이 왔다.\n...horse? ...barn?", now, false);
    } else if (drops == 2) {
        setBarmanBubble("말 두 번.\nBarn. Almost.", now, false);
    } else if (drops == 3) {
        setBarmanBubble("Subtitles work.\nTrust real XP.", now, true);
    } else if (BARMAN_KH_ADVICE_COUNT > 0) {
        const char* advice = BARMAN_KH_ADVICE[(drops - 4) % BARMAN_KH_ADVICE_COUNT];
        setBarmanBubble(advice, now, false);
    }
}

void Barman::clearDialogue() {
    barmanBubbleText[0] = '\0';
    barmanWaitingToRespond = false;
    barmanResponseCue = BarmanResponseCue::NONE;
    barmanFourthWall = false;
    barmanCatFocusUntil = 0;
}

bool Barman::isSpeaking(uint32_t now) {
    return barmanBubbleText[0] != '\0' &&
           (now - barmanSpokeAt < BARMAN_BUBBLE_DURATION);
}

bool Barman::drawBubble(M5Canvas& canvas, uint32_t now) {
    // process delayed response — barman takes a beat before replying
    if (barmanWaitingToRespond) {
        if (now - barmanTriggerAt >= barmanResponseDelay) {
            barmanWaitingToRespond = false;
            bool fourthWall = false;
            const BarmanResponseCue cue = barmanResponseCue;
            barmanResponseCue = BarmanResponseCue::NONE;
            const char* phrase = cue == BarmanResponseCue::CAT_PIG
                ? pickBarmanCatPhrase()
                : pickBarmanPhrase(fourthWall);
            if (phrase && phrase[0]) {
                strncpy(barmanBubbleText, phrase, sizeof(barmanBubbleText) - 1);
                barmanBubbleText[sizeof(barmanBubbleText) - 1] = '\0';
                barmanSpokeAt = now;
                barmanFourthWall = fourthWall;
            }
        }
    }

    // expire bubble
    if (barmanBubbleText[0] && (now - barmanSpokeAt >= BARMAN_BUBBLE_DURATION)) {
        barmanBubbleText[0] = '\0';
        barmanFourthWall = false;
    }

    if (!barmanBubbleText[0]) return false;

    // barman nose position — same body geometry as drawBody
    int bx = cachedBx;
    int by = cachedBy;
    int noseX = cachedFaceRight ? (bx + 57) : (bx + 15);
    int noseY = by + 21;

    // flip bubble to opposite side from pig's bubble
    bool pigOnRight = (pigX + 36 >= SCREEN_WIDTH / 2);

    // Set the Korean-capable font for either unlabelled language, draw, restore.
    canvas.setFont(&fonts::efontKR_12);
    canvas.setTextSize(1);
    Mood::drawBubbleAt(canvas, barmanBubbleText, bx, bx + 72, by, noseX, noseY, pigOnRight);
    canvas.setFont(&fonts::Font0);
    canvas.setTextSize(1);
    return true;
}
