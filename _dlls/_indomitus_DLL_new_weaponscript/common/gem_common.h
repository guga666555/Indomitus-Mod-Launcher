/*
 * gem_common.h  -  shared code of the GEM WeaponScript modules (GEM Converge, GEM Overcharge, GEM Guidance)
 * for Call to Arms: Gates of Hell (GEM engine, game / editor / dedicated server exes).
 * Indomitus Launcher payload build (1.1): one dll, gem_weaponscript.dll, injected by the launcher before the
 * engine runs; gem_weaponscript.c holds DllMain, the log file and the ini and calls each module's <x>_init().
 *
 * Each module is one translation unit: define GEM_NAME (log tag), GEM_VERSION and GEM_SECT (L"x": its ini
 * section) and include this file. Everything here is static, so every module has its own copy of the finders
 * and its own engine bindings - exactly as when they were three dlls - and the hooks still chain.
 *
 * Contents:
 *   logging + ini                 wrappers around the dll-wide log / ini in gem_weaponscript.c
 *   PE / byte-pattern helpers     every engine address is found at run time, nothing is hard-coded
 *   exe-file readers              original bytes / vftable slots read from call_to_arms*.exe ON DISK, so a
 *                                 sibling payload dll that patched a function entry or swapped a vftable slot
 *                                 (BreedTags, ScriptExt, NoProne, ...) never hides the engine code from us
 *   .pdata function starts        function starts come from the exe's unwind table, never from prologue bytes
 *   hook framework                14-byte inline jumps that CHAIN: several dlls may hook the same function;
 *                                 patches are written with every other thread suspended and off the patched bytes
 *   shared engine finders         eBulletShooter::Launch, SpawnBullet, GetShooterBone, GetAimPoint, stuff tags
 *
 * Hook chaining contract (all three modules follow it, and any other payload dll may; see README.md):
 *   the first bytes of a hooked function become  FF 25 00000000 <stub>  (+ NOP padding). The stub, in the
 *   hooking dll's own memory, is  FF 25 00000000 <hook>, "GEMHOOK1", u8 n, the n bytes that were there before.
 *   A later dll hooking the same function copies the earlier jump into its own trampoline (it is position
 *   independent), so calls run  newest hook -> ... -> oldest hook -> original code. A 14-byte jump written by
 *   another payload dll (no GEMHOOK1 stub) is chained onto the same way.
 *
 * Build with -DGEM_TEST to get the offline finder test (test\chain_test.c) instead of a dll. The test build
 * uses no Win32 API at all, so it also compiles and runs on Linux (test\run_test.sh).
 */
#ifndef GEM_COMMON_H
#define GEM_COMMON_H

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
#include <math.h>

#ifndef GEM_TEST
#include <windows.h>
#include <tlhelp32.h>
#else
#include <stddef.h>
#if !defined(_WIN32)
#include <strings.h>
#define _strnicmp strncasecmp
#define _stricmp  strcasecmp
#endif
#endif

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#endif

/* ------------------------------------------------------------------------------------ */
/* logging / ini                                                                        */
/* ------------------------------------------------------------------------------------ */
static int g_verbose = 1;

#ifdef GEM_TEST
static const char *g_testExe;          /* path of the exe under test (chain_test sets it): exe-file reads use it */

static void logf_(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    printf("  [" GEM_NAME "] "); vprintf(fmt, ap); printf("\n");
    va_end(ap);
}
static void ini_str(const char *key, const char *def, char *out, size_t n) { (void)key; snprintf(out, n, "%s", def); }
static int ini_int(const char *key, int def) { (void)key; return def; }
#else
/* The dll's one log file and ini live in gem_weaponscript.c (ws_*); every module logs under its own name
   ("[GEM Converge]", ...) and reads its own ini section ([converge], ...). */
void ws_vlog(const char *name, const char *fmt, va_list ap);
int  ws_ini_int(const wchar_t *sect, const char *key, int def);
void ws_ini_str(const wchar_t *sect, const char *key, const char *def, char *out, size_t n);

