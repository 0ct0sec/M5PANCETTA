/**
 * Config Portal — the local setup desk.
 *
 * ==[ SETUP MODE ]== A phone-sized form edits NVS-backed settings while the
 * captive DNS and HTTP server own the radio. Credentials stay on the device;
 * mode exit tears the temporary network down.
 */

#include "config_portal.h"
#include "../core/config.h"
#include "../core/power.h"
#include "../hamlet.h"
#include "../ui/display.h"
#include <WiFi.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <esp_wifi.h>
#include <esp_system.h>
#include "../sync/nowflock_transport.h"
#include "../defense/recon.h"
#include "../defense/defense_pipeline.h"
#include "wifi_client.h"
#include "wpasec_client.h"

namespace ConfigPortal {

static const char* AP_SSID = "PANCETTA_CFG";
static const char* AP_IP = "192.168.4.1";
static const uint16_t DNS_PORT = 53;
static const uint16_t HTTP_PORT = 80;

static State currentState = PORTAL_STOPPED;
static DNSServer* dnsServer = nullptr;
static WebServer* webServer = nullptr;
static uint32_t startTime = 0;
static uint32_t autoTimeout = 300000;  // 5 minutes
static bool savedThisSession = false;
static char lastError[40] = "";
// ==[ EPHEMERAL AP PSK ]== regenerated per start(), shown on screen, never persisted.
// 8 chars from a 32-glyph unambiguous base32 alphabet → ~40 bits entropy, resistant
// to a GPU offline attack across the portal's 5-minute lifetime.
static char apPassword[9] = "";

static void generateApPassword() {
    static const char alphabet[] = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ";  // no 0/O/1/I
    const size_t alphLen = sizeof(alphabet) - 1;  // 32
    for (int i = 0; i < 8; i++) {
        apPassword[i] = alphabet[esp_random() % alphLen];
    }
    apPassword[8] = '\0';
}

// Embedded HTML - minimal, phone-friendly
static const char HTML_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>PANCETTA Config</title>
<style>
%THEME%
body{font-family:monospace;background:var(--bg);color:var(--fg);padding:20px;margin:0}
h1{color:var(--fg);font-size:1.5em;margin-bottom:20px}
.box{background:var(--box);padding:15px;border:1px solid var(--fg);margin-bottom:15px}
label{display:block;margin-bottom:5px;color:var(--dim)}
input[type=text],input[type=password]{width:100%;padding:10px;background:var(--bg);border:1px solid var(--fg);color:var(--fg);font-family:monospace;box-sizing:border-box;margin-bottom:10px}
.check{display:flex;gap:8px;align-items:center;margin:4px 0 10px;color:var(--dim)}
input[type=submit]{width:100%;padding:15px;background:var(--fg);color:var(--bg);border:none;font-weight:bold;cursor:pointer;font-family:monospace;font-size:1.1em}
input[type=submit]:hover{background:var(--dim)}
.hint{font-size:0.8em;color:var(--hint);margin-top:5px}
.ok{color:var(--fg);text-align:center;padding:20px}
</style>
</head>
<body>
<h1>// HAMLET CONFIG</h1>
<form method="POST" action="/save">
<div class="box">
<label>WiFi SSID</label>
<input type="text" name="ssid" maxlength="32" placeholder="Your WiFi network" value="%SSID%">
<label>WiFi Password</label>
<input type="password" name="pass" maxlength="64" placeholder="WiFi password">
<div class="hint">Leave password blank to keep existing</div>
<div class="hint">Uplink radio is 2.4 GHz. On phone hotspots, enable compatibility / 2.4 GHz mode.</div>
<label class="check"><input type="checkbox" name="open_net" value="1">Open network / clear stored password</label>
</div>
<div class="box">
<label>WPA-SEC API Key</label>
<input type="text" name="key" maxlength="32" placeholder="Get key at wpa-sec.stanev.org" value="%KEY%">
<div class="hint">Required for upload. Get free key at wpa-sec.stanev.org/?get_key</div>
</div>
<div class="box">
<label>WPA-SEC URL (optional)</label>
<input type="text" name="url" maxlength="64" value="%URL%">
<div class="hint">Default: https://wpa-sec.stanev.org</div>
</div>
<div class="box">
<label>WiGLE API Name</label>
<input type="text" name="wigle_user" maxlength="24" placeholder="WiGLE API name" value="%WIGLE_USER%">
<label>WiGLE API Token</label>
<input type="password" name="wigle_token" maxlength="64" placeholder="WiGLE API token" value="%WIGLE_TOKEN%">
<div class="hint">For wardrive upload. Use WiGLE account API name + token.</div>
</div>
<input type="submit" value="SAVE CONFIG">
</form>
</body>
</html>
)rawliteral";

