/* GEM SupplyTags 0.1 - switch an entity's supply zone off with an entity tag (Call to Arms: Gates of Hell, GEM 2, 1.069)
 *
 * While an entity with an {extender "supply_zone"} (ammo crates, supply trucks, the SM supply drop pod) carries the
 * tag "supply_off" (ini Tag=), its zone resupplies nobody and reports itself disabled. Remove the tag and it works
 * again. Written for the supply drop pod (Nov 21 2026, Alex): no resupply while the pod waits hidden or falls.
 *
 * Engine (found at runtime, nothing hard-coded; game 1.069 addresses for reference):
 *   eSupplyZoneExtender (RTTI .?AVeSupplyZoneExtender@Entity@@), vtable exe+0x109BA88.
 *   ext+0x430 = "enabled" byte (the reflected property the map files save as {Extender "supply_zone" {enabled}};
 *   ctor value 1; no SDL action writes it). ext+0x18 = owner entity.
 *   Message handler = vtable slot 7 (exe+0x48E740): msg [rdx] 0 = init, 0xB = tick (every 0.5 s the resupply pass
 *   exe+0x490670, which skips everything when enabled == 0, exe+0x4908C7), 0xC = status query (exe+0x490FA0, reports
 *   "can supply" only when enabled != 0, exe+0x49114C).
 * How: for a tagged owner the hook sets the byte to 0 for the length of a tick / query call and puts the engine's
 * value back right after, so a save written while held keeps the real value and nothing has to be restored later.
 * MP: resupply runs on the host; the host (game or call_to_arms_server.exe) must run the dll. Clients only draw.
 *
 * Build: build.bat   Offline finder + logic test: build.bat test
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>

#define SP_VERSION "0.1"
#define SP_SECT    "SupplyTags"

typedef struct { union { char buf[16]; char *ptr; } u; size_t size, cap; } MsString;

static int  cfg_enabled = 1, cfg_log = 0;
static char cfg_tag[48] = "supply_off";
static size_t cfg_taglen = 10;

static char  g_logpath[MAX_PATH];
static FILE *g_log;
static int   g_lines;
static void logf_(const char *fmt, ...)
{
    if (g_lines > 5000) return;
#ifndef SP_TEST
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

/* ---------------------------------------------------------------- PE helpers (as GEM SensorTags) */
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

static uint8_t *next_call(uint8_t *p, size_t window, uint8_t **site)
{
    for (size_t i = 0; i < window; i++)
        if (p[i] == 0xE8) {
            uint8_t *t = rel32(p + i, 5);
            if (in_text(t)) { if (site) *site = p + i; return t; }
        }
    return NULL;
}

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
typedef uint64_t (*MsgFn)(void *ext, void *msg, void *a3, void *a4);

#define TC_SELF_A  0x308
#define TC_SELF_B  0x310
#define TC_VEC_BEG 0x318
#define TC_VEC_END 0x320
#define TAG_SIZE   0x60
#define TAG_STR    0x40
#define MSG_TICK   0x0B
#define MSG_QUERY  0x0C

static void   **vt_sz;
static uint32_t s_msg, o_enabled, o_owner, o_tags;
static MsgFn    f_msg;

/* message handler: push rbx ; sub rsp,0x20 ; mov r8d,[rdx] ; mov rbx,rcx ; test r8d,r8d ; je ; sub r8d,0xB
   owner: mov rcx,[rbx+OWN] followed by mov rcx,[rcx+d32] (the init branch)
   enabled: the reflection slot loads lea rax,[rdi+EN] shortly before lea rdx,"enabled" */
