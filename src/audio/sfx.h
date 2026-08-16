/**
 * SFX - Non-blocking Sound Effects Module
 *
 * ==[ CHEF'S AUDIO ]== central beeps, no delay(), ISR-safe enqueue.
 *   DEAUTH:     low punch @500Hz (kick drum)
 *   EAPOL M1:   single tick 30ms @1200Hz
 *   EAPOL M2:   double tick
 *   EAPOL M3:   triple tick
 *   JACKPOT:    rising arp + Morse "GG"
 *   PMKID:      double-tap @ 1000/1200Hz
 *   FIRST:      fanfare + Morse "W"
 *   MILESTONE:  Morse "GG" easter egg
 */

#ifndef SFX_H
#define SFX_H

#include <stdint.h>

namespace SFX {

// ==[ EVENTS ]== safe from callbacks
enum Event {
    NONE = 0,
    DEAUTH,         // mudball sent - low kick
    EAPOL_M1,       // first handshake frame - single tick
    EAPOL_M2,       // second frame - double tick
    EAPOL_M3,       // third frame - triple tick
    EAPOL_M4,       // complete handshake - jackpot
    PMKID,          // PMKID captured - instant win
    FIRST_CATCH,    // first capture of session - victory lap
    MILESTONE,      // every 10th capture - easter egg
    CLIENT_NEW,     // new client spotted
    SIGNAL_LOST,    // network disappeared
    RING,           // Pork calling (single ring)
    ERROR,          // something went wrong
    PARANOIA_ALERT, // deauth detected (PARANOIA mode) - Morse DEAUTH
    RAIN_TICK,      // rain starting - look at screen warning
    SIGNAL_LOCK,    // strong signal locked (-25dB) - retro terminal
    HAMLET_BOOT,     // device power-on - long dramatic boot
    TRANSMISSION_BURST, // lore fragment incoming - damaged signal static
    BIRD_HIT,         // wave zaps bird - short electric zap
    BIRD_IMPACT,      // bird hits ground - low thud
    OINK_GRUNT,       // pig bumps into tree - low grunt
    SHIP_EXPLODE,     // shuttle shot down - dramatic explosion
    // ==[ PHASE 2: DOPAMINE OVERHAUL ]== 15 new earcons
    LEVEL_UP,         // rare peak event - 7-note fanfare
    GOAL_COMPLETE,    // daily goal done - warm 4-note chime
    CRITICAL_TICK,    // neglect countdown - low 400Hz pulse (accelerating)
    RIB_ESCAPE,       // panic sequence success - dark-to-bright arc
    HUNT_CAMP,        // behavior: stationary - descending settle
    HUNT_PATROL,      // behavior: walking - ascending forward
    HUNT_SPRINT,      // behavior: sprinting - rapid ascending
    HUNT_LURK,        // behavior: target lock - sustained low
    SESSION_ACTIVE,   // session marked active - quick pip
    THUNDER_RUMBLE,   // thunder flash - low band rumble
    CUTE_JUMP,        // pig jump animation - upward chirp
    WALK_MILESTONE,   // step goal milestone - bright ascending
    PLUG_IN,          // USB-C inserted - fast ascending
    SNIFF,            // pig sniff animation - nasal chirp
    SHOCKWAVE_BOOM,   // AKIRA dome expanding - pressure staircase
    DEBRIS_RAIN_START, // post-shockwave fallout begins - metallic scatter
    GESTURE_LONG_PIP,       // long press armed - double pip @1800Hz
    GESTURE_SUPER_LONG_PIP, // super long press armed - double pip @2500Hz
    // ==[ PHASE 3: REENTRY CINEMATIC ]== 4 atmospheric earcons
    REENTRY_WHISPER,        // first audible sign - faint high pip (2200Hz)
    REENTRY_RUMBLE,         // atmospheric friction building - low band (120Hz)
    REENTRY_ROAR,           // peak fireball plasma - mid frequency (200Hz)
    THRUSTER_POP,           // RCS stabilization burst - sharp pop (1400Hz)
    // ==[ IPP DEFENSE ]== 2 awareness earcons
    RECON_ALERT,            // tracker/twin/spam detected - short alert pip
    TRACKER_FOLLOWING,      // persistent tracker warning - urgent double pip
    // ==[ GAMIFICATION V3 ]== achievement + challenge earcons
    ACHIEVEMENT_UNLOCK,     // trophy earned - bright rising 5-note
    CHALLENGE_COMPLETE,     // session challenge done - warm resolve chord
    CHALLENGE_SWEEP,        // all 3 challenges done - triumphant fanfare
    // ==[ ADDICTION LOOP ]== near-miss arousal earcon
    NEAR_MISS,              // probe detected signal, capture failed - 900→400Hz descending chirp
    // ==[ COMPANION ]== the cat's own voice, and the object he works on
    CAT_MEOW,               // Pig calls out - rising "mee" into a falling "ow"
    CAT_PAW_TAP,            // a paw beat that only rocks the object - soft tick
    CAT_KNOCK,              // the beat that sends it over the edge - dry snap
    CAT_CLATTER,            // the object meeting the floor - thud + two rebounds
    GPS_FIX_LOCK,           // Wardrive navigation fix acquired - targeting-style cadence
    GPS_FIX_WARNING         // Wardrive navigation fix lost - repeated warning pair
};

// init audio system
void init();

// queue a sound event (main-loop only — priority path touches speaker HW)
void play(Event event);

// pump audio from main loop; true if playing
bool update();

// is anything playing now?
bool isPlaying();

// stop current playback
void stop();

// volume control (0-10, stored in Config)
uint8_t getVolume();
void setVolume(uint8_t vol);

// direct tone access (e.g., Paranoid Swine)
void tone(uint16_t freq, uint16_t duration);
void click();  // Geiger-style click
// Morse playback (non-blocking, queued)
void morseGG();  // --. --
void morseW();   // .--

// ==[ PITCH ENGINE ]== mood-reactive + combo escalation
void setMoodPitch(float multiplier);   // 0.79-1.12 per mood tier
float getMoodPitch();
void setComboPitch(float multiplier);  // 1.0-1.19 capture combo
float getComboPitch();
void resetComboPitch();

}  // namespace SFX

#endif  // SFX_H
