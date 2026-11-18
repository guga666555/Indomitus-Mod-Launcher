/* GEM ReviveFilter 0.1 - who may revive a downed soldier, by tags (Call to Arms: Gates of Hell, GEM 2, 1.068)
 *
 * Rule (read on every check, so script {tags add/remove} acts at once):
 *   patient tag or def prop "revive_by_<class>" (any number) -> only units of one of those classes may revive it
 *   patient with none                                       -> anyone may revive it (vanilla)
 *   unit tag "<UnitPrefix><class>" (any number)             -> the unit's classes (breed {tags} through GEM
 *                                                              BreedTags, or script {tags add})
 *   Exact, case-sensitive match after the prefix. Prefixes in the ini.
 * Example: Space Marine breed {tags ... "revive_by_apothecary"}, Apothecary breed {tags ... "apothecary"}
 * (UnitPrefix empty). A Guard medic cannot revive the marine; the Apothecary revives marines and Guardsmen alike.
 * A refused player command fires script event "revive_refused" (ini PopupEvent) on the patient, once per
 * PopupCooldownMs per unit + patient pair (the mod shows "This action requires an Apothecary").
 *
 * Engine (game exe 1.068 offsets; everything is found at runtime, same code in editor / server):
 *   Order::eMedic          the revive order (player click, auto-medic). vtable slot 19 = Execute(order, phase):
 *                          phase 0 start, 1 update, 2 finish; returns 0 running, 1 done, 2 failed. order+8 = actor
 *                          interface (entity = iface - 0xAF0), order+0x48 = the patient (entity pointer, set by
 *                          the order factory exe+0x378670).
 *   Interface::Action::e_medic   the player's revive command. vtable slot 17 = Apply(action, actor, target, r9,
 *                          flag): builds the order for the patient at target+0x20 (factory), gives it to the
 *                          actor, then ends in a tail call (UI: back to the default action).
 *   Runtime tags / def props: as GEM ReviveFilter (entity + 0x1D0 + 0x318; entity+0x548 -> def, def+0x330).
 * This dll swaps those two vtable slots:
 *   Apply   refused pair -> the order is never built, only the tail runs.
 *   Execute refused pair in the update phase -> returns 2 (failed) and the engine runs the finish phase. Covers
 *           orders from any other source (auto-medic, script).
 *
 * MP: the order runs in the simulation, so every machine (and the server, ini Server=1) needs the dll.
 *
 * Build: build.bat   Offline finder + rule + wrapper test on the three exes: build.bat test
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#define RF_VERSION "0.1"
#define RF_SECT    "ReviveFilter"
#define MAXC 16
#define CLEN 48

typedef struct { union { char buf[16]; char *ptr; } u; size_t size, cap; } MsString;

/* ---------------------------------------------------------------- config */
static int  cfg_enabled = 1, cfg_log = 0, cfg_server = 1, cfg_apply = 1;
static char cfg_vpre[CLEN] = "revive_by_", cfg_upre[CLEN] = "";
static size_t cfg_vlen = 10, cfg_ulen = 0;

/* ---------------------------------------------------------------- log */
static char  g_logpath[MAX_PATH];
static FILE *g_log;
static volatile LONG g_lines;
static void logf_(const char *fmt, ...)
{
    if (InterlockedIncrement(&g_lines) > 400) return;
#ifndef RF_TEST
    if (!g_log && g_logpath[0]) g_log = fopen(g_logpath, "w");
    if (!g_log) return;
    FILE *out = g_log;
#else
    FILE *out = stdout;
#endif
    va_list ap; va_start(ap, fmt);
    vfprintf(out, fmt, ap); fputc('\n', out); fflush(out);
    va_end(ap);
}

/* ---------------------------------------------------------------- PE helpers */
typedef struct { uint8_t *p; size_t n; } Sec;
static Sec g_text, g_rdata, g_data;
static uint8_t *g_base;
#define OFF(a) ((unsigned long long)((uint8_t *)(a) - g_base))

static int get_sections(uint8_t *base)
{
    g_base = base; g_text.p = g_rdata.p = g_data.p = NULL;
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_SECTION_HEADER *sh = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        Sec s = { base + sh[i].VirtualAddress, sh[i].Misc.VirtualSize };
        if (!strncmp((char *)sh[i].Name, ".text", 8)) g_text = s;
        else if (!strncmp((char *)sh[i].Name, ".rdata", 8)) g_rdata = s;
        else if (!strncmp((char *)sh[i].Name, ".data", 8)) g_data = s;
    }
    return g_text.p && g_rdata.p && g_data.p;
}
static int in_text(const void *a) { return (uint8_t *)a >= g_text.p && (uint8_t *)a < g_text.p + g_text.n; }
static uint8_t *rel32(uint8_t *at, int len) { return at + len + *(int32_t *)(at + len - 4); }

static size_t parse_pat(const char *s, uint8_t *b, uint8_t *m)
{
    size_t n = 0;
    while (*s) {
        while (*s == ' ') s++;
        if (!*s) break;
        if (s[0] == '?') { b[n] = 0; m[n] = 0; }
        else { b[n] = (uint8_t)strtoul(s, NULL, 16); m[n] = 1; }
        n++; s += 2;
    }
    return n;
}
static uint8_t *scan(uint8_t *p, size_t n, const char *pat, int *count)
{
    uint8_t b[96], m[96], *first = NULL; size_t k = parse_pat(pat, b, m);
    if (count) *count = 0;
    if (k > n) return NULL;
    for (size_t i = 0; i + k <= n; i++) {
        if (p[i] != b[0]) continue;
        size_t j = 1;
        while (j < k && (!m[j] || p[i + j] == b[j])) j++;
        if (j < k) continue;
        if (!first) first = p + i;
        if (!count) break;
        ++*count;
    }
    return first;
}

