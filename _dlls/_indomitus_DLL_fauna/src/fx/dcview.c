/* GEM Fauna - direct-control HUD for fauna
 * Part of the unity build: #included by gem_fauna.c, not compiled on its own (after ui.c).
 *
 * In direct control a vehicle-class unit gets GameControl::eVehicleView (SceneGameManualView "vehicle_view"), built in
 * code, not from a layout: two rows {panel, picture, text, loc key} at view+ROW1 / view+ROW1+0x50, filled by the view's
 * Update (vt slot 2 = OnEvent: code 1 -> Update):
 *   row 1 = /interface/scene/vehicle_view/fuel  - text from the fuel chassis (empty for a fauna: a foot chassis has none)
 *   row 2 = /interface/scene/vehicle_view/speed - "<speed>\n(<ground>)"
 * For a fauna, after the engine's Update: row 1 shows health "cur / max" with the human view's health icon, row 2 shows
 * stamina "cur / max" with the stamina icon (hidden when the def has no Stamina block). Any other vehicle gets the
 * engine's icons back. UI only (game and editor).
 */

typedef void (*ViewEventFn)(void *view, int code, void *a, void *b);
static void **g_vtVehView; static ViewEventFn g_vvEventOrig;
static uint32_t g_vvEntOff, g_vvRowsOff, g_vvText1Off;
#define VV_ROW_STRIDE  0x50
#define VV_ROW_PANEL  (-0x20)
#define VV_ROW_PIC    (-0x10)
static int g_okDcView, g_wantDcView = 1;
static char g_dcHpIcon[96] = "/interface/scene/human_view/health", g_dcStIcon[96] = "/interface/scene/human_view/stamina";
static char g_dcFuelIcon[96] = "/interface/scene/vehicle_view/fuel", g_dcSpeedIcon[96] = "/interface/scene/vehicle_view/speed";

static int find_dcview(void)
{
    if (!g_okIcons || !g_vtStaticText || !g_dlgFlagsOff) { logf_("dc view: needs the squad icon and text widget code (ui finders)"); return 0; }
    g_vtVehView = find_vtable_rtti(".?AVeVehicleView@GameControl@@");
    uint8_t *s2 = g_vtVehView ? (uint8_t *)g_vtVehView[2] : NULL;
    if (!s2 || !in_text(s2) || s2[0] != 0x83 || s2[1] != 0xFA || s2[2] != 0x01 || s2[3] != 0x0F || s2[4] != 0x84 || s2[9] != 0xC3) { logf_("dc view: eVehicleView not recognised"); return 0; }
    uint8_t *upd = s2 + 9 + *(int32_t *)(s2 + 5), *flag = NULL, *call1 = NULL, *call2 = NULL;
    for (uint8_t *q = upd; q < upd + 0x80 && !g_vvEntOff; q++)                 /* lea r64,[view+ENT] */
        if ((q[0] & 0xFB) == 0x48 && q[1] == 0x8D && (q[2] & 0xC0) == 0x80 && (q[2] & 7) != 4 && *(uint32_t *)(q + 3) >= 0x100 && *(uint32_t *)(q + 3) < 0x400) g_vvEntOff = *(uint32_t *)(q + 3);
    for (uint8_t *q = upd; q < upd + 0x200 && !flag; q++)                      /* cmp byte [view+ROWS],r12b */
        if (q[0] == 0x44 && q[1] == 0x38 && (q[2] & 0xC0) == 0x80 && (q[2] & 7) != 4) { flag = q; g_vvRowsOff = *(uint32_t *)(q + 3); }
    static const uint8_t settext[] = { 0xFF, 0x90, 0xC8, 0x00, 0x00, 0x00 };   /* call [rax+0xC8] = SetText (slot 25) */
    if (flag) call1 = find_bytes(flag, 0x200, settext, sizeof settext);
    if (call1) call2 = find_bytes(call1 + 6, 0x400, settext, sizeof settext);
    uint32_t t[2] = { 0, 0 }; uint8_t *from[2] = { flag, call1 }, *to[2] = { call1, call2 };
    for (int i = 0; i < 2 && to[1]; i++)                                       /* last mov r64,[view+TEXT] before each SetText */
        for (uint8_t *q = from[i]; q < to[i]; q++)
            if ((q[0] & 0xFB) == 0x48 && q[1] == 0x8B && (q[2] & 0xC0) == 0x80 && (q[2] & 7) != 4) {
                uint32_t d = *(uint32_t *)(q + 3);
                if (d >= 0x150 && d < 0x400 && d != g_vvEntOff) t[i] = d;
            }
    if (!g_vvEntOff || !g_vvRowsOff || !t[0] || t[1] != t[0] + VV_ROW_STRIDE) { logf_("dc view: eVehicleView rows not recognised (ent +0x%X, rows +0x%X, text +0x%X/+0x%X)", g_vvEntOff, g_vvRowsOff, t[0], t[1]); return 0; }
    g_vvText1Off = t[0];
    logf_("dc view: eVehicleView vt exe+0x%llX (Update exe+0x%llX): entity +0x%X, rows flag +0x%X, row texts +0x%X/+0x%X",
          EXE_OFF(g_vtVehView), EXE_OFF(upd), g_vvEntOff, g_vvRowsOff, t[0], t[1]);
    return 1;
}

