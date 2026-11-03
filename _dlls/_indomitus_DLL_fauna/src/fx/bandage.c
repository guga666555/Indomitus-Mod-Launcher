/* GEM Fauna - bandages from the inventory
 * Part of the unity build: #included by gem_fauna.c, not compiled on its own.
 *
 * Double-clicking an item in a unit's inventory window queues the engine's "UseItem" inventory operation
 * (Inventory::eOperation_UseItem; the window's click handler is GameControl::eInventoryBoxView slot 39, which needs
 * the owner alive and "operatable", entity+0xDB4 bit 8). The operation's process step (vtable slot 9) runs once
 * with its state == 1: for a human and a "bandage" item it gives the self-bandage order; a vehicle-class owner
 * (so a fauna unit) only gets the "load into a vehicle holder" path, so nothing happens.
 *
 * GEM Fauna wraps slot 9: when the owner is fauna and the item's class tags hold "bandage", one bandage is taken out
 * of the inventory (the engine's RemoveMany, as the console "inventory remove" command does) and the unit is healed
 * by fauna_bandage_heal_percent of its max health (the engine's HealPercent, the same call the medic "care" action
 * uses: capped at max, fires the health-changed event for the HUD). At full health nothing is used.
 * Any other item or owner goes to the engine unchanged.
 *
 * Multiplayer: the host (or dedicated server) runs the operation - a client's double-click reaches it as an interface
 * action; clients never run it themselves (the engine's own "is client" test, as in its inventory-remove code). On the
 * host the engine's step runs first (its operation-state sync to the clients), then the bandage is used as above and the
 * result is sent with the engine's own commands:
 *   eInventoryRemoveCommand {fauna, the bandage's class tags, count}: every client removes the same item (vanilla
 *     command, vanilla execute - the engine sends it the same way when an action takes an item out of an inventory);
 *   eUseMedkitCommand {fauna, new health}: the medic heal replication. Its execute only accepts human targets, so
 *     GEM Fauna wraps it on every peer: a fauna target gets the sent health (HealPercent by the difference, so the HUD
 *     event fires). A peer without GEM Fauna ignores it (and is out of sync anyway: squads are simulation too).
 * If those engine pieces are not recognised, fauna bandages stay off in a network session (log line).
 *
 * Diagnostics: the first double-clicks on a fauna inventory are logged (slot 39 wrapper, UI only), so a click
 * the engine drops before it becomes an operation shows up in the log.
 */

/* ---- engine bindings (found by find_bandage) ---- */
typedef void     (*OpTickFn)(void *op, void *a, void *b, void *c);
typedef uint8_t  (*BoolFn)(void);
typedef uint8_t  (*HasTagFn)(const void *tags, const void *tag);
typedef uint8_t  (*ViewTestFn)(void *view);
typedef void   **(*ViewDefFn)(void *view);                          /* -> pointer to the item's def entity */
typedef uint8_t *(*ViewCellFn)(void *view);
typedef void    *(*RemoveManyFn)(void *view, void *defCount);       /* {def, int count} -> removed record */
typedef void     (*ViewResetFn)(void *view);
typedef float    (*HealPctFn)(void *entity, float percent);         /* returns the percent actually applied */
typedef uint64_t (*BoxClickFn)(void *view, void *event);
typedef void    *(*CmdTargetFn)(void *cmd, void *entity);           /* command vt[5]: set target entity, returns cmd */
typedef void     (*CmdFn)(void *cmd);                               /* command vt[1] send / vt[4] execute */
typedef void     (*TagsCopyFn)(void *dst, const void *src);
typedef void     (*TagsDtorFn)(void *tags);

static void   **g_vtUseItem; static OpTickFn g_useItemOrig;
static uint32_t g_opStateOff, g_opUserOff, g_opViewOff, g_defTagsOff, g_cellCountOff, g_cellHolderOff;
static BoolFn   g_isNet;
static HasTagFn g_hasTag;
static ViewTestFn g_viewIsEmpty, g_viewIsReserved;
static ViewDefFn  g_viewGetDef;
static ViewCellFn g_viewCurrent;
static RemoveManyFn g_removeMany;
static ViewResetFn  g_viewReset;
static HealPctFn    g_healPercent;
static void   **g_vtBoxView; static BoxClickFn g_boxClickOrig; static uint32_t g_boxOwnerOff, g_boxActiveOff;
static int g_okBandage, g_bandagePct = 15;
/* multiplayer (find_bandage_net) */
static BoolFn   g_isClient;
static void   **g_vtRemoveCmd; static uint32_t g_removeCmdId, g_removeCmdTagsOff, g_removeCmdCountOff;
static TagsCopyFn g_tagsCopy; static TagsDtorFn g_tagsDtor;
static void   **g_vtMedkitCmd; static uint32_t g_medkitCmdId, g_entNetIdOff; static CmdFn g_medkitExecOrig;
static int g_okBandageNet;
/* eUseMedkitCommand layout (checked in find_bandage_net): +0x10 item entity, +0x18 target net id, +0x1C target health
   (absolute), +0x20 item pool, +0x24 count, +0x28 target entity (filled by the receiver from the id) */
#define MEDKIT_ITEM   0x10
#define MEDKIT_TID    0x18
#define MEDKIT_HP     0x1C
#define MEDKIT_TARGET 0x28
#define MEDKIT_SIZE   0x30

/* E8 at p whose target looks like a function entry (.pdata start, or after int3 padding: leaf functions have none) */
static uint8_t *call_target_fn(uint8_t *p)
{
    if (p[0] != 0xE8) return NULL;
    uint8_t *t = p + 5 + *(int32_t *)(p + 1);
    if (!in_text(t)) return NULL;
    uint32_t f = func_of(t);
    return (f && g_base + f == t) || t[-1] == 0xCC ? t : NULL;
}
static uint8_t *next_fn_call(uint8_t *p, size_t window, uint8_t **site)
{
    for (size_t i = 0; i < window; i++) { uint8_t *t = call_target_fn(p + i); if (t) { if (site) *site = p + i; return t; } }
    return NULL;
}

