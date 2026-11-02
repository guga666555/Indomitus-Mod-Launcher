/*
 * GEM Converge 0.4  -  Call to Arms: Gates of Hell (GEM engine)
 *
 * 0.4 adds "overcharge" (ammo swap by unit tag): while the shooter entity carries the script tag
 * "overcharge" (ini OverchargeTag), every shot is fired as the alternate ammo whose set-file name tags
 * are the loaded ammo's plus "overcharge" (ini OverchargeSuffix): plasmagun_mag.ammo -> plasmagun_mag.overcharge.ammo.
 * The weapon item's filling pointer is swapped for the duration of eBulletShooter::Launch only, so the
 * engine's own per-ammo lookups (weapon {parameters "<tag>"} by the ammo's {tag}s, bullet ammo, tracer)
 * see the alternate round, while the magazine, reload and unload logic never see it (no overcharge
 * item can ever land in an inventory). Round count still comes from the loaded magazine.
 *
 * Per-weapon opt-ins, stuff tags on the gun's set file:
 *   "converge"            every barrel of a multi-barrel weapon fires at the point the gun is aimed at,
 *                         instead of all barrels firing parallel to the single foresight bone.
 *   "bullets_per_shot_<N>"         one trigger pull spawns N real bullets inside a cone ("bullet_spread_<deg>",
 *                         default ini BulletSpread). Ammo is still one round per pull; each pellet does the
 *                         ammo's full damage, so tune the ammo down.
 *
 * Ships as a VCOMP140.dll proxy (game, editor and server import it from System32); every export
 * is forwarded to %SystemRoot%\System32\vcomp140.dll.
 *
 * Engine side (eWeaponShooterBullet.cpp, all addresses found at runtime):
 *   eBulletShooter::Launch (vtable slot 11) -> ComputeDir(shooter, out dir) builds ONE direction
 *   from the aimer (foresight bone X axis + spread + zeroing), then
 *   SpawnBullet(shooter, syncIdx, float3 *zero, float3 *dir) takes the barrel's "shooterNN" bone
 *   position as the start point and calls Bullet::Launch(pos, dir + zero, speed).
 *   The aimer has GetAimPoint(aimer, out double3): raycast along the foresight for the gun's range
 *   (direct control) or the cached target point; the engine calls it itself after every shot.
 *
 * Hook: 14-byte inline jmp on SpawnBullet. For tagged weapons the direction is rotated by
 * (normalize(aimPoint - shooterPos) - normalize(foresightDir)): spread and zeroing survive, only
 * the parallax between the barrel and the sight is taken out.
 *
 * Build (zig / mingw-w64):  build.bat.   Offline check:  build.bat test  ->  cv_test.exe <exe>
 */
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
#include <math.h>

#define CV_VERSION "0.6"
#ifdef CV_BOMBBAY_ONLY                 /* standalone bomb-bay build (-DCV_BOMBBAY_ONLY): own ini/log names, no other feature */
#define CV_NAME    "GEM BombBay"
#define CV_INI     L"\\gem_bombbay.ini"
#define CV_LOG     L"\\gem_bombbay.log"
#else
#define CV_NAME    "GEM Converge"
#define CV_INI     L"\\gem_converge.ini"
#define CV_LOG     L"\\gem_converge.log"
#endif
#define CV_SECT    L"converge"

#ifndef CV_TEST
/* ------------------------------------------------------------------------------------ */
/* vcomp140 forwarders                                                                  */
/* ------------------------------------------------------------------------------------ */
#define FWD(name) FARPROC gcv_##name;
#include "proxy_exports.h"
#undef FWD

