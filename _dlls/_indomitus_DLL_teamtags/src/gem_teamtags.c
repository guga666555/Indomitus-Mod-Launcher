/* GEM TeamTags 0.1 - automatic team tags for Call to Arms: Gates of Hell (GEM 2, 1.067)
 *
 * Every actor (human, vehicle, gun) carries the entity tag "team<N>", N = its owning player
 * (the byte {player N} sets): team0, team1, ... The tag is kept in step whenever the owner
 * changes (spawn, {player N}, capture, crew handoff, purchase) and after a map/save load.
 * Units with no player (0xFF) are left alone. Tags are plain entity tags, so {if tagged},
 * trigger {tag} selectors and {tags remove} all see them.
 *
 * Engine (all found at runtime, nothing hard-coded; game 1.067 addresses for reference):
 *   eActor::SetPlayer(entity, uint8_t *player)  exe+0x7EEEC0, the only writer of the player
 *     byte besides the ctor (0xFF). Virtual only (eActor + eHuman vtables, slot +0x1D0), no
 *     direct callers. Found through eAction_Player ({player N}): Execute -> its own slot ->
 *     "cmp al,[r8+PLAYER] ... call [rax+SLOT]".
 *   eActor::Unpack(entity) exe+0x7F6470 (map/save load, "Changing mid for %s failed"), eHuman's
 *     override exe+0x95BD30 calls it first. Load sets the player BEFORE the saved tags are read,
 *     so the tag is (re)applied after Unpack returns; SetPlayer calls inside Unpack are skipped.
 *   Tags: the {tags add} action's own functions (TagContainer ctor/copy/add_all/remove_all/dtor,
 *     Entity::SetTags, entity container +0x1D0) and its parser FromString(container, std::string*,
 *     ' ') to build "team<N>". Same derivation as GEM BreedTags.
 * The player byte is simulation state and SetPlayer runs on every machine, so every player in a
 * multiplayer game must run the dll (like the other launcher dlls).
 *
 * Build: build.bat   Offline finder test: build.bat test
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>

#define TT_VERSION "0.1"
#define TT_SECT    "TeamTags"
#define MAXP       32                      /* tags team0..team31 exist; the engine uses players 0..17 */

typedef struct { union { char buf[16]; char *ptr; } u; size_t size, cap; } MsString;

/* ---------------------------------------------------------------- config */
static int  cfg_enabled = 1, cfg_onload = 1, cfg_log = 0;
static char cfg_prefix[16] = "team";

/* ---------------------------------------------------------------- log */
static char  g_logpath[MAX_PATH];
static FILE *g_log;
static int   g_lines;
static void logf_(const char *fmt, ...)
{
    if (g_lines > 5000) return;
#ifndef TT_TEST
    if (!g_log && g_logpath[0]) g_log = fopen(g_logpath, "w");
    if (!g_log) return;
    FILE *out = g_log;
#else
    FILE *out = stdout;
#endif
    va_list ap; va_start(ap, fmt);
    vfprintf(out, fmt, ap); fputc('\n', out); fflush(out);
    va_end(ap);
    g_lines++;
}

/* ---------------------------------------------------------------- PE helpers */
typedef struct { uint8_t *p; size_t n; } Sec;
static Sec g_text, g_rdata, g_data;
static uint8_t *g_base;

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
static int in_text(const void *a)  { return (uint8_t *)a >= g_text.p  && (uint8_t *)a < g_text.p + g_text.n; }
static int in_rdata(const void *a) { return (uint8_t *)a >= g_rdata.p && (uint8_t *)a < g_rdata.p + g_rdata.n; }
#define OFF(a) ((unsigned long long)((uint8_t *)(a) - g_base))

static uint8_t *find_bytes(uint8_t *p, size_t n, const void *pat, size_t m)
{
    const uint8_t *q = pat;
    if (m > n) return NULL;
    for (size_t i = 0; i + m <= n; i++)
        if (p[i] == q[0] && !memcmp(p + i, q, m)) return p + i;
    return NULL;
}
static uint8_t *rel32(uint8_t *at, int len) { return at + len + *(int32_t *)(at + len - 4); }

