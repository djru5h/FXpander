/* FXpander harness: plays MPC's part (a VST2 host) against build/r5hfxpander.so, which hosts the test children
 * (testfx.c variants) and x86 builds of P-Valley PNG and YES-SL 9000. Prints one line per check; exit 1 on a failure.
 *   host <fxpander.so> <dir of the children>          (harness/run.sh builds everything and runs it under ASan) */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "params.h"

#include <stdarg.h>
#include <unistd.h>

#define NSLOTS 8
#define POOL 64
#if defined(__aarch64__)   /* the plugin list FXpander reads (see PLUGIN_LIST in src/fxpander.c) */
#define PLUGIN_LIST "pluginList-arm-64bit"
#else
#define PLUGIN_LIST "pluginList-arm"
#endif
enum { S_PICK, S_PREV, S_NEXT, S_LOAD, S_CLEAR, S_NAME, S_ON, S_MIX, S_UP, S_DOWN, S_DROP, S_N };
#define BLK 128

/* parameter indices by key, as the plugin finds them */
static int key_idx(const char *fmt, ...) {
    char k[64];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(k, sizeof k, fmt, ap);
    va_end(ap);
    for (int i = 0; i < NPARAMS; i++)
        if (!strcmp(PARAMS[i].key, k)) return i;
    fprintf(stderr, "no param %s\n", k);
    abort();
}
static int SLOT_PARAM(int s, int f) {
    static const char *suf[S_N] = {"pick", "pick_prev", "pick_next", "load", "clear", "name", "on", "mix", "up", "down", "drop"};
    return key_idx("s%d_%s", s + 1, suf[f]);
}
static int POOL_PARAM(int s, int k) { return key_idx("s%d_p%02d", s + 1, k + 1); }
static int PR(const char *f) { return key_idx("pr_%s", f); }
static int KB(const char *id) { return key_idx("kb_%s", id); }

typedef struct AEffect AEffect;
typedef intptr_t (*audioMasterCallback)(AEffect *, int32_t, int32_t, intptr_t, void *, float);
struct AEffect {
    int32_t magic;
    intptr_t (*dispatcher)(AEffect *, int32_t, int32_t, intptr_t, void *, float);
    void (*process)(AEffect *, float **, float **, int32_t);
    void (*setParameter)(AEffect *, int32_t, float);
    float (*getParameter)(AEffect *, int32_t);
    int32_t numPrograms, numParams, numInputs, numOutputs, flags;
    intptr_t resvd1, resvd2;
    int32_t initialDelay, realQualities, offQualities;
    float ioRatio;
    void *object, *user;
    int32_t uniqueID, version;
    void (*processReplacing)(AEffect *, float **, float **, int32_t);
    void (*processDoubleReplacing)(AEffect *, double **, double **, int32_t);
    char future[56];
};
typedef struct {
    double samplePos, sampleRate, nanoSeconds, ppqPos, tempo, barStartPos, cycleStartPos, cycleEndPos;
    int32_t timeSigNumerator, timeSigDenominator, smpteOffset, smpteFrameRate, samplesToNextClock, flags;
} VstTimeInfo;
typedef AEffect *(*vst_main_t)(audioMasterCallback);

static VstTimeInfo g_ti = {.sampleRate = 44100, .tempo = 97, .flags = 1 << 10 | 1 << 9};
static volatile int n_auto[NPARAMS], n_update, n_gettime;
static volatile float last_auto[NPARAMS];
static int fails, checks;

static intptr_t master(AEffect *e, int32_t op, int32_t idx, intptr_t v, void *p, float o) {
    switch (op) {
    case 0:   /* audioMasterAutomate: MPC calls setParameter back from inside it (mpc-vst-plugins NOTES 2026-10-08) */
        if (idx >= 0 && idx < NPARAMS) { n_auto[idx]++; last_auto[idx] = o; }
        if (e && idx >= 0 && idx < NPARAMS) e->setParameter(e, idx, o);
        return 0;
    case 1: return 2400;
    case 7: n_gettime++; return (intptr_t)&g_ti;
    case 32: strcpy(p, "Akai"); return 1;
    case 33: strcpy(p, "MPC"); return 1;
    case 42: n_update++; return 1;
    default: return 0;
    }
}

static void check(int ok, const char *what, const char *detail) {
    checks++;
    if (!ok) fails++;
    printf("%s  %s%s%s\n", ok ? "ok  " : "FAIL", what, detail && *detail ? ": " : "", detail ? detail : "");
}

/* a touch as MPC sends it: a button (momentary param) gets 1 when pressed and 1 again when the finger lifts (the
 * plugin has reported it back to 0 in between); a stepper arrow only on the release; anything else is set once */
static void tap(AEffect *e, int i) {
    const char *k = PARAMS[i].key;
    size_t n = strlen(k);
    int arrow = n > 5 && (!strcmp(k + n - 5, "_prev") || !strcmp(k + n - 5, "_next")) && strncmp(k, "dd_", 3);
    e->setParameter(e, i, 1);
    if (PARAMS[i].momentary && !arrow) e->setParameter(e, i, 1);   /* a stepper arrow fires on the release only */
}

static vst_main_t g_main;
static AEffect *open_fx(void) {
    AEffect *e = g_main(master);
    e->dispatcher(e, 0, 0, 0, NULL, 0);
    e->dispatcher(e, 10, 0, 0, NULL, 44100);
    e->dispatcher(e, 11, 0, BLK, NULL, 0);
    e->dispatcher(e, 12, 0, 1, NULL, 0);
    e->dispatcher(e, 71, 0, 0, NULL, 0);
    return e;
}
static void close_fx(AEffect *e) { e->dispatcher(e, 12, 0, 0, NULL, 0); e->dispatcher(e, 1, 0, 0, NULL, 0); }
static const char *disp_txt(AEffect *e, int i) { static char b[8][256]; static int r; char *o = b[r++ % 8]; memset(o, 0, 256); e->dispatcher(e, 7, i, 0, o, 0); return o; }
static const char *name_txt(AEffect *e, int i) { static char b[8][256]; static int r; char *o = b[r++ % 8]; memset(o, 0, 256); e->dispatcher(e, 8, i, 0, o, 0); return o; }

