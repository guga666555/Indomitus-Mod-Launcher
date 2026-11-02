/*
 * GEM BombBay 0.7  -  Call to Arms: Gates of Hell 1.067 (GEM engine)  -  Indomitus Launcher payload DLL
 *
 * Bomb-bay airstrikes. A conquest / MP roster {action "airstrike:<name>"} normally flies the plane over the
 * click and eBombDropOrder spawns ONE entity <name> there. With this dll a roster line
 *   {action "airstrike:<BombAction>[_<N>ms][_<N>m]"}            (BombAction = "marauder_bomb" in the ini)
 * makes the plane drop its OWN loaded bombs from its bomb place(s) ("bomb1", ...) through the engine's
 * eBombShooter::Launch: staggered N ms apart, carrying the plane's speed, released early so the stick lands
 * centred on the click, flying at N m. Every other airstrike name stays vanilla.
 *
 * 0.7 = the bomb-bay feature of GEM Converge 0.6 (gem_converge.c, -DCV_BOMBBAY_ONLY) made into a plain
 * payload DLL for the Indomitus Launcher:
 *   - no VCOMP140.dll proxy any more: the launcher injects gem_bombbay.dll itself (LoadLibrary before the
 *     game's entry point), so DllMain just installs the hooks. Ini + log live next to the dll (payload dir).
 *   - the weapon item / magazine finders (eAmmo::GetItem slot, item filling offsets) are run: the 0.6
 *     standalone build never called them, so the loaded round count read as -1 and no bay run could start.
 *   - the GetVelocity hook only answers for the plane that is releasing (other entities get the original).
 *   - BombFallback removed (it was read and never used; a bay-action name is no entity, nothing to spawn).
 *   - trampolines RX, ini section [bombbay], BB_TEST offline finder check (build.bat test).
 *   - review/cleanup: the eWeaponFire command, eTarget and Tags-ctor finders (found but never used) are gone, so they
 *     can no longer turn the feature OFF; find_launch shares find_vtable; a run / seen-order entry that stopped ticking
 *     for 5 s is dropped (plane shot down before the click would otherwise leave a stale run that a reused entity
 *     address could pick up); the shooter is re-validated (vtable + back pointer) and the magazine re-read before every
 *     release; a finished run is forgotten once the engine's drop step has passed.
 *
 * Engine side (1.067, everything found at run time by RTTI / byte patterns, nothing hard-coded):
 *   Order::eAirstrike = sequence MoveTo(click) -> eBombDropOrder(name) -> MoveTo(exit) -> destroy.
 *   Hooks: eAirstrike vtable slot 3 (tick) and eBombDropOrder vtable slot 19 (Execute) are vtable-slot
 *   swaps; GetVelocity(entity, float3*) and the two StrikeSetup functions (altitude) are 14-byte inline
 *   jmp hooks on their entry. All patching happens in DllMain, before any engine code runs.
 *
 * Payload coexistence: the launcher loads payload DLLs in alphabetical order, so gem_bombbay.dll runs
 * its finders FIRST, on a pristine image. It leaves these changes behind for the DLLs after it:
 *   .rdata  eAirstrike vtable [3], eBombDropOrder vtable [19]           (slot swaps)
 *   .text   GetVelocity entry, StrikeSetup A entry, StrikeSetup B entry   (FF 25 jmp, 14 bytes + nops)
 * The log lists the exe+offset of every patched address.
 *
 * Build: build.bat  (zig cc from the launcher's zig\, else zig on PATH, else python -m ziglang)
 *        build.bat test  ->  dist\bb_test.exe <call_to_arms*.exe>   runs every finder against the exe file.
 *        build.bat payload  ->  copies dll + ini into ..\..\launcher\payload\ (then rebuild the launcher).
 */
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
#include <math.h>

#define BB_VERSION "0.7"
#define BB_NAME    "GEM BombBay"
#define BB_INI     L"\\gem_bombbay.ini"
#define BB_LOG     L"\\gem_bombbay.log"
#define BB_SECT    L"bombbay"

/* ------------------------------------------------------------------------------------ */
/* logging / ini                                                                        */
/* ------------------------------------------------------------------------------------ */
#ifdef BB_TEST
static void logf_(const char *fmt, ...){ va_list ap; va_start(ap,fmt); vprintf(fmt,ap); va_end(ap); printf("\n"); }
#else
static int g_verbose = 1;
static FILE *g_log;
static int g_console;
static wchar_t g_dir[MAX_PATH];
static CRITICAL_SECTION g_logLock;

static void logf_(const char *fmt, ...)
{
    char buf[1024];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    SYSTEMTIME st; GetLocalTime(&st);
    char line[1100];
    snprintf(line, sizeof line, "%02d:%02d:%02d [" BB_NAME "] %s\n", st.wHour, st.wMinute, st.wSecond, buf);
    EnterCriticalSection(&g_logLock);
    if (g_log) { fputs(line, g_log); fflush(g_log); }
    if (g_console) { fputs(line, stdout); fflush(stdout); }
    LeaveCriticalSection(&g_logLock);
    OutputDebugStringA(line);
}

static void ini_str(const char *key, const char *def, char *out, size_t n)
{
    wchar_t ini[MAX_PATH]; wcscpy(ini, g_dir); wcscat(ini, BB_INI);
    wchar_t wkey[64], wdef[128], wout[256]; MultiByteToWideChar(CP_ACP, 0, key, -1, wkey, 64); MultiByteToWideChar(CP_ACP, 0, def, -1, wdef, 128);
    GetPrivateProfileStringW(BB_SECT, wkey, wdef, wout, 256, ini);
    WideCharToMultiByte(CP_ACP, 0, wout, -1, out, (int)n, NULL, NULL);
}

static int ini_int(const char *key, int def)
{
    wchar_t ini[MAX_PATH]; wcscpy(ini, g_dir); wcscat(ini, BB_INI);
    wchar_t wkey[64]; MultiByteToWideChar(CP_ACP, 0, key, -1, wkey, 64);
    return (int)GetPrivateProfileIntW(BB_SECT, wkey, def, ini);
}
#endif /* BB_TEST */

/* ------------------------------------------------------------------------------------ */
/* PE helpers                                                                           */
/* ------------------------------------------------------------------------------------ */
typedef struct { uint8_t *p; size_t n; } Sec;
static Sec g_text, g_rdata, g_data;
static uint8_t *g_base;

#ifndef BB_TEST
static uint32_t g_timestamp;
static int get_sections(void)
{
    g_base = (uint8_t *)GetModuleHandleW(NULL);
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)g_base;
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(g_base + dos->e_lfanew);
    g_timestamp = nt->FileHeader.TimeDateStamp;
    IMAGE_SECTION_HEADER *sh = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        Sec s = { g_base + sh[i].VirtualAddress, sh[i].Misc.VirtualSize };
        if (!strncmp((char *)sh[i].Name, ".text", 8)) g_text = s;
        else if (!strncmp((char *)sh[i].Name, ".rdata", 8)) g_rdata = s;
        else if (!strncmp((char *)sh[i].Name, ".data", 8)) g_data = s;
    }
    return g_text.p && g_rdata.p && g_data.p;
}
#endif

static int in_text(const void *a) { return (uint8_t *)a >= g_text.p && (uint8_t *)a < g_text.p + g_text.n; }
static int in_rdata(const void *a) { return (uint8_t *)a >= g_rdata.p && (uint8_t *)a < g_rdata.p + g_rdata.n; }

static uint8_t *find_bytes(uint8_t *p, size_t n, const uint8_t *pat, size_t m)
{
    if (m > n) return NULL;
    for (size_t i = 0; i + m <= n; i++)
        if (p[i] == pat[0] && !memcmp(p + i, pat, m)) return p + i;
    return NULL;
}

/* a whole NUL-terminated string in a section (preceded by NUL, so not a suffix of a longer one) */
static uint8_t *find_cstr_in(Sec sec, const char *s)
{
    size_t m = strlen(s) + 1;
    for (uint8_t *p = sec.p; (p = find_bytes(p, sec.p + sec.n - p, (const uint8_t *)s, m)); p++)
        if (p == sec.p || p[-1] == 0) return p;
    return NULL;
}
static uint8_t *find_cstr(const char *s) { return find_cstr_in(g_rdata, s); }

/* k-th RIP-relative 'lea reg,[rip+disp]' in .text whose target == want; modrm < 0 = any register */
static uint8_t *find_lea_to(uint8_t *from, size_t n, const void *want, int modrm, int which)
{
    int k = 0;
    for (size_t i = 0; i + 7 <= n; i++) {
        uint8_t r = from[i];
        if ((r & 0xF8) != 0x48 || from[i + 1] != 0x8D || (from[i + 2] & 0xC7) != 0x05) continue;
        if (modrm >= 0 && from[i + 2] != modrm) continue;
        uint8_t *t = from + i + 7 + *(int32_t *)(from + i + 3);
        if (t == want) { if (k++ == which) return from + i; }
    }
    return NULL;
}

/* k-th 'call rel32' target inside [p, p+n) */
static uint8_t *call_target(uint8_t *p, size_t n, int which)
{
    int k = 0;
    for (size_t i = 0; i + 5 <= n; i++) {
        if (p[i] != 0xE8) continue;
        uint8_t *t = p + i + 5 + *(int32_t *)(p + i + 1);
        if (!in_text(t)) continue;
        if (k++ == which) return t;
    }
    return NULL;
}

