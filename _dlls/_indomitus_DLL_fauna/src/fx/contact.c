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
 *
 * 0.11.0
 *  - The step's pre-test (is the target blocked for this pather class?) used the literal soldier class "human_tpc", so a
 *    fauna was tested as a soldier: whatever a soldier cannot pass (fences, hedgehogs...) stopped it, even where the
 *    fauna's own obstacle class (obstacleID, the class its AI path uses) passes. The class lookup in the step is now
 *    redirected to the fauna's own pather class (entity byte the engine's own query builder reads) - ini fauna_dc_pather:
 *    own (default) / human (engine) / off (no step at all: moves like under AI, only the terrain grid test).
 *  - AI-controlled fauna no longer contact (ram / crush / push) allied units: the physics contact pair of a fauna that is
 *    not in direct control and an allied actor (relation 2, the engine's own "friendly" value) is dropped. The engine
 *    already does this for an AI vehicle against an allied soldier only; fauna against fauna/vehicles were not covered.
 *    ini fauna_ai_ally_contact (0 = no contact with allies), fauna_ai_ally_contact_vehicles (allied vehicles too).
 *    Units in direct control still contact everything.
 */

typedef void *(*PushStepFn)(void *ch, void *dir, void *target, void *out, uint64_t a5, uint64_t a6);
typedef uint8_t (*VolHitFn)(void *ctx, void *shape, void *probe, void *xform, void *out);
static uint8_t *g_pushStep, *g_pushSites[3], *g_volHitSite; static int g_pushSiteN;
static PushStepFn g_pushStepOrig; static VolHitFn g_volHitOrig;
static uint32_t g_volXformOff;
#define VOL_OWNER 0x40                     /* collision volume -> owning entity (the engine's volume transform update reads it) */
#define PUSH_MASK 0x2013402u               /* the step's scene query: mov edx,0x2013402 */
static int g_okContact, g_wantContact = 1, g_contactObjects = 1;
/* 0.11.0: the pather class of the step */
typedef uint8_t (*ClassByNameFn)(void *name);
static uint8_t *g_classSite; static ClassByNameFn g_classOrig; static uint32_t g_entPatherOff;
enum { DCP_OFF, DCP_OWN, DCP_HUMAN };
static int g_dcPather = DCP_OWN;

static int find_contact(void)
{
    if (!g_vtFoot || !g_footEntOff || !g_entChassisOff || !g_footManualOff) { logf_("contact: needs the eChassisFoot finders (sprint)"); return 0; }
    static const char cls[] = "human_tpc";                                   /* loaded with movsd xmm0,[rip+X] (9 bytes) */
    uint8_t *s = find_bytes(g_rdata.p, g_rdata.n, (const uint8_t *)cls, sizeof cls - 1), *ld = NULL;
    static const uint8_t mask[] = { 0xBA, PUSH_MASK & 0xFF, (PUSH_MASK >> 8) & 0xFF, (PUSH_MASK >> 16) & 0xFF, PUSH_MASK >> 24 };
    for (uint8_t *p = g_text.p; s && p + 8 <= g_text.p + g_text.n && !g_pushStep; p++) {
        if (p[0] != 0xF2 || p[1] != 0x0F || p[2] != 0x10 || (p[3] & 0xC7) != 0x05 || p + 8 + *(int32_t *)(p + 4) != s) continue;
        uint32_t f = func_of(p);
        if (f && find_bytes(g_base + f, 0x600, mask, sizeof mask)) { g_pushStep = g_base + f; ld = p; }
    }
    if (!g_pushStep) { logf_("contact: direct-control collision step not found"); return 0; }
    /* the volume test: lea r9,[rsi+XFORM] ... call T ; test al,al */
    for (uint8_t *q = g_pushStep; q < g_pushStep + 0x600 && !g_volHitSite; q++) {
        if (q[0] != 0x4C || q[1] != 0x8D || q[2] != 0x4E) continue;
        uint8_t *site = NULL;
        if (next_call(q + 4, 0x30, &site) && site[5] == 0x84 && site[6] == 0xC0) { g_volHitSite = site; g_volXformOff = q[3]; }
    }
    if (!g_volHitSite || g_volXformOff < 0x20 || g_volXformOff > 0x100) { logf_("contact: volume test in the collision step not recognised"); return 0; }
    int n = calls_to(g_pushStep, g_pushSites, 3);
    if (n < 1 || n > 3) { logf_("contact: collision step has %d callers (expected 1-3)", n); return 0; }
    g_pushSiteN = n;
    logf_("contact: direct-control collision step exe+0x%llX (%d calls), volume test call @ exe+0x%llX (transform vol+0x%X)",
          EXE_OFF(g_pushStep), g_pushSiteN, EXE_OFF(g_volHitSite), g_volXformOff);
    /* the step's pather class: the first call after the "human_tpc" load is the class-by-name lookup (returns the class
       index); the query init that takes the index is the call after the next movzx edx,bl. The engine's own query
       builder for an entity loads the entity's class with movzx edx,byte [rdx+X] right before calling that init. */
    g_classSite = NULL; g_entPatherOff = 0;
    uint8_t *cs = NULL, *init = NULL;
    if (ld && next_call(ld + 8, 0x30, &cs)) {
        for (uint8_t *q = cs + 5; q < cs + 0x180 && !init; q++)
            if (q[0] == 0x0F && q[1] == 0xB6 && q[2] == 0xD3) init = next_call(q + 3, 0x60, NULL);
    }
    if (cs && init) {
        int hits = 0;
        for (uint8_t *q = g_text.p; q + 12 <= g_text.p + g_text.n; q++)
            if (q[0] == 0x0F && q[1] == 0xB6 && q[2] == 0x92 && q[5] == 0 && q[6] == 0 && q[7] == 0xE8 && q + 12 + *(int32_t *)(q + 8) == init) {
                uint32_t off = *(uint32_t *)(q + 3);
                if (!hits++) g_entPatherOff = off; else if (off != g_entPatherOff) g_entPatherOff = 0;
            }
        if (g_entPatherOff >= 0x200 && g_entPatherOff < 0x2000) g_classSite = cs; else g_entPatherOff = 0;
    }
    if (g_classSite) logf_("contact: step pather class lookup @ exe+0x%llX, entity pather class byte +0x%X", EXE_OFF(g_classSite), g_entPatherOff);
    else logf_("contact: step pather class lookup not recognised (the step keeps the soldier class)");
    return 1;
}

/* ---- AI fauna vs allies: the physics contact pair ----
   The pair function (eGameEntity contact chain, rcx = pair, [pair] = A, [pair+8] = B) holds the engine's own friendly
   skip: for a soldier B and a non-manual actor A, call R(A, B, 0); cmp eax,2; je <return>. R = relation of the two
   entities' players (same player = 2, else the diplomacy table). Its callers are found by R's byte pattern. */
typedef void (*PairFn)(void *pair);
static uint8_t *g_pairFn, *g_pairSite; static PairFn g_pairOrig;
static uint32_t g_plOff, g_plAltOff, g_chManualOff, g_plMax, g_pairChassisOff;
static int32_t *g_dipStride; static int32_t **g_dipTable;
static int g_okAlly, g_wantAllyContact = 0, g_allyVehicles = 0;

static int find_ally_filter(void)
{
    static const uint8_t pat[] = { 0x4C, 0x8B, 0xC9, 0x48, 0x85, 0xD2, 0x74 };     /* mov r9,rcx; test rdx,rdx; je */
    uint8_t *R = NULL; int nR = 0;
    for (uint8_t *p = g_text.p; p + 0x70 <= g_text.p + g_text.n; p++) {
        if (memcmp(p, pat, sizeof pat)) continue;
        if (p[8] != 0x8B || p[9] != 0x82 || p[14] != 0xC1 || p[15] != 0xE8 || p[16] != 0x08 || p[17] != 0xA8 || p[18] != 0x01) continue;
        if (!nR++) R = p;
    }
    if (nR != 1) { logf_("ally contact: relation function not found (%d candidates)", nR); return 0; }
    /* players: 0F B6 8A <alt> (B, r8 == 0 branch) right before 41 0F B6 81 <off> (A), then 3A C1, then 0F B6 8A <off> (B) */
    g_plOff = g_plAltOff = g_plMax = 0; g_dipStride = NULL; g_dipTable = NULL;
    for (uint8_t *q = R + 7; q < R + 0x60; q++) {
        if (!g_plOff && q[0] == 0x41 && q[1] == 0x0F && q[2] == 0xB6 && q[3] == 0x81 && q[-7] == 0x0F && q[-6] == 0xB6 && q[-5] == 0x8A) {
            g_plOff = *(uint32_t *)(q + 4); g_plAltOff = *(uint32_t *)(q - 4);
        }
        if (!g_plMax && q[0] == 0x83 && q[1] == 0xFA) g_plMax = q[2];
        if (!g_dipStride && q[0] == 0x0F && q[1] == 0xAF && q[2] == 0x05) g_dipStride = (int32_t *)(q + 7 + *(int32_t *)(q + 3));
        if (g_dipStride && !g_dipTable && q[0] == 0x48 && q[1] == 0x8B && q[2] == 0x05) g_dipTable = (int32_t **)(q + 7 + *(int32_t *)(q + 3));
    }
    if (!g_plOff || !g_plAltOff || g_plMax < 2 || g_plMax > 64 || !g_dipStride || !g_dipTable) { logf_("ally contact: relation function layout not recognised"); return 0; }
    /* the pair function: two R calls, each "xor r8d,r8d; mov rdx,r; mov rcx,r; call R; cmp eax,2; je" */
    /* (one other function has a single such call) */
    uint8_t *sites[48]; uint32_t fs[48]; int n = calls_to(R, sites, 48), k = 0, m = 0; uint32_t f0 = 0;
    for (int i = 0; i < n && i < 48; i++) {
        uint8_t *c = sites[i];
        if (c[-9] != 0x45 || c[-8] != 0x33 || c[-7] != 0xC0 || c[5] != 0x83 || c[6] != 0xF8 || c[7] != 0x02 || c[8] != 0x0F || c[9] != 0x84) continue;
        uint32_t f = func_of(c);
        if (f) fs[k++] = f;
    }
    for (int i = 0; i < k; i++) {
        int c2 = 0;
        for (int j = 0; j < k; j++) c2 += fs[j] == fs[i];
        if (c2 == 2 && fs[i] != f0) { f0 = fs[i]; m++; }
    }
    if (m != 1) { logf_("ally contact: contact pair function not found (%d candidates)", m); return 0; }
    g_pairFn = g_base + f0;
    /* the manual-control test in it: call X with X = cmp dword [rcx+off],0; setne al; ret */
    /* ... called as mov rcx,[reg+CHASSIS]; call X */
    g_chManualOff = g_pairChassisOff = 0;
    for (uint8_t *q = g_pairFn; q < g_pairFn + 0x800 && !g_chManualOff; q++) {
        if (q[0] != 0xE8) continue;
        uint8_t *t = q + 5 + *(int32_t *)(q + 1);
        if (in_text(t) && t[0] == 0x83 && t[1] == 0x79 && t[3] == 0 && t[4] == 0x0F && t[5] == 0x95 && t[6] == 0xC0 && t[7] == 0xC3 &&
            (q[-7] & 0xFB) == 0x48 && q[-6] == 0x8B && (q[-5] & 0xF8) == 0x88 && q[-5] != 0x8C) {
            g_chManualOff = t[2]; g_pairChassisOff = *(uint32_t *)(q - 4);
        }
    }
    if (!g_chManualOff || g_pairChassisOff < 0x100 || g_pairChassisOff > 0x4000) { logf_("ally contact: manual-control test not found"); return 0; }
    n = calls_to(g_pairFn, &g_pairSite, 1);
    if (n != 1) { logf_("ally contact: contact pair function has %d callers (expected 1)", n); return 0; }
    logf_("ally contact: pair function exe+0x%llX (call @ exe+0x%llX), relation exe+0x%llX (player +0x%X/+0x%X, %u players), entity->chassis +0x%X, manual +0x%X",
          EXE_OFF(g_pairFn), EXE_OFF(g_pairSite), EXE_OFF(R), g_plOff, g_plAltOff, g_plMax, g_pairChassisOff, g_chManualOff);
    return 1;
}

#ifndef FX_TEST
static volatile DWORD g_pushTid; static uint8_t *volatile g_pushFauna;   /* the fauna whose collision step is running */
static const char *ent_kind(const uint8_t *e)
{
    uint8_t fl = e[ENT_FLAGS];
    return (fl & 4) ? "soldier" : !(fl & 2) ? "object" : is_fauna(e) ? "fauna" : "vehicle";
}
static uint8_t fx_vol_hit(void *ctx, void *shape, void *probe, void *xform, void *out)
{
    uint8_t *fauna = g_pushFauna, *owner = NULL;
    if (fauna && xform && g_pushTid == GetCurrentThreadId()) {
        owner = *(uint8_t **)((uint8_t *)xform - g_volXformOff + VOL_OWNER);
        if (owner && owner != fauna && !((uintptr_t)owner & 7)) {
            uint8_t fl = owner[ENT_FLAGS];
            int human = (fl & 4) != 0, object = !(fl & 2);         /* bit2 human; no bit1 = not an actor (no chassis):
                                                                     fence, crate, tree... (as the engine's contact code tells them) */
            if (human || (object && g_contactObjects)) {
                static int logged; if (logged < 8) { logged++; logf_("contact: fauna %p in direct control walks into %s %p (no push-back)", fauna, human ? "soldier" : "object", owner); }
                return 0;
            }
        } else owner = NULL;
    }
    uint8_t r = g_volHitOrig(ctx, shape, probe, xform, out);
    if (r && owner) { static int logged; if (logged < 8) { logged++; logf_("contact: fauna %p in direct control pushed back by %s %p (flags 0x%02X)", fauna, ent_kind(owner), owner, owner[ENT_FLAGS]); } }
    return r;
}
static uint8_t fx_class_by_name(void *name)
{
    uint8_t *e = g_pushFauna;
    if (e && g_pushTid == GetCurrentThreadId()) {
        uint8_t c = e[g_entPatherOff];
        static int logged; if (logged < 2) { logged++; logf_("contact: fauna %p in direct control - collision step uses its own pather class %u (soldier class %u)", e, c, g_classOrig(name)); }
        return c;
    }
    return g_classOrig(name);
}
static void *fx_push_step(void *ch, void *dir, void *target, void *out, uint64_t a5, uint64_t a6)
{
    uint8_t *e = foot_fauna(ch);
    if (!e || !*(int32_t *)((uint8_t *)ch + g_footManualOff)) return g_pushStepOrig(ch, dir, target, out, a5, a6);
    if (g_dcPather == DCP_OFF) { ((float *)dir)[0] = ((float *)dir)[1] = 0.0f; return dir; }  /* no push-back step (like AI movement) */
    uint8_t *prevF = g_pushFauna; DWORD prevT = g_pushTid;                 /* (not expected to nest) */
    g_pushTid = GetCurrentThreadId(); g_pushFauna = e;
    void *r = g_pushStepOrig(ch, dir, target, out, a5, a6);
    g_pushFauna = prevF; g_pushTid = prevT;
    return r;
}
static int install_contact(void)
{
    int n = 0;
    if (g_dcPather != DCP_OFF) {
        void *orig = NULL;
        if (!redirect_call(g_volHitSite, (void *)fx_vol_hit, &orig, "contact (volume test)")) return 0;
        g_volHitOrig = (VolHitFn)orig;
        if (g_dcPather == DCP_OWN) {
            void *o = NULL;
            if (g_classSite && redirect_call(g_classSite, (void *)fx_class_by_name, &o, "contact (step pather class)")) g_classOrig = (ClassByNameFn)o;
            else { g_dcPather = DCP_HUMAN; logf_("contact: the step keeps the soldier pather class"); }
        }
    }
    for (int i = 0; i < g_pushSiteN; i++) {
        void *o = NULL;
        if (redirect_call(g_pushSites[i], (void *)fx_push_step, &o, "contact (collision step)")) { g_pushStepOrig = (PushStepFn)o; n++; }
    }
    return n > 0;
}

/* AI fauna vs allies */
static int chassis_manual(const uint8_t *e)
{
    const uint8_t *ch = *(const uint8_t *const *)(e + g_pairChassisOff);
    return ch && *(const int32_t *)(ch + g_chManualOff) != 0;
}
static int relation(const uint8_t *a, const uint8_t *b)       /* the engine's R(a, b, 0): 2 = friendly */
{
    unsigned pa = a[g_plOff], pb = b[g_plAltOff];
    if (pa == pb || pa == b[g_plOff]) return 2;
    pb = b[g_plOff];
    if (pa >= g_plMax || pb >= g_plMax) return 0;
    const int32_t *t = *g_dipTable;
    return t ? t[pb * (unsigned)*g_dipStride + pa] : 0;
}
static int ally_skip(uint8_t *a, uint8_t *b)                   /* a: an AI-controlled fauna; b: an allied actor not driven by the player */
{
    if (!(a[ENT_FLAGS] & 2) || !(b[ENT_FLAGS] & 2) || !is_fauna(a) || chassis_manual(a) || chassis_manual(b)) return 0;
    if (g_allyVehicles && !(b[ENT_FLAGS] & 4) && !is_fauna(b)) return 0;        /* ini: allied vehicles still contacted */
    return relation(a, b) == 2;
}
static void fx_pair(void *pair)
{
    uint8_t *a = pair ? ((uint8_t **)pair)[0] : NULL, *b = pair ? ((uint8_t **)pair)[1] : NULL;
    if (a && b) {
        int ab = ally_skip(a, b);
        if (ab || ally_skip(b, a)) {
            uint8_t *f = ab ? a : b, *o = ab ? b : a;
            static int logged; if (logged < 8) { logged++; logf_("ally contact: AI fauna %p - contact with allied %s %p skipped", f, ent_kind(o), o); }
            return;
        }
        static int dcLogged;                                              /* diagnostics: what a controlled fauna touches */
        if (dcLogged < 8) {
            uint8_t *f = is_fauna(a) && chassis_manual(a) ? a : is_fauna(b) && chassis_manual(b) ? b : NULL;
            if (f) { dcLogged++; uint8_t *o = f == a ? b : a; logf_("contact: fauna %p in direct control touches %s %p", f, ent_kind(o), o); }
        }
    }
    g_pairOrig(pair);
}
static int install_ally_filter(void)
{
    void *o = NULL;
    if (!redirect_call(g_pairSite, (void *)fx_pair, &o, "ally contact (contact pair)")) return 0;
    g_pairOrig = (PairFn)o;
    return 1;
}
#endif /* FX_TEST */
