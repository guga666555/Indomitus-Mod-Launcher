/* GEM Fauna - offline test of the over-head icon rule (fx_marker_name in src/fx/headicon.c)
 * Builds the real headicon.c and fauna.c (is_fauna, def_props) against fake entities / defs / props and a fake engine
 * marker-name builder, then checks which marker names are replaced.
 *   default        expectations of 0.12.2 and later: any alive non-human with an icon_ prop is replaced; the prop is
 *                  read on every call (0.12.3), so a def whose props change is followed at once
 *   -DEXPECT_0121  expectations of 0.12.1: only fauna are replaced (run against the untouched 0.12.1 source)
 * build + run: tests/run_offline_check_win.py (zig cc, Windows target), or any gcc/clang:
 *   cc -I src -o out/headicon_gate_test tests/headicon_gate_test.c && out/headicon_gate_test
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#ifndef FX_HEADICON_SRC
#define FX_HEADICON_SRC "src/fx/headicon.c"
#endif

/* ---- the engine bindings headicon.c / fauna.c use (same layout as src/fx/engine.c) */
static void logf_(const char *fmt, ...) { va_list ap; va_start(ap, fmt); printf("    log: "); vprintf(fmt, ap); va_end(ap); printf("\n"); }
static uint8_t *g_base;
#define EXE_OFF(p) ((unsigned long long)((const uint8_t *)(p) - g_base))
static uint32_t g_entDefOff = 0x548;
#define ENT_FLAGS 0x30
typedef struct { union { char buf[16]; char *ptr; } u; size_t size, cap; } MsString;
static const char *ms_cstr(const MsString *s) { return s->cap > 15 ? s->u.ptr : s->u.buf; }
#define TAG_SIZE 0x60
#define TAG_NAME 0x40
static uint32_t g_propBegOff = 0x330, g_propEndOff = 0x338;
static uint32_t g_actorOff, g_vitOff; static int g_okVit;
static void **g_vtVitality;
/* engine std::string assign */
static void *fake_assign(void *str, const char *p, size_t n)
{
    MsString *s = str;
    if (s->cap > 15) free(s->u.ptr);
    if (n > 15) { s->u.ptr = malloc(n + 1); memcpy(s->u.ptr, p, n); s->u.ptr[n] = 0; s->cap = n; }
    else { memcpy(s->u.buf, p, n); s->u.buf[n] = 0; s->cap = 15; }
    s->size = n;
    return str;
}
typedef void *(*StrAssignFn)(void *str, const char *p, size_t n);
static StrAssignFn g_strAssign = fake_assign;
/* finder / patch helpers headicon.c references (find_head_icon / install_head_icon are not run here) */
static uint8_t *find_cstr(const char *s) { (void)s; return NULL; }
static uint8_t *find_lea_to(const void *w, int k) { (void)w; (void)k; return NULL; }
static uint32_t func_of(const void *a) { (void)a; return 0; }
static int prologue_len(const uint8_t *f) { (void)f; return 0; }
static int install_jmp5(uint8_t *f, void *h, void **o, const char *w) { (void)f; (void)h; (void)o; (void)w; return 0; }

#include "fx/fauna.c"
#include "fx/headicon.c"

/* ---- fakes */
typedef struct { uint8_t b[0x600]; } FakeDef;
typedef struct { uint8_t b[0x600]; } FakeEnt;
static uint8_t *make_tag(uint64_t id, const char *name)
{
    uint8_t *t = calloc(1, TAG_SIZE);
    *(uint64_t *)t = id;
    fake_assign(t + TAG_NAME, name, strlen(name));
    return t;
}
static FakeDef *make_def(const char **props, int n)
{
    FakeDef *d = calloc(1, sizeof *d);
    uint8_t *v = calloc(n ? n : 1, TAG_SIZE);
    for (int i = 0; i < n; i++) {
        uint8_t *t = make_tag(0x1000 + (uint64_t)i + (strcmp(props[i], "fauna_main") ? 0 : 0xFA00000000ull), props[i]);
        memcpy(v + i * TAG_SIZE, t, TAG_SIZE);
    }
    *(uint8_t **)(d->b + g_propBegOff) = v; *(uint8_t **)(d->b + g_propEndOff) = v + n * TAG_SIZE;
    return d;
}
static FakeEnt *make_ent(FakeDef *d, uint8_t flags)
{
    FakeEnt *e = calloc(1, sizeof *e);
    e->b[ENT_FLAGS] = flags;
    *(FakeDef **)(e->b + g_entDefOff) = d;
    return e;
}
/* the engine builder: "/interface/scene/squad/" + BASE + "(" + faction + ")"; spectator -> fixed names */
static const char *g_fakeBase = "icon_tank", *g_fakeFaction = "sm";
static int g_builderCalls;
static void *fake_builder(void *out, void *entity, uint8_t spectator)
{
    (void)entity; g_builderCalls++;
    char b[256];
    if (spectator) snprintf(b, sizeof b, "/interface/scene/squad/spectator_enemy");
    else snprintf(b, sizeof b, "/interface/scene/squad/%s(%s)", g_fakeBase, g_fakeFaction);
    fake_assign(out, b, strlen(b));
    return out;
}

