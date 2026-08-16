/**
 * NoirNarrator — IPP hardboiled cyberpunk noir phrase library
 *
 * ==[ THE SCRIBE ]== pig detective narrates the wire.
 * station-aware, context-driven, 120+ phrases.
 * also provides settings RF explanations.
 */

#pragma once

#include <stdint.h>
#include "recon.h"

// forward-declare Station from menu_pig.cpp's internal enum
// we use uint8_t to avoid coupling
namespace NoirNarrator {

// ==[ PHRASE CATEGORIES ]==
enum class PhraseCategory : uint8_t {
    STATION_IDLE,       // ambient narration at stations
    TRACKER_NEW,        // new tracker detected
    TRACKER_FOLLOWING,  // tracker following us
    BLE_SPAM,           // flipper/spam detected
    EVIL_TWIN,          // evil twin AP
    KARMA_HONEYPOT,     // KARMA attack
    KNOWN_AP,           // potfile match
    OPEN_AP,            // open networks
    QUIET_WIRE,         // nothing happening
    SCAN_ACTIVE,        // during scan
    SETTINGS_HELP,      // RF explanations for settings
};

// station IDs (must match menu_pig.cpp Station enum values)
static constexpr uint8_t STATION_LAPTOP  = 0;
static constexpr uint8_t STATION_SOFA    = 1;
static constexpr uint8_t STATION_WINDOW  = 2;
static constexpr uint8_t STATION_COOKING = 3;
static constexpr uint8_t STATION_BED     = 4;
static constexpr uint8_t STATION_ANTENNA = 5;
static constexpr uint8_t STATION_LEDGE   = 6;
static constexpr uint8_t STATION_TERMINAL = 7;
static constexpr uint8_t STATION_BOOTH   = 8;

// ==[ API ]==
// Get a random station-aware idle phrase
const char* getStationPhrase(uint8_t station);

// Get context-aware phrase using live Recon/Capture/Pedometer data
// Falls back to getStationPhrase() when no live data available
const char* getContextPhrase(uint8_t station);

// Get event-driven phrase
const char* getTrackerPhrase(Recon::ThreatType type);
const char* getFollowingPhrase(const char* detail);
const char* getBleSpamPhrase();
const char* getEvilTwinPhrase(const char* ssid);
const char* getKarmaPhrase(const char* ssid);
const char* getKnownAPPhrase(const char* ssid);
const char* getOpenAPPhrase(uint8_t count);
const char* getQuietPhrase();
const char* getScanActivePhrase();
const char* getCoordinatedAttackPhrase(const char* detail);
const char* getToolIdentifiedPhrase(const char* toolName);
const char* getHostileClientPhrase(const char* label);

// Settings RF explanations (index = SettingItem enum value)
const char* getSettingExplanation(uint8_t settingIndex);

// Format a phrase with dynamic data into static buffer
const char* format(const char* fmt, ...);

}  // namespace NoirNarrator
