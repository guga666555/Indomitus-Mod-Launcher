/* test: call_to_arms_server.exe - exit code = number of gem_*.dll modules it has */
#include <windows.h>
#include <psapi.h>
#include <wchar.h>
int wmain(void)
{
    Sleep(400);
    HMODULE m[512]; DWORD need; int n = 0;
    K32EnumProcessModules(GetCurrentProcess(), m, sizeof m, &need);
    for (DWORD i = 0; i < need / sizeof(HMODULE); ++i) { wchar_t b[MAX_PATH]; K32GetModuleBaseNameW(GetCurrentProcess(), m[i], b, MAX_PATH); if (!_wcsnicmp(b, L"gem_", 4)) n++; }
    return n;
}
