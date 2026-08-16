/**
 * WPA-SEC Client — authenticated capture export.
 *
 * ==[ CLOUD UPLINK ]== Streams the reviewed export body over TLS with the
 * configured cookie, reports progress, and treats a parsed server response as
 * evidence of acceptance. Transport success alone never closes the case.
 */

#include "wpasec_client.h"
#include "wifi_client.h"
#include "capture_export.h"
#include "tls_policy.h"
#include "upload_contracts.h"
#include "../core/config.h"
#include "../core/capture.h"
#include "../util/debug_log.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <SD.h>
#include <climits>
#include <ctype.h>

namespace WpaSec {

static ProgressCallback progressCb = nullptr;
static int lastHttpCode = 0;
static char lastError[64] = "";
// ==[ BACKOFF ]== server-driven upload suppression. 429/503 sets a deadline;
// uploads short-circuit until millis() passes it. 401/403 sets a long lockout
// so we stop burning data on a revoked key until user rotates credentials.
static uint32_t backoffUntilMs = 0;
static char validatedKey[33] = "";
static Result backoffResult = UPLOAD_RATE_LIMITED;
static constexpr uint32_t BACKOFF_RATE_LIMIT_MS = 60000;   // 1min on 429/503
static constexpr uint32_t BACKOFF_AUTH_FAIL_MS  = 600000;  // 10min on 401/403

// Stitch multipart framing around capture bytes without cloning the payload.
class MultipartStream final : public Stream {
public:
    MultipartStream(const uint8_t* prefix, size_t prefixLen,
                    const uint8_t* payload, size_t payloadLen,
                    const uint8_t* suffix, size_t suffixLen)
        : prefix_(prefix), prefixLen_(prefixLen), payload_(payload),
          payloadLen_(payloadLen), suffix_(suffix), suffixLen_(suffixLen) {}

