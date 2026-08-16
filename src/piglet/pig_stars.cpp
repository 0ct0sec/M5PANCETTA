// PigStars — night sky pixel stars
#include "pig_stars.h"
#include "../core/achievements.h"
#include "../hamlet.h"
#include "../modes/hunt.h"
#include "../ui/display.h"
#include "weather.h"
#include <M5Unified.h>

// ==[ STAR STATE ]==
struct Star {
    uint16_t x;
    uint8_t y;
    uint8_t baseSize;       // 1 = dim cell, 2 = bright cell
    uint8_t blinkSpeed;     // 0 = static, >0 = twinkler phase speed
    uint32_t fadeInStart;
};

static constexpr uint8_t MAX_STARS = 40;
static Star stars[MAX_STARS];
static uint8_t starCount = 0;
static uint32_t lastStarSpawn = 0;
static uint32_t nextSpawnDelay = 0;
static bool starsActive = false;
static uint32_t lastNightCheck = 0;
static bool cachedNightMode = false;

enum class SkyGraffiti : uint8_t {
    NONE,
    IDLE_13_LIARS,
    HUNT_FUCK_CH6,
};

static SkyGraffiti skyGraffiti = SkyGraffiti::NONE;
static HamletMode graffitiScene = HamletMode::IDLE;
static uint32_t graffitiUntil = 0;
static uint32_t nextGraffitiAt = 0;

static constexpr int SKY_GRID = 4;
static constexpr int GRAFFITI_GRID = SKY_GRID;
static constexpr int GRAFFITI_ADVANCE = 4 * GRAFFITI_GRID;
static constexpr int GRAFFITI_X = 96;
static constexpr int GRAFFITI_Y = 32;
static constexpr size_t GRAFFITI_MAX_CHARS = 8;
static_assert(sizeof("13 LIARS") - 1 <= GRAFFITI_MAX_CHARS,
              "idle sky graffiti exceeds its geometry contract");
static_assert(sizeof("FUCK CH6") - 1 <= GRAFFITI_MAX_CHARS,
              "hunt sky graffiti exceeds its geometry contract");
static_assert(GRAFFITI_X - 1 >= 0 &&
              GRAFFITI_X + (GRAFFITI_MAX_CHARS - 1) * GRAFFITI_ADVANCE +
                  2 * GRAFFITI_GRID + 1 <= SCREEN_WIDTH,
              "sky graffiti leaves the display horizontally");
static_assert(GRAFFITI_Y - 1 >= TOP_BAR_H &&
              GRAFFITI_Y + 4 * GRAFFITI_GRID + 1 <= SCREEN_HEIGHT,
              "sky graffiti leaves the display vertically");

static bool deadlineReached(uint32_t now, uint32_t deadline) {
    return (int32_t)(now - deadline) >= 0;
}

static uint8_t glyphRow(char c, uint8_t row) {
    static constexpr uint8_t F[5] = {0b111, 0b100, 0b110, 0b100, 0b100};
    static constexpr uint8_t U[5] = {0b101, 0b101, 0b101, 0b101, 0b111};
    static constexpr uint8_t C[5] = {0b111, 0b100, 0b100, 0b100, 0b111};
    static constexpr uint8_t K[5] = {0b101, 0b110, 0b100, 0b110, 0b101};
    static constexpr uint8_t H[5] = {0b101, 0b101, 0b111, 0b101, 0b101};
    static constexpr uint8_t L[5] = {0b100, 0b100, 0b100, 0b100, 0b111};
    static constexpr uint8_t I[5] = {0b111, 0b010, 0b010, 0b010, 0b111};
    static constexpr uint8_t A[5] = {0b010, 0b101, 0b111, 0b101, 0b101};
    static constexpr uint8_t R[5] = {0b110, 0b101, 0b110, 0b101, 0b101};
    static constexpr uint8_t S[5] = {0b111, 0b100, 0b111, 0b001, 0b111};
    static constexpr uint8_t ONE[5] = {0b010, 0b110, 0b010, 0b010, 0b111};
    static constexpr uint8_t THREE[5] = {0b110, 0b001, 0b110, 0b001, 0b110};
    static constexpr uint8_t SIX[5] = {0b011, 0b100, 0b111, 0b101, 0b111};
    if (row >= 5) return 0;
    switch (c) {
        case 'F': return F[row];
        case 'U': return U[row];
        case 'C': return C[row];
        case 'K': return K[row];
        case 'H': return H[row];
        case 'L': return L[row];
        case 'I': return I[row];
        case 'A': return A[row];
        case 'R': return R[row];
        case 'S': return S[row];
        case '1': return ONE[row];
        case '3': return THREE[row];
        case '6': return SIX[row];
        default: return 0;
    }
}

static bool glyphHas(char c, uint8_t row, uint8_t col) {
    return (glyphRow(c, row) & (1u << (2u - col))) != 0;
}

