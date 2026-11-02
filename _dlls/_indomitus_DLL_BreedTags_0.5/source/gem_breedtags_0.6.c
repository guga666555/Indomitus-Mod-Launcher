/*
 * GEM BreedTags 0.1  -  Call to Arms: Gates of Hell (GEM engine)
 *
 * Copies a breed's {tags "..."} into the entity's script tags when the breed is applied,
 * so {if tagged "..."} in interaction_entity scripts (and trigger {tag ...} selectors) see
 * them. No inventory items, no mission triggers.
 *
 * Ships as a dbghelp.dll proxy: the game exes import exactly six dbghelp functions, which
 * are forwarded to the real %SystemRoot%\System32\dbghelp.dll. Drop next to the exe.
 *
 * How it works (all addresses found at runtime, nothing hard-coded per version):
 *   - ApplyBreed(human, flags)  : eHumanBreedUsage.cpp; found from the 'Unknown breed "%s"'
 *                                 string xref, walking back to the function prologue. The
 *                                 human->breed pointer offset is read from its
 *                                 'cmp [rcx+off], reg' (0x1390 in 1.067 game/editor, 0x1388 server).
 *   - Breed tag container       : eHumanBreed.cpp parser, the 'lea reg,[breed+off]' right before
 *                                 the "tags" key read (0x128 game/server, 0x130 editor).
 *   - TagContainer copy ctor, add_all, Entity::SetTags, dtor, and the entity container offset
 *                                 (0x1d0): all read out of the {tags add/remove} interaction
 *                                 action's eval function, reached through the action table.
 *   - Hook: 14-byte inline jmp on ApplyBreed, original prologue relocated to a trampoline.
 *     After the original returns, the breed's tags are merged into the entity exactly the way
 *     {tags add} does it.
 *
 * 0.6 - re-breeds: when a unit gets a different breed (the conquest/army preview re-breeds the same soldier while
 *       you click through a squad) the previous breed's tags are removed before the new ones are added, so a
 *       unit never carries two factions' tags at once. ini RemoveOldBreedTags.
 *     - {on "breed_applied"} (quoted = the engine's custom named event, loads silently without the DLL): fired on
 *       the unit every time a breed has been applied and its tags merged, if the unit is already in the scene.
 *       Covers the cases {on spawn} misses: a re-breed of an existing soldier (no new spawn) and a breed the engine
 *       applies on the unit's first update instead of at spawn (scene still loading). ini BreedEvent.
 *     - the log says for each application where it came from (at once / on the unit's update), which thread,
 *       which tags were removed and added, and whether the event fired.
 *
 * Build (zig / mingw-w64):  see build.sh
 */
#ifndef BT_TEST
#include <windows.h>
#else
#include <stdlib.h>
#include <sys/mman.h>
#endif
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>

#define BT_VERSION "0.6"
#ifdef BT_VIEW_ONLY                       /* GEM TagView: Alt+T tag view + floating labels only, simulation untouched */
#define BT_NAME  "GEM TagView"
#define BT_INI   L"\\gem_tagview.ini"
#define BT_LOG   L"\\gem_tagview.log"
#define BT_SECT  L"tagview"
#else
#define BT_NAME  "GEM BreedTags"
#define BT_INI   L"\\gem_breedtags.ini"
#define BT_LOG   L"\\gem_breedtags.log"
#define BT_SECT  L"breedtags"
#endif

#ifndef BT_TEST
/* ------------------------------------------------------------------------------------ */
/* dbghelp forwarders                                                                   */
/* ------------------------------------------------------------------------------------ */
FARPROC gbt_SymInitialize, gbt_SymCleanup, gbt_SymFromAddr, gbt_SymGetLineFromAddr64,
        gbt_ImageNtHeader, gbt_SymSetOptions, gbt_SymGetOptions, gbt_SymLoadModule64,
        gbt_SymGetModuleBase64, gbt_SymFunctionTableAccess64, gbt_StackWalk64, gbt_MiniDumpWriteDump;

#define FWD(name) \
    __declspec(dllexport) __attribute__((naked)) void name(void) { \
        __asm__ volatile("jmp *gbt_" #name "(%rip)"); }

FWD(SymInitialize)
FWD(SymCleanup)
FWD(SymFromAddr)
FWD(SymGetLineFromAddr64)
FWD(ImageNtHeader)
FWD(SymSetOptions)
FWD(SymGetOptions)
FWD(SymLoadModule64)
FWD(SymGetModuleBase64)
FWD(SymFunctionTableAccess64)
FWD(StackWalk64)
FWD(MiniDumpWriteDump)

static HMODULE g_realDbghelp;

static void load_real_dbghelp(void)
{
    wchar_t path[MAX_PATH];
    UINT n = GetSystemDirectoryW(path, MAX_PATH);
    if (!n) return;
    wcscat(path, L"\\dbghelp.dll");
    g_realDbghelp = LoadLibraryW(path);
    if (!g_realDbghelp) return;
#define RES(name) gbt_##name = GetProcAddress(g_realDbghelp, #name)
    RES(SymInitialize); RES(SymCleanup); RES(SymFromAddr); RES(SymGetLineFromAddr64);
    RES(ImageNtHeader); RES(SymSetOptions); RES(SymGetOptions); RES(SymLoadModule64);
    RES(SymGetModuleBase64); RES(SymFunctionTableAccess64); RES(StackWalk64); RES(MiniDumpWriteDump);
#undef RES
}
#endif /* BT_TEST */

/* ------------------------------------------------------------------------------------ */
/* logging / ini                                                                        */
/* ------------------------------------------------------------------------------------ */
static int g_verbose = 1;
#ifdef BT_TEST
static void logf_(const char *fmt, ...){ va_list ap; va_start(ap,fmt); vprintf(fmt,ap); va_end(ap); printf("\n"); }
#else
static FILE *g_log;
static int g_console, g_logfile = 1;
static wchar_t g_dir[MAX_PATH];

static void logf_(const char *fmt, ...)
{
    char buf[1024];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    SYSTEMTIME st; GetLocalTime(&st);
    char line[1100];
    snprintf(line, sizeof line, "%02d:%02d:%02d [" BT_NAME "] %s\n", st.wHour, st.wMinute, st.wSecond, buf);
    if (g_log) { fputs(line, g_log); fflush(g_log); }
    if (g_console) { fputs(line, stdout); fflush(stdout); }
    OutputDebugStringA(line);
}
#endif

#ifndef BT_TEST
static void ini_str(const char *key, const char *def, char *out, size_t n)
{
    wchar_t ini[MAX_PATH]; wcscpy(ini, g_dir); wcscat(ini, BT_INI);
    wchar_t wkey[64], wdef[128], wout[256]; MultiByteToWideChar(CP_ACP, 0, key, -1, wkey, 64); MultiByteToWideChar(CP_ACP, 0, def, -1, wdef, 128);
    GetPrivateProfileStringW(BT_SECT, wkey, wdef, wout, 256, ini);
    WideCharToMultiByte(CP_ACP, 0, wout, -1, out, (int)n, NULL, NULL);
}

static int ini_int(const char *key, int def)
{
    wchar_t ini[MAX_PATH]; wcscpy(ini, g_dir); wcscat(ini, BT_INI);
    wchar_t wkey[64]; MultiByteToWideChar(CP_ACP, 0, key, -1, wkey, 64);
    return (int)GetPrivateProfileIntW(BT_SECT, wkey, def, ini);
}
#endif /* BT_TEST */

/* ------------------------------------------------------------------------------------ */
/* PE helpers                                                                           */
/* ------------------------------------------------------------------------------------ */
typedef struct { uint8_t *p; size_t n; } Sec;
static Sec g_text, g_rdata, g_data;
static uint8_t *g_base;
static uint32_t g_timestamp;

#ifndef BT_TEST
static int get_sections(void)
{
    g_base = (uint8_t *)GetModuleHandleW(NULL);
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)g_base;
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(g_base + dos->e_lfanew);
    g_timestamp = nt->FileHeader.TimeDateStamp;
    IMAGE_SECTION_HEADER *sh = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        Sec s = { g_base + sh[i].VirtualAddress, sh[i].Misc.VirtualSize };
        if (!strncmp((char *)sh[i].Name, ".text", 8)) g_text = s;
        else if (!strncmp((char *)sh[i].Name, ".rdata", 8)) g_rdata = s;
        else if (!strncmp((char *)sh[i].Name, ".data", 8)) g_data = s;
    }
    return g_text.p && g_rdata.p && g_data.p;
}

#endif /* BT_TEST */
static int in_text(const void *a) { return (uint8_t *)a >= g_text.p && (uint8_t *)a < g_text.p + g_text.n; }
static int in_rdata(const void *a) { return (uint8_t *)a >= g_rdata.p && (uint8_t *)a < g_rdata.p + g_rdata.n; }

