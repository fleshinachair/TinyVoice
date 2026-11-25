/*
 * Freeze: a looper on the input, up to 16 seconds, worked by two separate
 * controls so a loop is never recorded over by accident (2026-11-25):
 *
 *   Rec  (Off / Rec)   turning it to Rec starts a take; it never plays anything
 *   Loop (Off / On)    plays the kept take into the engine instead of the live
 *                      input (On) or goes back to live (Off); it never records
 *
 * The old single knob went Off / Rec / Loop, so turning between Off and Loop
 * passed through Rec and wiped the loop at once.
 *
 * Takes go into the SPARE half of a double buffer: the kept loop is untouched
 * (and keeps playing if Loop is On) until the new take is accepted.
 *
 *   bar mode (freeze_set_target > 0, Length 1 / 2 / 4 Bars at the host tempo):
 *     Rec with the transport running arms and the take starts on the exact
 *     sample of the next downbeat (transport stopped: at once). The take
 *     ends by itself at exactly N bars (+ the 5 ms the wrap crossfade eats, so
 *     the loop period is exactly N bars and stays in time), is accepted and
 *     plays: freeze_take_done() says so once, and the caller moves Rec back to
 *     Off and Loop to On. Rec turned back to Off before then CANCELS (armed or
 *     recording): the old loop stays as it was.
 *   Free:
 *     Rec records at once; Rec back to Off stops it, and a take of at least
 *     FREEZE_KEEP_SEC is accepted and plays (freeze_take_done, as above); a
 *     shorter one was a slip and is dropped (the old loop stays). A take that
 *     fills the buffer is accepted by itself.
 *
 * While recording you hear the live input (to sing over); while armed the old
 * loop goes on playing if Loop is On. A full bar take started on a downbeat
 * and was recorded xf past the bar line, so its playback starts xf in: the
 * loop's beat 1 stays on the grid.
 *
 * The loop wraps with a 5 ms equal-power crossfade (the take's last 5 ms fade
 * into its first), and live <-> loop switches fade over 10 ms: no clicks.
 * Whatever the engine does on top (pads, carriers, knobs) runs on the loop
 * exactly as on a live voice.
 *
 * Two fixed-size 16-bit buffers (sized for 48 kHz, ~1.5 MB each); embedded in
 * vt_t, no allocation after init.
 */
#ifndef TINYVOICE_FREEZE_H
#define TINYVOICE_FREEZE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FREEZE_SEC 16.0f
#define FREEZE_MAX (16 * 48000)
#define FREEZE_XF_SEC 0.005f         /* loop-wrap crossfade */
#define FREEZE_KEEP_SEC 1.0f         /* a Free take stopped before this is a slip: dropped, the old loop kept */

enum { FREEZE_IDLE = 0, FREEZE_REC = 2, FREEZE_ARMED = 3 };   /* what the recorder is doing (the scope feed's codes) */

typedef struct {
    int16_t buf[2][FREEZE_MAX];
    int   cur;          /* the half holding the kept take; takes record into the other */
    int   len;          /* capacity: FREEZE_SEC at this sample rate */
    int   state;        /* FREEZE_IDLE / _REC / _ARMED */
    int   rec_asked;    /* the Rec control's last value (transitions fire on its changes) */
    int   play_asked;   /* the Loop control's last value */
    int   play;         /* Loop is On */
    int   rec_len;      /* samples in the take being recorded */
    int   keep_len;     /* a Free take needs this many samples to be kept */
    int   loop_len;     /* kept take length (0 = nothing to loop) */
    int   pos;          /* playback position, 0 .. loop_len - xf */
    int   xf;           /* wrap crossfade length */
    int   done;         /* a take was accepted (freeze_take_done) */
    int   target;       /* bar mode: loop period in samples, 0 = free */
    int   to_downbeat;  /* samples until the next downbeat, -1 = transport stopped */
    float mix, k_mix;   /* 0 = live, 1 = loop, and its slew */
} freeze_t;

void  freeze_init(freeze_t *f, float sample_rate);

/* One sample. rec: the Rec control (0 Off, 1 Rec); loop: the Loop control
 * (0 Off, 1 On). Records `in` while recording; returns the loop sample and sets
 * *mix (0 = use the live input, 1 = use the returned loop, in between = fading). */
float freeze_process(freeze_t *f, float in, int rec, int loop, float *mix);

/* Bar mode: the loop period in samples (0 = free). Clamped so the take, 5 ms
 * crossfade included, fits the buffer. Set it every block. */
void  freeze_set_target(freeze_t *f, int samples);

/* Transport: samples from the start of this block to the next downbeat (0 =
 * the block starts on one), or -1 when the transport is stopped / unknown.
 * Set it every block, before processing it. */
void  freeze_set_clock(freeze_t *f, int samples_to_downbeat);

/* After a block: a take was accepted and now plays. Returns 1 once (so the
 * caller can move Rec back to Off and Loop to On), then 0. */
int   freeze_take_done(freeze_t *f);

/* For the display: while looping, the playhead 0..1 and the loop length in
 * seconds; while recording, how far through the take 0..1 (Free: the buffer)
 * and the seconds so far; otherwise 0. */
float freeze_phase(const freeze_t *f);
float freeze_loop_seconds(const freeze_t *f, float sample_rate);
int   freeze_looping(const freeze_t *f);       /* a take is playing */
int   freeze_recording(const freeze_t *f);     /* a take is being recorded */
int   freeze_armed(const freeze_t *f);         /* Rec asked, waiting for the downbeat */

#ifdef __cplusplus
}
#endif
#endif
