/* GEM CrewVoice 0.1 - engine voice lines for vehicles with no crew (Call to Arms: Gates of Hell, GEM 2, 1.067)
 *
 * A vehicle speaks with the language (breed nationality) of its first crew member. A crewless unit
 * (walker, drone, daemon engine) has nobody to ask, so the engine's say function finds an empty
 * language and returns: the unit is silent, although the engine raises every talk moment on it and
 * runs its idle / in-battle timer (live reads, Nov 14 2026, _notes\dll_hooks.md).
 *
 * This dll fills that one gap. When the engine's language lookup for a NON-human entity comes back
 * empty and the entity's def has a prop that starts with "voice_" (ini Prefix=), that prop is used as
 * the language. So:
 *     def:       {props "voice_sm_dreadnought"}
 *     talk.set:  ("phrase_voice_as" args %0 voice_sm_dreadnought 1 %1 sm_dreadnought)   in "phrases_tank"
 *     sounds:    sound\talk\tank\sm_dreadnought\<event folder>\v1\*.wav
 * A unit with a crew keeps its crew's language (the prop is a fallback, not an override). Humans are
 * never touched. Script {talk "x"} on the unit goes through the same function, so it works too.
 *
 * Engine (found at runtime by signature; 1.067 addresses for reference):
 *   eTalkExtender::Say(ext, name): game exe+0x91BDF0, server exe+0x730FE0, editor exe+0x7A30B0.
 *   Language Tag = local at [rbp+d]; "empty" = its string size (Tag+0x50) is 0, tested right after.
 *   game / server: the lookup is inlined; the no-crew path ends in  lea rdx,[empty Tag] ; lea rcx,[dst] ;
 *     call TagCopy(dst, src)   (game exe+0x91BEB4, server exe+0x7310A4). That call is retargeted.
 *   editor: one  call LangOf(dst, entity)  (exe+0x7A30E8). That call is retargeted; TagCopy is taken
 *     from LangOf's own no-crew path.
 *   r15 = the extender at all three sites; entity = [ext+0x18], def = [entity+0x548], props = vector of
 *   0x60-byte Tags at def+0x330 / +0x338, name std::string at Tag+0x40 (all offsets read from the code
 *   that follows the site). A Tag holds pointers into itself, so it is only ever copied with the
 *   engine's own TagCopy (copy constructor); an empty Tag owns no heap memory, so constructing over it
 *   leaks nothing.
 * The stub calls the original target, then cv_fix(dst, extender).
 *
 * MP: Say only plays a sound (the engine saves and restores its random state around it). Every player
 * who wants to hear the lines needs the dll; the server is patched too so all machines run the same code
 * (ini Server=0 leaves it alone).
 *
 * Build: build.bat   Offline finder + logic + engine-call test: build.bat test
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>

#define CV_VERSION "0.1"
#define CV_SECT    "CrewVoice"

typedef struct { union { char buf[16]; char *ptr; } u; size_t size, cap; } MsString;

/* ---------------------------------------------------------------- config */
static int    cfg_enabled = 1, cfg_log = 0, cfg_server = 1;
static char   cfg_prefix[32] = "voice_";
static size_t cfg_prefixlen = 6;

/* ---------------------------------------------------------------- log */
static char  g_logpath[MAX_PATH];
static FILE *g_log;
static int   g_lines;
static void logf_(const char *fmt, ...)
{
    if (g_lines > 2000) return;
#ifndef CV_TEST
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
static Sec g_text;
static uint8_t *g_base;

static int get_sections(uint8_t *base)
{
    g_base = base; g_text.p = NULL;
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_SECTION_HEADER *sh = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++)
        if (!strncmp((char *)sh[i].Name, ".text", 8)) { g_text.p = base + sh[i].VirtualAddress; g_text.n = sh[i].Misc.VirtualSize; }
    return g_text.p != NULL;
}
static int in_text(const void *a) { return (uint8_t *)a >= g_text.p && (uint8_t *)a < g_text.p + g_text.n; }
#define OFF(a) ((unsigned long long)((uint8_t *)(a) - g_base))
static uint8_t *rel32(uint8_t *at, int len) { return at + len + *(int32_t *)(at + len - 4); }

