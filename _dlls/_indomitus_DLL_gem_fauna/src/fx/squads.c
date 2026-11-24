/* GEM Fauna - fauna squads
 * Part of the unity build: #included by gem_fauna.c, not compiled on its own.
 * Leader election for fauna squads (ElectLeader hooked at its entry), box-select of a whole squad or of loose units
 * ends leader-only like infantry, members take the leader's walk/run pace, a double-click run makes a fauna sprint.
 * Election and pace are simulation changes (every MP peer needs the DLL); the selection changes are local UI.
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
    logf_("squad select: box-select vehicle check @ exe+0x%llX (IsVehicle exe+0x%llX, SquadOf exe+0x%llX); entity->brain +0x%X, "
          "brain->squad +0x%X, brain->entity +0x%X, squad leader +0x%X, members +0x%X", EXE_OFF(g_sqSelSite), EXE_OFF(g_isVehOrig), EXE_OFF(sq),
          g_brainOff, g_squadOff, g_brainEntOff, g_squadLeadOff, g_squadMemOff);
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
   Leader election (simulation). ElectLeader(members) gives NO leader to a squad with 2+ vehicle-class members (flags&8):
   by design a multi-vehicle squad is leaderless. Fauna are vehicle-class, so a fauna squad never had a leader (no leader
   portrait, squad strip, group direct control or squad orders). ElectLeader is hooked at its entry (fx_elect_leader,
   below); the squad update is wrapped so a leaderless all-fauna squad raises the engine's own "elect again" flag.
   ------------------------------------------------------------------------------------------------------------------ */
