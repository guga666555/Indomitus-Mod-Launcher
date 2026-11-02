/* GEM Fauna - fauna UI
 * Part of the unity build: #included by gem_fauna.c, not compiled on its own.
 * Vehicle panel health row (bar_health / txt_health / pnl_health), squad strip items for fauna
 * (hp bar + circle icon), mixed human/fauna squads in the strip.
 */

/* selection */
static void **g_gameClient; static uint32_t g_selObjOff, g_selBegOff, g_selEndOff, g_selStride = 16;
static uint32_t g_entIdOff;                                           /* uint16 entity id (from PanelSquad::GetIds) */

/* UI (game exe only) */
static void **g_vtPanelVehicle, **g_vtPanelSquad, **g_vtProgressBar, **g_vtStaticText;
static int g_uiDebug;                                                  /* ini ui_debug: selection/squad diagnostics */
/* The squad-strip item class ("panel_human" = GameControl::ePanelHuman). Slot 68 = SetEntity refresh (Update(full=1)),
   slot 70 = per-frame tick (Update(full=0)). Update(full=1) builds the portrait from a Human* cast that is NULL for a
   non-human and the engine dereferences it -> must never run for fauna. */
static void **g_vtPanelHuman; static uint32_t g_phEntOff, g_phHpBarOff; static uint8_t *g_phUpdate;
/* Squad-strip circle icon for fauna. The engine builds it as prefix + "<name>", prefix = a global std::string set to
   "/interface/scene/squad/" (56x52 circle icons; 0.2.6-0.2.8 wrongly used squad_view = 32x32 world badges) (builder: Update's
   call after mov r8d,3) and sets it with SetImage(widget+COMP, 0, std::string*, 0); tint from IconColor(&c, entity).
   Fauna: first def prop whose name is a file in that folder (looked up in the file repository / on disk), else the engine's
   own builder with the real entity (vanilla vehicle classes: tank, car, cannon ...). */
typedef void *(*IconBuilderFn)(void *outStr, void *entity, int size);
typedef void  (*SetImageFn)(void *comp, int a, void *str, int b);
typedef void  (*StrDtorFn)(void *str);
typedef void *(*StrAssignFn)(void *str, const char *p, size_t n);
typedef uint32_t *(*IconColorFn)(uint32_t *out, void *entity);
static IconBuilderFn g_iconBuilder; static SetImageFn g_setImage; static StrDtorFn g_strDtor; static StrAssignFn g_strAssign; static IconColorFn g_iconColor;
static uint32_t g_phModeOff, g_phWidgetOff, g_phCompOff, g_phColorOff; static uint8_t **g_repoMap; static int g_okIcons;
static MsString *g_iconPrefix;              /* engine global std::string the builder prepends ("/interface/scene/squad/") */
static uint32_t g_panelEntOff, g_squadActiveOff;                    /* found in find_ui, or the UI stays off */
static uint32_t g_dlgFlagsOff;                                        /* eDialog flags: bit1 visible, bit2 enabled (from PanelSquad::Update) */
static uint8_t **g_sqVecEnd, **g_sqVecCap;                            /* static std::vector<uint16_t> of the squad strip */
typedef void (*VecU16EmplaceFn)(void *vec, void *where, void *val);
static VecU16EmplaceFn g_vecU16Emplace;
typedef void *(*StrEmptyCtorFn)(void *str);
static StrEmptyCtorFn g_strEmptyCtor;
static uint8_t *g_createItemFn, *g_createItemResume; static const char *g_strPanelHuman;
#define CI_PROLOGUE_LEN 0x28