/* UseItem process step: cmp dword [rcx+STATE],1 ; jne ; call IsNet ... lea rcx,[rOP+VIEW] ; call IsEmpty / GetDef /
   Current ... lea rdx,[rip+Tag "bandage"] ; mov rcx,DEFTAGS ; call HasTag */
static int find_bandage_op(void)
{
    g_vtUseItem = find_vtable_rtti(".?AUeOperation_UseItem@Inventory@@");
    if (!g_vtUseItem || !g_tagBandage) { logf_("bandage: UseItem operation %s", g_vtUseItem ? "ok, but no static Tag 'bandage'" : "class not found"); return 0; }
    uint8_t *f = (uint8_t *)g_vtUseItem[9], *p, *s;
    if (!in_text(f)) { logf_("bandage: UseItem slot 9 not code"); return 0; }
    for (p = f; p < f + 0x30; p++) if (p[0] == 0x83 && p[1] == 0xB9 && p[6] == 0x01) break;      /* cmp dword [rcx+imm32],1 */
    if (p >= f + 0x30) { logf_("bandage: UseItem state test not recognised"); return 0; }
    g_opStateOff = *(uint32_t *)(p + 2);
    g_isNet = (BoolFn)next_fn_call(p + 7, 0x10, NULL);
    /* the op's item view: the first 'lea rcx,[r64+disp8]' (no SIB) followed by a call; the next calls on the same view
       are IsEmpty, GetDef, Current, in that order */
    uint8_t *fns[3] = { 0 }; int nf = 0; int view = -1; uint8_t *getDefSite = NULL;
    for (p = f; p < f + 0x400 && nf < 3; p++) {
        if (p[0] != 0x48 || p[1] != 0x8D || (p[2] & 0xF8) != 0x48 || (p[2] & 7) == 4) continue;
        uint8_t *site, *t = next_fn_call(p + 4, 16, &site);
        if (!t) continue;
        if (view < 0) view = p[3];
        if (p[3] != view) continue;
        int dup = 0; for (int i = 0; i < nf; i++) dup |= fns[i] == t;
        if (dup) continue;
        if (nf == 1) getDefSite = site;
        fns[nf++] = t; p = site;
    }
    /* the op's owner: mov r64,[rOP+disp8] ... test byte [that r64+0x30],2 (alive) */
    for (p = f; p < f + 0x200 && !g_opUserOff; p++)
        if (p[0] == 0x48 && p[1] == 0x8B && (p[2] & 0xC0) == 0x40 && (p[2] & 7) != 4)
            for (uint8_t *q = p + 4; q < p + 0x24; q++)
                if (q[0] == 0xF6 && (q[1] & 0xF8) == 0x40 && (q[1] & 7) == ((p[2] >> 3) & 7) && q[2] == ENT_FLAGS && q[3] == 2) { g_opUserOff = p[3]; break; }
    /* the def's class tags: lea r64,[r64+disp32] right after GetDef */
    for (p = getDefSite ? getDefSite + 5 : NULL; p && p < getDefSite + 0x28; p++)
        if ((p[0] & 0xF8) == 0x48 && p[1] == 0x8D && (p[2] & 0xC0) == 0x80 && (p[2] & 7) != 4) { uint32_t o = *(uint32_t *)(p + 3); if (o >= 0x400 && o < 0x4000) { g_defTagsOff = o; break; } }
    /* HasTag: the call after 'lea rdx,[rip+Tag bandage]' */
    for (p = f, s = NULL; p < f + 0x500; p++)
        if (p[0] == 0x48 && p[1] == 0x8D && p[2] == 0x15 && p + 7 + *(int32_t *)(p + 3) == g_tagBandage) { s = p; break; }
    g_hasTag = s ? (HasTagFn)next_fn_call(s + 7, 12, NULL) : NULL;
    if (nf < 3 || !g_isNet || !g_opUserOff || !g_defTagsOff || !g_hasTag) {
        logf_("bandage: UseItem layout not recognised (view calls %d, IsNet %d, owner +0x%X, def tags +0x%X, HasTag %d)",
              nf, g_isNet != NULL, g_opUserOff, g_defTagsOff, g_hasTag != NULL);
        return 0;
    }
    g_opViewOff = (uint32_t)view;
    g_viewIsEmpty = (ViewTestFn)fns[0]; g_viewGetDef = (ViewDefFn)fns[1]; g_viewCurrent = (ViewCellFn)fns[2];
    logf_("bandage: UseItem step exe+0x%llX: state +0x%X, owner +0x%X, item view +0x%X (IsEmpty exe+0x%llX, GetDef exe+0x%llX, Current exe+0x%llX), "
          "def tags +0x%X, HasTag exe+0x%llX, IsNet exe+0x%llX", EXE_OFF(f), g_opStateOff, g_opUserOff, g_opViewOff, EXE_OFF(fns[0]), EXE_OFF(fns[1]),
          EXE_OFF(fns[2]), g_defTagsOff, EXE_OFF(g_hasTag), EXE_OFF(g_isNet));
    return 1;
}

/* the console "inventory remove" command (string "Can't remove reserved item"): call IsReserved ; test al,al ; ... lea [str] ...
   call GetDef ; ... call RemoveMany ; ... sub r32,[rax+COUNT] ... call r8 (delete record) ; ... call Reset */