typedef uint8_t *(*ElectFn)(void *members);
typedef void (*SquadUpdFn)(void *squad);
typedef void (*SetLeaderFn)(void *squad, void *leader);
/* SetSelected(entity, on): the entity's "selected" bit (entity+SELFLAGS bit 17) plus the local selection list */
typedef void (*SetSelectedFn)(void *entity, uint8_t on);
static uint8_t *g_electSiteCreate, *g_electSiteUpdate, *g_squadUpdSite;
static uint8_t *g_electSites[8]; static int g_electSiteN;      /* every E8 call of ElectLeader (fallback if the entry hook fails) */
static uint8_t *g_electEntry;                                    /* ElectLeader itself */
static ElectFn g_electOrig;                                      /* the engine's election (the trampoline once hooked) */
static SquadUpdFn g_squadUpdOrig; static uint32_t g_squadReelectOff;
static SetLeaderFn g_setLeader; static uint8_t *g_edSetLeaderSite;   /* editor squad-leader dialog: SetLeader(squad, chosen or NULL) */
static SetSelectedFn g_setSelected; static uint32_t g_entSelFlagsOff; /* optional: without it a new squad's leader-only selection is box-only */
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
    if (mem != g_squadMemOff) { logf_("squad leader: member vector offset +0x%X differs from the box select's +0x%X", mem, g_squadMemOff); return 0; }
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
    /* every caller of ElectLeader: besides creation and update there is the squad_create command (editor unit tool / in-game
       "create squad" on a selection), which elects the leader of the new squad itself - unhooked, a fauna selection got NULL */
    g_electSiteN = 0;
    for (uint8_t *q = g_text.p; q + 5 <= g_text.p + g_text.n; q++)
        if (q[0] == 0xE8 && q + 5 + *(int32_t *)(q + 1) == el) { if (g_electSiteN < 8) g_electSites[g_electSiteN] = q; g_electSiteN++; }
    if (g_electSiteN > 8) { logf_("squad leader: too many ElectLeader callers (%d)", g_electSiteN); return 0; }
    /* editor only (SceneEditor::eUnits#2, the "squad leader" property dialog): call SetLeader ; mov rax,[rip+gc] ; mov rcx,[rax+SEL] */
    int ne = 0;
    for (uint8_t *q = g_text.p; q + 20 <= g_text.p + g_text.n; q++)
        if (q[0] == 0xE8 && q + 5 + *(int32_t *)(q + 1) == sl && q[5] == 0x48 && q[6] == 0x8B && q[7] == 0x05 &&
            q[12] == 0x48 && q[13] == 0x8B && q[14] == 0x88) { ne++; g_edSetLeaderSite = q; }
    if (ne != 1) g_edSetLeaderSite = NULL;                       /* the game/server have no editor placement */
    /* SetSelected: the creation function's "mov dl,1 ; mov rcx,r64 ; call SetSelected" (select the new leader), whose first
       instruction reads the selection flags: mov r9d,[rcx+SELFLAGS]. Optional. */
    uint32_t cf = func_of(sc); uint8_t *cfn = cf ? g_base + cf : NULL; g_setSelected = NULL; g_entSelFlagsOff = 0;
    for (uint8_t *q = cfn; cfn && q < cfn + 0x100 && !g_setSelected; q++)
        if (q[0] == 0xB2 && q[1] == 0x01 && q[2] == 0x48 && q[3] == 0x8B && (q[4] & 0xF8) == 0xC8 && q[5] == 0xE8) {
            uint8_t *t = q + 10 + *(int32_t *)(q + 6);
            if (in_text(t) && t[0] == 0x44 && t[1] == 0x8B && t[2] == 0x89 && *(uint32_t *)(t + 3) >= 0x100 && *(uint32_t *)(t + 3) <= 0x2000) {
                g_setSelected = (SetSelectedFn)t; g_entSelFlagsOff = *(uint32_t *)(t + 3); }
        }
    g_electOrig = (ElectFn)el; g_electEntry = el; g_squadUpdOrig = (SquadUpdFn)uf; g_squadReelectOff = flag;
    logf_("squad leader: ElectLeader exe+0x%llX (create @ exe+0x%llX, update @ exe+0x%llX), SetLeader exe+0x%llX, squad update exe+0x%llX "
          "(called @ exe+0x%llX), members +0x%X, re-elect flag +0x%X, %d ElectLeader call sites", EXE_OFF(el), EXE_OFF(g_electSiteCreate), EXE_OFF(g_electSiteUpdate),
          EXE_OFF(sl), EXE_OFF(uf), EXE_OFF(call), mem, g_squadReelectOff, g_electSiteN);
    for (int i = 0; i < g_electSiteN; i++)
        if (g_electSites[i] != g_electSiteCreate && g_electSites[i] != g_electSiteUpdate) logf_("squad leader: other ElectLeader caller @ exe+0x%llX (squad_create command)", EXE_OFF(g_electSites[i]));
    if (g_setSelected) logf_("squad leader: SetSelected exe+0x%llX (selected = entity+0x%X bit 17)", EXE_OFF(g_setSelected), g_entSelFlagsOff);
    else logf_("squad leader: SetSelected not recognised - a new squad's leader-only selection only for a first box (not editor placement)");
    if (g_edSetLeaderSite) logf_("squad leader: editor squad-leader dialog SetLeader @ exe+0x%llX", EXE_OFF(g_edSetLeaderSite));
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
/* Double-click run. The mode comes from MODE_TABLE[run*? + chassis kind + 2*(unit in "running" mode)]: double-click in
   the unit's walk mode (brain+0x398 bit 14) asks for the double-flick walk speed, which for a fauna whose Run block has
   no faster entry looks like no run at all. A fauna that gets a run request (the move action's first argument is 0 for
   run, -1 for walk) is given the sprint mode as well: MODE_TABLE[3] = run in running mode (the "Fast" chassis entry).
   A stamina movement limit (vitality {Stamina {movement_limits {Fast ...}}}) still turns it into Normal when tired. */
