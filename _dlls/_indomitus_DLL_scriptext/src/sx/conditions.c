/* GEM ScriptExt - added {if} conditions
 * Part of the unity build: #included by gem_scriptext.c, not compiled on its own.
 * {if have_enemy}, {if health}, {if player}, {if team} registered in the engine condition registry, plus the
 * quoted {if "name"} spelling (have_enemy only) through the script-flag condition.
 */

#ifndef SX_TEST
/* ------------------------------------------------------------------------------------ */
/* {if have_enemy}                                                                      */
/* ------------------------------------------------------------------------------------ */
static int entity_sees_enemy(const uint8_t *e)
{
    if (!e || !(e[ENT_FLAGS] & 2)) return 0;
    const uint8_t *sensor = *(const uint8_t *const *)(e + g_sensorOff);
    if (!sensor || !*(const void *const *)(sensor + g_sensorDummyOff)) return 0;
    for (uint32_t k = 0; k < g_sensorListCount; k++) {
        const uint8_t *const *b = *(const uint8_t *const *const *)(sensor + g_sensorListOff + k * 0x18);
        const uint8_t *const *en = *(const uint8_t *const *const *)(sensor + g_sensorListOff + k * 0x18 + 8);
        if (!b || en < b || en - b > 4096) continue;
        for (; b < en; b++) {
            const uint8_t *rec = *b;
            if (!rec) continue;
            uint32_t flags = *(const uint32_t *)(rec + 0x14);
            if ((flags & 3) == 3 && *(const void *const *)(rec + 0x18)) return 1;
        }
    }
    return 0;
}

/* ---- generic condition objects: one vtable per added keyword, eval(entity) callback ---- */
/* cond = our 16-byte object (NULL on the quoted-flag path); e = ctx[0], the unit the script runs on (inside {with_effector}
   the effector: the engine swaps ctx[0] and ctx[1]); ctx = the interaction context */
typedef int (*SxEvalFn)(void *cond, uint8_t *e, void **ctx);
typedef struct { void **vt; } Cond;
typedef struct SxCond {
    const char *name; SxEvalFn eval; int enabled; void (*parse)(void *cond, void *reader); int noFlag;
    void *vt[3];                                          /* engine condition vtable: dtor, eval, parse */
    struct { const char *name; void *(*ctor)(void); } entry;
    void **node;                                          /* {next, entry*} allocated with the engine's operator new */
    void *flagVt[3];                                      /* clone of the script-flag vtable with our eval */
    int logged;
} SxCond;

static int  eval_have_enemy(void *c, uint8_t *e, void **ctx) { (void)c; (void)ctx; return entity_sees_enemy(e); }
static void *cond_dtor(Cond *c, unsigned flags) { if (flags & 1) g_engineDelete(c, 16); return c; }
static void  cond_parse(Cond *c, void *reader) { (void)c; (void)reader; }

/* Conditions that take an argument keep it in the object's 8 payload bytes ({vt, payload}, 16 bytes, engine heap) and
   have no quoted spelling: {if health 0.6} float, {if player 4|ally|neutral|enemy} int + mode, {if team 2} name
   (health.c, teams.c) */
#define SX_NCOND 4
#define COND_HAVE_ENEMY 0
#define COND_HEALTH     1
#define COND_PLAYER     2
#define COND_TEAM       3
/* enabled starts at 0 for all: init sets it only after the engine code each one needs was found
   (0.5.0 started have_enemy at 1, so it was registered with zero sensor offsets when find_sensor failed) */
static SxCond g_conds[SX_NCOND] = {
    { "have_enemy", eval_have_enemy, 0, NULL,         0 },
    { "health",     eval_health,     0, parse_health, 1 },
    { "player",     eval_player,     0, parse_player, 1 },
    { "team",       eval_team,       0, parse_team,   1 },
};

#define COND_FNS(i) \
    static int   cond_eval_##i(Cond *c, void **ctx) { uint8_t *e = ctx ? (uint8_t *)ctx[0] : NULL; int r = g_conds[i].eval(c, e, ctx); \
        if (g_verbose && !g_conds[i].logged++) logf_("{if %s} evaluated for the first time (entity %p -> %d)", g_conds[i].name, e, r); return r; } \
    static void *cond_ctor_##i(void) { Cond *c = g_engineNew(16); if (c) { c->vt = g_conds[i].vt; *(uint64_t *)((uint8_t *)c + 8) = 0; } return c; } \
    static int   flag_eval_##i(void *c, void **ctx) { (void)c; return g_conds[i].eval(NULL, ctx ? (uint8_t *)ctx[0] : NULL, ctx); }
COND_FNS(0) COND_FNS(1) COND_FNS(2) COND_FNS(3)
static void *(*const g_condCtors[SX_NCOND])(void) = { cond_ctor_0, cond_ctor_1, cond_ctor_2, cond_ctor_3 };
static void *const g_condEvals[SX_NCOND] = { (void *)cond_eval_0, (void *)cond_eval_1, (void *)cond_eval_2, (void *)cond_eval_3 };
static void *const g_flagEvals[SX_NCOND] = { (void *)flag_eval_0, (void *)flag_eval_1, (void *)flag_eval_2, (void *)flag_eval_3 };
static volatile int g_condInstalled;

