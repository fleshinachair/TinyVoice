/* Copyright (c) 2026 Mathew Taylor — Licensed under GPL-3.0-only */
/*
 * Looper Save: writes the processed loop to a WAV file you can load into a sampler or a clip.
 *
 * Turning the Looper's Save knob arms a capture. The audio thread waits for the loop to come round to its start, then keeps
 * what TinyVoice itself outputs (the engine, its filter, the dry/wet mix: everything you hear) for exactly one loop period. In
 * a bar take that period is N bars of the host tempo to the sample, so the file loops in time and drops straight onto a bar
 * grid. A worker thread (plugin.c, the same one that runs the banks) then writes the file:
 *
 *   <Samples>/TinyVoice/TinyVoice Loop 003 - 2 bars 120 bpm.wav     (16-bit stereo, the host's sample rate)
 *   <Samples>/TinyVoice/TinyVoice Loop 004 - 7.3 s.wav               (a Free take: no tempo in the name)
 *
 * Nothing here allocates or does I/O on the audio thread; the buffer is part of the instance (about 3 MB). The audio side
 * only moves a state and copies samples; loopsave_pump (worker) does the file work. Whatever the effects were doing during
 * that one period is what is kept: change a knob while it captures and the change is in the file. Effect tails and the
 * engine's latency are not wrapped back to the start, so a long reverb-like tail is cut at the end.
 */
#ifndef TINYVOICE_LOOPSAVE_H
#define TINYVOICE_LOOPSAVE_H

#include "freeze.h"

#include <stdatomic.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LOOPSAVE_NAME_MAX 160

typedef struct loopsave {
    int16_t buf[2][FREEZE_MAX];         /* the capture, left and right */
    atomic_int state;                   /* LS_* below */
    atomic_int rev;                     /* counts the finished attempts (the UI refreshes when it changes) */
    int   sample_rate;
    int   want;                         /* samples to capture (the loop period) */
    int   n;                            /* samples captured so far */
    int   loop_len;                     /* the take's length when the capture began (it must not change) */
    int   bars;                         /* the period in bars (0 = a Free take) */
    int   bpm10;                        /* the host tempo x 10 */
    int   ok;                           /* the last attempt wrote a file */
    int   why;                          /* the last failure: LS_WHY_* */
    char  name[LOOPSAVE_NAME_MAX];      /* the file written (without the folder), or the system's error text */
    char  dir[512];                     /* where files go (made when first needed) */
} loopsave_t;

enum { LS_IDLE = 0, LS_ARMED = 1, LS_CAPTURING = 2, LS_READY = 3, LS_WRITING = 4, LS_DONE = 5 };
enum { LS_WHY_NONE = 0, LS_WHY_NO_LOOP = 1, LS_WHY_CHANGED = 2, LS_WHY_WRITE = 3 };

void loopsave_init(loopsave_t *ls, int sample_rate);
void loopsave_set_dir(loopsave_t *ls, const char *dir);

/* ---- audio thread ---- */

/* Save was turned on: wait for the loop's start. Ignored while a capture or a write is going. no_loop: there is nothing playing, so it fails at once. */
void loopsave_arm(loopsave_t *ls, int no_loop);
/* Save was turned off before the capture began or finished: stop it (a file already being written still finishes). */
void loopsave_cancel(loopsave_t *ls);
/* The state (LS_*), for the audio thread to look at. */
int  loopsave_state(const loopsave_t *ls);
/* The capture begins on this very sample: period = the loop's length in samples; bars and bpm10 only name the file. */
void loopsave_begin(loopsave_t *ls, int period, int loop_len, int bars, int bpm10);
/* One output sample while capturing (call only after loopsave_begin, until it stops being CAPTURING). */
void loopsave_feed(loopsave_t *ls, float l, float r);
/* The loop stopped or changed under a capture: give up. */
void loopsave_abort(loopsave_t *ls, int why);
/* After a block: 0 nothing new, 1 a file was written, 2 it failed. Returns each result once and goes back to idle. */
int  loopsave_result(loopsave_t *ls);

/* ---- worker thread ---- */

/* Writes a finished capture. Returns 1 when it wrote one, else 0. */
int  loopsave_pump(loopsave_t *ls);

/* {"rev":N,"ok":1,"msg":"..."} for the last attempt. */
void loopsave_status(const loopsave_t *ls, char *buf, int buf_len);

#ifdef __cplusplus
}
#endif
#endif