static uint8_t *find_bytes(uint8_t *p, size_t n, const uint8_t *pat, size_t m)
{
    if (m > n) return NULL;
    for (size_t i = 0; i + m <= n; i++)
        if (p[i] == pat[0] && !memcmp(p + i, pat, m)) return p + i;
    return NULL;
}

/* bounded search for the next E8 call; returns target */
static uint8_t *next_call(uint8_t *p, size_t window, uint8_t **site)
{
    for (size_t i = 0; i < window; i++)
        if (p[i] == 0xE8) {
            uint8_t *t = p + i + 5 + *(int32_t *)(p + i + 1);
            if (in_text(t)) { if (site) *site = p + i; return t; }
        }
    return NULL;
}

/* find a RIP-relative 'lea reg,[rip+disp]' in .text whose target == want (REX.W/REX.WR, 8D, modrm mod=00 rm=101) */
static uint8_t *find_lea_to(uint8_t *from, size_t n, const void *want, int which)
{
    int k = 0;
    for (size_t i = 0; i + 7 <= n; i++) {
        uint8_t r = from[i];
        if ((r & 0xF8) != 0x48 || from[i + 1] != 0x8D || (from[i + 2] & 0xC7) != 0x05) continue;
        uint8_t *t = from + i + 7 + *(int32_t *)(from + i + 3);
        if (t == want) { if (k++ == which) return from + i; }
    }
    return NULL;
}

/* NUL-terminated string in .rdata that starts at a string boundary */
static uint8_t *find_cstr(const char *str)
{
    size_t m = strlen(str) + 1;
    for (uint8_t *p = g_rdata.p; (p = find_bytes(p, g_rdata.p + g_rdata.n - p, (const uint8_t *)str, m)); p++)
        if (p == g_rdata.p || p[-1] == 0) return p;
    return NULL;
}

/* any RIP-relative reference (lea/mov/cmp..., 2-byte opcode with optional REX) to want, within [from, from+n) */
static uint8_t *find_rip_ref(uint8_t *from, size_t n, const void *want)
{
    for (size_t i = 0; i + 6 <= n; i++) {
        size_t j = i;
        if ((from[j] & 0xF0) == 0x40) j++;
        uint8_t op = from[j];
        if (op != 0x8D && op != 0x8B && op != 0x89 && op != 0x39 && op != 0x3B && op != 0x83) continue;
        if ((from[j + 1] & 0xC7) != 0x05) continue;
        uint8_t *t = from + j + 6 + *(int32_t *)(from + j + 2);
        if (op == 0x83) t += 1;
        if (t == want) return from + i;
    }
    return NULL;
}

/* ------------------------------------------------------------------------------------ */
/* engine bindings                                                                      */
/* ------------------------------------------------------------------------------------ */
typedef void  (*ApplyBreedFn)(void *human, unsigned flags);
typedef void *(*TagCopyCtorFn)(void *dst, const void *src);
typedef void  (*TagAddAllFn)(void *dst, const void *src);
typedef void  (*SetTagsFn)(void *entity, const void *container);
typedef void  (*TagDtorFn)(void *c);

static ApplyBreedFn   g_applyBreed, g_applyBreedOrig;
static TagCopyCtorFn  g_tagCopyCtor;
static TagAddAllFn    g_tagAddAll, g_tagRemoveAll;
static SetTagsFn      g_setTags;
static TagDtorFn      g_tagDtor;
static uint32_t       g_humanBreedOff, g_breedTagsOff, g_entTagsOff;

#define TC_SELF_A   0x308   /* TagContainer: owner back-pointers (== container itself) */
#define TC_SELF_B   0x310
#define TC_VEC_BEG  0x318
#define TC_VEC_END  0x320
#define TC_SIZE     0x330
#define TAG_SIZE    0x60    /* vector element */
#define TAG_STR     0x40    /* std::string inside the element */
#define ENT_FLAGS   0x30    /* entity flags byte: bit 1 = alive / in scene (the engine's own check) */

static const char *tag_name(const uint8_t *tag, char *buf, size_t n)
{
    const char *s = (const char *)(tag + TAG_STR);
    size_t len = *(const size_t *)(tag + TAG_STR + 0x10), cap = *(const size_t *)(tag + TAG_STR + 0x18);
    if (cap > 15) s = *(const char *const *)(tag + TAG_STR);
    if (len >= n) len = n - 1;
    memcpy(buf, s, len); buf[len] = 0;
    return buf;
}

static int container_ok(const uint8_t *c)
{
    return *(const uint8_t *const *)(c + TC_SELF_A) == c && *(const uint8_t *const *)(c + TC_SELF_B) == c;
}

static int find_apply_breed(void)
{
    static const char key[] = "Unknown breed \"%s\"";
    uint8_t *s = find_bytes(g_rdata.p, g_rdata.n, (const uint8_t *)key, sizeof key);
    if (!s) { logf_("ApplyBreed: string not found"); return 0; }
    uint8_t *lea = find_lea_to(g_text.p, g_text.n, s, 0);
    if (!lea) { logf_("ApplyBreed: string xref not found"); return 0; }
    if (find_lea_to(g_text.p, g_text.n, s, 1)) { logf_("ApplyBreed: string xref ambiguous"); return 0; }
    static const uint8_t prologue[] = { 0x48,0x89,0x5C,0x24,0x10, 0x48,0x89,0x74,0x24,0x20, 0x55, 0x57, 0x41,0x54, 0x41,0x56, 0x41,0x57 };
    uint8_t *f = NULL;
    for (uint8_t *p = lea; p >= lea - 0x300 && p >= g_text.p; p--)
        if (!memcmp(p, prologue, sizeof prologue) && p[-1] == 0xCC) { f = p; break; }
    if (!f) { logf_("ApplyBreed: prologue not found"); return 0; }
    /* cmp qword [rcx+disp32], reg  ->  48/4C 39 modrm(mod=10,rm=001) disp32 */
    for (uint8_t *p = f; p < lea - 7; p++) {
        if ((p[0] == 0x48 || p[0] == 0x4C) && p[1] == 0x39 && (p[2] & 0xC7) == 0x81) {
            uint32_t off = *(uint32_t *)(p + 3);
            if (off > 0x800 && off < 0x4000) { g_humanBreedOff = off; break; }
        }
    }
    if (!g_humanBreedOff) { logf_("ApplyBreed: human->breed offset not found"); return 0; }
    g_applyBreed = (ApplyBreedFn)f;
    logf_("ApplyBreed = exe+0x%llX, human->breed @ +0x%X", (unsigned long long)(f - g_base), g_humanBreedOff);
    return 1;
}

static int find_breed_tags_off(void)
{
    /* parser (eHumanBreed.cpp): ... read("icon_priority") ... lea reg,[breed+off] ... read("tags") ... read("nationality") */
    static const char knat[] = "nationality", ktags[] = "tags";
    uint8_t *snat = NULL;
    for (uint8_t *p = g_rdata.p; (p = find_bytes(p, g_rdata.p + g_rdata.n - p, (const uint8_t *)knat, sizeof knat)); p++)
        if (p == g_rdata.p || p[-1] == 0) { snat = p; break; }
    if (!snat) { logf_("BreedTags: 'nationality' not found"); return 0; }
    uint8_t *lnat = find_lea_to(g_text.p, g_text.n, snat, 0);
    if (!lnat) { logf_("BreedTags: 'nationality' xref not found"); return 0; }
    /* the "tags" lea closest before it (within the parser) */
    uint8_t *ltags = NULL;
    for (uint8_t *p = lnat - 7; p > lnat - 0x600 && p > g_text.p; p--) {
        if ((p[0] & 0xF8) != 0x48 || p[1] != 0x8D || (p[2] & 0xC7) != 0x05) continue;
        uint8_t *t = p + 7 + *(int32_t *)(p + 3);
        if (in_rdata(t) && !memcmp(t, ktags, sizeof ktags) && t[-1] == 0) { ltags = p; break; }
    }
    if (!ltags) { logf_("BreedTags: 'tags' read not found in breed parser"); return 0; }
    /* the lea reg,[reg+disp32] just before it: 4? 8D modrm(mod=10) [sib] disp32 */
    for (uint8_t *p = ltags - 7; p > ltags - 0x80; p--) {
        if ((p[0] & 0xF0) != 0x40 || p[1] != 0x8D || (p[2] & 0xC0) != 0x80) continue;
        int sib = (p[2] & 7) == 4;
        uint32_t off = *(uint32_t *)(p + 3 + sib);
        if (off >= 0x80 && off <= 0x400) { g_breedTagsOff = off; break; }
    }
    if (!g_breedTagsOff) { logf_("BreedTags: breed tag container offset not found"); return 0; }
    logf_("Breed tag container @ breed+0x%X", g_breedTagsOff);
    return 1;
}

