/* =============================================================================
 * FXpander: a VST2 effect for MPC OS that hosts up to eight other VST2 effects in series, so one of MPC's four
 * insert slots holds eight effects. Hand-written VST2 on both sides (no Steinberg SDK): to MPC it is an effect with
 * a fixed parameter table (vst/make_skin.py, generated params.h); to each hosted plugin ("child") it is the host.
 *
 * Parameters (order in make_skin.py; found here by key, see params_init): per slot a browse list over MPC's
 * registered effects (MPC.settings, pluginList-arm), Load / Clear / Up / Down, On and Mix, plus 64 knobs that map onto
 * the child's parameters (its automatable ones, in order; all of them if it marks none). The mapping is fixed for a
 * given plugin, so automation and Q-Links keep their meaning. Names and value texts are the child's own. Chain
 * presets (all slots, named on an on-screen keyboard) are files beside the Synths folder and MPC's PRESET menu.
 *
 * Threads: MPC (JUCE) calls set/get parameter, chunks and displays from its message thread while audio runs on a
 * worker thread. `lock` (recursive, priority-inheriting) guards the slots' child pointers: the audio thread holds it
 * for the chain run, the screen side for a forwarded parameter call or a pointer swap. `ctl` serialises the slow
 * structural work (loading, unloading, chunks, presets), which runs outside `lock` so audio keeps going. MPC is never
 * called with `lock` held: what children report (automate, update display) is queued and told to MPC after the run.
 * ========================================================================== */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "params.h"

#define NSLOTS 8
#define POOL 64                         /* child parameters per slot */
enum { S_PICK, S_PREV, S_NEXT, S_LOAD, S_CLEAR, S_NAME, S_ON, S_MIX, S_UP, S_DOWN, S_DROP, S_N };
enum { PR_SLOT, PR_PREV, PR_NEXT, PR_RECALL, PR_SAVE, PR_DELETE, PR_NAME, PR_INFO, PR_N };
enum { DD_SHOWN, DD_PREV, DD_NEXT, DD_CLOSE, DD_TITLE, DD_N };
enum { K_NONE, K_STATUS, K_SLOT, K_POOL, K_PR, K_KEY, K_DD, K_DDROW };
#define DD_ROWS 52                      /* the drop-down list's rows: plugins per page */
#define PICK_MAX 127                    /* browse param range: 0 = empty, 1.. = the list */
#define MAXLIST PICK_MAX
#define NPRESETS 32
#define NAME_MAX_LEN 24
#define KEY_DEL 0x7f
#define KEY_CLEAR 0x01
#define MAXBLK 1024                     /* frames per chain pass; bigger host blocks run in pieces */
#define MAXCH 8                         /* a child's inputs/outputs; the extras get silence / a junk buffer */
#define REPORTS_PER_BLOCK 16            /* automates told to MPC per audio block (the rest follow next block) */
#define TEXT_MAX 64

/* ---- VST2 ABI (hand-written, as wrapper/vst2_wrap.c) --------------------- */
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

#define VST_MAGIC 0x56737450 /* 'VstP' */
enum {
    effOpen = 0, effClose = 1, effSetProgram = 2, effGetProgram = 3, effGetProgramName = 5, effGetParamLabel = 6,
    effGetParamDisplay = 7, effGetParamName = 8, effSetSampleRate = 10, effSetBlockSize = 11, effMainsChanged = 12,
    effGetChunk = 23, effSetChunk = 24, effCanBeAutomated = 26, effGetProgramNameIndexed = 29, effGetPlugCategory = 35,
    effGetEffectName = 45, effGetVendorString = 47, effGetProductString = 48, effGetVendorVersion = 49, effCanDo = 51,
    effGetVstVersion = 58, effStartProcess = 71, effStopProcess = 72,
};
enum {
    amAutomate = 0, amVersion = 1, amCurrentId = 2, amIdle = 3, amPinConnected = 4, amGetTime = 7,
    amIOChanged = 13, amSizeWindow = 15, amGetSampleRate = 16, amGetBlockSize = 17, amGetInputLatency = 18,
    amGetOutputLatency = 19, amGetCurrentProcessLevel = 23, amGetAutomationState = 24, amGetVendorString = 32,
    amGetProductString = 33, amGetVendorVersion = 34, amCanDo = 37, amGetLanguage = 38, amGetDirectory = 41,
    amUpdateDisplay = 42, amBeginEdit = 43, amEndEdit = 44,
};
enum { effFlagsCanReplacing = 1 << 4, effFlagsProgramChunks = 1 << 5, effFlagsIsSynth = 1 << 8 };
#define TIME_ALL 0xff00 /* kVstNanosValid .. kVstClockValid */

/* ---- the parameter table, by key ------------------------------------------ */
typedef struct { uint8_t kind, s, f; } pinfo_t;   /* f: slot field, pool knob, preset field or key character */
static pinfo_t g_pi[NPARAMS];
static int g_slot_param[NSLOTS][S_N], g_pool_param[NSLOTS][POOL], g_pr_param[PR_N], g_dd_param[DD_N], g_ddrow_param[DD_ROWS];
static int g_params_ok;
static pthread_once_t g_params_once = PTHREAD_ONCE_INIT;

static void params_init(void) {
    static const char *sf[S_N] = {"pick", "pick_prev", "pick_next", "load", "clear", "name", "on", "mix", "up", "down", "drop"};
    static const char *df[DD_N] = {"shown", "prev", "next", "close", "title"};
    static const char *pf[PR_N] = {"slot", "slot_prev", "slot_next", "recall", "save", "delete", "name", "info"};
    static const struct { const char *id; char c; } named[] = {
        {"dash", '-'}, {"space", ' '}, {"del", KEY_DEL}, {"clear", KEY_CLEAR}};
    int found = 0, want = 1 + NSLOTS * (S_N + POOL) + PR_N + DD_N + DD_ROWS;
    for (int i = 0; i < NPARAMS; i++) {
        const char *k = PARAMS[i].key;
        int s, n, len = 0;
        pinfo_t *p = &g_pi[i];
        if (!strcmp(k, "status")) { p->kind = K_STATUS; found++; }
        else if (sscanf(k, "s%d_p%2d%n", &s, &n, &len) == 2 && !k[len] && s >= 1 && s <= NSLOTS && n >= 1 && n <= POOL) {
            *p = (pinfo_t){K_POOL, (uint8_t)(s - 1), (uint8_t)(n - 1)};
            g_pool_param[s - 1][n - 1] = i;
            found++;
        } else if (sscanf(k, "s%d_%n", &s, &len) == 1 && len && s >= 1 && s <= NSLOTS) {
            for (int f = 0; f < S_N; f++)
                if (!strcmp(k + len, sf[f])) { *p = (pinfo_t){K_SLOT, (uint8_t)(s - 1), (uint8_t)f}; g_slot_param[s - 1][f] = i; found++; }
        } else if (!strncmp(k, "pr_", 3)) {
            for (int f = 0; f < PR_N; f++)
                if (!strcmp(k + 3, pf[f])) { *p = (pinfo_t){K_PR, 0, (uint8_t)f}; g_pr_param[f] = i; found++; }
        } else if (!strncmp(k, "dd_", 3)) {
            for (int f = 0; f < DD_N; f++)
                if (!strcmp(k + 3, df[f])) { *p = (pinfo_t){K_DD, 0, (uint8_t)f}; g_dd_param[f] = i; found++; }
            if (sscanf(k, "dd_%d%n", &n, &len) == 1 && !k[len] && n >= 1 && n <= DD_ROWS) {
                *p = (pinfo_t){K_DDROW, 0, (uint8_t)(n - 1)};
                g_ddrow_param[n - 1] = i;
                found++;
            }
        } else if (!strncmp(k, "kb_", 3)) {
            const char *id = k + 3;
            char c = 0;
            if (strlen(id) == 1 && isalnum((unsigned char)id[0])) c = (char)toupper((unsigned char)id[0]);
            for (size_t j = 0; j < sizeof named / sizeof named[0]; j++)
                if (!strcmp(id, named[j].id)) c = named[j].c;
            if (c) *p = (pinfo_t){K_KEY, 0, (uint8_t)c};
        }
    }
    g_params_ok = found == want;
}

/* ---- the plugin list (MPC's registered effects) -------------------------- */
typedef struct { char name[TEXT_MAX], path[512]; uint32_t uid; } entry_t;
static entry_t g_list[MAXLIST];
static int g_nlist;
static pthread_once_t g_list_once = PTHREAD_ONCE_INIT;

