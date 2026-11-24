/* gem_melee core (spec 2.2): log sink, ini, paths / exe image, clock finder, new-battle epoch, background thread, unit answers,
 * cease-fire, cost counters and the link hook forwarders. References no module, link or dllmain symbol, so the module test targets
 * link it with core_patch.c alone. */
#include "core_int.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <share.h>

static const char *const k_tag[CORE_MOD_N] = { "core", "chase", "stance", "pen", "link" };
const char *core_mod_name(int mod) { return (unsigned)mod < CORE_MOD_N ? k_tag[mod] : "?"; }

/* ------------------------------------------------------------------------------------------------ exe image */
static uint8_t *g_exe_base;                  /* NULL = not read yet */
static int      g_exe_test;                  /* core_test_set_exe was called */
static char     g_exe_name[MAX_PATH];
static wchar_t  g_exe_namew[MAX_PATH];
static uint8_t *g_text_lo, *g_text_hi;
static uint32_t g_exe_ts, g_exe_size;

static void image_read(uint8_t *base)
{
    g_text_lo = g_text_hi = NULL; g_exe_ts = 0; g_exe_size = 0;
    if (!base) return;
    const IMAGE_DOS_HEADER *d = (const IMAGE_DOS_HEADER *)base;
    if (d->e_magic != IMAGE_DOS_SIGNATURE) return;
    const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)(base + d->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;
    g_exe_ts = nt->FileHeader.TimeDateStamp;
    g_exe_size = nt->OptionalHeader.SizeOfImage;
    const IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++)
        if (!memcmp(sec->Name, ".text", 5)) { g_text_lo = base + sec->VirtualAddress; g_text_hi = g_text_lo + sec->Misc.VirtualSize; break; }
}

static void exe_ensure(void)
{
    if (g_exe_base) return;
    uint8_t *b = (uint8_t *)GetModuleHandleW(NULL);
    char p[MAX_PATH]; wchar_t pw[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, p, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) p[0] = 0;
    const char *nm = strrchr(p, '\\'); nm = nm ? nm + 1 : p;
    snprintf(g_exe_name, sizeof g_exe_name, "%s", nm);
    n = GetModuleFileNameW(NULL, pw, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) pw[0] = 0;
    const wchar_t *nw = wcsrchr(pw, L'\\'); nw = nw ? nw + 1 : pw;
    wcsncpy(g_exe_namew, nw, MAX_PATH - 1); g_exe_namew[MAX_PATH - 1] = 0;
    image_read(b);
    g_exe_base = b;
}

uint8_t *core_exe_base(void) { exe_ensure(); return g_exe_base; }
const char *core_exe_name(void) { exe_ensure(); return g_exe_name; }
const wchar_t *core_exe_namew(void) { exe_ensure(); return g_exe_namew; }
uint32_t core_exe_timestamp(void) { exe_ensure(); return g_exe_ts; }
void core_text_range(uint8_t **lo, uint8_t **hi) { exe_ensure(); if (lo) *lo = g_text_lo; if (hi) *hi = g_text_hi; }
int core_in_text(const void *a) { return g_text_lo && (const uint8_t *)a >= g_text_lo && (const uint8_t *)a < g_text_hi; }

void core_test_set_exe(uint8_t *base, const char *exe_file_name)
{
    snprintf(g_exe_name, sizeof g_exe_name, "%s", exe_file_name ? exe_file_name : "");
    MultiByteToWideChar(CP_ACP, 0, g_exe_name, -1, g_exe_namew, MAX_PATH);
    g_exe_namew[MAX_PATH - 1] = 0;
    image_read(base);
    g_exe_base = base;
    g_exe_test = 1;
}

void core_rva_str(const volatile void *p, char *out, size_t cap)
{
    const uint8_t *a = (const uint8_t *)(uintptr_t)p, *b = core_exe_base();
    if (!cap) return;
    if (!a) snprintf(out, cap, "NULL");
    else if (b && g_exe_size && a >= b && a < b + g_exe_size) snprintf(out, cap, "exe+0x%llX", (unsigned long long)(a - b));
    else snprintf(out, cap, "0x%llX", (unsigned long long)(uintptr_t)a);
}

/* ------------------------------------------------------------------------------------------------ sim clock */
static volatile uint32_t *volatile g_clk;

volatile uint32_t *core_clock_ptr(void) { return g_clk; }
uint32_t core_now(void) { volatile uint32_t *c = g_clk; return c ? *c : 0; }
void core_test_set_clock(volatile uint32_t *clock) { g_clk = clock; }

/* ------------------------------------------------------------------------------------------------ log sink */
static INIT_ONCE        g_sink_once = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION g_sink_cs;
static volatile int     g_sink_mode = CORE_SINK_DISCARD;
static int              g_sink_test;           /* a test chose the sink: core_init leaves it alone */
static wchar_t          g_sink_path[MAX_PATH + 16];   /* wide: the log also opens in a folder whose name is outside the ANSI code page */
static FILE            *g_sink_fp;

