/* GEM Teleport - crash log
 * Part of the unity build: #included by gem_teleport.c, not compiled on its own.
 * First-chance fatal exceptions with a stack scan (C++ throws only with log_cpp_exceptions=1). The exit trace of
 * GEM Fauna is not repeated here: Fauna loads first and already owns those imports.
 */

#ifndef FX_TEST
/* ------------------------------------------------------------------------------------ */
/* crash logger: first-chance fatal exceptions -> gem_teleport.log (does not handle them)  */
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

static int g_logCpp;                                     /* log_cpp_exceptions: the engine throws and catches some at load */

static LONG CALLBACK fx_veh(EXCEPTION_POINTERS *ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    static volatile LONG cxx;
    if (code == 0xE06D7363) {                                            /* C++ throw: log the first few with their call chain */
        if (!g_logCpp || InterlockedIncrement(&cxx) > 6) return EXCEPTION_CONTINUE_SEARCH;
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
