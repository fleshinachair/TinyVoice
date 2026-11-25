#define _POSIX_C_SOURCE 200809L
#include "loopsave.h"

#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifdef _WIN32
#include <direct.h>
#define LS_MKDIR(p) _mkdir(p)
#else
#define LS_MKDIR(p) mkdir((p), 0755)
#endif

void loopsave_init(loopsave_t *ls, int sample_rate)
{
    memset(ls, 0, sizeof *ls);
    atomic_init(&ls->state, LS_IDLE);
    atomic_init(&ls->rev, 0);
    ls->sample_rate = sample_rate > 0 ? sample_rate : 48000;
    snprintf(ls->dir, sizeof ls->dir, "%s", "/data/UserData/UserLibrary/Samples/TinyVoice");
}

void loopsave_set_dir(loopsave_t *ls, const char *dir)
{
    if (dir && *dir) snprintf(ls->dir, sizeof ls->dir, "%s", dir);
}

int loopsave_state(const loopsave_t *ls) { return atomic_load_explicit(&((loopsave_t *)ls)->state, memory_order_acquire); }

static void finish(loopsave_t *ls, int ok, int why)
{
    ls->ok = ok;
    ls->why = why;
    atomic_fetch_add_explicit(&ls->rev, 1, memory_order_release);
    atomic_store_explicit(&ls->state, LS_DONE, memory_order_release);
}

void loopsave_arm(loopsave_t *ls, int no_loop)
{
    if (loopsave_state(ls) != LS_IDLE) return;                   /* a capture or a write is going: this one is ignored */
    if (no_loop) { finish(ls, 0, LS_WHY_NO_LOOP); return; }
    atomic_store_explicit(&ls->state, LS_ARMED, memory_order_release);
}

void loopsave_cancel(loopsave_t *ls)
{
    const int s = loopsave_state(ls);
    if (s == LS_ARMED || s == LS_CAPTURING) atomic_store_explicit(&ls->state, LS_IDLE, memory_order_release);
}

void loopsave_begin(loopsave_t *ls, int period, int loop_len, int bars, int bpm10)
{
    if (loopsave_state(ls) != LS_ARMED) return;
    if (period < 1) period = 1;
    if (period > FREEZE_MAX) period = FREEZE_MAX;
    ls->want = period;
    ls->n = 0;
    ls->loop_len = loop_len;
    ls->bars = bars;
    ls->bpm10 = bpm10;
    atomic_store_explicit(&ls->state, LS_CAPTURING, memory_order_release);
}

static inline int16_t to16(float x)
{
    if (x > 1.0f) x = 1.0f;
    if (x < -1.0f) x = -1.0f;
    return (int16_t)lrintf(x * 32767.0f);
}

void loopsave_feed(loopsave_t *ls, float l, float r)
{
    if (loopsave_state(ls) != LS_CAPTURING) return;
    ls->buf[0][ls->n] = to16(l);
    ls->buf[1][ls->n] = to16(r);
    if (++ls->n >= ls->want) atomic_store_explicit(&ls->state, LS_READY, memory_order_release);   /* the worker's now */
}

void loopsave_abort(loopsave_t *ls, int why)
{
    const int s = loopsave_state(ls);
    if (s == LS_ARMED || s == LS_CAPTURING) finish(ls, 0, why);
}

int loopsave_result(loopsave_t *ls)
{
    if (loopsave_state(ls) != LS_DONE) return 0;
    const int ok = ls->ok;
    atomic_store_explicit(&ls->state, LS_IDLE, memory_order_release);
    return ok ? 1 : 2;
}

/* ---- the file ---- */

static void put16(unsigned char *p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void put32(unsigned char *p, unsigned long v) { for (int i = 0; i < 4; i++) p[i] = (unsigned char)(v >> (8 * i)); }

static int mkdir_p(const char *path)
{
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (LS_MKDIR(tmp) != 0 && errno != EEXIST) return -1;
        *p = '/';
    }
    return LS_MKDIR(tmp) != 0 && errno != EEXIST ? -1 : 0;
}

/* The next number: one past the highest "TinyVoice Loop NNN" already in the folder. */
static int next_number(const char *dir)
{
    int top = 0;
    DIR *d = opendir(dir);
    struct dirent *de;
    while (d && (de = readdir(d)) != NULL) {
        int n = 0;
        if (sscanf(de->d_name, "TinyVoice Loop %d", &n) == 1 && n > top) top = n;
    }
    if (d) closedir(d);
    return top + 1;
}