/* disp32 of the first 'opcode(3 bytes) disp32' in [p, p+n) */
static int find_disp32(uint8_t *p, size_t n, const uint8_t *op3, uint32_t lo, uint32_t hi, uint32_t *out)
{
    for (size_t i = 0; i + 7 <= n; i++)
        if (!memcmp(p + i, op3, 3)) {
            uint32_t d = *(uint32_t *)(p + i + 3);
            if (d >= lo && d <= hi) { *out = d; return 1; }
        }
    return 0;
}

/* ------------------------------------------------------------------------------------ */
/* engine bindings shared by the finders                                                */
/* ------------------------------------------------------------------------------------ */
typedef struct { union { char buf[16]; char *ptr; } u; size_t size, cap; } MsString;

typedef char    (*SpawnBulletFn)(void *shooter, int syncIdx, float *zero, float *dir);
typedef char    (*IsNetGameFn)(void);
typedef int     (*NextSyncIdFn)(void);

static uint8_t *g_launch;                   /* eBulletShooter::Launch (only read: anchors the weapon->stuff finder) */
static SpawnBulletFn    g_spawnBullet;      /* only read: anchors eAmmo::GetStuff -> item filling offsets */
static IsNetGameFn      g_isNetGame;
static NextSyncIdFn     g_nextSyncId;       /* logged only */
static uint32_t g_stuffOff, g_syncedOff;    /* weapon -> eStuff (set file object); SyncedProjectiles byte (logged only) */
static uint32_t g_stuffTagsBeg, g_stuffTagsEnd;   /* eStuff: {tag} vector begin / end */

#define ENT_POS      0x74    /* entity world position, float3 */
#define TAG_SIZE     0x60    /* Tag vector element */
#define TAG_STR      0x40    /* MSVC std::string inside the element */
#define BB_TAGLEN    48

/* SpawnBullet prologue, position independent, two compiler flavours:
   A (game, server): mov [rsp+8],rbx ; mov [rsp+20],r9 ; mov [rsp+18],r8 ; mov [rsp+10],edx   (19 bytes)
   B (editor):       mov rax,rsp ; mov [rax+8],rbx ; mov [rax+20],r9 ; mov [rax+18],r8 ; mov [rax+10],edx  (18 bytes) */
static const uint8_t k_sbPrologA[] = { 0x48,0x89,0x5C,0x24,0x08, 0x4C,0x89,0x4C,0x24,0x20, 0x4C,0x89,0x44,0x24,0x18, 0x89,0x54,0x24,0x10 };
static const uint8_t k_sbPrologB[] = { 0x48,0x8B,0xC4, 0x48,0x89,0x58,0x08, 0x4C,0x89,0x48,0x20, 0x4C,0x89,0x40,0x18, 0x89,0x50,0x10 };
static size_t sb_prolog_len(const uint8_t *f)
{
    if (!memcmp(f, k_sbPrologA, sizeof k_sbPrologA)) return sizeof k_sbPrologA;
    if (!memcmp(f, k_sbPrologB, sizeof k_sbPrologB)) return sizeof k_sbPrologB;
    return 0;
}
/* Launch: mov rax,rsp ; mov [rax+8],rbx */
static const uint8_t k_launchProlog[] = { 0x48,0x8B,0xC4, 0x48,0x89,0x58,0x08 };

static int in_data(const void *a) { return (uint8_t *)a >= g_data.p && (uint8_t *)a < g_data.p + g_data.n; }

/* vftable of a class via MSVC RTTI (TypeDescriptor in .data <- CompleteObjectLocator (offset 0) <- vftable[-1]) */
static uint8_t *find_vtable(const char *rtti)
{
    uint8_t *name = find_cstr_in(g_data, rtti);
    if (!name) return NULL;
    uint32_t tdRel = (uint32_t)(name - 0x10 - g_base);
    uint8_t *vt = NULL; int hits = 0;
    for (size_t i = 12; i + 4 <= g_rdata.n; i += 4) {
        if (*(uint32_t *)(g_rdata.p + i) != tdRel || *(uint32_t *)(g_rdata.p + i - 12) != 1 || *(uint32_t *)(g_rdata.p + i - 8) != 0) continue;
        uint64_t col = (uint64_t)(g_rdata.p + i - 12);
        for (size_t j = 0; j + 8 <= g_rdata.n; j += 8)
            if (*(uint64_t *)(g_rdata.p + j) == col) { vt = g_rdata.p + j + 8; hits++; }
    }
    return hits == 1 ? vt : NULL;
}

/* eBulletShooter vftable via MSVC RTTI (TypeDescriptor <- CompleteObjectLocator <- vftable[-1]); slot 11 = Launch.
   Gives weapon->stuff (needed for the bomb_interval_<ms> weapon tag) and SpawnBullet (anchor of the magazine
   offsets). If another payload DLL has already inline-hooked Launch its prologue is gone: that is reported as
   such, and the two dependent features degrade (interval from ini / action suffix only; round count unknown). */
static int find_launch(void)
{
    uint8_t *vt = find_vtable(".?AVeBulletShooter@Weapon@Actor@@");
    if (!vt) { logf_("Launch: eBulletShooter vftable not found"); return 0; }
    uint8_t *f = *(uint8_t **)(vt + 11 * 8);
    if (!in_text(f)) { logf_("Launch: slot 11 points outside .text (vtable slot swapped by another DLL?)"); return 0; }
    if (memcmp(f, k_launchProlog, sizeof k_launchProlog)) { logf_("Launch: slot 11 prologue not recognised%s", (f[0] == 0xFF && f[1] == 0x25) || f[0] == 0xE9 ? " (entry already hooked by another DLL)" : ""); return 0; }
    g_launch = f;
    /* weapon->stuff:  mov rdx,[rax+disp32] (48 8B 90) ;  SyncedProjectiles byte: cmp [rdx+disp32],r14b (44 38 B2) */
    if (!find_disp32(f, 0x300, (const uint8_t *)"\x48\x8B\x90", 0x40, 0x200, &g_stuffOff)) { logf_("Launch: weapon->stuff offset not found"); return 0; }
    find_disp32(f, 0x300, (const uint8_t *)"\x44\x38\xB2", 0x1000, 0x2000, &g_syncedOff);
    g_isNetGame = (IsNetGameFn)call_target(f, 0x300, 0);
    for (int k = 0; k < 16; k++) {
        uint8_t *t = call_target(f, 0x300, k);
        if (!t) break;
        if (sb_prolog_len(t)) g_spawnBullet = (SpawnBulletFn)t;
        /* projectile sync id counter (synced weapons, host side):  mov eax,[rip+g] ; inc eax ; mov [rip+g],eax ; ret */
        if (t[0] == 0x8B && t[1] == 0x05 && t[6] == 0xFF && t[7] == 0xC0 && t[8] == 0x89 && t[9] == 0x05 && t[14] == 0xC3) g_nextSyncId = (NextSyncIdFn)t;
    }
    if (!g_spawnBullet) { logf_("Launch: SpawnBullet call not found"); return 0; }
    logf_("Launch = exe+0x%llX, SpawnBullet = exe+0x%llX, weapon->stuff @ +0x%X, SyncedProjectiles @ stuff+0x%X, sync id counter %s",
          (unsigned long long)(f - g_base), (unsigned long long)((uint8_t *)g_spawnBullet - g_base), g_stuffOff, g_syncedOff, g_nextSyncId ? "found" : "NOT found");
    return 1;
}
/* stuff tag vector: CanTakePose (eHumanBrain.cpp) scans the held item's stuff tags for the global
   "cannot_lie" Tag:  mov rdi,[rax+end] ; mov rbx,[rax+begin] ; ... lea rdx,g_tag ; call Tag::eq */
static int find_stuff_tags(void)
{
    uint8_t *str = find_cstr("cannot_lie");
    if (!str) { logf_("stuff tags: 'cannot_lie' not found"); return 0; }
    uint8_t *init = find_lea_to(g_text.p, g_text.n, str, 0x15, 0);
    if (!init) { logf_("stuff tags: static init not found"); return 0; }
    uint8_t *tag = NULL;
    for (uint8_t *q = init + 7; q < init + 0x20; q++)
        if (q[0] == 0x48 && q[1] == 0x8D && q[2] == 0x0D) { tag = q + 7 + *(int32_t *)(q + 3); break; }
    if (!tag) { logf_("stuff tags: global tag not found"); return 0; }
    uint8_t *use = find_lea_to(g_text.p, g_text.n, tag, 0x15, 0);
    if (!use || find_lea_to(g_text.p, g_text.n, tag, 0x15, 1)) { logf_("stuff tags: tag reader missing or ambiguous"); return 0; }
    uint8_t *from = use - 0x40;
    if (!find_disp32(from, 0x40, (const uint8_t *)"\x48\x8B\x98", 0x800, 0x3000, &g_stuffTagsBeg) ||
        !find_disp32(from, 0x40, (const uint8_t *)"\x48\x8B\xB8", 0x800, 0x3000, &g_stuffTagsEnd) ||
        g_stuffTagsEnd != g_stuffTagsBeg + 8) { logf_("stuff tags: vector offsets not found"); return 0; }
    logf_("stuff tags vector @ stuff+0x%X", g_stuffTagsBeg);
    return 1;
}
/* ---- weapon item / magazine (from GEM Converge 0.4; needed here for the loaded round count) ----------------- */
/* The weapon's loaded magazine is the weapon ITEM's filling: item+fillStuff = ammo eStuff*, +8 = rounds
   (eInventoryItem::Fill); eAmmo (weapon+ammo) reaches the item through a weapon vtable slot (GetItem). */
static uint32_t g_weaponItemSlot;     /* weapon vtable byte offset of GetItem() */
static uint32_t g_fillStuffOff;       /* item: filling stuff pointer (rounds = +8) */
static uint32_t g_weaponAmmoOff;      /* weapon: eAmmo* (logged only) */

