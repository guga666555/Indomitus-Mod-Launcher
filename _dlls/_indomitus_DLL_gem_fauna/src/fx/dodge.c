/* GEM Fauna - AI fauna get out of the way of bigger units that can run them over (0.12.0)
 * Part of the unity build: #included by gem_fauna.c after contact.c.
 *
 * Soldiers dodge vehicles with a crush scanner in the human FSM. Vehicle-class units (fauna run the vehicle brain) have
 * their own version: the vehicle root state's "crush" transition. Its condition C (called from the root update right
 * after "and cl,3; cmp cl,1; je" = not in direct control):
 *   - the unit itself has no {props "tank"};
 *   - for each unit its sensor sees: not flagged out (+0x1C0 bit2), has {props "tank"} (tag compare call B), moving
 *     (velocity call V != 0,0,0), then the engine's danger-zone prediction (crush/min_danger_zone_time / _length);
 *   - in danger -> the "crush" state: the unit drives to a random point aside.
 * So vehicles flee only from moving tanks, of any side. For a fauna the threat test is replaced, the rest (sensor list,
 * moving test, danger zone, flee state) stays the engine's:
 *   - while C runs for a fauna (wrapped at its call site): B answers "yes" for every unit, and V answers "not moving"
 *     for a unit that is no threat to this fauna, so the engine skips it;
 *   - threat = an actor that is not a soldier whose size >= the fauna's size + fauna_dodge_size_margin. Size = the
 *     number in a {props "fauna_size<N>"} (fauna or not), else fauna_dodge_default_size (fauna),
 *     fauna_dodge_tank_size (props "tank") or fauna_dodge_vehicle_size (other vehicles);
 *   - allies are skipped (they do not contact an AI fauna since 0.11.0) unless in direct control (fauna_dodge_allies);
 *   - a fauna with {props "fauna_no_dodge"} (fauna_dodge_never_prop) or "tank" never dodges.
 * Simulation (the host runs the AI).
 */

typedef uint8_t (*CrushCondFn)(void *self);
typedef uint8_t (*TagCmpFn)(void *entry, void *tag);
typedef float *(*VelocityFn)(void *e, float *out);
static uint8_t *g_dodgeCondSite, *g_dodgeCmpSite, *g_dodgeVelSite, *g_dodgeTankTag;
static CrushCondFn g_dodgeCondOrig; static TagCmpFn g_dodgeCmpOrig; static VelocityFn g_dodgeVelOrig;
static uint32_t g_dodgeCtxOff, g_dodgeEntOff;
static int g_okDodge, g_wantDodge = 1, g_dodgeMargin = 1, g_dodgeDefSize = 2, g_dodgeVehSize = 3, g_dodgeTankSize = 4, g_dodgeAllies = 0;
static char g_dodgeNeverProp[48] = "fauna_no_dodge", g_sizePropPrefix[40] = "fauna_size";
#define DODGE_SIZES 9