static void logf_(const char *fmt, ...) { va_list ap; va_start(ap, fmt); ws_vlog(GEM_NAME, fmt, ap); va_end(ap); }
static void ini_str(const char *key, const char *def, char *out, size_t n) { ws_ini_str(GEM_SECT, key, def, out, n); }
static int ini_int(const char *key, int def) { return ws_ini_int(GEM_SECT, key, def); }
#endif /* GEM_TEST */

/* ------------------------------------------------------------------------------------ */
/* PE helpers (own structs: the same parser serves the dll and the Linux test build)    */
/* ------------------------------------------------------------------------------------ */
typedef struct { uint8_t *p; size_t n; } Sec;
typedef struct { char name[8]; uint32_t vsize, va, rawSize, rawPtr, relocs, lines; uint16_t nrelocs, nlines; uint32_t flags; } PeSec;   /* IMAGE_SECTION_HEADER */
typedef struct { uint32_t begin, end, unwind; } PeRtf;                                                                               /* RUNTIME_FUNCTION   */
#define PE_SCN_MEM_READ 0x40000000u

static Sec g_text, g_rdata, g_data;
static uint8_t *g_base;
static uint32_t g_timestamp, g_imageSize;
static uint64_t g_prefBase;             /* ImageBase of the headers = the address the exe FILE's absolute pointers use */
static PeSec g_secs[32];
static int g_nsecs;
static PeRtf *g_pdata;                  /* exception directory (unwind table), sorted by begin */
static size_t g_npdata;

/* sections of the image at base (the running exe, or the test's mapped copy: headers + sections) */
static int get_sections(uint8_t *base)
{
    g_base = base;
    g_text.p = g_rdata.p = g_data.p = NULL;
    g_pdata = NULL; g_npdata = 0; g_nsecs = 0;
    uint32_t pe = *(uint32_t *)(base + 0x3C);
    uint8_t *nt = base + pe;
    if (memcmp(nt, "PE\0\0", 4)) return 0;
    uint16_t nsec = *(uint16_t *)(nt + 6), optSize = *(uint16_t *)(nt + 20);
    g_timestamp = *(uint32_t *)(nt + 8);
    uint8_t *opt = nt + 24;
    if (*(uint16_t *)opt != 0x20B) return 0;                     /* PE32+ only */
    g_prefBase = *(uint64_t *)(opt + 24);
    g_imageSize = *(uint32_t *)(opt + 56);
    uint32_t ndirs = *(uint32_t *)(opt + 108);
    if (ndirs > 3) {
        uint32_t exRva = *(uint32_t *)(opt + 112 + 3 * 8), exSize = *(uint32_t *)(opt + 112 + 3 * 8 + 4);
        if (exRva && exSize >= sizeof(PeRtf) && exRva + exSize <= g_imageSize) { g_pdata = (PeRtf *)(base + exRva); g_npdata = exSize / sizeof(PeRtf); }
    }
    PeSec *sh = (PeSec *)(opt + optSize);
    for (int i = 0; i < nsec; i++) {
        if (g_nsecs < 32) g_secs[g_nsecs++] = sh[i];
        Sec s = { base + sh[i].va, sh[i].vsize };
        if (!strncmp(sh[i].name, ".text", 8)) g_text = s;
        else if (!strncmp(sh[i].name, ".rdata", 8)) g_rdata = s;
        else if (!strncmp(sh[i].name, ".data", 8)) g_data = s;
    }
    return g_text.p && g_rdata.p && g_data.p;
}

static int in_text(const void *a) { return (uint8_t *)a >= g_text.p && (uint8_t *)a < g_text.p + g_text.n; }
static int in_rdata(const void *a) { return (uint8_t *)a >= g_rdata.p && (uint8_t *)a < g_rdata.p + g_rdata.n; }
static int in_data(const void *a) { return (uint8_t *)a >= g_data.p && (uint8_t *)a < g_data.p + g_data.n; }
static int in_image(const void *a) { return (uint8_t *)a >= g_base && (uint8_t *)a < g_base + g_imageSize; }

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

