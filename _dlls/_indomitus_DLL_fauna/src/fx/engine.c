/* GEM Fauna - engine bindings and shared finders
 * Part of the unity build: #included by gem_fauna.c, not compiled on its own.
 * Engine types/functions the fauna features share: Tag (its ctor, via the engine's static Tag "bandage"),
 * condition tables ({if prop}, talk condition health_level), entity props (def tags) and the vitality
 * (health) extender. Copied from GEM ScriptExt 0.7.0 engine.c, without the script/event finders.
 */

/* ------------------------------------------------------------------------------------ */
/* engine bindings                                                                      */
/* ------------------------------------------------------------------------------------ */
typedef void  (*TagCtorCStrFn)(void *tag, const char *s);          /* Tag::Tag(const char*)           */
static TagCtorCStrFn g_tagCtorCStr;
static uint8_t      *g_tagBandage;                                   /* the engine's static Tag "bandage" (item class tag) */
static uint32_t      g_entDefOff;                                   /* entity->def (from {if prop} eval) */

#define ENT_FLAGS 0x30           /* bit1 = alive / in scene, bit2 = human, bit3 = vehicle class */
/* MSVC std::string (SSO) */
typedef struct { union { char buf[16]; char *ptr; } u; size_t size, cap; } MsString;
static const char *ms_cstr(const MsString *s) { return s->cap > 15 ? s->u.ptr : s->u.buf; }

/* Tag: 0x60 bytes, id at +0, name (std::string) at +0x40. GEM Fauna builds one (the fauna prop) and never frees it. */
#define TAG_SIZE 0x60
#define TAG_NAME 0x40

/* entity def props: {props "x"} are Tags in a vector at def+beg..def+end */
static uint32_t g_propBegOff, g_propEndOff;

/* actor block [entity+g_actorOff]: +g_vitOff vitality (Actor::eVitalityExtender) */
static uint32_t g_actorOff, g_vitOff; static int g_okVit;
typedef float (*VitGetFn)(void *vit);                                /* GetHealth / GetMaxHealth (display units) */
static VitGetFn g_getHealth, g_getMaxHealth;
static void **g_vtVitality;                                          /* RTTI: Actor::eVitalityExtender */
static uint32_t g_stamMaxOff, g_stamCurOff;                          /* vitality: stamina max / current (vitality {Stamina}) */

/* ------------------------------------------------------------------------------------ */
/* finders                                                                              */
/* ------------------------------------------------------------------------------------ */

/* {name, ctor} 16-byte entry in .data (condition tables) */
static uint8_t *find_table_ctor(const char *name)
{
    size_t m = strlen(name) + 1; uint8_t *fn = NULL; int hits = 0;
    for (size_t i = 0; i + 16 <= g_data.n; i += 8) {
        uint8_t *str = *(uint8_t **)(g_data.p + i), *f = *(uint8_t **)(g_data.p + i + 8);
        if (!in_rdata(str) || !in_text(f) || memcmp(str, name, m) || str[-1] != 0) continue;
        fn = f; hits++;
    }
    return hits == 1 ? fn : NULL;
}

/* condition ctor -> vtable (first lea r64,[rip+x] into .rdata whose 3 slots are code) */
static void **cond_vtable_of(uint8_t *ctor)
{
    for (uint8_t *p = ctor; p < ctor + 0x80; p++)
        if ((p[0] & 0xF8) == 0x48 && p[1] == 0x8D && (p[2] & 0xC7) == 0x05) {
            uint8_t *t = p + 7 + *(int32_t *)(p + 3);
            if (in_rdata(t) && in_text(*(uint8_t **)t) && in_text(*(uint8_t **)(t + 8)) && in_text(*(uint8_t **)(t + 16))) return (void **)t;
        }
    return NULL;
}

