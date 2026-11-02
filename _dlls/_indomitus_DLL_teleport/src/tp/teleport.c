/* GEM Teleport - the {teleport} instruction, its sub-order, the lock and the hooks
 * Part of the unity build: #included by gem_teleport.c, not compiled on its own.
 *
 * {teleport {max_radius M} {pre_use_delay S} {post_use_delay S}}   (metres, seconds, seconds)
 *
 * execute(): the soldier joins the teleporting set, {on "teleport_begin"} fires and a TpOrder sub-order is installed in
 * the custom action order (engine SetSubOrder). The engine ticks it: after pre_use_delay (eSingleOrder's own ms
 * countdown, simulation time, so every multiplayer peer agrees) the soldier is moved with Entity::SetPos,
 * {on "teleport_jump"} fires, and after post_use_delay the order ends; its Finish takes the soldier out of the set and
 * fires {on "teleport_end"}.
 * While in the set: Entity::GiveOrder refuses new orders (hook, lock_orders) and the health damage actions skip the
 * unit (vtable hooks, invulnerable). The two are independent.
 * The eCustom IsApplicable hook refuses a ground click farther than max_radius from the soldier (forbidden cursor,
 * and the running order fails the same way); an entity click is already refused by {target ground}.
 */

/* ------------------------------------------------------------------------------------ */
/* instruction object                                                                   */
/* ------------------------------------------------------------------------------------ */
typedef struct TpInstr {
    void **vt;
    uint8_t nocheck, pad[7];                 /* +8: written by the engine's parser ("nocheck") */
    float maxRadius, preS, postS;            /* metres, seconds, seconds */
    uint32_t magic;
} TpInstr;
#define TP_MAGIC 0x54504C54u                 /* 'TLPT' */
static void *g_instrVtStorage[5]; static void **const g_instrVt = g_instrVtStorage + 1;   /* [-1] = RTTI locator */
static struct { const char *name; void *(*ctor)(void); } g_instrEntry;
static void **g_instrNode;
static float g_defRadius = 30.0f, g_defPre = 0.5f, g_defPost = 0.5f;
static int g_fireEvents = 1, g_wantLock = 1, g_wantInvuln = 1, g_okEvents, g_okLock, g_okInvuln;
static volatile int g_instrInstalled;

/* ------------------------------------------------------------------------------------ */
/* teleporting set: units between the click and the release (read by the order and damage hooks) */
/* ------------------------------------------------------------------------------------ */
#define TP_MAXLOCK 64
static void *volatile g_locked[TP_MAXLOCK];
static int tp_is_locked(const void *e)
{
    if (!e) return 0;
    for (int i = 0; i < TP_MAXLOCK; i++) if (g_locked[i] == e) return 1;
    return 0;
}
static void tp_lock(void *e)
{
    if (tp_is_locked(e)) return;
    for (int i = 0; i < TP_MAXLOCK; i++) if (!g_locked[i]) { g_locked[i] = e; return; }
    logf_("teleport: more than %d units teleporting at once - %p is not locked nor invulnerable", TP_MAXLOCK, e);
}
static void tp_unlock(const void *e) { for (int i = 0; i < TP_MAXLOCK; i++) if (g_locked[i] == e) g_locked[i] = NULL; }

/* ------------------------------------------------------------------------------------ */
/* named events                                                                         */
/* ------------------------------------------------------------------------------------ */
#ifndef FX_TEST
static int g_evLogged;
static void fire_named(void *entity, const char *name)
{
    uint8_t *e = (uint8_t *)entity;
    if (!g_okEvents || !g_fireEvents || !e || !(e[ENT_FLAGS] & 2)) return;
    uint8_t *def = *(uint8_t **)(e + g_entDefOff);
    if (!def || !*(void **)(def + g_defSetOff)) return;      /* no compiled interaction set -> FireNamed would fault */
    __attribute__((aligned(16))) uint8_t ctx[0x400];
    __attribute__((aligned(16))) uint8_t tag[0x80];
    memset(ctx, 0, sizeof ctx);
    g_ctxCtor(ctx, entity);
    g_tagCtorCStr(tag, name);
    g_fireNamed(ctx, tag);
    g_tagDtor(tag);
    g_ctxDtor(ctx);
    if (g_verbose && g_evLogged < 100) { logf_("event \"%s\" -> entity %p", name, entity); if (++g_evLogged == 100) logf_("(further events not logged)"); }
}