/* the one vtable of class `name` (RTTI type name, e.g. ".?AUeMedic@Order@@") whose object offset is 0 */
static void **find_vtable(const char *name)
{
    size_t nl = strlen(name) + 1;
    int found = 0; void **vt = NULL;
    for (uint8_t *p = g_data.p; p + nl <= g_data.p + g_data.n; p++) {
        if (*p != '.' || memcmp(p, name, nl)) continue;
        uint32_t td = (uint32_t)(p - 0x10 - g_base);
        for (uint8_t *q = g_rdata.p; q + 24 <= g_rdata.p + g_rdata.n; q += 4) {
            uint32_t *col = (uint32_t *)q;
            if (col[0] != 1 || col[1] != 0 || col[3] != td || col[5] != (uint32_t)(q - g_base)) continue;
            uint64_t want = (uint64_t)(uintptr_t)q;
            for (uint8_t *r = g_rdata.p; r + 16 <= g_rdata.p + g_rdata.n; r += 8)
                if (*(uint64_t *)r == want) { vt = (void **)(r + 8); found++; }
        }
    }
    return found == 1 ? vt : NULL;
}

/* ---------------------------------------------------------------- engine bindings */
#define ENT_KIND   0x30
#define KIND_HUMAN 0x04

typedef uint64_t (*ExecFn)(void *order, uint64_t phase, uint64_t a3, uint64_t a4);
typedef void (*ApplyFn)(void *act, uint8_t *actor, void *target, void *a4, uint64_t a5);
typedef void (*TailFn)(void *ui, void *act);

static void   **g_vt_order, **g_vt_act;
static ExecFn   o_exec;
static ApplyFn  o_apply;
static TailFn   f_tail;
static void   **g_tail_ui;                      /* global the tail's first argument is loaded from */
static int32_t  o_iface;                        /* entity = iface + o_iface (negative) */
static uint32_t o_target, o_cmd_target, o_tags, o_def, o_props;
static const char *g_apply_err;

/* Runtime tags, read from the engine's own vehicle check "has tag no_auto_repair":
 *   lea rdx,"no_auto_repair" ; lea rcx,[TAG] ; call TagCtor              (static init of the Tag)
 *   ... add rcx,C ; lea rdx,[TAG] ; call HasTag   (editor)  or  lea rdx,[TAG] ; mov rcx,[rax+x] ; add rcx,C ; call HasTag
 *   HasTag: mov rdi,[rcx+B+8] ; mov rsi,rdx ; mov rbx,[rcx+B] ; ... add rbx,60h.   Tags = entity + C + B. */
static int find_tags(void)
{
    static const char name[] = "\0no_auto_repair";
    uint8_t *str = NULL;
    for (uint8_t *p = g_rdata.p; p + sizeof name <= g_rdata.p + g_rdata.n; p++)
        if (*p == 0 && !memcmp(p, name, sizeof name)) { str = p + 1; break; }
    if (!str) return 0;
    uint8_t *tag = NULL;
    for (uint8_t *p = g_text.p; p + 19 <= g_text.p + g_text.n && !tag; p++)
        if (p[0] == 0x48 && p[1] == 0x8D && p[2] == 0x15 && p[7] == 0x48 && p[8] == 0x8D && p[9] == 0x0D && p[14] == 0xE8 && rel32(p, 7) == str)
            tag = rel32(p + 7, 7);
    if (!tag) return 0;
    for (uint8_t *p = g_text.p; p + 7 <= g_text.p + g_text.n; p++) {
        if (p[0] != 0x48 || p[1] != 0x8D || p[2] != 0x15 || rel32(p, 7) != tag) continue;
        uint8_t *add = scan(p - 7, 7, "48 81 C1", NULL);
        if (!add) add = scan(p + 7, 8, "48 81 C1", NULL);
        if (!add) continue;
        uint8_t *call = scan(add + 7, 8, "E8", NULL);
        if (!call || call < p) call = scan(p + 7, 12, "E8", NULL);
        if (!call) continue;
        uint8_t *ht = rel32(call, 5);
        if (!in_text(ht)) continue;
        /* two loads mov r64,[rcx+d32] with d and d+8 (begin / end, either order, any registers), stride 60h */
        uint32_t d[2]; int nd = 0;
        for (uint8_t *q = ht; q < ht + 0x20 && nd < 2; q++)
            if (q[0] == 0x48 && q[1] == 0x8B && (q[2] & 0xC7) == 0x81) d[nd++] = *(uint32_t *)(q + 3);
        if (nd != 2 || (d[0] != d[1] + 8 && d[1] != d[0] + 8)) continue;
        int stride = 0;
        for (uint8_t *q = ht; q < ht + 0x40; q++)
            if (q[0] == 0x48 && q[1] == 0x83 && (q[2] & 0xF8) == 0xC0 && q[3] == 0x60) { stride = 1; break; }
        if (!stride) continue;
        o_tags = *(uint32_t *)(add + 3) + (d[0] < d[1] ? d[0] : d[1]);
        return o_tags > 0x100 && o_tags < 0x2000;
    }
    return 0;
}

