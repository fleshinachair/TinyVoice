/*
 * Bank management: your own banks of Resynth and Mangler presets, made, renamed, filled and removed while the module
 * runs (the Remote UI's bank buttons). Nothing here touches Schwung and nothing here starts a thread: plugin.c runs
 * bankmgr_pump on a worker thread of its own every 50 ms, and the tests call it by hand.
 *
 * Where: <banks_root>/<engine>/<bank>/<preset>.json, engine "resynth" or "mangler": each engine has banks of its own and
 * sees no other's. A preset file is the host's own shape ({"name","module":"tinyvoice","version":1,"state":{...}}), so the
 * same file also works in My Presets. rsbank.c lists them (rsbank_load_full) after the fixed banks.
 *
 * A command is text, fields split by newlines, the last one (the preset file's text) free:
 *     <op> \n <engine> \n <bank> \n <name> \n <payload>
 *   new     bank                       make an empty bank
 *   rename  bank, name = the new name  rename a bank
 *   delete  bank                       remove a bank and its preset files
 *   add     bank, name, payload        write a preset file into the bank (the bank is made if it is new)
 *   save    bank, name                 the same with the current sound as the preset (made here from dsp_get_param "state")
 *   remove  bank, name                 delete one preset file
 * bankmgr_submit queues one (real-time safe: a copy into a ring); bankmgr_pump runs everything queued, then reads the
 * banks again and swaps them into the instance (dsp_swap_*), and keeps the old lists a few seconds before freeing them
 * (the audio thread may still be reading one). Fixed banks (Default, Imported, the kinds) cannot be touched.
 */
#ifndef TINYVOICE_BANKMGR_H
#define TINYVOICE_BANKMGR_H

#include "dsp.h"

#define BANKMGR_CMD_MAX 16384         /* one command, bytes (a preset file with a wavetable is ~4 KB) */

typedef struct bankmgr bankmgr_t;

/* dflt / imported: the Default and Imported folders; banks_root: the folder that holds <engine>/<bank>/ (made when first needed). */
bankmgr_t *bankmgr_create(vt_t *vt, const char *dflt, const char *imported, const char *banks_root);
void bankmgr_destroy(bankmgr_t *bm);

/* 0 queued; -1 the queue is full or the command too long (the status says). Real-time safe. */
int bankmgr_submit(bankmgr_t *bm, const char *cmd);

/* Worker side: runs the queued commands, reads and swaps the banks when there were any (and at the first call), ages the
 * old lists. Returns how many commands it ran. */
int bankmgr_pump(bankmgr_t *bm);

/* {"rev":N,"ok":1,"msg":"..."}: rev counts the swaps (the UI refreshes its lists when it changes); msg is the last result. */
void bankmgr_status(bankmgr_t *bm, char *buf, int buf_len);

/* How many pumps an old list is kept (default 100 = 5 s at 50 ms); the tests lower it. */
void bankmgr_set_grace(bankmgr_t *bm, int pumps);

#endif