#define FWD(name) \
    __declspec(dllexport) __attribute__((naked)) void name(void) { \
        __asm__ volatile("jmp *gcv_" #name "(%rip)"); }
#include "proxy_exports.h"
#undef FWD

static void load_real_dll(void)
{
    wchar_t path[MAX_PATH];
    UINT n = GetSystemDirectoryW(path, MAX_PATH);
    if (!n) return;
    wcscat(path, L"\\vcomp140.dll");
    HMODULE real = LoadLibraryW(path);
    if (!real) return;
#define FWD(name) gcv_##name = GetProcAddress(real, #name);
#include "proxy_exports.h"
#undef FWD
}
#endif /* CV_TEST */

/* ------------------------------------------------------------------------------------ */
/* logging / ini                                                                        */
/* ------------------------------------------------------------------------------------ */
#ifdef CV_TEST
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
    snprintf(line, sizeof line, "%02d:%02d:%02d [" CV_NAME "] %s\n", st.wHour, st.wMinute, st.wSecond, buf);
    EnterCriticalSection(&g_logLock);
    if (g_log) { fputs(line, g_log); fflush(g_log); }
    if (g_console) { fputs(line, stdout); fflush(stdout); }
    LeaveCriticalSection(&g_logLock);
    OutputDebugStringA(line);
}

static void ini_str(const char *key, const char *def, char *out, size_t n)
{
    wchar_t ini[MAX_PATH]; wcscpy(ini, g_dir); wcscat(ini, CV_INI);
    wchar_t wkey[64], wdef[128], wout[256]; MultiByteToWideChar(CP_ACP, 0, key, -1, wkey, 64); MultiByteToWideChar(CP_ACP, 0, def, -1, wdef, 128);
    GetPrivateProfileStringW(CV_SECT, wkey, wdef, wout, 256, ini);
    WideCharToMultiByte(CP_ACP, 0, wout, -1, out, (int)n, NULL, NULL);
}

static int ini_int(const char *key, int def)
{
    wchar_t ini[MAX_PATH]; wcscpy(ini, g_dir); wcscat(ini, CV_INI);
    wchar_t wkey[64]; MultiByteToWideChar(CP_ACP, 0, key, -1, wkey, 64);
    return (int)GetPrivateProfileIntW(CV_SECT, wkey, def, ini);
}
#endif /* CV_TEST */

/* ------------------------------------------------------------------------------------ */
/* PE helpers                                                                           */
/* ------------------------------------------------------------------------------------ */
typedef struct { uint8_t *p; size_t n; } Sec;
static Sec g_text, g_rdata, g_data;
static uint8_t *g_base;

#ifndef CV_TEST
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
/* engine bindings                                                                      */
/* ------------------------------------------------------------------------------------ */
typedef struct { union { char buf[16]; char *ptr; } u; size_t size, cap; } MsString;

typedef char    (*SpawnBulletFn)(void *shooter, int syncIdx, float *zero, float *dir);
typedef char    (*GetShooterBoneFn)(void *shooter, void **bone, MsString *name);
typedef void    (*BoneRefreshFn)(void *bone);
typedef double *(*GetAimPointFn)(void *aimer, double *out);
typedef double *(*AimerGetVecFn)(void *aimer);
typedef char    (*IsNetGameFn)(void);
typedef int     (*NextSyncIdFn)(void);

typedef void    (*BulletLaunchFn)(void *bullet, float *pos, float *vel, float gravity);
typedef char    (*BulletQuantFn)(void *bullet);
typedef void   *(*GetHeldItemFn)(void *humanPart);

static uint8_t *g_launch;
static SpawnBulletFn    g_spawnBullet;
static GetShooterBoneFn g_getShooterBone;
static BoneRefreshFn    g_boneRefresh;
static GetAimPointFn    g_getAimPoint;
static IsNetGameFn      g_isNetGame;
static NextSyncIdFn     g_nextSyncId;
static BulletLaunchFn   g_bulletLaunch;     /* Bullet::Launch(bullet, pos, vel, gravityScale) */
static BulletQuantFn    g_bulletQuant;      /* Bullet::Quant(bullet) -> keep alive */
static GetHeldItemFn    g_getHeldItem;      /* human part -> held weapon item (the aimer's owner) */
static uint8_t *g_hitVec;                   /* aim raycast result vector {begin, end}; record: entity*, float3 point */
static uint8_t *g_quantMs;                  /* ms per quant (int) */
static uint32_t g_weaponOff, g_stuffOff, g_syncedOff, g_aimerOff, g_bonePosOff, g_boneXOff;
static uint32_t g_stuffTagsBeg, g_stuffTagsEnd, g_aimerDirSlot, g_aimerPosSlot;
static uint32_t g_ownerOff, g_ownerItemOff, g_itemFlagOff, g_aimTargetOff;

/* Bullet layout (eBullet.cpp, 1.067 game/editor/server identical): */
#define BUL_AMMO     0xA0    /* ammo stuff (its tag vector carries "atgm") */
#define BUL_TICKS    0xA8    /* int16 ticks since launch (closed-form flight time) */
#define BUL_FLAGS    0xAA    /* uint16: 0x400 = finished */
#define BUL_OWNER    0xB0    /* owner entity */
#define BUL_ORIGIN   0xC8    /* float3 launch origin */
#define BUL_VEL      0xD4    /* float3 velocity, units per tick */
#define BUL_POS      0xE0    /* float3 current position */
#define BUL_SPEED    0xEC    /* |velocity| */
#define ENT_POS      0x74    /* entity world position, float3 (the tick reads the owner's here) */

#define BONE_FLAGS  0x10    /* bone: bit 0 = world matrix stale, refresh first (the engine's own test) */
#define BONE_STALE  0x01
#define TAG_SIZE    0x60    /* Tag vector element */
#define TAG_STR     0x40    /* MSVC std::string inside the element */

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
/* GetShooterBone body: mov rax,[rcx+8] (weapon) ; mov r9,[rax+disp32] (aimer) ; mov rax,[r9+20] (aim bone) */
static const uint8_t k_gsbBody1[] = { 0x48,0x8B,0x41 };
static const uint8_t k_gsbBody2[] = { 0x4C,0x8B,0x88 };
static const uint8_t k_gsbBody3[] = { 0x49,0x8B,0x41,0x20 };
/* GetAimPoint body: mov rax,[rcx+10] (aim bone) ... mov r8,[rax+90] (weapon->owner), both in the first 0x40 bytes */
static const uint8_t k_apBody1[] = { 0x48,0x8B,0x41,0x10 };
static const uint8_t k_apBody2[] = { 0x4C,0x8B,0x80,0x90,0x00,0x00,0x00 };

/* eBulletShooter vftable via MSVC RTTI: TypeDescriptor <- CompleteObjectLocator <- vftable[-1] */
static int find_launch(void)
{
    uint8_t *name = find_cstr_in(g_data, ".?AVeBulletShooter@Weapon@Actor@@");    /* TypeDescriptors live in .data */
    if (!name) { logf_("Launch: RTTI name not found"); return 0; }
    uint32_t tdRel = (uint32_t)(name - 0x10 - g_base);
    uint8_t *vt = NULL; int hits = 0;
    for (size_t i = 12; i + 4 <= g_rdata.n; i += 4) {
        if (*(uint32_t *)(g_rdata.p + i) != tdRel || *(uint32_t *)(g_rdata.p + i - 12) != 1) continue;
        uint64_t col = (uint64_t)(g_rdata.p + i - 12);
        for (size_t j = 0; j + 8 <= g_rdata.n; j += 8)
            if (*(uint64_t *)(g_rdata.p + j) == col) { vt = g_rdata.p + j + 8; hits++; }
    }
    if (hits != 1 || !in_rdata(vt)) { logf_("Launch: vftable %s", hits ? "ambiguous" : "not found"); return 0; }
    uint8_t *f = *(uint8_t **)(vt + 11 * 8);
    if (!in_text(f) || memcmp(f, k_launchProlog, sizeof k_launchProlog)) { logf_("Launch: slot 11 not recognised"); return 0; }
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

static int find_spawn_bullet_parts(void)
{
    uint8_t *f = (uint8_t *)g_spawnBullet;
    uint8_t *gsb = call_target(f, 0x100, 0);
    if (!gsb) { logf_("SpawnBullet: GetShooterBone call not found"); return 0; }
    /* GetShooterBone: mov rax,[rcx+8] ; mov r9,[rax+aimer] ; mov rax,[r9+20]  (register allocation differs per build, so match the triple) */
    uint8_t *q = NULL, *w = NULL;
    for (uint8_t *p = gsb; p < gsb + 0x60 && !q; p++) {
        if (memcmp(p, k_gsbBody1, 3)) continue;
        for (uint8_t *r = p + 4; r < p + 0x18; r++)       /* register shuffles may sit between the two loads */
            if (!memcmp(r, k_gsbBody2, 3) && !memcmp(r + 7, k_gsbBody3, 4)) { w = p; q = r; break; }
    }
    if (!q) { logf_("GetShooterBone: body not recognised"); return 0; }
    g_getShooterBone = (GetShooterBoneFn)gsb;
    g_weaponOff = w[3];
    g_aimerOff = *(uint32_t *)(q + 3);
    if (g_aimerOff < 0x40 || g_aimerOff > 0x200) { logf_("GetShooterBone: aimer offset out of range"); return 0; }
    /* after that call:  test byte [bone+10],1 ; je ; mov rcx,bone ; call BoneRefresh ; lea rax,[bone+pos]  (bone register varies) */
    uint8_t *p = NULL;
    for (uint8_t *s = f; s < f + 0x200; s++) {
        int rex = s[0] == 0x41;
        const uint8_t *t = s + rex;
        if (t[0] == 0xF6 && (t[1] & 0xC0) == 0x40 && (t[1] & 0x38) == 0 && t[2] == BONE_FLAGS && t[3] == BONE_STALE) { p = s; break; }
    }
    if (!p) { logf_("SpawnBullet: bone stale test not found"); return 0; }
    uint8_t *call = find_bytes(p, 0x14, (const uint8_t *)"\xE8", 1);
    if (!call) { logf_("SpawnBullet: bone refresh call not found"); return 0; }
    g_boneRefresh = (BoneRefreshFn)(call + 5 + *(int32_t *)(call + 1));
    if (!in_text((void *)g_boneRefresh)) { logf_("SpawnBullet: bone refresh target bad"); return 0; }
    g_bonePosOff = 0;
    for (uint8_t *s = call + 5; s < call + 0x20; s++)
        if ((s[0] & 0xF0) == 0x40 && s[1] == 0x8D && (s[2] & 0xF8) == 0x80 && (s[2] & 7) != 4) {   /* lea rax,[reg+disp32] */
            uint32_t d = *(uint32_t *)(s + 3);
            if (d >= 0x40 && d <= 0x200) { g_bonePosOff = d; break; }
        }
    if (!g_bonePosOff) { logf_("SpawnBullet: bone position offset not found"); return 0; }
    g_boneXOff = g_bonePosOff - 36;    /* float 3x4: X, Y, Z axes then position */
    {   /* the engine reads the X axis too: lea reg,[bone+X] must exist in SpawnBullet */
        int seen = 0;
        for (uint8_t *s = call; s < f + 0x200; s++)
            if ((s[0] & 0xF0) == 0x40 && s[1] == 0x8D && (s[2] & 0xC0) == 0x80 && (s[2] & 7) != 4 && *(uint32_t *)(s + 3) == g_boneXOff) seen = 1;
        if (!seen) { logf_("SpawnBullet: bone X axis offset not confirmed"); return 0; }
    }
    logf_("GetShooterBone = exe+0x%llX, BoneRefresh = exe+0x%llX, shooter->weapon +0x%X, weapon->aimer +0x%X, bone pos +0x%X, bone X +0x%X",
          (unsigned long long)(gsb - g_base), (unsigned long long)((uint8_t *)g_boneRefresh - g_base), g_weaponOff, g_aimerOff, g_bonePosOff, g_boneXOff);
    return 1;
}

/* GetAimPoint: reached from the FireNow profile block -> post-fire bookkeeping -> the only callee
   with the GetAimPoint prologue. Its virtual calls, in order: range, direction, position, speed. */
static int find_aim_point(void)
{
    uint8_t *s = find_cstr("eBulletShooter::FireNow");
    if (!s) { logf_("AimPoint: 'eBulletShooter::FireNow' not found"); return 0; }
    uint8_t *lea = find_lea_to(g_text.p, g_text.n, s, -1, 0);
    if (!lea) { logf_("AimPoint: FireNow xref not found"); return 0; }
    int hits = 0;
    for (int k = 0; k < 24; k++) {
        uint8_t *c = call_target(lea, 0x140, k);
        if (!c) break;
        for (int m = 0; m < 48; m++) {
            uint8_t *t = call_target(c, 0x700, m);
            if (!t) break;
            if (find_bytes(t, 0x40, k_apBody1, sizeof k_apBody1) && find_bytes(t, 0x40, k_apBody2, sizeof k_apBody2)) {
                if (g_getAimPoint != (GetAimPointFn)t) hits++;
                g_getAimPoint = (GetAimPointFn)t;
            }
        }
    }
    if (hits != 1) { logf_("AimPoint: %s", hits ? "ambiguous" : "not found"); return 0; }
    uint8_t *f = (uint8_t *)g_getAimPoint;
    int n = 0; uint32_t slots[4] = {0};
    for (size_t i = 0; i + 3 <= 0x200 && n < 4; i++)
        if (f[i] == 0xFF && f[i + 1] == 0x50) slots[n++] = f[i + 2];
    if (n < 3) { logf_("AimPoint: aimer vtable calls not found"); return 0; }
    g_aimerDirSlot = slots[1]; g_aimerPosSlot = slots[2];
    logf_("GetAimPoint = exe+0x%llX, aimer GetDirection slot +0x%X, GetPosition slot +0x%X",
          (unsigned long long)(f - g_base), g_aimerDirSlot, g_aimerPosSlot);
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

/* function start = 'mov rax,rsp' right after int3 padding, walking back from a reference inside it */
static uint8_t *func_start_before(uint8_t *ref, size_t maxback)
{
    for (uint8_t *p = ref; p > ref - maxback && p > g_text.p + 1; p--)
        if (p[-1] == 0xCC && p[0] == 0x48 && p[1] == 0x8B && p[2] == 0xC4) return p;
    return NULL;
}

/* Bullet::Launch owns the "Invalid bullet velocity - %g" assert */
static int find_bullet_launch(void)
{
    uint8_t *s = find_cstr("Invalid bullet velocity - %g");
    if (!s) { logf_("Bullet::Launch: assert string not found"); return 0; }
    uint8_t *lea = find_lea_to(g_text.p, g_text.n, s, 0x15, 0);
    if (!lea) { logf_("Bullet::Launch: string xref not found"); return 0; }
    uint8_t *f = func_start_before(lea, 0x400);
    if (!f) { logf_("Bullet::Launch: prologue not found"); return 0; }
    g_bulletLaunch = (BulletLaunchFn)f;
    logf_("Bullet::Launch = exe+0x%llX", (unsigned long long)(f - g_base));
    return 1;
}

/* Bullet::Quant = the function that tests the ammo for the global "atgm" Tag and also calls Bullet::Launch
   (the other user of that Tag is the hit handler). Inside it: mov rcx,[owner+disp32] ; call GetHeldItem. */
static int find_quant(void)
{
    uint8_t *str = find_cstr("atgm");
    if (!str) { logf_("Quant: 'atgm' not found"); return 0; }
    uint8_t *init = find_lea_to(g_text.p, g_text.n, str, 0x15, 0);
    if (!init) { logf_("Quant: 'atgm' static init not found"); return 0; }
    uint8_t *tag = NULL;
    for (uint8_t *q = init + 7; q < init + 0x20; q++)
        if (q[0] == 0x48 && q[1] == 0x8D && q[2] == 0x0D) { tag = q + 7 + *(int32_t *)(q + 3); break; }
    if (!tag) { logf_("Quant: global atgm tag not found"); return 0; }
    for (int k = 0; k < 8 && !g_bulletQuant; k++) {
        uint8_t *use = find_lea_to(g_text.p, g_text.n, tag, 0x15, k);
        if (!use) break;
        uint8_t *f = func_start_before(use, 0x1000);
        if (!f) continue;
        for (int c = 0; c < 64; c++) {
            uint8_t *t = call_target(f, 0x400, c);
            if (!t) break;
            if (t == (uint8_t *)g_bulletLaunch) { g_bulletQuant = (BulletQuantFn)f; break; }
        }
    }
    if (!g_bulletQuant) { logf_("Quant: function not found"); return 0; }
    uint8_t *f = (uint8_t *)g_bulletQuant;
    /* ms per quant: first  mov r14d,[rip+g]  (44 8B 35) */
    for (uint8_t *p = f; p < f + 0x400; p++)
        if (p[0] == 0x44 && p[1] == 0x8B && p[2] == 0x35) { g_quantMs = p + 7 + *(int32_t *)(p + 3); break; }
    /* owner -> human part: GetAimPoint reads the same field (mov rax,[r8+disp32], 49 8B 80) */
    uint8_t *ap = (uint8_t *)g_getAimPoint;
    for (uint8_t *p = ap; p < ap + 0x60; p++)
        if (p[0] == 0x49 && p[1] == 0x8B && p[2] == 0x80) { g_ownerItemOff = *(uint32_t *)(p + 3); break; }
    if (!g_ownerItemOff) { logf_("Quant: human part offset not found in GetAimPoint"); return 0; }
    /* in Quant:  mov rcx,[rcx+part] ; call GetHeldItem   (48 8B 89 disp32 E8) */
    for (uint8_t *p = f; p < f + 0x1000 && !g_getHeldItem; p++)
        if (p[0] == 0x48 && p[1] == 0x8B && p[2] == 0x89 && *(uint32_t *)(p + 3) == g_ownerItemOff && p[7] == 0xE8)
            g_getHeldItem = (GetHeldItemFn)(p + 12 + *(int32_t *)(p + 8));
    if (!g_getHeldItem || !in_text((void *)g_getHeldItem)) { logf_("Quant: GetHeldItem not found"); return 0; }
    logf_("Bullet::Quant = exe+0x%llX, GetHeldItem = exe+0x%llX, owner->human part +0x%X, quant ms %s",
          (unsigned long long)(f - g_base), (unsigned long long)((uint8_t *)g_getHeldItem - g_base), g_ownerItemOff, g_quantMs ? "found" : "NOT found");
    return 1;
}

/* GetAimPoint extras: weapon->owner (mov r8,[rax+disp32]), the direct-control flag the raycast branch tests
   (mov rax,[r8+disp32] ; mov r8d,[rax+disp8]), the cached target point (movups xmm0,[aimer+disp8]) and the
   raycast result vector globals (mov rax,[rip+g] ; cmp rax,[rip+g+8]). */
static int find_aim_extras(void)
{
    uint8_t *f = (uint8_t *)g_getAimPoint;
    if (!find_disp32(f, 0x40, (const uint8_t *)"\x4C\x8B\x80", 0x40, 0x200, &g_ownerOff)) { logf_("AimPoint: owner offset not found"); return 0; }
    uint32_t partOff = 0;
    for (uint8_t *p = f; p < f + 0x60; p++)
        if (p[0] == 0x49 && p[1] == 0x8B && p[2] == 0x80) { partOff = *(uint32_t *)(p + 3); break; }
    if (!partOff || (g_ownerItemOff && partOff != g_ownerItemOff)) { logf_("AimPoint: human part offset mismatch (0x%X vs 0x%X)", partOff, g_ownerItemOff); return 0; }
    g_ownerItemOff = partOff;
    for (uint8_t *p = f; p < f + 0x60; p++)
        if (p[0] == 0x44 && p[1] == 0x8B && p[2] == 0x40) { g_itemFlagOff = p[3]; break; }
    if (!g_itemFlagOff) { logf_("AimPoint: direct-control flag offset not found"); return 0; }
    for (uint8_t *p = f; p < f + 0x200; p++)
        if (p[0] == 0x0F && p[1] == 0x10 && (p[2] & 0xF8) == 0x40 && (p[2] & 7) != 4 && p[3] >= 0x40 && p[3] <= 0xA0) { g_aimTargetOff = p[3]; break; }
    if (!g_aimTargetOff) { logf_("AimPoint: cached target offset not found"); return 0; }
    for (uint8_t *p = f; p < f + 0x200; p++)
        if (p[0] == 0x48 && p[1] == 0x8B && p[2] == 0x05 && p[7] == 0x48 && p[8] == 0x3B && p[9] == 0x05) {
            uint8_t *a = p + 7 + *(int32_t *)(p + 3), *b = p + 14 + *(int32_t *)(p + 10);
            if (b == a + 8) { g_hitVec = a; break; }
        }
    if (!g_hitVec) { logf_("AimPoint: raycast result vector not found"); return 0; }
    logf_("weapon->owner +0x%X, owner->human part +0x%X, control flag +0x%X, aimer target +0x%X, hit vector @ exe+0x%llX",
          g_ownerOff, g_ownerItemOff, g_itemFlagOff, g_aimTargetOff, (unsigned long long)(g_hitVec - g_base));
    return 1;
}

/* ---- overcharge bindings (0.4) ------------------------------------------------------------- */
/* eStuff registry (eStuffs.cpp): a global {eStuff* begin, end} vector walked by the tag lookup that logs
   "No Stuff found for tags"; each eStuff keeps its set-file NAME tags ("ammo plasmagun_mag overcharge") in
   a Tags object whose vector sits at stuff+0xB80/0xB88 (the {tag} tags are the other Tags object, vector at
   +0x11E0). The weapon's loaded magazine is the weapon ITEM's filling: item+0x50 = ammo eStuff*, +0x58 =
   rounds (eInventoryItem::Fill); eAmmo (weapon+0xB0) reaches the item through weapon vtable slot 3. */
static uint8_t *g_stuffReg;           /* {begin, end} of eStuff* */
static uint32_t g_nameTagsOff;        /* eStuff: name-tag vector begin (end = +8) */
static uint32_t g_weaponItemSlot;     /* weapon vtable byte offset of GetItem() */
static uint32_t g_fillStuffOff;       /* item: filling stuff pointer */
static uint32_t g_weaponAmmoOff;      /* weapon: eAmmo* (logged only) */
static uint32_t g_entTagsOff;         /* entity: script tag container */

#define TC_SELF_A   0x308   /* TagContainer: owner back-pointers (== container itself) */
#define TC_SELF_B   0x310
#define TC_VEC_BEG  0x318
#define TC_VEC_END  0x320

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

static int find_stuff_registry(void)
{
    uint8_t *s = find_cstr("No Stuff found for tags '%1%' (may lead to crash)");
    if (!s) { logf_("stuff registry: error string not found"); return 0; }
    uint8_t *lea = find_lea_to(g_text.p, g_text.n, s, 0x15, 0);
    if (!lea) { logf_("stuff registry: string xref not found"); return 0; }
    uint8_t *fn = NULL;
    /* the lookup is a call shortly before the error: its body does  lea rax,[rip+registry] ; mov [rsp+20h],rax */
    for (uint8_t *p = lea; p > lea - 0x100 && !fn; p--) {
        if (p[0] != 0xE8) continue;
        uint8_t *t = p + 5 + *(int32_t *)(p + 1);
        if (!in_text(t)) continue;
        for (uint8_t *q = t; q < t + 0x120; q++) {
            if (q[0] != 0x48 || q[1] != 0x8D || q[2] != 0x05) continue;
            uint8_t *g = q + 7 + *(int32_t *)(q + 3);
            if (!in_data(g)) continue;
            if (find_bytes(q + 7, 0x40, (const uint8_t *)"\x48\x89\x44\x24\x20", 5)) { fn = t; g_stuffReg = g; break; }
        }
    }
    if (!fn) { logf_("stuff registry: lookup function not recognised"); return 0; }
    /* name tags: the loop reads  mov r,[stuff+d] ; mov r,[stuff+d+8]  (vector begin/end), d > 0x400 */
    uint32_t disps[96]; int nd = 0;
    for (uint8_t *q = fn; q < fn + 0x300 && nd < 96; q++) {
        if ((q[0] != 0x48 && q[0] != 0x49 && q[0] != 0x4C && q[0] != 0x4D) || q[1] != 0x8B || (q[2] & 0xC0) != 0x80 || (q[2] & 7) == 4) continue;
        uint32_t d = *(uint32_t *)(q + 3);
        if (d >= 0x400 && d <= 0x1000) disps[nd++] = d;
    }
    for (int i = 0; i < nd && !g_nameTagsOff; i++)
        for (int j = 0; j < nd; j++)
            if (disps[j] == disps[i] + 8) { g_nameTagsOff = disps[i]; break; }
    if (!g_nameTagsOff) { logf_("stuff registry: name tag vector offset not found"); return 0; }
    logf_("stuff registry @ exe+0x%llX (lookup exe+0x%llX), name tags @ stuff+0x%X", (unsigned long long)(g_stuffReg - g_base), (unsigned long long)(fn - g_base), g_nameTagsOff);
    return 1;
}

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

/* entity script tags (same finder as GEM NoProne): action table { "tags", ctor } -> eval: add rdx,imm32 */
static int find_entity_tags_off(void)
{
    uint8_t *ctor = NULL; int hits = 0;
    for (size_t i = 0; i + 16 <= g_data.n; i += 8) {
        uint8_t *str = *(uint8_t **)(g_data.p + i), *fn = *(uint8_t **)(g_data.p + i + 8);
        if (!in_rdata(str) || !in_text(fn)) continue;
        if (memcmp(str, "tags", 5) || str[-1] != 0) continue;
        ctor = fn; hits++;
    }
    if (hits != 1) { logf_("entity tags: action table entry %s", hits ? "ambiguous" : "not found"); return 0; }
    uint8_t *vt = NULL;
    for (uint8_t *p = ctor; p < ctor + 0x80; p++)
        if (p[0] == 0x48 && p[1] == 0x8D && p[2] == 0x05) { vt = p + 7 + *(int32_t *)(p + 3); break; }
    if (!vt || !in_rdata(vt)) { logf_("entity tags: vtable not found"); return 0; }
    uint8_t *ev = *(uint8_t **)(vt + 8);
    if (!in_text(ev)) { logf_("entity tags: eval not in .text"); return 0; }
    for (uint8_t *q = ev; q < ev + 0x100; q++)
        if (q[0] == 0x48 && q[1] == 0x81 && q[2] == 0xC2) { g_entTagsOff = *(uint32_t *)(q + 3); break; }
    if (g_entTagsOff < 0x100 || g_entTagsOff > 0x400) { logf_("entity tags: container offset not found"); g_entTagsOff = 0; return 0; }
    logf_("entity {tags} @ entity+0x%X", g_entTagsOff);
    return 1;
}

/* {if linked} condition eval (eCondition_Linked vtable slot 1): bit 28 of [entity+0x1C0] = linked, [entity+0x560] -> link,
   [link+8] = the entity it is linked to (a crewman's vehicle). Vehicle guns are owned by their gunner, so an overcharge
   tag on the vehicle is found through this link. */
static uint32_t g_entLinkFlagOff, g_entLinkOff, g_linkEntOff;
static int find_linked(void)
{
    uint8_t *vt = find_vtable(".?AUeCondition_Linked@Interaction@@");
    if (!vt) { logf_("linked: vftable not found"); return 0; }
    uint8_t *f = *(uint8_t **)(vt + 8);
    if (!in_text(f)) { logf_("linked: eval not in .text"); return 0; }
    /* mov eax,[ent+d32] ([41] 8B modrm mod=10 reg=eax) ; shr eax,1Ch (C1 E8 1C) ; ... mov rax,[ent+d32] (48/49 8B modrm) ; mov rdx,[rax+d8] (48 8B 50 xx)
       (game: ent in rcx; editor: ent in r9, after an assert) */
    uint8_t *p = NULL, *q = NULL;
    for (uint8_t *s = f; s < f + 0x80 && !p; s++) {
        int rex = s[0] == 0x41;
        const uint8_t *t = s + rex;
        if (t[0] != 0x8B || (t[1] & 0xF8) != 0x80 || (t[1] & 7) == 4) continue;
        if (!memcmp(t + 6, "\xC1\xE8\x1C", 3)) { p = s; g_entLinkFlagOff = *(uint32_t *)(t + 2); }
    }
    if (!p) { logf_("linked: flag test not recognised"); return 0; }
    for (uint8_t *s = p + 9; s < p + 0x80 && !q; s++) {
        if ((s[0] != 0x48 && s[0] != 0x49) || s[1] != 0x8B || (s[2] & 0xC0) != 0x80 || (s[2] & 7) == 4) continue;   /* mov r64,[ent+d32] (game rax<-rcx, editor rcx<-rax) */
        if (s[7] == 0x48 && s[8] == 0x8B && (s[9] & 0xC0) == 0x40 && (s[9] & 7) != 4 && s[10] <= 0x20) { q = s; g_entLinkOff = *(uint32_t *)(s + 3); g_linkEntOff = s[10]; }   /* then mov r64,[link+d8] */
    }
    if (!q) { logf_("linked: link load not recognised"); return 0; }
    if (g_entLinkFlagOff < 0x100 || g_entLinkFlagOff > 0x400 || g_entLinkOff < 0x400 || g_entLinkOff > 0x1000) { logf_("linked: offsets out of range"); return 0; }
    logf_("linked: flag [entity+0x%X] bit 28, link [entity+0x%X] -> entity +0x%X", g_entLinkFlagOff, g_entLinkOff, g_linkEntOff);
    return 1;
}

/* eInventoryItem::Fill (the "Inventory Item filling: failed, item has no stuff" function): item+0x40 = the item's own
   stuff, stuff+0x474 = filling capacity (the loaded count is clamped to it) */
static uint32_t g_itemStuffOff, g_fillCapOff;
static int find_item_fill(void)
{
    uint8_t *s = find_cstr("Inventory Item filling: failed, item has no stuff");
    if (!s) { logf_("item fill: string not found"); return 0; }
    uint8_t *lea = find_lea_to(g_text.p, g_text.n, s, -1, 0);
    if (!lea) { logf_("item fill: xref not found"); return 0; }
    /* function start: int3 padding then  mov [rsp+8],rbx ; mov [rsp+10h],rsi ; push rdi ; sub rsp,imm32 */
    uint8_t *f = NULL;
    for (uint8_t *s = lea; s > lea - 0x400 && s > g_text.p + 1; s--)
        if (s[-1] == 0xCC && !memcmp(s, "\x48\x89\x5C\x24\x08\x48\x89\x74\x24\x10\x57\x48\x81\xEC", 14)) { f = s; break; }
    if (!f) { logf_("item fill: prologue not found"); return 0; }
    uint8_t *ld = NULL;
    for (uint8_t *p = f + 18; p < lea; p++) if (p[0] == 0x48 && p[1] == 0x8B && (p[2] & 0xC0) == 0x40 && (p[2] & 7) != 4 && p[3] >= 0x20 && p[3] <= 0x80) { ld = p; break; }   /* first mov r64,[r+d8]: item->stuff */
    if (!ld) { logf_("item fill: item->stuff load not found"); return 0; }
    g_itemStuffOff = ld[3];
    uint32_t cap = 0;
    for (uint8_t *p = ld; p < lea; p++) if (p[0] == 0x8B && (p[1] & 0xC7) == 0x83) { uint32_t d = *(uint32_t *)(p + 2); if (d > 0x400 && d < 0x600 && d > cap) cap = d; }  /* mov r32,[rbx+d32] */
    if (!cap) { logf_("item fill: capacity offset not found"); return 0; }
    g_fillCapOff = cap;
    logf_("item->stuff +0x%X, filling capacity @ stuff+0x%X", g_itemStuffOff, g_fillCapOff);
    return 1;
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

/* ---- bomb bay airstrikes (0.6) ---------------------------------------------------------- */
/* Units-set {action "airstrike:<entity>"}: the host's handler spawns the squad, then StrikeSetup(carrier, name, spawn,
   exit, target*) points the plane and gives it Order::eAirstrike = MoveTo(target), eBombDropOrder(name), MoveTo(exit),
   eDestroyOrder (a second setup, exe+0xB827B0 in 1.067, builds the same sequence). eBombDropOrder::Execute (vtable slot
   19) only spawns entity <name> at the target's x/y and the plane's height; the plane's own bomb weapon is never used.
   With this feature the first tick of every eAirstrike (vtable slot 3, patched in the vtable, so every build and both
   setups) gives the plane's bomb place(s) (place name starting with ini BombPlace, "bomb1") the engine's own "fire N
   shots at a point" command (the call Order::eWeaponFire makes: weapon, eTarget, mode, 3, count), so the bombs leave the
   bay through eBombShooter with the plane's velocity, ammo, barrels and the {on fire "bomb1"} script event. The target
   is the eBombDropOrder child's own point. eBombDropOrder::Execute (also a vtable patch) then skips its spawn when the
   bay has already released (ini BombFallback: else spawn as vanilla and stop the weapon). The carrier also gets script
   event "bomb_run" (ini BombEvent) at the start of the run (bomb doors). Orders only exist on the host / in SP. Layout:
   entity+0xDD8 = weaponry, weaponry+0x18 = place* vector, place+0x18 = place name Tags (string +0x40), place+0x2B8 =
   weapon (from the weapon_work action and GetHeldItem). */
typedef intptr_t (*OrderFn)(void *order, intptr_t a, intptr_t b, intptr_t c);
typedef void (*WeaponFireCmdFn)(void *weapon, void *target, int mode, int r9, int count);
typedef void (*WeaponStopFn)(void *weapon, int how);
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
static uint8_t *g_bdExec, *g_asTick, *g_vtBD, *g_vtAS, *g_vtTarget;
static ObjFn g_tagsCtor, g_targetReset, g_tagsDtor, g_evDtor;
static TagCtorFn g_tagCtor;
static EvInitFn g_evInit;
static EvSendFn g_evSend;
static WeaponFireCmdFn g_weaponFireCmd;
static WeaponStopFn g_weaponStop;
static uint32_t g_ifaceOff, g_bdTargetOff, g_weaponryOff, g_placesOff, g_placeNameOff, g_placeWeaponOff;

static uint8_t *vslot(uint8_t *vt, int i) { uint8_t *f = *(uint8_t **)(vt + 8 * i); return in_text(f) ? f : NULL; }
static uint8_t *call_after(uint8_t *p, size_t n) { uint8_t *c = find_bytes(p, n, (const uint8_t *)"\xE8", 1); return c ? c + 5 + *(int32_t *)(c + 1) : NULL; }

static int find_bombbay(void)
{
    uint8_t *vtBD = find_vtable(".?AUeBombDropOrder@Order@@"), *vtAS = find_vtable(".?AUeAirstrike@Order@@"),
            *vtWF = find_vtable(".?AUeWeaponFire@Order@@"), *vtWW = find_vtable(".?AUeWeaponWork@Interaction@@"),
            *vtAA = find_vtable(".?AUeAirAttack@Order@@");
    g_vtTarget = find_vtable(".?AVeTarget@@");
    if (!vtBD || !vtAS || !vtWF || !vtWW || !vtAA || !g_vtTarget) {
        logf_("bomb bay: vtables not found (drop %d strike %d fire %d work %d attack %d target %d)", !!vtBD, !!vtAS, !!vtWF, !!vtWW, !!vtAA, !!g_vtTarget); return 0; }
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
    /* eWeaponFire::Execute: first call = held/place weapon (mov rax,[rax+placeWeapon]) ; mov r9d,3 ... call FireCmd */
    uint8_t *wf = vslot(vtWF, 19), *held = wf ? call_target(wf, 0x40, 0) : NULL;
    if (!held || !find_disp32(held, 0x20, (const uint8_t *)"\x48\x8B\x80", 0x100, 0x800, &g_placeWeaponOff)) { logf_("bomb bay: place->weapon offset not found"); return 0; }
    if (!(p = find_bytes(wf, 0x300, (const uint8_t *)"\x41\xB9\x03\x00\x00\x00", 6)) || !(g_weaponFireCmd = (WeaponFireCmdFn)call_after(p, 0x20))) { logf_("bomb bay: weapon fire command not found"); return 0; }
    /* FireCmd: target type 0 -> mov edx,1 ; mov rcx,weapon ; call Stop */
    if (!(p = find_bytes((uint8_t *)g_weaponFireCmd, 0x100, (const uint8_t *)"\xBA\x01\x00\x00\x00", 5)) || !(g_weaponStop = (WeaponStopFn)call_after(p, 0x10))) { logf_("bomb bay: weapon stop not found"); return 0; }
    /* eWeaponFire dtor (slot 0): eTarget reset, then the target's Tags dtor */
    uint8_t *wd = vslot(vtWF, 0);
    if (!wd || !(g_targetReset = (ObjFn)call_target(wd, 0x40, 0)) || !(g_tagsDtor = (ObjFn)call_target(wd, 0x40, 1))) { logf_("bomb bay: target dtor not found"); return 0; }
    /* weapon_work execute (slot 9): mov rcx,[entity+weaponry] ; call FindPlace (mov rbx,[rcx+places] ; add rcx,name) */
    uint8_t *ww = vslot(vtWW, 9);
    if (!ww || !find_disp32(ww, 0x20, (const uint8_t *)"\x49\x8B\x88", 0x400, 0x2000, &g_weaponryOff)) { logf_("bomb bay: weaponry offset not found"); return 0; }
    uint8_t *fp = call_target(ww, 0x30, 0);
    if (!fp || !(p = find_bytes(fp, 0x20, (const uint8_t *)"\x48\x8B\x59", 3))) { logf_("bomb bay: place vector not found"); return 0; }
    g_placesOff = p[3];
    if (!(p = find_bytes(fp, 0x60, (const uint8_t *)"\x48\x83\xC1", 3))) { logf_("bomb bay: place name offset not found"); return 0; }
    g_placeNameOff = p[3];
    /* Tags ctor: the weapon_work action factory ({ "weapon_work", factory } in .data) = operator new, then Tags ctor */
    uint8_t *fac = NULL;
    for (size_t i = 0; i + 16 <= g_data.n && !fac; i += 8) {
        uint8_t *str = *(uint8_t **)(g_data.p + i), *fn = *(uint8_t **)(g_data.p + i + 8);
        if (in_rdata(str) && in_text(fn) && !memcmp(str, "weapon_work", 12) && str[-1] == 0) fac = fn;
    }
    if (!fac || !(g_tagsCtor = (ObjFn)call_target(fac, 0x60, 1))) { logf_("bomb bay: Tags ctor not found"); return 0; }
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
    logf_("bomb bay: airstrike tick = exe+0x%llX, drop Execute = exe+0x%llX, FireCmd = exe+0x%llX, carrier iface +0x%X, drop target +0x%X, weaponry +0x%X, places +0x%X, place name +0x%X, place weapon +0x%X, event %s",
          (unsigned long long)(g_asTick - g_base), (unsigned long long)(g_bdExec - g_base), (unsigned long long)((uint8_t *)g_weaponFireCmd - g_base),
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
static StrikeSetupFn g_strikeSetupAOrig, g_strikeSetupBOrig;

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

#ifndef CV_TEST
/* ------------------------------------------------------------------------------------ */
/* hook                                                                               */
/* ------------------------------------------------------------------------------------ */
static SpawnBulletFn g_spawnBulletOrig;
static char g_tag[64] = "converge";
static size_t g_tagLen;
static double g_minDist = 3.0;
static int g_allowNet = 0;
static int g_logShots = 10;

static double g_bulletSpread = 3.0;   /* cone half-angle, degrees */
static int g_bulletsMax = 32;

static char g_lockTag[64] = "lock_on";
static size_t g_lockTagLen;
static double g_lockTurn = 10.0;      /* deg/s, fallback steering when the engine's own guidance can't run */
static int g_lockLog = 10;

static int rd_mem(const void *src, void *dst, size_t n)
{
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), src, dst, n, &got) && got == n;
}

/* MSVC RTTI class name of a heap object, or NULL when it does not look like one of the exe's classes */
static const char *rtti_name(const void *obj, char *buf, size_t n)
{
    const uint8_t *vt, *col; uint32_t tdRel;
    if (!rd_mem(obj, &vt, 8) || !in_rdata(vt)) return NULL;
    if (!rd_mem(vt - 8, &col, 8) || !in_rdata(col)) return NULL;
    if (!rd_mem(col + 12, &tdRel, 4)) return NULL;
    const uint8_t *name = g_base + tdRel + 0x10;
    if (name < g_data.p || name >= g_data.p + g_data.n) return NULL;
    if (!rd_mem(name, buf, n < 4 ? n : 4) || memcmp(buf, ".?AV", 4)) return NULL;
    size_t i = 0;
    for (; i + 1 < n; i++) { if (!rd_mem(name + i, buf + i, 1) || !buf[i]) break; }
    buf[i] = 0;
    return buf;
}

/* one pass over the weapon's stuff tags: "converge", "bullets_per_shot_<N>", "bullet_spread_<deg>", "lock_on" */
static int g_scanLock;
static void scan_tags(const uint8_t *stuff, int *conv, int *pellets, double *spread)
{
    *conv = 0; *pellets = 0; *spread = g_bulletSpread; g_scanLock = 0;
    const uint8_t *b = *(const uint8_t *const *)(stuff + g_stuffTagsBeg), *e = *(const uint8_t *const *)(stuff + g_stuffTagsEnd);
    if (!b || e < b || (size_t)(e - b) > TAG_SIZE * 256) return;
    for (const uint8_t *t = b; t < e; t += TAG_SIZE) {
        const char *s = (const char *)(t + TAG_STR);
        size_t len = *(const size_t *)(t + TAG_STR + 0x10), cap = *(const size_t *)(t + TAG_STR + 0x18);
        if (cap > 15) s = *(const char *const *)(t + TAG_STR);
        if (len >= 64) continue;
        if (len == g_tagLen && !_strnicmp(s, g_tag, len)) { *conv = 1; continue; }     /* engine lowercases tags; ini value may not be */
        if (len == g_lockTagLen && !_strnicmp(s, g_lockTag, len)) { g_scanLock = 1; continue; }
        if (len > 17 && !_strnicmp(s, "bullets_per_shot_", 17)) { int n = atoi(s + 17); if (n >= 2 && n <= g_bulletsMax) *pellets = n; continue; }
        if (len > 14 && !_strnicmp(s, "bullet_spread_", 14)) { int n = atoi(s + 14); if (n >= 1 && n <= 45) *spread = n; continue; }
    }
}

static double vnorm(double *v) { double n = sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]); if (n > 1e-9) { v[0] /= n; v[1] /= n; v[2] /= n; } return n; }

/* splitmix64: pellet offsets must be identical on every lockstep machine, so they come from the shot's own
   geometry (direction + aim point bits) instead of a local RNG */
static uint64_t mix64(uint64_t z) { z += 0x9E3779B97F4A7C15ull; z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull; z = (z ^ (z >> 27)) * 0x94D049BB133111EBull; return z ^ (z >> 31); }
static uint64_t bits_d(double v) { uint64_t u; memcpy(&u, &v, 8); return u; }

/* ---- lock-on ------------------------------------------------------------------------- */
/* A lock = the entity (or ground point) under the sight when the shot left. Every tick we hand the
   engine's own "atgm" guidance that point instead of the live aim point, so the gunner may look away. */
typedef struct {
    void *bullet;        /* key */
    float origin[3];     /* bullet+ORIGIN at lock time: a reused bullet slot has a different one */
    void *ent;           /* locked entity, NULL = fixed point */
    const void *entVt;   /* its vtable at lock time (cheap liveness check) */
    float off[3];        /* hit point relative to the entity position */
    float point[3];      /* last target point */
    int fallback;        /* steered by us, not by the engine */
    int ticks;           /* quant calls seen (log) */
    int driftN;          /* weapon drift_<N> (0 = none), resolved on the first tick */
    float dperp[3];      /* drift direction (unit, perpendicular to the launch velocity) */
    double dtan;         /* drift slope: target offset = dtan * distance flown */
} Lock;
#define MAX_LOCKS 64
static Lock g_locks[MAX_LOCKS];
static int g_pendingLock;            /* set by SpawnBullet for the Launch that follows */
static Lock g_pending;

static Lock *lock_find(void *bullet)
{
    for (int i = 0; i < MAX_LOCKS; i++) if (g_locks[i].bullet == bullet) return &g_locks[i];
    return NULL;
}

static void lock_target(const uint8_t *weapon, void *aimer, const double *aim)
{
    memset(&g_pending, 0, sizeof g_pending);
    g_pending.point[0] = (float)aim[0]; g_pending.point[1] = (float)aim[1]; g_pending.point[2] = (float)aim[2];
    const uint8_t *owner = *(const uint8_t *const *)(weapon + g_ownerOff);
    uint8_t *beg = NULL, *end = NULL;
    rd_mem(g_hitVec, &beg, 8); rd_mem(g_hitVec + 8, &end, 8);
    char name[96] = "";
    /* hit record (0x48 bytes, engine readers: cmp [rec],1 / cmp [rec],4 / movss [rec+4] / entity flags via
       [[rec+0x18]+0x30]): +0 int type (1 = terrain: material looked up at the point; 4 = entity volume),
       +4 float distance, +8 float3 point, +0x18 entity* */
    if (beg && end > beg) {
        float hp[3]; void *cand = NULL; int type = 0;
        if (rd_mem(beg, &type, 4) && type != 1 && rd_mem(beg + 8, hp, 12) && rd_mem(beg + 0x18, &cand, 8) && cand && cand != owner &&
            fabs(hp[0] - aim[0]) < 1.0 && fabs(hp[1] - aim[1]) < 1.0 && fabs(hp[2] - aim[2]) < 1.0 &&
            rtti_name(cand, name, sizeof name)) {
            float ep[3];
            if (rd_mem((uint8_t *)cand + ENT_POS, ep, 12)) {
                g_pending.ent = cand;
                rd_mem(cand, &g_pending.entVt, 8);
                for (int i = 0; i < 3; i++) g_pending.off[i] = g_pending.point[i] - ep[i];
            }
        }
    }
    (void)aimer;
    g_pendingLock = 1;
    if (g_lockLog > 0) {
        g_lockLog--;
        int type = -1; if (beg && end > beg) rd_mem(beg, &type, 4);
        logf_("lock: %s at %.1f %.1f %.1f (%d hit records, first type %d)", g_pending.ent ? name : "ground point", aim[0], aim[1], aim[2],
              (beg && end > beg) ? (int)((end - beg) / 0x48) : 0, type);
    }
}

/* aim point with the direct-control flag forced on, so the raycast runs for AI shots as well */
static void aim_point_raycast(const uint8_t *weapon, void *aimer, double *aim)
{
    const uint8_t *owner = *(const uint8_t *const *)(weapon + g_ownerOff);
    uint8_t *part = NULL; uint32_t flags = 0, *fp = NULL;
    if (owner && rd_mem(owner + g_ownerItemOff, &part, 8) && part && rd_mem(part + g_itemFlagOff, &flags, 4)) fp = (uint32_t *)(part + g_itemFlagOff);
    if (fp && !(flags & 2)) *fp = flags | 2;
    g_getAimPoint(aimer, aim);
    if (fp && !(flags & 2)) *fp = flags;
}

/* overcharge (0.4.5): the loaded magazine of a tagged unit is kept as the overcharge sibling (persistent swap, see the
   overcharge section); SpawnBullet only makes sure the right magazine is in when a bullet is created (a SyncedProjectiles
   spawn could run outside Launch), it never swaps back */
typedef struct OcCtx OcCtx;
static int oc_resolve(void *shooter, OcCtx *c);
static char (*g_launchOrig)(void *shooter);   /* set when the overcharge Launch hook is installed (defined below) */
static int g_ocSpawnLog = 8;
static int g_ocSpawnCalls;       /* SpawnBullet calls seen (hook_launch reports how many happened inside Launch) */
static int g_ocStandardShot;     /* set by hook_launch while a tagged unit fires a standard round (too few rounds left) */
static void oc_spawn_fixup(void *shooter, int syncIdx);

static char hook_spawn_bullet_inner(void *shooter, int syncIdx, float *zero, float *dir);
static char hook_spawn_bullet(void *shooter, int syncIdx, float *zero, float *dir)
{
    g_ocSpawnCalls++;
    if (g_launchOrig) oc_spawn_fixup(shooter, syncIdx);
    return hook_spawn_bullet_inner(shooter, syncIdx, zero, dir);
}

static char hook_spawn_bullet_inner(void *shooter, int syncIdx, float *zero, float *dir)
{
    const uint8_t *weapon = *(const uint8_t *const *)((const uint8_t *)shooter + g_weaponOff);
    if (!weapon) goto pass;
    const uint8_t *stuff = *(const uint8_t *const *)(weapon + g_stuffOff);
    if (!stuff) goto pass;
    int conv, pellets; double spread;
    scan_tags(stuff, &conv, &pellets, &spread);
    int lock = g_scanLock && g_bulletQuant;
    if (!conv && !pellets && !lock) goto pass;
    int net = g_isNetGame && g_isNetGame();
    if (!g_allowNet && net) goto pass;
    void *aimer = *(void **)(weapon + g_aimerOff);
    if (!aimer) goto pass;
    if (lock) {
        double aim[3];
        aim_point_raycast(weapon, aimer, aim);
        lock_target(weapon, aimer, aim);
        if (!conv && !pellets) { char r = g_spawnBulletOrig(shooter, syncIdx, zero, dir); g_pendingLock = 0; return r; }
    }

    double d[3] = { dir[0], dir[1], dir[2] };
    double len = vnorm(d);
    if (len < 1e-9) goto pass;
    double nd[3] = { d[0], d[1], d[2] };
    double aim[3] = { 0, 0, 0 }, dist = 0;
    const char *boneName = "-";
    MsString name = { {{0}}, 0, 15 };

    if (conv) {
        void *bone = NULL;
        char ok = g_getShooterBone(shooter, &bone, &name);
        /* "shooterNN" fits the SSO buffer; a longer name would leak 1 heap block (no engine deallocator binding) */
        if (ok && bone) {
            boneName = name.cap > 15 ? name.u.ptr : name.u.buf;
            uint8_t *b = (uint8_t *)bone;
            if (b[BONE_FLAGS] & BONE_STALE) g_boneRefresh(bone);
            const float *bpos = (const float *)(b + g_bonePosOff);
            g_getAimPoint(aimer, aim);
            double cv[3] = { aim[0] - bpos[0], aim[1] - bpos[1], aim[2] - bpos[2] };
            dist = vnorm(cv);
            const double *fd = ((AimerGetVecFn)(*(void ***)aimer)[g_aimerDirSlot / 8])(aimer);
            double nom[3] = { fd[0], fd[1], fd[2] };
            if (dist >= g_minDist && vnorm(nom) > 1e-9) {
                double c[3] = { d[0] + cv[0] - nom[0], d[1] + cv[1] - nom[1], d[2] + cv[2] - nom[2] };
                if (vnorm(c) > 1e-9) { nd[0] = c[0]; nd[1] = c[1]; nd[2] = c[2]; }
            }
        }
    }

    if (g_logShots > 0) {
        g_logShots--;
        double dot = d[0]*nd[0] + d[1]*nd[1] + d[2]*nd[2];
        if (dot > 1) dot = 1;
        logf_("shot: bone %s, aim point %.1f away, converged %.2f deg, pellets %d (cone %.1f deg)%s", boneName, dist, acos(dot) * 57.29578, pellets, pellets ? spread : 0.0, syncIdx ? " [synced]" : "");
    }

    if (!pellets) {
        dir[0] = (float)(nd[0] * len); dir[1] = (float)(nd[1] * len); dir[2] = (float)(nd[2] * len);
        goto pass;
    }

    /* cone basis around nd */
    double up[3] = { 0, 0, 1 };
    if (fabs(nd[2]) > 0.9) { up[0] = 1; up[2] = 0; }
    double ax[3] = { nd[1]*up[2] - nd[2]*up[1], nd[2]*up[0] - nd[0]*up[2], nd[0]*up[1] - nd[1]*up[0] };
    vnorm(ax);
    double ay[3] = { nd[1]*ax[2] - nd[2]*ax[1], nd[2]*ax[0] - nd[0]*ax[2], nd[0]*ax[1] - nd[1]*ax[0] };
    uint64_t seed = mix64(bits_d(nd[0]) ^ mix64(bits_d(nd[1])) ^ mix64(bits_d(nd[2]) ^ 0x5bd1e995) ^ mix64(bits_d(aim[0]) + bits_d(aim[2])));
    double tanS = tan(spread * 0.01745329);
    char result = 0;
    for (int i = 0; i < pellets; i++) {
        uint64_t r1 = mix64(seed + 2 * (uint64_t)i), r2 = mix64(seed + 2 * (uint64_t)i + 1);
        double u1 = (double)(r1 >> 11) * (1.0 / 9007199254740992.0), u2 = (double)(r2 >> 11) * (1.0 / 9007199254740992.0);
        double rad = tanS * sqrt(u1), phi = 6.283185307 * u2;     /* uniform over the cone's cross-section */
        double ox = rad * cos(phi), oy = rad * sin(phi);
        double p[3] = { nd[0] + ax[0]*ox + ay[0]*oy, nd[1] + ax[1]*ox + ay[1]*oy, nd[2] + ax[2]*ox + ay[2]*oy };
        vnorm(p);
        dir[0] = (float)(p[0] * len); dir[1] = (float)(p[1] * len); dir[2] = (float)(p[2] * len);
        int sid = syncIdx;
        if (i > 0 && syncIdx) {
            if (!g_nextSyncId) break;             /* synced weapon on the host with no counter binding: single bullet */
            sid = g_nextSyncId();
        }
        result |= g_spawnBulletOrig(shooter, sid, zero, dir);
    }
    g_pendingLock = 0;
    return result;
pass:
    {
        char r = g_spawnBulletOrig(shooter, syncIdx, zero, dir);
        g_pendingLock = 0;
        return r;
    }
}

/* Bullet::Launch: the bullet SpawnBullet just created comes through here -> bind the pending lock to it */
static BulletLaunchFn g_bulletLaunchOrig;
static void hook_bullet_launch(void *bullet, float *pos, float *vel, float gravity)
{
    g_bulletLaunchOrig(bullet, pos, vel, gravity);
    if (!g_pendingLock) return;
    Lock *l = lock_find(bullet);
    if (!l) { for (int i = 0; i < MAX_LOCKS; i++) if (!g_locks[i].bullet) { l = &g_locks[i]; break; } }
    if (!l) return;
    *l = g_pending;
    l->bullet = bullet;
    memcpy(l->origin, (uint8_t *)bullet + BUL_ORIGIN, 12);
}

static int lock_target_pos(Lock *l, float *out)
{
    if (l->ent) {
        const void *vt = NULL; float ep[3];
        if (rd_mem(l->ent, &vt, 8) && vt == l->entVt && rd_mem((uint8_t *)l->ent + ENT_POS, ep, 12) &&
            fabs(ep[0]) < 1e7 && fabs(ep[1]) < 1e7 && fabs(ep[2]) < 1e7) {
            for (int i = 0; i < 3; i++) l->point[i] = ep[i] + l->off[i];
        } else l->ent = NULL;          /* gone: keep flying to its last position */
    }
    memcpy(out, l->point, 12);
    return 1;
}

/* ---- drift ---------------------------------------------------------------------------- */
/* weapon tag drift_<N>: the round veers off its line with a constant sideways pull, so the miss grows with
   distance (N = degrees off at 100 units, twice that at 200...). Each round draws its own strength (0..N)
   and direction from the launch geometry, so lockstep machines agree. Implemented by sliding the launch
   origin sideways every tick: the engine's closed-form flight (origin + v*t - gravity) keeps its gravity. */
#define BUL_WEAPON   0x128   /* weapon stuff */
static int g_driftMax = 45;
typedef struct { const void *stuff; int n; } DriftTagCache;
static DriftTagCache g_driftCache[32];

static int stuff_drift(const uint8_t *stuff)
{
    for (int i = 0; i < 32; i++) if (g_driftCache[i].stuff == stuff) return g_driftCache[i].n;
    int n = 0;
    const uint8_t *b = NULL, *e = NULL;
    if (rd_mem(stuff + g_stuffTagsBeg, &b, 8) && rd_mem(stuff + g_stuffTagsEnd, &e, 8) && b && e > b && (size_t)(e - b) <= TAG_SIZE * 256)
        for (const uint8_t *t = b; t < e; t += TAG_SIZE) {
            const char *s = (const char *)(t + TAG_STR);
            size_t len = *(const size_t *)(t + TAG_STR + 0x10), cap = *(const size_t *)(t + TAG_STR + 0x18);
            if (cap > 15) s = *(const char *const *)(t + TAG_STR);
            if (len > 6 && len < 64 && !_strnicmp(s, "drift_", 6)) { int v = atoi(s + 6); if (v >= 1 && v <= g_driftMax) n = v; }
        }
    static int slot;
    g_driftCache[slot] = (DriftTagCache){ stuff, n }; slot = (slot + 1) % 32;
    return n;
}

/* per-round drift draw from the launch geometry: unit direction perpendicular to v, strength tan(0..N deg) */
static int drift_params(const float *origin, const float *v, int n, float *perp, double *tanv)
{
    double vd[3] = { v[0], v[1], v[2] }, speed = vnorm(vd);
    if (speed < 1e-9) return 0;
    uint64_t h = mix64(bits_d(origin[0]) ^ mix64(bits_d(origin[1]) + bits_d(origin[2])) ^ mix64(bits_d(v[0]) ^ bits_d(v[2])));
    double u1 = (double)(h >> 11) * (1.0 / 9007199254740992.0), u2 = (double)(mix64(h) >> 11) * (1.0 / 9007199254740992.0);
    double up[3] = { 0, 0, 1 }; if (fabs(vd[2]) > 0.9) { up[0] = 1; up[2] = 0; }
    double e1[3] = { vd[1]*up[2] - vd[2]*up[1], vd[2]*up[0] - vd[0]*up[2], vd[0]*up[1] - vd[1]*up[0] }; vnorm(e1);
    double e2[3] = { vd[1]*e1[2] - vd[2]*e1[1], vd[2]*e1[0] - vd[0]*e1[2], vd[0]*e1[1] - vd[1]*e1[0] };
    double phi = 6.283185307 * u2;
    for (int i = 0; i < 3; i++) perp[i] = (float)(cos(phi) * e1[i] + sin(phi) * e2[i]);
    *tanv = tan(n * u1 * 0.01745329);
    return 1;
}

typedef struct { void *bullet; float origin[3]; float perp[3]; double kk; double dPrev; int16_t lastT; } Drift;
#define MAX_DRIFT 128
static Drift g_drift[MAX_DRIFT];

static void drift_tick(uint8_t *b, int n)
{
    float *origin = (float *)(b + BUL_ORIGIN), *v = (float *)(b + BUL_VEL);
    int16_t t = *(int16_t *)(b + BUL_TICKS);
    Drift *d = NULL, *freeSlot = NULL;
    for (int i = 0; i < MAX_DRIFT; i++) { if (g_drift[i].bullet == b) { d = &g_drift[i]; break; } if (!freeSlot && !g_drift[i].bullet) freeSlot = &g_drift[i]; }
    if (d && (memcmp(d->origin, origin, 12) || t < d->lastT)) d->bullet = NULL, d = NULL;   /* slot reused / relaunched */
    if (!d) {
        if (!freeSlot) return;
        d = freeSlot; memset(d, 0, sizeof *d); d->bullet = b;
        double vd[3] = { v[0], v[1], v[2] }, speed = vnorm(vd), tanv;
        if (!drift_params(origin, v, n, d->perp, &tanv)) { d->bullet = NULL; return; }
        d->kk = tanv / 100.0 * speed * speed;    /* D(t) = kk * t^2 (t in ticks, speed units/tick) */
        d->dPrev = d->kk * (double)t * t;
        d->lastT = t;
    }
    int ms = 0; if (g_quantMs) rd_mem(g_quantMs, &ms, 4);
    if (ms <= 0 || ms > 1000) ms = 50;
    double tn = (double)t + ms, dn = d->kk * tn * tn, step = dn - d->dPrev;
    for (int i = 0; i < 3; i++) origin[i] += (float)(d->perp[i] * step);
    memcpy(d->origin, origin, 12);
    d->dPrev = dn; d->lastT = (int16_t)(t + ms);
}

/* Bullet::Quant: for a locked bullet the engine's atgm branch reads aimer+target; swap our point in for the
   duration of the call. Without an aimer (owner dead, item dropped) steer the velocity ourselves. */
static BulletQuantFn g_bulletQuantOrig;
static char hook_bullet_quant(void *bullet)
{
    Lock *l = lock_find(bullet);
    if (!l) {
        const uint8_t *wstuff = NULL;
        if (rd_mem((uint8_t *)bullet + BUL_WEAPON, &wstuff, 8) && wstuff) {
            int n = stuff_drift(wstuff);
            if (n > 0) drift_tick((uint8_t *)bullet, n);
        }
        char alive = g_bulletQuantOrig(bullet);
        if (!alive) for (int i = 0; i < MAX_DRIFT; i++) if (g_drift[i].bullet == bullet) g_drift[i].bullet = NULL;
        return alive;
    }
    uint8_t *b = (uint8_t *)bullet;
    if (memcmp(l->origin, b + BUL_ORIGIN, 12) && !l->fallback) { l->bullet = NULL; return g_bulletQuantOrig(bullet); }   /* slot reused */
    float tp[3]; lock_target_pos(l, tp);
    /* drift on a guided round: the engine flies it from its current position (no closed form to bend),
       so the lock point itself walks sideways with the distance flown -> the miss grows with range */
    if (l->ticks == 0) {
        const uint8_t *wstuff = NULL; int n = 0;
        if (rd_mem(b + BUL_WEAPON, &wstuff, 8) && wstuff) n = stuff_drift(wstuff);
        if (n > 0 && drift_params(l->origin, (const float *)(b + BUL_VEL), n, l->dperp, &l->dtan)) l->driftN = n;
    }
    if (l->driftN > 0) {
        /* distance flown from our own launch record (the bullet's +0xFC field gave offsets far above tan(N)*d) */
        const float *pos = (const float *)(b + BUL_POS);
        double dx = pos[0] - l->origin[0], dy = pos[1] - l->origin[1], dz = pos[2] - l->origin[2];
        double flown = sqrt(dx*dx + dy*dy + dz*dz);
        if (flown > 0 && flown < 1e6) for (int i = 0; i < 3; i++) tp[i] += (float)(l->dperp[i] * l->dtan * flown);
    }

    double *aimTarget = NULL; double saved[3];
    const uint8_t *owner = *(const uint8_t *const *)(b + BUL_OWNER);
    uint8_t *part = NULL; void *item = NULL; uint8_t *aimer = NULL;
    if (owner && rd_mem(owner + g_ownerItemOff, &part, 8) && part && (item = g_getHeldItem(part)) != NULL &&
        rd_mem((uint8_t *)item + g_aimerOff, &aimer, 8) && aimer) {
        aimTarget = (double *)(aimer + g_aimTargetOff);
        memcpy(saved, aimTarget, 24);
        aimTarget[0] = tp[0]; aimTarget[1] = tp[1]; aimTarget[2] = tp[2];
    }
    char alive = g_bulletQuantOrig(bullet);
    if (l->ticks++ == 0 && g_lockLog > 0)
        logf_("lock tick: %s, target %.1f %.1f %.1f, bullet at %.1f %.1f %.1f", aimTarget ? "engine guidance (aimer target swapped)" : "no aimer -> dll steering",
              tp[0], tp[1], tp[2], ((float *)(b + BUL_POS))[0], ((float *)(b + BUL_POS))[1], ((float *)(b + BUL_POS))[2]);
    if (aimTarget) memcpy(aimTarget, saved, 24);
    else if (alive) {
        /* no engine guidance this tick: rotate the velocity toward the target (<= LockTurn deg/s) and
           restart the closed-form flight from the current position */
        float *v = (float *)(b + BUL_VEL), *pos = (float *)(b + BUL_POS);
        double vd[3] = { v[0], v[1], v[2] }, speed = vnorm(vd);
        double want[3] = { tp[0] - pos[0], tp[1] - pos[1], tp[2] - pos[2] };
        if (speed > 1e-6 && vnorm(want) > 1.0) {
            int ms = 0; if (g_quantMs) rd_mem(g_quantMs, &ms, 4);
            if (ms <= 0 || ms > 1000) ms = 50;
            double maxTurn = g_lockTurn * 0.01745329 * ms / 1000.0;
            double dot = vd[0]*want[0] + vd[1]*want[1] + vd[2]*want[2];
            if (dot > 1) dot = 1; if (dot < -1) dot = -1;
            double ang = acos(dot);
            if (ang > 1e-4) {
                double t = ang <= maxTurn ? 1.0 : maxTurn / ang;
                double nd[3] = { vd[0] + (want[0] - vd[0]) * t, vd[1] + (want[1] - vd[1]) * t, vd[2] + (want[2] - vd[2]) * t };
                if (vnorm(nd) > 1e-6) {
                    for (int i = 0; i < 3; i++) v[i] = (float)(nd[i] * speed);
                    memcpy(b + BUL_ORIGIN, pos, 12);
                    *(int16_t *)(b + BUL_TICKS) = 0;
                    memcpy(l->origin, pos, 12);
                    l->fallback = 1;
                }
            }
        }
    }
    if (!alive || (*(uint16_t *)(b + BUL_FLAGS) & 0x400)) {
        if (g_lockLog > 0) {
            const float *pos = (const float *)(b + BUL_POS);
            double dx = pos[0] - tp[0], dy = pos[1] - tp[1], dz = pos[2] - tp[2];
            double ox = pos[0] - l->origin[0], oy = pos[1] - l->origin[1], oz = pos[2] - l->origin[2];
            double flown = sqrt(ox*ox + oy*oy + oz*oz);
            logf_("lock end: %d ticks, %.1f units from the target, flew %.0f units, drift offset %.1f%s", l->ticks, sqrt(dx*dx + dy*dy + dz*dz),
                  flown, l->driftN > 0 ? l->dtan * flown : 0.0, l->fallback ? " (dll steering)" : "");
        }
        l->bullet = NULL;
    }
    return alive;
}

/* ---- overcharge ----------------------------------------------------------------------- */
/* Unit script tag "overcharge" -> the shot is fired as the ammo whose name tags are the loaded ammo's plus
   "overcharge". The swap lives only inside eBulletShooter::Launch (ComputeDir, SpawnBullet: params by the
   ammo's {tag}s, bullet ammo stuff, tracer all read the filling there); the magazine count, reload and
   unload never see the alternate stuff. */
typedef char  (*LaunchFn)(void *shooter);
typedef void *(*WeaponItemFn)(void *weapon);
static LaunchFn g_launchOrig;
static char g_ocTag[64] = "overcharge", g_ocSuffix[64] = "overcharge";
static size_t g_ocTagLen, g_ocSuffixLen;
static int g_ocLog = 10;
static int g_ocRounds = 2;            /* magazine rounds one overcharge shot costs */

#define OC_MAXTAGS 16
#define OC_TAGLEN  48

static int rd_msstr(const uint8_t *at, char *out, size_t n)
{
    MsString s; out[0] = 0;
    if (!rd_mem(at, &s, sizeof s)) return 0;
    if (s.size >= n) s.size = n - 1;
    if (s.cap > 15) { if (!rd_mem(s.u.ptr, out, s.size)) return 0; } else memcpy(out, s.u.buf, s.size);
    out[s.size] = 0;
    return 1;
}

/* entity script tag container (NoProne layout): self back-pointers at +0x308/+0x310, Tag vector +0x318/+0x320 */
static int ent_has_tag(const uint8_t *c, const char *tag, size_t len)
{
    const uint8_t *a = NULL, *b = NULL, *vb = NULL, *ve = NULL;
    if (!rd_mem(c + TC_SELF_A, &a, 8) || !rd_mem(c + TC_SELF_B, &b, 8) || a != c || b != c) return 0;
    if (!rd_mem(c + TC_VEC_BEG, &vb, 8) || !rd_mem(c + TC_VEC_END, &ve, 8) || !vb || ve < vb || (size_t)(ve - vb) > TAG_SIZE * 256) return 0;
    char s[OC_TAGLEN];
    for (const uint8_t *t = vb; t < ve; t += TAG_SIZE)
        if (rd_msstr(t + TAG_STR, s, sizeof s) && strlen(s) == len && !_strnicmp(s, tag, len)) return 1;
    return 0;
}

/* set-file name tags of an eStuff, lowercase strings; -1 when unreadable */
static int read_name_tags(const uint8_t *stuff, char out[][OC_TAGLEN], int max)
{
    const uint8_t *vb = NULL, *ve = NULL;
    if (!rd_mem(stuff + g_nameTagsOff, &vb, 8) || !rd_mem(stuff + g_nameTagsOff + 8, &ve, 8)) return -1;
    if (!vb || ve < vb || (size_t)(ve - vb) > TAG_SIZE * 64 || (ve - vb) % TAG_SIZE) return -1;
    int n = 0;
    for (const uint8_t *t = vb; t < ve && n < max; t += TAG_SIZE, n++)
        if (!rd_msstr(t + TAG_STR, out[n], OC_TAGLEN)) return -1;
    return n;
}

static void join_tags(char out[][OC_TAGLEN], int n, char *dst, size_t cap)
{
    dst[0] = 0;
    for (int i = 0; i < n; i++) { if (i) strncat(dst, " ", cap - strlen(dst) - 1); strncat(dst, out[i], cap - strlen(dst) - 1); }
}

/* base stuff s <-> overcharge sibling t (NULL = none); the name strings guard against a stale pointer after a map reload
   (every eStuff is recreated, an old address can come back as a different stuff) */
typedef struct { const void *s, *t; char sname[96], tname[96]; } OcCache;
#define OC_CACHE 64
static OcCache g_ocCache[OC_CACHE];
static int g_ocCacheN, g_ocCacheNext;

static int oc_name(const uint8_t *X, char *out, size_t cap)
{
    char xt[OC_MAXTAGS][OC_TAGLEN];
    int m = read_name_tags(X, xt, OC_MAXTAGS);
    if (m <= 0) { out[0] = 0; return 0; }
    join_tags(xt, m, out, cap);
    return m;
}

static void oc_cache_put(const void *s, const void *t, const char *sname, const char *tname)
{
    int i = g_ocCacheN < OC_CACHE ? g_ocCacheN++ : (g_ocCacheNext++ % OC_CACHE);
    g_ocCache[i].s = s; g_ocCache[i].t = t;
    strncpy(g_ocCache[i].sname, sname ? sname : "", sizeof g_ocCache[i].sname - 1); g_ocCache[i].sname[sizeof g_ocCache[i].sname - 1] = 0;
    strncpy(g_ocCache[i].tname, tname ? tname : "", sizeof g_ocCache[i].tname - 1); g_ocCache[i].tname[sizeof g_ocCache[i].tname - 1] = 0;
}

static int oc_tags_have_suffix(char xt[][OC_TAGLEN], int m)
{
    for (int j = 0; j < m; j++) if (strlen(xt[j]) == g_ocSuffixLen && !_strnicmp(xt[j], g_ocSuffix, g_ocSuffixLen)) return 1;
    return 0;
}

/* loaded ammo stuff -> its overcharge sibling (name tags = S's + suffix), cached; NULL = none */
static const uint8_t *oc_lookup(const uint8_t *S)
{
    char now[96];
    for (int i = 0; i < g_ocCacheN; i++) if (g_ocCache[i].s == S) {
        if (oc_name(S, now, sizeof now) && !strcmp(now, g_ocCache[i].sname)) return (const uint8_t *)g_ocCache[i].t;
        g_ocCache[i] = g_ocCache[--g_ocCacheN]; break;      /* stale: forget it and look again */
    }
    const uint8_t *T = NULL;
    char st[OC_MAXTAGS][OC_TAGLEN], xt[OC_MAXTAGS][OC_TAGLEN], sname[256] = "?", tname[256] = "";
    char cls[96];
    int n = read_name_tags(S, st, OC_MAXTAGS);
    const void *svt = NULL;
    if (n > 0 && n < OC_MAXTAGS - 1 && rd_mem(S, &svt, 8) && rtti_name(S, cls, sizeof cls) && strstr(cls, "eStuff")) {
        join_tags(st, n, sname, sizeof sname);
        const uint8_t *beg = NULL, *end = NULL;
        if (rd_mem(g_stuffReg, &beg, 8) && rd_mem(g_stuffReg + 8, &end, 8) && beg && end > beg && (size_t)(end - beg) < 8 * 200000) {
            const uint8_t *Tother = NULL; char oname[256] = "", ocls[96] = "?";
            for (const uint8_t *e = beg; e < end && !T; e += 8) {
                const uint8_t *X = NULL; const void *xvt = NULL;
                if (!rd_mem(e, &X, 8) || !X || X == S || !rd_mem(X, &xvt, 8)) continue;
                int m = read_name_tags(X, xt, OC_MAXTAGS);
                if (m != n + 1) continue;
                if (!oc_tags_have_suffix(xt, m)) continue;
                int ok = 1;
                for (int i = 0; i < n && ok; i++) {
                    int found = 0;
                    for (int j = 0; j < m; j++) if (!_stricmp(xt[j], st[i])) { found = 1; break; }
                    ok = found;
                }
                if (!ok) continue;
                if (xvt == svt) { T = X; join_tags(xt, m, tname, sizeof tname); }
                else if (!Tother) { Tother = X; join_tags(xt, m, oname, sizeof oname); rtti_name(X, ocls, sizeof ocls); }
            }
            if (!T && Tother) { T = Tother; strncpy(tname, oname, sizeof tname - 1); logf_("overcharge: sibling \"%s\" is class %s, loaded ammo is %s (accepted)", oname, ocls, cls); }
        }
    }
    oc_cache_put(S, T, sname, tname);
    if (T) logf_("overcharge: \"%s\" -> \"%s\"", sname, tname);
    else   logf_("overcharge: no \"%s\" sibling for \"%s\" (class %s)", g_ocSuffix, sname, n > 0 ? cls : "?");
    return T;
}

/* overcharge sibling -> its base (name tags = T's minus the suffix), cached in the same table; NULL = T is not a sibling */
static const uint8_t *oc_base(const uint8_t *T)
{
    char now[96];
    for (int i = 0; i < g_ocCacheN; i++) if (g_ocCache[i].t == T) {
        if (oc_name(T, now, sizeof now) && !strcmp(now, g_ocCache[i].tname)) return (const uint8_t *)g_ocCache[i].s;
        g_ocCache[i] = g_ocCache[--g_ocCacheN]; break;
    }
    char tt[OC_MAXTAGS][OC_TAGLEN], xt[OC_MAXTAGS][OC_TAGLEN], tname[256] = "?", sname[256] = "";
    int n = read_name_tags(T, tt, OC_MAXTAGS);
    if (n < 2 || !oc_tags_have_suffix(tt, n)) return NULL;
    join_tags(tt, n, tname, sizeof tname);
    const uint8_t *B = NULL, *beg = NULL, *end = NULL;
    if (rd_mem(g_stuffReg, &beg, 8) && rd_mem(g_stuffReg + 8, &end, 8) && beg && end > beg && (size_t)(end - beg) < 8 * 200000) {
        for (const uint8_t *e = beg; e < end && !B; e += 8) {
            const uint8_t *X = NULL;
            if (!rd_mem(e, &X, 8) || !X || X == T) continue;
            int m = read_name_tags(X, xt, OC_MAXTAGS);
            if (m != n - 1 || oc_tags_have_suffix(xt, m)) continue;
            int ok = 1;
            for (int j = 0; j < m && ok; j++) {
                int found = 0;
                for (int i = 0; i < n; i++) if (!_stricmp(tt[i], xt[j])) { found = 1; break; }
                ok = found;
            }
            if (ok) { B = X; join_tags(xt, m, sname, sizeof sname); }
        }
    }
    if (B) { oc_cache_put(B, T, sname, tname); logf_("overcharge: \"%s\" is the overcharge magazine of \"%s\"", tname, sname); }
    else logf_("overcharge: no base magazine for \"%s\"", tname);
    return B;
}

/* what the firing weapon has loaded and what it should have loaded */
struct OcCtx {
    int tagged;                 /* the firing unit (or its vehicle) carries the overcharge tag */
    const uint8_t **slot;       /* item filling stuff pointer */
    const uint8_t *cur;         /* loaded stuff */
    const uint8_t *base;        /* the standard magazine (cur itself, or the base of a loaded sibling) */
    const uint8_t *sib;         /* the overcharge magazine (cur itself when a sibling is loaded), NULL = none */
};

static int oc_resolve(void *shooter, OcCtx *c)
{
    memset(c, 0, sizeof *c);
    const uint8_t *weapon = *(const uint8_t *const *)((const uint8_t *)shooter + g_weaponOff);
    if (!weapon) return 0;
    const uint8_t *owner = *(const uint8_t *const *)(weapon + g_ownerOff);
    if (!owner) return 0;
    int tagged = ent_has_tag(owner + g_entTagsOff, g_ocTag, g_ocTagLen);
    if (!tagged && g_entLinkOff) {      /* vehicle gun: the owner is the gunner, the button tags the vehicle he is linked to */
        uint32_t fl = 0; const uint8_t *link = NULL, *veh = NULL; char cls[64];
        if (rd_mem(owner + g_entLinkFlagOff, &fl, 4) && ((fl >> 28) & 1) && rd_mem(owner + g_entLinkOff, &link, 8) && link &&
            rd_mem(link + g_linkEntOff, &veh, 8) && veh && veh != owner && rtti_name(veh, cls, sizeof cls))
            tagged = ent_has_tag(veh + g_entTagsOff, g_ocTag, g_ocTagLen);
    }
    uint8_t *item = (uint8_t *)((WeaponItemFn)(*(void ***)weapon)[g_weaponItemSlot / 8])((void *)weapon);
    if (!item) return 0;
    const uint8_t **slot = (const uint8_t **)(item + g_fillStuffOff);
    const uint8_t *cur = NULL;
    if (!rd_mem(slot, &cur, 8) || !cur) return 0;
    c->tagged = tagged; c->slot = slot; c->cur = cur;
    const uint8_t *b = oc_base(cur);
    if (b) { c->base = b; c->sib = cur; }
    else   { c->base = cur; c->sib = oc_lookup(cur); }
    return 1;
}

/* the swap is persistent: an overcharge magazine stays loaded while the unit is tagged (so the impact code, which asks the
   weapon for its current ammo, and the aiming display see it too) and goes back to the standard one on the first shot after
   the toggle is off. Rounds a partial reload returns to the inventory as the overcharge stuff fire as standard rounds when
   the unit is not tagged (same path). */
static void oc_spawn_fixup(void *shooter, int syncIdx)
{
    OcCtx c;
    if (g_ocStandardShot || !oc_resolve(shooter, &c)) return;
    if (c.tagged && c.sib && c.cur != c.sib) {
        *c.slot = c.sib;
        if (g_ocSpawnLog > 0) { g_ocSpawnLog--; logf_("overcharge: magazine swapped at SpawnBullet%s", syncIdx ? " [synced]" : ""); }
    } else if (!c.tagged && c.cur == c.sib && c.base && c.cur != c.base) *c.slot = c.base;
}

static char hook_launch(void *shooter)
{
    OcCtx c;
    if (!oc_resolve(shooter, &c)) return g_launchOrig(shooter);
    if (!c.tagged) {
        if (c.cur == c.sib && c.base && c.cur != c.base) {
            *c.slot = c.base;
            if (g_ocLog > 0) { g_ocLog--; logf_("overcharge: toggle off, standard magazine back in"); }
        }
        return g_launchOrig(shooter);
    }
    if (!c.sib) return g_launchOrig(shooter);
    uint8_t *item = (uint8_t *)c.slot - g_fillStuffOff;
    /* an overcharge shot costs OverchargeRounds (default 2): with fewer in the magazine the shot is a standard round,
       unless the magazine cannot hold that many at all (vehicle guns load 1 shell): then it costs what is there */
    int *rounds = (int *)(item + g_fillStuffOff + 8);
    int before = 0, cost = g_ocRounds;
    if (!rd_mem(rounds, &before, 4)) return g_launchOrig(shooter);
    if (g_fillCapOff) {
        const uint8_t *wstuff = NULL; int cap = 0;
        if (rd_mem(item + g_itemStuffOff, &wstuff, 8) && wstuff && rd_mem(wstuff + g_fillCapOff, &cap, 4) && cap >= 1 && cap < cost) cost = cap;
    }
    if (before < cost) {
        if (c.cur != c.base) *c.slot = c.base;
        if (g_ocLog > 0) { g_ocLog--; logf_("overcharge: %d round(s) left, standard shot", before); }
        g_ocStandardShot = 1;
        char r0 = g_launchOrig(shooter);
        g_ocStandardShot = 0;
        return r0;
    }
    if (c.cur != c.sib) *c.slot = c.sib;        /* stays loaded */
    /* item+0x58 IS the magazine count (an OverchargeRounds=3 run wrote 8 after the first shot and the next shot read 8 back:
       nothing else touches it between shots), and with the sibling loaded the engine's own consume step takes nothing at all
       (it never did in any run), so the dll charges the whole cost: whatever the engine took inside Launch plus the rest here */
    int spawnsBefore = g_ocSpawnCalls;
    char r = g_launchOrig(shooter);
    int spawnsIn = g_ocSpawnCalls - spawnsBefore;
    int after = before;
    rd_mem(rounds, &after, 4);
    int taken = (after < before) ? before - after : 0;
    int want = before - cost; if (want < 0) want = 0;
    if (after > want) *rounds = want;
    if (g_ocLog > 0) { g_ocLog--; int now = 0; rd_mem(rounds, &now, 4); logf_("overcharge shot: magazine %d -> %d (engine took %d in Launch, dll %d; %d SpawnBullet call(s) inside Launch)", before, now, taken, after - now, spawnsIn); }
    return r;
}
/* runs from DllMain, before any engine code, so nothing can be executing the patched bytes */
static int install_hook_at(const char *what, void *target, void *hook, void **orig)
{
    uint8_t *f = (uint8_t *)target;
    size_t n = prolog_len(f);
    if (!n) { logf_("%s prologue not hookable - not hooking", what); return 0; }

    uint8_t *tramp = VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) { logf_("VirtualAlloc failed"); return 0; }
    memcpy(tramp, f, n);
    tramp[n] = 0xFF; tramp[n + 1] = 0x25; *(uint32_t *)(tramp + n + 2) = 0;
    *(uint64_t *)(tramp + n + 6) = (uint64_t)(f + n);
    *orig = tramp;

    uint8_t patch[32]; memset(patch, 0x90, sizeof patch);
    patch[0] = 0xFF; patch[1] = 0x25; *(uint32_t *)(patch + 2) = 0;
    *(uint64_t *)(patch + 6) = (uint64_t)hook;
    DWORD old;
    if (!VirtualProtect(f, n, PAGE_EXECUTE_READWRITE, &old)) { logf_("VirtualProtect failed (%lu)", GetLastError()); return 0; }
    memcpy(f, patch, n);
    VirtualProtect(f, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), f, n);
    return 1;
}

