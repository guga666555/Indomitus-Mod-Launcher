/*
 * GEM ChildInject 0.1 - Indomitus Launcher payload dll
 *
 * Hosting a multiplayer game makes call_to_arms.exe start call_to_arms_server.exe
 * (-owner <game pid> -pipe "cta_ipc_0" -mod ...). The launcher only injects into the process it
 * creates, so the host server ran with no payload dlls and died at script load on ScriptExt words
 * ({if health} -> "unknown condition", eIA_If.cpp:1200).
 *
 * This dll carries the payload into that server:
 *   - in the game (any call_to_arms*.exe except the server and the workshop tool) it hooks
 *     kernelbase!CreateProcessInternalW, which every CreateProcess* / ShellExecute* path ends in
 *   - when the new image is call_to_arms_server.exe (file name, case-insensitive) it adds
 *     CREATE_SUSPENDED, copies every *.dll / *.ini of its own folder into <its folder>\server\
 *     (so the server's dlls read the same inis but write their own logs instead of truncating the
 *     game's), queues LoadLibraryW APCs for those dlls in alphabetical order (same early-bird
 *     injection and order as the launcher), then resumes the thread (unless the caller asked for a
 *     suspended process)
 *   - a worker thread then checks the server's module list for 15 s and logs each dll as
 *     verified or NOT LOADED, then logs the server's exit code
 *   - inside the server this dll only logs that it is there (no hook)
 *
 * Log: gem_aa_childinject.log next to the dll (game) and in server\ (server), appended, one
 * line per event with the pid.
 *
 * Name starts with gem_aa_ so it sorts first in the payload (launcher injects alphabetically).
 * Build: build.bat (zig cc, same flags as the other payload dlls).
 */
#include <windows.h>
#define PSAPI_VERSION 2
#include <psapi.h>
#include <stdio.h>
#include <stdarg.h>
#include <wchar.h>
#include <stdint.h>
#include <string.h>

#define CI_VERSION "0.1"
#define CI_LOG     L"gem_aa_childinject.log"
#define CI_SUBDIR  L"server"
#define CI_TARGET  L"call_to_arms_server.exe"
#define CI_MAXDLL  64

static wchar_t g_dir[MAX_PATH];      /* folder this dll was loaded from, no trailing slash */
static wchar_t g_self[MAX_PATH];     /* file name of this dll */
static DWORD   g_pid;
static SRWLOCK g_loglock = SRWLOCK_INIT;

/* ------------------------------------------------------------------ log */

static void logf_(const char *fmt, ...)
{
    wchar_t lp[MAX_PATH + 64];
    _snwprintf(lp, MAX_PATH + 63, L"%ls\\%ls", g_dir, CI_LOG); lp[MAX_PATH + 63] = 0;
    AcquireSRWLockExclusive(&g_loglock);
    FILE *f = _wfopen(lp, L"a");
    if (f) {
        SYSTEMTIME t; GetLocalTime(&t);
        fprintf(f, "[%04u-%02u-%02u %02u:%02u:%02u.%03u pid %lu] ", t.wYear, t.wMonth, t.wDay,
                t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, (unsigned long)g_pid);
        va_list a; va_start(a, fmt); vfprintf(f, fmt, a); va_end(a);
        fputc('\n', f);
        fclose(f);
    }
    ReleaseSRWLockExclusive(&g_loglock);
}

/* ------------------------------------------------------------------ names */

static const wchar_t *base_name(const wchar_t *p)
{
    const wchar_t *b = p;
    for (; *p; ++p) if (*p == L'\\' || *p == L'/') b = p + 1;
    return b;
}

/* file name of the image a CreateProcess call starts: lpApplicationName, else the first token of
   the command line (quoted or up to the first space) */
static int is_target(LPCWSTR app, LPCWSTR cmd)
{
    wchar_t buf[MAX_PATH * 2];
    if (app && *app) {
        wcsncpy(buf, app, MAX_PATH * 2 - 1); buf[MAX_PATH * 2 - 1] = 0;
    } else if (cmd && *cmd) {
        const wchar_t *s = cmd; size_t n = 0;
        while (*s == L' ' || *s == L'\t') ++s;
        if (*s == L'"') { ++s; while (s[n] && s[n] != L'"' && n < MAX_PATH * 2 - 1) ++n; }
        else            { while (s[n] && s[n] != L' ' && s[n] != L'\t' && n < MAX_PATH * 2 - 1) ++n; }
        wcsncpy(buf, s, n); buf[n] = 0;
    } else return 0;
    const wchar_t *b = base_name(buf);
    if (!_wcsicmp(b, CI_TARGET)) return 1;
    if (!_wcsicmp(b, L"call_to_arms_server")) return 1;     /* command line without the extension */
    return 0;
}

