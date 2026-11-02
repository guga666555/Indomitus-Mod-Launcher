/* GEM Fauna - fauna sprint (double-click run, Shift in direct control)
 * Part of the unity build: #included by gem_fauna.c, not compiled on its own (after squads.c).
 *
 * Fauna move on Actor::eChassisFoot, the human foot chassis. Its run speed comes from the chassis movement mode:
 * 0 slow, 1 normal, 2 fast (the {Run {Fast}} entry = sprint), 3 default, 7 = bit 7 key. Two engine calls set it:
 *   vt slot "SetMovementMode"  (mode, force):  chassis+MODE_REQ = mode, chassis+KIND = 0
 *   vt slot "SetMovementSpeed" (speed, force): chassis+SPEED = speed,  chassis+KIND = 1 (speed capped by the request)
 * A human gets the sprint from the double-click (Order::eMovementChange -> SetMovementMode(2)) or, in direct control, from
 * eChassisFoot::ManualSimulation (Shift = chassis key bit 6 -> SetMovementMode(2)). A fauna has a VEHICLE brain: its
 * vehicle states put the chassis back to the brain's movement mode (brain+FLAGS bits 6..9, 1 for vehicles) when a move
 * starts, and the vehicle side drives the chassis with its own calls - so the sprint request never stuck.
 *
 * Here (simulation, every exe):
 *  - a double-click run on a fauna (the move action's movement change with the sprint mode, squads.c fx_move_give) also
 *    sets the fauna's brain movement mode to sprint; any other move order puts it back to 1 (the vehicle-brain default);
 *  - both chassis calls are wrapped for fauna: while the fauna should sprint, a request for normal/default speed
 *    (mode 1 or 3, or a speed) becomes SetMovementMode(2). "Should sprint" = in direct control: Shift held while moving
 *    (the same key test ManualSimulation uses); otherwise: the brain movement mode is sprint.
 * The stamina limit (vitality {Stamina {movement_limits}}) is applied by the engine after this, so a tired fauna still
 * drops back to normal speed, and a fauna whose def has no Stamina block is never limited.
 */

typedef void (*FootModeFn)(void *ch, int mode, uint8_t force);
typedef void (*FootSpeedFn)(void *ch, float speed, uint8_t force);
static void **g_vtFoot; static int g_footModeSlot = -1, g_footSpeedSlot = -1;
static FootModeFn g_footModeOrig; static FootSpeedFn g_footSpeedOrig;
static uint32_t g_footEntOff, g_entChassisOff, g_footManualOff, g_footKeysOff, g_footModeOff, g_brainFlagsOff;
static int g_okFaunaSprint, g_wantFaunaSprint = 1, g_sprintDebug;
/* commit: eChassisFoot's "apply movement mode now" (the function that sends eChassisFootSetModeCommand): IsClient ->
   return; [+TYPE_REQ] != [+TYPE_CUR] -> return; Resolve(ch, &mode, &speed) ; ... ; Set(ch, mode, speed, accel) */
typedef void (*FootCommitFn)(void *ch);
typedef void (*FootResolveFn)(void *ch, int *mode, float *speed);
static FootCommitFn g_footCommit; static FootResolveFn g_footResolve; static uint32_t g_footTypeReqOff, g_footTypeCurOff, g_footSpeedOff;
#define BRAIN_MODE_SHIFT 6          /* brain+FLAGS bits 6..9: movement mode the vehicle states restore */
#define BRAIN_MODE_MASK  (0xFu << BRAIN_MODE_SHIFT)
#define FOOT_SPRINT_MODE 2          /* chassis "Fast" */
#define FOOT_KEY_SPRINT  0x40       /* chassis key bits: 1 fwd, 2 back, 4/8 strafe, 0x40 sprint, 0x80 (mode 7) */

