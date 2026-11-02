/*
 * chain_test.exe <call_to_arms*.exe>  -  offline check of the three GEM hook dlls against one exe.
 *
 * Maps the exe's headers + sections at its preferred base (vftables hold absolute pointers), then for every one of
 * the 6 load orders: runs each dll's finders and installs its hooks on the mapped copy, in that order, so every
 * dll after the first has to find its engine code in functions its siblings already patched. Afterwards it walks
 * every patched function's stub chain and checks that it ends in the exe's original bytes and holds one hook per dll
 * that patched it (SpawnBullet: all three). Nothing is executed. Prints ALL OK / FAILED.
 */
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

int converge_selftest(uint8_t *base, void ***hooked, int *nhooked);
int overcharge_selftest(uint8_t *base, void ***hooked, int *nhooked);
int guidance_selftest(uint8_t *base, void ***hooked, int *nhooked);

typedef int (*SelftestFn)(uint8_t *, void ***, int *);
static const struct { const char *name; SelftestFn fn; } k_dlls[3] = {
    { "converge", converge_selftest }, { "overcharge", overcharge_selftest }, { "guidance", guidance_selftest } };

static uint8_t *g_file, *g_img, *g_pristine;
static uint32_t g_pe, g_imgsz;

static void map_into(uint8_t *dst)
{
    uint16_t nsec = *(uint16_t *)(g_file + g_pe + 6), opt = *(uint16_t *)(g_file + g_pe + 20);
    uint32_t hdrs = *(uint32_t *)(g_file + g_pe + 24 + 60);
    memset(dst, 0, g_imgsz);
    memcpy(dst, g_file, hdrs);
    for (int i = 0; i < nsec; i++) {
        uint8_t *sh = g_file + g_pe + 24 + opt + 40 * i;
        uint32_t vs = *(uint32_t *)(sh + 8), va = *(uint32_t *)(sh + 12), rs = *(uint32_t *)(sh + 16), raw = *(uint32_t *)(sh + 20);
        memcpy(dst + va, g_file + raw, rs < vs ? rs : vs);
    }
}

static int is_absjmp(const uint8_t *p) { return p[0] == 0xFF && p[1] == 0x25 && *(const uint32_t *)(p + 2) == 0; }

/* follow f's stub chain: number of GEM hooks, and whether the bytes under them are the exe's original ones */
static int walk(uint8_t *f, int *depth)
{
    uint8_t buf[32]; size_t maxn = 0;
    memcpy(buf, f, 32);
    *depth = 0;
    while (is_absjmp(buf)) {
        const uint8_t *stub = *(const uint8_t **)(buf + 6);
        if (memcmp(stub + 14, "GEMHOOK1", 8)) { printf("    stub without magic\n"); return 0; }
        size_t n = stub[22];
        if (!is_absjmp(stub) || !*(const uint64_t *)(stub + 6)) { printf("    stub has no hook jump\n"); return 0; }
        if (memcmp(stub + 64, stub + 23, n)) { printf("    trampoline does not start with the saved bytes\n"); return 0; }
        if (*(const uint64_t *)(stub + 64 + n + 6) != (uint64_t)(f + n)) { printf("    trampoline does not jump back to f+%zu\n", n); return 0; }
        memcpy(buf, stub + 23, n);
        if (n > maxn) maxn = n;
        (*depth)++;
    }
    return !memcmp(buf, g_pristine + (f - g_img), maxn ? maxn : 14);
}

int main(int argc, char **argv)
{
    if (argc < 2) { printf("usage: chain_test <call_to_arms*.exe>\n"); return 2; }
    FILE *fp = fopen(argv[1], "rb"); if (!fp) { printf("cannot open %s\n", argv[1]); return 2; }
    fseek(fp, 0, SEEK_END); long n = ftell(fp); fseek(fp, 0, SEEK_SET);
    g_file = malloc(n); if (fread(g_file, 1, n, fp) != (size_t)n) { printf("read failed\n"); return 2; } fclose(fp);
    g_pe = *(uint32_t *)(g_file + 0x3c);
    g_imgsz = *(uint32_t *)(g_file + g_pe + 24 + 56);
    uint64_t base = *(uint64_t *)(g_file + g_pe + 24 + 24);
    g_img = VirtualAlloc((void *)base, g_imgsz, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (g_img != (uint8_t *)base) { printf("cannot map at %llX\n", (unsigned long long)base); return 2; }
    g_pristine = VirtualAlloc(NULL, g_imgsz, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    map_into(g_pristine);
    printf("%s timestamp %08X\n", argv[1], *(uint32_t *)(g_file + g_pe + 8));

    static const int orders[6][3] = { {0,1,2}, {0,2,1}, {1,0,2}, {1,2,0}, {2,0,1}, {2,1,0} };
    int allOk = 1;
    for (int o = 0; o < 6; o++) {
        map_into(g_img);
        printf("\n=== load order: %s, %s, %s\n", k_dlls[orders[o][0]].name, k_dlls[orders[o][1]].name, k_dlls[orders[o][2]].name);
        void *targets[48]; int counts[48], nt = 0, ok = 1;
        for (int k = 0; k < 3; k++) {
            void **h; int nh;
            int r = k_dlls[orders[o][k]].fn(g_img, &h, &nh);
            printf("  -> %s: %s, %d hook(s)\n", k_dlls[orders[o][k]].name, r ? "ON" : "OFF", nh);
            if (!r) ok = 0;
            for (int i = 0; i < nh; i++) {
                int j = 0; while (j < nt && targets[j] != h[i]) j++;
                if (j == nt) { targets[nt] = h[i]; counts[nt++] = 0; }
                counts[j]++;
            }
        }
        for (int j = 0; j < nt; j++) {
            int depth, good = walk((uint8_t *)targets[j], &depth);
            printf("  chain exe+0x%llX: %d hook(s), %s\n", (unsigned long long)((uint8_t *)targets[j] - g_img), depth,
                   good && depth == counts[j] ? "original bytes intact" : "BROKEN");
            if (!good || depth != counts[j]) ok = 0;
        }
        printf("  order %d: %s\n", o + 1, ok ? "OK" : "FAILED");
        if (!ok) allOk = 0;
    }
    printf(allOk ? "\nALL OK\n" : "\nFAILED\n");
    return !allOk;
}