static int find_tag_functions(void)
{
    /* interaction action table in .data: { "tags", ctor } */
    uint8_t *ctor = NULL; int hits = 0;
    for (size_t i = 0; i + 16 <= g_data.n; i += 8) {
        uint8_t *str = *(uint8_t **)(g_data.p + i), *fn = *(uint8_t **)(g_data.p + i + 8);
        if (!in_rdata(str) || !in_text(fn)) continue;
        if (memcmp(str, "tags", 5) || str[-1] != 0) continue;
        ctor = fn; hits++;
    }
    if (hits != 1) { logf_("tags action: table entry %s", hits ? "ambiguous" : "not found"); return 0; }
    /* ctor: lea rax,[rip+vtable] */
    uint8_t *vt = NULL;
    for (uint8_t *p = ctor; p < ctor + 0x80; p++)
        if (p[0] == 0x48 && p[1] == 0x8D && p[2] == 0x05) { vt = p + 7 + *(int32_t *)(p + 3); break; }
    if (!vt || !in_rdata(vt)) { logf_("tags action: vtable not found"); return 0; }
    uint8_t *ev = *(uint8_t **)(vt + 8);
    if (!in_text(ev)) { logf_("tags action: eval not in .text"); return 0; }
    /* eval:  add rdx, imm32 (entity container offset) ; call copy_ctor */
    uint8_t *p = NULL, *site;
    for (uint8_t *q = ev; q < ev + 0x100; q++)
        if (q[0] == 0x48 && q[1] == 0x81 && q[2] == 0xC2) { g_entTagsOff = *(uint32_t *)(q + 3); p = q + 7; break; }
    if (!p || g_entTagsOff < 0x100 || g_entTagsOff > 0x400) { logf_("tags action: entity container offset not found"); return 0; }
    uint8_t *cc = next_call(p, 0x20, &site);
    static const uint8_t cc_sig[] = { 0xC6,0x81,0x00,0x03,0x00,0x00,0x00 };   /* mov byte [rcx+0x300],0 */
    if (!cc || !find_bytes(cc, 0x40, cc_sig, sizeof cc_sig)) { logf_("tags action: copy ctor not recognised"); return 0; }
    g_tagCopyCtor = (TagCopyCtorFn)cc;
    /* cmp byte [reg+0x358],0 ; je REMOVE ; call add_all ; jmp ; REMOVE: call remove_all */
    p = NULL; uint8_t *rmv = NULL;
    for (uint8_t *q = site; q < ev + 0x200 && !p; q++)
        if (q[0] == 0x80 && (q[1] & 0xF8) == 0xB8 && q[6] == 0x00)           /* cmp byte [reg+disp32],0 */
            for (uint8_t *j = q + 7; j < q + 0x20; j++)
                if (j[0] == 0x74) { p = j + 2; rmv = j + 2 + (int8_t)j[1]; break; } /* je rel8 -> fall-through = add */
    if (!p) { logf_("tags action: add/remove switch not found"); return 0; }
    uint8_t *aa = next_call(p, 0x10, &site);
    static const uint8_t aa_sig[] = { 0x48,0x8B,0x9A,0x18,0x03,0x00,0x00 };   /* mov rbx,[rdx+0x318] */
    if (!aa || !find_bytes(aa, 0x20, aa_sig, sizeof aa_sig)) { logf_("tags action: add_all not recognised"); return 0; }
    g_tagAddAll = (TagAddAllFn)aa;
    {   /* the remove branch (0.6, optional): same container walk, different function */
        uint8_t *rs = NULL, *ra = rmv && in_text(rmv) && rmv[0] == 0xE8 ? next_call(rmv, 1, &rs) : NULL;
        if (ra && ra != aa && find_bytes(ra, 0x20, aa_sig, sizeof aa_sig)) g_tagRemoveAll = (TagAddAllFn)ra;
        else logf_("tags action: remove_all not recognised - old breed tags will not be removed");
    }
    /* lea rdx,[rsp+0x20] ; (mov rcx,[entity]) ; call SetTags */
    static const uint8_t lea_rdx[] = { 0x48,0x8D,0x54,0x24,0x20 }, lea_rcx[] = { 0x48,0x8D,0x4C,0x24,0x20 };
    p = find_bytes(site + 5, 0x80, lea_rdx, sizeof lea_rdx);
    uint8_t *st = p ? next_call(p + 5, 0x10, &site) : NULL;
    static const uint8_t st_sig[] = { 0xE8,0x04,0x00,0x00 };                   /* [rcx+0x4e8] */
    if (!st || !find_bytes(st, 0x30, st_sig, sizeof st_sig)) { logf_("tags action: SetTags not recognised"); return 0; }
    g_setTags = (SetTagsFn)st;
    p = find_bytes(site + 5, 0x40, lea_rcx, sizeof lea_rcx);
    uint8_t *dt = p ? next_call(p + 5, 0x10, &site) : NULL;
    static const uint8_t dt_sig[] = { 0x48,0x81,0xC1,0x10,0x03,0x00,0x00 };   /* add rcx,0x310 */
    if (!dt || memcmp(dt, dt_sig, sizeof dt_sig)) { logf_("tags action: dtor not recognised"); return 0; }
    g_tagDtor = (TagDtorFn)dt;
    logf_("TagContainer copy=exe+0x%llX add_all=exe+0x%llX remove_all=exe+0x%llX SetTags=exe+0x%llX dtor=exe+0x%llX, entity container @ +0x%X",
          (unsigned long long)(cc - g_base), (unsigned long long)(aa - g_base),
          g_tagRemoveAll ? (unsigned long long)((uint8_t *)g_tagRemoveAll - g_base) : 0ULL, (unsigned long long)(st - g_base),
          (unsigned long long)(dt - g_base), g_entTagsOff);
    return 1;
}

/* ------------------------------------------------------------------------------------ */
/* 0.6: named event {on "breed_applied"} - the engine's own custom-event path            */
/* ------------------------------------------------------------------------------------ */
/* The engine fires custom named events ({on "name"} handlers, also reached by {call "name"}) as:
   Tag tag("name") ; Ctx ctx(entity) ; FireNamedEvent(&ctx, &tag) ; ~Ctx ; ~Tag  (its own "start_healing" call site).
   FireNamedEvent looks the name up in [[entity+DEF]+SET] (the def's compiled interaction set). Same finder as
   GEM ScriptExt. */
typedef void  (*TagCtorCStrFn)(void *tag, const char *s);
typedef void *(*CtxCtorFn)(void *ctx, void *entity);
typedef void  (*CtxDtorFn)(void *ctx);
typedef void  (*FireNamedFn)(void *ctx, const void *tag);
static TagCtorCStrFn g_evTagCtor; static TagDtorFn g_evTagDtor;
static CtxCtorFn g_evCtxCtor; static CtxDtorFn g_evCtxDtor; static FireNamedFn g_evFire;
static uint32_t g_entDefOff, g_defSetOff;

static int find_named_event(void)
{
    uint8_t *s = find_cstr("start_healing"), *ev = find_cstr("EVENT: ");
    if (!s) { logf_("breed event: 'start_healing' not found"); return 0; }
    static const uint8_t tagdtor_sig[] = { 0x48,0x83,0xC1,0x38,0xE9 };       /* add rcx,0x38 ; jmp */
    static const uint8_t ctxctor_sig[] = { 0x48,0x89,0x11 };                 /* mov [rcx],rdx */
    uint8_t *fn[5] = { 0 }; int ok = 0;
    for (int k = 0; k < 8 && !ok; k++) {                     /* the string has several users: take the 5-call fire sequence */
        uint8_t *lea = find_lea_to(g_text.p, g_text.n, s, k);
        if (!lea) break;
        uint8_t *p = lea, *site; int i;
        for (i = 0; i < 5; i++) { fn[i] = next_call(p, 0x40, &site); if (!fn[i]) break; p = site + 5; }
        if (i < 5 || memcmp(fn[4], tagdtor_sig, 5) || !find_bytes(fn[1], 0x20, ctxctor_sig, 3)) continue;
        for (uint8_t *q = fn[2]; q < fn[2] + 0x100 && !ok; q++)     /* FireNamed calls the dispatcher that prints "EVENT: " */
            if (q[0] == 0xE8) { uint8_t *t = q + 5 + *(int32_t *)(q + 1); if (in_text(t) && ev && find_rip_ref(t, 0x400, ev)) ok = 1; }
    }
    if (!ok) { logf_("breed event: FireNamedEvent not recognised"); return 0; }
    for (uint8_t *q = fn[2]; q < fn[2] + 0x80; q++)               /* mov rX,[rax+DEF] ; mov rY,[rX+SET] */
        if ((q[0] & 0xF8) == 0x48 && q[1] == 0x8B && (q[2] & 0xC7) == 0x80 && (q[7] & 0xF8) == 0x48 && q[8] == 0x8B && (q[9] & 0xC0) == 0x40) {
            uint32_t off = *(uint32_t *)(q + 3);
            if (off > 0x100 && off < 0x1000) { g_entDefOff = off; g_defSetOff = q[10]; break; }
        }
    if (!g_entDefOff || !g_defSetOff) { logf_("breed event: entity->def / def->set offsets not recognised"); return 0; }
    g_evTagCtor = (TagCtorCStrFn)fn[0]; g_evCtxCtor = (CtxCtorFn)fn[1]; g_evFire = (FireNamedFn)fn[2];
    g_evCtxDtor = (CtxDtorFn)fn[3]; g_evTagDtor = (TagDtorFn)fn[4];
    logf_("breed event: Tag(cstr)=exe+0x%llX Ctx=exe+0x%llX FireNamed=exe+0x%llX ~Ctx=exe+0x%llX ~Tag=exe+0x%llX, entity->def +0x%X, def->set +0x%X",
          (unsigned long long)(fn[0] - g_base), (unsigned long long)(fn[1] - g_base), (unsigned long long)(fn[2] - g_base),
          (unsigned long long)(fn[3] - g_base), (unsigned long long)(fn[4] - g_base), g_entDefOff, g_defSetOff);
    return 1;
}

