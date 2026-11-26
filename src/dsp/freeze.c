/* Copyright (c) 2026 Mathew Taylor — Licensed under GPL-3.0-only */
#include "freeze.h"

#include <math.h>
#include <string.h>

void freeze_init(freeze_t *f, float sr)
{
    memset(f, 0, sizeof(*f));
    f->len = (int)(FREEZE_SEC * sr);
    if (f->len > FREEZE_MAX) f->len = FREEZE_MAX;
    if (f->len < 1) f->len = 1;
    f->xf = (int)(FREEZE_XF_SEC * sr);
    if (f->xf < 1) f->xf = 1;
    f->k_mix = 1.0f - expf(-1.0f / (0.01f * sr));
    f->to_downbeat = -1;
    f->keep_len = (int)(FREEZE_KEEP_SEC * sr) + f->xf;
    if (f->keep_len > f->len) f->keep_len = f->len;
}

void freeze_set_clock(freeze_t *f, int samples_to_downbeat)
{
    f->to_downbeat = samples_to_downbeat < 0 ? -1 : samples_to_downbeat;
}

void freeze_set_target(freeze_t *f, int samples)
{
    if (samples < 0) samples = 0;
    if (samples > f->len - f->xf) samples = f->len - f->xf;
    f->target = samples;
}

int freeze_armed(const freeze_t *f) { return f->state == FREEZE_ARMED; }
int freeze_recording(const freeze_t *f) { return f->state == FREEZE_REC; }

int freeze_looping(const freeze_t *f)
{
    return f->play && f->loop_len > 0 && f->state != FREEZE_REC;
}

float freeze_phase(const freeze_t *f)
{
    if (f->state == FREEZE_REC)
        return (float)f->rec_len / (float)(f->target > 0 ? f->target + f->xf : f->len);
    if (!freeze_looping(f)) return 0.0f;
    return (float)f->pos / (float)(f->loop_len - f->xf);
}

float freeze_loop_seconds(const freeze_t *f, float sr)
{
    if (f->state == FREEZE_REC) return (float)f->rec_len / sr;
    return freeze_looping(f) ? (float)(f->loop_len - f->xf) / sr : 0.0f;
}

int freeze_take_done(freeze_t *f)
{
    int r = f->done;
    f->done = 0;
    return r;
}

static inline float rd(const freeze_t *f, int i)
{
    return (float)f->buf[f->cur][i] * (1.0f / 32767.0f);
}

/* The take in the spare half becomes the loop, and plays. A bar take that ran
 * its full length started on a downbeat and was recorded xf past the bar line,
 * so playback starts xf in: the loop's beat 1 lands on the grid's next
 * downbeat, not 5 ms after it. */
static void accept(freeze_t *f)
{
    f->cur ^= 1;
    f->loop_len = f->rec_len;
    f->pos = (f->target > 0 && f->loop_len == f->target + f->xf) ? f->xf : 0;
    f->state = FREEZE_IDLE;
    f->play = 1;        /* the caller moves the Loop control to On: no restart then (it is already playing) */
    f->done = 1;
}

float freeze_process(freeze_t *f, float in, int rec, int loop, float *mix)
{
    rec = rec ? 1 : 0;
    loop = loop ? 1 : 0;
    /* Act on a change of a control, not on a mismatch with the state: an
     * accepted take turns itself on while Loop still reads Off, and Rec still
     * reads Rec until the caller moves it back. */
    if (loop != f->play_asked) {
        f->play_asked = loop;
        if (loop && !freeze_looping(f)) f->pos = 0;      /* replay the kept take from its start */
        f->play = loop;
    }
    if (rec != f->rec_asked) {
        f->rec_asked = rec;
        if (rec) {
            if (f->state == FREEZE_IDLE) {
                f->rec_len = 0;
                /* Bar mode with the transport running: wait for the downbeat. */
                f->state = (f->target > 0 && f->to_downbeat >= 0) ? FREEZE_ARMED : FREEZE_REC;
            }
        } else if (f->state == FREEZE_ARMED) {
            f->state = FREEZE_IDLE;                      /* cancelled before it started */
        } else if (f->state == FREEZE_REC) {
            if (f->target <= 0 && f->rec_len >= f->keep_len) accept(f);   /* Free: stopped, kept */
            else f->state = FREEZE_IDLE;                 /* an unfinished bar take, or a slip: dropped, the old loop stays */
        }
    }

    if (f->state == FREEZE_ARMED && (f->to_downbeat <= 0 || f->target <= 0)) {
        f->rec_len = 0;                                  /* the downbeat (or the transport stopping, or Free chosen) */
        f->state = FREEZE_REC;
    }
    if (f->to_downbeat > 0) f->to_downbeat--;

    if (f->state == FREEZE_REC) {
        float c = in > 1.0f ? 1.0f : (in < -1.0f ? -1.0f : in);
        f->buf[f->cur ^ 1][f->rec_len++] = (int16_t)lrintf(c * 32767.0f);
        /* Bar mode: the period is the target, plus the 5 ms the wrap crossfade
         * eats. Free: until the buffer is full. Either way, keep it and loop. */
        const int stop = f->target > 0 ? f->target + f->xf : f->len;
        if (f->rec_len >= stop || f->rec_len >= f->len) accept(f);
    }

    const int looping = freeze_looping(f);
    f->mix += ((looping ? 1.0f : 0.0f) - f->mix) * f->k_mix;
    if (f->mix < 1e-4f && !looping) { f->mix = 0.0f; *mix = 0.0f; return 0.0f; }
    if (f->loop_len <= 0) { *mix = f->mix; return 0.0f; }

    /* Play 0 .. P with P = loop_len - xf: over the first xf samples the take's
     * last xf samples fade out as its first fade in, so the wrap is seamless. */
    const int P = f->loop_len - f->xf;
    float y = rd(f, f->pos);
    if (f->pos < f->xf) {
        float t = ((float)f->pos + 0.5f) / (float)f->xf;
        y = sinf(t * 1.5707963f) * y + cosf(t * 1.5707963f) * rd(f, P + f->pos);   /* equal power */
    }
    if (++f->pos >= P) f->pos = 0;

    *mix = f->mix;
    return y;
}
