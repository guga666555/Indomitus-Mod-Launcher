/*
 * GEM NoProne 0.2  -  Call to Arms: Gates of Hell (GEM engine)
 *
 * Stops the AI from taking the prone pose for units whose breed carries a tag
 * (default "cannot_lie", the same name the engine already uses on stuff items:
 * rpg_weapon.pattern etc.). Player stance orders are not touched.
 *
 * Ships as a VCOMP140.dll proxy (game, editor and server all import it from System32).
 * Every export is forwarded to %SystemRoot%\System32\vcomp140.dll.
 *
 * How it works (all addresses found at runtime, nothing hard-coded per version):
 *   - The engine builds a global Tag "cannot_lie" in a static initialiser. Its only reader is
 *     the AI's CanTakePose(brain, pose) (eHumanBrain.cpp): for pose 1 (lie) it scans the held
 *     item's stuff tags for it and returns false. Pose 0 = stand, 2 = squat. Every caller
 *     (enemy reaction, injured crawl, return-to-zone) is AI brain code.
 *   - Hook: 14-byte inline jmp on CanTakePose. For pose 1 it reads brain->human (the
 *     'mov rcx,[rsi+off]' in its body, 0x38), checks the engine's own human flag (byte +0x30,
 *     bit 2, as the sibling brain function does), then human->breed (from ApplyBreed's
 *     'cmp [rcx+off],reg', 0x1390 game/editor, 0x1388 server) and the breed {tags} container
 *     (from the breed parser, 0x128 game/server, 0x130 editor). Tag found -> return false,
 *     as if holding an RPG.
 *
 * Build (zig / mingw-w64):  build.bat.   Offline check:  build.bat test  ->  np_test.exe <exe>
 */
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>

#define NP_VERSION "0.3"
#define NP_NAME    "GEM NoProne"
#define NP_INI     L"\\gem_noprone.ini"
#define NP_LOG     L"\\gem_noprone.log"
#define NP_SECT    L"noprone"

#ifndef NP_TEST
/* ------------------------------------------------------------------------------------ */
/* vcomp140 forwarders                                                                  */
/* ------------------------------------------------------------------------------------ */
#define FWD(name) FARPROC gnp_##name;
#include "proxy_exports.h"
#undef FWD

