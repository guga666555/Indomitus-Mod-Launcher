/* Minimal fake Win32 header: ONLY for the host-side syntax check of gem_previewscene.c
   (clang -fsyntax-only -target x86_64-windows-gnu -I tests/stub). Never used for the real build. */
#ifndef STUB_WINDOWS_H
#define STUB_WINDOWS_H
#include <stdint.h>
#include <stddef.h>
#include <wchar.h>
#include <stdio.h>

#define WINAPI
#define MAX_PATH 260
#define TRUE 1
#define FALSE 0
#define CP_UTF8 65001
#define DLL_PROCESS_ATTACH 1
#define PAGE_READWRITE 0x04
#define IMAGE_FIRST_SECTION(nt) ((IMAGE_SECTION_HEADER *)((uint8_t *)&(nt)->OptionalHeader + (nt)->FileHeader.SizeOfOptionalHeader))

typedef int BOOL; typedef unsigned long DWORD; typedef unsigned int UINT; typedef unsigned short WORD;
typedef void *HANDLE; typedef void *HMODULE; typedef void *HINSTANCE; typedef void *LPVOID;
typedef struct { WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds; } SYSTEMTIME;
typedef struct { DWORD VirtualAddress, Size; } IMAGE_DATA_DIRECTORY;
typedef struct { uint16_t e_magic; uint8_t pad[58]; int32_t e_lfanew; } IMAGE_DOS_HEADER;
typedef struct { WORD Machine, NumberOfSections; DWORD TimeDateStamp, PointerToSymbolTable, NumberOfSymbols; WORD SizeOfOptionalHeader, Characteristics; } IMAGE_FILE_HEADER;
typedef struct { WORD Magic; uint8_t rest[110]; IMAGE_DATA_DIRECTORY DataDirectory[16]; } IMAGE_OPTIONAL_HEADER64;
typedef struct { DWORD Signature; IMAGE_FILE_HEADER FileHeader; IMAGE_OPTIONAL_HEADER64 OptionalHeader; } IMAGE_NT_HEADERS64;
typedef struct { uint8_t Name[8]; union { DWORD PhysicalAddress, VirtualSize; } Misc; DWORD VirtualAddress, SizeOfRawData, PointerToRawData, PointerToRelocations, PointerToLinenumbers; WORD NumberOfRelocations, NumberOfLinenumbers; DWORD Characteristics; } IMAGE_SECTION_HEADER;

void GetLocalTime(SYSTEMTIME *); void OutputDebugStringA(const char *);
int MultiByteToWideChar(UINT, DWORD, const char *, int, wchar_t *, int);
int WideCharToMultiByte(UINT, DWORD, const wchar_t *, int, char *, int, const char *, int *);
DWORD GetPrivateProfileStringW(const wchar_t *, const wchar_t *, const wchar_t *, wchar_t *, DWORD, const wchar_t *);
UINT GetPrivateProfileIntW(const wchar_t *, const wchar_t *, int, const wchar_t *);
HMODULE GetModuleHandleW(const wchar_t *); DWORD GetModuleFileNameW(HMODULE, wchar_t *, DWORD);
BOOL VirtualProtect(void *, size_t, DWORD, DWORD *); void *InterlockedExchangePointer(void *volatile *, void *);
int _wcsnicmp(const wchar_t *, const wchar_t *, size_t); FILE *_wfopen(const wchar_t *, const wchar_t *);
BOOL AllocConsole(void); BOOL SetConsoleTitleA(const char *); BOOL DisableThreadLibraryCalls(HMODULE);
short GetAsyncKeyState(int); int _stricmp(const char *, const char *); unsigned long long GetTickCount64(void);
#define MEM_COMMIT 0x1000
#define PAGE_NOACCESS 0x01
#define PAGE_READONLY 0x02
#define PAGE_WRITECOPY 0x08
#define PAGE_EXECUTE_READ 0x20
#define PAGE_EXECUTE_READWRITE 0x40
#define PAGE_EXECUTE_WRITECOPY 0x80
#define PAGE_GUARD 0x100
typedef struct { void *BaseAddress, *AllocationBase; DWORD AllocationProtect; WORD PartitionId; size_t RegionSize; DWORD State, Protect, Type; } MEMORY_BASIC_INFORMATION;
size_t VirtualQuery(const void *, MEMORY_BASIC_INFORMATION *, size_t);
#endif
