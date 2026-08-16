/**
 * Noir Jazz — Procedural noir jazz ambient generator
 *
 * PCM synthesis at 8 kHz, 8-bit signed mono.
 * Double-buffered: while one bar plays via M5.Speaker.playRaw(),
 * the other bar is synthesized in the background.
 *
 * Instruments:
 *   1. Walking bass  — sine oscillator, Am pentatonic root motion
 *   2. Muted trumpet — filtered sawtooth, sparse melody fragments
 *   3. Rhodes pad    — detuned sine pair with tremolo
 *   4. Brush drums   — LFSR noise shaped by envelope, backbeat pattern
 *   5. Rain drops    — ultra-sparse filtered noise ticks, haptic-synced
 *
 * All buffers allocated in PSRAM to avoid eating internal SRAM.
 */

#include "noir_jazz.h"
#include "../core/config.h"
#include "../util/debug_log.h"
#include "../util/time_math.h"
#include <M5Unified.h>
#include <esp_heap_caps.h>
#include <string.h>

namespace NoirJazz {

// ── constants ───────────────────────────────────────────────────────
static constexpr int SAMPLE_RATE   = 8000;
static constexpr int SPEAKER_CH    = 1;       // SFX uses ch0

static uint8_t musicVol = 5;  // 0-10, loaded from Config

// Map 0-10 user scale to 0-255 channel volume
static uint8_t channelVolume() { return musicVol * 25; }

// Bar timing: at 75 BPM, 1 bar (4 beats) = 3.2s = 25600 samples @ 8kHz
// We allow BPM range 68-88, so buffer must hold the slowest bar
static constexpr int MAX_BAR_SAMPLES = 28240; // ceil(4 * 60/68 * 8000)
static constexpr int BARS_PER_PHRASE = 4;     // harmonic cycle length

// ── sine table (256 entries, amplitude ±127) ────────────────────────
static int8_t sineTab[256];

static void buildSineTable() {
    for (int i = 0; i < 256; i++) {
        float angle = (float)i / 256.0f * 6.2831853f;
        float s = sinf(angle);
        sineTab[i] = (int8_t)(s * 127.0f);
    }
}

/// Fixed-point phase: 16.16 format. Callers pre-shift >> 8,
/// so low byte here IS the table index (bits 15:8 of raw phase).
static inline int8_t sineLookup(uint32_t phase) {
    uint8_t idx = (uint8_t)(phase);  // callers already >> 8
    return sineTab[idx];
}

// ── LFSR noise (16-bit Galois) ──────────────────────────────────────
static uint16_t lfsr = 0xACE1u;

static inline int8_t noiseNext() {
    uint16_t bit = lfsr & 1u;
    lfsr >>= 1;
    if (bit) lfsr ^= 0xB400u;
    return (int8_t)(lfsr & 0x7F) - 64;
}

// ── scale: Am pentatonic (A C D E G) across two octaves ─────────────
// Frequencies in 16.16 phase-increment format for 8kHz sample rate:
//   phaseInc = (freq * 65536) / SAMPLE_RATE
// Using fixed table to avoid runtime float division
struct ScaleNote {
    uint16_t freq;       // Hz
    uint16_t phaseInc;   // 16.16 fixed >> 16 not needed; store as 16-bit
};

// Bass range (octave 2-3): A2=110, C3=131, D3=147, E3=165, G3=196
static const ScaleNote bassNotes[] = {
    {110, (uint16_t)((110UL * 65536UL) / SAMPLE_RATE)},   // A2
    {131, (uint16_t)((131UL * 65536UL) / SAMPLE_RATE)},   // C3
    {147, (uint16_t)((147UL * 65536UL) / SAMPLE_RATE)},   // D3
    {165, (uint16_t)((165UL * 65536UL) / SAMPLE_RATE)},   // E3
    {196, (uint16_t)((196UL * 65536UL) / SAMPLE_RATE)},   // G3
};
static constexpr int NUM_BASS_NOTES = 5;

// Pad chord voicings — 4 chords, 3 notes each (root, third, fifth/seventh)
// Bar 0: Am  (A3, C4, E4)
// Bar 1: Dm7 (D4, F4, C4)   — iv7, noir tension
// Bar 2: Em  (E3, G3, B3)   — v, dark minor dominant
// Bar 3: Am  (A3, C4, E4)   — resolve back home
struct PadChord {
    uint16_t inc1, inc2, inc3;
};
static const PadChord padChords[] = {
    { (uint16_t)((220UL*65536UL)/SAMPLE_RATE),   // Am:  A3=220
      (uint16_t)((262UL*65536UL)/SAMPLE_RATE),   //      C4=262
      (uint16_t)((330UL*65536UL)/SAMPLE_RATE) },  //      E4=330
    { (uint16_t)((294UL*65536UL)/SAMPLE_RATE),   // Dm7: D4=294
      (uint16_t)((349UL*65536UL)/SAMPLE_RATE),   //      F4=349
      (uint16_t)((262UL*65536UL)/SAMPLE_RATE) },  //      C4=262
    { (uint16_t)((165UL*65536UL)/SAMPLE_RATE),   // Em:  E3=165
      (uint16_t)((196UL*65536UL)/SAMPLE_RATE),   //      G3=196
      (uint16_t)((247UL*65536UL)/SAMPLE_RATE) },  //      B3=247
    { (uint16_t)((220UL*65536UL)/SAMPLE_RATE),   // Am:  A3=220
      (uint16_t)((262UL*65536UL)/SAMPLE_RATE),   //      C4=262
      (uint16_t)((330UL*65536UL)/SAMPLE_RATE) },  //      E4=330
};

// Sax range (octave 2-3): deep baritone, noir jazz
// A2=110, C3=131, D3=147, E3=165, G3=196
static const ScaleNote saxNotes[] = {
    {110, (uint16_t)((110UL * 65536UL) / SAMPLE_RATE)},   // A2
    {131, (uint16_t)((131UL * 65536UL) / SAMPLE_RATE)},   // C3
    {147, (uint16_t)((147UL * 65536UL) / SAMPLE_RATE)},   // D3
    {165, (uint16_t)((165UL * 65536UL) / SAMPLE_RATE)},   // E3
    {196, (uint16_t)((196UL * 65536UL) / SAMPLE_RATE)},   // G3
};
static constexpr int NUM_SAX_NOTES = 5;

// ═══ BLADE RUNNER mode tables ═══════════════════════════════════════
// Key: F#m, two chords (F#m, D), F# minor blues scale for lead

// Bass: F# pedal drone (F#1=46, F#2=93)
static const ScaleNote brBassNotes[] = {
    {46,  (uint16_t)((46UL  * 65536UL) / SAMPLE_RATE)},   // F#1
    {93,  (uint16_t)((93UL  * 65536UL) / SAMPLE_RATE)},   // F#2
};

// Pad: F#m(F#3=185, A3=220, C#4=277) and D(D3=147, F#3=185, A3=220)
// 4-bar cycle: F#m, F#m, D, D
static const PadChord brPadChords[] = {
    { (uint16_t)((185UL*65536UL)/SAMPLE_RATE),
      (uint16_t)((220UL*65536UL)/SAMPLE_RATE),
      (uint16_t)((277UL*65536UL)/SAMPLE_RATE) },  // F#m
    { (uint16_t)((185UL*65536UL)/SAMPLE_RATE),
      (uint16_t)((220UL*65536UL)/SAMPLE_RATE),
      (uint16_t)((277UL*65536UL)/SAMPLE_RATE) },  // F#m
    { (uint16_t)((147UL*65536UL)/SAMPLE_RATE),
      (uint16_t)((185UL*65536UL)/SAMPLE_RATE),
      (uint16_t)((220UL*65536UL)/SAMPLE_RATE) },  // D
    { (uint16_t)((147UL*65536UL)/SAMPLE_RATE),
      (uint16_t)((185UL*65536UL)/SAMPLE_RATE),
      (uint16_t)((220UL*65536UL)/SAMPLE_RATE) },  // D
};

// Lead: F# minor blues scale — F#3, A3, B3, C4, C#4, E4
static const ScaleNote brLeadNotes[] = {
    {185, (uint16_t)((185UL * 65536UL) / SAMPLE_RATE)},   // F#3
    {220, (uint16_t)((220UL * 65536UL) / SAMPLE_RATE)},   // A3
    {247, (uint16_t)((247UL * 65536UL) / SAMPLE_RATE)},   // B3
    {262, (uint16_t)((262UL * 65536UL) / SAMPLE_RATE)},   // C4
    {277, (uint16_t)((277UL * 65536UL) / SAMPLE_RATE)},   // C#4
    {330, (uint16_t)((330UL * 65536UL) / SAMPLE_RATE)},   // E4
};
static constexpr int NUM_BR_LEAD = 6;

// ── state ───────────────────────────────────────────────────────────
static int8_t* bufA = nullptr;
static int8_t* bufB = nullptr;
static int8_t* playBuf = nullptr;   // currently feeding speaker
static int8_t* fillBuf = nullptr;   // being synthesized
static int      fillLen = 0;        // samples in fillBuf
static int      playLen = 0;        // samples in playBuf

static bool     playing = false;
static bool     stopping = false;
static bool     initDone = false;
static float    tension = 0.0f;     // 0=calm, 1=intense
static float    fadeGain = 0.0f;    // 0..1 fade envelope
static uint32_t barCount = 0;       // running bar counter (for pattern)
static uint32_t rngState = 0x12345678u;  // deterministic PRNG for melody
static uint32_t saxRngState = 0x87654321u; // separate RNG for sax (avoids backing track jumps)
static uint32_t lastFadeMs = 0;            // wall-clock fade timing

// Oscillator phases (persistent across bars for continuity)
static uint32_t bassPhase = 0;
static uint32_t padPhase1 = 0;
static uint32_t padPhase2 = 0;
static uint32_t padPhase3 = 0;
static uint32_t saxPhase  = 0;

// Mode control
static Mode currentMode = Mode::NOIR_JAZZ;

// Saxophone / lead layer — slow crossfade controlled by external state
static bool     saxWanted  = false;  // true when terminal/rooftop active
static float    saxGain    = 0.0f;   // 0..1, ramps slowly per bar
static int      saxNoteIdx = 0;      // current note in legato phrase
static int      saxPhraseBar = 0;    // bars into current phrase

// Blade Runner lead state
static int      brLeadNoteIdx  = 0;
static int      brLeadHeldBars = 0;   // bars left on current note
static int      brLeadRestBars = 0;   // rest bars before next phrase
static int      brLeadNextRest = 0;   // scheduled rest after current note
static uint32_t brLeadPhase2   = 0;   // second detuned voice
static float    brLeadFilter   = 0.0f; // LP filter state 0=closed 1=open

// Blade Runner bass state
static int      brBassNoteIdx  = 1;   // start on F#2
static int      brBassHeldBars = 0;

static bool fillSynthPending = false;
static bool playSynthPending = false;
// Fires in sync with actual audio playback, not bar pre-rendering.
static uint32_t barStartMs      = 0;     // millis() when current bar began playing
static uint32_t barDurationMs   = 0;     // duration of current bar in ms
static bool     beat2HapticDone = false;  // fired haptic for beat 2 this bar?
static bool     beat4HapticDone = false;  // fired haptic for beat 4 this bar?
static bool     backbeatHitFlag = false;  // consumable by hamlet
static bool     rainDropFlag    = false;  // consumable by hamlet
static uint32_t nextRainMs      = 0;     // random time for next rain haptic
static bool     rainScheduled   = false;  // rain haptic pending this bar?

// ── simple PRNG (xorshift32) ────────────────────────────────────────
static uint32_t rng() {
    rngState ^= rngState << 13;
    rngState ^= rngState >> 17;
    rngState ^= rngState << 5;
    return rngState;
}

// Sax-private RNG — prevents sax activation from shifting backing track
static uint32_t saxRng() {
    saxRngState ^= saxRngState << 13;
    saxRngState ^= saxRngState >> 17;
    saxRngState ^= saxRngState << 5;
    return saxRngState;
}

// ── BPM from tension ────────────────────────────────────────────────
static int getBPM() {
    if (currentMode == Mode::BLADE_RUNNER) {
        return 60 + (int)(tension * 8.0f);  // 60-68 BPM
    }
    return 68 + (int)(tension * 20.0f);  // 68-88 BPM
}

static int samplesPerBar() {
    int bpm = getBPM();
    // 4 beats per bar: samples = 4 * 60 / bpm * sampleRate
    return (4 * 60 * SAMPLE_RATE) / bpm;
}

// ── envelope shapes ─────────────────────────────────────────────────
// pos = sample position within note, len = total note length
// Returns 0..255

static uint8_t envADSR(int pos, int len, int attackMs, int releaseMs) {
    if (len <= 0 || pos < 0 || pos >= len) return 0;
    int attackSamp = (attackMs * SAMPLE_RATE) / 1000;
    int releaseSamp = (releaseMs * SAMPLE_RATE) / 1000;
    // Clamp: if note is shorter than attack+release, split proportionally
    if (attackSamp + releaseSamp > len) {
        attackSamp = len / 3;
        releaseSamp = len / 3;
    }
    int sustainEnd = len - releaseSamp;
    if (pos < attackSamp) {
        return (attackSamp > 0) ? (uint8_t)((pos * 255) / attackSamp) : 200;
    } else if (pos < sustainEnd) {
        return 200;  // sustain level
    } else {
        int rPos = pos - sustainEnd;
        return (releaseSamp > 0) ? (uint8_t)(200 - (rPos * 200) / releaseSamp) : 0;
    }
}

static uint8_t envPercussive(int pos, int len) {
    // Fast attack, exponential-ish decay
    if (pos >= len) return 0;
    int decay = (pos * 255) / len;
    int env = 255 - decay;
    return (uint8_t)((env * env) >> 8);  // squared for faster falloff
}

// ── mix levels (single place to tune balance) ──────────────────────
static constexpr int MIX_BASS        = 60;   // was 100 — too dominant
static constexpr int MIX_PAD         = 90;   // was 50 — barely audible
static constexpr int MIX_BRUSH_BACK  = 80;   // was 60
static constexpr int MIX_BRUSH_GHOST = 35;   // was 25
static constexpr int MIX_SAX         = 110;  // was 90 — lead voice
static constexpr int MIX_RAIN        = 25;   // was 15

/// Consistent swing offset for bass + brush lockup
static int swingOffset(int samplesPerBeat) { return samplesPerBeat / 10; }

// ── instrument renderers ────────────────────────────────────────────
// Each writes additively into the buffer for one bar

/// Walking bass: quarter notes, Am pentatonic root motion
/// 2nd harmonic for upright body, swing on backbeats
static void renderBass(int8_t* buf, int barSamples) {
    int beatsInBar = 4;
    int samplesPerBeat = barSamples / beatsInBar;
    int sw = swingOffset(samplesPerBeat);

    // Pick 4 notes for this bar using walking pattern
    int noteIndices[4];
    int prevNote = (barCount == 0) ? 0 : (int)(rng() % NUM_BASS_NOTES);
    for (int b = 0; b < 4; b++) {
        if ((rng() & 7) == 0) {
            noteIndices[b] = 0;
        } else {
            int step = 1 + (int)(rng() & 1);
            if (rng() & 1) step = -step;
            int n = prevNote + step;
            if (n < 0) n += NUM_BASS_NOTES;
            if (n >= NUM_BASS_NOTES) n -= NUM_BASS_NOTES;
            noteIndices[b] = n;
        }
        prevNote = noteIndices[b];
    }

    for (int beat = 0; beat < beatsInBar; beat++) {
        int startSamp = beat * samplesPerBeat;
        // Swing on backbeats — same offset as brush
        if (beat == 1 || beat == 3) startSamp += sw;
        uint16_t inc = bassNotes[noteIndices[beat]].phaseInc;

        for (int s = 0; s < samplesPerBeat && (startSamp + s) < barSamples; s++) {
            uint8_t env = envADSR(s, samplesPerBeat, 20, 120);
            int8_t osc1 = sineLookup(bassPhase >> 8);
            // 2nd harmonic at ~20% for upright bass body
            int8_t osc2 = sineLookup((bassPhase * 2) >> 8);
            int osc = osc1 + (osc2 >> 2);
            int val = (osc * env) >> 8;
            int mixed = buf[startSamp + s] + ((val * MIX_BASS) >> 8);
            if (mixed > 127) mixed = 127;
            if (mixed < -128) mixed = -128;
            buf[startSamp + s] = (int8_t)mixed;
            bassPhase += inc;
        }
    }
}

/// Rhodes pad: sustained chord, detuned sines with tremolo
/// Am → Dm7 → Em → Am progression (4-bar phrase)
/// Wider detune + slower tremolo for dreamy atmospheric wash
static void renderPad(int8_t* buf, int barSamples) {
    int chordIdx = (int)(barCount % BARS_PER_PHRASE);
    const PadChord& ch = padChords[chordIdx];
    uint16_t inc1 = ch.inc1;
    // Wider detune: voice 2 sharp by 2, voice 3 flat by 1 → chorus shimmer
    uint16_t inc2 = ch.inc2 + 2;
    uint16_t inc3 = (ch.inc3 > 1) ? (ch.inc3 - 1) : (ch.inc3 + 1);

    for (int s = 0; s < barSamples; s++) {
        // Tremolo: ~3Hz for dreamy wash (was 4Hz)
        uint32_t tremPhase = (uint32_t)(s * 25);
        int8_t trem = sineLookup(tremPhase >> 8);
        uint8_t tremEnv = 190 + (trem >> 2);  // 158..222 range

        // Slower attack/release for atmospheric pad feel
        uint8_t env = envADSR(s, barSamples, 300, 400);

        int8_t o1 = sineLookup(padPhase1 >> 8);
        int8_t o2 = sineLookup(padPhase2 >> 8);
        int8_t o3 = sineLookup(padPhase3 >> 8);

        int chord = (o1 + o2 + o3) / 3;
        int val = (chord * env * tremEnv) >> 16;

        int mixed = buf[s] + ((val * MIX_PAD) >> 8);
        if (mixed > 127) mixed = 127;
        if (mixed < -128) mixed = -128;
        buf[s] = (int8_t)mixed;

        padPhase1 += inc1;
        padPhase2 += inc2;
        padPhase3 += inc3;
    }
}

/// Brush percussion: filtered noise on beats 2 and 4 (backbeat), ghost notes.
/// Ride tick on beat 1 for timekeeping.
/// Swing uses shared swingOffset() so bass and drums lock together.
static void renderBrush(int8_t* buf, int barSamples) {
    int beatsInBar = 4;
    int samplesPerBeat = barSamples / beatsInBar;
    int sw = swingOffset(samplesPerBeat);

    for (int beat = 0; beat < beatsInBar; beat++) {
        int startSamp = beat * samplesPerBeat;
        bool isBackbeat = (beat == 1 || beat == 3);

        bool hasHit;
        int hitVolume;
        int hitLen;
        if (isBackbeat) {
            hasHit = true;
            hitVolume = MIX_BRUSH_BACK + (int)(tension * 30.0f);
            hitLen = samplesPerBeat / 3;
        } else {
            hasHit = ((rng() & 0xFF) < 77);
            hitVolume = MIX_BRUSH_GHOST;
            hitLen = samplesPerBeat / 5;
        }

        if (!hasHit) continue;

        // Same swing as bass — everything locks together
        if (isBackbeat) startSamp += sw;

        for (int s = 0; s < hitLen && (startSamp + s) < barSamples; s++) {
            uint8_t env = envPercussive(s, hitLen);
            int8_t noise = noiseNext();
            static int8_t prevNoise = 0;
            int8_t filtered = (noise + prevNoise) / 2;
            prevNoise = noise;

            int val = (filtered * env) >> 8;
            int mixed = buf[startSamp + s] + ((val * hitVolume) >> 8);
            if (mixed > 127) mixed = 127;
            if (mixed < -128) mixed = -128;
            buf[startSamp + s] = (int8_t)mixed;
        }
    }

    // Ride tick on beat 1 — soft filtered noise for timekeeping
    int rideLen = samplesPerBeat / 10;
    int rideVol = 18 + (int)(tension * 10.0f);
    static int8_t ridePrev = 0;
    for (int s = 0; s < rideLen && s < barSamples; s++) {
        uint8_t env = envPercussive(s, rideLen);
        int8_t noise = noiseNext();
        int8_t filtered = (noise + ridePrev) / 2;
        ridePrev = filtered;
        int val = (filtered * env) >> 8;
        int mixed = buf[s] + ((val * rideVol) >> 8);
        if (mixed > 127) mixed = 127;
        if (mixed < -128) mixed = -128;
        buf[s] = (int8_t)mixed;
    }
}

/// Saxophone: reed timbre via odd harmonic series (1,3,5,7) + breath noise.
/// Blade Runner style — deep baritone, sub-octave growl, breathy attack.
/// Only rendered when saxGain > 0 (fades in/out over multiple bars).
static void renderSax(int8_t* buf, int barSamples) {
    if (saxGain <= 0.001f) return;  // silent, skip

    int beatsInBar = 4;
    int samplesPerBeat = barSamples / beatsInBar;

    // Sax plays long legato phrases: 1-2 notes per bar, held across beats.
    // Every 2 bars, pick a new note. Occasional rest bar for breathing room.
    // Uses saxRng() — private RNG so sax activation doesn't shift backing track.
    bool restBar = ((saxRng() & 7) == 0);  // ~12% chance of a rest bar
    if (restBar) { saxPhraseBar++; return; }

    // Pick note: step by small intervals for smooth melody
    if ((saxPhraseBar % 2) == 0) {
        int step = (saxRng() & 1) ? 1 : -1;
        if ((saxRng() & 3) == 0) step *= 2;  // occasional leap
        saxNoteIdx += step;
        if (saxNoteIdx < 0) saxNoteIdx += NUM_SAX_NOTES;
        if (saxNoteIdx >= NUM_SAX_NOTES) saxNoteIdx -= NUM_SAX_NOTES;
    }
    saxPhraseBar++;

    uint16_t inc = saxNotes[saxNoteIdx].phaseInc;

    // Second note on beat 3 (50% chance) — stepwise from first
    bool hasSecondNote = (saxRng() & 1);
    int secondNoteIdx = saxNoteIdx + ((saxRng() & 1) ? 1 : -1);
    if (secondNoteIdx < 0) secondNoteIdx += NUM_SAX_NOTES;
    if (secondNoteIdx >= NUM_SAX_NOTES) secondNoteIdx -= NUM_SAX_NOTES;
    uint16_t inc2 = saxNotes[secondNoteIdx].phaseInc;

    int splitSample = hasSecondNote ? (samplesPerBeat * 2 + samplesPerBeat / 2) : barSamples;

    // LFSR state backup — sax noise shouldn't disturb brush pattern
    uint16_t lfsrBackup = lfsr;

    for (int s = 0; s < barSamples; s++) {
        uint16_t noteInc = (s < splitSample) ? inc : inc2;
        int noteStart = (s < splitSample) ? 0 : splitSample;
        int noteLen = (s < splitSample) ? splitSample : (barSamples - splitSample);
        int notePos = s - noteStart;

        // Long ADSR: 120ms attack, 200ms release for breathy onset
        uint8_t env = envADSR(notePos, noteLen, 120, 200);

        // Vibrato: ~5Hz sine, ±1.5% pitch — subtle, not theremin
        // Delayed onset: no vibrato in first 30% of note, then ramps in
        uint32_t vibPhase = (uint32_t)(s * 41);  // ~5Hz at 8kHz
        int8_t vib = sineLookup(vibPhase >> 8);
        float vibDepth = 0.0f;
        float noteProgress = (float)notePos / (float)(noteLen > 0 ? noteLen : 1);
        if (noteProgress > 0.3f) {
            vibDepth = (noteProgress - 0.3f) / 0.7f;  // ramp 0→1 over remaining 70%
            if (vibDepth > 1.0f) vibDepth = 1.0f;
        }
        int vibMod = (int)((float)((noteInc * vib) >> 13) * vibDepth);  // ±~1.5%

        // === Reed timbre: odd harmonic series ===
        // Real sax spectrum: strong fundamental, 3rd = "honk", 5th = nasal, 7th = buzz
        int8_t h1 = sineLookup(saxPhase >> 8);                      // fundamental
        int8_t h3 = sineLookup((saxPhase * 3) >> 8);                // 3rd: the honk
        int8_t h5 = sineLookup((saxPhase * 5) >> 8);                // 5th: nasal edge
        int8_t h7 = sineLookup((saxPhase * 7) >> 8);                // 7th: buzz
        // Sub-octave growl for dirty baritone character
        int8_t hSub = sineLookup((saxPhase >> 1) >> 8);             // sub-octave

        // Mix: 100% fund + 40% 3rd + 15% 5th + 5% 7th + 8% sub
        int reed = h1 + ((h3 * 5) >> 4) + (h5 >> 3) + (h7 >> 4) + (hSub >> 3);

        // Breath noise — stronger during attack, fading to air during sustain
        uint16_t nbit = lfsrBackup & 1u;
        lfsrBackup >>= 1;
        if (nbit) lfsrBackup ^= 0xB400u;
        int8_t breathNoise = (int8_t)(lfsrBackup & 0x3F) - 32;

        // Breath envelope: 40% at attack, fading to 10% during sustain
        float breathMix = 0.10f + 0.30f * ((noteProgress < 0.4f) ? (1.0f - noteProgress * 2.5f) : 0.0f);
        int noiseScaled = (int)((float)breathNoise * breathMix);

        int tone = reed + noiseScaled;
        // Soft clip the stacked harmonics
        if (tone > 160) tone = 160;
        if (tone < -160) tone = -160;

        int val = (tone * env) >> 9;

        // Apply sax gain (slow crossfade) and mix — lead voice level
        int scaled = (int)((float)val * saxGain);
        int mixed = buf[s] + ((scaled * MIX_SAX) >> 8);
        if (mixed > 127) mixed = 127;
        if (mixed < -128) mixed = -128;
        buf[s] = (int8_t)mixed;

        saxPhase += noteInc + vibMod;
    }
}

/// Rain drops: extremely sparse filtered noise ticks — ambience + haptic sync.
/// ~15% chance per bar, 1-2 drops placed at random positions.
/// Volume is very low (barely audible), just enough to justify the haptic tap.
static void renderRain(int8_t* buf, int barSamples) {
    // Only drop rain ~15% of bars
    if ((rng() & 0xFF) >= 38) return;

    int numDrops = 1 + (int)((rng() & 1) && tension > 0.3f);  // 1 drop, 2 if tense
    for (int d = 0; d < numDrops; d++) {
        // Random position in bar, avoiding first 200 samples
        int pos = 200 + (int)(rng() % (uint32_t)(barSamples - 600));
        int dropLen = 80 + (int)(rng() % 60);  // 80-140 samples (~10-18ms)

        for (int s = 0; s < dropLen && (pos + s) < barSamples; s++) {
            // Percussive envelope: sharp attack, quick decay
            uint8_t env = (s < 5) ? (uint8_t)(s * 50) : (uint8_t)(250 - (s * 250 / dropLen));
            if (s >= dropLen - 1) env = 0;
            int8_t noise = noiseNext();
            // Two-pole low-pass for patter feel
            static int8_t rPrev1 = 0, rPrev2 = 0;
            int8_t filt = (int8_t)((noise + rPrev1 + rPrev1 + rPrev2) / 4);
            rPrev2 = rPrev1;
            rPrev1 = filt;

            int val = (filt * env) >> 8;
            int mixed = buf[pos + s] + ((val * MIX_RAIN) >> 8);
            if (mixed > 127) mixed = 127;
            if (mixed < -128) mixed = -128;
            buf[pos + s] = (int8_t)mixed;
        }
        (void)d;  // rain haptic timing handled in update() via playback clock
    }
}

// ═══ BLADE RUNNER instrument renderers ══════════════════════════════
static constexpr int BR_MIX_BASS    = 30;
static constexpr int BR_MIX_PAD     = 80;
static constexpr int BR_MIX_LEAD    = 120;
static constexpr int BR_MIX_RAIN    = 30;

/// Blade Runner bass: gentle F#2 pedal drone
static void renderBrBass(int8_t* buf, int barSamples) {
    if (brBassHeldBars <= 0) {
        brBassNoteIdx = 1;  // stay on F#2, skip doom-y F#1
        brBassHeldBars = 4;
    }
    brBassHeldBars--;
    uint16_t inc = brBassNotes[brBassNoteIdx].phaseInc;

    for (int s = 0; s < barSamples; s++) {
        uint8_t env = envADSR(s, barSamples, 500, 600);
        int8_t osc = sineLookup(bassPhase >> 8);
        int val = (osc * env) >> 8;
        int mixed = buf[s] + ((val * BR_MIX_BASS) >> 8);
        if (mixed > 127) mixed = 127;
        if (mixed < -128) mixed = -128;
        buf[s] = (int8_t)mixed;
        bassPhase += inc;
    }
}

/// Blade Runner pad: airy F#m/D string wash, 3-voice detuned
static void renderBrPad(int8_t* buf, int barSamples) {
    int chordIdx = (int)(barCount % 4);
    const PadChord& ch = brPadChords[chordIdx];
    uint16_t inc1 = ch.inc1;
    uint16_t inc2 = ch.inc2 + 2;
    uint16_t inc3 = (ch.inc3 > 1) ? (ch.inc3 - 1) : (ch.inc3 + 1);

    for (int s = 0; s < barSamples; s++) {
        uint32_t tremPhase = (uint32_t)(s * 16);
        int8_t trem = sineLookup(tremPhase >> 8);
        int tremEnv = 200 + (trem >> 3);
        if (tremEnv < 80) tremEnv = 80;
        if (tremEnv > 255) tremEnv = 255;

        uint8_t env = envADSR(s, barSamples, 400, 600);

        int8_t o1 = sineLookup(padPhase1 >> 8);
        int8_t o2 = sineLookup(padPhase2 >> 8);
        int8_t o3 = sineLookup(padPhase3 >> 8);

        int chord = (o1 + o2 + o3) / 3;
        int val = (chord * env * tremEnv) >> 16;

        int mixed = buf[s] + ((val * BR_MIX_PAD) >> 8);
        if (mixed > 127) mixed = 127;
        if (mixed < -128) mixed = -128;
        buf[s] = (int8_t)mixed;

        padPhase1 += inc1;
        padPhase2 += inc2;
        padPhase3 += inc3;
    }
}

/// Blade Runner lead: CS-80 'slutty sax' — F# blues scale
/// Two detuned voices, harmonics 1-4, filter envelope, lazy vibrato
static void renderBrLead(int8_t* buf, int barSamples) {
    if (saxGain <= 0.001f) return;

    // Rest management
    if (brLeadRestBars > 0) {
        brLeadRestBars--;
        saxRng();
        return;
    }

    if (brLeadHeldBars <= 0) {
        uint32_t r = saxRng();
        if ((r & 7) == 0) {
            brLeadNoteIdx = 0;  // resolve to root
        } else {
            int step = (r & 1) ? 1 : -1;
            if ((saxRng() & 3) == 0) step *= 2;
            brLeadNoteIdx += step;
            if (brLeadNoteIdx < 0) brLeadNoteIdx += NUM_BR_LEAD;
            if (brLeadNoteIdx >= NUM_BR_LEAD) brLeadNoteIdx -= NUM_BR_LEAD;
        }
        brLeadHeldBars = 1 + (int)(saxRng() % 2);  // 1-2 bars
        brLeadFilter = 0.0f;
        brLeadNextRest = (saxRng() & 3) ? 0 : 1;  // 75% no rest
    }

    brLeadHeldBars--;
    bool isLastBar = (brLeadHeldBars == 0);
    if (isLastBar) {
        brLeadRestBars = brLeadNextRest;
    }

    uint16_t inc = brLeadNotes[brLeadNoteIdx].phaseInc;
    uint16_t inc2 = inc + (inc >> 6);  // detuned voice
    if (inc2 == inc) inc2++;

    // Second note mid-bar (50% chance)
    bool hasSecond = (saxRng() & 3) < 2;
    int secondIdx = brLeadNoteIdx + ((saxRng() & 1) ? 1 : -1);
    if (secondIdx < 0) secondIdx += NUM_BR_LEAD;
    if (secondIdx >= NUM_BR_LEAD) secondIdx -= NUM_BR_LEAD;
    uint16_t incB = brLeadNotes[secondIdx].phaseInc;
    uint16_t inc2B = incB + (incB >> 6);
    if (inc2B == incB) inc2B++;
    int split = hasSecond ? (barSamples * 5 / 8) : barSamples;

    uint16_t lfsrBak = lfsr;

    for (int s = 0; s < barSamples; s++) {
        uint16_t curInc  = (s < split) ? inc  : incB;
        uint16_t curInc2 = (s < split) ? inc2 : inc2B;
        int noteStart = (s < split) ? 0 : split;
        int noteLen = (s < split) ? split : (barSamples - split);
        int notePos = s - noteStart;
        float noteProgress = (float)notePos / (float)(noteLen > 0 ? noteLen : 1);

        // Filter envelope: starts 30% open, opens lazily
        float filterTarget;
        if (isLastBar && s >= split) {
            filterTarget = 0.2f + 0.8f * (1.0f - noteProgress * 0.8f);
            if (filterTarget < 0.2f) filterTarget = 0.2f;
        } else {
            filterTarget = 0.3f + 0.7f * (noteProgress * 3.0f);
            if (filterTarget > 1.0f) filterTarget = 1.0f;
        }
        brLeadFilter += (filterTarget - brLeadFilter) * 0.002f;
        float filt = brLeadFilter;

        // Envelope
        uint8_t env;
        if (isLastBar && s >= split) {
            env = envADSR(notePos, noteLen, 30, 800);
        } else {
            env = envADSR(notePos, noteLen, 150, 200);
        }

        // Lazy vibrato ~4Hz, wide, early onset
        uint32_t vibPhase = (uint32_t)(s * 32);
        int8_t vib = sineLookup(vibPhase >> 8);
        float vibDepth = 0.0f;
        if (noteProgress > 0.15f) {
            vibDepth = (noteProgress - 0.15f) / 0.4f;
            if (vibDepth > 1.0f) vibDepth = 1.0f;
        }
        int vibMod = (int)((float)((curInc * vib) >> 11) * vibDepth);

        // CS-80 timbre: harmonics 1-4, filter-weighted
        int8_t h1a = sineLookup(saxPhase >> 8);
        int8_t h2a = sineLookup((saxPhase * 2) >> 8);
        int8_t h3a = sineLookup((saxPhase * 3) >> 8);
        int8_t h4a = sineLookup((saxPhase * 4) >> 8);

        int8_t h1b = sineLookup(brLeadPhase2 >> 8);
        int8_t h2b = sineLookup((brLeadPhase2 * 2) >> 8);
        int8_t h3b = sineLookup((brLeadPhase2 * 3) >> 8);
        int8_t h4b = sineLookup((brLeadPhase2 * 4) >> 8);

        int h2lev = (int)(filt * 7) >> 3;
        int h3lev = (int)(filt * 5) >> 3;
        int h4lev = (int)(filt * 4) >> 3;

        int voiceA = h1a + ((h2a * h2lev) >> 3) + ((h3a * h3lev) >> 3) + ((h4a * h4lev) >> 4);
        int voiceB = h1b + ((h2b * h2lev) >> 3) + ((h3b * h3lev) >> 3) + ((h4b * h4lev) >> 4);

        int tone = (voiceA + voiceB) >> 1;

        // Light breath
        uint16_t nbit = lfsrBak & 1u;
        lfsrBak >>= 1;
        if (nbit) lfsrBak ^= 0xB400u;
        int8_t breath = (int8_t)(lfsrBak & 0x1F) - 16;
        tone += (int)((float)breath * 0.06f);

        if (tone > 140) tone = 140;
        if (tone < -140) tone = -140;

        int val = (tone * env) >> 9;
        int scaled = (int)((float)val * saxGain);
        int mixed = buf[s] + ((scaled * BR_MIX_LEAD) >> 8);
        if (mixed > 127) mixed = 127;
        if (mixed < -128) mixed = -128;
        buf[s] = (int8_t)mixed;

        saxPhase += curInc + vibMod;
        brLeadPhase2 += curInc2 + vibMod;
    }
}

/// Blade Runner rain: continuous ambient wash, not percussive drops
static void renderBrRain(int8_t* buf, int barSamples) {
    // Consume RNG to keep stream aligned
    (void)rng();
    (void)rng();

    static int8_t rPrev1 = 0, rPrev2 = 0, rPrev3 = 0;
    for (int s = 0; s < barSamples; s++) {
        // Slow volume undulation ~0.5Hz
        uint32_t swellPhase = (uint32_t)(s * 4);
        int8_t swell = sineLookup(swellPhase >> 8);
        int volEnv = 180 + (swell >> 1);
        if (volEnv < 80) volEnv = 80;
        if (volEnv > 220) volEnv = 220;

        // No per-bar envelope — rain is continuous, no gaps at bar boundaries
        int8_t noise = noiseNext();
        int8_t filt = (int8_t)((noise + rPrev1 + rPrev1 + rPrev2 + rPrev3) / 5);
        rPrev3 = rPrev2;
        rPrev2 = rPrev1;
        rPrev1 = filt;

        int val = (filt * volEnv) >> 8;
        int mixed = buf[s] + ((val * BR_MIX_RAIN) >> 8);
        if (mixed > 127) mixed = 127;
        if (mixed < -128) mixed = -128;
        buf[s] = (int8_t)mixed;
    }
}

// ── bar synthesis (incremental — one instrument pass per slice) ─────
static constexpr uint32_t kSynthBudgetMs = 8;

enum class SynthPhase : uint8_t {
    IDLE = 0,
    INIT,
    INST1,
    INST2,
    INST3,
    INST4,
    FADE,
    CLIP,
    DONE
};

static struct {
    bool active;
    int8_t* buf;
    int samples;
    SynthPhase phase;
} synthJob;

static void rampSaxGain() {
    if (saxWanted && saxGain < 1.0f) {
        saxGain += (currentMode == Mode::BLADE_RUNNER) ? 0.15f : 0.25f;
        if (saxGain > 1.0f) saxGain = 1.0f;
    } else if (!saxWanted && saxGain > 0.0f) {
        saxGain -= (currentMode == Mode::BLADE_RUNNER) ? 0.20f : 0.33f;
        if (saxGain < 0.0f) saxGain = 0.0f;
    }
}

static void applyFadeAndClip(int8_t* buf, int samples) {
    if (fadeGain < 1.0f || stopping) {
        for (int s = 0; s < samples; s++) {
            buf[s] = (int8_t)((float)buf[s] * fadeGain);
        }
    }
    for (int s = 0; s < samples; s++) {
        int v = buf[s];
        if (v > 100 || v < -100) {
            int sign = (v >= 0) ? 1 : -1;
            int excess = (v >= 0) ? (v - 100) : (-v - 100);
            int compressed = 100 + (27 * excess) / (excess + 30);
            if (compressed > 127) compressed = 127;
            buf[s] = (int8_t)(sign * compressed);
        }
    }
}

static void beginSynthJob(int8_t* buf, int samples) {
    synthJob.buf = buf;
    synthJob.samples = samples;
    synthJob.phase = SynthPhase::INIT;
    synthJob.active = true;
}

static bool continueSynthJob(uint32_t budgetMs) {
    if (!synthJob.active) return true;

    uint32_t start = millis();
    while (synthJob.active && (millis() - start) < budgetMs) {
        switch (synthJob.phase) {
            case SynthPhase::INIT:
                memset(synthJob.buf, 0, synthJob.samples);
                rampSaxGain();
                synthJob.phase = SynthPhase::INST1;
                break;
            case SynthPhase::INST1:
                if (currentMode == Mode::BLADE_RUNNER) {
                    renderBrBass(synthJob.buf, synthJob.samples);
                } else {
                    renderBass(synthJob.buf, synthJob.samples);
                }
                synthJob.phase = SynthPhase::INST2;
                break;
            case SynthPhase::INST2:
                if (currentMode == Mode::BLADE_RUNNER) {
                    renderBrPad(synthJob.buf, synthJob.samples);
                } else {
                    renderPad(synthJob.buf, synthJob.samples);
                }
                synthJob.phase = SynthPhase::INST3;
                break;
            case SynthPhase::INST3:
                if (currentMode == Mode::BLADE_RUNNER) {
                    renderBrLead(synthJob.buf, synthJob.samples);
                } else {
                    renderBrush(synthJob.buf, synthJob.samples);
                }
                synthJob.phase = SynthPhase::INST4;
                break;
            case SynthPhase::INST4:
                if (currentMode == Mode::BLADE_RUNNER) {
                    renderBrRain(synthJob.buf, synthJob.samples);
                } else {
                    renderSax(synthJob.buf, synthJob.samples);
                    renderRain(synthJob.buf, synthJob.samples);
                }
                synthJob.phase = SynthPhase::FADE;
                break;
            case SynthPhase::FADE:
                applyFadeAndClip(synthJob.buf, synthJob.samples);
                barCount++;
                synthJob.phase = SynthPhase::DONE;
                synthJob.active = false;
                return true;
            default:
                synthJob.active = false;
                return true;
        }
    }
    return !synthJob.active;
}

// ── public API ──────────────────────────────────────────────────────

void init() {
    if (initDone) return;

    buildSineTable();

    // Allocate double buffers in PSRAM
    bufA = (int8_t*)heap_caps_calloc(MAX_BAR_SAMPLES, sizeof(int8_t), MALLOC_CAP_SPIRAM);
    bufB = (int8_t*)heap_caps_calloc(MAX_BAR_SAMPLES, sizeof(int8_t), MALLOC_CAP_SPIRAM);

    if (!bufA || !bufB) {
        HAMLET_LOGLN("[NOIR] PSRAM alloc FAILED");
        return;
    }

    playBuf = bufA;
    fillBuf = bufB;
    playLen = 0;
    fillLen = 0;
    initDone = true;
    HAMLET_LOGF("[NOIR] init OK, bufs=%p/%p (%d bytes each)\n",
                  bufA, bufB, MAX_BAR_SAMPLES);
}

void start() {
    if (!initDone || playing) return;

    playing = true;
    stopping = false;
    fadeGain = 0.05f;  // start with faint signal — NOT zero (avoids silent first bars)
    lastFadeMs = millis();
    barCount = 0;
    rngState = (uint32_t)millis() ^ 0xDEADBEEF;
    saxRngState = (uint32_t)millis() ^ 0x87654321u;
    lfsr = 0xACE1u;

    // Reset oscillator phases
    bassPhase = 0;
    padPhase1 = 0;
    padPhase2 = 0;
    padPhase3 = 0;
    saxPhase  = 0;
    saxGain   = 0.0f;
    saxNoteIdx = 0;
    saxPhraseBar = 0;
    // Blade Runner state reset
    brLeadNoteIdx  = 0;
    brLeadHeldBars = 0;
    brLeadRestBars = 0;
    brLeadNextRest = 0;
    brLeadPhase2   = 0;
    brLeadFilter   = 0.0f;
    brBassNoteIdx  = 1;  // start on F#2
    brBassHeldBars = 0;

    // Incremental first bar — playback starts when synthesis completes
    int samples = samplesPerBar();
    if (samples > MAX_BAR_SAMPLES) samples = MAX_BAR_SAMPLES;

    playLen = 0;
    fillLen = 0;
    playSynthPending = true;
    fillSynthPending = false;
    beginSynthJob(playBuf, samples);

    musicVol = Config::getMusicVolume();
    M5.Speaker.setChannelVolume(SPEAKER_CH, channelVolume());

    beat2HapticDone = false;
    beat4HapticDone = false;
    backbeatHitFlag = false;
    rainDropFlag = false;
    rainScheduled = false;

    HAMLET_LOGF("[NOIR] start: bpm=%d samples/bar=%d (sliced)\n", getBPM(), samples);
}

void stop() {
    if (!playing) return;
    stopping = true;
    HAMLET_LOGLN("[NOIR] stopping (fade out)");
}

void stopImmediate() {
    playing = false;
    stopping = false;
    fadeGain = 0.0f;
    playSynthPending = false;
    fillSynthPending = false;
    synthJob.active = false;
    backbeatHitFlag = false;
    rainDropFlag = false;
    M5.Speaker.stop(SPEAKER_CH);
}

bool update() {
    if (!initDone || !playing) return false;

    // Fade in/out management — time-based, independent of frame rate
    uint32_t now = millis();
    uint32_t dtMs = now - lastFadeMs;
    lastFadeMs = now;
    if (dtMs > 200) dtMs = 200;  // clamp in case of long stalls

    if (stopping) {
        // Fade out over ~4 seconds
        fadeGain -= (float)dtMs / 4000.0f;
        if (fadeGain <= 0.0f) {
            fadeGain = 0.0f;
            playing = false;
            stopping = false;
            playSynthPending = false;
            fillSynthPending = false;
            synthJob.active = false;
            M5.Speaker.stop(SPEAKER_CH);
            HAMLET_LOGLN("[NOIR] stopped");
            return false;
        }
    } else if (fadeGain < 1.0f) {
        // Fade in over ~8 seconds
        fadeGain += (float)dtMs / 8000.0f;
        if (fadeGain > 1.0f) fadeGain = 1.0f;
    }

    // ==[ SLICED SYNTH ]== advance pending bar generation under time budget
    if (playSynthPending) {
        if (continueSynthJob(kSynthBudgetMs)) {
            playLen = synthJob.samples;
            playSynthPending = false;
            M5.Speaker.playRaw(playBuf, playLen, SAMPLE_RATE, false, 1, SPEAKER_CH);
            barStartMs = now;
            barDurationMs = (uint32_t)playLen * 1000UL / SAMPLE_RATE;
            int samples = samplesPerBar();
            if (samples > MAX_BAR_SAMPLES) samples = MAX_BAR_SAMPLES;
            fillSynthPending = true;
            beginSynthJob(fillBuf, samples);
        }
        return true;
    }

    if (fillSynthPending) {
        continueSynthJob(kSynthBudgetMs);
        if (!synthJob.active) {
            fillLen = synthJob.samples;
            fillSynthPending = false;
        }
    }

    // Check if current playback buffer is done
    if (!M5.Speaker.isPlaying(SPEAKER_CH)) {
        if (fillSynthPending || fillLen == 0) {
            return true;
        }
        // Swap buffers: filled bar becomes play bar
        int8_t* tmp = playBuf;
        playBuf = fillBuf;
        fillBuf = tmp;
        playLen = fillLen;
        fillLen = 0;

        // Feed the freshly-filled bar to speaker
        M5.Speaker.playRaw(playBuf, playLen, SAMPLE_RATE, false, 1, SPEAKER_CH);

        // Synthesize next bar into the now-free buffer (incremental)
        int samples = samplesPerBar();
        if (samples > MAX_BAR_SAMPLES) samples = MAX_BAR_SAMPLES;
        fillSynthPending = true;
        beginSynthJob(fillBuf, samples);

        // Reset beat tracking for new bar
        barStartMs = now;
        barDurationMs = (uint32_t)playLen * 1000UL / SAMPLE_RATE;
        beat2HapticDone = false;
        beat4HapticDone = false;

        // Schedule rain haptic: ~15% chance, random position in bar
        // Uses millis() for randomness — must NOT touch rng() here (would shift backing track)
        uint32_t rainRand = now * 2654435761UL;  // Knuth multiplicative hash
        rainScheduled = ((rainRand & 0xFF) < 38);
        if (rainScheduled) {
            uint32_t span = (barDurationMs > 200) ? (barDurationMs - 200) : 1;
            nextRainMs = now + ((rainRand >> 8) % span) + 100;
            rainDropFlag = false;
        }
    }

    // ==[ HAPTIC BEAT TRACKING ]== only in noir jazz mode (no haptics in Blade Runner)
    if (playing && !stopping && barDurationMs > 0 && currentMode == Mode::NOIR_JAZZ) {
        uint32_t elapsed = now - barStartMs;
        uint32_t beatMs = barDurationMs / 4;
        uint32_t swMs = beatMs / 10;  // same swing as audio

        // Beat 2 (backbeat): at 1 beat + swing offset
        uint32_t beat2Ms = beatMs + swMs;
        if (!beat2HapticDone && elapsed >= beat2Ms) {
            beat2HapticDone = true;
            backbeatHitFlag = true;
        }

        // Beat 4 (backbeat): at 3 beats + swing offset
        uint32_t beat4Ms = 3 * beatMs + swMs;
        if (!beat4HapticDone && elapsed >= beat4Ms) {
            beat4HapticDone = true;
            backbeatHitFlag = true;
        }

        // Rain drop: at scheduled random time
        if (rainScheduled && !rainDropFlag && TimeMath::reached(now, nextRainMs)) {
            rainDropFlag = true;
        }
    }

    return true;
}

void setMode(Mode m) {
    currentMode = m;
}

void setTension(float t) {
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    tension = t;
}

void setSaxActive(bool active) {
    saxWanted = active;
}

uint8_t getVolume() {
    return musicVol;
}

void setVolume(uint8_t vol) {
    musicVol = (vol > 10) ? 10 : vol;
    Config::setMusicVolume(musicVol);
    M5.Speaker.setChannelVolume(SPEAKER_CH, channelVolume());
}

bool isPlaying() {
    return playing;
}

bool consumeBackbeatHit() {
    if (backbeatHitFlag) {
        backbeatHitFlag = false;
        return true;
    }
    return false;
}

bool consumeRainDrop() {
    if (rainDropFlag) {
        rainDropFlag = false;
        return true;
    }
    return false;
}

}  // namespace NoirJazz