static int find_ui(void)
{
    g_vtPanelVehicle = find_vtable_rtti(".?AVeGUI_GC_PanelVehicle@SceneUI@GUI2@@");
    g_vtPanelSquad   = find_vtable_rtti(".?AVeGUI_GC_PanelSquad@SceneUI@GUI2@@");
    g_vtProgressBar  = find_vtable_rtti(".?AVeProgressBar@@");
    g_vtStaticText   = find_vtable_rtti(".?AVeStaticText@@");             /* optional: without it txt_health is never written */
    if (!g_vtPanelVehicle || !g_vtPanelSquad || !g_vtProgressBar) { logf_("ui: panel classes not found (no GUI in this exe?)"); return 0; }
    /* PanelVehicle::Update = slot 73, references "vehiclebar/speed"; reads [rcx+ENT] first */
    uint8_t *upd = (uint8_t *)g_vtPanelVehicle[73], *s = find_cstr("vehiclebar/speed");
    if (!in_text(upd) || !s || !find_rip_ref(upd, 0x1000, s)) { logf_("ui: PanelVehicle::Update not recognised"); return 0; }
    for (uint8_t *p = upd; p < upd + 0x40; p++) if (p[0] == 0x48 && p[1] == 0x8B && p[2] == 0x91) { g_panelEntOff = *(uint32_t *)(p + 3); break; }
    if (g_panelEntOff < 0x100 || g_panelEntOff > 0x800) { logf_("ui: PanelVehicle entity offset not recognised"); return 0; }
    /* eDialog::FindChild(std::string*) = slot 60 (same function in the progress bar's vtable) */
    if (g_vtPanelVehicle[60] != g_vtProgressBar[60] || !in_text(g_vtPanelVehicle[60])) { logf_("ui: FindChild slot mismatch"); return 0; }
    /* squad strip: slot 71 GetIds (static vector<uint16>), 72 CreateItem ("panel_human"), 74 GetProperty ("/interface/scene/vehicle/commander") */
    uint8_t *gi = (uint8_t *)g_vtPanelSquad[71], *ci = (uint8_t *)g_vtPanelSquad[72], *gp = (uint8_t *)g_vtPanelSquad[74];
    uint8_t *sph = find_cstr("panel_human"), *scm = find_cstr("/interface/scene/vehicle/commander");
    if (!in_text(gi) || !in_text(ci) || !in_text(gp) || !sph || !scm || !find_rip_ref(ci, 0x40, sph) || !find_rip_ref(gp, 0x100, scm)) { logf_("ui: PanelSquad slots not recognised"); return 0; }
    if (gi[6] == 0x48 && gi[7] == 0x83 && gi[8] == 0xB9) g_squadActiveOff = *(uint32_t *)(gi + 9);
    if (g_squadActiveOff < 0x100 || g_squadActiveOff > 0x800) { logf_("ui: PanelSquad active-unit offset not recognised"); return 0; }
    for (uint8_t *p = gi; p < gi + 0x200; p++) {
        if (p[0] == 0x0F && p[1] == 0xB7 && p[2] == 0x80 && !g_entIdOff) g_entIdOff = *(uint32_t *)(p + 3);
        if (p[0] == 0x48 && p[1] == 0x8B && p[2] == 0x15 && p[7] == 0x48 && p[8] == 0x3B && p[9] == 0x15 && !g_sqVecEnd) {
            g_sqVecEnd = (uint8_t **)(p + 7 + *(int32_t *)(p + 3)); g_sqVecCap = (uint8_t **)(p + 14 + *(int32_t *)(p + 10));
            for (uint8_t *q = p + 14; q < p + 0x40; q++) if (q[0] == 0x48 && q[1] == 0x8B && q[2] == 0xCE && q[3] == 0xE8) { g_vecU16Emplace = (VecU16EmplaceFn)(q + 8 + *(int32_t *)(q + 4)); break; }
        }
    }
    if (!g_sqVecEnd || !in_data(g_sqVecEnd) || g_sqVecCap != g_sqVecEnd + 1 || !g_vecU16Emplace) { logf_("ui: squad id vector not recognised"); return 0; }
    if (g_entIdOff < 0x100 || g_entIdOff > 0x2000) { logf_("ui: entity id offset not recognised"); return 0; }
    /* PanelSquad::Update (slot 70): base update, then visible/enabled (this+0x48 bits 1,2 via slots 5,6) = active is human */
    uint8_t *up = (uint8_t *)g_vtPanelSquad[70];
    static const uint8_t vis1[] = { 0xF6,0x40,0x30,0x04 }, vis2[] = { 0xFF,0x50,0x28 }, vis3[] = { 0xFF,0x50,0x30 };
    uint8_t *fl = NULL;
    for (uint8_t *q = up; q < up + 0x80; q++) if (q[0] == 0x8B && q[1] == 0x4B && !fl) fl = q;             /* mov ecx,[rbx+FLAGS] */
    if (!in_text(up) || !find_bytes(up, 0x100, vis1, 4) || !find_bytes(up, 0x100, vis2, 3) || !find_bytes(up, 0x100, vis3, 3) || !fl) { logf_("ui: PanelSquad::Update not recognised"); return 0; }
    g_dlgFlagsOff = fl[2];
    /* empty std::string ctor: in GetProperty's no-brain path: call X ; mov rax,rbx ; add rsp,20 ; pop rbx ; ret */
    static const uint8_t ctorsig[] = { 0x33,0xC0,0x0F,0x57,0xC0,0x0F,0x11,0x01 }, capsig[] = { 0x48,0xC7,0x41,0x18,0x0F,0x00,0x00,0x00 };
    for (uint8_t *p = gp; p < gp + 0x100; p++) {
        if (p[0] != 0xE8) continue;
        uint8_t *t = p + 5 + *(int32_t *)(p + 1);                                                       /* xor eax,eax ; xorps ; movups [rcx] ; ... mov qword [rcx+18],0xf */
        if (in_text(t) && !memcmp(t, ctorsig, 8) && find_bytes(t, 0x20, capsig, 8)) { g_strEmptyCtor = (StrEmptyCtorFn)t; break; }
    }
    if (!g_strEmptyCtor) { logf_("ui: string ctor not found"); return 0; }
    /* CreateItem prologue template (forced "panel_human" entry) */
    static const uint8_t tmpl[] = { 0x48,0x89,0x5C,0x24,0x08, 0x48,0x89,0x74,0x24,0x20, 0x55,0x57,0x41,0x55,0x41,0x56,0x41,0x57, 0x48,0x8D,0x6C,0x24,0xC9,
                                    0x48,0x81,0xEC,0xB0,0x00,0x00,0x00 };
    static const uint8_t tail[] = { 0xA8,0x04,0x74,0x09, 0x48,0x8D,0x15 };
    /* between prologue and tail: movzx eax,[rdx+0x30] / mov rdi,rdx / mov r13,rcx in any order (game vs editor build) */
    int okMid = 0;
    { uint8_t *m = ci + sizeof tmpl; int seen = 0;
      for (int k = 0; k < 3; k++) {
          if (m[0] == 0x0F && m[1] == 0xB6 && m[2] == 0x42 && m[3] == 0x30) { seen |= 1; m += 4; }
          else if (m[0] == 0x48 && m[1] == 0x8B && m[2] == 0xFA) { seen |= 2; m += 3; }
          else if (m[0] == 0x4C && m[1] == 0x8B && m[2] == 0xE9) { seen |= 4; m += 3; }
          else break;
      }
      okMid = seen == 7 && m == ci + CI_PROLOGUE_LEN; }
    if (memcmp(ci, tmpl, sizeof tmpl) || !okMid || memcmp(ci + CI_PROLOGUE_LEN, tail, sizeof tail) || ci[0x33] != 0xEB) { logf_("ui: CreateItem prologue differs - fauna strip icons off"); return 0; }
    g_strPanelHuman = (const char *)(ci + 0x2C + 7 + *(int32_t *)(ci + 0x2F));
    if (g_strPanelHuman != (const char *)sph) { logf_("ui: CreateItem template string mismatch"); return 0; }
    g_createItemFn = ci; g_createItemResume = ci + 0x35 + (int8_t)ci[0x34];
    /* selection list: eSelect ctor (PanelSquad slot 75, call after mov edx,3): mov rax,[rip+gc] ; mov rdi,[rax+SEL] ; mov rbx,[rdi+B] ; cmp rbx,[rdi+E] */
    uint8_t *s75 = (uint8_t *)g_vtPanelSquad[75], *sel = NULL;
    for (uint8_t *p = s75; p < s75 + 0x80; p++) if (p[0] == 0xBA && *(uint32_t *)(p + 1) == 3) { sel = next_call(p + 5, 0x20, NULL); break; }
    if (!sel) { logf_("ui: selection filter not found"); return 0; }
    for (uint8_t *p = sel; p < sel + 0x200 && !g_selObjOff; p++) {
        if (!(p[0] == 0x48 && p[1] == 0x8B && p[2] == 0x05 && in_data(p + 7 + *(int32_t *)(p + 3)))) continue;      /* mov rax,[rip+gc] */
        void **gc = (void **)(p + 7 + *(int32_t *)(p + 3)); uint32_t so = 0, bo = 0, eo = 0; int R = -1;
        for (uint8_t *q = p + 7; q < p + 0x40; q++) {
            if (!so && q[0] == 0x48 && q[1] == 0x8B && (q[2] & 0xC7) == 0x80) { so = *(uint32_t *)(q + 3); R = (q[2] >> 3) & 7; q += 6; }     /* mov R,[rax+SEL]   */
            else if (so && !bo && q[0] == 0x48 && q[1] == 0x8B && (q[2] & 0xC0) == 0x40 && (q[2] & 7) == R) { bo = q[3]; q += 3; }         /* mov r64,[R+B]     */
            else if (bo && q[0] == 0x48 && (q[1] == 0x3B || q[1] == 0x8B) && (q[2] & 0xC0) == 0x40 && (q[2] & 7) == R) { eo = q[3]; break; } /* cmp/mov r64,[R+E] */
        }
        if (!so || !eo) continue;
        if (g_gameClient && gc != g_gameClient) { logf_("ui: game client global mismatch"); return 0; }
        g_gameClient = gc; g_selObjOff = so; g_selBegOff = bo; g_selEndOff = eo;
    }
    if (!g_gameClient || !g_selObjOff) { logf_("ui: selection list layout not recognised"); return 0; }
    /* squad strip item: GameControl::ePanelHuman. Tick (slot 70): call base ; ... cmp qword [rbx+ENT],0 ; je ;
       xor edx,edx ; mov rcx,rbx ; add rsp,20 ; pop rbx ; jmp Update. Hp bar = first [rdi+BAR] progress bar in Update
       (mov rcx,[rdi+BAR] ... test rcx,rcx ; je ; ... movss xmm0,[rcx+1A8]) - fed from the vitality fraction for humans. */
    g_vtPanelHuman = find_vtable_rtti(".?AUePanelHuman@GameControl@@");
    if (!g_vtPanelHuman || !in_text(g_vtPanelHuman[68]) || !in_text(g_vtPanelHuman[70])) { logf_("ui: ePanelHuman not found - fauna strip icons off"); return 0; }
    { uint8_t *tk = (uint8_t *)g_vtPanelHuman[70];
      static const uint8_t jsig[] = { 0x33,0xD2,0x48,0x8B,0xCB,0x48,0x83,0xC4,0x20,0x5B,0xE9 };
      for (uint8_t *q = tk; q < tk + 0x40; q++) {
          if (!g_phEntOff && q[0] == 0x48 && q[1] == 0x83 && q[2] == 0xBB && q[7] == 0x00) g_phEntOff = *(uint32_t *)(q + 3);
          if (!memcmp(q, jsig, sizeof jsig)) { g_phUpdate = q + sizeof jsig + 4 + *(int32_t *)(q + sizeof jsig); break; }
      }
      if (!g_phUpdate || !in_text(g_phUpdate) || g_phEntOff < 0x100 || g_phEntOff > 0x400) { logf_("ui: ePanelHuman tick not recognised - fauna strip icons off"); return 0; }
      static const uint8_t bsig[] = { 0xF3,0x0F,0x10,0x81,0xA8,0x01,0x00,0x00 };
      uint8_t *mv = find_bytes(g_phUpdate, 0x800, bsig, sizeof bsig);
      for (uint8_t *q = mv ? mv - 3 : NULL; q && q > mv - 0x48; q--)
          if (q[0] == 0x48 && q[1] == 0x8B && q[2] == 0x8F) { g_phHpBarOff = *(uint32_t *)(q + 3); break; }
      if (!g_phHpBarOff || g_phHpBarOff < 0x150 || g_phHpBarOff > 0x400 || g_phHpBarOff == g_phEntOff) { logf_("ui: ePanelHuman hp bar not recognised - fauna strip icons off"); return 0; }
      if (g_phEntOff != 0x150) logf_("ui: note: ePanelHuman entity at +0x%X", g_phEntOff); }
    /* circle icon (optional: failure only leaves the fauna circle empty) */
    do {
        uint8_t *u = g_phUpdate, *site = NULL, *q;
        for (q = u; q < u + 0x800 && !g_phModeOff; q++) if (q[0] == 0x8B && q[1] == 0x8F && q[6] == 0x83 && q[7] == 0xE9 && q[8] == 0x01) g_phModeOff = *(uint32_t *)(q + 2);
        if (!g_phModeOff) break;
        for (q = u; q < u + 0x800; q++) if (q[0] == 0x8B && q[1] == 0x8F && *(uint32_t *)(q + 2) == g_phModeOff) break;
        for (uint8_t *r = q; r < q + 0x60 && !g_phWidgetOff; r++)
            if (r[0] == 0x48 && r[1] == 0x8B && (r[2] & 0xC7) == 0x87 && *(uint32_t *)(r + 3) != g_phEntOff) g_phWidgetOff = *(uint32_t *)(r + 3);
        static const uint8_t m3[] = { 0x41,0xB8,0x03,0x00,0x00,0x00 };
        uint8_t *mv = find_bytes(q, 0x80, m3, sizeof m3);
        if (!g_phWidgetOff || !mv) break;
        g_iconBuilder = (IconBuilderFn)next_call(mv + 6, 12, &site);
        if (!g_iconBuilder) break;
        uint8_t *afterB = site + 5;
        for (uint8_t *r = afterB; r < afterB + 0x30 && !g_phCompOff; r++) {
            if (r[0] == 0x48 && r[1] == 0x81 && r[2] == 0xC1) g_phCompOff = *(uint32_t *)(r + 3);            /* add rcx,COMP      */
            if (r[0] == 0x48 && r[1] == 0x8D && (r[2] & 0xF8) == 0x88 && (r[2] & 7) != 4) g_phCompOff = *(uint32_t *)(r + 3); /* lea rcx,[r+COMP] */
        }
        g_setImage = (SetImageFn)next_call(afterB, 0x30, &site);
        if (!g_setImage || !g_phCompOff) break;
        g_strDtor = (StrDtorFn)next_call(site + 5, 0x20, &site);
        if (!g_strDtor) break;
        g_iconColor = (IconColorFn)next_call(site + 5, 0x40, &site);
        if (!g_iconColor) break;
        for (uint8_t *r = site + 5; r < site + 0x30 && !g_phColorOff; r++) if (r[0] == 0x39 && r[1] == 0x41) g_phColorOff = r[2];   /* cmp [rcx+COL],eax */
        static const uint8_t sl[] = { 0x42,0x80,0x3C,0x03,0x00,0x75 };                                   /* strlen loop, then assign */
        uint8_t *ls = find_bytes((uint8_t *)g_iconBuilder, 0x100, sl, sizeof sl);
        if (!ls || !g_phColorOff) break;
        g_strAssign = (StrAssignFn)next_call(ls + 7, 0x14, NULL);
        if (!g_strAssign) break;
        for (uint8_t *r = (uint8_t *)g_iconBuilder; r < (uint8_t *)g_iconBuilder + 0x120 && !g_iconPrefix; r++)   /* cmp qword [rip+cap],0xf */
            if (r[0] == 0x48 && r[1] == 0x83 && r[2] == 0x3D && r[7] == 0x0F) { uint8_t *cap = r + 8 + *(int32_t *)(r + 3); if (in_data(cap - 0x18)) g_iconPrefix = (MsString *)(cap - 0x18); }
        if (!g_iconPrefix) break;
        /* resource repository: std::map<string,...>; "Repository: %zd resources" prints its size (map+8) */
        uint8_t *rs = find_cstr("Repository: %zd resources"), *rl = rs ? find_lea_to(rs, 0) : NULL;
        for (uint8_t *r = rl ? rl - 0x20 : NULL; r && r < rl; r++)
            if (r[0] == 0x48 && r[1] == 0x8B && r[2] == 0x05) { uint8_t *g = r + 7 + *(int32_t *)(r + 3); if (in_data(g)) g_repoMap = (uint8_t **)(g - 8); }
        if (!g_repoMap) break;
        g_okIcons = 1;
    } while (0);
    if (!g_okIcons) logf_("ui: squad icon code not recognised - fauna circle icons off");
    logf_("ui: PanelVehicle vt exe+0x%llX (entity +0x%X, Update exe+0x%llX), PanelSquad vt exe+0x%llX (active +0x%X, GetIds/CreateItem/GetProperty exe+0x%llX/%llX/%llX), ProgressBar vt exe+0x%llX",
          EXE_OFF(g_vtPanelVehicle), g_panelEntOff, EXE_OFF(upd), EXE_OFF(g_vtPanelSquad), g_squadActiveOff, EXE_OFF(gi), EXE_OFF(ci), EXE_OFF(gp), EXE_OFF(g_vtProgressBar));
    logf_("ui: entity id +0x%X, selection = [[gc exe+0x%llX]+0x%X] +0x%X..+0x%X (stride %u), squad id vector end @ exe+0x%llX, string ctor exe+0x%llX",
          g_entIdOff, EXE_OFF(g_gameClient), g_selObjOff, g_selBegOff, g_selEndOff, g_selStride, EXE_OFF(g_sqVecEnd), EXE_OFF(g_strEmptyCtor));
    logf_("ui: ePanelHuman vt exe+0x%llX (entity +0x%X, hp bar +0x%X, Update exe+0x%llX), dialog flags +0x%X", EXE_OFF(g_vtPanelHuman), g_phEntOff, g_phHpBarOff, EXE_OFF(g_phUpdate), g_dlgFlagsOff);
    if (g_okIcons) logf_("ui: squad icon: mode +0x%X widget +0x%X comp +0x%X color +0x%X, builder exe+0x%llX SetImage exe+0x%llX ~str exe+0x%llX assign exe+0x%llX color exe+0x%llX, repository map @ exe+0x%llX, icon prefix @ exe+0x%llX",
          g_phModeOff, g_phWidgetOff, g_phCompOff, g_phColorOff, EXE_OFF(g_iconBuilder), EXE_OFF(g_setImage), EXE_OFF(g_strDtor), EXE_OFF(g_strAssign), EXE_OFF(g_iconColor), EXE_OFF(g_repoMap), EXE_OFF(g_iconPrefix));
    return 1;
}

