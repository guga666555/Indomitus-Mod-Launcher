/*
 * GEM NoProne 0.5  -  Call to Arms: Gates of Hell (GEM engine), Indomitus Launcher payload
 *
 * Stops the AI from taking the prone pose for units whose breed (or the unit itself) carries a
 * tag (default "cannot_lie", the name the engine already uses on stuff items: rpg_weapon.pattern).
 * Player stance orders are not touched.
 *
 * Loaded by the Indomitus Launcher (launcher\payload\gem_noprone.dll). No exports, no proxy.
 * The ini and the log live next to the DLL (the launcher's extracted payload folder).
 *
 * How it works (all addresses found at runtime, nothing hard-coded per version):
 *   - The engine builds a global Tag "cannot_lie" in a static initialiser. Its only reader is
 *     the AI's CanTakePose(brain, pose) (eHumanBrain.cpp): for pose 1 (lie) it scans the held
 *     item's stuff tags for it and returns false. Pose 0 = stand, 2 = squat. Every caller
 *     (enemy reaction, injured crawl, return-to-zone) is AI brain code.
 *   - Hook: 14-byte 'jmp [rip]' on CanTakePose. For pose 1 it reads brain->human (the
 *     'mov rcx,[rsi+off]' in its body, 0x38), checks the engine's own human flag (byte +0x30,
 *     bit 2, as the sibling brain function does), then human->breed (from ApplyBreed's
 *     'cmp [rcx+off],reg', 0x1390 game/editor, 0x1388 server), the breed {tags} container
 *     (from the breed parser, 0x128 game/server, 0x130 editor) and the entity {tags} container.
 *     Tag found -> return false, as if holding an RPG.
 *   - Function starts come from the exe's .pdata (exception directory), not from the first
 *     bytes of the function: other payload DLLs (BreedTags hooks ApplyBreed) overwrite those.
 *
 * Build (zig / mingw-w64):  build.bat           -> out\gem_noprone.dll
 *                           build.bat payload   -> same + copy dll and ini into launcher\payload\
 * Offline check:            build.bat test      -> out\np_test.exe <exe>  (also on Linux: tests\run_test.sh)
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
#ifdef NP_TEST
#  ifdef _WIN32
#    include <windows.h>
#  else
#    include <sys/mman.h>
#  endif
#else
#  include <windows.h>
#  include <tlhelp32.h>
#endif

#define NP_VERSION "0.5"
#define NP_NAME    "GEM NoProne"

/* ------------------------------------------------------------------------------------ */
/* logging / ini                                                                        */
/* ------------------------------------------------------------------------------------ */
#ifdef NP_TEST
static void logf_(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); printf("\n"); }
#else
#define NP_INI  L"\\gem_noprone.ini"
#define NP_LOG  L"\\gem_noprone.log"
#define NP_SECT L"noprone"

static FILE *g_log;
static int g_console, g_verbose = 1;
static wchar_t g_dir[MAX_PATH];
static CRITICAL_SECTION g_lock;      /* log output + the "already logged" list */

static void logf_(const char *fmt, ...)
{
    char buf[1024], line[1100];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    SYSTEMTIME st; GetLocalTime(&st);
    snprintf(line, sizeof line, "%02d:%02d:%02d [" NP_NAME "] %s\n", st.wHour, st.wMinute, st.wSecond, buf);
    EnterCriticalSection(&g_lock);
    if (g_log) { fputs(line, g_log); fflush(g_log); }
    if (g_console) { fputs(line, stdout); fflush(stdout); }
    LeaveCriticalSection(&g_lock);
    OutputDebugStringA(line);
}

static void ini_path(wchar_t *out) { wcscpy(out, g_dir); wcscat(out, NP_INI); }

static void ini_str(const wchar_t *key, const wchar_t *def, char *out, size_t n)
{
    wchar_t ini[MAX_PATH], w[256];
    ini_path(ini);
    GetPrivateProfileStringW(NP_SECT, key, def, w, 256, ini);
    if (!WideCharToMultiByte(CP_ACP, 0, w, -1, out, (int)n, NULL, NULL)) out[0] = 0;   /* too long -> empty */
    out[n - 1] = 0;
}