static int find_bandage_remove(void)
{
    uint8_t *str = find_cstr("Can't remove reserved item \"%1%\"");
    uint8_t *lea = str ? find_lea_to(str, 0) : NULL;
    if (!lea) { logf_("bandage: console 'inventory remove' not found"); return 0; }
    for (uint8_t *p = lea - 9; p > lea - 0x60; p--)                                               /* nearest call ; test al,al */
        if (p[5] == 0x84 && p[6] == 0xC0 && call_target_fn(p)) { g_viewIsReserved = (ViewTestFn)call_target_fn(p); break; }
    uint8_t *site = NULL;
    for (uint8_t *p = lea; p < lea + 0x1C0 && !g_removeMany; p++) {
        if (call_target_fn(p) != (uint8_t *)g_viewGetDef) continue;
        uint8_t *rm = next_fn_call(p + 5, 0x30, &site);
        if (!rm) continue;
        for (uint8_t *q = site + 5; q < site + 0x19; q++)
            if (q[0] == 0x2B && (q[1] & 0xC7) == 0x40) { g_cellCountOff = q[2]; g_removeMany = (RemoveManyFn)rm; break; }
    }
    if (g_removeMany)
        for (uint8_t *q = site + 5; q < site + 0x30; q++)
            if (q[0] == 0x41 && q[1] == 0xFF && q[2] == 0xD0) { g_viewReset = (ViewResetFn)next_fn_call(q + 3, 0x14, NULL); break; }
    /* item in a holder (hands, weapon slot): call Current ; cmp qword [rax+X],0 / cmp [rax+X],r64 */
    uint32_t fs = func_of(lea); uint8_t *fb = fs ? g_base + fs : lea - 0x600;
    for (uint8_t *p = fb; p < lea + 0x200 && !g_cellHolderOff; p++) {
        if (call_target_fn(p) != (uint8_t *)g_viewCurrent) continue;
        uint8_t *q = p + 5;
        if (q[0] == 0x48 && q[1] == 0x83 && q[2] == 0x78 && q[4] == 0) g_cellHolderOff = q[3];
        else if ((q[0] & 0xFB) == 0x48 && q[1] == 0x39 && (q[2] & 0xC7) == 0x40) g_cellHolderOff = q[3];
    }
    if (!g_viewIsReserved || !g_removeMany || !g_viewReset) {
        logf_("bandage: inventory remove layout not recognised (IsReserved %d, RemoveMany %d, Reset %d)", g_viewIsReserved != NULL, g_removeMany != NULL, g_viewReset != NULL);
        return 0;
    }
    logf_("bandage: IsReserved exe+0x%llX, RemoveMany exe+0x%llX (count +0x%X), Reset exe+0x%llX, holder %s+0x%X",
          EXE_OFF(g_viewIsReserved), EXE_OFF(g_removeMany), g_cellCountOff, EXE_OFF(g_viewReset), g_cellHolderOff ? "" : "NOT found ", g_cellHolderOff);
    return 1;
}

/* HealPercent(entity, pct): cur = GetHealth ; max = GetMax ; unit = max/100 ; pct = min(pct, (max-cur)/unit) ; SetHealth(cur+pct*unit) */
static int find_heal_percent(void)
{
    static const uint8_t sig[] = { 0xF3,0x0F,0x5C,0xC6, 0xF3,0x0F,0x5E,0xC2, 0xF3,0x0F,0x5D,0xF8 };   /* subss xmm0,xmm6 ; divss xmm0,xmm2 ; minss xmm7,xmm0 */
    uint8_t *hit = NULL; int n = 0;
    for (uint8_t *p = g_text.p; (p = find_bytes(p, g_text.p + g_text.n - p, sig, sizeof sig)); p++)
        if (p[-8] == 0xF3 && p[-7] == 0x0F && p[-6] == 0x5E && p[-5] == 0x15) { hit = p; n++; }      /* divss xmm2,[rip+100.0] before */
    uint32_t f = n == 1 ? func_of(hit) : 0;
    if (!f) { logf_("bandage: HealPercent not found (%d candidates)", n); return 0; }
    uint8_t *fn = g_base + f; int calls = 0;
    for (uint8_t *p = fn; p < hit; p++) { uint8_t *t = call_target_fn(p); if (t == (uint8_t *)g_getHealth || t == (uint8_t *)g_getMaxHealth) calls++; }
    if (calls < 2) { logf_("bandage: HealPercent candidate exe+0x%llX does not read the health getters", EXE_OFF(fn)); return 0; }
    g_healPercent = (HealPctFn)fn;
    logf_("bandage: HealPercent exe+0x%llX", EXE_OFF(fn));
    return 1;
}

/* diagnostics only: GameControl::eInventoryBoxView slot 39 (the item click): cmp eax,0x200 ... mov rax,[rV+OWNER] ;
   test rax,rax ; je ; test byte [rax+0x30],2 ; je ; mov eax,[rax+0xDB4] (operatable bit 8); cmp byte [rV+ACTIVE],0 */
static void find_bandage_click(void)
{
    g_vtBoxView = find_vtable_rtti(".?AVeInventoryBoxView@GameControl@@");
    uint8_t *f = g_vtBoxView ? (uint8_t *)g_vtBoxView[39] : NULL;
    if (!f || !in_text(f)) { g_vtBoxView = NULL; return; }
    static const uint8_t dbl[] = { 0x3D, 0x00, 0x02, 0x00, 0x00 };
    if (!find_bytes(f, 0x30, dbl, sizeof dbl)) { g_vtBoxView = NULL; return; }
    for (uint8_t *p = f; p < f + 0x200 && !g_boxOwnerOff; p++)
        if (p[0] == 0x48 && p[1] == 0x8B && (p[2] & 0xF8) == 0x80 && p[7] == 0x48 && p[8] == 0x85 && p[9] == 0xC0 && p[10] == 0x74 &&
            p[12] == 0xF6 && p[13] == 0x40 && p[14] == ENT_FLAGS && p[15] == 2) g_boxOwnerOff = *(uint32_t *)(p + 3);
    for (uint8_t *p = f; p < f + 0x200 && !g_boxActiveOff; p++)
        if (p[0] == 0x80 && (p[1] & 0xF8) == 0xB8 && p[6] == 0) g_boxActiveOff = *(uint32_t *)(p + 2);
    if (!g_boxOwnerOff) g_vtBoxView = NULL;
}

