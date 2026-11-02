/* GEM Fauna - over-head marker icon from a def prop
 * Part of the unity build: #included by gem_fauna.c, not compiled on its own (after ui.c).
 *
 * The marker over a unit's head (the shield with the faction badge) is an image named by the engine's marker-name
 * builder (found through its "spectator_enemy" / "icon_vehicle" strings; 3 callers, all marker views):
 *     out = "/interface/scene/squad/" + BASE + "(" + faction tag name + ")"
 * BASE for a non-human: icon_tank / icon_cannon / icon_airborne / icon_helicopter from def props, else icon_vehicle for a
 * vehicle-class unit, else icon_stub; the image set adds the _00/_01/_02 states. A spectator gets fixed names.
 * For a fauna whose def has a prop starting with "icon_", that prop replaces BASE and the faction part is dropped
 * (fauna have no nationality):
 *     {props "icon_carnifex"}  ->  /interface/scene/squad/icon_carnifex  (files icon_carnifex_00/_01/_02)
 * UI only (game and editor).
 */

typedef void *(*MarkerNameFn)(void *outStr, void *entity, uint8_t spectator);
static uint8_t *g_markerName; static MarkerNameFn g_markerNameOrig;
static int g_okHeadIcon, g_wantHeadIcon = 1;
#define HEAD_ICON_PREFIX "icon_"

static int find_head_icon(void)
{
    uint8_t *spec = find_cstr("spectator_enemy"), *veh = find_cstr("icon_vehicle");
    if (!spec || !veh) { logf_("head icon: marker strings not found"); return 0; }
    for (int k = 0; k < 8 && !g_markerName; k++) {                    /* the function that uses both strings */
        uint8_t *r = find_lea_to(spec, k); uint32_t f = r ? func_of(r) : 0;
        if (!r) break;
        if (!f) continue;
        for (int j = 0; j < 8; j++) {
            uint8_t *v = find_lea_to(veh, j);
            if (!v) break;
            if (func_of(v) == f) { g_markerName = g_base + f; break; }
        }
    }
    if (!g_markerName) { logf_("head icon: marker name builder not recognised"); return 0; }
    if (!prologue_len(g_markerName)) { logf_("head icon: marker name builder prologue not relocatable"); g_markerName = NULL; return 0; }
    logf_("head icon: marker name builder exe+0x%llX", EXE_OFF(g_markerName));
    return 1;
}

#ifndef FX_TEST
/* first def prop starting with "icon_" (props are stored lower-case) */
static const MsString *head_icon_prop(const uint8_t *e)
{
    const uint8_t *b, *en;
    if (!def_props(e, &b, &en)) return NULL;
    for (; b + TAG_SIZE <= en; b += TAG_SIZE) {
        const MsString *nm = (const MsString *)(b + TAG_NAME);
        if (nm->size > sizeof HEAD_ICON_PREFIX - 1 && nm->size < 64 && !memcmp(ms_cstr(nm), HEAD_ICON_PREFIX, sizeof HEAD_ICON_PREFIX - 1)) return nm;
    }
    return NULL;
}
static void *fx_marker_name(void *out, void *entity, uint8_t spectator)
{
    void *r = g_markerNameOrig(out, entity, spectator);
    uint8_t *e = entity;
    if (!e || (e[ENT_FLAGS] & 4) || !is_fauna(e)) return r;           /* humans: their own path; other vehicles untouched */
    const MsString *prop = head_icon_prop(e);
    if (!prop) return r;
    const MsString *s = out;
    const char *p = ms_cstr(s); size_t n = s->size;
    if (n >= 256) return r;
    const char *slash = NULL;
    for (size_t i = 0; i < n; i++) if (p[i] == '/') slash = p + i;
    if (!slash) return r;
    const char *base = slash + 1, *paren = memchr(base, '(', n - (size_t)(base - p));
    size_t baseLen = paren ? (size_t)(paren - base) : n - (size_t)(base - p);
    if (baseLen < sizeof HEAD_ICON_PREFIX - 1 || memcmp(base, HEAD_ICON_PREFIX, sizeof HEAD_ICON_PREFIX - 1)) return r;   /* spectator etc. */
    char buf[384];
    int m = snprintf(buf, sizeof buf, "%.*s%.*s", (int)(base - p), p, (int)prop->size, ms_cstr(prop));   /* no faction part: fauna have none */
    if (m <= 0 || m >= (int)sizeof buf) return r;
    static int logged; if (logged < 6) { logged++; logf_("head icon: fauna %p marker \"%.*s\" -> \"%s\"", e, (int)n, p, buf); }
    g_strAssign(out, buf, (size_t)m);
    return r;
}
static int install_head_icon(void)
{
    void *orig = NULL;
    if (!install_jmp5(g_markerName, (void *)fx_marker_name, &orig, "head icon (marker name builder)")) return 0;
    g_markerNameOrig = (MarkerNameFn)orig;
    return 1;
}
#endif /* FX_TEST */
