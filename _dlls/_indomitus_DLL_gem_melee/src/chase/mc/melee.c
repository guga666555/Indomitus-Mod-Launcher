/* GEM MeleeChase - melee chase logic
 * Part of the unity build: #included by gem_meleechase.c, not compiled on its own.
 *
 * HUMANS: nothing (0.5.0). The fight-distance multiplier (two Human::eOutdoorsBattle vtable slots + the eAttack const
 *   read) is removed: infantry melee belongs to GEM MeleeStance (stance, draw, charge, blow reach), and two DLLs moving
 *   the same hand-fight distance worked against each other. The "fight/distance" readers are still LOCATED, read-only:
 *   they are how find_weapons() reaches GetCurrentWeapon, the weapon list and Tag(const char*), which the vehicle part
 *   needs. Every hook below skips human entities.
 *
 * VEHICLES / FAUNA (approach)
 *   The vehicle brain (Actor::eVS_Root) has an "approach" behaviour: when its target is out of weapon range it
 *   gives itself ONE move order (Order::eMoveTask) into a circle of 0.95 x weapon range around the target's
 *   position at that moment. While that order runs the state does not run again, so a moving target is chased
 *   to where it WAS, and only then re-approached. For a unit with a melee-tagged weapon GEM MeleeChase:
 *   - re-plans the running approach when the point it drives to is off by a fraction of the remaining distance;
 *   - for a melee-reach approach, drives to contact_factor x reach (not the edge) and aims ahead of the target;
 *   - keeps pursuing a target that backs off inside weapon range (vanilla waits until it leaves range);
 *   - stops only when the target is in reach and not getting away. smooth_repath: re-plans don't brake.
 *   ai_prioritize_melee / ai_prioritize_ranged replace the approach range the brain computed: entity tags first (0.5.4,
 *   script can switch them at runtime), then def props.
 *
 * Simulation: it changes AI decisions. Every multiplayer peer needs the DLL (like GEM Fauna's squads).
 */

/* ------------------------------------------------------------------------------------ */
/* engine layout (found at startup)                                                     */
/* ------------------------------------------------------------------------------------ */
#define ENT_FLAGS    0x30        /* byte: bit1 alive / in scene, bit2 human, bit3 vehicle class */
#define ENT_POS      0x74        /* float x, y, z (world units) */
#define WORLD_SCALE  20.0f       /* world units per metre */
#define TAG_SIZE     0x60
#define TAG_NAME     0x40        /* MSVC std::string inside a Tag */

typedef struct { union { char buf[16]; char *ptr; } u; size_t size, cap; } MsString;

/* weapons: entity +o_weaponry -> weaponry; GetCurrentWeapon(weaponry) -> weapon in hands (or first weapon);
   weaponry +o_wlistBeg/End = vector<weapon*>; weapon +o_weapItem -> item entity; item +o_itemTagBeg/End = vector<Tag> */
typedef void *(*GetWeaponFn)(void *weaponry);
static GetWeaponFn g_getWeapon;
static uint32_t o_weaponry, o_wlistBeg, o_wlistEnd, o_weapItem, o_itemTagBeg, o_itemTagEnd;
static int g_okWeapons;

/* weapon finder (find_weapons): walks the human battle state's "fight/distance" readers to reach the weapon getter.
   Read-only since 0.5.0: nothing here is hooked any more. */
typedef void  (*TagCtorCStrFn)(void *tag, const char *s);
typedef float (*ConstGetFn)(void *brain, void *tag, float def);         /* brain const (metres) x world scale */
typedef uint64_t (*SlotFn)(void *self, uint64_t a, uint64_t b, uint64_t c);
static TagCtorCStrFn g_tagCtorCStr;
static ConstGetFn g_constGet;
static uint8_t *g_fdStr, *g_fdSiteState, *g_fdSiteAttack;               /* "fight/distance"; its two readers' getter calls */
static uint32_t o_fightOff;                                             /* eOutdoorsBattle: cached fight distance */
static uint32_t o_hStateBrain, o_hBrainEntity;                          /* state +x -> brain, brain +x -> entity */
static void **g_vtBattle;                                               /* Human::eOutdoorsBattle */
#define MAX_HSLOTS 4
static int g_hSlot[MAX_HSLOTS], g_hSlotN;

/* vehicles */
typedef void *(*MoveCircleFn)(void *circle, uint64_t flags);
typedef uint64_t (*UpdateFn)(void *order, uint64_t phase, uint64_t a, uint64_t b);
typedef void (*FinishSubFn)(void *order);
static void **g_vtVsRoot, **g_vtMoveTask;
static SlotFn g_vsTickOrig;
static MoveCircleFn g_moveCircle, g_moveCircleOrig;
static UpdateFn g_mtUpdateOrig;
typedef void *(*DtorFn)(void *self, uint64_t flags);
static DtorFn g_mtDtorOrig;
static FinishSubFn g_finishSub;                                          /* eMoveTask phase 2 minus the chassis stop */
static uint8_t *g_apSite;                                               /* "approach": call eMoveTask(circle) */
static uint32_t o_vStateBrain, o_vBrainEntity, o_vMode, o_vRecBeg, o_vRecEnd, o_recX, o_recY, o_aiOrder;
static volatile uint32_t *g_simTime;                                    /* simulation time, ms */
static int g_okVehicle;

/* weapon priority props (vehicles / fauna): override the approach range the vehicle brain computed */
typedef float (*WeaponRangeFn)(void *weapon);                           /* world units */
static WeaponRangeFn g_weaponRange;
static uint32_t o_approach;                                             /* brain: approach range (world units), -1 = none */
static uint32_t o_recEnt;                                               /* target record -> target entity (compared only) */
static uint32_t o_entDef, o_defPropBeg, o_defPropEnd;                   /* entity -> def; def {props} = vector<Tag> */
static char g_propMelee[48] = "ai_prioritize_melee", g_propRanged[48] = "ai_prioritize_ranged";
static int g_okPriority, g_wantPriority = 1;
/* 0.5.4: the same two names as ENTITY tags ({tags add "ai_prioritize_melee"} from script) override the def props.
   Precedence: tag melee, tag ranged, prop melee, prop ranged. o_entTags = entity +x -> vector<Tag> (begin, end at +8);
   0 = not found: def props only. */
static uint32_t o_entTags;

/* hold ranged fire in melee: the vehicle target evaluation hands each weapon its target with Weapon::SetTarget(weapon,
   eTarget*, a, mode, b); an eTarget of type 0 clears it (SetTarget then calls Clear(weapon, 1)) */
typedef uint64_t (*SetTargetFn)(void *weapon, void *desc, uint64_t a, uint64_t mode, uint64_t b);
typedef uint64_t (*ClearTargetFn)(void *weapon, uint64_t how);
typedef uint64_t (*EvalFn)(void *brain, uint64_t a, uint64_t b, uint64_t c);
static SetTargetFn g_setTarget;
static ClearTargetFn g_clearTarget;
static uint8_t *g_evalFn, *g_evalJump;                                  /* target evaluation; the one jump/call into it */
#define MAX_ST_SITES 8
static uint8_t *g_stSites[MAX_ST_SITES]; static int g_stSiteN;
static uint32_t o_weapOwner;                                            /* weapon -> owner entity */
static int g_okHold, g_wantHold = 1;
static float g_holdFactor = 1.0f;

/* settings */
static char  g_meleeTag[48] = "melee"; static size_t g_meleeTagLen = 5;
/* 0.5.1: second melee word for vehicle weapons. The engine gives the word "melee" a meaning of its own on weapons (it applies
   the human Melee skill; a "pattern gun" vehicle weapon tagged melee crashed the server at spawn), so vehicle guns carry this one */
static char  g_meleeTag2[48] = "veh_melee"; static size_t g_meleeTag2Len = 9;
/* re-plan threshold = clamp(distance x fraction, min, max); checks every repathMs, or repathMsClose within closeRange */
static float g_repathFrac = 0.25f, g_repathMin = 1.0f * WORLD_SCALE, g_repathMax = 10.0f * WORLD_SCALE;
static uint32_t g_repathMs = 500, g_repathMsClose = 200;
static float g_closeRange = 10.0f * WORLD_SCALE;
/* close combat: goal radius = melee reach x contact; aim lead_seconds ahead (at most lead_max); pursue a target backing
   off faster than g_recede even inside weapon range */
static float g_contact = 0.5f, g_leadSec = 0.6f, g_leadMax = 6.0f * WORLD_SCALE, g_recede = 0.3f * WORLD_SCALE;
static int   g_pursue = 1, g_okClose;
/* chase_always: a melee unit (melee-only, or ai_prioritize_melee) pursues whenever its target is outside 90% of its
   reach, not only when the target backs off; chase_run_mode: movement mode set on the chassis for a chase (-1 = keep) */
static int   g_chaseAlways = 1, g_chaseMode = 2;
static uint32_t o_chassis, o_modeSlot;                                   /* entity -> chassis; chassis vtable byte offset of SetMovementMode */
static int   g_smooth = 1, g_logChase = 1;

/* ------------------------------------------------------------------------------------ */
/* helpers                                                                              */
/* ------------------------------------------------------------------------------------ */
static const char *ms_cstr(const MsString *s) { return s->cap > 15 ? s->u.ptr : s->u.buf; }

static int rd_u32(const uint8_t *p) { return *(const int32_t *)p; }
static uint8_t *rel32_target(uint8_t *insn, int len) { return insn + len + rd_u32(insn + len - 4); }

/* Tags are interned: two Tags are equal when their first qword (the id) is equal (the engine's Tag== at exe+0x556810).
   The ids of the melee tag and the two priority props are built once, lazily on the first engine call that needs them
   (engine thread, after CRT init), with the engine's Tag(const char*) ctor. Until then / without the ctor: name compare. */
static uint64_t g_idMelee, g_idMelee2, g_idPropMelee, g_idPropRanged;
static volatile LONG g_idState;                                         /* 0 none, 1 building, 2 ready, 3 unavailable */
static TagCtorCStrFn g_tagCtorForIds;                                   /* set by find_weapons (Tag::Tag(const char*)) */
static int ids_ready(void)
{
#ifdef FX_TEST
    return 0;
#else
    if (g_idState == 2) return 1;
    if (g_idState != 0) return 0;
    if (InterlockedCompareExchange(&g_idState, 1, 0) != 0) return 0;
    if (!g_tagCtorForIds) { g_idState = 3; return 0; }
    static uint8_t buf[4][TAG_SIZE * 2] __attribute__((aligned(16)));   /* never freed */
    const char *names[4] = { g_meleeTag, g_propMelee, g_propRanged, g_meleeTag2 };
    uint64_t *ids[4] = { &g_idMelee, &g_idPropMelee, &g_idPropRanged, &g_idMelee2 };
    for (int i = 0; i < 4; i++) {
        if (!names[i][0]) { *ids[i] = 0; continue; }                       /* second melee word switched off */
        char low[48]; size_t k = 0;
        for (; names[i][k] && k < sizeof low - 1; k++) low[k] = (char)((names[i][k] >= 'A' && names[i][k] <= 'Z') ? names[i][k] + 32 : names[i][k]);
        low[k] = 0;
        g_tagCtorForIds(buf[i], low);
        *ids[i] = *(uint64_t *)buf[i];
    }
    MemoryBarrier();
    g_idState = 2;
    return 1;
#endif
}

/* safety: for the first checks, the id result is compared with the name compare; any disagreement turns ids off */
static volatile LONG g_idChecks, g_idPos;                               /* checks done / of which positive (both agreed on a hit) */
#define IDS_TRUSTED (g_idChecks > 400 && g_idPos > 5)
static int id_verify(int byId, int byName, const char *what)
{
    if (byId != byName) {
        g_idState = 3;
#ifndef FX_TEST
        logf_("tag ids disagree with names for \"%s\" - using name compares", what);
#endif
        return byName;
    }
#ifndef FX_TEST
    if (byId) InterlockedIncrement(&g_idPos);
#endif
    return byId;
}

/* does the tag vector [beg,end) contain the melee tag */
static int tags_have_melee(uint8_t *beg, uint8_t *end)
{
    if (!beg || end < beg || (size_t)(end - beg) > TAG_SIZE * 256) return 0;
    int byId = -1;
    if (ids_ready()) {
        byId = 0;
        for (uint8_t *t = beg; t + TAG_SIZE <= end; t += TAG_SIZE)
            if (*(uint64_t *)t == g_idMelee || (g_meleeTag2Len && *(uint64_t *)t == g_idMelee2)) { byId = 1; break; }
        if (IDS_TRUSTED) return byId;
    }
    int byName = 0;
    for (uint8_t *t = beg; t + TAG_SIZE <= end && !byName; t += TAG_SIZE) {
        const MsString *s = (const MsString *)(t + TAG_NAME);
        if (s->size == g_meleeTagLen && !_strnicmp(ms_cstr(s), g_meleeTag, g_meleeTagLen)) byName = 1;
        else if (g_meleeTag2Len && s->size == g_meleeTag2Len && !_strnicmp(ms_cstr(s), g_meleeTag2, g_meleeTag2Len)) byName = 1;
    }
    if (byId < 0) return byName;
#ifndef FX_TEST
    InterlockedIncrement(&g_idChecks);
#endif
    return id_verify(byId, byName, g_meleeTag);
}
static int weapon_is_melee(uint8_t *w)
{
    if (!w) return 0;
    uint8_t *item = *(uint8_t **)(w + o_weapItem);
    if (!item) return 0;
    return tags_have_melee(*(uint8_t **)(item + o_itemTagBeg), *(uint8_t **)(item + o_itemTagEnd));
}
/* which of the two priority names a Tag vector [b,en) holds (case-insensitive names, or the interned ids once trusted):
   bit 1 = prop_prioritize_melee, bit 2 = prop_prioritize_ranged. Used for the def {props} and (0.5.4) the entity tags. */
