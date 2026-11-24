/* GEM PenFilter 0.1.1 - the AI does not fire `pierce` ammo at vehicle armour it cannot penetrate
 * Call to Arms: Gates of Hell 1.068 / 1.069 (GEM 2, x64): game, editor and dedicated server exe.
 *
 * Engine (game exe RVAs, all found by byte pattern / call chain at load time, never hardcoded):
 *   MatchAmmo        0x92C9C0  (ctx) -> ammo.set row or NULL. ctx: +0 ammo eStuff, +8 shooter, +0x10 target info
 *                              (+0x18 target entity, +0x28 target position). Row flags byte +0x394, bit 2 = {volume "x"}.
 *     A1 0x92CC4A: `call TagTest` of the volume loop (volume tags vs row+0x330), rbx = &volume slot, r13 = ctx.
 *     A2 0x92CBE3: `test al,2; je MATCH` = a row without {volume} matches at once.
 *   ctx builder      0x92C8E0  (ctx, weapon or NULL, ammo, shooter, ti, flag): always called right before MatchAmmo (8 pairs);
 *                              only the enemy scan 0x970990 passes weapon NULL, its r14 = the item's stuff (weapon stuff).
 *   params           0x81EC40  (weapon) -> {parameters} entry for the loaded ammo = PLIST(weapon+0xA0, ammo+0xEC8);
 *                    0x83F210  range(weapon stuff, ammo) = PLIST(wstuff+0x1600, ammo+0xEC8)+0x20.
 *   pen(dist)        0x841650  projectileDamageTable lookup (params, world distance passed in edx as float bits).
 *   armour chain     (eGameEntityArmor.cpp, the one the sight overlay AimInfo 0x829320 and HitVolume 0x84B890 run):
 *                    0x7FC090 angle (bs, params, cos), 0x7FC310 block (blk, entity, shooter, R, bs), 0x7FC3A0 evaluate (blk):
 *                    bs.flags bit 1 = pierce (pen6 >= threshold 0x7FCDE0, what {on pierce} reacts to), bit 2 = full
 *                    penetration (pen6 >= effective armour 0x7FCA60). Thickness 0x7FC5B0, typed value get / set 0xA8BE80 / 0xA8BE10.
 *   volume picker    0x92B6F0 (aim, noAimCheck, weapon, target, filter Tag, previous aim): collects the unbroken, filter-tagged,
 *                    non-`no_target` volumes into a static vector (begin 0x15B3630, end +8, cap +0x10; ebp = index of the
 *                    previous aim's volume or -1), then up to min(n,5) tries: random index (engine RNG) unless ebp >= 0, set the
 *                    volume (0x852DC0), range check (0x81F710), aim solver 0x833C30; status 3 = keep, else that candidate is erased.
 *     P 0x92B8BA: `mov rdi,[end]`, the try-loop head right after the collection loop (every path into the tries passes it).
 *     B 0x92B9CD: the solver call (outRec = NULL there).
 *     X 0x92BA09: `call 0x853240` (aim clear) on the exhausted exit, also reached from the no-weapon / out-of-range exits.
 *   aim record       +8 mode (0 none, 1 entity, 2 point, 3 entity + point), +0xC point, +0x18 entity, +0x20 volume Tag
 *                    (name std::string at +0x60). Clear 0x853240 = mode 0, entity dropped, its origin left in the point field;
 *                    the callers of the picker treat mode 0 as "no aim" (eAttack 0x903BB6 and 0x95D543 keep their old aim then).
 *                    Volume of an aim record: 0x8527C0.
 *   weapon           +0x90 shooter, +0x98 weapon stuff, +0xA8 firer, +0xB0 ammo slot, aim slots +0xC8 / +0x168.
 *                    Stop 0x81F480(weapon, 1): what the fire function 0x81F0C0 does when handed a mode-0 aim.
 *   solver hit record (0x48): +8 point, +0x18 entity, +0x20 volume, +0x28 normal, +0x34 dir, +0x40 face index.
 *   TraceVolume::eBox face index (eBox trace 0x5FCC60): 0 +X, 1 +Z, 2 +Y, 3 -Y, 4 -X, 5 -Z; the volume's world->local
 *   matrix sits at volume+0x58+0x30 (the trace reads it there).
 *   sim clock: dword global read by 0x66E7B0 (the AI order clock, found through the picker wrapper); back to 0 every battle.
 *   direct control: eActor::IsManualControlled 0x804650 = [actor+0xDE0]+0x18 != 0 or bit 1 of [[actor+0xDD8]+0x70].
 *
 * This dll (0.1.1):
 *   Keys: shooter / target entity ids (+0xC0), ammo name-tag hash mixed with the weapon stuff's name-tag hash, volume index.
 *   A (HookRowFilter): per (shooter, target, ammo + weapon) an ammo verdict: ALLOW if any unbroken non-`no_target` volume of the
 *     target can be pierced from here (optimistic face test with the engine's own thickness / effective-armour / threshold
 *     functions, volumes B blocked count as not pierceable), else REJECT. Held for VerdictHoldMs of sim time; a REJECT is dropped
 *     early when the shooter comes more than HoldDistanceM nearer, an ALLOW when it moves that much farther away, either when
 *     the target moves into / out of penetration range. REJECT = every row of that ammo is skipped (next row / ammo / weapon);
 *     ALLOW = rows without {volume} match, rows with {volume "x"} keep only the volumes that pass.
 *   P (HookAimPriority): before the picker's tries, its candidate list is cut down to the volumes the loaded ammo can pierce
 *     (same test) and the solver could reach in the last UnreachableMs, of the highest-priority non-empty AimPriority group;
 *     the engine's own random pick then lands in that group. No such candidate = list left as it is.
 *   B (HookAimCheck): the picker asks the solver for its hit record; on a hit with filtered ammo the full armour chain runs on
 *     the real point / normal / face; no pierce = status 5 (blocked) and both the volume hit first and the volume aimed at are
 *     cached as blocked until the direction to the shooter in that volume's frame turns by more than 25 degrees, the shooter
 *     comes more than HoldDistanceM nearer, or CacheMs (safety limit) runs out. Solver failures (no solution, elevation,
 *     line of fire) on a list P cut are cached as unreachable (UnreachableMs) so the next pick moves to the next group.
 *   X (HookExhaust): a pick that ran out of candidates with at least one B block: the engine clear runs as always, the ammo
 *     verdict vs that target is worked out again without the blocked volumes and held, and a weapon still aimed at that target
 *     is stopped (ExhaustStop) when the verdict is REJECT or the volume its aim slot holds can no longer be pierced.
 *   Units under direct control are left alone by every hook (SkipDirectControl). Every cache is cleared when the sim clock
 *   goes back (new battle).
 *   C (HookApproach): not built (see README).
 *
 * Build: build.bat      Offline finder / stub / logic self-test: build.bat test
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <limits.h>

#define PF_VERSION "0.1.1"

/* ---------------------------------------------------------------- config */
#define MAXT 16
#define TLEN 64
#define MAXG 8
#define MAXGT 64
static int  cfg_enabled = 1, cfg_log = 1, cfg_logverdicts = 400, cfg_rowfilter = 1, cfg_aimcheck = 1, cfg_approach = 0;
static int  cfg_cachems = 60000, cfg_rule = 1, cfg_player = 1, cfg_facing = 1;
static int  cfg_prio = 1, cfg_exhaust = 1, cfg_exstop = 1, cfg_holdms = 8000, cfg_holddist = 15, cfg_oldstreak = -1;
static int  cfg_unrms = 3000, cfg_dcskip = 1;
#define BLK_COS 0.9063078f      /* cos 25 deg: a blocked volume is tried again once the shooter's direction in its frame turns more */
static char cfg_pierce[TLEN] = "pierce";
static char cfg_exempt[MAXT][TLEN] = { "melee_light", "melee_heavy", "melee" };
static int  n_exempt = 3;
static const char DEF_PRIO[] =
    "body,cabin,bodyback;"
    "turret,mantlet;aux_turret1,aux_turret2,aux_turret3,aux_turret4,aux_turret5,aux_turret6,aux_turret7,aux_turret8,aux_turret9;"
    "arm_left,arm_right,gun_left,gun_right,hkm_left,hkm_right;"
    "leg_left,leg_right,frontleftleg,frontrightleg,midleftleg,midrightleg,backleftleg,backrightleg;"
    "mover,track,trackleft,trackright,wheel";
static char  g_ptag[MAXGT][TLEN];
static int   g_ptag_grp[MAXGT], n_ptag, n_grp;
static float g_holdunits = 300.0f;      /* HoldDistanceM x 20 units / m; <= 0 = no distance gate */

