/**
 * wardrive_hud.cpp — HUD telemetry, comms bubbles, attitude indicator
 *
 * Extracted from wardrive_scene.cpp. Tower comms state machine,
 * telemetry text formatting, attitude indicator, speed helpers.
 */
#include "wardrive_hud.h"
#include "wardrive_shared.h"
#include "wardrive_glass.h"
#include "wardrive.h"
#include "../ui/display.h"
#include "../ui/menu_pig_render.h"
#include "../defense/recon.h"
#include "../defense/defense_pipeline.h"
#include "../activity/pedometer.h"
#include "../audio/sfx.h"
#include "../util/time_math.h"

namespace WardriveScene {

static constexpr float PI_F = 3.14159265f;

// ==[ TOWER COMMS STATE ]==
TowerPhase towerPhase         = TowerPhase::IDLE;
uint32_t   towerPhaseStart    = 0;
char       towerCallsign[16]  = {0};
char       towerBubbleText[80]= {0};
char       pigBubbleText[40]  = {0};
uint8_t    towerSpeakerIdx    = 0;
uint32_t   nextCommsTime      = 0;
bool       lastCommsWasRecon  = false;

// =========================================================================
// INTERNAL HELPERS (static to this translation unit)
// =========================================================================

void sampleAttitudeDegrees(float& rollDeg, float& pitchDeg) {
    float ax, ay, az;
    Pedometer::getCachedAccel(ax, ay, az);
    // negate ax: X points RIGHT but right-bank gravity gives -ax.
    // -ax flips convention so rollDeg > 0 = right bank, matching ATT negation.
    rollDeg = atan2f(-ax, ay) * (180.0f / PI_F);
    pitchDeg = atan2f(az, ay) * (180.0f / PI_F);
}

static void formatCompactCount(char* out, size_t outSize, uint32_t value) {
    if (!out || outSize == 0) return;
    if (value < 1000) {
        snprintf(out, outSize, "%lu", (unsigned long)value);
        return;
    }

    const char* suffix = "k";
    uint32_t divisor = 1000;
    if (value >= 1000000UL) {
        suffix = "M";
        divisor = 1000000UL;
    }

    uint32_t whole = value / divisor;
    uint32_t tenth = ((value % divisor) * 10UL + divisor / 2UL) / divisor;
    if (tenth >= 10) {
        whole++;
        tenth = 0;
    }

    // rounding overflow: 999.5k+ -> promote to next suffix
    if (whole >= 1000 && divisor == 1000) {
        suffix = "M";
        whole = 1;
        tenth = 0;
    }

    if (whole >= 100) {
        snprintf(out, outSize, "%lu%s", (unsigned long)whole, suffix);
    } else {
        snprintf(out, outSize, "%lu.%lu%s", (unsigned long)whole, (unsigned long)tenth, suffix);
    }
}

static void formatTenths(char* out, size_t outSize, uint32_t tenths, const char* suffix = "") {
    if (!out || outSize == 0) return;
    snprintf(out, outSize, "%lu.%lu%s",
             (unsigned long)(tenths / 10UL),
             (unsigned long)(tenths % 10UL),
             suffix ? suffix : "");
}

static bool hasWardrivePositionFix() {
    return Wardrive::hasGPSFix() || Wardrive::isUsingFreshC5WardriveCoords();
}

static double getWardriveFixLatitude() {
    return Wardrive::isUsingFreshC5WardriveCoords()
               ? Wardrive::getC5WardriveLatitude()
               : Wardrive::getLatitude();
}

static double getWardriveFixLongitude() {
    return Wardrive::isUsingFreshC5WardriveCoords()
               ? Wardrive::getC5WardriveLongitude()
               : Wardrive::getLongitude();
}

// =========================================================================
// PUBLIC FUNCTIONS
// =========================================================================

// effect speed: drives rain/streaks/whoosh — keeps IMU tilt ambiance without GPS
float getEffectSpeedKmh() {
    if (Wardrive::isPaused()) return 0.0f;
    if (Wardrive::hasGPSFix()) return Wardrive::getSpeedKmh();
    return 52.0f + imuPitch * 38.0f;
}

// display speed: HUD text only — no fake numbers
float getDisplaySpeedKmh() {
    if (Wardrive::isPaused()) return -1.0f;
    if (Wardrive::hasGPSFix()) return Wardrive::getSpeedKmh();
    return -1.0f;  // no fix -> caller shows "--"
}

float getMotion() {
    return clamp01(getEffectSpeedKmh() / 90.0f);
}

// ==[ TOWER COMMS LOGIC ]==
void generateCallsign(bool recon) {
    if (recon) {
        static const char* const ops[] = {
            "IPP-NET", "RECON-1", "SIGINT-3",
            "OINK-EW", "SNIFFER", "PEN-OPS",
        };
        snprintf(towerCallsign, sizeof(towerCallsign), "%s",
                 ops[esp_random() % (sizeof(ops) / sizeof(ops[0]))]);
    } else {
        static const char* const sectors[] = {
            "NEON-CTR", "SECTOR-7", "GRID-ATC", "ZONE-12",
            "HAZE-TWR", "RAIN-APP", "DRIFT-01", "BLADE-05",
            "VOLT-GND", "SMOG-CTR", "NITE-APP", "RUST-TWR",
        };
        snprintf(towerCallsign, sizeof(towerCallsign), "%s",
                 sectors[esp_random() % (sizeof(sectors) / sizeof(sectors[0]))]);
    }
}

// recon-flavored tower intel — returns true if it produced a message
bool generateReconTowerMsg(char* out, size_t sz) {
    // priority: deauth > evil twin > KARMA > spam > following > scan intel
    if (DefensePipeline::snapshot().isDeauthActive()) {
        uint16_t burst = DefensePipeline::snapshot().getLastDeauthBurstCount();
        uint8_t ch = DefensePipeline::snapshot().getLastDeauthChannel();
        int8_t rssi = DefensePipeline::snapshot().getLastDeauthRSSI();
        if (burst > 5) {
            uint8_t src = DefensePipeline::snapshot().getLastDeauthUniqueSources();
            snprintf(out, sz, "%s\nDEAUTH BURST\n%u FRM %u SRC", towerCallsign,
                     (unsigned)burst, (unsigned)src);
        } else {
            snprintf(out, sz, "%s\nDEAUTH CH%u\n%ddBm", towerCallsign,
                     (unsigned)ch, (int)rssi);
        }
        return true;
    }
    if (DefensePipeline::snapshot().isEvilTwinActive()) {
        snprintf(out, sz, "%s\nEVIL TWIN\nACTIVE", towerCallsign);
        return true;
    }
    if (DefensePipeline::snapshot().isKarmaActive()) {
        snprintf(out, sz, "%s\nKARMA TRAP\nHONEYPOT DET", towerCallsign);
        return true;
    }
    int spam = DefensePipeline::snapshot().getSpamCount();
    if (spam > 0) {
        snprintf(out, sz, "%s\nBLE SPAM\n%u SOURCE%s", towerCallsign,
                 (unsigned)spam, spam > 1 ? "S" : "");
        return true;
    }
    int following = DefensePipeline::snapshot().getFollowingCount();
    if (following > 0) {
        snprintf(out, sz, "%s\nTAIL %u\nADVISE EVADE", towerCallsign,
                 (unsigned)following);
        return true;
    }
    // general scan intel — random pick from available data
    int ap = DefensePipeline::snapshot().getLastWifiAPCount();
    int openAp = DefensePipeline::snapshot().getOpenAPCount();
    int trackers = DefensePipeline::snapshot().getTrackerCount();
    int bleDev = DefensePipeline::snapshot().getBleDeviceTableSize();
    uint16_t probes = DefensePipeline::snapshot().getKnownProbeRequestCount();
    uint16_t knownAp = DefensePipeline::snapshot().getKnownAPCount();
    // build candidate pool
    uint8_t pool[5]; int poolSz = 0;
    if (ap > 0)       pool[poolSz++] = 0;
    if (bleDev > 0)   pool[poolSz++] = 1;
    if (probes > 0)   pool[poolSz++] = 2;
    if (knownAp > 0)  pool[poolSz++] = 3;
    if (DefensePipeline::snapshot().getDeauthCount() > 0) pool[poolSz++] = 4;
    if (poolSz == 0) return false;
    switch (pool[esp_random() % poolSz]) {
        case 0:
            snprintf(out, sz, "%s\nSCAN %u AP\n%u OPEN", towerCallsign,
                     (unsigned)ap, (unsigned)openAp);
            break;
        case 1:
            snprintf(out, sz, "%s\nBLE %u DEV\n%u TRACKER", towerCallsign,
                     (unsigned)bleDev, (unsigned)trackers);
            break;
        case 2:
            snprintf(out, sz, "%s\nPROBE %u\n%u CLIENT", towerCallsign,
                     (unsigned)probes, (unsigned)DefensePipeline::snapshot().getKnownProbeClientCount());
            break;
        case 3:
            snprintf(out, sz, "%s\nKNOWN %u AP\nPOTFILE HIT", towerCallsign,
                     (unsigned)knownAp);
            break;
        case 4:
            snprintf(out, sz, "%s\nDEAUTH %u\nSESSION TOTAL", towerCallsign,
                     (unsigned)DefensePipeline::snapshot().getDeauthCount());
            break;
    }
    return true;
}

void generateTowerMsg(char* out, size_t sz) {
    // 30% chance of recon-flavored comms when Recon has data
    if ((esp_random() % 100) < 30) {
        generateCallsign(true);
        if (generateReconTowerMsg(out, sz)) {
            lastCommsWasRecon = true;
            return;
        }
    }
    lastCommsWasRecon = false;
    generateCallsign(false);
    if (hasWardrivePositionFix()) {
        uint32_t r = esp_random() % 4;
        switch (r) {
            case 0:
                snprintf(out, sz, "%s\nLAT %.3f\nLON %.3f",
                         towerCallsign, getWardriveFixLatitude(), getWardriveFixLongitude());
                break;
            case 1:
                if (Wardrive::hasGPSFix()) {
                    snprintf(out, sz, "%s\nSPD %u ALT %.0f\nSAT %u",
                             towerCallsign, (unsigned)Wardrive::getSpeedKmh(),
                             Wardrive::getAltitude(), (unsigned)Wardrive::getSatCount());
                } else {
                    snprintf(out, sz, "%s\nSPD -- ALT --\nSAT %u",
                             towerCallsign, (unsigned)Wardrive::getSatCount());
                }
                break;
            case 2:
                snprintf(out, sz, "%s\nFIX HELD\nSAT %u",
                         towerCallsign, (unsigned)Wardrive::getSatCount());
                break;
            default:
                snprintf(out, sz, "%s\nGPS LOCK\nNET %lu",
                         towerCallsign, (unsigned long)Wardrive::getSessionNewNets());
                break;
        }
    } else {
        static const char* const chatter[] = {
            "NO GPS FIX\nASK SKY AGAIN",
            "SATELLITES\nREFUSE LINEUP",
            "COORDS EMPTY\nCITY STILL MOVES",
            "NO FIX\nRAIN HAS ALIBI",
            "GPS WITNESS\nMISSED CHECK-IN",
            "LAT/LON BLANK\nCASE STILL ROLLS",
            "POSITION COLD\nVECTOR UNKNOWN",
            "SATELLITE DESK\nNO RECEIPT",
            "COORDS PENDING\nSTAY ON BEAT",
            "SKY WENT QUIET\nNO POSITION",
            "FIX MISSING\nRADIO STILL HEARS",
            "POSITION UNKNOWN\n2.4 STILL TALKS",
            "GPS CAME BACK\nWITHOUT A FIX",
            "NO GPS FIX\nCASE OPEN",
        };
        snprintf(out, sz, "%s\n%s", towerCallsign,
                 chatter[esp_random() % (sizeof(chatter) / sizeof(chatter[0]))]);
    }
}

void generatePigMsg(char* out, size_t sz) {
    if (!out || sz == 0) return;

    // recon-aware responses when tower sent defense intel
    if (lastCommsWasRecon) {
        static const char* const reconResp[] = {
            "copy.\ncase escalated.",
            "RSSI noted.\nno panic yet.",
            "frames logged.\neyes open.",
            "copy.\nDEFHOG has it.",
            "evidence first.\ncoffee second.",
            "ack.\nthe wire remembers.",
            "copy.\nno heroics. log it.",
            "noted.\nkeep the receipt.",
        };
        snprintf(out, sz, "%s",
                 reconResp[esp_random() % (sizeof(reconResp) / sizeof(reconResp[0]))]);
        return;
    }

    if (hasWardrivePositionFix()) {
        static const char* const gpsResp[] = {
            "copy.\nfix on file.",
            "coords received.\ncase moves.",
            "GPS testifies.\nfor once.",
            "position filed.\nkeep rolling.",
            "fix held.\nradio working.",
        };
        snprintf(out, sz, "%s",
                 gpsResp[esp_random() % (sizeof(gpsResp) / sizeof(gpsResp[0]))]);
        return;
    }

    static const char* const requests[] = {
        "copy.\nno coordinates.",
        "keep asking.\ni keep scanning.",
        "sky went quiet.\nradio did not.",
        "no fix.\ncase stays open.",
        "tower.\nask satellites.",
        "negative fix.\ncity still moves.",
        "coordinates\nfailed to testify.",
        "copy.\n2.4 keeps talking.",
        "GPS absent.\nscan present.",
        "understood.\nno fake heading.",
    };
    snprintf(out, sz, "%s",
             requests[esp_random() % (sizeof(requests) / sizeof(requests[0]))]);
}

void updateTowerComms(uint32_t now) {
    if (nextCommsTime == 0) {
        nextCommsTime = now + 5000 + (esp_random() % 10000);
        return;
    }

    switch (towerPhase) {
        case TowerPhase::IDLE:
            if (TimeMath::reached(now, nextCommsTime) && !Wardrive::isPaused()) {
                generateTowerMsg(towerBubbleText, sizeof(towerBubbleText));
                generatePigMsg(pigBubbleText, sizeof(pigBubbleText));
                towerSpeakerIdx = (uint8_t)(esp_random() % 251u);
                towerPhase = TowerPhase::TURNING;
                towerPhaseStart = now;
                SFX::click();  // radio crackle — pig keys mic
            }
            break;
        case TowerPhase::TURNING:
            if (now - towerPhaseStart >= TURN_MS) {
                towerPhase = TowerPhase::TALKING;
                towerPhaseStart = now;
            }
            break;
        case TowerPhase::TALKING:
            if (now - towerPhaseStart >= TALK_MS) {
                towerPhase = TowerPhase::TURNING_BACK;
                towerPhaseStart = now;
            }
            break;
        case TowerPhase::TURNING_BACK:
            if (now - towerPhaseStart >= TURN_MS) {
                towerPhase = TowerPhase::IDLE;
                towerBubbleText[0] = '\0';
                pigBubbleText[0] = '\0';
                hasCommsTower = false;
                nextCommsTime = now + COMMS_MIN + (esp_random() % (COMMS_MAX - COMMS_MIN));
            }
            break;
    }
}

// ==[ ATTITUDE INDICATOR ]== tilting horizon bar + pitch readout
// gravity decomposition via atan2: no gyro drift, no EMA, instant response.
// Shared MPU6886 landscape frame (screen facing user, USB-C left): ay ~ 1g.
// Y axis = UP. X axis = RIGHT (but right-bank gravity -> -ax). Z axis = TOWARD USER.
// roll  = atan2(-ax, ay) — right bank positive, +/-45 deg range on bar
// pitch = atan2(az, ay) — forward tilt positive, 0deg=upright 90deg=flat
void drawAttitudeIndicator(M5Canvas& canvas) {
    float rollDeg = attRollDeg;
    float pitchDeg = attPitchDeg;
    int pDeg = (int)(fabsf(pitchDeg) + 0.5f);
    if (pDeg > 90) pDeg = 90;

    // ==[ TILTING HORIZON BAR ]== 11 chars rendered at individual Y offsets
    // bar stays parallel to real-world horizon — device tilts, bar counter-tilts
    // each char gets vertical offset from roll: dy = (i - center) * sin(roll) * scale
    static const char barChars[] = "-----+-----";
    constexpr int BAR_LEN = 11;
    constexpr int BAR_CENTER = 5;  // '+' position

    int barCX = ATT_X + ATT_W / 2;           // horizontal center of ATT pane
    int barCY = ATT_Y + SCENE_TEXT_PAD_Y + 4; // vertical center of bar row
    // negate: device banks right -> world horizon tilts LEFT (standard ATT behavior)
    float rollRad = -rollDeg * (PI_F / 180.0f);
    float sinR = sinf(rollRad);
    float cosR = cosf(rollRad);
    // scale: at +/-45 deg roll, outermost chars shift +/-1 full line height
    float charSpacingX = (float)SCENE_CHAR_W;

    uint16_t hudPeak = Display::isInvertedTheme() ? Display::getColorBG() : Display::getColorFG();
    uint16_t hudDark = Display::isInvertedTheme() ? Display::getColorFG() : Display::getColorBG();
    // ATT matches the shared HUD text color for windshield telemetry consistency.
    uint16_t attCol = hudPeak;
    uint16_t shadow = Display::lerpColor565(hudDark, RP::DEEP, 0.10f);
    char ch[2] = {0, 0};

    // ATT glow halo — subtle, respects cockpit darkness
    for (int gy = q(ATT_Y - PX); gy < q(ATT_Y + ATT_H + PX); gy += PX * 2) {
        for (int gx = q(ATT_X - PX); gx < q(ATT_X + ATT_W + PX); gx += PX * 2) {
            float dx = ((float)gx + 2.0f - (float)barCX) / (float)(ATT_W / 2 + PX * 2);
            float dy = ((float)gy + 2.0f - (float)barCY) / (float)(ATT_H / 2 + PX * 2);
            float dist = dx * dx + dy * dy;
            if (dist < 1.0f) glowPx(canvas, gx, gy, attCol, (uint8_t)((1.0f - dist) * 36.0f));
        }
    }

    canvas.setTextSize(1);

    // shadow pass
    canvas.setTextColor(shadow);
    for (int i = 0; i < BAR_LEN; ++i) {
        float offset = (float)(i - BAR_CENTER) * charSpacingX;
        int cx = barCX + (int)(offset * cosR);
        int cy = barCY + (int)(offset * sinR);
        ch[0] = barChars[i];
        canvas.setCursor(q(cx - PX), q(cy - PX) + PX);  // grid-snapped half-char centering; +PX shadow
        canvas.print(ch);
    }

    // foreground pass
    canvas.setTextColor(attCol);
    for (int i = 0; i < BAR_LEN; ++i) {
        float offset = (float)(i - BAR_CENTER) * charSpacingX;
        int cx = barCX + (int)(offset * cosR);
        int cy = barCY + (int)(offset * sinR);
        ch[0] = barChars[i];
        canvas.setCursor(q(cx - PX), q(cy - PX));
        canvas.print(ch);
    }

    // ==[ PITCH READOUT ]== below the bar via drawSceneTextBlock
    char pitchText[32];
    snprintf(pitchText, sizeof(pitchText), "P %02d ATT", pDeg);
    drawSceneTextBlock(canvas, ATT_X, q(ATT_Y + PX * 5), ATT_W, ATT_H - PX * 5, pitchText, attCol);
}

static void drawHudChipRight(M5Canvas& canvas, int rightX, int y,
                             const char* label, uint16_t color) {
    if (!label || label[0] == '\0') return;

    constexpr int CHIP_H = 12;
    const int chipW = q((int)strlen(label) * 6 + 7);
    uint16_t bg = Display::isInvertedTheme() ? Display::getColorFG() : Display::getColorBG();
    uint16_t fill = Display::lerpColor565(bg, color, 0.12f);

    int x = q(rightX - chipW);
    y = q(y);
    canvas.fillRect(x, y, chipW, CHIP_H, fill);
    canvas.drawRect(x, y, chipW, CHIP_H, color);
    canvas.setTextSize(1);
    canvas.setTextColor(color);
    canvas.setTextDatum(MC_DATUM);
    canvas.drawString(label, x + chipW / 2, y + CHIP_H / 2);
    canvas.setTextDatum(TL_DATUM);
}

void drawTelemetryText(M5Canvas& canvas, uint32_t now) {
    (void)now;
    char buf[24];
    char sniffText[96];
    char navText[64];
    char coordText[64];
    char logText[64];
    char lifeBuf[16];
    char rateBuf[16];
    char kmBuf[12];

    bool positionFix = hasWardrivePositionFix();
    bool usingC5Coords = Wardrive::isUsingFreshC5WardriveCoords();
    bool c5Connected = Wardrive::hasC5WardriveConnection();
    // HUD text: brightest theme color — emissive projection on glass
    uint16_t hudPeak = Display::isInvertedTheme() ? Display::getColorBG() : Display::getColorFG();
    uint16_t textA = hudPeak;
    uint16_t textB = hudPeak;
    uint16_t textC = hudPeak;
    uint16_t textD = hudPeak;

    uint32_t elapsedMs = Wardrive::getSessionElapsedMs();
    uint16_t nets = Wardrive::getSessionNewNets();
    uint16_t scans = Wardrive::getSessionScanCycles();
    uint32_t lifetime = Wardrive::getLifetimeNets();
    uint32_t rate10 = (elapsedMs > 1000) ? (uint32_t)(((uint64_t)nets * 600000ULL + elapsedMs / 2UL) / elapsedMs) : 0;  // nets/min x10
    uint32_t dist10 = (uint32_t)(Wardrive::getSessionDistanceKm() * 10.0f + 0.5f);

    formatCompactCount(lifeBuf, sizeof(lifeBuf), lifetime);
    formatTenths(rateBuf, sizeof(rateBuf), rate10, "");
    formatTenths(kmBuf, sizeof(kmBuf), dist10, "K");

    canvas.setTextSize(1);
    // SIG pane: NET + BLE on first line, then lifetime/SCN/rate
    if (Wardrive::isBleEnabled()) {
        snprintf(sniffText, sizeof(sniffText),
                 "N%u B%u\n+%s\nSCN %u\n%s/m",
                 (unsigned)nets, (unsigned)Wardrive::getSessionNewBleDevices(),
                 lifeBuf, (unsigned)scans, rateBuf);
    } else {
        snprintf(sniffText, sizeof(sniffText),
                 "NET %u\n+%s\nSCN %u\n%s/m",
                 (unsigned)nets, lifeBuf, (unsigned)scans, rateBuf);
    }

    // NAV pane: speed + distance (2 lines only)
    float dspd = getDisplaySpeedKmh();
    if (dspd < 0.0f) {
        snprintf(navText, sizeof(navText), "--K/H\n%s", kmBuf);
    } else {
        snprintf(navText, sizeof(navText), "%uK/H\n%s", (unsigned)(dspd + 0.5f), kmBuf);
    }

    // COORD pane: source/fix state plus one signed coordinate per row. Keep
    // every row within the pane's eight-character budget so source labels can
    // never consume the latitude/longitude evidence below them.
    if (usingC5Coords) {
        snprintf(coordText, sizeof(coordText), "%s\n%+.2f\n%+.2f",
                 c5Connected ? "C5 FIX" : "C5 HOLD",
                 Wardrive::getC5WardriveLatitude(),
                 Wardrive::getC5WardriveLongitude());
    } else if (positionFix) {
        snprintf(coordText, sizeof(coordText), "GPS FIX\n%+.2f\n%+.2f",
                 Wardrive::getLatitude(), Wardrive::getLongitude());
    } else if (c5Connected) {
        if (Wardrive::hasGPSNMEA()) {
            snprintf(coordText, sizeof(coordText), "GPS ACQ\nS:%u\nNMEA OK",
                     (unsigned)Wardrive::getSatCount());
        } else if (Wardrive::hasGPSData()) {
            snprintf(coordText, sizeof(coordText), "GPS DATA\nNMEA BAD");
        } else {
            snprintf(coordText, sizeof(coordText), "C5 ACQ\nNO FIX");
        }
    } else if (Wardrive::hasGPSNMEA()) {
        snprintf(coordText, sizeof(coordText), "GPS ACQ\nS:%u\nNMEA OK",
                 (unsigned)Wardrive::getSatCount());
    } else if (Wardrive::hasGPSData()) {
        snprintf(coordText, sizeof(coordText), "GPS DATA\nNMEA BAD");
    } else if (Wardrive::isGPSRunning()) {
        snprintf(coordText, sizeof(coordText), "GPS WAIT\nNO DATA");
    } else {
        snprintf(coordText, sizeof(coordText), "GPS OFF");
    }

    // LOG pane: SD + REC/HOLD (BLE moved to SIG)
    if (Wardrive::isSDReady()) {
        uint32_t gb10 = (uint32_t)(Wardrive::getSDFreeGB() * 10.0f + 0.5f);
        formatTenths(kmBuf, sizeof(kmBuf), gb10, "G");
        snprintf(buf, sizeof(buf), "SD %s", kmBuf);
    } else {
        snprintf(buf, sizeof(buf), "SD NONE");
    }
    snprintf(logText, sizeof(logText), "%s\n%s", buf, Wardrive::isPaused() ? "HOLD" : "REC");

    // glass-bounds-relative clamping — keep panes inside windshield with margin
    int navLeft, navRight;
    glassBounds(NAV_Y + NAV_H / 2, navLeft, navRight);
    int navX = clampi(NAV_X, navLeft + PX * 2, navRight - NAV_W - PX * 2);

    int coordLeft, coordRight;
    glassBounds(COORD_Y + COORD_H / 2, coordLeft, coordRight);
    int coordX = clampi(COORD_X, coordLeft + PX * 2, coordRight - COORD_W - PX * 2);

    int sigLeft, sigRight;
    glassBounds(SIG_Y + SIG_H / 2, sigLeft, sigRight);
    int sigX = clampi(SIG_X, sigLeft + PX * 2, sigRight - SIG_W - PX * 2);

    drawSceneTextBlock(canvas, sigX, SIG_Y, SIG_W, SIG_H, sniffText, textA);
    drawSceneTextBlock(canvas, navX, NAV_Y, NAV_W, NAV_H, navText, textB);
    drawSceneTextBlock(canvas, coordX, COORD_Y, COORD_W, COORD_H, coordText, textC);
    drawSceneTextBlock(canvas, LOG_X, LOG_Y, LOG_W, LOG_H, logText, textD);

    if (c5Connected) {
        drawHudChipRight(canvas, sigX + SIG_W, SIG_Y - 12, "C5", textA);
    }
}

} // namespace WardriveScene