/* The engine applies a breed either at once (SetBreed -> ApplyBreed(human, flags)) or, while a scene is being created
   and the unit has an inventory, later on the unit's first update (SetBreed stores the flags in the human, eHuman::Quant
   then calls ApplyBreed(human, [human+PENDING])). The second kind of call site loads edx from memory. */
static uint8_t *g_abSites[8]; static int g_abSiteN; static uint8_t g_abSiteDeferred[8];
static void find_apply_breed_callers(void)
{
    for (size_t i = 0; i + 5 <= g_text.n && g_abSiteN < 8; i++) {
        uint8_t *c = g_text.p + i;
        if (c[0] != 0xE8 || c + 5 + *(int32_t *)(c + 1) != (uint8_t *)g_applyBreed) continue;
        int deferred = 0;
        for (uint8_t *q = c - 14; q < c - 4; q++)                 /* mov edx,[reg+disp32] (8B 9x / 41 8B 9x) */
            if (q[0] == 0x8B && (q[1] & 0xF8) == 0x90 && (q[1] & 7) != 4) deferred = 1;
        g_abSiteDeferred[g_abSiteN] = (uint8_t)deferred;
        g_abSites[g_abSiteN++] = c + 5;
    }
    char line[256] = ""; size_t n = 0;
    for (int k = 0; k < g_abSiteN; k++)
        n += snprintf(line + n, sizeof line - n, "%sexe+0x%llX (%s)", k ? ", " : "", (unsigned long long)(g_abSites[k] - g_base),
                      g_abSiteDeferred[k] ? "on the unit's update" : "at once");
    logf_("ApplyBreed callers: %s", g_abSiteN ? line : "none found");
}
static const char *apply_kind(const void *ret)
{
    for (int k = 0; k < g_abSiteN; k++) if (g_abSites[k] == ret) return g_abSiteDeferred[k] ? "on update" : "at once";
    return "other";
}

/* ------------------------------------------------------------------------------------ */
/* the hook                                                                             */
/* ------------------------------------------------------------------------------------ */
static int g_warned, g_applied, g_verboseMax = 200;
static volatile int g_modActive = 1;                 /* RequireMod: 0 until the named mod is seen enabled */
static void *g_humanVt[8]; static int g_humanVtN;
static int g_removeOld = 1, g_okEvent; static char g_breedEvent[64] = "breed_applied";

#ifndef BT_TEST
/* last breed whose tags were merged into each unit (so a re-breed can take the old breed's tags off again).
   Open addressing on the entity pointer; never shrinks, cleared when 3/4 full. A freed unit's address may be reused
   by a new one: then the stale breed's tags are removed from the newcomer before its own are added - only tags with
   those exact names that a script added before the breed was applied could be lost. */
#define BT_SLOTS 16384
static struct { void *e, *b; } g_slots[BT_SLOTS];
static int g_slotsUsed; static SRWLOCK g_slotLock = SRWLOCK_INIT;
static void *swap_breed(void *e, void *b)
{
    void *old = NULL;
    AcquireSRWLockExclusive(&g_slotLock);
    for (int pass = 0; pass < 2; pass++) {
        uint32_t h = (uint32_t)((((uintptr_t)e >> 4) * 0x9E3779B97F4A7C15ull) >> 50) & (BT_SLOTS - 1);
        int done = 0;
        for (int k = 0; k < BT_SLOTS; k++, h = (h + 1) & (BT_SLOTS - 1)) {
            if (g_slots[h].e == e) { old = g_slots[h].b; g_slots[h].b = b; done = 1; break; }
            if (!g_slots[h].e) {
                if (g_slotsUsed >= BT_SLOTS * 3 / 4) break;              /* full: clear and retry */
                g_slots[h].e = e; g_slots[h].b = b; g_slotsUsed++; done = 1; break;
            }
        }
        if (done) break;
        memset(g_slots, 0, sizeof g_slots); g_slotsUsed = 0;
        logf_("breed table full - cleared (old breed tags of units seen before are no longer removed on a re-breed)");
    }
    ReleaseSRWLockExclusive(&g_slotLock);
    return old;
}

static DWORD g_mainTid;
static _Thread_local int t_inEvent;                   /* our event handler re-applying a breed: no second event */

static int tags_of(const uint8_t *c, char *line, size_t n)
{
    char name[64]; size_t len = 0; int cnt = 0;
    uint8_t *vb = *(uint8_t *const *)(c + TC_VEC_BEG), *ve = *(uint8_t *const *)(c + TC_VEC_END);
    if (line) line[0] = 0;
    for (uint8_t *t = vb; t < ve; t += TAG_SIZE, cnt++)
        if (line && len < n - 70) len += snprintf(line + len, n - len, "%s\"%s\"", len ? " " : "", tag_name(t, name, sizeof name));
    return cnt;
}

static void hook_apply_breed(void *human, unsigned flags)
{
    void *ret = __builtin_return_address(0);           /* entered through the jmp patch: the caller of ApplyBreed */
    g_applyBreedOrig(human, flags);
    if (!human || !g_modActive) return;
    { void *vt = *(void **)human; int k; for (k = 0; k < g_humanVtN; k++) if (g_humanVt[k] == vt) break;
      if (k == g_humanVtN && g_humanVtN < 8) g_humanVt[g_humanVtN++] = vt; }
    uint8_t *breed = *(uint8_t **)((uint8_t *)human + g_humanBreedOff);
    if (!breed) return;
    uint8_t *bc = breed + g_breedTagsOff;
    uint8_t *ec = (uint8_t *)human + g_entTagsOff;
    if (!container_ok(bc) || !container_ok(ec)) {
        if (!g_warned++) logf_("container sanity check failed (breed %d, entity %d) - breed tags OFF", container_ok(bc), container_ok(ec));
        return;
    }
    uint8_t *old = (uint8_t *)swap_breed(human, breed), *oc = old ? old + g_breedTagsOff : NULL;
    int nNew = tags_of(bc, NULL, 0);
    int nOld = (g_removeOld && g_tagRemoveAll && old && old != breed && container_ok(oc)) ? tags_of(oc, NULL, 0) : 0;

    if (nNew || nOld) {
        __attribute__((aligned(16))) uint8_t tmp[TC_SIZE + 0x40];
        g_tagCopyCtor(tmp, ec);
        if (nOld) g_tagRemoveAll(tmp, oc);                  /* the previous breed's tags off ... */
        if (nNew) g_tagAddAll(tmp, bc);                     /* ... this breed's on (shared names end up present) */
        g_setTags(human, tmp);
        g_tagDtor(tmp);
    }

    /* {on "breed_applied"}: only for a unit already in the scene - during spawn the unit is not in yet and its
       {on spawn} handler runs afterwards with the tags already in place */
    const char *evs = "off";
    if (g_okEvent && g_breedEvent[0]) {
        uint8_t *e = (uint8_t *)human, *def = *(uint8_t **)(e + g_entDefOff);
        if (t_inEvent) evs = "skipped (inside our own event)";
        else if (!(e[ENT_FLAGS] & 2)) evs = "not fired (unit not in the scene yet - {on spawn} follows)";
        else if (!def || !*(void **)(def + g_defSetOff)) evs = "not fired (no interaction set)";
        else {
            __attribute__((aligned(16))) uint8_t ctx[0x400];
            __attribute__((aligned(16))) uint8_t tag[0x80];
            memset(ctx, 0, sizeof ctx);
            t_inEvent = 1;
            g_evCtxCtor(ctx, human);
            g_evTagCtor(tag, g_breedEvent);
            g_evFire(ctx, tag);
            g_evTagDtor(tag);
            g_evCtxDtor(ctx);
            t_inEvent = 0;
            evs = "fired";
        }
    }

    if (g_verbose && g_applied < g_verboseMax) {
        char bn[64] = "?", add[400], rem[300], th[24];
        tag_name((uint8_t *)human + g_humanBreedOff - 0x60, bn, sizeof bn);        /* breed name Tag sits 0x60 before */
        tags_of(bc, add, sizeof add);
        if (nOld) tags_of(oc, rem, sizeof rem); else snprintf(rem, sizeof rem, "-");
        DWORD tid = GetCurrentThreadId();
        if (tid == g_mainTid) snprintf(th, sizeof th, "main"); else snprintf(th, sizeof th, "%lu", (unsigned long)tid);
        logf_("breed \"%s\" -> entity %p (%s, thread %s): removed [%s] added [%s], event %s",
              bn, human, apply_kind(ret), th, rem, nNew ? add : "-", evs);
        if (++g_applied == g_verboseMax) logf_("(further applications not logged - ini VerboseMax)");
    }
}
#endif /* BT_TEST */

