/* GEM ScriptExt - fauna squads
 * Part of the unity build: #included by gem_scriptext.c, not compiled on its own.
 * Leader election for fauna squads, box-select of a whole squad picks the leader, editor placement,
 * members take the leader's walk/run pace. Simulation changes: every MP peer needs the DLL.
 */

/* ------------------------------------------------------------------------------------------------------------------
   Box-select: fauna squads selected like human squads.
   The engine's "active unit" (GetActiveEntity -> selection vt[4]) is the selection's ONLY entry, or the unit under direct
   control. Box-selecting a complete squad (SquadOf(list) = brain(+0xDC8)->squad(+0x100), all members, nothing else) puts
   just the squad leader (squad+0xA8) in the selection - that is how a human squad gets its leader portrait, squad strip
   (PanelSquad lists the leader's squad), direct control of the leader and squad orders (e_move: leader -> squad order).
   For a vehicle-class leader the box select adds a chassis check (leader+0xDE0) that fauna fail, so every fauna was
   selected separately and the group had no active unit. Here that one IsVehicle(leader) call in the box select is
   redirected: for a fauna leader it answers "not a vehicle", so the engine takes its human-squad path. Nothing else
   (orders, scripts, other IsVehicle callers) is touched.
   ------------------------------------------------------------------------------------------------------------------ */
typedef uint8_t (*IsVehFn)(void *e);
static uint8_t *g_sqSelSite;                     /* E8 of "call IsVehicle(leader)" in the box select */
static IsVehFn g_isVehOrig;
static uint32_t g_brainOff, g_brainEntOff, g_squadOff, g_squadLeadOff, g_squadMemOff;
static int g_okSqSel, g_wantSqSel = 1;

static int find_squad_select(void)
{
    /* mov rcx,rbx ; call IsVehicle ; test al,al ; je ; mov rcx,[rbx+CHASSIS] ; call ChassisCheck ; test al,al ; je ;
       test rbx,rbx ; je   (preceded by: call SquadOf ; mov rdi,rax ; test rax,rax ; je ; mov rbx,[rax+LEADER]) */
    static const uint8_t isveh[] = { 0x48,0x85,0xC9,0x74,0x09,0xF6,0x41,0x30,0x08,0x74,0x03,0xB0,0x01,0xC3,0x32,0xC0,0xC3 };
    int hits = 0; uint8_t *site = NULL;
    for (uint8_t *q = g_text.p; q + 40 <= g_text.p + g_text.n; q++) {
        if (q[0] != 0x48 || q[1] != 0x8B || q[2] != 0xCB || q[3] != 0xE8) continue;
        if (q[8] != 0x84 || q[9] != 0xC0 || q[10] != 0x74 || q[12] != 0x48 || q[13] != 0x8B || q[14] != 0x8B || q[19] != 0xE8) continue;
        if (q[24] != 0x84 || q[25] != 0xC0 || q[26] != 0x74 || q[28] != 0x48 || q[29] != 0x85 || q[30] != 0xDB || q[31] != 0x74) continue;
        uint8_t *tg = q + 8 + *(int32_t *)(q + 4);
        if (!in_text(tg) || memcmp(tg, isveh, sizeof isveh)) continue;
        hits++; site = q;
    }
    if (hits != 1) { logf_("squad select: box-select vehicle check not recognised (%d matches)", hits); return 0; }
    /* leader offset + SquadOf right before the site */
    uint8_t *sq = NULL; uint32_t leadOff = 0;
    for (uint8_t *q = site - 7; q > site - 0x30; q--)
        if (q[0] == 0x48 && q[1] == 0x8B && q[2] == 0x98) { leadOff = *(uint32_t *)(q + 3);
            for (uint8_t *c = q - 5; c > q - 0x18; c--) if (c[0] == 0xE8) { sq = c + 5 + *(int32_t *)(c + 1); break; }
            break; }
    if (!leadOff || !sq || !in_text(sq)) { logf_("squad select: squad lookup before the box-select check not recognised"); return 0; }
    /* SquadOf: mov rdi,[rax+BRAIN] ... call IsSquadLeader ... members: mov rcx,[rbx+END] ; sub rcx,[rbx+BEGIN] */
    uint8_t *isl = NULL; uint32_t brain = 0, memB = 0, memE = 0;
    for (uint8_t *q = sq; q < sq + 0x100; q++) {
        if (!brain && q[0] == 0x48 && q[1] == 0x8B && q[2] == 0xB8) brain = *(uint32_t *)(q + 3);
        if (!isl && brain && q[0] == 0xE8) { uint8_t *c = q + 5 + *(int32_t *)(q + 1); if (in_text(c)) isl = c; }
        if (!memE && q[0] == 0x48 && q[1] == 0x8B && q[2] == 0x8B && q[7] == 0x48 && q[8] == 0x2B && q[9] == 0x8B) {
            memE = *(uint32_t *)(q + 3); memB = *(uint32_t *)(q + 10); }
    }
    /* IsSquadLeader(brain): mov rdx,[rcx+SQUAD] ; test rdx,rdx ; je ; mov rax,[rcx+ENT] ; cmp [rdx+LEADER],rax */
    if (!isl || !brain || memE != memB + 8 || isl[0] != 0x48 || isl[1] != 0x8B || isl[2] != 0x91 || isl[12] != 0x48 || isl[13] != 0x8B ||
        isl[14] != 0x41 || isl[16] != 0x48 || isl[17] != 0x39 || isl[18] != 0x82 || *(uint32_t *)(isl + 19) != leadOff) {
        logf_("squad select: squad layout not recognised"); return 0; }
    g_sqSelSite = site + 3; g_isVehOrig = (IsVehFn)(site + 8 + *(int32_t *)(site + 4));
    g_brainOff = brain; g_squadOff = *(uint32_t *)(isl + 3); g_brainEntOff = isl[15]; g_squadLeadOff = leadOff; g_squadMemOff = memB;
    logf_("squad select: box-select vehicle check @ exe+0x%llX (IsVehicle exe+0x%llX); entity->brain +0x%X, brain->squad +0x%X, "
          "brain->entity +0x%X, squad leader +0x%X, members +0x%X", EXE_OFF(g_sqSelSite), EXE_OFF(g_isVehOrig), g_brainOff, g_squadOff,
          g_brainEntOff, g_squadLeadOff, g_squadMemOff);
    return 1;
}