#define PRIO_MELEE  1
#define PRIO_RANGED 2
static int tags_priority_bits_by_name(uint8_t *b, uint8_t *en)  /* caller checked [b,en) */
{
    size_t nm = strlen(g_propMelee), nr = strlen(g_propRanged);
    int bits = 0;
    for (uint8_t *t = b; t + TAG_SIZE <= en; t += TAG_SIZE) {
        const MsString *s = (const MsString *)(t + TAG_NAME);
        if (nm && s->size == nm && !_strnicmp(ms_cstr(s), g_propMelee, nm)) bits |= PRIO_MELEE;
        else if (nr && s->size == nr && !_strnicmp(ms_cstr(s), g_propRanged, nr)) bits |= PRIO_RANGED;
    }
    return bits;
}
static int tags_priority_bits(uint8_t *b, uint8_t *en, size_t maxTags)
{
    if (!b || en < b || (size_t)(en - b) > TAG_SIZE * maxTags) return 0;
    size_t nm = strlen(g_propMelee), nr = strlen(g_propRanged);
    int byId = -1;
    if (ids_ready()) {
        byId = 0;
        for (uint8_t *t = b; t + TAG_SIZE <= en; t += TAG_SIZE) {
            uint64_t id = *(uint64_t *)t;
            if (nm && id == g_idPropMelee) byId |= PRIO_MELEE;
            else if (nr && id == g_idPropRanged) byId |= PRIO_RANGED;
        }
        if (IDS_TRUSTED) return byId;
    }
    int byName = tags_priority_bits_by_name(b, en);
    if (byId < 0) return byName;
#ifndef FX_TEST
    InterlockedIncrement(&g_idChecks);
#endif
    return id_verify(byId, byName, "ai priority props / tags");
}
static int def_priority_bits(uint8_t *e)
{
    uint8_t *def = *(uint8_t **)(e + o_entDef);
    return def ? tags_priority_bits(*(uint8_t **)(def + o_defPropBeg), *(uint8_t **)(def + o_defPropEnd), 256) : 0;
}
/* log only: the same answer by name compare alone. No ids_ready / id_verify and no g_idChecks / g_idPos, so logging
   (a per-machine ini choice) never moves the id-trust state the decision path reads. */
static int def_priority_bits_quiet(uint8_t *e)
{
    uint8_t *def = *(uint8_t **)(e + o_entDef);
    if (!def) return 0;
    uint8_t *b = *(uint8_t **)(def + o_defPropBeg), *en = *(uint8_t **)(def + o_defPropEnd);
    if (!b || en < b || (size_t)(en - b) > TAG_SIZE * 256) return 0;
    return tags_priority_bits_by_name(b, en);
}
static int bits_to_priority(int bits) { return (bits & PRIO_MELEE) ? 2 : (bits & PRIO_RANGED) ? 1 : 0; }


/* ------------------------------------------------------------------------------------ */
/* finders                                                                              */
/* ------------------------------------------------------------------------------------ */

/* weapon -> item -> item tags, as read by the AI's "can take this pose" test (held item tagged cannot_lie):
   mov rax,[rax+ITEM] ; mov rdi,[rax+TAGS_END] ; mov rbx,[rax+TAGS_BEG]  (TAGS_END = TAGS_BEG + 8) */
static int find_weapon_layout(void)
{
    static const uint8_t pat[] = { 0x48, 0x8B, 0x80 };
    int hits = 0;
    for (uint8_t *p = g_text.p; (p = find_bytes(p, g_text.p + g_text.n - p - 24, pat, 3)); p++) {
        if (p[5] || p[6] || p[7] != 0x48 || p[8] != 0x8B || p[9] != 0xB8 || p[14] != 0x48 || p[15] != 0x8B || p[16] != 0x98) continue;
        uint32_t item = *(uint32_t *)(p + 3), end = *(uint32_t *)(p + 10), beg = *(uint32_t *)(p + 17);
        if (item > 0x200 || end != beg + 8 || beg < 0x400 || beg > 0x4000) continue;
        if (!hits) { o_weapItem = item; o_itemTagBeg = beg; o_itemTagEnd = end; }
        else if (item != o_weapItem || beg != o_itemTagBeg) { logf_("weapons: held-item tag reads disagree"); return 0; }
        hits++;
    }
    if (!hits) { logf_("weapons: held-item tag read not found"); return 0; }
    logf_("weapons: weapon +0x%X -> item, item tags +0x%X..+0x%X (%d site(s))", o_weapItem, o_itemTagBeg, o_itemTagEnd, hits);
    return 1;
}

/* the two "fight/distance" readers: lea rdx,str ; ... call Tag::Tag(cstr) ; ... call ConstGet(brain, tag, def) */
static int fd_site(uint8_t *lea, uint8_t **ctor, uint8_t **getSite, uint8_t **getFn)
{
    uint8_t *s1, *s2;
    *ctor = next_call(lea + 7, 0x20, &s1);
    if (!*ctor) return 0;
    *getFn = next_call(s1 + 5, 0x40, &s2);
    if (!*getFn) return 0;
    *getSite = s2;
    return 1;
}

/* 0.5.0: was find_human. Locates (never patches) the human battle state's "fight/distance" readers, because one of those
   slots is the shortest known road to GetCurrentWeapon, the entity's weaponry offset and the weapon list; also the
   Tag(const char*) ctor used for the interned tag ids. The log lines keep their "human:" prefix. */
static int find_weapons(void)
{
    g_fdStr = find_cstr("fight/distance");
    if (!g_fdStr) { logf_("human: \"fight/distance\" not found"); return 0; }
    uint8_t *sites[4]; int n = 0;
    for (int k = 0; k < 4 && n < 4; k++) { uint8_t *l = find_lea_to(g_fdStr, k); if (!l) break; if (l[0] == 0x48 && l[1] == 0x8D && l[2] == 0x15) sites[n++] = l; }
    if (n != 2) { logf_("human: expected 2 readers of \"fight/distance\", found %d", n); return 0; }
    uint8_t *ctor[2], *gs[2], *gf[2];
    for (int i = 0; i < 2; i++) if (!fd_site(sites[i], &ctor[i], &gs[i], &gf[i])) { logf_("human: reader %d not recognised", i); return 0; }
    if (gf[0] != gf[1] || ctor[0] != ctor[1]) { logf_("human: the two readers call different functions"); return 0; }
    g_constGet = (ConstGetFn)gf[0]; g_tagCtorCStr = (TagCtorCStrFn)ctor[0]; g_tagCtorForIds = g_tagCtorCStr;
    /* the state's reader stores the result: movss [reg+disp32],xmm0 within 0x20 bytes after the call */
    int stateIdx = -1;
    for (int i = 0; i < 2 && stateIdx < 0; i++)
        for (uint8_t *q = gs[i] + 5; q < gs[i] + 0x20; q++)
            if (q[0] == 0xF3 && q[1] == 0x0F && q[2] == 0x11 && (q[3] & 0xC0) == 0x80 && (q[3] & 7) != 4) {
                o_fightOff = *(uint32_t *)(q + 4); stateIdx = i; break;
            }
    if (stateIdx < 0 || o_fightOff < 0x100 || o_fightOff > 0x2000) { logf_("human: cached fight distance field not found"); return 0; }
    g_fdSiteState = gs[stateIdx]; g_fdSiteAttack = gs[1 - stateIdx];

    g_vtBattle = find_vtable_rtti(".?AUeOutdoorsBattle@Human@@");
    if (!g_vtBattle) { logf_("human: Human::eOutdoorsBattle vtable not found"); return 0; }
    /* slots whose function reads the cached field: movss xmm,[reg+off] (optional REX 41..47), one pass over .text */
    uint32_t fns[12]; int nfn = 0;
    while (nfn < 12 && in_text(g_vtBattle[nfn])) { fns[nfn] = (uint32_t)((uint8_t *)g_vtBattle[nfn] - g_base); nfn++; }
    int hit[12] = { 0 };
    for (uint8_t *p = g_text.p; (p = find_bytes(p, g_text.p + g_text.n - p - 8, (const uint8_t *)"\xF3", 1)); p++) {
        uint8_t *q = p + 1; if ((q[0] & 0xF0) == 0x40) q++;
        if (q[0] != 0x0F || q[1] != 0x10 || (q[2] & 0xC0) != 0x80 || (q[2] & 7) == 4) continue;
        if (*(uint32_t *)(q + 3) != o_fightOff) continue;
        uint32_t f = func_of(p);
        for (int s = 0; s < nfn; s++) if (fns[s] == f) hit[s] = 1;
    }
    g_hSlotN = 0;
    for (int s = 0; s < nfn; s++)
        if (hit[s]) { if (g_hSlotN >= MAX_HSLOTS) { logf_("human: too many slots read the field"); return 0; } g_hSlot[g_hSlotN++] = s; }
    if (g_hSlotN < 1) { logf_("human: no eOutdoorsBattle slot reads the field"); return 0; }

    /* in one of those slots: mov rcx,[rcx+B] (state->brain) ... mov rcx,[rcx+E] (brain->entity) ... mov rcx,[rcx+W] ; call GetCurrentWeapon */
    for (int i = 0; i < g_hSlotN && !g_getWeapon; i++) {
        uint8_t *f = (uint8_t *)g_vtBattle[g_hSlot[i]];
        uint32_t b = 0, e = 0;
        for (uint8_t *q = f; q < f + 0x80; q++) {
            if (q[0] == 0x48 && q[1] == 0x8B && q[2] == 0x49) { if (!b) b = q[3]; else if (!e) e = q[3]; }
            if (q[0] == 0x48 && q[1] == 0x8B && q[2] == 0x89 && q[7] == 0xE8 && b && e) {
                o_hStateBrain = b; o_hBrainEntity = e; o_weaponry = *(uint32_t *)(q + 3);
                g_getWeapon = (GetWeaponFn)rel32_target(q + 7, 5); break;
            }
        }
    }
    if (!g_getWeapon || !in_text((void *)g_getWeapon) || o_weaponry < 0x400 || o_weaponry > 0x4000) { logf_("human: weapon getter not found"); return 0; }
    /* GetCurrentWeapon falls back to the first weapon of the list: mov rax,[rcx+L] ; cmp rax,[rcx+L+8] */
    for (uint8_t *q = (uint8_t *)g_getWeapon; q < (uint8_t *)g_getWeapon + 0x40; q++)
        if (q[0] == 0x48 && q[1] == 0x8B && q[2] == 0x41 && q[4] == 0x48 && q[5] == 0x3B && q[6] == 0x41 && q[7] == q[3] + 8) { o_wlistBeg = q[3]; o_wlistEnd = q[7]; break; }
    if (!o_wlistBeg) { logf_("human: weapon list not found in the weapon getter"); return 0; }
    logf_("human: fight distance field +0x%X, slots", o_fightOff);
    for (int i = 0; i < g_hSlotN; i++) logf_("  eOutdoorsBattle slot %d = exe+0x%llX", g_hSlot[i], EXE_OFF(g_vtBattle[g_hSlot[i]]));
    logf_("human: const getter exe+0x%llX, eAttack reader call exe+0x%llX, state->brain +0x%X, brain->entity +0x%X",
          EXE_OFF(g_constGet), EXE_OFF(g_fdSiteAttack), o_hStateBrain, o_hBrainEntity);
    logf_("weapons: entity +0x%X -> weaponry, current weapon exe+0x%llX, list +0x%X..+0x%X", o_weaponry, EXE_OFF(g_getWeapon), o_wlistBeg, o_wlistEnd);
    return 1;
}

