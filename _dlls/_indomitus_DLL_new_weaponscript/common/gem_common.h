/*
 * gem_common.h  -  shared code of the GEM hook dlls (GEM Converge, GEM Overcharge, GEM Guidance)
 * for Call to Arms: Gates of Hell (GEM engine, game / editor / dedicated server exes).
 *
 * Each dll is one translation unit: define GEM_NAME, GEM_VERSION, GEM_FILE (L"gem_x": ini / log base name)
 * and GEM_SECT (L"x": ini section) and include this file. Everything here is static.
 *
 * Contents:
 *   logging + ini                 <dll folder>\<GEM_FILE>.log / .ini
 *   PE / byte-pattern helpers     every engine address is found at run time, nothing is hard-coded
 *   hook framework                14-byte inline jumps that CHAIN: several dlls may hook the same function
 *   shared engine finders         eBulletShooter::Launch, SpawnBullet, GetShooterBone, GetAimPoint, stuff tags
 *
 * Hook chaining contract (all three dlls follow it; see README.md):
 *   the first bytes of a hooked function become  FF 25 00000000 <stub>  (+ NOP padding). The stub, in the
 *   hooking dll's own memory, is  FF 25 00000000 <hook>, "GEMHOOK1", u8 n, the n bytes that were there before.
 *   A later dll hooking the same function copies the earlier jump into its own trampoline (it is position
 *   independent), so calls run  newest hook -> ... -> oldest hook -> original code. The finders read a hooked
 *   function's original first bytes back through the stub chain (hk_original), so load order never matters.
 *
 * Build with -DGEM_TEST to get the offline finder test (test\chain_test.c) instead of a dll.
 */
#ifndef GEM_COMMON_H
#define GEM_COMMON_H

#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
#include <math.h>

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#endif

/* ------------------------------------------------------------------------------------ */
/* logging / ini                                                                        */
/* ------------------------------------------------------------------------------------ */
static int g_verbose = 1;
static wchar_t g_dir[MAX_PATH];

#ifdef GEM_TEST
static void logf_(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    printf("  [" GEM_NAME "] "); vprintf(fmt, ap); printf("\n");
    va_end(ap);
}
static void ini_str(const char *key, const char *def, char *out, size_t n) { (void)key; snprintf(out, n, "%s", def); }
static int ini_int(const char *key, int def) { (void)key; return def; }
#else
static FILE *g_log;
static int g_console;
static CRITICAL_SECTION g_logLock;

static void logf_(const char *fmt, ...)
{
    char buf[1024];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    SYSTEMTIME st; GetLocalTime(&st);
    char line[1100];
    snprintf(line, sizeof line, "%02d:%02d:%02d [" GEM_NAME "] %s\n", st.wHour, st.wMinute, st.wSecond, buf);
    EnterCriticalSection(&g_logLock);
    if (g_log) { fputs(line, g_log); fflush(g_log); }
    if (g_console) { fputs(line, stdout); fflush(stdout); }
    LeaveCriticalSection(&g_logLock);
    OutputDebugStringA(line);
}

static void ini_path(wchar_t *ini) { wcscpy(ini, g_dir); wcscat(ini, L"\\" GEM_FILE L".ini"); }

static void ini_str(const char *key, const char *def, char *out, size_t n)
{
    wchar_t ini[MAX_PATH]; ini_path(ini);
    wchar_t wkey[64], wdef[256], wout[256];
    MultiByteToWideChar(CP_ACP, 0, key, -1, wkey, 64); MultiByteToWideChar(CP_ACP, 0, def, -1, wdef, 256);
    GetPrivateProfileStringW(GEM_SECT, wkey, wdef, wout, 256, ini);
    WideCharToMultiByte(CP_ACP, 0, wout, -1, out, (int)n, NULL, NULL);
}

static int ini_int(const char *key, int def)
{
    wchar_t ini[MAX_PATH]; ini_path(ini);
    wchar_t wkey[64]; MultiByteToWideChar(CP_ACP, 0, key, -1, wkey, 64);
    return (int)GetPrivateProfileIntW(GEM_SECT, wkey, def, ini);
}
#endif /* GEM_TEST */

/* ------------------------------------------------------------------------------------ */
/* PE helpers                                                                           */
/* ------------------------------------------------------------------------------------ */
typedef struct { uint8_t *p; size_t n; } Sec;
static Sec g_text, g_rdata, g_data;
static uint8_t *g_base;
static uint32_t g_timestamp;