/* ------------------------------------------------------------------------------------ */
/* the sub-order                                                                        */
/* ------------------------------------------------------------------------------------ */
typedef struct TpOrder {
    uint8_t base[ORD_SIZE];                  /* eSingleOrder: vptr, actor, link, flags, result, wait, retry, sub */
    uint8_t *entity;
    float pos[3];
    int32_t preMs, postMs;
    int32_t state;                           /* 0 new, 1 waiting to jump, 2 landed */
    int32_t active;                          /* in the teleporting set; cleared by the release */
    uint32_t magic;
} TpOrder;
static void *g_orderVtStorage[21]; static void **const g_orderVt = g_orderVtStorage + 1;   /* [-1] = RTTI locator, 0..19 */
typedef void *(*OrderDtorFn)(void *o, unsigned flags);
static OrderDtorFn g_singleOrderDtor;
static int g_tpLogged;

static void tp_order_release(TpOrder *o, int fire)
{
    if (!o->active) return;
    o->active = 0;
    tp_unlock(o->entity);
    if (fire) fire_named(o->entity, "teleport_end");
}
/* eSingleOrder::Tick counts +0x24 down by the simulation quant while bit 20 is set, then calls Update(this, 1) */
static void tp_order_wait(TpOrder *o, int32_t ms)
{
    *(int32_t *)(o->base + ORD_WAIT) = ms;
    *(uint32_t *)(o->base + ORD_FLAGS) |= ORD_WAITING;
}
static int tp_entity_ok(const uint8_t *e) { return e && (e[ENT_FLAGS] & 2) && (e[ENT_FLAGS] & 4); }

/* slot 19: Update(this, phase) -> 0 running, 1 done, 2 failed */
static int tp_order_update(TpOrder *o, int phase)
{
    if (phase == 2) { tp_order_release(o, 1); return 0; }
    if (!tp_entity_ok(o->entity)) { if (g_verbose) logf_("teleport: unit %p gone before the jump - cancelled", o->entity); return 2; }
    if (phase == 0) {                                        /* first tick: arm the pre-delay */
        o->state = 1;
        if (o->preMs > 0) { tp_order_wait(o, o->preMs); return 0; }
    }
    if (o->state == 1) {
        float *p = (float *)(o->entity + ENT_POS);
        if (g_verbose && g_tpLogged < 50) logf_("teleport: unit %p (%.1f %.1f %.1f) -> (%.1f %.1f %.1f)", o->entity, p[0], p[1], p[2], o->pos[0], o->pos[1], o->pos[2]);
        g_setPos(o->entity, o->pos, 0);
        fire_named(o->entity, "teleport_jump");
        o->state = 2;
        if (g_tpLogged < 50) g_tpLogged++;
        if (o->postMs > 0) { tp_order_wait(o, o->postMs); return 0; }
    }
    return 1;
}
/* slot 0: deleting dtor(this, flags) */
static void *tp_order_dtor(TpOrder *o, unsigned flags)
{
    tp_order_release(o, 0);                                  /* safety: a destroyed order never leaves a lock behind (no event: the unit may be gone) */
    g_singleOrderDtor(o, 0);                                 /* engine base: drops a sub-order, resets the vptr */
    if (flags & 1) g_engineDelete(o, sizeof *o);
    return o;
}

