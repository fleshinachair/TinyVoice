/* Copyright (c) 2026 Mathew Taylor — Licensed under GPL-3.0-only */
/*
 * Resynth's preset bank (rsbank.h). Off the audio thread only: reads a folder, allocates.
 */
#include "rsbank.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* ------------------------------------------------------------ a small JSON reader */

static const char *ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

/* p at '"': just past the closing quote, or NULL if unterminated */
static const char *str_end(const char *p)
{
    for (p++; *p; p++) {
        if (*p == '\\') { if (!*++p) return NULL; }
        else if (*p == '"') return p + 1;
    }
    return NULL;
}

/* just past any JSON value starting at p, or NULL */
static const char *val_end(const char *p)
{
    p = ws(p);
    if (*p == '"') return str_end(p);
    if (*p == '{' || *p == '[') {
        int depth = 0;
        while (*p) {
            if (*p == '"') { p = str_end(p); if (!p) return NULL; continue; }
            if (*p == '{' || *p == '[') depth++;
            else if (*p == '}' || *p == ']') { if (--depth == 0) return p + 1; }
            p++;
        }
        return NULL;
    }
    const char *s = p;
    while (*p && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') p++;
    return p > s ? p : NULL;
}

/* The value of top-level member `key` of the object at `obj` (keys compared as written), or NULL. */
static const char *member(const char *obj, const char *key)
{
    const char *p = ws(obj);
    const size_t kn = strlen(key);
    if (*p != '{') return NULL;
    p = ws(p + 1);
    while (*p == '"') {
        const char *ke = str_end(p);
        if (!ke) return NULL;
        const int hit = (size_t)(ke - p - 2) == kn && memcmp(p + 1, key, kn) == 0;
        p = ws(ke);
        if (*p != ':') return NULL;
        p = ws(p + 1);
        if (hit) return p;
        p = val_end(p);
        if (!p) return NULL;
        p = ws(p);
        if (*p != ',') return NULL;
        p = ws(p + 1);
    }
    return NULL;
}

static int hexv(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* The string at p ('"'), unescaped into dst (cap bytes, always terminated; longer text is cut). Length written, or -1. */
static int unescape(const char *p, char *dst, int cap)
{
    const char *e = str_end(p);
    int n = 0;
    if (!e || cap <= 0) return -1;
    for (p++; p < e - 1; p++) {
        unsigned c = (unsigned char)*p;
        if (c == '\\') {
            c = (unsigned char)*++p;
            switch (c) {
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            case 'u': {
                int h[4];
                for (int k = 0; k < 4; k++) if ((h[k] = hexv(p[1 + k])) < 0) return -1;
                c = (unsigned)(h[0] << 12 | h[1] << 8 | h[2] << 4 | h[3]);
                p += 4;
                if (c >= 0x80) {                                   /* as UTF-8 (a lone surrogate passes as is) */
                    unsigned char u[3];
                    int m;
                    if (c < 0x800) { u[0] = (unsigned char)(0xC0 | c >> 6); u[1] = (unsigned char)(0x80 | (c & 0x3F)); m = 2; }
                    else { u[0] = (unsigned char)(0xE0 | c >> 12); u[1] = (unsigned char)(0x80 | (c >> 6 & 0x3F));
                           u[2] = (unsigned char)(0x80 | (c & 0x3F)); m = 3; }
                    for (int k = 0; k < m; k++) if (n < cap - 1) dst[n++] = (char)u[k];
                    continue;
                }
                break;
            }
            default: break;                                        /* \" \\ \/ */
            }
        }
        if (n < cap - 1) dst[n++] = (char)c;
    }
    dst[n] = '\0';
    return n;
}

/* Does the state object run `want` ("engine": "Resynth" / "Mangler", any case; want is lower case)? */
static int runs_engine(const char *state, const char *want)
{
    const char *v = member(state, "engine");
    char eng[16];
    if (!v || *v != '"' || unescape(v, eng, sizeof eng) < 0) return 0;
    for (int i = 0;; i++) {
        const char c = eng[i] >= 'A' && eng[i] <= 'Z' ? (char)(eng[i] + 32) : eng[i];
        if (c != want[i]) return 0;
        if (!want[i]) return 1;
    }
}

int rsbank_parse_engine(const char *text, const char *engine, char *name, int name_len, char **state)
{
    *state = NULL;
    if (name_len > 0) name[0] = '\0';
    if (!text || *ws(text) != '{') return -1;
    const char *mod = member(text, "module");
    if (mod) {
        char m[32];
        if (*mod != '"' || unescape(mod, m, sizeof m) < 0 || strcmp(m, "tinyvoice") != 0) return -1;
    }
    const char *st = member(text, "state");
    if (!st) return -1;
    char *s;
    if (*st == '{') {                                              /* stored as the object (the host's own saves) */
        const char *e = val_end(st);
        if (!e) return -1;
        s = (char *)malloc((size_t)(e - st) + 1);
        if (!s) return -1;
        memcpy(s, st, (size_t)(e - st));
        s[e - st] = '\0';
    } else if (*st == '"') {                                       /* or as a string holding it */
        const char *e = str_end(st);
        if (!e) return -1;
        const int cap = (int)(e - st);
        s = (char *)malloc((size_t)cap);
        if (!s) return -1;
        if (unescape(st, s, cap) < 0) { free(s); return -1; }
    } else {
        return -1;
    }
    if (!runs_engine(s, engine)) { free(s); return -1; }
    const char *nm = member(text, "name");
    if (nm && *nm == '"' && name_len > 0) unescape(nm, name, name_len);
    *state = s;
    return 0;
}

int rsbank_parse(const char *text, char *name, int name_len, char **state)
{
    return rsbank_parse_engine(text, "resynth", name, name_len, state);
}

/* ------------------------------------------------------------ kinds */

static const char *const k_bank_names[RSBANK_BANKS] = { RSBANK_NAMES };

static int same_word(const char *a, size_t an, const char *b)   /* a[0..an) == b, ignoring case */
{
    if (strlen(b) != an) return 0;
    for (size_t i = 0; i < an; i++) {
        const char x = a[i] >= 'A' && a[i] <= 'Z' ? (char)(a[i] + 32) : a[i];
        const char y = b[i] >= 'A' && b[i] <= 'Z' ? (char)(b[i] + 32) : b[i];
        if (x != y) return 0;
    }
    return 1;
}

int rsbank_kind(const char *category, const char *name)
{
    if (category && category[0]) {
        for (int k = RSBANK_TUNER; k <= RSBANK_MINE; k++)
            if (same_word(category, strlen(category), k_bank_names[k])) return k;
        return -1;                                                 /* a kind the list doesn't have: Imported only */
    }
    /* Humanoid's Factory names: "<digits>. <Category> - ..." */
    const char *p = name ? name : "";
    while (*p == ' ') p++;
    if (*p >= '0' && *p <= '9') {
        while (*p >= '0' && *p <= '9') p++;
        if (*p == '.') {
            p++;
            while (*p == ' ') p++;
            const char *w = p;
            while (*p && *p != ' ' && *p != '-') p++;
            for (int k = RSBANK_TUNER; k <= RSBANK_INSTRUMENT; k++)
                if (same_word(w, (size_t)(p - w), k_bank_names[k])) return k;
        }
    }
    return RSBANK_MINE;
}

/* ------------------------------------------------------------ the folder */

static int name_cmp(const void *a, const void *b)
{
    const char *x = ((const rsbank_entry *)a)->name, *y = ((const rsbank_entry *)b)->name;
    for (const char *p = x, *q = y;; p++, q++) {
        const int c = *p >= 'A' && *p <= 'Z' ? *p + 32 : (unsigned char)*p;
        const int d = *q >= 'A' && *q <= 'Z' ? *q + 32 : (unsigned char)*q;
        if (c != d) return c - d;
        if (!c) break;
    }
    return strcmp(x, y);
}

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *buf = NULL;
    if (fseek(f, 0, SEEK_END) == 0) {
        const long n = ftell(f);
        if (n > 0 && n <= RSBANK_MAX_FILE && fseek(f, 0, SEEK_SET) == 0 && (buf = (char *)malloc((size_t)n + 1)) != NULL) {
            if (fread(buf, 1, (size_t)n, f) == (size_t)n) buf[n] = '\0';
            else { free(buf); buf = NULL; }
        }
    }
    fclose(f);
    return buf;
}

static void load_dir(rsbank_t *b, int bank, const char *dir, const char *engine)
{
    DIR *d = dir ? opendir(dir) : NULL;
    if (!d) return;
    int cap = 0;
    struct dirent *de;
    char path[1024];
    while ((de = readdir(d)) != NULL && b->count[bank] < RSBANK_MAX) {
        const char *fn = de->d_name;
        const size_t fl = strlen(fn);
        if (fl < 6 || fn[0] == '.' || strcmp(fn + fl - 5, ".json") != 0) continue;
        if (snprintf(path, sizeof path, "%s/%s", dir, fn) >= (int)sizeof path) continue;
        char *text = read_file(path);
        if (!text) continue;
        char name[RSBANK_NAME], cat[32], *state;
        const int ok = rsbank_parse_engine(text, engine, name, sizeof name, &state) == 0;
        const char *cv = ok ? member(text, "category") : NULL;
        const int has_cat = cv && *cv == '"' && unescape(cv, cat, sizeof cat) >= 0;
        free(text);
        if (!ok) continue;
        if (!name[0]) {                                            /* no name: the file's, as the host does */
            const size_t k = fl - 5 < sizeof name - 1 ? fl - 5 : sizeof name - 1;
            memcpy(name, fn, k);
            name[k] = '\0';
        }
        if (b->count[bank] == cap) {
            const int nc = cap ? 2 * cap : 64;
            rsbank_entry *ne = (rsbank_entry *)realloc(b->e[bank], (size_t)nc * sizeof *ne);
            if (!ne) { free(state); break; }
            b->e[bank] = ne;
            cap = nc;
        }
        memcpy(b->e[bank][b->count[bank]].name, name, sizeof name);
        b->e[bank][b->count[bank]].state = state;
        b->e[bank][b->count[bank]].kind = bank == RSBANK_IMPORTED ? rsbank_kind(has_cat ? cat : NULL, name) : -1;
        b->count[bank]++;
    }
    closedir(d);
    if (b->count[bank] > 1) qsort(b->e[bank], (size_t)b->count[bank], sizeof *b->e[bank], name_cmp);
}

/* ------------------------------------------------------------ your own banks */

static int name_char_ok(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '.' || c == '_' ||
           c == '-' || c == '(' || c == ')' || c == '&' || c == '+' || c == '\'' || c >= 0x80;
}

static int ieq_str(const char *a, const char *b)
{
    for (;; a++, b++) {
        const int c = *a >= 'A' && *a <= 'Z' ? *a + 32 : (unsigned char)*a;
        const int d = *b >= 'A' && *b <= 'Z' ? *b + 32 : (unsigned char)*b;
        if (c != d) return 0;
        if (!c) return 1;
    }
}

int rsbank_bank_name_ok(const char *name)
{
    if (!name) return 0;
    const size_t n = strlen(name);
    if (n < 1 || n > 40 || name[0] == '.' || name[0] == ' ' || name[n - 1] == ' ' || name[n - 1] == '.') return 0;
    for (size_t i = 0; i < n; i++) if (!name_char_ok((unsigned char)name[i])) return 0;
    for (int k = 0; k < RSBANK_BANKS; k++) if (ieq_str(name, k_bank_names[k])) return 0;      /* (Default, Imported, ... are ours) */
    return 1;
}

void rsbank_safe_name(const char *name, char *out, int out_len)
{
    int n = 0;
    if (out_len <= 0) return;
    for (const char *p = name ? name : ""; *p && n < 60 && n < out_len - 1; p++) out[n++] = name_char_ok((unsigned char)*p) ? *p : '_';
    while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '.')) n--;
    if (n == 0 && out_len > 7) { memcpy(out, "preset", 6); n = 6; }
    out[n] = '\0';
    if (out[0] == '.') out[0] = '_';
}