/* the squad of an entity (NULL if none) and its member count */
static uint8_t *squad_of(const uint8_t *e, int *members)
{
    if (members) *members = 0;
    if (!g_brainOff || !e) return NULL;
    uint8_t *brain = *(uint8_t *const *)(e + g_brainOff);
    uint8_t *sq = brain ? *(uint8_t **)(brain + g_squadOff) : NULL;
    if (sq && members) { uint8_t **b = *(uint8_t ***)(sq + g_squadMemOff), **en = *(uint8_t ***)(sq + g_squadMemOff + 8);
                         *members = (b && en >= b && en - b < 4096) ? (int)(en - b) : -1; }
    return sq;
}

/* ------------------------------------------------------------------------------------------------------------------
   Leader election: fauna squads get a squad leader (simulation).
   ElectLeader(members) gives NO leader to a squad with 2+ vehicle-class members (flags&8) - by design a multi-vehicle squad
   is leaderless, every unit is selected and ordered on its own. Fauna are vehicle-class, so a fauna squad never had a
   leader: no leader portrait, squad strip, group direct control or squad orders. Here the engine's two election call
   sites (squad creation, squad update re-election) go through sx_elect_leader: when the engine finds nobody and the
   squad's vehicle-class members are all fauna, the first eligible member (infantry first, then fauna - eligibility =
   the engine's own candidate test, ElectLeader on that member alone) becomes leader. The squad update is wrapped too:
   a leaderless all-fauna squad gets the engine's own "leader lost" flag, so it re-elects through the same path
   (squads that existed before the DLL, saved maps). Deterministic sim code - every MP peer needs the DLL.
   ------------------------------------------------------------------------------------------------------------------ */
typedef uint8_t *(*ElectFn)(void *members);
typedef void (*SquadUpdFn)(void *squad);
static uint8_t *g_electSiteCreate, *g_electSiteUpdate, *g_squadUpdSite;
static ElectFn g_electOrig; static SquadUpdFn g_squadUpdOrig;
static uint32_t g_squadReelectOff, g_squadMembersOff;
typedef void (*SetLeaderFn)(void *squad, void *leader);
static SetLeaderFn g_setLeader; static uint8_t *g_edSetLeaderSite;   /* editor unit placement: SetLeader(squad, template leader or NULL) */
static int g_okElect, g_wantElect = 1;