/* ------------------------------------------------------------------------------------ */
/* Tag overlay: floating text above units, through the engine's own damage-report text  */
/* ------------------------------------------------------------------------------------ */
/* {damage_report "bone" "text"} creates an entry in a global report vector that the HUD draws above the
   bone, world-projected, for a few seconds. Calling the same function again for the same entity+bone
   within 1 s updates the entry in place (text replaced, expiry extended). Re-issuing every ~0.5 s and
   pinning the entry's creation stamp therefore gives a static label - no renderer or camera hooking.
   Ticks come from the {if tagged} condition's eval, which the FSM scripts run every frame on the
   simulation thread: its vtable slot is redirected to a wrapper (no prologue patching needed). */

typedef void *(*TagFromStrFn)(void *tag, const void *stdstring);
typedef void  (*TagDtorFn2)(void *tag);
typedef void  (*DamageReportFn)(void *entity, void *boneTag, const void *stdstring);
typedef int   (*CondEvalFn)(void *cond, void **ctx);

static TagFromStrFn    g_tagFromStr;
static TagDtorFn2      g_tagDtor2;
static DamageReportFn  g_damageReport;
static CondEvalFn      g_taggedEvalOrig;
static uint8_t       **g_taggedVtSlot;          /* &vtable[1] of the tagged condition */
static uint8_t       **g_entList;               /* std::vector<Entity*> of tagged entities: [0]=begin [1]=end */
static uint8_t       **g_reportVec;             /* damage report vector: [0]=begin [1]=end, 0xb0-byte entries */
static int   g_showTags, g_showHotkey = 120, g_showMax = 60, g_showInterval = 500;
static char  g_showFilter[64], g_showBone[32] = "basis", g_showStyle[64] = "<s(12)>";

#define RPT_SIZE     0xb0
#define RPT_ENTITY   0x00
#define RPT_TYPE     0x10
#define RPT_CREATED  0x9c
#define RPT_EXPIRES  0x98

/* MSVC std::string, SSO layout */
typedef struct { union { char buf[16]; char *ptr; } u; size_t size, cap; } MsString;
static void msstr_init(MsString *s, const char *src, char *heap, size_t heapcap)
{
    size_t n = strlen(src);
    if (n < 16) { memcpy(s->u.buf, src, n + 1); s->size = n; s->cap = 15; }
    else { if (n >= heapcap) n = heapcap - 1; memcpy(heap, src, n); heap[n] = 0; s->u.ptr = heap; s->size = n; s->cap = heapcap - 1; }
}

static int find_tagged_condition(void)
{
    uint8_t *ctor = NULL; int hits = 0;
    for (size_t i = 0; i + 16 <= g_data.n; i += 8) {
        uint8_t *str = *(uint8_t **)(g_data.p + i), *fn = *(uint8_t **)(g_data.p + i + 8);
        if (!in_rdata(str) || !in_text(fn)) continue;
        if (memcmp(str, "tagged", 7) || str[-1] != 0) continue;
        ctor = fn; hits++;
    }
    if (hits != 1) { logf_("tagged condition: table entry %s", hits ? "ambiguous" : "not found"); return 0; }
    uint8_t *vt = NULL;
    for (uint8_t *p = ctor; p < ctor + 0x80; p++)
        if (p[0] == 0x48 && p[1] == 0x8D && (p[2] == 0x05 || p[2] == 0x0D)) { vt = p + 7 + *(int32_t *)(p + 3); break; }
    if (!vt || !in_rdata(vt)) { logf_("tagged condition: vtable not found"); return 0; }
    uint8_t *ev = *(uint8_t **)(vt + 8), *parse = *(uint8_t **)(vt + 16);
    if (!in_text(ev) || !in_text(parse)) { logf_("tagged condition: vtable slots not in .text"); return 0; }
    /* parse(): read string ; Tag::Tag(const string&) ; assign ; ~Tag */
    static const uint8_t dtor_sig[] = { 0x48,0x83,0xC1,0x38,0xE9 };            /* add rcx,0x38 ; jmp */
    for (uint8_t *p = parse; p < parse + 0x60; p++) {
        if (p[0] != 0xE8) continue;
        uint8_t *t = p + 5 + *(int32_t *)(p + 1);
        if (!in_text(t)) continue;
        if (!memcmp(t, dtor_sig, sizeof dtor_sig)) { g_tagDtor2 = (TagDtorFn2)t; continue; }
        if (g_tagFromStr) continue;
        /* Tag::Tag(const string&) initialises the SSO capacity: mov qword [reg+0x58|0x18],0xf  ->  (48) C7 4x disp8 0F 00 00 00 */
        for (uint8_t *q = t; q < t + 0x80; q++)
            if (q[0] == 0xC7 && (q[1] & 0xF8) == 0x40 && (q[2] == 0x58 || q[2] == 0x18) && q[3] == 0x0F && !q[4] && !q[5] && !q[6]) { g_tagFromStr = (TagFromStrFn)t; break; }
    }
    if (!g_tagFromStr || !g_tagDtor2) { logf_("tagged condition: Tag ctor/dtor not recognised"); return 0; }
    g_taggedVtSlot = (uint8_t **)(vt + 8);
    g_taggedEvalOrig = (CondEvalFn)ev;
    logf_("tagged eval=exe+0x%llX Tag(string)=exe+0x%llX ~Tag=exe+0x%llX",
          (unsigned long long)(ev - g_base), (unsigned long long)((uint8_t *)g_tagFromStr - g_base), (unsigned long long)((uint8_t *)g_tagDtor2 - g_base));
    return 1;
}

static int find_damage_report(void)
{
    uint8_t *ctor = NULL; int hits = 0;
    for (size_t i = 0; i + 16 <= g_data.n; i += 8) {
        uint8_t *str = *(uint8_t **)(g_data.p + i), *fn = *(uint8_t **)(g_data.p + i + 8);
        if (!in_rdata(str) || !in_text(fn)) continue;
        if (memcmp(str, "damage_report", 14) || str[-1] != 0) continue;
        ctor = fn; hits++;
    }
    if (hits != 1) { logf_("damage_report: table entry %s", hits ? "ambiguous" : "not found"); return 0; }
    uint8_t *vt = NULL;
    for (uint8_t *p = ctor; p < ctor + 0x100; p++)
        if (p[0] == 0x48 && p[1] == 0x8D && (p[2] == 0x05 || p[2] == 0x0D)) { uint8_t *c = p + 7 + *(int32_t *)(p + 3); if (in_rdata(c)) { vt = c; break; } }
    if (!vt) { logf_("damage_report: vtable not found"); return 0; }
    uint8_t *ev = *(uint8_t **)(vt + 8);
    if (!in_text(ev)) { logf_("damage_report: eval not in .text"); return 0; }
    /* eval: lea r8,[rsi+0x88] (the text) ; ... ; call DamageReport(entity, Tag*, string*) */
    static const uint8_t lea_r8[] = { 0x4C,0x8D,0x86,0x88,0x00,0x00,0x00 };
    uint8_t *p = find_bytes(ev, 0x100, lea_r8, sizeof lea_r8), *site;
    uint8_t *dr = p ? next_call(p + 7, 0x10, &site) : NULL;
    if (!dr) { logf_("damage_report: DamageReport call not found"); return 0; }
    g_damageReport = (DamageReportFn)dr;
    /* DamageReport: ... call AddReport ; (epilogue) ; jmp ~Tag  -> AddReport = last call before the first jmp rel32 */
    uint8_t *add = NULL;
    for (uint8_t *q = dr; q < dr + 0x200; q++) {
        if (q[0] == 0xE8) { uint8_t *t = q + 5 + *(int32_t *)(q + 1); if (in_text(t)) add = t; }
        if (q[0] == 0xE9) break;
    }
    if (!add) { logf_("damage_report: AddReport not found"); return 0; }
    /* AddReport: the first two 'mov reg,[rip+disp]' loads are the report vector's begin/end */
    uint8_t *lo = NULL, *hi = NULL;
    for (uint8_t *q = add; q < add + 0x60 && !(lo && hi); q++) {
        if ((q[0] == 0x48 || q[0] == 0x4C) && q[1] == 0x8B && (q[2] & 0xC7) == 0x05) {
            uint8_t *t = q + 7 + *(int32_t *)(q + 3);
            if (!lo) lo = t; else hi = t;
        }
    }
    if (!lo || !hi || (hi != lo + 8 && lo != hi + 8)) { logf_("damage_report: report vector not found"); return 0; }
    static const uint8_t esz[] = { 0x81,0xC7,0xB0,0x00,0x00,0x00 };   /* add rdi,0xb0 */
    uint8_t *es = find_bytes(add, 0x120, esz + 1, 5);
    if (!es) { logf_("damage_report: entry size not 0xb0 - overlay OFF"); return 0; }
    g_reportVec = (uint8_t **)(lo < hi ? lo : hi);
    logf_("DamageReport=exe+0x%llX reports @ exe+0x%llX", (unsigned long long)(dr - g_base), (unsigned long long)((uint8_t *)g_reportVec - g_base));
    return 1;
}