/* def props: mov rD,[rX+DEF] ; mov rE,[rD+PROPS+8] ; mov rB,[rD+PROPS] ; cmp  -- vote, as GEM BoardFilter */
static int find_props(void)
{
    uint32_t ka[8], kb[8]; int cnt[8] = { 0 }, nc = 0;
    for (uint8_t *p = g_text.p; p + 24 <= g_text.p + g_text.n; p++) {
        if (p[0] != 0x48 || p[1] != 0x8B || p[7] != 0x48 || p[8] != 0x8B || p[14] != 0x48 || p[15] != 0x8B ||
            p[21] != 0x48 || p[22] != 0x3B) continue;
        uint8_t m1 = p[2], m2 = p[9], m3 = p[16];
        if ((m1 >> 6) != 2 || (m2 >> 6) != 2 || (m3 >> 6) != 2) continue;
        uint8_t r1 = (m1 >> 3) & 7;
        if ((m1 & 7) == 4 || (m2 & 7) != r1 || (m3 & 7) != r1) continue;
        uint32_t a = *(uint32_t *)(p + 3), b = *(uint32_t *)(p + 10), c = *(uint32_t *)(p + 17);
        if (b != c + 8 || a < 0x100 || a > 0x2000 || c < 0x100 || c > 0x2000) continue;
        int i = 0;
        while (i < nc && !(ka[i] == a && kb[i] == c)) i++;
        if (i == nc) { if (nc == 8) continue; ka[nc] = a; kb[nc] = c; nc++; }
        cnt[i]++;
    }
    int best = -1, second = 0;
    for (int i = 0; i < nc; i++) if (best < 0 || cnt[i] > cnt[best]) best = i;
    for (int i = 0; i < nc; i++) if (i != best && cnt[i] > second) second = cnt[i];
    if (best < 0 || cnt[best] < 20 || cnt[best] < 2 * second) return 0;
    o_def = ka[best]; o_props = kb[best];
    return 1;
}

static const char *find_apply(void)
{
    uint8_t *ap = (uint8_t *)g_vt_act[17];
    if (!in_text(ap)) return "e_medic slot 17 not in .text";
    /* mov rdi,rcx ; mov rbx,rdx ; mov rcx,[r8+PATIENT] ; call OrderFactory */
    uint8_t *r = scan(ap, 0x20, "48 8B F9 48 8B DA 49 8B 48 ?? E8", NULL);
    if (!r) return "patient load not found";
    o_cmd_target = r[9];
    /* the factory stores the patient at order + o_target */
    uint8_t *fac = rel32(r + 10, 5);
    if (!in_text(fac)) return "order factory not in .text";
    int st = 0;                                    /* any lea r64,[r64+o_target] (the editor inlines the assignment) */
    for (uint8_t *q = fac; q < fac + 0xA0 && !st; q++)
        st = q[0] == 0x48 && q[1] == 0x8D && (q[2] & 0xC0) == 0x40 && (q[2] & 7) != 4 && q[3] == o_target;
    if (!st) return "order factory does not store the patient where Execute reads it";
    /* the tail: mov rcx,[rip+ui] ... jmp Tail  (after the give-order call) */
    uint8_t *m = scan(r + 15, 0x40, "48 8B 0D", NULL);
    if (!m) return "tail argument load not found";
    g_tail_ui = (void **)rel32(m, 7);
    uint8_t *j = NULL;
    for (uint8_t *q = m + 7; q < m + 0x30; q++)
        if (*q == 0xE9 && in_text(rel32(q, 5))) { j = q; break; }
    if (!j) return "tail jump not found";
    f_tail = (TailFn)rel32(j, 5);
    /* Tail: push rbx ; sub rsp,40h ; mov rbx,rdx ; cmp [rcx+x],rdx */
    if (!scan((uint8_t *)f_tail, 12, "40 53 48 83 EC ?? 48 8B DA 48 39 91", NULL)) return "tail function not recognised";
    return NULL;
}

static const char *g_event_err;
static const char *find_event(void);

static const char *find_all(uint8_t *base)
{
    if (!get_sections(base)) return "sections not found";
    g_vt_order = find_vtable(".?AUeMedic@Order@@");
    if (!g_vt_order) return "Order::eMedic vtable not found";
    uint8_t *ex = (uint8_t *)g_vt_order[19];
    if (!in_text(ex)) return "eMedic slot 19 not in .text";
    /* phase switch: test ebx,ebx ; je ; sub ebx,1 ; je ; cmp ebx,1   (editor: the same on edx) */
    if (!scan(ex, 0x80, "85 DB 0F 84 ?? ?? ?? ?? 83 EB 01 0F 84 ?? ?? ?? ?? 83 FB 01", NULL) &&
        !scan(ex, 0x80, "85 D2 0F 84 ?? ?? ?? ?? 83 EA 01 0F 84 ?? ?? ?? ?? 83 FA 01", NULL)) return "eMedic Execute not recognised";
    /* lea rdx,[rax-iface] */
    uint8_t *l = scan(ex, 0x100, "48 8D 90 ?? ?? FF FF", NULL);
    if (!l) return "actor interface offset not found";
    o_iface = *(int32_t *)(l + 3);
    /* the patient: mov rax,[rsi+T] ; test rax,rax ; je ; mov byte [rax+x],0   (find_apply checks T against the factory) */
    uint8_t *t = scan(ex, 0x100, "48 8B 46 ?? 48 85 C0 74", NULL);
    if (!t) return "order patient offset not found";
    o_target = t[3];
    if (o_target < 0x10 || o_target >= 0x80) return "order patient offset out of range";
    if (o_iface > -0x400 || o_iface < -0x2000) return "actor interface offset out of range";

    g_vt_act = find_vtable(".?AUe_medic@Action@Interface@@");
    g_apply_err = g_vt_act ? find_apply() : "Interface::Action::e_medic vtable not found";

    if (!find_tags()) return "runtime tag offset not found";
    if (!find_props()) return "def props offsets not found";
    g_event_err = find_event();                 /* optional: only the popup needs it */
    return NULL;
}