/* command stack object near a 'lea rax,[rip+vt]': mov [rsp+CMD],rax right after; the id is 'mov dword [rsp+CMD+8],imm32'
   in [lea-0x30, lea+0x30) */
static int cmd_stack(uint8_t *lea, uint32_t *cmdOff, uint32_t *id)
{
    uint32_t c = 0;
    for (uint8_t *p = lea + 7; p < lea + 0x30 && !c; p++) {
        if (p[0] == 0x48 && p[1] == 0x89 && p[2] == 0x84 && p[3] == 0x24) c = *(uint32_t *)(p + 4);
        else if (p[0] == 0x48 && p[1] == 0x89 && p[2] == 0x44 && p[3] == 0x24) c = p[4];
    }
    if (!c) return 0;
    for (uint8_t *p = lea - 0x30; p < lea + 0x30; p++) {
        uint32_t d = 0, imm = 0;
        if (p[0] == 0xC7 && p[1] == 0x84 && p[2] == 0x24) { d = *(uint32_t *)(p + 3); imm = *(uint32_t *)(p + 7); }
        else if (p[0] == 0xC7 && p[1] == 0x44 && p[2] == 0x24) { d = p[3]; imm = *(uint32_t *)(p + 4); }
        if (d == c + 8 && imm && imm < 0x400) { *cmdOff = c; *id = imm; return 1; }
    }
    return 0;
}
/* lea rcx,[rsp+disp] ; ... call  -> the call's target, searching [from, to) */
static uint8_t *call_after_lea_rcx_rsp(uint8_t *from, uint8_t *to, uint32_t disp, uint8_t **next)
{
    for (uint8_t *p = from; p < to; p++) {
        int len = (p[0] == 0x48 && p[1] == 0x8D && p[2] == 0x8C && p[3] == 0x24 && *(uint32_t *)(p + 4) == disp) ? 8 :
                  (disp < 0x80 && p[0] == 0x48 && p[1] == 0x8D && p[2] == 0x4C && p[3] == 0x24 && p[4] == disp) ? 5 : 0;
        if (!len) continue;
        uint8_t *site, *t = next_fn_call(p + len, 0x18, &site);
        if (t) { if (next) *next = site + 5; return t; }
    }
    return NULL;
}
/* the function holding the lea of vt that also calls fn (or any non-constructor function if fn is NULL) */
static uint8_t *vt_user(void **vt, const void *fn, int k0)
{
    for (int k = k0; k < 8; k++) {
        uint8_t *lea = find_lea_to(vt, k);
        if (!lea) return NULL;
        uint32_t f = func_of(lea);
        if (!f) continue;
        const FxRtf *rf = rtf_find(f);
        uint8_t *b = g_base + f, *e = rf ? g_base + rf->e : lea + 0x100;
        if (!fn) return lea;
        for (uint8_t *p = b; p + 5 <= e; p++) if (call_target_fn(p) == fn) return lea;
    }
    return NULL;
}

static int find_bandage_net(void)
{
    /* eInventoryRemoveCommand: execute = GetBox ; view ; Find(tags at cmd+TAGS) ; RemoveMany({def, [cmd+COUNT]}) */
    g_vtRemoveCmd = find_vtable_rtti(".?AUeInventoryRemoveCommand@@");
    uint8_t *ex = g_vtRemoveCmd ? (uint8_t *)g_vtRemoveCmd[4] : NULL;
    if (!ex || !in_text(ex)) { logf_("bandage net: eInventoryRemoveCommand not found"); return 0; }
    int callsRm = 0;
    for (uint8_t *p = ex; p < ex + 0xA0; p++) {
        if (p[0] == 0x8B && p[1] == 0x83 && !g_removeCmdCountOff) g_removeCmdCountOff = *(uint32_t *)(p + 2);
        if (call_target_fn(p) == (uint8_t *)g_removeMany) callsRm = 1;
    }
    /* the engine's sender (an action taking one item out of an inventory): call IsClient first ; ... lea vt ; Tags copy ;
       vt[5] set target ; vt[1] send ; ~Tags */
    uint8_t *lea = vt_user(g_vtRemoveCmd, g_removeMany, 0);
    uint32_t cmdOff = 0;
    if (lea && cmd_stack(lea, &cmdOff, &g_removeCmdId)) {
        uint32_t f = func_of(lea);
        g_isClient = f ? (BoolFn)next_fn_call(g_base + f, 0x40, NULL) : NULL;
        uint8_t *nx = NULL;
        g_removeCmdTagsOff = 0x18;
        g_tagsCopy = (TagsCopyFn)call_after_lea_rcx_rsp(lea - 0x10, lea + 0x40, cmdOff + g_removeCmdTagsOff, &nx);
        g_tagsDtor = nx ? (TagsDtorFn)call_after_lea_rcx_rsp(nx, nx + 0x60, cmdOff + g_removeCmdTagsOff, NULL) : NULL;
    }
    if (!callsRm || !g_removeCmdCountOff || g_removeCmdCountOff < 0x18 + 0x300 || !g_removeCmdId || !g_isClient || !g_tagsCopy || !g_tagsDtor) {
        logf_("bandage net: inventory remove command not recognised (execute %d, count +0x%X, id 0x%X, IsClient %d, Tags copy %d / dtor %d)",
              callsRm, g_removeCmdCountOff, g_removeCmdId, g_isClient != NULL, g_tagsCopy != NULL, g_tagsDtor != NULL);
        return 0;
    }
    /* eUseMedkitCommand: execute reads mov rdi,[rbx+0x28] (target) and movss xmm1,[rbx+0x1C] (health); the sender (medic
       care) stores the target's net id from mov ebx,[target+NETID] */
    g_vtMedkitCmd = find_vtable_rtti(".?AUeUseMedkitCommand@@");
    ex = g_vtMedkitCmd ? (uint8_t *)g_vtMedkitCmd[4] : NULL;
    static const uint8_t tgt[] = { 0x48, 0x8B, 0x7B, MEDKIT_TARGET }, hp[] = { 0xF3, 0x0F, 0x10, 0x4B, MEDKIT_HP };
    if (!ex || !in_text(ex) || !find_bytes(ex, 0x40, tgt, 4) || !find_bytes(ex, 0xC0, hp, 5)) { logf_("bandage net: eUseMedkitCommand not recognised"); return 0; }
    for (int k = 0; k < 6 && !g_entNetIdOff; k++) {
        uint8_t *l = find_lea_to(g_vtMedkitCmd, k);
        if (!l) break;
        uint32_t c = 0, id = 0;
        if (!cmd_stack(l, &c, &id)) continue;
        for (uint8_t *p = l - 0x30; p < l; p++)
            if (p[0] == 0x8B && (p[1] & 0xC0) == 0x80 && (p[1] & 7) != 4) { uint32_t d = *(uint32_t *)(p + 2); if (d >= 0x40 && d < 0x400) { g_entNetIdOff = d; g_medkitCmdId = id; } }
    }
    if (!g_entNetIdOff) { logf_("bandage net: medic heal sender not recognised (entity net id)"); return 0; }
    logf_("bandage net: IsClient exe+0x%llX; eInventoryRemoveCommand id 0x%X (tags +0x%X, count +0x%X, Tags copy exe+0x%llX / dtor exe+0x%llX); "
          "eUseMedkitCommand id 0x%X (execute exe+0x%llX), entity net id +0x%X", EXE_OFF(g_isClient), g_removeCmdId, g_removeCmdTagsOff,
          g_removeCmdCountOff, EXE_OFF(g_tagsCopy), EXE_OFF(g_tagsDtor), g_medkitCmdId, EXE_OFF(ex), g_entNetIdOff);
    return 1;
}