static int str_cmp_i(const void *a, const void *b)
{
    const char *x = (const char *)a, *y = (const char *)b;
    for (;; x++, y++) {
        const int c = *x >= 'A' && *x <= 'Z' ? *x + 32 : (unsigned char)*x;
        const int d = *y >= 'A' && *y <= 'Z' ? *y + 32 : (unsigned char)*y;
        if (c != d) return c - d;
        if (!c) return 0;
    }
}

/* the folders of banks_dir that are valid bank names, sorted; how many (at most RSBANK_USER_MAX) */
static int list_user_banks(const char *banks_dir, char names[RSBANK_USER_MAX][RSBANK_NAME])
{
    DIR *d = banks_dir ? opendir(banks_dir) : NULL;
    if (!d) return 0;
    enum { CAP = 64 };
    char (*all)[RSBANK_NAME] = (char (*)[RSBANK_NAME])malloc((size_t)CAP * RSBANK_NAME);
    int n = 0;
    struct dirent *de;
    while (all && (de = readdir(d)) != NULL && n < CAP) {
        char path[1024];
        struct stat st;
        if (strlen(de->d_name) >= RSBANK_NAME || !rsbank_bank_name_ok(de->d_name)) continue;
        if (snprintf(path, sizeof path, "%s/%s", banks_dir, de->d_name) >= (int)sizeof path) continue;
        if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        memcpy(all[n++], de->d_name, strlen(de->d_name) + 1);
    }
    closedir(d);
    if (n > 1) qsort(all, (size_t)n, RSBANK_NAME, str_cmp_i);
    if (n > RSBANK_USER_MAX) n = RSBANK_USER_MAX;
    for (int i = 0; i < n; i++) memcpy(names[i], all[i], RSBANK_NAME);
    free(all);
    return n;
}

