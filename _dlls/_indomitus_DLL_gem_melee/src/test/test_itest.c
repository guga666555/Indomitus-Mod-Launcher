/* gem_melee_itest.exe (spec 8.4), linked from the dll objects (core, core_patch, core_crash, core_dllmain, link, chase, stance, pen):
 *   --install <exe>   map the exe at its preferred base, core_attach_all(dry): the real finders and writes of all three modules into the
 *                     mapping (nothing of the exe runs); machine lines BASE| / PATCH| / ADDR| for tools\parity.py; I-R1, I-R2, I-R3, I-R6
 *   --epoch           I-E: stance's and pen's core term of the new-battle rule (no install, module statics, epoch not frozen)
 *   --replay <log>    link's real R4 counter over the `orders:` lines of a MeleeStance log
 *   --link            I-R4, I-R5, I-R8, I-R8b with fake units
 *   --attach-time <exe> walk|batch [<ini>]   one timed dry run, `batch` with the attach batch (STARTUP_batch A/B, parity P-3c)
 * GM = two folders up from this exe (out\test\); the ini is the absolute <GM>\src\gem_melee.ini. Temporary files: <this exe's folder>\tmp\.
 * The log lines a case checks are written through a FILE sink into tmp\ and echoed to stdout as they are read back (CRLF -> LF; stdout
 * is in text mode), so the output reads like a STDOUT sink. */
#include "../core/core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <share.h>

/* link.c test entries (not in core.h, which is fixed by the spec): the cost line from given counters (I-R8), note calls per kind */
int link_test_cost(int64_t hook_qpc, long timed, int64_t orig_qpc, int64_t pen_qpc, long pen_calls, int64_t qpf, int64_t bias_q16, uint32_t now,
                   char *out, int cap);
void link_test_sources(long *front, long *pick, long *verdict);
/* module test-only entries (port rules S20 / C22, review F4 / F1) and the hook functions the dll's patches must lead to (all of them
 * non-static in their TUs: stance ms_hook / ms_exinst, pen pf_stub_* / pf_solve; chase's are static, read through C22) */
uint32_t stance_test_frame(volatile uint32_t *clk, void *orig, int sim);
void chase_test_hook_fns(void **eval, void **st, void **ap, void **dtor, void **update, void **vs);
__attribute__((ms_abi)) void *ms_hook(uint8_t *ctx);
__attribute__((ms_abi)) void ms_exinst(uint8_t *action, void *list);
extern void pf_stub_a1(void);
extern void pf_stub_a2(void);
extern void pf_stub_w(void);
extern void pf_stub_p(void);
extern void pf_stub_x(void);
int pf_solve(uint8_t *firer, uint8_t *aim, void *a3, void *a4, uint8_t *outrec, void *a6);

static char g_gm[MAX_PATH], g_tmp[MAX_PATH + 8], g_ini[MAX_PATH + 32];
static int  g_fail;

static void fail(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    g_fail++;
    printf("  FAIL: "); vprintf(fmt, ap); printf("\n"); fflush(stdout);
    va_end(ap);
}
#define EXPECT(c, ...) do { if (!(c)) fail(__VA_ARGS__); } while (0)

static const char *base_name(const char *p)
{
    const char *n = p + strlen(p);
    while (n > p && n[-1] != '\\' && n[-1] != '/') n--;
    return n;
}

static int setup_paths(void)
{
    char me[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, me, MAX_PATH);
    if (!n || n >= MAX_PATH) { printf("cannot read my own path\n"); return 0; }
    char *bs = strrchr(me, '\\'); if (bs) *bs = 0;                       /* <GM>\out\test */
    snprintf(g_tmp, sizeof g_tmp, "%s\\tmp", me);
    CreateDirectoryA(g_tmp, NULL);
    snprintf(g_gm, sizeof g_gm, "%s", me);
    for (int up = 0; up < 2; up++) { bs = strrchr(g_gm, '\\'); if (bs) *bs = 0; }
    snprintf(g_ini, sizeof g_ini, "%s\\src\\gem_melee.ini", g_gm);
    return 1;
}

/* ------------------------------------------------------------------------------------------------ log capture */
static char g_cap[MAX_PATH + 64];
static long g_cap_off;
static void cap_open(const char *name)
{
    snprintf(g_cap, sizeof g_cap, "%s\\%s", g_tmp, name);
    core_test_sink(CORE_SINK_DISCARD);                                   /* closes the sink's file before it is deleted */
    DeleteFileA(g_cap);
    g_cap_off = 0;
    core_test_sink_file(g_cap);
}
/* the log text written since the last call (LF line ends); echo = also print it; the caller frees it */
static char *cap_read(int echo)
{
    char *b = NULL; size_t got = 0;
    FILE *f = _fsopen(g_cap, "rb", _SH_DENYNO);
    if (f) {
        fseek(f, 0, SEEK_END); long n = ftell(f);
        long len = n > g_cap_off ? n - g_cap_off : 0;
        b = (char *)malloc((size_t)len + 1);
        fseek(f, g_cap_off, SEEK_SET);
        got = b ? fread(b, 1, (size_t)len, f) : 0;
        fclose(f);
        g_cap_off += (long)got;
    }
    if (!b) { b = (char *)malloc(1); if (!b) exit(3); }
    b[got] = 0;
    char *w = b; for (char *r = b; *r; r++) if (*r != '\r') *w++ = *r; *w = 0;
    if (echo) { fputs(b, stdout); fflush(stdout); }
    return b;
}
static char *cap_take(void) { return cap_read(1); }

/* the first line of text holding needle (copied to out without its line end); NULL = none */
static const char *find_line(const char *text, const char *needle, char *out, size_t cap)
{
    const char *p = strstr(text, needle);
    if (!p) { if (out && cap) out[0] = 0; return NULL; }
    const char *a = p; while (a > text && a[-1] != '\n') a--;
    const char *e = strchr(p, '\n'); if (!e) e = p + strlen(p);
    if (out && cap) { size_t k = (size_t)(e - a) < cap - 1 ? (size_t)(e - a) : cap - 1; memcpy(out, a, k); out[k] = 0; }
    return a;
}
static int count_sub(const char *text, const char *needle)
{
    int n = 0; size_t k = strlen(needle);
    for (const char *p = strstr(text, needle); p; p = strstr(p + k, needle)) n++;
    return n;
}

