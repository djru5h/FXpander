/* FXpander on the device, outside MPC: a stand-in host process that loads r5hfxpander.so with the real MPC.settings
 * and the real installed effects. MPC keeps running; nothing is installed or registered.
 *   devhost <fxpander.so> scan                every listed effect, one at a time in a forked process (a crash
 *                                             only ends that one): load, 2 s of audio, names, state round trip
 *   devhost <fxpander.so> chain A [B .. H] those effects in slots 1-8: 20 s of audio, per-block CPU time
 *                                             against MPC's 2902 us block, then a save/restore into a second instance */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "params.h"

#define NSLOTS 8
#define POOL 64
enum { S_PICK, S_PREV, S_NEXT, S_LOAD, S_CLEAR, S_NAME, S_ON, S_MIX, S_N };
/* parameter indices by key, as the plugin finds them */
static int key_idx(const char *k) {
    for (int i = 0; i < NPARAMS; i++)
        if (!strcmp(PARAMS[i].key, k)) return i;
    fprintf(stderr, "no param %s\n", k);
    abort();
}
static int SLOT_PARAM(int s, int f) {
    static const char *suf[S_N] = {"pick", "pick_prev", "pick_next", "load", "clear", "name", "on", "mix"};
    char k[32];
    snprintf(k, sizeof k, "s%d_%s", s + 1, suf[f]);
    return key_idx(k);
}
static int POOL_PARAM(int s, int k) { char b[32]; snprintf(b, sizeof b, "s%d_p%02d", s + 1, k + 1); return key_idx(b); }
#define BLK 128
#define BUDGET_US 2902.0

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

static VstTimeInfo g_ti = {.sampleRate = 44100, .tempo = 90, .timeSigNumerator = 4, .timeSigDenominator = 4,
                           .flags = 1 << 1 | 1 << 9 | 1 << 10 | 1 << 13};
static intptr_t master(AEffect *e, int32_t op, int32_t idx, intptr_t v, void *p, float o) {
    switch (op) {
    case 1: return 2400;
    case 7: return (intptr_t)&g_ti;
    case 32: strcpy(p, "Akai"); return 1;
    case 33: strcpy(p, "MPC"); return 1;
    case 34: return 1;
    default: return 0;
    }
}
/* a touch as MPC sends it: a button (momentary param) gets 1 when pressed and 1 again when the finger lifts (the
 * plugin has reported it back to 0 in between); anything else is set once */
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
static void close_fx(AEffect *e) { e->dispatcher(e, 72, 0, 0, NULL, 0); e->dispatcher(e, 12, 0, 0, NULL, 0); e->dispatcher(e, 1, 0, 0, NULL, 0); }
static const char *txt(AEffect *e, int op, int i) { static char b[8][256]; static int r; char *o = b[r++ % 8]; memset(o, 0, 256); e->dispatcher(e, op, i, 0, o, 0); return o; }
static double now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e6 + t.tv_nsec / 1e3; }

static float il[BLK], ir[BLK], ol[BLK], orr[BLK];
static long g_ph;
/* one block of a drum-ish test signal (decaying 110 Hz hits + a little noise); returns the block's CPU time in us,
 * *bad set on a non-finite sample, *peak the block's peak */
static double block(AEffect *e, int *bad, float *peak) {
    float *in[2] = {il, ir}, *out[2] = {ol, orr};
    for (int i = 0; i < BLK; i++) {
        long t = (g_ph + i) % 22050;
        float v = 0.5f * expf(-t / 3000.0f) * sinf(2 * (float)M_PI * 110 * t / 44100) + 0.02f * ((rand() & 1023) / 512.0f - 1);
        il[i] = ir[i] = v;
    }
    g_ph += BLK;
    g_ti.samplePos = (double)g_ph;
    g_ti.ppqPos = g_ph / 44100.0 * g_ti.tempo / 60;
    double t0 = now_us();
    e->processReplacing(e, in, out, BLK);
    double dt = now_us() - t0;
    for (int i = 0; i < BLK; i++) {
        if (!isfinite(ol[i]) || !isfinite(orr[i])) *bad = 1;
        float a = fabsf(ol[i]) > fabsf(orr[i]) ? fabsf(ol[i]) : fabsf(orr[i]);
        if (a > *peak) *peak = a;
    }
    return dt;
}