static const char *find_supply(void)
{
    vt_sz = find_vtable(".?AVeSupplyZoneExtender@Entity@@");
    if (!vt_sz) return "eSupplyZoneExtender vtable not found";
    static const uint8_t msg_sig[] = { 0x44, 0x8B, 0x02, 0x48, 0x8B, 0xD9, 0x45, 0x85, 0xC0 };
    static const uint8_t sub_b[]   = { 0x41, 0x83, 0xE8, MSG_TICK };
    int hits = 0;
    for (uint32_t k = 2; k < 16; k++) {
        uint8_t *f = vt_sz[k];
        if (!in_text(f)) break;
        if (find_bytes(f, 0x10, msg_sig, sizeof msg_sig) && find_bytes(f, 0x20, sub_b, sizeof sub_b)) { hits++; s_msg = k; f_msg = (MsgFn)f; }
    }
    if (hits != 1) return hits ? "message slot ambiguous" : "message slot not found";
    uint8_t *f = (uint8_t *)f_msg;
    for (uint8_t *p = f; p < f + 0x90 && !o_owner; p++)
        if (p[0] == 0x48 && p[1] == 0x8B && p[2] == 0x4B && p[3] >= 0x08 && p[3] < 0x80 && !(p[3] & 7))
            for (uint8_t *q = p + 4; q < p + 24; q++)
                if (q[0] == 0x48 && q[1] == 0x8B && q[2] == 0x89) { o_owner = p[3]; break; }
    if (!o_owner) return "owner offset not found";

    for (uint32_t k = 1; k < 16 && !o_enabled; k++) {
        uint8_t *g = vt_sz[k];
        if (!in_text(g)) break;
        for (uint8_t *p = g; p < g + 0x300 && !o_enabled; p++) {
            if (!(p[0] == 0x48 && p[1] == 0x8D && p[2] == 0x15)) continue;
            uint8_t *s = rel32(p, 7);
            if (!in_rdata(s) || memcmp(s, "enabled", 8)) continue;
            for (uint8_t *q = p - 0x80; q < p; q++)
                if (q[0] == 0x48 && q[1] == 0x8D && q[2] == 0x87) { uint32_t d = *(uint32_t *)(q + 3); if (d >= 0x100 && d < 0x1000) o_enabled = d; }
        }
    }
    if (!o_enabled) return "enabled offset not found";
    /* the tick path tests it: cmp byte [r+EN],r8 */
    int tests = 0;
    for (uint8_t *p = g_text.p; p + 8 <= g_text.p + g_text.n; p++)
        if ((p[0] == 0x45 || p[0] == 0x44 || p[0] == 0x40 || p[0] == 0x41) && p[1] == 0x38 && (p[2] & 0xC0) == 0x80 &&
            *(uint32_t *)(p + 3) == o_enabled) tests++;
        else if (p[0] == 0x80 && (p[1] & 0xF8) == 0xB8 && *(uint32_t *)(p + 2) == o_enabled && p[6] == 0) tests++;
    if (tests < 2) return "enabled byte readers not recognised";
    return NULL;
}

/* entity tag container offset, from the {tags add|remove} action (as GEM TeamTags / SensorTags) */
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
    uint8_t *vt = NULL, *after = NULL;
    for (uint8_t *p = ctor; p < ctor + 0x80; p++)
        if ((p[0] == 0x48 || p[0] == 0x4C) && p[1] == 0x8D && (p[2] & 0xC7) == 0x05 && in_rdata(rel32(p, 7))) { vt = rel32(p, 7); after = p + 7; break; }
    if (!vt) return "tags action vtable not found";
    static const uint8_t ctor_sig[] = { 0xC6, 0x81, 0x00, 0x03, 0x00, 0x00, 0x00 };
    uint8_t *cc = next_call(after, 0x20, NULL);
    if (!cc || !find_bytes(cc, 0x20, ctor_sig, sizeof ctor_sig)) return "TagContainer ctor not recognised";
    uint8_t *ev = ((uint8_t **)vt)[1], *p = NULL;
    if (!in_text(ev)) return "tags action execute not in .text";
    for (uint8_t *q = ev; q < ev + 0x100; q++)
        if (q[0] == 0x48 && q[1] == 0x81 && q[2] == 0xC2) { o_tags = *(uint32_t *)(q + 3); p = q + 7; break; }
    if (!p || o_tags < 0x100 || o_tags > 0x400) return "entity tag container offset not found";
    uint8_t *cp = next_call(p, 0x20, NULL);
    if (!cp || !find_bytes(cp, 0x40, ctor_sig, sizeof ctor_sig)) return "TagContainer copy not recognised";
    return NULL;
}