static BOOL CALLBACK sink_once(PINIT_ONCE o, PVOID p, PVOID *c) { (void)o; (void)p; (void)c; InitializeCriticalSection(&g_sink_cs); return TRUE; }
static void sink_lock(void) { InitOnceExecuteOnce(&g_sink_once, sink_once, NULL, NULL); EnterCriticalSection(&g_sink_cs); }
static void sink_unlock(void) { LeaveCriticalSection(&g_sink_cs); }

static void sink_set(int mode, const wchar_t *path, int by_test)
{
    sink_lock();
    if (g_sink_fp) { fclose(g_sink_fp); g_sink_fp = NULL; }
    if (path) { wcsncpy(g_sink_path, path, MAX_PATH + 15); g_sink_path[MAX_PATH + 15] = 0; }
    g_sink_mode = mode;
    if (by_test) g_sink_test = 1;
    sink_unlock();
}
void core_test_sink(int mode) { sink_set(mode, NULL, 1); }
void core_test_sink_file(const char *log_path)
{
    wchar_t w[MAX_PATH + 16];
    if (!log_path || !MultiByteToWideChar(CP_ACP, 0, log_path, -1, w, MAX_PATH + 16)) w[0] = 0;
    w[MAX_PATH + 15] = 0;
    sink_set(CORE_SINK_FILE, w, 1);
}

/* "HH:MM:SS.mmm <sim> [tag] " */
static int line_prefix(char *b, size_t n, int mod)
{
    SYSTEMTIME st; GetLocalTime(&st);
    volatile uint32_t *c = g_clk;
    char sim[16];
    if (c) snprintf(sim, sizeof sim, "%9u", *c); else snprintf(sim, sizeof sim, "        -");
    int k = snprintf(b, n, "%02u:%02u:%02u.%03u %s [%s] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, sim, core_mod_name(mod));
    return k < 0 ? 0 : (size_t)k >= n ? (int)n - 1 : k;
}

/* may_open = 0 (the crash-path writer): write only to a file that is already open, never open it there (_wfsopen allocates a
 * CRT stream on a heap that may be the one that faulted; old MeleeChase's crash path only wrote to its open FILE, mc/core:25) */
static void sink_write(const char *line, size_t n, int may_open)
{
    sink_lock();
    if (g_sink_mode == CORE_SINK_FILE) {
        if (!g_sink_fp && may_open && g_sink_path[0]) g_sink_fp = _wfsopen(g_sink_path, L"a", _SH_DENYNO);
        if (g_sink_fp) { fwrite(line, 1, n, g_sink_fp); fflush(g_sink_fp); }
    } else if (g_sink_mode == CORE_SINK_STDOUT) {
        fwrite(line, 1, n, stdout); fflush(stdout);
    }
    sink_unlock();
}

void core_vlog(int mod, const char *fmt, va_list ap)
{
    if (g_sink_mode == CORE_SINK_DISCARD || !fmt) return;
    char buf[4096], *heap = NULL, *line = buf;
    int pre = line_prefix(buf, sizeof buf, mod);
    va_list ap2; va_copy(ap2, ap);
    int n = vsnprintf(buf + pre, sizeof buf - (size_t)pre, fmt, ap);
    if (n < 0) { n = 0; buf[pre] = 0; }
    if ((size_t)pre + (size_t)n + 2 > sizeof buf) {                 /* longer than the stack buffer: format again, no truncation */
        heap = (char *)malloc((size_t)pre + (size_t)n + 2);
        if (heap) { memcpy(heap, buf, (size_t)pre); vsnprintf(heap + pre, (size_t)n + 1, fmt, ap2); line = heap; }
        else n = (int)(sizeof buf - (size_t)pre - 2);              /* no memory: keep what fits */
    }
    va_end(ap2);
    line[pre + n] = '\n'; line[pre + n + 1] = 0;
    sink_write(line, (size_t)pre + (size_t)n + 1, 1);
    free(heap);
}

void core_log(int mod, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); core_vlog(mod, fmt, ap); va_end(ap);
}

/* crash-path writer: same line and sink, a fixed 512-byte stack buffer, truncation, no malloc; never opens the log file */
void core_log_fixed(int mod, const char *fmt, ...)
{
    if (g_sink_mode == CORE_SINK_DISCARD || !fmt) return;
    char buf[512];
    int pre = line_prefix(buf, sizeof buf - 2, mod);
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf + pre, sizeof buf - 1 - (size_t)pre, fmt, ap);
    va_end(ap);
    if (n < 0) n = 0;
    if ((size_t)pre + (size_t)n > sizeof buf - 2) n = (int)(sizeof buf - 2 - (size_t)pre);
    buf[pre + n] = '\n'; buf[pre + n + 1] = 0;
    sink_write(buf, (size_t)pre + (size_t)n + 1, 0);
}

/* ------------------------------------------------------------------------------------------------ ini */
static char    g_ini[MAX_PATH];
static wchar_t g_iniw[MAX_PATH];
static int     g_ini_test;