#define FWD(name) \
    __declspec(dllexport) __attribute__((naked)) void name(void) { \
        __asm__ volatile("jmp *gnp_" #name "(%rip)"); }
#include "proxy_exports.h"
#undef FWD

static void load_real_dll(void)
{
    wchar_t path[MAX_PATH];
    UINT n = GetSystemDirectoryW(path, MAX_PATH);
    if (!n) return;
    wcscat(path, L"\\vcomp140.dll");
    HMODULE real = LoadLibraryW(path);
    if (!real) return;
#define FWD(name) gnp_##name = GetProcAddress(real, #name);
#include "proxy_exports.h"
#undef FWD
}
#endif /* NP_TEST */

/* ------------------------------------------------------------------------------------ */
/* logging / ini                                                                        */
/* ------------------------------------------------------------------------------------ */
#ifdef NP_TEST
static void logf_(const char *fmt, ...){ va_list ap; va_start(ap,fmt); vprintf(fmt,ap); va_end(ap); printf("\n"); }
#else
static int g_verbose = 1;
static FILE *g_log;
static int g_console;
static wchar_t g_dir[MAX_PATH];
static CRITICAL_SECTION g_logLock;

static void logf_(const char *fmt, ...)
{
    char buf[1024];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    SYSTEMTIME st; GetLocalTime(&st);
    char line[1100];
    snprintf(line, sizeof line, "%02d:%02d:%02d [" NP_NAME "] %s\n", st.wHour, st.wMinute, st.wSecond, buf);
    EnterCriticalSection(&g_logLock);
    if (g_log) { fputs(line, g_log); fflush(g_log); }
    if (g_console) { fputs(line, stdout); fflush(stdout); }
    LeaveCriticalSection(&g_logLock);
    OutputDebugStringA(line);
}

static void ini_str(const char *key, const char *def, char *out, size_t n)
{
    wchar_t ini[MAX_PATH]; wcscpy(ini, g_dir); wcscat(ini, NP_INI);
    wchar_t wkey[64], wdef[128], wout[256]; MultiByteToWideChar(CP_ACP, 0, key, -1, wkey, 64); MultiByteToWideChar(CP_ACP, 0, def, -1, wdef, 128);
    GetPrivateProfileStringW(NP_SECT, wkey, wdef, wout, 256, ini);
    WideCharToMultiByte(CP_ACP, 0, wout, -1, out, (int)n, NULL, NULL);
}

static int ini_int(const char *key, int def)
{
    wchar_t ini[MAX_PATH]; wcscpy(ini, g_dir); wcscat(ini, NP_INI);
    wchar_t wkey[64]; MultiByteToWideChar(CP_ACP, 0, key, -1, wkey, 64);
    return (int)GetPrivateProfileIntW(NP_SECT, wkey, def, ini);
}
#endif /* NP_TEST */

/* ------------------------------------------------------------------------------------ */
/* PE helpers                                                                           */
/* ------------------------------------------------------------------------------------ */
typedef struct { uint8_t *p; size_t n; } Sec;
static Sec g_text, g_rdata, g_data;
static uint8_t *g_base;

#ifndef NP_TEST
static uint32_t g_timestamp;
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
#endif

static int in_text(const void *a) { return (uint8_t *)a >= g_text.p && (uint8_t *)a < g_text.p + g_text.n; }
static int in_rdata(const void *a) { return (uint8_t *)a >= g_rdata.p && (uint8_t *)a < g_rdata.p + g_rdata.n; }

static uint8_t *find_bytes(uint8_t *p, size_t n, const uint8_t *pat, size_t m)
{
    if (m > n) return NULL;
    for (size_t i = 0; i + m <= n; i++)
        if (p[i] == pat[0] && !memcmp(p + i, pat, m)) return p + i;
    return NULL;
}

/* a whole NUL-terminated string in .rdata (preceded by NUL, so not a suffix of a longer one) */
static uint8_t *find_cstr(const char *s)
{
    size_t m = strlen(s) + 1;
    for (uint8_t *p = g_rdata.p; (p = find_bytes(p, g_rdata.p + g_rdata.n - p, (const uint8_t *)s, m)); p++)
        if (p == g_rdata.p || p[-1] == 0) return p;
    return NULL;
}

/* k-th RIP-relative 'lea reg,[rip+disp]' in .text whose target == want; modrm < 0 = any register */
static uint8_t *find_lea_to(uint8_t *from, size_t n, const void *want, int modrm, int which)
{
    int k = 0;
    for (size_t i = 0; i + 7 <= n; i++) {
        uint8_t r = from[i];
        if ((r & 0xF8) != 0x48 || from[i + 1] != 0x8D || (from[i + 2] & 0xC7) != 0x05) continue;
        if (modrm >= 0 && from[i + 2] != modrm) continue;
        uint8_t *t = from + i + 7 + *(int32_t *)(from + i + 3);
        if (t == want) { if (k++ == which) return from + i; }
    }
    return NULL;
}

/* ------------------------------------------------------------------------------------ */
/* engine bindings                                                                      */
/* ------------------------------------------------------------------------------------ */
typedef char (*CanTakePoseFn)(void *brain, int pose);
static CanTakePoseFn g_canTakePose;
static uint32_t g_brainHumanOff, g_humanBreedOff, g_breedTagsOff, g_entTagsOff;

#define POSE_LIE    1
#define ENT_FLAGS   0x30    /* entity flag byte; bit 2 = human (the engine's own test in eHumanBrain) */
#define ENT_HUMAN   0x04
#define TC_SELF_A   0x308   /* TagContainer: owner back-pointers (== container itself) */
#define TC_SELF_B   0x310
#define TC_VEC_BEG  0x318
#define TC_VEC_END  0x320
#define TAG_SIZE    0x60    /* vector element */
#define TAG_STR     0x40    /* MSVC std::string inside the element */

/* CanTakePose prologue, position independent:
   mov [rsp+8],rbx ; mov [rsp+10],rsi ; push rdi ; sub rsp,20h  (15 bytes) */
static const uint8_t k_prologue[] = { 0x48,0x89,0x5C,0x24,0x08, 0x48,0x89,0x74,0x24,0x10, 0x57, 0x48,0x83,0xEC,0x20 };

static int find_can_take_pose(void)
{
    uint8_t *str = find_cstr("cannot_lie");
    if (!str) { logf_("CanTakePose: 'cannot_lie' string not found"); return 0; }
    /* static initialiser: lea rdx,"cannot_lie" ; lea rcx,g_tag ; call Tag::Tag */
    uint8_t *init = find_lea_to(g_text.p, g_text.n, str, 0x15, 0);
    if (!init || find_lea_to(g_text.p, g_text.n, str, -1, 1)) { logf_("CanTakePose: string xref missing or ambiguous"); return 0; }
    uint8_t *tag = NULL;
    for (uint8_t *q = init + 7; q < init + 0x20; q++)
        if (q[0] == 0x48 && q[1] == 0x8D && q[2] == 0x0D) { tag = q + 7 + *(int32_t *)(q + 3); break; }
    if (!tag) { logf_("CanTakePose: global tag not found"); return 0; }
    /* the reader: lea rdx,g_tag inside the stuff-tag loop of CanTakePose (the dtor uses lea rcx) */
    uint8_t *use = find_lea_to(g_text.p, g_text.n, tag, 0x15, 0);
    if (!use || find_lea_to(g_text.p, g_text.n, tag, 0x15, 1)) { logf_("CanTakePose: tag reader missing or ambiguous"); return 0; }
    uint8_t *f = NULL;
    for (uint8_t *p = use; p >= use - 0x300 && p > g_text.p; p--)
        if (!memcmp(p, k_prologue, sizeof k_prologue) && p[-1] == 0xCC) { f = p; break; }
    if (!f) { logf_("CanTakePose: prologue not found"); return 0; }
    /* mov ebx,edx ; mov rsi,rcx  then the first  mov rcx,[rsi+disp8]  = brain->human */
    if (memcmp(f + 15, "\x8B\xDA\x48\x8B\xF1", 5)) { logf_("CanTakePose: body not recognised"); return 0; }
    for (uint8_t *q = f + 20; q < f + 0x40; q++)
        if (q[0] == 0x48 && q[1] == 0x8B && q[2] == 0x4E) { g_brainHumanOff = q[3]; break; }
    if (!g_brainHumanOff) { logf_("CanTakePose: brain->human offset not found"); return 0; }
    /* the lie-only branch: cmp ebx,1 before the tag loop */
    if (!find_bytes(f, use - f, (const uint8_t *)"\x83\xFB\x01", 3)) { logf_("CanTakePose: lie branch not found"); return 0; }
    g_canTakePose = (CanTakePoseFn)f;
    logf_("CanTakePose = exe+0x%llX, brain->human @ +0x%X", (unsigned long long)(f - g_base), g_brainHumanOff);
    return 1;
}

static int find_human_breed_off(void)
{
    uint8_t *s = find_cstr("Unknown breed \"%s\"");
    if (!s) { logf_("ApplyBreed: string not found"); return 0; }
    uint8_t *lea = find_lea_to(g_text.p, g_text.n, s, -1, 0);
    if (!lea || find_lea_to(g_text.p, g_text.n, s, -1, 1)) { logf_("ApplyBreed: string xref missing or ambiguous"); return 0; }
    static const uint8_t prologue[] = { 0x48,0x89,0x5C,0x24,0x10, 0x48,0x89,0x74,0x24,0x20, 0x55, 0x57, 0x41,0x54, 0x41,0x56, 0x41,0x57 };
    uint8_t *f = NULL;
    for (uint8_t *p = lea; p >= lea - 0x300 && p > g_text.p; p--)
        if (!memcmp(p, prologue, sizeof prologue) && p[-1] == 0xCC) { f = p; break; }
    if (!f) { logf_("ApplyBreed: prologue not found"); return 0; }
    /* cmp qword [rcx+disp32], reg  ->  48/4C 39 modrm(mod=10,rm=001) disp32 */
    for (uint8_t *p = f; p < lea - 7; p++)
        if ((p[0] == 0x48 || p[0] == 0x4C) && p[1] == 0x39 && (p[2] & 0xC7) == 0x81) {
            uint32_t off = *(uint32_t *)(p + 3);
            if (off > 0x800 && off < 0x4000) { g_humanBreedOff = off; break; }
        }
    if (!g_humanBreedOff) { logf_("ApplyBreed: human->breed offset not found"); return 0; }
    logf_("human->breed @ +0x%X", g_humanBreedOff);
    return 1;
}

static int find_breed_tags_off(void)
{
    /* breed parser (eHumanBreed.cpp): ... lea reg,[breed+off] ... read("tags") ... read("nationality") */
    uint8_t *snat = find_cstr("nationality");
    if (!snat) { logf_("breed tags: 'nationality' not found"); return 0; }
    uint8_t *lnat = find_lea_to(g_text.p, g_text.n, snat, -1, 0);
    if (!lnat) { logf_("breed tags: 'nationality' xref not found"); return 0; }
    uint8_t *ltags = NULL;
    for (uint8_t *p = lnat - 7; p > lnat - 0x600 && p > g_text.p; p--) {
        if ((p[0] & 0xF8) != 0x48 || p[1] != 0x8D || (p[2] & 0xC7) != 0x05) continue;
        uint8_t *t = p + 7 + *(int32_t *)(p + 3);
        if (in_rdata(t) && !memcmp(t, "tags", 5) && t[-1] == 0) { ltags = p; break; }
    }
    if (!ltags) { logf_("breed tags: 'tags' read not found in breed parser"); return 0; }
    /* the lea reg,[reg+disp32] just before it: 4? 8D modrm(mod=10) [sib] disp32 */
    for (uint8_t *p = ltags - 7; p > ltags - 0x80; p--) {
        if ((p[0] & 0xF0) != 0x40 || p[1] != 0x8D || (p[2] & 0xC0) != 0x80) continue;
        int sib = (p[2] & 7) == 4;
        uint32_t off = *(uint32_t *)(p + 3 + sib);
        if (off >= 0x80 && off <= 0x400) { g_breedTagsOff = off; break; }
    }
    if (!g_breedTagsOff) { logf_("breed tags: container offset not found"); return 0; }
    logf_("breed {tags} @ breed+0x%X", g_breedTagsOff);
    return 1;
}

static int find_entity_tags_off(void)
{
    /* interaction action table in .data: { "tags", ctor } -> lea rax,vtable -> vtable[1] = eval:
       add rdx,imm32 (entity script tag container) ; call copy_ctor */
    uint8_t *ctor = NULL; int hits = 0;
    for (size_t i = 0; i + 16 <= g_data.n; i += 8) {
        uint8_t *str = *(uint8_t **)(g_data.p + i), *fn = *(uint8_t **)(g_data.p + i + 8);
        if (!in_rdata(str) || !in_text(fn)) continue;
        if (memcmp(str, "tags", 5) || str[-1] != 0) continue;
        ctor = fn; hits++;
    }
    if (hits != 1) { logf_("entity tags: action table entry %s", hits ? "ambiguous" : "not found"); return 0; }
    uint8_t *vt = NULL;
    for (uint8_t *p = ctor; p < ctor + 0x80; p++)
        if (p[0] == 0x48 && p[1] == 0x8D && p[2] == 0x05) { vt = p + 7 + *(int32_t *)(p + 3); break; }
    if (!vt || !in_rdata(vt)) { logf_("entity tags: vtable not found"); return 0; }
    uint8_t *ev = *(uint8_t **)(vt + 8);
    if (!in_text(ev)) { logf_("entity tags: eval not in .text"); return 0; }
    for (uint8_t *q = ev; q < ev + 0x100; q++)
        if (q[0] == 0x48 && q[1] == 0x81 && q[2] == 0xC2) { g_entTagsOff = *(uint32_t *)(q + 3); break; }
    if (g_entTagsOff < 0x100 || g_entTagsOff > 0x400) { logf_("entity tags: container offset not found"); g_entTagsOff = 0; return 0; }
    logf_("entity {tags} @ entity+0x%X", g_entTagsOff);
    return 1;
}

#ifndef NP_TEST
/* ------------------------------------------------------------------------------------ */
/* hook                                                                               */
/* ------------------------------------------------------------------------------------ */
static CanTakePoseFn g_canTakePoseOrig;
static char g_tag[64] = "cannot_lie";
static size_t g_tagLen;

typedef struct { union { char buf[16]; char *ptr; } u; size_t size, cap; } MsString;

static int rd_mem(const void *src, void *dst, size_t n)
{
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), src, dst, n, &got) && got == n;
}