/* eAmmo vtable slot 1 = GetItem thunk:  mov rcx,[rcx+8] (weapon) ; mov rax,[rcx] ; jmp [rax+slot] */
static int find_weapon_item_slot(void)
{
    uint8_t *vt = find_vtable(".?AVeAmmo@Weapon@Actor@@");
    if (!vt) { logf_("eAmmo: vftable not found"); return 0; }
    uint8_t *f = *(uint8_t **)(vt + 8);
    if (!in_text(f)) { logf_("eAmmo: slot 1 not in .text"); return 0; }
    uint8_t *ld = find_bytes(f, 0x10, (const uint8_t *)"\x48\x8B\x49\x08", 4);
    uint8_t *jm = find_bytes(f, 0x14, (const uint8_t *)"\xFF\x60", 2);
    if (!ld || !jm || jm < ld || jm[2] < 0x08 || jm[2] > 0x80) { logf_("eAmmo: GetItem thunk not recognised"); return 0; }
    g_weaponItemSlot = jm[2];
    logf_("eAmmo::GetItem -> weapon vtable +0x%X", g_weaponItemSlot);
    return 1;
}

/* SpawnBullet:  mov rcx,[weapon+ammo] ; call eAmmo::GetStuff ;  GetStuff = two  call [rax+8]  then  mov rax,[item+d8] */
static int find_filling_offs(void)
{
    uint8_t *f = (uint8_t *)g_spawnBullet;
    for (uint8_t *p = f; p < f + 0x400; p++) {
        if ((p[0] != 0x48 && p[0] != 0x49) || p[1] != 0x8B || (p[2] & 0xF8) != 0x88 || (p[2] & 7) == 4 || p[7] != 0xE8) continue;
        uint32_t d = *(uint32_t *)(p + 3);
        if (d < 0x40 || d > 0x200) continue;
        uint8_t *t = p + 12 + *(int32_t *)(p + 8);
        if (!in_text(t)) continue;
        uint8_t *c1 = find_bytes(t, 0x30, (const uint8_t *)"\xFF\x50\x08", 3);
        if (!c1) continue;
        uint8_t *c2 = find_bytes(c1 + 3, 0x20, (const uint8_t *)"\xFF\x50\x08", 3);
        if (!c2) continue;
        uint8_t *ld = find_bytes(c2 + 3, 0x10, (const uint8_t *)"\x48\x8B\x40", 3);
        if (!ld || ld[3] < 0x20 || ld[3] > 0xF0) continue;
        g_weaponAmmoOff = d; g_fillStuffOff = ld[3];
        logf_("weapon->ammo +0x%X, item filling stuff @ item+0x%X (rounds +0x%X)", d, g_fillStuffOff, g_fillStuffOff + 8);
        return 1;
    }
    logf_("filling: eAmmo::GetStuff not found in SpawnBullet");
    return 0;
}
/* length of a hookable prologue: whole position-independent instructions until >= 14 bytes */
static size_t prolog_len(const uint8_t *f)
{
    size_t n = 0;
    while (n < 14) {
        const uint8_t *p = f + n;
        if (p[0] == 0x48 && p[1] == 0x8B && p[2] == 0xC4) n += 3;                               /* mov rax,rsp */
        else if (p[0] == 0x55 || p[0] == 0x53 || p[0] == 0x56 || p[0] == 0x57) n += 1;          /* push rbp/rbx/rsi/rdi */
        else if (p[0] == 0x41 && p[1] >= 0x54 && p[1] <= 0x57) n += 2;                           /* push r12-r15 */
        else if (p[0] == 0x40 && (p[1] == 0x53 || p[1] == 0x55 || p[1] == 0x56 || p[1] == 0x57)) n += 2; /* rex push rbx/rbp/rsi/rdi */
        else if ((p[0] == 0x48 || p[0] == 0x4C) && p[1] == 0x89 && (p[2] & 0xC7) == 0x40) n += 4;/* mov [rax+d8],r64 */
        else if (p[0] == 0x89 && p[1] == 0x50) n += 3;                                           /* mov [rax+d8],edx */
        else if ((p[0] == 0x48 || p[0] == 0x4C) && p[1] == 0x89 && (p[2] & 0xC7) == 0x44 && p[3] == 0x24) n += 5; /* mov [rsp+d8],r64 */
        else if (p[0] == 0x89 && p[1] == 0x54 && p[2] == 0x24) n += 4;                           /* mov [rsp+d8],edx */
        else if (p[0] == 0x48 && p[1] == 0x8D && p[2] == 0xA8) n += 7;                           /* lea rbp,[rax+d32] */
        else if (p[0] == 0x48 && p[1] == 0x8D && p[2] == 0x6C && p[3] == 0x24) n += 5;           /* lea rbp,[rsp+d8] */
        else if (p[0] == 0x48 && p[1] == 0x81 && p[2] == 0xEC) n += 7;                           /* sub rsp,imm32 */
        else if (p[0] == 0x48 && p[1] == 0x83 && p[2] == 0xEC) n += 4;                           /* sub rsp,imm8 */
        else if (p[0] == 0x48 && p[1] == 0x8B && (p[2] & 0xC0) == 0xC0) n += 3;                  /* mov r64,r64 */
        else if (p[0] == 0x8B && (p[1] & 0xC0) == 0x80 && (p[1] & 7) != 4) n += 6;               /* mov r32,[r64+d32] */
        else return 0;
    }
    return n;
}
/* ---- bomb bay airstrikes ------------------------------------------------------------------------------------ */
/* Units-set {action "airstrike:<name>"}: the host's handler spawns the plane, then a StrikeSetup points it and gives it
   Order::eAirstrike = MoveTo(target), eBombDropOrder(name), MoveTo(exit), eDestroyOrder (two setups exist: A = units-set
   path, B = the command path every tested conquest run took; both build the same sequence). eBombDropOrder::Execute
   (vtable slot 19) only spawns entity <name> at the target's x/y and the plane's height; the plane's own bomb weapon is
   never used.
   With this dll the first tick of every eAirstrike (vtable slot 3, patched in the vtable, so every build and both setups)
   looks at the drop child's name: when it is a bay action the plane's bomb place(s) (place name starting with ini
   BombPlace, "bomb1") are collected (weapon, its eBombShooter, loaded rounds) and a BombRun starts; the carrier gets
   script event "bomb_run" (ini BombEvent). Every following tick measures the plane's velocity from its own movement and,
   with BombLead, starts the stick early so it lands centred on the click; one bomb per interval leaves the bay through
   eBombShooter::Launch on the next barrel (bone shooter_bbNN), the magazine loses one round, event "bomb_run_dropN".
   eBombDropOrder::Execute (also a vtable patch) then skips its vanilla spawn when a run exists for the carrier (a bay
   action on a plane without a usable bomb place skips it too: the name is no entity). Orders only exist on the host /
   in SP. Layout (1.067): entity+0xDD8 = weaponry, weaponry+0x18 = place* vector, place+0x18 = place name Tags (string
   +0x40), place+0x2B8 = weapon (from the weapon_work action; eWeaponFire::Execute gives the place->weapon offset). */
typedef intptr_t (*OrderFn)(void *order, intptr_t a, intptr_t b, intptr_t c);
typedef void *(*ObjFn)(void *obj);
typedef void *(*TagCtorFn)(void *tag, const char *s);
typedef void *(*EvInitFn)(void *ev, void *entity);
typedef void  (*EvSendFn)(void *ev, void *tag);
typedef char  (*BombLaunchFn)(void *shooter);
typedef float (*GroundZFn)(const float *xy);
typedef float *(*GetVelFn)(void *entity, float *out);
static BombLaunchFn g_bombLaunch;
static GroundZFn g_groundZ;
static GetVelFn g_getVel;
static float *g_gravity, *g_gravK;
static uint8_t *g_vtBombShooter;
static int find_strike_altitude(void);
static uint8_t *g_bdExec, *g_asTick, *g_vtBD, *g_vtAS;
static ObjFn g_evDtor;
static TagCtorFn g_tagCtor;
static EvInitFn g_evInit;
static EvSendFn g_evSend;
static uint32_t g_ifaceOff, g_bdTargetOff, g_weaponryOff, g_placesOff, g_placeNameOff, g_placeWeaponOff;

static uint8_t *vslot(uint8_t *vt, int i) { uint8_t *f = *(uint8_t **)(vt + 8 * i); return in_text(f) ? f : NULL; }
static uint8_t *call_after(uint8_t *p, size_t n) { uint8_t *c = find_bytes(p, n, (const uint8_t *)"\xE8", 1); return c ? c + 5 + *(int32_t *)(c + 1) : NULL; }

