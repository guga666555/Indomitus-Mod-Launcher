/* GEM ScriptExt - crash log
 * Part of the unity build: #included by gem_scriptext.c, not compiled on its own.
 * First-chance fatal exceptions with a stack scan, and who ends the process (ExitProcess /
 * TerminateProcess IAT hooks).
 */

#ifndef SX_TEST
/* ------------------------------------------------------------------------------------ */
/* crash logger: first-chance fatal exceptions -> gem_scriptext.log (does not handle them)  */
/* ------------------------------------------------------------------------------------ */
static HMODULE g_self; static volatile LONG g_vehCount;

static void addr_desc(uint64_t a, char *out, size_t n)
{
    HMODULE m = NULL; wchar_t path[MAX_PATH] = L"";
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)(uintptr_t)a, &m) && m) {
        GetModuleFileNameW(m, path, MAX_PATH);
        wchar_t *b = wcsrchr(path, L'\\'); b = b ? b + 1 : path;
        snprintf(out, n, "%ls+0x%llX", b, (unsigned long long)(a - (uint64_t)(uintptr_t)m));
    } else snprintf(out, n, "0x%llX (no module)", (unsigned long long)a);
}

/* stack scan: up to max values on the stack that point into the exe's code or into this DLL (likely return addresses),
   with their offset from rsp; returns how many were found */
static int stack_frames(uint64_t rsp, uint64_t *addr, int *off, int max)
{
    const uint64_t *sp = (const uint64_t *)(uintptr_t)rsp; int n = 0;
    for (int i = 0; i < 0x400 && n < max; i++) {
        uint64_t v;
        if (!rd_mem(sp + i, &v, 8)) break;
        HMODULE m = NULL;
        if (!in_text((uint8_t *)(uintptr_t)v) &&
            !(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)(uintptr_t)v, &m) && m == g_self))
            continue;
        addr[n] = v; if (off) off[n] = i * 8; n++;
    }
    return n;
}
static void log_stack(uint64_t rsp, int max)
{
    uint64_t a[32]; int o[32];
    int n = stack_frames(rsp, a, o, max < 32 ? max : 32);
    for (int i = 0; i < n; i++) { char d[160]; addr_desc(a[i], d, sizeof d); logf_("  [rsp+0x%03X] %s", o[i], d); }
}

/* C++ throws: the engine throws and catches some on its own (the same ones at every start), so each distinct call
   chain (first 3 code addresses on the stack) is logged once, on one line, up to 8 chains. Only the first 64 throws are
   looked at: the stack scan costs a few system calls, and a game that throws all the time must not pay for it. */
static void log_cxx_throw(uint64_t rsp)
{
    static uint64_t seen[8][3]; static volatile LONG nseen, total;
    if (nseen >= 8 || InterlockedIncrement(&total) > 64) return;
    uint64_t a[3] = { 0, 0, 0 };
    int n = stack_frames(rsp, a, NULL, 3);
    LONG k = nseen < 8 ? nseen : 8;
    for (LONG i = 0; i < k; i++) if (!memcmp(seen[i], a, sizeof a)) return;
    LONG slot = InterlockedIncrement(&nseen) - 1;
    if (slot >= 8) return;
    memcpy(seen[slot], a, sizeof a);
    char d[3][160] = { "", "", "" };
    for (int i = 0; i < n; i++) addr_desc(a[i], d[i], sizeof d[i]);
    logf_("C++ exception thrown (thread %lu): %s%s%s%s%s", GetCurrentThreadId(), d[0], n > 1 ? " < " : "", d[1], n > 2 ? " < " : "", d[2]);
    if (slot == 7) logf_("(further C++ exception call chains not logged)");
}

/* ---- who ends the process: IAT hooks on ExitProcess / TerminateProcess (exe + BugSplat) ---- */
typedef void (WINAPI *ExitProcessFn)(UINT);
typedef BOOL (WINAPI *TerminateProcessFn)(HANDLE, UINT);
static ExitProcessFn g_exitProcessOrig; static TerminateProcessFn g_terminateProcessOrig;

static void WINAPI hook_exit_process(UINT code)
{
    char who[160]; addr_desc((uint64_t)(uintptr_t)__builtin_return_address(0), who, sizeof who);
    logf_("ExitProcess(%u) called from %s", code, who);
    log_stack((uint64_t)(uintptr_t)__builtin_frame_address(0), 16);
    g_exitProcessOrig(code);
}
static BOOL WINAPI hook_terminate_process(HANDLE h, UINT code)
{
    if (h == GetCurrentProcess() || GetProcessId(h) == GetCurrentProcessId()) {
        char who[160]; addr_desc((uint64_t)(uintptr_t)__builtin_return_address(0), who, sizeof who);
        logf_("TerminateProcess(self, %u) called from %s", code, who);
        log_stack((uint64_t)(uintptr_t)__builtin_frame_address(0), 16);
    }
    return g_terminateProcessOrig(h, code);
}