static int find_squad_elect(void)
{
    /* squad creation: sub rcx,-MEM ; call Elect ; mov rbx,rax ; test rax,rax ; je ; mov rdx,rax ; mov rcx,rdi ; call SetLeader */
    uint8_t *sc = NULL; int nc = 0;
    for (uint8_t *q = g_text.p; q + 32 <= g_text.p + g_text.n; q++)
        if (q[0] == 0x48 && q[1] == 0x83 && q[2] == 0xE9 && q[4] == 0xE8 && q[9] == 0x48 && q[10] == 0x8B && q[11] == 0xD8 &&
            q[12] == 0x48 && q[13] == 0x85 && q[14] == 0xC0 && q[15] == 0x74 && q[17] == 0x48 && q[18] == 0x8B && q[19] == 0xD0 &&
            q[20] == 0x48 && q[21] == 0x8B && q[22] == 0xCF && q[23] == 0xE8) { sc = q; nc++; }
    if (nc != 1) { logf_("squad leader: squad-creation election not recognised (%d matches)", nc); return 0; }
    uint32_t mem = (uint32_t)(-(int8_t)sc[3]);
    uint8_t *el = sc + 9 + *(int32_t *)(sc + 5), *sl = sc + 28 + *(int32_t *)(sc + 24);
    if (!in_text(el) || !in_text(sl)) { logf_("squad leader: election targets outside .text"); return 0; }
    /* squad update: the other "call Elect ; mov rbx,rax ; test rax,rax ; je ; mov rdx,rax ; mov rcx,r14 ; call SetLeader",
       just after "lea rcx,[r14+MEM]" and the clear of the re-elect flag (mov byte [r14+FLAG],0 / mov [r14+FLAG],reg8) */
    uint8_t *su = NULL; int nu = 0; uint32_t flag = 0;
    for (uint8_t *q = g_text.p; q + 32 <= g_text.p + g_text.n; q++) {
        if (q[0] != 0xE8 || q + 5 + *(int32_t *)(q + 1) != el || q == sc + 4) continue;
        if (q[5] != 0x48 || q[6] != 0x8B || q[7] != 0xD8 || q[8] != 0x48 || q[9] != 0x85 || q[10] != 0xC0 || q[11] != 0x74 ||
            q[13] != 0x48 || q[14] != 0x8B || q[15] != 0xD0 || q[16] != 0x49 || q[17] != 0x8B || q[18] != 0xCE || q[19] != 0xE8 ||
            q + 24 + *(int32_t *)(q + 20) != sl) continue;
        uint32_t f = 0; int lea = 0;
        for (uint8_t *k = q - 1; k > q - 0x30; k--) {
            if (k[0] == 0x49 && k[1] == 0x8D && k[2] == 0x8E && *(uint32_t *)(k + 3) == mem) lea = 1;
            if (!f && k[0] == 0x41 && k[1] == 0xC6 && k[2] == 0x86 && k[7] == 0x00) f = *(uint32_t *)(k + 3);           /* mov byte [r14+F],0 */
            if (!f && (k[0] & 0xF0) == 0x40 && (k[0] & 1) && k[1] == 0x88 && (k[2] & 0xC7) == 0x86) f = *(uint32_t *)(k + 3); /* mov [r14+F],r8b */
        }
        if (lea && f) { su = q; nu++; flag = f; }
    }
    if (nu != 1) { logf_("squad leader: squad-update election not recognised (%d matches)", nu); return 0; }
    uint32_t upd = func_of(su);
    if (!upd) { logf_("squad leader: squad update bounds unknown"); return 0; }
    uint8_t *uf = g_base + upd;
    /* the one call of the squad update: mov rcx,rbx ; call Update */
    int nCalls = 0; uint8_t *call = NULL;
    for (uint8_t *q = g_text.p; q + 5 <= g_text.p + g_text.n; q++)
        if (q[0] == 0xE8 && q + 5 + *(int32_t *)(q + 1) == uf) { nCalls++; call = q; }
    if (nCalls != 1 || call[-3] != 0x48 || call[-2] != 0x8B || call[-1] != 0xCB) { logf_("squad leader: squad update call not recognised (%d calls)", nCalls); return 0; }
    g_electSiteCreate = sc + 4; g_electSiteUpdate = su; g_squadUpdSite = call; g_setLeader = (SetLeaderFn)sl;
    /* editor only (SceneEditor::eUnits, placing a squad): call SetLeader ; mov rax,[rip+gc] ; mov rcx,[rax+SEL] - the editor then
       selects the leader alone, or every member when the template has no leader */
    int ne = 0;
    for (uint8_t *q = g_text.p; q + 20 <= g_text.p + g_text.n; q++)
        if (q[0] == 0xE8 && q + 5 + *(int32_t *)(q + 1) == sl && q[5] == 0x48 && q[6] == 0x8B && q[7] == 0x05 &&
            q[12] == 0x48 && q[13] == 0x8B && q[14] == 0x88) { ne++; g_edSetLeaderSite = q; }
    if (ne != 1) g_edSetLeaderSite = NULL;                       /* the game/server have no editor placement */
    g_electOrig = (ElectFn)el; g_squadUpdOrig = (SquadUpdFn)uf;
    g_squadReelectOff = flag; g_squadMembersOff = mem;
    logf_("squad leader: ElectLeader exe+0x%llX (create @ exe+0x%llX, update @ exe+0x%llX), SetLeader exe+0x%llX, squad update exe+0x%llX "
          "(called @ exe+0x%llX), members +0x%X, re-elect flag +0x%X", EXE_OFF(el), EXE_OFF(g_electSiteCreate), EXE_OFF(g_electSiteUpdate),
          EXE_OFF(sl), EXE_OFF(uf), EXE_OFF(call), g_squadMembersOff, g_squadReelectOff);
    if (g_edSetLeaderSite) logf_("squad leader: editor squad placement SetLeader @ exe+0x%llX", EXE_OFF(g_edSetLeaderSite));
    return 1;
}

