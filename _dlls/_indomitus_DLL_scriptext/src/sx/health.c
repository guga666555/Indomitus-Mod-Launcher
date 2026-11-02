/* GEM ScriptExt - {if health X} and {health_damage_count ...}
 * Part of the unity build: #included by gem_scriptext.c, not compiled on its own.
 * Also the unit -> vitality (health component) access used by both.
 */

/* ------------------------------------------------------------------------------------ */
/* {if health X} and {health_damage_count value max_health|set_health [cap]}            */
/* ------------------------------------------------------------------------------------ */
/* Interaction::eHealthDamageCount (RTTI): slot 5 parse(reader) = ReadFloat -> [this+AMT] ; jmp common optional-block
   parser ; slot 10 GetAmount() = [this+AMT] ; slot 12 factor(this, vit, ctx, out) = damage-type multiplier (1.0 unless the
   context carries a damage type). Execute: new hp = GetHealth(vit) - GetAmount * factor * k ; SetHealth(vit, new hp)
   (no clamp either way: overheal and death through the engine's normal path). The script reader functions come from
   the same class (find_reader in engine.c). */
typedef void   (*HdcParseFn)(uint8_t *a, void *reader);
typedef float  (*HdcAmountFn)(uint8_t *a, void *ctx, void *out);
typedef float  (*HdcFactorFn)(uint8_t *a, void *vit, void *ctx, void *out);
static HdcParseFn g_hdcParseOrig, g_hdcCommon; static HdcAmountFn g_hdcAmountOrig; static HdcFactorFn g_hdcFactorOrig;
static uint32_t g_hdcAmtOff, g_hdcModeOff; static int g_okHdc, g_okHealthCond;
/* effects: vector<Tag> at [this+0x28] filled by the common parser only from an {effects ...} block (then padded to 7).
   The engine's execute always reads effects[0..5], so a damage action written without {effects} crashes the game
   (vanilla too - every vanilla use has the block). ScriptExt only warns about it in the log. */
static uint32_t g_hdcFxOff;
#define HDC_MAGIC 0x53480000u                                   /* mode word at [this+AMT+4] (padding of the 0x68-byte object) */
#define HDC_MAXH 1u
#define HDC_SETH 2u
#define HDC_CAP  0x10u

static int find_hdc(void)
{
    if (!find_reader()) return 0;
    uint8_t *ga = (uint8_t *)g_vtHdc[10];
    g_hdcCommon = (HdcParseFn)g_rdCommon;
    /* parse: call ReadFloat ; mov rdx,rbx? ; movss [rdi+AMT],xmm0 ; ... jmp common */
    for (uint8_t *q = g_rdFloatSite + 5; q < g_rdFloatSite + 0x20 && !g_hdcAmtOff; q++)
        if (q[0] == 0xF3 && q[1] == 0x0F && q[2] == 0x11 && (q[3] & 0xC0) == 0x40) g_hdcAmtOff = q[4];
    /* GetAmount = movss xmm0,[rcx+AMT] ; ret */
    if (!g_hdcAmtOff || !in_text(ga) || !in_text(g_vtHdc[12]) || ga[0] != 0xF3 || ga[1] != 0x0F || ga[2] != 0x10 || ga[3] != 0x41 || ga[4] != g_hdcAmtOff || ga[5] != 0xC3)
        { logf_("health_damage_count: parse / amount layout not recognised"); return 0; }
    /* object size from the {name, ctor} table entry: mov ecx,SIZE ; call new - the mode word lives in the padding after AMT */
    uint8_t *ctor = find_table_ctor("health_damage_count"); uint32_t size = 0;
    for (uint8_t *q = ctor; q && q < ctor + 0x10 && !size; q++) if (q[0] == 0xB9) size = *(uint32_t *)(q + 1);
    g_hdcModeOff = g_hdcAmtOff + 4;
    if (size < g_hdcModeOff + 4 || size > 0x400) { logf_("health_damage_count: object size %u leaves no room for the mode", size); return 0; }
    for (uint8_t *q = ctor; q < ctor + 0x60; q++)                       /* the ctor must not initialise our padding word */
        if ((q[0] == 0x89 || q[0] == 0xC7) && q[1] == 0x40 && q[2] == g_hdcModeOff) { logf_("health_damage_count: padding is used by the engine"); return 0; }
    /* effects vector: lea rbx,[this+0x28] in the common parser */
    uint8_t *cp = g_rdCommon;
    for (uint8_t *q = cp; q < cp + 0x300 && !g_hdcFxOff; q++)
        if (q[0] == 0x49 && q[1] == 0x8D && (q[2] & 0xF8) == 0x58 && (q[2] & 7) != 4 && q[3] == 0x28) g_hdcFxOff = 0x28;
    if (!g_hdcFxOff) logf_("health_damage_count: effects offset not recognised - no missing-{effects} warning");
    logf_("health_damage_count: vt exe+0x%llX amount +0x%X (mode word +0x%X of 0x%X), effects +0x%X, common parse exe+0x%llX",
          EXE_OFF(g_vtHdc), g_hdcAmtOff, g_hdcModeOff, size, g_hdcFxOff, EXE_OFF(g_hdcCommon));
    return 1;
}