/* XML entities (&amp; &#201; ...) in place, numeric ones to UTF-8 */
static void xml_unescape(char *s) {
    char *o = s;
    while (*s) {
        if (*s == '&') {
            static const struct { const char *e; char c; } named[] = {
                {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}};
            int done = 0;
            for (size_t i = 0; i < sizeof named / sizeof named[0] && !done; i++)
                if (!strncmp(s, named[i].e, strlen(named[i].e))) { *o++ = named[i].c; s += strlen(named[i].e); done = 1; }
            if (!done && s[1] == '#') {
                char *end;
                unsigned long cp = s[2] == 'x' || s[2] == 'X' ? strtoul(s + 3, &end, 16) : strtoul(s + 2, &end, 10);
                if (*end == ';' && cp > 0 && cp < 0x110000) {
                    if (cp < 0x80) *o++ = (char)cp;
                    else if (cp < 0x800) { *o++ = (char)(0xc0 | cp >> 6); *o++ = (char)(0x80 | (cp & 0x3f)); }
                    else if (cp < 0x10000) { *o++ = (char)(0xe0 | cp >> 12); *o++ = (char)(0x80 | (cp >> 6 & 0x3f)); *o++ = (char)(0x80 | (cp & 0x3f)); }
                    else { *o++ = (char)(0xf0 | cp >> 18); *o++ = (char)(0x80 | (cp >> 12 & 0x3f)); *o++ = (char)(0x80 | (cp >> 6 & 0x3f)); *o++ = (char)(0x80 | (cp & 0x3f)); }
                    s = end + 1;
                    done = 1;
                }
            }
            if (done) continue;
        }
        *o++ = *s++;
    }
    *o = 0;
}

/* attribute `name` of one <PLUGIN .../> element (attributes may span lines) into out; 1 if found */
static int xml_attr(const char *el, const char *end, const char *name, char *out, size_t n) {
    size_t nl = strlen(name);
    for (const char *p = el; p + nl + 2 < end; p++) {
        if (!isspace((unsigned char)p[0]) || strncmp(p + 1, name, nl) || p[1 + nl] != '=' || p[2 + nl] != '"') continue;
        const char *v = p + nl + 3, *q = memchr(v, '"', end - v);
        if (!q) return 0;
        size_t len = (size_t)(q - v) < n - 1 ? (size_t)(q - v) : n - 1;
        memcpy(out, v, len);
        out[len] = 0;
        xml_unescape(out);
        return 1;
    }
    return 0;
}

static int entry_cmp(const void *a, const void *b) { return strcasecmp(((const entry_t *)a)->name, ((const entry_t *)b)->name); }

static void load_list(void) {
    const char *paths[] = {getenv("FXPANDER_SETTINGS"), "/media/az01-internal/Settings/MPC/MPC.settings",
                           "/data/Settings/MPC/MPC.settings"};
    FILE *f = NULL;
    for (size_t i = 0; i < sizeof paths / sizeof paths[0] && !f; i++)
        if (paths[i]) f = fopen(paths[i], "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *xml = size > 0 ? malloc((size_t)size + 1) : NULL;
    if (!xml || fread(xml, 1, (size_t)size, f) != (size_t)size) { free(xml); fclose(f); return; }
    fclose(f);
    xml[size] = 0;
    const char *sec = strstr(xml, "name=\"pluginList-arm\"");
    const char *sec_end = sec ? strstr(sec, "</KNOWNPLUGINS>") : NULL;
    for (const char *p = sec; p && sec_end && (p = strstr(p, "<PLUGIN")) && p < sec_end && g_nlist < MAXLIST;) {
        const char *e = strstr(p, "/>");
        if (!e) break;
        char name[TEXT_MAX], file[512], uid[16], inst[8], fmt[16];
        entry_t *en = &g_list[g_nlist];
        if (xml_attr(p, e, "name", name, sizeof name) && xml_attr(p, e, "file", file, sizeof file) &&
            xml_attr(p, e, "uid", uid, sizeof uid) && xml_attr(p, e, "isInstrument", inst, sizeof inst) &&
            (!xml_attr(p, e, "format", fmt, sizeof fmt) || !strcmp(fmt, "VST")) && !strcmp(inst, "0") &&
            (uint32_t)strtoul(uid, NULL, 16) != (uint32_t)PLUG_UID && access(file, R_OK) == 0) {
            snprintf(en->name, sizeof en->name, "%s", name);
            snprintf(en->path, sizeof en->path, "%s", file);
            en->uid = (uint32_t)strtoul(uid, NULL, 16);
            g_nlist++;
        }
        p = e + 2;
    }
    free(xml);
    qsort(g_list, g_nlist, sizeof g_list[0], entry_cmp);
}

static int list_find(const char *path) {
    for (int i = 0; i < g_nlist; i++)
        if (!strcmp(g_list[i].path, path)) return i;
    return -1;
}

/* ---- chain presets: <drive>/FXpander Presets/NN.fxchain, shared by every instance ---------------------------- */
typedef struct { int used; char name[NAME_MAX_LEN + 1], info[TEXT_MAX * 2]; } preset_t;
static preset_t g_pr[NPRESETS];
static char g_pr_dir[600];
static pthread_mutex_t g_pr_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t g_pr_once = PTHREAD_ONCE_INIT;

/* the folder this .so was loaded from (as wrapper/plugin_dir.h: /proc/self/maps, no dladdr) */
static int self_dir(char *buf, size_t n) {
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return 0;
    char line[1024];
    unsigned long me = (unsigned long)&self_dir;
    int ok = 0;
    while (!ok && fgets(line, sizeof line, f)) {
        unsigned long lo, hi;
        char *p = strchr(line, '/');
        if (sscanf(line, "%lx-%lx", &lo, &hi) == 2 && me >= lo && me < hi && p) {
            p[strcspn(p, "\n")] = 0;
            char *sl = strrchr(p, '/');
            if (sl && sl > p) { *sl = 0; snprintf(buf, n, "%s", p); ok = 1; }
        }
    }
    fclose(f);
    return ok;
}

static void pr_path(int i, char *buf, size_t n) { snprintf(buf, n, "%s/%02d.fxchain", g_pr_dir, i + 1); }

/* a preset file's header (name, contents) into p; 1 if it is one */
static int pr_read_head(FILE *f, preset_t *p) {
    char magic[4];
    uint32_t ver;
    uint16_t l1, l2;
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "FXPR", 4) || fread(&ver, 4, 1, f) != 1) return 0;
    if (fread(&l1, 2, 1, f) != 1 || l1 > NAME_MAX_LEN || fread(p->name, 1, l1, f) != l1) return 0;
    p->name[l1] = 0;
    if (fread(&l2, 2, 1, f) != 1 || l2 >= sizeof p->info || fread(p->info, 1, l2, f) != l2) return 0;
    p->info[l2] = 0;
    return 1;
}

static void pr_scan(void) {
    const char *env = getenv("FXPANDER_PRESETS");
    char here[512];
    if (env) snprintf(g_pr_dir, sizeof g_pr_dir, "%s", env);
    else if (self_dir(here, sizeof here)) {
        /* <drive>/Synths/<plugin folder> -> <drive>/FXpander Presets (beside Synths, like the YES-SL 9000 presets) */
        char *syn = strstr(here, "/Synths/");
        if (syn) *syn = 0;
        snprintf(g_pr_dir, sizeof g_pr_dir, "%s/FXpander Presets", here);
    } else snprintf(g_pr_dir, sizeof g_pr_dir, "/media/MPC_ONE/FXpander Presets");
    for (int i = 0; i < NPRESETS; i++) {
        char path[700];
        pr_path(i, path, sizeof path);
        FILE *f = fopen(path, "rb");
        memset(&g_pr[i], 0, sizeof g_pr[i]);
        if (f) { g_pr[i].used = pr_read_head(f, &g_pr[i]); fclose(f); }
    }
}

/* ---- instances ----------------------------------------------------------- */
typedef struct chain chain_t;
typedef struct {
    AEffect *fx;
    chain_t *owner;
    int slot;              /* changed (under lock) when the slot moves */
    char name[TEXT_MAX], path[512], dir[512];
    int nmap, map[POOL];   /* pool knob k -> child parameter */
    int nrev;
    short *rev;            /* child parameter -> pool knob, -1 = not shown */
} child_t;

typedef struct {
    child_t *c;            /* swapped under lock */
    int pick;              /* browse position: 0 = empty, i = g_list[i - 1] */
    volatile int on;
    volatile float mix;
    float g;               /* wet gain applied at the end of the last block (audio thread; 0 after a swap) */
    unsigned char *orphan; /* a saved slot whose plugin didn't load: kept and saved again as it was */
    size_t orphan_len;
    char orphan_name[TEXT_MAX];
} slot_t;

