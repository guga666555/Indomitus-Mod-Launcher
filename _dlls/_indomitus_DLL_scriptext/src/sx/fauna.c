/* GEM ScriptExt - fauna identification
 * Part of the unity build: #included by gem_scriptext.c, not compiled on its own.
 * A fauna unit = vehicle-class entity whose def carries the fauna prop (ini fauna_prop).
 * Also the entity -> vitality (health) helpers.
 */

#ifndef SX_TEST
/* ------------------------------------------------------------------------------------ */
/* identification + health read                                                         */
/* ------------------------------------------------------------------------------------ */
static int g_okFauna;
static char g_faunaProp[64] = "fauna_main";
static __attribute__((aligned(16))) uint8_t g_tagFaunaBuf[0x80];
#define TAG_ID(t) (*(const uint64_t *)(t))

static int def_has_prop(const uint8_t *e, uint64_t id)
{
    if (!e || !g_propBegOff) return 0;
    const uint8_t *def = *(const uint8_t *const *)(e + g_entDefOff);
    if (!def) return 0;
    const uint8_t *b = *(const uint8_t *const *)(def + g_propBegOff), *en = *(const uint8_t *const *)(def + g_propEndOff);
    if (!b || en < b || en - b > 0x60 * 512) return 0;
    for (; b + 0x60 <= en; b += 0x60) if (TAG_ID(b) == id) return 1;
    return 0;
}
static int is_fauna(const uint8_t *e) { return g_okFauna && e && (e[ENT_FLAGS] & 2) && def_has_prop(e, TAG_ID(g_tagFaunaBuf)); }

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

static void *raw_vitality(const uint8_t *e)
{
    if (!g_okVit || !e) return NULL;
    const uint8_t *actor = *(const uint8_t *const *)(e + g_actorOff);
    return actor ? *(void *const *)(actor + g_vitOff) : NULL;
}

#endif /* SX_TEST */