/* ---------------------------------------------------------------- refusal event (popup) */
/* The engine's "throw_off_end" site, as GEM BoardFilter uses it:
 *   lea rdx,"throw_off_end"; lea rcx,tag; call TagCtor; ... mov rdx,rXX; lea rcx,[rsp+x]; call CtxCtor;
 *   lea rdx,tag; mov rcx,rax; call Fire; lea rcx,[rsp+x]; call CtxDtor */
typedef void *(*TagCtorFn)(void *tag, const char *name);
typedef void *(*CtxCtorFn)(void *buf, const void *ent);
typedef void  (*FireFn)(void *ctx, const void *tag);
typedef void  (*CtxDtorFn)(void *buf);
static TagCtorFn f_tagctor; static CtxCtorFn f_ctxctor; static FireFn f_fire; static CtxDtorFn f_ctxdtor;
static char cfg_event[CLEN] = "revive_refused";
static unsigned cfg_popup_ms = 3000;

static uint8_t *d32(uint8_t *p) { return p + 4 + *(int32_t *)p; }
static uint8_t *skip_nop(uint8_t *p) { while (*p == 0x90) p++; return p; }

static const char *find_event(void)
{
    static const char name[] = "\0throw_off_end";
    uint8_t *str = NULL;
    for (uint8_t *p = g_rdata.p; p + sizeof name <= g_rdata.p + g_rdata.n; p++)
        if (!memcmp(p, name, sizeof name)) { str = p + 1; break; }
    if (!str) return "throw_off_end string not found";
    for (uint8_t *p = g_text.p; p + 7 <= g_text.p + g_text.n; p++) {
        if (p[0] != 0x48 || p[1] != 0x8D || p[2] != 0x15 || d32(p + 3) != str) continue;
        uint8_t *q = skip_nop(p + 7);
        if (q[0] != 0x48 || q[1] != 0x8D || q[2] != 0x0D) continue;
        uint8_t *tag = d32(q + 3); q = skip_nop(q + 7);
        if (q[0] != 0xE8) continue;
        uint8_t *tagctor = d32(q + 1); q += 5;
        for (uint8_t *r = q; r < q + 0x40; r++) {
            uint8_t *x = r;
            if (x[0] != 0x48 || x[1] != 0x8B || (x[2] & 0xF8) != 0xD0) continue;
            x = skip_nop(x + 3);
            if (x[0] != 0x48 || x[1] != 0x8D || x[2] != 0x4C || x[3] != 0x24) continue;
            x = skip_nop(x + 5);
            if (x[0] != 0xE8) continue;
            uint8_t *ctx = d32(x + 1); x = skip_nop(x + 5);
            if (x[0] != 0x48 || x[1] != 0x8D || x[2] != 0x15 || d32(x + 3) != tag) continue;
            x = skip_nop(x + 7);
            if (x[0] != 0x48 || x[1] != 0x8B || x[2] != 0xC8) continue;
            x = skip_nop(x + 3);
            if (x[0] != 0xE8) continue;
            uint8_t *fire = d32(x + 1); x = skip_nop(x + 5);
            if (x[0] != 0x48 || x[1] != 0x8D || x[2] != 0x4C || x[3] != 0x24) continue;
            x = skip_nop(x + 5);
            if (x[0] != 0xE8) continue;
            uint8_t *dtor = d32(x + 1);
            if (!in_text(tagctor) || !in_text(ctx) || !in_text(fire) || !in_text(dtor)) continue;
            f_tagctor = (TagCtorFn)tagctor; f_ctxctor = (CtxCtorFn)ctx; f_fire = (FireFn)fire; f_ctxdtor = (CtxDtorFn)dtor;
            return NULL;
        }
    }
    return "event fire sequence not found";
}

static __attribute__((aligned(16))) uint8_t g_tag[0x80];   /* engine Tag is 0x60 bytes */
static int g_tag_ready;
typedef struct { const void *u, *v; ULONGLONG last; } pair_t;
static pair_t g_pairs[32];
static unsigned g_pair_next;

/* 1 = show the popup for this unit + patient now (once per PopupCooldownMs per pair) */
static int pair_due(const void *u, const void *v)
{
    ULONGLONG now = GetTickCount64();
    for (int i = 0; i < 32; i++)
        if (g_pairs[i].u == u && g_pairs[i].v == v) {
            if (now - g_pairs[i].last < cfg_popup_ms) return 0;
            g_pairs[i].last = now;
            return 1;
        }
    pair_t *p = &g_pairs[g_pair_next++ & 31];
    p->u = u; p->v = v; p->last = now;
    return 1;
}

