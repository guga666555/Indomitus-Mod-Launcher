/* Fake Win32 header for a host-side syntax check of the dll build:
   clang -fsyntax-only -fshort-wchar -I test/stub gem_x/gem_x.c   (no mingw headers needed). Not for real builds. */
#ifndef GEM_STUB_WINDOWS_H
#define GEM_STUB_WINDOWS_H
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <wchar.h>
#include <strings.h>
#define __declspec(x)
#define WINAPI
typedef int BOOL; typedef uint32_t DWORD; typedef unsigned int UINT; typedef void *HANDLE, *HMODULE, *HINSTANCE, *LPVOID;
typedef size_t SIZE_T; typedef uint16_t WORD;
#define TRUE 1
#define FALSE 0
#define MAX_PATH 260
#define CP_ACP 0
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define ERROR_ALREADY_EXISTS 183
#define DLL_PROCESS_ATTACH 1
#define MEM_COMMIT 0x1000
#define MEM_RESERVE 0x2000
#define PAGE_EXECUTE_READ 0x20
#define PAGE_EXECUTE_READWRITE 0x40
#define CONTEXT_CONTROL 0x100001
#define THREAD_SUSPEND_RESUME 2
#define THREAD_GET_CONTEXT 8
#define THREAD_QUERY_INFORMATION 0x40
typedef struct { WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds; } SYSTEMTIME;
typedef struct { void *p; } CRITICAL_SECTION;
typedef struct { DWORD ContextFlags; uint8_t pad[0xF0]; uint64_t Rip; uint8_t more[0x400]; } CONTEXT;
void GetLocalTime(SYSTEMTIME *); void EnterCriticalSection(CRITICAL_SECTION *); void LeaveCriticalSection(CRITICAL_SECTION *);
void InitializeCriticalSection(CRITICAL_SECTION *); void OutputDebugStringA(const char *);
int MultiByteToWideChar(UINT, DWORD, const char *, int, wchar_t *, int);
int WideCharToMultiByte(UINT, DWORD, const wchar_t *, int, char *, int, const char *, BOOL *);
DWORD GetPrivateProfileStringW(const wchar_t *, const wchar_t *, const wchar_t *, wchar_t *, DWORD, const wchar_t *);
UINT GetPrivateProfileIntW(const wchar_t *, const wchar_t *, int, const wchar_t *);
DWORD GetModuleFileNameW(HMODULE, wchar_t *, DWORD);
FILE *_wfopen(const wchar_t *, const wchar_t *);
BOOL ReadProcessMemory(HANDLE, const void *, void *, SIZE_T, SIZE_T *);
HANDLE GetCurrentProcess(void);
void *VirtualAlloc(void *, SIZE_T, DWORD, DWORD);
BOOL VirtualProtect(void *, SIZE_T, DWORD, DWORD *);
BOOL FlushInstructionCache(HANDLE, const void *, SIZE_T);
DWORD GetCurrentThreadId(void); DWORD GetCurrentProcessId(void);
HANDLE OpenThread(DWORD, BOOL, DWORD); DWORD SuspendThread(HANDLE); DWORD ResumeThread(HANDLE);
BOOL GetThreadContext(HANDLE, CONTEXT *); BOOL CloseHandle(HANDLE); DWORD GetLastError(void);
HMODULE GetModuleHandleW(const wchar_t *); UINT GetSystemDirectoryW(wchar_t *, UINT);
int _wcsnicmp(const wchar_t *, const wchar_t *, size_t);
HANDLE CreateMutexW(void *, BOOL, const wchar_t *);
BOOL AllocConsole(void); BOOL SetConsoleTitleA(const char *); BOOL DisableThreadLibraryCalls(HMODULE);
#define _strnicmp strncasecmp
#define _stricmp strcasecmp
#endif