static int ini_int(const wchar_t *key, int def)
{
    wchar_t ini[MAX_PATH];
    ini_path(ini);
    return (int)GetPrivateProfileIntW(NP_SECT, key, def, ini);
}
#endif /* NP_TEST */

/* ------------------------------------------------------------------------------------ */
/* PE helpers (raw offsets, so the same code runs on the live exe and in the offline test) */
/* ------------------------------------------------------------------------------------ */
typedef struct { uint8_t *p; size_t n; } Sec;
static Sec g_text, g_rdata, g_data;
static uint8_t *g_base;
static uint32_t g_timestamp;
static const uint32_t *g_pdata;      /* RUNTIME_FUNCTION { begin, end, unwind } RVAs, sorted by begin */
static size_t g_pdataN;

/* image already mapped at g_base (headers + sections) */
static int read_pe(void)
{
    uint8_t *nt = g_base + *(uint32_t *)(g_base + 0x3C);
    if (memcmp(nt, "PE\0\0", 4) || *(uint16_t *)(nt + 24) != 0x20B) return 0;     /* PE32+ only */
    g_timestamp = *(uint32_t *)(nt + 8);
    uint16_t nsec = *(uint16_t *)(nt + 6), opt = *(uint16_t *)(nt + 20);
    uint8_t *dd = nt + 24 + 112;                                               /* data directories */
    if (*(uint32_t *)(nt + 24 + 108) > 3) {                                    /* [3] = exception (.pdata) */
        g_pdata  = (const uint32_t *)(g_base + *(uint32_t *)(dd + 3 * 8));
        g_pdataN = *(uint32_t *)(dd + 3 * 8 + 4) / 12;
    }
    for (int i = 0; i < nsec; i++) {
        uint8_t *sh = nt + 24 + opt + 40 * i;
        Sec s = { g_base + *(uint32_t *)(sh + 12), *(uint32_t *)(sh + 8) };
        if      (!strncmp((char *)sh, ".text", 8))  g_text = s;
        else if (!strncmp((char *)sh, ".rdata", 8)) g_rdata = s;
        else if (!strncmp((char *)sh, ".data", 8))  g_data = s;
    }
    return g_text.p && g_rdata.p && g_data.p && g_pdataN;
}

static int in_text(const void *a)  { return (const uint8_t *)a >= g_text.p  && (const uint8_t *)a < g_text.p + g_text.n; }
static int in_rdata(const void *a) { return (const uint8_t *)a >= g_rdata.p && (const uint8_t *)a < g_rdata.p + g_rdata.n; }

/* Start of the function containing a: .pdata lookup, following chained unwind info back to the
   primary entry. Independent of the function's first bytes, which another DLL may have patched. */
static uint8_t *func_start(const uint8_t *a)
{
    uint32_t rva = (uint32_t)(a - g_base);
    size_t lo = 0, hi = g_pdataN;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        const uint32_t *rf = g_pdata + mid * 3;
        if (rva < rf[0]) hi = mid; else if (rva >= rf[1]) lo = mid + 1; else { lo = mid; goto found; }
    }
    return NULL;
found:;
    const uint32_t *rf = g_pdata + lo * 3;
    for (int depth = 0; depth < 32; depth++) {
        if (rf[2] & 1) { rf = (const uint32_t *)(g_base + rf[2] - 1); continue; }   /* indirect entry */
        const uint8_t *ui = g_base + rf[2];
        if (!((ui[0] >> 3) & 0x4)) return g_base + rf[0];                    /* no UNW_FLAG_CHAININFO */
        rf = (const uint32_t *)(ui + 4 + 2 * ((ui[2] + 1) & ~1));            /* parent RUNTIME_FUNCTION */
    }
    return NULL;
}