static int find_fauna_sprint(void)
{
    g_vtFoot = find_vtable_rtti(".?AVeChassisFoot@Actor@@");
    if (!g_vtFoot) { logf_("sprint: eChassisFoot vtable not found"); return 0; }
    /* SetMovementMode: mov [rbx+REQ],edi ; mov dword [rbx+KIND],0     SetMovementSpeed: ... mov dword [rbx+KIND],1
       (both start with IsClient + the force flag test, so the stores sit within the first 0x50 bytes) */
    uint32_t kindM = 0, kindS = 0;
    for (int s = 20; s < 70; s++) {
        uint8_t *f = (uint8_t *)g_vtFoot[s];
        if (!in_text(f)) continue;
        for (uint8_t *q = f; q < f + 0x50; q++) {
            if (q[0] != 0xC7 || (q[1] & 0xC0) != 0x80 || (q[1] & 7) == 4 || q[3] != 0x04 || q[4] || q[5]) continue;   /* mov dword [r+disp32],imm32 */
            uint32_t off = *(uint32_t *)(q + 2), imm = *(uint32_t *)(q + 6);
            if (off < 0x300 || off > 0x800) continue;
            if (imm == 0 && g_footModeSlot < 0) {
                for (uint8_t *r = f; r < q; r++)                        /* the mode store right before it */
                    if (r[0] == 0x89 && (r[1] & 0xC0) == 0x80 && (r[1] & 7) == (q[1] & 7) && *(uint32_t *)(r + 2) == off - 8) { g_footModeSlot = s; kindM = off; break; }
            } else if (imm == 1 && g_footSpeedSlot < 0) { g_footSpeedSlot = s; kindS = off; }
            break;
        }
    }
    if (g_footModeSlot < 0 || g_footSpeedSlot < 0 || kindM != kindS) { logf_("sprint: eChassisFoot SetMovementMode/SetMovementSpeed not recognised (%d/%d)", g_footModeSlot, g_footSpeedSlot); return 0; }
    /* IsSprinting: mov eax,[rcx+KEYS] ; and eax,0x41 ; cmp al,0x41 ; jne ; cmp dword [rcx+MODE],2 */
    for (int s = 20; s < 80 && !g_footKeysOff; s++) {
        uint8_t *f = (uint8_t *)g_vtFoot[s];
        if (in_text(f) && f[0] == 0x8B && f[1] == 0x41 && f[3] == 0x83 && f[4] == 0xE0 && f[5] == 0x41 && f[6] == 0x3C && f[7] == 0x41 &&
            f[8] == 0x75 && f[10] == 0x83 && f[11] == 0x79 && f[13] == FOOT_SPRINT_MODE) { g_footKeysOff = f[2]; g_footModeOff = f[12]; }
    }
    if (!g_footKeysOff) { logf_("sprint: eChassisFoot key state not recognised"); return 0; }
    /* direct control flag: eChassisFoot::ManualSimulation starts with  call IsManual = cmp dword [rcx+X],0 ; setne al ; ret */
    uint8_t *ms = find_cstr("eChassisFoot::ManualSimulation"), *ml = ms ? find_lea_to(ms, 0) : NULL;
    uint32_t fs = ml ? func_of(ml) : 0;
    uint8_t *im = fs ? next_call(g_base + fs, 0x80, NULL) : NULL;
    if (!im || im[0] != 0x83 || im[1] != 0x79 || im[3] != 0x00 || im[4] != 0x0F || im[5] != 0x95 || im[6] != 0xC0 || im[7] != 0xC3) { logf_("sprint: direct-control flag not recognised"); return 0; }
    g_footManualOff = im[2];
    /* the move action (squads.c pace site): mov rcx,[rdi+CHASSIS] ; cmp qword [rcx+ENT],0 ... mov r,[r+FLAGS] ... shr r,0xE */
    if (!g_brainOff && !find_squad_select()) return 0;            /* brain offset */
    if (!g_mcSite && !find_squad_pace()) return 0;
    uint8_t *lo = g_mcSite - 0x80;
    for (uint8_t *q = lo; q < g_mcSite; q++)                     /* (the editor puts one instruction in between) */
        if (q[0] == 0x48 && q[1] == 0x8B && q[2] == 0x8F)
            for (uint8_t *r = q + 7; r < q + 7 + 12; r++)
                if (r[0] == 0x48 && r[1] == 0x83 && r[2] == 0x79 && r[4] == 0x00) { g_entChassisOff = *(uint32_t *)(q + 3); g_footEntOff = r[3]; break; }
    for (uint8_t *q = lo; q < g_mcSite; q++) {
        int rex = (q[0] & 0xF0) == 0x40, o = rex;
        if (q[o] != 0x8B || (q[o + 1] & 0xC0) != 0x80 || (q[o + 1] & 7) == 4) continue;
        uint32_t d = *(uint32_t *)(q + o + 2); int reg = ((q[o + 1] >> 3) & 7) | (rex && (q[0] & 4) ? 8 : 0);
        if (d < 0x100 || d > 0x800) continue;
        for (uint8_t *r = q + o + 6; r < g_mcSite; r++) {           /* shr reg,0xE */
            int rr = (r[0] & 0xF0) == 0x40, ro = rr;
            if (r[ro] == 0xC1 && (r[ro + 1] & 0xF8) == 0xE8 && r[ro + 2] == 0x0E && ((r[ro + 1] & 7) | (rr && (r[0] & 1) ? 8 : 0)) == reg) { g_brainFlagsOff = d; break; }
        }
    }
    if (!g_entChassisOff || !g_footEntOff || !g_brainFlagsOff || !g_brainOff) { logf_("sprint: chassis/brain offsets not recognised"); return 0; }
    /* commit: the function that holds the 2nd reference to the eChassisFootSetModeCommand vtable (the 1st is its factory) */
    void **cvt = find_vtable_rtti(".?AUeChassisFootSetModeCommand@@");
    for (int k = 0; cvt && k < 4 && !g_footCommit; k++) {
        uint8_t *r = find_lea_to(cvt, k); uint32_t f = r ? func_of(r) : 0;
        uint8_t *c = f ? g_base + f : NULL, *site = NULL;
        if (!c || !next_call(c, 0x10, &site)) continue;                /* call IsClient ; test al,al ; jne */
        uint8_t *q = site + 5;
        if (q[0] != 0x84 || q[1] != 0xC0 || q[2] != 0x0F || q[3] != 0x85) continue;
        q += 8;                                                       /* mov eax,[r+REQ] ; cmp [r+CUR],eax ; jne */
        if (q[0] != 0x8B || (q[1] & 0xC0) != 0x40 || q[3] != 0x39 || (q[4] & 0xC0) != 0x40) continue;
        uint8_t *res = next_call(q + 6, 0x40, &site);
        if (!res) continue;
        uint8_t *mv = NULL, *set = NULL;                              /* movss xmm3,[rax+8] ; call Set */
        for (uint8_t *t = site + 5; t < site + 0x60 && !mv; t++) if (t[0] == 0xF3 && t[1] == 0x0F && t[2] == 0x10 && t[3] == 0x58 && t[4] == 0x08 && t[5] == 0xE8) mv = t;
        if (mv) set = mv + 10 + *(int32_t *)(mv + 6);
        for (uint8_t *t = set; t && t < set + 0x30 && !g_footSpeedOff; t++) if (t[0] == 0xF3 && t[1] == 0x0F && t[2] == 0x11 && t[3] == 0x51) g_footSpeedOff = t[4];
        if (!g_footSpeedOff) continue;
        g_footCommit = (FootCommitFn)c; g_footResolve = (FootResolveFn)res; g_footTypeReqOff = q[2]; g_footTypeCurOff = q[5];
    }
    if (!g_footCommit) logf_("sprint: eChassisFoot mode commit not recognised - the sprint may not apply to fauna");
    else logf_("sprint: mode commit exe+0x%llX (resolve exe+0x%llX, type +0x%X/+0x%X, speed +0x%X)", EXE_OFF(g_footCommit), EXE_OFF(g_footResolve), g_footTypeReqOff, g_footTypeCurOff, g_footSpeedOff);
    logf_("sprint: eChassisFoot vt exe+0x%llX: SetMovementMode slot %d, SetMovementSpeed slot %d (kind +0x%X), keys +0x%X, mode +0x%X, "
          "manual +0x%X; entity->chassis +0x%X, chassis->entity +0x%X, brain flags +0x%X",
          EXE_OFF(g_vtFoot), g_footModeSlot, g_footSpeedSlot, kindM, g_footKeysOff, g_footModeOff, g_footManualOff,
          g_entChassisOff, g_footEntOff, g_brainFlagsOff);
    return 1;
}