rsbank_t *rsbank_load_full(const char *engine, const char *default_dir, const char *imported_dir, const char *banks_dir)
{
    rsbank_t *b = (rsbank_t *)calloc(1, sizeof *b);
    if (!b) return NULL;
    const int resynth = strcmp(engine, "resynth") == 0;
    load_dir(b, RSBANK_DEFAULT, default_dir, engine);
    load_dir(b, RSBANK_IMPORTED, imported_dir, engine);
    /* the split banks: Imported's entries by kind, in Imported's order, sharing its states */
    for (int k = RSBANK_TUNER; k < RSBANK_BANKS && resynth; k++) {   /* (Resynth's list only) */
        int n = 0;
        b->borrowed[k] = 1;
        for (int i = 0; i < b->count[RSBANK_IMPORTED]; i++) n += b->e[RSBANK_IMPORTED][i].kind == k;
        if (!n) continue;
        b->e[k] = (rsbank_entry *)malloc((size_t)n * sizeof *b->e[k]);
        if (!b->e[k]) continue;                                    /* out of memory: that bank stays empty */
        for (int i = 0; i < b->count[RSBANK_IMPORTED]; i++)
            if (b->e[RSBANK_IMPORTED][i].kind == k) b->e[k][b->count[k]++] = b->e[RSBANK_IMPORTED][i];
    }
    /* the list: the fixed banks, then yours */
    b->first_user = resynth ? RSBANK_BANKS : RSBANK_IMPORTED + 1;
    for (int k = 0; k < b->first_user; k++) snprintf(b->name[k], RSBANK_NAME, "%s", k_bank_names[k]);
    b->nbanks = b->first_user;
    char names[RSBANK_USER_MAX][RSBANK_NAME];
    const int nu = list_user_banks(banks_dir, names);
    for (int u = 0; u < nu; u++) {
        char path[1024];
        const int slot = b->first_user + u;
        if (snprintf(path, sizeof path, "%s/%s", banks_dir, names[u]) >= (int)sizeof path) continue;
        snprintf(b->name[slot], RSBANK_NAME, "%s", names[u]);
        load_dir(b, slot, path, engine);
        b->nbanks = slot + 1;
    }
    return b;
}

rsbank_t *rsbank_load_engine(const char *engine, const char *default_dir, const char *imported_dir)
{
    return rsbank_load_full(engine, default_dir, imported_dir, NULL);
}

rsbank_t *rsbank_load(const char *default_dir, const char *imported_dir)
{
    return rsbank_load_engine("resynth", default_dir, imported_dir);
}

void rsbank_free(rsbank_t *b)
{
    if (!b) return;
    for (int k = 0; k < RSBANK_SLOTS; k++) {
        if (!b->borrowed[k])                                       /* the split banks only borrow Imported's states */
            for (int i = 0; i < b->count[k]; i++) free(b->e[k][i].state);
        free(b->e[k]);
    }
    free(b);
}
