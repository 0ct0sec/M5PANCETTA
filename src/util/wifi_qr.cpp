/**
 * WiFi QR — payload + render
 */

#include "wifi_qr.h"
#include <qrcode.h>
#include <string.h>

namespace WifiQR {

static size_t appendEscaped(char* out, size_t outLen, size_t pos, const char* src) {
    if (!src) return pos;
    for (size_t i = 0; src[i] && pos + 2 < outLen; i++) {
        char c = src[i];
        if (c == '\\' || c == ';' || c == ',' || c == '"') {
            out[pos++] = '\\';
        }
        out[pos++] = c;
    }
    if (pos < outLen) out[pos] = '\0';
    return pos;
}

size_t buildPayload(char* out, size_t outLen,
                    const char* ssid, const char* psk,
                    wifi_auth_mode_t auth, bool hidden) {
    if (!out || outLen < 16 || !ssid) return 0;

    const char* type = "WPA";
    if (auth == WIFI_AUTH_OPEN) type = "nopass";
    else if (auth == WIFI_AUTH_WEP) type = "WEP";

    size_t pos = 0;
    pos += snprintf(out, outLen, "WIFI:T:%s;S:", type);
    pos = appendEscaped(out, outLen, pos, ssid);
    if (pos + 4 >= outLen) return 0;
    pos += snprintf(out + pos, outLen - pos, ";P:");
    pos = appendEscaped(out, outLen, pos, psk ? psk : "");
    if (pos + 12 >= outLen) return 0;
    pos += snprintf(out + pos, outLen - pos, ";H:%s;;", hidden ? "true" : "false");
    return pos;
}

static char cachedPayload[180];
static constexpr uint16_t kQrModuleBytes = 220;  // v6 grid (41x41)
static uint8_t cachedModules[kQrModuleBytes];
static QRCode cachedCode;
static int cachedModulesCount = 0;
static uint8_t cachedScale = 0;
static bool cacheValid = false;

static bool ensureCached(const char* payload) {
    if (!payload || !payload[0]) return false;
    if (cacheValid && strcmp(cachedPayload, payload) == 0) {
        return cachedModulesCount > 0;
    }

    uint8_t version = 0;
    for (uint8_t v = 3; v <= 6; v++) {
        if (qrcode_initText(&cachedCode, cachedModules, v, ECC_MEDIUM, payload) == 0) {
            version = v;
            break;
        }
    }
    if (version == 0) {
        cacheValid = false;
        cachedModulesCount = 0;
        return false;
    }

    strncpy(cachedPayload, payload, sizeof(cachedPayload) - 1);
    cachedPayload[sizeof(cachedPayload) - 1] = '\0';
    cachedModulesCount = cachedCode.size;
    cachedScale = (cachedModulesCount <= 25) ? 3 : 2;
    cacheValid = true;
    return true;
}

int edgeFor(const char* payload, uint8_t scale) {
    if (!ensureCached(payload)) return 0;
    if (scale == 0) scale = cachedScale;
    return cachedModulesCount * scale;
}

int draw(M5Canvas& canvas, int x, int y, uint16_t fg, uint16_t bg,
         const char* payload, uint8_t scale) {
    if (!ensureCached(payload)) return 0;
    if (scale == 0) scale = cachedScale;

    int edge = cachedModulesCount * scale;
    canvas.fillRect(x - 2, y - 2, edge + 4, edge + 4, fg);
    canvas.fillRect(x, y, edge, edge, bg);

    for (int row = 0; row < cachedModulesCount; row++) {
        for (int col = 0; col < cachedModulesCount; col++) {
            if (qrcode_getModule(&cachedCode, col, row)) {
                canvas.fillRect(x + col * scale, y + row * scale, scale, scale, fg);
            }
        }
    }
    return edge;
}

}  // namespace WifiQR
