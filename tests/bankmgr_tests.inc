/*
 * Bank management (src/dsp/bankmgr.c, rsbank_load_full): your own banks of Resynth and Mangler presets. Included by
 * test_main.c. Works in build/tests/bm_root (made and removed here).
 */
#include <dirent.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define BMT_MKDIR(p) _mkdir(p)
#define BMT_RMDIR(p) _rmdir(p)
#else
#define BMT_MKDIR(p) mkdir((p), 0755)
#define BMT_RMDIR(p) rmdir(p)
#endif

#define BMT_ROOT "build/tests/bm_root"

static void bmt_rmrf(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        struct dirent *de;
        char sub[1024];
        while (d && (de = readdir(d)) != NULL) {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
            snprintf(sub, sizeof sub, "%s/%s", path, de->d_name);
            bmt_rmrf(sub);
        }
        if (d) closedir(d);
        BMT_RMDIR(path);
    } else {
        remove(path);
    }
}

static int bmt_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

/* one preset file's text */
static const char *bmt_rs(const char *name, double transform, char *out, size_t n)
{
    snprintf(out, n, "{\"name\":\"%s\",\"module\":\"tinyvoice\",\"version\":1,\"state\":{\"engine\":\"Resynth\",\"hm_transform\":%g}}", name, transform);
    return out;
}

static const char *bmt_mg(const char *name, double pitch, char *out, size_t n)
{
    snprintf(out, n, "{\"name\":\"%s\",\"module\":\"tinyvoice\",\"version\":1,\"state\":{\"engine\":\"Mangler\",\"mg_pitch\":%g}}", name, pitch);
    return out;
}

/* submit "op\nengine\nbank\nname\npayload" and run it; the status text afterwards */
static int bmt_cmd(bankmgr_t *bm, const char *op, const char *engine, const char *bank, const char *name, const char *payload, char *status, size_t sn)
{
    static char cmd[BANKMGR_CMD_MAX];
    snprintf(cmd, sizeof cmd, "%s\n%s\n%s\n%s%s%s", op, engine, bank, name, payload ? "\n" : "", payload ? payload : "");
    const int rc = bankmgr_submit(bm, cmd);
    bankmgr_pump(bm);
    if (status) bankmgr_status(bm, status, (int)sn);
    (void)sn;
    return rc;
}

static int bmt_status_ok(const char *status) { return strstr(status, "\"ok\":1") != NULL; }

static void test_bank_names(void)
{
    char out[96];
    CHECK(rsbank_bank_name_ok("Vocals") && rsbank_bank_name_ok("My Bank (2)") && rsbank_bank_name_ok("a.b-c_d+e&f'g"));
    CHECK(!rsbank_bank_name_ok("") && !rsbank_bank_name_ok(".hidden") && !rsbank_bank_name_ok(" lead") && !rsbank_bank_name_ok("trail "));
    CHECK(!rsbank_bank_name_ok("a/b") && !rsbank_bank_name_ok("a\\b") && !rsbank_bank_name_ok("..") && !rsbank_bank_name_ok("q\"x") && !rsbank_bank_name_ok("a\nb"));
    CHECK(!rsbank_bank_name_ok("Default") && !rsbank_bank_name_ok("imported") && !rsbank_bank_name_ok("TUNER") && !rsbank_bank_name_ok("Mine"));   /* the fixed banks' names */
    CHECK(!rsbank_bank_name_ok("0123456789012345678901234567890123456789X"));                                                                      /* 41 */
    rsbank_safe_name("Tune - Hard/Snap: 1?", out, sizeof out);
    CHECK(strcmp(out, "Tune - Hard_Snap_ 1_") == 0);
    rsbank_safe_name("", out, sizeof out);
    CHECK(strcmp(out, "preset") == 0);
    rsbank_safe_name("..", out, sizeof out);
    CHECK(out[0] != '.' && out[0] != '\0');
}