static int g_wantSprint = 1, g_sprintMode = -1;
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
    const int32_t *table = (const int32_t *)(s + 7 + *(int32_t *)(s + 3));
    if (in_rdata(table) || in_data(table)) { g_sprintMode = table[3]; if (g_sprintMode < 0 || g_sprintMode > 7) g_sprintMode = -1; }
    logf_("squad pace: move action movement-change @ exe+0x%llX (order exe+0x%llX), give @ exe+0x%llX (exe+0x%llX), sprint mode %d",
          EXE_OFF(g_mcSite), EXE_OFF(ctor), EXE_OFF(g_giveSite), EXE_OFF(give), g_sprintMode);
    return 1;
}

/* ------------------------------------------------------------------------------------------------------------------
   First box over LOOSE units. The box select adds every boxed unit to the selection; when they are not one whole existing
   squad, it builds the engine's create-squad command (mode 3) from the box list - the command's constructor elects the
   new squad's leader (cmd+LEADER) - and sends it. Nothing then trims the selection to that leader, so a new fauna squad
   showed every member selected until the next box. Here the constructor call in the box select is wrapped: after the
   engine built the command, if its leader is fauna, every other boxed unit leaves the selection (SetSelected(e, 0) for a
   unit flagged selected, and the engine's selection add/remove - the call the box select uses to select a squad leader -
   for the list). UI only: the command is untouched, so the simulation and every multiplayer peer see what they saw before.
   ------------------------------------------------------------------------------------------------------------------ */
typedef void *(*BoxCmdCtorFn)(void *cmd, int mode, uint64_t r8, void *list);
static void **g_gameClient; static uint32_t g_selObjOff;          /* defined (found) in ui.c */
typedef uint8_t (*SelAddFn)(void *sel, void *entity, uint8_t add);
static uint8_t *g_boxCtorSite; static BoxCmdCtorFn g_boxCtorOrig; static SelAddFn g_selAdd; static uint32_t g_cmdLeaderOff;

static int find_box_create(void)
{
    if (!g_sqSelSite || !g_electEntry) { logf_("box create: squad finders first"); return 0; }
    /* after the IsVehicle site: ... mov r8b,1 ... call SelAdd ; jmp ; lea r9,[rbp+LIST] ; xor r8d,r8d ; mov edx,3 ; lea rcx,[rbp+CMD] ; call Ctor */
    uint8_t *site = NULL, *add = NULL;
    for (uint8_t *q = g_sqSelSite + 5; q < g_sqSelSite + 0x80 && !site; q++)
        if (q[0] == 0x4C && q[1] == 0x8D && q[2] == 0x4D && q[4] == 0x45 && q[5] == 0x33 && q[6] == 0xC0 && q[7] == 0xBA && *(uint32_t *)(q + 8) == 3 &&
            q[12] == 0x48 && q[13] == 0x8D && q[14] == 0x4D && q[16] == 0xE8) site = q + 16;
    for (uint8_t *q = g_sqSelSite + 5; site && q < site; q++)
        if (q[0] == 0xE8 && in_text(q + 5 + *(int32_t *)(q + 1)))
            for (uint8_t *r = q - 16; r < q; r++) if (r[0] == 0x41 && r[1] == 0xB0 && r[2] == 0x01) { add = q + 5 + *(int32_t *)(q + 1); break; }
    if (!site || !add) { logf_("box create: create-squad command in the box select not recognised"); return 0; }
    /* the constructor elects the leader and stores it: call ElectLeader ; mov [r64+LEADER],rax */
    uint8_t *ctor = site + 5 + *(int32_t *)(site + 1); uint32_t lo = 0;
    for (uint8_t *q = ctor; q < ctor + 0x180 && !lo; q++)
        if (q[0] == 0xE8 && q + 5 + *(int32_t *)(q + 1) == g_electEntry && (q[5] & 0xF0) == 0x40 && (q[5] & 8) && q[6] == 0x89 && (q[7] & 0xC0) == 0x40) lo = q[8];   /* mov [r64+disp8],rax (any base reg) */
    if (!lo) { logf_("box create: leader field of the create-squad command not recognised"); return 0; }
    g_boxCtorSite = site; g_boxCtorOrig = (BoxCmdCtorFn)ctor; g_selAdd = (SelAddFn)add; g_cmdLeaderOff = lo;
    logf_("box create: command ctor exe+0x%llX (called @ exe+0x%llX, leader at cmd+0x%X), selection add/remove exe+0x%llX",
          EXE_OFF(ctor), EXE_OFF(site), lo, EXE_OFF(add));
    return 1;
}

