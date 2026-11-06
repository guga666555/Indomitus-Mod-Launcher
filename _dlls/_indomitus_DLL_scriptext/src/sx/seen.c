/* GEM ScriptExt - talk condition {seen ...} with prop / tag words
 * Part of the unity build: #included by gem_scriptext.c, not compiled on its own.
 *
 * Vanilla (eActorTalkScanner.cpp): {seen enemy tank 1} counts the units the actor's sensor currently sees that pass the
 * relation word (enemy / ally) and ANY of the class words (tank car cannon airborne helicopter human vehicle any), a class
 * word meaning "the unit's def {props} contain that class Tag"; true when the count >= the number. Any other word logs
 * "unknown" (0xBAD0001) and is ignored.
 *
 * Here any other word is a prop OR tag name: the seen unit also matches when its def {props} contain it or its runtime
 * tags ({tags} actions, breed tags, the list {if tagged} reads) contain it. Words are OR'ed like the vanilla class words:
 *   {seen enemy daemon 1}          an enemy with prop or tag "daemon"
 *   {seen enemy tank daemon 2}     two enemies that are tanks or carry "daemon"
 *
 * How: the condition object is {vt, vector<child> +8..+0x18, mask +MASK, count +CNT} (0x28 bytes, no spare room). Two
 * vtable slots are swapped (no code patch): parse re-runs the vanilla word loop (its keyword -> mask operations are read
 * out of the vanilla parser, so they match the exe) and keeps the extra words as an interned set whose 1-based index goes
 * into mask bits SEEN_SHIFT..31 (vanilla uses bits 0..9). eval: no set -> the original eval, unchanged; with a set ->
 * the same loop as the vanilla count function (sensor lists, relation, class Tags read out of that function) plus the
 * prop / tag test. Sets and Tags are append-only and live until exit, so no lifetime question when talk data is freed.
 */

#define SEEN_SHIFT    20
#define SEEN_LOW      ((1u << SEEN_SHIFT) - 1)
#define SEEN_MAX_KW   16
#define SEEN_MAX_GRP  12
#define SEEN_MAX_GTAG 4
#define SEEN_MAX_WORD 256
#define SEEN_MAX_SET  1024
#define SEEN_SET_N    8

typedef uint8_t (*SeenEvalFn)(void *cond, void *entity);
typedef void    (*SeenParseFn)(void *cond, void *reader);
typedef int32_t (*SeenReadIntFn)(void *reader);

/* keyword -> mask operation, read out of the vanilla parser */
typedef struct { char name[16]; uint32_t len; int set; uint32_t val; } SeenKw;
/* class bit -> the class Tags it tests in the def props, read out of the vanilla count function */
typedef struct { uint32_t bit; int n; const uint64_t *tag[SEEN_MAX_GTAG]; } SeenGrp;

static void      **g_seenVt;
static SeenEvalFn  g_seenEvalOrig;
static SeenParseFn g_seenParseOrig, g_seenBaseParse;
static RdPeekFn    g_seenPeek;
static RdWordFn    g_seenWord;
static SeenReadIntFn g_seenReadInt;
static int         g_seenNumKind;
static uint32_t    g_seenMaskOff, g_seenCntOff;
static SeenKw      g_seenKw[SEEN_MAX_KW]; static int g_seenNKw;
static SeenGrp     g_seenGrp[SEEN_MAX_GRP]; static int g_seenNGrp;
static uint32_t    g_seenRelEnemy = 2, g_seenRelAlly = 1;                 /* mask bits; record flag bit 1 = enemy */
static uint32_t    g_seenSensOff, g_seenDummyOff, g_seenListOff, g_seenLists;
static uint32_t    g_seenDefOff, g_seenPropBeg, g_seenTagBeg;            /* [e+def]+propBeg..+8, e+tagBeg..+8: vector<Tag 0x60> */
static int         g_okSeen;