static int find_entity_list(void)
{
    /* Entity::SetTags: cmp [rsi+0x4e8],rax ; mov rdx,[rip+END] ; mov r8,rsi ; mov rcx,[rip+BEGIN] ; call find */
    uint8_t *f = (uint8_t *)g_setTags, *end = NULL, *beg = NULL;
    for (uint8_t *q = f; q < f + 0x200; q++) {
        if (q[0] == 0x48 && q[1] == 0x8B && q[2] == 0x15 && !end) end = q + 7 + *(int32_t *)(q + 3);
        else if (q[0] == 0x48 && q[1] == 0x8B && q[2] == 0x0D && end) { beg = q + 7 + *(int32_t *)(q + 3); break; }
    }
    if (!beg || end != beg + 8) { logf_("entity list: not found in SetTags"); return 0; }
    g_entList = (uint8_t **)beg;
    logf_("tagged entity list @ exe+0x%llX", (unsigned long long)(beg - g_base));
    return 1;
}

/* ------------------------------------------------------------------------------------ */
/* Engine tag view ("v_tags" - the editor's Alt+T)                                       */
/* ------------------------------------------------------------------------------------ */
/* The entity debug renderer that draws every tag above every entity (green shadow text) is compiled into
   the game as well as the editor; only the Alt+T binding that flips the "v_tags" debug variable is editor-
   side. The variable lives in a static record { name*, type*, value } and the engine has a toggle function
   (lea rcx,"v_tags" ; call Lookup ; ... ; byte [var+0x10] ^= 1). We call that function. */
typedef void *(*VarLookupFn)(const char *name);
typedef void  (*VarToggleFn)(void);
static VarLookupFn g_varLookup;
static VarToggleFn g_vTagsToggle;
static uint8_t    *g_vTagsValue;
static int g_tagView = 0, g_tagViewKey = 'T', g_tagViewAlt = 1, g_isEditor;

static int find_v_tags(void)
{
    static const char key[] = "v_tags";
    uint8_t *str = NULL;
    for (uint8_t *p = g_rdata.p; (p = find_bytes(p, g_rdata.p + g_rdata.n - p, (const uint8_t *)key, sizeof key)); p++)
        if (p == g_rdata.p || p[-1] == 0) { str = p; break; }
    if (!str) { logf_("v_tags: string not found"); return 0; }
    uint8_t *rec = NULL;
    for (size_t i = 0; i + 24 <= g_data.n; i += 8)
        if (*(uint8_t **)(g_data.p + i) == str) { rec = g_data.p + i; break; }
    if (!rec) { logf_("v_tags: variable record not found"); return 0; }
    g_vTagsValue = rec + 0x10;
    /* toggle function: push rbx ; sub rsp,0x20 ; lea rcx,[rip+"v_tags"] ; call Lookup */
    uint8_t *lea = find_lea_to(g_text.p, g_text.n, str, 0);
    static const uint8_t pro[] = { 0x40,0x53,0x48,0x83,0xEC,0x20 };
    if (!lea || memcmp(lea - 6, pro, 6) || lea[7] != 0xE8) { logf_("v_tags: toggle function not recognised"); return 0; }
    g_vTagsToggle = (VarToggleFn)(lea - 6);
    g_varLookup = (VarLookupFn)(lea + 12 + *(int32_t *)(lea + 8));
    if (!in_text((uint8_t *)g_varLookup)) { logf_("v_tags: lookup function not in .text"); return 0; }
    logf_("v_tags value @ exe+0x%llX, toggle = exe+0x%llX", (unsigned long long)(g_vTagsValue - g_base), (unsigned long long)((uint8_t *)g_vTagsToggle - g_base));
    return 1;
}

/* ------------------------------------------------------------------------------------ */
/* Mod gate: the engine's mod list                                                      */
/* ------------------------------------------------------------------------------------ */
/* ModManager::Apply logs "mod.off" then walks the global std::vector<Mod*> disabling every mod
   (`Mod: disable mod <name>` ; mov byte [mod+0x118],0), then enables the selected ones. Name = MSVC
   std::string at mod+0x20, enabled = byte at mod+0x118 (all builds). Read from a watcher thread with
   ReadProcessMemory so a half-built list can never fault us. */
static uint8_t **g_modVec;                 /* [0]=begin [1]=end */
static uint32_t  g_modNameOff, g_modOnOff;
static char      g_requireMod[64];

static int find_mod_list(void)
{
    static const char key[] = "mod.off";
    uint8_t *str = NULL;
    for (uint8_t *p = g_rdata.p; (p = find_bytes(p, g_rdata.p + g_rdata.n - p, (const uint8_t *)key, sizeof key)); p++)
        if (p == g_rdata.p || p[-1] == 0) { str = p; break; }
    if (!str) { logf_("mod list: 'mod.off' not found"); return 0; }
    uint8_t *lea = find_lea_to(g_text.p, g_text.n, str, 0);
    if (!lea) { logf_("mod list: 'mod.off' xref not found"); return 0; }
    uint8_t *beg = NULL, *end = NULL;
    for (uint8_t *q = lea; q < lea + 0x40; q++) {
        if (q[0] == 0x48 && q[1] == 0x8B && q[2] == 0x1D && !beg) beg = q + 7 + *(int32_t *)(q + 3);
        else if (q[0] == 0x48 && q[1] == 0x8B && q[2] == 0x3D && beg) { end = q + 7 + *(int32_t *)(q + 3); break; }
    }
    if (!beg || end != beg + 8) { logf_("mod list: vector not found"); return 0; }
    for (uint8_t *q = lea; q < lea + 0x200 && !(g_modNameOff && g_modOnOff); q++) {
        if (q[0] == 0x49 && q[1] == 0x8D && (q[2] & 0xC7) == 0x46 && !g_modNameOff) g_modNameOff = q[3];            /* lea r?,[r14+disp8] */
        if (q[0] == 0x41 && q[1] == 0xC6 && q[2] == 0x86 && q[7] == 0 && !g_modOnOff) g_modOnOff = *(uint32_t *)(q + 3); /* mov byte [r14+disp32],0 */
    }
    if (!g_modNameOff || !g_modOnOff) { logf_("mod list: entry layout not recognised"); return 0; }
    g_modVec = (uint8_t **)beg;
    logf_("mod list @ exe+0x%llX (name +0x%X, enabled +0x%X)", (unsigned long long)(beg - g_base), g_modNameOff, g_modOnOff);
    return 1;
}

#ifndef BT_TEST
static int rd_mem(const void *src, void *dst, size_t n)
{
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), src, dst, n, &got) && got == n;
}

/* 1 = an enabled mod whose name contains g_requireMod exists; 0 = none; -1 = list unreadable */
static int mod_enabled(char *seen, size_t seenN)
{
    uint8_t *b, *e;
    if (!rd_mem(&g_modVec[0], &b, 8) || !rd_mem(&g_modVec[1], &e, 8) || !b || e < b || e - b > 8 * 512) return -1;
    int hit = 0; size_t sl = 0; if (seen) seen[0] = 0;
    for (uint8_t *p = b; p < e; p += 8) {
        uint8_t *mod; MsString nm; uint8_t on; char name[128] = "";
        if (!rd_mem(p, &mod, 8) || !mod) continue;
        if (!rd_mem(mod + g_modOnOff, &on, 1) || !rd_mem(mod + g_modNameOff, &nm, sizeof nm)) continue;
        if (nm.size >= sizeof name) nm.size = sizeof name - 1;
        if (nm.cap > 15) { if (!rd_mem(nm.u.ptr, name, nm.size)) continue; } else memcpy(name, nm.u.buf, nm.size);
        name[nm.size] = 0;
        if (!on) continue;
        if (seen && sl < seenN - 2) sl += snprintf(seen + sl, seenN - sl, "%s\"%s\"", sl ? ", " : "", name);
        /* case-insensitive substring */
        size_t L = strlen(g_requireMod), N = strlen(name);
        for (size_t i = 0; i + L <= N; i++) if (!_strnicmp(name + i, g_requireMod, L)) { hit = 1; break; }
    }
    return hit;
}
#endif /* BT_TEST */