/* next E8 call within window; returns target */
static uint8_t *next_call(uint8_t *p, size_t window, uint8_t **site)
{
    for (size_t i = 0; i < window; i++)
        if (p[i] == 0xE8) {
            uint8_t *t = rel32(p + i, 5);
            if (in_text(t)) { if (site) *site = p + i; return t; }
        }
    return NULL;
}

/* k-th 'lea reg,[rip+disp]' in .text whose target == want */
static uint8_t *find_lea_to(const void *want, int which)
{
    int k = 0;
    for (size_t i = 0; i + 7 <= g_text.n; i++) {
        uint8_t *p = g_text.p + i;
        if ((p[0] & 0xF8) != 0x48 || p[1] != 0x8D || (p[2] & 0xC7) != 0x05) continue;
        if (rel32(p, 7) == want && k++ == which) return p;
    }
    return NULL;
}

/* string in .rdata starting at a string boundary (prefix match when !whole) */
static uint8_t *find_str(const char *s, int whole)
{
    size_t m = strlen(s) + (whole ? 1 : 0);
    for (uint8_t *p = g_rdata.p; (p = find_bytes(p, g_rdata.p + g_rdata.n - p, s, m)); p++)
        if (p == g_rdata.p || p[-1] == 0) return p;
    return NULL;
}

/* vtable of a class by its RTTI name (complete object locator, offset 0) */
static void **find_vtable(const char *rtti)
{
    size_t n = strlen(rtti) + 1;
    uint8_t *td = NULL;
    for (uint8_t *p = g_data.p; p + n <= g_data.p + g_data.n; p++)
        if (*p == '.' && !memcmp(p, rtti, n)) { td = p - 0x10; break; }
    if (!td) return NULL;
    uint32_t tdr = (uint32_t)(td - g_base);
    for (uint8_t *p = g_rdata.p; p + 24 <= g_rdata.p + g_rdata.n; p += 4) {
        uint32_t *col = (uint32_t *)p;
        if (col[0] != 1 || col[1] != 0 || col[3] != tdr || col[5] != (uint32_t)(p - g_base)) continue;
        uint64_t va = (uint64_t)(uintptr_t)p;
        for (uint8_t *q = g_rdata.p; q + 16 <= g_rdata.p + g_rdata.n; q += 8)
            if (*(uint64_t *)q == va) return (void **)(q + 8);
    }
    return NULL;
}

/* ---------------------------------------------------------------- engine bindings */
typedef void     (*SetPlayerFn)(void *e, uint8_t *player);
typedef uint64_t (*UnpackFn)(void *a, void *b, void *c, void *d);
typedef void    *(*TcCtorFn)(void *c);
typedef void    *(*TcCopyFn)(void *dst, const void *src);
typedef void     (*TcAllFn)(void *dst, const void *src);
typedef void     (*TcFromStrFn)(void *c, const MsString *s, char sep);
typedef void     (*SetTagsFn)(void *e, const void *c);
typedef void     (*TcDtorFn)(void *c);

#define TC_SELF_A  0x308
#define TC_SELF_B  0x310
#define TC_VEC_BEG 0x318
#define TC_VEC_END 0x320
#define TC_SIZE    0x330
#define TAG_SIZE   0x60
#define TAG_STR    0x40

static uint32_t o_player, o_setslot, o_unpslot, o_tags;
static void   **vt_actor, **vt_human;
static SetPlayerFn f_setplayer;
static UnpackFn    f_unp_actor, f_unp_human;
static TcCtorFn    f_tcctor;
static TcCopyFn    f_tccopy;
static TcAllFn     f_addall, f_rmall;
static TcFromStrFn f_fromstr;
static SetTagsFn   f_settags;
static TcDtorFn    f_tcdtor;

/* {player N}: eAction_Player Execute tail-jumps to its own slot, which does
   movzx eax,[action+..] ; cmp al,[r8+PLAYER] ; je ; ... ; call [rax+SETSLOT] */