/* ------------------------------------------------------------------------------------ */
/* instruction vtable                                                                   */
/* ------------------------------------------------------------------------------------ */
static void *tp_instr_dtor(TpInstr *i, unsigned flags) { if (flags & 1) g_engineDelete(i, sizeof *i); return i; }
static uint8_t tp_instr_ready(TpInstr *i) { (void)i; return 1; }

static void tp_instr_parse(TpInstr *i, void *reader)
{
    int n = 0;
    while (g_rdPeek(reader, 4, 0) && n++ < 16) {
        g_rdEnter(reader);
        const MsString *w = (const MsString *)g_rdWord(reader, 1);
        const char *s = w ? ms_cstr(w) : "";
        float *dst = NULL;
        if (!_stricmp(s, "max_radius")) dst = &i->maxRadius;
        else if (!_stricmp(s, "pre_use_delay")) dst = &i->preS;
        else if (!_stricmp(s, "post_use_delay")) dst = &i->postS;
        else logf_("{teleport}: unknown parameter \"%s\" (max_radius, pre_use_delay, post_use_delay)", s);
        if (g_rdPeek(reader, 9, 0)) { float v = g_rdFloat(reader); if (dst) *dst = v; }
        else if (dst) logf_("{teleport}: \"%s\" needs a number", s);
        g_rdLeave(reader);
    }
    if (!(i->maxRadius > 0.0f) || i->maxRadius > 100000.0f) i->maxRadius = g_defRadius;
    if (!(i->preS >= 0.0f) || i->preS > 600.0f) i->preS = g_defPre;
    if (!(i->postS >= 0.0f) || i->postS > 600.0f) i->postS = g_defPost;
    logf_("{teleport} parsed: max_radius %g m, pre_use_delay %g s, post_use_delay %g s", i->maxRadius, i->preS, i->postS);
}

static float tp_dist2d(const uint8_t *entity, const float *tgt)
{
    const float *p = (const float *)(entity + ENT_POS);
    float dx = p[0] - tgt[0], dy = p[1] - tgt[1];
    return sqrtf(dx * dx + dy * dy);
}

/* slot 3: execute(this, action, order, entity, target, flagF0) -> bool */
static uint8_t tp_instr_execute(TpInstr *i, uint8_t *action, uint8_t *order, uint8_t *entity, uint8_t *target, uint8_t *f0)
{
    (void)action; (void)f0;
    if (!order || !target || !tp_entity_ok(entity)) { logf_("{teleport}: not a living human - action failed"); return 0; }
    if (*(int32_t *)(target + TGT_TYPE) != TGT_GROUND) { logf_("{teleport}: target is not a ground point (add {target ground} to the action) - action failed"); return 0; }
    const float *tp = (const float *)(target + TGT_POS);
    float d = tp_dist2d(entity, tp), lim = i->maxRadius * *g_worldScale;
    if (d > lim) { if (g_verbose) logf_("{teleport}: target %.1f units away, limit %.1f - action failed", d, lim); return 0; }
    TpOrder *o = (TpOrder *)g_engineNew(sizeof *o);
    if (!o) return 0;
    memset(o, 0, sizeof *o);
    *(void ***)o->base = g_orderVt;                          /* eSingleOrder ctor state = all zero; SetSubOrder sets +8 */
    o->entity = entity; o->pos[0] = tp[0]; o->pos[1] = tp[1]; o->pos[2] = tp[2];
    o->preMs = (int32_t)(i->preS * 1000.0f + 0.5f); o->postMs = (int32_t)(i->postS * 1000.0f + 0.5f);
    o->magic = TP_MAGIC; o->active = 1;
    tp_lock(entity);
    if (g_verbose) logf_("{teleport}: unit %p -> (%.1f %.1f %.1f), %.1f units (limit %.1f), wait %d ms / %d ms",
                         entity, tp[0], tp[1], tp[2], d, lim, o->preMs, o->postMs);
    g_setSubOrder(order, o);                                 /* finishes and deletes a previous sub-order, links ours */
    fire_named(entity, "teleport_begin");
    return 1;
}