const char *core_ini_path(void) { return g_ini; }
const wchar_t *core_ini_pathw(void) { return g_iniw; }
/* p = the ANSI path (stance, pen, core and link read it: GetPrivateProfile*A, as MeleeStance / PenFilter did); w = the wide path
 * chase reads (MeleeChase built it from GetModuleFileNameW, mc/main:19, mc/core:32/41), NULL = p converted */
static void ini_set(const char *p, const wchar_t *w)
{
    snprintf(g_ini, sizeof g_ini, "%s", p ? p : "");
    if (w) { wcsncpy(g_iniw, w, MAX_PATH - 1); g_iniw[MAX_PATH - 1] = 0; }
    else if (!MultiByteToWideChar(CP_ACP, 0, g_ini, -1, g_iniw, MAX_PATH)) g_iniw[0] = 0;
    g_iniw[MAX_PATH - 1] = 0;
}
void core_test_set_ini(const char *ini_path) { ini_set(ini_path, NULL); g_ini_test = 1; }

int core_ini_int(const char *section, const char *key, int def)
{
    if (!g_ini[0]) return def;
    return (int)GetPrivateProfileIntA(section, key, def, g_ini);
}
void core_ini_str(const char *section, const char *key, const char *def, char *out, int cap)
{
    if (!out || cap <= 0) return;
    if (!def) def = "";
    if (!g_ini[0]) { snprintf(out, (size_t)cap, "%s", def); return; }
    GetPrivateProfileStringA(section, key, def, out, (DWORD)cap, g_ini);
}

/* ------------------------------------------------------------------------------------------------ init (3.1 step 1) */
static HMODULE g_self;
static int     g_init_ok;
static int     g_patch_suspend = 1, g_late_secs = 60;
static uint32_t g_back_ms = 1000;

HMODULE core_self(void) { return g_self; }
int core_init_ok(void) { return g_init_ok; }
int core_patch_suspend_on(void) { return g_patch_suspend; }
int core_late_guard_secs(void) { return g_late_secs; }

static const char *const k_old_ini[3] = { "gem_meleestance.ini", "gem_meleechase.ini", "gem_penfilter.ini" };

void core_init(HMODULE self)
{
    g_self = self;
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(self, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH - 16) { g_init_ok = 0; return; }        /* the ms:3290 / pf:1940 rule: do nothing at all */
    g_init_ok = 1;
    /* the same names from the wide path: in a folder whose name is outside the ANSI code page the A path holds '?' / best-fit
     * characters; chase's ini (as MeleeChase read it) and the log file use the wide one, everything else keeps the A one */
    wchar_t pw[MAX_PATH];
    DWORD nw = GetModuleFileNameW(self, pw, MAX_PATH);
    int wide = nw > 0 && nw < MAX_PATH - 16;
    if (!wide) pw[0] = 0;
    char *dot = strrchr(path, '.');
    if (dot) *dot = 0;
    wchar_t *dotw = wide ? wcsrchr(pw, L'.') : NULL;
    if (dotw) *dotw = 0;
    char dir[MAX_PATH];
    snprintf(dir, sizeof dir, "%s", path);
    char *bs = strrchr(dir, '\\');
    if (bs) bs[1] = 0; else dir[0] = 0;
    wchar_t dirw[MAX_PATH];
    wcsncpy(dirw, pw, MAX_PATH - 1); dirw[MAX_PATH - 1] = 0;
    wchar_t *bsw = wcsrchr(dirw, L'\\');
    if (bsw) bsw[1] = 0; else dirw[0] = 0;
    if (!g_ini_test) {
        char ini[MAX_PATH + 8]; snprintf(ini, sizeof ini, "%s.ini", path);
        wchar_t iniw[MAX_PATH + 8]; _snwprintf(iniw, MAX_PATH + 7, L"%ls.ini", pw); iniw[MAX_PATH + 7] = 0;
        ini_set(ini, wide ? iniw : NULL);
    }
    const char *nm = core_exe_name();
    const wchar_t *sfx = strstr(nm, "_ed") ? L"_editor" : (strstr(nm, "_server") ? L"_server" : L"");
    wchar_t logw[MAX_PATH + 16];
    if (wide) _snwprintf(logw, MAX_PATH + 15, L"%ls%ls.log", pw, sfx);
    else {
        wchar_t pa[MAX_PATH];
        if (!MultiByteToWideChar(CP_ACP, 0, path, -1, pa, MAX_PATH)) pa[0] = 0;
        pa[MAX_PATH - 1] = 0;
        _snwprintf(logw, MAX_PATH + 15, L"%ls%ls.log", pa, sfx);
    }
    logw[MAX_PATH + 15] = 0;

    int log = core_ini_int("Core", "Log", 1);
    int back = core_ini_int("Core", "NewBattleBackMs", 1000);
    g_back_ms = (uint32_t)(back < 1 ? 1 : back > 60000 ? 60000 : back);
    g_patch_suspend = core_ini_int("Core", "PatchSuspendThreads", 1);
    g_late_secs = core_ini_int("Core", "LateGuardSeconds", 60);
    if (g_late_secs < 0) g_late_secs = 0;
    if (!g_sink_test) sink_set(log ? CORE_SINK_FILE : CORE_SINK_DISCARD, logw, 0);

    int found = g_ini[0] && GetFileAttributesA(g_ini) != INVALID_FILE_ATTRIBUTES;
    int foundw = !found && g_iniw[0] && GetFileAttributesW(g_iniw) != INVALID_FILE_ATTRIBUTES;
    const char *ininame = strrchr(g_ini, '\\'); ininame = ininame ? ininame + 1 : g_ini;
    core_log(CORE_MOD_CORE, GEM_MELEE_NAME " " GEM_MELEE_VERSION " (" GEM_MELEE_MODULES ") loaded into %s (timestamp %08X), ini %s %s",
             nm, core_exe_timestamp(), g_ini, found ? "found" : foundw ? "found by its wide name only" : "NOT FOUND");
    if (foundw)
        core_log(CORE_MOD_CORE, "%s: the dll's folder name is outside the ANSI code page: chase reads the ini (wide path, as MeleeChase did); "
                 "stance, pen, core and link read their keys through the ANSI path, as MeleeStance / PenFilter did, and run on their code "
                 "defaults ([Charge] CooldownMs 4000 instead of 2000, [AutoDraw] ProbeAnyRange 1 instead of 0). Move the payload to a "
                 "folder with plain names", ininame);
    else if (!found)
        core_log(CORE_MOD_CORE, "%s not found next to the dll: every module runs on its code defaults ([Charge] CooldownMs 4000 instead of 2000, "
                 "[AutoDraw] ProbeAnyRange 1 instead of 0, [MeleeStance] AutoMeleeTag melee_infantry)", ininame);
    for (int i = 0; i < 3; i++) {
        char old[MAX_PATH + 32];
        snprintf(old, sizeof old, "%s%s", dir, k_old_ini[i]);
        wchar_t oldw[MAX_PATH + 32], onw[32];
        if (!MultiByteToWideChar(CP_ACP, 0, k_old_ini[i], -1, onw, 32)) onw[0] = 0;
        _snwprintf(oldw, MAX_PATH + 31, L"%ls%ls", dirw, onw); oldw[MAX_PATH + 31] = 0;
        int there = wide ? GetFileAttributesW(oldw) != INVALID_FILE_ATTRIBUTES : GetFileAttributesA(old) != INVALID_FILE_ATTRIBUTES;
        if (there) core_log(CORE_MOD_CORE, "%s ignored (%s replaces it)", k_old_ini[i], ininame);
    }
}