/* sections of the image at base (the running exe, or the test's mapped copy: headers + sections) */
static int get_sections(uint8_t *base)
{
    g_base = base;
    g_text.p = g_rdata.p = g_data.p = NULL;
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

static int in_text(const void *a) { return (uint8_t *)a >= g_text.p && (uint8_t *)a < g_text.p + g_text.n; }
static int in_rdata(const void *a) { return (uint8_t *)a >= g_rdata.p && (uint8_t *)a < g_rdata.p + g_rdata.n; }
static int in_data(const void *a) { return (uint8_t *)a >= g_data.p && (uint8_t *)a < g_data.p + g_data.n; }

static uint8_t *find_bytes(uint8_t *p, size_t n, const uint8_t *pat, size_t m)
{
    if (m > n) return NULL;
    for (size_t i = 0; i + m <= n; i++)
        if (p[i] == pat[0] && !memcmp(p + i, pat, m)) return p + i;
    return NULL;
}

/* a whole NUL-terminated string in a section (preceded by NUL, so not a suffix of a longer one) */
static uint8_t *find_cstr_in(Sec sec, const char *s)
{
    size_t m = strlen(s) + 1;
    for (uint8_t *p = sec.p; (p = find_bytes(p, sec.p + sec.n - p, (const uint8_t *)s, m)); p++)
        if (p == sec.p || p[-1] == 0) return p;
    return NULL;
}
static uint8_t *find_cstr(const char *s) { return find_cstr_in(g_rdata, s); }

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

/* k-th 'call rel32' target inside [p, p+n) */
static uint8_t *call_target(uint8_t *p, size_t n, int which)
{
    int k = 0;
    for (size_t i = 0; i + 5 <= n; i++) {
        if (p[i] != 0xE8) continue;
        uint8_t *t = p + i + 5 + *(int32_t *)(p + i + 1);
        if (!in_text(t)) continue;
        if (k++ == which) return t;
    }
    return NULL;
}

/* disp32 of the first 'opcode(3 bytes) disp32' in [p, p+n) */
static int find_disp32(uint8_t *p, size_t n, const uint8_t *op3, uint32_t lo, uint32_t hi, uint32_t *out)
{
    for (size_t i = 0; i + 7 <= n; i++)
        if (!memcmp(p + i, op3, 3)) {
            uint32_t d = *(uint32_t *)(p + i + 3);
            if (d >= lo && d <= hi) { *out = d; return 1; }
        }
    return 0;
}

/* vftable of a class via MSVC RTTI (TypeDescriptor in .data <- CompleteObjectLocator (offset 0) <- vftable[-1]) */
static uint8_t *find_vtable(const char *rtti)
{
    uint8_t *name = find_cstr_in(g_data, rtti);
    if (!name) return NULL;
    uint32_t tdRel = (uint32_t)(name - 0x10 - g_base);
    uint8_t *vt = NULL; int hits = 0;
    for (size_t i = 12; i + 4 <= g_rdata.n; i += 4) {
        if (*(uint32_t *)(g_rdata.p + i) != tdRel || *(uint32_t *)(g_rdata.p + i - 12) != 1 || *(uint32_t *)(g_rdata.p + i - 8) != 0) continue;
        uint64_t col = (uint64_t)(g_rdata.p + i - 12);
        for (size_t j = 0; j + 8 <= g_rdata.n; j += 8)
            if (*(uint64_t *)(g_rdata.p + j) == col) { vt = g_rdata.p + j + 8; hits++; }
    }
    return hits == 1 ? vt : NULL;
}

/* function start = 'mov rax,rsp' right after int3 padding, walking back from a reference inside it */
static uint8_t *func_start_before(uint8_t *ref, size_t maxback)
{
    for (uint8_t *p = ref; p > ref - maxback && p > g_text.p + 1; p--)
        if (p[-1] == 0xCC && p[0] == 0x48 && p[1] == 0x8B && p[2] == 0xC4) return p;
    return NULL;
}

/* ------------------------------------------------------------------------------------ */
/* memory / object readers                                                              */
/* ------------------------------------------------------------------------------------ */
typedef struct { union { char buf[16]; char *ptr; } u; size_t size, cap; } MsString;   /* MSVC std::string */

#define TAG_SIZE    0x60    /* Tag vector element */
#define TAG_STR     0x40    /* MSVC std::string inside the element */

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

/* MSVC RTTI class name of a heap object, or NULL when it does not look like one of the exe's classes */
static const char *rtti_name(const void *obj, char *buf, size_t n)
{
    const uint8_t *vt, *col; uint32_t tdRel;
    if (!rd_mem(obj, &vt, 8) || !in_rdata(vt)) return NULL;
    if (!rd_mem(vt - 8, &col, 8) || !in_rdata(col)) return NULL;
    if (!rd_mem(col + 12, &tdRel, 4)) return NULL;
    const uint8_t *name = g_base + tdRel + 0x10;
    if (name < g_data.p || name >= g_data.p + g_data.n) return NULL;
    if (!rd_mem(name, buf, n < 4 ? n : 4) || memcmp(buf, ".?AV", 4)) return NULL;
    size_t i = 0;
    for (; i + 1 < n; i++) { if (!rd_mem(name + i, buf + i, 1) || !buf[i]) break; }
    buf[i] = 0;
    return buf;
}

/* string of one Tag vector element (engine tags are lowercase) */
static const char *tag_str(const uint8_t *t, size_t *len)
{
    const char *s = (const char *)(t + TAG_STR);
    size_t cap = *(const size_t *)(t + TAG_STR + 0x18);
    *len = *(const size_t *)(t + TAG_STR + 0x10);
    if (cap > 15) s = *(const char *const *)(t + TAG_STR);
    return s;
}

static double vnorm(double *v) { double n = sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]); if (n > 1e-9) { v[0] /= n; v[1] /= n; v[2] /= n; } return n; }