#ifndef BT_TEST
static int is_human(void *e)
{
    void *vt = *(void **)e;
    for (int i = 0; i < g_humanVtN; i++) if (g_humanVt[i] == vt) return 1;
    return 0;
}

static void tagview_tick(void)
{
    static int lastKey, applied, warned;
    if (!g_vTagsToggle) return;
    if (!applied) {                                   /* ini state, applied once the engine is up */
        applied = 1;
        void *var = g_varLookup("v_tags");
        if (var && var != g_vTagsValue - 0x10 && !warned++) logf_("v_tags: registry object %p differs from static record %p - using the registry one", var, g_vTagsValue - 0x10);
        if (var) g_vTagsValue = (uint8_t *)var + 0x10;
        if (g_tagView && !*g_vTagsValue) { g_vTagsToggle(); logf_("Tag view ON (ini)"); }
    }
    if (g_isEditor || !g_tagViewKey) return;          /* the editor binds Alt+T itself */
    int down = (GetAsyncKeyState(g_tagViewKey) & 0x8000) && (!g_tagViewAlt || (GetAsyncKeyState(VK_MENU) & 0x8000));
    if (down && !lastKey) { g_vTagsToggle(); logf_("Tag view %s", *g_vTagsValue ? "ON" : "OFF"); }
    lastKey = down;
}

static void overlay_tick(void)
{
    static ULONGLONG last; static int lastKey;
    if (!g_modActive) return;
    tagview_tick();
    if (g_showHotkey) {
        int down = (GetAsyncKeyState(g_showHotkey) & 0x8000) != 0;
        if (down && !lastKey) { g_showTags = !g_showTags; logf_("Tag overlay %s", g_showTags ? "ON" : "OFF"); }
        lastKey = down;
    }
    if (!g_showTags || !g_damageReport || !g_entList) return;
    ULONGLONG now = GetTickCount64();
    if (now - last < (ULONGLONG)g_showInterval) return;
    last = now;

    uint8_t **eb = (uint8_t **)g_entList[0], **ee = (uint8_t **)g_entList[1];
    if (!eb || ee < eb || ee - eb > 100000) return;
    int shown = 0;
    char text[480], heap[480], tagn[64];
    for (uint8_t **it = eb; it < ee && shown < g_showMax; it++) {
        uint8_t *e = *it;
        if (!e || !(e[ENT_FLAGS] & 2)) continue;
        uint8_t *ec = e + g_entTagsOff;
        if (!container_ok(ec)) continue;
        uint8_t *tb = *(uint8_t **)(ec + TC_VEC_BEG), *te = *(uint8_t **)(ec + TC_VEC_END);
        if (tb == te || te < tb || te - tb > 0x60 * 256) continue;
        size_t len = 0; int match = !g_showFilter[0];
        len += snprintf(text, sizeof text, "%s", g_showStyle);
        if (is_human(e)) {
            uint8_t *nm = e + g_humanBreedOff - 0x20;                 /* breed name string (Tag at breedOff-0x60) */
            char bn[64]; tag_name(nm - TAG_STR, bn, sizeof bn);
            len += snprintf(text + len, sizeof text - len, "[%s]", bn);
        }
        for (uint8_t *t = tb; t < te && len < sizeof text - 70; t += TAG_SIZE) {
            tag_name(t, tagn, sizeof tagn);
            if (!match && strstr(tagn, g_showFilter)) match = 1;
            len += snprintf(text + len, sizeof text - len, " %s", tagn);
        }
        if (!match) continue;
        if (len >= sizeof text - 70) snprintf(text + len, sizeof text - len, " ...");

        MsString bone, msg; char boneHeap[64];
        msstr_init(&bone, g_showBone, boneHeap, sizeof boneHeap);
        msstr_init(&msg, text, heap, sizeof heap);
        __attribute__((aligned(16))) uint8_t tag[0x80];
        g_tagFromStr(tag, &bone);
        g_damageReport(e, tag, &msg);
        g_tagDtor2(tag);
        /* pin the entry so it neither animates nor expires: creation = expiry - lifetime */
        uint8_t *rb = g_reportVec[0], *re = g_reportVec[1];
        if (rb && re > rb && re - rb < RPT_SIZE * 4096)
            for (uint8_t *r = rb; r < re; r += RPT_SIZE)
                if (*(uint8_t **)(r + RPT_ENTITY) == e && *(int32_t *)(r + RPT_TYPE) == 0) {
                    static int32_t lifetime;
                    int32_t created = *(int32_t *)(r + RPT_CREATED), expires = *(int32_t *)(r + RPT_EXPIRES);
                    if (!lifetime && expires > created) lifetime = expires - created;
                    if (lifetime) *(int32_t *)(r + RPT_CREATED) = expires - lifetime;
                }
        shown++;
    }
}

static int hook_tagged_eval(void *cond, void **ctx)
{
    int r = g_taggedEvalOrig(cond, ctx);
    overlay_tick();
    return r;
}

static int install_overlay(void)
{
    DWORD old;
    if (!VirtualProtect(g_taggedVtSlot, 8, PAGE_READWRITE, &old)) { logf_("overlay: VirtualProtect failed"); return 0; }
    *g_taggedVtSlot = (uint8_t *)hook_tagged_eval;
    VirtualProtect(g_taggedVtSlot, 8, old, &old);
    return 1;
}

#include <tlhelp32.h>
/* Suspend every other thread, making sure none is stopped inside the bytes we are about to overwrite. */
static int suspend_others(HANDLE *hs, int max, uint8_t *lo, uint8_t *hi)
{
    for (int attempt = 0; attempt < 20; attempt++) {
        int n = 0, bad = 0;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap == INVALID_HANDLE_VALUE) return 0;
        THREADENTRY32 te = { sizeof te };
        for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
            if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == GetCurrentThreadId() || n >= max) continue;
            HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID);
            if (!h) continue;
            if (SuspendThread(h) == (DWORD)-1) { CloseHandle(h); continue; }
            CONTEXT c; c.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(h, &c) && (uint8_t *)c.Rip >= lo && (uint8_t *)c.Rip < hi) bad = 1;
            hs[n++] = h;
        }
        CloseHandle(snap);
        if (!bad) return n;
        for (int i = 0; i < n; i++) { ResumeThread(hs[i]); CloseHandle(hs[i]); }
        Sleep(5);
    }
    return 0;
}

static void resume_others(HANDLE *hs, int n) { for (int i = 0; i < n; i++) { ResumeThread(hs[i]); CloseHandle(hs[i]); } }

static int install_hook(void)
{
    uint8_t *f = (uint8_t *)g_applyBreed;
    /* the first 14 bytes are position independent in every known build:
       mov [rsp+10],rbx ; mov [rsp+20],rsi ; push rbp ; push rdi ; push r12 */
    static const uint8_t expect[] = { 0x48,0x89,0x5C,0x24,0x10, 0x48,0x89,0x74,0x24,0x20, 0x55, 0x57, 0x41,0x54 };
    if (memcmp(f, expect, sizeof expect)) { logf_("ApplyBreed prologue changed - not hooking"); return 0; }

    uint8_t *tramp = VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) { logf_("VirtualAlloc failed"); return 0; }
    memcpy(tramp, f, 14);
    tramp[14] = 0xFF; tramp[15] = 0x25; *(uint32_t *)(tramp + 16) = 0;
    *(uint64_t *)(tramp + 20) = (uint64_t)(f + 14);
    g_applyBreedOrig = (ApplyBreedFn)tramp;

    uint8_t patch[14] = { 0xFF, 0x25, 0, 0, 0, 0 };
    *(uint64_t *)(patch + 6) = (uint64_t)hook_apply_breed;
    DWORD old;
    if (!VirtualProtect(f, 14, PAGE_EXECUTE_READWRITE, &old)) { logf_("VirtualProtect failed (%lu)", GetLastError()); return 0; }
    HANDLE hs[256]; int n = suspend_others(hs, 256, f, f + 14);      /* no-op at DllMain time, needed from the watcher thread */
    memcpy(f, patch, 14);
    resume_others(hs, n);
    VirtualProtect(f, 14, old, &old);
    FlushInstructionCache(GetCurrentProcess(), f, 14);
    return 1;
}

