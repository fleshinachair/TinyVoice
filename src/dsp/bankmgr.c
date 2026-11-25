/*
 * Bank management (bankmgr.h). Off the audio thread except bankmgr_submit, which only copies.
 */
#define _POSIX_C_SOURCE 200809L
#include "bankmgr.h"
#include "rsbank.h"

#include <dirent.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define BM_MKDIR(p) _mkdir(p)
#define BM_RMDIR(p) _rmdir(p)
#else
#include <unistd.h>
#define BM_MKDIR(p) mkdir((p), 0755)
#define BM_RMDIR(p) rmdir(p)
#endif

#define BM_RING   8                   /* commands queued at most */
#define BM_GRAVE  16                  /* old lists waiting to be freed */
#define BM_PATH   1024
#define BM_MSG    160

struct bankmgr {
    vt_t *vt;
    char dflt[BM_PATH], imported[BM_PATH], root[BM_PATH];
    char (*ring)[BANKMGR_CMD_MAX];    /* BM_RING commands */
    atomic_uint head, tail;           /* produced / consumed */
    atomic_flag lock;                 /* one producer at a time */
    int loaded;                       /* the first read has been done */
    int grace;
    struct { rsbank_t *bank; int age; } grave[BM_GRAVE];
    atomic_uint rev;
    atomic_flag msg_lock;
    int ok;
    char msg[BM_MSG];
};

static void set_msg(bankmgr_t *bm, int ok, const char *fmt, const char *a)
{
    while (atomic_flag_test_and_set_explicit(&bm->msg_lock, memory_order_acquire)) {}
    bm->ok = ok;
    snprintf(bm->msg, sizeof bm->msg, fmt, a ? a : "");
    atomic_flag_clear_explicit(&bm->msg_lock, memory_order_release);
}

bankmgr_t *bankmgr_create(vt_t *vt, const char *dflt, const char *imported, const char *banks_root)
{
    bankmgr_t *bm = (bankmgr_t *)calloc(1, sizeof *bm);
    if (!bm) return NULL;
    bm->ring = (char (*)[BANKMGR_CMD_MAX])calloc(BM_RING, BANKMGR_CMD_MAX);
    if (!bm->ring) { free(bm); return NULL; }
    bm->vt = vt;
    snprintf(bm->dflt, sizeof bm->dflt, "%s", dflt ? dflt : "");
    snprintf(bm->imported, sizeof bm->imported, "%s", imported ? imported : "");
    snprintf(bm->root, sizeof bm->root, "%s", banks_root ? banks_root : "");
    bm->grace = 100;
    atomic_init(&bm->head, 0u);
    atomic_init(&bm->tail, 0u);
    atomic_init(&bm->rev, 0u);
    atomic_flag_clear(&bm->lock);
    atomic_flag_clear(&bm->msg_lock);
    bm->ok = 1;
    return bm;
}

void bankmgr_destroy(bankmgr_t *bm)
{
    if (!bm) return;
    for (int i = 0; i < BM_GRAVE; i++) rsbank_free(bm->grave[i].bank);
    free(bm->ring);
    free(bm);
}

void bankmgr_set_grace(bankmgr_t *bm, int pumps)
{
    if (bm && pumps >= 0) bm->grace = pumps;
}

/* ------------------------------------------------------------ the queue */

