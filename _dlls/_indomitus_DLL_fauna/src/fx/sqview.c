/* GEM Fauna - over-head squad icons for fauna (GameControl::eSquadView)
 * Part of the unity build: #included by gem_fauna.c after ui.c (uses its repository walk), not compiled on its own.
 *
 * The icon over each unit of a selected squad comes from GameControl::eSquadView. When a view is built (vtable slot
 * found by its use of the engine string "/interface/scene/squad_view/"), it loads one sprite per Tag of a static list -
 * normal, medic, mg, at, sniper, engineer, squad_leader - into its std::map<Tag, sprite> (texture = that prefix + the
 * Tag's name). Per unit, a classifier picks one of those Tags from the unit's role flags (the squad leader gets
 * squad_leader) and shows that sprite.
 *
 * GEM Fauna:
 *   - before the first view is built, appends to that static list every other file found in interface/scene/squad_view/
 *     (the file repository, like the squad strip icons), so the engine loads those sprites too;
 *   - wraps the classifier calls: for a fauna whose def carries {props "squad_view_<file>"}, the role icons (everything
 *     but squad_leader) point at that file's sprite while the engine classifies the unit, then are put back. Optional
 *     {props "squad_view_leader_<file>"} replaces the squad leader icon of that unit as well.
 * UI only (game and editor): nothing in the simulation changes.
 */

typedef void (*SqvBuildFn)(void *self, void *a, void *b, void *c);
typedef void (*SqvClassifyFn)(void *item, void *entity, void *a);
typedef void *(*EngNewFn)(size_t n);

static MsString *g_sqvPrefix;                    /* engine global std::string "/interface/scene/squad_view/" */
static void **g_vtSqv; static int g_sqvBuildSlot = -1; static SqvBuildFn g_sqvBuildOrig;
static uint8_t **g_sqvVec;                       /* static std::vector<Tag>: begin, end, capacity */
static EngNewFn g_engNew;
static SqvClassifyFn g_sqvClassify; static uint8_t *g_sqvSites[4]; static int g_sqvSiteN;
static int g_okSqvIcon, g_wantSqvIcon = 1, g_sqvExtended;
#define SQV_MAP_OFF   0x08                       /* classifier: mov rax,[item+8] -> the view's map ; mov r14,[rax] -> head */
#define SQV_NODE_NAME (0x20 + TAG_NAME)          /* map node: header 0x20, Tag key (name std::string at +0x40), value */
#define SQV_NODE_VAL  (0x20 + TAG_SIZE)
#define SQV_MAX_ADD   256