#ifndef SX_TEST
/* the unit's vitality extender (health component): [[entity+actor]+vit], NULL if it has none */
static void *raw_vitality(const uint8_t *e)
{
    if (!g_okVit || !e) return NULL;
    const uint8_t *actor = *(const uint8_t *const *)(e + g_actorOff);
    return actor ? *(void *const *)(actor + g_vitOff) : NULL;
}
static void parse_health(void *c, void *reader)
{
    float v = g_rdFloat ? g_rdFloat(reader) : 1.0f;
    *(float *)((uint8_t *)c + 8) = v;
}
static int eval_health(void *c, uint8_t *e, void **ctx)
{
    (void)ctx;
    if (!c || !e || !(e[ENT_FLAGS] & 2)) return 0;
    void *vit = raw_vitality(e);
    if (!vit) return 0;
    float cur = g_getHealth(vit), max = g_getMaxHealth(vit);
    if (!(max > 0.0f)) return 0;
    return cur <= *(float *)((uint8_t *)c + 8) * max + 1e-4f;
}

static uint32_t hdc_mode(const uint8_t *a)
{
    uint32_t m = *(const uint32_t *)(a + g_hdcModeOff);
    return (m & 0xFFFF0000u) == HDC_MAGIC ? (m & 0xFFFFu) : 0;
}
static int ieq(const MsString *w, const char *lit)
{
    size_t n = strlen(lit);
    return w && w->size == n && !_strnicmp(ms_cstr(w), lit, n);
}
/* no {effects ...} given: the engine's execute will read effects[0..5] from an empty list and crash (vanilla too) -
   only warn in the log, the script must carry the block */
static void hdc_check_effects(uint8_t *a)
{
    if (!g_hdcFxOff) return;
    uint8_t **v = (uint8_t **)(a + g_hdcFxOff);
    if (v[0] || v[1]) return;
    static int n; if (n++ < 5) logf_("health_damage_count WITHOUT an {effects ...} block parsed - the game crashes when it hits (add e.g. {effects \"\" \"hit\" \"damage\" \"die\" \"die\"})");
}
static void hdc_parse_hook(uint8_t *a, void *reader)
{
    g_hdcParseOrig(a, reader);                                  /* value (+ optional {effects ...} block) */
    uint32_t mode = 0;
    if (g_rdPeek(reader, 1, 0)) {                               /* a bare word after the value: ours */
        MsString *w = g_rdWord(reader, 1);
        if (ieq(w, "max_health")) mode = HDC_MAXH;
        else if (ieq(w, "set_health")) mode = HDC_SETH;
        else { static int n; if (n++ < 10) logf_("health_damage_count: unknown word \"%.*s\" (expected max_health or set_health)", w ? (int)(w->size < 40 ? w->size : 40) : 0, w ? ms_cstr(w) : ""); }
        if (mode && g_rdPeek(reader, 9, 0) && g_rdFloat(reader) != 0.0f) mode |= HDC_CAP;
        if (mode) g_hdcCommon(a, reader);                       /* an {effects ...} block may still follow */
    }
    *(uint32_t *)(a + g_hdcModeOff) = mode ? (HDC_MAGIC | mode) : 0;
    hdc_check_effects(a);
}
/* factor (slot 12) is called right before GetAmount (slot 10) in the same execute: remember the target's vitality.
   Thread-local: one parsed action object is shared by every unit running that script, and actions can execute on
   several engine threads at once - a process-wide slot let two executions swap targets (0.5.0). */
static _Thread_local uint8_t *t_hdcLastObj;
static _Thread_local void *t_hdcLastVit;
static float hdc_factor_hook(uint8_t *a, void *vit, void *ctx, void *out)
{
    if (hdc_mode(a)) { t_hdcLastObj = a; t_hdcLastVit = vit; return 1.0f; }   /* exact % of max: no damage-type scaling */
    return g_hdcFactorOrig(a, vit, ctx, out);
}
static float hdc_amount_hook(uint8_t *a, void *ctx, void *out)
{
    float v = g_hdcAmountOrig(a, ctx, out);
    uint32_t m = hdc_mode(a);
    if (!m) return v;
    void *vit = (t_hdcLastObj == a) ? t_hdcLastVit : NULL;
    t_hdcLastObj = NULL; t_hdcLastVit = NULL;
    if (!vit) {   /* no factor call seen for this action on this thread: do NOT guess the target from ctx */
        static volatile LONG n;
        if (InterlockedIncrement(&n) <= 10) logf_("health_damage_count: no target vitality for this execute - action skipped");
        return 0.0f;   /* no change rather than a guessed max_health/set_health on the wrong unit */
    }
    float cur = g_getHealth(vit), max = g_getMaxHealth(vit);
    if (!(max > 0.0f)) return 0.0f;
    if (m & HDC_SETH) return cur - v * max;                     /* engine: new = cur - amount = v * max */
    float amt = v * max;
    if ((m & HDC_CAP) && amt < 0.0f) {                          /* capped heal: never above max */
        float room = max - cur; if (room < 0.0f) room = 0.0f;
        if (-amt > room) amt = -room;
    }
    return amt;
}
static int install_hdc(void)
{
    DWORD old;
    if (!VirtualProtect(g_vtHdc + 5, 8 * 8, PAGE_READWRITE, &old)) { logf_("health_damage_count: VirtualProtect failed"); return 0; }
    g_hdcParseOrig = (HdcParseFn)g_vtHdc[5];   g_vtHdc[5]  = (void *)hdc_parse_hook;
    g_hdcAmountOrig = (HdcAmountFn)g_vtHdc[10]; g_vtHdc[10] = (void *)hdc_amount_hook;
    g_hdcFactorOrig = (HdcFactorFn)g_vtHdc[12]; g_vtHdc[12] = (void *)hdc_factor_hook;
    VirtualProtect(g_vtHdc + 5, 8 * 8, old, &old);
    return 1;
}
#endif /* SX_TEST */