#endif /* BT_TEST */
/* ------------------------------------------------------------------------------------ */
#ifndef BT_TEST
static int install_all(void);
static DWORD WINAPI mod_watch_thread(LPVOID arg);
static void init(HMODULE self)
{
    GetModuleFileNameW(self, g_dir, MAX_PATH);
    wchar_t *bs = wcsrchr(g_dir, L'\\'); if (bs) *bs = 0;

    wchar_t exe[MAX_PATH]; GetModuleFileNameW(NULL, exe, MAX_PATH);
    wchar_t *en = wcsrchr(exe, L'\\'); en = en ? en + 1 : exe;
    if (_wcsnicmp(en, L"call_to_arms", 12)) return;        /* only the game / editor / server exes */
    if (!ini_int("Enabled", 1)) return;
    g_mainTid = GetCurrentThreadId();                 /* DllMain runs on the game's main thread (proxy load or launcher APC) */
    g_logfile = ini_int("LogFile", 1);
    g_console = ini_int("Console", 0);
    g_verbose = ini_int("Verbose", 1);
    if (g_logfile) {
        wchar_t lp[MAX_PATH]; wcscpy(lp, g_dir); wcscat(lp, BT_LOG);
        g_log = _wfopen(lp, L"w");
    }
    if (g_console) { AllocConsole(); SetConsoleTitleA(BT_NAME); freopen("CONOUT$", "w", stdout); }

    if (!get_sections()) { logf_("not a PE I understand"); return; }
    logf_(BT_NAME " " BT_VERSION " loaded into %ls (timestamp %08X)", en, g_timestamp);

    ini_str("RequireMod", "", g_requireMod, sizeof g_requireMod);
    g_isEditor = !_wcsnicmp(en, L"call_to_arms_ed", 15);
    if (g_requireMod[0]) {
        if (!find_mod_list()) { logf_("Mod gate: cannot read the mod list - staying INACTIVE (RequireMod=%s)", g_requireMod); return; }
        g_modActive = 0;
        logf_("Mod gate: waiting for an enabled mod matching \"%s\" - nothing is patched until then", g_requireMod);
        CreateThread(NULL, 0, mod_watch_thread, NULL, 0, NULL);
        return;
    }
    install_all();
}

static int install_all(void)
{
#ifdef BT_VIEW_ONLY
    if (!find_tag_functions()) { logf_("engine code not recognised"); return 0; }
    logf_("Breed tags -> entity tags      not in this build (view-only, simulation untouched)");
#else
    int ok = find_apply_breed() & find_breed_tags_off() & find_tag_functions();
    if (!ok) { logf_("Breed tags -> entity tags      OFF (engine code not recognised)"); return 0; }
    g_removeOld = ini_int("RemoveOldBreedTags", 1);
    g_verboseMax = ini_int("VerboseMax", 200);
    ini_str("BreedEvent", "breed_applied", g_breedEvent, sizeof g_breedEvent);
    find_apply_breed_callers();
    if (g_breedEvent[0]) g_okEvent = find_named_event();
    if (!install_hook()) { logf_("Breed tags -> entity tags      OFF"); return 0; }
    logf_("Breed tags -> entity tags      ON");
    logf_("Old breed tags on a re-breed   %s", !g_removeOld ? "kept (ini)" : g_tagRemoveAll ? "removed" : "kept (remove_all not recognised)");
    logf_("{on \"%s\"} event        %s", g_breedEvent[0] ? g_breedEvent : "breed_applied",
          !g_breedEvent[0] ? "off (ini)" : g_okEvent ? "ON (fired after every breed applied to a unit in the scene)" : "OFF (engine code not recognised)");
#endif

    g_showTags = ini_int("ShowTags", 0);
    g_showHotkey = ini_int("ShowTagsHotkey", 120);
    g_showMax = ini_int("ShowTagsMax", 60);
    g_showInterval = ini_int("ShowTagsInterval", 500);
    ini_str("ShowTagsFilter", "", g_showFilter, sizeof g_showFilter);
    ini_str("ShowTagsBone", "basis", g_showBone, sizeof g_showBone);
    ini_str("ShowTagsStyle", "<s(12)>", g_showStyle, sizeof g_showStyle);
    g_tagView = ini_int("TagView", 0);
    g_tagViewKey = ini_int("TagViewKey", 'T');
    g_tagViewAlt = ini_int("TagViewAlt", 1);

    int okView = find_v_tags();
    int okOverlay = find_damage_report() & find_entity_list();
    if (!find_tagged_condition() || !install_overlay()) { logf_("Tag view / overlay             OFF (tick hook not available)"); return 1; }
    logf_("Engine tag view (v_tags)       %s", !okView ? "OFF (not recognised)" : g_isEditor ? "editor: native Alt+T" : g_tagViewKey ? (g_tagViewAlt ? "ready (Alt+key)" : "ready (key)") : "ini only");
    if (!okOverlay) { g_showTags = 0; g_showHotkey = 0; logf_("Floating tag labels            OFF (engine code not recognised)"); }
    else logf_("Floating tag labels            %s (hotkey %d%s)", g_showTags ? "ON" : "ready", g_showHotkey, g_showFilter[0] ? ", filtered" : "");
    return 1;
}

/* Polls the engine's mod list. First time the required mod is enabled: install everything. After that
   the flag follows the list, so the DLL goes inert again if the mod is switched off. */
static DWORD WINAPI mod_watch_thread(LPVOID arg)
{
    (void)arg;
    static int installed, lastState = -2, logged;
    char seen[512];
    for (;;) {
        Sleep(installed ? 2000 : 500);
        int st = mod_enabled(seen, sizeof seen);
        if (st != lastState) {
            if (st > 0 && !logged++) logf_("Mod gate: enabled mods: %s", seen);
            lastState = st;
            if (st > 0 && !installed) { installed = 1; logf_("Mod gate: \"%s\" is enabled - installing", g_requireMod); install_all(); g_modActive = 1; }
            else if (installed) { g_modActive = st > 0; logf_("Mod gate: \"%s\" %s - BreedTags %s", g_requireMod, st > 0 ? "enabled" : "disabled", st > 0 ? "active" : "inert"); }
        }
    }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        load_real_dbghelp();
        init(h);
    }
    return TRUE;
}
#endif /* BT_TEST */
#ifdef BT_TEST
#include <stdio.h>
int main(int argc, char **argv)
{
    FILE *f = fopen(argv[1], "rb"); fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *file = malloc(n); fread(file, 1, n, f); fclose(f);
    uint32_t pe = *(uint32_t *)(file + 0x3c);
    uint16_t nsec = *(uint16_t *)(file + pe + 6), opt = *(uint16_t *)(file + pe + 20);
    uint64_t base = *(uint64_t *)(file + pe + 24 + 24);
    uint32_t imgsz = *(uint32_t *)(file + pe + 24 + 56);
    g_base = mmap((void *)base, imgsz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (g_base != (uint8_t *)base) { printf("mmap failed\n"); return 1; }
    for (int i = 0; i < nsec; i++) {
        uint8_t *sh = file + pe + 24 + opt + 40 * i;
        uint32_t vs = *(uint32_t *)(sh + 8), va = *(uint32_t *)(sh + 12), rs = *(uint32_t *)(sh + 16), raw = *(uint32_t *)(sh + 20);
        memcpy(g_base + va, file + raw, rs < vs ? rs : vs);
        Sec s = { g_base + va, vs };
        if (!strncmp((char *)sh, ".text", 8)) g_text = s; else if (!strncmp((char *)sh, ".rdata", 8)) g_rdata = s; else if (!strncmp((char *)sh, ".data", 8)) g_data = s;
    }
    printf("%s timestamp %08X\n", argv[1], *(uint32_t *)(file + pe + 8));
    int ok = find_apply_breed() & find_breed_tags_off() & find_tag_functions();
    ok &= find_tagged_condition() & find_damage_report() & find_entity_list() & find_v_tags() & find_mod_list();
    printf("--- 0.6\n");
    ok &= g_tagRemoveAll != NULL;
    ok &= find_named_event();
    find_apply_breed_callers();
    { int d = 0, a = 0; for (int k = 0; k < g_abSiteN; k++) { if (g_abSiteDeferred[k]) d++; else a++; } ok &= d >= 1 && a >= 1; }
    static const uint8_t expect[] = { 0x48,0x89,0x5C,0x24,0x10, 0x48,0x89,0x74,0x24,0x20, 0x55, 0x57, 0x41,0x54 };
    if (ok) printf("prologue ok: %d\n", !memcmp((void*)g_applyBreed, expect, 14));
    printf(ok ? "ALL FOUND\n" : "FAILED\n");
    return !ok;
}
#endif
