/* gem_melee patch registry (spec 2.2 "Patch registry"): every engine write of the dll goes through these. One table of
 * CORE_PATCH_MAX entries under an SRW lock; overlap and expected-bytes checks; code writes with the other threads suspended
 * (the logic of mc/patch:37-62; inside the attach batch one thread walk serves every code write). The writers return 1 for
 * WRITTEN and 0 for every other state. No stubs, no jumps: the bytes a module hands over are written verbatim. Nothing is logged,
 * allocated or locked while other threads are suspended. */
#include "core_int.h"
#include <tlhelp32.h>
#include <stdio.h>
#include <string.h>

static SRWLOCK      g_plock = SRWLOCK_INIT;
static core_patch_t g_tab[CORE_PATCH_MAX];
static volatile LONG g_n;
static volatile int g_reported;                       /* core_patch_report ran: later writes (stance H2) are logged as they happen */
static core_susp_stats_t g_ss;                        /* suspension counters (under g_plock) */

static const char *const k_state[] = { "?", "WRITTEN", "REFUSED_OVERLAP", "REFUSED_BYTES", "REFUSED_FULL", "FAILED" };
static const char *state_name(int s) { return s >= 1 && s <= 5 ? k_state[s] : k_state[0]; }

static void hexs(const uint8_t *b, uint32_t n, char *out, size_t cap)
{
    size_t k = 0;
    out[0] = 0;
    if (n > CORE_PATCH_MAXLEN) n = CORE_PATCH_MAXLEN;
    for (uint32_t i = 0; i < n && k + 4 < cap; i++) k += (size_t)snprintf(out + k, cap - k, i ? " %02X" : "%02X", b[i]);
    if (!n) snprintf(out, cap, "-");
}

/* suspend every other thread of the process, retrying while one of them executes inside [lo, hi) (mc/patch:37-62); -1 = the
 * threads cannot all be stopped safely (no snapshot, more than max threads, one keeps running inside the range) */
#define CORE_MAX_THREADS 1024
static HANDLE g_hs[CORE_MAX_THREADS];                 /* used under g_plock only */
static int suspend_others(HANDLE *hs, int max, uint8_t *lo, uint8_t *hi)
{
    for (int attempt = 0; attempt < 20; attempt++) {
        int n = 0, bad = 0, over = 0;
        g_ss.walks++;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap == INVALID_HANDLE_VALUE) return -1;
        THREADENTRY32 te; memset(&te, 0, sizeof te); te.dwSize = sizeof te;
        for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
            if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == GetCurrentThreadId()) continue;
            if (n >= max) { over = 1; break; }
            HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID);
            if (!h) continue;
            if (SuspendThread(h) == (DWORD)-1) { CloseHandle(h); continue; }
            CONTEXT c; c.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(h, &c) && (uint8_t *)c.Rip >= lo && (uint8_t *)c.Rip < hi) bad = 1;
            hs[n++] = h;
        }
        CloseHandle(snap);
        if (!bad && !over) return n;
        for (int i = 0; i < n; i++) { ResumeThread(hs[i]); CloseHandle(hs[i]); }
        if (over) return -1;
        Sleep(5);
    }
    return -1;
}
static void resume_others(HANDLE *hs, int n) { for (int i = 0; i < n; i++) { ResumeThread(hs[i]); CloseHandle(hs[i]); } }

/* attach batch (review INST-1, spec 2.2 step 4): core_attach_all runs inside DllMain, under the loader lock, so a thread created
 * during the attach runs no user code before DllMain returns and the set of threads that can execute is fixed for the whole
 * attach. The first code write of the batch that suspends walks the thread snapshot ONCE and keeps the other threads' handles;
 * every code write of the batch then only suspends / checks / resumes those handles (same bounded retry as suspend_others);
 * core_patch_batch_end closes them. Only writes from the thread that opened the batch use it; a failed walk (no snapshot, more
 * than CORE_MAX_THREADS threads) FAILS that write as suspend_others does and is tried again at the next one. */
static HANDLE  g_bh[CORE_MAX_THREADS];                /* the batch's handles (under g_plock) */
static uint8_t g_bf[CORE_MAX_THREADS];                /* per handle: 1 = suspended by the write in progress, 2 = seen exited */
static int     g_batch, g_bn = -1;                    /* batch open; handles in g_bh (-1 = no walk yet) */
static DWORD   g_batch_tid;