/* bytes another DLL's entry hook put at f: 'jmp [rip+0]' + abs64 (14), 'jmp [rip+d]' (6), 'jmp rel32' (5) */
static int hook_stub_len(const uint8_t *f)
{
    if (f[0] == 0xFF && f[1] == 0x25) return *(const int32_t *)(f + 2) == 0 ? 14 : 6;
    if (f[0] == 0xE9) return 5;
    return 0;
}

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
static uint8_t *find_lea_to(const void *want, int modrm, int which)
{
    int k = 0;
    for (size_t i = 0; i + 7 <= g_text.n; i++) {
        uint8_t *p = g_text.p + i;
        if ((p[0] & 0xF8) != 0x48 || p[1] != 0x8D || (p[2] & 0xC7) != 0x05) continue;
        if (modrm >= 0 && p[2] != modrm) continue;
        if (p + 7 + *(int32_t *)(p + 3) == want && k++ == which) return p;
    }
    return NULL;
}

/* ------------------------------------------------------------------------------------ */
/* engine bindings                                                                      */
/* ------------------------------------------------------------------------------------ */
typedef char (*CanTakePoseFn)(void *brain, int pose);
static uint8_t *g_canTakePose, *g_applyBreed;
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

/* CanTakePose prologue, position independent (copied into the trampoline as is):
   mov [rsp+8],rbx ; mov [rsp+10],rsi ; push rdi ; sub rsp,20h  (15 bytes) */
static const uint8_t k_prologue[] = { 0x48,0x89,0x5C,0x24,0x08, 0x48,0x89,0x74,0x24,0x10, 0x57, 0x48,0x83,0xEC,0x20 };

static int find_can_take_pose(void)
{
    uint8_t *str = find_cstr("cannot_lie");
    if (!str) { logf_("CanTakePose: 'cannot_lie' string not found"); return 0; }
    /* static initialiser: lea rdx,"cannot_lie" ; lea rcx,g_tag ; call Tag::Tag */
    uint8_t *init = find_lea_to(str, 0x15, 0);
    if (!init || find_lea_to(str, -1, 1)) { logf_("CanTakePose: string xref missing or ambiguous"); return 0; }
    uint8_t *tag = NULL;
    for (uint8_t *q = init + 7; q < init + 0x20; q++)
        if (q[0] == 0x48 && q[1] == 0x8D && q[2] == 0x0D) { tag = q + 7 + *(int32_t *)(q + 3); break; }
    if (!tag) { logf_("CanTakePose: global tag not found"); return 0; }
    /* the reader: lea rdx,g_tag inside the stuff-tag loop of CanTakePose (the dtor uses lea rcx) */
    uint8_t *use = find_lea_to(tag, 0x15, 0);
    if (!use || find_lea_to(tag, 0x15, 1)) { logf_("CanTakePose: tag reader missing or ambiguous"); return 0; }
    uint8_t *f = func_start(use);
    if (!f || use < f || use - f > 0x300) { logf_("CanTakePose: function start not found"); return 0; }
    if (memcmp(f, k_prologue, sizeof k_prologue)) {
        logf_(hook_stub_len(f) ? "CanTakePose (exe+0x%llX) is already hooked by another DLL - not hooking"
                               : "CanTakePose (exe+0x%llX) prologue changed - not hooking", (unsigned long long)(f - g_base));
        return 0;
    }
    /* mov ebx,edx ; mov rsi,rcx  then the first  mov rcx,[rsi+disp8]  = brain->human */
    if (memcmp(f + 15, "\x8B\xDA\x48\x8B\xF1", 5)) { logf_("CanTakePose: body not recognised"); return 0; }
    for (uint8_t *q = f + 20; q < f + 0x40 && !g_brainHumanOff; q++)
        if (q[0] == 0x48 && q[1] == 0x8B && q[2] == 0x4E) g_brainHumanOff = q[3];
    if (!g_brainHumanOff) { logf_("CanTakePose: brain->human offset not found"); return 0; }
    /* the lie-only branch: cmp ebx,1 before the tag loop */
    if (!find_bytes(f, use - f, (const uint8_t *)"\x83\xFB\x01", 3)) { logf_("CanTakePose: lie branch not found"); return 0; }
    g_canTakePose = f;
    logf_("CanTakePose = exe+0x%llX, brain->human @ +0x%X", (unsigned long long)(f - g_base), g_brainHumanOff);
    return 1;
}