/* "name" string -> static Tag initialiser: lea rdx,[str] ; lea rcx,[tag] ; call Tag::Tag. Returns the Tag, *ctorOut = Tag::Tag. */
static uint8_t *find_static_tag(const char *name, uint8_t **ctorOut)
{
    uint8_t *s = find_cstr(name);
    if (!s) return NULL;
    for (int k = 0; k < 16; k++) {
        uint8_t *lea = find_lea_to(s, k);
        if (!lea) break;
        if (lea[2] != 0x15 || lea[7] != 0x48 || lea[8] != 0x8D || lea[9] != 0x0D || lea[14] != 0xE8) continue;
        uint8_t *tag = lea + 14 + *(int32_t *)(lea + 10), *ctor = lea + 19 + *(int32_t *)(lea + 15);
        if (!in_data(tag) || !in_text(ctor)) continue;
        *ctorOut = ctor;
        return tag;
    }
    return NULL;
}

static int find_tag_ctor(void)
{
    uint8_t *ctor = NULL, *tag = find_static_tag("bandage", &ctor);
    if (!tag) { logf_("fauna: static Tag 'bandage' not found"); return 0; }
    g_tagCtorCStr = (TagCtorCStrFn)ctor; g_tagBandage = tag;
    logf_("fauna: Tag(cstr)=exe+0x%llX, from the engine's Tag 'bandage' @ exe+0x%llX", EXE_OFF(ctor), EXE_OFF(tag));
    return 1;
}

/* {if prop "x"} eval: mov rax,[rdx] ; mov rbx,[rax+DEF] ; mov rdi,[rbx+END] ; mov rbx,[rbx+BEG] ; loop Tag==  */
static int find_prop_cond(void)
{
    uint8_t *ctor = find_table_ctor("prop");
    void **vt = ctor ? cond_vtable_of(ctor) : NULL;
    if (!vt) { logf_("fauna: {if prop} condition not found"); return 0; }
    uint8_t *ev = (uint8_t *)vt[1]; uint32_t defOff = 0, o1 = 0, o2 = 0; int reg = -1;
    for (uint8_t *p = ev; p < ev + 0x80; p++) {
        if (p[0] == 0x48 && p[1] == 0x8B && (p[2] & 0xC7) == 0x80 && !defOff) { defOff = *(uint32_t *)(p + 3); reg = (p[2] >> 3) & 7; continue; }
        if (defOff && p[0] == 0x48 && p[1] == 0x8B && (p[2] & 0xC0) == 0x80 && (p[2] & 7) == reg) {
            uint32_t o = *(uint32_t *)(p + 3);
            if (!o1) o1 = o; else if (!o2) { o2 = o; break; }
        }
    }
    if (defOff < 0x100 || !o1 || !o2 || (o1 != o2 + 8 && o2 != o1 + 8)) { logf_("fauna: {if prop} eval layout not recognised"); return 0; }
    g_entDefOff = defOff; g_propBegOff = o1 < o2 ? o1 : o2; g_propEndOff = g_propBegOff + 8;
    logf_("fauna: props = Tags at def+0x%X..0x%X (entity->def +0x%X), from {if prop} eval exe+0x%llX", g_propBegOff, g_propEndOff, g_entDefOff, EXE_OFF(ev));
    return 1;
}

