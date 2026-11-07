/* test: call_to_arms_citest.exe <payload dir> <server exe>  - loads gem_aa_childinject.dll from the payload
   dir (as the launcher would), then starts the server exe the three ways the game could */
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
int wmain(int argc, wchar_t **argv)
{
    if (argc < 3) return 2;
    wchar_t p[MAX_PATH]; swprintf(p, MAX_PATH, L"%ls\\gem_aa_childinject.dll", argv[1]);
    if (!LoadLibraryW(p)) { printf("load failed %lu\n", GetLastError()); return 3; }
    if (argc > 4) {   /* probe: <payload> <server exe> <args> <ms>: start once, kill after ms */
        wchar_t c1[2048]; swprintf(c1, 2048, L"\"%ls\" %ls", argv[2], argv[3]);
        STARTUPINFOW s1 = { sizeof s1 }; PROCESS_INFORMATION p1;
        if (!CreateProcessW(NULL, c1, NULL, NULL, FALSE, 0, NULL, NULL, &s1, &p1)) { printf("cp failed %lu\n", GetLastError()); return 4; }
        DWORD w = WaitForSingleObject(p1.hProcess, (DWORD)_wtoi(argv[4])), c = 0;
        if (w == WAIT_TIMEOUT) { TerminateProcess(p1.hProcess, 0xDEAD); printf("killed after %ls ms\n", argv[4]); }
        else { GetExitCodeProcess(p1.hProcess, &c); printf("exited 0x%08lX\n", c); }
        Sleep(500);
        return 0;
    }
    /* 1: CreateProcessW, app name null, quoted command line with args (like the game) */
    wchar_t cmd[1024]; swprintf(cmd, 1024, L"\"%ls\" -appid 400750 -owner %lu -pipe \"cta_ipc_0\"", argv[2], GetCurrentProcessId());
    STARTUPINFOW si = { sizeof si }; PROCESS_INFORMATION pi;
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) { printf("cp failed %lu\n", GetLastError()); return 4; }
    WaitForSingleObject(pi.hProcess, 10000); DWORD c; GetExitCodeProcess(pi.hProcess, &c); printf("run1 exit %lu\n", c);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    /* 2: ShellExecuteExW */
    SHELLEXECUTEINFOW se = { sizeof se }; se.fMask = SEE_MASK_NOCLOSEPROCESS; se.lpFile = argv[2]; se.lpParameters = L"-pipe x"; se.nShow = SW_HIDE;
    if (!ShellExecuteExW(&se)) { printf("se failed %lu\n", GetLastError()); return 5; }
    WaitForSingleObject(se.hProcess, 10000); GetExitCodeProcess(se.hProcess, &c); printf("run2 exit %lu\n", c); CloseHandle(se.hProcess);
    /* 3: caller asks for suspended, resumes itself */
    if (!CreateProcessW(argv[2], cmd, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, NULL, &si, &pi)) return 6;
    Sleep(300); ResumeThread(pi.hThread);
    WaitForSingleObject(pi.hProcess, 10000); GetExitCodeProcess(pi.hProcess, &c); printf("run3 exit %lu\n", c);
    /* 4: an unrelated child is left alone */
    swprintf(cmd, 1024, L"cmd.exe /c exit 7");
    if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) { WaitForSingleObject(pi.hProcess, 5000); GetExitCodeProcess(pi.hProcess, &c); printf("run4 cmd exit %lu\n", c); }
    Sleep(1500);   /* let the watch threads log the exits */
    return 0;
}