static int find_bandage(void)
{
    if (!find_bandage_op() || !find_bandage_remove() || !find_heal_percent()) return 0;
    g_okBandageNet = find_bandage_net();
    find_bandage_click();
    logf_("bandage: multiplayer %s; inventory click diagnostics %s", g_okBandageNet ? "supported (host sends the result)" : "OFF (engine commands not recognised)",
          g_vtBoxView ? "found" : "not found (no log of dropped clicks)");
    return 1;
}

#ifndef FX_TEST
static void healing_start(uint8_t *e);                                  /* healing events, below */
/* host: tell the clients (engine commands; see the header) */
static void send_bandage_result(uint8_t *user, void *def, int removed, float health)
{
    __attribute__((aligned(16))) uint8_t rc[0x400]; memset(rc, 0, sizeof rc);
    if (g_removeCmdCountOff + 4 > sizeof rc) return;
    *(void ***)rc = g_vtRemoveCmd; *(uint32_t *)(rc + 8) = g_removeCmdId;
    g_tagsCopy(rc + g_removeCmdTagsOff, (const uint8_t *)def + g_defTagsOff);
    *(int32_t *)(rc + g_removeCmdCountOff) = removed;
    void *c = ((CmdTargetFn)g_vtRemoveCmd[5])(rc, user);
    ((CmdFn)(*(void ***)c)[1])(c);
    g_tagsDtor(rc + g_removeCmdTagsOff);

    __attribute__((aligned(16))) uint8_t mc[0x40]; memset(mc, 0, sizeof mc);
    *(void ***)mc = g_vtMedkitCmd; *(uint32_t *)(mc + 8) = g_medkitCmdId;
    *(void **)(mc + MEDKIT_ITEM) = user;                                /* any entity every peer knows: the fauna itself */
    *(uint32_t *)(mc + MEDKIT_TID) = *(uint32_t *)(user + g_entNetIdOff);
    *(float *)(mc + MEDKIT_HP) = health;
    ((CmdFn)g_vtMedkitCmd[1])(mc);
}

/* every peer: the medic heal replication, for a fauna target too (the engine's execute skips non-humans) */
static void fx_medkit_exec(void *cmd)
{
    uint8_t *t = *(uint8_t **)((uint8_t *)cmd + MEDKIT_TARGET);
    if (t && is_fauna(t)) {
        void *vit = entity_vitality(t);
        if (!vit) return;
        float want = *(float *)((uint8_t *)cmd + MEDKIT_HP), cur = g_getHealth(vit), max = g_getMaxHealth(vit);
        if (max > 0.f && want > 0.f && (want - cur > 1e-3f || cur - want > 1e-3f)) g_healPercent(t, (want - cur) / (max / 100.f));
        if (want - cur > 1e-3f) healing_start(t);
        static int n; if (n < 20) { n++; logf_("bandage net: fauna %p health set by the host: %.0f -> %.0f of %.0f", t, cur, g_getHealth(vit), max); }
        return;
    }
    g_medkitExecOrig(cmd);
}