static int rd_msstr(const uint8_t *at, char *out, size_t n)
{
    MsString s; out[0] = 0;
    if (!rd_mem(at, &s, sizeof s)) return 0;
    if (s.size >= n) s.size = n - 1;
    if (s.cap > 15) { if (!rd_mem(s.u.ptr, out, s.size)) return 0; } else memcpy(out, s.u.buf, s.size);
    out[s.size] = 0;
    return 1;
}

static int has_tag(const uint8_t *c)
{
    if (*(const uint8_t *const *)(c + TC_SELF_A) != c || *(const uint8_t *const *)(c + TC_SELF_B) != c) return 0;
    const uint8_t *b = *(const uint8_t *const *)(c + TC_VEC_BEG), *e = *(const uint8_t *const *)(c + TC_VEC_END);
    if (!b || e < b || (size_t)(e - b) > TAG_SIZE * 256) return 0;
    for (const uint8_t *t = b; t < e; t += TAG_SIZE) {
        const char *s = (const char *)(t + TAG_STR);
        size_t len = *(const size_t *)(t + TAG_STR + 0x10), cap = *(const size_t *)(t + TAG_STR + 0x18);
        if (len != g_tagLen) continue;
        if (cap > 15) s = *(const char *const *)(t + TAG_STR);
        if (!_strnicmp(s, g_tag, len)) return 1;    /* engine lowercases tags; ini value may not be */
    }
    return 0;
}

