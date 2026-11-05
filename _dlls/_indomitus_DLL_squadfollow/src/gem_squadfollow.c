/* GEM SquadFollow 0.2 - looser squad follow for Call to Arms: Gates of Hell (GEM 2, 1.067)
 * Indomitus Launcher payload dll (gem_squadfollow.dll + gem_squadfollow.ini).
 *
 * Squad members in the brain state "following" (and the "move_follow" command) ask
 * NeedRefollow(brain) every brain tick. Vanilla returns true - and the follower gets a
 * brand-new move order, which restarts its run animation - when its formation slot has
 * drifted from the point of the last order by more than sqrt(20*2) ~ 6.3 units AND
 * drift^2 > 0.5 * (slot-to-commander)^2. While the leader runs the slot drifts all the
 * time, so followers are re-ordered every few metres.
 *
 * This dll replaces that function (same float math, same fields) with:
 *   drift^2 > MinDrift^2  AND  drift^2 > DriftRatio * lead^2
 *   AND at least MinIntervalMs of game time since the last order (skipped past ForceDrift)
 * Game time = the engine's own AI clock (global uint32 ms, read by the brain timers), so
 * every machine decides the same way in a lockstep game, as long as all run the dll.
 *
 * Layout (all read back from the function bytes, not hard-coded):
 *   brain+0x30 human, human+0x38 commander, commander+0x74/+0x78 position,
 *   human+0x138/+0x13C current slot point, brain+0x3CC/+0x3D0 slot point at the last order.
 *
 * 0.2 (launcher adaptation, 2026-11-05):
 *   - only acts inside call_to_arms*.exe (game / editor / server), one copy per process
 *   - unicode paths for the ini and log (the payload lives under %LocalAppData%)
 *   - stamp table: an expired entry is reset when its brain comes back, so the answer no
 *     longer depends on whether another brain reused the slot (pointer hashes differ
 *     between machines -> 0.1 could decide differently on two peers = MP desync risk)
 *   - table + stats under an SRW lock (safe if brains ever tick on worker threads)
 *   - ini values clamped; "not found" says whether the exe build is unknown or another
 *     dll already patched NeedRefollow (checked against call_to_arms*.exe on disk)
 *   - the dll pins itself once the patch is in (it must never be unloaded)
 *
 * Build: build.bat (repo root of this dll)   Offline finder + equivalence test: build.bat test
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <wchar.h>

#define SF_VERSION "0.2"

/* ---------------------------------------------------------------- config */
static int      cfg_enabled   = 1;
static float    cfg_min2      = 144.0f;  /* MinDrift^2 (vanilla 40) */
static float    cfg_ratio     = 0.5f;    /* vanilla 0.5 */
static uint32_t cfg_interval  = 600;     /* ms, vanilla 0 */
static float    cfg_force2    = 1600.0f; /* ForceDrift^2, 0 = off */
static int      cfg_log       = 0;       /* 0 off, 1 stats, 2 stats + every re-order */

/* ---------------------------------------------------------------- found layout */
static uint8_t  o_human, o_cmdr, o_cx, o_cy;
static uint32_t o_lx, o_ly, o_sx, o_sy;
static volatile uint32_t *g_clock;  /* engine AI clock, ms */