static const char HTML_SAVED[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>PANCETTA Config</title>
<style>
%THEME%
body{font-family:monospace;background:var(--bg);color:var(--fg);padding:20px;margin:0;text-align:center}
h1{color:var(--fg);font-size:1.5em}
.ok{background:var(--ok);border:2px solid var(--fg);padding:30px;margin:20px 0}
a{color:var(--fg)}
</style>
</head>
<body>
<h1>// CONFIG SAVED</h1>
<div class="ok">
<p>Credentials stored.</p>
<p>Press [B] on device to exit config mode.</p>
</div>
<p><a href="/">Configure again</a></p>
</body>
</html>
)rawliteral";

// ==[ THEME COLOR INJECTION ]==
// RGB565 -> CSS hex. mix = c1*(100-pct)/100 + c2*pct/100
static void rgb565ToHex(uint16_t c, char* out) {
    uint8_t r = ((c >> 11) & 0x1F) * 255 / 31;
    uint8_t g = ((c >> 5) & 0x3F) * 255 / 63;
    uint8_t b = (c & 0x1F) * 255 / 31;
    snprintf(out, 8, "#%02x%02x%02x", r, g, b);
}

static void mixToHex(uint16_t c1, uint16_t c2, int pct2, char* out) {
    int r1 = ((c1 >> 11) & 0x1F) * 255 / 31;
    int g1 = ((c1 >> 5) & 0x3F) * 255 / 63;
    int b1 = (c1 & 0x1F) * 255 / 31;
    int r2 = ((c2 >> 11) & 0x1F) * 255 / 31;
    int g2 = ((c2 >> 5) & 0x3F) * 255 / 63;
    int b2 = (c2 & 0x1F) * 255 / 31;
    int w1 = 100 - pct2;
    snprintf(out, 8, "#%02x%02x%02x",
        (r1 * w1 + r2 * pct2) / 100,
        (g1 * w1 + g2 * pct2) / 100,
        (b1 * w1 + b2 * pct2) / 100);
}

static void applyThemeColors(String& html) {
    uint16_t fg = Display::getColorFG();
    uint16_t bg = Display::getColorBG();
    char fgH[8], bgH[8], dimH[8], hintH[8], boxH[8], okH[8];

    rgb565ToHex(fg, fgH);
    rgb565ToHex(bg, bgH);
    mixToHex(fg, bg, 33, dimH);   // fg faded 33% toward bg
    mixToHex(fg, bg, 60, hintH);  // fg faded 60% toward bg
    mixToHex(bg, fg, 12, boxH);   // bg with 12% fg tint
    mixToHex(bg, fg, 8, okH);     // bg with 8% fg tint

    char vars[160];
    snprintf(vars, sizeof(vars),
        ":root{--fg:%s;--bg:%s;--dim:%s;--hint:%s;--box:%s;--ok:%s}",
        fgH, bgH, dimH, hintH, boxH, okH);

    html.replace("%THEME%", vars);
}

// ==[ XSS ESCAPE ]== sanitize user-controlled strings before HTML insertion
// Written to refuse buffer-overflow attacks: a 64-char '<' URL would expand to
// 256 bytes of '&lt;' — if we don't clamp 'j' against maxLen after each entity
// write, the next (maxLen - j) wraps under size_t and snprintf writes past the
// buffer.
static void htmlEscape(const char* in, char* out, size_t maxLen) {
    if (maxLen == 0) return;
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 1 < maxLen; i++) {
        const char* entity = nullptr;
        switch (in[i]) {
            case '<': entity = "&lt;"; break;
            case '>': entity = "&gt;"; break;
            case '"': entity = "&quot;"; break;
            case '&': entity = "&amp;"; break;
            default:  out[j++] = in[i]; continue;
        }
        size_t entLen = strlen(entity);
        if (j + entLen >= maxLen) break;  // no room for this entity — stop
        memcpy(out + j, entity, entLen);
        j += entLen;
    }
    out[j] = '\0';
}