static void test_bank_user_folders(void)
{
    /* each engine lists the banks of its own folder, after its fixed ones, and only the presets that run it */
    char b[512];
    bmt_rmrf(BMT_ROOT);
    BMT_MKDIR("build"); BMT_MKDIR("build/tests"); BMT_MKDIR(BMT_ROOT); BMT_MKDIR(BMT_ROOT "/resynth"); BMT_MKDIR(BMT_ROOT "/mangler");
    BMT_MKDIR(BMT_ROOT "/resynth/Vocals"); BMT_MKDIR(BMT_ROOT "/resynth/aardvark"); BMT_MKDIR(BMT_ROOT "/mangler/Choirs");
    BMT_MKDIR(BMT_ROOT "/mangler/Default");                                                                    /* not a bank name: left out */
    FILE *f = fopen(BMT_ROOT "/resynth/Vocals/one.json", "wb"); fputs(bmt_rs("One", 0.5, b, sizeof b), f); fclose(f);
    f = fopen(BMT_ROOT "/resynth/Vocals/two.json", "wb"); fputs(bmt_rs("two", 0.25, b, sizeof b), f); fclose(f);
    f = fopen(BMT_ROOT "/resynth/Vocals/wrong.json", "wb"); fputs(bmt_mg("A Mangler one", 3, b, sizeof b), f); fclose(f);   /* the other engine's: left out */
    f = fopen(BMT_ROOT "/mangler/Choirs/c.json", "wb"); fputs(bmt_mg("Choir", 7, b, sizeof b), f); fclose(f);
    f = fopen(BMT_ROOT "/resynth/stray.txt", "wb"); fputs("x", f); fclose(f);                                    /* a file, not a bank */

    rsbank_t *r = rsbank_load_full("resynth", "tests/fixtures/rs_default", "tests/fixtures/rs_presets", BMT_ROOT "/resynth");
    rsbank_t *m = rsbank_load_full("mangler", "tests/fixtures/mg_default", "tests/fixtures/rs_presets", BMT_ROOT "/mangler");
    CHECK(r && m);
    if (!r || !m) { rsbank_free(r); rsbank_free(m); return; }
    CHECK(r->first_user == 10 && r->nbanks == 12);                              /* Default .. Mine, aardvark, Vocals (sorted) */
    CHECK(strcmp(r->name[0], "Default") == 0 && strcmp(r->name[9], "Mine") == 0);
    CHECK(strcmp(r->name[10], "aardvark") == 0 && strcmp(r->name[11], "Vocals") == 0);
    CHECK(r->count[10] == 0 && r->count[11] == 2 && strcmp(r->e[11][0].name, "One") == 0 && strcmp(r->e[11][1].name, "two") == 0);
    CHECK(m->first_user == 2 && m->nbanks == 3 && strcmp(m->name[0], "Default") == 0 && strcmp(m->name[1], "Imported") == 0 && strcmp(m->name[2], "Choirs") == 0);
    CHECK(m->count[2] == 1 && strcmp(m->e[2][0].name, "Choir") == 0);
    CHECK(m->count[3] == 0 && m->e[3] == NULL);                                  /* nothing beyond the list */
    rsbank_t *plain = rsbank_load("tests/fixtures/rs_default", "tests/fixtures/rs_presets");   /* the old calls: just the fixed banks */
    CHECK(plain && plain->nbanks == 10 && plain->first_user == 10);
    rsbank_free(plain);
    rsbank_free(r);
    rsbank_free(m);
    bmt_rmrf(BMT_ROOT);
}