static const char *find_player(void)
{
    void **vt = find_vtable(".?AUeAction_Player@Interaction@@");
    if (!vt || !in_text(vt[1])) return "eAction_Player vtable not found";
    uint8_t *ex = vt[1], *f = NULL;
    for (uint8_t *p = ex; p < ex + 0x60 && !f; p++)       /* game: jmp [rax+0x48] (tail call), editor: call [rax+0x48] */
        if (p[0] == 0xFF && (p[1] == 0x60 || p[1] == 0x50) && !(p[2] & 7) && p[2] >= 0x18) f = vt[p[2] / 8];
    if (!f) f = ex;
    if (!in_text(f)) return "eAction_Player apply not in .text";
    for (uint8_t *p = f; p < f + 0x60; p++) {
        int r = (p[0] & 0xF0) == 0x40;                                   /* optional REX */
        uint8_t op = p[r], m = p[r + 1];
        if (!o_player && (op == 0x3A || op == 0x38) && (m & 0xC0) == 0x80 && (m & 7) != 4) {
            uint32_t d = *(uint32_t *)(p + r + 2);
            if (d > 0x100 && d < 0x4000) o_player = d;
        }
        if (o_player && !o_setslot && p[0] == 0xFF && (p[1] & 0xF8) == 0x90 && (p[1] & 7) != 4) {
            uint32_t d = *(uint32_t *)(p + 2);
            if (d >= 0x40 && d < 0x800 && !(d & 7)) o_setslot = d;
        }
    }
    if (!o_player) return "player byte offset not found";
    if (!o_setslot) return "SetPlayer slot not found";
    vt_actor = find_vtable(".?AVeActor@@");
    vt_human = find_vtable(".?AVeHuman@@");
    if (!vt_actor || !vt_human) return "eActor/eHuman vtables not found";
    f_setplayer = (SetPlayerFn)vt_actor[o_setslot / 8];
    if (!in_text(f_setplayer)) return "SetPlayer not in .text";
    if ((void *)vt_human[o_setslot / 8] != (void *)f_setplayer) return "eHuman overrides SetPlayer (unexpected)";
    /* it must store the player byte: (REX) 88 modrm(mod=10) disp32 == PLAYER */
    int stores = 0;
    for (uint8_t *p = (uint8_t *)f_setplayer; p < (uint8_t *)f_setplayer + 0x140; p++) {
        int r = (p[0] & 0xF0) == 0x40;
        if (p[r] == 0x88 && (p[r + 1] & 0xC0) == 0x80 && (p[r + 1] & 7) != 4 && *(uint32_t *)(p + r + 2) == o_player) stores++;
    }
    if (!stores) return "SetPlayer does not write the player byte";
    return NULL;
}

/* map/save load: the function that logs "Changing mid for %s failed" is eActor's Unpack slot */
static const char *find_unpack(void)
{
    uint8_t *s = find_str("Changing mid for %s failed", 0);
    if (!s) return "'Changing mid' string not found";
    for (int k = 0; k < 4; k++) {
        uint8_t *lea = find_lea_to(s, k);
        if (!lea) break;
        for (uint8_t *f = (uint8_t *)((uintptr_t)lea & ~(uintptr_t)15); f > lea - 0x3000 && f > g_text.p; f -= 16) {
            if (f[-1] != 0xCC) continue;
            for (uint32_t i = 0; i < 0x200; i++)
                if (vt_actor[i] == (void *)f) {
                    o_unpslot = i * 8;
                    f_unp_actor = (UnpackFn)f;
                    f_unp_human = (UnpackFn)vt_human[i];
                    if (!in_text(f_unp_human)) return "eHuman Unpack not in .text";
                    return NULL;
                }
        }
    }
    return "eActor Unpack not found";
}

