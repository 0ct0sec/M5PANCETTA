/**
 * Geiger scan view - shared RAD/THRU UI bits.
 *
 * ==[ THRU MAP ]== no radio work here. pixels only.
 */

#include "geiger_scan_view.h"
#include "geiger_scan_math.h"
#include "../gfx/gfx.h"
#include "../util/bearing.h"
#include <Arduino.h>
#include <math.h>
#include <string.h>
#if defined(ESP32)
#include <esp_heap_caps.h>
#endif

namespace GeigerScanView {

static constexpr int kFreshMax = 16;
static constexpr int kPointDrawMax = 128;
static constexpr int kCloudInspectMax = Bearing::RF_POINT_MAX;
static constexpr int kThermalCols = 32;
static constexpr int kThermalRows = 14;
static constexpr uint32_t kThermalFadeMs = 70;
static constexpr uint32_t kRfFreshFullMs = 3500;
static constexpr uint32_t kRfFreshGoodMs = 9000;
static constexpr uint32_t kRfFreshWeakMs = 18000;
static constexpr uint32_t kRfFreshHoldMs = 30000;
static constexpr uint8_t kCsiVisualMinQuality = 16;
static constexpr uint8_t kCsiVisualMinStability = 8;
static constexpr uint32_t kCsiVisualMaxAgeMs = 4200u;
static constexpr uint8_t kCsiEvidenceMinQuality = 24;
static constexpr uint8_t kCsiEvidenceMinStability = 14;
static constexpr uint32_t kCsiEvidenceMaxAgeMs = 3000u;
static constexpr uint8_t kCsiUsableStrengthWeak = 24;
static constexpr uint8_t kCsiUsableTurbulenceThreshold = 130;
static constexpr uint8_t kThroughBrightnessBoost = 4;

static const char* estimateStateLabel(const ThroughTarget& target) {
    if (target.locked) return "LOCK";
    if (!target.tracker) return "SEEK";
    switch (target.tracker->estimateState) {
        case Bearing::EstimateState::COARSE: return "COARSE";
        case Bearing::EstimateState::AMBIG: return "AMBIG";
        case Bearing::EstimateState::LOCK: return "LOCK";
        case Bearing::EstimateState::SEEK:
        default:
            return "SEEK";
    }
}
static constexpr float kDegToRad = 0.017453292519943295f;

static uint8_t thermalMap[kThermalRows][kThermalCols] = {};
static uint32_t thermalKey = 0;
static uint32_t thermalLastFade = 0;
static int thermalShiftX256 = 0;
static int thermalShiftY256 = 0;
static uint32_t thermalShiftLastMs = 0;

static uint32_t mix32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

static int clampInt(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static int boostThroughHeat(int heat, int maxOut) {
    return clampInt(heat * (int)kThroughBrightnessBoost, 0, maxOut);
}

static int absInt(int v) {
    return v < 0 ? -v : v;
}

static bool isBleScope(const ThroughTarget& t) {
    return t.scope && strncmp(t.scope, "BLE", 3) == 0;
}

static uint8_t signalDepthForRssi(int rssi, bool ble) {
    rssi = clampInt(rssi, -95, -25);
    if (ble) {
        if (rssi >= -45) return (uint8_t)map((long)rssi, -45, -25, 6, 1);
        if (rssi >= -70) return (uint8_t)map((long)rssi, -70, -45, 24, 6);
        return (uint8_t)map((long)rssi, -95, -70, 99, 24);
    }

    if (rssi >= -45) return (uint8_t)map((long)rssi, -45, -25, 4, 1);
    if (rssi >= -70) return (uint8_t)map((long)rssi, -70, -45, 18, 4);
    return (uint8_t)map((long)rssi, -95, -70, 80, 18);
}

static uint8_t signalDepth(const ThroughTarget& t) {
    return signalDepthForRssi((int)t.rssi, isBleScope(t));
}

static const char* rangeBandForRssi(int rssi) {
    if (rssi >= -45) return "NEAR";
    if (rssi >= -65) return "MID";
    if (rssi >= -82) return "FAR";
    return "EDGE";
}

static const char* rangeBand(const ThroughTarget& t) {
    return rangeBandForRssi((int)t.rssi);
}

static int depthPermille(uint8_t signalDepth) {
    const float farT = sqrtf((float)clampInt((int)signalDepth - 1, 0, 98) / 98.0f);
    return clampInt((int)roundf((1.0f - farT) * 1000.0f), 0, 1000);
}

static int depthY(int plotY0, int plotY1, int depthT) {
    const int span = max(1, plotY1 - plotY0 - 6);
    return clampInt(plotY0 + 3 + (span * depthT) / 1000,
                    plotY0 + 2, plotY1 - 2);
}

static int depthHalfWidth(int plotW, int depthT) {
    const int farHalf = max(6, plotW / 12);
    const int nearHalf = max(farHalf + 1, plotW / 2 - 5);
    return clampInt(farHalf + ((nearHalf - farHalf) * depthT) / 1000,
                    farHalf, nearHalf);
}

static int targetDepthPermille(const ThroughTarget& t) {
    return clampInt((int)t.proximity, 0, 1000);
}

static int proximityRangeIndex(uint16_t proximity) {
    return clampInt(99 - ((int)proximity * 98) / 1000, 1, 99);
}

static bool lastKnownUsable(const ThroughTarget& t) {
    return t.lastKnownValid && t.tracker;
}

static int lastKnownRelativeDegrees(const ThroughTarget& t) {
    if (!t.tracker) return 0;
    return GeigerScanMath::anchoredRelativeDegrees(
        t.lastKnownHeadingDegX10, t.tracker->lastHeadingDegX10);
}

static bool radarAnchorUsable(const ThroughTarget& t) {
    return t.tracker && (t.radarAnchorValid || t.lastKnownValid);
}

static int freshWeight(uint32_t ageMs) {
    if (ageMs <= kRfFreshFullMs) return kFreshMax;
    if (ageMs <= kRfFreshGoodMs) return 12;
    if (ageMs <= kRfFreshWeakMs) return 7;
    if (ageMs <= kRfFreshHoldMs) return 3;
    return 0;
}

static bool targetStale(const ThroughTarget& t) {
    return freshWeight(t.ageMs) <= 0;
}

static bool csiVisualUsable(const ThroughTarget& t) {
    return t.csiValid && t.csiAgeMs <= kCsiVisualMaxAgeMs &&
           (int)t.csiQuality >= kCsiVisualMinQuality &&
           (int)t.csiStability >= kCsiVisualMinStability;
}

static bool csiEvidenceUsable(const ThroughTarget& t) {
    return t.csiValid && t.csiAgeMs <= kCsiEvidenceMaxAgeMs &&
           (int)t.csiQuality >= kCsiEvidenceMinQuality &&
           (int)t.csiStability >= kCsiEvidenceMinStability;
}

static int divRoundSigned(int num, int den) {
    if (den == 0) return 0;
    return (num >= 0) ? ((num + den / 2) / den)
                      : ((num - den / 2) / den);
}

static void drawFit(M5Canvas& canvas, const char* text, int x, int y,
                    int maxW, textdatum_t datum) {
    if (!text || maxW <= 0) return;

    char buf[48];
    strncpy(buf, text, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    size_t n = strlen(buf);
    while (n > 0 && canvas.textWidth(buf) > maxW) {
        buf[--n] = '\0';
    }

    canvas.setTextDatum(datum);
    canvas.drawString(buf, x, y);
    canvas.setTextDatum(textdatum_t::top_left);
}

static void drawDottedH(M5Canvas& canvas, int x, int y, int w, uint16_t c) {
    for (int xx = x; xx < x + w; xx += 3) canvas.drawPixel(xx, y, c);
}

static uint16_t sonarDotColor(uint16_t fg, uint16_t bg,
                              int heat, bool ghost) {
    heat = clampInt(heat, 0, 255);
    int intensity = 28 + (heat * 227) / 255;
    if (ghost) intensity = (intensity * 2) / 5;
    intensity = clampInt(intensity, ghost ? 22 : 44, ghost ? 120 : 255);
    return Gfx::lerpColor565_8(bg, fg, (uint8_t)intensity);
}

static void drawSonarDot(M5Canvas& canvas, int x, int y, int heat,
                         bool ghost, uint16_t fg, uint16_t bg) {
    canvas.drawPixel(x, y, sonarDotColor(fg, bg, heat, ghost));
}

static void drawGhostPixel(M5Canvas& canvas, int x, int y, uint16_t c) {
    if (((x ^ y) & 1) == 0) canvas.drawPixel(x, y, c);
}

static void drawDottedLine(M5Canvas& canvas, int x0, int y0,
                           int x1, int y1, int cadence, uint16_t c) {
    if (cadence < 2) cadence = 2;
    const int dx = absInt(x1 - x0);
    const int sx = x0 < x1 ? 1 : -1;
    const int dy = -absInt(y1 - y0);
    const int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    int x = x0;
    int y = y0;
    int n = 0;
    for (;;) {
        if ((n % cadence) == 0) canvas.drawPixel(x, y, c);
        if (x == x1 && y == y1) break;
        const int e2 = err * 2;
        if (e2 >= dy) {
            err += dy;
            x += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y += sy;
        }
        ++n;
    }
}

static void formatAge(char* out, size_t n, uint32_t ageMs) {
    if (!out || n == 0) return;
    if (ageMs < 10000) {
        snprintf(out, n, "%lu.%luS",
                 (unsigned long)(ageMs / 1000),
                 (unsigned long)((ageMs % 1000) / 100));
    } else if (ageMs < 60000) {
        snprintf(out, n, "%luS", (unsigned long)(ageMs / 1000));
    } else {
        snprintf(out, n, "%luM", (unsigned long)(ageMs / 60000));
    }
}

static void drawMiniBar(M5Canvas& canvas, int x, int y, int w, int value,
                        uint16_t fg, uint16_t bg) {
    value = clampInt(value, 0, 100);
    canvas.drawRect(x, y, w, 5, fg);
    const int fillW = ((w - 2) * value) / 100;
    if (fillW > 0) canvas.fillRect(x + 1, y + 1, fillW, 3, fg);
    if (fillW < w - 2) canvas.fillRect(x + 1 + fillW, y + 1,
                                       w - 2 - fillW, 3, bg);
}

static void drawMiniPpsTrace(M5Canvas& canvas, int x, int y, int w, int h,
                             const uint16_t* samples, uint8_t count,
                             uint16_t fg, uint16_t bg) {
    if (!samples || count < 2u || w < 12 || h < 8) return;

    uint16_t peak = 1u;
    for (uint8_t i = 0; i < count; ++i) {
        if (samples[i] > peak) peak = samples[i];
    }

    canvas.fillRect(x, y, w, h, bg);
    canvas.drawRect(x, y, w, h, fg);
    const int plotW = w - 3;
    const int plotH = h - 3;
    int prevX = x + 1;
    int prevY = y + h - 2 -
        ((int)samples[0] * plotH) / (int)peak;
    for (uint8_t i = 1; i < count; ++i) {
        const int px = x + 1 +
            ((int)i * plotW) / ((int)count - 1);
        const int py = y + h - 2 -
            ((int)samples[i] * plotH) / (int)peak;
        canvas.drawLine(prevX, prevY, px, py, fg);
        prevX = px;
        prevY = py;
    }
    canvas.fillRect(prevX - 1, prevY - 1, 2, 2, fg);
}

static void drawMetricStack(M5Canvas& canvas,
                            int x, int y, int w, int h,
                            uint16_t fg, uint16_t bg,
                            const ThroughTarget& t,
                            uint8_t signalDepth) {
    (void)signalDepth;
    if (w < 58 || h < 50) return;

    canvas.fillRect(x, y, w, h, bg);
    canvas.drawRect(x, y, w, h, fg);
    canvas.setTextColor(fg);
    canvas.setTextSize(1);
    canvas.setTextDatum(textdatum_t::top_left);

    char l[36];
    const int tx = x + 4;
    const int tw = w - 8;
    int row = y + 4;
    const int step = 11;
    const int metricBottom = y + h - 24;
    auto drawMetricLine = [&](const char* text) -> bool {
        if (!text || row + 8 > metricBottom) return false;
        drawFit(canvas, text, tx, row, tw, textdatum_t::top_left);
        row += step;
        return true;
    };

    if (t.evidenceLabel && t.evidenceLabel[0]) {
        // The stack is only ~12 glyphs wide. The help contract defines this
        // compact pair as 2.4GHz|5GHz, preserving both independent censuses.
        snprintf(l, sizeof(l), "%s %u|%u", t.evidenceLabel,
                 (unsigned)t.band24Count, (unsigned)t.band5Count);
        drawMetricLine(l);

        if (t.channelPpsValid) {
            if (t.channelPps >= 1000u) {
                snprintf(l, sizeof(l), "CH%u N%u %luK",
                         (unsigned)t.channel, (unsigned)t.channelCount,
                         (unsigned long)(t.channelPps / 1000u));
            } else {
                snprintf(l, sizeof(l), "CH%u N%u %luP",
                         (unsigned)t.channel, (unsigned)t.channelCount,
                         (unsigned long)t.channelPps);
            }
        } else {
            snprintf(l, sizeof(l), "CH%u N%u --P",
                     (unsigned)t.channel, (unsigned)t.channelCount);
        }
        drawMetricLine(l);
    } else {
        snprintf(l, sizeof(l), "RF %u|%u",
                 (unsigned)t.band24Count, (unsigned)t.band5Count);
        drawMetricLine(l);
    }

    snprintf(l, sizeof(l), "RNG %s", rangeBand(t));
    drawMetricLine(l);

    snprintf(l, sizeof(l), "C%02u P%02u H%02u",
             (unsigned)t.confidence,
             (unsigned)clampInt((int)t.proximity / 10, 0, 100),
             (unsigned)t.historyConfidence);
    drawMetricLine(l);

    snprintf(l, sizeof(l), "D%02u K%02u S%02u",
             (unsigned)t.historyDensity,
             (unsigned)t.historyConsistency,
             (unsigned)t.historyCadence);
    drawMetricLine(l);

    char age[12];
    formatAge(age, sizeof(age), t.ageMs);
    snprintf(l, sizeof(l), "AGE %s", age);
    drawMetricLine(l);

    if (!t.evidenceLabel || !t.evidenceLabel[0]) {
        snprintf(l, sizeof(l), "I%02u S%02u R%02u",
                 (unsigned)clampInt((int)t.motionHeat, 0, 100),
                 (unsigned)clampInt((int)t.stationaryConfidence, 0, 100),
                 (unsigned)clampInt((int)t.sceneMotionHeat, 0, 100));
        drawMetricLine(l);
    }

    if (t.ftmResponder) {
        if (t.ftmActive) {
            snprintf(l, sizeof(l), "FTM ACTIVE");
        } else if (t.ftmValid) {
            snprintf(l, sizeof(l), "FTM %luCM N%u",
                     (unsigned long)t.ftmDistanceCm,
                     (unsigned)t.ftmSampleCount);
        } else {
            snprintf(l, sizeof(l), "FTM READY");
        }
        drawMetricLine(l);
    }

    if (t.gpsRouteValid) {
        snprintf(l, sizeof(l), "G%u H%u R%uM",
                 (unsigned)t.gpsRouteSamples,
                 (unsigned)t.gpsHdopX10,
                 (unsigned)t.gpsStrongestRadiusM);
        drawMetricLine(l);
    }

    if (t.csiValid) {
        if (t.csiAgeMs > kCsiVisualMaxAgeMs) {
            snprintf(l, sizeof(l), "CSI OLD %luS",
                     (unsigned long)(t.csiAgeMs / 1000u));
            drawMetricLine(l);
        } else if (csiVisualUsable(t)) {
            snprintf(l, sizeof(l), "CSI Q%02u S%02u",
                     (unsigned)clampInt((int)t.csiQuality, 0, 100),
                     (unsigned)clampInt((int)t.csiStability, 0, 100));
            const bool drewCsi = drawMetricLine(l);

            if (drewCsi) {
                snprintf(l, sizeof(l), "D%02u F%02u V%02u",
                         (unsigned)clampInt((int)t.csiChannelChange, 0, 100),
                         (unsigned)clampInt((int)t.csiFrequencySpread, 0, 100),
                         (unsigned)clampInt((int)t.csiFade, 0, 100));
                drawMetricLine(l);
            }
        } else {
            snprintf(l, sizeof(l), "CSI LOW %02u",
                     (unsigned)clampInt((int)t.csiQuality, 0, 100));
            drawMetricLine(l);
        }
    } else if (t.csiWaiting) {
        snprintf(l, sizeof(l), "CSI WAIT");
        drawMetricLine(l);
    } else if (t.csiUnsupported || isBleScope(t)) {
        snprintf(l, sizeof(l), "CSI N/A");
        drawMetricLine(l);
    } else {
        snprintf(l, sizeof(l), "CSI OFF");
        drawMetricLine(l);
    }

    if (t.channelPpsHistory && t.channelPpsHistoryCount >= 2u) {
        const int traceY = clampInt(row + 2, y + 2, y + h - 24);
        const int traceH = y + h - traceY - 4;
        drawMiniPpsTrace(canvas, x + 5, traceY, w - 10, traceH,
                         t.channelPpsHistory, t.channelPpsHistoryCount,
                         fg, bg);
    } else {
        const int barY = clampInt(row + 3, y + 2, y + h - 22);
        drawMiniBar(canvas, x + 5, barY, w - 10,
                    clampInt((int)t.proximity / 10, 0, 100), fg, bg);
        drawMiniBar(canvas, x + 5, barY + 8, w - 10,
                    clampInt((int)t.confidence, 0, 100), fg, bg);
        if (barY + 16 <= y + h - 4 && csiVisualUsable(t)) {
            drawMiniBar(canvas, x + 5, barY + 16, w - 10,
                        clampInt((int)t.csiQuality, 0, 100), fg, bg);
        } else {
            drawMiniBar(canvas, x + 5, barY + 16, w - 10,
                        clampInt((int)t.historyConfidence, 0, 100), fg, bg);
        }
    }
}

static uint32_t hashText(uint32_t h, const char* text) {
    if (!text) return h ^ 0x9e3779b9u;
    while (*text) {
        h ^= (uint8_t)*text++;
        h *= 16777619u;
    }
    return h;
}

static uint32_t hashPointer(uint32_t h, const void* p) {
    if (!p) return h;
    const uintptr_t v = (uintptr_t)p;
    for (size_t i = 0; i < sizeof(v); ++i) {
        h ^= (uint8_t)((v >> (i * 8)) & 0xffu);
        h *= 16777619u;
    }
    return h;
}

static const char* stableScope(const ThroughTarget& t) {
    if (t.scope && strcmp(t.scope, "BLE?") == 0) return "BLE";
    return t.scope;
}

static uint32_t targetKey(const ThroughTarget& t) {
    uint32_t h = 2166136261u;
    h = hashText(h, stableScope(t));
    h = hashText(h, t.header);
    h = hashPointer(h, t.tracker);
    return h ? h : 1u;
}

static void fadeThermal(uint32_t now) {
    if (thermalLastFade == 0) {
        thermalLastFade = now;
        return;
    }

    uint32_t elapsed = now - thermalLastFade;
    if (elapsed < kThermalFadeMs) return;

    int steps = clampInt((int)(elapsed / kThermalFadeMs), 1, 8);
    int fade = steps * 4;
    for (int y = 0; y < kThermalRows; ++y) {
        for (int x = 0; x < kThermalCols; ++x) {
            thermalMap[y][x] = (thermalMap[y][x] > fade)
                ? (uint8_t)(thermalMap[y][x] - fade)
                : 0;
        }
    }
    thermalLastFade = now;
}

static void depositThermal(int cx, int cy, int heat, int motion, int spread) {
    if (cx < 0 || cx >= kThermalCols || cy < 0 || cy >= kThermalRows) return;

    const int boostedHeat = boostThroughHeat(heat, 255);
    const int motionBoost = clampInt(motion, 0, 100) / 5;
    const int extra = clampInt(spread, 0, 4);
    const int radius = 2 + extra;
    const int maxDist = 3 + extra;
    const int divisor = maxDist + 1;

    for (int dy = -radius; dy <= radius; ++dy) {
        for (int dx = -radius; dx <= radius; ++dx) {
            const int tx = cx + dx;
            const int ty = cy + dy;
            if (tx < 0 || tx >= kThermalCols ||
                ty < 0 || ty >= kThermalRows) continue;

            const int dist = absInt(dx) + absInt(dy);
            if (dist > maxDist) continue;

            const int weighted = ((boostedHeat + motionBoost) *
                                 clampInt((maxDist + 1) - dist, 1, divisor)) /
                                 divisor;
            const int current = thermalMap[ty][tx];
            const int target = clampInt(weighted, 0, 255);
            if (target > current) {
                const int rise = max(1, (target - current) / 2);
                thermalMap[ty][tx] = (uint8_t)clampInt(current + rise, 0, 255);
            }
        }
    }
}

static void depositThermal(int cx, int cy, int heat, int motion) {
    depositThermal(cx, cy, heat, motion, 0);
}

static void depositCsiThermal(int cx, int cy,
                              int csiQuality,
                              int csiFrequencySpread,
                              int csiChannelChange,
                              int csiFade,
                              int csiStability) {
    if (cx < 0 || cx >= kThermalCols || cy < 0 || cy >= kThermalRows) return;

    const int stability = clampInt(csiStability, 0, 100);
    const int baseIntensity = clampInt(
        csiQuality + stability / 2 - csiChannelChange / 3 - csiFade / 4,
        0, 220);
    const int intensity = boostThroughHeat(clampInt(baseIntensity, 0, 220), 255);
    if (intensity <= 0) return;

    const int radius = clampInt(1 + csiFrequencySpread / 40 +
                                csiChannelChange / 50, 1, 4);
    for (int dy = -radius; dy <= radius; ++dy) {
        for (int dx = -radius; dx <= radius; ++dx) {
            const int tx = cx + dx;
            const int ty = cy + dy;
            if (tx < 0 || tx >= kThermalCols ||
                ty < 0 || ty >= kThermalRows) continue;

            const int dist = absInt(dx) + absInt(dy);
            if (dist > radius) continue;
            const int deposit = clampInt(intensity - (dist * 45), 0, 255);
            const int current = thermalMap[ty][tx];
            if (deposit > current) {
                const int rise = max(1, (deposit - current) / 2);
                thermalMap[ty][tx] = (uint8_t)clampInt(current + rise, 0, 255);
            }
        }
    }
}

static int thermalCellX(int px, int plotX0, int plotW) {
    int rel = clampInt(px - plotX0, 0, plotW - 1);
    const int denom = (plotW > 1) ? (plotW - 1) : 1;
    return clampInt((rel * (kThermalCols - 1)) / denom,
                    0, kThermalCols - 1);
}

static int thermalCellY(int py, int plotY0, int plotH) {
    int rel = clampInt(py - plotY0, 0, plotH - 1);
    const int denom = (plotH > 1) ? (plotH - 1) : 1;
    return clampInt((rel * (kThermalRows - 1)) / denom,
                    0, kThermalRows - 1);
}

static void updateThermalTrace(const ThroughTarget& t,
                               int px0, int py0,
                               int plotX0, int plotY0,
                               int plotW, int plotH,
                               int strength, int conf, int fresh,
                               int history,
                               bool usableBearing,
                               bool paintContact,
                               uint32_t now) {
    uint32_t key = targetKey(t);
    if (key != thermalKey) {
        memset(thermalMap, 0, sizeof(thermalMap));
        thermalKey = key;
        thermalLastFade = now;
        thermalShiftX256 = 0;
        thermalShiftY256 = 0;
        thermalShiftLastMs = now;
    }

    fadeThermal(now);
    if (!paintContact) return;

    const int sceneMotion = clampInt((int)t.sceneMotionHeat, 0, 100);
    // px0/py0 already encode the RF bearing and depth. Observer translation
    // is applied once to the retained thermal surface in updateThermalShift();
    // applying it again at deposit time made contacts rubber-band.
    const int heatPx = clampInt(px0, plotX0, plotX0 + plotW - 1);
    const int heatPy = clampInt(py0, plotY0, plotY0 + plotH - 1);
    int heat = (strength * 5 + conf * 2 + fresh * 6 +
                sceneMotion * 2 +
                clampInt(history, 0, 100) * 4) / 19;
    if (!usableBearing) heat = (heat * 3) / 5;
    heat = clampInt(heat, 12, 100);

    const int tCellX = thermalCellX(heatPx, plotX0, plotW);
    const int tCellY = thermalCellY(heatPy, plotY0, plotH);
    depositThermal(tCellX, tCellY,
                   heat, sceneMotion);

    if (csiVisualUsable(t)) {
        depositCsiThermal(tCellX, tCellY,
                          clampInt((int)t.csiQuality, 0, 100),
                          clampInt((int)t.csiFrequencySpread, 0, 100),
                          clampInt((int)t.csiChannelChange, 0, 100),
                          clampInt((int)t.csiFade, 0, 100),
                          clampInt((int)t.csiStability, 0, 100));
    }
}

static int thermalAt(int x, int y) {
    x = clampInt(x, 0, kThermalCols - 1);
    y = clampInt(y, 0, kThermalRows - 1);
    return thermalMap[y][x];
}

static void updateThermalShift(const ThroughTarget& t,
                               int plotW, int plotH,
                               bool paintContact,
                               uint32_t nowMs) {
    const int motionCarry = clampInt((int)t.motionHeat, 0, 100);
    const int signedScanX = GeigerScanMath::motionComponent(
        t.scanX, t.motionScreenSign);
    const int signedScanY = GeigerScanMath::motionComponent(
        t.scanY, t.motionScreenSign);
    const int motionScanX = divRoundSigned(signedScanX * motionCarry, 100);
    const int motionScanY = divRoundSigned(signedScanY * motionCarry, 100);

    int desiredX = 0;
    int desiredY = 0;
    if (paintContact) {
        // World-relative parallax counters handset translation. Bearing and
        // range are already represented by the deposit coordinates.
        desiredX = clampInt(-motionScanX * 256 / 95, -768, 768);
        desiredY = clampInt(-motionScanY * 256 / 110, -640, 640);
    }

    uint32_t elapsedMs = thermalShiftLastMs == 0u
        ? 16u : nowMs - thermalShiftLastMs;
    if (elapsedMs > 250u) elapsedMs = 250u;
    thermalShiftLastMs = nowMs;
    const uint32_t responseMs = paintContact ? 100u : 180u;
    auto approach = [elapsedMs, responseMs](int current, int target) {
        if (current == target || elapsedMs == 0u) return current;
        if (elapsedMs >= responseMs) return target;
        const int delta = target - current;
        int step = static_cast<int>(
            static_cast<int64_t>(delta) * elapsedMs / responseMs);
        if (step == 0) step = delta > 0 ? 1 : -1;
        const int next = current + step;
        if ((delta > 0 && next > target) ||
            (delta < 0 && next < target)) {
            return target;
        }
        return next;
    };
    thermalShiftX256 = approach(thermalShiftX256, desiredX);
    thermalShiftY256 = approach(thermalShiftY256, desiredY);
    thermalShiftX256 = clampInt(thermalShiftX256, -plotW * 128, plotW * 128);
    thermalShiftY256 = clampInt(thermalShiftY256, -plotH * 128, plotH * 128);
}

static void drawThermalGrid(M5Canvas& canvas,
                            int plotX0, int plotY0,
                            int plotX1, int plotY1,
                            int plotW, int plotH,
                            uint16_t fg, uint16_t bg) {
    static constexpr int GRID_STEP = 4;
    const int seed = (int)(thermalKey ^ 0x51f15eedu);

    for (int y = plotY0 + 2; y <= plotY1 - 2; y += GRID_STEP) {
        for (int x = plotX0 + 2; x <= plotX1 - 2; x += GRID_STEP) {
            const int cellX = thermalCellX(x, plotX0, plotW);
            const int cellY = thermalCellY(y, plotY0, plotH);
            const int heat = thermalAt(cellX, cellY);
            const int gradX = thermalAt(cellX + 1, cellY) -
                              thermalAt(cellX - 1, cellY);
            const int gradY = thermalAt(cellX, cellY + 1) -
                              thermalAt(cellX, cellY - 1);
            const uint32_t h = mix32((uint32_t)seed ^
                                     ((uint32_t)cellX * 0x45d9f3bu) ^
                                     ((uint32_t)cellY * 0x9e3779b9u));
            const int staticDither = (int)(h & 0x0fu) - 7;
            const int shiftX = thermalShiftX256 + gradX * 5;
            const int shiftY = thermalShiftY256 + gradY * 4;
            const int px = clampInt(x + divRoundSigned(shiftX, 256),
                                    plotX0, plotX1);
            const int py = clampInt(y + divRoundSigned(shiftY, 256),
                                    plotY0, plotY1);
            const int dotHeat = clampInt(6 + (heat * 3) / 4 + staticDither, 0, 255);
            drawSonarDot(canvas, px, py, dotHeat, heat < (24 * (int)kThroughBrightnessBoost),
                         fg, bg);
        }
    }
}

enum CloudConfidenceTier {
    CloudTier_High = 0,
    CloudTier_Medium,
    CloudTier_Ghost
};

struct CloudPoint {
    int x = 0;
    int y = 0;
    int heat = 0;
    int score = 0;
    int relDeg = 0;
    bool ghost = false;
    CloudConfidenceTier tier = CloudTier_Medium;
    int spread = 0;
};

static int cloudHeatScale(CloudConfidenceTier tier) {
    switch (tier) {
        case CloudTier_High: return 112;
        case CloudTier_Medium: return 96;
        case CloudTier_Ghost: return 56;
        default: return 60;
    }
}

static int cloudSpreadForTier(CloudConfidenceTier tier) {
    switch (tier) {
        case CloudTier_High: return 0;
        case CloudTier_Medium: return 1;
        case CloudTier_Ghost: return 3;
        default: return 1;
    }
}

static CloudConfidenceTier classifyCloudTier(const ThroughTarget& t,
                                           int conf,
                                           int fresh,
                                           uint32_t ageMs,
                                           int relDeg,
                                           int tiltDelta,
                                           bool behind,
                                           bool sampleMoving) {
    const int history = clampInt((int)t.historyConfidence, 0, 100);
    const int consistency = clampInt((int)t.historyConsistency, 0, 100);
    const bool csiUsable = csiEvidenceUsable(t);
    const int csiQuality = clampInt((int)t.csiQuality, 0, 100);
    const int csiStability = clampInt((int)t.csiStability, 0, 100);
    const int csiSignal = csiUsable ? ((csiQuality + csiStability) / 2) : 0;
    const int csiTurbulence = clampInt((int)t.csiChannelChange +
                                       (int)t.csiFrequencySpread, 0, 200);
    int highReqConf = 76;
    int highReqFresh = 11;
    int highReqHistory = 66;
    int highReqConsistency = 62;
    int highReqTilt = 32;
    int medReqConf = 50;
    int medReqFresh = 7;
    int medReqHistory = 38;
    int medReqConsistency = 40;
    int medReqTilt = 62;

    if (csiUsable) {
        // CSI here has channel quality and temporal stability, not AoA. It
        // may demote contradictory evidence, but never promote a direction.
        if (csiSignal <= kCsiUsableStrengthWeak) {
            highReqConf += 10;
            highReqFresh += 2;
            highReqHistory += 10;
            highReqConsistency += 12;
            highReqTilt += 10;
            medReqConf += 8;
            medReqFresh += 2;
            medReqHistory += 8;
            medReqConsistency += 10;
            medReqTilt += 8;
        }

        if (csiTurbulence >= kCsiUsableTurbulenceThreshold) {
            highReqConf += 9;
            medReqConf += 7;
            highReqTilt += 6;
            medReqTilt += 6;
        }

        if ((csiQuality <= 14) || (csiStability <= 14)) {
            return CloudTier_Ghost;
        }
    }

    if (conf < 24 || fresh <= 0 || behind) return CloudTier_Ghost;
    if (absInt(tiltDelta) > 68 || ageMs > 24000u) return CloudTier_Ghost;
    if (sampleMoving && conf < 66) return CloudTier_Ghost;

    if (conf >= highReqConf && fresh >= highReqFresh &&
        history >= highReqHistory && consistency >= highReqConsistency &&
        !sampleMoving && absInt(relDeg) <= 62 &&
        absInt(tiltDelta) <= highReqTilt) {
        return CloudTier_High;
    }

    const int medRelTol = 100;
    const int medTiltTol = medReqTilt;

    if (conf >= medReqConf && fresh >= medReqFresh && history >= medReqHistory &&
        consistency >= medReqConsistency &&
        absInt(relDeg) <= medRelTol &&
        absInt(tiltDelta) <= medTiltTol) {
        return CloudTier_Medium;
    }

    return CloudTier_Ghost;
}

#if defined(ESP32)
static CloudPoint* cloudScratch = nullptr;

static CloudPoint* getCloudScratch() {
    if (!cloudScratch) {
        cloudScratch = static_cast<CloudPoint*>(heap_caps_malloc(
            sizeof(CloudPoint) * kPointDrawMax,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
    return cloudScratch;
}
#else
// Native/simulator builds do not own an ESP32 heap. Keep their deterministic
// fixed buffer while production spends these 4 KiB in PSRAM instead of the
// Core2 linker's last internal-DRAM page.
static CloudPoint cloudScratch[kPointDrawMax];
static CloudPoint* getCloudScratch() { return cloudScratch; }
#endif

static void insertCloudPoint(CloudPoint* pts, int& count,
                             const CloudPoint& p) {
    if (count < kPointDrawMax) {
        int pos = count;
        count++;
        while (pos > 0 && pts[pos - 1].score > p.score) {
            pts[pos] = pts[pos - 1];
            pos--;
        }
        pts[pos] = p;
        return;
    }

    if (p.score <= pts[0].score) return;

    int pos = 0;
    while (pos + 1 < count && pts[pos + 1].score < p.score) {
        pts[pos] = pts[pos + 1];
        pos++;
    }
    pts[pos] = p;
}

static int collectRfCloud(const ThroughTarget& t,
                          int plotX0, int plotY0,
                          int plotX1, int plotY1,
                          int plotW, int plotH,
                          CloudPoint* out,
                          uint32_t nowMs) {
    if (!t.tracker) return 0;

    const uint8_t available = Bearing::getRfPointCount(*t.tracker);
    if (available == 0) return 0;

    const int cx = plotX0 + plotW / 2;
    const bool bleScope = isBleScope(t);
    const uint16_t currentHeading = t.tracker->lastHeadingDegX10;
    const int currentElev = t.tracker->lastElevDegX10;
    const bool csiUsable = csiEvidenceUsable(t);
    const int csiQuality = clampInt((int)t.csiQuality, 0, 100);
    const int csiStability = clampInt((int)t.csiStability, 0, 100);
    const int csiSignal = csiUsable ? ((csiQuality + csiStability) / 2) : 0;
    const int csiTurbulence = clampInt((int)t.csiChannelChange +
                                       (int)t.csiFrequencySpread,
                                       0, 180);
    const int csiSpreadBoost = csiUsable ? (csiTurbulence / 45) : 0;
    int count = 0;

    const uint8_t inspect = (available > kCloudInspectMax)
        ? kCloudInspectMax
        : available;
    for (uint8_t i = 0; i < inspect; ++i) {
        Bearing::RfPoint p;
        if (!Bearing::getRfPointNewest(*t.tracker, i, p)) break;
        if (p.seenMs == 0) continue;

        uint32_t ageMs = nowMs - p.seenMs;
        if (ageMs > kRfFreshHoldMs) continue;

        const int fresh = freshWeight(ageMs);
        const int conf = clampInt((int)p.confidence, 0, 100);
        const int bearing = (conf >= 15) ? p.bearing : t.fallbackBearing;
        // Confidence changes spread/heat, never the estimated direction.
        const int relDeg = GeigerScanMath::relativeDegrees(
            p.headingDegX10, bearing, currentHeading);
        const int tiltDelta = clampInt(((int)p.elevDegX10 - currentElev) / 10,
                                       -80, 80);

        const uint8_t sampleDepth = signalDepthForRssi((int)p.rssi, bleScope);
        const int depthT = depthPermille(sampleDepth);
        const int half = depthHalfWidth(plotW, depthT);
        const int sampleY = depthY(plotY0, plotY1, depthT);
        const int elevSpan = clampInt(6 + (depthT * max(6, plotH / 3)) / 1000,
                                      6, max(7, plotH / 3));
        const int rangeSpread = map((long)depthT, 0, 1000, 1, 8);
        const int csiJitter = clampInt(rangeSpread + csiSpreadBoost, 1, 14);
        const uint32_t s = mix32(p.seenMs ^
                                 ((uint32_t)(uint8_t)p.rssi << 16) ^
                                 ((uint32_t)i * 0x9e3779b9u));
        const int jx = (((int)((s >> 5) & 0x0f) - 7) * csiJitter) / 7;
        const int jy = (((int)((s >> 11) & 0x0f) - 7) * csiJitter) / 8;
        const int sceneAmp = clampInt((int)t.sceneMotionHeat, 0, 100);

        CloudPoint cp;
        cp.tier = classifyCloudTier(t, conf, fresh, ageMs, relDeg, tiltDelta,
                                   p.behind || absInt(relDeg) > 105 ||
                                   absInt(tiltDelta) > 70 || conf < 25,
                                   p.moving);
        cp.spread = clampInt(cloudSpreadForTier(cp.tier) +
                             sceneAmp / 40 +
                             csiSpreadBoost, 0, 4);
        cp.heat = clampInt(((int)p.strength + fresh * 3 + conf / 3 +
                            depthT / 18 +
                            (csiUsable ? ((csiSignal - 60) / 2) : 0)) *
                           cloudHeatScale(cp.tier) / 100, 4, 180);
        cp.relDeg = relDeg;
        cp.x = clampInt(cx + (relDeg * half) / 95 + jx,
                        plotX0, plotX1);
        cp.y = clampInt(sampleY - (tiltDelta * elevSpan) / 80 + jy,
                        plotY0, plotY1);
        const int recency = ((inspect - (int)i) * 28) / inspect;
        cp.score = fresh * 24 + (int)p.strength * 4 + conf * 2 + recency -
                   absInt(relDeg) / 2 - absInt(tiltDelta) / 3 -
                   (int)(ageMs / 400) -
                   (csiUsable ? (csiTurbulence / 10) : 0) +
                   (csiUsable ? ((csiSignal - 50) / 5) : 0);
        if (cp.tier == CloudTier_Medium) cp.score -= 12;
        if (cp.tier == CloudTier_Ghost) cp.score -= 58;
        if (conf < 25) cp.score -= 40;
        if (p.behind || absInt(relDeg) > 105 || absInt(tiltDelta) > 70) {
            cp.score -= 96;
        }
        cp.ghost = cp.tier == CloudTier_Ghost;
        insertCloudPoint(out, count, cp);
    }

    return count;
}

void drawThroughScanner(M5Canvas& canvas,
                        int boxX, int boxY, int boxW, int boxH,
                        uint16_t fg, uint16_t bg,
                        const ThroughTarget& t) {
    const uint16_t scanBg = bg;
    const uint16_t scanFg = fg;

    canvas.drawRoundRect(boxX - 2, boxY - 2, boxW + 4, boxH + 4, 4, scanFg);
    canvas.fillRoundRect(boxX, boxY, boxW, boxH, 4, scanBg);
    canvas.setTextColor(scanFg);
    canvas.setTextSize(1);

    char header[56];
    snprintf(header, sizeof(header), "THRU %s", t.header ? t.header : "RF");
    drawFit(canvas, header, boxX + boxW / 2, boxY + 4, boxW - 8,
            textdatum_t::top_center);

    const bool wide = boxW >= 260;
    const int stackW = wide ? clampInt(boxW / 4, 72, 88) : 0;
    const int stackGap = stackW ? 4 : 0;
    const int scopeX = boxX + 6;
    const int scopeY = boxY + 20;
    const int scopeW = boxW - 12 - stackW - stackGap;
    const int scopeH = boxH - 32;
    const int stackX = scopeX + scopeW + stackGap;

    canvas.drawRect(scopeX, scopeY, scopeW, scopeH, scanFg);

    const int plotX0 = scopeX + 4;
    const int plotX1 = scopeX + scopeW - 5;
    const int plotY0 = scopeY + 8;
    const int plotY1 = scopeY + scopeH - 13;
    const int plotW = plotX1 - plotX0 + 1;
    const int plotH = plotY1 - plotY0 + 1;
    const int cx = scopeX + scopeW / 2;
    const int cy = plotY0 + plotH / 2;

    const int fresh = freshWeight(t.ageMs);
    const int strength = clampInt((int)t.proximity / 10, 0, 100);
    const int conf = clampInt((int)t.confidence, 0, 100);
    const int history = clampInt((int)t.historyConfidence, 0, 100);
    const uint8_t depth = signalDepth(t);
    const int depthT = targetDepthPermille(t);
    const bool usableBearing = t.locked && conf >= 15 && !t.behind;
    const bool staleContact = targetStale(t);
    const bool stablePosition = usableBearing && conf > 60 && !staleContact;
    const bool retainedPosition = lastKnownUsable(t) && !stablePosition;
    const int retainedRelDeg = retainedPosition
        ? lastKnownRelativeDegrees(t) : 0;
    // Bearing is the estimate; confidence controls heat/spread, not angle.
    const int relDeg = usableBearing
        ? GeigerScanMath::bearingDegrees(t.bearing)
        : 0;
    const int contactHalf = depthHalfWidth(plotW, depthT);
    const int px0 = cx + (relDeg * contactHalf) / 95;
    const int py0 = depthY(plotY0, plotY1, depthT);
    const uint32_t nowMs = millis();
    const bool paintContact = !staleContact && !(t.behind && t.locked) &&
                              !retainedPosition;
    CloudPoint* cloud = getCloudScratch();
    const int cloudCount = cloud
        ? collectRfCloud(t, plotX0, plotY0, plotX1, plotY1,
                         plotW, plotH, cloud, nowMs)
        : 0;

    updateThermalTrace(t, px0, py0, plotX0, plotY0, plotW, plotH,
                       strength, conf, fresh, history, usableBearing,
                       paintContact, nowMs);
    if (cloudCount > 0) {
        for (int i = 0; i < cloudCount; ++i) {
            const int xCell = thermalCellX(cloud[i].x, plotX0, plotW);
            const int yCell = thermalCellY(cloud[i].y, plotY0, plotH);
            int heat = cloud[i].heat;
            int spread = clampInt(cloud[i].spread, 0, 3);
            if (cloud[i].tier == CloudTier_Ghost) {
                heat = heat * 3 / 5;
                spread = clampInt(spread + 2, 0, 4);
            }
            if (cloud[i].tier == CloudTier_Medium) heat = heat * 9 / 10;
            const int finalHeat = clampInt(heat, 4, 180);

            depositThermal(xCell, yCell, finalHeat,
                           t.sceneMotionHeat, spread);

            if (cloud[i].tier == CloudTier_Ghost && t.sceneMotionHeat > 22) {
                const int foamPass = clampInt((int)(t.sceneMotionHeat / 55), 1, 2);
                for (int k = 0; k < foamPass; ++k) {
                    const int foamX = (k == 0) ? 1 : -1;
                    const int foamY = (k == 0) ? -1 : 1;
                    depositThermal(clampInt(xCell + foamX, 0, kThermalCols - 1),
                                   clampInt(yCell + foamY, 0, kThermalRows - 1),
                                   clampInt(finalHeat / (2 + k), 4, 120),
                                   t.sceneMotionHeat, spread + 1);
                }
            }
        }
    }
    updateThermalShift(t, plotW, plotH, paintContact, nowMs);
    drawThermalGrid(canvas, plotX0, plotY0, plotX1, plotY1, plotW, plotH,
                    scanFg, scanBg);

    if (retainedPosition && (nowMs % 1000u) < 720u) {
        const int retainedDepthT = clampInt((int)t.lastKnownProximity, 0, 1000);
        const int retainedHalf = depthHalfWidth(plotW, retainedDepthT);
        const int retainedTilt = clampInt(
            ((int)t.lastKnownElevDegX10 -
             (int)t.tracker->lastElevDegX10) / 10, -80, 80);
        const int retainedElevSpan = clampInt(
            6 + (retainedDepthT * max(6, plotH / 3)) / 1000,
            6, max(7, plotH / 3));
        int retainedX = clampInt(
            cx + (clampInt(retainedRelDeg, -105, 105) * retainedHalf) / 95,
            plotX0 + 2, plotX1 - 2);
        int retainedY = clampInt(
            depthY(plotY0, plotY1, retainedDepthT) -
                (retainedTilt * retainedElevSpan) / 80,
            plotY0 + 2, plotY1 - 2);
        int motionX = 0;
        int motionY = 0;
        const int motionScale = max(6, min(plotW, plotH) / 8);
        const int motionLimit = max(8, min(plotW, plotH) / 3);
        GeigerScanMath::retainedMotionPixels(
            t.lastKnownObserverX, t.lastKnownObserverY,
            t.tracker->observerPositionX, t.tracker->observerPositionY,
            t.tracker->lastHeadingDegX10, t.motionScreenSign,
            motionScale, motionLimit, motionX, motionY);
        retainedX = clampInt(
            retainedX + motionX, plotX0 + 2, plotX1 - 2);
        retainedY = clampInt(
            retainedY + motionY, plotY0 + 2, plotY1 - 2);
        drawGhostPixel(canvas, retainedX, retainedY, scanFg);
        drawGhostPixel(canvas, retainedX - 2, retainedY, scanFg);
        drawGhostPixel(canvas, retainedX + 2, retainedY, scanFg);
        drawGhostPixel(canvas, retainedX, retainedY - 2, scanFg);
        drawGhostPixel(canvas, retainedX, retainedY + 2, scanFg);
        char retainedLabel[18];
        snprintf(retainedLabel, sizeof(retainedLabel), "LKP %+d", retainedRelDeg);
        drawFit(canvas, retainedLabel,
                clampInt(retainedX + 4, plotX0 + 2, plotX1 - 42),
                clampInt(retainedY - 9, plotY0 + 1, plotY1 - 8),
                42, textdatum_t::top_left);
    }

    if (staleContact && !retainedPosition) {
        drawFit(canvas, "RF STALE", cx, cy - 5, scopeW - 16,
                textdatum_t::top_center);
        drawFit(canvas, "MOVE / RESCAN", cx, cy + 6, scopeW - 16,
                textdatum_t::top_center);
    } else if (t.behind && t.locked && !retainedPosition) {
        canvas.drawString("BEHIND", cx - 18, cy - 4);
        canvas.drawPixel(cx - 8, plotY1 - 3, scanFg);
        canvas.drawPixel(cx + 8, plotY1 - 3, scanFg);
    }

    char l[24];
    snprintf(l, sizeof(l), "%s", t.scope ? t.scope : "RF");
    drawFit(canvas, l, plotX0 + 2, plotY0 + 1, plotW / 2,
            textdatum_t::top_left);
    const int displayRssi = retainedPosition && t.lastKnownRssi > -127
        ? static_cast<int>(t.lastKnownRssi) : static_cast<int>(t.rssi);
    const char* displayRange = rangeBandForRssi(displayRssi);
    char left[36];
    if (retainedPosition) {
        snprintf(left, sizeof(left), "%dDB %s LKP %+d %lu.%luS",
                 displayRssi, displayRange, retainedRelDeg,
                 (unsigned long)(t.lastKnownAgeMs / 1000u),
                 (unsigned long)((t.lastKnownAgeMs % 1000u) / 100u));
    } else if (staleContact) {
        snprintf(left, sizeof(left), "%dDB %s STALE", t.rssi, rangeBand(t));
    } else if (t.behind && t.locked) {
        snprintf(left, sizeof(left), "%dDB %s BEHIND", t.rssi, rangeBand(t));
    } else if (usableBearing) {
        snprintf(left, sizeof(left), "%dDB %s %+d%c %u%% H%u",
                 t.rssi, rangeBand(t), relDeg, (char)0xF8,
                 (unsigned)conf, (unsigned)t.historyConfidence);
    } else {
        snprintf(left, sizeof(left), "%dDB %s %s %+d",
                 t.rssi, rangeBand(t), estimateStateLabel(t),
                 (int)t.trend);
    }

    const int statusY = scopeY + scopeH - 10;
    drawFit(canvas, left, plotX0, statusY, plotW, textdatum_t::top_left);

    if (stackW > 0) {
        drawMetricStack(canvas, stackX, scopeY, stackW, scopeH, scanFg, scanBg,
                        t, depth);
    }
}

static int radarRangePx(int distM, int rmax) {
    const float frac = sqrtf((float)clampInt(distM, 1, 99) / 99.0f);
    return 4 + (int)roundf(frac * (float)(rmax - 4));
}

static void radarProjectPolar(int cx, int originY, int rmax,
                              int relDeg, int distM, int* px, int* py) {
    if (!px || !py) return;
    const int fanDeg = clampInt(relDeg, -85, 85);
    const float angle = (float)fanDeg * kDegToRad;
    const int rangePx = radarRangePx(distM, rmax);
    *px = cx + (int)roundf(sinf(angle) * (float)rangePx);
    *py = originY - (int)roundf(cosf(angle) * (float)rangePx);
}

static void paintRadarFan(M5Canvas& canvas,
                          int innerX, int topY, int innerW, int bottomY,
                          int cx, int originY, int rmax, uint32_t now,
                          char originChar, uint16_t c) {
    drawDottedH(canvas, innerX + 2, topY + 1, innerW - 4, c);

    static const int rays[] = { -60, -30, 0, 30, 60 };
    for (int i = 0; i < (int)(sizeof(rays) / sizeof(rays[0])); ++i) {
        const float a = (float)rays[i] * kDegToRad;
        const int ex = cx + (int)roundf(sinf(a) * (float)rmax);
        const int ey = originY - (int)roundf(cosf(a) * (float)rmax);
        drawDottedLine(canvas, cx, originY,
                       clampInt(ex, innerX + 1, innerX + innerW - 2),
                       clampInt(ey, topY + 1, originY - 1), 4, c);
    }

    for (int ring = 1; ring <= 2; ++ring) {
        const int rr = (rmax * ring) / 3;
        for (int deg = -72; deg <= 72; deg += 12) {
            const float a = (float)deg * kDegToRad;
            canvas.drawPixel(cx + (int)roundf(sinf(a) * (float)rr),
                             originY - (int)roundf(cosf(a) * (float)rr),
                             c);
        }
    }

    const int pulse = 4 + (int)(((uint32_t)(now % 1200u) *
                                 (uint32_t)(rmax - 4)) / 1200u);
    for (int deg = -78; deg <= 78; deg += 6) {
        const float a = (float)deg * kDegToRad;
        canvas.drawPixel(cx + (int)roundf(sinf(a) * (float)pulse),
                         originY - (int)roundf(cosf(a) * (float)pulse),
                         c);
    }

    const int midY = topY + (originY - topY) / 2;
    canvas.setTextColor(c);
    canvas.setTextDatum(textdatum_t::top_left);
    canvas.drawString("<", innerX + 2, midY - 4);
    canvas.drawString(">", innerX + innerW - 8, midY - 4);
    canvas.setCursor(cx - 3, originY - 7);
    canvas.print(originChar);
}

static void radarLabelText(const ThroughTarget& t,
                           int relDeg, bool behind, uint8_t signalDepth,
                           char* out, size_t outN) {
    if (!out || outN == 0) return;
    char tag[4] = {'R', 'F', ' ', '\0'};
    if (t.scope && t.scope[0]) {
        tag[0] = t.scope[0];
        tag[1] = t.scope[1] ? t.scope[1] : ' ';
        tag[2] = t.scope[2] ? t.scope[2] : ' ';
    }
    if (behind) {
        snprintf(out, outN, "%.3s BK %s", tag, rangeBand(t));
    } else {
        snprintf(out, outN, "%.3s %+03d %s",
                 tag, clampInt(relDeg, -99, 99), rangeBand(t));
    }
    (void)signalDepth;
}

static void drawRadarCallout(M5Canvas& canvas,
                             int innerX, int innerW, int topY, int bottomY,
                             int px, int py, int cx,
                             const char* label,
                             uint16_t fg, uint16_t bg) {
    if (!label || !label[0]) return;

    const int boxH = 10;
    int boxW = canvas.textWidth(label) + 6;
    boxW = clampInt(boxW, 18, innerW - 6);

    const bool rightSlot = px < cx;
    const int lx = rightSlot ? innerX + innerW - boxW - 3 : innerX + 3;
    int ly = topY + 12;  // keep the scope label's 8px row clear
    if (py < topY + 18) ly = bottomY - boxH - 3;
    ly = clampInt(ly, topY + 2, bottomY - boxH - 1);

    const int anchorX = clampInt(px, lx + 1, lx + boxW - 2);
    const int anchorY = py < ly ? ly : ly + boxH - 1;
    canvas.drawLine(px, py, anchorX, anchorY, fg);
    canvas.fillRect(lx, ly, boxW, boxH, fg);
    canvas.setTextColor(bg);
    drawFit(canvas, label, lx + 3, ly + 1, boxW - 6, textdatum_t::top_left);
    canvas.setTextColor(fg);
}

static void drawCsiRadarHalo(M5Canvas& canvas,
                             int blipX, int blipY,
                             int innerX, int innerW,
                             int topY, int bottomY,
                             uint32_t now, uint16_t fg, uint16_t bg,
                             const ThroughTarget& t) {
    if (!csiVisualUsable(t) || blipX < innerX + 1 ||
        blipX > innerX + innerW - 2 ||
        blipY < topY + 1 || blipY > bottomY - 1) return;

    const int quality = clampInt((int)t.csiQuality, 0, 100);
    const int frequencySpread = clampInt((int)t.csiFrequencySpread, 0, 100);
    const int channelChange = clampInt((int)t.csiChannelChange, 0, 100);
    const int fade = clampInt((int)t.csiFade, 0, 100);
    const int stability = clampInt((int)t.csiStability, 0, 100);

    if (quality < kCsiVisualMinQuality) return;

    const int stableQuality = clampInt((quality * (50 + stability)) / 150, 5, 100);
    const int baseRadius = clampInt(5 + stableQuality / 18, 5, 14);
    const int radiusStep = 1 + (frequencySpread / 50);
    const int rings = clampInt(2 + stableQuality / 50, 2, 4);
    const int pulse = (int)(now % 320u);
    const uint16_t csiCol = Gfx::lerpColor565_8(bg, fg,
                                                 (uint8_t)clampInt(80 + stableQuality * 145 / 100,
                                                                   80, 230));

    for (int r = 0; r < rings; ++r) {
        const int rr = baseRadius + r * radiusStep;
        const int phase = (int)(((int)now / 70u) + r * 29) % 90;
        const int arcLen = 12 + (stableQuality + frequencySpread) / 7;
        const int arcStart = phase + r * 6;

        for (int i = 0; i < arcLen; ++i) {
            const int deg = arcStart + (i * 5) + (fade / 20) +
                            (channelChange / 25);
            const float a = (float)deg * kDegToRad;
            const int px = blipX + (int)roundf(sinf(a) * (float)rr);
            const int py = blipY - (int)roundf(cosf(a) * (float)rr);
            if (px <= innerX || px >= innerX + innerW - 1 ||
                py <= topY || py >= bottomY) continue;
            if (((i + r * 3 + (pulse / 40)) % 3) == 0) {
                canvas.drawPixel(px, py, csiCol);
            }
        }
    }

    if (fade < 60 && (now % 700u) < 260u) {
        const int pulseR = clampInt(baseRadius + (int)(pulse / 70), baseRadius, 30);
        canvas.drawCircle(blipX, blipY, pulseR, csiCol);
    }
}

void drawRadarScanner(M5Canvas& canvas,
                      int boxX, int boxY, int boxW, int boxH,
                      uint16_t fg, uint16_t bg,
                      const ThroughTarget& t) {
    const uint16_t scanBg = bg;
    const uint16_t scanFg = fg;

    canvas.drawRoundRect(boxX - 2, boxY - 2, boxW + 4, boxH + 4, 4, scanFg);
    canvas.fillRoundRect(boxX, boxY, boxW, boxH, 4, scanBg);
    canvas.setTextColor(scanFg);
    canvas.setTextSize(1);

    char header[56];
    snprintf(header, sizeof(header), "RAD %s", t.header ? t.header : "RF");
    drawFit(canvas, header, boxX + boxW / 2, boxY + 4, boxW - 8,
            textdatum_t::top_center);

    const bool wide = boxW >= 260;
    const int stackW = wide ? clampInt(boxW / 4, 72, 88) : 0;
    const int stackGap = stackW ? 4 : 0;
    const int scopeX = boxX + 6;
    const int scopeY = boxY + 20;
    const int scopeW = boxW - 12 - stackW - stackGap;
    const int scopeH = boxH - 32;
    const int stackX = scopeX + scopeW + stackGap;
    const int innerX = scopeX + 4;
    const int innerW = scopeW - 8;
    const int topY = scopeY + 8;
    const int bottomY = scopeY + scopeH - 13;
    const int cx = innerX + innerW / 2;
    const int originY = bottomY - 5;
    const int rmax = clampInt(min(innerW / 2 - 8, originY - topY - 2),
                              8, 96);
    const uint32_t now = millis();
    const int conf = clampInt((int)t.confidence, 0, 100);
    const int history = clampInt((int)t.historyConfidence, 0, 100);
    const bool staleContact = targetStale(t);
    const bool stablePosition = t.locked && conf > 60 && !t.behind &&
                                !staleContact;
    // RAD is a world-relative plot: hold the last qualified RF position
    // across THRU/RAD posture changes even when the optional THRU ghost is
    // disabled. Its angle still comes from the current IMU heading below.
    const bool anchoredPosition = radarAnchorUsable(t);
    const bool retainedPosition = anchoredPosition && !stablePosition;
    const bool liveBearing = t.locked && conf >= 15;
    const int bearing = liveBearing ? t.bearing : t.fallbackBearing;
    const int relDeg = anchoredPosition
        ? lastKnownRelativeDegrees(t)
        : (!liveBearing && t.seekHeadingValid && t.tracker)
            ? GeigerScanMath::seekRelativeDegrees(
                  t.seekHeadingDegX10, t.tracker->lastHeadingDegX10)
            : GeigerScanMath::bearingDegrees(bearing);
    const bool behind = GeigerScanMath::radarBehind(
        anchoredPosition, staleContact, t.behind && t.locked,
        liveBearing, relDeg);
    const uint8_t depth = anchoredPosition
        ? (uint8_t)proximityRangeIndex(t.lastKnownProximity)
        : (uint8_t)proximityRangeIndex(t.proximity);

    char originChar = '^';
    if (staleContact) {
        originChar = '?';
    } else if (t.motionHeat > 35) {
        const char frames[] = {'-', '\\', '|', '/'};
        originChar = frames[(now / 100) % 4];
    } else if (t.moving) {
        originChar = ((now % 500) < 250) ? '+' : '^';
    }

    canvas.drawRect(scopeX, scopeY, scopeW, scopeH, scanFg);
    paintRadarFan(canvas, innerX, topY, innerW, bottomY, cx, originY, rmax,
                  now, originChar, scanFg);

    int blipX = cx;
    int blipY = originY - 6;
    if (!behind) {
        radarProjectPolar(cx, originY, rmax, relDeg, depth, &blipX, &blipY);
        if (anchoredPosition) {
            int motionX = 0;
            int motionY = 0;
            GeigerScanMath::retainedMotionPixels(
                t.lastKnownObserverX, t.lastKnownObserverY,
                t.tracker->observerPositionX, t.tracker->observerPositionY,
                t.tracker->lastHeadingDegX10, t.motionScreenSign,
                max(6, rmax / 3), max(8, rmax / 2), motionX, motionY);
            blipX += motionX;
            blipY += motionY;
        }
    }
    blipX = clampInt(blipX, innerX + 2, innerX + innerW - 3);
    blipY = clampInt(blipY, topY + 2, bottomY - 3);
    if (retainedPosition) {
        drawCsiRadarHalo(canvas, blipX, blipY, innerX, innerW,
                         topY, bottomY, now, scanFg, scanBg, t);
        if ((now % 1000u) < 720u) {
            drawGhostPixel(canvas, blipX, blipY, scanFg);
            drawGhostPixel(canvas, blipX - 2, blipY, scanFg);
            drawGhostPixel(canvas, blipX + 2, blipY, scanFg);
            drawGhostPixel(canvas, blipX, blipY - 2, scanFg);
            drawGhostPixel(canvas, blipX, blipY + 2, scanFg);
        }
    } else if (staleContact) {
        drawFit(canvas, "RAD STALE", cx, topY + (bottomY - topY) / 2 - 5,
                innerW - 16, textdatum_t::top_center);
        drawFit(canvas, "MOVE / RESCAN", cx,
                topY + (bottomY - topY) / 2 + 7,
                innerW - 16, textdatum_t::top_center);
    } else if (behind) {
        drawCsiRadarHalo(canvas, blipX, blipY, innerX, innerW,
                         topY, bottomY, now, scanFg, scanBg, t);
        if ((now % 600) < 420) {
            canvas.drawString("V", blipX - 9, blipY - 4);
            canvas.drawString("V", blipX + 4, blipY - 4);
        }
    } else {
        drawCsiRadarHalo(canvas, blipX, blipY, innerX, innerW,
                         topY, bottomY, now, scanFg, scanBg, t);
        const uint32_t phase = now / 420;
        const int fresh = freshWeight(t.ageMs);
        const bool strongHit = t.locked && conf >= 60;
        const bool locateHit = t.locked && fresh >= 3;
        const bool historyStable = history >= 60 && t.historyDensity >= 65;
        if (!t.locked || conf < 40) {
            const int nearDepth = clampInt((int)depth - 8, 1, 99);
            const int farDepth = clampInt((int)depth + 14, nearDepth, 99);
            int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
            radarProjectPolar(cx, originY, rmax, relDeg, nearDepth, &x0, &y0);
            radarProjectPolar(cx, originY, rmax, relDeg, farDepth, &x1, &y1);
            drawDottedLine(canvas,
                           clampInt(x0, innerX + 1, innerX + innerW - 2),
                           clampInt(y0, topY + 1, bottomY - 1),
                           clampInt(x1, innerX + 1, innerX + innerW - 2),
                           clampInt(y1, topY + 1, bottomY - 1), 3, scanFg);
        }

        if (strongHit) {
            canvas.fillRect(blipX - 1, blipY - 1, 3, 3, scanFg);
            canvas.drawRect(blipX - 3, blipY - 3, 7, 7, scanFg);
            if (historyStable) {
                canvas.drawRect(blipX - 5, blipY - 5, 11, 11, scanFg);
            }
            if ((phase & 1u) == 0u) {
                canvas.drawFastHLine(blipX - 4, blipY, 9, scanFg);
                canvas.drawFastVLine(blipX, blipY - 4, 9, scanFg);
            }
        } else if (locateHit) {
            canvas.drawRect(blipX - 2, blipY - 2, 5, 5, scanFg);
            if ((phase & 1u) == 0u) canvas.drawPixel(blipX, blipY, scanFg);
        } else if (fresh >= 12 || conf >= 45) {
            canvas.fillRect(blipX - 1, blipY - 1, 2, 2, scanFg);
        } else if (fresh >= 3 || t.proximity > 100) {
            canvas.drawPixel(blipX, blipY, scanFg);
            if (((phase + (uint32_t)conf) & 3u) == 0u) {
                canvas.drawPixel(blipX + 1, blipY, scanFg);
            }
        } else {
            drawGhostPixel(canvas, blipX, blipY, scanFg);
        }
    }

    char scope[24];
    snprintf(scope, sizeof(scope), "%s", t.scope ? t.scope : "RF");
    drawFit(canvas, scope, innerX + 2, topY + 1, innerW / 2,
            textdatum_t::top_left);

    if (!staleContact || retainedPosition) {
        char label[28];
        if (retainedPosition) {
            snprintf(label, sizeof(label), "LKP %+03d %s",
                     clampInt(relDeg, -99, 99), rangeBand(t));
        } else {
            radarLabelText(t, relDeg, behind, depth, label, sizeof(label));
        }
        drawRadarCallout(canvas, innerX, innerW, topY, bottomY, blipX, blipY,
                         cx, label, scanFg, scanBg);
    }

    const int displayRssi = anchoredPosition && t.lastKnownRssi > -127
        ? static_cast<int>(t.lastKnownRssi) : static_cast<int>(t.rssi);
    const char* displayRange = rangeBandForRssi(displayRssi);
    char status[40];
    if (retainedPosition) {
        snprintf(status, sizeof(status), "%dDB %s LKP %+d %lu.%luS",
                 displayRssi, displayRange, relDeg,
                 (unsigned long)(t.lastKnownAgeMs / 1000u),
                 (unsigned long)((t.lastKnownAgeMs % 1000u) / 100u));
    } else if (staleContact) {
        snprintf(status, sizeof(status), "%dDB %s STALE", t.rssi, rangeBand(t));
    } else if (behind && t.locked) {
        snprintf(status, sizeof(status), "%dDB %s BEHIND", t.rssi, rangeBand(t));
    } else if (t.locked && conf >= 15) {
        snprintf(status, sizeof(status), "%dDB %s %+d %u%% H%u",
                 t.rssi, rangeBand(t), relDeg, (unsigned)conf,
                 (unsigned)t.historyConfidence);
    } else {
        snprintf(status, sizeof(status), "%dDB %s %s %+d",
                 t.rssi, rangeBand(t), estimateStateLabel(t),
                 (int)t.trend);
    }
    const int statusY = scopeY + scopeH - 10;
    drawFit(canvas, status, innerX, statusY, innerW,
            textdatum_t::top_left);

    if (stackW > 0) {
        drawMetricStack(canvas, stackX, scopeY, stackW, scopeH, scanFg, scanBg,
                        t, depth);
    }

}

}  // namespace GeigerScanView
