/* GEM BoardFilter 0.4 - who may crew/board a vehicle, by entity tags
 * Call to Arms: Gates of Hell (GEM 2, 1.067). Indomitus Launcher payload dll
 * (gem_boardfilter.dll + gem_boardfilter.ini). 0.1 by Alex; 0.2 = launcher adaptation; 0.3 = crew_only_ rule;
 * 0.4 = Alex's 0.1 board_/boarder_ rule back on the 0.2/0.3 code.
 *
 * Rule (runtime tags only, read on every check, so script {tags add/remove} acts at once):
 *   vehicle tag "board_<class>" (any number) -> humans of one of those classes may board it
 *   vehicle without a board_ tag             -> classes from ini DefaultBoard (default "mortal"; empty = anyone)
 *   vehicle tag "no_board"                   -> nobody may board it
 *   human tag "boarder_<class>" (any number) -> the human's classes (breed {tags} via GEM BreedTags, {tags add})
 *   human without a boarder_ tag             -> classes from ini DefaultClass (default "mortal")
 *   Exact, case-sensitive match, like the engine's own tag checks. Non-humans are never filtered.
 *   The mod turns vehicle def props board_<class> / no_board into these tags on spawn (40k_vehicle_main.inc).
 *
 * Engine (game exe offsets, found by pattern at runtime, same code in editor/server):
 *   core  exe+0x7FA9D0 CanCrew(vehicle, playerByte*): vehicle script tag "abandoned" -> false,
 *         placer check, owner/relation + ables any_crew/ally_crew. Reads vehicle tags at +0x4E8.
 *   A     exe+0x7FA7E0 (vehicle, human): human player byte -> core -> placer slot. Called twice by
 *         eOrderBoard (order start, and again on arrival); false = order fails "Unable to board".
 *   B     exe+0x7FA9A0 (vehicle, entity): used by the vehicle-state loop over its places
 *         (place stride 0x298, occupant +0x280); false = the engine emits that occupant
 *         ("emit_wrong_crew").
 * This dll replaces the first bytes of A and B with a jump to a stub that asks bf_allow() first;
 * a refusal returns false, otherwise the original code runs unchanged.
 *
 * 0.4 (2026-11-06): the board_/boarder_/no_board/DefaultBoard/DefaultClass rule is back (Alex: the mod's 151 files
 *   use it); crew_only_ removed. Finders, stubs, popup event, lock and def-prop offsets unchanged from 0.3.
 *
 * 0.3 (2026-11-06): board_/boarder_/no_board/DefaultBoard/DefaultClass replaced by the crew_only_<tag> prop rule
 *   above (user's design). Def props offsets voted from the engine's own def-props loads (def +0x548, props +0x330).
 *
 * 0.2 (Indomitus Launcher adaptation, 2026-11-06):
 *   - only acts inside call_to_arms*.exe (game / editor / server), one copy per process
 *     (mutex Local\gem_boardfilter_<pid>), pins itself once patched (must never unload)
 *   - unicode ini / log paths (the payload lives under %LocalAppData%)
 *   - fixed a stack overflow in the refusal log line and the install line (many / long classes)
 *   - refusal bookkeeping (counters, popup pair table, log) under an SRW lock; the script event
 *     is fired outside the lock, so a handler that re-enters a board check cannot deadlock
 *   - stub page made execute-read after writing; ini values clamped
 *
 * Build: build.bat (folder root)   Offline finder + rule + stub test: build.bat test
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <wchar.h>

#define BF_VERSION "0.4"
#define MAXC 32         /* classes per vehicle / per unit that are honoured */
#define CLEN 64

/* ---------------------------------------------------------------- config */
static int  cfg_enabled = 1, cfg_eject = 1, cfg_log = 1;
static char cfg_vpre[CLEN] = "board_", cfg_upre[CLEN] = "boarder_", cfg_noboard[CLEN] = "no_board";
static char cfg_defboard[256] = "mortal", cfg_defclass[256] = "mortal";
static uint32_t cfg_tagoff = 0x4E8;     /* runtime tags; replaced by the offset read from the engine's own code */
static uint32_t cfg_defoff = 0x548;     /* entity -> def; voted from the engine's def-props loads */
static uint32_t cfg_propoff = 0x330;    /* def -> {props} Tag vector */
static uint8_t  cfg_humanbit = 0x04;    /* entity byte +0x30: 2 = actor, 4 = human */