static int install_hook(void)
{
    return install_hook_at("SpawnBullet", (void *)g_spawnBullet, (void *)hook_spawn_bullet, (void **)&g_spawnBulletOrig);
}

static int install_lock_hooks(void)
{
    return install_hook_at("Bullet::Launch", (void *)g_bulletLaunch, (void *)hook_bullet_launch, (void **)&g_bulletLaunchOrig) &&
           install_hook_at("Bullet::Quant", (void *)g_bulletQuant, (void *)hook_bullet_quant, (void **)&g_bulletQuantOrig);
}

static int install_oc_hook(void)
{
    return install_hook_at("eBulletShooter::Launch", (void *)g_launch, (void *)hook_launch, (void **)&g_launchOrig);
}

/* ---- button pressed state (0.5) --------------------------------------------------------- */
/* GameControl::PanelControl::eCommand::Update (eCommand vtable slot 3) draws an action-set button pressed only while its
   action is the pending one: it compares [GameControl+pending] with its action and writes the widget's pressed byte
   (widget+0x178) + refresh. For the buttons named in ini PressedButtons the hook lets that compare succeed while the selected
   unit (or vehicle) carries the overcharge tag: the pending pointer is set to the action around the original call and put
   back afterwards, so the engine itself draws the pressed frame (no flicker, nothing left behind). Single selection only
   ([GameControl+actorHolder] -> +0x28 = the selected actor); a multi-unit selection never shows the state. */