static int find_bombbay(void)
{
    uint8_t *vtBD = find_vtable(".?AUeBombDropOrder@Order@@"), *vtAS = find_vtable(".?AUeAirstrike@Order@@"),
            *vtWF = find_vtable(".?AUeWeaponFire@Order@@"), *vtWW = find_vtable(".?AUeWeaponWork@Interaction@@"),
            *vtAA = find_vtable(".?AUeAirAttack@Order@@");
    if (!vtBD || !vtAS || !vtWF || !vtWW || !vtAA) {
        logf_("bomb bay: vtables not found (drop %d strike %d fire %d work %d attack %d)", !!vtBD, !!vtAS, !!vtWF, !!vtWW, !!vtAA); return 0; }
    uint8_t *p;
    /* eBombDropOrder::Execute (slot 19; the editor build is a thin wrapper): lea r64,[rax-iface] -> order+8 = carrier interface */
    g_vtBD = vtBD; g_vtAS = vtAS;
    if (!(g_bdExec = vslot(vtBD, 19))) { logf_("bomb bay: drop Execute not in .text"); return 0; }
    for (p = g_bdExec; p < g_bdExec + 0x100 && !g_ifaceOff; p++)
        if ((p[0] == 0x48 || p[0] == 0x4C) && p[1] == 0x8D && (p[2] & 0xC7) == 0x80) {   /* lea r64,[rax+disp32] */
            int32_t d = *(int32_t *)(p + 3);
            if (d < -0x400 && d > -0x2000) g_ifaceOff = (uint32_t)(-d);              /* not the frame setup lea rbp,[rax-0x108] */
        }
    if (!g_ifaceOff) { logf_("bomb bay: carrier interface offset not found"); return 0; }
    /* drop target point: movss xmm0,[rdi+d8] (game) / lea rdx,[rcx+d8] (editor) */
    if ((p = find_bytes(g_bdExec, 0x120, (const uint8_t *)"\xF3\x0F\x10\x47", 4))) g_bdTargetOff = p[4];
    else if ((p = find_bytes(g_bdExec, 0x40, (const uint8_t *)"\x48\x8D\x51", 3))) g_bdTargetOff = p[3];
    if (g_bdTargetOff < 0x40 || g_bdTargetOff > 0x68) { logf_("bomb bay: drop target offset not found"); return 0; }
    if (!(g_asTick = vslot(vtAS, 3))) { logf_("bomb bay: airstrike tick not in .text"); return 0; }
    /* eWeaponFire::Execute (slot 19): its first call returns the held / place weapon: mov rax,[rax+placeWeapon] */
    uint8_t *wf = vslot(vtWF, 19), *held = wf ? call_target(wf, 0x40, 0) : NULL;
    if (!held || !find_disp32(held, 0x20, (const uint8_t *)"\x48\x8B\x80", 0x100, 0x800, &g_placeWeaponOff)) { logf_("bomb bay: place->weapon offset not found"); return 0; }
    /* weapon_work execute (slot 9): mov rcx,[entity+weaponry] ; call FindPlace (mov rbx,[rcx+places] ; add rcx,name) */
    uint8_t *ww = vslot(vtWW, 9);
    if (!ww || !find_disp32(ww, 0x20, (const uint8_t *)"\x49\x8B\x88", 0x400, 0x2000, &g_weaponryOff)) { logf_("bomb bay: weaponry offset not found"); return 0; }
    uint8_t *fp = call_target(ww, 0x30, 0);
    if (!fp || !(p = find_bytes(fp, 0x20, (const uint8_t *)"\x48\x8B\x59", 3))) { logf_("bomb bay: place vector not found"); return 0; }
    g_placesOff = p[3];
    if (!(p = find_bytes(fp, 0x60, (const uint8_t *)"\x48\x83\xC1", 3))) { logf_("bomb bay: place name offset not found"); return 0; }
    g_placeNameOff = p[3];
    /* script event (eAirAttack slot 20 raises "air_attack"): lea rdx,[entity-iface] ; cmove ; call EvInit ...
       lea rdx,[tag] ; lea rcx,[ev] ; call EvSend ; lea rcx,[ev] ; call EvDtor ; guarded init: lea rdx,"air_attack" ; lea rcx,[tag] ; call TagCtor */
    uint8_t *aa = vslot(vtAA, 20), *ls = NULL;
    for (uint8_t *q = aa; aa && q < aa + 0x200 && !ls; q++)      /* lea rdx,"air_attack" (the exe holds several copies of the string) */
        if (q[0] == 0x48 && q[1] == 0x8D && q[2] == 0x15) { uint8_t *t = q + 7 + *(int32_t *)(q + 3); if (in_rdata(t) && !memcmp(t, "air_attack", 11)) ls = q; }
    if (ls) {
        uint8_t *lt = find_bytes(ls + 7, 0x10, (const uint8_t *)"\x48\x8D\x0D", 3);
        uint8_t *tag = lt ? lt + 7 + *(int32_t *)(lt + 3) : NULL;
        g_tagCtor = lt ? (TagCtorFn)call_after(lt, 0x10) : NULL;
        uint8_t *lu = tag ? find_lea_to(aa, 0x200, tag, 0x15, 0) : NULL;
        g_evSend = lu ? (EvSendFn)call_after(lu, 0x10) : NULL;
        uint8_t *cs = lu ? find_bytes(lu, 0x10, (const uint8_t *)"\xE8", 1) : NULL;
        g_evDtor = cs ? (ObjFn)call_after(cs + 5, 0x10) : NULL;
        uint8_t *li = find_bytes(aa, 0x200, (const uint8_t *)"\x48\x8D\x90", 3);
        g_evInit = li ? (EvInitFn)call_after(li, 0x20) : NULL;
    }
    if (!g_tagCtor || !g_evSend || !g_evDtor || !g_evInit) {
        logf_("bomb bay: script event call not found (no bomb_run event; string %d tag ctor %d send %d dtor %d init %d)", !!ls, !!g_tagCtor, !!g_evSend, !!g_evDtor, !!g_evInit);
        g_evSend = NULL;
    }
    /* eBombShooter (slot 11 = Launch: spawns the loaded bomb at bone "<shooter>NN", NN = [shooter+0x10], with the owner's
       velocity; slot 3 = the impact predictor: GroundZ(float2*) first call, GetVelocity(entity, float3*) second call,
       t = (sqrt(k*g*h + vz^2) - vz) / g with g = movss xmm2,[rip+g] and k = the mulss xmm2,[rip+k] after it) */
    uint8_t *vtBS = find_vtable(".?AUeBombShooter@Weapon@Actor@@"), *imp;
    if (!vtBS || !(g_bombLaunch = (BombLaunchFn)vslot(vtBS, 11)) || !(imp = vslot(vtBS, 3))) { logf_("bomb bay: eBombShooter not found"); return 0; }
    g_vtBombShooter = vtBS;
    g_groundZ = (GroundZFn)call_target(imp, 0x80, 0);
    g_getVel = (GetVelFn)call_target(imp, 0x80, 1);
    uint8_t *pg = find_bytes(imp, 0x100, (const uint8_t *)"\xF3\x0F\x10\x15", 4), *pk = pg ? find_bytes(pg + 8, 0x10, (const uint8_t *)"\xF3\x0F\x59\x15", 4) : NULL;
    if (!g_groundZ || !g_getVel || !pg || !pk) { logf_("bomb bay: impact predictor not recognised"); return 0; }
    g_gravity = (float *)(pg + 8 + *(int32_t *)(pg + 4));
    g_gravK = (float *)(pk + 8 + *(int32_t *)(pk + 4));
    logf_("bomb bay: eBombShooter::Launch = exe+0x%llX, GroundZ = exe+0x%llX, GetVelocity = exe+0x%llX, gravity %g x %g",
          (unsigned long long)((uint8_t *)g_bombLaunch - g_base), (unsigned long long)((uint8_t *)g_groundZ - g_base),
          (unsigned long long)((uint8_t *)g_getVel - g_base), *g_gravity, *g_gravK);
    logf_("bomb bay: airstrike tick = exe+0x%llX, drop Execute = exe+0x%llX, carrier iface +0x%X, drop target +0x%X, weaponry +0x%X, places +0x%X, place name +0x%X, place weapon +0x%X, event %s",
          (unsigned long long)(g_asTick - g_base), (unsigned long long)(g_bdExec - g_base),
          g_ifaceOff, g_bdTargetOff, g_weaponryOff, g_placesOff, g_placeNameOff, g_placeWeaponOff, g_evSend ? "found" : "off");
    find_strike_altitude();                              /* optional: altitude stays vanilla when it fails */
    return 1;
}

/* Airstrike altitude: the flight setup (called by StrikeSetup(carrier, name, spawn, target, exit)) stores one static
   float (40 m, .data) into the flight command's altitude field: movss xmm6,[rip+g] … movss [rax+0x4c],xmm6. StrikeSetup
   is its caller that first tests the name string (cmp qword [rdx+0x10],0) and starts with mov [rsp+8],rbx. */
/* Two setups call it: A (units-set path, `(carrier, name, spawn, target, exit)`, starts mov [rsp+8],rbx; mov [rsp+18],rbp;
   mov [rsp+20],rsi; push rdi) and B (the command path every tested game run took, `(player*, entity id, name, spawn, …)`,
   starts mov [rsp+18],rbx; mov [rsp+10],rdx; push rbp; push rsi; push rdi). Both are hooked; the name is found among the
   first three pointer args. */
typedef intptr_t (*StrikeSetupFn)(intptr_t a0, intptr_t a1, intptr_t a2, intptr_t a3, intptr_t a4, intptr_t a5, intptr_t a6, intptr_t a7);
static float *g_strikeAlt;
static uint8_t *g_strikeSetupA, *g_strikeSetupB;

