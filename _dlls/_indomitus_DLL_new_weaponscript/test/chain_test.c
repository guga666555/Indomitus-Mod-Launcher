/*
 * chain_test <call_to_arms*.exe>  -  offline check of the three GEM WeaponScript modules against one exe.
 *
 * Maps the exe's headers + sections at its preferred base (vftables hold absolute pointers), then for every one of
 * the 6 init orders: runs each module's finders and installs its hooks on the mapped copy, in that order, so every
 * module after the first has to find its engine code in functions its siblings already patched. Afterwards it walks
 * every patched function's stub chain and checks that it ends in the exe's original bytes and holds one hook per
 * module that patched it (SpawnBullet: all three).
 *
 * Then the same 6 orders again on a copy that looks like the Indomitus Launcher payload got there first:
 * a BreedTags / NoProne style 14-byte jump (no GEMHOOK1 stub) over the entry of eBulletShooter::Launch, SpawnBullet,
 * Bullet::Launch and Bullet::Quant, and eBulletShooter's vftable slot 11 swapped to a pointer outside the exe
 * (ScriptExt style). Every module must still be ON, and every chain must end in that foreign jump.
 *
 * Nothing is executed. Prints ALL OK / FAILED. Builds on Windows (build.bat test) and Linux (run_test.sh).
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000     /* glibc < 2.28; older kernels ignore it and the address check below catches that */
#endif
#endif

typedef struct { uint8_t *launch, *spawnBullet, *bulletLaunch, *bulletQuant, *launchVt; } GemFound;   /* = common\gem_common.h */
typedef int (*SelftestFn)(uint8_t *base, const char *exePath, void ***hooked, int *nhooked, GemFound *found);
int converge_selftest(uint8_t *base, const char *exePath, void ***hooked, int *nhooked, GemFound *found);
int overcharge_selftest(uint8_t *base, const char *exePath, void ***hooked, int *nhooked, GemFound *found);
int guidance_selftest(uint8_t *base, const char *exePath, void ***hooked, int *nhooked, GemFound *found);

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

/* the bytes a foreign (non-GEM) hook writes: FF 25 00000000 <target> + NOP pad to 16 */
static uint8_t g_foreignStub[64];
static void foreign_patch(uint8_t *f)
{
    uint8_t p[16]; memset(p, 0x90, sizeof p);
    p[0] = 0xFF; p[1] = 0x25; *(uint32_t *)(p + 2) = 0; *(uint64_t *)(p + 6) = (uint64_t)g_foreignStub;
    memcpy(f, p, sizeof p);
}
static int is_foreign_jmp(const uint8_t *p) { return is_absjmp(p) && *(const uint64_t *)(p + 6) == (uint64_t)g_foreignStub; }

/* follow f's stub chain: number of GEM hooks, and whether the bytes under them are the exe's original ones
   (or, in the foreign scenario, the foreign jump the test planted) */
static int walk(uint8_t *f, int *depth, int foreign)
{
    uint8_t buf[32]; size_t maxn = 0;
    memcpy(buf, f, 32);
    *depth = 0;
    while (is_absjmp(buf) && !is_foreign_jmp(buf)) {
        const uint8_t *stub = *(const uint8_t **)(buf + 6);
        if (memcmp(stub + 14, "GEMHOOK1", 8)) { printf("    stub without magic\n"); return 0; }
        size_t n = stub[22];
        if (!is_absjmp(stub) || !*(const uint64_t *)(stub + 6)) { printf("    stub has no hook jump\n"); return 0; }
        if (memcmp(stub + 64, stub + 23, n)) { printf("    trampoline does not start with the saved bytes\n"); return 0; }
        if (*(const uint64_t *)(stub + 64 + n + 6) != (uint64_t)(f + n)) { printf("    trampoline does not jump back to f+%u\n", (unsigned)n); return 0; }
        memcpy(buf, stub + 23, n);
        if (n > maxn) maxn = n;
        (*depth)++;
    }
    if (foreign) {
        if (!is_foreign_jmp(buf)) { printf("    chain does not end in the foreign jump\n"); return 0; }
        return 1;
    }
    return !memcmp(buf, g_pristine + (f - g_img), maxn ? maxn : 14);
}