static int find_human_breed_off(void)
{
    uint8_t *s = find_cstr("Unknown breed \"%s\"");
    if (!s) { logf_("ApplyBreed: string not found"); return 0; }
    uint8_t *lea = find_lea_to(s, -1, 0);
    if (!lea || find_lea_to(s, -1, 1)) { logf_("ApplyBreed: string xref missing or ambiguous"); return 0; }
    uint8_t *f = func_start(lea);
    if (!f || lea < f || lea - f > 0x400) { logf_("ApplyBreed: function start not found"); return 0; }
    /* cmp qword [rcx+disp32], reg  ->  48/4C 39 modrm(mod=10,rm=001) disp32; skip a hook stub at the entry */
    for (uint8_t *p = f + hook_stub_len(f); p < lea - 7; p++)
        if ((p[0] == 0x48 || p[0] == 0x4C) && p[1] == 0x39 && (p[2] & 0xC7) == 0x81) {
            uint32_t off = *(uint32_t *)(p + 3);
            if (off > 0x800 && off < 0x4000) { g_humanBreedOff = off; break; }
        }
    if (!g_humanBreedOff) { logf_("ApplyBreed: human->breed offset not found"); return 0; }
    g_applyBreed = f;
    logf_("ApplyBreed = exe+0x%llX%s, human->breed @ +0x%X", (unsigned long long)(f - g_base),
          hook_stub_len(f) ? " (hooked by another DLL)" : "", g_humanBreedOff);
    return 1;
}

static int find_breed_tags_off(void)
{
    /* breed parser (eHumanBreed.cpp): ... lea reg,[breed+off] ... read("tags") ... read("nationality") */
    uint8_t *snat = find_cstr("nationality");
    if (!snat) { logf_("breed tags: 'nationality' not found"); return 0; }
    uint8_t *lnat = find_lea_to(snat, -1, 0);
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
    for (uint8_t *p = ctor; p < ctor + 0x80 && !vt; p++)
        if (p[0] == 0x48 && p[1] == 0x8D && p[2] == 0x05) vt = p + 7 + *(int32_t *)(p + 3);
    if (!vt || !in_rdata(vt)) { logf_("entity tags: vtable not found"); return 0; }
    uint8_t *ev = *(uint8_t **)(vt + 8);
    if (!in_text(ev)) { logf_("entity tags: eval not in .text"); return 0; }
    for (uint8_t *q = ev; q < ev + 0x100 && !g_entTagsOff; q++)
        if (q[0] == 0x48 && q[1] == 0x81 && q[2] == 0xC2) g_entTagsOff = *(uint32_t *)(q + 3);
    if (g_entTagsOff < 0x100 || g_entTagsOff > 0x400) { logf_("entity tags: container offset not found"); g_entTagsOff = 0; return 0; }
    logf_("entity {tags} @ entity+0x%X", g_entTagsOff);
    return 1;
}

#ifndef NP_TEST
/* ------------------------------------------------------------------------------------ */
/* hook                                                                                 */
/* ------------------------------------------------------------------------------------ */
static CanTakePoseFn g_canTakePoseOrig;
static char g_tag[64];
static size_t g_tagLen;

/* breed {tags} / entity {tags} container holds g_tag (engine lowercases tag names; the ini may not) */
static int has_tag(const uint8_t *c)
{
    if (*(const uint8_t *const *)(c + TC_SELF_A) != c || *(const uint8_t *const *)(c + TC_SELF_B) != c) return 0;
    const uint8_t *b = *(const uint8_t *const *)(c + TC_VEC_BEG), *e = *(const uint8_t *const *)(c + TC_VEC_END);
    if (!b || e < b || (size_t)(e - b) > TAG_SIZE * 256 || (size_t)(e - b) % TAG_SIZE) return 0;
    for (const uint8_t *t = b; t < e; t += TAG_SIZE) {
        const char *s = (const char *)(t + TAG_STR);
        size_t len = *(const size_t *)(t + TAG_STR + 0x10), cap = *(const size_t *)(t + TAG_STR + 0x18);
        if (len != g_tagLen) continue;
        if (cap > 15) s = *(const char *const *)(t + TAG_STR);
        if (!_strnicmp(s, g_tag, len)) return 1;
    }
    return 0;
}