/* splitmix64: per-shot "random" values must be identical on every lockstep machine, so they are hashed from the
   shot's own geometry instead of coming from a local RNG */
static uint64_t mix64(uint64_t z) { z += 0x9E3779B97F4A7C15ull; z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull; z = (z ^ (z >> 27)) * 0x94D049BB133111EBull; return z ^ (z >> 31); }
static uint64_t bits_d(double v) { uint64_t u; memcpy(&u, &v, 8); return u; }

/* ------------------------------------------------------------------------------------ */
/* hook framework (chainable 14-byte inline jumps)                                      */
/* ------------------------------------------------------------------------------------ */
#define HK_MAGIC    "GEMHOOK1"
#define HK_SAVE_MAX 32          /* longest prologue a hook may replace */
#define HK_STUB     0           /* allocation layout: stub at +0 (jmp hook, magic, n, saved bytes) */
#define HK_TRAMP    64          /*                    trampoline at +64 (saved bytes, jmp back)    */

static int is_absjmp(const uint8_t *p) { return p[0] == 0xFF && p[1] == 0x25 && *(const uint32_t *)(p + 2) == 0; }

/* the first 'want' (>= HK_SAVE_MAX) bytes of f as they were before any GEM hook, and how many bytes at f the
   hooks cover. 0 = f starts with a jump that is not a GEM hook (another tool's): the original is unknown. */
static int hk_original(const uint8_t *f, uint8_t *out, size_t want, size_t *patched)
{
    if (want < HK_SAVE_MAX) return 0;
    memcpy(out, f, want);
    size_t maxn = 0;
    for (int depth = 0; depth < 16 && is_absjmp(out); depth++) {
        const uint8_t *stub = *(const uint8_t *const *)(out + 6);
        uint8_t hdr[23 + HK_SAVE_MAX];
        if (!rd_mem(stub, hdr, sizeof hdr) || memcmp(hdr + 14, HK_MAGIC, 8)) return 0;
        size_t n = hdr[22];
        if (n < 14 || n > HK_SAVE_MAX) return 0;
        memcpy(out, hdr + 23, n);       /* the bytes this hook replaced: original code, or the previous hook's jump */
        if (n > maxn) maxn = n;
    }
    if (is_absjmp(out)) return 0;
    if (patched) *patched = maxn;
    return 1;
}

/* first byte of f past any GEM hook patch (pattern scans start here, so a hook's address bytes never match) */
static uint8_t *hk_body(uint8_t *f)
{
    uint8_t b[HK_SAVE_MAX]; size_t n = 0;
    return hk_original(f, b, sizeof b, &n) ? f + n : f;
}

/* length of a hookable prologue: whole position-independent instructions until >= 14 bytes. An earlier
   hook's 14-byte jump counts as one such instruction (it is absolute), which is what makes hooks chain. */
static size_t prolog_len(const uint8_t *f)
{
    size_t n = 0;
    while (n < 14) {
        const uint8_t *p = f + n;
        if (n == 0 && is_absjmp(p)) n += 14;                                                    /* an earlier hook */
        else if (p[0] == 0x48 && p[1] == 0x8B && p[2] == 0xC4) n += 3;                           /* mov rax,rsp */
        else if (p[0] == 0x55 || p[0] == 0x53 || p[0] == 0x56 || p[0] == 0x57) n += 1;          /* push rbp/rbx/rsi/rdi */
        else if (p[0] == 0x41 && p[1] >= 0x54 && p[1] <= 0x57) n += 2;                           /* push r12-r15 */
        else if (p[0] == 0x40 && (p[1] == 0x53 || p[1] == 0x55 || p[1] == 0x56 || p[1] == 0x57)) n += 2; /* rex push rbx/rbp/rsi/rdi */
        else if ((p[0] == 0x48 || p[0] == 0x4C) && p[1] == 0x89 && (p[2] & 0xC7) == 0x40) n += 4;/* mov [rax+d8],r64 */
        else if (p[0] == 0x89 && p[1] == 0x50) n += 3;                                           /* mov [rax+d8],edx */
        else if ((p[0] == 0x48 || p[0] == 0x4C) && p[1] == 0x89 && (p[2] & 0xC7) == 0x44 && p[3] == 0x24) n += 5; /* mov [rsp+d8],r64 */
        else if (p[0] == 0x89 && p[1] == 0x54 && p[2] == 0x24) n += 4;                           /* mov [rsp+d8],edx */
        else if (p[0] == 0x48 && p[1] == 0x8D && p[2] == 0xA8) n += 7;                           /* lea rbp,[rax+d32] */
        else if (p[0] == 0x48 && p[1] == 0x8D && p[2] == 0x6C && p[3] == 0x24) n += 5;           /* lea rbp,[rsp+d8] */
        else if (p[0] == 0x48 && p[1] == 0x81 && p[2] == 0xEC) n += 7;                           /* sub rsp,imm32 */
        else if (p[0] == 0x48 && p[1] == 0x83 && p[2] == 0xEC) n += 4;                           /* sub rsp,imm8 */
        else if (p[0] == 0x48 && p[1] == 0x8B && (p[2] & 0xC0) == 0xC0) n += 3;                  /* mov r64,r64 */
        else if (p[0] == 0x8B && (p[1] & 0xC0) == 0x80 && (p[1] & 7) != 4) n += 6;               /* mov r32,[r64+d32] */
        else return 0;
    }
    return n;
}