static int g_fail, g_n;
static void check(const char *what, FakeEnt *e, uint8_t spectator, const char *base, const char *want)
{
    MsString s; memset(&s, 0, sizeof s); s.cap = 15;
    g_fakeBase = base;
    fx_marker_name(&s, e ? e->b : NULL, spectator);
    int ok = !strcmp(ms_cstr(&s), want);
    g_n++; if (!ok) g_fail++;
    printf("  %-4s %-52s -> %s%s%s\n", ok ? "OK" : "FAIL", what, ms_cstr(&s), ok ? "" : "   WANTED ", ok ? "" : want);
}

int main(void)
{
    g_markerNameOrig = fake_builder;
    g_okFauna = 1;
    *(uint64_t *)g_tagFaunaBuf = 0xFA00000000ull + 0x1000;      /* id of "fauna_main" at index 0 of a def */
    const char *pFaunaIcon[] = { "fauna_main", "fauna_size05", "icon_carnifex" };
    const char *pFaunaNone[] = { "fauna_main", "fauna_size02" };
    const char *pTankIcon[]  = { "tank", "board_astartes", "icon_sm_cannon", "squad_icon_cannon" };
    const char *pTankNone[]  = { "tank", "board_astartes", "heavy" };
    const char *pHumanIcon[] = { "icon_should_not_matter" };
    const char *pIconOnly[]  = { "icon_" };                      /* prefix alone: not a name */
    const char *pCannonIcon[] = { "cannon", "icon_my_gun" };
    FakeDef *dFI = make_def(pFaunaIcon, 3), *dFN = make_def(pFaunaNone, 2), *dTI = make_def(pTankIcon, 4),
            *dTN = make_def(pTankNone, 3), *dHI = make_def(pHumanIcon, 1), *dIO = make_def(pIconOnly, 1),
            *dCI = make_def(pCannonIcon, 2), *dNone = make_def(NULL, 0);
    /* flags: 2 alive / in scene, 4 human, 8 vehicle class */
    FakeEnt *fauna = make_ent(dFI, 2 | 8), *faunaNo = make_ent(dFN, 2 | 8), *tank = make_ent(dTI, 2 | 8),
            *tank2 = make_ent(dTI, 2 | 8), *tankNo = make_ent(dTN, 2 | 8), *human = make_ent(dHI, 2 | 4),
            *iconOnly = make_ent(dIO, 2 | 8), *cannon = make_ent(dCI, 2 | 8), *noProps = make_ent(dNone, 2 | 8),
            *deadTank = make_ent(dTI, 8), *noDef = make_ent(NULL, 2 | 8);
#ifdef EXPECT_0121
    const char *veh = "0.12.1 (fauna only)";
    const char *tankWant = "/interface/scene/squad/icon_tank(sm)", *cannonWant = "/interface/scene/squad/icon_cannon(sm)";
    const char *editWant1 = tankWant, *editWant2 = tankWant;
#else
    const char *veh = "0.12.2 and later (any non-human)";
    const char *tankWant = "/interface/scene/squad/icon_sm_cannon", *cannonWant = "/interface/scene/squad/icon_my_gun";
    const char *editWant1 = "/interface/scene/squad/icon_first_name", *editWant2 = "/interface/scene/squad/icon_second";
#endif
    printf("headicon gate test, expectations of %s, source " FX_HEADICON_SRC "\n", veh);
    check("fauna + icon_carnifex", fauna, 0, "icon_vehicle", "/interface/scene/squad/icon_carnifex");
    check("fauna, no icon_ prop", faunaNo, 0, "icon_vehicle", "/interface/scene/squad/icon_vehicle(sm)");
    check("tank + icon_sm_cannon", tank, 0, "icon_tank", tankWant);
    check("tank + icon_sm_cannon (second unit, same def)", tank2, 0, "icon_tank", tankWant);
    check("tank + icon_sm_cannon again", tank, 0, "icon_tank", tankWant);
    check("cannon + icon_my_gun", cannon, 0, "icon_cannon", cannonWant);
    check("tank, no icon_ prop", tankNo, 0, "icon_tank", "/interface/scene/squad/icon_tank(sm)");
    check("tank, no icon_ prop again", tankNo, 0, "icon_tank", "/interface/scene/squad/icon_tank(sm)");
    check("vehicle with no props at all", noProps, 0, "icon_vehicle", "/interface/scene/squad/icon_vehicle(sm)");
    check("prop \"icon_\" alone (no name)", iconOnly, 0, "icon_vehicle", "/interface/scene/squad/icon_vehicle(sm)");
    check("human whose def has an icon_ prop", human, 0, "icon_stub", "/interface/scene/squad/icon_stub(sm)");
    check("tank + icon_ prop, not alive / in scene (flag 2 off)", deadTank, 0, "icon_tank", "/interface/scene/squad/icon_tank(sm)");
    check("vehicle without a def", noDef, 0, "icon_vehicle", "/interface/scene/squad/icon_vehicle(sm)");
    check("tank + icon_ prop, spectator name", tank, 1, "icon_tank", "/interface/scene/squad/spectator_enemy");
    check("fauna + icon_ prop, spectator name", fauna, 1, "icon_vehicle", "/interface/scene/squad/spectator_enemy");
    check("no entity", NULL, 0, "icon_stub", "/interface/scene/squad/icon_stub(sm)");
    /* a def whose props change after it was first read (a reload): the answer follows the def at once (0.12.3; the
       0.12.2 local build answered from its per-def copy and failed the second and third of these) */
    {
        const char *pEdit[] = { "tank", "icon_first_name" };
        FakeDef *dE = make_def(pEdit, 2); FakeEnt *eE = make_ent(dE, 2 | 8);
        uint8_t *v = *(uint8_t **)(dE->b + g_propBegOff);
        check("def read for the first time: icon_first_name", eE, 0, "icon_tank", editWant1);
        fake_assign(v + 1 * TAG_SIZE + TAG_NAME, "icon_second", 11);
        check("same def, icon_ prop renamed", eE, 0, "icon_tank", editWant2);
        fake_assign(v + 1 * TAG_SIZE + TAG_NAME, "heavy", 5);
        check("same def, icon_ prop removed", eE, 0, "icon_tank", "/interface/scene/squad/icon_tank(sm)");
    }
    /* two defs on one slot of is_fauna's per-def table ((def >> 4) % 64): the answer follows the def, not the slot */
    FakeDef *a = NULL, *b = NULL;
    for (int i = 0; i < 4000 && !b; i++) {
        FakeDef *d = make_def(i & 1 ? pTankNone : pTankIcon, i & 1 ? 3 : 4);
        if (!(i & 1) && !a) { a = d; continue; }
        if (a && (i & 1) && (((uintptr_t)d >> 4) % 64) == (((uintptr_t)a >> 4) % 64)) b = d;
    }
    if (a && b) {
        FakeEnt *ea = make_ent(a, 2 | 8), *eb = make_ent(b, 2 | 8);
        check("slot collision: def A (icon_)", ea, 0, "icon_tank", tankWant);
        check("slot collision: def B (none), same slot", eb, 0, "icon_tank", "/interface/scene/squad/icon_tank(sm)");
        check("slot collision: def A again", ea, 0, "icon_tank", tankWant);
    } else printf("  (no colliding def pair found: slot collision case skipped)\n");
    printf("%d checks, %d failed: %s\n", g_n, g_fail, g_fail ? "FAILED" : "ALL OK");
    return g_fail != 0;
}