/* ---------------------------------------------------------------- log */
static wchar_t g_logpath[MAX_PATH];
static FILE   *g_log;
static void logf_(const char *fmt, ...)
{
    if (!g_log) {
        if (!g_logpath[0]) return;
        g_log = _wfopen(g_logpath, L"a");
        if (!g_log) return;
    }
    va_list ap; va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

/* ---------------------------------------------------------------- lock */
static SRWLOCK g_lock = SRWLOCK_INIT;

/* ---------------------------------------------------------------- per-brain stamp table */
/* lx/ly = the last-order point seen for this brain, stamp = game time it was first seen.
 * Every result must depend only on (brain state, game clock), never on where the brain sits
 * in the table: pointer values - and so hash slots - differ between machines. */
typedef struct { uintptr_t brain; float lx, ly; uint32_t stamp, touched; } Entry;
#define TBL_BITS 14
#define TBL_SIZE (1u << TBL_BITS)
#define TBL_PROBE 64
#define TBL_EXPIRE 10000u
static Entry g_tbl[TBL_SIZE];

static void entry_reset(Entry *e, uintptr_t brain, uint32_t now)
{
    e->brain = brain;
    e->lx = e->ly = 3.4e38f;   /* forces "new order seen" on this call */
    e->stamp = 0; e->touched = now;
}

static Entry *lookup(uintptr_t brain, uint32_t now)
{
    uint32_t h = (uint32_t)(((uint64_t)(brain >> 4) * 0x9E3779B97F4A7C15ull) >> (64 - TBL_BITS));
    Entry *freeslot = NULL;
    for (uint32_t i = 0; i < TBL_PROBE; i++) {
        Entry *e = &g_tbl[(h + i) & (TBL_SIZE - 1)];
        if (e->brain == brain) {
            /* not checked for TBL_EXPIRE: treat exactly like a brand-new entry, which is what
             * it would be had another brain taken the slot meanwhile (0.1 kept the old stamp) */
            if (now - e->touched > TBL_EXPIRE) entry_reset(e, brain, now);
            return e;
        }
        if (!freeslot && (e->brain == 0 || now - e->touched > TBL_EXPIRE)) freeslot = e;
        if (e->brain == 0) break;
    }
    if (freeslot) entry_reset(freeslot, brain, now);
    return freeslot;
}

/* ---------------------------------------------------------------- stats */
static uint32_t st_calls, st_vanilla, st_new, st_blk_drift, st_blk_time, st_forced, st_full;
static uint32_t st_last = 0;
static int      st_lines = 0;

static void stats_tick(uint32_t now)
{
    if (cfg_log < 1 || !g_clock) return;
    if (st_last == 0) { st_last = now; return; }
    if (now - st_last < 10000u) return;
    logf_("t=%u  10s: checks %u, vanilla would re-order %u, re-ordered %u "
          "(held by drift %u, by interval %u, forced %u, table full %u)",
          now, st_calls, st_vanilla, st_new, st_blk_drift, st_blk_time, st_forced, st_full);
    st_calls = st_vanilla = st_new = st_blk_drift = st_blk_time = st_forced = st_full = 0;
    st_last = now;
}

/* ---------------------------------------------------------------- the replacement */
#define F(p, o) (*(const float *)((const uint8_t *)(p) + (o)))
#define P(p, o) (*(uint8_t *const *)((const uint8_t *)(p) + (o)))

static char __attribute__((ms_abi)) need_refollow(uint8_t *brain)
{
    uint8_t *h = P(brain, o_human);
    uint8_t *c = P(h, o_cmdr);
    float sx = F(h, o_sx), sy = F(h, o_sy);
    float lx = F(brain, o_lx), ly = F(brain, o_ly);
    float cx = F(c, o_cx), cy = F(c, o_cy);
    float a = ly - sy, b = lx - sx;
    float drift2 = a * a + b * b;          /* engine: (ly-sy)^2 + (lx-sx)^2 */
    float e = cy - sy, f = cx - sx;
    float lead2 = e * e + f * f;           /* engine: (cy-sy)^2 + (cx-sx)^2 */

    int vanilla = drift2 > 20.0f * 2.0f && drift2 > lead2 * 0.5f;
    if (!cfg_enabled) return (char)vanilla;

    uint32_t now = g_clock ? *g_clock : 0;
    int result;
    AcquireSRWLockExclusive(&g_lock);

    Entry *en = NULL;
    uint32_t since = 0;
    if (cfg_interval && g_clock) {
        en = lookup((uintptr_t)brain, now);
        if (en) {
            en->touched = now;
            if (en->lx != lx || en->ly != ly) { en->lx = lx; en->ly = ly; en->stamp = now; }
            since = now - en->stamp;
        }
    }

    st_calls++;
    st_vanilla += vanilla;
    stats_tick(now);

    int forced = cfg_force2 > 0.0f && drift2 > cfg_force2;
    if (!(drift2 > cfg_min2 && drift2 > lead2 * cfg_ratio)) {
        if (vanilla) st_blk_drift++;
        result = 0;
    } else if (cfg_interval && g_clock && !forced && en && since < cfg_interval) {
        st_blk_time++;
        result = 0;
    } else {
        if (cfg_interval && g_clock && !forced && !en) st_full++;
        st_new++;
        st_forced += forced;
        if (cfg_log >= 2 && st_lines < 3000) {
            st_lines++;
            logf_("t=%u re-order brain=%p drift=%.2f lead=%.2f since=%ums%s",
                  now, (void *)brain, __builtin_sqrtf(drift2), __builtin_sqrtf(lead2), since,
                  forced ? " (forced)" : "");
        }
        result = 1;
    }

    ReleaseSRWLockExclusive(&g_lock);
    return (char)result;
}

/* ---------------------------------------------------------------- finders */
/* NeedRefollow, bytes +0x00..+0x5D; 0x100 = wildcard */
static const uint16_t SIG_FN[] = {
    0x48,0x8B,0x51,0x100,                       /* mov rdx,[rcx+human]        */
    0xF3,0x0F,0x10,0xA1,0x100,0x100,0x100,0x100,/* movss xmm4,[rcx+lastY]     */
    0x48,0x8B,0x42,0x100,                       /* mov rax,[rdx+cmdr]         */
    0xF3,0x0F,0x10,0x92,0x100,0x100,0x100,0x100,/* movss xmm2,[rdx+slotX]     */
    0xF3,0x0F,0x10,0x8A,0x100,0x100,0x100,0x100,/* movss xmm1,[rdx+slotY]     */
    0xF3,0x0F,0x5C,0xE1,                        /* subss xmm4,xmm1            */
    0xF3,0x0F,0x10,0x40,0x100,                  /* movss xmm0,[rax+cx]        */
    0xF3,0x0F,0x10,0x58,0x100,                  /* movss xmm3,[rax+cy]        */
    0xF3,0x0F,0x5C,0xC2, 0xF3,0x0F,0x5C,0xD9,
    0xF3,0x0F,0x59,0xE4, 0xF3,0x0F,0x59,0xC0, 0xF3,0x0F,0x59,0xDB, 0xF3,0x0F,0x58,0xD8,
    0xF3,0x0F,0x10,0x81,0x100,0x100,0x100,0x100,/* movss xmm0,[rcx+lastX]     */
    0xF3,0x0F,0x5C,0xC2, 0xF3,0x0F,0x59,0xC0, 0xF3,0x0F,0x58,0xE0,
    0xF3,0x0F,0x10,0x05                         /* movss xmm0,[rip+20.0]      */
};
#define SIG_FN_N (sizeof SIG_FN / sizeof SIG_FN[0])
/* brain timer: mov ebx,[rdi+X]; call GetClock; cmp eax,ebx; jb far */
static const uint16_t SIG_CLK[] = {
    0x8B,0x9F,0x100,0x100,0x00,0x00, 0xE8,0x100,0x100,0x100,0x100, 0x3B,0xC3, 0x0F,0x82
};
#define SIG_CLK_N (sizeof SIG_CLK / sizeof SIG_CLK[0])

static int text_range(uint8_t *base, uint8_t **lo, uint8_t **hi)
{
    IMAGE_DOS_HEADER *d = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(base + d->e_lfanew);
    IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, s++)
        if (!memcmp(s->Name, ".text", 5)) {
            *lo = base + s->VirtualAddress;
            *hi = *lo + s->Misc.VirtualSize;
            return 1;
        }
    return 0;
}