/* run nblk blocks of a 220 Hz sine at amplitude amp (L = R) through e; returns out/in RMS of the last block's L
 * (and R in *rr); *last gets the last block's left output for comparisons */
static float in_l[4096], in_r[4096], out_l[4096], out_r[4096];
static long g_phase;
static float run_blocks(AEffect *e, int nblk, int n, float amp, float *rr) {
    float *in[2] = {in_l, in_r}, *out[2] = {out_l, out_r};
    double si = 0, so = 0, sr = 0;
    for (int b = 0; b < nblk; b++) {
        for (int i = 0; i < n; i++) in_l[i] = in_r[i] = amp * (float)sin(2 * M_PI * 220 * (g_phase + i) / 44100.0);
        g_phase += n;
        e->processReplacing(e, in, out, n);
        if (b == nblk - 1) {
            si = so = sr = 0;
            for (int i = 0; i < n; i++) { si += in_l[i] * in_l[i]; so += out_l[i] * out_l[i]; sr += out_r[i] * out_r[i]; }
        }
    }
    if (rr) *rr = si > 0 ? (float)sqrt(sr / si) : (float)sqrt(sr / n);
    return si > 0 ? (float)sqrt(so / si) : (float)sqrt(so / n);
}

static int pick_by_name(AEffect *e, int s, const char *want) {
    for (int t = 0; t < 140; t++) {
        if (!strcmp(disp_txt(e, SLOT_PARAM(s, S_PICK)), want)) return 1;
        tap(e, SLOT_PARAM(s, S_NEXT));
    }
    return 0;
}
static int load(AEffect *e, int s, const char *want) {
    if (!pick_by_name(e, s, want)) return 0;
    tap(e, SLOT_PARAM(s, S_LOAD));
    return 1;
}
static int near(float a, float b, float tol) { return fabsf(a - b) <= tol; }

