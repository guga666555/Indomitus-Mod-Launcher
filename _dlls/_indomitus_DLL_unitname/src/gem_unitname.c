/* GEM UnitName 0.1 - personal names for units that are not humans (Call to Arms: Gates of Hell, GEM 2, 1.068)
 *
 * A soldier's name comes from the localization lists names/<nationality>/first|last, rolled when its breed is
 * applied and kept on the human. Vehicles have no such field: the unit panel's name line (and every other place
 * that builds the same unit description) shows "info/special/<def>" or the vehicle class text.
 *
 * With this dll, a non-human unit whose def has a prop "name_<list>" (ini Prefix=) shows a name built from
 *     names/<list>/first   (title lines, e.g. "Brother\nVenerable Brother\n")
 *     names/<list>/last    (name lines)
 * in that name line instead. The pick is a hash of the unit's entity id (the 0x.... id saved in map / save
 * files), so a unit keeps its name across panel refreshes and save / load, and every MP player sees the same
 * name. Text format in the ini (Format=, default "<c(c3baae)>{first} {last}</c>", the colour vanilla uses for
 * that line).
 *     def:          {props ... "name_sm_dreadnought"}
 *     localization: msgctxt "names/sm_dreadnought/first" / msgctxt "names/sm_dreadnought/last"
 *
 * Engine (game exe 1.068 offsets; found at runtime by signature, editor has the same code; the server has no UI):
 *   UnitDesc ctor exe+0xB47980 (desc, unit, mode): clears the strings, stores mode +0x1D4 / unit +0x1D8, then
 *     call Fill(desc) at exe+0xB47AB1. That call is retargeted: original Fill, then un_fix(desc).
 *   Fill exe+0xB47AD0: desc+0 = Localize("info/special/<def>") if it exists, else the class text
 *     ("entity_info/desc/tank/medium"...); desc+0x20 = the unit display name. GUI2::SceneUI::eGUI_GC_PanelVehicle
 *     shows desc+0 in txt_name and desc+0x20 in txt_cls; 5 other callers (tooltips, unit info) build the same desc.
 *   Localize: Find(char *key) exe+0x485F70 -> entry or NULL; Text(entry) exe+0x506DF0 -> std::string*;
 *     std::string assign(dst, ptr, len) exe+0xA6400 (all three taken from Fill's own info/special lookup).
 *   Entity id: dword at entity+0xC0 (read from the engine's "cbi-%1%" cover-link code, which formats it).
 *   Def props: entity+0x548 -> def, def+0x330 vector of 0x60-byte Tags, name std::string at Tag+0x40.
 *
 * Build: build.bat   Offline finder + logic test: build.bat test
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>

#define UN_VERSION "0.1"
#define UN_SECT    "UnitName"

typedef struct { union { char buf[16]; char *ptr; } u; size_t size, cap; } MsString;

/* ---------------------------------------------------------------- config */
static int    cfg_enabled = 1, cfg_log = 0;
static char   cfg_prefix[32] = "name_";
static size_t cfg_prefixlen = 5;
static char   cfg_format[128] = "<c(c3baae)>{first} {last}</c>";