/* ---------------------------------------------------------------- log */
static wchar_t g_logpath[MAX_PATH];
static FILE   *g_log;
static int     g_lines;
static void logf_(const char *fmt, ...)
{
    if (!g_log) {
        if (!g_logpath[0]) return;
        g_log = _wfopen(g_logpath, L"a");
        if (!g_log) return;
    }
    va_list ap; va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

/* bounded append: never writes past cap, always terminated */
static void app(char *dst, size_t cap, const char *s)
{
    size_t n = strlen(dst);
    if (n + 1 >= cap) return;
    snprintf(dst + n, cap - n, "%s", s);
}

/* ---------------------------------------------------------------- lock */
/* Taken only on a refusal (counters, pair table, log) and around the lazy Tag ctor.
 * Never held while engine code that could re-enter a board check runs. */
static SRWLOCK g_lock = SRWLOCK_INIT;

/* ---------------------------------------------------------------- tags */

/* MSVC std::string at p: buf/ptr +0, size +0x10, cap +0x18 */
static const char *str_get(const uint8_t *p, size_t *n)
{
    uint64_t size = *(const uint64_t *)(p + 0x10);
    uint64_t cap  = *(const uint64_t *)(p + 0x18);
    if (cap < size || cap > 0x10000 || size == 0) return NULL;
    *n = (size_t)size;
    return cap > 15 ? *(const char *const *)p : (const char *)p;
}

/* calls fn(name, len, ctx) per Tag of a vector<Tag> at vec (0x60-byte Tags, name std::string at +0x40);
 * returns 0 on a broken vector */
typedef int (*tagfn)(const char *, size_t, void *);     /* return 1 = stop */
static int each_tag_vec(const uint8_t *vec, tagfn fn, void *ctx)
{
    const uint8_t *b = *(const uint8_t *const *)vec;
    const uint8_t *e = *(const uint8_t *const *)(vec + 8);
    if (!b || !e) return 1;                     /* no tags */
    if (e < b) return 0;
    uintptr_t len = (uintptr_t)(e - b);
    if (len % 0x60 || len > 0x60 * 1024) return 0;
    for (const uint8_t *t = b; t < e; t += 0x60) {
        size_t n; const char *s = str_get(t + 0x40, &n);
        if (s && fn(s, n, ctx)) break;
    }
    return 1;
}
/* a set of classes: the <class> part of board_<class> / boarder_<class> tags, or an ini default list */
typedef struct { char cls[MAXC][CLEN]; uint8_t len[MAXC]; int n, dropped; } cls_t;
static void cls_add(cls_t *c, const char *s, size_t k)
{
    if (!k) return;                                             /* bare prefix: no class */
    if (k >= CLEN || c->n >= MAXC) { c->dropped++; return; }
    memcpy(c->cls[c->n], s, k);
    c->cls[c->n][k] = 0;
    c->len[c->n] = (uint8_t)k;
    c->n++;
}
/* "mortal, guard" -> {mortal, guard}; empty list -> empty set */
static void cls_from_list(cls_t *c, const char *list)
{
    c->n = c->dropped = 0;
    for (const char *p = list; *p; ) {
        while (*p == ' ' || *p == ',' || *p == '\t') p++;
        const char *b = p;
        while (*p && *p != ',') p++;
        const char *e = p;
        while (e > b && (e[-1] == ' ' || e[-1] == '\t')) e--;
        cls_add(c, b, (size_t)(e - b));
    }
}
static int cls_meet(const cls_t *a, const cls_t *b)
{
    for (int i = 0; i < a->n; i++)
        for (int j = 0; j < b->n; j++)
            if (a->len[i] == b->len[j] && !memcmp(a->cls[i], b->cls[j], a->len[i])) return 1;
    return 0;
}
static cls_t g_defboard, g_defclass;    /* parsed from the ini at load */

/* one entity's runtime tags: classes after the prefix, plus the no_board tag (vehicles) */
typedef struct { const char *pre; size_t plen; const char *nob; size_t nlen; cls_t *c; int noboard; } scan_t;
static int scan_tags(const char *s, size_t n, void *vp)
{
    scan_t *t = (scan_t *)vp;
    if (t->nob && n == t->nlen && !memcmp(s, t->nob, n)) { t->noboard = 1; return 0; }
    if (n >= t->plen && !memcmp(s, t->pre, t->plen)) cls_add(t->c, s + t->plen, n - t->plen);
    return 0;
}

static void cls_names(const cls_t *c, char *buf, size_t cap)
{
    for (int i = 0; i < c->n; i++) { app(buf, cap, c->cls[i]); app(buf, cap, " "); }
}

/* human tag list for the log line */
typedef struct { char *buf; size_t cap; } names_t;
static int name_app(const char *s, size_t n, void *vp)
{
    names_t *w = (names_t *)vp;
    char tmp[CLEN];
    size_t k = n < CLEN - 1 ? n : CLEN - 1;
    memcpy(tmp, s, k); tmp[k] = 0;
    app(w->buf, w->cap, tmp); app(w->buf, w->cap, " ");
    return 0;
}

static uint32_t st_deny_order, st_deny_seat;

/* ---------------------------------------------------------------- refusal event */
/* On an order refusal the dll fires the script event <PopupEvent> (default "board_refused") on the vehicle,
 * the way the engine fires "throw_off_end": Tag ctor once, then ctx ctor(buf, entity) / fire(ctx, &tag) / ctx dtor(buf).
 * Throttled per unit+vehicle pair on the engine's sim clock (same on every machine -> lockstep-safe). */
typedef void *(*tagctor_t)(void *tag, const char *name);
typedef void *(*ctxctor_t)(void *buf, const void *ent);
typedef void  (*fire_t)(void *ctx, const void *tag);
typedef void  (*ctxdtor_t)(void *buf);
static tagctor_t g_tagctor; static ctxctor_t g_ctxctor; static fire_t g_fire; static ctxdtor_t g_ctxdtor;
static volatile uint32_t *g_clock;
static char cfg_event[CLEN] = "board_refused";
static uint32_t cfg_popup_ms = 3000;
static __attribute__((aligned(16))) uint8_t g_tag[0x80];   /* engine Tag is 0x60 bytes */
static volatile int g_tag_ready;

/* The table is filled in refusal order and recycled round-robin, never by pointer hash, so the
 * popup decision depends only on the (lockstep) sequence of refusals - the same on every machine. */
typedef struct { const void *u, *v; uint32_t last_ms, repeats; } pair_t;
static pair_t g_pairs[64];
static uint32_t g_pair_next;

/* returns 1 when the popup should fire; counts back-to-back refusals of the same pair (AI retry diagnostic) */
static int pair_note(const void *u, const void *v, uint32_t now, uint32_t *repeats)
{
    for (int i = 0; i < 64; i++)
        if (g_pairs[i].u == u && g_pairs[i].v == v) {
            pair_t *p = &g_pairs[i];
            int fire = (uint32_t)(now - p->last_ms) >= cfg_popup_ms;
            p->repeats = (uint32_t)(now - p->last_ms) < 10000 ? p->repeats + 1 : 0;
            if (fire) p->last_ms = now;
            *repeats = p->repeats;
            return fire;
        }
    pair_t *p = &g_pairs[g_pair_next++ & 63];
    p->u = u; p->v = v; p->last_ms = now; p->repeats = 0;
    *repeats = 0;
    return 1;
}

static void fire_refused(const uint8_t *veh)
{
    if (!g_fire || !cfg_event[0]) return;
    /* lazy: the engine's Tag statics are not ready yet when the dll is injected before start */
    if (!g_tag_ready) {
        AcquireSRWLockExclusive(&g_lock);
        if (!g_tag_ready) { g_tagctor(g_tag, cfg_event); g_tag_ready = 1; }
        ReleaseSRWLockExclusive(&g_lock);
    }
    __attribute__((aligned(16))) uint8_t buf[0x400];   /* engine context is >= 0x18C bytes */
    void *ctx = g_ctxctor(buf, veh);
    g_fire(ctx, g_tag);
    g_ctxdtor(buf);
}

/* 1 = allowed. Only ever refuses; anything unexpected -> allowed (the engine decides). */
static int bf_allow(const uint8_t *veh, const uint8_t *unit, int seat)
{
    if (!cfg_enabled || !veh || !unit) return 1;
    if (!(unit[0x30] & cfg_humanbit)) return 1;
    cls_t vc, uc; vc.n = vc.dropped = uc.n = uc.dropped = 0;
    scan_t vs = { cfg_vpre, strlen(cfg_vpre), cfg_noboard[0] ? cfg_noboard : NULL, strlen(cfg_noboard), &vc, 0 };
    if (!each_tag_vec(veh + cfg_tagoff, scan_tags, &vs)) return 1;
    const cls_t *vset = &vc;
    if (!vs.noboard && vc.n == 0) {
        if (g_defboard.n == 0) return 1;                        /* no board_ tag, empty DefaultBoard: anyone */
        vset = &g_defboard;
    }
    scan_t us = { cfg_upre, strlen(cfg_upre), NULL, 0, &uc, 0 };
    if (!each_tag_vec(unit + cfg_tagoff, scan_tags, &us)) return 1;
    const cls_t *uset = uc.n ? &uc : &g_defclass;
    if (!vs.noboard && cls_meet(vset, uset)) return 1;

    /* refused */
    int popup = 0;
    AcquireSRWLockExclusive(&g_lock);
    if (seat) st_deny_seat++; else st_deny_order++;
    uint32_t repeats = 0;
    if (!seat) popup = pair_note(unit, veh, g_clock ? *g_clock : 0, &repeats);
    if (!seat && repeats) {   /* the same unit refused at the same vehicle again within 10 s (one click = 2 checks) */
        if (cfg_log && (repeats == 4 || repeats == 20 || repeats == 100 || repeats % 1000 == 0))
            logf_("order retry: unit %p -> vehicle %p refused %u times in a row (AI retry loop?)", (void *)unit, (void *)veh, repeats + 1);
    } else if (cfg_log && g_lines < 400) {
        g_lines++;
        char vl[256] = "", ucl[256] = "", ul[256] = "";
        if (vs.noboard) app(vl, sizeof vl, "no_board ");
        else { cls_names(vset, vl, sizeof vl); if (vset == &g_defboard) app(vl, sizeof vl, "(default) "); }
        cls_names(uset, ucl, sizeof ucl);
        if (uset == &g_defclass) app(ucl, sizeof ucl, "(default) ");
        names_t w = { ul, sizeof ul };
        each_tag_vec(unit + cfg_tagoff, name_app, &w);
        logf_("%s: unit %p class [%s] tags [%s] -> vehicle %p board [%s%s] refused (orders %u, seats %u)",
              seat ? "seat" : "order", (void *)unit, ucl, ul, (void *)veh, vl,
              (vc.dropped || uc.dropped) ? "+more ignored " : "", st_deny_order, st_deny_seat);
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (popup) fire_refused(veh);   /* outside the lock: script handlers may re-enter */
    return 0;
}

__attribute__((used, ms_abi)) int bf_allow_order(const uint8_t *veh, const uint8_t *unit) { return bf_allow(veh, unit, 0); }
__attribute__((used, ms_abi)) int bf_allow_seat(const uint8_t *veh, const uint8_t *unit)  { return cfg_eject ? bf_allow(veh, unit, 1) : 1; }

/* ---------------------------------------------------------------- finders */
#define W 0x100
/* A: push rbx; sub rsp,20h; mov rbx,rcx; test rdx,rdx; je r8; movzx eax,byte[rdx+d32]; lea rdx,[rsp+38h];
 *    mov [rsp+38h],al; call core; test al,al; je r8; mov rcx,[rbx+d32]; mov rax,[rcx]; add rsp,20h; pop rbx; jmp [rax+d32] */
static const uint16_t SIG_A[] = {
    0x40,0x53, 0x48,0x83,0xEC,0x20, 0x48,0x8B,0xD9, 0x48,0x85,0xD2, 0x74,W, 0x0F,0xB6,0x82,W,W,W,W,
    0x48,0x8D,0x54,0x24,0x38, 0x88,0x44,0x24,0x38, 0xE8,W,W,W,W, 0x84,0xC0, 0x74,W, 0x48,0x8B,0x8B,W,W,W,W,
    0x48,0x8B,0x01, 0x48,0x83,0xC4,0x20, 0x5B, 0x48,0xFF,0xA0,W,W,W,W
};
#define A_JE 12        /* je rel8 opcode offset */
#define A_PATCH 14     /* bytes replaced; resume at the movzx */
#define A_CALL 30
/* B: sub rsp,28h; test rdx,rdx; je r8; movzx eax,byte[rdx+d32]; lea rdx,[rsp+38h]; mov [rsp+38h],al; call core; add rsp,28h; ret */
static const uint16_t SIG_B[] = {
    0x48,0x83,0xEC,0x28, 0x48,0x85,0xD2, 0x74,W, 0x0F,0xB6,0x82,W,W,W,W,
    0x48,0x8D,0x54,0x24,0x38, 0x88,0x44,0x24,0x38, 0xE8,W,W,W,W, 0x48,0x83,0xC4,0x28, 0xC3
};
#define B_JE 7
#define B_PATCH 16     /* sub, test, je, movzx; resume at the lea */
#define B_CALL 25
/* core: ... mov rsi,[rcx+d32]; mov rbx,[rcx+d32] (tag vector end/begin) */
static const uint16_t SIG_CORE_TAGS[] = { 0x48,0x8B,0xB1,W,W,W,W, 0x48,0x8B,0x99,W,W,W,W };

static int text_range(uint8_t *base, uint8_t **lo, uint8_t **hi)
{
    IMAGE_DOS_HEADER *d = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(base + d->e_lfanew);
    IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, s++)
        if (!memcmp(s->Name, ".text", 5)) {
            *lo = base + s->VirtualAddress;
            *hi = *lo + s->Misc.VirtualSize;
            return 1;
        }
    return 0;
}

static int match(const uint8_t *p, const uint16_t *sig, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (sig[i] != W && p[i] != sig[i]) return 0;
    return 1;
}

static uint8_t *find_unique(uint8_t *lo, uint8_t *hi, const uint16_t *sig, size_t n, int *count)
{
    uint8_t *hit = NULL; *count = 0;
    for (uint8_t *p = lo; p + n <= hi; p++)
        if (match(p, sig, n)) { if (!hit) hit = p; (*count)++; }
    return hit;
}

static uint8_t *rel32(uint8_t *at) { return at + 4 + *(int32_t *)at; }

typedef struct { uint8_t *a, *b, *core; uint32_t tagoff, defoff, propoff; int prop_votes;
                 uint8_t *tagctor, *ctxctor, *fire, *ctxdtor; uint8_t *clock; const char *ev_err; } sites_t;

/* walk one expected instruction at p, skipping single-byte nops; returns the next position or NULL */
static uint8_t *expect(uint8_t *p, const uint16_t *sig, size_t n)
{
    while (*p == 0x90) p++;
    return match(p, sig, n) ? p + n : NULL;
}

/* event fire: the engine's "throw_off_end" site:
 *   lea rdx,"throw_off_end"; lea rcx,tag; call TagCtor; ... mov rdx,rXX; lea rcx,[rsp+x]; call CtxCtor;
 *   lea rdx,tag; mov rcx,rax; call Fire; lea rcx,[rsp+x]; call CtxDtor */
static const char *find_event(uint8_t *base, uint8_t *lo, uint8_t *hi, sites_t *s)
{
    IMAGE_DOS_HEADER *d = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(base + d->e_lfanew);
    uint8_t *img_end = base + nt->OptionalHeader.SizeOfImage;
    static const char name[] = "\0throw_off_end";
    uint8_t *str = NULL;
    for (uint8_t *p = hi; p + sizeof name <= img_end; p++)
        if (!memcmp(p, name, sizeof name)) { str = p + 1; break; }
    if (!str) return "throw_off_end string not found";
    static const uint16_t LEA_RDX[] = { 0x48, 0x8D, 0x15 }, LEA_RCX_RIP[] = { 0x48, 0x8D, 0x0D }, CALL[] = { 0xE8 };
    static const uint16_t MOV_RDX_R[] = { 0x48, 0x8B, W }, LEA_RCX_RSP[] = { 0x48, 0x8D, 0x4C, 0x24, W }, MOV_RCX_RAX[] = { 0x48, 0x8B, 0xC8 };
    for (uint8_t *p = lo; p + 7 <= hi; p++) {
        if (p[0] != 0x48 || p[1] != 0x8D || p[2] != 0x15 || rel32(p + 3) != str) continue;
        uint8_t *q = p + 7;
        if (!(q = expect(q, LEA_RCX_RIP, 3))) continue;
        uint8_t *tag = rel32(q); q += 4;
        if (!(q = expect(q, CALL, 1))) continue;
        uint8_t *tagctor = rel32(q); q += 4;
        /* scan forward for the context sequence */
        for (uint8_t *r = q; r < q + 0x40; r++) {
            uint8_t *x = r;
            if (!match(x, MOV_RDX_R, 3) || (x[2] & 0xF8) != 0xD0) continue;
            x += 3;
            if (!(x = expect(x, LEA_RCX_RSP, 5)) || !(x = expect(x, CALL, 1))) continue;
            uint8_t *ctx = rel32(x); x += 4;
            if (!(x = expect(x, LEA_RDX, 3)) || rel32(x) != tag) continue;
            x += 4;
            if (!(x = expect(x, MOV_RCX_RAX, 3)) || !(x = expect(x, CALL, 1))) continue;
            uint8_t *fire = rel32(x); x += 4;
            if (!(x = expect(x, LEA_RCX_RSP, 5)) || !(x = expect(x, CALL, 1))) continue;
            uint8_t *dtor = rel32(x);
            if (tagctor < lo || ctx < lo || fire < lo || dtor < lo || tagctor >= hi || ctx >= hi || fire >= hi || dtor >= hi) continue;
            s->tagctor = tagctor; s->ctxctor = ctx; s->fire = fire; s->ctxdtor = dtor;
            return NULL;
        }
    }
    return "event fire sequence not found";
}

/* sim clock: brain timers `mov ebx,[rdi+X]; call GetClock; cmp eax,ebx; jb` with GetClock = `mov eax,[rip+g]; ret`;
 * take the g most sites agree on */
static uint8_t *find_clock(uint8_t *lo, uint8_t *hi)
{
    static const uint16_t SIG_CLK[] = { 0x8B, 0x9F, W, W, 0x00, 0x00, 0xE8, W, W, W, W, 0x3B, 0xC3, 0x0F, 0x82 };
    uint8_t *cand[8] = { 0 }; int cnt[8] = { 0 }, nc = 0;
    for (uint8_t *p = lo; p + sizeof SIG_CLK / 2 <= hi; p++) {
        if (!match(p, SIG_CLK, sizeof SIG_CLK / 2)) continue;
        uint8_t *t = rel32(p + 7);
        if (t < lo || t + 7 > hi || t[0] != 0x8B || t[1] != 0x05 || t[6] != 0xC3) continue;
        uint8_t *g = rel32(t + 2);
        int i = 0;
        while (i < nc && cand[i] != g) i++;
        if (i == nc) { if (nc == 8) continue; cand[nc++] = g; }
        cnt[i]++;
    }
    int best = -1;
    for (int i = 0; i < nc; i++) if (best < 0 || cnt[i] > cnt[best]) best = i;
    return (best >= 0 && cnt[best] >= 5) ? cand[best] : NULL;
}

/* def props: the engine loads them in ~70-90 places as
 *   mov rD,[rX+DEF]; mov rE,[rD+PROPS+8]; mov rB,[rD+PROPS]; cmp ...   (end, then begin)
 * take the (DEF, PROPS) pair most sites agree on; the runner-up is the FSM state vector (~21 sites) */
static int find_props(uint8_t *lo, uint8_t *hi, uint32_t *defoff, uint32_t *propoff, int *votes)
{
    uint32_t ka[8], kb[8]; int cnt[8] = { 0 }, nc = 0;
    for (uint8_t *p = lo; p + 24 <= hi; p++) {
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
    *votes = best >= 0 ? cnt[best] : 0;
    if (best < 0 || cnt[best] < 20 || cnt[best] < 2 * second) return 0;
    *defoff = ka[best]; *propoff = kb[best];
    return 1;
}

static const char *find_all(uint8_t *base, sites_t *s)
{
    uint8_t *lo, *hi;
    int na, nb, nt;
    memset(s, 0, sizeof *s);
    if (!text_range(base, &lo, &hi)) return "no .text section";
    s->a = find_unique(lo, hi, SIG_A, sizeof SIG_A / 2, &na);
    if (na == 0) return "board-order check not found (unknown exe build, or already patched by another dll)";
    if (na > 1) return "board-order check not unique";
    s->b = find_unique(lo, hi, SIG_B, sizeof SIG_B / 2, &nb);
    if (nb == 0) return "seat check not found (unknown exe build, or already patched by another dll)";
    if (nb > 1) return "seat check not unique";
    s->core = rel32(s->a + A_CALL + 1);
    if (rel32(s->b + B_CALL + 1) != s->core) return "the two checks call different functions";
    uint8_t *t = find_unique(s->core, s->core + 0x40, SIG_CORE_TAGS, sizeof SIG_CORE_TAGS / 2, &nt);
    if (nt != 1) return "tag vector offset not found in the crew check";
    uint32_t end = *(uint32_t *)(t + 3), beg = *(uint32_t *)(t + 10);
    if (end != beg + 8 || beg < 0x100 || beg > 0x2000) return "unexpected tag vector offset";
    s->tagoff = beg;
    /* 0.4 reads runtime tags only; the def props offsets are just logged (and used by the offline test's fakes) */
    if (!find_props(lo, hi, &s->defoff, &s->propoff, &s->prop_votes)) { s->defoff = 0x548; s->propoff = 0x330; s->prop_votes = 0; }
    /* optional: popup event + clock (the filter works without them) */
    s->ev_err = find_event(base, lo, hi, s);
    s->clock = find_clock(lo, hi);
    if (!s->ev_err && !s->clock) s->ev_err = "sim clock not found";
    return NULL;
}

/* ---------------------------------------------------------------- stubs */
static uint8_t *emit(uint8_t *p, const void *b, size_t n) { memcpy(p, b, n); return p + n; }
static uint8_t *emit_absjmp(uint8_t *p, const void *to)
{
    static const uint8_t j[6] = { 0xFF, 0x25, 0, 0, 0, 0 };
    p = emit(p, j, 6);
    uint64_t t = (uint64_t)(uintptr_t)to;
    return emit(p, &t, 8);
}
/* stub: push rcx; push rdx; sub rsp,28h; mov rax,fn; call rax; add rsp,28h; pop rdx; pop rcx;
 *       test eax,eax; jnz go; xor eax,eax; ret; go: <original bytes before je> ; jnz +14 ; jmp fail ;
 *       <original bytes after je up to patch end> ; jmp resume
 * At entry rsp = 8 mod 16; two pushes + 0x28 make the call 16-aligned with 0x20 shadow space. */
static uint8_t *build_stub(uint8_t *p, void *fn, const uint8_t *site, int je_off, int patch_len)
{
    static const uint8_t pre[] = { 0x51, 0x52, 0x48, 0x83, 0xEC, 0x28, 0x48, 0xB8 };
    static const uint8_t mid[] = { 0xFF, 0xD0, 0x48, 0x83, 0xC4, 0x28, 0x5A, 0x59, 0x85, 0xC0, 0x75, 0x03, 0x31, 0xC0, 0xC3 };
    p = emit(p, pre, sizeof pre);
    uint64_t f = (uint64_t)(uintptr_t)fn;
    p = emit(p, &f, 8);
    p = emit(p, mid, sizeof mid);
    p = emit(p, site, je_off);                                   /* replay up to the je */
    const uint8_t *fail = site + je_off + 2 + (int8_t)site[je_off + 1];
    static const uint8_t jnz14[] = { 0x75, 0x0E };
    p = emit(p, jnz14, 2);
    p = emit_absjmp(p, fail);
    p = emit(p, site + je_off + 2, patch_len - (je_off + 2));     /* replay after the je */
    p = emit_absjmp(p, site + patch_len);
    return p;
}

#ifndef BF_TEST
/* ---------------------------------------------------------------- ini */
static wchar_t g_ini[MAX_PATH];

static int ini_i(const wchar_t *key, int def, int lo, int hi)
{
    int v = (int)GetPrivateProfileIntW(L"BoardFilter", key, def, g_ini);
    return v < lo ? lo : v > hi ? hi : v;
}

/* UTF-8 result; a value that does not fit falls back to the default */
static void ini_s(const wchar_t *key, const char *def, char *out, int cap)
{
    wchar_t wdef[64], buf[512];
    if (!MultiByteToWideChar(CP_UTF8, 0, def, -1, wdef, 64)) wdef[0] = 0;
    GetPrivateProfileStringW(L"BoardFilter", key, wdef, buf, 512, g_ini);
    if (!WideCharToMultiByte(CP_UTF8, 0, buf, -1, out, cap, NULL, NULL)) snprintf(out, (size_t)cap, "%s", def);
    out[cap - 1] = 0;
}

static void load_ini(void)
{
    cfg_enabled = ini_i(L"Enabled", 1, 0, 1);
    cfg_eject   = ini_i(L"EjectOccupants", 1, 0, 1);
    cfg_log     = ini_i(L"Log", 1, 0, 1);
    ini_s(L"VehiclePrefix", "board_", cfg_vpre, sizeof cfg_vpre);
    if (!cfg_vpre[0]) snprintf(cfg_vpre, sizeof cfg_vpre, "board_");     /* an empty prefix would match every tag */
    ini_s(L"UnitPrefix", "boarder_", cfg_upre, sizeof cfg_upre);
    if (!cfg_upre[0]) snprintf(cfg_upre, sizeof cfg_upre, "boarder_");
    ini_s(L"NoBoardTag", "no_board", cfg_noboard, sizeof cfg_noboard);  /* empty = no such tag */
    ini_s(L"DefaultBoard", "mortal", cfg_defboard, sizeof cfg_defboard);
    ini_s(L"DefaultClass", "mortal", cfg_defclass, sizeof cfg_defclass);
    cls_from_list(&g_defboard, cfg_defboard);
    cls_from_list(&g_defclass, cfg_defclass);
    ini_s(L"PopupEvent", "board_refused", cfg_event, sizeof cfg_event);
    cfg_popup_ms = (uint32_t)ini_i(L"PopupCooldownMs", 3000, 0, 600000);
}

/* ---------------------------------------------------------------- install */
static int write_code(uint8_t *at, const uint8_t *bytes, size_t n)
{
    DWORD old;
    if (!VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &old)) return 0;
    memcpy(at, bytes, n);
    VirtualProtect(at, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, n);
    return 1;
}

static void install(const char *exe_name)
{
    uint8_t *base = (uint8_t *)GetModuleHandleW(NULL);
    sites_t s;
    logf_("[GEM BoardFilter %s] %s | Enabled=%d VehiclePrefix=%s UnitPrefix=%s NoBoardTag=%s DefaultBoard=%s "
          "DefaultClass=%s EjectOccupants=%d PopupEvent=%s PopupCooldownMs=%u Log=%d",
          BF_VERSION, exe_name, cfg_enabled, cfg_vpre, cfg_upre, cfg_noboard[0] ? cfg_noboard : "(none)",
          cfg_defboard[0] ? cfg_defboard : "(anyone)", cfg_defclass, cfg_eject,
          cfg_event[0] ? cfg_event : "(none)", cfg_popup_ms, cfg_log);
    if (!cfg_enabled) { logf_("[GEM BoardFilter] Enabled=0, nothing patched (vanilla boarding)"); return; }
    const char *err = find_all(base, &s);
    if (err) { logf_("[GEM BoardFilter] NOT installed: %s - nothing patched", err); return; }
    cfg_tagoff = s.tagoff; cfg_defoff = s.defoff; cfg_propoff = s.propoff;
    if (!s.ev_err) {
        g_tagctor = (tagctor_t)s.tagctor; g_ctxctor = (ctxctor_t)s.ctxctor;
        g_fire = (fire_t)s.fire; g_ctxdtor = (ctxdtor_t)s.ctxdtor;
        g_clock = (volatile uint32_t *)s.clock;
    }

    uint8_t *mem = (uint8_t *)VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!mem) { logf_("[GEM BoardFilter] NOT installed: VirtualAlloc failed (%lu)", GetLastError()); return; }
    uint8_t *stub_a = mem, *stub_b = mem + 256;
    build_stub(stub_a, (void *)bf_allow_order, s.a, A_JE, A_PATCH);
    build_stub(stub_b, (void *)bf_allow_seat, s.b, B_JE, B_PATCH);
    DWORD old;
    if (!VirtualProtect(mem, 4096, PAGE_EXECUTE_READ, &old)) {
        logf_("[GEM BoardFilter] NOT installed: VirtualProtect (stubs) failed (%lu)", GetLastError());
        VirtualFree(mem, 0, MEM_RELEASE);
        return;
    }
    FlushInstructionCache(GetCurrentProcess(), mem, 512);

    uint8_t pa[A_PATCH], pb[B_PATCH];
    memset(pa, 0x90, sizeof pa); memset(pb, 0x90, sizeof pb);
    emit_absjmp(pa, stub_a);
    emit_absjmp(pb, stub_b);
    if (!write_code(s.a, pa, sizeof pa)) {
        logf_("[GEM BoardFilter] NOT installed: VirtualProtect (order check) failed (%lu)", GetLastError());
        return;
    }
    int seat_ok = write_code(s.b, pb, sizeof pb);

    HMODULE self;
    int pinned = GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                                    (LPCWSTR)(void *)&bf_allow_order, &self) != 0;
    logf_("[GEM BoardFilter] installed: order check exe+0x%llX, seat check exe+0x%llX%s, crew core exe+0x%llX, tags +0x%X, "
          "def +0x%X props +0x%X (%d sites)%s",
          (unsigned long long)(s.a - base), (unsigned long long)(s.b - base), seat_ok ? "" : " (PATCH FAILED - no eject)",
          (unsigned long long)(s.core - base), cfg_tagoff, cfg_defoff, cfg_propoff, s.prop_votes,
          pinned ? "" : " (could not pin the dll)");
    if (s.ev_err) logf_("  popup event OFF: %s", s.ev_err);
    else logf_("  popup event \"%s\" on the vehicle (cooldown %u ms): fire exe+0x%llX, clock exe+0x%llX", cfg_event, cfg_popup_ms,
               (unsigned long long)(s.fire - base), (unsigned long long)(s.clock - base));
    logf_("[GEM BoardFilter] simulation change: every player in a multiplayer match needs this dll with the same ini");
}