/* the {tags add|remove} action: table entry {"tags", ctor} in .data */
static const char *find_tags(void)
{
    uint8_t *ctor = NULL; int hits = 0;
    for (size_t i = 0; i + 16 <= g_data.n; i += 8) {
        uint8_t *str = *(uint8_t **)(g_data.p + i), *fn = *(uint8_t **)(g_data.p + i + 8);
        if (!in_rdata(str) || !in_text(fn)) continue;
        if (memcmp(str, "tags", 5) || str[-1] != 0) continue;
        ctor = fn; hits++;
    }
    if (hits != 1) return hits ? "tags action entry ambiguous" : "tags action entry not found";
    /* ctor: ... lea rax,[rip+vtable] ; mov [rbx],rax ; call TagContainer::ctor(rbx+0x28) */
    uint8_t *vt = NULL, *after = NULL;
    for (uint8_t *p = ctor; p < ctor + 0x80; p++)
        if ((p[0] == 0x48 || p[0] == 0x4C) && p[1] == 0x8D && (p[2] & 0xC7) == 0x05 && in_rdata(rel32(p, 7))) { vt = rel32(p, 7); after = p + 7; break; }
    if (!vt) return "tags action vtable not found";
    static const uint8_t ctor_sig[] = { 0xC6, 0x81, 0x00, 0x03, 0x00, 0x00, 0x00 };            /* mov byte [rcx+0x300],0 */
    uint8_t *cc = next_call(after, 0x20, NULL);
    if (!cc || !find_bytes(cc, 0x20, ctor_sig, sizeof ctor_sig)) return "TagContainer ctor not recognised";
    f_tcctor = (TcCtorFn)cc;

    /* Execute (slot 1): add rdx,OFF ; call copy ; cmp byte [..],0 ; je REMOVE ; call add_all ; ... call remove_all ;
       lea rdx,[rsp+0x20] ; call SetTags ; lea rcx,[rsp+0x20] ; call dtor  (BreedTags derivation) */
    uint8_t *ev = ((uint8_t **)vt)[1], *p = NULL, *site = NULL;
    if (!in_text(ev)) return "tags action execute not in .text";
    for (uint8_t *q = ev; q < ev + 0x100; q++)
        if (q[0] == 0x48 && q[1] == 0x81 && q[2] == 0xC2) { o_tags = *(uint32_t *)(q + 3); p = q + 7; break; }
    if (!p || o_tags < 0x100 || o_tags > 0x400) return "entity tag container offset not found";
    uint8_t *cp = next_call(p, 0x20, &site);
    if (!cp || !find_bytes(cp, 0x40, ctor_sig, sizeof ctor_sig)) return "TagContainer copy not recognised";
    f_tccopy = (TcCopyFn)cp;
    uint8_t *rmv = NULL; p = NULL;
    for (uint8_t *q = site; q < ev + 0x200 && !p; q++)
        if (q[0] == 0x80 && (q[1] & 0xF8) == 0xB8 && q[6] == 0x00)
            for (uint8_t *j = q + 7; j < q + 0x20; j++)
                if (j[0] == 0x74) { p = j + 2; rmv = j + 2 + (int8_t)j[1]; break; }
    if (!p) return "tags add/remove switch not found";
    static const uint8_t aa_sig[] = { 0x48, 0x8B, 0x9A, 0x18, 0x03, 0x00, 0x00 };             /* mov rbx,[rdx+0x318] */
    uint8_t *aa = next_call(p, 0x10, &site);
    if (!aa || !find_bytes(aa, 0x20, aa_sig, sizeof aa_sig)) return "add_all not recognised";
    f_addall = (TcAllFn)aa;
    uint8_t *ra = (rmv && in_text(rmv) && rmv[0] == 0xE8) ? rel32(rmv, 5) : NULL;
    if (!ra || ra == aa || !in_text(ra) || !find_bytes(ra, 0x20, aa_sig, sizeof aa_sig)) return "remove_all not recognised";
    f_rmall = (TcAllFn)ra;
    static const uint8_t lea_rdx[] = { 0x48, 0x8D, 0x54, 0x24, 0x20 }, lea_rcx[] = { 0x48, 0x8D, 0x4C, 0x24, 0x20 };
    p = find_bytes(site + 5, 0x80, lea_rdx, sizeof lea_rdx);
    uint8_t *st = p ? next_call(p + 5, 0x10, &site) : NULL;
    static const uint8_t st_sig[] = { 0xE8, 0x04, 0x00, 0x00 };                                /* [rcx+0x4e8] */
    if (!st || !find_bytes(st, 0x30, st_sig, sizeof st_sig)) return "SetTags not recognised";
    f_settags = (SetTagsFn)st;
    p = find_bytes(site + 5, 0x40, lea_rcx, sizeof lea_rcx);
    uint8_t *dt = p ? next_call(p + 5, 0x10, NULL) : NULL;
    static const uint8_t dt_sig[] = { 0x48, 0x81, 0xC1, 0x10, 0x03, 0x00, 0x00 };             /* add rcx,0x310 */
    if (!dt || memcmp(dt, dt_sig, sizeof dt_sig)) return "TagContainer dtor not recognised";
    f_tcdtor = (TcDtorFn)dt;

    /* parse (slot 5): ... mov r8b,0x20 ; call FromString(container, std::string*, ' ') */
    uint8_t *ps = ((uint8_t **)vt)[5], *fs = NULL;
    if (!in_text(ps)) return "tags action parse not in .text";
    for (uint8_t *q = ps; q < ps + 0x180 && !fs; q++)
        if (q[0] == 0x41 && q[1] == 0xB0 && q[2] == 0x20) fs = next_call(q + 3, 0x10, NULL);
    if (!fs) return "FromString not found";
    int clears = 0;                                                       /* mov reg,[rcx+0x318] early on */
    for (uint8_t *q = fs; q < fs + 0x40; q++)
        if ((q[0] & 0xFB) == 0x48 && q[1] == 0x8B && (q[2] & 0xC7) == 0x81 && *(uint32_t *)(q + 3) == TC_VEC_BEG) clears = 1;
    if (!clears) return "FromString not recognised";
    f_fromstr = (TcFromStrFn)fs;
    return NULL;
}