static int pick(AEffect *e, int s, const char *want) {
    e->setParameter(e, SLOT_PARAM(s, S_PICK), 0);
    for (int t = 0; t < 130; t++) {
        if (!strcmp(txt(e, 7, SLOT_PARAM(s, S_PICK)), want)) return 1;
        tap(e, SLOT_PARAM(s, S_NEXT));
    }
    return 0;
}

static int cmp_d(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }

static int scan_one(int idx) {
    AEffect *e = open_fx();
    e->setParameter(e, SLOT_PARAM(0, S_PICK), 0);
    for (int i = 0; i < idx; i++) tap(e, SLOT_PARAM(0, S_NEXT));
    char name[128];
    snprintf(name, sizeof name, "%s", txt(e, 7, SLOT_PARAM(0, S_PICK)));
    double t0 = now_us();
    tap(e, SLOT_PARAM(0, S_LOAD));
    double load_ms = (now_us() - t0) / 1000;
    if (strcmp(txt(e, 7, SLOT_PARAM(0, S_NAME)), name)) {
        printf("FAIL  %-22s %s\n", name, txt(e, 7, 0));
        return 1;
    }
    int bad = 0;
    float peak = 0;
    double sum = 0, mx = 0;
    for (int b = 0; b < 690; b++) {   /* 2 s */
        double dt = block(e, &bad, &peak);
        sum += dt;
        if (dt > mx) mx = dt;
    }
    void *ck = NULL;
    intptr_t len = e->dispatcher(e, 23, 0, 0, &ck, 0);
    unsigned char *copy = malloc(len > 0 ? len : 1);
    if (len > 0) memcpy(copy, ck, len);
    AEffect *f = open_fx();
    int restored = len > 0 && f->dispatcher(f, 24, 0, len, copy, 0) == 1 && !strcmp(txt(f, 7, SLOT_PARAM(0, S_NAME)), name);
    for (int k = 0; k < POOL; k++)
        if (strcmp(txt(e, 7, POOL_PARAM(0, k)), txt(f, 7, POOL_PARAM(0, k)))) {
            const char *pn = txt(e, 8, POOL_PARAM(0, k)), *pe = txt(e, 7, POOL_PARAM(0, k)), *pf = txt(f, 7, POOL_PARAM(0, k));
            float ve = e->getParameter(e, POOL_PARAM(0, k)), vf = f->getParameter(f, POOL_PARAM(0, k));
            printf("      %s knob %d %s: \"%s\" (%.4f) restored \"%s\" (%.4f)\n", name, k + 1, pn, pe, ve, pf, vf);
            restored = 0;
        }
    char status[128];
    snprintf(status, sizeof status, "%s", txt(e, 7, 0));
    const char *colon = strchr(status, '(');
    printf("%s  %-22s load %6.1f ms  cpu avg %5.1f%% max %5.1f%%  peak %5.2f  restore %s  %s  [%s|%s]\n",
           bad ? "BAD " : "ok  ", name, load_ms, 100 * sum / 690 / BUDGET_US, 100 * mx / BUDGET_US, peak,
           restored ? "ok" : "DIFF", colon ? colon : "", txt(e, 8, POOL_PARAM(0, 0)), txt(e, 7, POOL_PARAM(0, 0)));
    fflush(stdout);
    close_fx(f);
    close_fx(e);
    free(copy);
    return bad || !restored;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: devhost <fxpander.so> scan | chain A [B .. H]\n"); return 2; }
    void *lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) { fprintf(stderr, "%s\n", dlerror()); return 1; }
    g_main = (vst_main_t)dlsym(lib, "VSTPluginMain");
    if (!strcmp(argv[2], "scan")) {
        AEffect *e = open_fx();
        printf("%s\n", txt(e, 7, 0));
        int n = 0;   /* list length: step until it wraps */
        e->setParameter(e, SLOT_PARAM(0, S_PICK), 0);
        do { tap(e, SLOT_PARAM(0, S_NEXT)); n++; } while (e->getParameter(e, SLOT_PARAM(0, S_PICK)) > 0 && n < 130);
        close_fx(e);
        int fails = 0;
        for (int i = 1; i < n; i++) {
            fflush(stdout);
            pid_t pid = fork();
            if (pid == 0) _exit(scan_one(i));
            int st = 0;
            waitpid(pid, &st, 0);
            if (WIFSIGNALED(st)) { printf("CRASH entry %d: signal %d\n", i, WTERMSIG(st)); fails++; }
            else if (WEXITSTATUS(st)) fails++;
        }
        printf("%d of %d effects had a problem\n", fails, n - 1);
        return fails != 0;
    }
    if (!strcmp(argv[2], "chain")) {
        AEffect *e = open_fx();
        for (int s = 0; s < NSLOTS && 3 + s < argc; s++) {
            if (!pick(e, s, argv[3 + s])) { printf("not in the list: %s\n", argv[3 + s]); return 1; }
            double t0 = now_us();
            tap(e, SLOT_PARAM(s, S_LOAD));
            printf("FX %d: %s  (%.1f ms)\n", s + 1, txt(e, 7, 0), (now_us() - t0) / 1000);
        }
        enum { NB = 6890 };   /* 20 s */
        static double t[NB];
        int bad = 0;
        float peak = 0;
        for (int b = 0; b < NB; b++) {
            if (b == NB / 2)   /* halfway: sweep knob 1 of every slot each block, like a Q-Link turn */
                printf("(sweeping knob 1 of each slot from here)\n");
            if (b >= NB / 2)
                for (int s = 0; s < NSLOTS; s++) e->setParameter(e, POOL_PARAM(s, 0), 0.5f + 0.4f * sinf(b * 0.01f));
            t[b] = block(e, &bad, &peak);
        }
        for (int half = 0; half < 2; half++) {
            double *h = t + half * (NB / 2);
            qsort(h, NB / 2, sizeof *h, cmp_d);
            double sum = 0;
            for (int i = 0; i < NB / 2; i++) sum += h[i];
            printf("%s: mean %.1f%%  p50 %.1f%%  p99 %.1f%%  max %.1f%% of a %g us block\n", half ? "sweeping" : "steady  ",
                   100 * sum / (NB / 2) / BUDGET_US, 100 * h[NB / 4] / BUDGET_US, 100 * h[NB / 2 * 99 / 100] / BUDGET_US,
                   100 * h[NB / 2 - 1] / BUDGET_US, BUDGET_US);
        }
        printf("output: %s, peak %.2f\n", bad ? "NON-FINITE" : "finite", peak);
        void *ck = NULL;
        intptr_t len = e->dispatcher(e, 23, 0, 0, &ck, 0);
        unsigned char *copy = malloc(len);
        memcpy(copy, ck, len);
        AEffect *f = open_fx();
        f->dispatcher(f, 24, 0, len, copy, 0);
        int same = 1;
        for (int s = 0; s < NSLOTS; s++) {
            char a[256], b[256];
            snprintf(a, sizeof a, "%s", txt(e, 7, SLOT_PARAM(s, S_NAME)));
            snprintf(b, sizeof b, "%s", txt(f, 7, SLOT_PARAM(s, S_NAME)));
            if (strcmp(a, b)) { same = 0; printf("  differs: FX %d name '%s' vs '%s'\n", s + 1, a, b); }
            for (int k = 0; k < POOL; k++) {
                snprintf(a, sizeof a, "%s", txt(e, 7, POOL_PARAM(s, k)));
                snprintf(b, sizeof b, "%s", txt(f, 7, POOL_PARAM(s, k)));
                if (strcmp(a, b)) {
                    same = 0;
                    printf("  differs: FX %d knob %d %s: '%s' vs '%s'\n", s + 1, k + 1, txt(e, 8, POOL_PARAM(s, k)), a, b);
                }
            }
        }
        printf("chunk %ld bytes, restore into a second instance: %s\n", (long)len, same ? "same plugins and values" : "DIFFERENT");
        for (int s = 0; s < NSLOTS; s++) {
            printf("FX %d %s:", s + 1, txt(e, 7, SLOT_PARAM(s, S_NAME)));
            for (int k = 0; k < 6; k++) printf("  %s=%s", txt(e, 8, POOL_PARAM(s, k)), txt(e, 7, POOL_PARAM(s, k)));
            printf("\n");
        }
        close_fx(f);
        close_fx(e);
        free(copy);
        return bad || !same;
    }
    return 2;
}