/* ------------------------------------------------------------------------------------------------ clock finder (ms:2601-2614) */
volatile uint32_t *core_clock_find(const uint8_t *base, int *hits)
{
    /* brain timers `mov ebx,[rdi+X]; call GetClock; cmp eax,ebx; jb` (stance's SIG_CLK, from GEM BoardFilter); 0x100 = any byte */
    static const uint16_t sig[] = { 0x8B, 0x9F, 0x100, 0x100, 0x00, 0x00, 0xE8, 0x100, 0x100, 0x100, 0x100, 0x3B, 0xC3, 0x0F, 0x82 };
    const size_t m = sizeof sig / sizeof sig[0];
    if (hits) *hits = 0;
    if (!base) return NULL;
    const IMAGE_DOS_HEADER *d = (const IMAGE_DOS_HEADER *)base;
    if (d->e_magic != IMAGE_DOS_SIGNATURE) return NULL;
    const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)(base + d->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return NULL;
    const IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    const uint8_t *lo = NULL, *hi = NULL;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++)
        if (!memcmp(sec->Name, ".text", 5)) { lo = base + sec->VirtualAddress; hi = lo + sec->Misc.VirtualSize; break; }
    if (!lo) return NULL;
    const uint8_t *cg[8] = { 0 }; int cc[8] = { 0 }, ncg = 0;
    for (const uint8_t *p = lo; p + m <= hi; p++) {
        size_t k = 0;
        while (k < m && (sig[k] == 0x100 || p[k] == sig[k])) k++;
        if (k < m) continue;
        const uint8_t *t = p + 11 + *(const int32_t *)(p + 7);                  /* rel32(p + 7) */
        if (t < lo || t + 7 > hi || t[0] != 0x8B || t[1] != 0x05 || t[6] != 0xC3) continue;
        const uint8_t *g = t + 6 + *(const int32_t *)(t + 2);                   /* rel32(t + 2) */
        int i = 0; while (i < ncg && cg[i] != g) i++;
        if (i == ncg) { if (ncg == 8) continue; cg[ncg++] = g; }
        cc[i]++;
    }
    int best = -1;
    for (int i = 0; i < ncg; i++) if (best < 0 || cc[i] > cc[best]) best = i;
    if (best < 0) return NULL;
    if (hits) *hits = cc[best];
    if (cc[best] < 5) return NULL;
    return (volatile uint32_t *)(uintptr_t)cg[best];
}