static const char *find_all(uint8_t *base)
{
    o_player = o_setslot = o_unpslot = o_tags = 0;
    f_unp_actor = f_unp_human = NULL;
    if (!get_sections(base)) return "sections not found";
    const char *e;
    if ((e = find_player())) return e;
    if ((e = find_tags())) return e;
    if ((e = find_unpack())) { logf_("[GEM TeamTags] load re-apply unavailable: %s", e); f_unp_actor = f_unp_human = NULL; }
    return NULL;
}

/* ---------------------------------------------------------------- tags */
static const char *ms_cstr(const MsString *s) { return s->cap > 15 ? s->u.ptr : s->u.buf; }
static void ms_make(MsString *s, char *text)
{
    memset(s, 0, sizeof *s);
    s->size = strlen(text);
    if (s->size < 16) { memcpy(s->u.buf, text, s->size + 1); s->cap = 15; }
    else { s->u.ptr = text; s->cap = s->size; }                          /* read only: the engine copies it */
}

/* team number of a tag "<prefix><digits>", or -1 */
static int team_num(const uint8_t *tag)
{
    const MsString *s = (const MsString *)(tag + TAG_STR);
    if (s->size > 40) return -1;
    const char *t = ms_cstr(s);
    size_t pl = strlen(cfg_prefix);
    if (s->size <= pl || s->size > pl + 2 || memcmp(t, cfg_prefix, pl)) return -1;
    int n = 0;
    for (size_t i = pl; i < s->size; i++) { if (t[i] < '0' || t[i] > '9') return -1; n = n * 10 + (t[i] - '0'); }
    if (s->size == pl + 2 && t[pl] == '0') return -1;                    /* "team05" is not ours */
    return n < MAXP ? n : -1;
}