int bankmgr_submit(bankmgr_t *bm, const char *cmd)
{
    if (!bm || !cmd) return -1;
    const size_t n = strlen(cmd);
    int rc = -1;
    while (atomic_flag_test_and_set_explicit(&bm->lock, memory_order_acquire)) {}
    const unsigned h = atomic_load_explicit(&bm->head, memory_order_relaxed);
    const unsigned t = atomic_load_explicit(&bm->tail, memory_order_acquire);
    if (h - t >= BM_RING) {
        set_msg(bm, 0, "error: %s", "busy, try again in a moment");
    } else if (n + 1 >= BANKMGR_CMD_MAX) {
        set_msg(bm, 0, "error: %s", "preset too big");
    } else {
        char *slot = bm->ring[h % BM_RING];
        memcpy(slot, cmd, n + 1);
        const char *nl = strchr(slot, '\n');                       /* "save": the sound as it is now (the host's own state text) */
        if (nl && (size_t)(nl - slot) == 4 && strncmp(slot, "save", 4) == 0) {
            char *f[4] = { slot, NULL, NULL, NULL };               /* op, engine, bank, name */
            char *p = slot;
            for (int k = 1; k < 4; k++) { p = strchr(p, '\n'); if (!p) break; f[k] = ++p; }
            if (!f[3]) {
                set_msg(bm, 0, "error: %s", "bad command");
            } else {
                char *tail = strchr(f[3], '\n');
                if (!tail) tail = f[3] + strlen(f[3]);
                char name[RSBANK_NAME];
                const size_t nl_ = (size_t)(tail - f[3]) < sizeof name - 1 ? (size_t)(tail - f[3]) : sizeof name - 1;
                memcpy(name, f[3], nl_); name[nl_] = '\0';
                for (const char *c = name; *c; c++) if (*c == '"' || *c == '\\' || (unsigned char)*c < 0x20) { name[0] = '\0'; break; }
                const size_t head_len = (size_t)(tail - slot);
                char *pl = slot + head_len;
                int w = snprintf(pl, BANKMGR_CMD_MAX - head_len, "\n{\"name\":\"%s\",\"module\":\"tinyvoice\",\"version\":1,\"state\":", name);
                if (name[0] && w > 0 && (size_t)w < BANKMGR_CMD_MAX - head_len - 16) {
                    const int avail = (int)(BANKMGR_CMD_MAX - head_len) - w - 2;
                    const int g = dsp_get_param(bm->vt, "state", pl + w, avail);
                    if (g > 0 && g < avail) {
                        pl[w + g] = '}';
                        pl[w + g + 1] = '\0';
                        atomic_store_explicit(&bm->head, h + 1, memory_order_release);
                        rc = 0;
                    } else {
                        set_msg(bm, 0, "error: %s", "the sound is too big to save");
                    }
                } else {
                    set_msg(bm, 0, "error: %s", "bad name");
                }
            }
        } else {
            atomic_store_explicit(&bm->head, h + 1, memory_order_release);
            rc = 0;
        }
    }
    atomic_flag_clear_explicit(&bm->lock, memory_order_release);
    return rc;
}

/* ------------------------------------------------------------ the files */

