/* Host test of the {seen} runtime (sx/seen.c): the vanilla word loop with prop/tag words, the set interning, and the
   count / match logic, against fake readers, fake engine Tags and fake entities with the offsets the finder reports
   for game 1.067. Built and run by tests/run_offline_check.sh. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#define SX_TEST
#define SX_SEEN_STUB_TEST
typedef struct { int x; } CRITICAL_SECTION;
static void EnterCriticalSection(CRITICAL_SECTION *c) { (void)c; }
static void LeaveCriticalSection(CRITICAL_SECTION *c) { (void)c; }
#define MemoryBarrier() __sync_synchronize()
static int g_verbose = 0;
static void logf_(const char *fmt, ...) { va_list ap; va_start(ap, fmt); printf("  log: "); vprintf(fmt, ap); va_end(ap); printf("\n"); }
typedef struct { union { char buf[16]; char *ptr; } u; size_t size, cap; } MsString;
static const char *ms_cstr(const MsString *s) { return s->cap > 15 ? s->u.ptr : s->u.buf; }
typedef uint8_t (*RdPeekFn)(void *reader, int kind, int z);
typedef MsString *(*RdWordFn)(void *reader, int flag);
/* fake Tag: id = hash of the lower-cased name */
static uint64_t tag_id(const char *s) { uint64_t h = 1469598103934665603ull; for (; *s; s++) { int c = *s; if (c >= 'A' && c <= 'Z') c += 32; h = (h ^ (uint8_t)c) * 1099511628211ull; } return h; }
static int g_tagsAlive;
static void fake_tag_ctor(void *t, const char *s) { *(uint64_t *)t = tag_id(s); g_tagsAlive++; }
static void fake_tag_dtor(void *t) { (void)t; g_tagsAlive--; }
static void (*g_tagCtorCStr)(void *, const char *) = fake_tag_ctor;
static void (*g_tagDtor)(void *) = fake_tag_dtor;
#include "../src/sx/seen.c"

/* fake reader: tokens "w:<word>" or "n:<int>" */
typedef struct { const char **tok; int i; MsString cur; int baseCalls; } Reader;
static uint8_t r_peek(void *r_, int kind, int z) { Reader *r = r_; (void)z; const char *t = r->tok[r->i]; if (!t) return 0; return kind == 1 ? t[0] == 'w' : kind == 11 ? t[0] == 'n' : 0; }
static MsString *r_word(void *r_, int f) { Reader *r = r_; (void)f; const char *t = r->tok[r->i++] + 2; memset(&r->cur, 0, sizeof r->cur); r->cur.size = strlen(t); r->cur.cap = 15; strncpy(r->cur.u.buf, t, 15); return &r->cur; }
static int32_t r_int(void *r_) { Reader *r = r_; return atoi(r->tok[r->i++] + 2); }
static void base_parse(void *c, void *r) { (void)c; ((Reader *)r)->baseCalls++; }
static int g_origEvals;
static uint8_t orig_eval(void *c, void *e) { (void)c; (void)e; g_origEvals++; return 0x5A; }

