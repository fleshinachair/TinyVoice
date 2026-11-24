/*
 * Demo voice: a synthesized 2-second sung phrase, generated in real time (no
 * PCM blob, nothing heavy at instance creation) and looped.
 *
 * Four 0.5 s syllables "ah - ee - oh - oo" on A3, C4, E4, D4, each sung a
 * little out of tune (+30, -25, +20, -35 cents) so the pitch-correction modes
 * have something audible to fix, with vibrato, gliding between notes, and an
 * "s" onset on syllables 2 and 4 for the vocoder's sibilance detector.
 *
 * Source: band-limited sawtooth (glottal buzz) with spectral tilt plus a little
 * breath noise, through three vowel formant resonators.
 */
#ifndef TINYVOICE_DEMO_H
#define TINYVOICE_DEMO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DEMO_PHRASE_SEC 2.0f
#define DEMO_SYLLABLES  4
#define DEMO_CHORDS     4

typedef struct {
    float sr;
    double t;               /* seconds into the phrase */
    float ph;               /* oscillator phase */
    float tilt;             /* spectral tilt lowpass */
    float res[3][2];        /* formant resonator states */
    unsigned rng;
    float pn, pn2;          /* noise high-pass memory (the "s") */
    float norm_env, k_norm; /* level normaliser: mean |voice| over ~20 ms */
} demo_t;

void  demo_init(demo_t *dm, float sample_rate);
void  demo_reset(demo_t *dm);
float demo_process(demo_t *dm);                 /* next sample, peak ~0.35 */

/* A recorded phrase for the Demo, when the module folder has one (demo_voice.raw: signed 16-bit little-endian, mono,
 * 44100 Hz, already at the demo level; see scripts/make_demo_voice.py and CREDITS.md). Loaded off the audio thread
 * (allocates, reads a file); played by demo_pcm_next in place of the synthetic voice. */
typedef struct demo_pcm { int16_t *pcm; int n; } demo_pcm_t;
demo_pcm_t *demo_pcm_load(const char *path);    /* NULL: no such file, or it is not a usable clip (0.5 .. 30 s) */
void  demo_pcm_free(demo_pcm_t *d);
/* The next output sample at `*pos` (a position in source samples, advanced by `step`, wrapped; linear interpolation). */
float demo_pcm_next(const demo_pcm_t *d, double *pos, double step);

/* Chord progression for the polyphonic modes, one chord per bar:
 * Am, F, C, G (4 notes each). */
void  demo_chord(int bar, int out[4]);

/* The pitch (fractional MIDI) the demo voice is singing at phrase time t, for tests. */
float demo_pitch_at(float t);

#ifdef __cplusplus
}
#endif
#endif