/* Squad pace. A move order (Interface::Action::e_move) gives the ordered unit an Order::eMovementChange (walk/run,
   double-click = fast) built from the move modifiers: lea rax,[rip+MODE_TABLE] ; xor r8d,r8d ; mov edx,[rax+rdx*4] ;
   call MovementChange(a, mode, flag) ; lea r8,[rbp+CTX] ; mov rdx,rax ; mov rcx,rdi ; call GiveOrder.
   For a squad that is only the leader. Infantry members match the leader's pace in their own human AI states; fauna
   (vehicle brain) have none, so they kept walking. Here, when that unit leads an all-fauna squad, every other fauna member
   gets the same eMovementChange through the same GiveOrder (with the same order context - modify, not replace). */
typedef void *(*MoveChangeFn)(int a, int mode, uint8_t flag);
typedef uint64_t (*GiveOrderFn)(void *entity, void *order, void *ctx);
static uint8_t *g_mcSite, *g_giveSite; static MoveChangeFn g_mcCtor; static GiveOrderFn g_giveOrder;
static int g_okPace, g_wantPace = 1;
static int find_squad_pace(void)
{
    int n = 0; uint8_t *s = NULL;
    for (uint8_t *q = g_text.p; q + 36 <= g_text.p + g_text.n; q++)
        if (q[0] == 0x48 && q[1] == 0x8D && q[2] == 0x05 && q[7] == 0x45 && q[8] == 0x33 && q[9] == 0xC0 && q[10] == 0x8B &&
            q[11] == 0x14 && q[12] == 0x90 && q[13] == 0xE8 && q[18] == 0x4C && q[19] == 0x8D && q[20] == 0x85 && q[25] == 0x48 &&
            q[26] == 0x8B && q[27] == 0xD0 && q[28] == 0x48 && q[29] == 0x8B && q[30] == 0xCF && q[31] == 0xE8) { n++; s = q; }
    if (n != 1) { logf_("squad pace: move-mode site not recognised (%d matches)", n); return 0; }
    uint8_t *ctor = s + 18 + *(int32_t *)(s + 14), *give = s + 36 + *(int32_t *)(s + 32);
    /* the order is a 0x48-byte object: mov ecx,0x48 ; call new  near the start */
    int ok = 0;
    for (uint8_t *q = ctor; q < ctor + 0x30; q++) if (q[0] == 0xB9 && *(uint32_t *)(q + 1) == 0x48) { ok = 1; break; }
    if (!ok || !in_text(give)) { logf_("squad pace: movement-change order not recognised"); return 0; }
    g_mcSite = s + 13; g_giveSite = s + 31; g_mcCtor = (MoveChangeFn)ctor; g_giveOrder = (GiveOrderFn)give;
    logf_("squad pace: move action movement-change @ exe+0x%llX (order exe+0x%llX), give @ exe+0x%llX (exe+0x%llX)",
          EXE_OFF(g_mcSite), EXE_OFF(ctor), EXE_OFF(g_giveSite), EXE_OFF(give));
    return 1;
}

#ifndef SX_TEST
/* ui_debug: on each selection change, what the engine sees - entry count, its active unit, and the squad of the first
   fauna (does the box select of a whole fauna squad select only the leader?) */