    int available() override {
        size_t remaining = totalSize() - position_;
        return remaining > (size_t)INT_MAX ? INT_MAX : (int)remaining;
    }
    int read() override {
        if (position_ >= totalSize()) return -1;
        uint8_t value;
        if (position_ < prefixLen_) value = prefix_[position_];
        else if (position_ < prefixLen_ + payloadLen_) value = payload_[position_ - prefixLen_];
        else value = suffix_[position_ - prefixLen_ - payloadLen_];
        position_++;
        return value;
    }
    int peek() override {
        if (position_ >= totalSize()) return -1;
        if (position_ < prefixLen_) return prefix_[position_];
        if (position_ < prefixLen_ + payloadLen_) return payload_[position_ - prefixLen_];
        return suffix_[position_ - prefixLen_ - payloadLen_];
    }
    void flush() override {}
    size_t write(uint8_t) override { return 0; }

private:
    size_t totalSize() const { return prefixLen_ + payloadLen_ + suffixLen_; }
    const uint8_t* prefix_;
    size_t prefixLen_;
    const uint8_t* payload_;
    size_t payloadLen_;
    const uint8_t* suffix_;
    size_t suffixLen_;
    size_t position_ = 0;
};

void setProgressCallback(ProgressCallback cb) {
    progressCb = cb;
}

int getLastHttpCode() {
    return lastHttpCode;
}

const char* getLastError() {
    return lastError;
}

void resetBackoff() {
    backoffUntilMs = 0;
    backoffResult = UPLOAD_RATE_LIMITED;
    validatedKey[0] = '\0';
    lastHttpCode = 0;
    lastError[0] = '\0';
}

static bool isOfficialEndpoint(const char* url) {
    static const char host[] = "https://wpa-sec.stanev.org";
    if (!url) return false;
    size_t hostLen = strlen(host);
    if (strncmp(url, host, hostLen) != 0) return false;
    char next = url[hostLen];
    return next == '\0' || next == '/' || next == '?' || next == '#';
}

// dwpa's upload endpoint accepts an unknown-but-well-formed key and silently
// ingests the capture without associating it to the user. Exercise the site's
// own key-login path first; a registered key is acknowledged by Set-Cookie.
static Result ensureRegisteredKey() {
    const char* key = Config::getWpaSecKey();
    const char* url = Config::getWpaSecUrl();
    if (!Config::hasWpaSecKey()) return UPLOAD_NO_KEY;
    if (!isOfficialEndpoint(url)) return UPLOAD_OK;
    if (strcmp(validatedKey, key) == 0) return UPLOAD_OK;

    WiFiClientSecure client;
    if (!TlsPolicy::configureWpaSec(client, url)) {
        strcpy(lastError, "Trusted clock required");
        return UPLOAD_HTTP_ERROR;
    }
    client.setTimeout(15);

    HTTPClient http;
    http.setConnectTimeout(10000);
    http.setTimeout(15000);
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    const char* responseHeaders[] = {"Set-Cookie"};
    http.collectHeaders(responseHeaders, 1);
    if (!http.begin(client, url)) {
        strcpy(lastError, "Key check begin failed");
        return UPLOAD_HTTP_ERROR;
    }
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");
    char payload[40];
    snprintf(payload, sizeof(payload), "key=%s", key);
    int code = http.POST((uint8_t*)payload, strlen(payload));
    String cookie = http.header("Set-Cookie");
    http.end();

    if ((code == 301 || code == 302 || code == 303 || code == 307 || code == 308) &&
        UploadContracts::wpaSecCookieConfirmsKey(cookie.c_str(), key)) {
        snprintf(validatedKey, sizeof(validatedKey), "%s", key);
        return UPLOAD_OK;
    }
    lastHttpCode = code;
    if (code < 0) {
        snprintf(lastError, sizeof(lastError), "Key check transport %d", code);
        return (code == HTTPC_ERROR_READ_TIMEOUT) ? UPLOAD_TIMEOUT : UPLOAD_HTTP_ERROR;
    }
    backoffUntilMs = millis() + BACKOFF_AUTH_FAIL_MS;
    backoffResult = UPLOAD_AUTH_FAIL;
    snprintf(lastError, sizeof(lastError), "Key not registered");
    return UPLOAD_AUTH_FAIL;
}

Result uploadPCAP(const uint8_t* data, uint32_t len) {
    if (!WifiClient::isConnected()) {
        strcpy(lastError, "WiFi not connected");
        return UPLOAD_NO_WIFI;
    }

    if (!Config::hasWpaSecKey()) {
        strcpy(lastError, "No API key configured");
        return UPLOAD_NO_KEY;
    }

    // Respect server-driven backoff: short-circuit if we were throttled or
    // auth-failed recently. Callers bump the deadline forward based on the
    // HTTP class so a bad key doesn't burn data at full upload cadence.
    uint32_t nowMs = millis();
    if (backoffUntilMs != 0 && (int32_t)(backoffUntilMs - nowMs) > 0) {
        return backoffResult;
    }

    Result keyCheck = ensureRegisteredKey();
    if (keyCheck != UPLOAD_OK) return keyCheck;

    if (!data || len == 0) {
        strcpy(lastError, "No data to upload");
        return UPLOAD_NO_DATA;
    }

    // Check heap before SSL (needs ~50KB for TLS buffers)
    uint32_t freeHeap = ESP.getFreeHeap();

    if (freeHeap < 60000) {
        strcpy(lastError, "Low memory");
        return UPLOAD_HTTP_ERROR;
    }

    // Yield to let WiFi stack settle before TLS
    yield();
    delay(100);

    const char* url = Config::getWpaSecUrl();
    WiFiClientSecure client;
    if (!TlsPolicy::configureWpaSec(client, url)) {
        strcpy(lastError, "Trusted clock required");
        return UPLOAD_HTTP_ERROR;
    }
    client.setTimeout(45);  // server runs capture extraction before replying

    HTTPClient http;
    http.setConnectTimeout(15000);
    http.setTimeout(45000);

    if (!http.begin(client, url)) {
        strcpy(lastError, "HTTP begin failed");
        return UPLOAD_HTTP_ERROR;
    }

    // Set cookie with API key
    char cookie[64];
    snprintf(cookie, sizeof(cookie), "key=%s", Config::getWpaSecKey());
    http.addHeader("Cookie", cookie);

    // Multipart form boundary — stack buffers, no String fragmentation
    char boundary[32];
    snprintf(boundary, sizeof(boundary), "----WpaSec%lu", millis());

    char contentType[80];
    snprintf(contentType, sizeof(contentType), "multipart/form-data; boundary=%s", boundary);
    http.addHeader("Content-Type", contentType);

    // Build multipart body parts on stack
    char bodyStart[160];
    int bsLen = snprintf(bodyStart, sizeof(bodyStart),
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"capture.pcap\"\r\n"
        "Content-Type: application/octet-stream\r\n\r\n", boundary);

    char bodyEnd[48];
    int beLen = snprintf(bodyEnd, sizeof(bodyEnd), "\r\n--%s--\r\n", boundary);

    // Guard against snprintf failure — negative return would corrupt totalLen
    if (bsLen < 0 || beLen < 0 ||
        bsLen >= (int)sizeof(bodyStart) || beLen >= (int)sizeof(bodyEnd)) {
        snprintf(lastError, sizeof(lastError), "snprintf encoding error");
        http.end();
        return UPLOAD_HTTP_ERROR;
    }

    uint32_t totalLen = (uint32_t)bsLen + len + (uint32_t)beLen;

    // Send with streaming
    char lenBuf[16];
    snprintf(lenBuf, sizeof(lenBuf), "%lu", totalLen);
    http.addHeader("Content-Length", lenBuf);

    MultipartStream body((const uint8_t*)bodyStart, (size_t)bsLen,
                         data, len,
                         (const uint8_t*)bodyEnd, (size_t)beLen);
    lastHttpCode = http.sendRequest("POST", &body, totalLen);
    String response;
    if (lastHttpCode > 0) response = http.getString();
    http.end();
    response.trim();

    // dwpa reports semantic failures in a 200 body, so HTTP success is not
    // enough. Only a non-empty non-error body flips the local dedup flag.
    bool semanticError = UploadContracts::wpaSecResponseIsSemanticError(response.c_str());
    if (lastHttpCode == 200 && response.length() > 0 && !semanticError) {
        lastError[0] = '\0';
        HAMLET_LOGF("[WPA-SEC] capture accepted; response-bytes=%u\n",
                    (unsigned)response.length());
        return UPLOAD_OK;
    } else if (lastHttpCode == 200) {
        String summary = response.substring(0, 63);
        snprintf(lastError, sizeof(lastError), "%s",
                 summary.length() ? summary.c_str() : "Empty WPA-SEC response");
        return UPLOAD_HTTP_ERROR;
    } else if (lastHttpCode == 401 || lastHttpCode == 403) {
        backoffUntilMs = millis() + BACKOFF_AUTH_FAIL_MS;
        backoffResult = UPLOAD_AUTH_FAIL;
        snprintf(lastError, sizeof(lastError), "HTTP %d (auth)", lastHttpCode);
        return UPLOAD_AUTH_FAIL;
    } else if (lastHttpCode == 429 || lastHttpCode == 503) {
        backoffUntilMs = millis() + BACKOFF_RATE_LIMIT_MS;
        backoffResult = UPLOAD_RATE_LIMITED;
        snprintf(lastError, sizeof(lastError), "HTTP %d (throttle)", lastHttpCode);
        return UPLOAD_RATE_LIMITED;
    } else if (lastHttpCode == HTTPC_ERROR_READ_TIMEOUT) {
        snprintf(lastError, sizeof(lastError), "HTTP timeout");
        return UPLOAD_TIMEOUT;
    } else {
        snprintf(lastError, sizeof(lastError), "HTTP %d", lastHttpCode);
        return UPLOAD_HTTP_ERROR;
    }
}

Result uploadAll(uint16_t* uploadedCount) {
    if (uploadedCount) *uploadedCount = 0;

    if (!WifiClient::isConnected()) {
        strcpy(lastError, "WiFi not connected");
        return UPLOAD_NO_WIFI;
    }

    if (!Config::hasWpaSecKey()) {
        strcpy(lastError, "No API key configured");
        return UPLOAD_NO_KEY;
    }

    uint16_t hsCount = Capture::getUnsyncedHandshakeCount();
    uint16_t total = hsCount;

    if (total == 0) {
        if (Capture::getUnsyncedPMKIDCount() > 0) {
            strcpy(lastError, "WPA-SEC requires PCAP");
        } else {
            strcpy(lastError, "No captures to upload");
        }
        return UPLOAD_NO_DATA;
    }

    uint16_t uploaded = 0;
    uint16_t current = 0;
    bool hadError = false;
    Result fatalCode = UPLOAD_OK;  // sticky if an upload returns AUTH_FAIL or RATE_LIMITED

    // WPA-SEC accepts original PCAP captures. PMKID-only records are HC22000
    // text here, so they remain available for local export instead of being
    // mislabeled as capture.pcap and falsely marked synced.
    uint8_t* pcapBuf = (uint8_t*)heap_caps_malloc(8192, MALLOC_CAP_SPIRAM);
    if (!pcapBuf) pcapBuf = (uint8_t*)malloc(8192);
    if (pcapBuf) {
        uint16_t totalHs = Capture::getHandshakeCount();
        for (uint16_t i = 0; i < totalHs && !hadError; i++) {
            CapturedHandshake hs;
            if (Capture::getHandshake(i, &hs) && !hs.synced) {
                current++;
                if (progressCb) progressCb(current, total);

                uint32_t pcapLen = CaptureExport::handshakeToPCAP(&hs, pcapBuf, 8192);
                if (pcapLen > 0) {
                    Result r = uploadPCAP(pcapBuf, pcapLen);
                    if (r == UPLOAD_OK) {
                        Capture::markHandshakeSynced(i);
                        uploaded++;
                    } else {
                        hadError = true;
                        if (r == UPLOAD_AUTH_FAIL || r == UPLOAD_RATE_LIMITED) fatalCode = r;
                    }
                } else {
                    hadError = true;
                    strcpy(lastError, "PCAP export failed");
                }
            }
        }
        heap_caps_free(pcapBuf);
    } else {
        hadError = true;
        strcpy(lastError, "PCAP buffer unavailable");
    }

    if (uploadedCount) *uploadedCount = uploaded;

    // Fatal codes (auth/throttle) take priority so callers can surface the
    // right remediation (rotate key vs. wait out the server cooldown).
    if (fatalCode != UPLOAD_OK) {
        return fatalCode;
    }
    if (uploaded == 0) {
        return UPLOAD_HTTP_ERROR;
    } else if (hadError && uploaded < total) {
        snprintf(lastError, sizeof(lastError), "Partial: %d/%d", uploaded, total);
        return UPLOAD_PARTIAL;
    } else {
        lastError[0] = '\0';
        return UPLOAD_OK;
    }
}

static bool isValidPotfileLine(const char* line, size_t len) {
    if (!line || len < 28 || line[12] != ':' || line[25] != ':') return false;
    for (size_t i = 0; i < 25; i++) {
        if (i == 12) continue;
        if (!isxdigit((unsigned char)line[i])) return false;
    }
    return memchr(line + 26, ':', len - 26) != nullptr;
}

static DownloadResult downloadPotfileAtomic(const char* savePath, uint32_t* linesOut) {
    char downloadPath[128];
    char filteredPath[128];
    char backupPath[128];
    int dn = snprintf(downloadPath, sizeof(downloadPath), "%s.download", savePath);
    int fn = snprintf(filteredPath, sizeof(filteredPath), "%s.new", savePath);
    int bn = snprintf(backupPath, sizeof(backupPath), "%s.bak", savePath);
    if (dn < 0 || fn < 0 || bn < 0 ||
        dn >= (int)sizeof(downloadPath) || fn >= (int)sizeof(filteredPath) ||
        bn >= (int)sizeof(backupPath)) {
        strcpy(lastError, "Save path too long");
        return DL_HTTP_ERROR;
    }

    SD.mkdir("/hamlet/export");
    SD.remove(downloadPath);
    SD.remove(filteredPath);
    if (!SD.exists(savePath) && SD.exists(backupPath) && !SD.rename(backupPath, savePath)) {
        strcpy(lastError, "Potfile recovery failed");
        return DL_HTTP_ERROR;
    }
    if (SD.exists(savePath)) SD.remove(backupPath);

    WiFiClientSecure client;
    const char* baseUrl = Config::getWpaSecUrl();
    if (!TlsPolicy::configureWpaSec(client, baseUrl)) {
        strcpy(lastError, "Trusted clock required");
        return DL_HTTP_ERROR;
    }
    client.setTimeout(30);
    HTTPClient http;
    http.setConnectTimeout(10000);
    http.setTimeout(30000);
    String downloadUrl(baseUrl);
    if (!downloadUrl.endsWith("/")) downloadUrl += "/";
    downloadUrl += "?api&dl=1";
    if (!http.begin(client, downloadUrl)) {
        strcpy(lastError, "HTTP begin failed");
        return DL_HTTP_ERROR;
    }
    char cookie[64];
    snprintf(cookie, sizeof(cookie), "key=%s", Config::getWpaSecKey());
    http.addHeader("Cookie", cookie);

    lastHttpCode = http.GET();
    if (lastHttpCode != 200) {
        snprintf(lastError, sizeof(lastError), "HTTP %d", lastHttpCode);
        http.end();
        return DL_HTTP_ERROR;
    }

    File raw = SD.open(downloadPath, FILE_WRITE);
    if (!raw) {
        strcpy(lastError, "SD open failed");
        http.end();
        return DL_HTTP_ERROR;
    }
    int written = http.writeToStream(&raw);
    raw.close();
    http.end();
    if (written < 0) {
        SD.remove(downloadPath);
        strcpy(lastError, "Download interrupted");
        return DL_HTTP_ERROR;
    }

    File src = SD.open(downloadPath, FILE_READ);
    File dst = SD.open(filteredPath, FILE_WRITE);
    if (!src || !dst) {
        if (src) src.close();
        if (dst) dst.close();
        SD.remove(downloadPath);
        SD.remove(filteredPath);
        strcpy(lastError, "SD staging failed");
        return DL_HTTP_ERROR;
    }

    uint32_t lines = 0;
    char lineBuf[160];
    while (src.available()) {
        size_t len = src.readBytesUntil('\n', lineBuf, sizeof(lineBuf) - 1);
        bool truncated = len == sizeof(lineBuf) - 1 && src.peek() != '\n' && src.peek() >= 0;
        if (truncated) {
            while (src.available() && src.read() != '\n') {}
            continue;
        }
        lineBuf[len] = '\0';
        if (len > 0 && lineBuf[len - 1] == '\r') lineBuf[--len] = '\0';

        if (isValidPotfileLine(lineBuf, len)) {
            if (dst.write((uint8_t*)lineBuf, len) != len || dst.write('\n') != 1) {
                src.close();
                dst.close();
                SD.remove(downloadPath);
                SD.remove(filteredPath);
                strcpy(lastError, "SD write failed");
                return DL_HTTP_ERROR;
            }
            lines++;
        }
    }
    src.close();
    dst.close();
    SD.remove(downloadPath);

    if (lines == 0) {
        SD.remove(filteredPath);
        strcpy(lastError, "Empty potfile");
        return DL_EMPTY;
    }

    bool hadOld = SD.exists(savePath);
    if (hadOld && !SD.rename(savePath, backupPath)) {
        SD.remove(filteredPath);
        strcpy(lastError, "Potfile backup failed");
        return DL_HTTP_ERROR;
    }
    if (!SD.rename(filteredPath, savePath)) {
        if (hadOld) SD.rename(backupPath, savePath);
        SD.remove(filteredPath);
        strcpy(lastError, "Potfile commit failed");
        return DL_HTTP_ERROR;
    }
    if (hadOld) SD.remove(backupPath);

    if (linesOut) *linesOut = lines;
    lastError[0] = '\0';
    return DL_OK;
}

DownloadResult downloadPotfile(const char* savePath, uint32_t* linesOut) {
    if (linesOut) *linesOut = 0;
    if (!savePath || savePath[0] != '/') {
        strcpy(lastError, "Invalid save path");
        return DL_HTTP_ERROR;
    }
    if (!WifiClient::isConnected()) {
        strcpy(lastError, "WiFi not connected");
        return DL_NO_WIFI;
    }

    if (!Config::hasWpaSecKey()) {
        strcpy(lastError, "No API key configured");
        return DL_NO_KEY;
    }

    Result keyCheck = ensureRegisteredKey();
    if (keyCheck != UPLOAD_OK) {
        return keyCheck == UPLOAD_AUTH_FAIL || keyCheck == UPLOAD_NO_KEY
                   ? DL_NO_KEY : DL_HTTP_ERROR;
    }

    if (ESP.getFreeHeap() < 60000) {
        strcpy(lastError, "Low memory");
        return DL_HTTP_ERROR;
    }

    yield();
    delay(50);
    return downloadPotfileAtomic(savePath, linesOut);
}

} // namespace WpaSec