/* ------------------------------------------------------------------ payload copy */

typedef struct { wchar_t path[CI_MAXDLL][MAX_PATH]; int n; } dll_list;

static int same_file(const wchar_t *a, const wchar_t *b)
{
    HANDLE ha = CreateFileW(a, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    HANDLE hb = CreateFileW(b, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    int same = 0;
    if (ha != INVALID_HANDLE_VALUE && hb != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER sa, sb;
        if (GetFileSizeEx(ha, &sa) && GetFileSizeEx(hb, &sb) && sa.QuadPart == sb.QuadPart) {
            static char x[65536], y[65536];   /* only used under the hook's serial path */
            same = 1;
            for (;;) {
                DWORD ra = 0, rb = 0;
                if (!ReadFile(ha, x, sizeof x, &ra, NULL) || !ReadFile(hb, y, sizeof y, &rb, NULL) || ra != rb) { same = 0; break; }
                if (!ra) break;
                if (memcmp(x, y, ra)) { same = 0; break; }
            }
        }
    }
    if (ha != INVALID_HANDLE_VALUE) CloseHandle(ha);
    if (hb != INVALID_HANDLE_VALUE) CloseHandle(hb);
    return same;
}

static int is_ext(const wchar_t *name, const wchar_t *ext)
{
    size_t n = wcslen(name), e = wcslen(ext);
    return n > e && !_wcsicmp(name + n - e, ext);
}

/* copies *.dll / *.ini of g_dir into dst; a file another server still has loaded can't be
   overwritten, which is fine when it is already identical. Returns 0 when a file is locked AND
   different (caller then tries a fresh folder). */
static int copy_payload(const wchar_t *dst)
{
    CreateDirectoryW(dst, NULL);
    wchar_t pat[MAX_PATH + 8];
    _snwprintf(pat, MAX_PATH + 7, L"%ls\\*", g_dir); pat[MAX_PATH + 7] = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) { logf_("cannot list %ls (%lu)", g_dir, GetLastError()); return 0; }
    int ok = 1;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (!is_ext(fd.cFileName, L".dll") && !is_ext(fd.cFileName, L".ini")) continue;
        wchar_t s[MAX_PATH], d[MAX_PATH];
        _snwprintf(s, MAX_PATH - 1, L"%ls\\%ls", g_dir, fd.cFileName); s[MAX_PATH - 1] = 0;
        _snwprintf(d, MAX_PATH - 1, L"%ls\\%ls", dst, fd.cFileName); d[MAX_PATH - 1] = 0;
        if (CopyFileW(s, d, FALSE)) continue;
        DWORD e = GetLastError();
        if (same_file(s, d)) { logf_("  %ls in use, already identical (copy error %lu)", fd.cFileName, e); continue; }
        logf_("  could not copy %ls -> %ls (error %lu)", fd.cFileName, dst, e);
        ok = 0;
    } while (ok && FindNextFileW(h, &fd));
    FindClose(h);
    return ok;
}

static void list_dlls(const wchar_t *dir, dll_list *out)
{
    out->n = 0;
    wchar_t pat[MAX_PATH + 8];
    _snwprintf(pat, MAX_PATH + 7, L"%ls\\*.dll", dir); pat[MAX_PATH + 7] = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (!is_ext(fd.cFileName, L".dll")) continue;
        if (out->n >= CI_MAXDLL) { logf_("more than %d dlls, the rest are skipped", CI_MAXDLL); break; }
        _snwprintf(out->path[out->n], MAX_PATH - 1, L"%ls\\%ls", dir, fd.cFileName);
        out->path[out->n][MAX_PATH - 1] = 0;
        out->n++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    /* same order as the launcher: case-insensitive by file name */
    for (int i = 1; i < out->n; ++i)
        for (int j = i; j > 0 && _wcsicmp(base_name(out->path[j]), base_name(out->path[j - 1])) < 0; --j) {
            wchar_t t[MAX_PATH];
            memcpy(t, out->path[j], sizeof t); memcpy(out->path[j], out->path[j - 1], sizeof t); memcpy(out->path[j - 1], t, sizeof t);
        }
}