static int find_strike_altitude(void)
{
    uint8_t *t = g_text.p, *te = t + g_text.n, *flight = NULL;
    for (uint8_t *s = t; s + 5 <= te && !g_strikeAlt; s++) {
        if (s[0] != 0xF3 || s[1] != 0x0F || s[2] != 0x11 || s[4] != 0x4C || !((s[3] >= 0x40 && s[3] <= 0x47) || (s[3] >= 0x70 && s[3] <= 0x77))) continue;
        for (uint8_t *k = s - 0x80 > t ? s - 0x80 : t; k + 8 <= s; k++) {              /* movss xmmN,[rip+d32] of a 40.0 in .data */
            if (k[0] != 0xF3 || k[1] != 0x0F || k[2] != 0x10 || (k[3] & 0xC7) != 0x05) continue;
            float *g = (float *)(k + 8 + *(int32_t *)(k + 4));
            if (!in_data(g) || *g != 40.f) continue;
            for (uint8_t *f = k; f > t && f > k - 0x400; f--)
                if (!memcmp(f, "\x48\x8B\xC4\x48\x89\x58\x08", 7)) { flight = f; break; }
            if (flight) { g_strikeAlt = g; break; }
        }
    }
    if (!g_strikeAlt) { logf_("bomb bay: airstrike altitude constant not found"); return 0; }
    static const uint8_t proA[] = "\x48\x89\x5C\x24\x08\x48\x89\x6C\x24\x18\x48\x89\x74\x24\x20\x57", proB[] = "\x48\x89\x5C\x24\x18\x48\x89\x54\x24\x10\x55\x56\x57";
    for (uint8_t *s = t; s + 5 <= te; s++) {                                             /* every caller: its function start (mov [rsp+d8],rbx after padding) */
        if (s[0] != 0xE8 || s + 5 + *(int32_t *)(s + 1) != flight) continue;
        for (uint8_t *f = s; f > t + 2 && f > s - 0x600; f--) {
            if (memcmp(f, "\x48\x89\x5C\x24", 4) || (f[-1] != 0xCC && f[-1] != 0xC3)) continue;
            if (!memcmp(f, proA, 16)) g_strikeSetupA = f;
            else if (!memcmp(f, proB, 13)) g_strikeSetupB = f;
            break;
        }
    }
    logf_("bomb bay: airstrike altitude %g m @ exe+0x%llX (flight setup exe+0x%llX), StrikeSetup A %s, B %s",
          *g_strikeAlt, (unsigned long long)((uint8_t *)g_strikeAlt - g_base), (unsigned long long)(flight - g_base),
          g_strikeSetupA ? "found" : "NOT found", g_strikeSetupB ? "found" : "NOT found");
    return g_strikeSetupA || g_strikeSetupB;
}

#ifndef BB_TEST
/* ------------------------------------------------------------------------------------ */
/* run-time helpers + inline hook                                                       */
/* ------------------------------------------------------------------------------------ */
static int rd_mem(const void *src, void *dst, size_t n)
{
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), src, dst, n, &got) && got == n;
}

static int rd_msstr(const uint8_t *at, char *out, size_t n)
{
    MsString s; out[0] = 0;
    if (!rd_mem(at, &s, sizeof s)) return 0;
    if (s.size >= n) s.size = n - 1;
    if (s.cap > 15) { if (!rd_mem(s.u.ptr, out, s.size)) return 0; } else memcpy(out, s.u.buf, s.size);
    out[s.size] = 0;
    return 1;
}

/* 14-byte 'jmp [rip+0]; imm64' over the function's prologue, the displaced instructions in a trampoline that jumps
   back. Runs from DllMain, before any engine code, so nothing can be executing the patched bytes. Refuses an entry
   another DLL has already patched (prolog_len does not know FF 25 / E9) - that is logged, the caller degrades. */
static int install_hook_at(const char *what, void *target, void *hook, void **orig)
{
    uint8_t *f = (uint8_t *)target;
    size_t n = prolog_len(f);
    if (!n) {
        logf_("%s: prologue at exe+0x%llX not hookable%s - not hooking", what, (unsigned long long)(f - g_base),
              (f[0] == 0xFF && f[1] == 0x25) || f[0] == 0xE9 ? " (entry already hooked by another DLL)" : "");
        return 0;
    }
    uint8_t *tramp = VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!tramp) { logf_("%s: VirtualAlloc failed", what); return 0; }
    memcpy(tramp, f, n);
    tramp[n] = 0xFF; tramp[n + 1] = 0x25; *(uint32_t *)(tramp + n + 2) = 0;
    *(uint64_t *)(tramp + n + 6) = (uint64_t)(f + n);
    DWORD old;
    if (!VirtualProtect(tramp, 64, PAGE_EXECUTE_READ, &old)) { logf_("%s: trampoline VirtualProtect failed (%lu)", what, GetLastError()); return 0; }
    *orig = tramp;

    uint8_t patch[32]; memset(patch, 0x90, sizeof patch);
    patch[0] = 0xFF; patch[1] = 0x25; *(uint32_t *)(patch + 2) = 0;
    *(uint64_t *)(patch + 6) = (uint64_t)hook;
    if (!VirtualProtect(f, n, PAGE_EXECUTE_READWRITE, &old)) { logf_("%s: VirtualProtect failed (%lu)", what, GetLastError()); *orig = NULL; return 0; }
    memcpy(f, patch, n);
    VirtualProtect(f, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), f, n);
    logf_("patched: %s = entry exe+0x%llX (%zu bytes)", what, (unsigned long long)(f - g_base), n);
    return 1;
}

static OrderFn g_asTickOrig, g_bdExecOrig;
static char g_bombPlace[64] = "bomb", g_bombEvent[64] = "bomb_run";
static int g_bombNet = 0, g_bombLog = 16;
static int g_bombInterval = 200;                  /* ms between the bombs of one stick */
static int g_bombVelPct = 100, g_bombLead = 1;    /* % of the plane's velocity the bombs carry; 1 = release early so the stick lands on the click */
static char g_bombAction[64] = "marauder_bomb";   /* {action "airstrike:<BombAction>[_<N>ms][_<N>m]"} = bay run (N ms between bombs, N m altitude); other airstrikes stay vanilla (empty = every airstrike) */
static int g_bombAltitude = 40;                   /* m, bay runs without a _<N>m suffix (vanilla 40) */
static int g_bombLeadDelay = 1000;                /* ms: release the stick this much later than the computed lead (Alex: the lead was ~1 s early) */
#define BD_NAME_OFF 0x38                          /* eBombDropOrder: std::string entity name (the part after "airstrike:") */

/* the roster line's text after "airstrike:": 1 when it selects a bay run; *ms = "_<N>ms" (or bare "_<N>") token,
   *alt = "_<N>m" token, -1 when absent; tokens in any order, anything else = not a bay action */
static int bomb_action_parse(const char *name, int *ms, int *alt)
{
    *ms = -1; *alt = -1;
    if (!g_bombAction[0]) return 1;
    size_t a = strlen(g_bombAction);
    if (_strnicmp(name, g_bombAction, a)) return 0;
    const char *p = name + a;
    while (*p) {
        if (*p != '_') return 0;
        const char *d = ++p; while (*p >= '0' && *p <= '9') p++;
        if (p == d) return 0;
        int v = atoi(d);
        if (!*p || *p == '_') { if (v <= 10000) *ms = v; }
        else if ((p[0] == 'm' || p[0] == 'M') && (p[1] == 's' || p[1] == 'S') && (!p[2] || p[2] == '_')) { if (v <= 10000) *ms = v; p += 2; }
        else if ((p[0] == 'm' || p[0] == 'M') && (!p[1] || p[1] == '_')) { if (v >= 1 && v <= 1000) *alt = v; p += 1; }
        else return 0;
    }
    return 1;
}

/* the drop order's entity name (same text) */
static int bomb_action_match(const uint8_t *drop, char *name, size_t n, int *ms)
{
    int alt; name[0] = 0; *ms = -1;
    if (!rd_msstr(drop + BD_NAME_OFF, name, n)) return !g_bombAction[0];
    for (char *s = name; *s; s++) if (*s < 0x20 || *s > 0x7E) { name[0] = 0; return !g_bombAction[0]; }
    return bomb_action_parse(name, ms, &alt);
}

static StrikeSetupFn g_strikeSetupAOrig, g_strikeSetupBOrig;
/* StrikeSetup hooks: a bay action flies at its _<N>m altitude (else BombAltitude); the vanilla constant is put back after.
   The name std::string is whichever of the first three pointer args reads as a printable bay action. */
static intptr_t strike_core(StrikeSetupFn orig, const char *which, intptr_t a0, intptr_t a1, intptr_t a2, intptr_t a3, intptr_t a4, intptr_t a5, intptr_t a6, intptr_t a7)
{
    char nm[64]; int ms, alt = -1, hit = 0;
    intptr_t args[3] = { a1, a2, a3 };
    for (int i = 0; i < 3 && !hit; i++) {
        if (args[i] < 0x10000 || !rd_msstr((const uint8_t *)args[i], nm, sizeof nm) || !nm[0]) continue;
        int ok = 1; for (char *s = nm; *s; s++) if (*s < 0x20 || *s > 0x7E) { ok = 0; break; }
        if (ok && bomb_action_parse(nm, &ms, &alt)) hit = 1;
    }
    if (!hit) return orig(a0, a1, a2, a3, a4, a5, a6, a7);
    float save = *g_strikeAlt, want = (float)(alt > 0 ? alt : g_bombAltitude);
    *g_strikeAlt = want;
    intptr_t rv = orig(a0, a1, a2, a3, a4, a5, a6, a7);
    *g_strikeAlt = save;
    if (g_bombLog > 0) { g_bombLog--; logf_("bomb bay: airstrike:%s set up at %g m (vanilla %g, setup %s)", nm, want, save, which); }
    return rv;
}
static intptr_t hook_strike_a(intptr_t a0, intptr_t a1, intptr_t a2, intptr_t a3, intptr_t a4, intptr_t a5, intptr_t a6, intptr_t a7)
{ return strike_core(g_strikeSetupAOrig, "A", a0, a1, a2, a3, a4, a5, a6, a7); }
static intptr_t hook_strike_b(intptr_t a0, intptr_t a1, intptr_t a2, intptr_t a3, intptr_t a4, intptr_t a5, intptr_t a6, intptr_t a7)
{ return strike_core(g_strikeSetupBOrig, "B", a0, a1, a2, a3, a4, a5, a6, a7); }