static void squad_debug(void *panel)
{
    static uint8_t *lastFirst; static intptr_t lastN = -1; static int logged;
    if (logged >= 30 || !g_gameClient || !*g_gameClient) return;
    uint8_t *selObj = *(uint8_t **)((uint8_t *)*g_gameClient + g_selObjOff);
    if (!selObj) return;
    uint8_t *b = *(uint8_t **)(selObj + g_selBegOff), *en = *(uint8_t **)(selObj + g_selEndOff);
    if (!b || en < b || en - b > 4096 * 16) return;
    intptr_t n = (en - b) / g_selStride; uint8_t *first = n ? *(uint8_t **)b : NULL;
    if (n == lastN && first == lastFirst) return;
    lastN = n; lastFirst = first;
    uint8_t *f = NULL;
    for (uint8_t *q = b; q < en; q += g_selStride) { uint8_t *e = *(uint8_t **)q; if (e && is_fauna(e)) { f = e; break; } }
    if (!f) return;
    int m; uint8_t *sq = squad_of(f, &m);
    uint8_t *lead = sq ? *(uint8_t **)(sq + g_squadLeadOff) : NULL;
    logged++;
    logf_("ui debug: selection %d unit(s), engine active unit %p; fauna %p squad %p (%d members, leader %p%s)", (int)n,
          *(void **)((uint8_t *)panel + g_squadActiveOff), f, sq, m, lead, lead == f ? " = this fauna" : "");
}
static uint8_t sx_box_isvehicle(void *leader)
{
    uint8_t *e = (uint8_t *)leader;
    if (g_wantSqSel && e && is_fauna(e)) {
        static int n;
        if (n < 10) { int m; uint8_t *sq = squad_of(e, &m); n++;
                      logf_("squad select: fauna squad box-selected - leader %p (squad %p, %d members) selected like a human squad leader", e, sq, m); }
        return 0;                                               /* "not a vehicle": the engine selects the leader only */
    }
    return g_isVehOrig(leader);
}

static int install_squad_select(void)
{
    uint8_t *pad = alloc_near(g_sqSelSite, 0x40);
    if (!pad) { logf_("squad select: no memory within 2GB for the jump pad"); return 0; }
    pad[0] = 0xFF; pad[1] = 0x25; *(uint32_t *)(pad + 2) = 0; *(uint64_t *)(pad + 6) = (uint64_t)(uintptr_t)sx_box_isvehicle;
    FlushInstructionCache(GetCurrentProcess(), pad, 0x40);
    int64_t rel = (int64_t)(pad - (g_sqSelSite + 5));
    if (rel > 0x7FFFFFFF || rel < -0x80000000LL) { VirtualFree(pad, 0, MEM_RELEASE); logf_("squad select: jump pad too far"); return 0; }
    DWORD old;
    if (!VirtualProtect(g_sqSelSite, 5, PAGE_EXECUTE_READWRITE, &old)) { VirtualFree(pad, 0, MEM_RELEASE); logf_("squad select: VirtualProtect failed"); return 0; }
    HANDLE hs[256]; int n = suspend_others(hs, 256, g_sqSelSite, g_sqSelSite + 5);
    *(int32_t *)(g_sqSelSite + 1) = (int32_t)rel;
    resume_others(hs, n);
    VirtualProtect(g_sqSelSite, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), g_sqSelSite, 5);
    return 1;
}

/* runtime: fauna squad leader election */
static int squad_all_fauna(uint8_t **b, uint8_t **en, int *fauna)
{
    int nf = 0;
    if (!b || en < b || en - b > 64) return 0;
    for (uint8_t **q = b; q < en; q++) {
        uint8_t *e = *q;
        if (!e || !(e[ENT_FLAGS] & 8)) continue;                 /* infantry: fine */
        if (!is_fauna(e)) return 0;                              /* a real vehicle in the squad: vanilla rules */
        nf++;
    }
    *fauna = nf;
    return nf >= 2;                                              /* 0-1 vehicles: the engine already elects */
}
/* the leader for an all-fauna squad: the first member that passes the engine's own candidate test (alive, controllable -
   ElectLeader on that member alone), infantry first. NULL if the squad is not all-fauna or nobody qualifies. */
