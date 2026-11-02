/* GEM Fauna - direct-control contact for foot-chassis fauna (ram people, crush objects)
 * Part of the unity build: #included by gem_fauna.c, not compiled on its own (after sprint.c).
 *
 * Under AI a fauna on a foot chassis follows its path and simply walks into whatever stands on it: the physics contact
 * between the two entities then does the crushing / ramming, as for any vehicle-class unit. In direct control the foot
 * chassis moves through eChassisFoot::ManualSimulation, which (like for a soldier) runs a "human_tpc" collision step
 * before every move: it queries the scene around the target point (mask 0x2013402) and, for each volume the unit
 * would overlap, pushes the target back out of it. So a controlled fauna stops at the edge of every soldier, fence or
 * crate and never touches them - no contact, nothing is crushed or rammed.
 *
 * The step (found by its "human_tpc" pather class and its query mask; 2 callers, both foot chassis, rcx = chassis) is
 * wrapped; while it runs for a fauna in direct control, its volume test (call after lea r9,[vol+XFORM]) reports
 * "no overlap" for volumes owned by
 *   - a soldier (entity flag human)                    -> the fauna walks into him: the engine's contact rams/crushes
 *   - an object that is not an actor (fence, crate...) -> ini fauna_dc_crush_objects (default on)
 * Vehicles and other fauna still push it back. Static terrain and buildings are not part of this step: the chassis
 * tests the fauna's own pather class at the target first (the same grid the AI path uses), so a fauna stays out of
 * what it could not walk through under AI either. Simulation (every exe that simulates a direct-controlled unit).
 */

typedef void *(*PushStepFn)(void *ch, void *dir, void *target, void *out, uint64_t a5, uint64_t a6);
typedef uint8_t (*VolHitFn)(void *ctx, void *shape, void *probe, void *xform, void *out);
static uint8_t *g_pushStep, *g_pushSites[4], *g_volHitSite; static int g_pushSiteN;
static PushStepFn g_pushStepOrig; static VolHitFn g_volHitOrig;
static uint32_t g_volXformOff;
#define VOL_OWNER 0x40                     /* collision volume -> owning entity (the engine's volume transform update reads it) */
#define PUSH_MASK 0x2013402u               /* the step's scene query: mov edx,0x2013402 */
static int g_okContact, g_wantContact = 1, g_contactObjects = 1;

static int find_contact(void)
{
    if (!g_vtFoot || !g_footEntOff || !g_entChassisOff || !g_footManualOff) { logf_("contact: needs the eChassisFoot finders (sprint)"); return 0; }
    static const char cls[] = "human_tpc";                                   /* loaded with movsd xmm0,[rip+X] (9 bytes) */
    uint8_t *s = find_bytes(g_rdata.p, g_rdata.n, (const uint8_t *)cls, sizeof cls - 1);
    static const uint8_t mask[] = { 0xBA, PUSH_MASK & 0xFF, (PUSH_MASK >> 8) & 0xFF, (PUSH_MASK >> 16) & 0xFF, PUSH_MASK >> 24 };
    for (uint8_t *p = g_text.p; s && p + 8 <= g_text.p + g_text.n && !g_pushStep; p++) {
        if (p[0] != 0xF2 || p[1] != 0x0F || p[2] != 0x10 || (p[3] & 0xC7) != 0x05 || p + 8 + *(int32_t *)(p + 4) != s) continue;
        uint32_t f = func_of(p);
        if (f && find_bytes(g_base + f, 0x600, mask, sizeof mask)) g_pushStep = g_base + f;
    }
    if (!g_pushStep) { logf_("contact: direct-control collision step not found"); return 0; }
    /* the volume test: lea r9,[rsi+XFORM] ... call T ; test al,al */
    for (uint8_t *q = g_pushStep; q < g_pushStep + 0x600 && !g_volHitSite; q++) {
        if (q[0] != 0x4C || q[1] != 0x8D || q[2] != 0x4E) continue;
        uint8_t *site = NULL;
        if (next_call(q + 4, 0x30, &site) && site[5] == 0x84 && site[6] == 0xC0) { g_volHitSite = site; g_volXformOff = q[3]; }
    }
    if (!g_volHitSite || g_volXformOff < 0x20 || g_volXformOff > 0x100) { logf_("contact: volume test in the collision step not recognised"); return 0; }
    for (uint8_t *p = g_text.p; p + 5 <= g_text.p + g_text.n && g_pushSiteN < 4; p++)
        if (p[0] == 0xE8 && p + 5 + *(int32_t *)(p + 1) == g_pushStep) g_pushSites[g_pushSiteN++] = p;
    if (!g_pushSiteN || g_pushSiteN > 3) { logf_("contact: collision step has %d callers (expected 1-3)", g_pushSiteN); return 0; }
    logf_("contact: direct-control collision step exe+0x%llX (%d calls), volume test call @ exe+0x%llX (transform vol+0x%X)",
          EXE_OFF(g_pushStep), g_pushSiteN, EXE_OFF(g_volHitSite), g_volXformOff);
    return 1;
}

#ifndef FX_TEST
static volatile DWORD g_pushTid; static uint8_t *volatile g_pushFauna;   /* the fauna whose collision step is running */
static uint8_t fx_vol_hit(void *ctx, void *shape, void *probe, void *xform, void *out)
{
    uint8_t *fauna = g_pushFauna;
    if (fauna && xform && g_pushTid == GetCurrentThreadId()) {
        uint8_t *owner = *(uint8_t **)((uint8_t *)xform - g_volXformOff + VOL_OWNER);
        if (owner && owner != fauna && !((uintptr_t)owner & 7)) {
            uint8_t fl = owner[ENT_FLAGS];
            int human = (fl & 4) != 0, object = !(fl & 2);
            if (human || (object && g_contactObjects)) {
                static int logged; if (logged < 8) { logged++; logf_("contact: fauna %p in direct control walks into %s %p (no push-back)", fauna, human ? "soldier" : "object", owner); }
                return 0;
            }
        }
    }
    return g_volHitOrig(ctx, shape, probe, xform, out);
}
static void *fx_push_step(void *ch, void *dir, void *target, void *out, uint64_t a5, uint64_t a6)
{
    uint8_t *e = foot_fauna(ch);
    if (!e || !*(int32_t *)((uint8_t *)ch + g_footManualOff)) return g_pushStepOrig(ch, dir, target, out, a5, a6);
    uint8_t *prevF = g_pushFauna; DWORD prevT = g_pushTid;                 /* (not expected to nest) */
    g_pushTid = GetCurrentThreadId(); g_pushFauna = e;
    void *r = g_pushStepOrig(ch, dir, target, out, a5, a6);
    g_pushFauna = prevF; g_pushTid = prevT;
    return r;
}
static int install_contact(void)
{
    void *orig = NULL;
    if (!redirect_call(g_volHitSite, (void *)fx_vol_hit, &orig, "contact (volume test)")) return 0;
    g_volHitOrig = (VolHitFn)orig;
    int n = 0;
    for (int i = 0; i < g_pushSiteN; i++) {
        void *o = NULL;
        if (redirect_call(g_pushSites[i], (void *)fx_push_step, &o, "contact (collision step)")) { g_pushStepOrig = (PushStepFn)o; n++; }
    }
    return n > 0;
}
#endif /* FX_TEST */
