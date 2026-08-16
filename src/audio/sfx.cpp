/**
 * SFX - Non-blocking Sound Effects Implementation
 *
 * ==[ CHEF'S AUDIO ENGINE ]== (freq,duration,pause) steps; update() ticks without blocking.
 */

#include "sfx.h"
#include "bath_mic.h"
#include "../core/config.h"
#include <M5Unified.h>

namespace SFX {

// SFX plays exclusively on speaker channel 0; noir jazz owns channel 1.
static constexpr int SFX_CH = 0;

// ==[ SOUND DEFINITIONS ]== arrays of {freq, duration, pause}; freq=0 = silence, duration=0 = END
struct Note {
    uint16_t freq;      // Hz (0 = silence)
    uint16_t duration;  // ms
    uint16_t pause;     // ms after this note
};

// DEAUTH: low punch (kick drum energy)
static const Note SND_DEAUTH[] = {
    {500, 80, 0},
    {0, 0, 0}  // END
};

// EAPOL M1: single tick
static const Note SND_EAPOL_M1[] = {
    {1200, 30, 0},
    {0, 0, 0}
};

// EAPOL M2: double tick
static const Note SND_EAPOL_M2[] = {
    {1200, 30, 40},
    {1300, 30, 0},
    {0, 0, 0}
};

// EAPOL M3: triple tick (anticipation building)
static const Note SND_EAPOL_M3[] = {
    {1200, 30, 40},
    {1300, 30, 40},
    {1400, 30, 0},
    {0, 0, 0}
};

// JACKPOT (M4 complete): rising arpeggio + will queue Morse GG after
static const Note SND_JACKPOT[] = {
    {800, 80, 30},
    {1000, 80, 30},
    {1200, 80, 30},
    {1500, 120, 100},  // longer final note, pause before Morse
    {0, 0, 0}
};

// PMKID: quick double-tap (instant gratification)
static const Note SND_PMKID[] = {
    {1000, 60, 40},
    {1200, 80, 0},
    {0, 0, 0}
};

// FIRST CATCH: celebratory fanfare + will queue Morse W after
static const Note SND_FIRST[] = {
    {600, 60, 30},
    {800, 60, 30},
    {1000, 80, 30},
    {1200, 100, 30},
    {1500, 150, 100},
    {0, 0, 0}
};

// CLIENT NEW: Geiger-style click (short pulse)
static const Note SND_CLIENT[] = {
    {1000, 10, 0},  // 10ms click
    {0, 0, 0}
};

// SIGNAL LOST: descending sad tones
static const Note SND_SIGNAL_LOST[] = {
    {800, 100, 50},
    {500, 150, 0},
    {0, 0, 0}
};

// RING: single ring pip
static const Note SND_RING[] = {
    {1000, 40, 0},
    {0, 0, 0}
};

// ERROR: harsh buzz
static const Note SND_ERROR[] = {
    {300, 200, 0},
    {0, 0, 0}
};

// RAIN TICK: continuous patter (~2.5 sec of rain drops)
// varied frequencies + timing = natural rain rhythm
static const Note SND_RAIN_TICK[] = {
    {400, 15, 60},   // drop
    {350, 12, 90},   // drop
    {420, 18, 40},   // drop
    {380, 10, 110},  // drop
    {360, 15, 70},   // drop
    {410, 12, 50},   // drop
    {340, 18, 100},  // drop
    {390, 10, 80},   // drop
    {370, 15, 60},   // drop
    {430, 12, 90},   // drop
    {355, 18, 40},   // drop
    {400, 10, 120},  // drop
    {380, 15, 50},   // drop
    {420, 12, 80},   // drop
    {345, 18, 70},   // drop
    {395, 10, 100},  // drop
    {365, 15, 55},   // drop
    {415, 12, 85},   // drop
    {350, 18, 45},   // drop
    {405, 10, 95},   // drop
    {375, 15, 65},   // drop
    {425, 12, 75},   // drop
    {360, 18, 110},  // drop
    {385, 10, 0},    // final drop
    {0, 0, 0}
};

// SIGNAL LOCK: retro sci-fi terminal (1970s analog computer aesthetic)
// ascending lock-on sequence, cold CRT vibes
static const Note SND_SIGNAL_LOCK[] = {
    {200, 40, 20},   // low acquisition pulse
    {400, 40, 20},   // rising
    {600, 40, 20},   // climbing
    {800, 60, 40},   // lock tone hold
    {1000, 80, 30},  // confirmation high
    {800, 100, 0},   // settle (target acquired)
    {0, 0, 0}
};

// GPS FIX LOCK: avionics-flavored acquisition cadence. This confirms a fresh
// navigation position only; it is not target bearing or RF localization.
static const Note SND_GPS_FIX_LOCK[] = {
    {720, 45, 35},
    {920, 45, 35},
    {1180, 55, 45},
    {1480, 90, 25},
    {1480, 90, 0},
    {0, 0, 0}
};

// GPS FIX WARNING: separated equal-pitch pair, repeated by the telemetry state
// machine at a restrained cadence while a previously held fix remains absent.
static const Note SND_GPS_FIX_WARNING[] = {
    {430, 140, 90},
    {430, 140, 0},
    {0, 0, 0}
};

// HAMLET BOOT: dramatic mainframe cold start (~3.5 sec) for device power-on
static const Note SND_HAMLET_BOOT[] = {
    // Power sequence (slow wake)
    {80, 340, 120},   // deep transformer hum
    {100, 300, 90},   // capacitors charging
    {80, 240, 120},   // power stabilizing
    {120, 190, 80},   // tape thud
    // Core boot (systems waking)
    {120, 12, 70},   // bios init
    {150, 10, 60},   // memory check
    {180, 10, 60},    // cpu waking
    {200, 10, 50},    // bus enumeration
    // Subsystems online (rising chirps)
    {250, 10, 40},    // storage spin
    {300, 8, 35},    // io ready
    {350, 8, 35},    // network interface
    {400, 8, 30},    // comms module
    {450, 8, 30},    // sensors online
    // System ready (confirmation tones)
    {500, 10, 40},    // kernel loaded
    {600, 10, 50},    // services started
    {700, 12, 60},    // system ready chirp
    {800, 12, 80},    // tracking enabled
    {600, 220, 0},    // settle tone (target acquired)
    {0, 0, 0}
};

// TRANSMISSION BURST: damaged signal static (~400ms) for lore fragments
// crackle + burst = incoming transmission from deep space
static const Note SND_TRANSMISSION_BURST[] = {
    {150, 25, 15},    // static crackle
    {80, 20, 10},     // low interference
    {200, 30, 20},    // signal fighting through
    {100, 15, 10},    // dropout
    {300, 40, 25},    // lock attempt
    {250, 35, 15},    // stabilizing
    {400, 50, 0},     // transmission acquired
    {0, 0, 0}
};

// BIRD HIT uses a runtime-generated micro "poof" with slight per-hit jitter.

// BIRD IMPACT: low thud (bird hits ground)
static const Note SND_BIRD_IMPACT[] = {
    {200, 60, 20},    // meat thump
    {120, 80, 0},     // ground thud
    {0, 0, 0}
};

// OINK GRUNT: low pig grunt (tree bump)
static const Note SND_OINK_GRUNT[] = {
    {180, 50, 10},    // chest rumble
    {140, 70, 0},     // grunt tail
    {0, 0, 0}
};

// SHIP EXPLODE: dramatic descending explosion with debris
static const Note SND_SHIP_EXPLODE[] = {
    {100, 80, 10},    // deep boom
    {80, 120, 20},    // hull breach
    {150, 40, 10},    // debris scatter
    {60, 150, 30},    // fireball
    {200, 30, 15},    // secondary explosion
    {100, 60, 20},    // impact
    {300, 20, 10},    // sizzle
    {80, 100, 0},     // final thud
    {0, 0, 0}
};

// ==[ PHASE 2: NEW SOUND DEFINITIONS ]== 15 earcons

// LEVEL UP: rare peak event — wide 7-note fanfare (Dixon 2014: duration=value)
static const Note SND_LEVEL_UP[] = {
    {600, 80, 30},
    {800, 80, 30},
    {1000, 80, 30},
    {1200, 100, 40},
    {1500, 120, 40},
    {1800, 150, 60},
    {1400, 150, 0},   // resolve down for warmth
    {0, 0, 0}
};

// GOAL COMPLETE: warm 4-note chime — distinct from capture chain (Bhatara 2025)
static const Note SND_GOAL_COMPLETE[] = {
    {500, 100, 40},
    {700, 100, 40},
    {900, 120, 60},
    {800, 140, 0},    // warm settle
    {0, 0, 0}
};

// CRITICAL TICK: low 400Hz pulse — 800Hz below EAPOL_M1, no semantic confusion
static const Note SND_CRITICAL_TICK[] = {
    {400, 50, 0},
    {0, 0, 0}
};

// RIB ESCAPE: dark-to-bright emotional arc (Kahneman 1993: peak-end)
static const Note SND_RIB_ESCAPE[] = {
    {300, 60, 20},
    {500, 60, 20},
    {800, 70, 25},
    {1100, 80, 30},
    {1300, 100, 0},
    {1100, 80, 0},    // resolve
    {0, 0, 0}
};

// HUNT CAMP: descending = settling (Bhatara 2025)
static const Note SND_HUNT_CAMP[] = {
    {800, 60, 20},
    {600, 60, 0},
    {0, 0, 0}
};

// HUNT PATROL: ascending = forward motion
static const Note SND_HUNT_PATROL[] = {
    {600, 60, 20},
    {800, 60, 0},
    {0, 0, 0}
};

// HUNT SPRINT: rapid ascending = urgency
static const Note SND_HUNT_SPRINT[] = {
    {800, 30, 10},
    {1000, 30, 10},
    {1200, 40, 0},
    {0, 0, 0}
};

// HUNT LURK: sustained low = focused tension
static const Note SND_HUNT_LURK[] = {
    {500, 120, 0},
    {0, 0, 0}
};

// SESSION ACTIVE: quick confirmation pip (Schultz 1997: conditioned cue)
static const Note SND_SESSION_ACTIVE[] = {
    {800, 25, 10},
    {1100, 50, 0},
    {0, 0, 0}
};

// THUNDER RUMBLE: low band = power/dread
static const Note SND_THUNDER_RUMBLE[] = {
    {60, 80, 15},
    {100, 60, 20},
    {80, 70, 20},
    {200, 50, 30},
    {60, 80, 0},
    {0, 0, 0}
};

// CUTE JUMP: ultra-brief upward chirp (Tamaglitchi 2018)
static const Note SND_CUTE_JUMP[] = {
    {1400, 18, 5},
    {1800, 20, 0},
    {0, 0, 0}
};

// WALK MILESTONE: bright ascending (Amabile 2011: progress principle)
static const Note SND_WALK_MILESTONE[] = {
    {700, 50, 20},
    {900, 60, 20},
    {1100, 70, 0},
    {0, 0, 0}
};

// PLUG IN: fast ascending = energy incoming
static const Note SND_PLUG_IN[] = {
    {600, 30, 10},
    {900, 30, 10},
    {1200, 40, 0},
    {0, 0, 0}
};

// CAT MEOW: the companion's voice. A real meow is one glide, not two tones —
// the pitch climbs through the open "mee" and falls away on the "ow", so the
// steps are close enough to slur together on a small speaker. Total length is
// held under the clip's 460 ms open-mouth hold so the sound never outlives the
// visible gape.
static const Note SND_CAT_MEOW[] = {
    {620, 55, 0},
    {760, 65, 0},
    {880, 85, 0},
    {700, 75, 0},
    {560, 110, 0},
    {0, 0, 0}
};

// CAT PAW TAP: a beat that only rocks the object. It has to be audible under
// a four-stroke rake without becoming a rhythm, so it is one short tick well
// above the knock — a claw meeting tin, not tin meeting anything.
static const Note SND_CAT_PAW_TAP[] = {
    {1250, 14, 0},
    {0, 0, 0}
};

// CAT KNOCK: the beat that sends the object over the edge. Contact only —
// the landing has its own sound, because the object is still airborne for up
// to 352ms after this fires and a clatter here would arrive before the impact
// it is describing.
static const Note SND_CAT_KNOCK[] = {
    {1500, 16, 5},
    {1100, 12, 0},
    {0, 0, 0}
};

// CAT CLATTER: the object meeting the floor, played on the contact frame. The
// three onsets are placed on the bounce schedule scripts/sim_cat_trinket.py
// reports — first contact, then +176..240ms, then a further +48..80ms
// depending on the drop height, so 0 / 214 / 270ms sits inside every case.
static const Note SND_CAT_CLATTER[] = {
    {520, 24, 190},
    {380, 16, 40},
    {300, 14, 0},
    {0, 0, 0}
};

// SNIFF: quick nasal chirp — pairs with sniff animation
static const Note SND_SNIFF[] = {
    {500, 15, 5},
    {700, 20, 0},
    {0, 0, 0}
};

// SHOCKWAVE BOOM: rising pressure staircase — AKIRA dome expansion
static const Note SND_SHOCKWAVE_BOOM[] = {
    {80, 200, 30},     // deep underground rumble
    {100, 160, 25},    // pressure rising
    {130, 120, 20},    // wave accelerating
    {170, 100, 15},    // faster
    {220, 80, 10},     // rushing
    {300, 150, 0},     // air blast arrival
    {0, 0, 0}
};

// DEBRIS RAIN START: irregular metallic scatter — fallout begins
static const Note SND_DEBRIS_RAIN_START[] = {
    {250, 20, 40},     // first shard
    {180, 25, 60},     // heavier piece
    {320, 15, 35},     // metallic ping
    {140, 30, 50},     // thud
    {280, 18, 45},     // scatter
    {160, 35, 0},      // settle
    {0, 0, 0}
};

// GESTURE LONG PIP: double pip for long press feedback
static const Note SND_GESTURE_LONG_PIP[] = {
    {1800, 30, 40},
    {1800, 30, 0},
    {0, 0, 0}
};

// GESTURE SUPER LONG PIP: double pip for super long press feedback
static const Note SND_GESTURE_SUPER_LONG_PIP[] = {
    {2500, 100, 60},
    {2500, 100, 0},
    {0, 0, 0}
};

// ==[ PHASE 3: REENTRY CINEMATIC ]== atmospheric earcons

// REENTRY WHISPER: barely-there high pip — first audio cue after 65s silence
// 2200Hz glint + slight drop. 75ms total. Distant, suggests "something's up there"
// (Salimpoor 2011: anticipatory silence before reward increases perceived value)
static const Note SND_REENTRY_WHISPER[] = {
    {2200, 15, 40},   // faint high glint
    {1800, 20, 0},    // slight drop
    {0, 0, 0}
};

// REENTRY RUMBLE: atmospheric friction building — low sustained
// 100-150Hz band, 650ms total. Below THUNDER_RUMBLE (60-200Hz), distinct rhythm.
// (Bhatara 2025: low frequency = power/mass; Grimshaw 2008: ambient baseline shift)
static const Note SND_REENTRY_RUMBLE[] = {
    {120, 200, 30},   // deep sub-bass
    {150, 150, 20},   // rising slightly
    {100, 250, 0},    // sustained low
    {0, 0, 0}
};

// REENTRY ROAR: plasma fireball peak — intense mid-frequency
// 180-300Hz crackling fire, 400ms total. Between THUNDER and SHIP_EXPLODE bands.
// (Dixon 2014: duration scales with significance; ACM 2024: audiovisual coherence)
static const Note SND_REENTRY_ROAR[] = {
    {200, 80, 15},    // base roar
    {300, 60, 10},    // crackling
    {250, 100, 15},   // sustained fire
    {180, 120, 0},    // rumble fade
    {0, 0, 0}
};

// THRUSTER POP: RCS attitude correction burst — ultra-brief
// 1400Hz, 12ms total. Mimics compressed gas release. Sharp and clinical.
// (Brewster 1993: rhythm > pitch for identification — single pop = unique pattern)
static const Note SND_THRUSTER_POP[] = {
    {1400, 12, 0},    // sharp pop
    {0, 0, 0}
};

// ==[ IPP DEFENSE EARCONS ]==

// RECON ALERT: short awareness pip — mid-high, non-threatening
// 1600Hz 25ms + 1800Hz 20ms — quick double chirp, distinct from paranoia
static const Note SND_RECON_ALERT[] = {
    {1600, 25, 30},   // first pip
    {1800, 20, 0},    // second pip (slightly higher)
    {0, 0, 0}
};

// TRACKER FOLLOWING: urgent double pip — lower, more insistent
// 800Hz 40ms + 1000Hz 40ms — slower, weightier, signals persistence
static const Note SND_TRACKER_FOLLOWING[] = {
    {800,  40, 60},   // low pip
    {1000, 40, 60},   // rising pip
    {800,  30, 0},    // resolves low
    {0, 0, 0}
};

// ==[ GAMIFICATION V3 ]== achievement + challenge earcons

// ACHIEVEMENT UNLOCK: bright rising 5-note — trophy earned
static const Note SND_ACHIEVEMENT_UNLOCK[] = {
    {800,  60, 30},
    {1000, 60, 30},
    {1200, 60, 30},
    {1600, 80, 40},
    {1400, 100, 0},   // warm settle
    {0, 0, 0}
};

// CHALLENGE COMPLETE: warm resolve chord — per-session task done
static const Note SND_CHALLENGE_COMPLETE[] = {
    {600, 80, 30},
    {800, 80, 30},
    {1000, 100, 0},   // bright resolve
    {0, 0, 0}
};

// CHALLENGE SWEEP: all 3 complete — triumphant full fanfare
static const Note SND_CHALLENGE_SWEEP[] = {
    {600,  60, 20},
    {800,  60, 20},
    {1000, 60, 20},
    {1200, 80, 30},
    {1500, 100, 40},
    {1800, 120, 40},
    {1600, 150, 0},   // heroic settle
    {0, 0, 0}
};

// NEAR MISS: 900→400Hz descending chirp — aversive tension (Clark 2009 SCR ~1.8x)
// no XP reward — arousal IS the hook. 3 steps = ~185ms total
static const Note SND_NEAR_MISS[] = {
    {900, 60, 15},   // high burst
    {580, 50, 15},   // mid fall
    {380, 65, 0},    // low tail
    {0, 0, 0}
};

// ==[ MORSE DEFINITIONS ]== dit=1 unit, dah=3, inter-element=1, inter-char=3
static const uint16_t MORSE_UNIT = 60;  // ms per unit (fast morse)
static const uint16_t MORSE_FREQ = 800; // Hz
// G = --.  (dah dah dit)
// W = .--  (dit dah dah)

// Morse "GG" = --. --
static const Note SND_MORSE_GG[] = {
    // First G: --
    {MORSE_FREQ, MORSE_UNIT * 3, MORSE_UNIT},      // dah
    {MORSE_FREQ, MORSE_UNIT * 3, MORSE_UNIT},      // dah
    {MORSE_FREQ, MORSE_UNIT * 1, MORSE_UNIT * 3},  // dit + inter-char pause
    // Second G: --
    {MORSE_FREQ, MORSE_UNIT * 3, MORSE_UNIT},      // dah
    {MORSE_FREQ, MORSE_UNIT * 3, MORSE_UNIT},      // dah
    {MORSE_FREQ, MORSE_UNIT * 1, 0},               // dit
    {0, 0, 0}
};

// Morse "W" = .--
static const Note SND_MORSE_W[] = {
    {MORSE_FREQ, MORSE_UNIT * 1, MORSE_UNIT},      // dit
    {MORSE_FREQ, MORSE_UNIT * 3, MORSE_UNIT},      // dah
    {MORSE_FREQ, MORSE_UNIT * 3, 0},               // dah
    {0, 0, 0}
};

// Morse "DEAUTH" = -.. . .- ..- - .... (D,E,A,U,T,H)
static const Note SND_MORSE_DEAUTH[] = {
    // D: -..
    {MORSE_FREQ, MORSE_UNIT * 3, MORSE_UNIT},      // dah
    {MORSE_FREQ, MORSE_UNIT * 1, MORSE_UNIT},      // dit
    {MORSE_FREQ, MORSE_UNIT * 1, MORSE_UNIT * 3},  // dit + inter-char
    // E: 
    {MORSE_FREQ, MORSE_UNIT * 1, MORSE_UNIT * 3},  // dit + inter-char
    // A: .-
    {MORSE_FREQ, MORSE_UNIT * 1, MORSE_UNIT},      // dit
    {MORSE_FREQ, MORSE_UNIT * 3, MORSE_UNIT * 3},  // dah + inter-char
    // U: ..-
    {MORSE_FREQ, MORSE_UNIT * 1, MORSE_UNIT},      // dit
    {MORSE_FREQ, MORSE_UNIT * 1, MORSE_UNIT},      // dit
    {MORSE_FREQ, MORSE_UNIT * 3, MORSE_UNIT * 3},  // dah + inter-char
    // T: -
    {MORSE_FREQ, MORSE_UNIT * 3, MORSE_UNIT * 3},  // dah + inter-char
    // H: ....
    {MORSE_FREQ, MORSE_UNIT * 1, MORSE_UNIT},      // dit
    {MORSE_FREQ, MORSE_UNIT * 1, MORSE_UNIT},      // dit
    {MORSE_FREQ, MORSE_UNIT * 1, MORSE_UNIT},      // dit
    {MORSE_FREQ, MORSE_UNIT * 1, 0},               // dit (final)
    {0, 0, 0}
};

// ==[ STATE MACHINE ]==
static Note dynamicSeqBuf[4];  // copy buffer for mutable sequences (bird poof etc.)
static const Note* currentSequence = nullptr;
static uint8_t currentStep = 0;
static uint32_t stepStartTime = 0;
static bool inNote = false;  // true = playing tone, false = in pause
static bool morseGGQueued = false;
static bool morseWQueued = false;

// ==[ EVENT RING BUFFER ]== prevents event loss under rapid fire
static constexpr uint8_t QUEUE_SIZE = 8;  // 8 bytes DRAM. prevents event loss during rapid captures.
static Event eventQueue[QUEUE_SIZE] = {NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE};
static bool priorityFlag[QUEUE_SIZE] = {};  // parallel: true if event needs preemption
static uint8_t queueHead = 0;  // next write position
static uint8_t queueTail = 0;  // next read position

// ==[ VOLUME ]== 0-10, 0=mute (but sound enabled)
static uint8_t volume = 2;  // default 20%

// ==[ PITCH ENGINE ]== mood-reactive + combo escalation
static float moodPitchMult = 1.0f;
static float comboPitchMult = 1.0f;
static Event currentEvent = NONE;

// Route pitch to appropriate sounds: mood→creature/ambient, combo→capture rewards
static uint16_t applyPitch(uint16_t freq, Event evt) {
    float mult = 1.0f;
    switch (evt) {
        // Creature/ambient: mood pitch
        case OINK_GRUNT:
        case CUTE_JUMP:
        case SNIFF:
        case CAT_MEOW:
        case CLIENT_NEW:
            mult = moodPitchMult;
            break;
        // Capture rewards: combo pitch
        case PMKID:
        case EAPOL_M4:
        case FIRST_CATCH:
            mult = comboPitchMult;
            break;
        default:
            return freq;  // no pitch shift
    }
    float shifted = freq * mult;
    if (shifted < 60.0f) shifted = 60.0f;
    if (shifted > 4000.0f) shifted = 4000.0f;
    return (uint16_t)shifted;
}

// ==[ IMPLEMENTATION ]==

void init() {
    currentSequence = nullptr;
    currentStep = 0;
    queueHead = 0;
    queueTail = 0;
    morseGGQueued = false;
    morseWQueued = false;
    
    // Load volume from Config — set master to max, control via channel volume
    volume = Config::getSfxVolume();
    M5.Speaker.setVolume(255);  // master full; individual channels control mix
    M5.Speaker.setChannelVolume(SFX_CH, volume * 12);  // 0-10 -> 0-120 (half loudness)
}

void play(Event event) {
    if (!Config::getSoundEnabled() || BathMic::isAudioBusReserved()) return;

    // priority flag — preemption handled in update() (main-loop safe)
    bool isPriority = (event == PMKID || event == EAPOL_M4 || event == FIRST_CATCH ||
                       event == LEVEL_UP || event == RIB_ESCAPE);

    // enqueue event (ring buffer - drops oldest if full)
    uint8_t nextHead = (queueHead + 1) % QUEUE_SIZE;
    if (nextHead == queueTail) {
        // buffer full - advance tail (drop oldest)
        queueTail = (queueTail + 1) % QUEUE_SIZE;
    }
    eventQueue[queueHead] = event;
    priorityFlag[queueHead] = isPriority;
    queueHead = nextHead;
}

static void startSequence(const Note* seq, Event evt = NONE) {
    currentSequence = seq;
    currentStep = 0;
    stepStartTime = millis();
    inNote = true;
    currentEvent = evt;

    // Start first note (pitch-shifted if applicable)
    if (seq[0].freq > 0 && seq[0].duration > 0) {
        M5.Speaker.tone(applyPitch(seq[0].freq, evt), seq[0].duration, SFX_CH);
    }
}

// BIRD_HIT: very short poof, randomized each trigger (pitch/timing jitter).
// Writes into dynamicSeqBuf so re-trigger can't corrupt in-flight playback.
static void startBirdHitPoof(Event evt) {
    uint16_t base = (uint16_t)(780 + (esp_random() % 281));      // 780-1060 Hz
    uint16_t snap = (uint16_t)(base + (esp_random() % 221));      // +0..220 Hz
    uint16_t d0 = (uint16_t)(8 + (esp_random() % 7));             // 8-14 ms
    uint16_t p0 = (uint16_t)(2 + (esp_random() % 4));             // 2-5 ms
    uint16_t d1 = (uint16_t)(6 + (esp_random() % 6));             // 6-11 ms
    dynamicSeqBuf[0] = {snap, d0, p0};
    dynamicSeqBuf[1] = {base, d1, 0};
    dynamicSeqBuf[2] = {0, 0, 0};
    startSequence(dynamicSeqBuf, evt);
}

bool update() {
    if (BathMic::isAudioBusReserved()) return false;
    // Priority preemption — dequeue side, always main-loop context
    if (queueTail != queueHead && currentSequence != nullptr) {
        // scan queue for priority event
        uint8_t scan = queueTail;
        while (scan != queueHead) {
            if (priorityFlag[scan]) {
                // kill current playback for capture feedback
                M5.Speaker.stop(SFX_CH);
                currentSequence = nullptr;
                currentStep = 0;
                morseGGQueued = false;
                morseWQueued = false;
                break;
            }
            scan = (scan + 1) % QUEUE_SIZE;
        }
    }

    // Morse follow-up BEFORE queue drain — JACKPOT→GG / FIRST→W stay glued
    if (currentSequence == nullptr) {
        if (morseGGQueued) {
            morseGGQueued = false;
            startSequence(SND_MORSE_GG);
            return true;
        }
        if (morseWQueued) {
            morseWQueued = false;
            startSequence(SND_MORSE_W);
            return true;
        }
    }

    // Process queued event (from ring buffer)
    if (queueTail != queueHead && currentSequence == nullptr) {
        Event e = eventQueue[queueTail];
        priorityFlag[queueTail] = false;
        queueTail = (queueTail + 1) % QUEUE_SIZE;

        switch (e) {
            case DEAUTH:        startSequence(SND_DEAUTH, e); break;
            case EAPOL_M1:      startSequence(SND_EAPOL_M1, e); break;
            case EAPOL_M2:      startSequence(SND_EAPOL_M2, e); break;
            case EAPOL_M3:      startSequence(SND_EAPOL_M3, e); break;
            case EAPOL_M4:
                startSequence(SND_JACKPOT, e);
                morseGGQueued = true;
                break;
            case PMKID:         startSequence(SND_PMKID, e); break;
            case FIRST_CATCH:
                startSequence(SND_FIRST, e);
                morseWQueued = true;
                break;
            case MILESTONE:     startSequence(SND_MORSE_GG, e); break;
            case CLIENT_NEW:    startSequence(SND_CLIENT, e); break;
            case SIGNAL_LOST:   startSequence(SND_SIGNAL_LOST, e); break;
            case RING:          startSequence(SND_RING, e); break;
            case ERROR:         startSequence(SND_ERROR, e); break;
            case PARANOIA_ALERT: startSequence(SND_MORSE_DEAUTH, e); break;
            case RAIN_TICK:     startSequence(SND_RAIN_TICK, e); break;
            case SIGNAL_LOCK:   startSequence(SND_SIGNAL_LOCK, e); break;
            case HAMLET_BOOT:  startSequence(SND_HAMLET_BOOT, e); break;
            case TRANSMISSION_BURST: startSequence(SND_TRANSMISSION_BURST, e); break;
            case BIRD_HIT:      startBirdHitPoof(e); break;
            case BIRD_IMPACT:   startSequence(SND_BIRD_IMPACT, e); break;
            case OINK_GRUNT:    startSequence(SND_OINK_GRUNT, e); break;
            case SHIP_EXPLODE:  startSequence(SND_SHIP_EXPLODE, e); break;
            // ==[ NEW EARCONS ]==
            case LEVEL_UP:      startSequence(SND_LEVEL_UP, e); break;
            case GOAL_COMPLETE: startSequence(SND_GOAL_COMPLETE, e); break;
            case CRITICAL_TICK: startSequence(SND_CRITICAL_TICK, e); break;
            case RIB_ESCAPE:    startSequence(SND_RIB_ESCAPE, e); break;
            case HUNT_CAMP:     startSequence(SND_HUNT_CAMP, e); break;
            case HUNT_PATROL:   startSequence(SND_HUNT_PATROL, e); break;
            case HUNT_SPRINT:   startSequence(SND_HUNT_SPRINT, e); break;
            case HUNT_LURK:     startSequence(SND_HUNT_LURK, e); break;
            case SESSION_ACTIVE: startSequence(SND_SESSION_ACTIVE, e); break;
            case THUNDER_RUMBLE: startSequence(SND_THUNDER_RUMBLE, e); break;
            case CUTE_JUMP:     startSequence(SND_CUTE_JUMP, e); break;
            case WALK_MILESTONE: startSequence(SND_WALK_MILESTONE, e); break;
            case PLUG_IN:       startSequence(SND_PLUG_IN, e); break;
            case SNIFF:         startSequence(SND_SNIFF, e); break;
            case CAT_MEOW:      startSequence(SND_CAT_MEOW, e); break;
            case CAT_PAW_TAP:   startSequence(SND_CAT_PAW_TAP, e); break;
            case CAT_KNOCK:     startSequence(SND_CAT_KNOCK, e); break;
            case CAT_CLATTER:   startSequence(SND_CAT_CLATTER, e); break;
            case SHOCKWAVE_BOOM: startSequence(SND_SHOCKWAVE_BOOM, e); break;
            case DEBRIS_RAIN_START: startSequence(SND_DEBRIS_RAIN_START, e); break;
            case GESTURE_LONG_PIP: startSequence(SND_GESTURE_LONG_PIP, e); break;
            case GESTURE_SUPER_LONG_PIP: startSequence(SND_GESTURE_SUPER_LONG_PIP, e); break;
            // ==[ REENTRY CINEMATIC ]==
            case REENTRY_WHISPER: startSequence(SND_REENTRY_WHISPER, e); break;
            case REENTRY_RUMBLE:  startSequence(SND_REENTRY_RUMBLE, e); break;
            case REENTRY_ROAR:   startSequence(SND_REENTRY_ROAR, e); break;
            case THRUSTER_POP:   startSequence(SND_THRUSTER_POP, e); break;
            // ==[ IPP DEFENSE ]==
            case RECON_ALERT:        startSequence(SND_RECON_ALERT, e); break;
            case TRACKER_FOLLOWING:  startSequence(SND_TRACKER_FOLLOWING, e); break;
            // ==[ GAMIFICATION V3 ]==
            case ACHIEVEMENT_UNLOCK:  startSequence(SND_ACHIEVEMENT_UNLOCK, e); break;
            case CHALLENGE_COMPLETE:  startSequence(SND_CHALLENGE_COMPLETE, e); break;
            case CHALLENGE_SWEEP:     startSequence(SND_CHALLENGE_SWEEP, e); break;
            case NEAR_MISS:           startSequence(SND_NEAR_MISS, e); break;
            case GPS_FIX_LOCK:        startSequence(SND_GPS_FIX_LOCK, e); break;
            case GPS_FIX_WARNING:     startSequence(SND_GPS_FIX_WARNING, e); break;
            default: break;
        }
    }

    // Nothing playing, nothing queued
    if (currentSequence == nullptr) return false;
    
    uint32_t now = millis();
    const Note& note = currentSequence[currentStep];
    
    // Check if sequence ended — kill speaker to prevent last-note bleed
    if (note.duration == 0) {
        M5.Speaker.stop(SFX_CH);
        currentSequence = nullptr;
        currentStep = 0;
        return morseGGQueued || morseWQueued;  // More to play?
    }
    
    if (inNote) {
        // In note phase - wait for duration
        if (now - stepStartTime >= note.duration) {
            // Note finished, enter pause phase
            inNote = false;
            stepStartTime = now;
            
            // If no pause, advance immediately
            if (note.pause == 0) {
                currentStep++;
                const Note& next = currentSequence[currentStep];
                if (next.duration == 0) {
                    M5.Speaker.stop(SFX_CH);
                    currentSequence = nullptr;
                    currentStep = 0;
                    return morseGGQueued || morseWQueued;
                }
                inNote = true;
                stepStartTime = now;
                if (next.freq > 0) {
                    M5.Speaker.tone(applyPitch(next.freq, currentEvent), next.duration, SFX_CH);
                }
            }
        }
    } else {
        // In pause phase - wait for pause duration
        if (now - stepStartTime >= note.pause) {
            // Pause finished, advance to next note
            currentStep++;
            const Note& next = currentSequence[currentStep];
            if (next.duration == 0) {
                M5.Speaker.stop(SFX_CH);
                currentSequence = nullptr;
                currentStep = 0;
                return morseGGQueued || morseWQueued;
            }
            inNote = true;
            stepStartTime = now;
            if (next.freq > 0) {
                M5.Speaker.tone(applyPitch(next.freq, currentEvent), next.duration, SFX_CH);
            }
        }
    }
    
    return true;
}

bool isPlaying() {
    return currentSequence != nullptr || morseGGQueued || morseWQueued || (queueTail != queueHead);
}

void stop() {
    currentSequence = nullptr;
    currentStep = 0;
    queueHead = queueTail = 0;  // clear ring buffer
    morseGGQueued = false;
    morseWQueued = false;
    M5.Speaker.stop(SFX_CH);
}

uint8_t getVolume() {
    return volume;
}

void setVolume(uint8_t vol) {
    volume = (vol > 10) ? 10 : vol;
    Config::setSfxVolume(volume);
    M5.Speaker.setChannelVolume(SFX_CH, volume * 12);  // 0-10 -> 0-120 (half loudness)
}

void tone(uint16_t freq, uint16_t duration) {
    if (!Config::getSoundEnabled() || BathMic::isAudioBusReserved()) return;
    // kill active sequence — direct tones take immediate control
    if (currentSequence != nullptr) {
        currentSequence = nullptr;
        currentStep = 0;
    }
    M5.Speaker.tone(freq, duration, SFX_CH);
}

void click() {
    if (!Config::getSoundEnabled() || BathMic::isAudioBusReserved()) return;
    // kill active sequence — clicks take immediate control
    if (currentSequence != nullptr) {
        currentSequence = nullptr;
        currentStep = 0;
    }
    // Geiger-style click: very short pulse
    uint16_t freq = 900 + (esp_random() % 200);  // 900-1100Hz random
    M5.Speaker.tone(freq, 8, SFX_CH);  // 8ms click
}

void morseGG() {
    if (!Config::getSoundEnabled() || BathMic::isAudioBusReserved()) return;
    if (currentSequence == nullptr) {
        startSequence(SND_MORSE_GG);
    } else {
        morseGGQueued = true;
    }
}

void morseW() {
    if (!Config::getSoundEnabled() || BathMic::isAudioBusReserved()) return;
    if (currentSequence == nullptr) {
        startSequence(SND_MORSE_W);
    } else {
        morseWQueued = true;
    }
}

// ==[ PITCH API ]== mood tier + capture combo
void setMoodPitch(float multiplier) {
    if (multiplier < 0.5f) multiplier = 0.5f;
    if (multiplier > 1.5f) multiplier = 1.5f;
    moodPitchMult = multiplier;
}

float getMoodPitch() { return moodPitchMult; }

void setComboPitch(float multiplier) {
    if (multiplier < 1.0f) multiplier = 1.0f;
    if (multiplier > 1.19f) multiplier = 1.19f;
    comboPitchMult = multiplier;
}

float getComboPitch() { return comboPitchMult; }

void resetComboPitch() { comboPitchMult = 1.0f; }

}  // namespace SFX
