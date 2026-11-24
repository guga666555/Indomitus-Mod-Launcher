/* chase_probe: Windows port of GEM MeleeChase 0.5.4's offline finder test (src/mc/test_main.c, Linux mmap) for the
 * gem_melee merge map. It #includes the READ-ONLY snapshot's core.c and melee.c in FX_TEST mode (exactly the finder
 * code the dll runs) and maps an exe at its preferred image base with VirtualAlloc (the finders compare absolute
 * pointers: vtable slots, RTTI locator pointers), falling back to a mapping elsewhere + base relocations.
 * Then it prints every patch site the dll would write and every byte window / whole-.text hit its finders read.
 *
 * Build: zig cc -target x86_64-windows-gnu -mcpu=baseline -O2 -ffp-contract=off -fno-fast-math -o chase_probe.exe chase_probe.c
 * Run:   chase_probe.exe <call_to_arms*.exe>
 */
#define FX_TEST 1
#include <windows.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#define FX_VERSION "0.5.4"
#define FX_NAME    "GEM MeleeChase"
#include "../chase/mc/core.c"
#include "../chase/mc/melee.c"

static void hexdump(const char *what, const uint8_t *p, int n)
{
    printf("  %-34s exe+0x%llX :", what, EXE_OFF(p));
    for (int i = 0; i < n; i++) printf(" %02X", p[i]);
    printf("\n");
}
static void window(const char *what, const void *start, unsigned len)
{
    printf("  READ %-30s exe+0x%llX .. exe+0x%llX (0x%X bytes)\n", what, EXE_OFF(start), EXE_OFF(start) + len, len);
}

/* known patch sites of the other two modules on 1.069 (start, bytes), from inputs\penfilter_0.1.1\out\selftest.txt and
   inputs\meleestance_0.7.42\build_log.txt / live logs. Lengths are upper bounds (prologue detours <= 16, rel32 = 5). */
typedef struct { uint32_t ts; const char *what; uint32_t at, len; } Other;
static const Other g_other[] = {
    { 0x6AFC6D57, "MS MatchAmmo prologue",        0x92C9C0, 16 }, { 0x6AFC6D57, "PF ctx builder prologue", 0x92C8E0, 16 },
    { 0x6AFC6D57, "PF A volume site",             0x92CC4A, 5 },  { 0x6AFC6D57, "PF A no-volume site",     0x92CBE3, 5 },
    { 0x6AFC6D57, "PF B solver call",             0x92B9CD, 5 },  { 0x6AFC6D57, "PF P try-loop head",      0x92B8BA, 16 },
    { 0x6AFC6D57, "PF X exhausted clear call",    0x92BA09, 5 },
    { 0x6AFC6E14, "MS MatchAmmo prologue",        0x7AADA0, 16 }, { 0x6AFC6E14, "PF ctx builder prologue", 0x7AACC0, 16 },
    { 0x6AFC6E14, "PF A volume site",             0x7AB02A, 5 },  { 0x6AFC6E14, "PF A no-volume site",     0x7AAFC3, 5 },
    { 0x6AFC6E14, "PF B solver call",             0x7A9D0D, 5 },  { 0x6AFC6E14, "PF P try-loop head",      0x7A9BFA, 16 },
    { 0x6AFC6E14, "PF X exhausted clear call",    0x7A9D49, 5 },
    { 0x6AFC6DAF, "MS MatchAmmo prologue",        0x73EE60, 16 }, { 0x6AFC6DAF, "PF ctx builder prologue", 0x73ED80, 16 },
    { 0x6AFC6DAF, "PF A volume site",             0x73F0EA, 5 },  { 0x6AFC6DAF, "PF A no-volume site",     0x73F083, 5 },
    { 0x6AFC6DAF, "PF B solver call",             0x73DE6D, 5 },  { 0x6AFC6DAF, "PF P try-loop head",      0x73DD5A, 16 },
    { 0x6AFC6DAF, "PF X exhausted clear call",    0x73DEA9, 5 },
};
#define MAXW 64
static struct { const char *what; uint64_t lo, hi; } g_win[MAXW]; static int g_nwin;
static void addw(const char *what, const void *start, unsigned len)
{
    window(what, start, len);
    if (g_nwin < MAXW) { g_win[g_nwin].what = what; g_win[g_nwin].lo = EXE_OFF(start); g_win[g_nwin].hi = EXE_OFF(start) + len; g_nwin++; }
}

