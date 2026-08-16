/**
 * Loot Menu - Full implementation with detail view
 *
 * ==[ LOOT VAULT ]== PORKCHOP 3-color theme: FG, BG, DIM.
 */

#include "loot_menu.h"
#include "pin_entry.h"
#include "display.h"
#include "frame_presenter.h"
#include <M5Unified.h>
#include "../core/capture.h"
#include "../core/config.h"
#include "../hal/sd_storage.h"
#include "../util/rf_util.h"
#include "../net/wifi_client.h"
#include "../net/wpasec_client.h"
#include "../net/wigle_client.h"
#include "../net/ntp_sync.h"
#include "../defense/potfile.h"
#include "../input/touch_hints.h"
#include "../haptic/haptic.h"
#include <SD.h>
#include <esp_heap_caps.h>
#include <ctype.h>

namespace LootMenu {

static int currentIdx = 0;
static bool inDetailView = false;
static M5Canvas* canvas = nullptr;
static bool pinUnlocked = false;   // session flag — reset on enter()
static uint8_t wrongPinCount = 0;
static bool pinDeadmanNuked = false;
static char pinTaunt[40] = "";

// ==[ LOOT VIEW TABS ]== cycle with SWIPE_RIGHT
enum LootView { LOOT_LIST, LOOT_CRACKED, LOOT_WIGLE, LOOT_NUKE };
static LootView currentLootView = LOOT_LIST;

// Upload state (shared by WPA-SEC + potfile download + WiGLE upload)
enum UploadState {
    UPLOAD_IDLE = 0,
    UPLOAD_CONNECTING,
    UPLOAD_SENDING,
    UPLOAD_SUCCESS,
    UPLOAD_FAILED
};
static UploadState uploadState = UPLOAD_IDLE;
static char uploadMessage[64] = "";
static uint32_t uploadStartTime = 0;
// which op is in flight
enum UploadOp { OP_WPASEC, OP_POTFILE, OP_WIGLE, OP_NUKE };
static UploadOp activeOp = OP_WPASEC;

// rate-limit progress redraws during blocking uploads (~5 Hz)
static uint32_t lastProgressDrawMs = 0;

// ==[ CRACKED VIEW ]== PSRAM-backed potfile entries
struct PotEntry {
    char ssid[33];
    char pass[65];
    char bssid[18];
    bool captured;
};
static PotEntry* potEntries = nullptr;
static uint16_t potCount = 0;
static int potScroll = 0;
static bool potfileTruncated = false;

// ==[ WIGLE VIEW ]== cached stats + file list
static WiGLE::UserStats wigleStats = {0, 0, 0, false};
static uint16_t wigleCsvCount = 0;
static int wigleScroll = 0;
static uint32_t wigleRowCount = 0;
static uint64_t wigleBytes = 0;
static uint16_t wigleStateCounts[4] = {};
static bool wigleFilesTruncated = false;

static const int MAX_WIGLE_FILES = 64;
struct WigleCsvInfo {
    char name[28];       // display name (e.g. "WD_20260328_143045")
    uint32_t rows;       // data rows (lines minus 2 header lines)
    uint32_t sizeBytes;  // file size
};
static WigleCsvInfo wigleFiles[MAX_WIGLE_FILES];
// Keep the byte-sized state outside the aligned record. Core2 cannot afford
// three bytes of tail padding per entry just to carry this enum.
static WiGLE::FileState wigleFileStates[MAX_WIGLE_FILES];

// ==[ NUKE TAB ]== wipe all captures + files (4th tab stop)

// column alignment constants
static const int COL_LABEL = 4;      // labels start here
static const int COL_VALUE = 50;     // values align here
static const int COL_RIGHT = SCREEN_WIDTH - 4;  // right-aligned items
static const int LINE_H = 18;        // standardized menu spacing

// forward declaration
static void draw();
static void drawList();
static void drawDetail();
static void drawCracked();
static void drawWigleView();
static void drawNukeConfirm();
static void drawTabBreadcrumb();
static void drawPrereqGlyphs(int x, int y);
static void loadCrackedView();
static void loadWigleView();
static void executeNuke(bool showResult = true);

// ==[ LAYOUT CONSTANTS ]== unified vertical rhythm
static const int BREAD_Y = TOP_BAR_H + 2;       // 16 — breadcrumb row top
static const int BREAD_H = 12;                  // breadcrumb row height (even → symmetric 2/2 padding around 8-px text)
static const int STATS_Y = TOP_BAR_H + 16;      // 30 — stats row baseline
static const int LIST_Y  = TOP_BAR_H + 30;      // 44 — first content row

// ==[ KEY VERSION ]== extract from EAPOL Key Info field
// data[5..6] big-endian Key Info, bits 0-2 = descriptor version
static const char* getKeyVerStr(const EAPOLFrame* f) {
    if (!f || f->len < 7) return "?";
    uint8_t kv = ((f->data[5] << 8) | f->data[6]) & 0x07;
    switch (kv) {
        case 1: return "WPA";
        case 2: return "WPA2";
        case 3: return "PMF";
        default: return "?";
    }
}

// best message pair string for display
static const char* getMsgPairStr(const CapturedHandshake* hs) {
    if (hs->hasMessage(1) && hs->hasMessage(2)) return "M1+M2";
    if (hs->hasMessage(2) && hs->hasMessage(3)) return "M2+M3";
    if (hs->hasMessage(3) && hs->hasMessage(4)) return "M3+M4";
    return "?";
}
static void drawUploadView();

// Format epoch time as HH:MM
static void formatCaptureTime(uint32_t epoch, char* out, size_t len) {
    if (epoch == 0) {
        snprintf(out, len, "--:--");
        return;
    }
    uint32_t totalMins = epoch / 60;
    uint8_t hours = (totalMins / 60) % 24;
    uint8_t mins = totalMins % 60;
    snprintf(out, len, "%02d:%02d", hours, mins);
}

// Compact evidence counters keep lifetime values honest without letting an
// eight-digit field bulldoze the rest of the status rail.
static void formatCompactCount(uint32_t value, char* out, size_t len) {
    if (value < 10000u) {
        snprintf(out, len, "%lu", (unsigned long)value);
    } else if (value < 1000000u) {
        snprintf(out, len, "%.1fK", value / 1000.0f);
    } else {
        snprintf(out, len, "%.1fM", value / 1000000.0f);
    }
}

static void formatSize(uint64_t bytes, char* out, size_t len) {
    if (bytes < 1024u) {
        snprintf(out, len, "%lluB", (unsigned long long)bytes);
    } else if (bytes < 1024u * 1024u) {
        snprintf(out, len, "%.1fK", bytes / 1024.0f);
    } else if (bytes < 1024ull * 1024ull * 1024ull) {
        snprintf(out, len, "%.1fM", bytes / (1024.0f * 1024.0f));
    } else {
        snprintf(out, len, "%.1fG", bytes / (1024.0f * 1024.0f * 1024.0f));
    }
}

static void formatHandshakeEvidence(const CapturedHandshake& hs,
                                    char* out, size_t len) {
    const char* pair = getMsgPairStr(&hs);
    if (hs.isComplete() && strcmp(pair, "?") != 0) {
        snprintf(out, len, "%s%s", pair,
                 hs.hasReliableSNonce() ? "" : "?");
        return;
    }

    size_t used = 0;
    for (uint8_t msg = 1; msg <= 4; ++msg) {
        if (!hs.hasMessage(msg)) continue;
        int wrote = snprintf(out + used, len > used ? len - used : 0,
                             "%sM%u", used ? "/" : "", msg);
        if (wrote < 0 || (size_t)wrote >= (len > used ? len - used : 0)) {
            if (len) out[len - 1] = '\0';
            return;
        }
        used += (size_t)wrote;
    }
    if (used == 0 && len) snprintf(out, len, "N0 E4P0L");
}

// ==[ TAB BREADCRUMB ]== 4 segments under status bar, current inverted
static void drawTabBreadcrumb() {
    uint16_t fg = Display::getColorFG();
    uint16_t bg = Display::getColorBG();
    uint16_t dim = Display::lerpColor565(fg, bg, 0.5f);

    static const char* const tabs[4] = { "L1ST", "CR4CK3D", "W1GL3", "NUK3" };
    int tabIdx = (int)currentLootView;

    // widths: text chars × 6px + 6px horizontal pad
    int widths[4];
    int totalW = 0;
    for (int i = 0; i < 4; i++) {
        widths[i] = (int)strlen(tabs[i]) * 6 + 6;
        totalW += widths[i];
    }
    totalW += 3 * 3;  // 3px spacers

    int x = (SCREEN_WIDTH - totalW) / 2;
    int y = BREAD_Y;

    canvas->setTextSize(1);
    canvas->setTextDatum(MC_DATUM);
    for (int i = 0; i < 4; i++) {
        bool active = (i == tabIdx);
        if (active) {
            canvas->fillRect(x, y, widths[i], BREAD_H, fg);
            canvas->setTextColor(bg);
        } else {
            canvas->setTextColor(i == 3 ? dim : fg);  // NUK3 stays dim until focused
        }
        canvas->drawString(tabs[i], x + widths[i] / 2, y + BREAD_H / 2);
        x += widths[i] + 3;
    }
    canvas->setTextColor(fg);
    canvas->setTextDatum(TL_DATUM);
}

// ==[ PREREQ GLYPHS ]== !K (no WPA-SEC key) / !W (no upload WiFi)
static void drawPrereqGlyphs(int x, int y) {
    uint16_t fg = Display::getColorFG();
    uint16_t bg = Display::getColorBG();
    canvas->setTextSize(1);
    canvas->setTextDatum(TL_DATUM);
    if (!Config::hasWpaSecKey()) {
        canvas->fillRect(x, y - 1, 14, 10, fg);
        canvas->setTextColor(bg);
        canvas->drawString("!K", x + 2, y);
        x += 17;
    }
    if (!Config::hasUploadWifi()) {
        canvas->fillRect(x, y - 1, 14, 10, fg);
        canvas->setTextColor(bg);
        canvas->drawString("!W", x + 2, y);
    }
    canvas->setTextColor(fg);
}

void enter() {
    currentIdx = 0;
    inDetailView = false;
    currentLootView = LOOT_LIST;
    uploadState = UPLOAD_IDLE;
    uploadMessage[0] = '\0';
    potScroll = 0;
    pinUnlocked = false;
    wrongPinCount = 0;
    pinDeadmanNuked = false;
    pinTaunt[0] = '\0';
    canvas = Display::getSharedCanvas();
    // hard reset the widget first. pin settings can change between visits.
    PinEntry::cancel();
    // gate with PIN if configured
    if (Config::hasLootPin()) {
        PinEntry::begin();
    }
    draw();
}

void exit() {
    // A long-back mode exit can arrive while STA is still associating. Always
    // release the uplink owner here so Recon/NOWFLOCK are not stranded dark.
    WpaSec::setProgressCallback(nullptr);
    if (WifiClient::ownsRadio() || isUploading()) WifiClient::disconnect();
    uploadState = UPLOAD_IDLE;
    activeOp = OP_WPASEC;
    WiGLE::freeUploadedListMemory();
    Capture::saveJournal();
}

// ==[ PROGRESS TICK ]== redraw the upload view mid-blocking-call, rate-limited
static void uploadProgressTick(uint16_t current, uint16_t total) {
    if (total == 0) return;
    uint32_t now = millis();
    if (now - lastProgressDrawMs < 200) return;   // ~5 Hz cap
    lastProgressDrawMs = now;
    const char* verb = (activeOp == OP_WIGLE)   ? "W1GL3" :
                       (activeOp == OP_POTFILE) ? "PULL"  :
                                                  "SH1P";
    snprintf(uploadMessage, sizeof(uploadMessage), "%s %u/%u", verb, current, total);
    draw();
}

static void setWifiFailureMessage() {
    const char* detail = WifiClient::getLastError();
    snprintf(uploadMessage, sizeof(uploadMessage), "W1F1: %.48s",
             detail && detail[0] ? detail : "CONNECT FAILED");
}

static void setServiceFailureMessage(const char* service, const char* detail) {
    snprintf(uploadMessage, sizeof(uploadMessage), "%s: %.46s",
             service ? service : "UPLINK", detail && detail[0] ? detail : "FAILED");
}

void update() {
    // PIN entry tick (wrong-flash timeout)
    if (isPinEntry()) {
        bool wasFlash = PinEntry::isWrongFlash();
        PinEntry::update();
        if (wasFlash && !PinEntry::isWrongFlash()) draw();
        return;
    }

    // Handle upload state machine FIRST — overlay redraw must never stall it
    if (uploadState == UPLOAD_CONNECTING) {
        WifiClient::update();

        if (WifiClient::getState() == WifiClient::WIFI_CONNECTED) {
            uploadState = UPLOAD_SENDING;

            // TLS certificate validation needs real time. Establish it before
            // either API key leaves the device; NTP itself carries no secret.
            if (!Config::hasTrustedClock() && !NtpSync::syncTime()) {
                uploadState = UPLOAD_FAILED;
                setServiceFailureMessage("CL0CK", NtpSync::getLastError());
                WifiClient::disconnect();
                uploadStartTime = millis();
                draw();
                return;
            }

            if (activeOp == OP_POTFILE) {
                snprintf(uploadMessage, sizeof(uploadMessage), "PULL1NG P0TF1L3...");
                draw();

                uint32_t lines = 0;
                WpaSec::DownloadResult dlRes = WpaSec::downloadPotfile(
                    "/hamlet/export/wpasec.pot", &lines);

                if (dlRes == WpaSec::DL_OK) {
                    uploadState = UPLOAD_SUCCESS;
                    snprintf(uploadMessage, sizeof(uploadMessage), "G0T %lu CR4CK3D.", lines);
                    loadCrackedView();  // reload from file
                } else {
                    uploadState = UPLOAD_FAILED;
                    switch (dlRes) {
                        case WpaSec::DL_NO_KEY:
                            snprintf(uploadMessage, sizeof(uploadMessage), "N0 K3Y. TUN3 P1G.");
                            break;
                        case WpaSec::DL_EMPTY:
                            snprintf(uploadMessage, sizeof(uploadMessage), "P0TF1L3 3MPTY.");
                            break;
                        default:
                            snprintf(uploadMessage, sizeof(uploadMessage), "D0WNL04D F41L3D.");
                            break;
                    }
                }

            } else if (activeOp == OP_WIGLE) {
                snprintf(uploadMessage, sizeof(uploadMessage), "SH1PP1NG W4RDR1V3...");
                lastProgressDrawMs = 0;
                draw();

                WiGLE::SyncResult wr = WiGLE::uploadAll(uploadProgressTick);
                if (wr.error != WiGLE::UploadError::NONE) {
                    uploadState = UPLOAD_FAILED;
                    const char* detail = WiGLE::getLastError();
                    switch (wr.error) {
                        case WiGLE::UploadError::AUTH:
                            snprintf(uploadMessage, sizeof(uploadMessage), "W1GL3 CR3DS R3J3CT3D.");
                            break;
                        case WiGLE::UploadError::RATE_LIMIT:
                            snprintf(uploadMessage, sizeof(uploadMessage), "W1GL3 THR0TTL3D. W41T.");
                            break;
                        case WiGLE::UploadError::BAD_JSON:
                        case WiGLE::UploadError::NO_TRANS_ID:
                        case WiGLE::UploadError::API_REJECTED:
                            snprintf(uploadMessage, sizeof(uploadMessage), "W1GL3 B4D R3C31PT.");
                            break;
                        case WiGLE::UploadError::LEDGER_IO:
                            snprintf(uploadMessage, sizeof(uploadMessage), "QU3U3D; L0G F41L.");
                            break;
                        default:
                            snprintf(uploadMessage, sizeof(uploadMessage), "W1GL3 L1NK F41L3D.");
                            break;
                    }
                    if (detail && detail[0]) setServiceFailureMessage("W1GL3", detail);
                } else if (wr.submitted > 0) {
                    uploadState = UPLOAD_SUCCESS;
                    snprintf(uploadMessage, sizeof(uploadMessage), "W1GL3: %u QU3U3D.", wr.submitted);
                } else if (wr.rejected > 0) {
                    uploadState = UPLOAD_FAILED;
                    snprintf(uploadMessage, sizeof(uploadMessage), "W1GL3: %u R3J3CT3D.", wr.rejected);
                } else if (wr.confirmed > 0) {
                    uploadState = UPLOAD_SUCCESS;
                    snprintf(uploadMessage, sizeof(uploadMessage), "W1GL3: %u CL34R.", wr.confirmed);
                } else if (wr.pending > 0) {
                    uploadState = UPLOAD_SUCCESS;
                    snprintf(uploadMessage, sizeof(uploadMessage), "W1GL3: %u P3ND1NG.", wr.pending);
                } else if (wr.failed > 0) {
                    uploadState = UPLOAD_FAILED;
                    snprintf(uploadMessage, sizeof(uploadMessage), "W1GL3 UP F41L3D.");
                } else if (wr.oversize > 0) {
                    uploadState = UPLOAD_FAILED;
                    snprintf(uploadMessage, sizeof(uploadMessage), "W1GL3: %u CSV T00 B1G.", wr.oversize);
                } else if (wr.empty > 0) {
                    uploadState = UPLOAD_FAILED;
                    snprintf(uploadMessage, sizeof(uploadMessage), "W1GL3: %d CSV N0 R0WS.", wr.empty);
                } else if (wr.skipped > 0) {
                    uploadState = UPLOAD_SUCCESS;
                    snprintf(uploadMessage, sizeof(uploadMessage), "4LL CSVs 4LR34DY UP.");
                } else {
                    uploadState = UPLOAD_FAILED;
                    snprintf(uploadMessage, sizeof(uploadMessage), "N0 W1GL3 CSVS.");
                }
                // refresh rank even when "all already up" — user still wants the latest number
                if (wr.error == WiGLE::UploadError::NONE) {
                    WiGLE::refreshStats();
                    wigleStats = WiGLE::getCachedStats();
                }

            } else {
                snprintf(uploadMessage, sizeof(uploadMessage), "SH1PP1NG...");
                lastProgressDrawMs = 0;
                WpaSec::setProgressCallback(uploadProgressTick);
                draw();

                // Perform actual upload
                uint16_t uploadedCount = 0;
                WpaSec::Result uploadResult = WpaSec::uploadAll(&uploadedCount);
                WpaSec::setProgressCallback(nullptr);
                if (uploadedCount > 0) Capture::saveJournal();

                if (uploadResult == WpaSec::UPLOAD_OK) {
                    uploadState = UPLOAD_SUCCESS;
                    snprintf(uploadMessage, sizeof(uploadMessage), "S3NT %d C4PTUR3S!", uploadedCount);
                } else {
                    uploadState = UPLOAD_FAILED;
                    const char* detail = WpaSec::getLastError();
                    switch (uploadResult) {
                        case WpaSec::UPLOAD_NO_KEY:
                            snprintf(uploadMessage, sizeof(uploadMessage), "N0 K3Y. N0 3NTRY.");
                            break;
                        case WpaSec::UPLOAD_NO_DATA:
                            snprintf(uploadMessage, sizeof(uploadMessage), "N0TH1NG 2 SH1P.");
                            break;
                        case WpaSec::UPLOAD_NO_WIFI:
                            snprintf(uploadMessage, sizeof(uploadMessage), "W1F1 D34D.");
                            break;
                        case WpaSec::UPLOAD_HTTP_ERROR:
                            snprintf(uploadMessage, sizeof(uploadMessage), "HTTP G0 BR0K3.");
                            break;
                        case WpaSec::UPLOAD_TIMEOUT:
                            snprintf(uploadMessage, sizeof(uploadMessage), "T00 SL0W. T1M30UT.");
                            break;
                        case WpaSec::UPLOAD_AUTH_FAIL:
                            snprintf(uploadMessage, sizeof(uploadMessage), "K3Y R3J3CT3D. R0T4T3.");
                            break;
                        case WpaSec::UPLOAD_RATE_LIMITED:
                            snprintf(uploadMessage, sizeof(uploadMessage), "S3RV3R THR0TTL3D. W41T.");
                            break;
                        case WpaSec::UPLOAD_PARTIAL:
                            snprintf(uploadMessage, sizeof(uploadMessage), "S3NT %u. B4TCH P4RT14L.", uploadedCount);
                            break;
                        default:
                            snprintf(uploadMessage, sizeof(uploadMessage), "SH1P S4NK.");
                            break;
                    }
                    if (detail && detail[0]) setServiceFailureMessage("WP4-S3C", detail);
                }
            }

            // Disconnect WiFi. WifiClient owns Recon restoration.
            WifiClient::disconnect();

            // Reset timer for result display (so user can read message)
            uploadStartTime = millis();
            draw();

        } else if (WifiClient::getState() == WifiClient::WIFI_FAILED) {
            uploadState = UPLOAD_FAILED;
            setWifiFailureMessage();
            WifiClient::disconnect();
            uploadStartTime = millis();  // Reset timer for message display
            draw();
        }

    } else if (uploadState == UPLOAD_SUCCESS || uploadState == UPLOAD_FAILED) {
        // Auto-dismiss after 5 seconds (give user time to read)
        if (millis() - uploadStartTime > 5000) {
            uploadState = UPLOAD_IDLE;
            activeOp = OP_WPASEC;   // reset so stale op doesn't leak into bar title
            draw();
        }
    }

    // Refresh after overlays close (runs after state machine, never replaces it)
    if (Display::needsOverlayRedraw()) {
        draw();
    }
}

void next() {
    if (isPinEntry() || currentLootView == LOOT_NUKE) return;
    if (uploadState != UPLOAD_IDLE) return;  // don't scroll the list behind the upload modal
    if (currentLootView == LOOT_CRACKED) {
        if (potScroll < (int)potCount - 1) { potScroll++; draw(); }
        return;
    }
    if (currentLootView == LOOT_WIGLE) {
        if (wigleScroll < (int)wigleCsvCount - 1) { wigleScroll++; draw(); }
        return;
    }
    // allow cycling in detail view (button = next)
    int total = Capture::getTotalCount();
    if (total > 0) {
        currentIdx = (currentIdx + 1) % total;
        draw();
    }
}

void prev() {
    if (isPinEntry() || currentLootView == LOOT_NUKE) return;
    if (uploadState != UPLOAD_IDLE) return;  // don't scroll the list behind the upload modal
    if (currentLootView == LOOT_CRACKED) {
        if (potScroll > 0) { potScroll--; draw(); }
        return;
    }
    if (currentLootView == LOOT_WIGLE) {
        if (wigleScroll > 0) { wigleScroll--; draw(); }
        return;
    }
    if (inDetailView) {
        return;
    }

    int total = Capture::getTotalCount();
    if (total > 0) {
        currentIdx = (currentIdx - 1 + total) % total;
        draw();
    }
}

void select() {
    if (isPinEntry()) return;
    if (currentLootView != LOOT_LIST) return;  // detail only makes sense for captures
    if (!inDetailView && Capture::getTotalCount() > 0) {
        inDetailView = true;
        draw();
    }
}

void back() {
    if (inDetailView) {
        inDetailView = false;
        draw();
    }
}

bool isInDetailView() {
    return inDetailView;
}

static void draw() {
    if (!canvas) return;
    uint16_t fg = Display::getColorFG();
    uint16_t bg = Display::getColorBG();

    canvas->fillSprite(bg);
    canvas->setTextColor(fg);

    // ==[ TOP BAR ]== shared helper — title reflects operation when uploading
    // (Menu-Design.md: one title source per screen)
    const char* barTitle = "TH3 T4K3";
    if (uploadState != UPLOAD_IDLE) {
        barTitle = (activeOp == OP_POTFILE) ? "PULL P0TF1L3" :
                   (activeOp == OP_WIGLE)   ? "SH1P W4RDR1V3" :
                   (activeOp == OP_NUKE)    ? "D4T4 NUK3D"    :
                                              "SH1P TH3 L00T";
    }
    Display::drawStatusBarTo(canvas, barTitle);

    // ==[ PIN GATE ]== block until unlocked
    if (isPinEntry()) {
        PinEntry::draw(canvas, "3NT3R P1N", pinTaunt);
        Display::drawUiOverlaysTo(canvas);
        FramePresenter::present(*canvas);
        return;
    }

    // Check if showing upload view
    if (uploadState != UPLOAD_IDLE) {
        drawUploadView();
        Display::drawUiOverlaysTo(canvas);
        FramePresenter::present(*canvas);
        return;
    }

    // ==[ BREADCRUMB ]== shared across LIST / CRACKED / WIGLE / NUKE
    drawTabBreadcrumb();

    // Tab-specific views
    if (currentLootView == LOOT_CRACKED) {
        drawCracked();
        Display::drawUiOverlaysTo(canvas);
        FramePresenter::present(*canvas);
        return;
    }
    if (currentLootView == LOOT_WIGLE) {
        drawWigleView();
        Display::drawUiOverlaysTo(canvas);
        FramePresenter::present(*canvas);
        return;
    }
    if (currentLootView == LOOT_NUKE) {
        drawNukeConfirm();
        Display::drawUiOverlaysTo(canvas);
        FramePresenter::present(*canvas);
        return;
    }

    // reset for body
    canvas->setTextDatum(TL_DATUM);
    canvas->setTextSize(1);

    if (inDetailView) {
        drawDetail();
    } else {
        drawList();
    }

    Display::drawUiOverlaysTo(canvas);
    FramePresenter::present(*canvas);
}

static void drawList() {
    uint16_t fg = Display::getColorFG();
    uint16_t bg = Display::getColorBG();
    uint16_t dim = Display::lerpColor565(fg, bg, 0.5f);

    uint16_t pmkids = Capture::getPMKIDCount();
    uint16_t hsCount = Capture::getHandshakeCount();
    uint16_t total = pmkids + hsCount;
    uint16_t unsynced = Capture::getUnsyncedCount();
    // Counts are sampled separately and a callback may race between them,
    // so clamp rather than wrap under.
    uint16_t synced = (unsynced <= total) ? (uint16_t)(total - unsynced) : 0;
    char lifeP[12];
    char lifeH[12];
    char usedBuf[12];
    char capacityBuf[12];
    formatCompactCount(Config::getTotalPMKIDs(), lifeP, sizeof(lifeP));
    formatCompactCount(Config::getTotalHandshakes(), lifeH, sizeof(lifeH));
    const uint32_t usedBytes = Capture::getUsedBytes();
    const uint32_t capacityBytes = usedBytes + Capture::getFreeBytes();
    formatSize(usedBytes, usedBuf, sizeof(usedBuf));
    formatSize(capacityBytes, capacityBuf, sizeof(capacityBuf));

    // ==[ EVIDENCE SUMMARY ]== live locker first, lifetime ledger underneath.
    canvas->setTextSize(1);
    canvas->setTextColor(fg);
    canvas->setCursor(COL_LABEL, STATS_Y);
    canvas->printf("N0W P:%u H:%u", pmkids, hsCount);

    drawPrereqGlyphs(104, STATS_Y);

    // Current position stays on the live row; the second rail owns durability.
    if (total > 0) {
        canvas->setTextDatum(TR_DATUM);
        canvas->setTextColor(fg);
        char posStr[20];
        snprintf(posStr, sizeof(posStr), "%d/%d", currentIdx + 1, total);
        canvas->drawString(posStr, COL_RIGHT, STATS_Y);
        canvas->setTextDatum(TL_DATUM);
    }

    const int ledgerY = STATS_Y + 14;
    canvas->setTextColor(dim);
    canvas->setCursor(COL_LABEL, ledgerY);
    canvas->printf("L1F3 P:%s H:%s", lifeP, lifeH);
    canvas->setTextDatum(TR_DATUM);
    char durability[44];
    snprintf(durability, sizeof(durability), "S:%u U:%u %s/%s",
             synced, unsynced, usedBuf, capacityBuf);
    canvas->drawString(durability, COL_RIGHT, ledgerY);
    canvas->setTextDatum(TL_DATUM);
    canvas->setTextColor(fg);

    // ==[ EMPTY STATE ]== centered in playfield
    if (total == 0) {
        canvas->setTextColor(fg);
        canvas->setTextDatum(MC_DATUM);
        canvas->drawString("3MPTY. SK1LL 1SSU3?",
                           SCREEN_WIDTH / 2, SCREEN_HEIGHT / 2);
        canvas->setTextColor(dim);
        canvas->drawString("H1T HUNT. C4TCH H4ND5H4K3S.",
                           SCREEN_WIDTH / 2, SCREEN_HEIGHT / 2 + 14);
        canvas->setTextColor(fg);
        canvas->setTextDatum(TL_DATUM);

        // empty bottom bar: tab hint only
        Display::drawBottomBar3To(canvas, "", "TAB[>]", "");
        return;
    }

    // ==[ SCROLLABLE LIST ]== eight evidence-rich rows below both rails.
    const int ROW_H = 18;
    const int VISIBLE = 8;
    const int FIRST_Y = LIST_Y + 18;

    int startIdx = 0;
    if ((int)total > VISIBLE) {
        // keep selection comfortably visible
        startIdx = currentIdx - VISIBLE / 2;
        if (startIdx < 0) startIdx = 0;
        if (startIdx + VISIBLE > (int)total) startIdx = total - VISIBLE;
    }

    CapturedPMKID pmkid;
    CapturedHandshake hs;

    for (int i = 0; i < VISIBLE && (startIdx + i) < (int)total; i++) {
        int idx = startIdx + i;
        int rowY = FIRST_Y + i * ROW_H;
        bool selected = (idx == currentIdx);

        uint16_t rowFg = selected ? bg : fg;
        uint16_t rowBg = selected ? fg : bg;

        if (selected) {
            canvas->fillRect(0, rowY, SCREEN_WIDTH, ROW_H, rowBg);
        }
        canvas->setTextColor(rowFg);

        const char* typeTag;
        const char* ssid;
        bool entrySynced;
        char evidence[20] = "PMKID";
        uint32_t capturedAt = 0;

        if (idx < (int)pmkids && Capture::getPMKID(idx, &pmkid)) {
            typeTag = "P";
            ssid = pmkid.ssid;
            entrySynced = pmkid.synced;
            capturedAt = pmkid.timestamp;
        } else if (Capture::getHandshake(idx - pmkids, &hs)) {
            typeTag = "H";
            ssid = hs.ssid;
            entrySynced = hs.synced;
            capturedAt = hs.firstSeen;
            formatHandshakeEvidence(hs, evidence, sizeof(evidence));
        } else {
            continue;
        }

        // type tag on left
        canvas->setCursor(COL_LABEL + 2, rowY + 5);
        canvas->print(typeTag);

        // SSID leaves a fixed evidence column for pair quality + timestamp.
        const char* ssidOut = (ssid && ssid[0]) ? ssid : "(h1dd3n)";
        char ssidClip[25];
        snprintf(ssidClip, sizeof(ssidClip), "%.24s", ssidOut);
        canvas->setCursor(COL_LABEL + 14, rowY + 5);
        canvas->print(ssidClip);

        char timeBuf[8];
        char evidenceLine[30];
        formatCaptureTime(capturedAt, timeBuf, sizeof(timeBuf));
        snprintf(evidenceLine, sizeof(evidenceLine), "%s %s", evidence, timeBuf);
        canvas->setTextColor(selected ? rowFg : dim);
        canvas->setTextDatum(TR_DATUM);
        canvas->drawString(evidenceLine, COL_RIGHT - 40, rowY + 5);
        canvas->setTextDatum(TL_DATUM);

        // sync pill on right (shared Display helper — PND loud / SYN dim)
        Display::drawStatusPillTo(canvas, COL_RIGHT, rowY + 5,
                                   "PND", "SYN", entrySynced, selected);
        canvas->setTextColor(fg);
    }

    // scroll ticks on right edge (non-invasive, between rows + pill)
    if (startIdx > 0) {
        canvas->setTextColor(dim);
        canvas->setCursor(COL_RIGHT - 4, FIRST_Y - 9);
        canvas->print("^");
    }
    if (startIdx + VISIBLE < (int)total) {
        // The v-tick lands inside the last visible row. If that row is the
        // current selection, dim-on-fg is unreadable — flip to bg-on-fg.
        bool lastVisibleSelected = ((startIdx + VISIBLE - 1) == currentIdx);
        canvas->setTextColor(lastVisibleSelected ? bg : dim);
        canvas->setCursor(COL_RIGHT - 4, FIRST_Y + VISIBLE * ROW_H - 1);
        canvas->print("v");
    }
    canvas->setTextColor(fg);

    // ==[ BOTTOM BAR ]==
    if (!Display::drawHintBottomBar(canvas)) {
        Display::drawBottomBar3To(canvas, "[A/C]SCR", "[B]V13W  [B+]UP", "TAB[>]");
    }
}

static void drawDetail() {
    uint16_t fg = Display::getColorFG();
    uint16_t bg = Display::getColorBG();
    uint16_t pmkids = Capture::getPMKIDCount();

    CapturedPMKID pmkid;
    CapturedHandshake handshake;

    int y = TOP_BAR_H + 36;  // Shifted down for top bar

    if (currentIdx < pmkids && Capture::getPMKID(currentIdx, &pmkid)) {
        // ==[ PMKID DETAIL - COMPACT LAYOUT ]==
        // Line 1: title + timestamp (right-aligned)
        char timeBuf[8];
        formatCaptureTime(pmkid.timestamp, timeBuf, sizeof(timeBuf));

        canvas->setTextColor(fg);
        canvas->setCursor(COL_LABEL, y);
        canvas->printf("PMKID #%d: %.16s", currentIdx + 1, pmkid.ssid);
        canvas->setTextDatum(TR_DATUM);
        canvas->drawString(timeBuf, COL_RIGHT, y);
        canvas->setTextDatum(TL_DATUM);

        // Line 2: full PMKID hex (32 chars = 192px at size1, fits!)
        y += LINE_H;
        canvas->setCursor(COL_LABEL, y);
        for (int i = 0; i < 16; i++) {
            canvas->printf("%02X", pmkid.pmkid[i]);
        }

        // Line 3: BSSID + channel
        y += LINE_H;
        char bssidStr[18];
        RFUtil::formatMAC(bssidStr, pmkid.bssid);
        canvas->setCursor(COL_LABEL, y);
        canvas->printf("AP: %s", bssidStr);

        // Line 4: STA
        y += LINE_H;
        char staStr[18];
        RFUtil::formatMAC(staStr, pmkid.station);
        canvas->setCursor(COL_LABEL, y);
        canvas->printf("STA: %s", staStr);

        // Line 5: sync status + format hint
        y += LINE_H;
        if (pmkid.synced) {
            canvas->setTextColor(Display::lerpColor565(fg, bg, 0.5f));
            canvas->setCursor(COL_LABEL, y);
            canvas->print("[SYNCED]");
            canvas->setTextColor(fg);
        } else {
            const char* lbl = "[PENDING]";
            int w = (int)strlen(lbl) * 6 + 4;
            canvas->fillRect(COL_LABEL - 2, y - 1, w, 10, fg);
            canvas->setTextColor(bg);
            canvas->setCursor(COL_LABEL, y);
            canvas->print(lbl);
            canvas->setTextColor(fg);
        }
        canvas->setTextDatum(TR_DATUM);
        canvas->drawString("HC22000", COL_RIGHT, y);
        canvas->setTextDatum(TL_DATUM);

    } else if (Capture::getHandshake(currentIdx - pmkids, &handshake)) {
        // ==[ HANDSHAKE DETAIL - COMPACT LAYOUT ]==
        // Line 1: title + timestamp (right-aligned)
        char timeBuf[8];
        formatCaptureTime(handshake.firstSeen, timeBuf, sizeof(timeBuf));

        canvas->setTextColor(fg);
        canvas->setCursor(COL_LABEL, y);
        canvas->printf("HS #%d: %.18s", currentIdx + 1 - pmkids, handshake.ssid);
        canvas->setTextDatum(TR_DATUM);
        canvas->drawString(timeBuf, COL_RIGHT, y);
        canvas->setTextDatum(TL_DATUM);

        // Lines 2-3: 2 messages per row, fixed columns
        // COL_LABEL=4, second msg at fixed x=124 (20 chars in)
        static const int COL_M2 = 124;

        y += LINE_H;
        canvas->setCursor(COL_LABEL, y);
        if (handshake.hasMessage(1)) {
            canvas->printf("M1:%3db %4ddB", handshake.frames[0].len, handshake.frames[0].rssi);
        } else {
            canvas->print("M1:---");
        }
        canvas->setCursor(COL_M2, y);
        if (handshake.hasMessage(2)) {
            canvas->printf("M2:%3db %4ddB", handshake.frames[1].len, handshake.frames[1].rssi);
        } else {
            canvas->print("M2:---");
        }

        y += LINE_H;
        canvas->setCursor(COL_LABEL, y);
        if (handshake.hasMessage(3)) {
            canvas->printf("M3:%3db %4ddB", handshake.frames[2].len, handshake.frames[2].rssi);
        } else {
            canvas->print("M3:---");
        }
        canvas->setCursor(COL_M2, y);
        if (handshake.hasMessage(4)) {
            canvas->printf("M4:%3db %4ddB", handshake.frames[3].len, handshake.frames[3].rssi);
        } else {
            canvas->print("M4:---");
        }

        // Line 4: validity + key version + message pair
        y += LINE_H;
        canvas->setCursor(COL_LABEL, y);
        // find best EAPOL frame for key version (prefer M2, fallback M1/M3)
        const EAPOLFrame* kvFrame = nullptr;
        if (handshake.hasMessage(2)) kvFrame = &handshake.frames[1];
        else if (handshake.hasMessage(1)) kvFrame = &handshake.frames[0];
        else if (handshake.hasMessage(3)) kvFrame = &handshake.frames[2];
        const char* kv = getKeyVerStr(kvFrame);
        if (handshake.isComplete()) {
            if (handshake.hasReliableSNonce()) {
                canvas->printf("[V4L1D %s]", kv);
            } else {
                canvas->printf("[R1SKY %s]", kv);
            }
        } else {
            canvas->print("[1NC0MPL3T3]");
        }
        // right side: best message pair
        if (handshake.isComplete()) {
            canvas->setTextDatum(TR_DATUM);
            canvas->drawString(getMsgPairStr(&handshake), COL_RIGHT, y);
            canvas->setTextDatum(TL_DATUM);
        }

        // Line 5: sync status
        y += LINE_H;
        if (handshake.synced) {
            canvas->setTextColor(Display::lerpColor565(fg, bg, 0.5f));
            canvas->setCursor(COL_LABEL, y);
            canvas->print("[SYNCED]");
            canvas->setTextColor(fg);
        } else {
            const char* lbl = "[PENDING]";
            int w = (int)strlen(lbl) * 6 + 4;
            canvas->fillRect(COL_LABEL - 2, y - 1, w, 10, fg);
            canvas->setTextColor(bg);
            canvas->setCursor(COL_LABEL, y);
            canvas->print(lbl);
            canvas->setTextColor(fg);
        }
    }

    // ==[ BOTTOM BAR ]== detail view
    Display::drawBottomBar3To(canvas, "[A] N3XT", "", "[C] B4CK");
}

static void drawUploadView() {
    uint16_t fg = Display::getColorFG();
    uint16_t bg = Display::getColorBG();

    // title moved to status bar (Menu-Design.md: one title source)
    // keep the body separator so status area reads cleanly
    canvas->drawLine(20, TOP_BAR_H + 2, SCREEN_WIDTH - 20, TOP_BAR_H + 2, fg);

    // status message - centered
    canvas->setTextDatum(MC_DATUM);
    canvas->setTextSize(1);

    int centerY = SCREEN_HEIGHT / 2 + 5;

    switch (uploadState) {
        case UPLOAD_CONNECTING:
            canvas->drawString("SN1FF1NG W1F1...", SCREEN_WIDTH / 2, centerY);
            break;

        case UPLOAD_SENDING:
            canvas->drawString(uploadMessage, SCREEN_WIDTH / 2, centerY);
            break;

        case UPLOAD_SUCCESS:
            canvas->setTextColor(fg);
            canvas->drawString(uploadMessage, SCREEN_WIDTH / 2, centerY);
            break;

        case UPLOAD_FAILED:
            canvas->setTextColor(fg);
            canvas->drawString(uploadMessage, SCREEN_WIDTH / 2, centerY);
            break;

        default:
            break;
    }

    // ==[ BOTTOM BAR ]==
    // SENDING is a blocking HTTP call — main loop is parked, cancel can't fire.
    // Only promise what we can keep.
    if (uploadState == UPLOAD_CONNECTING) {
        Display::drawBottomBar3To(canvas, "", "[B] CANCEL", "");
    } else if (uploadState == UPLOAD_SENDING) {
        Display::drawBottomBar3To(canvas, "", "SH1PP1NG...", "");
    } else {
        Display::drawBottomBar3To(canvas, "", "[B] BACK", "");
    }
}

void startUpload() {
    activeOp = OP_WPASEC;
    // Check prerequisites
    if (!Config::hasWpaSecKey()) {
        uploadState = UPLOAD_FAILED;
        snprintf(uploadMessage, sizeof(uploadMessage), "N0 K3Y. TUN3 P1G F1RST.");
        uploadStartTime = millis();
        draw();
        return;
    }

    if (!Config::hasUploadWifi()) {
        uploadState = UPLOAD_FAILED;
        snprintf(uploadMessage, sizeof(uploadMessage), "N0 W1F1. TUN3 P1G F1RST.");
        uploadStartTime = millis();
        draw();
        return;
    }

    uint16_t handshakes = Capture::getUnsyncedHandshakeCount();
    if (handshakes == 0) {
        uploadState = UPLOAD_FAILED;
        if (Capture::getUnsyncedPMKIDCount() > 0) {
            snprintf(uploadMessage, sizeof(uploadMessage), "WP4-S3C N33DS PCAP.");
        } else {
            snprintf(uploadMessage, sizeof(uploadMessage), "N0 N3W L00T 2 SH1P.");
        }
        uploadStartTime = millis();
        draw();
        return;
    }

    // Start connection
    uploadState = UPLOAD_CONNECTING;
    uploadStartTime = millis();
    snprintf(uploadMessage, sizeof(uploadMessage), "C0NN3CT1NG...");

    // Begin WiFi connection
    if (!WifiClient::connect(Config::getUploadWifiSsid(), Config::getUploadWifiPass())) {
        uploadState = UPLOAD_FAILED;
        setWifiFailureMessage();
        WifiClient::disconnect();
        uploadStartTime = millis();
    }
    draw();
}

bool isUploading() {
    return uploadState == UPLOAD_CONNECTING || uploadState == UPLOAD_SENDING;
}

bool isUploadModalActive() {
    return uploadState != UPLOAD_IDLE;
}

void cancelUpload() {
    if (isUploading()) {
        WifiClient::disconnect();
        uploadState = UPLOAD_IDLE;
        activeOp = OP_WPASEC;   // don't let OP_NUKE / OP_POTFILE leak into bar title
        draw();
    } else if (uploadState == UPLOAD_SUCCESS || uploadState == UPLOAD_FAILED) {
        uploadState = UPLOAD_IDLE;
        activeOp = OP_WPASEC;
        draw();
    }
}

// ==[ CRACKED VIEW ]== parse potfile into PSRAM, cross-ref with captures

static void loadCrackedView() {
    // free old entries
    if (potEntries) {
        heap_caps_free(potEntries);
        potEntries = nullptr;
        potCount = 0;
    }
    potScroll = 0;

    // check potfile exists
    if (!SD.exists("/hamlet/export/wpasec.pot")) return;

    File f = SD.open("/hamlet/export/wpasec.pot", FILE_READ);
    if (!f) return;

    // count lines first
    uint16_t lineCount = 0;
    while (f.available()) {
        if (f.read() == '\n') lineCount++;
    }
    f.seek(0);

    uint16_t maxEntries = (lineCount < 500) ? lineCount : 500;
    potfileTruncated = (lineCount > 500);
    size_t allocSize = maxEntries * sizeof(PotEntry);
    potEntries = (PotEntry*)heap_caps_malloc(allocSize, MALLOC_CAP_SPIRAM);
    if (!potEntries) { f.close(); return; }
    memset(potEntries, 0, allocSize);

    // build BSSID lookup from captures for cross-ref
    uint16_t pmkCount = Capture::getPMKIDCount();

    char lineBuf[180];
    int lineLen = 0;
    while (f.available() && potCount < maxEntries) {
        char c = f.read();
        if (c == '\n' || c == '\r') {
            if (lineLen == 0) continue;
            lineBuf[lineLen] = '\0';

            // wpa-sec potfile: AP_BSSID(12hex):STA(12hex):SSID:password
            // 1st colon at 12, 2nd at 25, then LAST colon separates SSID from password
            // (SSIDs can legitimately contain ':', passwords rarely do in the wild).
            const char* c1 = strchr(lineBuf, ':');
            if (!c1 || (c1 - lineBuf) != 12) { lineLen = 0; continue; }
            const char* c2 = strchr(c1 + 1, ':');
            if (!c2 || (c2 - lineBuf) != 25) { lineLen = 0; continue; }
            const char* c3 = strrchr(c2 + 1, ':');
            if (!c3) { lineLen = 0; continue; }

            PotEntry& e = potEntries[potCount];

            // bssid: convert 12-char hex to AA:BB:CC:DD:EE:FF for display + cross-ref
            for (int i = 0; i < 6; i++) {
                e.bssid[i*3]   = toupper(lineBuf[i*2]);
                e.bssid[i*3+1] = toupper(lineBuf[i*2+1]);
                e.bssid[i*3+2] = (i < 5) ? ':' : '\0';
            }
            e.bssid[17] = '\0';

            // ssid: between 2nd and 3rd colon
            int ssidLen = (int)(c3 - (c2 + 1));
            if (ssidLen > 32) ssidLen = 32;
            strncpy(e.ssid, c2 + 1, ssidLen);
            e.ssid[ssidLen] = '\0';

            // pass: everything after 3rd colon
            int passLen = lineLen - (int)(c3 + 1 - lineBuf);
            if (passLen > 64) passLen = 64;
            strncpy(e.pass, c3 + 1, passLen);
            e.pass[passLen] = '\0';

            // cross-ref: check if bssid matches any of our PMKIDs
            e.captured = false;
            for (uint16_t p = 0; p < pmkCount && !e.captured; p++) {
                CapturedPMKID pmkid;
                if (Capture::getPMKID(p, &pmkid)) {
                    char capBssid[18];
                    snprintf(capBssid, sizeof(capBssid), "%02X:%02X:%02X:%02X:%02X:%02X",
                             pmkid.bssid[0], pmkid.bssid[1], pmkid.bssid[2],
                             pmkid.bssid[3], pmkid.bssid[4], pmkid.bssid[5]);
                    if (strcasecmp(capBssid, e.bssid) == 0) e.captured = true;
                }
            }

            potCount++;
            lineLen = 0;
        } else if (lineLen < 179) {
            lineBuf[lineLen++] = c;
        } else {
            lineLen = 0;  // overlong line, discard
        }
    }
    f.close();
}

static void drawCracked() {
    uint16_t fg = Display::getColorFG();
    uint16_t bg = Display::getColorBG();
    uint16_t dim = Display::lerpColor565(fg, bg, 0.5f);

    canvas->setTextDatum(TL_DATUM);
    canvas->setTextSize(1);

    if (potCount == 0) {
        canvas->setTextColor(fg);
        canvas->setTextDatum(MC_DATUM);
        canvas->drawString("N0 P0TF1L3.", SCREEN_WIDTH / 2, SCREEN_HEIGHT / 2 - 10);
        canvas->setTextColor(dim);
        canvas->drawString("[B] PULL P0TF1L3 FR0M WP4-S3C.",
                           SCREEN_WIDTH / 2, SCREEN_HEIGHT / 2 + 8);
        canvas->setTextColor(fg);
        canvas->setTextDatum(TL_DATUM);
    } else {
        // counts + prereq glyphs
        uint16_t oursCount = 0;
        for (uint16_t i = 0; i < potCount; i++) if (potEntries[i].captured) oursCount++;

        canvas->setTextColor(fg);
        canvas->setCursor(COL_LABEL, STATS_Y);
        // trailing '+' marks that the potfile was truncated at MAX (500)
        canvas->printf("%u%s CR4CK  %u 0URS",
                       potCount, potfileTruncated ? "+" : "", oursCount);

        drawPrereqGlyphs(140, STATS_Y);

        // position indicator
        canvas->setTextDatum(TR_DATUM);
        char posStr[16];
        snprintf(posStr, sizeof(posStr), "%d/%u", potScroll + 1, potCount);
        canvas->drawString(posStr, COL_RIGHT, STATS_Y);
        canvas->setTextDatum(TL_DATUM);

        // scrollable list: visible rows
        const int ROW_H = 14;
        const int FIRST_ROW_Y = LIST_Y + 2;
        const int LAST_ROW_Y = SCREEN_HEIGHT - BOTTOM_BAR_H - ROW_H;
        int visRows = (LAST_ROW_Y - FIRST_ROW_Y) / ROW_H;
        if (visRows < 1) visRows = 1;

        for (int i = 0; i < visRows && (potScroll + i) < (int)potCount; i++) {
            int idx = potScroll + i;
            int y = FIRST_ROW_Y + i * ROW_H;
            bool ours = potEntries[idx].captured;
            canvas->setTextColor(ours ? fg : dim);

            // leading marker: ">" if ours, else " "
            canvas->setCursor(COL_LABEL, y);
            canvas->print(ours ? ">" : " ");

            // ssid + pass
            char row[40];
            snprintf(row, sizeof(row), "%-16.16s %-16.16s",
                     potEntries[idx].ssid, potEntries[idx].pass);
            canvas->drawString(row, COL_LABEL + 8, y);
        }

        // scroll ticks on right edge — mirror drawList()
        if (potScroll > 0) {
            canvas->setTextColor(dim);
            canvas->setCursor(COL_RIGHT - 4, FIRST_ROW_Y - 9);
            canvas->print("^");
        }
        if (potScroll + visRows < (int)potCount) {
            canvas->setTextColor(dim);
            canvas->setCursor(COL_RIGHT - 4, FIRST_ROW_Y + visRows * ROW_H - 1);
            canvas->print("v");
        }
        canvas->setTextColor(fg);
    }

    // bottom bar — [A]action [C]scroll [TAB]cycle
    Display::drawBottomBar3To(canvas, "[A]PULL", "[C]SCR", "TAB[>]");
}

// ==[ WIGLE VIEW ]== wardrive CSV list + cached stats

static bool endsWithIgnoreCase(const char* s, const char* suffix) {
    if (!s || !suffix) return false;
    size_t sl = strlen(s);
    size_t tl = strlen(suffix);
    if (tl > sl) return false;
    s += sl - tl;
    for (size_t i = 0; i < tl; i++) {
        if (tolower((unsigned char)s[i]) != tolower((unsigned char)suffix[i])) return false;
    }
    return true;
}

static bool isCsvFilename(const char* name) {
    if (!name) return false;
    const char* base = strrchr(name, '/');
    base = base ? base + 1 : name;
    if (!base[0] || base[0] == '.') return false;
    return endsWithIgnoreCase(base, ".csv");
}

static void buildWardrivePath(const char* name, char* out, size_t outSize) {
    if (!name || !out || outSize == 0) return;
    if (strchr(name, '/')) {
        snprintf(out, outSize, "%s", name);
    } else {
        snprintf(out, outSize, "/hamlet/wardrive/%s", name);
    }
}

static bool countCsvDataLines(const char* path, uint32_t* rowsOut) {
    if (rowsOut) *rowsOut = 0;
    File f = SD.open(path);
    if (!f) return false;

    char hdr[16] = {};
    size_t got = f.readBytes(hdr, sizeof(hdr) - 1);
    hdr[got] = '\0';
    if (strncmp(hdr, "WigleWifi-", 10) != 0) {
        f.close();
        return false;
    }

    f.seek(0);
    uint32_t lines = 0;
    int c;
    bool atLineStart = true;
    while ((c = f.read()) >= 0) {
        if (c == '\n') { lines++; atLineStart = true; }
        else atLineStart = false;
    }
    // don't count trailing newline as extra line
    if (!atLineStart) lines++;
    f.close();
    uint32_t rows = (lines > 2) ? lines - 2 : 0;  // subtract 2 header lines
    if (rowsOut) *rowsOut = rows;
    return true;
}

static void loadWigleView() {
    wigleCsvCount = 0;
    wigleScroll = 0;
    wigleRowCount = 0;
    wigleBytes = 0;
    memset(wigleStateCounts, 0, sizeof(wigleStateCounts));
    wigleFilesTruncated = false;
    File dir = SD.open("/hamlet/wardrive");
    if (dir && dir.isDirectory()) {
        File f = dir.openNextFile();
        while (f && wigleCsvCount < MAX_WIGLE_FILES) {
            if (!f.isDirectory() && isCsvFilename(f.name())) {
                WigleCsvInfo& info = wigleFiles[wigleCsvCount];
                // Own the basename before close(); File::name() storage is not
                // required to survive the next directory call on every SD API.
                const char* fname = f.name();
                const char* slash = strrchr(fname, '/');
                if (slash) fname = slash + 1;
                char basename[64];
                snprintf(basename, sizeof(basename), "%s", fname);

                size_t len = strlen(basename);
                if (len > 4) len -= 4;  // strip ".csv"
                if (len > sizeof(info.name) - 1) len = sizeof(info.name) - 1;
                memcpy(info.name, basename, len);
                info.name[len] = '\0';
                info.sizeBytes = f.size();
                // count data rows
                char fullPath[64];
                buildWardrivePath(f.name(), fullPath, sizeof(fullPath));
                f.close();
                if (countCsvDataLines(fullPath, &info.rows)) {
                    wigleFileStates[wigleCsvCount] = WiGLE::getFileState(basename);
                    wigleRowCount += info.rows;
                    wigleBytes += info.sizeBytes;
                    uint8_t stateIdx = static_cast<uint8_t>(wigleFileStates[wigleCsvCount]);
                    if (stateIdx < 4) wigleStateCounts[stateIdx]++;
                    wigleCsvCount++;
                }
            } else {
                f.close();
            }
            f = dir.openNextFile();
        }
        // If we exited because we hit MAX_WIGLE_FILES, `f` is the next entry
        // that we never got to close — release it before closing the dir.
        if (f) {
            wigleFilesTruncated = true;
            f.close();
        }
        dir.close();
    }
    wigleStats = WiGLE::getCachedStats();
    // The view now owns a compact snapshot; release both ledgers instead of
    // making the rest of the firmware pay PSRAM rent for a closed drawer.
    WiGLE::freeUploadedListMemory();
}

static const char* wigleStateLabel(WiGLE::FileState state) {
    switch (state) {
        case WiGLE::FileState::PENDING:   return "PND";
        case WiGLE::FileState::CONFIRMED: return "UP";
        case WiGLE::FileState::REJECTED:  return "REJ";
        case WiGLE::FileState::LOCAL:
        default:                          return "LOC";
    }
}

static void drawWigleState(int rowY, WiGLE::FileState state) {
    const uint16_t fg = Display::getColorFG();
    const uint16_t bg = Display::getColorBG();
    const uint16_t dim = Display::lerpColor565(fg, bg, 0.5f);
    const char* label = wigleStateLabel(state);
    const bool loud = state == WiGLE::FileState::PENDING ||
                      state == WiGLE::FileState::REJECTED;
    const int width = (int)strlen(label) * 6 + (loud ? 4 : 0);

    if (loud) {
        canvas->fillRect(COL_RIGHT - width, rowY - 1, width, 10, fg);
        canvas->setTextColor(bg);
        canvas->setTextDatum(MC_DATUM);
        canvas->drawString(label, COL_RIGHT - width / 2, rowY + 4);
    } else {
        canvas->setTextColor(state == WiGLE::FileState::CONFIRMED ? dim : fg);
        canvas->setTextDatum(TR_DATUM);
        canvas->drawString(label, COL_RIGHT, rowY);
    }
    canvas->setTextDatum(TL_DATUM);
    canvas->setTextColor(fg);
}

static void drawWigleView() {
    uint16_t fg = Display::getColorFG();
    uint16_t bg = Display::getColorBG();
    uint16_t dim = Display::lerpColor565(fg, bg, 0.5f);

    canvas->setTextDatum(TL_DATUM);
    canvas->setTextSize(1);

    // First rail is durable lifetime truth, with cached WiGLE account context.
    char lifeNets[12];
    char remoteNets[12];
    formatCompactCount(Config::getWDTotal(), lifeNets, sizeof(lifeNets));
    formatCompactCount(wigleStats.wifiNets, remoteNets, sizeof(remoteNets));
    canvas->setTextColor(fg);
    canvas->setCursor(COL_LABEL, STATS_Y);
    canvas->printf("L1F3:%sN %u RUNS", lifeNets, Config::getWDSessions());
    if (wigleStats.valid) {
        char remote[32];
        snprintf(remote, sizeof(remote), "WG:%s RNK:%lu",
                 remoteNets, (unsigned long)wigleStats.rank);
        canvas->setTextDatum(TR_DATUM);
        canvas->drawString(remote, COL_RIGHT, STATS_Y);
        canvas->setTextDatum(TL_DATUM);
    }

    // Second rail inventories the local SD evidence and its receipt states.
    char rowBuf[12];
    char sizeBuf[12];
    formatCompactCount(wigleRowCount, rowBuf, sizeof(rowBuf));
    formatSize(wigleBytes, sizeBuf, sizeof(sizeBuf));
    canvas->setTextColor(dim);
    canvas->setCursor(COL_LABEL, LIST_Y);
    canvas->printf("SD:%u%sF %sR %s", wigleCsvCount,
                   wigleFilesTruncated ? "+" : "", rowBuf, sizeBuf);
    char states[40];
    snprintf(states, sizeof(states), "UP:%u P:%u L:%u X:%u",
             wigleStateCounts[(uint8_t)WiGLE::FileState::CONFIRMED],
             wigleStateCounts[(uint8_t)WiGLE::FileState::PENDING],
             wigleStateCounts[(uint8_t)WiGLE::FileState::LOCAL],
             wigleStateCounts[(uint8_t)WiGLE::FileState::REJECTED]);
    canvas->setTextDatum(TR_DATUM);
    canvas->drawString(states, COL_RIGHT, LIST_Y);
    canvas->setTextDatum(TL_DATUM);

    if (wigleCsvCount == 0) {
        canvas->setTextColor(fg);
        canvas->setTextDatum(MC_DATUM);
        canvas->drawString(SDStorage::isAvailable() ? "N0 W4RDR1V3S." : "SD 0FFL1N3.",
                           SCREEN_WIDTH / 2, SCREEN_HEIGHT / 2 - 4);
        canvas->setTextColor(dim);
        canvas->drawString(SDStorage::isAvailable()
                               ? "H1T TH3 R04D. L0G B344C0NS."
                               : "W4RDR1V3 F1L3S L1V3 0N C4RD.",
                           SCREEN_WIDTH / 2, SCREEN_HEIGHT / 2 + 14);
        canvas->setTextColor(fg);
        canvas->setTextDatum(TL_DATUM);
    } else {
        // Prerequisite glyphs stay close to the inventory they gate.
        int glyphX = 150;
        if (!Config::hasUploadWifi()) {
            canvas->fillRect(glyphX, LIST_Y - 1, 14, 10, fg);
            canvas->setTextColor(bg);
            canvas->drawString("!W", glyphX + 2, LIST_Y);
            glyphX += 17;
        }
        if (!Config::hasWigleCredentials()) {
            canvas->fillRect(glyphX, LIST_Y - 1, 14, 10, fg);
            canvas->setTextColor(bg);
            canvas->drawString("!C", glyphX + 2, LIST_Y);
        }
        canvas->setTextColor(fg);

        // scrollable file list
        const int ROW_H = 18;
        const int FIRST_ROW_Y = LIST_Y + 18;
        int visRows = (SCREEN_HEIGHT - BOTTOM_BAR_H - FIRST_ROW_Y) / ROW_H;
        if (visRows < 1) visRows = 1;

        for (int i = 0; i < visRows && (wigleScroll + i) < (int)wigleCsvCount; i++) {
            int idx = wigleScroll + i;
            const WigleCsvInfo& info = wigleFiles[idx];
            int rowY = FIRST_ROW_Y + i * ROW_H;

            // file name (left)
            canvas->setTextColor(fg);
            char nameClip[25];
            snprintf(nameClip, sizeof(nameClip), "%.24s", info.name);
            canvas->drawString(nameClip, COL_LABEL, rowY + 4);

            // CSV row count + size + durable submission state.
            char fileSize[10];
            formatSize(info.sizeBytes, fileSize, sizeof(fileSize));
            char detail[32];
            snprintf(detail, sizeof(detail), "%lur %s",
                     (unsigned long)info.rows, fileSize);
            canvas->setTextColor(dim);
            canvas->setTextDatum(TR_DATUM);
            canvas->drawString(detail, COL_RIGHT - 34, rowY + 4);
            canvas->setTextDatum(TL_DATUM);
            drawWigleState(rowY + 4, wigleFileStates[idx]);
        }

        // scroll ticks on right edge — mirror drawList()
        if (wigleScroll > 0) {
            canvas->setTextColor(dim);
            canvas->setCursor(COL_RIGHT - 4, FIRST_ROW_Y - 9);
            canvas->print("^");
        }
        if (wigleScroll + visRows < (int)wigleCsvCount) {
            canvas->setTextColor(dim);
            canvas->setCursor(COL_RIGHT - 4, FIRST_ROW_Y + visRows * ROW_H - 1);
            canvas->print("v");
        }
        canvas->setTextColor(fg);
    }

    // bottom bar — [A]ship [C]scroll [TAB]cycle
    Display::drawBottomBar3To(canvas, "[A]SH1P", "[C]SCR", "TAB[>]");
}

// ==[ TAB CYCLE ]==

void handleBtnA() {
    if (isPinEntry() || currentLootView == LOOT_NUKE) return;
    if (uploadState != UPLOAD_IDLE) return;
    if (currentLootView == LOOT_CRACKED || currentLootView == LOOT_WIGLE) {
        triggerAction();
    } else {
        prev();
    }
}

void triggerAction() {
    if (currentLootView == LOOT_CRACKED) {
        startPotfileDownload();
    } else if (currentLootView == LOOT_WIGLE) {
        startWigleUpload();
    } else {
        startUpload();  // original WPA-SEC upload
    }
}

void cycleTab() {
    if (uploadState != UPLOAD_IDLE) return;  // no tab shuffles while the network is busy
    if (currentLootView == LOOT_LIST) {
        currentLootView = LOOT_CRACKED;
        loadCrackedView();
    } else if (currentLootView == LOOT_CRACKED) {
        currentLootView = LOOT_WIGLE;
        loadWigleView();
    } else if (currentLootView == LOOT_WIGLE) {
        currentLootView = LOOT_NUKE;
    } else {
        currentLootView = LOOT_LIST;
    }
    potScroll = 0;
    draw();
}

void startPotfileDownload() {
    activeOp = OP_POTFILE;
    if (!Config::hasWpaSecKey()) {
        uploadState = UPLOAD_FAILED;
        snprintf(uploadMessage, sizeof(uploadMessage), "N0 K3Y. TUN3 P1G F1RST.");
        uploadStartTime = millis();
        draw();
        return;
    }
    if (!Config::hasUploadWifi()) {
        uploadState = UPLOAD_FAILED;
        snprintf(uploadMessage, sizeof(uploadMessage), "N0 W1F1. TUN3 P1G F1RST.");
        uploadStartTime = millis();
        draw();
        return;
    }
    uploadState = UPLOAD_CONNECTING;
    uploadStartTime = millis();
    snprintf(uploadMessage, sizeof(uploadMessage), "C0NN3CT1NG...");
    if (!WifiClient::connect(Config::getUploadWifiSsid(), Config::getUploadWifiPass())) {
        uploadState = UPLOAD_FAILED;
        setWifiFailureMessage();
        WifiClient::disconnect();
        uploadStartTime = millis();
    }
    draw();
}

void startWigleUpload() {
    activeOp = OP_WIGLE;
    if (!Config::hasWigleCredentials()) {
        uploadState = UPLOAD_FAILED;
        snprintf(uploadMessage, sizeof(uploadMessage), "N0 W1GL3 CR3DS. TUN3 P1G.");
        uploadStartTime = millis();
        draw();
        return;
    }
    if (!Config::hasUploadWifi()) {
        uploadState = UPLOAD_FAILED;
        snprintf(uploadMessage, sizeof(uploadMessage), "N0 W1F1. TUN3 P1G F1RST.");
        uploadStartTime = millis();
        draw();
        return;
    }
    uploadState = UPLOAD_CONNECTING;
    uploadStartTime = millis();
    snprintf(uploadMessage, sizeof(uploadMessage), "C0NN3CT1NG...");
    if (!WifiClient::connect(Config::getUploadWifiSsid(), Config::getUploadWifiPass())) {
        uploadState = UPLOAD_FAILED;
        setWifiFailureMessage();
        WifiClient::disconnect();
        uploadStartTime = millis();
    }
    draw();
}

// ==[ PIN GATE ]==

bool isPinEntry() {
    return PinEntry::isActive() && !pinUnlocked;
}

static void setPinTaunt(uint8_t strikes) {
    if (strikes <= 1) {
        snprintf(pinTaunt, sizeof(pinTaunt), "N1C3 TRY. V4ULT Y4WNS.");
    } else if (strikes == 2) {
        snprintf(pinTaunt, sizeof(pinTaunt), "TW0 STR1K3S. JAN1T0R S1GHS.");
    } else {
        snprintf(pinTaunt, sizeof(pinTaunt), "ST1LL TYP1NG? BR4V3.");
    }
}

void pinInput(char btn) {
    if (!isPinEntry()) return;
    PinEntry::handleButton(btn);
    if (PinEntry::isComplete()) {
        char code[5];
        PinEntry::getCode(code);
        char stored[5];
        Config::getLootPin(stored);
        if (strcmp(code, stored) == 0) {
            pinUnlocked = true;
            wrongPinCount = 0;
            pinTaunt[0] = '\0';
            PinEntry::cancel();
        } else {
            if (wrongPinCount < 255) wrongPinCount++;
            setPinTaunt(wrongPinCount);
            PinEntry::setWrong();
            if (wrongPinCount >= 2 && !pinDeadmanNuked) {
                pinDeadmanNuked = true;
                executeNuke(false);
            }
        }
    }
    draw();
}

// ==[ NUKE ]==

bool isNukeConfirm() { return currentLootView == LOOT_NUKE; }

void showNuke() {
    currentLootView = LOOT_NUKE;
    draw();
}

void confirmNuke() {
    if (currentLootView != LOOT_NUKE) return;
    executeNuke();
    draw();
}

void cancelNuke() {
    currentLootView = LOOT_LIST;
    draw();
}

static bool buildNukeChildPath(const char* dirPath, const char* name,
                               char* out, size_t outSize) {
    if (!dirPath || !name || !name[0] || !out || outSize == 0) return false;
    int written = 0;
    if (name[0] == '/') {
        written = snprintf(out, outSize, "%s", name);
    } else {
        written = snprintf(out, outSize, "%s/%s", dirPath, name);
    }
    return written > 0 && (size_t)written < outSize;
}

// Returns the count of paths that failed to delete (0 = fully wiped).
static uint16_t nukeDirContents(const char* dirPath, uint8_t depth = 0) {
    if (depth > 6) return 1;  // data nuke, not a spelunking expedition.
    File dir = SD.open(dirPath);
    if (!dir) return 0;
    if (!dir.isDirectory()) {
        dir.close();
        return 0;
    }
    dir.close();

    uint16_t failures = 0;
    uint16_t guard = 0;
    static constexpr uint16_t MAX_NUKE_PASSES = 4096;

    while (guard++ < MAX_NUKE_PASSES) {
        File scan = SD.open(dirPath);
        if (!scan || !scan.isDirectory()) {
            if (scan) scan.close();
            break;
        }

        File child = scan.openNextFile();
        if (!child) {
            scan.close();
            break;
        }

        char childPath[128];
        bool childDir = child.isDirectory();
        bool pathOk = buildNukeChildPath(dirPath, child.name(),
                                         childPath, sizeof(childPath));
        child.close();
        scan.close();

        if (!pathOk) {
            failures++;
            break;
        }

        if (childDir) {
            failures += nukeDirContents(childPath, depth + 1);
            if (!SD.rmdir(childPath) && SD.exists(childPath)) {
                failures++;
                break;
            }
        } else if (!SD.remove(childPath) && SD.exists(childPath)) {
            failures++;
            break;
        }
    }

    if (guard >= MAX_NUKE_PASSES) failures++;
    return failures;
}

static void executeNuke(bool showResult) {
    // 1. clear PSRAM captures
    Capture::clearAll();

    // 2-4. Flush deferred writes before deleting anything. Otherwise old
    // wardrive/stats/capture queue entries can recreate files after a reported
    // successful wipe. If the card is unavailable or the flush fails, fail
    // closed and report a partial wipe rather than making a false durability
    // claim.
    uint16_t fails = 0;
    const bool sdReady = SDStorage::isAvailable();
    const bool deferredFlushed = sdReady && SDStorage::flushDeferred();
    if (!deferredFlushed) {
        fails++;
    } else {
        fails += nukeDirContents("/hamlet/captures");   // stash.bin, pmkid_*.22000, hs_*.pcap
        fails += nukeDirContents("/hamlet/export");     // wpasec.pot, wpasec_uploaded.txt, wpasec_sent.txt
        fails += nukeDirContents("/hamlet/wardrive");   // *.csv, .wigle_uploaded, .wigle_stats.json
        fails += nukeDirContents("/hamlet/stats");      // hunt_YYYYMMDD.log
        fails += nukeDirContents("/recon");             // forensic live/session dumps

        // clearAll() marks the journal dirty. Seal an empty snapshot now so a
        // stale stash cannot resurrect captures after a power cut or reboot.
        if (!Capture::sealJournal()) fails++;
    }

    // 5. clear cracked-network intel + cached views
    Potfile::clear();
    if (potEntries) {
        heap_caps_free(potEntries);
        potEntries = nullptr;
        potCount = 0;
    }
    potScroll = 0;
    potfileTruncated = false;
    wigleCsvCount = 0;
    wigleScroll = 0;
    wigleStats = {0, 0, 0, false};
    WiGLE::freeUploadedListMemory();

    // 6. reset list view
    currentIdx = 0;
    inDetailView = false;
    currentLootView = LOOT_LIST;

    if (!showResult) return;

    Haptic::pulse();

    // Reuse the upload modal for the result banner — OP_NUKE drives the status bar title.
    activeOp = OP_NUKE;
    uploadState = (fails > 0) ? UPLOAD_FAILED : UPLOAD_SUCCESS;
    if (fails > 0) {
        snprintf(uploadMessage, sizeof(uploadMessage), "NUK3D. %u F41LS.", fails);
    } else {
        snprintf(uploadMessage, sizeof(uploadMessage), "4LL D4T4 NUK3D.");
    }
    uploadStartTime = millis();
}

static void drawNukeConfirm() {
    uint16_t fg = Display::getColorFG();
    uint16_t bg = Display::getColorBG();
    uint16_t dim = Display::lerpColor565(fg, bg, 0.5f);

    // shift title below breadcrumb (breadcrumb owns y=16..27)
    canvas->setTextDatum(TC_DATUM);
    canvas->setTextSize(2);
    canvas->drawString("NUK3 4LL", SCREEN_WIDTH / 2, LIST_Y - 2);
    canvas->drawLine(60, LIST_Y + 15, SCREEN_WIDTH - 60, LIST_Y + 15, fg);

    canvas->setTextSize(1);
    canvas->setTextDatum(TL_DATUM);

    int y = LIST_Y + 22;
    canvas->drawString("W1P3 3V3RYTH1NG:", COL_LABEL, y); y += 14;
    canvas->drawString("- PMKID + HANDSHAKE CAPTURES", COL_LABEL + 4, y); y += 12;
    canvas->drawString("- PCAP + HC22000 FILES", COL_LABEL + 4, y); y += 12;
    canvas->drawString("- WPA-SEC POTFILE", COL_LABEL + 4, y); y += 12;
    canvas->drawString("- WARDRIVE CSVs", COL_LABEL + 4, y); y += 12;
    canvas->drawString("- WIGLE UPLOAD STATE", COL_LABEL + 4, y); y += 12;
    canvas->drawString("- HUNT SESSION STATS", COL_LABEL + 4, y); y += 18;

    canvas->setTextColor(fg);
    canvas->drawString("NO UNDO. SETTINGS KEPT.", COL_LABEL, y);
    y += 14;
    canvas->setTextColor(dim);
    canvas->drawString("H0LD [B] T0 C0MM1T.", COL_LABEL, y);
    canvas->setTextColor(fg);

    // bottom bar — require HOLD so short tap can't nuke
    Display::drawBottomBar3To(canvas, "", "[B+] NUK3 1T", "[C] NAH");
}

} // namespace LootMenu