static int find_vehicle(void)
{
    g_vtVsRoot = find_vtable_rtti(".?AUeVS_Root@Actor@@");
    g_vtMoveTask = find_vtable_rtti(".?AUeMoveTask@Order@@");
    void **vtSingle = find_vtable_rtti(".?AVeSingleOrder@@");
    if (!g_vtVsRoot || !g_vtMoveTask || !vtSingle) { logf_("vehicle: vtables not found (eVS_Root %p, eMoveTask %p, eSingleOrder %p)", (void *)g_vtVsRoot, (void *)g_vtMoveTask, (void *)vtSingle); return 0; }
    uint8_t *tick = (uint8_t *)g_vtVsRoot[3];
    if (!in_text(tick)) return 0;
    uint32_t tickRva = (uint32_t)(tick - g_base);

    /* tick head: mov rcx,[rcx+B] ; cmp dword [rcx+M],1 ; mov rax,[rcx+E] ; mov r12,[rcx+R] ; cmp r12,[rcx+R+8] */
    /* (the order of the entity load and the mode compare differs between the exes) */
    for (uint8_t *q = tick; q < tick + 0xC0; q++) {
        if (!o_vStateBrain && q[0] == 0x48 && q[1] == 0x8B && q[2] == 0x49) o_vStateBrain = q[3];
        if (!o_vMode && o_vStateBrain && q[0] == 0x83 && q[1] == 0xB9 && q[6] == 0x01) o_vMode = *(uint32_t *)(q + 2);
        if (!o_vBrainEntity && o_vStateBrain && q[0] == 0x48 && q[1] == 0x8B && q[2] == 0x41) o_vBrainEntity = q[3];
        if (!o_vRecBeg && q[0] == 0x4C && q[1] == 0x8B && q[2] == 0xA1 && q[7] == 0x4C && q[8] == 0x3B && q[9] == 0xA1 &&
            *(uint32_t *)(q + 10) == *(uint32_t *)(q + 3) + 8) { o_vRecBeg = *(uint32_t *)(q + 3); o_vRecEnd = o_vRecBeg + 8; }
    }
    if (!o_vStateBrain || !o_vMode || !o_vBrainEntity || !o_vRecBeg) { logf_("vehicle: tick head not recognised"); return 0; }

    /* the approach: movabs rax,"approach" in the tick, then the call that builds an eMoveTask from the circle */
    static const uint8_t ap[] = { 0x48, 0xB8, 'a', 'p', 'p', 'r', 'o', 'a', 'c', 'h' };
    uint8_t *mov = NULL;
    for (uint8_t *p = g_text.p; (p = find_bytes(p, g_text.p + g_text.n - p, ap, sizeof ap)); p++)
        if (func_of(p) == tickRva) { mov = p; break; }
    if (!mov) { logf_("vehicle: \"approach\" not found in the eVS_Root tick"); return 0; }
    for (uint8_t *q = mov + 10; q < mov + 0xA0 && !g_apSite; q++) {
        if (q[0] != 0xE8) continue;
        uint8_t *t = rel32_target(q, 5);
        if (!in_text(t)) continue;
        for (uint8_t *r = t; r < t + 0x40; r++)
            if (r[0] == 0x48 && r[1] == 0x8D && r[2] == 0x05 && r + 7 + rd_u32(r + 3) == (uint8_t *)g_vtMoveTask) { g_apSite = q; g_moveCircle = (MoveCircleFn)t; break; }
    }
    if (!g_apSite) { logf_("vehicle: approach move order call not found"); return 0; }
    /* right after: mov rcx,[rsi+B] ; cmp rax,[rcx+AI] (is it already the brain's AI order) */
    for (uint8_t *q = g_apSite + 5; q < g_apSite + 0x20; q++)
        if (q[0] == 0x48 && q[1] == 0x3B && q[2] == 0x81) { o_aiOrder = *(uint32_t *)(q + 3); break; }
    if (o_aiOrder < 0x40 || o_aiOrder > 0x400) { logf_("vehicle: AI order slot not found"); return 0; }
    /* target record position: movss xmm0,[r12+X] ; movss xmm1,[r12+Y] in the tick */
    for (uint8_t *p = tick; p < tick + 0x800 && !o_recX; p++) {
        if (p[0] != 0xF3 || p[1] != 0x41 || p[2] != 0x0F || p[3] != 0x10 || p[4] != 0x44 || p[5] != 0x24) continue;
        for (uint8_t *q = p + 7; q < p + 0x18; q++)
            if (q[0] == 0xF3 && q[1] == 0x41 && q[2] == 0x0F && q[3] == 0x10 && q[4] == 0x4C && q[5] == 0x24 && q[6] == p[6] + 4) { o_recX = p[6]; o_recY = q[6]; break; }
    }
    if (!o_recX) { logf_("vehicle: target record position not found"); return 0; }

    /* eMoveTask slot 0 = deleting dtor(this, flags): test bl,1 ; ... (hooked to forget deleted orders) */
    if (!in_text(g_vtMoveTask[0]) || !find_bytes((uint8_t *)g_vtMoveTask[0], 0x20, (const uint8_t *)"\xF6\xC3\x01", 3)) { logf_("vehicle: eMoveTask dtor not recognised"); return 0; }
    /* eMoveTask Update (slot 19): switch on phase 0/1/2; phase 2 = chassis stop, then FinishSub(order) */
    uint8_t *upd = (uint8_t *)g_vtMoveTask[19];
    static const uint8_t ph[] = { 0x85, 0xD2, 0x74 };
    uint8_t *sw = find_bytes(upd, 0x40, ph, 3);
    if (!sw || sw[4] != 0x83 || sw[5] != 0xEA || sw[6] != 0x01) { logf_("vehicle: eMoveTask Update not recognised"); return 0; }
    static const uint8_t fs[] = { 0x33, 0xD2, 0x48, 0x8B, 0x09, 0xE8 };
    uint8_t *f2 = find_bytes(upd, 0xA0, fs, sizeof fs);
    if (f2 && f2[10] == 0x48 && f2[11] == 0x8B && f2[12] == 0xCB && f2[13] == 0xE8) g_finishSub = (FinishSubFn)rel32_target(f2 + 13, 5);
    if (!g_finishSub) logf_("vehicle: eMoveTask phase-2 helper not found - smooth_repath off");

    /* simulation clock: eSingleOrder::Tick does mov eax,[rip+QUANT] ; sub [rbx+0x24],eax ; the clock is the dword before */
    uint8_t *st = (uint8_t *)vtSingle[3];
    for (uint8_t *q = st; q < st + 0x40; q++)
        if (q[0] == 0x8B && q[1] == 0x05 && q[6] == 0x29 && q[7] == 0x43) { g_simTime = (volatile uint32_t *)(rel32_target(q, 6) - 4); break; }
    if (!g_simTime || !in_data((void *)g_simTime)) { logf_("vehicle: simulation clock not found"); return 0; }
    /* cross-check: a getter 'mov eax,[rip+CLOCK] ; ret' exists */
    int getter = 0;
    for (uint8_t *p = g_text.p; (p = find_bytes(p, g_text.p + g_text.n - p - 8, (const uint8_t *)"\x8B\x05", 2)); p++)
        if (p[6] == 0xC3 && rel32_target(p, 6) == (uint8_t *)g_simTime) { getter = 1; break; }
    if (!getter) { logf_("vehicle: simulation clock not confirmed"); return 0; }

    logf_("vehicle: eVS_Root tick exe+0x%llX (state->brain +0x%X, mode +0x%X, brain->entity +0x%X, targets +0x%X, AI order +0x%X)",
          EXE_OFF(tick), o_vStateBrain, o_vMode, o_vBrainEntity, o_vRecBeg, o_aiOrder);
    logf_("vehicle: approach call exe+0x%llX -> eMoveTask(circle) exe+0x%llX, eMoveTask Update exe+0x%llX, record pos +0x%X/+0x%X, clock exe+0x%llX",
          EXE_OFF(g_apSite), EXE_OFF(g_moveCircle), EXE_OFF(upd), o_recX, o_recY, EXE_OFF(g_simTime));
    return 1;
}

/* weapon priority: the tick reads the approach range (movss xmm0,[rcx+A] ; comiss xmm0,xmm14 ; jb) then checks the target
   with mov rdx,r12 ; call T, and T reads the target's def props (mov rax,[rsi+DEF] ; mov rdi,[rax+END] ; mov rbx,[rax+BEG]).
   The target evaluation resets the range (mov dword [rcx+A],-1.0) and gets each weapon's range with
   mov rcx,rdi ; call RANGE ; mov rcx,[rsi+x] ; movaps xmm6,xmm0. */
static int find_priority(void)
{
    uint8_t *tick = (uint8_t *)g_vtVsRoot[3], *tagFn = NULL;
    for (uint8_t *q = tick; q < tick + 0x800 && !o_approach; q++)
        if (q[0] == 0xF3 && q[1] == 0x0F && q[2] == 0x10 && q[3] == 0x81 && q[8] == 0x41 && q[9] == 0x0F && q[10] == 0x2F)
            for (uint8_t *r = q + 11; r < q + 0x20; r++)
                if (r[0] == 0x49 && r[1] == 0x8B && r[2] == 0xD4 && r[3] == 0xE8) { o_approach = *(uint32_t *)(q + 4); tagFn = rel32_target(r + 3, 5); break; }
    if (!o_approach || !in_text(tagFn)) { logf_("priority: approach range read not found"); return 0; }
    /* T starts with mov rsi,[rdx+E] : the record's entity */
    for (uint8_t *q = tagFn; q < tagFn + 0x20 && !o_recEnt; q++) if (q[0] == 0x48 && q[1] == 0x8B && q[2] == 0x72) o_recEnt = q[3];
    if (!o_recEnt) { logf_("priority: target record entity offset not found"); return 0; }
    for (uint8_t *q = tagFn; q < tagFn + 0x40 && !o_entDef; q++)
        if (q[0] == 0x48 && q[1] == 0x8B && q[2] == 0x86 && q[7] == 0x48 && q[8] == 0x8B && q[9] == 0xB8 && q[14] == 0x48 && q[15] == 0x8B && q[16] == 0x98 &&
            *(uint32_t *)(q + 10) == *(uint32_t *)(q + 17) + 8) { o_entDef = *(uint32_t *)(q + 3); o_defPropBeg = *(uint32_t *)(q + 17); o_defPropEnd = *(uint32_t *)(q + 10); }
    if (!o_entDef) { logf_("priority: def props read not found"); return 0; }
    uint8_t reset[10] = { 0xC7, 0x81 }; memcpy(reset + 2, &o_approach, 4); reset[6] = 0; reset[7] = 0; reset[8] = 0x80; reset[9] = 0xBF;
    uint8_t *ev = find_bytes(g_text.p, g_text.n, reset, sizeof reset);
    if (!ev) { logf_("priority: target evaluation not found"); return 0; }
    /* (the order of 'mov rcx,[rsi+x]' and 'movaps xmm6,xmm0' differs between the exes); the range is then stored with
       movss [rsi+A],xmm6 within 0x80 bytes */
    for (uint8_t *q = ev; q < ev + 0x800 && !g_weaponRange; q++) {
        if (q[0] != 0x48 || q[1] != 0x8B || q[2] != 0xCF || q[3] != 0xE8) continue;
        int a = q[8] == 0x48 && q[9] == 0x8B && q[10] == 0x4E && q[12] == 0x0F && q[13] == 0x28 && q[14] == 0xF0;
        int b = q[8] == 0x0F && q[9] == 0x28 && q[10] == 0xF0 && q[11] == 0x48 && q[12] == 0x8B && q[13] == 0x4E;
        if (!a && !b) continue;
        uint8_t st[8] = { 0xF3, 0x0F, 0x11, 0xB6 }; memcpy(st + 4, &o_approach, 4);
        if (find_bytes(q, 0x80, st, 8)) g_weaponRange = (WeaponRangeFn)rel32_target(q + 3, 5);
    }
    if (!g_weaponRange || !in_text((void *)g_weaponRange)) { logf_("priority: weapon range getter not found"); return 0; }
    logf_("priority: approach range brain +0x%X, entity +0x%X -> def, props +0x%X..+0x%X, weapon range exe+0x%llX, record entity +0x%X",
          o_approach, o_entDef, o_defPropBeg, o_defPropEnd, EXE_OFF(g_weaponRange), o_recEnt);
    return 1;
}

/* the chassis' movement-mode setter, as Order::eMovementChange uses it (slot 19): mov edi,CHASSIS ... mov edx,[rbx+0x3c] ;
   ... call [rax+MODE] */
static int find_run_mode(void)
{
    void **vt = find_vtable_rtti(".?AUeMovementChange@Order@@");
    if (!vt || !in_text(vt[19])) { logf_("run mode: eMovementChange not found"); return 0; }
    uint8_t *f = (uint8_t *)vt[19];
    for (uint8_t *q = f; q < f + 0x30 && !o_chassis; q++)
        if (q[0] == 0xBF && *(uint32_t *)(q + 1) >= 0x800 && *(uint32_t *)(q + 1) < 0x2000) o_chassis = *(uint32_t *)(q + 1);
    uint8_t *m = find_bytes(f, 0x80, (const uint8_t *)"\x8B\x53\x3C", 3);
    if (m) for (uint8_t *q = m; q < m + 0x30 && !o_modeSlot; q++)
        if (q[0] == 0xFF && q[1] == 0x90 && *(uint32_t *)(q + 2) < 0x800 && !(*(uint32_t *)(q + 2) & 7)) o_modeSlot = *(uint32_t *)(q + 2);
    if (!o_chassis || !o_modeSlot) { logf_("run mode: chassis / mode setter not recognised"); return 0; }
    logf_("run mode: entity +0x%X -> chassis, SetMovementMode = chassis vtable +0x%X", o_chassis, o_modeSlot);
    return 1;
}

/* the target evaluation (function of the 'mov dword [rcx+A],-1.0' reset), its single caller (a tail jmp), and the
   Weapon::SetTarget calls inside it. SetTarget: cmp qword [rcx+OWNERITEM],0 near the start; cmp dword [rsi+8],0 ; jne ;
   mov edx,1 ; mov rcx,rbx ; call Clear. The owner entity: the weapon's 'mov r,[rcx+OWNER]' followed by
   'mov r,[r+WEAPONRY]' (the same weaponry offset the human finder found). */
static int find_hold(void)
{
    if (!o_approach || !o_weaponry) { logf_("hold: needs the priority and weapon finders"); return 0; }
    uint8_t reset[10] = { 0xC7, 0x81 }; memcpy(reset + 2, &o_approach, 4); reset[6] = 0; reset[7] = 0; reset[8] = 0x80; reset[9] = 0xBF;
    uint8_t *ev = find_bytes(g_text.p, g_text.n, reset, sizeof reset);
    uint32_t evRva = ev ? func_of(ev) : 0;
    if (!evRva) { logf_("hold: target evaluation not found"); return 0; }
    g_evalFn = g_base + evRva;
    int callers = 0;
    for (uint8_t *p = g_text.p; p + 5 < g_text.p + g_text.n; p++)
        if ((p[0] == 0xE8 || p[0] == 0xE9) && rel32_target(p, 5) == g_evalFn) { g_evalJump = p; callers++; }
    if (callers != 1) { logf_("hold: target evaluation has %d callers (expected 1)", callers); return 0; }
    /* SetTarget = the call target inside the evaluation that starts like Weapon::SetTarget */
    g_stSiteN = 0;
    for (uint8_t *p = g_evalFn; p < g_evalFn + 0x1400; p++) {
        if (p[0] != 0xE8 || func_of(p) != evRva) continue;
        uint8_t *t = rel32_target(p, 5);
        if (!in_text(t)) continue;
        if (!g_setTarget) {
            uint8_t *c = find_bytes(t, 0x30, (const uint8_t *)"\x48\x83\xB9", 3);
            uint8_t *z = NULL;                                          /* cmp dword [rDESC+8],0 ; jne (rsi or rbp) */
            for (uint8_t *q = t; q < t + 0x90 && !z; q++)
                if (q[0] == 0x83 && (q[1] & 0xF8) == 0x78 && q[2] == 0x08 && q[3] == 0x00 && q[4] == 0x75 && q[6] == 0xBA &&
                    *(uint32_t *)(q + 7) == 1 && q[11] == 0x48 && q[12] == 0x8B && q[13] == 0xCB && q[14] == 0xE8) z = q;
            if (!c || c[7] != 0x00 || !z) continue;
            g_setTarget = (SetTargetFn)t; g_clearTarget = (ClearTargetFn)rel32_target(z + 14, 5);
        }
        if (t == (uint8_t *)g_setTarget && g_stSiteN < MAX_ST_SITES) g_stSites[g_stSiteN++] = p;
    }
    if (!g_setTarget || !g_stSiteN) { logf_("hold: Weapon::SetTarget not found in the target evaluation"); return 0; }
    for (uint8_t *q = (uint8_t *)g_setTarget; q < (uint8_t *)g_setTarget + 0x60 && !o_weapOwner; q++)
        if (q[0] == 0x48 && q[1] == 0x8B && (q[2] & 0xC7) == 0x81 && *(uint32_t *)(q + 3) >= 0x40 && *(uint32_t *)(q + 3) < 0x200 &&
            q[7] == 0x48 && q[8] == 0x8B && (q[9] & 0xC0) == 0x80 && *(uint32_t *)(q + 10) == 0x568) o_weapOwner = *(uint32_t *)(q + 3);
    if (!o_weapOwner) {
        /* fallback: any 'mov rax,[r+OWN] ; mov r,[rax+WEAPONRY]' in the exe (weapon code reading its owner's weaponry) */
        for (uint8_t *p = g_text.p; p + 14 < g_text.p + g_text.n && !o_weapOwner; p++)
            if (p[0] == 0x48 && p[1] == 0x8B && (p[2] & 0xC7) == 0x81 && *(uint32_t *)(p + 3) >= 0x40 && *(uint32_t *)(p + 3) < 0x200 &&
                p[7] == 0x48 && p[8] == 0x8B && (p[9] & 0xC7) == 0x80 && *(uint32_t *)(p + 10) == o_weaponry) o_weapOwner = *(uint32_t *)(p + 3);
    }
    if (!o_weapOwner) { logf_("hold: weapon owner offset not found"); return 0; }
    logf_("hold: target evaluation exe+0x%llX (entered from exe+0x%llX), Weapon::SetTarget exe+0x%llX (%d site(s)), clear exe+0x%llX, weapon +0x%X -> owner",
          EXE_OFF(g_evalFn), EXE_OFF(g_evalJump), EXE_OFF(g_setTarget), g_stSiteN, EXE_OFF(g_clearTarget), o_weapOwner);
    return 1;
}