static int find_sqview(void)
{
    /* the prefix string: lea rdx,["/interface/scene/squad_view/"] ; lea rcx,[G] ; call assign  (its static initialiser) */
    static const char litText[] = "/interface/scene/squad_view/";        /* not at a string boundary (0xFF padding before it) */
    uint8_t *lit = find_bytes(g_rdata.p, g_rdata.n, (const uint8_t *)litText, sizeof litText);
    for (int k = 0; lit && k < 4 && !g_sqvPrefix; k++) {
        uint8_t *l = find_lea_to(lit, k);
        if (!l) break;
        if (l[2] == 0x15 && l[7] == 0x48 && l[8] == 0x8D && l[9] == 0x0D) { uint8_t *g = l + 14 + *(int32_t *)(l + 10); if (in_data(g)) g_sqvPrefix = (MsString *)g; }
    }
    g_vtSqv = find_vtable_rtti(".?AVeSquadView@GameControl@@");
    if (!g_sqvPrefix || !g_vtSqv) { logf_("head icon: squad view %s not found", g_sqvPrefix ? "class" : "prefix string"); return 0; }
    /* the builder: the view's virtual that reads the prefix */
    for (int s = 0; s < 60 && g_sqvBuildSlot < 0; s++) {
        uint8_t *f = (uint8_t *)g_vtSqv[s];
        if (!in_text(f)) break;
        if (find_rip_ref(f, 0x400, g_sqvPrefix) || find_rip_ref(f, 0x400, (uint8_t *)g_sqvPrefix + 0x18) || find_rip_ref(f, 0x400, (uint8_t *)g_sqvPrefix + 0x10)) g_sqvBuildSlot = s;
    }
    if (g_sqvBuildSlot < 0) { logf_("head icon: squad view builder not found"); return 0; }
    /* the Tag list it walks: two loads mov r64,[rip+V] / [rip+V+8] near its start */
    uint8_t *bf = (uint8_t *)g_vtSqv[g_sqvBuildSlot];
    for (uint8_t *p = bf; p < bf + 0x200 && !g_sqvVec; p++) {
        if ((p[0] & 0xFB) != 0x48 || p[1] != 0x8B || (p[2] & 0xC7) != 0x05 || (p[7] & 0xFB) != 0x48 || p[8] != 0x8B || (p[9] & 0xC7) != 0x05) continue;
        uint8_t *a1 = p + 7 + *(int32_t *)(p + 3), *a2 = p + 14 + *(int32_t *)(p + 10);
        if (a2 == a1 + 8 && in_data(a1)) g_sqvVec = (uint8_t **)a1;
        else if (a1 == a2 + 8 && in_data(a2)) g_sqvVec = (uint8_t **)a2;
    }
    /* the engine allocator: the list's static initialiser stores the new buffer: call NEW ; ... mov [rip+V],rax */
    for (uint8_t *p = g_text.p; g_sqvVec && p + 7 <= g_text.p + g_text.n && !g_engNew; p++)
        if (p[0] == 0x48 && p[1] == 0x89 && p[2] == 0x05 && p + 7 + *(int32_t *)(p + 3) == (uint8_t *)g_sqvVec)
            for (uint8_t *q = p - 5; q > p - 0x28; q--) { uint8_t *t = call_target_fn(q); if (t) { g_engNew = (EngNewFn)t; break; } }
    if (!g_sqvVec || !g_engNew || !g_tagCtorCStr) { logf_("head icon: icon list %s not recognised", g_sqvVec ? "allocator" : "vector"); return 0; }
    /* the classifier: the function holding lea rdx,[Tag squad_leader] that also looks up the view's map by name */
    uint8_t *ctor = NULL, *tl = find_static_tag("squad_leader", &ctor);
    static const uint8_t mapLoad[] = { 0x49, 0x8B, 0x44, 0x24, SQV_MAP_OFF }, nameLea[] = { 0x48, 0x8D, 0x4B, SQV_NODE_NAME };
    for (int k = 0; tl && k < 8 && !g_sqvClassify; k++) {
        uint8_t *l = find_lea_to(tl, k);
        if (!l) break;
        uint32_t f = func_of(l);
        if (!f || l[2] != 0x15) continue;
        uint8_t *fn = g_base + f;
        if (find_bytes(fn, 0x400, mapLoad, sizeof mapLoad) && find_bytes(fn, 0x400, nameLea, sizeof nameLea)) g_sqvClassify = (SqvClassifyFn)fn;
    }
    for (uint8_t *p = g_text.p; g_sqvClassify && p + 5 <= g_text.p + g_text.n && g_sqvSiteN < 4; p++)
        if (p[0] == 0xE8 && p + 5 + *(int32_t *)(p + 1) == (uint8_t *)g_sqvClassify) g_sqvSites[g_sqvSiteN++] = p;
    if (!g_sqvClassify || !g_sqvSiteN) { logf_("head icon: icon classifier %s not found", g_sqvClassify ? "call" : "function"); return 0; }
    logf_("head icon: squad view builder = vt slot %d exe+0x%llX, icon list @ exe+0x%llX (%d icons), engine new exe+0x%llX, classifier exe+0x%llX (%d calls)",
          g_sqvBuildSlot, EXE_OFF(bf), EXE_OFF(g_sqvVec), g_sqvVec[0] ? (int)((g_sqvVec[1] - g_sqvVec[0]) / TAG_SIZE) : -1, EXE_OFF(g_engNew),
          EXE_OFF(g_sqvClassify), g_sqvSiteN);
    return 1;
}