#ifdef GEM_TEST
static void *g_hooked[16];       /* targets this dll patched (the chain test walks them) */
static int g_nhooked;
#endif

/* Patch target to jump to hook; *orig = trampoline that runs what was there before (the original code or the
   previous hook). Called from DllMain: the dlls must be loaded before the engine runs game code (README). */
static int install_hook_at(const char *what, void *target, void *hook, void **orig)
{
    uint8_t *f = (uint8_t *)target;
    size_t n = prolog_len(f);
    if (!n || n > HK_SAVE_MAX) { logf_("%s prologue not hookable - not hooking", what); return 0; }
    uint8_t chk[HK_SAVE_MAX];
    int chained = is_absjmp(f);
    if (chained && !hk_original(f, chk, sizeof chk, NULL)) logf_("%s is hooked by another tool - chaining onto it", what);

    uint8_t *mem = VirtualAlloc(NULL, 128, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!mem) { logf_("%s: VirtualAlloc failed", what); return 0; }
    uint8_t *stub = mem + HK_STUB, *tramp = mem + HK_TRAMP;
    stub[0] = 0xFF; stub[1] = 0x25; *(uint32_t *)(stub + 2) = 0; *(uint64_t *)(stub + 6) = (uint64_t)hook;
    memcpy(stub + 14, HK_MAGIC, 8);
    stub[22] = (uint8_t)n;
    memcpy(stub + 23, f, n);
    memcpy(tramp, f, n);
    tramp[n] = 0xFF; tramp[n + 1] = 0x25; *(uint32_t *)(tramp + n + 2) = 0;
    *(uint64_t *)(tramp + n + 6) = (uint64_t)(f + n);
    DWORD old;
    VirtualProtect(mem, 128, PAGE_EXECUTE_READ, &old);
    *orig = tramp;

    uint8_t patch[HK_SAVE_MAX]; memset(patch, 0x90, sizeof patch);
    patch[0] = 0xFF; patch[1] = 0x25; *(uint32_t *)(patch + 2) = 0;
    *(uint64_t *)(patch + 6) = (uint64_t)stub;
    if (!VirtualProtect(f, n, PAGE_EXECUTE_READWRITE, &old)) { logf_("%s: VirtualProtect failed (%lu)", what, GetLastError()); return 0; }
    memcpy(f, patch, n);
    VirtualProtect(f, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), f, n);
    if (chained) logf_("%s: chained onto an earlier hook", what);
#ifdef GEM_TEST
    if (g_nhooked < 16) g_hooked[g_nhooked++] = f;
#endif
    return 1;
}

/* ------------------------------------------------------------------------------------ */
/* start-up guards                                                                      */
/* ------------------------------------------------------------------------------------ */
#ifndef GEM_TEST
/* The old all-in-one GEM Converge (<= 0.5.1, a VCOMP140.dll proxy in binaries\x64) carries every feature of the
   split dlls; running both would apply each effect twice. Its log prefix "[GEM Converge]" is in its image. */
static int old_combined_loaded(void)
{
    HMODULE m = GetModuleHandleW(L"vcomp140.dll");
    if (!m) return 0;
    wchar_t p[MAX_PATH], sys[MAX_PATH];
    if (!GetModuleFileNameW(m, p, MAX_PATH)) return 0;
    UINT ns = GetSystemDirectoryW(sys, MAX_PATH);
    if (ns && !_wcsnicmp(p, sys, ns)) return 0;                          /* the real OpenMP runtime */
    uint8_t *b = (uint8_t *)m;
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(b + ((IMAGE_DOS_HEADER *)b)->e_lfanew);
    IMAGE_SECTION_HEADER *sh = IMAGE_FIRST_SECTION(nt);
    static const char k[] = "[GEM Converge]";
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++)
        if ((sh[i].Characteristics & IMAGE_SCN_MEM_READ) && find_bytes(b + sh[i].VirtualAddress, sh[i].Misc.VirtualSize, (const uint8_t *)k, sizeof k - 1))
            return 1;
    return 0;
}

/* one copy per process, even if the launcher injects the dll twice from different paths */
static int first_instance(void)
{
    wchar_t name[96];
    swprintf(name, 96, L"Local\\" GEM_FILE L"_%lu", GetCurrentProcessId());
    HANDLE h = CreateMutexW(NULL, FALSE, name);
    return h && GetLastError() != ERROR_ALREADY_EXISTS;
}

