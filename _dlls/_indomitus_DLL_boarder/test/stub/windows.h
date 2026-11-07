/* minimal host stub for test/run_host_test.sh (Linux): only what the BF_TEST build uses */
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <wchar.h>
typedef uint8_t BYTE; typedef uint16_t WORD; typedef uint32_t DWORD; typedef int32_t LONG; typedef uint64_t ULONGLONG;
#define MAX_PATH 260
typedef struct { WORD e_magic; WORD e_cblp, e_cp, e_crlc, e_cparhdr, e_minalloc, e_maxalloc, e_ss, e_sp, e_csum, e_ip, e_cs, e_lfarlc, e_ovno, e_res[4], e_oemid, e_oeminfo, e_res2[10]; LONG e_lfanew; } IMAGE_DOS_HEADER;
typedef struct { WORD Machine; WORD NumberOfSections; DWORD TimeDateStamp, PointerToSymbolTable, NumberOfSymbols; WORD SizeOfOptionalHeader, Characteristics; } IMAGE_FILE_HEADER;
typedef struct { WORD Magic; BYTE MajorLinkerVersion, MinorLinkerVersion; DWORD SizeOfCode, SizeOfInitializedData, SizeOfUninitializedData, AddressOfEntryPoint, BaseOfCode; ULONGLONG ImageBase; DWORD SectionAlignment, FileAlignment; WORD v[6]; DWORD Win32VersionValue, SizeOfImage, SizeOfHeaders, CheckSum; /* rest unused */ } IMAGE_OPTIONAL_HEADER64;
typedef struct { DWORD Signature; IMAGE_FILE_HEADER FileHeader; IMAGE_OPTIONAL_HEADER64 OptionalHeader; } IMAGE_NT_HEADERS64;
typedef struct { BYTE Name[8]; union { DWORD PhysicalAddress, VirtualSize; } Misc; DWORD VirtualAddress, SizeOfRawData, PointerToRawData, PointerToRelocations, PointerToLinenumbers; WORD NumberOfRelocations, NumberOfLinenumbers; DWORD Characteristics; } IMAGE_SECTION_HEADER;
#define IMAGE_FIRST_SECTION(nt) ((IMAGE_SECTION_HEADER *)((BYTE *)&(nt)->OptionalHeader + (nt)->FileHeader.SizeOfOptionalHeader))
typedef struct { void *p; } SRWLOCK;
#define SRWLOCK_INIT {0}
#define AcquireSRWLockExclusive(l) ((void)(l))
#define ReleaseSRWLockExclusive(l) ((void)(l))
#define _wfopen(a, b) ((FILE *)0)