/* "bomb_interval_<ms>" on the bomb weapon's set file, else the ini default */
static int bomb_interval_of(const uint8_t *weapon)
{
    const uint8_t *stuff = NULL;
    if (!g_stuffOff || !g_stuffTagsBeg || !rd_mem(weapon + g_stuffOff, &stuff, 8) || !stuff) return g_bombInterval;
    const uint8_t *b = NULL, *e = NULL;
    if (!rd_mem(stuff + g_stuffTagsBeg, &b, 8) || !rd_mem(stuff + g_stuffTagsEnd, &e, 8) || !b || e < b || (size_t)(e - b) > TAG_SIZE * 256) return g_bombInterval;
    char s[BB_TAGLEN];
    for (const uint8_t *t = b; t < e; t += TAG_SIZE)
        if (rd_msstr(t + TAG_STR, s, sizeof s) && !_strnicmp(s, "bomb_interval_", 14)) { int ms = atoi(s + 14); if (ms >= 0 && ms <= 10000) return ms; }
    return g_bombInterval;
}
static GetVelFn g_getVelOrig;                     /* GetVelocity hook (the plane reads 0 there: planes fly without a physics body) */
static int g_bombVelActive, g_bombVelLog = 3;
static float g_bombVel[3];
static const void *g_bombVelOwner;                /* the plane whose bomb is leaving: only its GetVelocity is answered */

#define BOMB_PLACES 4
typedef struct {
    uint8_t *carrier;
    uint8_t *weapon[BOMB_PLACES], *shooter[BOMB_PLACES];
    int left[BOMB_PLACES], barrel[BOMB_PLACES];
    int n, total, released;
    int interval;               /* ms between bombs (weapon tag bomb_interval_<ms>, else ini BombInterval) */
    int dropping;               /* the stick has started: one bomb per interval */
    DWORD next;                 /* time the next bomb may leave the bay */
    float tx, ty;
    float lp[3], vel[3]; DWORD lt; int haveVel;   /* plane velocity (units/ms) from its own movement, smoothed */
    DWORD t0, last;             /* start / last airstrike tick that saw this carrier (a run not ticked for RUN_STALE_MS is dead) */
    int passed;                 /* the engine's own drop step has run (hook_bd_exec): the run may be forgotten once the bay is empty */
} BombRun;
#define RUN_STALE_MS 5000       /* an eAirstrike ticks every frame while it lives; silence this long = the order / plane is gone */

/* while the dll releases a bomb, the engine's GetVelocity(owner) answers with the plane's measured velocity */
static float *hook_getvel(void *entity, float *out)
{
    if (!g_bombVelActive || entity != g_bombVelOwner) return g_getVelOrig(entity, out);
    memcpy(out, g_bombVel, 12);
    if (g_bombVelLog > 0) { g_bombVelLog--; logf_("bomb bay: GetVelocity(%p) during the release -> (%.3f %.3f %.3f) units/ms", entity, out[0], out[1], out[2]); }
    return out;
}
#define MAX_RUNS 16
static BombRun g_runs[MAX_RUNS];
static int g_runNext;

/* the weapon's item (loaded magazine), NULL when unknown */
static uint8_t *weapon_item(uint8_t *w)
{
    if (!g_weaponItemSlot || !g_fillStuffOff) return NULL;
    uint8_t *vt; if (!rd_mem(w, &vt, 8) || !in_rdata(vt)) return NULL;
    void *(*getItem)(void *) = *(void *(**)(void *))(vt + g_weaponItemSlot);
    if (!in_text((void *)getItem)) return NULL;
    return getItem(w);
}

/* loaded rounds of a weapon (item filling count), -1 when unknown */
static int weapon_rounds(uint8_t *w)
{
    uint8_t *item = weapon_item(w); int r;
    if (!item || !rd_mem(item + g_fillStuffOff + 8, &r, 4)) return -1;
    return r;
}

static int bomb_place_match(const char *nm)
{
    size_t m = strlen(g_bombPlace);
    return m && !_strnicmp(nm, g_bombPlace, m);
}

/* script event <name> on the carrier (Tags built once per name) */
static void bomb_event(uint8_t *carrier, const char *name)
{
    static struct { char name[64]; uint8_t tag[0x80]; } tags[12];
    static int ntags;
    if (!g_evSend || !name[0]) return;
    uint8_t *tag = NULL;
    for (int i = 0; i < ntags; i++) if (!strcmp(tags[i].name, name)) tag = tags[i].tag;
    if (!tag) {
        if (ntags >= 12) return;
        snprintf(tags[ntags].name, sizeof tags[ntags].name, "%s", name);
        g_tagCtor(tags[ntags].tag, tags[ntags].name);
        tag = tags[ntags++].tag;
    }
    static uint8_t ev[0x400];
    memset(ev, 0, sizeof ev);
    g_evInit(ev, carrier);
    g_evSend(ev, tag);
    g_evDtor(ev);
}

/* the eBombShooter that belongs to weapon w (a pointer field of the weapon whose object points back at it) */
static uint8_t *weapon_bomb_shooter(uint8_t *w)
{
    for (int off = 8; off < 0x400; off += 8) {
        uint8_t *s, *vt, *back;
        if (rd_mem(w + off, &s, 8) && s && rd_mem(s, &vt, 8) && vt == g_vtBombShooter && rd_mem(s + 8, &back, 8) && back == w) return s;
    }
    return NULL;
}

static void bomb_start(uint8_t *carrier, const float *xy, const char *action, int actionMs)
{
    if (!carrier || !xy) return;
    if (g_isNetGame && g_isNetGame() && !g_bombNet) return;
    uint8_t *wr, *b, *e;
    if (!rd_mem(carrier + g_weaponryOff, &wr, 8) || !wr || !rd_mem(wr + g_placesOff, &b, 8) || !rd_mem(wr + g_placesOff + 8, &e, 8)) return;
    if (!b || e < b || e - b > 64 * 8) return;
    BombRun r; memset(&r, 0, sizeof r);
    char names[200] = "";
    for (uint8_t **pp = (uint8_t **)b; pp < (uint8_t **)e && r.n < BOMB_PLACES; pp++) {
        uint8_t *pl, *w, *s; char nm[64];
        if (!rd_mem(pp, &pl, 8) || !pl || !rd_msstr(pl + g_placeNameOff + TAG_STR, nm, sizeof nm) || !bomb_place_match(nm)) continue;
        if (!rd_mem(pl + g_placeWeaponOff, &w, 8) || !w) continue;
        int rounds = weapon_rounds(w);
        s = weapon_bomb_shooter(w);
        if (rounds < 0) snprintf(names + strlen(names), sizeof names - strlen(names), "%s%s (rounds UNKNOWN - magazine offsets not found, shooter %s)", r.n ? ", " : "", nm, s ? "found" : "NOT found");
        else snprintf(names + strlen(names), sizeof names - strlen(names), "%s%s (rounds %d, shooter %s)", r.n ? ", " : "", nm, rounds, s ? "found" : "NOT found");
        if (!s || rounds <= 0) continue;
        if (rounds > 16) rounds = 16;
        if (!r.n) r.interval = actionMs >= 0 ? actionMs : bomb_interval_of(w);
        r.weapon[r.n] = w; r.shooter[r.n] = s; r.left[r.n] = rounds; r.total += rounds; r.n++;
    }
    if (g_bombLog > 0) { g_bombLog--; logf_("bomb bay: run started (airstrike:%s), target (%.1f %.1f), %s, interval %d ms", action, xy[0], xy[1], names[0] ? names : "no bomb place", r.interval); }
    for (int i = 0; i < MAX_RUNS; i++) if (g_runs[i].carrier == carrier) g_runs[i].carrier = NULL;   /* a reused entity address */
    if (!r.n) return;
    r.carrier = carrier; r.tx = xy[0]; r.ty = xy[1]; r.t0 = r.last = GetTickCount();
    g_runs[g_runNext] = r; g_runNext = (g_runNext + 1) % MAX_RUNS;
    bomb_event(carrier, g_bombEvent);
}

/* the live run of a carrier; a run whose order stopped ticking (plane shot down / deleted before the drop step) is
   forgotten here instead of surviving until its entity address is reused by another plane */
static BombRun *bomb_run_of(const uint8_t *carrier)
{
    DWORD now = GetTickCount();
    for (int i = 0; i < MAX_RUNS && carrier; i++) {
        if (g_runs[i].carrier != carrier) continue;
        if (now - g_runs[i].last > RUN_STALE_MS) { g_runs[i].carrier = NULL; return NULL; }
        return &g_runs[i];
    }
    return NULL;
}

/* one bomb out of the bay: the bomb shooter's own Launch on the next barrel, one round off the magazine, event
   "<BombEvent>_drop<N>" (N = barrel + 1, matching the mdl's bomb_01..04 bones) */