#ifndef FX_TEST
/* ------------------------------------------------------------------------------------ */
/* runtime: vehicle panel hp bar, squad member strip                                    */
/* ------------------------------------------------------------------------------------ */
typedef void  (*PanelUpdateFn)(void *panel);
typedef void *(*PanelGetIdsFn)(void *panel);
typedef void *(*PanelCreateItemFn)(void *panel, void *entity);
typedef void *(*PanelGetPropFn)(void *panel, void *outStr, void *entity);
typedef void *(*FindChildFn)(void *dlg, MsString *name);
static PanelUpdateFn g_pvUpdateOrig, g_psUpdateOrig; static PanelGetIdsFn g_psGetIdsOrig; static PanelCreateItemFn g_psCreateItemOrig, g_psCreateItemHuman; static PanelGetPropFn g_psGetPropOrig;
static int g_okUi;
/* ui_debug: one line before and after the engine's original in each UI hook (first two calls each), so a crash in a hook
   shows where it happened */
#define UI_TRACE(tag, phase) do { static int once_; if (g_uiDebug && once_ < 2) { once_++; logf_("ui trace: %s %s", tag, phase); } } while (0)
/* widget names from the ini; FindChild takes a short (SSO) std::string, so at most 15 characters (checked in init) */
#define UI_NAME_MAX 15
static char g_barName[64] = "bar_health", g_txtName[64] = "txt_health", g_pnlName[64] = "pnl_health", g_faunaIconDefault[48] = "soldier";
static char g_stBarName[64] = "bar_stamina", g_stTxtName[64] = "txt_stamina", g_stPnlName[64] = "pnl_stamina";
static char g_faunaHide[128] = "pnl_speed";                         /* vehicle panel widgets hidden for fauna (space separated, up to 4) */