/* common DllMain prologue: folder, exe filter, Enabled, log. 0 = do nothing in this process. */
static int gem_open(HMODULE self, const wchar_t **exeName)
{
    GetModuleFileNameW(self, g_dir, MAX_PATH);
    wchar_t *bs = wcsrchr(g_dir, L'\\'); if (bs) *bs = 0;
    static wchar_t exe[MAX_PATH]; GetModuleFileNameW(NULL, exe, MAX_PATH);
    wchar_t *en = wcsrchr(exe, L'\\'); en = en ? en + 1 : exe;
    *exeName = en;
    if (_wcsnicmp(en, L"call_to_arms", 12)) return 0;          /* only the game / editor / server exes */
    if (!ini_int("Enabled", 1)) return 0;
    if (!first_instance()) return 0;
    g_console = ini_int("Console", 0);
    g_verbose = ini_int("Verbose", 1);
    if (ini_int("LogFile", 1)) {
        wchar_t lp[MAX_PATH]; wcscpy(lp, g_dir); wcscat(lp, L"\\" GEM_FILE L".log");
        g_log = _wfopen(lp, L"w");
    }
    if (g_console) { AllocConsole(); SetConsoleTitleA(GEM_NAME); freopen("CONOUT$", "w", stdout); }
    if (!get_sections((uint8_t *)GetModuleHandleW(NULL))) { logf_("not a PE I understand"); return 0; }
    logf_(GEM_NAME " " GEM_VERSION " loaded into %ls (timestamp %08X)", en, g_timestamp);
    if (old_combined_loaded()) {
        logf_("the old all-in-one GEM Converge VCOMP140.dll is loaded in this process - doing nothing (remove it from binaries\\x64)");
        return 0;
    }
    return 1;
}
#endif /* GEM_TEST */

/* ------------------------------------------------------------------------------------ */
/* shared engine bindings (eWeaponShooterBullet.cpp)                                    */
/* ------------------------------------------------------------------------------------ */
typedef char    (*SpawnBulletFn)(void *shooter, int syncIdx, float *zero, float *dir);
typedef char    (*GetShooterBoneFn)(void *shooter, void **bone, MsString *name);
typedef void    (*BoneRefreshFn)(void *bone);
typedef double *(*GetAimPointFn)(void *aimer, double *out);
typedef double *(*AimerGetVecFn)(void *aimer);
typedef char    (*IsNetGameFn)(void);
typedef int     (*NextSyncIdFn)(void);

static uint8_t *g_launch;                   /* eBulletShooter::Launch (vtable slot 11) */
static SpawnBulletFn    g_spawnBullet;
static GetShooterBoneFn g_getShooterBone;
static BoneRefreshFn    g_boneRefresh;
static GetAimPointFn    g_getAimPoint;
static IsNetGameFn      g_isNetGame;
static NextSyncIdFn     g_nextSyncId;
static uint8_t *g_hitVec;                   /* aim raycast result vector {begin, end} */
static uint32_t g_weaponOff, g_stuffOff, g_syncedOff, g_aimerOff, g_bonePosOff, g_boneXOff;
static uint32_t g_stuffTagsBeg, g_stuffTagsEnd, g_aimerDirSlot, g_aimerPosSlot;
static uint32_t g_ownerOff, g_ownerItemOff, g_itemFlagOff, g_aimTargetOff;

#define BONE_FLAGS  0x10    /* bone: bit 0 = world matrix stale, refresh first (the engine's own test) */
#define BONE_STALE  0x01
#define ENT_POS     0x74    /* entity world position, float3 */

/* SpawnBullet prologue, two compiler flavours:
   A (game, server): mov [rsp+8],rbx ; mov [rsp+20],r9 ; mov [rsp+18],r8 ; mov [rsp+10],edx   (19 bytes)
   B (editor):       mov rax,rsp ; mov [rax+8],rbx ; mov [rax+20],r9 ; mov [rax+18],r8 ; mov [rax+10],edx  (18 bytes) */
static const uint8_t k_sbPrologA[] = { 0x48,0x89,0x5C,0x24,0x08, 0x4C,0x89,0x4C,0x24,0x20, 0x4C,0x89,0x44,0x24,0x18, 0x89,0x54,0x24,0x10 };
static const uint8_t k_sbPrologB[] = { 0x48,0x8B,0xC4, 0x48,0x89,0x58,0x08, 0x4C,0x89,0x48,0x20, 0x4C,0x89,0x40,0x18, 0x89,0x50,0x10 };
static int is_spawn_bullet(const uint8_t *f)        /* f may already be hooked by a sibling dll */
{
    uint8_t h[HK_SAVE_MAX];
    if (!hk_original(f, h, sizeof h, NULL)) return 0;
    return !memcmp(h, k_sbPrologA, sizeof k_sbPrologA) || !memcmp(h, k_sbPrologB, sizeof k_sbPrologB);
}
/* Launch: mov rax,rsp ; mov [rax+8],rbx */
static const uint8_t k_launchProlog[] = { 0x48,0x8B,0xC4, 0x48,0x89,0x58,0x08 };
/* GetShooterBone body: mov rax,[rcx+8] (weapon) ; mov r9,[rax+disp32] (aimer) ; mov rax,[r9+20] (aim bone) */
static const uint8_t k_gsbBody1[] = { 0x48,0x8B,0x41 };
static const uint8_t k_gsbBody2[] = { 0x4C,0x8B,0x88 };
static const uint8_t k_gsbBody3[] = { 0x49,0x8B,0x41,0x20 };
/* GetAimPoint body: mov rax,[rcx+10] (aim bone) ... mov r8,[rax+90] (weapon->owner), both in the first 0x40 bytes */
static const uint8_t k_apBody1[] = { 0x48,0x8B,0x41,0x10 };
static const uint8_t k_apBody2[] = { 0x4C,0x8B,0x80,0x90,0x00,0x00,0x00 };

