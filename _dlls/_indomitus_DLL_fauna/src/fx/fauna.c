/* GEM Fauna - fauna identification
 * Part of the unity build: #included by gem_fauna.c, not compiled on its own.
 * A fauna unit = alive entity whose def carries the fauna prop (ini fauna_prop). Also the def-props walk and the
 * entity -> vitality (health) helper.
 */

#ifndef FX_TEST
static int g_okFauna;
static char g_faunaProp[64] = "fauna_main";
static __attribute__((aligned(16))) uint8_t g_tagFaunaBuf[0x80];   /* Tag built from g_faunaProp (only its id is compared) */
#define TAG_ID(t) (*(const uint64_t *)(t))

/* the def props of an entity ({props "a" "b" ...}: a vector of Tags); 0 if none or the vector looks wrong */
static int def_props(const uint8_t *e, const uint8_t **b, const uint8_t **en)
{
    if (!e || !g_propBegOff) return 0;
    const uint8_t *def = *(const uint8_t *const *)(e + g_entDefOff);
    if (!def) return 0;
    *b = *(const uint8_t *const *)(def + g_propBegOff); *en = *(const uint8_t *const *)(def + g_propEndOff);
    return *b && *en >= *b && *en - *b <= TAG_SIZE * 512;
}

static int def_has_prop(const uint8_t *e, uint64_t id)
{
    const uint8_t *b, *en;
    if (!def_props(e, &b, &en)) return 0;
    for (; b + TAG_SIZE <= en; b += TAG_SIZE) if (TAG_ID(b) == id) return 1;
    return 0;
}
/* is_fauna is on hot paths (every chassis speed call, panel refreshes): the answer per def is cached - a def's props
   never change after load. Small direct-mapped cache; a miss walks the props once. */
#define FAUNA_CACHE_N 64
static struct { const void *def; int yes; } g_faunaCache[FAUNA_CACHE_N];
static int is_fauna(const uint8_t *e)
{
    if (!g_okFauna || !e || !(e[ENT_FLAGS] & 2) || (e[ENT_FLAGS] & 4)) return 0;      /* humans are never fauna */
    const void *def = *(const void *const *)(e + g_entDefOff);
    if (!def) return 0;
    unsigned h = (unsigned)(((uintptr_t)def >> 4) % FAUNA_CACHE_N);
    if (g_faunaCache[h].def == def) return g_faunaCache[h].yes;
    int yes = def_has_prop(e, TAG_ID(g_tagFaunaBuf));
    g_faunaCache[h].def = def; g_faunaCache[h].yes = yes;
    return yes;
}

/* the entity's Actor::eVitalityExtender, NULL if missing or (with the RTTI vtable known) of another class */
static void *entity_vitality(const uint8_t *e)
{
    if (!g_okVit) return NULL;
    const uint8_t *actor = *(const uint8_t *const *)(e + g_actorOff);
    if (!actor) return NULL;
    void *vit = *(void *const *)(actor + g_vitOff);
    if (!vit) return NULL;
    if (g_vtVitality && *(void **)vit != (void *)g_vtVitality) return NULL;
    return vit;
}
#endif /* FX_TEST */