struct chain {
    AEffect fx;
    audioMasterCallback master;
    slot_t slot[NSLOTS];
    pthread_mutex_t lock, ctl;
    float sr;
    int bs, resumed, started;
    float buf[2][2][MAXBLK], mono[MAXBLK], zero[MAXBLK], junk[MAXBLK];
    VstTimeInfo *ti;               /* MPC's time info for the block being run */
    volatile char dirty[NPARAMS];  /* values to report to MPC (audioMasterAutomate) */
    volatile char hold[NPARAMS];   /* triggers fired: report them back to 0 */
    volatile int need_update;      /* audioMasterUpdateDisplay to send */
    int rep_next;
    char status[TEXT_MAX];
    int pr_sel;                    /* the PRESETS page's preset slot */
    char pr_name[NAME_MAX_LEN + 1];/* the name being typed */
    int program;                   /* MPC's PRESET menu: 0 = none, n = preset n (see effSetProgram) */
    volatile int dd_open;          /* the drop-down plugin list: shown, for which slot, which page, since when */
    int dd_slot, dd_page;
    double dd_opened;
    char release_due[NPARAMS];     /* buttons: a press fired, its release (a second 1) is still to come */
    double pressed_at[NPARAMS];
    unsigned char *chunk;
    size_t chunk_cap;
};

static __thread chain_t *tl_proc;     /* the instance whose chain this thread is running */
static __thread child_t *tl_loading;  /* the child whose VSTPluginMain this thread is in */
static __thread int tl_echo = -1;     /* the param this thread is reporting to MPC (see report) */

/* child AEffect -> child_t, for the host callback (a child calls it with its own AEffect) */
#define MAXREG 256
static AEffect *volatile reg_fx[MAXREG];
static child_t *volatile reg_c[MAXREG];
static pthread_mutex_t reg_mu = PTHREAD_MUTEX_INITIALIZER;

static child_t *reg_find(AEffect *fx) {
    if (!fx) return NULL;
    for (int i = 0; i < MAXREG; i++)
        if (__atomic_load_n(&reg_fx[i], __ATOMIC_ACQUIRE) == fx) return __atomic_load_n(&reg_c[i], __ATOMIC_ACQUIRE);
    return NULL;
}
static int reg_add(AEffect *fx, child_t *c) {
    pthread_mutex_lock(&reg_mu);
    for (int i = 0; i < MAXREG; i++)
        if (!reg_fx[i]) {
            __atomic_store_n(&reg_c[i], c, __ATOMIC_RELEASE);
            __atomic_store_n(&reg_fx[i], fx, __ATOMIC_RELEASE);
            pthread_mutex_unlock(&reg_mu);
            return 1;
        }
    pthread_mutex_unlock(&reg_mu);
    return 0;
}
static void reg_del(AEffect *fx) {
    pthread_mutex_lock(&reg_mu);
    for (int i = 0; i < MAXREG; i++)
        if (reg_fx[i] == fx) __atomic_store_n(&reg_fx[i], NULL, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&reg_mu);
}

static void copy_str(void *dst, const char *src, int n) {
    if (!dst) return;
    strncpy(dst, src, n - 1);
    ((char *)dst)[n - 1] = 0;
}
static float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
static intptr_t disp(AEffect *fx, int32_t op, int32_t idx, intptr_t v, void *p, float o) { return fx->dispatcher(fx, op, idx, v, p, o); }

static void mark_slot(chain_t *w, int s) {
    for (int k = 0; k < POOL; k++) w->dirty[g_pool_param[s][k]] = 1;
    w->dirty[g_slot_param[s][S_PICK]] = w->dirty[g_slot_param[s][S_ON]] = w->dirty[g_slot_param[s][S_MIX]] = 1;
    w->need_update = 1;
}

/* ---- the host side: what children call ----------------------------------- */
static intptr_t child_master(AEffect *cfx, int32_t op, int32_t idx, intptr_t v, void *p, float o) {
    child_t *c = reg_find(cfx);
    if (!c) c = tl_loading;
    chain_t *w = c ? c->owner : NULL;
    switch (op) {
    case amVersion: return 2400;
    case amCurrentId: return c && c->fx ? c->fx->uniqueID : 0;
    case amAutomate:   /* a child changed one of its values: tell MPC, if a knob shows it */
        if (c && w && idx >= 0 && idx < c->nrev && c->rev[idx] >= 0) w->dirty[g_pool_param[c->slot][c->rev[idx]]] = 1;
        return 0;
    case amGetTime:
        if (!w) return 0;
        if (tl_proc == w) return (intptr_t)w->ti;   /* fetched once for the block, before the chain run */
        return w->master(&w->fx, amGetTime, idx, v, p, o);
    case amGetSampleRate: return w ? (intptr_t)w->sr : 44100;
    case amGetBlockSize: return w ? w->bs : 128;
    case amGetCurrentProcessLevel: return tl_proc ? 2 : 1;   /* realtime / user */
    case amGetAutomationState: return 1;                     /* off */
    case amGetVendorString: case amGetProductString: case amGetVendorVersion:
        return w ? w->master(&w->fx, op, idx, v, p, o) : 0;
    case amCanDo:
        return p && (!strcmp(p, "sendVstTimeInfo") || !strcmp(p, "startStopProcess")) ? 1 : 0;
    case amGetLanguage: return 1;   /* English */
    case amGetDirectory: return c ? (intptr_t)c->dir : 0;
    case amUpdateDisplay: if (w) w->need_update = 1; return 1;
    case amBeginEdit: case amEndEdit: return 1;
    case amPinConnected: return 0;  /* 0 = connected */
    case amIdle: case amIOChanged: case amSizeWindow: case amGetInputLatency: case amGetOutputLatency:
    default: return 0;
    }
}

/* Open a child (no locks held: it isn't in the chain yet). NULL and a reason in err on failure. */
static child_t *child_open(chain_t *w, int s, const char *name, const char *path, char *err, size_t errn) {
    child_t *c = calloc(1, sizeof *c);
    if (!c) { snprintf(err, errn, "out of memory"); return NULL; }
    c->owner = w;
    c->slot = s;
    snprintf(c->name, sizeof c->name, "%s", name);
    snprintf(c->path, sizeof c->path, "%s", path);
    snprintf(c->dir, sizeof c->dir, "%s", path);
    char *sl = strrchr(c->dir, '/');
    if (sl) *sl = 0;
    void *lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        const char *e = dlerror(), *colon = e ? strrchr(e, ':') : NULL;
        snprintf(err, errn, "can't open %s:%s", name, colon ? colon + 1 : " unknown error");
        free(c);
        return NULL;
    }
    vst_main_t vmain = (vst_main_t)dlsym(lib, "VSTPluginMain");
    if (!vmain) vmain = (vst_main_t)dlsym(lib, "main");
    tl_loading = c;
    AEffect *fx = vmain ? vmain(child_master) : NULL;
    tl_loading = NULL;
    /* the library stays loaded (never dlclose()d): MPC or another slot may share it, and a plugin's own threads
     * may outlive its effClose */
    if (!fx || fx->magic != VST_MAGIC) { snprintf(err, errn, "%s is not a VST2 plugin", name); free(c); return NULL; }
    const char *why = fx->uniqueID == (int32_t)PLUG_UID ? "is FXpander itself"
                    : (fx->flags & effFlagsIsSynth) ? "is an instrument"
                    : fx->numOutputs < 1 || fx->numOutputs > MAXCH || fx->numInputs < 0 || fx->numInputs > MAXCH ? "has unsupported channels"
                    : !reg_add(fx, c) ? "can't be registered (too many plugins)" : NULL;
    if (why) {
        snprintf(err, errn, "%s %s", name, why);
        tl_loading = c;
        disp(fx, effClose, 0, 0, NULL, 0);
        tl_loading = NULL;
        free(c);
        return NULL;
    }
    c->fx = fx;
    disp(fx, effOpen, 0, 0, NULL, 0);
    disp(fx, effSetSampleRate, 0, 0, NULL, w->sr);
    disp(fx, effSetBlockSize, 0, w->bs, NULL, 0);
    if (w->resumed) disp(fx, effMainsChanged, 0, 1, NULL, 0);
    if (w->started) disp(fx, effStartProcess, 0, 0, NULL, 0);
    int n = fx->numParams > 0 ? fx->numParams : 0;
    c->rev = malloc((n ? n : 1) * sizeof *c->rev);
    if (!c->rev) n = 0;
    c->nrev = n;
    for (int i = 0; i < n; i++) c->rev[i] = -1;
    for (int i = 0; i < n && c->nmap < POOL; i++)   /* its automatable parameters (meters and the like are not) */
        if (disp(fx, effCanBeAutomated, i, 0, NULL, 0) == 1) c->map[c->nmap++] = i;
    if (!c->nmap)                                   /* none marked: all of them */
        for (int i = 0; i < n && c->nmap < POOL; i++) c->map[c->nmap++] = i;
    for (int k = 0; k < c->nmap; k++) c->rev[c->map[k]] = (short)k;
    return c;
}

