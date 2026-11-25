/*
 * Mangler: TinyVoice's fourth engine, modelled on Devious Machines' PitchMonster
 * (laws measured in TinyVoice-lab pitchmonster/FINDINGS.md). A multi-voice pitch
 * shifter with three cores sharing one input ring:
 *
 *   Granular  per voice, a two-tap rotating delay-line shifter. Each tap restarts
 *             at Grain Rate Hz whatever the shift (sweep |1 - r| / rate), Hann
 *             windows half a period apart; Jitter gives every grain its own length.
 *             Formants move with the pitch.
 *   Formant   per voice, TD-PSOLA grains (2 periods) on pitch marks, emitted at
 *             period / r and read at the formant ratio:
 *             formant = Formant Shift (Fix On) or Formant Shift + the shift (Fix Off).
 *             Unvoiced input passes at ratio 1.
 *   Vocode    Formant's core with the output pitch an absolute note: C4 + Pitch +
 *             the note offset, whatever is sung (a monotone / robot voice).
 *
 * Voices: the note offsets (Normal: 0; Pads: the held pads - C4, the newest in
 * Mono, up to 4 in Poly, silent with none; Chord: the slot's notes - C4) times
 * Unison, at most MG_MAX_VOICES (unison per note reduced to fit). Pitch Spread
 * puts a note's unison voices evenly over [-S/2, S/2] (Jitter: a random offset
 * per grain), Time Spread delays them evenly over 0..T (Jitter: per grain);
 * each voice x 1/sqrt(N); Wide pans them across the field. Then a 2nd-order
 * high-pass at f 2^(-bw/2) and low-pass at f 2^(bw/2) (Filter), PitchMonster's
 * Mix (dry = min(1, 2(1-w)), wet = min(1, 2w)) and Volume.
 *
 * Latency is ours, not PitchMonster's 30 ms: a few samples (Granular) to about
 * half a period plus the grain's reach (Formant / Vocode), for live singing.
 *
 * Fixed-size state, no allocation; the caller supplies the tracked period
 * (the shared tracker, cher) every sample.
 */
#ifndef TINYVOICE_MANGLE_H
#define TINYVOICE_MANGLE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MG_RING        131072            /* input history, samples (power of two): 2.97 s at 44.1 kHz (1 s of Time
                                            Spread plus a slow grain's sweep: |1 - r| / rate, 0.94 s at -48 st, 1 Hz) */
#define MG_HOP         256               /* period history resolution */
#define MG_MAX_VOICES  16
#define MG_MAX_GRAINS  8                 /* per PSOLA voice */
#define MG_MAX_NOTES   8                 /* a chord slot */
#define MG_MAX_TIME_MS 1000.0f

enum { MG_GRANULAR = 0, MG_FORMANT, MG_VOCODE };
enum { MG_NOTES_NORMAL = 0, MG_NOTES_PADS, MG_NOTES_CHORD };

/* The controls, in PitchMonster's own units. */
typedef struct {
    int   engine;          /* MG_GRANULAR / _FORMANT / _VOCODE */
    float pitch;           /* semitones, -48..+24 */
    int   snap;            /* round Pitch to a semitone */
    int   unison;          /* 1..8 voices per note */
    float detune;          /* Pitch Spread, total semitones 0..12 */
    int   detune_jitter;   /* 0 = Spread (fixed, even), 1 = Jitter (per grain) */
    float time_ms;         /* Time Spread 0..1000 */
    int   time_jitter;     /* 0 = Spread, 1 = Jitter */
    float grain_hz;        /* Granular restart rate 1..100 */
    int   grain_jitter;    /* random grain lengths */
    int   wide;            /* pan the voices */
    float formant;         /* semitones -24..+24 (Formant / Vocode) */
    int   fix;             /* Formant Fix */
    int   filter_on;
    float filter_hz;       /* 20..20000 */
    float filter_oct;      /* bandwidth, octaves 0.1..10 */
    float mix;             /* 0..1, PitchMonster's law */
    float volume_db;
    int   note_mode;       /* MG_NOTES_* */
    int   mono;            /* Pads: the newest pad only */
} mg_params;

typedef struct {
    double start;          /* absolute input index of the grain's first sample */
    float  rate, t, len, gain;
    int    active;
} mg_grain;

typedef struct {
    /* the voice's place: note + spread offsets (st), delay (samples), pan gains */
    float st, delay, pl, pr;
    float off;             /* a small fixed extra delay per unison voice (decorrelates voices at one pitch) */
    float amp, amp_goal;   /* fades a voice in / out when the voice count changes */
    /* Granular: two taps */
    float ph[2], inc[2], r[2], w[2], base[2];
    /* PSOLA */
    mg_grain g[MG_MAX_GRAINS];
    double mark;
    float  until_next, lat;
    int    started, have_mark;
    uint32_t rng;
} mg_voice;

typedef struct {
    float sr;
    float ring[MG_RING];
    double n;                      /* samples written */
    float t0h[MG_RING / MG_HOP];   /* tracked period per hop (0 = unvoiced) */
    mg_params p;
    int   notes[MG_MAX_NOTES], n_notes;   /* the note offsets (st from C4) in play */
    int   chord[8][MG_MAX_NOTES], chord_n[8];
    int   slot;                    /* chord slot 0..7 */
    int   pads[4], n_pads;         /* held pads, newest last */
    int   n_voices, n_unison;          /* voices sounding; unison voices per note */
    mg_voice v[MG_MAX_VOICES];
    float vgain;                   /* 1/sqrt(N), smoothed */
    float k_amp;
    /* filter (TPT SVFs, per channel: high-pass then low-pass) */
    float hp_g, hp_k, lp_g, lp_k;
    float hs[2][2], ls[2][2];
    float dry_lat;                 /* the dry path's delay (samples, smoothed) to line up with the wet */
    float k_lat;
    uint32_t rng;
} mg_t;

void  mg_init(mg_t *m, float sample_rate);
void  mg_set(mg_t *m, const mg_params *p);          /* once a block (or whenever a control changes) */
void  mg_set_chord(mg_t *m, int slot, const int *notes, int n);   /* a chord slot's MIDI notes */
void  mg_select_chord(mg_t *m, int slot);
void  mg_default_chords(mg_t *m);                   /* eight plain chords in the slots (C, Dm, Em, F, G, Am, G7, Cmaj7) */
void  mg_set_pads(mg_t *m, const int *notes, int n); /* held pads, oldest first */
/* One sample: in (mono), t0 = the tracked input period in samples now (0 unvoiced). */
void  mg_tick(mg_t *m, float in, float t0, float *out_l, float *out_r);
float mg_latency(const mg_t *m);                     /* the wet path's delay now, samples */
int   mg_voices(const mg_t *m);                      /* voices sounding */

/* The chord text saved in state: "60,64,67;62,65,69,72;..." (8 slots, up to 8 notes each).
 * mg_chords_parse returns 0 on success; mg_chords_format writes at most len bytes. */
int   mg_chords_parse(mg_t *m, const char *s);
int   mg_chords_format(const mg_t *m, char *buf, int len);

#ifdef __cplusplus
}
#endif
#endif