static void drawConstellationText(M5Canvas& canvas, const char* text,
                                  int x, int y, uint16_t fg) {
    uint16_t wire = Display::lerpColor565(fg, Display::getColorBG(), 0.58f);
    for (size_t i = 0; text[i] != '\0'; ++i) {
        int gx = x + (int)i * GRAFFITI_ADVANCE;
        for (uint8_t row = 0; row < 5; ++row) {
            for (uint8_t col = 0; col < 3; ++col) {
                if (!glyphHas(text[i], row, col)) continue;
                int px = gx + col * GRAFFITI_GRID;
                int py = y + row * GRAFFITI_GRID;
                if (col < 2 && glyphHas(text[i], row, col + 1))
                    canvas.fillRect(px, py, GRAFFITI_GRID * 2,
                                    GRAFFITI_GRID, wire);
                if (row < 4 && glyphHas(text[i], row + 1, col))
                    canvas.fillRect(px, py, GRAFFITI_GRID,
                                    GRAFFITI_GRID * 2, wire);
            }
        }
    }
    for (size_t i = 0; text[i] != '\0'; ++i) {
        int gx = x + (int)i * GRAFFITI_ADVANCE;
        for (uint8_t row = 0; row < 5; ++row) {
            for (uint8_t col = 0; col < 3; ++col) {
                if (!glyphHas(text[i], row, col)) continue;
                int px = gx + col * GRAFFITI_GRID;
                int py = y + row * GRAFFITI_GRID;
                canvas.fillRect(px, py, GRAFFITI_GRID, GRAFFITI_GRID, fg);
            }
        }
    }
}

static bool channelSixEarnedProfanity() {
    const ChannelStats* stats = Hunt::getChannelStats(6);
    return stats && stats->beaconCount >= 64 &&
           stats->pmkidHits == 0 && stats->handshakeHits == 0;
}

static void updateSkyGraffiti(uint32_t now) {
    HamletMode scene = Hamlet::getMode();
    if (!starsActive || (scene != HamletMode::IDLE && scene != HamletMode::HUNT)) {
        skyGraffiti = SkyGraffiti::NONE;
        return;
    }

    if (skyGraffiti != SkyGraffiti::NONE) {
        if (scene != graffitiScene || deadlineReached(now, graffitiUntil))
            skyGraffiti = SkyGraffiti::NONE;
        else
            return;
    }

    if (nextGraffitiAt == 0) {
        nextGraffitiAt = now + (uint32_t)random(45000, 90001);
        return;
    }
    if (!deadlineReached(now, nextGraffitiAt)) return;

    // One look per several minutes. The joke needs evidence before it gets ink.
    nextGraffitiAt = now + (uint32_t)random(240000, 480001);
    if (scene == HamletMode::HUNT && channelSixEarnedProfanity()) {
        skyGraffiti = SkyGraffiti::HUNT_FUCK_CH6;
    } else if (scene == HamletMode::IDLE &&
               Achievements::has(Achievement::FULL_CIRCUIT)) {
        skyGraffiti = SkyGraffiti::IDLE_13_LIARS;
    }
    if (skyGraffiti != SkyGraffiti::NONE) {
        graffitiScene = scene;
        graffitiUntil = now + 6500u;
    }
}

static void drawSkyGraffiti(M5Canvas& canvas, uint16_t fg) {
    switch (skyGraffiti) {
        case SkyGraffiti::IDLE_13_LIARS:
            drawConstellationText(canvas, "13 LIARS", GRAFFITI_X, GRAFFITI_Y, fg);
            break;
        case SkyGraffiti::HUNT_FUCK_CH6:
            drawConstellationText(canvas, "FUCK CH6", GRAFFITI_X, GRAFFITI_Y, fg);
            break;
        default:
            break;
    }
}

// ==[ NIGHT DETECTION ]==
bool PigStars::isNightTime() {
    uint32_t now = millis();
    if (now - lastNightCheck < 60000 && lastNightCheck != 0) {
        return cachedNightMode;
    }
    lastNightCheck = now;

    // System time (NTP)
    time_t sysTime = time(nullptr);
    if (sysTime > 1704067200) {
        struct tm timeinfo;
        localtime_r(&sysTime, &timeinfo);
        uint8_t hour = timeinfo.tm_hour;
        cachedNightMode = (hour >= 20 || hour < 6);
        return cachedNightMode;
    }

    // M5 RTC fallback
    if (M5.Rtc.isEnabled()) {
        auto dt = M5.Rtc.getDateTime();
        if (dt.date.year >= 2024) {
            uint8_t hour = dt.time.hours;
            cachedNightMode = (hour >= 20 || hour < 6);
            return cachedNightMode;
        }
    }

    // no RTC, no NTP — assume night after 5min uptime
    cachedNightMode = (now > 300000);
    return cachedNightMode;
}