/* Command refusals only: the player's click runs on the engine thread that runs the simulation (same thread as
 * script events), on the clicking player's machine; the handler only shows a Damage_Report. */
static void fire_refused(const uint8_t *veh)
{
    if (!f_fire || !cfg_event[0]) return;
    if (!g_tag_ready) { f_tagctor(g_tag, cfg_event); g_tag_ready = 1; }   /* lazy: engine statics not ready at inject */
    __attribute__((aligned(16))) uint8_t buf[0x400];                        /* engine context is >= 0x18C bytes */
    void *ctx = f_ctxctor(buf, veh);
    f_fire(ctx, g_tag);
    f_ctxdtor(buf);
}

/* ---------------------------------------------------------------- the rule */
static const char *ms_cstr(const MsString *s) { return s->cap > 15 ? s->u.ptr : s->u.buf; }

typedef struct { char c[MAXC][CLEN]; int n; } cls_t;

/* collect the <class> part of every Tag in vector vec whose name starts with pre */
static void collect(const uint8_t *vec, const char *pre, size_t pl, cls_t *out)
{
    const uint8_t *b = *(const uint8_t *const *)vec, *e = *(const uint8_t *const *)(vec + 8);
    if (!b || e <= b || (size_t)(e - b) > 0x60 * 2048 || (size_t)(e - b) % 0x60) return;
    for (const uint8_t *t = b; t < e; t += 0x60) {
        const MsString *s = (const MsString *)(t + 0x40);
        if (s->size <= pl || s->size >= pl + CLEN || s->cap < s->size) continue;
        const char *c = ms_cstr(s);
        if (memcmp(c, pre, pl)) continue;
        if (out->n >= MAXC) return;
        memcpy(out->c[out->n], c + pl, s->size - pl);
        out->c[out->n][s->size - pl] = 0;
        out->n++;
    }
}

/* 1 = the Tag vector vec holds a tag named exactly pre + cls */
static int has_tag(const uint8_t *vec, const char *pre, size_t pl, const char *cls)
{
    const uint8_t *b = *(const uint8_t *const *)vec, *e = *(const uint8_t *const *)(vec + 8);
    size_t cl = strlen(cls);
    if (!b || e <= b || (size_t)(e - b) > 0x60 * 2048 || (size_t)(e - b) % 0x60) return 0;
    for (const uint8_t *t = b; t < e; t += 0x60) {
        const MsString *s = (const MsString *)(t + 0x40);
        if (s->size != pl + cl || s->cap < s->size) continue;
        const char *c = ms_cstr(s);
        if (!memcmp(c, pre, pl) && !memcmp(c + pl, cls, cl)) return 1;
    }
    return 0;
}

static volatile LONG g_refused, g_checked;

/* 1 = actor may revive target */
static int rf_allowed(uint8_t *actor, uint8_t *target, const char *where)
{
    if (!cfg_enabled || !actor || !target || actor == target) return 1;
    InterlockedIncrement(&g_checked);
    cls_t need = { .n = 0 };
    collect(target + o_tags, cfg_vpre, cfg_vlen, &need);
    uint8_t *def = *(uint8_t **)(target + o_def);
    if (def) collect(def + o_props, cfg_vpre, cfg_vlen, &need);
    if (!need.n) return 1;
    for (int k = 0; k < need.n; k++)
        if (has_tag(actor + o_tags, cfg_upre, cfg_ulen, need.c[k])) return 1;
    LONG n = InterlockedIncrement(&g_refused);
    if (cfg_log && n <= 300) {
        char nd[200] = "";
        for (int k = 0; k < need.n; k++) {
            strncat(nd, cfg_upre, sizeof nd - strlen(nd) - 1);
            strncat(nd, need.c[k], sizeof nd - strlen(nd) - 1);
            strncat(nd, " ", sizeof nd - strlen(nd) - 1);
        }
        logf_("refused (%s): unit %p (id 0x%X) -> patient %p (id 0x%X, needs unit tag: %s) [%ld]", where, (void *)actor,
              *(uint32_t *)(actor + 0xC0), (void *)target, *(uint32_t *)(target + 0xC0), nd, n);
    }
    return 0;
}

/* ---------------------------------------------------------------- the two wrappers */
static uint64_t rf_exec(void *order, uint64_t phase, uint64_t a3, uint64_t a4)
{
    if (phase == 1) {
        uint8_t *iface = *(uint8_t **)((uint8_t *)order + 8);
        uint8_t *actor = iface ? iface + o_iface : NULL;
        uint8_t *target = *(uint8_t **)((uint8_t *)order + o_target);
        if (target && !rf_allowed(actor, target, "order")) return 2;   /* the engine's own "failed" */
    }
    return o_exec(order, phase, a3, a4);
}

static void rf_apply(void *act, uint8_t *actor, void *target, void *a4, uint64_t a5)
{
    if (cfg_apply && actor && target) {
        uint8_t *veh = *(uint8_t **)((uint8_t *)target + o_cmd_target);
        if (veh && !rf_allowed(actor, veh, "command")) {
            if (pair_due(actor, veh)) fire_refused(veh);
            f_tail(*g_tail_ui, act);                    /* what Apply always ends with */
            return;
        }
    }
    o_apply(act, actor, target, a4, a5);
}

