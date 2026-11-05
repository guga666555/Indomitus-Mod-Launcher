/* gem_inject - in-house test helper: loads a dll into a running GoH game/editor.
 *   gem_inject.exe [dll]      (default: gem_squadfollow.dll next to this exe)
 * Targets the first call_to_arms.exe / call_to_arms_ed.exe found. Start the game through the
 * Indomitus launcher first, inject in the main menu (before loading a map). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <wchar.h>

int wmain(int argc, wchar_t **argv)
{
    wchar_t dll[MAX_PATH];
    if (argc > 1) GetFullPathNameW(argv[1], MAX_PATH, dll, NULL);
    else {
        GetModuleFileNameW(NULL, dll, MAX_PATH);
        wchar_t *s = wcsrchr(dll, L'\\');
        wcscpy(s ? s + 1 : dll, L"gem_squadfollow.dll");
    }
    if (GetFileAttributesW(dll) == INVALID_FILE_ATTRIBUTES) { wprintf(L"dll not found: %ls\n", dll); return 1; }

    DWORD pid = 0; wchar_t name[MAX_PATH] = L"";
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe = { sizeof pe };
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
        if (!_wcsicmp(pe.szExeFile, L"call_to_arms.exe") || !_wcsicmp(pe.szExeFile, L"call_to_arms_ed.exe")) {
            pid = pe.th32ProcessID; wcscpy(name, pe.szExeFile); break;
        }
    CloseHandle(snap);
    if (!pid) { wprintf(L"game/editor not running\n"); return 1; }

    HANDLE p = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_WRITE |
                           PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!p) { wprintf(L"OpenProcess failed %lu\n", GetLastError()); return 1; }
    SIZE_T sz = (wcslen(dll) + 1) * sizeof(wchar_t);
    void *mem = VirtualAllocEx(p, NULL, sz, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!mem || !WriteProcessMemory(p, mem, dll, sz, NULL)) { wprintf(L"write failed %lu\n", GetLastError()); return 1; }
    LPTHREAD_START_ROUTINE ll = (LPTHREAD_START_ROUTINE)(void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    HANDLE t = CreateRemoteThread(p, NULL, 0, ll, mem, 0, NULL);
    if (!t) { wprintf(L"CreateRemoteThread failed %lu\n", GetLastError()); return 1; }
    WaitForSingleObject(t, 10000);
    DWORD code = 0; GetExitCodeThread(t, &code);
    VirtualFreeEx(p, mem, 0, MEM_RELEASE);
    wprintf(code ? L"injected %ls into %ls (pid %lu) - check the .log next to the dll\n"
                 : L"LoadLibrary failed inside %ls%ls (pid %lu)\n",
            code ? dll : L"", name, pid);
    return code ? 0 : 1;
}