/* engine widget layout (GUI2): eDialog vtable slots, progress bar value and caption */
#define DLG_VT_SHOW        5      /* re-reads flags bit1 (visible) */
#define DLG_VT_ENABLE      6      /* re-reads flags bit2 (enabled) */
#define DLG_VT_TEXT_CHANGED 17    /* after writing the caption std::string */
#define DLG_VT_FIND_CHILD  60     /* eDialog::FindChild(std::string*) */
#define PB_VT_REDRAW       71     /* progress bar: after writing the value */
#define TEXT_VT_SET        25     /* eStaticText::SetText(std::string*) */
#define PB_VALUE           0x1A8  /* float 0..1 */
#define PB_CAPTION         0xA8   /* std::string drawn over the bar */
#define VCALL(obj, slot)   ((void (*)(void *))(*(void ***)(obj))[slot])(obj)

static void ms_set(MsString *s, const char *v)
{
    size_t n = strlen(v); if (n > 15) n = 15;
    memcpy(s->u.buf, v, n); s->u.buf[n] = 0; s->size = n; s->cap = 15;
}
/* assign into an existing engine std::string without touching its heap buffer: only if it fits; 1 = changed */
static int ms_assign(MsString *s, const char *v)
{
    size_t n = strlen(v);
    char *dst = s->cap > 15 ? s->u.ptr : s->u.buf;
    if (n > s->cap) return 0;
    if (s->size == n && !memcmp(dst, v, n)) return 0;
    memcpy(dst, v, n); dst[n] = 0; s->size = n;
    return 1;
}

/* the local selection's entry range [*b, *en) (stride g_selStride, entity* first); 0 if none */
static int sel_range(uint8_t **b, uint8_t **en)
{
    if (!g_gameClient || !*g_gameClient) return 0;
    uint8_t *selObj = *(uint8_t **)((uint8_t *)*g_gameClient + g_selObjOff);
    if (!selObj) return 0;
    *b = *(uint8_t **)(selObj + g_selBegOff); *en = *(uint8_t **)(selObj + g_selEndOff);
    return *b && *en >= *b && *en - *b <= 4096 * 16;
}

/* first alive fauna in the local selection (multi-selections of vehicles leave the panels without an active entity) */
static uint8_t *first_selected_fauna(void)
{
    uint8_t *b, *en;
    if (!sel_range(&b, &en)) return NULL;
    for (; b < en; b += g_selStride) { uint8_t *e = *(uint8_t **)b; if (e && is_fauna(e)) return e; }
    return NULL;
}

/* ui_debug: on each selection change, what the engine sees - entry count, its active unit, and the squad of the first
   fauna (does the box select of a whole fauna squad select only the leader?) */
static void squad_debug(void *panel)
{
    static uint8_t *lastFirst; static intptr_t lastN = -1; static int logged;
    uint8_t *b, *en;
    if (logged >= 30 || !sel_range(&b, &en)) return;
    intptr_t n = (en - b) / g_selStride; uint8_t *first = n ? *(uint8_t **)b : NULL;
    if (n == lastN && first == lastFirst) return;
    lastN = n; lastFirst = first;
    uint8_t *f = first_selected_fauna();
    if (!f) return;
    int m; uint8_t *sq = squad_of(f, &m);
    uint8_t *lead = sq ? *(uint8_t **)(sq + g_squadLeadOff) : NULL;
    logged++;
    logf_("ui debug: selection %d unit(s), engine active unit %p; fauna %p squad %p (%d members, leader %p%s)", (int)n,
          *(void **)((uint8_t *)panel + g_squadActiveOff), f, sq, m, lead, lead == f ? " = this fauna" : "");
}

