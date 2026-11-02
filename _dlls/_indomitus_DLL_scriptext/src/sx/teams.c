/* GEM ScriptExt - {if player N} and {if team N}
 * Part of the unity build: #included by gem_scriptext.c, not compiled on its own.
 * The unit's owning player (entity byte found by find_diplomacy), its relation (ally / neutral / enemy) to the
 * other unit of the script context (or the local player), and its team: the multiplayer session team ("a" = 1,
 * "b" = 2, ...) in a network or bot battle, alliances (diplomacy matrix) everywhere else.
 */

#ifndef SX_TEST
static int g_okMpTeams;                                         /* find_mp_teams succeeded */

/* condition objects: {vt, 8 payload bytes}.
   player: int32 number (-1 = never true) at +8, int32 mode at +12 (PL_NUMBER, or ally / neutral / enemy).
   team: lower-case name, NUL-padded (max 7 characters; a number N is stored as the N-th letter) */
enum { PL_NUMBER, PL_ALLY, PL_NEUTRAL, PL_ENEMY };
static const char *const g_plModeName[] = { "", "ally", "neutral", "enemy" };

static void parse_player(void *c, void *reader)
{
    int32_t n = -1, mode = PL_NUMBER;
    if (!g_rdPeek(reader, 9, 0) && g_rdPeek(reader, 1, 0)) {        /* {if player ally|neutral|enemy} */
        MsString *w = g_rdWord(reader, 1);
        const char *s = w ? ms_cstr(w) : "";
        size_t L = w ? w->size : 0;
        for (int m = PL_ALLY; m <= PL_ENEMY && mode == PL_NUMBER; m++)
            if (L == strlen(g_plModeName[m]) && !_strnicmp(s, g_plModeName[m], L)) mode = m;
        if (mode == PL_NUMBER) { static int k; if (k++ < 10) logf_("{if player %.*s}: expected a player number, ally, neutral or enemy - never true", (int)(L < 40 ? L : 40), s); }
    } else {                                                        /* {if player 4} */
        float v = g_rdFloat(reader);
        n = (v >= 0.0f && v < (float)g_maxPlayers && v == (float)(int32_t)v) ? (int32_t)v : -1;
        if (n < 0) { static int k; if (k++ < 10) logf_("{if player %g}: not a player number 0..%d - never true", v, g_maxPlayers - 1); }
    }
    *(int32_t *)((uint8_t *)c + 8) = n;
    *(int32_t *)((uint8_t *)c + 12) = mode;
}

/* relation of player ref to the unit, as the engine computes it (Entity::GetRelation(ref, unit) / the local-player
   variant {if fogged} uses): 0 neutral when the unit is not operatable, 2 when it is ref's own unit, else the
   diplomacy matrix (2 ally, 1 enemy, 0 neutral) */
static int player_relation(int ref, const uint8_t *unit)
{
    if (!(*(const uint32_t *)(unit + g_operOff) & g_operBit)) return 0;
    int u = unit[g_playerOff];
    if (u == ref) return 2;
    if (ref >= g_maxPlayers || u >= g_maxPlayers) return 0;
    const int32_t *d = *(const int32_t *const volatile *)g_dipData;
    return d ? d[u * *g_dipStride + ref] : 0;
}

/* who ally / neutral / enemy is measured against: the other unit of the context (the effector; inside {with_effector}
   that is the unit the script belongs to, since the engine swaps the two), else the local player. -1 = none. */
static int relation_ref(void **ctx, int *fromEffector)
{
    *fromEffector = 0;
    if (ctx && g_ctxEffBit && (*(const uint32_t *)((const uint8_t *)ctx + g_ctxFlagsOff) & g_ctxEffBit) && ctx[1]) {
        *fromEffector = 1;
        return ((const uint8_t *)ctx[1])[g_playerOff];
    }
    return g_localPlayer ? *(volatile uint8_t *)g_localPlayer : -1;
}

static int eval_player(void *c, uint8_t *e, void **ctx)
{
    if (!c || !e) return 0;
    int32_t n = *(int32_t *)((uint8_t *)c + 8), mode = *(int32_t *)((uint8_t *)c + 12);
    if (mode == PL_NUMBER) return n >= 0 && e[g_playerOff] == (uint8_t)n;
    if (mode < PL_ALLY || mode > PL_ENEMY) return 0;
    int eff, ref = relation_ref(ctx, &eff);
    if (ref < 0) return 0;
    int rel = player_relation(ref, e);
    if (g_verbose) { static volatile LONG k; if (InterlockedIncrement(&k) <= 12)
        logf_("{if player %s}: unit of player %d vs %s player %d -> %s", g_plModeName[mode], e[g_playerOff],
              eff ? "the effector's" : "the local", ref, rel == 2 ? "ally" : rel == 1 ? "enemy" : "neutral"); }
    return mode == PL_ALLY ? rel == 2 : mode == PL_ENEMY ? rel == 1 : rel != 1 && rel != 2;
}