static int batch_walk(void)
{
    int n = 0, over = 0;
    g_ss.walks++;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return -1;
    THREADENTRY32 te; memset(&te, 0, sizeof te); te.dwSize = sizeof te;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == GetCurrentThreadId()) continue;
        if (n >= CORE_MAX_THREADS) { over = 1; break; }
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID);
        if (!h) continue;
        g_bh[n] = h; g_bf[n] = 0; n++;
    }
    CloseHandle(snap);
    if (over) { for (int i = 0; i < n; i++) CloseHandle(g_bh[i]); return -1; }
    g_bn = n;
    if (n > g_ss.others) g_ss.others = n;
    return n;
}
static void batch_resume(void) { for (int i = 0; i < g_bn; i++) if (g_bf[i] & 1) { ResumeThread(g_bh[i]); g_bf[i] &= 2; } }
/* 0 = every other thread of the batch is stopped outside [lo, hi) (batch_resume undoes it), -1 = it could not be done safely */
static int batch_suspend(uint8_t *lo, uint8_t *hi)
{
    if (g_bn < 0 && batch_walk() < 0) return -1;
    for (int attempt = 0; attempt < 20; attempt++) {
        int bad = 0;
        for (int i = 0; i < g_bn; i++) {
            if (SuspendThread(g_bh[i]) == (DWORD)-1) { g_bf[i] = 2; continue; }        /* exited: skipped, never resumed */
            g_bf[i] = (uint8_t)((g_bf[i] & 2) | 1);
            CONTEXT c; c.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(g_bh[i], &c) && (uint8_t *)c.Rip >= lo && (uint8_t *)c.Rip < hi) bad = 1;
        }
        if (!bad) return 0;
        batch_resume();
        Sleep(5);
    }
    return -1;
}

void core_patch_batch_begin(void)
{
    AcquireSRWLockExclusive(&g_plock);
    if (!g_batch) { g_batch = 1; g_bn = -1; g_batch_tid = GetCurrentThreadId(); g_ss.batches++; }
    ReleaseSRWLockExclusive(&g_plock);
}

void core_patch_batch_end(void)
{
    AcquireSRWLockExclusive(&g_plock);
    if (g_batch) {
        for (int i = 0; i < g_bn; i++) { if (g_bf[i] & 2) g_ss.exited++; CloseHandle(g_bh[i]); }   /* none is suspended here */
        g_bn = -1; g_batch = 0; g_batch_tid = 0;
    }
    ReleaseSRWLockExclusive(&g_plock);
}

void core_patch_suspend_stats(core_susp_stats_t *out)
{
    if (!out) return;
    AcquireSRWLockShared(&g_plock);
    *out = g_ss;
    out->open = g_bn > 0 ? g_bn : 0;
    ReleaseSRWLockShared(&g_plock);
}

/* the attach's "code writes:" line (core_attach_all logs it after core_patch_report) */
void core_patch_suspend_line(void)
{
    core_susp_stats_t s; core_patch_suspend_stats(&s);
    LARGE_INTEGER f; QueryPerformanceFrequency(&f);
    double ms = f.QuadPart > 0 ? (double)s.qpc * 1000.0 / (double)f.QuadPart : 0.0;
    char how[160], fail[96] = "";
    if (!core_patch_suspend_on()) {
        core_log(CORE_MOD_CORE, "code writes: %ld, the other threads not suspended ([Core] PatchSuspendThreads=0)", s.plain);
        return;
    }
    if (!s.susp && !s.failed) {
        core_log(CORE_MOD_CORE, "code writes: none (%s)", s.batches ? "attach batch open, no thread walk needed" : "no thread walk");
        return;
    }
    if (s.batched && s.batched == s.susp + s.failed) snprintf(how, sizeof how, "attach batch, %d other thread%s", s.others, s.others == 1 ? "" : "s");
    else if (!s.batched) snprintf(how, sizeof how, "one per write, no attach batch, up to %d other thread%s", s.others, s.others == 1 ? "" : "s");
    else snprintf(how, sizeof how, "attach batch for %ld of the writes, one walk per write for the others, up to %d other threads", s.batched, s.others);
    if (s.exited) snprintf(how + strlen(how), sizeof how - strlen(how), ", %ld exited during the batch", s.exited);
    if (s.failed) snprintf(fail, sizeof fail, ", %ld FAILED (the other threads could not be stopped safely)", s.failed);
    core_log(CORE_MOD_CORE, "code writes: %ld with the other threads suspended, %ld thread walk%s (%s), %.1f ms%s", s.susp, s.walks,
             s.walks == 1 ? "" : "s", how, ms, fail);
}