/* ------------------------------------------------------------------------------------ */
/* the exe FILE on disk: the engine code as it was before any dll touched the process    */
/* ------------------------------------------------------------------------------------ */
/* Other payload dlls patch function entries (BreedTags / NoProne: 14-byte jmp, ScriptExt: 5-byte jmp) and swap
   vftable slots (ScriptExt) before or after us, in alphabetical payload order. Identifying engine code by what
   is in memory is therefore unreliable (it switched NoProne 0.4 off in every launcher run); the file is not. */
static FILE *g_exeFile;
static int g_exeFileTried;

static int exe_file_open(void)
{
    if (g_exeFile) return 1;
    if (g_exeFileTried) return 0;
    g_exeFileTried = 1;
#ifdef GEM_TEST
    g_exeFile = g_testExe ? fopen(g_testExe, "rb") : NULL;
#else
    wchar_t p[MAX_PATH];
    if (GetModuleFileNameW(NULL, p, MAX_PATH)) g_exeFile = _wfopen(p, L"rb");
#endif
    if (!g_exeFile) logf_("cannot open the exe file for reading - falling back to in-memory code (less robust against sibling dlls)");
    return g_exeFile != NULL;
}

/* file offset of n bytes at rva, 0 = not backed by the file */
static uint32_t rva_to_raw(uint64_t rva, size_t n)
{
    if (g_nsecs && rva + n <= g_secs[0].va && rva + n <= g_secs[0].rawPtr) return (uint32_t)rva;     /* headers */
    for (int i = 0; i < g_nsecs; i++) {
        const PeSec *s = &g_secs[i];
        uint32_t mapped = s->rawSize < s->vsize ? s->rawSize : s->vsize;        /* bytes the loader really took from the file */
        if (rva >= s->va && rva + n <= (uint64_t)s->va + mapped) return s->rawPtr + (uint32_t)(rva - s->va);
    }
    return 0;
}

static int exe_file_read(uint64_t rva, void *out, size_t n)
{
    if (!exe_file_open()) return 0;
    uint32_t raw = rva_to_raw(rva, n);
    if (!raw) return 0;
    if (fseek(g_exeFile, (long)raw, SEEK_SET)) return 0;
    return fread(out, 1, n, g_exeFile) == n;
}

/* the finders are done: let go of the file (each module holds its own handle) */
static void exe_file_close(void)
{
    if (g_exeFile) { fclose(g_exeFile); g_exeFile = NULL; }
    g_exeFileTried = 0;
}

/* ------------------------------------------------------------------------------------ */
/* function starts from the unwind table (.pdata), never from prologue bytes            */
/* ------------------------------------------------------------------------------------ */
/* function start = 'mov rax,rsp' right after int3 padding, walking back from a reference inside it
   (fallback only: a sibling's entry patch breaks it) */
static uint8_t *func_start_before(uint8_t *ref, size_t maxback)
{
    for (uint8_t *p = ref; p > ref - maxback && p > g_text.p + 1; p--)
        if (p[-1] == 0xCC && p[0] == 0x48 && p[1] == 0x8B && p[2] == 0xC4) return p;
    return NULL;
}

/* the primary RUNTIME_FUNCTION covering rva (chained entries followed), or NULL */
static const PeRtf *pdata_entry(uint32_t rva)
{
    if (!g_pdata) return NULL;
    size_t lo = 0, hi = g_npdata;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (g_pdata[mid].end <= rva) lo = mid + 1;
        else if (g_pdata[mid].begin > rva) hi = mid;
        else { lo = mid; break; }
    }
    if (lo >= g_npdata || g_pdata[lo].begin > rva || g_pdata[lo].end <= rva) return NULL;
    const PeRtf *e = &g_pdata[lo];
    for (int depth = 0; depth < 16; depth++) {
        if (e->unwind & 1) {                                            /* indirect: points at another RUNTIME_FUNCTION */
            const PeRtf *t = (const PeRtf *)(g_base + (e->unwind & ~1u));
            if (!in_image(t) || !in_image(t + 1)) return NULL;
            e = t; continue;
        }
        const uint8_t *ui = g_base + e->unwind;
        if (!in_image(ui) || !in_image(ui + 4)) return NULL;
        if (!((ui[0] >> 3) & 4)) return e;                              /* UNW_FLAG_CHAININFO clear: primary entry */
        const PeRtf *parent = (const PeRtf *)(ui + 4 + ((ui[2] + 1) & ~1) * 2);
        if (!in_image(parent) || !in_image(parent + 1)) return NULL;
        e = parent;
    }
    return NULL;
}

