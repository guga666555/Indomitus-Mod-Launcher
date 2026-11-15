/* GEM HostServer - offline test, included by src/gem_hostserver.c when built with -DHS_TEST.
 *   hs_test [folder with call_to_arms.exe]
 * 1. the "is this the server?" rule and the name check
 * 2. with a folder: the import-slot lookup on the real call_to_arms.exe
 * 3. on Windows only: the real handshake (events + shared block) between hook_CreateProcessW and a
 *    stand-in launcher thread, with a stand-in CreateProcessW that starts nothing
 * Portable C for 1 and 2 (also runs on Linux). */

static int T_fail, T_checks;
#define CHECK(cond, what) do { T_checks++; if (!(cond)) { T_fail++; printf("  FAIL  %s  (line %d)\n", what, __LINE__); } } while (0)

static void rules(void)
{
    printf("which CreateProcessW calls are the host server\n");
    CHECK(names_server(L"F:\\SteamLibrary\\steamapps\\common\\Call to Arms - Gates of Hell\\binaries\\x64\\call_to_arms_server.exe",
                       L"\"F:\\x\\call_to_arms_server.exe\" -appid 400750 -owner 1234"), "full path as application name (what the game passes)");
    CHECK(names_server(L"C:/Games/GoH/binaries/x64/CALL_TO_ARMS_SERVER.EXE", NULL), "forward slashes, upper case");
    CHECK(names_server(L"call_to_arms_server", L"x"), "no extension");
    CHECK(names_server(NULL, L"\"C:\\Program Files (x86)\\Steam\\x64\\call_to_arms_server.exe\" -mode steam"), "quoted first token of the command line");
    CHECK(names_server(NULL, L"   D:\\goh\\call_to_arms_server.exe -pipe \"cta_ipc_0\""), "unquoted first token, leading blanks");
    CHECK(names_server(L"", L"call_to_arms_server.exe"), "empty application name falls back to the command line");
    CHECK(!names_server(L"C:\\x\\call_to_arms.exe", L"\"C:\\x\\call_to_arms_server.exe\""), "the application name decides, not the command line");
    CHECK(!names_server(L"C:\\x\\BsSndRpt64.exe", L"report"), "another program");
    CHECK(!names_server(NULL, L"\"C:\\x\\notepad.exe\" C:\\x\\call_to_arms_server.exe"), "the server only as an argument");
    CHECK(!names_server(L"C:\\x\\call_to_arms_server.exe.bak", NULL), "longer file name");
    CHECK(!names_server(L"C:\\x\\my_call_to_arms_server.exe", NULL), "longer file name in front");
    CHECK(!names_server(L"C:\\call_to_arms_server.exe\\other.exe", NULL), "a folder of that name");
    CHECK(!names_server(L"C:\\x\\call_to_arms_serve", NULL), "shorter file name");
    CHECK(!names_server(NULL, NULL) && !names_server(NULL, L"") && !names_server(NULL, L"   ") && !names_server(NULL, L"\""), "nothing to look at");
    CHECK(sync_name_ok(L"Indomitus-0123456789abcdef-1234") && sync_name_ok(L"abcdefgh"), "launcher names are accepted");
    CHECK(!sync_name_ok(L"short") && !sync_name_ok(L"") && !sync_name_ok(L"Global\\abcdefgh") && !sync_name_ok(L"abc defgh") &&
          !sync_name_ok(L"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"), "anything else is not");
}

/* a PE file laid out as the loader would (headers + sections at their rva), zero filled */
static uint8_t *map_image(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *file = (uint8_t *)malloc((size_t)n + 1), *m = NULL;
    if (file && n > 0x400 && fread(file, 1, (size_t)n, f) == (size_t)n && file[0] == 'M' && file[1] == 'Z') {
        uint32_t lfa = rd32(file + 0x3C);
        if (lfa < (uint32_t)n - 0x200 && rd32(file + lfa) == 0x4550 && rd16(file + lfa + 24) == 0x20B) {
            const uint8_t *nt = file + lfa;
            uint32_t size = rd32(nt + 24 + 56), hdr = rd32(nt + 24 + 60), ns = rd16(nt + 6);
            const uint8_t *s = nt + 24 + rd16(nt + 20);
            m = (uint8_t *)calloc(1, (size_t)size + 16);
            if (m && hdr <= (uint32_t)n && hdr <= size) {
                memcpy(m, file, hdr);
                for (uint32_t i = 0; i < ns; i++, s += 40) {
                    uint32_t vs = rd32(s + 8), va = rd32(s + 12), rs = rd32(s + 16), raw = rd32(s + 20);
                    uint32_t sz = rs < vs ? rs : vs;
                    if (raw && sz && raw <= (uint32_t)n && sz <= (uint32_t)n - raw && va <= size && sz <= size - va) memcpy(m + va, file + raw, sz);
                }
            }
        }
    }
    free(file); fclose(f);
    return m;
}