/* ------------------------------------------------------------------------------------------------ exe mapping (chase_probe.c map_image) */
static uint8_t *map_image(const char *path, int *relocated, uint32_t *imgsz_out)
{
    FILE *f = fopen(path, "rb"); if (!f) { printf("cannot open %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *file = (uint8_t *)malloc((size_t)n);
    if (!file || fread(file, 1, (size_t)n, f) != (size_t)n) { fclose(f); printf("cannot read %s\n", path); return NULL; }
    fclose(f);
    uint32_t pe = *(uint32_t *)(file + 0x3c);
    uint16_t nsec = *(uint16_t *)(file + pe + 6), opt = *(uint16_t *)(file + pe + 20);
    uint64_t base = *(uint64_t *)(file + pe + 24 + 24);
    uint32_t imgsz = *(uint32_t *)(file + pe + 24 + 56), hdrsz = *(uint32_t *)(file + pe + 24 + 60);
    uint8_t *g = (uint8_t *)VirtualAlloc((void *)(uintptr_t)base, imgsz, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    *relocated = 0;
    if (g != (uint8_t *)(uintptr_t)base) {
        if (g) VirtualFree(g, 0, MEM_RELEASE);
        g = (uint8_t *)VirtualAlloc(NULL, imgsz, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!g) { printf("VirtualAlloc failed\n"); return NULL; }
        *relocated = 1;
    }
    memcpy(g, file, hdrsz);
    for (int i = 0; i < nsec; i++) {
        uint8_t *sh = file + pe + 24 + opt + 40 * i;
        uint32_t vs = *(uint32_t *)(sh + 8), va = *(uint32_t *)(sh + 12), rs = *(uint32_t *)(sh + 16), raw = *(uint32_t *)(sh + 20);
        memcpy(g + va, file + raw, rs < vs ? rs : vs);
    }
    if (*relocated) {                                                    /* .reloc DIR64 only */
        uint32_t rrva = *(uint32_t *)(file + pe + 24 + 112 + 8 * 5), rsz = *(uint32_t *)(file + pe + 24 + 112 + 8 * 5 + 4);
        int64_t delta = (int64_t)((uint64_t)(uintptr_t)g - base);
        for (uint8_t *b = g + rrva; b < g + rrva + rsz; ) {
            uint32_t page = *(uint32_t *)b, sz = *(uint32_t *)(b + 4);
            if (!sz) break;
            for (uint16_t *e = (uint16_t *)(b + 8); (uint8_t *)e < b + sz; e++)
                if ((*e >> 12) == 10) *(uint64_t *)(g + page + (*e & 0xFFF)) += delta;
            b += sz;
        }
    }
    free(file);
    if (imgsz_out) *imgsz_out = imgsz;
    return g;
}

/* ------------------------------------------------------------------------------------------------ fake engine objects */
/* MSVC std::string at s (ms:104-111): SSO (cap 15) or heap (pointer, cap >= 16) */
static void ms_str(uint8_t *s, const char *txt, int heap)
{
    size_t n = strlen(txt);
    if (heap || n > 15) {
        char *p = (char *)malloc(n + 1); if (!p) exit(3);
        memcpy(p, txt, n + 1);
        *(char **)s = p; *(uint64_t *)(s + 0x10) = n; *(uint64_t *)(s + 0x18) = n < 16 ? 31 : n;
    } else { memcpy(s, txt, n + 1); *(uint64_t *)(s + 0x10) = n; *(uint64_t *)(s + 0x18) = 15; }
}
/* tag vector {begin, end} at v with the NULL-terminated names (each Tag 0x60 bytes, its name std::string at +0x40) */
static uint8_t *mk_vec(uint8_t *v, const char *const *names, int heap)
{
    int n = 0; while (names && names[n]) n++;
    uint8_t *arr = (uint8_t *)calloc((size_t)(n ? n : 1), 0x60); if (!arr) exit(3);
    for (int i = 0; i < n; i++) ms_str(arr + 0x60 * i + 0x40, names[i], heap);
    *(uint8_t **)v = arr; *(uint8_t **)(v + 8) = arr + 0x60 * n;
    return arr;
}
static uint8_t *mk_unit(uint32_t id, int human, uint8_t owner, const char *tag)
{
    uint8_t *u = (uint8_t *)calloc(1, 0x1400); if (!u) exit(3);
    u[0x30] = human ? 6 : 2; *(uint32_t *)(u + 0xC0) = id; u[0xDA8] = owner;
    const char *with[] = { "faction_test", tag, NULL }, *without[] = { "faction_test", NULL };
    mk_vec(u + stance_enttags_off(), tag ? with : without, 0);
    return u;
}
static uint8_t *mk_ammo(const char *const *names, int heap)
{
    uint8_t *a = (uint8_t *)calloc(1, 0x1400); if (!a) exit(3);
    mk_vec(a + stance_name_vec_off(), names, heap);
    return a;
}
static uint8_t *mk_ti(const void *target)
{
    uint8_t *t = (uint8_t *)calloc(1, 0x40); if (!t) exit(3);
    *(const void **)(t + 0x18) = target;
    return t;
}
static void set_ctx(uint8_t *ctx, const void *ammo, const void *unit, const void *ti)
{
    memset(ctx, 0, 0x20);
    *(const void **)ctx = ammo; *(const void **)(ctx + 8) = unit; *(const void **)(ctx + 0x10) = ti;
}

/* ================================================================================================ --install */
static const char *k_mname[CORE_MOD_N] = { "core", "chase", "stance", "pen", "link" };
static const char *state_name(int s)
{
    static const char *const k[] = { "?", "WRITTEN", "REFUSED_OVERLAP", "REFUSED_BYTES", "REFUSED_FULL", "FAILED" };
    return s >= 1 && s <= 5 ? k[s] : "?";
}
static void hexs(const uint8_t *b, uint32_t n, char *out)
{
    static const char d[] = "0123456789ABCDEF";
    for (uint32_t i = 0; i < n; i++) { out[2 * i] = d[b[i] >> 4]; out[2 * i + 1] = d[b[i] & 15]; }
    out[2 * n] = 0;
}
static uint8_t *g_base; static uint32_t g_imgsz;
static void rva_of(const void *p, char *out, size_t cap)
{
    const uint8_t *a = (const uint8_t *)p;
    if (!a) snprintf(out, cap, "NULL");
    else if (a >= g_base && a < g_base + g_imgsz) snprintf(out, cap, "%llX", (unsigned long long)(a - g_base));
    else snprintf(out, cap, "abs:%llX", (unsigned long long)(uintptr_t)a);
}
static int reg_find(int mod, const char *name, core_patch_t *out)
{
    for (int i = 0; i < core_patch_count(); i++) {
        core_patch_t e;
        if (core_patch_get(i, &e) && e.mod == mod && e.name && !strcmp(e.name, name) && e.state == CORE_PATCH_WRITTEN) { if (out) *out = e; return 1; }
    }
    return 0;
}

static volatile long g_stop_calls;
static void *g_stop_w; static uint64_t g_stop_a;
static uint64_t fake_stop(void *w, uint64_t a) { g_stop_calls++; g_stop_w = w; g_stop_a = a; return 0x5A; }

/* a fake weapon for chase's layout: weapon +wi -> item, item +tb / +te = tag vector begin / end */
static uint8_t *mk_weapon(uint32_t wi, uint32_t tb, uint32_t te, const uint8_t *beg, const uint8_t *end, int no_item)
{
    uint8_t *w = (uint8_t *)calloc(1, wi + 16); if (!w) exit(3);
    if (no_item) return w;
    uint8_t *it = (uint8_t *)calloc(1, (tb > te ? tb : te) + 16); if (!it) exit(3);
    *(const uint8_t **)(it + tb) = beg; *(const uint8_t **)(it + te) = end;
    *(uint8_t **)(w + wi) = it;
    return w;
}
static uint8_t *mk_tagarr(const char *const *names, int heap, uint8_t **end)
{
    uint8_t v[16];
    uint8_t *a = mk_vec(v, names, heap);
    *end = *(uint8_t **)(v + 8);
    return a;
}

static void ir1(const char *log)
{
    char out[4096], l[4096];
    int w0 = link_word_check(g_ini, out, sizeof out);
    printf("I-R1 shipped ini: %d warning(s)\n", w0);
    EXPECT(w0 == 0 && !strstr(out, "WARNING"), "I-R1: the shipped ini gives %d warning(s): %s", w0, out);
    EXPECT(find_line(log, "[link] R1 owner words: pen exempts {melee_light,melee_heavy,melee}", l, sizeof l) && !strstr(l, "WARNING"),
           "I-R1: the attach line is missing or warns: %s", l);
    /* temp ini = the shipped ini without melee_heavy in [PenFilter] ExemptTags */
    FILE *f = fopen(g_ini, "rb"); if (!f) { fail("I-R1: cannot read %s", g_ini); return; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *t = (char *)malloc((size_t)n + 64); if (!t) exit(3);
    size_t got = fread(t, 1, (size_t)n, f); fclose(f); t[got] = 0;
    const char *key = "\nExemptTags=melee_light,melee_heavy,melee\r\n";
    char *p = strstr(t, key);
    char tmp[MAX_PATH + 64]; snprintf(tmp, sizeof tmp, "%s\\r1_no_melee_heavy.ini", g_tmp);
    if (!p || strstr(p + 1, "\nExemptTags=")) { fail("I-R1: the shipped ini has no single ExemptTags=melee_light,melee_heavy,melee line"); free(t); return; }
    f = fopen(tmp, "wb"); if (!f) { fail("I-R1: cannot write %s", tmp); free(t); return; }
    fwrite(t, 1, (size_t)(p - t), f);
    fputs("\nExemptTags=melee_light,melee\r\n", f);
    fputs(p + strlen(key), f);
    fclose(f); free(t);
    int w1 = link_word_check(tmp, out, sizeof out);
    printf("I-R1 ini without melee_heavy in ExemptTags: %d warning(s): %s\n", w1, strstr(out, " | WARNING") ? strstr(out, " | WARNING") + 3 : "-");
    EXPECT(w1 == 1 && strstr(out, "\"melee_heavy\""), "I-R1: want exactly 1 warning naming melee_heavy, got %d: %s", w1, out);
}

static void ir2(void)
{
    core_patch_t h1, a1, a2, w;
    int ok = reg_find(CORE_MOD_STANCE, "H1 MatchAmmo prologue", &h1) && reg_find(CORE_MOD_PEN, "A1 volume tag test call", &a1)
          && reg_find(CORE_MOD_PEN, "A2 no-volume row test", &a2) && reg_find(CORE_MOD_PEN, "W ctx builder prologue", &w);
    if (!ok) { fail("I-R2: an H1 / A1 / A2 / W entry is missing from the patch table"); return; }
    uint8_t *ma = (uint8_t *)pen_matchammo_fn();
    int in1 = a1.addr >= h1.addr && a1.addr < h1.addr + 0x600, in2 = a2.addr >= h1.addr && a2.addr < h1.addr + 0x600;
    int wout = w.addr < h1.addr || w.addr >= h1.addr + 0x600;
    char s1[32], s2[32], s3[32];
    rva_of(h1.addr, s1, sizeof s1); rva_of(ma, s2, sizeof s2); rva_of(w.addr, s3, sizeof s3);
    int good = h1.addr == ma && in1 && in2 && wout;
    printf("I-R2: stance H1 exe+0x%s = pen MatchAmmo exe+0x%s: %s; pen A1 H1+0x%llX, A2 H1+0x%llX inside [H1, H1+0x600): %s; pen W exe+0x%s outside: %s -> %s\n",
           s1, s2, h1.addr == ma ? "yes" : "NO", (unsigned long long)(a1.addr - h1.addr), (unsigned long long)(a2.addr - h1.addr),
           in1 && in2 ? "yes" : "NO", s3, wout ? "yes" : "NO", good ? "OK" : "FAIL");
    EXPECT(good, "I-R2: the pick order structure does not hold");
}

static void ir3(void)
{
    uint32_t wi = 0, tb = 0, te = 0;
    if (!chase_layout(&wi, &tb, &te)) { fail("I-R3: chase found no weapon layout"); return; }
    printf("I-R3: chase layout weapon +0x%X -> item, tags +0x%X..+0x%X\n", wi, tb, te);
    free(cap_take());
    long c0[CORE_MOD_N], v0[CORE_MOD_N], k0, c1[CORE_MOD_N], v1[CORE_MOD_N], k1;
    core_ceasefire_stats(c0, v0, &k0);
    uint8_t *e1, *e2, *e3, *e4;
    const char *n_melee[] = { "pierce", "melee", NULL }, *n_veh[] = { "veh_melee", NULL }, *n_case[] = { "MeLeE", NULL };
    const char *n_gun[] = { "rifle", "pierce", "meleex", "mele", NULL };
    uint8_t *t1 = mk_tagarr(n_melee, 0, &e1), *t2 = mk_tagarr(n_veh, 1, &e2), *t3 = mk_tagarr(n_case, 0, &e3), *t4 = mk_tagarr(n_gun, 0, &e4);
    uint8_t *w_melee = mk_weapon(wi, tb, te, t1, e1, 0), *w_veh = mk_weapon(wi, tb, te, t2, e2, 0), *w_case = mk_weapon(wi, tb, te, t3, e3, 0);
    uint8_t *w_gun = mk_weapon(wi, tb, te, t4, e4, 0), *w_noitem = mk_weapon(wi, tb, te, NULL, NULL, 1);
    uint8_t *w_back = mk_weapon(wi, tb, te, t1 + 0x60, t1, 0);                         /* end < begin */
    uint8_t *big = (uint8_t *)calloc(257, 0x60); if (!big) exit(3);                     /* 257 Tags, the first one "melee": */
    ms_str(big + 0x40, "melee", 0);                                                    /* a missing size guard would find it */
    uint8_t *w_long = mk_weapon(wi, tb, te, big, big + 0x60 * 257, 0);
    uint64_t r = 0; long s;
    struct { const char *what; int mod; uint8_t *w; int want_ret; } k[] = {
        { "melee weapon (\"melee\", SSO), pen stop",            CORE_MOD_PEN,   w_melee,  0 },
        { "melee weapon (\"veh_melee\", heap string), pen stop", CORE_MOD_PEN,   w_veh,    0 },
        { "melee weapon (\"MeLeE\": case-insensitive), pen stop", CORE_MOD_PEN,  w_case,   0 },
        { "melee weapon, chase hold",                           CORE_MOD_CHASE, w_melee,  1 },
        { "ranged weapon (rifle, meleex, mele), pen stop",      CORE_MOD_PEN,   w_gun,    1 },
        { "same ranged weapon, chase hold in the same sim ms",  CORE_MOD_CHASE, w_gun,    1 },
        { "weapon with item NULL, pen stop",                    CORE_MOD_PEN,   w_noitem, 1 },
        { "tag vector end < begin, pen stop",                   CORE_MOD_PEN,   w_back,   1 },
        { "tag vector longer than 0x60 * 256, pen stop",        CORE_MOD_PEN,   w_long,   1 },
    };
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++) {
        s = g_stop_calls; g_stop_w = NULL; g_stop_a = 0; r = 0;
        int ret = core_ceasefire(k[i].mod, k[i].w, (void *)fake_stop, &r);
        int called = g_stop_calls - s;
        int ok = ret == k[i].want_ret && called == k[i].want_ret && (!called || (g_stop_w == k[i].w && g_stop_a == 1 && r == 0x5A));
        printf("I-R3 %-52s -> %s %s\n", k[i].what, ret ? "stopped (engine fn called with (w, 1))" : "refused (engine fn not called)", ok ? "OK" : "FAIL");
        EXPECT(ok, "I-R3 %s: return %d, engine calls %d", k[i].what, ret, called);
    }
    core_ceasefire_stats(c1, v1, &k1);
    long dv = v1[CORE_MOD_PEN] - v0[CORE_MOD_PEN], dvc = v1[CORE_MOD_CHASE] - v0[CORE_MOD_CHASE];
    long dcp = c1[CORE_MOD_PEN] - c0[CORE_MOD_PEN], dcc = c1[CORE_MOD_CHASE] - c0[CORE_MOD_CHASE], dk = k1 - k0;
    printf("I-R3 counters: pen vetoed %ld, chase vetoed %ld, pen stops %ld, chase stops %ld, collisions %ld\n", dv, dvc, dcp, dcc, dk);
    EXPECT(dv == 3 && dvc == 0 && dcp == 4 && dcc == 2 && dk == 1, "I-R3 counters: want pen vetoed 3, chase vetoed 0, pen stops 4, chase stops 2, collisions 1");
    char *log = cap_take();
    EXPECT(count_sub(log, "[link] R3: pen stop of melee weapon ") == 3, "I-R3: want 3 '[link] R3: pen stop of melee weapon' lines");
    EXPECT(count_sub(log, "[link] R3: weapon ") == 1 && strstr(log, "stopped by chase and pen in the same sim ms"), "I-R3: want 1 collision line");
    free(log);
}

static void ir6(const char *log)
{
    static const char want[] = "R6 player policy: direct control: stance skips (SkipDirectControl=1; IsManualControlled with the 0.7.32 component guard, "
        "or the local manual-control manager), pen skips (SkipDirectControl=1; property or DD8 bit, no manager), chase does not check | human player's "
        "units: stance AutoDraw PlayerUnits=0, Charge PlayerUnits=1, PlayerMaxDistance=1000; pen PlayerUnits=1; chase acts on them";
    char l[2048];
    const char *p = find_line(log, "[link] R6 player policy:", l, sizeof l) ? strstr(l, "R6 player policy:") : NULL;
    int ok = p && !strcmp(p, want);
    printf("I-R6: R6 line with the shipped values: %s\n", ok ? "OK" : "FAIL");
    EXPECT(ok, "I-R6: line is\n    %s\n  want\n    %s", p ? p : "(missing)", want);
}

/* review F1 (spec K1): where every written entry leads. The machine lines only say what was written; this follows the bytes to the
 * function they must reach. how 1 = `FF 25 00000000` + abs64 at the entry itself (stance H1, pen W), 2 = E8 / E9 rel32 that lands on
 * a `FF 25 00000000` pad whose abs64 is the function (chase redirects, pen A1 / A2 / B / P / X), 3 = the slot value (PTR); H4 is data. */
/* gem_melee 0.1.14: the two hull-melee call redirects of the chase module are written together where the exe's weapon selection code
 * is recognised (the editor build so far): 0 or 2 more written CODE entries */
static int hull_patches(void)
{
    int n = 0;
    for (int i = 0; i < core_patch_count(); i++) {
        core_patch_t e; if (!core_patch_get(i, &e) || e.state != CORE_PATCH_WRITTEN || e.mod != CORE_MOD_CHASE || !e.name) continue;
        if (!strcmp(e.name, "melee weapon selection reach test") || !strcmp(e.name, "melee range in the target evaluation")
            || !strcmp(e.name, "melee range against a target")                      /* 0.1.16: the 6 call sites of range(weapon, target), every exe */
            || !strcmp(e.name, "melee shot range slot")                             /* 0.1.18: 3 shooter vtable slots, every exe */
            || !strcmp(e.name, "melee aim status slot") || !strcmp(e.name, "melee fire line slot")) n++;   /* 0.1.17: 5 + 5 aimer vtable slots, every exe */
    }
    return n;
}
static int hull_ptr_patches(void)                            /* gem_melee 0.1.17: the pointer writes among them (the aimer vtable slots) */
{
    int n = 0;
    for (int i = 0; i < core_patch_count(); i++) {
        core_patch_t e; if (!core_patch_get(i, &e) || e.state != CORE_PATCH_WRITTEN || e.mod != CORE_MOD_CHASE || !e.name) continue;
        if (!strcmp(e.name, "melee aim status slot") || !strcmp(e.name, "melee fire line slot") || !strcmp(e.name, "melee shot range slot")) n++;
    }
    return n;
}
static const void *abs_at(const uint8_t *p) { return (p[0] == 0xFF && p[1] == 0x25 && !*(const uint32_t *)(p + 2)) ? *(const void *const *)(p + 6) : NULL; }
static int target_check(void)
{
    void *ev = NULL, *st = NULL, *ap = NULL, *dt = NULL, *up = NULL, *vs = NULL;
    chase_test_hook_fns(&ev, &st, &ap, &dt, &up, &vs);
    void *sl = NULL, *er = NULL; chase_test_hull_fns(&sl, &er);
    void *rw = NULL; chase_test_rwt_fn(&rw);
    void *as = NULL, *bl = NULL; chase_test_ast_fns(&as, &bl);
    void *sp = NULL; chase_test_spt_fn(&sp);
    const struct { int mod; const char *name; int how; const void *fn; } k[] = {
        { CORE_MOD_STANCE, "H1 MatchAmmo prologue",   1, (const void *)ms_hook },
        { CORE_MOD_STANCE, "H3 ExecuteInstant slot",  3, (const void *)ms_exinst },
        { CORE_MOD_CHASE,  "target evaluation",       2, ev },
        { CORE_MOD_CHASE,  "weapon target",           2, st },
        { CORE_MOD_CHASE,  "approach move order",     2, ap },
        { CORE_MOD_CHASE,  "melee weapon selection reach test",    2, sl },
        { CORE_MOD_CHASE,  "melee range in the target evaluation", 2, er },
        { CORE_MOD_CHASE,  "melee range against a target",         2, rw },
        { CORE_MOD_CHASE,  "melee aim status slot",                3, as },
        { CORE_MOD_CHASE,  "melee fire line slot",                 3, bl },
        { CORE_MOD_CHASE,  "melee shot range slot",                3, sp },
        { CORE_MOD_CHASE,  "vt eMoveTask[0] dtor",    3, dt },
        { CORE_MOD_CHASE,  "vt eMoveTask[19] Update", 3, up },
        { CORE_MOD_CHASE,  "vt eVS_Root[3] tick",     3, vs },
        { CORE_MOD_PEN,    "W ctx builder prologue",  1, (const void *)pf_stub_w },
        { CORE_MOD_PEN,    "A1 volume tag test call", 2, (const void *)pf_stub_a1 },
        { CORE_MOD_PEN,    "A2 no-volume row test",   2, (const void *)pf_stub_a2 },
        { CORE_MOD_PEN,    "B solver call",           2, (const void *)pf_solve },
        { CORE_MOD_PEN,    "P try-loop head",         2, (const void *)pf_stub_p },
        { CORE_MOD_PEN,    "X exhausted clear call",  2, (const void *)pf_stub_x },
    };
    int good = 0, bad = 0, data = 0, unknown = 0;
    for (int i = 0; i < core_patch_count(); i++) {
        core_patch_t e; if (!core_patch_get(i, &e) || e.state != CORE_PATCH_WRITTEN) continue;
        if (e.kind == CORE_PATCH_DATA) { data++; continue; }
        int w = -1;
        for (size_t j = 0; j < sizeof k / sizeof k[0]; j++) if (k[j].mod == e.mod && e.name && !strcmp(k[j].name, e.name)) w = (int)j;
        if (w < 0) { unknown++; printf("  TARGET FAIL: %s %s: no expected target known\n", k_mname[e.mod], e.name ? e.name : "?"); continue; }
        const void *got = NULL; const char *why = NULL;
        if (k[w].how == 1) { if (e.kind != CORE_PATCH_CODE || !(got = abs_at(e.now))) why = "not an FF 25 absolute jump"; }
        else if (k[w].how == 2) {
            if (e.kind != CORE_PATCH_CODE || (e.now[0] != 0xE8 && e.now[0] != 0xE9)) why = "not a call / jmp rel32";
            else {
                const uint8_t *t = e.addr + 5 + *(const int32_t *)(e.now + 1);
                MEMORY_BASIC_INFORMATION mb;
                if (!VirtualQuery(t, &mb, sizeof mb) || mb.State != MEM_COMMIT || (uintptr_t)t + 14 > (uintptr_t)mb.BaseAddress + mb.RegionSize) why = "rel32 leads to unmapped memory";
                else if (!(got = abs_at(t))) why = "rel32 does not land on an FF 25 00000000 pad";
            }
        } else { if (e.kind != CORE_PATCH_PTR) why = "not a pointer write"; else got = *(const void *const *)e.now; }
        if (!why && got != k[w].fn) why = "leads to another function";
        if (why) { bad++; printf("  TARGET FAIL: %s %s: %s (%p, want %p)\n", k_mname[e.mod], e.name, why, got, k[w].fn); }
        else good++;
    }
    printf("targets: %d entries lead to their gem_melee function, %d data, %d wrong, %d unknown\n", good, data, bad, unknown);
    EXPECT(good == 25 + hull_patches() && data == 1 && !bad && !unknown, "target check: want 25 (+2 hull melee) entries on their function + 1 data entry (H4)");
    return !bad && !unknown;
}

static int run_install(const char *exe)
{
    int rel = 0;
    uint8_t *base = map_image(exe, &rel, &g_imgsz);
    if (!base) { printf("INSTALL FAILED\n"); return 1; }
    g_base = base;
    const char *nm = base_name(exe);
    core_test_set_exe(base, nm);
    core_test_set_ini(g_ini);
    char cn[MAX_PATH]; snprintf(cn, sizeof cn, "install_%s.log", nm);
    cap_open(cn);
    core_attach_all(GetModuleHandleA(NULL), 1);
    char *log = cap_take();
    char l[4096];
    /* banner: the shipped ini was found (a run on code defaults would pass H3 / H4 / H5 silently) */
    if (!find_line(log, "[core] " GEM_MELEE_NAME " " GEM_MELEE_VERSION " (", l, sizeof l)) fail("no banner line");
    else EXPECT(strlen(l) > 6 && !strcmp(l + strlen(l) - 6, " found") && strstr(l, g_ini), "the banner does not name %s as found: %s", g_ini, l);
    EXPECT(!strstr(log, "NOT FOUND"), "a NOT FOUND line");
    EXPECT(!strstr(log, "[core] WARNING"), "a [core] WARNING line (module clock differs from the core's)");
    EXPECT(strstr(log, "[core] modules: chase ON, stance ON, pen ON |"), "not every module is ON");
    EXPECT(find_line(log, "[link] clock check: core exe+0x", l, sizeof l) && strlen(l) > 7 && !strcmp(l + strlen(l) - 7, ": equal"), "link clock check: %s", l);
    EXPECT(find_line(log, "[link] cease-fire: chase clear target exe+0x", l, sizeof l) && strstr(l, "one engine function"), "link cease-fire line: %s", l);
    EXPECT(find_line(log, "[link] R2 one pick: stance H1", l, sizeof l) && !strstr(l, "WARNING"), "link R2 line: %s", l);
    /* review N2: the R4 attach line says the order poll is on because stance can run it (S21) */
    EXPECT(stance_order_poll_state() == 1 && find_line(log, "[link] R4 flip detector: ", l, sizeof l) && strstr(l, "inputs: stance order poll on, "),
           "R4 attach line (stance_order_poll_state %d): %s", stance_order_poll_state(), l);
    /* the dry run has no loader lock, so no attach batch: one thread walk per code write (STARTUP_batch) */
    char cw[160]; snprintf(cw, sizeof cw, "[core] code writes: %d with the other threads suspended, %d thread walks (one per write, no attach batch, ", 13 + hull_patches() - hull_ptr_patches(), 13 + hull_patches() - hull_ptr_patches());
    if (!find_line(log, cw, l, sizeof l)) {
        find_line(log, "[core] code writes:", l, sizeof l);
        fail("the code writes line: want 13 code writes with 13 thread walks (no attach batch in the dry run): %s", l[0] ? l : "(missing)");
    }
    /* machine lines */
    printf("BASE|%llX|%s\n", (unsigned long long)(uintptr_t)base, rel ? "relocated" : "preferred");
    int nw[CORE_MOD_N] = { 0 }, nk[CORE_MOD_N][4], other = 0;
    memset(nk, 0, sizeof nk);
    for (int i = 0; i < core_patch_count(); i++) {
        core_patch_t e; if (!core_patch_get(i, &e)) continue;
        uint32_t n = e.len <= CORE_PATCH_MAXLEN ? e.len : 0;
        char o[2 * CORE_PATCH_MAXLEN + 1], w[2 * CORE_PATCH_MAXLEN + 1], a[32];
        hexs(e.orig, n, o); hexs(e.now, n, w); rva_of(e.addr, a, sizeof a);
        printf("PATCH|%s|%s|%s|%u|%s|%s|%s\n", (unsigned)e.mod < CORE_MOD_N ? k_mname[e.mod] : "?", e.name ? e.name : "?", a, e.len, o, w, state_name(e.state));
        if (e.state == CORE_PATCH_WRITTEN && (unsigned)e.mod < CORE_MOD_N) { nw[e.mod]++; if (e.kind >= 1 && e.kind <= 3) nk[e.mod][e.kind]++; }
        else other++;
    }
    struct { const char *key; const void *p; } ad[] = {
        { "core_clock", (const void *)core_clock_ptr() }, { "stance_clock", stance_clock_ptr() }, { "chase_clock", chase_clock_ptr() },
        { "pen_clock", pen_clock_ptr() }, { "chase_ceasefire", chase_ceasefire_fn() }, { "pen_stop", pen_stop_fn() },
        { "pen_matchammo", pen_matchammo_fn() },
    };
    for (size_t i = 0; i < sizeof ad / sizeof ad[0]; i++) { char a[32]; rva_of(ad[i].p, a, sizeof a); printf("ADDR|%s|%s\n", ad[i].key, a); }
    int total = nw[CORE_MOD_CHASE] + nw[CORE_MOD_STANCE] + nw[CORE_MOD_PEN];
    printf("registry: %d written (chase %d = %d PTR + %d CODE, stance %d = %d CODE + %d PTR + %d DATA, pen %d = %d CODE), %d refused/failed\n",
           total, nw[CORE_MOD_CHASE], nk[CORE_MOD_CHASE][CORE_PATCH_PTR], nk[CORE_MOD_CHASE][CORE_PATCH_CODE], nw[CORE_MOD_STANCE],
           nk[CORE_MOD_STANCE][CORE_PATCH_CODE], nk[CORE_MOD_STANCE][CORE_PATCH_PTR], nk[CORE_MOD_STANCE][CORE_PATCH_DATA], nw[CORE_MOD_PEN],
           nk[CORE_MOD_PEN][CORE_PATCH_CODE], other);
    int hx = hull_patches();                                     /* gem_melee 0.1.14: 0, or 2 on an exe whose selection code is recognised */
    int hp = hull_ptr_patches();                                 /* gem_melee 0.1.17: 10 aimer vtable slots */
    EXPECT(total == 26 + hx && other == 0 && hp == 13 && (hx == 19 || hx == 21), "want 26 + 6 range call sites + 10 aimer slots + 3 shooter slots (+2 hull melee selection hooks on the editor exe) written, 0 refused / failed");
    EXPECT(nk[CORE_MOD_CHASE][CORE_PATCH_PTR] == 3 + hp && nk[CORE_MOD_CHASE][CORE_PATCH_CODE] == 6 + hx - hp && nw[CORE_MOD_CHASE] == 9 + hx, "chase: want 3 (+10 aimer slots) PTR + 6 (+ hull melee) CODE");
    EXPECT(nk[CORE_MOD_STANCE][CORE_PATCH_CODE] == 1 && nk[CORE_MOD_STANCE][CORE_PATCH_PTR] == 9 && nk[CORE_MOD_STANCE][CORE_PATCH_DATA] == 1,
           "stance: want H1 (CODE) + 9 x H3 (PTR) + H4 (DATA)");
    EXPECT(nk[CORE_MOD_PEN][CORE_PATCH_CODE] == 6 && nw[CORE_MOD_PEN] == 6, "pen: want 6 CODE");
    const void *cc = (const void *)core_clock_ptr();
    EXPECT(cc && cc == stance_clock_ptr() && cc == chase_clock_ptr() && cc == pen_clock_ptr(), "the four clocks are not one pointer");
    EXPECT(chase_ceasefire_fn() && chase_ceasefire_fn() == pen_stop_fn(), "chase cease-fire fn != pen stop fn");
    EXPECT(chase_status() == 15 && pen_status() == 15, "chase_status %d / pen_status %d (want 15 / 15)", chase_status(), pen_status());
    target_check();
    ir1(log);
    ir2();
    ir3();
    ir6(log);
    free(log);
    printf(g_fail ? "INSTALL FAILED\n" : "INSTALL OK\n");
    return g_fail ? 1 : 0;
}

/* ================================================================================================ --epoch (I-E) */
static uint32_t g_ls, g_lp;
/* one probe: flush count delta, the extra "flush for core new battle" line, and its order before the module's own flush line */
static void probe(int pen, uint32_t now, int want, int want_extra, const char *what)
{
    uint32_t n = pen ? pen_epoch_probe(now) : stance_epoch_probe(now);
    int d = (int)(n - (pen ? g_lp : g_ls));
    if (pen) g_lp = n; else g_ls = n;
    char *t = cap_take();
    const char *tag = pen ? "[pen] " : "[stance] ";
    char x[64], y[64];
    snprintf(x, sizeof x, "%sflush for core new battle", tag);
    snprintf(y, sizeof y, "%s%s", tag, pen ? "new battle [" : "--- clock ");
    int extra = count_sub(t, x), own = count_sub(t, y);
    const char *px = strstr(t, x), *py = strstr(t, y);
    int order = !extra || (px && py && px < py);
    int ok = d == want && extra == want_extra && own == want && order;
    printf("I-E %-62s %s probe(%u): flushes +%d (want +%d), core line %d (want %d)%s -> %s\n", what, pen ? "pen   " : "stance", now, d, want, extra,
           want_extra, extra && !order ? ", AFTER the flush line" : "", ok ? "OK" : "FAIL");
    EXPECT(ok, "I-E %s", what);
    free(t);
}
static volatile uint32_t g_fake, g_other;
static void quant(uint32_t v) { g_fake = v; core_on_quant(); }
static int g_frames;
static __attribute__((ms_abi)) void *fake_frame(void *a, void *b, void *c, void *d)
{
    (void)a; (void)b; (void)c; (void)d;
    g_frames++;
    core_log(CORE_MOD_CORE, "test: engine frame function (quant %d)", g_frames);
    return NULL;
}

static int run_epoch(void)
{
    cap_open("epoch.log");
    core_test_set_clock(&g_fake);
    core_epoch_bind(CORE_MOD_STANCE, (const volatile void *)&g_fake);
    core_epoch_bind(CORE_MOD_PEN, (const volatile void *)&g_fake);
    char *t = cap_take(); EXPECT(!strstr(t, "WARNING"), "binding to the core clock logged a WARNING"); free(t);
    /* the modules' flush counters start at 0 (g_ls / g_lp too); probe() works on deltas */
    /* (1) the core fires before either module's first call: no flush, no line (first call: nothing of an earlier battle is held) */
    quant(600000); quant(0);
    t = cap_take(); EXPECT(core_epoch() == 1 && strstr(t, "[core] --- new battle 1"), "(1) the core did not fire"); free(t);
    probe(0, 10, 0, 0, "(1) first call after a core fire");
    probe(1, 10, 0, 0, "(1) first call after a core fire");
    /* (2) monotonic */
    static const uint32_t mono[3] = { 2000, 4000, 9000 };
    for (int i = 0; i < 3; i++) {
        quant(mono[i]);
        probe(0, mono[i], 0, 0, "(2) monotonic");
        probe(1, mono[i], 0, 0, "(2) monotonic");
    }
    /* (3) the core fires on a 3 s back-step, under stance's own 5 s rule; pen's clock forward (its own rule silent) */
    quant(6000);
    t = cap_take(); EXPECT(core_epoch() == 2, "(3) the core did not fire on 9000 -> 6000"); free(t);
    probe(0, 6500, 1, 1, "(3) core-only back-step");
    probe(1, 9500, 1, 1, "(3) core-only back-step, pen's clock forward");
    probe(0, 6600, 0, 0, "(3) second call");
    probe(1, 9600, 0, 0, "(3) second call");
    /* (4) own rules with the core silent (the core clock stays 6000) */
    probe(1, 100, 1, 0, "(4) pen's own rule 9600 -> 100");
    probe(0, 100, 1, 0, "(4) stance's own rule 6600 -> 100");
    /* (5) pen bound to another clock: detached (WARNING), keeps only its own rule; stance still attached */
    core_epoch_bind(CORE_MOD_PEN, (const volatile void *)&g_other);
    t = cap_take(); EXPECT(strstr(t, "[core] WARNING: pen clock"), "(5) no WARNING line for the detached pen"); free(t);
    quant(9000); quant(1000);
    t = cap_take(); EXPECT(core_epoch() == 3, "(5) the core did not fire on 9000 -> 1000"); free(t);
    probe(1, 200, 0, 0, "(5) detached pen after a core fire");
    probe(0, 1100, 1, 1, "(5) attached stance after a core fire");
    /* (6) no core clock: back-steps never fire, the modules run on their own rules. The first call of each after the clock is removed
     * flushes once more ("flush for core new battle 0"): core_epoch_poll returns 0 without a clock, which differs from the counter the
     * module last acted on (WP1 / WP3 note; production sets the core clock once at attach and never removes it). */
    core_test_set_clock(NULL);
    probe(1, 300, 1, 1, "(6) transition: pen's first call without a core clock");
    probe(0, 1200, 1, 1, "(6) transition: stance's first call without a core clock");
    uint32_t e0 = core_epoch();
    g_fake = 50000; core_on_quant(); g_fake = 10; core_on_quant();
    t = cap_take(); EXPECT(core_epoch() == e0 && !strstr(t, "new battle"), "(6) the core fired without a clock"); free(t);
    printf("I-E (6) no core clock: back-step 50000 -> 10 seen by core_on_quant: epoch stays %u -> %s\n", core_epoch(), core_epoch() == e0 ? "OK" : "FAIL");
    probe(1, 400, 0, 0, "(6) pen forward");
    probe(1, 350, 1, 0, "(6) pen's own rule 400 -> 350");
    probe(0, 7000, 0, 0, "(6) stance forward");
    probe(0, 4500, 0, 0, "(6) stance 2.5 s back: under its 5 s rule, no core");
    probe(0, 1000, 0, 0, "(6) stance 3.5 s back: under its 5 s rule, no core");
    probe(0, 9000, 0, 0, "(6) stance forward");
    probe(0, 1000, 1, 0, "(6) stance's own rule 9000 -> 1000");
    /* (7) S3 (review F4): stance's quant hook frame_hook itself (stance_test_frame: a fake engine frame function, the tick off): the
     * core poll and stance's flush check run at quant start, before the engine's frame function; on a thread that is not stance's
     * engine thread the flush check does not run (ms_tick's thread test, review RP-2) and the next engine-thread call flushes */
    core_test_set_clock(&g_fake);
    core_epoch_bind(CORE_MOD_STANCE, (const volatile void *)&g_fake);
    g_fake = 20000;
    probe(0, 20000, 1, 1, "(7) transition: the core clock is back (the counter moved while it was gone)");
    g_frames = 0;
    g_fake = 21000; g_ls = stance_test_frame(&g_fake, (void *)fake_frame, 1);
    t = cap_take();
    EXPECT(g_frames == 1 && !strstr(t, "flush") && !strstr(t, "--- clock") && strstr(t, "[core] test: engine frame function"),
           "(7a) frame_hook forward: want no flush and one call of the frame function");
    printf("I-E (7a) frame_hook at 21000 (forward): frame function called %d time(s), no flush -> %s\n", g_frames, g_frames == 1 && !strstr(t, "flush") ? "OK" : "FAIL");
    free(t);
    uint32_t e7 = core_epoch(), n7 = g_ls;
    g_fake = 18000; g_ls = stance_test_frame(&g_fake, (void *)fake_frame, 1);
    t = cap_take();
    const char *pc = strstr(t, "[core] --- new battle"), *pf = strstr(t, "[stance] flush for core new battle"), *pk = strstr(t, "[stance] --- clock 21000 -> 18000");
    const char *pe = strstr(t, "[core] test: engine frame function");
    int ok7b = core_epoch() == e7 + 1 && g_ls == n7 + 1 && g_frames == 2 && pc && pf && pk && pe && pc < pf && pf < pk && pk < pe;
    printf("I-E (7b) frame_hook at 18000 (3 s back, under stance's 5 s rule): core fire, stance flush (core line first), then the engine's frame "
           "function -> %s\n", ok7b ? "OK" : "FAIL");
    EXPECT(ok7b, "(7b) want: [core] --- new battle, [stance] flush for core new battle, [stance] --- clock 21000 -> 18000, then the frame function");
    free(t);
    uint32_t n7c = g_ls;
    g_fake = 15000; g_ls = stance_test_frame(&g_fake, (void *)fake_frame, 0);
    t = cap_take();
    int ok7c = core_epoch() == e7 + 2 && g_ls == n7c && g_frames == 3 && !strstr(t, "[stance] flush") && !strstr(t, "[stance] --- clock");
    printf("I-E (7c) frame_hook at 15000 on another thread: core fire %s, stance flush check skipped -> %s\n", core_epoch() == e7 + 2 ? "yes" : "NO",
           ok7c ? "OK" : "FAIL");
    EXPECT(ok7c, "(7c) the quant-start flush ran on a thread that is not stance's engine thread");
    free(t);
    probe(0, 15100, 1, 1, "(7d) the next stance call on the engine thread flushes for it");
    printf(g_fail ? "EPOCH FAILED (%d)\n" : "EPOCH OK\n", g_fail);
    return g_fail ? 1 : 0;
}

/* ================================================================================================ --replay */
static int run_replay(const char *path)
{
    core_test_set_ini(g_ini);                                            /* the shipped [Link] values */
    core_test_sink(CORE_SINK_STDOUT);
    link_attach(0, 0, 0);
    static char units[8192];
    int n = link_test_replay(path, units, sizeof units);
    printf("REPLAY: %d flip blocks, units %s\n", n, units[0] ? units : "(none)");
    int ok = n > 0 && strstr(units, "00000001EB7D9490") && strstr(units, "000000023326A590");
    printf(ok ? "REPLAY OK\n" : "REPLAY FAILED (want units 00000001EB7D9490 and 000000023326A590)\n");
    return ok ? 0 : 1;
}

/* ================================================================================================ --link */
static const void *const k_vt[2] = { (const void *)(uintptr_t)0x140F6D078ull, (const void *)(uintptr_t)0x140F6BF08ull };
static const char *const k_onm[2] = { ".?AVeComplexOrder@@", ".?AVeAttackOrder@@" };
static int g_cls_i;
static void front(const uint8_t *u, uint32_t t)
{
    g_fake = t;
    CORE_NOTE_FRONT(u, k_vt[g_cls_i & 1], k_onm[g_cls_i & 1], t);
    g_cls_i++;
}
static int flips_since(int *f0) { int n = link_flip_count(), d = n - *f0; *f0 = n; return d; }

static void ir4(void)
{
    char *t, want[512];
    int f0 = link_flip_count(), d;
    /* (a) sampled: the order class alternates every 500 ms, stance's poll samples it at 250-300 ms spacing */
    uint8_t *A = mk_unit(0x101, 1, 1, NULL);
    static const uint32_t step[5] = { 250, 300, 275, 260, 290 };
    uint32_t start = 1000, tt = start, trip_at = 0; int last = -1, changes = 0;
    for (int i = 0; tt <= start + 4000; i++) {
        int cls = (int)((tt / 500) & 1);
        if (cls != last) { g_fake = tt; CORE_NOTE_FRONT(A, k_vt[cls], k_onm[cls], tt); last = cls; changes++; }
        if (link_flip_count() > f0) { trip_at = tt; break; }
        tt += step[i % 5];
    }
    t = cap_take(); d = flips_since(&f0);
    printf("I-R4 (a) sampled alternation: %d changes, trip at %u ms = %u ms after the first sample -> %s\n", changes, trip_at, trip_at - start,
           trip_at && trip_at - start <= 3000 && d == 1 ? "OK" : "FAIL");
    snprintf(want, sizeof want, "[link] flip: [%u ms] unit %p (human, owner 1, id 0x101) front order changed ", trip_at, (void *)A);
    EXPECT(trip_at && trip_at - start <= 3000 && d == 1 && strstr(t, want), "I-R4 (a): no block within 3 s (%s)", want);
    free(t);
    /* (b) 3 changes in 3 s: none, but a suspect from its 2nd change */
    uint8_t *B = mk_unit(0x102, 1, 1, NULL);
    front(B, 10000); front(B, 11000); front(B, 12000);
    t = cap_take(); d = flips_since(&f0);
    const core_suspect_t *sb = &core_suspect[core_suspect_slot(B)];
    int okb = d == 0 && sb->unit == B && sb->id == 0x102 && sb->until == 15000;
    printf("I-R4 (b) 3 changes in 2 s: %d blocks, suspect {unit, id 0x%X, until %u} -> %s\n", d, sb->id, sb->until, okb ? "OK" : "FAIL");
    EXPECT(okb, "I-R4 (b)"); free(t);
    /* (c) 5 changes in 2.9 s: one block; (d) a 6th within the 10 s mute: none */
    uint8_t *C = mk_unit(0x103, 0, 3, NULL);
    C[0x30] = 2;                                                         /* an actor, not human: "vehicle" */
    front(C, 20000); front(C, 20700); front(C, 21400); front(C, 22100); front(C, 22900);
    t = cap_take(); d = flips_since(&f0);
    snprintf(want, sizeof want, "[link] flip: [22900 ms] unit %p (vehicle, owner 3, id 0x103) front order changed 5 times in 2900 ms (FlipChanges=5, FlipWindowMs=3000):", (void *)C);
    printf("I-R4 (c) 5 changes in 2.9 s: %d block(s) -> %s\n", d, d == 1 && strstr(t, want) ? "OK" : "FAIL");
    EXPECT(d == 1 && strstr(t, want), "I-R4 (c): want '%s'", want);
    EXPECT(count_sub(t, "  [2") == 5 && strstr(t, "  [20000 ms] .?AVe") && strstr(t, "Order@@ (vt 0000000140F6") && strstr(t, "  stance: none") && strstr(t, "  pen: none")
           && strstr(t, "  chase: none"), "I-R4 (c): the block's change / verdict lines");
    free(t);
    front(C, 23500);
    t = cap_take(); d = flips_since(&f0);
    printf("I-R4 (d) a 6th change within the mute: %d block(s) -> %s\n", d, d == 0 ? "OK" : "FAIL");
    EXPECT(d == 0, "I-R4 (d)"); free(t);
    /* (e) verdict lines: stance MELEE pick, pen ALLOW, chase hold on a suspect, then the trip */
    uint8_t *D = mk_unit(0x104, 1, 2, "ai_trace"), *T = mk_unit(0x77, 0, 1, NULL);
    const char *an[] = { "melee_light", "melee_heavy", NULL };
    uint8_t *ammo = mk_ammo(an, 0), *ti = mk_ti(T), ctx[0x20];
    front(D, 30000); front(D, 30500);
    set_ctx(ctx, ammo, D, ti);
    g_fake = 30600; CORE_NOTE_PICK(CORE_MOD_STANCE, ctx, CORE_PICK_MELEE);
    CORE_NOTE_VERDICT(CORE_MOD_PEN, D, T, CORE_PEN_ALLOW, 30700);
    CORE_NOTE_VERDICT(CORE_MOD_CHASE, D, NULL, CORE_CHASE_HOLD, 30800);
    front(D, 31000); front(D, 31500); front(D, 32000);
    t = cap_take(); d = flips_since(&f0);
    char w1[256], w2[160], w3[96], w4[200];
    snprintf(w1, sizeof w1, "unit %p (human, owner 2, id 0x104, traced) front order changed 5 times in 2000 ms", (void *)D);
    snprintf(w2, sizeof w2, "  stance: last MELEE at 30600 ms: ammo \"melee_light melee_heavy\" vs %p", (void *)T);
    snprintf(w3, sizeof w3, "  pen: last ALLOW vs target 0x77 at 30700 ms");
    snprintf(w4, sizeof w4, "  chase: last hold at 30800 ms");
    int oke = d == 1 && strstr(t, w1) && strstr(t, w2) && strstr(t, w3) && strstr(t, w4);
    printf("I-R4 (e) verdict lines in the block (stance MELEE, pen ALLOW, chase hold): %s\n", oke ? "OK" : "FAIL");
    EXPECT(oke, "I-R4 (e): want\n    %s\n    %s\n    %s\n    %s", w1, w2, w3, w4);
    free(t);
    /* (f) pick contexts: ammo NULL, ti NULL, empty / broken (len % 0x60) / SSO / heap name vectors; a no-melee REJECT */
    const char *sso[] = { "melee_light", NULL }, *heap[] = { "a_heap_stored_melee_tag", NULL }, *none[] = { NULL };
    uint8_t *a_sso = mk_ammo(sso, 0), *a_heap = mk_ammo(heap, 0), *a_empty = mk_ammo(none, 0), *a_broken = mk_ammo(sso, 0);
    uint8_t *bv = a_broken + stance_name_vec_off();
    *(uint8_t **)(bv + 8) = *(uint8_t **)bv + 0x30;                     /* length 0x30: not a multiple of 0x60 */
    uint8_t *T2 = mk_unit(0x78, 1, 1, NULL), *ti2 = mk_ti(T2);
    char tp[32]; snprintf(tp, sizeof tp, "%p", (void *)T2);
    struct { const char *what; uint8_t *ammo; uint8_t *ti; int code; const char *ammo_txt; const char *tgt_txt; } v[] = {
        { "ammo NULL",            NULL,     ti2,  CORE_PICK_REJECT,                     "ammo none",                         tp },
        { "ti NULL",              a_sso,    NULL, CORE_PICK_REJECT,                     "ammo \"melee_light\"",              "none" },
        { "empty name vector",    a_empty,  ti2,  CORE_PICK_REJECT,                     "ammo \"\"",                         tp },
        { "broken name vector",   a_broken, ti2,  CORE_PICK_REJECT,                     "ammo \"\"",                         tp },
        { "SSO name",             a_sso,    ti2,  CORE_PICK_MELEE,                      "ammo \"melee_light\"",              tp },
        { "heap name",            a_heap,   ti2,  CORE_PICK_MELEE,                      "ammo \"a_heap_stored_melee_tag\"",  tp },
        { "no-melee REJECT",      a_sso,    ti2,  CORE_PICK_REJECT | CORE_PICK_NOMELEE, "ammo \"melee_light\"",              tp },
    };
    for (size_t i = 0; i < sizeof v / sizeof v[0]; i++) {
        uint32_t b0 = 40000 + 1000 * (uint32_t)i;
        uint8_t *U = mk_unit(0x200 + (uint32_t)i, 1, 1, NULL);
        front(U, b0); front(U, b0 + 100);
        set_ctx(ctx, v[i].ammo, U, v[i].ti);
        g_fake = b0 + 150; CORE_NOTE_PICK(CORE_MOD_STANCE, ctx, v[i].code);
        front(U, b0 + 200); front(U, b0 + 300); front(U, b0 + 400);
        t = cap_take(); d = flips_since(&f0);
        snprintf(want, sizeof want, "  stance: last %s at %u ms: %s vs %s", (v[i].code & CORE_PICK_NOMELEE) ? "no-melee REJECT" :
                 (v[i].code & 0xFF) == CORE_PICK_MELEE ? "MELEE" : "REJECT", b0 + 150, v[i].ammo_txt, v[i].tgt_txt);
        int ok = d == 1 && strstr(t, want);
        printf("I-R4 (f) pick context %-20s -> %s\n", v[i].what, ok ? "OK" : "FAIL");
        EXPECT(ok, "I-R4 (f) %s: want '%s'", v[i].what, want);
        free(t);
    }
    /* (g) a pen verdict with target NULL */
    uint8_t *F = mk_unit(0x301, 1, 1, NULL);
    front(F, 50000); front(F, 50100);
    CORE_NOTE_VERDICT(CORE_MOD_PEN, F, NULL, CORE_PEN_REJECT, 50150);
    front(F, 50200); front(F, 50300); front(F, 50400);
    t = cap_take(); d = flips_since(&f0);
    int okg = d == 1 && strstr(t, "  pen: last REJECT vs target none at 50150 ms");
    printf("I-R4 (g) pen verdict with target NULL: %s\n", okg ? "OK" : "FAIL");
    EXPECT(okg, "I-R4 (g)"); free(t);
    /* (h) a new battle clears the table and the suspects */
    uint8_t *G = mk_unit(0x401, 1, 1, NULL);
    front(G, 60000); front(G, 60100); front(G, 60200); front(G, 60300);
    CORE_NOTE_VERDICT(CORE_MOD_PEN, G, T, CORE_PEN_REJECT, 60350);
    int was = core_suspect[core_suspect_slot(G)].unit == G;
    uint32_t e0 = core_epoch();
    quant(60400); quant(100);                                            /* back 60300 ms: the core fires, link's listener clears */
    int zero = 1;
    for (int i = 0; i < CORE_SUSPECT_N; i++) if (core_suspect[i].unit || core_suspect[i].id || core_suspect[i].until) zero = 0;
    front(G, 200); front(G, 300); front(G, 400); front(G, 500);
    int before5 = flips_since(&f0);
    front(G, 600);
    t = cap_take(); d = flips_since(&f0);
    int okh = was && core_epoch() == e0 + 1 && zero && before5 == 0 && d == 1 && strstr(t, "  pen: none") && !strstr(t, "[60000 ms]")
           && strstr(t, "front order changed 5 times in 400 ms");
    printf("I-R4 (h) new battle: suspect before %d, epoch %u -> %u, every suspect slot zero %d, block after 5 new changes only (old ones and the pen "
           "verdict gone) -> %s\n", was, e0, core_epoch(), zero, okh ? "OK" : "FAIL");
    EXPECT(okh, "I-R4 (h)"); free(t);
    /* (i) review RP-1 / RQ-5: at most 2 blocks per unit per battle (a looping unit cannot spend FlipLines alone), one line when FlipLines
     * is reached, the trips not written counted at the next battle. This battle has G's block (h) so far. */
    uint8_t *P = mk_unit(0x701, 1, 1, NULL);
    for (int k = 0; k < 3; k++) for (int j = 0; j < 5; j++) front(P, 1000 + 11000 * (uint32_t)k + 100 * (uint32_t)j);   /* trips at 1400, 12400, 23400 */
    t = cap_take(); d = flips_since(&f0);
    int oki1 = d == 2 && count_sub(t, "(no more flip blocks for this unit in this battle: 2 per unit)") == 1;
    printf("I-R4 (i) one unit trips 3 times (each after its 10 s mute): %d blocks, the 2nd says it is the last -> %s\n", d, oki1 ? "OK" : "FAIL");
    EXPECT(oki1, "I-R4 (i): want 2 blocks for one unit, the 2nd with the 'no more flip blocks' line");
    free(t);
    for (int k = 0; k < 19; k++) {                                       /* 3 blocks so far: 17 more fill FlipLines=20, then 2 trips more */
        uint8_t *Q = mk_unit(0x800 + (uint32_t)k, 1, 1, NULL);
        for (int j = 0; j < 5; j++) front(Q, 30000 + 1000 * (uint32_t)k + 100 * (uint32_t)j);
    }
    t = cap_take(); d = flips_since(&f0);
    int oki2 = d == 17 && count_sub(t, "[link] flip: FlipLines=20 reached for this battle") == 1;
    printf("I-R4 (i) 19 more units trip: %d blocks (FlipLines=20 per battle), %d 'FlipLines=20 reached' line -> %s\n", d,
           count_sub(t, "[link] flip: FlipLines=20 reached for this battle"), oki2 ? "OK" : "FAIL");
    EXPECT(oki2, "I-R4 (i): want 17 blocks and one 'FlipLines=20 reached' line");
    free(t);
    quant(60000); quant(100);                                            /* new battle: the 3 trips not written are counted */
    t = cap_take();
    int oki3 = strstr(t, "[link] flip: 3 more trips in the last battle were not written (FlipLines=20 per battle, 2 per unit)") != NULL;
    printf("I-R4 (i) new battle: the count of trips not written -> %s\n", oki3 ? "OK" : "FAIL");
    EXPECT(oki3, "I-R4 (i): want '3 more trips in the last battle were not written'");
    free(t);
    /* (j) review MPC-4: a stance note that repeats the class of the unit's previous change (stance's unit slot was evicted and handed
     * back empty) is no change; chase notes are not filtered (k) */
    uint8_t *R = mk_unit(0x901, 1, 1, NULL);
    const void *va = k_vt[0], *vb = k_vt[1];
    static const uint32_t tj[6] = { 1000, 1100, 1200, 1300, 1400, 1500 };
    for (int j = 0; j < 6; j++) { g_fake = tj[j]; CORE_NOTE_FRONT(R, j ? vb : va, j ? k_onm[1] : k_onm[0], tj[j]); }
    t = cap_take(); d = flips_since(&f0);
    int okj1 = d == 0 && core_suspect[core_suspect_slot(R)].unit == R;
    free(t);
    g_fake = 1600; CORE_NOTE_FRONT(R, va, k_onm[0], 1600);
    g_fake = 1700; CORE_NOTE_FRONT(R, vb, k_onm[1], 1700);
    g_fake = 1800; CORE_NOTE_FRONT(R, va, k_onm[0], 1800);
    t = cap_take(); d = flips_since(&f0);
    snprintf(want, sizeof want, "unit %p (human, owner 1, id 0x901) front order changed 5 times in 800 ms", (void *)R);
    int okj = okj1 && d == 1 && strstr(t, want) && count_sub(t, "  [1") == 5;
    printf("I-R4 (j) A B B B B B (4 repeats of the same class) -> no block, suspect; then A B A -> one block with 5 changes -> %s\n", okj ? "OK" : "FAIL");
    EXPECT(okj, "I-R4 (j): want no block for the repeats, then '%s' with 5 change lines", want);
    free(t);
    /* (k) chase: every approach registration counts, same class each time (the OB-5 loop) */
    uint8_t *V = mk_unit(0x902, 0, 3, NULL);
    for (int j = 0; j < 5; j++) { g_fake = 3000 + 25 * (uint32_t)j; CORE_NOTE_FRONT(V, k_vt[0], "chase approach", 3000 + 25 * (uint32_t)j); }
    t = cap_take(); d = flips_since(&f0);
    snprintf(want, sizeof want, "unit %p (vehicle, owner 3, id 0x902) chase approach registered 5 times in 100 ms", (void *)V);
    int okk = d == 1 && strstr(t, want) && count_sub(t, "] chase approach (vt ") == 5;
    printf("I-R4 (k) 5 chase approach registrations in 100 ms, one class: one block '... chase approach registered 5 times' -> %s\n", okk ? "OK" : "FAIL");
    EXPECT(okk, "I-R4 (k): want '%s'", want);
    free(t);
    /* (l) review RS-1: without a core clock (the core's listener never fires) link's own rule clears its tables when a note's clock
     * steps back by [Core] NewBattleBackMs, per clock (stance's, chase's) */
    core_test_set_clock(NULL);
    uint8_t *W = mk_unit(0x903, 1, 1, NULL), *X = mk_unit(0x904, 1, 1, NULL), *Y = mk_unit(0x905, 0, 3, NULL);
    front(W, 9000); front(W, 9100);
    for (int j = 0; j < 2; j++) CORE_NOTE_FRONT(Y, k_vt[0], "chase approach", 9000 + 100 * (uint32_t)j);
    int wsus = core_suspect[core_suspect_slot(W)].unit == W, ysus = core_suspect[core_suspect_slot(Y)].unit == Y;
    free(cap_take());
    front(X, 500);                                                       /* stance clock 9100 -> 500 */
    t = cap_take();
    int okl1 = wsus && ysus && core_suspect[core_suspect_slot(W)].unit == NULL && core_suspect[core_suspect_slot(Y)].unit == NULL
            && strstr(t, "[link] flip: new battle on the stance clock (9100 -> 500 ms, link's own rule): flip tables cleared");
    free(t);
    for (int j = 0; j < 2; j++) CORE_NOTE_FRONT(Y, k_vt[0], "chase approach", 9000 + 100 * (uint32_t)j);
    free(cap_take());
    CORE_NOTE_FRONT(Y, k_vt[0], "chase approach", 200);                 /* chase clock 9100 -> 200 */
    t = cap_take();
    int okl = okl1 && strstr(t, "[link] flip: new battle on the chase clock (9100 -> 200 ms, link's own rule): flip tables cleared");
    printf("I-R4 (l) no core clock: a stance note 9100 -> 500 and a chase note 9100 -> 200 each clear link's tables (suspects gone) -> %s\n",
           okl ? "OK" : "FAIL");
    EXPECT(okl, "I-R4 (l): link's own new-battle rule");
    free(t);
    core_test_set_clock(&g_fake);
    quant(9000); quant(100);                                             /* a core battle for the cases after this one */
    free(cap_take());
}

static void ir5(void)
{
    char *t, want[400];
    uint8_t *T1 = mk_unit(0x501, 1, 1, "AI_Trace"), *T0 = mk_unit(0x502, 1, 1, NULL), *T2 = mk_unit(0x503, 1, 1, "ai_trace");
    uint8_t *tg = mk_unit(0x504, 1, 2, NULL), *ti = mk_ti(tg), ctx[0x20], ctx0[0x20];
    const char *an[] = { "melee_light", "melee_heavy", NULL };
    uint8_t *ammo = mk_ammo(an, 0);
    set_ctx(ctx, ammo, T1, ti); set_ctx(ctx0, ammo, T0, ti);
    long n0, tr0, n1, tr1;
    /* (a) Trace=0: the switch is off, nothing reaches link */
    link_test_counts(&n0, &tr0);
    core_trace_on = 0;
    g_fake = 70000;
    CORE_TRACE(CORE_MOD_CHASE, T1, "hold test %d", 1);
    CORE_TRACE_PICK(CORE_MOD_STANCE, ctx, CORE_PICK_MELEE);
    core_trace_on = 1;
    t = cap_take(); link_test_counts(&n1, &tr1);
    printf("I-R5 (a) Trace=0: %d trace lines, %ld trace calls -> %s\n", count_sub(t, "[link] trace:"), tr1 - tr0, !strstr(t, "trace:") && tr1 == tr0 ? "OK" : "FAIL");
    EXPECT(!strstr(t, "trace:") && tr1 == tr0, "I-R5 (a)"); free(t);
    /* (b) tagged unit (case-insensitive tag): a line per decision; (c) untagged: none; NULL unit (pen P6 may pass one): none */
    g_fake = 70100;
    CORE_TRACE(CORE_MOD_PEN, T1, "A: test %d", 7);
    CORE_TRACE_PICK(CORE_MOD_STANCE, ctx, CORE_PICK_MELEE);
    CORE_TRACE_PICK(CORE_MOD_STANCE, ctx, CORE_PICK_DOWNED);
    CORE_TRACE_PICK(CORE_MOD_STANCE, ctx, CORE_PICK_FAST);
    CORE_TRACE_PICK(CORE_MOD_STANCE, ctx, CORE_PICK_REJECT | CORE_PICK_NOMELEE);
    CORE_TRACE(CORE_MOD_CHASE, T1, "approach registered: %.1f m away", 12.5);
    CORE_TRACE(CORE_MOD_PEN, T0, "A: untagged %d", 1);
    CORE_TRACE_PICK(CORE_MOD_STANCE, ctx0, CORE_PICK_MELEE);
    CORE_TRACE(CORE_MOD_PEN, (const void *)NULL, "X: no shooter");
    t = cap_take();
    char w[6][300];
    snprintf(w[0], 300, "[link] trace: [70100 ms] pen unit %p: A: test 7", (void *)T1);
    snprintf(w[1], 300, "[link] trace: [70100 ms] stance unit %p: pick MELEE: ammo \"melee_light melee_heavy\" vs target %p", (void *)T1, (void *)tg);
    snprintf(w[2], 300, "[link] trace: [70100 ms] stance unit %p: pick downed target: no weapon: ammo \"melee_light melee_heavy\" vs target %p", (void *)T1, (void *)tg);
    snprintf(w[3], 300, "[link] trace: [70100 ms] stance unit %p: pick fast pass: engine's choice:", (void *)T1);
    snprintf(w[4], 300, "[link] trace: [70100 ms] stance unit %p: pick no-melee REJECT:", (void *)T1);
    snprintf(w[5], 300, "[link] trace: [70100 ms] chase unit %p: approach registered: 12.5 m away", (void *)T1);
    int okb = count_sub(t, "[link] trace:") == 6;
    for (int i = 0; i < 6; i++) if (!strstr(t, w[i])) { okb = 0; fail("I-R5 (b) missing '%s'", w[i]); }
    printf("I-R5 (b) tagged unit: 6 decisions -> 6 lines; (c) untagged unit and NULL unit: none -> %s\n", okb ? "OK" : "FAIL");
    EXPECT(okb, "I-R5 (b)/(c): %d trace lines (want 6)", count_sub(t, "[link] trace:"));
    free(t);
    /* (d) the tag test is cached per (entity, sim clock): the tag removed in the same ms is still seen, the next ms reads again */
    g_fake = 71000;
    CORE_TRACE(CORE_MOD_PEN, T2, "cache %d", 1);
    uint8_t *tv = T2 + stance_enttags_off();
    *(uint8_t **)(tv + 8) = *(uint8_t **)tv;                             /* tag vector now empty */
    CORE_TRACE(CORE_MOD_PEN, T2, "cache %d", 2);
    g_fake = 71001;
    CORE_TRACE(CORE_MOD_PEN, T2, "cache %d", 3);
    t = cap_take();
    int okd = strstr(t, ": cache 1") && strstr(t, ": cache 2") && !strstr(t, ": cache 3");
    printf("I-R5 (d) cache per (entity, sim ms): same ms after the tag was removed -> traced, next ms -> not traced: %s\n", okd ? "OK" : "FAIL");
    EXPECT(okd, "I-R5 (d)"); free(t);
    /* (e) TraceLines=2000 per battle, then one cap line; a new battle starts the count again */
    quant(71001); quant(500);                                            /* a new battle: the per-battle count starts at 0 */
    free(cap_take());
    for (int i = 0; i < 2100; i++) { g_fake = 600 + (uint32_t)i; CORE_TRACE(CORE_MOD_PEN, T1, "cap %d", i); }
    t = cap_read(0);
    int lines = count_sub(t, "[link] trace: ["), capl = count_sub(t, "[link] trace: TraceLines=2000 reached");
    snprintf(want, sizeof want, "[link] trace: [2599 ms] pen unit %p: cap 1999\n", (void *)T1);
    int oke1 = lines == 2000 && capl == 1 && strstr(t, want) && !strstr(t, ": cap 2000\n");
    free(t);
    quant(2700); quant(100);
    g_fake = 150; CORE_TRACE(CORE_MOD_PEN, T1, "after the cap %d", 1);
    t = cap_take();
    int oke2 = strstr(t, ": after the cap 1") != NULL;
    printf("I-R5 (e) 2100 decisions in one battle: %d lines + %d cap line (not echoed); after a new battle: %s -> %s\n", lines, capl,
           oke2 ? "traced again" : "still capped", oke1 && oke2 ? "OK" : "FAIL");
    EXPECT(oke1 && oke2, "I-R5 (e)"); free(t);
    /* (f) review RS-1: without a core clock the tag test is not cached (its key would never change): a tag added later is seen at once */
    core_test_set_clock(NULL);
    uint8_t *T3 = mk_unit(0x506, 1, 1, NULL);
    CORE_TRACE(CORE_MOD_PEN, T3, "no clock %d", 1);
    const char *tagged[] = { "faction_test", "ai_trace", NULL };
    mk_vec(T3 + stance_enttags_off(), tagged, 0);
    CORE_TRACE(CORE_MOD_PEN, T3, "no clock %d", 2);
    t = cap_take();
    int okf = !strstr(t, ": no clock 1") && strstr(t, ": no clock 2") != NULL;
    printf("I-R5 (f) no core clock: untagged -> no line, tag added -> traced at once (no stale cache) -> %s\n", okf ? "OK" : "FAIL");
    EXPECT(okf, "I-R5 (f)"); free(t);
    core_test_set_clock(&g_fake);
}

static void ir8(void)
{
    char out[512];
    /* 10 MHz counter: stance own 2.327 s over 1e6 calls, engine MatchAmmo 5 s, pen A 0.8 s over 4e6 calls */
    link_test_cost(23270000, 1000000, 50000000, 8000000, 4000000, 10000000, 0, 1234, out, sizeof out);
    static const char want[] = "cost: [1234 ms] stance own 2327000 us over 1000000 calls (2327 ns/call) | engine MatchAmmo 5000000 us incl. pen A 800000 us "
                               "over 4000000 calls (200 ns/call, timer bias 0 ns/call removed) | engine alone 4200000 us";
    int ok1 = !strcmp(out, want);
    printf("I-R8 %s\n", out);
    /* review MPC-2: a timer bias of half a tick (50 ns at 10 MHz) per pen A call comes off pen A and goes to "engine alone" */
    link_test_cost(23270000, 1000000, 50000000, 8000000, 4000000, 10000000, 32768, 1234, out, sizeof out);
    static const char wantb[] = "cost: [1234 ms] stance own 2327000 us over 1000000 calls (2327 ns/call) | engine MatchAmmo 5000000 us incl. pen A 600000 us "
                                "over 4000000 calls (150 ns/call, timer bias 50 ns/call removed) | engine alone 4400000 us";
    int okb = !strcmp(out, wantb);
    printf("I-R8 %s\n", out);
    /* no calls yet; a 2.6 GHz-class counter whose products overflow 64 bits (hq * 1e9); a bias larger than the time: pen A 0, not negative */
    link_test_cost(0, 0, 0, 0, 0, 10000000, 0, 5, out, sizeof out);
    int ok2 = !strcmp(out, "cost: [5 ms] stance own 0 us over 0 calls (0 ns/call) | engine MatchAmmo 0 us incl. pen A 0 us over 0 calls (0 ns/call, timer "
                           "bias 0 ns/call removed) | engine alone 0 us");
    link_test_cost(36000000000000ll, 2000000000l, 72000000000000ll, 18000000000000ll, 1000000000l, 10000000000ll, 0, 7, out, sizeof out);
    int ok3 = !strcmp(out, "cost: [7 ms] stance own 3600000000 us over 2000000000 calls (1800 ns/call) | engine MatchAmmo 7200000000 us incl. pen A 1800000000 "
                           "us over 1000000000 calls (1800 ns/call, timer bias 0 ns/call removed) | engine alone 5400000000 us");
    char out4[512];
    link_test_cost(100, 1, 1000, 10, 10, 10000000, 3 * 65536, 9, out4, sizeof out4);
    int ok4 = strstr(out4, "incl. pen A 0 us over 10 calls (0 ns/call, timer bias 300 ns/call removed) | engine alone 100 us") != NULL;
    printf("I-R8 zero counters, 64-bit overflow-sized counters, bias above the time: %s / %s / %s; [Link] CostLine=0 -> timing switch %d\n", ok2 ? "OK" : "FAIL",
           ok3 ? "OK" : "FAIL", ok4 ? "OK" : "FAIL", core_timing_on);
    EXPECT(ok1, "I-R8: want '%s'", want);
    EXPECT(okb, "I-R8 bias: want '%s'", wantb);
    EXPECT(ok2 && ok3, "I-R8 zero / large counters: last '%s'", out);
    EXPECT(ok4, "I-R8 bias above the time: '%s'", out4);
    EXPECT(core_timing_on == 0, "I-R8: CostLine=0 must leave core_timing_on 0");
}

static void ir8b(void)
{
    uint8_t *N = mk_unit(0x601, 1, 1, NULL), *tg = mk_unit(0x602, 1, 2, NULL), *ti = mk_ti(tg), ctx[0x20];
    const char *an[] = { "melee_light", NULL };
    set_ctx(ctx, mk_ammo(an, 0), N, ti);
    EXPECT(core_suspect[core_suspect_slot(N)].unit != N && !core_suspect_all, "I-R8b: N is a suspect");
    long n0, t0, n1, t1;
    link_test_counts(&n0, &t0);
    int64_t q0 = core_qpc();
    for (int i = 0; i < 100000; i++) {
        CORE_NOTE_PICK(CORE_MOD_STANCE, ctx, (i & 1) ? CORE_PICK_REJECT : CORE_PICK_MELEE);
        CORE_NOTE_VERDICT(CORE_MOD_PEN, N, tg, CORE_PEN_REJECT, (uint32_t)i);
    }
    int64_t q1 = core_qpc();
    link_test_counts(&n1, &t1);
    LARGE_INTEGER f; QueryPerformanceFrequency(&f);
    printf("I-R8b: 100000 non-PASS picks + 100000 pen verdicts of a non-suspect -> %ld hook calls (%.1f ns per macro, information) -> %s\n",
           n1 - n0, (double)(q1 - q0) * 1e9 / (double)f.QuadPart / 200000.0, n1 == n0 ? "OK" : "FAIL");
    EXPECT(n1 == n0, "I-R8b: %ld hook calls", n1 - n0);
}

static int run_link(void)
{
    char ini[MAX_PATH + 32];
    snprintf(ini, sizeof ini, "%s\\link.ini", g_tmp);
    FILE *f = fopen(ini, "wb"); if (!f) { printf("cannot write %s\nLINK FAILED\n", ini); return 1; }
    fputs("[Link]\r\nFlip=1\r\nTrace=1\r\n", f);
    fclose(f);
    core_test_set_ini(ini);
    core_test_set_clock(&g_fake);
    cap_open("link.log");
    g_fake = 0;
    link_attach(0, 0, 0);
    free(cap_take());
    EXPECT(core_flip_on == 1 && core_trace_on == 1 && core_timing_on == 0 && !core_suspect_all, "switches after link_attach: flip %d trace %d timing %d",
           core_flip_on, core_trace_on, core_timing_on);
    EXPECT(stance_order_poll_state() == -2, "stance_order_poll_state() = %d with no stance install (want -2: no attack-order vtable, review N2)",
           stance_order_poll_state());
    ir4();
    ir5();
    ir8();
    ir8b();
    printf(g_fail ? "LINK FAILED (%d)\n" : "LINK OK\n", g_fail);
    return g_fail ? 1 : 0;
}

/* ================================================================================================ --attach-time (STARTUP_batch) */
/* map the exe, time ONE core_attach_all dry run on it, print one TIME| line (time, all modules ON, the [core] code writes line).
 * walk = dry 1 (the --install path: one thread walk per code write), batch = dry 2 (the attach batch the real DllMain uses, asked for
 * explicitly: in a dry run the loader-lock invariant does not hold, which does not matter for a time or for counting the walks).
 * <ini>: another ini (the A/B floor: PatchSuspendThreads=0). Used by work\startup\ab_timing.py and parity P-3c, not by run-test */
static int run_attach_time(const char *exe, const char *mode, const char *ini)
{
    int dry = !strcmp(mode, "walk") ? 1 : !strcmp(mode, "batch") ? 2 : 0;
    if (!dry) { printf("--attach-time: the mode is walk or batch\n"); return 2; }
    int rel = 0;
    uint8_t *base = map_image(exe, &rel, &g_imgsz);
    if (!base) { printf("TIME FAILED\n"); return 1; }
    g_base = base;
    const char *nm = base_name(exe);
    core_test_set_exe(base, nm);
    core_test_set_ini(ini ? ini : g_ini);
    char cn[MAX_PATH]; snprintf(cn, sizeof cn, "time_%s_%s.log", mode, nm);
    cap_open(cn);
    LARGE_INTEGER f; QueryPerformanceFrequency(&f);
    int64_t q0 = core_qpc();
    core_attach_all(GetModuleHandleA(NULL), dry);
    int64_t q1 = core_qpc();
    char *log = cap_read(0), l[1024];
    const char *cw = find_line(log, "[core] code writes:", l, sizeof l) ? strstr(l, "code writes:") : "-";
    int on = strstr(log, "[core] modules: chase ON, stance ON, pen ON |") != NULL;
    printf("TIME|%s|%s|%.1f|%s|%s\n", nm, mode, (double)(q1 - q0) * 1000.0 / (double)(f.QuadPart > 0 ? f.QuadPart : 1), on ? "all ON" : "NOT all ON", cw);
    free(log);
    return on ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (!setup_paths()) return 2;
    if (argc >= 3 && !strcmp(argv[1], "--install")) return run_install(argv[2]);
    if (argc >= 2 && !strcmp(argv[1], "--epoch")) return run_epoch();
    if (argc >= 3 && !strcmp(argv[1], "--replay")) return run_replay(argv[2]);
    if (argc >= 2 && !strcmp(argv[1], "--link")) return run_link();
    if (argc >= 4 && !strcmp(argv[1], "--attach-time")) return run_attach_time(argv[2], argv[3], argc >= 5 ? argv[4] : NULL);
    printf("usage: gem_melee_itest.exe --install <exe> | --epoch | --replay <MeleeStance log> | --link | --attach-time <exe> walk|batch [<ini>]\n");
    return 2;
}