static void *tp_instr_ctor(void)
{
    TpInstr *i = (TpInstr *)g_engineNew(sizeof *i);
    if (!i) return NULL;
    memset(i, 0, sizeof *i);
    i->vt = g_instrVt; i->maxRadius = g_defRadius; i->preS = g_defPre; i->postS = g_defPost; i->magic = TP_MAGIC;
    return i;
}

/* ------------------------------------------------------------------------------------ */
/* hooks                                                                                */
/* ------------------------------------------------------------------------------------ */
static IsApplicableFn g_isApplicableOrig;
static TpInstr *tp_find_instr(const uint8_t *action)
{
    if (!action) return NULL;
    TpInstr **b = *(TpInstr ***)(action + g_instrBegOff), **e = *(TpInstr ***)(action + g_instrEndOff);
    if (!b || e < b || e - b > 256) return NULL;
    for (; b < e; b++) if (*b && (*b)->vt == g_instrVt && (*b)->magic == TP_MAGIC) return *b;
    return NULL;
}
static uint8_t hook_is_applicable(uint8_t *action, uint8_t *flag, uint8_t *entity, uint8_t *target)
{
    uint8_t r = g_isApplicableOrig(action, flag, entity, target);
    if (!r) return 0;
    TpInstr *i = tp_find_instr(action);
    if (!i) return r;
    if (tp_is_locked(entity)) return r;                      /* the running order re-checks every tick: keep it going */
    if (!entity || !target || *(int32_t *)(target + TGT_TYPE) != TGT_GROUND) return 0;
    return tp_dist2d(entity, (const float *)(target + TGT_POS)) <= i->maxRadius * *g_worldScale;
}

static GiveOrderFn g_giveOrderOrig; static int g_rejectLogged;
static uint64_t hook_give_order(uint8_t *entity, void **order, void *ctx)
{
    if (order && tp_is_locked(entity)) {
        if (g_verbose && g_rejectLogged < 50) { logf_("lock: order refused for teleporting unit %p", entity); g_rejectLogged++; }
        ((void (*)(void *))((void **)order[0])[4])(order);               /* Finish */
        ((void (*)(void *, unsigned))((void **)order[0])[0])(order, 1);  /* delete */
        return 0;
    }
    return g_giveOrderOrig(entity, order, ctx);
}

static int g_dmgLogged;
static int tp_damage_skip(void *entity)
{
    if (!entity || !tp_is_locked(entity)) return 0;
    if (g_verbose && g_dmgLogged < 50) { logf_("invulnerable: damage ignored for teleporting unit %p", entity); g_dmgLogged++; }
    return 1;
}
#define DMG_HOOK(k) static void hook_damage_##k(void *a, void *ctx, void *e) { if (!tp_damage_skip(e)) g_damageExec[k](a, ctx, e); }
DMG_HOOK(0) DMG_HOOK(1) DMG_HOOK(2) DMG_HOOK(3) DMG_HOOK(4) DMG_HOOK(5)
static void *const g_dmgHooks[TP_NDMG] = { (void *)hook_damage_0, (void *)hook_damage_1, (void *)hook_damage_2,
                                           (void *)hook_damage_3, (void *)hook_damage_4, (void *)hook_damage_5 };