/* ---------------------------------------------------------------- log */
static char  g_logpath[MAX_PATH];
static FILE *g_log;
static volatile LONG g_vlines;
static void logf_(const char *fmt, ...)
{
    if (!g_log) {
        if (!g_logpath[0]) return;
        g_log = fopen(g_logpath, "a");
        if (!g_log) return;
    }
    va_list ap; va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

/* ---------------------------------------------------------------- strings / tags */
static int eq_ci(const char *s, size_t n, const char *t)
{
    size_t m = strlen(t);
    if (m != n) return 0;
    for (size_t i = 0; i < n; i++) {
        char a = s[i], b = t[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return 0;
    }
    return 1;
}
/* MSVC std::string: buf/ptr +0, size +0x10, cap +0x18 */
static const char *str_get(const uint8_t *p, size_t *n)
{
    uint64_t size = *(const uint64_t *)(p + 0x10), cap = *(const uint64_t *)(p + 0x18);
    if (cap < size || cap > 0x10000 || size > 0x1000) return NULL;
    const char *d = cap > 15 ? *(const char *const *)p : (const char *)p;
    if (!d) return NULL;
    *n = (size_t)size;
    return d;
}
/* vector of 0x60-byte Tag (name std::string at +0x40) at v: 1 = one of list, 0 = none, -1 = broken */
static int vec_has(const uint8_t *v, char list[][TLEN], int n)
{
    const uint8_t *b = *(const uint8_t *const *)v, *e = *(const uint8_t *const *)(v + 8);
    if (!b || !e) return 0;
    if (e < b || (uintptr_t)(e - b) % 0x60 || (uintptr_t)(e - b) > 0x60 * 1024) return -1;
    for (const uint8_t *t = b; t < e; t += 0x60) {
        size_t k; const char *s = str_get(t + 0x40, &k);
        if (!s) continue;
        for (int i = 0; i < n; i++) if (eq_ci(s, k, list[i])) return 1;
    }
    return 0;
}
static void vec_str(const uint8_t *v, char *out, size_t cap, int maxn)
{
    out[0] = 0;
    const uint8_t *b = *(const uint8_t *const *)v, *e = *(const uint8_t *const *)(v + 8);
    if (!b || !e || e < b || (uintptr_t)(e - b) % 0x60 || (uintptr_t)(e - b) > 0x60 * 256) return;
    int c = 0;
    for (const uint8_t *t = b; t < e && c < maxn; t += 0x60, c++) {
        size_t k; const char *s = str_get(t + 0x40, &k);
        if (!s) continue;
        size_t l = strlen(out);
        if (l + k + 2 >= cap) break;
        if (l) out[l++] = ' ';
        memcpy(out + l, s, k); out[l + k] = 0;
    }
}
/* stable 32-bit hash of a tag vector's names (same on every machine) */
static uint32_t vec_hash(const uint8_t *v)
{
    uint32_t h = 2166136261u;
    const uint8_t *b = *(const uint8_t *const *)v, *e = *(const uint8_t *const *)(v + 8);
    if (!b || !e || e < b || (uintptr_t)(e - b) % 0x60 || (uintptr_t)(e - b) > 0x60 * 256) return 0;
    for (const uint8_t *t = b; t < e; t += 0x60) {
        size_t k; const char *s = str_get(t + 0x40, &k);
        if (!s) continue;
        for (size_t i = 0; i < k; i++) { char c = s[i]; if (c >= 'A' && c <= 'Z') c += 32; h = (h ^ (uint8_t)c) * 16777619u; }
        h = (h ^ ' ') * 16777619u;
    }
    return h;
}
static uint64_t mix64(uint64_t x)
{
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL; x ^= x >> 33;
    return x;
}

/* AimPriority: groups separated by ';', alternatives by ',' (volume TAGS, case-insensitive) */
static void parse_prio(const char *s)
{
    n_ptag = 0;
    int g = 0, any = 0, tl = 0;
    char tok[TLEN];
    for (const char *c = s; g < MAXG; c++) {
        if (*c == ',' || *c == ';' || *c == 0) {
            if (tl && n_ptag < MAXGT) { tok[tl] = 0; strcpy(g_ptag[n_ptag], tok); g_ptag_grp[n_ptag++] = g; any = 1; }
            tl = 0;
            if ((*c == ';' || *c == 0) && any) { g++; any = 0; }
            if (!*c) break;
        } else if (*c != ' ' && *c != '\t' && tl < TLEN - 1) tok[tl++] = *c;
    }
    n_grp = g;
}
/* priority group of a volume: lowest group holding one of its tags, n_grp = in no group (last) */
static int vol_group(const uint8_t *vol)
{
    const uint8_t *info = *(const uint8_t *const *)(vol + 0x30);
    if (!info || !n_ptag) return n_grp;
    const uint8_t *b = *(const uint8_t *const *)(info + 0x398), *e = *(const uint8_t *const *)(info + 0x3A0);
    if (!b || !e || e < b || (uintptr_t)(e - b) % 0x60 || (uintptr_t)(e - b) > 0x60 * 256) return n_grp;
    int best = n_grp;
    for (const uint8_t *t = b; t < e; t += 0x60) {
        size_t k; const char *s = str_get(t + 0x40, &k);
        if (!s) continue;
        for (int i = 0; i < n_ptag; i++) if (g_ptag_grp[i] < best && eq_ci(s, k, g_ptag[i])) best = g_ptag_grp[i];
    }
    return best;
}

/* ---------------------------------------------------------------- engine sites */
typedef struct {
    uint8_t *base, *lo, *hi;
    /* MatchAmmo */
    uint8_t *a2, *a1, *match, *next, *tagtest, *ma_fn; uint32_t ammotags;
    /* ctx builder + enemy scan */
    uint8_t *builder; int npairs; uint8_t *scan_ret[2]; uint8_t *wcast, *range; uint32_t wlistoff; const char *scan_err;
    /* params */
    uint8_t *P, *loaded, *plist; uint32_t ilistoff;
    /* AimInfo chain */
    uint8_t *aiminfo_call, *rtdc, *c090, *a8be10, *fabs_, *pen;
    uint8_t *pc_src, *pc_dst, *wc_src, *wc_dst; uint32_t wstuffoff, caliboff;
    /* armour */
    uint8_t *c310, *c3a0, *a79530, *armorof, *matof, *relof, *a8be80, *c5b0, *ca60, *cde0;
    /* picker / solver */
    uint8_t *bsite, *solver;
    uint8_t *psite, *xsite, *vbeg, *vend, *clr, *getvol;
    /* weapon stop */
    uint8_t *wstop; uint32_t ws1, ws2;
    uint8_t *clk;
    uint8_t *box_vt, *poly_vt;
    uint8_t *ismanual;
    const char *err_ma, *err_bld, *err_ai, *err_arm, *err_b, *err_clk, *err_box, *err_p, *err_x, *err_gv, *err_stop, *err_man;
} sites_t;

/* ---------------------------------------------------------------- patterns */
typedef struct { uint8_t b[512]; uint8_t m[512]; int n; } pat_t;
static void pat_parse(pat_t *p, const char *s)
{
    p->n = 0;
    while (*s && p->n < 512) {
        while (*s == ' ') s++;
        if (!*s) break;
        if (s[0] == '?') { p->b[p->n] = 0; p->m[p->n] = 0; s += 2; }
        else { unsigned v; sscanf(s, "%2x", &v); p->b[p->n] = (uint8_t)v; p->m[p->n] = 1; s += 2; }
        p->n++;
    }
}
static int pat_at(const uint8_t *a, const pat_t *p)
{
    for (int i = 0; i < p->n; i++) if (p->m[i] && a[i] != p->b[i]) return 0;
    return 1;
}
static uint8_t *pat_find(uint8_t *lo, uint8_t *hi, const pat_t *p, int *count)
{
    uint8_t *hit = NULL; int c = 0;
    for (uint8_t *q = lo; q + p->n <= hi; q++)
        if (q[0] == p->b[0] && pat_at(q, p)) { if (!hit) hit = q; c++; }
    if (count) *count = c;
    return hit;
}
static uint8_t *rel32(uint8_t *at) { return at + 4 + *(int32_t *)at; }   /* at = address of the rel32 field */

/* patterns cut from the 1.068 game exe (rel32 / rip displacements wildcarded), identical in editor and server */
static const char *P_MA =     /* A2 site .. end of the volume loop */
    "A8 02 0F 84 ?? ?? ?? ?? 49 8B 45 10 48 8B 70 18 48 85 F6 74 6D 48 8B 9E 10 05 00 00 48 8B B6 18 05 00 00 "
    "48 3B DE 74 5A 0F 1F 44 00 00 48 8B 0B 48 8B 41 38 48 85 C0 74 40 80 78 24 00 75 3A 48 8B 41 30 "
    "4C 8B B8 A0 03 00 00 4C 8B B0 98 03 00 00 4D 3B F7 74 23 0F 1F 80 00 00 00 00 48 8D 97 30 03 00 00 "
    "49 8B CE E8 ?? ?? ?? ?? 84 C0 75 44 49 83 C6 60 4D 3B F7 75 E4 48 83 C3 08 48 3B DE 75 AB";
#define MA_A1   0x67            /* call TagTest */
#define MA_JNE  0x6E            /* 75 44 -> MATCH */
#define MA_NEXT_REL 0x14        /* rel8 of `je NEXT` */
static const char *P_BLD =
    "48 89 5C 24 08 57 48 83 EC 20 80 7C 24 58 00 48 8B FA 48 8B 44 24 50 48 8B D9 48 89 41 10 4C 89 01 4C 89 49 08";
#define BLD_COPY 15
static const char *P_PAIR = "E8 ?? ?? ?? ?? 48 8B C8 E8 ?? ?? ?? ??";
static const char *P_SCANW =
    "49 8B CE E8 ?? ?? ?? ?? 48 85 C0 74 ?? 80 7E 18 00 74 ?? 48 8B D3 48 8B C8 E8 ?? ?? ?? ??";
static const char *P_WCAST =
    "48 83 EC 38 4C 8D 0D ?? ?? ?? ?? C7 44 24 20 00 00 00 00 4C 8D 05 ?? ?? ?? ?? 33 D2 E8 ?? ?? ?? ?? 48 83 C4 38 C3";
static const char *P_RANGE = "48 8B 8F ?? ?? 00 00 48 8D 93 ?? ?? 00 00 E8 ?? ?? ?? ??";
static const char *P_P =
    "40 53 48 83 EC 20 48 8B D9 48 8B 89 B0 00 00 00 E8 ?? ?? ?? ?? 48 85 C0 74 14 48 8B 8B B0 00 00 00 E8 ?? ?? ?? ?? "
    "48 05 ?? ?? 00 00 EB 07 48 8D 05 ?? ?? ?? ?? 48 8B 8B ?? ?? 00 00 48 8B D0 48 83 C4 20 5B E9 ?? ?? ?? ??";
static const char *P_PEN =
    "40 53 48 83 EC 30 4C 8B 81 90 00 00 00 48 8B D9 66 0F 6E CA 48 8B 91 88 00 00 00 49 3B D0";
static const char *P_C090 =
    "48 89 5C 24 20 57 48 83 EC 50 0F 29 74 24 40 0F 28 C2 0F 29 7C 24 30 48 8B FA 44 0F 29 44 24 20";
static const char *P_FABS = "0F 54 05 ?? ?? ?? ?? C3";
static const char *P_C310 =
    "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 C7 41 20 00 00 80 3F 49 8B F0 4C 89 49 08 4C 8D 41 20 48 8B DA "
    "48 8B F9 48 85 D2 74 16 F6 42 30 02 74 10 F6 42 30 04 75 0A B9 0B 00 00 00 E8 ?? ?? ?? ?? 48 8B CB E8 ?? ?? ?? ?? "
    "48 8B 57 08 48 8B C8 48 89 07 E8 ?? ?? ?? ?? 48 89 47 10 48 8B D3 48 8B 44 24 50 48 8B CE 48 89 47 18 "
    "E8 ?? ?? ?? ?? 48 8B 5C 24 30 48 8B 74 24 38 88 47 24 48 8B C7 48 83 C4 20 5F C3";
static const char *P_C3A0 =
    "48 89 5C 24 20 57 48 83 EC 50 48 8B D9 0F 29 74 24 40 48 8B 49 18 48 83 C1 2C 83 39 02 75 78 BA 06 00 00 00 "
    "E8 ?? ?? ?? ?? 80 7B 24 00 0F 28 F0 F3 0F 11 74 24 68 0F 84 ?? ?? ?? ?? 48 8B CB E8 ?? ?? ?? ?? 0F 2F F0 "
    "4C 8D 44 24 60 48 8D 44 24 68 F3 0F 11 44 24 60 4C 0F 46 C0 72 14 48 8B 43 10 F6 80 B0 00 00 00 01 75 07 "
    "48 8B 43 18 80 08 01 48 8B 4B 18 BA 06 00 00 00 F3 41 0F 10 10 48 83 C1 2C 48 8B 5C 24 78 0F 28 74 24 40 "
    "48 83 C4 50 5F E9 ?? ?? ?? ?? 33 D2 0F 29 7C 24 30 48 8B CB E8 ?? ?? ?? ?? 0F 28 C8 48 8B CB E8 ?? ?? ?? ?? "
    "48 8B 4B 18 0F 28 F8 48 83 C1 2C F3 0F 11 7C 24 60 BA 06 00 00 00 E8 ?? ?? ?? ?? 80 7B 24 00 0F 28 F0 "
    "F3 0F 11 74 24 68 0F 84 ?? ?? ?? ?? 0F 2F F7 48 89 74 24 70 48 8D 73 18 44 0F 29 44 24 20 72 0B 48 8B 43 18 "
    "48 8D 73 18 80 08 02 0F 2F F7 48 8D 44 24 60 48 8D 4C 24 68 0F 57 C0 48 0F 46 C1 F3 44 0F 10 00 48 8B 43 10 "
    "F3 0F 10 48 10 0F 2E C8 7A 02 74 0E 48 8B 43 18 F3 0F 10 40 04 0F 2F C1 72 32 48 8B CB E8 ?? ?? ?? ?? "
    "0F 2F F0 72 25 48 8B 43 10 F6 80 B0 00 00 00 01 75 18 48 8B 43 18 41 0F 28 C0 F3 0F 5E C7 80 08 01";
static const char *P_BSITE =
    "49 8B 8D A8 00 00 00 33 C0 48 89 44 24 28 45 33 C9 45 33 C0 48 89 44 24 20 49 8B D6 E8 ?? ?? ?? ?? 83 F8 03";
#define BSITE_CALL 0x1C
static const char *P_SOLV =
    "48 83 EC 58 80 3D ?? ?? ?? ?? 00 4C 8B 51 10 4C 89 4C 24 20 4C 89 54 24 28 4C 89 44 24 30 49 8B 82 A8 00 00 00 "
    "75 06 48 8B 48 20 EB 04 48 8B 48 28 49 8B 82 90 00 00 00";
/* picker try-loop head: mov rdi,[end]; mov eax,5; sub rdi,[begin]; sar rdi,3; cmp edi,eax; cmovg; sub edi,1; js EXH; test ebp,ebp; jns */
static const char *P_PICKP =
    "48 8B 3D ?? ?? ?? ?? B8 05 00 00 00 48 2B 3D ?? ?? ?? ?? 48 C1 FF 03 3B F8 0F 4F F8 83 EF 01 0F 88 ?? ?? ?? ?? 85 ED 79";
#define PICKP_LEN 7             /* the `mov rdi,[end]` the jmp replaces */
#define PICKP_JS  0x1F
/* candidate push: mov rax,[end]; cmp rax,[cap]; mov [rsp+30h],r15; je grow; mov [rax],r15; add qword [end],8 */
static const char *P_PUSH =
    "48 8B 05 ?? ?? ?? ?? 48 3B 05 ?? ?? ?? ?? 4C 89 7C 24 30 74 0D 4C 89 38 48 83 05 ?? ?? ?? ?? 08";
/* previous-aim test: mov rcx,[rsp+0A8h]; test rcx,rcx; je; mov rsi,[rbx]; call AimVolume; cmp rax,rsi */
static const char *P_GETV = "48 8B 8C 24 A8 00 00 00 48 85 C9 74 ?? 48 8B 33 E8 ?? ?? ?? ?? 48 3B C6";
static const char *P_GETVB = "48 8B 41 18 48 85 C0 74 ?? 48 83 79 70 00 74 ?? 48 8D 51 20 48 8D 88 00 05 00 00 E9";
static const char *P_CLR = "48 89 5C 24 08 57 48 83 EC 20 33 FF 48 8D 41 60 48 89 79 70 48 8B D9";
/* weapon fire with a mode-0 aim: cmp dword [rXX+8],0; jne; mov edx,1; mov rcx,rbx; call Stop */
static const char *P_FIRE0 = "83 ?? 08 00 75 12 BA 01 00 00 00 48 8B CB E8 ?? ?? ?? ??";
/* Stop: lea rdi,[rbx+slot]; cmp dword [rdi+8],0; je; mov rcx,rdi; call Clear */
static const char *P_WSLOT = "48 8D BB ?? ?? ?? ?? 83 7F 08 00 74 08 48 8B CF E8 ?? ?? ?? ??";
static const char *P_CLK = "41 8B 59 64 E8 ?? ?? ?? ?? 2B C3 8B C8 E8 ?? ?? ?? ??";
/* eActor::IsManualControlled: push rbx; sub rsp,20h; mov rbx,rcx; mov rcx,[rcx+0DE0h]; call PropOn; test al,al; jne;
 * mov rax,[rbx+0DD8h]; mov ecx,[rax+70h]; shr ecx,1; test cl,1 (the same test MeleeStance's SkipDirectControl uses) */
static const char *P_MANUAL =
    "40 53 48 83 EC 20 48 8B D9 48 8B 89 E0 0D 00 00 E8 ?? ?? ?? ?? 84 C0 75 ?? 48 8B 83 D8 0D 00 00 8B 48 70 D1 E9 F6 C1 01";
static const char *P_PROPON = "83 79 18 00 0F 95 C0 C3";    /* cmp dword [rcx+18h],0; setne al; ret */
static const char *P_PLAYER = "48 8B 88 C8 0D 00 00 F6 81 98 03 00 00 03";

/* ---------------------------------------------------------------- image helpers */
static int sec_range(uint8_t *base, const char *name, uint8_t **lo, uint8_t **hi)
{
    IMAGE_DOS_HEADER *d = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(base + d->e_lfanew);
    IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, s++)
        if (!strncmp((const char *)s->Name, name, 8)) {
            *lo = base + s->VirtualAddress;
            *hi = *lo + s->Misc.VirtualSize;
            return 1;
        }
    return 0;
}
static RUNTIME_FUNCTION *g_pdata; static DWORD g_npdata;
static void pdata_init(uint8_t *base)
{
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY *dd = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    g_pdata = (RUNTIME_FUNCTION *)(base + dd->VirtualAddress);
    g_npdata = dd->Size / sizeof(RUNTIME_FUNCTION);
}
static int is_func_start(uint8_t *base, uint8_t *a)
{
    DWORD rva = (DWORD)(a - base), lo = 0, hi = g_npdata;
    while (lo < hi) {
        DWORD m = (lo + hi) / 2;
        if (g_pdata[m].BeginAddress < rva) lo = m + 1; else hi = m;
    }
    return lo < g_npdata && g_pdata[lo].BeginAddress == rva;
}
/* rip-relative lea r8 / r9 in [from, to): last occurrence of each */
static void find_leas(uint8_t *from, uint8_t *to, uint8_t **r8, uint8_t **r9)
{
    *r8 = *r9 = NULL;
    for (uint8_t *q = from; q + 7 <= to; q++) {
        if (q[0] == 0x4C && q[1] == 0x8D && q[2] == 0x05) *r8 = rel32(q + 3);
        if (q[0] == 0x4C && q[1] == 0x8D && q[2] == 0x0D) *r9 = rel32(q + 3);
    }
}
/* vtable of an MSVC RTTI class by its decorated name */
static uint8_t *rtti_vtable(uint8_t *base, const char *name)
{
    uint8_t *dlo, *dhi, *rlo, *rhi;
    if (!sec_range(base, ".data", &dlo, &dhi) || !sec_range(base, ".rdata", &rlo, &rhi)) return NULL;
    size_t n = strlen(name) + 1;
    for (uint8_t *q = dlo; q + n <= dhi; q++) {
        if (memcmp(q, name, n)) continue;
        DWORD td = (DWORD)(q - 0x10 - base);
        for (uint8_t *c = rlo; c + 24 <= rhi; c += 4) {
            DWORD *col = (DWORD *)c;
            if (col[0] != 1 || col[3] != td || col[5] != (DWORD)(c - base) || col[1] != 0) continue;
            IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
            uint64_t va = (uint64_t)(uintptr_t)c, va2 = nt->OptionalHeader.ImageBase + (uint64_t)(c - base);
            for (uint8_t *v = rlo; v + 16 <= rhi; v += 8)
                if (*(uint64_t *)v == va || *(uint64_t *)v == va2) return v + 8;
        }
    }
    return NULL;
}

/* ---------------------------------------------------------------- finders */
static void find_all(uint8_t *base, sites_t *s)
{
    memset(s, 0, sizeof *s);
    s->base = base;
    pdata_init(base);
    if (!sec_range(base, ".text", &s->lo, &s->hi)) { s->err_ma = s->err_b = s->err_p = s->err_x = "no .text section"; return; }
    uint8_t *lo = s->lo, *hi = s->hi;
    pat_t p; int c;
#define IN_TEXT(x) ((x) >= lo && (x) < hi)

    /* MatchAmmo sites */
    pat_parse(&p, P_MA);
    uint8_t *ma = pat_find(lo, hi, &p, &c);
    if (c != 1) s->err_ma = c ? "MatchAmmo volume loop not unique" : "MatchAmmo volume loop not found (unknown build or already patched)";
    else {
        s->a2 = ma; s->a1 = ma + MA_A1;
        s->match = rel32(ma + 4);
        s->next = ma + MA_NEXT_REL + 1 + (int8_t)ma[MA_NEXT_REL];
        s->tagtest = rel32(s->a1 + 1);
        uint8_t *jne = ma + MA_JNE + 2 + (int8_t)ma[MA_JNE + 1];
        if (jne != s->match) s->err_ma = "MatchAmmo: volume match target differs from the no-volume match target";
        else if (!(s->match[0] == 0x48 && s->match[1] == 0x8B && s->match[2] == 0xC7)) s->err_ma = "MatchAmmo: match target is not `mov rax,rdi`";
        else if (!(s->next[0] == 0x48 && s->next[1] == 0x81 && s->next[2] == 0xC7 && *(uint32_t *)(s->next + 3) == 0x398)) s->err_ma = "MatchAmmo: next-row target is not `add rdi,398h`";
        else if (!IN_TEXT(s->tagtest)) s->err_ma = "MatchAmmo: tag test call outside .text";
        else {
            /* ammo {tag} offset: add rcx,imm32; call TagSet (before the sites) + the player test the code relies on */
            for (uint8_t *q = ma - 0x200; q < ma; q++)
                if (q[0] == 0x48 && q[1] == 0x81 && q[2] == 0xC1 && q[5] == 0 && q[6] == 0 && q[7] == 0xE8) s->ammotags = *(uint32_t *)(q + 3);
            pat_t pp; pat_parse(&pp, P_PLAYER);
            if (!s->ammotags) s->err_ma = "MatchAmmo: ammo tag offset not found";
            else if (!pat_find(ma - 0x200, ma, &pp, NULL)) s->err_ma = "MatchAmmo: player-controlled test (brain+0xDC8, +0x398) not found";
            else {
                int same = 0;    /* the class loop calls the same tag test */
                for (uint8_t *q = ma - 0x200; q < ma; q++) if (q[0] == 0xE8 && rel32(q + 1) == s->tagtest) same++;
                if (!same) s->err_ma = "MatchAmmo: volume tag test call target differs from the class tag test (already redirected?)";
            }
        }
    }

    /* ctx builder: `call Builder; mov rcx,rax; call MatchAmmo` pairs, MatchAmmo = the function holding the sites */
    if (!s->err_ma) {
        pat_parse(&p, P_PAIR);
        uint8_t *calls[16]; int n = 0; uint8_t *fn = NULL, *bld = NULL; int bad = 0;
        for (uint8_t *q = lo; q + p.n <= hi; q++) {
            if (q[0] != 0xE8 || q[5] != 0x48 || !pat_at(q, &p)) continue;
            uint8_t *t2 = rel32(q + 9);
            if (t2 > s->a2 || t2 + 0x400 < s->a2) continue;
            if (!is_func_start(base, t2)) continue;
            if (fn && t2 != fn) { bad = 1; continue; }
            fn = t2;
            uint8_t *t1 = rel32(q + 1);
            if (bld && t1 != bld) bad = 1;
            bld = t1;
            if (n < 16) calls[n++] = q;
        }
        pat_t pb; pat_parse(&pb, P_BLD);
        if (!n) s->err_bld = "no `call builder; mov rcx,rax; call MatchAmmo` pair found";
        else if (bad) s->err_bld = "MatchAmmo pairs disagree on the builder / MatchAmmo address";
        else if (!pat_at(bld, &pb)) s->err_bld = "ctx builder prologue differs (unknown build or already hooked)";
        else {
            s->ma_fn = fn; s->builder = bld; s->npairs = n;
            /* enemy scan: the only two pairs with `xor edx,edx` (no weapon) right before the builder call */
            uint8_t *sc[4]; int ns = 0;
            for (int i = 0; i < n; i++)
                for (uint8_t *q = calls[i] - 0x18; q < calls[i]; q++)
                    if (q[0] == 0x33 && q[1] == 0xD2) { if (ns < 4) sc[ns++] = calls[i]; break; }
            if (ns != 2) s->scan_err = "enemy scan: not exactly two weapon-less builder calls";
            else {
                if (sc[0] > sc[1]) { uint8_t *t = sc[0]; sc[0] = sc[1]; sc[1] = t; }
                int r14a = 0, r14b = 0;
                for (uint8_t *q = sc[0] - 0x400; q < sc[0]; q++) if (q[0] == 0x4C && q[1] == 0x8B && q[2] == 0x72 && q[3] == 0x40) r14a = 1;
                for (uint8_t *q = sc[1] - 0x20; q < sc[1]; q++) if (q[0] == 0x4D && q[1] == 0x8B && q[2] == 0xC6) r14b = 1;
                pat_t pw; pat_parse(&pw, P_SCANW);
                uint8_t *w = pat_find(sc[0] + 13, sc[0] + 0x80, &pw, NULL);
                if (sc[1] - sc[0] > 0x300) s->scan_err = "enemy scan: the two calls are too far apart";
                else if (!r14a || !r14b) s->scan_err = "enemy scan: r14 is not the item's stuff ([item+40h]) there";
                else if (!w) s->scan_err = "enemy scan: weapon-stuff cast / range calls after MatchAmmo not found";
                else {
                    s->wcast = rel32(w + 4); s->range = rel32(w + 0x1A);
                    pat_t pc; pat_parse(&pc, P_WCAST);
                    pat_t pr; pat_parse(&pr, P_RANGE);
                    uint8_t *rq = IN_TEXT(s->range) ? pat_find(s->range, s->range + 0x80, &pr, NULL) : NULL;
                    if (!IN_TEXT(s->wcast) || !pat_at(s->wcast, &pc)) s->scan_err = "enemy scan: weapon-stuff cast differs";
                    else if (!rq) s->scan_err = "enemy scan: weapon-stuff {parameters} read in range() not found";
                    else if (*(uint32_t *)(rq + 10) != s->ammotags) s->scan_err = "range(): ammo tag offset differs from MatchAmmo's";
                    else {
                        s->wlistoff = *(uint32_t *)(rq + 3);
                        s->plist = rel32(rq + 15);          /* checked against params() below */
                        s->scan_ret[0] = sc[0] + 5; s->scan_ret[1] = sc[1] + 5;
                    }
                }
            }
        }
    } else s->err_bld = "needs MatchAmmo";

    /* armour chain */
    pat_parse(&p, P_C310);
    s->c310 = pat_find(lo, hi, &p, &c);
    if (c != 1) s->err_arm = c ? "armour block builder not unique" : "armour block builder (eGameEntityArmor) not found";
    else {
        s->a79530 = rel32(s->c310 + 0x3E); s->armorof = rel32(s->c310 + 0x46);
        s->matof = rel32(s->c310 + 0x55); s->relof = rel32(s->c310 + 0x6D);
        pat_parse(&p, P_C3A0);
        s->c3a0 = pat_find(lo, hi, &p, &c);
        if (c != 1) s->err_arm = c ? "armour evaluator not unique" : "armour evaluator not found";
        else {
            s->a8be80 = rel32(s->c3a0 + 0x25); s->cde0 = rel32(s->c3a0 + 0x40); s->a8be10 = rel32(s->c3a0 + 0x93);
            s->c5b0 = rel32(s->c3a0 + 0xA2); s->ca60 = rel32(s->c3a0 + 0xAD);
            if (rel32(s->c3a0 + 0x139) != s->cde0 || rel32(s->c3a0 + 0xC8) != s->a8be80)
                s->err_arm = "armour evaluator: repeated calls disagree";
        }
    }

    /* AimInfo (sight overlay): the c3a0 caller whose calls before it run
     * ... P, cast, Loaded, TagTest, Pen, c310 ... cast, fabs, ValSet, Angle, c310, c3a0 */
    if (!s->err_arm) {
        pat_t pfab, ppen, pP, p090;
        pat_parse(&pfab, P_FABS); pat_parse(&ppen, P_PEN); pat_parse(&pP, P_P); pat_parse(&p090, P_C090);
        int found = 0;
        for (uint8_t *q = lo; q + 5 <= hi && !found; q++) {
            if (q[0] != 0xE8 || rel32(q + 1) != s->c3a0) continue;
            uint8_t *cs[256], *ct[256]; int n = 0;
            for (uint8_t *r = q - 0x700; r < q && n < 256; r++) {
                if (r[0] != 0xE8) continue;
                uint8_t *t = rel32(r + 1);
                if (!IN_TEXT(t) || !(is_func_start(base, t) || t[-1] == 0xCC || t[-1] == 0x90 || t[-1] == 0xC3)) continue;   /* leaf functions have no .pdata */
                cs[n] = r; ct[n] = t; n++;
            }
            if (n < 10) continue;
            if (ct[n - 1] != s->c310 || ct[n - 3] != s->a8be10 || !pat_at(ct[n - 4], &pfab) || !pat_at(ct[n - 2], &p090)) continue;
            uint8_t *rt = ct[n - 5];
            int j = -1;
            for (int i = n - 6; i >= 4; i--)
                if (pat_at(ct[i], &ppen) && ct[i + 1] == s->c310 && ct[i - 1] == s->tagtest && ct[i - 3] == rt && pat_at(ct[i - 4], &pP)) { j = i; break; }
            if (j < 0) continue;
            found = 1;
            s->aiminfo_call = q; s->c090 = ct[n - 2]; s->rtdc = rt; s->fabs_ = ct[n - 4]; s->pen = ct[j];
            s->loaded = ct[j - 2]; s->P = ct[j - 4];
            find_leas(cs[j - 3] - 0x28, cs[j - 3], &s->pc_src, &s->pc_dst);
            find_leas(cs[n - 5] - 0x28, cs[n - 5], &s->wc_src, &s->wc_dst);
            for (uint8_t *mv = cs[n - 5] - 0x28; mv + 7 <= cs[n - 5]; mv++)   /* mov rcx,[rXX+disp32] feeding the weapon-stuff cast */
                if ((mv[0] == 0x48 || mv[0] == 0x49) && mv[1] == 0x8B && (mv[2] & 0xF8) == 0x88 && (mv[2] & 7) != 4) s->wstuffoff = *(uint32_t *)(mv + 3);
            for (uint8_t *r = cs[n - 5]; r < cs[n - 2]; r++)
                if (r[0] == 0xF3 && r[1] == 0x0F && r[2] == 0x10 && (r[3] & 0xC0) == 0x80) { s->caliboff = *(uint32_t *)(r + 4); break; }
        }
        if (!found) s->err_ai = "sight overlay armour chain (AimInfo call sequence) not found";
        else {
            uint8_t *pl = rel32(s->P + 0x45);
            if (rel32(s->P + 0x11) != s->loaded || rel32(s->P + 0x22) != s->loaded) s->err_ai = "params(): loaded-ammo getter differs from AimInfo's";
            else if (*(uint32_t *)(s->P + 0x28) != s->ammotags && s->ammotags) s->err_ai = "params(): ammo tag offset differs from MatchAmmo's";
            else if (!s->pc_src || !s->pc_dst || !s->wc_src || !s->wc_dst) s->err_ai = "AimInfo: dynamic_cast type descriptors not found";
            else if (s->wstuffoff != 0x98 || !s->caliboff) s->err_ai = "AimInfo: weapon stuff / calibre offsets not found";
            else {
                s->ilistoff = *(uint32_t *)(s->P + 0x38);
                if (s->plist && s->plist != pl) s->scan_err = "range() and params() use different {parameters} lookups";
                s->plist = pl;
                if (s->wcast && rel32(s->wcast + 0x1D) != s->rtdc) s->scan_err = "weapon-stuff cast uses another dynamic_cast";
            }
        }
    } else s->err_ai = "needs the armour chain";

    /* picker -> solver site */
    pat_parse(&p, P_BSITE);
    uint8_t *bs = pat_find(lo, hi, &p, &c);
    if (c != 1) s->err_b = c ? "picker solver call not unique" : "picker solver call not found (unknown build or already patched)";
    else {
        s->bsite = bs + BSITE_CALL; s->solver = rel32(s->bsite + 1);
        pat_parse(&p, P_SOLV);
        if (!IN_TEXT(s->solver) || !pat_at(s->solver, &p)) s->err_b = "aim solver prologue differs (weapon+0x10 / +0xA8 / +0x90 layout)";
    }

    /* picker: P (try-loop head), X (exhausted clear), the candidate vector, the aim-volume getter */
    if (!s->err_b) {
        pat_parse(&p, P_PICKP);
        uint8_t *ps = pat_find(s->bsite - 0x200, s->bsite, &p, &c);
        if (c != 1) s->err_p = s->err_x = c ? "picker try-loop head not unique" : "picker try-loop head (`mov rdi,[end]; mov eax,5`) not found (unknown build or already patched)";
        else {
            uint8_t *vend = rel32(ps + 3), *vbeg = rel32(ps + 15);
            uint8_t *T = ps + PICKP_JS + 6 + *(int32_t *)(ps + PICKP_JS + 2);
            pat_t pu; pat_parse(&pu, P_PUSH);
            uint8_t *pu_at = pat_find(ps - 0x100, ps, &pu, NULL);
            if (vend != vbeg + 8) s->err_p = "picker: candidate vector end != begin + 8";
            else if (!pu_at || rel32(pu_at + 3) != vend || rel32(pu_at + 10) != vbeg + 16 || rel32(pu_at + 0x1B) + 1 != vend)
                s->err_p = "picker: the candidate push does not use the same vector";
            else { s->psite = ps; s->vbeg = vbeg; s->vend = vend; }
            /* X: the js out of the head lands on `mov rcx,r14; call Clear`, right after the try loop's `sub edi,1; jns` */
            if (!(T > s->bsite && T < s->bsite + 0x100)) s->err_x = "picker: exhausted exit not after the solver call";
            else if (!(T[0] == 0x49 && T[1] == 0x8B && T[2] == 0xCE && T[3] == 0xE8)) s->err_x = "picker: exhausted exit is not `mov rcx,r14; call` (already patched?)";
            else if (!(T[-9] == 0x83 && T[-8] == 0xEF && T[-7] == 0x01 && T[-6] == 0x0F && T[-5] == 0x89)) s->err_x = "picker: exhausted exit does not follow the try loop";
            else {
                uint8_t *clr = rel32(T + 4), *loop = T + *(int32_t *)(T - 4);
                pat_t pc; pat_parse(&pc, P_CLR);
                if (!IN_TEXT(clr) || !pat_at(clr, &pc)) s->err_x = "aim clear prologue differs";
                else if (!(loop > ps && loop < s->bsite)) s->err_x = "picker: the try loop does not start after the head";
                else { s->xsite = T + 3; s->clr = clr; }
            }
            pat_t pg, pgb; pat_parse(&pg, P_GETV); pat_parse(&pgb, P_GETVB);
            uint8_t *g = pat_find(ps - 0x80, ps, &pg, NULL);
            if (!g || !IN_TEXT(rel32(g + 0x11)) || !pat_at(rel32(g + 0x11), &pgb)) s->err_gv = "aim-volume getter not found";
            else s->getvol = rel32(g + 0x11);
        }
    } else s->err_p = s->err_x = s->err_gv = "needs the picker solver call";

    /* weapon stop: the call the fire function makes for a mode-0 aim; it clears the two aim slots with the same Clear */
    pat_parse(&p, P_FIRE0);
    uint8_t *f0 = pat_find(lo, hi, &p, &c);
    if (c != 1) s->err_stop = c ? "weapon fire: mode-0 stop call not unique" : "weapon fire: mode-0 stop call not found";
    else if (!s->clr) s->err_stop = "needs the aim clear";
    else {
        uint8_t *st = rel32(f0 + 15); int n = 0, chk = 0; uint32_t off[2] = { 0, 0 };
        if (IN_TEXT(st)) {
            for (uint8_t *q = st; q < st + 0x30; q++)
                if (q[0] == 0x48 && q[1] == 0x83 && q[2] == 0xB9 && *(uint32_t *)(q + 3) == 0x98 && q[7] == 0) chk = 1;
            pat_t pw; pat_parse(&pw, P_WSLOT);
            for (uint8_t *q = st; q + pw.n <= st + 0x120; q++)
                if (pat_at(q, &pw) && rel32(q + 17) == s->clr) { if (n < 2) off[n] = *(uint32_t *)(q + 3); n++; }
        }
        if (!chk) s->err_stop = "weapon stop: no weapon-stuff (+0x98) test at its start";
        else if (n != 2 || off[0] == off[1]) s->err_stop = "weapon stop: not exactly two aim slots cleared";
        else { s->wstop = st; s->ws1 = off[0]; s->ws2 = off[1]; }
    }

    /* sim clock: picker wrapper `mov ebx,[r9+64h]; call Clock; sub eax,ebx; mov ecx,eax; call MsToSec` */
    pat_parse(&p, P_CLK);
    uint8_t *ck = pat_find(lo, hi, &p, &c);
    if (c != 1) s->err_clk = "sim clock read not found";
    else {
        uint8_t *f = rel32(ck + 5);
        if (!IN_TEXT(f) || f[0] != 0x8B || f[1] != 0x05 || f[6] != 0xC3) s->err_clk = "sim clock getter differs";
        else s->clk = rel32(f + 2);
    }

    /* direct control test (offsets are fixed bytes of the pattern; the property test must be `cmp dword [rcx+18h],0; setne al; ret`) */
    pat_parse(&p, P_MANUAL);
    uint8_t *mn = pat_find(lo, hi, &p, &c);
    if (c != 1) s->err_man = c ? "IsManualControlled not unique" : "IsManualControlled not found";
    else {
        pat_t pq; pat_parse(&pq, P_PROPON);
        uint8_t *pt = rel32(mn + 17);
        if (!IN_TEXT(pt) || !pat_at(pt, &pq)) s->err_man = "IsManualControlled: property test differs";
        else if (!is_func_start(base, mn)) s->err_man = "IsManualControlled: not a function start";
        else s->ismanual = mn;
    }

    /* eBox trace volume vtable (face directions) */
    s->box_vt = rtti_vtable(base, ".?AVeBox@TraceVolume@@");
    s->poly_vt = rtti_vtable(base, ".?AVePolyhedron@TraceVolume@@");
    if (!s->box_vt || !s->poly_vt) s->err_box = "TraceVolume::eBox / ePolyhedron vtable not found";
#undef IN_TEXT
}

/* ---------------------------------------------------------------- engine calls */
typedef void *(*fp_p)(void *);
typedef void *(*fp_pp)(void *, void *);
typedef float (*fp_pen)(void *, uint32_t);
typedef void *(*fp_rtdc)(void *, long, void *, void *, int);
typedef void (*fp_angle)(void *, void *, float);
typedef void *(*fp_blk)(void *, void *, void *, void *, void *);
typedef void (*fp_eval)(void *);
typedef float (*fp_thick)(void *, int);
typedef float (*fp_eff)(void *, float);
typedef float (*fp_thr)(void *);
typedef float (*fp_dvget)(void *, int);
typedef void (*fp_mult)(int, void *, float *);
typedef uint8_t (*fp_rel)(void *, void *);
typedef int (*fp_solve)(void *, void *, void *, void *, void *, void *);
typedef void (*fp_v)(void *);
typedef void (*fp_wstop)(void *, int);

static sites_t S;
static volatile uint32_t *g_clock;
static uint32_t now_ms(void) { return g_clock ? *g_clock : 0; }
static fp_p      e_loaded, e_P, e_wcast, e_armorof, e_getvol;
static fp_pp     e_plist, e_matof;
static fp_pen    e_pen;
static fp_rtdc   e_rtdc;
static fp_angle  e_angle;
static fp_blk    e_block;
static fp_eval   e_eval;
static fp_thick  e_thick;
static fp_eff    e_eff;
static fp_thr    e_thr;
static fp_dvget  e_dvget;
static fp_mult   e_mult;
static fp_rel    e_rel;
static fp_solve  e_solve;
static fp_v      e_clear;
static fp_wstop  e_wstop;
__attribute__((used)) void *g_tagtest;

static int hookA_on, hookB_on, hookP_on, hookX_on, prio_on, facing_on, solve_on;
static SRWLOCK g_lock = SRWLOCK_INIT;

/* engine structs (layouts read from AimInfo / HitVolume / eGameEntityArmor) */
typedef struct {                /* "bullet state" the armour chain reads / writes, 0x34 bytes */
    uint32_t flags;             /* +00 bit1 pierce, bit2 full penetration */
    float calibre;              /* +04 weapon stuff +0x1628 */
    float angle;                /* +08 degrees (set by Angle) */
    float f0c, f10;             /* +0C */
    float f14[4];               /* +14 .. +20 */
    float slope;                /* +24 */
    float frac;                 /* +28 */
    int32_t dv_type;            /* +2C typed damage value: unit */
    float dv_val;               /* +30 */
    uint8_t pad[12];
} bstate_t;
typedef struct {                /* armour block 0x28 */
    void *armour;               /* +00 */
    void *rec;                  /* +08 -> R */
    uint8_t *mat;               /* +10 */
    bstate_t *bs;               /* +18 */
    float mult;                 /* +20 */
    uint8_t enemy;              /* +24 */
    uint8_t pad[11];
} blk_t;
typedef struct {                /* hit record the chain reads, 0x34 */
    float pt[3];                /* +00 */
    float nrm[3];               /* +0C */
    float dir[3];               /* +18 */
    int32_t face;               /* +24 */
    void *vol;                  /* +28 */
    uint8_t flag;               /* +30 */
    uint8_t pad[15];
} rec_t;

static int is_vehicle(const uint8_t *t) { return t && (t[0x30] & 0x02) && !(t[0x30] & 0x04); }
static int player_unit(const uint8_t *e)
{
    const uint8_t *br = *(const uint8_t *const *)(e + 0xDC8);
    return br && (*(const uint32_t *)(br + 0x398) & 3) == 1;
}
/* under direct control (engine IsManualControlled, its two halves read here with null checks); 0 when the test was not found
 * or SkipDirectControl=0. Inferred: the manual-control property is sim state on every machine (MeleeStance uses the same test). */
static int dc_on;
static int is_manual(const uint8_t *e)
{
    if (!dc_on || !e || !(e[0x30] & 0x02)) return 0;
    const uint8_t *pr = *(const uint8_t *const *)(e + 0xDE0);
    if (pr && *(const int32_t *)(pr + 0x18) != 0) return 1;
    const uint8_t *d = *(const uint8_t *const *)(e + 0xDD8);
    return d && ((*(const uint32_t *)(d + 0x70) >> 1) & 1);
}
static uint32_t ent_id(const uint8_t *e) { return *(const uint32_t *)(e + 0xC0); }
static float edist(const uint8_t *a, const uint8_t *b)
{
    const float *p = (const float *)(a + 0x74), *q = (const float *)(b + 0x74);
    float dx = p[0] - q[0], dy = p[1] - q[1], dz = p[2] - q[2];
    return sqrtf(dx * dx + dy * dy + dz * dz);
}
static void ent_desc(const uint8_t *e, char *out, size_t cap)
{
    char props[96] = "";
    const uint8_t *def = *(const uint8_t *const *)(e + 0x548);
    if (def) vec_str(def + 0x330, props, sizeof props, 3);
    snprintf(out, cap, "0x%X(%s)", ent_id(e), props);
}
static uint32_t vol_index(const uint8_t *target, const void *vol)
{
    uint8_t **b = *(uint8_t ***)(target + 0x510), **e = *(uint8_t ***)(target + 0x518);
    if (!vol || !b || e < b || e - b > 4096) return UINT32_MAX;
    for (uint8_t **v = b; v < e; v++) if (*v == vol) return (uint32_t)(v - b);
    return UINT32_MAX;
}
static void vol_desc(const uint8_t *vol, uint32_t idx, char *out, size_t cap)
{
    char t[96] = "";
    const uint8_t *info = vol ? *(const uint8_t *const *)(vol + 0x30) : NULL;
    if (info) vec_str(info + 0x398, t, sizeof t, 3);
    if (!vol) snprintf(out, cap, "-");
    else if (idx == UINT32_MAX) snprintf(out, cap, "vol#?(%s)", t);
    else snprintf(out, cap, "vol#%u(%s)", idx, t);
}

/* ---------------------------------------------------------------- stats (log only) */
typedef struct {
    uint32_t a_allow, a_reject, a_held, a_vpass, a_vblock, a_rpass, a_rblock, a_noweap;
    uint32_t p_picks, p_removed, p_curdrop, p_none, p_unr;
    uint32_t b_pierce, b_block, b_aimed, s_unr;
    volatile LONG x_clear, x_exh, x_stop; uint32_t x_bc, x_kept;
    uint32_t dc_skip, flushes;
    uint32_t log_rep, log_supp;
} stats_t;
static stats_t ST;

/* ---------------------------------------------------------------- change-only verdict log (log only, never read by the sim) */
#define V_NONE INT32_MIN
typedef struct { uint32_t sid, tid, akey, reps; int32_t verdict; int hook, set; } vl_t;
static vl_t g_vl[4096];
/* 1 = print a line: the verdict of (hook, shooter, target, ammo) changed (budget LogVerdicts); 0 = repeat (counted) */
static int vl_change(int hook, uint32_t sid, uint32_t tid, uint32_t akey, int32_t verdict, int32_t *pv, uint32_t *preps)
{
    vl_t *e = &g_vl[mix64(((uint64_t)sid << 32 | tid) ^ ((uint64_t)akey << 11) ^ ((uint64_t)(hook + 1) * 0x9E3779B97F4A7C15ULL)) & 4095];
    *pv = V_NONE; *preps = 0;
    if (e->set && e->hook == hook && e->sid == sid && e->tid == tid && e->akey == akey) {
        if (e->verdict == verdict) { e->reps++; ST.log_rep++; return 0; }
        *pv = e->verdict; *preps = e->reps;
    }
    e->set = 1; e->hook = hook; e->sid = sid; e->tid = tid; e->akey = akey; e->verdict = verdict; e->reps = 1;
    if (!cfg_log || g_vlines >= cfg_logverdicts) { ST.log_supp++; return 0; }
    g_vlines++;
    return 1;
}
static void was_str(char *out, size_t cap, int32_t pv, uint32_t preps, const char *(*name)(int32_t))
{
    if (pv == V_NONE) out[0] = 0;
    else snprintf(out, cap, " (was %s, seen %u times)", name(pv), preps);
}
static const char *nm_allow(int32_t v) { return v ? "ALLOW" : "REJECT"; }
static const char *nm_pierce(int32_t v) { return v ? "PIERCE" : "BLOCK"; }
static const char *nm_group(int32_t v)
{
    static __thread char b[24];
    if (v < 0) return "no pierceable candidate";
    snprintf(b, sizeof b, "group %d", (int)v);
    return b;
}
static const char *nm_exh(int32_t v) { return v == 2 ? "weapon stopped" : v == 1 ? "weapon kept (aimed at a pierceable volume)" : "weapon not aimed at it"; }

/* ---------------------------------------------------------------- memo tables (pure functions of their keys) */
typedef struct { const void *ammo; int filtered; uint32_t key; char name[48]; } am_memo_t;
static am_memo_t m_ammo[512];
static am_memo_t *ammo_info(const uint8_t *ammo)
{
    am_memo_t *m = &m_ammo[mix64((uint64_t)(uintptr_t)ammo) & 511];
    if (m->ammo == ammo) return m;
    static char pl[1][TLEN];
    strcpy(pl[0], cfg_pierce);
    const uint8_t *tv = ammo + S.ammotags + 0x318;
    int pr = vec_has(tv, pl, 1), ex = n_exempt ? vec_has(tv, cfg_exempt, n_exempt) : 0;
    m->filtered = pr == 1 && ex == 0;
    m->key = vec_hash(ammo + 0x868 + 0x318) ^ 0x9E3779B9u;
    vec_str(ammo + 0x868 + 0x318, m->name, sizeof m->name, 2);
    m->ammo = ammo;
    return m;
}
typedef struct { const void *list, *ammo; void *params; } pm_memo_t;
static pm_memo_t m_par[1024];
static void *params_for(void *list, const uint8_t *ammo)
{
    if (!list) return NULL;
    pm_memo_t *m = &m_par[mix64((uint64_t)(uintptr_t)list * 31 + (uint64_t)(uintptr_t)ammo) & 1023];
    if (m->list == list && m->ammo == ammo) return m->params;
    void *raw = e_plist(list, (void *)(ammo + S.ammotags));
    void *pp = raw ? e_rtdc(raw, 0, S.pc_src, S.pc_dst, 0) : NULL;
    m->list = list; m->ammo = ammo; m->params = pp;
    return pp;
}
typedef struct { const void *params; float calibre; int cosb; bstate_t bs; } bs_memo_t;
static bs_memo_t m_bs[1024];
#define NCOSB 32
static float cos_rep(int b) { return (float)(b + 1) / NCOSB; }
static int cos_bucket(float c) { int b = (int)(c * NCOSB); return b < 0 ? 0 : b >= NCOSB ? NCOSB - 1 : b; }
static void bs_init(bstate_t *bs, float calibre)
{
    memset(bs, 0, sizeof *bs);
    bs->calibre = calibre;
    bs->f14[2] = 1.0f;          /* the 16-byte constant {0,0,1,0} AimInfo / HitVolume copy to +0x14 */
    bs->slope = 1.0f;
    bs->dv_type = -1;
}
static const bstate_t *bs_for(void *params, float calibre, int cosb)
{
    uint32_t cb; memcpy(&cb, &calibre, 4);
    bs_memo_t *m = &m_bs[mix64((uint64_t)(uintptr_t)params * 131 + cb * 7 + (uint64_t)cosb) & 1023];
    if (m->params == params && m->calibre == calibre && m->cosb == cosb) return &m->bs;
    bs_init(&m->bs, calibre);
    m->bs.dv_type = 0; m->bs.dv_val = 0.0f;
    e_angle(&m->bs, params, cos_rep(cosb));
    m->params = params; m->calibre = calibre; m->cosb = cosb;
    return &m->bs;
}
typedef struct { const void *armour, *info; uint8_t *mat; int set; } mat_memo_t;
static mat_memo_t m_mat[2048];
static uint8_t *mat_for(void *armour, void *vol)
{
    void *info = *(void **)((uint8_t *)vol + 0x30);
    mat_memo_t *m = &m_mat[mix64((uint64_t)(uintptr_t)armour * 17 + (uint64_t)(uintptr_t)info) & 2047];
    if (m->set && m->armour == armour && m->info == info) return m->mat;
    rec_t r; memset(&r, 0, sizeof r); r.vol = vol; r.nrm[2] = 1.0f; r.dir[2] = -1.0f;
    m->mat = (uint8_t *)e_matof(armour, &r);
    m->armour = armour; m->info = info; m->set = 1;
    return m->mat;
}
typedef struct { const void *info, *armour, *mat, *params; uint32_t fb, mb, cb; int face, cosb, rule; float thr; int set; } thr_memo_t;
static thr_memo_t m_thr[8192];
#define INF_THR 1e30f
/* pen6 needed to pierce face `face` of `vol` at incidence bucket cosb (engine's own sub-functions) */
static float face_threshold(void *vol, void *armour, float factor, float mult, uint8_t *mat, void *params, float calibre, int face, int cosb)
{
    void *info = *(void **)((uint8_t *)vol + 0x30);
    uint32_t fb, mb, cb; memcpy(&fb, &factor, 4); memcpy(&mb, &mult, 4); memcpy(&cb, &calibre, 4);
    uint64_t h = mix64((uint64_t)(uintptr_t)info ^ ((uint64_t)(uintptr_t)armour << 1) ^ ((uint64_t)(uintptr_t)params << 2)
                       ^ ((uint64_t)fb << 32) ^ mb ^ ((uint64_t)cb << 16) ^ ((uint64_t)face << 56) ^ ((uint64_t)cosb << 48) ^ ((uint64_t)cfg_rule << 60));
    thr_memo_t *m = &m_thr[h & 8191];
    if (m->set && m->info == info && m->armour == armour && m->mat == mat && m->params == params && m->fb == fb && m->mb == mb
        && m->cb == cb && m->face == face && m->cosb == cosb && m->rule == cfg_rule) return m->thr;
    bstate_t bs = *bs_for(params, calibre, cosb);
    if (mat[0xB0] & 6) bs.slope = 1.0f;
    rec_t r; memset(&r, 0, sizeof r); r.vol = vol; r.face = face; r.nrm[2] = 1.0f; r.dir[2] = -1.0f;
    blk_t b; memset(&b, 0, sizeof b);
    b.armour = armour; b.rec = &r; b.mat = mat; b.bs = &bs; b.mult = mult; b.enemy = 1;
    float t;
    float minc = *(float *)(mat + 0x10);
    if ((minc != 0.0f && !(bs.calibre >= minc)) || (mat[0xB0] & 1)) t = INF_THR;
    else {
        float th = e_thick(&b, 0);
        float eff = e_eff(&b, th);
        float t1 = e_thr(&b);
        t = cfg_rule == 3 ? (t1 > eff ? t1 : eff) : t1;
        if (!(t == t)) t = 0.0f;   /* NaN: do not filter */
    }
    m->info = info; m->armour = armour; m->mat = mat; m->params = params; m->fb = fb; m->mb = mb; m->cb = cb;
    m->face = face; m->cosb = cosb; m->rule = cfg_rule; m->thr = t; m->set = 1;
    return t;
}

/* ---------------------------------------------------------------- volume frame (box / polyhedron trace volumes)
 * world->local matrix at volume+0x58+0x30 (what the eBox trace reads); rows normalised: ax / ay / az = the volume's local
 * axes in world space, c = its centre. 1 = box, 2 = polyhedron, 0 = other shape or a degenerate matrix. */
static int vol_frame(const uint8_t *vol, float ax[3], float ay[3], float az[3], float c[3])
{
    const uint8_t *tv = *(const uint8_t *const *)(vol + 0x50);
    if (!tv || !S.box_vt) return 0;
    int mode = *(uint8_t *const *)tv == S.box_vt ? 1 : *(uint8_t *const *)tv == S.poly_vt ? 2 : 0;
    if (!mode) return 0;
    const float *m = (const float *)(vol + 0x58 + 0x30);
    ax[0] = m[0]; ax[1] = m[3]; ax[2] = m[6]; ay[0] = m[1]; ay[1] = m[4]; ay[2] = m[7]; az[0] = m[2]; az[1] = m[5]; az[2] = m[8];
    float *v[3] = { ax, ay, az };
    for (int i = 0; i < 3; i++) {
        float l = sqrtf(v[i][0] * v[i][0] + v[i][1] * v[i][1] + v[i][2] * v[i][2]);
        if (!(l > 1e-6f) || !(l < 1e6f)) return 0;
        v[i][0] /= l; v[i][1] /= l; v[i][2] /= l;
    }
    /* local = M*p + t with orthonormal rows -> centre = -(t0*ax + t1*ay + t2*az) */
    for (int k = 0; k < 3; k++) c[k] = -(m[9] * ax[k] + m[10] * ay[k] + m[11] * az[k]);
    return mode;
}
/* unit direction from a volume to the eye: in the volume's own frame for box / polyhedron volumes (1; so the target or the
 * part turning counts as a change), else in world axes from the target's origin (2); 0 = degenerate */
static int vol_dir(const uint8_t *vol, const uint8_t *target, const float *eye, float u[3])
{
    float ax[3], ay[3], az[3], c[3], d[3];
    int fr = vol ? vol_frame(vol, ax, ay, az, c) : 0;
    if (!fr) { const float *tp = (const float *)(target + 0x74); c[0] = tp[0]; c[1] = tp[1]; c[2] = tp[2]; }
    d[0] = eye[0] - c[0]; d[1] = eye[1] - c[1]; d[2] = eye[2] - c[2];
    float l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (!(l > 1e-3f)) return 0;
    d[0] /= l; d[1] /= l; d[2] /= l;
    if (!fr) { u[0] = d[0]; u[1] = d[1]; u[2] = d[2]; return 2; }
    u[0] = ax[0] * d[0] + ax[1] * d[1] + ax[2] * d[2];
    u[1] = ay[0] * d[0] + ay[1] * d[1] + ay[2] * d[2];
    u[2] = az[0] * d[0] + az[1] * d[1] + az[2] * d[2];
    l = sqrtf(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
    if (!(l > 1e-6f)) return 0;
    u[0] /= l; u[1] /= l; u[2] /= l;
    return 1;
}

/* ---------------------------------------------------------------- deterministic sim caches (stable keys only)
 * Keys: entity ids (+0xC0), the ammo's name-tag hash mixed with the weapon stuff's name-tag hash, volume index in the target's
 * list; values: sim clock, distances and directions. Direct-mapped by those keys, so which entry a collision evicts is the same
 * on every machine. Cleared when the sim clock goes back (new battle). */
static int aged(uint32_t t, uint32_t now, int ms) { return (uint32_t)(now - t) >= (uint32_t)ms; }
static int nearer(float was, float now) { return g_holdunits > 0.0f && was - now > g_holdunits; }     /* came > HoldDistanceM nearer */
static int farther(float was, float now) { return g_holdunits > 0.0f && now - was > g_holdunits; }    /* moved > HoldDistanceM away */
/* key of an ammo fired from a weapon stuff: the ammo's name-tag hash mixed with the weapon stuff's (eStuffWeapon has eStuff at
 * offset 0, name Tags at +0x868, vector +0x318: the same field as the ammo's) */
static uint32_t wa_key(uint32_t akey, const uint8_t *ws)
{
    uint32_t w = ws ? vec_hash(ws + 0x868 + 0x318) : 0;
    return akey ^ (w * 0x9E3779B9u);
}

/* blocked volumes (from B): kept until the direction to the shooter in the volume's frame turns by more than 25 deg, the shooter
 * comes more than HoldDistanceM nearer, or CacheMs (safety limit) runs out. Moving away keeps them: penetration only falls. */
typedef struct { uint32_t sid, tid, key, vidx, t; float dist; float u[3]; int fr; int set; } blk_cache_t;
static blk_cache_t c_blk[8192];
static blk_cache_t *blk_slot(uint32_t sid, uint32_t tid, uint32_t key, uint32_t vidx)
{
    return &c_blk[mix64(((uint64_t)sid << 32 | tid) ^ ((uint64_t)key << 7) ^ vidx * 0x9E3779B97F4A7C15ULL) & 8191];
}
static int blk_hit(uint32_t sid, uint32_t tid, uint32_t key, uint32_t vidx, uint32_t now, float gdist,
                   const uint8_t *vol, const uint8_t *target, const float *eye)
{
    blk_cache_t *b = blk_slot(sid, tid, key, vidx);
    if (!(b->set && b->sid == sid && b->tid == tid && b->key == key && b->vidx == vidx)) return 0;
    if (aged(b->t, now, cfg_cachems) || nearer(b->dist, gdist)) return 0;
    float u[3]; int fr = vol_dir(vol, target, eye, u);
    if (b->fr && fr && (fr != b->fr || b->u[0] * u[0] + b->u[1] * u[1] + b->u[2] * u[2] < BLK_COS)) return 0;
    return 1;
}
static void blk_put(uint32_t sid, uint32_t tid, uint32_t key, uint32_t vidx, uint32_t now, float gdist,
                    const uint8_t *vol, const uint8_t *target, const float *eye)
{
    blk_cache_t *b = blk_slot(sid, tid, key, vidx);
    b->set = 1; b->sid = sid; b->tid = tid; b->key = key; b->vidx = vidx; b->t = now; b->dist = gdist;
    b->fr = vol_dir(vol, target, eye, b->u);
}
/* ammo verdicts (from A, rewritten by X): VerdictHoldMs; a REJECT is dropped when the shooter comes more than HoldDistanceM
 * nearer, an ALLOW when it moves that much away; either when the penetration-range state (pen >= 1e-5) differs */
typedef struct { uint32_t sid, tid, key, t; float dist; int32_t verdict; int rs, set; } hold_t;
static hold_t c_hold[2048];
static hold_t *hold_slot(uint32_t sid, uint32_t tid, uint32_t key)
{
    return &c_hold[mix64(((uint64_t)sid << 32 | tid) ^ ((uint64_t)key * 0x9E3779B97F4A7C15ULL)) & 2047];
}
static int hold_get(uint32_t sid, uint32_t tid, uint32_t key, uint32_t now, float gdist, int rs, int *v, uint32_t *since)
{
    hold_t *h = hold_slot(sid, tid, key);
    if (!(h->set && h->sid == sid && h->tid == tid && h->key == key) || aged(h->t, now, cfg_holdms) || h->rs != rs) return 0;
    if (h->verdict ? farther(h->dist, gdist) : nearer(h->dist, gdist)) return 0;
    *v = h->verdict; if (since) *since = h->t;
    return 1;
}
static void hold_put(uint32_t sid, uint32_t tid, uint32_t key, uint32_t now, float gdist, int rs, int v)
{
    hold_t *h = hold_slot(sid, tid, key);
    h->set = 1; h->sid = sid; h->tid = tid; h->key = key; h->t = now; h->dist = gdist; h->rs = rs; h->verdict = v;
}
/* volumes the solver could not reach (no solution, elevation limit, line of fire blocked) on a list P had cut: UnreachableMs */
typedef struct { uint32_t sid, tid, key, vidx, t; int set; } unr_t;
static unr_t c_unr[2048];
static unr_t *unr_slot(uint32_t sid, uint32_t tid, uint32_t key, uint32_t vidx)
{
    return &c_unr[mix64(((uint64_t)sid << 32 | tid) ^ ((uint64_t)key << 9) ^ vidx * 0xC2B2AE3D27D4EB4FULL) & 2047];
}
static int unr_hit(uint32_t sid, uint32_t tid, uint32_t key, uint32_t vidx, uint32_t now)
{
    unr_t *u = unr_slot(sid, tid, key, vidx);
    return u->set && u->sid == sid && u->tid == tid && u->key == key && u->vidx == vidx && !aged(u->t, now, cfg_unrms);
}
static void unr_put(uint32_t sid, uint32_t tid, uint32_t key, uint32_t vidx, uint32_t now)
{
    unr_t *u = unr_slot(sid, tid, key, vidx);
    u->set = 1; u->sid = sid; u->tid = tid; u->key = key; u->vidx = vidx; u->t = now;
}

/* new battle: the sim clock starts at 0 again and entity ids repeat, so every cache and memo (and the change-only log with
 * its budget) is cleared the first time a hook sees the clock go back. Caller holds g_lock. */
static uint32_t g_last_now, g_epoch;
static void epoch_check(uint32_t now)
{
    if (now < g_last_now) {
        uint32_t was = g_last_now;
        memset(c_blk, 0, sizeof c_blk); memset(c_hold, 0, sizeof c_hold); memset(c_unr, 0, sizeof c_unr);
        memset(m_ammo, 0, sizeof m_ammo); memset(m_par, 0, sizeof m_par); memset(m_bs, 0, sizeof m_bs);
        memset(m_mat, 0, sizeof m_mat); memset(m_thr, 0, sizeof m_thr); memset(g_vl, 0, sizeof g_vl);
        g_epoch++; ST.flushes++;
        g_vlines = 0;
        if (cfg_log) logf_("new battle [%u ms]: sim clock went back from %u ms, caches cleared (LogVerdicts budget restarts)", now, was);
    }
    g_last_now = now;
}

/* ---------------------------------------------------------------- builder record (per thread) */
typedef struct { void *ctx, *weapon, *ret, *r14; } brec_t;
static __thread brec_t t_rec;
__attribute__((used)) void pf_record(void *ctx, void *weapon, void *ret, void *r14)
{
    t_rec.ctx = ctx; t_rec.weapon = weapon; t_rec.ret = ret; t_rec.r14 = r14;
}

/* ---------------------------------------------------------------- per-pick state (per thread): P opens it, B counts, X closes */
static __thread struct {
    int open, tries, maxtries, bblocks, haskey, cut;
    void *target, *weapon;
    uint32_t sid, tid, key; float gdist;
} t_pick;

/* ---------------------------------------------------------------- shared pierce test */
typedef struct {
    int state;                  /* 0 = do not filter, 1 = filter, 2 = reject every volume */
    uint8_t *ammo, *shooter, *target; am_memo_t *am;
    void *params; float calibre, dist, gdist, pen, pen6, factor, mult; void *armour;
    float spos[3]; uint32_t sid, tid, key;
    int rs;                     /* penetration-range state: 1 = pen >= 1e-5 at this distance */
    int verdict, held; uint32_t since;
} prep_t;
typedef struct { int vidx, face, nvol, ncached; float cos, thr; } vinfo_t;

/* fills the numbers both A and P need once the target is known to be a filtered-ammo enemy vehicle */
static void prep_fill(prep_t *p, uint8_t *ammo, uint8_t *shooter, uint8_t *target, am_memo_t *am, void *params, void *cw, float pendist,
                      const uint8_t *ws)
{
    p->ammo = ammo; p->shooter = shooter; p->target = target; p->am = am; p->params = params;
    p->key = wa_key(am->key, ws);
    p->calibre = *(float *)((uint8_t *)cw + S.caliboff);
    p->dist = pendist;
    uint32_t db; memcpy(&db, &p->dist, 4);
    p->pen = e_pen(params, db);
    int32_t dv[2] = { 0, 0 }; memcpy(&dv[1], &p->pen, 4);
    p->pen6 = e_dvget(dv, 6);
    p->armour = e_armorof(target);
    p->factor = p->armour ? *(float *)p->armour : 1.0f;
    p->mult = 1.0f;
    e_mult(0xB, target, &p->mult);
    float *sp = (float *)(shooter + 0x74);
    p->spos[0] = sp[0]; p->spos[1] = sp[1]; p->spos[2] = sp[2] + 30.0f;   /* ~1.5 m eye height (20 units / m) */
    p->sid = ent_id(shooter); p->tid = ent_id(target);
    p->gdist = edist(shooter, target);
    p->rs = p->pen >= 1e-5f;
    p->state = p->rs ? 1 : 2;                                      /* HitVolume: below 1e-5 nothing pierces */
}

/* cos between each face index's outward axis and the line of sight (eBox faces / .vol SIDE bytes share one frame:
 * 0 front +X, 1 top +Z, 2 left +Y, 3 right -Y, 4 rear -X, 5 bottom -Z). 1 = box (exact planes), 2 = polyhedron
 * (triangles carry a SIDE byte, normally along those axes but slanted), 0 = other shape / no facing */
static int box_faces(uint8_t *vol, const float *eye, float cs[6])
{
    if (!facing_on) return 0;
    float ax[3], ay[3], az[3], c[3];
    int mode = vol_frame(vol, ax, ay, az, c);
    if (!mode) return 0;
    float d[3] = { c[0] - eye[0], c[1] - eye[1], c[2] - eye[2] };
    float l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (!(l > 1e-3f)) return 0;
    d[0] /= l; d[1] /= l; d[2] /= l;
    float px = ax[0] * d[0] + ax[1] * d[1] + ax[2] * d[2];
    float py = ay[0] * d[0] + ay[1] * d[1] + ay[2] * d[2];
    float pz = az[0] * d[0] + az[1] * d[1] + az[2] * d[2];
    cs[0] = -px; cs[4] = px; cs[2] = -py; cs[3] = py; cs[1] = -pz; cs[5] = pz;
    return mode;
}

/* can this ammo pierce `vol` from here (optimistic: best face that faces the shooter); a volume B blocked is not pierceable */
static int vol_ok(prep_t *p, uint8_t *vol, uint32_t vidx, uint32_t now, vinfo_t *vi)
{
    if (vi) { vi->vidx = (int)vidx; vi->face = -1; vi->cos = 0; vi->thr = INF_THR; }
    if (blk_hit(p->sid, p->tid, p->key, vidx, now, p->gdist, vol, p->target, p->spos)) { if (vi) vi->face = -2; return 0; }
    if (!p->armour) return 1;
    uint8_t *mat = mat_for(p->armour, vol);
    if (!mat || (mat[0xB0] & 4)) return 1;                      /* computed-thickness material: unknown here, let hook B judge */
    float cs[6];
    int box = box_faces(vol, p->spos, cs);
    float best = INF_THR; int bf = -1; float bcos = 0;
    for (int f = 0; f < 6; f++) {
        float c = box == 1 ? cs[f] : 1.0f;
        if (box == 1 && !(cs[f] > 0.1f)) continue;              /* box: face turned away (or grazing past ~84 deg) */
        if (box == 2 && !(cs[f] > -0.2f)) continue;             /* polyhedron: only sides clearly turned away drop out, head-on */
        float t = face_threshold(vol, p->armour, p->factor, p->mult, mat, p->params, p->calibre, f, cos_bucket(c));
        if (t < best) { best = t; bf = f; bcos = c; }
        if (p->pen6 >= t) { if (vi) { vi->face = f; vi->cos = c; vi->thr = t; } return 1; }
    }
    if (box && bf < 0) {                                         /* degenerate: shooter inside / along the box: any face */
        for (int f = 0; f < 6; f++) {
            float t = face_threshold(vol, p->armour, p->factor, p->mult, mat, p->params, p->calibre, f, NCOSB - 1);
            if (p->pen6 >= t) { if (vi) { vi->face = f; vi->cos = 1; vi->thr = t; } return 1; }
        }
    }
    if (vi) { vi->face = bf; vi->cos = bcos; vi->thr = best; }
    return 0;
}

static char g_notarget[1][TLEN] = { "no_target" };
static int vol_candidate(uint8_t *vol)                              /* the picker's own rule: unbroken, not `no_target` */
{
    if (!vol) return 0;
    uint8_t *st = *(uint8_t **)(vol + 0x38);
    if (st && st[0x24]) return 0;
    uint8_t *info = *(uint8_t **)(vol + 0x30);
    return !(info && vec_has(info + 0x398, g_notarget, 1) == 1);
}
/* ammo verdict: any candidate volume pierceable (vi = the passing one, or the nearest miss) */
static int any_volume_ok(prep_t *p, uint32_t now, vinfo_t *vi)
{
    uint8_t **b = *(uint8_t ***)(p->target + 0x510), **e = *(uint8_t ***)(p->target + 0x518);
    vi->vidx = -1; vi->face = -1; vi->thr = INF_THR; vi->cos = 0; vi->nvol = 0; vi->ncached = 0;
    if (!b || e < b || e - b > 4096) return 1;
    for (uint8_t **v = b; v < e; v++) {
        if (!vol_candidate(*v)) continue;
        vinfo_t w;
        vi->nvol++;
        if (vol_ok(p, *v, (uint32_t)(v - b), now, &w)) { int n = vi->nvol, c = vi->ncached; *vi = w; vi->nvol = n; vi->ncached = c; return 1; }
        if (w.face == -2) vi->ncached++;
        else if (w.thr < vi->thr) { int n = vi->nvol, c = vi->ncached; *vi = w; vi->nvol = n; vi->ncached = c; }
    }
    return 0;
}

/* ---------------------------------------------------------------- hook A */
static __thread struct { void *ctx, *ammo, *shooter, *target, *weapon, *r14; uint32_t now, epoch; prep_t p; } t_prep;

static void a_verdict(prep_t *p, uint32_t now)
{
    int v; vinfo_t vi; memset(&vi, 0, sizeof vi); vi.vidx = -1; vi.face = -1; vi.thr = INF_THR;
    if (hold_get(p->sid, p->tid, p->key, now, p->gdist, p->rs, &v, &p->since)) { p->held = 1; ST.a_held++; }
    else {
        v = p->state == 1 ? any_volume_ok(p, now, &vi) : 0;
        hold_put(p->sid, p->tid, p->key, now, p->gdist, p->rs, v);
        p->since = now;
        if (v) ST.a_allow++; else ST.a_reject++;
    }
    p->verdict = v;
    p->state = v ? 1 : 2;
    int32_t pv; uint32_t preps;
    if (!vl_change(0, p->sid, p->tid, p->key, v, &pv, &preps)) return;
    char a[128], b[128], w[80], d[200];
    ent_desc(p->shooter, a, sizeof a); ent_desc(p->target, b, sizeof b); was_str(w, sizeof w, pv, preps, nm_allow);
    if (p->held) snprintf(d, sizeof d, "held since %u ms", p->since);
    else if (!(p->pen >= 1e-5f)) snprintf(d, sizeof d, "no penetration at this range");
    else if (v) snprintf(d, sizeof d, "vol#%d face %d cos %.2f needs %.1f", vi.vidx, vi.face, vi.cos, vi.thr);
    else if (vi.vidx >= 0) snprintf(d, sizeof d, "no candidate pierceable (%d volumes, %d blocked by B); nearest vol#%d face %d needs %s%.1f",
                                     vi.nvol, vi.ncached, vi.vidx, vi.face, vi.thr >= INF_THR ? "no-pierce material/calibre " : "", vi.thr >= INF_THR ? 0.0f : vi.thr);
    else snprintf(d, sizeof d, "no candidate pierceable (%d volumes, %d blocked by B)", vi.nvol, vi.ncached);
    logf_("A [%u ms] shooter %s target %s ammo \"%s\" d=%.0f (%.1f m) pen=%.1f (%.1f armour units) -> %s (%s)%s",
          now, a, b, p->am->name, p->dist, p->dist / 20.0f, p->pen, p->pen6, v ? "ALLOW" : "REJECT", d, w);
}

static void prep_ctx(uint8_t *ctx, prep_t *p, uint32_t now)
{
    memset(p, 0, sizeof *p);
    uint8_t *ammo = *(uint8_t **)ctx, *shooter = *(uint8_t **)(ctx + 8), *ti = *(uint8_t **)(ctx + 0x10);
    uint8_t *target = ti ? *(uint8_t **)(ti + 0x18) : NULL;
    if (!ammo || !shooter || !target || !is_vehicle(target) || target == shooter) return;
    am_memo_t *am = ammo_info(ammo);
    if (!am->filtered) return;
    if (!cfg_player && player_unit(shooter)) return;
    if (is_manual(shooter)) { ST.dc_skip++; return; }
    /* the weapon this ammo is matched for (recorded by the ctx builder right before this MatchAmmo call) */
    void *list = NULL, *ws = NULL;
    if (t_rec.ctx != ctx) { ST.a_noweap++; return; }
    if (t_rec.weapon) {
        list = *(void **)((uint8_t *)t_rec.weapon + S.ilistoff);
        ws = *(void **)((uint8_t *)t_rec.weapon + S.wstuffoff);
    } else if (S.scan_ret[0] && (t_rec.ret == S.scan_ret[0] || t_rec.ret == S.scan_ret[1]) && t_rec.r14) {
        ws = e_wcast(t_rec.r14);
        list = ws ? *(void **)((uint8_t *)ws + S.wlistoff) : NULL;
    }
    if (!list || !ws) { ST.a_noweap++; return; }
    void *params = params_for(list, ammo);
    void *cw = e_rtdc(ws, 0, S.wc_src, S.wc_dst, 0);
    if (!params || !cw) return;
    if (!e_rel(shooter, target)) return;                       /* friendly: the engine never pierces those anyway */
    float *sp = (float *)(shooter + 0x74), *tp = (float *)(ti + 0x28);
    float dx = sp[0] - tp[0], dy = sp[1] - tp[1], dz = sp[2] - tp[2];
    prep_fill(p, ammo, shooter, target, am, params, cw, sqrtf(dx * dx + dy * dy + dz * dz), (const uint8_t *)ws);
    a_verdict(p, now);
}
static prep_t *prep(uint8_t *ctx)   /* caller holds g_lock */
{
    uint32_t now = now_ms();
    epoch_check(now);
    uint8_t *ti = *(uint8_t **)(ctx + 0x10);
    void *ammo = *(void **)ctx, *sh = *(void **)(ctx + 8), *tg = ti ? *(void **)(ti + 0x18) : NULL;
    if (t_prep.ctx == ctx && t_prep.ammo == ammo && t_prep.shooter == sh && t_prep.target == tg && t_prep.now == now
        && t_prep.epoch == g_epoch && t_rec.ctx == ctx && t_prep.weapon == t_rec.weapon && t_prep.r14 == t_rec.r14) return &t_prep.p;
    prep_ctx(ctx, &t_prep.p, now);
    t_prep.ctx = ctx; t_prep.ammo = ammo; t_prep.shooter = sh; t_prep.target = tg; t_prep.now = now; t_prep.epoch = g_epoch;
    t_prep.weapon = t_rec.weapon; t_prep.r14 = t_rec.r14;
    return &t_prep.p;
}

__attribute__((used)) int pf_vol(uint8_t *ctx, uint8_t **slot)
{
    if (!hookA_on) return 1;
    AcquireSRWLockExclusive(&g_lock);
    prep_t *p = prep(ctx);
    int ok = 1;
    if (p->state == 2) ok = 0;
    else if (p->state == 1) {
        uint8_t **b = *(uint8_t ***)(p->target + 0x510);
        ok = vol_ok(p, *slot, (uint32_t)(slot - b), now_ms(), NULL);
    }
    if (p->state) { if (ok) ST.a_vpass++; else ST.a_vblock++; }
    ReleaseSRWLockExclusive(&g_lock);
    return ok;
}

__attribute__((used)) int pf_novol(uint8_t *ctx, uint8_t *row)
{
    (void)row;
    if (!hookA_on) return 1;
    AcquireSRWLockExclusive(&g_lock);
    prep_t *p = prep(ctx);
    int ok = p->state != 2;
    if (p->state) { if (ok) ST.a_rpass++; else ST.a_rblock++; }
    ReleaseSRWLockExclusive(&g_lock);
    return ok;
}

/* ---------------------------------------------------------------- hook P: picker candidate list */
static void prep_weapon(prep_t *p, uint8_t *weapon, uint8_t *shooter, uint8_t *target, uint8_t *ammo)
{
    memset(p, 0, sizeof *p);
    if (!is_vehicle(target) || target == shooter) return;
    am_memo_t *am = ammo_info(ammo);
    if (!am->filtered) return;
    if (!cfg_player && player_unit(shooter)) return;
    if (is_manual(shooter)) { ST.dc_skip++; return; }
    void *raw = e_P(weapon);
    void *params = raw ? e_rtdc(raw, 0, S.pc_src, S.pc_dst, 0) : NULL;
    void *wsd = *(void **)(weapon + S.wstuffoff);
    void *cw = wsd ? e_rtdc(wsd, 0, S.wc_src, S.wc_dst, 0) : NULL;
    if (!params || !cw) return;
    if (!e_rel(shooter, target)) return;
    prep_fill(p, ammo, shooter, target, am, params, cw, edist(shooter, target), (const uint8_t *)wsd);
}
/* keep the flagged candidates in their order; *cur follows its volume or becomes -1 */
static int pick_compact(uint8_t **b, int n, const uint8_t *keep, int *cur)
{
    int j = 0, nc = -1;
    for (int i = 0; i < n; i++) if (keep[i]) { if (i == *cur) nc = j; b[j++] = b[i]; }
    *cur = nc;
    return j;
}
#define PICK_MAX 512
__attribute__((used)) uint8_t ***g_vb, ***g_ve;
/* P: rcx = aim, rdx = weapon, r8 = target, r9 = previous-volume index (ebp), [5] = noAimCheck; returns the new ebp */
__attribute__((used)) int pf_pick(uint8_t *aim, uint8_t *weapon, uint8_t *target, int64_t cur64, int64_t noaim)
{
    (void)aim; (void)noaim;
    int cur = (int)(int32_t)cur64;
    uint8_t **b = g_vb ? *g_vb : NULL, **e = g_ve ? *g_ve : NULL;
    int n = (b && e >= b && e - b <= 4096) ? (int)(e - b) : 0;
    t_pick.open = 1; t_pick.target = target; t_pick.weapon = weapon;
    t_pick.tries = 0; t_pick.bblocks = 0; t_pick.haskey = 0; t_pick.cut = 0;
    t_pick.maxtries = n < 5 ? n : 5;
    if (!prio_on || !weapon || !target || n <= 0 || n > PICK_MAX || !is_vehicle(target)) return cur;
    uint8_t *shooter = *(uint8_t **)(weapon + 0x90);
    void *slot = *(void **)(weapon + 0xB0);
    uint8_t *ammo = slot ? (uint8_t *)e_loaded(slot) : NULL;
    if (!shooter || !ammo) return cur;
    AcquireSRWLockExclusive(&g_lock);
    uint32_t now = now_ms();
    epoch_check(now);
    prep_t p; prep_weapon(&p, weapon, shooter, target, ammo);
    if (!p.state) { ReleaseSRWLockExclusive(&g_lock); return cur; }
    ST.p_picks++;
    t_pick.haskey = 1; t_pick.sid = p.sid; t_pick.tid = p.tid; t_pick.key = p.key; t_pick.gdist = p.gdist;
    uint8_t keep[PICK_MAX]; int grp[PICK_MAX], best = INT_MAX, nunr = 0;
    uint8_t **lb = *(uint8_t ***)(target + 0x510), **le = *(uint8_t ***)(target + 0x518);
    size_t m = (lb && le >= lb && le - lb <= 4096) ? (size_t)(le - lb) : 0, j = 0;
    for (int i = 0; i < n; i++) {
        grp[i] = INT_MAX;
        if (p.state != 1 || !b[i]) continue;
        size_t k = j;                                             /* candidates come in list order: walk on, else search again */
        while (k < m && lb[k] != b[i]) k++;
        if (k == m) for (k = 0; k < m && lb[k] != b[i]; k++);
        if (k == m) continue;
        j = k + 1;
        if (!vol_ok(&p, b[i], (uint32_t)k, now, NULL)) continue;
        if (unr_hit(p.sid, p.tid, p.key, (uint32_t)k, now)) { nunr++; continue; }   /* the solver could not reach it a moment ago */
        grp[i] = vol_group(b[i]); if (grp[i] < best) best = grp[i];
    }
    ST.p_unr += (uint32_t)nunr;
    int32_t pv; uint32_t preps; int kept = n, nc = cur;
    if (best == INT_MAX) ST.p_none++;
    else {
        for (int i = 0; i < n; i++) keep[i] = grp[i] == best;
        kept = pick_compact(b, n, keep, &nc);
        *g_ve = b + kept;
        t_pick.maxtries = kept < 5 ? kept : 5;
        t_pick.cut = 1;
        ST.p_removed += (uint32_t)(n - kept);
        if (cur >= 0 && nc < 0) ST.p_curdrop++;
    }
    if (vl_change(2, p.sid, p.tid, p.key, best == INT_MAX ? -1 : best, &pv, &preps)) {
        char a[128], t[128], w[80], g[160] = "", u[48] = "";
        ent_desc(p.shooter, a, sizeof a); ent_desc(p.target, t, sizeof t); was_str(w, sizeof w, pv, preps, nm_group);
        if (best != INT_MAX) for (int i = 0; i < n_ptag; i++) if (g_ptag_grp[i] == best && strlen(g) + strlen(g_ptag[i]) + 2 < sizeof g) {
            if (g[0]) strcat(g, ","); strcat(g, g_ptag[i]);
        }
        if (nunr) snprintf(u, sizeof u, ", %d pierceable but unreachable skipped", nunr);
        if (best == INT_MAX) logf_("P [%u ms] shooter %s target %s ammo \"%s\" d=%.0f (%.1f m) pen=%.1f -> no reachable pierceable candidate of %d%s, list left as it is%s",
                                   now, a, t, p.am->name, p.dist, p.dist / 20.0f, p.pen6, n, u, w);
        else logf_("P [%u ms] shooter %s target %s ammo \"%s\" d=%.0f (%.1f m) pen=%.1f -> aim group %d (%s): %d of %d candidates kept%s%s%s",
                   now, a, t, p.am->name, p.dist, p.dist / 20.0f, p.pen6, best, best >= n_grp ? "volumes in no group" : g, kept, n,
                   cur >= 0 && nc < 0 ? ", previous volume dropped" : "", u, w);
    }
    ReleaseSRWLockExclusive(&g_lock);
    return nc;
}

/* ---------------------------------------------------------------- hook B: picker -> solver */
__attribute__((used)) int pf_solve(uint8_t *firer, uint8_t *aim, void *a3, void *a4, uint8_t *outrec, void *a6)
{
    __attribute__((aligned(16))) uint8_t local[0x60];
    uint8_t *r = (outrec || !hookB_on) ? outrec : local;          /* B asks for the hit record; without B the call is the engine's */
    uint8_t *target = *(uint8_t **)(aim + 0x18);
    int inpick = t_pick.open && t_pick.target == target;
    if (inpick) t_pick.tries++;
    int st = e_solve(firer, aim, a3, a4, r, a6);
    if (st != 3) {
        /* no solution / elevation limit / line of fire blocked on a list P cut: that volume is unreachable for a moment, so the
         * next pick moves on to the next pierceable volume or group (not an armour verdict: A never reads it) */
        if (inpick && t_pick.cut && t_pick.haskey && e_getvol && target) {
            AcquireSRWLockExclusive(&g_lock);
            uint32_t now = now_ms();
            epoch_check(now);
            uint8_t *av = (uint8_t *)e_getvol(aim);
            uint32_t ai = av ? vol_index(target, av) : UINT32_MAX;
            if (ai != UINT32_MAX) { unr_put(t_pick.sid, t_pick.tid, t_pick.key, ai, now); ST.s_unr++; }
            ReleaseSRWLockExclusive(&g_lock);
        }
        return st;
    }
    if (!hookB_on) return st;
    uint8_t *weapon = *(uint8_t **)(firer + 0x10);
    if (!weapon) return st;
    uint8_t *shooter = *(uint8_t **)(weapon + 0x90);
    uint8_t *hitent = *(uint8_t **)(r + 0x18), *vol = *(uint8_t **)(r + 0x20);
    if (!shooter || !target || hitent != target || !vol || !is_vehicle(target)) return st;
    if (!cfg_player && player_unit(shooter)) return st;
    void *slot = *(void **)(weapon + 0xB0);
    uint8_t *ammo = slot ? (uint8_t *)e_loaded(slot) : NULL;
    if (!ammo) return st;
    AcquireSRWLockExclusive(&g_lock);
    uint32_t now0 = now_ms();
    epoch_check(now0);
    am_memo_t *am = ammo_info(ammo);
    int res = st;
    if (am->filtered && is_manual(shooter)) ST.dc_skip++;
    else if (am->filtered && e_rel(shooter, target)) {
        void *raw = e_P(weapon);
        void *params = raw ? e_rtdc(raw, 0, S.pc_src, S.pc_dst, 0) : NULL;
        void *wsd = *(void **)(weapon + S.wstuffoff);
        void *cw = wsd ? e_rtdc(wsd, 0, S.wc_src, S.wc_dst, 0) : NULL;
        if (params && cw) {
            float *sp = (float *)(shooter + 0x74), *pt = (float *)(r + 8);
            float dx = sp[0] - pt[0], dy = sp[1] - pt[1], dz = sp[2] - pt[2];
            float dist = sqrtf(dx * dx + dy * dy + dz * dz);
            uint32_t db; memcpy(&db, &dist, 4);
            float pen = e_pen(params, db);
            rec_t R; memset(&R, 0, sizeof R);
            memcpy(R.pt, r + 8, 12); memcpy(R.nrm, r + 0x28, 12); memcpy(R.dir, r + 0x34, 12);
            R.face = *(int32_t *)(r + 0x40); R.vol = vol; R.flag = 0;
            float cosv = fabsf(R.nrm[0] * R.dir[0] + R.nrm[1] * R.dir[1] + R.nrm[2] * R.dir[2]);
            if (!(cosv >= 0.01f)) cosv = 0.01f;
            bstate_t bs; bs_init(&bs, *(float *)((uint8_t *)cw + S.caliboff));
            bs.dv_type = 0; bs.dv_val = pen;
            bstate_t pre; blk_t b; memset(&b, 0, sizeof b); memset(&pre, 0, sizeof pre);
            int pierced = 0, evald = 0;
            if (pen >= 1e-5f) {
                e_angle(&bs, params, cosv);
                e_block(&b, target, shooter, &R, &bs);
                if (!b.mat || (b.mat[0xB0] & 4)) pierced = 1;      /* HitVolume: such a volume does not stop the round */
                else {
                    if (b.mat[0xB0] & 6) bs.slope = 1.0f;
                    pre = bs;
                    e_eval(&b);
                    evald = 1;
                    pierced = cfg_rule == 3 ? (bs.flags & 3) == 3 : (bs.flags & 1) != 0;
                    if (!b.enemy) pierced = 1;
                }
            }
            int32_t dv[2] = { 0, 0 }; memcpy(&dv[1], &pen, 4);
            float pen6 = e_dvget(dv, 6);
            uint32_t now = now0, sid = ent_id(shooter), tid = ent_id(target), key = wa_key(am->key, (const uint8_t *)wsd);
            float gdist = edist(shooter, target);
            float eye[3] = { sp[0], sp[1], sp[2] + 30.0f };             /* the same eye point A / P test from */
            uint32_t hidx = vol_index(target, vol);
            uint8_t *aimed = e_getvol ? (uint8_t *)e_getvol(aim) : NULL;
            uint32_t aidx = aimed ? vol_index(target, aimed) : UINT32_MAX;
            if (pierced) ST.b_pierce++;
            else {
                ST.b_block++;
                if (hidx != UINT32_MAX) blk_put(sid, tid, key, hidx, now, gdist, vol, target, eye);
                if (aimed && aimed != vol && aidx != UINT32_MAX) { blk_put(sid, tid, key, aidx, now, gdist, aimed, target, eye); ST.b_aimed++; }
                res = 5;
                if (inpick) { t_pick.bblocks++; t_pick.haskey = 1; t_pick.sid = sid; t_pick.tid = tid; t_pick.key = key; t_pick.gdist = gdist; }
            }
            int32_t pv; uint32_t preps;
            if (vl_change(1, sid, tid, key, pierced, &pv, &preps)) {
                /* log only: the flag-1 threshold the evaluator compares pen6 with (0x7FCDE0 on the pre-evaluation state, flag 2
                 * as the evaluator had set it by then) and the full-penetration armour; pure calls on copies */
                float thr1 = 0, eff = 0, minc = 0; uint32_t mfl = 0;
                if (evald) {
                    blk_t bb = b; bstate_t b2 = pre; b2.flags |= bs.flags & 2; bb.bs = &b2;
                    thr1 = e_thr(&bb);
                    bstate_t b3 = pre; bb.bs = &b3; eff = e_eff(&bb, e_thick(&bb, 0));
                    minc = *(float *)(b.mat + 0x10); mfl = b.mat[0xB0];
                }
                char a[128], t[128], w[80], hv[128], av[160], an[64] = "";
                ent_desc(shooter, a, sizeof a); ent_desc(target, t, sizeof t); was_str(w, sizeof w, pv, preps, nm_pierce);
                vol_desc(vol, hidx, hv, sizeof hv);
                size_t k; const char *s = str_get(aim + 0x60, &k);
                if (s && k < sizeof an) { memcpy(an, s, k); an[k] = 0; }
                if (aimed == vol) snprintf(av, sizeof av, "same volume \"%s\"", an);
                else { char x[128]; vol_desc(aimed, aidx, x, sizeof x); snprintf(av, sizeof av, "%s \"%s\"", x, an); }
                if (!evald) logf_("B [%u ms] shooter %s target %s ammo \"%s\" d=%.0f (%.1f m) pen=%.1f hit %s face %d cos %.2f, aimed %s -> %s (%s)%s",
                                  now, a, t, am->name, dist, dist / 20.0f, pen, hv, R.face, cosv, av, pierced ? "PIERCE" : "BLOCK",
                                  pen < 1e-5f ? "no penetration at this range" : "volume material does not stop the round", w);
                else logf_("B [%u ms] shooter %s target %s ammo \"%s\" d=%.0f (%.1f m) pen=%.1f pen6=%.1f vs flag-1 threshold %.1f "
                           "(full-penetration armour %.1f) calibre %.1f min %.1f mat flags 0x%X -> flags 0x%X%s; hit %s face %d cos %.2f, aimed %s -> %s%s",
                           now, a, t, am->name, dist, dist / 20.0f, pen, pen6, thr1, eff, pre.calibre, minc, mfl, bs.flags,
                           b.enemy ? "" : " (not an enemy)", hv, R.face, cosv, av, pierced ? "PIERCE" : "BLOCK", w);
            }
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    return res;
}

/* ---------------------------------------------------------------- hook X: the picker ran out of candidates */
static int slot_on(const uint8_t *slot, const uint8_t *target)
{
    return *(const int32_t *)(slot + 8) != 0 && *(uint8_t *const *)(slot + 0x18) == target;
}
__attribute__((used)) void pf_exhaust(uint8_t *aim, uint8_t *weapon, uint8_t *target)
{
    e_clear(aim);                                                 /* the engine's own call: the pick record ends with no aim */
    InterlockedIncrement(&ST.x_clear);                            /* every exit through this clear (empty list, no weapon, out of range ...) */
    int mine = t_pick.open && t_pick.target == target && t_pick.weapon == weapon;
    int full = mine && t_pick.maxtries > 0 && t_pick.tries >= t_pick.maxtries;    /* every try reached the solver and failed */
    if (full) InterlockedIncrement(&ST.x_exh);
    int bc = full && t_pick.bblocks > 0 && t_pick.haskey;
    t_pick.open = 0;
    if (!bc) return;
    int stop = 0, aimed = 0, v = -1;
    vinfo_t vi; memset(&vi, 0, sizeof vi);
    uint8_t *shooter = weapon ? *(uint8_t **)(weapon + 0x90) : NULL;
    AcquireSRWLockExclusive(&g_lock);
    uint32_t now = now_ms();
    epoch_check(now);
    ST.x_bc++;
    if (shooter && is_manual(shooter)) {                          /* taken over by the player since: the engine clear only */
        ST.dc_skip++;
        int32_t pv; uint32_t preps;
        if (vl_change(4, ent_id(shooter), 0, 0, 1, &pv, &preps))
            logf_("X [%u ms] shooter 0x%X is under direct control: left alone (no verdict, no stop)", now, ent_id(shooter));
        ReleaseSRWLockExclusive(&g_lock);
        return;
    }
    /* the ammo verdict again, now with the volumes B just blocked out: REJECT only when no candidate volume is left
     * (the pick may have had a {volume "x"} subset only); held like A's own verdict */
    void *slot = weapon ? *(void **)(weapon + 0xB0) : NULL;
    uint8_t *ammo = slot ? (uint8_t *)e_loaded(slot) : NULL;
    prep_t p; memset(&p, 0, sizeof p);
    if (shooter && ammo) {
        prep_weapon(&p, weapon, shooter, target, ammo);
        if (p.state) {
            v = p.state == 1 ? any_volume_ok(&p, now, &vi) : 0;
            hold_put(p.sid, p.tid, p.key, now, p.gdist, p.rs, v);
        }
    }
    /* stop a weapon whose aim slot holds this target only when that aim can no longer pierce: verdict REJECT, or the volume
     * in the slot is B-blocked / fails the face test. A weapon firing at a volume it can pierce (a lower group) keeps firing. */
    if (cfg_exstop && e_wstop && weapon && v >= 0 && *(void **)(weapon + S.wstuffoff)) {
        uint8_t *sl[2] = { weapon + S.ws1, weapon + S.ws2 };
        for (int i = 0; i < 2 && !stop; i++) {
            if (!slot_on(sl[i], target)) continue;
            aimed = 1;
            if (v == 0) { stop = 1; break; }
            uint8_t *sv = e_getvol ? (uint8_t *)e_getvol(sl[i]) : NULL;
            uint32_t si = sv ? vol_index(target, sv) : UINT32_MAX;
            if (si != UINT32_MAX && !vol_ok(&p, sv, si, now, NULL)) stop = 1;   /* volume unknown: can't judge, keep firing */
        }
    }
    if (aimed && !stop) ST.x_kept++;
    int32_t pv; uint32_t preps;
    if (vl_change(3, t_pick.sid, t_pick.tid, t_pick.key, stop ? 2 : aimed ? 1 : 0, &pv, &preps)) {
        char w[80]; was_str(w, sizeof w, pv, preps, nm_exh);
        logf_("X [%u ms] shooter 0x%X target 0x%X: pick ran out after %d tries, %d blocked by B -> ammo verdict %s held (%d volumes, %d blocked by B)%s%s",
              now, t_pick.sid, t_pick.tid, t_pick.tries, t_pick.bblocks, v < 0 ? "unchanged (ammo not filtered here)" : v ? "ALLOW" : "REJECT",
              vi.nvol, vi.ncached, stop ? ", weapon stopped (its aim at this target cannot pierce)" : aimed ? ", weapon kept (aimed at a pierceable volume)" : "", w);
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (stop) { e_wstop(weapon, 1); InterlockedIncrement(&ST.x_stop); }
}

/* ---------------------------------------------------------------- stubs */
__attribute__((used)) uint64_t g_a2_back, g_a2_match, g_a2_next, g_w_tramp, g_p_back;
__asm__(
    ".intel_syntax noprefix\n"
    ".text\n"
    /* A1: replaces `call TagTest` in MatchAmmo's volume loop. rbx = &volume slot, r13 = ctx (MatchAmmo's own registers) */
    ".globl pf_stub_a1\n"
    "pf_stub_a1:\n"
    "  push rbp\n"
    "  mov rbp, rsp\n"
    "  and rsp, -16\n"
    "  sub rsp, 0x20\n"
    "  call qword ptr [rip + g_tagtest]\n"
    "  test al, al\n"
    "  jz 1f\n"
    "  mov rcx, r13\n"
    "  mov rdx, rbx\n"
    "  call pf_vol\n"
    "1:\n"
    "  mov rsp, rbp\n"
    "  pop rbp\n"
    "  ret\n"
    /* A2: replaces `test al,2; je MATCH` (jumped to). al = row flags, r13 = ctx, rdi = row */
    ".globl pf_stub_a2\n"
    "pf_stub_a2:\n"
    "  test al, 2\n"
    "  jz 2f\n"
    "  jmp qword ptr [rip + g_a2_back]\n"
    "2:\n"
    "  push rbp\n"
    "  mov rbp, rsp\n"
    "  and rsp, -16\n"
    "  sub rsp, 0x20\n"
    "  mov rcx, r13\n"
    "  mov rdx, rdi\n"
    "  call pf_novol\n"
    "  mov rsp, rbp\n"
    "  pop rbp\n"
    "  test eax, eax\n"
    "  jz 3f\n"
    "  jmp qword ptr [rip + g_a2_match]\n"
    "3:\n"
    "  jmp qword ptr [rip + g_a2_next]\n"
    /* W: ctx builder entry. rcx = ctx, rdx = weapon or NULL, [rsp] = return address, r14 = caller's r14 */
    ".globl pf_stub_w\n"
    "pf_stub_w:\n"
    "  push rcx\n"
    "  push rdx\n"
    "  push r8\n"
    "  push r9\n"
    "  sub rsp, 0x28\n"
    "  mov r8, [rsp + 0x48]\n"
    "  mov r9, r14\n"
    "  call pf_record\n"
    "  add rsp, 0x28\n"
    "  pop r9\n"
    "  pop r8\n"
    "  pop rdx\n"
    "  pop rcx\n"
    "  jmp qword ptr [rip + g_w_tramp]\n"
    /* P: replaces `mov rdi,[end]` at the picker's try-loop head (jumped to). r14 = aim, r13 = weapon, r15 = target,
     * ebp = previous-volume index, r12b = noAimCheck; rax / rcx / rdx / r8-r11 / flags are dead there */
    ".globl pf_stub_p\n"
    "pf_stub_p:\n"
    "  push rbp\n"
    "  mov rbp, rsp\n"
    "  and rsp, -16\n"
    "  sub rsp, 0x30\n"
    "  mov rcx, r14\n"
    "  mov rdx, r13\n"
    "  mov r8, r15\n"
    "  mov r9, [rbp]\n"
    "  movzx eax, r12b\n"
    "  mov [rsp + 0x20], rax\n"
    "  call pf_pick\n"
    "  mov rsp, rbp\n"
    "  pop rbp\n"
    "  mov ebp, eax\n"
    "  mov rax, qword ptr [rip + g_ve]\n"
    "  mov rdi, qword ptr [rax]\n"
    "  jmp qword ptr [rip + g_p_back]\n"
    /* X: replaces `call Clear` on the picker's exhausted exit (called). rcx = aim, r13 = weapon, r15 = target */
    ".globl pf_stub_x\n"
    "pf_stub_x:\n"
    "  push rbp\n"
    "  mov rbp, rsp\n"
    "  and rsp, -16\n"
    "  sub rsp, 0x20\n"
    "  mov rdx, r13\n"
    "  mov r8, r15\n"
    "  call pf_exhaust\n"
    "  mov rsp, rbp\n"
    "  pop rbp\n"
    "  ret\n"
    ".att_syntax prefix\n"
);
extern void pf_stub_a1(void);
extern void pf_stub_a2(void);
extern void pf_stub_w(void);
extern void pf_stub_p(void);
extern void pf_stub_x(void);

/* ---------------------------------------------------------------- ini */
static char g_prio_str[2048];
static void load_ini(const char *ini)
{
#define GI(k, d) GetPrivateProfileIntA("PenFilter", k, d, ini)
    cfg_enabled = GI("Enabled", 1);
    cfg_log = GI("Log", 1);
    cfg_logverdicts = GI("LogVerdicts", 400);
    cfg_rowfilter = GI("HookRowFilter", 1);
    cfg_aimcheck = GI("HookAimCheck", 1);
    cfg_prio = GI("HookAimPriority", 1);
    cfg_exhaust = GI("HookExhaust", 1);
    cfg_exstop = GI("ExhaustStop", 1);
    cfg_approach = GI("HookApproach", 0);
    cfg_cachems = GI("CacheMs", 60000);
    cfg_holdms = GI("VerdictHoldMs", 8000);
    cfg_holddist = GI("HoldDistanceM", 15);
    cfg_unrms = GI("UnreachableMs", 3000);
    cfg_rule = GI("PierceRule", 1) == 3 ? 3 : 1;
    cfg_player = GI("PlayerUnits", 1);
    cfg_dcskip = GI("SkipDirectControl", 1);
    cfg_facing = GI("Facing", 1);
    cfg_oldstreak = GI("BlockStreak", -1);
#undef GI
    if (cfg_cachems < 0) cfg_cachems = 0;
    if (cfg_holdms < 0) cfg_holdms = 0;
    if (cfg_unrms < 0) cfg_unrms = 0;
    g_holdunits = cfg_holddist > 0 ? cfg_holddist * 20.0f : 0.0f;
    GetPrivateProfileStringA("PenFilter", "PierceTag", "pierce", cfg_pierce, sizeof cfg_pierce, ini);
    char buf[MAXT * TLEN];
    GetPrivateProfileStringA("PenFilter", "ExemptTags", "melee_light,melee_heavy,melee", buf, sizeof buf, ini);
    n_exempt = 0;
    for (char *t = strtok(buf, ", "); t && n_exempt < MAXT; t = strtok(NULL, ", "))
        if (strlen(t) < TLEN) strcpy(cfg_exempt[n_exempt++], t);
    GetPrivateProfileStringA("PenFilter", "AimPriority", DEF_PRIO, g_prio_str, sizeof g_prio_str, ini);
    parse_prio(g_prio_str);
}
static void prio_desc(char *out, size_t cap)
{
    out[0] = 0;
    for (int i = 0; i < n_ptag; i++) {
        size_t l = strlen(out);
        if (l + strlen(g_ptag[i]) + 3 >= cap) break;
        if (i) strcat(out, g_ptag_grp[i] != g_ptag_grp[i - 1] ? ";" : ",");
        strcat(out, g_ptag[i]);
    }
}

#define OFF(x) ((unsigned long long)((x) ? (uint8_t *)(x) - S.base : 0))
static void log_sites(void)
{
    logf_("  MatchAmmo exe+0x%llX: volume site exe+0x%llX, no-volume site exe+0x%llX (match +0x%llX, next +0x%llX), tag test exe+0x%llX, ammo tags +0x%X%s%s",
          OFF(S.ma_fn), OFF(S.a1), OFF(S.a2), OFF(S.match), OFF(S.next), OFF(S.tagtest), S.ammotags, S.err_ma ? " | ERROR " : "", S.err_ma ? S.err_ma : "");
    logf_("  ctx builder exe+0x%llX (%d MatchAmmo pairs)%s%s | enemy scan returns exe+0x%llX / exe+0x%llX, weapon-stuff cast exe+0x%llX, list +0x%X%s%s",
          OFF(S.builder), S.npairs, S.err_bld ? " ERROR " : "", S.err_bld ? S.err_bld : "",
          OFF(S.scan_ret[0]), OFF(S.scan_ret[1]), OFF(S.wcast), S.wlistoff, S.scan_err ? " | scan OFF: " : "", S.scan_err ? S.scan_err : "");
    logf_("  AimInfo chain call exe+0x%llX: params exe+0x%llX (list +0x%X), loaded ammo exe+0x%llX, {parameters} lookup exe+0x%llX, pen exe+0x%llX, "
          "angle exe+0x%llX, dynamic_cast exe+0x%llX (params exe+0x%llX->0x%llX, weapon stuff exe+0x%llX->0x%llX), weapon stuff +0x%X calibre +0x%X%s%s",
          OFF(S.aiminfo_call), OFF(S.P), S.ilistoff, OFF(S.loaded), OFF(S.plist), OFF(S.pen), OFF(S.c090), OFF(S.rtdc),
          OFF(S.pc_src), OFF(S.pc_dst), OFF(S.wc_src), OFF(S.wc_dst), S.wstuffoff, S.caliboff,
          S.err_ai ? " | ERROR " : "", S.err_ai ? S.err_ai : "");
    logf_("  armour: block exe+0x%llX, evaluate exe+0x%llX, thickness exe+0x%llX, effective exe+0x%llX, threshold exe+0x%llX, material exe+0x%llX, relation exe+0x%llX%s%s",
          OFF(S.c310), OFF(S.c3a0), OFF(S.c5b0), OFF(S.ca60), OFF(S.cde0), OFF(S.matof), OFF(S.relof), S.err_arm ? " | ERROR " : "", S.err_arm ? S.err_arm : "");
    logf_("  picker solver call exe+0x%llX -> solver exe+0x%llX%s%s | sim clock exe+0x%llX%s%s | eBox vtable exe+0x%llX%s%s",
          OFF(S.bsite), OFF(S.solver), S.err_b ? " ERROR " : "", S.err_b ? S.err_b : "", OFF(S.clk), S.err_clk ? " ERROR " : "", S.err_clk ? S.err_clk : "",
          OFF(S.box_vt), S.err_box ? " " : "", S.err_box ? S.err_box : "");
    logf_("  ePolyhedron vtable exe+0x%llX", OFF(S.poly_vt));
    logf_("  picker try-loop head exe+0x%llX (candidates exe+0x%llX..+8)%s%s | exhausted clear call exe+0x%llX -> clear exe+0x%llX%s%s | aim volume exe+0x%llX%s%s",
          OFF(S.psite), OFF(S.vbeg), S.err_p ? " ERROR " : "", S.err_p ? S.err_p : "", OFF(S.xsite), OFF(S.clr),
          S.err_x ? " ERROR " : "", S.err_x ? S.err_x : "", OFF(S.getvol), S.err_gv ? " ERROR " : "", S.err_gv ? S.err_gv : "");
    logf_("  weapon stop exe+0x%llX (aim slots +0x%X / +0x%X)%s%s | direct control test exe+0x%llX%s%s",
          OFF(S.wstop), S.ws1, S.ws2, S.err_stop ? " ERROR " : "", S.err_stop ? S.err_stop : "",
          OFF(S.ismanual), S.err_man ? " ERROR " : "", S.err_man ? S.err_man : "");
}

/* ---------------------------------------------------------------- install */
static int write_code(uint8_t *at, const uint8_t *bytes, size_t n)
{
    DWORD old;
    if (!VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &old)) return 0;
    memcpy(at, bytes, n);
    VirtualProtect(at, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, n);
    return 1;
}
static void absjmp(uint8_t *p, const void *to)
{
    p[0] = 0xFF; p[1] = 0x25; p[2] = p[3] = p[4] = p[5] = 0;
    uint64_t t = (uint64_t)(uintptr_t)to;
    memcpy(p + 6, &t, 8);
}
/* executable page within +-2 GB of `near` (for 5-byte call / jmp redirects) */
static uint8_t *alloc_near(uint8_t *site)
{
    SYSTEM_INFO si; GetSystemInfo(&si);
    uintptr_t gran = si.dwAllocationGranularity, start = (uintptr_t)site;
    for (uintptr_t d = gran; d < 0x70000000; d += gran) {
        for (int sgn = -1; sgn <= 1; sgn += 2) {
            uintptr_t a = sgn < 0 ? (start > d ? (start - d) & ~(gran - 1) : 0) : (start + d) & ~(gran - 1);
            if (!a) continue;
            MEMORY_BASIC_INFORMATION mbi;
            if (!VirtualQuery((void *)a, &mbi, sizeof mbi) || mbi.State != MEM_FREE) continue;
            void *p = VirtualAlloc((void *)a, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
            if (p) return (uint8_t *)p;
        }
    }
    return NULL;
}
static int rel_ok(uint8_t *from_end, uint8_t *to) { intptr_t d = to - from_end; return d >= INT32_MIN && d <= INT32_MAX; }
static void put_rel(uint8_t *patch, uint8_t op, uint8_t *site, uint8_t *to)
{
    patch[0] = op; int32_t d = (int32_t)(to - (site + 5)); memcpy(patch + 1, &d, 4);
}
#define TH_A1 0
#define TH_A2 16
#define TH_B  32
#define TH_P  48
#define TH_X  64

static void install_at(uint8_t *base)
{
    if (!cfg_enabled) { logf_("[GEM PenFilter %s] Enabled=0, not installed", PF_VERSION); return; }
    find_all(base, &S);
    logf_("[GEM PenFilter %s] finders:", PF_VERSION);
    log_sites();
    int core = !S.err_ai && !S.err_arm && !S.err_clk;
    if (core) {
        g_clock = (volatile uint32_t *)S.clk;
        e_loaded = (fp_p)S.loaded; e_P = (fp_p)S.P; e_wcast = (fp_p)S.wcast; e_armorof = (fp_p)S.armorof;
        e_plist = (fp_pp)S.plist; e_matof = (fp_pp)S.matof; e_pen = (fp_pen)S.pen; e_rtdc = (fp_rtdc)S.rtdc;
        e_angle = (fp_angle)S.c090; e_block = (fp_blk)S.c310; e_eval = (fp_eval)S.c3a0; e_thick = (fp_thick)S.c5b0;
        e_eff = (fp_eff)S.ca60; e_thr = (fp_thr)S.cde0; e_dvget = (fp_dvget)S.a8be80; e_mult = (fp_mult)S.a79530;
        e_rel = (fp_rel)S.relof; e_solve = (fp_solve)S.solver; g_tagtest = S.tagtest;
        e_getvol = S.err_gv ? NULL : (fp_p)S.getvol;
        e_wstop = S.err_stop ? NULL : (fp_wstop)S.wstop;
    }
    facing_on = cfg_facing && S.box_vt != NULL && S.poly_vt != NULL;
    int want = cfg_rowfilter || cfg_aimcheck || cfg_prio || cfg_exhaust;
    uint8_t *nearp = (core && want) ? alloc_near(S.a2 ? S.a2 : S.bsite ? S.bsite : S.lo) : NULL;
    if (core && want && !nearp) { logf_("  no executable page near the exe: nothing installed"); core = 0; }

    /* A: row filter */
    if (!cfg_rowfilter) logf_("  row filter (hook A) OFF by ini");
    else if (!core || S.err_ma || S.err_bld) logf_("  row filter (hook A) NOT installed: %s", S.err_ma ? S.err_ma : S.err_bld ? S.err_bld : S.err_ai ? S.err_ai : S.err_arm ? S.err_arm : S.err_clk ? S.err_clk : "core functions missing");
    else if (!rel_ok(S.a1 + 5, nearp + TH_A1) || !rel_ok(S.a2 + 5, nearp + TH_A2)) logf_("  row filter (hook A) NOT installed: thunk page out of rel32 range");
    else {
        uint8_t *tr = (uint8_t *)VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!tr) logf_("  row filter (hook A) NOT installed: VirtualAlloc failed (%lu)", GetLastError());
        else {
            /* builder trampoline: original 15 bytes + jmp back */
            memcpy(tr, S.builder, BLD_COPY);
            absjmp(tr + BLD_COPY, S.builder + BLD_COPY);
            FlushInstructionCache(GetCurrentProcess(), tr, 64);
            g_w_tramp = (uint64_t)(uintptr_t)tr;
            g_a2_back = (uint64_t)(uintptr_t)(S.a2 + 8);
            g_a2_match = (uint64_t)(uintptr_t)S.match;
            g_a2_next = (uint64_t)(uintptr_t)S.next;
            absjmp(nearp + TH_A1, (void *)pf_stub_a1);
            absjmp(nearp + TH_A2, (void *)pf_stub_a2);
            FlushInstructionCache(GetCurrentProcess(), nearp, 64);
            uint8_t pw[BLD_COPY], p1[5], p2[8];
            memset(pw, 0x90, sizeof pw); absjmp(pw, (void *)pf_stub_w);
            put_rel(p1, 0xE8, S.a1, nearp + TH_A1);
            memset(p2, 0x90, sizeof p2); put_rel(p2, 0xE9, S.a2, nearp + TH_A2);
            if (!write_code(S.builder, pw, sizeof pw) || !write_code(S.a1, p1, 5) || !write_code(S.a2, p2, 8))
                logf_("  row filter (hook A) VirtualProtect failed (%lu)", GetLastError());
            else {
                hookA_on = 1;
                logf_("  row filter (hook A) installed: builder exe+0x%llX, volume site exe+0x%llX, no-volume site exe+0x%llX%s",
                      OFF(S.builder), OFF(S.a1), OFF(S.a2), S.scan_err ? " (enemy scan calls unfiltered: no weapon known there)" : "");
            }
        }
    }
    /* B: aim check (the solver call is also redirected for P alone: P's unreachable fallback reads the solver's failures) */
    if (!cfg_aimcheck && !cfg_prio) logf_("  aim check (hook B) OFF by ini");
    else if (!core || S.err_b) logf_("  aim check (hook B) NOT installed: %s", S.err_b ? S.err_b : S.err_ai ? S.err_ai : S.err_arm ? S.err_arm : S.err_clk ? S.err_clk : "core functions missing");
    else if (!rel_ok(S.bsite + 5, nearp + TH_B)) logf_("  aim check (hook B) NOT installed: thunk page out of rel32 range");
    else {
        absjmp(nearp + TH_B, (void *)pf_solve);
        FlushInstructionCache(GetCurrentProcess(), nearp + TH_B, 16);
        uint8_t pb[5]; put_rel(pb, 0xE8, S.bsite, nearp + TH_B);
        if (!write_code(S.bsite, pb, 5)) logf_("  aim check (hook B) VirtualProtect failed (%lu)", GetLastError());
        else {
            hookB_on = cfg_aimcheck; solve_on = 1;
            if (cfg_aimcheck) logf_("  aim check (hook B) installed: picker solver call exe+0x%llX%s", OFF(S.bsite),
                                    e_getvol ? "" : " (aim-volume getter missing: only the volume hit first is cached, no unreachable fallback for P)");
            else logf_("  aim check (hook B) OFF by ini; picker solver call exe+0x%llX redirected for P's unreachable fallback only", OFF(S.bsite));
        }
    }
    /* P: picker candidate list (also opens the per-pick state X reads) */
    if (!cfg_prio && !cfg_exhaust) logf_("  aim priority (hook P) OFF by ini");
    else if (!core || S.err_b || S.err_p) logf_("  aim priority (hook P) NOT installed: %s", S.err_p ? S.err_p : S.err_b ? S.err_b : "core functions missing");
    else if (!rel_ok(S.psite + 5, nearp + TH_P)) logf_("  aim priority (hook P) NOT installed: thunk page out of rel32 range");
    else {
        g_vb = (uint8_t ***)S.vbeg; g_ve = (uint8_t ***)S.vend; g_p_back = (uint64_t)(uintptr_t)(S.psite + PICKP_LEN);
        absjmp(nearp + TH_P, (void *)pf_stub_p);
        FlushInstructionCache(GetCurrentProcess(), nearp + TH_P, 16);
        uint8_t pp[PICKP_LEN]; memset(pp, 0x90, sizeof pp); put_rel(pp, 0xE9, S.psite, nearp + TH_P);
        if (!write_code(S.psite, pp, sizeof pp)) logf_("  aim priority (hook P) VirtualProtect failed (%lu)", GetLastError());
        else {
            hookP_on = 1; prio_on = cfg_prio && solve_on;
            logf_("  aim priority (hook P) installed: picker try-loop head exe+0x%llX%s", OFF(S.psite),
                  !cfg_prio ? " (HookAimPriority=0: per-pick state for X only, candidates untouched)"
                  : !solve_on ? " (solver call not redirected: no unreachable fallback, so the candidates are left untouched)" : "");
        }
    }
    /* X: exhausted pick */
    if (!cfg_exhaust) logf_("  exhausted-pick hook (X) OFF by ini");
    else if (!hookP_on) logf_("  exhausted-pick hook (X) NOT installed: needs hook P");
    else if (S.err_x) logf_("  exhausted-pick hook (X) NOT installed: %s", S.err_x);
    else if (!rel_ok(S.xsite + 5, nearp + TH_X)) logf_("  exhausted-pick hook (X) NOT installed: thunk page out of rel32 range");
    else {
        e_clear = (fp_v)S.clr;
        absjmp(nearp + TH_X, (void *)pf_stub_x);
        FlushInstructionCache(GetCurrentProcess(), nearp + TH_X, 16);
        uint8_t px[5]; put_rel(px, 0xE8, S.xsite, nearp + TH_X);
        if (!write_code(S.xsite, px, 5)) logf_("  exhausted-pick hook (X) VirtualProtect failed (%lu)", GetLastError());
        else {
            hookX_on = 1;
            logf_("  exhausted-pick hook (X) installed: exhausted clear call exe+0x%llX%s", OFF(S.xsite),
                  !cfg_exstop ? " (ExhaustStop=0: no weapon stop)" : e_wstop ? "" : " (weapon stop not found: no weapon stop)");
            if (!hookB_on) logf_("  note: hook B is off, so no pick is ever exhausted by B and X only counts");
        }
    }
    if (!cfg_dcskip) logf_("  SkipDirectControl=0: units under direct control are filtered like the others");
    else if (S.err_man) logf_("  direct control test NOT found (%s): units under direct control are filtered like the others", S.err_man);
    else { dc_on = 1; logf_("  direct control: units under direct control are left alone by every hook (IsManualControlled exe+0x%llX)", OFF(S.ismanual)); }
    if (cfg_approach) logf_("  HookApproach=1 ignored: the approach hook (C) is not built");
    if (cfg_oldstreak != -1) logf_("  BlockStreak=%d ignored: the whole-ammo streak ban is gone in 0.1.1 (VerdictHoldMs / HookExhaust replace it)", cfg_oldstreak);
    char ex[MAXT * TLEN] = "", pr[1024];
    for (int i = 0; i < n_exempt; i++) { strcat(ex, cfg_exempt[i]); if (i + 1 < n_exempt) strcat(ex, ","); }
    prio_desc(pr, sizeof pr);
    logf_("[GEM PenFilter %s] %s | PierceTag=%s ExemptTags=%s PierceRule=%d PlayerUnits=%d SkipDirectControl=%d CacheMs=%d VerdictHoldMs=%d "
          "HoldDistanceM=%d UnreachableMs=%d ExhaustStop=%d Facing=%d%s LogVerdicts=%d | AimPriority (%d groups) %s",
          PF_VERSION, hookA_on || hookB_on || hookP_on ? "active" : "NOTHING installed", cfg_pierce, ex, cfg_rule, cfg_player, cfg_dcskip, cfg_cachems,
          cfg_holdms, cfg_holddist, cfg_unrms, cfg_exstop, cfg_facing, cfg_facing && !facing_on ? " (trace volume vtables not found: all faces head-on)" : "",
          cfg_logverdicts, n_grp, pr);
}

#ifndef PF_TEST
static DWORD WINAPI stats_thread(LPVOID a)
{
    (void)a;
    stats_t last; memset(&last, 0, sizeof last);
    for (;;) {
        Sleep(10000);
        stats_t c = ST;
        if (!memcmp(&c, &last, sizeof c)) continue;
#define D(f) (uint32_t)(c.f - last.f)
        logf_("stats [%u ms]: A ammo verdicts ALLOW %u (+%u) REJECT %u (+%u) held %u (+%u), volumes passed %u (+%u) blocked %u (+%u), "
              "rows without {volume} passed %u (+%u) blocked %u (+%u), no weapon known %u (+%u) | P picks %u (+%u), candidates cut %u (+%u), "
              "previous volume dropped %u (+%u), unreachable skipped %u (+%u), no reachable pierceable candidate %u (+%u) | "
              "B pierced %u (+%u) blocked %u (+%u), aimed volume cached %u (+%u), solver failures cached %u (+%u) | "
              "X picks exhausted %ld (+%u), by B %u (+%u), weapons stopped %ld (+%u), kept %u (+%u), picker clear exits %ld (+%u) | "
              "direct control skips %u (+%u) | new battles %u | log repeats %u, lines over budget %u",
              now_ms(), c.a_allow, D(a_allow), c.a_reject, D(a_reject), c.a_held, D(a_held), c.a_vpass, D(a_vpass), c.a_vblock, D(a_vblock),
              c.a_rpass, D(a_rpass), c.a_rblock, D(a_rblock), c.a_noweap, D(a_noweap), c.p_picks, D(p_picks), c.p_removed, D(p_removed),
              c.p_curdrop, D(p_curdrop), c.p_unr, D(p_unr), c.p_none, D(p_none), c.b_pierce, D(b_pierce), c.b_block, D(b_block),
              c.b_aimed, D(b_aimed), c.s_unr, D(s_unr), c.x_exh, D(x_exh), c.x_bc, D(x_bc), c.x_stop, D(x_stop), c.x_kept, D(x_kept),
              c.x_clear, D(x_clear), c.dc_skip, D(dc_skip), c.flushes, c.log_rep, c.log_supp);
#undef D
        last = c;
    }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID res)
{
    (void)res;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        char path[MAX_PATH], ini[MAX_PATH];
        DWORD n = GetModuleFileNameA(inst, path, MAX_PATH);
        if (n == 0 || n >= MAX_PATH - 16) return TRUE;
        char *dot = strrchr(path, '.');
        if (dot) *dot = 0;
        char exe[MAX_PATH], *nm;
        GetModuleFileNameA(NULL, exe, MAX_PATH);
        nm = strrchr(exe, '\\'); nm = nm ? nm + 1 : exe;
        snprintf(ini, sizeof ini, "%s.ini", path);
        snprintf(g_logpath, sizeof g_logpath, "%s%s.log", path,
                 strstr(nm, "_ed") ? "_editor" : (strstr(nm, "_server") ? "_server" : ""));
        load_ini(ini);
        install_at((uint8_t *)GetModuleHandleW(NULL));
        if (cfg_log && (hookA_on || hookB_on || hookP_on)) CreateThread(NULL, 0, stats_thread, NULL, 0, NULL);
    }
    return TRUE;
}
#else
/* ---------------------------------------------------------------- offline self-test */
/* build.bat test -> out\pf_test.exe <exe>...: every finder on each exe image mapped from disk, a patch run on that image,
 * the asm stubs called the way the engine calls them, and P / X / caches / log / priority on a faked engine */
static void fake_str(uint8_t *p, const char *s)
{
    size_t n = strlen(s);
    memset(p, 0, 0x20);
    if (n <= 15) { memcpy(p, s, n); *(uint64_t *)(p + 0x18) = 15; }
    else { char *h = _strdup(s); *(char **)p = h; *(uint64_t *)(p + 0x18) = n; }
    *(uint64_t *)(p + 0x10) = n;
}
/* a tag vector {begin,end} at v holding the given names */
static void fake_tags(uint8_t *v, int n, const char **names)
{
    uint8_t *t = (uint8_t *)calloc((size_t)(n ? n : 1), 0x60);
    for (int i = 0; i < n; i++) fake_str(t + 0x60 * i + 0x40, names[i]);
    *(uint8_t **)v = t; *(uint8_t **)(v + 8) = t + 0x60 * n;
}
__attribute__((used)) uint64_t t_p_rdi;
/* stub harnesses: call the asm stubs the way MatchAmmo / the ctx builder callers / the picker do (registers, stack alignment) */
__asm__(
    ".intel_syntax noprefix\n"
    ".text\n"
    ".globl t_call_a2\n"
    "t_call_a2:\n"                     /* int t_call_a2(int al, void *ctx): 1 back (volume path), 2 match, 3 next */
    "  push r13\n  push rdi\n  sub rsp, 8\n"
    "  mov r13, rdx\n  xor edi, edi\n  mov eax, ecx\n"
    "  jmp pf_stub_a2\n"
    ".globl ta2_back\nta2_back:\n  mov eax, 1\n  jmp 9f\n"
    ".globl ta2_match\nta2_match:\n  mov eax, 2\n  jmp 9f\n"
    ".globl ta2_next\nta2_next:\n  mov eax, 3\n"
    "9:\n  add rsp, 8\n  pop rdi\n  pop r13\n  ret\n"
    ".globl t_call_a1\n"
    "t_call_a1:\n"                     /* int t_call_a1(void *ctx, void *slot) */
    "  push r13\n  push rbx\n  sub rsp, 8\n"
    "  mov r13, rcx\n  mov rbx, rdx\n  xor ecx, ecx\n  xor edx, edx\n"
    "  call pf_stub_a1\n"
    "  movzx eax, al\n"
    "  add rsp, 8\n  pop rbx\n  pop r13\n  ret\n"
    ".globl t_call_w\n"
    "t_call_w:\n"                      /* void *t_call_w(void *ctx, void *weapon, void *r14) */
    "  push r14\n  sub rsp, 0x30\n"
    "  mov r14, r8\n"
    "  call pf_stub_w\n"
    ".globl tw_ret\ntw_ret:\n"
    "  add rsp, 0x30\n  pop r14\n  ret\n"
    ".globl t_call_p\n"
    "t_call_p:\n"                      /* int t_call_p(aim, weapon, target, int cur, int noaim): returns the new ebp, t_p_rdi = rdi */
    "  push rbp\n  push rdi\n  push r12\n  push r13\n  push r14\n  push r15\n  sub rsp, 8\n"
    "  mov r12, [rsp + 0x60]\n"
    "  mov r14, rcx\n  mov r13, rdx\n  mov r15, r8\n  mov ebp, r9d\n  xor edi, edi\n"
    "  jmp pf_stub_p\n"
    ".globl tp_back\ntp_back:\n"
    "  mov eax, ebp\n"
    "  mov qword ptr [rip + t_p_rdi], rdi\n"
    "  add rsp, 8\n  pop r15\n  pop r14\n  pop r13\n  pop r12\n  pop rdi\n  pop rbp\n  ret\n"
    ".globl t_call_x\n"
    "t_call_x:\n"                      /* void t_call_x(aim, weapon, target) */
    "  push r13\n  push r15\n  sub rsp, 0x28\n"
    "  mov r13, rdx\n  mov r15, r8\n"
    "  call pf_stub_x\n"
    "  add rsp, 0x28\n  pop r15\n  pop r13\n  ret\n"
    ".att_syntax prefix\n"
);
extern int t_call_a2(int al, void *ctx);
extern int t_call_a1(void *ctx, void *slot);
extern void *t_call_w(void *ctx, void *weapon, void *r14);
extern int t_call_p(void *aim, void *weapon, void *target, int cur, int noaim);
extern void t_call_x(void *aim, void *weapon, void *target);
extern void ta2_back(void), ta2_match(void), ta2_next(void), tw_ret(void), tp_back(void);
static int g_tagret;
static int fake_tag(void *a, void *b) { (void)a; (void)b; return g_tagret; }
static void *fake_builder(void *ctx, void *w, void *a, void *b) { (void)w; (void)a; (void)b; return ctx; }
static int stub_test(void)
{
    static uint8_t ctx[0x40];
    hookA_on = 1;
    g_a2_back = (uint64_t)(uintptr_t)ta2_back; g_a2_match = (uint64_t)(uintptr_t)ta2_match; g_a2_next = (uint64_t)(uintptr_t)ta2_next;
    g_tagtest = (void *)fake_tag; g_w_tramp = (uint64_t)(uintptr_t)fake_builder;
    int r1 = t_call_a2(2, ctx), r2 = t_call_a2(0, ctx);
    g_tagret = 1; int r3 = t_call_a1(ctx, NULL);
    g_tagret = 0; int r4 = t_call_a1(ctx, NULL);
    void *r5 = t_call_w(ctx, (void *)0x1234, (void *)0x5678);
    int ok = r1 == 1 && r2 == 2 && r3 == 1 && r4 == 0 && r5 == ctx && t_rec.ctx == ctx && t_rec.weapon == (void *)0x1234
             && t_rec.r14 == (void *)0x5678 && t_rec.ret == (void *)tw_ret;
    printf("stubs A/W: A2 volume-row %d (1), A2 no-volume row no target %d (2 = match), A1 tag hit %d (1), A1 tag miss %d (0), "
           "builder record ctx %d weapon %d r14 %d ret %d -> %s\n", r1, r2, r3, r4, t_rec.ctx == ctx, t_rec.weapon == (void *)0x1234,
           t_rec.r14 == (void *)0x5678, t_rec.ret == (void *)tw_ret, ok ? "OK" : "FAIL");
    hookA_on = 0;
    return ok;
}

/* ---- faked engine for P / B / caches / X / A: a vehicle with 4 volumes (vol0 body, vol1 turret, vol2 tracks, vol3 body), a weapon
 * with pierce ammo at pen fk_penv; the armour fakes read a per-volume threshold at volume+0x80 */
static uint32_t fk_clock = 1000;
static float fk_penv = 100.0f;
static int fk_solve_st = 3; static uint8_t *fk_hit_ent, *fk_hit_vol;
static uint8_t *fk_ammo, *fk_params, *fk_wsd, *fk_armour, fk_mat[0x200];
static void *fk_cleared; static void *fk_stop_w; static int fk_stop_arg, fk_stops;
static void *fk_loaded(void *s) { (void)s; return fk_ammo; }
static void *fk_P(void *w) { (void)w; return fk_params; }
static void *fk_plist(void *l, void *a) { (void)l; (void)a; return fk_params; }
static void *fk_rtdc(void *p, long a, void *b, void *c, int d) { (void)a; (void)b; (void)c; (void)d; return p; }
static uint8_t fk_rel(void *a, void *b) { (void)a; (void)b; return 1; }
static float fk_pen(void *p, uint32_t d) { (void)p; (void)d; return fk_penv; }
static float fk_dvget(void *dv, int u) { (void)u; return *(float *)((int32_t *)dv + 1); }
static void *fk_armorof(void *t) { (void)t; return fk_armour; }
static void fk_mult(int i, void *t, float *o) { (void)i; (void)t; *o = 1.0f; }
static void *fk_matof(void *a, void *r) { (void)a; (void)r; return fk_mat; }
static void fk_angle(void *bs, void *p, float c) { (void)bs; (void)p; (void)c; }
static float fk_thick(void *b, int m) { (void)b; (void)m; return 0.0f; }
static float fk_eff(void *b, float t) { (void)b; (void)t; return 0.0f; }
static float fk_thr(void *b) { return *(float *)((uint8_t *)((rec_t *)((blk_t *)b)->rec)->vol + 0x80); }   /* per-volume threshold */
static void fk_clear(void *aim) { fk_cleared = aim; *(int32_t *)((uint8_t *)aim + 8) = 0; }
static void fk_wstop(void *w, int a) { fk_stop_w = w; fk_stop_arg = a; fk_stops++; }
static void *fk_getvol(void *rec) { return *(void **)((uint8_t *)rec + 0x90); }               /* the volume an aim record names */
static int fk_solve(void *f, void *aim, void *a3, void *a4, void *out, void *a6)
{
    (void)f; (void)aim; (void)a3; (void)a4; (void)a6;
    if (out && fk_solve_st == 3) {
        uint8_t *r = (uint8_t *)out; memset(r, 0, 0x48);
        *(uint8_t **)(r + 0x18) = fk_hit_ent; *(uint8_t **)(r + 0x20) = fk_hit_vol;
        ((float *)(r + 0x28))[2] = 1.0f; ((float *)(r + 0x34))[2] = -1.0f;
    }
    return fk_solve_st;
}
static void *fk_block(void *b, void *ent, void *sh, void *R, void *bs)
{
    (void)ent; (void)sh;
    blk_t *k = (blk_t *)b; k->armour = fk_armour; k->rec = R; k->mat = fk_mat; k->bs = (bstate_t *)bs; k->mult = 1.0f; k->enemy = 1;
    return b;
}
static void fk_eval(void *b)   /* flag 1 when the pen in the bullet state reaches the threshold of the volume hit first */
{
    blk_t *k = (blk_t *)b;
    if (k->bs->dv_val >= *(float *)((uint8_t *)((rec_t *)k->rec)->vol + 0x80)) k->bs->flags |= 1;
}
static uint8_t *fk_vol[4], *fk_cand[8], **fk_vec[2];
static void fk_thr4(float a, float b, float c, float d)
{
    float t[4] = { a, b, c, d };
    for (int i = 0; i < 4; i++) *(float *)(fk_vol[i] + 0x80) = t[i];
    memset(m_thr, 0, sizeof m_thr); memset(m_mat, 0, sizeof m_mat);
}
static void fk_reset_cands(void) { for (int i = 0; i < 4; i++) fk_cand[i] = fk_vol[i]; fk_vec[0] = fk_cand; fk_vec[1] = fk_cand + 4; }
static int fk_idx(const uint8_t *v) { for (int i = 0; i < 4; i++) if (fk_vol[i] == v) return i; return -1; }
static void fk_rotz(uint8_t *vol, float deg)    /* world->local matrix of a box volume turned `deg` about Z, centre at the origin */
{
    float r = deg * 3.14159265f / 180.0f, c = cosf(r), s = sinf(r), *m = (float *)(vol + 0x58 + 0x30);
    memset(m, 0, 12 * sizeof(float));
    m[0] = c; m[3] = s; m[1] = -s; m[4] = c; m[8] = 1.0f;
}

static int logic_test(void)
{
    int ok = 1;
    sites_t keep = S;
    memset(&S, 0, sizeof S);
    S.ammotags = 0xEC8; S.wstuffoff = 0x98; S.caliboff = 0x1628; S.ws1 = 0xC8; S.ws2 = 0x168; S.ilistoff = 0xA0;
    g_clock = &fk_clock; facing_on = 0; cfg_rule = 1; cfg_player = 1; cfg_cachems = 60000; cfg_holdms = 8000; cfg_unrms = 3000;
    g_holdunits = 300.0f; cfg_log = 0; cfg_exstop = 1; prio_on = 1; hookB_on = 1; dc_on = 1; g_last_now = 0;
    e_loaded = fk_loaded; e_P = fk_P; e_rtdc = fk_rtdc; e_rel = fk_rel; e_pen = fk_pen; e_dvget = fk_dvget; e_armorof = fk_armorof;
    e_mult = fk_mult; e_matof = (fp_pp)fk_matof; e_angle = fk_angle; e_thick = fk_thick; e_eff = fk_eff; e_thr = fk_thr;
    e_clear = fk_clear; e_wstop = fk_wstop; e_getvol = fk_getvol; e_solve = fk_solve; e_block = fk_block; e_eval = fk_eval;
    e_plist = fk_plist;

    /* priority parser + default */
    parse_prio("body; turret ,mantlet;;mover");
    int pp = n_grp == 3 && n_ptag == 4 && g_ptag_grp[0] == 0 && g_ptag_grp[1] == 1 && g_ptag_grp[2] == 1 && g_ptag_grp[3] == 2
             && !strcmp(g_ptag[2], "mantlet");
    parse_prio(DEF_PRIO);
    int pd = n_grp == 6 && n_ptag == 33 && g_ptag_grp[0] == 0 && g_ptag_grp[3] == 1 && g_ptag_grp[5] == 2 && g_ptag_grp[14] == 3 && g_ptag_grp[20] == 4
             && g_ptag_grp[32] == 5 && !strcmp(g_ptag[32], "wheel");
    printf("AimPriority parser: custom %s, default %d groups %d tags -> %s\n", pp ? "OK" : "FAIL", n_grp, n_ptag, pp && pd ? "OK" : "FAIL");
    ok &= pp && pd;

    /* the fake world */
    fk_ammo = (uint8_t *)calloc(1, 0x2000); fk_params = (uint8_t *)calloc(1, 0x400); fk_wsd = (uint8_t *)calloc(1, 0x2000);
    uint8_t *fk_wsd2 = (uint8_t *)calloc(1, 0x2000);
    fk_armour = (uint8_t *)calloc(1, 0x40); *(float *)fk_armour = 1.0f;
    *(float *)(fk_wsd + 0x1628) = 10.0f; *(float *)(fk_wsd2 + 0x1628) = 10.0f;
    const char *ap[] = { "pierce", "shell" }, *an[] = { "bulletig_75", "ap" }, *wn[] = { "ig_tank_cannon" }, *wn2[] = { "ig_hull_cannon" };
    fake_tags(fk_ammo + 0xEC8 + 0x318, 2, ap); fake_tags(fk_ammo + 0x868 + 0x318, 2, an);
    fake_tags(fk_wsd + 0x868 + 0x318, 1, wn); fake_tags(fk_wsd2 + 0x868 + 0x318, 1, wn2);
    uint8_t *target = (uint8_t *)calloc(1, 0x1000), *shooter = (uint8_t *)calloc(1, 0x1000), *weapon = (uint8_t *)calloc(1, 0x400);
    uint8_t *weapon2 = (uint8_t *)calloc(1, 0x400), *aim = (uint8_t *)calloc(1, 0xA0), *firer = (uint8_t *)calloc(1, 0x40);
    uint8_t *dcprop = (uint8_t *)calloc(1, 0x40), *wl = (uint8_t *)calloc(1, 0x40);
    target[0x30] = 0x02; shooter[0x30] = 0x02; *(uint32_t *)(target + 0xC0) = 0xC00A; *(uint32_t *)(shooter + 0xC0) = 0x8000;
    ((float *)(shooter + 0x74))[0] = 2000.0f;                    /* 100 m */
    *(uint8_t **)(weapon + 0x90) = shooter; *(void **)(weapon + 0xB0) = (void *)1; *(uint8_t **)(weapon + 0x98) = fk_wsd; *(uint8_t **)(weapon + 0xA0) = wl;
    *(uint8_t **)(weapon2 + 0x90) = shooter; *(void **)(weapon2 + 0xB0) = (void *)1; *(uint8_t **)(weapon2 + 0x98) = fk_wsd2; *(uint8_t **)(weapon2 + 0xA0) = wl;
    *(uint8_t **)(firer + 0x10) = weapon; *(uint8_t **)(aim + 0x18) = target;
    const char *tb[] = { "body" }, *tt[] = { "turret" }, *tm[] = { "mover", "track" };
    const char **tg[4] = { tb, tt, tm, tb }; int tn[4] = { 1, 1, 2, 1 };
    uint8_t *list[4];
    for (int i = 0; i < 4; i++) {
        fk_vol[i] = (uint8_t *)calloc(1, 0x100);
        uint8_t *info = (uint8_t *)calloc(1, 0x400);
        fake_tags(info + 0x398, tn[i], tg[i]);
        *(uint8_t **)(fk_vol[i] + 0x30) = info;
        list[i] = fk_vol[i];
    }
    uint8_t **vol = fk_vol;
    *(uint8_t ***)(target + 0x510) = list; *(uint8_t ***)(target + 0x518) = list + 4;
    g_vb = &fk_vec[0]; g_ve = &fk_vec[1]; g_p_back = (uint64_t)(uintptr_t)tp_back;
    uint32_t ka = ammo_info(fk_ammo)->key, k = wa_key(ka, fk_wsd), k2 = wa_key(ka, fk_wsd2);
    float eye[3] = { 2000.0f, 0.0f, 30.0f };
    printf("ammo + weapon key: ammo %08X, with weapon 1 %08X, with weapon 2 %08X -> %s\n", ka, k, k2, k != ka && k2 != ka && k != k2 ? "OK" : "FAIL");
    ok &= k != ka && k2 != ka && k != k2;

    /* P: group cut */
    memset(c_blk, 0, sizeof c_blk); memset(c_unr, 0, sizeof c_unr); memset(c_hold, 0, sizeof c_hold);
    float thr[4][4] = { { 200, 50, 10, 80 },  { 200, 50, 10, 300 }, { 200, 50, 10, 300 }, { 999, 999, 999, 999 } };
    int want_cur[5] = { -1, 0, -1, 1, 1 }, want_n[5] = { 1, 1, 1, 4, 4 }; uint8_t *want_v0[5];
    want_v0[0] = vol[3]; want_v0[1] = vol[1]; want_v0[2] = vol[2]; want_v0[3] = vol[0]; want_v0[4] = vol[0];
    const char *cname[5] = { "body pierceable (current turret dropped)", "body not, turret kept as current", "turret blocked by B -> tracks",
                             "nothing pierceable -> untouched", "infantry target -> untouched" };
    for (int c = 0; c < 5; c++) {
        int t = c < 4 ? c : 3;
        fk_thr4(thr[t][0], thr[t][1], thr[t][2], thr[t][3]);
        fk_reset_cands();
        if (c == 2) blk_put(0x8000, 0xC00A, k, 1, fk_clock, 2000.0f, vol[1], target, eye);
        target[0x30] = c == 4 ? 0x06 : 0x02;
        t_p_rdi = 0;
        int r = t_call_p(aim, weapon, target, 1, 0);
        int n = (int)(fk_vec[1] - fk_vec[0]);
        int good = r == want_cur[c] && n == want_n[c] && fk_cand[0] == want_v0[c] && t_p_rdi == (uint64_t)(uintptr_t)fk_vec[1]
                   && t_pick.open && t_pick.target == target && t_pick.maxtries == (n < 5 ? n : 5) && t_pick.cut == (c < 3);
        printf("P stub + filter case %d (%s): ebp %d (%d), kept %d (%d), first vol%d, rdi = new end %d, cut %d -> %s\n", c, cname[c], r, want_cur[c],
               n, want_n[c], fk_idx(fk_cand[0]), t_p_rdi == (uint64_t)(uintptr_t)fk_vec[1], t_pick.cut, good ? "OK" : "FAIL");
        ok &= good;
    }
    target[0x30] = 0x02;

    /* P fallback: a pierceable body the solver cannot reach -> next pick goes to the turret; after UnreachableMs back to the body */
    {
        memset(c_blk, 0, sizeof c_blk); memset(c_unr, 0, sizeof c_unr);
        fk_thr4(50, 50, 10, 999);
        fk_reset_cands(); int r0 = t_call_p(aim, weapon, target, -1, 0); uint8_t *v0 = fk_cand[0]; int n0 = (int)(fk_vec[1] - fk_vec[0]);
        *(uint8_t **)(aim + 0x90) = vol[0]; fk_solve_st = 1;
        uint32_t s0 = ST.s_unr, pu0 = ST.p_unr;
        int st = pf_solve(firer, aim, NULL, NULL, NULL, NULL);
        int rec = ST.s_unr == s0 + 1 && unr_hit(0x8000, 0xC00A, k, 0, fk_clock);
        fk_reset_cands(); t_call_p(aim, weapon, target, -1, 0); uint8_t *v1 = fk_cand[0]; int n1 = (int)(fk_vec[1] - fk_vec[0]);
        int skipped = ST.p_unr == pu0 + 1;
        fk_clock += 2999; fk_reset_cands(); t_call_p(aim, weapon, target, -1, 0); uint8_t *v2 = fk_cand[0];
        fk_clock += 1; fk_reset_cands(); t_call_p(aim, weapon, target, -1, 0); uint8_t *v3 = fk_cand[0];
        /* no cut (nothing pierceable): solver failures are not recorded */
        fk_thr4(999, 999, 999, 999); fk_reset_cands(); t_call_p(aim, weapon, target, -1, 0);
        uint32_t s1 = ST.s_unr; pf_solve(firer, aim, NULL, NULL, NULL, NULL); int norec = ST.s_unr == s1;
        fk_solve_st = 3;
        int good = r0 == -1 && n0 == 1 && v0 == vol[0] && st == 1 && rec && n1 == 1 && v1 == vol[1] && skipped && v2 == vol[1] && v3 == vol[0] && norec;
        printf("P unreachable fallback: first body vol%d, solver status %d recorded %d, next pick vol%d (skipped %d), at 2.999 s vol%d, at 3 s vol%d, "
               "uncut list not recorded %d -> %s\n", fk_idx(v0), st, rec, fk_idx(v1), skipped, fk_idx(v2), fk_idx(v3), norec, good ? "OK" : "FAIL");
        ok &= good;
    }

    /* B: the round hits a plate (vol3) first while aimed at the body (vol0): status 5, both cached, next pick goes to the turret */
    {
        memset(c_blk, 0, sizeof c_blk); memset(c_unr, 0, sizeof c_unr);
        fk_thr4(50, 50, 10, 999);
        fk_reset_cands(); t_call_p(aim, weapon, target, -1, 0); uint8_t *v0 = fk_cand[0];
        fk_hit_ent = target; fk_hit_vol = vol[3]; *(uint8_t **)(aim + 0x90) = vol[0]; fk_solve_st = 3;
        uint32_t ba = ST.b_aimed, bb = ST.b_block;
        int st = pf_solve(firer, aim, NULL, NULL, NULL, NULL);
        int c3 = blk_hit(0x8000, 0xC00A, k, 3, fk_clock, 2000.0f, vol[3], target, eye), c0 = blk_hit(0x8000, 0xC00A, k, 0, fk_clock, 2000.0f, vol[0], target, eye);
        int other = !blk_hit(0x8000, 0xC00A, k2, 0, fk_clock, 2000.0f, vol[0], target, eye);   /* another weapon of the same shooter: not blocked */
        int cnt = ST.b_aimed == ba + 1 && ST.b_block == bb + 1 && t_pick.bblocks == 1;
        fk_reset_cands(); t_call_p(aim, weapon, target, -1, 0); uint8_t *v1 = fk_cand[0];
        fk_hit_vol = vol[1]; *(uint8_t **)(aim + 0x90) = vol[1];
        int st2 = pf_solve(firer, aim, NULL, NULL, NULL, NULL);
        int good = v0 == vol[0] && st == 5 && c3 && c0 && other && cnt && v1 == vol[1] && st2 == 3;
        printf("B aim check: aimed body vol%d hits plate vol3 -> status %d, plate cached %d, aimed body cached %d, other weapon unaffected %d, "
               "counters %d; next pick vol%d hits it -> status %d -> %s\n", fk_idx(v0), st, c3, c0, other, cnt, fk_idx(v1), st2, good ? "OK" : "FAIL");
        ok &= good;
    }

    /* blocked cache gates: CacheMs safety limit, distance one way, direction (world fallback and the volume's own frame) */
    {
        memset(c_blk, 0, sizeof c_blk);
        blk_put(0x8000, 0xC00A, k, 1, 1000, 2000.0f, vol[1], target, eye);
        float e30[3] = { 2000.0f * 0.8660254f, 1000.0f, 30.0f }, e10[3] = { 2000.0f * 0.9848078f, 2000.0f * 0.1736482f, 30.0f };
        int g1 = blk_hit(0x8000, 0xC00A, k, 1, 1000 + 59999, 2100.0f, vol[1], target, eye);
        int g2 = !blk_hit(0x8000, 0xC00A, k, 1, 1000 + 60000, 2000.0f, vol[1], target, eye);
        int g3 = blk_hit(0x8000, 0xC00A, k, 1, 1010, 2400.0f, vol[1], target, eye);
        int g4 = !blk_hit(0x8000, 0xC00A, k, 1, 1010, 1600.0f, vol[1], target, eye);
        int g5 = !blk_hit(0x8000, 0xC00A, k, 2, 1000, 2000.0f, vol[2], target, eye);
        int g6 = !blk_hit(0x8000, 0xC00A, k, 1, 1010, 2000.0f, vol[1], target, e30);
        int g7 = blk_hit(0x8000, 0xC00A, k, 1, 1010, 2000.0f, vol[1], target, e10);
        static uint64_t fvt[2], tvb[2]; tvb[0] = (uint64_t)(uintptr_t)fvt; S.box_vt = (uint8_t *)fvt;
        uint8_t *vd = (uint8_t *)calloc(1, 0x100); *(uint8_t **)(vd + 0x50) = (uint8_t *)tvb; fk_rotz(vd, 0);
        blk_put(0x8000, 0xC00A, k, 7, 1000, 2000.0f, vd, target, eye);
        int f0 = 0; { blk_cache_t *b = blk_slot(0x8000, 0xC00A, k, 7); f0 = b->fr == 1; }
        fk_rotz(vd, 10); int g8 = blk_hit(0x8000, 0xC00A, k, 7, 1010, 2000.0f, vd, target, eye);
        fk_rotz(vd, 30); int g9 = !blk_hit(0x8000, 0xC00A, k, 7, 1010, 2000.0f, vd, target, eye);
        S.box_vt = NULL;
        int good = g1 && g2 && g3 && g4 && g5 && g6 && g7 && f0 && g8 && g9;
        printf("blocked-volume cache: kept 59.999 s / 5 m farther %d, safety limit 60 s %d, 20 m farther kept %d, 20 m nearer dropped %d, other volume %d, "
               "shooter 30 deg round dropped %d, 10 deg kept %d; volume frame used %d, target turned 10 deg kept %d, 30 deg dropped %d -> %s\n",
               g1, g2, g3, g4, g5, g6, g7, f0, g8, g9, good ? "OK" : "FAIL");
        ok &= good;
    }

    /* verdict hold: time, one-way distance, penetration-range state */
    {
        int v = -1;
        memset(c_hold, 0, sizeof c_hold);
        hold_put(1, 2, 3, 5000, 1000.0f, 1, 1);
        int h1 = hold_get(1, 2, 3, 12999, 1200.0f, 1, &v, NULL) && v == 1;
        int h2 = !hold_get(1, 2, 3, 13000, 1000.0f, 1, &v, NULL);
        int h3 = !hold_get(1, 2, 3, 6000, 1301.0f, 1, &v, NULL);
        int h4 = hold_get(1, 2, 3, 6000, 600.0f, 1, &v, NULL) && v == 1;
        int h5 = !hold_get(1, 2, 3, 6000, 1000.0f, 0, &v, NULL);
        hold_put(1, 2, 3, 6000, 1000.0f, 1, 0);
        int h6 = hold_get(1, 2, 3, 7000, 1000.0f, 1, &v, NULL) && v == 0;
        int h7 = hold_get(1, 2, 3, 7000, 1400.0f, 1, &v, NULL) && v == 0;
        int h8 = !hold_get(1, 2, 3, 7000, 600.0f, 1, &v, NULL);
        int good = h1 && h2 && h3 && h4 && h5 && h6 && h7 && h8;
        printf("verdict hold: ALLOW kept 7.999 s / 10 m farther %d, expired at 8 s %d, dropped 15.05 m farther %d, kept 20 m nearer %d, "
               "dropped out of range %d; REJECT overwrites %d, kept 20 m farther %d, dropped 20 m nearer %d -> %s\n",
               h1, h2, h3, h4, h5, h6, h7, h8, good ? "OK" : "FAIL");
        ok &= good;
    }

    /* change-only log state */
    {
        int32_t pv; uint32_t pr; cfg_log = 1; cfg_logverdicts = 1000; g_vlines = 0; memset(g_vl, 0, sizeof g_vl);
        FILE *keepf = g_log; g_logpath[0] = 0; g_log = NULL;
        int l1 = vl_change(1, 7, 8, 9, 0, &pv, &pr), l2 = vl_change(1, 7, 8, 9, 0, &pv, &pr), l3 = vl_change(1, 7, 8, 9, 0, &pv, &pr);
        int l4 = vl_change(1, 7, 8, 9, 1, &pv, &pr);
        int32_t pv5; uint32_t pr5; int l5 = vl_change(0, 7, 8, 9, 1, &pv5, &pr5), l5new = pv5 == V_NONE;
        cfg_logverdicts = 3; int l6 = vl_change(1, 7, 8, 9, 0, &pv5, &pr5);
        int lg = l1 == 1 && l2 == 0 && l3 == 0 && l4 == 1 && pv == 0 && pr == 3 && l5 == 1 && l5new && l6 == 0 && g_vlines == 3;
        printf("change-only log: first %d, repeats %d %d, change %d (was %s x%u), other hook %d, change over budget %d, lines %ld -> %s\n",
               l1, l2, l3, l4, nm_pierce(pv), pr, l5, l6, (long)g_vlines, lg ? "OK" : "FAIL");
        ok &= lg; cfg_log = 0; g_log = keepf;
    }

    /* X: exhausted pick */
    {
        uint8_t *slot1 = weapon + 0xC8;
        struct { const char *name; int tries, blocks, slotent, mixed, slotvol, dc; int want_stop, want_hold, want_full; } xc[8] = {
            { "B-caused, aimed at the target, nothing pierceable",          1, 1, 1, 0,  2, 0, 1,  0, 1 },
            { "no B block (other failures)",                                1, 0, 1, 0,  2, 0, 0, -1, 1 },
            { "B-caused, weapon aimed elsewhere",                           1, 1, 0, 0,  2, 0, 0,  0, 1 },
            { "out-of-range exit before the tries ran out",                 0, 1, 1, 0,  2, 0, 0, -1, 0 },
            { "B-caused, aimed at the pierceable tracks (lower group)",     1, 1, 1, 1,  2, 0, 0,  1, 1 },
            { "B-caused, aimed at the B-blocked turret, tracks pierceable", 1, 1, 1, 1,  1, 0, 1,  1, 1 },
            { "B-caused, aim volume unknown, tracks pierceable",            1, 1, 1, 1, -1, 0, 0,  1, 1 },
            { "B-caused, shooter taken under direct control",               1, 1, 1, 0,  2, 1, 0, -1, 1 } };
        for (int c = 0; c < 8; c++) {
            memset(c_hold, 0, sizeof c_hold); memset(c_blk, 0, sizeof c_blk);
            if (xc[c].mixed) fk_thr4(200, 50, 10, 300); else fk_thr4(999, 999, 999, 999);
            blk_put(0x8000, 0xC00A, k, 1, fk_clock, 2000.0f, vol[1], target, eye);                /* the turret: B-blocked */
            t_pick.open = 1; t_pick.target = target; t_pick.weapon = weapon; t_pick.maxtries = 1; t_pick.tries = xc[c].tries;
            t_pick.bblocks = xc[c].blocks; t_pick.haskey = 1; t_pick.cut = 1; t_pick.sid = 0x8000; t_pick.tid = 0xC00A; t_pick.key = k; t_pick.gdist = 2000.0f;
            *(int32_t *)(slot1 + 8) = 1; *(uint8_t **)(slot1 + 0x18) = xc[c].slotent ? target : shooter;
            *(uint8_t **)(slot1 + 0x90) = xc[c].slotvol >= 0 ? vol[xc[c].slotvol] : NULL;
            *(int32_t *)(dcprop + 0x18) = xc[c].dc; *(uint8_t **)(shooter + 0xDE0) = dcprop;
            *(int32_t *)(aim + 8) = 1; fk_cleared = NULL; fk_stops = 0; fk_stop_w = NULL; fk_stop_arg = 0;
            LONG cl0 = ST.x_clear, ex0 = ST.x_exh;
            t_call_x(aim, weapon, target);
            int hv = -1; if (!hold_get(0x8000, 0xC00A, k, fk_clock, 2000.0f, 1, &hv, NULL)) hv = -1;
            int good = fk_cleared == aim && *(int32_t *)(aim + 8) == 0 && ST.x_clear == cl0 + 1 && ST.x_exh == ex0 + xc[c].want_full && !t_pick.open
                       && fk_stops == xc[c].want_stop && (!xc[c].want_stop || (fk_stop_w == weapon && fk_stop_arg == 1)) && hv == xc[c].want_hold;
            printf("X stub case %d (%s): engine clear %d, counted as exhausted %d (%d), weapon stop %d (%d), held verdict %d (%d) -> %s\n", c, xc[c].name,
                   fk_cleared == aim, (int)(ST.x_exh - ex0), xc[c].want_full, fk_stops, xc[c].want_stop, hv, xc[c].want_hold, good ? "OK" : "FAIL");
            ok &= good;
        }
        *(int32_t *)(dcprop + 0x18) = 0; *(int32_t *)(slot1 + 8) = 0;
    }

    /* A through the MatchAmmo stubs: verdict, hold, expiry (fresh clock values so the per-tick prep memo never answers) */
    {
        static uint8_t actx[0x40], ti[0x80];
        *(uint8_t **)actx = fk_ammo; *(uint8_t **)(actx + 8) = shooter; *(uint8_t **)(actx + 0x10) = ti;
        *(uint8_t **)(ti + 0x18) = target;
        hookA_on = 1; g_tagret = 1; memset(c_hold, 0, sizeof c_hold); memset(c_blk, 0, sizeof c_blk);
        int res[5], a1v[5];
        float th[5] = { 10, 999, 999, 999, 10 };            /* vol2 (tracks) threshold per step */
        uint32_t clk[5] = { 20000, 20001, 27999, 28000, 28001 };
        for (int s = 0; s < 5; s++) {
            fk_thr4(999, 999, th[s], 999);
            fk_clock = clk[s];
            t_rec.ctx = actx; t_rec.weapon = weapon; t_rec.r14 = NULL; t_rec.ret = NULL;
            res[s] = t_call_a2(0, actx);
            a1v[s] = t_call_a1(actx, list + 2);
        }
        /* 0 ALLOW (tracks), 1 held ALLOW though nothing passes now, 2 still held at 7.999 s, 3 expired -> REJECT, 4 REJECT held */
        int ga = res[0] == 2 && a1v[0] == 1 && res[1] == 2 && a1v[1] == 0 && res[2] == 2 && res[3] == 3 && a1v[3] == 0 && res[4] == 3 && a1v[4] == 0;
        printf("A stubs + verdict hold: rows %d %d %d %d %d (2 2 2 3 3 = match match match next next), tracks volume %d %d . %d %d (1 0 . 0 0) -> %s\n",
               res[0], res[1], res[2], res[3], res[4], a1v[0], a1v[1], a1v[3], a1v[4], ga ? "OK" : "FAIL");
        ok &= ga;

        /* range state: a held ALLOW does not survive the target leaving penetration range, nor a range REJECT it coming back */
        memset(c_hold, 0, sizeof c_hold); fk_thr4(999, 999, 10, 999);
        int r[3];
        fk_clock = 40000; fk_penv = 100.0f; t_rec.weapon = weapon; r[0] = t_call_a2(0, actx);
        fk_clock = 40001; fk_penv = 0.0f; r[1] = t_call_a2(0, actx);
        fk_clock = 40002; fk_penv = 100.0f; r[2] = t_call_a2(0, actx);
        int gr = r[0] == 2 && r[1] == 3 && r[2] == 2;
        printf("A range state vs hold: in range %d, out of range %d, back in range %d (2 3 2) -> %s\n", r[0], r[1], r[2], gr ? "OK" : "FAIL");
        ok &= gr;

        /* weapon key: a REJECT held for weapon 1 does not answer for weapon 2 of the same shooter (another {parameters}) */
        memset(c_hold, 0, sizeof c_hold);
        hold_put(0x8000, 0xC00A, k, 40003, 2000.0f, 1, 0);
        fk_clock = 40004; t_rec.weapon = weapon2; int w2 = t_call_a2(0, actx);
        fk_clock = 40005; t_rec.weapon = weapon; int w1 = t_call_a2(0, actx);
        int gw = w2 == 2 && w1 == 3;
        printf("A weapon key: weapon 2 %d (2, own verdict), weapon 1 %d (3, held REJECT) -> %s\n", w2, w1, gw ? "OK" : "FAIL");
        ok &= gw;

        /* direct control: left alone (vanilla), even with nothing pierceable */
        memset(c_hold, 0, sizeof c_hold); fk_thr4(999, 999, 999, 999);
        fk_clock = 40010; int d0 = t_call_a2(0, actx);
        *(int32_t *)(dcprop + 0x18) = 1; fk_clock = 40011; int d1 = t_call_a2(0, actx);
        fk_reset_cands(); t_call_p(aim, weapon, target, 1, 0); int dn = (int)(fk_vec[1] - fk_vec[0]);
        *(int32_t *)(dcprop + 0x18) = 0;
        uint8_t *dd8 = (uint8_t *)calloc(1, 0x80); *(uint32_t *)(dd8 + 0x70) = 2; *(uint8_t **)(shooter + 0xDD8) = dd8;
        fk_clock = 40012; int d2 = t_call_a2(0, actx);
        *(uint8_t **)(shooter + 0xDD8) = NULL;
        int gd = d0 == 3 && d1 == 2 && dn == 4 && d2 == 2;
        printf("direct control: A without %d (3), with the property %d (2), P list untouched %d (4), with the DD8 bit %d (2) -> %s\n",
               d0, d1, dn, d2, gd ? "OK" : "FAIL");
        ok &= gd;

        /* new battle: the clock going back clears every cache and the per-tick prep memo */
        fk_thr4(999, 999, 10, 999);
        fk_clock = 50000; t_call_a2(0, actx);
        blk_put(0x8000, 0xC00A, k, 1, 50000, 2000.0f, vol[1], target, eye); unr_put(0x8000, 0xC00A, k, 2, 50000); hold_put(1, 2, 3, 50000, 1000.0f, 1, 0);
        uint32_t fl0 = ST.flushes, ep0 = g_epoch;
        fk_clock = 1000; int e1 = t_call_a2(0, actx);
        int v = -1;
        int ge = ST.flushes == fl0 + 1 && g_epoch == ep0 + 1 && e1 == 2 && !blk_hit(0x8000, 0xC00A, k, 1, 50000, 2000.0f, vol[1], target, eye)
                 && !unr_hit(0x8000, 0xC00A, k, 2, 50000) && !hold_get(1, 2, 3, 50000, 1000.0f, 1, &v, NULL) && g_last_now == 1000;
        printf("new battle: flush %d, epoch %d, caches empty %d -> %s\n", ST.flushes == fl0 + 1, g_epoch == ep0 + 1, ge, ge ? "OK" : "FAIL");
        ok &= ge;
        hookA_on = 0; fk_clock = 1000;
    }

    /* pick_compact edge cases */
    uint8_t *cc[4] = { vol[0], vol[1], vol[2], vol[3] }; uint8_t kp[4] = { 0, 1, 0, 1 }; int cur = 3;
    int nk = pick_compact(cc, 4, kp, &cur);
    int pc = nk == 2 && cc[0] == vol[1] && cc[1] == vol[3] && cur == 1;
    printf("candidate compaction: kept %d, order kept %d, current follows %d -> %s\n", nk, cc[0] == vol[1] && cc[1] == vol[3], cur, pc ? "OK" : "FAIL");
    ok &= pc;
    S = keep; prio_on = 0; hookB_on = 0; dc_on = 0; g_clock = NULL; cfg_log = 1; cfg_logverdicts = 400; e_getvol = NULL; e_solve = NULL; g_last_now = 0;
    return ok;
}

/* install on the mapped image (nothing of it runs) and check the patched bytes lead where they should */
static int patch_test(uint8_t *base)
{
    hookA_on = hookB_on = hookP_on = hookX_on = 0;
    g_logpath[0] = 0; g_log = NULL;
    install_at(base);
    int ok = hookA_on && hookB_on && hookP_on && hookX_on;
    if (!ok) { printf("  patch test: hooks not installed (A %d B %d P %d X %d)\n", hookA_on, hookB_on, hookP_on, hookX_on); return 0; }
#define THUNK(at, want) ((at)[0] == 0xFF && (at)[1] == 0x25 && *(uint64_t *)((at) + 6) == (uint64_t)(uintptr_t)(want))
    uint8_t *t1 = rel32(S.a1 + 1), *t2 = rel32(S.a2 + 1), *tb = rel32(S.bsite + 1), *tp = rel32(S.psite + 1), *tx = rel32(S.xsite + 1);
    int a1 = S.a1[0] == 0xE8 && THUNK(t1, pf_stub_a1);
    int a2 = S.a2[0] == 0xE9 && THUNK(t2, pf_stub_a2) && S.a2[5] == 0x90 && S.a2[7] == 0x90;
    int bb = S.bsite[0] == 0xE8 && THUNK(tb, pf_solve);
    int pp = S.psite[0] == 0xE9 && THUNK(tp, pf_stub_p) && S.psite[5] == 0x90 && S.psite[6] == 0x90 && S.psite[7] == 0xB8;
    int xx = S.xsite[0] == 0xE8 && THUNK(tx, pf_stub_x) && S.xsite[-3] == 0x49;
    int w = THUNK(S.builder, pf_stub_w) && S.builder[14] == 0x90;
    uint8_t *tr = (uint8_t *)(uintptr_t)g_w_tramp;
    pat_t pb; pat_parse(&pb, P_BLD);
    int wt = !memcmp(tr, pb.b, BLD_COPY) && THUNK(tr + BLD_COPY, S.builder + BLD_COPY);
    int tg = g_tagtest == (void *)S.tagtest && g_a2_back == (uint64_t)(uintptr_t)(S.a2 + 8) && g_a2_match == (uint64_t)(uintptr_t)S.match
             && g_a2_next == (uint64_t)(uintptr_t)S.next && (uint8_t *)e_solve == S.solver && g_p_back == (uint64_t)(uintptr_t)(S.psite + PICKP_LEN)
             && (uint8_t *)g_ve == S.vend && (uint8_t *)g_vb == S.vbeg && (uint8_t *)e_clear == S.clr && (uint8_t *)e_getvol == S.getvol
             && (uint8_t *)e_wstop == S.wstop;
    int fl = solve_on && prio_on && dc_on;
    ok = a1 && a2 && bb && pp && xx && w && wt && tg && fl;
    printf("  patch test: A1 call %d, A2 jmp %d, B call %d, P jmp %d, X call %d, builder jmp %d, builder trampoline %d, targets %d, "
           "solver redirect + priority + direct control %d -> %s\n", a1, a2, bb, pp, xx, w, wt, tg, fl, ok ? "OK" : "FAIL");
    hookA_on = hookB_on = hookP_on = hookX_on = prio_on = solve_on = dc_on = 0;
    return ok;
}
int main(int argc, char **argv)
{
    int bad = 0;
    g_logpath[0] = 0;
    parse_prio(DEF_PRIO);
    printf("GEM PenFilter %s self-test\n", PF_VERSION);
    for (int i = 1; i < argc; i++) {
        HMODULE m = LoadLibraryExA(argv[i], NULL, DONT_RESOLVE_DLL_REFERENCES);
        if (!m) { printf("%s: load failed %lu\n", argv[i], GetLastError()); bad = 1; continue; }
        find_all((uint8_t *)m, &S);
        printf("== %s\n", argv[i]);
        g_log = stdout;
        log_sites();
        g_log = NULL;
        int a = !S.err_ma && !S.err_bld && !S.err_ai && !S.err_arm && !S.err_clk, b = !S.err_b && !S.err_ai && !S.err_arm && !S.err_clk;
        int p = b && !S.err_p, x = p && !S.err_x;
        printf("  => hook A %s%s, hook B %s%s, hook P %s, hook X %s%s, facing %s, direct control test %s\n", a ? "OK" : "FAIL", a && S.scan_err ? " (enemy scan unfiltered)" : "",
               b ? "OK" : "FAIL", S.err_gv ? " (no aim-volume getter)" : "", p ? "OK" : "FAIL", x ? "OK" : "FAIL",
               S.err_stop ? " (no weapon stop)" : "", S.box_vt && S.poly_vt ? "OK" : "OFF", S.err_man ? "FAIL" : "OK");
        if (!a || !b || !p || !x || S.scan_err || S.err_gv || S.err_stop || S.err_man) bad = 1;
        if (!patch_test((uint8_t *)m)) bad = 1;
    }
    if (!stub_test()) bad = 1;
    if (!logic_test()) bad = 1;
    /* helpers on fake data */
    static uint8_t tags[0x60 * 3], v[16];
    memset(tags, 0, sizeof tags);
    fake_str(tags + 0x40, "Pierce"); fake_str(tags + 0x60 + 0x40, "bolt"); fake_str(tags + 0xC0 + 0x40, "a_much_longer_tag_name_heap");
    *(uint8_t **)v = tags; *(uint8_t **)(v + 8) = tags + 0x60 * 3;
    char l1[1][TLEN] = { "pierce" }, l2[2][TLEN] = { "melee", "a_much_longer_tag_name_heap" }, l3[1][TLEN] = { "he" };
    int t1 = vec_has(v, l1, 1), t2 = vec_has(v, l2, 2), t3 = vec_has(v, l3, 1);
    uint32_t h1 = vec_hash(v); fake_str(tags + 0x40, "PIERCE"); uint32_t h2 = vec_hash(v);
    *(uint8_t **)(v + 8) = tags + 0x50; int t4 = vec_has(v, l1, 1);
    int ok = t1 == 1 && t2 == 1 && t3 == 0 && t4 == -1 && h1 == h2 && h1 != 0;
    printf("tag reader / stable hash: %d %d %d %d %08X %08X -> %s\n", t1, t2, t3, t4, h1, h2, ok ? "OK" : "FAIL");
    if (!ok) bad = 1;
    printf(bad ? "SOME FAILED\n" : "ALL OK\n");
    return bad;
}
#endif