/* ReadProcessMemory = a read that cannot fault; only used for the log line */
static int rd_mem(const void *src, void *dst, size_t n)
{
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), src, dst, n, &got) && got == n;
}

static int rd_msstr(const uint8_t *at, char *out, size_t n)
{
    struct { union { char buf[16]; char *ptr; } u; size_t size, cap; } s;
    out[0] = 0;
    if (!rd_mem(at, &s, sizeof s)) return 0;
    if (s.size >= n) s.size = n - 1;
    if (s.cap > 15) { if (!rd_mem(s.u.ptr, out, s.size)) return 0; } else memcpy(out, s.u.buf, s.size);
    out[s.size] = 0;
    return 1;
}

/* first block per breed + source goes to the log (breed name = the Tag just before the breed pointer) */
static const void *g_seen[256];
static int g_seenN;
static void log_block(const uint8_t *human, const uint8_t *breed, int fromEntity)
{
    const void *key = (const void *)((uintptr_t)breed + (uintptr_t)fromEntity);
    int fresh = 1;
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < g_seenN && fresh; i++) if (g_seen[i] == key) fresh = 0;
    if (fresh && g_seenN < 256) g_seen[g_seenN++] = key; else fresh = 0;
    LeaveCriticalSection(&g_lock);
    if (!fresh) return;
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

/* Suspend every other thread, retrying until none is stopped inside [lo,hi) (as BreedTags does).
   Normal launcher start (early-bird APC): no other thread runs engine code yet. Launcher watch mode:
   the game is already running and an AI thread could be inside CanTakePose's first bytes. */
static int suspend_others(HANDLE *hs, int max, const uint8_t *lo, const uint8_t *hi)
{
    for (int attempt = 0; attempt < 20; attempt++) {
        int n = 0, bad = 0;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap == INVALID_HANDLE_VALUE) return -1;
        THREADENTRY32 te = { sizeof te };
        for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
            if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == GetCurrentThreadId()) continue;
            if (n >= max) { bad = 2; break; }
            /* a thread that cannot be opened, stopped or inspected is not proven safe: retry
               (one that just exited drops out of the next snapshot) */
            HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID);
            if (!h) { bad = 1; continue; }
            if (SuspendThread(h) == (DWORD)-1) { CloseHandle(h); bad = 1; continue; }
            hs[n++] = h;
            CONTEXT c; c.ContextFlags = CONTEXT_CONTROL;
            if (!GetThreadContext(h, &c) || ((const uint8_t *)c.Rip >= lo && (const uint8_t *)c.Rip < hi)) bad = 1;
        }
        CloseHandle(snap);
        if (!bad) return n;
        for (int i = 0; i < n; i++) { ResumeThread(hs[i]); CloseHandle(hs[i]); }
        if (bad == 2) return -1;
        Sleep(5);
    }
    return -1;
}

static void resume_others(HANDLE *hs, int n) { for (int i = 0; i < n; i++) { ResumeThread(hs[i]); CloseHandle(hs[i]); } }

/* 15-byte prologue copied into a trampoline; 'jmp [rip]' + abs64 + nop written over the original */
/* Nothing between suspend_others and resume_others may log, allocate or take a lock: a suspended
   thread could hold the CRT or heap lock. */