static void child_close(chain_t *w, child_t *c) {
    AEffect *fx = c->fx;
    if (w->started) disp(fx, effStopProcess, 0, 0, NULL, 0);
    if (w->resumed) disp(fx, effMainsChanged, 0, 0, NULL, 0);
    disp(fx, effClose, 0, 0, NULL, 0);
    reg_del(fx);
    free(c->rev);
    free(c);
}

static void orphan_drop(slot_t *sl) {
    free(sl->orphan);
    sl->orphan = NULL;
    sl->orphan_len = 0;
    sl->orphan_name[0] = 0;
}

/* Put c (or nothing) in slot s; the old child is closed after it's out of the chain. Under ctl. */
static void slot_swap(chain_t *w, int s, child_t *c) {
    pthread_mutex_lock(&w->lock);
    child_t *old = w->slot[s].c;
    w->slot[s].c = c;
    w->slot[s].g = 0;   /* the new one fades in */
    pthread_mutex_unlock(&w->lock);
    if (old) child_close(w, old);
    mark_slot(w, s);
}

static void status_loaded(chain_t *w, int s, const child_t *c) {
    if (c->fx->numParams > c->nmap)
        snprintf(w->status, sizeof w->status, "FX %d: %s (%d of %d params)", s + 1, c->name, c->nmap, c->fx->numParams);
    else
        snprintf(w->status, sizeof w->status, "FX %d: %s (%d params)", s + 1, c->name, c->nmap);
}

static void slot_load(chain_t *w, int s) {
    pthread_mutex_lock(&w->ctl);
    int pick = w->slot[s].pick;
    if (pick <= 0 || pick > g_nlist) {
        slot_swap(w, s, NULL);
        orphan_drop(&w->slot[s]);
        snprintf(w->status, sizeof w->status, "FX %d: empty", s + 1);
    } else {
        char err[TEXT_MAX];
        child_t *c = child_open(w, s, g_list[pick - 1].name, g_list[pick - 1].path, err, sizeof err);
        if (c) {
            slot_swap(w, s, c);
            orphan_drop(&w->slot[s]);
            status_loaded(w, s, c);
        } else snprintf(w->status, sizeof w->status, "FX %d: %s", s + 1, err);
    }
    w->need_update = 1;
    pthread_mutex_unlock(&w->ctl);
}

/* Swap slot s with its neighbour (d = -1 up, +1 down): plugin, settings, On and Mix go with it; no reload. */
static void slot_move(chain_t *w, int s, int d) {
    int t = s + d;
    if (t < 0 || t >= NSLOTS) return;
    pthread_mutex_lock(&w->ctl);
    pthread_mutex_lock(&w->lock);
    slot_t tmp = w->slot[s];
    w->slot[s] = w->slot[t];
    w->slot[t] = tmp;
    if (w->slot[s].c) w->slot[s].c->slot = s;
    if (w->slot[t].c) w->slot[t].c->slot = t;
    pthread_mutex_unlock(&w->lock);
    const slot_t *mv = &w->slot[t];
    snprintf(w->status, sizeof w->status, "%s moved to FX %d", mv->c ? mv->c->name : mv->orphan ? mv->orphan_name : "Empty", t + 1);
    mark_slot(w, s);
    mark_slot(w, t);
    pthread_mutex_unlock(&w->ctl);
}

/* ---- a diagnostic trace of the drop-down's traffic: on when /tmp/fxpander-trace exists at load, into
 * /tmp/fxpander-trace.log (ms, thread, what). Off, it costs one test of a NULL pointer. */
static FILE *g_trace;
static pthread_mutex_t g_trace_mu = PTHREAD_MUTEX_INITIALIZER;
static double now_s(void);

static int traced(int i) {
    return g_trace && i >= 0 && i < NPARAMS && (g_pi[i].kind == K_DD || g_pi[i].kind == K_DDROW || PARAMS[i].momentary);
}

__attribute__((format(printf, 1, 2))) static void trace(const char *fmt, ...) {
    if (!g_trace) return;
    va_list ap;
    va_start(ap, fmt);
    pthread_mutex_lock(&g_trace_mu);
    fprintf(g_trace, "%10.1f %08lx ", now_s() * 1000, (unsigned long)pthread_self() & 0xffffffffUL);
    vfprintf(g_trace, fmt, ap);
    fputc('\n', g_trace);
    fflush(g_trace);
    pthread_mutex_unlock(&g_trace_mu);
    va_end(ap);
}

/* ---- the chain ------------------------------------------------------------ */
/* Tell MPC param i's value. MPC calls setParameter(i, v) back from inside audioMasterAutomate (mpc-vst-plugins
 * docs/NOTES.md, 2026-10-08): that echo is not a user's touch, so setParameter ignores it (tl_echo). */
static void report(chain_t *w, int i, float v) {
    if (traced(i)) trace("report %s = %g", PARAMS[i].key, v);
    tl_echo = i;
    w->master(&w->fx, amAutomate, i, 0, NULL, v);
    tl_echo = -1;
}

static void reports(chain_t *w) {
    for (int i = 0; i < NPARAMS; i++)
        if (w->hold[i]) { w->hold[i] = 0; report(w, i, 0.0f); }
    int si = g_dd_param[DD_SHOWN];   /* the drop-down's open state first: the list shows or hides on it */
    if (w->dirty[si]) { w->dirty[si] = 0; report(w, si, w->dd_open ? 1.0f : 0.0f); }
    int sent = 0;
    for (int j = 0; j < NPARAMS && sent < REPORTS_PER_BLOCK; j++) {
        int i = (w->rep_next + j) % NPARAMS;
        if (!w->dirty[i]) continue;
        w->dirty[i] = 0;
        report(w, i, w->fx.getParameter(&w->fx, i));
        sent++;
        w->rep_next = (i + 1) % NPARAMS;
    }
    if (w->need_update) { w->need_update = 0; w->master(&w->fx, amUpdateDisplay, 0, 0, NULL, 0.0f); }
}

/* One pass of at most MAXBLK frames through the slots; the result's buffers are returned in res. Under lock. */
static void chain_pass(chain_t *w, float **in, int off, int n, float **res) {
    int cur = 0;
    float *L = w->buf[0][0], *R = w->buf[0][1];
    for (int i = 0; i < n; i++) {
        L[i] = in && in[0] ? in[0][off + i] : 0;
        R[i] = in && in[1] ? in[1][off + i] : 0;
    }
    for (int s = 0; s < NSLOTS; s++) {
        slot_t *sl = &w->slot[s];
        child_t *c = sl->c;
        float target = c && sl->on ? clamp01(sl->mix) : 0;
        if (!c || (target == 0 && sl->g == 0)) { sl->g = target; continue; }
        AEffect *fx = c->fx;
        float *dl = w->buf[cur][0], *dr = w->buf[cur][1], *wl = w->buf[!cur][0], *wr = w->buf[!cur][1];
        float *ins[MAXCH], *outs[MAXCH];
        for (int ch = 0; ch < MAXCH; ch++) { ins[ch] = w->zero; outs[ch] = w->junk; }
        if (fx->numInputs == 1) {
            for (int i = 0; i < n; i++) w->mono[i] = 0.5f * (dl[i] + dr[i]);
            ins[0] = w->mono;
        } else { ins[0] = dl; ins[1] = dr; }
        outs[0] = wl;
        outs[1] = wr;
        if (fx->flags & effFlagsCanReplacing && fx->processReplacing) fx->processReplacing(fx, ins, outs, n);
        else {
            memset(wl, 0, n * sizeof *wl);
            memset(wr, 0, n * sizeof *wr);
            fx->process(fx, ins, outs, n);
        }
        if (fx->numOutputs == 1) memcpy(wr, wl, n * sizeof *wr);
        float g0 = sl->g, g1 = target;
        if (g0 != 1 || g1 != 1)   /* dry/wet, ramped across the pass so On/Mix changes don't click */
            for (int i = 0; i < n; i++) {
                float g = g0 + (g1 - g0) * (float)(i + 1) / n;
                wl[i] = dl[i] + g * (wl[i] - dl[i]);
                wr[i] = dr[i] + g * (wr[i] - dr[i]);
            }
        sl->g = g1;
        cur = !cur;
    }
    res[0] = w->buf[cur][0];
    res[1] = w->buf[cur][1];
}

