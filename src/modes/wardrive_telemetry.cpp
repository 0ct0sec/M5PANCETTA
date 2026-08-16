/**
 * Wardrive telemetry tape — the cockpit's quiet instrument page.
 *
 * No synthetic bearing, no animated horizon, no second scan engine. The page
 * reports the navigation and scan evidence already owned by Wardrive, then
 * lets the panel and frame governor coast.
 */

#include "wardrive_telemetry.h"

#include "wardrive.h"
#include "wardrive_policy.h"
#include "wardrive_scene.h"
#include "../audio/sfx.h"
#include "../core/gps.h"
#include "../core/power.h"
#include "../ui/display.h"
#include "../ui/frame_presenter.h"
#include <stdio.h>
#include <string.h>

namespace WardriveTelemetry {
namespace {

bool visible = false;
bool redrawPending = true;
bool previousFix = false;
bool hadFixThisSession = false;
uint32_t lastRedrawMs = 0;
uint32_t nextFixWarningMs = 0;

void scheduleFixWarning(uint32_t nowMs) {
    nextFixWarningMs = nowMs + WardrivePolicy::FIX_WARNING_REPEAT_MS;
    if (nextFixWarningMs == 0) nextFixWarningMs = 1;
}

struct PositionView {
    bool fixed;
    bool fromC5;
    double latitude;
    double longitude;
    float altitudeMeters;
};

PositionView readPosition() {
    if (Wardrive::isUsingFreshC5WardriveCoords()) {
        return {true, true,
                Wardrive::getC5WardriveLatitude(),
                Wardrive::getC5WardriveLongitude(), 0.0f};
    }
    if (Wardrive::hasGPSFix()) {
        return {true, false, Wardrive::getLatitude(), Wardrive::getLongitude(),
                Wardrive::getAltitude()};
    }
    return {false, false, 0.0, 0.0, 0.0f};
}

void drawPanel(M5Canvas& canvas, int x, int y, int w, int h,
               uint16_t border, uint16_t fill) {
    canvas.fillRect(x, y, w, h, fill);
    canvas.drawRect(x, y, w, h, border);
    canvas.drawFastHLine(x + 4, y + 4, 12, border);
    canvas.drawFastHLine(x + w - 16, y + h - 5, 12, border);
}

void formatAge(char* out, size_t cap, uint32_t ageMs) {
    if (!out || cap == 0) return;
    if (ageMs == UINT32_MAX) {
        snprintf(out, cap, "--");
    } else if (ageMs < 10000u) {
        snprintf(out, cap, "%lu.%lus",
                 static_cast<unsigned long>(ageMs / 1000u),
                 static_cast<unsigned long>((ageMs % 1000u) / 100u));
    } else {
        snprintf(out, cap, "%lus",
                 static_cast<unsigned long>(ageMs / 1000u));
    }
}

void formatElapsed(char* out, size_t cap, uint32_t elapsedMs) {
    const uint32_t seconds = elapsedMs / 1000u;
    snprintf(out, cap, "%02lu:%02lu:%02lu",
             static_cast<unsigned long>(seconds / 3600u),
             static_cast<unsigned long>((seconds / 60u) % 60u),
             static_cast<unsigned long>(seconds % 60u));
}

const char* fixStateLabel(WardrivePolicy::TelemetryFixState state) {
    switch (state) {
        case WardrivePolicy::TelemetryFixState::LOCKED:       return "NAV LOCK";
        case WardrivePolicy::TelemetryFixState::ACQUIRING:    return "ACQUIRING";
        case WardrivePolicy::TelemetryFixState::INVALID_NMEA: return "NMEA BAD";
        case WardrivePolicy::TelemetryFixState::NO_UART_DATA: return "NO RX";
        case WardrivePolicy::TelemetryFixState::GPS_OFF:      return "GPS OFF";
        default:                                               return "NO FIX";
    }
}

void copyDisplaySsid(char* out, size_t cap, const char* ssid) {
    if (!out || cap == 0) return;
    if (!ssid || ssid[0] == '\0') {
        snprintf(out, cap, "<HIDDEN>");
        return;
    }
    size_t n = strnlen(ssid, cap - 1);
    for (size_t i = 0; i < n; ++i) {
        const unsigned char ch = static_cast<unsigned char>(ssid[i]);
        out[i] = (ch >= 32 && ch <= 126) ? static_cast<char>(ch) : '?';
    }
    out[n] = '\0';
}

} // namespace

void reset() {
    visible = false;
    redrawPending = true;
    previousFix = readPosition().fixed;
    hadFixThisSession = previousFix;
    lastRedrawMs = 0;
    nextFixWarningMs = 0;
    Power::setTargetFPSOverride(0);
    Display::setLowPowerDimmed(false);
}

void shutdown() {
    visible = false;
    redrawPending = true;
    previousFix = false;
    hadFixThisSession = false;
    lastRedrawMs = 0;
    nextFixWarningMs = 0;
    Power::setTargetFPSOverride(0);
    Display::setLowPowerDimmed(false);
}

bool isVisible() {
    return visible;
}

void setVisible(bool shouldShow) {
    if (visible == shouldShow) return;
    visible = shouldShow;
    redrawPending = true;
    lastRedrawMs = 0;
    nextFixWarningMs = 0;

    Power::setTargetFPSOverride(
        visible ? WardrivePolicy::TELEMETRY_TARGET_FPS : 0);
    Display::setLowPowerDimmed(visible);
    FramePresenter::invalidate();

    const bool fixed = readPosition().fixed;
    previousFix = fixed;
    if (fixed) hadFixThisSession = true;

    if (visible) {
        if (fixed) {
            SFX::play(SFX::GPS_FIX_LOCK);
        } else if (hadFixThisSession) {
            SFX::play(SFX::GPS_FIX_WARNING);
            scheduleFixWarning(millis());
        }
    } else {
        WardriveScene::resumeFrameClock();
    }
}

void toggle() {
    setVisible(!visible);
}

void update(uint32_t nowMs) {
    const bool fixed = readPosition().fixed;
    if (fixed) hadFixThisSession = true;

    if (visible && fixed != previousFix) {
        if (fixed) {
            SFX::play(SFX::GPS_FIX_LOCK);
            nextFixWarningMs = 0;
        } else if (hadFixThisSession) {
            SFX::play(SFX::GPS_FIX_WARNING);
            scheduleFixWarning(nowMs);
        }
        redrawPending = true;
    } else if (visible && !fixed && hadFixThisSession &&
               WardrivePolicy::warningDeadlineReached(nowMs, nextFixWarningMs)) {
        SFX::play(SFX::GPS_FIX_WARNING);
        scheduleFixWarning(nowMs);
    }
    previousFix = fixed;
}

void draw(M5Canvas& canvas, uint32_t nowMs) {
    if (!redrawPending && lastRedrawMs != 0 &&
        nowMs - lastRedrawMs < WardrivePolicy::TELEMETRY_REDRAW_MS) {
        return;
    }
    redrawPending = false;
    lastRedrawMs = nowMs;

    const uint16_t fg = Display::getColorFG();
    const uint16_t bg = Display::getColorBG();
    const uint16_t panel = Display::lerpColor565(bg, fg, 0.07f);
    const uint16_t dim = Display::lerpColor565(bg, fg, 0.46f);
    const uint16_t bright = Display::lerpColor565(bg, fg, 0.90f);
    const PositionView pos = readPosition();
    const auto fixState = WardrivePolicy::classifyTelemetryFixState(
        pos.fixed, Wardrive::isGPSRunning(), Wardrive::hasGPSData(),
        Wardrive::hasGPSNMEA());

    Wardrive::ScanTelemetry scan{};
    Wardrive::getScanTelemetry(scan);
    GPS::Diagnostics gps{};
    GPS::getDiagnostics(gps);

    canvas.fillSprite(bg);
    canvas.setTextDatum(TL_DATUM);
    canvas.setTextSize(1);
    canvas.setTextColor(dim);

    // Static instrument lattice: enough aircraft language to read as a sensor
    // tape, still snapped to Pancetta's two-color case-desk vocabulary.
    for (int x = 0; x < 320; x += 32) canvas.drawFastVLine(x, 0, 240, panel);
    for (int y = 0; y < 240; y += 24) canvas.drawFastHLine(0, y, 320, panel);

    canvas.fillRect(0, 0, 320, 26, panel);
    canvas.drawFastHLine(0, 25, 320, bright);
    canvas.setTextColor(bright);
    canvas.setCursor(7, 5);
    canvas.print("WARTHOG // SENSOR TAPE");
    canvas.setTextColor(dim);
    canvas.setCursor(7, 15);
    canvas.printf(Wardrive::isPaused()
                      ? "HOLD // NEW %05lu // ENGINE LIVE"
                      : "REC // NEW %05lu // 10HZ LOW POWER",
                  static_cast<unsigned long>(Wardrive::getSessionNewNets()));

    const bool c5Waiting = !pos.fixed && Wardrive::hasC5WardriveConnection() &&
                           !Wardrive::isGPSRunning();
    const char* state = c5Waiting ? "C5 WAIT" : fixStateLabel(fixState);
    const int stateW = static_cast<int>(strlen(state)) * 6 + 10;
    canvas.fillRect(315 - stateW, 4, stateW, 17, pos.fixed ? bright : dim);
    canvas.setTextColor(bg);
    canvas.setCursor(320 - stateW, 9);
    canvas.print(state);

    drawPanel(canvas, 5, 31, 310, 82, pos.fixed ? bright : dim, panel);
    canvas.setTextColor(dim);
    canvas.setCursor(12, 37);
    if (pos.fixed) {
        canvas.printf("SOURCE %s // FIX VALID", pos.fromC5 ? "C5 UART" : "CORE GPS");
    } else {
        canvas.printf("SOURCE NONE // %s", state);
    }

    char line[64];
    canvas.setTextColor(bright);
    canvas.setTextSize(2);
    canvas.setCursor(12, 51);
    if (pos.fixed) snprintf(line, sizeof(line), "LAT %+010.6f", pos.latitude);
    else snprintf(line, sizeof(line), "LAT --.------");
    canvas.print(line);
    canvas.setCursor(12, 72);
    if (pos.fixed) snprintf(line, sizeof(line), "LON %+011.6f", pos.longitude);
    else snprintf(line, sizeof(line), "LON ---.------");
    canvas.print(line);
    canvas.setTextSize(1);
    canvas.setTextColor(dim);

    char fixAge[12];
    formatAge(fixAge, sizeof(fixAge), pos.fromC5
        ? Wardrive::getC5WardriveCoordAgeMs() : GPS::getFixAgeMs());
    canvas.setCursor(12, 96);
    if (pos.fromC5) {
        canvas.printf("AGE %s  CORE SAT %u  NMEA %s", fixAge,
                      static_cast<unsigned>(gps.sats), gps.nmeaFresh ? "LIVE" : "--");
    } else if (pos.fixed) {
        canvas.printf("SAT %u  HDOP %.1f  AGE %s  ALT %.0fm",
                      static_cast<unsigned>(gps.sats), gps.hdop, fixAge,
                      pos.altitudeMeters);
    } else {
        canvas.printf("SAT %u  HDOP %.1f  LAST %s  ALT --",
                      static_cast<unsigned>(gps.sats), gps.hdop, fixAge);
    }

    drawPanel(canvas, 5, 118, 202, 72, dim, panel);
    canvas.setTextColor(bright);
    canvas.setCursor(12, 124);
    char wifiAge[12];
    formatAge(wifiAge, sizeof(wifiAge), scan.wifiCompletedMs == 0
        ? UINT32_MAX : nowMs - scan.wifiCompletedMs);
    canvas.printf("WIFI SWEEP %04u // %s", static_cast<unsigned>(Wardrive::getSessionScanCycles()),
                  scan.wifiScanPending ? "SCANNING" : wifiAge);
    canvas.setTextColor(dim);
    canvas.setCursor(12, 138);
    if (scan.wifiResultValid) {
        canvas.printf("AP %03u  OPEN %03u  SEC %03u",
                      static_cast<unsigned>(scan.wifiNetworks),
                      static_cast<unsigned>(scan.wifiOpenNetworks),
                      static_cast<unsigned>(scan.wifiNetworks - scan.wifiOpenNetworks));
        canvas.setCursor(12, 151);
        if (scan.wifiNetworks > 0) {
            canvas.printf("HOT %ddBm  CH %u",
                          static_cast<int>(scan.wifiStrongestRssi),
                          static_cast<unsigned>(scan.wifiStrongestChannel));
            char ssid[25];
            copyDisplaySsid(ssid, sizeof(ssid), scan.wifiStrongestSsid);
            canvas.setCursor(12, 164);
            canvas.printf("SSID %.23s", ssid);
        } else {
            canvas.print("AIR EMPTY THIS SWEEP");
        }
    } else {
        canvas.print("NO COMPLETED WIFI SWEEP");
    }

    drawPanel(canvas, 212, 118, 103, 72, dim, panel);
    canvas.setTextColor(bright);
    canvas.setCursor(219, 124);
    char c5Age[12];
    formatAge(c5Age, sizeof(c5Age), scan.c5CompletedMs == 0
        ? UINT32_MAX : nowMs - scan.c5CompletedMs);
    canvas.printf("5G %03u %s", static_cast<unsigned>(scan.c5Networks5GHz), c5Age);
    canvas.setTextColor(dim);
    canvas.setCursor(219, 138);
    if (scan.c5ResultValid && scan.c5Networks5GHz > 0) {
        canvas.printf("PK %ddBm C%u", static_cast<int>(scan.c5StrongestRssi),
                      static_cast<unsigned>(scan.c5StrongestChannel));
    } else if (!scan.c5ResultValid) {
        canvas.print(Wardrive::hasC5WardriveConnection() ? "C5 WAIT" : "C5 OFF");
    } else {
        canvas.print("AIR EMPTY");
    }
    canvas.setCursor(219, 151);
    canvas.printf("BLE %03u", static_cast<unsigned>(Wardrive::getSessionNewBleDevices()));
    canvas.setCursor(219, 164);
    if (Wardrive::isSDReady()) canvas.printf("SD %.1fGB", Wardrive::getSDFreeGB());
    else canvas.print("SD NO REC");
    canvas.setCursor(219, 177);
    if (Wardrive::isPaused()) canvas.print("HOLD");
    else if (!Wardrive::isSDReady()) canvas.print("SCAN ONLY");
    else if (!pos.fixed) canvas.print("CSV WAIT GPS");
    else canvas.print("CSV REC");

    drawPanel(canvas, 5, 195, 310, 40, dim, panel);
    char elapsed[16];
    formatElapsed(elapsed, sizeof(elapsed), Wardrive::getSessionElapsedMs());
    canvas.setTextColor(bright);
    canvas.setCursor(12, 201);
    const bool coreMotionAvailable = Wardrive::hasGPSFix();
    if (pos.fromC5 && coreMotionAvailable) {
        canvas.printf("RUN %s CORE DIST %.2fkm SPD %.1fkm/h", elapsed,
                      Wardrive::getSessionDistanceKm(), Wardrive::getSpeedKmh());
    } else if (pos.fromC5) {
        canvas.printf("RUN %s CORE DIST %.2fkm SPD --", elapsed,
                      Wardrive::getSessionDistanceKm());
    } else if (coreMotionAvailable) {
        canvas.printf("RUN %s DIST %.2fkm SPD %.1fkm/h", elapsed,
                      Wardrive::getSessionDistanceKm(), Wardrive::getSpeedKmh());
    } else {
        canvas.printf("RUN %s DIST %.2fkm SPD --", elapsed,
                      Wardrive::getSessionDistanceKm());
    }
    canvas.setTextColor(dim);
    canvas.setCursor(12, 216);
    if (GPS::hasCourse()) {
        canvas.printf(pos.fromC5 ? "CORE TRK %.1f DEG " : "TRK %.1f DEG ",
                      GPS::getCourseDeg());
    }
    canvas.print("[<>]COCKPIT [TOP+]LOCK");

    canvas.setTextSize(1);
    canvas.setTextColor(fg);
}

} // namespace WardriveTelemetry