/* 0.5.4: entity tags (the vector {tags add/remove} changes), read the way the engine's own vehicle-AI test
   "has tag no_auto_repair" reads them:
     lea rdx,"no_auto_repair" ; lea rcx,[TAG] ; call Tag::Tag(const char*)          static init of the Tag (same ctor as
                                                                                     the one the interned ids are built with)
     mov rcx,[rax+BRAIN_ENTITY] ; add rcx,C ; call HasTag   with lea rdx,[TAG] just before the mov (game, server) or
                                                            just before the call (editor); BRAIN_ENTITY = o_vBrainEntity
     HasTag(container, Tag*): mov rX,[rcx+B+8] / mov rY,[rcx+B] ; ... call Tag== ; ... add rY,60h
     Tag== : mov rax,[rdx] ; cmp [rcx],rax ; sete al ; ret     (entity tags compare by interned id, like item / def tags)
   entity tags = entity + C + B (1.067-1.069: 0x1D0 + 0x318 = 0x4E8). Every site found must agree. tests/check_entity_tags.py
   runs the same patterns on the three exes and checks the result against the {tags} script action's container. */
static int find_entity_tags(void)
{
    static const uint8_t tagEq[] = { 0x48, 0x8B, 0x02, 0x48, 0x39, 0x01, 0x0F, 0x94, 0xC0, 0xC3 };
    if (!o_vBrainEntity || !g_tagCtorCStr) { logf_("entity tags: needs the vehicle and weapon finders"); return 0; }
    uint8_t *str = find_cstr("no_auto_repair");
    if (!str) { logf_("entity tags: \"no_auto_repair\" not found"); return 0; }
    uint8_t *tags[4]; int nt = 0;
    uint8_t *te = g_text.p + g_text.n;
    for (uint8_t *p = g_text.p; p + 19 <= te && nt < 4; p++)
        if (p[0] == 0x48 && p[1] == 0x8D && p[2] == 0x15 && p[7] == 0x48 && p[8] == 0x8D && p[9] == 0x0D && p[14] == 0xE8 &&
            rel32_target(p, 7) == str && rel32_target(p + 14, 5) == (uint8_t *)g_tagCtorCStr)
            tags[nt++] = rel32_target(p + 7, 7);
    if (!nt) { logf_("entity tags: static Tag \"no_auto_repair\" not found"); return 0; }
    uint32_t off = 0; int sites = 0;
    uint8_t *site0 = NULL, *has0 = NULL;
    for (uint8_t *p = g_text.p + 0x10; p + 0x30 <= te; p++) {
        if (p[0] != 0x48 || p[1] != 0x8D || p[2] != 0x15) continue;
        uint8_t *t = rel32_target(p, 7); int k = 0;
        while (k < nt && tags[k] != t) k++;
        if (k == nt) continue;
        uint8_t *mv = NULL;                                             /* mov rcx,[rax+E] ; add rcx,C */
        for (uint8_t *q = p - 0x10; q < p + 0x10 && !mv; q++)
            if (q[0] == 0x48 && q[1] == 0x8B && q[2] == 0x48 && q[4] == 0x48 && q[5] == 0x81 && q[6] == 0xC1) mv = q;
        if (!mv || mv[3] != o_vBrainEntity) continue;
        uint32_t c = *(uint32_t *)(mv + 7);
        uint8_t *from = mv + 11 > p + 7 ? mv + 11 : p + 7, *call = NULL;
        for (uint8_t *q = from; q < from + 8 && !call; q++) if (q[0] == 0xE8) call = q;
        if (!call) continue;
        uint8_t *ht = rel32_target(call, 5);
        if (!in_text(ht)) continue;
        uint32_t d[2]; int nd = 0;                                      /* the vector's begin / end loads */
        for (uint8_t *q = ht; q < ht + 0x20; q++)
            if (nd < 2 && q[0] == 0x48 && q[1] == 0x8B && (q[2] & 0xC7) == 0x81) d[nd++] = *(uint32_t *)(q + 3);
        if (nd != 2 || (d[0] != d[1] + 8 && d[1] != d[0] + 8)) continue;
        int stride = 0, idEq = 0;
        for (uint8_t *q = ht; q < ht + 0x40; q++) {
            if (q[0] == 0x48 && q[1] == 0x83 && (q[2] & 0xF8) == 0xC0 && q[3] == 0x60) stride = 1;
            if (q[0] == 0xE8) { uint8_t *eq = rel32_target(q, 5); if (in_text(eq) && !memcmp(eq, tagEq, sizeof tagEq)) idEq = 1; }
        }
        if (!stride || !idEq) continue;
        uint32_t o = c + (d[0] < d[1] ? d[0] : d[1]);
        if (o < 0x100 || o >= 0x2000) continue;
        if (sites && o != off) { logf_("entity tags: the no_auto_repair tests disagree (+0x%X, +0x%X)", off, o); return 0; }
        off = o; sites++;
        if (!site0) { site0 = p; has0 = ht; }
    }
    if (!sites) { logf_("entity tags: the no_auto_repair test (HasTag on the brain's entity) not recognised"); return 0; }
    o_entTags = off;
    logf_("entity tags: entity +0x%X (no_auto_repair test exe+0x%llX, HasTag exe+0x%llX, %d site(s))", o_entTags, EXE_OFF(site0), EXE_OFF(has0), sites);
    return 1;
}

#ifndef FX_TEST
/* ------------------------------------------------------------------------------------ */
/* vehicles: approach orders follow a moving target                                     */
/* ------------------------------------------------------------------------------------ */
typedef struct { void *order; uint8_t *ent, *brain; float cx, cy, r, reach; uint32_t next; uint8_t used, started, repath; } Chase;
#define MAX_CHASE 512
static Chase g_ch[MAX_CHASE];
static int g_chHigh;                                                   /* slots [0, g_chHigh) may be used */
static volatile LONG g_chLive;                                         /* number of used slots (fast path when 0) */
static CRITICAL_SECTION g_cs;                                          /* g_ch and g_tr */
static volatile LONG g_regLogged, g_repLogged, g_stopLogged, g_pursueLogged;
static _Thread_local void *t_apOrder;
static _Thread_local float t_apX, t_apY, t_apR;
/* set by vs_tick for the unit whose tick is running, read by ap_move */
static _Thread_local int t_chase, t_haveV;
static _Thread_local float t_mel, t_vx, t_vy;

static int ch_find(void *o) { for (int i = 0; i < g_chHigh; i++) if (g_ch[i].used && g_ch[i].order == o) return i; return -1; }
static void ch_remove(int i) { g_ch[i].used = 0; InterlockedDecrement(&g_chLive); while (g_chHigh && !g_ch[g_chHigh - 1].used) g_chHigh--; }

/* ---- target velocity: per chasing unit, the last sample of its target record (game time) ---- */
typedef struct { uint8_t *ent; void *tgt; float x, y, vx, vy; uint32_t t; } Track;
#define MAX_TRACK 1024
#define TRACK_STALE_MS 3000            /* a sample older than this is restarted with velocity 0 (track) */
static Track g_tr[MAX_TRACK];

/* ---- 0.5.2: both tables are emptied when a new mission starts ---- */
/* The chase table names orders, units and brains of the running scene. A slot is freed by the order's own stop step
   or destructor (mt_update / mt_dtor); an order that goes away with its whole scene runs neither, so its slot stayed
   used into the next mission. Two effects: g_chLive stayed above 0 for the rest of the session (every eMoveTask
   update of every unit then took the lock and searched the table), and a new order allocated at a stale slot's
   address was taken for the old one, with mt_update reading that slot's dead brain and unit. GEM MeleeStance needed
   the same reset in 0.7.31 (dedicated server crash right after a match ended).
   "New mission" = the simulation clock is not where the last hooked call left it: it stepped back (restart, next
   map, a save from earlier), or it jumped ahead by more than EPOCH_GAP_MS (a save from later). Within one mission
   some hooked call runs every quant while a chase is live, and a quant is well under 100 ms, so a live chase never
   sees such a gap. The rule only reads the simulation clock, so every machine of a network game resets on the same
   quant. Caller must NOT hold g_cs. */
#define EPOCH_GAP_MS 5000
static volatile uint32_t g_epochLast;                                  /* clock at the last check, 0 = none yet */
static volatile LONG g_epochLogged;
static int epoch_changed(uint32_t last, uint32_t now) { return last && (uint32_t)(now - last) > EPOCH_GAP_MS; }
static void mission_check(void)
{
    uint32_t now = *g_simTime, last = g_epochLast;
    if (epoch_changed(last, now)) {
        EnterCriticalSection(&g_cs);
        if (epoch_changed(g_epochLast, now)) {                         /* not done by another thread meanwhile */
            int live = (int)g_chLive;
            memset(g_ch, 0, sizeof g_ch); g_chHigh = 0; InterlockedExchange(&g_chLive, 0);
            memset(g_tr, 0, sizeof g_tr);
            g_epochLast = now ? now : 1;
            if (g_logChase && InterlockedIncrement(&g_epochLogged) <= 20)
                logf_("simulation clock %u -> %u ms: new mission, chase table cleared (%d slot(s) were still in use)", last, now, live);
        }
        LeaveCriticalSection(&g_cs);
        return;
    }
    g_epochLast = now ? now : 1;
}

/* caller holds g_cs. Velocity in world units per second, smoothed; 0 for a new target. Only floats are kept: the target
   pointer is compared, never dereferenced. */
static void track(uint8_t *e, uint8_t *rec, float *vx, float *vy)
{
    void *tgt = *(void **)(rec + o_recEnt);
    float x = *(float *)(rec + o_recX), y = *(float *)(rec + o_recY);
    uint32_t now = *g_simTime;
    size_t h = (size_t)(((uintptr_t)e >> 4) * 2654435761u) % MAX_TRACK;
    /* 0.5.3: which slot a unit gets depends on its heap address, which differs on every machine of a network game, so
       the RESULT must not depend on the slot. Before, a unit that found its 16 slots taken pushed out the oldest of
       them, even one still in use: that unit's target velocity restarted from 0 at random, on this machine only (seen
       in the offline replay from about 500 units chasing at once). Now: a unit's own slot is found wherever it is (slots are only
       emptied by mission_check, so the search ends at the first never-used one), and a new unit takes a never-used
       slot or one whose sample is older than TRACK_STALE_MS. Such a sample gives velocity 0 and is restarted below
       in any case, so taking it over changes nothing for its old owner. With more than MAX_TRACK fresh samples the
       unit gets no velocity; that depends only on how many there are, the same on every machine. */
    Track *t = NULL, *spare = NULL;
    for (int k = 0; k < MAX_TRACK; k++) {
        Track *c = &g_tr[(h + k) % MAX_TRACK];
        if (c->ent == e) { t = c; break; }
        if (!c->ent) { if (!spare) spare = c; break; }
        if (!spare && (uint32_t)(now - c->t) > TRACK_STALE_MS) spare = c;
    }
    if (!t) t = spare;
    if (!t) { *vx = *vy = 0; return; }
    if (t->ent != e || t->tgt != tgt) { t->ent = e; t->tgt = tgt; t->x = x; t->y = y; t->vx = t->vy = 0; t->t = now; }
    else {
        uint32_t dt = now - t->t;
        if (dt >= 100) {
            if (dt <= TRACK_STALE_MS) {
                float ix = (x - t->x) * 1000.0f / (float)dt, iy = (y - t->y) * 1000.0f / (float)dt;
                t->vx = 0.5f * t->vx + 0.5f * ix; t->vy = 0.5f * t->vy + 0.5f * iy;
            } else t->vx = t->vy = 0;
            t->x = x; t->y = y; t->t = now;
        }
    }
    *vx = t->vx; *vy = t->vy;
}

static float dist2d(float ax, float ay, float bx, float by) { float dx = ax - bx, dy = ay - by; return __builtin_sqrtf(dx * dx + dy * dy); }

/* gem_melee 0.1.10 (Alex: dreadnoughts walked up to a Baneblade and did not swing; ogryns hit a dreadnought only once it clipped
   into them): how far a target is for melee reach = to its centre, or for a vehicle-class target to the nearest of its volume
   centres (core_hull_point, set by the pen module), whichever is nearer. A wide hull keeps the centre out of a fist's reach.
   The record's entity is the one the brain's own tick reads in the same call; simulation state only, the same on every machine. */
static float tgt_dist(uint8_t *e, uint8_t *rec, float tx, float ty)
{
    float *p = (float *)(e + ENT_POS), d = dist2d(p[0], p[1], tx, ty);
    uint8_t *t = (core_hull_point && o_recEnt && rec) ? *(uint8_t **)(rec + o_recEnt) : NULL;
    if (t && t != e && (t[ENT_FLAGS] & 2) && !(t[ENT_FLAGS] & 4)) {
        float pt[3], h = core_hull_point(e, t, pt);
        if (h >= 0.0f && h < d) d = h;
    }
    return d;
}
/* gem_melee 0.1.13: the target of this record is a vehicle-class entity (vehicle, walker, fauna), not a human */
static int tgt_wide(uint8_t *rec)
{
    uint8_t *t = (o_recEnt && rec) ? *(uint8_t **)(rec + o_recEnt) : NULL;
    return t && (t[ENT_FLAGS] & 2) && !(t[ENT_FLAGS] & 4);
}

/* where to aim: the target's position plus lead_seconds of its velocity (at most lead_max_m) */
static void lead_point(float tx, float ty, float vx, float vy, float *px, float *py)
{
    float lx = vx * g_leadSec, ly = vy * g_leadSec, l = __builtin_sqrtf(lx * lx + ly * ly);
    if (l > g_leadMax && l > 0) { lx *= g_leadMax / l; ly *= g_leadMax / l; }
    *px = tx + lx; *py = ty + ly;
}