static int is_dir(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int make_dir(const char *path)
{
    return BM_MKDIR(path) == 0 || (errno == EEXIST && is_dir(path)) ? 0 : -1;
}

static int ends_json(const char *fn)
{
    const size_t n = strlen(fn);
    return n > 5 && strcmp(fn + n - 5, ".json") == 0;
}

/* how many user banks the engine's folder holds */
static int count_banks(const char *edir)
{
    DIR *d = opendir(edir);
    int n = 0;
    struct dirent *de;
    char path[BM_PATH];
    while (d && (de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        if (snprintf(path, sizeof path, "%s/%s", edir, de->d_name) < (int)sizeof path && is_dir(path)) n++;
    }
    if (d) closedir(d);
    return n;
}

static int write_file(const char *path, const char *text)
{
    char tmp[BM_PATH + 8];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;
    const size_t n = strlen(text);
    const int ok = fwrite(text, 1, n, f) == n;
    if (fclose(f) != 0 || !ok) { remove(tmp); return -1; }
#ifdef _WIN32
    remove(path);                                                   /* (rename does not replace there) */
#endif
    if (rename(tmp, path) != 0) { remove(tmp); return -1; }
    return 0;
}

/* Runs one command. 0 ok (msg says what), -1 refused (msg says why). */
static int run_command(bankmgr_t *bm, char *cmd, char *msg, size_t msg_n)
{
    char *f[5] = { cmd, NULL, NULL, NULL, NULL };                  /* op, engine, bank, name, payload */
    char *p = cmd;
    for (int k = 1; k < 5; k++) {
        p = strchr(p, '\n');
        if (!p) break;
        *p++ = '\0';
        f[k] = p;
    }
    if (!f[3]) { snprintf(msg, msg_n, "error: bad command"); return -1; }
    char *nl = strchr(f[3], '\n');                                 /* the name ends at its newline (or at the end) */
    if (nl) { *nl = '\0'; f[4] = nl + 1; }
    const char *op = f[0], *engine = f[1], *bank = f[2], *name = f[3], *payload = f[4] ? f[4] : "";
    if (strcmp(engine, "resynth") != 0 && strcmp(engine, "mangler") != 0) { snprintf(msg, msg_n, "error: unknown engine"); return -1; }
    if (!bm->root[0]) { snprintf(msg, msg_n, "error: no folder for banks"); return -1; }
    if (!rsbank_bank_name_ok(bank)) {
        snprintf(msg, msg_n, "error: a bank name is 1-40 letters, digits, spaces or . _ - ( ) & + ' (not one of the fixed banks)");
        return -1;
    }
    char edir[BM_PATH], bdir[BM_PATH], path[BM_PATH];
    snprintf(edir, sizeof edir, "%s/%s", bm->root, engine);
    snprintf(bdir, sizeof bdir, "%s/%s", edir, bank);
    if (strcmp(op, "delete") != 0 && strcmp(op, "remove") != 0 && make_dir(bm->root) != 0 && !is_dir(bm->root)) {
        snprintf(msg, msg_n, "error: cannot make the banks folder");
        return -1;
    }
    if (strcmp(op, "delete") != 0 && strcmp(op, "remove") != 0 && make_dir(edir) != 0) {
        snprintf(msg, msg_n, "error: cannot make the banks folder");
        return -1;
    }

    if (strcmp(op, "new") == 0) {
        if (is_dir(bdir)) { snprintf(msg, msg_n, "error: there is a bank named %s already", bank); return -1; }
        if (count_banks(edir) >= RSBANK_USER_MAX) { snprintf(msg, msg_n, "error: %d banks is the most", RSBANK_USER_MAX); return -1; }
        if (make_dir(bdir) != 0) { snprintf(msg, msg_n, "error: cannot make the bank"); return -1; }
        snprintf(msg, msg_n, "ok: made bank %s", bank);
        return 0;
    }
    if (strcmp(op, "rename") == 0) {
        char ndir[BM_PATH];
        if (!rsbank_bank_name_ok(name)) { snprintf(msg, msg_n, "error: that is not a name a bank can have"); return -1; }
        snprintf(ndir, sizeof ndir, "%s/%s", edir, name);
        if (!is_dir(bdir)) { snprintf(msg, msg_n, "error: no bank named %s", bank); return -1; }
        if (is_dir(ndir) && strcmp(bank, name) != 0) { snprintf(msg, msg_n, "error: there is a bank named %s already", name); return -1; }
        if (rename(bdir, ndir) != 0) { snprintf(msg, msg_n, "error: cannot rename the bank"); return -1; }
        dsp_bank_renamed(bm->vt, strcmp(engine, "mangler") == 0, bank, name);                     /* (before the lists are swapped) */
        snprintf(msg, msg_n, "ok: %s is now %s", bank, name);
        return 0;
    }
    if (strcmp(op, "delete") == 0) {
        DIR *d = opendir(bdir);
        struct dirent *de;
        int n = 0;
        if (!d) { snprintf(msg, msg_n, "error: no bank named %s", bank); return -1; }
        while ((de = readdir(d)) != NULL) {                        /* only the preset files; anything else stays and stops the rmdir */
            if (!ends_json(de->d_name) || snprintf(path, sizeof path, "%s/%s", bdir, de->d_name) >= (int)sizeof path) continue;
            if (remove(path) == 0) n++;
        }
        closedir(d);
        if (BM_RMDIR(bdir) != 0) { snprintf(msg, msg_n, "error: the bank has files that are not presets; it was emptied of presets only"); return -1; }
        snprintf(msg, msg_n, "ok: deleted bank %s (%d presets)", bank, n);
        return 0;
    }
    if (strcmp(op, "add") == 0 || strcmp(op, "save") == 0) {
        char nm[RSBANK_NAME], *state;
        char safe[80];
        if (!name[0]) { snprintf(msg, msg_n, "error: the preset has no name"); return -1; }
        if (rsbank_parse_engine(payload, engine, nm, sizeof nm, &state) != 0) {
            snprintf(msg, msg_n, "error: that is not a %s preset", engine);
            return -1;
        }
        free(state);
        if (!is_dir(bdir)) {
            if (count_banks(edir) >= RSBANK_USER_MAX) { snprintf(msg, msg_n, "error: %d banks is the most", RSBANK_USER_MAX); return -1; }
            if (make_dir(bdir) != 0) { snprintf(msg, msg_n, "error: cannot make the bank"); return -1; }
        }
        rsbank_safe_name(name, safe, sizeof safe);
        if (snprintf(path, sizeof path, "%s/%s.json", bdir, safe) >= (int)sizeof path || write_file(path, payload) != 0) {
            snprintf(msg, msg_n, "error: cannot write the preset");
            return -1;
        }
        snprintf(msg, msg_n, "ok: %s is in bank %s", name, bank);
        return 0;
    }
    if (strcmp(op, "remove") == 0) {
        char safe[80];
        rsbank_safe_name(name, safe, sizeof safe);
        if (snprintf(path, sizeof path, "%s/%s.json", bdir, safe) >= (int)sizeof path || remove(path) != 0) {
            snprintf(msg, msg_n, "error: %s is not in bank %s (only presets made here can be removed)", name, bank);
            return -1;
        }
        snprintf(msg, msg_n, "ok: removed %s from bank %s", name, bank);
        return 0;
    }
    snprintf(msg, msg_n, "error: unknown command");
    return -1;
}

/* ------------------------------------------------------------ the worker's pass */

static void bury(bankmgr_t *bm, rsbank_t *old)
{
    if (!old) return;
    int slot = -1, oldest = 0;
    for (int i = 0; i < BM_GRAVE; i++) {
        if (!bm->grave[i].bank) { slot = i; break; }
        if (bm->grave[i].age > bm->grave[oldest].age) oldest = i;
    }
    if (slot < 0) { rsbank_free(bm->grave[oldest].bank); slot = oldest; }   /* (never: the lists age out in seconds) */
    bm->grave[slot].bank = old;
    bm->grave[slot].age = 0;
}

static void reload(bankmgr_t *bm)
{
    char rdir[BM_PATH], mdir[BM_PATH];
    snprintf(rdir, sizeof rdir, "%s/resynth", bm->root);
    snprintf(mdir, sizeof mdir, "%s/mangler", bm->root);
    rsbank_t *r = rsbank_load_full("resynth", bm->dflt, bm->imported, bm->root[0] ? rdir : NULL);
    if (r) bury(bm, dsp_swap_rs_bank(bm->vt, r));
    rsbank_t *m = rsbank_load_full("mangler", bm->dflt, bm->imported, bm->root[0] ? mdir : NULL);
    if (m) bury(bm, dsp_swap_mg_bank(bm->vt, m));
    atomic_fetch_add_explicit(&bm->rev, 1u, memory_order_release);
}

int bankmgr_pump(bankmgr_t *bm)
{
    if (!bm) return 0;
    for (int i = 0; i < BM_GRAVE; i++)                             /* the old lists: freed once they are old enough */
        if (bm->grave[i].bank && ++bm->grave[i].age > bm->grace) { rsbank_free(bm->grave[i].bank); bm->grave[i].bank = NULL; }
    int ran = 0, first_err = 0;
    char msg[BM_MSG], last_ok[BM_MSG] = "";
    char *cmd = (char *)malloc(BANKMGR_CMD_MAX);
    if (!cmd) return 0;
    for (;;) {
        const unsigned t = atomic_load_explicit(&bm->tail, memory_order_relaxed);
        if (t == atomic_load_explicit(&bm->head, memory_order_acquire)) break;
        memcpy(cmd, bm->ring[t % BM_RING], BANKMGR_CMD_MAX);
        atomic_store_explicit(&bm->tail, t + 1, memory_order_release);
        ran++;
        if (run_command(bm, cmd, msg, sizeof msg) == 0) {
            snprintf(last_ok, sizeof last_ok, "%s", msg);
        } else if (!first_err) {
            first_err = 1;
            set_msg(bm, 0, "%s", msg);
        }
    }
    free(cmd);
    if (ran || !bm->loaded) {
        reload(bm);
        bm->loaded = 1;
    }
    if (ran && !first_err) set_msg(bm, 1, "%s", last_ok);
    return ran;
}

void bankmgr_status(bankmgr_t *bm, char *buf, int buf_len)
{
    if (!buf || buf_len <= 0) return;
    buf[0] = '\0';
    if (!bm) return;
    char m[BM_MSG * 2];
    int ok;
    while (atomic_flag_test_and_set_explicit(&bm->msg_lock, memory_order_acquire)) {}
    ok = bm->ok;
    size_t o = 0;
    for (const char *c = bm->msg; *c && o + 2 < sizeof m; c++) {   /* (JSON-safe: the messages hold no quote or backslash but names may) */
        if (*c == '"' || *c == '\\') m[o++] = '\\';
        if ((unsigned char)*c >= 0x20) m[o++] = *c;
    }
    m[o] = '\0';
    atomic_flag_clear_explicit(&bm->msg_lock, memory_order_release);
    snprintf(buf, (size_t)buf_len, "{\"rev\":%u,\"ok\":%d,\"msg\":\"%s\"}", atomic_load_explicit(&bm->rev, memory_order_acquire), ok, m);
}