static int swap_slot(void **slot, void *fn, void **orig)
{
    DWORD old;
    if (!VirtualProtect(slot, 8, PAGE_READWRITE, &old)) return 0;
    *orig = *slot;
    *slot = fn;
    VirtualProtect(slot, 8, old, &old);
    return 1;
}

/* ---------------------------------------------------------------- config */
/* a key set to nothing ("UnitPrefix=") gives an empty prefix when allow_empty; a missing key keeps the default */
static void read_prefix(const char *ini, const char *key, char *dst, size_t *len, int allow_empty)
{
    char buf[CLEN];
    GetPrivateProfileStringA(RF_SECT, key, dst, buf, sizeof buf, ini);
    size_t n = strlen(buf);
    while (n && (buf[n - 1] == ' ' || buf[n - 1] == '\t')) buf[--n] = 0;
    if ((n >= 2 || (n == 0 && allow_empty)) && n < CLEN - 8) strcpy(dst, buf);
    *len = strlen(dst);
}
static void load_ini(const char *ini)
{
    cfg_enabled = GetPrivateProfileIntA(RF_SECT, "Enabled", 1, ini);
    cfg_log     = GetPrivateProfileIntA(RF_SECT, "Log", 0, ini);
    cfg_server  = GetPrivateProfileIntA(RF_SECT, "Server", 1, ini);
    cfg_apply   = GetPrivateProfileIntA(RF_SECT, "Command", 1, ini);
    read_prefix(ini, "PatientPrefix", cfg_vpre, &cfg_vlen, 0);
    read_prefix(ini, "UnitPrefix", cfg_upre, &cfg_ulen, 1);
    char ev[CLEN];
    GetPrivateProfileStringA(RF_SECT, "PopupEvent", cfg_event, ev, sizeof ev, ini);
    strcpy(cfg_event, ev);
    cfg_popup_ms = GetPrivateProfileIntA(RF_SECT, "PopupCooldownMs", 3000, ini);
    if (cfg_popup_ms > 600000) cfg_popup_ms = 600000;
}

#ifndef RF_TEST
static void install(int is_server)
{
    const char *err = find_all((uint8_t *)GetModuleHandleW(NULL));
    if (err) { logf_("[GEM ReviveFilter %s] NOT installed: %s", RF_VERSION, err); return; }
    if (!cfg_enabled) { logf_("[GEM ReviveFilter %s] Enabled=0: not installed", RF_VERSION); return; }
    if (is_server && !cfg_server) { logf_("[GEM ReviveFilter %s] Server=0: not installed in the server", RF_VERSION); return; }
    if (!swap_slot(&g_vt_order[19], (void *)rf_exec, (void **)&o_exec)) { logf_("[GEM ReviveFilter %s] NOT installed: vtable write failed (%lu)", RF_VERSION, GetLastError()); return; }
    const char *cmd = "on";
    if (g_apply_err) cmd = g_apply_err;
    else if (!cfg_apply) cmd = "off (Command=0)";
    else if (!swap_slot(&g_vt_act[17], (void *)rf_apply, (void **)&o_apply)) cmd = "vtable write failed";
    char pop[96];
    if (!cfg_event[0]) snprintf(pop, sizeof pop, "off (PopupEvent empty)");
    else if (g_event_err) snprintf(pop, sizeof pop, "OFF: %s", g_event_err);
    else snprintf(pop, sizeof pop, "\"%s\" (cooldown %u ms, fire exe+0x%llX)", cfg_event, cfg_popup_ms, OFF(f_fire));
    logf_("[GEM ReviveFilter %s] installed | order Execute exe+0x%llX (iface %d, patient +0x%X) | command gate: %s | popup: %s | "
          "tags +0x%X def +0x%X props +0x%X | PatientPrefix=%s UnitPrefix=%s Log=%d",
          RF_VERSION, OFF(o_exec), o_iface, o_target, cmd, pop, o_tags, o_def, o_props, cfg_vpre, cfg_upre[0] ? cfg_upre : "(none)", cfg_log);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID res)
{
    (void)res;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        char path[MAX_PATH], ini[MAX_PATH], exe[MAX_PATH], *nm;
        DWORD n = GetModuleFileNameA(inst, path, MAX_PATH);
        if (n == 0 || n >= MAX_PATH - 16) return TRUE;
        char *dot = strrchr(path, '.');
        if (dot) *dot = 0;
        GetModuleFileNameA(NULL, exe, MAX_PATH);
        nm = strrchr(exe, '\\'); nm = nm ? nm + 1 : exe;
        if (_strnicmp(nm, "call_to_arms", 12)) return TRUE;
        int server = strstr(nm, "_server") != NULL;
        snprintf(ini, sizeof ini, "%s.ini", path);
        snprintf(g_logpath, sizeof g_logpath, "%s%s.log", path, strstr(nm, "_ed") ? "_editor" : (server ? "_server" : ""));
        load_ini(ini);
        install(server);
    }
    return TRUE;
}
#else
/* ---------------------------------------------------------------- offline test */
static __attribute__((aligned(16))) uint8_t t_actor[0x2000], t_veh[0x2000], t_def[0x1000], t_atags[4][0x60], t_vtags[4][0x60], t_props[4][0x60];
static void set_tag(uint8_t *t, const char *name)
{
    MsString *s = (MsString *)(t + 0x40);
    memset(t, 0, 0x60);
    s->size = strlen(name);
    if (s->size < 16) { memcpy(s->u.buf, name, s->size + 1); s->cap = 15; }
    else { s->u.ptr = (char *)name; s->cap = 63; }
}
static void set_vec(uint8_t *ent_or_def, uint32_t off, uint8_t (*tags)[0x60], int n)
{
    *(uint8_t **)(ent_or_def + off) = n ? tags[0] : NULL;
    *(uint8_t **)(ent_or_def + off + 8) = n ? tags[0] + 0x60 * n : NULL;
}
static int g_exec_calls, g_apply_calls, g_tail_calls;
static uint64_t fake_exec(void *o, uint64_t ph, uint64_t a, uint64_t b) { (void)o; (void)ph; (void)a; (void)b; g_exec_calls++; return 0; }
static void fake_apply(void *act, uint8_t *actor, void *t, void *a4, uint64_t a5) { (void)act; (void)actor; (void)t; (void)a4; (void)a5; g_apply_calls++; }
static void fake_tail(void *ui, void *act) { (void)ui; (void)act; g_tail_calls++; }
static void *t_ui;
static int g_fire_calls;
static void *fake_tagctor(void *t, const char *n) { (void)n; return t; }
static void *fake_ctxctor(void *b, const void *e) { (void)e; return b; }
static void fake_fire(void *c, const void *t) { (void)c; (void)t; g_fire_calls++; }
static void fake_ctxdtor(void *b) { (void)b; }