/* ---------------------------------------------------------------- log */
static char  g_logpath[MAX_PATH];
static FILE *g_log;
static volatile LONG g_lines;
static void logf_(const char *fmt, ...)
{
    if (InterlockedIncrement(&g_lines) > 400) return;
#ifndef UN_TEST
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
static Sec g_text, g_rdata;
static uint8_t *g_base;
#define OFF(a) ((unsigned long long)((uint8_t *)(a) - g_base))

static int get_sections(uint8_t *base)
{
    g_base = base; g_text.p = g_rdata.p = NULL;
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_SECTION_HEADER *sh = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        Sec s = { base + sh[i].VirtualAddress, sh[i].Misc.VirtualSize };
        if (!strncmp((char *)sh[i].Name, ".text", 8)) g_text = s;
        else if (!strncmp((char *)sh[i].Name, ".rdata", 8)) g_rdata = s;
    }
    return g_text.p && g_rdata.p;
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

/* ---------------------------------------------------------------- engine bindings */
#define ENT_KIND   0x30
#define KIND_HUMAN 0x04

typedef void     (*FillFn)(uint8_t *desc);
typedef void    *(*FindFn)(const char *key);
typedef MsString *(*TextFn)(void *entry);
typedef void    *(*AssignFn)(void *dst, const char *p, size_t n);

static uint8_t *g_site;                 /* the call Fill(desc) inside the UnitDesc ctor */
static FillFn   f_fill;
static FindFn   f_find;
static TextFn   f_text;
static AssignFn f_assign;
static uint32_t o_unit, o_uid, o_def, o_props;

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

/* entity id: mov ecx,[rax+ID] ; ... lea rdx,"cbi-%1%" */
static int find_uid(void)
{
    static const char name[] = "\0cbi-%1%";
    uint8_t *str = NULL;
    for (uint8_t *p = g_rdata.p; p + sizeof name <= g_rdata.p + g_rdata.n; p++)
        if (*p == 0 && !memcmp(p, name, sizeof name)) { str = p + 1; break; }
    if (!str) return 0;
    for (uint8_t *p = g_text.p + 8; p + 7 <= g_text.p + g_text.n; p++) {
        if (p[0] != 0x48 || p[1] != 0x8D || p[2] != 0x15 || rel32(p, 7) != str) continue;
        /* game: the load comes before the lea; editor: after "call Format ; nop" */
        for (uint8_t *q = p - 8; q < p + 0x18; q++)
            if (q[0] == 0x8B && (q[1] & 0xC0) == 0x80 && (q[1] & 7) != 4 && (q + 6 <= p || q >= p + 7)) {
                uint32_t d = *(uint32_t *)(q + 2);
                if (d >= 0x40 && d < 0x400) { o_uid = d; return 1; }
            }
    }
    return 0;
}

static const char *find_all(uint8_t *base)
{
    if (!get_sections(base)) return "sections not found";
    int n = 0;
    /* mov [rcx+a],eax ; mov [rcx+mode],r8d ; mov [rcx+unit],rdx ; call Fill */
    uint8_t *c = scan(g_text.p, g_text.n, "89 81 ?? ?? 00 00 44 89 81 ?? ?? 00 00 48 89 91 ?? ?? 00 00 E8", &n);
    if (n == 0) return "unit description constructor not found (unknown build, or already patched)";
    if (n > 1) return "unit description constructor not unique";
    o_unit = *(uint32_t *)(c + 16);
    g_site = c + 20;
    f_fill = (FillFn)rel32(g_site, 5);
    if (!in_text((void *)f_fill)) return "Fill not in .text";
    /* in Fill: call Find ; test rax,rax ; je ; mov rcx,rax ; call Text ; cmp rdi,rax  (rdi = desc) */
    uint8_t *l = scan((uint8_t *)f_fill, 0x400, "E8 ?? ?? ?? ?? 48 85 C0 74 ?? 48 8B C8 E8 ?? ?? ?? ?? 48 3B F8", NULL);
    if (!l) return "localization lookup in Fill not found";
    f_find = (FindFn)rel32(l, 5);
    f_text = (TextFn)rel32(l + 13, 5);
    uint8_t *a = scan(l + 21, 0x30, "4C 8B 40 10 48 8B CF E8", NULL);
    if (!a) return "string assign in Fill not found";
    f_assign = (AssignFn)rel32(a + 7, 5);
    if (!in_text((void *)f_find) || !in_text((void *)f_text) || !in_text((void *)f_assign)) return "call targets not in .text";
    if (!find_uid()) return "entity id offset not found";
    if (!find_props()) return "def props offsets not found";
    if (o_unit < 0x100 || o_unit > 0x400) return "unit offset out of range";
    return NULL;
}

/* ---------------------------------------------------------------- the name */
static const char *ms_cstr(const MsString *s) { return s->cap > 15 ? s->u.ptr : s->u.buf; }

/* the k-th non-empty line of s (k counted modulo the number of lines); 0 = no lines */
static size_t lines(const char *s, size_t n, uint32_t pick, const char **out)
{
    size_t count = 0;
    for (size_t i = 0; i < n; ) {
        size_t j = i; while (j < n && s[j] != '\n') j++;
        size_t e = j; while (e > i && (s[e - 1] == '\r' || s[e - 1] == ' ')) e--;
        if (e > i) count++;
        i = j + 1;
    }
    if (!count) return 0;
    size_t want = pick % count, k = 0;
    for (size_t i = 0; i < n; ) {
        size_t j = i; while (j < n && s[j] != '\n') j++;
        size_t e = j; while (e > i && (s[e - 1] == '\r' || s[e - 1] == ' ')) e--;
        if (e > i) { if (k == want) { *out = s + i; return e - i; } k++; }
        i = j + 1;
    }
    return 0;
}

static uint64_t mix(uint64_t x)
{
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL; x ^= x >> 33;
    return x;
}

static const MsString *loc(const char *list, const char *part)
{
    char key[128];
    snprintf(key, sizeof key, "names/%s/%s", list, part);
    void *e = f_find(key);
    if (!e) return NULL;
    const MsString *s = f_text(e);
    if (!s || s->size == 0 || s->size > 0x10000 || s->cap < s->size) return NULL;
    return s;
}

static volatile LONG g_named, g_missing;

/* builds the name for entity e into out; 0 = the unit gets no name */
static size_t un_name(uint8_t *e, char *out, size_t cap)
{
    if (!e || (e[ENT_KIND] & KIND_HUMAN)) return 0;
    uint8_t *def = *(uint8_t **)(e + o_def);
    if (!def) return 0;
    uint8_t *b = *(uint8_t **)(def + o_props), *end = *(uint8_t **)(def + o_props + 8);
    if (!b || end <= b || (size_t)(end - b) > 0x60 * 4096) return 0;
    char list[64] = "";
    for (uint8_t *t = b; t + 0x60 <= end; t += 0x60) {
        const MsString *s = (const MsString *)(t + 0x40);
        if (s->size <= cfg_prefixlen || s->size >= cfg_prefixlen + sizeof list || s->cap < s->size) continue;
        const char *c = ms_cstr(s);
        if (memcmp(c, cfg_prefix, cfg_prefixlen)) continue;
        memcpy(list, c + cfg_prefixlen, s->size - cfg_prefixlen);
        list[s->size - cfg_prefixlen] = 0;
        break;
    }
    if (!list[0]) return 0;
    const MsString *fs = loc(list, "first"), *ls = loc(list, "last");
    if (!ls) {
        if (InterlockedIncrement(&g_missing) <= 20) logf_("no localization text names/%s/last (unit id 0x%X)", list, *(uint32_t *)(e + o_uid));
        return 0;
    }
    uint32_t uid = *(uint32_t *)(e + o_uid);
    uint64_t lh = 1469598103934665603ULL;
    for (const char *p = list; *p; p++) lh = (lh ^ (uint8_t)*p) * 1099511628211ULL;
    uint64_t h = mix(((uint64_t)uid << 1 | 1) ^ lh);
    const char *f = "", *l = "";
    size_t fn = fs ? lines(ms_cstr(fs), fs->size, (uint32_t)h, &f) : 0;
    size_t ln = lines(ms_cstr(ls), ls->size, (uint32_t)(h >> 32), &l);
    if (!ln) return 0;
    /* format: {first} / {last} replaced; a missing first drops the space that follows {first} */
    size_t o = 0;
    for (const char *p = cfg_format; *p && o + 1 < cap; ) {
        if (!strncmp(p, "{first}", 7)) {
            if (fn && o + fn < cap) { memcpy(out + o, f, fn); o += fn; }
            p += 7;
            if (!fn && *p == ' ') p++;
        } else if (!strncmp(p, "{last}", 6)) {
            if (o + ln < cap) { memcpy(out + o, l, ln); o += ln; }
            p += 6;
        } else out[o++] = *p++;
    }
    out[o] = 0;
    return o;
}

__attribute__((used)) void un_stub(uint8_t *desc)
{
    f_fill(desc);
    if (!cfg_enabled) return;
    char buf[256];
    size_t n = un_name(*(uint8_t **)(desc + o_unit), buf, sizeof buf);
    if (!n) return;
    f_assign(desc, buf, n);
    LONG k = InterlockedIncrement(&g_named);
    if (cfg_log && k <= 50) logf_("named unit id 0x%X: %s", *(uint32_t *)(*(uint8_t **)(desc + o_unit) + o_uid), buf);
}

/* ---------------------------------------------------------------- config */
static void load_ini(const char *ini)
{
    char buf[128];
    cfg_enabled = GetPrivateProfileIntA(UN_SECT, "Enabled", 1, ini);
    cfg_log     = GetPrivateProfileIntA(UN_SECT, "Log", 0, ini);
    GetPrivateProfileStringA(UN_SECT, "Prefix", cfg_prefix, buf, sizeof buf, ini);
    size_t n = strlen(buf);
    while (n && (buf[n - 1] == ' ' || buf[n - 1] == '\t')) buf[--n] = 0;
    if (n >= 2 && n < sizeof cfg_prefix) {
        for (size_t i = 0; i < n; i++) buf[i] = (char)tolower((unsigned char)buf[i]);
        strcpy(cfg_prefix, buf);
    }
    cfg_prefixlen = strlen(cfg_prefix);
    GetPrivateProfileStringA(UN_SECT, "Format", cfg_format, buf, sizeof buf, ini);
    if (strstr(buf, "{last}")) strcpy(cfg_format, buf);
}

static uint8_t *alloc_near(uint8_t *lo, uint8_t *hi)
{
    SYSTEM_INFO si; GetSystemInfo(&si);
    uintptr_t g = si.dwAllocationGranularity, reach = 0x70000000;
    uintptr_t from = (uintptr_t)hi > reach ? ((uintptr_t)hi - reach + g) & ~(g - 1) : g;
    uintptr_t to = (uintptr_t)lo + reach;
    for (uintptr_t a = (uintptr_t)lo & ~(g - 1); a >= from && a >= g; a -= g) {
        void *p = VirtualAlloc((void *)a, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        if (p) return p;
    }
    for (uintptr_t a = ((uintptr_t)hi + g) & ~(g - 1); a < to; a += g) {
        void *p = VirtualAlloc((void *)a, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        if (p) return p;
    }
    return NULL;
}

static const char *patch_site(void)
{
    uint8_t *page = alloc_near(g_text.p, g_text.p + g_text.n);
    if (!page) return "no memory near the exe";
    page[0] = 0xFF; page[1] = 0x25; *(uint32_t *)(page + 2) = 0;
    *(uint64_t *)(page + 6) = (uint64_t)(uintptr_t)un_stub;
    DWORD old;
    VirtualProtect(page, 0x1000, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), page, 0x1000);
    int64_t d = (int64_t)(page - (g_site + 5));
    if (d > 0x7FFFFFFFLL || d < -0x7FFFFFFFLL) return "stub page out of reach";
    if (!VirtualProtect(g_site + 1, 4, PAGE_EXECUTE_READWRITE, &old)) return "VirtualProtect failed";
    *(int32_t *)(g_site + 1) = (int32_t)d;
    VirtualProtect(g_site + 1, 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), g_site, 5);
    return NULL;
}

#ifndef UN_TEST
static void install(void)
{
    const char *err = find_all((uint8_t *)GetModuleHandleW(NULL));
    if (err) { logf_("[GEM UnitName %s] NOT installed: %s", UN_VERSION, err); return; }
    if (!cfg_enabled) { logf_("[GEM UnitName %s] Enabled=0: not installed", UN_VERSION); return; }
    if ((err = patch_site())) { logf_("[GEM UnitName %s] NOT installed: %s (%lu)", UN_VERSION, err, GetLastError()); return; }
    logf_("[GEM UnitName %s] installed | call at exe+0x%llX (Fill exe+0x%llX), Find exe+0x%llX Text exe+0x%llX Assign exe+0x%llX | "
          "desc unit +0x%X, entity id +0x%X, def +0x%X props +0x%X | Prefix=%s Format=%s Log=%d",
          UN_VERSION, OFF(g_site), OFF(f_fill), OFF(f_find), OFF(f_text), OFF(f_assign), o_unit, o_uid, o_def, o_props,
          cfg_prefix, cfg_format, cfg_log);
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
        if (_strnicmp(nm, "call_to_arms", 12) || strstr(nm, "_server")) return TRUE;     /* the server shows no UI */
        snprintf(ini, sizeof ini, "%s.ini", path);
        snprintf(g_logpath, sizeof g_logpath, "%s%s.log", path, strstr(nm, "_ed") ? "_editor" : "");
        load_ini(ini);
        install();
    }
    return TRUE;
}
#else
/* ---------------------------------------------------------------- offline test */
static __attribute__((aligned(16))) uint8_t t_ent[0x2000], t_def[0x1000], t_props[3][0x60], t_desc[0x200];
static MsString t_first, t_last;
static const char *t_first_txt = "Brother\nVenerable Brother\n", *t_last_txt = "Varro\nCassius\r\n\nTitus\nOrestes\n";
static int t_have_first = 1, t_have_last = 1, g_fills, g_assigns;
static char g_assigned[256];
static void set_tag(uint8_t *t, const char *name)
{
    MsString *s = (MsString *)(t + 0x40);
    memset(t, 0, 0x60);
    s->size = strlen(name);
    if (s->size < 16) { memcpy(s->u.buf, name, s->size + 1); s->cap = 15; }
    else { s->u.ptr = (char *)name; s->cap = 63; }
}
static void *fake_find(const char *key)
{
    if (!strcmp(key, "names/sm_dreadnought/first")) return t_have_first ? (void *)&t_first : NULL;
    if (!strcmp(key, "names/sm_dreadnought/last")) return t_have_last ? (void *)&t_last : NULL;
    return NULL;
}
static MsString *fake_text(void *e) { return (MsString *)e; }
static void *fake_assign(void *dst, const char *p, size_t n) { g_assigns++; memcpy(g_assigned, p, n); g_assigned[n] = 0; return dst; }
static void fake_fill(uint8_t *desc) { (void)desc; g_fills++; }