// ==[ STRATIFIED GRID PLACEMENT ]==
static void initStarPositions() {
    const uint8_t SKY_TOP = TOP_BAR_H + 1;
    const uint8_t SKY_BOT = (SCREEN_HEIGHT - BOTTOM_BAR_H - 42 - 5) - 2;  // PIG_Y - 2
    const uint8_t SKY_H = SKY_BOT - SKY_TOP;

    const uint8_t COLS = 10;
    const uint8_t ROWS = 8;
    const uint8_t CELL_W = SCREEN_WIDTH / COLS;
    const uint8_t CELL_H = SKY_H / ROWS;

    uint8_t idx = 0;
    for (uint8_t row = 0; row < ROWS && idx < MAX_STARS; row++) {
        for (uint8_t col = 0; col < COLS && idx < MAX_STARS; col++) {
            if (random(0, 100) < 25) continue;

            uint16_t cx = col * CELL_W + random(2, CELL_W - 1);
            uint8_t cy = SKY_TOP + row * CELL_H + random(1, CELL_H - 1);
            if (cx > SCREEN_WIDTH - 3) cx = SCREEN_WIDTH - 3;

            stars[idx].x = (uint16_t)((cx / SKY_GRID) * SKY_GRID);
            stars[idx].y = (uint8_t)((cy / SKY_GRID) * SKY_GRID);
            // baseSize is now a brightness class; every raster mark remains a
            // full scenery cell instead of changing between 1px and 2px dots.
            stars[idx].baseSize = (random(0, 100) < 24) ? 2 : 1;
            stars[idx].blinkSpeed = (random(0, 100) < 30) ? (uint8_t)random(8, 24) : 0;
            stars[idx].fadeInStart = 0;
            idx++;
        }
    }
    starCount = idx;
}

void PigStars::init() {
    starsActive = false;
    starCount = 0;
    lastNightCheck = 0;
    cachedNightMode = false;
    skyGraffiti = SkyGraffiti::NONE;
    graffitiUntil = 0;
    nextGraffitiAt = 0;
}

void PigStars::update() {
    uint32_t now = millis();

    if (Weather::isRaining()) {
        if (starsActive) {
            starsActive = false;
            starCount = 0;
        }
        return;
    }

    bool nightNow = isNightTime();

    if (nightNow && !starsActive) {
        starsActive = true;
        initStarPositions();
        for (uint8_t i = 0; i < starCount; i++) {
            stars[i].fadeInStart = now + random(0, 3000);
        }
        lastStarSpawn = now;
    } else if (!nightNow && starsActive) {
        starsActive = false;
        starCount = 0;
    }
    updateSkyGraffiti(now);
}

void PigStars::draw(M5Canvas& canvas, uint16_t fg) {
    if (!starsActive || starCount == 0) return;

    uint32_t now = millis();
    const uint16_t bg = Display::getColorBG();
    const uint16_t dim = Display::lerpColor565(fg, bg, 0.62f);
    const uint16_t mid = Display::lerpColor565(fg, bg, 0.30f);

    for (uint8_t i = 0; i < starCount; i++) {
        if (!deadlineReached(now, stars[i].fadeInStart)) continue;
        uint32_t age = now - stars[i].fadeInStart;
        uint8_t intensity = stars[i].baseSize > 1 ? 1 : 0;

        // Fade by intensity/time rather than changing physical pixel size.
        if (age < 500) {
            if (age < 140 && (((age / 55u) + i) & 1u) != 0u) continue;
            intensity = age < 320 ? 0 : (stars[i].baseSize > 1 ? 1 : 0);
        }

        if (age >= 500 && stars[i].blinkSpeed > 0) {
            uint32_t phase = (now * stars[i].blinkSpeed + i * 7919) % 8000;
            if (phase >= 3500 && phase < 4100) {
                intensity = 2;
            }
        }

        const int sx = stars[i].x;
        const int sy = stars[i].y;
        const uint16_t starColor = intensity == 0 ? dim : intensity == 1 ? mid : fg;
        canvas.fillRect(sx, sy, SKY_GRID, SKY_GRID, starColor);

        if (intensity == 2) {
            // Rare one-beat flare. All arms remain full 4px scenery cells.
            if (sx >= SKY_GRID)
                canvas.fillRect(sx - SKY_GRID, sy, SKY_GRID, SKY_GRID, dim);
            if (sx + SKY_GRID * 2 <= SCREEN_WIDTH)
                canvas.fillRect(sx + SKY_GRID, sy, SKY_GRID, SKY_GRID, dim);
            if (sy >= TOP_BAR_H + SKY_GRID)
                canvas.fillRect(sx, sy - SKY_GRID, SKY_GRID, SKY_GRID, dim);
            if (sy + SKY_GRID * 2 < SCREEN_HEIGHT - BOTTOM_BAR_H)
                canvas.fillRect(sx, sy + SKY_GRID, SKY_GRID, SKY_GRID, dim);
        }
    }
    drawSkyGraffiti(canvas, fg);
}