static int match(const uint8_t *p, const uint16_t *sig, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (sig[i] != 0x100 && p[i] != sig[i]) return 0;
    return 1;
}

static uint8_t *find_unique(uint8_t *lo, uint8_t *hi, const uint16_t *sig, size_t n, int *count)
{
    uint8_t *hit = NULL; *count = 0;
    for (uint8_t *p = lo; p + n <= hi; p++)
        if (match(p, sig, n)) { if (!hit) hit = p; (*count)++; }
    return hit;
}

/* returns NULL on success, else a reason; *not_found = 1 when the pattern had no hit */
static const char *find_all(uint8_t *base, uint8_t **fn_out, volatile uint32_t **clk_out, int *not_found)
{
    uint8_t *lo, *hi;
    *not_found = 0;
    if (!text_range(base, &lo, &hi)) return "no .text section";
    int n;
    uint8_t *fn = find_unique(lo, hi, SIG_FN, SIG_FN_N, &n);
    if (n == 0) { *not_found = 1; return "NeedRefollow not found"; }
    if (n > 1) return "NeedRefollow pattern not unique";
    /* verify the constants are 20.0 * 2.0 */
    if (fn + 0x6A > hi) return "NeedRefollow tail out of range";
    int32_t d1 = *(int32_t *)(fn + 0x5E), d2 = *(int32_t *)(fn + 0x66);
    if (fn[0x62] != 0xF3 || fn[0x63] != 0x0F || fn[0x64] != 0x59 || fn[0x65] != 0x05)
        return "NeedRefollow tail differs";
    float k1 = *(float *)(fn + 0x62 + d1), k2 = *(float *)(fn + 0x6A + d2);
    if (k1 != 20.0f || k2 != 2.0f) return "NeedRefollow constants differ";
    o_human = fn[0x03]; o_ly = *(uint32_t *)(fn + 0x08);
    o_cmdr  = fn[0x0F]; o_sx = *(uint32_t *)(fn + 0x14); o_sy = *(uint32_t *)(fn + 0x1C);
    o_cx    = fn[0x28]; o_cy = fn[0x2D];
    o_lx    = *(uint32_t *)(fn + 0x4A);
    *fn_out = fn;

    /* clock: the brain timer site (`mov ebx,[rdi+X]; call GetClock; cmp; jb`) nearest to
     * NeedRefollow - the brain code - whose callee is `mov eax,[rip+g]; ret`. A site whose
     * call another payload dll redirected fails the callee check and is skipped. */
    *clk_out = NULL;
    intptr_t best = INTPTR_MAX;
    for (uint8_t *p = lo; p + SIG_CLK_N <= hi; p++) {
        if (!match(p, SIG_CLK, SIG_CLK_N)) continue;
        uint8_t *t = p + 11 + *(int32_t *)(p + 7);
        if (t < lo || t + 7 > hi || t[0] != 0x8B || t[1] != 0x05 || t[6] != 0xC3) continue;
        intptr_t dist = p > fn ? p - fn : fn - p;
        if (dist >= best || dist > 0x100000) continue;
        best = dist;
        *clk_out = (volatile uint32_t *)(t + 6 + *(int32_t *)(t + 2));
    }
    return NULL;
}