/* *has = the container holds prefix<p>; *other = it holds another prefix<k> */
static void scan_tags(const uint8_t *c, int p, int *has, int *other)
{
    *has = *other = 0;
    const uint8_t *b = *(uint8_t *const *)(c + TC_VEC_BEG), *e = *(uint8_t *const *)(c + TC_VEC_END);
    if (!b || e < b || (size_t)(e - b) > 4096 * TAG_SIZE) return;
    for (const uint8_t *t = b; t < e; t += TAG_SIZE) {
        int n = team_num(t);
        if (n == p) *has = 1; else if (n >= 0) *other = 1;
    }
}
static size_t tag_count(const uint8_t *c) { return (size_t)(*(uint8_t *const *)(c + TC_VEC_END) - *(uint8_t *const *)(c + TC_VEC_BEG)) / TAG_SIZE; }
static int container_ok(const uint8_t *c) { return *(const uint8_t *const *)(c + TC_SELF_A) == c && *(const uint8_t *const *)(c + TC_SELF_B) == c; }

static __attribute__((aligned(16))) uint8_t g_store[MAXP + 1][TC_SIZE + 16];
static uint8_t *g_one[MAXP], *g_allc;
static volatile LONG g_broken;
static CRITICAL_SECTION g_cs;

/* containers built on first use (game thread, engine fully up), kept for the process lifetime */
static int build(uint8_t *slot, char *text, size_t want)
{
    MsString s; ms_make(&s, text);
    f_tcctor(slot);
    f_fromstr(slot, &s, ' ');
    if (!container_ok(slot) || tag_count(slot) != want) {
        logf_("[GEM TeamTags] built \"%.40s\" has %zu tags, expected %zu: disabled", text, tag_count(slot), want);
        return 0;
    }
    return 1;
}
static int ensure(int p)
{
    if (g_broken) return 0;
    if (g_one[p] && g_allc) return 1;
    int ok = 1;
    EnterCriticalSection(&g_cs);
    if (!g_allc) {
        static char all[MAXP * 20]; size_t n = 0;
        for (int i = 0; i < MAXP; i++) n += snprintf(all + n, sizeof all - n, "%s%s%d", i ? " " : "", cfg_prefix, i);
        uint8_t *c = g_store[MAXP];
        if (build(c, all, MAXP)) g_allc = c; else ok = 0;
    }
    if (ok && !g_one[p]) {
        char one[24]; snprintf(one, sizeof one, "%s%d", cfg_prefix, p);
        uint8_t *c = g_store[p];
        if (build(c, one, 1)) g_one[p] = c; else ok = 0;
    }
    if (!ok) g_broken = 1;
    LeaveCriticalSection(&g_cs);
    return ok;
}

static void apply(uint8_t *e, const char *why)
{
    if (!cfg_enabled || g_broken || !e) return;
    int p = e[o_player];
    if (p >= MAXP) return;                                               /* no player: leave the tags alone */
    uint8_t *c = e + o_tags;
    if (!container_ok(c)) return;
    int has, other;
    scan_tags(c, p, &has, &other);
    if (has && !other) return;
    if (!ensure(p)) return;
    uint8_t raw[TC_SIZE + 32], *tmp = (uint8_t *)(((uintptr_t)raw + 15) & ~(uintptr_t)15);
    f_tccopy(tmp, c);
    if (other) f_rmall(tmp, g_allc);
    f_addall(tmp, g_one[p]);
    f_settags(e, tmp);
    f_tcdtor(tmp);
    if (cfg_log) logf_("%s: entity %p player %d -> %s%d%s", why, (void *)e, p, cfg_prefix, p, other ? " (old team tag removed)" : "");
}

/* ---------------------------------------------------------------- hooks */
static SetPlayerFn orig_set;
static UnpackFn    orig_unp_actor, orig_unp_human;
static DWORD       g_tls = TLS_OUT_OF_INDEXES;                           /* per thread: inside Unpack */

