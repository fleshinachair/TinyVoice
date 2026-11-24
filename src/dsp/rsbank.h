/*
 * Resynth's preset banks, read into memory so the host's Presets page (list_param rs_preset) can load
 * one from the audio thread with no I/O. Two banks:
 *   Default   the presets shipped with the module (<module folder>/presets),
 *   Imported  the Schwung User Presets in TinyVoice's preset folder
 *             (/data/UserData/schwung/presets/tinyvoice): imported Humanoid presets, and anything saved
 *             from My Presets, as long as its state runs Resynth.
 *
 * rsbank_load reads files and allocates: call it off the audio thread (plugin.c
 * runs it on a thread of its own at create_instance), then hand the bank to the
 * instance with dsp_set_rs_bank. Nothing here touches Schwung.
 *
 * A preset file is {"name": ..., "module": "tinyvoice", "version": 1, "state": <object or string>}
 * (what the host's My Presets saves and what scripts/import_humanoid.py writes). Taken: a file of at
 * most RSBANK_MAX_FILE bytes whose module is tinyvoice (or absent) and whose state has
 * "engine": "Resynth". Sorted by name the way the host's My Presets list is (case-insensitive).
 */
#ifndef TINYVOICE_RSBANK_H
#define TINYVOICE_RSBANK_H

#define RSBANK_BANKS    2             /* Default, Imported */
#define RSBANK_MAX      1024          /* per bank: rs_preset's range (0..1023) */
#define RSBANK_MAX_FILE (64 * 1024)
#define RSBANK_NAME     64

typedef struct {
    char  name[RSBANK_NAME];
    char *state;                      /* the state JSON object, as dsp_set_param("state") takes it */
} rsbank_entry;

typedef struct rsbank {
    int count[RSBANK_BANKS];
    rsbank_entry *e[RSBANK_BANKS];
} rsbank_t;

/* Reads the two folders (either may be NULL or missing: that bank is empty). NULL only when memory runs out. */
rsbank_t *rsbank_load(const char *default_dir, const char *imported_dir);
void rsbank_free(rsbank_t *b);

/* Exposed for the tests: one preset file's text -> name and state (state malloc'd; the caller frees it).
 * 0 if it is a TinyVoice Resynth preset, -1 otherwise. */
int rsbank_parse(const char *text, char *name, int name_len, char **state);

#endif