/* concurrent audio for the stress test */
static volatile int g_stop;
static void *audio_thread(void *arg) {
    AEffect *e = arg;
    static float il[BLK], ir[BLK], ol[BLK], orr[BLK];
    float *in[2] = {il, ir}, *out[2] = {ol, orr};
    long ph = 0;
    while (!g_stop) {
        for (int i = 0; i < BLK; i++) il[i] = ir[i] = 0.3f * (float)sin(0.05 * (ph + i));
        ph += BLK;
        e->processReplacing(e, in, out, BLK);
        for (int i = 0; i < BLK; i++)
            if (!isfinite(ol[i]) || !isfinite(orr[i])) { fprintf(stderr, "non-finite output\n"); abort(); }
    }
    return NULL;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: host <fxpander.so> <children dir>\n"); return 2; }
    const char *dir = argv[2];
    char path[600], detail[300];
    /* a fake MPC.settings: what's listed, what isn't (instrument, missing file, not VST format, FXpander itself) */
    snprintf(path, sizeof path, "%s/test.settings", dir);
    FILE *f = fopen(path, "w");
    fprintf(f, "<PROPERTIES>\n  <VALUE name=\"" PLUGIN_LIST "\">\n    <KNOWNPLUGINS>\n");
    struct { const char *name, *so, *uid, *inst, *fmt; } ents[] = {
        {"Gain FX", "testfx_gain.so", "54476e31", "0", "VST"},
        {"&#201;mile Chunk", "testfx_chunk.so", "54476e32", "0", "VST"},
        {"Mono FX", "testfx_mono.so", "54476e33", "0", "VST"},
        {"Synth X", "testfx_synth.so", "54476e34", "1", "VST"},
        {"Lying Synth", "testfx_synth.so", "54476e34", "0", "VST"},
        {"Missing FX", "nope.so", "54476e35", "0", "VST"},
        {"FXpander", "r5hfxpander.so", "52354658", "0", "VST"},
        {"AU Thing", "testfx_gain.so", "54476e36", "0", "AudioUnit"},
        {"Many Params", "testfx_many.so", "54476e37", "0", "VST"},
        {"Not VST", "notvst.so", "54476e38", "0", "VST"},
        {"P-Valley PNG", "pvalley.so", "52354e5a", "0", "VST"},
        {"YES-SL 9000", "yessl.so", "5235394a", "0", "VST"},
    };
    for (size_t i = 0; i < sizeof ents / sizeof ents[0]; i++)
        fprintf(f, "      <PLUGIN name=\"%s\" descriptiveName=\"%s\" format=\"%s\"\n              category=\"Effect\" manufacturer=\"t\" version=\"1.0\" file=\"%s/%s\"\n              uid=\"%s\" isInstrument=\"%s\" fileTime=\"0\" infoUpdateTime=\"0\"\n              numInputs=\"2\" numOutputs=\"2\" isShell=\"0\"/>\n",
                ents[i].name, ents[i].name, ents[i].fmt, dir, ents[i].so, ents[i].uid, ents[i].inst);
    fprintf(f, "    </KNOWNPLUGINS>\n  </VALUE>\n</PROPERTIES>\n");
    fclose(f);
    setenv("FXPANDER_SETTINGS", path, 1);

    snprintf(path, sizeof path, "%s/presets", dir);
    setenv("FXPANDER_PRESETS", path, 1);

    /* FXpander 1000's indices are unchanged (saved projects and automation store values by index) */
    int keys_ok = !strcmp(PARAMS[0].key, "status") && NPARAMS == 706;
    for (int s = 0; s < 4; s++) {
        for (int fi = 0; fi < S_MIX + 1; fi++) keys_ok &= SLOT_PARAM(s, fi) == 1 + s * 8 + fi;
        for (int q = 0; q < POOL; q++) keys_ok &= POOL_PARAM(s, q) == 33 + s * 64 + q;
    }
    check(keys_ok, "version 1000's parameter indices kept", "");

    void *lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) { fprintf(stderr, "%s\n", dlerror()); return 1; }
    g_main = (vst_main_t)dlsym(lib, "VSTPluginMain");
    AEffect *a = open_fx();
    check(a && a->magic == 0x56737450 && a->numParams == NPARAMS && a->numInputs == 2 && a->uniqueID == PLUG_UID, "opens as a stereo effect", "");
    check(!strcmp(disp_txt(a, 0), "8 effects available"), "status counts the listed effects", disp_txt(a, 0));

    /* 1. the browse list: sorted, entities decoded, the unlisted ones left out */
    char seen[600] = "";
    for (int t = 0; t < 10; t++) { strcat(seen, disp_txt(a, SLOT_PARAM(0, S_PICK))); strcat(seen, "|"); tap(a, SLOT_PARAM(0, S_NEXT)); }
    check(!strcmp(seen, "- Empty -|Gain FX|Lying Synth|Many Params|Mono FX|Not VST|P-Valley PNG|YES-SL 9000|\xc3\x89mile Chunk|- Empty -|"),
          "browse list (sorted, wraps, entities decoded)", seen);
    a->setParameter(a, SLOT_PARAM(0, S_PICK), 0);
    tap(a, SLOT_PARAM(0, S_PREV));
    check(!strcmp(disp_txt(a, SLOT_PARAM(0, S_PICK)), "\xc3\x89mile Chunk"), "prev wraps to the end", disp_txt(a, SLOT_PARAM(0, S_PICK)));
    /* data wheel / Q-Link nudges step one entry; a jump sets */
    a->setParameter(a, SLOT_PARAM(0, S_PICK), 0);
    float cur = a->getParameter(a, SLOT_PARAM(0, S_PICK));
    a->setParameter(a, SLOT_PARAM(0, S_PICK), cur + 0.01f);
    check(!strcmp(disp_txt(a, SLOT_PARAM(0, S_PICK)), "Gain FX"), "a wheel click moves one entry", disp_txt(a, SLOT_PARAM(0, S_PICK)));
    cur = a->getParameter(a, SLOT_PARAM(0, S_PICK));
    a->setParameter(a, SLOT_PARAM(0, S_PICK), cur + 1.0f / 128);
    check(!strcmp(disp_txt(a, SLOT_PARAM(0, S_PICK)), "Lying Synth"), "a Q-Link event moves one entry", disp_txt(a, SLOT_PARAM(0, S_PICK)));
    tap(a, SLOT_PARAM(0, S_PICK));
    check(!strcmp(disp_txt(a, SLOT_PARAM(0, S_PICK)), "\xc3\x89mile Chunk"), "a jump past the end clamps to the last", "");
    a->setParameter(a, SLOT_PARAM(0, S_PICK), 0);

    /* 2. load a child: names, mapping (the meter is skipped), texts with units */
    int up0 = n_update;
    check(load(a, 0, "Gain FX"), "Gain FX found in the list", "");
    check(!strcmp(disp_txt(a, SLOT_PARAM(0, S_NAME)), "Gain FX"), "slot name readout", disp_txt(a, SLOT_PARAM(0, S_NAME)));
    check(!strcmp(disp_txt(a, 0), "FX 1: Gain FX (3 of 4 params)"), "status after load", disp_txt(a, 0));
    snprintf(detail, sizeof detail, "%s|%s|%s|%s", name_txt(a, POOL_PARAM(0, 0)), name_txt(a, POOL_PARAM(0, 1)), name_txt(a, POOL_PARAM(0, 2)), name_txt(a, POOL_PARAM(0, 3)));
    check(!strcmp(detail, "Gain|Tempo|Poke|"), "knob names are the child's automatable params", detail);
    check(!strcmp(disp_txt(a, POOL_PARAM(0, 0)), "1.00 x"), "knob value text with the child's unit", disp_txt(a, POOL_PARAM(0, 0)));
    check(!strcmp(disp_txt(a, POOL_PARAM(0, 3)), "") && a->getParameter(a, POOL_PARAM(0, 3)) == 0, "an unmapped knob is blank", "");

    /* 3. audio: gain through the child, after the fade-in */
    a->setParameter(a, POOL_PARAM(0, 0), 0.25f);   /* x0.5 */
    check(near(a->getParameter(a, POOL_PARAM(0, 0)), 0.25f, 1e-6f), "a knob sets and reads back the child's param", "");
    run_blocks(a, 1, BLK, 0.5f, NULL);
    check(n_update > up0 && n_auto[SLOT_PARAM(0, S_LOAD)] > 0 && last_auto[SLOT_PARAM(0, S_LOAD)] == 0,
          "load reports the trigger back to 0 and asks for a redraw", "");
    float rr, g = run_blocks(a, 3, BLK, 0.5f, &rr);
    snprintf(detail, sizeof detail, "L %.4f R %.4f", g, rr);
    check(near(g, 0.5f, 1e-4f) && near(rr, 0.5f, 1e-4f), "slot 1 at x0.5", detail);
    check(!strcmp(disp_txt(a, POOL_PARAM(0, 1)), "97.0"), "the child sees MPC's tempo (audioMasterGetTime)", disp_txt(a, POOL_PARAM(0, 1)));

    /* 4. a child moving its own param reaches MPC as our knob */
    int na = n_auto[POOL_PARAM(0, 0)];
    tap(a, POOL_PARAM(0, 2));   /* Poke: the child sets Gain to 0.25 and automates it */
    run_blocks(a, 1, BLK, 0.5f, NULL);
    snprintf(detail, sizeof detail, "%d reports, last %.3f", n_auto[POOL_PARAM(0, 0)] - na, last_auto[POOL_PARAM(0, 0)]);
    check(n_auto[POOL_PARAM(0, 0)] > na && near(last_auto[POOL_PARAM(0, 0)], 0.25f, 1e-6f), "a child's own automate is reported on its knob", detail);

    /* 5. in series, bypass, mix */
    check(load(a, 2, "Gain FX"), "a second Gain FX in slot 3", "");
    a->setParameter(a, POOL_PARAM(2, 0), 0.75f);   /* x1.5 */
    g = run_blocks(a, 4, BLK, 0.5f, NULL);
    snprintf(detail, sizeof detail, "%.4f", g);
    check(near(g, 0.75f, 1e-4f), "slots 1 and 3 in series (x0.5 x1.5)", detail);
    a->setParameter(a, SLOT_PARAM(0, S_ON), 0);
    check(!strcmp(disp_txt(a, SLOT_PARAM(0, S_ON)), "Off"), "On reads Off", "");
    g = run_blocks(a, 3, BLK, 0.5f, NULL);
    snprintf(detail, sizeof detail, "%.4f", g);
    check(near(g, 1.5f, 1e-4f), "slot 1 off: only slot 3", detail);
    tap(a, SLOT_PARAM(0, S_ON));
    a->setParameter(a, SLOT_PARAM(2, S_MIX), 0.5f);
    check(!strcmp(disp_txt(a, SLOT_PARAM(2, S_MIX)), "50%"), "mix text", disp_txt(a, SLOT_PARAM(2, S_MIX)));
    g = run_blocks(a, 3, BLK, 0.5f, NULL);
    snprintf(detail, sizeof detail, "%.4f", g);
    check(near(g, 0.5f * 1.25f, 1e-4f), "slot 3 at 50% mix", detail);
    /* the ramp: from x0.5*1.25 to x0.5*1.5 over one block, no jump at the block start */
    tap(a, SLOT_PARAM(2, S_MIX));
    float prev = out_l[BLK - 1], *in[2] = {in_l, in_r}, *out[2] = {out_l, out_r};
    for (int i = 0; i < BLK; i++) in_l[i] = in_r[i] = 0.5f;
    a->processReplacing(a, in, out, BLK);
    snprintf(detail, sizeof detail, "first %.4f last %.4f", out_l[0], out_l[BLK - 1]);
    check(near(out_l[0], 0.5f * 0.5f * 1.25f, 0.01f) && near(out_l[BLK - 1], 0.5f * 0.5f * 1.5f, 1e-4f), "mix changes ramp across a block", detail);
    (void)prev;

    /* 6. big host blocks run in pieces */
    g = run_blocks(a, 2, 3000, 0.5f, NULL);
    snprintf(detail, sizeof detail, "%.4f", g);
    check(near(g, 0.75f, 1e-4f), "a 3000-frame block", detail);

    /* 7. refusals keep what's loaded */
    check(load(a, 2, "Lying Synth"), "Lying Synth listed", "");
    check(!strcmp(disp_txt(a, 0), "FX 3: Lying Synth is an instrument") && !strcmp(disp_txt(a, SLOT_PARAM(2, S_NAME)), "Gain FX"),
          "an instrument is refused, slot unchanged", disp_txt(a, 0));
    check(load(a, 2, "Not VST"), "Not VST listed", "");
    check(!strcmp(disp_txt(a, 0), "FX 3: Not VST is not a VST2 plugin"), "a non-VST library is refused", disp_txt(a, 0));

    /* 8. more params than knobs: the first 64 automatable */
    check(load(a, 3, "Many Params"), "Many Params listed", "");
    check(!strcmp(disp_txt(a, 0), "FX 4: Many Params (64 of 104 params)"), "64 of 104 shown", disp_txt(a, 0));
    check(!strcmp(name_txt(a, POOL_PARAM(3, 63)), "Extra 61") && !strcmp(name_txt(a, POOL_PARAM(3, 1)), "Tempo"), "the 64th knob is the 65th param", name_txt(a, POOL_PARAM(3, 63)));

    /* 9. mono child and a chunk child */
    check(load(a, 3, "Mono FX"), "Mono FX in slot 4", "");
    a->setParameter(a, POOL_PARAM(3, 0), 0.5f);
    check(load(a, 1, "\xc3\x89mile Chunk"), "the chunk child in slot 2", "");
    a->setParameter(a, POOL_PARAM(1, 0), 0.1f);    /* x0.2 */
    g = run_blocks(a, 4, BLK, 0.5f, &rr);
    snprintf(detail, sizeof detail, "L %.4f R %.4f", g, rr);
    check(near(g, 0.5f * 0.2f * 1.5f, 1e-4f) && near(rr, g, 1e-5f), "four slots incl. a mono child (both sides)", detail);

    /* 10. save and restore into a fresh instance */
    void *chunk = NULL;
    intptr_t clen = a->dispatcher(a, 23, 0, 0, &chunk, 0);
    check(clen > 16 && chunk && !memcmp(chunk, "FXP1", 4), "chunk saved", "");
    unsigned char *saved = malloc(clen);
    memcpy(saved, chunk, clen);
    AEffect *b = open_fx();
    check(b->dispatcher(b, 24, 0, clen, saved, 0) == 1, "chunk restored", "");
    snprintf(detail, sizeof detail, "%s|%s|%s|%s", disp_txt(b, SLOT_PARAM(0, S_NAME)), disp_txt(b, SLOT_PARAM(1, S_NAME)), disp_txt(b, SLOT_PARAM(2, S_NAME)), disp_txt(b, SLOT_PARAM(3, S_NAME)));
    check(!strcmp(detail, "Gain FX|\xc3\x89mile Chunk|Gain FX|Mono FX"), "same plugins after restore", detail);
    check(!strcmp(disp_txt(b, SLOT_PARAM(1, S_PICK)), "\xc3\x89mile Chunk"), "browse follows the restored plugin", disp_txt(b, SLOT_PARAM(1, S_PICK)));
    int same = 1;
    for (int s = 0; s < NSLOTS; s++)
        for (int k = 0; k < 4; k++) same &= a->getParameter(a, POOL_PARAM(s, k)) == b->getParameter(b, POOL_PARAM(s, k));
    same &= b->getParameter(b, SLOT_PARAM(2, S_MIX)) == 1 && b->getParameter(b, SLOT_PARAM(0, S_ON)) == 1;
    check(same, "same values after restore (param list and chunk children)", "");
    long ph = g_phase;
    float ga = run_blocks(a, 4, BLK, 0.5f, NULL), la = out_l[BLK - 1];
    g_phase = ph;
    float gb = run_blocks(b, 4, BLK, 0.5f, NULL), lb = out_l[BLK - 1];
    snprintf(detail, sizeof detail, "%.5f vs %.5f", ga, gb);
    check(ga == gb && la == lb, "same sound after restore", detail);
    /* a restore onto the same plugins only sets their values */
    b->setParameter(b, POOL_PARAM(0, 0), 0.9f);
    b->dispatcher(b, 24, 0, clen, saved, 0);
    check(b->getParameter(b, POOL_PARAM(0, 0)) == a->getParameter(a, POOL_PARAM(0, 0)), "restore over the same plugin resets its values", "");
    close_fx(b);
    free(saved);

    /* 11. a saved plugin that's gone: kept, and saved again unchanged */
    {
        unsigned char m[256];
        size_t n = 0;
        uint32_t u;
        float one = 1, v = 0.3f;
        uint16_t l;
        uint8_t c;
#define PUT(x, sz) (memcpy(m + n, (x), (sz)), n += (sz))
        PUT("FXP1", 4); u = 1; PUT(&u, 4); u = 4; PUT(&u, 4);
        c = 1; PUT(&c, 1); PUT(&one, 4); u = 0x11223344; PUT(&u, 4);
        l = 14; PUT(&l, 2); PUT("/nope/ghost.so", 14); l = 5; PUT(&l, 2); PUT("Ghost", 5);
        c = 2; PUT(&c, 1); u = 8; PUT(&u, 4); u = 1; PUT(&u, 4); PUT(&v, 4);
        for (int s = 1; s < 4; s++) { c = 1; PUT(&c, 1); PUT(&one, 4); u = 0; PUT(&u, 4); l = 0; PUT(&l, 2); PUT(&l, 2); c = 0; PUT(&c, 1); PUT(&u, 4); }
        AEffect *cfx = open_fx();
        cfx->dispatcher(cfx, 24, 0, (intptr_t)n, m, 0);
        check(!strcmp(disp_txt(cfx, SLOT_PARAM(0, S_NAME)), "Ghost (missing)") && strstr(disp_txt(cfx, 0), "can't open Ghost"),
              "a missing plugin shows as missing", disp_txt(cfx, 0));
        void *ck = NULL;
        intptr_t ckl = cfx->dispatcher(cfx, 23, 0, 0, &ck, 0);
        uint32_t ver;
        memcpy(&ver, (char *)ck + 4, 4);
        /* a version 1 (4-slot) chunk read fine; saved again as version 2 with the missing slot's bytes unchanged
         * (they start after magic, version, slot count, on and mix: offset 17, 40 bytes) */
        check(ckl > (intptr_t)n && ver == 2 && !memcmp((char *)ck + 17, m + 17, 40), "and is saved again byte for byte", "");
        g = run_blocks(cfx, 2, BLK, 0.5f, NULL);
        check(near(g, 1, 1e-6f), "an empty chain passes audio unchanged", "");
        close_fx(cfx);
    }

    /* 12. clear */
    tap(a, SLOT_PARAM(2, S_CLEAR));
    check(!strcmp(disp_txt(a, SLOT_PARAM(2, S_NAME)), "Empty") && !strcmp(disp_txt(a, SLOT_PARAM(2, S_PICK)), "- Empty -") &&
          !strcmp(name_txt(a, POOL_PARAM(2, 0)), ""), "clear empties the slot", "");
    g = run_blocks(a, 3, BLK, 0.5f, NULL);
    snprintf(detail, sizeof detail, "%.4f", g);
    check(near(g, 0.5f * 0.2f, 1e-4f), "and its effect is gone", detail);

    /* 13. real plugins built on the framework: P-Valley PNG and YES-SL 9000 (run.sh puts gain stand-ins under their
     * names when their repos aren't next to this one, and sets FXPANDER_NO_REAL) */
    if (getenv("FXPANDER_NO_REAL")) printf("skip  13. P-Valley PNG / YES-SL 9000 (their repos aren't checked out)\n");
    else {
    check(load(a, 0, "P-Valley PNG"), "P-Valley PNG listed", "");
    check(strstr(disp_txt(a, 0), "P-Valley PNG") != NULL, "P-Valley PNG loads", disp_txt(a, 0));
    snprintf(detail, sizeof detail, "%s = %s, %s = %s", name_txt(a, POOL_PARAM(0, 1)), disp_txt(a, POOL_PARAM(0, 1)), name_txt(a, POOL_PARAM(0, 2)), disp_txt(a, POOL_PARAM(0, 2)));
    check(!strcmp(name_txt(a, POOL_PARAM(0, 1)), "Color") && !strcmp(disp_txt(a, POOL_PARAM(0, 1)), "Pink") &&
          !strcmp(disp_txt(a, POOL_PARAM(0, 2)), "-24.0 dB"), "its names and value texts (unit not doubled)", detail);
    a->setParameter(a, SLOT_PARAM(1, S_ON), 0);
    a->setParameter(a, SLOT_PARAM(3, S_ON), 0);
    double sq = 0;   /* silence in: the noise alone, RMS of the output over 1.5 s (a block is too short for pink) */
    for (int t = 0; t < 520; t++) {
        float rb = run_blocks(a, 1, BLK, 0, NULL);
        if (t >= 8) sq += rb * rb;
    }
    g = (float)sqrt(sq / 512);
    snprintf(detail, sizeof detail, "RMS %.4f (-24 dBFS = 0.0631)", g);
    check(g > 0.04f && g < 0.09f, "its pink noise comes out", detail);
    check(load(a, 1, "YES-SL 9000"), "YES-SL 9000 listed", "");
    tap(a, SLOT_PARAM(1, S_ON));
    snprintf(detail, sizeof detail, "%s; knob 1 %s = %s", disp_txt(a, 0), name_txt(a, POOL_PARAM(1, 0)), disp_txt(a, POOL_PARAM(1, 0)));
    check(strstr(disp_txt(a, 0), "YES-SL 9000") && *name_txt(a, POOL_PARAM(1, 0)), "YES-SL 9000 loads with named knobs", detail);
    int finite = 1;
    for (int t = 0; t < 50; t++) {
        run_blocks(a, 1, BLK, 0.5f, NULL);
        for (int i = 0; i < BLK; i++) finite &= isfinite(out_l[i]) && isfinite(out_r[i]);
    }
    check(finite, "both play, output finite", "");
    a->setParameter(a, POOL_PARAM(1, 0), 0.8f);
    char before[64];
    snprintf(before, sizeof before, "%s", disp_txt(a, POOL_PARAM(1, 0)));
    clen = a->dispatcher(a, 23, 0, 0, &chunk, 0);
    saved = malloc(clen);
    memcpy(saved, chunk, clen);
    b = open_fx();
    b->dispatcher(b, 24, 0, clen, saved, 0);
    snprintf(detail, sizeof detail, "%s -> %s", before, disp_txt(b, POOL_PARAM(1, 0)));
    check(!strcmp(before, disp_txt(b, POOL_PARAM(1, 0))), "YES-SL's setting survives save/restore (its own chunk)", detail);
    close_fx(b);
    free(saved);
    }

    /* 14. eight slots, moving slots */
    AEffect *d = open_fx();
    load(d, 0, "Gain FX");
    d->setParameter(d, POOL_PARAM(0, 0), 0.25f);    /* x0.5 */
    check(load(d, 7, "Mono FX"), "slot 8 loads", disp_txt(d, 0));
    d->setParameter(d, POOL_PARAM(7, 0), 0.75f);    /* x1.5 */
    g = run_blocks(d, 3, BLK, 0.5f, NULL);
    snprintf(detail, sizeof detail, "%.4f", g);
    check(near(g, 0.75f, 1e-4f), "slots 1 and 8 in series", detail);
    tap(d, SLOT_PARAM(0, S_DOWN));
    snprintf(detail, sizeof detail, "%s|%s; %s", disp_txt(d, SLOT_PARAM(0, S_NAME)), disp_txt(d, SLOT_PARAM(1, S_NAME)), disp_txt(d, 0));
    check(!strcmp(detail, "Empty|Gain FX; Gain FX moved to FX 2"), "DN moves FX 1 to FX 2", detail);
    check(d->getParameter(d, POOL_PARAM(1, 0)) == 0.25f && !strcmp(name_txt(d, POOL_PARAM(1, 0)), "Gain") && !strcmp(name_txt(d, POOL_PARAM(0, 0)), ""),
          "its knobs and settings move with it", "");
    g = run_blocks(d, 3, BLK, 0.5f, NULL);
    snprintf(detail, sizeof detail, "%.4f", g);
    check(near(g, 0.75f, 1e-4f), "and it still plays", detail);
    na = n_auto[POOL_PARAM(1, 0)];
    tap(d, POOL_PARAM(1, 2));   /* Poke: the moved child automates its Gain */
    run_blocks(d, 20, BLK, 0.5f, NULL);   /* the move queued both slots: 16 reports a block */
    check(n_auto[POOL_PARAM(1, 0)] > na, "a moved plugin's own automates report on its new knobs", "");
    tap(d, SLOT_PARAM(0, S_UP));
    tap(d, SLOT_PARAM(7, S_DOWN));
    check(!strcmp(disp_txt(d, SLOT_PARAM(0, S_NAME)), "Empty") && !strcmp(disp_txt(d, SLOT_PARAM(7, S_NAME)), "Mono FX"), "UP on FX 1 and DN on FX 8 do nothing", "");
    tap(d, SLOT_PARAM(7, S_UP));
    d->setParameter(d, SLOT_PARAM(6, S_MIX), 0.5f);
    check(!strcmp(disp_txt(d, SLOT_PARAM(6, S_NAME)), "Mono FX") && !strcmp(disp_txt(d, SLOT_PARAM(7, S_NAME)), "Empty") &&
          d->getParameter(d, POOL_PARAM(6, 0)) == 0.75f, "UP moves FX 8 to FX 7", "");
    tap(d, SLOT_PARAM(6, S_MIX));

    /* 15. chain presets: name on the keyboard, save, recall from the page and from MPC's PRESET menu */
    check(d->numPrograms == 33 && !strcmp(disp_txt(d, PR("slot")), "01 -") && !strcmp(disp_txt(d, PR("info")), "empty slot"),
          "33 programs; preset slots start empty", disp_txt(d, PR("slot")));
    for (const char *k = "dub x"; *k; k++) tap(d, KB(*k == ' ' ? "space" : (char[2]){*k, 0}));
    tap(d, KB("del"));
    tap(d, KB("1"));
    check(!strcmp(disp_txt(d, PR("name")), "DUB 1_"), "the keyboard types the name", disp_txt(d, PR("name")));
    tap(d, PR("slot_next"));
    tap(d, PR("slot_next"));
    tap(d, PR("save"));
    snprintf(path, sizeof path, "%s/presets/03.fxchain", dir);
    snprintf(detail, sizeof detail, "%s; %s; %s", disp_txt(d, 0), disp_txt(d, PR("slot")), disp_txt(d, PR("info")));
    check(access(path, R_OK) == 0 && !strcmp(detail, "Saved preset 03 DUB 1; 03 DUB 1; Gain FX > Mono FX"), "SAVE writes preset 03", detail);
    char pn[64] = "";
    d->dispatcher(d, 29, 3, 0, pn, 0);
    check(!strcmp(pn, "03 DUB 1"), "it's in MPC's PRESET menu", pn);
    tap(d, SLOT_PARAM(1, S_CLEAR));
    tap(d, SLOT_PARAM(6, S_CLEAR));
    tap(d, PR("recall"));
    snprintf(detail, sizeof detail, "%s|%s; %s", disp_txt(d, SLOT_PARAM(1, S_NAME)), disp_txt(d, SLOT_PARAM(6, S_NAME)), disp_txt(d, 0));
    check(!strcmp(detail, "Gain FX|Mono FX; Recalled preset 03 DUB 1") && d->dispatcher(d, 3, 0, 0, NULL, 0) == 3, "RECALL brings the chain back", detail);
    g = run_blocks(d, 3, BLK, 0.5f, NULL);
    snprintf(detail, sizeof detail, "%.4f", g);
    check(near(g, 0.75f, 1e-4f) && d->getParameter(d, POOL_PARAM(1, 0)) == 0.25f, "with its settings", detail);
    d->setParameter(d, POOL_PARAM(1, 0), 0.9f);
    d->dispatcher(d, 2, 0, 3, NULL, 0);
    check(d->getParameter(d, POOL_PARAM(1, 0)) == 0.9f, "a re-pick of the current program is ignored (JUCE does it at load)", "");
    d->dispatcher(d, 2, 0, 0, NULL, 0);
    check(d->getParameter(d, POOL_PARAM(1, 0)) == 0.9f, "program 0 is a heading: picking it changes nothing", "");
    d->dispatcher(d, 2, 0, 3, NULL, 0);
    check(d->getParameter(d, POOL_PARAM(1, 0)) == 0.25f && !strcmp(disp_txt(d, PR("slot")), "03 DUB 1"), "picking preset 3 in the PRESET menu recalls it", "");
    tap(d, KB("clear"));
    tap(d, PR("slot_next"));
    tap(d, PR("save"));
    check(!strcmp(disp_txt(d, PR("slot")), "04 Gain FX > Mono FX"), "no name typed: named after its contents", disp_txt(d, PR("slot")));
    clen = d->dispatcher(d, 23, 0, 0, &chunk, 0);
    saved = malloc(clen);
    memcpy(saved, chunk, clen);
    b = open_fx();
    b->dispatcher(b, 24, 0, clen, saved, 0);
    b->setParameter(b, POOL_PARAM(1, 0), 0.9f);
    b->dispatcher(b, 2, 0, 3, NULL, 0);
    pn[0] = 0;
    b->dispatcher(b, 29, 4, 0, pn, 0);
    check(b->dispatcher(b, 3, 0, 0, NULL, 0) == 3 && b->getParameter(b, POOL_PARAM(1, 0)) == 0.9f,
          "a project remembers its program, so JUCE's re-pick at load doesn't overwrite it", "");
    check(!strcmp(pn, "04 Gain FX > Mono FX"), "another instance sees the presets", pn);
    close_fx(b);
    free(saved);
    tap(d, PR("delete"));
    snprintf(path, sizeof path, "%s/presets/04.fxchain", dir);
    check(access(path, F_OK) != 0 && !strcmp(disp_txt(d, PR("slot")), "04 -"), "DELETE removes preset 04", disp_txt(d, 0));
    tap(d, PR("recall"));
    check(!strcmp(disp_txt(d, 0), "Preset 04 is empty") && !strcmp(disp_txt(d, SLOT_PARAM(1, S_NAME)), "Gain FX"), "recalling an empty slot changes nothing", disp_txt(d, 0));
    close_fx(d);

    /* 16. the drop-down plugin list */
    {
        AEffect *q = open_fx();
        int shown = key_idx("dd_shown");
        int sa = n_auto[shown];
        tap(q, SLOT_PARAM(2, S_DROP));
        run_blocks(q, 1, BLK, 0.5f, NULL);
        snprintf(detail, sizeof detail, "%s; reported %d, last %.0f", disp_txt(q, key_idx("dd_title")), n_auto[shown] - sa, last_auto[shown]);
        check(q->getParameter(q, shown) == 1 && n_auto[shown] > sa && last_auto[shown] == 1 && !strcmp(disp_txt(q, key_idx("dd_title")), "Choose FX 3"),
              "FX 3's field opens the list (reported first)", detail);
        snprintf(detail, sizeof detail, "%s|%s|%s", disp_txt(q, key_idx("dd_1")), disp_txt(q, key_idx("dd_8")), disp_txt(q, key_idx("dd_9")));
        check(!strcmp(detail, "Gain FX|\xc3\x89mile Chunk|"), "its rows are the plugin list", detail);
        /* MPC echoes every report back as setParameter (this host does too): the open list's rows are all reported
         * now, and none of those echoes may count as a pick (on the device the list closed by itself, v1002) */
        run_blocks(q, 8, BLK, 0.5f, NULL);
        check(q->getParameter(q, shown) == 1 && !strcmp(disp_txt(q, SLOT_PARAM(2, S_NAME)), "Empty"), "the list stays open while its rows are reported (echoes ignored)", "");
        tap(q, key_idx("dd_2"));
        check(q->getParameter(q, shown) == 1 && !strcmp(disp_txt(q, SLOT_PARAM(2, S_NAME)), "Empty"), "a row set within 0.3 s of opening is no pick", "");
        usleep(350000);
        tap(q, key_idx("dd_20"));
        check(q->getParameter(q, shown) == 1 && !strcmp(disp_txt(q, SLOT_PARAM(2, S_NAME)), "Empty"), "an empty row does nothing", "");
        q->setParameter(q, key_idx("dd_1"), 0);
        check(q->getParameter(q, shown) == 1, "a set that doesn't change a row's state is no pick", "");
        tap(q, key_idx("dd_1"));
        snprintf(detail, sizeof detail, "%s; %s", disp_txt(q, SLOT_PARAM(2, S_NAME)), disp_txt(q, 0));
        check(q->getParameter(q, shown) == 0 && !strcmp(detail, "Gain FX; FX 3: Gain FX (3 of 4 params)"), "a tap loads it into FX 3 and closes the list", detail);
        q->setParameter(q, POOL_PARAM(2, 0), 0.3f);
        tap(q, SLOT_PARAM(2, S_DROP));
        int lit = 0;
        for (int r = 1; r <= 52; r++) { char k[16]; snprintf(k, sizeof k, "dd_%d", r); lit += q->getParameter(q, key_idx(k)) == 1 ? r : 0; }
        snprintf(detail, sizeof detail, "lit row %d", lit);
        check(lit == 1, "reopened, the current plugin's row is lit", detail);
        run_blocks(q, 8, BLK, 0.5f, NULL);
        usleep(350000);
        q->setParameter(q, key_idx("dd_1"), 0);   /* MPC toggles the lit row off: still a tap */
        check(q->getParameter(q, shown) == 0 && q->getParameter(q, POOL_PARAM(2, 0)) == 0.3f, "picking the loaded plugin keeps it and its settings", "");
        tap(q, SLOT_PARAM(5, S_DROP));
        tap(q, key_idx("dd_next"));
        tap(q, key_idx("dd_prev"));
        check(!strcmp(disp_txt(q, key_idx("dd_title")), "Choose FX 6") && !strcmp(disp_txt(q, key_idx("dd_1")), "Gain FX"), "one page: PREV/NEXT stay on it", disp_txt(q, key_idx("dd_title")));
        tap(q, SLOT_PARAM(5, S_DROP));
        check(q->getParameter(q, shown) == 1, "a second tap (or a finger lift) on the field keeps it open", "");
        q->setParameter(q, shown, 0);
        check(q->getParameter(q, shown) == 1, "a stray set of the open state can't hide it", "");
        tap(q, SLOT_PARAM(5, S_DROP));
        tap(q, key_idx("dd_close"));
        tap(q, key_idx("dd_2"));
        check(q->getParameter(q, shown) == 0 && !strcmp(disp_txt(q, SLOT_PARAM(5, S_NAME)), "Empty"), "CLOSE closes it; a row tap while closed does nothing", "");
        close_fx(q);
    }

    /* 17. one tap, one action: MPC sends a button 1 on the press and 1 again when the finger lifts */
    {
        AEffect *r = open_fx();
        load(r, 2, "Gain FX");
        tap(r, SLOT_PARAM(2, S_UP));
        snprintf(detail, sizeof detail, "%s|%s|%s", disp_txt(r, SLOT_PARAM(0, S_NAME)), disp_txt(r, SLOT_PARAM(1, S_NAME)), disp_txt(r, SLOT_PARAM(2, S_NAME)));
        check(!strcmp(detail, "Empty|Gain FX|Empty"), "one tap on UP moves one place", detail);
        tap(r, KB("a"));
        tap(r, KB("b"));
        check(!strcmp(disp_txt(r, PR("name")), "AB_"), "one tap on a key types it once", disp_txt(r, PR("name")));
        tap(r, PR("slot_next"));
        check(!strcmp(disp_txt(r, PR("slot")), "02 -"), "one tap on NEXT steps once", disp_txt(r, PR("slot")));
        r->setParameter(r, SLOT_PARAM(1, S_DOWN), 1);   /* a press whose release hasn't come yet ... */
        r->setParameter(r, SLOT_PARAM(2, S_DOWN), 1);   /* ... and a press on another button is its own */
        check(!strcmp(disp_txt(r, SLOT_PARAM(3, S_NAME)), "Gain FX"), "presses on different buttons both count", disp_txt(r, 0));
        usleep(1300000);
        r->setParameter(r, SLOT_PARAM(3, S_DOWN), 1);   /* a 1 more than 1.2 s after a press is a new press */
        check(!strcmp(disp_txt(r, SLOT_PARAM(4, S_NAME)), "Gain FX"), "after a long hold the next 1 is a new press", disp_txt(r, 0));
        close_fx(r);
    }

    /* 18. stress: audio on another thread while the screen side loads, clears, moves, tweaks, saves, recalls and picks */
    pthread_t th;
    g_stop = 0;
    pthread_create(&th, NULL, audio_thread, a);
    const char *names[] = {"Gain FX", "Mono FX", "P-Valley PNG", "YES-SL 9000", "\xc3\x89mile Chunk", "Many Params"};
    unsigned rnd = 1;
    for (int it = 0; it < 300; it++) {
        rnd = rnd * 1103515245 + 12345;
        int s = (rnd >> 8) % NSLOTS, op = (rnd >> 12) % 9;
        if (op == 0) load(a, s, names[(rnd >> 16) % 6]);
        else if (op == 1) tap(a, SLOT_PARAM(s, S_CLEAR));
        else if (op == 2) for (int k = 0; k < POOL; k++) { a->setParameter(a, POOL_PARAM(s, k), (float)((rnd >> 4) % 100) / 100); name_txt(a, POOL_PARAM(s, k)); disp_txt(a, POOL_PARAM(s, k)); }
        else if (op == 3) { void *ck; intptr_t l2 = a->dispatcher(a, 23, 0, 0, &ck, 0); unsigned char *cp = malloc(l2); memcpy(cp, ck, l2); a->dispatcher(a, 24, 0, l2, cp, 0); free(cp); }
        else if (op == 4) a->setParameter(a, SLOT_PARAM(s, S_ON), (float)((rnd >> 20) & 1));
        else if (op == 5) a->setParameter(a, SLOT_PARAM(s, S_MIX), (float)((rnd >> 4) % 100) / 100);
        else if (op == 6) tap(a, SLOT_PARAM(s, (rnd >> 20) & 1 ? S_UP : S_DOWN));
        else if (op == 7) { a->setParameter(a, PR("slot"), (float)((rnd >> 16) % 3) / 31); tap(a, PR((rnd >> 20) & 1 ? "save" : "recall")); }
        else { char k[16]; tap(a, SLOT_PARAM(s, S_DROP)); snprintf(k, sizeof k, "dd_%u", 1 + (rnd >> 16) % 52); disp_txt(a, key_idx(k)); tap(a, key_idx(k)); }
    }
    g_stop = 1;
    pthread_join(th, NULL);
    check(1, "300 screen-side operations against running audio", "");

    close_fx(a);
    printf("%d/%d checks passed\n", checks - fails, checks);
    return fails ? 1 : 0;
}
