/* Host-side tests, included by src/gem_researchpath.c when built with -DRP_TEST (Linux, gcc).
 *   rp_test <exe>...   maps each exe at its image base and runs every finder; for the game exe it also swaps a few
 *                      vtable slots for fake hooks (as GEM ConquestUI does) and checks the finders still pass by
 *                      reading the original slots from the file.
 *   rp_test --logic    fake research tree (icons, nodes, prerequisites, engine functions) -> path, highlight,
 *                      tooltip text, string free path.
 */
#include <assert.h>

static int map_exe(const char *path)
{
    FILE *f = fopen(path, "rb"); if (!f) { printf("cannot open %s\n", path); return 0; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *file = malloc(n); if (fread(file, 1, n, f) != (size_t)n) return 0; fclose(f);
    uint32_t pe = rd32(file + 0x3c);
    uint16_t nsec = *(uint16_t *)(file + pe + 6), opt = *(uint16_t *)(file + pe + 20);
    uint64_t base; memcpy(&base, file + pe + 24 + 24, 8);
    uint32_t imgsz = rd32(file + pe + 24 + 56);
    if (g_base) munmap(g_base, 0x2000000);
    g_base = mmap((void *)base, imgsz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (g_base != (uint8_t *)base) { printf("mmap at image base failed\n"); return 0; }
    memcpy(g_base, file, 0x1000);
    g_hdr = g_base; g_prefBase = base;
    for (int i = 0; i < nsec; i++) {
        uint8_t *sh = file + pe + 24 + opt + 40 * i;
        uint32_t vs = rd32(sh + 8), va = rd32(sh + 12), rs = rd32(sh + 16), raw = rd32(sh + 20);
        memcpy(g_base + va, file + raw, rs < vs ? rs : vs);
        Sec s = { g_base + va, vs };
        if (!strncmp((char *)sh, ".text", 8)) g_text = s; else if (!strncmp((char *)sh, ".rdata", 8)) g_rdata = s; else if (!strncmp((char *)sh, ".data", 8)) g_data = s;
    }
    g_timestamp = rd32(file + pe + 8);
    free(file);
    return 1;
}

static void print_found(void)
{
    printf("  vtables: tree %llX icon %llX\n", EXE_OFF(V_TREE), EXE_OFF(V_ICON));
    printf("  dialog: parent %X children %X name %X | tree units %X | icon node %X state %X | node prereqs +%X\n",
           O_DLG_PARENT, O_DLG_CHILD, O_DLG_NAME, O_TREE_UNITS, O_ICON_NODE, O_ICON_STATE, O_VAL_PREREQ);
    if (g_okIcon)
        printf("  fns: Tag(str) %llX ConfigLookup %llX ~Tag %llX GetUnitName %llX Localize %llX LocText %llX assign %llX delete %llX | prefix \"%s\"\n",
               EXE_OFF(g_tagFromStr), EXE_OFF(g_configLookup), EXE_OFF(g_tagDtor), EXE_OFF(g_unitName), EXE_OFF(g_locStr),
               EXE_OFF(g_locText), EXE_OFF(g_assign), EXE_OFF(g_delete), g_locPrefix);
    if (g_okHover) printf("  hovered dialog global exe+%llX\n", EXE_OFF(g_hovered));
    if (g_okGlow) printf("  glow: FindChild %llX SetActive %llX\n", EXE_OFF(g_findChild), EXE_OFF(g_setActive));
    if (g_okLines) printf("  lines: tree linesContainer %X | line icons %X %X, FX holder %X | vt line %llX animator %llX, color +%X | sprite animator +%X, quad built +%X\n",
                          O_TREE_LINES, O_LINE_A, O_LINE_B, O_LINE_FX, EXE_OFF(V_LINE), EXE_OFF(V_ANIM), O_ANIM_COLOR, O_SPR_ANIM, O_SPR_BUILT);
    if (g_okLineTex) printf("  line texture: \"%s\" (%zu) | PathCtor %llX Normalize %llX PathDtor %llX Sprite::Load %llX (flags %X) StateBits %llX (%X) SetState %llX Finalize %llX | material +%X\n",
                            g_lineTexVanilla, g_lineTexVanillaLen, EXE_OFF(g_pathCtor), EXE_OFF(g_pathNorm), EXE_OFF(g_pathDtor), EXE_OFF(g_spriteLoad), g_loadFlags,
                            EXE_OFF(g_stateBits), g_matState, EXE_OFF(g_setState), EXE_OFF(g_spriteFinalize), O_SPR_MAT);
}

static int finder_test(const char *path)
{
    if (!map_exe(path)) return 1;
    printf("%s timestamp %08X\n", path, g_timestamp);
    g_exeFile = fopen(path, "rb");
    if (!find_all()) { printf("  no research tree classes (expected for the editor / server)\n  INERT\n"); fclose(g_exeFile); return 0; }
    print_found();
    int ok = g_okDialog && g_okTree && g_okIcon && g_okHover && g_okLines && g_okLineTex && g_okGlow;
    if (!ok) { printf("  FAILED\n"); fclose(g_exeFile); return 1; }
    /* the same with slots swapped by "another DLL": a hook lives outside .text */
    uint32_t keep[] = { O_DLG_PARENT, O_DLG_CHILD, O_DLG_NAME, O_TREE_UNITS, O_ICON_NODE, O_ICON_STATE, O_VAL_PREREQ,
                        O_TREE_LINES, O_LINE_A, O_LINE_B, O_LINE_FX, O_ANIM_COLOR, O_SPR_ANIM, O_SPR_BUILT };
    void *fakeHook = malloc(16);
    int slotsT[] = { SL_DTOR, SL_LAYOUTINIT, SL_CLOSE, SL_BIND }, slotsI[] = { SL_ICON_STATEMAP, SL_ICON_HINT };
    for (size_t i = 0; i < 4; i++) V_TREE[slotsT[i]] = fakeHook;
    for (size_t i = 0; i < 2; i++) V_ICON[slotsI[i]] = fakeHook;
    g_quiet = 1; g_slotsFromDisk = 0;
    find_all();
    g_quiet = 0;
    uint32_t again[] = { O_DLG_PARENT, O_DLG_CHILD, O_DLG_NAME, O_TREE_UNITS, O_ICON_NODE, O_ICON_STATE, O_VAL_PREREQ,
                         O_TREE_LINES, O_LINE_A, O_LINE_B, O_LINE_FX, O_ANIM_COLOR, O_SPR_ANIM, O_SPR_BUILT };
    int same = !memcmp(keep, again, sizeof keep) && g_okDialog && g_okTree && g_okIcon && g_okHover && g_okLines && g_okLineTex && g_okGlow;
    printf("  with 6 slots pre-hooked by another DLL: %d slot reads from disk, %s\n", g_slotsFromDisk, same ? "same result" : "DIFFERENT");
    fclose(g_exeFile); g_exeFile = NULL;
    if (!same || g_slotsFromDisk < 6) { printf("  FAILED\n"); return 1; }
    printf("  ALL FOUND\n");
    return 0;
}

/* ------------------------------ logic test ------------------------------ */
static void *fake_vt_icon[100], *fake_vt_other[100], *fake_vt_tree[100], *fake_vt_line[100], *fake_vt_anim[8];
static int dirty_calls;
static void fake_mark_dirty(void *self) { (void)self; dirty_calls++; }

enum { F_PARENT = 0xF0, F_CHILD = 0xD8, F_NAME = 0x58, F_UNITS = 0x230, F_NODE = 0x2E0, F_STATE = 0x2E8, F_PREREQ = 0x20,
       F_LINES = 0x220, F_LA = 0x1A0, F_LB = 0x1A8, F_FX = 0x148, F_COLOR = 0x60, F_SPR_ANIM = 0xF0, F_SPR_BUILT = 0x14 };

/* a research line: FX holder with entry {id 7 (other), id 0 -> animator} */
static uint8_t *mk_line(uint8_t *a, uint8_t *b)
{
    uint8_t *l = calloc(1, 0x200), *entries = calloc(2, FX_STRIDE), *anim = calloc(1, 0x68), *spr = calloc(1, 0x110);
    *(void ***)l = fake_vt_line; *(void ***)anim = fake_vt_anim;
    U32(anim, F_COLOR) = 0xFFFFFFFF;
    PTR(spr, F_SPR_ANIM) = anim; U8(spr, F_SPR_BUILT) = 1;                 /* quad already built */
    PTR(spr, 0x48) = spr; strcpy((char *)spr + 0x80, "/interface/pages/main/dynamic_campaign/progress_line");
    U32(entries, FX_ID) = 7; U32(entries + FX_STRIDE, FX_ID) = 0; PTR(entries + FX_STRIDE, FX_ANIM) = anim;
    PTR(entries + FX_STRIDE, FX_SPRITE) = spr;
    PTR(l, F_FX + FX_VEC) = entries; PTR(l, F_FX + FX_VEC + 8) = entries + 2 * FX_STRIDE;
    PTR(l, F_LA) = a; PTR(l, F_LB) = b;
    return l;
}
static uint8_t *anim_of(uint8_t *l) { return PTR(PTR(l, F_FX + FX_VEC) + FX_STRIDE, FX_ANIM); }
#define SELCOLOR 0xFFFFFFFE      /* the test's line_color (white would equal the vanilla color) */
static uint32_t color_of(uint8_t *l) { return U32(anim_of(l), F_COLOR); }
static uint8_t *sprite_of(uint8_t *l) { return PTR(PTR(l, F_FX + FX_VEC) + FX_STRIDE, FX_SPRITE); }
/* fake texture load: the sprite remembers the path at +0x80 */
enum { F_SPR_MAT = 0x48, F_SPR_TEX = 0x80 };
static int set_state_calls;
static void fk_path_ctor(void *p) { memset(p, 0, 0x28); ((MsString *)p)->cap = 15; }
static void fk_noop(void *p) { (void)p; }
static void fk_sprite_load(void *sp, void *path, int flags, int extra)
{
    (void)extra; assert(flags == 0x11);
    const MsString *s = path; snprintf((char *)sp + F_SPR_TEX, 0x60, "%.*s", (int)s->size, ms_data(s));
}
static int fk_state_bits(int st) { assert(st == 0x40000); return 3; }
static void fk_set_state(void *mat, int bits) { (void)mat; assert(bits == 3); set_state_calls++; }
static void fk_finalize(void *sp) { U8(sp, F_SPR_BUILT) = 0; }
static const char *tex_of(uint8_t *l) { return (const char *)sprite_of(l) + F_SPR_TEX; }
/* fake glow: an icon's glow child pointer at +0x300; SetActive writes the state into byte 0x10 of the glow */
enum { F_GLOW = 0x300 };
static uint8_t *fk_find_child(void *dlg, const MsString *name) { assert(!strcmp(ms_data(name), "rp_glow")); return PTR(dlg, F_GLOW); }
static void fk_set_active(void *dlg, int on) { U8(dlg, 0x10) = (uint8_t)on; }
static int glow_on(uint8_t *icon) { uint8_t *g = PTR(icon, F_GLOW); return g && U8(g, 0x10); }

static void ms_set(MsString *s, const char *c)      /* engine-style string owned by the test (malloc) */
{
    size_t n = strlen(c);
    memset(s, 0, sizeof *s);
    if (n < 16) { memcpy(s->u.buf, c, n + 1); s->cap = 15; }
    else { char *p = malloc(n + 1); memcpy(p, c, n + 1); s->u.ptr = p; s->cap = n; }
    s->size = n;
}

typedef struct { uint8_t *icon, *node; MsString pre[8]; } FakeIcon;

static uint8_t *mk_icon(FakeIcon *f, uint8_t *units, const char *key, int state, int unit, const char **pre, int npre)
{
    f->icon = calloc(1, 0x400); f->node = calloc(1, 0x100);
    *(void ***)f->icon = fake_vt_icon;
    ms_set((MsString *)(f->icon + F_NAME), key);
    PTR(f->icon, F_PARENT) = units; U32(f->icon, F_STATE) = (uint32_t)state; PTR(f->icon, F_NODE) = f->node;
    f->node[0] = unit ? 0 : 1;
    if (strcmp(key, "u_y")) PTR(f->icon, F_GLOW) = calloc(1, 0x40);       /* u_y: no glow widget */
    for (int i = 0; i < npre; i++) ms_set(&f->pre[i], pre[i]);
    PTR(f->node, F_PREREQ) = (uint8_t *)f->pre; PTR(f->node, F_PREREQ + 8) = (uint8_t *)(f->pre + npre);
    return f->icon;
}

/* fake engine functions */
static void fk_tag(void *tag, const MsString *s) { memcpy(tag, ms_data(s), s->size); ((char *)tag)[s->size] = 0; }
static void fk_tagdtor(void *tag) { (void)tag; }
static void *fk_lookup(const void *tag) { return ((const char *)tag)[0] == 'u' ? (void *)tag : NULL; }
static int live_engine_strings;
static MsString *fk_unitname(void *tpl, MsString *out, int flag)
{
    (void)flag; char b[128]; snprintf(b, sizeof b, "Unit %s with a long display name", (const char *)tpl);
    ms_set(out, b); live_engine_strings++; return out;
}
static void fk_delete(void *p, size_t n) { (void)n; free(p); live_engine_strings--; }
static MsString loc_out;
static void *fk_locstr(const MsString *k) { char b[256]; snprintf(b, sizeof b, "Loc[%.*s]", (int)k->size, ms_data(k)); ms_set(&loc_out, b); return &loc_out; }
static const MsString *fk_loctext(void *e) { return (const MsString *)e; }
static void fk_assign(MsString *s, const char *p, size_t n) { char *b = malloc(n + 1); memcpy(b, p, n); b[n] = 0; s->u.ptr = b; s->size = n; s->cap = n < 16 ? 16 : n; }
static void *fk_draw(void *a, void *b, void *c, void *d) { (void)a; (void)b; (void)c; (void)d; return NULL; }
static void *fk_frames(void *icon, void *out, void *c, void *d) { (void)icon; (void)c; (void)d; uint32_t *o = out; o[0] = 4; o[1] = 1; o[2] = 2; o[3] = 0; return (void *)1; }
static void *fk_hint(void *icon, void *out, void *c, void *d)
{
    (void)c; (void)d;
    if (U32(icon, F_STATE) != ST_LOCKED) return (void *)0x1234500;     /* al = 0 with garbage above */
    ms_set(out, "Research previous unit to unlock"); return (void *)1;
}

static int logic_test(void)
{
    O_DLG_PARENT = F_PARENT; O_DLG_CHILD = F_CHILD; O_DLG_NAME = F_NAME; O_TREE_UNITS = F_UNITS;
    O_ICON_NODE = F_NODE; O_ICON_STATE = F_STATE; O_VAL_PREREQ = F_PREREQ;
    V_ICON = fake_vt_icon; V_TREE = fake_vt_tree; V_LINE = fake_vt_line; V_ANIM = fake_vt_anim;
    O_TREE_LINES = F_LINES; O_LINE_A = F_LA; O_LINE_B = F_LB; O_LINE_FX = F_FX; O_ANIM_COLOR = F_COLOR;
    O_SPR_ANIM = F_SPR_ANIM; O_SPR_BUILT = F_SPR_BUILT; O_SPR_MAT = F_SPR_MAT;
    g_okLineTex = 1; g_useLineTex = 1; g_loadFlags = 0x11; g_matState = 0x40000;
    g_pathCtor = fk_path_ctor; g_pathNorm = fk_noop; g_pathDtor = fk_noop; g_spriteLoad = fk_sprite_load;
    g_stateBits = fk_state_bits; g_setState = fk_set_state; g_spriteFinalize = fk_finalize;
    strcpy(g_lineTexVanilla, "/interface/pages/main/dynamic_campaign/progress_line"); g_lineTexVanillaLen = strlen(g_lineTexVanilla);
    strcpy(g_lineTexSel, "/interface/pages/main/dynamic_campaign/progress_line_selected"); g_lineTexSelLen = strlen(g_lineTexSel);
    g_lineColor = SELCOLOR;
    g_okGlow = 1; g_findChild = fk_find_child; g_setActive = fk_set_active;
    g_okLines = 1;
    fake_vt_icon[SL_MARKDIRTY] = (void *)fake_mark_dirty;
    fake_vt_line[SL_MARKDIRTY] = (void *)fake_mark_dirty;
    g_tagFromStr = fk_tag; g_tagDtor = fk_tagdtor; g_configLookup = fk_lookup; g_unitName = fk_unitname; g_delete = fk_delete;
    g_locStr = fk_locstr; g_locText = fk_loctext; g_assign = fk_assign;
    strcpy(g_locPrefix, "main_menu2/dynamic_campaign/"); g_locPrefixLen = strlen(g_locPrefix);
    o_treeDraw = fk_draw; o_iconFrames = fk_frames; o_iconHint = fk_hint;

    uint8_t *tree = calloc(1, 0x400), *units = calloc(1, 0x400);
    *(void ***)tree = fake_vt_tree; *(void ***)units = fake_vt_other;
    PTR(tree, F_UNITS) = units;
    static FakeIcon fi[16]; int k = 0;
    const char *p_root[] = { 0 }, *p_a[] = { "u_root" }, *p_b[] = { "u_a" }, *p_c[] = { "u_b", "u_x" }, *p_x[] = { "u_root" },
               *p_d[] = { "u_c" }, *p_y[] = { "u_nowhere" }, *p_up[] = { "u_a" };
    uint8_t *ic[16];
    ic[k] = mk_icon(&fi[k], units, "u_root", ST_RESEARCHED, 1, p_root, 0); k++;
    ic[k] = mk_icon(&fi[k], units, "u_a", ST_RESEARCHABLE, 1, p_a, 1); k++;
    ic[k] = mk_icon(&fi[k], units, "u_b", ST_LOCKED, 1, p_b, 1); k++;
    ic[k] = mk_icon(&fi[k], units, "u_c", ST_LOCKED, 1, p_c, 2); k++;
    ic[k] = mk_icon(&fi[k], units, "u_x", ST_RESEARCHABLE, 1, p_x, 1); k++;
    ic[k] = mk_icon(&fi[k], units, "u_d", ST_LOCKED, 1, p_d, 1); k++;
    ic[k] = mk_icon(&fi[k], units, "u_y", ST_LOCKED, 1, p_y, 1); k++;
    ic[k] = mk_icon(&fi[k], units, "upgrade_armour_with_a_long_key", ST_LOCKED, 0, p_up, 1); k++;
    uint8_t *other = calloc(1, 0x400); *(void ***)other = fake_vt_other;   /* a non-icon child of the container */
    uint8_t **kids = calloc(k + 1, sizeof *kids);
    for (int i = 0; i < k; i++) kids[i] = ic[i];
    kids[k] = other;
    PTR(units, F_CHILD) = (uint8_t *)kids; PTR(units, F_CHILD + 8) = (uint8_t *)(kids + k + 1);
    /* lines as LayoutInit makes them: (node icon, prerequisite icon) for each prerequisite that has an icon */
    uint8_t *linesC = calloc(1, 0x400); *(void ***)linesC = fake_vt_other; PTR(tree, F_LINES) = linesC;
    enum { L_A_ROOT, L_B_A, L_C_B, L_C_X, L_X_ROOT, L_D_C, L_UP_A, NL };
    uint8_t *ln[NL];
    ln[L_A_ROOT] = mk_line(ic[1], ic[0]); ln[L_B_A] = mk_line(ic[2], ic[1]); ln[L_C_B] = mk_line(ic[3], ic[2]);
    ln[L_C_X] = mk_line(ic[3], ic[4]); ln[L_X_ROOT] = mk_line(ic[4], ic[0]); ln[L_D_C] = mk_line(ic[5], ic[3]);
    ln[L_UP_A] = mk_line(ic[7], ic[1]);
    uint8_t **lk = calloc(NL, sizeof *lk); for (int i = 0; i < NL; i++) lk[i] = ln[i];
    PTR(linesC, F_CHILD) = (uint8_t *)lk; PTR(linesC, F_CHILD + 8) = (uint8_t *)(lk + NL);

    tree_bind(tree);
    assert(g_b.nl == NL);
    assert(g_b.n == k && g_b.tree == tree);
    for (int i = 0; i < k; i++) assert(ent_of_icon(ic[i]) && ent_of_icon(ic[i])->icon == ic[i]);
    assert(!ent_of_icon(other));

    /* path of u_d: deepest first, the hovered one last */
    Ent *chain[16]; int n = missing_chain(ent_of_icon(ic[5]), 1, chain, 16);
    const char *want[] = { "u_a", "u_b", "u_x", "u_c", "u_d" };
    assert(n == 5);
    for (int i = 0; i < n; i++) { if (strcmp(chain[i]->key, want[i])) { printf("chain[%d] = %s, want %s\n", i, chain[i]->key, want[i]); return 1; } }
    assert(missing_chain(ent_of_icon(ic[1]), 0, chain, 16) == 0);                 /* researchable: nothing missing */
    assert(missing_chain(ent_of_icon(ic[6]), 0, chain, 16) == 0);                 /* prerequisite without an icon */
    printf("chain OK (u_a, u_b, u_x, u_c, u_d)\n");

    /* hover: the GUI hovers a child of the icon */
    uint8_t *hoveredRef = NULL; g_hovered = &hoveredRef;
    uint8_t *btn = calloc(1, 0x400); *(void ***)btn = fake_vt_other; PTR(btn, F_PARENT) = ic[5];
    hoveredRef = btn; dirty_calls = 0;
    hk_tree_draw(tree, 0, 0, 0);
    int hl = 0; for (int i = 0; i < g_b.n; i++) hl += g_b.e[i].hl;
    assert(hl == 5 && dirty_calls == 5 + 6);                                       /* 5 icons + 6 lines */
    for (int i = 0; i < k; i++) assert(glow_on(ic[i]) == ent_of_icon(ic[i])->hl);   /* glow = on the path */
    assert(glow_on(ic[5]) && glow_on(ic[3]) && !glow_on(ic[0]));
    /* path of u_d: d-c, c-b, c-x, b-a + to the researched root: a-root, x-root; not the upgrade's line */
    for (int i = 0; i < NL; i++) assert((color_of(ln[i]) == SELCOLOR) == (i != L_UP_A));
    for (int i = 0; i < NL; i++) assert((U8(sprite_of(ln[i]), F_SPR_BUILT) == 0) == (i != L_UP_A));   /* quad rebuilt */
    for (int i = 0; i < NL; i++) assert(!strcmp(tex_of(ln[i]), i != L_UP_A ? g_lineTexSel : g_lineTexVanilla));
    assert(set_state_calls == 6);
    for (int i = 0; i < NL; i++) U8(sprite_of(ln[i]), F_SPR_BUILT) = 1;
    /* ConquestUI's zoom rebuilds a line as a vanilla one (new sprite + animator): switched again on the next frame */
    uint8_t *e1 = PTR(ln[L_C_B], F_FX + FX_VEC) + FX_STRIDE, *fresh = calloc(1, 0x68);
    *(void ***)fresh = fake_vt_anim; U32(fresh, F_COLOR) = 0xFFFFFFFF; PTR(e1, FX_ANIM) = fresh;
    uint8_t *fspr = calloc(1, 0x110); PTR(fspr, F_SPR_ANIM) = fresh; U8(fspr, F_SPR_BUILT) = 1; PTR(fspr, 0x48) = fspr;
    strcpy((char *)fspr + 0x80, g_lineTexVanilla); PTR(e1, FX_SPRITE) = fspr;
    hk_tree_draw(tree, 0, 0, 0);
    assert(color_of(ln[L_C_B]) == SELCOLOR && !strcmp(tex_of(ln[L_C_B]), g_lineTexSel));
    g_lineToResearched = 0;                                                        /* re-evaluated at the next hover change */
    uint32_t fr[4];
    hk_icon_frames(ic[3], fr, 0, 0); assert(fr[0] == 1);                           /* u_c on the path: hover frame */
    hk_icon_frames(ic[0], fr, 0, 0); assert(fr[0] == 4);                           /* researched root: untouched */
    dirty_calls = 0; hk_tree_draw(tree, 0, 0, 0); assert(dirty_calls == 0);        /* same hover: nothing to do */
    hoveredRef = ic[2];                                                            /* u_b: path u_a, u_b */
    hk_tree_draw(tree, 0, 0, 0); assert(dirty_calls == 3 + 5);                     /* u_x, u_c, u_d cleared; 5 lines restored (b-a stays) */
    for (int i = 0; i < NL; i++) assert((color_of(ln[i]) == SELCOLOR) == (i == L_B_A));
    for (int i = 0; i < NL; i++) assert(!strcmp(tex_of(ln[i]), i == L_B_A ? g_lineTexSel : g_lineTexVanilla));
    g_lineToResearched = 1;
    hoveredRef = units; dirty_calls = 0;
    hk_tree_draw(tree, 0, 0, 0); assert(dirty_calls == 2 + 1);                     /* off the icons: all cleared */
    hl = 0; for (int i = 0; i < g_b.n; i++) hl += g_b.e[i].hl; assert(!hl);
    for (int i = 0; i < k; i++) assert(!glow_on(ic[i]));
    for (int i = 0; i < NL; i++) assert(color_of(ln[i]) == 0xFFFFFFFF && (i == L_UP_A || U8(sprite_of(ln[i]), F_SPR_BUILT) == 0));
    for (int i = 0; i < NL; i++) assert(!strcmp(tex_of(ln[i]), g_lineTexVanilla));
    hoveredRef = ic[0]; hk_tree_draw(tree, 0, 0, 0);                               /* researched unit: nothing */
    hl = 0; for (int i = 0; i < g_b.n; i++) hl += g_b.e[i].hl; assert(!hl);
    printf("lines OK\n");
    printf("highlight OK\n");

    /* tooltip */
    MsString s;
    memset(&s, 0, sizeof s); hk_icon_hint(ic[5], &s, 0, 0);
    printf("tooltip u_d: [%s]\n", ms_data(&s));
    assert(!strcmp(ms_data(&s), "Research previous unit to unlock\nRequires: Unit u_a with a long display name, Unit u_b with a long display name, "
                                "Unit u_x with a long display name, Unit u_c with a long display name"));
    assert(live_engine_strings == 0);                                              /* every GetUnitName result freed */
    g_maxNames = 2; free(g_b.e[0].name); g_b.e[0].name = NULL;
    memset(&s, 0, sizeof s); hk_icon_hint(ic[5], &s, 0, 0);
    printf("tooltip u_d, max 2: [%s]\n", ms_data(&s));
    assert(strstr(ms_data(&s), ", +2 more"));
    g_maxNames = 10;
    Ent *ex = ent_of_icon(ic[4]); free(ex->name); ex->name = dup_n("Unit u_b with a long display name", 33);   /* same name as u_b */
    memset(&s, 0, sizeof s); hk_icon_hint(ic[5], &s, 0, 0);
    printf("tooltip u_d, duplicate name: [%s]\n", ms_data(&s));
    assert(!strcmp(strstr(ms_data(&s), "Requires: "), "Requires: Unit u_a with a long display name, Unit u_b with a long display name, Unit u_c with a long display name"));
    memset(&s, 0, sizeof s); hk_icon_hint(ic[7], &s, 0, 0);                        /* non-unit node, its prereq is a unit */
    printf("tooltip upgrade: [%s]\n", ms_data(&s));
    assert(strstr(ms_data(&s), "Requires: Unit u_a"));
    assert(!strcmp(ent_name(ent_of_icon(ic[7])), "Loc[main_menu2/dynamic_campaign/upgrade_armour_with_a_long_key]"));
    memset(&s, 0, sizeof s); void *r = hk_icon_hint(ic[1], &s, 0, 0);              /* researchable: untouched, al = 0 */
    assert(!al_true(r) && s.size == 0);
    memset(&s, 0, sizeof s); hk_icon_hint(ic[6], &s, 0, 0);                        /* only an icon-less prerequisite */
    assert(!strcmp(ms_data(&s), "Research previous unit to unlock"));
    printf("tooltip OK\n");

    hk_tree_close(tree, 0, 0, 0);
    return 0;
}

static void *fk_close(void *a, void *b, void *c, void *d) { (void)a; (void)b; (void)c; (void)d; return NULL; }

int main(int argc, char **argv)
{
    if (argc < 2) { printf("usage: rp_test <exe>... | --logic\n"); return 2; }
    if (!strcmp(argv[1], "--logic")) {
        o_treeClose = fk_close;
        int r = logic_test();
        printf(r ? "LOGIC FAILED\n" : "LOGIC OK\n");
        return r;
    }
    int fail = 0;
    for (int i = 1; i < argc; i++) fail |= finder_test(argv[i]);
    return fail;
}