#ifndef FX_TEST
static int is_selected(const uint8_t *e) { return g_entSelFlagsOff && ((*(const uint32_t *)(e + g_entSelFlagsOff) >> 17) & 1); }

/* leave only the leader selected among b..en (the state an infantry squad's selection ends in). A unit flagged selected is
   deselected through SetSelected (flag + list); sel (optional) also removes it from the selection list directly, for units
   in the list whose flag is not set yet. Snapshot first: deselecting changes the selection, not b..en. */
static int deselect_except(uint8_t *leader, uint8_t **b, uint8_t **en, void *sel)
{
    if (!b || en < b || en - b > 64) return 0;
    uint8_t *mem[64]; int k = 0;
    for (uint8_t **q = b; q < en; q++) if (*q && *q != leader && (sel || is_selected(*q))) mem[k++] = *q;
    for (int i = 0; i < k; i++) {
        if (g_setSelected && is_selected(mem[i])) g_setSelected(mem[i], 0);
        if (sel && g_selAdd) g_selAdd(sel, mem[i], 0);          /* no-op when not in the list */
    }
    return k;
}

static uint8_t fx_box_isvehicle(void *leader)
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

static void *fx_box_cmd_ctor(void *cmd, int mode, uint64_t r8, void *list)
{
    void *r = g_boxCtorOrig(cmd, mode, r8, list);
    if (mode != 3 || !list || !g_wantSqSel || !g_gameClient || !*g_gameClient) return r;
    uint8_t *leader = *(uint8_t **)((uint8_t *)cmd + g_cmdLeaderOff);
    if (!leader || !is_fauna(leader)) return r;
    void *sel = *(void **)((uint8_t *)*g_gameClient + g_selObjOff);
    if (!sel) return r;
    int k = deselect_except(leader, ((uint8_t ***)list)[0], ((uint8_t ***)list)[1], sel);
    g_selAdd(sel, leader, 1);                                       /* no-op when already selected */
    static int n; if (n < 10) { n++; logf_("box create: new fauna squad from the box - leader %p kept selected, %d other unit(s) deselected", leader, k); }
    return r;
}

static int install_squad_select(void)
{
    if (!redirect_call(g_sqSelSite, (void *)fx_box_isvehicle, NULL, "squad select (IsVehicle)")) return 0;
    if (g_okElect && find_box_create() && redirect_call(g_boxCtorSite, (void *)fx_box_cmd_ctor, NULL, "box create (command ctor)"))
        logf_("box create: first box over loose fauna selects the new squad's leader ON");
    else logf_("box create: OFF - the first box over loose fauna keeps every unit selected");
    return 1;
}