/* one bandage out of the item view, then the heal (and on the host, the result to the clients); 1 = used */
static int fauna_use_bandage(uint8_t *user, void *view, void *def, int host)
{
    static int nlog;
    void *vit = entity_vitality(user);
    if (!vit) { if (nlog < 20) { nlog++; logf_("bandage: fauna %p has no vitality - bandage not used", user); } return 0; }
    float cur = g_getHealth(vit), max = g_getMaxHealth(vit);
    if (!(max > 0.f) || !(cur > 0.f)) return 0;
    if (cur >= max) { if (nlog < 20) { nlog++; logf_("bandage: fauna %p at full health (%.0f) - bandage kept", user, cur); } return 0; }
    if (g_viewIsReserved(view)) { if (nlog < 20) { nlog++; logf_("bandage: the bandage is reserved - not used"); } return 0; }
    uint8_t *cell = g_viewCurrent(view);
    if (cell && g_cellHolderOff && *(void **)(cell + g_cellHolderOff)) { if (nlog < 20) { nlog++; logf_("bandage: the bandage is in a holder - not used"); } return 0; }
    struct { void *def; int32_t count; int32_t pad; } req = { def, 1, 0 };
    uint8_t *rec = (uint8_t *)g_removeMany(view, &req);
    int removed = rec ? *(int32_t *)(rec + g_cellCountOff) : 0;
    if (rec) (*(void (**)(void *, int))*(void **)rec)(rec, 1);       /* the record's deleting destructor, as the console does */
    g_viewReset(view);
    if (removed < 1) { if (nlog < 20) { nlog++; logf_("bandage: fauna %p - the engine removed no bandage, no heal", user); } return 0; }
    float applied = g_healPercent(user, (float)g_bandagePct);
    if (applied > 0.f) healing_start(user);
    if (host) send_bandage_result(user, def, removed, g_getHealth(vit));
    if (nlog < 20) { nlog++; logf_("bandage: fauna %p used a bandage: health %.0f -> %.0f of %.0f (+%.1f%% of max)%s", user, cur, g_getHealth(vit), max, applied,
                                   host ? " - sent to the clients" : ""); }
    return 1;
}

static void fx_useitem_tick(void *op, void *a, void *b, void *c)
{
    uint8_t *o = (uint8_t *)op;
    if (o && *(int32_t *)(o + g_opStateOff) == 1) {
        uint8_t *user = *(uint8_t **)(o + g_opUserOff);
        void *view = o + g_opViewOff;
        if (user && is_fauna(user) && !g_viewIsEmpty(view)) {
            void **pdef = g_viewGetDef(view); uint8_t *def = pdef ? (uint8_t *)*pdef : NULL;
            if (def && g_hasTag(def + g_defTagsOff, g_tagBandage)) {
                if (g_isClient && g_isClient()) {                     /* a client (it should never get here): the host
                                                                         uses the bandage and sends the result */
                    static int once; if (!once) { once = 1; logf_("bandage: UseItem step on a client - left to the engine, the host decides"); }
                } else if (!g_isNet()) {                              /* single player / editor */
                    *(int32_t *)(o + g_opStateOff) = -1;              /* what the engine's step does first: run once */
                    fauna_use_bandage(user, view, def, 0);
                    return;
                } else if (g_okBandageNet) {                          /* host / dedicated server */
                    g_useItemOrig(op, a, b, c);                       /* engine first: op-state sync to the clients, state -> -1;
                                                                         a vehicle-class owner's holder path takes no bandage */
                    if (!g_viewIsEmpty(view)) fauna_use_bandage(user, view, def, 1);
                    return;
                } else {
                    static int once; if (!once) { once = 1; logf_("bandage: network session - fauna bandages are off (engine commands not recognised)"); }
                }
            }
        }
    }
    g_useItemOrig(op, a, b, c);
}

static uint64_t fx_box_click(void *view, void *ev)
{
    if (ev && *(const uint32_t *)((const uint8_t *)ev + 4) == 0x200) {
        uint8_t *owner = *(uint8_t **)((uint8_t *)view + g_boxOwnerOff);
        static int n;
        if (owner && n < 10 && is_fauna(owner)) {
            n++;
            logf_("bandage: double-click in fauna %p's inventory (operatable %d, window active %d) - the engine sends UseItem only when both are 1",
                  owner, g_entSelFlagsOff ? (int)((*(uint32_t *)(owner + g_entSelFlagsOff) >> 8) & 1) : -1,
                  g_boxActiveOff ? *((uint8_t *)view + g_boxActiveOff) != 0 : -1);
        }
    }
    return g_boxClickOrig(view, ev);
}

static int install_bandage(void)
{
    if (!vt_hook(g_vtUseItem, 9, (void *)fx_useitem_tick, (void **)&g_useItemOrig)) { logf_("bandage: UseItem vtable not writable"); return 0; }
    if (g_vtBoxView && !vt_hook(g_vtBoxView, 39, (void *)fx_box_click, (void **)&g_boxClickOrig)) g_vtBoxView = NULL;
    if (g_okBandageNet && !vt_hook(g_vtMedkitCmd, 4, (void *)fx_medkit_exec, (void **)&g_medkitExecOrig)) {
        g_okBandageNet = 0; logf_("bandage net: eUseMedkitCommand vtable not writable - multiplayer OFF");
    }
    return 1;
}
#endif /* FX_TEST */

/* ---- healing events (0.10.0) ----
 * The engine tells a soldier's interaction script that he is being healed with the events "start_healing" /
 * "stop_healing" (the healing actions call them; scripts use them for the healing marker:
 *     {on "start_healing" {view start ally neutral "marker_healing"}}   {on "stop_healing" {view pause "marker_healing"}}).
 * A fauna's bandage heals at once, so GEM Fauna calls the start event on the fauna when a bandage heals it - on every
 * peer: where it is used (single player, host) and on the clients when the host's heal arrives - and the stop event
 * fauna_healing_event_seconds later. The call is the engine's own sequence from the soldier's healing action:
 * Tag(name) ; ctx = InteractionCtx(buf, entity) ; Call(ctx, &tag) ; ~ctx ; ~Tag. */
typedef void *(*IxCtxFn)(void *buf, void *entity);
typedef void  (*IxCallFn)(void *ctx, void *tag);
typedef void  (*IxDtorFn)(void *p);
static IxCtxFn g_ixCtx; static IxCallFn g_ixCall; static IxDtorFn g_ixCtxDtor, g_ixTagDtor;
static int g_healEventSecs = 3;
static char g_healEventStart[48] = "start_healing", g_healEventStop[48] = "stop_healing";