static int install_hook(void)
{
    uint8_t *f = g_canTakePose;
    DWORD old;
    uint8_t *tramp = VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!tramp) { logf_("VirtualAlloc failed"); return 0; }
    memcpy(tramp, f, sizeof k_prologue);
    tramp[15] = 0xFF; tramp[16] = 0x25; *(uint32_t *)(tramp + 17) = 0;
    *(uint64_t *)(tramp + 21) = (uint64_t)(f + sizeof k_prologue);
    if (!VirtualProtect(tramp, 64, PAGE_EXECUTE_READ, &old)) {
        logf_("VirtualProtect (trampoline) failed (%lu)", GetLastError());
        VirtualFree(tramp, 0, MEM_RELEASE);
        return 0;
    }

    uint8_t patch[15] = { 0xFF, 0x25, 0, 0, 0, 0 };
    *(uint64_t *)(patch + 6) = (uint64_t)hook_can_take_pose;
    patch[14] = 0x90;
    if (!VirtualProtect(f, sizeof patch, PAGE_EXECUTE_READWRITE, &old)) {
        logf_("VirtualProtect failed (%lu)", GetLastError());
        VirtualFree(tramp, 0, MEM_RELEASE);
        return 0;
    }
    static HANDLE hs[1024];
    int n = suspend_others(hs, 1024, f, f + sizeof patch);
    if (n < 0) {
        VirtualProtect(f, sizeof patch, old, &old);
        VirtualFree(tramp, 0, MEM_RELEASE);
        logf_("could not stop the other threads safely - not hooking");
        return 0;
    }
    g_canTakePoseOrig = (CanTakePoseFn)tramp;     /* plain store: set before the jump goes live */
    memcpy(f, patch, sizeof patch);
    FlushInstructionCache(GetCurrentProcess(), f, sizeof patch);
    resume_others(hs, n);
    VirtualProtect(f, sizeof patch, old, &old);
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
    if (!ini_int(L"Enabled", 1)) return;
    g_console = ini_int(L"Console", 0);
    g_verbose = ini_int(L"Verbose", 1);
    if (ini_int(L"LogFile", 1)) {
        wchar_t lp[MAX_PATH]; wcscpy(lp, g_dir); wcscat(lp, NP_LOG);
        g_log = _wfopen(lp, L"w");
    }
    if (g_console) { AllocConsole(); SetConsoleTitleA(NP_NAME); freopen("CONOUT$", "w", stdout); }

    g_base = (uint8_t *)GetModuleHandleW(NULL);
    if (!read_pe()) { logf_("not a PE I understand"); return; }
    logf_(NP_NAME " " NP_VERSION " loaded into %ls (timestamp %08X)", en, g_timestamp);

    ini_str(L"Tag", L"cannot_lie", g_tag, sizeof g_tag);
    g_tagLen = strlen(g_tag);
    if (!g_tagLen) { logf_("Tag= is empty - nothing to do"); return; }

    int ok = find_can_take_pose() & find_human_breed_off() & find_breed_tags_off();
    int okEnt = find_entity_tags_off();
    if (!ok || !install_hook()) { logf_("AI prone block                 OFF"); return; }
    logf_("AI prone block                 ON (tag \"%s\" on breed%s)", g_tag, okEnt ? " or entity" : " only");
    logf_("simulation change: every player of a multiplayer match needs this DLL");
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        InitializeCriticalSection(&g_lock);
        init(h);
    }
    return TRUE;
}
#endif /* !NP_TEST */

#ifdef NP_TEST
/* ------------------------------------------------------------------------------------ */
/* np_test <exe> : map the exe at its preferred base (the .data action table holds absolute
   pointers), run every finder, then repeat with a BreedTags-style 14-byte hook written over
   ApplyBreed's entry (what the launcher payload does before NoProne loads) and expect the same
   results; finally hook CanTakePose and expect a clean refusal. */
static void reset(void) { g_canTakePose = g_applyBreed = NULL; g_brainHumanOff = g_humanBreedOff = g_breedTagsOff = g_entTagsOff = 0; }
static int run_all(void) { reset(); return find_can_take_pose() & find_human_breed_off() & find_breed_tags_off() & find_entity_tags_off(); }
/* 'jmp [rip]' + abs64; the address bytes spell 'cmp [rcx+0x1000],rcx', so a finder that scans the stub
   instead of skipping it picks up a wrong human->breed offset */
static void put_hook(uint8_t *f) { static const uint8_t j[14] = { 0xFF, 0x25, 0, 0, 0, 0, 0x48, 0x39, 0x81, 0x00, 0x10, 0x00, 0x00, 0x00 }; memcpy(f, j, sizeof j); }

