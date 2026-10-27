/* Test children for the harness: a tiny VST2 effect that probes what FXpander gives a hosted plugin.
 *   params: 0 Gain (out = in * 2 * gain), 1 Meter (not automatable; set by the plugin), 2 Tempo (display: the host
 *   tempo it last saw from audioMasterGetTime in process), 3 Poke (set > 0.5: the plugin moves Gain to 0.25 and calls
 *   audioMasterAutomate for it, as a plugin with its own controls would), 4.. extra (EXTRA of them, inert)
 * Variants (-D): USE_CHUNK (state as an effGetChunk chunk, else FXpander saves the param values), MONO (1 in, 1 out),
 * SYNTH (flags an instrument), EXTRA=<n>. TFX_UID / TFX_NAME name it. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef EXTRA
#define EXTRA 0
#endif
#define NP (4 + EXTRA)

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

typedef struct {
    AEffect fx;
    audioMasterCallback master;
    float p[NP];
    double tempo;
    float sr;
    int opened, resumed;
    char chunk[64];
} tfx_t;

static intptr_t dispatcher(AEffect *e, int32_t op, int32_t idx, intptr_t v, void *ptr, float o) {
    tfx_t *t = e->object;
    switch (op) {
    case 0: t->opened = 1; return 0;
    case 1: free(t); return 0;
    case 6: strcpy(ptr, idx == 0 ? "x" : ""); return 0;                         /* label */
    case 7:                                                                      /* display */
        if (idx == 2) snprintf(ptr, 24, "%.1f", t->tempo);
        else if (idx >= 0 && idx < NP) snprintf(ptr, 24, "%.2f", t->p[idx] * (idx == 0 ? 2 : 1));
        return 0;
    case 8: {                                                                    /* name */
        static const char *names[] = {"Gain", "Meter", "Tempo", "Poke"};
        if (idx < 4) strcpy(ptr, names[idx]);
        else snprintf(ptr, 24, "Extra %d", idx - 3);
        return 0;
    }
    case 10: t->sr = o; return 0;
    case 12: t->resumed = (int)v; return 0;
    case 26: return idx != 1;                                                    /* the meter isn't automatable */
#ifdef USE_CHUNK
    case 23: snprintf(t->chunk, sizeof t->chunk, "gain=%.6f", t->p[0]); *(void **)ptr = t->chunk; return (intptr_t)strlen(t->chunk) + 1;
    case 24: { char b[64]; snprintf(b, sizeof b, "%.*s", (int)v, (const char *)ptr); float g; if (sscanf(b, "gain=%f", &g) == 1) t->p[0] = g; return 1; }
#endif
    case 35: return 1;
    case 47: case 48: strcpy(ptr, "test"); return 1;
    default: return 0;
    }
}

static void setParameter(AEffect *e, int32_t i, float v) {
    tfx_t *t = e->object;
    if (i < 0 || i >= NP) return;
    t->p[i] = v;
    if (i == 3 && v > 0.5f) {
        t->p[0] = 0.25f;
        t->master(e, 0, 0, 0, 0, 0.25f);   /* audioMasterAutomate: Gain moved by the plugin */
    }
}
static float getParameter(AEffect *e, int32_t i) { tfx_t *t = e->object; return i >= 0 && i < NP ? t->p[i] : 0; }

static void processReplacing(AEffect *e, float **in, float **out, int32_t n) {
    tfx_t *t = e->object;
    VstTimeInfo *ti = (VstTimeInfo *)t->master(e, 7, 0, 1 << 10, 0, 0);
    if (ti) t->tempo = ti->tempo;
    float g = 2 * t->p[0];
#ifdef MONO
    for (int i = 0; i < n; i++) out[0][i] = in[0][i] * g;
#else
    for (int i = 0; i < n; i++) { out[0][i] = in[0][i] * g; out[1][i] = in[1][i] * g; }
#endif
    t->p[1] = 0.5f;
}

__attribute__((visibility("default"))) AEffect *VSTPluginMain(audioMasterCallback master) {
    if (master(NULL, 1, 0, 0, 0, 0) < 2400) return NULL;   /* audioMasterVersion */
    tfx_t *t = calloc(1, sizeof *t);
    t->master = master;
    t->p[0] = 0.5f;   /* unity */
    AEffect *e = &t->fx;
    e->magic = 0x56737450;
    e->dispatcher = dispatcher;
    e->setParameter = setParameter;
    e->getParameter = getParameter;
    e->processReplacing = processReplacing;
    e->numParams = NP;
#ifdef MONO
    e->numInputs = e->numOutputs = 1;
#else
    e->numInputs = e->numOutputs = 2;
#endif
    e->flags = 1 << 4;
#ifdef USE_CHUNK
    e->flags |= 1 << 5;
#endif
#ifdef SYNTH
    e->flags |= 1 << 8;
#endif
    e->uniqueID = TFX_UID;
    e->object = t;
    return e;
}
