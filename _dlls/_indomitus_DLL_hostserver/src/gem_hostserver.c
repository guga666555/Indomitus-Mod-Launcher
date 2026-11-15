/* GEM HostServer 0.1 - the multiplayer host server starts frozen, so that the launcher can load the
 * payload into it before its first instruction.
 * Indomitus Launcher payload dll (gem_hostserver.dll + gem_hostserver.ini). Call to Arms: Gates of Hell 1.067.
 *
 * Why: hosting makes the game start call_to_arms_server.exe, and that server runs the simulation.
 * It reads the mod's scripts a few seconds after it starts and stops with "unknown condition" when
 * gem_scriptext.dll is not in yet. The game starts it running, and outside every job
 * (CreateProcessW with CREATE_BREAKAWAY_FROM_JOB at exe+0x3BB565), so the launcher can only find it
 * in the process list after it is already running, and then races the script loader. Launcher 0.9.13
 * lost that race by 76 ms on one PC (15 Nov 2026).
 *
 * What this dll does, in the game only:
 *   - the exe's import slot of CreateProcessW is pointed at hook_CreateProcessW (one pointer; no code
 *     of the game or of Windows is patched)
 *   - a call that starts call_to_arms_server.exe gets CREATE_SUSPENDED added. The dll then writes the
 *     new process and thread id into a small shared block, signals the launcher and waits for its
 *     answer; then it resumes the server (unless the game itself asked for a suspended start)
 *   - every other CreateProcessW call goes through untouched
 * The launcher does the rest exactly as it does for the game: it checks that the process really is
 * the game's call_to_arms_server.exe, writes its own SHA-256 checked copy of the payload and queues
 * the LoadLibraryW calls on the server's still-frozen first thread. This dll copies no file, loads
 * nothing and injects nothing.
 *
 * The two sides find each other through INDOMITUS_SERVER_SYNC, a name the launcher makes up for every
 * game start and passes in the environment: events "Local\<name>-req" / "-ack" and the shared block
 * "Local\<name>-map" (struct HsSync). They are opened for each server start and closed again.
 * If the launcher is not there (no such objects, launcher closed, no answer within WaitSeconds) the
 * server is simply started as the game asked; the launcher's process-list watch is then the backup.
 *
 * Inert in the editor and in the server itself.
 *
 * Build: build.bat      Offline test: build.bat test (on Windows it also runs the real handshake
 * against a stand-in launcher thread).
 */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <wchar.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#define HS_VERSION   "0.1"
#define HS_MAGIC     0x53484749u            /* "IGHS" */
#define SERVER_EXE   L"call_to_arms_server"

/* the shared block; the launcher has the same struct (ServerSync in launcher\main.cpp) */
typedef struct {
    uint32_t magic;          /* HS_MAGIC, written by the launcher */
    uint32_t launcher_pid;   /* so that the wait ends when the launcher goes away */
    uint32_t seq;            /* dll: counts its requests */
    uint32_t server_pid;     /* dll: the frozen server */
    uint32_t server_tid;     /* dll: its first thread */
    uint32_t ack_seq;        /* launcher: the request it has finished with */
    uint32_t result;         /* launcher: 1 = payload queued, 0 = not */
    uint32_t reserved;
} HsSync;

/* ---------------------------------------------------------------- is this call starting the server? */
static wchar_t lower_w(wchar_t c) { return (c >= L'A' && c <= L'Z') ? (wchar_t)(c + 32) : c; }

/* the file name part of s[0..n) is call_to_arms_server or call_to_arms_server.exe, in any case */
static int base_is_server(const wchar_t *s, size_t n)
{
    size_t b = n;
    while (b > 0 && s[b - 1] != L'\\' && s[b - 1] != L'/' && s[b - 1] != L':') b--;
    const wchar_t *want = SERVER_EXE;
    size_t i = b, k = 0;
    for (; want[k]; k++, i++) if (i >= n || lower_w(s[i]) != want[k]) return 0;
    if (i == n) return 1;
    static const wchar_t ext[] = L".exe";
    for (k = 0; ext[k]; k++, i++) if (i >= n || lower_w(s[i]) != ext[k]) return 0;
    return i == n;
}

/* CreateProcessW's own rule: the application name if there is one, else the first token of the
 * command line (quoted, or up to the first blank). A wrong "yes" costs nothing: the launcher looks at
 * the real image of the process before it does anything. */
static int names_server(const wchar_t *app, const wchar_t *cmd)
{
    if (app && *app) return base_is_server(app, wcslen(app));
    if (!cmd) return 0;
    const wchar_t *s = cmd, *e;
    while (*s == L' ' || *s == L'\t') s++;
    if (*s == L'"') { s++; for (e = s; *e && *e != L'"'; e++) {} }
    else for (e = s; *e && *e != L' ' && *e != L'\t'; e++) {}
    return base_is_server(s, (size_t)(e - s));
}