int main(int argc, char **argv)
{
    int bad = 0;
    for (int a = 1; a < argc; a++) {
        HMODULE m = LoadLibraryExA(argv[a], NULL, DONT_RESOLVE_DLL_REFERENCES);
        if (!m) { printf("%s: load failed %lu\n", argv[a], GetLastError()); bad = 1; continue; }
        const char *err = find_all((uint8_t *)m);
        if (err) { printf("%s: FAIL %s\n", argv[a], err); bad = 1; continue; }
        printf("%s:\n  order vtable exe+0x%llX, Execute exe+0x%llX, iface %d, target +0x%X\n"
               "  command: %s (Apply exe+0x%llX, patient at target +0x%X, tail exe+0x%llX ui exe+0x%llX)\n"
               "  tags +0x%X, def +0x%X, props +0x%X\n  popup event: %s (tag ctor exe+0x%llX ctx exe+0x%llX fire exe+0x%llX dtor exe+0x%llX)\n",
               argv[a], OFF(g_vt_order), OFF(g_vt_order[19]), o_iface, o_target,
               g_apply_err ? g_apply_err : "found", g_vt_act ? OFF(g_vt_act[17]) : 0ULL, o_cmd_target, g_vt_act && !g_apply_err ? OFF(f_tail) : 0ULL, g_vt_act && !g_apply_err ? OFF(g_tail_ui) : 0ULL,
               o_tags, o_def, o_props, g_event_err ? g_event_err : "found",
               OFF(f_tagctor), OFF(f_ctxctor), OFF(f_fire), OFF(f_ctxdtor));
        int srv = strstr(argv[a], "_server") != NULL;
        if (o_iface != -0xAF0 || o_target != 0x48 || o_tags != 0x4E8 || o_def != 0x548 || o_props != 0x330 || (!srv && (g_apply_err || o_cmd_target != 0x20)) || g_event_err) {
            printf("  FAIL values differ from the ones read by hand on 1.068\n"); bad = 1;
        }
        FreeLibrary(m);
    }
    /* rule + wrappers on fake objects */
    o_tags = 0x4E8; o_def = 0x548; o_props = 0x330; o_iface = -0xAF0; o_target = 0x48; o_cmd_target = 0x20; cfg_log = 1;
    strcpy(cfg_upre, "reviver_"); cfg_ulen = 8;
    f_tagctor = fake_tagctor; f_ctxctor = fake_ctxctor; f_fire = fake_fire; f_ctxdtor = fake_ctxdtor;
    o_exec = fake_exec; o_apply = fake_apply; f_tail = fake_tail; g_tail_ui = &t_ui;
    static uint8_t t_click[0x40];
    *(uint8_t **)(t_click + o_cmd_target) = t_veh;
    *(uint8_t **)(t_veh + o_def) = t_def;
    set_tag(t_vtags[0], "human"); set_tag(t_vtags[1], "revive_by_apothecary");
    set_tag(t_props[0], "soldier"); set_tag(t_props[1], "revive_by_surgeon"); set_tag(t_props[2], "medkit");
    set_tag(t_atags[0], "astartes"); set_tag(t_atags[1], "reviver_apothecary"); set_tag(t_atags[2], "reviver_surgeon");
    set_tag(t_atags[3], "reviver_a_much_longer_class_name_on_the_heap");
    struct { const char *what; int vt, vp, at, want; } st[] = {
        { "unrestricted patient, plain unit"   ,      1, 1, 1, 1 },
        { "tag-restricted, plain unit",            2, 1, 1, 0 },
        { "tag-restricted, apothecary",            2, 1, 2, 1 },
        { "prop-restricted (surgeon), apothecary",1, 2, 2, 0 },
        { "prop-restricted (surgeon), surgeon"  ,  1, 3, 3, 1 },
        { "both, apothecary",                      2, 3, 2, 1 },
        { "restricted, unit with no tags",         2, 1, 0, 0 },
        { "restricted, long heap class only",      2, 1, -1, 0 },
    };
    for (size_t i = 0; i < sizeof st / sizeof *st; i++) {
        set_vec(t_veh, o_tags, t_vtags, st[i].vt);
        set_vec(t_def, o_props, t_props, st[i].vp);
        if (st[i].at >= 0) set_vec(t_actor, o_tags, t_atags, st[i].at);
        else set_vec(t_actor, o_tags, t_atags + 3, 1);
        int r = rf_allowed(t_actor, t_veh, "test");
        printf("  %-42s %s %s\n", st[i].what, r ? "allowed" : "refused", r == st[i].want ? "OK" : "FAIL");
        if (r != st[i].want) bad = 1;
    }
    /* empty unit prefix: patient revive_by_apothecary2, unit tag "apothecary2" among many others */
    {
        static __attribute__((aligned(16))) uint8_t many[40][0x60];
        char nm[40][24];
        for (int i = 0; i < 40; i++) { snprintf(nm[i], sizeof nm[i], "tag_%02d", i); set_tag(many[i], nm[i]); }
        set_tag(many[37], "apothecary2");
        static __attribute__((aligned(16))) uint8_t vt[1][0x60];
        set_tag(vt[0], "revive_by_apothecary2");
        cfg_upre[0] = 0; cfg_ulen = 0;
        set_vec(t_veh, o_tags, vt, 1); set_vec(t_def, o_props, t_props, 0);
        set_vec(t_actor, o_tags, many, 40);
        int r1 = rf_allowed(t_actor, t_veh, "test");
        set_vec(t_actor, o_tags, many, 37);
        int r2 = rf_allowed(t_actor, t_veh, "test");
        set_tag(many[0], "apothecary2_x");
        set_vec(t_actor, o_tags, many, 1);
        int r3 = rf_allowed(t_actor, t_veh, "test");
        int ok = r1 == 1 && r2 == 0 && r3 == 0;
        printf("  %-42s 38th of 40 tags %s, without it %s, prefix-only %s %s\n", "empty UnitPrefix, apothecary2",
               r1 ? "allowed" : "refused", r2 ? "allowed" : "refused", r3 ? "allowed" : "refused", ok ? "OK" : "FAIL");
        if (!ok) bad = 1;
        strcpy(cfg_upre, "reviver_"); cfg_ulen = 8;
    }
    /* Execute wrapper: refused update -> 2 without the original; start / finish always run */
    static uint8_t order[0x100];
    *(uint8_t **)(order + 8) = t_actor - o_iface;
    *(uint8_t **)(order + o_target) = t_veh;
    set_vec(t_veh, o_tags, t_vtags, 2); set_vec(t_def, o_props, t_props, 1); set_vec(t_actor, o_tags, t_atags, 1);
    g_exec_calls = 0;
    uint64_t r0 = rf_exec(order, 0, 0, 0), r1 = rf_exec(order, 1, 0, 0), r2 = rf_exec(order, 2, 0, 0);
    int ok = r0 == 0 && r1 == 2 && r2 == 0 && g_exec_calls == 2;
    printf("  %-42s start %llu update %llu finish %llu, original %d x %s\n", "Execute, refused pair", r0, r1, r2, g_exec_calls, ok ? "OK" : "FAIL");
    if (!ok) bad = 1;
    set_vec(t_actor, o_tags, t_atags, 2); g_exec_calls = 0;
    r1 = rf_exec(order, 1, 0, 0);
    ok = r1 == 0 && g_exec_calls == 1;
    printf("  %-42s update %llu, original %d x %s\n", "Execute, allowed pair", r1, g_exec_calls, ok ? "OK" : "FAIL");
    if (!ok) bad = 1;
    *(uint8_t **)(order + o_target) = NULL; g_exec_calls = 0;
    r1 = rf_exec(order, 1, 0, 0);
    ok = r1 == 0 && g_exec_calls == 1;
    printf("  %-42s update %llu, original %d x %s\n", "Execute, no target (engine decides)", r1, g_exec_calls, ok ? "OK" : "FAIL");
    if (!ok) bad = 1;
    /* Apply wrapper */
    set_vec(t_actor, o_tags, t_atags, 1); g_apply_calls = g_tail_calls = 0;
    g_fire_calls = 0;
    rf_apply(NULL, t_actor, t_click, NULL, 0);
    ok = g_apply_calls == 0 && g_tail_calls == 1 && g_fire_calls == 1;
    printf("  %-42s original %d, tail %d, popup %d %s\n", "Apply, refused pair", g_apply_calls, g_tail_calls, g_fire_calls, ok ? "OK" : "FAIL");
    if (!ok) bad = 1;
    rf_apply(NULL, t_actor, t_click, NULL, 0);
    ok = g_fire_calls == 1 && g_tail_calls == 2;
    printf("  %-42s popup %d (cooldown) %s\n", "Apply, refused again at once", g_fire_calls, ok ? "OK" : "FAIL");
    if (!ok) bad = 1;
    set_vec(t_actor, o_tags, t_atags, 2); g_apply_calls = g_tail_calls = 0;
    rf_apply(NULL, t_actor, t_click, NULL, 0);
    ok = g_apply_calls == 1 && g_tail_calls == 0;
    printf("  %-42s original %d, tail %d %s\n", "Apply, allowed pair", g_apply_calls, g_tail_calls, ok ? "OK" : "FAIL");
    if (!ok) bad = 1;
    printf(bad ? "SOME FAILED\n" : "ALL OK\n");
    return bad;
}
#endif