/* eBulletShooter vftable via MSVC RTTI: TypeDescriptor <- CompleteObjectLocator <- vftable[-1] */
static int find_launch(void)
{
    uint8_t *name = find_cstr_in(g_data, ".?AVeBulletShooter@Weapon@Actor@@");    /* TypeDescriptors live in .data */
    if (!name) { logf_("Launch: RTTI name not found"); return 0; }
    uint32_t tdRel = (uint32_t)(name - 0x10 - g_base);
    uint8_t *vt = NULL; int hits = 0;
    for (size_t i = 12; i + 4 <= g_rdata.n; i += 4) {
        if (*(uint32_t *)(g_rdata.p + i) != tdRel || *(uint32_t *)(g_rdata.p + i - 12) != 1) continue;
        uint64_t col = (uint64_t)(g_rdata.p + i - 12);
        for (size_t j = 0; j + 8 <= g_rdata.n; j += 8)
            if (*(uint64_t *)(g_rdata.p + j) == col) { vt = g_rdata.p + j + 8; hits++; }
    }
    if (hits != 1 || !in_rdata(vt)) { logf_("Launch: vftable %s", hits ? "ambiguous" : "not found"); return 0; }
    uint8_t *f = *(uint8_t **)(vt + 11 * 8), head[HK_SAVE_MAX];
    if (!in_text(f) || !hk_original(f, head, sizeof head, NULL) || memcmp(head, k_launchProlog, sizeof k_launchProlog)) { logf_("Launch: slot 11 not recognised"); return 0; }
    g_launch = f;
    g_spawnBullet = NULL; g_nextSyncId = NULL;
    uint8_t *body = hk_body(f);
    size_t n = 0x300 - (size_t)(body - f);
    /* weapon->stuff:  mov rdx,[rax+disp32] (48 8B 90) ;  SyncedProjectiles byte: cmp [rdx+disp32],r14b (44 38 B2) */
    if (!find_disp32(body, n, (const uint8_t *)"\x48\x8B\x90", 0x40, 0x200, &g_stuffOff)) { logf_("Launch: weapon->stuff offset not found"); return 0; }
    find_disp32(body, n, (const uint8_t *)"\x44\x38\xB2", 0x1000, 0x2000, &g_syncedOff);
    g_isNetGame = (IsNetGameFn)call_target(body, n, 0);
    for (int k = 0; k < 16; k++) {
        uint8_t *t = call_target(body, n, k);
        if (!t) break;
        if (is_spawn_bullet(t)) g_spawnBullet = (SpawnBulletFn)t;
        /* projectile sync id counter (synced weapons, host side):  mov eax,[rip+g] ; inc eax ; mov [rip+g],eax ; ret */
        if (t[0] == 0x8B && t[1] == 0x05 && t[6] == 0xFF && t[7] == 0xC0 && t[8] == 0x89 && t[9] == 0x05 && t[14] == 0xC3) g_nextSyncId = (NextSyncIdFn)t;
    }
    if (!g_spawnBullet) { logf_("Launch: SpawnBullet call not found"); return 0; }
    logf_("Launch = exe+0x%llX, SpawnBullet = exe+0x%llX, weapon->stuff @ +0x%X, SyncedProjectiles @ stuff+0x%X, sync id counter %s",
          (unsigned long long)(f - g_base), (unsigned long long)((uint8_t *)g_spawnBullet - g_base), g_stuffOff, g_syncedOff, g_nextSyncId ? "found" : "NOT found");
    return 1;
}