/* the name from the environment: 8..64 of [0-9A-Za-z_-], nothing else gets near an object name */
static int sync_name_ok(const wchar_t *s)
{
    size_t n = 0;
    for (; s[n]; n++) {
        wchar_t c = s[n];
        if (!((c >= L'0' && c <= L'9') || (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') || c == L'_' || c == L'-')) return 0;
        if (n >= 64) return 0;
    }
    return n >= 8;
}

/* ---------------------------------------------------------------- PE: an import slot of a loaded 64-bit image */
static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static uint32_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }

static int pe_strlen(const uint8_t *b, uint32_t rva, uint32_t img)
{
    for (uint32_t n = 0; n < 256 && rva + n < img; n++) if (!b[rva + n]) return (int)n;
    return -1;
}

/* the import slot (IAT entry) for `name`, imported by name from any dll; NULL if the image has none.
 * dll_out (optional, 64 chars) gets the dll the import names. */
static uint64_t *pe_import_slot(uint8_t *b, const char *name, char *dll_out)
{
    if (!b || b[0] != 'M' || b[1] != 'Z') return NULL;
    uint32_t lfa = rd32(b + 0x3C);
    if (lfa > 0x1000) return NULL;
    const uint8_t *nt = b + lfa;
    if (rd32(nt) != 0x4550 || rd16(nt + 24) != 0x20B) return NULL;                  /* "PE", PE32+ */
    uint32_t img = rd32(nt + 24 + 56);
    if (rd32(nt + 24 + 108) < 2) return NULL;
    uint32_t dir = rd32(nt + 24 + 112 + 8), size = rd32(nt + 24 + 112 + 12);       /* import directory */
    if (!dir || dir >= img || size > img - dir) return NULL;
    for (uint32_t o = dir; o + 20 <= dir + size; o += 20) {
        uint32_t oft = rd32(b + o), nm = rd32(b + o + 12), ft = rd32(b + o + 16);
        if (!nm) break;
        if (nm >= img || pe_strlen(b, nm, img) < 0) continue;
        if (!oft || oft >= img || !ft || ft >= img) continue;                       /* no name table */
        for (uint32_t i = 0; oft + 8 * i + 8 <= img && ft + 8 * i + 8 <= img; i++) {
            uint64_t v = rd64(b + oft + 8 * i);
            if (!v) break;
            if (v >> 63) continue;                                                  /* by ordinal */
            uint32_t h = (uint32_t)v;                                               /* hint word, then the name */
            if (h + 2 >= img || pe_strlen(b, h + 2, img) < 0) continue;
            if (strcmp((const char *)b + h + 2, name)) continue;
            if (dll_out) { size_t k = 0; for (; k < 63 && b[nm + k]; k++) dll_out[k] = (char)b[nm + k]; dll_out[k] = 0; }
            return (uint64_t *)(uintptr_t)(b + ft + 8 * i);
        }
    }
    return NULL;
}

#ifdef _WIN32
/* ================================================================ windows side */
static wchar_t g_logpath[MAX_PATH];
static FILE   *g_log;
static int     cfg_log = 1;
static DWORD   cfg_wait_ms = 30000;
static wchar_t g_sync[80];                  /* "" = the launcher does not wait for a server */

static void logf_(const char *fmt, ...)
{
    if (!cfg_log) return;
    if (!g_log) {
        if (!g_logpath[0]) return;
        g_log = _wfopen(g_logpath, L"w");          /* one file per run */
        if (!g_log) return;
    }
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(g_log, "[%02u:%02u:%02u.%03u] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list ap; va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

typedef struct { wchar_t *b; size_t cap, n; } WBuf;
static void w_str(WBuf *w, const wchar_t *s) { while (*s && w->n + 1 < w->cap) w->b[w->n++] = *s++; w->b[w->n] = 0; }

/* ---------------------------------------------------------------- the link to the launcher, for one server start */
typedef struct { HANDLE req, ack, map, launcher; volatile HsSync *view; } Link;

static void link_close(Link *l)
{
    if (l->view) UnmapViewOfFile((LPCVOID)l->view);
    if (l->map) CloseHandle(l->map);
    if (l->req) CloseHandle(l->req);
    if (l->ack) CloseHandle(l->ack);
    if (l->launcher) CloseHandle(l->launcher);
    memset(l, 0, sizeof *l);
}

/* 1 = the launcher's objects are there. *why says what was missing otherwise. */
static int link_open(Link *l, const char **why)
{
    memset(l, 0, sizeof *l);
    *why = "the launcher did not pass INDOMITUS_SERVER_SYNC";
    if (!g_sync[0]) return 0;
    wchar_t nm[120];
    { WBuf w = { nm, 120, 0 }; nm[0] = 0; w_str(&w, L"Local\\"); w_str(&w, g_sync); w_str(&w, L"-req"); }
    l->req = OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, nm);
    { WBuf w = { nm, 120, 0 }; nm[0] = 0; w_str(&w, L"Local\\"); w_str(&w, g_sync); w_str(&w, L"-ack"); }
    l->ack = OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, nm);
    { WBuf w = { nm, 120, 0 }; nm[0] = 0; w_str(&w, L"Local\\"); w_str(&w, g_sync); w_str(&w, L"-map"); }
    l->map = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, nm);
    *why = "the launcher is not waiting for a server (closed, or started with no-wait)";
    if (!l->req || !l->ack || !l->map) { link_close(l); return 0; }
    l->view = (volatile HsSync *)MapViewOfFile(l->map, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(HsSync));
    *why = "the launcher's shared block could not be read";
    if (!l->view || l->view->magic != HS_MAGIC) { link_close(l); return 0; }
    if (l->view->launcher_pid) l->launcher = OpenProcess(SYNCHRONIZE, FALSE, l->view->launcher_pid);   /* NULL is fine */
    return 1;
}

/* ---------------------------------------------------------------- the hook */
typedef BOOL (WINAPI *CreateProcessW_t)(LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD,
                                        LPVOID, LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION);
static CreateProcessW_t  g_orig;
static CRITICAL_SECTION  g_lock;            /* one server start at a time */

static BOOL WINAPI hook_CreateProcessW(LPCWSTR app, LPWSTR cmd, LPSECURITY_ATTRIBUTES pa, LPSECURITY_ATTRIBUTES ta, BOOL inherit,
                                       DWORD flags, LPVOID env, LPCWSTR cwd, LPSTARTUPINFOW si, LPPROCESS_INFORMATION pi)
{
    if (!pi || !names_server(app, cmd)) return g_orig(app, cmd, pa, ta, inherit, flags, env, cwd, si, pi);

    EnterCriticalSection(&g_lock);
    Link l; const char *why = "";
    int linked = link_open(&l, &why);
    BOOL ok = g_orig(app, cmd, pa, ta, inherit, linked ? (flags | CREATE_SUSPENDED) : flags, env, cwd, si, pi);
    DWORD err = GetLastError();
    if (!ok) {
        logf_("the game's start of the host server failed (error %lu) - nothing to do", err);
    } else if (!linked) {
        logf_("host server pid %lu started as the game asked: %s. The launcher's process-list watch is the backup.",
              pi->dwProcessId, why);
    } else {
        uint32_t seq = l.view->seq + 1;
        l.view->server_pid = pi->dwProcessId;
        l.view->server_tid = pi->dwThreadId;
        l.view->result = 0;
        l.view->seq = seq;
        ResetEvent(l.ack);
        SetEvent(l.req);
        logf_("host server pid %lu created frozen (thread %lu) - waiting for the launcher to load the payload", pi->dwProcessId, pi->dwThreadId);
        HANDLE hs[2] = { l.ack, l.launcher };
        DWORD t0 = GetTickCount(), w = WAIT_TIMEOUT;
        for (;;) {
            DWORD gone = GetTickCount() - t0;
            if (gone >= cfg_wait_ms) { w = WAIT_TIMEOUT; break; }
            w = WaitForMultipleObjects(l.launcher ? 2 : 1, hs, FALSE, cfg_wait_ms - gone);
            if (w == WAIT_OBJECT_0 && l.view->ack_seq != seq) continue;     /* an answer to an older request */
            break;
        }
        DWORD took = GetTickCount() - t0;
        uint32_t result = l.view->result;
        if (!(flags & CREATE_SUSPENDED)) ResumeThread(pi->hThread);
        if (w == WAIT_OBJECT_0 && result)
            logf_("launcher answered after %lu ms: payload queued - server resumed, the dlls load before its first instruction", took);
        else if (w == WAIT_OBJECT_0)
            logf_("!!! launcher answered after %lu ms: it did NOT load the payload (see gem_launcher.log) - server resumed without it", took);
        else if (w == WAIT_OBJECT_0 + 1)
            logf_("!!! the launcher closed while the server waited (%lu ms) - server resumed without the payload", took);
        else
            logf_("!!! no answer from the launcher within %lu ms - server resumed; its process-list watch may still load the payload late", took);
    }
    link_close(&l);
    LeaveCriticalSection(&g_lock);
    SetLastError(err);
    return ok;
}

#ifndef HS_TEST
/* ---------------------------------------------------------------- install */
static void install(void)
{
    uint8_t *exe = (uint8_t *)GetModuleHandleW(NULL);
    char dll[64] = "";
    uint64_t *slot = pe_import_slot(exe, "CreateProcessW", dll);
    if (!slot) { logf_("OFF: call_to_arms.exe does not import CreateProcessW by name. Nothing hooked."); return; }

    InitializeCriticalSection(&g_lock);
    HMODULE self;
    int pinned = GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                                    (LPCWSTR)(void *)&hook_CreateProcessW, &self) != 0;
    g_orig = (CreateProcessW_t)(uintptr_t)*slot;
    void *real = (void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "CreateProcessW");
    DWORD old;
    if (!VirtualProtect(slot, 8, PAGE_READWRITE, &old)) {
        logf_("OFF: VirtualProtect on the import slot failed (%lu). Nothing hooked.", GetLastError());
        return;
    }
    *slot = (uint64_t)(uintptr_t)&hook_CreateProcessW;       /* one aligned pointer: seen whole by any thread */
    VirtualProtect(slot, 8, old, &old);
    logf_("ON: import slot exe+0x%llX (%s!CreateProcessW) -> this dll%s%s",
          (unsigned long long)((uint8_t *)slot - exe), dll,
          (void *)g_orig == real ? "" : "; the slot already led somewhere else than kernel32 (another hook, kept in the chain)",
          pinned ? "" : " (could not pin the dll)");
    logf_("a start of %ls.exe will be frozen until the launcher has loaded the payload (WaitSeconds=%lu)", SERVER_EXE, cfg_wait_ms / 1000);
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
    if (_wcsicmp(nm, L"call_to_arms.exe") != 0) return TRUE;      /* the server itself, the editor, tools: inert */

    wchar_t path[MAX_PATH], ini[MAX_PATH];
    DWORD n = GetModuleFileNameW(inst, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH - 8) return TRUE;
    wchar_t *dot = wcsrchr(path, L'.');
    wchar_t *sl = wcsrchr(path, L'\\');
    if (dot && (!sl || dot > sl)) *dot = 0;
    { WBuf w = { ini, MAX_PATH, 0 }; ini[0] = 0; w_str(&w, path); w_str(&w, L".ini"); }
    { WBuf w = { g_logpath, MAX_PATH, 0 }; g_logpath[0] = 0; w_str(&w, path); w_str(&w, L".log"); }

    cfg_log = GetPrivateProfileIntW(L"HostServer", L"Log", 1, ini) != 0;
    UINT ws = GetPrivateProfileIntW(L"HostServer", L"WaitSeconds", 30, ini);
    if (ws < 2) ws = 2;
    if (ws > 300) ws = 300;
    cfg_wait_ms = ws * 1000;
    int enabled = GetPrivateProfileIntW(L"HostServer", L"Enabled", 1, ini) != 0;

    wchar_t sync[200] = L"";
    DWORD sn = GetEnvironmentVariableW(L"INDOMITUS_SERVER_SYNC", sync, 200);
    if (sn == 0 || sn >= 200 || !sync_name_ok(sync)) sync[0] = 0;
    { WBuf w = { g_sync, 80, 0 }; g_sync[0] = 0; w_str(&w, sync); }

    logf_("[GEM HostServer %s] call_to_arms.exe | WaitSeconds=%lu | launcher link %s", HS_VERSION, cfg_wait_ms / 1000,
          g_sync[0] ? "announced (INDOMITUS_SERVER_SYNC)" : "NOT announced");
    if (!enabled) { logf_("Enabled=0 - nothing hooked"); return TRUE; }
    if (!g_sync[0]) {
        logf_("OFF: INDOMITUS_SERVER_SYNC is not set - this launcher does not take a frozen server "
              "(older launcher, or started with no-wait). Nothing hooked.");
        return TRUE;
    }

    /* one copy per process */
    static const wchar_t hex[] = L"0123456789abcdef";
    wchar_t mx[64]; WBuf w = { mx, 64, 0 }; mx[0] = 0;
    w_str(&w, L"Local\\gem_hostserver_pid_");
    DWORD pid = GetCurrentProcessId();
    for (int i = 28; i >= 0; i -= 4) mx[w.n++] = hex[(pid >> i) & 15];
    mx[w.n] = 0;
    HANDLE h = CreateMutexW(NULL, FALSE, mx);      /* kept open for the process lifetime */
    if (h && GetLastError() == ERROR_ALREADY_EXISTS) {
        logf_("another copy is already loaded in this process - this one stays off");
        CloseHandle(h);
        return TRUE;
    }
    install();
    return TRUE;
}
#endif /* !HS_TEST */
#endif /* _WIN32 */

#ifdef HS_TEST
#include "../test/hs_test.inc"
#endif
