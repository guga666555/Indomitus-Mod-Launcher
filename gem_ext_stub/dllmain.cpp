// gem_ext stub - proves injection and shows *when* the DLL was loaded relative to
// the game's own startup. Replace with the real engine extender once this works.
//
// Writes <game dir>\gem_ext.log. Only kernel32 calls happen inside DllMain (safe
// under loader lock); the module census runs on a worker thread a bit later.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cstdarg>

#pragma comment(lib, "psapi.lib")

static wchar_t g_logPath[MAX_PATH];

static void Log(const wchar_t* fmt, ...)
{
    wchar_t line[2048];
    va_list ap; va_start(ap, fmt);
    int n = _vsnwprintf(line, 2047, fmt, ap);
    va_end(ap);
    if (n < 0) n = 2047;
    line[n] = 0;

    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t out[2200];
    int m = swprintf(out, 2200, L"[%02d:%02d:%02d.%03d] %ls\r\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, line);

    char utf8[4400];
    int b = WideCharToMultiByte(CP_UTF8, 0, out, m, utf8, sizeof(utf8), nullptr, nullptr);
    HANDLE h = CreateFileW(g_logPath, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w; WriteFile(h, utf8, b, &w, nullptr);
    CloseHandle(h);
}

static const wchar_t* Loaded(const wchar_t* mod) { return GetModuleHandleW(mod) ? L"yes" : L"no"; }

static DWORD WINAPI Worker(LPVOID)
{
    // By now the game has run for a few seconds: list what it loaded.
    Sleep(5000);
    HMODULE mods[1024]; DWORD needed = 0;
    if (EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) {
        DWORD count = needed / sizeof(HMODULE);
        Log(L"module census after 5 s: %u modules", count);
        for (DWORD i = 0; i < count; ++i) {
            wchar_t name[MAX_PATH];
            if (GetModuleFileNameW(mods[i], name, MAX_PATH)) Log(L"  %p %ls", mods[i], name);
        }
    }
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);

        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        lstrcpynW(g_logPath, exe, MAX_PATH);
        wchar_t* slash = wcsrchr(g_logPath, L'\\');
        if (slash) lstrcpynW(slash + 1, L"gem_ext.log", (int)(MAX_PATH - (slash + 1 - g_logPath)));

        Log(L"==== gem_ext stub attached ====");
        Log(L"process: %ls (pid %u)", exe, GetCurrentProcessId());
        Log(L"cmdline: %ls", GetCommandLineW());
        Log(L"SteamAppId env: %ls", _wgetenv(L"SteamAppId") ? _wgetenv(L"SteamAppId") : L"(unset)");

        // Timing indicators. With early-bird injection the first three are usually
        // "yes" (static imports are resolved before any APC runs) but nothing the game
        // loads *dynamically* (d3d11/dxgi, mss, etc.) should be present yet.
        Log(L"already loaded: steam_api64=%ls  d3d11=%ls  dxgi=%ls  user32=%ls  gdi32=%ls",
            Loaded(L"steam_api64.dll"), Loaded(L"d3d11.dll"), Loaded(L"dxgi.dll"),
            Loaded(L"user32.dll"), Loaded(L"gdi32.dll"));

        HANDLE t = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