static int find_healing_event(void)
{
    uint8_t *str = find_cstr("start_healing");
    for (int k = 0; str && g_tagCtorCStr && k < 6 && !g_ixCall; k++) {
        uint8_t *lea = find_lea_to(str, k), *p, *c[5];
        if (!lea) break;
        p = lea + 7;
        int i = 0;
        for (; i < 5; i++) { uint8_t *site; c[i] = next_fn_call(p, 0x30, &site); if (!c[i]) break; p = site + 5; }
        if (i < 5 || c[0] != (uint8_t *)g_tagCtorCStr) continue;
        g_ixCtx = (IxCtxFn)c[1]; g_ixCall = (IxCallFn)c[2]; g_ixCtxDtor = (IxDtorFn)c[3]; g_ixTagDtor = (IxDtorFn)c[4];
    }
    if (!g_ixCall) { logf_("healing events: the engine's \"start_healing\" call not recognised"); return 0; }
    logf_("healing events: interaction call exe+0x%llX (ctx exe+0x%llX / ~ctx exe+0x%llX, ~Tag exe+0x%llX)",
          EXE_OFF(g_ixCall), EXE_OFF(g_ixCtx), EXE_OFF(g_ixCtxDtor), EXE_OFF(g_ixTagDtor));
    return 1;
}

#ifndef FX_TEST
static void ix_call(uint8_t *e, const char *name)
{
    if (!g_ixCall || !e || !name[0]) return;
    __attribute__((aligned(16))) uint8_t tag[0x100], ctx[0x400];
    memset(tag, 0, sizeof tag); memset(ctx, 0, sizeof ctx);
    g_tagCtorCStr(tag, name);
    void *c = g_ixCtx(ctx, e);
    g_ixCall(c ? c : ctx, tag);
    g_ixCtxDtor(ctx);
    g_ixTagDtor(tag);
}
#define HEAL_SLOTS 32
static struct { uint8_t *e; uint64_t stop; } g_healOn[HEAL_SLOTS];
static int g_healActive;                                                 /* used slots */
static void healing_start(uint8_t *e)
{
    if (!g_ixCall || g_healEventSecs <= 0) return;
    uint64_t stop = GetTickCount64() + (uint64_t)g_healEventSecs * 1000;
    int freeSlot = -1;
    for (int i = 0; i < HEAL_SLOTS; i++) {
        if (g_healOn[i].e == e) { g_healOn[i].stop = stop; return; }  /* already showing: keep it on longer */
        if (!g_healOn[i].e && freeSlot < 0) freeSlot = i;
    }
    if (freeSlot < 0) return;
    g_healOn[freeSlot].e = e; g_healOn[freeSlot].stop = stop; g_healActive++;
    ix_call(e, g_healEventStart);
}
/* from the vitality timer wrapper, for the unit whose timer just ran (so it exists): its stop event when due. A slot
   whose unit stopped ticking (removed from the scene) is dropped 30 s after its stop time without touching it. */
static void healing_tick(uint8_t *e)
{
    uint64_t now = GetTickCount64();
    for (int i = 0; i < HEAL_SLOTS; i++) {
        if (!g_healOn[i].e || now < g_healOn[i].stop) continue;
        if (g_healOn[i].e == e) { g_healOn[i].e = NULL; g_healActive--; ix_call(e, g_healEventStop); }
        else if (now > g_healOn[i].stop + 30000) { g_healOn[i].e = NULL; g_healActive--; }
    }
}
#endif /* FX_TEST */

/* ---- automatic use (0.10.0) ----
 * The vitality extender's timer (host side: it returns at once on a client; the same function GEM Fauna patches for
 * vitality on any chassis, owner entity at vit+OWNER) is wrapped at its single call site. After the engine's tick, an
 * AI-controlled fauna (not in direct control) whose health is below fauna_auto_bandage_below percent of max looks for a
 * bandage in its inventory and uses one exactly like a double-click does (fauna_use_bandage: same heal, same multiplayer
 * commands), at most once per fauna_auto_bandage_cooldown seconds.
 * The inventory walk is the one of eInventoryRemoveCommand's execute: box = GetBox(entity) ; view(box) ;
 * Find(view, filter Tags, 0, predicate) ; GetDef ; RemoveMany ; ~view. An empty filter and an empty predicate match
 * every item; each one is tested for the "bandage" class tag like the UseItem step does. */
typedef void   *(*GetBoxFn)(void *entity);
typedef void   *(*ViewCtorFn)(void *view, void *box);
typedef uint8_t (*ViewFindFn)(void *view, const void *filter, int mode, void *pred);
typedef void    (*ViewDtorFn)(void *view);
typedef uint64_t (*VitTimerFn)(void *vit, void *a, void *b, void *c);
static GetBoxFn g_invGetBox; static ViewCtorFn g_viewCtor; static ViewFindFn g_viewFind; static ViewDtorFn g_viewDtor;
static uint8_t *g_vitTimer, *g_vitTimerSites[3]; static int g_vitTimerSiteN; static VitTimerFn g_vitTimerOrig;
static uint32_t g_vitOwnerOff;
static int g_okAutoBandage, g_wantAutoBandage = 1, g_autoBelowPct = 50, g_autoCooldownS = 10;