/* talk condition {health_level}: eval reads vit = [[entity+ACTOR]+VIT] ; call GetHealth ; call GetMax ; divss xmm6,xmm0 */
static int find_vitality(void)
{
    uint8_t *ctor = find_table_ctor("health_level");
    void **vt = ctor ? cond_vtable_of(ctor) : NULL;
    if (!vt) { logf_("fauna: talk condition health_level not found"); return 0; }
    uint8_t *ev = (uint8_t *)vt[1];
    for (uint8_t *p = ev; p < ev + 0x40; p++)
        if (p[0] == 0x48 && p[1] == 0x8B && (p[2] & 0xC7) == 0x82 && p[7] == 0x48 && p[8] == 0x8B && (p[9] & 0xC7) == 0x40) {
            g_actorOff = *(uint32_t *)(p + 3); g_vitOff = p[10]; break;
        }
    static const uint8_t divsig[] = { 0xF3,0x0F,0x5E,0xF0 };
    uint8_t *dv = find_bytes(ev, 0x200, divsig, 4);
    if (!g_actorOff || !dv) { logf_("fauna: health_level eval layout not recognised"); return 0; }
    uint8_t *c1 = NULL, *c2 = NULL;
    for (uint8_t *p = dv - 5; p > ev; p--)
        if (p[0] == 0xE8 && in_text(p + 5 + *(int32_t *)(p + 1))) { if (!c2) c2 = p; else { c1 = p; break; } }
    if (!c1) { logf_("fauna: health getters not found"); return 0; }
    g_getHealth = (VitGetFn)(c1 + 5 + *(int32_t *)(c1 + 1)); g_getMaxHealth = (VitGetFn)(c2 + 5 + *(int32_t *)(c2 + 1));
    const uint8_t *gh = (const uint8_t *)g_getHealth;
    if (gh[0] != 0xF3 || gh[1] != 0x0F || gh[2] != 0x10 || (gh[3] & 0xC7) != 0x81) { logf_("fauna: GetHealth prologue not recognised"); return 0; }
    g_vtVitality = find_vtable_rtti(".?AVeVitalityExtender@Actor@@");
    logf_("fauna: vitality = [[entity+0x%X]+0x%X], GetHealth exe+0x%llX GetMax exe+0x%llX, vtable %s",
          g_actorOff, g_vitOff, EXE_OFF(g_getHealth), EXE_OFF(g_getMaxHealth), g_vtVitality ? "found (RTTI)" : "NOT found - type check off");
    return 1;
}

/* stamina fields of the vitality extender, from its stamina update (the caller of eChassisFoot::SetStaminaMovementModeLimit
   that is not the network command): lea r12,[rdi+MAX] ... addss xmm6,[rdi+MAX+4] (clamp of max / current). Optional. */
/* ---- vitality on any chassis ----
   The vitality extender's timer (health/stamina regen, host side) ends every tick with
       dynamic_cast<eChassisFoot&>(entity->chassis)   -> throws std::bad_cast on any other chassis
       eChassisFoot::SetStaminaMovementModeLimit(limit)
   so a unit with {extender "vitality" {Health}} and a wheel/track/human_carrier chassis is a fatal error as soon as it
   ticks (no {Stamina} block needed: the empty-limits case still gets there). Patch: the cast's throw flag
   (mov dword [rsp+0x20],1 -> 0, the __RTDynamicCast isReference argument) and the SetStaminaMovementModeLimit call
   redirected to a wrapper that skips a NULL chassis. The client-side path calls a base-class virtual and is fine. */