static void lookups(const char *dir)
{
    char path[1024], dll[64] = "";
    printf("import slot on the files in %s\n", dir);
    snprintf(path, sizeof path, "%s/call_to_arms.exe", dir);
    uint8_t *exe = map_image(path);
    CHECK(exe != NULL, "call_to_arms.exe opened");
    if (exe) {
        uint64_t *slot = pe_import_slot(exe, "CreateProcessW", dll);
        if (slot) printf("  call_to_arms.exe imports %s!CreateProcessW through the slot at exe+0x%X\n", dll, (unsigned)((uint8_t *)slot - exe));
        CHECK(slot != NULL, "the game imports CreateProcessW by name");
        CHECK(pe_import_slot(exe, "CreateProcessWW", NULL) == NULL && pe_import_slot(exe, "CreateProcess", NULL) == NULL &&
              pe_import_slot(exe, "", NULL) == NULL, "a wrong name finds nothing");
        uint64_t *other = pe_import_slot(exe, "AssignProcessToJobObject", NULL);
        CHECK(other != NULL && other != slot, "another import has its own slot");
        free(exe);
    }
    uint8_t junk[64] = { 'M', 'Z' };
    CHECK(pe_import_slot(junk, "CreateProcessW", NULL) == NULL && pe_import_slot(NULL, "x", NULL) == NULL, "not a PE image: nothing found");
}

#ifdef _WIN32
/* ---------------------------------------------------------------- the handshake, for real */
static DWORD T_flags; static int T_calls;
static BOOL WINAPI fake_CreateProcessW(LPCWSTR app, LPWSTR cmd, LPSECURITY_ATTRIBUTES pa, LPSECURITY_ATTRIBUTES ta, BOOL inherit,
                                       DWORD flags, LPVOID env, LPCWSTR cwd, LPSTARTUPINFOW si, LPPROCESS_INFORMATION pi)
{
    (void)app; (void)cmd; (void)pa; (void)ta; (void)inherit; (void)env; (void)cwd; (void)si;
    T_flags = flags; T_calls++;
    pi->hProcess = GetCurrentProcess(); pi->hThread = GetCurrentThread();   /* ResumeThread on a running thread: harmless */
    pi->dwProcessId = 4242; pi->dwThreadId = 4343;
    SetLastError(0);
    return TRUE;
}

/* the stand-in launcher: mode 0 answers "queued", 1 answers "not queued", 2 never answers,
 * 3 first sends an answer for an older request, then the real one */
static struct { HANDLE req, ack, map, stop; volatile HsSync *view; int mode; uint32_t seen_pid, seen_tid; int requests; } TL;
static DWORD WINAPI launcher_thread(LPVOID arg)
{
    (void)arg;
    HANDLE hs[2] = { TL.stop, TL.req };
    for (;;) {
        if (WaitForMultipleObjects(2, hs, FALSE, INFINITE) != WAIT_OBJECT_0 + 1) return 0;
        TL.requests++;
        TL.seen_pid = TL.view->server_pid; TL.seen_tid = TL.view->server_tid;
        if (TL.mode == 2) continue;
        if (TL.mode == 3) { TL.view->ack_seq = TL.view->seq - 1; TL.view->result = 0; SetEvent(TL.ack); Sleep(300); }
        Sleep(100);                                     /* "loading the payload" */
        TL.view->result = TL.mode == 1 ? 0 : 1;
        TL.view->ack_seq = TL.view->seq;
        SetEvent(TL.ack);
    }
}

