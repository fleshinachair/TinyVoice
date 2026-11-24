#ifndef TINYVOICE_HM_H
#define TINYVOICE_HM_H

/* Resynth: a harmonic analysis / resynthesis voice processor modelled on the
 * measured behaviour of Baby Audio Humanoid (calibration/FINDINGS.md).
 *
 * Every hop (256 samples) the input is analysed (pitch by MPM, then the
 * harmonics and the residual noise from a pitch-adaptive FFT frame), tuned
 * (Scale / Note / MIDI), reshaped (Robotify, Formant, Transform toward a
 * wavetable, Note Shift and Octave lines), and resynthesised by inverse FFT
 * (overlap-add) plus band-shaped noise. Parameters are in Humanoid's preset
 * units (MIDI notes for frequencies, dB, seconds), so a .baby preset maps one
 * to one. Host-free, no allocation after hm_init. */

#include "fft.h"
#include "hmfx.h"
#include "hm_params.h"

#define HM_HOP      256
#define HM_NS       1024          /* synthesis frame */
#define HM_NFFT     4096          /* analysis FFT (zero-padded) */
#define HM_RING     8192          /* input history (power of two) */
#define HM_OUTRING  8192          /* output overlap-add ring (power of two) */
#define HM_KMAX     256           /* harmonics per line: Humanoid plays exactly 256 (measured, a 4096 saw at 61.7 Hz) */
#define HM_NB       24            /* residual noise bands */
#define HM_NLINE    8             /* 2 targets x (main, note shift, octave down, octave up) */
#define HM_WT_N     2048          /* wavetable cycle after resampling */
#define HM_WT_SRC   4096          /* longest wavetable accepted */
#define HM_GRID     160           /* envelope grid points (log frequency) */
#define HM_DEC_N    768           /* pitch frame, decimated samples */
#define HM_KERN_OS  64            /* synthesis kernel oversampling per bin */
#define HM_KERN_HW  4             /* kernel half width, bins */

#define HM_X_ENUM(id, pid, label, kind, def, lo, hi, step, unit, ...) id,
typedef enum { HM_PARAM_LIST(HM_X_ENUM) HM_NPARAM } hm_param;

enum { HM_MODE_SCALE = 0, HM_MODE_NOTE = 1, HM_MODE_MIDI = 2 };

typedef struct {
    const char *id;      /* Humanoid preset id ("robotify", "pitch_mono", ...) */
    float def, lo, hi;
} hm_param_info;

const hm_param_info *hm_param_get(int id);
int hm_param_find(const char *id);   /* -1 if unknown */

typedef struct {
    float sr;
    fft_t fft;
    float p[HM_NPARAM];
    unsigned mask;                    /* tuning_auto_notes: bit pc set = allowed */

    /* input stage */
    float in_ring[HM_RING];
    float dry_ring[HM_RING];
    unsigned w;                       /* samples written */
    int cnt;                          /* samples since the last hop */
    float gate_env, gate_g;
    int gate_hold;

    /* analysis */
    float re[HM_NFFT], im[HM_NFFT];
    float dec[HM_DEC_N];
    float nsdf[402];
    float f0, midi_med[3];
    int voiced, oct_wait;
    int nh;                           /* harmonics measured this hop */
    float h_db[HM_KMAX + 1];          /* measured level per harmonic, dB (1-based) */
    float h_dev[HM_KMAX + 1];         /* measured frequency deviation, ratio - 1 */
    float env[HM_GRID];               /* smoothed spectral envelope, dB, on the log grid */
    float env_new[HM_GRID];
    int env_valid;
    float nz[HM_NB];                  /* residual noise density per band (linear power) */

    /* tuner */
    float lp_dev;                     /* low-passed deviation (the part that gets corrected) */
    int ref_note;                     /* note the deviation is measured from */
    int midi_held[8], n_held;         /* MIDI mode: held notes, newest last */
    float tgt_hz;                     /* the main line's target note, Hz (0 none) */
    int midi_idle_tune;               /* MIDI mode with no note held: 1 = correct to the scale (Scale mode), 0 = silence (Humanoid) */
    int fold_note[2], fold_off[2];    /* MIDI mode: a pad is moved by whole octaves to within one of the voice; what it was moved by */
    float warble_ph;

    /* wavetable */
    float wt_src[HM_WT_SRC];
    int wt_src_n;                     /* samples in wt_src: the table zero-padded to a power of two */
    int wt_len;                       /* the table's own length before padding (0: a full cycle, no short-table law) */
    float wt_db[HM_KMAX + 1];         /* harmonic level, dB re the strongest */
    float wt_for[10];                 /* shape, stretch, cheb0..6 the table was made for */
    int wt_dirty;

    /* synthesis */
    float kern[2 * HM_KERN_HW * HM_KERN_OS + 1];
    float bh[HM_NS];
    float olaw[2 * HM_HOP];           /* tri / bh over the central 2 hops */
    float sre[HM_NS], sim[HM_NS];
    float nre[2 * HM_HOP], nim[2 * HM_HOP];
    float ph[HM_NLINE][HM_KMAX + 1];  /* partial phase at the frame centre */
    float fr_prev[HM_NLINE][HM_KMAX + 1];
    float out_ring[HM_OUTRING];
    unsigned rng;
    float level_prev;

    hmfx_t fx;                        /* De-ess, Freeze, Widen */
    float bpm;                        /* host tempo: Freeze sync, synced Warble */

    /* per-block constants (hm_begin) */
    float k_ig, k_og, k_dw, k_thr, k_grel, k_gclose, k_gopen;
    /* output leveller (autolevel on): every preset lands at the same loudness whatever the input level */
    int   autolevel;
    float lv_pow, lv_ms, lv_g, lv_lim, k_lv_p, k_lv_up, k_lv_dn, k_lv_g, k_lim_a, k_lim_r;
    int k_lat, k_gate_on, k_ghold;

    int lat_set;                      /* latency setting the analysis lag was made for */
    int la;                           /* analysis lag (samples behind the write head) */
} hm_t;

void  hm_init(hm_t *h, float sample_rate);
void  hm_reset(hm_t *h);
void  hm_set(hm_t *h, int id, float v);
float hm_get(const hm_t *h, int id);
void  hm_set_mask(hm_t *h, unsigned mask12);
/* One table as Humanoid holds it (any length up to HM_WT_SRC): zero-padded to max(256, the next power of two). */
void  hm_set_wavetable(hm_t *h, const float *cycle, int n);
/* The cycle given to hm_set_wavetable was a table of `len` samples already padded (and resampled) by the caller:
 * the module's hm_wt, 512 samples whatever the table was. 0 = a full cycle (the built-in waves). */
void  hm_set_wavetable_len(hm_t *h, int len);
void  hm_set_bpm(hm_t *h, float bpm);
void  hm_note_on(hm_t *h, int note);
void  hm_note_off(hm_t *h, int note);
void  hm_all_notes_off(hm_t *h);
/* Mono in, stereo out (the wet/dry mix included). */
void  hm_process(hm_t *h, const float *in, float *outl, float *outr, int n);
/* The same one sample at a time: hm_begin once per block (after any parameter
 * change), then hm_tick per sample. */
void  hm_begin(hm_t *h);
void  hm_tick(hm_t *h, float in, float *outl, float *outr);
int   hm_latency(const hm_t *h);      /* samples */
float hm_f0(const hm_t *h);           /* last tracked input pitch, Hz (0 unvoiced) */
float hm_target(const hm_t *h);       /* the note the voice is being tuned to, Hz (0 none): for the live display */

#endif