typedef void (*CmdUpdateFn)(void *cmd);
static CmdUpdateFn g_cmdUpdateOrig;
static uint8_t *g_cmdUpdate;
static uint8_t *g_gcPtr;                 /* the GameControl* global */
static uint32_t g_cmdActionOff = 0x68, g_gcPendingOff, g_gcActorHolderOff, g_holderActorOff = 0x28, g_actionNameOff = 0x60;
static char g_pressedButtons[256] = "overcharge,overcharge_vehicle";
static int g_btnLog = 3;

static int find_cmd_update(void)
{
    uint8_t *vt = find_vtable(".?AVeCommand@PanelControl@GameControl@@");
    if (!vt) { logf_("button state: eCommand vtable not found"); return 0; }
    uint8_t *upd = (uint8_t *)*(uint64_t *)(vt + 3 * 8);
    if (!in_text(upd)) { logf_("button state: eCommand::Update not in .text"); return 0; }
    uint8_t *p = find_bytes(upd, 0x40, (const uint8_t *)"\x48\x8B\x0D", 3);            /* mov rcx,[rip+GameControl] */
    if (!p) { logf_("button state: GameControl global not found"); return 0; }
    g_gcPtr = p + 7 + *(int32_t *)(p + 3);
    if (!in_data(g_gcPtr)) { logf_("button state: GameControl global outside .data"); return 0; }
    uint8_t *q = find_bytes(upd, 0x120, (const uint8_t *)"\x48\x39\x88", 3);           /* cmp [rax+pending], rcx */
    if (!q) { logf_("button state: pending-action compare not found"); return 0; }
    g_gcPendingOff = *(uint32_t *)(q + 3);
    uint8_t *a = find_bytes(upd, 0x60, (const uint8_t *)"\x4C\x8B\x47", 3);            /* mov r8,[rdi+action] */
    if (a) g_cmdActionOff = a[3];
    uint8_t *c = NULL;                                                                    /* first call = HasActor(gc): cmp [rcx+holder],0 */
    for (uint8_t *s = upd; s < upd + 0x40; s++) if (*s == 0xE8) { c = s + 5 + *(int32_t *)(s + 1); break; }
    uint8_t *h = c && in_text(c) ? find_bytes(c, 0x10, (const uint8_t *)"\x48\x83\xB9", 3) : NULL;
    if (!h) { logf_("button state: selected-actor holder not found"); return 0; }
    g_gcActorHolderOff = *(uint32_t *)(h + 3);
    g_cmdUpdate = upd;
    logf_("button state: eCommand::Update = exe+0x%llX, GameControl @ exe+0x%llX, pending action +0x%X, actor holder +0x%X, action +0x%X",
          (unsigned long long)(upd - g_base), (unsigned long long)(g_gcPtr - g_base), g_gcPendingOff, g_gcActorHolderOff, g_cmdActionOff);
    return 1;
}