/* ------------------------------------------------------------------------------------------------------------------
   fx_elect_leader: every caller of ElectLeader (squad creation, the squad update's re-election, the create-squad command,
   the map loader, anything else). The engine's answer is always taken first; the fauna rule only applies when it finds
   nobody:
     1. the list's vehicle-class units are all fauna, 2+ of them (any other case: the engine's NULL stands);
     2. the first member that passes the engine's own candidate test (ElectLeader on that member alone), infantry first;
     3. nobody passes (new units are not "operatable" - entity+0xDB4 bit 8 - until the engine activates them): the first
        in-scene member, infantry first - but ONLY when the members' squad has no leader. Otherwise the answer stays NULL
        and the engine keeps that leader: every SetLeader resets the squad anchor and fires an event on all members, so a
        forced answer there would churn every 0.3 s.
   A squad getting its first leader while that leader is selected (editor placement, the engine's squad of a box) is left
   leader-only selected - local UI state (the selected flag is the local player's), not simulation.
   ------------------------------------------------------------------------------------------------------------------ */
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
static uint8_t **squad_members(uint8_t *sq, uint8_t ***en)
{
    *en = *(uint8_t ***)(sq + g_squadMemOff + 8);
    return *(uint8_t ***)(sq + g_squadMemOff);
}
/* the squad the members belong to now (first member that has one), NULL while it is being created */
static uint8_t *members_squad(uint8_t **b, uint8_t **en)
{
    for (uint8_t **q = b; q < en; q++) { uint8_t *sq = *q ? squad_of(*q, NULL) : NULL; if (sq) return sq; }
    return NULL;
}
static uint8_t *elect_fauna_leader(uint8_t **b, uint8_t **en, int force, const char *why)
{
    int nf; uint8_t *r = NULL; const char *how = "eligible";
    if (!squad_all_fauna(b, en, &nf)) return NULL;
    for (int pass = 0; pass < 2 && !r; pass++)                   /* pass 0 infantry, pass 1 fauna */
        for (uint8_t **q = b; q < en && !r; q++) {
            uint8_t *e = *q;
            if (!e || ((e[ENT_FLAGS] & 8) != 0) != (pass == 1)) continue;
            uint8_t *one[3] = { (uint8_t *)q, (uint8_t *)(q + 1), (uint8_t *)(q + 1) };   /* std::vector {begin,end,cap} */
            if (g_electOrig(one) == e) r = e;
        }
    if (!r && force) {
        how = "forced, not yet operatable";
        for (int pass = 0; pass < 2 && !r; pass++)
            for (uint8_t **q = b; q < en && !r; q++) {
                uint8_t *e = *q;
                if (e && (e[ENT_FLAGS] & 2) && ((e[ENT_FLAGS] & 8) != 0) == (pass == 1)) r = e;
            }
    }
    static int n;
    if (n < 30) { n++; if (r) logf_("squad leader: fauna squad (%d members, %d fauna) - leader %p (%s; %s)", (int)(en - b), nf, r, how, why);
                       else logf_("squad leader: fauna squad (%d members, %d fauna) - nobody eligible yet (%s)", (int)(en - b), nf, why); }
    return r;
}
static uint8_t *fx_elect_leader(void *members)
{
    uint8_t *r = g_electOrig(members);
    if (r || !g_wantElect || !members) return r;
    uint8_t **b = ((uint8_t ***)members)[0], **en = ((uint8_t ***)members)[1];
    uint8_t *sq = members_squad(b, en);
    int hasLeader = sq && *(uint8_t **)(sq + g_squadLeadOff);
    r = elect_fauna_leader(b, en, !hasLeader, hasLeader ? "their squad has a leader: no forced pick" : "new leader");
    if (r && !hasLeader && is_selected(r)) {
        int k = deselect_except(r, b, en, NULL);
        static int n; if (k && n < 10) { n++; logf_("squad leader: new fauna leader %p is selected - %d other member(s) deselected", r, k); }
    }
    return r;
}
/* editor squad-leader dialog: no leader named for a fauna squad -> elect one */
static void fx_editor_set_leader(void *squad, void *leader)
{
    if (!leader && g_wantElect && squad) {
        uint8_t **en, **b = squad_members((uint8_t *)squad, &en);
        leader = elect_fauna_leader(b, en, 1, "editor leader dialog");
    }
    g_setLeader(squad, leader);
}
static void fx_squad_update(void *squad)
{
    uint8_t *s = (uint8_t *)squad;
    if (g_wantElect && s && !*(void **)(s + g_squadLeadOff) && !s[g_squadReelectOff]) {
        uint8_t **en, **b = squad_members(s, &en);
        int nf;
        if (squad_all_fauna(b, en, &nf)) s[g_squadReelectOff] = 1;   /* engine's own "elect again": it re-elects below */
    }
    g_squadUpdOrig(squad);
}
/* runtime: squad pace */
static void fauna_sprint_order(uint8_t *e, int mode);          /* sprint.c */
static _Thread_local int t_mcA, t_mcMode, t_mcValid; static _Thread_local uint8_t t_mcFlag;
static void *fx_move_change(int a, int mode, uint8_t flag)
{
    t_mcA = a; t_mcMode = mode; t_mcFlag = flag; t_mcValid = 1;
    return g_mcCtor(a, mode, flag);
}
static uint64_t fx_move_give(void *entity, void *order, void *ctx)
{
    int valid = t_mcValid; t_mcValid = 0;
    uint64_t r = g_giveOrder(entity, order, ctx);
    if (!valid || !entity) return r;
    uint8_t *e = (uint8_t *)entity; int m;
    if (g_wantSprint && g_sprintMode >= 0 && t_mcA == 0 && t_mcMode != g_sprintMode && is_fauna(e)) {   /* double-click run */
        void *o = g_mcCtor(0, g_sprintMode, t_mcFlag);
        if (o) g_giveOrder(e, o, ctx);                            /* a second movement change replaces the first */
        static int n; if (n < 10) { n++; logf_("fauna run: %p double-click run, movement mode %d -> %d (sprint)", e, t_mcMode, g_sprintMode); }
        t_mcMode = g_sprintMode;                                  /* squad members below get the same */
    }
    if (is_fauna(e)) fauna_sprint_order(e, t_mcMode);             /* brain movement mode: sprint, or back from sprint */
    if (!g_wantPace) return r;
    uint8_t *sq = squad_of(e, &m);
    if (!sq || *(uint8_t **)(sq + g_squadLeadOff) != e || m < 2) return r;      /* only the leader of a squad */
    uint8_t **en, **b = squad_members(sq, &en);
    int nf;
    if (!squad_all_fauna(b, en, &nf)) return r;
    uint8_t *mem[64]; int k = 0;                                  /* snapshot: giving orders must not walk a changing vector */
    for (uint8_t **q = b; q < en && k < 64; q++) if (*q && *q != e && ((*q)[ENT_FLAGS] & 2) && is_fauna(*q)) mem[k++] = *q;
    for (int i = 0; i < k; i++) { void *o = g_mcCtor(t_mcA, t_mcMode, t_mcFlag); if (o) g_giveOrder(mem[i], o, ctx); fauna_sprint_order(mem[i], t_mcMode); }
    static int logged; if (k && logged < 10) { logged++; logf_("squad pace: leader %p movement mode %d -> %d fauna squad members", e, t_mcMode, k); }
    return r;
}

static int install_squad_elect(void)
{
    void *orig = NULL;
    if (install_jmp5(g_electEntry, (void *)fx_elect_leader, &orig, "squad leader (ElectLeader entry)")) {
        g_electOrig = (ElectFn)orig;                                /* the trampoline: the engine's own election */
        logf_("squad leader: ElectLeader hooked at its entry (covers all %d call sites)", g_electSiteN);
    } else {                                                        /* fallback: every call site (g_electOrig stays ElectLeader) */
        for (int i = 0; i < g_electSiteN; i++)
            if (!redirect_call(g_electSites[i], (void *)fx_elect_leader, NULL, "squad leader (elect call site)")) return 0;
        logf_("squad leader: ElectLeader call sites redirected (%d)", g_electSiteN);
    }
    return redirect_call(g_squadUpdSite, (void *)fx_squad_update, NULL, "squad leader (squad update)")
        && (!g_edSetLeaderSite || redirect_call(g_edSetLeaderSite, (void *)fx_editor_set_leader, NULL, "squad leader (editor leader dialog)"));
}

#endif /* FX_TEST */