static int is_game_exe(const wchar_t *nm)
{
    if (_wcsnicmp(nm, L"call_to_arms", 12)) return 0;
    if (_wcsnicmp(nm, L"call_to_arms_workshop", 21) == 0) return 0;
    return 1;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID res)
{
    (void)res;
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(inst);

    wchar_t exe[MAX_PATH], *nm;
    DWORD ne = GetModuleFileNameW(NULL, exe, MAX_PATH);
    if (ne == 0 || ne >= MAX_PATH) return TRUE;
    nm = wcsrchr(exe, L'\\'); nm = nm ? nm + 1 : exe;
    if (!is_game_exe(nm)) return TRUE;          /* not GoH: stay inert */

    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(inst, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH - 16) return TRUE;
    wchar_t *dot = wcsrchr(path, L'.');
    wchar_t *sl = wcsrchr(path, L'\\');
    if (dot && (!sl || dot > sl)) *dot = 0;
    const wchar_t *suffix = wcsstr(nm, L"_ed") ? L"_editor" : (wcsstr(nm, L"_server") ? L"_server" : L"");
    swprintf(g_ini, MAX_PATH, L"%ls.ini", path);
    swprintf(g_logpath, MAX_PATH, L"%ls%ls.log", path, suffix);

    char exe_a[MAX_PATH];
    if (!WideCharToMultiByte(CP_UTF8, 0, nm, -1, exe_a, sizeof exe_a, NULL, NULL)) snprintf(exe_a, sizeof exe_a, "?");

    /* one copy per process (e.g. the payload copy plus a gem_inject test copy) */
    wchar_t mx[64];
    swprintf(mx, 64, L"Local\\gem_boardfilter_%lu", GetCurrentProcessId());
    HANDLE h = CreateMutexW(NULL, FALSE, mx);   /* kept open for the process lifetime */
    if (h && GetLastError() == ERROR_ALREADY_EXISTS) {
        logf_("[GEM BoardFilter %s] %s: another copy is already loaded in this process - this one stays off",
              BF_VERSION, exe_a);
        CloseHandle(h);
        return TRUE;
    }

    load_ini();
    install(exe_a);
    return TRUE;
}
#else
/* ---------------------------------------------------------------- offline test */
/* build.bat test -> bf_test.exe <exe>... (Windows), test/run_host_test.sh (Linux, exe image mapped by hand).
 * 1. finders on each exe; 2. the stubs are built against the mapped image and RUN through the real engine
 *    code for every path (refusal, pass-through to the real crew check, the original NULL-unit branch);
 * 3. the patched prologue is not found again; 4. the 14 rule cases; 5. popup throttle. */