#ifndef SF_TEST
/* ---------------------------------------------------------------- ini */
static wchar_t g_ini[MAX_PATH];

static float ini_f(const wchar_t *key, float def)
{
    wchar_t buf[64], dflt[64];
    swprintf(dflt, 64, L"%g", (double)def);
    GetPrivateProfileStringW(L"SquadFollow", key, dflt, buf, 64, g_ini);
    float v = (float)_wtof(buf);
    return v == v ? v : def;   /* NaN -> default */
}
static int ini_i(const wchar_t *key, int def)
{
    return (int)GetPrivateProfileIntW(L"SquadFollow", key, def, g_ini);
}
static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

static void load_ini(void)
{
    cfg_enabled  = ini_i(L"Enabled", 1) != 0;
    float md     = clampf(ini_f(L"MinDrift", 12.0f), 0.0f, 1000.0f);
    cfg_min2     = md * md;
    cfg_ratio    = clampf(ini_f(L"DriftRatio", 0.5f), 0.0f, 100.0f);
    int iv       = ini_i(L"MinIntervalMs", 600);
    cfg_interval = (uint32_t)(iv < 0 ? 0 : iv > 60000 ? 60000 : iv);
    float fd     = clampf(ini_f(L"ForceDrift", 40.0f), 0.0f, 10000.0f);
    cfg_force2   = fd > 0 ? fd * fd : 0.0f;
    int lg       = ini_i(L"Log", 0);
    cfg_log      = lg < 0 ? 0 : lg > 2 ? 2 : lg;
}