/* first block per breed + source goes to the log (breed name = the Tag just before the breed pointer) */
static const void *g_seen[256]; static int g_seenN;
static void log_block(const uint8_t *human, const uint8_t *breed, int fromEntity)
{
    const void *key = (const uint8_t *)breed + fromEntity;
    for (int i = 0; i < g_seenN; i++) if (g_seen[i] == key) return;
    if (g_seenN >= 256) return;
    g_seen[g_seenN++] = key;
    char name[96];
    if (!rd_msstr(human + g_humanBreedOff - TAG_SIZE + TAG_STR, name, sizeof name)) strcpy(name, "?");
    logf_("prone blocked for breed \"%s\" (%s tag)", name, fromEntity ? "entity" : "breed");
}

static char hook_can_take_pose(void *brain, int pose)
{
    if (pose == POSE_LIE) {
        const uint8_t *h = *(const uint8_t *const *)((const uint8_t *)brain + g_brainHumanOff);
        if (h && (h[ENT_FLAGS] & ENT_HUMAN)) {
            const uint8_t *breed = *(const uint8_t *const *)(h + g_humanBreedOff);
            int fromBreed = breed && has_tag(breed + g_breedTagsOff);
            int fromEntity = !fromBreed && g_entTagsOff && has_tag(h + g_entTagsOff);
            if (fromBreed || fromEntity) {
                if (g_verbose) log_block(h, breed, fromEntity);
                return 0;
            }
        }
    }
    return g_canTakePoseOrig(brain, pose);
}