static int find_spawn_bullet_parts(void)
{
    uint8_t *f = (uint8_t *)g_spawnBullet, *body = hk_body(f);
    uint8_t *gsb = call_target(body, 0x100 - (size_t)(body - f), 0);
    if (!gsb) { logf_("SpawnBullet: GetShooterBone call not found"); return 0; }
    /* GetShooterBone: mov rax,[rcx+8] ; mov r9,[rax+aimer] ; mov rax,[r9+20]  (register allocation differs per build, so match the triple) */
    uint8_t *q = NULL, *w = NULL;
    for (uint8_t *p = gsb; p < gsb + 0x60 && !q; p++) {
        if (memcmp(p, k_gsbBody1, 3)) continue;
        for (uint8_t *r = p + 4; r < p + 0x18; r++)       /* register shuffles may sit between the two loads */
            if (!memcmp(r, k_gsbBody2, 3) && !memcmp(r + 7, k_gsbBody3, 4)) { w = p; q = r; break; }
    }
    if (!q) { logf_("GetShooterBone: body not recognised"); return 0; }
    g_getShooterBone = (GetShooterBoneFn)gsb;
    g_weaponOff = w[3];
    g_aimerOff = *(uint32_t *)(q + 3);
    if (g_aimerOff < 0x40 || g_aimerOff > 0x200) { logf_("GetShooterBone: aimer offset out of range"); return 0; }
    /* after that call:  test byte [bone+10],1 ; je ; mov rcx,bone ; call BoneRefresh ; lea rax,[bone+pos]  (bone register varies) */
    uint8_t *p = NULL;
    for (uint8_t *s = body; s < f + 0x200; s++) {
        int rex = s[0] == 0x41;
        const uint8_t *t = s + rex;
        if (t[0] == 0xF6 && (t[1] & 0xC0) == 0x40 && (t[1] & 0x38) == 0 && t[2] == BONE_FLAGS && t[3] == BONE_STALE) { p = s; break; }
    }
    if (!p) { logf_("SpawnBullet: bone stale test not found"); return 0; }
    uint8_t *call = find_bytes(p, 0x14, (const uint8_t *)"\xE8", 1);
    if (!call) { logf_("SpawnBullet: bone refresh call not found"); return 0; }
    g_boneRefresh = (BoneRefreshFn)(call + 5 + *(int32_t *)(call + 1));
    if (!in_text((void *)g_boneRefresh)) { logf_("SpawnBullet: bone refresh target bad"); return 0; }
    g_bonePosOff = 0;
    for (uint8_t *s = call + 5; s < call + 0x20; s++)
        if ((s[0] & 0xF0) == 0x40 && s[1] == 0x8D && (s[2] & 0xF8) == 0x80 && (s[2] & 7) != 4) {   /* lea rax,[reg+disp32] */
            uint32_t d = *(uint32_t *)(s + 3);
            if (d >= 0x40 && d <= 0x200) { g_bonePosOff = d; break; }
        }
    if (!g_bonePosOff) { logf_("SpawnBullet: bone position offset not found"); return 0; }
    g_boneXOff = g_bonePosOff - 36;    /* float 3x4: X, Y, Z axes then position */
    {   /* the engine reads the X axis too: lea reg,[bone+X] must exist in SpawnBullet */
        int seen = 0;
        for (uint8_t *s = call; s < f + 0x200; s++)
            if ((s[0] & 0xF0) == 0x40 && s[1] == 0x8D && (s[2] & 0xC0) == 0x80 && (s[2] & 7) != 4 && *(uint32_t *)(s + 3) == g_boneXOff) seen = 1;
        if (!seen) { logf_("SpawnBullet: bone X axis offset not confirmed"); return 0; }
    }
    logf_("GetShooterBone = exe+0x%llX, BoneRefresh = exe+0x%llX, shooter->weapon +0x%X, weapon->aimer +0x%X, bone pos +0x%X, bone X +0x%X",
          (unsigned long long)(gsb - g_base), (unsigned long long)((uint8_t *)g_boneRefresh - g_base), g_weaponOff, g_aimerOff, g_bonePosOff, g_boneXOff);
    return 1;
}

/* GetAimPoint: reached from the FireNow profile block -> post-fire bookkeeping -> the only callee
   with the GetAimPoint prologue. Its virtual calls, in order: range, direction, position, speed. */
static int find_aim_point(void)
{
    uint8_t *s = find_cstr("eBulletShooter::FireNow");
    if (!s) { logf_("AimPoint: 'eBulletShooter::FireNow' not found"); return 0; }
    uint8_t *lea = find_lea_to(g_text.p, g_text.n, s, -1, 0);
    if (!lea) { logf_("AimPoint: FireNow xref not found"); return 0; }
    int hits = 0;
    g_getAimPoint = NULL;
    for (int k = 0; k < 24; k++) {
        uint8_t *c = call_target(lea, 0x140, k);
        if (!c) break;
        for (int m = 0; m < 48; m++) {
            uint8_t *t = call_target(c, 0x700, m);
            if (!t) break;
            if (find_bytes(t, 0x40, k_apBody1, sizeof k_apBody1) && find_bytes(t, 0x40, k_apBody2, sizeof k_apBody2)) {
                if (g_getAimPoint != (GetAimPointFn)t) hits++;
                g_getAimPoint = (GetAimPointFn)t;
            }
        }
    }
    if (hits != 1) { logf_("AimPoint: %s", hits ? "ambiguous" : "not found"); return 0; }
    uint8_t *f = (uint8_t *)g_getAimPoint;
    int n = 0; uint32_t slots[4] = {0};
    for (size_t i = 0; i + 3 <= 0x200 && n < 4; i++)
        if (f[i] == 0xFF && f[i + 1] == 0x50) slots[n++] = f[i + 2];
    if (n < 3) { logf_("AimPoint: aimer vtable calls not found"); return 0; }
    g_aimerDirSlot = slots[1]; g_aimerPosSlot = slots[2];
    logf_("GetAimPoint = exe+0x%llX, aimer GetDirection slot +0x%X, GetPosition slot +0x%X",
          (unsigned long long)(f - g_base), g_aimerDirSlot, g_aimerPosSlot);
    return 1;
}