#ifndef SX_SEEN_STUB_TEST     /* tests/seen_stub_test.c supplies its own engine stand-ins */
/* ------------------------------------------------------------------------------------ */
/* finder (also runs in the offline test)                                               */
/* ------------------------------------------------------------------------------------ */
/* two consecutive 'mov r64,[r64+disp32]' (REX.W 8B, mod 10) with disps 8 apart -> the lower (vector begin) */
static uint32_t seen_vec_pair(const uint8_t *p, const uint8_t *end)
{
    for (; p + 14 <= end; p++) {
        if ((p[0] & 0xF8) != 0x48 || p[1] != 0x8B || (p[2] & 0xC0) != 0x80 || (p[2] & 7) == 4) continue;
        const uint8_t *q = p + 7;
        if ((q[0] & 0xF8) != 0x48 || q[1] != 0x8B || (q[2] & 0xC0) != 0x80 || (q[2] & 7) == 4) continue;
        uint32_t a = *(const uint32_t *)(p + 3), b = *(const uint32_t *)(q + 3);
        if (a == b + 8) return b;
        if (b == a + 8) return a;
    }
    return 0;
}

static int find_seen(void)
{
    g_okSeen = 0; g_seenNKw = 0; g_seenNGrp = 0;
    uint8_t *ctor = find_table_ctor("seen");
    void **vt = ctor ? cond_vtable_of(ctor) : NULL;
    if (!vt) { logf_("{seen}: talk condition 'seen' not found"); return 0; }
    uint8_t *ev = (uint8_t *)vt[1], *ps = (uint8_t *)vt[2];
    if (!in_text(vt[0]) || !in_text(ev) || !in_text(ps)) { logf_("{seen}: vtable slots not in .text"); return 0; }

    /* eval: mov r8d,[rcx+MASK] ; mov ebx,[rcx+CNT] ; call COUNT ; cmp eax,ebx ; setge al */
    uint8_t *count = NULL;
    for (uint8_t *p = ev; p < ev + 0x18; p++)
        if (p[0] == 0x44 && p[1] == 0x8B && p[2] == 0x41 && p[4] == 0x8B && p[5] == 0x59 && p[7] == 0xE8) {
            g_seenMaskOff = p[3]; g_seenCntOff = p[6]; count = p + 12 + *(int32_t *)(p + 8); break;
        }
    static const uint8_t setge[] = { 0x3B,0xC3, 0x0F,0x9D,0xC0 };
    if (!count || !in_text(count) || !find_bytes(ev, 0x20, setge, sizeof setge) || g_seenMaskOff == g_seenCntOff)
        { logf_("{seen}: eval not recognised"); return 0; }

    /* count: sensor = [rdx+SENS] ; cmp qword [rax+DUMMY],0 ; lists at [rax+r13+LIST] (+8 end) ; add r13,0x18 ; cmp r13,N*0x18 */
    for (uint8_t *p = count; p < count + 0x80; p++) {
        if (p[0] == 0x48 && p[1] == 0x8B && p[2] == 0x82 && !g_seenSensOff) g_seenSensOff = *(uint32_t *)(p + 3);
        if (p[0] == 0x48 && p[1] == 0x83 && p[2] == 0xB8 && p[7] == 0 && !g_seenDummyOff) g_seenDummyOff = *(uint32_t *)(p + 3);
        if (p[0] == 0x4A && p[1] == 0x8B && (p[2] & 0xC7) == 0x84 && (p[3] & 0x38) == 0x28 && !g_seenListOff) g_seenListOff = *(uint32_t *)(p + 4);
    }
    uint8_t *stop = NULL;                                       /* epilogue: pop r12 ; pop rdi ; ret */
    static const uint8_t epi[] = { 0x41,0x5C, 0x5F, 0xC3 };
    stop = find_bytes(count, 0x500, epi, sizeof epi);
    for (uint8_t *p = count; stop && p < stop; p++)
        if (p[0] == 0x49 && p[1] == 0x83 && p[2] == 0xFD && p[3] % 0x18 == 0) { g_seenLists = p[3] / 0x18; break; }
    static const uint8_t recFlags[] = { 0x8B,0x48,0x14 }, recEnt[] = { 0x4C,0x8B,0x78,0x18 },
                         relE[] = { 0x41,0xF6,0xC4,0x02 }, relA[] = { 0x41,0xF6,0xC4,0x01 };
    if (!stop || !g_seenSensOff || !g_seenDummyOff || !g_seenListOff || g_seenLists < 1 || g_seenLists > 8 ||
        !find_bytes(count, 0xC0, recFlags, 3) || !find_bytes(count, 0xC0, recEnt, 4) ||
        !find_bytes(count, 0xC0, relE, 4) || !find_bytes(count, 0xC0, relA, 4))
        { logf_("{seen}: count function layout not recognised (stop %d sens %X dummy %X list %X lists %u)", stop != NULL, g_seenSensOff, g_seenDummyOff, g_seenListOff, g_seenLists); return 0; }
    /* class groups: 'test r12b,BIT' / 'bt r12d,N' / 'test r12b,r12b ; jns' then the class Tags 'lea rdx,[rip+TAG]' (.data) */
    SeenGrp *g = NULL; int ntags = 0;
    for (uint8_t *p = count; p < stop; p++) {
        uint32_t bit = 0;
        if (p[0] == 0x41 && p[1] == 0xF6 && p[2] == 0xC4 && p[3] != 1 && p[3] != 2) bit = p[3];
        else if (p[0] == 0x41 && p[1] == 0x0F && p[2] == 0xBA && p[3] == 0xE4 && p[4] < 31) bit = 1u << p[4];
        else if (p[0] == 0x45 && p[1] == 0x84 && p[2] == 0xE4 && p[3] == 0x79) bit = 0x80;
        if (bit) {
            if (g_seenNGrp >= SEEN_MAX_GRP) { logf_("{seen}: too many class groups"); return 0; }
            g = &g_seenGrp[g_seenNGrp++]; g->bit = bit; g->n = 0; continue;
        }
        if (g && p[0] == 0x48 && p[1] == 0x8D && p[2] == 0x15) {
            uint8_t *t = p + 7 + *(int32_t *)(p + 3);
            if (!in_data(t)) continue;
            if (g->n >= SEEN_MAX_GTAG) { logf_("{seen}: too many Tags in a class group"); return 0; }
            g->tag[g->n++] = (const uint64_t *)t; ntags++;
        }
        if (!g_seenDefOff && p[0] == 0x49 && p[1] == 0x8B && (p[2] & 0xC7) == 0x87) {      /* mov r64,[r15+DEF] */
            uint32_t d = *(uint32_t *)(p + 3), pb = seen_vec_pair(p + 7, p + 7 + 14);
            if (d > 0x100 && d < 0x2000 && pb > 0x40 && pb < 0x2000) { g_seenDefOff = d; g_seenPropBeg = pb; }
        }
    }
    for (int i = 0; i < g_seenNGrp; i++) if (!g_seenGrp[i].n) { logf_("{seen}: class bit 0x%X has no Tag", g_seenGrp[i].bit); return 0; }
    if (g_seenNGrp < 4 || !g_seenDefOff) { logf_("{seen}: class tests not recognised (%d groups)", g_seenNGrp); return 0; }

    /* parse: Peek(reader,1,0) / ReadWord(reader,1) loop; keyword: cmp rbx,LEN ; jne ; ... lea rdx,"kw" ; call strncmp ;
       test eax,eax ; jne ; [one mov] ; op on [rbp+MASK]; then Peek(reader,NUM,0) ; ReadInt ; mov [rbp+CNT],eax ; jmp BASE */
    for (uint8_t *p = ps; p < ps + 0x40 && !g_seenPeek; p++)
        if (p[0] == 0xBA && *(uint32_t *)(p + 1) == 1) { uint8_t *t = next_call(p + 5, 8, NULL); if (t) g_seenPeek = (RdPeekFn)t; }
    for (uint8_t *p = ps; p < ps + 0x80 && !g_seenWord; p++)
        if (p[0] == 0xB2 && p[1] == 0x01) { uint8_t *t = next_call(p + 2, 0x18, NULL); if (t) g_seenWord = (RdWordFn)t; }
    if (!g_seenPeek || !g_seenWord) { logf_("{seen}: parser reads not recognised"); return 0; }
    uint8_t *pend = ps + 0x500, *numSite = NULL;
    for (uint8_t *p = ps; p < pend; p++) {
        if (p[0] == 0x48 && p[1] == 0x8D && p[2] == 0x15) {
            uint8_t *s = p + 7 + *(int32_t *)(p + 3);
            if (!in_rdata(s)) continue;
            size_t L = 0; while (L < 16 && s[L]) L++;
            if (L < 2 || L > 15) continue;
            uint32_t len = 0;
            for (uint8_t *q = p - 4; q > p - 20; q--) if (q[0] == 0x48 && q[1] == 0x83 && (q[2] & 0xF8) == 0xF8) { len = q[3]; break; }
            uint8_t *c = p + 7;
            if (len != L || c[0] != 0xE8 || c[5] != 0x85 || c[6] != 0xC0 || c[7] != 0x75) continue;
            int set = -1; uint32_t val = 0;
            for (uint8_t *o = c + 9; o < c + 9 + 8 && set < 0; o++) {
                if (o[0] == 0xC7 && o[1] == 0x45 && o[2] == g_seenMaskOff) { set = 1; val = *(uint32_t *)(o + 3); }
                else if (o[0] == 0x83 && o[1] == 0x4D && o[2] == g_seenMaskOff) { set = 0; val = o[3]; }
                else if (o[0] == 0x81 && o[1] == 0x4D && o[2] == g_seenMaskOff) { set = 0; val = *(uint32_t *)(o + 3); }
                else if (o[0] == 0x09 && o[1] == 0x5D && o[2] == g_seenMaskOff) { set = 0; val = len; }    /* or [rbp+MASK],ebx (ebx = len) */
            }
            if (set < 0 || !val || val > SEEN_LOW || g_seenNKw >= SEEN_MAX_KW) continue;
            SeenKw *k = &g_seenKw[g_seenNKw++];
            memcpy(k->name, s, L); k->name[L] = 0; k->len = len; k->set = set; k->val = val;
        }
        if (p[0] == 0xBA && p[2] == 0 && p[3] == 0 && p[4] == 0 && p[1] > 1 && p[1] < 0x20) {
            uint8_t *site, *t = next_call(p + 5, 8, &site);
            if (t != (uint8_t *)g_seenPeek) continue;
            uint8_t *r, *ri = next_call(site + 5, 0x10, &r);
            if (ri && r[5] == 0x89 && r[6] == 0x45 && r[7] == g_seenCntOff) { g_seenNumKind = p[1]; g_seenReadInt = (SeenReadIntFn)ri; numSite = r + 8; break; }
        }
    }
    for (uint8_t *q = numSite; q && q < numSite + 0x40 && !g_seenBaseParse; q++)
        if (q[0] == 0xE9) { uint8_t *t = q + 5 + *(int32_t *)(q + 1); if (in_text(t)) g_seenBaseParse = (SeenParseFn)t; }
    int haveE = 0, haveA = 0;
    for (int i = 0; i < g_seenNKw; i++) {
        if (!strcmp(g_seenKw[i].name, "enemy")) { haveE = 1; g_seenRelEnemy = g_seenKw[i].val; }
        if (!strcmp(g_seenKw[i].name, "ally"))  { haveA = 1; g_seenRelAlly  = g_seenKw[i].val; }
    }
    if (!haveE || !haveA || g_seenNKw < 6 || !g_seenReadInt || !g_seenBaseParse || g_seenRelEnemy != 2 || g_seenRelAlly != 1)
        { logf_("{seen}: parser not recognised (%d keywords)", g_seenNKw); return 0; }

    /* runtime tags: the {if tagged "x"} eval walks [ctx[0]+BEG .. +END] */
    uint8_t *tctor = find_table_ctor("tagged");
    void **tvt = tctor ? cond_vtable_of(tctor) : NULL;
    uint8_t *tev = tvt ? (uint8_t *)tvt[1] : NULL;
    g_seenTagBeg = tev && in_text(tev) ? seen_vec_pair(tev, tev + 0x50) : 0;
    if (g_seenTagBeg < 0x100 || g_seenTagBeg > 0x2000) { logf_("{seen}: {if tagged} layout not recognised"); g_seenTagBeg = 0; return 0; }

    g_seenVt = vt; g_seenEvalOrig = (SeenEvalFn)ev; g_seenParseOrig = (SeenParseFn)ps;
    char kws[256]; size_t o = 0;
    for (int i = 0; i < g_seenNKw && o < sizeof kws - 32; i++)
        o += snprintf(kws + o, sizeof kws - o, " %s%s0x%X", g_seenKw[i].name, g_seenKw[i].set ? "=" : "|", g_seenKw[i].val);
    logf_("{seen}: vtable exe+0x%llX (eval exe+0x%llX parse exe+0x%llX count exe+0x%llX base parse exe+0x%llX), mask +0x%X count +0x%X, number kind %d",
          EXE_OFF(vt), EXE_OFF(ev), EXE_OFF(ps), EXE_OFF(count), EXE_OFF(g_seenBaseParse), g_seenMaskOff, g_seenCntOff, g_seenNumKind);
    logf_("{seen}: keywords%s", kws);
    logf_("{seen}: %d class groups (%d Tags), sensor entity+0x%X dummy +0x%X %u lists @ +0x%X; props [e+0x%X]+0x%X, tags e+0x%X",
          g_seenNGrp, ntags, g_seenSensOff, g_seenDummyOff, g_seenLists, g_seenListOff, g_seenDefOff, g_seenPropBeg, g_seenTagBeg);
    return g_okSeen = 1;
}
#endif /* SX_SEEN_STUB_TEST */

