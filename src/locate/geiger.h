/**
 * Geiger - audible RSSI evidence
 *
 * Converts a filtered RSSI sample and its age into a click cadence and pitch.
 * RSSI is not distance: callers and UI must retain that evidence limit.
 */

#ifndef GEIGER_H
#define GEIGER_H

#include <stdint.h>

namespace Geiger {

// The active source identifies which case currently owns the shared clicker.
enum Source : uint8_t {
    SOURCE_NONE = 0,
    SOURCE_DEAUTH,   // deauthentication source selected by Spectrum
    SOURCE_CLIENT,   // WiFi client selected by Spectrum
    SOURCE_BLE       // BLE device selected by P1G 34RS
};

// Posture view: flat or stale pose = RAD; valid upright pose = THRU.
enum ViewMode : uint8_t {
    VIEW_RAD = 0,
    VIEW_THRU
};

// Start a new source and reset filter/trend history.
void start(Source src);

// Stop tracking and silence subsequent updates.
void stop();

bool isActive();

Source getSource();

// Feed main-loop RSSI in dBm; returns true only when this call emits a click.
bool update(int8_t rssi);

// Feed RSSI plus sample age in milliseconds. Age zero means newly received;
// older samples are progressively de-emphasized so idle clients cannot restart
// the clicker at stale urgency.
bool update(int8_t rssi, uint32_t ageMs);

// Positive means RSSI strengthened; negative means it weakened.
int8_t getTrend();

int8_t getSmoothed();

// Current scan view derived from cached pose validity and orientation.
ViewMode getViewMode();
const char* getViewLabel();

}  // namespace Geiger

#endif  // GEIGER_H
