/* Windows stand-in for <sys/mman.h>, only what fx_test (FX_TEST build) uses: an anonymous mapping at a fixed address
 * (the exe's image base). Lets tests/run_offline_check_win.py run the finder check on Windows with zig cc. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#define PROT_READ  1
#define PROT_WRITE 2
#define MAP_PRIVATE 2
#define MAP_ANONYMOUS 0x20
#define MAP_FIXED_NOREPLACE 0x100000
#define MAP_FAILED ((void *)-1)
__declspec(dllimport) void *__stdcall VirtualAlloc(void *addr, size_t size, unsigned long type, unsigned long protect);
static void *mmap(void *addr, size_t len, int prot, int flags, int fd, long off)
{
    (void)prot; (void)flags; (void)fd; (void)off;
    void *p = VirtualAlloc(addr, len, 0x3000 /* MEM_COMMIT | MEM_RESERVE */, 0x04 /* PAGE_READWRITE */);
    return p ? p : MAP_FAILED;
}