#if !defined(SX_TEST) || defined(SX_SEEN_STUB_TEST)
/* ------------------------------------------------------------------------------------ */
/* runtime                                                                              */
/* ------------------------------------------------------------------------------------ */
typedef struct { char name[64]; uint64_t id; _Alignas(16) uint8_t tag[0x60]; } SeenWord;   /* tag kept alive until exit */
typedef struct { int n; uint64_t id[SEEN_SET_N]; } SeenSet;
static SeenWord g_seenWords[SEEN_MAX_WORD]; static volatile int g_seenNWords;
static SeenSet  g_seenSets[SEEN_MAX_SET];   static volatile int g_seenNSets;
static CRITICAL_SECTION g_seenLock;
static int g_seenFullLogged, g_seenEvalLogged;

static int seen_ieq(const char *a, const char *b)
{
    for (;; a++, b++) {
        int x = (unsigned char)*a, y = (unsigned char)*b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
        if (!x) return 1;
    }
}

/* word -> its Tag id (0 = failed). The engine Tag is built outside the lock (engine code), then published once. */
static uint64_t seen_word(const char *w)
{
    EnterCriticalSection(&g_seenLock);
    for (int i = 0; i < g_seenNWords; i++) if (seen_ieq(g_seenWords[i].name, w)) { uint64_t id = g_seenWords[i].id; LeaveCriticalSection(&g_seenLock); return id; }
    LeaveCriticalSection(&g_seenLock);
    _Alignas(16) uint8_t tmp[0x60]; memset(tmp, 0, sizeof tmp);
    g_tagCtorCStr(tmp, w);
    uint64_t id = *(uint64_t *)tmp;
    int dup = 0, full = 0, added = 0;
    EnterCriticalSection(&g_seenLock);
    for (int i = 0; i < g_seenNWords; i++) if (seen_ieq(g_seenWords[i].name, w)) { id = g_seenWords[i].id; dup = 1; break; }
    if (!dup && id) {
        if (g_seenNWords < SEEN_MAX_WORD) {
            SeenWord *s = &g_seenWords[g_seenNWords];
            strncpy(s->name, w, sizeof s->name - 1); memcpy(s->tag, tmp, sizeof tmp); s->id = id;
            MemoryBarrier(); g_seenNWords++; added = 1;
        } else full = 1;
    }
    LeaveCriticalSection(&g_seenLock);
    if (added) logf_("{seen}: word '%s' = prop or tag (Tag %016llX)", w, (unsigned long long)id);
    if (dup || full || !id) g_tagDtor(tmp);
    if (full && !g_seenFullLogged++) logf_("{seen}: more than %d different prop/tag words - '%s' and later new words are ignored", SEEN_MAX_WORD, w);
    return full ? 0 : id;
}

