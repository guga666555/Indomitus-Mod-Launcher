/* Host-side simulation of the research tree zoom (zm_capture / zm_apply / zm_wheel / the pane hooks) against fake
   icons, lines, containers and a scroll pane that behave like the engine's: SetSize (vt[16]) clamps to min/max,
   LineSetup puts the line at the first icon's centre, the scroll pane clamps its position to [0, content - view].
   Build/run: gcc -O1 -o out/zoom_test tests/zoom_test.c -lm && ./out/zoom_test */
#define CU_TEST
#define main cu_offline_main
#include "../src/gem_conquestui.c"
#undef main
#include <assert.h>
#include <math.h>

#define NI 6
enum { F_POS = 0x98, F_SIZE = 0xA0, F_PREF = 0x120, F_MIN = 0x128, F_MAX = 0x130, F_PARENT = 0xF0, F_CHILD = 0xD8,
       T_UNITS = 0x230, T_LINES = 0x220, T_PANE = 0x210, L_A = 0x1A0, L_B = 0x1A8, L_CENTRE = 0x300 };
static void *vt_icon[80], *vt_other[80], *vt_line[80], *vt_tree[80], *vt_pane[80], *vt_cont[80];
static _Alignas(16) uint8_t icons[NI][0x400], other[0x400], lineObj[NI - 1][0x400], units[0x400], lines[0x400], tree[0x400], pane[0x400];
static void *ukids[NI + 1], *lkids[NI - 1];
static Pt scroll, view = { 400, 300 }, origin = { 100, 100 }, cursor;
static int setsizes, poschanges, linesetups, invalidates, arranges, foreignWheel;

static void f_setsize(void *d, unsigned long long v)
{
    Pt s = { (int32_t)(uint32_t)v, (int32_t)(v >> 32) }, mn = PT(d, F_MIN), mx = PT(d, F_MAX);
    if (mn.x && s.x < mn.x) s.x = mn.x;
    if (mn.y && s.y < mn.y) s.y = mn.y;
    if (mx.x && s.x > mx.x) s.x = mx.x;
    if (mx.y && s.y > mx.y) s.y = mx.y;
    PT(d, F_SIZE) = s; setsizes++;
}
static void f_poschanged(void *d) { poschanges++; }
static void f_invalidate(void *d) { invalidates++; }
static void f_linesetup(void *l, void *a, void *b)
{
    PTR(l, L_A) = a; PTR(l, L_B) = b;
    Pt c = { PT(a, F_POS).x + PT(a, F_SIZE).x / 2, PT(a, F_POS).y + PT(a, F_SIZE).y / 2 };
    PT(l, L_CENTRE) = c; linesetups++;
}
static Pt content(void)          /* what the canvas measures: max(pos + preferred-or-size) */
{
    Pt m = { 0, 0 };
    for (int i = 0; i < NI; i++) {
        Pt p = PT(icons[i], F_POS), s = PT(icons[i], F_PREF);
        if (!s.x && !s.y) s = PT(icons[i], F_SIZE);
        if (p.x + s.x > m.x) m.x = p.x + s.x;
        if (p.y + s.y > m.y) m.y = p.y + s.y;
    }
    return m;
}
static Pt range;                 /* scroll range as of the last Arrange */
static void clamp_scroll(void)
{
    if (scroll.x > range.x) scroll.x = range.x;
    if (scroll.y > range.y) scroll.y = range.y;
    if (scroll.x < 0) scroll.x = 0;
    if (scroll.y < 0) scroll.y = 0;
}
static Pt *f_getscroll(void *p, Pt *o) { *o = scroll; return o; }
static void f_setscroll(void *p, const Pt *pos, int flag) { scroll = *pos; clamp_scroll(); }
static Pt *f_cursor(Pt *o) { *o = cursor; return o; }
static Pt *f_tolocal(void *d, Pt *o, const Pt *g) { o->x = g->x - origin.x + scroll.x; o->y = g->y - origin.y + scroll.y; return o; }
static void *f_arrange(void *lm, void *p, void *a3, void *a4)
{
    Pt c = content(); range.x = c.x > view.x ? c.x - view.x : 0; range.y = c.y > view.y ? c.y - view.y : 0;
    clamp_scroll(); arranges++; return NULL;
}
static void *f_wheel(void *p, void *n, void *a3, void *a4) { foreignWheel++; return NULL; }
static void *f_noop(void *a, void *b, void *c, void *d) { return NULL; }
static void layout_pass(void) { hk_pane_arrange(NULL, pane, NULL, NULL); }