void core_clock_setup(void)
{
    int hits = 0;
    volatile uint32_t *c = core_clock_find(core_exe_base(), &hits);
    g_clk = c;
    if (c) { char a[48]; core_rva_str(c, a, sizeof a); core_log(CORE_MOD_CORE, "sim clock %s (%d getter calls)", a, hits); }
    else core_log(CORE_MOD_CORE, "sim clock NOT found: the core new-battle detector is off (each module keeps its own)");
}

/* ------------------------------------------------------------------------------------------------ new-battle epoch (2.2) */
typedef void (*epoch_fn_t)(uint32_t epoch, uint32_t now);
static volatile LONG64 g_state;                       /* (last << 32) | valid; 0 = no poll yet */
static volatile LONG   g_epoch, g_epoch_lines;
static volatile LONG   g_use[CORE_MOD_N] = { 1, 1, 1, 1, 1 };
static volatile LONG   g_det[CORE_MOD_N];
static volatile int    g_frozen;
static void *volatile  g_listen[4];

uint32_t core_epoch_poll(int mod)
{
    if (g_frozen) return 0;
    volatile uint32_t *clk = g_clk;
    if (!clk) return 0;                                              /* no core clock: detector idle, modules run on their own rules */
    if ((unsigned)mod >= CORE_MOD_N) mod = CORE_MOD_CORE;
    for (;;) {
        LONG64 st = g_state;                                         /* state FIRST, then the clock: a successful CAS proves nobody */
        uint32_t now = *clk, last = (uint32_t)((uint64_t)st >> 32);  /* moved the state between the two reads */
        LONG64 nx = (LONG64)(((uint64_t)now << 32) | 1);
        if ((st & 1) && now < last && last - now >= g_back_ms) {
            if (InterlockedCompareExchange64(&g_state, nx, st) != st) continue;   /* another thread got there: re-read */
            LONG e = InterlockedIncrement(&g_epoch);                 /* only the CAS winner: one fire per back-step */
            if (InterlockedIncrement(&g_epoch_lines) <= 200)
                core_log(CORE_MOD_CORE, "--- new battle %ld: sim clock %u -> %u ms (stance and pen flush at their next detector call)", (long)e, last, now);
            for (int i = 0; i < 4; i++) { epoch_fn_t fn = (epoch_fn_t)g_listen[i]; if (fn) fn((uint32_t)e, now); }   /* no core lock held */
            break;
        }
        if (st == nx || InterlockedCompareExchange64(&g_state, nx, st) == st) break;   /* same quant: no write at all */
    }
    return g_use[mod] ? (uint32_t)g_epoch : (uint32_t)g_det[mod];
}

uint32_t core_epoch(void) { return (uint32_t)g_epoch; }

void core_epoch_bind(int mod, const volatile void *module_clock)
{
    volatile uint32_t *clk = g_clk;
    if (!clk || (unsigned)mod >= CORE_MOD_N) return;
    if (module_clock == (const volatile void *)clk) { g_use[mod] = 1; return; }
    g_det[mod] = g_epoch;                                            /* frozen at its current value */
    MemoryBarrier();
    g_use[mod] = 0;
    char a[48], b[48];
    core_rva_str(module_clock, a, sizeof a); core_rva_str(clk, b, sizeof b);
    core_log(CORE_MOD_CORE, "WARNING: %s clock %s != core clock %s: %s keeps only its own new-battle rule", core_mod_name(mod), a, b, core_mod_name(mod));
}

int core_epoch_listen(void (*fn)(uint32_t epoch, uint32_t now))
{
    if (!fn) return 0;
    for (int i = 0; i < 4; i++)
        if (InterlockedCompareExchangePointer(&g_listen[i], (void *)fn, NULL) == NULL) return 1;
    return 0;
}

void core_on_quant(void) { core_epoch_poll(CORE_MOD_CORE); }
void core_test_freeze_epoch(int on) { g_frozen = on ? 1 : 0; }

/* ------------------------------------------------------------------------------------------------ background thread (3.3, 3.4) */
static void *volatile g_bg[8];
static volatile int  g_bg_started;
static DWORD         g_bg_t0, g_lg_last;
static const char *const k_old_dll[3] = { "gem_meleechase.dll", "gem_meleestance.dll", "gem_penfilter.dll" };
static const int k_old_mod[3] = { CORE_MOD_CHASE, CORE_MOD_STANCE, CORE_MOD_PEN };
static int g_lg_seen[3], g_lg_warned[3];
int core_guard_skip[CORE_MOD_N];

int core_bg_register(int mod, core_bg_fn fn)
{
    (void)mod;
    if (!fn) return 0;
    for (int i = 0; i < 8; i++)
        if (InterlockedCompareExchangePointer(&g_bg[i], (void *)fn, NULL) == NULL) return 1;
    return 0;
}

int core_bg_count(void) { int n = 0; for (int i = 0; i < 8; i++) if (g_bg[i]) n++; return n; }