/* "48 8D 4D ?? E8" -> bytes + mask; returns the length */
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
/* first match of pat in [p, p+n); *count = number of matches when asked */
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
typedef void *(*CopyFn)(void *dst, const void *src);        /* Tag copy constructor, returns dst */

#define ENT_KIND   0x30                                     /* byte; bit 0x04 = human */
#define KIND_HUMAN 0x04

static uint8_t *g_site;                                     /* the E8 call that gets retargeted */
static int      g_inline;                                   /* 1 = game / server layout, 0 = editor layout */
static CopyFn   f_copy;
static uint32_t o_ent, o_def, o_pbeg, o_pend, o_tag, o_size;   /* ext->entity, entity->def, def props, Tag size, Tag string size */

void *cv_orig;                                              /* original target of the retargeted call (used by the asm stub) */

static const char *find_all(uint8_t *base)
{
    g_site = NULL; f_copy = NULL; cv_orig = NULL;
    o_ent = o_def = o_pbeg = o_pend = o_tag = o_size = 0;
    if (!get_sections(base)) return "sections not found";

    int na = 0, nb = 0, dst = 0;
    uint8_t *after = NULL;
    /* game / server: mov rdx,r12 ; lea rcx,[rbp+d] ; call LangOf ; jmp +0x10 ; lea rdx,[empty] ; lea rcx,[rbp+d] ; call TagCopy */
    uint8_t *a = scan(g_text.p, g_text.n, "49 8B D4 48 8D 4D ?? E8 ?? ?? ?? ?? EB 10 48 8D 15 ?? ?? ?? ?? 48 8D 4D ?? E8", &na);
    /* editor: mov r15,rcx ; xor r12d,r12d ; mov rdx,[rcx+e] ; lea rcx,[rbp+d] ; call LangOf ; nop ; cmp [rbp+s],r12 */
    uint8_t *b = scan(g_text.p, g_text.n, "4C 8B F9 45 33 E4 48 8B 51 ?? 48 8D 4D ?? E8 ?? ?? ?? ?? 90 4C 39 65", &nb);
    if (na + nb == 0) return "say function's language lookup not found";
    if (na + nb != 1) return "say function's language lookup ambiguous";

    if (na) {
        if (a[6] != a[24]) return "language Tag locals differ";
        g_inline = 1; g_site = a + 25; dst = (int8_t)a[6]; after = a + 30;
        f_copy = (CopyFn)rel32(g_site, 5);
        if (!scan(a - 0x90, 0x90, "F6 41 30 04", NULL)) return "human test not recognised";
    } else {
        g_inline = 0; g_site = b + 14; dst = (int8_t)b[13]; after = b + 19;
        uint8_t *lang = rel32(g_site, 5);
        if (!in_text(lang)) return "LangOf not in .text";
        if (!scan(lang, 0x30, "F6 42 30 04", NULL)) return "human test not recognised";
        /* its no-crew path: lea rdx,[empty] ; call TagCopy ; mov rax,rbx */
        int nc = 0;
        uint8_t *c = scan(lang, 0x180, "48 8D 15 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B C3", &nc);
        if (nc != 1) return "TagCopy call in LangOf not recognised";
        f_copy = (CopyFn)rel32(c + 7, 5);
    }
    cv_orig = rel32(g_site, 5);
    if (!in_text(cv_orig) || !in_text(f_copy)) return "call targets not in .text";
    /* TagCopy builds a string with a 0x1F-byte inline buffer and capacity 0xF */
    if (!scan((uint8_t *)f_copy, 0x70, "1F 00 00 00", NULL) || !scan((uint8_t *)f_copy, 0x70, "0F 00 00 00", NULL))
        return "TagCopy body not recognised";

    /* the empty test that follows: cmp [rbp+s],r12 -> string size offset inside the Tag */
    uint8_t *c = scan(after, 0x40, "4C 39 65", NULL);
    if (!c) return "empty-language test not found";
    o_size = (uint32_t)((int8_t)c[3] - dst);
    /* then the def props loop: mov rax,[r15+e] ; mov rcx,[rax+def] ; mov rdi,[rcx+end] ; mov rbx,[rcx+beg] ... add rbx,size */
    uint8_t *p = scan(after, 0xA0, "49 8B 47 ?? 48 8B 88 ?? ?? ?? ?? 48 8B B9 ?? ?? ?? ?? 48 8B 99", NULL);
    if (!p) return "def props loop not found";
    o_ent = p[3]; o_def = *(uint32_t *)(p + 7); o_pend = *(uint32_t *)(p + 14); o_pbeg = *(uint32_t *)(p + 21);
    uint8_t *s = scan(p + 25, 0x40, "48 83 C3 ?? 48 3B DF", NULL);
    if (!s) return "Tag size not found";
    o_tag = s[3];
    if (!g_inline && b[9] != o_ent) return "extender entity offsets differ";
    if (o_size < 0x20 || o_size >= o_tag || o_tag > 0x100 || o_def < 0x100 || o_def > 0x2000 ||
        o_pbeg < 0x100 || o_pbeg > 0x1000 || o_pend != o_pbeg + 8 || o_ent > 0x80)
        return "offsets out of range";
    return NULL;
}