static void test_bankmgr_ops(void)
{
    char b[8192], st[256], pl[4096], info[1024];
    bmt_rmrf(BMT_ROOT);
    vt_t *vt = dsp_init(VT_SAMPLE_RATE);
    bankmgr_t *bm = bankmgr_create(vt, "tests/fixtures/rs_default", "tests/fixtures/rs_presets", BMT_ROOT);
    CHECK(vt && bm);
    if (!vt || !bm) { bankmgr_destroy(bm); dsp_cleanup(vt); return; }
    bankmgr_set_grace(bm, 0);
    CHECK(dsp_get_param(vt, "rs_preset_count", b, sizeof b) > 0 && strcmp(b, "0") == 0);          /* nothing read yet */
    CHECK(bankmgr_pump(bm) == 0);                                                                  /* the first pump reads the lists */
    CHECK(dsp_get_param(vt, "rs_bank_info", info, sizeof info) > 0 && strstr(info, "\"first_user\":10") && strstr(info, "\"Mine\"]"));
    bankmgr_status(bm, st, sizeof st);
    CHECK(strstr(st, "\"rev\":1") != NULL);

    /* new: the bank is in Resynth's list and not in Mangler's */
    CHECK(bmt_cmd(bm, "new", "resynth", "Vocals", "", NULL, st, sizeof st) == 0 && bmt_status_ok(st));
    CHECK(dsp_get_param(vt, "rs_bank_info", info, sizeof info) > 0 && strstr(info, "\"Mine\",\"Vocals\"]"));
    dsp_set_param(vt, "engine", "Mangler");
    CHECK(dsp_get_param(vt, "rs_bank_info", info, sizeof info) > 0 && strstr(info, "\"banks\":[\"Default\",\"Imported\"]") && strstr(info, "\"first_user\":2"));
    dsp_set_param(vt, "engine", "Resynth");
    CHECK(bmt_exists(BMT_ROOT "/resynth/Vocals") && !bmt_exists(BMT_ROOT "/mangler/Vocals"));

    /* refused: a name that is a fixed bank, a path, a duplicate */
    bmt_cmd(bm, "new", "resynth", "Default", "", NULL, st, sizeof st);
    CHECK(!bmt_status_ok(st) && strstr(st, "error") != NULL);
    bmt_cmd(bm, "new", "resynth", "../evil", "", NULL, st, sizeof st);
    CHECK(!bmt_status_ok(st) && !bmt_exists(BMT_ROOT "/evil") && !bmt_exists("build/tests/evil"));
    bmt_cmd(bm, "new", "resynth", "Vocals", "", NULL, st, sizeof st);
    CHECK(!bmt_status_ok(st) && strstr(st, "already") != NULL);
    bmt_cmd(bm, "frobnicate", "resynth", "Vocals", "", NULL, st, sizeof st);
    CHECK(!bmt_status_ok(st));
    bmt_cmd(bm, "new", "nonsense", "Vocals", "", NULL, st, sizeof st);
    CHECK(!bmt_status_ok(st));

    /* add: a preset into the bank; a new bank is made by adding to it */
    CHECK(bmt_cmd(bm, "add", "resynth", "Vocals", "Wide One", bmt_rs("Wide One", 0.75, pl, sizeof pl), st, sizeof st) == 0 && bmt_status_ok(st));
    CHECK(bmt_exists(BMT_ROOT "/resynth/Vocals/Wide One.json"));
    CHECK(bmt_cmd(bm, "add", "resynth", "Fresh", "Tiny/One", bmt_rs("Tiny/One", 0.1, pl, sizeof pl), st, sizeof st) == 0 && bmt_status_ok(st));
    CHECK(bmt_exists(BMT_ROOT "/resynth/Fresh/Tiny_One.json"));                                    /* the file name is made safe, the preset keeps its name */
    CHECK(dsp_get_param(vt, "rs_bank_info", info, sizeof info) > 0 && strstr(info, "\"Mine\",\"Fresh\",\"Vocals\"]"));
    CHECK(dsp_set_param(vt, "rs_bank", "Vocals") == 0);                                              /* by name, yours too */
    CHECK(dsp_get_param(vt, "rs_bank_name", b, sizeof b) > 0 && strcmp(b, "Vocals") == 0);
    CHECK(dsp_get_param(vt, "rs_preset_count", b, sizeof b) > 0 && strcmp(b, "1") == 0);
    CHECK(dsp_set_param(vt, "rs_preset", "0") == 0);
    CHECK(dsp_get_param(vt, "hm_transform", b, sizeof b) > 0 && fabs(atof(b) - 0.75) < 1e-6);
    CHECK(dsp_get_param(vt, "rs_preset_name", b, sizeof b) > 0 && strcmp(b, "Wide One") == 0);
    CHECK(dsp_set_param(vt, "rs_bank", "11") == 0 && dsp_get_param(vt, "rs_bank_name", b, sizeof b) > 0 && strcmp(b, "Vocals") == 0);   /* by index too */
    CHECK(dsp_set_param(vt, "rs_bank", "99") == 0 && dsp_get_param(vt, "rs_bank_name", b, sizeof b) > 0 && strcmp(b, "Vocals") == 0);   /* clamped to the list */
    CHECK(dsp_set_param(vt, "rs_bank", "1.375") == 0 && dsp_get_param(vt, "rs_bank_name", b, sizeof b) > 0 && strcmp(b, "Imported") == 0);   /* the Move's knob writes fractions */
    CHECK(dsp_set_param(vt, "rs_bank", "0.000") == 0 && dsp_get_param(vt, "rs_bank_name", b, sizeof b) > 0 && strcmp(b, "Default") == 0);
    CHECK(dsp_set_param(vt, "rs_bank", "x1") == -1);
    CHECK(dsp_set_param(vt, "rs_bank", "Vocals") == 0);                                                 /* (back where the checks below expect it) */

    /* not this engine's preset; not a preset at all */
    bmt_cmd(bm, "add", "resynth", "Vocals", "Wrong", bmt_mg("Wrong", 3, pl, sizeof pl), st, sizeof st);
    CHECK(!bmt_status_ok(st) && !bmt_exists(BMT_ROOT "/resynth/Vocals/Wrong.json"));
    bmt_cmd(bm, "add", "resynth", "Vocals", "Junk", "not json", st, sizeof st);
    CHECK(!bmt_status_ok(st) && !bmt_exists(BMT_ROOT "/resynth/Vocals/Junk.json"));

    /* save: the sound as it is now, under a name; loading it brings the sound back */
    CHECK(dsp_set_param(vt, "hm_transform", "0.4") == 0);
    CHECK(bmt_cmd(bm, "save", "resynth", "Vocals", "Saved Sound", NULL, st, sizeof st) == 0 && bmt_status_ok(st));
    CHECK(bmt_exists(BMT_ROOT "/resynth/Vocals/Saved Sound.json"));
    CHECK(dsp_set_param(vt, "hm_transform", "0.9") == 0);
    CHECK(dsp_get_param(vt, "rs_preset_count", b, sizeof b) > 0 && strcmp(b, "2") == 0);
    CHECK(dsp_set_param(vt, "rs_preset", "0") == 0);                                                 /* sorted by name: "Saved Sound", then "Wide One" */
    CHECK(dsp_get_param(vt, "rs_preset_name", b, sizeof b) > 0 && strcmp(b, "Saved Sound") == 0);
    CHECK(dsp_get_param(vt, "hm_transform", b, sizeof b) > 0 && fabs(atof(b) - 0.4) < 1e-6);
    bmt_cmd(bm, "save", "mangler", "Vocals", "Wrong Engine", NULL, st, sizeof st);                    /* the sound is Resynth's */
    CHECK(!bmt_status_ok(st));
    bmt_cmd(bm, "save", "resynth", "Vocals", "bad\"name", NULL, st, sizeof st);
    CHECK(!bmt_status_ok(st));

    /* the Presets page keeps its place by name when a bank is renamed: you are on Vocals, it becomes Choirs */
    CHECK(bmt_cmd(bm, "rename", "resynth", "Vocals", "Choirs", NULL, st, sizeof st) == 0 && bmt_status_ok(st));
    CHECK(bmt_exists(BMT_ROOT "/resynth/Choirs/Wide One.json") && !bmt_exists(BMT_ROOT "/resynth/Vocals"));
    CHECK(dsp_get_param(vt, "rs_bank_name", b, sizeof b) > 0 && strcmp(b, "Choirs") == 0);
    CHECK(dsp_get_param(vt, "rs_preset_count", b, sizeof b) > 0 && strcmp(b, "2") == 0);
    bmt_cmd(bm, "rename", "resynth", "Choirs", "Fresh", NULL, st, sizeof st);                         /* onto a bank that exists */
    CHECK(!bmt_status_ok(st) && bmt_exists(BMT_ROOT "/resynth/Choirs"));
    bmt_cmd(bm, "rename", "resynth", "Choirs", "Imported", NULL, st, sizeof st);
    CHECK(!bmt_status_ok(st));

    /* the state carries the bank by name, and a restore finds it again once the lists are in */
    CHECK(dsp_get_param(vt, "state", b, sizeof b) > 0 && strstr(b, "\"rs_bank\":\"Choirs\"") != NULL);
    {
        vt_t *v2 = dsp_init(VT_SAMPLE_RATE);
        bankmgr_t *b2 = bankmgr_create(v2, "tests/fixtures/rs_default", "tests/fixtures/rs_presets", BMT_ROOT);
        bankmgr_set_grace(b2, 0);
        static char saved_state[8192];
        snprintf(saved_state, sizeof saved_state, "%s", b);
        dsp_set_param(v2, "state", saved_state);                                                      /* the lists are not in yet */
        CHECK(dsp_get_param(v2, "state", b, sizeof b) > 0 && strstr(b, "\"rs_bank\":\"Choirs\"") != NULL);   /* ...and a save now keeps the place */
        bankmgr_pump(b2);
        CHECK(dsp_get_param(v2, "rs_bank_name", b, sizeof b) > 0 && strcmp(b, "Choirs") == 0);
        bankmgr_destroy(b2);
        dsp_cleanup(v2);
    }

    /* remove one preset; delete the bank you are on: the page falls back to Default */
    CHECK(bmt_cmd(bm, "remove", "resynth", "Choirs", "Wide One", NULL, st, sizeof st) == 0 && bmt_status_ok(st));
    CHECK(!bmt_exists(BMT_ROOT "/resynth/Choirs/Wide One.json"));
    bmt_cmd(bm, "remove", "resynth", "Choirs", "Wide One", NULL, st, sizeof st);
    CHECK(!bmt_status_ok(st));
    CHECK(bmt_cmd(bm, "delete", "resynth", "Choirs", "", NULL, st, sizeof st) == 0 && bmt_status_ok(st));
    CHECK(!bmt_exists(BMT_ROOT "/resynth/Choirs"));
    CHECK(dsp_get_param(vt, "rs_bank_name", b, sizeof b) > 0 && strcmp(b, "Default") == 0);
    bmt_cmd(bm, "delete", "resynth", "Default", "", NULL, st, sizeof st);                              /* the fixed banks are not yours to delete */
    CHECK(!bmt_status_ok(st));
    bmt_cmd(bm, "delete", "resynth", "Nope", "", NULL, st, sizeof st);
    CHECK(!bmt_status_ok(st));
    CHECK(bmt_exists(BMT_ROOT "/resynth/Fresh/Tiny_One.json"));                                        /* the other bank is untouched */

    bankmgr_destroy(bm);
    dsp_cleanup(vt);
    bmt_rmrf(BMT_ROOT);
}