static void bomb_release(BombRun *r)
{
    for (int k = 0; k < r->n; k++) {
        if (r->left[k] <= 0) continue;
        uint8_t *s = r->shooter[k], *vt = NULL, *back = NULL;
        if (!rd_mem(s, &vt, 8) || vt != g_vtBombShooter || !rd_mem(s + 8, &back, 8) || back != r->weapon[k]) {   /* weapon gone (plane damaged)? */
            if (g_bombLog > 0) { g_bombLog--; logf_("bomb bay: bomb place %d no longer has its shooter - %d bomb(s) left in it skipped", k + 1, r->left[k]); }
            r->released += r->left[k]; r->left[k] = 0; continue;
        }
        uint8_t *item = weapon_item(r->weapon[k]);
        int have = item ? weapon_rounds(r->weapon[k]) : -1;
        if (have == 0) { r->released += r->left[k]; r->left[k] = 0; continue; }     /* magazine emptied by something else */
        int barrel = r->barrel[k]++;
        *(int32_t *)(s + 0x10) = barrel;
        for (int i = 0; i < 3; i++) g_bombVel[i] = r->haveVel ? r->vel[i] * (g_bombVelPct / 100.f) : 0.f;
        g_bombVelOwner = r->carrier;
        g_bombVelActive = g_getVelOrig && g_bombVelPct > 0;
        g_bombLaunch(s);
        g_bombVelActive = 0; g_bombVelOwner = NULL;
        r->left[k]--; r->released++;
        int rounds;
        if (item && rd_mem(item + g_fillStuffOff + 8, &rounds, 4) && rounds > 0) *(int32_t *)(item + g_fillStuffOff + 8) = rounds - 1;
        char ev[96]; snprintf(ev, sizeof ev, "%s_drop%d", g_bombEvent, barrel + 1);
        bomb_event(r->carrier, ev);
        if (g_bombLog > 0) {
            float p[3] = { 0 }; rd_mem(r->carrier + ENT_POS, p, 12); g_bombLog--;
            logf_("bomb bay: bomb %d/%d released from barrel %02d at (%.1f %.1f %.1f), %.1f s after the run started", r->released, r->total, barrel, p[0], p[1], p[2], (GetTickCount() - r->t0) / 1000.0);
        }
        return;
    }
}

/* every airstrike tick: measure the plane's velocity from its movement; with BombLead the stick starts when the
   remaining along-track distance to the click point equals what the bombs will carry forward while falling
   (v_h x sqrt(2h/g), engine time in ms) plus half the stagger, so the stick lands centred on the click; otherwise it
   starts when the engine's own drop step fires (plane over the click). Then one bomb per BombInterval ms. */
static void bomb_tick(BombRun *r)
{
    if (r->released >= r->total) { if (r->passed) r->carrier = NULL; return; }
    DWORD now = GetTickCount();
    float p[3];
    if (rd_mem(r->carrier + ENT_POS, p, 12)) {
        if (r->lt && now > r->lt) {
            float dt = (float)(now - r->lt), inst[3];
            for (int i = 0; i < 3; i++) inst[i] = (p[i] - r->lp[i]) / dt;
            if (dt < 500.f) {                              /* one frame: smooth a little */
                for (int i = 0; i < 3; i++) r->vel[i] = r->haveVel ? r->vel[i] * 0.7f + inst[i] * 0.3f : inst[i];
                r->haveVel = 1;
            }
        }
        if (!r->lt || now > r->lt) { memcpy(r->lp, p, 12); r->lt = now; }
        if (!r->dropping && g_bombLead && r->haveVel) {
            float hv = sqrtf(r->vel[0] * r->vel[0] + r->vel[1] * r->vel[1]), g = *g_gravity;
            if (hv > 1e-4f && g > 0.f) {
                float dx = r->tx - p[0], dy = r->ty - p[1];
                float dist = (dx * r->vel[0] + dy * r->vel[1]) / hv;          /* along-track distance still to go */
                float xy[2] = { r->tx, r->ty }, h = p[2] - g_groundZ(xy);
                float tf = h > 0.f ? sqrtf(2.f * h / g) : 0.f;                  /* ms to fall onto the click point */
                float carry = (g_getVelOrig && g_bombVelPct > 0) ? hv * (g_bombVelPct / 100.f) * tf : 0.f;
                float stagger = hv * r->interval * (r->total - 1) * 0.5f, lead = carry + stagger - hv * g_bombLeadDelay;
                if (dist <= lead) {
                    r->dropping = 1; r->next = now;
                    if (g_bombLog > 0) { g_bombLog--; logf_("bomb bay: early release %.0f units before the click (speed %.3f units/ms, height %.0f, fall %.0f ms, carry %.0f, stagger %.0f, delay -%.0f)", dist, hv, h, tf, carry, stagger, hv * g_bombLeadDelay); }
                }
            }
        }
    }
    if (!r->dropping || (int)(now - r->next) < 0) return;
    bomb_release(r);
    r->next = now + (DWORD)r->interval;
    /* the run stays registered until the engine's own drop step has passed (hook_bd_exec), or it would spawn its single
       bomb over the click after an early-released stick */
}

static int is_drop_order(uint8_t *c, float *xy, uint8_t **child)
{
    uint8_t *vt;
    if (!c || !rd_mem(c, &vt, 8) || vt != g_vtBD) return 0;
    *child = c;
    return rd_mem(c + g_bdTargetOff, xy, 8);
}

/* the eBombDropOrder among the sequence's children -> its x/y. The children are an MSVC std::deque of order pointers
   (1.067: seq+0x28 proxy, +0x30 map, +0x38 map size (power of 2), +0x40 first index, +0x48 count; 2 pointers per
   block); a {begin, end} vector is tried as well. */
static int drop_child_target(const uint8_t *seq, float *xy, uint8_t **child)
{
    for (int off = 0x10; off <= 0x40; off += 8) {
        uint64_t map, msz, moff, sz;
        if (!rd_mem(seq + off, &map, 8) || !rd_mem(seq + off + 8, &msz, 8) || !rd_mem(seq + off + 16, &moff, 8) || !rd_mem(seq + off + 24, &sz, 8)) continue;
        if (!map || !msz || msz > 256 || (msz & (msz - 1)) || !sz || sz > 32 || moff > 4096) continue;
        for (int per = 2; per >= 1; per--)                         /* block = 16 bytes: 2 pointers, or 1 16-byte element */
            for (uint64_t i = 0; i < sz; i++) {
                uint64_t idx = moff + i, blk = (idx / per) & (msz - 1);
                uint8_t *block, *c;
                if (!rd_mem((uint8_t *)map + 8 * blk, &block, 8) || !block) continue;
                if (rd_mem(block + (per == 2 ? 8 * (idx % 2) : 0), &c, 8) && is_drop_order(c, xy, child)) return 1;
            }
    }
    for (int off = 0x10; off <= 0x50; off += 8) {
        uint8_t *b, *e;
        if (!rd_mem(seq + off, &b, 8) || !rd_mem(seq + off + 8, &e, 8) || !b || e <= b || e - b > 0x100) continue;
        for (int stride = 8; stride <= 16; stride += 8) {
            if ((e - b) % stride) continue;
            for (uint8_t *q = b; q < e; q += stride) {
                uint8_t *c, *vt;
                if (!rd_mem(q, &c, 8) || !c || !rd_mem(c, &vt, 8) || vt != g_vtBD) continue;
                *child = c;
                return rd_mem(c + g_bdTargetOff, xy, 8);
            }
        }
    }
    return 0;
}

typedef struct { const void *order; const uint8_t *carrier; DWORD last; } SeenRun;   /* orders already parsed; an entry silent for RUN_STALE_MS is dropped (order addresses get reused) */
#define MAX_SEEN 32
static SeenRun g_seen[MAX_SEEN];
static int g_seenNext, g_seqDumpLog = 2;

static intptr_t hook_as_tick(void *order, intptr_t a, intptr_t b, intptr_t c)
{
    uint8_t *vt = NULL, *iface = NULL, *carrier = NULL, *child = NULL;
    float xy[2];
    char action[64] = "";
    int start = 0, actionMs = -1;
    if (rd_mem(order, &vt, 8) && vt == g_vtAS && rd_mem((uint8_t *)order + 8, &iface, 8) && iface) {
        carrier = iface - g_ifaceOff;
        BombRun *run = bomb_run_of(carrier);
        if (run) { run->last = GetTickCount(); bomb_tick(run); }   /* before the tick: the last child deletes the plane */
        int seen = 0; DWORD now = GetTickCount();
        for (int i = 0; i < MAX_SEEN; i++)
            if (g_seen[i].order == order && g_seen[i].carrier == carrier) { seen = now - g_seen[i].last <= RUN_STALE_MS; g_seen[i].last = now; break; }
        if (!seen) {
            g_seen[g_seenNext].order = order; g_seen[g_seenNext].carrier = carrier; g_seen[g_seenNext].last = now; g_seenNext = (g_seenNext + 1) % MAX_SEEN;
            if (drop_child_target(order, xy, &child)) start = bomb_action_match(child, action, sizeof action, &actionMs);   /* other airstrike names stay vanilla */
            else if (g_seqDumpLog > 0) {
                uint64_t q[11] = { 0 }; rd_mem(order, q, sizeof q); g_seqDumpLog--;
                logf_("bomb bay: airstrike %p has no drop child I can find: %llx %llx %llx %llx %llx %llx %llx %llx %llx %llx %llx", order,
                      q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7], q[8], q[9], q[10]);
            }
        }
    }
    intptr_t rv = g_asTickOrig(order, a, b, c);
    if (start) bomb_start(carrier, xy, action, actionMs);
    return rv;
}