int main(int argc, char **argv)
{
    int bad = 0;
    for (int a = 1; a < argc; a++) {
        HMODULE m = LoadLibraryExA(argv[a], NULL, DONT_RESOLVE_DLL_REFERENCES);
        if (!m) { printf("%s: load failed %lu\n", argv[a], GetLastError()); bad = 1; continue; }
        const char *err = find_all((uint8_t *)m);
        if (err) { printf("%s: FAIL %s\n", argv[a], err); bad = 1; continue; }
        printf("%s:\n  call at exe+0x%llX, Fill exe+0x%llX, Find exe+0x%llX, Text exe+0x%llX, Assign exe+0x%llX\n"
               "  desc unit +0x%X, entity id +0x%X, def +0x%X, props +0x%X\n",
               argv[a], OFF(g_site), OFF(f_fill), OFF(f_find), OFF(f_text), OFF(f_assign), o_unit, o_uid, o_def, o_props);
        if (o_unit != 0x1D8 || o_uid != 0xC0 || o_def != 0x548 || o_props != 0x330) { printf("  FAIL values differ from the ones read by hand on 1.068\n"); bad = 1; }
        const char *pe = patch_site();
        uint8_t *tgt = rel32(g_site, 5);
        int ok = !pe && tgt[0] == 0xFF && tgt[1] == 0x25 && *(void **)(tgt + 6) == (void *)un_stub;
        printf("  patch on the mapped image: %s\n", ok ? "OK" : (pe ? pe : "FAIL"));
        if (!ok) bad = 1;
    }
    o_unit = 0x1D8; o_uid = 0xC0; o_def = 0x548; o_props = 0x330; cfg_log = 1;
    f_fill = fake_fill; f_find = fake_find; f_text = fake_text; f_assign = fake_assign;
    t_first.size = strlen(t_first_txt); t_first.cap = 63; t_first.u.ptr = (char *)t_first_txt;
    t_last.size = strlen(t_last_txt); t_last.cap = 63; t_last.u.ptr = (char *)t_last_txt;
    *(uint8_t **)(t_desc + o_unit) = t_ent;
    *(uint8_t **)(t_ent + o_def) = t_def;
    set_tag(t_props[0], "tank"); set_tag(t_props[1], "name_sm_dreadnought"); set_tag(t_props[2], "voice_sm_dreadnought");
    *(uint8_t **)(t_def + o_props) = t_props[0]; *(uint8_t **)(t_def + o_props + 8) = t_props[0] + 0x60 * 3;
    struct { const char *what; uint8_t kind; uint32_t uid; int first, last, nprops, want; } st[] = {
        { "dreadnought id 0x802F",            0x0B, 0x802F, 1, 1, 3, 1 },
        { "dreadnought id 0x8030",            0x0B, 0x8030, 1, 1, 3, 1 },
        { "same id again: same name",         0x0B, 0x802F, 1, 1, 3, 1 },
        { "no first list: last name only",    0x0B, 0x802F, 0, 1, 3, 1 },
        { "no last list: untouched",          0x0B, 0x802F, 1, 0, 3, 0 },
        { "human: untouched",                 0x07, 0x802F, 1, 1, 3, 0 },
        { "no name_ prop: untouched",         0x0B, 0x802F, 1, 1, 1, 0 },
    };
    char first_name[256] = "";
    for (size_t i = 0; i < sizeof st / sizeof *st; i++) {
        t_ent[ENT_KIND] = st[i].kind; *(uint32_t *)(t_ent + o_uid) = st[i].uid;
        t_have_first = st[i].first; t_have_last = st[i].last;
        *(uint8_t **)(t_def + o_props + 8) = t_props[0] + 0x60 * st[i].nprops;
        g_fills = g_assigns = 0; g_assigned[0] = 0;
        un_stub(t_desc);
        int ok = g_fills == 1 && g_assigns == st[i].want;
        if (i == 0) strcpy(first_name, g_assigned);
        if (i == 2) ok = ok && !strcmp(first_name, g_assigned);
        if (i == 3) ok = ok && !strstr(g_assigned, "Brother") && !strstr(g_assigned, "> ");
        if (st[i].want) ok = ok && strstr(g_assigned, "<c(c3baae)>") && strstr(g_assigned, "</c>") && !strchr(g_assigned, '\r') && !strchr(g_assigned, '\n');
        printf("  %-34s \"%s\" %s\n", st[i].what, g_assigned, ok ? "OK" : "FAIL");
        if (!ok) bad = 1;
    }
    /* spread over 64 ids */
    t_ent[ENT_KIND] = 0x0B; t_have_first = t_have_last = 1; *(uint8_t **)(t_def + o_props + 8) = t_props[0] + 0x60 * 3;
    int seen[4] = { 0 };
    for (uint32_t id = 0x8000; id < 0x8040; id++) {
        *(uint32_t *)(t_ent + o_uid) = id; un_stub(t_desc);
        if (strstr(g_assigned, "Varro")) seen[0]++; else if (strstr(g_assigned, "Cassius")) seen[1]++;
        else if (strstr(g_assigned, "Titus")) seen[2]++; else if (strstr(g_assigned, "Orestes")) seen[3]++;
    }
    int ok = seen[0] && seen[1] && seen[2] && seen[3] && seen[0] + seen[1] + seen[2] + seen[3] == 64;
    printf("  64 ids over 4 names: %d %d %d %d %s\n", seen[0], seen[1], seen[2], seen[3], ok ? "OK" : "FAIL");
    if (!ok) bad = 1;
    printf(bad ? "SOME FAILED\n" : "ALL OK\n");
    return bad;
}
#endif