/* ---------------------------------------------------------------- the fallback */
static const char *ms_cstr(const MsString *s) { return s->cap > 15 ? s->u.ptr : s->u.buf; }
static volatile LONG g_fills;

/* dst = the language Tag the engine just built for this say call, ext = its talk extender */
__attribute__((used)) void *cv_fix(uint8_t *dst, uint8_t *ext)
{
    if (*(size_t *)(dst + o_size)) return dst;                       /* the engine found a language: crew or human */
    uint8_t *e = ext ? *(uint8_t **)(ext + o_ent) : NULL;
    if (!e || (e[ENT_KIND] & KIND_HUMAN)) return dst;
    uint8_t *def = *(uint8_t **)(e + o_def);
    if (!def) return dst;
    uint8_t *b = *(uint8_t **)(def + o_pbeg), *end = *(uint8_t **)(def + o_pend);
    if (!b || end < b || (size_t)(end - b) > (size_t)4096 * o_tag) return dst;
    for (uint8_t *t = b; t + o_tag <= end; t += o_tag) {
        const MsString *s = (const MsString *)(t + o_size - 0x10);
        if (s->size <= cfg_prefixlen || s->size > 255 || memcmp(ms_cstr(s), cfg_prefix, cfg_prefixlen)) continue;
        f_copy(dst, t);
        LONG n = InterlockedIncrement(&g_fills);
        if (cfg_log && n <= 300) logf_("voice: entity %p speaks as \"%.*s\" (%ld)", (void *)e, (int)s->size, ms_cstr(s), n);
        return dst;
    }
    return dst;
}

/* cv_stub(dst, arg2) with r15 = extender: the original call, then cv_fix(dst, r15). Returns dst like both originals. */
__asm__(
    ".text\n"
    ".globl cv_stub\n"
    ".def cv_stub; .scl 2; .type 32; .endef\n"
    ".seh_proc cv_stub\n"
    "cv_stub:\n"
    "    push %rbx\n"
    "    .seh_pushreg %rbx\n"
    "    sub $0x20, %rsp\n"
    "    .seh_stackalloc 0x20\n"
    "    .seh_endprologue\n"
    "    mov %rcx, %rbx\n"
    "    call *cv_orig(%rip)\n"
    "    mov %rbx, %rcx\n"
    "    mov %r15, %rdx\n"
    "    call cv_fix\n"
    "    add $0x20, %rsp\n"
    "    pop %rbx\n"
    "    ret\n"
    ".seh_endproc\n");
extern void cv_stub(void);

/* ---------------------------------------------------------------- config */
static void load_ini(const char *ini)
{
    char buf[64];
    cfg_enabled = GetPrivateProfileIntA(CV_SECT, "Enabled", 1, ini);
    cfg_log     = GetPrivateProfileIntA(CV_SECT, "Log", 0, ini);
    cfg_server  = GetPrivateProfileIntA(CV_SECT, "Server", 1, ini);
    GetPrivateProfileStringA(CV_SECT, "Prefix", cfg_prefix, buf, sizeof buf, ini);
    size_t n = strlen(buf);
    while (n && (buf[n - 1] == ' ' || buf[n - 1] == '\t')) buf[--n] = 0;
    if (n >= 2 && n < sizeof cfg_prefix) {
        for (size_t i = 0; i < n; i++) buf[i] = (char)tolower((unsigned char)buf[i]);     /* def props are lowercase */
        strcpy(cfg_prefix, buf);
    }
    cfg_prefixlen = strlen(cfg_prefix);
}

