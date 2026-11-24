/* Minimal fake Win32 header: ONLY for host-side syntax checking of gem_fauna.c
   (clang -fsyntax-only -target x86_64-windows-gnu -I tests/stub). Never used for the real build. */
#ifndef STUB_WINDOWS_H
#define STUB_WINDOWS_H
#include <stdint.h>
#include <stddef.h>
#include <wchar.h>
#include <stdio.h>

#define WINAPI
#define MAX_PATH 260
#define CP_ACP 0
#define TRUE 1
#define FALSE 0
#define DLL_PROCESS_ATTACH 1
#define MEM_RESERVE 0x2000
#define MEM_COMMIT 0x1000
#define PAGE_NOACCESS 0x01
#define PAGE_GUARD 0x100
#define MEM_RELEASE 0x8000
#define MEM_FREE 0x10000
#define PAGE_EXECUTE_READWRITE 0x40
#define PAGE_READWRITE 0x04
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define THREAD_SUSPEND_RESUME 2
#define THREAD_GET_CONTEXT 8
#define CONTEXT_CONTROL 0x100001
#define IMAGE_FIRST_SECTION(nt) ((IMAGE_SECTION_HEADER *)((uint8_t *)&(nt)->OptionalHeader + (nt)->FileHeader.SizeOfOptionalHeader))

typedef int BOOL; typedef unsigned long DWORD; typedef unsigned int UINT; typedef void *HANDLE; typedef void *HMODULE; typedef void *HINSTANCE;
typedef void *LPVOID; typedef size_t SIZE_T; typedef void (*FARPROC)(void); typedef unsigned short WORD; typedef uint64_t ULONGLONG;
typedef struct { WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds; } SYSTEMTIME;
typedef struct { DWORD VirtualAddress, Size; } IMAGE_DATA_DIRECTORY;
typedef struct { uint16_t e_magic; uint8_t pad[58]; int32_t e_lfanew; } IMAGE_DOS_HEADER;
typedef struct { WORD Machine, NumberOfSections; DWORD TimeDateStamp, PointerToSymbolTable, NumberOfSymbols; WORD SizeOfOptionalHeader, Characteristics; } IMAGE_FILE_HEADER;
typedef struct { WORD Magic; uint8_t rest[110]; IMAGE_DATA_DIRECTORY DataDirectory[16]; } IMAGE_OPTIONAL_HEADER64;
typedef struct { DWORD Signature; IMAGE_FILE_HEADER FileHeader; IMAGE_OPTIONAL_HEADER64 OptionalHeader; } IMAGE_NT_HEADERS64;
typedef struct { uint8_t Name[8]; union { DWORD PhysicalAddress, VirtualSize; } Misc; DWORD VirtualAddress, SizeOfRawData, PointerToRawData, PointerToRelocations, PointerToLinenumbers; WORD NumberOfRelocations, NumberOfLinenumbers; DWORD Characteristics; } IMAGE_SECTION_HEADER;
typedef struct { DWORD dwPageSize; void *lpMinimumApplicationAddress, *lpMaximumApplicationAddress; uintptr_t dwActiveProcessorMask; DWORD dwNumberOfProcessors, dwProcessorType, dwAllocationGranularity; WORD wProcessorLevel, wProcessorRevision; } SYSTEM_INFO;
typedef struct { void *BaseAddress, *AllocationBase; DWORD AllocationProtect; WORD PartitionId; SIZE_T RegionSize; DWORD State, Protect, Type; } MEMORY_BASIC_INFORMATION;
typedef struct { uint8_t pad[0x30]; DWORD ContextFlags; uint8_t pad2[0x44]; uint64_t Rax, Rcx, Rdx, Rbx, Rsp, Rbp, Rsi, Rdi, R8, R9, R10, R11, R12, R13, R14, R15, Rip; uint8_t pad3[0x400]; } CONTEXT;