static const char *find_all(uint8_t *base)
{
    s_msg = o_enabled = o_owner = o_tags = 0;
    if (!get_sections(base)) return "sections not found";
    const char *e;
    if ((e = find_supply())) return e;
    if ((e = find_tags())) return e;
    return NULL;
}

/* ---------------------------------------------------------------- tag test */
static const char *ms_cstr(const MsString *s) { return s->cap > 15 ? s->u.ptr : s->u.buf; }

static int has_tag(const uint8_t *e)
{
    const uint8_t *c = e + o_tags;
    if (*(const uint8_t *const *)(c + TC_SELF_A) != c || *(const uint8_t *const *)(c + TC_SELF_B) != c) return 0;
    const uint8_t *b = *(uint8_t *const *)(c + TC_VEC_BEG), *end = *(uint8_t *const *)(c + TC_VEC_END);
    if (!b || end < b || (size_t)(end - b) > 4096 * TAG_SIZE) return 0;
    for (const uint8_t *t = b; t < end; t += TAG_SIZE) {
        const MsString *s = (const MsString *)(t + TAG_STR);
        if (s->size == cfg_taglen && !memcmp(ms_cstr(s), cfg_tag, cfg_taglen)) return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------- hook */
static MsgFn orig_msg;
static volatile LONG g_held;            /* tick / query calls run disabled (log only) */

static uint64_t hk_msg(void *ext, void *msg, void *a3, void *a4)
{
    uint8_t *x = ext;
    uint32_t type = msg ? *(uint32_t *)msg : 0;
    if (type == MSG_TICK || type == MSG_QUERY) {
        uint8_t *e = *(uint8_t **)(x + o_owner);
        if (e && has_tag(e)) {
            uint8_t was = x[o_enabled];
            x[o_enabled] = 0;
            uint64_t r = orig_msg(ext, msg, a3, a4);
            x[o_enabled] = was;
            if (cfg_log && type == MSG_TICK && (InterlockedIncrement(&g_held) % 120) == 1)
                logf_("held: entity %p zone %p (%ld held ticks)", (void *)e, ext, g_held);
            return r;
        }
    }
    return orig_msg(ext, msg, a3, a4);
}

static void load_ini(const char *ini)
{
    char buf[64];
    cfg_enabled = GetPrivateProfileIntA(SP_SECT, "Enabled", 1, ini);
    cfg_log     = GetPrivateProfileIntA(SP_SECT, "Log", 0, ini);
    GetPrivateProfileStringA(SP_SECT, "Tag", cfg_tag, buf, sizeof buf, ini);
    size_t n = strlen(buf);
    while (n && (buf[n - 1] == ' ' || buf[n - 1] == '\t')) buf[--n] = 0;
    if (n && n < sizeof cfg_tag) {
        for (size_t i = 0; i < n; i++) buf[i] = (char)tolower((unsigned char)buf[i]);
        strcpy(cfg_tag, buf);
    }
    cfg_taglen = strlen(cfg_tag);
}

#ifndef SP_TEST
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
    if (err) { logf_("[GEM SupplyTags %s] NOT installed: %s", SP_VERSION, err); return; }
    if (!cfg_enabled) { logf_("[GEM SupplyTags %s] Enabled=0: not installed", SP_VERSION); return; }
    orig_msg = f_msg;
    if (!patch_slot(&vt_sz[s_msg], (void *)hk_msg)) { logf_("[GEM SupplyTags] VirtualProtect failed (%lu)", GetLastError()); return; }
    logf_("[GEM SupplyTags %s] installed | eSupplyZoneExtender vtable exe+0x%llX message slot %u exe+0x%llX | "
          "enabled +0x%X owner +0x%X tags +0x%X | Tag=%s Log=%d",
          SP_VERSION, OFF(vt_sz), s_msg, OFF(f_msg), o_enabled, o_owner, o_tags, cfg_tag, cfg_log);
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
/* sp_test.exe <exe>: the finders on that mapped exe, then the hold logic on a fake zone + entity. */
static uint8_t g_seen;                  /* the enabled byte as the engine saw it during the call */
static int g_calls;
static uint64_t fake_msg(void *ext, void *msg, void *a3, void *a4)
{
    (void)msg; (void)a3; (void)a4;
    g_seen = ((uint8_t *)ext)[o_enabled]; g_calls++; return 7;
}

static __attribute__((aligned(16))) uint8_t g_zone[0x800], g_ent[0x4000], g_tag[2][TAG_SIZE];
static void set_tags(int n, const char **names)
{
    uint8_t *c = g_ent + o_tags;
    *(uint8_t **)(c + TC_SELF_A) = c; *(uint8_t **)(c + TC_SELF_B) = c;
    for (int i = 0; i < n; i++) {
        MsString *s = (MsString *)(g_tag[i] + TAG_STR); memset(s, 0, sizeof *s);
        s->size = strlen(names[i]); memcpy(s->u.buf, names[i], s->size + 1); s->cap = 15;
    }
    *(uint8_t **)(c + TC_VEC_BEG) = g_tag[0]; *(uint8_t **)(c + TC_VEC_END) = g_tag[n];
}

int main(int argc, char **argv)
{
    int bad = 0;
    if (argc > 1) {
        HMODULE m = LoadLibraryExA(argv[1], NULL, DONT_RESOLVE_DLL_REFERENCES);
        if (!m) { printf("%s: load failed %lu\n", argv[1], GetLastError()); return 1; }
        const char *err = find_all((uint8_t *)m);
        if (err) { printf("%s: FAIL %s\n", argv[1], err); return 1; }
        printf("%s:\n  eSupplyZoneExtender vtable exe+0x%llX message slot %u exe+0x%llX\n  enabled +0x%X owner +0x%X tags +0x%X\n",
               argv[1], OFF(vt_sz), s_msg, OFF(f_msg), o_enabled, o_owner, o_tags);
        if (o_enabled != 0x430 || o_owner != 0x18 || s_msg != 7) { printf("  FAIL offsets differ from 1.069\n"); bad = 1; }
    } else { o_enabled = 0x430; o_owner = 0x18; o_tags = 0x1D0; }

    orig_msg = fake_msg;
    *(uint8_t **)(g_zone + o_owner) = g_ent;
    const char *on[] = { "drop_pod", "supply_off" }, *off[] = { "drop_pod" };
    struct { const char *what; int tag; uint32_t type; uint8_t stored, seen; } st[] = {
        { "untagged tick",              0, MSG_TICK,  1, 1 },
        { "tagged tick runs disabled",  1, MSG_TICK,  1, 0 },
        { "tagged query runs disabled", 1, MSG_QUERY, 1, 0 },
        { "tagged init untouched",      1, 0,         1, 1 },
        { "map-disabled zone stays 0",  1, MSG_TICK,  0, 0 },
        { "tag removed -> enabled",     0, MSG_TICK,  1, 1 },
        { "untagged query",             0, MSG_QUERY, 1, 1 },
    };
    for (size_t i = 0; i < sizeof st / sizeof *st; i++) {
        if (st[i].tag) set_tags(2, on); else set_tags(1, off);
        g_zone[o_enabled] = st[i].stored; g_calls = 0;
        uint32_t msg[4] = { st[i].type, 0, 0, 0 };
        uint64_t r = hk_msg(g_zone, msg, NULL, NULL);
        int ok = g_calls == 1 && r == 7 && g_seen == st[i].seen && g_zone[o_enabled] == st[i].stored;
        printf("  %-28s seen %d stored after %d %s\n", st[i].what, g_seen, g_zone[o_enabled], ok ? "OK" : "FAIL");
        if (!ok) bad = 1;
    }
    *(uint8_t **)(g_zone + o_owner) = NULL;
    g_zone[o_enabled] = 1; { uint32_t msg[4] = { MSG_TICK }; hk_msg(g_zone, msg, NULL, NULL); }
    printf("  %-28s seen %d %s\n", "no owner -> untouched", g_seen, g_seen == 1 ? "OK" : "FAIL");
    if (g_seen != 1) bad = 1;
    printf(bad ? "SOME FAILED\n" : "ALL OK\n");
    return bad;
}
#endif
