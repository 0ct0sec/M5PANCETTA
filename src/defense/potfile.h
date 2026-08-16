/**
 * Potfile — Known AP database (SPIFFS-backed)
 *
 * ==[ COLD FILE ]== SSID:PSK store for recognizing friendly networks.
 * lazy SPIFFS mount. hashcat import format. max 64 entries.
 * RAM index: 64x33-byte SSID array = ~2.1KB (PSK on disk only).
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

namespace Potfile {

static constexpr int MAX_ENTRIES = 64;
static constexpr int POTFILE_SSID_LEN = 32;

// ==[ API ]==
void init();                              // lazy mount SPIFFS, load index
bool isKnown(const char* ssid);           // check if SSID in potfile
bool getPSK(const char* ssid, char* out, size_t outLen);  // read PSK from SPIFFS (on demand)
int  getCount();                          // number of entries loaded
bool addEntry(const char* ssid, const char* psk);  // add single entry
bool removeEntry(const char* ssid);       // remove by SSID
int  importHashcat(const char* data, int len);     // import SSID:PSK\n block, returns count added
void clear();                             // wipe potfile

// for UI enumeration
const char* getSSID(int index);           // get SSID at index (NULL if out of range)

}  // namespace Potfile