/* speed (world units/s) at which the target moves away from (x,y); negative = coming closer */
static float receding(float x, float y, float tx, float ty, float vx, float vy)
{
    float dx = tx - x, dy = ty - y, d = __builtin_sqrtf(dx * dx + dy * dy);
    return d > 0 ? (dx * vx + dy * vy) / d : 0;
}

/* melee reach (largest range among melee-tagged weapons) and ranged reach of a unit; -1 = has melee, range unknown */
static float unit_reach(uint8_t *e, float *ranged)
{
    *ranged = 0;
    uint8_t *wy = *(uint8_t **)(e + o_weaponry);
    if (!wy) return 0;
    uint8_t **wb = *(uint8_t ***)(wy + o_wlistBeg), **we = *(uint8_t ***)(wy + o_wlistEnd);
    if (!wb || we < wb || we - wb > 64) return 0;
    float mel = 0;
    for (; wb < we; wb++) {
        if (!*wb) continue;
        int m = weapon_is_melee(*wb);
        float r = g_weaponRange ? g_weaponRange(*wb) : -1.0f;
        if (r > 5000.0f * WORLD_SCALE) r = 0;
        if (m) { if (r > mel || (r < 0 && mel == 0)) mel = r; }
        else if (r > *ranged) *ranged = r;
    }
    return mel;
}

static void *ap_move(void *circle, uint64_t flags)
{
    float *c = (float *)((uint8_t *)circle + 8);                       /* x, y, radius */
    if (t_chase && t_mel > 0 && c[2] <= t_mel + WORLD_SCALE) {         /* an approach to melee reach */
        float r = t_mel * g_contact;
        if (r < 0.5f * WORLD_SCALE) r = 0.5f * WORLD_SCALE;
        c[2] = r;                                                       /* drive into contact, not to the edge of reach */
        if (t_haveV) lead_point(c[0], c[1], t_vx, t_vy, &c[0], &c[1]);  /* and to where the target is going */
    }
    void *o = g_moveCircleOrig(circle, flags);
    t_apOrder = o; t_apX = c[0]; t_apY = c[1]; t_apR = c[2];
    return o;
}

/* 0.5.4: a unit's priority, 0 none / 1 ranged / 2 melee. Highest first: entity tag melee, entity tag ranged, def prop
   melee, def prop ranged. Entity tags are simulation state ({tags add/remove} runs on every machine), so the result is
   the same everywhere. */
/* Log only: the first 20 (unit, result) pairs where a tag changes the def prop's choice. Nothing here is read by the
   decision path. Cost is capped: a pair already logged is skipped before any scan (lock-free look at <= 20 entries),
   the def is scanned by name only (def_priority_bits_quiet), and after 4096 such scans in all the block is off for
   the rest of the process (as it is after 20 lines). */
#define TAG_PRIO_LOG_MAX   20
#define TAG_PRIO_SCAN_MAX  4096
static volatile LONG g_tagPrioLogged, g_tagPrioScans;
static struct { uint8_t *e; int prio; } g_tagPrioSeen[TAG_PRIO_LOG_MAX];
static int tag_prio_seen(uint8_t *e, int prio)
{
    int n = (int)g_tagPrioLogged;
    for (int i = 0; i < n && i < TAG_PRIO_LOG_MAX; i++) if (g_tagPrioSeen[i].e == e && g_tagPrioSeen[i].prio == prio) return 1;
    return 0;
}
static void log_tag_priority(uint8_t *e, int prio)
{
    if (tag_prio_seen(e, prio) || InterlockedIncrement(&g_tagPrioScans) > TAG_PRIO_SCAN_MAX) return;
    int dp = bits_to_priority(def_priority_bits_quiet(e));
    if (dp == prio) return;
    EnterCriticalSection(&g_cs);
    int n = (int)g_tagPrioLogged;
    if (n < TAG_PRIO_LOG_MAX && !tag_prio_seen(e, prio)) {
        g_tagPrioSeen[n].e = e; g_tagPrioSeen[n].prio = prio;
        MemoryBarrier();                                                /* entry before count, for the lock-free look */
        g_tagPrioLogged = n + 1;
        logf_("unit %p: entity tag \"%s\" -> %s priority (def props: %s)", (void *)e, prio == 2 ? g_propMelee : g_propRanged,
              prio == 2 ? "melee" : "ranged", dp == 2 ? g_propMelee : dp == 1 ? g_propRanged : "neither");
    }
    LeaveCriticalSection(&g_cs);
}
/* gem_melee 0.1.19 (Alex: a melee toggle for vehicles; "i cant imagine someone would want their bolter dread walking ... straight at
   a baneblade"): entity tag melee_hold = ranged priority whatever the def props / other tags say, and vs_tick cancels an approach
   to melee reach. The unit still punches what comes into reach. */
static char g_tagHold[48] = "melee_hold";
static int ent_melee_hold(uint8_t *e)
{
    if (!o_entTags || !e) return 0;
    uint8_t *b = *(uint8_t **)(e + o_entTags), *en = *(uint8_t **)(e + o_entTags + 8);
    if (!b || en < b || (size_t)(en - b) > TAG_SIZE * 1024) return 0;
    size_t n = strlen(g_tagHold);
    for (uint8_t *t = b; t + TAG_SIZE <= en; t += TAG_SIZE) {
        const MsString *s = (const MsString *)(t + TAG_NAME);
        if (s->size == n && !_strnicmp(ms_cstr(s), g_tagHold, n)) return 1;
    }
    return 0;
}
static int unit_priority(uint8_t *e)
{
    if (ent_melee_hold(e)) return 1;                                   /* gem_melee 0.1.19 */
    if (o_entTags) {
        int t = tags_priority_bits(*(uint8_t **)(e + o_entTags), *(uint8_t **)(e + o_entTags + 8), 1024);
        if (t) {
            int prio = bits_to_priority(t);
            if (g_logChase && g_tagPrioLogged < TAG_PRIO_LOG_MAX && g_tagPrioScans < TAG_PRIO_SCAN_MAX)
                log_tag_priority(e, prio);
            return prio;
        }
    }
    return bits_to_priority(def_priority_bits(e));
}

/* ai_prioritize_melee / ai_prioritize_ranged (entity tag, else def prop): the approach range the tick is about to use
   becomes the melee reach (charge in) or the ranged reach (stand off and shoot); inside that reach -1 = no approach.
   Returns 0 = no prop / tag, 1 = ranged priority, 2 = melee priority. */
static volatile LONG g_prioLogged;
static int apply_priority(uint8_t *brain, uint8_t *e, uint8_t *rec, float mel, float rng)
{
    int ret = unit_priority(e), pm = ret == 2, pr = ret == 1;
    if (!ret || !rec) return ret;
    if (pr) return ret;      /* 0.5.4: ranged priority leaves the engine's approach range alone (it only counts weapons that
                                can attack the target: a broken or useless gun no longer parks the unit); it still skips
                                the melee chase and the hold-ranged-in-melee rule */
    float reach = mel;
    if (!(reach > 0.0f)) return ret;                                    /* no weapon of that kind: vanilla */
    float *p = (float *)(e + ENT_POS), *t = (float *)(rec + o_recX);
    float dx = p[0] - t[0], dy = p[1] - t[1], dz = p[2] - t[2];
    float dist = __builtin_sqrtf(dx * dx + dy * dy + dz * dz);
    { float h = tgt_dist(e, rec, t[0], t[1]); if (h < dist) dist = h; }   /* gem_melee 0.1.10: a wide target's hull, not its centre */
    float want = dist >= reach ? reach : -1.0f, *cur = (float *)(brain + o_approach);
    if (*cur != want) {
        if (g_logChase && InterlockedIncrement(&g_prioLogged) <= 20)
            logf_("%s unit %p: target %.1f m, approach range %.1f -> %s", pm ? "melee-priority" : "ranged-priority", (void *)e,
                  dist / WORLD_SCALE, *cur < 0 ? -1.0f : *cur / WORLD_SCALE, want < 0 ? "hold (in reach)" : pm ? "melee reach" : "ranged reach");
        CORE_TRACE(CORE_MOD_CHASE, e, "%s priority: target %.1f m, approach range -> %s",      /* gem_melee: R5 trace (link) */
                   pm ? "melee" : "ranged", dist / WORLD_SCALE, want < 0 ? "hold (in reach)" : "melee reach");
        *cur = want;
    }
    return ret;
}

/* diagnostic: a vehicle-brain unit approached but none of its weapons has the melee tag - list what tags they do have */
static volatile LONG g_noMeleeLogged;
static void log_no_melee(uint8_t *e)
{
    if (!g_logChase || !e || InterlockedIncrement(&g_noMeleeLogged) > 15) return;
    char buf[700]; size_t n = 0; buf[0] = 0;
    uint8_t *wy = *(uint8_t **)(e + o_weaponry);
    uint8_t **wb = wy ? *(uint8_t ***)(wy + o_wlistBeg) : NULL, **we = wy ? *(uint8_t ***)(wy + o_wlistEnd) : NULL;
    int nw = 0;
    for (; wb && wb < we && we - wb <= 64 && n < sizeof buf - 80; wb++, nw++) {
        uint8_t *item = *wb ? *(uint8_t **)(*wb + o_weapItem) : NULL;
        n += snprintf(buf + n, sizeof buf - n, " [w%d:", nw);
        if (item) {
            uint8_t *tb = *(uint8_t **)(item + o_itemTagBeg), *te = *(uint8_t **)(item + o_itemTagEnd);
            for (uint8_t *t = tb; t && t + TAG_SIZE <= te && te - tb <= TAG_SIZE * 64 && n < sizeof buf - 40; t += TAG_SIZE)
                n += snprintf(buf + n, sizeof buf - n, " %.24s", ms_cstr((const MsString *)(t + TAG_NAME)));
        }
        n += snprintf(buf + n, sizeof buf - n, "]");
    }
    logf_("unit %p approaches but has no weapon tagged \"%s\" - not chased. Weapon tags:%s", (void *)e, g_meleeTag, nw ? buf : " (no weapons)");
}

static uint64_t vs_tick(void *s, uint64_t a, uint64_t b, uint64_t c)
{
    t_chase = 0; t_haveV = 0; t_mel = 0;
    mission_check();                                                    /* 0.5.2 */
    uint8_t *brain = *(uint8_t **)((uint8_t *)s + o_vStateBrain);
    uint8_t *e = brain ? *(uint8_t **)(brain + o_vBrainEntity) : NULL;
    if (e && !(e[ENT_FLAGS] & 4) && (e[ENT_FLAGS] & 2)) {
        float rng, mel = unit_reach(e, &rng);
        uint8_t *rec = NULL;
        if (*(int32_t *)(brain + o_vMode) == 1) {
            uint8_t **rb = *(uint8_t ***)(brain + o_vRecBeg), **re = *(uint8_t ***)(brain + o_vRecEnd);
            if (rb && rb < re) rec = *rb;
        }
        int prio = g_okPriority ? apply_priority(brain, e, rec, mel, rng) : 0;
        int hold = ent_melee_hold(e);                                   /* gem_melee 0.1.19 */
        if (mel != 0) {
            t_chase = 1; t_mel = mel;
            if (rec && g_okClose) {
                float vx, vy;
                EnterCriticalSection(&g_cs); track(e, rec, &vx, &vy); LeaveCriticalSection(&g_cs);
                t_vx = vx; t_vy = vy; t_haveV = 1;
                /* the vanilla brain approaches only when the target is outside weapon range, and stands still inside it.
                   A melee unit (melee-only, or melee priority) instead chases whenever the target is outside 90% of its
                   reach (chase_always), and always when the target backs off (pursue_retreating). */
                float *cur = (float *)(brain + o_approach), *p = (float *)(e + ENT_POS);
                float tx = *(float *)(rec + o_recX), ty = *(float *)(rec + o_recY);
                int meleeUnit = prio == 2 || (prio == 0 && !(rng > 0));
                /* gem_melee 0.1.10 / 0.1.12: in reach of a wide target's hull (but not of its centre) and it is not getting away: no
                   approach, the unit fights where it stands instead of pushing into the hull. 0.1.12 (Alex on 0.1.11: the dreadnoughts
                   "still walked up to it and stood in front not meleeing"; log: a new approach order twice a second at 8.1 m from the
                   centre): also a unit of any priority whose approach range is its melee reach - the engine's own choice when its guns
                   cannot hurt the target - and the threshold is the reach itself. */
                if (mel > 0 && *cur >= 0 && ((prio != 1 && meleeUnit) || *cur <= mel + WORLD_SCALE)) {
                    static volatile LONG hullLogged, farLogged;
                    float dc = dist2d(p[0], p[1], tx, ty), dh = tgt_dist(e, rec, tx, ty);
                    if (dh <= mel && dh < dc && receding(p[0], p[1], tx, ty, vx, vy) <= g_recede) {
                        if (g_logChase && InterlockedIncrement(&hullLogged) <= 20)
                            logf_("melee unit %p: in reach of the target's hull (nearest part %.1f m, centre %.1f m, reach %.1f m, approach range %.1f m) - holding",
                                  (void *)e, dh / WORLD_SCALE, dc / WORLD_SCALE, mel / WORLD_SCALE, *cur / WORLD_SCALE);
                        *cur = -1.0f;
                    } else if (dc <= mel * 3.0f && g_logChase && InterlockedIncrement(&farLogged) <= 20)
                        logf_("melee unit %p: approach to melee reach, out of reach of every part (nearest part %.1f m, centre %.1f m, reach %.1f m)",
                              (void *)e, dh / WORLD_SCALE, dc / WORLD_SCALE, mel / WORLD_SCALE);
                }
                if (hold && mel > 0 && *cur >= 0 && *cur <= mel + WORLD_SCALE) {     /* gem_melee 0.1.19: melee_hold = no walk into melee reach */
                    static volatile LONG holdTagLogged;
                    if (g_logChase && InterlockedIncrement(&holdTagLogged) <= 20)
                        logf_("melee unit %p: tagged %s - approach to melee reach (%.1f m) cancelled", (void *)e, g_tagHold, *cur / WORLD_SCALE);
                    *cur = -1.0f;
                }
                if (prio != 1 && mel > 0 && *cur < 0) {
                    float d = tgt_dist(e, rec, tx, ty), back = receding(p[0], p[1], tx, ty, vx, vy);   /* gem_melee 0.1.10 */
                    int always = g_chaseAlways && meleeUnit && d > mel * 0.9f;
                    int retreat = g_pursue && d > mel * g_contact && back > g_recede;
                    if (always || retreat) {
                        *cur = mel;
                        if (g_logChase && InterlockedIncrement(&g_pursueLogged) <= 20)
                            logf_("melee unit %p: target %.1f m (%s %.1f m/s) inside weapon range - chasing", (void *)e, d / WORLD_SCALE,
                                  back >= 0 ? "moving away at" : "closing at", (back >= 0 ? back : -back) / WORLD_SCALE);
                    }
                }
            }
        }
    }

    t_apOrder = NULL;
    uint64_t r = g_vsTickOrig(s, a, b, c);
    void *o = t_apOrder; t_apOrder = NULL;
    if (!o || !brain || *(void **)(brain + o_aiOrder) != o) return r;  /* no approach, or not (any more) the AI order */
    if (!t_chase) { log_no_melee(e); return r; }
    if (!(t_apR > 0.0f) || t_apR > 2000.0f * WORLD_SCALE) return r;
    float *p = (float *)(e + ENT_POS);
    float self = dist2d(p[0], p[1], t_apX, t_apY);
    EnterCriticalSection(&g_cs);
    int i = ch_find(o);
    if (i < 0) {
        i = 0;
        while (i < MAX_CHASE && g_ch[i].used) i++;
        if (i < MAX_CHASE) { if (i >= g_chHigh) g_chHigh = i + 1; InterlockedIncrement(&g_chLive); }
    }
    if (i >= 0 && i < MAX_CHASE) {
        Chase ch = { o, e, brain, t_apX, t_apY, t_apR, t_mel > 0 ? t_mel : t_apR,
                     *g_simTime + (self < g_closeRange ? g_repathMsClose : g_repathMs), 1, 0, 0 };
        g_ch[i] = ch;
    }
    LeaveCriticalSection(&g_cs);
    /* chase at the chosen pace (the vanilla vehicle AI moves at whatever mode the chassis has) */
    if (o_modeSlot && g_chaseMode >= 0) {
        uint8_t *chs = *(uint8_t **)(e + o_chassis);
        if (chs && *(void ***)chs) {
            typedef void (*SetModeFn)(void *chassis, int mode, int flag);
            ((SetModeFn)(*(void ***)chs)[o_modeSlot / 8])(chs, g_chaseMode, 0);
        }
    }
    CORE_NOTE_FRONT(e, *(void **)o, "chase approach", *g_simTime);     /* gem_melee: R4 front-order change (the approach order's vtable), R5 trace */
    CORE_TRACE(CORE_MOD_CHASE, e, "approach registered: %.1f m away, goal radius %.1f m", self / WORLD_SCALE, t_apR / WORLD_SCALE);
    if (g_logChase && InterlockedIncrement(&g_regLogged) <= 20)
        logf_("melee unit %p approaches (%.1f m away, goal radius %.1f m) - following the target", (void *)e, self / WORLD_SCALE, t_apR / WORLD_SCALE);
    return r;
}