/* unit names: icon i has a "description" StaticText (style string at +0x150, vt[69] applies it), except icon 5 (none)
   and icon 4 (a style without f(family): left alone). Fonts loaded: techno_narrow 15/18/22/27, main 21/30. */
#define S_STYLE 0x150
static void *vt_stext[80], *vt_stext2[80];
static _Alignas(16) uint8_t descs[NI][0x200];
static int applies;
static const char *STYLE0 = "x.e(shadow)f(techno_narrow)s(18)", *STYLE_NOFAM = "x.e(shadow)s(18)";
static void f_assign_raw(MsString *m, const char *p, size_t n)
{
    if (m->cap > 15) free((void *)m->u.ptr);
    if (n < 16) { memcpy(m->u.buf, p, n); m->u.buf[n] = 0; m->cap = 15; }
    else { char *b = malloc(n + 1); memcpy(b, p, n); b[n] = 0; m->u.ptr = b; m->cap = n; }
    m->size = n;
}
static void *f_assign(MsString *m, const char *p, size_t n) { f_assign_raw(m, p, n); return m; }
static void f_apply_style(void *d) { applies++; }
static void *f_findchild(void *dlg, const MsString *name)
{
    if (strcmp(ms_data(name), "description")) return NULL;
    for (int i = 0; i < NI - 1; i++) if (dlg == icons[i]) return descs[i];
    return NULL;
}
static void f_tag(void *tag, const MsString *s)        /* a Tag is compared by its first qword: a hash of the name */
{
    uint64_t h = 1469598103934665603ULL; const char *p = ms_data(s);
    for (size_t i = 0; i < s->size; i++) h = (h ^ (uint8_t)p[i]) * 1099511628211ULL;
    memcpy(tag, &h, 8);
}
static void f_tagdtor(void *tag) { }
static unsigned char f_tageq(const void *a, const void *b) { return !memcmp(a, b, 8); }
#define NF 6
static _Alignas(16) uint8_t fonts[NF][0xA0];
static uint8_t *fontList[NF], *fontVec[2];
static void add_font(int i, const char *fam, int size)
{
    MsString n; ms_view(&n, fam); f_tag(fonts[i] + 0x38, &n); fonts[i][0x9D] = (uint8_t)size; fontList[i] = fonts[i];
}
static void fresh_texts(void)   /* a newly opened tree: names at their layout style */
{
    for (int i = 0; i < NI - 1; i++) f_assign_raw((MsString *)(descs[i] + S_STYLE), i == 4 ? STYLE_NOFAM : STYLE0, strlen(i == 4 ? STYLE_NOFAM : STYLE0));
}
static const char *style_of(int i) { return ms_data((MsString *)(descs[i] + S_STYLE)); }
static void check_names(int size, const char *tag)
{
    char want[64]; snprintf(want, sizeof want, "x.e(shadow)f(techno_narrow)s(%d)", size);
    for (int i = 0; i < 4; i++) assert(!strcmp(style_of(i), want));
    assert(!strcmp(style_of(4), STYLE_NOFAM));
    printf("  ok: %s (unit names \"%s\")\n", tag, want);
}