#ifndef FX_TEST
static void dc_set_icon(uint8_t *pic, const char *path)
{
    if (!pic || !path[0]) return;
    __attribute__((aligned(16))) uint8_t str[0x20];
    g_strEmptyCtor(str); g_strAssign(str, path, strlen(path));
    g_setImage(pic + g_phCompOff, 0, str, 0);
    g_strDtor(str);
}
static void dc_set_text(uint8_t *t, const char *txt, char last[32])
{
    if (!t || !strcmp(txt, last)) return;                      /* unchanged: no SetText (it re-lays the widget out) */
    void **tvt = *(void ***)t;
    if (tvt != g_vtStaticText && tvt[TEXT_VT_SET] != g_vtStaticText[TEXT_VT_SET]) return;
    strcpy(last, txt);
    MsString s; ms_set(&s, txt); ((void (*)(void *, MsString *))tvt[TEXT_VT_SET])(t, &s);
}
static void dc_fauna_rows(uint8_t *view)
{
    static uint8_t *lastView, *lastPic; static int iconState = -1;   /* -1 unknown, 0 engine icons, 1 health/no stamina, 2 health+stamina */
    static char last1[32], last2[32];
    if (!view[g_vvRowsOff]) return;
    uint8_t *e = *(uint8_t **)(view + g_vvEntOff);
    uint8_t *row1 = view + g_vvText1Off, *row2 = row1 + VV_ROW_STRIDE;
    uint8_t *t1 = *(uint8_t **)row1, *t2 = *(uint8_t **)row2;
    uint8_t *p1 = *(uint8_t **)(row1 + VV_ROW_PIC), *p2 = *(uint8_t **)(row2 + VV_ROW_PIC), *pn2 = *(uint8_t **)(row2 + VV_ROW_PANEL);
    if (view != lastView || p1 != lastPic) { lastView = view; lastPic = p1; iconState = -1; last1[0] = last2[0] = 0; }   /* rows rebuilt */
    void *vit = e && is_fauna(e) ? entity_vitality(e) : NULL;
    if (!vit) {
        if (iconState > 0) { dc_set_icon(p1, g_dcFuelIcon); dc_set_icon(p2, g_dcSpeedIcon); if (pn2) dlg_show(pn2, 1); }
        iconState = 0; last1[0] = last2[0] = 0;
        return;
    }
    float max = g_getMaxHealth(vit), smax = g_stamMaxOff ? *(float *)((uint8_t *)vit + g_stamMaxOff) : 0.0f;
    int want = smax > 0.0f ? 2 : 1;
    if (iconState != want) {
        dc_set_icon(p1, g_dcHpIcon);
        if (want == 2) { dc_set_icon(p2, g_dcStIcon); if (pn2) dlg_show(pn2, 1); }
        else if (pn2) dlg_show(pn2, 0);
        iconState = want;
        static int logged; if (logged < 4) { logged++; logf_("dc view: fauna %p in direct control - health%s row(s)", e, want == 2 ? " + stamina" : " (no Stamina block: speed row hidden)"); }
    }
    char txt[32];
    if (max > 0.0f) { hp_text(txt, sizeof txt, g_getHealth(vit), max); dc_set_text(t1, txt, last1); }
    if (want == 2) { hp_text(txt, sizeof txt, *(float *)((uint8_t *)vit + g_stamCurOff), smax); dc_set_text(t2, txt, last2); }
}
static void fx_vehview_event(void *view, int code, void *a, void *b)
{
    g_vvEventOrig(view, code, a, b);
    if (code == 1) dc_fauna_rows((uint8_t *)view);
}
static int install_dcview(void)
{
    return vt_hook(g_vtVehView, 2, (void *)fx_vehview_event, (void **)&g_vvEventOrig);
}
#endif /* FX_TEST */