/* start of the function that contains ref (the exe's own view, independent of any entry patch) */
static uint8_t *func_start_of(uint8_t *ref, size_t maxback)
{
    if (in_text(ref)) {
        const PeRtf *e = pdata_entry((uint32_t)(ref - g_base));
        if (e && in_text(g_base + e->begin)) return g_base + e->begin;
    }
    return func_start_before(ref, maxback);
}

/* ------------------------------------------------------------------------------------ */
/* memory / object readers                                                              */
/* ------------------------------------------------------------------------------------ */
typedef struct { union { char buf[16]; char *ptr; } u; size_t size, cap; } MsString;   /* MSVC std::string */

#define TAG_SIZE    0x60    /* Tag vector element */
#define TAG_STR     0x40    /* MSVC std::string inside the element */

static int rd_mem(const void *src, void *dst, size_t n)
{
#ifdef GEM_TEST
    memcpy(dst, src, n);
    return 1;
#else
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), src, dst, n, &got) && got == n;
#endif
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
   hooks cover. 0 = f starts with a jump that is not a GEM hook (another dll's): the original is unknown here. */
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

/* the first n (<= HK_SAVE_MAX) bytes of f as the EXE FILE has them: survives any hook by any dll. Falls back to
   the GEM stub chain when the file cannot be read. */
static int orig_bytes(const uint8_t *f, uint8_t *out, size_t n)
{
    if (n > HK_SAVE_MAX || !in_text(f)) return 0;
    if (exe_file_read((uint64_t)(f - g_base), out, n)) return 1;
    uint8_t b[HK_SAVE_MAX];
    if (!hk_original(f, b, sizeof b, NULL)) return 0;
    memcpy(out, b, n);
    return 1;
}

/* first byte of f past any entry patch (pattern scans start here, so a hook's address bytes never match):
   past the GEM stub chain, or past a foreign 14-byte jump + NOP padding */
static uint8_t *hk_body(uint8_t *f)
{
    uint8_t b[HK_SAVE_MAX]; size_t n = 0;
    if (hk_original(f, b, sizeof b, &n)) return f + n;
    if (is_absjmp(f)) { uint8_t *p = f + 14; while (p < f + HK_SAVE_MAX && *p == 0x90) p++; return p; }
    return f;
}

/* the function a vftable slot pointed at in the exe file. A sibling dll (ScriptExt) swaps slots to its own code;
   the engine function is still there, so we hook it and its replacement calls through into our hook. */
static uint8_t *vt_slot_orig(const uint8_t *vt, int slot, const char *what)
{
    uint8_t *f = *(uint8_t **)(vt + slot * 8);
    if (in_text(f)) return f;
    uint64_t va = 0;
    if (in_rdata(vt + slot * 8) && exe_file_read((uint64_t)(vt + slot * 8 - g_base), &va, 8) && va >= g_prefBase) {
        uint8_t *g = g_base + (va - g_prefBase);
        if (in_text(g)) { logf_("%s: vftable slot %d points outside the exe (another dll's) - using the exe file's entry exe+0x%llX", what, slot, (unsigned long long)(g - g_base)); return g; }
    }
    return f;
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
static void *g_hooked[16];       /* targets this module patched (the chain test walks them) */
static int g_nhooked;

static uint8_t *hk_alloc(void) { return calloc(1, 128); }
static void hk_seal(uint8_t *mem) { (void)mem; }
static void hk_free(uint8_t *mem) { free(mem); }
static int patch_code(uint8_t *f, const uint8_t *bytes, size_t n, const char *what) { (void)what; memcpy(f, bytes, n); return 1; }
#else
static uint8_t *hk_alloc(void) { return VirtualAlloc(NULL, 128, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE); }
static void hk_seal(uint8_t *mem) { DWORD old; VirtualProtect(mem, 128, PAGE_EXECUTE_READ, &old); }
static void hk_free(uint8_t *mem) { VirtualFree(mem, 0, MEM_RELEASE); }

/* Write n bytes of code at f with every other thread of the process suspended and none of them stopped inside
   [f, f+n). Under the launcher's early-bird injection (the only injection path since the 2026-11-01 launcher
   cleanup) no other thread runs game code yet - only the loader's own worker threads exist, and one that exits
   while we look is skipped - so this is a safety net for any other way of loading the dll. No allocation and no
   logging happen while threads are suspended (a suspended thread may own the heap or stdio lock). */
static int patch_code(uint8_t *f, const uint8_t *bytes, size_t n, const char *what)
{
    HANDLE hs[256]; int nh = 0, unsafe = 0;
    DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) unsafe = 1;
    else {
        THREADENTRY32 te; te.dwSize = sizeof te;
        if (Thread32First(snap, &te)) do {
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == me) continue;
            if (nh >= 256) { unsafe = 1; break; }
            HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION | SYNCHRONIZE, FALSE, te.th32ThreadID);
            if (!h) { if (GetLastError() == ERROR_INVALID_PARAMETER) continue; unsafe = 1; break; }   /* gone since the snapshot */
            if (SuspendThread(h) == (DWORD)-1) {
                int gone = WaitForSingleObject(h, 0) == WAIT_OBJECT_0;
                CloseHandle(h);
                if (gone) continue;
                unsafe = 1; break;
            }
            hs[nh++] = h;
            CONTEXT c; memset(&c, 0, sizeof c); c.ContextFlags = CONTEXT_CONTROL;
            if (!GetThreadContext(h, &c)) { if (WaitForSingleObject(h, 0) == WAIT_OBJECT_0) continue; unsafe = 1; break; }
            if (c.Rip >= (uint64_t)f && c.Rip < (uint64_t)(f + n)) { unsafe = 1; break; }
        } while (Thread32Next(snap, &te));
        CloseHandle(snap);
    }
    int ok = 0; DWORD err = 0;
    if (!unsafe) {
        DWORD old;
        if (VirtualProtect(f, n, PAGE_EXECUTE_READWRITE, &old)) {
            memcpy(f, bytes, n);
            VirtualProtect(f, n, old, &old);
            FlushInstructionCache(GetCurrentProcess(), f, n);
            ok = 1;
        } else err = GetLastError();
    }
    for (int i = 0; i < nh; i++) { ResumeThread(hs[i]); CloseHandle(hs[i]); }
    if (unsafe) logf_("%s: could not suspend and inspect every other thread (or one is inside the bytes to patch) - not patched", what);
    else if (!ok) logf_("%s: VirtualProtect failed (%lu)", what, err);
    return ok;
}
#endif