/* show/hide an eDialog the way the engine does it: flags bit1 then slot 5, bit2 then slot 6 (PanelSquad::Update shows,
   ePanelActor's hide helper clears both) */
static void dlg_show(uint8_t *w, int on)
{
    uint32_t *fl = (uint32_t *)(w + g_dlgFlagsOff);
    if (on) {
        if (!(*fl & 2)) { *fl |= 2; VCALL(w, DLG_VT_SHOW); }
        if (!(*fl & 4)) { *fl |= 4; VCALL(w, DLG_VT_ENABLE); }
    } else {
        if (*fl & 2) { *fl &= ~2u; VCALL(w, DLG_VT_SHOW); }
        if (*fl & 4) { *fl &= ~4u; VCALL(w, DLG_VT_ENABLE); }
    }
}

/* progress bar: value cur/max (clamped) and caption "cur / max"; redraws only what changed. max must be > 0. */
static void bar_set(uint8_t *bar, float cur, float max, const char *txt)
{
    float v = cur / max; if (v < 0.0f) v = 0.0f; if (v > 1.0f) v = 1.0f;
    if (*(float *)(bar + PB_VALUE) != v) { *(float *)(bar + PB_VALUE) = v; VCALL(bar, PB_VT_REDRAW); }
    if (ms_assign((MsString *)(bar + PB_CAPTION), txt)) VCALL(bar, DLG_VT_TEXT_CHANGED);
}
static void hp_text(char *out, size_t n, float cur, float max) { snprintf(out, n, "%d / %d", (int)(cur + 0.5f), (int)(max + 0.5f)); }

/* one bar + text + container row of the vehicle panel (the mod's own widgets, found by name). The three widgets are
   looked up once per panel (FindChild walks the whole dialog tree) and the caption is written only when it changes.
   warnMissing: log a missing bar. */
typedef struct { void *panel; uint8_t *bar, *pnl, *txt; int looked, shown; char last[32]; int warned[2]; } PanelRow;
static void panel_row(void *panel, const char *barName, const char *txtName, const char *pnlName, int show, float cur, float max, PanelRow *r, int warnMissing)
{
    int rise = show && !r->shown; r->shown = show;
    if (r->panel != panel || !r->looked || rise) {                /* new panel, first run, or a row turning on (once per
                                                                     selection): resolve the widgets, so a rebuilt dialog
                                                                     tree never leaves stale pointers behind */
        FindChildFn find = (FindChildFn)(*(void ***)panel)[DLG_VT_FIND_CHILD];
        MsString nm;
        r->panel = panel; r->looked = 1; r->last[0] = 0;
        ms_set(&nm, barName); r->bar = barName[0] ? (uint8_t *)find(panel, &nm) : NULL;
        ms_set(&nm, pnlName); r->pnl = pnlName[0] ? (uint8_t *)find(panel, &nm) : NULL;
        ms_set(&nm, txtName); r->txt = txtName[0] ? (uint8_t *)find(panel, &nm) : NULL;
        if (r->bar && *(void ***)r->bar != g_vtProgressBar) { if (!r->warned[0]++) logf_("ui: \"%s\" in the vehicle panel is not a progress bar - not drawn", barName); r->bar = NULL; }
        if (r->txt) { void **tvt = *(void ***)r->txt;             /* SetText only on a text widget, or a class sharing it */
            if (!g_vtStaticText || (tvt != g_vtStaticText && tvt[TEXT_VT_SET] != g_vtStaticText[TEXT_VT_SET])) { if (!r->warned[1]++) logf_("ui: \"%s\" in the vehicle panel is not a text widget - text not set", txtName); r->txt = NULL; } }
        if (!r->bar && warnMissing && !r->warned[0]++) logf_("ui: vehicle panel has no progress bar named \"%s\" (add it to the vehicle panel layout) - not drawn", barName);
    }
    if (r->bar) dlg_show(r->bar, show);
    if (r->pnl) dlg_show(r->pnl, show);                           /* optional container (background, icon) shown/hidden with the bar */
    if (r->txt) dlg_show(r->txt, show);
    if (!show) { r->last[0] = 0; return; }
    char txt[32]; hp_text(txt, sizeof txt, cur, max);
    if (r->bar) bar_set(r->bar, cur, max, txt);
    if (r->txt && strcmp(txt, r->last)) {
        strcpy(r->last, txt);
        MsString s; ms_set(&s, txt); ((void (*)(void *, MsString *))(*(void ***)r->txt)[TEXT_VT_SET])(r->txt, &s);
    }
}

/* widgets of the vehicle panel that make no sense for fauna (ini fauna_hide_in_vehicle_panel, default the speed row):
   hidden while a fauna is shown, shown again for any other vehicle. The engine never toggles them itself (it only
   fills them), so giving them back is safe. */
#define HIDE_N 4
static void panel_hide_for_fauna(void *panel, int fauna)
{
    static void *lastPanel; static uint8_t *w[HIDE_N]; static int hidden = -1, n;
    if (panel != lastPanel || (fauna && hidden != 1)) {              /* new panel, or a fauna appears: (re)resolve */
        FindChildFn find = (FindChildFn)(*(void ***)panel)[DLG_VT_FIND_CHILD];
        char buf[sizeof g_faunaHide]; strcpy(buf, g_faunaHide); n = 0;
        for (char *tok = strtok(buf, " ,;"); tok && n < HIDE_N; tok = strtok(NULL, " ,;")) {
            if (strlen(tok) > UI_NAME_MAX) continue;
            MsString nm; ms_set(&nm, tok); w[n] = (uint8_t *)find(panel, &nm);
            if (w[n]) n++;
        }
        if (panel != lastPanel) hidden = -1;
        lastPanel = panel;
    }
    if (fauna) { for (int i = 0; i < n; i++) dlg_show(w[i], 0); hidden = 1; }
    else if (hidden == 1) { for (int i = 0; i < n; i++) dlg_show(w[i], 1); hidden = 0; }
}

/* vehicle panel: bar_health / txt_health are the mod's own widgets. Fauna -> shown and fed from the vitality;
   any other vehicle (or no entity) -> hidden, so normal vehicles look exactly as before. */