static uint64_t mt_update(void *o, uint64_t phase, uint64_t a, uint64_t b)
{
    if (!g_chLive) return g_mtUpdateOrig(o, phase, a, b);
    mission_check();                                                    /* 0.5.2: a mission without vehicles never runs vs_tick */
    if (!g_chLive) return g_mtUpdateOrig(o, phase, a, b);
    EnterCriticalSection(&g_cs);
    int i = ch_find(o);
    if (i < 0) { LeaveCriticalSection(&g_cs); return g_mtUpdateOrig(o, phase, a, b); }
    if ((uint32_t)phase == 0) {                                           /* start (a reused pointer that started before is stale) */
        if (g_ch[i].started) ch_remove(i); else g_ch[i].started = 1;
        LeaveCriticalSection(&g_cs);
        return g_mtUpdateOrig(o, phase, a, b);
    }
    if ((uint32_t)phase != 1) {                                           /* stop */
        int smooth = g_ch[i].repath && g_smooth && g_finishSub;
        /* the move reached its goal but the target is backing off: the next approach follows at once, so don't brake */
        if (!smooth && g_smooth && g_finishSub && g_okClose && *(int32_t *)(g_ch[i].brain + o_vMode) == 1) {
            uint8_t **rb = *(uint8_t ***)(g_ch[i].brain + o_vRecBeg), **re = *(uint8_t ***)(g_ch[i].brain + o_vRecEnd);
            if (rb && rb < re && *rb) {
                float vx, vy, *p = (float *)(g_ch[i].ent + ENT_POS);
                track(g_ch[i].ent, *rb, &vx, &vy);
                smooth = g_pursue && receding(p[0], p[1], *(float *)(*rb + o_recX), *(float *)(*rb + o_recY), vx, vy) > g_recede;
            }
        }
        ch_remove(i);
        LeaveCriticalSection(&g_cs);
        if (smooth) { g_finishSub(o); return 1; }                         /* drop the path, keep the chassis rolling */
        return g_mtUpdateOrig(o, phase, a, b);
    }
    uint32_t now = *g_simTime;
    if ((int32_t)(now - g_ch[i].next) < 0) { LeaveCriticalSection(&g_cs); return g_mtUpdateOrig(o, phase, a, b); }
    Chase ch = g_ch[i];
    uint8_t *brain = ch.brain, *rec = NULL;
    if (*(int32_t *)(brain + o_vMode) == 1) {
        uint8_t **rb = *(uint8_t ***)(brain + o_vRecBeg), **re = *(uint8_t ***)(brain + o_vRecEnd);
        if (rb && rb < re) rec = *rb;
    }
    float vx = 0, vy = 0;
    if (rec && g_okClose) track(ch.ent, rec, &vx, &vy);
    float *pos = (float *)(ch.ent + ENT_POS);
    float tx = rec ? *(float *)(rec + o_recX) : 0, ty = rec ? *(float *)(rec + o_recY) : 0;
    float self = rec ? tgt_dist(ch.ent, rec, tx, ty) : 0;              /* gem_melee 0.1.10: a wide target's hull, not its centre */
    g_ch[i].next = now + (self < g_closeRange ? g_repathMsClose : g_repathMs);
    LeaveCriticalSection(&g_cs);
    if (!rec) return g_mtUpdateOrig(o, phase, a, b);

    float px, py; lead_point(tx, ty, vx, vy, &px, &py);
    float moved = dist2d(px, py, ch.cx, ch.cy), back = receding(pos[0], pos[1], tx, ty, vx, vy);
    /* in reach and the target is not getting away: stop and fight */
    if (self <= ch.reach * 0.9f && back <= g_recede) {
        if (g_logChase && InterlockedIncrement(&g_stopLogged) <= 20)
            logf_("melee unit %p: target in reach (%.1f m) - stopping", (void *)ch.ent, self / WORLD_SCALE);
        return 1;
    }
    /* the goal moved: re-plan. The threshold shrinks near the target (fraction of the distance, min..max) */
    float thr = self * g_repathFrac;
    if (thr < g_repathMin) thr = g_repathMin; if (thr > g_repathMax) thr = g_repathMax;
    if (moved > thr) {
        EnterCriticalSection(&g_cs);
        int j = ch_find(o); if (j >= 0) g_ch[j].repath = 1;
        LeaveCriticalSection(&g_cs);
        if (g_logChase && InterlockedIncrement(&g_repLogged) <= 30)
            logf_("melee unit %p: target goal moved %.1f m - new path (%.1f m away, %s %.1f m/s)", (void *)ch.ent, moved / WORLD_SCALE,
                  self / WORLD_SCALE, back >= 0 ? "moving away at" : "closing at", (back >= 0 ? back : -back) / WORLD_SCALE);
        return 1;
    }
    return g_mtUpdateOrig(o, phase, a, b);
}

/* an approach order can be deleted without ever running its stop step (replaced before it started): forget it here,
   so a later order allocated at the same address is never mistaken for it */
static void *mt_dtor(void *o, uint64_t flags)
{
    if (g_chLive) {
        EnterCriticalSection(&g_cs);
        int i = ch_find(o); if (i >= 0) ch_remove(i);
        LeaveCriticalSection(&g_cs);
    }
    return g_mtDtorOrig(o, flags);
}

/* ---- hold ranged fire while the target is inside melee reach ---- */
static _Thread_local uint8_t *t_evalBrain;
static EvalFn g_evalOrig;
static SetTargetFn g_setTargetOrig;
static volatile LONG g_holdLogged;

static uint64_t eval_wrap(void *brain, uint64_t a, uint64_t b, uint64_t c)
{
    uint8_t *prev = t_evalBrain;
    t_evalBrain = (uint8_t *)brain;
    uint64_t r = g_evalOrig(brain, a, b, c);
    t_evalBrain = prev;
    return r;
}

static uint64_t st_wrap(void *w, void *desc, uint64_t a, uint64_t mode, uint64_t b)
{
    uint8_t *brain = t_evalBrain;
    {   /* gem_melee 0.1.13 (log only): what the target evaluation hands each weapon of a unit in melee reach of its target */
        static volatile LONG evLogged;
        uint8_t *de = (brain && w && desc && g_logChase && evLogged < 40) ? *(uint8_t **)(brain + o_vBrainEntity) : NULL;
        if (de && de == *(uint8_t **)((uint8_t *)w + o_weapOwner) && !(de[ENT_FLAGS] & 4) && (de[ENT_FLAGS] & 2) && *(int32_t *)(brain + o_vMode) == 1) {
            uint8_t **db = *(uint8_t ***)(brain + o_vRecBeg), **dn = *(uint8_t ***)(brain + o_vRecEnd);
            float drng, dmel = (db && db < dn && *db) ? unit_reach(de, &drng) : 0;
            if (dmel > 0) {
                float *dt = (float *)(*db + o_recX), dh = tgt_dist(de, *db, dt[0], dt[1]);
                if (dh <= dmel * 1.2f && InterlockedIncrement(&evLogged) <= 40)
                    logf_("eval: unit %p weapon %p (%s, range %.1f m) is handed target type %d, mode %llu | nearest part %.1f m, reach %.1f m, priority %d, target %s",
                          (void *)de, w, weapon_is_melee((uint8_t *)w) ? "melee" : "ranged", g_weaponRange ? g_weaponRange(w) / WORLD_SCALE : -1.0f,
                          *(int32_t *)((uint8_t *)desc + 8), (unsigned long long)mode, dh / WORLD_SCALE, dmel / WORLD_SCALE,
                          g_okPriority ? unit_priority(de) : 0, tgt_wide(*db) ? "vehicle-class" : "human");
            }
        }
    }
    /* gem_melee 0.1.15 (Alex: the claw works against the Baneblade under direct control; "cant we just let the ai shoot multiple
       weapons"): the evaluation hands the target to the one weapon its selection chose. When that is a ranged weapon and the target
       is a vehicle-class entity with a part inside a melee weapon's range, that melee weapon is handed the same target here, in the
       same call (the desc is the evaluation's own, alive now). The chosen weapon is not touched by this. */
    if (brain && w && desc && *(int32_t *)((uint8_t *)desc + 8) != 0 && g_weaponRange && o_weaponry && o_weapOwner && !weapon_is_melee((uint8_t *)w)) {
        static volatile LONG alsoLogged, st_also;
        uint8_t *me = *(uint8_t **)(brain + o_vBrainEntity);
        if (me && me == *(uint8_t **)((uint8_t *)w + o_weapOwner) && !(me[ENT_FLAGS] & 4) && (me[ENT_FLAGS] & 2) && *(int32_t *)(brain + o_vMode) == 1) {
            uint8_t **mb = *(uint8_t ***)(brain + o_vRecBeg), **mn = *(uint8_t ***)(brain + o_vRecEnd);
            uint8_t *wy = *(uint8_t **)(me + o_weaponry);
            if (mb && mb < mn && *mb && tgt_wide(*mb) && wy) {
                float *mt = (float *)(*mb + o_recX), mh = tgt_dist(me, *mb, mt[0], mt[1]);
                uint8_t **wb = *(uint8_t ***)(wy + o_wlistBeg), **we = *(uint8_t ***)(wy + o_wlistEnd);
                for (; wb && wb < we && we - wb <= 64; wb++) {
                    uint8_t *w2 = *wb;
                    if (!w2 || w2 == (uint8_t *)w || !weapon_is_melee(w2) || *(uint8_t **)(w2 + o_weapOwner) != me) continue;
                    float r2 = g_weaponRange(w2);
                    if (!(r2 > 0.0f) || mh > r2) continue;
                    g_setTargetOrig(w2, desc, a, mode, b);
                    InterlockedIncrement(&st_also);
                    if (g_logChase && InterlockedIncrement(&alsoLogged) <= 20)
                        logf_("melee unit %p: melee weapon %p is handed the target too (nearest part %.1f m, range %.1f m; the evaluation chose weapon %p, mode %llu)",
                              (void *)me, (void *)w2, mh / WORLD_SCALE, r2 / WORLD_SCALE, w, (unsigned long long)mode);
                }
            }
        }
    }
    if (brain && w && desc && *(int32_t *)((uint8_t *)desc + 8) != 0 && !weapon_is_melee((uint8_t *)w)) {
        uint8_t *e = *(uint8_t **)(brain + o_vBrainEntity);
        if (e && e == *(uint8_t **)((uint8_t *)w + o_weapOwner) && !(e[ENT_FLAGS] & 4) && (e[ENT_FLAGS] & 2) &&
            *(int32_t *)(brain + o_vMode) == 1) {
            uint8_t **rb = *(uint8_t ***)(brain + o_vRecBeg), **re = *(uint8_t ***)(brain + o_vRecEnd);
            float rng, mel = (rb && rb < re && *rb) ? unit_reach(e, &rng) : 0;
            /* not ranged priority (tag, else prop). gem_melee 0.1.13 (Alex: the dreadnoughts "still stand in front not meleeing"; log: the
               heavy flamer took the target at 40 m and the engine never looked at the fist again): a ranged-priority unit holds its
               ranged weapons too when the target in melee reach is a vehicle-class entity, so the engine falls through to the melee
               weapon. Against infantry it keeps shooting, as before. */
            if (mel > 0 && (!(g_okPriority && unit_priority(e) == 1) || tgt_wide(*rb))) {
                float *p = (float *)(e + ENT_POS), *t = (float *)(*rb + o_recX);
                if ((void)p, tgt_dist(e, *rb, t[0], t[1]) <= mel * g_holdFactor) {       /* gem_melee 0.1.10 */
                    if (g_logChase && InterlockedIncrement(&g_holdLogged) <= 10)
                        logf_("melee unit %p: target in melee reach - ranged weapon holds fire", (void *)e);
                    CORE_NOTE_VERDICT(CORE_MOD_CHASE, e, NULL, CORE_CHASE_HOLD, *g_simTime);   /* gem_melee: R4 note, R5 trace, R3 cease-fire through the core (same call) */
                    CORE_TRACE(CORE_MOD_CHASE, e, "hold: target in melee reach, ranged weapon %p cleared", w);
                    { uint64_t r = 0; core_ceasefire(CORE_MOD_CHASE, w, (void *)g_clearTarget, &r); return r; }
                }
            }
        }
    }
    return g_setTargetOrig(w, desc, a, mode, b);
}