static int button_listed(const char *name)
{
    size_t n = strlen(name); if (!n) return 0;
    const char *s = g_pressedButtons;
    while (*s) {
        while (*s == ' ' || *s == ',') s++;
        const char *e = s; while (*e && *e != ',') e++;
        const char *t = e; while (t > s && t[-1] == ' ') t--;
        if ((size_t)(t - s) == n && !_strnicmp(s, name, n)) return 1;
        s = e;
    }
    return 0;
}

/* 1 when this eCommand's button is listed and the single selected actor carries the overcharge tag; fills name/cls/action */
static int cmd_pressed(const void *cmd, const uint8_t **actionOut, char *name, size_t nn, char *cls, size_t nc)
{
    const uint8_t *action = NULL, *gc = NULL, *holder = NULL, *actor = NULL;
    if (!(rd_mem((const uint8_t *)cmd + g_cmdActionOff, &action, 8) && action && rd_mem(g_gcPtr, &gc, 8) && gc &&
          rd_msstr(action + g_actionNameOff, name, nn) && button_listed(name))) return 0;
    if (!(rd_mem(gc + g_gcActorHolderOff, &holder, 8) && holder && rd_mem(holder + g_holderActorOff, &actor, 8) && actor &&
          rtti_name(actor, cls, nc) && ent_has_tag(actor + g_entTagsOff, g_ocTag, g_ocTagLen))) return 0;
    if (actionOut) *actionOut = action;
    return 1;
}