static void hk_set(void *e, uint8_t *pl)
{
    orig_set(e, pl);
    if (TlsGetValue(g_tls)) return;                                      /* load: done once Unpack returns */
    apply((uint8_t *)e, "player");
}
static uint64_t unpack(UnpackFn orig, void *a, void *b, void *c, void *d)
{
    uintptr_t depth = (uintptr_t)TlsGetValue(g_tls);
    TlsSetValue(g_tls, (void *)(depth + 1));
    uint64_t r = orig(a, b, c, d);
    TlsSetValue(g_tls, (void *)depth);
    apply((uint8_t *)a, "load");
    return r;
}
static uint64_t hk_unp_actor(void *a, void *b, void *c, void *d) { return unpack(orig_unp_actor, a, b, c, d); }
static uint64_t hk_unp_human(void *a, void *b, void *c, void *d) { return unpack(orig_unp_human, a, b, c, d); }

/* ---------------------------------------------------------------- config */
static void load_ini(const char *ini)
{
    char buf[32];
    cfg_enabled = GetPrivateProfileIntA(TT_SECT, "Enabled", 1, ini);
    cfg_onload  = GetPrivateProfileIntA(TT_SECT, "ReapplyOnLoad", 1, ini);
    cfg_log     = GetPrivateProfileIntA(TT_SECT, "Log", 0, ini);
    GetPrivateProfileStringA(TT_SECT, "Prefix", cfg_prefix, buf, sizeof buf, ini);
    size_t n = strlen(buf);
    if (n && n <= 12) {
        for (size_t i = 0; i < n; i++) buf[i] = (char)tolower((unsigned char)buf[i]);  /* engine tags are lowercase */
        strcpy(cfg_prefix, buf);
    }
}

#ifndef TT_TEST
static int patch_slot(void **slot, void *hook)
{
    DWORD old;
    if (!VirtualProtect(slot, sizeof *slot, PAGE_READWRITE, &old)) return 0;
    *slot = hook;
    VirtualProtect(slot, sizeof *slot, old, &old);
    return 1;
}

static void install(void)
{
    const char *err = find_all((uint8_t *)GetModuleHandleW(NULL));
    if (err) { logf_("[GEM TeamTags %s] NOT installed: %s", TT_VERSION, err); return; }
    InitializeCriticalSection(&g_cs);
    g_tls = TlsAlloc();
    if (g_tls == TLS_OUT_OF_INDEXES) { logf_("[GEM TeamTags] TlsAlloc failed"); return; }
    if (!cfg_enabled) { logf_("[GEM TeamTags %s] Enabled=0: not installed", TT_VERSION); return; }
    orig_set = f_setplayer;
    int ok = patch_slot(&vt_actor[o_setslot / 8], (void *)hk_set) && patch_slot(&vt_human[o_setslot / 8], (void *)hk_set);
    int load = 0;
    if (ok && cfg_onload && f_unp_actor) {
        orig_unp_actor = f_unp_actor; orig_unp_human = f_unp_human;
        load = patch_slot(&vt_actor[o_unpslot / 8], (void *)hk_unp_actor);
        if (load && f_unp_human != f_unp_actor) load = patch_slot(&vt_human[o_unpslot / 8], (void *)hk_unp_human);
        else if (load) patch_slot(&vt_human[o_unpslot / 8], (void *)hk_unp_actor);
    }
    if (!ok) { logf_("[GEM TeamTags] VirtualProtect failed (%lu)", GetLastError()); return; }
    logf_("[GEM TeamTags %s] installed | SetPlayer exe+0x%llX (slot +0x%X, player +0x%X), Unpack actor exe+0x%llX human exe+0x%llX "
          "(slot +0x%X, %s) | tags +0x%X ctor exe+0x%llX copy exe+0x%llX add exe+0x%llX remove exe+0x%llX SetTags exe+0x%llX "
          "dtor exe+0x%llX FromString exe+0x%llX | Prefix=%s ReapplyOnLoad=%d Log=%d",
          TT_VERSION, OFF(f_setplayer), o_setslot, o_player,
          f_unp_actor ? OFF(f_unp_actor) : 0ULL, f_unp_human ? OFF(f_unp_human) : 0ULL, o_unpslot, load ? "hooked" : "off",
          o_tags, OFF(f_tcctor), OFF(f_tccopy), OFF(f_addall), OFF(f_rmall), OFF(f_settags), OFF(f_tcdtor), OFF(f_fromstr),
          cfg_prefix, cfg_onload, cfg_log);
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
        snprintf(ini, sizeof ini, "%s.ini", path);
        snprintf(g_logpath, sizeof g_logpath, "%s%s.log", path,
                 strstr(nm, "_ed") ? "_editor" : (strstr(nm, "_server") ? "_server" : ""));
        load_ini(ini);
        install();
    }
    return TRUE;
}
#else
/* ---------------------------------------------------------------- offline test */
/* build.bat test -> tt_test.exe <exe>...: the finders on each mapped exe (one exe per process),
   then the tag-scan logic on fake containers. Engine functions are not called offline. */