/* ---------------------------------------------------------------- "not found": why? */
/* Scan the exe file on disk. If NeedRefollow is there, the running image was changed by
 * someone else (another dll, or a second copy of this one); otherwise the build is unknown. */
static long disk_sig_rva(void)
{
    wchar_t exe[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return -1;
    HANDLE f = CreateFileW(exe, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return -1;
    long rva = -1;
    uint8_t hdr[4096], *buf = NULL;
    DWORD got = 0;
    if (!ReadFile(f, hdr, sizeof hdr, &got, NULL) || got < 0x200) goto done;
    IMAGE_DOS_HEADER *d = (IMAGE_DOS_HEADER *)hdr;
    if (d->e_magic != IMAGE_DOS_SIGNATURE || d->e_lfanew < 0 || d->e_lfanew > (LONG)got - 0x200) goto done;
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(hdr + d->e_lfanew);
    IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
    int ns = nt->FileHeader.NumberOfSections;
    if ((uint8_t *)(s + ns) > hdr + got) goto done;
    for (int i = 0; i < ns; i++, s++) {
        if (memcmp(s->Name, ".text", 5)) continue;
        DWORD sz = s->SizeOfRawData < s->Misc.VirtualSize ? s->SizeOfRawData : s->Misc.VirtualSize;
        if (sz < SIG_FN_N || sz > 256u * 1024 * 1024) break;
        buf = (uint8_t *)HeapAlloc(GetProcessHeap(), 0, sz);
        if (!buf) break;
        LARGE_INTEGER pos; pos.QuadPart = s->PointerToRawData;
        if (!SetFilePointerEx(f, pos, NULL, FILE_BEGIN) || !ReadFile(f, buf, sz, &got, NULL) || got != sz) break;
        int cnt;
        uint8_t *hit = find_unique(buf, buf + sz, SIG_FN, SIG_FN_N, &cnt);
        if (cnt == 1) rva = (long)(s->VirtualAddress + (DWORD)(hit - buf));
        break;
    }
done:
    if (buf) HeapFree(GetProcessHeap(), 0, buf);
    CloseHandle(f);
    return rva;
}

/* ---------------------------------------------------------------- install */
static void install(const char *exe_name)
{
    uint8_t *base = (uint8_t *)GetModuleHandleW(NULL);
    uint8_t *fn = NULL;
    int not_found = 0;
    logf_("[GEM SquadFollow %s] %s | Enabled=%d MinDrift=%.2f DriftRatio=%.2f MinIntervalMs=%u "
          "ForceDrift=%.2f Log=%d", SF_VERSION, exe_name, cfg_enabled, __builtin_sqrtf(cfg_min2),
          cfg_ratio, cfg_interval, __builtin_sqrtf(cfg_force2), cfg_log);
    const char *err = find_all(base, &fn, &g_clock, &not_found);
    if (err) {
        if (not_found) {
            long rva = disk_sig_rva();
            if (rva >= 0) {
                const uint8_t *m = base + rva;
                logf_("[GEM SquadFollow] NOT installed: NeedRefollow at exe+0x%lX was already changed in memory "
                      "by another dll (starts %02X %02X %02X %02X %02X %02X) - nothing patched",
                      rva, m[0], m[1], m[2], m[3], m[4], m[5]);
            } else
                logf_("[GEM SquadFollow] NOT installed: NeedRefollow not found in this exe or on disk "
                      "(unknown game build, needs 1.067) - nothing patched");
        } else
            logf_("[GEM SquadFollow] NOT installed: %s - nothing patched", err);
        return;
    }
    uint8_t patch[14] = { 0xFF, 0x25, 0, 0, 0, 0 };
    uint64_t dst = (uint64_t)(uintptr_t)&need_refollow;
    memcpy(patch + 6, &dst, 8);
    DWORD old;
    if (!VirtualProtect(fn, sizeof patch, PAGE_EXECUTE_READWRITE, &old)) {
        logf_("[GEM SquadFollow] NOT installed: VirtualProtect failed (%lu)", GetLastError());
        return;
    }
    memcpy(fn, patch, sizeof patch);
    VirtualProtect(fn, sizeof patch, old, &old);
    FlushInstructionCache(GetCurrentProcess(), fn, sizeof patch);

    HMODULE self;
    int pinned = GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                                    (LPCWSTR)(void *)&need_refollow, &self) != 0;
    logf_("[GEM SquadFollow] patched: NeedRefollow entry exe+0x%llX (14-byte jmp, replaced, no trampoline), "
          "clock exe+0x%llX%s%s",
          (unsigned long long)(fn - base),
          g_clock ? (unsigned long long)((uint8_t *)g_clock - base) : 0ull,
          g_clock ? "" : " (NOT FOUND - MinIntervalMs off)",
          pinned ? "" : " (could not pin the dll)");
    logf_("[GEM SquadFollow] %s; simulation change: every player in a multiplayer match needs this dll "
          "with the same ini", cfg_enabled ? "ON" : "installed, Enabled=0 (vanilla answers)");
}

static int is_game_exe(const wchar_t *nm)
{
    if (_wcsnicmp(nm, L"call_to_arms", 12)) return 0;
    if (_wcsnicmp(nm, L"call_to_arms_workshop", 21) == 0) return 0;
    return 1;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID res)
{
    (void)res;
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(inst);

    wchar_t exe[MAX_PATH], *nm;
    DWORD ne = GetModuleFileNameW(NULL, exe, MAX_PATH);
    if (ne == 0 || ne >= MAX_PATH) return TRUE;
    nm = wcsrchr(exe, L'\\'); nm = nm ? nm + 1 : exe;
    if (!is_game_exe(nm)) return TRUE;          /* not GoH: stay inert */

    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(inst, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH - 16) return TRUE;
    wchar_t *dot = wcsrchr(path, L'.');
    wchar_t *sl = wcsrchr(path, L'\\');
    if (dot && (!sl || dot > sl)) *dot = 0;
    const wchar_t *suffix = wcsstr(nm, L"_ed") ? L"_editor" : (wcsstr(nm, L"_server") ? L"_server" : L"");
    swprintf(g_ini, MAX_PATH, L"%ls.ini", path);
    swprintf(g_logpath, MAX_PATH, L"%ls%ls.log", path, suffix);

    char exe_a[MAX_PATH];
    WideCharToMultiByte(CP_UTF8, 0, nm, -1, exe_a, sizeof exe_a, NULL, NULL);

    /* one copy per process (e.g. the payload copy plus a gem_inject test copy) */
    wchar_t mx[64];
    swprintf(mx, 64, L"Local\\gem_squadfollow_%lu", GetCurrentProcessId());
    HANDLE h = CreateMutexW(NULL, FALSE, mx);   /* kept open for the process lifetime */
    if (h && GetLastError() == ERROR_ALREADY_EXISTS) {
        logf_("[GEM SquadFollow %s] %s: another copy is already loaded in this process - this one stays off",
              SF_VERSION, exe_a);
        CloseHandle(h);
        return TRUE;
    }

    load_ini();
    install(exe_a);
    return TRUE;
}
#else
/* ---------------------------------------------------------------- offline test */
/* build.bat test -> sf_test.exe <exe>...: runs the finders on a mapped exe and checks that
 * the replacement with vanilla settings returns exactly what the engine function returns.
 * test/run_host_test.sh builds the same test on Linux (the exe image is mapped by hand). */
typedef char (__attribute__((ms_abi)) *fn_t)(uint8_t *);

#ifdef _WIN32
static uint8_t *map_image(const char *path)
{
    HMODULE m = LoadLibraryExA(path, NULL, DONT_RESOLVE_DLL_REFERENCES);
    if (!m) printf("%s: load failed %lu\n", path, GetLastError());
    return (uint8_t *)m;
}
#else
uint8_t *host_map_image(const char *path);   /* test/host_map.c */
static uint8_t *map_image(const char *path) { return host_map_image(path); }
#endif

static uint32_t rng = 12345;
static float frand(float r)
{
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return ((rng & 0xFFFFFF) / (float)0xFFFFFF * 2.0f - 1.0f) * r;
}

int main(int argc, char **argv)
{
    int bad = 0;
    for (int i = 1; i < argc; i++) {
        uint8_t *base = map_image(argv[i]), *fn = NULL;
        if (!base) { bad = 1; continue; }
        volatile uint32_t *clk = NULL;
        int nf;
        const char *err = find_all(base, &fn, &clk, &nf);
        if (err) { printf("%s: FAIL %s\n", argv[i], err); bad = 1; continue; }
        printf("%s: fn exe+0x%llX clock exe+0x%llX | human+%X cmdr+%X cmd pos +%X/+%X slot +%X/+%X last +%X/+%X\n",
               argv[i], (unsigned long long)(fn - base),
               clk ? (unsigned long long)((uint8_t *)clk - base) : 0ull,
               o_human, o_cmdr, o_cx, o_cy, o_sx, o_sy, o_lx, o_ly);
        if (!clk) { printf("  FAIL clock not found\n"); bad = 1; }

        /* the patched image must not be found again (second copy / other dll case) */
        {
            uint8_t save[14]; memcpy(save, fn, 14);
            memset(fn, 0xCC, 2); fn[0] = 0xFF; fn[1] = 0x25;
            uint8_t *fn2 = NULL; volatile uint32_t *c2 = NULL; int nf2;
            const char *e2 = find_all(base, &fn2, &c2, &nf2);
            printf("  after patch: %s -> %s\n", e2 ? e2 : "found", (e2 && nf2) ? "OK" : "FAIL");
            if (!(e2 && nf2)) bad = 1;
            memcpy(fn, save, 14);
            find_all(base, &fn, &clk, &nf);   /* restore layout globals */
        }

        /* vanilla equivalence: replacement with vanilla numbers vs the real engine code */
        static uint8_t brain[0x1000], human[0x1000], cmdr[0x1000];
        *(uint8_t **)(brain + o_human) = human;
        *(uint8_t **)(human + o_cmdr) = cmdr;
        g_clock = NULL;
        cfg_enabled = 1; cfg_min2 = 40.0f; cfg_ratio = 0.5f; cfg_interval = 0; cfg_force2 = 0; cfg_log = 0;
        fn_t orig = (fn_t)fn;
        int diff = 0, trues = 0, N = 2000000;
        for (int k = 0; k < N; k++) {
            float r = (k & 3) == 0 ? 6.0f : (k & 3) == 1 ? 30.0f : (k & 3) == 2 ? 300.0f : 5000.0f;
            float sx = frand(4000), sy = frand(4000);
            *(float *)(human + o_sx) = sx; *(float *)(human + o_sy) = sy;
            *(float *)(brain + o_lx) = sx + frand(r); *(float *)(brain + o_ly) = sy + frand(r);
            *(float *)(cmdr + o_cx) = sx + frand(r); *(float *)(cmdr + o_cy) = sy + frand(r);
            char a = orig(brain) ? 1 : 0, b = need_refollow(brain) ? 1 : 0;
            trues += a;
            if (a != b) diff++;
        }
        printf("  vanilla equivalence: %d/%d differ (%d true)\n", diff, N, trues);
        if (diff) bad = 1;

        /* Enabled=0 must give the vanilla answer too */
        cfg_enabled = 0; cfg_min2 = 144.0f; cfg_interval = 600; cfg_force2 = 1600.0f;
        diff = 0;
        for (int k = 0; k < 200000; k++) {
            float sx = frand(4000), sy = frand(4000);
            *(float *)(human + o_sx) = sx; *(float *)(human + o_sy) = sy;
            *(float *)(brain + o_lx) = sx + frand(30); *(float *)(brain + o_ly) = sy + frand(30);
            *(float *)(cmdr + o_cx) = sx + frand(30); *(float *)(cmdr + o_cy) = sy + frand(30);
            if ((orig(brain) ? 1 : 0) != (need_refollow(brain) ? 1 : 0)) diff++;
        }
        printf("  Enabled=0: %d/200000 differ -> %s\n", diff, diff ? "FAIL" : "OK");
        if (diff) bad = 1;
        cfg_enabled = 1;

        /* interval logic with a fake clock */
        static uint32_t fake;
        g_clock = &fake; fake = 1000;
        cfg_min2 = 144.0f; cfg_interval = 600; cfg_force2 = 1600.0f;
        memset(g_tbl, 0, sizeof g_tbl);
        *(float *)(human + o_sx) = 0; *(float *)(human + o_sy) = 0;
        *(float *)(cmdr + o_cx) = 0;  *(float *)(cmdr + o_cy) = 20;
        *(float *)(brain + o_lx) = 0; *(float *)(brain + o_ly) = 15;   /* drift 15, lead 20 */
        char t1 = need_refollow(brain);           /* first sight: stamp = now -> held */
        fake += 599; char t2 = need_refollow(brain);
        fake += 1;   char t3 = need_refollow(brain);
        *(float *)(brain + o_ly) = 41;  /* "new order" with a different point, drift 41 > force */
        char t4 = need_refollow(brain);
        *(float *)(brain + o_ly) = 11;  /* drift 11 < 12 */
        fake += 5000; char t5 = need_refollow(brain);
        printf("  interval: held %d, held %d, go %d, forced %d, below drift %d -> %s\n",
               t1, t2, t3, t4, t5, (!t1 && !t2 && t3 && t4 && !t5) ? "OK" : "FAIL");
        if (!(!t1 && !t2 && t3 && t4 && !t5)) bad = 1;

        /* expiry: a brain unseen for > 10 s must answer the same whether its table slot
         * survived (machine A) or was taken by another brain (machine B) */
        {
            char r[2];
            for (int pass = 0; pass < 2; pass++) {
                memset(g_tbl, 0, sizeof g_tbl);
                fake = 50000;
                *(float *)(brain + o_lx) = 0; *(float *)(brain + o_ly) = 15;   /* drift 15 */
                need_refollow(brain);                 /* seen, stamped at 50000 */
                fake += 700; need_refollow(brain);    /* past interval: goes */
                fake += 20000;                        /* unseen 20 s */
                if (pass == 1) {                      /* machine B: slot reclaimed */
                    for (uint32_t k = 0; k < TBL_SIZE; k++)
                        if (g_tbl[k].brain == (uintptr_t)brain) {
                            g_tbl[k].brain = (uintptr_t)brain + 0x10000;   /* another brain */
                            g_tbl[k].touched = fake;
                        }
                }
                r[pass] = need_refollow(brain);       /* same order point as before */
            }
            printf("  expiry: slot kept %d, slot reclaimed %d -> %s\n", r[0], r[1],
                   r[0] == r[1] ? "OK" : "FAIL (peers would disagree)");
            if (r[0] != r[1]) bad = 1;
        }
    }
    printf(bad ? "SOME FAILED\n" : "ALL OK\n");
    return bad;
}
#endif