static void run(AEffect *e, float **in, float **out, int32_t n, int accumulate) {
    chain_t *w = e->object;
    w->ti = (VstTimeInfo *)w->master(e, amGetTime, 0, TIME_ALL, NULL, 0.0f);
    for (int off = 0; off < n;) {
        int m = n - off < MAXBLK ? n - off : MAXBLK;
        float *res[2];
        pthread_mutex_lock(&w->lock);
        tl_proc = w;
        chain_pass(w, in, off, m, res);
        tl_proc = NULL;
        for (int ch = 0; ch < 2; ch++) {
            if (!out || !out[ch]) continue;
            if (accumulate) for (int i = 0; i < m; i++) out[ch][off + i] += res[ch][i];
            else memcpy(out[ch] + off, res[ch], m * sizeof(float));
        }
        pthread_mutex_unlock(&w->lock);
        off += m;
    }
    reports(w);
}
static void processReplacing(AEffect *e, float **in, float **out, int32_t n) { run(e, in, out, n, 0); }
static void process(AEffect *e, float **in, float **out, int32_t n) { run(e, in, out, n, 1); }

/* ---- state (MPC saves an effect's settings only as a chunk; chain presets are the same bytes in a file) ----------
 * "FXP1", u32 version, u32 slots; per slot: u8 on, f32 mix, u32 uid, u16 + path, u16 + name, u8 kind, u32 + data;
 * version 2 adds u32 program after the slots. kind 0: no plugin; 1: the child's own chunk; 2: u32 count + its
 * parameter values. Little-endian, as the device. Version 1 (FXpander 1000) had 4 slots. */
typedef struct { unsigned char *p; size_t len, cap; int bad; } wbuf_t;

static void put(wbuf_t *b, const void *d, size_t n) {
    if (b->bad) return;
    if (b->len + n > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        while (cap < b->len + n) cap *= 2;
        unsigned char *np = realloc(b->p, cap);
        if (!np) { b->bad = 1; return; }
        b->p = np;
        b->cap = cap;
    }
    memcpy(b->p + b->len, d, n);
    b->len += n;
}
static void put_u8(wbuf_t *b, uint8_t v) { put(b, &v, 1); }
static void put_u16(wbuf_t *b, uint16_t v) { put(b, &v, 2); }
static void put_u32(wbuf_t *b, uint32_t v) { put(b, &v, 4); }
static void put_str(wbuf_t *b, const char *s) { size_t n = strlen(s); put_u16(b, (uint16_t)n); put(b, s, n); }

typedef struct { const unsigned char *p; size_t len, pos; int bad; } rbuf_t;
static const void *get(rbuf_t *b, size_t n) {
    if (b->bad || b->len - b->pos < n) { b->bad = 1; return NULL; }
    const void *r = b->p + b->pos;
    b->pos += n;
    return r;
}
static uint32_t get_u32(rbuf_t *b) { uint32_t v = 0; const void *p = get(b, 4); if (p) memcpy(&v, p, 4); return v; }
static uint16_t get_u16(rbuf_t *b) { uint16_t v = 0; const void *p = get(b, 2); if (p) memcpy(&v, p, 2); return v; }
static uint8_t get_u8(rbuf_t *b) { const uint8_t *p = get(b, 1); return p ? *p : 0; }
static void get_str(rbuf_t *b, char *out, size_t n) {
    size_t len = get_u16(b);
    const char *p = get(b, len);
    if (!p) { out[0] = 0; return; }
    if (len > n - 1) len = n - 1;
    memcpy(out, p, len);
    out[len] = 0;
}

/* everything after on/mix for one slot (also what an orphan keeps) */
static void put_slot_body(wbuf_t *b, child_t *c) {
    if (!c) { put_u32(b, 0); put_str(b, ""); put_str(b, ""); put_u8(b, 0); put_u32(b, 0); return; }
    AEffect *fx = c->fx;
    put_u32(b, (uint32_t)fx->uniqueID);
    put_str(b, c->path);
    put_str(b, c->name);
    void *data = NULL;
    intptr_t n = fx->flags & effFlagsProgramChunks ? disp(fx, effGetChunk, 0, 0, &data, 0) : 0;
    if (n > 0 && data) {
        put_u8(b, 1);
        put_u32(b, (uint32_t)n);
        put(b, data, (size_t)n);
    } else {
        int np = fx->numParams > 0 ? fx->numParams : 0;
        put_u8(b, 2);
        put_u32(b, 4 + 4 * (uint32_t)np);
        put_u32(b, (uint32_t)np);
        for (int i = 0; i < np; i++) { float v = fx->getParameter(fx, i); put(b, &v, 4); }
    }
}

/* the whole chain into b. Under ctl. */
static void put_chain(chain_t *w, wbuf_t *b) {
    put(b, "FXP1", 4);
    put_u32(b, 2);
    put_u32(b, NSLOTS);
    for (int s = 0; s < NSLOTS; s++) {
        slot_t *sl = &w->slot[s];
        float mix = sl->mix;
        put_u8(b, (uint8_t)sl->on);
        put(b, &mix, 4);
        if (!sl->c && sl->orphan) put(b, sl->orphan, sl->orphan_len);
        else put_slot_body(b, sl->c);
    }
    put_u32(b, (uint32_t)w->program);
}

static intptr_t get_chunk(chain_t *w, void **out) {
    wbuf_t b = {w->chunk, 0, w->chunk_cap, 0};
    pthread_mutex_lock(&w->ctl);
    put_chain(w, &b);
    pthread_mutex_unlock(&w->ctl);
    w->chunk = b.p;
    w->chunk_cap = b.cap;
    if (b.bad) return 0;
    *out = w->chunk;
    return (intptr_t)b.len;
}

static void restore_state(child_t *c, uint8_t kind, const unsigned char *data, uint32_t len) {
    AEffect *fx = c->fx;
    if (kind == 1 && len) disp(fx, effSetChunk, 0, (intptr_t)len, (void *)data, 0);
    else if (kind == 2 && len >= 4) {
        uint32_t np;
        memcpy(&np, data, 4);
        for (uint32_t i = 0; i < np && 4 + 4 * (i + 1) <= len && (int)i < fx->numParams; i++) {
            float v;
            memcpy(&v, data + 4 + 4 * i, 4);
            fx->setParameter(fx, (int32_t)i, v);
        }
    }
}

/* Restore a chain (a project chunk, or a preset with every slot set). keep_program: take the saved PRESET-menu pick
 * too (a project; a preset recall sets it itself). Under ctl. 1 if it parsed. */
static int set_chain(chain_t *w, const unsigned char *data, size_t len, int keep_program) {
    rbuf_t b = {data, len, 0, 0};
    const char *magic = get(&b, 4);
    if (!magic || memcmp(magic, "FXP1", 4)) return 0;
    uint32_t version = get_u32(&b);
    uint32_t ns = get_u32(&b);
    w->status[0] = 0;
    for (uint32_t s = 0; s < ns && !b.bad; s++) {
        uint8_t on = get_u8(&b);
        float mix = 1;
        const void *mp = get(&b, 4);
        if (mp) memcpy(&mix, mp, 4);
        size_t body = b.pos;
        get_u32(&b);   /* uid */
        char path[512], name[TEXT_MAX];
        get_str(&b, path, sizeof path);
        get_str(&b, name, sizeof name);
        uint8_t kind = get_u8(&b);
        uint32_t dlen = get_u32(&b);
        const unsigned char *d = get(&b, dlen);
        if (b.bad || s >= NSLOTS) continue;
        slot_t *sl = &w->slot[s];
        sl->on = on;
        sl->mix = clamp01(mix);
        orphan_drop(sl);
        if (!path[0]) { slot_swap(w, (int)s, NULL); sl->pick = 0; continue; }
        int li = list_find(path);
        sl->pick = li + 1;
        child_t *c = sl->c;
        if (c && !strcmp(c->path, path)) {   /* the same plugin: just its settings */
            restore_state(c, kind, d, dlen);
            mark_slot(w, (int)s);
            continue;
        }
        char err[TEXT_MAX];
        c = child_open(w, (int)s, name, path, err, sizeof err);
        if (c) {
            restore_state(c, kind, d, dlen);
            slot_swap(w, (int)s, c);
        } else {   /* keep what was saved, so saving again doesn't lose it */
            slot_swap(w, (int)s, NULL);
            sl->orphan_len = b.pos - body;
            sl->orphan = malloc(sl->orphan_len);
            if (sl->orphan) memcpy(sl->orphan, data + body, sl->orphan_len);
            else sl->orphan_len = 0;
            snprintf(sl->orphan_name, sizeof sl->orphan_name, "%s", name);
            snprintf(w->status, sizeof w->status, "FX %u: %s", s + 1, err);
        }
    }
    for (uint32_t s = ns; s < NSLOTS; s++) {   /* slots the saved chain didn't have (a 4-slot one): empty */
        slot_swap(w, (int)s, NULL);
        orphan_drop(&w->slot[s]);
        w->slot[s].pick = 0;
        w->slot[s].on = 1;
        w->slot[s].mix = 1;
    }
    int ok = !b.bad;
    if (ok && version >= 2) {
        uint32_t prog = get_u32(&b);
        ok = !b.bad;
        if (ok && keep_program && prog <= NPRESETS) w->program = (int)prog;
    }
    w->need_update = 1;
    return ok;
}

