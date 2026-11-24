/* Native render of the Resynth engine (src/dsp/hm.c) for the calibration A/B.
 *
 *   hm_render <params.txt> <in.f32> <out.f32>
 *
 * in.f32: mono float32 at 44.1 kHz; out.f32: interleaved stereo float32.
 * params.txt lines:  <preset id> <value>   (Humanoid units, as in a .baby)
 *                    mask <12-bit int>     (tuning_auto_notes)
 *                    wavetable <path.f32>  (one cycle, float32)
 *                    note <midi> <on s> <off s>
 * Prints the latency (samples) on stdout. */
#include "hm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { int note; long on, off; } ev_t;

static float *load_f32(const char *path, long *n)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    float *b = malloc((size_t)sz + 4);
    *n = (long)fread(b, 4, (size_t)sz / 4, f);
    fclose(f);
    return b;
}

int main(int argc, char **argv)
{
    if (argc != 4) { fprintf(stderr, "usage: hm_render params.txt in.f32 out.f32\n"); return 2; }
    static hm_t h;
    hm_init(&h, 44100.0f);
    ev_t ev[256]; int nev = 0;
    FILE *pf = fopen(argv[1], "r");
    if (!pf) { fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
    char line[1024];
    while (fgets(line, sizeof line, pf)) {
        char key[256], val[700];
        if (sscanf(line, "%255s %699s", key, val) != 2) continue;
        if (!strcmp(key, "mask")) hm_set_mask(&h, (unsigned)atoi(val));
        else if (!strcmp(key, "wavetable")) {
            long n; float *w = load_f32(val, &n);
            hm_set_wavetable(&h, w, (int)n); free(w);
        } else if (!strcmp(key, "note") && nev < 256) {
            float on, off;
            if (sscanf(line, "note %d %f %f", &ev[nev].note, &on, &off) == 3) {
                ev[nev].on = (long)(on * 44100); ev[nev].off = (long)(off * 44100); nev++;
            }
        } else {
            int id = hm_param_find(key);
            if (id < 0) fprintf(stderr, "unknown param %s\n", key);
            else hm_set(&h, id, (float)atof(val));
        }
    }
    fclose(pf);
    long n; float *in = load_f32(argv[2], &n);
    float *out = malloc((size_t)n * 8);
    float l[128], r[128];
    for (long i = 0; i < n; i += 128) {
        int m = (int)(n - i < 128 ? n - i : 128);
        for (int e = 0; e < nev; e++) {
            if (ev[e].on >= i && ev[e].on < i + m) hm_note_on(&h, ev[e].note);
            if (ev[e].off >= i && ev[e].off < i + m) hm_note_off(&h, ev[e].note);
        }
        hm_process(&h, in + i, l, r, m);
        for (int j = 0; j < m; j++) { out[2 * (i + j)] = l[j]; out[2 * (i + j) + 1] = r[j]; }
    }
    FILE *of = fopen(argv[3], "wb");
    fwrite(out, 8, (size_t)n, of);
    fclose(of);
    printf("%d\n", hm_latency(&h));
    free(in); free(out);
    return 0;
}