static uint8_t *elect_fauna_leader(uint8_t **b, uint8_t **en, const char *why)
{
    int nf; uint8_t *r = NULL;
    if (!squad_all_fauna(b, en, &nf)) return NULL;
    for (int pass = 0; pass < 2 && !r; pass++)
        for (uint8_t **q = b; q < en && !r; q++) {
            uint8_t *e = *q;
            if (!e || ((e[ENT_FLAGS] & 8) != 0) != (pass == 1)) continue;
            uint8_t *one[3] = { (uint8_t *)q, (uint8_t *)(q + 1), (uint8_t *)(q + 1) };   /* std::vector {begin,end,cap} */
            if (g_electOrig(one) == e) r = e;
        }
    if (r) { static int n; if (n < 20) { n++; logf_("squad leader: fauna squad (%d members, %d fauna) - leader %p elected (%s)", (int)(en - b), nf, r, why); } }
    return r;
}
static uint8_t *sx_elect_leader(void *members)
{
    uint8_t *r = g_electOrig(members);
    if (r || !g_wantElect) return r;
    return elect_fauna_leader(((uint8_t ***)members)[0], ((uint8_t ***)members)[1], "engine election");
}
/* editor squad placement: the template names no leader for a fauna squad -> elect one now, so the editor selects the leader
   alone (like an infantry squad) instead of every member */
static void sx_editor_set_leader(void *squad, void *leader)
{
    if (!leader && g_wantElect && squad) {
        uint8_t *s = (uint8_t *)squad;
        leader = elect_fauna_leader(*(uint8_t ***)(s + g_squadMembersOff), *(uint8_t ***)(s + g_squadMembersOff + 8), "editor placement");
    }
    g_setLeader(squad, leader);
}
static void sx_squad_update(void *squad)
{
    uint8_t *s = (uint8_t *)squad;
    if (g_wantElect && s && !*(void **)(s + g_squadLeadOff) && !s[g_squadReelectOff]) {
        uint8_t **b = *(uint8_t ***)(s + g_squadMembersOff), **en = *(uint8_t ***)(s + g_squadMembersOff + 8);
        int nf;
        if (squad_all_fauna(b, en, &nf)) s[g_squadReelectOff] = 1;   /* engine's own "leader lost": it re-elects below */
    }
    g_squadUpdOrig(squad);
}
/* runtime: squad pace */
static _Thread_local int t_mcA, t_mcMode, t_mcValid; static _Thread_local uint8_t t_mcFlag;
static void *sx_move_change(int a, int mode, uint8_t flag)
{
    t_mcA = a; t_mcMode = mode; t_mcFlag = flag; t_mcValid = 1;
    return g_mcCtor(a, mode, flag);
}
static uint64_t sx_move_give(void *entity, void *order, void *ctx)
{
    int valid = t_mcValid; t_mcValid = 0;
    uint64_t r = g_giveOrder(entity, order, ctx);
    if (!valid || !g_wantPace || !entity) return r;
    uint8_t *e = (uint8_t *)entity; int m;
    uint8_t *sq = squad_of(e, &m);
    if (!sq || *(uint8_t **)(sq + g_squadLeadOff) != e || m < 2) return r;      /* only the leader of a squad */
    uint8_t **b = *(uint8_t ***)(sq + g_squadMembersOff), **en = *(uint8_t ***)(sq + g_squadMembersOff + 8);
    int nf;
    if (!squad_all_fauna(b, en, &nf)) return r;
    uint8_t *mem[16]; int k = 0;                                  /* snapshot: giving orders must not walk a changing vector */
    for (uint8_t **q = b; q < en && k < 16; q++) if (*q && *q != e && ((*q)[ENT_FLAGS] & 2) && is_fauna(*q)) mem[k++] = *q;
    for (int i = 0; i < k; i++) { void *o = g_mcCtor(t_mcA, t_mcMode, t_mcFlag); if (o) g_giveOrder(mem[i], o, ctx); }
    static int logged; if (k && logged < 10) { logged++; logf_("squad pace: leader %p movement mode %d -> %d fauna squad members", e, t_mcMode, k); }
    return r;
}

static int install_squad_elect(void)
{
    return redirect_call(g_electSiteCreate, (void *)sx_elect_leader, "squad leader (create)")
        && redirect_call(g_electSiteUpdate, (void *)sx_elect_leader, "squad leader (update)")
        && redirect_call(g_squadUpdSite, (void *)sx_squad_update, "squad leader (squad update)")
        && (!g_edSetLeaderSite || redirect_call(g_edSetLeaderSite, (void *)sx_editor_set_leader, "squad leader (editor placement)"));
}

#endif /* SX_TEST */