/* A 44-byte header, then the frames: stereo, 16-bit, little-endian. */
static int write_wav(FILE *f, const loopsave_t *ls)
{
    const unsigned long bytes = (unsigned long)ls->n * 4ul;
    unsigned char h[44];
    memcpy(h, "RIFF", 4);          put32(h + 4, 36ul + bytes);
    memcpy(h + 8, "WAVEfmt ", 8);  put32(h + 16, 16);
    put16(h + 20, 1);              put16(h + 22, 2);                         /* PCM, 2 channels */
    put32(h + 24, (unsigned long)ls->sample_rate);
    put32(h + 28, (unsigned long)ls->sample_rate * 4ul);                     /* bytes per second */
    put16(h + 32, 4);              put16(h + 34, 16);                        /* block align, bits */
    memcpy(h + 36, "data", 4);     put32(h + 40, bytes);
    if (fwrite(h, 1, sizeof h, f) != sizeof h) return -1;
    unsigned char blk[4096];
    for (int i = 0; i < ls->n;) {
        int k = 0;
        while (i < ls->n && k + 4 <= (int)sizeof blk) {
            put16(blk + k, (unsigned)(uint16_t)ls->buf[0][i]);
            put16(blk + k + 2, (unsigned)(uint16_t)ls->buf[1][i]);
            k += 4;
            i++;
        }
        if (fwrite(blk, 1, (size_t)k, f) != (size_t)k) return -1;
    }
    return 0;
}

int loopsave_pump(loopsave_t *ls)
{
    if (loopsave_state(ls) != LS_READY) return 0;
    atomic_store_explicit(&ls->state, LS_WRITING, memory_order_release);

    char path[800], tmp[820], label[64];
    if (ls->bars > 0 && ls->bpm10 > 0) {
        snprintf(label, sizeof label, "%d bar%s %g bpm", ls->bars, ls->bars == 1 ? "" : "s", (double)ls->bpm10 / 10.0);
    } else {
        snprintf(label, sizeof label, "%.1f s", (double)ls->n / (double)ls->sample_rate);
    }
    if (mkdir_p(ls->dir) != 0) {
        snprintf(ls->name, sizeof ls->name, "%s", strerror(errno));
        finish(ls, 0, LS_WHY_WRITE);
        return 1;
    }
    snprintf(ls->name, sizeof ls->name, "TinyVoice Loop %03d - %s.wav", next_number(ls->dir), label);
    snprintf(path, sizeof path, "%s/%s", ls->dir, ls->name);
    snprintf(tmp, sizeof tmp, "%s.part", path);              /* written whole, then renamed: a sampler never sees half a file */
    FILE *f = fopen(tmp, "wb");
    int bad = f == NULL;
    if (f) {
        bad = write_wav(f, ls) != 0;
        if (fclose(f) != 0) bad = 1;
    }
#ifdef _WIN32
    if (!bad) remove(path);                                  /* (rename does not replace there) */
#endif
    if (bad || rename(tmp, path) != 0) {
        snprintf(ls->name, sizeof ls->name, "%s", strerror(errno));
        remove(tmp);
        finish(ls, 0, LS_WHY_WRITE);
        return 1;
    }
    finish(ls, 1, LS_WHY_NONE);
    return 1;
}

void loopsave_status(const loopsave_t *ls, char *buf, int buf_len)
{
    const int rev = atomic_load_explicit(&((loopsave_t *)ls)->rev, memory_order_acquire);
    const int st = loopsave_state(ls);
    char msg[LOOPSAVE_NAME_MAX + 64];
    int ok = 0;
    if (st == LS_ARMED) snprintf(msg, sizeof msg, "waiting for the loop to start");
    else if (st == LS_CAPTURING || st == LS_READY || st == LS_WRITING) snprintf(msg, sizeof msg, "saving the loop");
    else if (rev == 0) msg[0] = '\0';
    else if (ls->ok) { ok = 1; snprintf(msg, sizeof msg, "saved %s", ls->name); }
    else if (ls->why == LS_WHY_NO_LOOP) snprintf(msg, sizeof msg, "nothing to save: record a loop and turn Loop On");
    else if (ls->why == LS_WHY_CHANGED) snprintf(msg, sizeof msg, "the loop changed or stopped: nothing saved");
    else snprintf(msg, sizeof msg, "could not write the file (%s)", ls->name);
    for (char *p = msg; *p; p++) if (*p == '"' || *p == '\\' || (unsigned char)*p < 32) *p = ' ';
    snprintf(buf, (size_t)buf_len, "{\"rev\":%d,\"ok\":%d,\"msg\":\"%s\"}", rev, ok, msg);
}
