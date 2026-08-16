/**
 * Potfile — SPIFFS-backed known AP database
 *
 * ==[ THE ROLODEX ]== stores SSID:PSK pairs from cracked networks.
 * hashcat potfile format. lazy SPIFFS mount on first access.
 * RAM index holds SSIDs only — PSK read from file on demand.
 */

#include "potfile.h"
#include "../util/debug_log.h"
#include <SPIFFS.h>
#include <esp_heap_caps.h>
#include <string.h>

namespace Potfile {

static constexpr const char* POTFILE_PATH = "/potfile.txt";

// ==[ PSRAM INDEX ]== SSIDs only, PSK stays on disk. 64×33 = 2.1KB in PSRAM.
static char (*ssidIndex)[POTFILE_SSID_LEN + 1] = nullptr;
static int entryCount = 0;
static bool mounted = false;
static bool loaded = false;
static bool allocated = false;

static bool usedPSRAM = false;  // tracks which allocator was used for ssidIndex

static bool ensureAllocated() {
    if (allocated) return (ssidIndex != nullptr);
    allocated = true;
    size_t sz = MAX_ENTRIES * (POTFILE_SSID_LEN + 1);
    ssidIndex = (char(*)[POTFILE_SSID_LEN + 1])heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
    if (!ssidIndex) {
        ssidIndex = (char(*)[POTFILE_SSID_LEN + 1])malloc(sz);  // DRAM fallback
        usedPSRAM = false;
        HAMLET_LOGF("[POTFILE] DRAM fallback (%d bytes)\n", (int)sz);
    } else {
        usedPSRAM = true;
        HAMLET_LOGF("[POTFILE] PSRAM index (%d bytes)\n", (int)sz);
    }
    if (ssidIndex) memset(ssidIndex, 0, sz);
    return (ssidIndex != nullptr);
}

// ==[ INTERNAL ]==
static bool ensureMount() {
    if (mounted) return true;
    if (!SPIFFS.begin(true)) {  // format on fail
        HAMLET_LOGLN("[POTFILE] SPIFFS mount failed");
        return false;
    }
    mounted = true;
    return true;
}

static void loadIndex() {
    if (loaded) return;
    if (!ensureAllocated()) return;
    if (!ensureMount()) return;

    entryCount = 0;
    memset(ssidIndex, 0, MAX_ENTRIES * (POTFILE_SSID_LEN + 1));

    File f = SPIFFS.open(POTFILE_PATH, "r");
    if (!f) {
        loaded = true;
        HAMLET_LOGLN("[POTFILE] no potfile yet. clean slate.");
        return;
    }

    static char line[128];  // static to reduce stack pressure on ESP32
    while (f.available() && entryCount < MAX_ENTRIES) {
        int len = 0;
        while (f.available() && len < (int)sizeof(line) - 1) {
            char c = f.read();
            if (c == '\n' || c == '\r') break;
            line[len++] = c;
        }
        line[len] = '\0';
        if (len == 0) continue;

        // format: SSID:PSK
        char* sep = strchr(line, ':');
        if (!sep) continue;
        *sep = '\0';

        int ssidLen = sep - line;
        if (ssidLen > POTFILE_SSID_LEN || ssidLen == 0) continue;

        strncpy(ssidIndex[entryCount], line, POTFILE_SSID_LEN);
        ssidIndex[entryCount][POTFILE_SSID_LEN] = '\0';
        entryCount++;
    }
    f.close();
    loaded = true;
    HAMLET_LOGF("[POTFILE] loaded %d entries\n", entryCount);
}

static bool saveFile() {
    if (!ensureMount()) return false;

    File f = SPIFFS.open(POTFILE_PATH, "r");
    // read existing file to preserve PSKs (heap-allocated to avoid 6KB stack frame)
    struct PotEntry { char ssid[33]; char psk[65]; };
    PotEntry* entries = new PotEntry[MAX_ENTRIES];
    if (!entries) return false;
    int fileCount = 0;

    if (f) {
        static char line[128];  // static to reduce stack pressure on ESP32
        while (f.available() && fileCount < MAX_ENTRIES) {
            int len = 0;
            while (f.available() && len < (int)sizeof(line) - 1) {
                char c = f.read();
                if (c == '\n' || c == '\r') break;
                line[len++] = c;
            }
            line[len] = '\0';
            if (len == 0) continue;

            char* sep = strchr(line, ':');
            if (!sep) continue;
            *sep = '\0';

            strncpy(entries[fileCount].ssid, line, 32);
            entries[fileCount].ssid[32] = '\0';
            strncpy(entries[fileCount].psk, sep + 1, 64);
            entries[fileCount].psk[64] = '\0';
            fileCount++;
        }
        f.close();
    }

    // rewrite only entries that are still in RAM index
    f = SPIFFS.open(POTFILE_PATH, "w");
    if (!f) { delete[] entries; return false; }

    for (int i = 0; i < entryCount; i++) {
        // find PSK for this SSID
        const char* psk = "";
        for (int j = 0; j < fileCount; j++) {
            if (strcmp(entries[j].ssid, ssidIndex[i]) == 0) {
                psk = entries[j].psk;
                break;
            }
        }
        f.printf("%s:%s\n", ssidIndex[i], psk);
    }
    f.close();
    delete[] entries;
    return true;
}

// ==[ PUBLIC API ]==
void init() {
    loadIndex();
}

bool isKnown(const char* ssid) {
    if (!loaded) loadIndex();
    if (!ssid || !ssid[0]) return false;

    for (int i = 0; i < entryCount; i++) {
        if (strcmp(ssidIndex[i], ssid) == 0) return true;
    }
    return false;
}

bool getPSK(const char* ssid, char* out, size_t outLen) {
    if (!out || outLen == 0) return false;
    out[0] = '\0';
    if (!loaded) loadIndex();
    if (!ssid || !ssid[0] || !ensureMount()) return false;

    File f = SPIFFS.open(POTFILE_PATH, "r");
    if (!f) return false;

    static char line[128];
    bool found = false;
    while (f.available()) {
        int len = 0;
        while (f.available() && len < (int)sizeof(line) - 1) {
            char c = f.read();
            if (c == '\n' || c == '\r') break;
            line[len++] = c;
        }
        line[len] = '\0';
        if (len == 0) continue;

        char* sep = strchr(line, ':');
        if (!sep || sep == line) continue;
        *sep = '\0';
        if (strcmp(line, ssid) != 0) continue;

        const char* psk = sep + 1;
        strncpy(out, psk, outLen - 1);
        out[outLen - 1] = '\0';
        found = true;
        break;
    }
    f.close();
    return found;
}

int getCount() {
    if (!loaded) loadIndex();
    return entryCount;
}

bool addEntry(const char* ssid, const char* psk) {
    if (!loaded) loadIndex();
    if (!ssidIndex) return false;
    if (!ssid || !ssid[0] || entryCount >= MAX_ENTRIES) return false;
    if (strlen(ssid) > POTFILE_SSID_LEN) return false;
    if (isKnown(ssid)) return false;  // no dupes

    // commit disk first. no phantom cracks after reboot.
    if (!ensureMount()) return false;
    File f = SPIFFS.open(POTFILE_PATH, "a");
    if (!f) return false;
    size_t written = f.printf("%s:%s\n", ssid, psk ? psk : "");
    f.close();
    if (written == 0) return false;

    strncpy(ssidIndex[entryCount], ssid, POTFILE_SSID_LEN);
    ssidIndex[entryCount][POTFILE_SSID_LEN] = '\0';
    entryCount++;

    HAMLET_LOGF("[POTFILE] added: %s\n", ssid);
    return true;
}

bool removeEntry(const char* ssid) {
    if (!loaded) loadIndex();
    if (!ssid) return false;

    for (int i = 0; i < entryCount; i++) {
        if (strcmp(ssidIndex[i], ssid) == 0) {
            // shift remaining
            for (int j = i; j < entryCount - 1; j++) {
                strncpy(ssidIndex[j], ssidIndex[j + 1], POTFILE_SSID_LEN);
                ssidIndex[j][POTFILE_SSID_LEN] = '\0';
            }
            entryCount--;
            saveFile();
            return true;
        }
    }
    return false;
}

int importHashcat(const char* data, int len) {
    if (!loaded) loadIndex();
    if (!data || len <= 0) return 0;

    int added = 0;
    const char* p = data;
    const char* end = data + len;

    while (p < end && entryCount < MAX_ENTRIES) {
        // find end of line
        const char* eol = p;
        while (eol < end && *eol != '\n' && *eol != '\r') eol++;

        int lineLen = eol - p;
        if (lineLen > 0 && lineLen < 128) {
            static char line[128];  // static to reduce stack pressure on ESP32
            memcpy(line, p, lineLen);
            line[lineLen] = '\0';

            char* sep = strchr(line, ':');
            if (sep && sep > line) {
                *sep = '\0';
                const char* ssid = line;
                const char* psk = sep + 1;
                if (strlen(ssid) <= POTFILE_SSID_LEN && addEntry(ssid, psk)) {
                    added++;
                }
            }
        }

        // skip line ending
        p = eol;
        while (p < end && (*p == '\n' || *p == '\r')) p++;
    }

    if (added > 0) {
        HAMLET_LOGF("[POTFILE] imported %d entries (total: %d)\n", added, entryCount);
    }
    return added;
}

void clear() {
    entryCount = 0;
    if (ssidIndex) memset(ssidIndex, 0, MAX_ENTRIES * (POTFILE_SSID_LEN + 1));
    if (ensureMount()) {
        SPIFFS.remove(POTFILE_PATH);
    }
    HAMLET_LOGLN("[POTFILE] cleared");
}

const char* getSSID(int index) {
    if (!loaded) loadIndex();
    if (index < 0 || index >= entryCount) return nullptr;
    return ssidIndex[index];
}

}  // namespace Potfile