static void hook_cmd_update(void *cmd)
{
    const uint8_t *action = NULL, *gc = NULL, *saved = NULL;
    int forced = 0;
    char name[64], cls[64];
    if (cmd_pressed(cmd, &action, name, sizeof name, cls, sizeof cls) && rd_mem(g_gcPtr, &gc, 8) && gc) {
        const uint8_t **pend = (const uint8_t **)(gc + g_gcPendingOff);
        saved = *pend;
        if (saved != action) { *pend = action; forced = 1; }
        if (g_btnLog > 0) { g_btnLog--; logf_("button state: \"%s\" drawn pressed (selected %s tagged \"%s\")", name, cls, g_ocTag); }
    }
    g_cmdUpdateOrig(cmd);
    if (forced) { const uint8_t **pend = (const uint8_t **)(gc + g_gcPendingOff); if (*pend == action) *pend = saved; }
}

/* The pressed byte the Update writes (widget+0x178 -> +0x148 "down") is cleared again by the widget's own mouse handling
   (hover leave / button release re-pick the look with down = 0), and Update only re-applies it when the state byte CHANGES,
   so the pressed frame survived only until the mouse left the button (Nov 1 test). eItemButton::SetLook (vtable slot 72,
   the function with the "+0x1d8 state" compare) is what every re-pick ends in: look 0 normal, 1 down, 2 hover, 3 focus,
   4 disabled. Hooked: when the widget's item (widget+0x1E0 -> eItem, whose +8 is the widget back) is a listed, tagged
   command and the look would be "normal", it is drawn "down" instead; hover/disabled are left alone. */
