/* Minimal fake header for host-side syntax checking only. */
#ifndef STUB_TLHELP32_H
#define STUB_TLHELP32_H
#include "windows.h"
#define TH32CS_SNAPTHREAD 4
typedef struct { DWORD dwSize, cntUsage, th32ThreadID, th32OwnerProcessID; long tpBasePri, tpDeltaPri; DWORD dwFlags; } THREADENTRY32;
HANDLE CreateToolhelp32Snapshot(DWORD, DWORD); BOOL Thread32First(HANDLE, THREADENTRY32 *); BOOL Thread32Next(HANDLE, THREADENTRY32 *);
#endif