#ifndef FX_TEST
static uint8_t *foot_fauna(void *ch)
{
    uint8_t *e = ch ? *(uint8_t **)((uint8_t *)ch + g_footEntOff) : NULL;
    if (!e || *(void **)(e + g_entChassisOff) != ch || !is_fauna(e)) return NULL;   /* is_fauna: cached per def */
    return e;
}
static int brain_mode(uint8_t *e)
{
    uint8_t *b = *(uint8_t **)(e + g_brainOff);
    return b ? (int)((*(uint32_t *)(b + g_brainFlagsOff) & BRAIN_MODE_MASK) >> BRAIN_MODE_SHIFT) : -1;
}
/* squads.c: a fauna got a movement change from a player move order */
static void fauna_sprint_order(uint8_t *e, int mode)
{
    if (!g_okFaunaSprint || !e) return;
    uint8_t *b = *(uint8_t **)(e + g_brainOff);
    if (!b) return;
    uint32_t *fl = (uint32_t *)(b + g_brainFlagsOff), old = *fl;
    int cur = (int)((old & BRAIN_MODE_MASK) >> BRAIN_MODE_SHIFT);
    int want = mode == FOOT_SPRINT_MODE ? FOOT_SPRINT_MODE : (cur == FOOT_SPRINT_MODE ? 1 : cur);
    if (want != cur) *fl = (old & ~BRAIN_MODE_MASK) | ((uint32_t)want << BRAIN_MODE_SHIFT);
}
/* should this fauna chassis sprint right now? */
static int foot_wants_sprint(uint8_t *ch, uint8_t *e)
{
    if (*(int32_t *)(ch + g_footManualOff)) {                   /* direct control: ManualSimulation's own key test */
        uint32_t k = *(uint32_t *)(ch + g_footKeysOff);
        if ((k & 0x80) || !(k & FOOT_KEY_SPRINT)) return 0;
        return ((k ^ (k >> 1)) & 1) || (((k >> 2) ^ (k >> 3)) & 1);
    }
    return brain_mode(e) == FOOT_SPRINT_MODE;
}
static void sprint_log(uint8_t *ch, uint8_t *e, const char *what, int req, int out, void *ret)
{
    static int n, nChanged;
    int changed = req != out;
    if (!g_sprintDebug || (changed ? nChanged >= 20 : n >= 40)) return;
    if (changed) nChanged++; else n++;
    logf_("sprint debug: fauna %p %s(%d) from exe+0x%llX - manual %d keys 0x%X brain mode %d -> %s%d", e, what, req, EXE_OFF(ret),
          *(int32_t *)(ch + g_footManualOff), *(uint32_t *)(ch + g_footKeysOff), brain_mode(e), changed ? "SPRINT mode " : "kept ", out);
}
/* A soldier's mode/speed change goes through his FSM (a "movement mode" request) and is applied by the commit when the
   FSM gets to it; a fauna's FSM never does, so the chassis kept its old mode (0.6.1 log: ManualSimulation asked for
   mode 2 every frame, the chassis stayed at 1). For fauna the commit runs right away when the resolved mode or speed
   differs from the applied one (the commit itself checks IsClient and the movement type, applies the stamina limit and
   sends eChassisFootSetModeCommand in multiplayer). */
