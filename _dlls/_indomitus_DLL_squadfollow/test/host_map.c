/* maps a PE image at its RVAs (RWX, no relocations: the code under test is rip-relative) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "stub/windows.h"
uint8_t *host_map_image(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { printf("%s: open failed\n", path); return NULL; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *file = malloc(n);
    if (fread(file, 1, n, f) != (size_t)n) { fclose(f); return NULL; }
    fclose(f);
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(file + ((IMAGE_DOS_HEADER *)file)->e_lfanew);
    size_t img = nt->OptionalHeader.SizeOfImage;
    uint8_t *m = mmap(NULL, img, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED) { printf("%s: mmap failed\n", path); return NULL; }
    memcpy(m, file, nt->OptionalHeader.SizeOfHeaders);
    IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, s++) {
        size_t sz = s->SizeOfRawData < s->Misc.VirtualSize ? s->SizeOfRawData : s->Misc.VirtualSize;
        if (s->PointerToRawData && sz) memcpy(m + s->VirtualAddress, file + s->PointerToRawData, sz);
    }
    free(file);
    return m;
}