/* the module's import slot (IAT entry) for a function imported by name, NULL if it does not import it */
static uint64_t *iat_slot(HMODULE mod, const char *name)
{
    if (!mod) return NULL;
    uint8_t *b = (uint8_t *)mod;
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(b + ((IMAGE_DOS_HEADER *)b)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return NULL;
    for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(b + dir.VirtualAddress); d->Name; d++) {
        if (!d->OriginalFirstThunk) continue;                                /* no name table */
        const uint64_t *names = (const uint64_t *)(b + d->OriginalFirstThunk);
        uint64_t *iat = (uint64_t *)(b + d->FirstThunk);
        for (int i = 0; names[i]; i++)
            if ((int64_t)names[i] >= 0 && !strcmp((const char *)(b + (uint32_t)names[i] + 2), name)) return iat + i;   /* by name (no ordinal flag): hint word, then name */
    }
    return NULL;
}

/* ExitProcess / TerminateProcess imports of the exe and BugSplat. An import that already leads somewhere else than
   kernel32 was hooked by another DLL loaded before this one (the launcher loads them alphabetically, e.g. GEM Fauna);
   that DLL traces the exit then, so it is left alone and named in the log. */
static void install_exit_hooks(void)
{
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    void *ep = (void *)GetProcAddress(k32, "ExitProcess"), *tp = (void *)GetProcAddress(k32, "TerminateProcess");
    g_exitProcessOrig = (ExitProcessFn)ep; g_terminateProcessOrig = (TerminateProcessFn)tp;
    if (!ep || !tp) { logf_("exit trace                     OFF (kernel32 exports not found)"); return; }
    const wchar_t *mods[] = { NULL, L"BugSplat64.dll", L"bdcap64.dll" };
    const char *names[2] = { "ExitProcess", "TerminateProcess" };
    void *real[2] = { ep, tp }, *hooks[2] = { (void *)hook_exit_process, (void *)hook_terminate_process };
    int n = 0; char other[160] = "";
    for (int i = 0; i < 3; i++) {
        HMODULE m = mods[i] ? GetModuleHandleW(mods[i]) : GetModuleHandleW(NULL);
        for (int k = 0; k < 2; k++) {
            uint64_t *slot = iat_slot(m, names[k]);
            if (!slot) continue;
            void *cur = (void *)(uintptr_t)*slot;
            if (cur == real[k]) {
                DWORD old;
                if (VirtualProtect(slot, 8, PAGE_READWRITE, &old)) { *slot = (uint64_t)(uintptr_t)hooks[k]; VirtualProtect(slot, 8, old, &old); n++; }
            } else if (cur != hooks[k] && !other[0]) addr_desc((uint64_t)(uintptr_t)cur, other, sizeof other);
        }
    }
    if (n || !other[0]) logf_("exit trace                     %d import(s) hooked (ExitProcess/TerminateProcess)", n);
    else logf_("exit trace                     done by another DLL loaded earlier (%s)", other);
}

static LONG CALLBACK sx_veh(EXCEPTION_POINTERS *ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == 0xE06D7363) { log_cxx_throw(ep->ContextRecord->Rsp); return EXCEPTION_CONTINUE_SEARCH; }   /* C++ throw */
    if (code != 0xC0000005 && code != 0xC000001D && code != 0xC0000096 && code != 0xC00000FD && code != 0xC0000409 &&
        code != 0xC0000374 && code != 0xC0000094 && code != 0xC000008C) return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedIncrement(&g_vehCount) > 8) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT *c = ep->ContextRecord; char at[160]; addr_desc((uint64_t)(uintptr_t)ep->ExceptionRecord->ExceptionAddress, at, sizeof at);
    if (code == 0xC0000005 && ep->ExceptionRecord->NumberParameters >= 2)
        logf_("EXCEPTION 0x%08lX at %s (%s 0x%llX) thread %lu", code, at, ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read",
              (unsigned long long)ep->ExceptionRecord->ExceptionInformation[1], GetCurrentThreadId());
    else logf_("EXCEPTION 0x%08lX at %s thread %lu", code, at, GetCurrentThreadId());
    logf_("  rax=%llX rcx=%llX rdx=%llX r8=%llX r9=%llX rsp=%llX", (unsigned long long)c->Rax, (unsigned long long)c->Rcx, (unsigned long long)c->Rdx,
          (unsigned long long)c->R8, (unsigned long long)c->R9, (unsigned long long)c->Rsp);
    log_stack((uint64_t)(uintptr_t)c->Rsp, 16);
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif /* SX_TEST */
