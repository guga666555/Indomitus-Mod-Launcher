/* GEM Fauna - code patching (same as GEM ScriptExt 0.8.1 patch.c, plus vt_hook / patch_call_rel32)
 * Part of the unity build: #included by gem_fauna.c, not compiled on its own.
 * Executable memory near the exe (alloc_near), thread suspension while patching (patch_code: every other thread
 * stopped outside the bytes being written, or nothing is written), 5-byte inline hooks (install_jmp5), call-site
 * redirection (redirect_call / patch_call_rel32) and vtable slot hooks (vt_hook).
 */

#ifndef FX_TEST
/* executable memory within +-2 GB of target, so a rel32 jmp/call can reach it */
static uint8_t *alloc_near(uint8_t *target, size_t size)
{
    SYSTEM_INFO si; GetSystemInfo(&si);
    uintptr_t gran = si.dwAllocationGranularity;
    uintptr_t lo = (uintptr_t)target > 0x7FF00000 ? (uintptr_t)target - 0x7FF00000 : 0x10000;
    uintptr_t hi = (uintptr_t)target + 0x7FF00000;
    for (uintptr_t a = ((uintptr_t)target + gran) & ~(gran - 1); a < hi; ) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((void *)a, &mbi, sizeof mbi)) break;
        if (mbi.State == MEM_FREE) {
            void *p = VirtualAlloc((void *)a, size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
            if (p) return p;
        }
        a = ((uintptr_t)mbi.BaseAddress + mbi.RegionSize + gran - 1) & ~(gran - 1);
    }
    for (uintptr_t a = ((uintptr_t)target - gran) & ~(gran - 1); a > lo; a -= gran) {
        void *p = VirtualAlloc((void *)a, size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        if (p) return p;
    }
    return NULL;
}

/* suspend every other thread of the process, retrying while one of them is executing inside [lo, hi) - the bytes about
   to be patched. Returns the number of handles in hs (resume_others resumes and closes them), or -1 when the threads
   cannot all be stopped safely (no snapshot, more than max threads, one keeps running inside the range): the caller
   then does not patch. */
#define FX_MAX_THREADS 1024
static int suspend_others(HANDLE *hs, int max, uint8_t *lo, uint8_t *hi)
{
    for (int attempt = 0; attempt < 20; attempt++) {
        int n = 0, bad = 0, over = 0;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap == INVALID_HANDLE_VALUE) return -1;
        THREADENTRY32 te = { sizeof te };
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

/* write len bytes at dst (code) with every other thread stopped; 0 = not written */
static int patch_code(uint8_t *dst, const void *src, size_t len, const char *what)
{
    DWORD old;
    if (!VirtualProtect(dst, len, PAGE_EXECUTE_READWRITE, &old)) { logf_("%s: VirtualProtect failed (%lu)", what, GetLastError()); return 0; }
    static HANDLE hs[FX_MAX_THREADS];                            /* init runs once, on one thread */
    int n = suspend_others(hs, FX_MAX_THREADS, dst, dst + len);
    if (n < 0) { VirtualProtect(dst, len, old, &old); logf_("%s: could not stop the other threads safely - not patched", what); return 0; }
    memcpy(dst, src, len);
    resume_others(hs, n);
    VirtualProtect(dst, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), dst, len);
    return 1;
}

/* hook f: its first instructions become 'jmp rel32' to a near stub that jumps to hook. *orig receives the trampoline
   (the relocated original instructions + a jump back) BEFORE the jump is written, so a thread entering f at once
   already finds it. 0 = not hooked. */
static int install_jmp5(uint8_t *f, void *hook, void **orig, const char *what)
{
    int prolen = prologue_len(f);
    if (!prolen) {
        char got[64] = ""; for (int i = 0; i < 12; i++) snprintf(got + 3 * i, sizeof got - 3 * i, "%02X ", f[i]);
        logf_("%s: prologue not relocatable (%s) - not hooking", what, got); return 0;
    }
    uint8_t *stub = alloc_near(f, 0x1000);
    if (!stub) { logf_("%s: no memory within 2GB for the stub", what); return 0; }
    int64_t rel = (int64_t)(stub - (f + 5));
    if (rel > 0x7FFFFFFF || rel < -0x80000000LL) { logf_("%s: stub too far", what); VirtualFree(stub, 0, MEM_RELEASE); return 0; }
    /* stub: jmp [rip+0] ; abs64 hook */
    stub[0] = 0xFF; stub[1] = 0x25; *(uint32_t *)(stub + 2) = 0; *(uint64_t *)(stub + 6) = (uint64_t)(uintptr_t)hook;
    /* trampoline at stub+0x40: original bytes ; jmp [rip+0] ; abs64 f+prolen */
    uint8_t *tr = stub + 0x40;
    memcpy(tr, f, prolen);
    tr[prolen] = 0xFF; tr[prolen + 1] = 0x25; *(uint32_t *)(tr + prolen + 2) = 0; *(uint64_t *)(tr + prolen + 6) = (uint64_t)(uintptr_t)(f + prolen);
    FlushInstructionCache(GetCurrentProcess(), stub, 0x1000);
    *orig = tr;
    uint8_t patch[16]; memset(patch, 0x90, sizeof patch);       /* jmp rel32, nop padding up to the relocated length */
    patch[0] = 0xE9; *(int32_t *)(patch + 1) = (int32_t)rel;
    if (!patch_code(f, patch, prolen, what)) { *orig = NULL; VirtualFree(stub, 0, MEM_RELEASE); return 0; }
    return 1;
}

/* point the rel32 of an existing 5-byte 'call rel32' (E8) at pad (near code already written there). 0 = not changed. */
static int patch_call_rel32(uint8_t *e8, uint8_t *pad, const char *what)
{
    int64_t rel = (int64_t)(pad - (e8 + 5));
    if (rel > 0x7FFFFFFF || rel < -0x80000000LL) { logf_("%s: jump pad too far", what); return 0; }
    int32_t r32 = (int32_t)rel;
    return patch_code(e8 + 1, &r32, 4, what);
}

/* point an existing 'call rel32' (E8) at fn, through a near jump pad (jmp [rip+0] ; abs64 fn). *orig (if given)
   receives the call's original target before the call is changed. 0 = not changed. */
static int redirect_call(uint8_t *e8, void *fn, void **orig, const char *what)
{
    uint8_t *pad = alloc_near(e8, 0x40);
    if (!pad) { logf_("%s: no memory within 2GB for the jump pad", what); return 0; }
    pad[0] = 0xFF; pad[1] = 0x25; *(uint32_t *)(pad + 2) = 0; *(uint64_t *)(pad + 6) = (uint64_t)(uintptr_t)fn;
    FlushInstructionCache(GetCurrentProcess(), pad, 0x40);
    if (orig) *orig = e8 + 5 + *(int32_t *)(e8 + 1);
    if (!patch_call_rel32(e8, pad, what)) { VirtualFree(pad, 0, MEM_RELEASE); return 0; }
    return 1;
}

/* replace one vtable slot; *orig receives the old pointer first */
static int vt_hook(void **vt, int slot, void *fn, void **orig)
{
    DWORD old;
    if (!VirtualProtect(vt + slot, 8, PAGE_READWRITE, &old)) return 0;
    *orig = vt[slot]; vt[slot] = fn;
    VirtualProtect(vt + slot, 8, old, &old);
    return 1;
}
#endif /* FX_TEST */