#ifndef FX_TEST
/* names (lower case, no extension) of the files directly in interface/scene/squad_view/, from the file repository */
static char (*g_sqvAdd)[ICON_NAME]; static int g_sqvAddN;
static int sqv_listed(const char *nm)
{
    for (uint8_t *t = g_sqvVec[0]; t && t + TAG_SIZE <= g_sqvVec[1]; t += TAG_SIZE) {
        const MsString *s = (const MsString *)(t + TAG_NAME);
        if (s->size == strlen(nm) && !_strnicmp(ms_cstr(s), nm, s->size)) return 1;
    }
    for (int i = 0; i < g_sqvAddN; i++) if (!strcmp(g_sqvAdd[i], nm)) return 1;
    return 0;
}
static int sqv_scan(void)
{
    uint8_t *head = g_repoMap ? *g_repoMap : NULL;
    if (!head) return 0;
    static const char pat[] = "interface/scene/squad_view/"; const size_t pl = sizeof pat - 1;
    if (!g_sqvAdd) g_sqvAdd = calloc(SQV_MAX_ADD, ICON_NAME);
    if (!g_sqvAdd) return 0;
    size_t keys = 0;
    for (uint8_t *n = RB_L(head); n && n != head && !RB_NIL(n) && keys < 4000000 && g_sqvAddN < SQV_MAX_ADD; n = repo_next(n), keys++) {
        const MsString *key = (const MsString *)(n + 0x20);
        if (key->size <= pl || key->size > 400) continue;
        const char *k = ms_cstr(key);
        for (size_t i = 0; i + pl < key->size; i++) {
            size_t j = 0;
            for (; j < pl; j++) { char c = k[i + j]; if (c == '\\') c = '/'; if (c >= 'A' && c <= 'Z') c += 32; if (c != pat[j]) break; }
            if (j < pl) continue;
            const char *nm = k + i + pl; size_t m = key->size - i - pl, e = 0, sub = 0;
            for (size_t q = 0; q < m; q++) { if (nm[q] == '/' || nm[q] == '\\') sub = 1; if (nm[q] == '.' && !e) e = q; }
            if (sub || !e || e >= ICON_NAME) break;
            char low[ICON_NAME]; for (size_t q = 0; q < e; q++) { char c = nm[q]; low[q] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; } low[e] = 0;
            if (!sqv_listed(low)) strcpy(g_sqvAdd[g_sqvAddN++], low);
            break;
        }
    }
    return keys > 0;
}
/* the static Tag list, re-built in an engine buffer with the extra names appended (the old buffer is left alone: the
   list's exit-time destructor frees whatever the vector points at, with the same engine allocator) */
static void sqv_extend(void)
{
    if (!sqv_scan()) return;                                       /* repository not filled yet: try at the next build */
    g_sqvExtended = 1;
    if (!g_sqvAddN) { logf_("head icon: no extra files in interface/scene/squad_view/ - only the engine's 7 icons"); return; }
    size_t old = (size_t)(g_sqvVec[1] - g_sqvVec[0]) / TAG_SIZE, n = old + (size_t)g_sqvAddN;
    uint8_t *buf = (uint8_t *)g_engNew(n * TAG_SIZE);
    if (!buf) return;
    memset(buf, 0, n * TAG_SIZE);
    for (size_t i = 0; i < old; i++) g_tagCtorCStr(buf + i * TAG_SIZE, ms_cstr((const MsString *)(g_sqvVec[0] + i * TAG_SIZE + TAG_NAME)));
    for (int i = 0; i < g_sqvAddN; i++) g_tagCtorCStr(buf + (old + (size_t)i) * TAG_SIZE, g_sqvAdd[i]);
    g_sqvVec[0] = buf; g_sqvVec[1] = buf + n * TAG_SIZE; g_sqvVec[2] = buf + n * TAG_SIZE;
    char list[400] = "";
    for (int i = 0; i < g_sqvAddN && strlen(list) + strlen(g_sqvAdd[i]) + 2 < sizeof list; i++) { strcat(list, g_sqvAdd[i]); strcat(list, " "); }
    logf_("head icon: %d extra icon(s) from interface/scene/squad_view/ loaded with the engine's %zu: %s", g_sqvAddN, old, list);
}
static void fx_sqv_build(void *self, void *a, void *b, void *c)
{
    if (!g_sqvExtended) sqv_extend();
    g_sqvBuildOrig(self, a, b, c);
}