static int set_chunk(chain_t *w, const unsigned char *data, size_t len) {
    pthread_mutex_lock(&w->ctl);
    int ok = set_chain(w, data, len, 1);
    pthread_mutex_unlock(&w->ctl);
    return ok;
}

/* ---- chain presets -------------------------------------------------------------------------------------------- */
static void chain_info(chain_t *w, char *out, size_t n) {
    out[0] = 0;
    for (int s = 0; s < NSLOTS; s++) {
        const slot_t *sl = &w->slot[s];
        const char *nm = sl->c ? sl->c->name : sl->orphan ? sl->orphan_name : NULL;
        if (!nm) continue;
        size_t l = strlen(out);
        snprintf(out + l, n - l, "%s%s", l ? " > " : "", nm);
    }
    if (!out[0]) snprintf(out, n, "(nothing loaded)");
}

static void pr_save(chain_t *w, int i) {
    pthread_mutex_lock(&w->ctl);
    pthread_mutex_lock(&g_pr_mu);
    preset_t p = {1, "", ""};
    chain_info(w, p.info, sizeof p.info);
    snprintf(p.name, sizeof p.name, "%s", w->pr_name[0] ? w->pr_name : p.info);   /* no name typed: its contents */
    wbuf_t b = {NULL, 0, 0, 0};
    put(&b, "FXPR", 4);
    put_u32(&b, 1);
    put_str(&b, p.name);
    put_str(&b, p.info);
    size_t at = b.len;
    put_u32(&b, 0);
    put_chain(w, &b);
    uint32_t clen = (uint32_t)(b.len - at - 4);
    if (!b.bad) memcpy(b.p + at, &clen, 4);
    char path[700], tmp[720];
    pr_path(i, path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.new", path);
    mkdir(g_pr_dir, 0755);
    FILE *f = b.bad ? NULL : fopen(tmp, "wb");
    int ok = f && fwrite(b.p, 1, b.len, f) == b.len;
    if (f) ok &= fclose(f) == 0;
    ok = ok && rename(tmp, path) == 0;
    if (ok) {
        g_pr[i] = p;
        snprintf(w->pr_name, sizeof w->pr_name, "%s", p.name);
        snprintf(w->status, sizeof w->status, "Saved preset %02d %s", i + 1, p.name);
    } else {
        unlink(tmp);
        snprintf(w->status, sizeof w->status, "Can't save preset %02d: %s", i + 1, strerror(errno ? errno : EIO));
    }
    free(b.p);
    pthread_mutex_unlock(&g_pr_mu);
    w->need_update = 1;
    pthread_mutex_unlock(&w->ctl);
}

static int pr_recall(chain_t *w, int i) {
    char path[700];
    pthread_mutex_lock(&g_pr_mu);
    pr_path(i, path, sizeof path);
    pthread_mutex_unlock(&g_pr_mu);
    FILE *f = fopen(path, "rb");
    preset_t p;
    uint32_t len = 0;
    unsigned char *data = NULL;
    int ok = f && pr_read_head(f, &p) && fread(&len, 4, 1, f) == 1 && len < (64u << 20) && (data = malloc(len ? len : 1)) &&
             fread(data, 1, len, f) == len;
    if (f) fclose(f);
    pthread_mutex_lock(&w->ctl);
    if (ok) ok = set_chain(w, data, len, 0);
    if (ok) {
        if (!w->status[0]) snprintf(w->status, sizeof w->status, "Recalled preset %02d %s", i + 1, p.name);
        snprintf(w->pr_name, sizeof w->pr_name, "%s", p.name);
    } else snprintf(w->status, sizeof w->status, "Preset %02d is empty", i + 1);
    w->need_update = 1;
    pthread_mutex_unlock(&w->ctl);
    free(data);
    return ok;
}

static void pr_delete(chain_t *w, int i) {
    char path[700];
    pthread_mutex_lock(&g_pr_mu);
    pr_path(i, path, sizeof path);
    if (g_pr[i].used && unlink(path) == 0) {
        memset(&g_pr[i], 0, sizeof g_pr[i]);
        snprintf(w->status, sizeof w->status, "Deleted preset %02d", i + 1);
    } else snprintf(w->status, sizeof w->status, "Preset %02d is empty", i + 1);
    pthread_mutex_unlock(&g_pr_mu);
    w->need_update = 1;
}

static void pr_select(chain_t *w, int i) {
    w->pr_sel = i < 0 ? 0 : i >= NPRESETS ? NPRESETS - 1 : i;
    pthread_mutex_lock(&g_pr_mu);
    if (g_pr[w->pr_sel].used) snprintf(w->pr_name, sizeof w->pr_name, "%s", g_pr[w->pr_sel].name);
    pthread_mutex_unlock(&g_pr_mu);
    w->dirty[g_pr_param[PR_SLOT]] = 1;
    w->need_update = 1;
}

static void key_press(chain_t *w, char c) {
    size_t l = strlen(w->pr_name);
    if (c == KEY_DEL) { if (l) w->pr_name[l - 1] = 0; }
    else if (c == KEY_CLEAR) w->pr_name[0] = 0;
    else if (l < NAME_MAX_LEN && !(c == ' ' && (l == 0 || w->pr_name[l - 1] == ' '))) { w->pr_name[l] = c; w->pr_name[l + 1] = 0; }
    w->need_update = 1;
}

/* ---- the drop-down plugin list ---------------------------------------------------------------------------------
 * A slot's field (s<n>_drop) opens it over the page: dd_shown drives the list's when=, dd_1..dd_52 are its rows
 * (g_list from page * 52), lit on the slot's current plugin. A tap on a row loads that plugin and closes it. */
static int dd_pages(void) { return g_nlist ? (g_nlist + DD_ROWS - 1) / DD_ROWS : 1; }

/* the list entry the slot's plugin is (-1: none, or not in the list) */
static int slot_entry(chain_t *w, int s) {
    pthread_mutex_lock(&w->lock);
    int li = w->slot[s].c ? list_find(w->slot[s].c->path) : -1;
    pthread_mutex_unlock(&w->lock);
    return li;
}

static void dd_refresh(chain_t *w) {
    w->dirty[g_dd_param[DD_SHOWN]] = 1;
    for (int r = 0; r < DD_ROWS; r++) w->dirty[g_ddrow_param[r]] = 1;
    w->need_update = 1;
}

static double now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

static void dd_show(chain_t *w, int s) {
    int li = slot_entry(w, s);
    w->dd_slot = s;
    w->dd_page = li >= 0 ? li / DD_ROWS : 0;
    w->dd_opened = now_s();
    w->dd_open = 1;
    trace("list OPEN for FX %d", s + 1);
    dd_refresh(w);
}

static void dd_hide(chain_t *w) {
    trace("list CLOSE");
    w->dd_open = 0;
    dd_refresh(w);
}

/* A row set by MPC. A touch toggles the row (MPC flips it before calling us), so it differs from the row's lit state;
 * a value equal to it is a late echo of a report, not a touch. Nothing counts in the first 300 ms after opening:
 * the rows are being reported then, and no finger is that quick. */
static void dd_tap(chain_t *w, int row, float v) {
    int li = w->dd_page * DD_ROWS + row;
    float lit = w->fx.getParameter(&w->fx, g_ddrow_param[row]);
    if ((v > 0.5f) == (lit > 0.5f)) return;
    if (!w->dd_open || li >= g_nlist || now_s() - w->dd_opened < 0.3) {   /* not a pick: put MPC's toggle back */
        w->dirty[g_ddrow_param[row]] = 1;
        return;
    }
    int s = w->dd_slot;
    dd_hide(w);
    if (li == slot_entry(w, s)) return;   /* already loaded: keep it (and its settings) */
    w->slot[s].pick = li + 1;
    slot_load(w, s);
}

static void program_name(int n, char *out, int len) {
    if (n <= 0) { snprintf(out, len, "- Chain presets -"); return; }
    pthread_mutex_lock(&g_pr_mu);
    if (n <= NPRESETS && g_pr[n - 1].used) snprintf(out, len, "%02d %s", n, g_pr[n - 1].name);
    else snprintf(out, len, "%02d -", n);
    pthread_mutex_unlock(&g_pr_mu);
}

/* ---- parameters ------------------------------------------------------------ */
/* a stepped value (0..max) moved by the host: the data wheel and a Q-Link nudge by about one step from the value read
 * back, so move one; a bigger jump (a drag, automation) goes where it says */
static int nudge(int cur, float v, int max, int limit) {
    float d = v * max - cur;
    if (fabsf(d) < 0.001f) return cur;
    int np = fabsf(d) < 2.0f ? cur + (d > 0 ? 1 : -1) : (int)lroundf(v * max);
    return np < 0 ? 0 : np > limit ? limit : np;
}

static void setParameter(AEffect *e, int32_t i, float v) {
    chain_t *w = e->object;
    if (traced(i)) trace("set %s = %g%s", PARAMS[i].key, v, i == tl_echo ? " (echo, ignored)" : "");
    if (i < 0 || i >= NPARAMS || i == tl_echo) return;   /* MPC echoing what we just reported */
    v = clamp01(v);
    const pinfo_t *pi = &g_pi[i];
    if (PARAMS[i].momentary) {
        /* A button (Mouse Down -> Toggle Switch) sets 1 when pressed; we report it back to 0 (hold), and the finger
         * lifting toggles it to 1 again (its Button part toggles on release; seen in the trace on the drop-down field,
         * 2026-10-26: a second set ~250 ms after every press). So a press fires and the next 1 within 1.2 s is its
         * release. Presses and releases alternate, so quick repeated taps all count. 0 is never a press. A stepper
         * arrow has no Mouse Down action, only the release: one 1 per tap, so it isn't paired. */
        if (v <= 0.5f) return;
        int arrow = (pi->kind == K_SLOT && (pi->f == S_PREV || pi->f == S_NEXT)) || (pi->kind == K_PR && (pi->f == PR_PREV || pi->f == PR_NEXT));
        double t = now_s();
        if (!arrow && w->release_due[i] && t - w->pressed_at[i] < 1.2) { w->release_due[i] = 0; return; }
        w->release_due[i] = 1;
        w->pressed_at[i] = t;
    }
    if (pi->kind == K_POOL) {
        pthread_mutex_lock(&w->lock);
        child_t *c = w->slot[pi->s].c;
        if (c && pi->f < c->nmap) c->fx->setParameter(c->fx, c->map[pi->f], v);
        pthread_mutex_unlock(&w->lock);
        return;
    }
    int on = v > 0.5f;
    if (pi->kind == K_KEY) { if (on) { key_press(w, (char)pi->f); w->hold[i] = 1; } return; }
    if (pi->kind == K_DDROW) { dd_tap(w, pi->f, v); return; }
    if (pi->kind == K_DD) {
        switch (pi->f) {
        case DD_SHOWN: break;   /* display only (the list's when=): a late echo must not hide it */
        case DD_PREV: case DD_NEXT:
            if (on) {
                int np = dd_pages();
                w->dd_page = (w->dd_page + (pi->f == DD_NEXT ? 1 : np - 1)) % np;
                dd_refresh(w);
                w->hold[i] = 1;
            }
            break;
        case DD_CLOSE: if (on) { dd_hide(w); w->hold[i] = 1; } break;
        default: break;
        }
        return;
    }
    if (pi->kind == K_PR) {
        switch (pi->f) {
        case PR_SLOT: pr_select(w, nudge(w->pr_sel, v, NPRESETS - 1, NPRESETS - 1)); break;
        case PR_PREV: case PR_NEXT:
            if (on) { pr_select(w, (w->pr_sel + (pi->f == PR_NEXT ? 1 : NPRESETS - 1)) % NPRESETS); w->hold[i] = 1; }
            break;
        case PR_RECALL: if (on) { if (pr_recall(w, w->pr_sel)) w->program = w->pr_sel + 1; w->hold[i] = 1; } break;
        case PR_SAVE: if (on) { pr_save(w, w->pr_sel); w->hold[i] = 1; } break;
        case PR_DELETE: if (on) { pr_delete(w, w->pr_sel); w->hold[i] = 1; } break;
        default: break;
        }
        return;
    }
    if (pi->kind != K_SLOT) return;   /* the status readout (param 0) and anything unknown */
    int s = pi->s;
    slot_t *sl = &w->slot[s];
    switch (pi->f) {
    case S_PICK: sl->pick = nudge(sl->pick, v, PICK_MAX, g_nlist); w->need_update = 1; break;
    case S_PREV: case S_NEXT:
        if (on) {
            int n1 = g_nlist + 1, d = pi->f == S_NEXT ? 1 : -1;
            sl->pick = ((sl->pick + d) % n1 + n1) % n1;
            w->dirty[g_slot_param[s][S_PICK]] = 1;
            w->need_update = 1;
            w->hold[i] = 1;
        }
        break;
    case S_LOAD: if (on) { slot_load(w, s); w->hold[i] = 1; } break;
    case S_CLEAR:
        if (on) {
            pthread_mutex_lock(&w->ctl);
            slot_swap(w, s, NULL);
            orphan_drop(sl);
            sl->pick = 0;
            snprintf(w->status, sizeof w->status, "FX %d: cleared", s + 1);
            pthread_mutex_unlock(&w->ctl);
            w->hold[i] = 1;
        }
        break;
    case S_UP: case S_DOWN: if (on) { slot_move(w, s, pi->f == S_UP ? -1 : 1); w->hold[i] = 1; } break;
    case S_DROP:
        /* only ever opens (CLOSE or a pick closes it): whatever MPC sends when the finger lifts can't close it */
        if (on && !(w->dd_open && w->dd_slot == s)) dd_show(w, s);
        if (on) w->hold[i] = 1;
        break;
    case S_ON: sl->on = on; break;
    case S_MIX: sl->mix = v; break;
    default: break;
    }
}

static float getParameter(AEffect *e, int32_t i) {
    chain_t *w = e->object;
    if (i < 0 || i >= NPARAMS) return 0;
    const pinfo_t *pi = &g_pi[i];
    if (pi->kind == K_POOL) {
        float v = 0;
        pthread_mutex_lock(&w->lock);
        child_t *c = w->slot[pi->s].c;
        if (c && pi->f < c->nmap) v = c->fx->getParameter(c->fx, c->map[pi->f]);
        pthread_mutex_unlock(&w->lock);
        return v;
    }
    if (pi->kind == K_PR && pi->f == PR_SLOT) return (float)w->pr_sel / (NPRESETS - 1);
    if (pi->kind == K_DD) return pi->f == DD_SHOWN && w->dd_open ? 1.0f : 0.0f;
    if (pi->kind == K_DDROW)   /* lit: the slot's current plugin */
        return w->dd_open && w->dd_page * DD_ROWS + pi->f == slot_entry(w, w->dd_slot) ? 1.0f : 0.0f;
    if (pi->kind != K_SLOT) return 0;
    slot_t *sl = &w->slot[pi->s];
    switch (pi->f) {
    case S_PICK: return (float)sl->pick / PICK_MAX;
    case S_ON: return sl->on ? 1.0f : 0.0f;
    case S_MIX: return sl->mix;
    default: return 0;
    }
}

/* a pool knob's name or value text from its child ("" when the knob maps to nothing) */
static void pool_text(chain_t *w, int s, int k, int op, char *out, int n) {
    char a[256] = "", b[256] = "";
    pthread_mutex_lock(&w->lock);
    child_t *c = w->slot[s].c;
    if (c && k < c->nmap) {
        disp(c->fx, op, c->map[k], 0, a, 0);
        if (op == effGetParamDisplay) disp(c->fx, effGetParamLabel, c->map[k], 0, b, 0);
    }
    pthread_mutex_unlock(&w->lock);
    a[sizeof a - 1] = b[sizeof b - 1] = 0;
    char *t = a, *u = b;
    while (*t == ' ') t++;
    while (*u == ' ') u++;
    size_t tl = strlen(t), ul = strlen(u);
    while (tl && t[tl - 1] == ' ') t[--tl] = 0;
    /* the unit (effGetParamLabel), unless the value text already ends with it (plugins on the mpc-vst-plugins
     * wrapper put it in both) */
    if (ul && !(tl >= ul && !strcasecmp(t + tl - ul, u))) snprintf(out, n, "%s %s", t, u);
    else snprintf(out, n, "%s", t);
}

static void slot_name(chain_t *w, int s, char *out, int n) {
    pthread_mutex_lock(&w->lock);
    child_t *c = w->slot[s].c;
    if (c) snprintf(out, n, "%s", c->name);
    else if (w->slot[s].orphan) snprintf(out, n, "%s (missing)", w->slot[s].orphan_name);
    else snprintf(out, n, "Empty");
    pthread_mutex_unlock(&w->lock);
}

static void param_display(chain_t *w, int i, char *out, int n) {
    const pinfo_t *pi = &g_pi[i];
    out[0] = 0;
    switch (pi->kind) {
    case K_POOL: pool_text(w, pi->s, pi->f, effGetParamDisplay, out, n); return;
    case K_STATUS: snprintf(out, n, "%s", w->status); return;
    case K_PR:
        if (pi->f == PR_SLOT) program_name(w->pr_sel + 1, out, n);
        else if (pi->f == PR_NAME) snprintf(out, n, "%s_", w->pr_name);   /* the cursor */
        else if (pi->f == PR_INFO) {
            pthread_mutex_lock(&g_pr_mu);
            snprintf(out, n, "%s", g_pr[w->pr_sel].used ? g_pr[w->pr_sel].info : "empty slot");
            pthread_mutex_unlock(&g_pr_mu);
        }
        return;
    case K_DD:
        if (pi->f == DD_SHOWN) snprintf(out, n, "%s", w->dd_open ? "On" : "Off");
        else if (pi->f == DD_TITLE) {
            if (dd_pages() > 1) snprintf(out, n, "Choose FX %d   page %d/%d", w->dd_slot + 1, w->dd_page + 1, dd_pages());
            else snprintf(out, n, "Choose FX %d", w->dd_slot + 1);
        }
        return;
    case K_DDROW: {
        int li = w->dd_page * DD_ROWS + pi->f;
        if (li < g_nlist) snprintf(out, n, "%s", g_list[li].name);
        return;
    }
    case K_SLOT: break;
    default: return;
    }
    slot_t *sl = &w->slot[pi->s];
    switch (pi->f) {
    case S_PICK:
        if (sl->pick <= 0 || sl->pick > g_nlist) snprintf(out, n, "%s", g_nlist ? "- Empty -" : "No effects found");
        else snprintf(out, n, "%s", g_list[sl->pick - 1].name);
        break;
    case S_NAME: slot_name(w, pi->s, out, n); break;
    case S_ON: snprintf(out, n, "%s", sl->on ? "On" : "Off"); break;
    case S_MIX: snprintf(out, n, "%d%%", (int)lroundf(sl->mix * 100)); break;
    default: break;
    }
}

/* ---- dispatcher -------------------------------------------------------------- */
static void each_child(chain_t *w, int32_t op, intptr_t v, float o) {
    pthread_mutex_lock(&w->ctl);
    for (int s = 0; s < NSLOTS; s++)
        if (w->slot[s].c) disp(w->slot[s].c->fx, op, 0, v, NULL, o);
    pthread_mutex_unlock(&w->ctl);
}

static void chain_free(chain_t *w) {
    pthread_mutex_lock(&w->ctl);
    for (int s = 0; s < NSLOTS; s++) { slot_swap(w, s, NULL); orphan_drop(&w->slot[s]); }
    pthread_mutex_unlock(&w->ctl);
    pthread_mutex_destroy(&w->lock);
    pthread_mutex_destroy(&w->ctl);
    free(w->chunk);
    free(w);
}

static intptr_t dispatcher(AEffect *e, int32_t op, int32_t idx, intptr_t v, void *p, float o) {
    chain_t *w = e->object;
    char buf[TEXT_MAX];
    switch (op) {
    case effOpen: return 1;
    case effClose: chain_free(w); return 1;
    case effGetPlugCategory: return 1;   /* kPlugCategEffect */
    case effGetEffectName: case effGetProductString: copy_str(p, PLUG_NAME, 32); return 1;
    case effGetVendorString: copy_str(p, PLUG_VENDOR, 32); return 1;
    case effGetVendorVersion: return PLUG_VERSION;
    case effGetVstVersion: return 2400;
    case effCanBeAutomated:
        if (idx < 0 || idx >= NPARAMS) return 0;
        return g_pi[idx].kind == K_POOL || (g_pi[idx].kind == K_SLOT && (g_pi[idx].f == S_ON || g_pi[idx].f == S_MIX || g_pi[idx].f == S_PICK));
    case effGetParamName:
        if (idx < 0 || idx >= NPARAMS) return 0;
        if (g_pi[idx].kind == K_POOL) pool_text(w, g_pi[idx].s, g_pi[idx].f, effGetParamName, buf, sizeof buf);
        else snprintf(buf, sizeof buf, "%s", PARAMS[idx].name);
        copy_str(p, buf, 32);
        return 1;
    case effGetParamLabel: copy_str(p, "", 8); return 1;
    case effGetParamDisplay:
        if (idx < 0 || idx >= NPARAMS) return 0;
        param_display(w, idx, buf, sizeof buf);
        copy_str(p, buf, TEXT_MAX);
        return 1;
    case effSetSampleRate: w->sr = o > 0 ? o : 44100; each_child(w, op, 0, w->sr); return 1;
    case effSetBlockSize: w->bs = v > 0 ? (int)v : 128; each_child(w, op, w->bs, 0); return 1;
    case effMainsChanged: w->resumed = v != 0; each_child(w, op, v, 0); return 1;
    case effStartProcess: w->started = 1; each_child(w, op, 0, 0); return 1;
    case effStopProcess: w->started = 0; each_child(w, op, 0, 0); return 1;
    case effGetChunk: return p ? get_chunk(w, (void **)p) : 0;
    case effSetChunk: return v > 0 && p ? set_chunk(w, p, (size_t)v) : 0;
    /* MPC's PRESET menu: program 0 is a do-nothing heading, n the chain preset n. JUCE re-picks the current program at
     * load (and the project chunk carries it), so a re-pick is ignored rather than reloading the chain over the
     * project's own settings. */
    case effSetProgram:
        if (v < 0 || v > NPRESETS || v == w->program) return 0;
        w->program = (int)v;
        if (v > 0) { pr_select(w, (int)v - 1); pr_recall(w, (int)v - 1); }
        return 1;
    case effGetProgram: return w->program;
    case effGetProgramName: program_name(w->program, buf, sizeof buf); copy_str(p, buf, 24); return 1;
    case effGetProgramNameIndexed:
        if (idx < 0 || idx > NPRESETS) return 0;
        program_name(idx, buf, sizeof buf);
        copy_str(p, buf, 24);
        return 1;
    case effCanDo: return 0;
    default: return 0;
    }
}

__attribute__((visibility("default"))) AEffect *VSTPluginMain(audioMasterCallback master) {
    pthread_once(&g_params_once, params_init);
    if (!g_params_ok) return NULL;   /* params.h doesn't match: regenerate it (vst/make_skin.py + gen_vst.py) */
    pthread_once(&g_list_once, load_list);
    pthread_once(&g_pr_once, pr_scan);
    pthread_mutex_lock(&g_trace_mu);
    if (!g_trace && access("/tmp/fxpander-trace", F_OK) == 0) g_trace = fopen("/tmp/fxpander-trace.log", "a");
    pthread_mutex_unlock(&g_trace_mu);
    chain_t *w = calloc(1, sizeof *w);
    if (!w) return NULL;
    pthread_mutexattr_t ma;
    pthread_mutexattr_init(&ma);
    pthread_mutexattr_settype(&ma, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutexattr_setprotocol(&ma, PTHREAD_PRIO_INHERIT);
    pthread_mutex_init(&w->lock, &ma);
    pthread_mutexattr_setprotocol(&ma, PTHREAD_PRIO_NONE);
    pthread_mutex_init(&w->ctl, &ma);
    pthread_mutexattr_destroy(&ma);
    w->master = master;
    w->sr = 44100;
    w->bs = 128;
    for (int s = 0; s < NSLOTS; s++) { w->slot[s].on = 1; w->slot[s].mix = 1; }
    if (g_nlist) snprintf(w->status, sizeof w->status, "%d effects available", g_nlist);
    else snprintf(w->status, sizeof w->status, "No effects found in MPC.settings");
    AEffect *e = &w->fx;
    e->magic = VST_MAGIC;
    e->dispatcher = dispatcher;
    e->process = process;
    e->setParameter = setParameter;
    e->getParameter = getParameter;
    e->processReplacing = processReplacing;
    e->numParams = NPARAMS;
    e->numPrograms = NPRESETS + 1;
    e->numInputs = 2;
    e->numOutputs = 2;
    e->flags = effFlagsCanReplacing | effFlagsProgramChunks;
    e->uniqueID = PLUG_UID;
    e->version = PLUG_VERSION;
    e->object = w;
    return e;
}