static int find_bandage_auto(void)
{
    /* the inventory walk: in eInventoryRemoveCommand's execute, the calls around GetDef */
    if (!g_vtRemoveCmd) g_vtRemoveCmd = find_vtable_rtti(".?AUeInventoryRemoveCommand@@");
    uint8_t *ex = g_vtRemoveCmd ? (uint8_t *)g_vtRemoveCmd[4] : NULL;
    uint8_t *t[16]; int n = 0, j = -1;
    for (uint8_t *p = ex; ex && in_text(ex) && p < ex + 0xC0 && n < 16; p++) { uint8_t *c = call_target_fn(p); if (c) { if (c == (uint8_t *)g_viewGetDef && j < 0) j = n; t[n++] = c; p += 4; } }
    if (j < 3 || j + 2 >= n || t[j + 1] != (uint8_t *)g_removeMany) { logf_("auto bandage: inventory walk not recognised (eInventoryRemoveCommand execute)"); return 0; }
    g_invGetBox = (GetBoxFn)t[j - 3]; g_viewCtor = (ViewCtorFn)t[j - 2]; g_viewFind = (ViewFindFn)t[j - 1]; g_viewDtor = (ViewDtorFn)t[j + 2];
    /* the vitality timer and its owner: the function of the SetStaminaMovementModeLimit call site; mov rdi,rcx ; call IsClient ;
       test al,al ; jne ; ... first mov r64,[rdi+disp8] */
    if (!g_vitSetLimitSite && !find_vitality_chassis()) return 0;
    uint32_t f = func_of(g_vitSetLimitSite);
    g_vitTimer = f ? g_base + f : NULL;
    uint8_t *mv = NULL;
    for (uint8_t *p = g_vitTimer; p && p < g_vitTimer + 0x40 && !mv; p++) if (p[0] == 0x48 && p[1] == 0x8B && p[2] == 0xF9) mv = p;
    for (uint8_t *p = mv ? mv + 3 : NULL; p && p < mv + 0x60 && !g_vitOwnerOff; p++)
        if ((p[0] == 0x48 || p[0] == 0x4C) && p[1] == 0x8B && (p[2] & 0xC7) == 0x47 && p[3] >= 0x08 && p[3] < 0x80) g_vitOwnerOff = p[3];
    int n2 = calls_to(g_vitTimer, g_vitTimerSites, 3);
    g_vitTimerSiteN = n2 > 3 ? 0 : n2;
    if (!mv || !g_vitOwnerOff || !g_vitTimerSiteN) { logf_("auto bandage: vitality timer not recognised (owner +0x%X, %d calls)", g_vitOwnerOff, g_vitTimerSiteN); return 0; }
    logf_("auto bandage: GetBox exe+0x%llX, view exe+0x%llX / Find exe+0x%llX / ~view exe+0x%llX; vitality timer exe+0x%llX (%d calls), owner vit+0x%X",
          EXE_OFF(g_invGetBox), EXE_OFF(g_viewCtor), EXE_OFF(g_viewFind), EXE_OFF(g_viewDtor), EXE_OFF(g_vitTimer), g_vitTimerSiteN, g_vitOwnerOff);
    return 1;
}

#ifndef FX_TEST
/* per-fauna time of the next allowed try; the slot of a fauna that is gone is reused once its time has passed */
#define AUTO_SLOTS 64
static struct { const uint8_t *e; uint64_t next; } g_autoNext[AUTO_SLOTS];
static uint64_t *auto_slot(const uint8_t *e, uint64_t now)
{
    int pick = -1;
    for (int i = 0; i < AUTO_SLOTS; i++) {
        if (g_autoNext[i].e == e) return &g_autoNext[i].next;
        if (pick < 0 && (!g_autoNext[i].e || now >= g_autoNext[i].next)) pick = i;
    }
    if (pick < 0) return NULL;                                          /* 64 fauna waiting: try again later */
    g_autoNext[pick].e = e; g_autoNext[pick].next = 0;
    return &g_autoNext[pick].next;
}
static void auto_bandage(uint8_t *e)
{
    void *vit = entity_vitality(e);
    if (!vit) return;
    float cur = g_getHealth(vit), max = g_getMaxHealth(vit);
    if (!(max > 0.f) || !(cur > 0.f) || cur * 100.f >= max * (float)g_autoBelowPct) return;
    if (g_brainOff && g_brainFlagsOff) {                                /* direct control: the player decides */
        uint8_t *b = *(uint8_t **)(e + g_brainOff);
        if (b && (*(uint32_t *)(b + g_brainFlagsOff) & 3) == 1) return;
    }
    uint64_t now = GetTickCount64(), *next = auto_slot(e, now);
    if (!next || now < *next) return;
    *next = now + 3000;                                                 /* no usable bandage: look again in 3 s */
    int net = g_isNet();
    if (net && !g_okBandageNet) return;                                 /* could not be sent to the clients */
    void *box = g_invGetBox(e);
    if (!box) return;
    __attribute__((aligned(16))) uint8_t view[0x100], filter[0x400], pred[0x80];
    memset(view, 0, sizeof view); memset(filter, 0, sizeof filter); memset(pred, 0, sizeof pred);   /* empty Tags, empty std::function */
    g_viewCtor(view, box);
    for (int i = 0; i < 128 && g_viewFind(view, filter, 0, pred); i++) {
        void **pdef = g_viewGetDef(view); uint8_t *def = pdef ? (uint8_t *)*pdef : NULL;
        if (!def || !g_hasTag(def + g_defTagsOff, g_tagBandage)) continue;
        if (!fauna_use_bandage(e, view, def, net)) continue;            /* reserved / in a holder: try the next stack */
        *next = now + (uint64_t)g_autoCooldownS * 1000;
        static int nlog; if (nlog < 10) { nlog++; logf_("auto bandage: fauna %p was below %d%% health (%.0f / %.0f) - used a bandage by itself", e, g_autoBelowPct, cur, max); }
        break;
    }
    g_viewDtor(view);
}
static uint64_t fx_vit_timer(void *vit, void *a, void *b, void *c)
{
    uint64_t r = g_vitTimerOrig(vit, a, b, c);
    uint8_t *e = vit ? *(uint8_t **)((uint8_t *)vit + g_vitOwnerOff) : NULL;
    if (!e || !(e[ENT_FLAGS] & 2)) return r;
    if (g_healActive) healing_tick(e);
    if (g_wantAutoBandage && is_fauna(e) && !(g_isClient && g_isClient())) auto_bandage(e);
    return r;
}
static int install_bandage_auto(void)
{
    int n = 0;
    for (int i = 0; i < g_vitTimerSiteN; i++) {
        void *o = NULL;
        if (redirect_call(g_vitTimerSites[i], (void *)fx_vit_timer, &o, "auto bandage (vitality timer)")) { g_vitTimerOrig = (VitTimerFn)o; n++; }
    }
    return n > 0;
}
#endif /* FX_TEST */