/* fake world */
typedef struct { _Alignas(16) uint8_t b[0x60]; } Tag60;
typedef struct { uint8_t raw[0x600]; Tag60 props[4]; } Def;
typedef struct { uint8_t raw[0x1000]; Def def; Tag60 tags[4]; } Ent;
typedef struct { uint8_t raw[0x300]; void *recs[16]; } Sensor;
typedef struct { uint8_t pad[0x14]; uint32_t flags; void *ent; } Rec;
static uint64_t g_classTag[10];
static void ent_init(Ent *e, const char *const *props, const char *const *tags)
{
    memset(e, 0, sizeof *e);
    *(Def **)(e->raw + 0x548) = &e->def;
    int n = 0; for (; props && props[n]; n++) *(uint64_t *)e->def.props[n].b = tag_id(props[n]);
    *(Tag60 **)(e->def.raw + 0x330) = e->def.props; *(Tag60 **)(e->def.raw + 0x338) = e->def.props + n;
    n = 0; for (; tags && tags[n]; n++) *(uint64_t *)e->tags[n].b = tag_id(tags[n]);
    *(Tag60 **)(e->raw + 0x4E8) = e->tags; *(Tag60 **)(e->raw + 0x4F0) = e->tags + n;
}
static uint8_t cond[0x28];
static int parse_cond(const char **tok)
{
    Reader r = { tok, 0, { { { 0 } }, 0, 0 }, 0 };
    memset(cond, 0, sizeof cond); *(uint32_t *)(cond + 0x20) = 0x100; *(int32_t *)(cond + 0x24) = 1;
    seen_parse(cond, &r);
    return r.baseCalls == 1 && tok[r.i] == NULL;
}
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void)
{
    /* the values the finder reads from game 1.067 */
    static const struct { const char *n; int set; uint32_t v; } kw[] = { {"enemy",1,2},{"ally",1,1},{"tank",0,4},{"car",0,8},{"cannon",0,0x10},
        {"airborne",0,0x20},{"helicopter",0,0x200},{"human",0,0x40},{"vehicle",0,0x80},{"any",0,0x100} };
    for (int i = 0; i < 10; i++) { strcpy(g_seenKw[i].name, kw[i].n); g_seenKw[i].len = strlen(kw[i].n); g_seenKw[i].set = kw[i].set; g_seenKw[i].val = kw[i].v; }
    g_seenNKw = 10;
    static const char *cls[] = { "human","airborne","helicopter","cannon","car","tank","vehicle" };
    static const uint32_t bits[] = { 0x40, 0x20, 0x200, 0x10, 0x08, 0x04, 0x80 };
    for (int i = 0; i < 7; i++) { g_classTag[i] = tag_id(cls[i]); g_seenGrp[i].bit = bits[i]; g_seenGrp[i].n = 1; g_seenGrp[i].tag[0] = &g_classTag[i]; }
    g_seenGrp[7].bit = 0x100; g_seenGrp[7].n = 2; g_seenGrp[7].tag[0] = &g_classTag[6]; g_seenGrp[7].tag[1] = &g_classTag[0];
    g_seenNGrp = 8;
    g_seenMaskOff = 0x20; g_seenCntOff = 0x24; g_seenNumKind = 11;
    g_seenSensOff = 0xDD0; g_seenDummyOff = 0x260; g_seenListOff = 0x268; g_seenLists = 4;
    g_seenDefOff = 0x548; g_seenPropBeg = 0x330; g_seenTagBeg = 0x4E8;
    g_seenPeek = r_peek; g_seenWord = r_word; g_seenReadInt = r_int; g_seenBaseParse = base_parse; g_seenEvalOrig = orig_eval;

    /* --- parse --- */
    { const char *t[] = { "w:enemy", "w:tank", "n:2", NULL }; CHECK(parse_cond(t), "consume"); CHECK(*(uint32_t *)(cond + 0x20) == 6 && *(int32_t *)(cond + 0x24) == 2, "vanilla mask %X", *(uint32_t *)(cond + 0x20)); }
    { const char *t[] = { "w:tank", "w:enemy", NULL }; parse_cond(t); CHECK(*(uint32_t *)(cond + 0x20) == 2, "enemy resets like vanilla"); }
    { const char *t[] = { "w:enemy", "w:daemon", "n:1", NULL }; parse_cond(t);
      uint32_t m = *(uint32_t *)(cond + 0x20); CHECK((m & SEEN_LOW) == 2 && (m >> SEEN_SHIFT) == 1, "custom set 1, mask %X", m); }
    { const char *t[] = { "w:daemon", "w:enemy", "n:1", NULL }; parse_cond(t);
      uint32_t m = *(uint32_t *)(cond + 0x20); CHECK((m & SEEN_LOW) == 2 && (m >> SEEN_SHIFT) == 1, "word before enemy survives, same set reused, mask %X", m); }
    { const char *t[] = { "w:enemy", "w:Daemon", "w:daemon", "n:1", NULL }; parse_cond(t);
      CHECK((*(uint32_t *)(cond + 0x20) >> SEEN_SHIFT) == 1, "case-insensitive word dedupe"); }
    { const char *t[] = { "w:enemy", "w:nurgle", "w:daemon", NULL }; parse_cond(t);
      uint32_t s = *(uint32_t *)(cond + 0x20) >> SEEN_SHIFT; CHECK(s == 2 && g_seenSets[1].n == 2, "two-word set"); }
    { const char *t[] = { "w:enemy", "w:daemon", "w:nurgle", NULL }; parse_cond(t);
      CHECK((*(uint32_t *)(cond + 0x20) >> SEEN_SHIFT) == 2, "order-independent set"); }
    CHECK(g_seenNWords == 2 && g_tagsAlive == 2, "2 words interned, %d tags alive", g_tagsAlive);

    /* --- eval --- */
    static Ent self, a, b, c, d, f; static Sensor sen; static Rec rec[6];
    ent_init(&self, NULL, NULL);
    static const char *pa[] = { "tank", NULL }, *pb[] = { "human", "daemon", NULL }, *tc[] = { "daemon", NULL }, *pd[] = { "human", NULL };
    ent_init(&a, pa, NULL);    /* enemy tank */
    ent_init(&b, pb, NULL);    /* enemy human with prop daemon */
    ent_init(&c, pd, tc);      /* enemy human with runtime tag daemon */
    ent_init(&d, pd, NULL);    /* enemy plain human */
    ent_init(&f, pb, NULL);    /* ALLY human with prop daemon */
    memset(&sen, 0, sizeof sen);
    *(Sensor **)(self.raw + 0xDD0) = &sen; *(void **)(sen.raw + 0x260) = &sen;
    Ent *who[] = { &a, &b, &c, &d, &f }; uint32_t fl[] = { 3, 3, 3, 3, 1 };
    for (int i = 0; i < 5; i++) { rec[i].flags = fl[i]; rec[i].ent = who[i]; sen.recs[i] = &rec[i]; }
    rec[5].flags = 2; rec[5].ent = &b; sen.recs[5] = &rec[5];           /* enemy daemon, but not currently seen */
    *(void ***)(sen.raw + 0x268) = sen.recs; *(void ***)(sen.raw + 0x270) = sen.recs + 6;   /* list 0; lists 1..3 empty */

    struct { const char *t[6]; int expect; } cases[] = {
        { { "w:enemy", "w:daemon", "n:1", NULL }, 1 },          /* b, c */
        { { "w:enemy", "w:daemon", "n:2", NULL }, 1 },
        { { "w:enemy", "w:daemon", "n:3", NULL }, 0 },
        { { "w:enemy", "w:tank", "w:daemon", "n:3", NULL }, 1 },/* a, b, c */
        { { "w:enemy", "w:tank", "w:daemon", "n:4", NULL }, 0 },
        { { "w:ally", "w:daemon", "n:1", NULL }, 1 },           /* f */
        { { "w:ally", "w:daemon", "n:2", NULL }, 0 },
        { { "w:daemon", "n:4", NULL }, 1 },                     /* no relation word: the default "any" (human/vehicle props) b c d f, plus daemon b c f -> 4 */
        { { "w:daemon", "n:6", NULL }, 0 },
        { { "w:enemy", "w:nurgle", "n:1", NULL }, 0 },
    };
    for (unsigned i = 0; i < sizeof cases / sizeof *cases; i++) {
        parse_cond(cases[i].t);
        int r = seen_eval(cond, self.raw);
        CHECK(r == cases[i].expect, "case %u -> %d, expected %d", i, r, cases[i].expect);
    }
    /* no custom words -> the original eval, untouched */
    { const char *t[] = { "w:enemy", "w:tank", "n:1", NULL }; parse_cond(t); g_origEvals = 0;
      CHECK(seen_eval(cond, self.raw) == 0x5A && g_origEvals == 1, "vanilla path"); }
    /* dummy sensor -> 0, null entity -> 0 */
    { const char *t[] = { "w:enemy", "w:daemon", "n:0", NULL }; parse_cond(t); *(int32_t *)(cond + 0x24) = 1;
      *(void **)(sen.raw + 0x260) = NULL; CHECK(seen_eval(cond, self.raw) == 0, "dummy sensor"); *(void **)(sen.raw + 0x260) = &sen;
      CHECK(seen_eval(cond, NULL) == 0, "null entity"); }

    printf(fails ? "seen_stub_test: %d FAILED\n" : "seen_stub_test: all passed\n", fails);
    return fails != 0;
}