/* ---- gem_melee 0.1.14: a melee weapon in reach of a wide target's hull is selected and counts as in range ----
   Alex on 0.1.13 (three dreadnoughts at a Baneblade's hull, the evaluation hands the target to the heavy flamer only): the
   engine's weapon selection drops a weapon whose reach test against the target's CENTRE answers 6 (out of range) before
   ammo.set is asked, and the target evaluation compares the selected weapon's range with the distance to the centre again.
   A hull keeps a 6 m fist away from a Baneblade's centre for good. Both answers are changed only for a melee weapon of a
   vehicle-brain unit against a vehicle-class target that has a part inside the weapon's range (core_hull_point). */
typedef uint64_t (*SelCheckFn)(void *firer, void *aim, void *out, uint64_t flag);
static SelCheckFn g_selOrig;
static WeaponRangeFn g_evrOrig;
static uint8_t *g_selSite, *g_selFn, *g_evrSite;
static volatile LONG g_selLogged, g_evrLogged, st_selKeep, st_evrIn;

static uint64_t sel_wrap(void *firer, void *aim, void *out, uint64_t flag)
{
    uint64_t r = g_selOrig(firer, aim, out, flag);
    if ((uint32_t)r != 6 || !firer || !aim || !core_hull_point || !g_weaponRange || !o_weapOwner) return r;
    uint8_t *w = *(uint8_t **)((uint8_t *)firer + 0x10);
    if (!w || !weapon_is_melee(w)) return r;
    uint8_t *e = *(uint8_t **)(w + o_weapOwner), *t = *(uint8_t **)((uint8_t *)aim + 0x18);
    if (!e || !t || t == e || (e[ENT_FLAGS] & 4) || !(e[ENT_FLAGS] & 2) || (t[ENT_FLAGS] & 4) || !(t[ENT_FLAGS] & 2)) return r;
    float pt[3], h = core_hull_point(e, t, pt), rng = g_weaponRange(w);
    if (h < 0.0f || !(rng > 0.0f) || h > rng) return r;
    InterlockedIncrement(&st_selKeep);
    if (g_logChase && InterlockedIncrement(&g_selLogged) <= 20)
        logf_("melee unit %p: weapon %p is out of range of the target's centre but a part is %.1f m away (range %.1f m) - kept in the weapon selection",
              (void *)e, (void *)w, h / WORLD_SCALE, rng / WORLD_SCALE);
    return 3;
}

static float evr_wrap(void *weapon)
{
    float r = g_evrOrig(weapon);
    uint8_t *brain = t_evalBrain, *w = (uint8_t *)weapon;
    if (!brain || !w || !core_hull_point || !(r > 0.0f) || !o_weapOwner || !weapon_is_melee(w)) return r;
    uint8_t *e = *(uint8_t **)(brain + o_vBrainEntity);
    if (!e || e != *(uint8_t **)(w + o_weapOwner) || (e[ENT_FLAGS] & 4) || !(e[ENT_FLAGS] & 2) || *(int32_t *)(brain + o_vMode) != 1) return r;
    uint8_t **rb = *(uint8_t ***)(brain + o_vRecBeg), **re = *(uint8_t ***)(brain + o_vRecEnd);
    if (!rb || rb >= re || !*rb || !tgt_wide(*rb)) return r;
    float *p = (float *)(e + ENT_POS), *t = (float *)(*rb + o_recX);
    float dx = p[0] - t[0], dy = p[1] - t[1], dz = p[2] - t[2], dc = __builtin_sqrtf(dx * dx + dy * dy + dz * dz);
    float dh = tgt_dist(e, *rb, t[0], t[1]);
    if (!(dh <= r) || dc < r) return r;                               /* no part in reach, or the centre is in range anyway */
    InterlockedIncrement(&st_evrIn);
    if (g_logChase && InterlockedIncrement(&g_evrLogged) <= 20)
        logf_("melee unit %p: weapon %p counts as in range in the target evaluation (nearest part %.1f m, centre %.1f m, range %.1f m)",
              (void *)e, (void *)w, dh / WORLD_SCALE, dc / WORLD_SCALE, r / WORLD_SCALE);
    return dc + WORLD_SCALE;
}

/* gem_melee 0.1.16 (Alex on 0.1.15: "they still stand there", "an attack order against LR it works"): the fist had its target and
   an accepted aim and still did not fire. The engine's range(weapon, target) = target->vt[0x80]() + weaponRange(weapon) is compared
   by the weapon's fire task and by the attack task with the distance to the target's CENTRE (the aim's entity position, whatever
   part the aim names): fine against a Leman Russ, never against a Baneblade. Every call site of that function goes through here:
   for a melee weapon of a vehicle-brain unit against a vehicle-class target with a part inside the weapon's range the answer is at
   least the centre distance + 1 m. */
typedef float (*RangeWTFn)(void *weapon, void *target);
static RangeWTFn g_rwtOrig;
static uint8_t *g_rwtFn;
static volatile LONG g_rwtLogged, st_rwt;
static float rwt_wrap(void *weapon, void *target)
{
    float r = g_rwtOrig(weapon, target);
    uint8_t *w = (uint8_t *)weapon, *t = (uint8_t *)target;
    if (!w || !t || !core_hull_point || !g_weaponRange || !o_weapOwner || !weapon_is_melee(w)) return r;
    uint8_t *e = *(uint8_t **)(w + o_weapOwner);
    if (!e || t == e || (e[ENT_FLAGS] & 4) || !(e[ENT_FLAGS] & 2) || (t[ENT_FLAGS] & 4) || !(t[ENT_FLAGS] & 2)) return r;
    float *p = (float *)(e + ENT_POS), *q = (float *)(t + ENT_POS);
    float dx = p[0] - q[0], dy = p[1] - q[1], dz = p[2] - q[2], dc = __builtin_sqrtf(dx * dx + dy * dy + dz * dz);
    if (dc + WORLD_SCALE <= r) return r;                               /* the centre is in range anyway */
    float pt[3], h = core_hull_point(e, t, pt), wr = g_weaponRange(w);
    if (h < 0.0f || !(wr > 0.0f) || h > wr) return r;
    InterlockedIncrement(&st_rwt);
    if (g_logChase && InterlockedIncrement(&g_rwtLogged) <= 20)
        logf_("melee unit %p: weapon %p range against its target %.1f -> %.1f m (nearest part %.1f m, centre %.1f m, weapon range %.1f m)",
              (void *)e, (void *)w, r / WORLD_SCALE, (dc + WORLD_SCALE) / WORLD_SCALE, h / WORLD_SCALE, dc / WORLD_SCALE, wr / WORLD_SCALE);
    return dc + WORLD_SCALE;
}
static void install_rwt(void)
{
    /* ... call qword [rax+0x80] ; movaps xmm6,xmm0 ; jmp +3 ; xorps xmm6,xmm6 ; mov rcx,rbx ; call weaponRange ; addss xmm0,xmm6 */
    static const uint8_t pat[] = { 0xFF, 0x90, 0x80, 0x00, 0x00, 0x00, 0x0F, 0x28, 0xF0, 0xEB, 0x03, 0x0F, 0x57, 0xF6, 0x48, 0x8B, 0xCB, 0xE8 };
    int m = 0, sites = 0, ok = 0;
    if (g_weaponRange)
        for (uint8_t *p = g_text.p; p + sizeof pat + 8 < g_text.p + g_text.n; p++)
            if (!memcmp(p, pat, sizeof pat) && rel32_target(p + 17, 5) == (uint8_t *)g_weaponRange && p[22] == 0xF3 && p[23] == 0x0F && p[24] == 0x58 && p[25] == 0xC6) {
                uint32_t rva = func_of(p);
                if (rva) { g_rwtFn = g_base + rva; m++; }
            }
    if (m == 1) {
        for (uint8_t *p = g_text.p; p + 5 < g_text.p + g_text.n; p++)
            if (p[0] == 0xE8 && rel32_target(p, 5) == g_rwtFn && func_of(p)) {
                sites++;
                ok += redirect_call(p, (void *)rwt_wrap, (void **)&g_rwtOrig, "melee range against a target", g_rwtFn);
            }
    }
    logf_("hull melee                     range against a target %s (range(weapon, target) exe+0x%llX, %d of %d call sites): a melee weapon with a part of a wide target in reach is in range of it",
          ok && ok == sites ? "ON" : ok ? "PARTLY ON" : m == 1 ? "OFF (no call site redirected)" : "OFF (function not recognised)", EXE_OFF(g_rwtFn), ok, sites);
}

/* gem_melee 0.1.17 (Alex on 0.1.16: same problem at the Baneblade, a Macharius is not hit either, a Leman Russ is; "why cant you make
   them just fire the melee weapon against the hulls? they are clearly in range and can aim"). The 0.1.16 range rule never fired:
   range was not the gate. The weaponry quant fires a weapon only when (1) its aimer's status (vtable +0x48) is 3 or 4 - the aim
   bones reach the picked part inside their limits, error under the weapon's aimingTolerance - and (2) the aimer's fire line check
   (vtable +0x18, a trace along the barrel) is clear. At a big hull the picked part is beside / above the fist and the fist is inside
   the target's volumes. For a melee weapon of a vehicle-brain unit whose entity target has a part inside the weapon's range, both
   are answered here: status 3 when the engine's own readiness gate says the weapon is ready, fire line clear. */