void core_guard_early(void)
{
    for (int i = 0; i < 3; i++)
        if (GetModuleHandleA(k_old_dll[i])) {
            g_lg_seen[i] = 1; core_guard_skip[k_old_mod[i]] = 1;
            core_log(CORE_MOD_CORE, "%s is already loaded in this process: the %s module stays off (remove one of them)", k_old_dll[i], core_mod_name(k_old_mod[i]));
        }
}

static int late_guard_live(void) { return g_bg_t0 && g_late_secs > 0 && GetTickCount() - g_bg_t0 <= (DWORD)g_late_secs * 1000u; }

static void late_guard_step(void)
{
    if (!late_guard_live()) return;
    DWORD now = GetTickCount();
    if (g_lg_last && now - g_lg_last < 1000) return;
    g_lg_last = now ? now : 1;
    for (int i = 0; i < 3; i++)
        if (!g_lg_seen[i] && !g_lg_warned[i] && GetModuleHandleA(k_old_dll[i])) {
            g_lg_warned[i] = 1;
            core_log(CORE_MOD_CORE, "WARNING: %s was loaded after " GEM_MELEE_DLL ": remove it from the payload", k_old_dll[i]);
        }
}

/* 0.1.4: the process's memory in the log (Alex, Nov 21 2026 on 0.1.3: "100% a memory leak"; nothing in the log could say yes or no, and
 * the editor was already at 4.2 GB before the first in-place re-aim). Looked at every 5 s on the background thread; a line when the
 * private bytes moved 64 MB since the last line, or after 60 s. K32GetProcessMemoryInfo is looked up by name, so the dll still
 * imports KERNEL32 + the CRT only. */
typedef struct { DWORD cb, faults; SIZE_T peak_ws, ws, qpp, qp, qpnp, qnp, pagefile, peak_pagefile, priv; } core_pmc_t;
typedef BOOL (WINAPI *core_gpmi_t)(HANDLE, void *, DWORD);
static void mem_step(void)
{
    static DWORD last_look, last_line; static SIZE_T last_priv; static core_gpmi_t fn; static int tried;
    DWORD now = GetTickCount();
    if (last_look && now - last_look < 5000) return;
    last_look = now ? now : 1;
    if (!tried) { HMODULE k = GetModuleHandleA("kernel32.dll"); tried = 1; fn = k ? (core_gpmi_t)(void *)GetProcAddress(k, "K32GetProcessMemoryInfo") : NULL; }
    core_pmc_t m; memset(&m, 0, sizeof m); m.cb = sizeof m;
    if (!fn || !fn(GetCurrentProcess(), &m, sizeof m)) return;
    SIZE_T d = m.priv > last_priv ? m.priv - last_priv : last_priv - m.priv;
    if (last_line && d < ((SIZE_T)64 << 20) && now - last_line < 60000) return;
    core_log(CORE_MOD_CORE, "mem: private %lu MB (%s%lu since the last line), working set %lu MB, peak %lu MB", (unsigned long)(m.priv >> 20),
             m.priv >= last_priv ? "+" : "-", (unsigned long)(d >> 20), (unsigned long)(m.ws >> 20), (unsigned long)(m.peak_pagefile >> 20));
    last_priv = m.priv; last_line = now ? now : 1;
}

void core_bg_run_once(void)
{
    for (int i = 0; i < 8; i++) { core_bg_fn fn = (core_bg_fn)g_bg[i]; if (fn) fn(); }
    late_guard_step();
    mem_step();
}

static DWORD WINAPI bg_thread(LPVOID p)
{
    (void)p;
    for (;;) {
        Sleep(100);
        core_bg_run_once();
        if (!core_bg_count() && !late_guard_live()) return 0;          /* nothing left to do */
    }
}

int core_bg_start(void)
{
    if (g_bg_started) return 1;
    if (!core_bg_count() && g_late_secs <= 0) return 0;
    g_bg_t0 = GetTickCount(); if (!g_bg_t0) g_bg_t0 = 1;
    HANDLE h = CreateThread(NULL, 0, bg_thread, NULL, 0, NULL);
    if (!h) { core_log(CORE_MOD_CORE, "background thread could not be started (%lu)", GetLastError()); g_bg_t0 = 0; return 0; }
    CloseHandle(h);
    g_bg_started = 1;
    return 1;
}

/* ------------------------------------------------------------------------------------------------ link hook forwarders */
typedef void (*nfront_t)(const void *, const void *, const char *, uint32_t);
typedef void (*npick_t)(int, const uint8_t *, int, uint32_t);
typedef void (*nverd_t)(int, const void *, const void *, int, uint32_t);
typedef int  (*tunit_t)(const void *);
typedef void (*tvline_t)(int, const void *, const char *, va_list);
typedef int  (*veto_t)(int, void *);
static volatile nfront_t h_note_front;
static volatile npick_t  h_note_pick, h_trace_pick;
static volatile nverd_t  h_note_verdict;
static volatile tunit_t  h_trace_unit;
static volatile tvline_t h_trace_vline;
static volatile veto_t   h_veto;