static void hook_panel_vehicle_update(void *panel)
{
    UI_TRACE("PanelVehicle::Update", "enter");
    g_pvUpdateOrig(panel);
    UI_TRACE("PanelVehicle::Update", "original returned");
    uint8_t *e = *(uint8_t **)((uint8_t *)panel + g_panelEntOff);
    if (!e) e = first_selected_fauna();                       /* several fauna selected: the engine gives the panel no entity */
    float cur = 0.0f, max = 0.0f; int show = 0;
    if (e && is_fauna(e)) {
        void *vit = entity_vitality(e);
        if (vit) { cur = g_getHealth(vit); max = g_getMaxHealth(vit); show = max > 0.0f; }
    } else if (e && g_uiDebug) {                                /* ui_debug: why is this vehicle not fauna? (first 6 defs) */
        static const uint8_t *seen[6]; static int ns;
        const uint8_t *def = *(const uint8_t *const *)(e + g_entDefOff); int dup = 0;
        for (int i = 0; i < ns; i++) if (seen[i] == def) dup = 1;
        if (!dup && ns < 6) {
            seen[ns++] = def; const uint8_t *b, *en; char props[400] = "";
            int okp = def_props(e, &b, &en);
            if (okp) for (; b + TAG_SIZE <= en; b += TAG_SIZE) { const MsString *nm = (const MsString *)(b + TAG_NAME); if (nm->size && strlen(props) + nm->size + 2 < sizeof props) { strncat(props, ms_cstr(nm), nm->size); strcat(props, " "); } }
            logf_("ui: vehicle panel unit %p is not fauna: flags 0x%02X, def %p, props %s%s (fauna tag id %016llX, prop \"%s\")",
                  e, e[ENT_FLAGS], def, okp ? "" : "[unreadable] ", props, (unsigned long long)TAG_ID(g_tagFaunaBuf), g_faunaProp);
        }
    }
    if (g_faunaHide[0]) panel_hide_for_fauna(panel, e && is_fauna(e));
    static PanelRow rowHp, rowSt;
    panel_row(panel, g_barName, g_txtName, g_pnlName, show, cur, max, &rowHp, 1);
    /* stamina row (optional widgets): fauna with a vitality {Stamina} block (max > 0) */
    float scur = 0.0f, smax = 0.0f; int sshow = 0;
    if (show && g_stamMaxOff) { void *vit = entity_vitality(e); if (vit) { smax = *(float *)((uint8_t *)vit + g_stamMaxOff); scur = *(float *)((uint8_t *)vit + g_stamCurOff); sshow = smax > 0.0f; } }
    if (g_stamMaxOff) panel_row(panel, g_stBarName, g_stTxtName, g_stPnlName, sshow, scur, smax, &rowSt, 0);
}

static void hook_panel_squad_update(void *panel)
{
    UI_TRACE("PanelSquad::Update", "enter");
    if (g_uiDebug) squad_debug(panel);
    g_psUpdateOrig(panel);
    UI_TRACE("PanelSquad::Update", "original returned");
    uint8_t *active = *(uint8_t **)((uint8_t *)panel + g_squadActiveOff);
    /* fauna leader, or several fauna selected (no active entity then - the engine hides the strip for vehicle groups) */
    if (active ? !is_fauna(active) : !first_selected_fauna()) return;
    dlg_show(panel, 1);                                        /* same two steps the engine does for a human leader */
}

static void *hook_panel_squad_get_ids(void *panel)
{
    UI_TRACE("PanelSquad::GetIds", "enter");
    uint8_t **vec = (uint8_t **)g_psGetIdsOrig(panel);            /* NULL or the static vector {begin,end,cap} */
    UI_TRACE("PanelSquad::GetIds", "original returned");
    uint8_t **sv = g_sqVecEnd - 1;
    if (!vec) sv[1] = sv[0];                                      /* NULL = no active unit (vector left stale) or empty: start clean */
    uint8_t *b, *en;
    if (!sel_range(&b, &en)) return vec;
    int added = 0;
    /* fauna (or no) active unit: the engine lists only that unit, so add every selected fauna AND soldier (mixed squads).
       Human active unit: the engine already lists his squad - add only the selected fauna. */
    uint8_t *active = *(uint8_t **)((uint8_t *)panel + g_squadActiveOff);
    int withHumans = active ? is_fauna(active) : first_selected_fauna() != NULL;
    for (; b < en; b += g_selStride) {
        uint8_t *e = *(uint8_t **)b;
        if (!e || !(e[ENT_FLAGS] & 2)) continue;
        if (!is_fauna(e) && !(withHumans && (e[ENT_FLAGS] & 4))) continue;
        uint16_t id = *(uint16_t *)(e + g_entIdOff); int dup = 0;
        for (uint16_t *q = (uint16_t *)sv[0]; q && q < (uint16_t *)sv[1]; q++) if (*q == id) { dup = 1; break; }
        if (dup) continue;
        if (sv[1] != sv[2]) { *(uint16_t *)sv[1] = id; sv[1] += 2; }
        else g_vecU16Emplace(sv, sv[1], &id);
        added++;
    }
    return (added || vec) ? sv : NULL;
}

/* squad strip item (ePanelHuman) showing a fauna unit: the engine's own refresh would build the portrait from a NULL Human*
   and crash (0.2.3 click crash), so for any non-human entity we run only the light Update(full=0) the per-frame tick runs
   (it skips the portrait and the human data gatherer bails out cleanly), then feed the hp bar from the vitality ourselves. */
typedef void (*ItemFn)(void *item);
static ItemFn g_phRefreshOrig, g_phTickOrig;
static void fauna_item_fill(uint8_t *item, const uint8_t *e)
{
    uint8_t *bar = *(uint8_t **)(item + g_phHpBarOff);
    if (!bar || !is_fauna(e)) return;
    void **bvt = *(void ***)bar;                                /* a progress bar (or a subclass with the same redraw) */
    if (bvt != g_vtProgressBar && bvt[PB_VT_REDRAW] != g_vtProgressBar[PB_VT_REDRAW]) {
        static int warned; if (!warned++) logf_("ui: squad item hp bar is not a progress bar - fauna strip hp not drawn"); return; }
    void *vit = entity_vitality(e);
    if (!vit) return;
    float cur = g_getHealth(vit), max = g_getMaxHealth(vit);
    if (!(max > 0.0f)) return;
    char txt[32]; hp_text(txt, sizeof txt, cur, max);
    bar_set(bar, cur, max, txt);
}

/* ---- squad circle icons -----------------------------------------------------------------------------------------------
   Names of the icon files in the engine's squad icon folder (the builder's prefix, "/interface/scene/squad/" = the 56x52
   circle icons; any mod, any resource prefix such as 40k_interface), built once on first use: (1) every key of the engine's
   file repository, (2) if that finds none, the mods folders on disk. Kept sorted (lower case, no extension) for lookups. */