static int find_dodge(void)
{
    /* the condition's call site in the root update: and cl,3; cmp cl,1; je +x; mov rcx,rsi; call C */
    static const uint8_t pre[] = { 0x80, 0xE1, 0x03, 0x80, 0xF9, 0x01, 0x74 };
    int n = 0;
    for (uint8_t *p = g_text.p; p + 16 <= g_text.p + g_text.n; p++)
        if (!memcmp(p, pre, sizeof pre) && p[8] == 0x48 && p[9] == 0x8B && p[10] == 0xCE && p[11] == 0xE8) { if (!n++) g_dodgeCondSite = p + 11; }
    if (n != 1) { logf_("dodge: crush condition call not found (%d candidates)", n); return 0; }
    uint8_t *C = g_dodgeCondSite + 5 + *(int32_t *)(g_dodgeCondSite + 1);
    /* the unit: mov rax,[rcx+X]; ... mov rdx,[rax+Y] near the start */
    g_dodgeCtxOff = g_dodgeEntOff = 0;
    for (uint8_t *q = C; q < C + 0x40; q++) {
        if (!g_dodgeCtxOff && q[0] == 0x48 && q[1] == 0x8B && q[2] == 0x41) g_dodgeCtxOff = q[3];
        if (g_dodgeCtxOff && !g_dodgeEntOff && q[0] == 0x48 && q[1] == 0x8B && q[2] == 0x50) g_dodgeEntOff = q[3];
    }
    /* the two "tank" compares: lea rdx,[rip+TAG]; mov rcx,r; call CMP - the second one is the threat's */
    uint8_t *lea[2] = { 0 }; int nl = 0;
    for (uint8_t *q = C; q < C + 0x180 && nl < 2; q++)
        if (q[0] == 0x48 && q[1] == 0x8D && q[2] == 0x15) {
            uint8_t *t = q + 7 + *(int32_t *)(q + 3);
            if (!nl || t == lea[0] + 7 + *(int32_t *)(lea[0] + 3)) lea[nl++] = q;
        }
    if (nl != 2 || !g_dodgeCtxOff || !g_dodgeEntOff) { logf_("dodge: crush condition layout not recognised"); return 0; }
    g_dodgeTankTag = lea[1] + 7 + *(int32_t *)(lea[1] + 3);
    if (!next_call(lea[1] + 7, 0x10, &g_dodgeCmpSite)) { logf_("dodge: threat tag test not found"); return 0; }
    /* the moving test: the next call, followed by movss xmm0,[rax]; ucomiss xmm0,[rip+zero] */
    uint8_t *v = NULL;
    for (uint8_t *q = g_dodgeCmpSite + 5; q < g_dodgeCmpSite + 0x60 && !v; q++)
        if (q[0] == 0xE8 && q[5] == 0xF3 && q[6] == 0x0F && q[7] == 0x10 && q[8] == 0x00 && q[9] == 0x0F && q[10] == 0x2E) v = q;
    if (!v) { logf_("dodge: threat moving test not found"); return 0; }
    g_dodgeVelSite = v;
    logf_("dodge: crush condition exe+0x%llX (call @ exe+0x%llX, unit [[this+0x%X]+0x%X]), threat tag test @ exe+0x%llX, moving test @ exe+0x%llX",
          EXE_OFF(C), EXE_OFF(g_dodgeCondSite), g_dodgeCtxOff, g_dodgeEntOff, EXE_OFF(g_dodgeCmpSite), EXE_OFF(g_dodgeVelSite));
    return 1;
}

#ifndef FX_TEST
static __attribute__((aligned(16))) uint8_t g_sizeTags[DODGE_SIZES][0x80], g_neverTag[0x80];
static uint64_t g_sizeIds[DODGE_SIZES], g_neverId;
static volatile DWORD g_dodgeTid; static uint8_t *volatile g_dodgeUnit; static int g_dodgeUnitSize;