static void handleRoot() {
    String html = FPSTR(HTML_PAGE);

    // Fill in current values (masked for security)
    const char* ssid = Config::getUploadWifiSsid();
    const char* key = Config::getWpaSecKey();
    const char* url = Config::getWpaSecUrl();
    const char* wigleUser = Config::getWigleUsername();
    const char* wigleToken = Config::getWigleToken();


    // HTML-escape all user-controlled values before template insertion
    char escaped[256];
    htmlEscape(ssid ? ssid : "", escaped, sizeof(escaped));
    html.replace("%SSID%", escaped);

    // Mask API key for display
    if (key && strlen(key) > 4) {
        char masked[33];
        snprintf(masked, sizeof(masked), "****%s", key + strlen(key) - 4);
        htmlEscape(masked, escaped, sizeof(escaped));
        html.replace("%KEY%", escaped);
    } else {
        html.replace("%KEY%", "");
    }

    htmlEscape(url ? url : "https://wpa-sec.stanev.org", escaped, sizeof(escaped));
    html.replace("%URL%", escaped);

    htmlEscape(wigleUser ? wigleUser : "", escaped, sizeof(escaped));
    html.replace("%WIGLE_USER%", escaped);

    if (wigleToken && strlen(wigleToken) > 4) {
        char masked[65];
        snprintf(masked, sizeof(masked), "****%s", wigleToken + strlen(wigleToken) - 4);
        htmlEscape(masked, escaped, sizeof(escaped));
        html.replace("%WIGLE_TOKEN%", escaped);
    } else {
        html.replace("%WIGLE_TOKEN%", "");
    }

    applyThemeColors(html);
    webServer->send(200, "text/html", html);
}

static void handleSave() {
    bool changed = false;

    if (webServer->hasArg("ssid")) {
        String ssid = webServer->arg("ssid");
        if (ssid.length() > 0) {
            Config::setUploadWifiSsid(ssid.c_str());
            changed = true;
        }
    }

    if (webServer->hasArg("pass")) {
        String pass = webServer->arg("pass");
        if (webServer->hasArg("open_net")) {
            Config::setUploadWifiPass("");
            changed = true;
        } else if (pass.length() > 0) {  // Only update if not blank
            Config::setUploadWifiPass(pass.c_str());
            changed = true;
        }
    }

    if (webServer->hasArg("key")) {
        String key = webServer->arg("key");
        // Only update if not a masked placeholder
        if (key.length() > 0 && !key.startsWith("****")) {
            Config::setWpaSecKey(key.c_str());
            changed = true;
        }
    }

    if (webServer->hasArg("url")) {
        String url = webServer->arg("url");
        if (url.length() > 0) {
            Config::setWpaSecUrl(url.c_str());
            changed = true;
        }
    }

    if (webServer->hasArg("wigle_user")) {
        String wigleUser = webServer->arg("wigle_user");
        if (wigleUser.length() > 0) {
            Config::setWigleUsername(wigleUser.c_str());
            changed = true;
        }
    }

    if (webServer->hasArg("wigle_token")) {
        String wigleToken = webServer->arg("wigle_token");
        if (wigleToken.length() > 0 && !wigleToken.startsWith("****")) {
            Config::setWigleToken(wigleToken.c_str());
            changed = true;
        }
    }

    if (changed) {
        Config::save();
        savedThisSession = true;
        // Credentials just changed — wipe the reconnect backoff so the next
        // upload attempt is immediate rather than serving stale cooldown.
        WifiClient::resetBackoff();
        WpaSec::resetBackoff();
    }

    String savedHtml = FPSTR(HTML_SAVED);
    applyThemeColors(savedHtml);
    webServer->send(200, "text/html", savedHtml);
}