static const Pt basePos[NI] = { { 0, 0 }, { 136, 0 }, { 272, 0 }, { 136, 164 }, { 272, 164 }, { 408, 328 } };
static void check_zoom(double z, const char *tag)
{
    for (int i = 0; i < NI; i++) {
        Pt p = PT(icons[i], F_POS), s = PT(icons[i], F_SIZE);
        assert(p.x == zscale(basePos[i].x, z) && p.y == zscale(basePos[i].y, z));
        assert(s.x == zscale(116, z) && s.y == zscale(144, z));
    }
    for (int i = 0; i < NI - 1; i++) {   /* every line rebuilt from its icon's current centre */
        uint8_t *a = PTR(lineObj[i], L_A);
        Pt c = PT(lineObj[i], L_CENTRE);
        assert(c.x == PT(a, F_POS).x + PT(a, F_SIZE).x / 2 && c.y == PT(a, F_POS).y + PT(a, F_SIZE).y / 2);
    }
    assert(PT(other, F_POS).x == 777 && PT(other, F_SIZE).x == 50);   /* a non-icon child is never touched */
    printf("  ok: %s (zoom %d%%, scroll %d,%d)\n", tag, (int)(g_zoom * 100 + 0.5), scroll.x, scroll.y);
}
static Pt local_at_cursor(void) { Pt l; f_tolocal(units, &l, &cursor); return l; }