static uint8_t g_fake[TC_SIZE + 16], g_tags[8][TAG_SIZE];
static void fake_set(int n, const char **names)
{
    uint8_t *c = g_fake;
    *(uint8_t **)(c + TC_SELF_A) = c; *(uint8_t **)(c + TC_SELF_B) = c;
    for (int i = 0; i < n; i++) ms_make((MsString *)(g_tags[i] + TAG_STR), (char *)names[i]);
    *(uint8_t **)(c + TC_VEC_BEG) = g_tags[0]; *(uint8_t **)(c + TC_VEC_END) = g_tags[n];
}

int main(int argc, char **argv)
{
    int bad = 0;
    if (argc > 1) {
        HMODULE m = LoadLibraryExA(argv[1], NULL, DONT_RESOLVE_DLL_REFERENCES);
        if (!m) { printf("%s: load failed %lu\n", argv[1], GetLastError()); return 1; }
        const char *err = find_all((uint8_t *)m);
        if (err) { printf("%s: FAIL %s\n", argv[1], err); return 1; }
        printf("%s:\n  SetPlayer exe+0x%llX slot +0x%X player +0x%X\n  Unpack actor exe+0x%llX human exe+0x%llX slot +0x%X\n"
               "  tags +0x%X ctor exe+0x%llX copy exe+0x%llX add exe+0x%llX remove exe+0x%llX SetTags exe+0x%llX dtor exe+0x%llX FromString exe+0x%llX\n",
               argv[1], OFF(f_setplayer), o_setslot, o_player,
               f_unp_actor ? OFF(f_unp_actor) : 0ULL, f_unp_human ? OFF(f_unp_human) : 0ULL, o_unpslot,
               o_tags, OFF(f_tcctor), OFF(f_tccopy), OFF(f_addall), OFF(f_rmall), OFF(f_settags), OFF(f_tcdtor), OFF(f_fromstr));
        if (!f_unp_actor) { printf("  FAIL no Unpack\n"); bad = 1; }
    }
    struct { int p; int n; const char *t[5]; int has, other; } cs[] = {
        { 3, 2, { "soldier", "team3" }, 1, 0 },
        { 3, 2, { "soldier", "team1" }, 0, 1 },
        { 0, 3, { "team0", "team17", "x" }, 1, 1 },
        { 2, 3, { "team05", "teamx", "team_a" }, 0, 0 },
        { 12, 2, { "team12", "team123" }, 1, 0 },
        { 5, 0, { 0 }, 0, 0 },
    };
    for (size_t i = 0; i < sizeof cs / sizeof *cs; i++) {
        int has, other;
        fake_set(cs[i].n, cs[i].t);
        scan_tags(g_fake, cs[i].p, &has, &other);
        int ok = has == cs[i].has && other == cs[i].other;
        printf("  scan case %zu (player %d): has=%d other=%d %s\n", i, cs[i].p, has, other, ok ? "OK" : "FAIL");
        if (!ok) bad = 1;
    }
    printf(bad ? "SOME FAILED\n" : "ALL OK\n");
    return bad;
}
#endif