static void foot_commit(uint8_t *ch, uint8_t *e)
{
    if (!g_footCommit) return;
    int m = -1; float sp = 0.0f;
    g_footResolve(ch, &m, &sp);
    int cur = *(int32_t *)(ch + g_footModeOff); float csp = *(float *)(ch + g_footSpeedOff);
    float d = sp - csp; if (d < 0) d = -d;
    if (m < 0 || (m == cur && d < 1e-4f)) return;
    g_footCommit(ch);
    static int n; if (g_sprintDebug && n < 25) { n++;
        logf_("sprint debug: fauna %p commit: mode %d -> %d (now %d), speed %.2f -> %.2f (now %.2f), type %d/%d", e, cur, m,
              *(int32_t *)(ch + g_footModeOff), csp, sp, *(float *)(ch + g_footSpeedOff),
              *(int32_t *)(ch + g_footTypeReqOff), *(int32_t *)(ch + g_footTypeCurOff)); }
}
/* engine.c: the vitality timer changed a foot chassis' stamina limit (exhausted / recovered) */
static void fauna_limit_changed(uint8_t *ch)
{
    if (!g_okFaunaSprint) return;
    uint8_t *e = foot_fauna(ch);
    if (e) foot_commit(ch, e);
}
static void fx_foot_set_mode(void *chv, int mode, uint8_t force)
{
    uint8_t *ch = chv, *e = foot_fauna(ch);
    if (e) {
        int out = mode;
        if ((mode == 1 || mode == 3) && foot_wants_sprint(ch, e)) out = FOOT_SPRINT_MODE;
        sprint_log(ch, e, "SetMovementMode", mode, out, __builtin_return_address(0));
        mode = out;
    }
    g_footModeOrig(chv, mode, force);
    if (e) foot_commit(ch, e);
}
static void fx_foot_set_speed(void *chv, float speed, uint8_t force)
{
    uint8_t *ch = chv, *e = foot_fauna(ch);
    if (e && speed > 0.0f && *(int32_t *)(ch + g_footManualOff) && foot_wants_sprint(ch, e)) {      /* only in direct control */
        sprint_log(ch, e, "SetMovementSpeed", -1, FOOT_SPRINT_MODE, __builtin_return_address(0));
        g_footModeOrig(chv, FOOT_SPRINT_MODE, force);
        foot_commit(ch, e);
        return;
    }
    if (e) sprint_log(ch, e, "SetMovementSpeed", -1, -1, __builtin_return_address(0));
    g_footSpeedOrig(chv, speed, force);
    if (e) foot_commit(ch, e);
}
static int install_fauna_sprint(void)
{
    void *o1 = NULL, *o2 = NULL;
    if (!vt_hook(g_vtFoot, g_footModeSlot, (void *)fx_foot_set_mode, &o1)) return 0;
    g_footModeOrig = (FootModeFn)o1;
    if (!vt_hook(g_vtFoot, g_footSpeedSlot, (void *)fx_foot_set_speed, &o2)) return 0;
    g_footSpeedOrig = (FootSpeedFn)o2;
    return 1;
}
#endif /* FX_TEST */