volatile int core_flip_on, core_trace_on, core_timing_on, core_suspect_all;
float (*core_hull_point)(const uint8_t *from, const uint8_t *target, float *pt);   /* gem_melee 0.1.10: set by pen_attach */
core_suspect_t core_suspect[CORE_SUSPECT_N];

void core_set_link_hooks(const core_link_hooks_t *h)
{
    h_note_front   = h ? h->note_front : NULL;
    h_note_pick    = h ? h->note_pick : NULL;
    h_note_verdict = h ? h->note_verdict : NULL;
    h_trace_pick   = h ? h->trace_pick : NULL;
    h_trace_unit   = h ? h->trace_unit : NULL;
    h_trace_vline  = h ? h->trace_vline : NULL;
    h_veto         = h ? h->ceasefire_veto : NULL;
    MemoryBarrier();
}

void core_note_front(const void *unit, const void *order_vt, const char *name, uint32_t now)
{ nfront_t f = h_note_front; if (f) f(unit, order_vt, name, now); }
void core_note_pick(int mod, const uint8_t *ctx, int code)
{ npick_t f = h_note_pick; if (f) f(mod, ctx, code, core_now()); }
void core_note_verdict(int mod, const void *unit, const void *target, int code, uint32_t now)
{ nverd_t f = h_note_verdict; if (f) f(mod, unit, target, code, now); }
void core_trace_pick(int mod, const uint8_t *ctx, int code)
{ npick_t f = h_trace_pick; if (f) f(mod, ctx, code, core_now()); }
int core_trace_unit(const void *unit)
{ tunit_t f = h_trace_unit; return f ? f(unit) : 0; }
void core_trace_line(int mod, const void *unit, const char *fmt, ...)
{
    tvline_t f = h_trace_vline;
    if (!f) return;
    va_list ap; va_start(ap, fmt); f(mod, unit, fmt, ap); va_end(ap);
}

/* ------------------------------------------------------------------------------------------------ cease-fire (R3) */
static SRWLOCK g_cf_lock = SRWLOCK_INIT;
static volatile LONG g_cf_calls[CORE_MOD_N], g_cf_veto[CORE_MOD_N], g_cf_coll;
static void *g_cf_w; static uint32_t g_cf_clock; static int g_cf_mod = -1;

int core_ceasefire(int mod, void *weapon, void *engine_stop, uint64_t *ret)
{
    if (!weapon || !engine_stop) return 0;
    if ((unsigned)mod >= CORE_MOD_N) mod = CORE_MOD_CORE;
    veto_t v = h_veto;
    if (v && v(mod, weapon)) { InterlockedIncrement(&g_cf_veto[mod]); return 0; }   /* R3: link decides and logs */
    uint32_t now = core_now();
    AcquireSRWLockExclusive(&g_cf_lock);
    if (g_cf_w == weapon && g_cf_clock == now && g_cf_mod != mod) InterlockedIncrement(&g_cf_coll);   /* two modules, one sim ms */
    g_cf_w = weapon; g_cf_clock = now; g_cf_mod = mod;
    ReleaseSRWLockExclusive(&g_cf_lock);
    uint64_t r = ((uint64_t (*)(void *, uint64_t))engine_stop)(weapon, 1);   /* chase: g_clearTarget(w, 1), pen: e_wstop(w, 1) */
    InterlockedIncrement(&g_cf_calls[mod]);
    if (ret) *ret = r;
    return 1;
}

void core_ceasefire_stats(long calls[CORE_MOD_N], long vetoed[CORE_MOD_N], long *collisions)
{
    for (int i = 0; i < CORE_MOD_N; i++) { if (calls) calls[i] = g_cf_calls[i]; if (vetoed) vetoed[i] = g_cf_veto[i]; }
    if (collisions) *collisions = g_cf_coll;
}

/* ------------------------------------------------------------------------------------------------ cost counters (R8) */
static volatile LONG64 g_t_qpc[CORE_T_N];
static volatile LONG   g_t_calls[CORE_T_N];

void core_time_add(int id, int64_t qpc_ticks)
{
    if ((unsigned)id >= CORE_T_N) return;
    InterlockedAdd64(&g_t_qpc[id], qpc_ticks);
    InterlockedIncrement(&g_t_calls[id]);
}
void core_time_read(int id, int64_t *qpc_ticks, long *calls)
{
    int ok = (unsigned)id < CORE_T_N;
    if (qpc_ticks) *qpc_ticks = ok ? g_t_qpc[id] : 0;
    if (calls) *calls = ok ? g_t_calls[id] : 0;
}