static void *alloc_at(uint64_t base, size_t size)
{
#ifdef _WIN32
    return VirtualAlloc((void *)base, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
    void *p = mmap((void *)base, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    return p == MAP_FAILED ? NULL : p;
#endif
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
    g_img = alloc_at(base, g_imgsz);
    if (g_img != (uint8_t *)base) { printf("cannot map at %llX\n", (unsigned long long)base); return 2; }
    g_pristine = malloc(g_imgsz);
    map_into(g_pristine);
    printf("%s timestamp %08X\n", argv[1], *(uint32_t *)(g_file + g_pe + 8));

    static const int orders[6][3] = { {0,1,2}, {0,2,1}, {1,0,2}, {1,2,0}, {2,0,1}, {2,1,0} };
    int allOk = 1;
    GemFound found0; memset(&found0, 0, sizeof found0);
    for (int scenario = 0; scenario < 2; scenario++) {
        int foreign = scenario == 1;
        if (foreign) {
            if (!found0.launch || !found0.spawnBullet || !found0.bulletLaunch || !found0.bulletQuant || !found0.launchVt) {
                printf("\n=== foreign-hook scenario skipped: the clean pass did not find every function\n"); allOk = 0; break;
            }
            printf("\n=== scenario 2: entries of Launch, SpawnBullet, Bullet::Launch, Bullet::Quant carry another dll's 14-byte jump,\n"
                   "    eBulletShooter vftable slot 11 swapped to a pointer outside the exe\n");
        } else printf("\n=== scenario 1: clean exe\n");
        for (int o = 0; o < 6; o++) {
            map_into(g_img);
            if (foreign) {
                foreign_patch(found0.launch); foreign_patch(found0.spawnBullet); foreign_patch(found0.bulletLaunch); foreign_patch(found0.bulletQuant);
                *(uint64_t *)(found0.launchVt + 11 * 8) = 0x7FFE00001000ull;
            }
            printf("\n--- load order: %s, %s, %s\n", k_dlls[orders[o][0]].name, k_dlls[orders[o][1]].name, k_dlls[orders[o][2]].name);
            void *targets[48]; int counts[48], nt = 0, ok = 1;
            for (int k = 0; k < 3; k++) {
                void **h; int nh; GemFound fnd;
                int r = k_dlls[orders[o][k]].fn(g_img, argv[1], &h, &nh, &fnd);
                printf("  -> %s: %s, %d hook(s)\n", k_dlls[orders[o][k]].name, r ? "ON" : "OFF", nh);
                if (!r) ok = 0;
                if (!foreign && orders[o][k] == 2 && r) found0 = fnd;          /* guidance finds all five addresses */
                for (int i = 0; i < nh; i++) {
                    int j = 0; while (j < nt && targets[j] != h[i]) j++;
                    if (j == nt) { targets[nt] = h[i]; counts[nt++] = 0; }
                    counts[j]++;
                }
            }
            if (foreign) {   /* the dlls must have hooked the ORIGINAL functions, not the swapped slot */
                int seen = 0;
                for (int j = 0; j < nt; j++) if (targets[j] == found0.launch) seen = 1;
                if (!seen) { printf("  Launch (from the exe file) was not hooked\n"); ok = 0; }
            }
            for (int j = 0; j < nt; j++) {
                int depth, good = walk((uint8_t *)targets[j], &depth, foreign);
                printf("  chain exe+0x%llX: %d hook(s), %s\n", (unsigned long long)((uint8_t *)targets[j] - g_img), depth,
                       good && depth == counts[j] ? (foreign ? "ends in the foreign jump" : "original bytes intact") : "BROKEN");
                if (!good || depth != counts[j]) ok = 0;
            }
            printf("  order %d: %s\n", o + 1, ok ? "OK" : "FAILED");
            if (!ok) allOk = 0;
        }
    }
    printf(allOk ? "\nALL OK\n" : "\nFAILED\n");
    return !allOk;
}