static void entry_line(int i, const core_patch_t *e, char *out, size_t cap)
{
    char a[48], o[CORE_PATCH_MAXLEN * 3 + 4], w[CORE_PATCH_MAXLEN * 3 + 4];
    core_rva_str(e->addr, a, sizeof a);
    uint32_t n = e->len <= CORE_PATCH_MAXLEN && e->state != CORE_PATCH_REFUSED_FULL && e->addr ? e->len : 0;   /* "-": bytes never read */
    hexs(e->orig, n, o, sizeof o); hexs(e->now, n, w, sizeof w);
    snprintf(out, cap, "#%02d %-6s %-28s %s %2u bytes %s -> %s %s", i + 1, core_mod_name(e->mod), e->name ? e->name : "?", a, e->len, o, w, state_name(e->state));
}

static int patch_do(int mod, int kind, const char *name, uint8_t *addr, size_t len, const void *expected, const void *bytes, void **oldp)
{
    char why[160] = "";
    core_patch_t e; memset(&e, 0, sizeof e);
    e.mod = mod; e.kind = kind; e.name = name ? name : "?"; e.addr = addr; e.len = (uint32_t)(len > 0xFFFFFFFFu ? 0xFFFFFFFFu : len);
    AcquireSRWLockExclusive(&g_plock);
    int idx = (int)g_n;
    if (idx >= CORE_PATCH_MAX) {                                          /* table full: nothing recorded */
        ReleaseSRWLockExclusive(&g_plock);
        char a[48]; core_rva_str(addr, a, sizeof a);
        core_log(CORE_MOD_CORE, "patch REFUSED (table full, %d entries): %s %s %s %u bytes", CORE_PATCH_MAX, core_mod_name(mod), e.name, a, e.len);
        return 0;
    }
    if (len < 1 || len > CORE_PATCH_MAXLEN || (kind == CORE_PATCH_PTR && len != 8)) {
        e.state = CORE_PATCH_REFUSED_FULL;
        snprintf(why, sizeof why, "REFUSED (length %llu not in 1..%d)", (unsigned long long)len, CORE_PATCH_MAXLEN);
    } else if (!addr || !bytes) {
        e.state = CORE_PATCH_FAILED;
        snprintf(why, sizeof why, "FAILED (NULL %s)", addr ? "bytes" : "address");
    } else {
        for (int i = 0; i < idx; i++) {                                   /* overlap with any WRITTEN entry, any module */
            const core_patch_t *o = &g_tab[i];
            if (o->state != CORE_PATCH_WRITTEN) continue;
            if (addr < o->addr + o->len && o->addr < addr + len) {
                char a[48]; core_rva_str(o->addr, a, sizeof a);
                e.state = CORE_PATCH_REFUSED_OVERLAP;
                snprintf(why, sizeof why, "REFUSED (overlap with %s %s %s)", core_mod_name(o->mod), o->name, a);
                break;
            }
        }
    }
    if (!e.state || e.state == CORE_PATCH_REFUSED_OVERLAP) {
        memcpy(e.orig, addr, len);                                        /* ptr: the slot's 8 bytes (an overlap refusal shows what is there) */
        memcpy(e.now, e.orig, len);
    }
    if (!e.state) {
        int differs = expected && (kind == CORE_PATCH_PTR ? *(void *const *)addr != expected : memcmp(addr, expected, len) != 0);
        if (differs) {
            char x[CORE_PATCH_MAXLEN * 3 + 4], f[CORE_PATCH_MAXLEN * 3 + 4];
            hexs(kind == CORE_PATCH_PTR ? (const uint8_t *)&expected : (const uint8_t *)expected, (uint32_t)len, x, sizeof x);
            hexs(e.orig, (uint32_t)len, f, sizeof f);
            e.state = CORE_PATCH_REFUSED_BYTES;
            snprintf(why, sizeof why, "REFUSED (bytes differ: expected %s found %s)", x, f);
        }
    }
    if (!e.state) {
        DWORD old = 0;
        if (kind == CORE_PATCH_CODE) {
            if (!VirtualProtect(addr, len, PAGE_EXECUTE_READWRITE, &old)) {
                e.state = CORE_PATCH_FAILED; snprintf(why, sizeof why, "FAILED (VirtualProtect failed (%lu))", GetLastError());
            } else {
                int n = 0, on = core_patch_suspend_on(), batched = on && g_batch && g_batch_tid == GetCurrentThreadId();
                int64_t q0 = on ? core_qpc() : 0;
                if (on) n = batched ? batch_suspend(addr, addr + len) : suspend_others(g_hs, CORE_MAX_THREADS, addr, addr + len);
                if (n < 0) {
                    g_ss.failed++; g_ss.batched += batched; g_ss.qpc += core_qpc() - q0;
                    VirtualProtect(addr, len, old, &old);
                    e.state = CORE_PATCH_FAILED; snprintf(why, sizeof why, "FAILED (could not stop the other threads safely)");
                } else {
                    memcpy(addr, bytes, len);
                    if (batched) batch_resume(); else resume_others(g_hs, n);
                    if (on) { g_ss.susp++; g_ss.batched += batched; g_ss.qpc += core_qpc() - q0; if (!batched && n > g_ss.others) g_ss.others = n; }
                    else g_ss.plain++;
                    VirtualProtect(addr, len, old, &old);
                    FlushInstructionCache(GetCurrentProcess(), addr, len);
                    e.state = CORE_PATCH_WRITTEN;
                }
            }
        } else if (kind == CORE_PATCH_DATA) {
            if (!VirtualProtect(addr, len, PAGE_READWRITE, &old)) {
                e.state = CORE_PATCH_FAILED; snprintf(why, sizeof why, "FAILED (VirtualProtect failed (%lu))", GetLastError());
            } else {
                memcpy(addr, bytes, len);
                VirtualProtect(addr, len, old, &old);
                e.state = CORE_PATCH_WRITTEN;
            }
        } else {
            void **slot = (void **)addr;
            if (!VirtualProtect(slot, 8, PAGE_READWRITE, &old)) {
                e.state = CORE_PATCH_FAILED; snprintf(why, sizeof why, "FAILED (VirtualProtect failed (%lu))", GetLastError());
            } else {
                if (oldp) *oldp = *slot;
                *slot = *(void *const *)bytes;                            /* an aligned 8-byte store */
                VirtualProtect(slot, 8, old, &old);
                e.state = CORE_PATCH_WRITTEN;
            }
        }
        if (e.state == CORE_PATCH_WRITTEN) memcpy(e.now, addr, len);
    }
    g_tab[idx] = e;
    MemoryBarrier();
    g_n = idx + 1;
    int log_written = e.state == CORE_PATCH_WRITTEN && g_reported;
    ReleaseSRWLockExclusive(&g_plock);
    if (e.state != CORE_PATCH_WRITTEN) {
        char a[48]; core_rva_str(addr, a, sizeof a);
        core_log(CORE_MOD_CORE, "patch %s: %s %s %s %u bytes", why, core_mod_name(mod), e.name, a, e.len);
        return 0;
    }
    if (log_written) { char l[400]; entry_line(idx, &e, l, sizeof l); core_log(CORE_MOD_CORE, "runtime patch %s", l); }
    return 1;
}