#ifdef _WIN32
static uint8_t *map_image(const char *path)
{
    HMODULE m = LoadLibraryExA(path, NULL, DONT_RESOLVE_DLL_REFERENCES);
    if (!m) { printf("%s: load failed %lu\n", path, GetLastError()); return NULL; }
    /* the "after patch" check writes fake jumps into .text: make the whole image writable, as host_map.c does */
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)((uint8_t *)m + ((IMAGE_DOS_HEADER *)m)->e_lfanew);
    DWORD old;
    if (!VirtualProtect(m, nt->OptionalHeader.SizeOfImage, PAGE_EXECUTE_READWRITE, &old))
        printf("%s: VirtualProtect failed %lu\n", path, GetLastError());
    return (uint8_t *)m;
}
static uint8_t *exec_alloc(size_t n) { return (uint8_t *)VirtualAlloc(NULL, n, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE); }
#else
#include <sys/mman.h>
#define _strdup strdup
uint8_t *host_map_image(const char *path);   /* test/host_map.c */
static uint8_t *map_image(const char *path) { return host_map_image(path); }
static uint8_t *exec_alloc(size_t n)
{
    void *p = mmap(NULL, n, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? NULL : (uint8_t *)p;
}
#endif

static void fake_str(uint8_t *p, const char *s)
{
    size_t n = strlen(s);
    memset(p, 0, 0x20);
    if (n <= 15) { memcpy(p, s, n); *(uint64_t *)(p + 0x18) = 15; }
    else { char *h = _strdup(s); *(char **)p = h; *(uint64_t *)(p + 0x18) = n; }
    *(uint64_t *)(p + 0x10) = n;
}

/* fake entity: runtime tags "a|b" and def props "c|d" (NULL = none) */
typedef struct { uint8_t ent[0x1000]; uint8_t tags[0x60 * 8]; uint8_t def[0x800]; uint8_t props[0x60 * 8]; } fake_t;
static int fill(uint8_t *vec, const char *list)
{
    char buf[256]; int n = 0;
    if (!list) return 0;
    snprintf(buf, sizeof buf, "%s", list);
    for (char *t = strtok(buf, "|"); t && n < 8; t = strtok(NULL, "|")) fake_str(vec + 0x60 * n++ + 0x40, t);
    return n;
}
static void fake(fake_t *f, int human, const char *tags, const char *props)
{
    memset(f, 0, sizeof *f);
    int nt = fill(f->tags, tags), np = fill(f->props, props);
    f->ent[0x30] = human ? 6 : 2;
    *(uint8_t **)(f->ent + cfg_tagoff) = f->tags;
    *(uint8_t **)(f->ent + cfg_tagoff + 8) = f->tags + 0x60 * nt;
    *(uint8_t **)(f->ent + cfg_defoff) = f->def;
    *(uint8_t **)(f->def + cfg_propoff) = f->props;
    *(uint8_t **)(f->def + cfg_propoff + 8) = f->props + 0x60 * np;
}

/* ---- stub run-through: a fake vehicle the real crew check accepts (no "abandoned" tag scan is
 * needed because the stub only reaches the core when the filter allows; placer has places; the
 * relation flag bit is off -> core returns true). Wrapper A then tail-calls the placer's vt[0x120/8]. */
typedef char (__attribute__((ms_abi)) *chk_t)(const void *veh, const void *unit);
static const void *g_slot_this; static int g_slot_calls;
static __attribute__((ms_abi)) char fake_slot(const void *placer) { g_slot_this = placer; g_slot_calls++; return 1; }
static void *g_vtbl[64];
static uint8_t g_placer[0x100];      /* +0x00 vtable, +0x58 "has places" */

/* vehicle fields used by the real code: tags (+tagoff), placer ptr (+0x570), flags (+0xDB4), player (+0xDA8),
 * placer slot object (+0xDE0). Offsets other than tags are read back from the exe for this build. */
typedef struct { uint32_t placer, flags, slotobj, player, slot_vt; } core_lay_t;
static int read_core_layout(const sites_t *s, core_lay_t *L)
{
    const uint8_t *a = s->a, *c = s->core;
    L->player  = *(const uint32_t *)(a + 17);                  /* movzx eax,byte[rdx+player] */
    L->slotobj = *(const uint32_t *)(a + 42);                  /* mov rcx,[rbx+slotobj] */
    L->slot_vt = *(const uint32_t *)(a + 57);                  /* jmp [rax+slot_vt] */
    L->placer = L->flags = 0;
    for (const uint8_t *p = c; p < c + 0x80; p++) {
        if (!L->placer && p[0] == 0x48 && p[1] == 0x8B && p[2] == 0x87 && p[7] == 0x48 && p[8] == 0x83 && p[9] == 0x78)
            L->placer = *(const uint32_t *)(p + 3);            /* mov rax,[rdi+placer]; cmp qword[rax+58h],0 */
        if (!L->flags && p[0] == 0x8B && p[1] == 0x87 && p[6] == 0xC1 && p[7] == 0xE8 && p[8] == 0x08)
            L->flags = *(const uint32_t *)(p + 2);             /* mov eax,[rdi+flags]; shr eax,8 */
    }
    return L->placer && L->flags && L->slot_vt < sizeof g_vtbl && (L->slot_vt % 8) == 0;
}

static int stub_runs(const sites_t *s)
{
    core_lay_t L;
    if (!read_core_layout(s, &L)) { printf("  FAIL stub run: core layout not read\n"); return 0; }
    uint8_t *mem = exec_alloc(4096);
    if (!mem) { printf("  FAIL stub run: no exec memory\n"); return 0; }
    chk_t sa = (chk_t)mem, sb = (chk_t)(mem + 256);
    build_stub(mem, (void *)bf_allow_order, s->a, A_JE, A_PATCH);
    build_stub(mem + 256, (void *)bf_allow_seat, s->b, B_JE, B_PATCH);

    static fake_t veh, sm, ig;
    fake(&veh, 0, NULL, NULL);                                 /* no runtime tags: the core skips its tag scan; = board_mortal */
    fake(&sm, 1, "boarder_astartes", NULL);
    fake(&ig, 1, "guard", NULL);                               /* no boarder_ tag = mortal */
    memset(g_placer, 0, sizeof g_placer);
    memset(g_vtbl, 0, sizeof g_vtbl);
    g_vtbl[L.slot_vt / 8] = (void *)fake_slot;
    *(void **)g_placer = g_vtbl;
    *(uint64_t *)(g_placer + 0x58) = 1;
    *(void **)(veh.ent + L.placer) = g_placer;
    *(uint32_t *)(veh.ent + L.flags) = 0;                     /* relation bit off -> owner check skipped */
    *(void **)(veh.ent + L.slotobj) = g_placer;
    ig.ent[L.player] = 1; sm.ent[L.player] = 1;

    int ok = 1;
    cfg_eject = 1; cfg_enabled = 1;
    /* refusal: stub answers 0 without entering the engine */
    g_slot_calls = 0;
    char r1 = sa(veh.ent, sm.ent), r2 = sb(veh.ent, sm.ent);
    if (r1 || r2 || g_slot_calls) { printf("  FAIL refusal path: order %d seat %d slot calls %d\n", r1, r2, g_slot_calls); ok = 0; }
    /* allowed: replayed prologue -> real core -> real tail (A: placer slot vcall) */
    g_slot_calls = 0;
    char r3 = sa(veh.ent, ig.ent), r4 = sb(veh.ent, ig.ent);
    if (r3 != 1 || r4 != 1 || g_slot_calls != 1 || g_slot_this != g_placer) {
        printf("  FAIL pass-through: order %d seat %d slot calls %d\n", r3, r4, g_slot_calls); ok = 0;
    }
    /* the real core still decides: placer without places -> false through the engine's own code */
    *(uint64_t *)(g_placer + 0x58) = 0;
    char r5 = sa(veh.ent, ig.ent), r6 = sb(veh.ent, ig.ent);
    *(uint64_t *)(g_placer + 0x58) = 1;
    if (r5 || r6) { printf("  FAIL engine refusal not kept: order %d seat %d\n", r5, r6); ok = 0; }
    /* NULL unit: filter passes, the replayed test/je takes the original fail branch */
    char r7 = sa(veh.ent, NULL), r8 = sb(veh.ent, NULL);
    if (r7 || r8) { printf("  FAIL NULL-unit branch: order %d seat %d\n", r7, r8); ok = 0; }
    /* EjectOccupants=0: the seat stub lets the engine decide */
    cfg_eject = 0;
    char r9 = sb(veh.ent, sm.ent);
    cfg_eject = 1;
    if (r9 != 1) { printf("  FAIL EjectOccupants=0: seat %d\n", r9); ok = 0; }
    /* the stubs are called 1000x more to catch stack imbalance (a crash or a bad return here) */
    for (int i = 0; i < 1000; i++) { sa(veh.ent, ig.ent); sb(veh.ent, sm.ent); sa(veh.ent, NULL); }
    printf("  stub run (layout player +0x%X placer +0x%X flags +0x%X slot +0x%X vt +0x%X): refuse %d/%d, pass %d/%d, "
           "engine-false %d/%d, null %d/%d, no-eject %d -> %s\n", L.player, L.placer, L.flags, L.slotobj, L.slot_vt,
           r1, r2, r3, r4, r5, r6, r7, r8, r9, ok ? "OK" : "FAIL");
    return ok;
}

int main(int argc, char **argv)
{
    int bad = 0;
    cfg_log = 0;
    cls_from_list(&g_defboard, cfg_defboard);
    cls_from_list(&g_defclass, cfg_defclass);
    for (int i = 1; i < argc; i++) {
        uint8_t *base = map_image(argv[i]);
        if (!base) { bad = 1; continue; }
        sites_t s;
        const char *err = find_all(base, &s);
        if (err) { printf("%s: FAIL %s\n", argv[i], err); bad = 1; continue; }
        cfg_tagoff = s.tagoff;
        uint8_t stub[256];
        size_t la = (size_t)(build_stub(stub, (void *)bf_allow_order, s.a, A_JE, A_PATCH) - stub);
        size_t lb = (size_t)(build_stub(stub, (void *)bf_allow_seat, s.b, B_JE, B_PATCH) - stub);
        if (s.a[A_JE] != 0x74 || s.b[B_JE] != 0x74 || la > 128 || lb > 128) { printf("  FAIL stub layout\n"); bad = 1; }
        printf("%s: order exe+0x%llX, seat exe+0x%llX, core exe+0x%llX, tags +0x%X, def +0x%X props +0x%X (%d sites), "
               "stubs %zu/%zu bytes\n", argv[i],
               (unsigned long long)(s.a - base), (unsigned long long)(s.b - base),
               (unsigned long long)(s.core - base), s.tagoff, s.defoff, s.propoff, s.prop_votes, la, lb);
        cfg_defoff = s.defoff; cfg_propoff = s.propoff;
        if (s.ev_err) { printf("  FAIL popup: %s\n", s.ev_err); bad = 1; }
        else printf("  popup: tagctor exe+0x%llX ctx exe+0x%llX fire exe+0x%llX dtor exe+0x%llX clock exe+0x%llX\n",
                    (unsigned long long)(s.tagctor - base), (unsigned long long)(s.ctxctor - base),
                    (unsigned long long)(s.fire - base), (unsigned long long)(s.ctxdtor - base), (unsigned long long)(s.clock - base));
        if (!stub_runs(&s)) bad = 1;
        /* a patched prologue (second copy / another dll) must not be found again */
        {
            uint8_t sa_[A_PATCH], sb_[B_PATCH];
            memcpy(sa_, s.a, A_PATCH); memcpy(sb_, s.b, B_PATCH);
            uint8_t pa[A_PATCH], pb[B_PATCH];
            memset(pa, 0x90, sizeof pa); memset(pb, 0x90, sizeof pb);
            emit_absjmp(pa, stub); emit_absjmp(pb, stub);
            memcpy(s.a, pa, A_PATCH);
            sites_t s2; const char *e2 = find_all(base, &s2);
            memcpy(s.a, sa_, A_PATCH);
            memcpy(s.b, pb, B_PATCH);
            sites_t s3; const char *e3 = find_all(base, &s3);
            memcpy(s.b, sb_, B_PATCH);
            int ok = e2 && e3;
            printf("  after patch: order -> %s | seat -> %s -> %s\n", e2 ? e2 : "found", e3 ? e3 : "found", ok ? "OK" : "FAIL");
            if (!ok) bad = 1;
        }
    }
    static fake_t plain_v, mortal_v, sm_v, daemon_v, both_v, closed_v, prop_v, case_v, long_v, bare_v, many_v;
    static fake_t sm, csm, scout, guard, Astartes, nonhuman, bare, longu, two, prop_sm;
    fake(&plain_v, 0, "smoke", "tank|heavy");                         /* no board_ tag -> DefaultBoard = mortal */
    fake(&mortal_v, 0, "board_mortal", NULL);
    fake(&sm_v, 0, "board_astartes", NULL);
    fake(&daemon_v, 0, "tank|board_daemon", NULL);
    fake(&both_v, 0, "board_mortal|board_astartes", NULL);
    fake(&closed_v, 0, "board_mortal|no_board", NULL);                /* no_board beats every board_ tag */
    fake(&prop_v, 0, NULL, "board_astartes");                         /* def PROP only, no tag: = no board_ tag */
    fake(&case_v, 0, "board_Astartes", NULL);
    fake(&long_v, 0, "board_a_very_long_heap_allocated_class_name", NULL);
    fake(&bare_v, 0, "board_", NULL);                                 /* bare prefix: no class -> default */
    fake(&many_v, 0, "board_a|board_b|board_c|board_d|board_e|board_f|board_astartes", NULL);
    fake(&sm, 1, "boarder_astartes|sm_tactical", NULL);
    fake(&csm, 1, "boarder_astartes|chaos", NULL);
    fake(&scout, 1, "sm_scout", NULL);                                /* no boarder_ tag -> mortal */
    fake(&guard, 1, "faction_guard", NULL);
    fake(&Astartes, 1, "boarder_Astartes", NULL);
    fake(&nonhuman, 0, "boarder_astartes", NULL);
    fake(&bare, 1, NULL, NULL);
    fake(&longu, 1, "boarder_a_very_long_heap_allocated_class_name", NULL);
    fake(&two, 1, "boarder_astartes|boarder_mortal", NULL);
    fake(&prop_sm, 1, NULL, "boarder_astartes");                      /* human def prop is not read: mortal */
    struct { fake_t *v, *u; int want; const char *what; } c[] = {
        { &plain_v, &guard, 1, "mortal -> untagged vehicle" },
        { &plain_v, &bare, 1, "no tags at all -> untagged vehicle" },
        { &plain_v, &sm, 0, "astartes -> untagged vehicle" },
        { &plain_v, &scout, 1, "scout (mortal) -> untagged vehicle" },
        { &mortal_v, &guard, 1, "mortal -> board_mortal" },
        { &mortal_v, &csm, 0, "astartes(chaos) -> board_mortal" },
        { &sm_v, &sm, 1, "astartes -> board_astartes" },
        { &sm_v, &guard, 0, "mortal -> board_astartes" },
        { &daemon_v, &guard, 0, "mortal -> board_daemon" },
        { &daemon_v, &sm, 0, "astartes -> board_daemon" },
        { &both_v, &sm, 1, "astartes -> mortal+astartes" },
        { &both_v, &guard, 1, "mortal -> mortal+astartes" },
        { &closed_v, &guard, 0, "mortal -> no_board" },
        { &closed_v, &sm, 0, "astartes -> no_board" },
        { &prop_v, &sm, 0, "board_ as vehicle def prop only is not read" },
        { &prop_v, &guard, 1, "mortal -> vehicle with board_ prop only" },
        { &case_v, &sm, 0, "astartes -> board_Astartes (case-sensitive)" },
        { &sm_v, &Astartes, 0, "boarder_Astartes -> board_astartes (case-sensitive)" },
        { &long_v, &longu, 1, "heap-allocated class names" },
        { &bare_v, &guard, 1, "bare board_ tag ignored -> default" },
        { &many_v, &sm, 1, "7th board_ tag still honoured" },
        { &sm_v, &two, 1, "unit with two classes" },
        { &mortal_v, &two, 1, "unit with two classes (2nd)" },
        { &sm_v, &prop_sm, 0, "boarder_ as human def prop is not read" },
        { &sm_v, &nonhuman, 1, "non-human never filtered" },
    };
    int fails = 0;
    cfg_log = 1; g_lines = 0;     /* builds every refusal log line (no log file in the test) */
    for (size_t i = 0; i < sizeof c / sizeof c[0]; i++) {
        int got = bf_allow(c[i].v->ent, c[i].u->ent, 0);
        if (got != c[i].want) { printf("  FAIL %s: got %d\n", c[i].what, got); fails++; }
    }
    /* ini defaults: empty DefaultBoard = untagged vehicles take anyone; DefaultClass list */
    {
        cls_from_list(&g_defboard, "");
        int a = bf_allow(plain_v.ent, sm.ent, 0);
        cls_from_list(&g_defboard, " mortal , astartes ");
        int b = bf_allow(plain_v.ent, sm.ent, 0);
        cls_from_list(&g_defboard, cfg_defboard);
        cls_from_list(&g_defclass, "guardsman");
        int d = bf_allow(mortal_v.ent, bare.ent, 0);
        cls_from_list(&g_defclass, cfg_defclass);
        int ok = a == 1 && b == 1 && d == 0;
        printf("  ini defaults: empty DefaultBoard %d, list %d, DefaultClass %d -> %s\n", a, b, d, ok ? "OK" : "FAIL");
        if (!ok) fails++;
    }
    printf("rules: %zu cases, %d failed\n", sizeof c / sizeof c[0], fails);
    /* popup throttle on a fake clock: first refusal fires, a retry 1 s later doesn't, 3 s later does */
    {
        uint32_t r, clk = 50000; int f1, f2, f3, f4;
        memset(g_pairs, 0, sizeof g_pairs);
        f1 = pair_note((void *)1, (void *)2, clk, &r);
        f2 = pair_note((void *)1, (void *)2, clk + 1000, &r);
        int r2 = (int)r;
        f3 = pair_note((void *)1, (void *)2, clk + 3500, &r);
        f4 = pair_note((void *)1, (void *)3, clk + 3600, &r);
        int ok = f1 && !f2 && r2 == 1 && f3 && f4;
        printf("popup throttle: %d %d(repeat %d) %d %d -> %s\n", f1, f2, r2, f3, f4, ok ? "OK" : "FAIL");
        if (!ok) bad = 1;
    }
    /* log line builder with 16 maximum-length classes must stay inside its buffer */
    {
        char vl[256] = "", cls[CLEN];
        memset(cls, 'x', CLEN - 1); cls[CLEN - 1] = 0;
        for (int i = 0; i < MAXC; i++) { app(vl, sizeof vl, cls); app(vl, sizeof vl, " "); }
        int ok = strlen(vl) == sizeof vl - 1;
        printf("log append bound: %zu chars -> %s\n", strlen(vl), ok ? "OK" : "FAIL");
        if (!ok) bad = 1;
    }
    if (fails) bad = 1;
    printf(bad ? "SOME FAILED\n" : "ALL OK\n");
    return bad;
}
#endif
