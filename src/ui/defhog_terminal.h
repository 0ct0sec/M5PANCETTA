/**
 * DefhogTerminal — cyberpunk defense terminal overlay
 *
 * ==[ DEFHOG4 DEFENSE ]== floating terminal window.
 * materializes when pig takes left-side post (laptop, sofa, antenna).
 * scrolling recon feed with sequential line reveal. linux terminal aesthetic.
 * cross-namespace correlated dumps from Hunt/Recon/Capture/Config/Spectrum/Potfile.
 * 12 dump types, weighted random selection, no immediate repeats.
 */
#pragma once

#include <M5Unified.h>

namespace DefhogTerminal {

// ==[ STATE ]==
enum class State : uint8_t {
    HIDDEN,          // not visible
    MATERIALIZING,   // appear animation (2000ms: scatter -> converge -> logo -> boot)
    ACTIVE,          // live terminal rendering
    DISSOLVING,      // disappear animation (600ms)
};

// ==[ LINE COLOR MODE ]==
enum class LineColor : uint8_t {
    FG,    // neon accent (TERM_FG), primary, accent-hue-shifted
    HYPE,  // legacy, renders same as FG
    DIM,   // theme fg (TERM_DIM), secondary, usual theme color
    ALERT, // inverted (black on neon), blinks 400ms
    BAR,   // solid accent bar (filled rect, no text)
};

// ==[ DATA DUMP TYPE ]== correlated cross-namespace dumps
enum class DumpType : uint8_t {
    SITREP,           // Hunt+Recon+Capture: attack surface, deauth correlation, threat score
    YIELD,            // Hunt+Config: D-UCB per-channel yield with lifetime career context
    STASH,            // Hunt+Capture+Config: capture intel, hit rate, buffer
    CLIENTS,          // Hunt+Potfile: probe harvest with vulnerable client cross-ref
    RF_POSTURE,       // Hunt+Spectrum: RF environment vs hunt channel correlation
    BLE_TRACKERS,     // Recon+Hunt: BLE tracker table, population, probe correlation
    XBAND,            // XBand: cross-domain WiFi+BLE fusion intelligence
    FORENSIC_TIMELINE, // Recon forensic log: chronological event timeline + evidence chain
    HOGWASH,           // WifiChaff: fake handshake injection stats
    HOSTILE_CLIENTS,   // Recon: passive client fingerprinting + canary/KARMA confirmation
    THREAT_HEATMAP,    // Recon: temporal threat intensity sparklines
    ATTACK_TIMELINE,   // Recon: reconstructed chronological attack narrative
    COUNT
};

// ==[ API ]==
void init();
void reset();                               // clear buffer + state
void update(uint32_t now, uint8_t station, uint8_t room);
void draw(M5Canvas& canvas, uint16_t fg, uint16_t bg);

// push terminal lines (safe to call anytime, buffered)
void pushLine(const char* fmt, ...);        // neon accent (primary)
void pushLineHype(const char* fmt, ...);    // legacy — same as FG
void pushLineDim(const char* fmt, ...);     // theme fg (secondary)
void pushLineAlert(const char* fmt, ...);   // inverted neon + blink
void pushLineBar();                         // solid accent bar (no text)

// state control
void show(uint8_t station);       // -> MATERIALIZING
void hide();                      // → DISSOLVING
void hideImmediate();             // → HIDDEN (no animation)

// queries
bool isVisible();                 // any state except HIDDEN
State getState();
uint8_t getThreatLevel();         // current threat level 0-3 (GREEN/YELLOW/ORANGE/RED)
uint8_t computeThreatLevel();     // live composite score, no overlay lifecycle lag

}  // namespace DefhogTerminal