#define SIZE_CACHE_N 64
static struct { const void *def; int8_t size; int8_t never; int8_t tank; } g_sizeCache[SIZE_CACHE_N];
static void def_size_info(const uint8_t *e, int *size, int *never, int *tank)   /* size 0 = no size prop */
{
    const void *def = *(const void *const *)(e + g_entDefOff);
    unsigned h = (unsigned)(((uintptr_t)def >> 4) % SIZE_CACHE_N);
    if (def && g_sizeCache[h].def == def) { *size = g_sizeCache[h].size; *never = g_sizeCache[h].never; *tank = g_sizeCache[h].tank; return; }
    int s = 0, nv = 0, tk = 0;
    const uint8_t *b, *en;
    uint64_t tankId = g_dodgeTankTag ? TAG_ID(g_dodgeTankTag) : 0;
    if (def_props(e, &b, &en))
        for (; b + TAG_SIZE <= en; b += TAG_SIZE) {
            uint64_t id = TAG_ID(b);
            for (int i = 0; i < DODGE_SIZES; i++) if (id == g_sizeIds[i] && i + 1 > s) s = i + 1;
            if (g_neverId && id == g_neverId) nv = 1;
            if (tankId && id == tankId) tk = 1;
        }
    *size = s; *never = nv; *tank = tk;
    if (def) { g_sizeCache[h].def = def; g_sizeCache[h].size = (int8_t)s; g_sizeCache[h].never = (int8_t)nv; g_sizeCache[h].tank = (int8_t)tk; }
}
static int unit_size(const uint8_t *e, int fauna)
{
    int s, nv, tk; def_size_info(e, &s, &nv, &tk);
    return s ? s : fauna ? g_dodgeDefSize : tk ? g_dodgeTankSize : g_dodgeVehSize;
}
static int dodge_threat(const uint8_t *unit, const uint8_t *t)
{
    if (!t || t == unit || !(t[ENT_FLAGS] & 2) || (t[ENT_FLAGS] & 4)) return 0;          /* actors only, never soldiers */
    int sz = unit_size(t, is_fauna(t));
    if (sz < g_dodgeUnitSize + g_dodgeMargin) return 0;
    if (!g_dodgeAllies && g_okAlly && relation(unit, t) == 2 && !chassis_manual(t)) return 0;
    static int logged; if (logged < 8) { logged++; logf_("dodge: fauna %p (size %d) sees a bigger %s %p (size %d) - danger-zone test", unit, g_dodgeUnitSize, ent_kind(t), t, sz); }
    return 1;
}
static uint8_t fx_dodge_cmp(void *entry, void *tag)
{
    if (g_dodgeUnit && g_dodgeTid == GetCurrentThreadId()) return 1;                 /* the size test (moving test) decides */
    return g_dodgeCmpOrig(entry, tag);
}
static float *fx_dodge_vel(void *t, float *out)
{
    uint8_t *u = g_dodgeUnit;
    if (u && g_dodgeTid == GetCurrentThreadId() && !dodge_threat(u, t)) { out[0] = out[1] = out[2] = 0.0f; return out; }
    return g_dodgeVelOrig(t, out);
}
static uint8_t fx_dodge_cond(void *self)
{
    uint8_t *ctx = self ? *(uint8_t **)((uint8_t *)self + g_dodgeCtxOff) : NULL;
    uint8_t *e = ctx ? *(uint8_t **)(ctx + g_dodgeEntOff) : NULL;
    if (!e || !is_fauna(e)) return g_dodgeCondOrig(self);
    int s, nv, tk; def_size_info(e, &s, &nv, &tk);
    if (nv) return 0;
    uint8_t *prevU = g_dodgeUnit; DWORD prevT = g_dodgeTid; int prevS = g_dodgeUnitSize;
    g_dodgeUnitSize = s ? s : g_dodgeDefSize; g_dodgeTid = GetCurrentThreadId(); g_dodgeUnit = e;
    uint8_t r = g_dodgeCondOrig(self);
    g_dodgeUnit = prevU; g_dodgeTid = prevT; g_dodgeUnitSize = prevS;
    static int first, logged;
    if (!first) { first = 1; logf_("dodge: fauna %p (size %d) runs the vehicle crush check", e, s ? s : g_dodgeDefSize); }
    if (r && logged < 8) { logged++; logf_("dodge: fauna %p gets out of the way", e); }
    return r;
}
static int install_dodge(void)
{
    if (!g_tagCtorCStr) { logf_("dodge: no tag constructor"); return 0; }
    char nm[64];
    for (int i = 0; i < DODGE_SIZES; i++) {
        snprintf(nm, sizeof nm, "%s%d", g_sizePropPrefix, i + 1);
        g_tagCtorCStr(g_sizeTags[i], nm); g_sizeIds[i] = TAG_ID(g_sizeTags[i]);
    }
    if (g_dodgeNeverProp[0]) { g_tagCtorCStr(g_neverTag, g_dodgeNeverProp); g_neverId = TAG_ID(g_neverTag); }
    void *o = NULL;
    if (!redirect_call(g_dodgeCmpSite, (void *)fx_dodge_cmp, &o, "dodge (threat tag test)")) return 0;
    g_dodgeCmpOrig = (TagCmpFn)o;
    if (!redirect_call(g_dodgeVelSite, (void *)fx_dodge_vel, &o, "dodge (threat moving test)")) return 0;
    g_dodgeVelOrig = (VelocityFn)o;
    if (!redirect_call(g_dodgeCondSite, (void *)fx_dodge_cond, &o, "dodge (crush condition)")) return 0;
    g_dodgeCondOrig = (CrushCondFn)o;
    return 1;
}
#endif /* FX_TEST */