/* runs from DllMain, before any engine code, so nothing can be executing the patched bytes */
static int install_hook(void)
{
    uint8_t *f = (uint8_t *)g_canTakePose;
    if (memcmp(f, k_prologue, sizeof k_prologue)) { logf_("CanTakePose prologue changed - not hooking"); return 0; }

    uint8_t *tramp = VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) { logf_("VirtualAlloc failed"); return 0; }
    memcpy(tramp, f, sizeof k_prologue);
    tramp[15] = 0xFF; tramp[16] = 0x25; *(uint32_t *)(tramp + 17) = 0;
    *(uint64_t *)(tramp + 21) = (uint64_t)(f + sizeof k_prologue);
    g_canTakePoseOrig = (CanTakePoseFn)tramp;

    uint8_t patch[15] = { 0xFF, 0x25, 0, 0, 0, 0 };
    *(uint64_t *)(patch + 6) = (uint64_t)hook_can_take_pose;
    patch[14] = 0x90;
    DWORD old;
    if (!VirtualProtect(f, sizeof patch, PAGE_EXECUTE_READWRITE, &old)) { logf_("VirtualProtect failed (%lu)", GetLastError()); return 0; }
    memcpy(f, patch, sizeof patch);
    VirtualProtect(f, sizeof patch, old, &old);
    FlushInstructionCache(GetCurrentProcess(), f, sizeof patch);
    return 1;
}