static int map_image(const char *path, uint32_t *ts)
{
    FILE *f = fopen(path, "rb"); if (!f) { printf("cannot open %s\n", path); return 0; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *file = malloc(n); if (!file || fread(file, 1, n, f) != (size_t)n) { fclose(f); return 0; } fclose(f);
    uint32_t pe = *(uint32_t *)(file + 0x3c);
    uint16_t nsec = *(uint16_t *)(file + pe + 6), opt = *(uint16_t *)(file + pe + 20);
    uint64_t base = *(uint64_t *)(file + pe + 24 + 24);
    uint32_t imgsz = *(uint32_t *)(file + pe + 24 + 56), hdrsz = *(uint32_t *)(file + pe + 24 + 60);
    *ts = *(uint32_t *)(file + pe + 8);
    g_base = VirtualAlloc((void *)(uintptr_t)base, imgsz, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    int relocated = 0;
    if (g_base != (uint8_t *)(uintptr_t)base) {
        if (g_base) VirtualFree(g_base, 0, MEM_RELEASE);
        g_base = VirtualAlloc(NULL, imgsz, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!g_base) { printf("VirtualAlloc failed\n"); return 0; }
        relocated = 1;
    }
    memcpy(g_base, file, hdrsz);
    for (int i = 0; i < nsec; i++) {
        uint8_t *sh = file + pe + 24 + opt + 40 * i;
        uint32_t vs = *(uint32_t *)(sh + 8), va = *(uint32_t *)(sh + 12), rs = *(uint32_t *)(sh + 16), raw = *(uint32_t *)(sh + 20);
        memcpy(g_base + va, file + raw, rs < vs ? rs : vs);
        Sec s = { g_base + va, vs };
        if (!strncmp((char *)sh, ".text", 8)) g_text = s; else if (!strncmp((char *)sh, ".rdata", 8)) g_rdata = s;
        else if (!strncmp((char *)sh, ".data", 8)) g_data = s; else if (!strncmp((char *)sh, ".pdata", 8)) g_pdata = s;
    }
    if (relocated) {                                   /* apply IMAGE_REL_BASED_DIR64 entries */
        uint32_t rrva = *(uint32_t *)(file + pe + 24 + 112 + 8 * 5), rsz = *(uint32_t *)(file + pe + 24 + 112 + 8 * 5 + 4);
        int64_t delta = (int64_t)((uint64_t)(uintptr_t)g_base - base);
        for (uint8_t *b = g_base + rrva; b < g_base + rrva + rsz; ) {
            uint32_t page = *(uint32_t *)b, sz = *(uint32_t *)(b + 4);
            if (!sz) break;
            for (uint16_t *e = (uint16_t *)(b + 8); (uint8_t *)e < b + sz; e++)
                if ((*e >> 12) == 10) *(uint64_t *)(g_base + page + (*e & 0xFFF)) += delta;
            b += sz;
        }
    }
    printf("%s timestamp %08X image base %llX mapped at %p%s\n", path, *ts, (unsigned long long)base, (void *)g_base,
           relocated ? " (relocated)" : " (preferred base)");
    free(file);
    return 1;
}

int test_chase_main(int argc, char **argv)
{
    if (argc < 2) { printf("usage: chase_probe <call_to_arms*.exe>\n"); return 2; }
    uint32_t ts = 0;
    if (!map_image(argv[1], &ts)) return 2;
    int ok = 1, r;
    printf("-- finders (same order as test_main.c) --\n");
    r = find_weapon_layout(); printf("[find_weapon_layout %d]\n", r); ok &= r;
    r = find_weapons();       printf("[find_weapons %d]\n", r);       ok &= r;
    r = find_vehicle();       printf("[find_vehicle %d]\n", r);       ok &= r;
    r = find_priority();      printf("[find_priority %d]\n", r);      ok &= r;
    r = find_run_mode();      printf("[find_run_mode %d]\n", r);      ok &= r;
    r = find_hold();          printf("[find_hold %d]\n", r);          ok &= r;
    r = find_entity_tags();   printf("[find_entity_tags %d]\n", r);   ok &= r;
    printf("smooth re-path helper: %s (exe+0x%llX)\n", g_finishSub ? "found" : "not found", g_finishSub ? EXE_OFF(g_finishSub) : 0ULL);
    printf(ok ? "ALL FOUND\n" : "FAILED\n");
    if (!ok) return 1;

    printf("-- patch sites the dll writes (install_vehicle / install_hold) --\n");
    printf("  VT  eVS_Root slot 3 (tick)          slot exe+0x%llX = exe+0x%llX\n", EXE_OFF(&g_vtVsRoot[3]), EXE_OFF(g_vtVsRoot[3]));
    printf("  VT  eMoveTask slot 0 (deleting dtor) slot exe+0x%llX = exe+0x%llX\n", EXE_OFF(&g_vtMoveTask[0]), EXE_OFF(g_vtMoveTask[0]));
    printf("  VT  eMoveTask slot 19 (Update)       slot exe+0x%llX = exe+0x%llX\n", EXE_OFF(&g_vtMoveTask[19]), EXE_OFF(g_vtMoveTask[19]));
    hexdump("CALL approach -> eMoveTask(circle)", g_apSite, 5);
    printf("       original target exe+0x%llX\n", EXE_OFF(rel32_target(g_apSite, 5)));
    hexdump(g_evalJump[0] == 0xE9 ? "JMP  target evaluation (tail jmp)" : "CALL target evaluation", g_evalJump, 5);
    printf("       original target exe+0x%llX, caller function exe+0x%llX\n", EXE_OFF(rel32_target(g_evalJump, 5)), (unsigned long long)func_of(g_evalJump));
    for (int i = 0; i < g_stSiteN; i++) {
        char w[40]; snprintf(w, sizeof w, "CALL Weapon::SetTarget site %d", i);
        hexdump(w, g_stSites[i], 5);
    }
    printf("  (no prologue detours: install_jmp5 is never called)\n");

    printf("-- engine functions called (not patched) --\n");
    printf("  GetCurrentWeapon exe+0x%llX (located; the vehicle code walks the list itself)\n", EXE_OFF(g_getWeapon));
    printf("  Tag::Tag(const char*) exe+0x%llX (ids_ready)\n", EXE_OFF(g_tagCtorCStr));
    printf("  brain const getter exe+0x%llX (located only)\n", EXE_OFF(g_constGet));
    printf("  weapon range exe+0x%llX, SetTarget exe+0x%llX, clear target exe+0x%llX, eMoveTask FinishSub exe+0x%llX, eMoveTask(circle) exe+0x%llX\n",
           EXE_OFF(g_weaponRange), EXE_OFF(g_setTarget), EXE_OFF(g_clearTarget), EXE_OFF(g_finishSub), EXE_OFF(g_moveCircle));
    printf("  sim clock exe+0x%llX (dword)\n", EXE_OFF((void *)g_simTime));

    printf("-- offsets --\n");
    printf("  weapon+0x%X item, item tags +0x%X..+0x%X, entity+0x%X weaponry, list +0x%X..+0x%X, entity+0x%X def, def props +0x%X..+0x%X\n",
           o_weapItem, o_itemTagBeg, o_itemTagEnd, o_weaponry, o_wlistBeg, o_wlistEnd, o_entDef, o_defPropBeg, o_defPropEnd);
    printf("  state+0x%X brain, brain+0x%X entity, brain+0x%X mode, brain+0x%X records, brain+0x%X AI order, brain+0x%X approach range\n",
           o_vStateBrain, o_vBrainEntity, o_vMode, o_vRecBeg, o_aiOrder, o_approach);
    printf("  record+0x%X/+0x%X pos, record+0x%X entity, weapon+0x%X owner, entity+0x%X chassis, chassis vt +0x%X SetMovementMode, entity+0x%X tags\n",
           o_recX, o_recY, o_recEnt, o_weapOwner, o_chassis, o_modeSlot, o_entTags);
    printf("  human (located only): fight distance +0x%X, state->brain +0x%X, brain->entity +0x%X, eOutdoorsBattle slots", o_fightOff, o_hStateBrain, o_hBrainEntity);
    for (int i = 0; i < g_hSlotN; i++) printf(" %d", g_hSlot[i]);
    printf("\n");

    printf("-- byte windows the finders read (start .. end) --\n");
    uint8_t *tick = (uint8_t *)g_vtVsRoot[3];
    addw("eVS_Root tick head (vehicle)", tick, 0xC0);
    addw("eVS_Root tick (recpos/priority)", tick, 0x800);
    addw("approach builder head", g_moveCircle, 0x40);
    addw("approach site + AI order cmp", g_apSite, 0x20);
    addw("eMoveTask dtor head", g_vtMoveTask[0], 0x20);
    addw("eMoveTask Update head", g_vtMoveTask[19], 0xA0);
    {
        void **vtSingle = find_vtable_rtti(".?AVeSingleOrder@@");
        addw("eSingleOrder::Tick head (clock)", vtSingle[3], 0x40);
        void **vtMc = find_vtable_rtti(".?AUeMovementChange@Order@@");
        addw("eMovementChange slot 19 head", vtMc[19], 0x80);
    }
    addw("target evaluation", g_evalFn, 0x1400);
    addw("Weapon::SetTarget head", g_setTarget, 0x90);
    addw("GetCurrentWeapon head", g_getWeapon, 0x40);
    for (int i = 0; i < g_hSlotN; i++) addw("eOutdoorsBattle slot head", g_vtBattle[g_hSlot[i]], 0x80);
    addw("fight/distance state reader", g_fdSiteState - 0x40, 0x80);
    addw("fight/distance eAttack reader", g_fdSiteAttack - 0x40, 0x80);

    printf("-- whole-.text scan hits (a patch over one of these changes a finder result) --\n");
    {   /* find_weapon_layout */
        static const uint8_t pat[] = { 0x48, 0x8B, 0x80 };
        for (uint8_t *p = g_text.p; (p = find_bytes(p, g_text.p + g_text.n - p - 24, pat, 3)); p++) {
            if (p[5] || p[6] || p[7] != 0x48 || p[8] != 0x8B || p[9] != 0xB8 || p[14] != 0x48 || p[15] != 0x8B || p[16] != 0x98) continue;
            uint32_t item = *(uint32_t *)(p + 3), end = *(uint32_t *)(p + 10), beg = *(uint32_t *)(p + 17);
            if (item > 0x200 || end != beg + 8 || beg < 0x400 || beg > 0x4000) continue;
            addw("weapon layout hit (21 bytes)", p, 21);
        }
    }
    for (int k = 0; k < 4; k++) { uint8_t *l = find_lea_to(g_fdStr, k); if (!l) break; addw("lea fight/distance", l, 7); }
    {
        uint8_t reset[10] = { 0xC7, 0x81 }; memcpy(reset + 2, &o_approach, 4); reset[6] = 0; reset[7] = 0; reset[8] = 0x80; reset[9] = 0xBF;
        int n = 0;
        for (uint8_t *p = g_text.p; (p = find_bytes(p, g_text.p + g_text.n - p, reset, 10)); p++) { addw("approach reset (first used)", p, 10); n++; }
        printf("  approach reset pattern: %d hit(s) in .text (find_priority / find_hold take the first)\n", n);
        static const uint8_t ap[] = { 0x48, 0xB8, 'a', 'p', 'p', 'r', 'o', 'a', 'c', 'h' };
        for (uint8_t *p = g_text.p; (p = find_bytes(p, g_text.p + g_text.n - p, ap, sizeof ap)); p++)
            if (func_of(p) == (uint32_t)(tick - g_base)) addw("movabs 'approach' in tick", p, 10);
        int getters = 0;
        for (uint8_t *p = g_text.p; (p = find_bytes(p, g_text.p + g_text.n - p - 8, (const uint8_t *)"\x8B\x05", 2)); p++)
            if (p[6] == 0xC3 && rel32_target(p, 6) == (uint8_t *)g_simTime) { if (!getters) addw("clock getter (first)", p, 7); getters++; }
        printf("  clock getters: %d\n", getters);
    }
    int callers = 0;
    for (uint8_t *p = g_text.p; p + 5 < g_text.p + g_text.n; p++)
        if ((p[0] == 0xE8 || p[0] == 0xE9) && rel32_target(p, 5) == g_evalFn) callers++;
    printf("  callers of the target evaluation (must be 1): %d\n", callers);

    printf("-- overlap with the other modules' known patch sites on this exe --\n");
    int any = 0, nother = 0;
    for (size_t k = 0; k < sizeof g_other / sizeof g_other[0]; k++) {
        if (g_other[k].ts != ts) continue;
        nother++;
        for (int w = 0; w < g_nwin; w++)
            if (g_other[k].at < g_win[w].hi && g_other[k].at + g_other[k].len > g_win[w].lo) {
                printf("  OVERLAP: %s exe+0x%X (+%u) with %s exe+0x%llX..0x%llX\n", g_other[k].what, g_other[k].at, g_other[k].len,
                       g_win[w].what, (unsigned long long)g_win[w].lo, (unsigned long long)g_win[w].hi);
                any = 1;
            }
    }
    printf("  %s (%d known sites checked)\n", any ? "overlaps listed above" : "none", nother);
    return 0;
}
#ifdef TEST_CHASE_STANDALONE
int main(int argc, char **argv) { return test_chase_main(argc, argv); }
#endif