/* {props "squad_view_<file>"} / {props "squad_view_leader_<file>"} of an entity (Tag names, case kept as written) */
static void sqv_props(const uint8_t *e, const MsString **role, const MsString **leader)
{
    const uint8_t *b, *en; *role = *leader = NULL;
    if (!def_props(e, &b, &en)) return;
    for (const uint8_t *t = b; t + TAG_SIZE <= en; t += TAG_SIZE) {
        const MsString *nm = (const MsString *)(t + TAG_NAME);
        const char *s = ms_cstr(nm);
        if (nm->size > 18 && !_strnicmp(s, "squad_view_leader_", 18)) *leader = nm;
        else if (nm->size > 11 && !_strnicmp(s, "squad_view_", 11)) *role = nm;
    }
}
static int node_is(const uint8_t *n, const char *s, size_t len)
{
    const MsString *nm = (const MsString *)(n + SQV_NODE_NAME);
    return nm->size == len && !_strnicmp(ms_cstr(nm), s, len);
}
static void fx_sqv_classify(void *item, void *entity, void *a)
{
    uint8_t *e = (uint8_t *)entity;
    const MsString *role = NULL, *leader = NULL;
    if (e && is_fauna(e)) sqv_props(e, &role, &leader);
    uint8_t *map = (role || leader) && item ? *(uint8_t **)((uint8_t *)item + SQV_MAP_OFF) : NULL;
    uint8_t *head = map ? *(uint8_t **)map : NULL;
    if (!head) { g_sqvClassify(item, entity, a); return; }
    const char *rn = role ? ms_cstr(role) + 11 : NULL, *ln = leader ? ms_cstr(leader) + 18 : NULL;
    size_t rl = role ? role->size - 11 : 0, ll = leader ? leader->size - 18 : 0;
    void *rs = NULL, *ls = NULL;                                     /* the custom sprites */
    int cnt = 0;
    for (uint8_t *n = RB_L(head); n && n != head && !RB_NIL(n) && cnt < 1024; n = repo_next(n), cnt++) {
        if (rn && node_is(n, rn, rl)) rs = *(void **)(n + SQV_NODE_VAL);
        if (ln && node_is(n, ln, ll)) ls = *(void **)(n + SQV_NODE_VAL);
    }
    static int miss;
    if ((rn && !rs) || (ln && !ls)) {
        if (miss < 6) { miss++; logf_("head icon: fauna %p: icon file \"%.*s\" not loaded (put it in interface/scene/squad_view/)", e,
                                      (int)(rn && !rs ? rl : ll), rn && !rs ? rn : ln); }
        if (!rs && !ls) { g_sqvClassify(item, entity, a); return; }
    }
    uint8_t *swapped[64]; void *saved[64]; int k = 0;
    cnt = 0;
    for (uint8_t *n = RB_L(head); n && n != head && !RB_NIL(n) && cnt < 1024 && k < 64; n = repo_next(n), cnt++) {
        int isLeader = node_is(n, "squad_leader", 12);
        void *want = isLeader ? ls : (node_is(n, "normal", 6) || node_is(n, "medic", 5) || node_is(n, "mg", 2) || node_is(n, "at", 2) ||
                                      node_is(n, "sniper", 6) || node_is(n, "engineer", 8)) ? rs : NULL;
        if (!want) continue;
        swapped[k] = n; saved[k] = *(void **)(n + SQV_NODE_VAL); k++;
        *(void **)(n + SQV_NODE_VAL) = want;
    }
    g_sqvClassify(item, entity, a);                                  /* the engine picks a role; every role is the custom icon */
    for (int i = 0; i < k; i++) *(void **)(swapped[i] + SQV_NODE_VAL) = saved[i];
    static int logged; if (logged < 6) { logged++; logf_("head icon: fauna %p uses \"%.*s\"%s%.*s%s", e, (int)rl, rn ? rn : "", ln ? ", as squad leader \"" : "", (int)ll, ln ? ln : "", ln ? "\"" : ""); }
}

static int install_sqview(void)
{
    if (!vt_hook(g_vtSqv, g_sqvBuildSlot, (void *)fx_sqv_build, (void **)&g_sqvBuildOrig)) { logf_("head icon: squad view vtable not writable"); return 0; }
    for (int i = 0; i < g_sqvSiteN; i++)
        if (!redirect_call(g_sqvSites[i], (void *)fx_sqv_classify, NULL, "head icon (classifier)")) return i > 0;
    return 1;
}
#endif /* FX_TEST */