int core_patch_code(int mod, const char *name, void *addr, size_t len, const void *expected, const void *bytes)
{ return patch_do(mod, CORE_PATCH_CODE, name, (uint8_t *)addr, len, expected, bytes, NULL); }
int core_patch_data(int mod, const char *name, void *addr, size_t len, const void *expected, const void *bytes)
{ return patch_do(mod, CORE_PATCH_DATA, name, (uint8_t *)addr, len, expected, bytes, NULL); }
int core_patch_ptr(int mod, const char *name, void **slot, const void *expected, const void *value, void **old)
{ return patch_do(mod, CORE_PATCH_PTR, name, (uint8_t *)slot, 8, expected, &value, old); }

int core_patch_count(void) { return (int)g_n; }

int core_patch_get(int i, core_patch_t *out)
{
    int ok = 0;
    AcquireSRWLockShared(&g_plock);
    if (i >= 0 && i < (int)g_n && out) { *out = g_tab[i]; ok = 1; }
    ReleaseSRWLockShared(&g_plock);
    return ok;
}

void core_patch_report(void)
{
    static core_patch_t t[CORE_PATCH_MAX];                               /* attach-time only (one thread) */
    AcquireSRWLockExclusive(&g_plock);
    int n = (int)g_n;
    memcpy(t, g_tab, sizeof(core_patch_t) * (size_t)n);
    g_reported = 1;
    ReleaseSRWLockExclusive(&g_plock);
    int w = 0;
    for (int i = 0; i < n; i++) if (t[i].state == CORE_PATCH_WRITTEN) w++;
    core_log(CORE_MOD_CORE, "patches: %d written, %d refused/failed", w, n - w);
    for (int i = 0; i < n; i++) { char l[400]; entry_line(i, &t[i], l, sizeof l); core_log(CORE_MOD_CORE, "  %s", l); }
}