int main(void)
{
    O_DLG_POS = F_POS; O_DLG_SIZE = F_SIZE; O_DLG_PREF = F_PREF; O_DLG_MIN = F_MIN; O_DLG_MAX = F_MAX; O_DLG_PARENT = F_PARENT;
    O_DLG_CHILD = F_CHILD; O_TREE_UNITS = T_UNITS; O_TREE_LINES = T_LINES; O_TREE_PANE = T_PANE; O_LINE_A = L_A; O_LINE_B = L_B;
    V_ICON = vt_icon; V_LINE = vt_line; V_TREE = vt_tree; V_PANE = vt_pane;
    vt_icon[SL_SETSIZE] = (void *)f_setsize; vt_icon[SL_POSCHANGED] = (void *)f_poschanged;
    vt_cont[SL_INVALIDATE] = (void *)f_invalidate;
    g_lineSetup = f_linesetup; g_getScroll = f_getscroll; g_setScroll = f_setscroll; g_cursor = f_cursor; g_toLocal = f_tolocal;
    o_paneArrange = (AnyFn)f_arrange; o_paneWheel = (AnyFn)f_wheel; o_treeLayoutInit = (AnyFn)f_noop; o_treeClose = (AnyFn)f_noop;
    g_verbose = 0;
    g_zoomStep = 1.15; g_zoomDelayMs = 0;   /* the first part checks each level without the delay */
    /* unit names */
    V_STEXT = vt_stext; SL_TEXT_STYLE = 69; O_TEXT_STYLE = S_STYLE; O_FONT_SIZE = 0x9D; O_FONT_NAME = 0x38;
    vt_stext[69] = vt_stext2[69] = (void *)f_apply_style;   /* desc 3 is a subclass with the same style code */
    vt_icon[69] = (void *)f_noop;
    g_findChild = f_findchild; g_strAssign = f_assign; g_tagFromStr = f_tag; g_tagDtor = f_tagdtor; g_tagEq = f_tageq;
    add_font(0, "main", 21); add_font(1, "techno_narrow", 18); add_font(2, "techno_narrow", 15);
    add_font(3, "main", 30); add_font(4, "techno_narrow", 27); add_font(5, "techno_narrow", 22);
    fontVec[0] = (uint8_t *)fontList; fontVec[1] = (uint8_t *)(fontList + NF); g_fontVec = fontVec;
    g_okZoomText = 1;
    for (int i = 0; i < NI - 1; i++) *(void ***)descs[i] = i == 3 ? vt_stext2 : vt_stext;
    fresh_texts();

    *(void ***)tree = vt_tree; *(void ***)pane = vt_pane; *(void ***)units = vt_cont; *(void ***)lines = vt_cont; *(void ***)other = vt_other;
    PTR(tree, T_UNITS) = units; PTR(tree, T_LINES) = lines; PTR(tree, T_PANE) = pane;
    for (int i = 0; i < NI; i++) {
        *(void ***)icons[i] = vt_icon; PTR(icons[i], F_PARENT) = units;
        PT(icons[i], F_POS) = basePos[i]; PT(icons[i], F_SIZE) = (Pt){ 116, 144 };
        PT(icons[i], F_PREF) = i == 0 ? (Pt){ 0, 0 } : (Pt){ 116, 144 };   /* icon 0: preferred size unset */
        PT(icons[i], F_MIN) = i == 1 ? (Pt){ 100, 120 } : (Pt){ 0, 0 };    /* icon 1: a min size that must scale too */
        ukids[i] = icons[i];
    }
    PTR(other, F_PARENT) = units; PT(other, F_POS) = (Pt){ 777, 0 }; PT(other, F_SIZE) = (Pt){ 50, 50 }; ukids[NI] = other;
    PTR(units, F_CHILD) = (uint8_t *)ukids; PTR(units, F_CHILD + 8) = (uint8_t *)(ukids + NI + 1);
    for (int i = 0; i < NI - 1; i++) {
        *(void ***)lineObj[i] = vt_line; PTR(lineObj[i], F_PARENT) = lines;
        f_linesetup(lineObj[i], icons[i], icons[i + 1]); lkids[i] = lineObj[i];
    }
    PTR(lines, F_CHILD) = (uint8_t *)lkids; PTR(lines, F_CHILD + 8) = (uint8_t *)(lkids + NI - 1);

    printf("research tree opens at 100%%:\n");
    hk_tree_layoutinit(tree, NULL, NULL, NULL);
    assert(g_zm.tree == tree && g_zm.n == NI);
    layout_pass(); check_zoom(1.0, "captured, nothing changed");
    assert(setsizes == 0 && poschanges == 0);
    check_names(18, "names bound at their layout size, untouched");
    assert(applies == 0 && g_zm.nsizes == 4 && g_zm.sizes[0] == 15 && g_zm.sizes[3] == 27);
    assert(g_zm.it[0].text && !g_zm.it[4].text && !g_zm.it[5].text);

    printf("wheel over another scroll pane:\n");
    hk_pane_wheel(other, (void *)(intptr_t)-1, NULL, NULL);
    assert(foreignWheel == 1 && g_zoom == 1.0); printf("  ok: passed to the game\n");

    printf("zoom in at the mouse:\n");
    cursor = (Pt){ 100 + 200, 100 + 150 };
    Pt before = local_at_cursor();
    hk_pane_wheel(pane, (void *)(intptr_t)-2, NULL, NULL);          /* two notches up */
    double r = g_zoom / 1.0;
    assert(fabs(g_zoom - 1.15 * 1.15) < 1e-9);
    layout_pass(); layout_pass();
    check_zoom(g_zoom, "two notches in");
    Pt after = local_at_cursor();
    assert(abs(after.x - zscale(before.x, r)) <= 1 && abs(after.y - zscale(before.y, r)) <= 1);
    printf("  ok: the point under the mouse stayed under it\n");
    assert(PT(icons[0], F_PREF).x == PT(icons[0], F_SIZE).x);         /* unset preferred size follows the zoom */
    assert(PT(icons[1], F_MIN).x == zscale(100, g_zoom));
    check_names(22, "132% wants 24: the largest techno_narrow size <= 24 (never main_21)");
    assert(applies == 4);

    printf("zoom in past the maximum, then out past the minimum:\n");
    hk_pane_wheel(pane, (void *)(intptr_t)-9, NULL, NULL); layout_pass(); layout_pass();
    assert(g_zoom == g_zoomMax); check_zoom(g_zoom, "clamped at the maximum");
    check_names(27, "150% wants 27");
    hk_pane_wheel(pane, (void *)(intptr_t)20, NULL, NULL); layout_pass(); layout_pass();
    assert(g_zoom == g_zoomMin); check_zoom(g_zoom, "clamped at the minimum");
    check_names(15, "40% wants 7: no size that small, the smallest one");
    assert(PT(icons[1], F_SIZE).x == zscale(116, g_zoomMin));          /* the scaled min size did not block it */

    printf("back to 100%%:\n");
    while (g_zoom < 1.0) { hk_pane_wheel(pane, (void *)(intptr_t)-1, NULL, NULL); layout_pass(); layout_pass(); }
    assert(g_zoom == 1.0); check_zoom(1.0, "exactly 100% again");
    assert(PT(icons[0], F_PREF).x == 0 && PT(icons[1], F_MIN).x == 100);   /* zoom-1 values restored exactly */
    check_names(18, "the layout's own size again");
    applies = 0; zm_apply(1.0); assert(applies == 0);   /* nothing re-applied when the size does not change */

    printf("modifier key required (zoom_modifier=ctrl), not held:\n");
    g_zoomKey = 0x11; foreignWheel = 0;
    hk_pane_wheel(pane, (void *)(intptr_t)-1, NULL, NULL);
    assert(foreignWheel == 1 && g_zoom == 1.0); printf("  ok: vanilla scrolling\n");
    g_zoomKey = 0;

    printf("reopening the tree keeps the zoom:\n");
    hk_pane_wheel(pane, (void *)(intptr_t)-1, NULL, NULL); layout_pass();
    double kept = g_zoom;
    hk_tree_close(tree, NULL, NULL, NULL);
    assert(!g_zm.tree);
    for (int i = 0; i < NI; i++) { PT(icons[i], F_POS) = basePos[i]; PT(icons[i], F_SIZE) = (Pt){ 116, 144 }; }  /* a fresh tree */
    PT(icons[0], F_PREF) = (Pt){ 0, 0 }; PT(icons[1], F_MIN) = (Pt){ 100, 120 };
    fresh_texts();
    hk_tree_layoutinit(tree, NULL, NULL, NULL);
    assert(g_zoom == kept); check_zoom(kept, "fresh tree opened at the kept zoom");
    check_names(18, "115% wants 21: techno_narrow has 18");

    printf("delay between levels (zoom_delay=150):\n");
    g_zoomDelayMs = 150; g_testNow = 10000;
    double z0 = g_zoom;
    hk_pane_wheel(pane, (void *)(intptr_t)-1, NULL, NULL); layout_pass();
    double z1 = g_zoom; assert(z1 > z0);
    g_testNow += 60; foreignWheel = 0;
    hk_pane_wheel(pane, (void *)(intptr_t)-1, NULL, NULL);          /* too soon: dropped, not passed to the game */
    assert(g_zoom == z1 && foreignWheel == 0);
    g_testNow += 100;
    hk_pane_wheel(pane, (void *)(intptr_t)-1, NULL, NULL); layout_pass();
    assert(g_zoom > z1); check_zoom(g_zoom, "a notch 60 ms later was dropped, one 160 ms later applied");

    printf("style parsing:\n");
    size_t a, b;
    assert(style_arg("x.e(shadow)f(techno_narrow)s(18)", 32, 's', &a, &b) && a == 29 && b == 31);
    assert(style_arg("x.e(shadow)f(techno_narrow)s(18)", 32, 'f', &a, &b) && a == 13 && b == 26);
    assert(!style_arg("f(a)e(x", 7, 's', &a, &b));          /* unterminated */
    assert(!style_arg("e(shadows(3))", 13, 's', &a, &b));   /* inside another argument */
    printf("  ok\n");

    printf("unit names off (zoom_unit_names=0):\n");
    hk_tree_close(tree, NULL, NULL, NULL); fresh_texts(); g_wantZoomText = 0; applies = 0;
    hk_tree_layoutinit(tree, NULL, NULL, NULL);
    assert(!g_zm.it[0].text && applies == 0 && !strcmp(style_of(0), STYLE0)); printf("  ok: names untouched\n");

    printf("ALL ZOOM TESTS PASSED\n");
    return 0;
}
