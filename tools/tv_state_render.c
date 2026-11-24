/* Render through the whole TinyVoice module with a state loaded (e.g. an imported Humanoid preset).
 *   tv_state_render <state.json|preset.json> <in.f32> <out.f32>
 * A Schwung preset file's "state" string is unwrapped; a plain state object is used as is.
 * in: mono float32 44.1 kHz; out: interleaved stereo float32. */
#include "dsp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *slurp(const char *p, long *n)
{
    FILE *f = fopen(p, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", p); exit(1); }
    fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)*n + 1);
    *n = (long)fread(b, 1, (size_t)*n, f); b[*n] = 0; fclose(f);
    return b;
}

int main(int argc, char **argv)
{
    if (argc != 4) { fprintf(stderr, "usage: tv_state_render state.json in.f32 out.f32\n"); return 2; }
    long n;
    char *js = slurp(argv[1], &n);
    char *state = js;
    char *k = strstr(js, "\"state\"");
    if (k) {                                    /* a preset file: unescape its "state" string */
        char *q = strchr(k + 7, '"');
        state = malloc((size_t)n + 1);
        int o = 0;
        for (q++; *q && *q != '"'; q++) { if (*q == 92 && q[1]) q++;   /* backslash */ state[o++] = *q; }
        state[o] = 0;
    }
    vt_t *vt = dsp_init(44100);
    if (dsp_set_param(vt, "state", state) != 0) { fprintf(stderr, "state refused\n"); return 1; }
    long bytes;
    float *in = (float *)slurp(argv[2], &bytes);
    long frames = bytes / 4;
    float *out = malloc((size_t)frames * 8);
    float blk[256];
    for (long i = 0; i < frames; i += 128) {
        int m = (int)(frames - i < 128 ? frames - i : 128);
        for (int j = 0; j < m; j++) blk[2 * j] = blk[2 * j + 1] = in[i + j];
        dsp_process_block_f(vt, blk, m);
        memcpy(out + 2 * i, blk, (size_t)m * 8);
    }
    FILE *of = fopen(argv[3], "wb"); fwrite(out, 8, (size_t)frames, of); fclose(of);
    dsp_cleanup(vt);
    return 0;
}