static void test_bankmgr_engines(void)
{
    /* Mangler's banks are its own: made, filled and chosen there, and invisible to Resynth */
    char b[2048], st[256], pl[4096], info[1024];
    bmt_rmrf(BMT_ROOT);
    vt_t *vt = dsp_init(VT_SAMPLE_RATE);
    bankmgr_t *bm = bankmgr_create(vt, "tests/fixtures/mg_default", "tests/fixtures/rs_presets", BMT_ROOT);
    if (!vt || !bm) { bankmgr_destroy(bm); dsp_cleanup(vt); CHECK(0); return; }
    bankmgr_set_grace(bm, 0);
    bankmgr_pump(bm);
    CHECK(bmt_cmd(bm, "add", "mangler", "Choirs", "Up", bmt_mg("Up", 5, pl, sizeof pl), st, sizeof st) == 0 && bmt_status_ok(st));
    CHECK(bmt_exists(BMT_ROOT "/mangler/Choirs/Up.json") && !bmt_exists(BMT_ROOT "/resynth/Choirs"));
    dsp_set_param(vt, "engine", "Mangler");
    CHECK(dsp_get_param(vt, "rs_bank_info", info, sizeof info) > 0 && strstr(info, "\"banks\":[\"Default\",\"Imported\",\"Choirs\"]"));
    {   /* the host's Bank picker lists this engine's banks, yours included (chain_params carries them at run time) */
        static char cp[32768];
        CHECK(dsp_get_param(vt, "chain_params", cp, sizeof cp) > 0 && strstr(cp, "\"key\":\"rs_bank\",\"name\":\"Bank\",\"type\":\"enum\",\"options\":[\"Default\",\"Imported\",\"Choirs\"]") != NULL && strstr(cp, "@B@") == NULL);
        CHECK(dsp_set_param(vt, "rs_bank", "2.0") == 0 && dsp_get_param(vt, "rs_bank", b, sizeof b) > 0 && strcmp(b, "Choirs") == 0);   /* and reads back by name */
    }
    CHECK(dsp_set_param(vt, "rs_bank", "Choirs") == 0 && dsp_set_param(vt, "rs_preset", "0") == 0);
    CHECK(dsp_get_param(vt, "mg_pitch", b, sizeof b) > 0 && fabs(atof(b) - 5.0) < 1e-6);
    CHECK(dsp_get_param(vt, "rs_bank_name", b, sizeof b) > 0 && strcmp(b, "Choirs") == 0);
    dsp_set_param(vt, "engine", "Resynth");                                                            /* Resynth has no Choirs: back to Default */
    CHECK(dsp_get_param(vt, "rs_bank_name", b, sizeof b) > 0 && strcmp(b, "Default") == 0);
    CHECK(dsp_get_param(vt, "rs_bank_info", info, sizeof info) > 0 && strstr(info, "Choirs") == NULL);
    bmt_cmd(bm, "add", "mangler", "Choirs", "Rs", bmt_rs("Rs", 0.5, pl, sizeof pl), st, sizeof st);   /* a Resynth preset into Mangler's bank */
    CHECK(!bmt_status_ok(st));

    /* the queue is a ring of 8; a full one refuses and says so */
    int refused = 0;
    for (int i = 0; i < 12; i++) refused += bankmgr_submit(bm, "new\nmangler\nQ\n") != 0;
    CHECK(refused == 4);
    bankmgr_status(bm, st, sizeof st);
    CHECK(strstr(st, "busy") != NULL);
    CHECK(bankmgr_pump(bm) == 8);
    CHECK(bankmgr_submit(bm, "new\nmangler\nQ2\n") == 0 && bankmgr_pump(bm) == 1);

    /* sixteen banks is the most */
    for (int i = 0; i < 20; i++) {
        char nm[16];
        snprintf(nm, sizeof nm, "B%02d", i);
        bmt_cmd(bm, "new", "mangler", nm, "", NULL, st, sizeof st);
    }
    CHECK(!bmt_status_ok(st) && strstr(st, "most") != NULL);
    dsp_set_param(vt, "engine", "Mangler");
    CHECK(dsp_get_param(vt, "rs_bank_info", info, sizeof info) > 0 && strstr(info, "\"max\":16") != NULL);
    {
        int quotes = 0;
        for (const char *p = info; *p; p++) quotes += *p == '"';
        CHECK(quotes == 2 * (2 + 16) + 8);                                                               /* the two fixed banks and 16 of yours, and the four keys */
    }

    bankmgr_destroy(bm);
    dsp_cleanup(vt);
    bmt_rmrf(BMT_ROOT);
}