/* ------------------------------------------------------------------ injection */

static int queue_dlls(HANDLE proc, HANDLE thr, const dll_list *dl)
{
    PAPCFUNC ll = (PAPCFUNC)(void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    if (!ll) { logf_("LoadLibraryW not found"); return 0; }
    for (int i = 0; i < dl->n; ++i) {
        SIZE_T bytes = (wcslen(dl->path[i]) + 1) * sizeof(wchar_t), wr = 0;
        void *mem = VirtualAllocEx(proc, NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!mem) { logf_("VirtualAllocEx failed (%lu)", GetLastError()); return 0; }
        if (!WriteProcessMemory(proc, mem, dl->path[i], bytes, &wr) || wr != bytes) {
            logf_("WriteProcessMemory failed (%lu)", GetLastError()); return 0;
        }
        if (!QueueUserAPC(ll, thr, (ULONG_PTR)mem)) { logf_("QueueUserAPC failed (%lu)", GetLastError()); return 0; }
        logf_("  queued %ls", base_name(dl->path[i]));
    }
    return 1;
}

typedef struct { HANDLE proc; DWORD pid; int susp; dll_list dl; } watch_t;

static int module_loaded(HANDLE proc, const wchar_t *path)
{
    HMODULE mods[1024]; DWORD need = 0;
    if (!K32EnumProcessModulesEx(proc, mods, sizeof mods, &need, LIST_MODULES_ALL)) return 0;
    DWORD n = (need < sizeof mods ? need : (DWORD)sizeof mods) / sizeof(HMODULE);
    for (DWORD i = 0; i < n; ++i) {
        wchar_t p[MAX_PATH];
        if (K32GetModuleFileNameExW(proc, mods[i], p, MAX_PATH) && !_wcsicmp(p, path)) return 1;
    }
    return 0;
}

static DWORD WINAPI watch_thread(void *vp)
{
    watch_t *w = (watch_t *)vp;
    int limit = w->susp ? 600 : 150;            /* 100 ms steps: 15 s, 60 s if the caller resumes it */
    int all = 0; DWORD code = 0;
    for (int i = 0; i < limit && !all; ++i) {
        Sleep(100);
        if (GetExitCodeProcess(w->proc, &code) && code != STILL_ACTIVE) break;
        all = 1;
        for (int k = 0; k < w->dl.n; ++k) if (!module_loaded(w->proc, w->dl.path[k])) { all = 0; break; }
    }
    if (GetExitCodeProcess(w->proc, &code) && code != STILL_ACTIVE) {
        logf_("server pid %lu exited (code 0x%08lX) before every dll was checked", (unsigned long)w->pid, (unsigned long)code);
    } else {
        int missing = 0;
        for (int k = 0; k < w->dl.n; ++k) {
            if (module_loaded(w->proc, w->dl.path[k])) logf_("verified: %ls is loaded in server pid %lu", base_name(w->dl.path[k]), (unsigned long)w->pid);
            else { logf_("!!!!! %ls did NOT load in server pid %lu !!!!!", base_name(w->dl.path[k]), (unsigned long)w->pid); missing++; }
        }
        if (missing) logf_("!!!!! %d of %d payload dlls missing in the server - the host will run without them !!!!!", missing, w->dl.n);
        else         logf_("all %d payload dlls loaded in server pid %lu", w->dl.n, (unsigned long)w->pid);
        WaitForSingleObject(w->proc, INFINITE);
        GetExitCodeProcess(w->proc, &code);
        logf_("server pid %lu exited (code 0x%08lX)", (unsigned long)w->pid, (unsigned long)code);
    }
    CloseHandle(w->proc);
    HeapFree(GetProcessHeap(), 0, w);
    return 0;
}

/* ------------------------------------------------------------------ hook */

typedef BOOL (WINAPI *cpiw_t)(HANDLE, LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL,
                              DWORD, LPVOID, LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION, PHANDLE);
static cpiw_t g_orig;
static SRWLOCK g_hooklock = SRWLOCK_INIT;

static BOOL WINAPI hook_cpiw(HANDLE tok, LPCWSTR app, LPWSTR cmd, LPSECURITY_ATTRIBUTES pa, LPSECURITY_ATTRIBUTES ta,
                             BOOL inh, DWORD flags, LPVOID env, LPCWSTR cwd, LPSTARTUPINFOW si,
                             LPPROCESS_INFORMATION pi, PHANDLE ntok)
{
    if (!pi || !is_target(app, cmd)) return g_orig(tok, app, cmd, pa, ta, inh, flags, env, cwd, si, pi, ntok);

    AcquireSRWLockExclusive(&g_hooklock);
    int susp = (flags & CREATE_SUSPENDED) != 0;
    logf_("server start: app=%ls cmd=%ls flags=0x%08lX%s", app ? app : L"(null)", cmd ? cmd : L"(null)",
          (unsigned long)flags, susp ? " (caller asked for suspended)" : "");

    /* the server's own copy of the payload */
    wchar_t dst[MAX_PATH];
    _snwprintf(dst, MAX_PATH - 1, L"%ls\\%ls", g_dir, CI_SUBDIR); dst[MAX_PATH - 1] = 0;
    if (!copy_payload(dst)) {
        _snwprintf(dst, MAX_PATH - 1, L"%ls\\%ls_%lu", g_dir, CI_SUBDIR, (unsigned long)GetTickCount()); dst[MAX_PATH - 1] = 0;
        logf_("  using a fresh folder %ls", dst);
        if (!copy_payload(dst)) logf_("!!!!! payload copy failed again - injecting what is there !!!!!");
    }
    watch_t *w = (watch_t *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *w);
    if (w) list_dlls(dst, &w->dl);
    if (!w || !w->dl.n) {
        logf_("!!!!! no dlls in %ls - server starts WITHOUT the payload !!!!!", dst);
        if (w) HeapFree(GetProcessHeap(), 0, w);
        ReleaseSRWLockExclusive(&g_hooklock);
        return g_orig(tok, app, cmd, pa, ta, inh, flags, env, cwd, si, pi, ntok);
    }

    BOOL ok = g_orig(tok, app, cmd, pa, ta, inh, flags | CREATE_SUSPENDED, env, cwd, si, pi, ntok);
    if (!ok) {
        DWORD e = GetLastError();
        logf_("CreateProcess failed (%lu)", e);
        HeapFree(GetProcessHeap(), 0, w);
        ReleaseSRWLockExclusive(&g_hooklock);
        SetLastError(e);
        return ok;
    }
    logf_("server pid %lu created suspended, %d dll(s) from %ls", (unsigned long)pi->dwProcessId, w->dl.n, dst);
    int q = queue_dlls(pi->hProcess, pi->hThread, &w->dl);
    if (!q) logf_("!!!!! injection failed - server pid %lu starts without (all of) the payload !!!!!", (unsigned long)pi->dwProcessId);
    if (!susp) { ResumeThread(pi->hThread); logf_("server main thread resumed"); }
    else logf_("left suspended for the caller");

    w->pid = pi->dwProcessId; w->susp = susp;
    if (DuplicateHandle(GetCurrentProcess(), pi->hProcess, GetCurrentProcess(), &w->proc, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
        HANDLE t = CreateThread(NULL, 0, watch_thread, w, 0, NULL);
        if (t) CloseHandle(t); else { CloseHandle(w->proc); HeapFree(GetProcessHeap(), 0, w); }
    } else HeapFree(GetProcessHeap(), 0, w);
    ReleaseSRWLockExclusive(&g_hooklock);
    SetLastError(ERROR_SUCCESS);
    return ok;
}

/* Length of the instructions at p (x64 function prologues only). Returns 0 on anything it does
   not know or that can't be moved (relative branches, RIP-relative operands). */
static int insn_len(const uint8_t *p)
{
    int n = 0, rexw = 0;
    if (p[0] == 0x66) n++;                                          /* operand-size prefix */
    if ((p[n] & 0xF0) == 0x40) { rexw = p[n] & 8; n++; }            /* REX */
    uint8_t op = p[n++];
    if (op >= 0x50 && op <= 0x5F) return n;                         /* push / pop r */
    if (op == 0x90 || op == 0xCC) return n;                         /* nop / int3 padding */
    if (op >= 0xB8 && op <= 0xBF) return n + (rexw ? 8 : 4);        /* mov r, imm */
    int imm = -1;
    switch (op) {
    case 0x89: case 0x8B: case 0x8D: case 0x31: case 0x33: case 0x85: case 0x29: case 0x2B: case 0x01: case 0x03:
        imm = 0; break;
    case 0x83: case 0xC0: case 0xC1: imm = 1; break;
    case 0x81: case 0xC7: imm = 4; break;
    default: return 0;
    }
    uint8_t m = p[n++], mod = m >> 6, rm = m & 7;
    if (mod != 3 && rm == 4) n++;                                   /* SIB */
    if (mod == 0 && rm == 5) return 0;                              /* RIP-relative: not movable */
    if (mod == 1) n += 1; else if (mod == 2) n += 4;
    return n + imm;
}

static int install_hook(void)
{
    HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
    uint8_t *fn = kb ? (uint8_t *)GetProcAddress(kb, "CreateProcessInternalW") : NULL;
    if (!fn) { logf_("!!!!! kernelbase!CreateProcessInternalW not found - no server injection !!!!!"); return 0; }

    uint8_t *tr = (uint8_t *)VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tr) { logf_("!!!!! VirtualAlloc failed (%lu) !!!!!", GetLastError()); return 0; }

    int len = 0;
    if (fn[0] == 0xE9) {                                            /* already a jmp rel32 (another hook) */
        uint8_t *tgt = fn + 5 + *(int32_t *)(fn + 1);
        tr[0] = 0xFF; tr[1] = 0x25; memset(tr + 2, 0, 4); memcpy(tr + 6, &tgt, 8);
        len = 5;
    } else if (fn[0] == 0xFF && fn[1] == 0x25 && *(int32_t *)(fn + 2) == 0) {   /* jmp [rip+0] abs */
        memcpy(tr, fn, 14);
        len = 14;
    } else {
        while (len < 14) {
            int l = insn_len(fn + len);
            if (!l) {
                logf_("!!!!! unknown prologue byte %02X at +%d of CreateProcessInternalW - no server injection !!!!!", fn[len], len);
                char hex[3 * 24 + 1]; for (int i = 0; i < 24; ++i) sprintf(hex + 3 * i, "%02X ", fn[i]);
                logf_("  prologue: %s", hex);
                VirtualFree(tr, 0, MEM_RELEASE);
                return 0;
            }
            len += l;
        }
        memcpy(tr, fn, len);
        uint8_t *back = fn + len;
        tr[len] = 0xFF; tr[len + 1] = 0x25; memset(tr + len + 2, 0, 4); memcpy(tr + len + 6, &back, 8);
    }
    g_orig = (cpiw_t)(void *)tr;

    DWORD old;
    if (!VirtualProtect(fn, len < 14 ? 14 : len, PAGE_EXECUTE_READWRITE, &old)) {
        logf_("!!!!! VirtualProtect failed (%lu) !!!!!", GetLastError()); return 0;
    }
    void *h = (void *)hook_cpiw;
    uint8_t patch[32]; int plen = len < 14 ? 14 : len;
    patch[0] = 0xFF; patch[1] = 0x25; memset(patch + 2, 0, 4); memcpy(patch + 6, &h, 8);
    for (int i = 14; i < plen; ++i) patch[i] = 0x90;
    memcpy(fn, patch, plen);
    VirtualProtect(fn, plen, old, &old);
    FlushInstructionCache(GetCurrentProcess(), fn, plen);
    logf_("hooked kernelbase!CreateProcessInternalW at %p (%d bytes moved)", (void *)fn, len);
    return 1;
}

/* ------------------------------------------------------------------ entry */

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID res)
{
    (void)res;
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(inst);
    g_pid = GetCurrentProcessId();
    if (!GetModuleFileNameW(inst, g_dir, MAX_PATH)) return TRUE;
    wcscpy(g_self, base_name(g_dir));
    *(wchar_t *)base_name(g_dir) = 0;
    size_t dl = wcslen(g_dir);
    if (dl && (g_dir[dl - 1] == L'\\' || g_dir[dl - 1] == L'/')) g_dir[dl - 1] = 0;

    wchar_t exe[MAX_PATH];
    if (!GetModuleFileNameW(NULL, exe, MAX_PATH)) return TRUE;
    const wchar_t *en = base_name(exe);
    if (_wcsnicmp(en, L"call_to_arms", 12)) return TRUE;           /* only the game exes */
    if (!_wcsnicmp(en, L"call_to_arms_workshop", 21)) return TRUE;

    if (!_wcsicmp(en, CI_TARGET)) {
        logf_("GEM ChildInject " CI_VERSION " in %ls (server): payload folder %ls, no hook here", en, g_dir);
        return TRUE;
    }
    logf_("GEM ChildInject " CI_VERSION " in %ls: payload folder %ls", en, g_dir);
    install_hook();
    return TRUE;
}