/* stuff tag vector: CanTakePose (eHumanBrain.cpp) scans the held item's stuff tags for the global
   "cannot_lie" Tag:  mov rdi,[rax+end] ; mov rbx,[rax+begin] ; ... lea rdx,g_tag ; call Tag::eq */
static int find_stuff_tags(void)
{
    uint8_t *str = find_cstr("cannot_lie");
    if (!str) { logf_("stuff tags: 'cannot_lie' not found"); return 0; }
    uint8_t *init = find_lea_to(g_text.p, g_text.n, str, 0x15, 0);
    if (!init) { logf_("stuff tags: static init not found"); return 0; }
    uint8_t *tag = NULL;
    for (uint8_t *q = init + 7; q < init + 0x20; q++)
        if (q[0] == 0x48 && q[1] == 0x8D && q[2] == 0x0D) { tag = q + 7 + *(int32_t *)(q + 3); break; }
    if (!tag) { logf_("stuff tags: global tag not found"); return 0; }
    uint8_t *use = find_lea_to(g_text.p, g_text.n, tag, 0x15, 0);
    if (!use || find_lea_to(g_text.p, g_text.n, tag, 0x15, 1)) { logf_("stuff tags: tag reader missing or ambiguous"); return 0; }
    uint8_t *from = use - 0x40;
    if (!find_disp32(from, 0x40, (const uint8_t *)"\x48\x8B\x98", 0x800, 0x3000, &g_stuffTagsBeg) ||
        !find_disp32(from, 0x40, (const uint8_t *)"\x48\x8B\xB8", 0x800, 0x3000, &g_stuffTagsEnd) ||
        g_stuffTagsEnd != g_stuffTagsBeg + 8) { logf_("stuff tags: vector offsets not found"); return 0; }
    logf_("stuff tags vector @ stuff+0x%X", g_stuffTagsBeg);
    return 1;
}

/* GetAimPoint: weapon->owner (mov r8,[rax+disp32]) and owner->human part (mov rax,[r8+disp32]) */
static int find_owner_off(void)
{
    uint8_t *f = (uint8_t *)g_getAimPoint;
    if (!find_disp32(f, 0x40, (const uint8_t *)"\x4C\x8B\x80", 0x40, 0x200, &g_ownerOff)) { logf_("AimPoint: owner offset not found"); return 0; }
    uint32_t partOff = 0;
    for (uint8_t *p = f; p < f + 0x60; p++)
        if (p[0] == 0x49 && p[1] == 0x8B && p[2] == 0x80) { partOff = *(uint32_t *)(p + 3); break; }
    if (!partOff || (g_ownerItemOff && partOff != g_ownerItemOff)) { logf_("AimPoint: human part offset mismatch (0x%X vs 0x%X)", partOff, g_ownerItemOff); return 0; }
    g_ownerItemOff = partOff;
    return 1;
}

/* GetAimPoint extras: the direct-control flag the raycast branch tests (mov rax,[r8+disp32] ; mov r8d,[rax+disp8]),
   the cached target point (movups xmm0,[aimer+disp8]) and the raycast result vector globals
   (mov rax,[rip+g] ; cmp rax,[rip+g+8]). */
static int find_aim_extras(void)
{
    if (!find_owner_off()) return 0;
    uint8_t *f = (uint8_t *)g_getAimPoint;
    for (uint8_t *p = f; p < f + 0x60; p++)
        if (p[0] == 0x44 && p[1] == 0x8B && p[2] == 0x40) { g_itemFlagOff = p[3]; break; }
    if (!g_itemFlagOff) { logf_("AimPoint: direct-control flag offset not found"); return 0; }
    for (uint8_t *p = f; p < f + 0x200; p++)
        if (p[0] == 0x0F && p[1] == 0x10 && (p[2] & 0xF8) == 0x40 && (p[2] & 7) != 4 && p[3] >= 0x40 && p[3] <= 0xA0) { g_aimTargetOff = p[3]; break; }
    if (!g_aimTargetOff) { logf_("AimPoint: cached target offset not found"); return 0; }
    for (uint8_t *p = f; p < f + 0x200; p++)
        if (p[0] == 0x48 && p[1] == 0x8B && p[2] == 0x05 && p[7] == 0x48 && p[8] == 0x3B && p[9] == 0x05) {
            uint8_t *a = p + 7 + *(int32_t *)(p + 3), *b = p + 14 + *(int32_t *)(p + 10);
            if (b == a + 8) { g_hitVec = a; break; }
        }
    if (!g_hitVec) { logf_("AimPoint: raycast result vector not found"); return 0; }
    logf_("weapon->owner +0x%X, owner->human part +0x%X, control flag +0x%X, aimer target +0x%X, hit vector @ exe+0x%llX",
          g_ownerOff, g_ownerItemOff, g_itemFlagOff, g_aimTargetOff, (unsigned long long)(g_hitVec - g_base));
    return 1;
}

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

#endif /* GEM_COMMON_H */