/* set -> 1-based index (0 = table full) */
static uint32_t seen_set(const uint64_t *id, int n)
{
    uint32_t r = 0;
    EnterCriticalSection(&g_seenLock);
    for (int i = 0; i < g_seenNSets && !r; i++) {
        if (g_seenSets[i].n != n) continue;
        int same = 1;
        for (int k = 0; k < n && same; k++) same = g_seenSets[i].id[k] == id[k];
        if (same) r = (uint32_t)i + 1;
    }
    if (!r && g_seenNSets < SEEN_MAX_SET) {
        SeenSet *s = &g_seenSets[g_seenNSets];
        s->n = n; memcpy(s->id, id, n * sizeof *id);
        MemoryBarrier(); g_seenNSets++; r = (uint32_t)g_seenNSets;
    }
    LeaveCriticalSection(&g_seenLock);
    if (!r && !g_seenFullLogged++) logf_("{seen}: more than %d different word sets - the extra words of later {seen} conditions are ignored", SEEN_MAX_SET);
    return r;
}

/* the vanilla word loop, with unknown words kept instead of logged */
static void seen_parse(uint8_t *c, void *reader)
{
    uint32_t *mask = (uint32_t *)(c + g_seenMaskOff);
    uint64_t ids[SEEN_SET_N]; int n = 0;
    while (g_seenPeek(reader, 1, 0)) {
        const MsString *w = g_seenWord(reader, 1);
        if (!w) break;
        char buf[64]; size_t L = w->size < sizeof buf - 1 ? w->size : sizeof buf - 1;
        memcpy(buf, ms_cstr(w), L); buf[L] = 0;
        const SeenKw *k = NULL;
        for (int i = 0; i < g_seenNKw && !k; i++) if (g_seenKw[i].len == L && !memcmp(g_seenKw[i].name, buf, L)) k = &g_seenKw[i];
        if (k) { if (k->set) *mask = k->val; else *mask |= k->val; continue; }
        if (!L) continue;
        uint64_t id = seen_word(buf);
        if (!id) continue;
        int have = 0;
        for (int i = 0; i < n; i++) have |= ids[i] == id;
        if (have) continue;
        if (n < SEEN_SET_N) ids[n++] = id;
        else logf_("{seen}: more than %d prop/tag words in one condition - '%s' ignored", SEEN_SET_N, buf);
    }
    if (g_seenPeek(reader, g_seenNumKind, 0)) *(int32_t *)(c + g_seenCntOff) = g_seenReadInt(reader);
    *mask &= SEEN_LOW;
    if (n) {
        for (int i = 1; i < n; i++) for (int j = i; j > 0 && ids[j - 1] > ids[j]; j--) { uint64_t t = ids[j]; ids[j] = ids[j - 1]; ids[j - 1] = t; }
        uint32_t s = seen_set(ids, n);
        if (s) *mask |= s << SEEN_SHIFT;
    }
    g_seenBaseParse(c, reader);
}