static uint8_t *g_vitCastFlag, *g_vitSetLimitSite; static void *g_vitSetLimitFn;
static int find_vitality_chassis(void)
{
    static const char lbl[] = "eChassisFoot::SetStaminaMovementModeLimit";
    uint8_t *s = find_bytes(g_rdata.p, g_rdata.n, (const uint8_t *)lbl, sizeof lbl - 1), *lea = NULL;
    for (uint8_t *p = g_text.p; s && p + 8 <= g_text.p + g_text.n && !lea; p++)
        if (p[0] == 0x0F && p[1] == 0x10 && (p[2] & 0xC7) == 0x05 && p + 7 + *(int32_t *)(p + 3) == s) lea = p;
    uint32_t sf = lea ? func_of(lea) : 0;
    if (!sf) { logf_("vitality chassis: SetStaminaMovementModeLimit not found"); return 0; }
    uint8_t *setLimit = g_base + sf; int n = 0;
    for (uint8_t *p = g_text.p; p + 5 <= g_text.p + g_text.n; p++) {
        if (p[0] != 0xE8 || p + 5 + *(int32_t *)(p + 1) != setLimit) continue;
        uint32_t f = func_of(p);
        if (!f || p - (g_base + f) < 0x200) continue;                /* not the network command's read handler */
        /* ... mov dword [rsp+0x20],1 ... call __RTDynamicCast ; mov rcx,rax ; xor r8d,r8d ; mov edx,ebx ; call SetLimit */
        if (memcmp(p - 8, "\x48\x8B\xC8\x45\x33\xC0\x8B\xD3", 8) || p[-13] != 0xE8) continue;
        uint8_t *flag = NULL;
        for (uint8_t *q = p - 0x50; q < p - 13; q++) if (!memcmp(q, "\xC7\x44\x24\x20\x01\x00\x00\x00", 8)) flag = q;
        if (!flag) continue;
        n++; g_vitCastFlag = flag + 4; g_vitSetLimitSite = p; g_vitSetLimitFn = setLimit;
    }
    if (n != 1) { logf_("vitality chassis: timer site not recognised (%d matches)", n); return 0; }
    logf_("vitality chassis: vitality timer cast flag @ exe+0x%llX, SetStaminaMovementModeLimit call @ exe+0x%llX", EXE_OFF(g_vitCastFlag), EXE_OFF(g_vitSetLimitSite));
    return 1;
}
#ifndef FX_TEST
static int patch_code(uint8_t *dst, const void *src, size_t len, const char *what);          /* patch.c (included later) */
static int redirect_call(uint8_t *e8, void *fn, void **orig, const char *what);
typedef void (*SetLimitFn)(void *chassis, int limit, uint8_t force);
static void fx_set_stamina_limit(void *chassis, int limit, uint8_t force)
{
    if (chassis) ((SetLimitFn)g_vitSetLimitFn)(chassis, limit, force);   /* not a foot chassis: nothing to limit */
}
static int install_vitality_chassis(void)
{
    uint8_t zero = 0;
    return patch_code(g_vitCastFlag, &zero, 1, "vitality chassis (cast flag)")
        && redirect_call(g_vitSetLimitSite, (void *)fx_set_stamina_limit, NULL, "vitality chassis (limit call)");
}
#endif

static int find_stamina_fields(void)
{
    static const char lbl[] = "eChassisFoot::SetStaminaMovementModeLimit";
    uint8_t *s = find_bytes(g_rdata.p, g_rdata.n, (const uint8_t *)lbl, sizeof lbl - 1);
    uint8_t *lea = NULL;                                               /* movups xmm,[rip+label] (the log text is copied) */
    for (uint8_t *p = g_text.p; s && p + 8 <= g_text.p + g_text.n && !lea; p++)
        if (p[0] == 0x0F && p[1] == 0x10 && (p[2] & 0xC7) == 0x05 && p + 7 + *(int32_t *)(p + 3) == s) lea = p;
    uint32_t sf = lea ? func_of(lea) : 0;
    if (!sf) { logf_("stamina: SetStaminaMovementModeLimit not found"); return 0; }
    uint8_t *setLimit = g_base + sf;
    for (uint8_t *p = g_text.p; p + 5 <= g_text.p + g_text.n && !g_stamMaxOff; p++) {
        if (p[0] != 0xE8 || p + 5 + *(int32_t *)(p + 1) != setLimit) continue;
        uint32_t f = func_of(p);
        if (!f || p - (g_base + f) < 0x200) continue;                /* the command's read handler is short */
        uint32_t maxo = 0, curo = 0;
        for (uint8_t *q = g_base + f; q + 8 < p; q++) {
            if (q[0] == 0x4C && q[1] == 0x8D && q[2] == 0xA7) maxo = *(uint32_t *)(q + 3);               /* lea r12,[rdi+X] */
            if (q[0] == 0xF3 && q[1] == 0x0F && q[2] == 0x58 && (q[3] & 0xC7) == 0x87) curo = *(uint32_t *)(q + 4);   /* addss xmm,[rdi+X] */
            if (maxo && curo == maxo + 4) { g_stamMaxOff = maxo; g_stamCurOff = curo; break; }
        }
    }
    if (!g_stamMaxOff) { logf_("stamina: vitality stamina fields not recognised (no stamina bar)"); return 0; }
    logf_("stamina: vitality stamina max +0x%X, current +0x%X (SetStaminaMovementModeLimit exe+0x%llX)", g_stamMaxOff, g_stamCurOff, EXE_OFF(setLimit));
    return 1;
}