typedef void (*SetLookFn)(void *widget, int look);
static SetLookFn g_setLookOrig;
static uint8_t *g_setLook;
static uint32_t g_widgetItemOff = 0x1E0, g_itemWidgetOff = 0x8;
static int g_lookLog = 3;

static int find_set_look(void)
{
    uint8_t *vt = find_vtable(".?AVeItemButton@PanelControl@GameControl@@");
    if (!vt) { logf_("button look: eItemButton vtable not found"); return 0; }
    uint8_t *f = (uint8_t *)*(uint64_t *)(vt + 72 * 8);
    if (!in_text(f) || !find_bytes(f, 0x30, (const uint8_t *)"\x83\xB9\xD8\x01\x00\x00\x00", 7)) {  /* cmp dword [rcx+0x1d8],0 */
        logf_("button look: eItemButton::SetLook not recognised"); return 0;
    }
    g_setLook = f;
    logf_("button look: eItemButton::SetLook = exe+0x%llX", (unsigned long long)(f - g_base));
    return 1;
}

static int g_lookDiag = 24;   /* diagnostic lines per run: every SetLook on a listed button, plus the first unmatched calls */

static void hook_set_look(void *widget, int look)
{
    const uint8_t *item = NULL, *back = NULL, *action = NULL; char name[64], cls[64];
    int linked = rd_mem((const uint8_t *)widget + g_widgetItemOff, &item, 8) && item &&
                 rd_mem(item + g_itemWidgetOff, &back, 8) && back == (const uint8_t *)widget;
    if (linked && rd_mem(item + g_cmdActionOff, &action, 8) && action &&
        rd_msstr(action + g_actionNameOff, name, sizeof name) && button_listed(name)) {
        int pressed = cmd_pressed(item, NULL, name, sizeof name, cls, sizeof cls);
        /* diagnostic: every look call on a listed button while its selected unit is tagged (panel build spam is untagged) */
        if (pressed && g_lookDiag > 0) { g_lookDiag--; logf_("button look: \"%s\" look %d -> %d (widget %p, selected %s tagged)", name, look, look == 0 ? 1 : look, widget, cls); }
        if (look == 0 && pressed) look = 1;
    }
    g_setLookOrig(widget, look);
}