UINT GetSystemDirectoryW(wchar_t *, UINT); HMODULE LoadLibraryW(const wchar_t *); FARPROC GetProcAddress(HMODULE, const char *);
void GetLocalTime(SYSTEMTIME *); void OutputDebugStringA(const char *);
int MultiByteToWideChar(UINT, DWORD, const char *, int, wchar_t *, int); int WideCharToMultiByte(UINT, DWORD, const wchar_t *, int, char *, int, const char *, int *);
DWORD GetPrivateProfileStringW(const wchar_t *, const wchar_t *, const wchar_t *, wchar_t *, DWORD, const wchar_t *);
UINT GetPrivateProfileIntW(const wchar_t *, const wchar_t *, int, const wchar_t *);
HMODULE GetModuleHandleW(const wchar_t *); DWORD GetModuleFileNameW(HMODULE, wchar_t *, DWORD);
HANDLE GetProcessHeap(void); void *HeapAlloc(HANDLE, DWORD, SIZE_T); BOOL HeapFree(HANDLE, DWORD, void *);
void MemoryBarrier(void); void GetSystemInfo(SYSTEM_INFO *);
SIZE_T VirtualQuery(const void *, MEMORY_BASIC_INFORMATION *, SIZE_T); void *VirtualAlloc(void *, SIZE_T, DWORD, DWORD); BOOL VirtualFree(void *, SIZE_T, DWORD);
BOOL VirtualProtect(void *, SIZE_T, DWORD, DWORD *); BOOL FlushInstructionCache(HANDLE, const void *, SIZE_T);
HANDLE GetCurrentProcess(void); DWORD GetCurrentProcessId(void); DWORD GetCurrentThreadId(void);
HANDLE OpenThread(DWORD, BOOL, DWORD); DWORD SuspendThread(HANDLE); DWORD ResumeThread(HANDLE); BOOL GetThreadContext(HANDLE, CONTEXT *); BOOL CloseHandle(HANDLE);
void Sleep(DWORD); BOOL ReadProcessMemory(HANDLE, const void *, void *, SIZE_T, SIZE_T *);
int _strnicmp(const char *, const char *, size_t); int _wcsnicmp(const wchar_t *, const wchar_t *, size_t); FILE *_wfopen(const wchar_t *, const wchar_t *);
BOOL AllocConsole(void); BOOL SetConsoleTitleA(const char *); HANDLE CreateThread(void *, SIZE_T, DWORD (WINAPI *)(LPVOID), LPVOID, DWORD, DWORD *);
BOOL DisableThreadLibraryCalls(HMODULE); DWORD GetLastError(void);
typedef struct { uint8_t opaque[40]; } CRITICAL_SECTION; typedef short SHORT;
void InitializeCriticalSection(CRITICAL_SECTION *); void EnterCriticalSection(CRITICAL_SECTION *); void LeaveCriticalSection(CRITICAL_SECTION *);
DWORD GetTickCount(void); unsigned long long GetTickCount64(void); SHORT GetAsyncKeyState(int);
#define CALLBACK
#define EXCEPTION_CONTINUE_SEARCH 0
#define GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS 4
#define GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT 2
typedef long LONG; typedef const wchar_t *LPCWSTR; typedef uintptr_t ULONG_PTR;
typedef struct { DWORD ExceptionCode, ExceptionFlags; void *ExceptionRecord; void *ExceptionAddress; DWORD NumberParameters; ULONG_PTR ExceptionInformation[15]; } EXCEPTION_RECORD;
typedef struct { EXCEPTION_RECORD *ExceptionRecord; CONTEXT *ContextRecord; } EXCEPTION_POINTERS;
void *AddVectoredExceptionHandler(unsigned long, LONG (*)(EXCEPTION_POINTERS *));
BOOL GetModuleHandleExW(DWORD, LPCWSTR, HMODULE *); LONG InterlockedIncrement(volatile LONG *);
#define DLL_PROCESS_DETACH 0
#define IMAGE_DIRECTORY_ENTRY_IMPORT 1
typedef struct { DWORD OriginalFirstThunk, TimeDateStamp, ForwarderChain, Name, FirstThunk; } IMAGE_IMPORT_DESCRIPTOR;
DWORD GetProcessId(HANDLE); BOOL TerminateProcess(HANDLE, UINT); void ExitProcess(UINT);
/* 0.2.7+ icon folder scan, 0.5 multiplier */
#define FILE_ATTRIBUTE_DIRECTORY 0x10
#define CP_UTF8 65001
typedef struct { DWORD dwLowDateTime, dwHighDateTime; } FILETIME;
typedef struct { DWORD dwFileAttributes; FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime; DWORD nFileSizeHigh, nFileSizeLow, dwReserved0, dwReserved1; wchar_t cFileName[MAX_PATH]; wchar_t cAlternateFileName[14]; } WIN32_FIND_DATAW;
HANDLE FindFirstFileW(const wchar_t *, WIN32_FIND_DATAW *); BOOL FindNextFileW(HANDLE, WIN32_FIND_DATAW *); BOOL FindClose(HANDLE);
int _snwprintf(wchar_t *, size_t, const wchar_t *, ...);
typedef unsigned short USHORT; USHORT RtlCaptureStackBackTrace(DWORD, DWORD, void **, DWORD *);
#endif
