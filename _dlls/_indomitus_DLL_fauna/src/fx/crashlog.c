/* GEM Fauna - crash log
 * Part of the unity build: #included by gem_fauna.c, not compiled on its own.
 * First-chance fatal exceptions with a stack scan, and who ends the process (ExitProcess /
 * TerminateProcess IAT hooks).
 */

#ifndef FX_TEST
/* ------------------------------------------------------------------------------------ */
/* crash logger: first-chance fatal exceptions -> gem_fauna.log (does not handle them)  */
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

static void log_stack(uint64_t rsp, int max)
{
    uint64_t *sp = (uint64_t *)(uintptr_t)rsp; int shown = 0;
    for (int i = 0; i < 0x400 && shown < max; i++) {
        uint64_t v; SIZE_T got = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), sp + i, &v, 8, &got) || got != 8) break;
        int inExe = in_text((uint8_t *)(uintptr_t)v);
        HMODULE m = NULL;
        int inSelf = GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)(uintptr_t)v, &m) && m == g_self;
        if (!inExe && !inSelf) continue;
        char d[160]; addr_desc(v, d, sizeof d); logf_("  [rsp+0x%03X] %s", i * 8, d); shown++;
    }
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

/* patch module's IAT entries that point at target (already resolved) */
static int iat_patch(HMODULE mod, void *target, void *hook)
{
    if (!mod || !target) return 0;
    uint8_t *b = (uint8_t *)mod;
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(b + ((IMAGE_DOS_HEADER *)b)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return 0;
    int n = 0;
    for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(b + dir.VirtualAddress); d->Name; d++)
        for (uint64_t *t = (uint64_t *)(b + d->FirstThunk); *t; t++)
            if ((void *)(uintptr_t)*t == target) {
                DWORD old;
                if (VirtualProtect(t, 8, PAGE_READWRITE, &old)) { *t = (uint64_t)(uintptr_t)hook; VirtualProtect(t, 8, old, &old); n++; }
            }
    return n;
}

static void install_exit_hooks(void)
{
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    void *ep = (void *)GetProcAddress(k32, "ExitProcess"), *tp = (void *)GetProcAddress(k32, "TerminateProcess");
    g_exitProcessOrig = (ExitProcessFn)ep; g_terminateProcessOrig = (TerminateProcessFn)tp;
    const wchar_t *mods[] = { NULL, L"BugSplat64.dll", L"bdcap64.dll" };
    int n = 0;
    for (int i = 0; i < 3; i++) {
        HMODULE m = mods[i] ? GetModuleHandleW(mods[i]) : GetModuleHandleW(NULL);
        n += iat_patch(m, ep, (void *)hook_exit_process) + iat_patch(m, tp, (void *)hook_terminate_process);
    }
    logf_("exit trace                     %d import(s) hooked (ExitProcess/TerminateProcess)", n);
}

static LONG CALLBACK fx_veh(EXCEPTION_POINTERS *ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    static volatile LONG cxx;
    if (code == 0xE06D7363) {                                            /* C++ throw: log the first few with their call chain */
        if (InterlockedIncrement(&cxx) > 6) return EXCEPTION_CONTINUE_SEARCH;
        logf_("C++ exception thrown (thread %lu)", GetCurrentThreadId());
        log_stack((uint64_t)(uintptr_t)ep->ContextRecord->Rsp, 10);
        return EXCEPTION_CONTINUE_SEARCH;
    }
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

#endif /* FX_TEST */