/* ------------------------------------------------------------------------------------------------ engine strings (ms:101-153) */
/* MSVC std::string at p: buf/ptr +0, size +0x10, cap +0x18 (ms:104-111) */
static const char *str_get(const uint8_t *p, size_t *n)
{
    uint64_t size = *(const uint64_t *)(p + 0x10);
    uint64_t cap  = *(const uint64_t *)(p + 0x18);
    if (cap < size || cap > 0x10000 || size == 0) return NULL;
    *n = (size_t)size;
    return cap > 15 ? *(const char *const *)p : (const char *)p;
}
static char lc(char c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static int eq_ci(const char *s, size_t n, const char *z)
{
    size_t m = strlen(z);
    if (n != m) return 0;
    for (size_t i = 0; i < n; i++) if (lc(s[i]) != lc(z[i])) return 0;
    return 1;
}

/* stance vec_has (ms:123-137) for one word: 1 / 0 / -1 broken */
int core_vec_has_ci(const void *tag_vector, const char *word)
{
    if (!tag_vector || !word) return 0;
    const uint8_t *v = (const uint8_t *)tag_vector;
    const uint8_t *b = *(const uint8_t *const *)v;
    const uint8_t *e = *(const uint8_t *const *)(v + 8);
    if (!b || !e) return 0;
    if (e < b) return -1;
    uintptr_t len = (uintptr_t)(e - b);
    if (len % 0x60 || len > 0x60 * 1024) return -1;
    for (const uint8_t *t = b; t < e; t += 0x60) {
        size_t k; const char *s = str_get(t + 0x40, &k);
        if (!s) continue;
        if (eq_ci(s, k, word)) return 1;
    }
    return 0;
}

/* stance vec_str (ms:140-153), exact port: "name1 name2 ..." copied into out; returns the number of names copied */
int core_vec_names(const void *tag_vector, char *out, int cap)
{
    if (!out || cap < 1) return 0;
    out[0] = 0;
    if (!tag_vector) return 0;
    const uint8_t *v = (const uint8_t *)tag_vector;
    const uint8_t *b = *(const uint8_t *const *)v, *e = *(const uint8_t *const *)(v + 8);
    if (!b || !e || e < b || (uintptr_t)(e - b) % 0x60 || (uintptr_t)(e - b) > 0x60 * 64) return 0;
    int names = 0;
    for (const uint8_t *t = b; t < e; t += 0x60) {
        size_t k; const char *s = str_get(t + 0x40, &k);
        if (!s) continue;
        size_t l = strlen(out);
        if (l + k + 2 >= (size_t)cap) break;
        if (l) out[l++] = ' ';
        memcpy(out + l, s, k); out[l + k] = 0;
        names++;
    }
    return names;
}

/* ------------------------------------------------------------------------------------------------ shared unit answers */
typedef struct { const void *e; uint32_t key; core_unit_t u; } uslot_t;
static uslot_t g_ucache[1024];
static SRWLOCK g_ulock = SRWLOCK_INIT;

int core_unit(const void *entity, core_unit_t *out)
{
    if (!entity) { if (out) memset(out, 0, sizeof *out); return 0; }
    uint32_t now = core_now(), key = now + 1, s = core_suspect_slot(entity) & 1023u;
    int cache = g_clk != NULL;                                  /* no core clock: the key never changes, so read every time (RS-1) */
    core_unit_t u;
    int hit = 0;
    if (cache) {
        AcquireSRWLockShared(&g_ulock);
        hit = g_ucache[s].e == entity && g_ucache[s].key == key;
        if (hit) u = g_ucache[s].u;
        ReleaseSRWLockShared(&g_ulock);
    }
    if (!hit) {
        const uint8_t *e = (const uint8_t *)entity;
        memset(&u, 0, sizeof u);
        u.e = entity; u.clock = now;
        u.human = (e[0x30] & 4) != 0;                                                   /* ms:1949, pf:730, mc/melee:858 */
        u.actor = (e[0x30] & 2) != 0;                                                   /* ms:528, pf:741 */
        u.vehicle = u.actor && !u.human;                                                /* ms:380, pf:730 */
        u.owner = e[0xDA8];                                                             /* ms:611 */
        const uint8_t *br = *(const uint8_t *const *)(e + 0xDC8);
        u.player = br && (br[0x398] & 3) == 1;                                          /* ms:638-642, pf:731-735 */
        if (u.actor) {                                                                  /* pf:739-746 without the dc_on gate */
            const uint8_t *pr = *(const uint8_t *const *)(e + 0xDE0);
            const uint8_t *d = *(const uint8_t *const *)(e + 0xDD8);
            u.dc_sim = (pr && *(const int32_t *)(pr + 0x18) != 0) || (d && ((*(const uint32_t *)(d + 0x70) >> 1) & 1));
        }
        u.linked = (*(const uint32_t *)(e + 0x1C0) >> 28) & 1;                          /* ms:301 */
        u.id = *(const uint32_t *)(e + 0xC0);                                           /* pf:747 */
        u.traced = core_trace_unit(entity) ? 1 : 0;                                     /* link's trace_unit; outside the cache lock */
        if (cache) {
            AcquireSRWLockExclusive(&g_ulock);
            g_ucache[s].e = entity; g_ucache[s].key = key; g_ucache[s].u = u;
            ReleaseSRWLockExclusive(&g_ulock);
        }
    }
    if (out) *out = u;
    return 1;
}
