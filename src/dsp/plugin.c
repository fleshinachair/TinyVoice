/*
 * Schwung audio_fx v2 adapter: maps audio_fx_api_v2_t onto the engine in dsp.h.
 * Headers come from src/host/ (run scripts/fetch_host_headers.sh).
 */
#include "audio_fx_api_v2.h"
#include "dsp.h"
#include "rsbank.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const host_api_v1_t *g_host;

/* What the host holds: the engine, and the thread that reads Resynth's presets into it. */
typedef struct {
    vt_t *vt;
    pthread_t loader;
    int loader_on;
    char dir[512];          /* Imported bank: Schwung's preset folder for this module */
    char dflt[512];         /* Default bank: the presets shipped in the module folder */
} vt_inst;

#define VT(inst) (((vt_inst *)(inst))->vt)

/* Resynth's preset bank: the folder read (files, allocation) here, off the audio thread, then handed over. */
static void *bank_loader(void *arg)
{
    vt_inst *in = (vt_inst *)arg;
    rsbank_t *b = rsbank_load(in->dflt, in->dir);
    if (b && dsp_set_rs_bank(in->vt, b) != 0) rsbank_free(b);
    return NULL;
}

/* Schwung's preset folder for this module: <root>/presets/tinyvoice beside <root>/modules/audio_fx/tinyvoice. */
static void preset_dir(const char *module_dir, char *out, size_t n)
{
    const char *m = module_dir ? strstr(module_dir, "/modules/") : NULL;
    if (m) snprintf(out, n, "%.*s/presets/tinyvoice", (int)(m - module_dir), module_dir);
    else snprintf(out, n, "%s", "/data/UserData/schwung/presets/tinyvoice");
}

static void *vt_create_instance(const char *module_dir, const char *config_json)
{
    int sr = (g_host && g_host->sample_rate > 0) ? g_host->sample_rate : VT_SAMPLE_RATE;
    vt_inst *in = (vt_inst *)calloc(1, sizeof *in);
    if (!in) return NULL;
    in->vt = dsp_init(sr);
    if (!in->vt) { free(in); return NULL; }
    /* Host-supplied defaults use the same flat {"key": value} shape as "state".
     * Unknown or nested members are ignored. */
    if (config_json) (void)dsp_set_param(in->vt, "state", config_json);
    preset_dir(module_dir, in->dir, sizeof in->dir);
    snprintf(in->dflt, sizeof in->dflt, "%s/presets", module_dir ? module_dir : "");
    in->loader_on = pthread_create(&in->loader, NULL, bank_loader, in) == 0;
    return in;
}

static void vt_destroy_instance(void *inst)
{
    vt_inst *in = (vt_inst *)inst;
    if (!in) return;
    if (in->loader_on) pthread_join(in->loader, NULL);   /* it is done in well under a second */
    dsp_cleanup(in->vt);
    free(in);
}

/* The slot's receive channel (0-15; -1 All, -2 no slot: both omni). Asked
 * every block and before every message, so a channel changed in the slot
 * settings applies at once. NULL on hosts without slot context: omni. */
static void vt_sync_channel(void *inst)
{
    if (g_host && g_host->slot_recv_channel) dsp_set_midi_channel(VT(inst), g_host->slot_recv_channel(inst));
}

static void vt_process_block(void *inst, int16_t *audio_inout, int frames)
{
    vt_sync_channel(inst);
    if (g_host && g_host->get_bpm) dsp_set_bpm(VT(inst), g_host->get_bpm());   /* Demo's and Freeze's bar length */
    /* Transport position for Freeze's downbeat-synced takes; NULL on older hosts. */
    dsp_set_transport(VT(inst), g_host && g_host->get_beat_position ? g_host->get_beat_position() : -1.0);
    dsp_process_block(VT(inst), audio_inout, frames);
}

static void vt_set_param(void *inst, const char *key, const char *val)
{
    /* "state" (full snapshot restore for presets/sets) is handled by the engine. */
    (void)dsp_set_param(VT(inst), key, val);
}

static int vt_get_param(void *inst, const char *key, char *buf, int buf_len)
{
    /* chain_params / ui_hierarchy come from module.json; "state" is the full
     * 16-param snapshot used for autosave and presets. */
    return dsp_get_param(VT(inst), key, buf, buf_len);
}

static void vt_on_midi(void *inst, const uint8_t *msg, int len, int source)
{
    vt_sync_channel(inst);
    dsp_set_midi_source(VT(inst), source);
    dsp_handle_midi(VT(inst), msg, len);
}

/* The chain host decides whether to send an FX any MIDI by looking this symbol
 * up with dlsym() (see ducker/capicola/magneto, which all export it); having
 * on_midi in the struct alone is not enough, and pad notes never arrive. */
__attribute__((visibility("default")))
void move_audio_fx_on_midi(void *inst, const uint8_t *msg, int len, int source)
{
    vt_on_midi(inst, msg, len, source);
}

static audio_fx_api_v2_t g_api = {
    .api_version     = AUDIO_FX_API_VERSION_2,
    .create_instance = vt_create_instance,
    .destroy_instance = vt_destroy_instance,
    .process_block   = vt_process_block,
    .set_param       = vt_set_param,
    .get_param       = vt_get_param,
    .on_midi         = vt_on_midi,
};

__attribute__((visibility("default")))
audio_fx_api_v2_t *move_audio_fx_init_v2(const host_api_v1_t *host)
{
    g_host = host;
    return &g_api;
}