static intptr_t hook_bd_exec(void *order, intptr_t state_, intptr_t b, intptr_t c)
{
    int state = (int)state_;
    if (state == 0) {
        uint8_t *iface = NULL; rd_mem((uint8_t *)order + 8, &iface, 8);
        uint8_t *carrier = iface ? iface - g_ifaceOff : NULL;
        BombRun *r = bomb_run_of(carrier);
        if (r) {
            if (g_bombLog > 0) { g_bombLog--; logf_("bomb bay: over the click after %.1f s, %d of %d bombs out -> %s (vanilla spawn skipped)", (GetTickCount() - r->t0) / 1000.0, r->released, r->total,
                                                    r->dropping ? "stick already running" : "releasing the bay now"); }
            r->passed = 1;
            if (!r->dropping) { r->dropping = 1; r->next = GetTickCount(); bomb_tick(r); }   /* the first bomb now, the rest on the following ticks */
            if (r->released >= r->total) r->carrier = NULL;   /* engine step passed and bay empty: forget the run before the plane is deleted */
            return 1;
        }
        char action[64]; int ms;                           /* a bay action on a plane with no usable bomb place: nothing to spawn (the name is no entity) */
        if (g_bombAction[0] && bomb_action_match((uint8_t *)order, action, sizeof action, &ms)) {
            if (g_bombLog > 0) { g_bombLog--; logf_("bomb bay: airstrike:%s on a plane without a usable bomb place -> nothing dropped", action); }
            return 1;
        }
    }
    return g_bdExecOrig(order, state_, b, c);
}

/* replace one vtable slot (the class's own vtable only: other orders keep the shared base code) */
static int patch_vslot(const char *what, uint8_t *vt, int slot, void *hook, void **orig)
{
    void **p = (void **)(vt + 8 * slot);
    DWORD old;
    if (!VirtualProtect(p, 8, PAGE_READWRITE, &old)) { logf_("%s: VirtualProtect failed (%lu)", what, GetLastError()); return 0; }
    *orig = *p; *p = hook;
    VirtualProtect(p, 8, old, &old);
    logf_("patched: %s = vtable slot exe+0x%llX (was %s exe+0x%llX)", what, (unsigned long long)((uint8_t *)p - g_base),
          in_text(*orig) ? "" : "already-swapped, outside .text:", (unsigned long long)((uint8_t *)*orig - g_base));
    return 1;
}

static int install_bomb_hooks(void)
{
    if (!patch_vslot("eAirstrike tick", g_vtAS, 3, (void *)hook_as_tick, (void **)&g_asTickOrig) ||
        !patch_vslot("eBombDropOrder::Execute", g_vtBD, 19, (void *)hook_bd_exec, (void **)&g_bdExecOrig)) return 0;
    if (g_bombVelPct > 0 && !install_hook_at("GetVelocity", (void *)g_getVel, (void *)hook_getvel, (void **)&g_getVelOrig))
        logf_("bomb bay: bombs will not carry the plane's velocity");
    if (g_strikeSetupA && !install_hook_at("StrikeSetup A", (void *)g_strikeSetupA, (void *)hook_strike_a, (void **)&g_strikeSetupAOrig)) logf_("bomb bay: altitude stays vanilla on setup A");
    if (g_strikeSetupB && !install_hook_at("StrikeSetup B", (void *)g_strikeSetupB, (void *)hook_strike_b, (void **)&g_strikeSetupBOrig)) logf_("bomb bay: altitude stays vanilla on setup B");
    return 1;
}

/* ------------------------------------------------------------------------------------ */
static void init(HMODULE self)
{
    GetModuleFileNameW(self, g_dir, MAX_PATH);
    wchar_t *bs = wcsrchr(g_dir, L'\\'); if (bs) *bs = 0;

    wchar_t exe[MAX_PATH]; GetModuleFileNameW(NULL, exe, MAX_PATH);
    wchar_t *en = wcsrchr(exe, L'\\'); en = en ? en + 1 : exe;
    if (_wcsnicmp(en, L"call_to_arms", 12)) return;        /* only the game / editor / server exes */
    if (!ini_int("Enabled", 1)) return;
    g_console = ini_int("Console", 0);
    g_verbose = ini_int("Verbose", 1);
    if (ini_int("LogFile", 1)) {
        wchar_t lp[MAX_PATH]; wcscpy(lp, g_dir); wcscat(lp, BB_LOG);
        g_log = _wfopen(lp, L"w");
    }
    if (g_console) { AllocConsole(); SetConsoleTitleA(BB_NAME); freopen("CONOUT$", "w", stdout); }

    if (!get_sections()) { logf_("not a PE I understand"); return; }
    logf_(BB_NAME " " BB_VERSION " loaded into %ls (timestamp %08X, base %p) from %ls", en, g_timestamp, g_base, g_dir);

    ini_str("BombPlace", "bomb", g_bombPlace, sizeof g_bombPlace);
    ini_str("BombEvent", "bomb_run", g_bombEvent, sizeof g_bombEvent);
    g_bombInterval = ini_int("BombInterval", 200);
    ini_str("BombAction", "marauder_bomb", g_bombAction, sizeof g_bombAction);
    g_bombVelPct = ini_int("BombVelocity", 100);
    g_bombLead = ini_int("BombLead", 1);
    g_bombAltitude = ini_int("BombAltitude", 40);
    g_bombLeadDelay = ini_int("BombLeadDelay", 1000);
    g_bombNet = ini_int("BombBayMultiplayer", 0);
    g_bombLog = g_verbose ? ini_int("BombLog", 16) : 0;

    /* weapon finders first (the bay code reads their results at run time):
       find_launch -> weapon->stuff (bomb_interval_<ms> tag) + SpawnBullet (anchor for the magazine offsets);
       find_stuff_tags -> the {tag} vector inside eStuff;
       find_weapon_item_slot + find_filling_offs -> loaded round count = number of bombs dropped. Without the
       last two no run can start (0.6 standalone shipped without them: every place read as "rounds -1"), but the
       vtable hooks are still installed so the drop step skips its vanilla spawn of the (non-existent) action name. */
    int okLaunch = find_launch();
    if (!okLaunch || !find_stuff_tags()) { g_stuffOff = 0; logf_("bomb bay: weapon stuff tags not found - bomb_interval_<ms> weapon tags ignored (action suffix / ini still work)"); }
    int okMag = okLaunch && find_weapon_item_slot() && find_filling_offs();
    if (!okMag) { g_weaponItemSlot = 0; g_fillStuffOff = 0; logf_("bomb bay: magazine offsets not found - loaded rounds unknown: NO bombs will be dropped (the hooks still go in so a bay action never spawns its non-entity name)"); }

    if (find_bombbay() && install_bomb_hooks())
        logf_("bomb bay airstrikes           ON (action \"airstrike:%s[_Nms][_Nm]\", place \"%s*\", events \"%s\" / \"%s_dropN\", interval %d ms or tag bomb_interval_N, altitude %d m%s, velocity %d%%%s, lead %d (release %d ms late), multiplayer %s)",
              g_bombAction[0] ? g_bombAction : "<any>", g_bombPlace, g_bombEvent, g_bombEvent, g_bombInterval, g_bombAltitude,
              (g_strikeSetupAOrig || g_strikeSetupBOrig) ? "" : " (hook off)", g_bombVelPct, g_getVelOrig ? "" : " (hook off)", g_bombLead, g_bombLeadDelay, g_bombNet ? "on" : "off");
    else logf_("bomb bay airstrikes           OFF (engine code not recognised)");
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        InitializeCriticalSection(&g_logLock);
        init(h);
    }
    return TRUE;
}
#endif /* BB_TEST */

#ifdef BB_TEST
/* bb_test.exe <exe> : map the exe's sections at its preferred base and run every finder (the post-update check;
   also what to run when the launcher log says OFF). Exit code 0 = every finder resolved. */
int main(int argc, char **argv)
{
    if (argc < 2) { printf("usage: bb_test <call_to_arms*.exe>\n"); return 2; }
    FILE *f = fopen(argv[1], "rb"); if (!f) { printf("cannot open %s\n", argv[1]); return 2; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *file = malloc(n); if (fread(file, 1, n, f) != (size_t)n) { printf("short read\n"); return 2; } fclose(f);
    uint32_t pe = *(uint32_t *)(file + 0x3c);
    uint16_t nsec = *(uint16_t *)(file + pe + 6), opt = *(uint16_t *)(file + pe + 20);
    uint32_t imgsz = *(uint32_t *)(file + pe + 24 + 56);
    uint64_t base = *(uint64_t *)(file + pe + 24 + 24);
    g_base = VirtualAlloc((void *)base, imgsz, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);   /* real base: vftables hold absolute pointers */
    if (g_base != (uint8_t *)base) { printf("cannot map at %llX\n", (unsigned long long)base); return 2; }
    for (int i = 0; i < nsec; i++) {
        uint8_t *sh = file + pe + 24 + opt + 40 * i;
        uint32_t vs = *(uint32_t *)(sh + 8), va = *(uint32_t *)(sh + 12), rs = *(uint32_t *)(sh + 16), raw = *(uint32_t *)(sh + 20);
        memcpy(g_base + va, file + raw, rs < vs ? rs : vs);
        Sec s = { g_base + va, vs };
        if (!strncmp((char *)sh, ".text", 8)) g_text = s; else if (!strncmp((char *)sh, ".rdata", 8)) g_rdata = s; else if (!strncmp((char *)sh, ".data", 8)) g_data = s;
    }
    printf("%s timestamp %08X\n", argv[1], *(uint32_t *)(file + pe + 8));
    int ok = find_launch() && find_stuff_tags() && find_weapon_item_slot() && find_filling_offs() && find_bombbay();
    if (ok) printf("hookable prologues: GetVelocity %zu, StrikeSetup A %zu, StrikeSetup B %zu bytes (0 = not hookable)\n",
                   prolog_len((uint8_t *)g_getVel), g_strikeSetupA ? prolog_len(g_strikeSetupA) : 0, g_strikeSetupB ? prolog_len(g_strikeSetupB) : 0);
    printf(ok ? "ALL FOUND\n" : "FAILED\n");
    return !ok;
}
#endif