static int install_button_hook(void)
{
    return install_hook_at("eCommand::Update", (void *)g_cmdUpdate, (void *)hook_cmd_update, (void **)&g_cmdUpdateOrig) &&
           install_hook_at("eItemButton::SetLook", (void *)g_setLook, (void *)hook_set_look, (void **)&g_setLookOrig);
}

/* ---- bomb bay airstrikes (0.6), finders above -------------------------------------------- */
static OrderFn g_asTickOrig, g_bdExecOrig;
static char g_bombPlace[64] = "bomb", g_bombEvent[64] = "bomb_run";
static int g_bombFallback = 1, g_bombNet = 0, g_bombLog = 16;
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
    char s[OC_TAGLEN];
    for (const uint8_t *t = b; t < e; t += TAG_SIZE)
        if (rd_msstr(t + TAG_STR, s, sizeof s) && !_strnicmp(s, "bomb_interval_", 14)) { int ms = atoi(s + 14); if (ms >= 0 && ms <= 10000) return ms; }
    return g_bombInterval;
}
static GetVelFn g_getVelOrig;                     /* GetVelocity hook (the plane reads 0 there: planes fly without a physics body) */
static int g_bombVelActive, g_bombVelLog = 3;
static float g_bombVel[3];

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
    DWORD t0;
} BombRun;

/* while the dll releases a bomb, the engine's GetVelocity(owner) answers with the plane's measured velocity */
static float *hook_getvel(void *entity, float *out)
{
    if (!g_bombVelActive) return g_getVelOrig(entity, out);
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
        snprintf(names + strlen(names), sizeof names - strlen(names), "%s%s (rounds %d, shooter %s)", r.n ? ", " : "", nm, rounds, s ? "found" : "NOT found");
        if (!s || rounds <= 0) continue;
        if (rounds > 16) rounds = 16;
        if (!r.n) r.interval = actionMs >= 0 ? actionMs : bomb_interval_of(w);
        r.weapon[r.n] = w; r.shooter[r.n] = s; r.left[r.n] = rounds; r.total += rounds; r.n++;
    }
    if (g_bombLog > 0) { g_bombLog--; logf_("bomb bay: run started (airstrike:%s), target (%.1f %.1f), %s, interval %d ms", action, xy[0], xy[1], names[0] ? names : "no bomb place", r.interval); }
    for (int i = 0; i < MAX_RUNS; i++) if (g_runs[i].carrier == carrier) g_runs[i].carrier = NULL;   /* a reused entity address */
    if (!r.n) return;
    r.carrier = carrier; r.tx = xy[0]; r.ty = xy[1]; r.t0 = GetTickCount();
    g_runs[g_runNext] = r; g_runNext = (g_runNext + 1) % MAX_RUNS;
    bomb_event(carrier, g_bombEvent);
}

static BombRun *bomb_run_of(const uint8_t *carrier)
{
    for (int i = 0; i < MAX_RUNS && carrier; i++) if (g_runs[i].carrier == carrier) return &g_runs[i];
    return NULL;
}

/* one bomb out of the bay: the bomb shooter's own Launch on the next barrel, one round off the magazine, event
   "<BombEvent>_drop<N>" (N = barrel + 1, matching the mdl's bomb_01..04 bones) */
static void bomb_release(BombRun *r)
{
    for (int k = 0; k < r->n; k++) {
        if (r->left[k] <= 0) continue;
        uint8_t *s = r->shooter[k], *item = weapon_item(r->weapon[k]);
        int barrel = r->barrel[k]++;
        *(int32_t *)(s + 0x10) = barrel;
        for (int i = 0; i < 3; i++) g_bombVel[i] = r->haveVel ? r->vel[i] * (g_bombVelPct / 100.f) : 0.f;
        g_bombVelActive = g_getVelOrig && g_bombVelPct > 0;
        g_bombLaunch(s);
        g_bombVelActive = 0;
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
    if (r->released >= r->total) return;
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

typedef struct { const void *order; const uint8_t *carrier; } SeenRun;
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
        if (run) bomb_tick(run);                           /* before the tick: the last child deletes the plane */
        int seen = 0;
        for (int i = 0; i < MAX_SEEN; i++) if (g_seen[i].order == order && g_seen[i].carrier == carrier) { seen = 1; break; }
        if (!seen) {
            g_seen[g_seenNext].order = order; g_seen[g_seenNext].carrier = carrier; g_seenNext = (g_seenNext + 1) % MAX_SEEN;
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
        wchar_t lp[MAX_PATH]; wcscpy(lp, g_dir); wcscat(lp, CV_LOG);
        g_log = _wfopen(lp, L"w");
    }
    if (g_console) { AllocConsole(); SetConsoleTitleA(CV_NAME); freopen("CONOUT$", "w", stdout); }

    if (!get_sections()) { logf_("not a PE I understand"); return; }
    logf_(CV_NAME " " CV_VERSION " loaded into %ls (timestamp %08X)", en, g_timestamp);

    /* bomb bay airstrikes: independent of the features below (it reads their finds lazily at run time) */
    if (ini_int("BombBay", 1)) {
        ini_str("BombPlace", "bomb", g_bombPlace, sizeof g_bombPlace);
        ini_str("BombEvent", "bomb_run", g_bombEvent, sizeof g_bombEvent);
        g_bombInterval = ini_int("BombInterval", 200);
        ini_str("BombAction", "marauder_bomb", g_bombAction, sizeof g_bombAction);
        g_bombVelPct = ini_int("BombVelocity", 100);
        g_bombLead = ini_int("BombLead", 1);
        g_bombAltitude = ini_int("BombAltitude", 40);
        g_bombLeadDelay = ini_int("BombLeadDelay", 1000);
        g_bombFallback = ini_int("BombFallback", 1);
        g_bombNet = ini_int("BombBayMultiplayer", 0);
        g_bombLog = g_verbose ? ini_int("BombLog", 16) : 0;
        if (find_bombbay() && install_bomb_hooks())
            logf_("bomb bay airstrikes           ON (action \"airstrike:%s[_Nms][_Nm]\", place \"%s*\", events \"%s\" / \"%s_dropN\", interval %d ms or tag bomb_interval_N, altitude %d m%s, velocity %d%%%s, lead %d (release %d ms late), fallback %d, multiplayer %s)", g_bombAction[0] ? g_bombAction : "<any>", g_bombPlace, g_bombEvent, g_bombEvent, g_bombInterval, g_bombAltitude, (g_strikeSetupAOrig || g_strikeSetupBOrig) ? "" : " (hook off)", g_bombVelPct, g_getVelOrig ? "" : " (hook off)", g_bombLead, g_bombLeadDelay, g_bombFallback, g_bombNet ? "on" : "off");
        else logf_("bomb bay airstrikes           OFF (engine code not recognised)");
    }

#ifdef CV_BOMBBAY_ONLY
    /* standalone bomb-bay build: the weapon finders only serve the bomb_interval_<ms> stuff-tag lookup, no other hooks */
    if (!(find_launch() && find_stuff_tags())) { g_stuffOff = 0; logf_("bomb bay: weapon stuff tags not found - bomb_interval_<ms> weapon tags ignored (action suffix / ini still work)"); }
    return;
#endif
    ini_str("Tag", "converge", g_tag, sizeof g_tag);
    g_tagLen = strlen(g_tag);
    if (!g_tagLen) { logf_("Tag= is empty - nothing to do"); return; }
    g_minDist = ini_int("MinDistance", 3);
    g_allowNet = ini_int("AllowMultiplayer", 0);
    g_logShots = g_verbose ? ini_int("LogShots", 10) : 0;
    g_bulletSpread = ini_int("BulletSpread", 3);
    g_bulletsMax = ini_int("BulletsPerShotMax", 32);

    ini_str("LockTag", "lock_on", g_lockTag, sizeof g_lockTag);
    g_lockTagLen = strlen(g_lockTag);
    g_lockTurn = ini_int("LockTurn", 10);
    g_lockLog = g_verbose ? ini_int("LockLog", 10) : 0;
    g_driftMax = ini_int("DriftMax", 45);

    int ok = find_launch() && find_spawn_bullet_parts() && find_aim_point() && find_stuff_tags();
    if (!ok || !install_hook()) { logf_("barrel convergence             OFF (engine code not recognised)"); return; }
    logf_("barrel convergence / multi-shot   ON (stuff tags \"%s\", bullets_per_shot_N, bullet_spread_N; default cone %g deg, min distance %g, multiplayer %s)", g_tag, g_bulletSpread, g_minDist, g_allowNet ? "allowed" : "off");

    int okLock = g_lockTagLen && find_bullet_launch() && find_quant() && find_aim_extras();
    if (!okLock || !install_lock_hooks()) { g_bulletQuant = NULL; logf_("lock-on guidance               OFF (engine code not recognised)"); return; }
    logf_("lock-on guidance / drift       ON (stuff tag \"%s\" on the weapon + \"atgm\" on the ammo; fallback turn %g deg/s; drift_N up to %d deg per 100 units)", g_lockTag, g_lockTurn, g_driftMax);

    ini_str("OverchargeTag", "overcharge", g_ocTag, sizeof g_ocTag);
    ini_str("OverchargeSuffix", "overcharge", g_ocSuffix, sizeof g_ocSuffix);
    g_ocTagLen = strlen(g_ocTag); g_ocSuffixLen = strlen(g_ocSuffix);
    g_ocLog = g_verbose ? ini_int("OverchargeLog", 10) : 0;
    g_ocRounds = ini_int("OverchargeRounds", 2); if (g_ocRounds < 1) g_ocRounds = 1; if (g_ocRounds > 50) g_ocRounds = 50;
    int okOc = g_ocTagLen && g_ocSuffixLen && g_ownerOff && find_stuff_registry() && find_weapon_item_slot() && find_filling_offs() && find_entity_tags_off();
    if (okOc && !find_linked()) { g_entLinkOff = 0; logf_("overcharge: vehicle (gunner -> vehicle) tag lookup OFF"); }
    if (okOc && !find_item_fill()) { g_fillCapOff = 0; logf_("overcharge: magazine capacity rule OFF (fixed %d rounds)", g_ocRounds); }
    if (!okOc || !install_oc_hook()) { logf_("overcharge ammo swap           OFF (engine code not recognised)"); return; }
    logf_("overcharge ammo swap           ON (unit tag \"%s\" -> ammo name tag \"%s\", %d rounds per shot)", g_ocTag, g_ocSuffix, g_ocRounds);

    ini_str("PressedButtons", "overcharge,overcharge_vehicle", g_pressedButtons, sizeof g_pressedButtons);
    g_btnLog = g_verbose ? 3 : 0;
    if (!g_pressedButtons[0]) { logf_("button pressed state           OFF (PressedButtons= empty)"); return; }
    if (!find_cmd_update() || !find_set_look() || !install_button_hook()) { logf_("button pressed state           OFF (engine code not recognised)"); return; }
    logf_("button pressed state           ON (buttons %s drawn pressed while the selected unit carries tag \"%s\")", g_pressedButtons, g_ocTag);
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        InitializeCriticalSection(&g_logLock);
        load_real_dll();
        init(h);
    }
    return TRUE;
}
#endif /* CV_TEST */

#ifdef CV_TEST
/* cv_test.exe <exe> : map the exe's sections at the real base and run every finder (the post-update check) */
int main(int argc, char **argv)
{
    if (argc < 2) { printf("usage: cv_test <call_to_arms*.exe>\n"); return 2; }
    FILE *f = fopen(argv[1], "rb"); if (!f) { printf("cannot open %s\n", argv[1]); return 2; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *file = malloc(n); fread(file, 1, n, f); fclose(f);
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
    int ok = find_launch() && find_spawn_bullet_parts() && find_aim_point() && find_stuff_tags()
          && find_bullet_launch() && find_quant() && find_aim_extras()
          && find_stuff_registry() && find_weapon_item_slot() && find_filling_offs() && find_entity_tags_off() && find_linked() && find_item_fill()
          && find_bombbay();
    if (ok) printf("prologues: SpawnBullet %zu, Launch %zu, Quant %zu, Shooter::Launch %zu bytes\n", prolog_len((uint8_t *)g_spawnBullet), prolog_len((uint8_t *)g_bulletLaunch), prolog_len((uint8_t *)g_bulletQuant), prolog_len(g_launch));
    printf(ok ? "ALL FOUND\n" : "FAILED\n");
    return !ok;
}
#endif