/* resource repository: std::map<std::string, ...>, MSVC tree: node {left, parent, right, color, isnil @+0x19, key @+0x20} */
#define RB_L(n) (*(uint8_t **)(n))
#define RB_P(n) (*(uint8_t **)((n) + 8))
#define RB_R(n) (*(uint8_t **)((n) + 0x10))
#define RB_NIL(n) ((n)[0x19])
static uint8_t *repo_next(uint8_t *n)
{
    if (!RB_NIL(RB_R(n))) { n = RB_R(n); while (!RB_NIL(RB_L(n))) n = RB_L(n); return n; }
    uint8_t *p = RB_P(n);
    while (!RB_NIL(p) && n == RB_R(p)) { n = p; p = RB_P(p); }
    return p;
}
#define ICON_NAME 48
#define MAX_ICONS 65536                                      /* the folder holds thousands (faction packs): grow on demand */
static char g_iconPat[96] = "interface/scene/squad/"; static size_t g_iconPatLen = 22;
static wchar_t g_iconSub[96];                                 /* g_iconPat with backslashes, e.g. interface\scene\squad */
static char (*g_iconNames)[ICON_NAME]; static int g_iconN, g_iconCap, g_iconsBuilt;
static void icons_sort_unique(void);
static void icon_add(const char *nm, size_t n)                /* duplicates are removed when the list is complete (or full) */
{
    if (!n || n >= ICON_NAME) return;
    if (g_iconN >= MAX_ICONS) { icons_sort_unique(); if (g_iconN >= MAX_ICONS) return; }
    if (g_iconN == g_iconCap) {
        int nc = g_iconCap ? g_iconCap * 2 : 1024;
        void *p = realloc(g_iconNames, (size_t)nc * ICON_NAME);
        if (!p) return;
        g_iconNames = p; g_iconCap = nc;
    }
    char *t = g_iconNames[g_iconN++];
    for (size_t i = 0; i < n; i++) { char c = nm[i]; t[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
    t[n] = 0;
}
static int icon_cmp(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }
static void icons_sort_unique(void)
{
    if (!g_iconN) return;
    qsort(g_iconNames, (size_t)g_iconN, ICON_NAME, icon_cmp);
    int w = 1;
    for (int i = 1; i < g_iconN; i++) if (strcmp(g_iconNames[i], g_iconNames[w - 1])) memmove(g_iconNames[w++], g_iconNames[i], ICON_NAME);
    g_iconN = w;
}
/* name (n chars, any case) -> the icon's stored name, NULL if no such file */
static const char *icon_find(const char *nm, size_t n)
{
    char key[ICON_NAME];
    if (!n || n >= ICON_NAME || !g_iconN) return NULL;
    for (size_t i = 0; i < n; i++) { char c = nm[i]; key[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
    key[n] = 0;
    return bsearch(key, g_iconNames, (size_t)g_iconN, ICON_NAME, icon_cmp);
}
/* "<anything><g_iconPat><name>[.ext]" (either slash, any case) -> name */
static void icon_from_path(const char *k, size_t n)
{
    const char *pat = g_iconPat; const size_t pl = g_iconPatLen;
    for (size_t i = 0; i + pl < n; i++) {
        size_t j = 0;
        for (; j < pl; j++) { char c = k[i + j]; if (c == '\\') c = '/'; if (c >= 'A' && c <= 'Z') c += 32; if (c != pat[j]) break; }
        if (j < pl) continue;
        const char *nm = k + i + pl; size_t m = n - i - pl, e = 0;
        for (size_t q = 0; q < m; q++) { if (nm[q] == '/' || nm[q] == '\\') return; if (nm[q] == '.' && !e) e = q; }
        icon_add(nm, e ? e : m);
        return;
    }
}
static void icons_scan_dir(const wchar_t *dir)
{
    wchar_t pat[MAX_PATH * 2]; WIN32_FIND_DATAW fd;
    _snwprintf(pat, MAX_PATH * 2, L"%ls\\*", dir); pat[MAX_PATH * 2 - 1] = 0;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        char nm[64]; int n = WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, nm, sizeof nm, NULL, NULL);
        if (n <= 1) continue;
        char *dot = strrchr(nm, '.'); icon_add(nm, dot ? (size_t)(dot - nm) : strlen(nm));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}
static void icons_scan_mods_root(const wchar_t *root)            /* <root>\*\resource\[40k_]interface\scene\squad */
{
    wchar_t pat[MAX_PATH * 2], d[MAX_PATH * 2]; WIN32_FIND_DATAW fd;
    _snwprintf(pat, MAX_PATH * 2, L"%ls\\*", root); pat[MAX_PATH * 2 - 1] = 0;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
        _snwprintf(d, MAX_PATH * 2, L"%ls\\%ls\\resource\\%ls", root, fd.cFileName, g_iconSub); d[MAX_PATH * 2 - 1] = 0; icons_scan_dir(d);
        _snwprintf(d, MAX_PATH * 2, L"%ls\\%ls\\resource\\40k_%ls", root, fd.cFileName, g_iconSub); d[MAX_PATH * 2 - 1] = 0; icons_scan_dir(d);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}
static void icons_build(void)
{
    uint8_t *head = g_repoMap ? *g_repoMap : NULL;
    if (!head || !g_iconPrefix->size) return;                  /* repository / prefix not set up yet: try again later */
    g_iconsBuilt = 1;
    {   /* "/interface/scene/squad/" -> "interface/scene/squad/" (lower case, '/' separators, trailing '/') */
        const char *p = ms_cstr(g_iconPrefix); size_t n = g_iconPrefix->size, o = 0;
        while (n && (*p == '/' || *p == '\\')) { p++; n--; }
        for (size_t i = 0; i < n && o + 2 < sizeof g_iconPat; i++) { char c = p[i]; if (c == '\\') c = '/'; if (c >= 'A' && c <= 'Z') c += 32; g_iconPat[o++] = c; }
        if (o && g_iconPat[o - 1] != '/') g_iconPat[o++] = '/';
        g_iconPat[o] = 0; g_iconPatLen = o;
        size_t w = 0; for (size_t i = 0; i + 1 < o && w + 1 < 96; i++) g_iconSub[w++] = g_iconPat[i] == '/' ? L'\\' : (wchar_t)g_iconPat[i];
        g_iconSub[w] = 0;
    }
    size_t keys = 0; char sample[200] = "";
    for (uint8_t *n = RB_L(head); n && n != head && !RB_NIL(n) && keys < 4000000; n = repo_next(n), keys++) {
        const MsString *key = (const MsString *)(n + 0x20);
        if (key->size < 27 || key->size > 400) continue;
        int before = g_iconN;
        icon_from_path(ms_cstr(key), key->size);
        if (g_iconN > before && !sample[0]) snprintf(sample, sizeof sample, "%.*s", (int)(key->size < 150 ? key->size : 150), ms_cstr(key));
    }
    const char *src = "repository";
    if (!g_iconN) {                                            /* not in the repository: read the mods folders on disk */
        src = "mods folders";
        wchar_t exe[MAX_PATH], root[MAX_PATH * 2]; GetModuleFileNameW(NULL, exe, MAX_PATH);
        wchar_t *sl = wcsrchr(exe, L'\\'); if (sl) *sl = 0;                    /* ...\binaries\x64 */
        _snwprintf(root, MAX_PATH * 2, L"%ls\\..\\..\\mods", exe); root[MAX_PATH * 2 - 1] = 0; icons_scan_mods_root(root);
        _snwprintf(root, MAX_PATH * 2, L"%ls\\..\\..\\resource\\%ls", exe, g_iconSub); root[MAX_PATH * 2 - 1] = 0; icons_scan_dir(root);
        _snwprintf(root, MAX_PATH * 2, L"%ls\\..\\..\\..\\..\\workshop\\content\\400750", exe); root[MAX_PATH * 2 - 1] = 0; icons_scan_mods_root(root);
    }
    icons_sort_unique();
    char list[600] = "";
    for (int i = 0; i < g_iconN && strlen(list) + strlen(g_iconNames[i]) + 2 < sizeof list; i++) { strcat(list, g_iconNames[i]); strcat(list, " "); }
    logf_("ui: squad icons (%s*) from the %s (%zu repository keys): %d: %s%s%s", g_iconPat, src, keys, g_iconN, list, sample[0] ? "| e.g. " : "", sample);
}
/* first def prop of the entity whose name is one of those icons (Tag name = std::string at +0x40) */
static const char *fauna_icon_name(const uint8_t *e)
{
    if (!g_iconsBuilt) icons_build();
    const uint8_t *b, *en;
    if (!g_iconN || !def_props(e, &b, &en)) return NULL;
    for (const uint8_t *t = b; t + TAG_SIZE <= en; t += TAG_SIZE) {
        const MsString *nm = (const MsString *)(t + TAG_NAME);
        const char *hit = icon_find(ms_cstr(nm), nm->size);
        if (hit) return hit;
    }
    static int miss; if (miss < 6) {                          /* say which props were tried, so a typo is easy to spot */
        miss++; char tried[300] = "";
        for (const uint8_t *t = b; t + TAG_SIZE <= en; t += TAG_SIZE) {
            const MsString *nm = (const MsString *)(t + TAG_NAME);
            if (nm->size && strlen(tried) + nm->size + 2 < sizeof tried) { strncat(tried, ms_cstr(nm), nm->size); strcat(tried, " "); }
        }
        logf_("ui: fauna circle: no prop names a file in %s (props: %s) - using the default icon \"%s\"", g_iconPat, tried, g_faunaIconDefault);
    }
    return NULL;
}
static void fauna_item_icon(uint8_t *item, uint8_t *e)
{
    if (!g_okIcons || *(uint32_t *)(item + g_phModeOff) != 3) return;
    uint8_t *w = *(uint8_t **)(item + g_phWidgetOff);
    if (!w) return;
    uint8_t *comp = w + g_phCompOff;
    __attribute__((aligned(16))) uint8_t str[0x20];
    const char *name = fauna_icon_name(e);
    if (!name && g_faunaIconDefault[0]) {                      /* ini fauna_default_icon, only if that file exists (else the engine draws a checker) */
        if (icon_find(g_faunaIconDefault, strlen(g_faunaIconDefault))) name = g_faunaIconDefault;
        else if (g_iconsBuilt) { static int warned; if (!warned++) logf_("ui: fauna_default_icon \"%s\" is not a file in %s - no default icon", g_faunaIconDefault, g_iconPat); }
    }
    if (name) {
        char path[160];
        int n = snprintf(path, sizeof path, "%.*s%s", (int)g_iconPrefix->size, ms_cstr(g_iconPrefix), name);
        if (n >= (int)sizeof path) n = (int)sizeof path - 1;
        g_strEmptyCtor(str); g_strAssign(str, path, (size_t)n);
        static int logged; if (logged < 8) { logged++; logf_("ui: fauna circle icon \"%s\"", path); }
    } else g_iconBuilder(str, e, 3);                           /* engine: vehicle class icon from the def props, or empty */
    g_setImage(comp, 0, str, 0);
    g_strDtor(str);
    uint32_t col = 0; g_iconColor(&col, e);
    if (*(uint32_t *)(comp + g_phColorOff) != col) { *(uint32_t *)(comp + g_phColorOff) = col; VCALL(comp, 1); }
}

static void hook_panel_human_refresh(void *item)
{
    uint8_t *e = *(uint8_t **)((uint8_t *)item + g_phEntOff);
    if (!e || (e[ENT_FLAGS] & 4)) { g_phRefreshOrig(item); return; }          /* human (or cleared): engine path */
    UI_TRACE("ePanelHuman::Refresh (non-human)", "enter");
    ((void (*)(void *, int))g_phUpdate)(item, 0);
    fauna_item_fill((uint8_t *)item, e);
    if (is_fauna(e)) fauna_item_icon((uint8_t *)item, e);
    UI_TRACE("ePanelHuman::Refresh (non-human)", "done");
}
static void hook_panel_human_tick(void *item)
{
    g_phTickOrig(item);
    uint8_t *e = *(uint8_t **)((uint8_t *)item + g_phEntOff);
    if (e && !(e[ENT_FLAGS] & 4)) fauna_item_fill((uint8_t *)item, e);
}

static void *hook_panel_squad_create_item(void *panel, void *entity)
{
    UI_TRACE("PanelSquad::CreateItem", "enter");
    if (entity && is_fauna((uint8_t *)entity) && g_psCreateItemHuman) return g_psCreateItemHuman(panel, entity);
    return g_psCreateItemOrig(panel, entity);
}

static void *hook_panel_squad_get_prop(void *panel, void *out, void *entity)
{
    UI_TRACE("PanelSquad::GetProperty", "enter");
    if (entity && is_fauna((uint8_t *)entity)) return g_strEmptyCtor(out);       /* no role icon: the vehicle brain has no squad role */
    return g_psGetPropOrig(panel, out, entity);
}

/* PanelSquad::CreateItem with the item template forced to "panel_human": the function's own prologue (copied, it has no
   RIP-relative operand - find_ui checked the bytes), then mov rdx,"panel_human" and a jump to where the engine continues
   after choosing the template */
static int build_create_item_entry(void)
{
    uint8_t *stub = VirtualAlloc(NULL, 0x100, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!stub) return 0;
    uint8_t *p = stub;
    memcpy(p, g_createItemFn, CI_PROLOGUE_LEN); p += CI_PROLOGUE_LEN;
    *p++ = 0x48; *p++ = 0xBA; memcpy(p, &g_strPanelHuman, 8); p += 8;              /* mov rdx, imm64 */
    *p++ = 0xFF; *p++ = 0x25; memset(p, 0, 4); p += 4; memcpy(p, &g_createItemResume, 8); p += 8;   /* jmp [rip+0] ; abs64 */
    FlushInstructionCache(GetCurrentProcess(), stub, 0x100);
    g_psCreateItemHuman = (PanelCreateItemFn)stub;
    return 1;
}

static int install_ui(void)
{
    if (!build_create_item_entry()) { logf_("ui: no memory for the CreateItem entry stub"); return 0; }
    int ok = vt_hook(g_vtPanelVehicle, 73, (void *)hook_panel_vehicle_update, (void **)&g_pvUpdateOrig)
           & vt_hook(g_vtPanelSquad, 70, (void *)hook_panel_squad_update, (void **)&g_psUpdateOrig)
           & vt_hook(g_vtPanelSquad, 71, (void *)hook_panel_squad_get_ids, (void **)&g_psGetIdsOrig)
           & vt_hook(g_vtPanelSquad, 72, (void *)hook_panel_squad_create_item, (void **)&g_psCreateItemOrig)
           & vt_hook(g_vtPanelSquad, 74, (void *)hook_panel_squad_get_prop, (void **)&g_psGetPropOrig)
           & vt_hook(g_vtPanelHuman, 68, (void *)hook_panel_human_refresh, (void **)&g_phRefreshOrig)
           & vt_hook(g_vtPanelHuman, 70, (void *)hook_panel_human_tick, (void **)&g_phTickOrig);
    if (!ok) logf_("ui: VirtualProtect on a vtable failed");
    return ok;
}
#endif /* FX_TEST */