/* The engine fills the registry from its static initialisers (dozens of them, on the main thread, while our watch
   thread already runs), each with a plain 'node->next = head; head = node'. So: the watch thread waits until the
   registry static is constructed and its head has stopped changing, prepends with a compare-exchange (never overwrites
   an engine insert), and for 2 s re-checks that an engine insert racing ours did not drop our node. Registrations only
   ever add, so a NULL head after that means the registry is being destroyed (process exit): then nothing is touched. */
static int cond_registry_built(void)
{
    int32_t g = *g_condGuard;                            /* MSVC thread-safe static guard: 0 = not yet, -1 = in progress, else = epoch (done) */
    return g != 0 && g != -1;
}
/* 0 = the list is empty (being destroyed): not inserted */
static int cond_insert(void **node)
{
    void *old;
    do {
        old = *(void *volatile *)g_condHead;
        if (!old) return 0;
        node[0] = old; MemoryBarrier();
    } while (InterlockedCompareExchangePointer((void *volatile *)g_condHead, node, old) != old);
    return 1;
}
/* 1 = node is in the list, 0 = not, -1 = the list is empty or unreadable (being destroyed). Faults cannot happen: the
   walk reads through rd_mem, since a node may be freed under it at exit. */
static int cond_listed(void **node)
{
    void **n = *(void **volatile *)g_condHead;
    if (!n) return -1;
    for (int i = 0; n && i < 4096; i++) {
        if (n == node) return 1;
        if (!rd_mem(n, &n, sizeof n)) return -1;
    }
    return 0;
}
static void install_conditions(void)
{
    for (int i = SX_NCOND - 1; i >= 0; i--) {
        SxCond *c = &g_conds[i];
        if (!c->enabled) continue;
        c->vt[0] = (void *)cond_dtor; c->vt[1] = g_condEvals[i]; c->vt[2] = c->parse ? (void *)c->parse : (void *)cond_parse;
        c->entry.name = c->name; c->entry.ctor = g_condCtors[i];
        void **node = g_engineNew(0x10);                 /* the engine deletes every node at exit with its own allocator */
        if (!node) { logf_("{if %s}: engine allocation failed", c->name); continue; }
        node[1] = &c->entry;
        if (!cond_insert(node)) { logf_("{if %s}: registry is empty - not registered", c->name); continue; }
        c->node = node;
        logf_("{if %s}%*s ON (registered)", c->name, (int)(20 - strlen(c->name)), "");
    }
    g_condInstalled = 1;
}
/* the 2 s re-check after install; 0 = stop (the registry is being destroyed) */
static int check_conditions(void)
{
    for (int i = 0; i < SX_NCOND; i++) {
        SxCond *c = &g_conds[i];
        if (!c->node) continue;
        int st = cond_listed(c->node);
        if (st < 0) return 0;
        if (st == 1) continue;
        if (!cond_insert(c->node)) return 0;
        logf_("{if %s}: lost to a concurrent engine registration - registered again", c->name);
    }
    return 1;
}

/* --- {if "name"}: the quoted (script flag) spelling, bound to the same evals --- */
typedef struct { void **vt; int32_t index; } FlagCond;

static void flag_parse_hook(FlagCond *c, void *reader)
{
    g_flagParseOrig(c, reader);
    if (!c || c->index < 0 || c->index > 1024) return;
    uint8_t *reg = g_flagRegistry();
    if (!reg) return;
    uint8_t *b = *(uint8_t **)(reg + g_flagBegOff), *e = *(uint8_t **)(reg + g_flagBegOff + 8);
    uint8_t *tag = b + (size_t)c->index * 0x60;
    if (!b || tag + 0x60 > e) return;
    const MsString *nm = (const MsString *)(tag + 0x40);      /* Tag: MSVC std::string at +0x40 (lower-cased) */
    const char *s = ms_cstr(nm);
    for (int i = 0; i < SX_NCOND; i++) {
        SxCond *k = &g_conds[i]; size_t L = strlen(k->name);
        if (!k->enabled || k->noFlag || nm->size != L || memcmp(s, k->name, L)) continue;
        c->vt = k->flagVt;
        if (!k->logged) logf_("{if \"%s\"} parsed (flag #%d) - bound to ScriptExt", k->name, c->index);
        return;
    }
}

static int install_flag_cond(void)
{
    for (int i = 0; i < SX_NCOND; i++) { g_conds[i].flagVt[0] = g_flagVt[0]; g_conds[i].flagVt[1] = g_flagEvals[i]; g_conds[i].flagVt[2] = g_flagVt[2]; }
    DWORD old;
    if (!VirtualProtect(g_flagVt + 2, 8, PAGE_READWRITE, &old)) { logf_("{if \"...\"}: VirtualProtect failed"); return 0; }
    g_flagVt[2] = (void *)flag_parse_hook;
    VirtualProtect(g_flagVt + 2, 8, old, &old);
    return 1;
}
#endif /* SX_TEST */