/* Patch target to jump to hook; *orig = trampoline that runs what was there before (the original code or the
   previous hook). Called from DllMain: the launcher loads the dll before the engine runs game code. */
static int install_hook_at(const char *what, void *target, void *hook, void **orig)
{
    uint8_t *f = (uint8_t *)target;
    size_t n = prolog_len(f);
    if (!n || n > HK_SAVE_MAX) {
        if (f[0] == 0xE9 || f[0] == 0xEB) logf_("%s starts with a short jump (another dll's hook) - cannot chain onto it, not hooking", what);
        else logf_("%s prologue not hookable - not hooking", what);
        return 0;
    }
    uint8_t chk[HK_SAVE_MAX];
    int chained = is_absjmp(f);
    int foreign = chained && !hk_original(f, chk, sizeof chk, NULL);

    uint8_t *mem = hk_alloc();
    if (!mem) { logf_("%s: stub allocation failed", what); return 0; }
    uint8_t *stub = mem + HK_STUB, *tramp = mem + HK_TRAMP;
    stub[0] = 0xFF; stub[1] = 0x25; *(uint32_t *)(stub + 2) = 0; *(uint64_t *)(stub + 6) = (uint64_t)hook;
    memcpy(stub + 14, HK_MAGIC, 8);
    stub[22] = (uint8_t)n;
    memcpy(stub + 23, f, n);
    memcpy(tramp, f, n);
    tramp[n] = 0xFF; tramp[n + 1] = 0x25; *(uint32_t *)(tramp + n + 2) = 0;
    *(uint64_t *)(tramp + n + 6) = (uint64_t)(f + n);
    hk_seal(mem);

    uint8_t patch[HK_SAVE_MAX]; memset(patch, 0x90, sizeof patch);
    patch[0] = 0xFF; patch[1] = 0x25; *(uint32_t *)(patch + 2) = 0;
    *(uint64_t *)(patch + 6) = (uint64_t)stub;
    *orig = tramp;                      /* published before the jump goes live: a thread may enter the hook at once */
    if (!patch_code(f, patch, n, what)) { *orig = NULL; hk_free(mem); return 0; }
    if (chained) logf_("%s: chained onto an earlier hook (%s)", what, foreign ? "another dll's 14-byte jump" : "GEM stub");
#ifdef GEM_TEST
    if (g_nhooked < 16) g_hooked[g_nhooked++] = f;
#endif
    return 1;
}