#define AIM_WEAPON   0x10        /* aimer -> its weapon (mov rbx / rcx,[rcx+0x10] at the top of the status function, all three exes) */
#define W_AIMER      0xA8        /* weapon -> its aimer; checked on every call */
#define DESC_TYPE    0x08        /* aim desc: 1 = an entity */
#define DESC_ENTITY  0x18
typedef int (*AimStatFn)(void *aimer);
typedef uint8_t (*AimBlkFn)(void *aimer);
typedef int (*AimGateFn)(void *aimer, uint8_t reset);
static AimStatFn g_astOrig;
static AimBlkFn g_blkOrig;
static AimGateFn g_astGate;
static uint8_t *g_astFn, *g_blkFn;
static uint32_t o_wAimDesc, o_wFireMode;
static void *g_hmAimer; static uint8_t *g_hmE, *g_hmW;       /* status call -> fire line call of the same weapon, same quant */
static volatile LONG st_astForced, st_astNotReady, st_astAimed, st_blkCleared, g_astLogged, g_astNrLogged, g_blkLogged;
static volatile LONG st_sptForced;                                  /* gem_melee 0.1.18 */
static LONG g_astSumSeen; static DWORD g_astSumTick;
static void ast_summary(void)
{
    DWORD now = GetTickCount();
    LONG sum = st_astForced + st_astNotReady + st_astAimed + st_blkCleared + st_sptForced;
    if (!g_logChase || now - g_astSumTick < 5000 || sum == g_astSumSeen) return;
    g_astSumTick = now; g_astSumSeen = sum;
    logf_("hull melee fire: aim status answered 3 %ld times (the engine said aimed %ld, weapon not ready %ld), blocked fire line cleared %ld, shot range test answered %ld",
          (long)st_astForced, (long)st_astAimed, (long)st_astNotReady, (long)st_blkCleared, (long)st_sptForced);
}
static int hull_melee_aimer(void *aimer, uint8_t **pe, uint8_t **pw, float *ph, float *pwr, float *pface)
{
    uint8_t *w = *(uint8_t **)((uint8_t *)aimer + AIM_WEAPON);
    if (!w || *(void **)(w + W_AIMER) != aimer) return 0;
    uint8_t *e = *(uint8_t **)(w + o_weapOwner);
    if (!e || (e[ENT_FLAGS] & 4) || !(e[ENT_FLAGS] & 2)) return 0;       /* humans leave here */
    if (!weapon_is_melee(w)) return 0;
    uint8_t *d = w + o_wAimDesc;
    if (*(int32_t *)(d + DESC_TYPE) != 1) return 0;
    uint8_t *t = *(uint8_t **)(d + DESC_ENTITY);
    if (!t || t == e || (t[ENT_FLAGS] & 4) || !(t[ENT_FLAGS] & 2)) return 0;
    float pt[3] = { 0, 0, 0 }, h = core_hull_point(e, t, pt), wr = g_weaponRange(w);
    if (h < 0.0f || !(wr > 0.0f) || h > wr) return 0;
    float *p = (float *)(e + ENT_POS), *fw = (float *)(e + ENT_POS - 0x24);          /* log only: the entity's X row, if the matrix sits before the position */
    float dx = pt[0] - p[0], dy = pt[1] - p[1], dl = __builtin_sqrtf(dx * dx + dy * dy), fl = __builtin_sqrtf(fw[0] * fw[0] + fw[1] * fw[1]);
    *pface = dl > 1.0f && fl > 0.5f && fl < 1.5f ? (dx * fw[0] + dy * fw[1]) / (dl * fl) : 9.0f;
    *pe = e; *pw = w; *ph = h; *pwr = wr;
    return 1;
}
static int ast_wrap(void *aimer)
{
    int st = g_astOrig(aimer);
    uint8_t *e = NULL, *w = NULL; float h = 0.0f, wr = 0.0f, face = 0.0f;
    if (!aimer || !core_hull_point || !hull_melee_aimer(aimer, &e, &w, &h, &wr, &face)) { if (g_hmAimer == aimer) g_hmAimer = NULL; return st; }
    g_hmAimer = aimer; g_hmE = e; g_hmW = w;
    if (st == 3 || st == 4) { InterlockedIncrement(&st_astAimed); ast_summary(); return st; }
    int gate = g_astGate(aimer, 0);                                       /* the engine's readiness gate, asked without its side effects */
    int mode = o_wFireMode ? *(int32_t *)(w + o_wFireMode) : -1;
    if (gate != 3) {
        InterlockedIncrement(&st_astNotReady);
        if (g_logChase && InterlockedIncrement(&g_astNrLogged) <= 20)
            logf_("melee unit %p: weapon %p in reach of the hull (nearest part %.1f m, range %.1f m): aim status %d kept, the weapon is not ready (engine gate %d, fire mode %d)",
                  (void *)e, (void *)w, h / WORLD_SCALE, wr / WORLD_SCALE, st, gate, mode);
        ast_summary();
        return st;
    }
    InterlockedIncrement(&st_astForced);
    if (g_logChase && InterlockedIncrement(&g_astLogged) <= 30)
        logf_("melee unit %p: weapon %p in reach of the hull (nearest part %.1f m, range %.1f m): aim status %d -> 3 (fire mode %d, facing %.2f)",
              (void *)e, (void *)w, h / WORLD_SCALE, wr / WORLD_SCALE, st, mode, face);
    ast_summary();
    return 3;
}
static uint8_t blk_wrap(void *aimer)
{
    uint8_t r = g_blkOrig(aimer);
    if (aimer && g_hmAimer == aimer) {
        g_hmAimer = NULL;
        if (r) {
            InterlockedIncrement(&st_blkCleared);
            if (g_logChase && InterlockedIncrement(&g_blkLogged) <= 20)
                logf_("melee unit %p: weapon %p in reach of the hull: the engine reports its fire line blocked - answered clear", (void *)g_hmE, (void *)g_hmW);
            return 0;
        }
    }
    return r;
}
static void install_ast(void)
{
    /* inside the status function: mov dl,1 ; mov rcx,rdi ; call gate ; cmp eax,3 ; jne ... ; then add rcx, <aim desc offset> */
    static const uint8_t pat[] = { 0xB2, 0x01, 0x48, 0x8B, 0xCF, 0xE8 };
    static const uint8_t bp[] = { 0x48, 0x83, 0xEC, 0x28, 0x80, 0x79, 0x78, 0x00, 0x75 };   /* fire line slot: sub rsp,28 ; cmp byte [rcx+78],0 ; jne */
    int m = 0, slots = 0, okS = 0, okB = 0;
    uint8_t *site = NULL;
    for (uint8_t *p = g_text.p; p + 0x60 < g_text.p + g_text.n; p++)
        if (!memcmp(p, pat, sizeof pat) && p[10] == 0x83 && p[11] == 0xF8 && p[12] == 0x03 && p[13] == 0x0F && p[14] == 0x85) { site = p; m++; }
    if (m == 1) {
        uint32_t rva = func_of(site);
        uint8_t *gate = rel32_target(site + 5, 5);
        if (rva && site - (g_base + rva) < 0x80 && in_text(gate)) {
            g_astFn = g_base + rva; g_astGate = (AimGateFn)gate;
            for (uint8_t *q = site + 15; q < site + 0x50; q++)
                if (q[0] == 0x48 && q[1] == 0x81 && q[2] == 0xC1 && !q[5] && !q[6]) { o_wAimDesc = *(uint32_t *)(q + 3); break; }
        }
    }
    if (g_setTarget)                                                    /* log only: mov [rbx+X],r32 ; mov [rbx+X+4],r32 = the weapon's target mode / fire mode */
        for (uint8_t *q = (uint8_t *)g_setTarget; q < (uint8_t *)g_setTarget + 0x100 && !o_wFireMode; q++)
            if (q[0] == 0x89 && (q[1] & 0xC7) == 0x83 && q[6] == 0x89 && (q[7] & 0xC7) == 0x83 && *(uint32_t *)(q + 8) == *(uint32_t *)(q + 2) + 4 && *(uint32_t *)(q + 2) < 0x400)
                o_wFireMode = *(uint32_t *)(q + 8);
    if (g_astFn && o_wAimDesc && o_weapOwner && g_weaponRange)
        for (size_t j = 9 * 8; j + 8 <= g_rdata.n; j += 8) {
            void **slot = (void **)(g_rdata.p + j);
            if (*slot != (void *)g_astFn) continue;
            void **vt = slot - 9;
            uint8_t *b = (uint8_t *)vt[3];
            int isb = (b == (uint8_t *)blk_wrap) || (in_text(b) && !memcmp(b, bp, sizeof bp) && (!g_blkFn || g_blkFn == b));
            slots++;
            if (isb && b != (uint8_t *)blk_wrap) { g_blkFn = b; okB += core_patch_ptr(CORE_MOD_CHASE, "melee fire line slot", vt + 3, b, (void *)blk_wrap, (void **)&g_blkOrig); }
            if (g_blkOrig) okS += core_patch_ptr(CORE_MOD_CHASE, "melee aim status slot", slot, g_astFn, (void *)ast_wrap, (void **)&g_astOrig);
        }
    logf_("hull melee fire                aim status %s, fire line %s (status exe+0x%llX, readiness gate exe+0x%llX, fire line exe+0x%llX; %d + %d of %d aimer vtables; aim desc +0x%X, fire mode +0x%X): a melee weapon with a part of its target in reach fires",
          okS && okS == slots ? "ON" : okS ? "PARTLY ON" : "OFF", okB && okB == slots ? "ON" : okB ? "PARTLY ON" : "OFF",
          EXE_OFF(g_astFn), EXE_OFF((uint8_t *)g_astGate), EXE_OFF(g_blkFn), okS, okB, slots, o_wAimDesc, o_wFireMode);
}

/* gem_melee 0.1.18 (Alex on 0.1.17: "ogryns cant melee tanks, dreads dont do it", "i can clearly take direct control and aim and fire
   the melee"). With the aim status 3 and a clear fire line the weaponry quant did call the shooter with fire = 1, and still nothing.
   The shooter's update only shoots when shooter->vt[0x60](point) says the point is between the min and max range of the weapon's
   ammo, measured from the aimer in 2D. Under direct control the point is the cursor's aim point; for the AI it is the target
   ENTITY's position, its centre: 7-10 m from a fist at a Baneblade's hull, always out of an ogryn's 3.5 m. When that test fails for
   a melee weapon of a vehicle-brain unit, the tested point is the target's centre and a part of the target is inside the weapon's
   range, the answer is true. */
#define SHOOTER_WEAPON 0x08      /* shooter -> its weapon */
typedef uint8_t (*ShotPtFn)(void *shooter, float *pt);
static ShotPtFn g_sptOrig;
static uint8_t *g_sptFn;
static volatile LONG g_sptLogged;
static uint8_t spt_wrap(void *shooter, float *pt)
{
    uint8_t r = g_sptOrig(shooter, pt);
    if (r || !shooter || !pt || !core_hull_point) return r;
    uint8_t *w = *(uint8_t **)((uint8_t *)shooter + SHOOTER_WEAPON);
    if (!w) return r;
    void *aimer = *(void **)(w + W_AIMER);
    uint8_t *e = NULL, *w2 = NULL; float h = 0.0f, wr = 0.0f, face = 0.0f;
    if (!aimer || !hull_melee_aimer(aimer, &e, &w2, &h, &wr, &face) || w2 != w) return r;
    uint8_t *t = *(uint8_t **)(w + o_wAimDesc + DESC_ENTITY);
    float *c = (float *)(t + ENT_POS), *p = (float *)(e + ENT_POS);
    if (pt[0] != c[0] || pt[1] != c[1]) return r;                         /* a direct-control aim point stays the engine's */
    InterlockedIncrement(&st_sptForced);
    if (g_logChase && InterlockedIncrement(&g_sptLogged) <= 30) {
        float dx = p[0] - c[0], dy = p[1] - c[1];
        logf_("melee unit %p: weapon %p in reach of the hull (nearest part %.1f m, range %.1f m): the shot's range test against the target's centre (%.1f m) failed - answered in range",
              (void *)e, (void *)w, h / WORLD_SCALE, wr / WORLD_SCALE, __builtin_sqrtf(dx * dx + dy * dy) / WORLD_SCALE);
    }
    ast_summary();
    return 1;
}
static void install_spt(void)
{
    /* mov [rsp+8],rbx ; push rdi ; sub rsp,30 ; mov rax,[rcx+8] */
    static const uint8_t sp[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x8B, 0x41, 0x08 };
    void **vt = find_vtable_rtti(".?AVeBulletShooter@Weapon@Actor@@");
    int slots = 0, ok = 0;
    uint8_t *fn = vt ? (uint8_t *)vt[12] : NULL;
    if (fn && in_text(fn) && !memcmp(fn, sp, sizeof sp) && g_astFn && o_wAimDesc && o_weapOwner && g_weaponRange) {
        g_sptFn = fn;
        for (size_t j = 12 * 8; j + 8 <= g_rdata.n; j += 8) {
            void **slot = (void **)(g_rdata.p + j);
            if (*slot != (void *)fn) continue;
            slots++;
            ok += core_patch_ptr(CORE_MOD_CHASE, "melee shot range slot", slot, fn, (void *)spt_wrap, (void **)&g_sptOrig);
        }
    }
    logf_("hull melee fire                shot range %s (shooter point test exe+0x%llX, %d of %d shooter vtables): a melee weapon with a part of its target in reach is not held back by the distance to the target's centre",
          ok && ok == slots ? "ON" : ok ? "PARTLY ON" : "OFF", EXE_OFF(g_sptFn), ok, slots);
}

static void install_contact(void)
{
    install_rwt();                                                      /* gem_melee 0.1.16: all three exes */
    install_ast();                                                      /* gem_melee 0.1.17: all three exes */
    install_spt();                                                      /* gem_melee 0.1.18: all three exes */
    if (!g_evalFn || !g_weaponRange || !o_weapOwner) { logf_("hull melee: needs the hold and weapon range finders - off"); return; }
    uint32_t evRva = func_of(g_evalFn); int n = 0;
    for (uint8_t *p = g_evalFn; p < g_evalFn + 0x1400; p++)
        if (p[0] == 0xE8 && func_of(p) == evRva && rel32_target(p, 5) == (uint8_t *)g_weaponRange) { g_evrSite = p; n++; }
    int okE = 0;
    /* the selection's reach test: mov rcx,[rcx+0xA8] ; call F, with mov r9b,1 in the 20 bytes before and mov ebx,eax in the 16 bytes
       after (gem_melee 0.1.19: all three exes; the release builds order these instructions differently from the editor build) */
    int m = 0;
    for (uint8_t *p = g_text.p + 24; p + 40 < g_text.p + g_text.n; p++)
        if (p[0] == 0x48 && p[1] == 0x8B && p[2] == 0x89 && *(uint32_t *)(p + 3) == 0xA8 && p[7] == 0xE8 && in_text(rel32_target(p + 7, 5))) {
            int r9 = 0, bx = 0;
            for (int k = 3; k <= 20; k++) if (p[-k] == 0x41 && p[-k + 1] == 0xB1 && p[-k + 2] == 0x01) r9 = 1;
            for (int k = 12; k <= 26; k++) if (p[k] == 0x8B && p[k + 1] == 0xD8) bx = 1;
            if (r9 && bx) { g_selSite = p + 7; m++; }
        }
    int okS = 0;
    if (m == 1 && n == 1) {                                             /* both or none: the range hook alone changes nothing when the selection drops the weapon */
        g_selFn = rel32_target(g_selSite, 5);
        okS = redirect_call(g_selSite, (void *)sel_wrap, (void **)&g_selOrig, "melee weapon selection reach test", g_selFn);
        okE = okS && redirect_call(g_evrSite, (void *)evr_wrap, (void **)&g_evrOrig, "melee range in the target evaluation", (void *)g_weaponRange);
    }
    logf_("hull melee                     selection %s, evaluation range %s (a melee weapon with a part of a wide target in reach is selected and counts as in range)",
          okS ? "ON" : m > 1 ? "OFF (several matches)" : m ? "OFF" : "OFF (this exe's selection code is not recognised yet)",
          okE ? "ON" : n == 1 ? "OFF (goes in with the selection hook only)" : "OFF (range call not found once)");
    if (okS) logf_("hull melee: selection reach test exe+0x%llX -> exe+0x%llX; evaluation range call exe+0x%llX", EXE_OFF(g_selSite), EXE_OFF(g_selFn), EXE_OFF(g_evrSite));
}

static int install_hold(void)
{
    if (!redirect_call(g_evalJump, (void *)eval_wrap, (void **)&g_evalOrig, "target evaluation", g_evalFn)) return 0;   /* gem_melee: + expect */
    for (int i = 0; i < g_stSiteN; i++)
        if (!redirect_call(g_stSites[i], (void *)st_wrap, (void **)&g_setTargetOrig, "weapon target", (void *)g_setTarget)) return i > 0;   /* gem_melee: + expect */
    install_contact();                                                  /* gem_melee 0.1.14: never fails the hold */
    return 1;
}

static int install_vehicle(void)
{
    InitializeCriticalSection(&g_cs);
    if (!vt_hook(g_vtMoveTask, 0, (void *)mt_dtor, (void **)&g_mtDtorOrig, "vt eMoveTask[0] dtor")) { logf_("vehicle: eMoveTask dtor not hooked"); return 0; }   /* gem_melee: registry names, + expect */
    if (!vt_hook(g_vtMoveTask, 19, (void *)mt_update, (void **)&g_mtUpdateOrig, "vt eMoveTask[19] Update")) { logf_("vehicle: eMoveTask Update not hooked"); return 0; }
    if (!redirect_call(g_apSite, (void *)ap_move, (void **)&g_moveCircleOrig, "approach move order", (void *)g_moveCircle)) return 0;
    if (!vt_hook(g_vtVsRoot, 3, (void *)vs_tick, (void **)&g_vsTickOrig, "vt eVS_Root[3] tick")) { logf_("vehicle: eVS_Root tick not hooked"); return 0; }
    return 1;
}
/* ------------------------------------------------------------------------------------ */
/* gem_melee API (src/core/core.h): read-only views of what the finders found           */
int chase_layout(uint32_t *weap_item, uint32_t *tag_beg, uint32_t *tag_end)
{ if (!g_okWeapons) return 0; *weap_item = o_weapItem; *tag_beg = o_itemTagBeg; *tag_end = o_itemTagEnd; return 1; }
void *chase_clock_ptr(void) { return (void *)g_simTime; }
void *chase_ceasefire_fn(void) { return (void *)g_clearTarget; }
uint32_t chase_enttags_off(void) { return o_entTags; }
int chase_status(void) { return (g_okVehicle ? 1 : 0) | (g_okClose ? 2 : 0) | (g_okPriority ? 4 : 0) | (g_okHold ? 8 : 0); }
#endif /* FX_TEST */