/* every chained .pdata entry must resolve to an unchained primary entry */
static int check_chains(void)
{
    size_t chained = 0, bad = 0;
    for (size_t i = 0; i < g_pdataN; i++) {
        const uint32_t *rf = g_pdata + i * 3;
        if (rf[2] & 1) continue;
        const uint8_t *ui = g_base + rf[2];
        if (!((ui[0] >> 3) & 0x4)) continue;
        chained++;
        uint8_t *f = func_start(g_base + rf[0]);
        const uint32_t *p = NULL;
        if (f) for (size_t k = 0; k < g_pdataN && !p; k++) if (g_pdata[k * 3] == (uint32_t)(f - g_base)) p = g_pdata + k * 3;
        if (!p || (!(p[2] & 1) && (((g_base + p[2])[0] >> 3) & 0x4))) bad++;
    }
    printf("chained .pdata entries: %lu, unresolved: %lu\n", (unsigned long)chained, (unsigned long)bad);
    return bad == 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) { printf("usage: np_test <call_to_arms*.exe>\n"); return 2; }
    FILE *fp = fopen(argv[1], "rb"); if (!fp) { printf("cannot open %s\n", argv[1]); return 2; }
    fseek(fp, 0, SEEK_END); long n = ftell(fp); fseek(fp, 0, SEEK_SET);
    uint8_t *file = malloc(n);
    if (!file || fread(file, 1, n, fp) != (size_t)n) { printf("read failed\n"); return 2; }
    fclose(fp);
    uint32_t pe = *(uint32_t *)(file + 0x3C);
    uint16_t nsec = *(uint16_t *)(file + pe + 6), opt = *(uint16_t *)(file + pe + 20);
    uint32_t imgsz = *(uint32_t *)(file + pe + 24 + 56), hdrsz = *(uint32_t *)(file + pe + 24 + 60);
    uint64_t base = *(uint64_t *)(file + pe + 24 + 24);
#ifdef _WIN32
    g_base = VirtualAlloc((void *)base, imgsz, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
    g_base = mmap((void *)base, imgsz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
#endif
    if (g_base != (uint8_t *)base) { printf("cannot map at %llX\n", (unsigned long long)base); return 2; }
    memcpy(g_base, file, hdrsz);
    for (int i = 0; i < nsec; i++) {
        uint8_t *sh = file + pe + 24 + opt + 40 * i;
        uint32_t vs = *(uint32_t *)(sh + 8), va = *(uint32_t *)(sh + 12), rs = *(uint32_t *)(sh + 16), raw = *(uint32_t *)(sh + 20);
        memcpy(g_base + va, file + raw, rs < vs ? rs : vs);
    }
    if (!read_pe()) { printf("not a PE32+ with .pdata\n"); return 2; }
    printf("%s timestamp %08X, %lu .pdata entries\n", argv[1], g_timestamp, (unsigned long)g_pdataN);

    if (!check_chains()) { printf("FAILED (chained unwind)\n"); return 1; }
    printf("-- clean exe\n");
    int ok = run_all();
    if (!ok) { printf("FAILED\n"); return 1; }
    uint32_t want[4] = { g_brainHumanOff, g_humanBreedOff, g_breedTagsOff, g_entTagsOff };
    uint8_t *ctp = g_canTakePose, *ab = g_applyBreed;

    printf("-- ApplyBreed hooked by another DLL (BreedTags)\n");
    put_hook(ab);
    ok = run_all();
    uint32_t got[4] = { g_brainHumanOff, g_humanBreedOff, g_breedTagsOff, g_entTagsOff };
    if (!ok || memcmp(want, got, sizeof want) || g_canTakePose != ctp || g_applyBreed != ab) { printf("FAILED (results differ)\n"); return 1; }

    printf("-- CanTakePose hooked by another DLL (must refuse)\n");
    put_hook(ctp);
    reset();
    if (find_can_take_pose()) { printf("FAILED (hooked over another hook)\n"); return 1; }

    printf("ALL FOUND\n");
    return 0;
}
#endif /* NP_TEST */