/* ------------------------------------------------------------------------------------ */
/* module start-up                                                                      */
/* ------------------------------------------------------------------------------------ */
#ifndef GEM_TEST
/* module prologue / epilogue (one per feature, from gem_weaponscript.c's DllMain after the process-level checks):
   [section] Enabled, Verbose, the exe's sections. open() == 0 = this feature does nothing. */
static int gem_module_open(void)
{
    if (!ini_int("Enabled", 1)) { logf_("disabled ([%ls] Enabled=0)", GEM_SECT); return 0; }
    g_verbose = ini_int("Verbose", 1);
    if (!get_sections((uint8_t *)GetModuleHandleW(NULL))) { logf_("not a PE I understand"); return 0; }
    return 1;
}
static void gem_module_close(void) { exe_file_close(); }
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
static uint8_t *g_launchVt;                 /* eBulletShooter vftable */
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
static int is_spawn_bullet(const uint8_t *f)        /* f may already be hooked by a sibling dll (ours or not) */
{
    uint8_t h[HK_SAVE_MAX];
    if (!orig_bytes(f, h, sizeof h)) return 0;
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
    g_launchVt = vt;
    uint8_t *f = vt_slot_orig(vt, 11, "Launch"), head[HK_SAVE_MAX];
    if (!in_text(f) || !orig_bytes(f, head, sizeof head) || memcmp(head, k_launchProlog, sizeof k_launchProlog)) { logf_("Launch: slot 11 not recognised"); return 0; }
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

/* multiplayer note, logged once a feature is ON (the launcher ships the same payload to every player) */
static void log_mp_note(int active)
{
    if (active) logf_("simulation change: every player of a multiplayer match needs this DLL (the Indomitus Launcher payload)");
    else        logf_("network games: standing down (AllowMultiplayer=0), vanilla behaviour online");
}

#ifdef GEM_TEST
/* what the finders located, for the chain test's foreign-hook scenarios (same layout as in test\chain_test.c) */
typedef struct { uint8_t *launch, *spawnBullet, *bulletLaunch, *bulletQuant, *launchVt; } GemFound;
static void gem_found(GemFound *out)
{
    memset(out, 0, sizeof *out);
    out->launch = g_launch; out->spawnBullet = (uint8_t *)g_spawnBullet; out->launchVt = g_launchVt;
}
/* reset everything a selftest run leaves behind, so the next load order starts clean */
static void gem_test_reset(void)
{
    g_nhooked = 0;
    exe_file_close();
    g_launch = NULL; g_launchVt = NULL; g_spawnBullet = NULL; g_getShooterBone = NULL; g_boneRefresh = NULL;
    g_getAimPoint = NULL; g_isNetGame = NULL; g_nextSyncId = NULL; g_hitVec = NULL;
    g_weaponOff = g_stuffOff = g_syncedOff = g_aimerOff = g_bonePosOff = g_boneXOff = 0;
    g_stuffTagsBeg = g_stuffTagsEnd = g_aimerDirSlot = g_aimerPosSlot = 0;
    g_ownerOff = g_ownerItemOff = g_itemFlagOff = g_aimTargetOff = 0;
}
#endif

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

#endif /* GEM_COMMON_H */