static int seen_vec_has(const uint8_t *vec, uint64_t id)
{
    const uint8_t *b = *(const uint8_t *const *)vec, *e = *(const uint8_t *const *)(vec + 8);
    if (!b || e <= b || (size_t)(e - b) > 0x60u * 4096) return 0;
    for (; b + 0x60 <= e; b += 0x60) if (*(const uint64_t *)b == id) return 1;
    return 0;
}

static int seen_matches(const uint8_t *t, uint32_t mask, const SeenSet *set)
{
    const uint8_t *def = *(const uint8_t *const *)(t + g_seenDefOff);
    if (def)
        for (int i = 0; i < g_seenNGrp; i++) {
            if (!(mask & g_seenGrp[i].bit)) continue;
            for (int k = 0; k < g_seenGrp[i].n; k++) if (seen_vec_has(def + g_seenPropBeg, *g_seenGrp[i].tag[k])) return 1;
        }
    for (int k = 0; k < set->n; k++) {
        if (def && seen_vec_has(def + g_seenPropBeg, set->id[k])) return 1;
        if (seen_vec_has(t + g_seenTagBeg, set->id[k])) return 1;
    }
    return 0;
}

static uint8_t seen_eval(uint8_t *c, uint8_t *e)
{
    uint32_t m = *(const uint32_t *)(c + g_seenMaskOff), s = m >> SEEN_SHIFT;
    if (!s || s > (uint32_t)g_seenNSets) return g_seenEvalOrig(c, e);
    const SeenSet *set = &g_seenSets[s - 1];
    int need = *(const int32_t *)(c + g_seenCntOff), n = 0;
    const uint8_t *sensor = e ? *(const uint8_t *const *)(e + g_seenSensOff) : NULL;
    if (sensor && *(const void *const *)(sensor + g_seenDummyOff))
        for (uint32_t k = 0; k < g_seenLists; k++) {
            const uint8_t *const *b = *(const uint8_t *const *const *)(sensor + g_seenListOff + k * 0x18);
            const uint8_t *const *en = *(const uint8_t *const *const *)(sensor + g_seenListOff + k * 0x18 + 8);
            if (!b || en < b || en - b > 4096) continue;
            for (; b < en; b++) {
                const uint8_t *rec = *b;
                if (!rec) continue;
                uint32_t fl = *(const uint32_t *)(rec + 0x14);
                const uint8_t *t = *(const uint8_t *const *)(rec + 0x18);
                if (!(fl & 1) || !t) continue;                         /* currently seen */
                if ((m & g_seenRelEnemy) && !(fl & 2)) continue;       /* enemy: record flag bit 1 */
                if ((m & g_seenRelAlly) && (fl & 2)) continue;
                if (seen_matches(t, m, set)) n++;
            }
        }
    if (g_verbose && g_seenEvalLogged < 20) {
        g_seenEvalLogged++;
        logf_("{seen} with prop/tag words: entity %p sees %d matching (needs %d) -> %d", e, n, need, n >= need);
    }
    return n >= need;
}

#ifndef SX_SEEN_STUB_TEST
static int install_seen(void)
{
    InitializeCriticalSection(&g_seenLock);
    DWORD old;
    if (!VirtualProtect(g_seenVt + 1, 16, PAGE_READWRITE, &old)) { logf_("{seen}: VirtualProtect failed"); return 0; }
    g_seenVt[1] = (void *)seen_eval;
    g_seenVt[2] = (void *)seen_parse;
    VirtualProtect(g_seenVt + 1, 16, old, &old);
    return 1;
}
#endif
#endif /* runtime */