/* executable page within rel32 reach of the exe's code (as GEM CostRed) */
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

/* retarget the call found by find_all to cv_stub; NULL = done */
static const char *patch_site(void)
{
    /* jmp [rip+0] ; dq cv_stub  on a page the call's rel32 can reach */
    uint8_t *page = alloc_near(g_text.p, g_text.p + g_text.n);
    if (!page) return "no memory near the exe";
    page[0] = 0xFF; page[1] = 0x25; *(uint32_t *)(page + 2) = 0;
    *(uint64_t *)(page + 6) = (uint64_t)(uintptr_t)cv_stub;
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

#ifndef CV_TEST
static void install(int is_server)
{
    const char *err = find_all((uint8_t *)GetModuleHandleW(NULL));
    if (err) { logf_("[GEM CrewVoice %s] NOT installed: %s", CV_VERSION, err); return; }
    if (!cfg_enabled) { logf_("[GEM CrewVoice %s] Enabled=0: not installed", CV_VERSION); return; }
    if (is_server && !cfg_server) { logf_("[GEM CrewVoice %s] Server=0: not installed in the server", CV_VERSION); return; }
    if ((err = patch_site())) { logf_("[GEM CrewVoice %s] NOT installed: %s (%lu)", CV_VERSION, err, GetLastError()); return; }
    logf_("[GEM CrewVoice %s] installed | %s layout, call at exe+0x%llX (was exe+0x%llX), TagCopy exe+0x%llX | "
          "ext->entity +0x%X entity->def +0x%X props +0x%X Tag 0x%X name size +0x%X | Prefix=%s Log=%d",
          CV_VERSION, g_inline ? "inline (game / server)" : "call (editor)", OFF(g_site), OFF(cv_orig), OFF(f_copy),
          o_ent, o_def, o_pbeg, o_tag, o_size, cfg_prefix, cfg_log);
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
/* build.bat test -> cv_test.exe <exe>: the finders on that mapped exe, the engine's own TagCopy called on a
   hand-made Tag, then the fallback logic and the asm stub on fake objects. */
static int g_copies; static const uint8_t *g_copied;
static void *fake_copy(void *dst, const void *src)
{
    g_copies++; g_copied = src;
    memcpy(dst, src, 0x60);                                              /* enough for the test: short names only */
    return dst;
}
static int g_origs;
static void *fake_orig(void *dst, const void *src) { (void)src; g_origs++; memset(dst, 0, 0x60); return dst; }

/* cv_call(dst, src, ext): what the engine's call site does, with r15 = ext */
__asm__(
    ".text\n"
    ".globl cv_call\n"
    "cv_call:\n"
    "    push %r15\n"
    "    mov %r8, %r15\n"
    "    sub $0x20, %rsp\n"
    "    call cv_stub\n"
    "    add $0x20, %rsp\n"
    "    pop %r15\n"
    "    ret\n");
extern void *cv_call(void *dst, const void *src, void *ext);
/* cv_callp(dst, arg2, ext, target): the same through any target (the patched site's) */
__asm__(
    ".text\n"
    ".globl cv_callp\n"
    "cv_callp:\n"
    "    push %r15\n"
    "    mov %r8, %r15\n"
    "    sub $0x20, %rsp\n"
    "    call *%r9\n"
    "    add $0x20, %rsp\n"
    "    pop %r15\n"
    "    ret\n");
extern void *cv_callp(void *dst, const void *arg2, void *ext, void *target);

static __attribute__((aligned(16))) uint8_t t_ext[0x200], t_ent[0x2000], t_def[0x1000], t_props[4][0x60], t_dst[0x60], t_src[0x60];
static char t_long[64] = "voice_a_rather_long_nationality_name_here";

static void set_prop(int i, const char *name)
{
    MsString *s = (MsString *)(t_props[i] + o_size - 0x10);
    memset(t_props[i], 0, 0x60); *(uint64_t *)t_props[i] = 0x1000 + i;
    s->size = strlen(name);
    if (s->size < 16) { memcpy(s->u.buf, name, s->size + 1); s->cap = 15; }
    else { s->u.ptr = (char *)name; s->cap = 63; }
}
static void set_props(int n) { *(uint8_t **)(t_def + o_pbeg) = t_props[0]; *(uint8_t **)(t_def + o_pend) = t_props[0] + n * (size_t)o_tag; }

static LONG WINAPI crash(EXCEPTION_POINTERS *x)
{
    printf("  FAIL exception %08lX at %p inside the engine call\n", x->ExceptionRecord->ExceptionCode, x->ExceptionRecord->ExceptionAddress);
    fflush(stdout); ExitProcess(3);
}

int main(int argc, char **argv)
{
    int bad = 0;
    if (argc > 1) {
        HMODULE m = LoadLibraryExA(argv[1], NULL, DONT_RESOLVE_DLL_REFERENCES);
        if (!m) { printf("%s: load failed %lu\n", argv[1], GetLastError()); return 1; }
        const char *err = find_all((uint8_t *)m);
        if (err) { printf("%s: FAIL %s\n", argv[1], err); return 1; }
        printf("%s:\n  %s layout, call at exe+0x%llX -> exe+0x%llX, TagCopy exe+0x%llX\n"
               "  ext->entity +0x%X entity->def +0x%X props +0x%X/+0x%X Tag 0x%X name size +0x%X\n",
               argv[1], g_inline ? "inline" : "call", OFF(g_site), OFF(cv_orig), OFF(f_copy), o_ent, o_def, o_pbeg, o_pend, o_tag, o_size);
        if (o_ent != 0x18 || o_def != 0x548 || o_pbeg != 0x330 || o_tag != 0x60 || o_size != 0x50) { printf("  FAIL offsets differ from 1.067\n"); bad = 1; }
        /* the engine's TagCopy on a hand-made source Tag: id + short name */
        AddVectoredExceptionHandler(1, crash);
        memset(t_src, 0, sizeof t_src); memset(t_dst, 0xCC, sizeof t_dst);
        *(uint64_t *)t_src = 0x1122334455667788ULL;
        MsString *ss = (MsString *)(t_src + o_size - 0x10);
        strcpy(ss->u.buf, "voice_test"); ss->size = 10; ss->cap = 15;
        void *r = f_copy(t_dst, t_src);
        const MsString *ds = (const MsString *)(t_dst + o_size - 0x10);
        int ok = r == t_dst && *(uint64_t *)t_dst == 0x1122334455667788ULL && ds->size == 10 && !memcmp(ms_cstr(ds), "voice_test", 11);
        printf("  engine TagCopy: returns dst %d, id copied %d, name \"%.*s\" size %llu cap %llu %s\n", r == t_dst,
               *(uint64_t *)t_dst == 0x1122334455667788ULL, (int)(ds->size < 40 ? ds->size : 0), ms_cstr(ds),
               (unsigned long long)ds->size, (unsigned long long)ds->cap, ok ? "OK" : "FAIL");
        if (!ok) bad = 1;
        /* a second copy over the first (what cv_fix does over the engine's empty Tag) must give the same */
        memset(t_src, 0, sizeof t_src);
        r = f_copy(t_dst, t_src);
        ok = r == t_dst && ds->size == 0;
        printf("  engine TagCopy of an empty Tag: size %llu %s\n", (unsigned long long)ds->size, ok ? "OK" : "FAIL");
        if (!ok) bad = 1;
        /* the real patch on this mapped image, then a call through the patched site's new target with the engine's
           own functions behind it: game / server = TagCopy(dst, empty Tag), editor = LangOf(dst, entity with no crew) */
        const char *pe = patch_site();
        if (pe) { printf("  FAIL patch: %s\n", pe); return 1; }
        uint8_t *tgt = rel32(g_site, 5);
        int jmp_ok = tgt[0] == 0xFF && tgt[1] == 0x25 && *(uint32_t *)(tgt + 2) == 0 && *(void **)(tgt + 6) == (void *)cv_stub;
        *(uint8_t **)(t_ext + o_ent) = t_ent; *(uint8_t **)(t_ent + o_def) = t_def;
        memset(t_ent, 0, 0x1000); *(uint8_t **)(t_ent + o_def) = t_def; t_ent[ENT_KIND] = 0x0B;      /* vehicle, operated flag clear */
        set_prop(0, "vehicle"); set_prop(1, "tank"); set_prop(2, "voice_sm_dreadnought"); set_prop(3, "crewless");
        memset(t_src, 0, sizeof t_src);
        for (int pass = 0; pass < 2; pass++) {
            set_props(pass ? 2 : 4);                                         /* pass 1: no voice prop in reach */
            memset(t_dst, 0xCC, sizeof t_dst);
            r = cv_callp(t_dst, g_inline ? (void *)t_src : (void *)t_ent, t_ext, tgt);
            if (pass == 0) ok = jmp_ok && r == t_dst && *(uint64_t *)t_dst == 0x1002 && ds->size == 20 && !memcmp(ms_cstr(ds), "voice_sm_dreadnought", 21);
            else           ok = r == t_dst && ds->size == 0;
            printf("  patched call, %s: language \"%.*s\" %s\n", pass ? "unit without the prop" : "unit with the prop   ",
                   (int)(ds->size < 40 ? ds->size : 0), ms_cstr(ds), ok ? "OK" : "FAIL");
            if (!ok) bad = 1;
        }
    } else { o_ent = 0x18; o_def = 0x548; o_pbeg = 0x330; o_pend = 0x338; o_tag = 0x60; o_size = 0x50; }

    f_copy = fake_copy; cv_orig = (void *)fake_orig;
    *(uint8_t **)(t_ext + o_ent) = t_ent; *(uint8_t **)(t_ent + o_def) = t_def;
    set_prop(0, "vehicle"); set_prop(1, "tank"); set_prop(2, "voice_sm_dreadnought"); set_prop(3, "crewless");
    struct { const char *what; int nonempty, noent, human, nprops, want; const char *p2; } st[] = {
        { "crew gave a language: untouched",       1, 0, 0, 4, 0, NULL },
        { "extender with no entity: untouched",    0, 1, 0, 4, 0, NULL },
        { "human with no language: untouched",     0, 0, 1, 4, 0, NULL },
        { "vehicle, no voice prop: silent",        0, 0, 0, 2, 0, NULL },
        { "vehicle with voice prop: filled",       0, 0, 0, 4, 1, NULL },
        { "bare prefix prop: ignored",             0, 0, 0, 4, 0, "voice_" },
        { "long name (heap string): filled",       0, 0, 0, 4, 1, t_long },
        { "no props at all: silent",               0, 0, 0, 0, 0, NULL },
    };
    for (size_t i = 0; i < sizeof st / sizeof *st; i++) {
        set_prop(2, st[i].p2 ? st[i].p2 : "voice_sm_dreadnought");
        set_props(st[i].nprops);
        *(uint8_t **)(t_ext + o_ent) = st[i].noent ? NULL : t_ent;
        t_ent[ENT_KIND] = st[i].human ? 0x07 : 0x0B;
        memset(t_dst, 0, sizeof t_dst);
        if (st[i].nonempty) *(size_t *)(t_dst + o_size) = 2;
        g_copies = 0; g_copied = NULL;
        void *r = cv_fix(t_dst, t_ext);
        int ok = r == t_dst && g_copies == st[i].want && (!st[i].want || g_copied == t_props[2]);
        printf("  %-38s copies %d %s\n", st[i].what, g_copies, ok ? "OK" : "FAIL");
        if (!ok) bad = 1;
    }
    /* the asm stub: original called once, dst handed on, r15 used as the extender, dst returned */
    set_prop(2, "voice_sm_dreadnought"); set_props(4);
    *(uint8_t **)(t_ext + o_ent) = t_ent; t_ent[ENT_KIND] = 0x0B;
    memset(t_dst, 0xCC, sizeof t_dst); g_copies = g_origs = 0;
    void *r = cv_call(t_dst, t_src, t_ext);
    int ok = r == t_dst && g_origs == 1 && g_copies == 1 && g_copied == t_props[2];
    printf("  %-38s orig %d copies %d %s\n", "asm stub (r15 = extender)", g_origs, g_copies, ok ? "OK" : "FAIL");
    if (!ok) bad = 1;
    printf(bad ? "SOME FAILED\n" : "ALL OK\n");
    return bad;
}
#endif