static void handshake(void)
{
    printf("handshake with a stand-in launcher (real events and shared block)\n");
    static const wchar_t SERVER[] = L"C:\\x\\call_to_arms_server.exe";
    PROCESS_INFORMATION pi; STARTUPINFOW si; memset(&si, 0, sizeof si); si.cb = sizeof si;
    wchar_t name[80], obj[120];
    swprintf(name, 80, L"IndomitusTest-%08lx-%08lx", GetCurrentProcessId(), GetTickCount());
    g_orig = fake_CreateProcessW;
    InitializeCriticalSection(&g_lock);
    cfg_wait_ms = 1500;

    /* no launcher at all */
    g_sync[0] = 0; T_calls = 0;
    CHECK(hook_CreateProcessW(SERVER, NULL, NULL, NULL, FALSE, 0x01000400, NULL, NULL, &si, &pi) && T_calls == 1 && T_flags == 0x01000400,
          "no name from the launcher: the server starts exactly as the game asked");
    { WBuf w = { g_sync, 80, 0 }; g_sync[0] = 0; w_str(&w, name); }
    DWORD t0 = GetTickCount();
    CHECK(hook_CreateProcessW(SERVER, NULL, NULL, NULL, FALSE, 0x01000400, NULL, NULL, &si, &pi) && T_flags == 0x01000400 && GetTickCount() - t0 < 500,
          "name given but no launcher objects: started as asked, no wait");

    swprintf(obj, 120, L"Local\\%ls-req", name); TL.req = CreateEventW(NULL, FALSE, FALSE, obj);
    swprintf(obj, 120, L"Local\\%ls-ack", name); TL.ack = CreateEventW(NULL, FALSE, FALSE, obj);
    swprintf(obj, 120, L"Local\\%ls-map", name); TL.map = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(HsSync), obj);
    TL.stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    TL.view = TL.map ? (volatile HsSync *)MapViewOfFile(TL.map, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(HsSync)) : NULL;
    CHECK(TL.req && TL.ack && TL.view, "stand-in launcher objects created");
    if (!TL.req || !TL.ack || !TL.view) return;
    HANDLE th = CreateThread(NULL, 0, launcher_thread, NULL, 0, NULL);

    /* objects there, but not a launcher's block (magic missing) */
    t0 = GetTickCount();
    CHECK(hook_CreateProcessW(SERVER, NULL, NULL, NULL, FALSE, 0x01000400, NULL, NULL, &si, &pi) && T_flags == 0x01000400 && TL.requests == 0,
          "shared block without the launcher's mark: started as asked");
    TL.view->magic = HS_MAGIC; TL.view->launcher_pid = GetCurrentProcessId();

    TL.mode = 0; t0 = GetTickCount();
    BOOL ok = hook_CreateProcessW(SERVER, NULL, NULL, NULL, FALSE, 0x01000400, NULL, NULL, &si, &pi);
    DWORD took = GetTickCount() - t0;
    CHECK(ok && T_flags == (0x01000400 | CREATE_SUSPENDED), "launcher there: the server is created with CREATE_SUSPENDED added");
    CHECK(TL.requests == 1 && TL.seen_pid == 4242 && TL.seen_tid == 4343, "the launcher got the process and thread id");
    CHECK(took >= 90 && took < 1000, "the hook returned when the launcher answered (about 100 ms)");
    CHECK(pi.dwProcessId == 4242, "the game gets its process information back");

    CHECK(hook_CreateProcessW(L"C:\\x\\notepad.exe", NULL, NULL, NULL, FALSE, 0x10, NULL, NULL, &si, &pi) && T_flags == 0x10 && TL.requests == 1,
          "another program: untouched, the launcher is not asked");

    TL.mode = 1;
    CHECK(hook_CreateProcessW(SERVER, NULL, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi) && TL.requests == 2 && TL.view->result == 0,
          "launcher answers 'not loaded': the hook still returns success to the game");

    TL.mode = 3; t0 = GetTickCount();
    ok = hook_CreateProcessW(SERVER, NULL, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi); took = GetTickCount() - t0;
    CHECK(ok && took >= 350 && took < 1400 && TL.view->result == 1, "an answer to an older request is ignored, the right one is waited for");

    TL.mode = 2; t0 = GetTickCount();
    ok = hook_CreateProcessW(SERVER, NULL, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi); took = GetTickCount() - t0;
    CHECK(ok && took >= 1400 && took < 3000, "launcher never answers: the server is released after WaitSeconds");

    TL.mode = 0; t0 = GetTickCount();
    ok = hook_CreateProcessW(SERVER, NULL, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi); took = GetTickCount() - t0;
    CHECK(ok && took < 1000 && TL.view->result == 1, "and the next start works again");

    SetEvent(TL.stop); WaitForSingleObject(th, 2000); CloseHandle(th);
    UnmapViewOfFile((LPCVOID)TL.view); CloseHandle(TL.map); CloseHandle(TL.req); CloseHandle(TL.ack); CloseHandle(TL.stop);
    t0 = GetTickCount();
    CHECK(hook_CreateProcessW(SERVER, NULL, NULL, NULL, FALSE, 0x01000400, NULL, NULL, &si, &pi) && T_flags == 0x01000400 && GetTickCount() - t0 < 500,
          "launcher gone: started as asked, no wait");
}
#endif

int main(int argc, char **argv)
{
    rules();
    if (argc > 1) lookups(argv[1]); else printf("(no folder given: the lookup on call_to_arms.exe was not run)\n");
#ifdef _WIN32
    handshake();
#else
    printf("(not Windows: the handshake test was not run)\n");
#endif
    printf("%d checks, %d failed\n", T_checks, T_fail);
    printf(T_fail ? "FAILED\n" : "ALL OK\n");
    return T_fail ? 1 : 0;
}
