/**
 * WiGLE Client — CSV upload to api.wigle.net
 *
 * ==[ WARDROVE YIELD ]== streaming 2KB chunks via direct TLS. no PSRAM body buffer.
 * sorted PSRAM-backed uploaded list. batch mode for SD write efficiency.
 * max CSV: 500KB. recon suspend during TLS.
 */

#include "wigle_client.h"
#include "wifi_client.h"
#include "tls_policy.h"
#include "upload_contracts.h"
#include "../core/config.h"
#include "../util/debug_log.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <SD.h>
#include <mbedtls/base64.h>
#include <esp_heap_caps.h>
#include <ctype.h>
#include <climits>

namespace WiGLE {

static const char* UPLOADED_LIST = "/hamlet/wardrive/.wigle_uploaded";
static const char* TRANSACTION_LIST = "/hamlet/wardrive/.wigle_transactions";
static const char* STATS_CACHE   = "/hamlet/wardrive/.wigle_stats.json";
static const size_t MAX_CSV_SIZE  = 180u * 1024u * 1024u;
static constexpr size_t LEDGER_NAME_BYTES = 64;
static constexpr size_t TRANS_ID_BYTES = 32;
static int lastHttpCode = 0;
static char lastError[96] = "";

static void setLastError(const char* detail, int code = 0) {
    lastHttpCode = code;
    snprintf(lastError, sizeof(lastError), "%s", detail ? detail : "WiGLE failure");
    HAMLET_LOGF("[WIGLE] %s (http=%d)\n", lastError, code);
}

static void setHttpError(const char* prefix, int code) {
    char detail[64];
    snprintf(detail, sizeof(detail), "%s HTTP %d", prefix ? prefix : "WiGLE", code);
    setLastError(detail, code);
}

static void setApiErrorFromJson(const JsonDocument& doc, const char* fallback, int code) {
    const char* message = doc["message"] | "";
    if (!message[0]) message = doc["warning"] | "";
    if (!message[0]) message = fallback;
    setLastError(message, code);
}

// ==[ UPLOADED LIST — PSRAM-backed sorted array ]==
static char (*uploadedEntries)[LEDGER_NAME_BYTES] = nullptr;
static uint16_t uploadedEntryCount = 0;
static bool uploadedListLoaded = false;
static bool uploadedListTruncated = false;
static const uint16_t MAX_UPLOADED_ENTRIES = 4096;

struct TransactionEntry {
    char name[LEDGER_NAME_BYTES];
    char transId[TRANS_ID_BYTES];
    uint32_t size;
    char status;  // U=receipt unknown; W/I/T/S/A/C/G=pending; D/E=terminal
};
static TransactionEntry* transactionEntries = nullptr;
static uint16_t transactionCount = 0;
static bool transactionListLoaded = false;
static const uint16_t MAX_TRANSACTION_ENTRIES = 128;

static int uploadedCmp(const void* a, const void* b) {
    return strcmp((const char*)a, (const char*)b);
}

static const char* basenameOf(const char* path) {
    if (!path) return "";
    const char* slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static bool endsWithIgnoreCase(const char* s, const char* suffix) {
    if (!s || !suffix) return false;
    size_t sl = strlen(s);
    size_t tl = strlen(suffix);
    if (tl > sl) return false;
    s += sl - tl;
    for (size_t i = 0; i < tl; i++) {
        if (tolower((unsigned char)s[i]) != tolower((unsigned char)suffix[i])) {
            return false;
        }
    }
    return true;
}

static bool normalizeUploadedName(const char* filename, char* out, size_t outSize) {
    if (!filename || !out || outSize == 0) return false;
    const char* base = basenameOf(filename);
    if (!base[0]) return false;
    size_t len = strlen(base);
    if (len >= outSize) return false;
    memcpy(out, base, len + 1);
    return true;
}

static bool multipartFilenameOk(const char* filename) {
    const char* base = basenameOf(filename);
    if (!base[0] || strlen(base) >= 64) return false;
    for (const char* p = base; *p; p++) {
        if (*p == '"' || *p == '\r' || *p == '\n' || (uint8_t)*p < 0x20) return false;
    }
    return true;
}

static void buildWardrivePath(const char* name, char* out, size_t outSize) {
    if (!name || !out || outSize == 0) return;
    if (strchr(name, '/')) {
        snprintf(out, outSize, "%s", name);
    } else {
        snprintf(out, outSize, "/hamlet/wardrive/%s", name);
    }
}

static bool isCsvFilename(const char* name) {
    const char* base = basenameOf(name);
    if (!base[0] || base[0] == '.') return false;
    return endsWithIgnoreCase(base, ".csv");
}

static bool inspectWigleCsv(const char* path, uint32_t* sizeOut = nullptr,
                            uint16_t* rowsOut = nullptr, bool* oversizeOut = nullptr) {
    if (sizeOut) *sizeOut = 0;
    if (rowsOut) *rowsOut = 0;
    if (oversizeOut) *oversizeOut = false;

    File f = SD.open(path, FILE_READ);
    if (!f) return false;

    uint32_t fsize = f.size();
    if (sizeOut) *sizeOut = fsize;
    if (fsize == 0 || fsize > MAX_CSV_SIZE) {
        if (oversizeOut && fsize > MAX_CSV_SIZE) *oversizeOut = true;
        f.close();
        return false;
    }

    char hdr[16] = {};
    size_t got = f.readBytes(hdr, sizeof(hdr) - 1);
    hdr[got] = '\0';
    if (strncmp(hdr, "WigleWifi-", 10) != 0) {
        f.close();
        return false;
    }

    f.seek(0);
    char line[192];
    f.readBytesUntil('\n', line, sizeof(line) - 1);
    size_t secondLen = f.readBytesUntil('\n', line, sizeof(line) - 1);
    if (secondLen > 0 && line[secondLen - 1] == '\r') secondLen--;
    line[secondLen] = '\0';
    if (strcmp(line, UploadContracts::WIGLE_V16_COLUMNS) != 0) {
        f.close();
        return false;
    }

    uint32_t rows = 0;
    bool rowHasData = false;
    while (f.available()) {
        int c = f.read();
        if (c == '\n') {
            if (rowHasData) rows++;
            rowHasData = false;
        } else if (c != '\r' && !isspace((unsigned char)c)) {
            rowHasData = true;
        }
    }
    if (rowHasData) rows++;
    f.close();

    if (rowsOut) *rowsOut = (rows > 0xFFFFu) ? 0xFFFFu : (uint16_t)rows;
    return rows > 0;
}

static void loadUploadedList() {
    if (uploadedListLoaded) return;

    if (!uploadedEntries) {
        uploadedEntries = (char(*)[LEDGER_NAME_BYTES])heap_caps_malloc(
            MAX_UPLOADED_ENTRIES * sizeof(uploadedEntries[0]), MALLOC_CAP_SPIRAM);
        if (!uploadedEntries) {
            // Don't set uploadedListLoaded — allow retry on next call when memory frees up
            return;
        }
    }
    uploadedListLoaded = true;  // only cache "loaded" after successful allocation
    uploadedEntryCount = 0;
    uploadedListTruncated = false;

    File f = SD.open(UPLOADED_LIST, FILE_READ);
    if (!f) return;

    char line[128];
    int lineLen = 0;
    while (f.available() && uploadedEntryCount < MAX_UPLOADED_ENTRIES) {
        char c = f.read();
        if (c == '\n') {
            line[lineLen] = '\0';
            char normalized[LEDGER_NAME_BYTES];
            if (lineLen > 0 && normalizeUploadedName(line, normalized, sizeof(normalized))) {
                memcpy(uploadedEntries[uploadedEntryCount], normalized, strlen(normalized) + 1);
                uploadedEntryCount++;
            }
            lineLen = 0;
        } else if (c != '\r' && lineLen < (int)sizeof(line) - 1) {
            line[lineLen++] = c;
        }
    }
    if (lineLen > 0 && uploadedEntryCount < MAX_UPLOADED_ENTRIES) {
        line[lineLen] = '\0';
        char normalized[LEDGER_NAME_BYTES];
        if (normalizeUploadedName(line, normalized, sizeof(normalized))) {
            memcpy(uploadedEntries[uploadedEntryCount], normalized, strlen(normalized) + 1);
            uploadedEntryCount++;
        }
    }
    if (f.available()) uploadedListTruncated = true;
    f.close();

    qsort(uploadedEntries, uploadedEntryCount, sizeof(uploadedEntries[0]), uploadedCmp);

    // Legacy marker files may contain the same basename through full-path and
    // basename entries. Compact them so the fixed-capacity ledger stays useful.
    uint16_t unique = 0;
    for (uint16_t i = 0; i < uploadedEntryCount; i++) {
        if (unique == 0 || strcmp(uploadedEntries[i], uploadedEntries[unique - 1]) != 0) {
            if (unique != i) strcpy(uploadedEntries[unique], uploadedEntries[i]);
            unique++;
        }
    }
    uploadedEntryCount = unique;
}

static bool saveUploadedList() {
    if (!uploadedEntries) return false;
    static const char* tempPath = "/hamlet/wardrive/.wigle_uploaded.new";
    static const char* backupPath = "/hamlet/wardrive/.wigle_uploaded.bak";
    SD.remove(tempPath);
    if (!SD.exists(UPLOADED_LIST) && SD.exists(backupPath) &&
        !SD.rename(backupPath, UPLOADED_LIST)) return false;
    if (SD.exists(UPLOADED_LIST)) SD.remove(backupPath);
    File f = SD.open(tempPath, FILE_WRITE);
    if (!f) return false;
    for (uint16_t i = 0; i < uploadedEntryCount; i++) {
        f.println(uploadedEntries[i]);
    }
    f.close();

    bool hadOld = SD.exists(UPLOADED_LIST);
    if (hadOld && !SD.rename(UPLOADED_LIST, backupPath)) {
        SD.remove(tempPath);
        return false;
    }
    if (!SD.rename(tempPath, UPLOADED_LIST)) {
        if (hadOld) SD.rename(backupPath, UPLOADED_LIST);
        SD.remove(tempPath);
        return false;
    }
    if (hadOld) SD.remove(backupPath);
    return true;
}

static void loadTransactionList();

static int findTransactionByName(const char* name) {
    for (uint16_t i = 0; i < transactionCount; i++) {
        if (strcmp(transactionEntries[i].name, name) == 0) return (int)i;
    }
    return -1;
}

static int findTransactionById(const char* transId) {
    if (!transId || !transId[0]) return -1;
    for (uint16_t i = 0; i < transactionCount; i++) {
        if (strcmp(transactionEntries[i].transId, transId) == 0) return (int)i;
    }
    return -1;
}

static uint16_t countPendingTransactions() {
    uint16_t count = 0;
    for (uint16_t i = 0; i < transactionCount; ++i) {
        if (UploadContracts::wigleStatusPending(transactionEntries[i].status)) count++;
    }
    return count;
}

static uint16_t countRejectedTransactions() {
    uint16_t count = 0;
    for (uint16_t i = 0; i < transactionCount; ++i) {
        if (UploadContracts::wigleStatusRejected(transactionEntries[i].status)) count++;
    }
    return count;
}

static bool isRejected(const char* filename) {
    char normalized[LEDGER_NAME_BYTES];
    if (!normalizeUploadedName(filename, normalized, sizeof(normalized))) return false;
    loadTransactionList();
    int idx = transactionEntries ? findTransactionByName(normalized) : -1;
    return idx >= 0 && UploadContracts::wigleStatusRejected(transactionEntries[idx].status);
}

static void loadTransactionList() {
    if (transactionListLoaded) return;
    transactionEntries = (TransactionEntry*)heap_caps_malloc(
        MAX_TRANSACTION_ENTRIES * sizeof(TransactionEntry), MALLOC_CAP_SPIRAM);
    if (!transactionEntries) return;
    transactionListLoaded = true;
    transactionCount = 0;

    File f = SD.open(TRANSACTION_LIST, FILE_READ);
    if (!f) return;
    char line[160];
    while (f.available() && transactionCount < MAX_TRANSACTION_ENTRIES) {
        size_t len = f.readBytesUntil('\n', line, sizeof(line) - 1);
        if (len > 0 && line[len - 1] == '\r') len--;
        line[len] = '\0';
        if (line[0] == '#' || line[0] == '\0') continue;

        char* tab1 = strchr(line, '\t');
        if (!tab1) continue;
        *tab1++ = '\0';
        char* tab2 = strchr(tab1, '\t');
        if (!tab2) continue;
        *tab2++ = '\0';
        char* tab3 = strchr(tab2, '\t');
        if (!tab3) continue;
        *tab3++ = '\0';

        char normalized[LEDGER_NAME_BYTES];
        if (!normalizeUploadedName(line, normalized, sizeof(normalized)) ||
            strlen(tab1) >= TRANS_ID_BYTES || !tab2[0]) continue;
        TransactionEntry& tx = transactionEntries[transactionCount++];
        snprintf(tx.name, sizeof(tx.name), "%s", normalized);
        snprintf(tx.transId, sizeof(tx.transId), "%s", tab1);
        tx.status = tab2[0];
        tx.size = (uint32_t)strtoul(tab3, nullptr, 10);
    }
    f.close();
}

static bool saveTransactionList() {
    if (!transactionListLoaded || !transactionEntries) return false;
    static const char* tempPath = "/hamlet/wardrive/.wigle_transactions.new";
    static const char* backupPath = "/hamlet/wardrive/.wigle_transactions.bak";
    SD.remove(tempPath);
    if (!SD.exists(TRANSACTION_LIST) && SD.exists(backupPath) &&
        !SD.rename(backupPath, TRANSACTION_LIST)) return false;
    if (SD.exists(TRANSACTION_LIST)) SD.remove(backupPath);
    File f = SD.open(tempPath, FILE_WRITE);
    if (!f) return false;
    f.println("#hamlet-wigle-tx-v1");
    for (uint16_t i = 0; i < transactionCount; i++) {
        const TransactionEntry& tx = transactionEntries[i];
        f.printf("%s\t%s\t%c\t%lu\n", tx.name, tx.transId, tx.status,
                 (unsigned long)tx.size);
    }
    f.close();

    bool hadOld = SD.exists(TRANSACTION_LIST);
    if (hadOld && !SD.rename(TRANSACTION_LIST, backupPath)) {
        SD.remove(tempPath);
        return false;
    }
    if (!SD.rename(tempPath, TRANSACTION_LIST)) {
        if (hadOld) SD.rename(backupPath, TRANSACTION_LIST);
        SD.remove(tempPath);
        return false;
    }
    if (hadOld) SD.remove(backupPath);
    return true;
}

static bool upsertTransaction(const char* filename, const char* transId,
                              uint32_t size, char status) {
    loadTransactionList();
    if (!transactionEntries) return false;
    char normalized[LEDGER_NAME_BYTES];
    if (!normalizeUploadedName(filename, normalized, sizeof(normalized))) return false;
    int idx = findTransactionByName(normalized);
    if (idx < 0) {
        if (transactionCount >= MAX_TRANSACTION_ENTRIES) return false;
        idx = transactionCount++;
        memset(&transactionEntries[idx], 0, sizeof(transactionEntries[idx]));
        snprintf(transactionEntries[idx].name, sizeof(transactionEntries[idx].name),
                 "%s", normalized);
    }
    TransactionEntry& tx = transactionEntries[idx];
    snprintf(tx.transId, sizeof(tx.transId), "%s", transId ? transId : "");
    tx.size = size;
    tx.status = status;
    return true;
}

static void removeTransactionAt(uint16_t idx) {
    if (idx >= transactionCount) return;
    if (idx + 1 < transactionCount) {
        memmove(&transactionEntries[idx], &transactionEntries[idx + 1],
                (transactionCount - idx - 1) * sizeof(TransactionEntry));
    }
    transactionCount--;
}

void freeUploadedListMemory() {
    if (uploadedEntries) {
        heap_caps_free(uploadedEntries);
        uploadedEntries = nullptr;
    }
    uploadedEntryCount = 0;
    uploadedListLoaded = false;
    uploadedListTruncated = false;
    if (transactionEntries) {
        heap_caps_free(transactionEntries);
        transactionEntries = nullptr;
    }
    transactionCount = 0;
    transactionListLoaded = false;
}

FileState getFileState(const char* filename) {
    char normalized[LEDGER_NAME_BYTES];
    if (!normalizeUploadedName(filename, normalized, sizeof(normalized))) {
        return FileState::LOCAL;
    }
    loadTransactionList();
    if (transactionEntries) {
        int tx = findTransactionByName(normalized);
        if (tx >= 0) {
            return UploadContracts::wigleStatusRejected(transactionEntries[tx].status)
                       ? FileState::REJECTED
                       : FileState::PENDING;
        }
    }
    loadUploadedList();
    if (uploadedEntries && uploadedEntryCount > 0 &&
        bsearch(normalized, uploadedEntries, uploadedEntryCount,
                sizeof(uploadedEntries[0]), uploadedCmp) != nullptr) {
        return FileState::CONFIRMED;
    }
    return FileState::LOCAL;
}

bool isUploaded(const char* filename) {
    FileState state = getFileState(filename);
    // Pending receipts are deliberately considered uploaded here so immutable
    // evidence is reconciled before any retry. The UI uses getFileState() when
    // it needs to tell a receipt from a confirmed transaction.
    return state == FileState::PENDING || state == FileState::CONFIRMED;
}

static bool markUploadedDurable(const char* filename) {
    loadUploadedList();
    if (!uploadedEntries || uploadedListTruncated) return false;
    char normalized[LEDGER_NAME_BYTES];
    if (!normalizeUploadedName(filename, normalized, sizeof(normalized))) return false;
    if (uploadedEntryCount > 0 &&
        bsearch(normalized, uploadedEntries, uploadedEntryCount,
                sizeof(uploadedEntries[0]), uploadedCmp) != nullptr) return true;

    if (uploadedEntryCount >= MAX_UPLOADED_ENTRIES) {
        uploadedListTruncated = true;
        return false;
    }

    // add to array
    strcpy(uploadedEntries[uploadedEntryCount], normalized);
    uploadedEntryCount++;

    // re-sort for bsearch
    qsort(uploadedEntries, uploadedEntryCount, sizeof(uploadedEntries[0]), uploadedCmp);

    return saveUploadedList();
}

void markAsUploaded(const char* filename) {
    markUploadedDurable(filename);
}

uint16_t getUploadedCount() {
    loadUploadedList();
    return uploadedEntryCount;
}

// ==[ AUTH HEADER ]== build Basic base64(user:token) on stack
static bool buildAuthHeader(char* out, size_t outSize) {
    const char* user  = Config::getWigleUsername();
    const char* token = Config::getWigleToken();
    if (!user[0] || !token[0]) return false;

    char plain[25 + 1 + 65] = {};
    snprintf(plain, sizeof(plain), "%s:%s", user, token);

    size_t encLen = 0;
    uint8_t encBuf[128] = {};
    int rc = mbedtls_base64_encode(encBuf, sizeof(encBuf), &encLen,
                                   (const uint8_t*)plain, strlen(plain));
    if (rc != 0 || encLen == 0 || encLen + 7 > outSize) return false;

    snprintf(out, outSize, "Basic %.*s", (int)encLen, encBuf);
    return true;
}

bool hasCredentials() {
    return Config::hasWigleCredentials();
}

class MultipartFileStream final : public Stream {
public:
    MultipartFileStream(const uint8_t* prefix, size_t prefixLen, File& file,
                        size_t fileLen, const uint8_t* suffix, size_t suffixLen)
        : prefix_(prefix), prefixLen_(prefixLen), file_(file), fileLen_(fileLen),
          suffix_(suffix), suffixLen_(suffixLen) {}

    int available() override {
        size_t remaining = totalSize() - position_;
        return remaining > (size_t)INT_MAX ? INT_MAX : (int)remaining;
    }
    int read() override {
        uint8_t byte = 0;
        return readBytes(&byte, 1) == 1 ? byte : -1;
    }
    int peek() override {
        if (position_ < prefixLen_) return prefix_[position_];
        if (position_ < prefixLen_ + fileLen_) return file_.peek();
        size_t tailPos = position_ - prefixLen_ - fileLen_;
        return tailPos < suffixLen_ ? suffix_[tailPos] : -1;
    }
    void flush() override {}
    size_t write(uint8_t) override { return 0; }
    size_t readBytes(uint8_t* buffer, size_t length) override {
        size_t copied = 0;
        while (copied < length && position_ < totalSize()) {
            if (position_ < prefixLen_) {
                size_t n = prefixLen_ - position_;
                if (n > length - copied) n = length - copied;
                memcpy(buffer + copied, prefix_ + position_, n);
                position_ += n;
                copied += n;
            } else if (position_ < prefixLen_ + fileLen_) {
                size_t filePos = position_ - prefixLen_;
                size_t n = fileLen_ - filePos;
                if (n > length - copied) n = length - copied;
                size_t got = file_.read(buffer + copied, n);
                if (got == 0) break;
                position_ += got;
                copied += got;
            } else {
                size_t tailPos = position_ - prefixLen_ - fileLen_;
                size_t n = suffixLen_ - tailPos;
                if (n > length - copied) n = length - copied;
                memcpy(buffer + copied, suffix_ + tailPos, n);
                position_ += n;
                copied += n;
            }
        }
        return copied;
    }

private:
    size_t totalSize() const { return prefixLen_ + fileLen_ + suffixLen_; }
    const uint8_t* prefix_;
    size_t prefixLen_;
    File& file_;
    size_t fileLen_;
    const uint8_t* suffix_;
    size_t suffixLen_;
    size_t position_ = 0;
};

struct FileReceipt {
    UploadError error = UploadError::NONE;
    int16_t httpCode = 0;
    bool accepted = false;
    bool ambiguous = false;
    char transId[TRANS_ID_BYTES] = {};
};

static FileReceipt uploadSingleFileChecked(const char* path, const char* authHdr) {
    FileReceipt receipt;
    File f = SD.open(path, FILE_READ);
    if (!f) {
        setLastError("CSV OPEN FAILED");
        receipt.error = UploadError::SD_IO;
        return receipt;
    }
    size_t fsize = f.size();
    if (fsize == 0 || fsize > MAX_CSV_SIZE) {
        f.close();
        setLastError("CSV SIZE INVALID");
        receipt.error = UploadError::SD_IO;
        return receipt;
    }

    const char* fname = basenameOf(path);
    if (!multipartFilenameOk(fname)) {
        f.close();
        setLastError("CSV NAME INVALID");
        receipt.error = UploadError::SD_IO;
        return receipt;
    }
    char boundary[32];
    snprintf(boundary, sizeof(boundary), "WiGLE%lu", millis());
    char partHdr[256];
    int hdrLen = snprintf(partHdr, sizeof(partHdr),
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"%s\"\r\n"
        "Content-Type: text/csv\r\n\r\n", boundary, fname);
    char partTail[64];
    int tailLen = snprintf(partTail, sizeof(partTail), "\r\n--%s--\r\n", boundary);
    if (hdrLen < 0 || tailLen < 0 || hdrLen >= (int)sizeof(partHdr) ||
        tailLen >= (int)sizeof(partTail)) {
        f.close();
        setLastError("MULTIPART ENCODE FAIL");
        receipt.error = UploadError::SD_IO;
        return receipt;
    }

    WiFiClientSecure client;
    if (!TlsPolicy::configureWigle(client)) {
        f.close();
        setLastError("TRUSTED CLOCK REQUIRED");
        receipt.error = UploadError::TRANSPORT;
        return receipt;
    }
    client.setTimeout(30);
    HTTPClient http;
    http.setConnectTimeout(15000);
    http.setTimeout(30000);
    if (!http.begin(client, "https://api.wigle.net/api/v2/file/upload")) {
        f.close();
        setLastError("HTTPS BEGIN FAILED");
        receipt.error = UploadError::TRANSPORT;
        return receipt;
    }
    char contentType[80];
    snprintf(contentType, sizeof(contentType), "multipart/form-data; boundary=%s", boundary);
    http.addHeader("Authorization", authHdr);
    http.addHeader("Content-Type", contentType);
    http.addHeader("Accept", "application/json");

    MultipartFileStream body((const uint8_t*)partHdr, (size_t)hdrLen, f, fsize,
                             (const uint8_t*)partTail, (size_t)tailLen);
    size_t totalLen = (size_t)hdrLen + fsize + (size_t)tailLen;
    int code = http.sendRequest("POST", &body, totalLen);
    receipt.httpCode = (int16_t)code;
    String response;
    if (code > 0) response = http.getString();
    http.end();
    f.close();

    if (code < 0) {
        setHttpError(code == HTTPC_ERROR_READ_TIMEOUT ? "UPLOAD TIMEOUT" : "UPLOAD TRANSPORT", code);
        receipt.error = code == HTTPC_ERROR_READ_TIMEOUT ? UploadError::TIMEOUT
                                                         : UploadError::TRANSPORT;
        receipt.ambiguous = true;
        return receipt;
    }
    if (code == 401 || code == 403) {
        setHttpError("AUTH REJECTED", code);
        receipt.error = UploadError::AUTH;
        return receipt;
    }
    if (code == 429) {
        setHttpError("RATE LIMITED", code);
        receipt.error = UploadError::RATE_LIMIT;
        return receipt;
    }
    if (code >= 500) {
        setHttpError("SERVER ERROR", code);
        receipt.error = UploadError::HTTP_SERVER;
        receipt.ambiguous = true;
        return receipt;
    }
    if (code < 200 || code >= 300) {
        setHttpError("UPLOAD REJECTED", code);
        receipt.error = UploadError::HTTP_CLIENT;
        return receipt;
    }

    DynamicJsonDocument doc(2048);
    DeserializationError jsonError = deserializeJson(doc, response);
    if (jsonError) {
        setLastError("UPLOAD RECEIPT JSON INVALID", code);
        receipt.error = UploadError::BAD_JSON;
        receipt.ambiguous = true;
        return receipt;
    }
    if (!(doc["success"] | false)) {
        setApiErrorFromJson(doc, "UPLOAD API REJECTED", code);
        receipt.error = UploadError::API_REJECTED;
        return receipt;
    }
    const char* transId = doc["results"]["transids"][0]["transId"] | "";
    if (!transId[0] || strlen(transId) >= sizeof(receipt.transId)) {
        setLastError("UPLOAD RECEIPT HAS NO TRANSID", code);
        receipt.error = UploadError::NO_TRANS_ID;
        receipt.ambiguous = true;
        return receipt;
    }
    snprintf(receipt.transId, sizeof(receipt.transId), "%s", transId);
    receipt.accepted = true;
    lastHttpCode = code;
    lastError[0] = '\0';
    return receipt;
}

struct ReconcileResult {
    UploadError error = UploadError::NONE;
    int16_t httpCode = 0;
    uint16_t confirmed = 0;
    uint16_t rejected = 0;
    uint16_t pending = 0;
};

static ReconcileResult reconcileTransactions(const char* authHdr) {
    ReconcileResult out;
    loadTransactionList();
    if (!transactionEntries) {
        setLastError("TRANSACTION MEMORY FAILED");
        out.error = UploadError::NO_MEMORY;
        return out;
    }
    if (transactionCount == 0) return out;

    bool changed = false;
    for (int start = 0; start < 100 && transactionCount > 0; start += 25) {
        WiFiClientSecure client;
        if (!TlsPolicy::configureWigle(client)) {
            setLastError("TRUSTED CLOCK REQUIRED");
            out.error = UploadError::TRANSPORT;
            break;
        }
        client.setTimeout(30);
        HTTPClient http;
        http.setConnectTimeout(15000);
        http.setTimeout(30000);
        char url[128];
        snprintf(url, sizeof(url),
                 "https://api.wigle.net/api/v2/file/transactions?pagestart=%d&pageend=%d",
                 start, 25);
        if (!http.begin(client, url)) {
            setLastError("TRANSACTION HTTPS BEGIN FAILED");
            out.error = UploadError::TRANSPORT;
            break;
        }
        http.addHeader("Authorization", authHdr);
        http.addHeader("Accept", "application/json");
        int code = http.GET();
        out.httpCode = (int16_t)code;
        String response;
        if (code > 0) response = http.getString();
        http.end();

        if (code < 0) {
            setHttpError(code == HTTPC_ERROR_READ_TIMEOUT ? "TRANSACTION TIMEOUT"
                                                          : "TRANSACTION TRANSPORT", code);
            out.error = code == HTTPC_ERROR_READ_TIMEOUT ? UploadError::TIMEOUT
                                                         : UploadError::TRANSPORT;
            break;
        }
        if (code == 401 || code == 403) {
            setHttpError("TRANSACTION AUTH REJECTED", code);
            out.error = UploadError::AUTH;
            break;
        }
        if (code == 429) {
            setHttpError("TRANSACTION RATE LIMITED", code);
            out.error = UploadError::RATE_LIMIT;
            break;
        }
        if (code >= 500) {
            setHttpError("TRANSACTION SERVER ERROR", code);
            out.error = UploadError::HTTP_SERVER;
            break;
        }
        if (code != 200) {
            setHttpError("TRANSACTION REJECTED", code);
            out.error = UploadError::HTTP_CLIENT;
            break;
        }

        StaticJsonDocument<384> filter;
        filter["success"] = true;
        filter["message"] = true;
        filter["warning"] = true;
        filter["results"][0]["transid"] = true;
        filter["results"][0]["status"] = true;
        filter["results"][0]["fileName"] = true;
        filter["results"][0]["fileSize"] = true;
        DynamicJsonDocument doc(12288);
        DeserializationError err = deserializeJson(
            doc, response, DeserializationOption::Filter(filter));
        if (err) {
            setLastError("TRANSACTION JSON INVALID", code);
            out.error = UploadError::BAD_JSON;
            break;
        }
        if (!(doc["success"] | false)) {
            setApiErrorFromJson(doc, "TRANSACTION API REJECTED", code);
            out.error = UploadError::API_REJECTED;
            break;
        }

        JsonArray rows = doc["results"].as<JsonArray>();
        size_t rowCount = rows.size();
        for (JsonObject row : rows) {
            const char* transId = row["transid"] | "";
            const char* fileName = row["fileName"] | "";
            uint32_t fileSize = row["fileSize"] | 0u;
            const char* statusText = row["status"] | "";
            if (!statusText[0]) continue;

            int idx = findTransactionById(transId);
            if (idx < 0 && fileName[0]) {
                char normalized[LEDGER_NAME_BYTES];
                if (normalizeUploadedName(fileName, normalized, sizeof(normalized))) {
                    int byName = findTransactionByName(normalized);
                    if (byName >= 0 && transactionEntries[byName].transId[0] == '\0' &&
                        (transactionEntries[byName].size == 0 ||
                         transactionEntries[byName].size == fileSize)) idx = byName;
                }
            }
            if (idx < 0) continue;

            TransactionEntry& tx = transactionEntries[idx];
            if (!tx.transId[0] && transId[0] && strlen(transId) < sizeof(tx.transId)) {
                snprintf(tx.transId, sizeof(tx.transId), "%s", transId);
                changed = true;
            }
            char status = statusText[0];
            if (UploadContracts::wigleStatusSucceeded(status)) {
                char terminalName[LEDGER_NAME_BYTES];
                snprintf(terminalName, sizeof(terminalName), "%s", tx.name);
                if (!markUploadedDurable(terminalName)) {
                    setLastError("UPLOAD LEDGER WRITE FAILED", code);
                    out.error = UploadError::LEDGER_IO;
                    break;
                }
                out.confirmed++;
                removeTransactionAt((uint16_t)idx);
                changed = true;
            } else if (UploadContracts::wigleStatusRejected(status)) {
                // Keep the terminal rejection in the transaction ledger. It
                // must not be called uploaded, and it must not be blindly
                // resubmitted on every sync attempt.
                if (!UploadContracts::wigleStatusRejected(tx.status)) out.rejected++;
                if (tx.status != status) {
                    tx.status = status;
                    changed = true;
                }
            } else if (tx.status != status) {
                tx.status = status;
                changed = true;
            }
        }
        if (out.error != UploadError::NONE) break;
        if (rowCount < 25) break;
    }

    out.pending = countPendingTransactions();
    if (changed && !saveTransactionList() && out.error == UploadError::NONE) {
        setLastError("TRANSACTION LEDGER WRITE FAILED");
        out.error = UploadError::LEDGER_IO;
    }
    return out;
}

SyncResult uploadAll(ProgressCallback cb) {
    SyncResult result = {};
    result.error = UploadError::NONE;
    lastHttpCode = 0;
    lastError[0] = '\0';

    if (!WifiClient::isConnected()) {
        setLastError("WIFI NOT CONNECTED");
        result.error = UploadError::NOT_CONNECTED;
        return result;
    }
    if (!hasCredentials()) {
        setLastError("API NAME/TOKEN MISSING");
        result.error = UploadError::NO_CREDENTIALS;
        return result;
    }
    char authHdr[160];
    if (!buildAuthHeader(authHdr, sizeof(authHdr))) {
        setLastError("BASIC AUTH ENCODE FAILED");
        result.error = UploadError::NO_CREDENTIALS;
        return result;
    }

    freeUploadedListMemory();
    loadUploadedList();
    loadTransactionList();
    if (!uploadedEntries || !transactionEntries) {
        setLastError("UPLOAD LEDGER MEMORY FAILED");
        result.error = UploadError::NO_MEMORY;
        freeUploadedListMemory();
        return result;
    }
    if (uploadedListTruncated) {
        setLastError("UPLOAD LEDGER FULL/TRUNCATED");
        result.error = UploadError::LEDGER_IO;
        freeUploadedListMemory();
        return result;
    }

    ReconcileResult reconciled = reconcileTransactions(authHdr);
    result.confirmed = reconciled.confirmed;
    result.rejected = reconciled.rejected;
    result.pending = reconciled.pending;
    result.httpCode = reconciled.httpCode;
    if (reconciled.error != UploadError::NONE) {
        result.error = reconciled.error;
        freeUploadedListMemory();
        return result;
    }

    File dir = SD.open("/hamlet/wardrive");
    if (!dir || !dir.isDirectory()) {
        if (dir) dir.close();
        setLastError("WARDRIVE DIRECTORY MISSING");
        result.error = UploadError::SD_IO;
        freeUploadedListMemory();
        return result;
    }

    uint16_t total = 0;
    File countFile = dir.openNextFile();
    while (countFile) {
        if (!countFile.isDirectory()) {
            char basename[LEDGER_NAME_BYTES];
            if (normalizeUploadedName(countFile.name(), basename, sizeof(basename)) &&
                isCsvFilename(basename) &&
                !UploadContracts::isFlockReplaySidecarName(basename)) {
                char fullPath[112];
                buildWardrivePath(basename, fullPath, sizeof(fullPath));
                if (inspectWigleCsv(fullPath) && multipartFilenameOk(basename) &&
                    !isRejected(basename) && !isUploaded(basename)) total++;
            }
        }
        countFile.close();
        countFile = dir.openNextFile();
    }
    dir.rewindDirectory();

    uint16_t current = 0;
    File entry = dir.openNextFile();
    while (entry) {
        if (!entry.isDirectory()) {
            char basename[LEDGER_NAME_BYTES];
            bool nameOk = normalizeUploadedName(entry.name(), basename, sizeof(basename));
            if (nameOk && isCsvFilename(basename) &&
                !UploadContracts::isFlockReplaySidecarName(basename)) {
                char fullPath[112];
                buildWardrivePath(basename, fullPath, sizeof(fullPath));
                uint32_t fsize = 0;
                bool oversize = false;
                bool valid = multipartFilenameOk(basename) &&
                             inspectWigleCsv(fullPath, &fsize, nullptr, &oversize);
                if (!valid) {
                    if (oversize) result.oversize++;
                    else result.empty++;
                } else if (isRejected(basename)) {
                    // Terminal E stays in the transaction ledger so it is
                    // visible and cannot be mistaken for an uploaded file.
                } else if (isUploaded(basename)) {
                    result.skipped++;
                } else {
                    current++;
                    if (cb) cb(current, total);

                    // Never keep File::name() storage across close/TLS. basename
                    // above is an owned copy and remains valid for the receipt.
                    entry.close();

                    // Persist an in-flight intent before sending. If the response
                    // is lost, the next run reconciles filename+size instead of
                    // blindly posting the same immutable file again.
                    if (!upsertTransaction(basename, "", fsize, 'U') ||
                        !saveTransactionList()) {
                        setLastError("TRANSACTION INTENT WRITE FAILED");
                        result.error = UploadError::LEDGER_IO;
                        result.failed++;
                        break;
                    }

                    FileReceipt receipt = uploadSingleFileChecked(fullPath, authHdr);
                    result.httpCode = receipt.httpCode;
                    if (receipt.accepted) {
                        if (!upsertTransaction(basename, receipt.transId, fsize, 'W') ||
                            !saveTransactionList()) {
                            setLastError("TRANSACTION RECEIPT WRITE FAILED", receipt.httpCode);
                            result.error = UploadError::LEDGER_IO;
                            result.failed++;
                            break;
                        }
                        result.submitted++;
                    } else {
                        result.failed++;
                        result.error = receipt.error;
                        if (!receipt.ambiguous) {
                            int idx = findTransactionByName(basename);
                            if (idx >= 0) removeTransactionAt((uint16_t)idx);
                            if (!saveTransactionList()) result.error = UploadError::LEDGER_IO;
                        }
                        break;  // auth/link/server failures are batch-fatal
                    }

                    entry = dir.openNextFile();
                    continue;
                }
            }
        }
        entry.close();
        entry = dir.openNextFile();
    }
    if (entry) entry.close();
    dir.close();

    result.pending = countPendingTransactions();
    result.rejected = countRejectedTransactions();
    freeUploadedListMemory();
    return result;
}

UserStats getCachedStats() {
    UserStats stats = {0, 0, 0, false};
    File f = SD.open(STATS_CACHE, FILE_READ);
    if (!f) return stats;

    char json[512];
    int jsonLen = f.readBytes(json, sizeof(json) - 1);
    json[jsonLen] = '\0';
    f.close();

    auto extractUint = [&](const char* key) -> uint32_t {
        const char* p = strstr(json, key);
        if (!p) return 0;
        p += strlen(key);
        while (*p && (*p == '"' || *p == ':' || *p == ' ')) p++;
        return (uint32_t)atol(p);
    };

    stats.rank      = extractUint("\"rank\"");
    stats.wifiNets  = extractUint("\"wifiNets\"");
    if (stats.wifiNets == 0) stats.wifiNets = extractUint("\"wifi\"");
    if (stats.wifiNets == 0) stats.wifiNets = extractUint("\"discoveredWiFi\"");
    stats.totalNets = extractUint("\"totalNets\"");
    if (stats.totalNets == 0) {
        stats.totalNets = stats.wifiNets + extractUint("\"cell\"") + extractUint("\"bt\"");
    }
    if (stats.totalNets == 0) {
        stats.totalNets = stats.wifiNets + extractUint("\"discoveredCell\"") + extractUint("\"discoveredBt\"");
    }
    stats.valid     = (stats.wifiNets > 0 || stats.rank > 0);
    return stats;
}

bool refreshStats() {
    if (!WifiClient::isConnected()) return false;
    if (!hasCredentials()) return false;
    if (ESP.getFreeHeap() < 65000) return false;

    char authHdr[160];
    if (!buildAuthHeader(authHdr, sizeof(authHdr))) return false;

    WiFiClientSecure client;
    if (!TlsPolicy::configureWigle(client)) return false;
    client.setTimeout(15);

    HTTPClient http;
    http.setTimeout(15000);
    if (!http.begin(client, "https://api.wigle.net/api/v2/stats/user")) return false;
    http.addHeader("Authorization", authHdr);
    http.addHeader("Accept", "application/json");

    int code = http.GET();
    if (code != 200) {
        http.end();
        return false;
    }

    SD.mkdir("/hamlet/wardrive");
    static const char* tempPath = "/hamlet/wardrive/.wigle_stats.json.new";
    static const char* backupPath = "/hamlet/wardrive/.wigle_stats.json.bak";
    SD.remove(tempPath);
    if (!SD.exists(STATS_CACHE) && SD.exists(backupPath) &&
        !SD.rename(backupPath, STATS_CACHE)) return false;
    if (SD.exists(STATS_CACHE)) SD.remove(backupPath);
    File f = SD.open(tempPath, FILE_WRITE);
    if (!f) {
        http.end();
        return false;
    }
    int written = http.writeToStream(&f);
    f.close();
    http.end();

    if (written <= 0) {
        SD.remove(tempPath);
        return false;
    }
    File verify = SD.open(tempPath, FILE_READ);
    if (!verify) {
        SD.remove(tempPath);
        return false;
    }
    char json[512];
    size_t jsonLen = verify.readBytes(json, sizeof(json) - 1);
    verify.close();
    json[jsonLen] = '\0';
    const char* first = json;
    while (*first && isspace((unsigned char)*first)) first++;
    if (*first != '{' || !strchr(first, '}') ||
        (!strstr(first, "\"rank\"") && !strstr(first, "\"statistics\""))) {
        SD.remove(tempPath);
        return false;
    }
    bool hadOld = SD.exists(STATS_CACHE);
    if (hadOld && !SD.rename(STATS_CACHE, backupPath)) {
        SD.remove(tempPath);
        return false;
    }
    if (!SD.rename(tempPath, STATS_CACHE)) {
        if (hadOld) SD.rename(backupPath, STATS_CACHE);
        SD.remove(tempPath);
        return false;
    }
    if (hadOld) SD.remove(backupPath);
    return true;
}

int getLastHttpCode() {
    return lastHttpCode;
}

const char* getLastError() {
    return lastError;
}

} // namespace WiGLE