static void parse_team(void *c, void *reader)
{
    char *t = (char *)c + 8;
    memset(t, 0, 8);
    if (g_rdPeek(reader, 9, 0)) {                                   /* {if team 2} */
        float v = g_rdFloat(reader);
        if (v >= 1.0f && v <= 26.0f && v == (float)(int32_t)v) t[0] = (char)('a' + (int32_t)v - 1);
        else { static int k; if (k++ < 10) logf_("{if team %g}: teams are numbered 1..26 - never true", v); }
    } else if (g_rdPeek(reader, 1, 0)) {                            /* {if team b} / {if team "b"} */
        MsString *w = g_rdWord(reader, 1);
        const char *s = w ? ms_cstr(w) : "";
        if (w && w->size >= 1 && w->size <= 7)
            for (size_t i = 0; i < w->size; i++) { char ch = s[i]; t[i] = (ch >= 'A' && ch <= 'Z') ? (char)(ch + 32) : ch; }
        else { static int k; if (k++ < 10) logf_("{if team \"%.*s\"}: team names are 1..7 characters - never true", w ? (int)(w->size < 40 ? w->size : 40) : 0, s); }
    } else (void)g_rdFloat(reader);                                 /* nothing usable: the engine reports the missing value */
}

/* ---- multiplayer session team ----
   The session belongs to the network layer, so it is read with rd_mem (no fault if it goes away). Its player list
   only changes when players join or leave, so each player's team is read once per (session, list) and cached. */
static CRITICAL_SECTION g_teamLock;                             /* initialised by init before g_okMpTeams is set */
static struct { uint8_t *game, *beg, *end; uint8_t state[32]; char name[32][8]; } g_teamCache;   /* state: 0 unread, 1 team, 2 none */

static int mp_scan(int p, uint8_t *beg, uint8_t *end, char name[8])
{
    size_t n = (size_t)(end - beg) / 8;
    uint8_t *pl[64];
    if (!beg || end < beg || n > 64 || (n && !rd_mem(beg, pl, n * 8))) return 2;
    for (size_t i = 0; i < n; i++) {
        uint8_t id; MsString s; char tmp[8];
        if (!pl[i] || !rd_mem(pl[i] + g_mpIdOff, &id, 1) || id != p) continue;
        if (!rd_mem(pl[i] + g_mpTeamOff, &s, sizeof s) || s.size == 0 || s.size > 64) return 2;
        if (s.size > 7) return 1;                                   /* a team, but too long to be named in a script */
        if (s.cap > 15) { if (!rd_mem(s.u.ptr, tmp, s.size)) return 2; } else memcpy(tmp, s.u.buf, s.size);
        for (size_t k = 0; k < s.size; k++) { char ch = tmp[k]; name[k] = (ch >= 'A' && ch <= 'Z') ? (char)(ch + 32) : ch; }
        return 1;
    }
    return 2;
}
/* 1 = player p is in the running multiplayer session with team name out (may be ""), -1 = a session runs but p is not
   in it (map-owned units), 0 = no session (single player, editor) */
static int mp_team_of(int p, char out[8])
{
    memset(out, 0, 8);
    if (!g_okMpTeams || p < 0 || p >= 32) return 0;
    uint8_t *G = *(uint8_t *volatile *)g_mpGame, *v[2];
    if (!G || !rd_mem(G + g_mpPlayersOff, v, sizeof v)) return 0;
    EnterCriticalSection(&g_teamLock);
    if (g_teamCache.game != G || g_teamCache.beg != v[0] || g_teamCache.end != v[1]) {
        memset(&g_teamCache, 0, sizeof g_teamCache);
        g_teamCache.game = G; g_teamCache.beg = v[0]; g_teamCache.end = v[1];
    }
    int fresh = !g_teamCache.state[p];
    if (fresh) g_teamCache.state[p] = (uint8_t)mp_scan(p, v[0], v[1], g_teamCache.name[p]);
    int st = g_teamCache.state[p];
    memcpy(out, g_teamCache.name[p], 8);
    LeaveCriticalSection(&g_teamLock);
    if (fresh) { static volatile LONG n; if (InterlockedIncrement(&n) <= 24)
        logf_(st == 1 ? "{if team}: player %d is in multiplayer team \"%s\"" : "{if team}: player %d is not in the multiplayer session - no team", p, out); }
    return st == 1 ? 1 : -1;
}

/* ---- alliances (no session): team 1 = player 0 and its allies, then the next player not yet in a team with its
   allies, and so on. A player's group leader = the lowest player allied with it both ways (itself if none);
   team = 1 + the number of distinct leaders below its own. */
static int alliance_team(int p)
{
    const int32_t *d = *(const int32_t *const volatile *)g_dipData;
    int st = *g_dipStride, n = st < g_maxPlayers ? st : g_maxPlayers;
    if (!d || n <= 0 || n > 32 || p < 0 || p >= n) return 0;
    int lead[32];
    for (int q = 0; q < n; q++) {
        lead[q] = q;
        for (int r = 0; r < q; r++) if (d[q * st + r] == 2 && d[r * st + q] == 2) { lead[q] = r; break; }
    }
    uint32_t below = 0;
    for (int q = 0; q < n; q++) if (lead[q] < lead[p]) below |= 1u << lead[q];
    return 1 + __builtin_popcount(below);
}

static int eval_team(void *c, uint8_t *e, void **ctx)
{
    (void)ctx;
    const char *want = c ? (const char *)c + 8 : "";
    if (!e || !want[0]) return 0;
    int p = e[g_playerOff];
    char nm[8];
    int st = mp_team_of(p, nm);
    if (st > 0) return !memcmp(nm, want, 8);
    if (st < 0 || want[1]) return 0;                                /* alliances have numbers only (a = 1, b = 2, ...) */
    int t = alliance_team(p);
    if (g_verbose) { static volatile LONG n; if (InterlockedIncrement(&n) <= 8) logf_("{if team}: no multiplayer session - player %d is in alliance team %d", p, t); }
    return t == want[0] - 'a' + 1;
}
#endif /* SX_TEST */