/* ------------------------------------------------------------------------------------ */
static void init(HMODULE self)
{
    GetModuleFileNameW(self, g_dir, MAX_PATH);
    wchar_t *bs = wcsrchr(g_dir, L'\\'); if (bs) *bs = 0;

    wchar_t exe[MAX_PATH]; GetModuleFileNameW(NULL, exe, MAX_PATH);
    wchar_t *en = wcsrchr(exe, L'\\'); en = en ? en + 1 : exe;
    if (_wcsnicmp(en, L"call_to_arms", 12)) return;        /* only the game / editor / server exes */
    if (!ini_int("Enabled", 1)) return;
    g_console = ini_int("Console", 0);
    g_verbose = ini_int("Verbose", 1);
    if (ini_int("LogFile", 1)) {
        wchar_t lp[MAX_PATH]; wcscpy(lp, g_dir); wcscat(lp, NP_LOG);
        g_log = _wfopen(lp, L"w");
    }
    if (g_console) { AllocConsole(); SetConsoleTitleA(NP_NAME); freopen("CONOUT$", "w", stdout); }

    if (!get_sections()) { logf_("not a PE I understand"); return; }
    logf_(NP_NAME " " NP_VERSION " loaded into %ls (timestamp %08X)", en, g_timestamp);

    ini_str("Tag", "cannot_lie", g_tag, sizeof g_tag);
    g_tagLen = strlen(g_tag);
    if (!g_tagLen) { logf_("Tag= is empty - nothing to do"); return; }

    int ok = find_can_take_pose() & find_human_breed_off() & find_breed_tags_off();
    int okEnt = find_entity_tags_off();
    if (!ok || !install_hook()) { logf_("AI prone block                 OFF (engine code not recognised)"); return; }
    logf_("AI prone block                 ON (tag \"%s\" on breed%s)", g_tag, okEnt ? " or entity" : " only");
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        InitializeCriticalSection(&g_logLock);
        load_real_dll();
        init(h);
    }
    return TRUE;
}
#endif /* NP_TEST */

#ifdef NP_TEST
/* np_test.exe <exe> : map the exe's sections and run every finder (the post-update check) */
int main(int argc, char **argv)
{
    if (argc < 2) { printf("usage: np_test <call_to_arms*.exe>\n"); return 2; }
    FILE *f = fopen(argv[1], "rb"); if (!f) { printf("cannot open %s\n", argv[1]); return 2; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *file = malloc(n); fread(file, 1, n, f); fclose(f);
    uint32_t pe = *(uint32_t *)(file + 0x3c);
    uint16_t nsec = *(uint16_t *)(file + pe + 6), opt = *(uint16_t *)(file + pe + 20);
    uint32_t imgsz = *(uint32_t *)(file + pe + 24 + 56);
    uint64_t base = *(uint64_t *)(file + pe + 24 + 24);
    g_base = VirtualAlloc((void *)base, imgsz, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);   /* real base: the action table holds absolute pointers */
    if (g_base != (uint8_t *)base) { printf("cannot map at %llX\n", (unsigned long long)base); return 2; }
    for (int i = 0; i < nsec; i++) {
        uint8_t *sh = file + pe + 24 + opt + 40 * i;
        uint32_t vs = *(uint32_t *)(sh + 8), va = *(uint32_t *)(sh + 12), rs = *(uint32_t *)(sh + 16), raw = *(uint32_t *)(sh + 20);
        memcpy(g_base + va, file + raw, rs < vs ? rs : vs);
        Sec s = { g_base + va, vs };
        if (!strncmp((char *)sh, ".text", 8)) g_text = s; else if (!strncmp((char *)sh, ".rdata", 8)) g_rdata = s; else if (!strncmp((char *)sh, ".data", 8)) g_data = s;
    }
    printf("%s timestamp %08X\n", argv[1], *(uint32_t *)(file + pe + 8));
    int ok = find_can_take_pose() & find_human_breed_off() & find_breed_tags_off() & find_entity_tags_off();
    if (ok) printf("prologue ok: %d\n", !memcmp((void *)g_canTakePose, k_prologue, sizeof k_prologue));
    printf(ok ? "ALL FOUND\n" : "FAILED\n");
    return !ok;
}
#endif