static void install_hooks(void)
{
    /* vtables for our objects: the instruction (4 slots) and the sub-order (eSingleOrder clone with 0 and 19 replaced;
       eSingleOrder's Start, slot 16, is an empty 'ret' and is kept) */
    g_instrVt[0] = (void *)tp_instr_dtor; g_instrVt[1] = (void *)tp_instr_parse; g_instrVt[2] = (void *)tp_instr_ready; g_instrVt[3] = (void *)tp_instr_execute;
    /* RTTI locators so a typeid / dynamic_cast on our objects reads a real one (an engine instruction's, eSingleOrder's) */
    void **vtSimple = find_vtable_rtti(".?AUeInstruction_disable_action_restart@Action@Interface@@");
    g_instrVt[-1] = vtSimple ? vtSimple[-1] : NULL;
    memcpy(g_orderVt, g_vtSingleOrder, 20 * sizeof(void *));
    g_orderVt[-1] = g_vtSingleOrder[-1];
    g_singleOrderDtor = (OrderDtorFn)g_vtSingleOrder[0];
    g_orderVt[0] = (void *)tp_order_dtor; g_orderVt[19] = (void *)tp_order_update;
    g_instrEntry.name = "teleport"; g_instrEntry.ctor = tp_instr_ctor;
    /* radius / ground check on the cursor and on the running order */
    int okApp = vt_hook(g_vtCustomAction, 15, (void *)hook_is_applicable, (void **)&g_isApplicableOrig);
    logf_("radius / ground check          %s", okApp ? "ON (eCustom IsApplicable)" : "OFF (vtable not writable) - the instruction still checks the radius");
    if (g_wantLock) {
        g_okLock = install_jmp5((uint8_t *)g_giveOrder, (void *)hook_give_order, (void **)&g_giveOrderOrig, "Entity::GiveOrder");
        logf_("orders refused while teleporting %s", g_okLock ? "ON" : "OFF (hook failed)");
    } else logf_("orders refused while teleporting off (ini)");
    if (g_wantInvuln) {
        int n = 0;
        for (int i = 0; i < g_nDamage; i++) { void *orig; if (vt_hook(g_vtDamage[i], 9, g_dmgHooks[g_dmgExecIdx[i]], &orig)) n++; }
        g_okInvuln = n == g_nDamage;
        logf_("invulnerable while teleporting  %s (%d/%d damage classes)", g_okInvuln ? "ON" : "PARTIAL", n, g_nDamage);
    } else logf_("invulnerable while teleporting  off (ini)");
}

/* ------------------------------------------------------------------------------------ */
/* registry insertion (same rules as ScriptExt's condition registry)                    */
/* ------------------------------------------------------------------------------------ */
static int rd_mem(const void *src, void *dst, size_t n)
{
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), src, dst, n, &got) && got == n;
}
static int instr_registry_built(void) { int32_t g = *g_instrGuard; return g != 0 && g != -1; }
static int instr_insert(void **node)
{
    void *old;
    do {
        old = *(void *volatile *)g_instrHead;
        if (!old) return 0;
        node[0] = old; MemoryBarrier();
    } while (InterlockedCompareExchangePointer((void *volatile *)g_instrHead, node, old) != old);
    return 1;
}
static int instr_listed(void **node)
{
    void **n = *(void **volatile *)g_instrHead;
    if (!n) return -1;
    for (int i = 0; n && i < 4096; i++) {
        if (n == node) return 1;
        if (!rd_mem(n, &n, sizeof n)) return -1;
    }
    return 0;
}
static void install_instruction(void)
{
    void **node = (void **)g_engineNew(0x10);                /* the engine frees every node at exit with its own allocator */
    if (!node) { logf_("{teleport}: engine allocation failed"); g_instrInstalled = 1; return; }
    node[1] = &g_instrEntry;
    if (!instr_insert(node)) { logf_("{teleport}: registry is empty - not registered"); g_instrInstalled = 1; return; }
    g_instrNode = node;
    logf_("{teleport} instruction         ON (registered)");
    g_instrInstalled = 1;
}
static int check_instruction(void)
{
    if (!g_instrNode) return 0;
    int st = instr_listed(g_instrNode);
    if (st < 0) return 0;
    if (st == 1) return 1;
    if (!instr_insert(g_instrNode)) return 0;
    logf_("{teleport}: lost to a concurrent engine registration - registered again");
    return 1;
}
#endif /* FX_TEST */