static void handleNotFound() {
    // Redirect all requests to root (captive portal behavior)
    webServer->sendHeader("Location", String("http://") + AP_IP + "/", true);
    webServer->send(302, "text/plain", "");
}

void start() {
    if (currentState != PORTAL_STOPPED) {
        return;
    }

    currentState = PORTAL_STARTING;
    savedThisSession = false;
    lastError[0] = '\0';

    // Keep the shared controller initialized while NimBLE is warm.
    WiFi.disconnect(false);
    delay(25);

    // Configure AP — WPA2-PSK gated. Previously open: any nearby device could
    // join and rewrite WPA-SEC/WiGLE credentials (credential exfil MITM).
    generateApPassword();
    IPAddress ip(192, 168, 4, 1);
    IPAddress gateway(192, 168, 4, 1);
    IPAddress subnet(255, 255, 255, 0);
    if (!WiFi.mode(WIFI_AP) ||
        !WiFi.softAPConfig(ip, gateway, subnet) ||
        !WiFi.softAP(AP_SSID, apPassword)) {
        snprintf(lastError, sizeof(lastError), "AP START FAILED");
        apPassword[0] = '\0';
        WiFi.mode(WIFI_STA);
        Power::applyCurrentRadioSettings();
        NowFlock::markEspNowNeedsReinit();
        currentState = PORTAL_STOPPED;
        return;
    }
    Power::applyCurrentRadioSettings();
    delay(100);

    // Start DNS server (captive portal)
    dnsServer = new DNSServer();
    dnsServer->start(DNS_PORT, "*", ip);

    // Start web server
    webServer = new WebServer(HTTP_PORT);
    webServer->on("/", HTTP_GET, handleRoot);
    webServer->on("/save", HTTP_POST, handleSave);
    webServer->onNotFound(handleNotFound);
    webServer->begin();

    // Set WiFi power save mode for coexistence with BLE if initialized
    if (DefensePipeline::snapshot().isBleInitialized()) {
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    }

    startTime = millis();
    currentState = PORTAL_RUNNING;
}

void stop() {
    if (currentState == PORTAL_STOPPED) {
        return;
    }

    currentState = PORTAL_STOPPING;

    // Stop servers first (reverse order of start)
    if (webServer) {
        webServer->stop();
        webServer->close();  // Ensure all connections closed
        delete webServer;
        webServer = nullptr;
    }

    if (dnsServer) {
        dnsServer->stop();
        delete dnsServer;
        dnsServer = nullptr;
    }

    // Leave WiFi down after AP teardown. exitCurrentMode() restores BLE before
    // NOWFLOCK is allowed to bring STA back on the next background tick.
    WiFi.softAPdisconnect(false);
    WiFi.mode(WIFI_STA);
    Power::applyCurrentRadioSettings();
    delay(100);

    currentState = PORTAL_STOPPED;
    apPassword[0] = '\0';  // wipe PSK so getPassword() reflects "stopped" state
    NowFlock::markEspNowNeedsReinit();
}

void update() {
    if (currentState != PORTAL_RUNNING) {
        return;
    }

    // Process DNS and HTTP requests
    if (dnsServer) {
        dnsServer->processNextRequest();
    }
    if (webServer) {
        webServer->handleClient();
    }

    // Auto-timeout
    if (autoTimeout > 0 && (millis() - startTime) > autoTimeout) {
        Hamlet::enterMode(HamletMode::IDLE);
        return;
    }
}

State getState() {
    return currentState;
}

bool isRunning() {
    return currentState == PORTAL_RUNNING;
}

const char* getSSID() {
    return AP_SSID;
}

const char* getIP() {
    return AP_IP;
}

const char* getPassword() {
    return apPassword;
}

const char* getLastError() {
    return lastError;
}

bool credentialsSaved() {
    return savedThisSession;
}

void clearSavedFlag() {
    savedThisSession = false;
}

void setAutoTimeout(uint32_t ms) {
    autoTimeout = ms;
}

uint32_t getRemainingTime() {
    if (currentState != PORTAL_RUNNING || autoTimeout == 0) {
        return 0;
    }
    uint32_t elapsed = millis() - startTime;
    return (elapsed < autoTimeout) ? (autoTimeout - elapsed) : 0;
}

} // namespace ConfigPortal
