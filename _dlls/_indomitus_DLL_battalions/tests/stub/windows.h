/* Minimal fake <windows.h> for host-side compile checks of gem_battalions.c
 * (clang --target=x86_64-windows-gnu -c). Not used by the real build. */
#ifndef STUB_WINDOWS_H
#define STUB_WINDOWS_H
#include <stdint.h>
#include <stddef.h>
#include <wchar.h>

#define WINAPI __stdcall
typedef int BOOL;
typedef unsigned long DWORD;
typedef long LONG;
typedef unsigned int UINT;
typedef unsigned long long ULONGLONG;
typedef void *HANDLE, *HMODULE, *HINSTANCE, *LPVOID;
typedef const wchar_t *LPCWSTR;
typedef uint8_t BYTE;
typedef uint16_t WORD;
typedef void *LPSECURITY_ATTRIBUTES;
typedef struct { LONG v[8]; } CRITICAL_SECTION;
typedef struct { WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds; } SYSTEMTIME;
typedef struct { void *BaseAddress, *AllocationBase; DWORD AllocationProtect; WORD PartitionId; size_t RegionSize; DWORD State, Protect, Type; } MEMORY_BASIC_INFORMATION;

#define TRUE 1
#define FALSE 0
#define MAX_PATH 260
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
#define DLL_PROCESS_ATTACH 1
#define DLL_PROCESS_DETACH 0
#define MEM_COMMIT 0x1000
#define MEM_RESERVE 0x2000
#define PAGE_NOACCESS 1
#define PAGE_READONLY 2
#define PAGE_READWRITE 4
#define PAGE_EXECUTE 0x10
#define PAGE_EXECUTE_READ 0x20
#define PAGE_EXECUTE_READWRITE 0x40
#define PAGE_EXECUTE_WRITECOPY 0x80
#define PAGE_GUARD 0x100
#define HEAP_ZERO_MEMORY 8
#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define FILE_SHARE_READ 1
#define CREATE_ALWAYS 2
#define FILE_ATTRIBUTE_NORMAL 0x80
#define CP_UTF8 65001
#define CP_ACP 0
#define GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS 4
#define GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT 2

#define IMAGE_DOS_SIGNATURE 0x5A4D
#define IMAGE_NT_SIGNATURE 0x4550
#define IMAGE_SCN_MEM_DISCARDABLE 0x02000000
#define IMAGE_SCN_MEM_EXECUTE 0x20000000
#define IMAGE_SCN_MEM_READ 0x40000000
#define IMAGE_SCN_MEM_WRITE 0x80000000
#define IMAGE_DIRECTORY_ENTRY_IMPORT 1
#define IMAGE_ORDINAL_FLAG64 0x8000000000000000ull

typedef struct { WORD e_magic; WORD pad[29]; LONG e_lfanew; } IMAGE_DOS_HEADER;
typedef struct { DWORD VirtualAddress, Size; } IMAGE_DATA_DIRECTORY;
typedef struct { WORD Machine, NumberOfSections; DWORD TimeDateStamp, PointerToSymbolTable, NumberOfSymbols; WORD SizeOfOptionalHeader, Characteristics; } IMAGE_FILE_HEADER;
typedef struct { WORD Magic; BYTE a, b; DWORD c[7]; ULONGLONG ImageBase; DWORD d[2]; WORD e[6]; DWORD f; DWORD SizeOfImage; DWORD g[2]; WORD h[2]; ULONGLONG i[4]; DWORD j[2]; IMAGE_DATA_DIRECTORY DataDirectory[16]; } IMAGE_OPTIONAL_HEADER64;
typedef struct { DWORD Signature; IMAGE_FILE_HEADER FileHeader; IMAGE_OPTIONAL_HEADER64 OptionalHeader; } IMAGE_NT_HEADERS64;
typedef struct { BYTE Name[8]; union { DWORD VirtualSize; } Misc; DWORD VirtualAddress, SizeOfRawData, PointerToRawData, a, b; WORD c, d; DWORD Characteristics; } IMAGE_SECTION_HEADER;
#define IMAGE_FIRST_SECTION(nt) ((IMAGE_SECTION_HEADER *)((uint8_t *)&(nt)->OptionalHeader + (nt)->FileHeader.SizeOfOptionalHeader))
typedef struct { DWORD OriginalFirstThunk, TimeDateStamp, ForwarderChain, Name, FirstThunk; } IMAGE_IMPORT_DESCRIPTOR;
typedef struct { union { ULONGLONG ForwarderString, Function, Ordinal, AddressOfData; } u1; } IMAGE_THUNK_DATA64;
typedef struct { WORD Hint; char Name[1]; } IMAGE_IMPORT_BY_NAME;

HMODULE WINAPI GetModuleHandleW(LPCWSTR);
BOOL WINAPI GetModuleHandleExW(DWORD, LPCWSTR, HMODULE *);
DWORD WINAPI GetModuleFileNameW(HMODULE, wchar_t *, DWORD);
void WINAPI GetLocalTime(SYSTEMTIME *);
DWORD WINAPI GetCurrentThreadId(void);
HANDLE WINAPI GetCurrentProcess(void);
HANDLE WINAPI GetProcessHeap(void);
void *WINAPI HeapAlloc(HANDLE, DWORD, size_t);
BOOL WINAPI HeapFree(HANDLE, DWORD, void *);
void *WINAPI VirtualAlloc(void *, size_t, DWORD, DWORD);
BOOL WINAPI VirtualProtect(void *, size_t, DWORD, DWORD *);
size_t WINAPI VirtualQuery(const void *, MEMORY_BASIC_INFORMATION *, size_t);
BOOL WINAPI FlushInstructionCache(HANDLE, const void *, size_t);
BOOL WINAPI WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
HANDLE WINAPI CreateFileW(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
DWORD WINAPI GetFileAttributesW(LPCWSTR);
BOOL WINAPI CloseHandle(HANDLE);
DWORD WINAPI GetLastError(void);
DWORD WINAPI GetTickCount(void);
void WINAPI Sleep(DWORD);
void WINAPI InitializeCriticalSection(CRITICAL_SECTION *);
void WINAPI EnterCriticalSection(CRITICAL_SECTION *);
void WINAPI LeaveCriticalSection(CRITICAL_SECTION *);
BOOL WINAPI DisableThreadLibraryCalls(HMODULE);
HANDLE WINAPI CreateThread(void *, size_t, DWORD(WINAPI *)(LPVOID), LPVOID, DWORD, DWORD *);
LONG WINAPI InterlockedIncrement(volatile LONG *);
LONG WINAPI InterlockedExchange(volatile LONG *, LONG);
void *WINAPI InterlockedExchangePointer(void *volatile *, void *);
UINT WINAPI GetPrivateProfileIntW(LPCWSTR, LPCWSTR, int, LPCWSTR);
DWORD WINAPI GetPrivateProfileStringA(const char *, const char *, const char *, char *, DWORD, const char *);
int WINAPI WideCharToMultiByte(UINT, DWORD, LPCWSTR, int, char *, int, const char *, BOOL *);
int _snwprintf(wchar_t *, size_t, const wchar_t *, ...);
#endif
