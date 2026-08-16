/**
 * WiFi QR — ZXing payload builder + canvas renderer
 *
 * ==[ JOIN ME ]== standard WIFI: QR for phone import. ricmoo/QRCode backend.
 */
#pragma once

#include <M5GFX.h>
#include <esp_wifi_types.h>

namespace WifiQR {

// Build WIFI:T:...;S:...;P:...;H:...;; payload. returns bytes written (0 on fail).
size_t buildPayload(char* out, size_t outLen,
                    const char* ssid, const char* psk,
                    wifi_auth_mode_t auth, bool hidden);

// Draw QR at (x,y). scale=0 auto-picks pixel size. returns drawn edge length (0 on fail).
int draw(M5Canvas& canvas, int x, int y, uint16_t fg, uint16_t bg,
         const char* payload, uint8_t scale = 0);

// Measure cached QR edge without drawing.
int edgeFor(const char* payload, uint8_t scale = 0);

}  // namespace WifiQR
